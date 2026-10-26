/* SPDX-License-Identifier: MIT
 * Compile-time types for shared PanVK fallback paths in a NativeMali-only
 * build. Darwin has no DRM service. Every DRM operation explicitly fails.
 * No descriptor returned by NativeMali is a DRM file descriptor.
 */
#pragma once
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include "drm-uapi/drm.h"
#define DRM_NODE_PRIMARY 0
#define DRM_NODE_CONTROL 1
#define DRM_NODE_RENDER 2
#define DRM_NODE_MAX 3
#define DRM_BUS_PLATFORM 2
#define DRM_SYNCOBJ_CREATE_SIGNALED 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL 1
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT 2
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE 4
typedef struct _drmVersion { int version_major,version_minor,version_patchlevel; char *name; } drmVersion,*drmVersionPtr;
typedef struct _drmDevice { char **nodes;int available_nodes,bustype; } drmDevice,*drmDevicePtr;
static inline int native_mali_no_drm(void){errno=ENOTSUP;return -1;}
static inline drmVersionPtr drmGetVersion(int fd){(void)fd;errno=ENOTSUP;return NULL;}
static inline void drmFreeVersion(drmVersionPtr version){(void)version;}
static inline int drmGetDevices2(uint32_t flags,drmDevicePtr *devices,int count){(void)flags;(void)devices;(void)count;return native_mali_no_drm();}
static inline void drmFreeDevices(drmDevicePtr *devices,int count){(void)devices;(void)count;}
static inline void drmFreeDevice(drmDevicePtr *device){(void)device;}
#define drmGetDevice2(...) native_mali_no_drm()
#define drmDevicesEqual(...) 0
#define drmIoctl(...) native_mali_no_drm()
#define drmPrimeFDToHandle(...) native_mali_no_drm()
#define drmPrimeHandleToFD(...) native_mali_no_drm()
#define drmCloseBufferHandle(...) native_mali_no_drm()
#define drmSyncobjCreate(...) native_mali_no_drm()
#define drmSyncobjDestroy(...) native_mali_no_drm()
#define drmSyncobjWait(...) native_mali_no_drm()
#define drmSyncobjReset(...) native_mali_no_drm()
#define drmSyncobjSignal(...) native_mali_no_drm()
#define drmSyncobjTimelineWait(...) native_mali_no_drm()
#define drmSyncobjTimelineSignal(...) native_mali_no_drm()
#define drmSyncobjTransfer(...) native_mali_no_drm()
#define drmSyncobjQuery(...) native_mali_no_drm()
