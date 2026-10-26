#!/usr/bin/env python3
"""Prepare exact RC3 PanVK sources in task-owned caches; never access a device.

Use --fetch to acquire missing pinned inputs. Repeated preparation verifies the
recorded patched source, including copied UAPI files, rather than resetting it.
This only prepares the upstream Android baseline; Darwin changes are a separate
recorded patch series and neither preparation nor compilation qualifies a GPU.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd=None, capture=True):
    return subprocess.run([str(x) for x in args], cwd=cwd, check=True,
                          stdout=subprocess.PIPE if capture else None,
                          stderr=subprocess.PIPE if capture else None,
                          timeout=900).stdout


def sha(data):
    return hashlib.sha256(data).hexdigest()


def checkout(path, source, fetch):
    if not (path / '.git').exists():
        if not fetch:
            raise RuntimeError(f'Missing pinned input {path}; use --fetch')
        if path.exists():
            raise RuntimeError(f'Refusing non-repository destination {path}')
        path.mkdir(parents=True)
        run('git', 'init', '-q', path)
        run('git', 'remote', 'add', 'origin', source['repository'], cwd=path)
        if source.get('sparse'):
            run('git', 'sparse-checkout', 'init', '--cone', cwd=path)
            run('git', 'sparse-checkout', 'set', *source['sparse'], cwd=path)
        run('git', 'fetch', '--depth=1', '--filter=blob:none', 'origin', source['commit'], cwd=path, capture=False)
        run('git', 'checkout', '--detach', source['commit'], cwd=path, capture=False)
    if run('git', 'rev-parse', 'HEAD', cwd=path).decode().strip() != source['commit']:
        raise RuntimeError(f'{path} does not match its fixed source commit')


def snapshot(path):
    # The upstream series copies untracked files as well as applying patches.
    # Bind both to the manifest so an edited/unrecorded backend is never reused.
    extras = {}
    for name in run('git', 'ls-files', '--others', '--exclude-standard', '-z', cwd=path).split(b'\0'):
        if name:
            relative = name.decode()
            file = path / relative
            if file.is_symlink() or not file.is_file():
                raise RuntimeError(f'Unexpected non-file source addition: {relative}')
            extras[relative] = sha(file.read_bytes())
    return {'diff_sha256': sha(run('git', 'diff', 'HEAD', '--binary', cwd=path)), 'files': extras}


def prepare(root, fetch=False):
    pins = json.loads((root / 'scripts/data/mali-g925-inputs.json').read_text())
    cache = root / '.cache'
    reference = cache / 'panvk-kbase-android'
    source = cache / 'panvk-mesa'
    state = cache / 'panvk-prepared.json'
    checkout(reference, pins['panvk'], fetch)
    if run('git', 'status', '--porcelain', cwd=reference).strip():
        raise RuntimeError('PanVK reference checkout has local modifications')
    if pins['panvk'].get('sparse') and fetch:
        run('git', 'sparse-checkout', 'set', *pins['panvk']['sparse'], cwd=reference)
    upstream = json.loads((reference / 'sources.lock').read_text())
    if upstream['mesaCommit'] != pins['mesa']['commit']:
        raise RuntimeError('PanVK sources.lock and Mesa source pin differ')
    patches = []
    copied = []
    inputs = {}
    for family in pins['panvk']['families']:
        directory = reference / 'patches' / family
        if not directory.is_dir():
            raise RuntimeError(f'Missing pinned patch family {family}')
        for patch in sorted(directory.glob('*.patch')):
            patches.append(patch)
            inputs[str(patch.relative_to(reference))] = sha(patch.read_bytes())
        for file in sorted((directory / 'files').rglob('*')):
            if file.is_file() and file.name != 'README':
                if file.is_symlink():
                    raise RuntimeError(f'Unexpected reference symlink {file}')
                relative = file.relative_to(directory / 'files')
                copied.append((file, relative))
                inputs[str(file.relative_to(reference))] = sha(file.read_bytes())
    identity = {'pins': pins, 'inputs': inputs}
    if state.exists():
        recorded = json.loads(state.read_text())
        checkout(source, pins['mesa'], False)
        if recorded['identity'] != identity or recorded['source'] != snapshot(source):
            raise RuntimeError('Prepared PanVK source differs from its recorded inputs/source; preserve and inspect it')
        return recorded
    # Never resume a partially patched tree: it is useful failure evidence.
    if source.exists():
        raise RuntimeError(f'Unrecorded source exists at {source}; preserve and inspect it')
    checkout(source, pins['mesa'], fetch)
    for family in pins['panvk']['families']:
        directory = reference / 'patches' / family
        for patch in sorted(directory.glob('*.patch')):
            checked = subprocess.run(['git', 'apply', '--check', str(patch)], cwd=source,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            flags = [] if checked.returncode == 0 else ['--recount']
            run('git', 'apply', *flags, '--check', patch, cwd=source)
            run('git', 'apply', *flags, patch, cwd=source)
        for file, relative in copied:
            if file.is_relative_to(directory):
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(file, target)
    result = {'schema': 1, 'identity': identity, 'source': snapshot(source),
              'patch_count': len(patches), 'hardware_exercised': False}
    state.write_text(json.dumps(result, indent=2, sort_keys=True) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    args = parser.parse_args()
    try:
        result = prepare(ROOT, args.fetch)
    except (RuntimeError, OSError, subprocess.CalledProcessError) as error:
        detail = error.stderr.decode(errors='replace') if isinstance(error, subprocess.CalledProcessError) and error.stderr else str(error)
        sys.exit(detail)
    print(f"Verified fixed PanVK baseline: {result['patch_count']} patches; hardware_exercised=false")


if __name__ == '__main__':
    main()
