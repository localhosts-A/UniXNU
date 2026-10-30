"""Compiler-qualified XNU process and thread layout extraction."""
import hashlib
from pathlib import Path
import re
import subprocess


def dwarf_blocks(output):
    headers = list(re.finditer(
        r'(?m)^[ \t]*0x[0-9a-fA-F]+:([ \t]*)(DW_TAG_[A-Za-z_]+)', output))
    return [(match[2], len(match[1]), output[match.start():headers[index + 1].start()
             if index + 1 < len(headers) else len(output)])
            for index, match in enumerate(headers)]


def has_name(block, name):
    return re.search(r'DW_AT_name\s+\("' + re.escape(name) + r'"\)', block) is not None


def member(objects, structure, name):
    objects = Path(objects)
    if structure in ('thread', 'thread_ro', 'machine_thread'):
        relative = 'osfmk/DEVELOPMENT/thread.o'
    elif structure in ('proc', 'proc_ro'):
        relative = 'bsd/DEVELOPMENT/kern_proc.o'
    elif structure == 'task':
        relative = 'osfmk/DEVELOPMENT/task.o'
    elif structure == 'vm_shared_region':
        relative = 'osfmk/DEVELOPMENT/vm_shared_region.o'
    elif structure in ('pmap', 'page_table_attr'):
        relative = 'osfmk/DEVELOPMENT/pmap.o'
    elif structure == '_vm_map':
        relative = 'osfmk/DEVELOPMENT/task.o'
    elif structure in ('thread_kernel_state', 'arm_kernel_context'):
        relative = 'osfmk/DEVELOPMENT/pcb.o'
    else:
        raise SystemExit('Unsupported compiler process layout type: ' + structure)
    obj = objects / relative
    output = subprocess.check_output(
        ['xcrun', 'dwarfdump', '--name=' + structure, '--show-children', obj], text=True)
    blocks = dwarf_blocks(output)
    targets = []
    for index, (tag, depth, block) in enumerate(blocks):
        if tag != 'DW_TAG_member' or not has_name(block, name):
            continue
        parent = next((parent for parent in range(index - 1, -1, -1)
                       if blocks[parent][0] in ('DW_TAG_structure_type', 'DW_TAG_union_type') and
                       blocks[parent][1] < depth), None)
        if parent is not None and has_name(blocks[parent][2], structure):
            targets.append((index, parent, block))
    if not targets:
        raise SystemExit('Cannot derive compiler layout: ' + structure + '.' + name)

    offsets = []
    for index, parent, block in targets:
        match = re.search(r'DW_AT_data_member_location\s+\((0x[0-9a-fA-F]+|[0-9]+)\)', block)
        if match:
            offsets.append(int(match[1], 0))
            continue
        if structure == 'thread_kernel_state' and name == 'machine':
            parent_depth = blocks[parent][1]
            first_member = next((child for child in range(parent + 1, len(blocks))
                                 if blocks[child][1] <= parent_depth or
                                 (blocks[child][0] == 'DW_TAG_member' and blocks[child][1] == depth)), None)
            if first_member == index:
                offsets.append(0)
                continue
        raise SystemExit('Cannot derive compiler member location: ' + structure + '.' + name)
    if len(set(offsets)) != 1:
        raise SystemExit('Ambiguous compiler layout: ' + structure + '.' + name)
    return offsets[0]


def type_size(objects, structure):
    objects = Path(objects)
    if structure not in ('thread_kernel_state', 'arm_kernel_context'):
        raise SystemExit('Unsupported compiler process layout type size: ' + structure)
    obj = objects / 'osfmk/DEVELOPMENT/pcb.o'
    output = subprocess.check_output(
        ['xcrun', 'dwarfdump', '--name=' + structure, '--show-children', obj], text=True)
    sizes = []
    for tag, _, block in dwarf_blocks(output):
        if tag not in ('DW_TAG_structure_type', 'DW_TAG_union_type') or not has_name(block, structure):
            continue
        match = re.search(r'DW_AT_byte_size\s+\((0x[0-9a-fA-F]+|[0-9]+)\)', block)
        if match:
            sizes.append(int(match[1], 0))
    if not sizes or len(set(sizes)) != 1:
        raise SystemExit('Cannot uniquely derive compiler layout size: ' + structure)
    size = sizes[0]
    if not 0 < size < 65536:
        raise SystemExit('Invalid compiler layout size: ' + structure)
    return size


def symbol_address(symbols, name):
    matches = re.findall(r'^([0-9a-fA-F]+)\s+\S\s+' + re.escape(name) + '$', symbols, re.M)
    if len(matches) != 1:
        raise SystemExit('Cannot uniquely resolve compiled process-layout symbol: ' + name)
    return int(matches[0], 16)


def build_process_layout(objects):
    objects = Path(objects).resolve()
    thread_object = objects / 'osfmk/DEVELOPMENT/thread.o'
    proc_object = objects / 'bsd/DEVELOPMENT/kern_proc.o'
    task_object = objects / 'osfmk/DEVELOPMENT/task.o'
    region_object = objects / 'osfmk/DEVELOPMENT/vm_shared_region.o'
    pmap_object = objects / 'osfmk/DEVELOPMENT/pmap.o'
    pcb_object = objects / 'osfmk/DEVELOPMENT/pcb.o'
    native_path = objects / 'mach.development.bcm2837.unstripped.noctf'
    process = {
        'thread_ro': member(objects, 'thread', 't_tro'),
        'thread_user_state': member(objects, 'thread', 'machine') + member(objects, 'machine_thread', 'upcb'),
        'ro_owner': member(objects, 'thread_ro', 'tro_owner'),
        'ro_proc': member(objects, 'thread_ro', 'tro_proc'),
        'pid': member(objects, 'proc', 'p_pid'),
        'comm': None,
        'thread_object_sha256': hashlib.sha256(thread_object.read_bytes()).hexdigest(),
        'proc_object_sha256': hashlib.sha256(proc_object.read_bytes()).hexdigest(),
        'ro_task': member(objects, 'thread_ro', 'tro_task'),
        'task_region': member(objects, 'task', 'shared_region'),
        'region_slide': member(objects, 'vm_shared_region', 'sr_slide'),
        'task_object_sha256': hashlib.sha256(task_object.read_bytes()).hexdigest(),
        'region_object_sha256': hashlib.sha256(region_object.read_bytes()).hexdigest(),
    }
    disassembly = subprocess.check_output(
        ['xcrun', 'llvm-objdump', '--disassemble',
         '--disassemble-symbols=_proc_name_address', proc_object], text=True)
    comm = re.search(r'add\s+x0, x0, #(0x[0-9a-f]+)', disassembly)
    if not comm:
        raise SystemExit('Cannot derive actual process-name address from compiled accessor')
    process['comm'] = int(comm[1], 16)
    process.update(pmap_object_sha256=hashlib.sha256(pmap_object.read_bytes()).hexdigest(),
                   pcb_object_sha256=hashlib.sha256(pcb_object.read_bytes()).hexdigest())

    thread = {
        'proc_ro': member(objects, 'proc', 'p_proc_ro'),
        'proc_ro_owner': member(objects, 'proc_ro', 'pr_proc'),
        'proc_ro_task': member(objects, 'proc_ro', 'pr_task'),
        'task_threads': member(objects, 'task', 'threads'),
        'task_thread_count': member(objects, 'task', 'thread_count'),
        'thread_link': member(objects, 'thread', 'task_threads'),
        'thread_id': member(objects, 'thread', 'thread_id'),
        'continuation': member(objects, 'thread', 'continuation'),
        'wait_event': member(objects, 'thread', 'wait_event'),
        'thread_kernel_stack': member(objects, 'thread', 'kernel_stack'),
        'thread_kstackptr': member(objects, 'thread', 'machine') +
                            member(objects, 'machine_thread', 'kstackptr'),
        'thread_kernel_state_machine': member(objects, 'thread_kernel_state', 'machine'),
        'thread_kernel_state_size': type_size(objects, 'thread_kernel_state'),
        'kernel_context_fp': member(objects, 'arm_kernel_context', 'fp'),
        'kernel_context_lr': member(objects, 'arm_kernel_context', 'lr'),
        'kernel_context_sp': member(objects, 'arm_kernel_context', 'sp'),
        'thread_state': member(objects, 'thread', 'state'),
        'task_map': member(objects, 'task', 'map'),
        'map_pmap': member(objects, '_vm_map', 'pmap'),
        'pmap_ttep': member(objects, 'pmap', 'ttep'),
        'attr_root': member(objects, 'page_table_attr', 'pta_root_level'),
        'attr_shift': member(objects, 'page_table_attr', 'pta_page_shift'),
    }
    pmap_debug = subprocess.check_output(
        ['xcrun', 'dwarfdump', '--name=pmap', '--show-children', pmap_object], text=True)
    if '"pmap_pt_attr"' in pmap_debug:
        raise SystemExit('Thread stack observer requires the qualified non-parameterized pmap build')

    native_symbols = subprocess.check_output(['xcrun', 'nm', '-U', native_path], text=True)
    process_activation = [symbol_address(native_symbols, name) for name in
                          ('_shared_region_check_np', '_shared_region_map_and_slide_2_np')]
    return {
        'native_sha256': hashlib.sha256(native_path.read_bytes()).hexdigest(),
        'process_layout': process,
        'thread_snapshot_layout': thread,
        'thread_snapshot_native_pt_attr': symbol_address(native_symbols, '_native_pt_attr'),
        'thread_snapshot_kernel_stack_size': symbol_address(native_symbols, '_kernel_stack_size'),
        'process_activation': process_activation,
    }
