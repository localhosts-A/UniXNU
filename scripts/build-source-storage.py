#!/usr/bin/env python3
"""Build pinned public IOStorageFamily for the source-only XNU kernel.

Uses the kernel's real C++ compile record, including its SDK and allocation ABI.
The DriverKit bridge is generated with IIG and compiled from upstream sources;
no original-kernelcache symbols, replacement classes or vtables are supplied.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess


REVISION = 'e45fbe30a7879e0b16e26dddb78538f1b00e0bff'
# IOStorageFamily_Embedded Sources, project.pbxproj:934-952 at REVISION.
SOURCES = (
    'IORequest.cpp', 'IORequestsPool.cpp', 'IOUserBlockStorageDevice_kext.cpp',
    'IOApplePartitionScheme.cpp', 'IOBlockStorageDevice.cpp',
    'IOBlockStorageDriver.cpp', 'IOFDiskPartitionScheme.cpp', 'IOFilterScheme.cpp',
    'IOGUIDPartitionScheme.cpp', 'IOMedia.cpp', 'IOMediaBSDClient.cpp',
    'IOPartitionScheme.cpp', 'IOStorage.cpp',
)
CLASSES = ('IOBlockStorageDevice', 'IOBlockStorageDriver', 'IOStorage', 'IOMedia',
           'IOMediaBSDClient', 'IOGUIDPartitionScheme', 'IOUserBlockStorageDevice')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiler_record(objects):
    preferred = objects / 'iokit/DEVELOPMENT/IOService.cpo.json'
    paths = [preferred] + sorted(objects.rglob('*.cpo.json'))
    for path in paths:
        if not path.is_file():
            continue
        record = json.loads(path.read_text().strip().rstrip(','))
        if str(record.get('file', '')).endswith(('.cpp', '.cc', '.cxx')) and isinstance(record.get('arguments'), list):
            return path, record
    raise ValueError('No XNU C++ compile record; run the kernel bootstrap pass first')


def compile_command(record, source, output, includes):
    directory = Path(record['directory']).resolve()
    original = Path(record['file'])
    original = original.resolve() if original.is_absolute() else (directory / original).resolve()
    arguments = record['arguments'][1:]
    command = ['xcrun', '--sdk', 'iphoneos', 'clang++']
    replaced = 0
    at = 0
    # Do not overwrite the original object's output, dependency or compile record.
    paired = {'-o', '-MF', '-MT', '-MQ', '-MJ', '-serialize-diagnostics'}
    single = {'-MD', '-MMD', '-MP', '-MG'}
    while at < len(arguments):
        value = arguments[at]
        if value in paired:
            if at + 1 >= len(arguments):
                raise ValueError('Truncated compile-record output option: ' + value)
            at += 2
            continue
        if value in single:
            at += 1
            continue
        candidate = Path(value)
        candidate = candidate.resolve() if candidate.is_absolute() else (directory / candidate).resolve()
        if candidate == original:
            command.append(str(source.resolve()))
            replaced += 1
        else:
            command.append(value)
        at += 1
    if replaced != 1:
        raise ValueError('Expected exactly one source argument in XNU compile record')
    command += ['-o', str(output.resolve())]
    for path in includes:
        command += ['-I', str(path.resolve())]
    if source.name == 'IOMediaBSDClient.cpp':
        # Pinned upstream lines 2908-2909 use the obsolete register keyword.
        # Accept that inert spelling without changing XNU's C++ dialect/ABI.
        command.append('-Wno-register')
    return command


def include_flags(record):
    arguments, result, at = record['arguments'][1:], [], 0
    paired = {'-I', '-F', '-isystem', '-iquote', '-idirafter', '-iframework'}
    while at < len(arguments):
        value = arguments[at]
        if value in paired:
            if at + 1 >= len(arguments):
                raise ValueError('Truncated compile-record include option')
            result += arguments[at:at + 2]
            at += 2
            continue
        if value.startswith(('-I', '-F')):
            result.append(value)
        at += 1
    return result


def validate_object(path):
    data = path.read_bytes()
    if len(data) < 32:
        raise ValueError('Truncated storage object: ' + str(path))
    magic, cpu, subtype, kind, count, length, _, _ = struct.unpack_from('<8I', data)
    if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1) or 32 + length > len(data):
        raise ValueError('Storage object is not XNU-compatible arm64 MH_OBJECT: ' + str(path))
    at, platform = 32, None
    for _ in range(count):
        if at + 8 > 32 + length:
            raise ValueError('Truncated object load command')
        command, size = struct.unpack_from('<II', data, at)
        if size < 8 or at + size > 32 + length:
            raise ValueError('Invalid object load command')
        if command == 0x32:
            if size < 24:
                raise ValueError('Truncated build-version command')
            platform, minimum = struct.unpack_from('<II', data, at + 8)
            if (platform, minimum) != (2, 0x110000):
                raise ValueError('Storage object does not target the kernel iOS17 ABI')
        at += size
    if at != 32 + length or platform != 2:
        raise ValueError('Storage object lacks the required iOS build version')


def run_logged(command, directory, log):
    result = subprocess.run(command, cwd=directory, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        lines = result.stdout.splitlines()
        errors = []
        for at, line in enumerate(lines):
            if 'error:' in line or 'fatal error' in line:
                errors += lines[max(0, at - 1):at + 5]
        raise RuntimeError(f'{log.name}: command failed ({result.returncode})\n' +
                           '\n'.join(errors[:80]) + '\n' + result.stdout[-6000:])


def build(root):
    source = root / 'refer/IOStorageFamily'
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION:
        raise ValueError('Public IOStorageFamily revision differs from the fixed source pin')
    if subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True).strip():
        raise ValueError('Public IOStorageFamily source must be clean')
    objects = root / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    record_path, record = compiler_record(objects)
    directory = Path(record['directory']).resolve()
    out = root / 'build/source-storage'
    generated = out / 'generated'
    generated.mkdir(parents=True, exist_ok=True)
    archive = out / 'libIOStorageFamily.a'
    archive.unlink(missing_ok=True)
    (out / 'build-report.json').unlink(missing_ok=True)
    xnu = root / '.cache/xnu-work'
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    iig = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--find', 'iig'], text=True).strip()
    definition = source / 'BlockStorageDeviceDriverKit/IOUserBlockStorageDevice.iig'
    header = generated / 'IOUserBlockStorageDevice.h'
    implementation = generated / 'IOUserBlockStorageDevice.iig.cpp'
    # Same public IIG generation route as XNU iokit/DriverKit/Makefile:6-7,62.
    # IIG owns its frontend architecture; a clang -target conflicts with it.
    # The generated implementation is compiled with the actual kernel CPO flags.
    iig_command = [iig, '--def', str(definition), '--header', str(header),
                   '--impl', str(implementation), '--framework-name', 'BlockStorageDeviceDriverKit', '--',
                   '-I' + str(xnu / 'iokit'), '-I' + str(xnu / 'osfmk'),
                   '-I' + str(xnu / 'bsd'), '-I' + str(objects / 'bsd'),
                   '-I' + str(xnu / 'EXTERNAL_HEADERS'),
                   '-isysroot', sdk, '-x', 'c++', '-std=gnu++2b',
                   '-D__IIG=1',
                   '-DDRIVERKIT_PRIVATE=1', '-DPRIVATE_WIFI_ONLY=1', *include_flags(record)]
    run_logged(iig_command, directory, out / 'iig.log')
    if not header.is_file() or not implementation.is_file():
        raise ValueError('IIG did not generate both the real interface and implementation')
    includes = [generated, source, root / 'build/storage-abi/include', xnu / 'iokit']
    inputs = [source / name for name in SOURCES] + [implementation]
    compiled, reports = [], []
    for path in inputs:
        obj = out / (path.name + '.o')
        obj.unlink(missing_ok=True)
        run_logged(compile_command(record, path, obj, includes), directory, obj.with_suffix('.log'))
        validate_object(obj)
        compiled.append(obj)
        reports.append({'source': str(path.relative_to(root)), 'source_sha256': digest(path),
                        'object': str(obj.relative_to(root)), 'object_sha256': digest(obj)})
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, compiled)], check=True)
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', str(archive)], text=True)
    (out / 'defined-symbols.txt').write_text(defined)
    for name in CLASSES:
        if name + '10gMetaClassE' not in defined:
            raise ValueError('Source archive lacks the defined class: ' + name)
    report = {'source_revision': revision, 'compile_record': str(record_path.relative_to(root)),
              'compile_record_sha256': digest(record_path), 'definition_sha256': digest(definition),
              'generated_header_sha256': digest(header), 'generated_implementation_sha256': digest(implementation),
              'iig_command': iig_command, 'objects': reports, 'archive_sha256': digest(archive),
              'source_archive_built': True, 'kernel_link_verified': False,
              'storage_runtime_qualified': False, 'physical_ufs_qualified': False}
    (out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: actual public IOStorageFamily source archive built; kernel link/runtime still require validation')


def verify_kernel(root, kernel):
    out = root / 'build/source-storage'
    report = json.loads((out / 'build-report.json').read_text())
    if (report.get('source_revision') != REVISION or report.get('source_archive_built') is not True or
            report.get('archive_sha256') != digest(out / 'libIOStorageFamily.a')):
        raise ValueError('Source storage archive/report mismatch')
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', str(kernel)], text=True)
    undefined = subprocess.check_output(['xcrun', 'nm', '-u', str(kernel)], text=True)
    for name in CLASSES:
        if name + '10gMetaClassE' not in defined or name in undefined:
            raise ValueError('Final source kernel lacks a resolved storage class: ' + name)
    data = kernel.read_bytes()
    for name in ('IOBlockStorageDriver', 'IOGUIDPartitionScheme', 'IOMediaBSDClient'):
        if ("'IOClass'         = " + name + ';').encode() not in data:
            raise ValueError('Final kernel lacks builtin storage personality: ' + name)
    result = dict(report, kernel_link_verified=True, kernel_sha256=digest(kernel),
                  storage_runtime_qualified=False, physical_ufs_qualified=False)
    (out / 'kernel-link-report.json').write_text(json.dumps(result, indent=2) + '\n')
    (out / 'kernel-defined-symbols.txt').write_text(defined)
    print('PASS: actual IOStorageFamily classes/personality linked in source XNU; runtime/UFS unqualified')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-kernel', type=Path)
    args = parser.parse_args()
    mode = os.environ.get('XNU_SOURCE_STORAGE_FAMILY', '0')
    if mode == '0':
        print('SKIP: source IOStorageFamily disabled')
    elif mode != '1':
        raise SystemExit('XNU_SOURCE_STORAGE_FAMILY must be 0 or 1')
    else:
        root = Path(__file__).resolve().parents[1]
        verify_kernel(root, args.verify_kernel) if args.verify_kernel else build(root)
