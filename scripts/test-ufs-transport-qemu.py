#!/usr/bin/env python3
"""Execute production portable UFS commands on QEMU UFS; no XNU/phone/cache qualification."""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    if out == root or root in out.parents:
        raise ValueError('QEMU output must be outside the source repository')
    out.mkdir(parents=True, exist_ok=False)
    cc = shutil.which('clang++') or shutil.which('c++')
    qemu = shutil.which('qemu-system-aarch64')
    if not cc or not qemu:
        raise RuntimeError('C++ compiler and QEMU UFS model required')
    core = root/'drivers/UfsTransport'
    library = out/'transport.so'
    sources = [core/'UfsTransport.cpp', core/'UfsTransport.h', root/'tests/ufs_transport_bridge.cpp', Path(__file__).resolve()]
    hashes={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    snapshot=out/'sources';snapshot.mkdir()
    for path in sources:
        data=path.read_bytes();assert hashlib.sha256(data).hexdigest()==hashes[str(path.relative_to(root))]
        (snapshot/path.name).write_bytes(data)
    compile_command=[cc, '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror', '-shared', '-fPIC', '-I', str(snapshot),
                     str(snapshot/sources[0].name), str(snapshot/sources[2].name), '-o', str(library)]
    subprocess.run(compile_command, check=True)
    lib = C.CDLL(str(library))
    callback_type = C.CFUNCTYPE(C.c_uint64, C.c_uint, C.c_uint64, C.c_uint64, C.c_void_p)
    lib.ufs_create.argtypes = [callback_type,C.c_uint64,C.POINTER(C.c_uint32)];lib.ufs_create.restype=C.c_void_p
    for name in ('ufs_init','ufs_poisoned','ufs_close','ufs_destroy'):
        getattr(lib,name).argtypes=[C.c_void_p]
    for name in ('ufs_probe','ufs_sync'):
        getattr(lib,name).argtypes=[C.c_void_p,C.c_uint8]
    for name in ('ufs_read','ufs_write'):
        getattr(lib,name).argtypes=[C.c_void_p,C.c_uint8,C.c_uint64,C.c_uint32,C.c_void_p]
    lib.ufs_geometry.argtypes=[C.c_void_p,C.c_uint8,C.c_uint];lib.ufs_geometry.restype=C.c_uint64
    lib.ufs_poisoned.restype=C.c_bool;lib.ufs_destroy.restype=C.c_bool
    pattern=bytes((i*17+23)&255 for i in range(131072))
    disk=bytearray(8*1024*1024);disk[8*4096:8*4096+len(pattern)]=pattern
    (out/'disk.raw').write_bytes(disk);(out/'blank.bin').write_bytes(bytes(4096))
    results=[]
    with tempfile.TemporaryDirectory(prefix='ufs-core-',dir='/tmp') as td:
        sock=Path(td)/'qtest.sock'
        command=[qemu,'-machine','virt,gic-version=3,highmem-ecam=off,acpi=on','-cpu','max','-accel','tcg','-m','128M',
                 '-bios',str(out/'blank.bin'),'-device','ufs,id=ufs0,bus=pcie.0,addr=1.0',
                 '-drive',f'if=none,id=disk,format=raw,file={out / "disk.raw"}',
                 '-device','ufs-lu,bus=ufs0,drive=disk,lun=2','-nodefaults','-display','none','-S',
                 '-qtest',f'unix:{sock},server=on,wait=off','-qtest-log','/dev/null']
        with (out/'qemu.log').open('w') as log:
            process=subprocess.Popen(command,stdout=log,stderr=log)
            try:
                deadline=time.monotonic()+5
                while not sock.exists():
                    if process.poll() is not None or time.monotonic()>deadline:raise RuntimeError('Qtest startup failed')
                    time.sleep(.01)
                connection=socket.socket(socket.AF_UNIX);connection.settimeout(3);connection.connect(str(sock))
                channel=connection.makefile('rwb',buffering=0)
                def qtest(command):
                    connection.sendall((command+'\n').encode())
                    while True:
                        reply=channel.readline().decode().strip()
                        if reply.startswith('IRQ '):continue
                        assert reply.startswith('OK'),(command,reply)
                        return reply[2:].strip()
                hc=0x10000000;pci=0x3f008000
                def read(off):return int(qtest(f'readl {hc+off:#x}'),0)
                def write(off,value):qtest(f'writel {hc+off:#x} {value:#x}')
                qtest(f'writel {pci+0x10:#x} {hc:#x}');qtest(f'writel {pci+0x14:#x} 0');qtest(f'writew {pci+4:#x} 6')
                write(0x34,1);write(0x90,0x16)
                assert read(0x30)&15==15
                write(0x20,read(0x20)) # Fixture consumes its completed startup event, before production ownership.
                errors=[];writes=[];responses=[];grant=False;physical=0x40010000;pending=False
                def io(op,address,value,ptr):
                    nonlocal pending
                    try:
                        if errors:return (1<<64)-1
                        if op==0:
                            result=read(address)
                            if address==0x58 and result==0 and pending:
                                trd=bytes.fromhex(qtest(f'read {physical:#x} 32').removeprefix('0x'))
                                if trd[8]!=15:
                                    responses.append({'request':writes[-1], 'trd':trd.hex(),
                                                      'response':qtest(f'read {physical+4096+512:#x} 512')})
                                    pending=False
                            return result
                        if op==1:
                            if address==0x58:
                                q=qtest(f'read {physical+4096:#x} 32');writes.append(q);pending=True
                            write(address,value)
                        elif op==2:
                            data=C.string_at(ptr,value)
                            for at in range(0,value,4096):
                                part=data[at:at+4096];qtest(f'write {address+at:#x} {len(part):#x} 0x{part.hex()}')
                        elif op==3:
                            data=b''.join(bytes.fromhex(qtest(f'read {address+at:#x} {min(4096,value-at):#x}').removeprefix('0x')) for at in range(0,value,4096))
                            assert len(data)==value;C.memmove(ptr,data,value)
                        elif op==4:return time.monotonic_ns()//1000
                        elif op==5:time.sleep(value/1000000)
                        elif op==6:return 1 # QEMU fixture explicitly configured legacy mode/exclusive ownership.
                        elif op==7:return int(grant and value>>32==2 and address>=64 and address<72 and (value&0xffffffff)<=72-address)
                        return 0
                    except BaseException as error:
                        errors.append(repr(error));return (1<<64)-1
                callback=callback_type(io);status=C.c_uint32();host=lib.ufs_create(callback,physical,C.byref(status))
                assert status.value==0,('queue',status.value,errors)
                init=lib.ufs_init(host);assert init==0,('initialize',init,errors,writes)
                probe=lib.ufs_probe(host,2)
                if probe:
                    (out/'failed-response.txt').write_text(qtest(f'read {physical+4096+512:#x} 512'))
                assert probe==0,('probe',probe,errors,writes)
                assert (lib.ufs_geometry(host,2,0),lib.ufs_geometry(host,2,1))==(2048,4096)
                for blocks in (1,32):
                    buffer=C.create_string_buffer(blocks*4096)
                    assert lib.ufs_read(host,2,8,blocks,buffer)==0 and buffer.raw==pattern[:blocks*4096]
                buffer=C.create_string_buffer(bytes([0x5a])*4096,4096);before=len(writes)
                assert lib.ufs_write(host,2,64,1,buffer)==3 and len(writes)==before
                grant=True
                for lba,blocks in ((63,1),(71,2),(72,1)):
                    assert lib.ufs_write(host,2,lba,blocks,C.create_string_buffer(blocks*4096))==3 and len(writes)==before
                assert lib.ufs_write(host,2,64,1,buffer)==0
                assert lib.ufs_sync(host,2)==0
                back=C.create_string_buffer(4096);assert lib.ufs_read(host,2,64,1,back)==0 and back.raw==buffer.raw
                assert not lib.ufs_poisoned(host) and not errors,errors
                lib.ufs_close(host);assert not lib.ufs_destroy(host)
                write(0x34,0);assert lib.ufs_destroy(host)
                assert len(responses)==len(writes)
                results.append({'lun':2,'block_bytes':4096,'capacity_blocks':2048,'read_4k_and_128k':True,'write_policy_before_submit':True,'actual_sync_and_readback':True,'hce_release_gate':True,'requests':writes,'completed_responses':responses})
            finally:
                process.terminate()
                try:process.wait(timeout=5)
                except subprocess.TimeoutExpired:process.kill();process.wait(timeout=5)
    after=(out/'disk.raw').read_bytes();changed=[i for i in range(2048) if disk[i*4096:(i+1)*4096]!=after[i*4096:(i+1)*4096]]
    assert changed==[64] and after[64*4096:65*4096]==bytes([0x5a])*4096
    assert hashes=={str(p.relative_to(root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources},'Source changed during qualification'
    assert hashes=={str(p.relative_to(root)):hashlib.sha256((snapshot/p.name).read_bytes()).hexdigest() for p in sources},'Compiled snapshot changed'
    report={'results':results,'only_changed_blocks':changed,'source_sha256':hashes,
            'compiled_library_sha256':hashlib.sha256(library.read_bytes()).hexdigest(),
            'compile_command':compile_command,'source_snapshot_compiled':True,'source_unchanged_during_run':True,
            'disk_after_sha256':hashlib.sha256(after).hexdigest(),'disk_before_sha256':hashlib.sha256(disk).hexdigest(),
            'qemu':subprocess.check_output([qemu,'--version'],text=True).splitlines()[0],
            'scope':'Production portable engine, QEMU UFS DMA/protocol only; host CPU via qtest. Not XNU/MT6991/cache-coherency/physical-media/persistence qualification.',
            'physical_ufs_qualified':False,'xnu_ufs_qualified':False,'persistence_qualified':False}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print('PASS portable UFS / actual QEMU model / LUN2 / bounded writes + sync + readback')


if __name__=='__main__':main()
