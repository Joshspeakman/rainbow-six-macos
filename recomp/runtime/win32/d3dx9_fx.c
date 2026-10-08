/* D3DX effects (ID3DXEffect, from D3DXCreateEffect) on the HLSL compiler (hlsl.c): parameters with semantics and
 * annotations, techniques and passes. A pass applies its render, texture-stage and sampler states to the device and
 * binds its Metal program (vertex and pixel shader, or generated fixed-function stages); the parameters go to the
 * renderer as float4 registers when a pass begins and on CommitChanges.
 * Handles are addresses inside a block of guest memory reserved per effect (so they never collide with the name
 * strings D3DX also accepts in their place). D3DX9_LOG=1 (the D3D9_LOG switch) logs each method's first calls. */
#include "d3d9_priv.h"
#include "hlsl.h"
#include <strings.h>

enum { O9_FX = 31, O9_XBUF = 32 };
#define E_FAIL_ 0x80004005u
#define D3DXERR_INVALIDDATA 0x88760b59u
#define HB_SIZE 0x40000u
enum { HK_NONE, HK_PARAM, HK_TECH, HK_PASS, HK_FUNC, HK_ANNO, HK_ELEM };

typedef struct Anno { int owner_kind, owner, idx; HAnno *a; uint32_t gname, gstr; } Anno;
typedef struct Fx {
    uint32_t g; int ref; Dev9 *dev; HFx *fx;
    uint32_t hb;                                        /* handle block */
    int np, *pv;                                        /* parameters: their globals */
    int *pidx;                                          /* per global: parameter index or -1 */
    float *regs; int nregs, *preg;                      /* parameter registers */
    uint32_t *objv;                                     /* per global: texture interface / string */
    uint32_t *gname, *gsem;                             /* per global: guest name strings */
    Anno *an; int nan;
    int npasstot, *pbase;                               /* passes, flattened; first of each technique */
    uint32_t *prog; HProgram **hp;                      /* per flattened pass: program id (0 none, ~0 failed) */
    uint32_t *vsfn, *psfn, *pname, *tname;              /* fake shader functions and names (guest) */
    int tech, begun, cur_pass, dumped; uint32_t flags; void *saved;
    uint32_t snap[8];                                   /* device textures when the pass began (TEXTURESTAGEn) */
} Fx;

/* ---- guest strings and buffers ---- */
static uint32_t gstr(const char *s) { size_t n = strlen(s) + 1; uint32_t p = heap_alloc(w32_process_heap, 8, (uint32_t)n); memcpy(GP(p), s, n); return p; }

/* ID3DXBuffer */
typedef struct XBuf { uint32_t g; int ref; uint32_t mem, len; } XBuf;
static uint32_t xbuf_new(const void *data, uint32_t len)
{
    XBuf *b = calloc(1, sizeof *b); b->ref = 1; b->len = len; b->mem = heap_alloc(w32_process_heap, 8, len ? len : 4); if (data) memcpy(GP(b->mem), data, len);
    b->g = make_iface(O9_XBUF, b); return b->g;
}
M(xb_QueryInterface, 3) ME(XBuf, O9_XBUF) rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(xb_AddRef, 1) ME(XBuf, O9_XBUF) RET((uint32_t)++me->ref, 1); END
M(xb_Release, 1) ME(XBuf, O9_XBUF) uint32_t r = (uint32_t)--me->ref; if (!r) { heap_free(w32_process_heap, me->mem); obj_drop(me->g); free(me); } RET(r, 1); END
M(xb_GetBufferPointer, 1) ME(XBuf, O9_XBUF) RET(me->mem, 1); END
M(xb_GetBufferSize, 1) ME(XBuf, O9_XBUF) RET(me->len, 1); END

/* ---- handles ---- */
static uint32_t H(Fx *f, int kind, int i)
{
    switch (kind) {
    case HK_PARAM: return f->hb + 4u * (uint32_t)i;
    case HK_TECH: return f->hb + 0x4000u + 4u * (uint32_t)i;
    case HK_PASS: return f->hb + 0x8000u + 4u * (uint32_t)i;
    case HK_FUNC: return f->hb + 0xc000u + 4u * (uint32_t)i;
    case HK_ANNO: return f->hb + 0x10000u + 4u * (uint32_t)i;
    case HK_ELEM: return f->hb + 0x20000u + 4u * (uint32_t)i;      /* i = param * 64 + element */
    }
    return 0;
}
static int param_by_name(Fx *f, const char *n)
{
    for (int i = 0; i < f->np; i++) if (!strcmp(f->fx->v[f->pv[i]].name, n)) return i;
    return -1;
}
/* a handle (or a name) -> kind and index */
static int decode(Fx *f, uint32_t h, int *idx)
{
    if (!h) return HK_NONE;
    if (h >= f->hb && h < f->hb + HB_SIZE) {
        uint32_t o = h - f->hb;
        if (o < 0x4000) { *idx = (int)(o / 4); return *idx < f->np ? HK_PARAM : HK_NONE; }
        if (o < 0x8000) { *idx = (int)((o - 0x4000) / 4); return *idx < f->fx->nt ? HK_TECH : HK_NONE; }
        if (o < 0xc000) { *idx = (int)((o - 0x8000) / 4); return *idx < f->npasstot ? HK_PASS : HK_NONE; }
        if (o < 0x10000) { *idx = (int)((o - 0xc000) / 4); return *idx < f->fx->nf ? HK_FUNC : HK_NONE; }
        if (o < 0x20000) { *idx = (int)((o - 0x10000) / 4); return *idx < f->nan ? HK_ANNO : HK_NONE; }
        *idx = (int)((o - 0x20000) / 4); return HK_ELEM;
    }
    char n[128]; snprintf(n, sizeof n, "%s", GS(h));                /* a name: "param", "param[3]" */
    char *br = strchr(n, '[');
    if (br) { int e = atoi(br + 1); *br = 0; int p = param_by_name(f, n); if (p < 0 || e < 0 || e >= 64) return HK_NONE; *idx = p * 64 + e; return HK_ELEM; }
    int p = param_by_name(f, n); if (p >= 0) { *idx = p; return HK_PARAM; }
    for (int t = 0; t < f->fx->nt; t++) if (!strcmp(f->fx->t[t].name, n)) { *idx = t; return HK_TECH; }
    return HK_NONE;
}
/* the global, element and element count a parameter handle refers to */
static int param_target(Fx *f, uint32_t h, int *gi, int *elem, int *nelem)
{
    int i, k = decode(f, h, &i);
    if (k == HK_PARAM) { *gi = f->pv[i]; *elem = 0; *nelem = f->fx->v[*gi].t.arr ? f->fx->v[*gi].t.arr : 1; return 1; }
    if (k == HK_ELEM) { int p = i / 64; if (p >= f->np) return 0; *gi = f->pv[p]; *elem = i % 64; if (*elem >= (f->fx->v[*gi].t.arr ? f->fx->v[*gi].t.arr : 1)) return 0; *nelem = 1; return 1; }
    return 0;
}

/* ---- values: flattened (element-major, row-major) floats <-> registers ---- */
static int get_floats(Fx *f, int gi, int elem, float *v, int n);
static void put_floats(Fx *f, int gi, int elem, const float *v, int n)
{
    HType t = f->fx->v[gi].t; t.arr = 0; if (f->preg[gi] < 0) return;
    int rows = t.mat ? t.rows : 1, cols = t.mat ? t.cols : t.cols, per = rows * cols, rpe = hlsl_type_regs(f->fx, t);
    static int flog = -1; if (flog < 0) flog = getenv("FX_FOGLOG") != NULL;
    if (flog && !strncasecmp(f->fx->v[gi].semantic, "FOG", 3)) { float o[1]; get_floats(f, gi, elem, o, 1); if (o[0] != v[0]) fprintf(stderr, "d3dx9: %p %s = %g\n", (void *)f, f->fx->v[gi].name, v[0]); }
    static int plog = -1; if (plog < 0) plog = getenv("FX_PARAMLOG") != NULL;
    if (plog) { static char seen[512][48]; static int ns; char key[48]; int nz = 0; for (int k = 0; k < n; k++) if (v[k] != 0) nz = 1;
        snprintf(key, sizeof key, "%p%s%d", (void *)f, f->fx->v[gi].name, nz); int dup = 0;
        for (int i = 0; i < ns; i++) if (!strcmp(seen[i], key)) dup = 1;
        if (!dup && ns < 512) { snprintf(seen[ns++], 48, "%s", key);
            fprintf(stderr, "d3dx9: set %s (%s) [%d]:", f->fx->v[gi].name, f->fx->v[gi].semantic, n); for (int k = 0; k < n && k < 16; k++) fprintf(stderr, " %g", v[k]); fprintf(stderr, "\n"); } }
    for (int k = 0; k < n; k++) {
        int e = elem + k / per, w = k % per, r = w / cols, c = w % cols;
        int reg = f->preg[gi] + e * rpe + r; if (reg >= f->nregs) return;
        f->regs[reg * 4 + c] = v[k];
    }
}
static int get_floats(Fx *f, int gi, int elem, float *v, int n)
{
    HType t = f->fx->v[gi].t; t.arr = 0; if (f->preg[gi] < 0) return 0;
    int rows = t.mat ? t.rows : 1, cols = t.cols, per = rows * cols, rpe = hlsl_type_regs(f->fx, t);
    for (int k = 0; k < n; k++) {
        int e = elem + k / per, w = k % per, r = w / cols, c = w % cols, reg = f->preg[gi] + e * rpe + r;
        v[k] = reg < f->nregs ? f->regs[reg * 4 + c] : 0;
    }
    return n;
}
static int floats_of(Fx *f, int gi, int nelem) { HType t = f->fx->v[gi].t; t.arr = 0; return hlsl_type_floats(f->fx, t) * nelem; }

/* ---- the effect ---- */
static Fx *fx_of(Ctx *c) { return obj(ARG(0), O9_FX); }
#define FX_ME Fx *me = fx_of(c); if (!me) { fprintf(stderr, "d3dx9: bad effect %08x in %s\n", ARG(0), __func__); RET(D3DERR_INVALIDCALL, nargs_); }
static void fx_free(Fx *f)
{
    for (int i = 0; i < f->fx->nv; i++) if (f->fx->v[i].t.base == HB_TEXTURE) d9_tex_release(f->objv[i]);
    for (int k = 0; k < f->npasstot; k++) hlsl_program_free(f->hp[k]);
    hlsl_free(f->fx); free(f->pv); free(f->pidx); free(f->regs); free(f->preg); free(f->objv); free(f->gname); free(f->gsem); free(f->an);
    free(f->pbase); free(f->prog); free(f->hp); free(f->vsfn); free(f->psfn); free(f->pname); free(f->tname);
    obj_drop(f->g); free(f);
}
M(fx_QueryInterface, 3) FX_ME rt_w32(G_MEM, ARG(2), ARG(0)); me->ref++; RET(D3D_OK, 3); END
M(fx_AddRef, 1) FX_ME RET((uint32_t)++me->ref, 1); END
M(fx_Release, 1) FX_ME uint32_t r = (uint32_t)--me->ref; if (!r) fx_free(me); RET(r, 1); END
M(fx_GetDesc, 2) FX_ME uint32_t v[4] = {0, (uint32_t)me->np, (uint32_t)me->fx->nt, (uint32_t)me->fx->nf}; memcpy(GP(ARG(1)), v, 16); RET(D3D_OK, 2); END
static uint32_t anno_count(Fx *f, int kind, int owner) { uint32_t n = 0; for (int a = 0; a < f->nan; a++) if (f->an[a].owner_kind == kind && f->an[a].owner == owner) n++; return n; }
/* D3DXPARAMETER_DESC {Name, Semantic, Class, Type, Rows, Columns, Elements, Annotations, StructMembers, Flags, Bytes} */
static void param_desc(Fx *f, int gi, int elem_handle, uint32_t out)
{
    HVar *v = &f->fx->v[gi]; HType t = v->t; uint32_t cls, typ, rows = 1, cols = 1;
    switch (t.base) {
    case HB_BOOL: typ = 1; break; case HB_INT: typ = 2; break; case HB_FLOAT: typ = 3; break; case HB_STRING: typ = 4; break;
    case HB_TEXTURE: typ = 5; break; case HB_SAMPLER: typ = t.samp == HS_2D ? 12 : t.samp == HS_3D ? 13 : t.samp == HS_CUBE ? 14 : 10; break;
    default: typ = 0;
    }
    if (t.base == HB_STRUCT) cls = 5; else if (t.base >= HB_SAMPLER) cls = 4; else if (t.mat) { cls = 3; rows = t.rows; cols = t.cols; } else if (t.cols > 1) { cls = 1; cols = t.cols; } else cls = 0;
    uint32_t elems = elem_handle ? 0 : (uint32_t)t.arr, bytes = t.base >= HB_SAMPLER ? 4 : 4u * (uint32_t)floats_of(f, gi, elem_handle ? 1 : (t.arr ? t.arr : 1));
    uint32_t d[11] = {f->gname[gi], v->semantic[0] ? f->gsem[gi] : 0, cls, typ, rows, cols, elems, anno_count(f, HK_PARAM, f->pidx[gi]), 0, 0, bytes};
    memcpy(GP(out), d, 44);
}
M(fx_GetParameterDesc, 3) FX_ME
    int i, k = decode(me, ARG(1), &i);
    if (k == HK_ANNO) {
        Anno *a = &me->an[i]; uint32_t typ = a->a->str[0] ? 4 : !strcasecmp(a->a->type, "int") ? 2 : !strcasecmp(a->a->type, "bool") ? 1 : 3;
        uint32_t d[11] = {a->gname, 0, typ == 4 ? 4u : 0u, typ, 1, 1, 0, 0, 0, 0, 4}; memcpy(GP(ARG(2)), d, 44); RET(D3D_OK, 3);
    }
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne)) RET(D3DERR_INVALIDCALL, 3);
    param_desc(me, gi, k == HK_ELEM, ARG(2)); RET(D3D_OK, 3);
END
M(fx_GetTechniqueDesc, 3) FX_ME
    int i; if (decode(me, ARG(1), &i) != HK_TECH) RET(D3DERR_INVALIDCALL, 3);
    uint32_t d[3] = {me->tname[i], (uint32_t)me->fx->t[i].npass, anno_count(me, HK_TECH, i)}; memcpy(GP(ARG(2)), d, 12); RET(D3D_OK, 3);
END
/* a vertex shader "function": the version token, a dcl for each input semantic, the end token (what
 * D3DXGetShaderInputSemantics reads back) */
static uint32_t fake_function(Fx *f, int k, int vs)
{
    int t = 0; while (t + 1 < f->fx->nt && f->pbase[t + 1] <= k) t++;
    HPass *ps = &f->fx->t[t].pass[k - f->pbase[t]]; HState *s = NULL;
    for (int i = 0; i < ps->nst; i++) if (!strcasecmp(ps->st[i].name, vs ? "VertexShader" : "PixelShader") && ps->st[i].fn[0]) s = &ps->st[i];
    if (!s) return 0;
    uint32_t tok[200]; int n = 0; tok[n++] = vs ? 0xfffe0200u : 0xffff0200u;
    int fi = -1; for (int i = 0; i < f->fx->nf; i++) if (!strcmp(f->fx->f[i].name, s->fn)) fi = i;
    if (vs && fi >= 0) {
        static const struct { const char *n; int u; } us[] = {{"POSITION", 0}, {"BLENDWEIGHT", 1}, {"BLENDINDICES", 2}, {"NORMAL", 3}, {"PSIZE", 4}, {"TEXCOORD", 5},
                                                             {"TANGENT", 6}, {"BINORMAL", 7}, {"POSITIONT", 9}, {"COLOR", 10}, {"FOG", 11}, {0, 0}};
        HFunc *fn = &f->fx->f[fi]; int reg = 0;
        for (int i = 0; i < fn->np && n < 190; i++) {
            HParam *p = &fn->p[i]; if (p->is_uniform || !(p->dir & 1)) continue;
            int nm = p->t.base == HB_STRUCT ? f->fx->s[p->t.st].nf : 1;
            for (int m = 0; m < nm && n < 190; m++) {
                const char *sem = p->t.base == HB_STRUCT ? f->fx->s[p->t.st].f[m].semantic : p->semantic;
                char nb[48]; size_t l = strlen(sem), q = l; while (q && sem[q - 1] >= '0' && sem[q - 1] <= '9') q--;
                snprintf(nb, sizeof nb, "%.*s", (int)q, sem); for (char *c = nb; *c; c++) if (*c >= 'a' && *c <= 'z') *c -= 32;
                int u = -1; for (int z = 0; us[z].n; z++) if (!strcmp(us[z].n, nb)) u = us[z].u;
                if (u < 0) continue;
                uint32_t ui = q < l ? (uint32_t)atoi(sem + q) : 0;
                tok[n++] = 0x0200001fu;                                   /* dcl (length 2) */
                tok[n++] = 0x80000000u | (uint32_t)u | (ui << 16);
                tok[n++] = 0x80000000u | (1u << 28) | 0xf0000u | (uint32_t)reg++;   /* v<reg>, input register */
            }
        }
    }
    tok[n++] = 0x0000ffffu;
    uint32_t g = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n); memcpy(GP(g), tok, 4u * (size_t)n); return g;
}
M(fx_GetPassDesc, 3) FX_ME
    int k; if (decode(me, ARG(1), &k) != HK_PASS) RET(D3DERR_INVALIDCALL, 3);
    if (!me->vsfn[k]) me->vsfn[k] = fake_function(me, k, 1) | 1u;      /* | 1: built (an address is 8-aligned) */
    if (!me->psfn[k]) me->psfn[k] = fake_function(me, k, 0) | 1u;
    uint32_t d[4] = {me->pname[k], anno_count(me, HK_PASS, k), me->vsfn[k] & ~1u, me->psfn[k] & ~1u}; memcpy(GP(ARG(2)), d, 16); RET(D3D_OK, 3);
END
M(fx_GetFunctionDesc, 3) FX_ME
    int i; if (decode(me, ARG(1), &i) != HK_FUNC) RET(D3DERR_INVALIDCALL, 3);
    uint32_t d[2] = {gstr(me->fx->f[i].name), 0}; memcpy(GP(ARG(2)), d, 8); RET(D3D_OK, 3);
END
M(fx_GetParameter, 3) FX_ME RET(!ARG(1) && ARG(2) < (uint32_t)me->np ? H(me, HK_PARAM, (int)ARG(2)) : 0, 3); END
M(fx_GetParameterByName, 3) FX_ME
    if (ARG(1) || !ARG(2)) RET(0, 3);
    int i, k = decode(me, ARG(2), &i); RET(k == HK_PARAM ? H(me, HK_PARAM, i) : k == HK_ELEM ? H(me, HK_ELEM, i) : 0, 3);
END
M(fx_GetParameterBySemantic, 3) FX_ME
    if (ARG(1) || !ARG(2)) RET(0, 3);
    for (int i = 0; i < me->np; i++) if (!strcasecmp(me->fx->v[me->pv[i]].semantic, GS(ARG(2)))) RET(H(me, HK_PARAM, i), 3);
    RET(0, 3);
END
M(fx_GetParameterElement, 3) FX_ME
    int i; if (decode(me, ARG(1), &i) != HK_PARAM) RET(0, 3);
    int n = me->fx->v[me->pv[i]].t.arr; RET(ARG(2) < (uint32_t)n && ARG(2) < 64 ? H(me, HK_ELEM, i * 64 + (int)ARG(2)) : 0, 3);
END
M(fx_GetTechnique, 2) FX_ME RET(ARG(1) < (uint32_t)me->fx->nt ? H(me, HK_TECH, (int)ARG(1)) : 0, 2); END
M(fx_GetTechniqueByName, 2) FX_ME
    if (!ARG(1)) RET(0, 2);
    for (int t = 0; t < me->fx->nt; t++) if (!strcmp(me->fx->t[t].name, GS(ARG(1)))) RET(H(me, HK_TECH, t), 2);
    RET(0, 2);
END
M(fx_GetPass, 3) FX_ME int t; if (decode(me, ARG(1), &t) != HK_TECH || ARG(2) >= (uint32_t)me->fx->t[t].npass) RET(0, 3); RET(H(me, HK_PASS, me->pbase[t] + (int)ARG(2)), 3); END
M(fx_GetPassByName, 3) FX_ME
    int t; if (decode(me, ARG(1), &t) != HK_TECH || !ARG(2)) RET(0, 3);
    for (int p = 0; p < me->fx->t[t].npass; p++) if (!strcmp(me->fx->t[t].pass[p].name, GS(ARG(2)))) RET(H(me, HK_PASS, me->pbase[t] + p), 3);
    RET(0, 3);
END
M(fx_GetFunction, 2) FX_ME RET(ARG(1) < (uint32_t)me->fx->nf ? H(me, HK_FUNC, (int)ARG(1)) : 0, 2); END
M(fx_GetFunctionByName, 2) FX_ME
    if (!ARG(1)) RET(0, 2);
    for (int i = 0; i < me->fx->nf; i++) if (!strcmp(me->fx->f[i].name, GS(ARG(1)))) RET(H(me, HK_FUNC, i), 2);
    RET(0, 2);
END
static int owner_of(Fx *f, uint32_t h, int *kind, int *owner)
{
    int i, k = decode(f, h, &i);
    if (k == HK_PARAM || k == HK_TECH || k == HK_PASS || k == HK_FUNC) { *kind = k; *owner = i; return 1; }
    return 0;
}
M(fx_GetAnnotation, 3) FX_ME
    int kind, owner; if (!owner_of(me, ARG(1), &kind, &owner)) RET(0, 3);
    uint32_t n = 0; for (int a = 0; a < me->nan; a++) if (me->an[a].owner_kind == kind && me->an[a].owner == owner && n++ == ARG(2)) RET(H(me, HK_ANNO, a), 3);
    RET(0, 3);
END
M(fx_GetAnnotationByName, 3) FX_ME
    int kind, owner; if (!owner_of(me, ARG(1), &kind, &owner) || !ARG(2)) RET(0, 3);
    for (int a = 0; a < me->nan; a++) if (me->an[a].owner_kind == kind && me->an[a].owner == owner && !strcmp(me->an[a].a->name, GS(ARG(2)))) RET(H(me, HK_ANNO, a), 3);
    RET(0, 3);
END
/* setters and getters: (handle, data) */
static uint32_t set_typed(Fx *f, uint32_t h, const void *data, uint32_t count, int kind)   /* kind: 0 float, 1 int, 2 bool */
{
    int gi, el, ne; if (!param_target(f, h, &gi, &el, &ne)) return D3DERR_INVALIDCALL;
    HVar *v = &f->fx->v[gi]; if (v->t.base > HB_FLOAT) return D3DERR_INVALIDCALL;
    int max = floats_of(f, gi, ne); if ((int)count > max) count = (uint32_t)max;
    float tmp[1024]; if (count > 1024) count = 1024;
    for (uint32_t k = 0; k < count; k++) {
        if (kind == 0) memcpy(&tmp[k], (const uint8_t *)data + 4 * k, 4);
        else { int32_t x; memcpy(&x, (const uint8_t *)data + 4 * k, 4); tmp[k] = kind == 2 ? (x != 0) : (float)x; }
    }
    put_floats(f, gi, el, tmp, (int)count); return D3D_OK;
}
static uint32_t get_typed(Fx *f, uint32_t h, void *data, uint32_t count, int kind)
{
    int i, k0 = decode(f, h, &i);
    if (k0 == HK_ANNO) {                                   /* annotations: their number */
        float v = (float)f->an[i].a->num; if (!count) return D3D_OK;
        if (kind == 0) memcpy(data, &v, 4); else { int32_t x = kind == 2 ? v != 0 : (int32_t)v; memcpy(data, &x, 4); }
        return D3D_OK;
    }
    int gi, el, ne; if (!param_target(f, h, &gi, &el, &ne)) return D3DERR_INVALIDCALL;
    int max = floats_of(f, gi, ne); if ((int)count > max) count = (uint32_t)max;
    float tmp[1024]; if (count > 1024) count = 1024;
    get_floats(f, gi, el, tmp, (int)count);
    for (uint32_t k = 0; k < count; k++) {
        if (kind == 0) memcpy((uint8_t *)data + 4 * k, &tmp[k], 4);
        else { int32_t x = kind == 2 ? tmp[k] != 0 : (int32_t)tmp[k]; memcpy((uint8_t *)data + 4 * k, &x, 4); }
    }
    return D3D_OK;
}
M(fx_SetValue, 4) FX_ME
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne)) RET(D3DERR_INVALIDCALL, 4);
    HVar *v = &me->fx->v[gi];
    if (v->t.base == HB_TEXTURE || v->t.base == HB_STRING) { me->objv[gi] = ARG(3) >= 4 ? rt_r32(G_MEM, ARG(2)) : 0; RET(D3D_OK, 4); }
    RET(set_typed(me, ARG(1), GP(ARG(2)), ARG(3) / 4, v->t.base == HB_FLOAT ? 0 : v->t.base == HB_INT ? 1 : 2), 4);
END
M(fx_GetValue, 4) FX_ME
    int i, k = decode(me, ARG(1), &i);
    if (k == HK_ANNO) { Anno *a = &me->an[i]; if (a->a->str[0]) { if (!a->gstr) a->gstr = gstr(a->a->str); if (ARG(3) >= 4) rt_w32(G_MEM, ARG(2), a->gstr); RET(D3D_OK, 4); } RET(get_typed(me, ARG(1), GP(ARG(2)), ARG(3) / 4, !strcasecmp(a->a->type, "int") ? 1 : 0), 4); }
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne)) RET(D3DERR_INVALIDCALL, 4);
    HVar *v = &me->fx->v[gi];
    if (v->t.base == HB_TEXTURE || v->t.base == HB_STRING) { if (ARG(3) >= 4) rt_w32(G_MEM, ARG(2), me->objv[gi]); RET(D3D_OK, 4); }
    RET(get_typed(me, ARG(1), GP(ARG(2)), ARG(3) / 4, v->t.base == HB_FLOAT ? 0 : v->t.base == HB_INT ? 1 : 2), 4);
END
M(fx_SetBool, 3) FX_ME uint32_t b = ARG(2); RET(set_typed(me, ARG(1), &b, 1, 2), 3); END
M(fx_GetBool, 3) FX_ME RET(get_typed(me, ARG(1), GP(ARG(2)), 1, 2), 3); END
M(fx_SetBoolArray, 4) FX_ME RET(set_typed(me, ARG(1), GP(ARG(2)), ARG(3), 2), 4); END
M(fx_GetBoolArray, 4) FX_ME RET(get_typed(me, ARG(1), GP(ARG(2)), ARG(3), 2), 4); END
M(fx_SetInt, 3) FX_ME uint32_t b = ARG(2); RET(set_typed(me, ARG(1), &b, 1, 1), 3); END
M(fx_GetInt, 3) FX_ME RET(get_typed(me, ARG(1), GP(ARG(2)), 1, 1), 3); END
M(fx_SetIntArray, 4) FX_ME RET(set_typed(me, ARG(1), GP(ARG(2)), ARG(3), 1), 4); END
M(fx_GetIntArray, 4) FX_ME RET(get_typed(me, ARG(1), GP(ARG(2)), ARG(3), 1), 4); END
M(fx_SetFloat, 3) FX_ME uint32_t b = ARG(2); RET(set_typed(me, ARG(1), &b, 1, 0), 3); END
M(fx_GetFloat, 3) FX_ME RET(get_typed(me, ARG(1), GP(ARG(2)), 1, 0), 3); END
M(fx_SetFloatArray, 4) FX_ME RET(set_typed(me, ARG(1), GP(ARG(2)), ARG(3), 0), 4); END
M(fx_GetFloatArray, 4) FX_ME RET(get_typed(me, ARG(1), GP(ARG(2)), ARG(3), 0), 4); END
static uint32_t set_vectors(Fx *f, uint32_t h, uint32_t src, uint32_t count)   /* D3DXVECTOR4s: as many components as each element has */
{
    int gi, el, ne; if (!param_target(f, h, &gi, &el, &ne)) return D3DERR_INVALIDCALL;
    HType t = f->fx->v[gi].t; t.arr = 0; int per = hlsl_type_floats(f->fx, t); if (per > 4) per = 4;
    for (uint32_t e = 0; e < count && (int)e < ne; e++) { float v[4]; memcpy(v, GP(src + 16 * e), 16); put_floats(f, gi, el + (int)e, v, per); }
    return D3D_OK;
}
static uint32_t get_vectors(Fx *f, uint32_t h, uint32_t dst, uint32_t count)
{
    int gi, el, ne; if (!param_target(f, h, &gi, &el, &ne)) return D3DERR_INVALIDCALL;
    HType t = f->fx->v[gi].t; t.arr = 0; int per = hlsl_type_floats(f->fx, t); if (per > 4) per = 4;
    for (uint32_t e = 0; e < count && (int)e < ne; e++) { float v[4] = {0, 0, 0, 0}; get_floats(f, gi, el + (int)e, v, per); memcpy(GP(dst + 16 * e), v, 16); }
    return D3D_OK;
}
M(fx_SetVector, 3) FX_ME RET(set_vectors(me, ARG(1), ARG(2), 1), 3); END
M(fx_GetVector, 3) FX_ME RET(get_vectors(me, ARG(1), ARG(2), 1), 3); END
M(fx_SetVectorArray, 4) FX_ME RET(set_vectors(me, ARG(1), ARG(2), ARG(3)), 4); END
M(fx_GetVectorArray, 4) FX_ME RET(get_vectors(me, ARG(1), ARG(2), ARG(3)), 4); END
/* D3DXMATRIX (row-major 4x4) into an RxC parameter element: its upper-left RxC part */
static uint32_t set_matrix(Fx *f, uint32_t h, int elem_off, const float *m, int transpose)
{
    int gi, el, ne; if (!param_target(f, h, &gi, &el, &ne) || elem_off >= ne) return D3DERR_INVALIDCALL;
    HType t = f->fx->v[gi].t; t.arr = 0; int rows = t.mat ? t.rows : 1, cols = t.cols; float v[16]; int n = 0;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) v[n++] = transpose ? m[c * 4 + r] : m[r * 4 + c];
    put_floats(f, gi, el + elem_off, v, n); return D3D_OK;
}
static uint32_t get_matrix(Fx *f, uint32_t h, int elem_off, float *m, int transpose)
{
    int gi, el, ne; if (!param_target(f, h, &gi, &el, &ne) || elem_off >= ne) return D3DERR_INVALIDCALL;
    HType t = f->fx->v[gi].t; t.arr = 0; int rows = t.mat ? t.rows : 1, cols = t.cols; float v[16] = {0};
    get_floats(f, gi, el + elem_off, v, rows * cols);
    for (int k = 0; k < 16; k++) m[k] = (k % 5) ? 0 : 1;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++) { if (transpose) m[c * 4 + r] = v[r * cols + c]; else m[r * 4 + c] = v[r * cols + c]; }
    return D3D_OK;
}
#define MATRIX_SET(name, nargs, tr, ptrs) M(name, nargs) FX_ME \
    uint32_t n = nargs == 3 ? 1 : ARG(3), r = D3D_OK; \
    for (uint32_t e = 0; e < n && r == D3D_OK; e++) { float m[16]; uint32_t src = ptrs ? rt_r32(G_MEM, ARG(2) + 4 * e) : ARG(2) + 64 * e; memcpy(m, GP(src), 64); r = set_matrix(me, ARG(1), (int)e, m, tr); } \
    RET(r, nargs); END
#define MATRIX_GET(name, nargs, tr, ptrs) M(name, nargs) FX_ME \
    uint32_t n = nargs == 3 ? 1 : ARG(3), r = D3D_OK; \
    for (uint32_t e = 0; e < n && r == D3D_OK; e++) { float m[16]; r = get_matrix(me, ARG(1), (int)e, m, tr); uint32_t dst = ptrs ? rt_r32(G_MEM, ARG(2) + 4 * e) : ARG(2) + 64 * e; if (r == D3D_OK) memcpy(GP(dst), m, 64); } \
    RET(r, nargs); END
MATRIX_SET(fx_SetMatrix, 3, 0, 0) MATRIX_GET(fx_GetMatrix, 3, 0, 0) MATRIX_SET(fx_SetMatrixArray, 4, 0, 0) MATRIX_GET(fx_GetMatrixArray, 4, 0, 0)
MATRIX_SET(fx_SetMatrixPointerArray, 4, 0, 1) MATRIX_GET(fx_GetMatrixPointerArray, 4, 0, 1)
MATRIX_SET(fx_SetMatrixTranspose, 3, 1, 0) MATRIX_GET(fx_GetMatrixTranspose, 3, 1, 0) MATRIX_SET(fx_SetMatrixTransposeArray, 4, 1, 0) MATRIX_GET(fx_GetMatrixTransposeArray, 4, 1, 0)
MATRIX_SET(fx_SetMatrixTransposePointerArray, 4, 1, 1) MATRIX_GET(fx_GetMatrixTransposePointerArray, 4, 1, 1)
M(fx_SetString, 3) FX_ME int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne)) RET(D3DERR_INVALIDCALL, 3); me->objv[gi] = ARG(2) ? gstr(GS(ARG(2))) : 0; RET(D3D_OK, 3); END
M(fx_GetString, 3) FX_ME
    int i, k = decode(me, ARG(1), &i);
    if (k == HK_ANNO) { Anno *a = &me->an[i]; if (!a->gstr) a->gstr = gstr(a->a->str); rt_w32(G_MEM, ARG(2), a->gstr); RET(D3D_OK, 3); }
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne)) RET(D3DERR_INVALIDCALL, 3);
    HVar *v = &me->fx->v[gi];
    if (v->t.base != HB_STRING) RET(D3DERR_INVALIDCALL, 3);
    if (!me->objv[gi]) me->objv[gi] = gstr("");
    rt_w32(G_MEM, ARG(2), me->objv[gi]); RET(D3D_OK, 3);
END
M(fx_SetTexture, 3) FX_ME
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne) || me->fx->v[gi].t.base != HB_TEXTURE) {
        if (logon()) fprintf(stderr, "d3dx9: SetTexture: parameter %08x is not a texture\n", ARG(1));
        RET(D3DERR_INVALIDCALL, 3);
    }
    if (logon() && me->objv[gi] != ARG(2)) fprintf(stderr, "d3dx9: SetTexture %s = %08x\n", me->fx->v[gi].name, ARG(2));
    d9_tex_addref(ARG(2)); d9_tex_release(me->objv[gi]);                 /* the effect holds a reference (as D3DX's) */
    me->objv[gi] = ARG(2); RET(D3D_OK, 3);
END
M(fx_GetTexture, 3) FX_ME
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne) || me->fx->v[gi].t.base != HB_TEXTURE) RET(D3DERR_INVALIDCALL, 3);
    d9_tex_addref(me->objv[gi]); rt_w32(G_MEM, ARG(2), me->objv[gi]); RET(D3D_OK, 3);   /* the caller's reference */
END
M(fx_GetPixelShader, 3) FX_ME rt_w32(G_MEM, ARG(2), 0); RET(D3DERR_INVALIDCALL, 3); END
M(fx_GetVertexShader, 3) FX_ME rt_w32(G_MEM, ARG(2), 0); RET(D3DERR_INVALIDCALL, 3); END
OK_M(fx_SetArrayRange, 4)
M(fx_GetPool, 2) rt_w32(G_MEM, ARG(1), 0); RET(D3D_OK, 2); END

/* ---- techniques and passes ---- */
static int build_pass(Fx *f, int k)                    /* compile a pass's program once; 1 if usable */
{
    if (f->prog[k]) return f->prog[k] != ~0u;
    int t = 0; while (t + 1 < f->fx->nt && f->pbase[t + 1] <= k) t++;
    HPass *ps = &f->fx->t[t].pass[k - f->pbase[t]]; int any = 0;
    for (int i = 0; i < ps->nst; i++) if ((!strcasecmp(ps->st[i].name, "VertexShader") || !strcasecmp(ps->st[i].name, "PixelShader")) && ps->st[i].fn[0]) any = 1;
    if (!any) { f->prog[k] = ~0u - 1; return 1; }          /* fixed function only: no program */
    HProgram *pr = hlsl_compile_pass(f->fx, t, k - f->pbase[t]);
    if (!pr || pr->err[0]) { fprintf(stderr, "d3dx9: technique %s pass %d not built: %s\n", f->fx->t[t].name, k - f->pbase[t], pr ? pr->err : "out of memory"); hlsl_program_free(pr); f->prog[k] = ~0u; return 0; }
    uint32_t kinds = 0; for (int sl = 0; sl < 16; sl++) kinds |= (uint32_t)(pr->skind[sl] == HS_CUBE ? 1 : pr->skind[sl] == HS_3D ? 2 : 0) << (2 * sl);
    f->hp[k] = pr; f->prog[k] = d9_program_create(pr->msl, pr->msl_len, kinds);
    if (logon()) fprintf(stderr, "d3dx9: technique %s pass %d -> program %u (%zu bytes of Metal)\n", f->fx->t[t].name, k - f->pbase[t], f->prog[k], pr->msl_len);
    return f->prog[k] != 0;
}
static int tech_valid(Fx *f, int t) { for (int p = 0; p < f->fx->t[t].npass; p++) if (!build_pass(f, f->pbase[t] + p)) return 0; return 1; }
M(fx_SetTechnique, 2) FX_ME int t, k = decode(me, ARG(1), &t); if (k != HK_TECH) RET(D3DERR_INVALIDCALL, 2); me->tech = t; RET(D3D_OK, 2); END
M(fx_GetCurrentTechnique, 1) FX_ME RET(me->tech >= 0 ? H(me, HK_TECH, me->tech) : 0, 1); END
M(fx_ValidateTechnique, 2) FX_ME int t; if (decode(me, ARG(1), &t) != HK_TECH) RET(D3DERR_INVALIDCALL, 2);
    int ok = tech_valid(me, t); if (logon()) fprintf(stderr, "d3dx9: ValidateTechnique %s: %s\n", me->fx->t[t].name, ok ? "ok" : "fails");
    RET(ok ? D3D_OK : E_FAIL_, 2); END
M(fx_FindNextValidTechnique, 3) FX_ME
    int t = -1; if (ARG(1)) { if (decode(me, ARG(1), &t) != HK_TECH) RET(D3DERR_INVALIDCALL, 3); }
    for (int n = t + 1; n < me->fx->nt; n++) if (tech_valid(me, n)) { rt_w32(G_MEM, ARG(2), H(me, HK_TECH, n)); RET(D3D_OK, 3); }
    rt_w32(G_MEM, ARG(2), 0); RET(1, 3);                                              /* S_FALSE: none left */
END
M(fx_IsParameterUsed, 3) RET(1, 3); END

/* state names of a pass -> device states */
typedef struct SName { const char *n; int v; } SName;
static const SName RSN[] = {{"ZEnable", 7}, {"FillMode", 8}, {"ShadeMode", 9}, {"ZWriteEnable", 14}, {"AlphaTestEnable", 15}, {"LastPixel", 16},
    {"SrcBlend", 19}, {"DestBlend", 20}, {"CullMode", 22}, {"ZFunc", 23}, {"AlphaRef", 24}, {"AlphaFunc", 25}, {"DitherEnable", 26},
    {"AlphaBlendEnable", 27}, {"FogEnable", 28}, {"SpecularEnable", 29}, {"FogColor", 34}, {"FogTableMode", 35}, {"FogStart", 36}, {"FogEnd", 37},
    {"FogDensity", 38}, {"RangeFogEnable", 48}, {"StencilEnable", 52}, {"StencilFail", 53}, {"StencilZFail", 54}, {"StencilPass", 55},
    {"StencilFunc", 56}, {"StencilRef", 57}, {"StencilMask", 58}, {"StencilWriteMask", 59}, {"TextureFactor", 60}, {"Clipping", 136},
    {"Lighting", 137}, {"Ambient", 139}, {"FogVertexMode", 140}, {"ColorVertex", 141}, {"LocalViewer", 142}, {"NormalizeNormals", 143},
    {"DiffuseMaterialSource", 145}, {"SpecularMaterialSource", 146}, {"AmbientMaterialSource", 147}, {"EmissiveMaterialSource", 148},
    {"VertexBlend", 151}, {"ClipPlaneEnable", 152}, {"PointSize", 154}, {"PointSpriteEnable", 156}, {"ColorWriteEnable", 168}, {"BlendOp", 171},
    {"SeparateAlphaBlendEnable", 206}, {"SrcBlendAlpha", 207}, {"DestBlendAlpha", 208}, {"BlendOpAlpha", 209}, {0, 0}};
static const SName TSSN[] = {{"ColorOp", 1}, {"ColorArg1", 2}, {"ColorArg2", 3}, {"AlphaOp", 4}, {"AlphaArg1", 5}, {"AlphaArg2", 6},
    {"BumpEnvMat00", 7}, {"BumpEnvMat01", 8}, {"BumpEnvMat10", 9}, {"BumpEnvMat11", 10}, {"TexCoordIndex", 11}, {"BumpEnvLScale", 22},
    {"BumpEnvLOffset", 23}, {"TextureTransformFlags", 24}, {"ColorArg0", 26}, {"AlphaArg0", 27}, {"ResultArg", 28}, {0, 0}};
static const SName SSN[] = {{"AddressU", 1}, {"AddressV", 2}, {"AddressW", 3}, {"BorderColor", 4}, {"MagFilter", 5}, {"MinFilter", 6},
    {"MipFilter", 7}, {"MipMapLodBias", 8}, {"MaxMipLevel", 9}, {"MaxAnisotropy", 10}, {0, 0}};
static int sname(const SName *t, const char *n) { for (int i = 0; t[i].n; i++) if (!strcasecmp(t[i].n, n)) return t[i].v; return -1; }
/* a named value, by what the state takes */
static int enum_value(const char *state, const char *n, uint32_t *out)
{
    static const SName gen[] = {{"TRUE", 1}, {"FALSE", 0}, {"NULL", 0}, {0, 0}};
    static const SName cull[] = {{"NONE", 1}, {"CW", 2}, {"CCW", 3}, {0, 0}};
    static const SName blend[] = {{"ZERO", 1}, {"ONE", 2}, {"SRCCOLOR", 3}, {"INVSRCCOLOR", 4}, {"SRCALPHA", 5}, {"INVSRCALPHA", 6}, {"DESTALPHA", 7},
        {"INVDESTALPHA", 8}, {"DESTCOLOR", 9}, {"INVDESTCOLOR", 10}, {"SRCALPHASAT", 11}, {"BOTHSRCALPHA", 12}, {"BOTHINVSRCALPHA", 13},
        {"BLENDFACTOR", 14}, {"INVBLENDFACTOR", 15}, {0, 0}};
    static const SName cmp[] = {{"NEVER", 1}, {"LESS", 2}, {"EQUAL", 3}, {"LESSEQUAL", 4}, {"GREATER", 5}, {"NOTEQUAL", 6}, {"GREATEREQUAL", 7}, {"ALWAYS", 8}, {0, 0}};
    static const SName sop[] = {{"KEEP", 1}, {"ZERO", 2}, {"REPLACE", 3}, {"INCRSAT", 4}, {"DECRSAT", 5}, {"INVERT", 6}, {"INCR", 7}, {"DECR", 8}, {0, 0}};
    static const SName fill[] = {{"POINT", 1}, {"WIREFRAME", 2}, {"SOLID", 3}, {0, 0}}, shade[] = {{"FLAT", 1}, {"GOURAUD", 2}, {0, 0}};
    static const SName fog[] = {{"NONE", 0}, {"EXP", 1}, {"EXP2", 2}, {"LINEAR", 3}, {0, 0}};
    static const SName bop[] = {{"ADD", 1}, {"SUBTRACT", 2}, {"REVSUBTRACT", 3}, {"MIN", 4}, {"MAX", 5}, {0, 0}};
    static const SName msrc[] = {{"MATERIAL", 0}, {"COLOR1", 1}, {"COLOR2", 2}, {0, 0}};
    static const SName top[] = {{"DISABLE", 1}, {"SELECTARG1", 2}, {"SELECTARG2", 3}, {"MODULATE", 4}, {"MODULATE2X", 5}, {"MODULATE4X", 6}, {"ADD", 7},
        {"ADDSIGNED", 8}, {"ADDSIGNED2X", 9}, {"SUBTRACT", 10}, {"ADDSMOOTH", 11}, {"BLENDDIFFUSEALPHA", 12}, {"BLENDTEXTUREALPHA", 13},
        {"BLENDFACTORALPHA", 14}, {"BLENDTEXTUREALPHAPM", 15}, {"BLENDCURRENTALPHA", 16}, {"PREMODULATE", 17}, {"MODULATEALPHA_ADDCOLOR", 18},
        {"MODULATECOLOR_ADDALPHA", 19}, {"MODULATEINVALPHA_ADDCOLOR", 20}, {"MODULATEINVCOLOR_ADDALPHA", 21}, {"BUMPENVMAP", 22},
        {"BUMPENVMAPLUMINANCE", 23}, {"DOTPRODUCT3", 24}, {"MULTIPLYADD", 25}, {"LERP", 26}, {0, 0}};
    static const SName targ[] = {{"DIFFUSE", 0}, {"CURRENT", 1}, {"TEXTURE", 2}, {"TFACTOR", 3}, {"SPECULAR", 4}, {"TEMP", 5}, {"CONSTANT", 6}, {0, 0}};
    static const SName ttf[] = {{"DISABLE", 0}, {"COUNT1", 1}, {"COUNT2", 2}, {"COUNT3", 3}, {"COUNT4", 4}, {"PROJECTED", 256}, {0, 0}};
    static const SName addr[] = {{"WRAP", 1}, {"MIRROR", 2}, {"CLAMP", 3}, {"BORDER", 4}, {"MIRRORONCE", 5}, {0, 0}};
    static const SName filt[] = {{"NONE", 0}, {"POINT", 1}, {"LINEAR", 2}, {"ANISOTROPIC", 3}, {"PYRAMIDALQUAD", 6}, {"GAUSSIANQUAD", 7}, {0, 0}};
    const SName *t = NULL;
    if (!strcasecmp(state, "CullMode")) t = cull;
    else if (strcasestr(state, "Blend") && !strcasestr(state, "Op") && !strcasestr(state, "Enable")) t = blend;
    else if (strcasestr(state, "Func")) t = cmp;
    else if (!strncasecmp(state, "Stencil", 7)) t = sop;
    else if (!strcasecmp(state, "FillMode")) t = fill; else if (!strcasecmp(state, "ShadeMode")) t = shade;
    else if (strcasestr(state, "FogTableMode") || strcasestr(state, "FogVertexMode")) t = fog;
    else if (strcasestr(state, "BlendOp")) t = bop;
    else if (strcasestr(state, "MaterialSource")) t = msrc;
    else if (strcasestr(state, "Op")) t = top;
    else if (strcasestr(state, "Arg")) t = targ;
    else if (!strcasecmp(state, "TextureTransformFlags")) t = ttf;
    else if (!strncasecmp(state, "Address", 7)) t = addr;
    else if (strcasestr(state, "Filter")) t = filt;
    int v = t ? sname(t, n) : -1; if (v < 0) v = sname(gen, n);
    if (v < 0) return 0;
    *out = (uint32_t)v; return 1;
}
static int state_value(Fx *f, HState *st, uint32_t *out)
{
    if (st->nargs == -1) return enum_value(st->name, st->ref, out);
    if (st->ref[0]) {                                   /* <param>: its value */
        int p = param_by_name(f, st->ref); if (p < 0) return 0;
        float v[4] = {0}; get_floats(f, f->pv[p], 0, v, 1); *out = (uint32_t)v[0]; return 1;
    }
    if (!st->value) return 0;
    float v[4]; if (hlsl_eval(f->fx, st->value, v, 4) < 1) return 0;
    if (hlsl_expr_is_float(st->value) && v[0] != (float)(uint32_t)v[0]) memcpy(out, &v[0], 4);   /* float states (fog, point size): their bits */
    else *out = (uint32_t)v[0];
    return 1;
}
/* a texture parameter's texture: its value, or for TEXTURESTAGEn parameters the object's own texture on that stage */
static uint32_t texture_of(Fx *f, const char *param)
{
    int p = param_by_name(f, param); if (p < 0) return 0;
    int gi = f->pv[p]; if (f->objv[gi]) return f->objv[gi];
    const char *sem = f->fx->v[gi].semantic;
    if (!strncasecmp(sem, "TEXTURESTAGE", 12)) { int n = atoi(sem + 12); if (n >= 0 && n < 8) return f->snap[n]; }
    return 0;
}
static void apply_sampler(Fx *f, uint32_t stage, HState *list, int n)
{
    for (int i = 0; i < n; i++) {
        HState *st = &list[i]; uint32_t v;
        if (!strcasecmp(st->name, "Texture")) {
            uint32_t tx = st->ref[0] ? texture_of(f, st->ref) : 0;
            static int n = -1; if (n < 0) n = getenv("D3D9_TEXLOG") ? atoi(getenv("D3D9_TEXLOG")) : 0;
            if (n > 0) { n--; fprintf(stderr, "d3dx9: sampler %u: Texture <%s> = %08x\n", stage, st->ref, tx); }
            d9_set_texture(f->dev, stage, tx); continue;
        }
        int ss = sname(SSN, st->name); if (ss >= 0 && state_value(f, st, &v)) d9_set_ss(f->dev, stage, (uint32_t)ss, v);
    }
}
static void apply_pass(Fx *f, int k)
{
    int t = 0; while (t + 1 < f->fx->nt && f->pbase[t + 1] <= k) t++;
    HPass *ps = &f->fx->t[t].pass[k - f->pbase[t]];
    for (uint32_t i = 0; i < 8; i++) f->snap[i] = d9_texture_at(f->dev, i);
    for (int i = 0; i < ps->nst; i++) {
        HState *st = &ps->st[i]; uint32_t v; int n;
        if (!strcasecmp(st->name, "VertexShader") || !strcasecmp(st->name, "PixelShader")) continue;
        if (!strcasecmp(st->name, "Texture")) { d9_set_texture(f->dev, (uint32_t)st->index, st->ref[0] ? texture_of(f, st->ref) : 0); continue; }
        if (!strcasecmp(st->name, "Sampler")) {
            if (st->nsub) apply_sampler(f, (uint32_t)st->index, st->sub, st->nsub);
            else if (st->ref[0]) { int g = -1; for (int z = 0; z < f->fx->nv; z++) if (!strcmp(f->fx->v[z].name, st->ref)) g = z; if (g >= 0) apply_sampler(f, (uint32_t)st->index, f->fx->v[g].ss, f->fx->v[g].nss); }
            continue;
        }
        if ((n = sname(RSN, st->name)) >= 0) { if (state_value(f, st, &v)) d9_set_rs(f->dev, (uint32_t)n, v); continue; }
        if ((n = sname(TSSN, st->name)) >= 0) { if (state_value(f, st, &v)) d9_set_tss(f->dev, (uint32_t)st->index, (uint32_t)n, v); continue; }
        if ((n = sname(SSN, st->name)) >= 0) { if (state_value(f, st, &v)) d9_set_ss(f->dev, (uint32_t)st->index, (uint32_t)n, v); continue; }
        static int warned; if (!warned++ || logon() > 1) fprintf(stderr, "d3dx9: pass state %s is not applied\n", st->name);
    }
    build_pass(f, k);
    HProgram *pr = f->hp[k];
    if (pr && !pr->ps_ffp) for (int s = 0; s < pr->nsamp; s++) {   /* the pixel shader's samplers: texture and states on their slots */
        int g = pr->sslot[s]; if (g < 0) continue;
        apply_sampler(f, (uint32_t)s, f->fx->v[g].ss, f->fx->v[g].nss);
    }
    if (pr) { d9_program_consts(f->dev, f->regs, f->nregs); d9_program_use(f->dev, f->prog[k]); }
    else d9_program_use(f->dev, 0);
}
/* the states a technique's passes set (what apply_pass changes), for Begin to save and End to restore */
static void mask_sampler(D9Mask *m, uint32_t stage, const HState *list, int n)
{
    if (stage >= 16) return;
    for (int i = 0; i < n; i++) {
        if (!strcasecmp(list[i].name, "Texture")) { m->tex[stage] = 1; continue; }
        int ss = sname(SSN, list[i].name); if (ss >= 0 && ss < 14) m->ss[stage][ss] = 1;
    }
}
static void tech_mask(Fx *f, int t, D9Mask *m)
{
    memset(m, 0, sizeof *m); m->prog = 1;
    for (int p = 0; p < f->fx->t[t].npass; p++) {
        int k = f->pbase[t] + p; HPass *ps = &f->fx->t[t].pass[p]; int n;
        for (int i = 0; i < ps->nst; i++) {
            HState *st = &ps->st[i]; uint32_t ix = (uint32_t)st->index;
            if (!strcasecmp(st->name, "VertexShader") || !strcasecmp(st->name, "PixelShader")) continue;
            if (!strcasecmp(st->name, "Texture")) { if (ix < 16) m->tex[ix] = 1; continue; }
            if (!strcasecmp(st->name, "Sampler")) {
                if (st->nsub) mask_sampler(m, ix, st->sub, st->nsub);
                else for (int z = 0; z < f->fx->nv; z++) if (!strcmp(f->fx->v[z].name, st->ref)) mask_sampler(m, ix, f->fx->v[z].ss, f->fx->v[z].nss);
                continue;
            }
            if ((n = sname(RSN, st->name)) >= 0) { if (n < 256) m->rs[n] = 1; continue; }
            if ((n = sname(TSSN, st->name)) >= 0) { if (ix < 8 && n < 33) m->tss[ix][n] = 1; continue; }
            if ((n = sname(SSN, st->name)) >= 0) { if (ix < 16 && n < 14) m->ss[ix][n] = 1; continue; }
        }
        build_pass(f, k);
        HProgram *pr = f->hp[k];
        if (pr && !pr->ps_ffp) for (int sl = 0; sl < pr->nsamp; sl++) { int g = pr->sslot[sl]; if (g >= 0) mask_sampler(m, (uint32_t)sl, f->fx->v[g].ss, f->fx->v[g].nss); }
    }
}
M(fx_Begin, 3) FX_ME
    if (me->tech < 0) RET(D3DERR_INVALIDCALL, 3);
    if (ARG(1)) rt_w32(G_MEM, ARG(1), (uint32_t)me->fx->t[me->tech].npass);
    me->flags = ARG(2); me->begun = 1; me->cur_pass = -1;
    if (logon()) { static char seen[256][64]; static int ns; const char *tn_ = me->fx->t[me->tech].name; int dup = 0;
        for (int i = 0; i < ns; i++) if (!strcmp(seen[i], tn_)) dup = 1;
        if (!dup && ns < 256) { snprintf(seen[ns++], 64, "%s", tn_); fprintf(stderr, "d3dx9: Begin %s (flags %x)\n", tn_, ARG(2)); } }
    if (!(me->flags & 1)) { D9Mask m; tech_mask(me, me->tech, &m); me->saved = d9_state_save(me->dev, &m); }   /* unless D3DXFX_DONOTSAVESTATE */
    RET(D3D_OK, 3);
END
M(fx_BeginPass, 2) FX_ME
    if (!me->begun || me->tech < 0 || ARG(1) >= (uint32_t)me->fx->t[me->tech].npass) RET(D3DERR_INVALIDCALL, 2);
    me->cur_pass = me->pbase[me->tech] + (int)ARG(1);
    static int dump = -1; if (dump < 0) dump = getenv("FX_DUMP") ? atoi(getenv("FX_DUMP")) : 0;
    if (dump && me->dumped < dump && me->dumped++ >= 0) {                  /* development: every parameter at a pass */
        fprintf(stderr, "d3dx9: pass %s.%u:\n", me->fx->t[me->tech].name, ARG(1));
        for (int p = 0; p < me->np; p++) { int gi = me->pv[p]; if (me->preg[gi] < 0) continue; float v[16]; int n = floats_of(me, gi, 1); if (n > 16) n = 16; get_floats(me, gi, 0, v, n);
            fprintf(stderr, "    %s:", me->fx->v[gi].name); for (int k = 0; k < n; k++) fprintf(stderr, " %g", v[k]); fprintf(stderr, "\n"); }
        uint32_t *rs = me->dev->rs; float fs, fe; memcpy(&fs, &rs[36], 4); memcpy(&fe, &rs[37], 4);
        fprintf(stderr, "    fog enable %u table %u vertex %u range %g..%g colour %08x\n", rs[28], rs[35], rs[140], fs, fe, rs[34]);
    }
    apply_pass(me, me->cur_pass); RET(D3D_OK, 2);
END
M(fx_CommitChanges, 1) FX_ME
    if (me->cur_pass >= 0 && me->hp[me->cur_pass]) {
        HProgram *pr = me->hp[me->cur_pass];
        for (uint32_t i = 0; i < 8; i++) me->snap[i] = d9_texture_at(me->dev, i);
        if (!pr->ps_ffp) for (int s = 0; s < pr->nsamp; s++) { int g = pr->sslot[s]; if (g >= 0) apply_sampler(me, (uint32_t)s, me->fx->v[g].ss, me->fx->v[g].nss); }
        d9_program_consts(me->dev, me->regs, me->nregs);
    }
    RET(D3D_OK, 1);
END
M(fx_EndPass, 1) FX_ME me->cur_pass = -1; RET(D3D_OK, 1); END
M(fx_End, 1) FX_ME
    me->begun = 0; me->cur_pass = -1;
    if (me->saved) { d9_state_restore(me->dev, me->saved); me->saved = NULL; }
    else d9_program_use(me->dev, 0);
    RET(D3D_OK, 1);
END
M(fx_GetDevice, 2) FX_ME rt_w32(G_MEM, ARG(1), me->dev->g); me->dev->ref++; RET(D3D_OK, 2); END
OK_M(fx_OnLostDevice, 1)
OK_M(fx_OnResetDevice, 1)
OK_M(fx_SetStateManager, 2)
M(fx_GetStateManager, 2) rt_w32(G_MEM, ARG(1), 0); RET(D3D_OK, 2); END
OK_M(fx_BeginParameterBlock, 1)
M(fx_EndParameterBlock, 1) RET(0, 1); END
OK_M(fx_ApplyParameterBlock, 2)
OK_M(fx_DeleteParameterBlock, 2)
TODO_M(fx_CloneEffect, 3, E_FAIL_)
M(fx_SetRawValue, 5) FX_ME
    int gi, el, ne; if (!param_target(me, ARG(1), &gi, &el, &ne) || me->preg[gi] < 0) RET(D3DERR_INVALIDCALL, 5);
    uint32_t off = ARG(3) / 4, n = ARG(4) / 4, base = (uint32_t)me->preg[gi] * 4;
    for (uint32_t k = 0; k < n && base + off + k < (uint32_t)me->nregs * 4; k++) memcpy(&me->regs[base + off + k], GP(ARG(2) + 4 * k), 4);
    RET(D3D_OK, 5);
END

/* ---- creation ---- */
static Fx *fx_new(Dev9 *dev, HFx *hx)
{
    Fx *f = calloc(1, sizeof *f); f->ref = 1; f->dev = dev; f->fx = hx; f->tech = hx->nt ? 0 : -1; f->cur_pass = -1;
    int nv = hx->nv;
    f->pv = calloc((size_t)nv + 1, sizeof(int)); f->pidx = malloc(sizeof(int) * ((size_t)nv + 1)); f->preg = malloc(sizeof(int) * ((size_t)nv + 1));
    f->objv = calloc((size_t)nv + 1, 4); f->gname = calloc((size_t)nv + 1, 4); f->gsem = calloc((size_t)nv + 1, 4);
    int reg = 0;
    for (int i = 0; i < nv; i++) {
        HVar *v = &hx->v[i]; f->pidx[i] = -1; f->preg[i] = -1;
        if (v->is_static) continue;
        f->pidx[i] = f->np; f->pv[f->np++] = i;
        f->gname[i] = gstr(v->name); if (v->semantic[0]) f->gsem[i] = gstr(v->semantic);
        if (v->t.base <= HB_FLOAT) { f->preg[i] = reg; reg += hlsl_type_regs(hx, v->t); }
        if (v->t.base == HB_STRING && hlsl_expr_string(v->init)) f->objv[i] = gstr(hlsl_expr_string(v->init));
    }
    f->nregs = reg; f->regs = calloc((size_t)reg * 4 + 4, sizeof(float));
    for (int i = 0; i < nv; i++) if (f->preg[i] >= 0 && hx->v[i].defval) put_floats(f, i, 0, hx->v[i].defval, hx->v[i].ndef);   /* defaults */
    /* annotations, flattened */
    int cap = 0;
    #define ADD_ANNOS(K_, O_, ARR_, N_) for (int a_ = 0; a_ < (N_); a_++) { if (f->nan == cap) { cap = cap ? cap * 2 : 32; f->an = realloc(f->an, sizeof *f->an * (size_t)cap); } \
        Anno *x_ = &f->an[f->nan++]; x_->owner_kind = (K_); x_->owner = (O_); x_->idx = a_; x_->a = &(ARR_)[a_]; x_->gname = gstr((ARR_)[a_].name); x_->gstr = 0; }
    for (int i = 0; i < nv; i++) if (f->pidx[i] >= 0) ADD_ANNOS(HK_PARAM, f->pidx[i], hx->v[i].anno, hx->v[i].nanno);
    f->pbase = calloc((size_t)hx->nt + 1, sizeof(int)); f->tname = calloc((size_t)hx->nt + 1, 4);
    for (int t = 0; t < hx->nt; t++) { f->pbase[t] = f->npasstot; f->npasstot += hx->t[t].npass; f->tname[t] = gstr(hx->t[t].name); ADD_ANNOS(HK_TECH, t, hx->t[t].anno, hx->t[t].nanno); }
    f->prog = calloc((size_t)f->npasstot + 1, 4); f->hp = calloc((size_t)f->npasstot + 1, sizeof *f->hp);
    f->vsfn = calloc((size_t)f->npasstot + 1, 4); f->psfn = calloc((size_t)f->npasstot + 1, 4); f->pname = calloc((size_t)f->npasstot + 1, 4);
    for (int t = 0; t < hx->nt; t++) for (int p = 0; p < hx->t[t].npass; p++) { int k = f->pbase[t] + p; f->pname[k] = gstr(hx->t[t].pass[p].name); ADD_ANNOS(HK_PASS, k, hx->t[t].pass[p].anno, hx->t[t].pass[p].nanno); }
    f->hb = heap_alloc(w32_process_heap, 8, HB_SIZE);
    f->g = make_iface(O9_FX, f);
    return f;
}
static void init_fx_vtables(void);
/* D3DXCreateEffect(device, data, length, defines, include, flags, pool, effect *, errors *) */
IMPL(d3dx9_29, D3DXCreateEffect)
{
    init_fx_vtables();
    Dev9 *dev = obj(ARG(0), O9_DEV); uint32_t src = ARG(1), len = ARG(2), defs = ARG(3), out = ARG(7), errs = ARG(8);
    if (errs) rt_w32(G_MEM, errs, 0);
    if (!dev || !src || !out) RET(D3DERR_INVALIDCALL, 9);
    if (!d9_shaders_on()) { if (logon()) fprintf(stderr, "d3dx9: effects off (DS2_SHADERS=0)\n"); RET(E_FAIL_, 9); }
    size_t cap = len + 4096, n = 0; char *text = malloc(cap + 1);
    for (uint32_t d = defs; d && rt_r32(G_MEM, d) && n + 600 < cap; d += 8)      /* D3DXMACRO {Name, Definition}: as #define lines */
        n += (size_t)snprintf(text + n, cap - n, "#define %.200s %.300s\n", GS(rt_r32(G_MEM, d)), rt_r32(G_MEM, d + 4) ? GS(rt_r32(G_MEM, d + 4)) : "");
    memcpy(text + n, GP(src), len); n += len; text[n] = 0;
    HFx *hx = hlsl_parse(text, n); free(text);
    if (!hx || hx->err[0]) {
        char msg[600]; snprintf(msg, sizeof msg, "effect: %s", hx ? hx->err : "out of memory");
        fprintf(stderr, "d3dx9: D3DXCreateEffect: %s\n", msg);
        if (errs) rt_w32(G_MEM, errs, xbuf_new(msg, (uint32_t)strlen(msg) + 1));
        hlsl_free(hx); RET(D3DXERR_INVALIDDATA, 9);
    }
    Fx *f = fx_new(dev, hx);
    if (logon()) fprintf(stderr, "d3dx9: D3DXCreateEffect: %d parameters, %d techniques -> %08x\n", f->np, hx->nt, f->g);
    rt_w32(G_MEM, out, f->g); RET(D3D_OK, 9);
}
/* D3DXGetShaderInputSemantics(function, semantics, count): the dcl instructions of a vertex shader's inputs */
IMPL(d3dx9_29, D3DXGetShaderInputSemantics)
{
    uint32_t fn = ARG(0), out = ARG(1), cnt = ARG(2), n = 0;
    if (!fn) RET(D3DERR_INVALIDCALL, 3);
    for (uint32_t p = fn + 4, guard = 0; guard < 4096; guard++) {
        uint32_t tok = rt_r32(G_MEM, p);
        if (tok == 0x0000ffffu) break;
        uint32_t len = (tok >> 24) & 0xf;
        if ((tok & 0xffff) == 0x1f) {
            uint32_t u = rt_r32(G_MEM, p + 4), d = rt_r32(G_MEM, p + 8), rt = ((d >> 28) & 7) | ((d >> 8) & 0x18);
            if (rt == 1) { if (out) { rt_w32(G_MEM, out + 8 * n, u & 0x1f); rt_w32(G_MEM, out + 8 * n + 4, (u >> 16) & 0xf); } n++; }
        }
        p += 4 * (1 + (len ? len : 2));
    }
    if (cnt) rt_w32(G_MEM, cnt, n);
    RET(D3D_OK, 3);
}

/* ---- vtables (ID3DXEffect of d3dx9_29) ---- */
typedef void (*FnX)(Ctx *);
static void init_fx_vtables(void)
{
    static int done; if (done) return; done = 1;
    static const FnX fx[] = {fx_QueryInterface, fx_AddRef, fx_Release, fx_GetDesc, fx_GetParameterDesc, fx_GetTechniqueDesc, fx_GetPassDesc,
        fx_GetFunctionDesc, fx_GetParameter, fx_GetParameterByName, fx_GetParameterBySemantic, fx_GetParameterElement, fx_GetTechnique,
        fx_GetTechniqueByName, fx_GetPass, fx_GetPassByName, fx_GetFunction, fx_GetFunctionByName, fx_GetAnnotation, fx_GetAnnotationByName,
        fx_SetValue, fx_GetValue, fx_SetBool, fx_GetBool, fx_SetBoolArray, fx_GetBoolArray, fx_SetInt, fx_GetInt, fx_SetIntArray, fx_GetIntArray,
        fx_SetFloat, fx_GetFloat, fx_SetFloatArray, fx_GetFloatArray, fx_SetVector, fx_GetVector, fx_SetVectorArray, fx_GetVectorArray,
        fx_SetMatrix, fx_GetMatrix, fx_SetMatrixArray, fx_GetMatrixArray, fx_SetMatrixPointerArray, fx_GetMatrixPointerArray,
        fx_SetMatrixTranspose, fx_GetMatrixTranspose, fx_SetMatrixTransposeArray, fx_GetMatrixTransposeArray,
        fx_SetMatrixTransposePointerArray, fx_GetMatrixTransposePointerArray, fx_SetString, fx_GetString, fx_SetTexture, fx_GetTexture,
        fx_GetPixelShader, fx_GetVertexShader, fx_SetArrayRange,
        fx_GetPool, fx_SetTechnique, fx_GetCurrentTechnique, fx_ValidateTechnique, fx_FindNextValidTechnique, fx_IsParameterUsed,
        fx_Begin, fx_BeginPass, fx_CommitChanges, fx_EndPass, fx_End, fx_GetDevice, fx_OnLostDevice, fx_OnResetDevice,
        fx_SetStateManager, fx_GetStateManager, fx_BeginParameterBlock, fx_EndParameterBlock, fx_ApplyParameterBlock,
        fx_DeleteParameterBlock, fx_CloneEffect, fx_SetRawValue};
    static const FnX xb[] = {xb_QueryInterface, xb_AddRef, xb_Release, xb_GetBufferPointer, xb_GetBufferSize};
    int n = (int)(sizeof fx / sizeof *fx);
    uint32_t t = heap_alloc(w32_process_heap, 8, 4u * (uint32_t)n);
    for (int k = 0; k < n; k++) { char nm[64]; snprintf(nm, sizeof nm, "d3dx9!ID3DXEffect.%d", k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, fx[k])); }
    vt[O9_FX] = t;
    t = heap_alloc(w32_process_heap, 8, 4u * 5);
    for (int k = 0; k < 5; k++) { char nm[64]; snprintf(nm, sizeof nm, "d3dx9!ID3DXBuffer.%d", k); rt_w32(G_MEM, t + 4u * (uint32_t)k, w32_thunk_register(nm, xb[k])); }
    vt[O9_XBUF] = t;
}
