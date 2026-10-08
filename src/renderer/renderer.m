// DSR native renderer core: Direct3D 7 fixed-function state + DirectDraw surfaces on Metal.
#import "renderer.h"
#import <QuartzCore/QuartzCore.h>
#include "../dsr/dsr_proto.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int dsr_verbose; long dsr_stat[8]; double dsr_gpu_ms[100000]; int dsr_frame;
#define MAXSURF 65536
typedef struct { id<MTLTexture> tex, view2d; uint32_t w, h, kind, fmt, level, root, slice; int used, fxrt; } Surf;   /* fxrt: an effect pass drew into it (DSR_GLOWLOG) */   /* cube faces: tex the cube, view2d the face as 2D */
static inline id<MTLTexture> tex2d(const void *sp) { const Surf *s = sp; return s->view2d ? s->view2d : s->tex; }
typedef struct {   // must match shaders.metal Uniforms
    float wvp[16], wv[16], tex0[16];
    float vp[4], fog[4], fogcolor[4], tfactor[4];
    int32_t c0[4], a0[4], c1[4], a1[4];
    float at[4];
    int32_t misc[4];
    int32_t xrgb[4];     // texture stage 0/1 is XRGB (alpha reads as 1)
    float tex1[16];      // texture-1 transform
    int32_t s1[4];       // stage 1: texcoordindex, texturetransformflags
} Uniforms;

struct DSRRenderer {
    id<MTLDevice> dev; id<MTLCommandQueue> q; id<MTLLibrary> lib;
    Surf s[MAXSURF];
    uint32_t rs[256], tss[8][32]; float xf[32][16]; float vp[6]; uint32_t tex[8];
    uint32_t rt, ds;
    id<MTLCommandBuffer> cb, last_cb; id<MTLRenderCommandEncoder> enc; id<MTLBlitCommandEncoder> blit; uint32_t enc_rt, enc_ds, encoders; uint64_t cb_bytes;
    id<MTLBuffer> ring; uint32_t ring_off, ring_size; id<MTLBuffer> white;
    /* pipeline, depth and sampler states by key: open-addressing tables (keys never 0), no Objective-C boxing per draw */
    uint64_t pso_k[1024]; id<MTLRenderPipelineState> pso_v[1024];
    uint64_t dso_k[64]; id<MTLDepthStencilState> dso_v[64];
    uint32_t smp_k[1024]; id<MTLSamplerState> smp_v[1024]; int pso_n, dso_n, smp_n; uint32_t pso_last;   // filled at most 3/4, then uncached
    /* the current render encoder's state, so unchanged state isn't set again (cleared with each new encoder) */
    struct { __unsafe_unretained id pso, dso, tex[2], smp[2]; int cull, ring_bound; uint32_t sref; MTLViewport vp; } es;
    id<MTLRenderPipelineState> quad_pso, quad_over_pso, zclear_pso, present_pso; id<MTLTexture> scratch, gamma_lut; uint16_t gamma[3][256]; int gamma_dirty; id<MTLDepthStencilState> zclear_ds, nodepth_ds, zsclear_ds[4];
    id<MTLSamplerState> point_clamp, linear_clamp; id<MTLTexture> dummy;
    /* effect programs (DSR_PROG_*): Metal compiled from the game's HLSL effects (runtime/win32/hlsl.c) */
    struct { id<MTLFunction> vs, fs; uint32_t kinds; } prog[1024]; uint32_t cur_prog;
    float pconst[4096 * 4]; uint32_t npconst;
    uint64_t ppso_k[512]; id<MTLRenderPipelineState> ppso_v[512]; int ppso_n;
    id<MTLBuffer> attr_defaults; id<MTLTexture> dummy_cube, dummy_3d;
    uint32_t presented;
    id<MTLBuffer> rb_buf; volatile uint32_t *rb_done;
    double present_ts;
    id<MTLRenderPipelineState> graph_pso; const float *g_a, *g_b; int g_ha, g_hb, g_on;
};

static void mat_ident(float *m) { memset(m, 0, 64); m[0] = m[5] = m[10] = m[15] = 1; }
static void mat_mul(float *o, const float *a, const float *b)   // D3D row-major: o = a * b
{
    float t[16]; for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { float s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = s; }
    memcpy(o, t, 64);
}

static NSString *shader_source(void)
{
    NSString *path = [[[NSProcessInfo processInfo] arguments][0] stringByDeletingLastPathComponent];
    NSString *src = [NSString stringWithContentsOfFile:[path stringByAppendingPathComponent:@"shaders.metal"] encoding:NSUTF8StringEncoding error:nil];
    if (!src) src = [NSString stringWithContentsOfFile:@"shaders.metal" encoding:NSUTF8StringEncoding error:nil];
    return src;
}

DSRRenderer *dsr_renderer_create(id<MTLDevice> dev)
{
    DSRRenderer *r = calloc(1, sizeof *r); NSError *err = nil;
    r->dev = dev; r->q = [dev newCommandQueue];
    r->lib = [dev newLibraryWithSource:shader_source() options:nil error:&err];
    if (!r->lib) { fprintf(stderr, "shader compile failed: %s\n", err.localizedDescription.UTF8String); exit(1); }
    r->ring_size = 256u << 20; r->ring = [dev newBufferWithLength:r->ring_size options:MTLResourceStorageModeShared];
    { uint32_t w = 0xffffffff; r->white = [dev newBufferWithBytes:&w length:16 options:MTLResourceStorageModeShared]; }
    for (int k = 0; k < 32; k++) mat_ident(r->xf[k]);
    for (int k = 0; k < 256; k++) r->rs[k] = 0;
    r->rs[168] = 15; r->rs[56] = 8; r->rs[58] = r->rs[59] = 0xff; r->rs[53] = r->rs[54] = r->rs[55] = 1; r->rs[7] = 1; r->rs[14] = 1; r->rs[23] = 4; r->rs[22] = 3; r->rs[19] = 2; r->rs[20] = 1; r->rs[25] = 8;   // ZENABLE, ZWRITE, ZFUNC LEQUAL, CULL CCW, SRC ONE, DST ZERO, AFUNC ALWAYS
    for (int st = 0; st < 8; st++) { r->tss[st][1] = st ? 1 : 4; r->tss[st][2] = 2; r->tss[st][3] = 1; r->tss[st][4] = st ? 1 : 2; r->tss[st][5] = 2; r->tss[st][6] = 1;
                                     r->tss[st][11] = st; r->tss[st][13] = r->tss[st][14] = 1; r->tss[st][16] = r->tss[st][17] = 2; r->tss[st][18] = 1; }
    {   MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [r->lib newFunctionWithName:@"vs_quad"]; d.fragmentFunction = [r->lib newFunctionWithName:@"fs_quad"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        r->quad_pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        d.colorAttachments[0].blendingEnabled = YES;   // premultiplied "over" for DSR_UPLOAD_OVER
        d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        r->quad_over_pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        d.colorAttachments[0].blendingEnabled = NO;
        d.vertexFunction = [r->lib newFunctionWithName:@"vs_zclear"]; d.fragmentFunction = nil;
        d.colorAttachments[0].writeMask = MTLColorWriteMaskNone; d.depthAttachmentPixelFormat = d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
        r->zclear_pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        if (!r->quad_pso || !r->zclear_pso) { fprintf(stderr, "helper pipelines: %s\n", err.localizedDescription.UTF8String); exit(1); }
        MTLDepthStencilDescriptor *z = [MTLDepthStencilDescriptor new]; z.depthCompareFunction = MTLCompareFunctionAlways; z.depthWriteEnabled = YES;
        r->zclear_ds = [dev newDepthStencilStateWithDescriptor:z];
        z.depthWriteEnabled = NO; r->nodepth_ds = [dev newDepthStencilStateWithDescriptor:z];
        for (int k = 1; k < 4; k++) {                       // clears: [1] depth, [2] stencil, [3] both (stencil = the reference value)
            MTLDepthStencilDescriptor *c = [MTLDepthStencilDescriptor new]; c.depthCompareFunction = MTLCompareFunctionAlways; c.depthWriteEnabled = (k & 1) != 0;
            if (k & 2) { MTLStencilDescriptor *sd = [MTLStencilDescriptor new]; sd.stencilCompareFunction = MTLCompareFunctionAlways;
                         sd.depthStencilPassOperation = MTLStencilOperationReplace; sd.writeMask = 0xff; c.frontFaceStencil = c.backFaceStencil = sd; }
            r->zsclear_ds[k] = [dev newDepthStencilStateWithDescriptor:c];
        } }
    {   MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [r->lib newFunctionWithName:@"vs_present"]; d.fragmentFunction = [r->lib newFunctionWithName:@"fs_present"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        r->present_pso = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        if (!r->present_pso) { fprintf(stderr, "present pipeline: %s\n", err.localizedDescription.UTF8String); exit(1); }
        MTLTextureDescriptor *g = [MTLTextureDescriptor new]; g.textureType = MTLTextureType1D; g.pixelFormat = MTLPixelFormatRGBA16Unorm; g.width = 256;
        g.storageMode = MTLStorageModeShared; r->gamma_lut = [dev newTextureWithDescriptor:g];
        for (int k = 0; k < 256; k++) r->gamma[0][k] = r->gamma[1][k] = r->gamma[2][k] = k * 0x101;
        r->gamma_dirty = 1; }
    {   MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        r->point_clamp = [dev newSamplerStateWithDescriptor:sd]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; r->linear_clamp = [dev newSamplerStateWithDescriptor:sd]; }
    {   MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
        r->dummy = [dev newTextureWithDescriptor:td];
        MTLTextureDescriptor *sd2 = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:256 height:256 mipmapped:NO];
        sd2.storageMode = MTLStorageModePrivate; sd2.usage = MTLTextureUsageShaderRead; r->scratch = [dev newTextureWithDescriptor:sd2]; uint32_t white = 0xffffffff;
        [r->dummy replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&white bytesPerRow:4];
        MTLTextureDescriptor *cd = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:1 mipmapped:NO];
        r->dummy_cube = [dev newTextureWithDescriptor:cd];
        for (NSUInteger f = 0; f < 6; f++) [r->dummy_cube replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:f withBytes:&white bytesPerRow:4 bytesPerImage:4];
        MTLTextureDescriptor *vd = [MTLTextureDescriptor new]; vd.textureType = MTLTextureType3D; vd.pixelFormat = MTLPixelFormatBGRA8Unorm; vd.width = vd.height = vd.depth = 1;
        r->dummy_3d = [dev newTextureWithDescriptor:vd]; [r->dummy_3d replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:0 withBytes:&white bytesPerRow:4 bytesPerImage:4];
        /* vertex attributes a draw doesn't supply: (0, 0, 0, 1), except colour 0 white and colour 1 black (as D3D) */
        float def[16][4]; for (int k = 0; k < 16; k++) { def[k][0] = def[k][1] = def[k][2] = 0; def[k][3] = 1; }
        def[2][0] = def[2][1] = def[2][2] = 1; def[3][3] = 0;
        r->attr_defaults = [dev newBufferWithBytes:def length:sizeof def options:MTLResourceStorageModeShared]; }
    return r;
}

// ---------- command buffer / encoder ----------
static id<MTLCommandBuffer> cmdbuf(DSRRenderer *r) { if (!r->cb) { r->cb = [r->q commandBuffer]; r->encoders = 0; r->cb_bytes = 0; } return r->cb; }
static void end_enc(DSRRenderer *r)
{
    if (r->enc) { [r->enc endEncoding]; r->enc = nil; }
    if (r->blit) { [r->blit endEncoding]; r->blit = nil; }
}
static void commit(DSRRenderer *r)
{
    end_enc(r);
    if (r->cb) {
        if (dsr_verbose) [r->cb addCompletedHandler:^(id<MTLCommandBuffer> cb) { if (cb.error) fprintf(stderr, "command buffer error: %s\n", cb.error.localizedDescription.UTF8String); }];
        int f = dsr_frame; [r->cb addCompletedHandler:^(id<MTLCommandBuffer> cb) { if (f < 100000) dsr_gpu_ms[f] += (cb.GPUEndTime - cb.GPUStartTime) * 1000.0; }];
        [r->cb commit]; r->last_cb = r->cb; r->cb = nil;
    }
}
// Long resource-only stretches (level loading) would otherwise pile thousands of encoders into one command buffer.
static void maybe_commit(DSRRenderer *r) { if (r->cb && (r->encoders > 512 || r->cb_bytes > (64u << 20))) commit(r); }
static void *ring_alloc(DSRRenderer *r, uint32_t n, uint32_t *off)
{
    n = (n + 255) & ~255u;
    if (r->ring_off + n > r->ring_size) {   // wrap: make sure the GPU is done with the old contents
        commit(r); if (r->last_cb) [r->last_cb waitUntilCompleted];
        r->ring_off = 0;
    }
    *off = r->ring_off; r->ring_off += n; r->cb_bytes += n; return (uint8_t *)r->ring.contents + *off;
}
static id<MTLBlitCommandEncoder> blit_enc(DSRRenderer *r)
{
    if (r->enc) { [r->enc endEncoding]; r->enc = nil; }
    if (!r->blit) { r->blit = [cmdbuf(r) blitCommandEncoder]; r->encoders++; }
    return r->blit;
}
static Surf *surf(DSRRenderer *r, uint32_t id) { return (id && id < MAXSURF && r->s[id].used) ? &r->s[id] : NULL; }
static id<MTLRenderCommandEncoder> enc_for(DSRRenderer *r, uint32_t rt, uint32_t ds)
{
    if (r->enc && r->enc_rt == rt && r->enc_ds == ds) return r->enc;
    end_enc(r); maybe_commit(r);
    Surf *c = surf(r, rt), *z = surf(r, ds); if (!c) return nil;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = c->tex; p.colorAttachments[0].loadAction = MTLLoadActionLoad; p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].slice = c->slice; p.colorAttachments[0].level = c->level;
    if (z) { p.depthAttachment.texture = z->tex; p.depthAttachment.loadAction = MTLLoadActionLoad; p.depthAttachment.storeAction = MTLStoreActionStore;
             p.stencilAttachment.texture = z->tex; p.stencilAttachment.loadAction = MTLLoadActionLoad; p.stencilAttachment.storeAction = MTLStoreActionStore; }
    r->enc = [cmdbuf(r) renderCommandEncoderWithDescriptor:p]; r->enc_rt = rt; r->enc_ds = z ? ds : 0; r->encoders++;
    memset(&r->es, 0, sizeof r->es); r->es.cull = -1;
    [r->enc setFrontFacingWinding:MTLWindingClockwise];   // same convention as D3D
    return r->enc;
}

// ---------- surfaces ----------
static void surface_create(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    uint32_t id = p[0], w = p[1], h = p[2], kind = p[3], fmt = p[4], parent = p[5], level = p[6];
    uint32_t face = size >= 40 ? p[8] : 0, cube = size >= 40 && (p[9] & 1);
    if (id >= MAXSURF) return;
    Surf *s = &r->s[id]; s->tex = s->view2d = nil; s->used = 1; s->fxrt = 0; s->w = w; s->h = h; s->kind = kind; s->fmt = fmt; s->level = level; s->root = id; s->slice = face;
    if (parent && surf(r, parent)) {                       // a mip level (or another cube face): shares the root's texture
        Surf *root = surf(r, parent); s->tex = root->tex; s->root = parent;
        if (cube && level == 0)                            // a cube face: its own 2D view
            s->view2d = [s->tex newTextureViewWithPixelFormat:s->tex.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, s->tex.mipmapLevelCount) slices:NSMakeRange(face, 1)];
        else s->view2d = root->view2d;
        return;
    }
    if (kind == 3 && parent == 0 && cube) {
        uint32_t levels = p[7] ? p[7] : 1;
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:w mipmapped:levels > 1];
        d.mipmapLevelCount = levels; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
        s->tex = [r->dev newTextureWithDescriptor:d];
        s->view2d = [s->tex newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0, levels) slices:NSMakeRange(0, 1)];
    } else if (kind == 3 && parent == 0) {
        uint32_t levels = size >= 32 && p[7] ? p[7] : 1;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:levels > 1];
        d.mipmapLevelCount = levels; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;   // XRGB: alpha forced to 1 in the shader (swizzles can't be render targets)
        s->tex = [r->dev newTextureWithDescriptor:d];
    } else {
        MTLPixelFormat pf = fmt == 3 ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatBGRA8Unorm;   // depth buffers carry an 8-bit stencil
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget; d.storageMode = MTLStorageModePrivate;   // XRGB targets: alpha is never read back as destination alpha
        s->tex = [r->dev newTextureWithDescriptor:d];
    }
}
static void surface_upload(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    Surf *s = surf(r, p[0]); if (!s || s->fmt == 3) return;
    uint32_t x = p[1], y = p[2], w = p[3], h = p[4], pitch = p[5], off;
    if (!w || !h || 24 + (uint64_t)pitch * h > size) return;                     /* 64-bit: no wrap */
    if (s->level >= s->tex.mipmapLevelCount || (uint64_t)x + w > (s->tex.width >> s->level ?: 1) || (uint64_t)y + h > (s->tex.height >> s->level ?: 1)) return;
    if (!r->enc) maybe_commit(r);
    void *dst = ring_alloc(r, pitch * h, &off); memcpy(dst, p + 6, pitch * h);
    [blit_enc(r) copyFromBuffer:r->ring sourceOffset:off sourceBytesPerRow:pitch sourceBytesPerImage:pitch * h sourceSize:MTLSizeMake(w, h, 1)
            toTexture:s->tex destinationSlice:s->slice destinationLevel:s->level destinationOrigin:MTLOriginMake(x, y, 0)];
}

// ---------- quads (blits, fills) ----------
static void quad(DSRRenderer *r, uint32_t dst, const int32_t *drect, uint32_t src, const int32_t *srect, int fill, uint32_t color);
// The game composited its cursor over zeros (see surface.c): the pixels are premultiplied, alpha = coverage.
static void upload_over(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    Surf *s = surf(r, p[0]); uint32_t x = p[1], y = p[2], w = p[3], h = p[4], pitch = p[5], off;
    if (!s || !w || !h || 24 + (uint64_t)pitch * h > size) return;
    if (w > 256 || h > 256) { surface_upload(r, p, size); return; }
    if ((uint64_t)x + w > s->w || (uint64_t)y + h > s->h) return;
    if (!r->enc) maybe_commit(r);
    void *dst = ring_alloc(r, pitch * h, &off); memcpy(dst, p + 6, pitch * h);
    [blit_enc(r) copyFromBuffer:r->ring sourceOffset:off sourceBytesPerRow:pitch sourceBytesPerImage:pitch * h sourceSize:MTLSizeMake(w, h, 1)
            toTexture:r->scratch destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    int32_t dr[4] = {(int32_t)x, (int32_t)y, (int32_t)(x + w), (int32_t)(y + h)}, sr[4] = {0, 0, (int32_t)w, (int32_t)h};
    quad(r, p[0], dr, 0, sr, 2, 0);
}
static void quad(DSRRenderer *r, uint32_t dst, const int32_t *drect, uint32_t src, const int32_t *srect, int fill, uint32_t color)
{
    Surf *d = surf(r, dst), *s = surf(r, src); if (!d || d->fmt == 3 || !(d->tex.usage & MTLTextureUsageRenderTarget)) return;
    struct { float dst[4], src[4], color[4]; int32_t mode[4]; } q;
    q.dst[0] = drect[0] / (float)d->w; q.dst[1] = drect[1] / (float)d->h; q.dst[2] = drect[2] / (float)d->w; q.dst[3] = drect[3] / (float)d->h;
    if (fill == 2) { q.src[0] = srect[0] / 256.0f; q.src[1] = srect[1] / 256.0f; q.src[2] = srect[2] / 256.0f; q.src[3] = srect[3] / 256.0f; }
    else if (s) { float sw = s->w, sh = s->h; q.src[0] = srect[0] / sw; q.src[1] = srect[1] / sh; q.src[2] = srect[2] / sw; q.src[3] = srect[3] / sh; }
    q.color[0] = ((color >> 16) & 255) / 255.0f; q.color[1] = ((color >> 8) & 255) / 255.0f; q.color[2] = (color & 255) / 255.0f; q.color[3] = ((color >> 24) & 255) / 255.0f;
    q.mode[0] = fill == 1;
    id<MTLRenderCommandEncoder> e = enc_for(r, dst, 0); if (!e) return;
    [e setRenderPipelineState:fill == 2 ? r->quad_over_pso : r->quad_pso]; [e setDepthStencilState:r->nodepth_ds]; [e setCullMode:MTLCullModeNone];
    [e setViewport:(MTLViewport){0, 0, d->w, d->h, 0, 1}];
    [e setVertexBytes:&q length:sizeof q atIndex:0]; [e setFragmentBytes:&q length:sizeof q atIndex:0];
    [e setFragmentTexture:fill == 2 ? r->scratch : s ? tex2d(s) : r->dummy atIndex:0]; [e setFragmentSamplerState:r->point_clamp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    memset(&r->es, 0, sizeof r->es); r->es.cull = -1;   // the draw-state cache no longer matches the encoder
}

// ---------- fixed-function draws ----------
static MTLBlendFactor bf(uint32_t b)
{
    switch (b) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated; default: return MTLBlendFactorOne; }
}
static MTLCompareFunction cmpf(uint32_t f)
{
    switch (f) { case 1: return MTLCompareFunctionNever; case 2: return MTLCompareFunctionLess; case 3: return MTLCompareFunctionEqual; case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater; case 6: return MTLCompareFunctionNotEqual; case 7: return MTLCompareFunctionGreaterEqual; default: return MTLCompareFunctionAlways; }
}
/* a target without alpha (XRGB, the back buffer too): D3D reads its destination alpha as 1 */
static uint32_t target_bf(DSRRenderer *r, uint32_t b)
{
    Surf *t = surf(r, r->rt); if (!t || t->fmt != 2) return b;
    return b == 7 ? 2 : b == 8 ? 1 : b;                     /* DESTALPHA -> ONE, INVDESTALPHA -> ZERO */
}
static id<MTLRenderPipelineState> pipeline_for(DSRRenderer *r, uint32_t fvf, int has_depth)
{
    uint32_t blend = r->rs[27] ? 1 : 0, sb = blend ? target_bf(r, r->rs[19]) : 2, db = blend ? target_bf(r, r->rs[20]) : 1;
    uint32_t atest = r->rs[15] ? 1 : 0;                                                       // D3DRS_ALPHATESTENABLE
    uint32_t cw = r->rs[168] & 15;                                                            // D3DRS_COLORWRITEENABLE (R1 G2 B4 A8)
    uint64_t key = ((uint64_t)fvf << 32) | (uint64_t)cw << 20 | (blend << 16) | (sb << 8) | (db << 4) | atest << 2 | (uint64_t)has_depth << 1 | 1;   // never 0
    if (key == r->pso_k[r->pso_last]) return r->pso_v[r->pso_last];                                         // same as the last draw
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 54);
    while (r->pso_k[h] && r->pso_k[h] != key) h = (h + 1) & 1023;
    if (r->pso_k[h]) { r->pso_last = h; return r->pso_v[h]; }
    id<MTLRenderPipelineState> p;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; NSError *err = nil;
    d.vertexFunction = [r->lib newFunctionWithName:@"vs_main"];
    {   MTLFunctionConstantValues *fc = [MTLFunctionConstantValues new]; bool at = atest; [fc setConstantValue:&at type:MTLDataTypeBool atIndex:0];
        d.fragmentFunction = [r->lib newFunctionWithName:@"fs_main" constantValues:fc error:&err]; }
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    if (has_depth) d.depthAttachmentPixelFormat = d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    d.colorAttachments[0].writeMask = (cw & 1 ? MTLColorWriteMaskRed : 0) | (cw & 2 ? MTLColorWriteMaskGreen : 0) | (cw & 4 ? MTLColorWriteMaskBlue : 0) | (cw & 8 ? MTLColorWriteMaskAlpha : 0);
    if (blend) { d.colorAttachments[0].blendingEnabled = YES; d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = bf(sb);
                 d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = bf(db); }
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor]; uint32_t off = 0;
    int rhw = (fvf & 0xe) == 4;
    v.attributes[0].format = rhw ? MTLVertexFormatFloat4 : MTLVertexFormatFloat3; v.attributes[0].offset = 0; v.attributes[0].bufferIndex = 0; off = rhw ? 16 : 12;
    if (fvf & 0x10) off += 12;                                                            // normal (unused: lighting off)
    if (fvf & 0x40) { v.attributes[1].format = MTLVertexFormatUChar4Normalized_BGRA; v.attributes[1].offset = off; v.attributes[1].bufferIndex = 0; off += 4; }
    else { v.attributes[1].format = MTLVertexFormatUChar4Normalized_BGRA; v.attributes[1].offset = 0; v.attributes[1].bufferIndex = 2; }
    if (fvf & 0x80) off += 4;                                                             // specular (unused)
    if ((fvf >> 8) & 0xf) { v.attributes[2].format = MTLVertexFormatFloat2; v.attributes[2].offset = off; v.attributes[2].bufferIndex = 0; }
    else { v.attributes[2].format = MTLVertexFormatFloat2; v.attributes[2].offset = 0; v.attributes[2].bufferIndex = 2; }
    uint32_t stride = off; for (uint32_t n = (fvf >> 8) & 0xf, k = 0; k < n; k++) { uint32_t f = (fvf >> (16 + 2 * k)) & 3; stride += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4; }
    v.layouts[0].stride = stride;
    if (!(fvf & 0x40) || !((fvf >> 8) & 0xf)) { v.layouts[2].stride = 16; v.layouts[2].stepFunction = MTLVertexStepFunctionConstant; v.layouts[2].stepRate = 0; }
    d.vertexDescriptor = v;
    double t0 = CACurrentMediaTime();
    p = [r->dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) { fprintf(stderr, "pipeline fvf %x: %s\n", fvf, err.localizedDescription.UTF8String); return nil; }
    static int psolog = -1; if (psolog < 0) psolog = getenv("DSR_PSOLOG") != 0;
    if (psolog) fprintf(stderr, "dsr: new pipeline %#llx in %.1f ms (frame %d)\n", (unsigned long long)key, (CACurrentMediaTime() - t0) * 1000, dsr_frame);
    if (r->pso_n < 768) { r->pso_k[h] = key; r->pso_v[h] = p; r->pso_n++; r->pso_last = h; }   // the game uses a few dozen
    return p;
}
static MTLStencilOperation sop(uint32_t o)
{
    switch (o) { case 2: return MTLStencilOperationZero; case 3: return MTLStencilOperationReplace; case 4: return MTLStencilOperationIncrementClamp;
    case 5: return MTLStencilOperationDecrementClamp; case 6: return MTLStencilOperationInvert; case 7: return MTLStencilOperationIncrementWrap;
    case 8: return MTLStencilOperationDecrementWrap; default: return MTLStencilOperationKeep; }
}
/* depth test/write and the stencil test (D3DRS_STENCIL*: 52 enable, 53 fail, 54 z-fail, 55 pass, 56 function, 58 read
 * mask, 59 write mask; the reference, 57, is set on the encoder) */
static id<MTLDepthStencilState> depth_for(DSRRenderer *r)
{
    uint32_t en = r->rs[7] ? 1 : 0, wr = en && r->rs[14] ? 1 : 0, fn = en ? r->rs[23] : 8, st = r->rs[52] ? 1 : 0;
    uint64_t key = (uint64_t)(en << 16 | wr << 8 | fn << 1 | 1);
    if (st) key |= (uint64_t)1 << 20 | (uint64_t)(r->rs[53] & 15) << 24 | (uint64_t)(r->rs[54] & 15) << 28 | (uint64_t)(r->rs[55] & 15) << 32
                 | (uint64_t)(r->rs[56] & 15) << 36 | (uint64_t)(r->rs[58] & 255) << 40 | (uint64_t)(r->rs[59] & 255) << 48;
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 58);
    while (r->dso_k[h] && r->dso_k[h] != key) h = (h + 1) & 63;
    if (r->dso_k[h]) return r->dso_v[h];
    id<MTLDepthStencilState> s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = en ? cmpf(fn) : MTLCompareFunctionAlways; d.depthWriteEnabled = wr;
    if (st) {
        MTLStencilDescriptor *sd = [MTLStencilDescriptor new];
        sd.stencilCompareFunction = cmpf(r->rs[56]); sd.stencilFailureOperation = sop(r->rs[53]); sd.depthFailureOperation = sop(r->rs[54]);
        sd.depthStencilPassOperation = sop(r->rs[55]); sd.readMask = r->rs[58] & 255; sd.writeMask = r->rs[59] & 255;
        d.frontFaceStencil = d.backFaceStencil = sd;
    }
    s = [r->dev newDepthStencilStateWithDescriptor:d]; if (r->dso_n < 48) { r->dso_k[h] = key; r->dso_v[h] = s; r->dso_n++; } return s;
}
static id<MTLSamplerState> sampler_for(DSRRenderer *r, int st)
{
    uint32_t au = r->tss[st][13], av = r->tss[st][14], mag = r->tss[st][16], min = r->tss[st][17], mip = r->tss[st][18];
    uint32_t key = ((au << 16) | (av << 12) | (mag << 8) | (min << 4) | mip) << 1 | 1, h = (key * 2654435761u) >> 22;
    while (r->smp_k[h] && r->smp_k[h] != key) h = (h + 1) & 1023;
    if (r->smp_k[h]) return r->smp_v[h];
    id<MTLSamplerState> s;
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    MTLSamplerAddressMode (^am)(uint32_t) = ^MTLSamplerAddressMode(uint32_t a) { return a == 2 ? MTLSamplerAddressModeMirrorRepeat : a == 3 ? MTLSamplerAddressModeClampToEdge : a == 4 ? MTLSamplerAddressModeClampToBorderColor : MTLSamplerAddressModeRepeat; };
    d.sAddressMode = am(au); d.tAddressMode = am(av);
    d.magFilter = mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest; d.minFilter = min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = mip == 3 ? MTLSamplerMipFilterLinear : mip == 2 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterNotMipmapped;
    s = [r->dev newSamplerStateWithDescriptor:d]; if (r->smp_n < 768) { r->smp_k[h] = key; r->smp_v[h] = s; r->smp_n++; } return s;
}
static void draw(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    uint32_t prim = p[0], fvf = p[1], nv = p[2], ni = p[3], stride = 0, voff, ioff = 0;
    { int rhw = (fvf & 0xe) == 4; stride = rhw ? 16 : 12; if (fvf & 0x10) stride += 12; if (fvf & 0x40) stride += 4; if (fvf & 0x80) stride += 4;
      for (uint32_t n = (fvf >> 8) & 0xf, k = 0; k < n; k++) { uint32_t f = (fvf >> (16 + 2 * k)) & 3; stride += f == 0 ? 8 : f == 1 ? 12 : f == 2 ? 16 : 4; } }
    dsr_stat[0]++;
    static int drawlog = -1; if (drawlog < 0) drawlog = getenv("DSR_DRAWLOG") != 0;
    if (drawlog) {   /* each distinct kind of draw once (development) */
        static uint64_t seen[512]; static int ns;
        uint64_t key = ((uint64_t)prim << 56) ^ ((uint64_t)fvf << 24) ^ ((uint64_t)r->rs[27] << 20) ^ ((uint64_t)r->rs[19] << 12) ^ ((uint64_t)r->rs[20] << 8)
                     ^ ((uint64_t)r->tss[0][1] << 4) ^ (uint64_t)r->tss[1][1] ^ ((uint64_t)r->rs[15] << 40) ^ ((uint64_t)r->rs[14] << 44);
        int dup = 0; for (int k = 0; k < ns; k++) if (seen[k] == key) dup = 1;
        if (!dup && ns < 512) { seen[ns++] = key;
            fprintf(stderr, "draw: prim %u fvf %#x nv %u ni %u | blend %u src %u dst %u atest %u zwrite %u zfunc %u | st0 op %u/%u tci %#x ttf %#x tex %u | st1 op %u/%u ttf %#x tex %u\n",
                    prim, fvf, nv, ni, r->rs[27], r->rs[19], r->rs[20], r->rs[15], r->rs[14], r->rs[23], r->tss[0][1], r->tss[0][4], r->tss[0][11], r->tss[0][24], r->tex[0],
                    r->tss[1][1], r->tss[1][4], r->tss[1][24], r->tex[1]); }
    }
    if (!nv || 16 + (uint64_t)nv * stride + (uint64_t)ni * 2 > size) { dsr_stat[1]++; return; }   /* 64-bit: no wrap */
    if (prim == 6 && (ni ? ni : nv) < 3) return;                                 /* a fan of fewer than 3: nothing (as D3D) */
    { static int na = -1, nb = -1; if (na < 0) { na = nb = getenv("DSR_GLOWLOG") ? 400 : 0; } Surf *t_ = surf(r, r->rt), *x0_ = surf(r, r->tex[0]);
      int into = t_ && t_->fxrt, add = r->rs[27] && r->rs[19] == 2 && r->rs[20] == 2 && x0_ && x0_->fxrt;
      if ((into && na > 0) || (add && nb > 0)) { if (into) na--; else nb--; float fs, fe; memcpy(&fs, &r->rs[36], 4); memcpy(&fe, &r->rs[37], 4);
        fprintf(stderr, "glow %s rt %u (%ux%u) fvf %x nv %u: fog %u %u/%u | blend %u %u/%u cw %x z %u/%u | tf %08x st0 %u/%u,%u a%u/%u,%u st1 %u/%u | tex0 %u\n",
                into ? "draw into" : "add from", r->rt, t_ ? t_->w : 0, t_ ? t_->h : 0, fvf, nv, r->rs[28], r->rs[35], r->rs[140], r->rs[27], r->rs[19], r->rs[20], r->rs[168],
                r->rs[7], r->rs[14], r->rs[60], r->tss[0][1], r->tss[0][2], r->tss[0][3], r->tss[0][4], r->tss[0][5], r->tss[0][6], r->tss[1][1], r->tss[1][4], r->tex[0]); } }
    Surf *rt = surf(r, r->rt), *ds = surf(r, r->ds); if (!rt) { dsr_stat[2]++; return; }
    id<MTLRenderPipelineState> pso = pipeline_for(r, fvf, ds != NULL); if (!pso) { dsr_stat[3]++; return; }
    id<MTLRenderCommandEncoder> e = enc_for(r, r->rt, r->ds); if (!e) { dsr_stat[4]++; return; }
    void *vdst = ring_alloc(r, nv * stride, &voff); memcpy(vdst, p + 4, nv * stride);
    if (ni) { void *idst = ring_alloc(r, ni * 2, &ioff); memcpy(idst, (const uint8_t *)(p + 4) + nv * stride, ni * 2); }
    e = enc_for(r, r->rt, r->ds);   // ring_alloc may have flushed
    Uniforms u; memset(&u, 0, sizeof u);
    float wv[16]; mat_mul(wv, r->xf[1], r->xf[2]); mat_mul(u.wvp, wv, r->xf[3]); memcpy(u.wv, wv, 64); memcpy(u.tex0, r->xf[16], 64); memcpy(u.tex1, r->xf[17], 64);
    u.s1[0] = r->tss[1][11]; u.s1[1] = r->tss[1][24];
    u.vp[0] = r->vp[0]; u.vp[1] = r->vp[1]; u.vp[2] = r->vp[2] ? r->vp[2] : rt->w; u.vp[3] = r->vp[3] ? r->vp[3] : rt->h;
    { float fs, fe; memcpy(&fs, &r->rs[36], 4); memcpy(&fe, &r->rs[37], 4); u.fog[0] = fs; u.fog[1] = fe; u.fog[2] = r->rs[28] && r->rs[140] == 3 && fe != fs ? 1 : 0; }
    uint32_t fc = r->rs[34]; u.fogcolor[0] = ((fc >> 16) & 255) / 255.0f; u.fogcolor[1] = ((fc >> 8) & 255) / 255.0f; u.fogcolor[2] = (fc & 255) / 255.0f; u.fogcolor[3] = 1;
    uint32_t tf = r->rs[60]; u.tfactor[0] = ((tf >> 16) & 255) / 255.0f; u.tfactor[1] = ((tf >> 8) & 255) / 255.0f; u.tfactor[2] = (tf & 255) / 255.0f; u.tfactor[3] = (tf >> 24) / 255.0f;
    u.c0[0] = r->tss[0][1]; u.c0[1] = r->tss[0][2]; u.c0[2] = r->tss[0][3]; u.c0[3] = r->tss[0][4]; u.a0[0] = r->tss[0][5]; u.a0[1] = r->tss[0][6]; u.a0[2] = r->tss[0][11]; u.a0[3] = r->tss[0][24];
    u.c1[0] = r->tss[1][1]; u.c1[1] = r->tss[1][2]; u.c1[2] = r->tss[1][3]; u.c1[3] = r->tss[1][4]; u.a1[0] = r->tss[1][5]; u.a1[1] = r->tss[1][6];
    u.at[0] = (r->rs[24] & 255) / 255.0f; u.at[1] = r->rs[25]; u.at[2] = r->rs[15] ? 1 : 0;
    Surf *t0 = surf(r, r->tex[0]), *t1 = surf(r, r->tex[1]);
    u.misc[0] = (fvf & 0xe) == 4; u.misc[1] = (fvf & 0x40) != 0; u.misc[2] = t0 != NULL; u.misc[3] = t1 != NULL;
    u.xrgb[0] = t0 && t0->fmt == 2; u.xrgb[1] = t1 && t1->fmt == 2;
    uint32_t uoff; memcpy(ring_alloc(r, sizeof u, &uoff), &u, sizeof u);   // one copy, read by both stages
    e = enc_for(r, r->rt, r->ds);                                         // (ring_alloc may have flushed)
    id<MTLDepthStencilState> dso = depth_for(r);
    if (r->es.pso != pso) { [e setRenderPipelineState:pso]; r->es.pso = pso; }
    if (r->es.dso != dso) { [e setDepthStencilState:dso]; r->es.dso = dso; }
    if (r->rs[52] && r->es.sref != (r->rs[57] & 255) + 1) { [e setStencilReferenceValue:r->rs[57] & 255]; r->es.sref = (r->rs[57] & 255) + 1; }
    uint32_t cull = r->rs[22];
    int cm = cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone;   // D3DCULL_CW culls clockwise (= front here)
    if (r->es.cull != cm) { [e setCullMode:(MTLCullMode)cm]; r->es.cull = cm; }
    MTLViewport vp = {r->vp[0], r->vp[1], u.vp[2], u.vp[3], r->vp[4], r->vp[5] ? r->vp[5] : 1};
    if (memcmp(&vp, &r->es.vp, sizeof vp)) { [e setViewport:vp]; r->es.vp = vp; }
    if (!r->es.ring_bound) {
        [e setVertexBuffer:r->ring offset:voff atIndex:0]; [e setVertexBuffer:r->white offset:0 atIndex:2];
        [e setVertexBuffer:r->ring offset:uoff atIndex:1]; [e setFragmentBuffer:r->ring offset:uoff atIndex:1]; r->es.ring_bound = 1;
    } else { [e setVertexBufferOffset:voff atIndex:0]; [e setVertexBufferOffset:uoff atIndex:1]; [e setFragmentBufferOffset:uoff atIndex:1]; }
    id<MTLTexture> x0 = t0 ? tex2d(t0) : r->dummy, x1 = t1 ? tex2d(t1) : r->dummy; id<MTLSamplerState> s0 = sampler_for(r, 0), s1 = sampler_for(r, 1);
    if (r->es.tex[0] != x0) { [e setFragmentTexture:x0 atIndex:0]; r->es.tex[0] = x0; }
    if (r->es.smp[0] != s0) { [e setFragmentSamplerState:s0 atIndex:0]; r->es.smp[0] = s0; }
    if (r->es.tex[1] != x1) { [e setFragmentTexture:x1 atIndex:1]; r->es.tex[1] = x1; }
    if (r->es.smp[1] != s1) { [e setFragmentSamplerState:s1 atIndex:1]; r->es.smp[1] = s1; }
    MTLPrimitiveType pt; uint32_t count = ni ? ni : nv;
    switch (prim) { case 1: pt = MTLPrimitiveTypePoint; break; case 2: pt = MTLPrimitiveTypeLine; break; case 3: pt = MTLPrimitiveTypeLineStrip; break;
                    case 5: pt = MTLPrimitiveTypeTriangleStrip; break; default: pt = MTLPrimitiveTypeTriangle; }
    if (prim == 6) {   // triangle fan -> list
        uint32_t tris = count - 2, ooff; uint16_t *ix = ring_alloc(r, tris * 6, &ooff);
        const uint16_t *src = ni ? (const uint16_t *)((const uint8_t *)(p + 4) + nv * stride) : NULL;
        for (uint32_t k = 0; k < tris; k++) { ix[k * 3] = src ? src[0] : 0; ix[k * 3 + 1] = src ? src[k + 1] : k + 1; ix[k * 3 + 2] = src ? src[k + 2] : k + 2; }
        e = enc_for(r, r->rt, r->ds);
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:tris * 3 indexType:MTLIndexTypeUInt16 indexBuffer:r->ring indexBufferOffset:ooff];
    } else if (ni) [e drawIndexedPrimitives:pt indexCount:ni indexType:MTLIndexTypeUInt16 indexBuffer:r->ring indexBufferOffset:ioff];
    else [e drawPrimitives:pt vertexStart:0 vertexCount:nv];
}
/* ---- effect programs ---- */
typedef struct FxU {                   /* hlsl.c's FxU: fixed-function state an effect program reads */
    float wvp[16], wv[16]; float vp[4], fog[4], fogcolor[4], tfactor[4], at[4];
    int32_t misc[4], cop[8][4], aop[8][4], tex[8][4];
} FxU;
static void prog_create(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    uint32_t pid = p[0], kinds = p[1], len = p[2];
    if (pid >= 1024 || size < 12 || len > size - 12) return;
    NSString *src = [[NSString alloc] initWithBytes:p + 3 length:len encoding:NSUTF8StringEncoding]; NSError *err = nil;
    const char *patch = getenv("DSR_FXPATCH");                           // development: "old=>new||old=>new" applied to every program's source
    if (patch) for (NSString *one in [@(patch) componentsSeparatedByString:@"||"]) { NSArray *pp = [one componentsSeparatedByString:@"=>"];
        if (pp.count == 2) src = [src stringByReplacingOccurrencesOfString:pp[0] withString:pp[1]]; }
    double t0 = CACurrentMediaTime();
    id<MTLLibrary> lib = [r->dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&err];
    r->prog[pid].vs = [lib newFunctionWithName:@"vs_fx"]; r->prog[pid].fs = [lib newFunctionWithName:@"fs_fx"]; r->prog[pid].kinds = kinds;
    if (!r->prog[pid].vs || !r->prog[pid].fs) fprintf(stderr, "dsr: effect program %u: %s\n", pid, err ? err.localizedDescription.UTF8String : "functions missing");
    else if (getenv("DSR_PSOLOG")) fprintf(stderr, "dsr: effect program %u compiled in %.1f ms\n", pid, (CACurrentMediaTime() - t0) * 1000);
}
static id<MTLRenderPipelineState> prog_pipeline(DSRRenderer *r, uint32_t mask, int has_depth)
{
    uint32_t blend = r->rs[27] ? 1 : 0, sb = blend ? target_bf(r, r->rs[19]) : 2, db = blend ? target_bf(r, r->rs[20]) : 1, cw = r->rs[168] & 15;
    uint64_t key = (uint64_t)r->cur_prog << 48 | (uint64_t)mask << 32 | cw << 20 | blend << 16 | sb << 8 | db << 4 | (uint64_t)has_depth << 1 | 1;
    uint32_t h = (uint32_t)((key * 0x9e3779b97f4a7c15ull) >> 55);
    while (r->ppso_k[h] && r->ppso_k[h] != key) h = (h + 1) & 511;
    if (r->ppso_k[h]) return r->ppso_v[h];
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; NSError *err = nil;
    d.vertexFunction = r->prog[r->cur_prog].vs; d.fragmentFunction = r->prog[r->cur_prog].fs;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.colorAttachments[0].writeMask = (cw & 1 ? MTLColorWriteMaskRed : 0) | (cw & 2 ? MTLColorWriteMaskGreen : 0) | (cw & 4 ? MTLColorWriteMaskBlue : 0) | (cw & 8 ? MTLColorWriteMaskAlpha : 0);
    if (has_depth) d.depthAttachmentPixelFormat = d.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    if (blend) { d.colorAttachments[0].blendingEnabled = YES; d.colorAttachments[0].sourceRGBBlendFactor = d.colorAttachments[0].sourceAlphaBlendFactor = bf(sb);
                 d.colorAttachments[0].destinationRGBBlendFactor = d.colorAttachments[0].destinationAlphaBlendFactor = bf(db); }
    MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor]; int col = 0, ns = __builtin_popcount(mask);
    for (int sl = 0; sl < 16; sl++) {
        v.attributes[sl].format = MTLVertexFormatFloat4;
        if (mask >> sl & 1) { v.attributes[sl].bufferIndex = 0; v.attributes[sl].offset = 16u * (uint32_t)col++; }
        else { v.attributes[sl].bufferIndex = 3; v.attributes[sl].offset = 16u * (uint32_t)sl; }
    }
    v.layouts[0].stride = 16u * (uint32_t)(ns ? ns : 1);
    v.layouts[3].stride = 256; v.layouts[3].stepFunction = MTLVertexStepFunctionConstant; v.layouts[3].stepRate = 0;
    d.vertexDescriptor = v;
    id<MTLRenderPipelineState> pso = [r->dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!pso) { fprintf(stderr, "dsr: effect pipeline (program %u): %s\n", r->cur_prog, err.localizedDescription.UTF8String); return nil; }
    if (r->ppso_n < 384) { r->ppso_k[h] = key; r->ppso_v[h] = pso; r->ppso_n++; }
    return pso;
}
static void draw_prog(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    uint32_t prim = p[0], mask = p[1] & 0xffff, nv = p[2], ni = p[3], pretrans = p[4], ns = (uint32_t)__builtin_popcount(mask), stride = 16 * ns, voff, ioff = 0;
    dsr_stat[0]++;
    if (!nv || !ns || 20 + (uint64_t)nv * stride + (uint64_t)ni * 2 > size) { dsr_stat[1]++; return; }
    if (prim == 6 && (ni ? ni : nv) < 3) return;
    if (r->cur_prog >= 1024 || !r->prog[r->cur_prog].vs) return;
    Surf *rt = surf(r, r->rt), *ds = surf(r, r->ds); if (!rt) { dsr_stat[2]++; return; }
    id<MTLRenderPipelineState> pso = prog_pipeline(r, mask, ds != NULL); if (!pso) { dsr_stat[3]++; return; }
    id<MTLRenderCommandEncoder> e = enc_for(r, r->rt, r->ds); if (!e) { dsr_stat[4]++; return; }
    void *vdst = ring_alloc(r, nv * stride, &voff); memcpy(vdst, p + 5, nv * stride);
    if (ni) { void *idst = ring_alloc(r, ni * 2, &ioff); memcpy(idst, (const uint8_t *)(p + 5) + nv * stride, ni * 2); }
    FxU u; memset(&u, 0, sizeof u);
    float wv[16]; mat_mul(wv, r->xf[1], r->xf[2]); mat_mul(u.wvp, wv, r->xf[3]); memcpy(u.wv, wv, 64);
    u.vp[0] = r->vp[0]; u.vp[1] = r->vp[1]; u.vp[2] = r->vp[2] ? r->vp[2] : rt->w; u.vp[3] = r->vp[3] ? r->vp[3] : rt->h;
    { float fs, fe; memcpy(&fs, &r->rs[36], 4); memcpy(&fe, &r->rs[37], 4); u.fog[0] = fs; u.fog[1] = fe; u.fog[2] = r->rs[28] ? 1 : 0; }
    uint32_t fc = r->rs[34]; u.fogcolor[0] = ((fc >> 16) & 255) / 255.0f; u.fogcolor[1] = ((fc >> 8) & 255) / 255.0f; u.fogcolor[2] = (fc & 255) / 255.0f; u.fogcolor[3] = 1;
    uint32_t tf = r->rs[60]; u.tfactor[0] = ((tf >> 16) & 255) / 255.0f; u.tfactor[1] = ((tf >> 8) & 255) / 255.0f; u.tfactor[2] = (tf & 255) / 255.0f; u.tfactor[3] = (tf >> 24) / 255.0f;
    u.at[0] = (r->rs[24] & 255) / 255.0f; u.at[1] = r->rs[25]; u.at[2] = r->rs[15] ? 1 : 0;
    u.misc[0] = (int32_t)(pretrans & 1);
    for (int st = 0; st < 8; st++) {
        u.cop[st][0] = (int32_t)r->tss[st][1]; u.cop[st][1] = (int32_t)r->tss[st][2]; u.cop[st][2] = (int32_t)r->tss[st][3]; u.cop[st][3] = (int32_t)r->tss[st][4];
        u.aop[st][0] = (int32_t)r->tss[st][5]; u.aop[st][1] = (int32_t)r->tss[st][6]; u.aop[st][3] = (int32_t)r->tss[st][11];
        Surf *t = surf(r, r->tex[st]); u.tex[st][0] = t != NULL; u.tex[st][1] = t && t->fmt == 2;
    }
    e = enc_for(r, r->rt, r->ds);                                         // (ring_alloc may have flushed)
    [e setRenderPipelineState:pso]; [e setDepthStencilState:depth_for(r)];
    if (r->rs[52]) [e setStencilReferenceValue:r->rs[57] & 255];
    uint32_t cull = r->rs[22]; [e setCullMode:cull == 2 ? MTLCullModeFront : cull == 3 ? MTLCullModeBack : MTLCullModeNone];
    [e setViewport:(MTLViewport){r->vp[0], r->vp[1], u.vp[2], u.vp[3], r->vp[4], r->vp[5] ? r->vp[5] : 1}];
    [e setVertexBuffer:r->ring offset:voff atIndex:0]; [e setVertexBuffer:r->attr_defaults offset:0 atIndex:3];
    [e setVertexBytes:&u length:sizeof u atIndex:1]; [e setFragmentBytes:&u length:sizeof u atIndex:1];
    uint32_t nc = r->npconst ? r->npconst : 1, coff;
    void *cdst = ring_alloc(r, 16 * nc, &coff); memcpy(cdst, r->pconst, 16 * nc);
    e = enc_for(r, r->rt, r->ds);
    [e setVertexBuffer:r->ring offset:coff atIndex:2]; [e setFragmentBuffer:r->ring offset:coff atIndex:2];
    [e setVertexBuffer:r->ring offset:voff atIndex:0];
    for (int k = 0; k < 8; k++) {                                         // texture/sampler slots (the device's stages)
        uint32_t kind = (r->prog[r->cur_prog].kinds >> (2 * k)) & 3; Surf *t = surf(r, r->tex[k]);
        id<MTLTexture> x = kind == 1 ? r->dummy_cube : kind == 2 ? r->dummy_3d : (t ? tex2d(t) : r->dummy);
        if (kind == 1 && t && t->tex.textureType == MTLTextureTypeCube) x = t->tex;
        [e setFragmentTexture:x atIndex:(NSUInteger)k]; [e setFragmentSamplerState:sampler_for(r, k) atIndex:(NSUInteger)k];
    }
    if (rt->w == 256 && rt->h == 256) rt->fxrt = 1;
    { static int n = -1; if (n < 0) n = getenv("DSR_GLOWLOG") ? 600 : 0;
      if (n > 0 && rt->w == 256) { n--; Surf *t0 = surf(r, r->tex[0]);
        fprintf(stderr, "glow prog %u rt %u (%ux%u) vp %.0fx%.0f mask %04x nv %u pre %u | blend %u %u/%u | tex0 %u %ux%u%s\n", r->cur_prog, r->rt, rt->w, rt->h, r->vp[2], r->vp[3],
                mask, nv, pretrans, r->rs[27], r->rs[19], r->rs[20], r->tex[0], t0 ? t0->w : 0, t0 ? t0->h : 0, t0 ? "" : " (none: white)"); } }
    static int tlog = -1; if (tlog < 0) tlog = getenv("DSR_FXTEXLOG") ? atoi(getenv("DSR_FXTEXLOG")) : 0;
    { static int n = -1; if (n < 0) n = getenv("DSR_RTLOG") ? atoi(getenv("DSR_RTLOG")) : 0; static uint32_t lastp; static int lastf;
      if (n > 0 && r->rs[28] && (r->cur_prog != lastp || dsr_frame != lastf)) { n--; lastp = r->cur_prog; lastf = dsr_frame; fprintf(stderr, "dsr: frame %d: program %u draws into %u after %ld draws\n", dsr_frame, r->cur_prog, r->rt, dsr_stat[0]); } }
    if (tlog > 0) { tlog--; { float fs, fe, fd; memcpy(&fs, &r->rs[36], 4); memcpy(&fe, &r->rs[37], 4); memcpy(&fd, &r->rs[38], 4);
                                fprintf(stderr, "dsr: fog enable %u table %u vertex %u range %u start %g end %g density %g colour %08x; alpha test %u ref %u func %u; blend %u %u/%u\n", r->rs[28], r->rs[35], r->rs[140], r->rs[48], fs, fe, fd, r->rs[34], r->rs[15], r->rs[24], r->rs[25], r->rs[27], r->rs[19], r->rs[20]); }
                    fprintf(stderr, "dsr: program %u draws with", r->cur_prog);
        for (int k = 0; k < 8; k++) { Surf *t = surf(r, r->tex[k]); if (t) fprintf(stderr, " [%d] %u %ux%u%s%s", k, r->tex[k], t->w, t->h, t->tex.textureType == MTLTextureTypeCube ? " cube" : "", t->tex ? "" : " (no texture)"); else if (r->tex[k]) fprintf(stderr, " [%d] %u (unknown)", k, r->tex[k]); }
        fprintf(stderr, "\n"); }
    memset(&r->es, 0, sizeof r->es); r->es.cull = -1;                     // the fixed-function path sets its state again
    MTLPrimitiveType pt; uint32_t count = ni ? ni : nv;
    switch (prim) { case 1: pt = MTLPrimitiveTypePoint; break; case 2: pt = MTLPrimitiveTypeLine; break; case 3: pt = MTLPrimitiveTypeLineStrip; break;
                    case 5: pt = MTLPrimitiveTypeTriangleStrip; break; default: pt = MTLPrimitiveTypeTriangle; }
    if (prim == 6) {
        uint32_t tris = count - 2, ooff; uint16_t *ix = ring_alloc(r, tris * 6, &ooff);
        const uint16_t *src = ni ? (const uint16_t *)((const uint8_t *)(p + 5) + nv * stride) : NULL;
        for (uint32_t k = 0; k < tris; k++) { ix[k * 3] = src ? src[0] : 0; ix[k * 3 + 1] = src ? src[k + 1] : (uint16_t)(k + 1); ix[k * 3 + 2] = src ? src[k + 2] : (uint16_t)(k + 2); }
        e = enc_for(r, r->rt, r->ds);
        [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:tris * 3 indexType:MTLIndexTypeUInt16 indexBuffer:r->ring indexBufferOffset:ooff];
    } else if (ni) [e drawIndexedPrimitives:pt indexCount:ni indexType:MTLIndexTypeUInt16 indexBuffer:r->ring indexBufferOffset:ioff];
    else [e drawPrimitives:pt vertexStart:0 vertexCount:nv];
}
static void clear_rect(DSRRenderer *r, uint32_t flags, uint32_t color, float z, uint32_t stencil, const int32_t *rc, int has_ds)
{
    if (flags & 1) quad(r, r->rt, rc, 0, rc, 1, color);                          // D3DCLEAR_TARGET
    if ((flags & 6) && has_ds) {                                                // D3DCLEAR_ZBUFFER, D3DCLEAR_STENCIL
        id<MTLRenderCommandEncoder> e = enc_for(r, r->rt, r->ds); float zz[4] = {z, 0, 0, 0};
        [e setRenderPipelineState:r->zclear_pso]; [e setDepthStencilState:r->zsclear_ds[(flags >> 1) & 3]]; [e setCullMode:MTLCullModeNone];
        if (flags & 4) [e setStencilReferenceValue:stencil & 255];
        [e setViewport:(MTLViewport){rc[0], rc[1], rc[2] - rc[0], rc[3] - rc[1], 0, 1}];
        [e setVertexBytes:zz length:16 atIndex:0]; [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        memset(&r->es, 0, sizeof r->es); r->es.cull = -1;
    }
}

static void clear(DSRRenderer *r, const uint32_t *p, uint32_t size)
{
    uint32_t flags = p[0], color = p[1]; float z; memcpy(&z, &p[2], 4);
    Surf *rt = surf(r, r->rt), *ds = surf(r, r->ds); if (!rt) return;
    if (dsr_verbose) fprintf(stderr, "clear flags %x color %08x rt %u (%ux%u fmt %u usage %lx) ds %u vp %.0f %.0f %.0f %.0f\n", flags, color, r->rt, rt->w, rt->h, rt->fmt, (unsigned long)rt->tex.usage, r->ds, r->vp[0], r->vp[1], r->vp[2], r->vp[3]);
    int32_t full[4] = {(int32_t)r->vp[0], (int32_t)r->vp[1], (int32_t)(r->vp[0] + (r->vp[2] ? r->vp[2] : rt->w)), (int32_t)(r->vp[1] + (r->vp[3] ? r->vp[3] : rt->h))};
    full[0] = full[0] > 0 ? full[0] : 0; full[1] = full[1] > 0 ? full[1] : 0;
    full[2] = full[2] < (int32_t)rt->w ? full[2] : (int32_t)rt->w; full[3] = full[3] < (int32_t)rt->h ? full[3] : (int32_t)rt->h;
    // D3D clears only the given rects (each clipped to the viewport), or the whole viewport when there are none. The game
    // relies on it: the inventory's paper doll is kept inside its box by clearing depth to 0 everywhere, then to 1 in the
    // box, before drawing the doll.
    uint32_t nr = size >= 20 ? p[4] : 0; if (nr > (size - 20) / 16) nr = 0;
    for (uint32_t k = 0; k < (nr ? nr : 1); k++) {
        int32_t rc[4]; memcpy(rc, nr ? (const int32_t *)(p + 5 + 4 * k) : full, 16);
        if (rc[0] < full[0]) rc[0] = full[0];
        if (rc[1] < full[1]) rc[1] = full[1];
        if (rc[2] > full[2]) rc[2] = full[2];
        if (rc[3] > full[3]) rc[3] = full[3];
        if (rc[2] > rc[0] && rc[3] > rc[1]) clear_rect(r, flags, color, z, p[3], rc, ds != 0);
    }
    if (getenv("DSR_CHECKCLEAR")) {
        static int n; if (n++ < 4) {
            id<MTLBuffer> b = [r->dev newBufferWithLength:4 options:MTLResourceStorageModeShared]; end_enc(r);
            id<MTLBlitCommandEncoder> be = [cmdbuf(r) blitCommandEncoder];
            [be copyFromTexture:rt->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(100, 100, 0) sourceSize:MTLSizeMake(1, 1, 1) toBuffer:b destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
            [be endEncoding]; dsr_renderer_flush(r, 1);
            fprintf(stderr, "after clear %d: px %08x, cb status %ld err %s\n", n, *(uint32_t *)b.contents, (long)r->last_cb.status, r->last_cb.error.localizedDescription.UTF8String ?: "-"); } }
}

void dsr_renderer_flush(DSRRenderer *r, int wait)
{
    commit(r);
    if (wait && r->last_cb) [r->last_cb waitUntilCompleted];
}
// Encode "presented surface -> dst" (gamma applied) into the current command buffer; returns that command buffer,
// uncommitted, so the caller can attach presentDrawable / completion handlers and then call dsr_renderer_flush.
id<MTLCommandBuffer> dsr_renderer_present_to(DSRRenderer *r, id<MTLTexture> dst)
{
    Surf *s = surf(r, r->presented); if (!s) return nil;
    if (r->gamma_dirty) {
        uint16_t px[256][4];
        for (int k = 0; k < 256; k++) { px[k][0] = r->gamma[0][k]; px[k][1] = r->gamma[1][k]; px[k][2] = r->gamma[2][k]; px[k][3] = 0xffff; }
        [r->gamma_lut replaceRegion:MTLRegionMake1D(0, 256) mipmapLevel:0 withBytes:px bytesPerRow:sizeof px];
        r->gamma_dirty = 0;
    }
    end_enc(r);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = dst; p.colorAttachments[0].loadAction = MTLLoadActionDontCare; p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmdbuf(r) renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:r->present_pso];
    [e setFragmentTexture:s->tex atIndex:0]; [e setFragmentTexture:r->gamma_lut atIndex:1];
    [e setFragmentSamplerState:(dst.width == s->w && dst.height == s->h) ? r->point_clamp : r->linear_clamp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    if (r->g_on) {   // frame-time graph, bottom-left: 240 columns, 0..40 ms
        if (!r->graph_pso) {
            MTLRenderPipelineDescriptor *gd = [MTLRenderPipelineDescriptor new]; NSError *err = nil;
            gd.vertexFunction = [r->lib newFunctionWithName:@"vs_present"]; gd.fragmentFunction = [r->lib newFunctionWithName:@"fs_graph"];
            gd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm; gd.colorAttachments[0].blendingEnabled = YES;
            gd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha; gd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
            r->graph_pso = [r->dev newRenderPipelineStateWithDescriptor:gd error:&err];
            if (!r->graph_pso) fprintf(stderr, "graph pipeline: %s\n", err.localizedDescription.UTF8String);
        }
        if (r->graph_pso) {
            float g[2 * 240 + 4]; int k;
            for (k = 0; k < 240; k++) { g[k] = r->g_a[(r->g_ha + k) % 240]; g[240 + k] = r->g_b[(r->g_hb + k) % 240]; }
            double w = dst.width * 0.36, h = dst.height * 0.22;
            [e setViewport:(MTLViewport){8, dst.height - h - 8, w, h, 0, 1}];
            [e setRenderPipelineState:r->graph_pso];
            [e setFragmentBytes:g length:sizeof g atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    }
    [e endEncoding];
    return r->cb;
}
void dsr_renderer_set_readback(DSRRenderer *r, void *area, size_t size, volatile uint32_t *done)
{
    r->rb_buf = [r->dev newBufferWithBytesNoCopy:area length:size options:MTLResourceStorageModeShared deallocator:nil]; r->rb_done = done;
}
static void readback(DSRRenderer *r, const uint32_t *p)
{
    Surf *s = surf(r, p[0]); uint32_t x = p[1], y = p[2], w = p[3], h = p[4]; int ok = 0;
    if (s && r->rb_buf && s->fmt != 3 && w && h && (uint64_t)x + w <= s->w && (uint64_t)y + h <= s->h && (uint64_t)w * h * 4 <= r->rb_buf.length) {
        if (r->enc) { [r->enc endEncoding]; r->enc = nil; }
        [blit_enc(r) copyFromTexture:s->tex sourceSlice:s->slice sourceLevel:s->level sourceOrigin:MTLOriginMake(x, y, 0) sourceSize:MTLSizeMake(w, h, 1)
                             toBuffer:r->rb_buf destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        dsr_renderer_flush(r, 1); ok = 1;
    }
    if (r->rb_done) __atomic_store_n(r->rb_done, ok ? p[5] : p[5] | 0x80000000u, __ATOMIC_RELEASE);   /* high bit: not done */
}
double dsr_renderer_present_time(DSRRenderer *r) { return r->present_ts; }
void dsr_renderer_set_graph(DSRRenderer *r, const float *a, int ha, const float *b, int hb) { r->g_a = a; r->g_ha = ha; r->g_b = b; r->g_hb = hb; r->g_on = 1; }
void dsr_renderer_presented_size(DSRRenderer *r, uint32_t *w, uint32_t *h) { Surf *s = surf(r, r->presented); *w = s ? s->w : 0; *h = s ? s->h : 0; }
id<MTLTexture> dsr_renderer_rt(DSRRenderer *r) { Surf *s = surf(r, r->rt); return s ? s->tex : nil; }
id<MTLTexture> dsr_renderer_presented(DSRRenderer *r) { Surf *s = surf(r, r->presented); return s ? s->tex : nil; }

int dsr_renderer_exec(DSRRenderer *r, uint32_t op, const uint8_t *pl, uint32_t size)
{
    const uint32_t *p = (const uint32_t *)pl;
    switch (op) {
    case DSR_RENDER_STATE: if (p[0] < 256) r->rs[p[0]] = p[1]; break;
    case DSR_TSS: if (p[0] < 8 && p[1] < 32) { r->tss[p[0]][p[1]] = p[2]; if (p[1] == 12) r->tss[p[0]][13] = r->tss[p[0]][14] = p[2]; } break;
    case DSR_TEXTURE: if (p[0] < 8) r->tex[p[0]] = p[1]; break;
    case DSR_TRANSFORM: if (p[0] < 32) memcpy(r->xf[p[0]], p + 1, 64); break;
    case DSR_VIEWPORT: r->vp[0] = p[0]; r->vp[1] = p[1]; r->vp[2] = p[2]; r->vp[3] = p[3]; memcpy(&r->vp[4], p + 4, 8); break;
    case DSR_CLEAR: { static int n = -1; if (n < 0) n = getenv("DSR_RTLOG") ? atoi(getenv("DSR_RTLOG")) : 0;
        Surf *t = surf(r, r->rt); if (n > 0 && t && t->kind == 3) { n--; fprintf(stderr, "dsr: clear of %u: flags %x colour %08x\n", r->rt, p[0], p[1]); } }
        clear(r, p, size); break;
    case DSR_DRAW: draw(r, p, size); break;
    case DSR_SET_RT: r->rt = p[0]; r->ds = p[1];
        { static int n = -1; if (n < 0) n = getenv("DSR_RTLOG") ? atoi(getenv("DSR_RTLOG")) : 0;          // development: off-screen targets
          Surf *t = surf(r, p[0]); if (n > 0 && r->rs[28]) { n--; fprintf(stderr, "dsr: frame %d: render target %u (%ux%u) depth %u, after %ld draws\n", dsr_frame, p[0], t ? t->w : 0, t ? t->h : 0, p[1], dsr_stat[0]); } }
        break;
    case DSR_SURFACE_CREATE: surface_create(r, p, size); break;
    case DSR_SURFACE_DESTROY: if (p[0] < MAXSURF) { r->s[p[0]].used = 0; r->s[p[0]].tex = nil; } break;
    case DSR_SURFACE_UPLOAD: surface_upload(r, p, size); break;
    case DSR_UPLOAD_OVER: upload_over(r, p, size); break;
    case DSR_READBACK: readback(r, p); break;
    case DSR_BLT: { int32_t dr[4], sr[4]; memcpy(dr, p + 1, 16); memcpy(sr, p + 6, 16);
                    if (p[5]) quad(r, p[0], dr, p[5], sr, 0, 0); else if (!(p[10] & 0x20000)) quad(r, p[0], dr, 0, dr, 1, p[11]); } break;
    case DSR_PRESENT: r->presented = p[0]; dsr_frame++;   // caller presents/commits
        { static int glow = -1; if (glow < 0) glow = getenv("DSR_GLOWLOG") != 0;   // development: effect targets in the corner
          if (glow) { int x = 0; for (uint32_t id = 1; id < MAXSURF && x < 5 * 260; id++) if (r->s[id].used && r->s[id].fxrt) {
              int32_t d[4] = {x, 0, x + 256, 256}, sr[4] = {0, 0, 256, 256}; uint32_t ort = r->rt; quad(r, p[0], d, id, sr, 0, 0); r->rt = ort; x += 260; } } }
        { static int show = -1; if (show < 0) show = getenv("DSR_SHOWSURF") ? atoi(getenv("DSR_SHOWSURF")) : 0;   // development: a surface in the corner
          Surf *t = surf(r, (uint32_t)show); if (show > 0 && t) { int32_t d[4] = {0, 0, (int32_t)t->w, (int32_t)t->h}, sr[4] = {0, 0, (int32_t)t->w, (int32_t)t->h};
            uint32_t ort = r->rt; quad(r, p[0], d, (uint32_t)show, sr, 0, 0); r->rt = ort; } }
        r->present_ts = (size >= 16 && p[3]) ? (((uint64_t)p[2] << 32) | p[1]) / (double)p[3] : -1; return 1;
    case DSR_PROG_CREATE: prog_create(r, p, size); break;
    case DSR_PROG_USE: r->cur_prog = p[0] < 1024 ? p[0] : 0; break;
    case DSR_PROG_CONST: if (size >= 4 && p[0] <= 4096 && 4 + 16 * (uint64_t)p[0] <= size) { memcpy(r->pconst, p + 1, 16 * (size_t)p[0]); r->npconst = p[0]; } break;
    case DSR_DRAW_PROG: draw_prog(r, p, size); break;
    case DSR_GAMMA: if (size >= 1536) { memcpy(r->gamma, p, 1536); r->gamma_dirty = 1; } break;
    default: break;
    }
    return 0;
}
