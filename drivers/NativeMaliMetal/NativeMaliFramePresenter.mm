// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#include "NativeMaliFramePresenter.h"
#include <unistd.h>
extern "C" void sys_dcache_flush(void *,size_t);
typedef void (*NMMRelease)(void *);

@interface NMMFrameOwner : NSObject
@property(nonatomic) IOSurfaceRef surface;
@property(nonatomic) void *context;
@property(nonatomic) NMMRelease releaseContext;
- (void)finish;
@end
@implementation NMMFrameOwner
- (void)finish {IOSurfaceRef s=_surface;NMMRelease r=_releaseContext;void *c=_context;
    _surface=nullptr;_releaseContext=nullptr;_context=nullptr;if(s)CFRelease(s);if(r)r(c);}
- (void)dealloc {[self finish];}
@end

extern "C" bool native_mali_present_after_completion(id<MTLCommandBuffer> command,id<MTLTexture> texture,uint64_t sequence,
    void (*present)(void *,IOSurfaceRef,uint64_t),void *context,void (*release)(void *)) {
    if(!command||!texture||!present||!release||
       command.status>MTLCommandBufferStatusEnqueued||command.device!=texture.device)return false;
    IOSurfaceRef surface=texture.iosurface;
    if(!surface||IOSurfaceGetPlaneCount(surface)||texture.iosurfacePlane||
       texture.pixelFormat!=MTLPixelFormatBGRA8Unorm||texture.textureType!=MTLTextureType2D||
       texture.sampleCount!=1||texture.width!=IOSurfaceGetWidth(surface)||texture.height!=IOSurfaceGetHeight(surface))return false;
    const size_t width=texture.width,height=texture.height,row=IOSurfaceGetBytesPerRow(surface),bytes=IOSurfaceGetAllocSize(surface);
    void *base=IOSurfaceGetBaseAddress(surface);
    if(IOSurfaceGetPixelFormat(surface)!=0x42475241||!width||!height||width>SIZE_MAX/4||row<width*4||
       (row&63)||height>SIZE_MAX/row||row*height>bytes||!base||((uintptr_t)base&4095))return false;
    NMMFrameOwner *owner=[NMMFrameOwner new];owner.surface=(IOSurfaceRef)CFRetain(surface);owner.context=context;owner.releaseContext=release;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed){
        if(owner.surface&&completed.status==MTLCommandBufferStatusCompleted){
            sys_dcache_flush(IOSurfaceGetBaseAddress(owner.surface),IOSurfaceGetAllocSize(owner.surface));
            present(owner.context,owner.surface,sequence);
        }
        [owner finish];
    }];
    return true;
}
