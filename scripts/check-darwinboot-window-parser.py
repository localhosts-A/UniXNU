#!/usr/bin/env python3
"""Exercise the actual loader ADT parser on the host; no UEFI allocation claim."""
import hashlib
import json
from pathlib import Path
import struct
import subprocess
ROOT=Path(__file__).resolve().parents[1]
out=ROOT/'build/cpu-desktop/window-parser';out.mkdir(parents=True,exist_ok=True)
p=ROOT/'drivers/DarwinBoot/DarwinBootPkg/Driver/DarwinBoot.c'
s=p.read_text();start=s.index('STATIC\nBOOLEAN\nAdtNode (');end=s.index('\nSTATIC\nBOOLEAN\nValidTrustCaches',start)
def node(name, properties=(), children=()):
 props=[('name',name.encode()+b'\0'),*properties]
 data=struct.pack('<II',len(props),len(children))
 for key,value in props:data+=struct.pack('<32sI',key.encode(),len(value))+value+bytes(-len(value)%4)
 return data+b''.join(children)
prelude='''#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include "WindowSize.h"
typedef uint8_t UINT8;typedef uint32_t UINT32;typedef uint64_t UINT64;
typedef size_t UINTN;typedef int BOOLEAN;typedef char CHAR8;
#define STATIC static
#define CONST const
#define TRUE 1
#define FALSE 0
#define MAX_UINTN SIZE_MAX
#define ALIGN_VALUE(a,b) (((a)+(b)-1)&~((b)-1))
#define AsciiStrCmp strcmp
#define CompareMem memcmp
#define DARWIN_KERNEL_SHA256_PROPERTY "darwinboot-kernel-sha256"
#define DARWIN_KERNEL_SHA256_BYTES 32
static const void *ScanMem8(const void *p,size_t n,int v){return memchr(p,v,n);}
static UINT32 U32(const void *p){UINT32 v;memcpy(&v,p,4);return v;}
static UINT64 U64(const void *p){UINT64 v;memcpy(&v,p,8);return v;}
'''
code=prelude+s[start:end]+'\nint main(void){\n'
checks=[]
for value in [0,0xfffffff,0x10000000,0xe0000000,0x100000000,0x130000000,0x200000000,0x200200000,0x130001000,0xffffffffffffffff]:
 for variant in ['chosen','duplicate','misplaced']:
  props=[('darwinboot-memory-size',struct.pack('<Q',value))]
  if variant=='duplicate':props*=2
  tree=node('device-tree',props,[node('chosen')]) if variant=='misplaced' else node('device-tree',children=[node('chosen',props)])
  yes=0x10000000<=value<=0x200000000 and value%0x200000==0 and variant=='chosen'
  code+='{unsigned char b[]={'+','.join(str(v) for v in tree)+'};UINTN at=0,r=MAX_UINTN,t=MAX_UINTN,a=MAX_UINTN,h=MAX_UINTN;UINT64 w=0;BOOLEAN packed=FALSE;'
  code+=f'assert(AdtNode(b,sizeof(b),&at,0,3,&r,&t,&a,&w,&packed,&h)=={int(yes)});'
  if yes:code+=f'assert(w=={value}ULL && at==sizeof(b));'
  code+='}\n';checks.append({'size':hex(value),'placement':variant,'accepted':yes})
for variant in ['packed','dense','mixed','duplicate','outside']:
 props=[('PackedRAMDisk',bytes(16))]
 if variant=='dense':props=[('RAMDisk',bytes(16))]
 if variant=='mixed':props.append(('RAMDisk',bytes(16)))
 if variant=='duplicate':props*=2
 tree=node('device-tree',children=[node('chosen',children=[node('memory-map',props)])])
 if variant=='outside':tree=node('device-tree',children=[node('chosen',props)])
 yes=variant not in ('mixed','duplicate')
 code+='{unsigned char b[]={'+','.join(str(v) for v in tree)+'};UINTN at=0,r=MAX_UINTN,t=MAX_UINTN,a=MAX_UINTN,h=MAX_UINTN;UINT64 w=0;BOOLEAN packed=FALSE;'
 code+=f'assert(AdtNode(b,sizeof(b),&at,0,3,&r,&t,&a,&w,&packed,&h)=={int(yes)});'
 if variant=='packed':code+='assert(packed && r!=MAX_UINTN);'
 if variant=='dense':code+='assert(!packed && r!=MAX_UINTN);'
 if variant=='outside':code+='assert(packed && r==MAX_UINTN);' # Load rejects this before allocation.
 code+='}\n';checks.append({'ramdisk_property':variant,'parser_accepts':yes,'loader_can_use':variant in ('packed','dense')})
code+='return 0;}\n';(out/'parser.c').write_text(code)
subprocess.run(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(p.parent),str(out/'parser.c'),'-o',str(out/'parser')],check=True,timeout=60)
subprocess.run([str(out/'parser')],check=True,timeout=30)
(out/'report.json').write_text(json.dumps({'source_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'checks':checks,'hardware_observed':False,'uefi_allocation_exercised':False},indent=2)+'\n')
print('PASS: 35 actual ADT-parser memory and exclusive packed/dense handoff cases; no UEFI allocation exercised')
