/* SPDX-License-Identifier: MIT
 * Native XNU transport for the CSF-facing pan_kmod API. The kbase helper names
 * are reused by PanVK; every device operation below is a NativeMali IOKit call.
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include "NativeMaliABI.h"
#include "native_mali_kmod.h"
#include "kbase_kmod.h"
#include "pan_kmod_backend.h"
#include "drm-uapi/panthor_drm.h"
#include "util/os_time.h"

struct native_bo {
   struct pan_kmod_bo base;
   uint64_t va;
   uint32_t token;
   void *cpu;
   bool imported;
   struct native_bo *next;
};
struct native_queue {
   uint32_t group, stream, output_token;
   uint64_t ring;
   mach_vm_address_t pages, output;
   struct native_queue *next;
};
struct native_object {
   uint64_t va;
   uint32_t handle;
   struct native_object *next;
};
struct native_dev {
   struct pan_kmod_dev base;
   io_connect_t connection;
   struct nml_response info;
   struct drm_panthor_csif_info csif;
   simple_mtx_t lock;
   int32_t lost;
   struct native_bo *bos;
   struct native_queue *queues;
   struct native_object *aliases, *heaps;
};
extern const struct pan_kmod_ops kbase_kmod_ops;
static struct native_dev *ndev(const struct pan_kmod_dev *dev) { return container_of(dev,struct native_dev,base); }
static struct native_bo *nbo(struct pan_kmod_bo *bo) { return container_of(bo,struct native_bo,base); }
static struct nml_request request(uint32_t op) {
   return (struct nml_request){.version=NML_ABI_VERSION,.size=sizeof(struct nml_request),.operation=op};
}
static int invoke(struct native_dev *dev,const struct nml_request *in,struct nml_response *out) {
   struct nml_response temporary;
   if(!out)out=&temporary;
   size_t bytes=sizeof(*out);
   kern_return_t kr=IOConnectCallStructMethod(dev->connection,NML_SELECTOR_CALL,in,sizeof(*in),out,&bytes);
   if(kr!=KERN_SUCCESS||bytes!=sizeof(*out)||out->version!=NML_ABI_VERSION||out->size!=sizeof(*out)) {
      p_atomic_set(&dev->lost,1);errno=EIO;return -1;
   }
   switch(out->status){
   case NML_OK:return 0;
   case NML_TIMEOUT:errno=ETIMEDOUT;return -1;
   case NML_NO_MEMORY:errno=ENOMEM;return -1;
   case NML_INVALID:errno=EINVAL;return -1;
   case NML_BUSY:errno=EBUSY;return -1;
   case NML_UNSUPPORTED:errno=ENOTSUP;return -1;
   default:p_atomic_set(&dev->lost,1);errno=EIO;return -1;
   }
}
bool native_mali_service_available(void) {
   io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(NML_SERVICE_NAME));
   if(!service)return false;IOObjectRelease(service);return true;
}
static struct pan_kmod_dev *dev_create(int fd,uint32_t flags,const struct pan_kmod_driver *driver,const struct pan_kmod_allocator *allocator) {
   (void)fd;
   struct native_dev *dev=pan_kmod_alloc(allocator,sizeof(*dev));if(!dev)return NULL;
   io_service_t service=IOServiceGetMatchingService(kIOMainPortDefault,IOServiceMatching(NML_SERVICE_NAME));
   if(!service){pan_kmod_free(allocator,dev);errno=ENODEV;return NULL;}
   kern_return_t kr=IOServiceOpen(service,mach_task_self(),0,&dev->connection);IOObjectRelease(service);
   if(kr!=KERN_SUCCESS){pan_kmod_free(allocator,dev);errno=ENODEV;return NULL;}
   struct nml_request q=request(NML_GET_INFO);
   if(invoke(dev,&q,&dev->info))goto error;
   const uint32_t *w=dev->info.words;
   const uint32_t registers=(w[NML_INFO_STREAM_FEATURES]&255)+1,scoreboards=(w[NML_INFO_STREAM_FEATURES]>>8)&255;
   if((w[NML_INFO_GPU_ID]&0xffff0000)!=0xd8300000||registers!=128||!scoreboards||scoreboards>16||
      w[NML_INFO_GROUP_COUNT]<3||w[NML_INFO_STREAM_COUNT]<1) {errno=ENOTSUP;goto error;}
   pan_kmod_dev_init(&dev->base,-1,flags&~PAN_KMOD_DEV_FLAG_OWNS_FD,driver,&kbase_kmod_ops,allocator);
   simple_mtx_init(&dev->lock,mtx_plain);
   struct pan_kmod_dev_props *p=&dev->base.props;
   p->gpu_id=w[NML_INFO_GPU_ID];p->gpu_variant=w[NML_INFO_CORE_FEATURES]&255;
   p->shader_present=(uint64_t)w[NML_INFO_SHADER_HI]<<32|w[NML_INFO_SHADER_LO];
   p->tiler_features=w[NML_INFO_TILER_FEATURES];p->mem_features=w[NML_INFO_MEM_FEATURES];p->mmu_features=w[NML_INFO_MMU_FEATURES];
   p->l2_features=w[NML_INFO_L2_FEATURES];
   for(unsigned i=0;i<4;++i)p->texture_features[i]=w[NML_INFO_TEXTURE_0+i];
   p->max_threads_per_core=w[NML_INFO_MAX_THREADS];p->max_threads_per_wg=w[NML_INFO_MAX_WORKGROUP];
   p->max_tasks_per_core=MAX2(w[NML_INFO_THREAD_FEATURES]>>24,1);p->num_registers_per_core=w[NML_INFO_THREAD_FEATURES]&65535;
   if(!p->num_registers_per_core)p->num_registers_per_core=p->max_threads_per_core*32;
   p->max_tls_instance_per_core=w[NML_INFO_TLS_ALLOC]?w[NML_INFO_TLS_ALLOC]:p->max_threads_per_core;
   p->pgsize_bitmap=PAN_PGSIZE_4K;p->is_io_coherent=false;
   p->allowed_group_priorities_mask=PAN_KMOD_GROUP_ALLOW_PRIORITY_MEDIUM|PAN_KMOD_GROUP_ALLOW_PRIORITY_LOW;
   p->supported_bo_flags=PAN_KMOD_BO_FLAG_EXECUTABLE|PAN_KMOD_BO_FLAG_NO_MMAP|PAN_KMOD_BO_FLAG_GPU_UNCACHED|PAN_KMOD_BO_FLAG_CSF_EVENT;
   // Timestamp register frequency/coherency is deliberately not advertised
   // until the G925 platform counter route is qualified.
   dev->csif=(struct drm_panthor_csif_info){.csg_slot_count=w[NML_INFO_GROUP_COUNT],.cs_slot_count=w[NML_INFO_STREAM_COUNT],
      .cs_reg_count=registers,.scoreboard_slot_count=scoreboards,.unpreserved_cs_reg_count=4};
   return &dev->base;
error:
   IOServiceClose(dev->connection);pan_kmod_free(allocator,dev);return NULL;
}
static void unmap_queue(struct native_dev *dev,struct native_queue *q) {
   if(q->output)IOConnectUnmapMemory64(dev->connection,q->output_token,mach_task_self(),q->output);
   if(q->pages)mach_vm_deallocate(mach_task_self(),q->pages,8192);
}
static void dev_destroy(struct pan_kmod_dev *base) {
   struct native_dev *dev=ndev(base);
   while(dev->queues){struct native_queue *q=dev->queues;dev->queues=q->next;unmap_queue(dev,q);pan_kmod_dev_free(base,q);}
   while(dev->aliases){struct native_object *o=dev->aliases;dev->aliases=o->next;pan_kmod_dev_free(base,o);}
   while(dev->heaps){struct native_object *o=dev->heaps;dev->heaps=o->next;pan_kmod_dev_free(base,o);}
   while(dev->bos){struct native_bo *bo=dev->bos;dev->bos=bo->next;
      if(bo->cpu&&!bo->imported)IOConnectUnmapMemory64(dev->connection,bo->token,mach_task_self(),(mach_vm_address_t)bo->cpu);
      pan_kmod_dev_free(base,bo);}
   IOServiceClose(dev->connection);simple_mtx_destroy(&dev->lock);
   const struct pan_kmod_allocator *allocator=base->allocator;pan_kmod_dev_cleanup(base);pan_kmod_free(allocator,dev);
}
static struct pan_kmod_va_range va_range(const struct pan_kmod_dev *base) {
   const struct native_dev *dev=ndev(base);return (struct pan_kmod_va_range){.start=0x10000000ull,.size=dev->info.address+dev->info.length-0x10000000ull};
}
static struct pan_kmod_bo *alloc_bo(struct pan_kmod_dev *base,struct pan_kmod_vm *vm,uint64_t size,uint32_t flags,void *host) {
   struct native_dev *dev=ndev(base);if(!size||size>UINT64_MAX-4095||flags&~base->props.supported_bo_flags){errno=EINVAL;return NULL;}
   size=(size+4095)&~UINT64_C(4095);
   struct native_bo *bo=pan_kmod_dev_alloc(base,sizeof(*bo));if(!bo)return NULL;
   struct nml_request q=request(host?NML_BUFFER_IMPORT:NML_BUFFER_ALLOC);
   q.length=size;q.address=(uintptr_t)host;
   if(flags&PAN_KMOD_BO_FLAG_EXECUTABLE)q.flags|=NML_BUFFER_EXECUTABLE;
   if(flags&(PAN_KMOD_BO_FLAG_GPU_UNCACHED|PAN_KMOD_BO_FLAG_CSF_EVENT))q.flags|=NML_BUFFER_UNCACHED;
   struct nml_response out;
   if(invoke(dev,&q,&out)){pan_kmod_dev_free(base,bo);return NULL;}
   pan_kmod_bo_init(&bo->base,base,vm,size,flags|(host?PAN_KMOD_BO_FLAG_IMPORTED:0),out.object);bo->va=out.address;bo->token=out.auxiliary;
   bo->imported=host!=NULL;bo->cpu=host;
   simple_mtx_lock(&dev->lock);bo->next=dev->bos;dev->bos=bo;simple_mtx_unlock(&dev->lock);return &bo->base;
}
static struct pan_kmod_bo *bo_alloc(struct pan_kmod_dev *base,struct pan_kmod_vm *vm,uint64_t size,uint32_t flags){return alloc_bo(base,vm,size,flags,NULL);}
static void *bo_mmap(struct pan_kmod_bo *base,int prot,int flags,void *placed) {
   (void)flags;struct native_bo *bo=nbo(base);struct native_dev *dev=ndev(base->dev);
   if(placed||!(prot&PROT_READ)){errno=ENOTSUP;return MAP_FAILED;}
   simple_mtx_lock(&dev->lock);
   if(!bo->cpu){mach_vm_address_t address=0;mach_vm_size_t bytes=base->size;
      if(IOConnectMapMemory64(dev->connection,bo->token,mach_task_self(),&address,&bytes,kIOMapAnywhere|kIOMapWriteCombineCache)!=KERN_SUCCESS||bytes<base->size){
         if(address)IOConnectUnmapMemory64(dev->connection,bo->token,mach_task_self(),address);
         simple_mtx_unlock(&dev->lock);errno=ENOMEM;return MAP_FAILED;}bo->cpu=(void *)(uintptr_t)address;}
   void *cpu=bo->cpu;simple_mtx_unlock(&dev->lock);return cpu;
}
static int bo_munmap(struct pan_kmod_bo *bo,void *address,size_t size) {
   // Like AUTO_VA mappings, CPU mappings are owned by the BO's lifetime.
   return address==nbo(bo)->cpu&&size<=bo->size?0:-1;
}
static void bo_free(struct pan_kmod_bo *base) {
   struct native_bo *bo=nbo(base);struct native_dev *dev=ndev(base->dev);pan_kmod_bo_cleanup(base);
   if(bo->cpu&&!bo->imported){IOConnectUnmapMemory64(dev->connection,bo->token,mach_task_self(),(mach_vm_address_t)bo->cpu);bo->cpu=NULL;}
   struct nml_request q=request(NML_BUFFER_FREE);q.object=base->handle;
   if(invoke(dev,&q,NULL)){p_atomic_set(&dev->lost,1);return;} // Kernel retains on uncertain DMA; keep metadata until close.
   simple_mtx_lock(&dev->lock);struct native_bo **link=&dev->bos;while(*link&&*link!=bo)link=&(*link)->next;
   if(*link)*link=bo->next;
   simple_mtx_unlock(&dev->lock);pan_kmod_dev_free(base->dev,bo);
}
struct pan_kmod_bo *kbase_kmod_bo_import_host(struct pan_kmod_dev *base,void *host,uint64_t size) {
   if(!(ndev(base)->info.words[NML_INFO_CAPABILITIES]&NML_CAP_HOST_IMPORT)){errno=ENOTSUP;return NULL;}
   struct pan_kmod_bo *bo=alloc_bo(base,NULL,size,0,host);
   if(bo){struct pan_kmod_bo **slot=util_sparse_array_get(&base->handle_to_bo.array,bo->handle);
      if(!slot){bo_free(bo);return NULL;}*slot=bo;}return bo;
}
static struct pan_kmod_bo *bo_import(struct pan_kmod_dev *dev,uint32_t handle,uint64_t size){(void)dev;(void)handle;(void)size;errno=ENOTSUP;return NULL;}
static int bo_export(struct pan_kmod_bo *bo){(void)bo;errno=ENOTSUP;return -1;}
static off_t bo_offset(struct pan_kmod_bo *bo){(void)bo;errno=ENOTSUP;return -1;}
static bool bo_wait(struct pan_kmod_bo *bo,int64_t timeout,bool read_only){
   (void)bo;(void)timeout;(void)read_only;errno=ENOTSUP;return false; // No implicit BO fence is claimed.
}
static int flush_syncs(struct pan_kmod_dev *dev){struct nml_request q=request(NML_EVENT_SIGNAL);return invoke(ndev(dev),&q,NULL);}
static struct pan_kmod_vm *vm_create(struct pan_kmod_dev *base,uint32_t flags,uint64_t start,uint64_t size){
   (void)start;(void)size;if(!(flags&PAN_KMOD_VM_FLAG_AUTO_VA)){errno=ENOTSUP;return NULL;}
   struct pan_kmod_vm *vm=pan_kmod_dev_alloc(base,sizeof(*vm));if(vm)pan_kmod_vm_init(vm,base,0,flags);return vm;
}
static void vm_destroy(struct pan_kmod_vm *vm){pan_kmod_vm_cleanup(vm);pan_kmod_dev_free(vm->dev,vm);}
static int vm_bind(struct pan_kmod_vm *vm,enum pan_kmod_vm_op_mode mode,struct pan_kmod_vm_op *ops,uint32_t count){
   if(mode==PAN_KMOD_VM_OP_MODE_ASYNC){errno=ENOTSUP;return -1;}
   for(uint32_t i=0;i<count;++i){struct pan_kmod_vm_op *op=ops+i;
      if(pan_kmod_vm_op_check(vm,mode,op))return -1;
      if(op->type==PAN_KMOD_VM_OP_TYPE_MAP){
         if(!op->map.bo||op->map.bo->dev!=vm->dev||op->map.bo_offset>op->map.bo->size||op->va.size>op->map.bo->size-op->map.bo_offset){errno=EINVAL;return -1;}
         op->va.start=nbo(op->map.bo)->va+op->map.bo_offset;
      }else if(op->type!=PAN_KMOD_VM_OP_TYPE_UNMAP&&op->type!=PAN_KMOD_VM_OP_TYPE_SYNC_ONLY){errno=ENOTSUP;return -1;}
      // Native AUTO_VA mappings are established at BO allocation and removed
      // at BO free, matching the existing kbase lifetime contract.
   }return 0;
}
static enum pan_kmod_vm_state vm_state(struct pan_kmod_vm *vm){return p_atomic_read(&ndev(vm->dev)->lost)?PAN_KMOD_VM_FAULTY:PAN_KMOD_VM_USABLE;}
static uint64_t timestamp(const struct pan_kmod_dev *base){struct nml_request q=request(NML_TIMESTAMP);struct nml_response out;return invoke(ndev(base),&q,&out)?0:out.value;}
const struct drm_panthor_csif_info *kbase_kmod_get_csif_props(const struct pan_kmod_dev *dev){return &ndev(dev)->csif;}
uint32_t kbase_kmod_get_flush_id(const struct pan_kmod_dev *base){struct nml_request q=request(NML_FLUSH_ID);struct nml_response out;return invoke(ndev(base),&q,&out)?0:out.value;}
bool kbase_kmod_supports_dmabuf(const struct pan_kmod_dev *dev){(void)dev;return false;}
uint64_t kbase_kmod_page_size(void){return NML_PAGE_SIZE;}
int kbase_kmod_csf_group_create(struct pan_kmod_dev *dev,uint32_t streams,uint32_t *handle){struct nml_request q=request(NML_GROUP_CREATE);q.value=streams;struct nml_response out;
   if(invoke(ndev(dev),&q,&out))return -1;*handle=out.object;return 0;}
void kbase_kmod_csf_group_destroy(struct pan_kmod_dev *dev,uint32_t handle){struct nml_request q=request(NML_GROUP_DESTROY);q.object=handle;invoke(ndev(dev),&q,NULL);}
void *kbase_kmod_csf_queue_bind(struct pan_kmod_dev *base,uint32_t group,uint32_t stream,uint64_t ring,uint32_t size){
   struct native_dev *dev=ndev(base);struct native_queue *queue=pan_kmod_dev_alloc(base,sizeof(*queue));if(!queue)return NULL;
   simple_mtx_lock(&dev->lock);struct native_bo *bo=dev->bos;while(bo&&bo->va!=ring)bo=bo->next;
   if(!bo){simple_mtx_unlock(&dev->lock);pan_kmod_dev_free(base,queue);errno=EINVAL;return NULL;}
   struct nml_request q=request(NML_QUEUE_BIND);q.object=group;q.parent=bo->base.handle;q.value=stream;q.length=size;struct nml_response out;
   if(invoke(dev,&q,&out)){simple_mtx_unlock(&dev->lock);pan_kmod_dev_free(base,queue);return NULL;}
   queue->group=group;queue->stream=stream;queue->ring=ring;queue->output_token=out.auxiliary;
   mach_vm_address_t pages=0;bool mapped=false;
   if(mach_vm_allocate(mach_task_self(),&pages,12288,VM_FLAGS_ANYWHERE)==KERN_SUCCESS){
      queue->pages=pages;mach_vm_deallocate(mach_task_self(),pages+8192,4096);
      mach_vm_address_t output=pages+8192;mach_vm_size_t bytes=4096;
      if(IOConnectMapMemory64(dev->connection,queue->output_token,mach_task_self(),&output,&bytes,kIOMapReadOnly|kIOMapWriteCombineCache)==KERN_SUCCESS){
         queue->output=output;mapped=output==pages+8192&&bytes==4096;
      }
   }
   if(!mapped){unmap_queue(dev,queue);q=request(NML_QUEUE_UNBIND);q.object=group;q.value=stream;invoke(dev,&q,NULL);
      simple_mtx_unlock(&dev->lock);pan_kmod_dev_free(base,queue);errno=ENOMEM;return NULL;}
   // Page0 is deliberately inaccessible; native submission must always call
   // the kernel transport and cannot accidentally use kbase's MMIO fast path.
   mprotect((void *)(uintptr_t)pages,4096,PROT_NONE);
   queue->next=dev->queues;dev->queues=queue;simple_mtx_unlock(&dev->lock);return (void *)(uintptr_t)pages;
}
void kbase_kmod_csf_queue_term(struct pan_kmod_dev *base,uint64_t ring,void *user_io){
   struct native_dev *dev=ndev(base);simple_mtx_lock(&dev->lock);struct native_queue **link=&dev->queues;
   while(*link&&((*link)->ring!=ring||(*link)->pages!=(mach_vm_address_t)user_io))link=&(*link)->next;
   if(*link){struct native_queue *queue=*link;struct nml_request q=request(NML_QUEUE_UNBIND);q.object=queue->group;q.value=queue->stream;
      unmap_queue(dev,queue);invoke(dev,&q,NULL);*link=queue->next;pan_kmod_dev_free(base,queue);}
   simple_mtx_unlock(&dev->lock);
}
int kbase_kmod_csf_queue_kick(struct pan_kmod_dev *base,uint64_t ring){
   struct native_dev *dev=ndev(base);simple_mtx_lock(&dev->lock);struct native_queue *queue=dev->queues;while(queue&&queue->ring!=ring)queue=queue->next;
   if(!queue){simple_mtx_unlock(&dev->lock);errno=EINVAL;return -1;}
   struct nml_request q=request(NML_QUEUE_KICK);q.object=queue->group;q.offset=queue->stream;
   q.value=__atomic_load_n((uint64_t *)(uintptr_t)(queue->pages+4096),__ATOMIC_ACQUIRE);
   const int result=invoke(dev,&q,NULL);simple_mtx_unlock(&dev->lock);return result;
}
int kbase_kmod_csf_wait_event(struct pan_kmod_dev *dev,int64_t timeout){struct nml_request q=request(NML_POLL);int r=invoke(ndev(dev),&q,NULL);
   if(!r&&timeout>0)os_time_sleep(MIN2(timeout/1000,1000));return r;}
int kbase_kmod_csf_wait_cqs64(struct pan_kmod_dev *dev,uint64_t address,uint64_t target,int64_t timeout){
   struct nml_request q=request(NML_WAIT_VALUE);q.address=address;q.value=target;q.timeout_ns=timeout<0?50000000:MIN2((uint64_t)timeout,50000000);
   if(!invoke(ndev(dev),&q,NULL))return 1;return errno==ETIMEDOUT?0:-1;
}
int kbase_kmod_csf_export_cqs64_fence(struct pan_kmod_dev *dev,const uint64_t *addresses,const uint64_t *targets,unsigned count){
   (void)dev;(void)addresses;(void)targets;(void)count;errno=ENOTSUP;return -1;
}
bool kbase_kmod_csf_has_error(const struct pan_kmod_dev *base){struct native_dev *dev=ndev(base);struct nml_request q=request(NML_POLL);return p_atomic_read(&dev->lost)||invoke(dev,&q,NULL)!=0;}
int kbase_kmod_csf_tiler_heap_create(struct pan_kmod_dev *base,uint32_t size,uint32_t initial,uint32_t maximum,uint32_t target,uint32_t mem_group,uint64_t *context,uint64_t *first){
   if(mem_group){errno=ENOTSUP;return -1;}struct native_dev *dev=ndev(base);struct native_object *heap=pan_kmod_dev_alloc(base,sizeof(*heap));if(!heap)return -1;
   struct nml_request q=request(NML_HEAP_CREATE);q.length=size;q.value=initial;q.offset=maximum;q.address=target;struct nml_response out;
   if(invoke(dev,&q,&out)){pan_kmod_dev_free(base,heap);return -1;}
   heap->handle=out.object;heap->va=out.address;simple_mtx_lock(&dev->lock);heap->next=dev->heaps;dev->heaps=heap;simple_mtx_unlock(&dev->lock);
   *context=out.address;*first=out.value;return 0;
}
static void destroy_object(struct pan_kmod_dev *base,struct native_object **head,uint64_t va,uint32_t operation){
   struct native_dev *dev=ndev(base);simple_mtx_lock(&dev->lock);struct native_object **link=head;while(*link&&(*link)->va!=va)link=&(*link)->next;
   if(*link){struct native_object *object=*link;struct nml_request q=request(operation);q.object=object->handle;
      invoke(dev,&q,NULL);*link=object->next;pan_kmod_dev_free(base,object);}simple_mtx_unlock(&dev->lock);
}
void kbase_kmod_csf_tiler_heap_destroy(struct pan_kmod_dev *dev,uint64_t va){destroy_object(dev,&ndev(dev)->heaps,va,NML_HEAP_DESTROY);}
uint64_t kbase_kmod_alias_create(struct pan_kmod_dev *base,uint64_t va,uint64_t bytes,uint32_t repeats){
   struct native_dev *dev=ndev(base);struct native_object *alias=pan_kmod_dev_alloc(base,sizeof(*alias));if(!alias)return 0;
   simple_mtx_lock(&dev->lock);struct native_bo *bo=dev->bos;while(bo&&!(va>=bo->va&&va-bo->va<bo->base.size))bo=bo->next;
   if(!bo){simple_mtx_unlock(&dev->lock);pan_kmod_dev_free(base,alias);return 0;}
   struct nml_request q=request(NML_ALIAS_CREATE);q.parent=bo->base.handle;q.offset=va-bo->va;q.length=bytes;q.value=repeats;struct nml_response out;
   if(invoke(dev,&q,&out)){simple_mtx_unlock(&dev->lock);pan_kmod_dev_free(base,alias);return 0;}
   alias->handle=out.object;alias->va=out.address;alias->next=dev->aliases;dev->aliases=alias;simple_mtx_unlock(&dev->lock);return out.address;
}
void kbase_kmod_alias_destroy(struct pan_kmod_dev *dev,uint64_t va,uint64_t size,uint32_t count){(void)size;(void)count;destroy_object(dev,&ndev(dev)->aliases,va,NML_BUFFER_FREE);}
// Shared PanVK contains older-JM and DRM branches. Only a G925 CSF device can
// enumerate here; entering any of these unsupported paths is a device error.
static void unsupported_path(struct pan_kmod_dev *dev){p_atomic_set(&ndev(dev)->lost,1);errno=ENOTSUP;}
uint64_t kbase_kmod_job_submit(struct pan_kmod_dev *dev,uint64_t jc,uint32_t req,struct pan_kmod_bo **bos,uint32_t nbo,
                             struct base_external_resource *ext,uint32_t next){
   (void)jc;(void)req;(void)bos;(void)nbo;(void)ext;(void)next;unsupported_path(dev);return 0;}
uint64_t kbase_kmod_job_submit_dep(struct pan_kmod_dev *dev,uint64_t jc,uint32_t req,struct pan_kmod_bo **bos,uint32_t nbo,
                                 uint64_t dep0,uint8_t type0,uint64_t dep1,uint8_t type1,bool no_waiter){
   (void)jc;(void)req;(void)bos;(void)nbo;(void)dep0;(void)type0;(void)dep1;(void)type1;(void)no_waiter;unsupported_path(dev);return 0;}
bool kbase_kmod_job_submit_retry(struct pan_kmod_dev *dev,uint64_t jc,uint32_t req,struct pan_kmod_bo **bos,uint32_t nbo,
                               struct base_external_resource *ext,uint32_t next,int attempts){
   (void)attempts;kbase_kmod_job_submit(dev,jc,req,bos,nbo,ext,next);return false;}
bool kbase_kmod_wait_atom(struct pan_kmod_dev *dev,uint64_t atom,int64_t timeout){(void)atom;(void)timeout;unsupported_path(dev);return false;}
void kbase_kmod_atom_release(struct pan_kmod_dev *dev,uint64_t atom){(void)atom;unsupported_path(dev);}
void kbase_kmod_jm_clean_imports(struct pan_kmod_dev *dev){unsupported_path(dev);}
int panthor_kmod_bo_attach_sync_point(struct pan_kmod_bo *bo,uint32_t handle,uint64_t point,bool written){
   (void)handle;(void)point;(void)written;unsupported_path(bo->dev);return -1;}
int panthor_kmod_bo_get_sync_point(struct pan_kmod_bo *bo,uint32_t *handle,uint64_t *point,bool read_only){
   (void)handle;(void)point;(void)read_only;unsupported_path(bo->dev);return -1;}
uint32_t panthor_kmod_vm_sync_handle(struct pan_kmod_vm *vm){unsupported_path(vm->dev);return 0;}
uint64_t panthor_kmod_vm_sync_lock(struct pan_kmod_vm *vm){unsupported_path(vm->dev);return 0;}
void panthor_kmod_vm_sync_unlock(struct pan_kmod_vm *vm,uint64_t point){(void)point;unsupported_path(vm->dev);}
uint32_t panthor_kmod_get_flush_id(const struct pan_kmod_dev *dev){unsupported_path((struct pan_kmod_dev *)dev);return 0;}
const struct drm_panthor_csif_info *panthor_kmod_get_csif_props(const struct pan_kmod_dev *dev){unsupported_path((struct pan_kmod_dev *)dev);return NULL;}
const struct pan_kmod_ops kbase_kmod_ops={
   .dev_create=dev_create,.dev_destroy=dev_destroy,.dev_query_user_va_range=va_range,.bo_alloc=bo_alloc,.bo_free=bo_free,
   .bo_import=bo_import,.bo_export_fd=bo_export,.bo_get_mmap_offset=bo_offset,.bo_mmap=bo_mmap,.bo_munmap=bo_munmap,
   .flush_bo_map_syncs=flush_syncs,.cs_event_signal=flush_syncs,.bo_wait=bo_wait,.vm_create=vm_create,.vm_destroy=vm_destroy,
   .vm_bind=vm_bind,.vm_query_state=vm_state,.query_timestamp=timestamp,
};
