// fx_render_test <fx folder>: renders the game's pixel shaders, as runtime/win32/hlsl.c compiles them, with known
// inputs (textures per sampler slot, parameter registers from the effects' own defaults, fixed interpolants) and checks
// what each shader computes: the glow's bloom taps and weights, the shadow blur's average, the heat shimmer's lookup,
// Dot3 lighting, the ice and water blends, Specular's diffuse term, XRGB textures reading alpha 1, and every pass's
// sampler slots. Build (from recomp/):
//   clang -fobjc-arc -framework Metal -framework Foundation -I runtime -I runtime/win32 tests/fx_render_test.m runtime/win32/hlsl.c -o /tmp/fx_render_test
#import <Metal/Metal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hlsl.h"

typedef struct FxU {                   /* renderer.m's FxU */
    float wvp[16], wv[16]; float vp[4], fog[4], fogcolor[4], tfactor[4], at[4];
    int32_t misc[4], cop[8][4], aop[8][4], tex[8][4];
} FxU;

static id<MTLDevice> dev; static id<MTLCommandQueue> q;
static int fails, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { float t[8][4]; float c0[4], c1[4]; float fog; } Interp;   /* interpolants, the same at every pixel unless uvramp */
typedef struct { id<MTLTexture> tex[8]; int xrgb[8]; } Slots;

static HFx *load(const char *dir, const char *name)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, name);
    NSData *d = [NSData dataWithContentsOfFile:@(p)]; if (!d) { printf("%s: unreadable\n", p); exit(2); }
    HFx *fx = hlsl_parse((const char *)d.bytes, d.length);
    if (!fx || fx->err[0]) { printf("%s: %s\n", name, fx ? fx->err : "?"); exit(2); }
    return fx;
}
static int tech(HFx *fx, const char *name) { for (int t = 0; t < fx->nt; t++) if (!strcmp(fx->t[t].name, name)) return t; printf("no technique %s\n", name); exit(2); }
static int var(HFx *fx, const char *name) { for (int v = 0; v < fx->nv; v++) if (!strcmp(fx->v[v].name, name)) return v; return -1; }

/* parameter registers as d3dx9_fx.c lays them out, with the effect's defaults */
static float *regs_for(HFx *fx, HProgram *pr, int *n)
{
    int nregs = 0; for (int i = 0; i < fx->nv; i++) if (pr->preg[i] >= 0) { int e = pr->preg[i] + hlsl_type_regs(fx, fx->v[i].t); if (e > nregs) nregs = e; }
    float *R = calloc((size_t)nregs * 4 + 4, sizeof(float));
    for (int i = 0; i < fx->nv; i++) {
        HVar *v = &fx->v[i]; if (pr->preg[i] < 0 || !v->defval) continue;
        HType t = v->t; t.arr = 0; int rows = t.mat ? t.rows : 1, cols = t.cols, per = rows * cols, rpe = hlsl_type_regs(fx, t);
        for (int k = 0; k < v->ndef; k++) { int e = k / per, w = k % per; R[(pr->preg[i] + e * rpe + w / cols) * 4 + w % cols] = v->defval[k]; }
    }
    *n = nregs; return R;
}
static void set_param(HFx *fx, HProgram *pr, float *R, const char *name, const float *val, int nfloats)
{
    int i = var(fx, name); if (i < 0 || pr->preg[i] < 0) { printf("  (parameter %s not in this pass)\n", name); return; }
    HType t = fx->v[i].t; t.arr = 0; int rows = t.mat ? t.rows : 1, cols = t.cols, per = rows * cols, rpe = hlsl_type_regs(fx, t);
    for (int k = 0; k < nfloats; k++) { int e = k / per, w = k % per; R[(pr->preg[i] + e * rpe + w / cols) * 4 + w % cols] = val[k]; }
}
static int slot_of(HFx *fx, HProgram *pr, const char *sampler)
{
    int v = var(fx, sampler); for (int k = 0; k < pr->nsamp; k++) if (pr->sslot[k] == v) return k;
    return -1;
}

/* textures: BGRA8, filled by a function of (x, y) -> 0xAARRGGBB */
typedef uint32_t (*Gen)(int x, int y, int w, int h);
static id<MTLTexture> tex2d(int w, int h, Gen g)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:d]; uint32_t *px = malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) px[y * w + x] = g(x, y, w, h);
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4]; free(px); return t;
}
static id<MTLTexture> cube(uint32_t c)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm size:4 mipmapped:NO];
    id<MTLTexture> t = [dev newTextureWithDescriptor:d]; uint32_t px[16]; for (int k = 0; k < 16; k++) px[k] = c;
    for (int f = 0; f < 6; f++) [t replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 slice:f withBytes:px bytesPerRow:16 bytesPerImage:64];
    return t;
}
static uint32_t solid_c; static uint32_t g_solid(int x, int y, int w, int h) { (void)x; (void)y; (void)w; (void)h; return solid_c; }
static id<MTLTexture> solid(uint32_t c) { solid_c = c; return tex2d(4, 4, g_solid); }

/* renders pass p of technique t into a w x h target; uvramp: t0.xy runs 0..1 across the target (texel centres) */
static uint32_t *render(HFx *fx, int t, int p, Slots *sl, float *R, int nregs, const Interp *in, int w, int h, int uvramp, HProgram **out_pr)
{
    HProgram *pr = hlsl_compile_pass(fx, t, p);
    if (!pr || pr->err[0]) { printf("  compile: %s\n", pr ? pr->err : "?"); fails++; return NULL; }
    if (out_pr) *out_pr = pr;
    NSString *vs = @"\nstruct TI { float4 t[8]; float4 c0, c1; float4 fog; int4 ramp; };\n"
        "vertex V2F test_vs(uint vid [[vertex_id]], constant TI &ti [[buffer(0)]]) {\n"
        "  float2 p = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0); V2F o = v2f_default(); o.pos = float4(p, 0, 1);\n"
        "  o.t0 = ti.t[0]; o.t1 = ti.t[1]; o.t2 = ti.t[2]; o.t3 = ti.t[3]; o.t4 = ti.t[4]; o.t5 = ti.t[5]; o.t6 = ti.t[6]; o.t7 = ti.t[7];\n"
        "  if (ti.ramp.x) o.t0.xy = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);\n"
        "  o.c0 = ti.c0; o.c1 = ti.c1; o.fog = ti.fog.x; return o; }\n";
    NSString *src = [[[NSString alloc] initWithBytes:pr->msl length:pr->msl_len encoding:NSUTF8StringEncoding] stringByAppendingString:vs];
    NSError *err = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&err];
    if (!lib) { printf("  metal: %s\n", err.localizedDescription.UTF8String); fails++; return NULL; }
    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = [lib newFunctionWithName:@"test_vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs_fx"];
    pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
    id<MTLRenderPipelineState> pso = [dev newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pso) { printf("  pipeline: %s\n", err.localizedDescription.UTF8String); fails++; return NULL; }
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:w height:h mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:pso];
    struct { float t[8][4]; float c0[4], c1[4], fog[4]; int32_t ramp[4]; } ti; memset(&ti, 0, sizeof ti);
    memcpy(ti.t, in->t, sizeof ti.t); memcpy(ti.c0, in->c0, 16); memcpy(ti.c1, in->c1, 16); ti.fog[0] = in->fog; ti.ramp[0] = uvramp;
    [e setVertexBytes:&ti length:sizeof ti atIndex:0];
    FxU u; memset(&u, 0, sizeof u); u.vp[2] = (float)w; u.vp[3] = (float)h;          /* no fog, no alpha test */
    for (int k = 0; k < 8; k++) { u.tex[k][0] = sl->tex[k] != nil; u.tex[k][1] = sl->xrgb[k]; }
    [e setFragmentBytes:&u length:sizeof u atIndex:1];
    if (!R) { static float z[16]; R = z; nregs = 1; }
    [e setFragmentBytes:R length:(NSUInteger)(nregs ? nregs : 1) * 16 atIndex:2];
    MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
    for (int k = 0; k < 8; k++) {
        id<MTLTexture> x = sl->tex[k]; int kind = pr->skind[k];
        if (!x) x = kind == HS_CUBE ? cube(0xffffffff) : solid(0xffffffff);
        [e setFragmentTexture:x atIndex:(NSUInteger)k]; [e setFragmentSamplerState:smp atIndex:(NSUInteger)k];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4]; [e endEncoding]; [cb commit]; [cb waitUntilCompleted];
    float *px = malloc((size_t)w * h * 16); [rt getBytes:px bytesPerRow:w * 16 fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    return (uint32_t *)px;
}
static float *pixel(uint32_t *img, int w, int x, int y) { return (float *)img + ((size_t)y * w + x) * 4; }
static int near(float a, float b, float tol) { return fabsf(a - b) <= tol; }

/* ---- the checks ---- */
static int line_x; static uint32_t g_vline(int x, int y, int w, int h) { (void)y; (void)w; (void)h; return x == line_x ? 0xffffffff : 0xff000000; }
static int line_y; static uint32_t g_hline(int x, int y, int w, int h) { (void)x; (void)w; (void)h; return y == line_y ? 0xffffffff : 0xff000000; }
static void glow(const char *dir)
{
    printf("glow.fx\n");
    HFx *fx = load(dir, "glow.fx");
    static const float W[13] = {0.002216f, 0.008764f, 0.026995f, 0.064759f, 0.120985f, 0.176033f, 0.199471f, 0.176033f, 0.120985f, 0.064759f, 0.026995f, 0.008764f, 0.002216f};
    for (int dirn = 0; dirn < 2; dirn++) {                 /* OverlayVerticalBlur runs HorzBloomPS (taps along x); the other along y */
        const char *tn = dirn ? "OverlayHorizontalBlur_VS_0_0_PS_2_0" : "OverlayVerticalBlur_VS_0_0_PS_2_0", *samp = dirn ? "SS_GlowTwo" : "SS_GlowOne";
        int t = tech(fx, tn); HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
        Slots sl; memset((void *)&sl, 0, sizeof sl); int s = slot_of(fx, pr, samp);
        CHECK(s == 0, "%s: %s on slot %d (the pass's Sampler[0])", tn, samp, s);
        line_x = line_y = 128; sl.tex[s < 0 ? 0 : s] = tex2d(256, 256, dirn ? g_hline : g_vline);
        Interp in; memset(&in, 0, sizeof in);
        uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 256, 256, 1, NULL); if (!img) continue;
        for (int k = -6; k <= 6; k++) {                     /* the response to a one-texel line: the 13 weights, centred */
            float *p = dirn ? pixel(img, 256, 60, 128 + k) : pixel(img, 256, 128 + k, 60);
            CHECK(near(p[0], W[k + 6], 0.0015f), "%s: tap %d = %.4f, want %.4f", tn, k, p[0], W[k + 6]);
        }
        float *far = dirn ? pixel(img, 256, 60, 140) : pixel(img, 256, 140, 60);
        CHECK(far[0] < 0.0005f, "%s: 12 texels away = %.4f, want 0", tn, far[0]);
        free(img); free(R);
    }
}
static uint32_t g_half(int x, int y, int w, int h) { (void)y; (void)h; return x < w / 2 ? 0xffffffff : 0xff000000; }
static void shadowblur(const char *dir)
{
    printf("shadowblur.fx\n");
    HFx *fx = load(dir, "shadowblur.fx"); int t = tech(fx, "ShadowBlur_VS_0_0_PS_2_0");
    HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int s = slot_of(fx, pr, "ShadowMap"); CHECK(s == 0, "ShadowMap on slot %d", s);
    sl.tex[0] = solid(0xff999999); Interp in; memset(&in, 0, sizeof in);
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 64, 64, 1, NULL);
    if (img) { float *p = pixel(img, 64, 20, 30); CHECK(near(p[0], 0x99 / 255.0f, 0.002f) && near(p[3], 0x99 / 255.0f, 0.002f), "constant input: %.4f, want %.4f (all channels)", p[0], 0x99 / 255.0f); free(img); }
    sl.tex[0] = tex2d(256, 256, g_half);                    /* a hard edge: blurred more towards the top (1 - tc.y), monotonic across */
    img = render(fx, t, 0, &sl, R, n, &in, 256, 256, 1, NULL);
    if (img) {
        float prev = 2; int mono = 1; for (int x = 100; x < 156; x++) { float v = pixel(img, 256, x, 20)[0]; if (v > prev + 1e-4f) mono = 0; prev = v; }
        float top = pixel(img, 256, 126, 20)[0], bottom = pixel(img, 256, 126, 250)[0];
        CHECK(mono, "the edge isn't a monotonic ramp");
        CHECK(top < 0.99f && bottom > top, "blur grows towards the top: top %.3f, bottom %.3f", top, bottom);
        free(img);
    }
    free(R);
}
static void heatshimmer(const char *dir)
{
    printf("heatshimmer.fx\n");
    HFx *fx = load(dir, "heatshimmer.fx");
    int t = tech(fx, "HeatShimmerToScreen_VS_1_1_PS_2_0"); HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int sn = slot_of(fx, pr, "shimmer_noise"), sw = slot_of(fx, pr, "off_screen_world");
    CHECK(sn >= 0 && sw >= 0 && sn != sw, "samplers: shimmer_noise %d, off_screen_world %d", sn, sw);
    sl.tex[sn] = solid(0xff000000); sl.tex[sw] = tex2d(256, 256, g_half);   /* no distortion: the world as it was */
    Interp in; memset(&in, 0, sizeof in);
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 256, 256, 1, NULL);
    if (img) { CHECK(pixel(img, 256, 100, 50)[0] > 0.99f && pixel(img, 256, 156, 50)[0] < 0.01f, "undistorted lookup"); CHECK(near(pixel(img, 256, 100, 50)[3], 0, 0.001f), "alpha = length(normal) = 0"); free(img); }
    sl.tex[sn] = solid(0xff808080);                         /* a shift of shimmer_mod * 0.5 in uv (0.0025: under a texel at 256) */
    img = render(fx, t, 0, &sl, R, n, &in, 256, 256, 1, NULL);
    if (img) { CHECK(near(pixel(img, 256, 100, 50)[3], sqrtf(3) * 0x80 / 255.0f, 0.01f), "alpha = length(normal): %.3f", pixel(img, 256, 100, 50)[3]); free(img); }
    t = tech(fx, "HeatShimmerOffscreen_VS_1_1_PS_2_0"); pr = hlsl_compile_pass(fx, t, 0); free(R); R = regs_for(fx, pr, &n);
    memset((void *)&sl, 0, sizeof sl); int s0 = slot_of(fx, pr, "noise_sampler"); sl.tex[s0 < 0 ? 0 : s0] = solid(0x40336699);
    img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL);
    if (img) { float *p = pixel(img, 8, 3, 3); CHECK(near(p[0], 0x33 / 255.f, 0.002f) && near(p[1], 0x66 / 255.f, 0.002f) && near(p[2], 0x99 / 255.f, 0.002f) && near(p[3], 1, 0.001f), "offscreen: the noise colour with alpha 1"); free(img); }
    free(R);
}
static void dot3(const char *dir)
{
    printf("dot3.fx\n");
    HFx *fx = load(dir, "dot3.fx"); int t = tech(fx, "Dot3StaticDir_VS_1_1_PS_1_1");
    HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int sb = slot_of(fx, pr, "BaseTexture"), sn = slot_of(fx, pr, "NormalMap");
    CHECK(sb == 0 && sn == 1, "Sampler[0] = BaseTexture (%d), Sampler[1] = NormalMap (%d)", sb, sn);
    sl.tex[sb] = solid(0xff808080); sl.tex[sn] = solid(0xff8080ff);   /* a flat normal (0, 0, 1) */
    float amb[4] = {0, 0, 0, 0}; set_param(fx, pr, R, "Ambience", amb, 4);
    Interp in; memset(&in, 0, sizeof in);
    for (int k = 0; k < 4; k++) { in.c1[k] = 1; }
    float lz[3] = {1.0f, 0.75f, 0.5f}, want[3] = {1.0f, 0.5f, 0.0f};    /* LightNorm.z: straight on, 60 degrees, grazing */
    for (int i = 0; i < 3; i++) {
        in.c0[0] = 0.5f; in.c0[1] = 0.5f; in.c0[2] = lz[i]; in.c0[3] = 1;
        uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL); if (!img) continue;
        float *p = pixel(img, 8, 4, 4), nz = (0xff / 255.0f - 0.5f) * 2, nx = (0x80 / 255.0f - 0.5f) * 2, dotv = nx * 0 + nx * 0 + nz * (lz[i] - 0.5f) * 2;
        (void)want; float expect = fminf(fmaxf(dotv, 0), 1) * 0x80 / 255.0f;
        CHECK(near(p[0], expect, 0.01f), "Dot3, light z %.2f: %.3f, want %.3f", lz[i], p[0], expect);
        CHECK(near(p[3], 1, 0.001f), "alpha from Diffuse.w");
        free(img);
    }
    free(R);
}
static void ice(const char *dir)
{
    printf("ice.fx\n");
    HFx *fx = load(dir, "ice.fx"); int t = tech(fx, "IceSurface_VS_1_1_PS_2_0");
    HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int sd = slot_of(fx, pr, "sIceDiffuse"), sr = slot_of(fx, pr, "sRefraction"), sn = slot_of(fx, pr, "sIceNormal");
    CHECK(sd >= 0 && sr >= 0 && sn >= 0, "samplers %d %d %d", sd, sr, sn);
    sl.tex[sd] = solid(0x80ff0000); sl.tex[sr] = solid(0xff0000ff); sl.tex[sn] = solid(0xff000000);   /* half-transparent red over blue */
    float bm = 0; set_param(fx, pr, R, "icesurface_refraction_bump_mod", &bm, 1);
    Interp in; memset(&in, 0, sizeof in); for (int k = 0; k < 4; k++) in.c0[k] = 1;
    in.t[1][0] = 0.5f; in.t[1][1] = 0.5f; in.t[1][2] = 1; in.t[1][3] = 1;
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL);
    if (img) {
        /* color *= color.a scales the alpha too: colour = diffuse * a + refraction * (1 - a * a) */
        float a = 0x80 / 255.0f, *p = pixel(img, 8, 4, 4);
        CHECK(near(p[0], a, 0.01f) && near(p[2], 1 - a * a, 0.01f), "ice blend: %.3f %.3f %.3f, want %.3f 0 %.3f", p[0], p[1], p[2], a, 1 - a * a);
        free(img);
    }
    free(R);
}
static void specular(const char *dir)
{
    printf("specular.fx\n");
    HFx *fx = load(dir, "specular.fx"); int t = tech(fx, "Specular1_VS_2_0_PS_2_0");
    HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int s = slot_of(fx, pr, "TextureMap"); CHECK(s == 0, "TextureMap on slot %d", s);
    sl.tex[s < 0 ? 0 : s] = solid(0x00406080);              /* alpha 0: no specular; colour times Diffuse */
    Interp in; memset(&in, 0, sizeof in); in.c0[0] = 0.5f; in.c0[1] = 1; in.c0[2] = 1; in.c0[3] = 0.25f; in.c1[0] = 1;
    in.t[1][2] = 1; in.t[2][2] = -1; in.t[3][2] = 1;          /* Normal, Vertex, Light */
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL);
    if (img) { float *p = pixel(img, 8, 4, 4); CHECK(near(p[0], 0.5f * 0x40 / 255.f, 0.01f) && near(p[3], 0.25f, 0.002f), "specular diffuse term: %.3f a %.3f", p[0], p[3]); free(img); }
    free(R);
}
static void water(const char *dir)
{
    printf("water.fx\n");
    HFx *fx = load(dir, "water.fx");
    int t = tech(fx, "Underwater_VS_1_1_PS_1_0"); HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int s = slot_of(fx, pr, "sUnderwaterDiffuse"); sl.tex[s < 0 ? 0 : s] = solid(0xff408060);
    Interp in; memset(&in, 0, sizeof in); in.c0[0] = 0.5f; in.c0[1] = 0.5f; in.c0[2] = 0.5f; in.c0[3] = 1;
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL);
    if (img) { float *p = pixel(img, 8, 4, 4); CHECK(near(p[0], 0.5f * 0x40 / 255.f, 0.005f) && near(p[1], 0.5f * 0x80 / 255.f, 0.005f), "underwater: texture * diffuse"); free(img); }
    free(R);
    /* WaterSurface: flat bump (0.5 everywhere), the refraction red, the base texture white, the sky cube blue */
    t = tech(fx, "WaterSurface_VS_1_1_PS_2_0"); pr = hlsl_compile_pass(fx, t, 0); R = regs_for(fx, pr, &n);
    memset((void *)&sl, 0, sizeof sl);
    int sr = slot_of(fx, pr, "sRefraction"), sd = slot_of(fx, pr, "sWaterSurfaceDiffuse"), n1 = slot_of(fx, pr, "sWaterSurfaceNormal1"), n2 = slot_of(fx, pr, "sWaterSurfaceNormal2"), se = slot_of(fx, pr, "sWaterSurfaceEnvMap");
    CHECK(sr >= 0 && sd >= 0 && n1 >= 0 && n2 >= 0 && se >= 0 && pr->skind[se] == HS_CUBE, "water samplers and the cube map's kind");
    sl.tex[sr] = solid(0xffff0000); sl.tex[sd] = solid(0xffffffff); sl.tex[n1] = solid(0xffffffff); sl.tex[n2] = solid(0xff000000); sl.tex[se] = cube(0xff0000ff);
    in.t[2][0] = 0.5f; in.t[2][1] = 0.5f; in.t[2][2] = 1; in.t[2][3] = 1; in.t[3][2] = 1;   /* ProjCoords, Reflection (straight up) */
    for (int k = 0; k < 4; k++) in.c0[k] = 1;
    img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL);
    if (img) {
        /* bump = n1 * n2 = 0: compute_fresnel = 1 (cos term 1), fresnel = (1 - 1)^5 + 0.15 */
        float f = 0.15f, *p = pixel(img, 8, 4, 4);
        CHECK(near(p[0], 1 - f, 0.01f) && near(p[2], f, 0.01f) && near(p[3], 1, 0.001f), "water: red %.3f blue %.3f (fresnel %.3f)", p[0], p[2], f);
        free(img);
    }
    free(R);
}
static void water3(const char *dir)                        /* ps_1_1: COLOR0/COLOR1 as float3 interpolants, four samplers */
{
    printf("water3.fx\n");
    HFx *fx = load(dir, "water3.fx"); int t = tech(fx, "Water3_VS_1_1_PS_1_1");
    HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl);
    int s0 = slot_of(fx, pr, "StaticMap"), s1 = slot_of(fx, pr, "DynamicMap"), s2 = slot_of(fx, pr, "SkyMap"), s3 = slot_of(fx, pr, "GroundMap");
    CHECK(s0 == 0 && s1 == 1 && s2 == 2 && s3 == 3, "Sampler[0..3] = Static, Dynamic, Sky, Ground: %d %d %d %d", s0, s1, s2, s3);
    sl.tex[s0] = solid(0xff808080); sl.tex[s1] = solid(0xffffffff); sl.tex[s2] = solid(0xff200000); sl.tex[s3] = solid(0xff000040);
    Interp in; memset(&in, 0, sizeof in); in.c0[0] = in.c0[1] = in.c0[2] = 1; in.c0[3] = 1; in.c1[0] = 0.5f; in.c1[1] = 0.5f; in.c1[2] = 0.5f;
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 0, NULL);
    if (img) {
        float *p = pixel(img, 8, 4, 4), c0 = 0x80 / 255.f;
        float r = 0.3f * c0 + (0x20 / 255.f) * 0.5f * 4, b = 0.3f * c0 + (0x40 / 255.f) * 0.5f;
        CHECK(near(p[0], r, 0.01f) && near(p[2], b, 0.01f) && near(p[3], 0.7f, 0.002f), "water3: %.3f %.3f %.3f %.3f, want %.3f . %.3f 0.7", p[0], p[1], p[2], p[3], r, b);
        free(img);
    }
    free(R);
}
static void xrgb(const char *dir)
{
    printf("XRGB textures\n");
    HFx *fx = load(dir, "heatshimmer.fx"); int t = tech(fx, "HeatShimmerToScreen_VS_1_1_PS_2_0");
    HProgram *pr = hlsl_compile_pass(fx, t, 0); int n; float *R = regs_for(fx, pr, &n);
    Slots sl; memset((void *)&sl, 0, sizeof sl); int sn = slot_of(fx, pr, "shimmer_noise"), sw = slot_of(fx, pr, "off_screen_world");
    sl.tex[sn] = solid(0x00000000); sl.tex[sw] = solid(0x10203040);    /* the world texture's alpha 0x10, but the target is XRGB */
    Interp in; memset(&in, 0, sizeof in);
    int ok = 1;
    /* color.w is overwritten by length(normal), so look at the sample itself through a patched copy: not possible here;
       instead: the noise texture is XRGB with alpha 0 -> normal's xyz unaffected, and alpha must not leak into xyz */
    sl.xrgb[sn] = 1; sl.xrgb[sw] = 1;
    uint32_t *img = render(fx, t, 0, &sl, R, n, &in, 8, 8, 1, &pr);
    if (img) { float *p = pixel(img, 8, 4, 4); ok = near(p[0], 0x20 / 255.f, 0.003f) && near(p[2], 0x40 / 255.f, 0.003f); free(img); }
    CHECK(ok, "XRGB samples keep their colour");
    CHECK(strstr(pr->msl, "hlsl_x1(g.xr_off_screen_world") != NULL, "samples go through the XRGB alpha rule");
    free(R);
}
static void all_passes(const char *dir)                    /* every technique's passes: compiled, slots unique and in range */
{
    printf("every pass: sampler slots\n");
    const char *files[] = {"dot3.fx", "glow.fx", "heatshimmer.fx", "ice.fx", "shadowblur.fx", "specular.fx", "water.fx", "water2.fx", "water3.fx", "water3b.fx", "water4.fx"};
    for (size_t f = 0; f < sizeof files / sizeof *files; f++) {
        HFx *fx = load(dir, files[f]);
        for (int t = 0; t < fx->nt; t++) for (int p = 0; p < fx->t[t].npass; p++) {
            HProgram *pr = hlsl_compile_pass(fx, t, p); if (!pr || pr->err[0]) { CHECK(0, "%s %s: %s", files[f], fx->t[t].name, pr ? pr->err : "?"); continue; }
            if (pr->ps_ffp) continue;
            int seen[64] = {0};
            for (int k = 0; k < pr->nsamp; k++) { int v = pr->sslot[k]; if (v < 0) continue; CHECK(!seen[v], "%s %s: sampler %s on two slots", files[f], fx->t[t].name, fx->v[v].name); seen[v & 63] = 1; }
            CHECK(pr->nsamp <= 8, "%s %s: %d samplers (8 stages)", files[f], fx->t[t].name, pr->nsamp);
        }
    }
}
int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) { printf("usage: fx_render_test <folder with the game's .fx files>\n"); return 2; }
        dev = MTLCreateSystemDefaultDevice(); q = [dev newCommandQueue];
        glow(argv[1]); shadowblur(argv[1]); heatshimmer(argv[1]); dot3(argv[1]); ice(argv[1]); specular(argv[1]); water(argv[1]); water3(argv[1]); xrgb(argv[1]); all_passes(argv[1]);
        printf("%d checks, %d failed\n", checks, fails);
        return fails ? 1 : 0;
    }
}
