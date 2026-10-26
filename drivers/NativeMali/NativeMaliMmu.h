// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace native_mali {
enum class MmuResult : uint32_t { Ok, Invalid, State, NoMemory, Conflict, Missing, Corrupt, Busy };

struct TablePage {
    uint64_t physical = 0;
    uint64_t *entries = nullptr;
    void *allocation = nullptr;
};

// All callbacks are serialized by the owning address-space lock. resolve must
// return an owned table, never an arbitrary physical aperture mapping.
struct TableOps {
    void *cookie = nullptr;
    bool (*allocate)(void *, TablePage *) = nullptr;
    void (*release)(void *, const TablePage &) = nullptr;
    bool (*resolve)(void *, uint64_t, TablePage *) = nullptr;
    void (*publish)(void *, const TablePage &, size_t, size_t) = nullptr;
};

struct PageAttributes {
    enum class Sharing : uint8_t { None = 0, Outer = 2, Inner = 3 };
    uint8_t memoryIndex = 0;
    Sharing sharing = Sharing::None;
    bool writable = false;
    bool executable = false;
};

// Mali AArch64 4 KiB GPU page tables, not CPU translation tables and not the
// platform SMMU. Hardware descriptor encodings are documented by the pinned
// Mali AArch64 MMU reference in scripts/data/mali-g925-inputs.json.
// This class owns table construction only. The driver must quiesce the address
// space before mutation and complete GPU MMU/TLB synchronization before reuse.
class Mmu {
public:
    static constexpr uint64_t PageBytes = 4096;
    static constexpr size_t Entries = 512;
    static constexpr uint64_t InvalidEntry = 2;
    MmuResult initialize(const TableOps &, uint8_t physicalBits, uint8_t virtualBits);
    MmuResult map(uint64_t virtualAddress, const uint64_t *physicalPages,
                  size_t count, PageAttributes);
    MmuResult unmap(uint64_t virtualAddress, size_t count);
    MmuResult translate(uint64_t virtualAddress, uint64_t *physicalAddress,
                        uint64_t *descriptor = nullptr) const;
    MmuResult destroy(bool addressSpaceQuiesced);
    uint64_t rootPhysical() const { return initialized_ ? root_.physical : 0; }
    uint64_t generation() const { return generation_; }
    Mmu(const Mmu &) = delete;
    Mmu &operator=(const Mmu &) = delete;
    Mmu() = default;
private:
    TableOps ops_{};
    TablePage root_{};
    uint8_t physicalBits_ = 0, virtualBits_ = 0;
    bool initialized_ = false;
    uint64_t generation_ = 0;
    bool physicalValid(uint64_t) const;
    bool pageValid(const TablePage &) const;
    bool rangeValid(uint64_t, size_t) const;
    bool resolve(uint64_t, TablePage *) const;
    MmuResult allocate(TablePage *);
    void store(const TablePage &, size_t, uint64_t);
    MmuResult leaf(uint64_t, TablePage *, size_t *) const;
    MmuResult mapOne(uint64_t, uint64_t, uint64_t);
    MmuResult unmapOne(uint64_t);
    MmuResult releaseTree(const TablePage &, unsigned);
};
} // namespace native_mali
