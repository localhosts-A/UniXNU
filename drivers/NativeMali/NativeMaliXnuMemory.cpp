// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliXnuMemory.h"
#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <vm/memory_types.h>
#include <mach/vm_param.h>
extern "C" unsigned int pmap_cache_attributes(ppnum_t);
extern "C" void pmap_set_cache_attributes(ppnum_t,unsigned int);
extern "C" uint64_t gPhysBase, gPhysSize;
namespace native_mali {
FirmwareMemoryOps XnuMemory::memoryOps(XnuMemoryOwner *owner) { return {owner,alloc,free,publish,acquire}; }
TableOps XnuMemory::tableOps() { return {this,tableAlloc,tableFree,tableResolve,tablePublish}; }
void XnuMemory::delay(void *,uint32_t us) { if(us>=1000)IOSleep((us+999)/1000);else IODelay(us); }
uint64_t XnuMemory::now(void *) { uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns; }
bool XnuMemory::holdCache(uint64_t pa) {
    auto **link=&cache_[(pa>>12)&1023];
    for(auto *p=*link;p;p=p->next)if(p->physical==pa){if(p->refs==0xffffffff)return false;++p->refs;return true;}
    auto *p=IONewZero(CachePage,1);if(!p)return false;
    p->physical=pa;p->previous=pmap_cache_attributes(ppnum_t(pa>>12))&VM_WIMG_MASK;p->refs=1;p->next=*link;*link=p;
    pmap_set_cache_attributes(ppnum_t(pa>>12),VM_WIMG_WCOMB);return true;
}
void XnuMemory::dropCache(uint64_t pa) {
    auto **link=&cache_[(pa>>12)&1023];
    while(*link){auto *p=*link;if(p->physical==pa){
        if(--p->refs==0){pmap_set_cache_attributes(ppnum_t(pa>>12),p->previous);*link=p->next;IODelete(p,CachePage,1);}return;
    }link=&p->next;}
}
void XnuMemory::maintain(Allocation *a,size_t offset,size_t bytes,bool invalid) {
    if(!a||!bytes||offset>a->count*4096||bytes>a->count*4096-offset)return;
    if(a->uncached){
        for(size_t i=offset/4096;i<(offset+bytes+4095)/4096;++i)
            if((pmap_cache_attributes(ppnum_t(a->pages[i]>>12))&VM_WIMG_MASK)!=VM_WIMG_WCOMB)
                pmap_set_cache_attributes(ppnum_t(a->pages[i]>>12),VM_WIMG_WCOMB);
        __asm__ volatile("dsb sy":::"memory");return;
    }
    // As in the existing MIT NativeAdreno CPU cache path, maintenance is
    // explicit on these noncoherent ARM pages and bounded per preemption hold.
    for(size_t page=offset/4096;page<(offset+bytes+4095)/4096;++page){
        disable_preemption();uint64_t ctr;__asm__ volatile("mrs %0, ctr_el0":"=r"(ctr));
        const size_t line=size_t(4)<<((ctr>>16)&15);
        __asm__ volatile("dsb sy":::"memory");
        for(uintptr_t address=uintptr_t(a->cpu)+page*4096,end=address+4096;address<end;address+=line){
            if(invalid)__asm__ volatile("dc ivac, %0"::"r"(address):"memory");
            else __asm__ volatile("dc cvac, %0"::"r"(address):"memory");
        }
        __asm__ volatile("dsb sy":::"memory");enable_preemption();
    }
}
bool XnuMemory::allocate(task_t task,uint64_t host,size_t count,bool uncached,bool table,FirmwareAllocation *out) {
    if(!out||!count||count>65536||page_size!=4096||gPhysSize<4096||bytes_>1024ull*1024*1024-count*4096)return false;
    *out={};auto *a=IONewZero(Allocation,1);if(!a)return false;
    a->count=count;a->uncached=uncached;a->table=table;a->pages=IONewZero(uint64_t,count);
    if(!a->pages){IODelete(a,Allocation,1);return false;}
    IOOptionBits options=IOOptionBits(kIODirectionInOut)|IOOptionBits(kIOMemoryMapperNone);
    if(host){
        a->descriptor=IOMemoryDescriptor::withAddressRange(host,count*4096,options|kIOMemoryPersistent,task);
        if(!a->descriptor)a->descriptor=IOMemoryDescriptor::withAddressRange(host,count*4096,options,task);
    }else a->descriptor=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,count*4096,0x0000fffffffff000ull);
    bool ok=a->descriptor&&a->descriptor->getLength()==count*4096&&a->descriptor->prepare()==kIOReturnSuccess;
    if(ok){a->prepared=true;
        if(host){a->map=a->descriptor->map(kIOMapAnywhere);if(a->map)a->cpu=reinterpret_cast<uint8_t *>(a->map->getVirtualAddress());}
        else a->cpu=static_cast<uint8_t *>(static_cast<IOBufferMemoryDescriptor *>(a->descriptor)->getBytesNoCopy());
        ok=a->cpu&&!(uintptr_t(a->cpu)&4095);
    }
    for(size_t i=0;ok&&i<count;++i){IOByteCount length=0;
        const auto pa=a->descriptor->getPhysicalSegment(i*4096,&length,kIOMemoryMapperNone);
        ok=pa&&!(pa&4095)&&length>=4096&&pa>=gPhysBase&&pa-gPhysBase<=gPhysSize-4096;
        if(ok)a->pages[i]=pa;
    }
    // Changing page attributes cleans old cache lines before noncacheable use.
    for(size_t i=0;ok&&uncached&&i<count;++i){ok=holdCache(a->pages[i]);if(ok)++a->cachePages;}
    a->next=allocations_;allocations_=a;bytes_+=count*4096;
    *out={a->cpu,a->pages,count,a};
    if(!ok){release(*out);*out={};return false;}
    return true;
}
void XnuMemory::release(const FirmwareAllocation &value) {
    auto **link=&allocations_;while(*link&&*link!=value.handle)link=&(*link)->next;
    if(!*link)return;auto *a=*link;
    // GPU use has already quiesced. Restore this allocation's cache references
    // before unwiring; overlapping live imports keep their own WC reference.
    // A descriptor whose completion fails remains owned by this adapter.
    for(size_t i=0;i<a->cachePages;++i)dropCache(a->pages[i]);
    a->cachePages=0;
    if(a->prepared&&a->descriptor->complete()!=kIOReturnSuccess)return;
    a->prepared=false;
    if(a->map){a->map->release();a->map=nullptr;}
    *link=a->next;bytes_-=a->count*4096;if(a->descriptor)a->descriptor->release();IODelete(a->pages,uint64_t,a->count);IODelete(a,Allocation,1);
}
bool XnuMemory::alloc(void *cookie,size_t pages,bool uncached,FirmwareAllocation *out) {
    auto *owner=static_cast<XnuMemoryOwner *>(cookie);return owner&&owner->memory&&owner->memory->allocate(kernel_task,0,pages,uncached,false,out);
}
bool XnuMemory::importHost(void *cookie,uint64_t address,size_t pages,FirmwareAllocation *out) {
    auto *owner=static_cast<XnuMemoryOwner *>(cookie);return owner&&owner->task&&address&&!(address&4095)&&
        address<=UINT64_MAX-pages*4096&&owner->memory->allocate(owner->task,address,pages,true,false,out);
}
void XnuMemory::free(void *cookie,const FirmwareAllocation &a) {
    auto *owner=static_cast<XnuMemoryOwner *>(cookie);if(owner&&owner->memory)owner->memory->release(a);
}
void XnuMemory::publish(void *,const FirmwareAllocation &a,size_t offset,size_t bytes) { maintain(static_cast<Allocation *>(a.handle),offset,bytes,false); }
void XnuMemory::acquire(void *,const FirmwareAllocation &a,size_t offset,size_t bytes) { maintain(static_cast<Allocation *>(a.handle),offset,bytes,true); }
bool XnuMemory::tableAlloc(void *cookie,TablePage *out) {
    auto *self=static_cast<XnuMemory *>(cookie);FirmwareAllocation a{};
    if(!self->allocate(kernel_task,0,1,false,true,&a))return false;
    *out={a.physicalPages[0],reinterpret_cast<uint64_t *>(a.cpu),a.handle};return true;
}
void XnuMemory::tableFree(void *cookie,const TablePage &p) {
    auto *a=static_cast<Allocation *>(p.allocation);static_cast<XnuMemory *>(cookie)->release({a->cpu,a->pages,a->count,a});
}
bool XnuMemory::tableResolve(void *cookie,uint64_t pa,TablePage *out) {
    auto *self=static_cast<XnuMemory *>(cookie);
    for(auto *a=self->allocations_;a;a=a->next)if(a->table&&a->count==1&&a->pages[0]==pa){
        *out={pa,reinterpret_cast<uint64_t *>(a->cpu),a};return true;}return false;
}
void XnuMemory::tablePublish(void *,const TablePage &p,size_t offset,size_t bytes) { maintain(static_cast<Allocation *>(p.allocation),offset,bytes,false); }
IOMemoryDescriptor *XnuMemory::descriptor(const FirmwareAllocation &value) const {
    for(auto *a=allocations_;a;a=a->next)if(a==value.handle){a->descriptor->retain();return a->descriptor;}return nullptr;
}
} // namespace native_mali
