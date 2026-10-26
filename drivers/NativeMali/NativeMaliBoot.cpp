// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliBoot.h"
#include "NativeMaliSha256.h"
namespace native_mali {
BootResult Boot::fail(BootResult failure, uint32_t detail) {
    failedStage_ = stage_; failure_ = failure; detail_ = detail;
    if (stop() != BootResult::Ok) return BootResult::Retained;
    stage_ = BootStage::Failed; return failure;
}
BootResult Boot::start(const BootConfig &c) {
    if (stage_ != BootStage::New) return BootResult::State;
    if (!c.firmware || !c.timeoutUs || c.timeoutUs > 1000000 || !c.arenaPages || c.arenaPages > 4096)
        return BootResult::Invalid;
    config_ = c; stage_ = BootStage::Identity;
    if (!resources_.decode(c.resources,c.resourceBytes) || resources_.firmwareBytes() != c.firmwareBytes)
        return fail(BootResult::Resources);
    uint8_t digest[32]{}; sha256(c.firmware,c.firmwareBytes,digest);
    uint8_t difference = 0;
    for (size_t i=0;i<32;++i) difference |= digest[i] ^ resources_.firmwareDigest()[i];
    if (difference) return fail(BootResult::Digest);
    const auto parsed = image_.parse(c.firmware,c.firmwareBytes);
    if (parsed != FirmwareResult::Ok) return fail(BootResult::Firmware,uint32_t(parsed));
    bool vectors=false, executable=false;
    for (size_t i=0;i<image_.regionCount();++i) {
        const auto &r=image_.regions()[i];
        if (r.flags & FirmwareImage::Protected) continue;
        if (!r.start && (r.flags & FirmwareImage::Read)) vectors=true;
        if ((r.flags & (FirmwareImage::Read|FirmwareImage::Execute)) == (FirmwareImage::Read|FirmwareImage::Execute)) executable=true;
    }
    if (!vectors || !executable) return fail(BootResult::Firmware);
    auto p = platform_.attach(resources_,c.platform,c.sharedReservationOwned);
    if (p != PlatformResult::Ok) return fail(BootResult::Platform,uint32_t(p));
    stage_ = BootStage::Power;
    if ((p = platform_.powerOn(c.timeoutUs)) != PlatformResult::Ok) return fail(BootResult::Platform,uint32_t(p));
    stage_ = BootStage::Reset;
    auto h = hardware_.attach(c.registers,resources_.window(WindowId::Gpu)->bytes);
    if (h != HardwareResult::Ok) return fail(BootResult::Hardware,uint32_t(h));
    if ((h = hardware_.softReset(c.timeoutUs)) != HardwareResult::Ok) return fail(BootResult::Hardware,uint32_t(h));
    stage_ = BootStage::DmaRoute;
    if ((p = platform_.establishDmaRoute(hardware_.quiesced(),c.timeoutUs)) != PlatformResult::Ok)
        return fail(BootResult::Platform,uint32_t(p));
    stage_ = BootStage::Memory;
    const auto *props = hardware_.properties();
    const auto m = mmu_.initialize(c.tables,props->physicalBits,props->virtualBits,true);
    if (m != MmuResult::Ok) return fail(BootResult::Memory,uint32_t(m));
    FirmwareMemoryPolicy policy{}; policy.maxPages = 8192;
    auto f = memory_.load(image_,mmu_,c.memory,policy);
    if (f != FirmwareMemoryResult::Ok) return fail(BootResult::Memory,uint32_t(f));
    stage_ = BootStage::Metadata;
    const auto setup = setup_.prepare(image_,memory_);
    if (setup != FirmwareSetupResult::Ok) return fail(BootResult::Metadata,uint32_t(setup));
    f = memory_.allocateShared(c.arenaPages,&arena_);
    if (f != FirmwareMemoryResult::Ok) return fail(BootResult::Memory,uint32_t(f));
    arenaBytes_ = c.arenaPages * 4096;
    if (!memory_.seal()) return fail(BootResult::Memory);
    stage_ = BootStage::AddressSpace;
    if ((h = hardware_.powerL2(c.timeoutUs)) != HardwareResult::Ok ||
        (h = hardware_.configureAddressSpace(0,mmu_.rootPhysical(),c.timeoutUs)) != HardwareResult::Ok)
        return fail(BootResult::Hardware,uint32_t(h));
    stage_ = BootStage::Mcu;
    if ((h = hardware_.bootMcu(c.timeoutUs)) != HardwareResult::Ok) return fail(BootResult::Hardware,uint32_t(h));
    stage_ = BootStage::Interface;
    CsfMemoryOps shared{}; shared.cookie = &memory_;
    shared.read = [](void *cookie,uint32_t address,uint32_t *value) { return static_cast<FirmwareMemory *>(cookie)->read32(address,value); };
    shared.write = [](void *cookie,uint32_t address,uint32_t value) { return static_cast<FirmwareMemory *>(cookie)->write32(address,value); };
    const auto iface = csf_.discover(shared,setup_.sharedBytes());
    if (iface != CsfResult::Ok) return fail(BootResult::Interface,uint32_t(iface));
    if ((h = hardware_.acknowledgeJob(Hardware::GlobalInterrupt)) != HardwareResult::Ok)
        return fail(BootResult::Hardware,uint32_t(h));
    stage_ = BootStage::Global;
    const auto global = initializeGlobal();
    if (global != BootResult::Ok) return fail(global,detail_);
    stage_ = BootStage::Ready; return BootResult::Ok;
}
BootResult Boot::initializeGlobal() {
    const uint64_t shaders = hardware_.properties()->shaders;
    const struct { uint32_t offset,value; } values[] = {
        {0x18,uint32_t(shaders)}, {0x1c,uint32_t(shaders >> 32)},
        {0x10,config_.progressTimer}, {0x14,config_.shaderPoweroffTimer}, {4,0x10e}
    };
    for (const auto &v : values) {
        const auto r = csf_.globalWrite(v.offset,v.value);
        if (r != CsfResult::Ok) { detail_ = uint32_t(r); return BootResult::Interface; }
    }
    auto r = csf_.globalRequest(0xe,0,true);
    if (r != CsfResult::Ok) { detail_ = uint32_t(r); return BootResult::Interface; }
    auto h = hardware_.ringDoorbell(0);
    if (h != HardwareResult::Ok) { detail_ = uint32_t(h); return BootResult::Hardware; }
    for (uint32_t elapsed=0; ; ++elapsed) {
        bool done=false;
        if ((r = csf_.globalAcknowledged(0xe,&done)) != CsfResult::Ok) {
            detail_ = uint32_t(r); return BootResult::Interface;
        }
        HardwareFault fault{};
        h = hardware_.fault(&fault);
        if (h != HardwareResult::Ok || (fault.gpuIrq & 0x81) || fault.mmuIrq) {
            detail_ = uint32_t(h == HardwareResult::Ok ? HardwareResult::Fault : h); return BootResult::Hardware;
        }
        if (done) break;
        if (elapsed == config_.timeoutUs) return BootResult::Timeout;
        config_.registers.delayUs(config_.registers.cookie,1);
    }
    h = hardware_.acknowledgeJob(Hardware::GlobalInterrupt);
    if (h != HardwareResult::Ok) { detail_ = uint32_t(h); return BootResult::Hardware; }
    return BootResult::Ok;
}
BootResult Boot::stop() {
    if (stage_ == BootStage::Stopped) return BootResult::Ok;
    csf_.reset();
    bool quiesced = false;
    if (hardware_.properties()) {
        if (!hardware_.quiesced() && hardware_.softReset(config_.timeoutUs) != HardwareResult::Ok) {
            stage_ = BootStage::Retained; return BootResult::Retained;
        }
        quiesced = hardware_.quiesced();
    }
    if (memory_.ownsStorage() && (!quiesced || memory_.unload(true) != FirmwareMemoryResult::Ok)) {
        stage_ = BootStage::Retained; return BootResult::Retained;
    }
    if (mmu_.rootPhysical() && mmu_.destroy(true) != MmuResult::Ok) {
        stage_ = BootStage::Retained; return BootResult::Retained;
    }
    arena_ = arenaBytes_ = 0;
    if (platform_.ownsPowerState() && platform_.powerOff(quiesced,config_.timeoutUs) != PlatformResult::Ok) {
        stage_ = BootStage::Retained; return BootResult::Retained;
    }
    stage_ = BootStage::Stopped; return BootResult::Ok;
}
} // namespace native_mali
