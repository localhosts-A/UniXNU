// SPDX-License-Identifier: BSD-2-Clause-Patent
// Simulates only MMIO/device replies/cache/clock and the external write policy.
#include "UfsTransport.h"
#include <cstring>
#include <cstdlib>
using namespace unixnu_ufs;
static uint32_t le(const uint8_t *p) { return p[0] | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
static void little(uint8_t *p,uint32_t x) { for(unsigned i=0;i<4;++i)p[i]=uint8_t(x>>(8*i)); }
static void big(uint8_t *p,uint64_t x,unsigned n) { for(unsigned i=0;i<n;++i)p[i]=uint8_t(x>>((n-i-1)*8)); }
struct Sim {
  alignas(4096) uint8_t dma[DmaBytes]{};
  uint32_t regs[0x100/4]{}, fault=0, rings=0, queriesSet=0, bad=0, allowCalls=0, reads=0, writes=0;
  uint32_t size=4096,turs=0; uint64_t blocks=2048, pa=0x22000000, time=0,windowFirst=8;
  bool enabled=true, wp=false, grant=false;
  uint8_t lastCdb[16]{}, lastLun=0, lastTag=0;
  Sim(){regs[0x34/4]=1;regs[0x30/4]=15;}
  void complete() {
    auto q=dma+UcdOffset, r=q+ResponseOffset;
    memset(r,0,512);r[0]=q[0]|0x20;r[2]=q[2];r[3]=q[3];lastLun=q[2];lastTag=q[3];
    if(q[0]==0x16) {
      r[5]=q[5];memcpy(r+12,q+12,4);
      if(q[12]==6) {++queriesSet;if(q[13]!=1||q[5]!=0x81)++bad;}
      else if(q[12]==5) {if(q[13]!=1||q[5]!=1)++bad;if(fault==30)r[23]=1;}
      else if(q[12]==1) {
        if(q[19]!=255||q[18]||q[5]!=1)++bad;
        if(q[13]==0&&q[14]==0) {
          big(r+10,45,2);big(r+18,45,2);r[32]=45;r[33]=fault==37?1:0;
          r[54]=fault==38?0:7;r[56]=0x1f;r[57]=0x3a;
        } else if(q[13]==5&&q[14]==7) {
          big(r+10,6,2);big(r+18,6,2);r[32]=fault==39?5:6;r[33]=5;
          r[34]=0;r[35]='D';r[36]=0;r[37]='A';
        } else if(q[13]==2&&q[14]<8) {
          big(r+10,45,2);big(r+18,45,2);r[32]=45;r[33]=2;r[34]=q[14];r[35]=enabled;r[37]=wp;
          unsigned exponent=0;while((1U<<exponent)<size)++exponent;
          r[42]=uint8_t(exponent);big(r+43,blocks+(fault==18),8);
          if(fault==36)r[6]=0xfc;
        } else ++bad;
      } else ++bad;
    } else if(q[0]==1) {
      memcpy(lastCdb,q+16,16);
      uint8_t op=q[16];uint32_t expected=uint32_t(q[12])<<24|uint32_t(q[13])<<16|uint32_t(q[14])<<8|q[15];
      if(op==0x25) {big(dma+DataOffset,blocks-1>=UINT32_MAX?UINT32_MAX:blocks-1,4);big(dma+DataOffset+4,size,4);}
      else if(op==0x9e) {big(dma+DataOffset,blocks-1,8);big(dma+DataOffset+8,size,4);}
      else if(op==0x28||op==0x88) {++reads;for(uint32_t i=0;i<expected;++i)dma[DataOffset+i]=uint8_t(i*17+23);}
      else if(op==0x2a||op==0x8a) ++writes;
      else if(op==0x35) {for(unsigned i=17;i<32;++i)if(q[i])++bad;}
      else if(op==0) {++turs;if((fault==32&&turs<3)||fault==33){r[6]=1;r[7]=2;big(r+10,20,2);big(r+32,18,2);r[34]=0x70;r[36]=fault==32?6:2;r[46]=fault==32?0x29:4;}}
      else ++bad;
      if(fault==6)little(dma+8,1);
      if(fault==7)r[3]^=1;
      if(fault==8)r[2]^=1;
      if(fault==9)r[0]=0;
      if(fault==10){big(r+10,21,2);big(r+32,19,2);}
      if(fault==14||fault==15){r[1]=0x20;big(r+12,size/2,4);}
      if(fault==26)r[7]=2;
    }
    regs[0x58/4]=0;
  }
  static bool read(void *c,uint32_t off,uint32_t *v){auto &s=*static_cast<Sim*>(c);*v=s.regs[off/4];return true;}
  static bool write(void *c,uint32_t off,uint32_t v){
    auto &s=*static_cast<Sim*>(c);
    if(off==0x20){s.regs[off/4]&=~v;return true;}
    s.regs[off/4]=v;
    if(off==0x58){
      ++s.rings;auto t=s.dma,q=s.dma+UcdOffset;
      uint64_t ucd=uint64_t(le(t+16))|(uint64_t(le(t+20))<<32);
      uint32_t dir=(le(t)>>25)&3, prdCount=le(t+28)&0xffff;
      if(v!=1||ucd!=s.pa+UcdOffset||(le(t)&~0x06000000U)!=0x11000000U||le(t+4)||le(t+8)!=15||le(t+12)||
         le(t+24)!=(128U|((ResponseOffset/4)<<16))||(le(t+28)>>16)!=PrdtOffset/4||((dir!=0)!=(prdCount==1)))++s.bad;
      if(prdCount){auto p=q+PrdtOffset;uint64_t data=uint64_t(le(p))|(uint64_t(le(p+4))<<32);if(data!=s.pa+DataOffset||le(p+8)||le(p+12)>=DataMax)++s.bad;}
      if(s.fault==4||s.fault==27||s.fault==31)return s.fault!=27;
      if(s.fault==5){s.regs[0x58/4]=0;return true;}
      little(t+8,0);s.complete();
      if(s.fault==11)s.regs[0x20/4]|=4;
    }
    return true;
  }
  static bool cache(void *c,void *p,uint32_t n,Cache direction){auto &s=*static_cast<Sim*>(c);auto start=static_cast<uint8_t*>(p);if(start<s.dma||start+n>s.dma+DmaBytes)abort();return !(s.fault==12&&direction==Cache::ToDevice)&&!(s.fault==13&&direction==Cache::FromDevice);}
  static uint64_t clock(void *c){auto &s=*static_cast<Sim*>(c);return s.time;}
  static void delay(void *c,uint32_t n){auto &s=*static_cast<Sim*>(c);if(s.fault!=31)s.time+=(s.fault==4||s.fault==5)?CommandTimeoutUs:n;}
  static bool exclusive(void*){return true;}
  static bool allow(void *c,uint8_t lun,const Geometry &g,uint64_t lba,uint32_t n){auto &s=*static_cast<Sim*>(c);++s.allowCalls;return s.grant&&g.valid&&!g.writeProtected&&lun==2&&lba>=s.windowFirst&&lba-s.windowFirst<8&&n<=8-(lba-s.windowFirst)&&!(s.fault==16&&s.allowCalls>1);}
  Port port(){return Port{this,read,write,cache,clock,delay,exclusive,allow};}
};
extern "C" void ufs_core_case(unsigned scenario,uint64_t *out) {
  auto s=new Sim;Transport host;
  if(scenario==1)s->size=512;
  if(scenario==2)s->pa=0x123450000ULL;
  if(scenario==3)s->blocks=uint64_t(UINT32_MAX)+4096;
  if(scenario==35){s->blocks=uint64_t(UINT32_MAX)+4096;s->windowFirst=uint64_t(UINT32_MAX)+1;}
  if(scenario==17)s->size=1000;
  if(scenario==18)s->fault=18;
  if(scenario==19)s->enabled=false;
  if(scenario==20)s->wp=true;
  if(scenario==30||scenario==32||scenario==33||scenario==36)s->fault=scenario;
  Dma dma{s->dma,s->pa,UINT64_MAX,DmaBytes};
  if(scenario==28)dma.addressLimit=UINT32_MAX,s->pa=dma.device=0x123450000ULL;
  out[0]=uint32_t(host.start(s->port(),dma));
  if(out[0]){out[9]=s->rings;out[11]=s->bad;delete s;return;}
  out[1]=uint32_t(host.initializeDevice());out[2]=uint32_t(host.probe(2));
  out[12]=host.geometry(2)->blockBytes;out[13]=host.geometry(2)->blocks;out[14]=s->queriesSet;out[29]=s->turs;
  if(out[2]){out[7]=host.poisoned();out[9]=s->rings;out[11]=s->bad;out[18]=host.completion().target;delete s;return;}
  uint8_t *buffer=new uint8_t[DataMax];memset(buffer,0xa5,DataMax);
  s->fault=scenario>=4&&scenario<=16?scenario:scenario==26||scenario==27||scenario==31?scenario:0;
  if(scenario==21)s->regs[0x50/4]^=4096;
  if(scenario==22)s->regs[0x58/4]=1;
  if(scenario==34)s->regs[0x20/4]=8;
  s->grant=scenario==15||scenario==16||scenario==25||scenario==26||scenario==27||scenario==35;
  auto snapshot=new uint8_t[DmaBytes];memcpy(snapshot,s->dma,DmaBytes);
  uint32_t before=s->rings;uint32_t count=scenario==1?256:1;uint64_t lba=scenario==3||scenario==35?uint64_t(UINT32_MAX)+1:8;
  if(scenario==24)lba=UINT64_MAX;
  Status status=(scenario==15||scenario==16||scenario==20||scenario==23||scenario==25||scenario==27||scenario==35)?host.write(2,lba,count,buffer):scenario==26?host.synchronize(2):host.read(2,lba,count,buffer);
  out[3]=uint32_t(status);out[4]=s->rings-before;out[5]=host.completion().bytes;out[6]=buffer[0];out[7]=host.poisoned();out[8]=s->lastCdb[0];out[9]=s->rings;out[10]=s->time;out[11]=s->bad;
  out[15]=s->allowCalls;out[16]=buffer[s->size/2];out[17]=s->lastLun;out[18]=s->lastTag;
  out[26]=host.completion().polls;out[30]=memcmp(snapshot,s->dma,DmaBytes)==0;delete[]snapshot;
  if(scenario==0||scenario==1||scenario==2||scenario==3||scenario==25||scenario==35)out[19]=uint32_t(host.synchronize(2));
  else out[19]=uint32_t(host.read(2,8,1,buffer));
  out[20]=s->rings;out[21]=host.releaseAllowed();host.close();out[22]=host.releaseAllowed();s->regs[0x34/4]=0;out[23]=host.releaseAllowed();s->regs[0x34/4]=1;out[24]=host.releaseAllowed();
  if(host.poisoned()){out[27]=uint32_t(host.completion().status);host.synchronize(2);out[28]=uint32_t(host.completion().status);out[31]=s->rings;}
  out[25]=s->bad;delete[]buffer;delete s;
}
extern "C" void ufs_identity_case(unsigned fault,uint64_t *out,uint8_t *identity) {
  auto s=new Sim;Transport host;s->fault=fault;
  Dma dma{s->dma,s->pa,UINT64_MAX,DmaBytes};
  out[0]=uint32_t(host.start(s->port(),dma));
  out[1]=uint32_t(host.initializeDevice());
  uint32_t bytes=0;out[2]=uint32_t(host.readDeviceIdentity(identity,256,bytes));
  out[3]=bytes;out[4]=s->rings;out[5]=s->bad;out[6]=host.poisoned();
  delete s;
}
