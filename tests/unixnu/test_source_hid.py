import importlib.util
import io
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
import tempfile
import unittest
from unittest.mock import Mock, patch
import sys

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('source_hid_builder', ROOT / 'scripts/build-source-hid.py')
HID = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HID)
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu import cli

class SourceHidTests(unittest.TestCase):
    def test_disabled_mode_never_invokes_apple_tools(self):
        output = io.StringIO()
        with patch.dict(HID.os.environ, {'XNU_SOURCE_HID_FAMILY': '0'}), \
             patch.object(HID, 'build', side_effect=AssertionError('disabled')), redirect_stdout(output):
            HID.main([])
        self.assertEqual(output.getvalue().strip(), 'SKIP: source IOHIDFamily disabled')

    def test_hid_feed_is_fetched_only_for_selected_kernel_profile(self):
        modules = {('refer', 'IOHIDFamily'): SimpleNamespace(name='IOHIDFamily', kind='refer', origin='git')}
        for enabled in (False, True):
            ctx = SimpleNamespace(need_config=Mock(return_value={'XNU_SOURCE_HID_FAMILY': enabled}))
            with patch.object(cli.feeds, 'update_submodules'), \
                 patch.object(cli.feeds, 'resolve', return_value=modules), \
                 patch.object(cli.feeds, 'fetch') as fetch, patch.object(cli.tree, 'assemble'):
                cli.prepare(ctx, ['kernel'])
            self.assertEqual('IOHIDFamily' in fetch.call_args.kwargs['only'], enabled)

    def test_missing_pinned_generated_declarations_are_a_hard_error(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            family = root / 'source/IOHIDFamily'
            system = root / 'source/IOHIDSystem/IOKit/hidsystem'
            family.mkdir(parents=True)
            system.mkdir(parents=True)
            (family / 'IOHIDDeviceTypes.h').write_text('/* fixture declaration */\n')
            with self.assertRaisesRegex(ValueError, 'Missing pinned generated HID declaration'):
                HID.prepare_headers(root / 'source', root / 'out', root / 'absent-sdk', root / 'absent-headers')
            self.assertFalse((family / 'IOHIDEventTypes.h').exists())

    def test_header_staging_does_not_modify_fixed_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            family = root / 'source/IOHIDFamily'
            system = root / 'source/IOHIDSystem/IOKit/hidsystem'
            event_headers = root / 'event-source/IOHIDFamily'
            family.mkdir(parents=True)
            system.mkdir(parents=True)
            event_headers.mkdir(parents=True)
            (root / 'sdk/usr/include').mkdir(parents=True)
            (root / 'sdk/usr/include/TargetConditionals.h').write_text('/* fixture SDK */\n')
            (family / 'IOHIDDeviceTypes.h').write_text('/* fixture types */\n')
            for name in HID.GENERATED_EVENT_HEADERS:
                (event_headers / name).write_text('/* fixture generated declaration ' + name + ' */\n')
            include, imported = HID.prepare_headers(root / 'source', root / 'out', root / 'sdk', event_headers)
            self.assertEqual(len(imported), 5)
            for name in HID.GENERATED_EVENT_HEADERS:
                self.assertFalse((family / name).exists())
                self.assertEqual((include / 'IOKit/hid' / name).read_bytes(), (event_headers / name).read_bytes())
                self.assertEqual((include / 'IOKit/hidevent' / name).read_bytes(), (event_headers / name).read_bytes())

if __name__ == '__main__':
    unittest.main()
