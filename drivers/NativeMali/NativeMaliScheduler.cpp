// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliScheduler.h"
namespace native_mali {
QueueResult Scheduler::initialize(Boot &boot, const SchedulerOps &ops, uint32_t us) {
    if (boot_ || !boot.ready()) return QueueResult::State;
    if (!ops.delayUs || !us || us > 1000000) return QueueResult::Invalid;
    boot_ = &boot; ops_ = ops; timeout_ = us;
    uint64_t next = boot.arenaAddress(), end = next + boot.arenaBytes();
    for (size_t i = 0; i < boot.interface().groupCount() && i < MaxGroups; ++i) {
        const auto *info = boot.interface().group(i);
        // An unreasonably large firmware size must never overflow or consume
        // client storage. Protected execution is not exposed by this service.
        uint64_t suspend = (uint64_t(info->suspendBytes) + 4095) & ~uint64_t(4095);
        if (!suspend || suspend > 16 * 1024 * 1024) return QueueResult::Invalid;
        const uint64_t needed = suspend + MaxStreams * 8192;
        if (next > end || needed > end - next) break;
        auto &g = groups_[i]; g = {};
        g.suspend = uint32_t(next); g.suspendBytes = uint32_t(suspend);
        g.io = uint32_t(next + suspend); next += needed; ++count_;
    }
    if (count_ < 3) { boot_ = nullptr; count_ = 0; return QueueResult::Capacity; }
    return QueueResult::Ok;
}
Scheduler::Group *Scheduler::get(uint32_t handle, size_t *index) {
    if (!boot_ || !handle) return nullptr;
    for (size_t i = 0; i < count_; ++i) if (groups_[i].handle == handle) {
        if (index) *index = i; return &groups_[i];
    }
    return nullptr;
}
bool Scheduler::owns(uint32_t handle, uint8_t as) const {
    for (size_t i = 0; i < count_; ++i) if (groups_[i].handle == handle && handle && groups_[i].as == as) return true;
    return false;
}
uint32_t Scheduler::error(uint8_t as) const {
    uint32_t value = failed_ ? 1 : 0;
    for (size_t i = 0; i < count_; ++i) if (groups_[i].handle && groups_[i].as == as) value |= groups_[i].error;
    return value;
}
QueueResult Scheduler::hardwareFault() {
    HardwareFault f{};
    if (failed_ || !boot_ || !boot_->ready()) return QueueResult::Fault;
    if (boot_->hardware().fault(&f) != HardwareResult::Ok) { failed_ = true; return QueueResult::Access; }
    if ((f.gpuIrq & 0x81) || f.mmuIrq) { failed_ = true; return QueueResult::Fault; }
    return QueueResult::Ok;
}
QueueResult Scheduler::notify(size_t group, int stream) {
    auto &csf = boot_->interface();
    // Drain an earlier notification before changing its toggle bit.
    if (boot_->hardware().ringDoorbell(0) != HardwareResult::Ok) return QueueResult::Access;
    if (stream >= 0) {
        for (uint32_t elapsed = 0;; ++elapsed) {
            const auto r = csf.notifyStream(group, size_t(stream));
            if (r == CsfResult::Ok) break;
            if (r != CsfResult::Busy) return QueueResult::Access;
            if (elapsed == timeout_) return QueueResult::Timeout;
            ops_.delayUs(ops_.cookie, 1);
        }
    }
    for (uint32_t elapsed = 0;; ++elapsed) {
        const auto r = csf.notifyGroup(group);
        if (r == CsfResult::Ok) break;
        if (r != CsfResult::Busy) return QueueResult::Access;
        if (elapsed == timeout_) return QueueResult::Timeout;
        ops_.delayUs(ops_.cookie, 1);
    }
    return boot_->hardware().ringDoorbell(0) == HardwareResult::Ok ? QueueResult::Ok : QueueResult::Access;
}
QueueResult Scheduler::wait(size_t group, int stream, uint32_t mask) {
    for (uint32_t elapsed = 0;; ++elapsed) {
        const auto fault = hardwareFault(); if (fault != QueueResult::Ok) return fault;
        bool done = false;
        auto r = stream < 0 ? boot_->interface().groupAcknowledged(group, mask, &done) :
                             boot_->interface().streamAcknowledged(group, size_t(stream), mask, &done);
        if (r != CsfResult::Ok) return QueueResult::Access;
        if (done) return QueueResult::Ok;
        if (elapsed == timeout_) { groups_[group].error |= 4; return QueueResult::Timeout; }
        ops_.delayUs(ops_.cookie, 1);
    }
}
QueueResult Scheduler::create(uint8_t as, uint32_t streams, uint32_t *handle) {
    if (handle) *handle = 0;
    if (!boot_ || !boot_->ready() || failed_) return QueueResult::State;
    if (!handle || !as || as >= 16 || !(boot_->properties()->addressSpaces & (1u << as)) ||
        !streams || streams > MaxStreams) return QueueResult::Invalid;
    if (serial_ == 0xffffffff) return QueueResult::Capacity;
    for (size_t i = 0; i < count_; ++i) {
        auto &g = groups_[i]; if (g.handle) continue;
        uint32_t ack = 0;
        if (boot_->interface().groupRead(i, 0, &ack) != CsfResult::Ok || (ack & 7)) return QueueResult::Busy;
        if (!boot_->firmwareMemory().zero(g.suspend, g.suspendBytes) ||
            !boot_->firmwareMemory().zero(g.io, MaxStreams * 8192)) return QueueResult::Access;
        g.handle = ++serial_; g.as = as; g.streams = streams; g.started = g.suspended = false; g.error = 0;
        for (auto &q : g.queues) q = {};
        *handle = g.handle; return QueueResult::Ok;
    }
    return QueueResult::Capacity;
}
QueueResult Scheduler::programGroup(size_t index) {
    auto &g = groups_[index]; auto &c = boot_->interface(); const auto *p = boot_->properties();
    uint32_t cores = 0; for (uint64_t bits = p->shaders; bits; bits >>= 1) cores += bits & 1;
    const uint32_t ep = cores | (cores << 8) | (1u << 16);
    const uint32_t offsets[] = {0x20,0x24,0x28,0x2c,0x30,0x34,0x38,0x40,0x44,0x48,0x4c,0x50,0x54,0x60,0x64,4};
    const uint32_t values[] = {uint32_t(p->shaders),uint32_t(p->shaders >> 32),uint32_t(p->shaders),
        uint32_t(p->shaders >> 32),uint32_t(p->tilers),ep,0,g.suspend,0,0,0,g.as,g.handle,0,0,0xffffffff};
    for (size_t i = 0; i < sizeof(offsets)/sizeof(*offsets); ++i)
        if (c.groupWrite(index,offsets[i],values[i]) != CsfResult::Ok) return QueueResult::Access;
    if (c.groupRequest(index,1u << 4,0,true) != CsfResult::Ok) return QueueResult::Busy;
    return QueueResult::Ok;
}
QueueResult Scheduler::groupState(size_t index, uint32_t state) {
    auto &g = groups_[index];
    if (boot_->interface().groupRequest(index,7,state,false) != CsfResult::Ok) return QueueResult::Busy;
    // Once published, even a failed doorbell write may have reached hardware.
    // Keep the group/AS/backing memory owned until a confirmed ACK or reset.
    auto r = notify(index); if (r == QueueResult::Ok) r = wait(index,-1,7 | (1u << 4));
    if (r != QueueResult::Ok) g.error |= 4;
    return r;
}
QueueResult Scheduler::bind(uint32_t handle, uint32_t stream, uint64_t ring, uint32_t bytes, QueueView *out) {
    size_t index = 0; auto *g = get(handle,&index);
    if (!g || !out || g->error || failed_) return QueueResult::State;
    if (stream >= g->streams || !ring || (ring & 4095) || bytes < 4096 || bytes > 1024 * 1024 ||
        (bytes & (bytes - 1)) || ring + bytes < ring || (ring + bytes) >> boot_->properties()->virtualBits)
        return QueueResult::Invalid;
    auto &q = g->queues[stream]; if (q.bound || g->suspended) return QueueResult::Busy;
    q.ring = ring; q.bytes = bytes; q.io = g->io + stream * 8192; q.bound = true;
    auto &c = boot_->interface();
    const uint32_t offsets[] = {4,0xc,0x10,0x14,0x18,0x30,0x34,0x38,0x3c,0x40,0x44,0x48,0x4c,0x50,0x54};
    const uint32_t values[] = {uint32_t(index + 1) << 8,0xffffffff,uint32_t(ring),uint32_t(ring >> 32),bytes,
        q.io,0,q.io + 4096,0,0,0,0,0,0,0};
    for (size_t i = 0; i < sizeof(offsets)/sizeof(*offsets); ++i) {
        if (c.streamWrite(index,stream,offsets[i],values[i]) != CsfResult::Ok) { g->error |= 64; return QueueResult::Access; }
    }
    if (c.streamAccept(index,stream,0xc0000000) != CsfResult::Ok ||
        c.streamRequest(index,stream,7,1,false) != CsfResult::Ok) { g->error |= 64; return QueueResult::Access; }
    auto r = QueueResult::Ok;
    if (!g->started) {
        r = programGroup(index);
        if (r == QueueResult::Ok) { g->started = true; r = groupState(index,1); }
    }
    if (r == QueueResult::Ok) r = notify(index,int(stream));
    if (r == QueueResult::Ok) r = wait(index,int(stream),7);
    if (r != QueueResult::Ok) { g->error |= 64; return r; }
    return view(handle,stream,out);
}
QueueResult Scheduler::view(uint32_t handle, uint32_t stream, QueueView *out) {
    auto *g = get(handle);
    if (!g || !out || stream >= g->streams || !g->queues[stream].bound) return QueueResult::Invalid;
    const auto &q = g->queues[stream]; *out = {};
    out->input = q.io; out->output = q.io + 4096; out->insert = q.insert; out->error = g->error | (failed_ ? 1 : 0);
    if (!boot_->firmwareMemory().read64(out->output,&out->extract) ||
        !boot_->firmwareMemory().read32(out->output + 8,&out->active)) return QueueResult::Access;
    return QueueResult::Ok;
}
QueueResult Scheduler::kick(uint32_t handle, uint32_t stream, uint64_t insert) {
    size_t index = 0; auto *g = get(handle,&index); QueueView v{};
    if (!g || g->error || failed_ || !g->started || g->suspended) return QueueResult::State;
    if (view(handle,stream,&v) != QueueResult::Ok) return QueueResult::Invalid;
    auto &q = g->queues[stream];
    if ((insert & 7) || insert < q.insert || v.extract > insert || insert - v.extract > q.bytes) return QueueResult::Invalid;
    if (boot_->hardware().synchronizeMemory(timeout_) != HardwareResult::Ok) { g->error |= 1; return QueueResult::Fault; }
    if (!boot_->firmwareMemory().write64(q.io,insert)) return QueueResult::Access;
    q.insert = insert;
    if (boot_->hardware().ringDoorbell(uint8_t(index + 1)) != HardwareResult::Ok) { g->error |= 1; return QueueResult::Access; }
    return QueueResult::Ok;
}
QueueResult Scheduler::suspend(uint8_t as) {
    if (!boot_) return QueueResult::State;
    for (size_t i = 0; i < count_; ++i) {
        auto &g = groups_[i]; if (!g.handle || g.as != as || !g.started || g.suspended) continue;
        const auto r = groupState(i,2); if (r != QueueResult::Ok) return r;
        g.suspended = true;
    }
    return QueueResult::Ok;
}
QueueResult Scheduler::resume(uint8_t as) {
    if (!boot_ || failed_ || error(as)) return QueueResult::State;
    for (size_t i = 0; i < count_; ++i) {
        auto &g = groups_[i]; if (!g.handle || g.as != as || !g.started || !g.suspended) continue;
        for (size_t j = 0; j < g.streams; ++j) if (g.queues[j].bound) {
            uint64_t extract = 0; auto &q = g.queues[j];
            if (!boot_->firmwareMemory().read64(q.io + 4096,&extract) ||
                !boot_->firmwareMemory().write64(q.io + 8,extract)) return QueueResult::Access;
        }
        const auto r = groupState(i,3); if (r != QueueResult::Ok) return r;
        g.suspended = false;
    }
    return QueueResult::Ok;
}
QueueResult Scheduler::destroy(uint32_t handle) {
    size_t index = 0; auto *g = get(handle,&index); if (!g) return QueueResult::Invalid;
    if (g->started) { const auto r = groupState(index,0); if (r != QueueResult::Ok) return r; }
    g->handle = 0; g->as = 0; g->started = g->suspended = false; g->error = 0;
    return QueueResult::Ok;
}
QueueResult Scheduler::destroyContext(uint8_t as) {
    for (size_t i = 0; i < count_; ++i) if (groups_[i].handle && groups_[i].as == as) {
        const auto result = destroy(groups_[i].handle); if (result != QueueResult::Ok) return result;
    }
    return QueueResult::Ok;
}
QueueResult Scheduler::unbind(uint32_t handle, uint32_t stream) {
    size_t index = 0; auto *g = get(handle,&index);
    if (!g || stream >= g->streams || !g->queues[stream].bound) return QueueResult::Invalid;
    if (g->suspended) return QueueResult::Busy;
    if (boot_->interface().streamRequest(index,stream,7,0,false) != CsfResult::Ok) return QueueResult::Busy;
    auto r = notify(index,int(stream)); if (r == QueueResult::Ok) r = wait(index,int(stream),7);
    if (r != QueueResult::Ok) { g->error |= 4; return r; }
    g->queues[stream] = {}; return QueueResult::Ok;
}
QueueResult Scheduler::poll() {
    const auto h = hardwareFault(); if (h != QueueResult::Ok) return h;
    auto &c = boot_->interface(); bool notifyNeeded = false;
    for (size_t i = 0; i < count_; ++i) {
        auto &g = groups_[i]; if (!g.handle || !g.started) continue;
        uint32_t pending = 0, irq = 0;
        if (c.groupPending(i,0xb0000000,&pending) != CsfResult::Ok || c.groupRead(i,0xc,&irq) != CsfResult::Ok ||
            c.groupWrite(i,0xc,irq) != CsfResult::Ok) return QueueResult::Access;
        if (pending & 0x80000000) g.error |= 32;
        if (pending) { if (c.groupAccept(i,pending) != CsfResult::Ok) return QueueResult::Access; notifyNeeded = true; }
        for (size_t j = 0; j < g.streams; ++j) {
            if (!g.queues[j].bound) continue;
            if (c.streamPending(i,j,0xcc000000,&pending) != CsfResult::Ok) return QueueResult::Access;
            if (pending & 0xc0000000) g.error |= 2;
            if (pending & 0x08000000) g.error |= 16; // Protected execution is not supported.
            if ((pending & 0x04000000) && !g.suspended && !g.error) {
                uint32_t lo = 0, hi = 0, start = 0, end = 0, frag = 0; uint64_t chunk = 0;
                if (c.streamRead(i,j,0xd0,&lo) != CsfResult::Ok || c.streamRead(i,j,0xd4,&hi) != CsfResult::Ok ||
                    c.streamRead(i,j,0xc0,&start) != CsfResult::Ok || c.streamRead(i,j,0xc4,&end) != CsfResult::Ok ||
                    c.streamRead(i,j,0xcc,&frag) != CsfResult::Ok) return QueueResult::Access;
                if (!ops_.tilerOom || !ops_.tilerOom(ops_.cookie,g.as,uint64_t(hi) << 32 | lo,start,end,frag,&chunk)) g.error |= 8;
                if (c.streamWrite(i,j,0x20,uint32_t(chunk)) != CsfResult::Ok || c.streamWrite(i,j,0x24,uint32_t(chunk >> 32)) != CsfResult::Ok ||
                    c.streamWrite(i,j,0x28,uint32_t(chunk)) != CsfResult::Ok || c.streamWrite(i,j,0x2c,uint32_t(chunk >> 32)) != CsfResult::Ok)
                    return QueueResult::Access;
            }
            // Do not ACK unsupported protected execution or a fault as success.
            const uint32_t accepted = pending & 0x04000000;
            if (accepted && !g.suspended) {
                if (c.streamAccept(i,j,accepted) != CsfResult::Ok) return QueueResult::Access;
                const auto r = notify(i,int(j)); if (r != QueueResult::Ok) return r;
            }
        }
    }
    if (notifyNeeded && boot_->hardware().ringDoorbell(0) != HardwareResult::Ok) return QueueResult::Access;
    HardwareFault fault{};
    if (boot_->hardware().fault(&fault) != HardwareResult::Ok ||
        boot_->hardware().acknowledgeJob(fault.jobIrq) != HardwareResult::Ok) return QueueResult::Access;
    return QueueResult::Ok;
}
QueueResult Scheduler::signal(uint8_t as) {
    if (!boot_ || failed_) return QueueResult::State;
    for (size_t i = 0; i < count_; ++i) if (groups_[i].handle && groups_[i].as == as && groups_[i].started) {
        if (boot_->hardware().ringDoorbell(uint8_t(i + 1)) != HardwareResult::Ok) return QueueResult::Access;
    }
    return QueueResult::Ok;
}
void Scheduler::resetAfterQuiescence() {
    if (!boot_ || !boot_->hardware().quiesced()) return;
    for (auto &g : groups_) g = {};
    boot_ = nullptr; count_ = 0; failed_ = false;
}
} // namespace native_mali
