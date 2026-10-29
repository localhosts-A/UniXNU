import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
spec = importlib.util.spec_from_file_location(
    'build_native_virtio_qemu_source_storage', ROOT / 'scripts/build-native-virtio-qemu-source-storage.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
MODE, require_profile, source_variant = builder.MODE, builder.require_profile, builder.source_variant


class QemuSourceStorageVirtioProfileTests(unittest.TestCase):
    @staticmethod
    def profile(**overrides):
        values = {
            MODE: '1', 'XNU_NATIVE_VIRTIO_BLOCK': '1', 'XNU_NATIVE_AES': '0',
            'XNU_SOURCE_STORAGE_FAMILY': '1', 'XNU_SOURCE_CORECRYPTO14': '1',
            'XNU_CRYPTO_BOOTSTRAP': 'source', 'XNU_NATIVE_UFS': '0',
            'XNU_NATIVE_RAM_BLOCK': '0', 'XNU_VERIFIED_ROOT_PROVIDER': '1',
            'XNU_OWNER_ROOT_AUDIT': '1', 'XNU_OWNER_ROOT_TRUST': '1',
            'XNU_TRUST_CACHE_RUNTIME': '1', 'XNU_SOURCE_MOCKFS': '0',
            'XNU_SOURCE_ENTITLEMENT_FREE': '0',
        }
        values.update(overrides)
        return values

    def test_minimal_profile_is_accepted_without_dali_ufs_archive(self):
        require_profile(self.profile())
        with self.assertRaisesRegex(ValueError, 'Dali physical UFS'):
            require_profile(self.profile(XNU_EXTRA_LD_ARCHIVES='-force_load libDaliUfs.a'))

    def test_hardware_or_incomplete_profiles_are_rejected(self):
        invalid = [
            {'XNU_NATIVE_AES': '1'}, {'XNU_SOURCE_STORAGE_FAMILY': '0'},
            {'XNU_CRYPTO_BOOTSTRAP': 'prelinked'}, {'XNU_NATIVE_UFS': '1'},
            {'XNU_NATIVE_RAM_BLOCK': '1'}, {'XNU_OWNER_ROOT_TRUST': '0'},
            {'XNU_SOURCE_MOCKFS': '1'}, {'XNU_SOURCE_ENTITLEMENT_FREE': '1'},
            {'XNU_TRUST_CACHE_RUNTIME': '0'}, {MODE: '0'},
        ]
        for values in invalid:
            with self.subTest(values=values), self.assertRaisesRegex(ValueError, 'profile'):
                require_profile(self.profile(**values))

    def test_variant_uses_multi_policy_symbols_without_touching_pinned_input(self):
        source = ROOT / 'drivers/NativeVirtioBlock/NativeVirtioBlock.cpp'
        original = source.read_bytes()
        variant = source_variant(source)
        self.assertEqual(source.read_bytes(), original)
        text = variant.decode()
        self.assertIn('#include "VerifiedBlockPolicy.h"', text)
        self.assertIn('verified_root_policies[index]', text)
        self.assertIn('manifestBytes = 0;', text)
        self.assertIn('NATIVE_VERIFIED_BLOCK_MULTI_POLICY', text)
        self.assertIn('NATIVE_VERIFIED_BLOCK_QEMU_NO_UFS', text)
        single_declaration = text.index('extern const uint8_t verified_root_manifest[];')
        guard = text.rfind('#if !defined(NATIVE_VERIFIED_BLOCK_MULTI_POLICY)', 0, single_declaration)
        self.assertGreaterEqual(guard, 0)
        self.assertNotIn('QEMU_SOURCE_STORAGE', text)

    def test_builder_keeps_record_cwd_and_rejects_single_policy_imports(self):
        # Synthetic compiler/nm outputs test orchestration, never an XNU build.
        for single_policy, ufs_import in ((False, False), (True, False), (False, True)):
            with self.subTest(single_policy=single_policy, ufs_import=ufs_import), tempfile.TemporaryDirectory() as directory:
                tree = Path(directory)
                driver = tree / 'drivers/NativeVirtioBlock'
                driver.mkdir(parents=True)
                for name in ('NativeVirtioBlock.cpp', 'VerifiedBlockPolicy.h'):
                    shutil.copyfile(ROOT / 'drivers/NativeVirtioBlock' / name, driver / name)
                (tree / 'scripts').mkdir()
                shutil.copyfile(ROOT / 'scripts/build-source-storage.py', tree / 'scripts/build-source-storage.py')
                compile_dir = tree / 'compile-record-cwd'
                compile_dir.mkdir()
                record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOService.cpo.json'
                record_path.parent.mkdir(parents=True)
                record_path.write_text(json.dumps({
                    'directory': str(compile_dir), 'file': 'IOService.cpp',
                    'arguments': ['clang++', '-Irelative/include', '-c', 'IOService.cpp', '-o', 'IOService.cpo'],
                }))
                storage = tree / 'build/source-storage'
                storage.mkdir(parents=True)
                archive = storage / 'libIOStorageFamily.a'
                archive.write_bytes(b'synthetic source-storage archive')
                (storage / 'build-report.json').write_text(json.dumps({
                    'source_revision': 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff',
                    'source_archive_built': True, 'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
                }))
                commands = []

                def run(command, **options):
                    commands.append(command)
                    if command[:4] == ['xcrun', '--sdk', 'iphoneos', 'clang++']:
                        self.assertEqual(options['cwd'], compile_dir.resolve())
                        self.assertIn('-Irelative/include', command)
                        self.assertIn('-DNATIVE_VERIFIED_BLOCK=1', command)
                        self.assertIn('-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', command)
                        self.assertIn('-DNATIVE_VERIFIED_BLOCK_QEMU_NO_UFS=1', command)
                        obj = Path(command[command.index('-o') + 1])
                        obj.write_bytes(struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 1, 1, 24, 0, 0) +
                                        struct.pack('<6I', 0x32, 24, 2, 0x110000, 0, 0))
                    elif command[:3] == ['xcrun', 'ar', 'rcs']:
                        self.assertEqual(len(command), 5)
                        Path(command[3]).write_bytes(b'synthetic Virtio archive')
                    else:
                        self.fail(f'unexpected build command: {command}')
                    return subprocess.CompletedProcess(command, 0, 'synthetic fixture\n')

                def symbols(command, **options):
                    if '-u' in command:
                        values = [' U _vr_open']
                        if single_policy:
                            values.append(' U _verified_root_manifest')
                        if ufs_import:
                            values.append(' U _native_ufs_block_authenticates')
                        return '\n'.join(values) + '\n'
                    return '00000000 S __ZN17NativeVirtioBlock10gMetaClassE\n'

                if single_policy or ufs_import:
                    expected = 'single-policy manifest' if single_policy else 'disabled NativeUfs'
                    with self.assertRaisesRegex(RuntimeError, expected):
                        builder.build(tree, self.profile(), runner=run, check_output=symbols)
                    self.assertEqual(len(commands), 1)
                else:
                    result = builder.build(tree, self.profile(), runner=run, check_output=symbols)
                    self.assertTrue(result['multi_policy'])
                    self.assertTrue(result['source_storage_base_class'])
                    self.assertFalse(result['native_ufs_auth_delegate_compiled'])
                    self.assertFalse(result['hardware_qualified'])
                    self.assertFalse(result['dali_ufs_linked'])
                    self.assertEqual(len(commands), 2)
                    self.assertTrue((tree / 'build/native-virtio-block-qemu-source-storage/build-report.json').is_file())

    def test_disabled_profile_never_invokes_a_compiler(self):
        def unexpected(*args, **kwargs):
            self.fail('disabled profile invoked a build tool')
        result = builder.build(env={}, runner=unexpected, check_output=unexpected)
        self.assertTrue(result['skipped'])

    def test_variant_rejects_unrecognized_source_drift(self):
        source = ROOT / 'drivers/NativeVirtioBlock/NativeVirtioBlock.cpp'
        original = source.read_text()
        variants = [
            original.replace('verified_root_manifest_bytes;', 'verified_root_manifest_length;', 1),
            original.replace('__attribute__((weak));', '__attribute__((weak_import));', 1),
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'driver.cpp'
            for text in variants:
                path.write_text(text)
                with self.assertRaisesRegex(ValueError, 'anchors changed'):
                    source_variant(path)


if __name__ == '__main__':
    unittest.main()
