import importlib.util
import io
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
import tempfile
import sys
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu import cli
SPEC = importlib.util.spec_from_file_location('source_msdosfs_builder', ROOT / 'scripts/build-source-msdosfs.py')
FAT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FAT)


class SourceMsdosfsTests(unittest.TestCase):
    def make_sources(self, root, names=FAT.SOURCES):
        project = root / 'msdosfs.xcodeproj/project.pbxproj'
        project.parent.mkdir(parents=True)
        project.write_text(FAT.PHASE + ' /* Sources */ = {\nisa = PBXSourcesBuildPhase;\nfiles = (\n' +
                           '\n'.join('/* ' + name + ' in Sources */,' for name in names) + '\n);\n};\n')
        directory = root / FAT.SOURCE_DIR
        directory.mkdir(parents=True)
        for name in FAT.SOURCES:
            (directory / name).write_text('/* source fixture */\n')

    def test_only_the_complete_pinned_kernel_phase_is_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self.make_sources(root)
            self.assertEqual([p.name for p in FAT.kernel_sources(root)], list(FAT.SOURCES))
            (root / FAT.SOURCE_DIR / FAT.SOURCES[0]).unlink()
            with self.assertRaisesRegex(ValueError, 'implementation is missing'):
                FAT.kernel_sources(root)

    def test_replaced_or_extra_target_source_is_rejected(self):
        for names in (FAT.SOURCES[:-1], (*FAT.SOURCES, 'userspace.c')):
            with self.subTest(names=names), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                self.make_sources(root, names)
                with self.assertRaisesRegex(ValueError, 'phase changed'):
                    FAT.kernel_sources(root)

    def test_kernel_localargs_transform_is_exact_and_preserves_user_copyin(self):
        original = (
            '\t\terror = copyin(data, &args, sizeof(struct msdosfs_args));\n'
            '\t\tvfe.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLNOTYPENUM | '
            'VFS_TBLLOCALVOL | VFS_TBL64BITREADY | VFS_TBLREADDIR_EXTENDED;\n')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'msdosfs_vfsops.c'
            output = root / 'generated/msdosfs_vfsops.c'
            source.write_text(original)
            FAT.prepare_kernel_localargs_source(source, output)
            transformed = output.read_text()
            self.assertEqual(source.read_text(), original)
            self.assertIn('if (vfs_is_kernel_localargs(mp))', transformed)
            self.assertIn('bcopy((const void *)data, &args, sizeof(args));', transformed)
            self.assertIn('else {\n\t\t\terror = copyin(data, &args, sizeof(struct msdosfs_args));',
                          transformed)
            self.assertIn('VFS_TBLREADDIR_EXTENDED | VFS_TBLKERNELARGS;', transformed)
            with self.assertRaisesRegex(ValueError, 'Refusing to patch'):
                FAT.prepare_kernel_localargs_source(source, source)

    def test_kernel_localargs_transform_rejects_source_drift(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'msdosfs_vfsops.c'
            output = root / 'generated/msdosfs_vfsops.c'
            source.write_text('error = copyin(data, &args, sizeof(args));\n')
            with self.assertRaisesRegex(ValueError, 'source anchor changed'):
                FAT.prepare_kernel_localargs_source(source, output)
            self.assertFalse(output.exists())

    def test_kernel_localargs_patch_is_in_the_xnu_patch_set(self):
        patch = (ROOT / 'patches/xnu/source-kernel-local-mount-args.patch').read_text()
        self.assertIn('KERNEL_MOUNT_LOCALARGS', patch)
        self.assertIn('VFC_VFSKERNELARGS', patch)
        self.assertIn('VFS_TBLKERNELARGS', patch)
        self.assertIn('vfs_is_kernel_localargs', patch)
        self.assertIn('MNT_NOSUID | MNT_NODEV | MNT_NOEXEC', patch)

    def test_disabled_and_invalid_modes_do_not_call_apple_tools(self):
        with patch.object(FAT, 'build', side_effect=AssertionError('Apple tools called')):
            with patch.dict(FAT.os.environ, {'XNU_SOURCE_MSDOSFS': '0'}), redirect_stdout(io.StringIO()) as output:
                FAT.main([])
            self.assertIn('SKIP', output.getvalue())
            with patch.dict(FAT.os.environ, {'XNU_SOURCE_MSDOSFS': 'yes'}), self.assertRaises(SystemExit):
                FAT.main([])

    def test_every_real_vfs_symbol_must_be_defined_and_resolved_exactly(self):
        defined = '\n'.join('00000000 T ' + name for name in FAT.GLOBAL_SYMBOLS)
        private_defined = '\n'.join('00000000 (__DATA,__const) private external ' + name
                                     for name in FAT.PRIVATE_SYMBOLS)
        FAT.check_symbols(defined, private_defined=private_defined)
        for name in FAT.SYMBOLS:
            with self.subTest(name=name):
                global_defs = '\n'.join('00000000 T ' + item for item in FAT.GLOBAL_SYMBOLS
                                         if item != name)
                private_defs = '\n'.join('00000000 (__DATA,__const) private external ' + item
                                          for item in FAT.PRIVATE_SYMBOLS if item != name)
                with self.assertRaisesRegex(ValueError, 'Missing resolved'):
                    FAT.check_symbols(global_defs, private_defined=private_defs)
                with self.assertRaisesRegex(ValueError, 'Missing resolved'):
                    FAT.check_symbols(defined, 'U ' + name, private_defined)

    def test_final_kernel_must_define_and_resolve_the_localargs_accessor(self):
        symbol = FAT.KERNEL_LOCALARGS_SUPPORT_SYMBOLS[0]
        self.assertEqual(FAT.kernel_localargs_support('00000000 T ' + symbol), {symbol: True})
        self.assertEqual(FAT.kernel_localargs_support('', 'U ' + symbol), {symbol: False})

    def test_feed_is_fetched_only_for_the_selected_kernel_profile(self):
        modules = {('refer', 'msdosfs'): SimpleNamespace(name='msdosfs', kind='refer', origin='git')}
        for enabled in (False, True):
            ctx = SimpleNamespace(need_config=Mock(return_value={'XNU_SOURCE_MSDOSFS': enabled}))
            with patch.object(cli.feeds, 'update_submodules'), patch.object(cli.feeds, 'resolve', return_value=modules), \
                    patch.object(cli.feeds, 'fetch') as fetch, patch.object(cli.tree, 'assemble'):
                cli.prepare(ctx, ['kernel'])
            self.assertEqual('msdosfs' in fetch.call_args.kwargs['only'], enabled)

    def test_kernel_link_force_loads_the_real_msdosfs_archive(self):
        baseline = (ROOT / 'scripts/build-xnu-4k-baseline.sh').read_text()
        self.assertIn('SOURCE_MSDOSFS=${XNU_SOURCE_MSDOSFS:-0}', baseline)
        self.assertIn('if [ "$BOOTSTRAP" = 1 ]; then', baseline)
        self.assertIn('SOURCE_MSDOSFS_REGISTER=0', baseline)
        self.assertIn('.source-msdosfs-registration-mode', baseline)
        self.assertIn('MSDOSFS_ARCHIVE=$ROOT/build/source-msdosfs/libmsdosfs.a', baseline)
        self.assertIn('-Wl,-force_load,$MSDOSFS_ARCHIVE', baseline)
        self.assertIn('KERNEL_ARCHIVE_DEPENDENCIES="$KERNEL_ARCHIVE_DEPENDENCIES $MSDOSFS_ARCHIVE"', baseline)


if __name__ == '__main__':
    unittest.main()
