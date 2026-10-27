#!/usr/bin/env python3
"""Run one bounded original Legacy init/start/control using real GOP resources."""
import argparse
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--case',required=True,choices=['init','start','headless','default-off']);p.add_argument('--original-error',action='store_true',help='Observe the actual original start failure report before cleanup')
p.add_argument('--gop',choices=['mapped','publication'],help='Use actual GOP mapping backend; stop before any service publication')
p.add_argument('--system',choices=['started','client','mode','query','metal','protocol','empty-protocol','power','geometry','dotpitch','frameinfo','frameinfo-return','exception','panic'],help='Explicit genuine GOP-backed original system-client experiment')
p.add_argument('--profile',choices=['late','gop','system','system-dt','pitch','info','scale','pm','validate','transfer-ready'],default='gop',help='Exact retained candidate profile')
p.add_argument('--process',help='Observe actual exceptions from this named process only')
p.add_argument('--read-user-register',action='append',default=[],help='Read bounded saved user pointer memory at the actual exception')
p.add_argument('--capture-screen',action='store_true',help='Capture the actual QEMU console without resuming the guest')
a=p.parse_args()
if a.read_user_register and a.system!='exception':p.error('--read-user-register requires --system exception')
if a.process and a.system!='exception':p.error('--process requires --system exception')
if a.system:
    if a.gop or a.original_error or a.case!='start':p.error('System-client observation requires start without other GOP/error observations')
    if a.profile not in ('system','system-dt','pitch','info','scale','pm','validate','transfer-ready'):a.profile='system-dt'
if a.gop and a.profile!='gop':p.error('GOP observations require the GOP candidate profile')
base='build/native-prelink/native-owner-legacy-display-'+a.profile+'-candidate'
cmd=['python3','scripts/probe-darwinboot-xnu.py','--kernel',base+'.macho','--prelink-report',base+'.json',
    '--output','build/darwinboot-owner-hvf-legacy-display-'+a.case+('-error' if a.original_error else '')+('-'+a.profile+'-'+(a.system or 'control') if a.profile.startswith('system') or a.profile in ('pitch','info','scale','pm','validate','transfer-ready') else '-gop-'+(a.gop or 'control') if a.profile=='gop' else ''),
    '--virtio-udif-image','.cache/firmware/target-volumes/044-76040-038.dmg',
    '--virtio-extra-block-image','.cache/firmware/target-volumes/044-76034-038.dmg',
    '--virtio-mmio-fdt','build/virtio-block-fixture/qemu-virt.dtb',
    '--root-match-volume-uuid','04769D26-2CA0-4825-B443-E2E5F88300F2',
    '--trust-cache','build/original-static-system-trust/original-static-system.bundle',
    '--armvirt','--armcpu','--qemu-vmm-present','--qemu-accel','hvf','--owned-memory-size','0x20000000',
    '--nvram-proxy','--crypto-hash-method','sha2-384','--amfi-platform-only','--load-apfs','--load-mobile-graphics',
    '--skip-initializer-traces','--timeout','60','--data-ramdisk','build/owner-data-fixture/owner-data-32.raw','--inspect-boot-video']
if a.process:
    cmd[cmd.index('--output')+1]+='-'+a.process
    cmd+=['--entry-process-name',a.process,'--entry-max-skips','128']
if a.read_user_register:
    cmd[cmd.index('--output')+1]+='-registers'
    for request in a.read_user_register:cmd+=['--read-saved-user-register',request]
if a.capture_screen:
    cmd[cmd.index('--output')+1]+='-screen'
    cmd+=['--capture-qemu-screen']
if a.case!='headless':cmd+=['--qemu-ramfb']
if a.case=='default-off':cmd+=['--follow-target-main','--inspect-owner-root','--check-commpage']
else:
    cmd+=['--mobile-graphics-client-probe','--legacy-display-phase','4' if a.system else '2' if a.case=='start' else '1']
    if a.system:
        cmd+=['--legacy-display-gop','--legacy-display-register']
        if a.profile in ('scale','pm','validate','transfer-ready'):cmd+=['--legacy-display-unit-scale']
        if a.profile in ('pm','validate','transfer-ready'):cmd+=['--legacy-display-logical-pm']
        if a.profile in ('validate','transfer-ready'):cmd+=['--legacy-display-validate']
        if a.profile=='transfer-ready':cmd+=['--legacy-display-transfer']
        if a.system in ('protocol','empty-protocol','power','geometry'):
            instrument='framebuffer-geometry-diagnostic' if a.system=='geometry' else 'framebuffer-power-diagnostic' if a.system=='power' else 'framebuffer-empty-protocol-diagnostic' if a.system=='empty-protocol' else 'framebuffer-protocol-diagnostic'
            cmd[cmd.index('--data-ramdisk')+1]='build/'+instrument+'/compiler-data-32.cdr'
            cmd[cmd.index('--trust-cache')+1]='build/'+instrument+'/offline-trust.bundle'
            cmd+=['--native-air-init','--break-symbol','_exception_triage','--inspect-current-process','--inspect-entry-user-state','--read-saved-user-register','13:80','--inspect-owner-root','--check-commpage']
        elif a.system=='metal':cmd+=['--follow-target-main','--follow-metal-device','--inspect-owner-root','--check-commpage']
        elif a.system=='query':cmd+=['--follow-target-main','--follow-display-query','--inspect-owner-root','--check-commpage']
        elif a.system=='mode':cmd+=['--follow-target-main','--follow-display-mode','--inspect-owner-root','--check-commpage']
        elif a.system=='started':cmd+=['--break-symbol','_native_legacy_display_checkpoint','--skip-policy-pointer-reads']
        elif a.system=='client':cmd+=['--break-symbol','_native_legacy_client_checkpoint','--inspect-current-process','--inspect-owner-root','--inspect-dyld-flags','--check-commpage']
        elif a.system=='exception':cmd+=['--break-symbol','_exception_triage','--inspect-current-process','--inspect-entry-user-state','--read-entry-region','1:0:10','--read-frame-locals','--inspect-owner-root','--skip-policy-pointer-reads']
        elif a.system=='frameinfo-return':cmd+=['--break-address','0xfffffff0057786e8','--read-frame-locals','--read-frame-indirect','0:-20:40','--inspect-owner-root','--skip-policy-pointer-reads']
        elif a.system=='frameinfo':cmd+=['--break-address','0xfffffff0057786e4','--read-frame-locals','--read-entry-region','2:0:4','--read-entry-region','3:0:40','--inspect-owner-root','--skip-policy-pointer-reads']
        elif a.system=='dotpitch':cmd+=['--break-address','0xfffffff005778194','--read-frame-locals','--inspect-owner-root','--skip-policy-pointer-reads']
        else:
            # The general APFS probe intentionally replaces its default panic
            # target with its load checkpoint unless observing the owner-root
            # path. Keep that explicit observation for the real panic target.
            cmd+=['--break-symbol','_panic','--inspect-owner-root','--skip-policy-pointer-reads']
    elif a.gop:
        if a.case!='start' or a.original_error:p.error('GOP observation requires start without --original-error')
        cmd+=['--legacy-display-gop','--skip-policy-pointer-reads']
        cmd+=(['--break-symbol','_native_legacy_gop_mapping_checkpoint'] if a.gop=='mapped' else ['--break-address','0xfffffff0057880d4'])
    elif a.original_error:
        if a.case!='start':p.error('Original start failure observation requires --case start')
        cmd+=['--break-address','0xfffffff0057870b0','--read-frame-locals','--skip-policy-pointer-reads']
    else:cmd+=['--break-symbol','_native_legacy_display_checkpoint','--skip-policy-pointer-reads']
subprocess.run(cmd,cwd=ROOT,check=True,timeout=180)
