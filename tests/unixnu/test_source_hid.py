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
    def test_support_archive_includes_the_tablet_base_used_by_iohidpointing(self):
        self.assertIn('IOHIDSystem/IOHITablet.cpp', HID.SYSTEM_SUPPORT_SOURCES)
        self.assertIn('IOHITablet', HID.CLASSES)

    def test_support_archive_does_not_repeat_embedded_source_units(self):
        self.assertNotIn('IOHIDFamily/Cosmo_USB2ADB.c', HID.SYSTEM_SUPPORT_SOURCES)
        self.assertNotIn('IOHIDFamily/IOHIDEventServiceQueue.cpp', HID.SYSTEM_SUPPORT_SOURCES)

    def test_source_inputs_reject_basename_collisions(self):
        with self.assertRaisesRegex(ValueError, 'basename appears more than once'):
            HID.ensure_unique_source_basenames([
                Path('/tmp/one/Shared.cpp'), Path('/tmp/two/Shared.cpp')])

    def test_system_support_sources_are_resolved_from_the_fixed_family_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'IOHIDFamily'
            source.mkdir()
            files = '\n'.join('    /* ' + Path(name).name + ' in Sources */,'
                              for name in HID.SYSTEM_SUPPORT_SOURCES)
            project = source / 'IOHIDFamily.xcodeproj/project.pbxproj'
            project.parent.mkdir(parents=True)
            project.write_text(
                HID.SYSTEM_SUPPORT_PHASE + ' /* Sources */ = {\n'
                '    isa = PBXSourcesBuildPhase;\n'
                '    files = (\n' + files + '\n    );\n};\n')
            for name in HID.SYSTEM_SUPPORT_SOURCES:
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('/* pinned source fixture */\n')
            resolved = HID.system_support_sources(source)
            self.assertEqual([p.relative_to(source).as_posix() for p in resolved],
                             list(HID.SYSTEM_SUPPORT_SOURCES))

    def test_system_support_sources_reject_a_missing_target_member(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'IOHIDFamily'
            source.mkdir()
            project = source / 'IOHIDFamily.xcodeproj/project.pbxproj'
            project.parent.mkdir(parents=True)
            project.write_text(
                HID.SYSTEM_SUPPORT_PHASE + ' /* Sources */ = {\n'
                '    isa = PBXSourcesBuildPhase;\n'
                '    files = (\n    );\n};\n')
            with self.assertRaisesRegex(ValueError, 'required support units'):
                HID.system_support_sources(source)

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
            usages = root / 'published-apple-usages.h'
            usages.write_text('/* fixture published usage declarations */\n')
            for name in HID.GENERATED_EVENT_HEADERS:
                (event_headers / name).write_text('/* fixture generated declaration ' + name + ' */\n')
            include, imported = HID.prepare_headers(root / 'source', root / 'out', root / 'sdk', event_headers, usages)
            self.assertEqual(len(imported), 6)
            self.assertEqual((include / 'DaliPublishedAppleUsages.h').read_bytes(), usages.read_bytes())
            for name in HID.GENERATED_EVENT_HEADERS:
                self.assertFalse((family / name).exists())
                self.assertEqual((include / 'IOKit/hid' / name).read_bytes(), (event_headers / name).read_bytes())
                self.assertEqual((include / 'IOKit/hidevent' / name).read_bytes(), (event_headers / name).read_bytes())

    def test_public_port_records_unsupported_security_and_private_features(self):
        self.assertTrue(HID.PUBLIC_PATCH.is_file())
        self.assertTrue(HID.PRIVATE_COMPAT_PATCH.is_file())
        self.assertTrue(HID.PERSONALITY_PATCH.is_file())
        self.assertIn('interactive-tcc-authorization', HID.UNSUPPORTED_HID_FEATURES)
        self.assertIn('private-interface-rematch', HID.UNSUPPORTED_HID_FEATURES)
        self.assertIn('private-no-dropped-report-and-task-suspend-options', HID.UNSUPPORTED_HID_FEATURES)
        self.assertIn('raw-userclient-report-queue-can-drop-input-edges-under-backpressure', HID.UNSUPPORTED_HID_FEATURES)
        self.assertIn('unlisted-userclient-property-writes', HID.UNSUPPORTED_HID_FEATURES)

    def test_compatibility_patch_gates_input_and_allows_only_public_queue_properties(self):
        patch = HID.PRIVATE_COMPAT_PATCH.read_text()
        for fragment in (
            'DaliPublicHIDProtectedAccess',
            'kHIDUsage_GD_Keyboard',
            'kHIDUsage_GD_Keypad',
            'kHIDUsage_GD_Mouse',
            'kHIDUsage_Dig_TouchPad',
            'kIOHIDMaxReportBufferCountKey',
            'kIOHIDReportBufferEntrySizeKey',
            'props->getCount() == allowedCount',
            'kIOReturnUnsupported',
        ):
            with self.subTest(fragment=fragment):
                self.assertIn(fragment, patch)

    def test_hid_archive_is_part_of_opt_in_kernel_cache_identity(self):
        baseline = (ROOT / 'scripts/build-xnu-4k-baseline.sh').read_text()
        self.assertIn('SOURCE_HID_FAMILY=${XNU_SOURCE_HID_FAMILY:-0}', baseline)
        self.assertIn('"$SOURCE_HID_FAMILY" "$ROOT/build/source-hid/libIOHIDFamily.a"', baseline)
        self.assertIn('KERNEL_EXTRA_ARCHIVES="$KERNEL_EXTRA_ARCHIVES ${XNU_EXTRA_LD_ARCHIVES:-}"', baseline)

if __name__ == '__main__':
    unittest.main()
