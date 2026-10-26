// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliFirmwareMemory.h"

#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error NativeMali firmware shared words require a little-endian host
#endif

namespace native_mali {
FirmwareMemoryResult FirmwareMemory::clear()
{
    // The caller has established that the MCU cannot walk these mappings.
    while (count_) {
        Region &region = regions_[count_ - 1];
        if (region.mapped && mmu_->unmap(region.layout.start, region.memory.pageCount) != MmuResult::Ok)
            return FirmwareMemoryResult::Mapping;
        region.mapped = false;
        ops_.release(ops_.cookie, region.memory);
        region = {}; --count_;
    }
    prepared_ = false; sealed_ = false; totalPages_ = 0; mmu_ = nullptr;
    return FirmwareMemoryResult::Ok;
}
FirmwareMemoryResult FirmwareMemory::load(const FirmwareImage &image, Mmu &mmu,
                                         const FirmwareMemoryOps &ops, FirmwareMemoryPolicy policy)
{
    if (prepared_ || count_) return FirmwareMemoryResult::State;
    if (!image.valid() || !mmu.rootPhysical() || !ops.allocate || !ops.release || !ops.publish || !ops.acquire ||
        policy.cachedAttribute > 7 || policy.uncachedAttribute > 7 || !policy.maxPages)
        return FirmwareMemoryResult::Invalid;
    size_t pages = 0;
    for (size_t i = 0; i < image.regionCount(); ++i) {
        const auto &region = image.regions()[i];
        if (region.flags & FirmwareImage::Protected) continue;
        if (!(region.flags & FirmwareImage::Read)) return FirmwareMemoryResult::Invalid;
        const size_t count = (uint64_t(region.end) - region.start) / 4096;
        if (count > policy.maxPages - pages) return FirmwareMemoryResult::Capacity;
        pages += count;
    }
    ops_ = ops; policy_ = policy; mmu_ = &mmu; skipped_ = 0; sharedNext_ = 0x04000000;
    for (size_t i = 0; i < image.regionCount(); ++i) {
        const auto &r = image.regions()[i];
        if (r.start < 0x08000000 && r.end > sharedNext_)
            sharedNext_ = r.end < 0x08000000 ? r.end : 0x08000000;
    }
    FirmwareMemoryResult failure = FirmwareMemoryResult::Ok;
    for (size_t i = 0; i < image.regionCount(); ++i) {
        const auto &layout = image.regions()[i];
        if (layout.flags & FirmwareImage::Protected) { ++skipped_; continue; }
        const size_t count = (uint64_t(layout.end) - layout.start) / 4096;
        Region &region = regions_[count_];
        region = {}; region.layout = layout;
        const bool shared = (layout.flags & FirmwareImage::Shared) != 0;
        if (!ops_.allocate(ops_.cookie, count, shared, &region.memory)) {
            failure = FirmwareMemoryResult::Allocation; break;
        }
        ++count_; // Every successful allocation participates in cleanup.
        if (!region.memory.cpu || (reinterpret_cast<uintptr_t>(region.memory.cpu) & 4095) ||
            region.memory.pageCount != count || !region.memory.physicalPages) {
            failure = FirmwareMemoryResult::Allocation; break;
        }
        const size_t bytes = count * 4096;
        const size_t dataBytes = uint64_t(layout.fileEnd) - layout.fileStart;
        for (size_t j = 0; j < bytes; ++j)
            region.memory.cpu[j] = j < dataBytes ? image.data()[layout.fileStart + j] : 0;
        ops_.publish(ops_.cookie, region.memory, 0, bytes);
        PageAttributes attributes{};
        // With no validated ACE route, firmware's coherent cache modes are
        // mapped uncached as specified by the non-coherent CSF boot policy.
        const unsigned cacheMode = (layout.flags & FirmwareImage::CacheMask) >> 3;
        attributes.memoryIndex = cacheMode == 1 && !shared ? policy.cachedAttribute : policy.uncachedAttribute;
        attributes.writable = (layout.flags & FirmwareImage::Write) != 0;
        attributes.executable = (layout.flags & FirmwareImage::Execute) != 0;
        if (mmu_->map(layout.start, region.memory.physicalPages, count, attributes) != MmuResult::Ok) {
            failure = FirmwareMemoryResult::Mapping; break;
        }
        region.mapped = true;
        totalPages_ += count;
    }
    if (failure != FirmwareMemoryResult::Ok) {
        const auto cleaned = clear();
        return cleaned == FirmwareMemoryResult::Ok ? failure : cleaned;
    }
    prepared_ = true;
    return FirmwareMemoryResult::Ok;
}
FirmwareMemoryResult FirmwareMemory::unload(bool mcuQuiesced)
{
    if (!prepared_ && !count_) return FirmwareMemoryResult::State;
    if (!mcuQuiesced) return FirmwareMemoryResult::Busy;
    return clear();
}
FirmwareMemoryResult FirmwareMemory::allocateShared(size_t count, uint32_t *address)
{
    if (address) *address = 0;
    if (!prepared_) return FirmwareMemoryResult::State;
    if (sealed_) return FirmwareMemoryResult::Busy;
    if (!address || !count) return FirmwareMemoryResult::Invalid;
    if (count_ == FirmwareImage::MaxRegions || count > policy_.maxPages - totalPages_ ||
        count > (0x08000000 - sharedNext_) / 4096) return FirmwareMemoryResult::Capacity;
    FirmwareAllocation memory{};
    if (!ops_.allocate(ops_.cookie, count, true, &memory)) return FirmwareMemoryResult::Allocation;
    if (!memory.cpu || (reinterpret_cast<uintptr_t>(memory.cpu) & 4095) ||
        memory.pageCount != count || !memory.physicalPages) {
        ops_.release(ops_.cookie, memory); return FirmwareMemoryResult::Allocation;
    }
    for (size_t i = 0; i < count * 4096; ++i) memory.cpu[i] = 0;
    ops_.publish(ops_.cookie, memory, 0, count * 4096);
    PageAttributes attributes{};
    attributes.memoryIndex = policy_.uncachedAttribute; attributes.writable = true;
    if (mmu_->map(sharedNext_, memory.physicalPages, count, attributes) != MmuResult::Ok) {
        ops_.release(ops_.cookie, memory); return FirmwareMemoryResult::Mapping;
    }
    Region &region = regions_[count_++]; region = {};
    region.memory = memory; region.mapped = true;
    region.layout.flags = FirmwareImage::Read | FirmwareImage::Write | FirmwareImage::Shared;
    region.layout.start = sharedNext_; region.layout.end = sharedNext_ + uint32_t(count * 4096);
    *address = sharedNext_; sharedNext_ = region.layout.end; totalPages_ += count;
    return FirmwareMemoryResult::Ok;
}
bool FirmwareMemory::accessible(uint32_t address, size_t bytes, uint32_t flags) const
{
    if (!prepared_ || !bytes || bytes > uint64_t(0x100000000) - address) return false;
    for (size_t i = 0; i < count_; ++i) {
        const auto &r = regions_[i].layout;
        if (address >= r.start && uint64_t(address) + bytes <= r.end && (r.flags & flags) == flags) return true;
    }
    return false;
}
bool FirmwareMemory::initializable(uint32_t address, size_t bytes) const
{
    return !sealed_ && accessible(address,bytes,FirmwareImage::Read) &&
           !accessible(address,bytes,FirmwareImage::Execute);
}
bool FirmwareMemory::initialize32(uint32_t address, uint32_t value)
{
    if (!initializable(address,4)) return false;
    Region *region = containing(address,false);
    if (!region) return false;
    const size_t offset = address - region->layout.start;
    __atomic_store_n(reinterpret_cast<uint32_t *>(region->memory.cpu + offset),value,__ATOMIC_RELEASE);
    ops_.publish(ops_.cookie,region->memory,offset,4); return true;
}
FirmwareMemory::Region *FirmwareMemory::containing(uint32_t address, bool write)
{
    if (!prepared_ || (address & 3)) return nullptr;
    for (size_t i = 0; i < count_; ++i) {
        Region &region = regions_[i];
        if (address >= region.layout.start && uint64_t(address) + 4 <= region.layout.end &&
            (!write || (region.layout.flags & FirmwareImage::Write))) return &region;
    }
    return nullptr;
}
bool FirmwareMemory::read32(uint32_t address, uint32_t *value)
{
    if (!value) return false;
    *value = 0;
    Region *region = containing(address, false);
    if (!region) return false;
    const size_t offset = address - region->layout.start;
    ops_.acquire(ops_.cookie, region->memory, offset, 4);
    *value = __atomic_load_n(reinterpret_cast<uint32_t *>(region->memory.cpu + offset), __ATOMIC_ACQUIRE);
    return true;
}
bool FirmwareMemory::write32(uint32_t address, uint32_t value)
{
    Region *region = containing(address, true);
    if (!region) return false;
    const size_t offset = address - region->layout.start;
    __atomic_store_n(reinterpret_cast<uint32_t *>(region->memory.cpu + offset), value, __ATOMIC_RELEASE);
    ops_.publish(ops_.cookie, region->memory, offset, 4);
    return true;
}
bool FirmwareMemory::backing(uint32_t address, size_t bytes, FirmwareAllocation *allocation, size_t *offset)
{
    if (!allocation || !offset || !accessible(address, bytes, FirmwareImage::Read | FirmwareImage::Write)) return false;
    Region *region = containing(address, true);
    if (!region) return false;
    *allocation = region->memory; *offset = address - region->layout.start; return true;
}
bool FirmwareMemory::read64(uint32_t address, uint64_t *value)
{
    FirmwareAllocation allocation{}; size_t offset = 0;
    if (!value || (address & 7) || !backing(address, 8, &allocation, &offset)) return false;
    ops_.acquire(ops_.cookie, allocation, offset, 8);
    *value = __atomic_load_n(reinterpret_cast<uint64_t *>(allocation.cpu + offset), __ATOMIC_ACQUIRE); return true;
}
bool FirmwareMemory::write64(uint32_t address, uint64_t value)
{
    FirmwareAllocation allocation{}; size_t offset = 0;
    if ((address & 7) || !backing(address, 8, &allocation, &offset)) return false;
    __atomic_store_n(reinterpret_cast<uint64_t *>(allocation.cpu + offset), value, __ATOMIC_RELEASE);
    ops_.publish(ops_.cookie, allocation, offset, 8); return true;
}
bool FirmwareMemory::zero(uint32_t address, size_t bytes)
{
    FirmwareAllocation allocation{}; size_t offset = 0;
    if (!backing(address, bytes, &allocation, &offset)) return false;
    for (size_t i = 0; i < bytes; ++i) allocation.cpu[offset + i] = 0;
    ops_.publish(ops_.cookie, allocation, offset, bytes); return true;
}
} // namespace native_mali
