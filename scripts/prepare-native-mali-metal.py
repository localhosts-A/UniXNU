#!/usr/bin/env python3
"""Prepare a pinned NativeMetal copy with the parent-owned Mali profile patch."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
FILES = ['NativeBridgeMetalFrontend.mm', 'NativeBridgeMetalFrontend.h',
         'bridge/NativeMetalBridge.rs', 'bridge/NativeMetalObjects.rs',
         'bridge/NativeMetalBridge.h', 'bridge/NativeMetalObjects.h']


def prepare():
    kit = ROOT/'drivers/NativeMetal'
    pin = subprocess.check_output(['git', '-C', ROOT, 'rev-parse', 'HEAD:drivers/NativeMetal'], text=True).strip()
    if subprocess.check_output(['git', '-C', kit, 'rev-parse', 'HEAD'], text=True).strip() != pin:
        raise SystemExit('NativeMetal checkout differs from the parent gitlink')
    if subprocess.check_output(['git', '-C', kit, 'status', '--porcelain'], text=True).strip():
        raise SystemExit('NativeMetal checkout must be clean')
    patch = ROOT/'patches/native-mali-metal/0001-strict-mali-profile.patch'
    profile = {'schema': 1, 'gitlink': pin, 'patch_sha256': hashlib.sha256(patch.read_bytes()).hexdigest(),
               'inputs': {f: hashlib.sha256((kit/f).read_bytes()).hexdigest() for f in FILES},
               'metal_vocabulary': 'MacOS26_7', 'family_waivers': [], 'hardware_exercised': False}
    out = ROOT/'build/native-mali-metal/source'
    stamp = out.parent/'source-profile.json'
    if stamp.exists():
        old = json.loads(stamp.read_text())
        if {k: v for k, v in old.items() if k != 'outputs'} != profile:
            raise SystemExit('Mali Metal source inputs changed; preserve the previous build before preparing again')
        if old['outputs'] != {f: hashlib.sha256((out/f).read_bytes()).hexdigest() for f in FILES}:
            raise SystemExit('Prepared Mali Metal source was modified')
        return out
    out.mkdir(parents=True, exist_ok=True)
    for f in FILES:
        (out/f).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(kit/f, out/f)
    command = ['git', 'apply', '--unidiff-zero', '--directory', str(out.relative_to(ROOT))]
    subprocess.run([*command, '--check', str(patch)], cwd=ROOT, check=True)
    subprocess.run([*command, str(patch)], cwd=ROOT, check=True)
    if 'const TARGET_FAMILIES: [GpuFamily; 0] = [];' not in (out/'bridge/NativeMetalObjects.rs').read_text():
        raise SystemExit('Mali profile patch did not apply')
    profile['outputs'] = {f: hashlib.sha256((out/f).read_bytes()).hexdigest() for f in FILES}
    stamp.write_text(json.dumps(profile, indent=2, sort_keys=True)+'\n')
    return out


if __name__ == '__main__':
    print(prepare())
