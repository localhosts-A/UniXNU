#!/usr/bin/env python3
"""Export reviewed Darwin edits relative to the verified RC3 patched baseline.

New sources live in patches/panvk-darwin/files; the ABI is copied from the
production driver. Neither source tree is reset or overwritten by this script.
"""
import difflib
import importlib.util
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('panvk_prepare', ROOT/'scripts/prepare-panvk-darwin.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
prepare.prepare(ROOT)
baseline, source = ROOT/'.cache/panvk-mesa', ROOT/'.cache/panvk-mesa-darwin'
tracked = subprocess.check_output(['git','ls-files','-z'],cwd=source).split(b'\0')
patches = []
for raw in tracked:
    if not raw:
        continue
    name = raw.decode()
    before, after = baseline/name, source/name
    if before.is_file() and after.is_file() and before.read_bytes() != after.read_bytes():
        patches.append('diff --git a/{0} b/{0}\n'.format(name))
        patches.extend(difflib.unified_diff(before.read_text().splitlines(keepends=True),
                       after.read_text().splitlines(keepends=True), fromfile='a/'+name,tofile='b/'+name))
    elif before.is_file() != after.is_file():
        raise SystemExit('Unexpected deletion/addition of tracked source: '+name)
overlay = ROOT/'patches/panvk-darwin/files'
expected = {str(p.relative_to(overlay)): p for p in overlay.rglob('*') if p.is_file()}
expected['src/panfrost/lib/kmod/NativeMaliABI.h'] = ROOT/'drivers/NativeMali/NativeMaliABI.h'
extras = subprocess.check_output(['git','ls-files','--others','--exclude-standard','-z'],cwd=source).split(b'\0')
for raw in extras:
    if not raw:
        continue
    name = raw.decode()
    if (baseline/name).is_file():
        if (baseline/name).read_bytes() != (source/name).read_bytes():
            raise SystemExit('Unexpected edit to baseline copied source: '+name)
    elif name not in expected or expected[name].read_bytes() != (source/name).read_bytes():
        raise SystemExit('Unrecorded or mismatched Darwin source: '+name)
for name,p in expected.items():
    if not (source/name).is_file() or p.read_bytes() != (source/name).read_bytes():
        raise SystemExit('Missing/mismatched overlay: '+name)
destination = ROOT/'patches/panvk-darwin/0001-native-mali-darwin.patch'
destination.write_text(''.join(patches))
print('Exported {} changed baseline files; source trees preserved'.format(sum(x.startswith('diff --git ') for x in patches)))
