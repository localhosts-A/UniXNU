from pathlib import Path
import copy
import hashlib
import importlib.util
import struct
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
import native_prelink
from native_prelink import backed_offset, macho, unserialize_xml
from test_emit_prelink import images as prelink_images
from test_native_prelink import header, segment
SPEC = importlib.util.spec_from_file_location(
    "source_interface_prelink", ROOT / "scripts/emit-source-interface-prelink.py")
SOURCE_PRELINK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SOURCE_PRELINK)
STORAGE_SYMBOL = '__ZN7IOMedia13attachToChildEP15IORegistryEntryPK15IORegistryPlane'
STORAGE_SYNCHRONIZE_SYMBOL = '__ZN7IOMedia11synchronizeEP9IOServiceyyj'


def add_native_export(data, symbol, address):
    """Append a symbol to a synthetic kernel while keeping its Mach-O valid."""
    image = bytearray(data)
    native = macho(image)
    symtab_command = next(offset for command, offset, _ in native['commands'] if command == 2)
    symoff, count, stroff, strsize = struct.unpack_from('<4I', image, symtab_command + 8)
    old_strings = image[stroff:stroff + strsize]
    records = [struct.unpack_from('<IBBHQ', image, symoff + index * 16)
               for index in range(count)]
    string_table = bytearray(b'\0')
    rebuilt_records = []
    for strx, kind, section, description, value in records:
        end = old_strings.find(b'\0', strx)
        name = old_strings[strx:end]
        new_strx = len(string_table)
        string_table.extend(name + b'\0')
        rebuilt_records.append((new_strx, kind, section, description, value))
    name = symbol.encode()
    new_strx = len(string_table)
    string_table.extend(name + b'\0')
    rebuilt_records.append((new_strx, 0x0f, 1, 0, address))
    new_symoff = 0x1100
    new_stroff = new_symoff + len(rebuilt_records) * 16
    if new_stroff + len(string_table) > len(image):
        raise AssertionError('synthetic native symbol table exceeds its image')
    for index, record in enumerate(rebuilt_records):
        struct.pack_into('<IBBHQ', image, new_symoff + index * 16, *record)
    image[new_stroff:new_stroff + len(string_table)] = string_table
    struct.pack_into('<4I', image, symtab_command + 8,
                     new_symoff, len(rebuilt_records), new_stroff, len(string_table))
    text = next(segment for segment in native['segments'] if segment['name'] == '__TEXT')
    struct.pack_into('<I', image, text['command_offset'] + 60, 5)  # VM_PROT_READ | VM_PROT_EXECUTE
    return bytes(image)


class PrelinkProfileTests(unittest.TestCase):
    def test_source_interface_data_targets_require_exact_section_and_permissions(self):
        data_const = {
            'name': '__DATA_CONST', 'va': 0x5000, 'size': 0x100,
            'initprot': 1, 'maxprot': 1,
            'sections': [{'name': '__const', 'va': 0x5020, 'size': 0x40}],
        }
        segment, section = SOURCE_PRELINK.exact_data_owner(
            [data_const], 0x5030, 'data_const', '__ZTVFixture', 'Donor')
        self.assertIs(segment, data_const)
        self.assertEqual(section['name'], '__const')

        writable = {
            'name': '__DATA', 'va': 0x6000, 'size': 0x100,
            'initprot': 3, 'maxprot': 3,
            'sections': [{'name': '__common', 'va': 0x6020, 'size': 0x40}],
        }
        _, section = SOURCE_PRELINK.exact_data_owner(
            [writable], 0x6030, 'data_rw', '__ZN7IOMedia10gMetaClassE', 'Donor')
        self.assertEqual(section['name'], '__common')

        bad_protection = {**data_const, 'maxprot': 3}
        with self.assertRaisesRegex(ValueError, 'pinned data_const layout'):
            SOURCE_PRELINK.exact_data_owner(
                [bad_protection], 0x5030, 'data_const', '__ZTVFixture', 'Donor')
        with self.assertRaisesRegex(ValueError, 'unsupported identity'):
            SOURCE_PRELINK.exact_data_owner(
                [data_const], 0x5030, 'data_const', '__ZN7IOMedia10gMetaClassE', 'Donor')

    def test_source_interface_rewrite_preserves_donor_identity_without_executable(self):
        native, original = prelink_images()
        native = add_native_export(native, STORAGE_SYMBOL, 0x100a00)
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
        storage_text = bytearray(segment('__TEXT_EXEC', 0x2800, 0x100, 0, 0x100))
        struct.pack_into('<I', storage_text, 60, 5)
        storage_header = header(11, [bytes(storage_text)])
        original[0x1800:0x1800 + len(storage_header)] = storage_header
        struct.pack_into('<Q', original, 0x3000, 0x2800)
        encoded = ET.tostring(to_xml(metadata), encoding='utf-8') + b'\0'
        self.assertLessEqual(len(encoded), info['filesize'])
        original[info_at:info_at + info['filesize']] = encoded + bytes(info['filesize'] - len(encoded))
        original = bytes(original)
        digest = hashlib.sha256(original).hexdigest()
        image, report = SOURCE_PRELINK.build_candidate(
            native, original, ["test.component"], 0x4000,
            source_interfaces=["test.storage"],
            source_interface_bindings=[{
                'bundle': 'test.storage', 'donor_va': '0x2800', 'symbol': STORAGE_SYMBOL,
            }], kernelcache_sha256=digest)
        self.assertEqual(hashlib.sha256(original).hexdigest(), digest)
        self.assertEqual(report["original_sha256"], digest)
        self.assertEqual(report["source_interfaces"], ["test.storage"])
        self.assertIn("test.storage", report["codeless_components"])
        self.assertNotIn("test.storage", report["components"])
        self.assertEqual(report["candidate_sha256"], hashlib.sha256(image).hexdigest())
        record = report["source_interface_transform"]["entries"]["test.storage"]
        self.assertEqual(record["dependencies"], [])
        self.assertEqual(record["personalities"], ["StorageFixture"])
        self.assertEqual(report['source_interface_bindings'], [{
            'bundle': 'test.storage', 'donor_va': 0x2800, 'symbol': STORAGE_SYMBOL,
            'target_kind': 'code',
            'native_va': 0x100a00, 'reference_count': 1,
        }])
        binding, = (item for item in report['bindings'] if item['target'] == 0x2800)
        self.assertEqual((binding['new_target'], binding['kind']), (0x100a00, 'source_interface'))

        unused = bytearray(original)
        struct.pack_into('<Q', unused, 0x3000, 0x9000)
        unused_digest = hashlib.sha256(unused).hexdigest()
        with self.assertRaisesRegex(ValueError, 'was not used by the pinned pointer table'):
            SOURCE_PRELINK.build_candidate(
                native, bytes(unused), ["test.component"], 0x4000,
                source_interfaces=["test.storage"],
                source_interface_bindings=[{
                    'bundle': 'test.storage', 'donor_va': '0x2800', 'symbol': STORAGE_SYMBOL,
                }], kernelcache_sha256=unused_digest)

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
            source_interfaces=["test.storage"],
            source_interface_bindings=[{
                'bundle': 'test.storage', 'donor_va': '0x2800', 'symbol': STORAGE_SYMBOL,
            }], kernelcache_sha256=hashlib.sha256(bytes(idref_data)).hexdigest())
        self.assertIn("test.storage", idref_report["codeless_components"])

        wrong_type = bytearray(original)
        wrong_header = header(2, [segment('__TEXT', 0x2800, 0x100, 0, 0x100)])
        wrong_type[0x1800:0x1800 + len(wrong_header)] = wrong_header
        with self.assertRaisesRegex(ValueError, 'MH_KEXT_BUNDLE'):
            SOURCE_PRELINK.rewrite_source_interfaces(bytes(wrong_type), ["test.storage"])

    def test_source_interface_binding_requires_unique_native_symbol_and_exact_donor_segment(self):
        native, _ = prelink_images()
        native = add_native_export(native, STORAGE_SYMBOL, 0x100a00)
        manifest = {'test.storage': {'segments': [
            {'name': '__TEXT_EXEC', 'va': 0x2800, 'size': 0x100, 'initprot': 5},
        ]}}
        binding = {'bundle': 'test.storage', 'donor_va': '0x2800', 'symbol': STORAGE_SYMBOL}
        digest = 'a' * 64
        normalized, addresses, target_layouts = SOURCE_PRELINK.normalize_source_interface_bindings(
            native, manifest, [binding], digest, digest)
        self.assertEqual(normalized[0x2800]['symbol'], STORAGE_SYMBOL)
        self.assertEqual(addresses, {0x2800: 0x100a00})
        self.assertEqual(target_layouts, {})

        native = add_native_export(native, STORAGE_SYNCHRONIZE_SYMBOL, 0x100b00)
        multi_manifest = {'test.storage': {'segments': [
            {'name': '__TEXT_EXEC', 'va': 0x2800, 'size': 0x100, 'initprot': 5},
        ]}}
        multi_specs = [binding, {
            'bundle': 'test.storage', 'donor_va': '0x2840', 'symbol': STORAGE_SYNCHRONIZE_SYMBOL,
        }]
        multi_names, multi_addresses, _ = SOURCE_PRELINK.normalize_source_interface_bindings(
            native, multi_manifest, multi_specs, digest, digest)
        self.assertEqual(set(multi_names), {0x2800, 0x2840})
        self.assertEqual(multi_addresses, {0x2800: 0x100a00, 0x2840: 0x100b00})

        with self.assertRaisesRegex(ValueError, 'exact pinned kernelcache'):
            SOURCE_PRELINK.normalize_source_interface_bindings(native, manifest, [binding], 'b' * 64, digest)
        with self.assertRaisesRegex(ValueError, 'not in one declared segment'):
            SOURCE_PRELINK.normalize_source_interface_bindings(
                native, manifest, [{**binding, 'donor_va': '0x2900'}], digest, digest)
        non_executable = {'test.storage': {'segments': [
            {'name': '__TEXT_EXEC', 'va': 0x2800, 'size': 0x100, 'initprot': 1},
        ]}}
        with self.assertRaisesRegex(ValueError, 'not in one declared executable segment'):
            SOURCE_PRELINK.normalize_source_interface_bindings(native, non_executable, [binding], digest, digest)
        overlapping = {**manifest, 'other.storage': {'segments': [
            {'name': '__TEXT_EXEC', 'va': 0x2800, 'size': 0x20, 'initprot': 5},
        ]}}
        with self.assertRaisesRegex(ValueError, 'not in one declared segment'):
            SOURCE_PRELINK.normalize_source_interface_bindings(native, overlapping, [binding], digest, digest)
        with self.assertRaisesRegex(ValueError, '0 address candidates'):
            SOURCE_PRELINK.normalize_source_interface_bindings(
                native, manifest, [{**binding, 'symbol': '_missing_source_symbol'}], digest, digest)
        ambiguous = add_native_export(native, STORAGE_SYMBOL, 0x100a04)
        with self.assertRaisesRegex(ValueError, '2 address candidates'):
            SOURCE_PRELINK.normalize_source_interface_bindings(ambiguous, manifest, [binding], digest, digest)
        non_exec_native = bytearray(native)
        text = next(segment for segment in macho(non_exec_native)['segments'] if segment['name'] == '__TEXT')
        struct.pack_into('<I', non_exec_native, text['command_offset'] + 60, 3)
        with self.assertRaisesRegex(ValueError, 'not in one aligned executable kernel segment'):
            SOURCE_PRELINK.normalize_source_interface_bindings(
                bytes(non_exec_native), manifest, [binding], digest, digest)

        token = SOURCE_PRELINK._SOURCE_INTERFACE_CONTEXT.set({
            'binding_specs': {0x2800: {'bundle': 'test.storage', 'donor_va': 0x2800,
                                       'symbol': STORAGE_SYMBOL}},
            'native_addresses': {0x2800: 0x100a00},
            'resolved_counts': {},
        })
        try:
            with self.assertRaisesRegex(ValueError, 'unexpectedly has kernelcache exports'):
                SOURCE_PRELINK.resolve_kernel_binding(
                    0x2800, {0x2800: ['_unexpected_donor_export']}, {STORAGE_SYMBOL: {0x100a00}})
        finally:
            SOURCE_PRELINK._SOURCE_INTERFACE_CONTEXT.reset(token)

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
                "source_interface_bindings": [{
                    "bundle": "com.apple.iokit.IOStorageFamily",
                    "donor_va": "0xfffffff0059762a8",
                    "symbol": STORAGE_SYMBOL,
                }],
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
            self.assertIn("--source-interface-binding", command)
            self.assertIn("com.apple.iokit.IOStorageFamily=0xfffffff0059762a8=" + STORAGE_SYMBOL + "=code", command)
            self.assertIn("--kernelcache-sha256", command)
            self.assertIn("0" * 64, command)

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
