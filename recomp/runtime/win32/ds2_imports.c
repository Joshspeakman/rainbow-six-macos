/* Windows functions Dungeon Siege II imports beyond what the DS1 port needed. Wide-character (W) versions convert their
 * strings and run the ANSI implementation; input-method (IMM32), dial-up (RAS), HTTP (WinHTTP: the GameSpy-era
 * online services) and signature checks (WinTrust) report "not available", as on a PC without them. */
#include "w32.h"
#include <sys/stat.h>
#ifdef __linux__
#define st_birthtimespec st_ctim
#define st_atimespec st_atim
#define st_mtimespec st_mtim
#endif
#include <sys/time.h>
#include <time.h>

static uint32_t scratch(uint32_t n) { return heap_alloc(w32_process_heap, 8, n); }
static void unscratch(uint32_t p) { if (p) heap_free(w32_process_heap, p); }
static uint32_t ansi_of(uint32_t w)                    /* guest UTF-16 string -> new guest ANSI copy (0 for 0) */
{
    if (!w) return 0;
    int n = w32_wide_to_mb(0, (const uint16_t *)GP(w), -1, 0, 0); uint32_t a = scratch((uint32_t)n + 1);
    w32_wide_to_mb(0, (const uint16_t *)GP(w), -1, (uint8_t *)GP(a), n + 1); return a;
}
static void widen_into(uint32_t dst, const char *s, int cap)      /* ANSI -> guest UTF-16, terminated */
{
    int n = w32_mb_to_wide(0, (const uint8_t *)s, -1, (uint16_t *)GP(dst), cap);
    if (n <= 0 && cap > 0) rt_w16(G_MEM, dst, 0);
}
/* run the ANSI version with string arguments k (bitmask) converted; same argument count, so its RET pops ours */
#define VIA_ANSI(fn, mask) do { uint32_t tmp[8] = {0}; void fn(Ctx *); for (int k = 0; k < 8; k++) if ((mask) >> k & 1) { tmp[k] = ansi_of(ARG(k)); rt_w32(G_MEM, c->esp + 4 + 4 * k, tmp[k]); } \
                                fn(c); for (int k = 0; k < 8; k++) unscratch(tmp[k]); } while (0)

/* ---- kernel32 ---- */
IMPL(kernel32, HeapSize) { RET(heap_size(ARG(0), ARG(2)), 3); }
IMPL(kernel32, LocalAlloc) { RET(heap_alloc(w32_process_heap, (ARG(0) & 0x40) ? 8 : 0, ARG(1)), 2); }   /* LMEM_ZEROINIT */
IMPL(kernel32, lstrlenA) { RET(ARG(0) ? strlen(GS(ARG(0))) : 0, 1); }
IMPL(kernel32, lstrlenW) { uint32_t p = ARG(0), n = 0; if (p) while (rt_r16(G_MEM, p + 2 * n)) n++; RET(n, 1); }
IMPL(kernel32, lstrcmpA) { RET((uint32_t)strcmp(GS(ARG(0)), GS(ARG(1))), 2); }
IMPL(kernel32, lstrcmpiA) { RET((uint32_t)strcasecmp(GS(ARG(0)), GS(ARG(1))), 2); }
IMPL(kernel32, lstrcpynA)
{
    uint32_t d = ARG(0), s = ARG(1), n = ARG(2);
    if (n) { uint32_t k = 0; while (k + 1 < n && rt_r8(G_MEM, s + k)) { rt_w8(G_MEM, d + k, rt_r8(G_MEM, s + k)); k++; } rt_w8(G_MEM, d + k, 0); }
    RET(d, 3);
}
IMPL(kernel32, GetThreadLocale) { RET(0x409, 0); }
IMPL(kernel32, GetCPInfoExA)                           /* CPINFOEXA: MaxCharSize, DefaultChar[2], LeadByte[12], UnicodeDefaultChar, CodePage, CodePageName[260] */
{
    uint32_t cp = ARG(0), info = ARG(2); memset(GP(info), 0, 284);
    rt_w32(G_MEM, info, 1); rt_w8(G_MEM, info + 4, '?'); rt_w16(G_MEM, info + 18, '?'); rt_w32(G_MEM, info + 20, cp ? cp : 1252);
    snprintf((char *)GP(info + 24), 260, "%u (ANSI - Latin I)", cp ? cp : 1252); RET(1, 3);
}
IMPL(kernel32, GetSystemPowerStatus)                   /* on mains power, no battery reported */
{
    uint32_t p = ARG(0); rt_w8(G_MEM, p, 1); rt_w8(G_MEM, p + 1, 128); rt_w8(G_MEM, p + 2, 255); rt_w8(G_MEM, p + 3, 0);
    rt_w32(G_MEM, p + 4, 0xffffffffu); rt_w32(G_MEM, p + 8, 0xffffffffu); RET(1, 1);
}
IMPL(kernel32, SystemTimeToTzSpecificLocalTime)        /* (tz ignored: this Mac's time zone) */
{
    uint32_t in = ARG(1), out = ARG(2); struct tm t = {0};
    t.tm_year = rt_r16(G_MEM, in) - 1900; t.tm_mon = rt_r16(G_MEM, in + 2) - 1; t.tm_mday = rt_r16(G_MEM, in + 6);
    t.tm_hour = rt_r16(G_MEM, in + 8); t.tm_min = rt_r16(G_MEM, in + 10); t.tm_sec = rt_r16(G_MEM, in + 12);
    time_t u = timegm(&t); struct tm l; localtime_r(&u, &l);
    uint16_t v[8] = {(uint16_t)(l.tm_year + 1900), (uint16_t)(l.tm_mon + 1), (uint16_t)l.tm_wday, (uint16_t)l.tm_mday, (uint16_t)l.tm_hour, (uint16_t)l.tm_min, (uint16_t)l.tm_sec, rt_r16(G_MEM, in + 14)};
    memcpy(GP(out), v, 16); RET(1, 3);
}
IMPL(kernel32, TlsFree) { RET(1, 1); }
int w32_file_fd(uint32_t h);
static void filetime(uint32_t at, struct timespec ts)  /* Unix time -> FILETIME (100 ns since 1601) */
{
    uint64_t t = ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
    rt_w32(G_MEM, at, (uint32_t)t); rt_w32(G_MEM, at + 4, (uint32_t)(t >> 32));
}
IMPL(kernel32, GetFileInformationByHandle)             /* BY_HANDLE_FILE_INFORMATION */
{
    int fd = w32_file_fd(ARG(0)); uint32_t i = ARG(1); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { w32_set_last_error(c, 6); RET(0, 2); }
    memset(GP(i), 0, 52); rt_w32(G_MEM, i, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
    filetime(i + 4, st.st_birthtimespec); filetime(i + 12, st.st_atimespec); filetime(i + 20, st.st_mtimespec);
    rt_w32(G_MEM, i + 28, (uint32_t)st.st_dev); rt_w32(G_MEM, i + 32, (uint32_t)((uint64_t)st.st_size >> 32)); rt_w32(G_MEM, i + 36, (uint32_t)st.st_size);
    rt_w32(G_MEM, i + 40, (uint32_t)st.st_nlink); rt_w32(G_MEM, i + 44, (uint32_t)((uint64_t)st.st_ino >> 32)); rt_w32(G_MEM, i + 48, (uint32_t)st.st_ino);
    RET(1, 2);
}
IMPL(kernel32, SetFileTime)                            /* the modification time (the others aren't kept) */
{
    int fd = w32_file_fd(ARG(0)); uint32_t m = ARG(3);
    if (fd < 0) { w32_set_last_error(c, 6); RET(0, 4); }
    if (m) {
        uint64_t t = (uint64_t)rt_r32(G_MEM, m) | (uint64_t)rt_r32(G_MEM, m + 4) << 32;
        struct timespec ts[2] = {{0, UTIME_OMIT}, {(time_t)(t / 10000000ull - 11644473600ull), (long)(t % 10000000ull) * 100}};
        futimens(fd, ts);
    }
    RET(1, 4);
}
IMPL(kernel32, CreateFileW) { VIA_ANSI(imp_kernel32_CreateFileA, 1); }
IMPL(kernel32, CreateDirectoryW) { VIA_ANSI(imp_kernel32_CreateDirectoryA, 1); }
IMPL(kernel32, DeleteFileW) { VIA_ANSI(imp_kernel32_DeleteFileA, 1); }
IMPL(kernel32, RemoveDirectoryW) { VIA_ANSI(imp_kernel32_RemoveDirectoryA, 1); }
IMPL(kernel32, MoveFileW) { VIA_ANSI(imp_kernel32_MoveFileA, 3); }
static void find_data_to_wide(uint32_t a, uint32_t w)  /* WIN32_FIND_DATAA -> WIN32_FIND_DATAW */
{
    memcpy(GP(w), GP(a), 44); widen_into(w + 44, GS(a + 44), 260); widen_into(w + 44 + 520, GS(a + 44 + 260), 14);
}
IMPL(kernel32, FindFirstFileW)
{
    uint32_t pat = ansi_of(ARG(0)), wd = ARG(1), ad = scratch(320); void imp_kernel32_FindFirstFileA(Ctx *);
    rt_w32(G_MEM, c->esp + 4, pat); rt_w32(G_MEM, c->esp + 8, ad);
    imp_kernel32_FindFirstFileA(c);
    if (c->eax != 0xffffffffu) find_data_to_wide(ad, wd);
    unscratch(pat); unscratch(ad);
}
IMPL(kernel32, FindNextFileW)
{
    uint32_t wd = ARG(1), ad = scratch(320); void imp_kernel32_FindNextFileA(Ctx *);
    rt_w32(G_MEM, c->esp + 8, ad);
    imp_kernel32_FindNextFileA(c);
    if (c->eax) find_data_to_wide(ad, wd);
    unscratch(ad);
}
IMPL(kernel32, OutputDebugStringW)
{
    if (getenv("W32_DEBUGSTRINGS") && ARG(0)) { uint32_t a = ansi_of(ARG(0)); fprintf(stderr, "[game] %s", GS(a)); unscratch(a); }
    RET(0, 1);
}

IMPL(kernel32, FatalAppExitA) { fprintf(stderr, "w32: FatalAppExit: %s\n", ARG(1) ? GS(ARG(1)) : ""); w32_exit(c, 1); }
IMPL(kernel32, GetConsoleScreenBufferInfo) { w32_set_last_error(c, 6); RET(0, 2); }
IMPL(kernel32, SetConsoleCtrlHandler) { RET(1, 2); }
IMPL(kernel32, SetConsoleTextAttribute) { RET(1, 2); }
IMPL(kernel32, PeekNamedPipe) { w32_set_last_error(c, 6); RET(0, 6); }

/* ---- user32 ---- */
IMPL(user32, ChangeDisplaySettingsA) { RET(0, 2); }    /* DISP_CHANGE_SUCCESSFUL: the host picks the display mode */
IMPL(user32, CharNextA) { uint32_t p = ARG(0); RET(rt_r8(G_MEM, p) ? p + 1 : p, 1); }
IMPL(user32, CharNextExA) { uint32_t p = ARG(1); RET(rt_r8(G_MEM, p) ? p + 1 : p, 3); }
IMPL(user32, GetClassLongA) { RET(0, 2); }
IMPL(user32, RegisterClassExA)                         /* WNDCLASSEXA = cbSize, then WNDCLASSA's fields, then hIconSm */
{
    void imp_user32_RegisterClassA(Ctx *); rt_w32(G_MEM, c->esp + 4, ARG(0) + 4); imp_user32_RegisterClassA(c);
}

/* ---- gdi32: not reached yet (logged, so the first use shows which is needed) ---- */
#define TODO_BODY(what, n, v) { static int once; if (!once++) fprintf(stderr, "w32: %s is not implemented yet\n", what); RET(v, n); }
IMPL(gdi32, BitBlt) TODO_BODY("gdi32!BitBlt", 9, 0)
IMPL(gdi32, CreateDIBSection) TODO_BODY("gdi32!CreateDIBSection", 6, 0)
IMPL(gdi32, GetCharABCWidthsW) TODO_BODY("gdi32!GetCharABCWidthsW", 4, 0)
IMPL(gdi32, GetCharWidth32A) TODO_BODY("gdi32!GetCharWidth32A", 4, 0)
IMPL(gdi32, SetPixel) TODO_BODY("gdi32!SetPixel", 4, 0)
IMPL(gdi32, SetStretchBltMode) TODO_BODY("gdi32!SetStretchBltMode", 2, 0)
IMPL(gdi32, StretchBlt) TODO_BODY("gdi32!StretchBlt", 11, 0)
IMPL(gdi32, GetGlyphOutlineW) { static int once; if (!once++) fprintf(stderr, "w32: gdi32!GetGlyphOutlineW is not implemented yet\n"); RET(0xffffffffu, 7); }   /* GDI_ERROR */

/* ---- ole32 / oleaut32 ---- */
IMPL(ole32, CoInitialize) { RET(0, 1); }
IMPL(ole32, CoTaskMemAlloc) { RET(heap_alloc(w32_process_heap, 0, ARG(0)), 1); }
IMPL(ole32, CoTaskMemFree) { if (ARG(0)) heap_free(w32_process_heap, ARG(0)); RET(0, 1); }
IMPL(ole32, CoTaskMemRealloc) { RET(ARG(0) ? heap_realloc(w32_process_heap, 0, ARG(0), ARG(1)) : heap_alloc(w32_process_heap, 0, ARG(1)), 2); }
IMPL(oleaut32, ord2)                                   /* SysAllocString: length prefix, then the characters and a terminator */
{
    uint32_t s = ARG(0), n = 0; if (!s) RET(0, 1);
    while (rt_r16(G_MEM, s + 2 * n)) n++;
    uint32_t b = heap_alloc(w32_process_heap, 0, 4 + 2 * n + 2); if (!b) RET(0, 1);
    rt_w32(G_MEM, b, 2 * n); memcpy(GP(b + 4), GP(s), 2 * n + 2); RET(b + 4, 1);
}
IMPL(oleaut32, ord6) { if (ARG(0)) heap_free(w32_process_heap, ARG(0) - 4); RET(0, 1); }   /* SysFreeString */
IMPL(oleaut32, ord277)                                 /* VarUI4FromStr(str, lcid, flags, *out) */
{
    uint32_t a = ansi_of(ARG(0)); char *e; unsigned long v = strtoul(GS(a), &e, 10); int ok = e != GS(a);
    unscratch(a); if (ok) rt_w32(G_MEM, ARG(3), (uint32_t)v); RET(ok ? 0 : 0x80020005u, 4);   /* DISP_E_TYPEMISMATCH */
}

/* ---- winmm ---- */
IMPL(winmm, timeGetDevCaps) { rt_w32(G_MEM, ARG(0), 1); rt_w32(G_MEM, ARG(0) + 4, 1000000); RET(0, 2); }
IMPL(winmm, waveOutPause) { RET(0, 1); }
IMPL(winmm, waveOutRestart) { RET(0, 1); }
IMPL(winmm, waveOutSetVolume) { RET(0, 2); }

/* ---- imm32: no input method (as a Western-locale PC) ---- */
IMPL(imm32, ImmAssociateContext) { RET(0, 2); }
IMPL(imm32, ImmGetCandidateListA) { RET(0, 4); }
IMPL(imm32, ImmGetCandidateListW) { RET(0, 4); }
IMPL(imm32, ImmGetCompositionStringA) { RET(0, 4); }
IMPL(imm32, ImmGetCompositionStringW) { RET(0, 4); }
IMPL(imm32, ImmGetContext) { RET(0, 1); }
IMPL(imm32, ImmGetConversionStatus) { RET(0, 3); }
IMPL(imm32, ImmGetDefaultIMEWnd) { RET(0, 1); }
IMPL(imm32, ImmGetIMEFileNameA) { RET(0, 3); }
IMPL(imm32, ImmGetOpenStatus) { RET(0, 1); }
IMPL(imm32, ImmGetVirtualKey) { RET(0, 1); }
IMPL(imm32, ImmIsIME) { RET(0, 1); }
IMPL(imm32, ImmNotifyIME) { RET(0, 4); }
IMPL(imm32, ImmReleaseContext) { RET(1, 2); }
IMPL(imm32, ImmSetConversionStatus) { RET(0, 3); }
IMPL(imm32, ImmSetOpenStatus) { RET(0, 2); }
IMPL(imm32, ImmSimulateHotKey) { RET(0, 2); }

/* ---- rasapi32: no dial-up connections ---- */
IMPL(rasapi32, RasEnumConnectionsA) { if (ARG(2)) rt_w32(G_MEM, ARG(2), 0); RET(0, 3); }
IMPL(rasapi32, RasGetConnectStatusA) { RET(6, 2); }
IMPL(rasapi32, RasGetProjectionInfoA) { RET(6, 4); }

/* ---- shell32 ---- */
IMPL(shell32, SHGetFolderPathW)
{
    uint32_t out = ARG(4), tmp = scratch(520); void imp_shfolder_SHGetFolderPathA(Ctx *);
    rt_w32(G_MEM, c->esp + 4 + 16, tmp); imp_shfolder_SHGetFolderPathA(c);
    if (!c->eax) widen_into(out, GS(tmp), 260);
    unscratch(tmp);
}

/* ---- wintrust, winhttp: not available ---- */
IMPL(wintrust, WinVerifyTrust) { if (getenv("W32_TRUSTLOG")) fprintf(stderr, "w32: WinVerifyTrust (unsigned)\n"); RET(0x800B0100u, 3); }   /* TRUST_E_NOSIGNATURE */
#define HTTP_OFF_BODY(n) { w32_set_last_error(c, 12029); RET(0, n); }   /* ERROR_WINHTTP_CANNOT_CONNECT */
IMPL(winhttp, WinHttpOpen) HTTP_OFF_BODY(5)
IMPL(winhttp, WinHttpConnect) HTTP_OFF_BODY(4)
IMPL(winhttp, WinHttpOpenRequest) HTTP_OFF_BODY(7)
IMPL(winhttp, WinHttpSendRequest) HTTP_OFF_BODY(7)
IMPL(winhttp, WinHttpReceiveResponse) HTTP_OFF_BODY(2)
IMPL(winhttp, WinHttpQueryHeaders) HTTP_OFF_BODY(6)
IMPL(winhttp, WinHttpQueryDataAvailable) HTTP_OFF_BODY(2)
IMPL(winhttp, WinHttpReadData) HTTP_OFF_BODY(4)
IMPL(winhttp, WinHttpCloseHandle) { RET(1, 1); }
IMPL(winhttp, WinHttpSetStatusCallback) { RET(0, 4); }

/* ---- Direct3D 9 and D3DX: the next step (docs/PLAN.md); for now the game is told Direct3D 9 isn't there ---- */
/* (E_FAIL until written) */
IMPL(d3dx9_29, D3DXCompileShader) TODO_BODY("d3dx9!D3DXCompileShader", 10, 0x80004005u)
IMPL(d3dx9_29, D3DXFillCubeTextureTX) TODO_BODY("d3dx9!D3DXFillCubeTextureTX", 2, 0x80004005u)
IMPL(d3dx9_29, D3DXFillTextureTX) TODO_BODY("d3dx9!D3DXFillTextureTX", 2, 0x80004005u)
IMPL(d3dx9_29, D3DXFillVolumeTextureTX) TODO_BODY("d3dx9!D3DXFillVolumeTextureTX", 2, 0x80004005u)
IMPL(shfolder, SHGetFolderPathW) { void imp_shell32_SHGetFolderPathW(Ctx *); imp_shell32_SHGetFolderPathW(c); }   /* also looked up there */
