#!/usr/bin/env python3
"""Run one bounded original-client/provider ABI boot; no display is published."""
import argparse
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--case',choices=['query','headless','default-off'],required=True)
a=p.parse_args()
base='build/native-prelink/native-owner-mobile-provider-query-candidate'
cmd=['python3','scripts/probe-darwinboot-xnu.py','--kernel',base+'.macho',
    '--prelink-report',base+'.json','--output','build/darwinboot-owner-hvf-mobile-provider-'+a.case,
    '--virtio-udif-image','.cache/firmware/target-volumes/044-76040-038.dmg',
    '--virtio-extra-block-image','.cache/firmware/target-volumes/044-76034-038.dmg',
    '--virtio-mmio-fdt','build/virtio-block-fixture/qemu-virt.dtb',
    '--root-match-volume-uuid','04769D26-2CA0-4825-B443-E2E5F88300F2',
    '--trust-cache','build/original-static-system-trust/original-static-system.bundle',
    '--armvirt','--armcpu','--qemu-vmm-present','--qemu-accel','hvf',
    '--owned-memory-size','0x20000000','--nvram-proxy','--crypto-hash-method','sha2-384',
    '--amfi-platform-only','--load-apfs','--skip-initializer-traces','--timeout','60',
    '--data-ramdisk','build/owner-data-fixture/owner-data-32.raw','--load-mobile-graphics']
if a.case!='headless':cmd+=['--qemu-ramfb']
if a.case=='default-off':cmd+=['--follow-target-main','--inspect-owner-root','--check-commpage']
else:cmd+=['--mobile-graphics-client-probe','--skip-policy-pointer-reads',
           '--break-symbol','_native_mobile_display_provider_checkpoint']
cmd+=['--inspect-boot-video']
subprocess.run(cmd,cwd=ROOT,check=True,timeout=180)
