/* PE resources (the game's own image in guest memory, or another PE file on disk), VERSION.dll, and assorted
 * KERNEL32 text/resource functions. */
#include "w32.h"
#include "edition.h"
extern const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[]; extern const unsigned rt_images_n;

/* resource directory lookup: base = image base, rsrc = guest address of the resource directory root */
static uint32_t rsrc_root(uint32_t base)
{
    uint32_t pe = base + rt_r32(G_MEM, base + 0x3c);
    uint32_t rva = rt_r32(G_MEM, pe + 24 + 96 + 8 * 2);
    return rva ? base + rva : 0;
}
static int name_eq(uint32_t root, uint32_t name_off, const char *s)
{
    uint32_t p = root + (name_off & 0x7fffffffu); uint16_t n = (uint16_t)rt_r16(G_MEM, p);
    if (strlen(s) != n) return 0;
    for (uint16_t k = 0; k < n; k++) { uint16_t ch = (uint16_t)rt_r16(G_MEM, p + 2 + 2 * k); char a = s[k];
        if ((ch < 128 ? (ch | 32) : ch) != (uint16_t)((a >= 'A' && a <= 'Z') ? a | 32 : a)) return 0; }
    return 1;
}
/* find the entry matching id (< 0x10000) or name (guest string, "#123" allowed) in directory dir; -1: first entry */
static uint32_t dir_find(uint32_t root, uint32_t dir, uint32_t id_or_name)
{
    uint16_t nn = (uint16_t)rt_r16(G_MEM, dir + 12), ni = (uint16_t)rt_r16(G_MEM, dir + 14);
    uint32_t want_id = 0xffffffffu; const char *want = 0;
    if (id_or_name == 0xffffffffu) return nn + ni ? rt_r32(G_MEM, dir + 16 + 4) : 0;
    if (id_or_name < 0x10000) want_id = id_or_name; else { want = GS(id_or_name); if (want[0] == '#') { want_id = (uint32_t)atoi(want + 1); want = 0; } }
    for (uint32_t k = 0; k < (uint32_t)nn + ni; k++) {
        uint32_t e = dir + 16 + 8 * k, nm = rt_r32(G_MEM, e);
        if (want ? ((nm & 0x80000000u) && name_eq(root, nm, want)) : (!(nm & 0x80000000u) && nm == want_id)) return rt_r32(G_MEM, e + 4);
    }
    return 0;
}
/* returns the guest address of the IMAGE_RESOURCE_DATA_ENTRY (an HRSRC), or 0 */
static uint32_t find_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t lang)
{
    uint32_t root = rsrc_root(base); if (!root) return 0;
    uint32_t e = dir_find(root, root, type); if (!e || !(e & 0x80000000u)) return 0;
    e = dir_find(root, root + (e & 0x7fffffffu), name); if (!e || !(e & 0x80000000u)) return 0;
    uint32_t d = root + (e & 0x7fffffffu), l = dir_find(root, d, lang);
    if (!l) l = dir_find(root, d, 0xffffffffu);
    return l && !(l & 0x80000000u) ? root + l : 0;
}
static uint32_t base_of(uint32_t h)                     /* the executable, or a mapped recompiled DLL */
{
    if (!h || h == w32_image_base) return w32_image_base;
    for (unsigned k = 1; k < rt_images_n; k++) if (h == rt_images[k].base && rt_r16(G_MEM, h) == 0x5a4d) return h;
    return 0;
}
IMPL(kernel32, FindResourceA) { uint32_t b = base_of(ARG(0)); RET(b ? find_resource(b, ARG(2), ARG(1), 0x409) : 0, 3); }
IMPL(kernel32, FindResourceExA) { uint32_t b = base_of(ARG(0)); RET(b ? find_resource(b, ARG(1), ARG(2), ARG(3) & 0xffff) : 0, 4); }
IMPL(kernel32, LoadResource) { uint32_t r = ARG(1), b = base_of(ARG(0)); RET(r && b ? b + rt_r32(G_MEM, r) : 0, 2); }
IMPL(kernel32, LockResource) { RET(ARG(0), 1); }
IMPL(kernel32, SizeofResource) { uint32_t r = ARG(1); RET(r ? rt_r32(G_MEM, r + 4) : 0, 2); }
static int enum_dir(Ctx *c, uint32_t root, uint32_t dir, uint32_t fn, const uint32_t *pre, int npre, uint32_t lp, int want_lang)
{
    uint16_t nn = (uint16_t)rt_r16(G_MEM, dir + 12), ni = (uint16_t)rt_r16(G_MEM, dir + 14);
    for (uint32_t k = 0; k < (uint32_t)nn + ni; k++) {
        uint32_t nm = rt_r32(G_MEM, dir + 16 + 8 * k), arg, buf = 0;
        if (nm & 0x80000000u) {
            uint32_t p = root + (nm & 0x7fffffffu); uint16_t n = (uint16_t)rt_r16(G_MEM, p);
            buf = heap_alloc(w32_process_heap, 8, n + 1u); w32_wide_to_mb(1252, GW(p + 2), n, (uint8_t *)GP(buf), n); arg = buf;
        } else arg = want_lang ? (nm & 0xffff) : nm;
        uint32_t a[5]; int na = 0; for (int j = 0; j < npre; j++) a[na++] = pre[j];
        a[na++] = arg; a[na++] = lp;
        uint32_t r = w32_callback(c, fn, na, a);
        if (buf) heap_free(w32_process_heap, buf);
        if (!r) return 0;
    }
    return 1;
}
IMPL(kernel32, EnumResourceTypesA)
{
    uint32_t b = base_of(ARG(0)), root = b ? rsrc_root(b) : 0; if (!root) RET(0, 3);
    uint32_t pre[1] = {ARG(0) ? ARG(0) : b}; enum_dir(c, root, root, ARG(1), pre, 1, ARG(2), 0); RET(1, 3);
}
IMPL(kernel32, EnumResourceNamesA)
{
    uint32_t b = base_of(ARG(0)), root = b ? rsrc_root(b) : 0; if (!root) RET(0, 4);
    uint32_t e = dir_find(root, root, ARG(1)); if (!e || !(e & 0x80000000u)) { w32_set_last_error(c, 1813); RET(0, 4); }
    uint32_t pre[2] = {ARG(0) ? ARG(0) : b, ARG(1)}; enum_dir(c, root, root + (e & 0x7fffffffu), ARG(2), pre, 2, ARG(3), 0); RET(1, 4);
}
IMPL(kernel32, EnumResourceLanguagesA)
{
    uint32_t b = base_of(ARG(0)), root = b ? rsrc_root(b) : 0; if (!root) RET(0, 5);
    uint32_t e = dir_find(root, root, ARG(1)); if (!e || !(e & 0x80000000u)) RET(0, 5);
    e = dir_find(root, root + (e & 0x7fffffffu), ARG(2)); if (!e || !(e & 0x80000000u)) RET(0, 5);
    uint32_t pre[3] = {ARG(0) ? ARG(0) : b, ARG(1), ARG(2)}; enum_dir(c, root, root + (e & 0x7fffffffu), ARG(3), pre, 3, ARG(4), 1); RET(1, 5);
}

/* ---- VERSION.dll: version resources of the game's own executable and its recompiled DLLs (other files: none) ---- */
static uint32_t own_image(const char *path)
{
    const char *s = strrchr(path, '\\'), *t = strrchr(path, '/'); if (t > s) s = t; s = s ? s + 1 : path;
    if (!strcasecmp(s, GAME_EXE)) return w32_image_base;
    for (unsigned k = 1; k < rt_images_n; k++) {
        const char *n = rt_images[k].name, *b = strrchr(n, '/'); b = b ? b + 1 : n;
        if (!strcasecmp(s, b) && rt_r16(G_MEM, rt_images[k].base) == 0x5a4d) return rt_images[k].base;   /* mapped */
    }
    return 0;
}
static uint32_t own_version_of(uint32_t base, uint32_t *len)
{
    uint32_t r = base ? find_resource(base, 16, 1, 0x409) : 0;
    if (!r) return 0;
    *len = rt_r32(G_MEM, r + 4); return base + rt_r32(G_MEM, r);
}
IMPL(version, GetFileVersionInfoSizeA)
{
    uint32_t len = 0; if (ARG(1)) rt_w32(G_MEM, ARG(1), 0);
    if (!own_version_of(own_image(GS(ARG(0))), &len)) { w32_set_last_error(c, 1813); RET(0, 2); }
    RET(len * 3, 2);                                   /* room for ANSI copies of the strings, like Windows */
}
IMPL(version, GetFileVersionInfoA)
{
    uint32_t len = 0, src = own_version_of(own_image(GS(ARG(0))), &len), n = ARG(2), data = ARG(3);
    if (!src) { w32_set_last_error(c, 1813); RET(0, 4); }
    memset(GP(data), 0, n); memcpy(GP(data), GP(src), len < n ? len : n);
    rt_w32(G_MEM, data + len, 0);                      /* where ANSI conversions go (after the block) */
    RET(1, 4);
}
/* VS_VERSIONINFO (Unicode): { u16 wLength, wValueLength, wType; WCHAR szKey[]; pad to 4; value; pad; children } */
static uint32_t blk_value(uint32_t b, uint32_t *vlen, uint32_t *type)
{
    uint32_t p = b + 6; while (rt_r16(G_MEM, p)) p += 2; p += 2; p = (p + 3) & ~3u;
    *vlen = rt_r16(G_MEM, b + 2); *type = rt_r16(G_MEM, b + 4); return p;
}
static int key_eq(uint32_t b, const char *k, size_t n)
{
    uint32_t p = b + 6; size_t j = 0;
    for (; j < n; j++, p += 2) { uint16_t ch = (uint16_t)rt_r16(G_MEM, p); char a = k[j]; if ((ch | 32) != (uint16_t)(a | 32)) return 0; }
    return rt_r16(G_MEM, p) == 0;
}
IMPL(version, VerQueryValueA)
{
    uint32_t block = ARG(0), outp = ARG(2), lenp = ARG(3);
    const char *q = GS(ARG(1)); uint32_t b = block;
    while (*q == '\\') q++;
    while (*q) {
        const char *e = strchr(q, '\\'); size_t n = e ? (size_t)(e - q) : strlen(q);
        uint32_t vlen, type, v = blk_value(b, &vlen, &type), end = b + rt_r16(G_MEM, b), ch = (v + (type ? 2 * vlen : vlen) + 3) & ~3u, found = 0;
        while (ch < end) { if (key_eq(ch, q, n)) { found = ch; break; } uint32_t l = rt_r16(G_MEM, ch); if (!l) break; ch = (ch + l + 3) & ~3u; }
        if (!found) RET(0, 4);
        b = found; q += n; while (*q == '\\') q++;
    }
    uint32_t vlen, type, v = blk_value(b, &vlen, &type);
    if (type == 1) {                                   /* string: return an ANSI copy stored after the block */
        uint32_t total = rt_r16(G_MEM, block), pool = block + total, used = rt_r32(G_MEM, pool);
        uint32_t dst = pool + 4 + used; int len = w32_wide_to_mb(1252, GW(v), (int)vlen, (uint8_t *)GP(dst), 512);
        rt_w8(G_MEM, dst + (uint32_t)len, 0); rt_w32(G_MEM, pool, used + (uint32_t)len + 1);
        rt_w32(G_MEM, outp, dst); if (lenp) rt_w32(G_MEM, lenp, vlen);
    } else { rt_w32(G_MEM, outp, v); if (lenp) rt_w32(G_MEM, lenp, vlen); }
    RET(vlen != 0 || type == 1, 4);
}
IMPL(kernel32, VerLanguageNameA)
{
    const char *s = (ARG(0) & 0x3ff) == 9 ? "English (United States)" : "Language Neutral"; uint32_t buf = ARG(1), cap = ARG(2), n = (uint32_t)strlen(s);
    if (buf && cap) { uint32_t m = n < cap - 1 ? n : cap - 1; memcpy(GP(buf), s, m); rt_w8(G_MEM, buf + m, 0); }
    RET(n, 3);
}

/* ---- assorted text functions ---- */
IMPL(kernel32, ExpandEnvironmentStringsA)
{
    extern uint32_t w32_env_a; const char *s = GS(ARG(0)); uint32_t out = ARG(1), cap = ARG(2); char buf[4096]; size_t o = 0;
    while (*s && o < sizeof buf - 1) {
        const char *e;
        if (*s == '%' && (e = strchr(s + 1, '%'))) {
            size_t nl = (size_t)(e - s - 1); const char *val = 0;
            for (uint32_t p = w32_env_a; G_MEM[p]; p += (uint32_t)strlen(GS(p)) + 1) if (!strncasecmp(GS(p), s + 1, nl) && GS(p)[nl] == '=') { val = GS(p) + nl + 1; break; }
            if (val) { size_t l = strlen(val); if (o + l >= sizeof buf) break; memcpy(buf + o, val, l); o += l; s = e + 1; continue; }
        }
        buf[o++] = *s++;
    }
    buf[o++] = 0;
    if (o <= cap) memcpy(GP(out), buf, o);
    RET((uint32_t)o, 3);
}
IMPL(kernel32, FormatMessageA)
{
    uint32_t flags = ARG(0), id = ARG(2), buf = ARG(4), cap = ARG(5); char s[256];
    snprintf(s, sizeof s, "Error %u.\r\n", id);
    uint32_t n = (uint32_t)strlen(s);
    if (flags & 0x100) {                               /* FORMAT_MESSAGE_ALLOCATE_BUFFER */
        uint32_t p = heap_alloc(w32_process_heap, 0, n + 1); memcpy(GP(p), s, n + 1); rt_w32(G_MEM, buf, p);
    } else { if (n + 1 > cap) RET(0, 7); memcpy(GP(buf), s, n + 1); }
    RET(n, 7);
}
static int fmt_date_time(uint32_t st, const char *fmt, char *out, size_t cap, int date)
{
    static const char *days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    uint16_t v[8]; memcpy(v, GP(st), 16); size_t o = 0;
    if (!fmt) fmt = date ? "M/d/yyyy" : "h:mm:ss tt";
    while (*fmt && o < cap - 16) {
        char ch = *fmt; int n = 1; while (fmt[n] == ch) n++;
        if (ch == '\'') { fmt++; while (*fmt && *fmt != '\'') out[o++] = *fmt++; if (*fmt) fmt++; continue; }
        int h12 = v[4] % 12 ? v[4] % 12 : 12;
        if (date && ch == 'd') o += (size_t)(n >= 4 ? snprintf(out + o, cap - o, "%s", days[v[2] % 7]) : n == 3 ? snprintf(out + o, cap - o, "%.3s", days[v[2] % 7]) : snprintf(out + o, cap - o, n == 2 ? "%02u" : "%u", v[3]));
        else if (date && ch == 'M') o += (size_t)(n >= 4 ? snprintf(out + o, cap - o, "%s", months[(v[1] + 11) % 12]) : n == 3 ? snprintf(out + o, cap - o, "%.3s", months[(v[1] + 11) % 12]) : snprintf(out + o, cap - o, n == 2 ? "%02u" : "%u", v[1]));
        else if (date && ch == 'y') o += (size_t)(n >= 3 ? snprintf(out + o, cap - o, "%u", v[0]) : snprintf(out + o, cap - o, "%02u", v[0] % 100));
        else if (!date && ch == 'h') o += (size_t)snprintf(out + o, cap - o, n == 2 ? "%02d" : "%d", h12);
        else if (!date && ch == 'H') o += (size_t)snprintf(out + o, cap - o, n == 2 ? "%02u" : "%u", v[4]);
        else if (!date && ch == 'm') o += (size_t)snprintf(out + o, cap - o, n == 2 ? "%02u" : "%u", v[5]);
        else if (!date && ch == 's') o += (size_t)snprintf(out + o, cap - o, n == 2 ? "%02u" : "%u", v[6]);
        else if (!date && ch == 't') o += (size_t)snprintf(out + o, cap - o, "%.*s", n == 1 ? 1 : 2, v[4] < 12 ? "AM" : "PM");
        else { for (int j = 0; j < n; j++) out[o++] = ch; }
        fmt += n;
    }
    out[o] = 0; return (int)o + 1;
}
static uint32_t date_time(Ctx *c, int date)
{
    uint32_t st = ARG(2), fmt = ARG(3), out = ARG(4), cap = ARG(5), tmp = 0; char s[256];
    if (!st) { tmp = heap_alloc(w32_process_heap, 0, 16); uint32_t a = tmp; (void)a;
               uint64_t ft; extern void w32_now_systemtime(uint32_t); w32_now_systemtime(tmp); st = tmp; (void)ft; }
    int n = fmt_date_time(st, fmt ? GS(fmt) : 0, s, sizeof s, date);
    if (tmp) heap_free(w32_process_heap, tmp);
    if (!cap) return (uint32_t)n;
    if ((uint32_t)n > cap) { w32_set_last_error(c, 122); return 0; }
    memcpy(GP(out), s, (size_t)n); return (uint32_t)n;
}
IMPL(kernel32, GetDateFormatA) { RET(date_time(c, 1), 6); }
IMPL(kernel32, GetTimeFormatA) { RET(date_time(c, 0), 6); }
IMPL(kernel32, CreateProcessA) { fprintf(stderr, "w32: CreateProcessA(%s, %s) refused\n", ARG(0) ? GS(ARG(0)) : "", ARG(1) ? GS(ARG(1)) : ""); w32_set_last_error(c, 2); RET(0, 10); }
IMPL(kernel32, OpenProcess) { w32_set_last_error(c, 5); RET(0, 3); }
IMPL(kernel32, CreateToolhelp32Snapshot) { w32_set_last_error(c, 5); RET(0xffffffffu, 2); }
IMPL(kernel32, Thread32First) { w32_set_last_error(c, 18); RET(0, 2); }
IMPL(kernel32, Thread32Next) { w32_set_last_error(c, 18); RET(0, 2); }
