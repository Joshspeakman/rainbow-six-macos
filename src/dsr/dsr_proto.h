/* DSR command stream: a sequence of { u32 op; u32 size (payload bytes, multiple of 4); payload } records.
 * All objects are referred to by ids allocated by the DirectDraw layer. Vertex data is copied into the stream at draw time. */
#ifndef DSR_PROTO_H
#define DSR_PROTO_H
enum {
    DSR_NOP = 0,
    DSR_RENDER_STATE,      /* u32 state, u32 value */
    DSR_TSS,               /* u32 stage, u32 type, u32 value */
    DSR_TEXTURE,           /* u32 stage, u32 surface id (0 = none) */
    DSR_TRANSFORM,         /* u32 type, float m[16] */
    DSR_VIEWPORT,          /* u32 x, y, w, h; float minz, maxz */
    DSR_CLEAR,             /* u32 flags, u32 color, float z, u32 stencil, u32 nrects, RECT rects[n] */
    DSR_DRAW,              /* u32 prim, u32 fvf, u32 nverts, u32 nindices, verts[nverts*stride], u16 indices[n] (padded to 4) */
    DSR_SET_RT,            /* u32 color id, u32 depth id */
    DSR_SURFACE_CREATE,    /* u32 id, w, h, u32 kind (1 rt, 2 depth, 3 texture, 4 offscreen), u32 format (1 argb, 2 xrgb, 3 z32), u32 parent id, u32 level,
                              u32 levels, u32 cube face, u32 flags (1 = a cube texture's surface) */
    DSR_SURFACE_DESTROY,   /* u32 id */
    DSR_SURFACE_UPLOAD,    /* u32 id, u32 x, y, w, h, u32 pitch, pixels[h*pitch] */
    DSR_BLT,               /* u32 dst, RECT dstrect, u32 src (0 = fill), RECT srcrect, u32 flags, u32 fillcolor */
    DSR_PRESENT,           /* u32 surface id [, u32 qpc_lo, qpc_hi, qpc_freq: when the game finished the frame] */
    DSR_GAMMA,             /* u16 red[256], green[256], blue[256] */
    DSR_MODE,              /* u32 w, h */
    DSR_SCENE,             /* u32 begin (1) / end (0) */
    DSR_READBACK,          /* u32 surface id, u32 x, y, w, h, u32 cookie: write pixels into the readback area, then ack */
    DSR_UPLOAD_OVER,       /* as SURFACE_UPLOAD, but composited over the GPU copy as premultiplied alpha (ONE, INVSRCALPHA) */
    DSR_PROG_CREATE,       /* u32 id, u32 texture kinds (2 bits per slot: 0 2D, 1 cube, 2 3D), u32 length, Metal source
                              text (vs_fx, fs_fx; padded to 4): an effect pass (hlsl.c) */
    DSR_PROG_USE,          /* u32 id (0: fixed function again) */
    DSR_PROG_CONST,        /* u32 count, float4 registers[count]: the bound program's parameters */
    DSR_DRAW_PROG,         /* u32 prim, u32 attribute mask, u32 nverts, u32 nindices, u32 flags (1 pretransformed),
                              float4 attributes[nverts][popcount(mask)] in slot order, u16 indices[n] (padded to 4) */
    DSR_GLSL_CREATE,       /* u32 id, u32 nattr, u32 vs length, u32 ps length, {u32 slot; char name[28]} attr[nattr], vertex
                              GLSL, pixel GLSL (each NUL-terminated, padded to 4): the game's own shader pair (d3d9_shader.c) */
    DSR_SHADER_CONST,      /* u32 vs/ps float4 counts, vs/ps int4 counts, vs/ps bool counts, then those registers in that order
                              (bools one u32 each), then float4 alpha test (reference 0-255, D3DCMP function, enabled, 0) */
    DSR_QUERY,             /* u32 slot, u32 begin (1) / end (0), u32 serial: an occlusion query around the draws between; the
                              renderer publishes the samples passed in the header (dsr_shm.h DSR_QUERY_OFFSET) */
    DSR_SCISSOR,           /* i32 left, top, right, bottom: the scissor rectangle (D3DRS_SCISSORTESTENABLE turns it on) */
    DSR_VBUF_DATA,         /* u32 id (< DSR_VBUFS), u32 buffer size, u32 offset, u32 n, bytes[n] (padded to 4): a vertex or
                              index buffer's bytes kept by the renderer (created, or recreated at a new size, on first data) */
    DSR_VBUF_FREE,         /* u32 id */
    DSR_DRAW_PROG_VB,      /* u32 prim, nverts, nindices, flags (1 pretransformed, 2 32-bit indices), u32 index buffer id,
                              u32 index byte offset, i32 base vertex, u32 first vertex (no indices), u32 nelem,
                              {u32 slot, buffer id, byte offset, stride, D3DDECLTYPE}[nelem]: as DSR_DRAW_PROG, from kept buffers */
};
#define DSR_VBUFS 65536
#endif
