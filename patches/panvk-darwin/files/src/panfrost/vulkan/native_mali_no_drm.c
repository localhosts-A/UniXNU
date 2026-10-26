/* SPDX-License-Identifier: MIT */
#include <stdlib.h>
#include "vk_drm_syncobj.h"
// The native device exclusively selects the CSF CPU/timeline synchronization
// type, whose waits use real NativeMali GPU values. Shared DRM branches remain
// linkable but cannot create a usable DRM synchronization object on Darwin.
void vk_drm_syncobj_finish(struct vk_device *device,struct vk_sync *sync){(void)device;(void)sync;abort();}
struct vk_sync_type vk_drm_syncobj_get_type(int fd){(void)fd;return (struct vk_sync_type){0};}
VkResult vk_drm_syncobj_copy_payloads(struct vk_device *device,uint32_t wait_count,const struct vk_sync_wait *waits,
                                    uint32_t signal_count,const struct vk_sync_signal *signals){
   (void)device;(void)wait_count;(void)waits;(void)signal_count;(void)signals;return VK_ERROR_FEATURE_NOT_PRESENT;
}
