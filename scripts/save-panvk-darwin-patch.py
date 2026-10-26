#!/usr/bin/env python3
"""Export reviewed Darwin edits relative to the verified RC3 patched baseline.

New sources live in patches/panvk-darwin/files; the ABI is copied from the
production driver. Neither source tree is reset or overwritten by this script.
"""
import difflib
import importlib.util
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('panvk_prepare', ROOT/'scripts/prepare-panvk-darwin.py')
prepare = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prepare)
baseline, source = ROOT/'.cache/panvk-mesa', ROOT/'.cache/panvk-mesa-darwin'
state = ROOT/'.cache/panvk-darwin-prepared.json'


def identity():
    pins = json.loads((ROOT/'scripts/data/mali-g925-inputs.json').read_text())
    reference = ROOT/'.cache/panvk-kbase-android'
    prepare.checkout(reference,pins['panvk'],False)
    prepare.checkout(source,pins['mesa'],False)
    if subprocess.check_output(['git','-C',reference,'status','--porcelain'],text=True).strip():
        raise SystemExit('Pinned PanVK reference checkout has local modifications')
    if json.loads((reference/'sources.lock').read_text())['mesaCommit'] != pins['mesa']['commit']:
        raise SystemExit('PanVK source lock differs from the pinned Mesa commit')
    inputs={}
    for family in pins['panvk']['families']:
        directory=reference/'patches'/family
        for patch in sorted(directory.glob('*.patch')):
            inputs[str(patch.relative_to(reference))]=hashlib.sha256(patch.read_bytes()).hexdigest()
        for file in sorted((directory/'files').rglob('*')):
            if file.is_file() and file.name!='README':
                inputs[str(file.relative_to(reference))]=hashlib.sha256(file.read_bytes()).hexdigest()
    native=ROOT/'patches/panvk-darwin'
    for patch in sorted(native.glob('*.patch')):
        inputs[str(patch.relative_to(ROOT))]=hashlib.sha256(patch.read_bytes()).hexdigest()
    for file in sorted((native/'files').rglob('*')):
        if file.is_file():inputs[str(file.relative_to(ROOT))]=hashlib.sha256(file.read_bytes()).hexdigest()
    abi=ROOT/'drivers/NativeMali/NativeMaliABI.h'
    inputs[str(abi.relative_to(ROOT))]=hashlib.sha256(abi.read_bytes()).hexdigest()
    return {'pins':pins,'inputs':inputs}


recorded=json.loads(state.read_text())
if recorded['identity']!=identity():
    raise SystemExit('Prepared source identity differs from current pinned inputs; preserve and inspect it')
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
recorded['identity']=identity()
recorded['source']=prepare.snapshot(source)
recorded['patch_count']=sum(1 for family in recorded['identity']['pins']['panvk']['families']
                           for _ in (ROOT/'.cache/panvk-kbase-android'/'patches'/family).glob('*.patch'))
recorded['patch_count']+=len(list((ROOT/'patches/panvk-darwin').glob('*.patch')))
state.write_text(json.dumps(recorded,indent=2,sort_keys=True)+'\n')
prepare.prepare(ROOT,False,True)
print('Exported {} changed baseline files; source trees preserved'.format(sum(x.startswith('diff --git ') for x in patches)))
