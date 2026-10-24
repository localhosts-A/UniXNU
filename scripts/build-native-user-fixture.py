#!/usr/bin/env python3
"""Build an ordinary ARM64 static Darwin process for native-4K EL0/SVC tests."""
import json
import hashlib
from pathlib import Path
import re
import subprocess
import struct
import uuid
import plistlib
import zipfile
import argparse

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=ROOT / 'build/native-user-fixture')
parser.add_argument('--source', type=Path, help='Own static ARM64 assembly with the fixture checkpoint/result ABI')
parser.add_argument('--entitlements', type=Path, help='Own plist for explicit unsupported-entitlement negative tests')
parser.add_argument('--signing-page-size', type=int, choices=[4096, 16384], default=4096,
                    help='CodeDirectory integrity block size; hardware/VM page size stays native 4K')
parser.add_argument('--probe-file', type=Path,
                    help='Unchanged target dyld_shared_cache_arm64.02 for real file-signature/mmap tests')
parser.add_argument('--probe-cache-getuid', type=Path, help='Exact original cache file identified by audit-target-cache-leaf.py')
parser.add_argument('--apfs-device', help='Actual observed /dev/disk... volume for an offline read-only data mount')
parser.add_argument('--apfs-mount-mode', type=int, choices=(0, 1), default=1,
                    help='Original API mode: 1 ordinary data mount; 0 negative user-root request control')
parser.add_argument('--apfs-launchd-devices', nargs=2, metavar=('OS_DEVICE', 'CACHE_DEVICE'),
                    help='Actual observed volume devices for a read-only APFS/chroot/original-dyld component test')
parser.add_argument('--stop-before-apfs-exec', action='store_true', help='Qualify data mounts and chroot before original exec')
parser.add_argument('--fp-context',action='store_true',help='Bounded all-32-SIMD-register syscall and blocking thread-switch preservation instrument')
args = parser.parse_args()
if args.source and (args.fp_context or args.probe_file or args.probe_cache_getuid or
                    args.apfs_device or args.apfs_launchd_devices):
    parser.error('A custom own process source is separate from the original-file/APFS/SIMD probes')
if args.fp_context and args.signing_page_size!=4096:parser.error('FP context instrument requires its own native-4K CodeDirectory')
if args.fp_context and (args.probe_file or args.probe_cache_getuid or args.apfs_device or args.apfs_launchd_devices):parser.error('FP context instrument is separate from file/APFS probes')
if args.probe_cache_getuid and (not args.apfs_device or args.probe_file or args.apfs_mount_mode != 1):
    parser.error('The original cache-code probe requires one real ordinary APFS data mount')
if args.apfs_launchd_devices and (args.probe_file or args.probe_cache_getuid or args.apfs_device or
        any(not re.fullmatch(r'/dev/disk[0-9]+(?:s[0-9]+)+', d) for d in args.apfs_launchd_devices)):
    parser.error('Use two observed APFS device names without the separate file probe')
if args.stop_before_apfs_exec and not args.apfs_launchd_devices:
    parser.error('--stop-before-apfs-exec requires the two-volume component fixture')
if args.apfs_device and (not (args.probe_file or args.probe_cache_getuid) or not re.fullmatch(r'/dev/disk[0-9]+(?:s[0-9]+)+', args.apfs_device)):
    parser.error('The APFS data test needs the original cache probe and an actual volume device name')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
probe = None
cache_leaf = None
probe_flags = ['-DNATIVE_FP_CONTEXT_PROBE=1'] if args.fp_context else []
if args.apfs_launchd_devices:
    probe_flags += ['-DNATIVE_APFS_LAUNCHD_PROBE=1',
                    '-DNATIVE_APFS_OS_DEVICE="' + args.apfs_launchd_devices[0] + '"',
                    '-DNATIVE_APFS_CACHE_DEVICE="' + args.apfs_launchd_devices[1] + '"']
    if args.stop_before_apfs_exec:
        probe_flags += ['-DNATIVE_APFS_STOP_BEFORE_EXEC=1']
if args.apfs_device:
    probe_flags += ['-DNATIVE_APFS_FILE_PROBE=1', '-DNATIVE_APFS_DEVICE="' + args.apfs_device + '"',
                    f'-DNATIVE_APFS_MOUNT_MODE={args.apfs_mount_mode}']
if args.probe_cache_getuid:
    cache_leaf = json.loads((ROOT / 'build/cache-leaf-host/getuid.json').read_text())
    digest = hashlib.sha256()
    with args.probe_cache_getuid.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    if args.probe_cache_getuid.name != cache_leaf['file'] or digest.hexdigest() != cache_leaf['file_sha256']:
        raise SystemExit('Require the exact unchanged original cache file from the export audit')
    probe_flags += ['-DNATIVE_CACHE_LEAF_PROBE=1',
                    '-DNATIVE_CACHE_LEAF_PATH="/dev/apfs-offline/System/Library/Caches/com.apple.dyld/' + cache_leaf['file'] + '"',
                    f'-DNATIVE_CACHE_LEAF_PAGE_OFFSET={cache_leaf["native_page_offset"]}',
                    f'-DNATIVE_CACHE_LEAF_ENTRY_OFFSET={cache_leaf["within_page_offset"]}',
                    f'-DNATIVE_CACHE_LEAF_SIGNATURE_OFFSET={cache_leaf["signature_offset"]}',
                    f'-DNATIVE_CACHE_LEAF_SIGNATURE_SIZE={cache_leaf["signature_size"]}']
if args.probe_file:
    probe = args.probe_file.read_bytes()
    if hashlib.sha256(probe).hexdigest() != '3d923c404fe519184a99042b60a4cb3d37247dcb8b070242ed3e670d4d523d04':
        raise SystemExit('Expected the exact supplied 21H461 arm64.02 cache file')
    sigoff, sigsize = struct.unpack_from('<QQ', probe, 40)
    signature = probe[sigoff:sigoff + sigsize]
    count = struct.unpack_from('>I', signature, 8)[0]
    directories = []
    for i in range(count):
        _, at = struct.unpack_from('>II', signature, 12 + i * 8)
        magic, length = struct.unpack_from('>II', signature, at)
        if magic == 0xfade0c02:
            cd = signature[at:at + length]
            directories.append(cd)
    if len(directories) != 1 or directories[0][39] != 14:
        raise SystemExit('Expected the target original 16K CodeDirectory')
    cd = directories[0]
    limit = struct.unpack_from('>I', cd, 32)[0]
    hashoff = struct.unpack_from('>I', cd, 16)[0]
    if limit != 32768 or any(hashlib.sha256(probe[o:o + 16384]).digest() !=
            cd[hashoff + (o >> 14) * 32:hashoff + ((o >> 14) + 1) * 32]
            for o in range(0, limit, 16384)):
        raise SystemExit('Original complete signing block verification failed')
    probe_flags += ['-DNATIVE_FILE_PROBE=1', f'-DNATIVE_PROBE_LIMIT={limit}',
                   f'-DNATIVE_PROBE_SIGNATURE_OFFSET={sigoff}', f'-DNATIVE_PROBE_SIGNATURE_SIZE={sigsize}']
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
                *probe_flags, '-c', args.source.resolve() if args.source else ROOT / 'scripts/qemu-entry/native-user.S',
                '-o', out / 'user.o'], check=True)
subprocess.run(['xcrun', 'ld', '-arch', 'arm64', '-static', '-e', '_start',
                '-platform_version', 'ios', '17.0', '17.0', out / 'user.o',
                '-o', out / 'launchd-static.macho'], check=True)
path = out / 'launchd.macho'
subprocess.run(['xcrun', 'vtool', '-set-build-version', 'ios', '17.0', '17.0',
                '-replace', '-output', path, out / 'launchd-static.macho'], check=True)
sign_options = ['--entitlements', str(args.entitlements.resolve())] if args.entitlements else []
subprocess.run(['codesign', '--force', '--sign', '-', *sign_options, '--pagesize', str(args.signing_page_size),
                '--identifier', 'org.blu-sharky.native4k-fixture', path], check=True)
subprocess.run(['codesign', '--verify', '--strict', path], check=True)
details = subprocess.run(['codesign', '--display', '--verbose=4', path],
                         capture_output=True, text=True, check=True).stderr
match = re.search(r'^CDHash=([0-9a-f]{40})$', details, re.M)
if not match:
    raise SystemExit('Cannot identify the signed own fixture CodeDirectory')
page = re.search(r'^Page size=(\d+)$', details, re.M)
if not page or int(page[1]) != args.signing_page_size:
    raise SystemExit('CodeDirectory does not identify the requested integrity block size')
cdhash = bytes.fromhex(match[1])
identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.native4k-fixture:' + cdhash.hex())
module = struct.pack('<I16sI', 2, identifier.bytes, 1) + cdhash + bytes([2, 0, 0, 0])
bundle = struct.pack('<2I', 1, 8) + module
(out / 'trustcache.bundle').write_bytes(bundle)
(out / 'trustcache.json').write_text(json.dumps({
    'purpose': 'explicit trust for the own offline EL0 fixture only',
    'process_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
    'cdhash': cdhash.hex(), 'module_version': 2, 'entries': 1,
    'signature_hash_page_bytes': int(page[1]),
    'bundle_sha256': hashlib.sha256(bundle).hexdigest(),
    'apple_trust_chain_verified': False, 'test_instrument_only': True}, indent=2) + '\n')
symbols = subprocess.check_output(['xcrun', 'nm', '-U', path], text=True)
addresses = {name: int(address, 16) for address, name in re.findall(r'^([0-9a-fA-F]+)\s+\S\s+(_\S+)$', symbols, re.M)}
(out / 'addresses.json').write_text(json.dumps(addresses, indent=2) + '\n')
if probe is not None:
    own = path.read_bytes()
    test_offset = (4096 + len(own) + 4095) & ~4095
    image = bytearray((test_offset + len(probe) + 4095) & ~4095)
    struct.pack_into('<8sII4Q', image, 0, b'N4KROOT\0', 1, 2, 4096, len(own), test_offset, len(probe))
    image[4096:4096 + len(own)] = own
    image[test_offset:test_offset + len(probe)] = probe
    if not args.apfs_device:
        (out / 'root.img').write_bytes(image)
    file_cdhash = hashlib.sha256(cd).digest()[:20]
    test_hashes = sorted((cdhash, file_cdhash))
    test_id = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.offline-file-probe:' + file_cdhash.hex())
    test_module = struct.pack('<I16sI', 2, test_id.bytes, len(test_hashes))
    test_module += b''.join(h + bytes([2, 0, 0, 0]) for h in test_hashes)
    file_trust = struct.pack('<2I', 1, 8) + test_module
    (out / 'offline-file-trust.bundle').write_bytes(file_trust)
    (out / 'offline-file-trust.json').write_text(json.dumps({
        'purpose': 'explicit boot-owner trust for the own process and unchanged original file, offline test only',
        'cdhashes': [h.hex() for h in test_hashes], 'entries': len(test_hashes),
        'bundle_sha256': hashlib.sha256(file_trust).hexdigest(),
        'original_apple_trust_cache_modified': False,
        'apple_trust_chain_verified': False, 'test_instrument_only': True}, indent=2) + '\n')
    checksum = 0
    for at in range(0, limit, 4096):
        checksum ^= struct.unpack_from('<Q', probe, at)[0]
    (out / 'file-probe.json').write_text(json.dumps({
        'file': args.probe_file.name, 'original_file_sha256': hashlib.sha256(probe).hexdigest(),
        'original_cdhash': hashlib.sha256(cd).hexdigest()[:40],
        'code_limit': limit, 'signature_offset': sigoff, 'signature_size': sigsize,
        'hash_block_bytes': 16384, 'expected_native_pages': limit // 4096,
        'expected_page_word_xor': checksum, 'root_sha256': hashlib.sha256(own if args.apfs_device else image).hexdigest(),
        'apfs_data_mount': bool(args.apfs_device), 'actual_volume_device': args.apfs_device,
        'apfs_mount_mode': args.apfs_mount_mode if args.apfs_device else None,
        'own_process_sha256': hashlib.sha256(own).hexdigest(),
        'target_bytes_modified': False, 'test_instrument_only': True,
        'apple_trust_chain_verified': False}, indent=2) + '\n')
if args.apfs_launchd_devices or cache_leaf:
    def der_value(data, at):
        tag, count = data[at:at + 2]
        at += 2
        if count & 128:
            n = count & 127
            count = int.from_bytes(data[at:at + n], 'big')
            at += n
        if at + count > len(data):
            raise SystemExit('Trust-cache DER exceeds the original input')
        return tag, data[at:at + count], at + count

    with zipfile.ZipFile(ROOT / 'refer/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw') as archive:
        manifest = plistlib.loads(archive.read('BuildManifest.plist'))
        identities = [i for i in manifest['BuildIdentities'] if i['Info']['DeviceClass'] == 'j120ap']
        if manifest['ProductBuildVersion'] != '21H461' or len(identities) != 2:
            raise SystemExit('Expected both original iPad7,1 restore identities')
        entry = identities[0]['Manifest']['Cryptex1,SystemTrustCache']
        original_im4p = archive.read(entry['Info']['Path'])
        if any(i['Manifest']['Cryptex1,SystemTrustCache']['Digest'] != hashlib.sha384(original_im4p).digest()
               for i in identities):
            raise SystemExit('Original System trust-cache manifest digest mismatch')
    tag, sequence, end = der_value(original_im4p, 0)
    fields, cursor = [], 0
    while cursor < len(sequence):
        kind, value, cursor = der_value(sequence, cursor)
        fields.append((kind, value))
    if tag != 0x30 or end != len(original_im4p) or [t for t, _ in fields] != [0x16, 0x16, 0x16, 4] or fields[0][1] != b'IM4P':
        raise SystemExit('Expected the genuine unpersonalized trust-cache IM4P')
    system_module = fields[3][1]
    static_module = (ROOT / '.cache/firmware/target-volumes/static-trustcache.bin').read_bytes()
    if hashlib.sha256(static_module).hexdigest() != 'da88460e6e513438d5307e5c526f76497a74754476a24da29f239f0c90120fc2':
        raise SystemExit('Original static trust-cache identity mismatch')
    cache_audit = json.loads((ROOT / 'build/target-userspace/dyld-cache-audit.json').read_text())
    trusted = {system_module[i:i + 20].hex() for i in range(24, len(system_module), 24)}
    cache_hashes = {c['cdhash'] for f in cache_audit['cache_files'] for c in f['code_directories']}
    if len(cache_hashes) != 53 or cache_hashes - trusted:
        raise SystemExit('The unchanged original System module must cover all 53 audited cache files')
    modules = [module, static_module, system_module]
    for data in modules:
        if struct.unpack_from('<I', data)[0] != 2 or len(data) != 24 + struct.unpack_from('<I', data, 20)[0] * 24:
            raise SystemExit('Expected complete original version-2 trust-cache modules')
    offsets, cursor = [], 16
    for data in modules:
        offsets.append(cursor)
        cursor += len(data)
    combined = struct.pack('<4I', 3, *offsets) + b''.join(modules)
    (out / ('original-cache-trust.bundle' if cache_leaf else 'apfs-launchd-trust.bundle')).write_bytes(combined)
    (out / ('cache-leaf.json' if cache_leaf else 'apfs-launchd.json')).write_text(json.dumps(dict(
        purpose='offline original-cache-code instrument' if cache_leaf else 'offline original-dyld component instrument; global root remains the own RAM fixture',
        cache_leaf_input=cache_leaf,
        own_process_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
        os_device=args.apfs_launchd_devices[0] if args.apfs_launchd_devices else None,
        cache_device=args.apfs_launchd_devices[1] if args.apfs_launchd_devices else args.apfs_device,
        stop_before_exec=args.stop_before_apfs_exec, read_only_data_mounts=True,
        chroot_is_not_authenticated_boot_root=True,
        original_static_module_sha256=hashlib.sha256(static_module).hexdigest(),
        original_system_module_sha256=hashlib.sha256(system_module).hexdigest(),
        original_system_im4p_sha256=hashlib.sha256(original_im4p).hexdigest(),
        original_system_module_entries=struct.unpack_from('<I', system_module, 20)[0],
        trust_bundle_sha256=hashlib.sha256(combined).hexdigest(),
        original_modules_modified=False, audited_original_cache_files_covered=len(cache_hashes),
        apple_trust_chain_verified=False,
        test_instrument_only=True), indent=2) + '\n')
print('Signed static ARM64/iOS 17 fixture; real getpid and HW_PAGESIZE sysctl, no dynamic linker')
print(json.dumps(addresses, indent=2))

if args.fp_context:
    # Stage the fixed-address binary through the explicit signed Data instrument.
    (out/'air-native').write_bytes(path.read_bytes())
    original=(ROOT/'build/original-static-system-trust/original-static-system.bundle').read_bytes()
    count=struct.unpack_from('<I',original)[0];assert count==2
    offsets=struct.unpack_from('<2I',original,4)
    modules=[module,original[offsets[0]:offsets[1]],original[offsets[1]:]]
    cursor=16;starts=[]
    for item in modules:starts.append(cursor);cursor+=len(item)
    trust=struct.pack('<4I',3,*starts)+b''.join(modules)
    (out/'offline-trust.bundle').write_bytes(trust)
    manifest=dict(own_program_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                  source_sha256=hashlib.sha256((ROOT/'scripts/qemu-entry/native-fp-context.inc').read_bytes()).hexdigest(),
                  full_simd_registers=32,iterations=32,blocking_thread_switch=True,original_desktop=False)
    encoded=(json.dumps(manifest,indent=2)+'\n').encode();(out/'test-inputs.json').write_bytes(encoded)
    (out/'build-report.json').write_text(json.dumps(dict(process_sha256=manifest['own_program_sha256'],
        manifest_sha256=hashlib.sha256(encoded).hexdigest(),trust_bundle_sha256=hashlib.sha256(trust).hexdigest(),
        code_signature_page_bytes=4096,original_trust_modules_unchanged=True),indent=2)+'\n')
