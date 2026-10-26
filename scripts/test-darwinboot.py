#!/usr/bin/env python3
"""Exercise UniXNU -> DarwinBoot -> ExitBootServices -> synthetic Mach-O."""
import argparse
import hashlib
import json
import os
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
parser.add_argument('--storage-handoff', action='store_true', help='Exercise failed storage quiesce, sealed ownership and actual loader EBS retry')
parser.add_argument('--edk2', type=Path, help='Mu/EDK2 checkout for the storage-handoff mode rebuild')
args = parser.parse_args()
if args.ramfb_geometry_refusal and (args.gop or args.gop_high_resolution):
    parser.error('The refusal control requires no GOP device')
if args.gop_high_resolution:
    args.gop = True
if args.edk2 and not args.storage_handoff:
    parser.error('--edk2 is only used with --storage-handoff')
OUTPUT = ROOT / ('build/darwinboot-test' if not args.memory_size else f'build/darwinboot-test-{args.memory_size >> 20}m')
if args.gop:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-gop')
if args.packed_ramdisk:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-packed')
if args.gop_high_resolution:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-3048x2032')
if args.ramfb_geometry_refusal:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-geometry-refusal')
if args.storage_handoff:
    OUTPUT = OUTPUT.with_name(OUTPUT.name + '-storage-handoff')
ESP = OUTPUT / 'esp'
PAYLOADS = ESP / 'EFI/UniXNU/Payloads'

OUTPUT.mkdir(parents=True, exist_ok=True)
if args.storage_handoff:
    # Do not leave a prior PASS available after any failed build/entry/QEMU run.
    (OUTPUT / 'storage-handoff-result.json').unlink(missing_ok=True)
    env = dict(os.environ)
    if args.edk2:
        env['UNIXNU_EDK2'] = str(args.edk2.resolve())
    subprocess.run(['bash', ROOT / 'scripts/build-darwinboot.sh'], env=env, check=True)
subprocess.run(['python3', ROOT / 'scripts/test-darwinboot-entry.py',
                '--uefi-efi', package_efi(ROOT, args.uefi_efi)], check=True)
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
if args.storage_handoff:
    shutil.copyfile(ROOT / 'build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBootStorageValidation.efi',
                    PAYLOADS / 'DarwinBootStorageValidation.efi')
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
if args.storage_handoff:
    config = ESP / 'EFI/UniXNU/unixnu.toml'
    config.write_text(config.read_text().replace('[[entry]]',
        '[[driver]]\npath = "Payloads/DarwinBootStorageValidation.efi"\n[[entry]]', 1))
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
                '--expect', 'DARWINBOOT-VALIDATION: 40 rejected requests; firmware retained PASS',
                '--expect', 'DARWINBOOT-VIDEO-VALIDATION: 8 malformed/unsupported GOP field sets refused PASS',
                '--expect', 'DARWINBOOT: inputs validated; exiting boot services',
                '--expect', 'DARWINBOOT: ExitBootServices returned status=Success',
                '--expect', 'DARWINBOOT: entering loader trampoline',
                *(['--expect', 'DARWINBOOT-STORAGE-VALIDATION: failed quiesce refused kernel entry PASS',
                   '--expect', 'DARWINBOOT-STORAGE-VALIDATION: provider sealed before final map PASS',
                   '--expect', 'DARWINBOOT-STORAGE-VALIDATION: forced EBS retry while sealed PASS',
                   '--expect', 'DARWINBOOT: ExitBootServices returned status=Invalid Parameter'] if args.storage_handoff else []),
                '--expect', 'DARWINBOOT: trampoline enter',
                '--expect', 'DARWINBOOT: trampoline framebuffer complete',
                '--expect', 'DARWINBOOT: trampoline cache clean complete',
                '--expect', 'DARWINBOOT: trampoline ready to disable MMU',
                '--expect', 'DARWINBOOT-FIXTURE: loader whole-file kernel measurement PASS',
                *(['--expect', 'QEMU-RAMFB-GEOMETRY: actual SetMode 3048x2032'] if args.gop_high_resolution else []),
                *(['--expect', 'QEMU-RAMFB-GEOMETRY: REFUSED no GOP'] if args.ramfb_geometry_refusal else []),
                '--expect', ('DARWINBOOT-FIXTURE: actual GOP boot video after ExitBootServices PASS' if args.gop else
                             'DARWINBOOT-FIXTURE: boot_args, RAMDisk, TrustCache, zero-fill and MMU-off entry PASS'),
                '--reject', r'DARWINBOOT[^\n]*FAIL', '--reject', 'ASSERT'], check=True)
if args.storage_handoff:
    text = (OUTPUT / 'qemu.log').read_text(errors='replace')
    markers = ['DARWINBOOT-STORAGE-VALIDATION: failed quiesce refused kernel entry PASS',
               'DARWINBOOT-STORAGE-VALIDATION: provider sealed before final map PASS',
               'DARWINBOOT: inputs validated; exiting boot services',
               'DARWINBOOT-STORAGE-VALIDATION: forced EBS retry while sealed PASS',
               'DARWINBOOT: ExitBootServices returned status=Invalid Parameter',
               'DARWINBOOT: ExitBootServices returned status=Success',
               'DARWINBOOT: entering loader trampoline']
    position = -1
    for marker in markers:
        position = text.index(marker, position + 1)
    if text.count(markers[1]) != 1 or text.count(markers[3]) != 1:
        raise SystemExit('Storage handoff or EBS injection was repeated unexpectedly')
    (OUTPUT / 'storage-handoff-result.json').write_text(json.dumps({
        'failed_quiesce_refuses_kernel_entry': True, 'provider_sealed_before_final_map': True,
        'actual_loader_ebs_retry_while_sealed': True, 'synthetic_kernel_entry_pass': True,
        'loader_whole_file_kernel_measurement_after_ebs': True,
        'fixture_kernel_sha256': hashlib.sha256((PAYLOADS / 'kernel.macho').read_bytes()).hexdigest(),
        'darwinboot_built_this_run': True,
        'efi_sha256': {name: hashlib.sha256((PAYLOADS / (name + '.efi')).read_bytes()).hexdigest()
                       for name in ('DarwinBoot', 'DarwinBootStorageValidation')},
        'source_sha256': {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest()
                          for path in (ROOT / 'drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c',
                                       ROOT / 'drivers/DarwinBoot/DarwinBootPkg/Driver/StorageHandoff.c',
                                       ROOT / 'drivers/DarwinBoot/DarwinBootPkg/Driver/KernelMeasurement.c',
                                       ROOT / 'drivers/DarwinBoot/DarwinBootPkg/Driver/KernelMeasurement.h',
                                       ROOT / 'drivers/DarwinBoot/Test/StorageValidation.c',
                                       ROOT / 'package/UniXNUPkg/Include/Protocol/UniXNUStorageQuiesce.h')},
        'raw_console_sha256': hashlib.sha256((OUTPUT / 'qemu.log').read_bytes()).hexdigest(),
        'physical_storage_qualified': False, 'xnu_ufs_qualified': False,
    }, indent=2) + '\n')
