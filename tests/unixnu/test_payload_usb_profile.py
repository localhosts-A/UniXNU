from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu import payload
from unixnu.util import Error


class PayloadUsbProfileTests(unittest.TestCase):
    def test_default_profile_keeps_usb_absent_from_adt(self):
        self.assertEqual(payload.adt_profile_flags({}), [])
        self.assertEqual(payload.adt_profile_flags({'XNU_NATIVE_USB_HOST': False}), [])

    def test_usb_host_defconfig_requests_the_adt_resource_node(self):
        self.assertEqual(payload.adt_profile_flags({'XNU_NATIVE_USB_HOST': True}), ['--usb-host'])

    def test_unparsed_usb_host_value_fails_closed(self):
        with self.assertRaisesRegex(Error, 'parsed boolean'):
            payload.adt_profile_flags({'XNU_NATIVE_USB_HOST': 'y'})


if __name__ == '__main__':
    unittest.main()
