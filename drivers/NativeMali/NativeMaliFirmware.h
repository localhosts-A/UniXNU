// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace native_mali {
enum class FirmwareResult : uint32_t { Ok, Header, Version, Truncated, Entry, Unsupported, Region, Capacity };
struct FirmwareRegion {
    uint32_t flags = 0, start = 0, end = 0, fileStart = 0, fileEnd = 0, executionStart = 0;
    const char *name = nullptr;
    size_t nameBytes = 0;
};
struct FirmwareEntry {
    uint8_t type = 0;
    bool optional = false, updatable = false;
    const uint8_t *payload = nullptr;
    uint32_t payloadBytes = 0;
};

// Structural container validation and memory-layout inventory. Parsing does not
// allocate GPU memory or boot the MCU. Firmware authenticity/version binding and
// configuration/trace metadata handling belong to the driver boot lifecycle.
// The caller owns the immutable image for the lifetime of the parsed view.
class FirmwareImage {
public:
    static constexpr size_t MaxEntries = 128, MaxRegions = 64;
    static constexpr uint32_t Read = 1, Write = 2, Execute = 4, CacheMask = 24,
                              Protected = 32, Shared = uint32_t(1) << 30, Zero = uint32_t(1) << 31;
    FirmwareResult parse(const void *, size_t);
    bool valid() const { return valid_; }
    size_t regionCount() const { return valid_ ? regionCount_ : 0; }
    size_t entryCount() const { return valid_ ? entryCount_ : 0; }
    const FirmwareRegion *regions() const { return valid_ ? regions_ : nullptr; }
    const FirmwareEntry *entries() const { return valid_ ? entries_ : nullptr; }
    uint32_t versionHash() const { return valid_ ? versionHash_ : 0; }
    uint32_t errorOffset() const { return errorOffset_; }
    const uint8_t *data() const { return valid_ ? data_ : nullptr; }
    size_t bytes() const { return valid_ ? bytes_ : 0; }
private:
    FirmwareRegion regions_[MaxRegions]{};
    FirmwareEntry entries_[MaxEntries]{};
    size_t regionCount_ = 0, entryCount_ = 0, bytes_ = 0;
    uint32_t versionHash_ = 0, errorOffset_ = 0;
    const uint8_t *data_ = nullptr;
    bool valid_ = false;
    FirmwareResult fail(FirmwareResult, uint32_t);
};
} // namespace native_mali
