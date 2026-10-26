#!/usr/bin/env python3
"""Run actual arch13 compiler tools on an owned kernel; no GPU or loader."""
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/mali-offline-compiler'


def main():
    OUT.mkdir(parents=True,exist_ok=True)
    tools=ROOT/'build/panvk-host-tools/bin'
    source=ROOT/'tests/native-mali/compiler_scale.cl'
    spv=OUT/'scale.opencl.spv'
    header=OUT/'scale-v13.h'
    c=OUT/'scale-v13.c'
    commands=[[tools/'mesa_clc',source,'-o',spv,'--','-cl-std=cl2.0','-D__OPENCL_VERSION__=200'],
              [tools/'panfrost_compile','native_mali_shaders','13',spv,header,c]]
    for i,command in enumerate(commands):
        with (OUT/f'compile-{i}.log').open('w') as log:
            result=subprocess.run([str(x) for x in command],stdout=log,stderr=subprocess.STDOUT,timeout=120)
        if result.returncode:raise SystemExit((OUT/f'compile-{i}.log').read_text()[-10000:])
    if int.from_bytes(spv.read_bytes()[:4],'little')!=0x07230203:raise SystemExit('No actual SPIR-V module')
    text=c.read_text()
    if 'native_mali_scale_0_default' not in text:raise SystemExit('Compiler did not emit the fixture entry point')
    words=re.findall(r'0x[0-9a-fA-F]{8}',text)
    if len(words)<32 or not any(int(w,16) for w in words):raise SystemExit('Empty compiler output')
    report={'schema':1,'arch':13,'gpu_product':'Mali-G925 / PAN_PROD_ID(13,8,0)',
            'input_environment':'OpenCL kernel SPIR-V','entry':'native_mali_scale','compiled_archive_words':len(words),
            'hardware_exercised':False,'shader_executed':False,
            'tool_profile':json.loads((tools.parent/'report.json').read_text()),
            'sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in [source,spv,header,c]}}
    (OUT/'report.json').write_text(json.dumps(report,indent=2,sort_keys=True)+'\n')
    print(f'PASS: actual Mesa OpenCL -> SPIR-V -> arch13 compiler; {len(words)} archive words; hardware_exercised=false')


if __name__=='__main__':main()
