// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliMmu.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <map>
using namespace native_mali;

struct Pool {
    std::map<uint64_t, TablePage> pages;
    size_t remaining = 100, publications = 0, allocations = 0;
    uint64_t next = 0x10000000;
    bool invalidNext = false;
    static bool allocate(void *cookie, TablePage *page) {
        auto &self = *static_cast<Pool *>(cookie);
        if (!self.remaining) return false;
        --self.remaining;
        void *data = aligned_alloc(4096, 4096);
        assert(data);
        *page = {self.next, static_cast<uint64_t *>(data), data};
        self.next += 4096;
        if (self.invalidNext) { page->physical |= 1; self.invalidNext = false; }
        self.pages[page->physical] = *page;
        ++self.allocations;
        return true;
    }
    static void release(void *cookie, const TablePage &page) {
        auto &self = *static_cast<Pool *>(cookie);
        const auto it = self.pages.find(page.physical);
        assert(it != self.pages.end() && it->second.entries == page.entries);
        free(page.allocation);
        self.pages.erase(it);
    }
    static bool resolve(void *cookie, uint64_t physical, TablePage *page) {
        auto &self = *static_cast<Pool *>(cookie);
        const auto it = self.pages.find(physical);
        if (it == self.pages.end()) return false;
        *page = it->second;
        return true;
    }
    static void publish(void *cookie, const TablePage &page, size_t offset, size_t bytes) {
        auto &self = *static_cast<Pool *>(cookie);
        assert(self.pages.count(page.physical));
        assert(offset <= 4096 && bytes && bytes <= 4096 - offset);
        ++self.publications;
    }
    TableOps ops() { return {this, allocate, release, resolve, publish}; }
    ~Pool() { assert(pages.empty()); }
};

static void mapping() {
    Pool pool; Mmu mmu;
    assert(mmu.initialize(pool.ops(), 48, 48) == MmuResult::Ok);
    for (size_t i = 0; i < 512; ++i) assert(pool.pages.begin()->second.entries[i] == 2);
    const uint64_t addresses[] = {0x100000000, 0x100012000};
    PageAttributes attributes{};
    attributes.memoryIndex = 2; attributes.sharing = PageAttributes::Sharing::Inner; attributes.writable = true;
    assert(mmu.map(0x1ff000, addresses, 2, attributes) == MmuResult::Ok);
    uint64_t physical = 0, descriptor = 0;
    assert(mmu.translate(0x1ff023, &physical, &descriptor) == MmuResult::Ok);
    assert(physical == 0x100000023);
    // Golden AArch64 Mali descriptor: output address + NX, inner-shareable,
    // access flag, RW, memory attribute 2 and level-3 valid ATE.
    assert(descriptor == 0x004000010000074b);
    assert(mmu.translate(0x200fff, &physical) == MmuResult::Ok && physical == 0x100012fff);
    assert(pool.pages.size() == 5);
    const uint64_t generation = mmu.generation();
    assert(mmu.map(0x1ff000, addresses, 2, attributes) == MmuResult::Conflict);
    assert(mmu.generation() == generation);
    assert(mmu.destroy(false) == MmuResult::Busy && pool.pages.size() == 5);
    assert(mmu.unmap(0x1ff000, 1) == MmuResult::Ok && pool.pages.size() == 4);
    assert(mmu.translate(0x1ff000, &physical) == MmuResult::Missing);
    assert(mmu.translate(0x200000, &physical) == MmuResult::Ok && physical == addresses[1]);
    assert(mmu.unmap(0x200000, 1) == MmuResult::Ok && pool.pages.size() == 1);
    assert(mmu.destroy(true) == MmuResult::Ok);
}
static void transactions() {
    // The mapping straddles a 2 MiB boundary. Fault each of the four possible
    // subordinate table allocations and require complete rollback.
    for (size_t budget = 0; budget < 4; ++budget) {
        Pool pool; Mmu mmu;
        assert(mmu.initialize(pool.ops(), 40, 48) == MmuResult::Ok);
        pool.remaining = budget;
        const uint64_t addresses[] = {0x80000000, 0x80001000};
        assert(mmu.map(0x1ff000, addresses, 2, {}) == MmuResult::NoMemory);
        assert(pool.pages.size() == 1);
        uint64_t physical = 7;
        assert(mmu.translate(0x1ff000, &physical) == MmuResult::Missing && physical == 0);
        assert(mmu.translate(0x200000, &physical) == MmuResult::Missing);
        assert(mmu.destroy(true) == MmuResult::Ok);
    }
    Pool pool; Mmu mmu;
    assert(mmu.initialize(pool.ops(), 40, 48) == MmuResult::Ok);
    const uint64_t page = 0x80000000;
    assert(mmu.map(0x3000, &page, 1, {}) == MmuResult::Ok);
    uint64_t physical = 0, descriptor = 0;
    assert(mmu.translate(0x3000, &physical, &descriptor) == MmuResult::Ok && descriptor == 0x00400000800004c3);
    assert(mmu.unmap(0x3000, 2) == MmuResult::Missing);
    assert(mmu.translate(0x3000, &physical) == MmuResult::Ok);
    const uint64_t pair[] = {page, page};
    assert(mmu.map(0x2000, pair, 2, {}) == MmuResult::Conflict);
    assert(mmu.translate(0x2000, &physical) == MmuResult::Missing);
    // Aliasing one owned backing page is needed by ring-wrap mappings.
    PageAttributes executable{}; executable.executable = true;
    assert(mmu.map(0x5000, &page, 1, executable) == MmuResult::Ok);
    assert(mmu.translate(0x5000, &physical, &descriptor) == MmuResult::Ok && descriptor == 0x800004c3);
    assert(mmu.destroy(true) == MmuResult::Ok);
}
static void validation() {
    Pool pool; Mmu mmu;
    assert(mmu.initialize(pool.ops(), 49, 48) == MmuResult::Invalid);
    assert(mmu.initialize(pool.ops(), 40, 48) == MmuResult::Ok);
    const uint64_t page = 0x80000000, bad[] = {page, page + 1};
    assert(mmu.map(0, &page, 1, {}) == MmuResult::Invalid);
    assert(mmu.map(4097, &page, 1, {}) == MmuResult::Invalid);
    assert(mmu.map(4096, bad, 2, {}) == MmuResult::Invalid);
    assert(mmu.map((uint64_t(1) << 48) - 4096, bad, 2, {}) == MmuResult::Invalid);
    assert(mmu.map(4096, &page, size_t(-1), {}) == MmuResult::Invalid);
    const uint64_t outside = uint64_t(1) << 40;
    assert(mmu.map(4096, &outside, 1, {}) == MmuResult::Invalid);
    PageAttributes invalid{}; invalid.memoryIndex = 8;
    assert(mmu.map(4096, &page, 1, invalid) == MmuResult::Invalid);
    pool.invalidNext = true;
    assert(mmu.map(4096, &page, 1, {}) == MmuResult::Corrupt);
    assert(pool.pages.size() == 1);
    auto &root = pool.pages.at(mmu.rootPhysical());
    root.entries[0] = 1; // Unexpected block descriptor must never be replaced.
    assert(mmu.map(4096, &page, 1, {}) == MmuResult::Corrupt);
    assert(root.entries[0] == 1);
    root.entries[0] = 2;
    assert(mmu.destroy(true) == MmuResult::Ok);
}
int main() {
    mapping(); transactions(); validation();
    {
        Pool pool; Mmu firmware;
        assert(firmware.initialize(pool.ops(), 40, 32, true) == MmuResult::Ok);
        const uint64_t physical = 0x90000000;
        assert(firmware.map(0, &physical, 1, {}) == MmuResult::Ok);
        uint64_t translated = 0;
        assert(firmware.translate(7, &translated) == MmuResult::Ok && translated == physical + 7);
        assert(firmware.destroy(true) == MmuResult::Ok);
    }
    puts("PASS: production Mali GPU-MMU core, rollback, aliases and invalid inputs; hardware_exercised=false");
}
