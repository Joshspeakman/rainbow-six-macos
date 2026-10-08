/* Smaller KERNEL32/USER32 functions the recompiled Miles DLLs import. */
#include "w32.h"
#include <time.h>
#include <unistd.h>
#include <ctype.h>

uint32_t heap_realloc(uint32_t handle, uint32_t flags, uint32_t p, uint32_t size);
IMPL(kernel32, HeapReAlloc) { RET(heap_realloc(ARG(0), ARG(1), ARG(2), ARG(3)), 4); }
IMPL(kernel32, GlobalHandle) { RET(ARG(0), 1); }                                    /* GMEM_FIXED: the pointer is the handle */
IMPL(kernel32, DisableThreadLibraryCalls) { RET(1, 1); }
IMPL(kernel32, SetErrorMode) { static uint32_t mode; uint32_t o = mode; mode = ARG(0); RET(o, 1); }
IMPL(kernel32, SetThreadPriority) { RET(1, 2); }
IMPL(kernel32, SetEnvironmentVariableA) { RET(1, 2); }                              /* the process environment block is fixed */
IMPL(kernel32, lstrcatA) { uint32_t d = ARG(0); strcat((char *)GP(d), GS(ARG(1))); RET(d, 2); }
static uint32_t put_dir(uint32_t buf, uint32_t cap, const char *s)
{
    uint32_t n = (uint32_t)strlen(s); if (n + 1 > cap) return n + 1;
    memcpy(GP(buf), s, n + 1); return n;
}
IMPL(kernel32, GetWindowsDirectoryA) { RET(put_dir(ARG(0), ARG(1), "C:\\Windows"), 2); }
IMPL(kernel32, GetSystemDirectoryA) { RET(put_dir(ARG(0), ARG(1), "C:\\Windows\\system32"), 2); }
IMPL(kernel32, GetProfileStringA)                                                   /* no win.ini: the default */
{
    uint32_t def = ARG(2), buf = ARG(3), cap = ARG(4);
    if (!cap) RET(0, 5);
    const char *d = def ? GS(def) : ""; uint32_t n = (uint32_t)strlen(d); if (n > cap - 1) n = cap - 1;
    memcpy(GP(buf), d, n); G_MEM[buf + n] = 0; RET(n, 5);
}
IMPL(kernel32, GetTimeZoneInformation)
{
    uint32_t p = ARG(0); memset(GP(p), 0, 172);
    time_t t = time(0); struct tm lt; localtime_r(&t, &lt);
    rt_w32(G_MEM, p, (uint32_t)(int32_t)(-lt.tm_gmtoff / 60));                      /* Bias: UTC = local + bias */
    RET(0, 1);                                                                      /* TIME_ZONE_ID_UNKNOWN: no transition rules */
}
static int cmp_str(const char *a, int na, const char *b, int nb, int nocase)
{
    if (na < 0) na = (int)strlen(a); if (nb < 0) nb = (int)strlen(b);
    for (int i = 0; i < na && i < nb; i++) {
        int x = (uint8_t)a[i], y = (uint8_t)b[i];
        if (nocase) { x = tolower(x); y = tolower(y); }
        if (x != y) return x < y ? 1 : 3;
    }
    return na < nb ? 1 : na > nb ? 3 : 2;
}
IMPL(kernel32, CompareStringA) { RET(cmp_str(GS(ARG(2)), (int)ARG(3), GS(ARG(4)), (int)ARG(5), ARG(1) & 1), 6); }
IMPL(kernel32, CompareStringW)
{
    char a[1024], b[1024]; int na = (int)ARG(3), nb = (int)ARG(5);
    uint32_t pa = ARG(2), pb = ARG(4); int i;
    for (i = 0; i < 1023 && (na < 0 ? rt_r16(G_MEM, pa + 2 * i) : i < na); i++) a[i] = (char)rt_r16(G_MEM, pa + 2 * i); a[i] = 0; na = i;
    for (i = 0; i < 1023 && (nb < 0 ? rt_r16(G_MEM, pb + 2 * i) : i < nb); i++) b[i] = (char)rt_r16(G_MEM, pb + 2 * i); b[i] = 0; nb = i;
    RET(cmp_str(a, na, b, nb, ARG(1) & 1), 6);
}
IMPL(kernel32, SetEndOfFile)
{
    extern int w32_file_fd(uint32_t h);
    int fd = w32_file_fd(ARG(0)); if (fd < 0) { w32_set_last_error(c, 6); RET(0, 1); }
    off_t at = lseek(fd, 0, SEEK_CUR); RET(ftruncate(fd, at) == 0, 1);
}
/* OpenFile: only the existence check (OF_EXIST) and plain opens via CreateFileA's path mapping */
IMPL(kernel32, OpenFile)
{
    const char *name = GS(ARG(0)); uint32_t of = ARG(1), style = ARG(2); char host[2048];
    int ok = !w32_host_path(name, host, sizeof host, 0) && access(host, F_OK) == 0;
    if (of) { memset(GP(of), 0, 136); G_MEM[of] = 136; snprintf((char *)GP(of + 8), 128, "%s", name); }
    if (style & 0x4000) RET(ok ? 1 : 0xffffffffu, 3);                               /* OF_EXIST */
    if (style & 0x0300) { fprintf(stderr, "w32: OpenFile(%s, %#x) not supported\n", name, style); RET(0xffffffffu, 3); }
    /* OF_READ / OF_WRITE / OF_READWRITE, OF_CREATE: CreateFileA(name, access, share, 0, disposition, normal, 0) on a
     * frame built below ours (4 more arguments); the HFILE is the file handle */
    uint32_t access = (style & 3) == 1 ? 0x40000000u : (style & 3) == 2 ? 0xc0000000u : 0x80000000u;
    uint32_t share = (style & 0x70) == 0x10 ? 0 : (style & 0x70) == 0x20 ? 1 : (style & 0x70) == 0x30 ? 2 : 3;
    uint32_t esp0 = c->esp, ret = rt_r32(G_MEM, esp0), a[7] = {ARG(0), access, share, 0, (style & 0x1000) ? 2u : 3u, 0x80, 0};
    c->esp = esp0 - 16; rt_w32(G_MEM, c->esp, ret); for (int k = 0; k < 7; k++) rt_w32(G_MEM, c->esp + 4 + 4 * (uint32_t)k, a[k]);
    void imp_kernel32_CreateFileA(Ctx *); imp_kernel32_CreateFileA(c);
    c->esp = esp0 + 16;
}
/* ---- USER32 ---- */
IMPL(user32, GetTopWindow) { extern uint32_t w32_top_window(void); RET(ARG(0) ? 0 : w32_top_window(), 1); }
IMPL(user32, GetWindowThreadProcessId)
{
    extern uint32_t w32_window_tid(uint32_t hwnd); uint32_t tid = w32_window_tid(ARG(0));
    if (ARG(1)) rt_w32(G_MEM, ARG(1), tid ? 1 : 0);
    RET(tid, 2);
}
/* wsprintfA (cdecl): %[-][0][width][.prec][l|h]{d,i,u,x,X,c,s} and %% */
uint32_t w32_format(char *out, size_t cap, const char *f, uint32_t va)
{
    size_t n = 0;
#define PUT(ch) do { if (n + 1 < cap) out[n] = (ch); n++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++; if (*f == '%') { PUT('%'); continue; }
        int left = 0, zero = 0, width = 0, prec = -1;
        for (; *f == '-' || *f == '0' || *f == '#'; f++) { if (*f == '-') left = 1; if (*f == '0') zero = 1; }
        while (isdigit((uint8_t)*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { prec = 0; f++; while (isdigit((uint8_t)*f)) prec = prec * 10 + (*f++ - '0'); }
        while (*f == 'l' || *f == 'h') f++;
        char tmp[64]; const char *s = tmp; size_t len;
        uint32_t v = rt_r32(G_MEM, va); va += 4;
        switch (*f) {
        case 'd': case 'i': snprintf(tmp, sizeof tmp, "%d", (int32_t)v); break;
        case 'u': snprintf(tmp, sizeof tmp, "%u", v); break;
        case 'x': snprintf(tmp, sizeof tmp, "%x", v); break;
        case 'X': snprintf(tmp, sizeof tmp, "%X", v); break;
        case 'c': tmp[0] = (char)v; tmp[1] = 0; break;
        case 's': s = v ? GS(v) : "(null)"; break;
        default: tmp[0] = 0; if (!*f) f--; break;
        }
        len = strlen(s); if (*f == 's' && prec >= 0 && (size_t)prec < len) len = (size_t)prec;
        int pad = width > (int)len ? width - (int)len : 0;
        if (!left) while (pad-- > 0) PUT(zero && *f != 's' ? '0' : ' ');
        for (size_t i = 0; i < len; i++) PUT(s[i]);
        if (left) while (pad-- > 0) PUT(' ');
    }
    if (cap) out[n < cap ? n : cap - 1] = 0;
#undef PUT
    return (uint32_t)n;
}
IMPL(user32, wsprintfA)
{
    char buf[1025]; uint32_t n = w32_format(buf, sizeof buf, GS(ARG(1)), c->esp + 12);
    if (n > 1024) n = 1024;
    memcpy(GP(ARG(0)), buf, n + 1);
    RETC(n);
}
