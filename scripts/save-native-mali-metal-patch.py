#!/usr/bin/env python3
"""Export reviewed changes from the prepared NativeMetal copy as a parent patch."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]
KIT=ROOT/'drivers/NativeMetal'
OUT=ROOT/'build/native-mali-metal/source'
STAMP=ROOT/'build/native-mali-metal/source-profile.json'
PATCH=ROOT/'patches/native-mali-metal/0001-strict-mali-profile.patch'
FILES=['NativeBridgeMetalFrontend.mm','NativeBridgeMetalFrontend.h',
       'bridge/NativeMetalBridge.rs','bridge/NativeMetalObjects.rs',
       'bridge/NativeMetalBridge.h','bridge/NativeMetalObjects.h']
EDITABLE={'NativeBridgeMetalFrontend.mm','NativeBridgeMetalFrontend.h'}


def git(*args):
    return subprocess.check_output(['git','-C',KIT,*args],text=True).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    pin=subprocess.check_output(['git','rev-parse','HEAD:drivers/NativeMetal'],cwd=ROOT,text=True).strip()
    if git('rev-parse','HEAD')!=pin:raise SystemExit('NativeMetal checkout differs from the parent gitlink')
    if git('status','--porcelain'):raise SystemExit('NativeMetal checkout must stay clean')
    if not OUT.is_dir() or not STAMP.is_file():raise SystemExit('Prepare the NativeMali Metal source copy first')
    profile=json.loads(STAMP.read_text())
    if profile.get('gitlink')!=pin or profile.get('inputs')!={f:digest(KIT/f) for f in FILES}:
        raise SystemExit('Prepared source does not match the current NativeMetal pin')
    if profile.get('patch_sha256')!=digest(PATCH):raise SystemExit('Prepared source was not made from the current parent patch')
    old_outputs=profile.get('outputs',{})
    for name in FILES:
        if name not in EDITABLE and old_outputs.get(name)!=digest(OUT/name):
            raise SystemExit('Unexpected generated-source edit: '+name)
    blocks=[]
    for name in FILES:
        before,after=KIT/name,OUT/name
        if before.read_bytes()==after.read_bytes():continue
        blocks.append(f'diff --git a/{name} b/{name}\n')
        blocks.extend(difflib.unified_diff(before.read_text().splitlines(keepends=True),
            after.read_text().splitlines(keepends=True),fromfile='a/'+name,tofile='b/'+name))
    if not blocks:raise SystemExit('Prepared Mali source has no parent-tracked changes')
    PATCH.write_text(''.join(blocks))
    profile['patch_sha256']=digest(PATCH)
    profile['outputs']={f:digest(OUT/f) for f in FILES}
    STAMP.write_text(json.dumps(profile,indent=2,sort_keys=True)+'\n')
    print(f'Exported {len(blocks)} unified-diff records; NativeMetal gitlink remains {pin}')


if __name__=='__main__':main()
