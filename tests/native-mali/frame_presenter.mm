// SPDX-License-Identifier: MIT
// CPU-only completion actor: this tests the real frame owner/handoff code, not a GPU.
#import <Foundation/Foundation.h>
#include "NativeMaliFramePresenter.h"
#include <cassert>
@interface FrameCommand : NSObject
@property(nonatomic) MTLCommandBufferStatus status;
@property(nonatomic,strong) id device;
@property(nonatomic,strong) NSMutableArray *handlers;
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler;
- (void)complete:(MTLCommandBufferStatus)status;
@end
@implementation FrameCommand
- (id)init {self=[super init];if(self)_handlers=[NSMutableArray new];return self;}
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler {[_handlers addObject:[handler copy]];}
- (void)complete:(MTLCommandBufferStatus)status {_status=status;for(MTLCommandBufferHandler h in _handlers)h((id)self);[_handlers removeAllObjects];}
@end
@interface FrameTexture : NSObject
@property(nonatomic,strong) id device;
@property(nonatomic) IOSurfaceRef iosurface;
- (NSUInteger)iosurfacePlane;
- (NSUInteger)pixelFormat;
- (NSUInteger)textureType;
- (NSUInteger)sampleCount;
- (NSUInteger)width;
- (NSUInteger)height;
@end
@implementation FrameTexture
- (NSUInteger)iosurfacePlane {return 0;}
- (NSUInteger)pixelFormat {return MTLPixelFormatBGRA8Unorm;}
- (NSUInteger)textureType {return MTLTextureType2D;}
- (NSUInteger)sampleCount {return 1;}
- (NSUInteger)width {return IOSurfaceGetWidth(_iosurface);}
- (NSUInteger)height {return IOSurfaceGetHeight(_iosurface);}
@end
struct Counts {unsigned presented=0,released=0;uint64_t sequence=0;};
static void present(void *c,IOSurfaceRef s,uint64_t seq){auto *v=(Counts *)c;assert(s);v->presented++;v->sequence=seq;}
static void release(void *c){((Counts *)c)->released++;}
int main(){@autoreleasepool {
    IOSurfaceRef surface=IOSurfaceCreate((__bridge CFDictionaryRef)@{(id)kIOSurfaceWidth:@4,(id)kIOSurfaceHeight:@4,
        (id)kIOSurfaceBytesPerElement:@4,(id)kIOSurfaceBytesPerRow:@64,(id)kIOSurfacePixelFormat:@0x42475241});
    assert(surface);id device=[NSObject new];FrameTexture *texture=[FrameTexture new];texture.device=device;texture.iosurface=surface;
    Counts success{},error{},discard{},late{},foreign{};
    @autoreleasepool {FrameCommand *cb=[FrameCommand new];cb.device=device;
        assert(native_mali_present_after_completion((id)cb,(id)texture,42,present,&success,release));
        assert(!success.presented&&!success.released);[cb complete:MTLCommandBufferStatusCompleted];
        assert(success.presented==1&&success.sequence==42&&success.released==1);}
    @autoreleasepool {FrameCommand *cb=[FrameCommand new];cb.device=device;
        assert(native_mali_present_after_completion((id)cb,(id)texture,43,present,&error,release));
        [cb complete:MTLCommandBufferStatusError];assert(!error.presented&&error.released==1);}
    @autoreleasepool {FrameCommand *cb=[FrameCommand new];cb.device=device;
        assert(native_mali_present_after_completion((id)cb,(id)texture,44,present,&discard,release));}
    assert(!discard.presented&&discard.released==1);
    FrameCommand *cb=[FrameCommand new];cb.device=device;cb.status=MTLCommandBufferStatusCommitted;
    assert(!native_mali_present_after_completion((id)cb,(id)texture,45,present,&late,release));assert(!late.released);
    cb.status=MTLCommandBufferStatusNotEnqueued;cb.device=[NSObject new];
    assert(!native_mali_present_after_completion((id)cb,(id)texture,46,present,&foreign,release));assert(!foreign.released);
    CFRelease(surface);puts("PASS: actual IOSurface frame handoff success/error/discard/late/foreign-device; hardware_exercised=false");
}}
