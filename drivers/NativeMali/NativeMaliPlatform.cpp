// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliPlatform.h"
namespace native_mali {
static bool timeout(uint32_t us) { return us && us <= 1000000; }
PlatformResult Mailbox::attach(const PlatformIo &io) {
    if (attached_) return PlatformResult::State;
    if (!io.read || !io.write || !io.barrier || !io.delayUs) return PlatformResult::Invalid;
    io_ = io; attached_ = true; return PlatformResult::Ok;
}
PlatformResult Mailbox::wait(WindowId id, uint32_t mask, uint32_t expected, uint32_t us) {
    for (uint32_t elapsed = 0; ; ++elapsed) {
        uint32_t value = 0;
        if (!io_.read(io_.cookie,id,0,&value)) return PlatformResult::Access;
        if ((value & mask) == expected) return PlatformResult::Ok;
        if (elapsed == us) return PlatformResult::Timeout;
        io_.delayUs(io_.cookie,1);
    }
}
PlatformResult Mailbox::send(uint8_t pin, uint16_t offset, const uint32_t *words, size_t count, bool reply, uint32_t us) {
    if (!attached_ || poisoned_) return PlatformResult::State;
    if (!timeout(us) || !words || !count || pin >= 32 || size_t(offset) + count > 160) return PlatformResult::Invalid;
    const uint32_t mask = 1u << pin;
    uint32_t pending = 0;
    if (!io_.read(io_.cookie,WindowId::MboxSend,0,&pending)) return PlatformResult::Access;
    if (pending & mask) return PlatformResult::Busy;
    if (reply) {
        if (!io_.read(io_.cookie,WindowId::MboxReceive,0,&pending)) return PlatformResult::Access;
        if (pending & mask) return PlatformResult::Busy; // Never attribute an old reply to a new command.
    }
    for (size_t i = 0; i < count; ++i)
        if (!io_.write(io_.cookie,WindowId::Mbox0,(offset + i) * 4,words[i])) return PlatformResult::Access;
    io_.barrier(io_.cookie);
    poisoned_ = true; // Any failure from this point can leave a late reply.
    if (!io_.write(io_.cookie,WindowId::MboxSet,0,mask)) return PlatformResult::Access;
    const auto result = wait(reply ? WindowId::MboxReceive : WindowId::MboxSend,mask,reply ? mask : 0,us);
    if (result != PlatformResult::Ok) return result;
    if (!reply) poisoned_ = false;
    return PlatformResult::Ok;
}
PlatformResult Mailbox::exchange(const uint32_t request[8], uint32_t reply[8], uint32_t us) {
    if (!request || !reply) return PlatformResult::Invalid;
    const auto result = send(1,4,request,8,true,us);
    if (result != PlatformResult::Ok) return result;
    uint32_t received[8]{};
    // The producer holds the payload until this channel's next request. Match
    // the MTK polling protocol: W1C the receive bit, then acquire the payload.
    if (!io_.write(io_.cookie,WindowId::MboxClear,0,2)) return PlatformResult::Access;
    io_.barrier(io_.cookie);
    for (size_t i = 0; i < 8; ++i)
        if (!io_.read(io_.cookie,WindowId::Mbox0,(60 + i) * 4,received + i)) return PlatformResult::Access;
    if (received[1] != request[1]) return PlatformResult::Protocol;
    for (size_t i = 0; i < 8; ++i) reply[i] = received[i];
    poisoned_ = false; return PlatformResult::Ok;
}
PlatformResult Mailbox::suspend(uint32_t offState, uint32_t us) {
    if (offState > 1) return PlatformResult::Invalid;
    const uint32_t words[] = {0,offState,0x55667788};
    return send(2,12,words,3,false,us);
}
PlatformResult PlatformPower::attach(const Resources &resources, const PlatformIo &io, bool owned) {
    if (resources_) return PlatformResult::State;
    if (!owned || !resources.valid() || !io.zeroShared) return PlatformResult::Invalid;
    const struct { WindowId id; uint64_t minimum; } required[] = {
        {WindowId::PowerRpc,0x1000}, {WindowId::GhpmEco,4}, {WindowId::Mfg0Power,4},
        {WindowId::GpuebClockSet,4}, {WindowId::GpuebClockClear,4}, {WindowId::GpuebGpr,0x80},
        {WindowId::Mbox0,640}, {WindowId::MboxSet,4}, {WindowId::MboxClear,4},
        {WindowId::MboxSend,4}, {WindowId::MboxReceive,4}, {WindowId::PlatformSmmu,0x10000}
    };
    for (const auto &item : required) {
        const auto *w = resources.window(item.id);
        if (!w || w->kind != WindowKind::Device || w->bytes < item.minimum) return PlatformResult::Invalid;
    }
    const auto *shared = resources.window(WindowId::GpuebShared);
    if (!shared || shared->kind != WindowKind::ReservedMemory || shared->bytes != 0x180000) return PlatformResult::Invalid;
    const auto result = mailbox_.attach(io);
    if (result != PlatformResult::Ok) return result;
    io_ = io; resources_ = &resources; return PlatformResult::Ok;
}
PlatformResult PlatformPower::wait(WindowId id, uint32_t off, uint32_t mask, uint32_t expected, uint32_t us) {
    for (uint32_t elapsed = 0; ; ++elapsed) {
        uint32_t value = 0;
        if (!io_.read(io_.cookie,id,off,&value)) return PlatformResult::Access;
        if ((value & mask) == expected) return PlatformResult::Ok;
        if (elapsed == us) return PlatformResult::Timeout;
        io_.delayUs(io_.cookie,1);
    }
}
PlatformResult PlatformPower::update(WindowId id, uint32_t off, uint32_t clear, uint32_t set) {
    uint32_t value = 0;
    return io_.read(io_.cookie,id,off,&value) && io_.write(io_.cookie,id,off,(value & ~clear) | set)
        ? PlatformResult::Ok : PlatformResult::Access;
}
PlatformResult PlatformPower::wake(uint32_t us) {
    if (!io_.read(io_.cookie,WindowId::GhpmEco,0,&eco_)) return PlatformResult::Access;
    if (eco_ != 0 && eco_ != 0x101) return PlatformResult::Unsupported;
    uint32_t power = 0;
    if (!io_.read(io_.cookie,WindowId::Mfg0Power,0,&power)) return PlatformResult::Access;
    if ((power & 0xc0000000) != 0xc0000000) {
        uncertain_ = true;
        if (!io_.write(io_.cookie,WindowId::GpuebClockClear,0,1u << 23)) return PlatformResult::Access;
        auto result = wait(WindowId::PowerRpc,0x9a4,0x100ff,0,us);
        if (result != PlatformResult::Ok) return result;
        const uint32_t enable = eco_ == 0 ? 0x800 : 0x658;
        if ((result = update(WindowId::PowerRpc,enable,0,1)) != PlatformResult::Ok) return result;
        if ((result = update(WindowId::PowerRpc,0x800,4,0)) != PlatformResult::Ok) return result;
        if ((result = update(WindowId::PowerRpc,0x800,0,4)) != PlatformResult::Ok) return result;
        if ((result = update(WindowId::PowerRpc,enable,1,0)) != PlatformResult::Ok) return result;
        if ((result = wait(WindowId::PowerRpc,0x9a4,0x100ff,0x10000,us)) != PlatformResult::Ok) return result;
        if ((result = wait(WindowId::Mfg0Power,0,0xc0000000,0xc0000000,us)) != PlatformResult::Ok) return result;
        if ((result = wait(WindowId::GpuebGpr,40,0xffffffff,1,us)) != PlatformResult::Ok) return result;
    }
    eb_ = true; uncertain_ = false;
    if (!io_.read(io_.cookie,WindowId::GpuebGpr,52,&magic_)) return PlatformResult::Access;
    return magic_ ? PlatformResult::Ok : PlatformResult::Protocol;
}
PlatformResult PlatformPower::command(uint32_t cmd, uint32_t state, uint32_t us, int32_t *ret) {
    uint32_t request[8] = {magic_,cmd,0,0,state,0,0,0}, reply[8]{};
    const auto result = mailbox_.exchange(request,reply,us);
    if (result != PlatformResult::Ok) return result;
    *ret = int32_t(reply[4]);
    return *ret >= 0 ? PlatformResult::Ok : PlatformResult::Firmware;
}
PlatformResult PlatformPower::powerOn(uint32_t us) {
    if (!resources_ || eb_ || uncertain_ || mailbox_.poisoned()) return PlatformResult::State;
    if (!timeout(us)) return PlatformResult::Invalid;
    auto result = wake(us);
    if (result != PlatformResult::Ok) return result;
    const auto *shared = resources_->window(WindowId::GpuebShared);
    if (!io_.zeroShared(io_.cookie,0,0x4000)) return PlatformResult::Access;
    io_.barrier(io_.cookie);
    // GPUFREQ's DT partition is 16KiB; the rest of the reservation belongs to
    // other GPUEB users. No native definition of Linux gpufreq_shared_status.
    uint32_t request[8] = {magic_,0,0,0,uint32_t(shared->base),uint32_t(shared->base >> 32),0x4000,0}, reply[8]{};
    if ((result = mailbox_.exchange(request,reply,us)) != PlatformResult::Ok) return result;
    int32_t count = 0;
    gpu_ = true; // Preserve ownership if an issued command's result is unknown.
    if ((result = command(6,1,us,&count)) != PlatformResult::Ok) { uncertain_ = true; return result; }
    active_ = true;
    if ((result = command(7,1,us,&count)) != PlatformResult::Ok) { uncertain_ = true; return result; }
    result = wait(WindowId::PowerRpc,0x500,0xc0000000,0xc0000000,us);
    powerReady_ = result == PlatformResult::Ok; return result;
}
PlatformResult PlatformPower::establishDmaRoute(bool quiesced, uint32_t us) {
    if (!powered()) return PlatformResult::State;
    if (!quiesced) return PlatformResult::Busy;
    if (!timeout(us)) return PlatformResult::Invalid;
    uint32_t id = 0, gbpa = 0;
    if (!io_.read(io_.cookie,WindowId::PlatformSmmu,0,&id)) return PlatformResult::Access;
    if (!id || id == 0xffffffff) return PlatformResult::Unsupported;
    dmaReady_ = false;
    if (!io_.write(io_.cookie,WindowId::PlatformSmmu,0x50,0)) return PlatformResult::Access;
    auto result = wait(WindowId::PlatformSmmu,0x54,7,0,us);
    if (result != PlatformResult::Ok) return result;
    if (!io_.write(io_.cookie,WindowId::PlatformSmmu,0x20,0)) return PlatformResult::Access;
    if ((result = wait(WindowId::PlatformSmmu,0x24,0x1f,0,us)) != PlatformResult::Ok) return result;
    if ((result = wait(WindowId::PlatformSmmu,0x44,1u << 31,0,us)) != PlatformResult::Ok) return result;
    if (!io_.read(io_.cookie,WindowId::PlatformSmmu,0x44,&gbpa) ||
        !io_.write(io_.cookie,WindowId::PlatformSmmu,0x44,(gbpa & ~(1u << 20)) | (1u << 31))) return PlatformResult::Access;
    io_.barrier(io_.cookie);
    result = wait(WindowId::PlatformSmmu,0x44,(1u << 31) | (1u << 20),0,us);
    dmaReady_ = result == PlatformResult::Ok; return result;
}
PlatformResult PlatformPower::powerOff(bool quiesced, uint32_t us) {
    if (!resources_ || !eb_) return PlatformResult::State;
    if (!quiesced) return PlatformResult::Busy;
    if (!timeout(us)) return PlatformResult::Invalid;
    if (uncertain()) return PlatformResult::State;
    powerReady_ = false;
    PlatformResult result; int32_t count = 0;
    if (active_) {
        if ((result = command(7,0,us,&count)) != PlatformResult::Ok) { uncertain_ = true; return result; }
        active_ = false;
        if (count) { uncertain_ = true; return PlatformResult::Busy; }
    }
    if (gpu_) {
        if ((result = command(6,0,us,&count)) != PlatformResult::Ok) { uncertain_ = true; return result; }
        gpu_ = false;
        if (count) { uncertain_ = true; return PlatformResult::Busy; }
    }
    if ((result = wait(WindowId::PowerRpc,0x500,0xc0000000,0,us)) != PlatformResult::Ok) return result;
    uncertain_ = true;
    if ((result = mailbox_.suspend(0,us)) != PlatformResult::Ok) return result;
    if ((result = wait(WindowId::Mfg0Power,0,0xc0000000,0,us)) != PlatformResult::Ok) return result;
    if ((result = wait(WindowId::PowerRpc,0x9a4,0x100ff,0,us)) != PlatformResult::Ok) return result;
    if (!io_.write(io_.cookie,WindowId::GpuebClockSet,0,1u << 23)) return PlatformResult::Access;
    eb_ = dmaReady_ = uncertain_ = false; return PlatformResult::Ok;
}
} // namespace native_mali
