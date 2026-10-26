// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliSession.h"
namespace native_mali {
uint32_t Session::initialize(Boot &boot, Scheduler &scheduler, uint8_t as, const SessionOps &ops, size_t budget) {
    if (boot_ || !boot.ready()) return NML_STATE;
    if (!as || as >= 16 || !(boot.properties()->addressSpaces & (1u << as)) ||
        !ops.memory.allocate || !ops.memory.release || !ops.memory.publish || !ops.memory.acquire || !ops.delayUs || !ops.nowNs ||
        budget < 4096 || budget > 1024ull * 1024 * 1024) return NML_INVALID;
    boot_ = &boot; scheduler_ = &scheduler; ops_ = ops; as_ = as; budget_ = budget;
    if (mmu_.initialize(ops.tables,boot.properties()->physicalBits,boot.properties()->virtualBits) != MmuResult::Ok) {
        boot_ = nullptr; scheduler_ = nullptr; return NML_NO_MEMORY;
    }
    configured_ = true; // A failed AS write can still have reached the GPU.
    if (boot.hardware().configureAddressSpace(as,mmu_.rootPhysical(),timeout_) != HardwareResult::Ok) return fail();
    return NML_OK;
}
Session::Buffer *Session::buffer(uint32_t handle) {
    if (!handle) return nullptr;
    for (auto &b : buffers_) if (b.handle == handle) return &b;
    return nullptr;
}
Session::Buffer *Session::enclosing(uint64_t address, size_t bytes, size_t *offset) {
    if (!bytes) return nullptr;
    for (auto &b : buffers_) if (b.handle && !b.parent && b.mapped && address >= b.va &&
        address - b.va < b.pages * 4096 && bytes <= b.pages * 4096 - (address - b.va)) {
        if (offset) *offset = size_t(address - b.va); return &b;
    }
    return nullptr;
}
uint64_t Session::freeVa(size_t bytes, bool executable, bool within4G) const {
    uint64_t start = executable ? 0x10000000ull : 0x100000000ull;
    const uint64_t limit = executable ? 0xf0000000ull : (uint64_t(1) << boot_->properties()->virtualBits);
    for (size_t attempt = 0; attempt <= MaxBuffers; ++attempt) {
        if (within4G && (start >> 32) != ((start + bytes - 1) >> 32)) start = (start + 0xffffffff) & ~uint64_t(0xffffffff);
        if (start >= limit || bytes > limit - start) return 0;
        bool conflict = false;
        for (const auto &b : buffers_) if (b.handle && b.va && start < b.va + b.pages * 4096 * b.repeats && b.va < start + bytes) {
            start = b.va + b.pages * 4096 * b.repeats; conflict = true; break;
        }
        if (!conflict) return start;
    }
    return 0;
}
uint32_t Session::beginMutation() {
    if (poisoned_ || !boot_ || !boot_->ready() || scheduler_->error(as_)) return NML_DEVICE_LOST;
    return scheduler_->suspend(as_) == QueueResult::Ok ? NML_OK : fail();
}
uint32_t Session::endMutation() {
    if (boot_->hardware().synchronizeAddressSpace(as_,timeout_) != HardwareResult::Ok ||
        scheduler_->resume(as_) != QueueResult::Ok) return fail();
    return NML_OK;
}
uint32_t Session::allocate(size_t bytes, uint32_t flags, uint64_t host, Buffer **out, bool internal) {
    *out = nullptr;
    if (!bytes || (bytes & 4095) || bytes > budget_ || bytes_ > budget_ - bytes || flags & ~3u ||
        (host && (host & 4095)) || serial_ >= 0x3fffffff) return NML_INVALID;
    if (host && !ops_.importHost) return NML_UNSUPPORTED;
    Buffer *slot = nullptr; for (auto &b : buffers_) if (!b.handle) { slot = &b; break; }
    if (!slot) return NML_NO_MEMORY;
    const uint64_t va = freeVa(bytes,flags & NML_BUFFER_EXECUTABLE); if (!va) return NML_NO_MEMORY;
    const auto mutation = beginMutation(); if (mutation != NML_OK) return mutation;
    FirmwareAllocation memory{}; const size_t pages = bytes / 4096;
    const bool allocated = host ? ops_.importHost(ops_.memory.cookie,host,pages,&memory) :
        ops_.memory.allocate(ops_.memory.cookie,pages,true,&memory);
    if (!allocated || !memory.cpu || !memory.physicalPages || memory.pageCount != pages || !memory.handle) {
        if (memory.handle) ops_.memory.release(ops_.memory.cookie,memory);
        return endMutation() == NML_OK ? NML_NO_MEMORY : NML_DEVICE_LOST;
    }
    if (!host) for (size_t i = 0; i < bytes; ++i) memory.cpu[i] = 0;
    ops_.memory.publish(ops_.memory.cookie,memory,0,bytes);
    PageAttributes attr{}; attr.memoryIndex = flags & NML_BUFFER_UNCACHED ? 5 : 0;
    attr.writable = true; attr.executable = flags & NML_BUFFER_EXECUTABLE;
    const auto map = mmu_.map(va,memory.physicalPages,pages,attr);
    if (map != MmuResult::Ok) {
        ops_.memory.release(ops_.memory.cookie,memory);
        return endMutation() == NML_OK ? NML_NO_MEMORY : NML_DEVICE_LOST;
    }
    *slot = {}; slot->handle = ++serial_; slot->flags = flags; slot->va = va; slot->pages = pages;
    slot->allocation = memory; slot->mapped = true; slot->internal = internal; bytes_ += bytes; *out = slot;
    return endMutation();
}
uint32_t Session::release(uint32_t handle, bool internal) {
    auto *b = buffer(handle); if (!b || (b->internal && !internal)) return NML_INVALID;
    if (b->refs) return NML_BUSY;
    const auto mutation = beginMutation(); if (mutation != NML_OK) return mutation;
    if (b->mapped) {
        if (mmu_.unmap(b->va,b->pages * b->repeats) != MmuResult::Ok) return fail();
        b->mapped = false;
    }
    // Reclaim physical backing only after all stale translations/cache traffic
    // has drained. Failure deliberately leaves this handle and allocation live.
    if (endMutation() != NML_OK) return NML_DEVICE_LOST;
    if (b->parent) { auto *parent = buffer(b->parent); if (!parent || !parent->refs) return fail(); --parent->refs; }
    else { bytes_ -= b->pages * 4096; ops_.memory.release(ops_.memory.cookie,b->allocation); }
    *b = {}; return NML_OK;
}
uint32_t Session::alias(const nml_request &r, nml_response *out) {
    auto *parent = buffer(r.parent);
    if (!parent || parent->parent || parent->internal || !r.length || (r.length & 4095) || (r.offset & 4095) ||
        r.offset >= parent->pages * 4096 || r.length > parent->pages * 4096 - r.offset || !r.value || r.value > 4 ||
        serial_ >= 0x3fffffff) return NML_INVALID;
    Buffer *slot = nullptr; for (auto &b : buffers_) if (!b.handle) { slot = &b; break; }
    const size_t pages = r.length / 4096, repeats = r.value;
    const auto va = freeVa(pages * 4096 * repeats,false,true); if (!slot || !va) return NML_NO_MEMORY;
    const auto mutation = beginMutation(); if (mutation != NML_OK) return mutation;
    PageAttributes attr{}; attr.writable = true; attr.memoryIndex = parent->flags & NML_BUFFER_UNCACHED ? 5 : 0;
    size_t mapped = 0;
    for (; mapped < repeats; ++mapped) if (mmu_.map(va + mapped * r.length,
        parent->allocation.physicalPages + r.offset / 4096,pages,attr) != MmuResult::Ok) break;
    if (mapped != repeats) {
        if (mapped && mmu_.unmap(va,mapped * pages) != MmuResult::Ok) return fail();
        return endMutation() == NML_OK ? NML_NO_MEMORY : NML_DEVICE_LOST;
    }
    *slot = {}; slot->handle = ++serial_; slot->parent = parent->handle; slot->va = va; slot->pages = pages;
    slot->repeats = repeats; slot->offsetPages = r.offset / 4096; slot->mapped = true; ++parent->refs;
    out->object = slot->handle; out->address = va; out->length = r.length * repeats;
    return endMutation();
}
bool Session::bufferBacking(uint32_t handle, FirmwareAllocation *allocation, size_t *bytes) {
    auto *b = buffer(handle); if (!b || b->parent || b->internal || !allocation || !bytes) return false;
    *allocation = b->allocation; *bytes = b->pages * 4096; return true;
}
bool Session::queueBacking(uint32_t group, uint32_t stream, QueueView *view) {
    return boot_ && scheduler_->owns(group,as_) && scheduler_->view(group,stream,view) == QueueResult::Ok;
}
void Session::info(nml_response *out) {
    const auto *p = boot_->properties(); auto *w = out->words;
    w[NML_INFO_GPU_ID]=p->gpuId;w[NML_INFO_L2_FEATURES]=p->l2Features;w[NML_INFO_CORE_FEATURES]=p->coreFeatures;
    w[NML_INFO_TILER_FEATURES]=p->tilerFeatures;w[NML_INFO_MEM_FEATURES]=p->memFeatures;w[NML_INFO_MMU_FEATURES]=p->mmuFeatures;
    w[NML_INFO_ADDRESS_SPACES]=p->addressSpaces;w[NML_INFO_SHADER_LO]=p->shaders;w[NML_INFO_SHADER_HI]=p->shaders>>32;
    w[NML_INFO_TILER_LO]=p->tilers;w[NML_INFO_TILER_HI]=p->tilers>>32;w[NML_INFO_L2_LO]=p->l2Slices;w[NML_INFO_L2_HI]=p->l2Slices>>32;
    for(size_t i=0;i<4;++i)w[NML_INFO_TEXTURE_0+i]=p->textureFeatures[i];
    w[NML_INFO_MAX_THREADS]=p->maxThreads;w[NML_INFO_MAX_WORKGROUP]=p->maxWorkgroup;w[NML_INFO_THREAD_FEATURES]=p->threadFeatures;
    w[NML_INFO_TLS_ALLOC]=p->tlsAlloc;w[NML_INFO_CSF_VERSION]=boot_->interface().version();w[NML_INFO_CSF_FEATURES]=boot_->interface().features();
    w[NML_INFO_GROUP_COUNT]=scheduler_->capacity();w[NML_INFO_STREAM_COUNT]=Scheduler::MaxStreams;
    w[NML_INFO_STREAM_FEATURES]=boot_->interface().group(0)->streams[0].features;
    w[NML_INFO_CAPABILITIES]=NML_CAP_TILER_HEAP|NML_CAP_VALUE_WAIT|NML_CAP_RESIDENT_GROUPS|(ops_.importHost?NML_CAP_HOST_IMPORT:0);
    w[NML_INFO_VA_BITS]=p->virtualBits;w[NML_INFO_PA_BITS]=p->physicalBits;
    out->address=0x100000000ull;out->length=(uint64_t(1)<<p->virtualBits)-out->address;
}
uint32_t Session::heapCreate(const nml_request &r, nml_response *out) {
    // length=chunk bytes, value=initial count, offset=max count, address=target.
    if (!r.length || (r.length & 4095) || r.length > 0xfff000 || !r.value || r.value > MaxChunks ||
        r.offset < r.value || r.offset > MaxChunks || !r.address || r.address > 65535) return NML_INVALID;
    Heap *heap = nullptr; for (auto &h : heaps_) if (!h.handle) { heap = &h; break; }
    if (!heap) return NML_NO_MEMORY;
    Buffer *context = nullptr; auto status=allocate(4096,NML_BUFFER_UNCACHED,0,&context,true);
    if (status != NML_OK) return status;
    *heap={};heap->handle=context->handle;heap->context=context->handle;heap->chunkBytes=r.length;
    heap->maximum=r.offset;heap->target=r.address;
    for (size_t i=0;i<r.value;++i) {
        Buffer *chunk=nullptr;status=allocate(heap->chunkBytes,0,0,&chunk,true);
        if(status!=NML_OK){if(!poisoned_)heapDestroy(heap->handle);return status;}
        if(i){auto *prev=buffer(heap->chunks[i-1]);const uint64_t packed=chunk->va|(heap->chunkBytes>>12);
            __atomic_store_n(reinterpret_cast<uint64_t *>(prev->allocation.cpu),packed,__ATOMIC_RELEASE);
            ops_.memory.publish(ops_.memory.cookie,prev->allocation,0,64);}
        heap->chunks[heap->count++]=chunk->handle;
    }
    out->object=heap->handle;out->address=context->va;out->value=buffer(heap->chunks[0])->va;return NML_OK;
}
uint32_t Session::heapDestroy(uint32_t handle) {
    Heap *heap=nullptr;for(auto &h:heaps_)if(h.handle==handle&&handle){heap=&h;break;}
    if(!heap)return NML_INVALID;
    while(heap->count){const auto r=release(heap->chunks[heap->count-1],true);if(r!=NML_OK)return r;--heap->count;}
    const auto r=release(heap->context,true);if(r==NML_OK)*heap={};return r;
}
bool Session::oom(uint64_t address, uint32_t vtStart, uint32_t vtEnd, uint32_t fragEnd, uint64_t *chunk) {
    if(!chunk||poisoned_)return false;*chunk=0;
    const uint32_t inFlight=vtStart-fragEnd,pending=vtEnd-fragEnd;
    if(!inFlight||pending>inFlight)return false;
    for(auto &h:heaps_)if(h.handle&&buffer(h.context)->va==address){
        if(inFlight>h.target||(h.count>=h.maximum&&pending))return true;
        if(h.count>=h.maximum)return false;
        Buffer *b=nullptr;if(allocate(h.chunkBytes,0,0,&b,true)!=NML_OK)return false;
        h.chunks[h.count++]=b->handle;*chunk=b->va|(h.chunkBytes>>12);return true;
    }
    return false;
}
uint32_t Session::call(const nml_request &r, nml_response *out) {
    if(!out)return NML_INVALID;*out={};out->version=NML_ABI_VERSION;out->size=sizeof(*out);
    if(r.version!=NML_ABI_VERSION||r.size!=sizeof(r)||r.operation>NML_BUFFER_SYNC)return NML_INVALID;
    for(auto value:r.reserved)if(value)return NML_INVALID;
    if(!boot_||!configured_||!boot_->ready()||poisoned_)return NML_DEVICE_LOST;
    if(r.operation!=NML_BUFFER_ALLOC&&r.operation!=NML_BUFFER_IMPORT&&r.flags)return NML_INVALID;
    if(r.operation==NML_GET_INFO){info(out);return NML_OK;}
    if(r.operation==NML_BUFFER_ALLOC||r.operation==NML_BUFFER_IMPORT){
        if(r.length>SIZE_MAX||(!r.address&&r.operation==NML_BUFFER_IMPORT))return NML_INVALID;
        Buffer *b=nullptr;const auto s=allocate(size_t(r.length),r.flags,r.operation==NML_BUFFER_IMPORT?r.address:0,&b);
        if(b){out->object=b->handle;out->address=b->va;out->length=b->pages*4096;}return s;
    }
    if(r.operation==NML_BUFFER_FREE)return release(r.object);
    if(r.operation==NML_ALIAS_CREATE)return alias(r,out);
    if(r.operation==NML_GROUP_CREATE){if(r.value>Scheduler::MaxStreams)return NML_INVALID;uint32_t h=0;const auto q=scheduler_->create(as_,uint32_t(r.value),&h);
        out->object=h;return q==QueueResult::Ok?NML_OK:q==QueueResult::Capacity?NML_NO_MEMORY:NML_INVALID;}
    if(r.operation==NML_GROUP_DESTROY){
        if(!scheduler_->owns(r.object,as_))return NML_INVALID;
        if(scheduler_->destroy(r.object)!=QueueResult::Ok)return fail();
        for(auto &b:bindings_)if(b.group==r.object){auto *bo=buffer(b.buffer);if(bo&&bo->refs)--bo->refs;b={};}return NML_OK;
    }
    if(r.operation==NML_QUEUE_BIND){
        auto *bo=buffer(r.parent);if(!bo||bo->parent||bo->internal||!scheduler_->owns(r.object,as_)||r.offset||
            r.length!=bo->pages*4096||r.value>=Scheduler::MaxStreams)return NML_INVALID;
        Binding *binding=nullptr;for(auto &b:bindings_)if(!b.group){binding=&b;break;}if(!binding)return NML_NO_MEMORY;
        QueueView view{};const auto q=scheduler_->bind(r.object,uint32_t(r.value),bo->va,uint32_t(r.length),&view);
        // Even an unsuccessful published bind may DMA from the ring.
        if(q!=QueueResult::Invalid&&q!=QueueResult::State&&q!=QueueResult::Busy){*binding={r.object,uint32_t(r.value),bo->handle};++bo->refs;}
        if(q!=QueueResult::Ok)return q==QueueResult::Invalid?NML_INVALID:fail();return NML_OK;
    }
    if(r.operation==NML_QUEUE_KICK){
        if(!scheduler_->owns(r.object,as_)||r.offset>=Scheduler::MaxStreams)return NML_INVALID;
        const auto q=scheduler_->kick(r.object,uint32_t(r.offset),r.value);
        return q==QueueResult::Ok?NML_OK:q==QueueResult::Invalid?NML_INVALID:fail();
    }
    if(r.operation==NML_QUEUE_UNBIND){
        if(!scheduler_->owns(r.object,as_)||r.value>=Scheduler::MaxStreams)return NML_INVALID;
        if(scheduler_->unbind(r.object,uint32_t(r.value))!=QueueResult::Ok)return fail();
        for(auto &b:bindings_)if(b.group==r.object&&b.stream==r.value){auto *bo=buffer(b.buffer);if(bo&&bo->refs)--bo->refs;b={};}return NML_OK;
    }
    if(r.operation==NML_HEAP_CREATE)return heapCreate(r,out);
    if(r.operation==NML_HEAP_DESTROY)return heapDestroy(r.object);
    if(r.operation==NML_TIMESTAMP)return boot_->hardware().timestamp(&out->value)==HardwareResult::Ok?NML_OK:NML_DEVICE_LOST;
    if(r.operation==NML_FLUSH_ID){uint32_t value=0;if(boot_->hardware().latestFlush(&value)!=HardwareResult::Ok)return fail();out->value=value;return NML_OK;}
    if(r.operation==NML_EVENT_SIGNAL){
        for(auto &b:buffers_)if(b.handle&&!b.parent)ops_.memory.publish(ops_.memory.cookie,b.allocation,0,b.pages*4096);
        if(boot_->hardware().synchronizeMemory(timeout_)!=HardwareResult::Ok||scheduler_->signal(as_)!=QueueResult::Ok)return fail();return NML_OK;
    }
    if(r.operation==NML_BUFFER_SYNC){auto *b=buffer(r.object);if(!b||b->parent||r.offset>b->pages*4096||r.length>b->pages*4096-r.offset)return NML_INVALID;
        ops_.memory.publish(ops_.memory.cookie,b->allocation,r.offset,r.length);
        if(boot_->hardware().synchronizeMemory(timeout_)!=HardwareResult::Ok)return fail();
        ops_.memory.acquire(ops_.memory.cookie,b->allocation,r.offset,r.length);return NML_OK;}
    if(r.operation==NML_POLL||r.operation==NML_WAIT_VALUE){
        if(r.timeout_ns>50000000)return NML_INVALID;
        size_t offset=0;auto *b=r.operation==NML_WAIT_VALUE?enclosing(r.address,8,&offset):nullptr;
        if(r.operation==NML_WAIT_VALUE&&(!b||(r.address&15)))return NML_INVALID;
        const uint64_t start=ops_.nowNs(ops_.memory.cookie);
        for(;;){
            if(scheduler_->poll()!=QueueResult::Ok||scheduler_->error(as_))return fail();
            if(b){
                if(!(b->flags&NML_BUFFER_UNCACHED)&&boot_->hardware().synchronizeMemory(timeout_)!=HardwareResult::Ok)return fail();
                ops_.memory.acquire(ops_.memory.cookie,b->allocation,offset,8);
                out->value=__atomic_load_n(reinterpret_cast<uint64_t *>(b->allocation.cpu+offset),__ATOMIC_ACQUIRE);
                // The CQS word can be GPU-uncached while payload BOs are
                // cached. Drain their GPU writes before returning a host fence.
                if(out->value>r.value)return boot_->hardware().synchronizeMemory(timeout_)==HardwareResult::Ok?NML_OK:fail();
            }else return NML_OK;
            if(ops_.nowNs(ops_.memory.cookie)-start>=r.timeout_ns)return NML_TIMEOUT;
            ops_.delayUs(ops_.memory.cookie,1000);
        }
    }
    return NML_UNSUPPORTED;
}
uint32_t Session::close(bool quiesced) {
    if(!boot_)return NML_OK;
    if(quiesced&&!boot_->hardware().quiesced())return NML_STATE;
    if(!quiesced){
        for(auto &b:bindings_)if(b.group){const auto group=b.group;
            if(scheduler_->destroy(group)!=QueueResult::Ok)return fail();
            for(auto &other:bindings_)if(other.group==group)other={};}
        // Empty groups still belong to this AS; release those too.
        if(scheduler_->destroyContext(as_)!=QueueResult::Ok)return fail();
        if(configured_&&boot_->hardware().disableAddressSpace(as_,timeout_)!=HardwareResult::Ok)return fail();
    }
    configured_=false;
    if(mmu_.destroy(true)!=MmuResult::Ok)return fail();
    for(auto &b:buffers_)if(b.handle){if(!b.parent)ops_.memory.release(ops_.memory.cookie,b.allocation);b={};}
    for(auto &h:heaps_)h={};for(auto &b:bindings_)b={};bytes_=0;boot_=nullptr;scheduler_=nullptr;poisoned_=false;return NML_OK;
}
} // namespace native_mali
