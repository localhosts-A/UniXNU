// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace native_mali {
struct RegisterOps {
    void *cookie = nullptr;
    bool (*read)(void *, uint32_t offset, uint32_t *) = nullptr;
    bool (*write)(void *, uint32_t offset, uint32_t) = nullptr;
    void (*delayUs)(void *, uint32_t) = nullptr;
};
struct HardwareProperties {
    uint32_t gpuId = 0, l2Features = 0, coreFeatures = 0, tilerFeatures = 0;
    uint32_t memFeatures = 0, mmuFeatures = 0, addressSpaces = 0;
    uint32_t textureFeatures[4]{}, maxThreads = 0, maxWorkgroup = 0, threadFeatures = 0, tlsAlloc = 0;
    uint64_t shaders = 0, tilers = 0, l2Slices = 0;
    uint8_t physicalBits = 0, virtualBits = 0;
};
struct HardwareFault {
    uint32_t gpuIrq = 0, jobIrq = 0, mmuIrq = 0, gpuStatus = 0;
    uint64_t gpuAddress = 0;
};
enum class HardwareResult : uint32_t { Ok, State, Invalid, Access, Unsupported, Timeout, Fault, Busy };

// G925 CSF register protocol, serialized by the owning device lock. The platform
// must qualify power and SMMU routing before attach; this class never powers the
// SoC or assumes inherited translations. Its delays/waits are strictly bounded.
class Hardware {
public:
    static constexpr uint64_t Memattr = 0x00084c8d888d8f88ull;
    static constexpr uint32_t GlobalInterrupt = uint32_t(1) << 31;
    HardwareResult attach(const RegisterOps &, size_t apertureBytes);
    const HardwareProperties *properties() const { return attached_ ? &properties_ : nullptr; }
    HardwareResult softReset(uint32_t timeoutUs);
    HardwareResult powerL2(uint32_t timeoutUs);
    HardwareResult configureAddressSpace(uint8_t index, uint64_t root, uint32_t timeoutUs);
    // Caller must stop all users of this AS before mutation or teardown. Flush
    // covers the whole AS; no deprecated AS FLUSH_MEM command is used on G925.
    HardwareResult synchronizeAddressSpace(uint8_t index, uint32_t timeoutUs);
    HardwareResult disableAddressSpace(uint8_t index, uint32_t timeoutUs);
    HardwareResult bootMcu(uint32_t timeoutUs);
    HardwareResult haltMcu(uint32_t timeoutUs);
    HardwareResult ringDoorbell(uint8_t index);
    HardwareResult acknowledgeJob(uint32_t mask);
    HardwareResult fault(HardwareFault *);
    HardwareResult latestFlush(uint32_t *);
    HardwareResult timestamp(uint64_t *);
    HardwareResult synchronizeMemory(uint32_t timeoutUs);
    bool mcuRunning() const { return running_; }
    bool quiesced() const { return reset_ && !running_; }
private:
    RegisterOps ops_{};
    HardwareProperties properties_{};
    size_t aperture_ = 0;
    uint32_t configured_ = 0, touched_ = 0;
    bool attached_ = false, reset_ = false, l2_ = false, running_ = false;
    bool read(uint32_t, uint32_t *);
    bool write(uint32_t, uint32_t);
    bool read64(uint32_t, uint64_t *);
    bool write64(uint32_t, uint64_t);
    static bool timeoutValid(uint32_t us) { return us && us <= 1000000; }
    HardwareResult wait(uint32_t offset, uint32_t mask, uint32_t expected, uint32_t us);
    HardwareResult command(uint8_t as, uint32_t cmd, uint32_t us);
    HardwareResult flushCaches(uint32_t us);
};
} // namespace native_mali
