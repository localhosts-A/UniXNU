from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/qemu-entry'))
from gdb_stop_support import (finish_stop_action, matches_sysctl_return_mib,
                              parse_indirect_memory_request,
                              parse_syscall_return_mib_request,
                              parse_trace_register_memory_request,
                              validate_commpage_user64_shifts)


class SyscallReturnMibTests(unittest.TestCase):
    def test_parses_bounded_mib_bytes_and_matches_only_exact_sysctl(self):
        mib = parse_syscall_return_mib_request('65000000650000007600000064000000')
        self.assertEqual(mib, '65000000650000007600000064000000')
        self.assertTrue(matches_sysctl_return_mib(202, mib, mib))
        self.assertFalse(matches_sysctl_return_mib(5, mib, mib))
        self.assertFalse(matches_sysctl_return_mib(202, '0000000003000000', mib))

    def test_normalizes_case_and_rejects_partial_or_non_hex_integers(self):
        self.assertEqual(parse_syscall_return_mib_request('65000000'), '65000000')
        for value in ('', '6500000', '650000000', 'gg000000', '00000000' * 17):
            with self.subTest(value=value), self.assertRaises(ValueError):
                parse_syscall_return_mib_request(value)


class CommpageShiftTests(unittest.TestCase):
    def test_requires_both_actual_4k_commpage_shifts(self):
        requests = {'rw': 'rw', 'ro': 'ro'}
        capture = {'rw': {'bytes': '0c'}, 'ro': {'bytes': '0c'}}
        self.assertEqual(validate_commpage_user64_shifts(capture, requests), {'rw': 12, 'ro': 12})

    def test_unavailable_or_wrong_shifts_remain_a_strict_failure(self):
        requests = {'rw': 'rw', 'ro': 'ro'}
        cases = (
            {'rw': {'unavailable': 'cannot read pointer field'}, 'ro': {'bytes': '0c'}},
            {'rw': {'bytes': '10'}, 'ro': {'bytes': '0c'}},
            {'rw': {'bytes': '0c'}},
        )
        for capture in cases:
            with self.subTest(capture=capture), self.assertRaisesRegex(ValueError, 'Incorrect native user ABI'):
                validate_commpage_user64_shifts(capture, requests)

    def test_probe_uses_optional_gdb_reads_but_keeps_the_shift_gate(self):
        source = (ROOT / 'scripts/probe-darwinboot-xnu.py').read_text()
        self.assertIn("gdb_command.extend(['--read-indirect-optional', request])", source)
        self.assertIn('validate_commpage_user64_shifts(', source)


class FinishStopActionTests(unittest.TestCase):
    def test_tracepoint_is_not_treated_as_an_unexpected_stop(self):
        self.assertEqual(finish_stop_action(0x2000, 0x9000, 0x3000, 0x8000, {0x2000}), 'tracepoint')

    def test_matching_stack_return_completes_the_function(self):
        self.assertEqual(finish_stop_action(0x3000, 0x8000, 0x3000, 0x8000, {0x3000}), 'function-return')

    def test_other_thread_return_is_stepped_over_before_tracepoints(self):
        self.assertEqual(finish_stop_action(0x3000, 0x7000, 0x3000, 0x8000, {0x3000}), 'other-stack-return')

    def test_unrelated_pc_remains_a_failure(self):
        self.assertEqual(finish_stop_action(0x4000, 0x9000, 0x3000, 0x8000, {0x2000}), 'unexpected-stop')


class IndirectMemoryRequestTests(unittest.TestCase):
    def test_parses_bounded_pointer_field_read(self):
        self.assertEqual(parse_indirect_memory_request('fffffff006add1e8:10:10'),
                         (0xfffffff006add1e8, 0x10, 0x10))

    def test_rejects_malformed_and_unbounded_requests(self):
        for request in ('1:2', '-1:0:8', 'ffffffffffffffff:0:8', '1:0:0', '1:0:1001'):
            with self.subTest(request=request), self.assertRaises(ValueError):
                parse_indirect_memory_request(request)


class TraceRegisterMemoryRequestTests(unittest.TestCase):
    def test_parses_bounded_register_relative_read(self):
        self.assertEqual(parse_trace_register_memory_request(
            'fffffff004aaeb40:x0:88:8'),
            (0xfffffff004aaeb40, 0, 0x88, 8))

    def test_rejects_malformed_register_and_unbounded_requests(self):
        for request in ('1:x0:88', '1:x31:0:8', '1:sp:0:8',
                        '3:x0:0:8', '1:x0:0:0', '1:x0:0:1001'):
            with self.subTest(request=request), self.assertRaises(ValueError):
                parse_trace_register_memory_request(request)

    def test_rejects_address_and_target_range_overflow(self):
        for request in ('ffffffffffffffff:x0:0:8',
                        '1:x0:ffffffffffffffff:2'):
            with self.subTest(request=request), self.assertRaises(ValueError):
                parse_trace_register_memory_request(request)

    def test_rejects_pointer_and_target_range_overflow(self):
        for request in ('fffffffffffffff9:0:8', '1:ffffffffffffffff:2'):
            with self.subTest(request=request), self.assertRaises(ValueError):
                parse_indirect_memory_request(request)


class TraceRegisterSnapshotTests(unittest.TestCase):
    def test_trace_events_capture_x8_for_branch_diagnostics(self):
        source = (ROOT / 'scripts/qemu-entry/gdb-stop.py').read_text()
        self.assertIn("'x8': hex(current[8])", source)

    def test_syscall_return_observer_is_narrow_and_does_not_read_output_bytes(self):
        stop_source = (ROOT / 'scripts/qemu-entry/gdb-stop.py').read_text()
        probe_source = (ROOT / 'scripts/probe-darwinboot-xnu.py').read_text()
        self.assertIn("gdb_command += ['--syscall-return-mib', args.log_syscall_return_mib]", probe_source)
        self.assertIn('syscall_return_checkpoint=syscall_return_event', stop_source)
        self.assertIn("'old_length_after'", stop_source)
        self.assertIn('syscall_error_flag=', stop_source)
        self.assertNotIn('sysctl_output_bytes', stop_source)


if __name__ == '__main__':
    unittest.main()
