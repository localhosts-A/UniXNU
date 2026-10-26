// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliHardware.h"
#include <assert.h>
#include <stdio.h>
#include <map>
#include <utility>
#include <vector>
using namespace native_mali;
struct Registers {
    std::map<uint32_t, uint32_t> words;
    std::vector<std::pair<uint32_t, uint32_t>> writes;
    uint32_t delays = 0, failOffset = 0xffffffff;
    bool resetCompletes = true, bootCompletes = true, flushCompletes = true, haltCompletes = true;
    Registers() {
        words[0] = 0xd8300015; words[0x14] = 0x2830; words[0x18] = 0xffff;
        words[0x100] = 0xfff; words[0x110] = 1; words[0x120] = 3;
    }
    static bool read(void *cookie, uint32_t off, uint32_t *value) {
        auto &r = *static_cast<Registers *>(cookie);
        assert(off < 0x480000 && !(off & 3)); *value = r.words[off]; return true;
    }
    static bool write(void *cookie, uint32_t off, uint32_t value) {
        auto &r = *static_cast<Registers *>(cookie);
        assert(off < 0x480000 && !(off & 3)); r.writes.push_back({off,value});
        if (off == r.failOffset) return false;
        if (off == 0x24) r.words[0x20] &= ~value;
        else if (off == 0x1004) r.words[0x1000] &= ~value;
        else if (off == 0x2004) r.words[0x2000] &= ~value;
        else r.words[off] = value;
        if (off == 0x30 && value == 0x101 && r.resetCompletes) r.words[0x20] |= 1u << 8;
        if (off == 0x30 && value == 0x3304 && r.flushCompletes) r.words[0x20] |= 1u << 17;
        if (off == 0x1a0 || off == 0x1a4) r.words[off - 0x40] |= value;
        if (off == 0x190 || off == 0x194) r.words[off - 0x40] |= value;
        if (off == 0x700 && value == 2 && r.bootCompletes) r.words[0x1000] |= Hardware::GlobalInterrupt;
        if (off == 0x700 && value == 0 && r.haltCompletes) r.words[0x704] |= 2;
        return true;
    }
    static void delay(void *cookie, uint32_t us) { static_cast<Registers *>(cookie)->delays += us; }
    RegisterOps ops() { return {this,read,write,delay}; }
};
static void prepare(Registers &r, Hardware &gpu) {
    assert(gpu.attach(r.ops(), 0x480000) == HardwareResult::Ok);
    assert(gpu.properties()->gpuId == 0xd8300015 && gpu.properties()->physicalBits == 40);
    assert(gpu.softReset(10) == HardwareResult::Ok && gpu.quiesced());
    assert(gpu.powerL2(10) == HardwareResult::Ok);
}
int main() {
    Registers r; Hardware gpu; prepare(r,gpu);
    assert(gpu.bootMcu(10) == HardwareResult::State);
    assert(gpu.configureAddressSpace(0,0x100000000,10) == HardwareResult::Ok);
    // Golden AS0 layout and policy: PTW WB + read-allocate, 4K AArch64;
    // non-cacheable attribute at index5 is part of the same hardware table.
    assert(r.words[0x2430] == 0x42000006 && r.words[0x2434] == 0);
    assert(r.words[0x2400] == 0 && r.words[0x2404] == 1);
    assert(r.words[0x2408] == 0x888d8f88 && r.words[0x240c] == 0x00084c8d);
    assert(r.words[0x2418] == 1);
    assert(gpu.configureAddressSpace(0,0x200000000,10) == HardwareResult::State);
    assert(gpu.bootMcu(10) == HardwareResult::Ok && gpu.mcuRunning() && !gpu.quiesced());
    assert(gpu.ringDoorbell(23) == HardwareResult::Ok && r.words[0x1f0000] == 1);
    assert(gpu.ringDoorbell(24) == HardwareResult::Invalid);
    assert(gpu.disableAddressSpace(0,10) == HardwareResult::State);
    assert(gpu.configureAddressSpace(1,0x200000000,10) == HardwareResult::Ok);
    assert(gpu.synchronizeAddressSpace(1,10) == HardwareResult::Ok);
    assert(r.words[0x2450] == 47 && r.words[0x2458] == 3);
    r.flushCompletes = false; const auto before = r.delays;
    assert(gpu.synchronizeAddressSpace(1,7) == HardwareResult::Timeout);
    assert(r.delays - before == 7 && r.words[0x2458] == 3); // UNLOCK even after failed cache flush.
    r.flushCompletes = true;
    assert(gpu.disableAddressSpace(1,10) == HardwareResult::Ok && r.words[0x2470] == 0x42000001);
    r.haltCompletes = false;
    assert(gpu.haltMcu(5) == HardwareResult::Timeout && gpu.mcuRunning());
    r.haltCompletes = true;
    assert(gpu.haltMcu(10) == HardwareResult::Ok && !gpu.mcuRunning() && !gpu.quiesced());
    assert(gpu.disableAddressSpace(0,10) == HardwareResult::Ok);
    assert(gpu.softReset(10) == HardwareResult::Ok && gpu.quiesced());
    {
        Registers bad; Hardware failing; prepare(bad,failing);
        bad.failOffset = 0x2404;
        assert(failing.configureAddressSpace(0,0x100000000,10) == HardwareResult::Access);
        bad.failOffset = 0xffffffff;
        assert(failing.configureAddressSpace(0,0x100000000,10) == HardwareResult::State);
        assert(failing.bootMcu(10) == HardwareResult::State);
        assert(failing.disableAddressSpace(0,10) == HardwareResult::Ok);
        assert(failing.configureAddressSpace(0,0x100000000,10) == HardwareResult::Ok);
        bad.failOffset = 0x700;
        assert(failing.bootMcu(10) == HardwareResult::Access && failing.mcuRunning());
        bad.failOffset = 0xffffffff; bad.resetCompletes = false;
        assert(failing.softReset(3) == HardwareResult::Timeout && !failing.quiesced());
        bad.resetCompletes = true;
        assert(failing.softReset(10) == HardwareResult::Ok && failing.quiesced());
    }
    {
        Registers bad; Hardware failing; bad.words[0] = 0xa8670000;
        assert(failing.attach(bad.ops(),0x480000) == HardwareResult::Unsupported && !failing.properties());
        assert(bad.writes.empty());
    }
    puts("PASS: production G925 register sequence, AS/cache policy, deadlines and uncertain-hardware retention; hardware_exercised=false");
}
