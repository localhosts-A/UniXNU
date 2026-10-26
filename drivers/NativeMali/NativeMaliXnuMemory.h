// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliSession.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <mach/mach_types.h>
namespace native_mali {
class XnuMemory;
struct XnuMemoryOwner { XnuMemory *memory = nullptr; task_t task = TASK_NULL; };
// Serialized by the IOService lock. Persistent descriptors pin imports across
// task unmapping. Cache attributes are reference counted by physical page so
// overlapping imports cannot restore a still-live GPU allocation to WB.
class XnuMemory {
public:
    FirmwareMemoryOps memoryOps(XnuMemoryOwner *);
    TableOps tableOps();
    static bool importHost(void *, uint64_t, size_t, FirmwareAllocation *);
    static void delay(void *, uint32_t);
    static uint64_t now(void *);
    IOMemoryDescriptor *descriptor(const FirmwareAllocation &) const;
    bool empty() const { return !allocations_; }
private:
    struct Allocation {
        IOMemoryDescriptor *descriptor = nullptr; IOMemoryMap *map = nullptr;
        uint64_t *pages = nullptr; uint8_t *cpu = nullptr; size_t count = 0, cachePages = 0;
        bool prepared = false, uncached = false, table = false;
        Allocation *next = nullptr;
    } *allocations_ = nullptr;
    struct CachePage { uint64_t physical; uint32_t previous, refs; CachePage *next; } *cache_[1024]{};
    size_t bytes_ = 0;
    bool allocate(task_t, uint64_t, size_t, bool uncached, bool table, FirmwareAllocation *);
    void release(const FirmwareAllocation &);
    bool holdCache(uint64_t);
    void dropCache(uint64_t);
    static void maintain(Allocation *, size_t, size_t, bool acquire);
    static bool alloc(void *, size_t, bool, FirmwareAllocation *);
    static void free(void *, const FirmwareAllocation &);
    static void publish(void *, const FirmwareAllocation &, size_t, size_t);
    static void acquire(void *, const FirmwareAllocation &, size_t, size_t);
    static bool tableAlloc(void *, TablePage *);
    static void tableFree(void *, const TablePage &);
    static bool tableResolve(void *, uint64_t, TablePage *);
    static void tablePublish(void *, const TablePage &, size_t, size_t);
};
} // namespace native_mali
