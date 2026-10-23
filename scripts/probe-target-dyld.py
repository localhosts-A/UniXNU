#!/usr/bin/env python3
"""Observe genuine supplied dyld instructions at EL0 on native-4K XNU, offline."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu.artifacts import package_efi

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--kernel', type=Path, default=ROOT / 'build/native-prelink/native-target-dyld-candidate.macho')
parser.add_argument('--output', type=Path, default=ROOT / 'build/darwinboot-target-dyld-entry')
parser.add_argument('--uefi-efi', type=Path, help='built UniXNU.efi; otherwise require exactly one build output')
parser.add_argument('--mach-init', action='store_true', help='Observe genuine original mach_init returning and qualify its page ABI')
parser.add_argument('--prepare', action='store_true', help='Continue genuine original startup toward its dependent-library preparation')
parser.add_argument('--load-cache', action='store_true', help='Observe the original loadDyldCache function returning to its real caller')
parser.add_argument('--log-syscalls', action='store_true', help='Record bounded actual saved syscall states to diagnose startup waits')
parser.add_argument('--log-service-matching', action='store_true', help='Observe genuine IOService class-match requests and their callers')
parser.add_argument('--os-image', type=Path, help='Original supplied OS UDIF, already attached read-only; replace the minimal RAM root')
parser.add_argument('--root-bsd-name', help='Actual previously observed OS volume BSD name')
parser.add_argument('--virtio-mmio-fdt', type=Path, help='Actual QEMU FDT for the corresponding offline block transport')
parser.add_argument('--apfs-component-launcher', type=Path, help='Own RAM launcher for two read-only data mounts/chroot/exec; not a production root')
parser.add_argument('--systemos-image', type=Path, help='Second existing raw SystemCryptex image for the component launcher')
parser.add_argument('--timeout', type=int, choices=range(1, 61), default=25, help='Bounded guest deadline, at most 60 seconds')
parser.add_argument('--corrupt-dyld-byte', type=lambda v: int(v, 0),
                    help='Negative control only: flip one code byte in the isolated RAM root, leaving original signatures/trust unchanged')
args = parser.parse_args()
if sum((args.mach_init, args.prepare, args.load_cache)) > 1:
    parser.error('Choose one original dyld checkpoint')
if args.corrupt_dyld_byte is not None and not args.mach_init:
    parser.error('The corruption refusal control requires --mach-init')
if args.apfs_component_launcher:
    if not all((args.os_image, args.systemos_image, args.virtio_mmio_fdt)) or args.root_bsd_name or args.corrupt_dyld_byte is not None:
        parser.error('The component launcher requires both original disk inputs and the FDT, without a boot-root override')
elif args.systemos_image:
    parser.error('--systemos-image requires the explicit own component launcher')
elif any((args.os_image, args.root_bsd_name, args.virtio_mmio_fdt)):
    if not all((args.os_image, args.root_bsd_name, args.virtio_mmio_fdt)) or args.corrupt_dyld_byte is not None:
        parser.error('A real OS root requires its image, actual BSD name and actual FDT, without RAM-root corruption')
    if not re.fullmatch(r'disk[0-9]+(?:s[0-9]+)*', args.root_bsd_name):
        parser.error('Expected an actual observed disk BSD name')
output = args.output.resolve()
fixture = ROOT / 'build/target-dyld-fixture'
inputs = json.loads((fixture / 'inputs.json').read_text())
prelink = json.loads(args.kernel.with_suffix('.json').read_text())
if hashlib.sha256(args.kernel.read_bytes()).hexdigest() != prelink['candidate_sha256']:
    raise SystemExit('Candidate identity mismatch')
subprocess.run(['python3', ROOT / 'scripts/audit-xnu-kernel.py', args.kernel], check=True)
esp = output / 'esp'
payloads = esp / 'EFI/UniXNU/Payloads'
payloads.mkdir(parents=True, exist_ok=True)
(esp / 'EFI/BOOT').mkdir(exist_ok=True)
for source, target in [
    (package_efi(ROOT, args.uefi_efi), esp / 'EFI/BOOT/BOOTAA64.EFI'),
    (ROOT / 'build/DarwinBoot/DEBUG_CLANGPDB/AARCH64/DarwinBoot.efi', payloads / 'DarwinBoot.efi'),
    (args.kernel, payloads / 'kernel.macho'),
    (fixture / 'original-static-trust.bundle', payloads / 'kernel.macho.trustcache')]:
    shutil.copyfile(source, target)
if args.apfs_component_launcher:
    launcher = args.apfs_component_launcher.parent
    launcher_metadata = json.loads((launcher / 'apfs-launchd.json').read_text())
    if (hashlib.sha256(args.apfs_component_launcher.read_bytes()).hexdigest() != launcher_metadata['own_process_sha256'] or
            hashlib.sha256((launcher / 'apfs-launchd-trust.bundle').read_bytes()).hexdigest() != launcher_metadata['trust_bundle_sha256']):
        raise SystemExit('Component launcher and explicit trust bundle identities must match their build record')
    if launcher_metadata['stop_before_exec']:
        raise SystemExit('The original-dyld probe needs a launcher that actually execs')
    shutil.copyfile(args.apfs_component_launcher, payloads / 'root.img')
    shutil.copyfile(launcher / 'apfs-launchd-trust.bundle', payloads / 'kernel.macho.trustcache')
elif not args.os_image:
    shutil.copyfile(fixture / 'root.img', payloads / 'root.img')
if not args.apfs_component_launcher and ((not args.os_image and hashlib.sha256((payloads / 'root.img').read_bytes()).hexdigest() != inputs['root_sha256']) or \
        hashlib.sha256((payloads / 'kernel.macho.trustcache').read_bytes()).hexdigest() != inputs['trust_bundle_sha256']):
    raise SystemExit('Exact original target inputs were not preserved')
negative_input = None
if args.corrupt_dyld_byte is not None:
    import struct
    image = bytearray((payloads / 'root.img').read_bytes())
    _, _, _, _, _, dyld_offset, dyld_size = struct.unpack_from('<8sII4Q', image)
    at = args.corrupt_dyld_byte
    if not 0 <= at < min(dyld_size, 0x80000):
        raise SystemExit('Select a byte in original dyld code, not its signature')
    before = image[dyld_offset + at]
    image[dyld_offset + at] ^= 1
    (payloads / 'root.img').write_bytes(image)
    negative_input = dict(file='usr/lib/dyld', file_offset=at, before=before, after=before ^ 1,
        input_copy_sha256=hashlib.sha256(image).hexdigest(), signatures_or_trust_modified=False,
        supplied_original_file_modified=False, input_fault_instrument_only=True)
dt_command = ['python3', ROOT / 'scripts/qemu-entry/make-dt.py', '--output', payloads / 'kernel.macho.adt',
    '--dram-size', '0x40000000', '--armvirt', '--armcpu', '--nvram-proxy',
    '--crypto-hash-method', 'sha2-384', '--amfi-platform-only', '--trust-cache']
if args.os_image:
    dt_command += ['--virtio-mmio-fdt', args.virtio_mmio_fdt]
if not args.os_image or args.apfs_component_launcher:
    dt_command += ['--ramdisk']
subprocess.run(dt_command, check=True)
(esp / 'EFI/UniXNU/unixnu.toml').write_text('''version = 1
[boot]
timeout = 1
default = "target-dyld"
[kits]
autoload = false
[[driver]]
path = "Payloads/DarwinBoot.efi"
[[entry]]
id = "target-dyld"
title = "Original target dyld offline native 4K probe"
type = "xnu"
path = "Payloads/kernel.macho"
initrd = "Payloads/root.img"
args = "rd=md0 -v serial=3 debug=0x14e kt_fixed_zones=120"
''')
if args.os_image:
    if 'com.apple.filesystems.apfs' not in prelink['components']:
        raise SystemExit('The genuine original APFS component is required')
    config = esp / 'EFI/UniXNU/unixnu.toml'
    if args.apfs_component_launcher:
        config.write_text(config.read_text().replace('rd=md0 ', 'rd=md0 offline-apfs-load=1 '))
    else:
        config.write_text(config.read_text().replace('initrd = "Payloads/root.img"\n', '').replace(
            'rd=md0 ', f'rd={args.root_bsd_name} offline-apfs-load=1 '))
symbols = subprocess.check_output(['xcrun', 'nm', '-U', args.kernel], text=True)
addresses = {name: int(at, 16) for at, name in re.findall(r'^([0-9a-fA-F]+)\s+\S\s+(_\S+)$', symbols, re.M)}
checkpoint = inputs['dyld_symbols']['_mach_init'] if args.mach_init else inputs['dyld_startup_checkpoint']
if args.prepare:
    checkpoint = inputs['dyld_symbols']['__ZN5dyld4L7prepareERNS_4APIsEPKN5dyld313MachOAnalyzerE'] + 4
if args.load_cache:
    dyld_path = fixture / 'dyld'
    expected = next(f['sha256'] for f in inputs['files'] if f['path'] == 'usr/lib/dyld')
    if hashlib.sha256(dyld_path.read_bytes()).hexdigest() != expected:
        raise SystemExit('Unexpected original dyld symbol input')
    dyld_names = subprocess.check_output(['xcrun', 'nm', '-U', dyld_path], text=True)
    cache_name = '__ZN5dyld313loadDyldCacheERKNS_18SharedCacheOptionsEPNS_19SharedCacheLoadInfoE'
    cache_match = re.search(r'^([0-9a-fA-F]+)\s+\S\s+' + re.escape(cache_name) + '$', dyld_names, re.M)
    if not cache_match:
        raise SystemExit('The actual original cache loader symbol is unavailable')
    checkpoint = int(cache_match[1], 16) + 4
command = ['python3', ROOT / 'scripts/qemu-entry/gdb-stop.py', hex(addresses['_thread_setentrypoint']),
    '--fixture', output, '--esp', esp, '--kernel', args.kernel, '--gic-version', '3', '--system-registers',
    '--timeout', str(args.timeout), '--follow-entrypoint-arg', '1', '--step-followed-entry', '64',
    '--follow-entrypoint-checkpoint-offset=' + str(checkpoint - inputs['dyld_preferred_entry'])]
if args.os_image:
    command += ['--virtio-udif-image', args.os_image]
if args.apfs_component_launcher:
    command += ['--virtio-extra-block-image', args.systemos_image, '--skip-target-hits', '1']
dyld_requests = {}
if args.mach_init or args.prepare or args.load_cache:
    if args.mach_init or args.load_cache:
        command += ['--finish-followed-checkpoint']
    if args.load_cache:
        command += ['--read-followed-checkpoint-arg', '1:40']
    for name, width in dict(_vm_page_size=8, _vm_page_mask=8, _vm_page_shift=4, _vm_kernel_page_shift=4).items():
        request = f'{inputs["dyld_symbols"][name] - inputs["dyld_preferred_entry"]}:{width:x}'
        dyld_requests[name] = request
        command += ['--read-followed-relative', request]
for name in ('_panic', '_panic_with_options'):
    command += ['--also-stop', hex(addresses[name])]
if args.log_syscalls:
    for name in ('_unix_syscall', '_mach_syscall'):
        command += ['--syscall-log-address', hex(addresses[name])]
if args.log_service_matching:
    command += ['--cstring-log-address', hex(addresses['__ZN9IOService15serviceMatchingEPKcP12OSDictionary'])]
inspections = dict(_PAGE_SHIFT_CONST=4, _page_shift=4, _page_size=8,
                   _cs_validate_page_bad_hash=8, _cs_validate_page_no_hash=8, _amfi=8,
                   _armcpu_timer_interrupts=8, _armcpu_ipi_interrupts=8, _armcpu_gic_ready=4)
if '_armvirt_platform_defaults_count' in addresses:
    inspections['_armvirt_platform_defaults_count'] = 4
if args.os_image:
    inspections.update(_native_virtio_ready=4, _native_virtio_read_requests=4, _native_virtio_read_failures=4,
                       _native_virtio_sectors=8, _native_virtio_mmio_physical=8)
for name in ('_native_aes_kat_status', '_native_aes_interface_status', '_native_aes_registered',
             '_native_aes_completed', '_native_aes_unsupported_key'):
    if name in addresses:
        inspections[name] = 4
requests = {name: f'{addresses[name]:x}:{width:x}' for name, width in inspections.items()}
for request in requests.values():
    command += ['--read-memory', request]
for name in ('_commpage_rw_addr', '_commpage_kernel_ro_addr'):
    command += ['--read-indirect', f'{addresses[name]:x}:25:1']
command += ['--read-memory', 'fffff4025:1', '--read-memory', 'fffff4037:1']
subprocess.run(command, check=True)
report_path = output / 'gdb-stop.json'
d = json.loads(report_path.read_text())
r = {k: int(v, 16) for k, v in d['system_registers'].items()}
native = (r['TCR_EL1'] >> 14 & 3 == 0 and r['TCR_EL1'] >> 30 & 3 == 2 and
          r['SCTLR_EL1'] & 1 and r['TTBR0_EL1'] and r['TTBR1_EL1'])
d['native_4k_mmu_observed'] = bool(native)
d['target_inputs'] = inputs
if args.os_image:
    d['target_inputs'] = {k: v for k, v in inputs.items() if k not in ('root_sha256', 'root_bytes')}
    if args.apfs_component_launcher:
        d['boot_root_input'] = 'own RAM fixture; original APFS volumes are read-only data mounts'
        d['offline_component_launcher'] = launcher_metadata
        d['root_authentication_solved'] = False
        d['actual_trust_bundle_sha256'] = hashlib.sha256((payloads / 'kernel.macho.trustcache').read_bytes()).hexdigest()
        d['actual_ram_root_sha256'] = hashlib.sha256((payloads / 'root.img').read_bytes()).hexdigest()
    else:
        d['boot_root_input'] = 'actual supplied OS APFS volume; no RAM root'
        d['selected_bsd_name'] = args.root_bsd_name
if negative_input:
    d['negative_input_control'] = negative_input
d['kernel_observations'] = {name: int.from_bytes(bytes.fromhex(d['memory'][request]), 'little')
                            for name, request in requests.items()}
entry = d.get('followed_user_entry', {})
dyld = (fixture / 'dyld').read_bytes()
slide = int(entry['user_entry'], 16) - inputs['dyld_preferred_entry'] if entry else None
confirmed = []
if entry.get('observed') and int(entry['pstate'], 16) & 15 == 0 and \
        entry['entry_bytes'] == dyld[inputs['dyld_preferred_entry']:inputs['dyld_preferred_entry'] + 16].hex():
    for step in entry['steps']:
        offset = int(step['pc'], 16) - slide
        if int(step['pstate'], 16) & 15 == 0 and 0 <= offset < 0x80000 - 16 and \
                step['bytes'] == dyld[offset:offset + 16].hex():
            confirmed.append(step['pc'])
d['original_dyld_el0_instruction_observations'] = len(confirmed)
d['original_dyld_slide'] = hex(slide) if slide is not None else None
if args.mach_init or args.prepare or args.load_cache:
    d['original_dyld_page_abi'] = {}
    for name, request in dyld_requests.items():
        raw = d.get('followed_relative_memory', {}).get(request, {}).get('bytes', 'E14')
        d['original_dyld_page_abi'][name] = int.from_bytes(bytes.fromhex(raw), 'little') if not raw.startswith('E') else None
    if args.mach_init:
        d['original_dyld_mach_init_returned'] = entry.get('function_return', {}).get('observed', False)
    if args.prepare:
        d['original_dyld_prepare_entered'] = entry.get('checkpoint', {}).get('observed', False)
    if args.load_cache:
        d['original_dyld_cache_loader_returned'] = entry.get('function_return', {}).get('observed', False)
        d['original_dyld_cache_loader_symbol'] = cache_name
    d['original_dyld_native_4k_abi_observed'] = ((entry.get('function_return', {}).get('observed', False) if args.mach_init or args.load_cache else
        entry.get('checkpoint', {}).get('observed', False)) and
        d['original_dyld_page_abi'] == dict(_vm_page_size=4096, _vm_page_mask=4095, _vm_page_shift=12, _vm_kernel_page_shift=12))
if (d.get('argument_memory', {}).get('x3') or '').startswith('%s %s -- exit reason namespace'):
    import struct
    raw = d['stack_bytes']
    if not raw.startswith('E'):
        exit_args = struct.unpack_from('<5Q', bytes.fromhex(raw))
        d['target_process_exit'] = dict(namespace=exit_args[2] & 0xffffffff, code=exit_args[3])
d['target_launchd_main_executed'] = False
d['functional_ipados_boot_demonstrated'] = False
d['sheng_hardware_tested'] = False
report_path.write_text(json.dumps(d, indent=2) + '\n')
if negative_input:
    if native and not d['bounded_timeout_interrupted'] and not entry.get('function_return', {}).get('observed') and \
            d['kernel_observations']['_cs_validate_page_bad_hash'] >= 1:
        print('PASS: corrupted original dyld input copy was refused by genuine code-page validation')
        raise SystemExit(0)
    raise SystemExit('Original dyld corruption refusal is not yet established; inspect the bounded report')
if not native or d['bounded_timeout_interrupted'] or len(confirmed) < 8 or \
        ((args.mach_init or args.prepare or args.load_cache) and not d['original_dyld_native_4k_abi_observed']):
    raise SystemExit('Target dyld execution is not yet established; inspect the bounded report')
print(f'PASS: {len(confirmed)} byte-matched original dyld instruction observations at native-4K EL0')
print('Original static trust module is unchanged; authenticated boot chain and functional iPadOS remain unqualified')
