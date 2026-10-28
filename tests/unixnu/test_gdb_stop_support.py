from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/qemu-entry'))
from gdb_stop_support import finish_stop_action, parse_indirect_memory_request


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

    def test_rejects_pointer_and_target_range_overflow(self):
        for request in ('fffffffffffffff9:0:8', '1:ffffffffffffffff:2'):
            with self.subTest(request=request), self.assertRaises(ValueError):
                parse_indirect_memory_request(request)


if __name__ == '__main__':
    unittest.main()
