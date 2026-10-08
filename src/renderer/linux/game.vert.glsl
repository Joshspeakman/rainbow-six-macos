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
layout(location=0) in vec4 v_pos;
layout(location=1) in vec4 v_diffuse;
layout(location=2) in vec2 v_uv;
out vec4 o_diffuse; out vec2 o_uv; out vec2 o_uv1; out vec3 o_tcproj; out float o_fogf;
vec2 ttf(vec2 uv, mat4 m, int tci, int flags)
{
    int count = flags & 0xff;
    if (count == 0 || (tci & 0xffff0000) != 0) return uv;
    vec4 t = m * vec4(uv, 1.0, 0.0);
    if ((flags & 0x100) != 0) { float q = count == 2 ? t.y : count == 3 ? t.z : t.w; return q != 0 ? t.xy / q : t.xy; }
    return t.xy;
}

void main() {

    if (u.misc.x != 0) {          // XYZRHW: already in screen space
        float w = v_pos.w != 0 ? 1.0 / v_pos.w : 1.0;
        float x = (v_pos.x + 0.5 - u.vp.x) / u.vp.z * 2.0 - 1.0;   // +0.5: D3D samples pixel centres at integers
        float y = 1.0 - (v_pos.y + 0.5 - u.vp.y) / u.vp.w * 2.0;
        gl_Position = vec4(x * w, y * w, v_pos.z * w, w);
        o_fogf = 1.0;
        o_tcproj = vec3(0, 0, 1);
    } else {
        vec4 p = vec4(v_pos.xyz, 1.0);
        gl_Position = u.wvp * p;
        gl_Position.x += gl_Position.w / u.vp.z;      // half-pixel: same rasterisation as D3D7
        gl_Position.y -= gl_Position.w / u.vp.w;
        vec4 cam = u.wv * p;
        o_fogf = u.fog.z != 0 ? clamp((u.fog.y - cam.z) / (u.fog.y - u.fog.x), 0.0, 1.0) : 1.0;
        o_tcproj = (u.tex0 * cam).xyz;
    }
    o_diffuse = v_diffuse;
    o_uv = ttf(v_uv, u.tex0, u.a0.z, u.a0.w);
    o_uv1 = ttf(v_uv, u.tex1, u.s1.x, u.s1.y);

    gl_Position.y = -gl_Position.y;
    gl_Position.z = gl_Position.z * 2.0 - gl_Position.w;
}
