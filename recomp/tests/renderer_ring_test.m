// A reduced upload ring must grow for a larger upload/draw instead of returning an out-of-bounds range.
// Run from src/renderer after compiling with Metal, Foundation and QuartzCore.
#import "../../src/renderer/renderer.m"
#include <assert.h>

int main(void)
{
    @autoreleasepool {
        setenv("DSR_RINGMB", "4", 1);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); assert(dev);
        DSRRenderer *r = dsr_renderer_create(dev); assert(r && r->ring_size == (4u << 20));
        const uint32_t n = 2400 * 600 * 4; uint32_t off;
        uint8_t *p = ring_alloc(r, n, &off);
        // Check before writing, so the old implementation fails without corrupting driver memory.
        assert((uint64_t)off + n <= r->ring_size);
        for (uint32_t i = 0; i < n; i++) p[i] = (uint8_t)(i ^ (i >> 8));
        id<MTLBuffer> copy = [dev newBufferWithLength:n options:MTLResourceStorageModeShared]; assert(copy);
        id<MTLCommandBuffer> cb = [r->q commandBuffer];
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromBuffer:r->ring sourceOffset:off toBuffer:copy destinationOffset:0 size:n];
        [bl endEncoding]; [cb commit]; [cb waitUntilCompleted]; assert(!cb.error);
        assert(!memcmp(copy.contents, p, n));
        uint32_t before = r->ring_size;
        ring_room(r, (uint64_t)before + 1); // All allocations for a draw must fit before an encoder is opened.
        assert(r->ring_size >= (uint64_t)before + 1 + 6 * 256 && r->ring_off == 0);
        ring_alloc(r, r->ring_size - 256, &off); assert(off == 0);
        ring_alloc(r, 512, &off); assert(off == 0 && r->ring_off == 512);
        puts("Upload ring: oversized upload, draw reservation, GPU copy and wrap passed");
    }
    return 0;
}
