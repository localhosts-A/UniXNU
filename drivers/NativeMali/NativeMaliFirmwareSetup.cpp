// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliFirmwareSetup.h"
namespace native_mali {
static uint32_t word(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static bool name(const FirmwareEntry &entry, size_t offset) {
    for (size_t i = offset; i < entry.payloadBytes; ++i) if (!entry.payload[i]) return i > offset;
    return false;
}
FirmwareSetupResult FirmwareSetup::prepare(const FirmwareImage &image, FirmwareMemory &memory)
{
    if (attempted_) return FirmwareSetupResult::State;
    attempted_ = true;
    if (!image.valid() || !memory.prepared()) return FirmwareSetupResult::Invalid;
    for (size_t i = 0; i < image.regionCount(); ++i) {
        const auto &r = image.regions()[i];
        if (r.start != 0x04000000) continue;
        if (r.end > 0x08000000 || (r.flags & FirmwareImage::Protected) ||
            !memory.accessible(r.start, r.end - r.start, FirmwareImage::Read | FirmwareImage::Write | FirmwareImage::Shared))
            return FirmwareSetupResult::Layout;
        sharedBytes_ = r.end - r.start;
    }
    if (!sharedBytes_) return FirmwareSetupResult::Layout;
    // Validate every field before modifying firmware memory or allocating trace
    // backing. Configuration format is u32 address/min/max, not a u64 pointer.
    for (size_t i = 0; i < image.entryCount(); ++i) {
        errorEntry_ = uint32_t(i);
        const auto &entry = image.entries()[i];
        if (entry.type == 1) {
            if (entry.payloadBytes < 16 || !name(entry,12)) return FirmwareSetupResult::Layout;
            const uint32_t address = word(entry.payload), min = word(entry.payload + 4), max = word(entry.payload + 8);
            uint32_t value = 0;
            if ((address & 3) || min > max ||
                !memory.initializable(address,4) ||
                !memory.read32(address,&value) || value < min || value > max) return FirmwareSetupResult::Layout;
        } else if (entry.type == 3) {
            if (entry.payloadBytes < 32 || !name(entry,28)) return FirmwareSetupResult::Layout;
            if (word(entry.payload) > 1) {
                if (entry.optional) continue;
                return FirmwareSetupResult::Unsupported;
            }
            if (count_ == 16) return FirmwareSetupResult::Unsupported;
            auto &t = traces_[count_++];
            t.size = word(entry.payload + 4); t.insert = word(entry.payload + 8);
            t.extract = word(entry.payload + 12); t.data = word(entry.payload + 16);
            t.enable = word(entry.payload + 20); t.bits = word(entry.payload + 24);
            if (t.bits > 256) return FirmwareSetupResult::Unsupported;
            const uint32_t addresses[] = {t.size,t.insert,t.extract,t.data};
            for (uint32_t address : addresses) {
                if ((address & 3) || !memory.initializable(address,4))
                    return FirmwareSetupResult::Layout;
            }
            if (t.bits && ((t.enable & 3) ||
                !memory.initializable(t.enable,((t.bits + 31) / 32) * 4)))
                return FirmwareSetupResult::Layout;
        }
    }
    errorEntry_ = 0xffffffff;
    uint32_t variables = 0;
    if (count_ && memory.allocateShared(1,&variables) != FirmwareMemoryResult::Ok)
        return FirmwareSetupResult::Allocation;
    for (size_t i = 0; i < count_; ++i) {
        auto &t = traces_[i]; uint32_t data = 0;
        if (memory.allocateShared(4,&data) != FirmwareMemoryResult::Ok) return FirmwareSetupResult::Allocation;
        // Keep each producer/consumer variable on a separate 64-byte line.
        if (!memory.initialize32(t.data,data) || !memory.initialize32(t.insert,variables + i * 128) ||
            !memory.initialize32(t.extract,variables + i * 128 + 64) || !memory.initialize32(t.size,4 * 4096))
            return FirmwareSetupResult::Access;
        for (size_t j = 0; j < (t.bits + 31) / 32; ++j)
            if (!memory.initialize32(t.enable + j * 4,0)) return FirmwareSetupResult::Access;
    }
    errorEntry_ = 0xffffffff; prepared_ = true; return FirmwareSetupResult::Ok;
}
} // namespace native_mali
