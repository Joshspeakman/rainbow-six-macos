/* Direct3D 9 internals shared by d3d9.c and d3dx9.c */
#pragma once
#include "w32.h"
#include "dsr_producer.h"
#include "../../../src/dsr/dsr_proto.h"
#include <math.h>
#include <unistd.h>

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_NOTFOUND 0x88760866u
#define E_NOINTERFACE_ 0x80004002u
#define S_FALSE_ 1u
enum { O9_D3D = 16, O9_DEV, O9_SURF, O9_TEX, O9_CUBE, O9_VOLTEX, O9_VB, O9_IB, O9_DECL, O9_VS, O9_PS, O9_SWAP, O9_QUERY, O9_SB, O9_VOL };
enum { RT_SURFACE = 1, RT_VOLUME, RT_TEXTURE, RT_VOLUMETEXTURE, RT_CUBETEXTURE, RT_VERTEXBUFFER, RT_INDEXBUFFER };
enum { USAGE_RENDERTARGET = 1, USAGE_DEPTHSTENCIL = 2, USAGE_WRITEONLY = 8, USAGE_DYNAMIC = 0x200, USAGE_AUTOGENMIPMAP = 0x400 };
enum { LOCK_READONLY = 0x10, LOCK_DISCARD = 0x2000, LOCK_NOOVERWRITE = 0x1000 };

int d9_fmt_block(uint32_t f); int d9_fmt_bytes(uint32_t f); int d9_fmt_float(uint32_t f);
int d9_fmt_known(uint32_t f); int d9_fmt_depth(uint32_t f); int d9_fmt_alpha(uint32_t f);
uint32_t d9_pitch(uint32_t f, uint32_t w); uint32_t d9_rows(uint32_t f, uint32_t h); uint32_t d9_size(uint32_t f, uint32_t w, uint32_t h);
void d9_to_bgra(uint32_t f, const uint8_t *src, uint32_t pitch, uint32_t w, uint32_t y0, uint32_t y1, uint8_t *dst);
void d9_from_bgra(uint32_t f, const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, uint32_t pitch);

static int d9_log = -1;
static int logon(void) { if (d9_log < 0) d9_log = getenv("D3D9_LOG") ? atoi(getenv("D3D9_LOG")) ? atoi(getenv("D3D9_LOG")) : 1 : 0; return d9_log; }
#define M(name, n) static void name(Ctx *c) { enum { nargs_ = n }; { static int calls_; if (logon() && calls_++ < (d9_log > 1 ? 1000000 : 3)) fprintf(stderr, "d3d9: %s(%08x %08x %08x %08x)\n", #name, ARG(1), ARG(2), ARG(3), ARG(4)); }
#define END }
#define ME(T, ty) T *me = obj(ARG(0), ty); if (!me) { fprintf(stderr, "d3d9: bad " #T " %08x in %s\n", ARG(0), __func__); RET(D3DERR_INVALIDCALL, nargs_); }
#define TODO_M(name, n, ret) M(name, n) { static int once_; if (!once_++) fprintf(stderr, "d3d9: %s is not implemented yet\n", #name); } RET(ret, n); END
#define OK_M(name, n) M(name, n) RET(D3D_OK, n); END

static uint32_t gmem(uint32_t n) { return n >= 0x10000 ? vm_alloc(0, (n + 0xfff) & ~0xfffu, 0x3000, 4) : heap_alloc(w32_process_heap, 8, n ? n : 4); }
static void gfree_(uint32_t a, uint32_t n) { if (!a) return; if (n >= 0x10000) vm_free(a, 0, 0x8000); else heap_free(w32_process_heap, a); }
static float f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static uint32_t u32f(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

typedef struct Dev9 Dev9;
typedef struct Tex9 Tex9;
/* a surface: the back buffer, a depth buffer, a render target, an offscreen surface, or one level of a texture */
typedef struct Surf9 {
    uint32_t g; int ref; Dev9 *dev; Tex9 *tex; uint32_t level, face;
    uint32_t id, w, h, fmt, usage, pool; int kind;     /* renderer kind: 1 render target, 2 depth, 3 texture, 4 offscreen */
    uint32_t mem, pitch, size;                          /* CPU copy in the game's format */
    int locked; uint32_t lflags, ly0, ly1; int gpu_newer; uint32_t samples;
} Surf9;
struct Tex9 {
    uint32_t g; int ref; Dev9 *dev; int type;          /* O9_TEX, O9_CUBE, O9_VOLTEX */
    uint32_t w, h, d, levels, fmt, usage, pool, lod;
    Surf9 *lv[6][16];                                   /* [face][level] (volume textures: slice 0 only, for now) */
};
typedef struct Buf9 { uint32_t g; int ref; Dev9 *dev; int index; uint32_t len, usage, fmt, pool, mem; uint32_t gid, dlo, dhi, llo, lhi; } Buf9;   /* gid: the renderer's copy (0 none); [dlo, dhi): written since it was sent; [llo, lhi): locked now */
typedef struct DElem { uint8_t stream, type, usage, uidx; uint16_t off, dst, size; } DElem;
typedef struct Decl9 { uint32_t g; int ref; Dev9 *dev; DElem e[32]; int n; uint32_t raw, nraw, fvf; int ok; } Decl9;
typedef struct Shader9 { uint32_t g; int ref; Dev9 *dev; int ps; uint32_t mem, len; } Shader9;
typedef struct Query9 { uint32_t g; int ref; Dev9 *dev; uint32_t type, slot, serial, last, polls; } Query9;
typedef struct RecEnt { uint8_t kind; uint32_t a, b; float m[16]; void *p; } RecEnt;
enum { REC_RS, REC_TSS, REC_SS, REC_TEX, REC_XF, REC_FVF, REC_DECL, REC_VS, REC_PS, REC_VP, REC_MAT, REC_STREAM, REC_IND };
typedef struct SB9 { uint32_t g; int ref; Dev9 *dev; RecEnt *e; int n, cap; } SB9;

struct Dev9 {
    uint32_t g, g_swap, g_d3d; int ref;
    uint32_t pp[14];                                    /* D3DPRESENT_PARAMETERS */
    uint32_t hwnd, behavior;
    Surf9 *back, *depth, *rt, *ds;
    uint32_t rs[256], tss[8][33], ss[16][14];
    float xf[512][16];                                  /* by D3DTRANSFORMSTATETYPE (WORLD = 256) */
    Tex9 *tex[16]; uint32_t fvf; Decl9 *decl;
    Buf9 *vb[16]; uint32_t vb_off[16], vb_stride[16]; Buf9 *ib;
    uint32_t vp[6]; uint8_t material[68]; uint8_t light[8][104]; int light_on[8];
    Shader9 *vs, *ps; float vsc[256][4], psc[224][4]; int32_t vsi[16][4], psi[16][4]; uint32_t vsb[16], psb[16];
    float clip[6][4]; int32_t scissor[4];
    SB9 *rec;                                           /* recording a state block (BeginStateBlock) */
    Surf9 *cursor; int cursor_x, cursor_y, cursor_hx, cursor_hy, cursor_show;
    uint8_t *vbuf; size_t vbuf_cap;                     /* draw-time staging */
    uint32_t prog;                                      /* an effect pass's Metal program (d3dx9_fx.c), 0 if none */
};

/* d3d9.c */
uint32_t d9_create_tex(Dev9 *d, int type, uint32_t w, uint32_t h, uint32_t dep, uint32_t levels, uint32_t usage, uint32_t fmt, uint32_t pool, uint32_t out);
Tex9 *d9_any_tex(uint32_t g);
void d9_tex_addref(uint32_t g); void d9_tex_release(uint32_t g);   /* a texture interface's reference (0 ignored) */
uint32_t d9_surf_lock(Surf9 *s, uint32_t locked, uint32_t rect, uint32_t flags);
uint32_t d9_surf_unlock(Surf9 *s);
void d9_surf_upload(Surf9 *s, uint32_t y0, uint32_t y1);
/* the game's own shaders (d3d9_shader.c) */
uint32_t d9_shader_program(Dev9 *d);
void d9_shader_consts(Dev9 *d, uint32_t id);
void d9_shader_forget(Shader9 *s);
/* for the effect framework (d3dx9_fx.c) */
void d9_set_rs(Dev9 *d, uint32_t s, uint32_t v);
void d9_set_tss(Dev9 *d, uint32_t stage, uint32_t t, uint32_t v);
void d9_set_ss(Dev9 *d, uint32_t sampler, uint32_t t, uint32_t v);
void d9_set_texture(Dev9 *d, uint32_t stage, uint32_t tex_iface);     /* 0: none */
uint32_t d9_texture_at(Dev9 *d, uint32_t stage);                       /* the interface bound there, or 0 */
uint32_t d9_program_create(const char *msl, size_t len, uint32_t kinds);
void d9_program_use(Dev9 *d, uint32_t prog);
void d9_program_consts(Dev9 *d, const float *regs, int nregs);
/* which device states an effect's technique sets (as D3DX's Begin saves and End restores only those) */
typedef struct D9Mask { uint8_t rs[256], tss[8][33], ss[16][14], tex[16], prog; } D9Mask;
void *d9_state_save(Dev9 *d, const D9Mask *m);                         /* those states and textures, the program */
void d9_state_restore(Dev9 *d, void *saved);                           /* and frees it */
int d9_shaders_on(void);
