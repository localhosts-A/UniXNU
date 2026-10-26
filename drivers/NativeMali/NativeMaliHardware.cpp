// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliHardware.h"
namespace native_mali {
bool Hardware::read(uint32_t off, uint32_t *v) {
    return !(off & 3) && off < aperture_ && ops_.read(ops_.cookie, off, v);
}
bool Hardware::write(uint32_t off, uint32_t v) {
    return !(off & 3) && off < aperture_ && ops_.write(ops_.cookie, off, v);
}
bool Hardware::read64(uint32_t off, uint64_t *v) {
    uint32_t lo = 0, hi = 0;
    if (!read(off, &lo) || !read(off + 4, &hi)) return false;
    *v = uint64_t(hi) << 32 | lo; return true;
}
bool Hardware::write64(uint32_t off, uint64_t v) { return write(off, uint32_t(v)) && write(off + 4, uint32_t(v >> 32)); }
HardwareResult Hardware::attach(const RegisterOps &ops, size_t aperture) {
    if (attached_) return HardwareResult::State;
    if (!ops.read || !ops.write || !ops.delayUs || aperture < 0x200000 || aperture > 0x1000000)
        return HardwareResult::Invalid;
    ops_ = ops; aperture_ = aperture; properties_ = {};
    auto &p = properties_;
    if (!read(0, &p.gpuId) || !read(4, &p.l2Features) || !read(8, &p.coreFeatures) ||
        !read(0xc, &p.tilerFeatures) || !read(0x10, &p.memFeatures) || !read(0x14, &p.mmuFeatures) ||
        !read(0x18, &p.addressSpaces) || !read64(0x100, &p.shaders) ||
        !read64(0x110, &p.tilers) || !read64(0x120, &p.l2Slices) ||
        !read(0xa0, &p.maxThreads) || !read(0xa4, &p.maxWorkgroup) ||
        !read(0xac, &p.threadFeatures) || !read(0x310, &p.tlsAlloc)) return HardwareResult::Access;
    for (size_t i = 0; i < 4; ++i) if (!read(0xb0 + i * 4, p.textureFeatures + i)) return HardwareResult::Access;
    p.virtualBits = p.mmuFeatures & 0xff; p.physicalBits = (p.mmuFeatures >> 8) & 0xff;
    // Product fields only; revision/status bits are reported without inventing
    // a revision from Android's expanded human-readable product identifier.
    if ((p.gpuId & 0xffff0000) != 0xd8300000 || !p.shaders || !p.l2Slices ||
        (p.addressSpaces & 3) != 3 || (p.addressSpaces & 0xffff0000) ||
        p.virtualBits < 32 || p.virtualBits > 48 || p.physicalBits < 32 || p.physicalBits > 48)
        return HardwareResult::Unsupported;
    attached_ = true; return HardwareResult::Ok;
}
HardwareResult Hardware::wait(uint32_t off, uint32_t mask, uint32_t expected, uint32_t us) {
    for (uint32_t elapsed = 0; ; ++elapsed) {
        uint32_t value = 0;
        if (!read(off, &value)) return HardwareResult::Access;
        if ((value & mask) == expected) return HardwareResult::Ok;
        if (elapsed == us) return HardwareResult::Timeout;
        ops_.delayUs(ops_.cookie, 1);
    }
}
HardwareResult Hardware::softReset(uint32_t us) {
    if (!attached_) return HardwareResult::State;
    if (!timeoutValid(us)) return HardwareResult::Invalid;
    reset_ = false; l2_ = false;
    // Leave running_ set on a failed reset: the caller must retain all backing
    // memory until hardware has actually stopped, including on an I/O failure.
    if (!write(0x28, 0) || !write(0x1008, 0) || !write(0x2008, 0) ||
        !write(0x24, 0xffffffff) || !write(0x30, 0x101)) return HardwareResult::Access;
    const auto result = wait(0x20, 1u << 8, 1u << 8, us);
    if (result != HardwareResult::Ok) return result;
    if (!write(0x24, 1u << 8) || !write(0x1004, 0xffffffff) || !write(0x2004, 0xffffffff))
        return HardwareResult::Access;
    // AUTO may survive some reset paths. Explicitly disable the MCU and require
    // its disabled status before allowing DMA memory to be reclaimed.
    if (!write(0x700,0)) return HardwareResult::Access;
    const auto stopped = wait(0x704,0xffffffff,0,us);
    if (stopped != HardwareResult::Ok) return stopped;
    configured_ = touched_ = 0; running_ = false; reset_ = true; return HardwareResult::Ok;
}
HardwareResult Hardware::powerL2(uint32_t us) {
    if (!reset_ || running_) return HardwareResult::State;
    if (!timeoutValid(us)) return HardwareResult::Invalid;
    if (!write64(0x1a0, properties_.l2Slices)) return HardwareResult::Access;
    auto result = wait(0x160, uint32_t(properties_.l2Slices), uint32_t(properties_.l2Slices), us);
    if (result == HardwareResult::Ok)
        result = wait(0x164, uint32_t(properties_.l2Slices >> 32), uint32_t(properties_.l2Slices >> 32), us);
    if (result != HardwareResult::Ok) return result;
    if (!write64(0x190, properties_.tilers)) return HardwareResult::Access;
    result = wait(0x150, uint32_t(properties_.tilers), uint32_t(properties_.tilers), us);
    if (result == HardwareResult::Ok)
        result = wait(0x154, uint32_t(properties_.tilers >> 32), uint32_t(properties_.tilers >> 32), us);
    l2_ = result == HardwareResult::Ok; return result;
}
HardwareResult Hardware::command(uint8_t as, uint32_t cmd, uint32_t us) {
    const uint32_t base = 0x2400 + uint32_t(as) * 0x40;
    auto result = wait(base + 0x28, 1, 0, us);
    if (result != HardwareResult::Ok) return result;
    if (!write(base + 0x18, cmd)) return HardwareResult::Access;
    return wait(base + 0x28, 1, 0, us);
}
HardwareResult Hardware::configureAddressSpace(uint8_t as, uint64_t root, uint32_t us) {
    if (!l2_) return HardwareResult::State;
    if (!timeoutValid(us) || as >= 16 || !(properties_.addressSpaces & (1u << as)) ||
        !root || (root & 4095) || (root >> properties_.physicalBits)) return HardwareResult::Invalid;
    if (touched_ & (1u << as)) return HardwareResult::State;
    // Even a failed bus write can have taken effect. Keep the AS occupied until
    // it has been disabled or a reset has completed.
    touched_ |= 1u << as;
    const uint32_t base = 0x2400 + uint32_t(as) * 0x40;
    if (!write64(base + 0x30, 0x42000006) || !write64(base, root) || !write64(base + 8, Memattr))
        return HardwareResult::Access;
    auto result = command(as, 1, us);
    if (result == HardwareResult::Ok) configured_ |= 1u << as;
    return result;
}
HardwareResult Hardware::flushCaches(uint32_t us) {
    if (!write(0x24, 1u << 17) || !write(0x30, 0x3304)) return HardwareResult::Access;
    auto result = wait(0x20, 1u << 17, 1u << 17, us);
    if (result == HardwareResult::Ok && !write(0x24, 1u << 17)) return HardwareResult::Access;
    return result;
}
HardwareResult Hardware::synchronizeMemory(uint32_t us) {
    if (!l2_) return HardwareResult::State;
    return timeoutValid(us) ? flushCaches(us) : HardwareResult::Invalid;
}
HardwareResult Hardware::synchronizeAddressSpace(uint8_t as, uint32_t us) {
    if (!l2_ || as >= 16 || !(touched_ & (1u << as))) return HardwareResult::State;
    if (!timeoutValid(us)) return HardwareResult::Invalid;
    const uint32_t base = 0x2400 + uint32_t(as) * 0x40;
    if (!write64(base + 0x10, properties_.virtualBits - 1)) return HardwareResult::Access;
    auto result = command(as, 2, us);
    if (result != HardwareResult::Ok) return result;
    result = flushCaches(us);
    const auto unlock = command(as, 3, us);
    return result == HardwareResult::Ok ? unlock : result;
}
HardwareResult Hardware::disableAddressSpace(uint8_t as, uint32_t us) {
    if (as == 0 && running_) return HardwareResult::State;
    auto result = synchronizeAddressSpace(as, us);
    if (result != HardwareResult::Ok) return result;
    const uint32_t base = 0x2400 + uint32_t(as) * 0x40;
    if (!write64(base + 0x30, 0x42000001) || !write64(base, 0)) return HardwareResult::Access;
    result = command(as, 1, us);
    if (result == HardwareResult::Ok) { configured_ &= ~(1u << as); touched_ &= ~(1u << as); }
    return result;
}
HardwareResult Hardware::bootMcu(uint32_t us) {
    if (!l2_ || running_ || !(configured_ & 1)) return HardwareResult::State;
    if (!timeoutValid(us)) return HardwareResult::Invalid;
    if (!write(0x1004, GlobalInterrupt) || !write(0x1008, GlobalInterrupt))
        return HardwareResult::Access;
    running_ = true; reset_ = false;
    if (!write(0x700, 2)) return HardwareResult::Access;
    for (uint32_t elapsed = 0; ; ++elapsed) {
        HardwareFault f{};
        if (fault(&f) != HardwareResult::Ok) return HardwareResult::Access;
        if ((f.gpuIrq & 0x81) || f.mmuIrq) return HardwareResult::Fault;
        if (f.jobIrq & GlobalInterrupt) return HardwareResult::Ok;
        if (elapsed == us) return HardwareResult::Timeout;
        ops_.delayUs(ops_.cookie, 1);
    }
}
HardwareResult Hardware::haltMcu(uint32_t us) {
    if (!attached_ || !running_) return HardwareResult::State;
    if (!timeoutValid(us)) return HardwareResult::Invalid;
    if (!write(0x700, 0)) return HardwareResult::Access;
    const auto result = wait(0x704, 0xffffffff, 0, us);
    if (result == HardwareResult::Ok) running_ = false;
    return result;
}
HardwareResult Hardware::ringDoorbell(uint8_t index) {
    if (!running_) return HardwareResult::State;
    if (index >= 24) return HardwareResult::Invalid;
    return write(0x80000 + uint32_t(index) * 0x10000, 1) ? HardwareResult::Ok : HardwareResult::Access;
}
HardwareResult Hardware::acknowledgeJob(uint32_t mask) {
    if (!running_) return HardwareResult::State;
    return write(0x1004, mask) ? HardwareResult::Ok : HardwareResult::Access;
}
HardwareResult Hardware::fault(HardwareFault *f) {
    if (!attached_) return HardwareResult::State;
    if (!f) return HardwareResult::Invalid;
    *f = {};
    return read(0x20, &f->gpuIrq) && read(0x1000, &f->jobIrq) && read(0x2000, &f->mmuIrq) &&
           read(0x3c, &f->gpuStatus) && read64(0x40, &f->gpuAddress) ? HardwareResult::Ok : HardwareResult::Access;
}
HardwareResult Hardware::latestFlush(uint32_t *v) {
    if (!attached_) return HardwareResult::State;
    if (!v) return HardwareResult::Invalid;
    return read(0x10000, v) ? HardwareResult::Ok : HardwareResult::Access;
}
HardwareResult Hardware::timestamp(uint64_t *v) {
    if (!attached_) return HardwareResult::State;
    if (!v) return HardwareResult::Invalid;
    *v = 0;
    for (size_t attempt = 0; attempt < 3; ++attempt) {
        uint32_t hi = 0, lo = 0, again = 0;
        if (!read(0x9c, &hi) || !read(0x98, &lo) || !read(0x9c, &again)) return HardwareResult::Access;
        if (hi == again) { *v = uint64_t(hi) << 32 | lo; return HardwareResult::Ok; }
    }
    return HardwareResult::Busy;
}
} // namespace native_mali
