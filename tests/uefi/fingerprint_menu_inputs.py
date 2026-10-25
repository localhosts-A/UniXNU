#!/usr/bin/env python3
"""Record exact source/EDK2 header hashes; no shared CI cache is used."""
import hashlib
import json
import os
from pathlib import Path
import subprocess


def inputs(root, *prefixes):
    paths = subprocess.check_output(['git', 'ls-files', '-z', '--', *prefixes], cwd=root).split(b'\0')
    return {os.fsdecode(p): hashlib.sha256((root / os.fsdecode(p)).read_bytes()).hexdigest()
            for p in sorted(paths) if p}


def main():
    root = Path(__file__).resolve().parents[2]
    edk = Path(os.environ.get('DALI_USB_EDK2', root.parent / 'UEFI-Dali/Mu_Basecore')).resolve()
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=edk, text=True).strip()
    if commit != '3c9c7b2e211e62d2a954ad760a36fe05adc43aa3':
        raise RuntimeError('unexpected EDK2 header source commit')
    result = {'scope': 'host production-menu input fingerprint; no hardware qualification',
              'source': inputs(root, 'package/UniXNUPkg', 'tests/uefi', '.github/workflows/usb-core.yml'),
              'edk2_commit': commit, 'edk2_headers': inputs(edk, 'MdePkg/Include'),
              'pcd_modes': [False, True], 'shared_ci_cache': False, 'hardware_qualified': False}
    if not result['source'] or not result['edk2_headers']:
        raise RuntimeError('missing tracked source/header inputs')
    encoded = json.dumps(result, sort_keys=True, separators=(',', ':')).encode()
    result['input_fingerprint'] = hashlib.sha256(encoded).hexdigest()
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
