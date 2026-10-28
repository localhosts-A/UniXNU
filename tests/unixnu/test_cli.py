import hashlib
from pathlib import Path
import struct
import sys
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu import cli, kernel
from unixnu.artifacts import package_efi
from unixnu.util import Error


class SourceOnlyBuild(unittest.TestCase):
    def test_default_source_only_build_is_kernel_only(self):
        args = cli.parser().parse_args(['build', '--source-only'])
        self.assertEqual(cli.build_steps(args.steps, args.source_only), ['kernel'])

    def test_build_prelink_accepts_a_board_named_profile(self):
        args = cli.parser().parse_args(['build', 'prelink', '--prelink-profile', 'donor-runtime-audit'])
        self.assertEqual(args.steps, ['prelink'])
        self.assertEqual(args.prelink_profile, 'donor-runtime-audit')

    def test_source_only_accepts_explicit_kernel_and_all(self):
        self.assertEqual(cli.build_steps(['kernel'], True), ['kernel'])
        self.assertEqual(cli.build_steps(['all'], True), ['kernel'])

    def test_source_only_rejects_prelink_and_other_steps(self):
        for steps in (['prelink'], ['package'], ['kernel', 'firmware']):
            with self.subTest(steps=steps), self.assertRaisesRegex(Error, '--source-only'):
                cli.build_steps(steps, True)

    def test_source_only_build_skips_prelink(self):
        args = SimpleNamespace(steps=['all'], source_only=True)
        ctx = SimpleNamespace(args=args, refuse_example=Mock(), need_config=Mock())
        with patch.object(cli, 'prepare', return_value={}) as prepare, \
             patch.object(kernel, 'build') as build, \
             patch.object(kernel, 'validate_source_output', return_value=(Path('/kernel.macho'), 'a' * 64)), \
             patch.object(kernel, 'prelink') as prelink:
            cli.cmd_build(ctx)
        prepare.assert_called_once_with(ctx, ['kernel'])
        build.assert_called_once_with(ctx, {})
        prelink.assert_not_called()

    def test_validate_source_output_checks_arm64_macho_and_hash(self):
        data = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, 24, 0, 0)
        data += struct.pack('<IIQQ', 0x80000028, 24, 0, 0)
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            output = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
            output.parent.mkdir(parents=True)
            output.write_bytes(data)
            path, digest = kernel.validate_source_output(SimpleNamespace(tree=tree))
        self.assertEqual(path, output)
        self.assertEqual(digest, hashlib.sha256(data).hexdigest())

    def test_validate_source_output_rejects_non_macho(self):
        with tempfile.TemporaryDirectory() as temporary:
            tree = Path(temporary)
            output = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
            output.parent.mkdir(parents=True)
            output.write_bytes(b'not a Mach-O image')
            with self.assertRaisesRegex(Error, 'truncated'):
                kernel.validate_source_output(SimpleNamespace(tree=tree))

    def test_package_efi_finds_one_board_build_and_rejects_ambiguity(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / 'out/dali/package/Build/Out/AARCH64-DEBUG/UniXNU.efi'
            first.parent.mkdir(parents=True)
            first.write_bytes(b'efi')
            with patch.dict('os.environ', {'UNIXNU_OUT': str(root / 'out')}):
                self.assertEqual(package_efi(root), first.resolve())
                second = root / 'out/sheng/package/Build/Out/AARCH64-DEBUG/UniXNU.efi'
                second.parent.mkdir(parents=True)
                second.write_bytes(b'efi')
                with self.assertRaisesRegex(Error, 'multiple UniXNU EFI'):
                    package_efi(root)


if __name__ == '__main__':
    unittest.main()
