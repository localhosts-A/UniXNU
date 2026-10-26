// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliPlatform.h"
#include <assert.h>
#include <stdio.h>
#include <map>
#include <utility>
#include <vector>
using namespace native_mali;
struct Io {
    std::map<std::pair<WindowId,uint32_t>,uint32_t> words;
    std::vector<uint32_t> commands;
    size_t zeroed = 0, barriers = 0, delays = 0;
    bool respond = true, wrongReply = false, smmuCompletes = true, rejectPower = false, foreignActive = false;
    uint32_t &at(WindowId id, uint32_t off=0) { return words[{id,off}]; }
    Io() {
        at(WindowId::Mfg0Power) = 0xc0000000; at(WindowId::GpuebGpr,52) = 0xabcdef01;
        at(WindowId::GpuebGpr,40) = 1; at(WindowId::GhpmEco) = 0x101;
        at(WindowId::PlatformSmmu) = 3; at(WindowId::PlatformSmmu,0x20) = 1;
        at(WindowId::PlatformSmmu,0x24) = 1; at(WindowId::PlatformSmmu,0x44) = 1u << 20;
    }
    static bool read(void *cookie, WindowId id, uint32_t off, uint32_t *v) {
        *v = static_cast<Io *>(cookie)->at(id,off); return true;
    }
    static bool write(void *cookie, WindowId id, uint32_t off, uint32_t v) {
        auto &io = *static_cast<Io *>(cookie); io.at(id,off) = v;
        if (id == WindowId::MboxClear) io.at(WindowId::MboxReceive) &= ~v;
        if (id == WindowId::MboxSet && v == 2) {
            const uint32_t cmd = io.at(WindowId::Mbox0,20), state = io.at(WindowId::Mbox0,32);
            io.commands.push_back(cmd);
            assert(io.at(WindowId::Mbox0,28) == 0); // 64-bit union alignment padding.
            if (cmd == 0) {
                assert(io.at(WindowId::Mbox0,32) == 0xfc810000 && io.at(WindowId::Mbox0,36) == 0);
                assert(io.at(WindowId::Mbox0,40) == 0x4000);
            }
            if (io.respond) {
                for (size_t i=0;i<8;++i) io.at(WindowId::Mbox0,(60+i)*4) = io.at(WindowId::Mbox0,(4+i)*4);
                io.at(WindowId::Mbox0,244) = io.wrongReply ? cmd+1 : cmd;
                io.at(WindowId::Mbox0,256) = (cmd==6 && io.rejectPower) ? uint32_t(-22) :
                    ((cmd==7 && !state && io.foreignActive) ? 1 : state);
                io.at(WindowId::MboxReceive) |= 2;
                if (cmd==6 && !io.rejectPower) io.at(WindowId::PowerRpc,0x500) = state ? 0xc0000000 : 0;
            }
        }
        if (id==WindowId::MboxSet && v==4) {
            assert(io.at(WindowId::Mbox0,48)==0 && io.at(WindowId::Mbox0,52)==0 && io.at(WindowId::Mbox0,56)==0x55667788);
            io.at(WindowId::Mfg0Power)=0; io.at(WindowId::PowerRpc,0x9a4)=0;
        }
        if (id==WindowId::PowerRpc && off==0x800 && (v&4)) {
            io.at(WindowId::Mfg0Power)=0xc0000000; io.at(WindowId::PowerRpc,0x9a4)=0x10000;
        }
        if (id==WindowId::PlatformSmmu && io.smmuCompletes) {
            if (off==0x20) io.at(id,0x24)=v;
            if (off==0x50) io.at(id,0x54)=v;
            if (off==0x44) io.at(id,0x44)=v & ~(1u<<31);
        }
        return true;
    }
    static void barrier(void *cookie) { ++static_cast<Io *>(cookie)->barriers; }
    static void delay(void *cookie,uint32_t us) { static_cast<Io *>(cookie)->delays+=us; }
    static bool zero(void *cookie,size_t off,size_t bytes) {
        assert(off==0 && bytes==0x4000); static_cast<Io *>(cookie)->zeroed+=bytes; return true;
    }
    PlatformIo ops() { return {this,read,write,barrier,delay,zero}; }
};
static void put(std::vector<uint8_t> &b,size_t off,uint64_t v,size_t n=8) {
    for(size_t i=0;i<n;++i)b.at(off+i)=v>>(i*8);
}
static Resources resources() {
    const WindowId ids[] = {WindowId::Gpu,WindowId::PowerRpc,WindowId::GhpmEco,WindowId::Mfg0Power,
        WindowId::GpuebClockSet,WindowId::GpuebClockClear,WindowId::GpuebGpr,WindowId::Mbox0,
        WindowId::MboxSet,WindowId::MboxClear,WindowId::MboxSend,WindowId::MboxReceive,
        WindowId::PlatformSmmu,WindowId::GpuebShared};
    std::vector<uint8_t>b(176+sizeof(ids)/sizeof(*ids)*24);
    const uint64_t h[]={Resources::Magic,1,sizeof(ids)/sizeof(*ids),6,4096,20,13,1};
    for(size_t i=0;i<8;++i)put(b,i*8,h[i]); b[64]=b[96]=1;
    for(size_t i=0;i<6;++i)put(b,128+i*8,604+i);
    for(size_t i=0;i<sizeof(ids)/sizeof(*ids);++i) {
        const size_t off=176+i*24; const bool ram=ids[i]==WindowId::GpuebShared;
        put(b,off,uint32_t(ids[i]),4);put(b,off+4,ram?2:1,4);
        put(b,off+8,ram?0xfc810000:0x50000000+i*0x20000);
        put(b,off+16,ram?0x180000:0x10000);
    }
    Resources r;assert(r.decode(b.data(),b.size()));return r;
}
int main() {
    const auto res=resources(); Io io; PlatformPower power;
    assert(power.attach(res,io.ops(),false)==PlatformResult::Invalid && io.commands.empty());
    assert(power.attach(res,io.ops(),true)==PlatformResult::Ok);
    assert(power.powerOn(10)==PlatformResult::Ok && power.powered() && !power.dmaReady());
    assert((io.commands==std::vector<uint32_t>{0,6,7}) && io.zeroed==0x4000);
    assert(power.establishDmaRoute(false,10)==PlatformResult::Busy);
    assert(power.establishDmaRoute(true,10)==PlatformResult::Ok && power.dmaReady());
    assert(io.at(WindowId::PlatformSmmu,0x20)==0 && io.at(WindowId::PlatformSmmu,0x44)==0);
    assert(power.powerOff(false,10)==PlatformResult::Busy && power.powered());
    assert(power.powerOff(true,10)==PlatformResult::Ok && !power.powered() && !power.uncertain());
    assert((io.commands==std::vector<uint32_t>{0,6,7,7,6}));
    assert(io.at(WindowId::GpuebClockSet)==(1u<<23));
    assert(power.powerOn(10)==PlatformResult::Ok); // GHPM B0 warm resume.
    assert((io.at(WindowId::PowerRpc,0x658)&1)==0 && io.at(WindowId::GpuebClockClear)==(1u<<23));
    io.smmuCompletes=false; io.at(WindowId::PlatformSmmu,0x24)=1;
    const auto before=io.delays;
    assert(power.establishDmaRoute(true,7)==PlatformResult::Timeout && !power.dmaReady());
    assert(io.delays-before==7);
    {
        Io late; Mailbox box;assert(box.attach(late.ops())==PlatformResult::Ok);
        uint32_t request[8]={1,8,0,0,0,0,0,0},reply[8]{};
        late.at(WindowId::MboxReceive)=2;
        assert(box.exchange(request,reply,5)==PlatformResult::Busy && late.commands.empty() && !box.poisoned());
        late.at(WindowId::MboxReceive)=0;late.respond=false;
        assert(box.exchange(request,reply,5)==PlatformResult::Timeout && box.poisoned());
        late.at(WindowId::MboxReceive)=2;late.respond=true;
        assert(box.exchange(request,reply,5)==PlatformResult::State); // Late reply cannot complete another request.
    }
    {
        Io bad;PlatformPower p;bad.rejectPower=true;
        assert(p.attach(res,bad.ops(),true)==PlatformResult::Ok);
        assert(p.powerOn(10)==PlatformResult::Firmware && !p.powered() && p.uncertain());
        assert(p.powerOff(true,10)==PlatformResult::State);
    }
    {
        Io bad;PlatformPower p;bad.foreignActive=true;
        assert(p.attach(res,bad.ops(),true)==PlatformResult::Ok && p.powerOn(10)==PlatformResult::Ok);
        assert(p.powerOff(true,10)==PlatformResult::Busy && p.uncertain());
        assert(p.powerOff(true,10)==PlatformResult::State);
        assert((bad.commands==std::vector<uint32_t>{0,6,7,7})); // Do not drop GPU power while other active refs exist.
    }
    puts("PASS: production MT6991 mailbox/GHPM/power/SMMUv3 bypass, late replies, firmware errors and deadlines; hardware_exercised=false");
}
