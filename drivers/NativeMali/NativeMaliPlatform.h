// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliResources.h"
namespace native_mali {
struct PlatformIo {
    void *cookie = nullptr;
    bool (*read)(void *, WindowId, uint32_t offset, uint32_t *) = nullptr;
    bool (*write)(void *, WindowId, uint32_t offset, uint32_t) = nullptr;
    void (*barrier)(void *) = nullptr;
    void (*delayUs)(void *, uint32_t) = nullptr;
    // This is owned reserved Normal memory, never a Device MMIO mapping.
    bool (*zeroShared)(void *, size_t offset, size_t bytes) = nullptr;
};
enum class PlatformResult : uint32_t { Ok, State, Invalid, Access, Busy, Timeout, Protocol, Unsupported, Firmware };
class Mailbox {
public:
    PlatformResult attach(const PlatformIo &);
    PlatformResult exchange(const uint32_t request[8], uint32_t reply[8], uint32_t timeoutUs);
    PlatformResult suspend(uint32_t offState, uint32_t timeoutUs);
    bool poisoned() const { return poisoned_; }
private:
    PlatformIo io_{};
    bool attached_ = false, poisoned_ = false;
    PlatformResult send(uint8_t pin, uint16_t offset, const uint32_t *, size_t, bool reply, uint32_t us);
    PlatformResult wait(WindowId, uint32_t mask, uint32_t value, uint32_t us);
};

// MT6991's GPUEB/GHPM power path. References and protocol fields are pinned in
// the input inventory. The owner serializes calls and proves the entire GPUEB
// reservation is excluded from XNU allocation before attach. No inherited Linux
// state, Linux ioctl, or Linux shared-status structure is consumed.
class PlatformPower {
public:
    PlatformResult attach(const Resources &, const PlatformIo &, bool sharedReservationOwned);
    PlatformResult powerOn(uint32_t timeoutUs);
    // Only while GPU reset completion proves DMA is quiesced. The dedicated MFG
    // SMMUv3 is put in architected global bypass for the GPU MMU's PA outputs.
    PlatformResult establishDmaRoute(bool gpuQuiesced, uint32_t timeoutUs);
    PlatformResult powerOff(bool gpuQuiesced, uint32_t timeoutUs);
    bool powered() const { return powerReady_; }
    bool dmaReady() const { return dmaReady_; }
    bool ownsPowerState() const { return eb_ || gpu_ || active_ || uncertain(); }
    bool uncertain() const { return uncertain_ || mailbox_.poisoned(); }
private:
    PlatformIo io_{};
    const Resources *resources_ = nullptr;
    Mailbox mailbox_{};
    uint32_t magic_ = 0, eco_ = 0;
    bool eb_ = false, gpu_ = false, active_ = false, dmaReady_ = false, uncertain_ = false, powerReady_ = false;
    PlatformResult wake(uint32_t);
    PlatformResult wait(WindowId, uint32_t offset, uint32_t mask, uint32_t value, uint32_t us);
    PlatformResult update(WindowId, uint32_t offset, uint32_t clear, uint32_t set);
    PlatformResult command(uint32_t cmd, uint32_t state, uint32_t us, int32_t *result);
};
} // namespace native_mali
