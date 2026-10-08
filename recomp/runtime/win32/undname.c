/* MSVC C++ name demangler producing UnDecorateSymbolName's (dbghelp / undname) output for the forms used by the game's
 * exports (functions, static data, templates, pointers/references, back-references). FuBi parses these strings to
 * bind script functions, so the text must match exactly, including undname's spacing (e.g. "(void)const "). */
#include "w32.h"
#include <stdarg.h>

typedef struct { const char *p; char *names[64]; int nn, start; char *args[10]; int na; int err; } Und;
static char *dup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static char *fmt(const char *f, ...) { va_list ap; va_start(ap, f); char *s = 0; vasprintf(&s, f, ap); va_end(ap); return s; }
static char *cat_free(char *a, char *b) { char *r = fmt("%s%s", a, b); free(a); free(b); return r; }

static char *parse_type(Und *u, int in_args);
static char *qualified_name(Und *u, int skip_first_backref_store);
static char *template_args(Und *u)
{
    char *s = dup(""); int first = 1;
    while (*u->p && *u->p != '@') {
        char *t;
        if (u->p[0] == '$' && u->p[1] == '0') {                      /* integer constant */
            u->p += 2; int neg = 0; if (*u->p == '?') { neg = 1; u->p++; }
            long v = 0;
            if (*u->p >= '0' && *u->p <= '9') v = *u->p++ - '0' + 1;
            else { while (*u->p && *u->p != '@') { v = v * 16 + (*u->p - 'A'); u->p++; } if (*u->p == '@') u->p++; }
            t = fmt("%ld", neg ? -v : v);
        } else t = parse_type(u, 1);
        if (u->err) { free(s); free(t); return 0; }
        s = cat_free(s, fmt("%s%s", first ? "" : ",", t)); free(t); first = 0;
    }
    if (*u->p == '@') u->p++;
    return s;
}
/* one name fragment; stores it in the name back-reference table */
static char *fragment(Und *u)
{
    char *r;
    if (*u->p >= '0' && *u->p <= '9') { int k = u->start + (*u->p++ - '0'); if (k >= u->nn) { u->err = 1; return dup(""); } return dup(u->names[k]); }
    if (u->p[0] == '?' && u->p[1] >= '0' && u->p[1] <= '9') {        /* scope number: `n' (not a back-reference) */
        int k = u->p[1] - '0' + 1; u->p += 2; return fmt("`%d'", k);
    }
    if (u->p[0] == '?' && u->p[1] == '?') {                          /* nested symbol: `<demangled>' */
        u->p++;
        int start = u->start; Und save_args; memcpy(save_args.args, u->args, sizeof u->args); save_args.na = u->na;
        u->start = u->nn; u->na = 0;
        extern char *und_symbol(Und *);
        char *inner = und_symbol(u);
        for (int k = 0; k < u->na; k++) free(u->args[k]);
        memcpy(u->args, save_args.args, sizeof u->args); u->na = save_args.na; u->start = start;
        if (!inner) { u->err = 1; return dup(""); }
        r = fmt("`%s'", inner); free(inner);
        if (u->nn < 64) u->names[u->nn++] = dup(r);
        return r;
    }
    if (u->p[0] == '?' && u->p[1] == '$') {                          /* template: name<args> with its own back-references */
        u->p += 2;
        Und sub = *u; sub.nn = 0; sub.start = 0; sub.na = 0;
        const char *e = strchr(sub.p, '@'); if (!e) { u->err = 1; return dup(""); }
        char *base = fmt("%.*s", (int)(e - sub.p), sub.p); sub.p = e + 1;
        sub.names[sub.nn++] = base;
        char *args = template_args(&sub);
        if (!args) { u->err = 1; return dup(""); }
        r = fmt("%s<%s%s>", base, args, args[0] && args[strlen(args) - 1] == '>' ? " " : "");
        free(args); u->p = sub.p;
    } else {
        const char *e = strchr(u->p, '@'); if (!e) { u->err = 1; return dup(""); }
        r = fmt("%.*s", (int)(e - u->p), u->p); u->p = e + 1;
    }
    if (u->nn - u->start < 10 && u->nn < 64) u->names[u->nn++] = dup(r);
    return r;
}
static char *qualified_name(Und *u, int unused)
{
    (void)unused;
    char *n = fragment(u);
    while (*u->p && *u->p != '@' && !u->err) { char *s = fragment(u); n = cat_free(cat_free(s, dup("::")), n); }
    if (*u->p == '@') u->p++;
    return n;
}
static const char *cv(char c) { return c == 'B' ? " const" : c == 'C' ? " volatile" : c == 'D' ? " const volatile" : ""; }
static char *func_type_args(Und *u);
static char *parse_type(Und *u, int in_args)
{
    const char *start = u->p; char *r = 0; char c = *u->p++;
    switch (c) {
    case 'C': r = dup("signed char"); break; case 'D': r = dup("char"); break; case 'E': r = dup("unsigned char"); break;
    case 'F': r = dup("short"); break; case 'G': r = dup("unsigned short"); break; case 'H': r = dup("int"); break;
    case 'I': r = dup("unsigned int"); break; case 'J': r = dup("long"); break; case 'K': r = dup("unsigned long"); break;
    case 'M': r = dup("float"); break; case 'N': r = dup("double"); break; case 'O': r = dup("long double"); break;
    case 'X': r = dup("void"); break; case 'Z': r = dup("..."); break;
    case '_': {
        char d = *u->p++;
        r = dup(d == 'N' ? "bool" : d == 'J' ? "__int64" : d == 'K' ? "unsigned __int64" : d == 'W' ? "wchar_t" : d == 'D' ? "__int8" : d == 'E' ? "unsigned __int8" : "?");
        if (!strcmp(r, "?")) u->err = 1;
        break;
    }
    case 'T': case 'U': case 'V': { char *n = qualified_name(u, 0); r = fmt("%s %s", c == 'T' ? "union" : c == 'U' ? "struct" : "class", n); free(n); break; }
    case 'W': { u->p++; char *n = qualified_name(u, 0); r = fmt("enum %s", n); free(n); break; }
    case 'P': case 'Q': case 'R': case 'S': case 'A': case 'B': {
        if (*u->p == '6') {                                           /* pointer to function */
            u->p++; char cc = *u->p++;
            const char *conv = (cc == 'A' || cc == 'B') ? "__cdecl" : (cc == 'E' || cc == 'F') ? "__thiscall" : (cc == 'G' || cc == 'H') ? "__stdcall" : (cc == 'I' || cc == 'J') ? "__fastcall" : "__pascal";
            char *ret = parse_type(u, 0); char *args = func_type_args(u);
            r = fmt("%s (%s*)(%s)", ret, conv, args); free(ret); free(args);
            break;
        }
        char q = *u->p++;                                            /* qualifiers of the pointee */
        char *t = parse_type(u, 0);
        const char *sym = (c == 'A' || c == 'B') ? "&" : "*";
        const char *self = !in_args && c != 'B' ? "" : c == 'Q' ? " const" : c == 'R' ? " volatile" : c == 'S' ? " const volatile" : c == 'B' ? " volatile" : "";
        r = fmt("%s%s %s%s", t, cv(q), sym, self); free(t);
        break;
    }
    case '?': {                                                       /* cv-qualified value type */
        char q = *u->p++; char *t = parse_type(u, 0); r = fmt("%s%s", t, cv(q)); free(t); break;
    }
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9':
        if (in_args && c - '0' < u->na) return dup(u->args[c - '0']);
        u->err = 1; return dup("");
    default: u->err = 1; return dup("");
    }
    if (in_args && u->p - start > 1 && u->na < 10) u->args[u->na++] = dup(r);
    return r;
}
static char *func_type_args(Und *u)
{
    if (*u->p == 'X') { u->p++; return dup("void"); }
    char *s = dup(""); int first = 1;
    while (*u->p && *u->p != '@' && *u->p != 'Z' && !u->err) { char *t = parse_type(u, 1); s = cat_free(s, fmt("%s%s", first ? "" : ",", t)); free(t); first = 0; }
    if (*u->p == 'Z') { u->p++; s = cat_free(s, fmt("%s...", first ? "" : ",")); }
    else if (*u->p == '@') u->p++;
    return s;
}
/* returns a malloc'd string, or 0 if the name is not understood */
char *und_symbol(Und *up)
{
    Und u_ = *up; Und *U = up; (void)u_;
#define u (*U)
    if (*u.p != '?') { u.err = 1; return 0; }
    u.p++;
    if (*u.p == '?') { u.err = 1; return 0; }                          /* operators and other special names: not used by the game */
    char *name = qualified_name(&u, 0), *out = 0;
    if (u.err) goto done;
    char c = *u.p++;
    if (c >= '0' && c <= '4') {                                       /* data */
        char *t = parse_type(&u, 0); char q = *u.p ? *u.p++ : 'A';
        const char *acc = c == '0' ? "private: static " : c == '1' ? "protected: static " : c == '2' ? "public: static " : "";
        out = fmt("%s%s%s %s", acc, t, cv(q), name); free(t);
        goto done;
    }
    static const char *access[26] = {
        ['A' - 'A'] = "private: ", ['B' - 'A'] = "private: ", ['C' - 'A'] = "private: static ", ['D' - 'A'] = "private: static ",
        ['E' - 'A'] = "private: virtual ", ['F' - 'A'] = "private: virtual ", ['I' - 'A'] = "protected: ", ['J' - 'A'] = "protected: ",
        ['K' - 'A'] = "protected: static ", ['L' - 'A'] = "protected: static ", ['M' - 'A'] = "protected: virtual ", ['N' - 'A'] = "protected: virtual ",
        ['Q' - 'A'] = "public: ", ['R' - 'A'] = "public: ", ['S' - 'A'] = "public: static ", ['T' - 'A'] = "public: static ",
        ['U' - 'A'] = "public: virtual ", ['V' - 'A'] = "public: virtual ", ['Y' - 'A'] = "", ['Z' - 'A'] = ""};
    if (c < 'A' || c > 'Z' || !access[c - 'A']) goto done;
    int member = strchr("ABEFIJMNQRUV", c) != 0;
    char thiscv = member ? *u.p++ : 'A';
    char cc = *u.p++;
    const char *conv = (cc == 'A' || cc == 'B') ? "__cdecl" : (cc == 'E' || cc == 'F') ? "__thiscall" : (cc == 'G' || cc == 'H') ? "__stdcall" : (cc == 'I' || cc == 'J') ? "__fastcall" : (cc == 'C' || cc == 'D') ? "__pascal" : 0;
    if (!conv) goto done;
    char *ret;
    if (*u.p == '@') { u.p++; ret = dup(""); }
    else if (u.p[0] == '?' && (u.p[1] == 'A' || u.p[1] == 'B')) { char q = u.p[1]; u.p += 2; char *t = parse_type(&u, 0); ret = fmt("%s%s ", t, cv(q)); free(t); }
    else { char *t = parse_type(&u, 0); ret = fmt("%s ", t); free(t); }
    char *args = func_type_args(&u);
    if (*u.p == 'Z') u.p++;                                           /* throw specification: none */
    if (!u.err) out = fmt("%s%s%s %s(%s)%s", access[c - 'A'], ret, conv, name, args, thiscv == 'B' ? "const " : thiscv == 'C' ? "volatile " : thiscv == 'D' ? "const volatile " : "");
    free(ret); free(args);
done:
    free(name);
    if (u.err) { free(out); out = 0; }
    return out;
#undef u
}
char *w32_undname(const char *mangled)
{
    if (mangled[0] != '?') return 0;
    if (!strncmp(mangled, "??_C", 4)) return dup("`string'");
    Und u; memset(&u, 0, sizeof u); u.p = mangled;
    char *out = und_symbol(&u);
    if (out && *u.p) { free(out); out = 0; }                        /* trailing garbage: not understood */
    for (int k = 0; k < u.nn; k++) free(u.names[k]);
    for (int k = 0; k < u.na; k++) free(u.args[k]);
    return out;
}
