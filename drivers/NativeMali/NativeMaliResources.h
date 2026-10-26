// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace native_mali {
enum class WindowId : uint32_t {
    Gpu = 1, MfgTop = 2, PlatformSmmu = 3, PowerRpc = 4, Pll = 5,
    PllSc0 = 6, PllSc1 = 7, Sleep = 8, VcoreAo = 9, Hbvc = 10,
    CpuWrap = 11, ClockSystem = 12, Gpueb = 16, GpuebGpr = 17,
    GpuebConfig = 18, GpuebIntc = 19, GpuebDma = 20, Mbox0 = 21,
    MboxInit = 22, MboxSet = 23, MboxClear = 24, MboxSend = 25,
    MboxReceive = 26, Mfg0Power = 27, GhpmEco = 32, SpmMfg0Power = 33,
    GpuebClock = 34, GpuebClockSet = 35, GpuebClockClear = 36, GpuebShared = 128
};
enum class WindowKind : uint32_t { Device = 1, ReservedMemory = 2 };
struct ResourceWindow {
    WindowId id{};
    WindowKind kind{};
    uint64_t base = 0, bytes = 0;
};
class Resources {
public:
    static constexpr uint64_t Magic = 0x494c414d494c4144;
    static constexpr size_t HeaderBytes = 128, Interrupts = 6, MaxWindows = 32;
    bool decode(const void *, size_t);
    bool valid() const { return valid_; }
    const ResourceWindow *window(WindowId) const;
    uint32_t spi(size_t index) const { return valid_ && index < Interrupts ? spi_[index] : 0; }
    uint64_t firmwareBytes() const { return valid_ ? firmwareBytes_ : 0; }
    const uint8_t *fdtDigest() const { return valid_ ? fdtDigest_ : nullptr; }
    const uint8_t *firmwareDigest() const { return valid_ ? firmwareDigest_ : nullptr; }
private:
    ResourceWindow windows_[MaxWindows]{};
    size_t count_ = 0;
    uint32_t spi_[Interrupts]{};
    uint8_t fdtDigest_[32]{}, firmwareDigest_[32]{};
    uint64_t firmwareBytes_ = 0;
    bool valid_ = false;
};
} // namespace native_mali
