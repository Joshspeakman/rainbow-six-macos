// Check CPU source extents when the renderer creates Metal buffers. Metal's own copy is not ASAN-instrumented,
// so this forwarding device copies through instrumented code before sending the same bytes to the real device.
// From src/renderer:
// clang -fobjc-arc -fsanitize=address,undefined -framework Metal -framework QuartzCore -framework Foundation \
//   -I . ../../recomp/tests/renderer_init_test.m renderer.m -o /tmp/renderer_init_test
// /tmp/renderer_init_test
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "renderer.h"

@interface CheckedDevice : NSProxy
@property(nonatomic, strong) id<MTLDevice> realDevice;
@property(nonatomic) NSUInteger copies;
@end
@implementation CheckedDevice
- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
    return [(id)self.realDevice methodSignatureForSelector:selector];
}
- (void)forwardInvocation:(NSInvocation *)invocation
{
    [invocation invokeWithTarget:self.realDevice];
}
- (id<MTLBuffer>)newBufferWithBytes:(const void *)bytes length:(NSUInteger)length options:(MTLResourceOptions)options
{
    void *copy = malloc(length ? length : 1); assert(copy);
    memcpy(copy, bytes, length);
    self.copies++;
    id<MTLBuffer> buffer = [self.realDevice newBufferWithBytes:copy length:length options:options];
    free(copy);
    return buffer;
}
@end

int main(void)
{
    @autoreleasepool {
        CheckedDevice *device = [CheckedDevice alloc];
        device.realDevice = MTLCreateSystemDefaultDevice(); assert(device.realDevice);
        setenv("DSR_RINGMB", "4", 1);
        DSRRenderer *renderer = dsr_renderer_create((id<MTLDevice>)device); assert(renderer);
        assert(device.copies >= 1);
        puts("Renderer buffer initialization: source bounds passed");
    }
    return 0;
}
