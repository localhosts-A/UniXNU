// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliFirmware.h"
#include <assert.h>
#include <stdio.h>
#include <fstream>
#include <iterator>
#include <vector>
using namespace native_mali;

static void put(std::vector<uint8_t> &bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) bytes[offset + i] = value >> (8 * i);
}
static std::vector<uint8_t> fixture() {
    std::vector<uint8_t> bytes(128);
    put(bytes, 0, 0xc3f13a6e); bytes[4] = 3;
    put(bytes, 8, 0x12345678); put(bytes, 16, 56);
    put(bytes, 20, 32 << 8);
    put(bytes, 24, FirmwareImage::Read | FirmwareImage::Execute | FirmwareImage::Zero);
    put(bytes, 28, 0x800000); put(bytes, 32, 0x801000);
    put(bytes, 36, 64); put(bytes, 40, 128);
    bytes[44] = 'c'; bytes[45] = 0; put(bytes, 46, 0x800000);
    put(bytes, 52, 0x800004ff); // Unknown optional record with empty payload.
    return bytes;
}
static void rejection(FirmwareImage &image, const std::vector<uint8_t> &data) {
    assert(image.parse(data.data(), data.size()) != FirmwareResult::Ok);
    assert(!image.valid() && image.regionCount() == 0 && image.entryCount() == 0 && !image.data());
}
int main(int argc, char **argv) {
    FirmwareImage image;
    auto good = fixture();
    assert(image.parse(good.data(), good.size()) == FirmwareResult::Ok);
    assert(image.versionHash() == 0x12345678 && image.regionCount() == 1 && image.entryCount() == 2);
    const auto &region = image.regions()[0];
    assert(region.start == 0x800000 && region.end == 0x801000 && region.executionStart == 0x800000);
    assert(region.nameBytes == 1 && region.name[0] == 'c');
    for (size_t length = 0; length < good.size(); ++length) {
        assert(image.parse(good.data(), length) != FirmwareResult::Ok);
        assert(!image.valid());
    }
    auto bad = good; bad[4] = 4; rejection(image, bad);
    bad = good; put(bad, 16, 20); rejection(image, bad);
    bad = good; put(bad, 20, 0); rejection(image, bad);
    bad = good; put(bad, 20, 31 << 8); rejection(image, bad);
    bad = good; put(bad, 52, 0x000004ff); rejection(image, bad);
    bad = good; put(bad, 32, 0x7ff000); rejection(image, bad);
    bad = good; put(bad, 40, 129); rejection(image, bad);
    bad = good; put(bad, 24, 0x40); rejection(image, bad);
    bad = good; for (size_t i = 44; i < 48; ++i) bad[i] = 'x'; rejection(image, bad);
    assert(image.parse(good.data(), good.size()) == FirmwareResult::Ok);
    puts("PASS: production CSF image parser, bounds/version/optional-entry checks; hardware_exercised=false");
    if (argc == 2) {
        std::ifstream stream(argv[1], std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)), {});
        if (!stream || data.empty()) return 2;
        const auto result = image.parse(data.data(), data.size());
        printf("Private firmware container: result=%u error_offset=%u entries=%zu regions=%zu version=0x%x; GPU not accessed\n",
               unsigned(result), image.errorOffset(), image.entryCount(), image.regionCount(), image.versionHash());
        return result == FirmwareResult::Ok ? 0 : 1;
    }
}
