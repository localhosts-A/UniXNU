#!/usr/bin/env python3
"""Build the fixed-source NativeMali PanVK library against a real Apple SDK.

No IOKit service is opened and no GPU work is submitted. --target ios emits an
arm64 iOS17 library; host is a macOS compile/link gate. The build profile binds
the prepared source, ABI, host code generators, SDK/compiler and options.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def read(command, env=None):
    return subprocess.check_output(list(map(str, command)), text=True, env=env).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target', choices=('host', 'ios'), default='ios')
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--configure-only', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if sys.platform != 'darwin':
        parser.error('A real macOS host and Xcode SDK are required')
    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    spec = importlib.util.spec_from_file_location('panvk_prepare', ROOT/'scripts/prepare-panvk-darwin.py')
    prepare = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(prepare)
    source_identity = prepare.prepare(ROOT, args.fetch, True)
    source = ROOT/'.cache/panvk-mesa-darwin'
    out = ROOT/('build/panvk-darwin-' + args.target)
    out.mkdir(parents=True, exist_ok=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('PAN_', 'PANVK_', 'MESA_SHADER_', 'NIR_'))}
    tools = ROOT/'build/panvk-host-tools/bin'
    env['PATH'] = str(tools) + os.pathsep + str(ROOT/'.cache/mali-venv/bin') + os.pathsep + env['PATH']
    identities = {}
    for name in ('mesa_clc', 'vtn_bindgen2', 'panfrost_compile'):
        executable = tools/name
        if not executable.is_file() or not os.access(executable, os.X_OK):
            parser.error('Build the fixed-source host code generators first: '+name)
        identities[name] = hashlib.sha256(executable.read_bytes()).hexdigest()
    meson, ninja = shutil.which('meson', path=env['PATH']), shutil.which('ninja', path=env['PATH'])
    if not meson or not ninja or not shutil.which('glslangValidator', path=env['PATH']):
        parser.error('meson, ninja and native glslangValidator are required')
    sdk_name = 'iphoneos' if args.target == 'ios' else 'macosx'
    sdk = read(['xcrun', '--sdk', sdk_name, '--show-sdk-path'])
    find = lambda name: read(['xcrun', '--sdk', sdk_name, '--find', name])
    options = ['-Dgallium-drivers=', '-Dvulkan-drivers=panfrost', '-Dpanfrost-kmds=kbase',
               '-Dplatforms=', '-Dglx=disabled', '-Degl=disabled', '-Dgbm=disabled',
               '-Dopengl=false', '-Dgles1=disabled', '-Dgles2=disabled', '-Dllvm=disabled',
               '-Dmesa-clc=system', '-Dprecomp-compiler=system', '-Dbuild-tests=false',
               '-Dvideo-codecs=', '-Dvulkan-layers=', '-Dlibunwind=disabled', '-Dlmsensors=disabled',
               '-Dvalgrind=disabled', '-Dexpat=disabled', '-Dzstd=disabled', '-Dshader-cache=disabled',
               '-Dbuildtype=release', '-Db_ndebug=true']
    env.update(CC=find('clang'), CXX=find('clang++'), CFLAGS='', CXXFLAGS='', CPPFLAGS='', LDFLAGS='')
    setup = [meson, 'setup', out, source, '--wrap-mode=nodownload']
    flags = ['-isysroot', sdk, '-fstack-protector-strong']
    if args.target == 'ios':
        flags = ['-target', 'arm64-apple-ios17.0', *flags]
        cross = out/'ios.cross'
        cross.write_text('[binaries]\n' + ''.join('%s = %r\n' % (key, find(tool)) for key, tool in
                         [('c', 'clang'), ('cpp', 'clang++'), ('objc', 'clang'), ('ar', 'ar'), ('strip', 'strip')]) +
                         "pkg-config = '/usr/bin/false'\n[host_machine]\nsystem = 'darwin'\ncpu_family = 'aarch64'\ncpu = 'arm64'\nendian = 'little'\n" +
                         '[properties]\nneeds_exe_wrapper = true\n[built-in options]\n' +
                         ''.join('%s = %r\n' % (key, flags) for key in
                                 ('c_args', 'cpp_args', 'objc_args', 'c_link_args', 'cpp_link_args', 'objc_link_args')))
        setup += ['--cross-file', cross]
    else:
        env.update(CFLAGS=' '.join(flags), CXXFLAGS=' '.join(flags))
    profile = {'source': source_identity, 'tools': identities, 'target': args.target, 'options': options,
               'compiler': read([env['CXX'], '--version']), 'sdk_path': sdk,
               'sdk_version': read(['xcrun', '--sdk', sdk_name, '--show-sdk-version']),
               'script_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}
    recorded = out/'build-profile.json'
    if recorded.exists() and json.loads(recorded.read_text()) != profile:
        parser.error('Build inputs changed; preserve this output and use a fresh build directory')
    def logged(command, name):
        log = out/(name + '.log')
        with log.open('w') as stream:
            result = subprocess.run(list(map(str, command)), env=env, stdout=stream, stderr=subprocess.STDOUT)
        if result.returncode:
            lines = log.read_text(errors='replace').splitlines()
            errors = [line for line in lines if 'error:' in line or line.startswith('FAILED:') or 'ERROR:' in line]
            raise SystemExit('\n'.join(errors[:35] + lines[-20:]))
    if not (out/'build.ninja').exists():
        logged(setup + options, 'configure')
        recorded.write_text(json.dumps(profile, indent=2, sort_keys=True) + '\n')
    if args.configure_only:
        print('Configured NativeMali PanVK against the actual '+sdk_name+' SDK; hardware_exercised=false')
        return
    logged([ninja, '-C', out, '-j', args.jobs, 'src/panfrost/vulkan/libvulkan_panfrost.dylib'], 'build')
    library = out/'src/panfrost/vulkan/libvulkan_panfrost.dylib'
    magic, cpu, _, kind = struct.unpack_from('<4I', library.read_bytes())
    if magic != 0xfeedfacf or kind != 6 or (args.target == 'ios' and cpu != 0x100000c):
        raise SystemExit('The driver must be an actual target Mach-O dynamic library')
    symbols = read(['xcrun', 'nm', '-gU', library])
    if ' T _vk_icdGetInstanceProcAddr' not in symbols:
        raise SystemExit('Vulkan ICD entry point is missing')
    links = read(['xcrun', 'otool', '-L', library])
    if 'libdrm' in links or '.so' in links:
        raise SystemExit('NativeMali Darwin library must not depend on Linux/DRM libraries')
    output = out/'libvulkan_native_mali.dylib'
    shutil.copyfile(library, output)
    subprocess.run(['xcrun', 'install_name_tool', '-id', '@rpath/libvulkan_native_mali.dylib', str(output)], check=True)
    (out/'symbols.txt').write_text(symbols)
    (out/'dependencies.txt').write_text(links)
    report = {'schema': 1, 'profile': profile, 'hardware_exercised': False,
              'library': output.name, 'bytes': output.stat().st_size,
              'sha256': hashlib.sha256(output.read_bytes()).hexdigest()}
    (out/'report.json').write_text(json.dumps(report, indent=2, sort_keys=True)+'\n')
    print('Built actual NativeMali PanVK '+args.target+' library; hardware_exercised=false')


if __name__ == '__main__':
    main()
