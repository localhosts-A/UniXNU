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

    def test_disabled_and_invalid_modes_do_not_call_apple_tools(self):
        with patch.object(FAT, 'build', side_effect=AssertionError('Apple tools called')):
            with patch.dict(FAT.os.environ, {'XNU_SOURCE_MSDOSFS': '0'}), redirect_stdout(io.StringIO()) as output:
                FAT.main([])
            self.assertIn('SKIP', output.getvalue())
            with patch.dict(FAT.os.environ, {'XNU_SOURCE_MSDOSFS': 'yes'}), self.assertRaises(SystemExit):
                FAT.main([])

    def test_every_real_vfs_symbol_must_be_defined_and_resolved_exactly(self):
        defined = '\n'.join('00000000 T ' + name for name in FAT.SYMBOLS)
        FAT.check_symbols(defined)
        for name in FAT.SYMBOLS:
            with self.subTest(name=name):
                with self.assertRaisesRegex(ValueError, 'Missing resolved'):
                    FAT.check_symbols('\n'.join('00000000 T ' + item if item != name
                                                else '00000000 T ' + item + '_suffix'
                                                for item in FAT.SYMBOLS))
                with self.assertRaisesRegex(ValueError, 'Missing resolved'):
                    FAT.check_symbols(defined, 'U ' + name)

    def test_feed_is_fetched_only_for_the_selected_kernel_profile(self):
        modules = {('refer', 'msdosfs'): SimpleNamespace(name='msdosfs', kind='refer', origin='git')}
        for enabled in (False, True):
            ctx = SimpleNamespace(need_config=Mock(return_value={'XNU_SOURCE_MSDOSFS': enabled}))
            with patch.object(cli.feeds, 'update_submodules'), patch.object(cli.feeds, 'resolve', return_value=modules), \
                    patch.object(cli.feeds, 'fetch') as fetch, patch.object(cli.tree, 'assemble'):
                cli.prepare(ctx, ['kernel'])
            self.assertEqual('msdosfs' in fetch.call_args.kwargs['only'], enabled)


if __name__ == '__main__':
    unittest.main()
