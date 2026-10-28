"""Validate the QEMU PCI host aperture copied into the offline Apple DT."""
import struct
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
GENERATOR = ROOT / "scripts/qemu-entry/make-dt.py"
ECAM = (0x40, 0x10000000, 0, 0x10000000)
BUS_RANGE = (0, 255)
RANGES = (
    0x01000000, 0, 0, 0, 0x3EFF0000, 0, 0x10000,
    0x02000000, 0, 0x10000000, 0, 0x10000000, 0, 0x2EFF0000,
    0x03000000, 0x80, 0, 0x80, 0, 0x80, 0,
)


def fdt(properties, extra_hosts=()):
    host_properties = [
        ("compatible", b"pci-host-ecam-generic\0"),
        ("device_type", b"pci\0"),
        ("#address-cells", struct.pack(">I", 3)),
        ("#size-cells", struct.pack(">I", 2)),
        ("reg", struct.pack(">4I", *ECAM)),
        ("bus-range", struct.pack(">2I", *BUS_RANGE)),
        ("ranges", struct.pack(">21I", *RANGES)),
        ("dma-coherent", b""),
    ]
    host_properties = [(key, value) for key, value in host_properties if key not in properties]
    host_properties.extend((key, value) for key, value in properties.items() if value is not None)
    root_properties = [("#address-cells", struct.pack(">I", 2)),
                       ("#size-cells", struct.pack(">I", 2))]
    nodes = [("pcie@10000000", host_properties), *extra_hosts]

    strings = bytearray()
    name_offsets = {}

    def name_offset(name):
        if name not in name_offsets:
            name_offsets[name] = len(strings)
            strings.extend(name.encode("ascii") + b"\0")
        return name_offsets[name]

    structure = bytearray()

    def emit_node(name, props, children=()):
        structure.extend(struct.pack(">I", 1))
        encoded = name.encode("ascii") + b"\0"
        structure.extend(encoded)
        structure.extend(bytes(-len(encoded) % 4))
        for key, value in props:
            structure.extend(struct.pack(">I", 3))
            structure.extend(struct.pack(">II", len(value), name_offset(key)))
            structure.extend(value)
            structure.extend(bytes(-len(value) % 4))
        for child_name, child_props in children:
            emit_node(child_name, child_props)
        structure.extend(struct.pack(">I", 2))

    emit_node("", root_properties, nodes)
    structure.extend(struct.pack(">I", 9))
    reserve_offset = 40
    structure_offset = reserve_offset + 16
    strings_offset = structure_offset + len(structure)
    total = strings_offset + len(strings)
    header = struct.pack(">10I", 0xD00DFEED, total, structure_offset,
                         strings_offset, reserve_offset, 17, 16, 0,
                         len(strings), len(structure))
    return header + bytes(16) + bytes(structure) + bytes(strings)


def parse_adt_node(blob, offset=0):
    property_count, child_count = struct.unpack_from("<II", blob, offset)
    cursor = offset + 8
    properties = {}
    for _ in range(property_count):
        raw_name, length = struct.unpack_from("<32sI", blob, cursor)
        cursor += 36
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        properties[name] = blob[cursor:cursor + length]
        cursor = (cursor + length + 3) & ~3
    children = []
    for _ in range(child_count):
        child, cursor = parse_adt_node(blob, cursor)
        children.append(child)
    return {"properties": properties, "children": children}, cursor


class QemuPcieFdtTests(unittest.TestCase):
    def run_generator(self, blob, armvirt=True):
        temporary = tempfile.TemporaryDirectory(prefix="qemu-pcie-fdt-")
        self.addCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        source, output = directory / "qemu.dtb", directory / "qemu.adt"
        source.write_bytes(blob)
        command = [sys.executable, str(GENERATOR), "--qemu-pcie-fdt", str(source),
                   "--output", str(output)]
        if armvirt:
            command.append("--armvirt")
        result = subprocess.run(command, text=True, capture_output=True, timeout=15)
        return result, output

    def test_qemu_host_aperture_and_windows_are_copied(self):
        result, output = self.run_generator(fdt({}))
        self.assertEqual(result.returncode, 0, result.stderr)
        tree, consumed = parse_adt_node(output.read_bytes())
        self.assertEqual(consumed, output.stat().st_size)
        arm_io = next(child for child in tree["children"]
                      if child["properties"].get("name") == b"arm-io\0")
        host = next(child for child in arm_io["children"]
                    if child["properties"].get("name") == b"qemu-pcie-host\0")
        properties = host["properties"]
        self.assertEqual(properties["compatible"], b"qemu,virt-pcie-host\0pci-host-ecam-generic\0")
        self.assertEqual(properties["reg"], struct.pack("<QQ", 0x4010000000, 0x10000000))
        self.assertEqual(properties["bus-range"], struct.pack("<II", 0, 255))
        self.assertEqual(properties["ranges"], struct.pack("<21I", *RANGES))
        self.assertIn("dma-coherent", properties)

    def test_refuses_unqualified_or_translated_dma_hosts(self):
        cases = [
            {"dma-coherent": None},
            {"iommu-map": struct.pack(">4I", 0, 1, 0, 1)},
            {"dma-ranges": struct.pack(">4I", 0, 0, 0, 0)},
        ]
        for properties in cases:
            with self.subTest(properties=properties):
                result, _ = self.run_generator(fdt(properties))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("QEMU PCI host", result.stderr)

    def test_refuses_duplicate_host_and_malformed_ranges(self):
        host_properties = [
            ("compatible", b"pci-host-ecam-generic\0"),
            ("device_type", b"pci\0"),
            ("#address-cells", struct.pack(">I", 3)),
            ("#size-cells", struct.pack(">I", 2)),
        ]
        duplicate = fdt({}, extra_hosts=(("pcie@20000000", host_properties),))
        result, _ = self.run_generator(duplicate)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("exactly one QEMU generic PCI host", result.stderr)

        malformed = fdt({"ranges": struct.pack(">20I", *RANGES[:-1])})
        result, _ = self.run_generator(malformed)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("ranges", result.stderr)

    def test_pcie_export_requires_the_qemu_virt_profile(self):
        result, _ = self.run_generator(fdt({}), armvirt=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --armvirt", result.stderr)


if __name__ == "__main__":
    unittest.main()
