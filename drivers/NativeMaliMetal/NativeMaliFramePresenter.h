// SPDX-License-Identifier: MIT
#pragma once
#import <IOSurface/IOSurfaceRef.h>
#include <stdint.h>
// The caller invokes this only after a successfully completed render command
// and CPU cache invalidation. The sink receives a borrowed IOSurface; retain it
// if an asynchronous display transaction outlives the callback. This does not
// program a display controller or promise a vblank.
typedef void (*NativeMaliFrameSink)(void *,IOSurfaceRef,uint64_t);
// Public plugin hook. The context is borrowed and must outlive commands already
// submitted with this sink; set (NULL,NULL) to remove it for future commits.
extern "C" bool native_mali_metal_set_frame_sink(NativeMaliFrameSink,void *);
extern "C" bool native_mali_deliver_completed_frame(IOSurfaceRef,uint64_t,NativeMaliFrameSink,void *);
