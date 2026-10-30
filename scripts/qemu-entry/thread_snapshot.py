"""Bounded read-only UI thread snapshots using the exact compiler layout.

Saved user/kernel registers and bounded user/kernel frame records are read.
User stacks are read through the task's actual native 4K tables; no guest
registers, memory or translations are changed. A saved kernel LR/FP chain is a
candidate switch context, not proof of a thread's current wait cause.
"""
import struct


KERNEL_VA_MIN = 0xfffffff000000000
KERNEL_PAGE_BYTES = 4096
KERNEL_STACK_MAX_BYTES = 1024 * 1024
KERNEL_FRAME_LIMIT = 16


def capture_kernel_context(read, thread, layout, stack_size):
    def word(address):
        return int.from_bytes(read(address, 8), 'little')

    if (not isinstance(stack_size, int) or stack_size < KERNEL_PAGE_BYTES or
            stack_size > KERNEL_STACK_MAX_BYTES or stack_size % KERNEL_PAGE_BYTES):
        raise ValueError('Invalid runtime kernel stack size')
    base = word(thread + layout['thread_kernel_stack'])
    kstackptr = word(thread + layout['thread_kstackptr'])
    state_size = layout['thread_kernel_state_size']
    if (not isinstance(state_size, int) or state_size <= 0 or state_size > stack_size or
            base < KERNEL_VA_MIN or base >= (1 << 64) - stack_size or base % KERNEL_PAGE_BYTES):
        raise ValueError('Invalid bounded kernel stack base or state size')
    top = base + stack_size
    state_address = top - state_size
    if kstackptr != state_address:
        raise ValueError('Signed or out-of-range machine.kstackptr does not match stack attachment')

    machine = layout['thread_kernel_state_machine']
    values = {}
    for name in ('fp', 'lr', 'sp'):
        offset = (machine + layout['kernel_context_ss'] +
                  layout['kernel_saved_state_' + name])
        if not isinstance(offset, int) or offset < 0 or offset + 8 > state_size:
            raise ValueError('Kernel context member exceeds compiler-qualified state size')
        values[name] = word(state_address + offset)
    if values['sp'] < base or values['sp'] > state_address or values['sp'] & 15:
        raise ValueError('Saved kernel SP is outside the usable stack')
    if values['fp'] and (values['fp'] < base or values['fp'] > state_address - 16 or values['fp'] & 15):
        raise ValueError('Saved kernel FP is outside the usable stack')

    frames = []
    frame_error = None
    frames_complete = not values['fp']
    fp = values['fp']
    for _ in range(KERNEL_FRAME_LIMIT):
        if not fp:
            frames_complete = True
            break
        if fp < base or fp > state_address - 16 or fp & 15:
            frame_error = 'Kernel frame exceeds bounded usable stack'
            break
        next_fp, lr = struct.unpack('<2Q', read(fp, 16))
        frames.append(dict(fp=hex(fp), next_fp=hex(next_fp), lr=hex(lr)))
        if not next_fp:
            frames_complete = True
            break
        if next_fp <= fp or next_fp > state_address - 16 or next_fp & 15:
            frame_error = 'Kernel frame chain is not increasing within stack bounds'
            break
        fp = next_fp
    else:
        frame_error = 'Kernel frame chain exceeds bounded frame count'

    result = dict(read_only=True, stack_base=hex(base), stack_top=hex(top),
                  stack_size=stack_size, state_address=hex(state_address), state_size=state_size,
                  saved_pc_available=False, saved_fp=hex(values['fp']), saved_lr=hex(values['lr']),
                  saved_sp=hex(values['sp']), frames=frames, frames_complete=frames_complete,
                  scope='candidate saved Switch_context register image; LR/FP do not establish a wait cause')
    if frame_error:
        result['frames_error'] = frame_error
    return result


SNAPSHOT_PROCESS_NAMES = (
    'PreBoard', 'Setup', 'SpringBoard', 'backboardd', 'lsd', 'runningboardd', 'datamigrator',
    'com.apple.datami', 'keybagd', 'containermanager', 'installd', 'cfprefsd', 'launchd',
    'chronod', 'liveactivitiesd',
)


def select_processes(processes, include_kernel_task=False):
    if not isinstance(include_kernel_task, bool):
        raise ValueError('Kernel-task thread inclusion must be explicit')
    kernel_task = next((process for process in processes
                        if include_kernel_task and process.get('pid') == 0 and
                        process.get('name') == 'kernel_task'), None)
    selected = [process for process in processes
                if process.get('name') in SNAPSHOT_PROCESS_NAMES]
    if kernel_task is not None:
        selected.insert(0, kernel_task)
    return selected[:16]


def should_capture_kernel_context(process, thread_index, include_kernel_task_first_context=False):
    if process.get('pid') == 1 and process.get('name') == 'launchd':
        return True
    return (include_kernel_task_first_context and thread_index == 0 and
            process.get('pid') == 0 and process.get('name') == 'kernel_task')


def capture(packet, processes, plan, registers, include_kernel_task=False,
            include_kernel_task_first_context=False):
    if not isinstance(include_kernel_task_first_context, bool):
        raise ValueError('Kernel-task context inclusion must be explicit')
    if include_kernel_task_first_context and not include_kernel_task:
        raise ValueError('Kernel-task context capture requires kernel-task thread enumeration')
    layout = plan['thread_snapshot_layout']
    common = plan['process_layout']
    if any(not isinstance(v, int) or not 0 <= v < 65536 for v in layout.values()):
        raise ValueError('Unbounded compiler thread layout')
    required_kernel_layout = {
        'thread_kernel_stack', 'thread_kstackptr', 'thread_kernel_state_machine',
        'thread_kernel_state_size', 'kernel_context_ss',
        'kernel_saved_state_fp', 'kernel_saved_state_lr', 'kernel_saved_state_sp',
    }
    if not required_kernel_layout <= layout.keys():
        raise ValueError('Missing compiler-qualified kernel context layout')
    stack_size_address = plan.get('thread_snapshot_kernel_stack_size')
    if not isinstance(stack_size_address, int) or not KERNEL_VA_MIN <= stack_size_address < 1 << 64:
        raise ValueError('Missing compiler-qualified kernel_stack_size symbol')

    def read(address, size):
        if not 0xfffffff000000000 <= address <= (1 << 64) - size or not 0 < size <= 304:
            raise ValueError('Not a bounded kernel saved-state read')
        raw = packet(f'm{address:x},{size:x}')
        if raw.startswith('E') or len(raw) != size * 2:
            raise ValueError('Kernel saved-state memory unavailable')
        return bytes.fromhex(raw)

    def word(address, size=8):
        return int.from_bytes(read(address, size), 'little')

    results = []
    selected = select_processes(processes, include_kernel_task)
    kernel_stack_size = None
    kernel_stack_size_error = None
    if (any(p.get('pid') == 1 and p.get('name') == 'launchd' for p in selected) or
            (include_kernel_task_first_context and any(
                p.get('pid') == 0 and p.get('name') == 'kernel_task' for p in selected))):
        try:
            kernel_stack_size = word(stack_size_address)
            if (kernel_stack_size < KERNEL_PAGE_BYTES or kernel_stack_size > KERNEL_STACK_MAX_BYTES or
                    kernel_stack_size % KERNEL_PAGE_BYTES):
                raise ValueError('Invalid runtime kernel stack size')
        except (OSError, RuntimeError, ValueError) as error:
            kernel_stack_size = None
            kernel_stack_size_error = str(error)
    for proc in selected[:16]:
        row = dict(process=proc, threads=[], complete=False)
        results.append(row)
        try:
            address = int(proc['address'], 16)
            ro = word(address + layout['proc_ro'])
            if word(ro + layout['proc_ro_owner']) != address:
                raise ValueError('Process read-only owner mismatch')
            task = word(ro + layout['proc_ro_task'])
            region = word(task + common['task_region'])
            row.update(task=hex(task), thread_count=word(task + layout['task_thread_count'], 4),
                       cache_slide=word(region + common['region_slide'], 4) if region else None)
            vm_map = word(task + layout['task_map'])
            pmap = word(vm_map + layout['map_pmap'])
            attr = word(plan['thread_snapshot_native_pt_attr'])
            row.update(pmap=hex(pmap), ttep=word(pmap + layout['pmap_ttep']),
                       root_level=word(attr + layout['attr_root'], 4),
                       page_shift=word(attr + layout['attr_shift']))
            head = task + layout['task_threads']
            node = word(head)
            seen = set()
            while node != head and node not in seen and len(row['threads']) < 64:
                seen.add(node)
                # XNU queue_enter/queue_iterate stores object pointers, not
                # addresses of the embedded task_threads linkage.
                thread = node
                tro = word(thread + common['thread_ro'])
                if word(tro + common['ro_owner']) != thread or word(tro + common['ro_task']) != task:
                    raise ValueError('Thread read-only owner/task mismatch')
                state = word(thread + common['thread_user_state'])
                item = dict(thread=hex(thread), thread_id=word(thread + layout['thread_id']),
                            state=word(thread + layout['thread_state'], 4),
                            continuation=hex(word(thread + layout['continuation'])),
                            wait_event=hex(word(thread + layout['wait_event'])), saved_state=hex(state))
                row['threads'].append(item)
                if should_capture_kernel_context(
                        proc, len(row['threads']) - 1, include_kernel_task_first_context):
                    if kernel_stack_size_error:
                        item['saved_kernel_context_error'] = kernel_stack_size_error
                    else:
                        try:
                            item['saved_kernel_context'] = capture_kernel_context(
                                read, thread, layout, kernel_stack_size)
                        except (OSError, RuntimeError, ValueError) as error:
                            item['saved_kernel_context_error'] = str(error)
                if state:
                    raw = read(state, 304)
                    if struct.unpack_from('<2I', raw) == (21, 72):
                        regs = struct.unpack_from('<33Q', raw, 8)
                        cpsr, = struct.unpack_from('<I', raw, 272)
                        if cpsr & 31 == 0:
                            item.update(user_pc=hex(regs[32]), user_lr=hex(regs[30]),
                                        user_fp=hex(regs[29]), user_sp=hex(regs[31]),
                                        user_arguments=[hex(v) for v in regs[:8]], cpsr=hex(cpsr))
                node = word(thread + layout['thread_link'])
            row['complete'] = node == head and len(row['threads']) == row['thread_count']
        except ValueError as error:
            row['error'] = str(error)
    tcr = int(registers['TCR_EL1'], 16)
    if (tcr >> 14) & 3 or (tcr >> 30) & 3 != 2:
        raise ValueError('Stack snapshot requires active native 4K CPU translation')
    old_mode = packet('qqemu.PhyMemMode')
    if old_mode not in ('0', '1') or packet('Qqemu.PhyMemMode:1') != 'OK':
        raise ValueError('QEMU physical debugger reads unavailable')
    try:
        def physical(address, size):
            if not 0 < address < (1 << 48) - size or not 0 < size <= 16:
                raise ValueError('Invalid bounded physical read')
            raw = packet(f'm{address:x},{size:x}')
            if raw.startswith('E') or len(raw) != size * 2:
                raise ValueError('Unbacked physical memory')
            return bytes.fromhex(raw)

        for row in results:
            if row.get('page_shift') != 12 or row.get('root_level') not in (0, 1, 2):
                row['stack_error'] = 'Unqualified native 4K task geometry'
                continue

            def user_record(va):
                va_bits = 64 - (tcr & 63)
                root_bits = va_bits - (12 + 9 * (3 - row['root_level']))
                if not 1 <= root_bits <= 9:
                    raise ValueError('Unsupported task root-table extent')
                if not 0 < va < 1 << va_bits or va & 15:
                    raise ValueError('Invalid user frame address')
                table = row['ttep']
                # A native 4K hierarchy can have a smaller initial table:
                # T0SZ=26 uses 256 L1 entries, aligned to 2048 bytes.
                if table & ((8 << root_bits) - 1):
                    raise ValueError('Unaligned task page table')
                for level in range(row['root_level'], 4):
                    shift = 12 + 9 * (3 - level)
                    descriptor = int.from_bytes(physical(table + ((va >> shift) & 511) * 8, 8), 'little')
                    if descriptor & 1 == 0:
                        raise ValueError('User frame is not resident')
                    base = descriptor & 0x0000fffffffff000
                    if level < 3 and descriptor & 3 == 3:
                        table = base
                        continue
                    if (level == 3 and descriptor & 3 != 3) or (level < 3 and descriptor & 3 != 1):
                        raise ValueError('Invalid user frame page descriptor')
                    if not descriptor & (1 << 6):
                        raise ValueError('User frame page lacks EL0 access')
                    return physical((base & ~((1 << shift) - 1)) | (va & ((1 << shift) - 1)), 16)
                raise ValueError('Missing user frame leaf')

            for item in row['threads']:
                if 'user_fp' not in item:
                    continue
                fp, sp = int(item['user_fp'], 16), int(item['user_sp'], 16)
                item['user_frames'] = []
                item['user_stack_complete'] = False
                try:
                    for _ in range(32):
                        if not fp:
                            break
                        if not sp <= fp < sp + 65536:
                            raise ValueError('Frame exceeds bounded stack window')
                        next_fp, lr = struct.unpack('<2Q', user_record(fp))
                        item['user_frames'].append(dict(fp=hex(fp), lr=hex(lr)))
                        if next_fp and next_fp <= fp:
                            raise ValueError('Non-increasing user frame chain')
                        fp = next_fp
                    item['user_stack_complete'] = fp == 0
                except ValueError as error:
                    item['stack_error'] = str(error)
    finally:
        if packet('Qqemu.PhyMemMode:' + old_mode) != 'OK':
            raise RuntimeError('Cannot restore debugger virtual-memory mode')
    return dict(native_sha256=plan['native_sha256'], read_only=True,
                thread_snapshot_layout=dict(layout),
                thread_snapshot_layout_objects_sha256={
                    'thread.o': common.get('thread_object_sha256'),
                    'pcb.o': common.get('pcb_object_sha256'),
                },
                kernel_stack_size_symbol=hex(stack_size_address),
                kernel_stack_size_bytes=kernel_stack_size, kernel_stack_size_error=kernel_stack_size_error,
                kernel_context_process_limit=1 + int(include_kernel_task_first_context),
                kernel_context_frames_per_thread_limit=KERNEL_FRAME_LIMIT,
                kernel_context_scope=('candidate launchd context plus at most the first task-owned PID 0 '
                                      'kernel_task thread when explicitly opted in; LR/FP do not establish a wait cause'),
                kernel_task_threads_included=include_kernel_task,
                kernel_task_first_context_included=include_kernel_task_first_context,
                process_limit=16, threads_per_process_limit=64, frames_per_thread_limit=32,
                stack_window_bytes=65536, stack_source='actual-native-4k-task-tables',
                selected_processes=len(selected), processes=results)
