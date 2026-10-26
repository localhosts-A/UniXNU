// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliBoot.h"
namespace native_mali {
enum class QueueResult : uint32_t { Ok, State, Invalid, Capacity, Access, Timeout, Fault, Busy };
struct SchedulerOps {
    void *cookie = nullptr;
    void (*delayUs)(void *, uint32_t) = nullptr;
    // Return a packed size/address chunk pointer in the client's AS, or zero
    // when the bounded heap cannot grow. Called under the device's sleep lock.
    bool (*tilerOom)(void *, uint8_t as, uint64_t heap, uint32_t vtStart,
                     uint32_t vtEnd, uint32_t fragEnd, uint64_t *chunk) = nullptr;
};
struct QueueView { uint32_t input = 0, output = 0; uint64_t insert = 0, extract = 0; uint32_t active = 0, error = 0; };
// Resident CSG allocator. Capacity is explicitly bounded by real firmware slots
// and the preboot AS0 arena. No overcommit/time slicing is advertised. Every
// client AS mutation must suspend all its groups first, then flush and resume.
class Scheduler {
public:
    static constexpr size_t MaxGroups = 16, MaxStreams = 8;
    QueueResult initialize(Boot &, const SchedulerOps &, uint32_t timeoutUs);
    QueueResult create(uint8_t as, uint32_t streamCount, uint32_t *handle);
    QueueResult bind(uint32_t handle, uint32_t stream, uint64_t ring, uint32_t bytes, QueueView *);
    QueueResult kick(uint32_t handle, uint32_t stream, uint64_t insert);
    QueueResult view(uint32_t handle, uint32_t stream, QueueView *);
    QueueResult destroy(uint32_t handle);
    QueueResult unbind(uint32_t handle, uint32_t stream);
    QueueResult destroyContext(uint8_t as);
    QueueResult suspend(uint8_t as);
    QueueResult resume(uint8_t as);
    QueueResult poll();
    QueueResult signal(uint8_t as);
    uint32_t error(uint8_t as) const;
    size_t capacity() const { return count_; }
    bool owns(uint32_t handle, uint8_t as) const;
    // Only after the owning Boot reports actual hardware reset/quiescence.
    void resetAfterQuiescence();
private:
    struct Stream { uint64_t ring = 0, insert = 0; uint32_t bytes = 0, io = 0; bool bound = false; };
    struct Group {
        uint32_t handle = 0, suspend = 0, suspendBytes = 0, io = 0, streams = 0, error = 0;
        uint8_t as = 0; bool started = false, suspended = false;
        Stream queues[MaxStreams]{};
    } groups_[MaxGroups]{};
    Boot *boot_ = nullptr; SchedulerOps ops_{};
    uint32_t timeout_ = 0, serial_ = 0; size_t count_ = 0;
    bool failed_ = false;
    Group *get(uint32_t, size_t *index = nullptr);
    QueueResult notify(size_t group, int stream = -1);
    QueueResult wait(size_t group, int stream, uint32_t mask);
    QueueResult groupState(size_t index, uint32_t state);
    QueueResult programGroup(size_t index);
    QueueResult hardwareFault();
};
} // namespace native_mali
