// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliFirmware.h"
#include "NativeMaliMmu.h"
namespace native_mali {
struct FirmwareAllocation {
    uint8_t *cpu = nullptr;
    const uint64_t *physicalPages = nullptr;
    size_t pageCount = 0;
    void *handle = nullptr;
};
struct FirmwareMemoryOps {
    void *cookie = nullptr;
    bool (*allocate)(void *, size_t pages, bool uncachedCpu, FirmwareAllocation *) = nullptr;
    void (*release)(void *, const FirmwareAllocation &) = nullptr;
    void (*publish)(void *, const FirmwareAllocation &, size_t offset, size_t bytes) = nullptr;
    void (*acquire)(void *, const FirmwareAllocation &, size_t offset, size_t bytes) = nullptr;
};
struct FirmwareMemoryPolicy {
    // CSF AS_MEMATTR layout: implementation cache policy at 0, AArch64
    // non-cacheable (0x4c) at 5. The hardware AS setup must use the same layout.
    uint8_t cachedAttribute = 0, uncachedAttribute = 5;
    size_t maxPages = 4096;
};
enum class FirmwareMemoryResult : uint32_t { Ok, State, Invalid, Capacity, Allocation, Mapping, Busy };

// Own the MCU's non-protected image allocations and GPU VA mappings. Shared
// firmware memory is conservatively CPU/GPU uncached; cache hooks also handle
// the other CPU mappings. A load is a CPU-side preparation, not an MCU boot.
class FirmwareMemory {
public:
    FirmwareMemoryResult load(const FirmwareImage &, Mmu &, const FirmwareMemoryOps &, FirmwareMemoryPolicy);
    FirmwareMemoryResult unload(bool mcuQuiesced);
    bool read32(uint32_t virtualAddress, uint32_t *);
    bool write32(uint32_t virtualAddress, uint32_t);
    bool prepared() const { return prepared_; }
    size_t pageCount() const { return prepared_ ? totalPages_ : 0; }
    size_t skippedProtected() const { return skipped_; }
private:
    struct Region { FirmwareRegion layout{}; FirmwareAllocation memory{}; bool mapped = false; } regions_[FirmwareImage::MaxRegions]{};
    FirmwareMemoryOps ops_{};
    Mmu *mmu_ = nullptr;
    size_t count_ = 0, totalPages_ = 0, skipped_ = 0;
    bool prepared_ = false;
    Region *containing(uint32_t address, bool write);
    FirmwareMemoryResult clear();
};
} // namespace native_mali
