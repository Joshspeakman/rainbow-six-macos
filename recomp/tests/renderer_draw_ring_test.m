// Real Metal draw checks at upload-ring boundaries. The forwarding encoder rejects a missing pipeline
// before sending an invalid draw to the driver, making the old implementation fail safely.
#import "../../src/renderer/renderer.m"
#include <assert.h>

@interface CheckedEncoder : NSProxy
@property(nonatomic, strong) id<MTLRenderCommandEncoder> real;
@property(nonatomic) BOOL hasPipeline;
@end
@implementation CheckedEncoder
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s { return [(id)self.real methodSignatureForSelector:s]; }
- (void)forwardInvocation:(NSInvocation *)i { [i invokeWithTarget:self.real]; }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p { self.hasPipeline = p != nil; [self.real setRenderPipelineState:p]; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t
                 indexBuffer:(id<MTLBuffer>)b indexBufferOffset:(NSUInteger)o
{
    assert(self.hasPipeline && "ring wrap discarded the draw's pipeline");
    [self.real drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:b indexBufferOffset:o];
}
@end
@interface CheckedCommand : NSProxy
@property(nonatomic, strong) id<MTLCommandBuffer> real;
@end
@implementation CheckedCommand
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s { return [(id)self.real methodSignatureForSelector:s]; }
- (void)forwardInvocation:(NSInvocation *)i { [i invokeWithTarget:self.real]; }
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d
{
    CheckedEncoder *e = [CheckedEncoder alloc]; e.real = [self.real renderCommandEncoderWithDescriptor:d];
    return (id<MTLRenderCommandEncoder>)e;
}
@end
@interface CheckedQueue : NSProxy
@property(nonatomic, strong) id<MTLCommandQueue> real;
@end
@implementation CheckedQueue
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s { return [(id)self.real methodSignatureForSelector:s]; }
- (void)forwardInvocation:(NSInvocation *)i { [i invokeWithTarget:self.real]; }
- (id<MTLCommandBuffer>)commandBuffer
{
    CheckedCommand *c = [CheckedCommand alloc]; c.real = [self.real commandBuffer];
    return (id<MTLCommandBuffer>)c;
}
@end
static void cmd(DSRRenderer *r, uint32_t op, const void *p, uint32_t n) { dsr_renderer_exec(r, op, p, n); }
static void red_pixel(DSRRenderer *r)
{
    dsr_renderer_flush(r, 1); assert(!r->last_cb.error);
    id<MTLBuffer> out = [r->dev newBufferWithLength:256 * 32 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [r->q commandBuffer]; id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:r->s[1].tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
           sourceSize:MTLSizeMake(32, 32, 1) toBuffer:out destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256 * 32];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted]; assert(!cb.error);
    uint32_t pixel = ((uint32_t *)out.contents)[16 * 64 + 16];
    assert(pixel == 0xffff0000u);
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        setenv("DSR_RINGMB", "4", 1);
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); assert(dev);
        DSRRenderer *r = dsr_renderer_create(dev); assert(r && r->ring_size == (4u << 20));
        CheckedQueue *q = [CheckedQueue alloc]; q.real = r->q; r->q = (id<MTLCommandQueue>)q;
        uint32_t s[] = {1, 32, 32, 1, 0, 0, 0, 1}; cmd(r, DSR_SURFACE_CREATE, s, sizeof s);
        uint32_t rt[] = {1, 0}; cmd(r, DSR_SET_RT, rt, sizeof rt);
        uint32_t vp[] = {0, 0, 32, 32, 0, 0x3f800000}; cmd(r, DSR_VIEWPORT, vp, sizeof vp);
        uint32_t z[] = {7, 0}, cull[] = {22, 1}; cmd(r, DSR_RENDER_STATE, z, sizeof z); cmd(r, DSR_RENDER_STATE, cull, sizeof cull);
        if (argc < 2 || strcmp(argv[1], "program")) {
            // Force the complete fixed-function fan upload across the remaining 768-byte boundary.
            r->ring_off = r->ring_size - 768;
            struct { uint32_t h[4]; struct { float x, y, z, w; uint32_t color; } v[4]; } d = {
                {6, 0x44, 4, 0}, {{0,0,.5,1,0xffff0000}, {32,0,.5,1,0xffff0000}, {32,32,.5,1,0xffff0000}, {0,32,.5,1,0xffff0000}}};
            cmd(r, DSR_DRAW, &d, sizeof d); red_pixel(r);
        } else {
            const char *source = "#include <metal_stdlib>\nusing namespace metal;\nstruct V{float4 p [[attribute(0)]];};\n"
                "struct O{float4 p [[position]];}; vertex O vs_fx(V v [[stage_in]]){return O{v.p};}\nfragment float4 fs_fx(){return float4(1,0,0,1);}";
            size_t n = strlen(source); uint32_t *p = calloc(1, n + 12); p[0] = 1; p[2] = (uint32_t)n; memcpy(p + 3, source, n);
            prog_create(r, p, (uint32_t)n + 12); free(p); assert(r->prog[1].vs && r->prog[1].fs); r->cur_prog = 1;
            r->ring_off = r->ring_size - 256; // Constants used to wrap after the program and vertex state were bound.
            struct { uint32_t h[5]; float p[4][4]; } d = {{6, 1, 4, 0, 0}, {{-1,-1,.5,1},{1,-1,.5,1},{1,1,.5,1},{-1,1,.5,1}}};
            draw_prog(r, (uint32_t *)&d, sizeof d); red_pixel(r);
        }
        puts("Metal draw ring boundary: pipeline retained and rendered pixel correct");
    }
    return 0;
}
