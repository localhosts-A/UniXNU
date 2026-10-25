// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qtest executes production commands; replaces only hardware/cache/time/policy.
#include "UfsTransport.h"
#include <cstring>
using namespace unixnu_ufs;
using Callback = uint64_t (*)(unsigned,uint64_t,uint64_t,void*);
struct Bridge {
  Callback io; uint64_t physical;
  alignas(4096) uint8_t memory[DmaBytes]{};
  Transport host;
  static bool read(void *c,uint32_t off,uint32_t *v){auto b=static_cast<Bridge*>(c);uint64_t n=b->io(0,off,0,nullptr);*v=uint32_t(n);return n!=UINT64_MAX;}
  static bool write(void *c,uint32_t off,uint32_t v){return static_cast<Bridge*>(c)->io(1,off,v,nullptr)==0;}
  static bool cache(void *c,void *p,uint32_t bytes,Cache operation){auto b=static_cast<Bridge*>(c);uint64_t offset=static_cast<uint8_t*>(p)-b->memory;return b->io(operation==Cache::ToDevice?2:3,b->physical+offset,bytes,p)==0;}
  static uint64_t clock(void *c){return static_cast<Bridge*>(c)->io(4,0,0,nullptr);}
  static void delay(void *c,uint32_t us){static_cast<Bridge*>(c)->io(5,0,us,nullptr);}
  static bool exclusive(void *c){return static_cast<Bridge*>(c)->io(6,0,0,nullptr)==1;}
  static bool allow(void *c,uint8_t lun,const Geometry &g,uint64_t lba,uint32_t blocks){return static_cast<Bridge*>(c)->io(7,lba,(uint64_t(lun)<<32)|blocks,const_cast<Geometry*>(&g))==1;}
};
extern "C" {
void *ufs_create(Callback callback,uint64_t physical,uint32_t *status){
  auto b=new Bridge;b->io=callback;b->physical=physical;
  Port p{b,Bridge::read,Bridge::write,Bridge::cache,Bridge::clock,Bridge::delay,Bridge::exclusive,Bridge::allow};
  *status=uint32_t(b->host.start(p,Dma{b->memory,physical,UINT64_MAX,DmaBytes}));return b;
}
uint32_t ufs_init(void *p){return uint32_t(static_cast<Bridge*>(p)->host.initializeDevice());}
uint32_t ufs_probe(void *p,uint8_t lun){return uint32_t(static_cast<Bridge*>(p)->host.probe(lun));}
uint64_t ufs_geometry(void *p,uint8_t lun,unsigned field){auto g=static_cast<Bridge*>(p)->host.geometry(lun);return !g?0:field==0?g->blocks:field==1?g->blockBytes:g->valid;}
uint32_t ufs_read(void *p,uint8_t lun,uint64_t lba,uint32_t blocks,void *out){return uint32_t(static_cast<Bridge*>(p)->host.read(lun,lba,blocks,out));}
uint32_t ufs_write(void *p,uint8_t lun,uint64_t lba,uint32_t blocks,const void *in){return uint32_t(static_cast<Bridge*>(p)->host.write(lun,lba,blocks,in));}
uint32_t ufs_sync(void *p,uint8_t lun){return uint32_t(static_cast<Bridge*>(p)->host.synchronize(lun));}
bool ufs_poisoned(void *p){return static_cast<Bridge*>(p)->host.poisoned();}
void ufs_close(void *p){static_cast<Bridge*>(p)->host.close();}
bool ufs_destroy(void *p){auto b=static_cast<Bridge*>(p);if(!b->host.releaseAllowed())return false;delete b;return true;}
}
