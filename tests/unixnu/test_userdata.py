import hashlib
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu.userdata import (check_capacity, flash_command, partition_capacity, partition_lun,
                             partition_name, select_whole_disk, verify_attached_disk, verify_built_image,
                             attach)
from unixnu.util import Error


class UserdataTargetTests(unittest.TestCase):
    def test_default_target_preserves_userdata_behavior(self):
        spec = {'first_lba': 0, 'last_lba': 7, 'lba_bytes': 4096}
        self.assertEqual(partition_name(spec), 'userdata')
        self.assertEqual(partition_capacity(spec), 8 * 4096)
        self.assertEqual(flash_command(spec, Path('/tmp/root.img')), 'fastboot flash userdata /tmp/root.img')

    def test_named_partition_target_and_live_dali_capacity(self):
        spec = {'partition_name': 'unixnu_data', 'lun': 2, 'first_lba': 7_023_103,
                'last_lba': 15_403_518, 'lba_bytes': 4096}
        self.assertEqual(partition_name(spec), 'unixnu_data')
        self.assertEqual(partition_lun(spec), 2)
        self.assertEqual(partition_capacity(spec), 34_326_183_936)
        self.assertEqual(flash_command(spec, Path('/tmp/userdata-root.img')),
                         'fastboot flash unixnu_data /tmp/userdata-root.img')

    def test_rejects_invalid_partition_tokens(self):
        for name in ('', '../userdata', 'unixnu data', 'userdata; reboot', 'x' * 37, None):
            with self.subTest(name=name), self.assertRaisesRegex(Error, 'partition_name'):
                partition_name({'partition_name': name})

    def test_lun_is_optional_but_rejects_unmeasured_values(self):
        self.assertIsNone(partition_lun({}))
        for lun in (-1, 8, True, '2'):
            with self.subTest(lun=lun), self.assertRaisesRegex(Error, 'userdata lun'):
                partition_lun({'lun': lun})

    def test_rejects_invalid_geometry(self):
        for spec in (
            {'first_lba': -1, 'last_lba': 0, 'lba_bytes': 4096},
            {'first_lba': 2, 'last_lba': 1, 'lba_bytes': 4096},
            {'first_lba': 0, 'last_lba': 1, 'lba_bytes': 1000},
            {'first_lba': 0, 'last_lba': True, 'lba_bytes': 4096},
        ):
            with self.subTest(spec=spec), self.assertRaisesRegex(Error, 'userdata'):
                partition_capacity(spec)

    def test_capacity_boundary_and_overflow(self):
        check_capacity(4096, 4096)
        with self.assertRaisesRegex(Error, 'target partition'):
            check_capacity(4097, 4096)


class UserdataImageVerificationTests(unittest.TestCase):
    @staticmethod
    def image(path):
        windows = [('OS', 1 << 20, b'os-window' * 512),
                   ('SystemOS', 2 << 20, b'S' * 512)]
        total = 3 << 20
        header = bytearray(4096)
        header[:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, len(windows), total)
        entries = []
        with path.open('wb') as output:
            output.truncate(total)
            for name, offset, data in windows:
                digest = hashlib.sha256(data).digest()
                entries.append((name, offset, len(data), digest))
                output.seek(offset)
                output.write(data)
            for index, (name, offset, data) in enumerate(windows):
                digest = entries[index][3]
                struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index,
                                 name.encode(), offset, len(data), 512, 1, digest, bytes(40))
            header[4064:4096] = hashlib.sha256(header[:4064]).digest()
            output.seek(0)
            output.write(header)
        return entries, total

    def test_valid_image_checks_manifest_windows_padding_and_full_hash(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'userdata-root.img'
            entries, total = self.image(path)
            actual = verify_built_image(path, entries, total)
            self.assertEqual(actual, hashlib.sha256(path.read_bytes()).hexdigest())

    def test_window_content_must_match_manifest_digest(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'userdata-root.img'
            entries, total = self.image(path)
            with path.open('r+b') as image:
                image.seek(entries[0][1] + 1)
                image.write(b'X')
            with self.assertRaisesRegex(Error, 'window OS digest'):
                verify_built_image(path, entries, total)

    def test_nonzero_padding_and_bad_manifest_entry_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'userdata-root.img'
            entries, total = self.image(path)
            with path.open('r+b') as image:
                image.seek(entries[0][1] + entries[0][2])
                image.write(b'X')
            with self.assertRaisesRegex(Error, 'padding'):
                verify_built_image(path, entries, total)

            entries, total = self.image(path)
            with path.open('r+b') as image:
                image.seek(24 + 48)
                image.write(struct.pack('<I', 0))
                image.seek(0)
                header = bytearray(image.read(4096))
                header[4064:4096] = hashlib.sha256(header[:4064]).digest()
                image.seek(0)
                image.write(header)
            with self.assertRaisesRegex(Error, 'manifest entry'):
                verify_built_image(path, entries, total)

    def test_capacity_and_overlapping_windows_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'userdata-root.img'
            entries, total = self.image(path)
            with self.assertRaisesRegex(Error, 'target partition'):
                verify_built_image(path, entries, total - 1)
            overlapping = [entries[0], ('SystemOS', entries[0][1] + 512, entries[1][2], entries[1][3])]
            with self.assertRaisesRegex(Error, 'invalid or overlapping bounds'):
                verify_built_image(path, overlapping, total)


class UserdataAttachmentTests(unittest.TestCase):
    def test_selects_backing_whole_disk_and_ignores_apfs_children(self):
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
            [{'dev-entry': '/dev/disk9'}],
        )
        for entities in cases:
            with self.subTest(entities=entities):
                self.assertEqual(select_whole_disk(entities), '/dev/disk9')

    def test_rejects_ambiguous_whole_disk_mappings(self):
        with self.assertRaisesRegex(Error, 'one whole-disk image mapping'):
            select_whole_disk([{'dev-entry': '/dev/disk9'}, {'dev-entry': '/dev/disk10'}])

    def test_attach_uses_plist_whole_disk_not_first_synthesized_device(self):
        attached = plistlib.dumps({'system-entities': [
            {'dev-entry': '/dev/disk9', 'content-hint': 'GUID_partition_scheme'},
            {'dev-entry': '/dev/disk9s1', 'content-hint': 'Apple_APFS'},
            {'dev-entry': '/dev/disk10', 'content-hint': 'EF57347C-0000-11AA-AA11-0030654'},
        ]})
        disk_info = plistlib.dumps({'BusProtocol': 'Disk Image', 'Writable': False,
                                    'DeviceNode': '/dev/disk9', 'TotalSize': 6_000_000})
        with patch('unixnu.userdata.subprocess.check_output', side_effect=[attached, disk_info]) as check_output:
            self.assertEqual(attach(Path('/tmp/os.dmg'), 6_000_000), '/dev/disk9')
        self.assertIn('-plist', check_output.call_args_list[0].args[0])
        self.assertEqual(check_output.call_args_list[1].args[0][-1], '/dev/disk9')

    def test_accepts_only_exact_read_only_disk_image_mapping(self):
        record = {'BusProtocol': 'Disk Image', 'Writable': False,
                  'DeviceNode': '/dev/disk4', 'TotalSize': 6_000_000}
        with patch('unixnu.userdata.subprocess.check_output', return_value=plistlib.dumps(record)):
            verify_attached_disk('/dev/disk4', 6_000_000)

    def test_rejects_writable_wrong_device_or_size_mapping(self):
        records = (
            {'BusProtocol': 'Disk Image', 'Writable': True, 'DeviceNode': '/dev/disk4', 'TotalSize': 6_000_000},
            {'BusProtocol': 'Disk Image', 'Writable': False, 'DeviceNode': '/dev/disk5', 'TotalSize': 6_000_000},
            {'BusProtocol': 'Disk Image', 'Writable': False, 'DeviceNode': '/dev/disk4', 'TotalSize': 5_999_999},
            {'BusProtocol': 'USB', 'Writable': False, 'DeviceNode': '/dev/disk4', 'TotalSize': 6_000_000},
        )
        for record in records:
            with self.subTest(record=record), patch(
                    'unixnu.userdata.subprocess.check_output', return_value=plistlib.dumps(record)):
                with self.assertRaisesRegex(Error, 'expected .* observed'):
                    verify_attached_disk('/dev/disk4', 6_000_000)


if __name__ == '__main__':
    unittest.main()
