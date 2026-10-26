// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliCsf.h"
#include "NativeMaliFirmwareSetup.h"
#include "NativeMaliHardware.h"
#include "NativeMaliPlatform.h"
namespace native_mali {
struct BootConfig {
    const void *resources = nullptr; size_t resourceBytes = 0;
    const uint8_t *firmware = nullptr; size_t firmwareBytes = 0;
    PlatformIo platform{}; RegisterOps registers{};
    TableOps tables{}; FirmwareMemoryOps memory{};
    bool sharedReservationOwned = false;
    uint32_t timeoutUs = 100000, arenaPages = 4096;
    // Firmware register values. Driver-side operation deadlines remain bounded
    // independently of the progress timer. Zero disables shader poweroff.
    uint32_t progressTimer = 0xffffffff, shaderPoweroffTimer = 0;
};
enum class BootStage : uint32_t { New, Identity, Power, Reset, DmaRoute, Memory, Metadata, AddressSpace, Mcu, Interface, Global, Ready, Stopped, Failed, Retained };
enum class BootResult : uint32_t { Ok, State, Invalid, Resources, Firmware, Digest, Platform, Hardware, Memory, Metadata, Interface, Timeout, Retained };
// One boot epoch. Failed hardware quiescence retains every owned allocation;
// the IOService must retain this object and its callback owner in that case.
class Boot {
public:
    BootResult start(const BootConfig &);
    BootResult stop();
    bool ready() const { return stage_ == BootStage::Ready; }
    BootStage stage() const { return stage_; }
    BootStage failedStage() const { return failedStage_; }
    uint32_t detail() const { return detail_; }
    BootResult failure() const { return failure_; }
    const HardwareProperties *properties() const { return ready() ? hardware_.properties() : nullptr; }
    CsfInterface &interface() { return csf_; }
    Hardware &hardware() { return hardware_; }
    FirmwareMemory &firmwareMemory() { return memory_; }
    uint32_t arenaAddress() const { return ready() ? arena_ : 0; }
    uint32_t arenaBytes() const { return ready() ? arenaBytes_ : 0; }
private:
    Resources resources_{};
    PlatformPower platform_{};
    Hardware hardware_{};
    FirmwareImage image_{};
    Mmu mmu_{};
    FirmwareMemory memory_{};
    FirmwareSetup setup_{};
    CsfInterface csf_{};
    BootConfig config_{};
    BootStage stage_ = BootStage::New, failedStage_ = BootStage::New;
    BootResult failure_ = BootResult::Ok;
    uint32_t detail_ = 0, arena_ = 0, arenaBytes_ = 0;
    BootResult fail(BootResult, uint32_t detail = 0);
    BootResult initializeGlobal();
};
} // namespace native_mali
