#!/usr/bin/env python3
"""Exercise UniXNU -> DarwinBoot -> ExitBootServices -> synthetic Mach-O."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu.artifacts import package_efi

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--memory-size', type=lambda value: int(value, 0), choices=[0x10000000, 0x20000000])
parser.add_argument('--uefi-efi', type=Path, help='built UniXNU.efi; otherwise require exactly one build output')
parser.add_argument('--packed-ramdisk', action='store_true', help='Exercise real packed seed expansion before firmware exit')
parser.add_argument('--gop', action='store_true', help='Exercise the actual QEMU ramfb UEFI GOP handoff')
parser.add_argument('--gop-high-resolution', action='store_true', help='Exercise real QEMU ramfb 3048x2032 allocation and mode programming')
parser.add_argument('--ramfb-geometry-refusal', action='store_true', help='Load the geometry fixture without a GOP and require a clean refusal')
args = parser.parse_args()
if args.ramfb_geometry_refusal and (args.gop or args.gop_high_resolution):
    parser.error('The refusal control requires no GOP device')
if args.gop_high_resolution:
    args.gop = True
OUTPUT = ROOT / ('build/darwinboot-test' if not args.memory_size else f'build/darwinboot-test-{args.memory_size >> 20}m')
if args.gop:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-gop')
if args.packed_ramdisk:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-packed')
if args.gop_high_resolution:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-3048x2032')
if args.ramfb_geometry_refusal:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-geometry-refusal')
ESP = OUTPUT / 'esp'
PAYLOADS = ESP / 'EFI/UniXNU/Payloads'

OUTPUT.mkdir(parents=True, exist_ok=True)
PAYLOADS.mkdir(parents=True, exist_ok=True)
subprocess.run(['python3', ROOT / 'drivers/DarwinBoot/Test/make-fixtures.py', PAYLOADS,
                *(['--memory-size', str(args.memory_size)] if args.memory_size else []),
                *(['--gop'] if args.gop else []),
                *(['--gop-width', '3048', '--gop-height', '2032'] if args.gop_high_resolution else [])], check=True)
if args.packed_ramdisk:
    import sys
    sys.path.insert(0, str(ROOT / 'drivers/DarwinBoot/Test'))
    from packed_ramdisk import pack
    raw = PAYLOADS / 'ramdisk.bin'
    raw.write_bytes(raw.read_bytes() + bytes(-raw.stat().st_size % 4096))
    packed = PAYLOADS / 'ramdisk.packed'
    packed.unlink(missing_ok=True)
    pack(raw, packed)
    packed.replace(raw)
(ESP / 'EFI/BOOT').mkdir(exist_ok=True)
shutil.copyfile(package_efi(ROOT, args.uefi_efi), ESP / 'EFI/BOOT/BOOTAA64.EFI')
for name in ('DarwinBoot', 'DarwinBootValidation'):
    shutil.copyfile(ROOT / f'build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/{name}.efi', PAYLOADS / f'{name}.efi')
(ESP / 'EFI/UniXNU/unixnu.toml').write_text('''version = 1
[boot]
timeout = 1
default = "synthetic"
[kits]
autoload = false
[[driver]]
path = "Payloads/DarwinBoot.efi"
[[driver]]
path = "Payloads/DarwinBootValidation.efi"
[[entry]]
id = "synthetic"
title = "Synthetic Darwin ABI entry"
type = "xnu"
path = "Payloads/kernel.macho"
args = "darwinboot.fixture=1"
initrd = "Payloads/ramdisk.bin"
''')
if args.gop_high_resolution or args.ramfb_geometry_refusal:
    driver = ROOT / 'build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/QemuRamfbGeometry.efi'
    shutil.copyfile(driver, PAYLOADS / driver.name)
    config = ESP / 'EFI/UniXNU/unixnu.toml'
    config.write_text(config.read_text().replace('[[entry]]',
        '[[driver]]\npath = "Payloads/QemuRamfbGeometry.efi"\n[[entry]]', 1))
subprocess.run(['python3', ROOT / 'package/Scripts/qemu-run.py', ESP,
                '--log', OUTPUT / 'qemu.log', '--cpus', '1', *([] if args.gop else ['--no-gop']), '--timeout', '25',
                '--finish-on-expect',
                '--expect', 'DARWINBOOT: loader v2 installed EL1',
                '--expect', 'DARWINBOOT-VALIDATION: 33 rejected requests; firmware retained PASS',
                '--expect', 'DARWINBOOT-VIDEO-VALIDATION: 8 malformed/unsupported GOP field sets refused PASS',
                '--expect', 'DARWINBOOT: inputs validated; exiting boot services',
                *(['--expect', 'QEMU-RAMFB-GEOMETRY: actual SetMode 3048x2032'] if args.gop_high_resolution else []),
                *(['--expect', 'QEMU-RAMFB-GEOMETRY: REFUSED no GOP'] if args.ramfb_geometry_refusal else []),
                '--expect', ('DARWINBOOT-FIXTURE: actual GOP boot video after ExitBootServices PASS' if args.gop else
                             'DARWINBOOT-FIXTURE: boot_args, RAMDisk, TrustCache, zero-fill and MMU-off entry PASS'),
                '--reject', r'DARWINBOOT[^\n]*FAIL', '--reject', 'ASSERT'], check=True)
