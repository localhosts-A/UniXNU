#!/usr/bin/env python3
"""Load the real DarwinBoot driver before UEFI system consoles exist."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu.artifacts import package_efi

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--uefi-efi', type=Path)
args = parser.parse_args()
output = ROOT / 'build/darwinboot-entry-test'
esp = output / 'esp'
payloads = esp / 'EFI/UniXNU/Payloads'
payloads.mkdir(parents=True, exist_ok=True)
(esp / 'EFI/BOOT').mkdir(exist_ok=True)
shutil.copyfile(package_efi(ROOT, args.uefi_efi), esp / 'EFI/BOOT/BOOTAA64.EFI')
for name in ('DarwinBoot', 'DarwinBootEntryValidation'):
    shutil.copyfile(ROOT / f'build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/{name}.efi', payloads / f'{name}.efi')
(esp / 'EFI/UniXNU/unixnu.toml').write_text('''version = 1
[boot]
timeout = 0
default = "exit"
[kits]
autoload = false
[[driver]]
path = "Payloads/DarwinBootEntryValidation.efi"
[[entry]]
id = "exit"
type = "action"
action = "exit"
''')
subprocess.run(['python3', ROOT / 'package/Scripts/qemu-run.py', esp,
                '--log', output / 'qemu.log', '--cpus', '1', '--no-gop', '--timeout', '20',
                '--finish-on-expect', '--expect', 'DARWINBOOT-ENTRY: no ConOut PASS',
                '--expect', 'DARWINBOOT-ENTRY: no ConOut or StdErr PASS',
                '--reject', 'ASSERT', '--reject', r'DARWINBOOT-ENTRY[^\n]*FAIL'], check=True)
