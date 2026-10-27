from pathlib import Path
import copy
import hashlib
import importlib.util
import sys
from tempfile import TemporaryDirectory
import unittest
from types import SimpleNamespace
from unittest.mock import patch
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
from unixnu import kernel

PRELINK = ROOT / "kits/NativePrelink"
sys.path.insert(0, str(PRELINK))
from emit_prelink import to_xml
from native_prelink import backed_offset, macho, unserialize_xml
from test_emit_prelink import images as prelink_images
from test_native_prelink import header, segment
SPEC = importlib.util.spec_from_file_location(
    "source_interface_prelink", ROOT / "scripts/emit-source-interface-prelink.py")
SOURCE_PRELINK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOURCE_PRELINK)


class PrelinkProfileTests(unittest.TestCase):
    def test_source_interface_rewrite_preserves_donor_identity_without_executable(self):
        native, original = prelink_images()
        original = bytearray(original)
        parent = macho(original)
        info = next(item for item in parent['segments'] if item['name'] == '__PRELINK_INFO')
        info_at = backed_offset(parent['segments'], info['va'], info['filesize'], len(original))
        metadata = unserialize_xml(original[info_at:info_at + info['filesize']])
        component, = (entry for entry in metadata['_PrelinkInfoDictionary']
                      if entry['CFBundleIdentifier'] == 'test.component')
        component['OSBundleLibraries'] = {'test.storage': '1'}
        metadata['_PrelinkInfoDictionary'].append({
            'CFBundleIdentifier': 'test.storage',
            'CFBundleName': 'Source storage interface',
            'CFBundleExecutable': 'IOStorageFamily',
            'CFBundleVersion': '2.1',
            '_PrelinkExecutableLoadAddr': 0x2800,
            '_PrelinkExecutableSourceAddr': 0x2800,
            '_PrelinkExecutableSize': 0x100,
            '_PrelinkKmodInfo': 0x5800,
            'OSBundleLibraries': {},
            'IOKitPersonalities': {'StorageFixture': {'IOClass': 'SourceStorageFixture'}},
        })
        storage_header = header(11, [segment('__TEXT', 0x2800, 0x100, 0, 0x100)])
        original[0x1800:0x1800 + len(storage_header)] = storage_header
        encoded = ET.tostring(to_xml(metadata), encoding='utf-8') + b'\0'
        self.assertLessEqual(len(encoded), info['filesize'])
        original[info_at:info_at + info['filesize']] = encoded + bytes(info['filesize'] - len(encoded))
        original = bytes(original)
        digest = hashlib.sha256(original).hexdigest()
        image, report = SOURCE_PRELINK.build_candidate(
            native, original, ["test.component"], 0x4000,
            source_interfaces=["test.storage"])
        self.assertEqual(hashlib.sha256(original).hexdigest(), digest)
        self.assertEqual(report["original_sha256"], digest)
        self.assertEqual(report["source_interfaces"], ["test.storage"])
        self.assertIn("test.storage", report["codeless_components"])
        self.assertNotIn("test.storage", report["components"])
        self.assertEqual(report["candidate_sha256"], hashlib.sha256(image).hexdigest())
        record = report["source_interface_transform"]["entries"]["test.storage"]
        self.assertEqual(record["dependencies"], [])
        self.assertEqual(record["personalities"], ["StorageFixture"])

        # The kernelcache plist may share objects through ID/IDREF. Resolve both
        # the top-level component array and a bundle identifier while editing.
        idref_data = bytearray(original)
        info_blob = idref_data[info_at:info_at + info['filesize']]
        xml_root = ET.fromstring(info_blob.split(b'\0', 1)[0])
        root_children = list(xml_root)
        array_index = next(index for index in range(0, len(root_children), 2)
                           if root_children[index].tag == 'key' and
                           root_children[index].text == '_PrelinkInfoDictionary')
        component_array = root_children[array_index + 1]
        anchor_array = copy.deepcopy(component_array)
        anchor_array.set('ID', 'components-array')
        storage_entry = next(
            element for element in anchor_array
            if any(element[index].tag == 'key' and element[index].text == 'CFBundleIdentifier' and
                   element[index + 1].text == 'test.storage'
                   for index in range(0, len(element), 2)))
        storage_children = list(storage_entry)
        identifier_pairs = []
        for index in range(0, len(storage_children), 2):
            key_node, value_node = storage_children[index:index + 2]
            if key_node.text == 'CFBundleIdentifier':
                value_node = ET.Element('string', {'IDREF': 'source-identifier'})
            identifier_pairs.extend((key_node, value_node))
        identifier_anchor = ET.Element('string', {'ID': 'source-identifier'})
        identifier_anchor.text = 'test.storage'
        storage_entry[:] = [ET.Element('key'), identifier_anchor, *identifier_pairs]
        storage_entry[0].text = '_SourceIdentifierAnchor'

        rewritten_root = [ET.Element('key'), anchor_array]
        rewritten_root[0].text = '_PrelinkArrayAnchor'
        for index in range(0, len(root_children), 2):
            key_node, value_node = root_children[index:index + 2]
            if key_node.text == '_PrelinkInfoDictionary':
                value_node = ET.Element('array', {'IDREF': 'components-array'})
            rewritten_root.extend((key_node, value_node))
        xml_root[:] = rewritten_root
        idref_encoded = ET.tostring(xml_root, encoding='utf-8') + b'\0'
        self.assertLessEqual(len(idref_encoded), info['filesize'])
        idref_data[info_at:info_at + info['filesize']] = \
            idref_encoded + bytes(info['filesize'] - len(idref_encoded))
        _, idref_report = SOURCE_PRELINK.build_candidate(
            native, bytes(idref_data), ["test.component"], 0x4000,
            source_interfaces=["test.storage"])
        self.assertIn("test.storage", idref_report["codeless_components"])

        wrong_type = bytearray(original)
        wrong_header = header(2, [segment('__TEXT', 0x2800, 0x100, 0, 0x100)])
        wrong_type[0x1800:0x1800 + len(wrong_header)] = wrong_header
        with self.assertRaisesRegex(ValueError, 'MH_KEXT_BUNDLE'):
            SOURCE_PRELINK.rewrite_source_interfaces(bytes(wrong_type), ["test.storage"])

    def test_source_interfaces_are_passed_to_native_prelink(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "source.macho"
            source.write_bytes(b"native kernel")
            cache = root / "kernelcache.macho"
            cache.write_bytes(b"original kernelcache")
            spec = {
                "tool": "kits/NativePrelink/emit_prelink.py",
                "kernel": "source.macho",
                "slide": "-0x1000000",
                "roots": ["com.apple.filesystems.apfs"],
                "source_interfaces": ["com.apple.iokit.IOStorageFamily"],
                "kernelcache_sha256": "0" * 64,
            }
            ctx = SimpleNamespace(
                board={"kernel": {"prelink": spec}},
                tree=root,
                work=root / "out",
                logs=root / "logs",
                option=lambda name, env: cache if name == "kernelcache" else None,
            )
            with patch("unixnu.kernel.run") as run, patch("unixnu.kernel.log"), \
                    patch("unixnu.kernel.sha256_file", return_value="0" * 64):
                kernel.prelink(ctx)
            command = run.call_args.args[0]
            self.assertIn("--root", command)
            self.assertIn("com.apple.filesystems.apfs", command)
            self.assertIn("--source-interface", command)
            self.assertIn("com.apple.iokit.IOStorageFamily", command)

    def test_source_interface_requires_a_pinned_kernelcache(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            cache = root / "kernelcache.macho"
            cache.write_bytes(b"original kernelcache")
            spec = {
                "tool": "scripts/emit-source-interface-prelink.py",
                "kernel": "source.macho",
                "slide": "-0x1000000",
                "roots": ["com.apple.filesystems.apfs"],
                "source_interfaces": ["com.apple.iokit.IOStorageFamily"],
            }
            ctx = SimpleNamespace(
                board={"kernel": {"prelink": spec}},
                tree=root,
                work=root / "out",
                logs=root / "logs",
                option=lambda name, env: cache if name == "kernelcache" else None,
            )
            with patch("unixnu.kernel.log"), self.assertRaises(kernel.Error):
                kernel.prelink(ctx)


if __name__ == "__main__":
    unittest.main()
