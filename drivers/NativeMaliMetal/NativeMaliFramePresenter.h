// SPDX-License-Identifier: MIT
#pragma once
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#include <stdint.h>
// Register before commit. On true, the callback context is owned until completion
// or command discard, and release(context) runs once. On false it stays caller-owned.
// The receiver gets a borrowed IOSurface after GPU success and cache visibility;
// it must retain it if an asynchronous display transaction outlives the callback.
// This API does not program a display controller or promise a vblank.
extern "C" bool native_mali_present_after_completion(id<MTLCommandBuffer>,id<MTLTexture>,uint64_t sequence,
    void (*present)(void *,IOSurfaceRef,uint64_t),void *context,void (*release)(void *));
