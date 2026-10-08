/* Code pages (1252 and UTF-8), character classification, case mapping and locale data (English, US). */
#include "w32.h"

static const uint16_t cp1252_hi[32] = {
    0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008d, 0x017d, 0x008f,
    0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122, 0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178};
static uint16_t to_wide1252(uint8_t b) { return b >= 0x80 && b < 0xa0 ? cp1252_hi[b - 0x80] : b; }
static int from_wide1252(uint16_t w)
{
    if (w < 0x80 || (w >= 0xa0 && w < 0x100)) return w;
    for (int k = 0; k < 32; k++) if (cp1252_hi[k] == w) return 0x80 + k;
    return -1;
}
int w32_mb_to_wide(uint32_t cp, const uint8_t *s, int n, uint16_t *out, int cap)
{
    if (n < 0) n = (int)strlen((const char *)s) + 1;
    int o = 0;
    for (int k = 0; k < n; ) {
        uint32_t w;
        if (cp == 65001) {
            uint8_t b = s[k];
            if (b < 0x80) { w = b; k++; }
            else if ((b & 0xe0) == 0xc0 && k + 1 < n) { w = ((b & 0x1fu) << 6) | (s[k + 1] & 0x3fu); k += 2; }
            else if ((b & 0xf0) == 0xe0 && k + 2 < n) { w = ((b & 0x0fu) << 12) | ((s[k + 1] & 0x3fu) << 6) | (s[k + 2] & 0x3fu); k += 3; }
            else { w = 0xfffd; k++; }
        } else w = to_wide1252(s[k++]);
        if (cap) { if (o >= cap) return 0; out[o] = (uint16_t)w; }
        o++;
    }
    return o;
}
int w32_wide_to_mb(uint32_t cp, const uint16_t *s, int n, uint8_t *out, int cap)
{
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    int o = 0;
    for (int k = 0; k < n; k++) {
        uint16_t w = s[k]; uint8_t buf[3]; int m;
        if (cp == 65001) {
            if (w < 0x80) { buf[0] = (uint8_t)w; m = 1; }
            else if (w < 0x800) { buf[0] = (uint8_t)(0xc0 | (w >> 6)); buf[1] = (uint8_t)(0x80 | (w & 0x3f)); m = 2; }
            else { buf[0] = (uint8_t)(0xe0 | (w >> 12)); buf[1] = (uint8_t)(0x80 | ((w >> 6) & 0x3f)); buf[2] = (uint8_t)(0x80 | (w & 0x3f)); m = 3; }
        } else { int b = from_wide1252(w); buf[0] = (uint8_t)(b < 0 ? '?' : b); m = 1; }
        for (int j = 0; j < m; j++) { if (cap) { if (o >= cap) return 0; out[o] = buf[j]; } o++; }
    }
    return o;
}
static uint32_t cp_of(uint32_t cp) { return cp == 0 || cp == 1 || cp == 3 || cp == 437 || cp == 850 ? 1252 : cp; }

IMPL(kernel32, MultiByteToWideChar)
{
    uint32_t cp = cp_of(ARG(0)), src = ARG(2), dst = ARG(4); int n = (int)ARG(3), cap = (int)ARG(5);
    int r = w32_mb_to_wide(cp, (const uint8_t *)GP(src), n, cap ? (uint16_t *)GP(dst) : 0, cap);
    if (!r) w32_set_last_error(c, 122);
    RET(r, 6);
}
IMPL(kernel32, WideCharToMultiByte)
{
    uint32_t cp = cp_of(ARG(0)), src = ARG(2), dst = ARG(4), used = ARG(7); int n = (int)ARG(3), cap = (int)ARG(5);
    int r = w32_wide_to_mb(cp, (const uint16_t *)GP(src), n, cap ? (uint8_t *)GP(dst) : 0, cap);
    if (used) rt_w32(G_MEM, used, 0);
    if (!r) w32_set_last_error(c, 122);
    RET(r, 8);
}

/* CT_CTYPE1 flags for a UTF-16 code unit (Basic Latin, Latin-1 and the cp1252 extras) */
enum { C1_UPPER = 1, C1_LOWER = 2, C1_DIGIT = 4, C1_SPACE = 8, C1_PUNCT = 0x10, C1_CNTRL = 0x20, C1_BLANK = 0x40, C1_XDIGIT = 0x80, C1_ALPHA = 0x100, C1_DEFINED = 0x200 };
static int is_upper(uint16_t w) { return (w >= 'A' && w <= 'Z') || (w >= 0xc0 && w <= 0xde && w != 0xd7) || w == 0x160 || w == 0x152 || w == 0x17d || w == 0x178; }
static int is_lower(uint16_t w) { return (w >= 'a' && w <= 'z') || (w >= 0xdf && w <= 0xff && w != 0xf7) || w == 0x161 || w == 0x153 || w == 0x17e || w == 0x192 || w == 0xaa || w == 0xb5 || w == 0xba; }
static uint16_t ctype1(uint16_t w)
{
    uint16_t f = C1_DEFINED;
    if (w < 0x20 || (w >= 0x7f && w < 0xa0)) { f |= C1_CNTRL; if ((w >= 9 && w <= 13)) f |= C1_SPACE; if (w == 9) f |= C1_BLANK; return f; }
    if (w == ' ' || w == 0xa0) return f | C1_SPACE | C1_BLANK;
    if (w >= '0' && w <= '9') return f | C1_DIGIT | C1_XDIGIT;
    if (is_upper(w)) { f |= C1_UPPER | C1_ALPHA; if (w >= 'A' && w <= 'F') f |= C1_XDIGIT; return f; }
    if (is_lower(w)) { f |= C1_LOWER | C1_ALPHA; if (w >= 'a' && w <= 'f') f |= C1_XDIGIT; return f; }
    if (w == 0x2c6 || w == 0x2dc) return f | C1_ALPHA;            /* modifier letters */
    if (w == 0xb2 || w == 0xb3 || w == 0xb9) return f | C1_DIGIT | C1_PUNCT;
    return f | C1_PUNCT;
}
static uint16_t wupper(uint16_t w)
{
    if (w >= 'a' && w <= 'z') return (uint16_t)(w - 32);
    if (w >= 0xe0 && w <= 0xfe && w != 0xf7) return (uint16_t)(w - 32);
    if (w == 0xff) return 0x178; if (w == 0x161) return 0x160; if (w == 0x153) return 0x152; if (w == 0x17e) return 0x17d;
    return w;
}
static uint16_t wlower(uint16_t w)
{
    if (w >= 'A' && w <= 'Z') return (uint16_t)(w + 32);
    if (w >= 0xc0 && w <= 0xde && w != 0xd7) return (uint16_t)(w + 32);
    if (w == 0x178) return 0xff; if (w == 0x160) return 0x161; if (w == 0x152) return 0x153; if (w == 0x17d) return 0x17e;
    return w;
}
IMPL(kernel32, GetStringTypeW)
{
    uint32_t type = ARG(0), src = ARG(1), out = ARG(3); int n = (int)ARG(2);
    if (type != 1) RET(0, 4);
    const uint16_t *s = GW(src); if (n < 0) { n = 0; while (s[n]) n++; n++; }
    for (int k = 0; k < n; k++) rt_w16(G_MEM, out + 2 * k, ctype1(s[k]));
    RET(1, 4);
}
IMPL(kernel32, GetStringTypeA)
{
    uint32_t type = ARG(1), src = ARG(2), out = ARG(4); int n = (int)ARG(3);
    if (type != 1) RET(0, 5);
    const uint8_t *s = (const uint8_t *)GP(src); if (n < 0) n = (int)strlen((const char *)s) + 1;
    for (int k = 0; k < n; k++) rt_w16(G_MEM, out + 2 * k, ctype1(to_wide1252(s[k])));
    RET(1, 5);
}
static int lcmap(uint32_t flags, uint16_t *buf, int n)
{
    for (int k = 0; k < n; k++) buf[k] = (flags & 0x200) ? wupper(buf[k]) : (flags & 0x100) ? wlower(buf[k]) : buf[k];
    return n;
}
IMPL(kernel32, LCMapStringW)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4); int n = (int)ARG(3), cap = (int)ARG(5);
    const uint16_t *s = GW(src); if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (!cap) RET(n, 6);
    if (cap < n) { w32_set_last_error(c, 122); RET(0, 6); }
    uint16_t *d = (uint16_t *)GP(dst); memmove(d, s, 2 * (size_t)n); lcmap(flags, d, n);
    RET(n, 6);
}
IMPL(kernel32, LCMapStringA)
{
    uint32_t flags = ARG(1), src = ARG(2), dst = ARG(4); int n = (int)ARG(3), cap = (int)ARG(5);
    const uint8_t *s = (const uint8_t *)GP(src); if (n < 0) n = (int)strlen((const char *)s) + 1;
    if (!cap) RET(n, 6);
    if (cap < n) { w32_set_last_error(c, 122); RET(0, 6); }
    uint8_t *d = (uint8_t *)GP(dst);
    for (int k = 0; k < n; k++) { uint16_t w = to_wide1252(s[k]); lcmap(flags, &w, 1); int b = from_wide1252(w); d[k] = (uint8_t)(b < 0 ? s[k] : b); }
    RET(n, 6);
}
static const char *locale_str(uint32_t t)
{
    static const char *days[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
    static const char *sdays[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const char *months[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    static const char *smonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    t &= 0xffff;
    if (t >= 0x2a && t <= 0x30) return days[t - 0x2a];
    if (t >= 0x31 && t <= 0x37) return sdays[t - 0x31];
    if (t >= 0x38 && t <= 0x43) return months[t - 0x38];
    if (t >= 0x44 && t <= 0x4f) return smonths[t - 0x44];
    switch (t) {
    case 0x01: case 0x09: return "0409";     case 0x02: return "English (United States)"; case 0x03: return "ENU";
    case 0x04: return "English";             case 0x05: return "1";        case 0x06: return "United States";
    case 0x07: return "USA";                 case 0x0b: return "437";      case 0x0c: return ",";
    case 0x0d: return "1";                   case 0x0e: return ".";        case 0x0f: return ",";
    case 0x10: return "3;0";                 case 0x11: return "2";        case 0x12: return "1";
    case 0x14: return "$";                   case 0x15: return ".";        case 0x16: return ",";
    case 0x17: return "3;0";                 case 0x18: return "2";        case 0x19: return "2";
    case 0x1b: return "0";                   case 0x1c: return "0";        case 0x1d: return "/";
    case 0x1e: return ":";                   case 0x1f: return "M/d/yyyy"; case 0x20: return "dddd, MMMM dd, yyyy";
    case 0x21: return "0";                   case 0x22: return "1";        case 0x23: return "0";
    case 0x24: return "0";                   case 0x25: return "0";        case 0x26: return "0";
    case 0x28: return "AM";                  case 0x29: return "PM";       case 0x50: return "";
    case 0x51: return "-";                   case 0x1001: return "English";case 0x1002: return "United States";
    case 0x5c: return "en-US";               case 0x72: return "English (United States)"; case 0x6d: return "English";
    case 0x1003: return "h:mm:ss tt";        case 0x1004: return "1252";   case 0x1009: return "1";
    case 0x100c: return "0";                 case 0x1014: return "1";      case 0x5a: return "US";
    case 0x5b: return "en-US";               case 0x59: return "en";
    }
    return 0;
}
/* LOCALE_RETURN_NUMBER: the value as a DWORD (LOCALE_ILANGUAGE is hexadecimal, the others decimal) */
static int locale_number(uint32_t type, uint32_t buf, int cap, int unit)
{
    const char *s = locale_str(type); if (!s) return -1;
    uint32_t v = (uint32_t)strtoul(s, 0, (type & 0xffff) == 0x01 ? 16 : 10);
    if (cap * unit < 4) return cap ? 0 : 4 / unit;
    rt_w32(G_MEM, buf, v); return 4 / unit;
}
IMPL(kernel32, GetLocaleInfoA)
{
    uint32_t type = ARG(1), buf = ARG(2); int cap = (int)ARG(3);
    if (type & 0x20000000u) { int n = locale_number(type, buf, cap, 1); if (n <= 0) { w32_set_last_error(c, n ? 87 : 122); RET(0, 4); } RET(n, 4); }
    const char *s = locale_str(type);
    if (!s) { w32_set_last_error(c, 87); RET(0, 4); }
    int n = (int)strlen(s) + 1;
    if (!cap) RET(n, 4);
    if (cap < n) { w32_set_last_error(c, 122); RET(0, 4); }
    memcpy(GP(buf), s, (size_t)n); RET(n, 4);
}
IMPL(kernel32, GetLocaleInfoW)
{
    uint32_t type = ARG(1), buf = ARG(2); int cap = (int)ARG(3);
    if (type & 0x20000000u) { int n = locale_number(type, buf, cap, 2); if (n <= 0) { w32_set_last_error(c, n ? 87 : 122); RET(0, 4); } RET(n, 4); }
    const char *s = locale_str(type);
    if (!s) { w32_set_last_error(c, 87); RET(0, 4); }
    int n = (int)strlen(s) + 1;
    if (!cap) RET(n, 4);
    if (cap < n) { w32_set_last_error(c, 122); RET(0, 4); }
    w32_mb_to_wide(1252, (const uint8_t *)s, n, (uint16_t *)GP(buf), cap); RET(n, 4);
}
IMPL(kernel32, GetACP) { RET(1252, 0); }
IMPL(kernel32, GetOEMCP) { RET(437, 0); }
IMPL(kernel32, GetCPInfo)
{
    uint32_t info = ARG(1);
    memset(GP(info), 0, 20); rt_w32(G_MEM, info, 1); rt_w8(G_MEM, info + 4, '?');
    RET(1, 2);
}
IMPL(kernel32, IsValidCodePage) { uint32_t cp = ARG(0); RET(cp == 1252 || cp == 437 || cp == 65001 || cp == 850, 1); }
IMPL(kernel32, IsDBCSLeadByte) { RET(0, 1); }
IMPL(kernel32, IsDBCSLeadByteEx) { RET(0, 2); }
IMPL(kernel32, GetUserDefaultLCID) { RET(0x409, 0); }
IMPL(kernel32, GetUserDefaultLangID) { RET(0x409, 0); }
IMPL(kernel32, IsValidLocale) { RET(ARG(0) == 0x409 || ARG(0) == 0x400 || ARG(0) == 0x800, 2); }
IMPL(kernel32, EnumSystemLocalesA)
{
    uint32_t proc = ARG(0), s = PARAMS_ADDR + 0xf000;
    memcpy(GP(s), "00000409", 9);
    w32_callback(c, proc, 1, &s);
    RET(1, 2);
}
IMPL(kernel32, lstrcpyA) { uint32_t d = ARG(0); strcpy((char *)GP(d), GS(ARG(1))); RET(d, 2); }
