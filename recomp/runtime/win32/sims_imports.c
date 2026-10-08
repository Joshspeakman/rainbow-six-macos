/* Windows functions The Sims Legacy Collection (MSVC 2022, Steam) imports beyond what Fallout NV needed: the Vista-era
 * synchronisation its C++ runtime is built on (SRW locks, condition variables, one-time initialisation, SLists), more
 * wide-character (W) versions (strings converted, the ANSI implementation run), and small user32/gdi32 helpers. */
#include "w32.h"
#include <time.h>

static uint32_t scratch(uint32_t n) { return heap_alloc(w32_process_heap, 8, n); }
static void unscratch(uint32_t p) { if (p) heap_free(w32_process_heap, p); }
static uint32_t ansi_of(uint32_t w)                    /* guest UTF-16 string -> new guest ANSI copy (0 for 0) */
{
    if (!w) return 0;
    int n = w32_wide_to_mb(0, (const uint16_t *)GP(w), -1, 0, 0); uint32_t a = scratch((uint32_t)n + 1);
    w32_wide_to_mb(0, (const uint16_t *)GP(w), -1, (uint8_t *)GP(a), n + 1); return a;
}
#define VIA_ANSI(fn, mask) do { uint32_t tmp[8] = {0}; void fn(Ctx *); for (int k = 0; k < 8; k++) if ((mask) >> k & 1) { tmp[k] = ansi_of(ARG(k)); rt_w32(G_MEM, c->esp + 4 + 4 * k, tmp[k]); } \
                                fn(c); for (int k = 0; k < 8; k++) unscratch(tmp[k]); } while (0)
/* the ANSI version writes a string of at most `cap` characters into argument `out` and returns its length: run it on a
 * scratch buffer and give the caller UTF-16; the result follows the W convention (needed size with the terminator
 * when too small) */
static void via_ansi_out(Ctx *c, void (*fn)(Ctx *), int out, int cap, uint32_t in_mask)
{
    uint32_t wbuf = ARG(out), wcap = ARG(cap), abuf = scratch(1040), tmp[8] = {0};
    for (int k = 0; k < 8; k++) if (in_mask >> k & 1) { tmp[k] = ansi_of(ARG(k)); rt_w32(G_MEM, c->esp + 4 + 4 * k, tmp[k]); }
    rt_w32(G_MEM, c->esp + 4 + 4 * out, abuf); rt_w32(G_MEM, c->esp + 4 + 4 * cap, 1024);
    uint32_t esp = c->esp; fn(c); uint32_t r = c->eax;
    if (r && r < 1024) {
        int n = w32_mb_to_wide(0, (const uint8_t *)GP(abuf), -1, 0, 0);       /* with the terminator */
        if ((uint32_t)n <= wcap && wbuf) { w32_mb_to_wide(0, (const uint8_t *)GP(abuf), -1, (uint16_t *)GP(wbuf), (int)wcap); c->eax = (uint32_t)n - 1; }
        else c->eax = (uint32_t)n;
    }
    for (int k = 0; k < 8; k++) unscratch(tmp[k]);
    unscratch(abuf); (void)esp;
}

/* ---- kernel32: SRW locks and condition variables. One host mutex guards every lock word (a word in guest memory:
 * bit 0 exclusive, bits 1.. readers); waiters sleep on one host condition variable. ---- */
static pthread_mutex_t sync_m = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t sync_cv = PTHREAD_COND_INITIALIZER;
static uint32_t *word(uint32_t a) { return (uint32_t *)GP(a); }
static void srw_lock(uint32_t a, int shared)                         /* sync_m held */
{
    if (shared) { while (*word(a) & 1) pthread_cond_wait(&sync_cv, &sync_m); *word(a) += 2; }
    else { while (*word(a)) pthread_cond_wait(&sync_cv, &sync_m); *word(a) = 1; }
}
static void srw_unlock(uint32_t a, int shared)                       /* sync_m held */
{
    if (shared) { *word(a) -= 2; if (!*word(a)) pthread_cond_broadcast(&sync_cv); }
    else { *word(a) = 0; pthread_cond_broadcast(&sync_cv); }
}
IMPL(kernel32, InitializeSRWLock) { *word(ARG(0)) = 0; RET(0, 1); }
IMPL(kernel32, AcquireSRWLockExclusive) { pthread_mutex_lock(&sync_m); srw_lock(ARG(0), 0); pthread_mutex_unlock(&sync_m); RET(0, 1); }
IMPL(kernel32, AcquireSRWLockShared) { pthread_mutex_lock(&sync_m); srw_lock(ARG(0), 1); pthread_mutex_unlock(&sync_m); RET(0, 1); }
IMPL(kernel32, ReleaseSRWLockExclusive) { pthread_mutex_lock(&sync_m); srw_unlock(ARG(0), 0); pthread_mutex_unlock(&sync_m); RET(0, 1); }
IMPL(kernel32, ReleaseSRWLockShared) { pthread_mutex_lock(&sync_m); srw_unlock(ARG(0), 1); pthread_mutex_unlock(&sync_m); RET(0, 1); }
IMPL(kernel32, TryAcquireSRWLockExclusive)
{
    pthread_mutex_lock(&sync_m); int ok = !*word(ARG(0)); if (ok) *word(ARG(0)) = 1; pthread_mutex_unlock(&sync_m); RET(ok, 1);
}
IMPL(kernel32, TryAcquireSRWLockShared)
{
    pthread_mutex_lock(&sync_m); int ok = !(*word(ARG(0)) & 1); if (ok) *word(ARG(0)) += 2; pthread_mutex_unlock(&sync_m); RET(ok, 1);
}
/* a condition variable's word counts wakes; a sleeper waits until it changes */
static int cv_wait(uint32_t cv, uint32_t ms, void (*release)(uint32_t), void (*reacquire)(uint32_t), uint32_t lock)
{
    pthread_mutex_lock(&sync_m);
    uint32_t seq = *word(cv); int ok = 1;
    release(lock);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    if (ms != 0xffffffffu) { until.tv_sec += ms / 1000; until.tv_nsec += (long)(ms % 1000) * 1000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; } }
    while (*word(cv) == seq) {
        if (ms == 0xffffffffu) pthread_cond_wait(&sync_cv, &sync_m);
        else if (pthread_cond_timedwait(&sync_cv, &sync_m, &until)) { ok = 0; break; }
    }
    reacquire(lock);
    pthread_mutex_unlock(&sync_m);
    return ok;
}
static void rel_x(uint32_t a) { srw_unlock(a, 0); } static void acq_x(uint32_t a) { srw_lock(a, 0); }
static void rel_s(uint32_t a) { srw_unlock(a, 1); } static void acq_s(uint32_t a) { srw_lock(a, 1); }
IMPL(kernel32, InitializeConditionVariable) { *word(ARG(0)) = 0; RET(0, 1); }
IMPL(kernel32, SleepConditionVariableSRW)                            /* (cv, lock, ms, flags: 1 = shared) */
{
    int shared = ARG(3) & 1;
    if (!cv_wait(ARG(0), ARG(2), shared ? rel_s : rel_x, shared ? acq_s : acq_x, ARG(1))) { w32_set_last_error(c, 1460); RET(0, 4); }
    RET(1, 4);
}
IMPL(kernel32, WakeConditionVariable) { pthread_mutex_lock(&sync_m); (*word(ARG(0)))++; pthread_cond_broadcast(&sync_cv); pthread_mutex_unlock(&sync_m); RET(0, 1); }
IMPL(kernel32, WakeAllConditionVariable) { pthread_mutex_lock(&sync_m); (*word(ARG(0)))++; pthread_cond_broadcast(&sync_cv); pthread_mutex_unlock(&sync_m); RET(0, 1); }

/* InitOnceExecuteOnce(once, fn, param, context): fn(once, param, context) the first time; one at a time */
IMPL(kernel32, InitOnceExecuteOnce)
{
    static pthread_mutex_t once_m; static int once_init;
    if (!__atomic_load_n(&once_init, __ATOMIC_ACQUIRE)) {
        pthread_mutex_lock(&sync_m);
        if (!once_init) { pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(&once_m, &at); __atomic_store_n(&once_init, 1, __ATOMIC_RELEASE); }
        pthread_mutex_unlock(&sync_m);
    }
    uint32_t once = ARG(0), fn = ARG(1), param = ARG(2), ctx = ARG(3), ok = 1;
    if (rt_r32(G_MEM, once) != 2) {
        pthread_mutex_lock(&once_m);
        if (rt_r32(G_MEM, once) != 2) {
            uint32_t a[3] = {once, param, ctx};
            ok = w32_callback(c, fn, 3, a) ? 1 : 0;
            if (ok) rt_w32(G_MEM, once, 2);
        }
        pthread_mutex_unlock(&once_m);
    }
    RET(ok, 4);
}

/* SLists: { Next, Depth/Sequence } headers, pushed and flushed under the sync mutex */
IMPL(kernel32, InitializeSListHead) { rt_w32(G_MEM, ARG(0), 0); rt_w32(G_MEM, ARG(0) + 4, 0); RET(0, 1); }
IMPL(kernel32, InterlockedPushEntrySList)
{
    uint32_t h = ARG(0), e = ARG(1);
    pthread_mutex_lock(&sync_m);
    uint32_t first = rt_r32(G_MEM, h); rt_w32(G_MEM, e, first); rt_w32(G_MEM, h, e);
    rt_w32(G_MEM, h + 4, rt_r32(G_MEM, h + 4) + 0x10001u);               /* depth + 1, sequence + 1 */
    pthread_mutex_unlock(&sync_m);
    RET(first, 2);
}
IMPL(kernel32, InterlockedPopEntrySList)
{
    uint32_t h = ARG(0);
    pthread_mutex_lock(&sync_m);
    uint32_t first = rt_r32(G_MEM, h);
    if (first) { rt_w32(G_MEM, h, rt_r32(G_MEM, first)); rt_w32(G_MEM, h + 4, rt_r32(G_MEM, h + 4) + 0x10000u - 1); }
    pthread_mutex_unlock(&sync_m);
    RET(first, 1);
}
IMPL(kernel32, InterlockedFlushSList)
{
    uint32_t h = ARG(0);
    pthread_mutex_lock(&sync_m); uint32_t first = rt_r32(G_MEM, h); rt_w32(G_MEM, h, 0); rt_w16(G_MEM, h + 4, 0); pthread_mutex_unlock(&sync_m);
    RET(first, 1);
}

/* ---- kernel32: odds and ends ---- */
IMPL(kernel32, InitializeCriticalSectionEx) { void imp_kernel32_InitializeCriticalSectionAndSpinCount(Ctx *); imp_kernel32_InitializeCriticalSectionAndSpinCount(c); c->esp += 4; }   /* (cs, spin, flags) */
IMPL(kernel32, EncodePointer) { RET(ARG(0), 1); }
IMPL(kernel32, DecodePointer) { RET(ARG(0), 1); }
IMPL(kernel32, FlushProcessWriteBuffers) { __atomic_thread_fence(__ATOMIC_SEQ_CST); RET(0, 0); }
IMPL(kernel32, GetCurrentProcessorNumber) { RET(0, 0); }
IMPL(kernel32, GetTickCount64)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); uint64_t ms = (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
    c->edx = (uint32_t)(ms >> 32); RET((uint32_t)ms, 0);
}
IMPL(kernel32, GetProcessHeaps) { if (ARG(0) && ARG(1)) rt_w32(G_MEM, ARG(1), w32_process_heap); RET(1, 2); }
IMPL(kernel32, HeapQueryInformation) { w32_set_last_error(c, 50); RET(0, 5); }
IMPL(kernel32, GetProcessAffinityMask)
{
    if (ARG(1)) rt_w32(G_MEM, ARG(1), 0xff); if (ARG(2)) rt_w32(G_MEM, ARG(2), 0xff); RET(1, 3);   /* 8 processors */
}
IMPL(kernel32, AreFileApisANSI) { RET(1, 0); }
IMPL(kernel32, GetStartupInfoW) { void imp_kernel32_GetStartupInfoA(Ctx *); imp_kernel32_GetStartupInfoA(c); }   /* no strings set */
IMPL(kernel32, SetProcessWorkingSetSize) { RET(1, 3); }
IMPL(kernel32, GetProcessWorkingSetSize) { if (ARG(1)) rt_w32(G_MEM, ARG(1), 0x100000); if (ARG(2)) rt_w32(G_MEM, ARG(2), 0x40000000); RET(1, 3); }

/* ---- kernel32: wide-character versions ---- */
IMPL(kernel32, LoadLibraryW) { VIA_ANSI(imp_kernel32_LoadLibraryA, 1); }
IMPL(kernel32, LoadLibraryExW) { VIA_ANSI(imp_kernel32_LoadLibraryExA, 1); }
IMPL(kernel32, GetFileAttributesW) { VIA_ANSI(imp_kernel32_GetFileAttributesA, 1); }
IMPL(kernel32, SetFileAttributesW) { VIA_ANSI(imp_kernel32_SetFileAttributesA, 1); }
IMPL(kernel32, CopyFileW) { VIA_ANSI(imp_kernel32_CopyFileA, 3); }
IMPL(kernel32, GetDriveTypeW) { VIA_ANSI(imp_kernel32_GetDriveTypeA, 1); }
IMPL(kernel32, GetDiskFreeSpaceExW) { VIA_ANSI(imp_kernel32_GetDiskFreeSpaceExA, 1); }
IMPL(kernel32, SetEnvironmentVariableW) { VIA_ANSI(imp_kernel32_SetEnvironmentVariableA, 3); }
IMPL(kernel32, CreateEventW) { VIA_ANSI(imp_kernel32_CreateEventA, 8); }
IMPL(kernel32, CreateEventExW)                                       /* (attrs, name, flags: 1 manual, 2 initially set, access) */
{
    uint32_t name = ARG(1), f = ARG(2);                              /* as (attrs, manual, initial, name) */
    rt_w32(G_MEM, c->esp + 8, f & 1); rt_w32(G_MEM, c->esp + 12, f >> 1 & 1); rt_w32(G_MEM, c->esp + 16, name);
    VIA_ANSI(imp_kernel32_CreateEventA, 8);
}
IMPL(kernel32, CreateSemaphoreExW)                                   /* (attrs, initial, max, name, flags, access) */
{
    void imp_kernel32_CreateSemaphoreA(Ctx *); uint32_t ret = rt_r32(G_MEM, c->esp), esp = c->esp;
    uint32_t name = ansi_of(ARG(3)); rt_w32(G_MEM, c->esp + 16, name);
    imp_kernel32_CreateSemaphoreA(c); unscratch(name);
    c->esp = esp + 4 + 24; (void)ret;
}
IMPL(kernel32, GetModuleFileNameW) { void imp_kernel32_GetModuleFileNameA(Ctx *); via_ansi_out(c, imp_kernel32_GetModuleFileNameA, 1, 2, 0); }
IMPL(kernel32, GetCurrentDirectoryW) { void imp_kernel32_GetCurrentDirectoryA(Ctx *); via_ansi_out(c, imp_kernel32_GetCurrentDirectoryA, 1, 0, 0); }
IMPL(kernel32, GetTempPathW) { void imp_kernel32_GetTempPathA(Ctx *); via_ansi_out(c, imp_kernel32_GetTempPathA, 1, 0, 0); }
IMPL(kernel32, GetFullPathNameW)                                     /* (name, cap, buf, filepart) */
{
    void imp_kernel32_GetFullPathNameA(Ctx *); uint32_t fp = ARG(3); rt_w32(G_MEM, c->esp + 16, 0);
    via_ansi_out(c, imp_kernel32_GetFullPathNameA, 2, 1, 1);
    if (fp && c->eax && ARG(2)) {                                    /* the file part: after the last separator */
        uint32_t b = ARG(2), last = b; for (uint32_t p = b; rt_r16(G_MEM, p); p += 2) if (rt_r16(G_MEM, p) == '\\' || rt_r16(G_MEM, p) == '/') last = p + 2;
        rt_w32(G_MEM, fp, rt_r16(G_MEM, last) ? last : 0);
    }
}

/* ---- ole32, dwmapi, user32 ---- */
IMPL(ole32, CoInitializeSecurity) { RET(0, 9); }
IMPL(ole32, CoSetProxyBlanket) { RET(0, 8); }
IMPL(dwmapi, DwmSetWindowAttribute) { RET(0, 4); }                    /* dark title bar, rounded corners: nothing to do */
IMPL(user32, SetProcessDPIAware) { RET(1, 0); }
IMPL(kernel32, LockFileEx) { RET(1, 6); }                              /* one game at a time is all there is */
IMPL(kernel32, UnlockFileEx) { RET(1, 5); }

/* ---- user32: rectangles { left, top, right, bottom } ---- */
static int32_t R(uint32_t r, int k) { return (int32_t)rt_r32(G_MEM, r + 4u * (uint32_t)k); }
static void setr(uint32_t r, int32_t l, int32_t t, int32_t rr, int32_t b) { rt_w32(G_MEM, r, (uint32_t)l); rt_w32(G_MEM, r + 4, (uint32_t)t); rt_w32(G_MEM, r + 8, (uint32_t)rr); rt_w32(G_MEM, r + 12, (uint32_t)b); }
static int empty(uint32_t r) { return R(r, 2) <= R(r, 0) || R(r, 3) <= R(r, 1); }
IMPL(user32, SetRect) { setr(ARG(0), (int32_t)ARG(1), (int32_t)ARG(2), (int32_t)ARG(3), (int32_t)ARG(4)); RET(1, 5); }
IMPL(user32, IsRectEmpty) { RET(empty(ARG(0)), 1); }
IMPL(user32, OffsetRect) { uint32_t r = ARG(0); int32_t dx = (int32_t)ARG(1), dy = (int32_t)ARG(2); setr(r, R(r, 0) + dx, R(r, 1) + dy, R(r, 2) + dx, R(r, 3) + dy); RET(1, 3); }
IMPL(user32, InflateRect) { uint32_t r = ARG(0); int32_t dx = (int32_t)ARG(1), dy = (int32_t)ARG(2); setr(r, R(r, 0) - dx, R(r, 1) - dy, R(r, 2) + dx, R(r, 3) + dy); RET(1, 3); }
IMPL(user32, PtInRect) { uint32_t r = ARG(0); int32_t x = (int32_t)ARG(1), y = (int32_t)ARG(2); RET(x >= R(r, 0) && x < R(r, 2) && y >= R(r, 1) && y < R(r, 3), 3); }
IMPL(user32, IntersectRect)
{
    uint32_t d = ARG(0), a = ARG(1), b = ARG(2);
    int32_t l = R(a, 0) > R(b, 0) ? R(a, 0) : R(b, 0), t = R(a, 1) > R(b, 1) ? R(a, 1) : R(b, 1);
    int32_t r = R(a, 2) < R(b, 2) ? R(a, 2) : R(b, 2), bt = R(a, 3) < R(b, 3) ? R(a, 3) : R(b, 3);
    if (r <= l || bt <= t) { setr(d, 0, 0, 0, 0); RET(0, 3); }
    setr(d, l, t, r, bt); RET(1, 3);
}
IMPL(user32, UnionRect)
{
    uint32_t d = ARG(0), a = ARG(1), b = ARG(2);
    if (empty(a) && empty(b)) { setr(d, 0, 0, 0, 0); RET(0, 3); }
    if (empty(a)) { setr(d, R(b, 0), R(b, 1), R(b, 2), R(b, 3)); RET(1, 3); }
    if (empty(b)) { setr(d, R(a, 0), R(a, 1), R(a, 2), R(a, 3)); RET(1, 3); }
    setr(d, R(a, 0) < R(b, 0) ? R(a, 0) : R(b, 0), R(a, 1) < R(b, 1) ? R(a, 1) : R(b, 1),
         R(a, 2) > R(b, 2) ? R(a, 2) : R(b, 2), R(a, 3) > R(b, 3) ? R(a, 3) : R(b, 3)); RET(1, 3);
}

/* ---- advapi32 ---- */
IMPL(advapi32, SystemFunction036)                                     /* RtlGenRandom(buffer, length) */
{
    uint8_t *p = GP(ARG(0)); uint32_t n = ARG(1); FILE *f = fopen("/dev/urandom", "rb");
    size_t got = f ? fread(p, 1, n, f) : 0; if (f) fclose(f);
    for (size_t k = got; k < n; k++) p[k] = (uint8_t)rand();
    RET(1, 2);
}
IMPL(advapi32, GetUserNameW) { void imp_advapi32_GetUserNameA(Ctx *); uint32_t capp = ARG(1), cap = capp ? rt_r32(G_MEM, capp) : 0, buf = ARG(0), a = scratch(260), n = 260, esp = c->esp;
    rt_w32(G_MEM, esp + 4, a); uint32_t np = scratch(4); rt_w32(G_MEM, np, n); rt_w32(G_MEM, esp + 8, np);
    imp_advapi32_GetUserNameA(c); uint32_t ok = c->eax;
    if (ok) { int w = w32_mb_to_wide(0, (const uint8_t *)GP(a), -1, 0, 0); if ((uint32_t)w > cap) { rt_w32(G_MEM, capp, (uint32_t)w); w32_set_last_error(c, 122); c->eax = 0; }
              else { w32_mb_to_wide(0, (const uint8_t *)GP(a), -1, (uint16_t *)GP(buf), (int)cap); rt_w32(G_MEM, capp, (uint32_t)w); } }
    unscratch(a); unscratch(np); }
/* crashpad (linked into the game) talks to its handler process over named pipes; there is no handler here (the port
 * writes its own crash reports), so making the pipe fails and crashpad stays off */
IMPL(kernel32, CreateNamedPipeW) { w32_set_last_error(c, 5); RET(0xffffffffu, 8); }
IMPL(kernel32, CreateProcessW) { w32_set_last_error(c, 2); RET(0, 10); }
IMPL(kernel32, FormatMessageW) { w32_set_last_error(c, 317); RET(0, 7); }     /* no system message texts */

/* ---- wininet: the game posts a "boot_start" telemetry event; offline, every call fails (ERROR_INTERNET_CANNOT_CONNECT) ---- */
#define NO_NET(n) { w32_set_last_error(c, 12029); RET(0, n); }
IMPL(wininet, InternetOpenA) NO_NET(5)
IMPL(wininet, InternetConnectA) NO_NET(8)
IMPL(wininet, HttpOpenRequestA) NO_NET(8)
IMPL(wininet, HttpAddRequestHeadersA) NO_NET(4)
IMPL(wininet, HttpSendRequestExA) NO_NET(5)
IMPL(wininet, HttpEndRequestA) NO_NET(4)
IMPL(wininet, HttpQueryInfoA) NO_NET(5)
IMPL(wininet, InternetReadFile) NO_NET(4)
IMPL(wininet, InternetWriteFile) NO_NET(4)
IMPL(wininet, InternetCloseHandle) { RET(1, 1); }

/* ---- shell32 ---- */
/* SHGetKnownFolderPath(folder id, flags, token, PWSTR *out): Vista's folders (from the Diablo II port), the string for
 * CoTaskMemFree (the process heap here); the folder is made if missing, parents included */
#include <sys/stat.h>
IMPL(shell32, SHGetKnownFolderPath)
{
    static const struct { uint32_t d1; const char *path; } known[] = {
        {0x4c5c32ffu, "C:\\Users\\player\\Saved Games"},           /* FOLDERID_SavedGames */
        {0xfdd39ad0u, "C:\\Users\\player\\Documents"},             /* FOLDERID_Documents */
        {0x5e6c858fu, "C:\\Users\\player"},                        /* FOLDERID_Profile */
        {0xf1b32785u, "C:\\Users\\player\\AppData\\Local"},        /* FOLDERID_LocalAppData */
        {0x3eb685dbu, "C:\\Users\\player\\AppData\\Roaming"},      /* FOLDERID_RoamingAppData */
        {0x62ab5d82u, "C:\\ProgramData"},                          /* FOLDERID_ProgramData */
    };
    uint32_t id = ARG(0) ? rt_r32(G_MEM, ARG(0)) : 0, out = ARG(3);
    if (out) rt_w32(G_MEM, out, 0);
    for (unsigned k = 0; k < sizeof known / sizeof *known; k++) if (known[k].d1 == id) {
        char host[2048];
        if (!w32_host_path(known[k].path, host, sizeof host, 1))
            for (char *p = host + 1; ; p++) if (*p == '/' || !*p) { char t = *p; *p = 0; mkdir(host, 0755); *p = t; if (!t) break; }
        uint32_t n = (uint32_t)strlen(known[k].path), p = heap_alloc(w32_process_heap, 0, 2 * (n + 1));
        for (uint32_t j = 0; j <= n; j++) rt_w16(G_MEM, p + 2 * j, (uint8_t)known[k].path[j]);
        if (out) rt_w32(G_MEM, out, p);
        RET(0, 4);
    }
    fprintf(stderr, "w32: SHGetKnownFolderPath(%08x-...) unknown\n", id);
    RET(0x80070002u, 4);                               /* HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) */
}
IMPL(kernel32, GetFileAttributesExW) { VIA_ANSI(imp_kernel32_GetFileAttributesExA, 1); }
/* FindFirstFileExW(name, info level, WIN32_FIND_DATAW *, search op, filter, flags): the plain search (the basic info
 * level only leaves out the short name) */
IMPL(kernel32, FindFirstFileExW)
{
    void imp_kernel32_FindFirstFileW(Ctx *); uint32_t esp = c->esp;
    rt_w32(G_MEM, esp + 8, ARG(2)); imp_kernel32_FindFirstFileW(c);        /* (name, data): pops 2 of our 6 */
    c->esp = esp + 4 + 24;
}
/* GetFileInformationByHandleEx(handle, class, buffer, size): the classes std::filesystem asks for */
#include <sys/stat.h>
int w32_file_fd(uint32_t h);
static void ft64(uint32_t at, time_t sec, long nsec)
{
    uint64_t t = ((uint64_t)sec + 11644473600ull) * 10000000ull + (uint64_t)nsec / 100;
    rt_w32(G_MEM, at, (uint32_t)t); rt_w32(G_MEM, at + 4, (uint32_t)(t >> 32));
}
IMPL(kernel32, GetFileInformationByHandleEx)
{
    int fd = w32_file_fd(ARG(0)); uint32_t cls = ARG(1), b = ARG(2), n = ARG(3); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { w32_set_last_error(c, 6); RET(0, 4); }
    uint32_t attr = S_ISDIR(st.st_mode) ? 0x10 : (st.st_mode & S_IWUSR) ? 0x80 : 0x01;    /* NORMAL / READONLY */
    memset(GP(b), 0, n);
    switch (cls) {
    case 0:                                                           /* FileBasicInfo: 4 times, attributes */
#ifdef __APPLE__
        ft64(b, st.st_birthtimespec.tv_sec, st.st_birthtimespec.tv_nsec); ft64(b + 8, st.st_atimespec.tv_sec, st.st_atimespec.tv_nsec);
        ft64(b + 16, st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec); ft64(b + 24, st.st_mtimespec.tv_sec, st.st_mtimespec.tv_nsec);
#else
        ft64(b, st.st_ctim.tv_sec, st.st_ctim.tv_nsec); ft64(b + 8, st.st_atim.tv_sec, st.st_atim.tv_nsec);
        ft64(b + 16, st.st_mtim.tv_sec, st.st_mtim.tv_nsec); ft64(b + 24, st.st_mtim.tv_sec, st.st_mtim.tv_nsec);
#endif
        rt_w32(G_MEM, b + 32, attr); break;
    case 1:                                                           /* FileStandardInfo: allocation, size, links, delete, dir */
        rt_w64(G_MEM, b, (uint64_t)st.st_blocks * 512); rt_w64(G_MEM, b + 8, (uint64_t)st.st_size);
        rt_w32(G_MEM, b + 16, (uint32_t)st.st_nlink); rt_w8(G_MEM, b + 21, S_ISDIR(st.st_mode)); break;
    case 9:                                                           /* FileAttributeTagInfo */
        rt_w32(G_MEM, b, attr); break;
    case 18:                                                          /* FileIdInfo: volume serial (u64), 128-bit id */
        rt_w64(G_MEM, b, (uint64_t)st.st_dev); rt_w64(G_MEM, b + 8, (uint64_t)st.st_ino); break;
    default:
        fprintf(stderr, "w32: GetFileInformationByHandleEx class %u not supported\n", cls); w32_set_last_error(c, 87); RET(0, 4);
    }
    RET(1, 4);
}

/* ---- user32: one game window, no other applications ---- */
extern int w32_screen_w, w32_screen_h;
IMPL(user32, FindWindowExA) { RET(0, 4); }                             /* no other instance is running */
IMPL(user32, SetWindowTextW) { uint32_t a = ansi_of(ARG(1)); rt_w32(G_MEM, c->esp + 8, a); void imp_user32_SetWindowTextA(Ctx *); imp_user32_SetWindowTextA(c); unscratch(a); }
IMPL(user32, SetCapture) { static uint32_t cap; uint32_t o = cap; cap = ARG(0); RET(o, 1); }
IMPL(user32, ReleaseCapture) { RET(1, 0); }
IMPL(user32, DestroyCursor) { RET(1, 1); }
IMPL(user32, LoadCursorFromFileA) { RET(0x30000 + 32512, 1); }        /* the arrow (IDC_ARROW) */
/* one monitor, the display: HMONITOR 0x0001beef (as MonitorFromWindow answers) */
IMPL(user32, GetMonitorInfoA)                                         /* MONITORINFO(EX): size, monitor, work, flags[, device] */
{
    uint32_t m = ARG(1), sz = rt_r32(G_MEM, m);
    rt_w32(G_MEM, m + 4, 0); rt_w32(G_MEM, m + 8, 0); rt_w32(G_MEM, m + 12, (uint32_t)w32_screen_w); rt_w32(G_MEM, m + 16, (uint32_t)w32_screen_h);
    rt_w32(G_MEM, m + 20, 0); rt_w32(G_MEM, m + 24, 0); rt_w32(G_MEM, m + 28, (uint32_t)w32_screen_w); rt_w32(G_MEM, m + 32, (uint32_t)w32_screen_h);
    rt_w32(G_MEM, m + 36, 1);                                         /* MONITORINFOF_PRIMARY */
    if (sz >= 72) strcpy((char *)GP(m + 40), "\\\\.\\DISPLAY1");
    RET(1, 2);
}
IMPL(user32, EnumDisplayMonitors)                                     /* (hdc, clip, callback(hmon, hdc, rect *, data), data) */
{
    uint32_t r = scratch(16); rt_w32(G_MEM, r, 0); rt_w32(G_MEM, r + 4, 0); rt_w32(G_MEM, r + 8, (uint32_t)w32_screen_w); rt_w32(G_MEM, r + 12, (uint32_t)w32_screen_h);
    uint32_t a[4] = {0x0001beefu, 0, r, ARG(3)}; w32_callback(c, ARG(2), 4, a); unscratch(r);
    RET(1, 4);
}
/* menus: handles only (the game's window has none in full screen) */
static uint32_t next_menu = 0x00400000u;
IMPL(user32, CreateMenu) { RET(next_menu++, 0); }
IMPL(user32, CreatePopupMenu) { RET(next_menu++, 0); }
IMPL(user32, DestroyMenu) { RET(1, 1); }
IMPL(user32, AppendMenuA) { RET(1, 4); }
IMPL(user32, RemoveMenu) { RET(1, 3); }
IMPL(user32, GetMenu) { RET(0, 1); }
IMPL(user32, GetMenuItemCount) { RET(0, 1); }
IMPL(user32, GetMenuItemInfoA) { RET(0, 4); }
IMPL(user32, SetMenuItemInfoA) { RET(1, 4); }
IMPL(user32, TrackPopupMenu) { RET(0, 7); }
IMPL(user32, FrameRect) { RET(1, 3); }
IMPL(user32, InvertRect) { RET(1, 2); }
IMPL(user32, GetClassInfoA) { w32_set_last_error(c, 1411); RET(0, 3); }       /* ERROR_CLASS_DOES_NOT_EXIST */
IMPL(user32, LoadStringA) { if (ARG(3)) rt_w8(G_MEM, ARG(2), 0); RET(0, 4); }    /* no string table */
uint32_t w32_format(char *out, size_t cap, const char *f, uint32_t va);
IMPL(user32, wvsprintfA) { char buf[1025]; uint32_t n = w32_format(buf, sizeof buf, GS(ARG(1)), ARG(2)); memcpy(GP(ARG(0)), buf, n + 1); RET(n, 3); }

/* ---- gdi32: the game draws with Vulkan; what it does through GDI (palette setup on the window's DC, a few shapes on
 * loading screens) is accepted and tracked loosely, like the stock objects ---- */
#define GDI_LOOSE 0x00b80040u
IMPL(gdi32, SetMapMode) { RET(1, 2); }                                 /* MM_TEXT before */
IMPL(gdi32, CreatePalette) { RET(GDI_LOOSE + 1, 1); }
IMPL(gdi32, SelectPalette) { RET(GDI_LOOSE + 1, 3); }
IMPL(gdi32, RealizePalette) { RET(0, 1); }
IMPL(gdi32, UnrealizeObject) { RET(1, 1); }
IMPL(gdi32, GetSystemPaletteEntries) { RET(0, 4); }                    /* not a palette device */
IMPL(gdi32, CreatePen) { RET(GDI_LOOSE + 2, 3); }
IMPL(gdi32, CreateSolidBrush) { RET(GDI_LOOSE + 3, 1); }
IMPL(gdi32, MoveToEx) { if (ARG(3)) { rt_w32(G_MEM, ARG(3), 0); rt_w32(G_MEM, ARG(3) + 4, 0); } RET(1, 4); }
IMPL(gdi32, LineTo) { RET(1, 3); }
IMPL(gdi32, Ellipse) { RET(1, 5); }
IMPL(gdi32, GetBkColor) { RET(0xffffff, 1); }
/* EnumSystemLocalesW(callback(LPWSTR locale id), flags): one locale, US English ("00000409") */
IMPL(kernel32, EnumSystemLocalesW)
{
    uint32_t s = scratch(32); const char *id = "00000409";
    for (int k = 0; k < 9; k++) rt_w16(G_MEM, s + 2 * (uint32_t)k, (uint8_t)id[k]);
    uint32_t a[1] = {s}; w32_callback(c, ARG(0), 1, a); unscratch(s);
    RET(1, 2);
}
/* GetModuleHandleExW(flags, name or address, HMODULE *): flag 4 (FROM_ADDRESS) asks which image holds an address */
IMPL(kernel32, GetModuleHandleExW)
{
    uint32_t flags = ARG(0), n = ARG(1), out = ARG(2), h = 0;
    extern const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[]; extern const unsigned rt_images_n;
    if (flags & 4) {
        if (n >= w32_image_base && n < w32_image_base + w32_image_size) h = w32_image_base;
        for (unsigned k = 1; k < rt_images_n && !h; k++) {
            uint32_t b = rt_images[k].base; if (n < b || rt_r16(G_MEM, b) != 0x5a4d) continue;
            uint32_t pe = rt_r32(G_MEM, b + 0x3c), size = rt_r32(G_MEM, b + pe + 24 + 56);
            if (n < b + size) h = b;
        }
    } else if (!n) h = w32_image_base;
    else {
        uint32_t a = ansi_of(n), esp = c->esp; void imp_kernel32_GetModuleHandleA(Ctx *);
        rt_w32(G_MEM, esp, rt_r32(G_MEM, esp)); c->esp -= 0;                  /* GetModuleHandleA(name): one argument */
        uint32_t save = rt_r32(G_MEM, esp + 4); rt_w32(G_MEM, esp + 4, a);
        imp_kernel32_GetModuleHandleA(c); h = c->eax; c->esp = esp; rt_w32(G_MEM, esp + 4, save); unscratch(a);
    }
    if (out) rt_w32(G_MEM, out, h);
    if (!h) { w32_set_last_error(c, 126); RET(0, 3); }
    RET(1, 3);
}
/* SearchPathA(path, file, extension, cap, buffer, file part): the given path, else the game's folder */
IMPL(kernel32, SearchPathA)
{
    const char *dir = ARG(0) ? GS(ARG(0)) : "C:\\GOG Games\\Dungeon Siege", *file = GS(ARG(1)), *ext = ARG(2) ? GS(ARG(2)) : "";
    char win[1024], host[2048]; struct stat st;
    if (strchr(file, '\\') || strchr(file, ':')) snprintf(win, sizeof win, "%s", file);
    else snprintf(win, sizeof win, "%s\\%s%s", dir, file, strchr(file, '.') ? "" : ext);
    if (w32_host_path(win, host, sizeof host, 0) || stat(host, &st)) { w32_set_last_error(c, 2); RET(0, 6); }
    uint32_t n = (uint32_t)strlen(win), cap = ARG(3), buf = ARG(4);
    if (n + 1 > cap) RET(n + 1, 6);
    memcpy(GP(buf), win, n + 1);
    if (ARG(5)) { const char *b = strrchr(win, '\\'); rt_w32(G_MEM, ARG(5), buf + (uint32_t)(b ? b + 1 - win : 0)); }
    RET(n, 6);
}

/* ---- winmm: multimedia timers. Each runs on a runtime service thread that sleeps out its period and calls the
 * guest's callback (uTimerID, uMsg, dwUser, 0, 0), or sets/pulses the event handle it was given ---- */
int w32_spawn_service(void (*fn)(Ctx *, void *), void *arg);
typedef struct MmTimer { uint32_t id, delay, fn, user, flags; volatile int killed; } MmTimer;
static MmTimer *mm_timers[64];
static void mm_timer_run(Ctx *c, void *arg)
{
    MmTimer *t = arg;
    struct timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    do {
        uint64_t ns = (uint64_t)next.tv_nsec + (uint64_t)t->delay * 1000000u;
        next.tv_sec += (time_t)(ns / 1000000000u); next.tv_nsec = (long)(ns % 1000000000u);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);
        if (t->killed) break;
        if (t->flags & 0x30) w32_event_signal(t->fn, 1);              /* TIME_CALLBACK_EVENT_SET / _PULSE */
        else { uint32_t a[5] = {t->id, 0, t->user, 0, 0}; w32_callback(c, t->fn, 5, a); }
    } while ((t->flags & 1) && !t->killed);                        /* TIME_PERIODIC */
}
IMPL(winmm, timeSetEvent)                                              /* (delay, resolution, callback, user, flags) */
{
    int k = 1; while (k < 64 && mm_timers[k]) k++;
    if (k == 64) RET(0, 5);
    MmTimer *t = calloc(1, sizeof *t); t->id = (uint32_t)k; t->delay = ARG(0) ? ARG(0) : 1; t->fn = ARG(2); t->user = ARG(3); t->flags = ARG(4);
    mm_timers[k] = t;
    if (w32_spawn_service(mm_timer_run, t)) { mm_timers[k] = 0; free(t); RET(0, 5); }
    RET((uint32_t)k, 5);
}
IMPL(winmm, timeKillEvent)
{
    uint32_t k = ARG(0); if (k == 0 || k >= 64 || !mm_timers[k]) RET(97, 1);   /* MMSYSERR_INVALPARAM */
    mm_timers[k]->killed = 1; mm_timers[k] = 0;                    /* the service thread lets go of it */
    RET(0, 1);
}

/* ---- asynchronous procedure calls: QueueUserAPC to a thread, run when it next waits alertably (SleepEx) ---- */
typedef struct Sync { pthread_mutex_t m; pthread_cond_t cv; int manual, signaled, count; uint32_t owner; char name[64]; int waiters, closed; } Sync;   /* as in threads.c */
typedef struct ThreadObj { Sync s; uint32_t teb, tid, exit_code, suspended; pthread_t pt; } ThreadObj;
typedef struct Apc { uint32_t teb, fn, data; } Apc;
static Apc apcs[256]; static int napcs;
static uint32_t teb_of_handle(Ctx *c, uint32_t h)
{
    if (h == 0xfffffffeu) return c->fs_base;
    HObj *o = h_get(h, H_THREAD); return o ? ((ThreadObj *)o->p)->teb : 0;
}
IMPL(kernel32, QueueUserAPC)                                           /* (function, thread, data) */
{
    uint32_t teb = teb_of_handle(c, ARG(1)); if (!teb) { w32_set_last_error(c, 6); RET(0, 3); }
    pthread_mutex_lock(&sync_m);
    int ok = napcs < 256; if (ok) { apcs[napcs].teb = teb; apcs[napcs].fn = ARG(0); apcs[napcs].data = ARG(2); napcs++; pthread_cond_broadcast(&sync_cv); }
    pthread_mutex_unlock(&sync_m);
    RET(ok, 3);
}
/* runs this thread's queued APCs (sync_m held on entry, released on return); returns how many ran */
static int run_apcs(Ctx *c)
{
    Apc mine[256]; int n = 0;
    for (int k = 0; k < napcs; ) if (apcs[k].teb == c->fs_base) { mine[n++] = apcs[k]; apcs[k] = apcs[--napcs]; } else k++;
    pthread_mutex_unlock(&sync_m);
    for (int k = 0; k < n; k++) { uint32_t a[1] = {mine[k].data}; w32_callback(c, mine[k].fn, 1, a); }
    return n;
}
static int has_apc(uint32_t teb) { for (int k = 0; k < napcs; k++) if (apcs[k].teb == teb) return 1; return 0; }
IMPL(kernel32, SleepEx)                                                /* (ms, alertable): WAIT_IO_COMPLETION when APCs ran */
{
    uint32_t ms = ARG(0);
    if (!ARG(1)) { void imp_kernel32_Sleep(Ctx *); uint32_t esp = c->esp; imp_kernel32_Sleep(c); c->esp = esp + 12; c->eax = 0; return; }
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until);
    if (ms != 0xffffffffu) { until.tv_sec += ms / 1000; until.tv_nsec += (long)(ms % 1000) * 1000000; if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; } }
    pthread_mutex_lock(&sync_m);
    while (!has_apc(c->fs_base)) {
        if (ms == 0xffffffffu) pthread_cond_wait(&sync_cv, &sync_m);
        else if (!ms || pthread_cond_timedwait(&sync_cv, &sync_m, &until)) break;
    }
    if (has_apc(c->fs_base)) { run_apcs(c); RET(0xc0, 2); }
    pthread_mutex_unlock(&sync_m);
    RET(0, 2);
}
IMPL(kernel32, FreeLibraryAndExitThread)                               /* (module, code): modules stay loaded */
{
    rt_w32(G_MEM, c->esp + 4, ARG(1)); void imp_kernel32_ExitThread(Ctx *); imp_kernel32_ExitThread(c);
}
IMPL(kernel32, _lclose) { void imp_kernel32_CloseHandle(Ctx *); imp_kernel32_CloseHandle(c); c->eax = c->eax ? 0 : 0xffffffffu; }   /* HFILE_ERROR */

/* ---- kernel32: what std::filesystem uses to copy the template user data on first run ---- */
IMPL(kernel32, CreateDirectoryExW)                                     /* (template, new, attributes): the template is not copied */
{
    rt_w32(G_MEM, c->esp + 4, ARG(1)); rt_w32(G_MEM, c->esp + 8, ARG(2));
    void imp_kernel32_CreateDirectoryW(Ctx *); imp_kernel32_CreateDirectoryW(c);       /* pops 2 of our 3 */
    c->esp += 4;
}
IMPL(kernel32, MoveFileExA)                                            /* (from, to, flags: 1 = replace existing) */
{
    char a[2048], b[2048];
    if (w32_host_path(GS(ARG(0)), a, sizeof a, 0) || w32_host_path(GS(ARG(1)), b, sizeof b, 1)) { w32_set_last_error(c, 3); RET(0, 3); }
    struct stat st; if (!(ARG(2) & 1) && !stat(b, &st)) { w32_set_last_error(c, 183); RET(0, 3); }
    if (rename(a, b)) { w32_set_last_error(c, 2); RET(0, 3); }
    RET(1, 3);
}
IMPL(kernel32, MoveFileExW) { VIA_ANSI(imp_kernel32_MoveFileExA, 3); }
IMPL(kernel32, SetCurrentDirectoryW) { w32_set_last_error(c, 5); RET(0, 1); }          /* the game's folder stays current */
IMPL(kernel32, CreateHardLinkW) { w32_set_last_error(c, 1); RET(0, 3); }
IMPL(kernel32, CreateSymbolicLinkW) { w32_set_last_error(c, 1); RET(0, 3); }
IMPL(kernel32, SetFileInformationByHandle) { RET(1, 4); }             /* attributes and times: kept as they are */
IMPL(kernel32, GetFinalPathNameByHandleW)                              /* (handle, buffer, cap, flags): the path it was opened by */
{
    extern const char *w32_file_win_path(uint32_t h);
    const char *p = w32_file_win_path(ARG(0)); if (!p) { w32_set_last_error(c, 6); RET(0, 4); }
    char full[1100]; snprintf(full, sizeof full, "\\\\?\\%s", p);
    int n = w32_mb_to_wide(0, (const uint8_t *)full, -1, 0, 0);
    if ((uint32_t)n > ARG(2)) RET((uint32_t)n, 4);
    w32_mb_to_wide(0, (const uint8_t *)full, -1, (uint16_t *)GP(ARG(1)), (int)ARG(2)); RET((uint32_t)n - 1, 4);
}
