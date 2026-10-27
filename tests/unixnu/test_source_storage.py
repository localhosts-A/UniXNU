import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location('source_storage', Path(__file__).resolve().parents[2] / 'scripts/build-source-storage.py')
storage = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(storage)


class SourceStorageBuildTests(unittest.TestCase):
    def test_bsd_media_name_uses_the_iokit_bsd_header(self):
        fixture = (Path(__file__).resolve().parents[2] / 'scripts/fixtures/source-storage.cpp').read_text()
        self.assertIn('#include <IOKit/IOBSD.h>', fixture)
        self.assertIn('media->getProperty(kIOBSDNameKey)', fixture)

    def test_compile_record_keeps_kernel_flags_without_clobbering_kernel_outputs(self):
        record = {'directory': '/build/xnu', 'file': 'IOService.cpp',
                  'arguments': ['clang++', '-target', 'arm64-apple-ios17.0', '-DKERNEL_PRIVATE=1',
                                '-include', 'config.h', '-I', 'EXPORT_HDRS', '-isystem', '/kernel/sdk',
                                '-c', 'IOService.cpp', '-o', 'IOService.cpo', '-MD', '-MF', 'IOService.d',
                                '-MJ', 'IOService.cpo.json', '-serialize-diagnostics', 'IOService.dia']}
        result = storage.compile_command(record, Path('/storage/IOStorage.cpp'), Path('/out/storage.o'), [Path('/generated')])
        self.assertIn('-DKERNEL_PRIVATE=1', result)
        self.assertIn('/storage/IOStorage.cpp', result)
        self.assertIn('config.h', result)
        self.assertNotIn('IOService.cpp', result)
        for path in ('IOService.cpo', 'IOService.d', 'IOService.cpo.json', 'IOService.dia'):
            self.assertNotIn(path, result)
        self.assertEqual(result[result.index('-o') + 1], '/out/storage.o')
        self.assertEqual(storage.include_flags(record), ['-I', 'EXPORT_HDRS', '-isystem', '/kernel/sdk'])

    def test_missing_duplicate_or_truncated_record_refuses_to_compile(self):
        for args in (['clang++', '-c', 'Other.cpp'], ['clang++', 'Source.cpp', 'Source.cpp'],
                     ['clang++', 'Source.cpp', '-o']):
            record = {'directory': '/build', 'file': 'Source.cpp', 'arguments': args}
            with self.subTest(args=args), self.assertRaises(ValueError):
                storage.compile_command(record, Path('/storage.cpp'), Path('/out.o'), [])

    def object(self, *, platform=2, minimum=0x110000, cpu=0x100000c):
        return struct.pack('<8I', 0xfeedfacf, cpu, 0, 1, 1, 24, 0, 0) + struct.pack('<6I', 0x32, 24, platform, minimum, 0, 0)

    def test_object_requires_real_arm64_ios17_abi(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / 'object.o'
            path.write_bytes(self.object())
            storage.validate_object(path)
            for data in (self.object(platform=1), self.object(minimum=0x120000), self.object(cpu=0x1000007),
                         self.object()[:-1], b'', self.object()[:32]):
                path.write_bytes(data)
                with self.subTest(data=data[:20]), self.assertRaises(ValueError):
                    storage.validate_object(path)

    def test_compiler_record_prefers_iokit_cpp_and_refuses_an_absent_bootstrap(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            with self.assertRaisesRegex(ValueError, 'bootstrap'):
                storage.compiler_record(root)
            import json
            record = {'directory': str(root), 'file': 'IOService.cpp', 'arguments': ['clang++', '-c', 'IOService.cpp']}
            path = root / 'iokit/DEVELOPMENT/IOService.cpo.json'
            path.parent.mkdir(parents=True)
            path.write_text(json.dumps(record) + ',\n')
            self.assertEqual(storage.compiler_record(root), (path, record))


if __name__ == '__main__':
    unittest.main()
