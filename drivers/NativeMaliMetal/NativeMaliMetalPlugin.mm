// SPDX-License-Identifier: MIT
// Metal's bundle principal class. GPU work uses the NativeMali PanVK ICD only.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <unistd.h>
#include "NativeBridgeMetalFrontend.h"

@interface NativeMaliMetalDevice : NSObject
- (id)initWithAcceleratorPort:(unsigned)port;
@end

extern "C" id native_mali_metal_open(unsigned port,const char *loader,const char *cache) {
    if(!port||!loader||!cache||getpagesize()!=4096)return nil;
    void *iokit=dlopen("/System/Library/Frameworks/IOKit.framework/IOKit",RTLD_NOW|RTLD_LOCAL);
    if(!iokit)return nil;
    auto conforms=(bool (*)(unsigned,const char *))dlsym(iokit,"IOObjectConformsTo");
    auto registryID=(kern_return_t (*)(unsigned,uint64_t *))dlsym(iokit,"IORegistryEntryGetRegistryEntryID");
    uint64_t identity=0;
    const bool valid=conforms&&registryID&&
        (conforms(port,"DaliMaliAccelerator")||conforms(port,"DaliMali"))&&
        registryID(port,&identity)==KERN_SUCCESS&&identity;
    dlclose(iokit);
    if(!valid)return nil;
    // Check the exact ICD rather than falling back to a platform Vulkan loader.
    void *driver=dlopen(loader,RTLD_NOW|RTLD_LOCAL);
    if(!driver)return nil;
    const bool exports=dlsym(driver,"vkGetInstanceProcAddr")!=nullptr;
    dlclose(driver);
    if(!exports)return nil;
    return native_bridge_metal_open(loader,cache,identity,nullptr);
}

@implementation NativeMaliMetalDevice
- (id)initWithAcceleratorPort:(unsigned)port {
    self=[super init];if(!self)return nil;
    NSBundle *bundle=[NSBundle bundleForClass:[NativeMaliMetalDevice class]];
    NSString *loader=[bundle.bundlePath stringByAppendingPathComponent:@"libvulkan_native_mali.dylib"];
    NSArray<NSURL *> *paths=[[NSFileManager defaultManager] URLsForDirectory:NSCachesDirectory inDomains:NSUserDomainMask];
    NSURL *cache=[paths.firstObject URLByAppendingPathComponent:@"NativeMaliMetal" isDirectory:YES];
    NSError *error=nil;
    if(!cache||![[NSFileManager defaultManager] createDirectoryAtURL:cache withIntermediateDirectories:YES attributes:nil error:&error]) {
        NSLog(@"NativeMaliMetal: cache unavailable: %@",error);return nil;
    }
    id device=native_mali_metal_open(port,loader.fileSystemRepresentation,cache.path.fileSystemRepresentation);
    if(!device)NSLog(@"NativeMaliMetal: device open refused");
    return device;
}
@end
