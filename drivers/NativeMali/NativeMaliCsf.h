// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace native_mali {
struct CsfMemoryOps {
    void *cookie = nullptr;
    bool (*read)(void *, uint32_t address, uint32_t *) = nullptr;
    bool (*write)(void *, uint32_t address, uint32_t) = nullptr;
};
enum class CsfResult : uint32_t { Ok, State, Invalid, Access, Version, Layout, Busy };
struct CsfEndpoint { uint32_t input = 0, output = 0, features = 0; };
struct CsfGroup {
    CsfEndpoint endpoint{};
    uint32_t suspendBytes = 0, protectedSuspendBytes = 0, streamCount = 0;
    CsfEndpoint streams[32]{};
};

// Serialized CSF shared-memory protocol. The callbacks implement acquire/release
// and cache maintenance; MMIO doorbell writes and bounded waits are the driver's
// responsibility. Discovery is valid only after a real MCU boot interrupt.
class CsfInterface {
public:
    static constexpr uint32_t SharedBase = 0x04000000, SharedLimit = 0x08000000;
    static constexpr size_t MaxGroups = 31, MaxStreams = 32;
    CsfResult discover(const CsfMemoryOps &, uint32_t sharedBytes);
    void reset();
    bool valid() const { return valid_; }
    uint32_t version() const { return valid_ ? version_ : 0; }
    uint32_t features() const { return valid_ ? global_.features : 0; }
    size_t groupCount() const { return valid_ ? groupCount_ : 0; }
    const CsfGroup *group(size_t i) const { return valid_ && i < groupCount_ ? groups_ + i : nullptr; }
    CsfResult globalWrite(uint32_t offset, uint32_t value);
    CsfResult groupWrite(size_t group, uint32_t offset, uint32_t value);
    CsfResult streamWrite(size_t group, size_t stream, uint32_t offset, uint32_t value);
    CsfResult globalRead(uint32_t offset, uint32_t *value);
    CsfResult groupRead(size_t group, uint32_t offset, uint32_t *value);
    CsfResult streamRead(size_t group, size_t stream, uint32_t offset, uint32_t *value);
    // Request fields are changed only when the selected field has been ACKed.
    // toggle requests invert ACK; state requests replace the specified bits.
    CsfResult globalRequest(uint32_t mask, uint32_t value, bool toggle);
    CsfResult groupRequest(size_t group, uint32_t mask, uint32_t value, bool toggle);
    CsfResult streamRequest(size_t group, size_t stream, uint32_t mask, uint32_t value, bool toggle);
    CsfResult notifyGroup(size_t group);
    CsfResult notifyStream(size_t group, size_t stream);
    CsfResult globalAcknowledged(uint32_t mask, bool *);
    CsfResult groupAcknowledged(size_t group, uint32_t mask, bool *);
    CsfResult streamAcknowledged(size_t group, size_t stream, uint32_t mask, bool *);
    CsfResult groupPending(size_t group, uint32_t mask, uint32_t *);
    CsfResult streamPending(size_t group, size_t stream, uint32_t mask, uint32_t *);
    CsfResult groupAccept(size_t group, uint32_t mask);
    CsfResult streamAccept(size_t group, size_t stream, uint32_t mask);
private:
    CsfMemoryOps ops_{};
    uint32_t bytes_ = 0, version_ = 0, groupCount_ = 0;
    CsfEndpoint global_{};
    CsfGroup groups_[MaxGroups]{};
    bool valid_ = false;
    bool range(uint64_t, uint64_t) const;
    bool read(uint32_t, uint32_t *);
    bool endpoint(uint32_t descriptor, CsfEndpoint *, uint32_t inputBytes, uint32_t outputBytes);
    CsfResult write(const CsfEndpoint &, uint32_t, uint32_t, uint32_t limit);
    CsfResult output(const CsfEndpoint &, uint32_t, uint32_t *, uint32_t limit);
    CsfResult request(const CsfEndpoint &, uint32_t offset, uint32_t, uint32_t, bool);
    CsfResult acknowledged(const CsfEndpoint &, uint32_t, bool *);
    CsfResult pending(const CsfEndpoint &, uint32_t, uint32_t *, bool accept);
};
} // namespace native_mali
