/* Loader and import dispatch for the recompiled game: maps DungeonSiege.exe into guest memory at its preferred base,
 * points every IAT slot at a thunk address, builds the PEB, the main TEB (with static TLS) and process parameters. */
#include "w32.h"
#include "edition.h"
#include <strings.h>
#include <setjmp.h>
#include <sys/mman.h>

uint32_t w32_image_base, w32_image_size, w32_entry, w32_tls_index_addr, w32_tls_template, w32_tls_size, w32_tls_zero;
char     w32_game_dir[1024];
int      w32_deterministic;
volatile int w32_exited; uint32_t w32_exit_code;
void (*w32_callback_hook)(Ctx *c, uint32_t fn);
__thread jmp_buf *w32_exit_jmp;

/* ---- import table: static imports first, then names resolved later by GetProcAddress ---- */
typedef void (*ImpFn)(Ctx *);
struct impdef { const char *name; ImpFn fn; };
extern const struct impdef w32_impls[]; extern const unsigned w32_nimpls;     /* generated: imptab.c */
struct rt_imp { uint32_t iat; const char *name; };
extern const struct rt_imp rt_imptab[]; extern const unsigned rt_imptab_n;
static char *thunk_name[MAX_THUNKS]; static ImpFn thunk_fn[MAX_THUNKS]; static unsigned nthunks;
static uint32_t thunk_target[MAX_THUNKS];      /* import served by a recompiled DLL: its export's guest address */
struct rt_image { const char *name; uint32_t base, imp_first, imp_count; };
extern const struct rt_image rt_images[]; extern const unsigned rt_images_n;
static uint8_t image_loaded[64], image_inited[64], image_ref[64];   /* mapped / DllMain done / loaded by the game */
static pthread_mutex_t thunk_lock = PTHREAD_MUTEX_INITIALIZER;

/* "KERNEL32.dll!GetVersion" -> "kernel32!GetVersion"; "mss32.dll!_AIL_startup@0" -> "mss32!AIL_startup";
 * "WSOCK32.dll!#10" -> "wsock32!ord10" */
static void canon(const char *dll, const char *fn, char *out, size_t cap)
{
    char d[64]; size_t k = 0;
    for (; dll[k] && dll[k] != '.' && k < sizeof d - 1; k++) d[k] = (char)(dll[k] >= 'A' && dll[k] <= 'Z' ? dll[k] + 32 : dll[k] == '-' ? '_' : dll[k]);   /* vulkan-1 -> vulkan_1 */
    d[k] = 0;
    if (!strncmp(d, "d3dx9_", 6)) strcpy(d, "d3dx9_29");     /* every D3DX9 version: one implementation (DS2 29, Fallout NV 38) */
    char f[128]; const char *s = fn; size_t j = 0;
    if (*s == '#') { snprintf(f, sizeof f, "ord%s", s + 1); }
    else if (!strncmp(s, "??2@", 4) || !strncmp(s, "??3@", 4)) snprintf(f, sizeof f, s[2] == '2' ? "op_new" : "op_delete");   /* C++ operators */
    else {
        if (*s == '_' && strchr(s, '@')) s++;
        for (; *s && *s != '@' && j < sizeof f - 1; s++) f[j++] = *s;
        f[j] = 0;
    }
    snprintf(out, cap, "%s!%s", d, f);
}
/* an IAT slot: the thunk of import k, or for a data import (msvcrt.c w32_data_import) the variable's address */
static uint32_t iat_value(unsigned k)
{
    uint32_t w32_data_import(const char *canon_name);
    uint32_t a = thunk_name[k] ? w32_data_import(thunk_name[k]) : 0;
    return a ? a : THUNK_BASE + 16 * k;
}
static ImpFn lookup_impl(const char *canon_name)
{
    for (unsigned k = 0; k < w32_nimpls; k++) if (!strcmp(w32_impls[k].name, canon_name)) return w32_impls[k].fn;
    return 0;
}
uint32_t w32_thunk_for(const char *dll, const char *name)
{
    char cn[200]; canon(dll, name, cn, sizeof cn);
    pthread_mutex_lock(&thunk_lock);
    unsigned k;
    for (k = 0; k < nthunks; k++) if (!strcmp(thunk_name[k], cn)) break;
    if (k == nthunks && nthunks < MAX_THUNKS) { thunk_name[k] = strdup(cn); thunk_fn[k] = lookup_impl(cn); nthunks++; }
    pthread_mutex_unlock(&thunk_lock);
    return k < MAX_THUNKS ? THUNK_BASE + 16 * k : 0;
}
/* an internal entry point the game reaches through a pointer (COM methods, callbacks we hand out) */
uint32_t w32_thunk_register(const char *name, void (*fn)(Ctx *))
{
    pthread_mutex_lock(&thunk_lock);
    unsigned k = nthunks < MAX_THUNKS ? nthunks++ : MAX_THUNKS;
    if (k < MAX_THUNKS) { thunk_name[k] = strdup(name); thunk_fn[k] = fn; }
    pthread_mutex_unlock(&thunk_lock);
    return k < MAX_THUNKS ? THUNK_BASE + 16 * k : 0;
}
int w32_thunk_implemented(uint32_t addr) { uint32_t k = (addr - THUNK_BASE) / 16; return k < nthunks && thunk_fn[k]; }
const char *w32_thunk_name(uint32_t index) { return index < nthunks ? thunk_name[index] : "?"; }

/* W32_TRACE=<path>: text log of every import call (thread, arguments, result) in real runs */
static FILE *text_trace; static int text_trace_init; static pthread_mutex_t tt_lock = PTHREAD_MUTEX_INITIALIZER;
void w32_crash_record_call(uint32_t index);
__thread uint32_t w32_cur_import;                    /* the thunk being serviced (one handler for many entry points) */
void rt_import(Ctx *c, uint32_t index)
{
    w32_crash_record_call(index); w32_cur_import = index;
    if (!text_trace_init) { text_trace_init = 1; const char *p = getenv("W32_TRACE"); if (p && (text_trace = fopen(p, "w"))) setvbuf(text_trace, 0, _IOLBF, 0); }   /* lines survive an abrupt exit */
    static const char *tfilter; static int tf_init;                       /* W32_TRACE_ONLY=wsock32,kernel32!CreateFile: only those */
    if (!tf_init) { tf_init = 1; tfilter = getenv("W32_TRACE_ONLY"); }
    if (index < MAX_THUNKS && thunk_target[index]) {                     /* served by a recompiled DLL (traced by name, no result) */
        if (text_trace && tfilter && thunk_name[index] && strstr(tfilter, "dll:")) {
            const char *nm = thunk_name[index], *bang = strchr(nm, '!'); size_t dl = bang ? (size_t)(bang - nm) : strlen(nm);
            const char *f = strstr(tfilter, "dll:"); if (f && !strncasecmp(f + 4, nm, dl)) {
                pthread_mutex_lock(&tt_lock); fprintf(text_trace, "%x %08x %s(%x, %x, %x) [recompiled]\n", rt_r32(G_MEM, c->fs_base + 0x24), rt_r32(G_MEM, c->esp),
                    nm, rt_r32(G_MEM, c->esp + 4), rt_r32(G_MEM, c->esp + 8), rt_r32(G_MEM, c->esp + 12)); pthread_mutex_unlock(&tt_lock); }
        }
        rt_call(c, thunk_target[index]); return;
    }
    w32_trace_import(c, index);
    int traced = text_trace && index < nthunks && thunk_fn[index];
    if (traced && tfilter) {
        const char *nm = thunk_name[index], *bang = strchr(nm, '!'); size_t dl = bang ? (size_t)(bang - nm) : strlen(nm); traced = 0;
        for (const char *f = tfilter; *f; ) { const char *e = strchr(f, ','); size_t fl = e ? (size_t)(e - f) : strlen(f);
            if (fl == dl && !strncasecmp(f, nm, dl)) traced = 1;                       /* a whole DLL */
            if (memchr(f, '!', fl) && !strncasecmp(f, nm, fl)) traced = 1;            /* or dll!name prefix */
            f += fl; if (*f == ',') f++; }
    }
    if (traced) {
        uint32_t a[5]; for (int k = 0; k < 5; k++) a[k] = rt_r32(G_MEM, c->esp + 4 + 4 * k);
        uint32_t ret = rt_r32(G_MEM, c->esp), tid = rt_r32(G_MEM, c->fs_base + 0x24);
        thunk_fn[index](c);
        pthread_mutex_lock(&tt_lock);
        fprintf(text_trace, "%x %08x %s(%x, %x, %x, %x, %x) -> %x\n", tid, ret, thunk_name[index], a[0], a[1], a[2], a[3], a[4], c->eax);
        pthread_mutex_unlock(&tt_lock);
        return;
    }
    if (index >= nthunks || !thunk_fn[index]) {
        static __thread char buf[200];
        snprintf(buf, sizeof buf, "unimplemented import %s", index < nthunks ? thunk_name[index] : "?");
        rt_unhandled(c, rt_r32(G_MEM, c->esp), buf);
    }
    thunk_fn[index](c);
}

/* ---- process exit ---- */
void w32_exit(Ctx *c, uint32_t code)
{
    (void)c; w32_exit_code = code; w32_exited = 1;
    if (w32_exit_jmp) longjmp(*w32_exit_jmp, 1);
    exit((int)code);
}

/* ---- guest callbacks from imports ---- */
#define CALLBACK_RET 0xffeff000u
uint32_t w32_callback(Ctx *c, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t esp0 = c->esp, saved[4] = {c->ebx, c->esi, c->edi, c->ebp};
    for (int k = nargs - 1; k >= 0; k--) { c->esp -= 4; rt_w32(G_MEM, c->esp, args[k]); }
    c->esp -= 4; rt_w32(G_MEM, c->esp, CALLBACK_RET);
    rt_fpcr_sync(c->fcw);                                     /* this thread's rounding mode follows the game's */
    if (w32_callback_hook) w32_callback_hook(c, fn); else rt_call(c, fn);
    rt_fpcr_sync(c->fcw);
    uint32_t r = c->eax;
    c->esp = esp0; c->ebx = saved[0]; c->esi = saved[1]; c->edi = saved[2]; c->ebp = saved[3];
    return r;
}

/* ---- threads: TEB with static TLS ---- */
static uint32_t nteb; static pthread_mutex_t teb_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t w32_tid(Ctx *c) { return rt_r32(G_MEM, c->fs_base + 0x24); }
void w32_set_last_error(Ctx *c, uint32_t e) { rt_w32(G_MEM, c->fs_base + 0x34, e); }
uint32_t w32_new_thread_teb(uint32_t stack_lo, uint32_t stack_hi)
{
    pthread_mutex_lock(&teb_lock); uint32_t k = nteb < MAX_THREADS ? nteb++ : MAX_THREADS; pthread_mutex_unlock(&teb_lock);
    if (k >= MAX_THREADS) return 0;
    uint32_t teb = TEB_BASE + k * 0x2000, tlsarr = teb + 0x1000;
    memset(G_MEM + teb, 0, 0x2000);
    rt_w32(G_MEM, teb + 0x00, 0xffffffffu);          /* SEH chain end */
    rt_w32(G_MEM, teb + 0x04, stack_hi); rt_w32(G_MEM, teb + 0x08, stack_lo);
    rt_w32(G_MEM, teb + 0x18, teb);
    rt_w32(G_MEM, teb + 0x20, 0x40);                 /* process id */
    rt_w32(G_MEM, teb + 0x24, 0x100 + 4 * k);        /* thread id */
    rt_w32(G_MEM, teb + 0x2c, tlsarr);
    rt_w32(G_MEM, teb + 0x30, PEB_ADDR);
    if (w32_tls_size || w32_tls_zero) {
        uint32_t blk = heap_alloc(w32_process_heap, 8, w32_tls_size + w32_tls_zero);
        memcpy(G_MEM + blk, G_MEM + w32_tls_template, w32_tls_size);
        rt_w32(G_MEM, tlsarr + 4 * rt_r32(G_MEM, w32_tls_index_addr), blk);
    }
    return teb;
}

/* ---- image ---- */
static int read_file(const char *p, uint8_t **data, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    *data = malloc(*n); size_t r = fread(*data, 1, *n, f); fclose(f);
    return r == *n ? 0 : -1;
}
static void put_str(uint32_t *at, const char *s) { size_t n = strlen(s) + 1; memcpy(G_MEM + *at, s, n); *at += (uint32_t)((n + 3) & ~3u); }
uint32_t w32_cmdline_a, w32_cmdline_w, w32_modname_a, w32_env_a, w32_env_w;
void w32_load_static_images(void);

int w32_load(const char *exe_path)
{
    uint8_t *d; size_t n;
    if (!G_MEM) {
        void *p = mmap(0, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) return -1;
        G_MEM = p; mprotect(G_MEM, 0x10000, PROT_NONE);
        extern int w32_protect_memory;
        if (w32_protect_memory) mprotect(G_MEM, 1ull << 32, PROT_NONE);   /* free guest memory faults, as on Windows */
    }
    if (read_file(exe_path, &d, &n)) { fprintf(stderr, "w32: cannot read %s\n", exe_path); return -1; }
    uint32_t pe = *(uint32_t *)(d + 0x3c), opt = pe + 24;
    uint16_t nsec = *(uint16_t *)(d + pe + 6), optsz = *(uint16_t *)(d + pe + 20);
    w32_image_base = *(uint32_t *)(d + opt + 28); w32_image_size = *(uint32_t *)(d + opt + 56);
    w32_entry = w32_image_base + *(uint32_t *)(d + opt + 16);
    uint32_t hdrsz = *(uint32_t *)(d + opt + 60);
    vm_register(w32_image_base, (w32_image_size + 0xffff) & ~0xffffu, 0x1000000 /* MEM_IMAGE */);
    vm_register(PEB_ADDR, 0x10000, 0x20000); vm_register(TEB_BASE, MAX_THREADS * 0x2000, 0x20000);
    memcpy(G_MEM + w32_image_base, d, hdrsz);
    for (int k = 0; k < nsec; k++) {
        uint8_t *s = d + opt + optsz + 40 * k;
        uint32_t va = *(uint32_t *)(s + 12), vs = *(uint32_t *)(s + 8), rs = *(uint32_t *)(s + 16), ra = *(uint32_t *)(s + 20);
        if (!ra) continue;                       /* Watcom's .bss: a raw size but no file data */
        if (!vs) vs = rs;                        /* Watcom leaves VirtualSize 0 */
        memcpy(G_MEM + w32_image_base + va, d + ra, rs < vs ? rs : vs);
        if ((*(uint32_t *)(s + 36) & 0x20000000u) && memcmp(s, ".bind", 5) && getenv("W32_SMCLOG")) {   /* code: kept for w32_smc_check */
            extern uint8_t *w32_code_copy[8]; extern uint32_t w32_code_va[8], w32_code_n[8]; extern int w32_code_k;
            if (w32_code_k < 8) { uint32_t n = rs < vs ? rs : vs; w32_code_copy[w32_code_k] = malloc(n); memcpy(w32_code_copy[w32_code_k], d + ra, n);
                w32_code_va[w32_code_k] = w32_image_base + va; w32_code_n[w32_code_k++] = n; }
        }
    }
    /* imports: thunk k <-> rt_imptab[k] (the recompiled code calls imports by that index) */
    for (unsigned k = 0; k < rt_imptab_n && k < MAX_THUNKS; k++) {
        char dll[64]; const char *bang = strchr(rt_imptab[k].name, '!');
        snprintf(dll, sizeof dll, "%.*s", (int)(bang - rt_imptab[k].name), rt_imptab[k].name);
        char cn[200]; canon(dll, bang + 1, cn, sizeof cn);
        thunk_name[k] = strdup(cn); thunk_fn[k] = lookup_impl(cn);
        if (rt_imptab[k].iat - w32_image_base < w32_image_size)          /* the executable's; DLLs' when they are mapped */
            rt_w32(G_MEM, rt_imptab[k].iat, iat_value(k));
    }
    nthunks = rt_imptab_n;
    /* static TLS */
    uint32_t tlsdir = *(uint32_t *)(d + opt + 96 + 8 * 9);
    if (tlsdir) {
        uint32_t t = w32_image_base + tlsdir;
        w32_tls_template = rt_r32(G_MEM, t); w32_tls_size = rt_r32(G_MEM, t + 4) - w32_tls_template;
        w32_tls_index_addr = rt_r32(G_MEM, t + 8); w32_tls_zero = rt_r32(G_MEM, t + 16);
        rt_w32(G_MEM, w32_tls_index_addr, 0);
        uint32_t cb = rt_r32(G_MEM, t + 12);
        if (cb && rt_r32(G_MEM, cb)) fprintf(stderr, "w32: TLS callbacks present (not run)\n");
    }
    free(d);
    /* process heap, PEB, parameters */
    w32_process_heap = heap_create(0, 0);
    rt_w32(G_MEM, PEB_ADDR + 0x08, w32_image_base); rt_w32(G_MEM, PEB_ADDR + 0x18, w32_process_heap);
    uint32_t at = PARAMS_ADDR;
    w32_modname_a = at; put_str(&at, "C:\\GOG Games\\Dungeon Siege\\" GAME_EXE);
    {   /* the game's command line; DS_ARGS adds arguments (e.g. zonematch=true opens the multiplayer screens) */
        char cl[1024]; const char *extra = getenv("DS_ARGS");
        snprintf(cl, sizeof cl, "\"C:\\GOG Games\\Dungeon Siege\\" GAME_EXE "\"%s%s", extra ? " " : "", extra ? extra : "");
        w32_cmdline_a = at; put_str(&at, cl);
    }
    w32_cmdline_w = at; at += 2 * (uint32_t)w32_mb_to_wide(1252, (const uint8_t *)GS(w32_cmdline_a), -1, (uint16_t *)GP(at), 512); at = (at + 3) & ~3u;
    static char cname[48]; { void w32_computer_name(char *, size_t); char n[32]; w32_computer_name(n, sizeof n); snprintf(cname, sizeof cname, "COMPUTERNAME=%s", n); }
    const char *env[] = {"ALLUSERSPROFILE=C:\\ProgramData", cname, "OS=Windows_NT",
        "PATH=C:\\Windows\\system32;C:\\Windows", "SystemRoot=C:\\Windows", "TEMP=C:\\Temp", "TMP=C:\\Temp",
        "USERNAME=player", "USERPROFILE=C:\\Users\\player", "windir=C:\\Windows", 0};
    w32_env_a = at;
    for (int k = 0; env[k]; k++) { size_t l = strlen(env[k]) + 1; memcpy(G_MEM + at, env[k], l); at += (uint32_t)l; }
    G_MEM[at++] = 0; at = (at + 3) & ~3u;
    w32_env_w = at;
    for (uint32_t s = w32_env_a; G_MEM[s]; ) { int l = (int)strlen(GS(s)) + 1; at += 2 * (uint32_t)w32_mb_to_wide(1252, G_MEM + s, l, (uint16_t *)GP(at), l); s += (uint32_t)l; }
    rt_w16(G_MEM, at, 0); at += 2;
    {   /* PEB->ProcessParameters (RTL_USER_PROCESS_PARAMETERS): normalized, image path (+0x38) and command line (+0x40);
         * the 2015+ C runtime reads it on every new thread */
        at = (at + 15) & ~15u; uint32_t pp = at; memset(G_MEM + pp, 0, 0x2a0); at += 0x2a0;
        rt_w32(G_MEM, pp, 0x2a0); rt_w32(G_MEM, pp + 4, 0x2a0); rt_w32(G_MEM, pp + 8, 1);
        uint32_t cl = 0; while (rt_r16(G_MEM, w32_cmdline_w + 2 * cl)) cl++;
        rt_w16(G_MEM, pp + 0x40, 2 * cl); rt_w16(G_MEM, pp + 0x42, 2 * cl + 2); rt_w32(G_MEM, pp + 0x44, w32_cmdline_w);
        uint32_t ip = at, n = (uint32_t)w32_mb_to_wide(1252, (const uint8_t *)GS(w32_modname_a), -1, (uint16_t *)GP(ip), 260); at += 2 * n; at = (at + 3) & ~3u;
        rt_w16(G_MEM, pp + 0x38, 2 * (n - 1)); rt_w16(G_MEM, pp + 0x3a, 2 * n); rt_w32(G_MEM, pp + 0x3c, ip);
        rt_w32(G_MEM, PEB_ADDR + 0x10, pp);
    }
    w32_load_static_images();
    return 0;
}

/* ---- tracing (harness) ---- */
FILE *w32_trace_file; static uint64_t trace_seq;
static uint64_t hash_range(uint32_t a, uint32_t n)
{
    uint64_t h = 1469598103934665603ull; const uint64_t *w = (const uint64_t *)(G_MEM + a);
    for (uint32_t k = 0; k < n / 8; k++) { h ^= w[k]; h *= 1099511628211ull; }
    return h;
}
void w32_trace_import(Ctx *c, uint32_t index)
{
    if (!w32_trace_file) return;
    struct { uint32_t seq, index, regs[8]; uint64_t himg, hvm; uint32_t ret, pad; } r;
    r.seq = (uint32_t)trace_seq++; r.index = index; r.ret = rt_r32(G_MEM, c->esp); r.pad = 0;
    memcpy(r.regs, &c->eax, sizeof r.regs);
    r.himg = hash_range(0x75b000, 0x52000);            /* .data + .tls of GOG 1.11.1 */
    r.hvm = (r.seq % 64 == 0) ? vm_hash() : 0;
    fwrite(&r, sizeof r, 1, w32_trace_file); fflush(w32_trace_file);
}

/* ---- recompiled DLLs (Miles Sound System and its providers): mapped at their preferred base from the game folder ---- */
static int image_index(const char *name)
{
    const char *b = strrchr(name, '\\'), *f = strrchr(name, '/'); if (f > b) b = f; b = b ? b + 1 : name;
    for (unsigned k = 1; k < rt_images_n; k++) {
        const char *r = strrchr(rt_images[k].name, '/'); r = r ? r + 1 : rt_images[k].name;
        if (!strcasecmp(r, b)) return (int)k;
        char nb[256]; snprintf(nb, sizeof nb, "%s", b); char *dot = strrchr(nb, '.'); if (!dot) { strcat(nb, ".dll"); if (!strcasecmp(r, nb)) return (int)k; }
    }
    return -1;
}
uint32_t w32_export(uint32_t base, const char *name, uint32_t ordinal)
{
    uint32_t pe = base + rt_r32(G_MEM, base + 0x3c), erva = rt_r32(G_MEM, pe + 24 + 96);
    if (!erva) return 0;
    uint32_t ed = base + erva, nf = rt_r32(G_MEM, ed + 20), nn = rt_r32(G_MEM, ed + 24), funcs = base + rt_r32(G_MEM, ed + 28),
             names = base + rt_r32(G_MEM, ed + 32), ords = base + rt_r32(G_MEM, ed + 36), obase = rt_r32(G_MEM, ed + 16);
    if (!name) { uint32_t i = ordinal - obase; return i < nf ? base + rt_r32(G_MEM, funcs + 4 * i) : 0; }
    for (uint32_t k = 0; k < nn; k++) if (!strcmp(GS(base + rt_r32(G_MEM, names + 4 * k)), name)) return base + rt_r32(G_MEM, funcs + 4 * rt_r16(G_MEM, ords + 2 * k));
    return 0;
}
static int load_image(int k);
/* resolve image k's imports: into another recompiled image's exports where one exists */
static void bind_imports(int k)
{
    const struct rt_image *im = &rt_images[k];
    for (uint32_t j = im->imp_first; j < im->imp_first + im->imp_count && j < MAX_THUNKS; j++) {
        const char *full = rt_imptab[j].name, *bang = strchr(full, '!'); char dll[64];
        snprintf(dll, sizeof dll, "%.*s", (int)(bang - full), full);
        int t = image_index(dll);
        if (t > 0 && k == 0 && !strncasecmp(dll, "mss32", 5)) {                 /* the game's Miles: recompiled or native */
            const char *m = getenv("W32_MILES"); if (m && !strcmp(m, "native")) t = -1;
        }
        if (t > 0 && load_image(t) == 0) {
            image_ref[t] = 1;
            uint32_t a = bang[1] == '#' ? w32_export(rt_images[t].base, 0, (uint32_t)atoi(bang + 2)) : w32_export(rt_images[t].base, bang + 1, 0);
            if (a) thunk_target[j] = a; else fprintf(stderr, "w32: %s not exported by %s\n", bang + 1, rt_images[t].name);
        }
    }
}
static int load_image(int k)
{
    if (image_loaded[k]) return 0;
    const struct rt_image *im = &rt_images[k];
    char win[512], host[2048]; snprintf(win, sizeof win, "C:\\GOG Games\\Dungeon Siege\\%s", im->name);
    for (char *p = win; *p; p++) if (*p == '/') *p = '\\';
    if (w32_host_path(win, host, sizeof host, 0)) return -1;
    FILE *f = fopen(host, "rb"); if (!f) { fprintf(stderr, "w32: cannot open %s\n", host); return -1; }
    fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(n); if (fread(d, 1, n, f) != n) { fclose(f); free(d); return -1; } fclose(f);
    uint32_t pe = *(uint32_t *)(d + 0x3c), opt = pe + 24, size = *(uint32_t *)(d + opt + 56);
    uint16_t nsec = *(uint16_t *)(d + pe + 6), optsz = *(uint16_t *)(d + pe + 20);
    uint32_t file_base = *(uint32_t *)(d + opt + 28);
    vm_register(im->base, (size + 0xffff) & ~0xffffu, 0x1000000);
    memcpy(G_MEM + im->base, d, *(uint32_t *)(d + opt + 60));
    for (int s = 0; s < nsec; s++) {
        uint8_t *h = d + opt + optsz + 40 * s;
        uint32_t va = *(uint32_t *)(h + 12), vs = *(uint32_t *)(h + 8), rs = *(uint32_t *)(h + 16), ra = *(uint32_t *)(h + 20);
        memcpy(G_MEM + im->base + va, d + ra, rs < vs ? rs : vs);
    }
    free(d);
    if (file_base != im->base) {           /* recompiled at another base (tools/rebase_pe.py, two DLLs wanting one address): relocate */
        uint32_t delta = im->base - file_base, rel = rt_r32(G_MEM, im->base + pe + 24 + 96 + 5 * 8), rsz = rt_r32(G_MEM, im->base + pe + 24 + 96 + 5 * 8 + 4);
        if (!rel) { fprintf(stderr, "w32: %s: no relocations to move it to %#x\n", im->name, im->base); return -1; }
        for (uint32_t b = 0; b + 8 <= rsz; ) {
            uint32_t page = rt_r32(G_MEM, im->base + rel + b), bs = rt_r32(G_MEM, im->base + rel + b + 4);
            if (bs < 8) break;
            for (uint32_t e = 8; e + 2 <= bs; e += 2) {
                uint32_t v = rt_r16(G_MEM, im->base + rel + b + e);
                if ((v >> 12) == 3) { uint32_t a = im->base + page + (v & 0xfff); rt_w32(G_MEM, a, rt_r32(G_MEM, a) + delta); }
            }
            b += bs;
        }
        rt_w32(G_MEM, im->base + opt + 28, im->base);
    }
    image_loaded[k] = 1;
    for (uint32_t j = im->imp_first; j < im->imp_first + im->imp_count; j++) rt_w32(G_MEM, rt_imptab[j].iat, iat_value(j));
    bind_imports(k);
    return 0;
}
/* DllMain(hinst, DLL_PROCESS_ATTACH, 0) for loaded images, dependencies first (on a guest thread) */
static void init_image(Ctx *c, int k)
{
    if (image_inited[k] || !image_loaded[k]) return;
    image_inited[k] = 1;
    const struct rt_image *im = &rt_images[k];
    for (uint32_t j = im->imp_first; j < im->imp_first + im->imp_count; j++) if (thunk_target[j]) {
        for (unsigned t = 1; t < rt_images_n; t++) if (thunk_target[j] >= rt_images[t].base && thunk_target[j] < rt_images[t].base + 0x01000000u) init_image(c, (int)t);
    }
    uint32_t pe = im->base + rt_r32(G_MEM, im->base + 0x3c), ep = rt_r32(G_MEM, pe + 24 + 16);
    if (ep) { uint32_t a[3] = {im->base, 1, 0}; uint32_t r = w32_callback(c, im->base + ep, 3, a); if (!r) fprintf(stderr, "w32: %s DllMain failed\n", im->name); }
}
/* all recompiled images are mapped at start (their address ranges stay reserved); the executable's static
 * dependencies are bound now, the rest when the game loads them */
void w32_load_static_images(void)
{
    const char *m = getenv("W32_MILES");
    if (m && !strcmp(m, "native")) return;                 /* the hand-written Miles (miles.c); the DLLs stay unmapped */
    for (unsigned k = 1; k < rt_images_n; k++) load_image((int)k);
    bind_imports(0);
}
void w32_init_images(Ctx *c) { for (unsigned k = 1; k < rt_images_n; k++) if (image_ref[k]) init_image(c, (int)k); }
uint32_t w32_load_library(Ctx *c, const char *name)
{
    int k = image_index(name);
    if (k < 0 || load_image(k)) return 0;
    image_ref[k] = 1; init_image(c, k);
    return rt_images[k].base;
}
uint32_t w32_image_module(const char *name) { int k = image_index(name); return k > 0 && image_ref[k] ? rt_images[k].base : 0; }
const char *w32_image_name(uint32_t base) { for (unsigned k = 1; k < rt_images_n; k++) if (rt_images[k].base == base && image_ref[k]) return rt_images[k].name; return 0; }
