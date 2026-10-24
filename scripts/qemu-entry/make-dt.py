#!/usr/bin/env python3
"""Generate a synthetic Apple-format DT for the single-CPU QEMU entry fixture.

This is neither sheng board data nor an iPad device identity. The arm-io
range describes QEMU PL011 solely to pass early platform discovery.
"""
from pathlib import Path
import argparse
import os
import struct
import zlib
import uuid
import platform
import re
import subprocess


def node(name, properties=(), children=()):
    properties = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(properties), len(children))
    for key, data in properties:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """A fresh volatile CHRP bank for upstream IODTNVRAM, with no controller."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))),
            ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)),
            ('nvram-proxy-data', bytes(image))]


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'build/xnu-entry/qemu-test.adt')
parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), help='Request a real contiguous DarwinBoot allocation through chosen/darwinboot-memory-size; 256 MiB to 4 GiB in 2 MiB steps')
parser.add_argument('--dram-size', type=lambda value: int(value, 0), default=0x20000000)
parser.add_argument('--qemu-accel', choices=['tcg', 'hvf'], default='tcg', help='Offline CPU description; HVF uses the actual host counter frequency and a generic ARM CPU identity')
parser.add_argument('--qemu-vmm-present', action='store_true', help='Report the actual offline QEMU environment through the native XNU defaults ABI; never board data')
parser.add_argument('--armvirt', action='store_true', help='Describe actual QEMU virt/A710/GICv3 devices for the platform driver')
parser.add_argument('--platform-compatible', choices=['linux,dummy-virt', 'unmatched-test-platform', 'xiaomi,sheng'], default='linux,dummy-virt')
parser.add_argument('--smp', type=int, default=1, help='Number of QEMU CPUs; Affinity0 numbers them, PSCI conduit is hvc (virt without EL2)')
parser.add_argument('--armcpu', action='store_true', help='Add the real QEMU GICv3 distributor/redistributor DT resources')
parser.add_argument('--data-ramdisk', action='store_true', help='Provide a writable scratch RAMDisk sidecar while keeping actual disk root matching')
parser.add_argument('--ramdisk', action='store_true', help='Provide an owned memory-disk binding placeholder for the loader')
parser.add_argument('--test-service', action='append', default=[], metavar='NODE:COMPATIBLE',
                    help='Add an explicitly named offline software-service nub, with no MMIO resources')
parser.add_argument('--nvram-proxy', action='store_true', help='Initialize real IODTNVRAM from an empty volatile CHRP bank; no persistence controller')
parser.add_argument('--conflicting-boot-manifest', action='store_true', help='Negative offline identity-collision input: install an untrusted A5 hash which owner publication must refuse to replace')
parser.add_argument('--crypto-hash-method', choices=['sha2-384', 'sha1'], help='Explicit Image4 hash algorithm; target J120 DT specifies sha2-384')
parser.add_argument('--osenvironment', choices=['ramdisk','recoveryos','main'], help='Explicit /chosen/osenvironment fixture; the kernel publishes it as hw.osenvironment')
parser.add_argument('--amfi-platform-only', action='store_true', help='Require platform code in the offline AMFI policy fixture')
parser.add_argument('--trust-cache', action='store_true', help='Provide a zeroed owned TrustCache sidecar binding')
parser.add_argument('--virtio-mmio-fdt', type=Path, help='Describe actual VirtIO resource windows from a QEMU-generated FDT')
parser.add_argument('--root-match-volume-uuid', type=uuid.UUID, help='Match an actual IOMedia UUID through the source root-matching mechanism')
args = parser.parse_args()
if args.owned_memory_size is not None and (not 0x10000000 <= args.owned_memory_size <= 0x100000000 or args.owned_memory_size % 0x200000):
    parser.error('Owned memory must be 256 MiB to 4 GiB, aligned to 2 MiB')
timebase = 62500000
if args.qemu_accel == 'hvf':
    if platform.system() != 'Darwin' or platform.machine() != 'arm64' or not args.armvirt or not args.qemu_vmm_present:
        parser.error('HVF requires the actual macOS ARM64 QEMU fixture and explicit VMM declaration')
    timebase = int(subprocess.check_output(['sysctl', '-n', 'hw.tbfrequency'], text=True))
    if not 0 < timebase < 1 << 32:
        parser.error('Host counter frequency is outside the DT u32 ABI')
if args.qemu_vmm_present and not args.armvirt:
    parser.error('--qemu-vmm-present requires the actual QEMU platform fixture')
if args.armcpu and not args.armvirt:
    parser.error('--armcpu requires --armvirt')
test_nodes = []
test_names = {'intc', 'ufshc', 'ufsphy'}
if args.test_service and not args.armvirt:
    parser.error('--test-service requires the actual offline virt fixture')
for service in args.test_service:
    pair = service.split(':')
    if len(pair) != 2 or not re.fullmatch(r'[a-z][a-z0-9-]{0,31}', pair[0]) or \
            not re.fullmatch(r'[a-zA-Z0-9,_-]{1,64}', pair[1]) or pair[0] in test_names:
        parser.error('Test services require unique bounded NODE:COMPATIBLE names')
    test_names.add(pair[0])
    test_nodes.append(node(pair[0], [('compatible', pair[1].encode() + b'\0')]))
if not 1 <= args.smp <= 8 or (args.smp > 1 and not args.armcpu):
    parser.error('--smp is 1..8 and needs --armcpu')
if args.root_match_volume_uuid and (not args.virtio_mmio_fdt or args.ramdisk):
    parser.error('Direct volume matching requires a real offline disk and conflicts with a RAM root')
if args.data_ramdisk and (not args.root_match_volume_uuid or args.ramdisk):
    parser.error('Scratch Data requires actual disk root matching and no RAM root')
root_matching = []
if args.root_match_volume_uuid:
    xml = ('<dict><key>IOProviderClass</key><string>IOMedia</string>'
           '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
           str(args.root_match_volume_uuid).upper() + '</string></dict></dict>\0')
    root_matching = [('root-matching', xml.encode())]
virtio_nodes = []
if args.virtio_mmio_fdt:
    if not args.armvirt:
        parser.error('--virtio-mmio-fdt requires --armvirt')
    blob = args.virtio_mmio_fdt.read_bytes()
    magic, total, structure, strings, _, version, _, _, string_bytes, structure_bytes = struct.unpack_from('>10I', blob)
    if magic != 0xd00dfeed or total > len(blob) or version != 17 or strings + string_bytes > total or structure + structure_bytes > total:
        raise SystemExit('Unexpected actual QEMU FDT')
    stack, entries = [], []
    cursor = structure
    while cursor < structure + structure_bytes:
        tag = struct.unpack_from('>I', blob, cursor)[0]
        cursor += 4
        if tag == 1:
            end = blob.index(b'\0', cursor)
            stack.append(dict(name=blob[cursor:end].decode(), properties={}))
            cursor = (end + 4) & ~3
        elif tag == 2:
            entry = stack.pop()
            properties = entry['properties']
            if b'virtio,mmio' in properties.get('compatible', b'').split(b'\0'):
                parent = stack[-1]['properties']
                if (parent.get('#address-cells'), parent.get('#size-cells')) != (struct.pack('>I', 2), struct.pack('>I', 2)):
                    raise SystemExit('Unexpected QEMU VirtIO parent address encoding')
                values = struct.unpack('>4I', properties['reg'])
                address, size = (values[0] << 32) | values[1], (values[2] << 32) | values[3]
                if not 0x08000000 <= address < address + size <= 0x0c000000 or size < 0x108 or 'dma-coherent' not in properties:
                    raise SystemExit('VirtIO resource is outside the established QEMU bus or not coherent')
                entries.append((address, size))
        elif tag == 3:
            length, name_offset = struct.unpack_from('>II', blob, cursor)
            cursor += 8
            if name_offset >= string_bytes or cursor + length > structure + structure_bytes:
                raise SystemExit('Malformed QEMU FDT property')
            name = blob[strings + name_offset:blob.index(b'\0', strings + name_offset)].decode()
            stack[-1]['properties'][name] = blob[cursor:cursor + length]
            cursor = (cursor + length + 3) & ~3
        elif tag == 9:
            break
        elif tag != 4:
            raise SystemExit('Unknown QEMU FDT tag')
    if not entries or len(entries) != len(set(entries)):
        raise SystemExit('Missing or repeated actual VirtIO resource windows')
    for index, (address, size) in enumerate(sorted(entries)):
        virtio_nodes.append(node('virtio-mmio-' + str(index), [('compatible', b'virtio,mmio\0'),
            ('reg', struct.pack('<QQ', address - 0x08000000, size)), ('dma-coherent', b'')]))
cpu = node('cpu0', [('state', b'running\0'), ('reg', struct.pack('<I', 0)),
                    *([('device_type', b'cpu\0'), ('compatible', b'arm,cortex-a710\0' if args.qemu_accel == 'tcg' else b'arm,armv8\0')] if args.armvirt else []),
                    ('timebase-frequency', struct.pack('<I', timebase)),
                    *([('clock-frequency', struct.pack('<I', 1000000000))] if args.qemu_accel == 'tcg' else [])])
tree = node('device-tree', [('model', b'qemu-entry-fixture\0'),
                          *([('#address-cells', struct.pack('<I', 2)), ('#size-cells', struct.pack('<I', 2))] if virtio_nodes else []),
                          *([('compatible', args.platform_compatible.encode() + b'\0')] if args.armvirt else [])], [
    node('chosen', [('random-seed', os.urandom(64)),
                    # This fixture's writable Data lives only in the owned
                    # RAMDisk. Publish its actual nonpersistent lifetime via
                    # XNU's standard hw.ephemeral_storage device-tree input.
                    *([('ephemeral-storage', struct.pack('<I', 1))] if args.data_ramdisk else []),
                    *([('osenvironment', args.osenvironment.encode() + b'\0')] if args.osenvironment else []),
                    *([('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size))] if args.owned_memory_size else []),
                    *root_matching,
                    *([('boot-manifest-hash', bytes([0xa5]) * 48)] if args.conflicting_boot_manifest else []),
                    ('dram-base', struct.pack('<Q', 0x40000000)),
                    ('dram-size', struct.pack('<Q', args.dram_size)),
                    *([('crypto-hash-method', args.crypto_hash_method.encode() + b'\0')] if args.crypto_hash_method else []),
                    *([('amfi-only-platform-code', struct.pack('<I', 1))] if args.amfi_platform_only else []),
                    *(empty_nvram_proxy() if args.nvram_proxy else [])], [node('memory-map',
                    [*([('RAMDisk', bytes(16))] if args.ramdisk or args.data_ramdisk else []),
                     *([('TrustCache', bytes(16))] if args.trust_cache else [])]),
                     *([node('manifest-properties')] if args.armvirt else [])]),
    node('cpus', children=[cpu, *[node('cpu%d' % index, [('state', b'waiting\0'), ('reg', struct.pack('<I', index)),
                    ('device_type', b'cpu\0'), ('compatible', b'arm,cortex-a710\0' if args.qemu_accel == 'tcg' else b'arm,armv8\0'),
                    ('timebase-frequency', struct.pack('<I', timebase))]) for index in range(1, args.smp)]]),
    *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', b'hvc\0')])] if args.smp > 1 else []), node('defaults',
        [('vmm-present', struct.pack('<I', 1))] if args.qemu_vmm_present else []),
    *([node('options')] if args.nvram_proxy else []),
    *([node('product', [('product-name', b'QEMU virt\0'),
                        ('model', b'linux,dummy-virt\0')])] if args.armvirt else []),
    node('arm-io', [('device_type', b'qemu-virt\0'),
                    *([('#address-cells', struct.pack('<I', 2)), ('#size-cells', struct.pack('<I', 2))] if virtio_nodes else []),
                    ('ranges', struct.pack('<QQQ', 0, 0x08000000, 0x04000000) if args.armvirt
                     else struct.pack('<QQQ', 0, 0x09000000, 0x1000))],
         ([node('intc', [('compatible', b'arm,gic-v3\0'),
                        ('reg', struct.pack('<4Q', 0, 0x10000, 0xa0000, 0xf60000))])]
         if args.armcpu else []) +
         test_nodes +
         ([node('ufshc', [('compatible', b'qcom,sm8550-ufshc\0'),
                          ('reg', struct.pack('<2Q', 0x03ff0000, 0x3000))]),
           node('ufsphy', [('compatible', b'qcom,sm8550-qmp-ufs-phy\0'),
                           ('reg', struct.pack('<2Q', 0x03fe0000, 0x2000))])]
         if args.armvirt else []) + virtio_nodes)])
output = args.output
output.parent.mkdir(parents=True, exist_ok=True)
output.write_bytes(tree)
print(f'Synthetic QEMU DT: {len(tree)} bytes, timebase {timebase} Hz')
