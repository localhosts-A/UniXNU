import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from process_layout import build_process_layout, member, symbol_address


class ProcessLayoutTests(unittest.TestCase):
    def test_member_uses_compiler_record_and_rejects_missing_fields(self):
        objects = Path('/tmp/process-layout-fixture')
        output = 'DW_AT_name ("p_pid")\nDW_AT_data_member_location (0x60)'
        with patch('process_layout.subprocess.check_output', return_value=output) as run:
            self.assertEqual(member(objects, 'proc', 'p_pid'), 0x60)
        self.assertTrue(str(run.call_args.args[0][-1]).endswith('bsd/DEVELOPMENT/kern_proc.o'))
        with patch('process_layout.subprocess.check_output', return_value=''), \
                self.assertRaisesRegex(SystemExit, 'Cannot derive compiler layout: proc.p_pid'):
            member(objects, 'proc', 'p_pid')

    def test_builds_native_hash_bound_thread_layout_from_exact_objects(self):
        fields = {
            'thread': {'t_tro': 0x10, 'machine': 0x20, 'task_threads': 0x30, 'thread_id': 0x40,
                       'continuation': 0x50, 'wait_event': 0x60, 'state': 0x70},
            'machine_thread': {'upcb': 0x80},
            'thread_ro': {'tro_owner': 0x8, 'tro_proc': 0x10, 'tro_task': 0x18},
            'proc': {'p_pid': 0x60, 'p_proc_ro': 0x70},
            'proc_ro': {'pr_proc': 0x8, 'pr_task': 0x10},
            'task': {'shared_region': 0x18, 'threads': 0x20, 'thread_count': 0x28, 'map': 0x30},
            'vm_shared_region': {'sr_slide': 0x8},
            '_vm_map': {'pmap': 0x10},
            'pmap': {'ttep': 0x18},
            'page_table_attr': {'pta_root_level': 0x4, 'pta_page_shift': 0x8},
        }
        relatives = (
            'osfmk/DEVELOPMENT/thread.o', 'bsd/DEVELOPMENT/kern_proc.o',
            'osfmk/DEVELOPMENT/task.o', 'osfmk/DEVELOPMENT/vm_shared_region.o',
            'osfmk/DEVELOPMENT/pmap.o', 'mach.development.bcm2837.unstripped.noctf',
        )
        with tempfile.TemporaryDirectory() as directory:
            objects = Path(directory) / 'ios-obj' / 'DEVELOPMENT_ARM64_BCM2837'
            for index, relative in enumerate(relatives):
                path = objects / relative
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(f'fixture-{index}'.encode())
            native_hash = hashlib.sha256((objects / relatives[-1]).read_bytes()).hexdigest()
            calls = []

            def check_output(command, **kwargs):
                parts = [str(part) for part in command]
                calls.append(parts)
                if parts[1] == 'dwarfdump':
                    type_name = parts[2].split('=', 1)[1]
                    return '\n'.join(
                        f'DW_AT_name ("{name}")\nDW_AT_data_member_location (0x{offset:x})'
                        for name, offset in fields[type_name].items())
                if parts[1] == 'llvm-objdump':
                    return 'add x0, x0, #0x5bc\n'
                if parts[1] == 'nm':
                    return ('fffffff007001000 S _native_pt_attr\n'
                            'fffffff007002000 T _shared_region_check_np\n'
                            'fffffff007003000 T _shared_region_map_and_slide_2_np\n')
                raise AssertionError(parts)

            with patch('process_layout.subprocess.check_output', side_effect=check_output):
                plan = build_process_layout(objects)

        self.assertEqual(plan['native_sha256'], native_hash)
        self.assertEqual(plan['process_layout']['pid'], 0x60)
        self.assertEqual(plan['process_layout']['thread_user_state'], 0xa0)
        self.assertEqual(plan['process_layout']['comm'], 0x5bc)
        self.assertEqual(plan['thread_snapshot_layout']['thread_state'], 0x70)
        self.assertEqual(plan['thread_snapshot_native_pt_attr'], 0xfffffff007001000)
        self.assertEqual(plan['process_activation'], [0xfffffff007002000, 0xfffffff007003000])
        vm_map_call = next(call for call in calls if call[1] == 'dwarfdump' and call[2] == '--name=_vm_map')
        self.assertTrue(vm_map_call[-1].endswith('/osfmk/DEVELOPMENT/task.o'))

    def test_probe_uses_compiler_only_layout_plan_and_preserves_stop_on_snapshot_error(self):
        probe = (ROOT / 'scripts/probe-darwinboot-xnu.py').read_text()
        stop = (ROOT / 'scripts/qemu-entry/gdb-stop.py').read_text()
        layout = (ROOT / 'scripts/process_layout.py').read_text()
        display = (ROOT / 'scripts/audit-target-display-query.py').read_text()
        self.assertIn('scripts/audit-target-process-layout.py', probe)
        self.assertIn("process_plan = ROOT / 'build/process-analysis/process-layout-plan.json'", probe)
        self.assertIn('"pmap_pt_attr"', layout)
        self.assertIn('plan.update(build_process_layout(', display)
        self.assertIn("report['ui_thread_snapshots'] = dict(read_only=True, error=str(error))", stop)

    def test_symbol_resolution_requires_one_exact_compiled_definition(self):
        self.assertEqual(symbol_address('ffff S _target\n', '_target'), 0xffff)
        for symbols in ('', 'ffff S _target\n1000 T _target\n'):
            with self.subTest(symbols=symbols), self.assertRaisesRegex(SystemExit, 'Cannot uniquely resolve'):
                symbol_address(symbols, '_target')


if __name__ == '__main__':
    unittest.main()
