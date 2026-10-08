#version 330 core
layout(std140) uniform GameUniforms {
    mat4 wvp;        // world*view*proj (D3D row-vector order; uploaded as-is => m * v)
    mat4 wv;         // world*view
    mat4 tex0;       // texture-0 transform
    vec4 vp;           // viewport x, y, w, h (pixels)
    vec4 fog;          // start, end, enabled, unused
    vec4 fogcolor;
    vec4 tfactor;
    ivec4 c0;             // stage 0: colorop, colorarg1, colorarg2, alphaop
    ivec4 a0;             // stage 0: alphaarg1, alphaarg2, texcoordindex, texturetransformflags
    ivec4 c1;             // stage 1: colorop, colorarg1, colorarg2, alphaop
    ivec4 a1;             // stage 1: alphaarg1, alphaarg2, unused, unused
    vec4 at;           // alpha test: ref (0..1), func, enabled, unused
    ivec4 misc;           // pretransformed, has diffuse, has texture bound (stage0), has texture bound (stage1)
    ivec4 xrgb;           // stage 0/1 texture has no alpha channel
    mat4 tex1;       // texture-1 transform
    ivec4 s1;             // stage 1: texcoordindex, texturetransformflags, unused, unused
} u;
in vec4 o_diffuse; in vec2 o_uv; in vec2 o_uv1; in vec3 o_tcproj; in float o_fogf;
uniform sampler2D t0; uniform sampler2D t1; out vec4 fragColor;
vec4 sat(vec4 v) { return clamp(v, 0.0, 1.0); }
vec4 arg(int a, vec4 diffuse, vec4 current, vec4 texel, vec4 tfactor)
{
    vec4 v;
    switch (a & 0xf) { case 0: v = diffuse; break; case 1: v = current; break; case 2: v = texel; break; case 3: v = tfactor; break; default: v = diffuse; }
    if ((a & 0x10) != 0) v = 1.0 - v;            // D3DTA_COMPLEMENT
    if ((a & 0x20) != 0) v = vec4(v.a);        // D3DTA_ALPHAREPLICATE
    return v;
}
vec4 op(int o, vec4 x, vec4 y, vec4 diffuse, vec4 texel, vec4 tf, vec4 current)
{
    switch (o) {
    case 2: return x;                                   // SELECTARG1
    case 3: return y;                                   // SELECTARG2
    case 4: return x * y;                               // MODULATE
    case 5: return sat(x * y * 2);                 // MODULATE2X
    case 6: return sat(x * y * 4);                 // MODULATE4X
    case 7: return sat(x + y);                     // ADD
    case 8: return sat(x + y - 0.5);               // ADDSIGNED
    case 9: return sat((x + y - 0.5) * 2);         // ADDSIGNED2X
    case 10: return sat(x - y);                    // SUBTRACT
    case 11: return sat(x + y - x * y);            // ADDSMOOTH
    case 12: return mix(y, x, diffuse.a);               // BLENDDIFFUSEALPHA
    case 13: return mix(y, x, texel.a);                 // BLENDTEXTUREALPHA
    case 14: return mix(y, x, tf.a);                    // BLENDFACTORALPHA
    case 15: return sat(x + y * (1 - texel.a));    // BLENDTEXTUREALPHAPM
    case 16: return mix(y, x, current.a);               // BLENDCURRENTALPHA
    default: return x;
    }
}


void main() {
    vec4 diffuse = o_diffuse, current = diffuse;
    vec2 uv0 = o_uv;
    if ((u.a0.z & 0xffff0000) == 0x20000) uv0 = (u.a0.w & 0x100) != 0 ? o_tcproj.xy / o_tcproj.z : o_tcproj.xy;   // TCI_CAMERASPACEPOSITION
    vec4 texel = u.misc.z != 0 ? texture(t0, uv0) : vec4(1);
    if (u.xrgb.x != 0) texel.a = 1;
    if (u.c0.x != 1) {
        vec4 c = op(u.c0.x, arg(u.c0.y, diffuse, current, texel, u.tfactor), arg(u.c0.z, diffuse, current, texel, u.tfactor), diffuse, texel, u.tfactor, current);
        vec4 a = u.c0.w == 1 ? current : op(u.c0.w, arg(u.a0.x, diffuse, current, texel, u.tfactor), arg(u.a0.y, diffuse, current, texel, u.tfactor), diffuse, texel, u.tfactor, current);
        current = vec4(c.rgb, a.a);
        if (u.c1.x != 1) {
            vec4 texel1 = u.misc.w != 0 ? texture(t1, o_uv1) : vec4(1);
            if (u.xrgb.y != 0) texel1.a = 1;
            vec4 c1 = op(u.c1.x, arg(u.c1.y, diffuse, current, texel1, u.tfactor), arg(u.c1.z, diffuse, current, texel1, u.tfactor), diffuse, texel1, u.tfactor, current);
            vec4 a1 = u.c1.w == 1 ? current : op(u.c1.w, arg(u.a1.x, diffuse, current, texel1, u.tfactor), arg(u.a1.y, diffuse, current, texel1, u.tfactor), diffuse, texel1, u.tfactor, current);
            current = vec4(c1.rgb, a1.a);
        }
    }
    if (u.at.z != 0) {                                   // alpha test (D3DCMP_*)
        float a = current.a, r = u.at.x; int f = int(u.at.y); bool pass;
        switch (f) { case 1: pass = false; break; case 2: pass = a < r; break; case 3: pass = a == r; break; case 4: pass = a <= r; break;
                     case 5: pass = a > r; break; case 6: pass = a != r; break; case 7: pass = a >= r; break; default: pass = true; }
        if (!pass) discard;
    }
    current.rgb = mix(u.fogcolor.rgb, current.rgb, o_fogf);
    fragColor = current;
}
