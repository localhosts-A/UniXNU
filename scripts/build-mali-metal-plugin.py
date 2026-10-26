#!/usr/bin/env python3
"""Build the NativeMali Metal plugin with actual Apple SDKs; never opens a GPU.

--compile-only omits the ICD and records an incomplete bundle for compiler CI.
The default requires the matching built PanVK driver and signs a complete bundle.
"""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--target',choices=['host','ios'],default='ios')
    parser.add_argument('--compile-only',action='store_true')
    parser.add_argument('--fetch',action='store_true')
    args=parser.parse_args()
    subprocess.run([sys.executable,ROOT/'scripts/build-native-metal-bridge.py','--backend','mali','--target',args.target,
                    *(['--fetch'] if args.fetch else [])],check=True)
    kit=ROOT/'build/native-mali-metal/source'
    out=kit.parent/args.target
    bundle=out/'NativeMaliMetal.bundle'
    bundle.mkdir(parents=True,exist_ok=True)
    sdk_name='iphoneos' if args.target=='ios' else 'macosx'
    triple='arm64-apple-ios17.0' if args.target=='ios' else 'arm64-apple-macos14.0'
    sdk=subprocess.check_output(['xcrun','--sdk',sdk_name,'--show-sdk-path'],text=True).strip()
    exe=bundle/'NativeMaliMetal'
    command=['xcrun','clang++','-target',triple,'-isysroot',sdk,'-std=c++17','-fobjc-arc','-O2','-g',
             '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-deprecated-declarations',
             '-fstack-protector-strong','-bundle','-I',kit,'-I',ROOT/'drivers/NativeMaliMetal',ROOT/'drivers/NativeMaliMetal/NativeMaliMetalPlugin.mm',
             ROOT/'drivers/NativeMaliMetal/NativeMaliFramePresenter.mm',
             kit/'NativeBridgeMetalFrontend.mm',ROOT/f'build/native-mali-metal/bridge/{args.target}/libnative_metal_bridge.a',
             '-Wl,-dead_strip','-lobjc','-framework','Foundation','-framework','Metal','-framework','IOSurface',
             '-framework','CoreFoundation','-o',exe]
    with (out/'plugin-build.log').open('w') as log:
        result=subprocess.run([str(x) for x in command],stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:raise SystemExit((out/'plugin-build.log').read_text()[-10000:])
    if int.from_bytes(exe.read_bytes()[:4],'little')!=0xfeedfacf:raise SystemExit('Plugin is not Mach-O')
    dependencies=subprocess.check_output(['otool','-L',exe],text=True)
    if any(name in dependencies for name in ['libdrm','freedreno','MoltenVK']):raise SystemExit('Unexpected GPU dependency')
    (out/'dependencies.txt').write_text(dependencies)
    symbols=subprocess.check_output(['nm','-gU',exe],text=True)
    for symbol in ('_native_mali_metal_open','_native_mali_metal_set_frame_sink'):
        if symbol not in symbols:raise SystemExit('Missing plugin entry point '+symbol)
    (bundle/'Info.plist').write_bytes(plistlib.dumps(dict(CFBundleIdentifier='org.unixnu.NativeMaliMetal',
        CFBundleExecutable='NativeMaliMetal',CFBundlePackageType='BNDL',CFBundleVersion='1',
        CFBundleShortVersionString='1',NSPrincipalClass='NativeMaliMetalDevice',MinimumOSVersion='17.0')))
    identities={}
    driver=ROOT/f'build/panvk-darwin-{args.target}/libvulkan_native_mali.dylib'
    if not args.compile_only:
        if not driver.is_file():raise SystemExit('Build the matching PanVK Darwin ICD first')
        shutil.copyfile(driver,bundle/driver.name)
    elif (bundle/'libvulkan_native_mali.dylib').exists():
        raise SystemExit('Compile-only output must not contain an old ICD')
    for item in ([bundle/driver.name] if not args.compile_only else [])+[bundle]:
        identifier='org.unixnu.NativeMaliMetal' if item==bundle else 'org.unixnu.NativeMali.ICD'
        subprocess.run(['codesign','--force','--sign','-','--pagesize','4096','--identifier',identifier,item],check=True)
        subprocess.run(['codesign','--verify','--strict',item],check=True)
        detail=subprocess.run(['codesign','--display','--verbose=4',item],capture_output=True,text=True,check=True).stderr
        if not re.search(r'^Page size=4096$',detail,re.M):raise SystemExit('Signature page size is not 4096')
        identities[item.name]=re.search(r'^CDHash=([a-f0-9]{40})$',detail,re.M)[1]
    profile={'schema':1,'target':triple,'sdk':sdk,'source':json.loads((kit.parent/'source-profile.json').read_text()),
             'driver_bundled':not args.compile_only,'cdhashes':identities,'hardware_exercised':False,
             'files':{str(p.relative_to(bundle)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(bundle.rglob('*')) if p.is_file()}}
    (out/'plugin-profile.json').write_text(json.dumps(profile,indent=2,sort_keys=True)+'\n')
    print(f'Built NativeMali Metal plugin for {triple}; driver_bundled={not args.compile_only}; hardware_exercised=false')


if __name__=='__main__':main()
