// SPDX-License-Identifier: MIT
// CPU-only IOSurface fixture for the same completed-frame handoff helper.
#import <Foundation/Foundation.h>
#include "NativeMaliFramePresenter.h"
#include <cassert>
struct Counts {unsigned presented=0;uint64_t sequence=0;};
static void present(void *c,IOSurfaceRef s,uint64_t seq){auto *v=(Counts *)c;assert(s);v->presented++;v->sequence=seq;}
int main(){@autoreleasepool {
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@4,(id)kIOSurfaceHeight:@4,
        (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@64,(id)kIOSurfacePixelFormat:@0x42475241});
    assert(surface);Counts completed{};
    assert(native_mali_deliver_completed_frame(surface,42,present,&completed));
    assert(completed.presented==1&&completed.sequence==42);
    IOSurfaceRef wrong=IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@4,(id)kIOSurfaceHeight:@4,
        (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@64,(id)kIOSurfacePixelFormat:@0x52474241});
    assert(wrong&&!native_mali_deliver_completed_frame(wrong,43,present,&completed));
    assert(completed.presented==1&&completed.sequence==42);CFRelease(wrong);
    CFRelease(surface);puts("PASS: completed BGRA IOSurface validation, cache handoff and format rejection; hardware_exercised=false");
}}
