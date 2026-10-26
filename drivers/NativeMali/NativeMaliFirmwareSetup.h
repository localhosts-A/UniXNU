// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "NativeMaliFirmwareMemory.h"
namespace native_mali {
enum class FirmwareSetupResult : uint32_t { Ok, State, Invalid, Layout, Unsupported, Allocation, Access };

// Validate boot-relevant metadata, retain compiled configuration defaults, and
// give trace writers real owned MCU buffers with all trace-enable bits cleared.
// Diagnostic timeline/build/function/core-dump metadata is not executed.
// On any failure unload the entire FirmwareMemory before retrying. MCU must not
// have been started; allocation and metadata publication happen before AS0 load.
class FirmwareSetup {
public:
    FirmwareSetupResult prepare(const FirmwareImage &, FirmwareMemory &);
    bool prepared() const { return prepared_; }
    uint32_t sharedBytes() const { return prepared_ ? sharedBytes_ : 0; }
    size_t traceCount() const { return prepared_ ? count_ : 0; }
    uint32_t errorEntry() const { return errorEntry_; }
private:
    struct Trace { uint32_t size = 0, insert = 0, extract = 0, data = 0, enable = 0, bits = 0; } traces_[16]{};
    size_t count_ = 0;
    uint32_t sharedBytes_ = 0;
    uint32_t errorEntry_ = 0xffffffff;
    bool attempted_ = false, prepared_ = false;
};
} // namespace native_mali
