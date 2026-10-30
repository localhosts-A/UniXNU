import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts/qemu-entry'))
from thread_snapshot import capture_kernel_context


class Memory:
    def __init__(self):
        self.bytes = {}
        self.reads = []

    def write(self, address, value):
        for offset, byte in enumerate(value):
            self.bytes[address + offset] = byte

    def word(self, address, value):
        self.write(address, struct.pack('<Q', value))

    def read(self, address, size):
        self.reads.append((address, size))
        try:
            return bytes(self.bytes[address + offset] for offset in range(size))
        except KeyError as error:
            raise ValueError('unmapped synthetic kernel memory') from error


class ThreadSnapshotTests(unittest.TestCase):
    base = 0xfffffff100000000
    thread = 0xfffffff200000000
    stack_size = 0x4000
    state_size = 0x90

    @classmethod
    def fixture(cls, frame_count=2):
        memory = Memory()
        state = cls.base + cls.stack_size - cls.state_size
        fp = cls.base + 0x300
        memory.word(cls.thread + 0x10, cls.base)
        memory.word(cls.thread + 0x20, state)
        memory.word(state + 0x50, fp)
        memory.word(state + 0x58, 0xfffffff000002000)
        memory.word(state + 0x60, cls.base + 0x280)
        for index in range(frame_count):
            current = fp + index * 16
            next_fp = current + 16 if index + 1 < frame_count else 0
            memory.write(current, struct.pack('<2Q', next_fp, 0xfffffff000003000 + index * 4))
        layout = {
            'thread_kernel_stack': 0x10, 'thread_kstackptr': 0x20,
            'thread_kernel_state_machine': 0, 'thread_kernel_state_size': cls.state_size,
            'kernel_context_ss': 0,
            'kernel_saved_state_fp': 0x50, 'kernel_saved_state_lr': 0x58, 'kernel_saved_state_sp': 0x60,
        }
        return memory, layout, state, fp

    def test_reads_only_compiler_qualified_state_and_bounded_frame_chain(self):
        memory, layout, state, fp = self.fixture()
        result = capture_kernel_context(memory.read, self.thread, layout, self.stack_size)
        self.assertEqual(result['state_address'], hex(state))
        self.assertEqual(result['saved_fp'], hex(fp))
        self.assertEqual(result['saved_sp'], hex(self.base + 0x280))
        self.assertEqual(result['saved_lr'], '0xfffffff000002000')
        self.assertFalse(result['saved_pc_available'])
        self.assertTrue(result['frames_complete'])
        self.assertEqual(len(result['frames']), 2)
        self.assertTrue(all(size in (8, 16) for _, size in memory.reads))

    def test_rejects_signed_or_out_of_range_kstackptr_before_context_read(self):
        memory, layout, state, _ = self.fixture()
        memory.word(self.thread + 0x20, state ^ 0x0100000000000000)
        with self.assertRaisesRegex(ValueError, 'Signed or out-of-range'):
            capture_kernel_context(memory.read, self.thread, layout, self.stack_size)
        self.assertEqual(memory.reads, [(self.thread + 0x10, 8), (self.thread + 0x20, 8)])

    def test_rejects_saved_sp_outside_usable_stack(self):
        memory, layout, state, _ = self.fixture()
        memory.word(state + 0x60, self.base - 16)
        with self.assertRaisesRegex(ValueError, 'Saved kernel SP'):
            capture_kernel_context(memory.read, self.thread, layout, self.stack_size)

    def test_invalid_frame_chain_is_reported_without_unbounded_reads(self):
        memory, layout, _, fp = self.fixture(frame_count=1)
        memory.write(fp, struct.pack('<2Q', fp, 0xfffffff000003000))
        result = capture_kernel_context(memory.read, self.thread, layout, self.stack_size)
        self.assertEqual(len(result['frames']), 1)
        self.assertFalse(result['frames_complete'])
        self.assertIn('not increasing', result['frames_error'])

    def test_frame_chain_has_a_fixed_read_limit(self):
        memory, layout, _, _ = self.fixture(frame_count=20)
        result = capture_kernel_context(memory.read, self.thread, layout, self.stack_size)
        self.assertEqual(len(result['frames']), 16)
        self.assertFalse(result['frames_complete'])
        self.assertIn('frame count', result['frames_error'])
        self.assertEqual(sum(size == 16 for _, size in memory.reads), 16)
        self.assertLessEqual(len(memory.reads), 21)

    def test_rejects_non_page_sized_runtime_stack(self):
        memory, layout, _, _ = self.fixture()
        with self.assertRaisesRegex(ValueError, 'runtime kernel stack size'):
            capture_kernel_context(memory.read, self.thread, layout, 0x4001)


if __name__ == '__main__':
    unittest.main()
