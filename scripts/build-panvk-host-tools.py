#!/usr/bin/env python3
"""Build pinned native Mesa codegen tools used by the PanVK iOS cross build.

The host requires LLVM/Clang development libraries, SPIRV-Tools and the matching
LLVM-SPIRV translator. --dependency-prefix supports an isolated host dependency
prefix without installing anything into the operating system. No GPU is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependency-prefix', type=Path)
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    subprocess.run([sys.executable, ROOT/'scripts/prepare-panvk-darwin.py'], check=True)
    source = ROOT/'.cache/panvk-mesa'
    out = ROOT/'build/panvk-host-tools'
    out.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ)
    venv = ROOT/'.cache/mali-venv/bin'
    env['PATH'] = str(venv) + os.pathsep + env['PATH']
    meson = shutil.which('meson', path=env['PATH'])
    ninja = shutil.which('ninja', path=env['PATH'])
    if not meson or not ninja:
        raise SystemExit('Install scripts/data/mali-build-requirements.txt in .cache/mali-venv')
    env.update(CC=shutil.which('clang', path=env['PATH']) or 'clang',
               CXX=shutil.which('clang++', path=env['PATH']) or 'clang++',
               CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')
    if args.dependency_prefix:
        prefix = args.dependency_prefix.resolve()
        if not (prefix/'include').is_dir() or not (prefix/'lib').is_dir():
            parser.error('dependency prefix must contain include/ and lib/')
        # Keep each search-path option in one argument. Meson's compiler probe
        # normalization can reorder a detached -I/-L away from its operand.
        env['CPPFLAGS'] = shlex.join(['-I' + str(prefix/'include')])
        env['LDFLAGS'] = shlex.join(['-L' + str(prefix/'lib'), '-Wl,-rpath,' + str(prefix/'lib')])
        env['PKG_CONFIG_PATH'] = str(prefix/'lib/pkgconfig') + os.pathsep + env.get('PKG_CONFIG_PATH', '')
    options = ['-Dbuildtype=release', '-Dgallium-drivers=', '-Dvulkan-drivers=panfrost',
               '-Dpanfrost-kmds=kbase', '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled',
               '-Dgbm=disabled', '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled',
               '-Dmesa-clc=enabled', '-Dinstall-mesa-clc=true', '-Dprecomp-compiler=enabled',
               '-Dinstall-precomp-compiler=true', '-Dllvm=enabled', '-Dzstd=disabled',
               '-Dbuild-tests=false', '-Dvideo-codecs=', '-Dvulkan-layers=']
    profile = {'source': json.loads((ROOT/'.cache/panvk-prepared.json').read_text()), 'options': options,
               'compiler': subprocess.check_output([env['CXX'], '--version'], text=True),
               'environment': {key: env.get(key, '') for key in ('CC', 'CXX', 'CPPFLAGS', 'LDFLAGS', 'PKG_CONFIG_PATH')},
               'meson': subprocess.check_output([meson, '--version'], text=True).strip()}
    recorded = out/'build-profile.json'
    if recorded.exists() and json.loads(recorded.read_text()) != profile:
        raise SystemExit('Host tool build inputs changed; preserve the existing build and use a fresh output tree')
    def logged(command, name):
        log = out/(name + '.log')
        with log.open('w') as stream:
            result = subprocess.run([str(x) for x in command], env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            lines = log.read_text(errors='replace').splitlines()
            errors = [line for line in lines if 'error:' in line or line.startswith('FAILED:')]
            raise SystemExit('\n'.join(errors[:25] + lines[-30:]))
    if not (out/'build.ninja').exists():
        logged([meson, 'setup', out, source, '--wrap-mode=nodownload', *options], 'configure')
        recorded.write_text(json.dumps(profile, indent=2, sort_keys=True) + '\n')
    if args.configure_only:
        print('Configured exact-source native PanVK codegen build; GPU not accessed')
        return
    tools = {'mesa_clc': 'src/compiler/clc/mesa_clc', 'vtn_bindgen2': 'src/compiler/spirv/vtn_bindgen2',
             'panfrost_compile': 'src/panfrost/clc/panfrost_compile'}
    logged([ninja, '-C', out, '-j', args.jobs, *tools.values()], 'build')
    binaries = out/'bin'; binaries.mkdir(exist_ok=True)
    outputs = {}
    for name, relative in tools.items():
        executable = out/relative
        shutil.copyfile(executable, binaries/name)
        (binaries/name).chmod(0o755)
        outputs[name] = hashlib.sha256(executable.read_bytes()).hexdigest()
    (out/'report.json').write_text(json.dumps({'schema': 1, 'profile': profile, 'tools': outputs,
                                              'hardware_exercised': False}, indent=2, sort_keys=True) + '\n')
    print('Built exact-source mesa_clc, vtn_bindgen2 and panfrost_compile; hardware_exercised=false')


if __name__ == '__main__':
    main()
