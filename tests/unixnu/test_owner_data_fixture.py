import importlib.util
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/prepare-owner-data-fixture.py'
spec = importlib.util.spec_from_file_location('prepare_owner_data_fixture', SCRIPT)
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)


class OwnerDataFixtureDiskSelectionTests(unittest.TestCase):
    def test_selects_one_whole_disk_and_ignores_source_os_and_partition_nodes(self):
        cases = (
            [
                {'dev-entry': '/dev/disk9', 'content-hint': 'GUID_partition_scheme'},
                {'dev-entry': '/dev/disk9s1', 'content-hint': 'Apple_APFS'},
                {'dev-entry': '/dev/disk10', 'content-hint': 'EF57347C-0000-11AA-AA11-0030654'},
                {'dev-entry': '/dev/disk10s1', 'content-hint': '41504653-0000-11AA-AA11-0030654'},
            ],
            [
                {'dev-entry': '/dev/disk9'},
                {'dev-entry': '/dev/disk10', 'content-hint': 'EF57347C-0000-11AA-AA11-0030654'},
                {'dev-entry': '/dev/disk10s1', 'content-hint': '41504653-0000-11AA-AA11-0030654'},
            ],
        )
        for entities in cases:
            with self.subTest(entities=entities):
                self.assertEqual(fixture.select_whole_disk(entities), '/dev/disk9')

    def test_rejects_missing_or_ambiguous_whole_disk(self):
        for entities in ([], [{'dev-entry': '/dev/disk9'}, {'dev-entry': '/dev/disk10'}]):
            with self.subTest(entities=entities), self.assertRaisesRegex(SystemExit, 'GUID partition-scheme'):
                fixture.select_whole_disk(entities)

    def test_selects_exact_apfs_partition_from_diskutil_list(self):
        listing = {'AllDisksAndPartitions': [
            {'DeviceIdentifier': 'disk7', 'Partitions': [
                {'DeviceIdentifier': 'disk7s1', 'Content': 'Apple_APFS'}]},
            {'DeviceIdentifier': 'disk9', 'Partitions': [
                {'DeviceIdentifier': 'disk9s1', 'Content': 'Apple_APFS'}]},
        ]}
        self.assertEqual(fixture.select_apfs_partition('/dev/disk9', listing), '/dev/disk9s1')

    def test_rejects_wrong_disk_missing_or_ambiguous_apfs_partition(self):
        cases = (
            ({'AllDisksAndPartitions': []}, '/dev/disk9', 'did not identify'),
            ({'AllDisksAndPartitions': [{'DeviceIdentifier': 'disk9', 'Partitions': []}]},
             '/dev/disk9', 'exactly one Apple_APFS'),
            ({'AllDisksAndPartitions': [{'DeviceIdentifier': 'disk9', 'Partitions': [
                {'DeviceIdentifier': 'disk9s1', 'Content': 'Apple_APFS'},
                {'DeviceIdentifier': 'disk9s2', 'Content': 'Apple_APFS'},
            ]}]}, '/dev/disk9', 'exactly one Apple_APFS'),
            ({'AllDisksAndPartitions': [{'DeviceIdentifier': 'disk9', 'Partitions': [
                {'DeviceIdentifier': '../disk9s1', 'Content': 'Apple_APFS'},
            ]}]}, '/dev/disk9', 'exactly one Apple_APFS'),
        )
        for listing, whole_device, message in cases:
            with self.subTest(listing=listing), self.assertRaisesRegex(SystemExit, message):
                fixture.select_apfs_partition(whole_device, listing)


if __name__ == '__main__':
    unittest.main()
