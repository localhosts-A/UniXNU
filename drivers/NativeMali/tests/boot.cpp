// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliBoot.h"
#include "NativeMaliSha256.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <utility>
#include <vector>
using namespace native_mali;
static void put(std::vector<uint8_t> &b,size_t off,uint64_t v,size_t n=4) {
    for(size_t i=0;i<n;++i)b.at(off+i)=v>>(i*8);
}
static std::vector<uint8_t> image(bool badCaps=false) {
    std::vector<uint8_t>b(4096+65536);
    put(b,0,0xc3f13a6e);b[4]=3;put(b,16,216);
    const uint32_t addresses[]={0x410000,0x04000000,0x800000,0};
    const uint32_t flags[]={1,0xc0000003,5,1};
    for(size_t i=0;i<4;++i) {
        const size_t off=20+i*32;const bool shared=i==1;
        put(b,off,32<<8);put(b,off+4,flags[i]);put(b,off+8,addresses[i]);
        put(b,off+12,addresses[i]+(shared?65536:4096));put(b,off+16,shared?4096:256);
        put(b,off+20,shared?b.size():272);b[off+24]='a'+i;put(b,off+26,addresses[i]);
    }
    put(b,148,1|(28<<8));put(b,152,0x410080);put(b,156,0);put(b,160,7);b[164]='c';
    put(b,176,3|(40<<8));put(b,180,1);
    for(size_t i=0;i<5;++i)put(b,184+i*4,0x410020+i*4);
    put(b,204,4);b[208]='t';
    const auto shared=[&](size_t off,uint32_t v){put(b,4096+off,v);};
    const auto ep=[&](size_t off,uint32_t in,uint32_t out){
        shared(off,0x107f);shared(off+4,0x04000000+in);shared(off+8,0x04000000+out);
    };
    shared(0,0x01060001);ep(4,0x4000,0x5000);shared(0x10,3);shared(0x14,badCaps?0:0x100);
    for(size_t i=0;i<3;++i) {
        const uint32_t group=0x1000+i*0x100;
        ep(group,0x6000+i*0x100,0x6800+i*0x100);shared(group+0xc,0x2000);
        shared(group+0x14,8);shared(group+0x18,12);
        for(size_t j=0;j<8;++j)ep(group+0x40+j*12,0x7000+i*0x1000+j*0x80,0xa000+i*0x1000+j*0x100);
    }
    return b;
}
static std::vector<uint8_t> resources(const std::vector<uint8_t> &fw) {
    const WindowId ids[]={WindowId::Gpu,WindowId::PowerRpc,WindowId::GhpmEco,WindowId::Mfg0Power,
        WindowId::GpuebClockSet,WindowId::GpuebClockClear,WindowId::GpuebGpr,WindowId::Mbox0,
        WindowId::MboxSet,WindowId::MboxClear,WindowId::MboxSend,WindowId::MboxReceive,WindowId::PlatformSmmu,WindowId::GpuebShared};
    std::vector<uint8_t>b(176+sizeof(ids)/sizeof(*ids)*24);
    const uint64_t header[]={Resources::Magic,1,sizeof(ids)/sizeof(*ids),6,4096,fw.size(),13,1};
    for(size_t i=0;i<8;++i)put(b,i*8,header[i],8);
    b[64]=1;sha256(fw.data(),fw.size(),b.data()+96);
    for(size_t i=0;i<6;++i)put(b,128+i*8,604+i,8);
    for(size_t i=0;i<sizeof(ids)/sizeof(*ids);++i) {
        const size_t off=176+i*24;const bool ram=ids[i]==WindowId::GpuebShared,gpu=ids[i]==WindowId::Gpu;
        put(b,off,uint32_t(ids[i]));put(b,off+4,ram?2:1);
        put(b,off+8,ram?0xfc810000:0x50000000+i*0x20000,8);
        put(b,off+16,ram?0x180000:(gpu?0x480000:0x10000),8);
    }
    return b;
}
struct Model {
    struct Allocation { FirmwareAllocation value{};std::vector<uint64_t> pages; };
    std::map<uint64_t,TablePage> tables;
    std::map<void *,Allocation> memory;
    std::map<uint32_t,uint32_t> regs;
    std::map<std::pair<WindowId,uint32_t>,uint32_t> platform;
    uint64_t next=0x10000000;
    size_t writes=0,delays=0;
    bool resetWorks=true,bootWorks=true,globalWorks=true;
    Model() {
        regs[0]=0xd8300015;regs[0x14]=0x2830;regs[0x18]=0xffff;
        regs[0x100]=0xfff;regs[0x110]=1;regs[0x120]=3;
        platform[{WindowId::Mfg0Power,0}]=0xc0000000;
        platform[{WindowId::GpuebGpr,52}]=0x12345678;
        platform[{WindowId::PlatformSmmu,0}]=3;
    }
    static bool tableAlloc(void *c,TablePage *p) {
        auto &m=*static_cast<Model *>(c);void *data=aligned_alloc(4096,4096);assert(data);
        *p={m.next,static_cast<uint64_t *>(data),data};m.next+=4096;m.tables[p->physical]=*p;return true;
    }
    static void tableFree(void *c,const TablePage &p) {
        assert(static_cast<Model *>(c)->tables.erase(p.physical)==1);free(p.allocation);
    }
    static bool tableResolve(void *c,uint64_t pa,TablePage *p) {
        auto &m=*static_cast<Model *>(c);auto it=m.tables.find(pa);
        if(it==m.tables.end())return false;*p=it->second;return true;
    }
    static void tablePublish(void *,const TablePage &,size_t,size_t) {}
    static bool memAlloc(void *c,size_t pages,bool,FirmwareAllocation *out) {
        auto &m=*static_cast<Model *>(c);void *data=aligned_alloc(4096,pages*4096);assert(data);
        auto &a=m.memory[data];for(size_t i=0;i<pages;++i){a.pages.push_back(m.next);m.next+=8192;}
        a.value={static_cast<uint8_t *>(data),a.pages.data(),pages,data};*out=a.value;return true;
    }
    static void memFree(void *c,const FirmwareAllocation &a) {
        assert(static_cast<Model *>(c)->memory.erase(a.handle)==1);free(a.handle);
    }
    static void memSync(void *,const FirmwareAllocation &,size_t,size_t) {}
    uint32_t *gpu(uint32_t va) {
        uint64_t pa=uint64_t(regs[0x2404])<<32|regs[0x2400];
        for(unsigned level=0;level<4;++level) {
            const auto table=tables.find(pa);if(table==tables.end())return nullptr;
            const uint64_t desc=table->second.entries[(uint64_t(va)>>(39-level*9))&511];
            if((desc&3)!=3)return nullptr;
            pa=desc&0x0000fffffffff000ull;
        }
        for(auto &entry:memory)for(size_t i=0;i<entry.second.pages.size();++i)
            if(entry.second.pages[i]==pa)return reinterpret_cast<uint32_t *>(entry.second.value.cpu+i*4096+(va&4095));
        return nullptr;
    }
    static bool regRead(void *c,uint32_t off,uint32_t *v){*v=static_cast<Model *>(c)->regs[off];return true;}
    static bool regWrite(void *c,uint32_t off,uint32_t v) {
        auto &m=*static_cast<Model *>(c);++m.writes;
        if(off==0x24)m.regs[0x20]&=~v;
        else if(off==0x1004)m.regs[0x1000]&=~v;
        else if(off==0x2004)m.regs[0x2000]&=~v;
        else m.regs[off]=v;
        if(off==0x30&&v==0x101&&m.resetWorks){m.regs[0x20]|=1u<<8;m.regs[0x704]=0;}
        if(off==0x1a0||off==0x1a4||off==0x190||off==0x194)m.regs[off-0x40]=v;
        if(off==0x700&&v==2){m.regs[0x704]=1;if(m.bootWorks){assert(m.gpu(0x04000000));m.regs[0x1000]|=Hardware::GlobalInterrupt;}}
        if(off==0x80000&&m.globalWorks) {
            auto in=m.gpu(0x04000008),out=m.gpu(0x0400000c);assert(in&&out);
            auto request=m.gpu(*in),ack=m.gpu(*out);assert(request&&ack);*ack=*request;
            m.regs[0x1000]|=Hardware::GlobalInterrupt;
        }
        return true;
    }
    static bool platformRead(void *c,WindowId id,uint32_t off,uint32_t *v){*v=static_cast<Model *>(c)->platform[{id,off}];return true;}
    static bool platformWrite(void *c,WindowId id,uint32_t off,uint32_t v) {
        auto &m=*static_cast<Model *>(c);++m.writes;m.platform[{id,off}]=v;
        const auto key=[](uint32_t off){return std::make_pair(WindowId::Mbox0,off);};
        if(id==WindowId::MboxClear)m.platform[{WindowId::MboxReceive,0}]&=~v;
        if(id==WindowId::MboxSet&&v==2) {
            for(size_t i=0;i<8;++i)m.platform[key((60+i)*4)]=m.platform[key((4+i)*4)];
            const auto cmd=m.platform[key(20)],state=m.platform[key(32)];
            if(cmd==6||cmd==7)m.platform[key(256)]=state;
            if(cmd==6)m.platform[{WindowId::PowerRpc,0x500}]=state?0xc0000000:0;
            m.platform[{WindowId::MboxReceive,0}]|=2;
        }
        if(id==WindowId::MboxSet&&v==4){m.platform[{WindowId::Mfg0Power,0}]=0;m.platform[{WindowId::PowerRpc,0x9a4}]=0;}
        if(id==WindowId::PlatformSmmu){
            if(off==0x20)m.platform[{id,0x24}]=v;
            if(off==0x50)m.platform[{id,0x54}]=v;
            if(off==0x44)m.platform[{id,0x44}]=v&~(1u<<31);
        }
        return true;
    }
    static void delay(void *c,uint32_t us){static_cast<Model *>(c)->delays+=us;}
    static void barrier(void *){}
    static bool zeroShared(void *,size_t off,size_t bytes){return !off&&bytes==0x4000;}
    BootConfig config(const std::vector<uint8_t>&r,const std::vector<uint8_t>&f) {
        BootConfig c;c.resources=r.data();c.resourceBytes=r.size();c.firmware=f.data();c.firmwareBytes=f.size();
        c.platform={this,platformRead,platformWrite,barrier,delay,zeroShared};c.registers={this,regRead,regWrite,delay};
        c.tables={this,tableAlloc,tableFree,tableResolve,tablePublish};c.memory={this,memAlloc,memFree,memSync,memSync};
        c.sharedReservationOwned=true;c.timeoutUs=5;c.arenaPages=16;return c;
    }
    ~Model(){assert(tables.empty()&&memory.empty());}
};
int main() {
    uint8_t digest[32]{};sha256(reinterpret_cast<const uint8_t *>("abc"),3,digest);
    const uint8_t golden[]={0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    assert(!memcmp(digest,golden,32));
    const auto fw=image(),res=resources(fw);
    {
        Model m;Boot boot;assert(boot.start(m.config(res,fw))==BootResult::Ok&&boot.ready());
        assert(boot.interface().groupCount()==3&&boot.arenaBytes()==65536&&boot.properties()->gpuId==0xd8300015);
        assert(boot.start(m.config(res,fw))==BootResult::State);
        const auto pages=m.memory.size();m.resetWorks=false;
        assert(boot.stop()==BootResult::Retained&&!boot.ready()&&m.memory.size()==pages&&!m.tables.empty());
        m.resetWorks=true;assert(boot.stop()==BootResult::Ok&&m.memory.empty()&&m.tables.empty());
    }
    {
        Model m;Boot boot;auto corrupt=fw;corrupt.back()^=1;
        assert(boot.start(m.config(res,corrupt))==BootResult::Digest&&!boot.ready());
        assert(boot.failedStage()==BootStage::Identity&&m.writes==0);
    }
    for(bool bootFailure:{true,false}) {
        Model m;Boot boot;m.bootWorks=!bootFailure;m.globalWorks=bootFailure;
        assert(boot.start(m.config(res,fw))==(bootFailure?BootResult::Hardware:BootResult::Timeout));
        assert(!boot.ready()&&m.memory.empty()&&m.tables.empty());
        assert(boot.failedStage()==(bootFailure?BootStage::Mcu:BootStage::Global));
    }
    {
        Model m;Boot boot;const auto bad=image(true),badRes=resources(bad);
        assert(boot.start(m.config(badRes,bad))==BootResult::Interface&&!boot.ready());
        assert(m.memory.empty()&&m.tables.empty());
    }
    puts("PASS: production boot composition, real page-table walk, SHA binding, MCU/global ACK gates, rollback and DMA quarantine; hardware_exercised=false");
}
