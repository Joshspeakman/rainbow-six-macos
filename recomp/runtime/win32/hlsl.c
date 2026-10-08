/* hlsl.c: parser and Metal code generator for the HLSL subset of Dungeon Siege II's effect files (see hlsl.h).
 *
 * Parsing builds typed declarations, expressions and statements. Code generation emits, per pass, one Metal program:
 * the pass's vertex shader (or a generated fixed-function vertex stage) and pixel shader (or a generated
 * fixed-function texture-stage combiner), sharing one interface (position, two colours, eight texture coordinates,
 * fog). HLSL matrices are kept transposed in Metal (a D3D row-major matrix loads as a Metal column-major one), so
 * mul(v, M) becomes M * v; HLSL's implicit conversions (scalar broadcast, vector truncation) are made explicit. */
#include "hlsl.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- arena: everything a parse allocates, freed at once ---- */
typedef struct Blk { struct Blk *next; size_t used, cap; char d[]; } Blk;
static void *A(HFx *fx, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    Blk *b = fx->arena;
    if (!b || b->used + n > b->cap) {
        size_t cap = n > 65536 ? n : 65536; Blk *nb = malloc(sizeof *nb + cap); if (!nb) return NULL;
        nb->next = b; nb->used = 0; nb->cap = cap; fx->arena = nb; b = nb;
    }
    void *p = b->d + b->used; b->used += n; memset(p, 0, n); return p;
}
#define GROW(fx, arr, n, cap) do { if ((n) == (cap)) { int nc_ = (cap) ? (cap) * 2 : 8; void *na_ = A(fx, sizeof *(arr) * (size_t)nc_); if ((arr)) memcpy(na_, (arr), sizeof *(arr) * (size_t)(n)); (arr) = na_; (cap) = nc_; } } while (0)

/* ---- lexer ---- */
enum { TK_EOF, TK_ID, TK_NUM, TK_STR, TK_PUNCT };
typedef struct Tok { int k; char s[128]; double num; int isfloat; int line; } Tok;
typedef struct P {
    HFx *fx; const char *src, *end, *p; int line; Tok t, peek; int haspeek; int failed;
    struct Scope *scope;
    int capv, caps, capf, capt;
    struct { char name[64]; char body[256]; } mac[64]; int nmac;   /* #define NAME text (object-like macros) */
    struct { const char *p, *end; } mstack[8]; int msp;
} P;
static void perr(P *p, const char *fmt, ...)
{
    if (p->failed) return;
    p->failed = 1; va_list ap; va_start(ap, fmt);
    int n = snprintf(p->fx->err, sizeof p->fx->err, "line %d: ", p->t.line);
    vsnprintf(p->fx->err + n, sizeof p->fx->err - (size_t)n, fmt, ap); va_end(ap);
}
static void lex1(P *p, Tok *t)
{
    const char *s = p->p;
    for (;;) {
        while (s < p->end && isspace((unsigned char)*s)) { if (*s == '\n') p->line++; s++; }
        if (s + 1 < p->end && s[0] == '/' && s[1] == '/') { while (s < p->end && *s != '\n') s++; continue; }
        if (s + 1 < p->end && s[0] == '/' && s[1] == '*') { s += 2; while (s + 1 < p->end && !(s[0] == '*' && s[1] == '/')) { if (*s == '\n') p->line++; s++; } s += 2; continue; }
        if (s < p->end && *s == '#') {                    /* #define NAME text; other directives are skipped */
            const char *e = s; while (e < p->end && *e != '\n') e++;
            if (e - s > 8 && !strncmp(s, "#define", 7) && p->nmac < 64) {
                const char *q = s + 7; while (q < e && isspace((unsigned char)*q)) q++;
                int n = 0; char *nm = p->mac[p->nmac].name; while (q < e && (isalnum((unsigned char)*q) || *q == '_') && n < 63) nm[n++] = *q++;
                nm[n] = 0;
                if (n && (q >= e || *q != '(')) { snprintf(p->mac[p->nmac].body, sizeof p->mac[p->nmac].body, "%.*s", (int)(e - q), q); p->nmac++; }
            }
            s = e; continue;
        }
        if (s >= p->end && p->msp > 0) { p->msp--; s = p->mstack[p->msp].p; p->end = p->mstack[p->msp].end; continue; }   /* the end of a macro's text */
        break;
    }
    t->line = p->line; t->s[0] = 0; t->isfloat = 0;
    if (s >= p->end) { t->k = TK_EOF; p->p = s; return; }
    if (isalpha((unsigned char)*s) || *s == '_') {
        int n = 0; while (s < p->end && (isalnum((unsigned char)*s) || *s == '_')) { if (n < 127) t->s[n++] = *s; s++; }
        t->s[n] = 0; t->k = TK_ID;
        for (int m = 0; m < p->nmac; m++) if (!strcmp(p->mac[m].name, t->s) && p->msp < 8) {   /* a macro: lex its text instead */
            p->mstack[p->msp].p = s; p->mstack[p->msp].end = p->end; p->msp++;
            p->p = p->mac[m].body; p->end = p->mac[m].body + strlen(p->mac[m].body);
            lex1(p, t); return;
        }
    } else if (isdigit((unsigned char)*s) || (*s == '.' && s + 1 < p->end && isdigit((unsigned char)s[1]))) {
        char *e; int n = 0;
        if (s[0] == '0' && s + 1 < p->end && (s[1] == 'x' || s[1] == 'X')) t->num = (double)strtoull(s, &e, 16);
        else { t->num = strtod(s, &e); for (const char *q = s; q < e; q++) if (*q == '.' || *q == 'e' || *q == 'E') t->isfloat = 1; }
        for (const char *q = s; q < e && n < 127; q++) t->s[n++] = *q;
        t->s[n] = 0; s = e;
        while (s < p->end && (*s == 'f' || *s == 'F' || *s == 'h' || *s == 'H' || *s == 'l' || *s == 'L' || *s == 'u' || *s == 'U')) { if (*s == 'f' || *s == 'F' || *s == 'h' || *s == 'H') t->isfloat = 1; s++; }
        t->k = TK_NUM;
    } else if (*s == '"') {
        int n = 0; s++; while (s < p->end && *s != '"') { if (n < 127) t->s[n++] = *s; s++; }
        t->s[n] = 0; s++; t->k = TK_STR;
    } else {
        static const char *two[] = {"==", "!=", "<=", ">=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=", "<<", ">>", 0};
        t->k = TK_PUNCT; t->s[0] = *s; t->s[1] = 0;
        for (int i = 0; two[i]; i++) if (s + 1 < p->end && s[0] == two[i][0] && s[1] == two[i][1]) { t->s[1] = s[1]; t->s[2] = 0; s++; break; }
        s++;
    }
    p->p = s;
}
static void next(P *p) { if (p->haspeek) { p->t = p->peek; p->haspeek = 0; } else lex1(p, &p->t); }
static Tok *peek(P *p) { if (!p->haspeek) { lex1(p, &p->peek); p->haspeek = 1; } return &p->peek; }
static int is(P *p, const char *s) { return (p->t.k == TK_PUNCT || p->t.k == TK_ID) && !strcmp(p->t.s, s); }
static int accept(P *p, const char *s) { if (is(p, s)) { next(p); return 1; } return 0; }
static void expect(P *p, const char *s) { if (!accept(p, s)) perr(p, "expected '%s' before '%s'", s, p->t.s); }
static void ident(P *p, char *out, size_t cap) { if (p->t.k != TK_ID) { perr(p, "expected a name before '%s'", p->t.s); out[0] = 0; return; } snprintf(out, cap, "%s", p->t.s); next(p); }

/* ---- types ---- */
static HType ty(int base, int rows, int cols, int mat) { HType t = {0}; t.base = (uint8_t)base; t.rows = (uint8_t)rows; t.cols = (uint8_t)cols; t.mat = (uint8_t)mat; t.st = -1; return t; }
static int find_struct(HFx *fx, const char *n) { for (int i = 0; i < fx->ns; i++) if (!strcmp(fx->s[i].name, n)) return i; return -1; }
/* a type name (float3, half4x4, int, bool, sampler2D, texture, string, a struct name); 0 when the name isn't a type */
static int type_name(HFx *fx, const char *n, HType *out)
{
    static const struct { const char *pre; int base; } b[] = {{"float", HB_FLOAT}, {"half", HB_FLOAT}, {"double", HB_FLOAT}, {"int", HB_INT}, {"uint", HB_INT}, {"bool", HB_BOOL}, {"dword", HB_INT}, {0, 0}};
    for (int i = 0; b[i].pre; i++) {
        size_t l = strlen(b[i].pre); if (strncmp(n, b[i].pre, l)) continue;
        const char *r = n + l;
        if (!*r) { *out = ty(b[i].base, 1, 1, 0); return 1; }
        if (r[0] >= '1' && r[0] <= '4' && !r[1]) { *out = ty(b[i].base, 1, r[0] - '0', 0); return 1; }
        if (r[0] >= '1' && r[0] <= '4' && r[1] == 'x' && r[2] >= '1' && r[2] <= '4' && !r[3]) { *out = ty(b[i].base, r[0] - '0', r[2] - '0', 1); return 1; }
    }
    if (!strcmp(n, "sampler") || !strcmp(n, "sampler_state")) { *out = ty(HB_SAMPLER, 1, 1, 0); out->samp = HS_GENERIC; return 1; }
    if (!strcmp(n, "sampler2D")) { *out = ty(HB_SAMPLER, 1, 1, 0); out->samp = HS_2D; return 1; }
    if (!strcmp(n, "sampler3D")) { *out = ty(HB_SAMPLER, 1, 1, 0); out->samp = HS_3D; return 1; }
    if (!strcmp(n, "samplerCUBE")) { *out = ty(HB_SAMPLER, 1, 1, 0); out->samp = HS_CUBE; return 1; }
    if (!strcmp(n, "texture") || !strcmp(n, "texture2D") || !strcmp(n, "texture3D") || !strcmp(n, "textureCUBE")) { *out = ty(HB_TEXTURE, 1, 1, 0); return 1; }
    if (!strcmp(n, "string")) { *out = ty(HB_STRING, 1, 1, 0); return 1; }
    if (!strcmp(n, "void")) { *out = ty(HB_VOID, 1, 1, 0); return 1; }
    if (!strcmp(n, "vector")) { *out = ty(HB_FLOAT, 1, 4, 0); return 1; }
    if (!strcmp(n, "matrix")) { *out = ty(HB_FLOAT, 4, 4, 1); return 1; }
    int s = find_struct(fx, n); if (s >= 0) { *out = ty(HB_STRUCT, 1, 1, 0); out->st = (int16_t)s; return 1; }
    return 0;
}
static int is_scalar(HType t) { return t.base <= HB_FLOAT && !t.mat && t.cols == 1 && t.rows == 1; }
static int is_vector(HType t) { return t.base <= HB_FLOAT && !t.mat && t.cols > 1; }
static int ncomp(HType t) { return t.mat ? t.rows * t.cols : t.cols; }
int hlsl_type_floats(const HFx *fx, HType t)
{
    int n;
    if (t.base == HB_STRUCT) { n = 0; for (int i = 0; i < fx->s[t.st].nf; i++) n += hlsl_type_floats(fx, fx->s[t.st].f[i].t); }
    else if (t.base == HB_SAMPLER || t.base == HB_TEXTURE || t.base == HB_STRING) n = 0;
    else n = t.mat ? t.rows * t.cols : t.cols;
    return n * (t.arr ? t.arr : 1);
}
/* float4 registers, D3D-style: a matrix one per row, a scalar or vector one, arrays one per element */
int hlsl_type_regs(const HFx *fx, HType t)
{
    int n;
    if (t.base == HB_STRUCT) { n = 0; for (int i = 0; i < fx->s[t.st].nf; i++) n += hlsl_type_regs(fx, fx->s[t.st].f[i].t); }
    else if (t.base == HB_SAMPLER || t.base == HB_TEXTURE || t.base == HB_STRING) n = 0;
    else n = t.mat ? t.rows : 1;
    return n * (t.arr ? t.arr : 1);
}

/* ---- syntax tree ---- */
enum { E_NUM, E_GVAR, E_LVAR, E_FIELD, E_SWZ, E_INDEX, E_CALL, E_INTR, E_CTOR, E_CAST, E_UN, E_BIN, E_ASSIGN, E_COND, E_INIT, E_POST };
struct HExpr {
    int k; HType t; char op[4]; double num; int isfloat;
    int idx;                                  /* E_GVAR global, E_CALL function, E_FIELD field */
    char name[64];                            /* E_LVAR, E_INTR, E_SWZ (swizzle) */
    HExpr *a, *b, *c; HExpr **args; int nargs;
};
enum { S_BLOCK, S_DECL, S_EXPR, S_RET, S_IF, S_FOR, S_WHILE, S_DISCARD, S_BREAK, S_CONTINUE };
struct HStmt { int k; HStmt **list; int n; HType t; char name[64]; HExpr *e, *e2; HStmt *a, *b, *init; int declist; };

typedef struct Sym { char name[64]; HType t; } Sym;
typedef struct Scope { struct Scope *up; Sym s[64]; int n; } Scope;
static Sym *lookup_local(P *p, const char *n)
{
    for (Scope *s = p->scope; s; s = s->up) for (int i = s->n - 1; i >= 0; i--) if (!strcmp(s->s[i].name, n)) return &s->s[i];
    return NULL;
}
static void add_local(P *p, const char *n, HType t)
{
    if (!p->scope || p->scope->n == 64) { perr(p, "too many local variables"); return; }
    Sym *s = &p->scope->s[p->scope->n++]; snprintf(s->name, sizeof s->name, "%s", n); s->t = t;
}
static int find_global(HFx *fx, const char *n) { for (int i = 0; i < fx->nv; i++) if (!strcmp(fx->v[i].name, n)) return i; return -1; }
static int find_func(HFx *fx, const char *n) { for (int i = fx->nf - 1; i >= 0; i--) if (!strcmp(fx->f[i].name, n)) return i; return -1; }

static HExpr *E(P *p, int k) { HExpr *e = A(p->fx, sizeof *e); if (!e) { perr(p, "out of memory"); static HExpr dummy; return &dummy; } e->k = k; e->t = ty(HB_FLOAT, 1, 1, 0); return e; }
static HType widest(HType a, HType b)       /* result of an arithmetic operation */
{
    HType r = a.base >= b.base ? a : b; r.base = a.base > b.base ? a.base : b.base; r.arr = 0;
    if (is_scalar(a)) { HType t = b; t.base = r.base; t.arr = 0; return t; }
    if (is_scalar(b)) { HType t = a; t.base = r.base; t.arr = 0; return t; }
    if (is_vector(a) && is_vector(b)) { HType t = a; t.base = r.base; t.cols = a.cols < b.cols ? a.cols : b.cols; t.arr = 0; return t; }
    return r;
}
static HExpr *expr(P *p);
static HExpr *assign_expr(P *p);
static HExpr *unary(P *p);
static HExpr **call_args(P *p, int *n)
{
    HExpr **a = NULL; int cap = 0; *n = 0; expect(p, "(");
    if (!accept(p, ")")) { do { GROW(p->fx, a, *n, cap); a[(*n)++] = assign_expr(p); } while (accept(p, ",")); expect(p, ")"); }
    return a;
}
/* the result type of an intrinsic */
static HType intrinsic_type(P *p, const char *n, HExpr **a, int na)
{
    HType f1 = ty(HB_FLOAT, 1, 1, 0);
    if (!strcmp(n, "tex2D") || !strcmp(n, "texCUBE") || !strcmp(n, "tex3D") || !strcmp(n, "tex2Dproj") || !strcmp(n, "tex2Dlod") || !strcmp(n, "tex2Dbias")) return ty(HB_FLOAT, 1, 4, 0);
    if (!strcmp(n, "dot") || !strcmp(n, "length") || !strcmp(n, "distance") || !strcmp(n, "noise") || !strcmp(n, "determinant")) return f1;
    if (!strcmp(n, "cross")) return ty(HB_FLOAT, 1, 3, 0);
    if (!strcmp(n, "clip")) return ty(HB_VOID, 1, 1, 0);
    if (!strcmp(n, "any") || !strcmp(n, "all")) return ty(HB_BOOL, 1, 1, 0);
    if (!strcmp(n, "mul")) {
        if (na != 2) { perr(p, "mul needs two arguments"); return f1; }
        HType x = a[0]->t, y = a[1]->t;
        if (is_scalar(x)) return y;
        if (is_scalar(y)) return x;
        if (!x.mat && y.mat) return ty(HB_FLOAT, 1, y.cols, 0);           /* row vector * matrix */
        if (x.mat && !y.mat) return ty(HB_FLOAT, 1, x.rows, 0);           /* matrix * column vector */
        if (x.mat && y.mat) return ty(HB_FLOAT, x.rows, y.cols, 1);
        return f1;                                                         /* vector . vector */
    }
    if (!strcmp(n, "transpose") && na == 1) { HType t = a[0]->t; return ty(HB_FLOAT, t.cols, t.rows, 1); }
    /* component-wise: the widest argument (lerp, min, max, clamp, pow, saturate, normalize, abs, frac, sin, ...) */
    HType r = na ? a[0]->t : f1; for (int i = 1; i < na; i++) r = widest(r, a[i]->t);
    if (r.base < HB_FLOAT && strcmp(n, "abs") && strcmp(n, "min") && strcmp(n, "max") && strcmp(n, "sign")) r.base = HB_FLOAT;
    r.arr = 0; return r;
}
static int swizzle_ok(const char *s, int maxc)
{
    int n = (int)strlen(s); if (n < 1 || n > 4) return 0;
    for (int i = 0; i < n; i++) {
        const char *x = strchr("xyzw", s[i]), *c = strchr("rgba", s[i]);
        int k = x ? (int)(x - "xyzw") : c ? (int)(c - "rgba") : -1; if (k < 0 || k >= maxc) return 0;
    }
    return 1;
}
static HExpr *primary(P *p)
{
    HExpr *e;
    if (p->t.k == TK_STR) { e = E(p, E_NUM); e->t = ty(HB_STRING, 1, 1, 0); snprintf(e->name, sizeof e->name, "%s", p->t.s); next(p); return e; }
    if (p->t.k == TK_NUM) { e = E(p, E_NUM); e->num = p->t.num; e->isfloat = p->t.isfloat; e->t = ty(p->t.isfloat ? HB_FLOAT : HB_INT, 1, 1, 0); next(p); return e; }
    if (accept(p, "(")) {
        HType ct;   /* a cast: (float3x3)m, (VS_OUTPUT)0 */
        if (p->t.k == TK_ID && type_name(p->fx, p->t.s, &ct) && (peek(p)->k == TK_PUNCT && !strcmp(peek(p)->s, ")"))) {
            next(p); expect(p, ")"); e = E(p, E_CAST); e->t = ct; e->a = unary(p);
            /* the cast binds to a unary expression */
            return e;
        }
        e = expr(p); expect(p, ")"); return e;
    }
    if (accept(p, "{")) {                  /* initializer list */
        e = E(p, E_INIT); int cap = 0;
        if (!accept(p, "}")) { do { if (is(p, "}")) break; GROW(p->fx, e->args, e->nargs, cap); e->args[e->nargs++] = assign_expr(p); } while (accept(p, ",")); expect(p, "}"); }
        return e;
    }
    if (p->t.k != TK_ID) { perr(p, "unexpected '%s'", p->t.s); next(p); return E(p, E_NUM); }
    char n[64]; snprintf(n, sizeof n, "%s", p->t.s);
    if (!strcmp(n, "true") || !strcmp(n, "false")) { next(p); e = E(p, E_NUM); e->num = n[0] == 't'; e->t = ty(HB_BOOL, 1, 1, 0); return e; }
    HType ct;
    if (type_name(p->fx, n, &ct) && ct.base <= HB_FLOAT && peek(p)->k == TK_PUNCT && !strcmp(peek(p)->s, "(")) {   /* constructor */
        next(p); e = E(p, E_CTOR); e->t = ct; e->args = call_args(p, &e->nargs); return e;
    }
    next(p);
    if (is(p, "(")) {                      /* a call: a user function, else an intrinsic */
        int f = find_func(p->fx, n);
        e = E(p, f >= 0 ? E_CALL : E_INTR); snprintf(e->name, sizeof e->name, "%s", n); e->idx = f;
        e->args = call_args(p, &e->nargs);
        e->t = f >= 0 ? p->fx->f[f].ret : intrinsic_type(p, n, e->args, e->nargs);
        return e;
    }
    Sym *s = lookup_local(p, n);
    if (s) { e = E(p, E_LVAR); snprintf(e->name, sizeof e->name, "%s", n); e->t = s->t; return e; }
    int g = find_global(p->fx, n);
    if (g >= 0) { e = E(p, E_GVAR); e->idx = g; e->t = p->fx->v[g].t; snprintf(e->name, sizeof e->name, "%s", n); return e; }
    perr(p, "unknown name '%s'", n); return E(p, E_NUM);
}
static HExpr *postfix(P *p)
{
    HExpr *e = primary(p);
    for (;;) {
        if (accept(p, ".")) {
            char m[64]; ident(p, m, sizeof m);
            if (e->t.base == HB_STRUCT) {
                HStruct *st = &p->fx->s[e->t.st]; int f = -1;
                for (int i = 0; i < st->nf; i++) if (!strcmp(st->f[i].name, m)) f = i;
                if (f < 0) { perr(p, "no member '%s' in %s", m, st->name); return e; }
                HExpr *x = E(p, E_FIELD); x->a = e; x->idx = f; x->t = st->f[f].t; e = x;
            } else if (!e->t.mat && swizzle_ok(m, e->t.cols < 1 ? 1 : (is_scalar(e->t) ? 1 : e->t.cols)) ) {
                HExpr *x = E(p, E_SWZ); x->a = e; snprintf(x->name, sizeof x->name, "%s", m); x->t = ty(e->t.base, 1, (int)strlen(m), 0); e = x;
            } else if (is_scalar(e->t) && swizzle_ok(m, 4)) {   /* scalar.xxx */
                HExpr *x = E(p, E_SWZ); x->a = e; snprintf(x->name, sizeof x->name, "%s", m); x->t = ty(e->t.base, 1, (int)strlen(m), 0); e = x;
            } else { perr(p, "bad member or swizzle '.%s'", m); return e; }
        } else if (accept(p, "[")) {
            HExpr *x = E(p, E_INDEX); x->a = e; x->b = expr(p); expect(p, "]");
            HType t = e->t;
            if (t.arr) t.arr = 0; else if (t.mat) t = ty(t.base, 1, t.cols, 0); else t = ty(t.base, 1, 1, 0);
            x->t = t; e = x;
        } else if (is(p, "++") || is(p, "--")) {
            HExpr *x = E(p, E_POST); snprintf(x->op, sizeof x->op, "%s", p->t.s); next(p); x->a = e; x->t = e->t; e = x;
        } else break;
    }
    return e;
}
static HExpr *unary(P *p)
{
    if (is(p, "-") || is(p, "!") || is(p, "+") || is(p, "~") || is(p, "++") || is(p, "--")) {
        HExpr *e = E(p, E_UN); snprintf(e->op, sizeof e->op, "%s", p->t.s); next(p); e->a = unary(p); e->t = e->a->t;
        if (!strcmp(e->op, "!")) e->t.base = HB_BOOL;
        return e;
    }
    return postfix(p);
}
static int prec(const char *op)
{
    static const struct { const char *o; int p; } t[] = {{"||", 1}, {"&&", 2}, {"|", 3}, {"^", 4}, {"&", 5}, {"==", 6}, {"!=", 6},
        {"<", 7}, {">", 7}, {"<=", 7}, {">=", 7}, {"<<", 8}, {">>", 8}, {"+", 9}, {"-", 9}, {"*", 10}, {"/", 10}, {"%", 10}, {0, 0}};
    for (int i = 0; t[i].o; i++) if (!strcmp(t[i].o, op)) return t[i].p;
    return 0;
}
static HExpr *binary(P *p, int minp)
{
    HExpr *l = unary(p);
    for (;;) {
        if (p->t.k != TK_PUNCT) break;
        int pr = prec(p->t.s); if (!pr || pr < minp) break;
        HExpr *e = E(p, E_BIN); snprintf(e->op, sizeof e->op, "%s", p->t.s); next(p);
        e->a = l; e->b = binary(p, pr + 1);
        if (pr <= 2) e->t = ty(HB_BOOL, 1, 1, 0);
        else if (pr == 6 || pr == 7) { e->t = widest(e->a->t, e->b->t); e->t.base = HB_BOOL; }
        else e->t = widest(e->a->t, e->b->t);
        l = e;
    }
    return l;
}
static HExpr *cond_expr(P *p)
{
    HExpr *c = binary(p, 1);
    if (accept(p, "?")) { HExpr *e = E(p, E_COND); e->a = c; e->b = assign_expr(p); expect(p, ":"); e->c = assign_expr(p); e->t = widest(e->b->t, e->c->t); return e; }
    return c;
}
static HExpr *assign_expr(P *p)
{
    HExpr *l = cond_expr(p);
    if (is(p, "=") || is(p, "+=") || is(p, "-=") || is(p, "*=") || is(p, "/=")) {
        HExpr *e = E(p, E_ASSIGN); snprintf(e->op, sizeof e->op, "%s", p->t.s); next(p); e->a = l; e->b = assign_expr(p); e->t = l->t; return e;
    }
    return l;
}
static HExpr *expr(P *p)
{
    HExpr *e = assign_expr(p);
    while (accept(p, ",")) { HExpr *x = E(p, E_BIN); snprintf(x->op, sizeof x->op, ","); x->a = e; x->b = assign_expr(p); x->t = x->b->t; e = x; }
    return e;
}

/* ---- statements ---- */
static HStmt *S(P *p, int k) { HStmt *s = A(p->fx, sizeof *s); if (!s) { perr(p, "out of memory"); static HStmt d; return &d; } s->k = k; return s; }
static HStmt *stmt(P *p);
static int parse_type(P *p, HType *t)          /* a type at the cursor (with modifiers skipped); 0 if there is none */
{
    while (p->t.k == TK_ID && (!strcmp(p->t.s, "const") || !strcmp(p->t.s, "static") || !strcmp(p->t.s, "uniform") || !strcmp(p->t.s, "inline") ||
                               !strcmp(p->t.s, "row_major") || !strcmp(p->t.s, "column_major") || !strcmp(p->t.s, "extern") || !strcmp(p->t.s, "shared") || !strcmp(p->t.s, "volatile"))) next(p);
    if (p->t.k != TK_ID || !type_name(p->fx, p->t.s, t)) return 0;
    next(p); return 1;
}
static int array_suffix(P *p)
{
    if (!accept(p, "[")) return 0;
    int n = 0; if (p->t.k == TK_NUM) { n = (int)p->t.num; next(p); }
    else if (p->t.k == TK_ID) {                  /* a static const int */
        int g = find_global(p->fx, p->t.s);
        if (g >= 0 && p->fx->v[g].init && p->fx->v[g].init->k == E_NUM) n = (int)p->fx->v[g].init->num; else perr(p, "array size '%s' isn't a constant", p->t.s);
        next(p);
    }
    expect(p, "]"); return n;
}
static HStmt *local_decl(P *p, HType bt)
{
    HStmt *blk = NULL; HStmt *first = NULL; int cap = 0;
    do {
        HStmt *d = S(p, S_DECL); d->t = bt; ident(p, d->name, sizeof d->name);
        int n = array_suffix(p); if (n) d->t.arr = n;
        if (accept(p, "=")) d->e = assign_expr(p);
        add_local(p, d->name, d->t);
        if (!first) first = d;
        else { if (!blk) { blk = S(p, S_BLOCK); blk->k = S_BLOCK; GROW(p->fx, blk->list, blk->n, cap); blk->list[blk->n++] = first; } GROW(p->fx, blk->list, blk->n, cap); blk->list[blk->n++] = d; }
    } while (accept(p, ","));
    expect(p, ";");
    if (blk) blk->declist = 1;                            /* a declaration list: no scope of its own */
    return blk ? blk : first;
}
static HStmt *block(P *p)
{
    Scope sc = {0}; sc.up = p->scope; p->scope = &sc;
    HStmt *b = S(p, S_BLOCK); int cap = 0; expect(p, "{");
    while (!is(p, "}") && p->t.k != TK_EOF && !p->failed) { GROW(p->fx, b->list, b->n, cap); b->list[b->n++] = stmt(p); }
    expect(p, "}"); p->scope = sc.up; return b;
}
static HStmt *stmt(P *p)
{
    HStmt *s; HType t;
    if (is(p, "{")) return block(p);
    if (accept(p, ";")) return S(p, S_BLOCK);
    if (accept(p, "return")) { s = S(p, S_RET); if (!is(p, ";")) s->e = expr(p); expect(p, ";"); return s; }
    if (accept(p, "discard")) { expect(p, ";"); return S(p, S_DISCARD); }
    if (accept(p, "break")) { expect(p, ";"); return S(p, S_BREAK); }
    if (accept(p, "continue")) { expect(p, ";"); return S(p, S_CONTINUE); }
    if (accept(p, "if")) { s = S(p, S_IF); expect(p, "("); s->e = expr(p); expect(p, ")"); s->a = stmt(p); if (accept(p, "else")) s->b = stmt(p); return s; }
    if (accept(p, "while")) { s = S(p, S_WHILE); expect(p, "("); s->e = expr(p); expect(p, ")"); s->a = stmt(p); return s; }
    if (accept(p, "for")) {
        Scope sc = {0}; sc.up = p->scope; p->scope = &sc;
        s = S(p, S_FOR); expect(p, "(");
        if (!accept(p, ";")) { HType it; if (parse_type(p, &it)) s->init = local_decl(p, it); else { s->init = S(p, S_EXPR); s->init->e = expr(p); expect(p, ";"); } }
        if (!is(p, ";")) s->e = expr(p);
        expect(p, ";"); if (!is(p, ")")) s->e2 = expr(p);
        expect(p, ")"); s->a = stmt(p); p->scope = sc.up; return s;
    }
    if (p->t.k == TK_ID && type_name(p->fx, p->t.s, &t) && !(peek(p)->k == TK_PUNCT && !strcmp(peek(p)->s, "("))) { HType bt; parse_type(p, &bt); return local_decl(p, bt); }
    if (p->t.k == TK_ID && (!strcmp(p->t.s, "const") || !strcmp(p->t.s, "static"))) { HType bt; if (parse_type(p, &bt)) return local_decl(p, bt); }
    s = S(p, S_EXPR); s->e = expr(p); expect(p, ";"); return s;
}

/* ---- declarations ---- */
static void semantic(P *p, char *out, size_t cap, int *reg_kind, int *reg)
{
    while (accept(p, ":")) {
        if (accept(p, "register")) {
            expect(p, "("); char r[32]; ident(p, r, sizeof r);
            if (reg_kind) { *reg_kind = r[0]; *reg = atoi(r + 1); }
            expect(p, ")");
        } else ident(p, out, cap);
    }
}
static void annotations(P *p, HAnno **out, int *n)
{
    int cap = 0; *n = 0; *out = NULL;
    if (!accept(p, "<")) return;
    while (!accept(p, ">") && !p->failed && p->t.k != TK_EOF) {
        HAnno a = {0}; ident(p, a.type, sizeof a.type); ident(p, a.name, sizeof a.name); expect(p, "=");
        if (p->t.k == TK_STR) { snprintf(a.str, sizeof a.str, "%s", p->t.s); next(p); }
        else {
            int neg = accept(p, "-");
            if (p->t.k == TK_NUM) { a.num = neg ? -p->t.num : p->t.num; next(p); }
            else if (p->t.k == TK_ID) {                    /* true/false, or a static const */
                if (!strcmp(p->t.s, "true") || !strcmp(p->t.s, "TRUE")) a.num = 1;
                else if (strcmp(p->t.s, "false") && strcmp(p->t.s, "FALSE")) {
                    int g = find_global(p->fx, p->t.s);
                    if (g >= 0 && p->fx->v[g].init && p->fx->v[g].init->k == E_NUM) a.num = p->fx->v[g].init->num;
                    else snprintf(a.str, sizeof a.str, "%s", p->t.s);
                }
                next(p);
            }
        }
        expect(p, ";");
        GROW(p->fx, *out, *n, cap); (*out)[(*n)++] = a;
    }
}
static void state_assign(P *p, HState **out, int *n, int *cap);
/* one state assignment of a pass or a sampler_state: Name[index] = value; */
static void state_assign(P *p, HState **out, int *n, int *cap)
{
    HState s = {0}; ident(p, s.name, sizeof s.name);
    if (accept(p, "[")) { s.index = (int)p->t.num; next(p); expect(p, "]"); }
    expect(p, "=");
    if (accept(p, "sampler_state")) {               /* Sampler[1] = sampler_state { Texture = <t>; }; */
        int cap = 0; expect(p, "{");
        while (!accept(p, "}") && !p->failed && p->t.k != TK_EOF) state_assign(p, &s.sub, &s.nsub, &cap);
    } else if (accept(p, "compile")) {
        ident(p, s.profile, sizeof s.profile); ident(p, s.fn, sizeof s.fn);
        s.args = call_args(p, &s.nargs);
    } else if (accept(p, "<")) { ident(p, s.ref, sizeof s.ref); expect(p, ">"); }
    else if (accept(p, "(")) { ident(p, s.ref, sizeof s.ref); expect(p, ")"); }    /* Texture = (tex) */
    else if (p->t.k == TK_ID && find_global(p->fx, p->t.s) < 0 && !(peek(p)->k == TK_PUNCT && !strcmp(peek(p)->s, "("))) { snprintf(s.ref, sizeof s.ref, "%s", p->t.s); s.index |= 0; s.value = NULL; next(p); s.profile[0] = 0; s.fn[0] = 0; s.args = NULL; s.nargs = -1; }   /* LINEAR, TRUE, NULL ... (nargs -1: a name) */
    else s.value = assign_expr(p);
    expect(p, ";");
    GROW(p->fx, *out, *n, *cap); (*out)[(*n)++] = s;
}
static void technique(P *p)
{
    HTech t = {0}; ident(p, t.name, sizeof t.name); annotations(p, &t.anno, &t.nanno); expect(p, "{"); int pcap = 0;
    while (accept(p, "pass") && !p->failed) {
        HPass ps = {0}; int scap = 0;
        if (p->t.k == TK_ID) ident(p, ps.name, sizeof ps.name);
        annotations(p, &ps.anno, &ps.nanno); expect(p, "{");
        while (!accept(p, "}") && !p->failed && p->t.k != TK_EOF) state_assign(p, &ps.st, &ps.nst, &scap);
        GROW(p->fx, t.pass, t.npass, pcap); t.pass[t.npass++] = ps;
    }
    expect(p, "}");
    GROW(p->fx, p->fx->t, p->fx->nt, p->capt); p->fx->t[p->fx->nt++] = t;
}
static void top_level(P *p)
{
    if (accept(p, "struct")) {
        HStruct st = {0}; int cap = 0; ident(p, st.name, sizeof st.name);
        GROW(p->fx, p->fx->s, p->fx->ns, p->caps); int si = p->fx->ns++; p->fx->s[si] = st;   /* visible to its own members */
        expect(p, "{");
        while (!accept(p, "}") && !p->failed) {
            HType t; if (!parse_type(p, &t)) { perr(p, "expected a member type before '%s'", p->t.s); return; }
            do { HField f = {0}; f.t = t; ident(p, f.name, sizeof f.name); int n = array_suffix(p); if (n) f.t.arr = n; semantic(p, f.semantic, sizeof f.semantic, NULL, NULL);
                 GROW(p->fx, p->fx->s[si].f, p->fx->s[si].nf, cap); p->fx->s[si].f[p->fx->s[si].nf++] = f; } while (accept(p, ","));
            expect(p, ";");
        }
        accept(p, ";"); return;
    }
    if (accept(p, "technique") || accept(p, "technique10")) { technique(p); return; }
    int is_static = 0, is_const = 0, is_uniform = 0;
    while (p->t.k == TK_ID) {
        if (!strcmp(p->t.s, "static")) is_static = 1; else if (!strcmp(p->t.s, "const")) is_const = 1; else if (!strcmp(p->t.s, "uniform")) is_uniform = 1;
        else if (strcmp(p->t.s, "extern") && strcmp(p->t.s, "shared") && strcmp(p->t.s, "row_major") && strcmp(p->t.s, "column_major") && strcmp(p->t.s, "inline")) break;
        next(p);
    }
    HType t; if (!parse_type(p, &t)) { perr(p, "expected a declaration before '%s'", p->t.s); return; }
    char name[64]; ident(p, name, sizeof name);
    if (is(p, "(")) {                                  /* a function */
        HFunc f = {0}; snprintf(f.name, sizeof f.name, "%s", name); f.ret = t; int cap = 0;
        Scope sc = {0}; sc.up = NULL; p->scope = &sc;
        next(p);
        if (!accept(p, ")")) do {
            HParam pa = {0}; pa.dir = 1;
            for (;;) {
                if (accept(p, "in")) pa.dir = 1; else if (accept(p, "out")) pa.dir = 2; else if (accept(p, "inout")) pa.dir = 3; else if (accept(p, "uniform")) pa.is_uniform = 1;
                else if (accept(p, "const")) {} else break;
            }
            if (!parse_type(p, &pa.t)) { perr(p, "expected a parameter type before '%s'", p->t.s); return; }
            ident(p, pa.name, sizeof pa.name); int n = array_suffix(p); if (n) pa.t.arr = n;
            semantic(p, pa.semantic, sizeof pa.semantic, NULL, NULL);
            GROW(p->fx, f.p, f.np, cap); f.p[f.np++] = pa; add_local(p, pa.name, pa.t);
        } while (accept(p, ","));
        if (!p->failed && p->t.k == TK_PUNCT && !strcmp(p->t.s, ")")) next(p);
        semantic(p, f.semantic, sizeof f.semantic, NULL, NULL);
        GROW(p->fx, p->fx->f, p->fx->nf, p->capf); int fi = p->fx->nf++; p->fx->f[fi] = f;   /* visible to recursion and later code */
        if (accept(p, ";")) { p->scope = NULL; return; }              /* a prototype */
        p->fx->f[fi].body = block(p); p->scope = NULL;
        return;
    }
    for (;;) {                                         /* variables */
        HVar v = {0}; snprintf(v.name, sizeof v.name, "%s", name); v.t = t; v.is_static = is_static; v.is_const = is_const; v.is_uniform = is_uniform;
        int n = array_suffix(p); if (n) v.t.arr = n;
        semantic(p, v.semantic, sizeof v.semantic, &v.reg_kind, &v.reg);
        annotations(p, &v.anno, &v.nanno);
        if (accept(p, "=")) {
            if (accept(p, "sampler_state")) {
                int cap = 0; expect(p, "{");
                while (!accept(p, "}") && !p->failed && p->t.k != TK_EOF) state_assign(p, &v.ss, &v.nss, &cap);
            } else v.init = assign_expr(p);
        }
        GROW(p->fx, p->fx->v, p->fx->nv, p->capv); p->fx->v[p->fx->nv++] = v;
        if (!accept(p, ",")) break;
        ident(p, name, sizeof name);
    }
    expect(p, ";");
}
HFx *hlsl_parse(const char *src, size_t len)
{
    HFx *fx = calloc(1, sizeof *fx); if (!fx) return NULL;
    P p = {0}; p.fx = fx; p.src = p.p = src; p.end = src + len; p.line = 1;
    next(&p);
    static int dbg = -1; if (dbg < 0) dbg = getenv("HLSL_DEBUG") != NULL;
    while (p.t.k != TK_EOF && !p.failed) { if (dbg) fprintf(stderr, "hlsl: line %d '%s'\n", p.t.line, p.t.s); top_level(&p); }
    for (int i = 0; i < fx->nv && !p.failed; i++) hlsl_eval_init(fx, &fx->v[i]);
    return fx;
}
void hlsl_free(HFx *fx)
{
    if (!fx) return;
    for (Blk *b = fx->arena; b; ) { Blk *n = b->next; free(b); b = n; }
    free(fx);
}

/* ---- constant evaluation (parameter defaults, static constants) ---- */
static int ceval(HFx *fx, HExpr *e, float *out, int cap, int depth)
{
    if (!e || depth > 16 || cap <= 0) return 0;
    switch (e->k) {
    case E_NUM: out[0] = (float)e->num; return 1;
    case E_INIT: case E_CTOR: {
        int n = 0;
        for (int i = 0; i < e->nargs; i++) n += ceval(fx, e->args[i], out + n, cap - n, depth + 1);
        if (e->k == E_CTOR && n == 1 && ncomp(e->t) > 1) { int m = ncomp(e->t); for (int i = 1; i < m && i < cap; i++) out[i] = out[0]; n = m < cap ? m : cap; }
        return n;
    }
    case E_CAST: {
        int n = ceval(fx, e->a, out, cap, depth + 1), m = hlsl_type_floats(fx, e->t);
        if (n == 1) { for (int i = 1; i < m && i < cap; i++) out[i] = out[0]; return m < cap ? m : cap; }
        return n < m ? n : m;
    }
    case E_GVAR: {
        HVar *v = &fx->v[e->idx];
        if (!v->defval) hlsl_eval_init(fx, v);
        int n = v->ndef < cap ? v->ndef : cap; if (v->defval) memcpy(out, v->defval, sizeof *out * (size_t)n); return v->defval ? n : 0;
    }
    case E_UN: { int n = ceval(fx, e->a, out, cap, depth + 1); if (!strcmp(e->op, "-")) for (int i = 0; i < n; i++) out[i] = -out[i]; return n; }
    case E_BIN: {
        float a[64], b[64]; int na = ceval(fx, e->a, a, 64, depth + 1), nb = ceval(fx, e->b, b, 64, depth + 1), n = na > nb ? na : nb;
        if (!na || !nb || n > cap) return 0;
        for (int i = 0; i < n; i++) {
            float x = a[na == 1 ? 0 : i], y = b[nb == 1 ? 0 : i];
            switch (e->op[0]) { case '+': out[i] = x + y; break; case '-': out[i] = x - y; break; case '*': out[i] = x * y; break; case '/': out[i] = y != 0 ? x / y : 0; break; default: return 0; }
        }
        return n;
    }
    case E_INTR: {                                 /* a few functions in initializers */
        float a[16]; int n = e->nargs ? ceval(fx, e->args[0], a, 16, depth + 1) : 0;
        if (!strcmp(e->name, "sqrt") && n) { for (int i = 0; i < n; i++) out[i] = sqrtf(a[i]); return n; }
        if (!strcmp(e->name, "normalize") && n) { float l = 0; for (int i = 0; i < n; i++) l += a[i] * a[i]; l = sqrtf(l); for (int i = 0; i < n; i++) out[i] = l ? a[i] / l : 0; return n; }
        return 0;
    }
    }
    return 0;
}
int hlsl_eval(HFx *fx, HExpr *e, float *out, int cap) { return ceval(fx, e, out, cap, 0); }
int hlsl_expr_is_float(const HExpr *e) { return e && e->t.base == HB_FLOAT; }
const char *hlsl_expr_string(const HExpr *e) { return e && e->k == E_NUM && e->t.base == HB_STRING ? e->name : NULL; }
int hlsl_eval_init(HFx *fx, HVar *v)
{
    if (v->defval) return v->ndef;
    int n = hlsl_type_floats(fx, v->t); if (n <= 0) return 0;
    v->defval = A(fx, sizeof(float) * (size_t)n); v->ndef = n;
    if (!v->init) return n;
    float *tmp = A(fx, sizeof(float) * (size_t)(n + 64)); int got = ceval(fx, v->init, tmp, n + 64, 0);
    if (got == 1 && n > 1) for (int i = 0; i < n; i++) v->defval[i] = tmp[0];
    else memcpy(v->defval, tmp, sizeof(float) * (size_t)(got < n ? got : n));
    return n;
}

/* ---- Metal code generation ---- */
typedef struct B { char *s; size_t n, cap; } B;
static void bp(B *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        va_start(ap, fmt); size_t room = b->cap - b->n; int w = vsnprintf(b->s ? b->s + b->n : NULL, b->s ? room : 0, fmt, ap); va_end(ap);
        if (w < 0) return;
        if (b->s && (size_t)w < room) { b->n += (size_t)w; return; }
        size_t nc = b->cap ? b->cap * 2 : 4096; while (nc < b->n + (size_t)w + 1) nc *= 2;
        char *ns = realloc(b->s, nc); if (!ns) return; b->s = ns; b->cap = nc;
    }
}
typedef struct Gen {
    HFx *fx; HProgram *pr; char err[512];
    uint8_t *gused, *fused;                     /* globals and functions the program uses */
    uint8_t *samp_of;                           /* [function][global]: samplers a function (transitively) samples */
    uint8_t *samp_kind;                         /* per global sampler: HS_2D / HS_CUBE / HS_3D from its uses */
    int in_ps;                                  /* generating fragment code (discard allowed) */
} Gen;
static void gerr(Gen *g, const char *fmt, ...) { if (g->err[0]) return; va_list ap; va_start(ap, fmt); vsnprintf(g->err, sizeof g->err, fmt, ap); va_end(ap); }
static const char *bname(int base) { return base == HB_INT ? "int" : base == HB_BOOL ? "bool" : "float"; }
static const char *tn(Gen *g, HType t, char *buf)   /* the Metal spelling of a (non-array) type */
{
    if (t.base == HB_STRUCT) snprintf(buf, 80, "s_%s", g->fx->s[t.st].name);
    else if (t.base == HB_VOID) snprintf(buf, 80, "void");
    else if (t.mat) snprintf(buf, 80, "float%dx%d", t.rows, t.cols);   /* transposed: HLSL RxC is a Metal RxC (R columns) */
    else if (t.cols > 1) snprintf(buf, 80, "%s%d", bname(t.base), t.cols);
    else snprintf(buf, 80, "%s", bname(t.base));
    return buf;
}
static void ex(Gen *g, B *b, HExpr *e);
static const char *SW = "xyzw";
static void num(B *b, double v, int base)
{
    if (base == HB_BOOL) { bp(b, v ? "true" : "false"); return; }
    if (base == HB_INT) { bp(b, "%d", (int)v); return; }
    char s[48]; snprintf(s, sizeof s, "%.9g", v);
    if (!strpbrk(s, ".eEn")) strcat(s, ".0");
    bp(b, "%sf", s);
}
/* e converted to type 'to' (HLSL's implicit conversions, made explicit) */
static void exc(Gen *g, B *b, HExpr *e, HType to)
{
    char tb[80]; HType f = e->t;
    if (to.base == HB_STRUCT) {
        if (f.base != HB_STRUCT) { bp(b, "%s{}", tn(g, to, tb)); return; }   /* (VS_OUTPUT)0 */
        ex(g, b, e); return;
    }
    if (to.base >= HB_SAMPLER || to.base == HB_VOID || to.arr || f.arr || f.base >= HB_SAMPLER) { ex(g, b, e); return; }
    if (e->k == E_INIT || (e->k == E_CTOR && e->nargs == 0)) {         /* {a, b, c} as a vector or matrix */
        bp(b, "%s(", tn(g, to, tb));
        for (int i = 0; i < e->nargs; i++) { if (i) bp(b, ", "); exc(g, b, e->args[i], ty(to.base, 1, 1, 0).base == to.base && is_scalar(e->args[i]->t) ? ty(to.base, 1, 1, 0) : e->args[i]->t); }
        bp(b, ")"); return;
    }
    if (to.mat) {
        if (f.mat && (f.rows != to.rows || f.cols != to.cols)) {           /* (float3x3)m: the upper-left part */
            B t = {0}; ex(g, &t, e);
            bp(b, "%s(", tn(g, to, tb));
            for (int r = 0; r < to.rows; r++) { if (r) bp(b, ", "); bp(b, "(%s)[%d].%.*s", t.s, r, to.cols, SW); }
            bp(b, ")"); free(t.s); return;
        }
        if (!f.mat && is_scalar(f)) {                                      /* a scalar broadcast to every element */
            B t = {0}; ex(g, &t, e); bp(b, "%s(", tn(g, to, tb));
            for (int r = 0; r < to.rows; r++) { if (r) bp(b, ", "); bp(b, "float%d(%s)", to.cols, t.s); }
            bp(b, ")"); free(t.s); return;
        }
        ex(g, b, e); return;
    }
    if (is_scalar(to)) {
        if (is_scalar(f)) { if (f.base == to.base) ex(g, b, e); else { bp(b, "%s(", bname(to.base)); ex(g, b, e); bp(b, ")"); } return; }
        if (f.base != to.base) bp(b, "%s(", bname(to.base));
        bp(b, "("); ex(g, b, e); bp(b, f.mat ? ")[0].x" : ").x");
        if (f.base != to.base) bp(b, ")");
        return;
    }
    /* a vector */
    if (is_scalar(f)) { bp(b, "%s(", tn(g, to, tb)); ex(g, b, e); bp(b, ")"); return; }
    if (f.mat) { bp(b, "%s((", tn(g, to, tb)); ex(g, b, e); bp(b, ")[0])"); return; }
    if (f.cols == to.cols) { if (f.base == to.base) ex(g, b, e); else { bp(b, "%s(", tn(g, to, tb)); ex(g, b, e); bp(b, ")"); } return; }
    if (f.cols > to.cols) {                                                 /* truncation */
        if (f.base != to.base) bp(b, "%s(", tn(g, to, tb));
        bp(b, "("); ex(g, b, e); bp(b, ").%.*s", to.cols, SW);
        if (f.base != to.base) bp(b, ")");
        return;
    }
    bp(b, "%s(", tn(g, to, tb)); ex(g, b, e);                               /* widening: zero padded */
    for (int i = f.cols; i < to.cols; i++) bp(b, ", 0");
    bp(b, ")");
}
static HType vec(int n) { return n > 1 ? ty(HB_FLOAT, 1, n, 0) : ty(HB_FLOAT, 1, 1, 0); }
static int sampler_global(Gen *g, HExpr *e) { return e->k == E_GVAR && g->fx->v[e->idx].t.base == HB_SAMPLER ? e->idx : -1; }
static void sampler_args(Gen *g, B *b, HExpr *s)   /* the texture and sampler a sampler expression stands for */
{
    if (s->k == E_GVAR) bp(b, "t_%s, s_%s", s->name, s->name);
    else if (s->k == E_LVAR) bp(b, "t_l_%s, s_l_%s", s->name, s->name);
    else gerr(g, "unsupported sampler expression");
}
static void intrinsic(Gen *g, B *b, HExpr *e)
{
    const char *n = e->name; HExpr **a = e->args; int na = e->nargs; char tb[80];
    if (!strcmp(n, "mul") && na == 2) {
        HType x = a[0]->t, y = a[1]->t;
        if (is_scalar(x) || is_scalar(y)) { bp(b, "("); ex(g, b, a[0]); bp(b, " * "); ex(g, b, a[1]); bp(b, ")"); return; }
        if (!x.mat && y.mat) { bp(b, "("); ex(g, b, a[1]); bp(b, " * "); exc(g, b, a[0], vec(y.rows)); bp(b, ")"); return; }   /* mul(v, M) = M' v */
        if (x.mat && !y.mat) { bp(b, "("); exc(g, b, a[1], vec(x.cols)); bp(b, " * "); ex(g, b, a[0]); bp(b, ")"); return; }   /* mul(M, v) = v M' */
        if (x.mat && y.mat) { bp(b, "("); ex(g, b, a[1]); bp(b, " * "); ex(g, b, a[0]); bp(b, ")"); return; }                   /* (AB)' = B'A' */
        int c = x.cols < y.cols ? x.cols : y.cols; bp(b, "dot("); exc(g, b, a[0], vec(c)); bp(b, ", "); exc(g, b, a[1], vec(c)); bp(b, ")"); return;
    }
    if (!strncmp(n, "tex", 3) && na >= 2) {
        int sk = HS_2D, gi = sampler_global(g, a[0]);
        if (!strcmp(n, "texCUBE")) sk = HS_CUBE; else if (!strcmp(n, "tex3D")) sk = HS_3D;
        if (gi >= 0 && g->samp_kind) g->samp_kind[gi] = (uint8_t)sk;
        B s = {0}; sampler_args(g, &s, a[0]);
        char *comma = s.s ? strchr(s.s, ',') : NULL; if (comma) *comma = 0;
        if (gi >= 0) bp(b, "hlsl_x1(g.xr_%s, ", g->fx->v[gi].name);   /* an XRGB texture's alpha reads as 1 (D3D) */
        bp(b, "%s.sample(%s, ", s.s ? s.s : "?", comma ? comma + 2 : "?");
        if (!strcmp(n, "tex2Dproj")) { B c = {0}; exc(g, &c, a[1], vec(4)); bp(b, "(%s).xy / (%s).w)", c.s, c.s); free(c.s); }
        else if (!strcmp(n, "tex2Dlod")) { B c = {0}; exc(g, &c, a[1], vec(4)); bp(b, "(%s).xy, level((%s).w))", c.s, c.s); free(c.s); }
        else if (!strcmp(n, "tex2Dbias")) { B c = {0}; exc(g, &c, a[1], vec(4)); bp(b, "(%s).xy, bias((%s).w))", c.s, c.s); free(c.s); }
        else { exc(g, b, a[1], vec(sk == HS_2D ? 2 : 3)); bp(b, ")"); }
        if (gi >= 0) bp(b, ")");
        free(s.s); return;
    }
    static const struct { const char *h, *m; } same[] = {{"lerp", "mix"}, {"frac", "fract"}, {"saturate", "saturate"}, {"normalize", "normalize"},
        {"length", "length"}, {"cross", "cross"}, {"reflect", "reflect"}, {"refract", "refract"}, {"pow", "pow"}, {"min", "min"}, {"max", "max"}, {"clamp", "clamp"},
        {"abs", "abs"}, {"sin", "sin"}, {"cos", "cos"}, {"tan", "tan"}, {"asin", "asin"}, {"acos", "acos"}, {"atan", "atan"}, {"atan2", "atan2"},
        {"sqrt", "sqrt"}, {"rsqrt", "rsqrt"}, {"exp", "exp"}, {"exp2", "exp2"}, {"log", "log"}, {"log2", "log2"}, {"log10", "log10"}, {"floor", "floor"},
        {"ceil", "ceil"}, {"round", "round"}, {"trunc", "trunc"}, {"step", "step"}, {"smoothstep", "smoothstep"}, {"sign", "sign"}, {"distance", "distance"},
        {"fmod", "fmod"}, {"any", "any"}, {"all", "all"}, {"ddx", "dfdx"}, {"ddy", "dfdy"}, {"transpose", "transpose"}, {"determinant", "determinant"},
        {"noise", "hlsl_noise"}, {"clip", "hlsl_clip"}, {"sincos", "sincos"}, {0, 0}};
    for (int i = 0; same[i].h; i++) if (!strcmp(n, same[i].h)) {
        HType at = e->t;                                     /* arguments converted to the operation's type */
        int keep = !strcmp(n, "dot") || !strcmp(n, "length") || !strcmp(n, "distance") || !strcmp(n, "any") || !strcmp(n, "all") || !strcmp(n, "clip") ||
                   !strcmp(n, "noise") || !strcmp(n, "transpose") || !strcmp(n, "determinant") || !strcmp(n, "cross") || !strcmp(n, "normalize");
        if (!strcmp(n, "cross")) at = vec(3);
        if (!strcmp(n, "clip") && !g->in_ps) { gerr(g, "clip() in a vertex shader"); return; }
        bp(b, "%s(", same[i].m);
        for (int k = 0; k < na; k++) {
            if (k) bp(b, ", ");
            if (!strcmp(n, "cross")) exc(g, b, a[k], vec(3));
            else if (keep || a[k]->t.mat) ex(g, b, a[k]);
            else exc(g, b, a[k], at);
        }
        bp(b, ")"); return;
    }
    if (!strcmp(n, "dot") && na == 2) {
        int c = a[0]->t.cols < a[1]->t.cols ? a[0]->t.cols : a[1]->t.cols; if (c < 2) { bp(b, "("); ex(g, b, a[0]); bp(b, " * "); ex(g, b, a[1]); bp(b, ")"); return; }
        bp(b, "dot("); exc(g, b, a[0], vec(c)); bp(b, ", "); exc(g, b, a[1], vec(c)); bp(b, ")"); return;
    }
    (void)tb;
    gerr(g, "unsupported function %s()", n);
}
static void ex(Gen *g, B *b, HExpr *e)
{
    char tb[80];
    switch (e->k) {
    case E_NUM: num(b, e->num, e->t.base); return;
    case E_GVAR: {
        HVar *v = &g->fx->v[e->idx];
        if (v->t.base == HB_SAMPLER) { gerr(g, "sampler '%s' used as a value", v->name); return; }
        if (v->is_static) bp(b, "k_%s", v->name); else bp(b, "g.%s", v->name);
        return;
    }
    case E_LVAR: bp(b, "l_%s", e->name); return;
    case E_FIELD: bp(b, "("); ex(g, b, e->a); bp(b, ").m_%s", g->fx->s[e->a->t.st].f[e->idx].name); return;
    case E_SWZ:
        if (is_scalar(e->a->t)) { if (strlen(e->name) == 1) { bp(b, "("); ex(g, b, e->a); bp(b, ")"); } else { bp(b, "%s(", tn(g, e->t, tb)); ex(g, b, e->a); bp(b, ")"); } }
        else { bp(b, "("); ex(g, b, e->a); bp(b, ").%s", e->name); }
        return;
    case E_INDEX: bp(b, "("); ex(g, b, e->a); bp(b, ")[int("); ex(g, b, e->b); bp(b, ")]"); return;
    case E_CALL: {
        HFunc *f = &g->fx->f[e->idx];
        bp(b, "f_%s(", f->name);
        for (int i = 0; i < e->nargs && i < f->np; i++) {
            if (i) bp(b, ", ");
            if (f->p[i].t.base == HB_SAMPLER) sampler_args(g, b, e->args[i]);
            else if (f->p[i].dir & 2) ex(g, b, e->args[i]);
            else exc(g, b, e->args[i], f->p[i].t);
        }
        bp(b, "%sg", f->np ? ", " : "");
        for (int v = 0; v < g->fx->nv; v++) if (g->samp_of[(size_t)e->idx * (size_t)g->fx->nv + (size_t)v]) bp(b, ", t_%s, s_%s", g->fx->v[v].name, g->fx->v[v].name);
        bp(b, ")"); return;
    }
    case E_INTR: intrinsic(g, b, e); return;
    case E_CTOR:
        bp(b, "%s(", tn(g, e->t, tb));            /* matrices too: rows are the transposed form's columns */
        for (int i = 0; i < e->nargs; i++) { if (i) bp(b, ", "); if (e->args[i]->t.base != e->t.base && is_vector(e->args[i]->t)) { HType c = e->args[i]->t; c.base = e->t.base; exc(g, b, e->args[i], c); } else ex(g, b, e->args[i]); }
        bp(b, ")"); return;
    case E_CAST: exc(g, b, e->a, e->t); return;
    case E_UN: bp(b, "(%s", e->op); ex(g, b, e->a); bp(b, ")"); return;
    case E_POST: bp(b, "("); ex(g, b, e->a); bp(b, ")%s", e->op); return;
    case E_BIN: {
        if (!strcmp(e->op, ",")) { bp(b, "("); ex(g, b, e->a); bp(b, ", "); ex(g, b, e->b); bp(b, ")"); return; }
        if (!strcmp(e->op, "&&") || !strcmp(e->op, "||")) { bp(b, "("); exc(g, b, e->a, ty(HB_BOOL, 1, 1, 0)); bp(b, " %s ", e->op); exc(g, b, e->b, ty(HB_BOOL, 1, 1, 0)); bp(b, ")"); return; }
        HType ot = widest(e->a->t, e->b->t); if (ot.base == HB_BOOL) ot.base = HB_INT;
        if (ot.mat && (e->a->t.mat && e->b->t.mat) && e->op[0] == '*') { gerr(g, "component-wise matrix * matrix"); return; }
        if (e->op[0] == '%' && ot.base == HB_FLOAT) { bp(b, "fmod("); exc(g, b, e->a, ot); bp(b, ", "); exc(g, b, e->b, ot); bp(b, ")"); return; }
        bp(b, "(");
        if (ot.mat) ex(g, b, e->a); else exc(g, b, e->a, ot);
        bp(b, " %s ", e->op);
        if (ot.mat) ex(g, b, e->b); else exc(g, b, e->b, ot);
        bp(b, ")"); return;
    }
    case E_ASSIGN:
        ex(g, b, e->a); bp(b, " %s ", e->op);
        if (e->op[0] != '=' && is_scalar(e->b->t)) ex(g, b, e->b);           /* v *= s */
        else exc(g, b, e->b, e->a->t);
        return;
    case E_COND: bp(b, "(("); exc(g, b, e->a, ty(HB_BOOL, 1, 1, 0)); bp(b, ") ? "); exc(g, b, e->b, e->t); bp(b, " : "); exc(g, b, e->c, e->t); bp(b, ")"); return;
    case E_INIT: gerr(g, "initializer list in an unsupported place"); return;
    }
}

static void ind(B *b, int d) { for (int i = 0; i < d; i++) bp(b, "    "); }
static void decl(Gen *g, B *b, HType t, const char *name, HExpr *init)
{
    char tb[80]; HType et = t; et.arr = 0;
    bp(b, "%s l_%s", tn(g, et, tb), name);
    if (t.arr) bp(b, "[%d]", t.arr);
    if (!init) { if (t.base != HB_STRUCT && !t.arr) bp(b, " = %s(0)", tn(g, et, tb)); else bp(b, " = {}"); return; }
    if (t.arr) {                                            /* {e0, e1, ...}: elements converted */
        if (init->k != E_INIT) { bp(b, " = "); ex(g, b, init); return; }
        bp(b, " = {");
        int per = ncomp(et), k = 0;
        int flat = init->nargs == t.arr * per && per > 1;   /* a flat list of scalars for an array of vectors */
        for (int i = 0; i < t.arr && k < init->nargs; i++) {
            if (i) bp(b, ", ");
            if (flat) { bp(b, "%s(", tn(g, et, tb)); for (int c = 0; c < per; c++) { if (c) bp(b, ", "); ex(g, b, init->args[k++]); } bp(b, ")"); }
            else exc(g, b, init->args[k++], et);
        }
        bp(b, "}"); return;
    }
    bp(b, " = "); exc(g, b, init, t);
}
static void stmt_gen(Gen *g, B *b, HStmt *s, int d, HType ret)
{
    if (!s) return;
    switch (s->k) {
    case S_BLOCK:
        if (s->declist) { for (int i = 0; i < s->n; i++) stmt_gen(g, b, s->list[i], d, ret); return; }   /* a declaration list */
        ind(b, d); bp(b, "{\n"); for (int i = 0; i < s->n; i++) stmt_gen(g, b, s->list[i], d + 1, ret); ind(b, d); bp(b, "}\n"); return;
    case S_DECL: ind(b, d); decl(g, b, s->t, s->name, s->e); bp(b, ";\n"); return;
    case S_EXPR: if (!s->e) return; ind(b, d); ex(g, b, s->e); bp(b, ";\n"); return;
    case S_RET: ind(b, d); if (s->e) { bp(b, "return "); exc(g, b, s->e, ret); bp(b, ";\n"); } else bp(b, "return;\n"); return;
    case S_DISCARD: ind(b, d); bp(b, g->in_ps ? "discard_fragment();\n" : ";\n"); return;
    case S_BREAK: ind(b, d); bp(b, "break;\n"); return;
    case S_CONTINUE: ind(b, d); bp(b, "continue;\n"); return;
    case S_IF:
        ind(b, d); bp(b, "if ("); exc(g, b, s->e, ty(HB_BOOL, 1, 1, 0)); bp(b, ")\n");
        stmt_gen(g, b, s->a, d + (s->a->k == S_BLOCK ? 0 : 1), ret);
        if (s->b) { ind(b, d); bp(b, "else\n"); stmt_gen(g, b, s->b, d + (s->b->k == S_BLOCK ? 0 : 1), ret); }
        return;
    case S_WHILE: ind(b, d); bp(b, "while ("); exc(g, b, s->e, ty(HB_BOOL, 1, 1, 0)); bp(b, ")\n"); stmt_gen(g, b, s->a, d + (s->a->k == S_BLOCK ? 0 : 1), ret); return;
    case S_FOR:
        ind(b, d); bp(b, "for (");
        if (s->init) {
            if (s->init->k == S_DECL) decl(g, b, s->init->t, s->init->name, s->init->e);
            else if (s->init->k == S_EXPR && s->init->e) ex(g, b, s->init->e);
            else if (s->init->k == S_BLOCK && s->init->n && s->init->list[0]->k == S_DECL) {   /* int i = 0, j = 1 */
                HStmt *d0 = s->init->list[0]; decl(g, b, d0->t, d0->name, d0->e);
                for (int i = 1; i < s->init->n; i++) { bp(b, ", l_%s", s->init->list[i]->name); if (s->init->list[i]->e) { bp(b, " = "); exc(g, b, s->init->list[i]->e, s->init->list[i]->t); } }
            }
        }
        bp(b, "; "); if (s->e) exc(g, b, s->e, ty(HB_BOOL, 1, 1, 0)); bp(b, "; "); if (s->e2) ex(g, b, s->e2); bp(b, ")\n");
        stmt_gen(g, b, s->a, d + (s->a->k == S_BLOCK ? 0 : 1), ret); return;
    }
}

/* ---- what a program uses ---- */
static void use_expr(Gen *g, HExpr *e, int fn);
static void use_func(Gen *g, int f);
static void use_stmt(Gen *g, HStmt *s, int fn)
{
    if (!s) return;
    use_expr(g, s->e, fn); use_expr(g, s->e2, fn); use_stmt(g, s->a, fn); use_stmt(g, s->b, fn); use_stmt(g, s->init, fn);
    for (int i = 0; i < s->n; i++) use_stmt(g, s->list[i], fn);
}
static void use_expr(Gen *g, HExpr *e, int fn)
{
    if (!e) return;
    int nv = g->fx->nv;
    if (e->k == E_GVAR) { g->gused[e->idx] = 1; if (g->fx->v[e->idx].t.base == HB_SAMPLER && fn >= 0) g->samp_of[(size_t)fn * (size_t)nv + (size_t)e->idx] = 1; }
    if (e->k == E_CALL) {
        use_func(g, e->idx);
        if (fn >= 0) for (int v = 0; v < nv; v++) if (g->samp_of[(size_t)e->idx * (size_t)nv + (size_t)v]) g->samp_of[(size_t)fn * (size_t)nv + (size_t)v] = 1;
    }
    if (e->k == E_INTR && !strncmp(e->name, "tex", 3) && e->nargs && e->args[0]->k == E_GVAR) {
        int sk = !strcmp(e->name, "texCUBE") ? HS_CUBE : !strcmp(e->name, "tex3D") ? HS_3D : HS_2D; g->samp_kind[e->args[0]->idx] = (uint8_t)sk;
    }
    use_expr(g, e->a, fn); use_expr(g, e->b, fn); use_expr(g, e->c, fn);
    for (int i = 0; i < e->nargs; i++) use_expr(g, e->args[i], fn);
}
static void use_func(Gen *g, int f)
{
    if (f < 0 || g->fused[f]) return;
    g->fused[f] = 1; use_stmt(g, g->fx->f[f].body, f);
}
static int sampler_texture_kind(Gen *g, int v) { int k = g->samp_kind[v]; if (!k) k = g->fx->v[v].t.samp; return k ? k : HS_2D; }
static const char *texty(int k) { return k == HS_CUBE ? "texturecube<float>" : k == HS_3D ? "texture3d<float>" : "texture2d<float>"; }

/* ---- semantics ---- */
static int sem_split(const char *s, char *name, size_t cap)
{
    size_t n = strlen(s), k = n; while (k > 0 && isdigit((unsigned char)s[k - 1])) k--;
    snprintf(name, cap, "%.*s", (int)k, s); for (char *c = name; *c; c++) *c = (char)toupper((unsigned char)*c);
    return k < n ? atoi(s + k) : 0;
}
/* vertex attributes by semantic (every attribute a float4; d3d9.c gathers vertices in this layout) */
static int attr_of(const char *sem)
{
    char n[48]; int i = sem_split(sem, n, sizeof n);
    if (!strcmp(n, "POSITION") || !strcmp(n, "SV_POSITION")) return 0;
    if (!strcmp(n, "NORMAL")) return 1;
    if (!strcmp(n, "COLOR") || !strcmp(n, "DIFFUSE")) return i ? 3 : 2;
    if (!strcmp(n, "TEXCOORD") && i < 8) return 4 + i;
    if (!strcmp(n, "TANGENT")) return 12;
    if (!strcmp(n, "BINORMAL")) return 13;
    if (!strcmp(n, "BLENDWEIGHT")) return 14;
    if (!strcmp(n, "BLENDINDICES")) return 15;
    return -1;
}
/* a float4 source converted to type t (the shader's declared input) */
static void from_f4(Gen *g, B *b, const char *src, HType t)
{
    char tb[80];
    if (t.mat) { gerr(g, "matrix-typed input"); return; }
    if (is_scalar(t)) bp(b, "%s((%s).x)", bname(t.base), src);
    else if (t.cols == 4) { if (t.base == HB_FLOAT) bp(b, "%s", src); else bp(b, "%s(%s)", tn(g, t, tb), src); }
    else bp(b, "%s((%s).%.*s)", tn(g, t, tb), src, t.cols, SW);
}
static void to_f4(Gen *g, B *b, const char *src, HType t, const char *pad)
{
    if (is_scalar(t)) bp(b, "float4(float(%s), %s)", src, pad);
    else if (t.cols == 4) bp(b, "float4(%s)", src);
    else if (t.cols == 3) bp(b, "float4(float3(%s), 0)", src);
    else if (t.cols == 2) bp(b, "float4(float2(%s), 0, 0)", src);
    (void)g;
}

/* ---- program assembly ---- */
static const char PRELUDE[] =
"#include <metal_stdlib>\nusing namespace metal;\n"
"struct FxU {                         // fixed-function state for effect draws (filled by the renderer)\n"
"    float4x4 wvp; float4x4 wv;\n"
"    float4 vp; float4 fog; float4 fogcolor; float4 tfactor; float4 at;   // fog: start, end, enabled; at: ref, func, enabled\n"
"    int4 misc;                       // x: pretransformed vertices\n"
"    int4 cop[8];                     // per stage: colorop, colorarg1, colorarg2, alphaop\n"
"    int4 aop[8];                     // per stage: alphaarg1, alphaarg2, -, texcoordindex\n"
"    int4 tex[8];                     // per stage: texture bound, texture without alpha\n"
"};\n"
"struct VIn { float4 a0 [[attribute(0)]]; float4 a1 [[attribute(1)]]; float4 a2 [[attribute(2)]]; float4 a3 [[attribute(3)]];\n"
"             float4 a4 [[attribute(4)]]; float4 a5 [[attribute(5)]]; float4 a6 [[attribute(6)]]; float4 a7 [[attribute(7)]];\n"
"             float4 a8 [[attribute(8)]]; float4 a9 [[attribute(9)]]; float4 a10 [[attribute(10)]]; float4 a11 [[attribute(11)]];\n"
"             float4 a12 [[attribute(12)]]; float4 a13 [[attribute(13)]]; float4 a14 [[attribute(14)]]; float4 a15 [[attribute(15)]]; };\n"
"struct V2F { float4 pos [[position]]; float4 c0; float4 c1; float4 t0; float4 t1; float4 t2; float4 t3; float4 t4; float4 t5; float4 t6; float4 t7; float fog; };\n"
"static V2F v2f_default() { V2F o; o.pos = float4(0); o.c0 = float4(1); o.c1 = float4(0); o.t0 = o.t1 = o.t2 = o.t3 = o.t4 = o.t5 = o.t6 = o.t7 = float4(0); o.fog = 1; return o; }\n"
"static float4 hlsl_x1(bool xrgb, float4 c) { if (xrgb) c.a = 1; return c; }   // a texture without alpha: alpha reads as 1\n"
"static void hlsl_clip(float x) { if (x < 0) discard_fragment(); }\n"
"static void hlsl_clip(float2 x) { if (any(x < 0)) discard_fragment(); }\n"
"static void hlsl_clip(float3 x) { if (any(x < 0)) discard_fragment(); }\n"
"static void hlsl_clip(float4 x) { if (any(x < 0)) discard_fragment(); }\n"
"static float hlsl_hash(float3 p) { p = fract(p * 0.3183099 + 0.1); p *= 17.0; return fract(p.x * p.y * p.z * (p.x + p.y + p.z)) * 2 - 1; }\n"
"static float hlsl_noise(float3 x) { float3 i = floor(x), f = fract(x); f = f * f * (3 - 2 * f);\n"
"    return mix(mix(mix(hlsl_hash(i), hlsl_hash(i + float3(1,0,0)), f.x), mix(hlsl_hash(i + float3(0,1,0)), hlsl_hash(i + float3(1,1,0)), f.x), f.y),\n"
"               mix(mix(hlsl_hash(i + float3(0,0,1)), hlsl_hash(i + float3(1,0,1)), f.x), mix(hlsl_hash(i + float3(0,1,1)), hlsl_hash(i + float3(1,1,1)), f.x), f.y), f.z); }\n"
"static float hlsl_noise(float2 x) { return hlsl_noise(float3(x, 0)); }\n"
"static float hlsl_noise(float x) { return hlsl_noise(float3(x, 0, 0)); }\n"
"static float hlsl_noise(float4 x) { return hlsl_noise(x.xyz); }\n"
"static float4 ffp_arg(int a, float4 diffuse, float4 current, float4 texel, float4 tf, float4 spec)\n"
"{\n"
"    float4 v;\n"
"    switch (a & 0xf) { case 0: v = diffuse; break; case 1: v = current; break; case 2: v = texel; break; case 3: v = tf; break; case 4: v = spec; break; default: v = diffuse; }\n"
"    if (a & 0x10) v = 1.0 - v;\n"
"    if (a & 0x20) v = float4(v.a);\n"
"    return v;\n"
"}\n"
"static float4 ffp_op(int o, float4 x, float4 y, float4 diffuse, float4 texel, float4 tf, float4 current)\n"
"{\n"
"    switch (o) {\n"
"    case 2: return x; case 3: return y; case 4: return x * y; case 5: return saturate(x * y * 2); case 6: return saturate(x * y * 4);\n"
"    case 7: return saturate(x + y); case 8: return saturate(x + y - 0.5); case 9: return saturate((x + y - 0.5) * 2); case 10: return saturate(x - y);\n"
"    case 11: return saturate(x + y - x * y); case 12: return mix(y, x, diffuse.a); case 13: return mix(y, x, texel.a); case 14: return mix(y, x, tf.a);\n"
"    case 15: return saturate(x + y * (1 - texel.a)); case 16: return mix(y, x, current.a);\n"
"    case 24: { float3 d = (x.rgb - 0.5) * (y.rgb - 0.5); return float4(saturate(4 * (d.r + d.g + d.b))); }   // DOTPRODUCT3\n"
"    default: return x;\n"
"    }\n"
"}\n"
"static float4 ffp_finish(float4 c, float fogf, constant FxU &u)   // alpha test, then fog\n"
"{\n"
"    if (u.at.z != 0) { float a = c.a, r = u.at.x; int f = int(u.at.y); bool pass;\n"
"        switch (f) { case 1: pass = false; break; case 2: pass = a < r; break; case 3: pass = a == r; break; case 4: pass = a <= r; break;\n"
"                     case 5: pass = a > r; break; case 6: pass = a != r; break; case 7: pass = a >= r; break; default: pass = true; }\n"
"        if (!pass) discard_fragment(); }\n"
"    if (u.fog.z != 0) c.rgb = mix(u.fogcolor.rgb, c.rgb, saturate(fogf));\n"
"    return c;\n"
"}\n";

static void emit_const(Gen *g, B *b, HVar *v)      /* a static global as a Metal constant, from its folded value */
{
    char tb[80]; HType et = v->t; et.arr = 0; int per = hlsl_type_floats(g->fx, et), n = v->t.arr ? v->t.arr : 1;
    if (et.base == HB_STRUCT || et.base >= HB_SAMPLER) { gerr(g, "static %s of unsupported type", v->name); return; }
    if (!v->defval) hlsl_eval_init(g->fx, v);
    bp(b, "constant %s k_%s", tn(g, et, tb), v->name); if (v->t.arr) bp(b, "[%d]", v->t.arr);
    bp(b, " = "); if (v->t.arr) bp(b, "{");
    for (int i = 0; i < n; i++) {
        if (i) bp(b, ", ");
        const float *f = v->defval + i * per;
        if (per == 1) { num(b, f[0], et.base); continue; }
        bp(b, "%s(", tn(g, et, tb));
        if (et.mat) for (int r = 0; r < et.rows; r++) { if (r) bp(b, ", "); bp(b, "float%d(", et.cols); for (int c = 0; c < et.cols; c++) { if (c) bp(b, ", "); num(b, f[r * et.cols + c], HB_FLOAT); } bp(b, ")"); }
        else for (int c = 0; c < per; c++) { if (c) bp(b, ", "); num(b, f[c], et.base); }
        bp(b, ")");
    }
    if (v->t.arr) bp(b, "}");
    bp(b, ";\n");
}
static void load_reg(Gen *g, B *b, const char *dst, HType t, int reg)   /* one element of a parameter from its registers */
{
    char tb[80];
    if (t.mat) { bp(b, "    %s = %s(", dst, tn(g, t, tb)); for (int r = 0; r < t.rows; r++) bp(b, "%sR[%d].%.*s", r ? ", " : "", reg + r, t.cols, SW); bp(b, ");\n"); }
    else if (is_scalar(t)) bp(b, t.base == HB_BOOL ? "    %s = R[%d].x != 0;\n" : t.base == HB_INT ? "    %s = int(R[%d].x);\n" : "    %s = R[%d].x;\n", dst, reg);
    else bp(b, "    %s = %s(R[%d].%.*s);\n", dst, tn(g, t, tb), reg, t.cols, SW);
}
static int is_param(HVar *v) { return !v->is_static && v->t.base <= HB_FLOAT; }
static void emit_func(Gen *g, B *b, int fi)
{
    HFunc *f = &g->fx->f[fi]; char tb[80]; int nv = g->fx->nv;
    bp(b, "static %s f_%s(", tn(g, f->ret, tb), f->name);
    for (int i = 0; i < f->np; i++) {
        HParam *p = &f->p[i]; HType et = p->t; et.arr = 0;
        if (i) bp(b, ", ");
        if (p->t.base == HB_SAMPLER) { bp(b, "%s t_l_%s, sampler s_l_%s", texty(p->t.samp ? p->t.samp : HS_2D), p->name, p->name); continue; }
        bp(b, (p->dir & 2) ? "thread %s &l_%s" : "%s l_%s", tn(g, et, tb), p->name);
        if (p->t.arr) bp(b, "[%d]", p->t.arr);
    }
    bp(b, "%sthread const G &g", f->np ? ", " : "");
    for (int v = 0; v < nv; v++) if (g->samp_of[(size_t)fi * (size_t)nv + (size_t)v]) bp(b, ", %s t_%s, sampler s_%s", texty(sampler_texture_kind(g, v)), g->fx->v[v].name, g->fx->v[v].name);
    bp(b, ")\n");
    stmt_gen(g, b, f->body, 0, f->ret);
}
/* the compile arguments of a shader assignment that fill its uniform parameters, in order */
static void entry_args(Gen *g, B *b, HFunc *f, HState *s, const char *kind)
{
    char tb[80]; int u = 0;
    for (int i = 0; i < f->np; i++) {
        HParam *p = &f->p[i]; HType et = p->t; et.arr = 0;
        if (p->is_uniform) {
            bp(b, "    %s a%d = ", tn(g, et, tb), i);
            if (u < s->nargs) exc(g, b, s->args[u++], p->t); else bp(b, "%s(0)", tn(g, et, tb));
            bp(b, ";\n"); continue;
        }
        if (p->t.base == HB_STRUCT) {
            HStruct *st = &g->fx->s[p->t.st];
            bp(b, "    %s a%d = {};\n", tn(g, et, tb), i);
            if (p->dir & 1) for (int k = 0; k < st->nf; k++) {
                char src[64]; int at = -1; char sn[48];
                if (!strcmp(kind, "vs")) { at = attr_of(st->f[k].semantic); if (at < 0) continue; snprintf(src, sizeof src, "in.a%d", at); }
                else { int idx = sem_split(st->f[k].semantic, sn, sizeof sn);
                       if (!strcmp(sn, "TEXCOORD")) snprintf(src, sizeof src, "in.t%d", idx & 7); else if (!strcmp(sn, "COLOR")) snprintf(src, sizeof src, "in.c%d", idx & 1);
                       else if (!strcmp(sn, "VPOS")) snprintf(src, sizeof src, "in.pos"); else continue; }
                bp(b, "    a%d.m_%s = ", i, st->f[k].name); from_f4(g, b, src, st->f[k].t); bp(b, ";\n");
            }
            continue;
        }
        bp(b, "    %s a%d = ", tn(g, et, tb), i);
        if (p->dir & 1) {
            char src[64]; char sn[48]; int ok = 1;
            if (!strcmp(kind, "vs")) { int at = attr_of(p->semantic); if (at < 0) ok = 0; else snprintf(src, sizeof src, "in.a%d", at); }
            else { int idx = sem_split(p->semantic, sn, sizeof sn);
                   if (!strcmp(sn, "TEXCOORD")) snprintf(src, sizeof src, "in.t%d", idx & 7); else if (!strcmp(sn, "COLOR")) snprintf(src, sizeof src, "in.c%d", idx & 1);
                   else if (!strcmp(sn, "VPOS")) snprintf(src, sizeof src, "in.pos"); else ok = 0; }
            if (ok) from_f4(g, b, src, p->t); else bp(b, "%s(0)", tn(g, et, tb));
        } else bp(b, "%s(0)", tn(g, et, tb));
        bp(b, ";\n");
    }
}
/* a shader output with its semantic into the interface (vertex shaders) */
static void vs_out(Gen *g, B *b, const char *sem, const char *val, HType t)
{
    char sn[48]; int idx = sem_split(sem, sn, sizeof sn);
    if (!strcmp(sn, "POSITION") || !strcmp(sn, "SV_POSITION")) { bp(b, "    o.pos = "); if (t.cols == 4) bp(b, "%s", val); else to_f4(g, b, val, t, "0, 0, 1"); bp(b, ";\n"); }
    else if (!strcmp(sn, "COLOR") || !strcmp(sn, "DIFFUSE")) { bp(b, "    o.c%d = saturate(", idx & 1); if (t.cols == 3) bp(b, "float4(%s, 1)", val); else to_f4(g, b, val, t, "0, 0, 1"); bp(b, ");\n"); }
    else if (!strcmp(sn, "TEXCOORD")) { bp(b, "    o.t%d = ", idx & 7); to_f4(g, b, val, t, "0, 0, 0"); bp(b, ";\n"); }
    else if (!strcmp(sn, "FOG")) { bp(b, "    o.fog = saturate(float(%s%s));\n", val, is_scalar(t) ? "" : ".x"); }
}
static void gen_vs(Gen *g, B *b, HState *s)
{
    if (!s) {                                         /* fixed-function vertex processing */
        bp(b, "vertex V2F vs_fx(VIn in [[stage_in]], constant FxU &u [[buffer(1)]])\n{\n    V2F o = v2f_default();\n"
              "    if (u.misc.x) {\n        float w = in.a0.w != 0 ? 1.0 / in.a0.w : 1.0;\n"
              "        o.pos = float4(((in.a0.x + 0.5 - u.vp.x) / u.vp.z * 2.0 - 1.0) * w, (1.0 - (in.a0.y + 0.5 - u.vp.y) / u.vp.w * 2.0) * w, in.a0.z * w, w);\n"
              "    } else {\n        float4 p = float4(in.a0.xyz, 1.0);\n        o.pos = u.wvp * p; o.pos.x += o.pos.w / u.vp.z; o.pos.y -= o.pos.w / u.vp.w;\n"
              "        float4 cam = u.wv * p;\n        o.fog = u.fog.z != 0 ? clamp((u.fog.y - cam.z) / (u.fog.y - u.fog.x), 0.0, 1.0) : 1.0;\n    }\n"
              "    o.c0 = in.a2; o.c1 = in.a3;\n"
              "    float4 tc[8] = {in.a4, in.a5, in.a6, in.a7, in.a8, in.a9, in.a10, in.a11};\n"
              "    o.t0 = tc[u.aop[0].w & 7]; o.t1 = tc[u.aop[1].w & 7]; o.t2 = tc[u.aop[2].w & 7]; o.t3 = tc[u.aop[3].w & 7];\n"
              "    o.t4 = tc[u.aop[4].w & 7]; o.t5 = tc[u.aop[5].w & 7]; o.t6 = tc[u.aop[6].w & 7]; o.t7 = tc[u.aop[7].w & 7];\n"
              "    return o;\n}\n");
        g->pr->vs_ffp = 1; return;
    }
    int fi = find_func(g->fx, s->fn); HFunc *f = &g->fx->f[fi]; char tb[80];
    bp(b, "vertex V2F vs_fx(VIn in [[stage_in]], constant FxU &u [[buffer(1)]], constant float4 *R [[buffer(2)]])\n{\n    G g = load_g(R);\n    V2F o = v2f_default();\n");
    entry_args(g, b, f, s, "vs");
    bp(b, "    ");
    if (f->ret.base != HB_VOID) bp(b, "%s r = ", tn(g, f->ret, tb));
    bp(b, "f_%s(", f->name); for (int i = 0; i < f->np; i++) bp(b, "%sa%d", i ? ", " : "", i); bp(b, "%sg);\n", f->np ? ", " : "");
    if (f->ret.base == HB_STRUCT) { HStruct *st = &g->fx->s[f->ret.st]; for (int k = 0; k < st->nf; k++) { char v[96]; snprintf(v, sizeof v, "r.m_%s", st->f[k].name); vs_out(g, b, st->f[k].semantic, v, st->f[k].t); } }
    else if (f->ret.base != HB_VOID) vs_out(g, b, f->semantic, "r", f->ret);
    for (int i = 0; i < f->np; i++) if (f->p[i].dir & 2) {
        if (f->p[i].t.base == HB_STRUCT) { HStruct *st = &g->fx->s[f->p[i].t.st]; for (int k = 0; k < st->nf; k++) { char v[96]; snprintf(v, sizeof v, "a%d.m_%s", i, st->f[k].name); vs_out(g, b, st->f[k].semantic, v, st->f[k].t); } }
        else { char v[16]; snprintf(v, sizeof v, "a%d", i); vs_out(g, b, f->p[i].semantic, v, f->p[i].t); }
    }
    bp(b, "    o.pos.x += o.pos.w / u.vp.z; o.pos.y -= o.pos.w / u.vp.w;   // D3D's pixel centres\n    return o;\n}\n");
}
static void gen_ps(Gen *g, B *b, HState *s)
{
    if (!s) {                                         /* fixed-function texture stages */
        bp(b, "fragment float4 fs_fx(V2F in [[stage_in]], constant FxU &u [[buffer(1)]]");
        for (int k = 0; k < 8; k++) bp(b, ",\n    texture2d<float> x%d [[texture(%d)]], sampler y%d [[sampler(%d)]]", k, k, k, k);
        bp(b, ")\n{\n    float4 diffuse = in.c0, spec = in.c1, current = diffuse; bool on = true;\n"
              "    float4 tc[8] = {in.t0, in.t1, in.t2, in.t3, in.t4, in.t5, in.t6, in.t7};\n");
        for (int k = 0; k < 8; k++)
            bp(b, "    if (on && u.cop[%d].x != 1) {\n"
                  "        float4 texel = u.tex[%d].x ? x%d.sample(y%d, tc[%d].xy) : float4(1); if (u.tex[%d].y) texel.a = 1;\n"
                  "        float4 c = ffp_op(u.cop[%d].x, ffp_arg(u.cop[%d].y, diffuse, current, texel, u.tfactor, spec), ffp_arg(u.cop[%d].z, diffuse, current, texel, u.tfactor, spec), diffuse, texel, u.tfactor, current);\n"
                  "        float4 a = u.cop[%d].w == 1 ? current : ffp_op(u.cop[%d].w, ffp_arg(u.aop[%d].x, diffuse, current, texel, u.tfactor, spec), ffp_arg(u.aop[%d].y, diffuse, current, texel, u.tfactor, spec), diffuse, texel, u.tfactor, current);\n"
                  "        current = float4(c.rgb, a.a);\n    } else on = false;\n", k, k, k, k, k, k, k, k, k, k, k, k, k);
        bp(b, "    return ffp_finish(current, in.fog, u);\n}\n");
        g->pr->ps_ffp = 1; g->pr->nsamp = 8; for (int k = 0; k < 8; k++) g->pr->sslot[k] = -1;
        return;
    }
    int fi = find_func(g->fx, s->fn); HFunc *f = &g->fx->f[fi]; char tb[80]; int nv = g->fx->nv;
    bp(b, "fragment float4 fs_fx(V2F in [[stage_in]], constant FxU &u [[buffer(1)]], constant float4 *R [[buffer(2)]]");
    for (int k = 0; k < g->pr->nsamp; k++) { int v = g->pr->sslot[k]; if (v < 0) continue; bp(b, ",\n    %s t_%s [[texture(%d)]], sampler s_%s [[sampler(%d)]]", texty(sampler_texture_kind(g, v)), g->fx->v[v].name, k, g->fx->v[v].name, k); }
    bp(b, ")\n{\n    G g = load_g(R);\n");
    for (int k = 0; k < g->pr->nsamp; k++) { int v = g->pr->sslot[k]; if (v >= 0) bp(b, "    g.xr_%s = u.tex[%d].y != 0;\n", g->fx->v[v].name, k); }
    g->in_ps = 1; entry_args(g, b, f, s, "ps"); g->in_ps = 0;
    bp(b, "    %s r = f_%s(", tn(g, f->ret, tb), f->name); for (int i = 0; i < f->np; i++) bp(b, "%sa%d", i ? ", " : "", i);
    bp(b, "%sg", f->np ? ", " : "");
    for (int v = 0; v < nv; v++) if (g->samp_of[(size_t)fi * (size_t)nv + (size_t)v]) bp(b, ", t_%s, s_%s", g->fx->v[v].name, g->fx->v[v].name);
    bp(b, ");\n");
    if (f->ret.base == HB_STRUCT) {
        HStruct *st = &g->fx->s[f->ret.st]; int k0 = -1; char sn[48];
        for (int k = 0; k < st->nf; k++) { int idx = sem_split(st->f[k].semantic, sn, sizeof sn); if ((!strcmp(sn, "COLOR") || !strcmp(sn, "SV_TARGET")) && idx == 0) k0 = k; }
        if (k0 < 0) { gerr(g, "pixel shader %s has no COLOR0 output", f->name); return; }
        char v[96]; snprintf(v, sizeof v, "r.m_%s", st->f[k0].name); bp(b, "    float4 c = "); if (st->f[k0].t.cols == 3) bp(b, "float4(%s, 1)", v); else to_f4(g, b, v, st->f[k0].t, "0, 0, 1"); bp(b, ";\n");
    } else { bp(b, "    float4 c = "); if (f->ret.cols == 3) bp(b, "float4(r, 1)"); else to_f4(g, b, "r", f->ret, "0, 0, 1"); bp(b, ";\n"); }
    if (getenv("HLSL_MAGENTA")) bp(b, "    return float4(1, 0, 1, 1);   // HLSL_MAGENTA (development)\n}\n");
    else bp(b, "    return ffp_finish(c, in.fog, u);\n}\n");
}
static HState *shader_state(HPass *ps, const char *name)
{
    for (int i = 0; i < ps->nst; i++) if (!strcasecmp(ps->st[i].name, name)) return &ps->st[i];
    return NULL;
}
HProgram *hlsl_compile_pass(HFx *fx, int tech, int pass)
{
    HProgram *pr = calloc(1, sizeof *pr); if (!pr) return NULL;
    if (tech < 0 || tech >= fx->nt || pass < 0 || pass >= fx->t[tech].npass) { snprintf(pr->err, sizeof pr->err, "no such pass"); return pr; }
    HPass *ps = &fx->t[tech].pass[pass];
    HState *vs = shader_state(ps, "VertexShader"), *px = shader_state(ps, "PixelShader");
    if (vs && !vs->fn[0]) vs = NULL;                  /* VertexShader = NULL */
    if (px && !px->fn[0]) px = NULL;
    Gen g = {0}; g.fx = fx; g.pr = pr; int nv = fx->nv, nf = fx->nf;
    g.gused = calloc((size_t)nv + 1, 1); g.fused = calloc((size_t)nf + 1, 1); g.samp_of = calloc((size_t)(nf + 1) * (size_t)(nv + 1), 1); g.samp_kind = calloc((size_t)nv + 1, 1);
    pr->preg = malloc(sizeof(int) * (size_t)(nv + 1));
    int fvs = vs ? find_func(fx, vs->fn) : -1, fps = px ? find_func(fx, px->fn) : -1;
    if (vs && fvs < 0) gerr(&g, "no vertex shader function %s", vs->fn);
    if (px && fps < 0) gerr(&g, "no pixel shader function %s", px->fn);
    if (vs && fvs >= 0) { use_func(&g, fvs); for (int i = 0; i < vs->nargs; i++) use_expr(&g, vs->args[i], -1); }
    if (px && fps >= 0) { use_func(&g, fps); for (int i = 0; i < px->nargs; i++) use_expr(&g, px->args[i], -1); }
    /* every parameter gets fixed registers (the same layout in every pass of the effect) */
    int reg = 0;
    for (int i = 0; i < nv; i++) { pr->preg[i] = -1; if (is_param(&fx->v[i])) { pr->preg[i] = reg; reg += hlsl_type_regs(fx, fx->v[i].t); } }
    pr->nregs = reg;
    if (px && fps >= 0) {                             /* the pixel shader's samplers, in slots (their register(sN) if declared) */
        int used[16] = {0};
        for (int v = 0; v < nv; v++) if (g.samp_of[(size_t)fps * (size_t)nv + (size_t)v] && fx->v[v].reg_kind == 's' && fx->v[v].reg < 16) { pr->sslot[fx->v[v].reg] = v + 1; used[fx->v[v].reg] = 1; }
        for (int v = 0; v < nv; v++) if (g.samp_of[(size_t)fps * (size_t)nv + (size_t)v] && !(fx->v[v].reg_kind == 's' && fx->v[v].reg < 16)) {
            int k = 0; while (k < 16 && used[k]) k++; if (k == 16) { gerr(&g, "too many samplers"); break; }
            pr->sslot[k] = v + 1; used[k] = 1;
        }
        int n = 0; for (int k = 0; k < 16; k++) if (used[k]) n = k + 1;
        for (int k = 0; k < 16; k++) { pr->sslot[k] = used[k] ? pr->sslot[k] - 1 : -1; pr->skind[k] = pr->sslot[k] >= 0 ? sampler_texture_kind(&g, pr->sslot[k]) : HS_2D; }
        pr->nsamp = n;
    }
    B b = {0}; char tb[80];
    bp(&b, "%s", PRELUDE);
    for (int s = 0; s < fx->ns; s++) {
        bp(&b, "struct s_%s {", fx->s[s].name);
        for (int k = 0; k < fx->s[s].nf; k++) { HType et = fx->s[s].f[k].t; et.arr = 0; bp(&b, " %s m_%s", tn(&g, et, tb), fx->s[s].f[k].name); if (fx->s[s].f[k].t.arr) bp(&b, "[%d]", fx->s[s].f[k].t.arr); bp(&b, ";"); }
        bp(&b, " };\n");
    }
    for (int i = 0; i < nv; i++) if (g.gused[i] && fx->v[i].is_static) emit_const(&g, &b, &fx->v[i]);
    bp(&b, "struct G { int unused_;");
    for (int i = 0; i < nv; i++) if (fx->v[i].t.base == HB_SAMPLER) bp(&b, " bool xr_%s;", fx->v[i].name);
    for (int i = 0; i < nv; i++) if (g.gused[i] && is_param(&fx->v[i])) { HType et = fx->v[i].t; et.arr = 0; bp(&b, " %s %s", tn(&g, et, tb), fx->v[i].name); if (fx->v[i].t.arr) bp(&b, "[%d]", fx->v[i].t.arr); bp(&b, ";"); }
    bp(&b, " };\nstatic G load_g(constant float4 *R)\n{\n    G g; g.unused_ = 0;\n");
    for (int i = 0; i < nv; i++) if (fx->v[i].t.base == HB_SAMPLER) bp(&b, "    g.xr_%s = false;\n", fx->v[i].name);
    for (int i = 0; i < nv; i++) if (g.gused[i] && is_param(&fx->v[i])) {
        HType et = fx->v[i].t; et.arr = 0; int per = hlsl_type_regs(fx, et);
        if (et.base == HB_STRUCT) { gerr(&g, "struct parameter %s", fx->v[i].name); continue; }
        if (!fx->v[i].t.arr) { char d[96]; snprintf(d, sizeof d, "g.%s", fx->v[i].name); load_reg(&g, &b, d, et, pr->preg[i]); }
        else for (int k = 0; k < fx->v[i].t.arr; k++) { char d[96]; snprintf(d, sizeof d, "g.%s[%d]", fx->v[i].name, k); load_reg(&g, &b, d, et, pr->preg[i] + k * per); }
    }
    bp(&b, "    return g;\n}\n");
    for (int f = 0; f < nf; f++) if (g.fused[f] && fx->f[f].body) { g.in_ps = 0; if (fps >= 0) g.in_ps = 1; emit_func(&g, &b, f); }
    gen_vs(&g, &b, vs);
    gen_ps(&g, &b, px);
    pr->msl = b.s; pr->msl_len = b.n;
    if (g.err[0]) snprintf(pr->err, sizeof pr->err, "%s", g.err);
    free(g.gused); free(g.fused); free(g.samp_of); free(g.samp_kind);
    return pr;
}
void hlsl_program_free(HProgram *p) { if (!p) return; free(p->msl); free(p->preg); free(p); }
