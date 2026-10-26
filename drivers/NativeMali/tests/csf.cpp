// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliCsf.h"
#include <assert.h>
#include <stdio.h>
#include <utility>
#include <vector>
using namespace native_mali;
struct Memory {
    std::vector<uint32_t> words = std::vector<uint32_t>(0x10000 / 4);
    bool failWrite = false;
    uint32_t &at(uint32_t offset) { return words.at(offset / 4); }
    static bool read(void *cookie, uint32_t a, uint32_t *v) {
        auto &m = *static_cast<Memory *>(cookie);
        if ((a & 3) || a < CsfInterface::SharedBase || a - CsfInterface::SharedBase >= m.words.size() * 4) return false;
        *v = m.at(a - CsfInterface::SharedBase); return true;
    }
    static bool write(void *cookie, uint32_t a, uint32_t v) {
        auto &m = *static_cast<Memory *>(cookie);
        uint32_t old = 0; if (m.failWrite || !read(cookie, a, &old)) return false;
        m.at(a - CsfInterface::SharedBase) = v; return true;
    }
    void descriptor(uint32_t offset, uint32_t in, uint32_t out) {
        at(offset) = 0x107f; at(offset + 4) = CsfInterface::SharedBase + in;
        at(offset + 8) = CsfInterface::SharedBase + out;
    }
    Memory() {
        at(0) = 0x01010001; descriptor(4, 0x4000, 0x5000); at(0x10) = 3; at(0x14) = 0x100;
        for (size_t i = 0; i < 3; ++i) {
            const uint32_t group = 0x1000 + i * 0x100;
            descriptor(group, 0x6000 + i * 0x100, 0x6800 + i * 0x100);
            at(group + 0xc) = 0x2000; at(group + 0x14) = 8; at(group + 0x18) = 12;
            for (size_t j = 0; j < 8; ++j) {
                const uint32_t input = 0x7000 + i * 0x1000 + j * 0x80;
                descriptor(group + 0x40 + j * 12, input, 0xa000 + i * 0x1000 + j * 0x100);
            }
        }
    }
    CsfMemoryOps ops() { return {this, read, write}; }
};
int main() {
    Memory m; CsfInterface csf;
    assert(csf.discover(m.ops(), 0x10000) == CsfResult::Ok);
    assert(csf.version() == 0x01010001 && csf.groupCount() == 3);
    assert(csf.group(2)->streamCount == 8 && !csf.group(3));
    assert(csf.globalRequest(0x80000000, 0, true) == CsfResult::Ok);
    assert(m.at(0x4000) == 0x80000000);
    bool done = true;
    assert(csf.globalAcknowledged(0x80000000, &done) == CsfResult::Ok && !done);
    assert(csf.globalRequest(0x80000000, 0, true) == CsfResult::Busy);
    m.at(0x5000) = m.at(0x4000);
    assert(csf.globalAcknowledged(0x80000000, &done) == CsfResult::Ok && done);
    assert(csf.globalRequest(0x80000000, 0, true) == CsfResult::Ok && m.at(0x4000) == 0);
    assert(csf.streamRequest(1, 7, 7, 1, false) == CsfResult::Ok);
    assert(m.at(0x8380) == 1 && csf.streamRequest(1, 7, 7, 0, false) == CsfResult::Busy);
    assert(csf.notifyStream(1, 7) == CsfResult::Ok && m.at(0x6108) == 128);
    assert(csf.notifyGroup(1) == CsfResult::Ok && m.at(0x4008) == 2);
    assert(csf.notifyGroup(1) == CsfResult::Busy);
    assert(csf.groupWrite(3, 0, 0) == CsfResult::Invalid);
    assert(csf.streamWrite(0, 8, 0, 0) == CsfResult::Invalid);
    assert(csf.streamWrite(0, 0, 0x58, 0) == CsfResult::Invalid);
    m.failWrite = true;
    assert(csf.groupWrite(0, 0x50, 0) == CsfResult::Access);
    m.failWrite = false;
    for (auto test : std::vector<std::pair<uint32_t, uint32_t>>{{0,0}, {0,0x02000001},
             {0x10,2}, {0x10,32}, {0x14,0xfffffff0}, {0x1000+0x18,0},
             {0x1000+0x14,33}, {0xc,CsfInterface::SharedLimit},
             {0x1000+0x40+8,CsfInterface::SharedBase+0xfffc}}) {
        Memory bad; bad.at(test.first) = test.second;
        assert(csf.discover(bad.ops(), 0x10000) != CsfResult::Ok);
        assert(!csf.valid() && csf.groupCount() == 0);
        assert(csf.globalRequest(1,0,true) == CsfResult::State);
    }
    assert(csf.discover(m.ops(), 4097) == CsfResult::Invalid);
    puts("PASS: production CSF discovery, bounds/version/stride validation and request/ACK transitions; hardware_exercised=false");
}
