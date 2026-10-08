/* Windows functions Fallout: New Vegas imports beyond what the Dungeon Siege II port needed: INI files (the engine
 * reads every setting through GetPrivateProfileString), 64-bit file positions, shell and user32 odds and ends, and
 * "not available" answers for Steam/Galaxy, the console, common controls and XInput without a controller. */
#include "w32.h"
#include <sys/stat.h>
#include <ctype.h>

/* ---- INI files ---- */
static int ini_host(const char *file, char *host, size_t cap, int create)
{
    char win[1024];
    if (!strchr(file, '\\') && !strchr(file, '/')) snprintf(win, sizeof win, "C:\\Windows\\%s", file);   /* bare name: the Windows folder */
    else snprintf(win, sizeof win, "%s", file);
    return w32_host_path(win, host, cap, create);
}
static char *ini_read(const char *file, size_t *len)
{
    char host[2048]; if (ini_host(file, host, sizeof host, 0)) return 0;
    FILE *f = fopen(host, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    char *b = malloc((size_t)n + 2); size_t got = fread(b, 1, (size_t)n, f); fclose(f);
    b[got] = 0; if (len) *len = got; return b;
}
static void trim(const char **s, const char **e)
{
    while (*s < *e && isspace((unsigned char)**s)) (*s)++;
    while (*e > *s && isspace((unsigned char)(*e)[-1])) (*e)--;
}
static int eq_ci(const char *a, const char *ae, const char *b) { size_t n = strlen(b); return (size_t)(ae - a) == n && !strncasecmp(a, b, n); }
/* walks the file: for each line, its section [s..se) and key/value spans (key 0 for a section header or other line) */
typedef void (*IniVisit)(void *u, const char *sec, const char *se, const char *k, const char *ke, const char *v, const char *ve);
static void ini_walk(const char *b, IniVisit fn, void *u)
{
    const char *sec = "", *se = sec;
    for (const char *p = b; *p; ) {
        const char *l = p, *le = strchr(p, '\n'); if (!le) le = p + strlen(p);
        p = *le ? le + 1 : le;
        const char *s = l, *e = le; trim(&s, &e);
        if (s == e || *s == ';' || *s == '#') continue;
        if (*s == '[') { const char *c = memchr(s, ']', (size_t)(e - s)); if (c) { sec = s + 1; se = c; trim(&sec, &se); } continue; }
        const char *eqp = memchr(s, '=', (size_t)(e - s)); if (!eqp) continue;
        const char *k = s, *ke = eqp, *v = eqp + 1, *ve = e; trim(&k, &ke); trim(&v, &ve);
        if (ve - v >= 2 && (*v == '"' || *v == '\'') && ve[-1] == *v) { v++; ve--; }
        fn(u, sec, se, k, ke, v, ve);
    }
}
typedef struct { const char *app, *key; char *out; size_t cap, n; int found; } IniGet;
static void put(IniGet *g, const char *s, size_t n, int sep)     /* append, NUL-separated when listing */
{
    for (size_t k = 0; k < n && g->n + 1 < g->cap; k++) g->out[g->n++] = s[k];
    if (sep && g->n + 1 < g->cap) g->out[g->n++] = 0;
}
static void visit_get(void *u, const char *sec, const char *se, const char *k, const char *ke, const char *v, const char *ve)
{
    IniGet *g = u;
    if (!g->app) {                                         /* section names, each once */
        for (size_t o = 0; o < g->n; o += strlen(g->out + o) + 1) if (eq_ci(sec, se, g->out + o)) return;
        put(g, sec, (size_t)(se - sec), 1); return;
    }
    if (!eq_ci(sec, se, g->app)) return;
    if (!g->key) { put(g, k, (size_t)(ke - k), 1); return; }
    if (!g->found && eq_ci(k, ke, g->key)) { g->found = 1; put(g, v, (size_t)(ve - v), 0); }
}
static uint32_t profile_string(const char *app, const char *key, const char *def, char *out, uint32_t cap, const char *file)
{
    if (!cap) return 0;
    char *b = ini_read(file, 0);
    IniGet g = {app, key, out, cap, 0, 0};
    if (b) { ini_walk(b, visit_get, &g); free(b); }
    if (!app || !key) {                                    /* a list: double NUL at the end */
        if (g.n + 1 >= cap) { out[cap - 1] = 0; if (cap >= 2) out[cap - 2] = 0; return cap - 2; }
        out[g.n] = 0; out[g.n + 1 < cap ? g.n + 1 : g.n] = 0; return (uint32_t)g.n;
    }
    if (!g.found) {
        const char *d = def ? def : "", *de = d + strlen(d); while (de > d && de[-1] == ' ') de--;
        g.n = 0; put(&g, d, (size_t)(de - d), 0);
    }
    out[g.n] = 0; return (uint32_t)g.n;
}
IMPL(kernel32, GetPrivateProfileStringA)              /* (app, key, default, buffer, size, file) */
{
    uint32_t a = ARG(0), k = ARG(1), d = ARG(2), buf = ARG(3), n = ARG(4), f = ARG(5);
    uint32_t r = profile_string(a ? GS(a) : 0, k ? GS(k) : 0, d ? GS(d) : 0, (char *)GP(buf), n, f ? GS(f) : "win.ini");
    if (getenv("W32_INILOG")) fprintf(stderr, "w32: ini [%s] %s = \"%s\" (%s)\n", a ? GS(a) : "*", k ? GS(k) : "*", GS(buf), f ? GS(f) : "");
    RET(r, 6);
}
IMPL(kernel32, GetPrivateProfileIntA)                 /* (app, key, default, file) */
{
    char v[64]; uint32_t a = ARG(0), k = ARG(1), f = ARG(3);
    if (!a || !k || !profile_string(GS(a), GS(k), 0, v, sizeof v, f ? GS(f) : "win.ini")) RET(ARG(2), 4);
    RET((uint32_t)strtol(v, 0, 0), 4);
}
typedef struct { const char *app, *key; int in_app, done; } IniPut;
IMPL(kernel32, WritePrivateProfileStringA)            /* (app, key, value, file): key 0 deletes the section, value 0 the key */
{
    uint32_t ap = ARG(0), kp = ARG(1), vp = ARG(2), fp = ARG(3);
    if (!ap || !fp) RET(0, 4);
    const char *app = GS(ap), *key = kp ? GS(kp) : 0, *val = vp ? GS(vp) : 0, *file = GS(fp);
    size_t len = 0; char *b = ini_read(file, &len); if (!b) { b = calloc(1, 1); len = 0; }
    size_t cap = len + strlen(app) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 64;
    char *o = malloc(cap); size_t n = 0; int in_app = 0, done = 0, saw_app = 0;
#define EMIT(s, l) do { memcpy(o + n, (s), (l)); n += (l); } while (0)
    for (const char *p = b; *p; ) {
        const char *l = p, *le = strchr(p, '\n'); le = le ? le + 1 : p + strlen(p); p = le;
        const char *s = l, *e = le; trim(&s, &e);
        if (s < e && *s == '[') {
            if (in_app && !done && key && val) { n += (size_t)sprintf(o + n, "%s=%s\r\n", key, val); done = 1; }
            const char *c = memchr(s, ']', (size_t)(e - s)), *ss = s + 1, *se = c ? c : e; trim(&ss, &se);
            in_app = eq_ci(ss, se, app); saw_app |= in_app;
            if (in_app && !key) continue;                  /* deleting the section */
        } else if (in_app) {
            if (!key) continue;
            const char *eqp = memchr(s, '=', (size_t)(e - s));
            if (eqp) { const char *k = s, *ke = eqp; trim(&k, &ke);
                if (eq_ci(k, ke, key)) { if (val && !done) { n += (size_t)sprintf(o + n, "%s=%s\r\n", key, val); done = 1; } continue; } }
        }
        EMIT(l, (size_t)(le - l));
    }
    if (key && val && !done) {
        if (n && o[n - 1] != '\n') EMIT("\r\n", 2);
        if (!saw_app) n += (size_t)sprintf(o + n, "[%s]\r\n", app);
        n += (size_t)sprintf(o + n, "%s=%s\r\n", key, val);    /* the section was last, or is new */
    }
#undef EMIT
    char host[2048]; int ok = 0;
    if (!ini_host(file, host, sizeof host, 1)) { FILE *f = fopen(host, "wb"); if (f) { ok = fwrite(o, 1, n, f) == n; fclose(f); } }
    free(o); free(b); RET(ok, 4);
}

/* ---- console: none ---- */
IMPL(kernel32, GetConsoleCP) { RET(0, 0); }
IMPL(kernel32, GetConsoleOutputCP) { RET(0, 0); }
IMPL(kernel32, GetConsoleMode) { w32_set_last_error(c, 6); RET(0, 2); }
IMPL(kernel32, WriteConsoleA) { w32_set_last_error(c, 6); RET(0, 5); }
IMPL(kernel32, WriteConsoleW) { w32_set_last_error(c, 6); RET(0, 5); }
IMPL(kernel32, SetThreadIdealProcessor) { RET(0, 2); }
IMPL(kernel32, GetModuleHandleW)
{
    uint32_t w = ARG(0); if (!w) RET(w32_image_base, 1);
    char a[260]; int k = 0; for (; k < 259 && rt_r16(G_MEM, w + 2 * k); k++) a[k] = (char)rt_r16(G_MEM, w + 2 * k); a[k] = 0;
    uint32_t tmp = heap_alloc(w32_process_heap, 8, 260); strcpy((char *)GP(tmp), a);
    rt_w32(G_MEM, c->esp + 4, tmp); void imp_kernel32_GetModuleHandleA(Ctx *); imp_kernel32_GetModuleHandleA(c);
    heap_free(w32_process_heap, tmp);
}

/* ---- shell32 ---- */
IMPL(shell32, SHGetFolderPathA) { void imp_shfolder_SHGetFolderPathA(Ctx *); imp_shfolder_SHGetFolderPathA(c); }
IMPL(shell32, ShellExecuteA) { RET(2, 6); }                         /* "file not found": no browser launching */
IMPL(shell32, CommandLineToArgvW)                     /* (command line, int *argc): one allocation, argv then strings */
{
    uint32_t cl = ARG(0), pn = ARG(1); uint16_t buf[1024]; int n = 0, argc = 0, k = 0;
    while (n < 1023 && rt_r16(G_MEM, cl + 2 * n)) { buf[n] = (uint16_t)rt_r16(G_MEM, cl + 2 * n); n++; } buf[n] = 0;
    uint32_t mem = heap_alloc(w32_process_heap, 8, 64 * 4 + 2 * (uint32_t)(n + 64)); uint32_t str = mem + 64 * 4, w = str;
    while (k < n && argc < 63) {
        while (k < n && (buf[k] == ' ' || buf[k] == '\t')) k++;
        if (k >= n) break;
        rt_w32(G_MEM, mem + 4u * (uint32_t)argc++, w); int q = 0;
        for (; k < n && (q || (buf[k] != ' ' && buf[k] != '\t')); k++) { if (buf[k] == '"') { q = !q; continue; } rt_w16(G_MEM, w, buf[k]); w += 2; }
        rt_w16(G_MEM, w, 0); w += 2;
    }
    rt_w32(G_MEM, mem + 4u * (uint32_t)argc, 0);
    if (pn) rt_w32(G_MEM, pn, (uint32_t)argc);
    RET(mem, 2);
}

/* ---- user32 ---- */
IMPL(user32, EnumChildWindows) { RET(1, 3); }
IMPL(user32, EnumDisplayDevicesA)                     /* (device name, index, DISPLAY_DEVICEA *, flags): one adapter */
{
    uint32_t i = ARG(1), d = ARG(2);
    if (i > 0 || !d) RET(0, 4);
    uint32_t cb = rt_r32(G_MEM, d); memset(GP(d + 4), 0, cb > 4 && cb < 1024 ? cb - 4 : 420);
    strcpy((char *)GP(d + 4), "\\\\.\\DISPLAY1"); strcpy((char *)GP(d + 36), "Native Metal/OpenGL Display");
    rt_w32(G_MEM, d + 164, 5);                             /* ATTACHED_TO_DESKTOP | PRIMARY_DEVICE */
    RET(1, 4);
}
IMPL(user32, GetClassNameA) { uint32_t b = ARG(1), n = ARG(2); if (!n) RET(0, 3); snprintf((char *)GP(b), n, "%s", "Fallout: New Vegas"); RET(strlen(GS(b)), 3); }
IMPL(user32, SendInput) { RET(0, 3); }
IMPL(user32, SwapMouseButton) { RET(0, 1); }
IMPL(user32, UnhookWindowsHookEx) { RET(1, 1); }

/* ---- comctl32, ole32, d3d9 ---- */
IMPL(comctl32, InitCommonControlsEx) { RET(1, 1); }
IMPL(comctl32, ImageList_LoadImageA) { RET(0, 7); }
IMPL(comctl32, ImageList_Destroy) { RET(1, 1); }
IMPL(ole32, CoFreeUnusedLibraries) { RET(0, 0); }
IMPL(d3d9, D3DPERF_SetOptions) { RET(0, 1); }

/* ---- Steam/Galaxy (GalaxyWrp.dll exports the Steamworks flat API): not running ---- */
IMPL(galaxywrp, SteamAPI_Init) { RETC(0); }
IMPL(galaxywrp, SteamAPI_IsSteamRunning) { RETC(0); }
IMPL(galaxywrp, SteamAPI_RegisterCallback) { RETC(0); }
IMPL(galaxywrp, SteamAPI_UnregisterCallback) { RETC(0); }
IMPL(galaxywrp, SteamAPI_RunCallbacks) { RETC(0); }
IMPL(galaxywrp, SteamAPI_Shutdown) { RETC(0); }
IMPL(galaxywrp, SteamApps) { RETC(0); }
IMPL(galaxywrp, SteamFriends) { RETC(0); }
IMPL(galaxywrp, SteamUser) { RETC(0); }
IMPL(galaxywrp, SteamUserStats) { RETC(0); }
IMPL(galaxywrp, SteamUtils) { RETC(0); }

/* ---- XInput 1.3 by ordinal: 2 XInputGetState, 3 XInputSetState; no controller yet ---- */
IMPL(xinput1_3, ord2) { RET(1167, 2); }                            /* ERROR_DEVICE_NOT_CONNECTED */
IMPL(xinput1_3, ord3) { RET(1167, 2); }

/* ---- DirectSound 8 (DSOUND ordinal 11): the DirectSound object the runtime already has ---- */
IMPL(dsound, ord11) { void imp_dsound_DirectSoundCreate(Ctx *); imp_dsound_DirectSoundCreate(c); }

/* ---- WINMM multimedia I/O (RIFF/WAVE reading): the whole file or memory block stays in guest memory, so code that
 * reads through MMIOINFO.pchNext after mmioGetInfo (as the DirectX SDK's wave reader does) sees real bytes ---- */
typedef struct { uint32_t base, size, pos, owned; } Mmio;
#define NMMIO 64
static Mmio mmio[NMMIO]; static pthread_mutex_t mmio_lock = PTHREAD_MUTEX_INITIALIZER;
static Mmio *mm_get(uint32_t h) { return h >= 1 && h <= NMMIO && mmio[h - 1].base ? &mmio[h - 1] : 0; }
enum { MMIO_FINDCHUNK = 0x10, MMIO_FINDRIFF = 0x20, MMIO_FINDLIST = 0x40, FOURCC_RIFF = 0x46464952, FOURCC_LIST = 0x5453494c, FOURCC_MEM = 0x204d454d };
IMPL(winmm, mmioOpenA)                                /* (file name, MMIOINFO *, flags): reading only */
{
    uint32_t name = ARG(0), info = ARG(1), flags = ARG(2), base = 0, size = 0, owned = 0;
    if (flags & 3) { if (info) rt_w32(G_MEM, info + 12, 257); RET(0, 3); }                 /* write access: MMSYSERR_ERROR */
    if (info && rt_r32(G_MEM, info + 4) == FOURCC_MEM) { base = rt_r32(G_MEM, info + 24); size = rt_r32(G_MEM, info + 20); }
    else if (name) {
        char host[2048]; struct stat st; FILE *f = 0;
        if (!w32_host_path(GS(name), host, sizeof host, 0) && !stat(host, &st)) f = fopen(host, "rb");
        if (!f) { if (info) rt_w32(G_MEM, info + 12, 257); RET(0, 3); }
        size = (uint32_t)st.st_size; base = heap_alloc(w32_process_heap, 0, size ? size : 1); owned = 1;
        if (fread(GP(base), 1, size, f) != size) size = 0;
        fclose(f);
    }
    if (!base) RET(0, 3);
    pthread_mutex_lock(&mmio_lock); uint32_t h = 0;
    for (int k = 0; k < NMMIO; k++) if (!mmio[k].base) { mmio[k] = (Mmio){base, size, 0, owned}; h = (uint32_t)k + 1; break; }
    pthread_mutex_unlock(&mmio_lock);
    if (info) rt_w32(G_MEM, info + 12, 0);
    RET(h, 3);
}
IMPL(winmm, mmioClose)
{
    pthread_mutex_lock(&mmio_lock); Mmio *m = mm_get(ARG(0));
    if (m) { if (m->owned) heap_free(w32_process_heap, m->base); m->base = 0; }
    pthread_mutex_unlock(&mmio_lock); RET(m ? 0 : 5, 2);
}
IMPL(winmm, mmioRead)                                 /* (handle, buffer, bytes) -> bytes read */
{
    Mmio *m = mm_get(ARG(0)); if (!m) RET(0xffffffffu, 3);
    uint32_t n = ARG(2); if (n > m->size - m->pos) n = m->size - m->pos;
    memmove(GP(ARG(1)), GP(m->base + m->pos), n); m->pos += n; RET(n, 3);
}
IMPL(winmm, mmioDescend)                              /* (handle, MMCKINFO *, parent MMCKINFO *, flags) */
{
    Mmio *m = mm_get(ARG(0)); uint32_t ck = ARG(1), parent = ARG(2), flags = ARG(3);
    if (!m) RET(5, 4);
    uint32_t end = m->size;
    if (parent) { uint32_t e = rt_r32(G_MEM, parent + 12) + rt_r32(G_MEM, parent + 4); if (e < end) end = e; }
    uint32_t want = rt_r32(G_MEM, ck), wtype = rt_r32(G_MEM, ck + 8);
    for (;;) {
        if (m->pos + 8 > end) RET(265, 4);                                                  /* MMIOERR_CHUNKNOTFOUND */
        uint32_t id = rt_r32(G_MEM, m->base + m->pos), sz = rt_r32(G_MEM, m->base + m->pos + 4), at = m->pos + 8, type = 0;
        int list = id == FOURCC_RIFF || id == FOURCC_LIST;
        if (list && at + 4 <= end) type = rt_r32(G_MEM, m->base + at);
        int match = !(flags & (MMIO_FINDCHUNK | MMIO_FINDRIFF | MMIO_FINDLIST)) ||
                    ((flags & MMIO_FINDCHUNK) && id == want) ||
                    ((flags & MMIO_FINDRIFF) && id == FOURCC_RIFF && type == wtype) ||
                    ((flags & MMIO_FINDLIST) && id == FOURCC_LIST && type == wtype);
        if (match) {
            rt_w32(G_MEM, ck, id); rt_w32(G_MEM, ck + 4, sz); rt_w32(G_MEM, ck + 8, type);
            rt_w32(G_MEM, ck + 12, at); rt_w32(G_MEM, ck + 16, 0);
            m->pos = at + (list ? 4 : 0); RET(0, 4);
        }
        m->pos = at + sz + (sz & 1);
    }
}
IMPL(winmm, mmioAscend)                               /* (handle, MMCKINFO *, flags): past the chunk (padded to even) */
{
    Mmio *m = mm_get(ARG(0)); uint32_t ck = ARG(1); if (!m) RET(5, 3);
    uint32_t e = rt_r32(G_MEM, ck + 12) + rt_r32(G_MEM, ck + 4); e += e & 1;
    m->pos = e < m->size ? e : m->size; RET(0, 3);
}
IMPL(winmm, mmioGetInfo)                              /* (handle, MMIOINFO *, flags): the buffer is the data itself */
{
    Mmio *m = mm_get(ARG(0)); uint32_t i = ARG(1); if (!m) RET(5, 3);
    memset(GP(i), 0, 72);
    rt_w32(G_MEM, i + 4, FOURCC_MEM); rt_w32(G_MEM, i + 20, m->size); rt_w32(G_MEM, i + 24, m->base);
    rt_w32(G_MEM, i + 28, m->base + m->pos); rt_w32(G_MEM, i + 32, m->base + m->size); rt_w32(G_MEM, i + 36, m->base + m->size);
    rt_w32(G_MEM, i + 68, ARG(0)); RET(0, 3);
}
IMPL(winmm, mmioAdvance)                              /* (handle, MMIOINFO *, flags): everything is already "buffered" */
{
    Mmio *m = mm_get(ARG(0)); uint32_t i = ARG(1); if (!m) RET(5, 3);
    if (i) { uint32_t next = rt_r32(G_MEM, i + 28); if (next >= m->base && next <= m->base + m->size) m->pos = next - m->base;
             rt_w32(G_MEM, i + 28, m->base + m->pos); rt_w32(G_MEM, i + 32, m->base + m->size); }
    RET(0, 3);
}

/* ---- WSOCK32 101 (WSAAsyncSelect): accepted, no window messages ---- */
IMPL(wsock32, ord101) { RET(0, 4); }
