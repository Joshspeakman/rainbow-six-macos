/* hlsl.h: the HLSL subset of Dungeon Siege II's effect files (.fx), parsed and compiled to Metal Shading Language
 * (hlsl.c), for the D3DX effect framework (d3dx9_fx.c). Effects declare parameters (with semantics, annotations and
 * default values), samplers, structs, functions, and techniques whose passes assign render states and compiled shaders. */
#pragma once
#include <stdint.h>
#include <stddef.h>

enum { HB_VOID, HB_BOOL, HB_INT, HB_FLOAT, HB_STRUCT, HB_SAMPLER, HB_TEXTURE, HB_STRING };
enum { HS_GENERIC, HS_2D, HS_3D, HS_CUBE };           /* sampler kinds */
typedef struct HType { uint8_t base, rows, cols, mat, samp; int16_t st; int arr; } HType;   /* vector: rows 1; scalar 1x1 */

typedef struct HExpr HExpr;
typedef struct HAnno { char type[16], name[64], str[128]; double num; } HAnno;
typedef struct HState { char name[48]; int index; HExpr *value; char ref[64]; char profile[16], fn[64]; HExpr **args; int nargs;
                       struct HState *sub; int nsub; } HState;   /* sub: an inline sampler_state { ... } */
typedef struct HVar {                                  /* a global */
    char name[64], semantic[48]; HType t;
    int is_static, is_const, is_uniform, reg_kind, reg;   /* reg_kind: 'c', 's' or 0 */
    HExpr *init; HAnno *anno; int nanno;
    HState *ss; int nss;                               /* sampler_state { ... } */
    float *defval; int ndef;                           /* default value, as floats (registers of 4) */
} HVar;
typedef struct HField { char name[64], semantic[48]; HType t; } HField;
typedef struct HStruct { char name[64]; HField *f; int nf; } HStruct;
typedef struct HParam { char name[64], semantic[48]; HType t; int dir; int is_uniform; } HParam;   /* dir: 1 in, 2 out, 3 inout */
typedef struct HStmt HStmt;
typedef struct HFunc { char name[64], semantic[48]; HType ret; HParam *p; int np; HStmt *body; } HFunc;
typedef struct HPass { char name[64]; HAnno *anno; int nanno; HState *st; int nst; } HPass;
typedef struct HTech { char name[64]; HAnno *anno; int nanno; HPass *pass; int npass; } HTech;

typedef struct HFx {
    HVar *v; int nv; HStruct *s; int ns; HFunc *f; int nf; HTech *t; int nt;
    char err[512];
    void *arena;
} HFx;

HFx *hlsl_parse(const char *src, size_t len);          /* NULL on out-of-memory; check fx->err */
void hlsl_free(HFx *fx);
int hlsl_type_regs(const HFx *fx, HType t);           /* float4 registers a parameter of this type occupies */
int hlsl_type_floats(const HFx *fx, HType t);         /* floats of its value (rows*cols per element) */
int hlsl_eval_init(HFx *fx, HVar *v);                  /* default value from the initializer into v->defval */
int hlsl_eval(HFx *fx, HExpr *e, float *out, int cap); /* a constant expression (state values); count of floats, 0 if not constant */
int hlsl_expr_is_float(const HExpr *e);                /* of float type (a state value whose bits are a float) */
const char *hlsl_expr_string(const HExpr *e);          /* a string literal's text, else NULL */

/* One pass compiled to a Metal program: vertex function "vs_fx", fragment function "fs_fx". The parameters it reads
 * are in one buffer of float4 registers (preg[i]: first register of global i, -1 when unused); its samplers are bound
 * to texture/sampler slots 0..n-1 (sslot[k]: index of the global sampler in slot k). */
typedef struct HProgram {
    char *msl; size_t msl_len;
    int *preg; int nregs;                              /* per global */
    int sslot[16]; int nsamp; int skind[16];          /* skind: HS_2D, HS_CUBE or HS_3D per slot */
    int vs_ffp, ps_ffp;                                /* fixed-function stages generated */
    char err[512];
} HProgram;
HProgram *hlsl_compile_pass(HFx *fx, int tech, int pass);
void hlsl_program_free(HProgram *p);
