from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/qemu-entry'))
from gdb_stop_support import finish_stop_action


class FinishStopActionTests(unittest.TestCase):
    def test_tracepoint_is_not_treated_as_an_unexpected_stop(self):
        self.assertEqual(finish_stop_action(0x2000, 0x9000, 0x3000, 0x8000, {0x2000}), 'tracepoint')

    def test_matching_stack_return_completes_the_function(self):
        self.assertEqual(finish_stop_action(0x3000, 0x8000, 0x3000, 0x8000, {0x3000}), 'function-return')

    def test_other_thread_return_is_stepped_over_before_tracepoints(self):
        self.assertEqual(finish_stop_action(0x3000, 0x7000, 0x3000, 0x8000, {0x3000}), 'other-stack-return')

    def test_unrelated_pc_remains_a_failure(self):
        self.assertEqual(finish_stop_action(0x4000, 0x9000, 0x3000, 0x8000, {0x2000}), 'unexpected-stop')


if __name__ == '__main__':
    unittest.main()
