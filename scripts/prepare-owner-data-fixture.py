#!/usr/bin/env python3
"""Create a small 21H461 APFS Data-seed candidate from the exact OS private/var tree.

This creates a 32 MiB raw APFS image for offline boot experiments; it does not
create device keybags or qualify a production Data volume. Run as root on
macOS so ditto can preserve the source UID/GID, ACLs, xattrs and resource forks.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import stat
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LAUNCHD_SHA256 = '8cec9a3c44a443b08e977ccc5d577850316ad428c575dc01f09b018dbd5b6a25'
DYLD_SHA256 = 'f8df3c58fae6e409db740df22c98aa699aee110b73523541a63f1bd5e23cf705'
MIB = 1024 * 1024
IMAGE_BYTES = 32 * MIB


def select_whole_disk(system_entities):
    entities = [entry for entry in system_entities if isinstance(entry, dict)]
    devices = [entry for entry in entities
               if isinstance(entry.get('dev-entry'), str) and
               re.fullmatch(r'/dev/disk[0-9]+', entry['dev-entry'])]
    partitioned = [entry for entry in devices if entry.get('content-hint') == 'GUID_partition_scheme']
    if len(partitioned) == 1:
        return partitioned[0]['dev-entry']
    if len(partitioned) > 1:
        raise SystemExit('Require one GUID partition-scheme parent for the Data seed image')

    untyped = [entry for entry in devices if not entry.get('content-hint')]
    if len(untyped) == 1:
        return untyped[0]['dev-entry']

    apfs_parents = {match.group(1) for entry in entities
                    if entry.get('content-hint') == 'Apple_APFS' and
                    isinstance(entry.get('dev-entry'), str)
                    for match in [re.fullmatch(r'(/dev/disk[0-9]+)s[0-9]+', entry['dev-entry'])]
                    if match}
    matching = [entry['dev-entry'] for entry in devices if entry['dev-entry'] in apfs_parents]
    if len(matching) == 1:
        return matching[0]
    if not matching and len(devices) == 1:
        return devices[0]['dev-entry']
    summary = ', '.join(f"{entry.get('dev-entry')}:{entry.get('content-hint', '?')}" for entry in entities)
    raise SystemExit('Require one GUID partition-scheme parent for the Data seed image; '
                     f'attach reported [{summary}]')


def select_apfs_partition(whole_device, listing):
    disk_name = Path(whole_device).name
    if not re.fullmatch(r'disk[0-9]+', disk_name):
        raise SystemExit('Invalid Data seed whole-disk device name')
    disks = [entry for entry in listing.get('AllDisksAndPartitions', [])
             if entry.get('DeviceIdentifier') == disk_name]
    if len(disks) != 1:
        raise SystemExit('diskutil did not identify the Data seed whole disk')
    partitions = [entry for entry in disks[0].get('Partitions', [])
                  if entry.get('Content') == 'Apple_APFS' and
                  isinstance(entry.get('DeviceIdentifier'), str) and
                  re.fullmatch(r'disk[0-9]+s[0-9]+', entry['DeviceIdentifier'])]
    if len(partitions) != 1:
        raise SystemExit('Require exactly one Apple_APFS partition in the Data seed disk image')
    return '/dev/' + partitions[0]['DeviceIdentifier']


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as source:
        while chunk := source.read(8 * MIB):
            digest.update(chunk)
    return digest.hexdigest()


def xattr_inventory(path):
    xattr = '/usr/bin/xattr'
    nofollow = ['-s'] if path.is_symlink() else []
    listed = subprocess.run([xattr, *nofollow, str(path)], capture_output=True)
    if listed.returncode:
        raise SystemExit(f'Cannot list extended attributes for {path}: {listed.stderr.decode(errors="replace")}')
    names = listed.stdout.decode().splitlines()
    result = []
    for name in names:
        value = subprocess.run([xattr, '-p', *nofollow, name, str(path)], capture_output=True)
        if value.returncode:
            raise SystemExit(f'Cannot read extended attribute {name} from {path}: {value.stderr.decode(errors="replace")}')
        result.append((name, hashlib.sha256(value.stdout).hexdigest()))
    return sorted(result)


def inventory(root):
    result = {}
    for current, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = sorted(name for name in dirs if name != 'staged_system_apps')
        for name in sorted(dirs + files):
            path = Path(current) / name
            relative = str(path.relative_to(root))
            item = path.lstat()
            record = {'mode': stat.S_IMODE(item.st_mode), 'uid': item.st_uid, 'gid': item.st_gid,
                      'xattrs': xattr_inventory(path)}
            if stat.S_ISLNK(item.st_mode):
                record.update(kind='symlink', target=os.readlink(path))
            elif stat.S_ISDIR(item.st_mode):
                record['kind'] = 'directory'
            elif stat.S_ISREG(item.st_mode):
                record.update(kind='file', bytes=item.st_size, sha256=sha256(path))
            else:
                record['kind'] = 'special'
            result[relative] = record
    return result


def copy_var(source, destination):
    if os.geteuid() != 0:
        raise SystemExit('Run as root to preserve the target Data seed numeric owners')
    source_items = sorted(source.iterdir(), key=lambda path: path.name)
    for item in source_items:
        if item.name == 'staged_system_apps':
            continue
        target = destination / item.name
        subprocess.run(['ditto', '--rsrc', '--extattr', '--acl', str(item), str(target)], check=True)
    expected, actual = inventory(source), inventory(destination)
    if any(actual.get(key) != value for key, value in expected.items()):
        different = sorted(key for key, value in expected.items() if actual.get(key) != value)
        raise SystemExit('Data seed copy changed source metadata/content: ' + ', '.join(different[:12]))
    return expected, sorted(actual.keys() - expected.keys())


def create_directory(path, uid, gid, mode):
    path.mkdir(parents=True, exist_ok=True)
    os.chown(path, uid, gid)
    os.chmod(path, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--os-root', type=Path, default=ROOT / '.cache/firmware/target-volumes/os-mount')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/owner-data-fixture')
    args = parser.parse_args()
    os_root = args.os_root.resolve()
    fixture = args.output.resolve()
    write_image = fixture / 'owner-data-32.dmg'
    image = fixture / 'owner-data-32.raw'
    source = os_root / 'private/var'
    launchd, dyld = os_root / 'sbin/launchd', os_root / 'usr/lib/dyld'
    if not launchd.is_file() or sha256(launchd) != LAUNCHD_SHA256:
        raise SystemExit('Require the exact iPad7,1 / iPadOS 17.7.11 / 21H461 launchd')
    if not dyld.is_file() or sha256(dyld) != DYLD_SHA256:
        raise SystemExit('Require the exact iPad7,1 / iPadOS 17.7.11 / 21H461 dyld')
    if not source.is_dir() or source.is_symlink():
        raise SystemExit('Require the mounted target OS private/var directory')
    if os.geteuid() != 0:
        raise SystemExit('Run as root on macOS to preserve Data seed UID/GID and metadata')
    if image.exists() or write_image.exists() or (fixture / 'seed.json').exists():
        raise SystemExit('Refuse to overwrite an existing Data seed')

    fixture.mkdir(parents=True, exist_ok=True)
    subprocess.run(['hdiutil', 'create', '-size', '32m', '-layout', 'NONE', '-fs', 'APFS',
                    '-volname', 'OwnerData', '-type', 'UDIF', str(write_image)], check=True)
    target = fixture / 'mount'
    target.mkdir(exist_ok=True)
    attached = plistlib.loads(subprocess.check_output(
        ['hdiutil', 'attach', '-nobrowse', '-owners', 'on', '-mountpoint', str(target), '-plist', str(write_image)]))
    volumes = [entry for entry in attached.get('system-entities', [])
               if entry.get('mount-point') == str(target) and entry.get('dev-entry')]
    if len(volumes) != 1:
        raise SystemExit('Require exactly one newly formatted APFS Data volume')
    device = volumes[0]['dev-entry']
    try:
        disk = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', device]))
        if not disk.get('VolumeUUID') or disk.get('Writable') is not True:
            raise SystemExit('New Data seed volume is not a writable APFS image')
        source_items, host_created_entries = copy_var(source, target)
        create_directory(target / 'keybags', 0, 0, 0o755)
        create_directory(target / 'mobile/tmp', 501, 501, 0o700)
        create_directory(target / 'tmp', 0, 0, 0o1777)
        create_directory(target / 'run', 0, 1, 0o755)
        volume_uuid = disk['VolumeUUID']
    finally:
        subprocess.run(['hdiutil', 'detach', device], check=True)

    raw_attached = plistlib.loads(subprocess.check_output(
        ['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', '-plist', str(write_image)]))
    whole_device = select_whole_disk(raw_attached.get('system-entities', []))
    try:
        listing = plistlib.loads(subprocess.check_output(
            ['diskutil', 'list', '-plist', Path(whole_device).name]))
        raw_device = select_apfs_partition(whole_device, listing)
        raw_info = plistlib.loads(subprocess.check_output(['diskutil', 'info', '-plist', raw_device]))
        if (raw_info.get('BusProtocol') != 'Disk Image' or raw_info.get('Writable') is not False or
                raw_info.get('DeviceNode') != raw_device):
            raise SystemExit('Raw Data seed mapping is not an exact read-only disk-image partition')
        data_bytes = raw_info.get('TotalSize')
        if (isinstance(data_bytes, bool) or not isinstance(data_bytes, int) or data_bytes <= 0 or
                data_bytes > IMAGE_BYTES or data_bytes % 4096):
            raise SystemExit(f'APFS Data partition size is outside the aligned 32 MiB image bound: {data_bytes}')
        raw_device_path = raw_device.replace('/dev/disk', '/dev/rdisk', 1)
        copied = 0
        with open(raw_device_path, 'rb', buffering=0) as source_image, image.open('xb') as destination:
            while copied < data_bytes:
                data = source_image.read(min(8 * MIB, data_bytes - copied))
                if not data or destination.write(data) != len(data):
                    raise SystemExit(f'Short read while exporting raw APFS Data image at byte {copied}')
                copied += len(data)
    finally:
        subprocess.run(['hdiutil', 'detach', whole_device], check=True)
    write_image.unlink()

    with image.open('rb') as source_image:
        superblock = source_image.read(4096)
        if image.stat().st_size != data_bytes or superblock[32:36] != b'NXSB':
            raise SystemExit('Host formatting did not produce the requested raw APFS container')
        block_size = int.from_bytes(superblock[36:40], 'little')
        block_count = int.from_bytes(superblock[40:48], 'little')
        if block_size != 4096 or not 0 < block_count <= data_bytes // block_size:
            raise SystemExit('Raw Data seed does not contain a bounded 4 KiB APFS container')
    regular = [entry for entry in source_items.values() if entry['kind'] == 'file']
    report = {
        'target': 'iPad7,1 / iPadOS 17.7.11 / 21H461',
        'os_root_launchd_sha256': LAUNCHD_SHA256,
        'os_root_dyld_sha256': DYLD_SHA256,
        'volume_uuid': volume_uuid,
        'whole_disk_device': Path(whole_device).name,
        'apfs_partition_device': Path(raw_device).name,
        'bytes': image.stat().st_size,
        'sha256': sha256(image),
        'apfs_block_size': block_size,
        'apfs_block_count': block_count,
        'virtual_disk_image_bytes': IMAGE_BYTES,
        'source_regular_files': len(regular),
        'source_regular_bytes': sum(entry['bytes'] for entry in regular),
        'source_entries': len(source_items),
        'host_created_entries': host_created_entries,
        'staged_system_apps_excluded': True,
        'numeric_owner_mode_xattr_copy_verified': True,
        'acl_copy_requested': True,
        'acl_content_verified': False,
        'metadata_copy_tool': 'ditto --rsrc --extattr --acl',
        'guest_mutable': True,
        'initial_seed_signature_verified': False,
        'owner_keybag_created': False,
        'production_storage_qualified': False,
        'seed': 'Exact target private/var excluding staged_system_apps; runtime directories added',
    }
    (fixture / 'seed.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
