#!/usr/bin/env python3
"""Stop the prepared QEMU entry fixture at an address and record registers."""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import shutil
import socket
import stat
import struct
import subprocess
import time
import tempfile
import xml.etree.ElementTree as ET
from gdb_stop_support import (finish_stop_action, parse_indirect_memory_request,
                              parse_trace_register_memory_request)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('address', type=lambda value: int(value, 0))
parser.add_argument('--qemu-accel', choices=['tcg', 'hvf'], default='tcg', help='Offline QEMU accelerator; HVF is restricted to macOS ARM64 ESP tests')
parser.add_argument('--qemu-memory-mib', type=int, choices=[512, 1024, 2048, 4096, 8192],
                    help='Actual VM RAM; defaults remain 1024 MiB for ESP and 512 MiB for raw fixtures')
parser.add_argument('--fixture', type=Path, default=Path('build/xnu-entry'))
parser.add_argument('--read-memory', action='append', default=[], metavar='HEX_ADDRESS:HEX_LENGTH')
parser.add_argument('--read-indirect', action='append', default=[], metavar='HEX_POINTER_ADDRESS:HEX_OFFSET:HEX_LENGTH')
parser.add_argument('--read-indirect-optional', action='append', default=[],
                    metavar='HEX_POINTER_ADDRESS:HEX_OFFSET:HEX_LENGTH',
                    help='Best-effort bounded pointer-field read; report an unreadable target without failing the stop')
parser.add_argument('--mac-policy-layout',type=Path,help='Compiler-derived bounded read-only registered policy observer')
parser.add_argument('--system-registers', action='store_true')
parser.add_argument('--live-hid-dir',type=Path,help='QEMU-only own HID mailbox at actual timer callbacks; bounded by timeout')
parser.add_argument('--exercise-ufs-allocation', action='store_true',
                    help='Offline only: invoke actual XNU UFS-sized allocation APIs after the checkpoint; never maps UFS hardware')
parser.add_argument('--esp', type=Path, help='Boot a read-only ESP through real QEMU UEFI instead of the raw fixture')
parser.add_argument('--kernel', type=Path, help='Kernel whose SHA-256 identifies an ESP test')
block_group = parser.add_mutually_exclusive_group()
block_group.add_argument('--virtio-block-image', type=Path, help='Attach an existing sector-aligned raw file as a strictly read-only modern MMIO disk')
block_group.add_argument('--virtio-udif-image', type=Path, help='Read an already attached read-only UDIF image through its verified host virtual media')
parser.add_argument('--virtio-extra-block-image', type=Path, help='Second existing raw file, strictly read-only, for offline two-volume component tests')
parser.add_argument('--missing-virtio-device', action='store_true', help='Refusal control: leave described MMIO windows empty while retaining the read-only input identity')
parser.add_argument('--capture-screen',action='store_true',help='Read the actual stopped QEMU console through QMP screendump into a PNG; no guest execution or drawing')
parser.add_argument('--qemu-ramfb', action='store_true', help='Attach actual offline QEMU ramfb for UEFI GOP handoff qualification')
parser.add_argument('--virtio-console-socket', type=Path, help='Connect an existing private Unix socket to real modern VirtIO console port zero')
parser.add_argument('--firmware', type=Path, help='QEMU AArch64 UEFI code FD for an ESP test')
parser.add_argument('--timeout', type=float, default=3, help='Maximum seconds waiting for the guest breakpoint')
parser.add_argument('--primary-start-address', type=lambda value: int(value, 0), help='Arm the primary breakpoint only after this actual one-shot kernel entry')
parser.add_argument('--trace-address', action='append', default=[], type=lambda value: int(value, 0))
parser.add_argument('--trace-read-register-offset', action='append', default=[],
                    metavar='HEX_PC:x0-x30:HEX_OFFSET:HEX_LENGTH',
                    help='Read a bounded kernel field relative to a register at a selected trace PC')
parser.add_argument('--syscall-number', type=int, action='append', default=[], help='Capture only these genuine syscall numbers, preserving the bounded event budget')
parser.add_argument('--syscall-start-address', type=lambda value: int(value, 0), help='Arm syscall saved-state observation only after this actual kernel entry')
parser.add_argument('--kext-log-address', type=lambda value: int(value, 0), help='Capture up to 256 real OSKextLog calls within the same guest deadline')
parser.add_argument('--syscall-log-address', type=lambda value: int(value, 0), action='append', default=[],
                    help='Observe genuine Darwin 23 ARM64 syscall handlers; at most 512 saved states in the same deadline')
parser.add_argument('--cstring-log-address', type=lambda value: int(value, 0), action='append', default=[],
                    help='Observe at most 128 genuine calls whose first argument is a kernel C string')
parser.add_argument('--cstring-start-address', type=lambda value: int(value, 0), help='Arm C-string observation only after this real entrypoint; conserves hardware breakpoints and the bounded log budget')
parser.add_argument('--registry-string-log-address', type=lambda value: int(value, 0),
                    help='Observe at most 128 real IORegistryEntry::setProperty(char const*, char const*) calls')
parser.add_argument('--also-stop', action='append', default=[], type=lambda value: int(value, 0))
parser.add_argument('--finish-function', action='store_true', help='After the entry breakpoint, stop at its caller return address')
parser.add_argument('--skip-target-hits', type=int, default=0,
                    help='Skip at most 16 earlier calls to the primary breakpoint within the same deadline')
parser.add_argument('--follow-entrypoint-arg', type=int, choices=range(4),
                    help='Observe the actual user PC passed in x0-x3, then follow it without changing state')
parser.add_argument('--follow-main-entry-offset', type=lambda value: int(value, 0), help='Observe the main executable LC_MAIN offset relative to its actual Mach header on the original dyld startup stack')
parser.add_argument('--main-cache-query-plan', type=Path, help='Follow an audited original cache display query after real main entry; read-only hardware breakpoints')
parser.add_argument('--process-layout-plan', type=Path, help='Inspect current process identity with compiler-qualified kernel layout')
parser.add_argument('--allproc-address', type=lambda value: int(value, 0), help='Read the actual kernel allproc list once at the final stop')
parser.add_argument('--proc-list-offset', type=int, default=0, help='Byte offset of p_list.le_next in struct proc')
parser.add_argument('--proc-pid-offset', type=int, default=96, help='Byte offset of p_pid')
parser.add_argument('--proc-comm-offset', type=int, default=1468, help='Byte offset of p_comm')
parser.add_argument('--proc-stat-offset', type=int, default=100, help='Byte offset of p_stat')
parser.add_argument('--proc-list-max', type=int, default=128, help='Bounded number of actual process entries')
parser.add_argument('--inspect-ui-threads', action='store_true', help='Read original UI task thread lists and kernel-owned saved user registers, without changing address spaces')
parser.add_argument('--capture-scratch-data', metavar='BASE_SYMBOL:SIZE_SYMBOL:EXPECTED_BYTES',
                    help='Read the exact native mutable Data RAM range into an offline snapshot; never flush or modify guest state')

parser.add_argument('--step-followed-entry', type=int, default=0,
                    help='Execute at most 64 real instructions after reaching that user entry')
parser.add_argument('--follow-entrypoint-checkpoint-offset', type=lambda value: int(value, 0),
                    help='After entry, continue normally to this source-derived relative user checkpoint')
parser.add_argument('--finish-followed-checkpoint', action='store_true',
                    help='Observe that real user function returning to its caller, without state changes')
parser.add_argument('--read-followed-checkpoint-arg', action='append', default=[], metavar='ARG:HEX_LENGTH',
                    help='Read at most 4096 bytes at an original user checkpoint argument after its real return')
parser.add_argument('--read-followed-relative', action='append', default=[], metavar='SIGNED_OFFSET:HEX_LENGTH',
                    help='Read bounded user data relative to the actual observed user entry')
parser.add_argument('--entry-kernel-bytes-field', help='Select a real call by ARG:HEX_OFFSET:HEX_BYTES in bounded kernel memory')
parser.add_argument('--entry-process-name', help='Select a real primary call by compiler-qualified current process name; bounded read-only observation')
parser.add_argument('--entry-min-pid',type=int,default=0,help='Minimum actual PID for an explicitly named process predicate')
parser.add_argument('--entry-register-equals', help='Select a real call by ARG:HEX_VALUE; observation only')
parser.add_argument('--entry-max-skips', type=int, default=16, help='Bounded real entry selection attempts, at most 65536')
parser.add_argument('--read-entry-region', action='append', default=[], help='Read a bounded direct kernel region ARG:HEX_OFFSET:HEX_LENGTH at entry or after real return')
parser.add_argument('--read-saved-user-register',action='append',default=[],help='At an ABI-qualified saved EL0 state, read bounded user memory through HEX_REGISTER:HEX_LENGTH')
parser.add_argument('--inspect-entry-user-state', action='store_true', help='Read ABI-qualified user saved states from bounded saved registers and compiler-derived current-thread linkage')
parser.add_argument('--read-frame-locals', action='store_true', help='Read at most 192 bytes below each of at most 16 actual kernel frame pointers')
parser.add_argument('--read-frame-indirect', action='append', default=[], help='Read kernel memory through a real saved frame slot FRAME:SIGNED_HEX_OFFSET:HEX_LENGTH')
parser.add_argument('--entry-user-string-field', metavar='ARG:HEX_OFFSET:VALUE', help='Select an actual function call by a bounded user C-string pointer field; skip at most 16 other calls')
parser.add_argument('--read-entry-indirect', action='append', default=[], metavar='ARG:HEX_OFFSET:HEX_LENGTH',
                    help='After function return, read a pointer field from an original x0-x3 argument')
parser.add_argument('--read-entry-user-vector', action='append', default=[], metavar='ARG:HEX_OFFSET:HEX_COUNT', help='Read a bounded user argv vector reached through an actual kernel argument field after return')
parser.add_argument('--read-entry-user-indirect', action='append', default=[], metavar='ARG:HEX_OFFSET:HEX_LENGTH',
                    help='After function return, read an explicitly user-space pointer field from an original argument')
parser.add_argument('--inspect-entry-translations', action='store_true', help='Walk actual 4K TTBR1 tables for captured entry pointers using QEMU physical reads')
parser.add_argument('--allow-unmapped-kernel-pages', action='store_true', help='Diagnostic only: record invalid descriptors in requested kernel spans instead of requiring every page mapped')
parser.add_argument('--inspect-indirect-kernel-range', action='append', default=[], metavar='HEX_POINTER_ADDRESS:HEX_BYTES[:HEX_OFFSET]',
                    help='Walk a bounded kernel extent addressed by an actual captured global pointer')
parser.add_argument('--inspect-kernel-range', action='append', default=[], metavar='HEX_VA:HEX_BYTES',
                    help='Walk every actual hardware page covering an explicit kernel range, at most four 1-MiB ranges')
parser.add_argument('--mutate-md0-after-return', metavar='ENTRY_PC:MD0_VA:IMAGE_BYTES:SHA256:OFFSET',
                    help='Offline input-fault test: XOR one SHA-identified own md0 byte after a successful function return')
parser.add_argument('--mutate-mapped-vnode-page', metavar='ENTRY_PC:FILE_PAGE:BYTE:SHA256',
                    help='Offline fault test: XOR one SHA-identified actual vnode page at cs_validate_hash entry')
parser.add_argument('--mapped-vnode-max-hits', type=int, default=16, help='Maximum real validation entries before a selected page fault, bounded to 8192')
parser.add_argument('--mapped-vnode-cdhash', metavar='FIELD_OFFSET:CDHASH',
                    help='Require this actual cs_blob CodeDirectory identity before page fault injection')
parser.add_argument('--gic-version', choices=['2', '3'], default='2')
parser.add_argument('--smp', type=int, default=1, help='QEMU CPU count (offline secondary-CPU start check)')
parser.add_argument('--boot-progress-clock-fixture', help='Own diagnostic-only clock epoch: ENTRY:EPOCH:FREQUENCY:SECONDS')
args = parser.parse_args()
trace_register_reads = {}
for request in args.trace_read_register_offset:
    try:
        pc, register_index, offset, length = parse_trace_register_memory_request(request)
    except ValueError as error:
        parser.error(str(error))
    if pc not in args.trace_address:
        parser.error('Trace-register reads require the same PC in --trace-address')
    trace_register_reads.setdefault(pc, []).append((request, register_index, offset, length))
progress_clock=None
if args.boot_progress_clock_fixture:
    parts=args.boot_progress_clock_fixture.split(':')
    if len(parts)!=4:parser.error('Invalid diagnostic clock fixture')
    progress_clock=tuple(int(x,16) for x in parts[:3])+(int(parts[3]),)
    if (not args.esp or not args.kernel or args.qemu_accel!='tcg' or
        progress_clock[0] not in args.trace_address or progress_clock[3] not in (31,121) or
        any(not 0xfffffff000000000<=v<0xfffffffffffffff0 for v in progress_clock[:3]) or
        progress_clock[0]%4 or progress_clock[1]%8 or progress_clock[2]%8):
        parser.error('Clock fixture requires identified XNU TCG and its exact diagnostic symbols')
if args.exercise_ufs_allocation and (not args.esp or not args.kernel or not args.system_registers or args.qemu_accel != 'tcg'):
    parser.error('UFS allocation exercise requires an identified full XNU ESP boot, TCG and system registers')
cache_query_plan = None
if args.main_cache_query_plan:
    if args.follow_main_entry_offset is None:
        parser.error('Cache query observation requires actual main entry')
    cache_query_plan = json.loads(args.main_cache_query_plan.read_text())
process_plan = json.loads(args.process_layout_plan.read_text()) if args.process_layout_plan else cache_query_plan
if args.inspect_ui_threads and (not process_plan or not process_plan.get('thread_snapshot_layout') or args.allproc_address is None):
    parser.error('UI thread snapshots require compiler-qualified layouts and the process list')
if process_plan and not args.system_registers:
    parser.error('Actual process inspection requires real system-register observation')
if args.qemu_accel == 'hvf' and (platform.system() != 'Darwin' or platform.machine() != 'arm64' or not args.esp):
    parser.error('HVF requires a macOS ARM64 ESP test')
if args.primary_start_address is not None and (not args.esp or args.primary_start_address in
        [args.address, *args.also_stop, *args.trace_address, *args.syscall_log_address, *args.cstring_log_address,
         args.syscall_start_address, args.cstring_start_address, args.registry_string_log_address]):
    parser.error('Primary activation requires a distinct actual ESP kernel entry')
if args.syscall_start_address is not None and (not args.syscall_log_address or args.syscall_start_address in [args.address, *args.also_stop, *args.syscall_log_address, *args.trace_address] or args.syscall_start_address == args.cstring_start_address):
    parser.error('Delayed syscall logging requires a distinct activation entrypoint and logger')
if args.cstring_start_address is not None and (not args.cstring_log_address or args.cstring_start_address in [args.address, *args.also_stop, *args.cstring_log_address, *args.trace_address]):
    parser.error('Delayed C-string logging requires a distinct activation entrypoint and logger')
if not 0 <= args.skip_target_hits <= 16:
    parser.error('--skip-target-hits must be between 0 and 16')
if not 0 <= args.step_followed_entry <= 64 or (args.step_followed_entry and args.follow_entrypoint_arg is None):
    parser.error('--step-followed-entry requires an entrypoint argument and 0..64 instructions')
if args.follow_entrypoint_arg is not None and (args.finish_function or not args.esp):
    parser.error('Following an actual entrypoint requires an ESP and conflicts with --finish-function')
if args.follow_main_entry_offset is not None and (args.follow_entrypoint_arg is None or args.follow_entrypoint_checkpoint_offset is not None or
        args.step_followed_entry or not 0 < args.follow_main_entry_offset < 16 * 1024 * 1024 or args.follow_main_entry_offset & 3):
    parser.error('Main observation requires a bounded aligned LC_MAIN offset and a followed entry, without another checkpoint or stepping')
if args.follow_entrypoint_checkpoint_offset is not None and args.follow_entrypoint_arg is None:
    parser.error('A relative user checkpoint requires --follow-entrypoint-arg')
if args.finish_followed_checkpoint and args.follow_entrypoint_checkpoint_offset is None:
    parser.error('--finish-followed-checkpoint requires a relative checkpoint')
if args.read_followed_checkpoint_arg and not args.finish_followed_checkpoint:
    parser.error('--read-followed-checkpoint-arg requires --finish-followed-checkpoint')
if args.read_followed_relative and args.follow_entrypoint_arg is None:
    parser.error('--read-followed-relative requires an actual followed entry')
if (args.read_entry_indirect or args.read_entry_user_indirect) and not args.finish_function:
    parser.error('--read-entry-indirect requires --finish-function')
if args.read_entry_user_vector and not args.finish_function:
    parser.error('User argv observation requires a real finished function call')
if args.read_entry_user_indirect and args.inspect_entry_translations:
    parser.error('User pointer capture is separate from the existing TTBR1-only translation walker')
if set(args.read_entry_indirect) & set(args.read_entry_user_indirect):
    parser.error('An entry pointer request must select exactly one address space')
if args.inspect_entry_translations and not args.system_registers:
    parser.error('--inspect-entry-translations requires --system-registers')
kernel_ranges = []
for request in args.inspect_kernel_range:
    va, size = (int(part, 16) for part in request.split(':'))
    if not args.system_registers or len(args.inspect_kernel_range) > 4 or not 0 < size <= 1024 * 1024 or not 0xfffffff000000000 <= va < va + size < 1 << 64:
        parser.error('Kernel range translation inspection requires system registers and at most four bounded kernel spans')
    kernel_ranges.append((request, va, size))
indirect_kernel_ranges = []
for request in args.inspect_indirect_kernel_range:
    parts=request.split(':')
    if len(parts) not in (2,3):parser.error('Indirect kernel range requires pointer, bytes and optional offset')
    address,size=(int(part,16) for part in parts[:2]);offset=int(parts[2],16) if len(parts)==3 else 0
    if (not args.system_registers or len(args.inspect_kernel_range) + len(args.inspect_indirect_kernel_range) > 4 or
            not 0 < size <= 1024 * 1024 or not 0<=offset<=1024*1024*1024 or not 0xfffffff000000000 <= address < address + 8 < 1 << 64):
        parser.error('Indirect kernel ranges require system registers and at most four total bounded kernel spans')
    indirect_kernel_ranges.append((request, address, size,offset))
if not 1 <= args.mapped_vnode_max_hits <= 8192:
    parser.error('Mapped-page validation entry limit must be 1..8192')
entry_kernel_bytes = None
entry_register_equal = None
if args.entry_kernel_bytes_field:
    parts = args.entry_kernel_bytes_field.split(':')
    if len(parts) != 3 or not args.finish_function or not args.esp:
        parser.error('Kernel field selection requires a real finished ESP function call')
    entry_kernel_bytes = (int(parts[0], 16), int(parts[1], 16), bytes.fromhex(parts[2]))
    if not 0 <= entry_kernel_bytes[0] < 8 or not 0 <= entry_kernel_bytes[1] <= 4096 or not 1 <= len(entry_kernel_bytes[2]) <= 128:
        parser.error('Read a bounded field from kernel x0-x7')
if args.entry_register_equals:
    parts = args.entry_register_equals.split(':')
    if len(parts) != 2 or not args.esp or not (args.finish_function or (args.inspect_entry_user_state and args.entry_process_name and process_plan)):
        parser.error('Register selection requires a finished ESP function call or a named real user-state boundary')
    entry_register_equal = (int(parts[0], 16), int(parts[1], 16))
    if not 0 <= entry_register_equal[0] < 8 or not 0 <= entry_register_equal[1] < 1 << 64:
        parser.error('Select an actual 64-bit entry argument')
if args.entry_process_name and (not process_plan or not args.esp or not 1 <= len(args.entry_process_name.encode('ascii')) <= 16):
    parser.error('Process selection requires exact compiled process layout, ESP and a 1..16-byte ASCII process name')
if args.entry_min_pid and (not args.entry_process_name or not 1<=args.entry_min_pid<1<<31):parser.error('Minimum PID requires a named process and a positive bounded PID')
if not 1 <= args.entry_max_skips <= 65536 or (args.entry_max_skips > 16 and not (entry_kernel_bytes or args.entry_process_name)):
    parser.error('Large bounded entry selection requires an explicit kernel-byte identity')
saved_user_requests=[]
for request in args.read_saved_user_register:
    index,length=(int(v,16) for v in request.split(':'))
    if not args.inspect_entry_user_state or len(args.read_saved_user_register)>4 or not 0<=index<=30 or not 0<length<=4096:parser.error('Read at most four saved user x0-x30 regions of at most 4096 bytes with --inspect-entry-user-state')
    saved_user_requests.append((request,index,length))
if args.inspect_entry_user_state and (not args.esp or args.finish_function):
    parser.error('User saved-state candidate capture requires an actual ESP entry, without finishing')
entry_string = None
if args.entry_user_string_field:
    parts = args.entry_user_string_field.split(':', 2)
    if len(parts) != 3 or not args.finish_function or not args.esp:
        parser.error('Entry string selection requires ARG:OFFSET:VALUE and a finished ESP function call')
    entry_string = (int(parts[0], 16), int(parts[1], 16), parts[2].encode('ascii'))
    if not 0 <= entry_string[0] < 4 or not 0 <= entry_string[1] <= 4096 or not 0 < len(entry_string[2]) <= 64 or b'\0' in entry_string[2]:
        parser.error('Select x0-x3, a bounded field and at most 64 ASCII string bytes')
mutation = None
mapped_mutation = None
mapped_cdhash = None
if args.mapped_vnode_cdhash:
    fields = args.mapped_vnode_cdhash.split(':')
    if not args.mutate_mapped_vnode_page or len(fields) != 2 or len(bytes.fromhex(fields[1])) != 20:
        parser.error('CodeDirectory identity requires a mapped-page fault control and a 20-byte hash')
    mapped_cdhash = (int(fields[0], 16), fields[1].lower())
    if not 0 <= mapped_cdhash[0] <= 4096:
        parser.error('Read a bounded source-qualified cs_blob field')
if args.mutate_mapped_vnode_page:
    fields = args.mutate_mapped_vnode_page.split(':')
    if len(fields) != 4 or len(fields[3]) != 64 or not args.esp:
        parser.error('Mapped-page mutation requires an ESP and complete own-page identity')
    mapped_mutation = (int(fields[0], 16), int(fields[1], 16), int(fields[2], 16), fields[3])
    if mapped_mutation[1] % 4096 or not 0 <= mapped_mutation[2] < 4096 or mapped_mutation[0] not in args.trace_address:
        parser.error('Use an aligned file page, a byte within it and an explicit tracepoint')
if args.mutate_md0_after_return:
    fields = args.mutate_md0_after_return.split(':')
    if len(fields) != 5 or len(fields[3]) != 64 or not args.esp:
        parser.error('Mutation requires an ESP fixture and the complete own-input identity')
    mutation = (int(fields[0], 16), int(fields[1], 16), int(fields[2], 16), fields[3], int(fields[4], 16))
    if not 0 <= mutation[4] < mutation[2] <= 32 * 1024**2:
        parser.error('Mutate one byte inside an own fixture of at most 32 MiB')
    if mutation[0] not in args.trace_address:
        parser.error('The mutation trigger must be an explicit one-shot tracepoint')
fixture = args.fixture.resolve()
fixture.mkdir(parents=True, exist_ok=True)
data_capture = None
if args.capture_scratch_data:
    try:
        base_symbol, size_symbol, expected = (int(v, 16) for v in args.capture_scratch_data.split(':'))
    except ValueError:
        parser.error('Scratch Data capture requires two symbol addresses and one byte extent')
    if (not args.esp or not args.kernel or not args.system_registers or
            not 0 < expected <= 256 * 1024**2 or expected & 4095 or
            any(not 0xfffffff000000000 <= value < (1 << 64) - 8 for value in (base_symbol, size_symbol))):
        parser.error('Scratch Data capture requires exact native pointers and a bounded ESP input')
    if (fixture / 'scratch-data-snapshot.apfs').exists():
        parser.error('Preserve the existing scratch Data snapshot before another capture')
    data_capture = base_symbol, size_symbol, expected
block_input = None
if args.missing_virtio_device and not (args.virtio_block_image or args.virtio_udif_image):
    parser.error('--missing-virtio-device requires the explicit offline block input')
if args.virtio_block_image:
    disk = args.virtio_block_image.resolve()
    if not args.esp or not disk.is_file() or disk.stat().st_size < 512 or disk.stat().st_size % 512:
        parser.error('The offline raw disk requires an ESP and an existing sector-aligned regular file')
    with disk.open('rb') as stream:
        first_sector = stream.read(512)
    block_input = dict(name=disk.name, bytes=disk.stat().st_size, format='raw', read_only=True,
                       device_attached=not args.missing_virtio_device,
                       first_sector_sha256=hashlib.sha256(first_sector).hexdigest())
    block_file_driver = 'file'
if args.virtio_udif_image:
    import plistlib
    source = args.virtio_udif_image.resolve()
    if not args.esp or not source.is_file():
        parser.error('UDIF observation requires an ESP and the actual existing image file')
    with source.open('rb') as stream:
        stream.seek(-512, 2)
        if stream.read(4) != b'koly':
            parser.error('Expected a real UDIF trailer')
    images = plistlib.loads(subprocess.check_output(['hdiutil', 'info', '-plist']))['images']
    matches = [i for i in images if Path(i['image-path']).resolve() == source and i.get('writeable') is False]
    if len(matches) != 1:
        parser.error('The exact UDIF source must already have one read-only image attachment')
    whole = [e['dev-entry'] for e in matches[0]['system-entities']
             if e.get('content-hint') == 'GUID_partition_scheme']
    if len(whole) != 1:
        parser.error('Require a unique actual whole-disk image mapping')
    info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', whole[0]]))
    if (info.get('BusProtocol') != 'Disk Image' or info.get('Writable') is not False or
            info.get('DeviceNode') != whole[0] or not info.get('TotalSize') or info['TotalSize'] % 512):
        parser.error('Only actual read-only Disk Image virtual media is accepted')
    disk = Path('/dev/r' + Path(whole[0]).name)
    if not stat.S_ISCHR(disk.stat().st_mode):
        parser.error('Expected the read-only image raw character device')
    with disk.open('rb', buffering=0) as stream:
        sector = stream.read(512)
    if len(sector) != 512:
        parser.error('Cannot read the real virtual-media sector')
    digest = hashlib.sha256()
    with source.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    block_input = dict(name=source.name, bytes=info['TotalSize'], format='raw', read_only=True,
                       device_attached=not args.missing_virtio_device,
                       source_format='UDIF', source_bytes=source.stat().st_size,
                       source_sha256=digest.hexdigest(), host_bus_protocol='Disk Image',
                       host_media_writable=False, first_sector_sha256=hashlib.sha256(sector).hexdigest())
    block_file_driver = 'host_device'
extra_block_input = None
if args.virtio_extra_block_image:
    extra_disk = args.virtio_extra_block_image.resolve()
    if (not block_input or args.missing_virtio_device or not extra_disk.is_file() or
            extra_disk.stat().st_size < 512 or extra_disk.stat().st_size % 512 or extra_disk == disk):
        parser.error('The second read-only raw file requires a distinct real first disk and attached devices')
    with extra_disk.open('rb') as stream:
        extra_sector = stream.read(512)
    extra_block_input = dict(name=extra_disk.name, bytes=extra_disk.stat().st_size,
                            format='raw', read_only=True, device_attached=True,
                            first_sector_sha256=hashlib.sha256(extra_sector).hexdigest())
if args.esp:
    if not args.kernel:
        parser.error('--esp requires --kernel for provenance')
    state = {'sha256': hashlib.sha256(args.kernel.read_bytes()).hexdigest()}
else:
    state = json.loads((fixture / 'cortex-a710.json').read_text())
live_hid=None
if args.live_hid_dir:
    if not args.esp or not args.kernel:parser.error('Live HID requires the own source kernel ESP experiment')
    import importlib.util
    spec=importlib.util.spec_from_file_location('live_hid',Path(__file__).with_name('live-hid.py'))
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    live_hid=module.LiveHid(args.kernel,args.live_hid_dir)
with socket.socket() as reservation:
    reservation.bind(('127.0.0.1', 0))
    port = reservation.getsockname()[1]
qemu_memory_mib = args.qemu_memory_mib or (1024 if args.esp else 512)
command = ['qemu-system-aarch64', '-M', f'virt,virtualization=off,secure=off,gic-version={args.gic_version}',
           '-accel', args.qemu_accel, '-cpu', 'cortex-a710' if args.qemu_accel == 'tcg' else 'host', '-m', str(qemu_memory_mib), '-smp', str(args.smp),
           '-display', 'none', '-monitor', 'none', '-nic', 'none', '-no-reboot',
           '-serial', f'file:{fixture}/gdb-uart.log',
           '-gdb', f'tcp:127.0.0.1:{port}', '-S']
if args.qemu_ramfb:
    if not args.esp:
        parser.error('Actual ramfb qualification requires UEFI ESP entry')
    command += ['-device', 'ramfb']
if block_input:
    command += ['-global', 'virtio-mmio.force-legacy=false', '-blockdev', json.dumps({
        'driver': 'raw', 'node-name': 'offline-block', 'read-only': True,
        'file': {'driver': block_file_driver, 'filename': str(disk), 'read-only': True, 'locking': 'off'}})]
    if not args.missing_virtio_device:
        command += ['-device', 'virtio-blk-device,drive=offline-block']
if extra_block_input:
    command += ['-blockdev', json.dumps({
        'driver': 'raw', 'node-name': 'offline-extra', 'read-only': True,
        'file': {'driver': 'file', 'filename': str(extra_disk), 'read-only': True, 'locking': 'off'}}),
        '-device', 'virtio-blk-device,drive=offline-extra']
if args.virtio_console_socket:
    console_path = args.virtio_console_socket.resolve()
    if not args.esp or not stat.S_ISSOCK(console_path.stat().st_mode) or ',' in str(console_path):
        parser.error('Offline console requires a real Unix socket, ESP boot and a comma-free path')
    command += ['-global', 'virtio-mmio.force-legacy=false',
                '-chardev', f'socket,id=offline-console,path={console_path},server=off',
                '-device', 'virtio-serial-device,id=offline-serial',
                '-device', 'virtconsole,chardev=offline-console,name=offline-transport']
firmware_info = None
if args.esp:
    qemu = shutil.which('qemu-system-aarch64')
    if not qemu:
        parser.error('QEMU not found')
    share = Path(qemu).resolve().parent.parent / 'share/qemu'
    code = args.firmware or share / 'edk2-aarch64-code.fd'
    if not code.is_file():
        parser.error('UEFI code FD not found; use --firmware')
    variables = code.with_name('edk2-arm-vars.fd')
    firmware_info = dict(code_file=code.name, code_bytes=code.stat().st_size,
                         code_sha256=hashlib.sha256(code.read_bytes()).hexdigest(),
                         variables_file=variables.name if variables.is_file() else None,
                         variables_sha256=(hashlib.sha256(variables.read_bytes()).hexdigest()
                                           if variables.is_file() else None))
    command += ['-drive', f'if=pflash,format=raw,readonly=on,file={code}']
    if variables.is_file():
        shutil.copyfile(variables, fixture / 'vars.fd')
        command += ['-drive', f'if=pflash,format=raw,file={fixture / "vars.fd"}']
    command += ['-drive', f'if=none,id=esp,format=raw,readonly=on,file=fat:{args.esp.resolve()}',
                '-device', 'virtio-blk-pci,drive=esp']
else:
    command += ['-kernel', str(fixture / 'entry.bin'),
                '-device', f'loader,file={fixture}/kernel-flat.bin,addr={state["physical_base"]},force-raw=on',
                '-device', f'loader,file={fixture}/boot-args.bin,addr={state["boot_args_pa"]},force-raw=on']
if args.capture_screen:
    if not args.qemu_ramfb:parser.error('Actual screen capture requires the explicit QEMU ramfb console')
    screen_directory=tempfile.TemporaryDirectory(prefix='sheng-qmp-',dir='/tmp')
    screen_socket=Path(screen_directory.name)/'qmp.sock'
    command+=['-qmp',f'unix:{screen_socket.resolve()},server=on,wait=off']
for name in ('gdb-stop.json', 'gdb-failure.json'):
    (fixture / name).unlink(missing_ok=True)
with (fixture / 'gdb-stderr.log').open('w') as errors:
    process = subprocess.Popen(command, stdout=errors, stderr=errors)
    try:
        deadline = time.monotonic() + 2
        while True:
            try:
                connection = socket.create_connection(('127.0.0.1', port), timeout=args.timeout)
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                break
            except ConnectionRefusedError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.01)

        with connection:
            from gdb_packets import Packets
            packet = Packets(connection).request

            def feature(name):
                offset = 0
                text = ''
                while True:
                    chunk = packet(f'qXfer:features:read:{name}:{offset:x},1000')
                    if not chunk or chunk[0] not in 'ml':
                        raise RuntimeError(f'QEMU did not provide {name}')
                    text += chunk[1:]
                    offset += len(chunk) - 1
                    if chunk[0] == 'l':
                        # QEMU's target.xml uses xi:include without declaring xi.
                        if 'xi:include' in text and 'xmlns:xi=' not in text:
                            text = text.replace('<target', '<target xmlns:xi="http://www.w3.org/2001/XInclude"', 1)
                        return ET.fromstring(text)

            process_regnum = None
            if process_plan:
                target_description = feature('target.xml')
                for include in target_description.findall('{http://www.w3.org/2001/XInclude}include'):
                    for register in feature(include.attrib['href']).findall('reg'):
                        if register.attrib['name'] == 'TPIDR_EL1':
                            process_regnum = int(register.attrib['regnum'])
                if process_regnum is None:
                    raise RuntimeError('Actual process observation requires the active TPIDR_EL1 register')

            def process_identity():
                layout = process_plan['process_layout']

                def kernel_pointer(address):
                    if not 0xfffffff000000000 <= address < (1 << 64) - 8:
                        raise RuntimeError('Expected an actual kernel thread/process pointer')
                    raw = packet(f'm{address:x},8')
                    if len(raw) != 16 or raw.startswith('E'):
                        raise RuntimeError('Cannot read actual kernel thread/process linkage')
                    return int.from_bytes(bytes.fromhex(raw), 'little')

                thread = int.from_bytes(bytes.fromhex(packet(f'p{process_regnum:x}')), 'little')
                ro = kernel_pointer(thread + layout['thread_ro'])
                if kernel_pointer(ro + layout['ro_owner']) != thread:
                    raise RuntimeError('Actual read-only thread owner must match the active thread')
                proc = kernel_pointer(ro + layout['ro_proc'])
                task = kernel_pointer(ro + layout['ro_task'])
                region = kernel_pointer(task + layout['task_region'])
                slide = packet(f'm{region + layout["region_slide"]:x},4') if region else None
                pid = packet(f'm{proc + layout["pid"]:x},4')
                name = packet(f'm{proc + layout["comm"]:x},11')
                if len(pid) != 8 or len(name) != 34 or pid.startswith('E') or name.startswith('E'):
                    raise RuntimeError('Cannot read actual process identity')
                return dict(thread=hex(thread), thread_ro=hex(ro), proc=hex(proc),
                    user_state=hex(kernel_pointer(thread+layout['thread_user_state'])) if 'thread_user_state' in layout else None,
                    task=hex(task), shared_region=hex(region),
                    shared_region_slide=int.from_bytes(bytes.fromhex(slide), 'little') if slide and not slide.startswith('E') else None,
                    pid=int.from_bytes(bytes.fromhex(pid), 'little'), name=bytes.fromhex(name).split(b'\0')[0].decode())

            for address in dict.fromkeys([args.address if args.primary_start_address is None else args.primary_start_address, *args.also_stop, *args.trace_address,
                                          *([args.kext_log_address] if args.kext_log_address else []),
                                          *(args.syscall_log_address if args.syscall_start_address is None else [args.syscall_start_address]),
                                          *(args.cstring_log_address if args.cstring_start_address is None else [args.cstring_start_address]),
                                          *([args.registry_string_log_address] if args.registry_string_log_address else []),
                                          *([live_hid.address] if live_hid else [])]):
                if packet(f'Z1,{address:x},4') != 'OK':
                    raise RuntimeError('QEMU rejected breakpoint')
            interrupted = False
            trace_events = []
            progress_clock_event = None
            kext_log_events = []
            kext_log_hits = 0
            mutation_return = None
            mutation_event = None
            mapped_mutation_event = None
            mapped_mutation_hits = 0
            skipped_target_hits = 0
            entry_string_events = []
            entry_process_events = []
            entry_kernel_bytes_match = None
            trace_remaining = set(args.trace_address) - {args.address, *args.also_stop}
            started = time.monotonic()
            deadline = started + args.timeout
            syscall_events = []
            syscall_start_event = None
            primary_start_event = None
            cstring_events = []
            cstring_start_event = None
            registry_string_events = []

            def observe_mapped_fault(current):
                global mapped_mutation_event, mapped_mutation_hits
                if mapped_mutation is None or mapped_mutation_event is not None or current[32] != mapped_mutation[0]:
                    return False
                mapped_mutation_hits += 1
                if mapped_mutation_hits > args.mapped_vnode_max_hits:
                    raise RuntimeError('Selected vnode page was not found within the bounded validation entries')
                matched_page = False
                if current[1] and current[2] == mapped_mutation[1]:
                    raw = packet(f'm{current[3]:x},1000')
                    if len(raw) != 8192 or raw.startswith('E'):
                        raise RuntimeError('Cannot inspect the actual validation input page')
                    matched_page = hashlib.sha256(bytes.fromhex(raw)).hexdigest() == mapped_mutation[3]
                    if matched_page and mapped_cdhash:
                        observed_cdhash = packet(f'm{current[0] + mapped_cdhash[0]:x},14')
                        if observed_cdhash.startswith('E'):
                            raise RuntimeError('Cannot inspect actual cs_blob CodeDirectory identity')
                        matched_page = observed_cdhash == mapped_cdhash[1]
                if matched_page:
                    original = bytes.fromhex(raw)[mapped_mutation[2]]
                    target = current[3] + mapped_mutation[2]
                    if packet(f'M{target:x},1:{original ^ 1:02x}') != 'OK' or packet(f'm{target:x},1') != f'{original ^ 1:02x}':
                        raise RuntimeError('Own mapped-page byte mutation failed')
                    mapped_mutation_event = {'entry': hex(current[32]), 'pager': hex(current[1]),
                        'file_page': current[2], 'data_kva': hex(current[3]), 'byte_in_page': mapped_mutation[2],
                        'page_sha256_before': mapped_mutation[3], 'before': original, 'after': original ^ 1,
                        'registers_or_validation_state_modified': False}
                    if mapped_cdhash:
                        mapped_mutation_event['cs_blob'] = hex(current[0])
                        mapped_mutation_event['code_directory_hash'] = observed_cdhash
                        mapped_mutation_event['compiled_cdhash_field_offset'] = mapped_cdhash[0]
                else:
                    if packet(f'z1,{current[32]:x},4') != 'OK':
                        raise RuntimeError('Cannot step over a non-target page validation')
                    packet('s')
                    if packet(f'Z1,{current[32]:x},4') != 'OK':
                        raise RuntimeError('Cannot restore mapped-page validation tracepoint')
                    return True
                trace_events.append({'pc': hex(current[32]), 'x0': hex(current[0]),
                                     'x1': hex(current[1]), 'lr': hex(current[30]),
                                     'elapsed_seconds': round(time.monotonic() - started, 3)})
                if packet(f'z1,{current[32]:x},4') != 'OK':
                    raise RuntimeError('Cannot retire the applied mapped-page fault breakpoint')
                trace_remaining.discard(current[32])
                return True

            def continue_observing():
                global cstring_start_event, syscall_start_event, primary_start_event
                while True:
                    connection.settimeout(max(0.001, deadline - time.monotonic()))
                    timed_out = False
                    try:
                        stopped = packet('c')
                    except TimeoutError:
                        stopped = packet(None)
                        timed_out = True
                    current = struct.unpack_from('<33Q', bytes.fromhex(packet('g')))
                    if not timed_out and live_hid and current[32]==live_hid.address:
                        connection.settimeout(3)
                        if live_hid.checkpoint(packet,time.monotonic()-started):return stopped,current,False
                        if packet(f'z1,{current[32]:x},4')!='OK':raise RuntimeError('Cannot step own HID timer checkpoint')
                        packet('s')
                        if struct.unpack_from('<33Q',bytes.fromhex(packet('g')))[32]==current[32]:
                            raise RuntimeError('Own HID checkpoint instruction did not advance')
                        if packet(f'Z1,{current[32]:x},4')!='OK':raise RuntimeError('Cannot restore own HID timer checkpoint')
                        continue
                    if not timed_out and args.primary_start_address is not None and current[32] == args.primary_start_address:
                        primary_start_event = dict(pc=hex(current[32]), lr=hex(current[30]),
                                                   elapsed_seconds=round(time.monotonic() - started, 3))
                        if packet(f'z1,{current[32]:x},4') != 'OK' or packet(f'Z1,{args.address:x},4') != 'OK':
                            raise RuntimeError('Cannot activate the actual primary entry observation')
                        continue
                    if not timed_out and observe_mapped_fault(current):
                        continue
                    if not timed_out and args.syscall_start_address is not None and current[32] == args.syscall_start_address:
                        syscall_start_event = {'pc': hex(current[32]), 'lr': hex(current[30]),
                            'elapsed_seconds': round(time.monotonic() - started, 3)}
                        if packet(f'z1,{current[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot retire syscall activation breakpoint')
                        for address in set(args.syscall_log_address):
                            if packet(f'Z1,{address:x},4') != 'OK':
                                raise RuntimeError('Cannot arm delayed syscall observation')
                        continue
                    if not timed_out and args.cstring_start_address is not None and current[32] == args.cstring_start_address:
                        cstring_start_event = {'pc': hex(current[32]), 'lr': hex(current[30]),
                            'elapsed_seconds': round(time.monotonic() - started, 3)}
                        if packet(f'z1,{current[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot retire kernel-string activation breakpoint')
                        for address in set(args.cstring_log_address):
                            if packet(f'Z1,{address:x},4') != 'OK':
                                raise RuntimeError('Cannot arm delayed kernel-string observation')
                        continue
                    if not timed_out and args.registry_string_log_address and current[32] == args.registry_string_log_address:
                        event = {'entry': hex(current[0]), 'caller': hex(current[30]),
                                 'elapsed_seconds': round(time.monotonic() - started, 3)}
                        for index, name in ((1, 'key'), (2, 'value')):
                            if current[index] < 0xfffffff000000000:
                                raise RuntimeError('Expected actual kernel registry string arguments')
                            raw = packet(f'm{current[index]:x},{min(256, 4096 - (current[index] & 4095)):x}')
                            if raw.startswith('E'):
                                raise RuntimeError('Cannot read actual registry property argument')
                            event[name] = bytes.fromhex(raw).split(b'\0', 1)[0].decode(errors='replace')
                        registry_string_events.append(event)
                        if packet(f'z1,{current[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot remove registry string observation breakpoint')
                        packet('s')
                        if len(registry_string_events) < 128 and packet(f'Z1,{current[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot restore registry string observation breakpoint')
                        continue
                    if not timed_out and current[32] in args.cstring_log_address:
                        if current[0] < 0xfffffff000000000:
                            raise RuntimeError('Expected a kernel C-string call argument')
                        raw = packet(f'm{current[0]:x},100')
                        if raw.startswith('E'):
                            raise RuntimeError('Cannot read the observed kernel string')
                        cstring_events.append({'handler': hex(current[32]), 'caller': hex(current[30]),
                            'argument': hex(current[0]), 'stack_bytes': packet(f'm{current[31]:x},80'), 'value': bytes.fromhex(raw).split(b'\0', 1)[0].decode(errors='replace'),
                            'elapsed_seconds': round(time.monotonic() - started, 3)})
                        if packet(f'z1,{current[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot remove kernel string observation breakpoint')
                        packet('s')
                        if len(cstring_events) < 128 and packet(f'Z1,{current[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot restore kernel string observation breakpoint')
                        if len(cstring_events) == 128:
                            for address in set(args.cstring_log_address) - {current[32]}:
                                if packet(f'z1,{address:x},4') != 'OK':
                                    raise RuntimeError('Cannot retire exhausted string observation breakpoint')
                        continue
                    if timed_out or current[32] not in args.syscall_log_address:
                        return stopped, current, timed_out
                    raw = packet(f'm{current[0]:x},128')
                    if len(raw) != 592 or raw.startswith('E'):
                        raise RuntimeError('Cannot read actual ARM64 syscall saved state')
                    state = bytes.fromhex(raw)
                    if struct.unpack_from('<2I', state) != (21, 72):
                        raise RuntimeError('Unexpected saved-state ABI for pinned Darwin 23 ARM64')
                    regs = struct.unpack_from('<33Q', state, 8)
                    event = {'number': struct.unpack_from('<i', state, 136)[0], 'handler': hex(current[32]),
                        'user_pc': hex(regs[32]), 'arguments': [hex(v) for v in regs[:8]],
                        'user_lr': hex(regs[30]), 'user_fp': hex(regs[29]),
                        'elapsed_seconds': round(time.monotonic() - started, 3)}
                    if event['number'] in (-31, -47) and 0 < regs[0] < 0xfffffff000000000:
                        event['message_bytes'] = packet(f'm{regs[0]:x},80')
                    if event['number'] == 202 and 0 < regs[1] <= 16:
                        event['sysctl_mib_bytes'] = packet(f'm{regs[0]:x},{regs[1] * 4:x}')
                        if regs[4] and 0 < regs[5] <= 256:
                            event['sysctl_new_bytes'] = packet(f'm{regs[4]:x},{regs[5]:x}')
                    if event['number'] in (4, 397) and regs[2] > 0:
                        event['write_bytes'] = packet(f'm{regs[1]:x},{min(256, regs[2]):x}')
                        event['write_truncated'] = regs[2] > 256
                    if event['number'] in (5, 10, 21, 57, 128, 136, 242, 345, 398) and 0 < regs[0] < 0xfffffff000000000:
                        count = min(256, 4096 - (regs[0] & 4095))
                        raw = packet(f'm{regs[0]:x},{count:x}')
                        event['path_bytes'] = raw
                        if not raw.startswith('E'):
                            event['path'] = bytes.fromhex(raw).split(b'\0', 1)[0].decode(errors='replace')
                    if event['number'] in (57, 128) and 0 < regs[1] < 0xfffffff000000000:
                        count = min(256, 4096 - (regs[1] & 4095))
                        raw = packet(f'm{regs[1]:x},{count:x}')
                        event['second_path_bytes'] = raw
                        if not raw.startswith('E'):
                            event['second_path'] = bytes.fromhex(raw).split(b'\0', 1)[0].decode(errors='replace')
                    if not args.syscall_number or event['number'] in args.syscall_number:
                        syscall_events.append(event)
                    if packet(f'z1,{current[32]:x},4') != 'OK':
                        raise RuntimeError('Cannot remove syscall observation breakpoint')
                    packet('s')
                    if len(syscall_events) < 512 and packet(f'Z1,{current[32]:x},4') != 'OK':
                        raise RuntimeError('Cannot restore syscall observation breakpoint')
                    if len(syscall_events) == 512:
                        for address in set(args.syscall_log_address) - {current[32]}:
                            if packet(f'z1,{address:x},4') != 'OK':
                                raise RuntimeError('Cannot retire exhausted syscall observation breakpoint')

            def observe_tracepoint(current):
                global progress_clock_event, mutation_return, mutation_event
                address = current[32]
                if address not in trace_remaining:
                    return False
                event = {'pc': hex(address), 'x0': hex(current[0]),
                         'x1': hex(current[1]), 'x8': hex(current[8]),
                         'lr': hex(current[30]),
                         'elapsed_seconds': round(time.monotonic() - started, 3)}
                reads = {}
                for request, register_index, offset, length in trace_register_reads.get(address, []):
                    base = current[register_index]
                    result = {'register': f'x{register_index}', 'base': hex(base),
                              'offset': hex(offset), 'length': length}
                    target = base + offset
                    if not base:
                        result['unavailable'] = 'trace register is null'
                    elif not 0xfffffff000000000 <= base < 1 << 64:
                        result['unavailable'] = 'trace register is not a kernel pointer'
                    elif target >= 1 << 64 or target + length > 1 << 64:
                        result['unavailable'] = 'trace register target overflows 64 bits'
                    else:
                        raw = packet(f'm{target:x},{length:x}')
                        if raw.startswith('E') or len(raw) != length * 2:
                            result['unavailable'] = 'GDB could not read the bounded trace target'
                        else:
                            result.update(address=hex(target), raw_hex=raw)
                    reads[request] = result
                if reads:
                    event['register_memory_reads'] = reads
                trace_events.append(event)
                if progress_clock and address == progress_clock[0]:
                    epoch_raw = packet(f'm{progress_clock[1]:x},8')
                    freq_raw = packet(f'm{progress_clock[2]:x},8')
                    if len(epoch_raw) != 16 or len(freq_raw) != 16 or epoch_raw.startswith('E') or freq_raw.startswith('E'):
                        raise RuntimeError('Cannot read initialized diagnostic clock')
                    before = int.from_bytes(bytes.fromhex(epoch_raw), 'little')
                    frequency = int.from_bytes(bytes.fromhex(freq_raw), 'little')
                    if not 1000 <= frequency <= 10**10:
                        raise RuntimeError('Invalid actual counter frequency')
                    after = (before - progress_clock[3] * frequency) & ((1 << 64) - 1)
                    raw = after.to_bytes(8, 'little').hex()
                    if packet(f'M{progress_clock[1]:x},8:{raw}') != 'OK' or packet(f'm{progress_clock[1]:x},8') != raw:
                        raise RuntimeError('Cannot qualify diagnostic-only epoch write/readback')
                    progress_clock_event = dict(advance_seconds=progress_clock[3], frequency=frequency,
                        epoch_before=before, epoch_after=after, entry=hex(address),
                        changed_only_own_diagnostic_epoch=True, hardware_or_os_clock_modified=False)
                if mutation is not None and address == mutation[0]:
                    mutation_return = current[30]
                    if mutation_return in {args.address, *args.also_stop, *trace_remaining}:
                        raise RuntimeError('Mutation return must be distinct from existing breakpoints')
                    if packet(f'Z1,{mutation_return:x},4') != 'OK':
                        raise RuntimeError('Cannot observe authentication return')
                if packet(f'z1,{address:x},4') != 'OK':
                    raise RuntimeError('Cannot remove one-shot tracepoint')
                trace_remaining.remove(address)
                return True

            while True:
                stop, values, interrupted = continue_observing()
                connection.settimeout(3)
                entry_string_match = True
                if not interrupted and values[32] == args.address and entry_string is not None:
                    index, offset, expected = entry_string
                    raw = packet(f'm{values[index] + offset:x},8')
                    if len(raw) != 16 or raw.startswith('E'):
                        raise RuntimeError('Cannot read actual entry string pointer field')
                    pointer = int.from_bytes(bytes.fromhex(raw), 'little')
                    if not 0 < pointer < 0xfffffff000000000 - len(expected) - 1:
                        raise RuntimeError('Expected a bounded non-null user C-string pointer')
                    raw = packet(f'm{pointer:x},{len(expected) + 1:x}')
                    if len(raw) != 2 * (len(expected) + 1) or raw.startswith('E'):
                        raise RuntimeError('Cannot read actual user entry string')
                    entry_string_match = bytes.fromhex(raw) == expected + b'\0'
                    entry_string_events.append({'pc': hex(values[32]), 'pointer': hex(pointer),
                        'value': bytes.fromhex(raw).split(b'\0')[0].decode(errors='replace'),
                        'matched': entry_string_match, 'elapsed_seconds': round(time.monotonic() - started, 3)})
                process_match = True
                if not interrupted and values[32] == args.address and args.entry_process_name:
                    identity=process_identity();process_match=identity['name']==args.entry_process_name and identity['pid']>=args.entry_min_pid
                    entry_process_events.append(dict(pc=hex(values[32]),process=identity,matched=process_match,
                                                     elapsed_seconds=round(time.monotonic()-started,3)))
                kernel_match = register_match = True
                if not interrupted and values[32] == args.address and entry_kernel_bytes:
                    index, offset, expected = entry_kernel_bytes
                    pointer = values[index] + offset
                    kernel_match = 0xfffffff000000000 <= pointer <= (1 << 64) - len(expected)
                    raw = packet(f'm{pointer:x},{len(expected):x}') if kernel_match else ''
                    kernel_match = raw == expected.hex()
                if not interrupted and values[32] == args.address and entry_register_equal:
                    index, expected_value = entry_register_equal
                    register_match = values[index] == expected_value
                if not interrupted and values[32] == args.address and entry_kernel_bytes and kernel_match and register_match:
                    entry_kernel_bytes_match = dict(pc=hex(values[32]), address=hex(pointer), bytes=raw,
                                                    elapsed_seconds=round(time.monotonic() - started, 3))
                if not interrupted and values[32] == args.address and (skipped_target_hits < args.skip_target_hits or not entry_string_match or not kernel_match or not register_match or not process_match):
                    skipped_target_hits += 1
                    if skipped_target_hits > args.entry_max_skips:
                        raise RuntimeError('Entry predicate did not match within the bounded skipped calls')
                    if packet(f'z1,{args.address:x},4') != 'OK':
                        raise RuntimeError('Cannot step over an earlier primary breakpoint call')
                    packet('s')
                    if packet(f'Z1,{args.address:x},4') != 'OK':
                        raise RuntimeError('Cannot restore primary breakpoint')
                    continue
                if not interrupted and mutation_return is not None and values[32] == mutation_return:
                    if values[0] != 0 or packet(f'z1,{mutation_return:x},4') != 'OK':
                        raise RuntimeError('Own-input mutation requires successful authentication return')
                    raw = packet(f'm{mutation[1]:x},10')
                    if len(raw) != 32 or raw.startswith('E'):
                        raise RuntimeError('Cannot inspect actual md0 state')
                    pages, count, flags = struct.unpack('<QII', bytes.fromhex(raw))
                    if not flags & 1 or mutation[2] > count * 4096:
                        raise RuntimeError(f'Expected initialized md0 containing the own input: pages={pages:#x}, size={count:#x}, flags={flags:#x}')
                    base = pages * 4096
                    old_mode = packet('qqemu.PhyMemMode')
                    input_mode = '1' if flags & 4 else '0'
                    if old_mode not in ('0', '1') or packet('Qqemu.PhyMemMode:' + input_mode) != 'OK':
                        raise RuntimeError('Own-input memory inspection unavailable')
                    try:
                        digest = hashlib.sha256()
                        original = None
                        for offset in range(0, mutation[2], 4096):
                            size = min(4096, mutation[2] - offset)
                            raw = packet(f'm{base + offset:x},{size:x}')
                            if len(raw) != size * 2 or raw.startswith('E'):
                                raise RuntimeError('Cannot read the complete own md0 image')
                            chunk = bytes.fromhex(raw)
                            digest.update(chunk)
                            if offset <= mutation[4] < offset + size:
                                original = chunk[mutation[4] - offset]
                        if digest.hexdigest() != mutation[3] or original is None:
                            raise RuntimeError('Actual md0 bytes do not match the explicit own-input SHA-256')
                        target = base + mutation[4]
                        if packet(f'M{target:x},1:{original ^ 1:02x}') != 'OK' or packet(f'm{target:x},1') != f'{original ^ 1:02x}':
                            raise RuntimeError('Own-input byte mutation failed')
                        mutation_event = {'trigger': hex(mutation[0]), 'return_pc': hex(mutation_return),
                            'md0_va': hex(mutation[1]), 'input_address': hex(base), 'input_bytes': mutation[2],
                            'address_space': 'guest_physical' if input_mode == '1' else 'guest_virtual',
                            'input_sha256_before': digest.hexdigest(), 'byte_offset': mutation[4],
                            'before': original, 'after': original ^ 1, 'md0_flags': flags,
                            'registers_or_validation_state_modified': False}
                    finally:
                        if packet('Qqemu.PhyMemMode:' + old_mode) != 'OK':
                            raise RuntimeError('Cannot restore debugger virtual-memory mode')
                    mutation_return = None
                    continue
                if not interrupted and args.kext_log_address and values[32] == args.kext_log_address:
                    kext_log_hits += 1
                    if values[1] & 7 in (1, 2):
                        raw_format = packet(f'm{values[2]:x},200')
                        raw_stack = packet(f'm{values[31]:x},40')
                        event = {'pc': hex(values[32]), 'level': values[1] & 7,
                                 'format_bytes': raw_format, 'stack_bytes': raw_stack}
                        if not raw_format.startswith('E'):
                            event['format'] = bytes.fromhex(raw_format).split(b'\0', 1)[0].decode(errors='replace')
                        event['argument_strings'] = {}
                        if not raw_stack.startswith('E'):
                            for index, pointer in enumerate(struct.unpack_from('<4Q', bytes.fromhex(raw_stack))):
                                if pointer >= 0xfffffff000000000:
                                    raw = packet(f'm{pointer:x},100')
                                    if not raw.startswith('E'):
                                        event['argument_strings'][str(index)] = bytes.fromhex(raw).split(b'\0', 1)[0].decode(errors='replace')
                        kext_log_events.append(event)
                    if packet(f'z1,{values[32]:x},4') != 'OK':
                        raise RuntimeError('Cannot remove repeated logpoint')
                    if kext_log_hits < 256:
                        packet('s')
                        if packet(f'Z1,{values[32]:x},4') != 'OK':
                            raise RuntimeError('Cannot restore repeated logpoint')
                    continue
                if interrupted or not observe_tracepoint(values):
                    break
            connection.settimeout(3)
            followed_entry = None
            followed_main = None
            followed_display = None
            if args.follow_entrypoint_arg is not None and values[32] == args.address:
                target = values[args.follow_entrypoint_arg]
                if not 0 < target < 0xfffffff000000000 or target & 3:
                    raise RuntimeError('Expected an actual aligned user entrypoint')
                followed_entry = {'kernel_entry': hex(values[32]), 'argument': args.follow_entrypoint_arg,
                                  'user_entry': hex(target), 'observed': False, 'steps': []}
                if packet(f'z1,{args.address:x},4') != 'OK' or packet(f'Z1,{target:x},4') != 'OK':
                    raise RuntimeError('Cannot observe actual dynamic user entrypoint')
                stop, values, interrupted = continue_observing()
                connection.settimeout(3)
                if not interrupted and values[32] == target:
                    followed_entry['observed'] = True
                    followed_entry['pstate'] = hex(struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0])
                    followed_entry['entry_bytes'] = packet(f'm{target:x},10')
                    if packet(f'z1,{target:x},4') != 'OK':
                        raise RuntimeError('Cannot step the actual entrypoint')
                    checkpoint_ready = True
                    if args.follow_main_entry_offset is not None:
                        raw_stack = packet(f'm{values[31]:x},10')
                        if len(raw_stack) != 32 or raw_stack.startswith('E'):
                            raise RuntimeError('Cannot read actual dyld startup Mach-header/argc stack')
                        main_header, startup_argc = struct.unpack('<2Q', bytes.fromhex(raw_stack))
                        main_target = main_header + args.follow_main_entry_offset
                        if not 0 < main_header < main_target < 0xfffffff000000000 - 4 or main_header & 4095:
                            raise RuntimeError('Expected a real native-page-aligned user Mach header and bounded main entry')
                        followed_main = {'startup_stack': hex(values[31]), 'startup_stack_bytes': raw_stack,
                            'mach_header': hex(main_header), 'argc_on_startup_stack': startup_argc,
                            'entry_offset': args.follow_main_entry_offset, 'target': hex(main_target),
                            'observed': False, 'first_instruction_completed': False}
                        if packet(f'Z1,{main_target:x},4') != 'OK':
                            raise RuntimeError('Cannot observe actual original main entry')
                        stop, values, interrupted = continue_observing()
                        connection.settimeout(3)
                        if not interrupted and values[32] == main_target:
                            followed_main.update(observed=True, pc=hex(values[32]),
                                pstate=hex(struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0]),
                                arguments=[hex(value) for value in values[:4]],
                                entry_bytes=packet(f'm{main_target:x},10'))
                            if packet(f'z1,{main_target:x},4') != 'OK' or packet(f'Z1,{main_target + 4:x},4') != 'OK':
                                raise RuntimeError('Cannot observe completion of the first actual main instruction')
                            stop, values, interrupted = continue_observing()
                            connection.settimeout(3)
                            followed_main.update(first_instruction_completed=not interrupted and values[32] == main_target + 4,
                                checkpoint_pc=hex(values[32]), checkpoint_pstate=hex(struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0]),
                                entry_bytes_after_fault=packet(f'm{main_target:x},10'),
                                mach_header_bytes=packet(f'm{main_header:x},20'))
                            if packet(f'z1,{main_target + 4:x},4') != 'OK':
                                raise RuntimeError('Cannot retire actual main instruction checkpoint')
                        if cache_query_plan and followed_main['first_instruction_completed']:
                            plan = cache_query_plan
                            binding = main_header + plan['main_import_offset']
                            raw = packet(f'm{binding:x},8')
                            if len(raw) != 16 or raw.startswith('E'):
                                raise RuntimeError('Cannot read the actual original main GOT binding')
                            imported = int.from_bytes(bytes.fromhex(raw), 'little')
                            slide = imported - plan['unslid_import']
                            if not 0 <= slide <= plan['max_slide'] or slide & 4095:
                                raise RuntimeError('Actual bound cache address has an invalid native-page slide')
                            query = plan['query'] + slide
                            followed_display = dict(observed=False, main_binding=hex(binding),
                                main_binding_bytes=raw, imported_address=hex(imported), cache_slide=hex(slide),
                                plan=plan, query=hex(query), checkpoints=[])
                            followed_display['launchd_cache_slide'] = hex(slide)
                            followed_display['activation_events'] = []
                            activation_limit = plan.get('max_activation_events', 512)
                            if not 1 <= activation_limit <= 2048:
                                raise RuntimeError('Invalid shared-region observation budget')
                            activations = set(plan['process_activation']) if plan['mode'] in ('query','user-return','user-trace') else set()
                            for address in activations:
                                if packet(f'Z1,{address:x},4') != 'OK':
                                    raise RuntimeError('Cannot observe actual per-process shared-region startup')
                            while activations:
                                stop, values, interrupted = continue_observing()
                                connection.settimeout(3)
                                if interrupted or values[32] not in activations:
                                    break
                                identity = process_identity()
                                event = dict(pc=hex(values[32]), process=identity)
                                followed_display['activation_events'].append(event)
                                if len(followed_display['activation_events']) > activation_limit:
                                    followed_display['activation_limit_reached'] = True
                                    for address in activations:
                                        if packet(f'z1,{address:x},4') != 'OK':
                                            raise RuntimeError('Cannot retire bounded activation observation')
                                    break
                                if identity['name'] != plan['expected_process']:
                                    address = values[32]
                                    if packet(f'z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot retire an unrelated activation entry')
                                    packet('s')
                                    if packet(f'Z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot restore process activation observation')
                                    continue
                                activation_return, activation_stack = values[30], values[31]
                                for address in activations:
                                    if packet(f'z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot retire actual shared-region activation entries')
                                if packet(f'Z1,{activation_return:x},4') != 'OK':
                                    raise RuntimeError('Cannot observe actual shared-region syscall return')
                                stop, values, interrupted = continue_observing()
                                connection.settimeout(3)
                                if interrupted or values[32] != activation_return or values[31] != activation_stack or process_identity()['thread'] != identity['thread']:
                                    raise RuntimeError('Shared-region startup must return on the selected actual thread/stack')
                                if packet(f'z1,{activation_return:x},4') != 'OK':
                                    raise RuntimeError('Cannot retire shared-region syscall return observation')
                                event['result'] = values[0]
                                identity = process_identity()
                                event['return_process'] = identity
                                if values[0] == 0 and identity['shared_region_slide'] is not None:
                                    slide = identity['shared_region_slide']
                                    if not 0 <= slide <= plan['max_slide'] or slide & 4095:
                                        raise RuntimeError('Selected process must have an actual native-page-aligned cache slide')
                                    query = plan['query'] + slide
                                    followed_display.update(cache_slide=hex(slide), query=hex(query), activated_process=identity)
                                    break
                                for address in activations:
                                    if packet(f'Z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot observe the next real shared-region operation')
                            activated = plan['mode'] == 'discovery' or 'activated_process' in followed_display
                            if activated and plan['mode'] == 'user-trace':
                                sites = plan['trace_sites']
                                if not 1 <= len(sites) <= 4:
                                    raise RuntimeError('Original trace requires one to four bounded sites')
                                pending = {}
                                hits = {}
                                for site in sites:
                                    address = site['address'] + slide
                                    prefix = site['prefix']
                                    if address in pending or address & 3 or not 0 < address < 0xfffffff000000000 - 16 or len(bytes.fromhex(prefix)) != 16:
                                        raise RuntimeError('Invalid or duplicate original trace site')
                                    pending[address] = site
                                    hits[address] = 0
                                    if not 1 <= site.get('max_hits', 1) <= 8:
                                        raise RuntimeError('Original trace hit budget must be one to eight')
                                    if packet(f'Z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot install original trace site')
                                events, skipped = [], []
                                followed_display['trace_events'] = events
                                followed_display['trace_skipped_processes'] = skipped
                                while pending:
                                    stop, values, interrupted = continue_observing()
                                    connection.settimeout(3)
                                    if interrupted:
                                        break
                                    address = values[32]
                                    if address not in pending:
                                        raise RuntimeError('Unexpected stop during original user trace')
                                    identity = process_identity()
                                    site = pending[address]
                                    prefix = packet(f'm{address:x},10')
                                    pstate = struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0]
                                    if prefix != site['prefix'] or pstate & 31:
                                        raise RuntimeError('Original trace requires unchanged EL0 instructions')
                                    if packet(f'z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot remove original trace site')
                                    if identity['name'] == plan['expected_process']:
                                        if identity['shared_region_slide'] != slide:
                                            raise RuntimeError('Original trace cache binding changed')
                                        event = dict(name=site['name'], pc=hex(address), process=identity,
                                            entry_bytes=prefix, pstate=hex(pstate), arguments=[hex(v) for v in values[:8]],
                                            registers={f'x{i}':hex(v) for i,v in enumerate(values[:31])},
                                            caller=hex(values[30]), stack=hex(values[31]))
                                        reads = site.get('memory_reads', [])
                                        if len(reads) > 8:
                                            raise RuntimeError('Original trace memory reads exceed bound')
                                        event['memory'] = []
                                        for request in reads:
                                            reg, offset, size = request['register'], request['offset'], request['size']
                                            if not 0 <= reg <= 30 or not 0 <= offset <= 65536 or not 1 <= size <= 256:
                                                raise RuntimeError('Invalid bounded original trace memory read')
                                            target = values[reg] + offset
                                            read = dict(request, address=hex(target))
                                            if request.get('indirect'):
                                                raw = packet(f'm{target:x},8') if 0 < target < 0xfffffff000000000 - 8 else ''
                                                read['pointer_bytes'] = raw
                                                target = int.from_bytes(bytes.fromhex(raw), 'little') if len(raw) == 16 and not raw.startswith('E') else 0
                                                read['target'] = hex(target)
                                            read['bytes'] = packet(f'm{target:x},{size:x}') if 0 < target < 0xfffffff000000000 - size else None
                                            event['memory'].append(read)
                                        events.append(event)
                                        hits[address] += 1
                                        if hits[address] == site.get('max_hits', 1):
                                            del pending[address]
                                        else:
                                            packet('s')
                                            if packet(f'Z1,{address:x},4') != 'OK':
                                                raise RuntimeError('Cannot restore bounded original trace site')
                                    else:
                                        skipped.append(identity)
                                        if len(skipped) > 64:
                                            raise RuntimeError('Original trace exceeded process selection bound')
                                        packet('s')
                                        if packet(f'Z1,{address:x},4') != 'OK':
                                            raise RuntimeError('Cannot restore process-selected trace site')
                                followed_display['trace_pending'] = [site['name'] for site in pending.values()]
                                followed_display['observed'] = not pending
                                for address in pending:
                                    if packet(f'z1,{address:x},4') != 'OK':
                                        raise RuntimeError('Cannot retire pending original trace site')
                                activated = False
                            if activated and packet(f'Z1,{query:x},4') != 'OK':
                                raise RuntimeError('Cannot observe the original display query')
                            if activated:
                                stop, values, interrupted = continue_observing()
                                connection.settimeout(3)
                            skipped_processes = []
                            while activated and not interrupted and values[32] == query:
                                identity = process_identity()
                                if identity['name'] == plan['expected_process']:
                                    followed_display['entry_process'] = identity
                                    if plan['mode'] == 'discovery':
                                        if identity['shared_region_slide'] != slide:
                                            raise RuntimeError('Actual discovery task cache slide differs from the bound observation address')
                                        followed_display['activated_process'] = identity
                                    break
                                skipped_processes.append(identity)
                                if len(skipped_processes) > 64 or packet(f'z1,{query:x},4') != 'OK':
                                    raise RuntimeError('Display observation exceeded bounded process selection')
                                packet('s')
                                if packet(f'Z1,{query:x},4') != 'OK':
                                    raise RuntimeError('Cannot restore the process-selected display observation')
                                stop, values, interrupted = continue_observing()
                                connection.settimeout(3)
                            followed_display['skipped_processes'] = skipped_processes
                            if activated and not interrupted and values[32] == query:
                                prefix = packet(f'm{query:x},10')
                                pstate = struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0]
                                if prefix != plan['query_prefix'] or pstate & 31:
                                    raise RuntimeError('Require unchanged original query instructions at actual EL0')
                                caller, entry_stack = values[30], values[31]
                                followed_display.update(entry_bytes=prefix, entry_pstate=hex(pstate),
                                    entry_stack=hex(entry_stack), caller=hex(caller))
                                if plan['mode']=='user-return':
                                    followed_display['entry_arguments']=[hex(v) for v in values[:8]]
                                    final_target=query+plan['return_checkpoint']
                                    if packet(f'z1,{query:x},4')!='OK' or packet(f'Z1,{final_target:x},4')!='OK':
                                        raise RuntimeError('Cannot observe the qualified original user initializer return')
                                    stop,values,interrupted=continue_observing();connection.settimeout(3)
                                    unrelated=0
                                    while not interrupted and values[32]==final_target and (values[31]!=entry_stack or process_identity()['thread']!=followed_display['entry_process']['thread']):
                                        unrelated+=1
                                        if unrelated>64 or packet(f'z1,{final_target:x},4')!='OK':raise RuntimeError('Bounded user-return frame selection failed')
                                        packet('s')
                                        if packet(f'Z1,{final_target:x},4')!='OK':raise RuntimeError('Cannot restore original user-return breakpoint')
                                        stop,values,interrupted=continue_observing();connection.settimeout(3)
                                    if not interrupted and values[32]==final_target and values[31]==entry_stack:
                                        if packet(f'm{final_target:x},4')!='c0035fd6' or values[30]!=caller or packet(f'z1,{final_target:x},4')!='OK':
                                            raise RuntimeError('Require unchanged original RET and restored caller/stack')
                                        returned=values[0]
                                        object_bytes=packet(f'm{returned:x},{plan["object_bytes"]:x}') if 0<returned<0xfffffff000000000-plan['object_bytes'] else None
                                        followed_display.update(epilogue_pc=hex(values[32]),epilogue_lr=hex(values[30]),caller_restored=True,
                                            return_value=hex(returned),returned_object_bytes=object_bytes)
                                        stop=packet('s');values=struct.unpack_from('<33Q',bytes.fromhex(packet('g')))
                                        if values[32]!=caller or values[31]!=entry_stack or process_identity()['thread']!=followed_display['entry_process']['thread']:
                                            raise RuntimeError('Actual original initializer RET did not restore its original thread/caller/stack')
                                        followed_display.update(observed=True,return_pc=hex(values[32]),completion_pc=hex(values[32]),
                                            return_stack=hex(values[31]),other_stacks_skipped=unrelated,query_bytes_after_fault=packet(f'm{query:x},10'))
                                else:
                                    checkpoint = query + plan['port_checkpoint']
                                    if packet(f'z1,{query:x},4') != 'OK' or packet(f'Z1,{checkpoint:x},4') != 'OK':
                                        raise RuntimeError('Cannot observe actual render-server port result')
                                    stop, values, interrupted = continue_observing()
                                    connection.settimeout(3)
                                    if not interrupted and values[32] == checkpoint:
                                        if values[29] != entry_stack - 16 or process_identity()['thread'] != followed_display['entry_process']['thread']:
                                            raise RuntimeError('Display port result must be on the original query frame')
                                        followed_display['checkpoints'].append(dict(stage='registry-arm-io' if plan['mode'] == 'discovery' else 'server-port', pc=hex(values[32]),
                                            x0=hex(values[0]), frame=hex(values[29]), stack=hex(values[31])))
                                        server_port = values[0]
                                        if packet(f'z1,{checkpoint:x},4') != 'OK':
                                            raise RuntimeError('Cannot retire the port-result observation')
                                        if server_port and plan['mode'] == 'query':
                                            checkpoint = query + plan['reply_checkpoint']
                                            if packet(f'Z1,{checkpoint:x},4') != 'OK':
                                                raise RuntimeError('Cannot observe actual CASGetDisplays result')
                                            stop, values, interrupted = continue_observing()
                                            connection.settimeout(3)
                                            if not interrupted and values[32] == checkpoint:
                                                if values[29] != entry_stack - 16 or process_identity()['thread'] != followed_display['entry_process']['thread']:
                                                    raise RuntimeError('Display reply must be on the original query frame')
                                                followed_display['checkpoints'].append(dict(stage='display-reply', pc=hex(values[32]),
                                                    x0=hex(values[0]), frame=hex(values[29]), stack=hex(values[31]),
                                                    output_address=packet(f'm{values[29] - 96:x},8'),
                                                    output_length=packet(f'm{values[29] - 100:x},4')))
                                                reply = followed_display['checkpoints'][-1]
                                                pointer = int.from_bytes(bytes.fromhex(reply['output_address']), 'little')
                                                length = int.from_bytes(bytes.fromhex(reply['output_length']), 'little')
                                                if values[0] == 0 and 0 < pointer < 0xfffffff000000000 and 0 < length <= 4096:
                                                    reply['output_bytes'] = packet(f'm{pointer:x},{length:x}')
                                                if packet(f'z1,{checkpoint:x},4') != 'OK':
                                                    raise RuntimeError('Cannot retire the display-reply observation')
                                        if not interrupted and values[32] == checkpoint:
                                            final_target = query + plan['return_checkpoint']
                                            if not 0 < caller < 0xfffffff000000000 or caller & 3 or packet(f'Z1,{final_target:x},4') != 'OK':
                                                raise RuntimeError('Cannot observe the actual user query return')
                                            stop, values, interrupted = continue_observing()
                                            connection.settimeout(3)
                                            unrelated = 0
                                            while not interrupted and values[32] == final_target and (values[31] != entry_stack or
                                                    process_identity()['thread'] != followed_display['entry_process']['thread']):
                                                unrelated += 1
                                                if unrelated > 64 or packet(f'z1,{final_target:x},4') != 'OK':
                                                    raise RuntimeError('Display return exceeded the bounded unrelated frames')
                                                packet('s')
                                                if packet(f'Z1,{final_target:x},4') != 'OK':
                                                    raise RuntimeError('Cannot restore original query return observation')
                                                stop, values, interrupted = continue_observing()
                                                connection.settimeout(3)
                                            if not interrupted and values[32] == final_target and values[31] == entry_stack:
                                                if packet(f'm{final_target:x},4') != 'c0035fd6' or packet(f'z1,{final_target:x},4') != 'OK':
                                                    raise RuntimeError('Require the unchanged actual original RET instruction')
                                                followed_display.update(epilogue_pc=hex(values[32]), epilogue_lr=hex(values[30]),
                                                    caller_restored=values[30] == caller)
                                                if plan['mode'] == 'query':
                                                    stop = packet('s')
                                                    values = struct.unpack_from('<33Q', bytes.fromhex(packet('g')))
                                                    if values[32] != caller or values[31] != entry_stack:
                                                        raise RuntimeError('Original user RET did not restore its actual caller/stack')
                                                    followed_display['return_pc'] = hex(values[32])
                                                followed_display.update(observed=True, completion_pc=hex(values[32]),
                                                    return_stack=hex(values[31]), other_stacks_skipped=unrelated,
                                                    query_bytes_after_fault=packet(f'm{query:x},10'))
                                                if plan['mode'] == 'query':
                                                    followed_display.update(main_display_bytes=packet(f'm{plan["main_display"] + slide:x},8'),
                                                        displays_bytes=packet(f'm{plan["displays"] + slide:x},8'))
                                                if plan['mode'] == 'discovery':
                                                    followed_display['discovery_counts'] = {name: packet(f'm{plan[name] + slide:x},4')
                                                        for name in ('reference_count', 'enumerated_count', 'display_count')}
                    if args.follow_entrypoint_checkpoint_offset is not None:
                        checkpoint = target + args.follow_entrypoint_checkpoint_offset
                        if not 0 < checkpoint < 0xfffffff000000000 or checkpoint & 3:
                            raise RuntimeError('Expected an aligned relative user checkpoint')
                        if packet(f'Z1,{checkpoint:x},4') != 'OK':
                            raise RuntimeError('Cannot observe relative user checkpoint')
                        stop, values, interrupted = continue_observing()
                        connection.settimeout(3)
                        checkpoint_ready = not interrupted and values[32] == checkpoint
                        followed_entry['checkpoint'] = {'target': hex(checkpoint), 'observed': checkpoint_ready,
                            'pc': hex(values[32]),
                            'pstate': hex(struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0]),
                            'bytes': packet(f'm{values[32]:x},10')}
                        if packet(f'z1,{checkpoint:x},4') != 'OK':
                            raise RuntimeError('Cannot remove relative user checkpoint')
                        if checkpoint_ready and args.finish_followed_checkpoint:
                            checkpoint_arguments = values[:4]
                            caller = values[30]
                            if not 0 < caller < 0xfffffff000000000 or caller & 3:
                                raise RuntimeError('Expected an actual aligned user return address')
                            if packet(f'Z1,{caller:x},4') != 'OK':
                                raise RuntimeError('Cannot observe user function return')
                            stop, values, interrupted = continue_observing()
                            connection.settimeout(3)
                            checkpoint_ready = not interrupted and values[32] == caller
                            followed_entry['function_return'] = {'target': hex(caller), 'observed': checkpoint_ready,
                                'pc': hex(values[32]), 'x0': hex(values[0]),
                                'pstate': hex(struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0])}
                            if checkpoint_ready:
                                followed_entry['entry_bytes_after_fault'] = packet(f'm{target:x},10')
                                followed_entry['checkpoint_argument_memory'] = {}
                                for request in args.read_followed_checkpoint_arg:
                                    argument, count = (int(v, 16) for v in request.split(':'))
                                    if not 0 <= argument <= 3 or not 0 < count <= 4096 or not 0 < checkpoint_arguments[argument] < 0xfffffff000000000:
                                        raise RuntimeError('Expected a bounded original user checkpoint argument')
                                    pointer = checkpoint_arguments[argument]
                                    followed_entry['checkpoint_argument_memory'][request] = {
                                        'address': hex(pointer), 'bytes': packet(f'm{pointer:x},{count:x}')}
                            if packet(f'z1,{caller:x},4') != 'OK':
                                raise RuntimeError('Cannot remove user return checkpoint')
                    for _ in range(args.step_followed_entry if checkpoint_ready else 0):
                        connection.settimeout(max(0.001, deadline - time.monotonic()))
                        try:
                            stop = packet('s')
                        except TimeoutError:
                            stop = packet(None)
                            interrupted = True
                            break
                        state_bytes = bytes.fromhex(packet('g'))
                        values = struct.unpack_from('<33Q', state_bytes)
                        followed_entry['steps'].append({'pc': hex(values[32]),
                            'pstate': hex(struct.unpack_from('<I', state_bytes, 264)[0]),
                            'bytes': packet(f'm{values[32]:x},10')})
                connection.settimeout(3)
            finished_entry = None
            return_target = None
            entry_arguments = None
            return_stack = None
            other_stack_returns = 0
            if args.finish_function and values[32] == args.address:
                finished_entry = values[32]
                return_target = values[30]
                entry_arguments = values[:8]
                return_stack = values[31]
                if packet(f'z1,{args.address:x},4') != 'OK' or packet(f'Z1,{return_target:x},4') != 'OK':
                    raise RuntimeError('Cannot observe the function return')
                stop, values, interrupted = continue_observing()
                connection.settimeout(3)
                while not interrupted:
                    action = finish_stop_action(values[32], values[31], return_target, return_stack, trace_remaining)
                    if action == 'function-return':
                        break
                    if action == 'other-stack-return':
                        other_stack_returns += 1
                        if other_stack_returns > 64:
                            raise RuntimeError('Too many unrelated thread returns at the shared caller')
                        if packet(f'z1,{return_target:x},4') != 'OK':
                            raise RuntimeError('Cannot step over another thread at the shared caller')
                        packet('s')
                        if packet(f'Z1,{return_target:x},4') != 'OK':
                            raise RuntimeError('Cannot restore the original thread return observation')
                        stop, values, interrupted = continue_observing()
                        connection.settimeout(3)
                        continue
                    if action == 'tracepoint':
                        if not observe_tracepoint(values):
                            raise RuntimeError('Finish-function tracepoint was not pending')
                        stop, values, interrupted = continue_observing()
                        connection.settimeout(3)
                        continue
                    break
            report = {f'x{i}': hex(value) for i, value in enumerate(values[:31])}
            if process_plan:
                report['process_layout_audit']=dict(native_sha256=process_plan.get('native_sha256'),layout=process_plan['process_layout'])
            report.update(sp=hex(values[31]), pc=hex(values[32]), stop=stop,
                          pstate=hex(struct.unpack_from('<I', bytes.fromhex(packet('g')), 264)[0]),
                          kernel_sha256=state['sha256'],
                          qemu_memory_mib=qemu_memory_mib,
                          qemu_accelerator=args.qemu_accel, qemu_cpu_model='cortex-a710' if args.qemu_accel == 'tcg' else 'host',
                          bounded_timeout_interrupted=interrupted, trace_events=trace_events,
                          skipped_target_hits=skipped_target_hits, entry_string_events=entry_string_events,
                          entry_register_selection=(dict(index=entry_register_equal[0], expected=hex(entry_register_equal[1]), observed=hex(values[entry_register_equal[0]])) if entry_register_equal else None),
                          entry_process_name=args.entry_process_name, entry_min_pid=args.entry_min_pid, entry_process_events=entry_process_events,
                          entry_kernel_bytes_match=entry_kernel_bytes_match, entry_selection_limit=args.entry_max_skips, primary_start_event=primary_start_event,
                          kext_log_events=kext_log_events, kext_log_hits=kext_log_hits,
                          kext_log_capture_exhausted=(kext_log_hits == 256),
                          syscall_number_filter=args.syscall_number, syscall_start_event=syscall_start_event, syscall_events=syscall_events, syscall_log_exhausted=(len(syscall_events) == 512),
                          cstring_start_event=cstring_start_event, cstring_events=cstring_events, cstring_log_exhausted=(len(cstring_events) == 128),
                          registry_string_events=registry_string_events,
                          registry_string_log_exhausted=(len(registry_string_events) == 128),
                          boot_path='uefi-unixnu-darwinboot' if args.esp else 'raw-entry-fixture')
            if firmware_info is not None:
                report['qemu_firmware'] = firmware_info
            if block_input:
                report['virtio_block_input'] = block_input
            if extra_block_input:
                report['virtio_extra_block_input'] = extra_block_input
            if mutation is not None:
                report['own_input_mutation'] = mutation_event
            if followed_main is not None:
                report['followed_main_entry'] = followed_main
            if followed_display is not None:
                report['followed_display_query'] = followed_display
            if followed_entry is not None:
                report['followed_user_entry'] = followed_entry
                report['followed_relative_memory'] = {}
                for request in args.read_followed_relative:
                    offset, length = request.split(':')
                    offset, length = int(offset, 0), int(length, 16)
                    address = int(followed_entry['user_entry'], 16) + offset
                    if abs(offset) > 16 * 1024 * 1024 or not 0 < length <= 4096 or not 0 < address < 0xfffffff000000000:
                        raise ValueError('Read bounded data from the observed user image')
                    report['followed_relative_memory'][request] = {'address': hex(address),
                        'bytes': packet(f'm{address:x},{length:x}')}
            if mapped_mutation is not None:
                report['own_mapped_page_mutation'] = mapped_mutation_event
                report['mapped_page_validation_entries'] = mapped_mutation_hits
                report['mapped_page_validation_entry_limit'] = args.mapped_vnode_max_hits
            if progress_clock:
                report['boot_progress_clock_fixture']=progress_clock_event
            if finished_entry is not None:
                report['finished_function_entry'] = hex(finished_entry)
                report['function_return_target'] = hex(return_target)
                report['function_entry_arguments'] = [hex(value) for value in entry_arguments]
                report['function_entry_stack'] = hex(return_stack)
                report['function_return_stack_matched'] = values[31] == return_stack and values[32] == return_target and not interrupted
                report['function_return_other_stacks_skipped'] = other_stack_returns
            report['entry_regions'] = {}
            region_arguments = entry_arguments
            if region_arguments is None and values[32] == args.address and not interrupted:
                region_arguments = values[:8]
                report['observed_function_entry_arguments'] = [hex(v) for v in region_arguments]
            if region_arguments is not None:
                for request in args.read_entry_region:
                    index, offset, length = (int(part, 16) for part in request.split(':'))
                    if not 0 <= index < 8 or not 0 <= offset <= 4096 or not 0 < length <= 4096:
                        raise ValueError('Read bounded direct entry regions from kernel x0-x7')
                    pointer = region_arguments[index] + offset
                    if not 0xfffffff000000000 <= pointer <= (1 << 64) - length:
                        raise ValueError('Entry region must be actual bounded kernel memory')
                    raw = packet(f'm{pointer:x},{length:x}')
                    if len(raw) != 2 * length or raw.startswith('E'):
                        raise RuntimeError('Cannot read actual entry argument region')
                    report['entry_regions'][request] = dict(address=hex(pointer), bytes=raw)
            report['entry_indirect_memory'] = {}
            if entry_arguments is not None:
                for request in args.read_entry_indirect + args.read_entry_user_indirect:
                    index, offset, length = (int(part, 16) for part in request.split(':'))
                    if not 0 <= index < 4 or not 0 <= offset <= 4096 or not 0 < length <= 4096:
                        raise ValueError('Read bounded pointer fields from entry arguments x0-x3')
                    raw = packet(f'm{entry_arguments[index] + offset:x},8')
                    if len(raw) != 16 or raw.startswith('E'):
                        raise RuntimeError('Cannot read original function argument pointer')
                    pointer = int.from_bytes(bytes.fromhex(raw), 'little')
                    user_pointer = request in args.read_entry_user_indirect
                    if user_pointer and not 0 < pointer < 0xfffffff000000000 - length:
                        raise RuntimeError('Expected a non-null bounded user virtual pointer')
                    if not user_pointer and pointer < 0xfffffff000000000:
                        raise RuntimeError('Expected a kernel virtual pointer')
                    report['entry_indirect_memory'][request] = {'pointer': hex(pointer),
                        'bytes': packet(f'm{pointer:x},{length:x}')}
                    if user_pointer:
                        report['entry_indirect_memory'][request]['address_space'] = 'user'
            report['entry_user_vectors'] = {}
            if finished_entry is not None:
                for request in args.read_entry_user_vector:
                    index, offset, count = (int(part, 16) for part in request.split(':'))
                    if not 0 <= index < 4 or not 0 <= offset <= 4096 or not 1 <= count <= 16 or entry_arguments[index] < 0xfffffff000000000:
                        raise ValueError('Read a bounded user vector field from actual kernel x0-x3')
                    raw = packet(f'm{entry_arguments[index] + offset:x},8')
                    if len(raw) != 16 or raw.startswith('E'):
                        raise RuntimeError('Cannot read actual user argv pointer field')
                    base = int.from_bytes(bytes.fromhex(raw), 'little')
                    if not 0 < base < 0xfffffff000000000 - count * 8:
                        raise RuntimeError('Expected a bounded user argv vector')
                    capture = {'pointer': hex(base), 'values': [], 'terminated': False}
                    for slot in range(count):
                        raw = packet(f'm{base + slot * 8:x},8')
                        if len(raw) != 16 or raw.startswith('E'):
                            raise RuntimeError('Cannot read actual user argv element')
                        pointer = int.from_bytes(bytes.fromhex(raw), 'little')
                        if not pointer:
                            capture['terminated'] = True
                            break
                        if pointer >= 0xfffffff000000000:
                            raise RuntimeError('Expected a user argv string')
                        raw = packet(f'm{pointer:x},{min(256, 4096 - (pointer & 4095)):x}')
                        if raw.startswith('E'):
                            raise RuntimeError('Cannot read actual user argv string')
                        value = bytes.fromhex(raw)
                        capture['values'].append({'pointer': hex(pointer), 'value': value.split(b'\0')[0].decode(errors='replace'),
                                                  'truncated': b'\0' not in value})
                    report['entry_user_vectors'][request] = capture
            if args.inspect_entry_user_state and values[32] == args.address and not interrupted:
                report['entry_user_state_candidates'] = []
                state_pointers=[(f'x{index}',values[index]) for index in range(19,29)]
                if process_plan:
                    current=process_identity()
                    if current.get('user_state'):state_pointers.append(('current-thread.upcb',int(current['user_state'],16)))
                seen_state_pointers=set()
                for source,pointer in state_pointers:
                    if pointer in seen_state_pointers:continue
                    seen_state_pointers.add(pointer)
                    if not 0xfffffff000000000 <= pointer < (1 << 64) - 304 or pointer % 8:
                        continue
                    raw = packet(f'm{pointer:x},130')
                    if raw.startswith('E') or len(raw) != 608:
                        continue
                    saved = bytes.fromhex(raw)
                    if struct.unpack_from('<2I', saved) != (21, 72):
                        continue
                    regs = struct.unpack_from('<33Q', saved, 8)
                    cpsr = struct.unpack_from('<I', saved, 272)[0]
                    if cpsr & 31 or not 0 < regs[32] < 0xfffffff000000000 or not 0 < regs[31] < 0xfffffff000000000:
                        continue
                    candidate = dict(source_register=source, pointer=hex(pointer), saved_bytes=raw,
                        user_pc=hex(regs[32]), user_sp=hex(regs[31]), user_fp=hex(regs[29]), user_lr=hex(regs[30]),
                        user_arguments=[hex(v) for v in regs[:8]], cpsr=hex(cpsr),
                        far=hex(struct.unpack_from('<Q', saved, 280)[0]), esr=hex(struct.unpack_from('<I', saved, 288)[0]),
                        instruction_bytes=packet(f'm{regs[32]:x},20'), frames=[])
                    candidate['register_memory']={}
                    for request,register,length in saved_user_requests:
                        address=regs[register]
                        if not 0<address<0xfffffff000000000-length:
                            candidate['register_memory'][request]=dict(address=hex(address),error='Not a bounded saved user pointer')
                            continue
                        capture=dict(address=hex(address),bytes='')
                        # Keep actual bytes preceding an unavailable demand-
                        # paged region. Never lose a useful diagnostic prefix
                        # because the tail of its bounded buffer is unmapped.
                        while len(capture['bytes'])//2<length:
                            cursor=address+len(capture['bytes'])//2
                            count=min(512,length-len(capture['bytes'])//2,4096-(cursor&4095))
                            chunk=packet(f'm{cursor:x},{count:x}')
                            if chunk.startswith('E') or len(chunk)!=count*2:
                                capture['read_error']=dict(address=hex(cursor),reply=chunk)
                                break
                            capture['bytes']+=chunk
                        candidate['register_memory'][request]=capture
                    frame = regs[29]
                    for _ in range(16):
                        if frame < regs[31] or frame - regs[31] > 0x10000 or frame % 16:
                            break
                        frame_raw = packet(f'm{frame:x},10')
                        if frame_raw.startswith('E') or len(frame_raw) != 32:
                            break
                        parent, caller = struct.unpack('<2Q', bytes.fromhex(frame_raw))
                        candidate['frames'].append(dict(fp=hex(frame), lr=hex(caller)))
                        if parent <= frame:
                            break
                        frame = parent
                    report['entry_user_state_candidates'].append(candidate)
            report['stack_bytes'] = packet(f'm{values[31]:x},100')
            report['frame_chain'] = []
            frame = values[29]
            for _ in range(16):
                if frame < values[31] or frame - values[31] > 0x10000 or frame % 16:
                    break
                raw = packet(f'm{frame:x},10')
                if len(raw) != 32 or raw.startswith('E'):
                    break
                parent, caller = struct.unpack('<2Q', bytes.fromhex(raw))
                report['frame_chain'].append({'fp': hex(frame), 'lr': hex(caller)})
                if args.read_frame_locals and frame >= 0xfffffff000000000:
                    start = max(values[31], frame - 192)
                    report['frame_chain'][-1]['local_address'] = hex(start)
                    report['frame_chain'][-1]['local_bytes'] = packet(f'm{start:x},{frame + 16 - start:x}')
                if parent <= frame:
                    break
                frame = parent
            report['frame_indirect_memory'] = {}
            for request in args.read_frame_indirect:
                index, offset, length = (int(part, 16) for part in request.split(':'))
                if not 0 <= index < 16 or not -192 <= offset <= 8 or offset % 8 or not 0 < length <= 4096:
                    raise ValueError('Read a bounded aligned kernel frame slot and at most 4096 pointed bytes')
                if index >= len(report['frame_chain']):
                    continue
                frame = int(report['frame_chain'][index]['fp'], 16)
                if frame < 0xfffffff000000000:
                    continue
                raw = packet(f'm{frame + offset:x},8')
                if raw.startswith('E') or len(raw) != 16:
                    raise RuntimeError('Cannot read actual kernel frame pointer slot')
                pointer = int.from_bytes(bytes.fromhex(raw), 'little')
                if not 0xfffffff000000000 <= pointer <= (1 << 64) - length:
                    raise RuntimeError('Actual frame slot is not a bounded kernel pointer')
                report['frame_indirect_memory'][request] = dict(slot_address=hex(frame + offset),
                    pointer=hex(pointer), bytes=packet(f'm{pointer:x},{length:x}'))
            report['stack_pointer_memory'] = {}
            stack_raw = bytes.fromhex(report['stack_bytes']) if not report['stack_bytes'].startswith('E') else b''
            stack_pointers = struct.unpack_from('<5Q', stack_raw) if len(stack_raw) >= 40 else ()
            for index, pointer in enumerate(stack_pointers):
                if pointer >= 0xfffffff000000000:
                    raw = packet(f'm{pointer:x},130')
                    report['stack_pointer_memory'][str(index)] = {'address': hex(pointer), 'bytes': raw}
            saved = report['stack_pointer_memory'].get('3', {}).get('bytes', '')
            if len(saved) == 608 and not saved.startswith('E'):
                raw_state = bytes.fromhex(saved)
                if struct.unpack_from('<2I', raw_state) == (21, 72):
                    report['saved_exception_registers'] = {}
                    report['saved_exception_argument_memory'] = {}
                    for index in range(4):
                        value, = struct.unpack_from('<Q', raw_state, 8 + index * 8)
                        report['saved_exception_registers'][f'x{index}'] = hex(value)
                        if value >= 0xfffffff000000000:
                            raw = packet(f'm{value:x},100')
                            if not raw.startswith('E'):
                                report['saved_exception_argument_memory'][f'x{index}'] = bytes.fromhex(raw).split(b'\0', 1)[0].decode(errors='replace')
            report['memory'] = {}
            for request in args.read_memory:
                address, length = (int(part, 16) for part in request.split(':'))
                if not 0 < length <= 4096:
                    raise ValueError('Inspect at most 4096 bytes per request')
                report['memory'][request] = packet(f'm{address:x},{length:x}')
            report['indirect_memory'] = {}
            if args.mac_policy_layout:
                plan=json.loads(args.mac_policy_layout.read_text());layout=plan['layout'];base=plan['address']
                def mac_read(address,size):
                    if address<0xfffffff000000000 or not 0<size<=4096:raise ValueError('Bounded kernel MAC policy read')
                    raw=packet(f'm{address:x},{size:x}')
                    if raw.startswith('E') or len(raw)!=size*2:raise ValueError('Actual MAC policy memory unavailable')
                    return bytes.fromhex(raw)
                def mac_word(address,size):return int.from_bytes(mac_read(address,size),'little')
                header=layout['mac_policy_list'];count=mac_word(base+header['numloaded'],4);maximum=mac_word(base+header['maxindex'],4)
                capacity=mac_word(base+header['max'],4);entries=mac_word(base+header['entries'],8)
                if not 0<=count<=64 or not 0<=maximum<64 or capacity<=maximum:raise ValueError('Bounded real MAC policy list')
                policies=[]
                for i in range(maximum+1):
                    conf=mac_word(entries+i*8+layout['mac_policy_list_element']['mpc'],8)
                    if not conf:continue
                    fields=layout['mac_policy_conf'];ops=mac_word(conf+fields['mpc_ops'],8)
                    name=mac_word(conf+fields['mpc_name'],8);fullname=mac_word(conf+fields['mpc_fullname'],8)
                    row=dict(index=i,configuration=hex(conf),operations=hex(ops),name=mac_read(name,64).split(b'\0',1)[0].decode(errors='replace'),
                        fullname=mac_read(fullname,128).split(b'\0',1)[0].decode(errors='replace'),hooks={})
                    for hook,offset in layout['mac_policy_ops'].items():
                        address=mac_word(ops+offset,8);row['hooks'][hook]=dict(address=hex(address),instructions=mac_read(address,64).hex() if address else '')
                    policies.append(row)
                report['registered_mac_policies']=dict(count=count,maximum_index=maximum,entries=hex(entries),policies=policies,layout=plan,read_only=True)
            def capture_indirect_memory(request, optional):
                address, offset, length = parse_indirect_memory_request(request)
                pointer = packet(f'm{address:x},8')
                if len(pointer) != 16 or pointer.startswith('E'):
                    if not optional:
                        raise RuntimeError(f'Cannot read pointer at {address:x}')
                    report['indirect_memory'][request] = {
                        'unavailable': f'cannot read pointer field at {address:x}'}
                    return
                base = int.from_bytes(bytes.fromhex(pointer), 'little')
                if not base:
                    # Early panics legitimately precede optional subsystem
                    # initialization. Preserve registers and other evidence.
                    report['indirect_memory'][request] = {'pointer': '0x0',
                        'unavailable': 'subsystem pointer is not initialized'}
                    return
                target = base + offset
                if target >= 1 << 64 or target + length > 1 << 64:
                    if not optional:
                        raise RuntimeError('Invalid indirect memory pointer: ' + request)
                    report['indirect_memory'][request] = {
                        'pointer': hex(base), 'unavailable': 'indirect target range overflows 64 bits',
                    }
                    return
                contents = packet(f'm{target:x},{length:x}')
                if contents.startswith('E') or len(contents) != length * 2:
                    if not optional:
                        raise RuntimeError('Cannot read indirect target: ' + request)
                    report['indirect_memory'][request] = {
                        'pointer': hex(base), 'address': hex(target),
                        'unavailable': 'GDB could not read the bounded indirect target',
                    }
                    return
                report['indirect_memory'][request] = {
                    'pointer': hex(base), 'address': hex(target), 'bytes': contents,
                }
            for request in args.read_indirect:
                capture_indirect_memory(request, optional=False)
            for request in args.read_indirect_optional:
                capture_indirect_memory(request, optional=True)
            if args.system_registers:
                target = feature('target.xml')
                report['system_registers'] = {}
                wanted = {'TCR_EL1', 'TTBR0_EL1', 'TTBR1_EL1', 'SCTLR_EL1', 'CNTFRQ_EL0',
                          'ESR_EL1', 'FAR_EL1', 'ELR_EL1', 'SPSR_EL1', 'ID_AA64MMFR1_EL1', 'ID_AA64MMFR0_EL1', 'MIDR_EL1'}
                if process_plan:
                    wanted.add('TPIDR_EL1')
                for include in target.findall('{http://www.w3.org/2001/XInclude}include'):
                    document = feature(include.attrib['href'])
                    for register in document.findall('reg'):
                        if register.attrib['name'] in wanted:
                            raw = packet(f'p{int(register.attrib["regnum"]):x}')
                            value = int.from_bytes(bytes.fromhex(raw), 'little')
                            report['system_registers'][register.attrib['name']] = hex(value)
                if followed_display and followed_display['observed']:
                    followed_display['actual_process'] = process_identity()
                if args.process_layout_plan:
                    try:
                        report['actual_process'] = process_identity()
                    except RuntimeError as error:
                        if not args.inspect_ui_threads or args.entry_process_name:
                            raise
                        # An arbitrary final snapshot may interrupt a kernel
                        # thread with no BSD process. Preserve that limitation;
                        # the selected UI tasks are independently owner-checked.
                        report['actual_process_error'] = str(error)
                if args.inspect_entry_translations or kernel_ranges or indirect_kernel_ranges:
                    regs = {name: int(value, 16) for name, value in report['system_registers'].items()}
                    tcr = regs['TCR_EL1']
                    bits = 64 - ((tcr >> 16) & 63)
                    if (tcr >> 30) & 3 != 2 or not 25 <= bits <= 48:
                        raise RuntimeError('Translation inspection requires actual 4K TTBR1 configuration')

                    def translate_kernel_pointer(va):
                        input_va = va & ((1 << bits) - 1)
                        table = regs['TTBR1_EL1'] & 0x0000fffffffff000
                        levels = (bits - 12 + 8) // 9
                        walk = []
                        for level in range(4 - levels, 4):
                            shift = 12 + 9 * (3 - level)
                            address = table + ((input_va >> shift) & 511) * 8
                            raw = packet(f'm{address:x},8')
                            if len(raw) != 16 or raw.startswith('E'):
                                raise RuntimeError('Cannot read actual page-table descriptor')
                            descriptor = int.from_bytes(bytes.fromhex(raw), 'little')
                            walk.append({'level': level, 'descriptor_pa': hex(address), 'descriptor': hex(descriptor)})
                            if not descriptor & 1:
                                if args.allow_unmapped_kernel_pages:
                                    return {'walk': walk, 'mapped': False}
                                raise RuntimeError('Captured pointer has no valid hardware translation')
                            if level < 3 and descriptor & 3 == 3:
                                table = descriptor & 0x0000fffffffff000
                                continue
                            if (level == 3 and descriptor & 3 != 3) or (level < 3 and descriptor & 3 != 1):
                                raise RuntimeError('Invalid leaf descriptor type')
                            return {'walk': walk, 'leaf_level': level,
                                'leaf_bytes': 1 << shift, 'ap': (descriptor >> 6) & 3,
                                'attr_index': (descriptor >> 2) & 7, 'shareability': (descriptor >> 8) & 3,
                                'accessed': bool(descriptor & (1 << 10)),
                                'pxn': bool(descriptor & (1 << 53)), 'uxn': bool(descriptor & (1 << 54)),
                                'physical_address': hex((descriptor & 0x0000fffffffff000 & ~((1 << shift) - 1)) | (va & ((1 << shift) - 1)))}
                        raise RuntimeError('No hardware translation leaf')

                    resolved_ranges = [(name, va, size, None) for name, va, size in kernel_ranges]
                    for name, pointer_address, size,offset in indirect_kernel_ranges:
                        raw = packet(f'm{pointer_address:x},8')
                        if len(raw) != 16 or raw.startswith('E'):
                            raise RuntimeError('Cannot read actual kernel range pointer')
                        va = int.from_bytes(bytes.fromhex(raw), 'little')+offset
                        if not 0xfffffff000000000 <= va < va + size < 1 << 64:
                            raise RuntimeError('Invalid actual indirect kernel range')
                        resolved_ranges.append((name, va, size, pointer_address))

                    old_mode = packet('qqemu.PhyMemMode')
                    if old_mode not in ('0', '1') or packet('Qqemu.PhyMemMode:1') != 'OK':
                        raise RuntimeError('QEMU physical debugger reads are unavailable')
                    try:
                        if args.inspect_entry_translations:
                            for capture in [*report['entry_indirect_memory'].values(), *report['indirect_memory'].values()]:
                                capture['translation'] = translate_kernel_pointer(int(capture['pointer'], 16))
                        if resolved_ranges:
                            report['kernel_range_translations'] = {}
                            for name, va, size, pointer_address in resolved_ranges:
                                pages = [dict(virtual_address=hex(page), **translate_kernel_pointer(page))
                                         for page in range(va & ~4095, (va + size + 4095) & ~4095, 4096)]
                                report['kernel_range_translations'][name] = dict(address=hex(va), bytes=size, pages=pages)
                                if pointer_address is not None:
                                    report['kernel_range_translations'][name]['pointer_address'] = hex(pointer_address)
                    finally:
                        if packet('Qqemu.PhyMemMode:' + old_mode) != 'OK':
                            raise RuntimeError('Cannot restore debugger virtual-memory mode')
            report['argument_memory'] = {}
            for index, value in enumerate(values[:4]):
                if value >= 0xfffffff000000000:
                    raw = packet(f'm{value:x},100')
                    if not raw.startswith('E'):
                        report['argument_memory'][f'x{index}'] = bytes.fromhex(raw).split(b'\0')[0].decode(errors='replace')
            if args.capture_screen:
                try:
                    with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as qmp:
                        qmp.settimeout(5);qmp.connect(str(screen_socket.resolve()))
                        with qmp.makefile('rwb',buffering=0) as stream:
                            if 'QMP' not in json.loads(stream.readline()):raise RuntimeError('Invalid actual QMP greeting')
                            def screen_request(command_name,arguments,identifier):
                                qmp.sendall((json.dumps(dict(execute=command_name,arguments=arguments,id=identifier))+'\n').encode())
                                while True:
                                    response=json.loads(stream.readline())
                                    if response.get('id')!=identifier:continue
                                    if 'error' in response:raise RuntimeError(str(response['error']))
                                    if 'return' not in response:raise RuntimeError('Missing actual QMP response')
                                    return response['return']
                            screen_request('qmp_capabilities',{},1)
                            screen=fixture/'screen.png'
                            screen_request('screendump',dict(filename=str(screen.resolve()),format='png'),2)
                            data=screen.read_bytes()
                            if data[:8]!=bytes.fromhex('89504e470d0a1a0a'):raise RuntimeError('QEMU did not produce an actual PNG')
                            width,height=struct.unpack_from('>2I',data,16)
                            report['qemu_screen_capture']=dict(file='screen.png',sha256=hashlib.sha256(data).hexdigest(),bytes=len(data),width=width,height=height)
                except Exception as error:
                    report['qemu_screen_capture']=dict(error=str(error))
            if args.allproc_address is not None:
                def mem_bytes(address, length):
                    if not 0xfffffff000000000 <= address < (1 << 64) - length:
                        return None
                    raw = packet(f'm{address:x},{length:x}')
                    if len(raw) != length * 2 or raw.startswith('E'):
                        return None
                    return bytes.fromhex(raw)
                entries = []
                node = None
                head = mem_bytes(args.allproc_address, 8)
                if head:
                    node = int.from_bytes(head, 'little')
                seen = set()
                while node and node not in seen and len(entries) < args.proc_list_max:
                    seen.add(node)
                    pid_raw = mem_bytes(node + args.proc_pid_offset, 4)
                    comm_raw = mem_bytes(node + args.proc_comm_offset, 17)
                    stat_raw = mem_bytes(node + args.proc_stat_offset, 1)
                    next_raw = mem_bytes(node + args.proc_list_offset, 8)
                    if pid_raw is None or next_raw is None:
                        break
                    entries.append(dict(
                        address=hex(node),
                        pid=int.from_bytes(pid_raw, 'little', signed=True),
                        name=(comm_raw.split(b'\0', 1)[0].decode('utf-8', 'replace') if comm_raw else ''),
                        state=(stat_raw[0] if stat_raw else None)))
                    node = int.from_bytes(next_raw, 'little')
                report['process_list'] = entries
                if args.inspect_ui_threads:
                    from thread_snapshot import capture
                    report['ui_thread_snapshots'] = capture(packet, entries, process_plan, report['system_registers'])
            if args.capture_scratch_data:
                base_symbol, size_symbol, expected = data_capture
                def data_word(address):
                    if not 0xfffffff000000000 <= address < (1 << 64) - 8:
                        raise ValueError('Invalid native Data capture pointer')
                    raw = packet(f'm{address:x},8')
                    if raw.startswith('E') or len(raw) != 16:
                        raise ValueError('Cannot read native Data capture pointer')
                    return int.from_bytes(bytes.fromhex(raw), 'little')
                base, size = data_word(base_symbol), data_word(size_symbol)
                if size != expected or base & 4095 or not 0xfffffff000000000 <= base < (1 << 64) - size:
                    raise ValueError('Actual native Data range differs from the bounded input')
                destination = fixture / 'scratch-data-snapshot.apfs'
                digest = hashlib.sha256()
                with destination.open('xb') as output:
                    for offset in range(0, size, 16384):
                        count = min(16384, size - offset)
                        raw = packet(f'm{base + offset:x},{count:x}')
                        if raw.startswith('E') or len(raw) != count * 2:
                            raise ValueError('Incomplete scratch Data snapshot')
                        block = bytes.fromhex(raw)
                        if offset == 0 and block[32:36] != b'NXSB':
                            raise ValueError('Actual Data container header differs')
                        output.write(block)
                        digest.update(block)
                report['scratch_data_snapshot'] = dict(file=destination.name, bytes=size,
                    address=hex(base), sha256=digest.hexdigest(), read_only=True,
                    guest_flushed=False, consistency='paused live block image; filesystem may be unclean')
            if args.exercise_ufs_allocation:
                if interrupted or values[32] != args.address:
                    raise RuntimeError('Allocation exercise requires the requested checkpoint')
                from ufs_allocation import exercise
                report['ufs_allocation_exercise'] = exercise(packet, feature, continue_observing,
                    args.kernel, report['system_registers'], fixture, args.esp)
            (fixture / 'gdb-stop.json').write_text(json.dumps(report, indent=2) + '\n')
            print(json.dumps({key: report[key] for key in ['x0', 'x1', 'x29', 'x30', 'sp', 'pc', 'stop']}, indent=2))
            print(json.dumps(report['argument_memory'], ensure_ascii=False))
            if args.system_registers:
                print(json.dumps(report['system_registers'], indent=2))
    except Exception as error:
        (fixture / 'gdb-failure.json').write_text(json.dumps({
            'kernel_sha256': state['sha256'], 'qemu_accelerator': args.qemu_accel,
            'qemu_memory_mib': qemu_memory_mib,
            'qemu_cpu_model': 'cortex-a710' if args.qemu_accel == 'tcg' else 'host',
            'error': str(error), 'qemu_exit_code': process.poll(),
            'trace_events': locals().get('trace_events', []),
            # A late diagnostic read can fail at an early boot panic. Preserve
            # already captured registers/arguments without certifying a stop.
            'partial_report': locals().get('report', {}),
            'checkpoint_observed': False}, indent=2) + '\n')
        raise
    finally:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        if args.capture_screen:screen_directory.cleanup()
