#!/usr/bin/env python3
"""Compile pinned public IOHIDFamily sources with the source XNU ABI.

Real IIG definitions produce the DriverKit bridge; no original kernelcache,
replacement class declarations or absolute-address symbol bindings are used.
The Embedded source phase is extended only by the real system-event support
units required by keyboard, pointing, and IOHIDSystem event delivery.
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
APPLE_USAGES_SOURCE = ROOT / 'refer/IOHIDAppleUsageTables'
APPLE_USAGES_REVISION = '12d918c6280fcd5e8312e4fb47b4e0e9c48a0fac'
# Public 2031 omits its schema/generator. This explicit source port retains
# the published 1915 event layout; it does not claim the private 2031 event ABI.
EVENT_SCHEMA = 'IOHIDFamily-1915.140.3'
UNSUPPORTED_EVENTS = ('game-controller-bottom-M1-M4', 'digitizer-estimated-roll',
                      'unpublished-apple-vendor-usages', 'private-property-forwarding')
UNSUPPORTED_HID_FEATURES = (
    'interactive-tcc-authorization',
    'private-no-dropped-report-and-task-suspend-options',
    'raw-userclient-report-queue-can-drop-input-edges-under-backpressure',
    'unlisted-userclient-property-writes',
    'private-game-controller-property',
    'private-interface-rematch',
    'private-property-change-notification-list',
    'blessed-apple-vendor-keyboard-usages',
)
OUT = ROOT / 'build/source-hid'
ARCHIVE = OUT / 'libIOHIDFamily.a'
PHASE = '84D292320CC90C8E00698218'
SYSTEM_SUPPORT_PHASE = '848E563B0CC55C7800D5BE22'
IIG_NAMES = ('IOHIDInterface', 'IOHIDDevice', 'IOHIDEventService')
CLASSES = ('IOHIDDevice', 'IOHIDInterface', 'IOHIDElementPrivate',
           'IOHIDElementContainer', 'IOHIDDeviceElementContainer',
           'IOHIDEvent', 'IOHIDEventService', 'IOHIDEventDriver',
           'IOHIDKeyboard', 'IOHIDConsumer', 'IOHIDPointing', 'IOHIDSystem',
           'IOHIKeyboard', 'IOHIPointing')
# These real units belong to the pinned IOHIDFamily target's Sources phase.
# The Embedded variant omits the system keyboard/pointer shims even though its
# event/device units still reference them. Keep this list explicit so new files
# cannot silently enter the source-only archive through a recursive glob.
SYSTEM_SUPPORT_SOURCES = (
    'IOHIDFamily/IOHIDConsumer.cpp',
    'IOHIDFamily/IOHIDKeyboard.cpp',
    'IOHIDFamily/IOHIDPointing.cpp',
    'IOHIDFamily/Cosmo_USB2ADB.c',
    'IOHIDSystem/IOFixed64.cpp',
    'IOHIDSystem/IOFixedPoint64.cpp',
    'IOHIDSystem/IOHIDDeviceShim.cpp',
    'IOHIDFamily/IOHIDEventServiceQueue.cpp',
    'IOHIDSystem/IOHIDKeyboardDevice.cpp',
    'IOHIDSystem/IOHIDKeyboardEventDevice.cpp',
    'IOHIDSystem/IOHIDPointingDevice.cpp',
    'IOHIDSystem/IOHIDPointingEventDevice.cpp',
    'IOHIDSystem/IOHIDPrivate.cpp',
    'IOHIDSystem/IOHIDSystem.cpp',
    'IOHIDSystem/IOHIDSystemCursorHelper.cpp',
    'IOHIDSystem/IOHIDUserClient.cpp',
    'IOHIDSystem/IOHIDWorkLoop.cpp',
    'IOHIDSystem/IOHIDevice.cpp',
    'IOHIDSystem/IOHIKeyboard.cpp',
    'IOHIDSystem/IOHIKeyboardMapper.cpp',
    'IOHIDSystem/IOHIPointing.cpp',
)
GENERATED_EVENT_HEADERS = ('IOHIDEventTypes.h', 'IOHIDEventFieldDefs.h',
                           'IOHIDEventStructDefs.h', 'IOHIDEventMacroDefs.h')
PRIVATE_SOURCE = 'IOHIDEventServiceFastPathUserClient.cpp'
PUBLIC_PATCH = ROOT / 'scripts/patches/iohidfamily-public.patch'
PRIVATE_COMPAT_PATCH = ROOT / 'scripts/patches/iohidfamily-private-compat.patch'
PERSONALITY_PATCH = ROOT / 'patches/xnu/source-hid-personalities.patch'

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

def system_support_sources(source=SOURCE):
    """Resolve a reviewed support closure from the exact pinned family target."""
    project = (source / 'IOHIDFamily.xcodeproj/project.pbxproj').read_text()
    match = re.search(r'\b' + SYSTEM_SUPPORT_PHASE + r' /\* Sources \*/ = \{(.*?)\n\s*\};', project, re.S)
    if not match or 'isa = PBXSourcesBuildPhase;' not in match[1]:
        raise ValueError('Pinned IOHIDFamily support source phase is absent')
    names = re.findall(r'/\* ([^*]+) in Sources \*/', match[1])
    if len(names) != len(set(names)):
        raise ValueError('Pinned IOHIDFamily support source phase has duplicate entries')
    missing = sorted({Path(name).name for name in SYSTEM_SUPPORT_SOURCES} - set(names))
    if missing:
        raise ValueError('Pinned IOHIDFamily target no longer declares required support units: ' + ', '.join(missing))
    result = []
    for relative in SYSTEM_SUPPORT_SOURCES:
        path = source / relative
        if not path.is_file():
            raise ValueError('Pinned IOHIDFamily support source is missing: ' + relative)
        result.append(path)
    return result

def prepare_headers(source, out, macos_sdk, event_headers=None, apple_usages=None):
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
    # Only IOHIDSystem's NXEvent wrapper imports this official published table,
    # inside a private C++ namespace. Do not enable unverified vendor parsing.
    apple_usages = apple_usages or APPLE_USAGES_SOURCE / 'IOHIDFamily/AppleHIDUsageTables.h'
    if not apple_usages.is_file():
        raise ValueError('Missing pinned published Apple usage declaration: ' + str(apple_usages))
    shutil.copyfile(apple_usages, include / 'DaliPublishedAppleUsages.h')
    imported.append({'source': str(apple_usages), 'sha256': digest(apple_usages),
                     'scope': 'IOHIDSystem NXEvent wrapper only'})
    return include, imported

def verify_event_headers(source):
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != EVENT_REVISION or subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True).strip():
        raise ValueError('Generated HID declarations must use their clean fixed source pin')
    return revision


def verify_apple_usages(source):
    revision = subprocess.check_output(['git', '-C', source, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != APPLE_USAGES_REVISION or subprocess.check_output(['git', '-C', source, 'status', '--porcelain'], text=True).strip():
        raise ValueError('Published Apple usage table must use its clean fixed source pin')
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
    inputs = embedded_sources(source) + system_support_sources(source)
    header_source = source.parent / 'IOHIDEventHeaders'
    header_revision = verify_event_headers(header_source)
    usages_source = source.parent / 'IOHIDAppleUsageTables'
    usages_revision = verify_apple_usages(usages_source)
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip())
    _, imported = prepare_headers(source, OUT / 'input-audit', sdk, header_source / 'IOHIDFamily',
                                  usages_source / 'IOHIDFamily/AppleHIDUsageTables.h')
    OUT.mkdir(parents=True, exist_ok=True)
    report = {'source_revision': revision, 'event_header_revision': header_revision,
              'apple_usage_revision': usages_revision,
              'event_schema': EVENT_SCHEMA, 'unsupported_event_features': list(UNSUPPORTED_EVENTS),
              'unsupported_hid_features': list(UNSUPPORTED_HID_FEATURES),
              'public_port_patch_sha256': digest(PUBLIC_PATCH),
              'private_compatibility_patch_sha256': digest(PRIVATE_COMPAT_PATCH),
              'kernel_personality_patch_sha256': digest(PERSONALITY_PATCH),
              'protected_device_access_policy': 'Dali-owned access property conservatively gates raw keyboard/keypad/mouse/touchpad IOHIDLibUserClient admission; kernel IOHIDEventDriver delivery remains available; no TCC consent UI is provided',
              'declaration_inputs': imported,
              'support_source_phase': SYSTEM_SUPPORT_PHASE,
              'support_source_files': list(SYSTEM_SUPPORT_SOURCES),
              'source_units': [{'path': str(p.relative_to(source)), 'sha256': digest(p)} for p in inputs],
              'unavailable_private_source': PRIVATE_SOURCE, 'source_archive_built': False,
              'hardware_qualified': False}
    (OUT / 'input-audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: pinned HID public source/IIG/declaration inputs present; compatibility is not yet compiled')

def build():
    revision = subprocess.check_output(['git', '-C', SOURCE, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION or subprocess.check_output(['git', '-C', SOURCE, 'status', '--porcelain'], text=True).strip():
        raise ValueError('IOHIDFamily must be a clean checkout of the fixed revision')
    inputs = embedded_sources() + system_support_sources()
    header_revision = verify_event_headers(EVENT_SOURCE)
    usages_revision = verify_apple_usages(APPLE_USAGES_SOURCE)
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
    for patch in (PUBLIC_PATCH, PRIVATE_COMPAT_PATCH):
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=source_tree, check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=source_tree, check=True)
    inputs = embedded_sources(source_tree) + system_support_sources(source_tree)
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
              'apple_usage_revision': usages_revision,
              'event_schema': EVENT_SCHEMA, 'unsupported_event_features': list(UNSUPPORTED_EVENTS),
              'unsupported_hid_features': list(UNSUPPORTED_HID_FEATURES),
              'embedded_phase': PHASE, 'support_source_phase': SYSTEM_SUPPORT_PHASE,
              'support_source_files': list(SYSTEM_SUPPORT_SOURCES),
              'objects': rows, 'iig': generated_rows,
              'public_port_patch_sha256': digest(PUBLIC_PATCH),
              'private_compatibility_patch_sha256': digest(PRIVATE_COMPAT_PATCH),
              'kernel_personality_patch_sha256': digest(PERSONALITY_PATCH),
              'protected_device_access_policy': 'Dali-owned access property conservatively gates raw keyboard/keypad/mouse/touchpad IOHIDLibUserClient admission; kernel IOHIDEventDriver delivery remains available; no TCC consent UI is provided',
              'unavailable_private_source': PRIVATE_SOURCE,
              'declaration_inputs': sdk_inputs, 'c_compile_record_sha256': digest(c_path),
              'cpp_compile_record_sha256': digest(cpp_path), 'archive_sha256': digest(ARCHIVE),
              'source_archive_built': True, 'kernel_link_verified': False,
              'hid_runtime_qualified': False, 'hardware_qualified': False}
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: actual public IOHIDFamily Embedded and system-support archive built; kernel link/runtime require verification')

def verify_kernel(kernel):
    report = json.loads((OUT / 'build-report.json').read_text())
    if report.get('source_revision') != REVISION or not report.get('source_archive_built') or report.get('archive_sha256') != digest(ARCHIVE):
        raise ValueError('Source HID archive/report mismatch')
    if report.get('apple_usage_revision') != APPLE_USAGES_REVISION:
        raise ValueError('Source HID published usage revision mismatch')
    if report.get('private_compatibility_patch_sha256') != digest(PRIVATE_COMPAT_PATCH):
        raise ValueError('Source HID compatibility patch/report mismatch')
    if report.get('kernel_personality_patch_sha256') != digest(PERSONALITY_PATCH):
        raise ValueError('Source HID personality patch/report mismatch')
    if report.get('support_source_phase') != SYSTEM_SUPPORT_PHASE or report.get('support_source_files') != list(SYSTEM_SUPPORT_SOURCES):
        raise ValueError('Source HID support source manifest mismatch')
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', kernel], text=True)
    undefined = subprocess.check_output(['xcrun', 'nm', '-u', kernel], text=True)
    for name in CLASSES:
        if name + '10gMetaClassE' not in defined or name in undefined:
            raise ValueError('Final XNU kernel lacks resolved source HID class: ' + name)
    data = kernel.read_bytes()
    personalities = {'keyboard': 6, 'keypad': 7, 'mouse': 2}
    if b"'IOClass'         = IOHIDEventDriver;" not in data or b"'IOProviderClass' = IOHIDInterface;" not in data:
        raise ValueError('Final XNU kernel lacks the built-in IOHIDEventDriver interface personality')
    for name, usage in personalities.items():
        marker = ("'DeviceUsage' = " + str(usage) + ':32;').encode()
        if marker not in data:
            raise ValueError('Final XNU kernel lacks the source HID ' + name + ' personality')
    report.update(kernel_link_verified=True, kernel_sha256=digest(kernel), kernel_classes=list(CLASSES),
                  kernel_personalities=personalities)
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
