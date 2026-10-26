// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliMmu.h"

namespace native_mali {
static constexpr uint64_t AddressMask = ((uint64_t(1) << 48) - 1) & ~uint64_t(4095);
static constexpr uint64_t AccessFlag = uint64_t(1) << 10;
static uint64_t load(const TablePage &page, size_t slot)
{
    return __atomic_load_n(page.entries + slot, __ATOMIC_ACQUIRE);
}
static size_t indexAt(uint64_t address, unsigned level)
{
    return (address >> (39 - 9 * level)) & 511;
}
bool Mmu::physicalValid(uint64_t address) const
{
    return address && !(address & (PageBytes - 1)) && address < (uint64_t(1) << physicalBits_);
}
bool Mmu::pageValid(const TablePage &page) const
{
    return physicalValid(page.physical) && page.entries &&
           !(reinterpret_cast<uintptr_t>(page.entries) & (PageBytes - 1));
}
bool Mmu::rangeValid(uint64_t address, size_t count) const
{
    const uint64_t end = uint64_t(1) << virtualBits_;
    return initialized_ && (firmwareAddressSpace_ || address >= PageBytes) && !(address & (PageBytes - 1)) &&
           count && address < end && count <= (end - address) / PageBytes;
}
bool Mmu::resolve(uint64_t descriptor, TablePage *page) const
{
    const uint64_t physical = descriptor & AddressMask;
    return ops_.resolve(ops_.cookie, physical, page) && pageValid(*page) && page->physical == physical;
}
void Mmu::store(const TablePage &page, size_t slot, uint64_t value)
{
    __atomic_store_n(page.entries + slot, value, __ATOMIC_RELEASE);
    ops_.publish(ops_.cookie, page, slot * sizeof(uint64_t), sizeof(uint64_t));
}
MmuResult Mmu::allocate(TablePage *page)
{
    *page = {};
    if (!ops_.allocate(ops_.cookie, page)) return MmuResult::NoMemory;
    if (!pageValid(*page)) {
        ops_.release(ops_.cookie, *page);
        *page = {};
        return MmuResult::Corrupt;
    }
    for (size_t i = 0; i < Entries; ++i) page->entries[i] = InvalidEntry;
    ops_.publish(ops_.cookie, *page, 0, PageBytes);
    return MmuResult::Ok;
}
MmuResult Mmu::initialize(const TableOps &ops, uint8_t physicalBits, uint8_t virtualBits, bool firmwareAddressSpace)
{
    if (initialized_) return MmuResult::State;
    if (!ops.allocate || !ops.release || !ops.resolve || !ops.publish ||
        physicalBits < 32 || physicalBits > 48 || virtualBits < 32 || virtualBits > 48)
        return MmuResult::Invalid;
    ops_ = ops; physicalBits_ = physicalBits; virtualBits_ = virtualBits;
    firmwareAddressSpace_ = firmwareAddressSpace;
    const MmuResult result = allocate(&root_);
    if (result != MmuResult::Ok) return result;
    initialized_ = true;
    generation_ = 1;
    return MmuResult::Ok;
}
MmuResult Mmu::leaf(uint64_t address, TablePage *page, size_t *slot) const
{
    if (!initialized_) return MmuResult::State;
    if (address >= (uint64_t(1) << virtualBits_)) return MmuResult::Invalid;
    *page = root_;
    for (unsigned level = 0; level < 3; ++level) {
        const uint64_t descriptor = load(*page, indexAt(address, level));
        if (descriptor == InvalidEntry) return MmuResult::Missing;
        if ((descriptor & 3) != 3 || !resolve(descriptor, page)) return MmuResult::Corrupt;
    }
    *slot = indexAt(address, 3);
    const uint64_t descriptor = load(*page, *slot);
    if (descriptor == InvalidEntry) return MmuResult::Missing;
    return (descriptor & 3) == 3 ? MmuResult::Ok : MmuResult::Corrupt;
}
MmuResult Mmu::translate(uint64_t address, uint64_t *physicalAddress, uint64_t *descriptor) const
{
    if (!physicalAddress) return MmuResult::Invalid;
    *physicalAddress = 0;
    if (descriptor) *descriptor = 0;
    TablePage page{}; size_t slot = 0;
    const MmuResult result = leaf(address, &page, &slot);
    if (result != MmuResult::Ok) return result;
    const uint64_t value = load(page, slot);
    const uint64_t physical = value & AddressMask;
    if (!physicalValid(physical)) return MmuResult::Corrupt;
    *physicalAddress = physical | (address & (PageBytes - 1));
    if (descriptor) *descriptor = value;
    return MmuResult::Ok;
}
MmuResult Mmu::mapOne(uint64_t address, uint64_t physical, uint64_t attributes)
{
    struct Created { TablePage parent, child; size_t slot; } created[3]{};
    size_t count = 0;
    TablePage page = root_;
    MmuResult result = MmuResult::Ok;
    for (unsigned level = 0; level < 3; ++level) {
        const size_t slot = indexAt(address, level);
        const uint64_t descriptor = load(page, slot);
        TablePage next{};
        if (descriptor == InvalidEntry) {
            result = allocate(&next);
            if (result != MmuResult::Ok) break;
            created[count++] = {page, next, slot};
            store(page, slot, next.physical | AccessFlag | 3);
        } else if ((descriptor & 3) != 3 || !resolve(descriptor, &next)) {
            result = MmuResult::Corrupt;
            break;
        }
        page = next;
    }
    if (result == MmuResult::Ok) {
        const size_t slot = indexAt(address, 3);
        if (load(page, slot) != InvalidEntry) result = MmuResult::Conflict;
        else store(page, slot, physical | attributes);
    }
    if (result != MmuResult::Ok) {
        while (count) {
            const Created &entry = created[--count];
            store(entry.parent, entry.slot, InvalidEntry);
            ops_.release(ops_.cookie, entry.child);
        }
    }
    return result;
}
MmuResult Mmu::map(uint64_t address, const uint64_t *physicalPages, size_t count, PageAttributes attributes)
{
    if (!rangeValid(address, count) || !physicalPages || attributes.memoryIndex > 7 ||
        (attributes.sharing != PageAttributes::Sharing::None &&
         attributes.sharing != PageAttributes::Sharing::Outer &&
         attributes.sharing != PageAttributes::Sharing::Inner)) return MmuResult::Invalid;
    // Validate the entire request before publishing any mapping.
    for (size_t i = 0; i < count; ++i) {
        if (!physicalValid(physicalPages[i])) return MmuResult::Invalid;
        TablePage page{}; size_t slot = 0;
        const MmuResult result = leaf(address + i * PageBytes, &page, &slot);
        if (result == MmuResult::Ok) return MmuResult::Conflict;
        if (result != MmuResult::Missing) return result;
    }
    uint64_t flags = 3 | AccessFlag | (uint64_t(attributes.memoryIndex) << 2) |
                     (uint64_t(attributes.sharing) << 8) |
                     (attributes.writable ? uint64_t(1) << 6 : uint64_t(3) << 6);
    if (!attributes.executable) flags |= uint64_t(1) << 54;
    size_t done = 0;
    for (; done < count; ++done) {
        const MmuResult result = mapOne(address + done * PageBytes, physicalPages[done], flags);
        if (result != MmuResult::Ok) {
            while (done) {
                if (unmapOne(address + --done * PageBytes) != MmuResult::Ok) return MmuResult::Corrupt;
            }
            ++generation_; // A failed transaction may still have touched table memory.
            return result;
        }
    }
    ++generation_;
    return MmuResult::Ok;
}
MmuResult Mmu::unmapOne(uint64_t address)
{
    TablePage pages[4]{root_}; size_t slots[4]{};
    for (unsigned level = 0; level < 4; ++level) {
        slots[level] = indexAt(address, level);
        const uint64_t descriptor = load(pages[level], slots[level]);
        if (descriptor == InvalidEntry) return MmuResult::Missing;
        if ((descriptor & 3) != 3) return MmuResult::Corrupt;
        if (level != 3 && !resolve(descriptor, &pages[level + 1])) return MmuResult::Corrupt;
    }
    store(pages[3], slots[3], InvalidEntry);
    for (unsigned level = 3; level > 0; --level) {
        bool empty = true;
        for (size_t i = 0; i < Entries && empty; ++i) empty = load(pages[level], i) == InvalidEntry;
        if (!empty) break;
        store(pages[level - 1], slots[level - 1], InvalidEntry);
        ops_.release(ops_.cookie, pages[level]);
    }
    return MmuResult::Ok;
}
MmuResult Mmu::unmap(uint64_t address, size_t count)
{
    if (!rangeValid(address, count)) return MmuResult::Invalid;
    for (size_t i = 0; i < count; ++i) {
        uint64_t physical = 0;
        const MmuResult result = translate(address + i * PageBytes, &physical);
        if (result != MmuResult::Ok) return result;
    }
    for (size_t i = 0; i < count; ++i) {
        const MmuResult result = unmapOne(address + i * PageBytes);
        if (result != MmuResult::Ok) return result;
    }
    ++generation_;
    return MmuResult::Ok;
}
MmuResult Mmu::releaseTree(const TablePage &page, unsigned level)
{
    if (level < 3) {
        for (size_t slot = 0; slot < Entries; ++slot) {
            const uint64_t descriptor = load(page, slot);
            if (descriptor == InvalidEntry) continue;
            TablePage child{};
            if ((descriptor & 3) != 3 || !resolve(descriptor, &child)) return MmuResult::Corrupt;
            const MmuResult result = releaseTree(child, level + 1);
            if (result != MmuResult::Ok) return result;
            store(page, slot, InvalidEntry);
        }
    }
    ops_.release(ops_.cookie, page);
    return MmuResult::Ok;
}
MmuResult Mmu::destroy(bool addressSpaceQuiesced)
{
    if (!initialized_) return MmuResult::State;
    if (!addressSpaceQuiesced) return MmuResult::Busy;
    const MmuResult result = releaseTree(root_, 0);
    if (result != MmuResult::Ok) return result;
    initialized_ = false; root_ = {}; ++generation_;
    return MmuResult::Ok;
}
} // namespace native_mali
