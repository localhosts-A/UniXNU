// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliABI.h"
#include "NativeMaliScheduler.h"
namespace native_mali {
struct SessionOps {
    FirmwareMemoryOps memory{}; TableOps tables{};
    bool (*importHost)(void *, uint64_t address, size_t pages, FirmwareAllocation *) = nullptr;
    void (*delayUs)(void *, uint32_t) = nullptr;
    uint64_t (*nowNs)(void *) = nullptr;
};
// One connection/AS. The caller serializes this object with the scheduler and
// all other sessions. Failed MMU/CSF handshakes poison the session and retain
// backing storage until close succeeds or reset quiescence is proven.
class Session {
public:
    static constexpr size_t MaxBuffers = 512, MaxHeaps = 16, MaxChunks = 256;
    uint32_t initialize(Boot &, Scheduler &, uint8_t as, const SessionOps &, size_t byteBudget);
    uint32_t call(const nml_request &, nml_response *);
    uint32_t close(bool deviceQuiesced = false);
    bool bufferBacking(uint32_t handle, FirmwareAllocation *, size_t *bytes);
    bool queueBacking(uint32_t group, uint32_t stream, QueueView *);
    bool oom(uint64_t heap, uint32_t vtStart, uint32_t vtEnd, uint32_t fragEnd, uint64_t *chunk);
    uint8_t addressSpace() const { return as_; }
    bool retained() const { return poisoned_; }
private:
    struct Buffer {
        uint32_t handle = 0, flags = 0, parent = 0, refs = 0;
        uint64_t va = 0; size_t pages = 0, offsetPages = 0, repeats = 1;
        FirmwareAllocation allocation{}; bool mapped = false, internal = false;
    } buffers_[MaxBuffers]{};
    struct Binding { uint32_t group = 0, stream = 0, buffer = 0; } bindings_[Scheduler::MaxGroups * Scheduler::MaxStreams]{};
    struct Heap {
        uint32_t handle = 0, context = 0, chunkBytes = 0, count = 0, maximum = 0, target = 0;
        uint32_t chunks[MaxChunks]{};
    } heaps_[MaxHeaps]{};
    Boot *boot_ = nullptr; Scheduler *scheduler_ = nullptr; SessionOps ops_{}; Mmu mmu_{};
    uint8_t as_ = 0; uint32_t serial_ = 0, timeout_ = 100000;
    size_t bytes_ = 0, budget_ = 0; bool configured_ = false, poisoned_ = false;
    Buffer *buffer(uint32_t);
    Buffer *enclosing(uint64_t address, size_t bytes, size_t *offset);
    uint64_t freeVa(size_t bytes, bool executable, bool within4G = false) const;
    uint32_t allocate(size_t bytes, uint32_t flags, uint64_t host, Buffer **, bool internal = false);
    uint32_t release(uint32_t handle, bool internal = false);
    uint32_t alias(const nml_request &, nml_response *);
    uint32_t beginMutation();
    uint32_t endMutation();
    uint32_t heapCreate(const nml_request &, nml_response *);
    uint32_t heapDestroy(uint32_t);
    uint32_t fail() { poisoned_ = true; return NML_DEVICE_LOST; }
    void info(nml_response *);
};
} // namespace native_mali
