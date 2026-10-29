#!/usr/bin/env python3
"""Build a source-IOStorage Virtio root driver for Dali QEMU diagnostics only.

This makes a temporary multi-policy source variant under build/. It does not
modify the pinned driver submodule, install donor IOStorage aliases, enable the
Dali UFS personality, or qualify a bare-metal Virtio provider.
"""
import hashlib
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MODE = 'XNU_NATIVE_VIRTIO_QEMU_SOURCE_STORAGE'
SINGLE_EXTERN = (
    'extern const struct vr_crypto vr_xnu_crypto;\n'
    'extern const uint8_t verified_root_manifest[], verified_root_key[];\n'
    'extern const uint64_t verified_root_manifest_bytes;\n'
)
MULTI_EXTERN = (
    'extern const struct vr_crypto vr_xnu_crypto;\n'
    'extern const uint8_t verified_root_key[];\n'
    '#if !defined(NATIVE_VERIFIED_BLOCK_MULTI_POLICY) || !NATIVE_VERIFIED_BLOCK_MULTI_POLICY\n'
    'extern const uint8_t verified_root_manifest[];\n'
    'extern const uint64_t verified_root_manifest_bytes;\n'
    '#endif\n'
)
SINGLE_SELECT = (
    '  const uint8_t *manifest = verified_root_manifest;\n'
    '  uint64_t manifestBytes = verified_root_manifest_bytes;\n'
    '#if defined(NATIVE_VERIFIED_BLOCK_MULTI_POLICY) && NATIVE_VERIFIED_BLOCK_MULTI_POLICY\n'
    '  manifest = nullptr;\n'
    '  manifestBytes = 0;\n'
)
MULTI_SELECT = (
    '#if defined(NATIVE_VERIFIED_BLOCK_MULTI_POLICY) && NATIVE_VERIFIED_BLOCK_MULTI_POLICY\n'
    '  const uint8_t *manifest = nullptr;\n'
    '  uint64_t manifestBytes = 0;\n'
)
SINGLE_SELECT_END = '  if (!manifest) return false;\n#endif\n'
MULTI_SELECT_END = (
    '  if (!manifest) return false;\n'
    '#else\n'
    '  const uint8_t *manifest = verified_root_manifest;\n'
    '  uint64_t manifestBytes = verified_root_manifest_bytes;\n'
    '#endif\n'
)
SINGLE_UFS_AUTH = 'int native_ufs_block_authenticates(IOService *, const char *, uint8_t *) __attribute__((weak));'
MULTI_UFS_AUTH = 'int native_ufs_block_authenticates(IOService *, const char *, uint8_t *) __attribute__((weak_import));'


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_variant(source):
    text = Path(source).read_text()
    if (text.count(SINGLE_EXTERN) != 1 or text.count(SINGLE_SELECT) != 1 or
            text.count(SINGLE_SELECT_END) != 1 or text.count(SINGLE_UFS_AUTH) != 1):
        raise ValueError('pinned NativeVirtioBlock multi-policy source anchors changed')
    text = text.replace(SINGLE_EXTERN, MULTI_EXTERN, 1)
    text = text.replace(SINGLE_SELECT, MULTI_SELECT, 1)
    text = text.replace(SINGLE_SELECT_END, MULTI_SELECT_END, 1)
    text = text.replace(SINGLE_UFS_AUTH, MULTI_UFS_AUTH, 1)
    if (text.count('extern const uint8_t verified_root_manifest[];') != 1 or
            text.count('extern const uint64_t verified_root_manifest_bytes;') != 1):
        raise ValueError('multi-policy source variant declaration contract changed')
    return text.encode()


def require_profile(env):
    required = {
        MODE: '1', 'XNU_NATIVE_VIRTIO_BLOCK': '1', 'XNU_NATIVE_AES': '0',
        'XNU_SOURCE_STORAGE_FAMILY': '1', 'XNU_SOURCE_CORECRYPTO14': '1',
        'XNU_CRYPTO_BOOTSTRAP': 'source', 'XNU_NATIVE_UFS': '0',
        'XNU_NATIVE_RAM_BLOCK': '0', 'XNU_VERIFIED_ROOT_PROVIDER': '1',
        'XNU_OWNER_ROOT_AUDIT': '1', 'XNU_OWNER_ROOT_TRUST': '1',
        'XNU_TRUST_CACHE_RUNTIME': '1', 'XNU_SOURCE_MOCKFS': '0',
        'XNU_SOURCE_ENTITLEMENT_FREE': '0',
    }
    mismatches = {name: (env.get(name, '0'), expected) for name, expected in required.items()
                  if env.get(name, '0') != expected}
    if mismatches:
        raise ValueError(f'invalid QEMU/source-storage Virtio profile: {mismatches}')
    if any(name in env.get('XNU_EXTRA_LD_ARCHIVES', '') for name in ('libDaliUfs.a', 'libNativeUfs.a')):
        raise ValueError('QEMU-only Virtio profile must not link the Dali physical UFS driver')


def build(root=ROOT, env=None, runner=subprocess.run, check_output=subprocess.check_output):
    root = Path(root).resolve()
    env = dict(os.environ if env is None else env)
    if env.get(MODE, '0') != '1':
        return {'skipped': True, 'reason': f'{MODE} is disabled'}
    require_profile(env)
    output = root / 'build/native-virtio-block-qemu-source-storage'
    configured_archive = Path(env.get('XNU_NATIVE_VIRTIO_BLOCK_ARCHIVE',
                                      str(output / 'libNativeVirtioBlock.a'))).resolve()
    if configured_archive != (output / 'libNativeVirtioBlock.a').resolve():
        raise ValueError('QEMU source-storage Virtio archive path differs from the isolated output')
    driver = root / 'drivers/NativeVirtioBlock'
    if not (driver / 'NativeVirtioBlock.cpp').is_file() or not (driver / 'VerifiedBlockPolicy.h').is_file():
        raise ValueError('pinned NativeVirtioBlock source or multi-policy ABI is missing')
    source_path = driver / 'NativeVirtioBlock.cpp'
    output = root / 'build/native-virtio-block-qemu-source-storage'
    output.mkdir(parents=True, exist_ok=True)
    temporary_source = output / 'NativeVirtioBlock.qemu.cpp'
    variant = source_variant(source_path)
    temporary_source.write_bytes(variant)

    helper_path = root / 'scripts/build-source-storage.py'
    namespace = {'__file__': str(helper_path), '__name__': 'dali_source_storage_helpers'}
    exec(compile(helper_path.read_bytes(), str(helper_path), 'exec'), namespace)
    objects = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    record_path, record = namespace['compiler_record'](objects)
    obj = output / 'NativeVirtioBlock.o'
    command = namespace['compile_command'](
        record, temporary_source, obj,
        [driver, root / 'build/storage-abi/include', root / 'refer/IOStorageFamily',
         root / 'kits/VerifiedRoot/include'])
    command.extend(['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1'])
    result = runner(command, cwd=Path(record['directory']).resolve(),
                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    text=True, timeout=180)
    (output / 'compile.log').write_text(result.stdout)
    (output / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
    if result.returncode:
        raise RuntimeError('QEMU multi-policy NativeVirtioBlock compile failed:\n' + result.stdout[-12000:])
    namespace['validate_object'](obj)
    undefined = check_output(['xcrun', 'nm', '-u', str(obj)], text=True)
    if any(name in undefined for name in ('_verified_root_manifest', '_verified_root_manifest_bytes')):
        raise RuntimeError('multi-policy source object imports single-policy manifest symbols')
    archive = output / 'libNativeVirtioBlock.a'
    archive.unlink(missing_ok=True)
    runner(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=60)
    symbols = check_output(['xcrun', 'nm', '-g', str(archive)], text=True)
    if not any('NativeVirtioBlock10gMetaClassE' in line and ' U ' not in line
               for line in symbols.splitlines()):
        raise RuntimeError('QEMU Virtio archive lacks its native IOService personality class')
    storage_archive = root / 'build/source-storage/libIOStorageFamily.a'
    storage_report_path = root / 'build/source-storage/build-report.json'
    if not storage_report_path.is_file() or not storage_archive.is_file():
        raise ValueError('source IOStorageFamily must be built before QEMU Virtio')
    storage_report = json.loads(storage_report_path.read_bytes())
    if (storage_report.get('source_revision') != namespace['REVISION'] or
            storage_report.get('source_archive_built') is not True or
            storage_report.get('archive_sha256') != sha256(storage_archive)):
        raise ValueError('source IOStorageFamily archive/report does not match its pin')
    report = {
        'schema': 'native-virtio-qemu-source-storage-v1',
        'driver_source_sha256': sha256(source_path),
        'driver_source_variant_sha256': hashlib.sha256(variant).hexdigest(),
        'driver_policy_header_sha256': sha256(driver / 'VerifiedBlockPolicy.h'),
        'source_storage_revision': namespace['REVISION'],
        'source_storage_archive_sha256': sha256(root / 'build/source-storage/libIOStorageFamily.a'),
        'xnu_compile_record_sha256': sha256(record_path),
        'object_sha256': sha256(obj), 'archive_sha256': sha256(archive),
        'multi_policy': True, 'source_storage_base_class': True,
        'optional_native_ufs_auth_weak_import': True,
        'original_donor_binding_map': False, 'dali_ufs_linked': False,
        'hardware_qualified': False, 'qemu_root_profile_only': True,
    }
    (output / 'build-report.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
    print('PASS: NativeVirtioBlock source-storage multi-policy archive built for QEMU-only root diagnostics')
    print('No donor ABI map or bare-metal hardware qualification is asserted')
    return report


if __name__ == '__main__':
    try:
        report = build()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from error
    if report.get('skipped'):
        print('SKIP:', report['reason'])
