from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
PATCH = ROOT / 'patches/xnu/offline-component-load-probes.patch'
CDC_PATCH = ROOT / 'patches/xnu/zzzz-usb-cdc-console-log.patch'
HUNK = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@')


class XnuPatchContracts(unittest.TestCase):
    def test_offline_component_patch_hunk_counts_match_body(self):
        lines = PATCH.read_text().splitlines()
        headers = [(index, HUNK.match(line)) for index, line in enumerate(lines) if HUNK.match(line)]
        self.assertGreater(len(headers), 0)
        for header_index, header in headers:
            old_count = new_count = 0
            for line in lines[header_index + 1:]:
                if line.startswith('@@ ') or line.startswith('--- a/'):
                    break
                if line.startswith(' '):
                    old_count += 1
                    new_count += 1
                elif line.startswith('-') and not line.startswith('---'):
                    old_count += 1
                elif line.startswith('+') and not line.startswith('+++'):
                    new_count += 1
            self.assertEqual((old_count, new_count),
                             (int(header.group(2) or 1), int(header.group(4) or 1)))

    def test_legacy_display_probe_reference_is_compile_time_gated(self):
        lines = PATCH.read_text().splitlines()
        declaration = next(index for index, line in enumerate(lines) if 'legacy_register = 0;' in line)
        call = next(index for index, line in enumerate(lines) if 'native_legacy_display_probe();' in line)
        guard = next(index for index, line in enumerate(lines)
                     if 'XNU_NATIVE_LEGACY_DISPLAY' in line and index < call)
        self.assertGreater(declaration, guard)
        self.assertGreater(declaration - guard, 0)
        self.assertTrue(any(line.startswith('+#endif') for line in lines[call + 1:call + 3]))

    def test_usb_cdc_console_tap_is_gated_to_profile_and_hunks_are_balanced(self):
        lines = CDC_PATCH.read_text().splitlines()
        headers = [(index, HUNK.match(line)) for index, line in enumerate(lines) if HUNK.match(line)]
        self.assertEqual(len(headers), 4)
        for header_index, header in headers:
            old_count = new_count = 0
            for line in lines[header_index + 1:]:
                if line.startswith('@@ ') or line.startswith('diff --git '):
                    break
                if line.startswith(' '):
                    old_count += 1
                    new_count += 1
                elif line.startswith('-') and not line.startswith('---'):
                    old_count += 1
                elif line.startswith('+') and not line.startswith('+++'):
                    new_count += 1
            self.assertEqual((old_count, new_count),
                             (int(header.group(2) or 1), int(header.group(4) or 1)))
        self.assertEqual(CDC_PATCH.read_text().count(
            '#if defined(XNU_NATIVE_USB_CDC_LOG) && XNU_NATIVE_USB_CDC_LOG'), 5)
        self.assertEqual(CDC_PATCH.read_text().count('dali_usb_cdc_log_write('), 5)
        build = (ROOT / 'scripts/build-xnu-4k-baseline.sh').read_text()
        self.assertIn('USB_CDC_LOG_MODE=${XNU_NATIVE_USB_CDC_LOG:-0}', build)
        self.assertIn('-DXNU_NATIVE_USB_CDC_LOG=$USB_CDC_LOG_MODE', build)
        self.assertIn('USB_CDC_LOG_STAMP', build)


if __name__ == '__main__':
    unittest.main()
