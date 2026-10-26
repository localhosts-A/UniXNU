"""Exercise production SHA/ADT parser/measurement, without firmware substitutes."""
import ctypes
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest


def node(name, properties=(), children=()):
    props = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(props), len(children))
    for key, value in props:
        result += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return result + b''.join(children)


class KernelMeasurementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which('clang') or shutil.which('cc')
        if not compiler:
            raise unittest.SkipTest('native C compiler required')
        root = Path(__file__).resolve().parents[2]
        driver = root / 'drivers/DarwinBoot/DarwinBootPkg/Driver'
        cls.temp = tempfile.TemporaryDirectory()
        base = Path(cls.temp.name)
        (base / 'Library').mkdir()
        (base / 'Uefi.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef uint8_t UINT8; typedef uint32_t UINT32; typedef uint64_t UINT64;
typedef size_t UINTN; typedef uint64_t EFI_STATUS; typedef int BOOLEAN; typedef char CHAR8;
#define STATIC static
#define CONST const
#define VOID void
#define TRUE 1
#define FALSE 0
#define SIZE_128MB 0x8000000
#define MAX_UINTN SIZE_MAX
#define EFI_SUCCESS 0
#define EFI_BAD_BUFFER_SIZE (0x8000000000000000ULL | 4)
#define EFI_COMPROMISED_DATA (0x8000000000000000ULL | 33)
#define ALIGN_VALUE(a,b) (((a)+(b)-1)&~((b)-1))
#define AsciiStrCmp strcmp
#define CompareMem memcmp
''')
        (base / 'Library/BaseMemoryLib.h').write_text('''#include <Uefi.h>
#define ZeroMem(p,n) memset(p,0,n)
#define CopyMem memcpy
''')
        source = (driver / 'DarwinBoot.c').read_text()
        start = source.index('STATIC\nBOOLEAN\nAdtNode (')
        end = source.index('\nSTATIC\nBOOLEAN\nValidTrustCaches', start)
        (base / 'parser.c').write_text('''#include <Uefi.h>
#include "KernelMeasurement.h"
#include "WindowSize.h"
static const void *ScanMem8(const void *p,size_t n,int v){return memchr(p,v,n);}
static UINT32 U32(const void *p){UINT32 v;memcpy(&v,p,4);return v;}
static UINT64 U64(const void *p){UINT64 v;memcpy(&v,p,8);return v;}
''' + source[start:end] + '''
int parse(const void *data,size_t bytes,size_t *hash) {
  UINTN at=0,r=MAX_UINTN,t=MAX_UINTN,a=MAX_UINTN;
  UINT64 window=0; BOOLEAN packed=FALSE; *hash=MAX_UINTN;
  return AdtNode(data,bytes,&at,0,3,&r,&t,&a,&window,&packed,hash) && at==bytes;
}
''')
        subprocess.run([compiler, '-shared', '-fPIC', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-I', str(base), '-I', str(driver), str(driver / 'KernelMeasurement.c'),
                        str(base / 'parser.c'), '-o', str(base / 'measurement.so')], check=True)
        cls.lib = ctypes.CDLL(str(base / 'measurement.so'))
        cls.lib.DarwinKernelSha256.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.DarwinKernelSha256.restype = ctypes.c_int
        cls.lib.DarwinMeasureKernel.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_size_t,
                                               ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.DarwinMeasureKernel.restype = ctypes.c_uint64
        cls.lib.parse.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        cls.lib.parse.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_sha256_padding_blocks_and_unaligned_inputs(self):
        for length in (1, 3, 55, 56, 57, 63, 64, 65, 119, 120, 127, 128, 129, 4096, 1048576):
            for skew in (0, 1):
                with self.subTest(length=length, skew=skew):
                    value = (bytes(range(251)) * (length // 251 + 1))[:length]
                    memory = ctypes.create_string_buffer(b'x' * skew + value)
                    result = ctypes.create_string_buffer(32)
                    self.assertEqual(self.lib.DarwinKernelSha256(ctypes.addressof(memory)+skew, length, result), 1)
                    self.assertEqual(result.raw, hashlib.sha256(value).digest())
        memory = ctypes.create_string_buffer(b'a' * 1000000)
        result = ctypes.create_string_buffer(32)
        self.assertEqual(self.lib.DarwinKernelSha256(memory, 1000000, result), 1)
        self.assertEqual(result.raw.hex(), 'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0')

    def test_hash_invalid_input_clears_output_without_dereference(self):
        for pointer, length in ((None, 3), (1, 0), (1, 0x8000001), (1, ctypes.c_size_t(-1).value)):
            result = ctypes.create_string_buffer(b'x'*32, 32)
            self.assertEqual(self.lib.DarwinKernelSha256(pointer, length, result), 0)
            self.assertEqual(result.raw, bytes(32))
        self.assertEqual(self.lib.DarwinKernelSha256(None, 0, None), 0)

    def parse(self, data):
        offset = ctypes.c_size_t()
        accepted = self.lib.parse(ctypes.create_string_buffer(data), len(data), ctypes.byref(offset))
        return accepted, offset.value

    def test_unique_zero_slot_and_legacy_absence(self):
        tree = node('device-tree', children=[node('chosen', [('darwinboot-kernel-sha256', bytes(32))])])
        accepted, offset = self.parse(tree)
        self.assertEqual(accepted, 1)
        self.assertEqual(tree[offset:offset+32], bytes(32))
        tree = node('device-tree', children=[node('chosen')])
        self.assertEqual(self.parse(tree), (1, ctypes.c_size_t(-1).value))
        self.assertEqual(self.lib.DarwinMeasureKernel(None, 0, ctypes.c_size_t(-1).value, None, 0), 0)

    def test_forged_duplicate_misplaced_and_truncated_slots(self):
        key = 'darwinboot-kernel-sha256'
        slot = [(key, bytes(32))]
        invalid = [node('device-tree', children=[node('chosen', [(key, value)])])
                   for value in (bytes(31), bytes(33), b'\x01'+bytes(31), bytes(31)+b'\x01')]
        invalid += [node('device-tree', children=[node('chosen', slot*2)]),
                    node('device-tree', slot, [node('chosen')]),
                    node('device-tree', children=[node('chosen', children=[node('memory-map', slot)])]),
                    node('device-tree', children=[node('other', slot)]),
                    node('device-tree', children=[node('chosen', slot)]*2)]
        for tree in invalid:
            self.assertEqual(self.parse(tree)[0], 0)
        tree = node('device-tree', children=[node('chosen', slot)])
        for size in range(len(tree)):
            self.assertEqual(self.parse(tree[:size])[0], 0, size)
        self.assertEqual(self.parse(tree + b'\0')[0], 0)

    def test_measurement_covers_whole_original_file_and_only_changes_slot(self):
        tree = node('device-tree', children=[node('chosen', [('darwinboot-kernel-sha256', bytes(32))])])
        _, offset = self.parse(tree)
        image = b'loaded-segment'*40 + b'UNLOADED TRAILER'
        memory = ctypes.create_string_buffer(tree, len(tree))
        self.assertEqual(self.lib.DarwinMeasureKernel(memory, len(tree), offset,
                                                     ctypes.create_string_buffer(image), len(image)), 0)
        expected = tree[:offset] + hashlib.sha256(image).digest() + tree[offset+32:]
        self.assertEqual(memory.raw, expected)
        self.assertEqual(self.lib.DarwinMeasureKernel(memory, len(tree), offset,
                                                     ctypes.create_string_buffer(image), len(image)), (1<<63)|33)
        self.assertEqual(memory.raw, expected)

    def test_bad_measurement_bounds_and_input_never_fill_a_slot(self):
        memory = ctypes.create_string_buffer(bytes(64), 64)
        for offset, error in ((33,33), (65,33), (0,4)):
            self.assertEqual(self.lib.DarwinMeasureKernel(memory, 64, offset, None, 3), (1<<63)|error)
            self.assertEqual(memory.raw, bytes(64))


if __name__ == '__main__':
    unittest.main()
