#!/usr/bin/env python3
"""Compile pinned public IOHIDFamily-Embedded sources with the source XNU ABI.

Real IIG definitions produce the DriverKit bridge; no original kernelcache,
replacement class declarations or absolute-address symbol bindings are used.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
REVISION = 'b126cf696bf1c6eede740b92d3bfca7e62d09c06'
SOURCE = ROOT / 'refer/IOHIDFamily'
EVENT_SOURCE = ROOT / 'refer/IOHIDEventHeaders'
EVENT_REVISION = '2365347f5147a1e2a46de21acd08d27fe0906e85'
# Public 2031 omits its schema/generator. This explicit source port retains
# the published 1915 event layout; it does not claim the private 2031 event ABI.
EVENT_SCHEMA = 'IOHIDFamily-1915.140.3'
UNSUPPORTED_EVENTS = ('game-controller-bottom-M1-M4', 'digitizer-estimated-roll',
                      'unpublished-apple-vendor-usages', 'private-property-forwarding')
OUT = ROOT / 'build/source-hid'
ARCHIVE = OUT / 'libIOHIDFamily.a'
PHASE = '84D292320CC90C8E00698218'
IIG_NAMES = ('IOHIDInterface', 'IOHIDDevice', 'IOHIDEventService')
CLASSES = ('IOHIDDevice', 'IOHIDInterface', 'IOHIDElementPrivate',
           'IOHIDElementContainer', 'IOHIDDeviceElementContainer',
           'IOHIDEvent', 'IOHIDEventService', 'IOHIDEventDriver')
GENERATED_EVENT_HEADERS = ('IOHIDEventTypes.h', 'IOHIDEventFieldDefs.h',
                           'IOHIDEventStructDefs.h', 'IOHIDEventMacroDefs.h')
PRIVATE_SOURCE = 'IOHIDEventServiceFastPathUserClient.cpp'
PUBLIC_PATCH = ROOT / 'scripts/patches/iohidfamily-public.patch'

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def storage_helpers():
    spec = importlib.util.spec_from_file_location('hid_source_build_helpers', ROOT / 'scripts/build-source-storage.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    return helper

def embedded_sources(source=SOURCE):
    """Read the exact pinned Embedded source phase, never a broad glob of drivers."""
    project = (source / 'IOHIDFamily.xcodeproj/project.pbxproj').read_text()
    match = re.search(r'\b' + PHASE + r' /\* Sources \*/ = \{(.*?)\n\s*\};', project, re.S)
    if not match or 'isa = PBXSourcesBuildPhase;' not in match[1]:
        raise ValueError('Pinned Embedded source phase is absent')
    names = re.findall(r'/\* ([^*]+) in Sources \*/', match[1])
    if len(names) != 49 or len(set(names)) != len(names):
        raise ValueError('Pinned Embedded source phase changed')
    result = []
    for name in names:
        if name == PRIVATE_SOURCE:
            # The public import names this private target but ships no source.
            # The explicit port patch leaves unknown client types unsupported.
            continue
        if Path(name).name != name or not name.endswith(('.cpp', '.c', '.iig')):
            raise ValueError('Unexpected source in Embedded phase: ' + name)
        if name.endswith('.iig'):
            candidates = [source / 'HIDDriverKit' / name]
        else:
            candidates = list(source.rglob(name))
        found = [p for p in candidates if p.is_file()]
        if len(found) != 1:
            raise ValueError('Cannot resolve exact Embedded source: ' + name)
        result.append(found[0])
    return result

def prepare_headers(source, out, macos_sdk, event_headers=None):
    include = out / 'include'
    (include / 'IOKit').mkdir(parents=True, exist_ok=True)
    mappings = {'hid': source / 'IOHIDFamily', 'hidevent': source / 'IOHIDFamily',
                'hidsystem': source / 'IOHIDSystem/IOKit/hidsystem'}
    # Copy into the owned build directory: generated additions never dirty a feed.
    for name, directory in mappings.items():
        destination = include / 'IOKit' / name
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(directory, destination, ignore=shutil.ignore_patterns('*.cpp', '*.c'))
    framework = include / 'HIDDriverKit'
    framework.mkdir(exist_ok=True)
    types = source / 'IOHIDFamily/IOHIDDeviceTypes.h'
    shutil.copyfile(types, framework / types.name)
    imported = []
    # 2031 omits these generated declarations and the public SDK omits them too.
    # Import the real, separately pinned 1915 declarations used by the explicit
    # source port. Compile/link/runtime remain separate validation gates.
    event_headers = event_headers or EVENT_SOURCE / 'IOHIDFamily'
    for name in GENERATED_EVENT_HEADERS:
        destination = include / 'IOKit/hid' / name
        if not destination.is_file():
            original = event_headers / name
            if not original.is_file():
                raise ValueError('Missing pinned generated HID declaration: ' + str(original))
            shutil.copyfile(original, destination)
            imported.append({'source': str(original), 'sha256': digest(original)})
        shutil.copyfile(destination, include / 'IOKit/hidevent' / name)
    conditions = macos_sdk / 'usr/include/TargetConditionals.h'
    shutil.copyfile(conditions, include / conditions.name)
    imported.append({'source': str(conditions), 'sha256': digest(conditions)})
    return include, imported

def verify_event_headers(source):
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != EVENT_REVISION or subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True).strip():
        raise ValueError('Generated HID declarations must use their clean fixed source pin')
    return revision

def c_record(objects):
    preferred = objects / 'osfmk/DEVELOPMENT/startup.o.json'
    for path in [preferred, *sorted(objects.rglob('*.o.json'))]:
        if not path.is_file():
            continue
        record = json.loads(path.read_text().strip().rstrip(','))
        if str(record.get('file', '')).endswith('.c') and isinstance(record.get('arguments'), list):
            return path, record
    raise ValueError('XNU C compile record is required')

def audit_inputs(source):
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION or subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True).strip():
        raise ValueError('HID input audit requires the clean fixed source revision')
    inputs = embedded_sources(source)
    header_source = source.parent / 'IOHIDEventHeaders'
    header_revision = verify_event_headers(header_source)
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    _, imported = prepare_headers(source, OUT / 'input-audit', sdk, header_source / 'IOHIDFamily')
    OUT.mkdir(parents=True, exist_ok=True)
    report = {'source_revision': revision, 'event_header_revision': header_revision,
              'event_schema': EVENT_SCHEMA, 'unsupported_event_features': list(UNSUPPORTED_EVENTS),
              'public_port_patch_sha256': digest(PUBLIC_PATCH), 'declaration_inputs': imported,
              'source_units': [{'path': str(p.relative_to(source)), 'sha256': digest(p)} for p in inputs],
              'unavailable_private_source': PRIVATE_SOURCE, 'source_archive_built': False,
              'hardware_qualified': False}
    (OUT / 'input-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: pinned HID public source/IIG/declaration inputs present; compatibility is not yet compiled')

def build():
    revision = subprocess.check_output(['git', '-C', SOURCE, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION or subprocess.check_output(['git', '-C', SOURCE, 'status', '--porcelain'], text=True).strip():
        raise ValueError('IOHIDFamily must be a clean checkout of the fixed revision')
    inputs = embedded_sources()
    header_revision = verify_event_headers(EVENT_SOURCE)
    helper = storage_helpers()
    objects = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    cpp_path, cpp = helper.compiler_record(objects)
    c_path, c = c_record(objects)
    OUT.mkdir(parents=True, exist_ok=True)
    ARCHIVE.unlink(missing_ok=True)
    (OUT / 'build-report.json').unlink(missing_ok=True)
    (OUT / 'compile-failures.json').unlink(missing_ok=True)
    source_tree = OUT / 'source'
    if source_tree.exists():
        shutil.rmtree(source_tree)
    shutil.copytree(SOURCE, source_tree, ignore=shutil.ignore_patterns('.git'))
    subprocess.run(['git', 'init', '--quiet', str(source_tree)], check=True)
    subprocess.run(['git', 'apply', '--check', str(PUBLIC_PATCH)], cwd=source_tree, check=True)
    subprocess.run(['git', 'apply', str(PUBLIC_PATCH)], cwd=source_tree, check=True)
    inputs = embedded_sources(source_tree)
    macos_sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    include, sdk_inputs = prepare_headers(source_tree, OUT, macos_sdk)
    xnu = ROOT / '.cache/xnu-work'
    includes = [include, include / 'IOKit/hid', source_tree, source_tree / 'IOHIDFamily', source_tree / 'IOHIDSystem',
                source_tree / 'IOHIDSystem/IOKit/hidsystem', source_tree / 'IOHIDSystem/IOHIDDescriptorParser',
                xnu / 'iokit', xnu / 'osfmk', xnu / 'bsd', ROOT / 'build/storage-abi/include',
                macos_sdk / 'System/Library/Frameworks/Kernel.framework/Headers']
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    iig = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--find', 'iig'], text=True).strip()
    generated = OUT / 'generated'
    generated.mkdir(exist_ok=True)
    generated_rows = []
    for name in IIG_NAMES:
        definition = source_tree / 'HIDDriverKit' / (name + '.iig')
        shutil.copyfile(definition, include / 'HIDDriverKit' / definition.name)
    for name in IIG_NAMES:
        definition = source_tree / 'HIDDriverKit' / (name + '.iig')
        header = include / 'HIDDriverKit' / (name + '.h')
        implementation = generated / (name + '.iig.cpp')
        header.unlink(missing_ok=True)
        implementation.unlink(missing_ok=True)
        command = [iig, '--def', str(definition), '--header', str(header),
                   '--impl', str(implementation), '--framework-name', 'HIDDriverKit', '--',
                   '-I' + str(xnu / 'iokit'), '-I' + str(xnu / 'osfmk'), '-I' + str(xnu / 'bsd'),
                   '-I' + str(objects / 'bsd'), '-I' + str(xnu / 'EXTERNAL_HEADERS'),
                   '-I' + str(include), '-I' + str(source_tree), '-isysroot', sdk,
                   '-x', 'c++', '-std=gnu++2b', '-D__IIG=1', '-DDRIVERKIT_PRIVATE=1',
                   # The restored public DriverKit/IOTypes.h selects its plain
                   # integer declarations with this frontend-only platform flag.
                   # Generated C++ still uses the actual kernel compile record.
                   '-DPRIVATE_WIFI_ONLY=1', '-DXNU_PLATFORM_DriverKit=1', *helper.include_flags(cpp)]
        # Preserve the exact frontend invocation even when generation fails.
        (OUT / (name + '.iig-command.json')).write_text(json.dumps(command, indent=2) + '\n')
        helper.run_logged(command, Path(cpp['directory']), OUT / (name + '.iig.log'))
        if not header.is_file() or not implementation.is_file():
            raise ValueError('IIG did not produce the real HID bridge: ' + name)
        generated_rows.append({'definition': str(definition.relative_to(ROOT)), 'definition_sha256': digest(definition),
                               'header_sha256': digest(header), 'implementation_sha256': digest(implementation),
                               'command': command})
    def compile_unit(source):
        record = c if source.suffix == '.c' else cpp
        obj = OUT / (source.name + '.o')
        obj.unlink(missing_ok=True)
        command = helper.compile_command(record, source, obj, includes)
        if source.suffix == '.c':
            command[3] = 'clang'
        command += ['-DAPPLE_KEXT_ASSERTIONS=1', '-DDISPATCH_MACH_SPI=1',
                    '-DOS_CRASH_ENABLE_EXPERIMENTAL_LIBTRACE=1', '-DIOHIDFAMILY_104187687_HIDE=1',
                    '-DDALI_HID_PRIVATE_APPLE_USAGES=0']
        log = obj.with_suffix('.log')
        (OUT / (source.name + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        row = {'source': str(source.relative_to(ROOT)), 'source_sha256': digest(source), 'command': command}
        try:
            helper.run_logged(command, Path(record['directory']), log)
            helper.validate_object(obj)
        except (RuntimeError, ValueError) as error:
            return obj, dict(row, error=str(error), log_sha256=digest(log) if log.exists() else None)
        return obj, dict(row, object_sha256=digest(obj), log_sha256=digest(log))

    units = [p for p in inputs if p.suffix != '.iig'] + [generated / (name + '.iig.cpp') for name in IIG_NAMES]
    # Compile every independent source so one CI pass exposes the complete set
    # of public-header gaps. Any failed unit still refuses archive creation.
    with ThreadPoolExecutor(max_workers=4) as executor:
        results = list(executor.map(compile_unit, units))
    failures = [row for _, row in results if 'error' in row]
    (OUT / 'compile-failures.json').write_text(json.dumps(failures, indent=2) + '\n')
    if failures:
        raise RuntimeError('HID source compile failures: ' + str(len(failures)) + '\n' +
                           '\n'.join(row['source'] + ': ' + row['error'][:1200] for row in failures))
    compiled = [obj for obj, _ in results]
    rows = [row for _, row in results]
    subprocess.run(['xcrun', 'ar', 'rcs', str(ARCHIVE), *map(str, compiled)], check=True)
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', ARCHIVE], text=True)
    for name in CLASSES:
        if name + '10gMetaClassE' not in defined:
            raise ValueError('Source HID archive lacks the real class: ' + name)
    report = {'source_revision': revision, 'event_header_revision': header_revision,
              'event_schema': EVENT_SCHEMA, 'unsupported_event_features': list(UNSUPPORTED_EVENTS),
              'embedded_phase': PHASE, 'objects': rows, 'iig': generated_rows,
              'public_port_patch_sha256': digest(PUBLIC_PATCH), 'unavailable_private_source': PRIVATE_SOURCE,
              'declaration_inputs': sdk_inputs, 'c_compile_record_sha256': digest(c_path),
              'cpp_compile_record_sha256': digest(cpp_path), 'archive_sha256': digest(ARCHIVE),
              'source_archive_built': True, 'kernel_link_verified': False,
              'hid_runtime_qualified': False, 'hardware_qualified': False}
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: actual public IOHIDFamily-Embedded archive built; kernel link/runtime require verification')

def verify_kernel(kernel):
    report = json.loads((OUT / 'build-report.json').read_text())
    if report.get('source_revision') != REVISION or not report.get('source_archive_built') or report.get('archive_sha256') != digest(ARCHIVE):
        raise ValueError('Source HID archive/report mismatch')
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', kernel], text=True)
    undefined = subprocess.check_output(['xcrun', 'nm', '-u', kernel], text=True)
    for name in CLASSES:
        if name + '10gMetaClassE' not in defined or name in undefined:
            raise ValueError('Final XNU kernel lacks resolved source HID class: ' + name)
    report.update(kernel_link_verified=True, kernel_sha256=digest(kernel), kernel_classes=list(CLASSES))
    (OUT / 'kernel-link-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: source IOHIDFamily classes resolve in source XNU; HID runtime/hardware remain unqualified')

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-kernel', type=Path)
    parser.add_argument('--audit-inputs', type=Path)
    args = parser.parse_args(argv)
    mode = os.environ.get('XNU_SOURCE_HID_FAMILY', '0')
    if mode == '0':
        print('SKIP: source IOHIDFamily disabled')
    elif mode != '1':
        raise SystemExit('XNU_SOURCE_HID_FAMILY must be 0 or 1')
    elif args.audit_inputs:
        audit_inputs(args.audit_inputs.resolve())
    elif args.verify_kernel:
        verify_kernel(args.verify_kernel.resolve())
    else:
        build()

if __name__ == '__main__':
    main()
