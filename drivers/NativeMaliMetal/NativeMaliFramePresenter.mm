// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#include "NativeMaliFramePresenter.h"
#include <unistd.h>
extern "C" void sys_dcache_flush(void *,size_t);
extern "C" bool native_mali_deliver_completed_frame(IOSurfaceRef surface,uint64_t sequence,NativeMaliFrameSink present,void *context) {
    if(!surface||!present||IOSurfaceGetPlaneCount(surface))return false;
    const size_t width=IOSurfaceGetWidth(surface),height=IOSurfaceGetHeight(surface),row=IOSurfaceGetBytesPerRow(surface),bytes=IOSurfaceGetAllocSize(surface);
    void *base=IOSurfaceGetBaseAddress(surface);
    if(IOSurfaceGetPixelFormat(surface)!=0x42475241||!width||!height||width>SIZE_MAX/4||row<width*4||
       (row&63)||height>SIZE_MAX/row||row*height>bytes||!base||((uintptr_t)base&4095))return false;
    sys_dcache_flush(base,bytes);
    present(context,surface,sequence);
    return true;
}
