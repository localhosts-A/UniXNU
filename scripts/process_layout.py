"""Compiler-qualified XNU process and thread layout extraction."""
import hashlib
from pathlib import Path
import re
import subprocess


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
    else:
        raise SystemExit('Unsupported compiler process layout type: ' + structure)
    obj = objects / relative
    output = subprocess.check_output(
        ['xcrun', 'dwarfdump', '--name=' + structure, '--show-children', obj], text=True)
    match = re.search(r'DW_AT_name\s+\("' + re.escape(name) +
                      r'"\)[\s\S]*?DW_AT_data_member_location\s+\((0x[0-9a-f]+)\)', output)
    if not match:
        raise SystemExit('Cannot derive compiler layout: ' + structure + '.' + name)
    return int(match[1], 16)


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
    process.update(pmap_object_sha256=hashlib.sha256(pmap_object.read_bytes()).hexdigest())

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
        'process_activation': process_activation,
    }
