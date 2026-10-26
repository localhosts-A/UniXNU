#!/usr/bin/env python3
"""Build and verify pinned Apple msdosfs with the source XNU C ABI.

The six kernel implementation units come from the fixed Xcode target, with
no userspace filesystem library or replacement vnode operations. Registration
uses the upstream module entry point after XNU has initialized its VFS tables.
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
REVISION = 'f8d71e40f171f152b558c8163a971fee68c8072b'
PHASE = '4D3DCB3608E084D300C78A0B'
SOURCE = ROOT / 'refer/msdosfs'
SOURCE_DIR = Path('msdosfs.kextproj/msdosfs.kmodproj')
SOURCES = ('msdosfs_conv.c', 'msdosfs_denode.c', 'msdosfs_fat.c',
           'msdosfs_lookup.c', 'msdosfs_vfsops.c', 'msdosfs_vnops.c')
OUT = ROOT / 'build/source-msdosfs'
ARCHIVE = OUT / 'libmsdosfs.a'
REGISTRATION = ROOT / 'scripts/fixtures/source-msdosfs.c'
PATCH = ROOT / 'patches/xnu/source-msdosfs-registration.patch'
KERNEL_LOCALARGS_PATCH = ROOT / 'patches/xnu/source-kernel-local-mount-args.patch'
SYMBOLS = ('_msdosfs_module_start', '_msdosfs_vfsops',
           '_msdosfs_vnodeop_opv_desc', '_msdosfs_fat_vnodeop_opv_desc',
           '_msdosfs_vfs_mount', '_msdosfs_vfs_sync', '_msdosfs_vfs_unmount',
           '_source_msdosfs_register')
PRIVATE_SYMBOLS = ('_msdosfs_fat_vnodeop_opv_desc',)
GLOBAL_SYMBOLS = tuple(name for name in SYMBOLS if name not in PRIVATE_SYMBOLS)
KERNEL_LOCALARGS_SUPPORT_SYMBOLS = ('_vfs_is_kernel_localargs',)
VFSOPS = 'msdosfs_vfsops.c'
KERNEL_LOCALARGS_TRANSFORM = 'msdosfs-kernel-localargs-v1'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def helpers():
    spec = importlib.util.spec_from_file_location('msdosfs_build_helpers', ROOT / 'scripts/build-source-storage.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    return helper


def kernel_sources(source=SOURCE):
    project = (source / 'msdosfs.xcodeproj/project.pbxproj').read_text()
    match = re.search(r'\b' + PHASE + r' /\* Sources \*/ = \{(.*?)\n\s*\};', project, re.S)
    if not match or 'isa = PBXSourcesBuildPhase;' not in match[1]:
        raise ValueError('Pinned msdosfs kernel source phase is absent')
    names = re.findall(r'/\* ([^*]+) in Sources \*/', match[1])
    if tuple(names) != SOURCES:
        raise ValueError('Pinned msdosfs kernel source phase changed')
    paths = [source / SOURCE_DIR / name for name in names]
    if not all(path.is_file() for path in paths):
        raise ValueError('Pinned msdosfs kernel implementation is missing')
    return paths


def prepare_kernel_localargs_source(source, destination):
    """Patch only the owned build copy of the fixed msdosfs VFS source."""
    if source.resolve() == destination.resolve():
        raise ValueError('Refusing to patch the pinned msdosfs source in place')
    text = source.read_text()
    replacements = (
        ('\t\terror = copyin(data, &args, sizeof(struct msdosfs_args));',
         '\t\tif (vfs_is_kernel_localargs(mp)) {\n'
         '\t\t\tbcopy((const void *)data, &args, sizeof(args));\n'
         '\t\t\terror = 0;\n'
         '\t\t} else {\n'
         '\t\t\terror = copyin(data, &args, sizeof(struct msdosfs_args));\n'
         '\t\t}'),
        ('\t\tvfe.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBL64BITREADY | VFS_TBLREADDIR_EXTENDED;',
         '\t\tvfe.vfe_flags = VFS_TBLTHREADSAFE | VFS_TBLNOTYPENUM | VFS_TBLLOCALVOL | VFS_TBL64BITREADY | VFS_TBLREADDIR_EXTENDED | VFS_TBLKERNELARGS;'),
    )
    for anchor, replacement in replacements:
        if text.count(anchor) != 1:
            raise ValueError('Pinned msdosfs kernel-localargs source anchor changed')
        text = text.replace(anchor, replacement, 1)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text)
    return destination


def c_record(objects):
    path = objects / 'bsd/DEVELOPMENT/vfs_conf.o.json'
    record = json.loads(path.read_text().strip().rstrip(','))
    if not str(record.get('file', '')).endswith('/vfs_conf.c') or not isinstance(record.get('arguments'), list):
        raise ValueError('The actual XNU VFS C compile record is required')
    return path, record


def symbol_names(output):
    return {line.split()[-1] for line in output.splitlines() if line.split()}


def check_symbols(defined, undefined='', private_defined=''):
    # Compare exact final tokens, so e.g. a function name embedded in a longer
    # symbol or debug metadata cannot satisfy a missing implementation. The FAT
    # vnodeop descriptor is __private_extern__ upstream, so it is verified from
    # the complete Mach-O symbol listing instead of the global-only listing.
    definitions = symbol_names(defined)
    private_definitions = symbol_names(private_defined)
    unresolved = symbol_names(undefined)
    for name in GLOBAL_SYMBOLS:
        if name not in definitions or name in unresolved:
            raise ValueError('Missing resolved source msdosfs implementation: ' + name)
    for name in PRIVATE_SYMBOLS:
        if name not in private_definitions or name in unresolved:
            raise ValueError('Missing resolved source msdosfs implementation: ' + name)


def kernel_localargs_support(defined, undefined=''):
    definitions = symbol_names(defined)
    unresolved = symbol_names(undefined)
    return {name: name in definitions and name not in unresolved
            for name in KERNEL_LOCALARGS_SUPPORT_SYMBOLS}


def build():
    revision = subprocess.check_output(['git', '-C', SOURCE, 'rev-parse', 'HEAD'], text=True).strip()
    if revision != REVISION or subprocess.check_output(['git', '-C', SOURCE, 'status', '--porcelain'], text=True).strip():
        raise ValueError('msdosfs must use its clean fixed source revision')
    upstream_sources = kernel_sources()
    generated_vfsops = OUT / 'generated' / VFSOPS
    compiled_sources = [
        prepare_kernel_localargs_source(source, generated_vfsops)
        if source.name == VFSOPS else source
        for source in upstream_sources
    ]
    inputs = compiled_sources + [REGISTRATION]
    objects = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    record_path, record = c_record(objects)
    helper = helpers()
    OUT.mkdir(parents=True, exist_ok=True)
    ARCHIVE.unlink(missing_ok=True)
    (OUT / 'build-report.json').unlink(missing_ok=True)
    (OUT / 'compile-failures.json').unlink(missing_ok=True)
    sdk = Path(subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip())
    conditions = sdk / 'usr/include/TargetConditionals.h'
    if not conditions.is_file():
        raise ValueError('Missing actual iPhoneOS SDK TargetConditionals.h: ' + str(conditions))
    include = OUT / 'include'
    include.mkdir(exist_ok=True)
    shutil.copyfile(conditions, include / conditions.name)
    declaration_inputs = [{'source': str(conditions), 'sha256': digest(conditions)}]
    # The DEVELOPMENT compile record leaves DEBUG undefined, while msdosfs
    # also uses it as a C expression in its hibernation diagnostics. Preserve
    # explicit DEBUG values and give the undefined case its preprocessor value.
    build_config = include / 'source-msdosfs-config.h'
    build_config.write_text('#ifndef DEBUG\n#define DEBUG 0\n#endif\n')
    declaration_inputs.append({'source': str(build_config), 'sha256': digest(build_config)})
    (OUT / 'sdk-inputs.json').write_text(json.dumps(declaration_inputs, indent=2) + '\n')
    xnu = ROOT / '.cache/xnu-work'
    includes = [OUT / 'generated', include, SOURCE / SOURCE_DIR,
                xnu / 'bsd', xnu / 'iokit', xnu / 'osfmk']

    def compile_unit(source):
        obj = OUT / (source.name + '.o')
        obj.unlink(missing_ok=True)
        command = helper.compile_command(record, source, obj, includes)
        command[3] = 'clang'
        command += ['-DMSDOSFS_AUTO_UNLOAD=0', '-include', str(build_config)]
        log = obj.with_suffix('.log')
        row = {'source': str(source.relative_to(ROOT)), 'source_sha256': digest(source), 'command': command}
        (OUT / (source.name + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
        try:
            helper.run_logged(command, Path(record['directory']), log)
            helper.validate_object(obj)
        except (RuntimeError, ValueError) as error:
            return obj, dict(row, error=str(error), log_sha256=digest(log) if log.exists() else None)
        return obj, dict(row, object_sha256=digest(obj), log_sha256=digest(log))

    with ThreadPoolExecutor(max_workers=4) as executor:
        results = list(executor.map(compile_unit, inputs))
    failures = [row for _, row in results if 'error' in row]
    (OUT / 'compile-failures.json').write_text(json.dumps(failures, indent=2) + '\n')
    if failures:
        raise RuntimeError('msdosfs source compile failures: ' + str(len(failures)) + '\n' +
                           '\n'.join(row['source'] + ': ' + row['error'][:1200] for row in failures))
    subprocess.run(['xcrun', 'ar', 'rcs', ARCHIVE, *[obj for obj, _ in results]], check=True)
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', ARCHIVE], text=True)
    private_defined = subprocess.check_output(['xcrun', 'nm', '-mU', ARCHIVE], text=True)
    check_symbols(defined, private_defined=private_defined)
    report = {'source_revision': revision, 'source_phase': PHASE,
              'source_files': list(SOURCES), 'objects': [row for _, row in results],
              'kernel_localargs_transform': KERNEL_LOCALARGS_TRANSFORM,
              'kernel_localargs_transform_sha256': digest(Path(__file__).resolve()),
              'kernel_localargs_xnu_patch_sha256': digest(KERNEL_LOCALARGS_PATCH),
              'upstream_vfsops_sha256': digest(next(path for path in upstream_sources if path.name == VFSOPS)),
              'kernel_localargs_vfsops_sha256': digest(generated_vfsops),
              'registration_source_sha256': digest(REGISTRATION), 'registration_patch_sha256': digest(PATCH),
              'compile_record_sha256': digest(record_path), 'archive_sha256': digest(ARCHIVE),
              'declaration_inputs': declaration_inputs,
              'source_archive_built': True, 'kernel_link_verified': False,
              'registration_runtime_qualified': False, 'fat32_runtime_qualified': False,
              'hardware_qualified': False}
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: actual source msdosfs archive built; kernel link, VFS and FAT32 runtime require verification')


def verify_kernel(kernel):
    report = json.loads((OUT / 'build-report.json').read_text())
    if (report.get('source_revision') != REVISION or report.get('source_files') != list(SOURCES) or
            report.get('source_archive_built') is not True or report.get('archive_sha256') != digest(ARCHIVE) or
            report.get('kernel_localargs_transform') != KERNEL_LOCALARGS_TRANSFORM or
            report.get('kernel_localargs_transform_sha256') != digest(Path(__file__).resolve()) or
            report.get('kernel_localargs_xnu_patch_sha256') != digest(KERNEL_LOCALARGS_PATCH) or
            report.get('upstream_vfsops_sha256') != digest(next(
                path for path in kernel_sources() if path.name == VFSOPS)) or
            report.get('kernel_localargs_vfsops_sha256') != digest(OUT / 'generated' / VFSOPS) or
            report.get('registration_source_sha256') != digest(REGISTRATION) or
            report.get('registration_patch_sha256') != digest(PATCH)):
        raise ValueError('Source msdosfs archive/registration/report mismatch')
    defined = subprocess.check_output(['xcrun', 'nm', '-gU', kernel], text=True)
    private_defined = subprocess.check_output(['xcrun', 'nm', '-mU', kernel], text=True)
    undefined = subprocess.check_output(['xcrun', 'nm', '-u', kernel], text=True)
    definitions = symbol_names(defined)
    private_definitions = symbol_names(private_defined)
    unresolved = symbol_names(undefined)
    localargs_support = kernel_localargs_support(defined, undefined)
    symbol_report = {
        'kernel_sha256': digest(kernel),
        'global_definitions': {name: name in definitions for name in GLOBAL_SYMBOLS},
        'private_definitions': {name: name in private_definitions for name in PRIVATE_SYMBOLS},
        'kernel_localargs_support': localargs_support,
        'unresolved_required': sorted(set(SYMBOLS) & unresolved),
    }
    (OUT / 'kernel-symbol-report.json').write_text(json.dumps(symbol_report, indent=2) + '\n')
    check_symbols(defined, undefined, private_defined)
    if not all(localargs_support.values()):
        raise ValueError('Final kernel lacks the opted-in local mount-args VFS API')
    caller = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o'
    references = subprocess.check_output(['xcrun', 'nm', '-u', caller], text=True)
    if '_source_msdosfs_register' not in {line.split()[-1] for line in references.splitlines() if line.split()}:
        raise ValueError('Compiled bsd_init does not call source msdosfs registration')
    report.update(kernel_link_verified=True, kernel_sha256=digest(kernel),
                  kernel_symbols_verified=symbol_report,
                  registration_caller_sha256=digest(caller), registration_hook_compiled=True)
    (OUT / 'kernel-link-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: source msdosfs implementations and VFS registration hook linked; FAT32 runtime remains unqualified')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-kernel', type=Path)
    args = parser.parse_args(argv)
    mode = os.environ.get('XNU_SOURCE_MSDOSFS', '0')
    if mode == '0':
        print('SKIP: source msdosfs disabled')
    elif mode != '1':
        raise SystemExit('XNU_SOURCE_MSDOSFS must be 0 or 1')
    elif args.verify_kernel:
        verify_kernel(args.verify_kernel.resolve())
    else:
        build()


if __name__ == '__main__':
    main()
