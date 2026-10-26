// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliResources.h"
namespace native_mali {
static uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
static uint64_t le64(const uint8_t *p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }
bool Resources::decode(const void *raw, size_t bytes)
{
    valid_ = false; count_ = 0; firmwareBytes_ = 0;
    if (!raw || bytes < HeaderBytes + Interrupts * 8) return false;
    const auto *p = static_cast<const uint8_t *>(raw);
    if (le64(p) != Magic || le64(p + 8) != 1 || !le64(p + 16) || le64(p + 16) > MaxWindows ||
        le64(p + 24) != Interrupts || le64(p + 32) != 4096 || le64(p + 40) < 20 ||
        le64(p + 40) > 16 * 1024 * 1024 || le64(p + 48) != 13 || le64(p + 56) != 1)
        return false;
    const size_t count = le64(p + 16);
    if (bytes != HeaderBytes + Interrupts * 8 + count * 24) return false;
    uint8_t fdtNonzero = 0, firmwareNonzero = 0;
    for (size_t i = 0; i < 32; ++i) {
        fdtNonzero |= p[64 + i]; firmwareNonzero |= p[96 + i];
        fdtDigest_[i] = p[64 + i]; firmwareDigest_[i] = p[96 + i];
    }
    if (!fdtNonzero || !firmwareNonzero) return false;
    for (size_t i = 0; i < Interrupts; ++i) {
        const uint64_t value = le64(p + HeaderBytes + i * 8);
        if (value > 987) return false; // GIC SPI + 32 must remain below reserved INTIDs.
        for (size_t j = 0; j < i; ++j) if (value == spi_[j]) return false;
        spi_[i] = uint32_t(value);
    }
    const uint8_t *records = p + HeaderBytes + Interrupts * 8;
    bool gpu = false;
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *record = records + i * 24;
        const uint32_t tag = le32(record), kind = le32(record + 4);
        const uint64_t base = le64(record + 8), length = le64(record + 16);
        if (!tag || tag > 255 || (kind != 1 && kind != 2) || !base || !length ||
            ((base | length) & 3) || base >= (uint64_t(1) << 48) ||
            length > (uint64_t(1) << 48) - base) return false;
        if (kind == 2 && ((base | length) & 4095)) return false;
        for (size_t j = 0; j < i; ++j) {
            if (uint32_t(windows_[j].id) == tag) return false;
            // Device subwindows may intentionally overlap. A RAM reservation
            // must never be interpreted as a Device-nGnRnE register window.
            if (uint32_t(windows_[j].kind) != kind && base < windows_[j].base + windows_[j].bytes &&
                windows_[j].base < base + length) return false;
        }
        if (tag == uint32_t(WindowId::Gpu)) {
            if (kind != 1 || (base & 4095) || length < 4096 || length > 16 * 1024 * 1024) return false;
            gpu = true;
        }
        windows_[i] = {WindowId(tag), WindowKind(kind), base, length};
    }
    if (!gpu) return false;
    count_ = count; firmwareBytes_ = le64(p + 40); valid_ = true;
    return true;
}
const ResourceWindow *Resources::window(WindowId id) const
{
    if (!valid_) return nullptr;
    for (size_t i = 0; i < count_; ++i) if (windows_[i].id == id) return windows_ + i;
    return nullptr;
}
} // namespace native_mali
