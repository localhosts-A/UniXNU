// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliResources.h"
#include <assert.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <vector>
#include <utility>
using namespace native_mali;
static void put(std::vector<uint8_t> &data, size_t offset, uint64_t value, size_t bytes = 8) {
    for (size_t i = 0; i < bytes; ++i) data[offset + i] = value >> (8 * i);
}
static std::vector<uint8_t> fixture() {
    std::vector<uint8_t> bytes(128 + 6 * 8 + 3 * 24);
    const uint64_t header[] = {Resources::Magic, 1, 3, 6, 4096, 4096, 13, 1};
    for (size_t i = 0; i < 8; ++i) put(bytes, i * 8, header[i]);
    bytes[64] = 1; bytes[96] = 2;
    for (size_t i = 0; i < 6; ++i) put(bytes, 128 + i * 8, 604 + i);
    put(bytes, 176, 1, 4); put(bytes, 180, 1, 4);
    put(bytes, 184, 0x48000000); put(bytes, 192, 0x480000);
    put(bytes, 200, 4, 4); put(bytes, 204, 1, 4);
    put(bytes, 208, 0x4b800000); put(bytes, 216, 0x1000);
    put(bytes, 224, 128, 4); put(bytes, 228, 2, 4);
    put(bytes, 232, 0xfc810000); put(bytes, 240, 0x180000);
    return bytes;
}
int main(int argc, char **argv) {
    Resources resources;
    const auto good = fixture();
    assert(resources.decode(good.data(), good.size()));
    assert(resources.window(WindowId::Gpu)->base == 0x48000000);
    assert(resources.window(WindowId::GpuebShared)->kind == WindowKind::ReservedMemory);
    assert(!resources.window(WindowId::Mbox0) && resources.spi(0) == 604);
    for (size_t bytes = 0; bytes < good.size(); ++bytes) {
        assert(!resources.decode(good.data(), bytes));
        assert(!resources.valid() && !resources.window(WindowId::Gpu));
    }
    for (auto entry : std::vector<std::pair<size_t, uint64_t>>{
             {0, 0}, {8, 2}, {16, 33}, {24, 5}, {32, 16384}, {40, 0},
             {48, 14}, {56, 0}, {64, 0}, {96, 0}, {128, 1000}, {136, 604},
             {200, 1}, {204, 3}, {208, 0xfffffffffffff000}, {232, 0x48000000}}) {
        auto bad = good;
        put(bad, entry.first, entry.second, entry.first == 200 || entry.first == 204 ? 4 : 8);
        assert(!resources.decode(bad.data(), bad.size()));
        assert(!resources.valid());
    }
    assert(resources.decode(good.data(), good.size()));
    puts("PASS: production Mali resource decoder, limits/IRQ/digest/window checks; hardware_exercised=false");
    if (argc == 2) {
        std::ifstream stream(argv[1], std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
        if (!stream || !resources.decode(bytes.data(), bytes.size())) return 1;
        printf("Private resource descriptor: %zu bytes, CSF image %llu bytes; GPU not accessed\n", bytes.size(),
               static_cast<unsigned long long>(resources.firmwareBytes()));
    }
}
