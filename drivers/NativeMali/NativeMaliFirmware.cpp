// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliFirmware.h"

namespace native_mali {
static uint32_t little32(const uint8_t *at)
{
    return uint32_t(at[0]) | (uint32_t(at[1]) << 8) | (uint32_t(at[2]) << 16) | (uint32_t(at[3]) << 24);
}
FirmwareResult FirmwareImage::fail(FirmwareResult result, uint32_t offset)
{
    valid_ = false; data_ = nullptr; bytes_ = 0; regionCount_ = 0; entryCount_ = 0;
    versionHash_ = 0; errorOffset_ = offset;
    return result;
}
FirmwareResult FirmwareImage::parse(const void *image, size_t bytes)
{
    fail(FirmwareResult::Header, 0);
    if (!image || bytes < 20 || bytes > 16 * 1024 * 1024) return FirmwareResult::Header;
    const auto *data = static_cast<const uint8_t *>(image);
    if (little32(data) != 0xc3f13a6e) return FirmwareResult::Header;
    if (data[4] != 3 || data[5] != 0) return FirmwareResult::Version;
    const uint32_t tableEnd = little32(data + 16);
    if (tableEnd < 20 || tableEnd > bytes || (tableEnd & 3)) return FirmwareResult::Truncated;
    uint32_t offset = 20;
    while (offset < tableEnd) {
        if (tableEnd - offset < 4) return fail(FirmwareResult::Truncated, offset);
        const uint32_t header = little32(data + offset);
        const uint32_t length = (header >> 8) & 255;
        const uint8_t type = header & 255;
        if (length < 4 || (length & 3) || length > tableEnd - offset || (header & 0x3fff0000))
            return fail(FirmwareResult::Entry, offset);
        if (entryCount_ == MaxEntries) return fail(FirmwareResult::Capacity, offset);
        const FirmwareEntry entry{type, bool(header >> 31), bool(header & (uint32_t(1) << 30)),
                                  data + offset + 4, length - 4};
        if (type == 0) {
            if (regionCount_ == MaxRegions) return fail(FirmwareResult::Capacity, offset);
            if (entry.payloadBytes < 28) return fail(FirmwareResult::Region, offset);
            FirmwareRegion region{};
            region.flags = little32(entry.payload);
            region.start = little32(entry.payload + 4);
            region.end = little32(entry.payload + 8);
            region.fileStart = little32(entry.payload + 12);
            region.fileEnd = little32(entry.payload + 16);
            if ((region.flags & ~(Read | Write | Execute | CacheMask | Protected | Shared | Zero)) ||
                region.end <= region.start || ((region.start | region.end) & 4095) ||
                region.fileEnd < region.fileStart || region.fileEnd > bytes ||
                uint64_t(region.fileEnd - region.fileStart) > uint64_t(region.end - region.start))
                return fail(FirmwareResult::Region, offset);
            const uint8_t *name = entry.payload + 20;
            size_t count = 0;
            while (count < entry.payloadBytes - 24 && name[count]) ++count;
            if (count == entry.payloadBytes - 24) return fail(FirmwareResult::Region, offset);
            region.name = reinterpret_cast<const char *>(name);
            region.nameBytes = count;
            region.executionStart = little32(name + count + 1);
            for (size_t i = 0; i < regionCount_; ++i) {
                if (region.start < regions_[i].end && regions_[i].start < region.end)
                    return fail(FirmwareResult::Region, offset);
            }
            regions_[regionCount_++] = region;
        } else if (type != 1 && type != 3 && type != 4 && type != 6 && type != 7 && type != 9 && !entry.optional) {
            return fail(FirmwareResult::Unsupported, offset);
        }
        entries_[entryCount_++] = entry;
        offset += length;
    }
    if (!regionCount_) return fail(FirmwareResult::Region, 20);
    data_ = data; bytes_ = bytes; versionHash_ = little32(data + 8); errorOffset_ = 0; valid_ = true;
    return FirmwareResult::Ok;
}
} // namespace native_mali
