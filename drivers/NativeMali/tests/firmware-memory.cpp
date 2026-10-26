// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliFirmwareMemory.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <map>
#include <vector>
using namespace native_mali;

struct Pool {
    std::map<uint64_t, TablePage> tables;
    std::map<void *, std::vector<uint64_t>> allocations;
    uint64_t next = 0x10000000;
    size_t budget = 100, published = 0, acquired = 0, uncached = 0;
    static bool table(void *cookie, TablePage *page) {
        auto &p = *static_cast<Pool *>(cookie);
        void *data = aligned_alloc(4096, 4096); assert(data);
        *page = {p.next, static_cast<uint64_t *>(data), data};
        p.next += 4096; p.tables[page->physical] = *page; return true;
    }
    static void freeTable(void *cookie, const TablePage &page) {
        auto &p = *static_cast<Pool *>(cookie);
        assert(p.tables.erase(page.physical) == 1); free(page.allocation);
    }
    static bool resolve(void *cookie, uint64_t address, TablePage *page) {
        auto &p = *static_cast<Pool *>(cookie); auto it = p.tables.find(address);
        if (it == p.tables.end()) return false;
        *page = it->second; return true;
    }
    static void publishTable(void *, const TablePage &, size_t, size_t) {}
    static bool memory(void *cookie, size_t count, bool uncached, FirmwareAllocation *allocation) {
        auto &p = *static_cast<Pool *>(cookie);
        if (!p.budget) return false;
        --p.budget;
        void *data = aligned_alloc(4096, count * 4096); assert(data);
        auto &pages = p.allocations[data];
        for (size_t i = 0; i < count; ++i) { pages.push_back(p.next); p.next += 8192; }
        *allocation = {static_cast<uint8_t *>(data), pages.data(), count, data};
        p.uncached += uncached; return true;
    }
    static void freeMemory(void *cookie, const FirmwareAllocation &allocation) {
        auto &p = *static_cast<Pool *>(cookie);
        assert(p.allocations.erase(allocation.handle) == 1); free(allocation.handle);
    }
    static void publish(void *cookie, const FirmwareAllocation &a, size_t offset, size_t bytes) {
        assert(offset <= a.pageCount * 4096 && bytes <= a.pageCount * 4096 - offset);
        ++static_cast<Pool *>(cookie)->published;
    }
    static void acquire(void *cookie, const FirmwareAllocation &a, size_t offset, size_t bytes) {
        assert(offset <= a.pageCount * 4096 && bytes <= a.pageCount * 4096 - offset);
        ++static_cast<Pool *>(cookie)->acquired;
    }
    TableOps tableOps() { return {this, table, freeTable, resolve, publishTable}; }
    FirmwareMemoryOps memoryOps() { return {this, memory, freeMemory, publish, acquire}; }
    ~Pool() { assert(tables.empty() && allocations.empty()); }
};
static void put(std::vector<uint8_t> &data, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) data.at(offset + i) = value >> (8 * i);
}
static std::vector<uint8_t> fixture() {
    std::vector<uint8_t> data(144);
    put(data, 0, 0xc3f13a6e); data[4] = 3; put(data, 16, 116);
    const uint32_t flags[] = {FirmwareImage::Read | FirmwareImage::Execute | 8,
        FirmwareImage::Read | FirmwareImage::Write | FirmwareImage::Shared | FirmwareImage::Zero,
        FirmwareImage::Read | FirmwareImage::Protected};
    for (size_t i = 0; i < 3; ++i) {
        const size_t offset = 20 + 32 * i;
        put(data, offset, 32 << 8); put(data, offset + 4, flags[i]);
        put(data, offset + 8, i * 0x100000); put(data, offset + 12, i * 0x100000 + 4096);
        put(data, offset + 16, 128); put(data, offset + 20, 144);
        data[offset + 24] = 'a' + i; put(data, offset + 26, i * 0x100000);
    }
    put(data, 128, 0xdecafbad); return data;
}
static void lifecycle() {
    auto data = fixture(); FirmwareImage image; Pool pool; Mmu mmu; FirmwareMemory memory;
    assert(image.parse(data.data(), data.size()) == FirmwareResult::Ok);
    assert(mmu.initialize(pool.tableOps(), 40, 32, true) == MmuResult::Ok);
    assert(memory.load(image, mmu, pool.memoryOps(), {}) == FirmwareMemoryResult::Ok);
    assert(memory.prepared() && memory.pageCount() == 2 && memory.skippedProtected() == 1);
    assert(pool.allocations.size() == 2 && pool.uncached == 1 && pool.published == 2);
    uint32_t word = 0; uint64_t physical = 0, descriptor = 0;
    assert(memory.read32(0, &word) && word == 0xdecafbad);
    assert(memory.read32(128, &word) && word == 0);
    assert(!memory.write32(0, 7));
    assert(memory.write32(0x100000, 0x12345678));
    assert(memory.read32(0x100000, &word) && word == 0x12345678);
    assert(!memory.read32(1, &word) && !memory.read32(0x200000, &word));
    assert(!memory.write32(0x101000, 9));
    assert(mmu.translate(0, &physical, &descriptor) == MmuResult::Ok && !(descriptor & (1ull << 54)));
    assert(mmu.translate(0x100000, &physical, &descriptor) == MmuResult::Ok && (descriptor & 0x1c) == 0x14);
    assert(memory.unload(false) == FirmwareMemoryResult::Busy && pool.allocations.size() == 2);
    assert(memory.load(image, mmu, pool.memoryOps(), {}) == FirmwareMemoryResult::State);
    assert(memory.unload(true) == FirmwareMemoryResult::Ok && pool.allocations.empty());
    assert(!memory.prepared() && !memory.read32(0, &word));
    assert(mmu.translate(0, &physical) == MmuResult::Missing);
    assert(mmu.destroy(true) == MmuResult::Ok);
}
static void rollback() {
    auto data = fixture(); FirmwareImage image;
    assert(image.parse(data.data(), data.size()) == FirmwareResult::Ok);
    for (size_t budget = 0; budget < 2; ++budget) {
        Pool pool; Mmu mmu; FirmwareMemory memory; pool.budget = budget;
        assert(mmu.initialize(pool.tableOps(), 40, 32, true) == MmuResult::Ok);
        assert(memory.load(image, mmu, pool.memoryOps(), {}) == FirmwareMemoryResult::Allocation);
        assert(!memory.prepared() && pool.allocations.empty() && pool.tables.size() == 1);
        assert(mmu.destroy(true) == MmuResult::Ok);
    }
    Pool pool; Mmu mmu; FirmwareMemory memory;
    assert(mmu.initialize(pool.tableOps(), 40, 32, true) == MmuResult::Ok);
    FirmwareMemoryPolicy policy{}; policy.maxPages = 1;
    assert(memory.load(image, mmu, pool.memoryOps(), policy) == FirmwareMemoryResult::Capacity);
    const uint64_t page = 0x90000000;
    assert(mmu.map(0x100000, &page, 1, {}) == MmuResult::Ok);
    assert(memory.load(image, mmu, pool.memoryOps(), {}) == FirmwareMemoryResult::Mapping);
    assert(!memory.prepared() && pool.allocations.empty());
    uint64_t physical = 0;
    assert(mmu.translate(0, &physical) == MmuResult::Missing);
    assert(mmu.translate(0x100000, &physical) == MmuResult::Ok && physical == page);
    assert(mmu.destroy(true) == MmuResult::Ok);
}
int main(int argc, char **argv) {
    lifecycle(); rollback();
    puts("PASS: production MCU memory copy/zero/cache permissions, protected skip and rollback; hardware_exercised=false");
    if (argc == 2) {
        std::ifstream input(argv[1], std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(input)), {});
        FirmwareImage image; Pool pool; Mmu mmu; FirmwareMemory memory;
        if (!input || image.parse(data.data(), data.size()) != FirmwareResult::Ok) return 2;
        assert(mmu.initialize(pool.tableOps(), 40, 32, true) == MmuResult::Ok);
        const auto result = memory.load(image, mmu, pool.memoryOps(), {});
        printf("Private MCU image: result=%u pages=%zu protected=%zu; GPU not accessed\n",
               unsigned(result), memory.pageCount(), memory.skippedProtected());
        if (result == FirmwareMemoryResult::Ok) assert(memory.unload(true) == FirmwareMemoryResult::Ok);
        assert(mmu.destroy(true) == MmuResult::Ok);
        return result == FirmwareMemoryResult::Ok ? 0 : 1;
    }
}
