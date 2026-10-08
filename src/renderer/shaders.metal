// DSR fixed-function emulation for Dungeon Siege's Direct3D 7 usage (lighting off; 2 texture stages; vertex fog).
#include <metal_stdlib>
using namespace metal;

struct Uniforms {
    float4x4 wvp;        // world*view*proj (D3D row-vector order; uploaded as-is => m * v)
    float4x4 wv;         // world*view
    float4x4 tex0;       // texture-0 transform
    float4 vp;           // viewport x, y, w, h (pixels)
    float4 fog;          // start, end, enabled, unused
    float4 fogcolor;
    float4 tfactor;
    int4 c0;             // stage 0: colorop, colorarg1, colorarg2, alphaop
    int4 a0;             // stage 0: alphaarg1, alphaarg2, texcoordindex, texturetransformflags
    int4 c1;             // stage 1: colorop, colorarg1, colorarg2, alphaop
    int4 a1;             // stage 1: alphaarg1, alphaarg2, unused, unused
    float4 at;           // alpha test: ref (0..1), func, enabled, unused
    int4 misc;           // pretransformed, has diffuse, has texture bound (stage0), has texture bound (stage1)
    int4 xrgb;           // stage 0/1 texture has no alpha channel
    float4x4 tex1;       // texture-1 transform
    int4 s1;             // stage 1: texcoordindex, texturetransformflags, unused, unused
};

struct VIn {
    float4 pos [[attribute(0)]];     // xyz (w=1) or xyz+rhw
    float4 diffuse [[attribute(1)]]; // BGRA8 normalized (bound to a constant white buffer when absent)
    float2 uv [[attribute(2)]];
};
struct VOut {
    float4 pos [[position]];
    float4 diffuse;
    float2 uv;                       // stage 0 coordinates (after its texture transform)
    float2 uv1;                      // stage 1 coordinates
    float3 tcproj;                   // projected texgen (camera-space position * tex0)
    float fogf;
};

/* D3DTSS_TEXTURETRANSFORMFLAGS on passed-through coordinates: 2-D coordinates enter the texture matrix as (u, v, 1, 0),
 * so translation lives in the third row (how the game scrolls river and waterfall layers). Camera-space texgen is
 * handled separately (tcproj). */
static float2 ttf(float2 uv, float4x4 m, int tci, int flags)
{
    int count = flags & 0xff;
    if (count == 0 || (tci & 0xffff0000) != 0) return uv;
    float4 t = m * float4(uv, 1.0, 0.0);
    if (flags & 0x100) { float q = count == 2 ? t.y : count == 3 ? t.z : t.w; return q != 0 ? t.xy / q : t.xy; }
    return t.xy;
}
vertex VOut vs_main(VIn in [[stage_in]], constant Uniforms &u [[buffer(1)]])
{
    VOut o;
    if (u.misc.x) {          // XYZRHW: already in screen space
        float w = in.pos.w != 0 ? 1.0 / in.pos.w : 1.0;
        float x = (in.pos.x + 0.5 - u.vp.x) / u.vp.z * 2.0 - 1.0;   // +0.5: D3D samples pixel centres at integers
        float y = 1.0 - (in.pos.y + 0.5 - u.vp.y) / u.vp.w * 2.0;
        o.pos = float4(x * w, y * w, in.pos.z * w, w);
        o.fogf = 1.0;
        o.tcproj = float3(0, 0, 1);
    } else {
        float4 p = float4(in.pos.xyz, 1.0);
        o.pos = u.wvp * p;
        o.pos.x += o.pos.w / u.vp.z;      // half-pixel: same rasterisation as D3D7
        o.pos.y -= o.pos.w / u.vp.w;
        float4 cam = u.wv * p;
        o.fogf = u.fog.z != 0 ? clamp((u.fog.y - cam.z) / (u.fog.y - u.fog.x), 0.0, 1.0) : 1.0;
        o.tcproj = (u.tex0 * cam).xyz;
    }
    o.diffuse = in.diffuse;
    o.uv = ttf(in.uv, u.tex0, u.a0.z, u.a0.w);
    o.uv1 = ttf(in.uv, u.tex1, u.s1.x, u.s1.y);
    return o;
}

static float4 arg(int a, float4 diffuse, float4 current, float4 texel, float4 tfactor)
{
    float4 v;
    switch (a & 0xf) { case 0: v = diffuse; break; case 1: v = current; break; case 2: v = texel; break; case 3: v = tfactor; break; default: v = diffuse; }
    if (a & 0x10) v = 1.0 - v;            // D3DTA_COMPLEMENT
    if (a & 0x20) v = float4(v.a);        // D3DTA_ALPHAREPLICATE
    return v;
}
static float4 op(int o, float4 x, float4 y, float4 diffuse, float4 texel, float4 tf, float4 current)
{
    switch (o) {
    case 2: return x;                                   // SELECTARG1
    case 3: return y;                                   // SELECTARG2
    case 4: return x * y;                               // MODULATE
    case 5: return saturate(x * y * 2);                 // MODULATE2X
    case 6: return saturate(x * y * 4);                 // MODULATE4X
    case 7: return saturate(x + y);                     // ADD
    case 8: return saturate(x + y - 0.5);               // ADDSIGNED
    case 9: return saturate((x + y - 0.5) * 2);         // ADDSIGNED2X
    case 10: return saturate(x - y);                    // SUBTRACT
    case 11: return saturate(x + y - x * y);            // ADDSMOOTH
    case 12: return mix(y, x, diffuse.a);               // BLENDDIFFUSEALPHA
    case 13: return mix(y, x, texel.a);                 // BLENDTEXTUREALPHA
    case 14: return mix(y, x, tf.a);                    // BLENDFACTORALPHA
    case 15: return saturate(x + y * (1 - texel.a));    // BLENDTEXTUREALPHAPM
    case 16: return mix(y, x, current.a);               // BLENDCURRENTALPHA
    default: return x;
    }
}

/* Pipelines for draws without alpha testing are built with kAlphaTest false: a shader that can discard keeps the GPU
 * from removing hidden surfaces before shading (Apple GPUs' HSR) for every draw that uses it. */
constant bool kAlphaTest [[function_constant(0)]];
fragment float4 fs_main(VOut in [[stage_in]], constant Uniforms &u [[buffer(1)]],
                        texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]],
                        texture2d<float> t1 [[texture(1)]], sampler s1 [[sampler(1)]])
{
    float4 diffuse = in.diffuse, current = diffuse;
    float2 uv0 = in.uv;
    if ((u.a0.z & 0xffff0000) == 0x20000) uv0 = (u.a0.w & 0x100) ? in.tcproj.xy / in.tcproj.z : in.tcproj.xy;   // TCI_CAMERASPACEPOSITION
    float4 texel = u.misc.z ? t0.sample(s0, uv0) : float4(1);
    if (u.xrgb.x) texel.a = 1;
    if (u.c0.x != 1) {
        float4 c = op(u.c0.x, arg(u.c0.y, diffuse, current, texel, u.tfactor), arg(u.c0.z, diffuse, current, texel, u.tfactor), diffuse, texel, u.tfactor, current);
        float4 a = u.c0.w == 1 ? current : op(u.c0.w, arg(u.a0.x, diffuse, current, texel, u.tfactor), arg(u.a0.y, diffuse, current, texel, u.tfactor), diffuse, texel, u.tfactor, current);
        current = float4(c.rgb, a.a);
        if (u.c1.x != 1) {
            float4 texel1 = u.misc.w ? t1.sample(s1, in.uv1) : float4(1);
            if (u.xrgb.y) texel1.a = 1;
            float4 c1 = op(u.c1.x, arg(u.c1.y, diffuse, current, texel1, u.tfactor), arg(u.c1.z, diffuse, current, texel1, u.tfactor), diffuse, texel1, u.tfactor, current);
            float4 a1 = u.c1.w == 1 ? current : op(u.c1.w, arg(u.a1.x, diffuse, current, texel1, u.tfactor), arg(u.a1.y, diffuse, current, texel1, u.tfactor), diffuse, texel1, u.tfactor, current);
            current = float4(c1.rgb, a1.a);
        }
    }
    if (kAlphaTest && u.at.z != 0) {                     // alpha test (D3DCMP_*)
        float a = current.a, r = u.at.x; int f = int(u.at.y); bool pass;
        switch (f) { case 1: pass = false; break; case 2: pass = a < r; break; case 3: pass = a == r; break; case 4: pass = a <= r; break;
                     case 5: pass = a > r; break; case 6: pass = a != r; break; case 7: pass = a >= r; break; default: pass = true; }
        if (!pass) discard_fragment();
    }
    current.rgb = mix(u.fogcolor.rgb, current.rgb, in.fogf);
    return current;
}

// Blit / fill helper: textured or solid quad into a destination rectangle.
struct QOut { float4 pos [[position]]; float2 uv; };
struct QUni { float4 dst; float4 src; float4 color; int4 mode; };   // dst/src in normalized [0,1] rects; mode.x: 0 tex, 1 fill
vertex QOut vs_quad(uint vid [[vertex_id]], constant QUni &q [[buffer(0)]])
{
    float2 c = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    float2 d = mix(q.dst.xy, q.dst.zw, c);
    QOut o; o.pos = float4(d.x * 2 - 1, 1 - d.y * 2, 0, 1); o.uv = mix(q.src.xy, q.src.zw, c); return o;
}
fragment float4 fs_quad(QOut in [[stage_in]], constant QUni &q [[buffer(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    return q.mode.x ? q.color : t.sample(s, in.uv);
}
// Depth clear helper: writes a constant depth.
struct ZOut { float4 pos [[position]]; };
vertex ZOut vs_zclear(uint vid [[vertex_id]], constant float4 &z [[buffer(0)]])
{
    float2 c = float2((vid & 1) ? 1.0 : -1.0, (vid & 2) ? -1.0 : 1.0); ZOut o; o.pos = float4(c, z.x, 1); return o;
}

// Present: back buffer -> drawable, through the game's gamma ramp (256-entry LUT per channel).
struct POut { float4 pos [[position]]; float2 uv; };
vertex POut vs_present(uint vid [[vertex_id]])
{
    float2 c = float2((vid << 1) & 2, vid & 2);   // fullscreen triangle
    POut o; o.pos = float4(c * 2 - 1, 0, 1); o.uv = float2(c.x, 1 - c.y); return o;
}
fragment float4 fs_present(POut in [[stage_in]], texture2d<float> src [[texture(0)]], texture1d<float> lut [[texture(1)]], sampler s [[sampler(0)]])
{
    float3 c = src.sample(s, in.uv).rgb;
    uint3 i = uint3(saturate(c) * 255.0 + 0.5);
    return float4(lut.read(i.r).r, lut.read(i.g).g, lut.read(i.b).b, 1);
}

// Frame-time graph: 240 columns (oldest left), y = 0..40 ms. g[0..239] game frame interval, g[240..479] time on screen.
fragment float4 fs_graph(POut in [[stage_in]], constant float *g [[buffer(0)]])
{
    float x = in.uv.x, y = (1.0 - in.uv.y) * 40.0;           // ms at this pixel row
    int i = clamp(int(x * 240.0), 0, 239);
    float px = 40.0 / 200.0;                                  // ~1 px line thickness in ms
    float4 c = float4(0, 0, 0, 0.55);
    if (abs(y - 8.333) < px * 0.6 || abs(y - 12.5) < px * 0.6 || abs(y - 16.667) < px * 0.6) c = float4(0.6, 0.6, 0.2, 0.8);
    float a = g[i], b = g[240 + i];
    if (b > 0 && abs(y - b) < px * 1.5) c = float4(0.2, 1.0, 0.3, 1);
    if (a > 0 && abs(y - a) < px * 1.2) c = float4(1, 1, 1, 1);
    if (a > 40.0 && y > 38.5) c = float4(1, 0.2, 0.2, 1);   // off-scale spike marker
    return c;
}
