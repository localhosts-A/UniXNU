#!/usr/bin/env python3
"""Clone our mutable APFS Data fixture and stage only own code plus exact AIR input."""
import argparse
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--broker-processes',action='store_true');p.add_argument('--broker-policy',choices=['full','without-container','without-client','parent-client','unrelated-client'],default='full');p.add_argument('--legacy-swap',action='store_true');p.add_argument('--legacy-swap-wait',action='store_true');p.add_argument('--framebuffer-geometry',action='store_true');p.add_argument('--framebuffer-power',action='store_true');p.add_argument('--framebuffer-protocol',action='store_true');p.add_argument('--empty-end',action='store_true');p.add_argument('--display-environment',action='store_true');p.add_argument('--fp-context',action='store_true');p.add_argument('--compose',action='store_true');p.add_argument('--resources',action='store_true');p.add_argument('--present',action='store_true');p.add_argument('--bgra',action='store_true');p.add_argument('--iosurface',action='store_true');p.add_argument('--offline-metal-api',action='store_true');p.add_argument('--offline-gpu-transport',action='store_true');p.add_argument('--metal-runtime',action='store_true');p.add_argument('--library-input',action='store_true');p.add_argument('--fragment-inputs',action='store_true');p.add_argument('--library-tests',action='store_true');p.add_argument('--vpc-tests',action='store_true');p.add_argument('--case',choices=['positive','bad-input'],default='positive');p.add_argument('--async-commands',action='store_true');p.add_argument('--fences',action='store_true');p.add_argument('--sandbox-extension',action='store_true');p.add_argument('--metal-layer',action='store_true');p.add_argument('--renderbox-present',type=int,choices=[1,2]);p.add_argument('--depth-stencil',action='store_true');p.add_argument('--private-texture',action='store_true');p.add_argument('--renderbox-draw',action='store_true');p.add_argument('--renderbox-device',action='store_true');p.add_argument('--plugin-control',choices=['full','without-mount-permission','tampered-code'],default='full');p.add_argument('--plugin-addon',action='store_true');p.add_argument('--plugin-bundle',action='store_true');p.add_argument('--shared-surfaces',action='store_true',help='Pass actual IOSurface Mach rights between two GPU processes')
p.add_argument('--shared-events',action='store_true',help='Real native shared-event Mach capabilities between two processes')
p.add_argument('--shared-notifications',action='store_true',help='Original listener queue delivery and bounded context cancellation')
p.add_argument('--shared-coding',action='store_true',help='Original NSXPC shared-handle secure coding, separate from cross-process authority')
p.add_argument('--shared-cross-process',action='store_true',help='Actual original NSXPC endpoint and handle coding between two native GPU workers')
p.add_argument('--vertex-inputs',action='store_true',help='Actual original QuartzCore vertex layouts and native GPU buffers')
args=p.parse_args()
if args.vertex_inputs and any((args.shared_events, args.shared_notifications, args.shared_coding, args.shared_cross_process, args.async_commands, args.fences, args.metal_layer, args.renderbox_present, args.depth_stencil, args.private_texture, args.renderbox_draw, args.renderbox_device, args.plugin_addon, args.plugin_bundle, args.shared_surfaces, args.sandbox_extension, args.legacy_swap, args.legacy_swap_wait, args.compose, args.resources, args.present)):p.error("Select the independent vertex input instrument")
if args.vertex_inputs:args.broker_processes=True
if args.shared_notifications and (args.shared_coding or args.shared_cross_process):p.error('Select notifications or secure coding as independent instruments')
if args.shared_cross_process:args.shared_coding=True
if args.shared_coding:args.shared_events=True
if args.shared_notifications:args.shared_events=True
if args.shared_events:args.broker_processes=True
if args.async_commands:args.fences=True
if args.fences:args.metal_layer=True
if args.sandbox_extension:args.broker_processes=True
if args.metal_layer:
    if args.renderbox_present or args.depth_stencil or args.private_texture or args.renderbox_draw:p.error('Select one independent layer/renderer instrument')
    args.renderbox_device=True
if args.renderbox_present:
    if args.depth_stencil or args.private_texture or args.renderbox_draw:p.error('Select one independent renderer/presentation instrument')
    args.renderbox_device=True
if args.depth_stencil:
    if args.private_texture or args.renderbox_draw:p.error('Select one independent resource or renderer instrument')
    args.renderbox_device=True
if args.private_texture:
    if args.renderbox_draw:p.error('Select one independent resource or original renderer instrument')
    args.renderbox_device=True
if args.renderbox_draw:args.renderbox_device=True
if args.renderbox_device:
    if args.plugin_control!='full':p.error('RenderBox device observation is a separate instrument')
    args.plugin_addon=True
if args.plugin_control!='full':args.plugin_addon=True
if args.plugin_addon:args.plugin_bundle=True
if args.plugin_bundle:args.shared_surfaces=True
if args.shared_surfaces:args.broker_processes=True
if args.shared_surfaces and args.broker_policy!='full':p.error('Sharing instrument requires both existing scoped diagnostic permissions')
if args.broker_policy!='full' and not args.broker_processes:p.error('Permission controls require the GPU process instrument')
if args.broker_processes:
    if not args.offline_metal_api:p.error('GPU process instrument requires --offline-metal-api')
    if args.legacy_swap or args.legacy_swap_wait or args.compose or args.resources or args.present:p.error('Select one GPU runtime instrument')
    args.bgra=True
if args.legacy_swap_wait:args.legacy_swap=True
if args.empty_end and not args.framebuffer_protocol:p.error('--empty-end requires the framebuffer protocol instrument')
if args.legacy_swap and (args.compose or args.resources or args.present):p.error('Select one presentation instrument')
if args.legacy_swap:
    if not args.offline_metal_api:p.error('--legacy-swap requires --offline-metal-api')
    args.bgra=True
if args.compose and args.resources:p.error('Select composition or resource instrument')
if args.compose:args.present=True
if args.resources:args.present=True
if args.present:args.bgra=True
if args.bgra:args.iosurface=True
if args.iosurface and not args.offline_metal_api:raise SystemExit('IOSurface requires the Metal API instrument')
if args.fragment_inputs and not args.library_input:raise SystemExit('Fragment inputs require direct libraries')
if sum((args.framebuffer_geometry,args.framebuffer_power,args.framebuffer_protocol,args.display_environment,args.fp_context,args.library_input,args.library_tests,args.metal_runtime,args.vpc_tests,args.offline_gpu_transport,args.offline_metal_api))>1:raise SystemExit('Select one independent instrument')
broker_kind='offline-metal-vertex-inputs' if args.vertex_inputs else 'offline-metal-shared-cross-process' if args.shared_cross_process else 'offline-metal-shared-coding' if args.shared_coding else 'offline-metal-shared-notifications' if args.shared_notifications else 'offline-metal-shared-events' if args.shared_events else 'offline-metal-async' if args.async_commands else 'offline-metal-fences' if args.fences else 'offline-sandbox-extension' if args.sandbox_extension else 'offline-metal-layer' if args.metal_layer else ('offline-renderbox-present-'+str(args.renderbox_present)) if args.renderbox_present else 'offline-metal-depth-stencil' if args.depth_stencil else 'offline-metal-private-texture' if args.private_texture else 'offline-renderbox-draw' if args.renderbox_draw else 'offline-renderbox-device' if args.renderbox_device else ('offline-metal-addon'+('' if args.plugin_control=='full' else '-'+args.plugin_control)) if args.plugin_addon else 'offline-metal-plugin' if args.plugin_bundle else 'offline-metal-shared-surfaces' if args.shared_surfaces else 'offline-metal-processes'
OUT=ROOT/(('build/'+broker_kind+('' if args.broker_policy=='full' else '-'+args.broker_policy)) if args.broker_processes else 'build/framebuffer-geometry-diagnostic' if args.framebuffer_geometry else 'build/framebuffer-power-diagnostic' if args.framebuffer_power else 'build/offline-legacy-swap-wait' if args.legacy_swap_wait else 'build/offline-legacy-swap' if args.legacy_swap else 'build/framebuffer-empty-protocol-diagnostic' if args.empty_end else 'build/framebuffer-protocol-diagnostic' if args.framebuffer_protocol else 'build/display-environment-diagnostic' if args.display_environment else 'build/native-fp-context' if args.fp_context else 'build/offline-gop-composition' if args.compose else 'build/offline-gop-resources' if args.resources else 'build/offline-gop-presentation' if args.present else 'build/offline-metal-bgra' if args.bgra else 'build/offline-metal-iosurface' if args.iosurface else 'build/offline-metal-api' if args.offline_metal_api else 'build/offline-gpu-transport' if args.offline_gpu_transport else 'build/adreno-vpc-native-tests' if args.vpc_tests else 'build/metal-runtime-diagnostic' if args.metal_runtime else 'build/air-library-native-tests' if args.library_tests else 'build/air-fragment-inputs-diagnostic' if args.fragment_inputs else 'build/air-library-diagnostic' if args.library_input else 'build/air-nir-diagnostic')
suffix='' if args.case=='positive' else '-bad-input'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()
source=ROOT/'build/owner-data-fixture/owner-data-32.raw';seed=json.loads(source.with_name('seed.json').read_text())
if sha(source)!=seed['sha256']:raise SystemExit('Own Data seed identity mismatch')
process=OUT/'air-native';report=json.loads((OUT/'build-report.json').read_text())
if sha(process)!=report['process_sha256']:raise SystemExit('Own signed compiler changed')
manifest=(OUT/'test-inputs.json') if (args.bgra or args.fp_context or args.display_environment or args.framebuffer_protocol or args.framebuffer_power or args.framebuffer_geometry) else ROOT/('build/offline-metal-iosurface/test-inputs.json' if args.iosurface else 'build/offline-metal-api/test-inputs.json' if args.offline_metal_api else 'build/offline-gpu-transport/test-inputs.json' if args.offline_gpu_transport else 'build/adreno-vpc-native-tests/test-inputs.json' if args.vpc_tests else 'build/gpu-analysis/metal-device-abi.json' if args.metal_runtime else 'build/air-library-native-tests/test-inputs.json' if args.library_tests else 'scripts/data/native-air-fragment-inputs.json' if args.fragment_inputs else 'scripts/data/native-air-diagnostic-inputs.json')
if sha(manifest)!=report['manifest_sha256']:raise SystemExit('Input manifest differs from signed instrument')
cases=[] if (args.framebuffer_geometry or args.framebuffer_power or args.framebuffer_protocol or args.display_environment or args.fp_context or args.metal_runtime or args.library_tests or args.vpc_tests or args.offline_gpu_transport or args.offline_metal_api) else json.loads(manifest.read_text())['cases']
inputs=[]
for case in ([] if args.library_input else cases):
    bitcode=ROOT/f"build/gpu-analysis/air/{case['library']}/module-{case['index']}.bc"
    if sha(bitcode)!=case['input_sha256']:raise SystemExit('Original AIR differs')
    inputs.append((case,bitcode))
image=OUT/('compiler-data'+suffix+'-32.cdr')
if image.exists():raise SystemExit('Refuse to overwrite a diagnostic Data image')
shutil.copyfile(source,image);mount=OUT/'data-mount';mount.mkdir(exist_ok=True)
attached=plistlib.loads(subprocess.check_output(['hdiutil','attach',str(image),'-readwrite','-nobrowse','-owners','off','-mountpoint',str(mount),'-plist'],timeout=30))
device=attached['system-entities'][0]['dev-entry']
addon_report={}
try:
    info=plistlib.loads(subprocess.check_output(['diskutil','info','-plist',str(mount)],timeout=30))
    if info['VolumeUUID']!=seed['volume_uuid']:raise SystemExit('Cloned Data UUID differs')
    if shutil.disk_usage(mount).free<process.stat().st_size+sum(p.stat().st_size for _,p in inputs)+1024*1024:raise SystemExit('Insufficient scratch Data space')
    (mount/'tmp').mkdir(exist_ok=True)
    if args.plugin_bundle:
        bundle=OUT/'NativeOfflineMetal.bundle'
        for member,key in [('NativeOfflineMetal','plugin_sha256'),('Info.plist','plugin_info_sha256'),('_CodeSignature/CodeResources','plugin_resources_sha256')]:
            if sha(bundle/member)!=report[key]:raise SystemExit('Own signed plugin changed')
        shutil.copytree(bundle,mount/'tmp/NativeOfflineMetal.bundle')
        if args.plugin_addon:
            addon_root=mount/'tmp/native-metal-addons';addon_root.mkdir()
            destination=addon_root/'NativeOfflineMetal.bundle';shutil.copytree(bundle,destination)
            for member,key in [('NativeOfflineMetal','plugin_sha256'),('Info.plist','plugin_info_sha256'),('_CodeSignature/CodeResources','plugin_resources_sha256')]:
                if sha(destination/member)!=report[key]:raise SystemExit('Own addon plugin copy changed')
            addon_report=dict(volume_uuid=seed['volume_uuid'],plugin_sha256=report['plugin_sha256'],offline_only=True,
                mutable_backing=True,read_only_bindfs_view=True,production_root_authorization=False)
            if args.plugin_control=='tampered-code':
                # Corrupt only our own factory's actual instruction page,
                # retaining its original signature and explicit trusted hash.
                target=destination/'NativeOfflineMetal';symbols=subprocess.check_output(['xcrun','nm','-n',target],text=True)
                method=re.search(r'^([0-9a-f]{16})\s+\S\s+-\[NativeOfflineMetalPlugin initWithAcceleratorPort:\]$',symbols,re.M)
                if not method:raise SystemExit('Cannot identify actual own factory method')
                sys.path.insert(0,str(ROOT/'kits/NativePrelink'));from native_prelink import macho,backed_offset
                pristine=target.read_bytes();offset=backed_offset(macho(pristine)['segments'],int(method[1],16),4,len(pristine))
                broken=bytearray(pristine);broken[offset]^=1;target.write_bytes(broken)
                if sum(a!=b for a,b in zip(pristine,broken))!=1:raise SystemExit('Own negative copy must change exactly one instruction byte')
                addon_report.update(tamper_file_offset=offset,actual_plugin_sha256=sha(target),original_signature_retained=True)
            (mount/'tmp/native-metal-addon.plist').write_bytes(plistlib.dumps(addon_report))
    if args.sandbox_extension:
        consumer=OUT/'sandbox-consumer'
        if sha(consumer)!=report['consumer_sha256']:raise SystemExit('Exact signed consumer required')
        shutil.copyfile(consumer,mount/'tmp/sandbox-consumer');(mount/'tmp/sandbox-consumer').chmod(0o755)
    shutil.copyfile(process,mount/'tmp/air-native');(mount/'tmp/air-native').chmod(0o755)
    for case,bitcode in inputs:
        target=mount/('tmp/'+case['name']+'.bc');shutil.copyfile(bitcode,target)
        if sha(target)!=case['input_sha256']:raise SystemExit('Staged input differs')
    if sha(mount/'tmp/air-native')!=report['process_sha256']:raise SystemExit('Staged process differs')
    if args.case=='bad-input':
        if not cases:raise SystemExit('Bad AIR input requires the compiler instrument')
        target=mount/('tmp/'+cases[0]['name']+'.bc');raw=bytearray(target.read_bytes());raw[0]^=1;target.write_bytes(raw)
        if sha(target)==cases[0]['input_sha256']:raise SystemExit('Negative input must actually differ')
finally:subprocess.run(['hdiutil','detach',device],check=True,timeout=30)
(OUT/('data'+suffix+'-report.json')).write_text(json.dumps(dict(bytes=image.stat().st_size,sha256=sha(image),volume_uuid=seed['volume_uuid'],
    process_sha256=report['process_sha256'],manifest_sha256=sha(manifest),cases=len(cases),seed_sha256=seed['sha256'],
    addon=addon_report,
    original_os_modified=False,negative_input_copy_modified=args.case=='bad-input',own_mutable_data=True,production_storage_integrity=False),indent=2)+'\n')
print('PASS: own cloned Data fixture staged; original OS and SystemOS remain unchanged')
