"""Userdata payload: a raw image for a named device partition.

It holds the unchanged OS and SystemOS disk images of the IPSW, byte for byte, behind a small manifest
(the SHNGROOT layout the native UFS root driver reads):

    0x000000  manifest block (4096 bytes), zero padding to 1 MiB
    0x100000  window "OS"        the whole attached OS disk image, 1 MiB aligned
    ....      window "SystemOS"  the whole attached SystemOS disk image, 1 MiB aligned

Manifest (little endian): "SHNGROOT", u32 version=1, u32 count, u64 image_bytes, then `count` 128-byte entries
{char name[32], u64 offset, u64 size, u32 block_size=512, u32 flags(1=read-only), u8 sha256[32], 40 reserved},
and at byte 4064 the SHA-256 of bytes 0..4063.

The images come from the IPSW (a local file or a remote URL, see ipsw.py) and are checked against the pins of
board.toml. The disk images are attached read-only without mounting (macOS hdiutil). Nothing is flashed: the
report prints the fastboot command, to be run by the user. `partition_name` controls that command and defaults
to `userdata`; it does not describe the UFS LUN used by an XNU root reader.
"""
import hashlib
import json
import plistlib
import re
import struct
import subprocess

from .ipsw import Ipsw
from .util import Error, log, run, tool

MIB = 1 << 20
CHUNK = 8 * MIB


def paths(ctx):
    out = ctx.work / 'userdata'
    return out, out / 'userdata-root.img', out / 'userdata-root.json'


def image_size(dmg):
    info = subprocess.run(['hdiutil', 'imageinfo', '-plist', str(dmg)], capture_output=True, check=True).stdout
    return plistlib.loads(info)['Size Information']['Total Bytes']


def partition_name(spec):
    """Return the fastboot partition target, keeping it to a single safe token."""
    name = spec.get('partition_name', 'userdata')
    if not isinstance(name, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,36}', name):
        raise Error('userdata partition_name must be 1-36 ASCII letters, digits, underscores or hyphens')
    return name


def partition_lun(spec):
    """Optional UFS LUN metadata for the XNU reader; fastboot does not select a LUN."""
    lun = spec.get('lun')
    if lun is not None and (isinstance(lun, bool) or not isinstance(lun, int) or not 0 <= lun <= 7):
        raise Error('userdata lun must be an integer from 0 through 7')
    return lun


def partition_capacity(spec):
    """Capacity of the configured partition geometry in bytes."""
    first, last = spec.get('first_lba'), spec.get('last_lba')
    block = spec.get('lba_bytes', 4096)
    if any(isinstance(value, bool) or not isinstance(value, int) for value in (first, last, block)):
        raise Error('userdata first_lba, last_lba and lba_bytes must be integers')
    if first < 0 or last < first:
        raise Error('userdata LBA range must be non-negative and last_lba must be at least first_lba')
    if block < 512 or block > 65536 or block & (block - 1):
        raise Error('userdata lba_bytes must be a power of two from 512 through 65536')
    return (last - first + 1) * block


def flash_command(spec, image):
    return f'fastboot flash {partition_name(spec)} {image}'


def check_capacity(image_bytes, capacity):
    if isinstance(image_bytes, bool) or not isinstance(image_bytes, int) or image_bytes < 0:
        raise Error('userdata image size must be a non-negative integer')
    if image_bytes > capacity:
        raise Error('the image is larger than the target partition')


def verify_built_image(image, entries, capacity):
    """Validate the complete container and every byte of its read-only windows before it can be flashed."""
    image_bytes = image.stat().st_size
    check_capacity(image_bytes, capacity)
    if image_bytes < MIB or image_bytes & 4095:
        raise Error('userdata image must be at least 1 MiB and 4 KiB aligned')
    if not entries or len(entries) > 4:
        raise Error('userdata image must contain 1-4 root windows')

    with image.open('rb', buffering=0) as stream:
        header = stream.read(4096)
        if len(header) != 4096 or header[:8] != b'SHNGROOT':
            raise Error('userdata image has no complete SHNGROOT header')
        version, count, declared_bytes = struct.unpack_from('<IIQ', header, 8)
        if version != 1 or count != len(entries) or declared_bytes != image_bytes:
            raise Error('userdata manifest version, window count or byte size does not match the image')
        if hashlib.sha256(header[:4064]).digest() != header[4064:4096]:
            raise Error('userdata manifest header checksum mismatch')
        if any(header[24 + count * 128:4064]):
            raise Error('userdata manifest has unexpected trailing entry data')

        seen = set()
        normalized = []
        prior_end = MIB
        for index, (expected_name, expected_offset, expected_size, expected_digest) in enumerate(entries):
            if (not isinstance(expected_name, str) or not re.fullmatch(r'[A-Za-z0-9_-]{1,31}', expected_name) or
                    expected_name in seen):
                raise Error('userdata window names must be unique bounded ASCII tokens')
            seen.add(expected_name)
            if (isinstance(expected_offset, bool) or not isinstance(expected_offset, int) or
                    isinstance(expected_size, bool) or not isinstance(expected_size, int) or
                    expected_offset < MIB or expected_offset % MIB or expected_size <= 0 or expected_size % 512 or
                    expected_offset < prior_end or expected_offset >= image_bytes or
                    expected_size > image_bytes - expected_offset):
                raise Error(f'userdata window {expected_name} has invalid or overlapping bounds')
            padded_end = expected_offset + ((expected_size + 4095) & ~4095)
            if padded_end > image_bytes or not isinstance(expected_digest, bytes) or len(expected_digest) != 32:
                raise Error(f'userdata window {expected_name} has invalid padded bounds or digest')
            raw = struct.unpack_from('<32sQQII32s40s', header, 24 + 128 * index)
            raw_name, offset, size, block_size, flags, digest, reserved = raw
            if (raw_name != expected_name.encode('ascii').ljust(32, b'\0') or offset != expected_offset or
                    size != expected_size or block_size != 512 or flags != 1 or digest != expected_digest or
                    reserved != bytes(40)):
                raise Error(f'userdata manifest entry {index} does not match the built window')
            normalized.append((expected_name, expected_offset, expected_size, expected_digest, padded_end))
            prior_end = padded_end

        # Hash the full image while checking every inter-window and final padding byte is zero.
        whole = hashlib.sha256()
        whole.update(header)
        cursor = 4096

        def consume_padding(end):
            nonlocal cursor
            while cursor < end:
                data = stream.read(min(CHUNK, end - cursor))
                if not data or any(data):
                    raise Error(f'userdata padding is short or nonzero at byte {cursor}')
                whole.update(data)
                cursor += len(data)

        for name, offset, size, expected_digest, padded_end in normalized:
            consume_padding(offset)
            window = hashlib.sha256()
            remaining = size
            while remaining:
                data = stream.read(min(CHUNK, remaining))
                if not data:
                    raise Error(f'userdata window {name} is truncated at byte {cursor}')
                window.update(data)
                whole.update(data)
                cursor += len(data)
                remaining -= len(data)
            if window.digest() != expected_digest:
                raise Error(f'userdata window {name} digest mismatch')
            consume_padding(padded_end)
        consume_padding(image_bytes)
        if stream.read(1):
            raise Error('userdata image grew while being verified')
    return whole.hexdigest()


def verify_attached_disk(device, expected_size):
    """Require hdiutil's whole-disk mapping to be the exact read-only virtual media."""
    try:
        info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', device]))
    except (OSError, subprocess.CalledProcessError, plistlib.InvalidFileException) as error:
        raise Error(f'could not verify attached disk {device}: {error}') from error
    if (info.get('BusProtocol') != 'Disk Image' or info.get('Writable') is not False or
            info.get('DeviceNode') != device or info.get('TotalSize') != expected_size):
        raise Error(f'{device} is not the expected read-only disk image of {expected_size} bytes')


def attach(dmg, expected_size):
    """Attach read-only without mounting; return only a verified whole-disk device."""
    out = subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', str(dmg)],
                         capture_output=True, text=True, check=True).stdout
    device = None
    for line in out.splitlines():
        match = re.match(r'(/dev/disk\d+)\s', line)
        if match:
            device = match.group(1)
            break
    if not device:
        raise Error('could not find the attached disk device')
    try:
        verify_attached_disk(device, expected_size)
    except Exception:
        subprocess.run(['hdiutil', 'detach', device], capture_output=True)
        raise
    return device


def build(ctx, dry_run=False):
    log('userdata payload')
    spec = ctx.board.get('userdata')
    if not spec:
        raise Error('board.toml has no [userdata]')
    target_partition = partition_name(spec)
    target_lun = partition_lun(spec)
    capacity = partition_capacity(spec)
    tool('hdiutil', 'the disk images are attached with macOS hdiutil')
    source = Ipsw(ctx)
    if dry_run:
        lun_text = f', UFS LUN {target_lun}' if target_lun is not None else ''
        log(f'userdata: target {target_partition}{lun_text}, capacity {capacity} bytes')
        for window in spec['windows']:
            name, size, _ = source.info(window['component'])
            log(f'userdata: {window["name"]} would be extracted from {name} ({size >> 20} MiB)')
        log('userdata: dry run, nothing extracted or written')
        return None
    sources = [(window['name'], source.extract(window['component'])) for window in spec['windows']]
    layout, cursor = [], MIB
    for name, dmg in sources:
        size = image_size(dmg)
        layout.append((name, cursor, size))
        cursor += (size + MIB - 1) // MIB * MIB
    total = cursor
    log(f'userdata: image {total} bytes ({total / 2**30:.2f} GiB) in a partition of {capacity} bytes')
    check_capacity(total, capacity)
    for name, dmg in sources:
        log(f'userdata: {name} from {dmg.name}')
    out, image, report = paths(ctx)
    out.mkdir(parents=True, exist_ok=True)
    entries = []
    with image.open('wb') as target:
        target.truncate(total)
        for (name, offset, size), (_, dmg) in zip(layout, sources):
            device = attach(dmg, size)
            try:
                raw = device.replace('/dev/disk', '/dev/rdisk')
                digest = hashlib.sha256()
                target.seek(offset)
                done = 0
                with open(raw, 'rb', buffering=0) as handle:
                    while done < size:
                        want = min(CHUNK, size - done)
                        data = handle.read(want)
                        if len(data) != want:
                            raise Error(f'short read from {raw} at {done}')
                        digest.update(data)
                        if target.write(data) != len(data):
                            raise Error(f'short write to userdata image at {offset + done}')
                        done += want
            finally:
                subprocess.run(['hdiutil', 'detach', device], capture_output=True)
            entries.append((name, offset, size, digest.digest()))
            log(f'userdata: {name} {size} bytes at {offset:#x} sha256 {digest.hexdigest()}')
        header = bytearray(4096)
        header[0:8] = b'SHNGROOT'
        struct.pack_into('<IIQ', header, 8, 1, len(entries), total)
        for index, (name, offset, size, digest) in enumerate(entries):
            struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index, name.encode(), offset, size, 512, 1, digest, bytes(40))
        header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
        target.seek(0)
        if target.write(header) != len(header):
            raise Error('short write of userdata manifest')
    image_digest = verify_built_image(image, entries, capacity)
    command = flash_command(spec, image)
    data = {'image_bytes': total, 'image_sha256': image_digest, 'manifest_sha256': hashlib.sha256(bytes(header)).hexdigest(),
            'windows': [{'name': n, 'offset': o, 'size': s, 'sha256': d.hex()} for n, o, s, d in entries],
            'partition_name': target_partition, 'lun': target_lun, 'partition_bytes': capacity, 'userdata_bytes': capacity,
            'flash_command': command,
            'source': 'remote' if source.remote else 'local', 'hardware_tested': False}
    report.write_text(json.dumps(data, indent=2) + '\n')
    log(f'userdata: {image}')
    log(f'flash it yourself with: {command}')
    return image
