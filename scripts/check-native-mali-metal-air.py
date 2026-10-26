#!/usr/bin/env python3
"""Translate the pinned Metal AIR compute fixture to validated Vulkan SPIR-V without a GPU."""
import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'.cache/gpu-reference/metal2vulkan'
OUT=ROOT/'build/native-mali-metal/air-offline'
FIXTURE=SOURCE/'crates/metal2vulkan-validation/fixtures/public/examples/runtime_examples.ll'
PROFILE=SOURCE/'crates/metal2vulkan-validation/profiles/vk12-bare.json'


def main():
    # Recheck the parent-owned patch set and refresh the exact Cargo lock from
    # tracked inputs; the AIR validation is reproducible as a standalone gate.
    subprocess.run([sys.executable,ROOT/'scripts/build-native-metal-bridge.py','--backend','mali',
                    '--target','host','--fetch','--manifest-only'],check=True)
    pin=json.loads((ROOT/'scripts/data/gpu-metal-vulkan-inputs.json').read_text())['translator']['commit']
    actual=subprocess.check_output(['git','-C',SOURCE,'rev-parse','HEAD'],text=True).strip()
    if actual!=pin:raise SystemExit(f'translator pin mismatch: {actual}')
    if not (SOURCE/'Cargo.lock').is_file() or (SOURCE/'Cargo.lock').read_bytes()!=(ROOT/'scripts/data/metal2vulkan-cli.Cargo.lock').read_bytes():
        raise SystemExit('translator dependency lock differs from the recorded pins')
    if not FIXTURE.is_file() or not PROFILE.is_file():raise SystemExit('pinned AIR fixture/profile is missing')
    profile_data=json.loads(PROFILE.read_text())
    def find(obj,key):
        if isinstance(obj,dict):
            if key in obj:return obj[key]
            for value in obj.values():
                found=find(value,key)
                if found is not None:return found
        elif isinstance(obj,list):
            for value in obj:
                found=find(value,key)
                if found is not None:return found
        return None
    if find(profile_data,'uniformBufferStandardLayout') is not True:
        raise SystemExit('Pinned Vulkan profile does not require uniformBufferStandardLayout')
    OUT.mkdir(parents=True,exist_ok=True)
    spirv=OUT/'scale_and_offset.spv';meta=OUT/'scale_and_offset.json'
    manifest=SOURCE/'crates/metal2vulkan/Cargo.toml'
    env=dict(__import__('os').environ,CARGO_TARGET_DIR=str(OUT/'target'))
    command=['cargo','+1.98.0','run','--release','--locked','--manifest-path',manifest,'--features','cli','--',
        FIXTURE,spirv,'--profile',PROFILE,'--function','scale_and_offset','--emit-meta',meta]
    with (OUT/'translate.log').open('w') as log:
        result=subprocess.run([str(v) for v in command],env=env,stdout=log,stderr=subprocess.STDOUT,timeout=1800)
    if result.returncode:raise SystemExit((OUT/'translate.log').read_text()[-10000:])
    data=spirv.read_bytes()
    if len(data)<20 or struct.unpack_from('<I',data)[0]!=0x07230203:raise SystemExit('Metal AIR did not yield a SPIR-V module')
    validator=shutil.which('spirv-val')
    if not validator:raise SystemExit('spirv-val is required for the offline Metal AIR gate')
    subprocess.run([validator,'--target-env','vulkan1.2','--uniform-buffer-standard-layout',spirv],check=True,timeout=60)
    record={'schema':1,'translator_commit':pin,'translator_source_diff_sha256':hashlib.sha256(
        subprocess.check_output(['git','-C',SOURCE,'diff','HEAD','--'])).hexdigest(),
        'cargo_lock_sha256':hashlib.sha256((SOURCE/'Cargo.lock').read_bytes()).hexdigest(),
        'input_sha256':hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        'profile':'vk12-bare','profile_sha256':hashlib.sha256(PROFILE.read_bytes()).hexdigest(),
        'function':'scale_and_offset','spirv_bytes':len(data),'spirv_sha256':hashlib.sha256(data).hexdigest(),
        'spirv_magic':'07230203','spirv_val':'PASS vulkan1.2','hardware_exercised':False,
        'outputs':{spirv.name:hashlib.sha256(data).hexdigest(),meta.name:hashlib.sha256(meta.read_bytes()).hexdigest()}}
    (OUT/'report.json').write_text(json.dumps(record,indent=2,sort_keys=True)+'\n')
    print(f'PASS: pinned Metal AIR -> Vulkan 1.2 SPIR-V ({len(data)} bytes); hardware_exercised=false')


if __name__=='__main__':main()
