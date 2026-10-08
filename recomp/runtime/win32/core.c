/* KERNEL32 core: version, modules, environment, TLS, heaps, virtual memory, critical sections, interlocked, time,
 * synchronisation objects, handles, process control. */
#include "w32.h"

#include <unistd.h>
#include <time.h>
#include "platform.h"
#include <sys/stat.h>

extern uint32_t w32_cmdline_a, w32_cmdline_w, w32_modname_a, w32_env_a, w32_env_w;

/* ---- handles ---- */
#define MAXH 4096
static HObj htab[MAXH]; static pthread_mutex_t hlock = PTHREAD_MUTEX_INITIALIZER;
uint32_t h_new(int type, void *p)
{
    pthread_mutex_lock(&hlock);
    for (int k = 1; k < MAXH; k++) if (htab[k].type == H_FREE) {
        htab[k].type = type; htab[k].refs = 1; htab[k].p = p; pthread_mutex_unlock(&hlock); return 0x100 + 4 * (uint32_t)k;
    }
    pthread_mutex_unlock(&hlock); return 0;
}
HObj *h_get(uint32_t h, int type)
{
    uint32_t k = (h - 0x100) / 4;
    if (h < 0x100 || (h & 3) || k >= MAXH || htab[k].type == H_FREE || (type && htab[k].type != type)) return 0;
    return &htab[k];
}
void w32_handle_closed(HObj *o);   /* files.c / sync below */
int h_close(uint32_t h)
{
    HObj *o = h_get(h, 0); if (!o) return 0;
    pthread_mutex_lock(&hlock);
    if (--o->refs == 0) { w32_handle_closed(o); o->type = H_FREE; o->p = 0; }
    pthread_mutex_unlock(&hlock); return 1;
}

/* ---- version / modules ---- */
IMPL(kernel32, GetVersion) { RET(0x0a280105u, 0); }          /* Windows XP, build 2600 */
IMPL(kernel32, IsProcessorFeaturePresent)
{
    /* Match rt_cpuid's guest CPU: CMPXCHG8B and RDTSC, without MMX/SSE.
     * Reporting the host CPU's SIMD support would select unlifted guest code.
     * Feature IDs: https://learn.microsoft.com/windows/win32/api/processthreadsapi/nf-processthreadsapi-isprocessorfeaturepresent */
    RET(ARG(0) == 2 || ARG(0) == 8, 1);
}
IMPL(kernel32, GetVersionExA)
{
    uint32_t p = ARG(0), sz = rt_r32(G_MEM, p);
    memset(GP(p + 4), 0, sz - 4);
    rt_w32(G_MEM, p + 4, 5); rt_w32(G_MEM, p + 8, 1); rt_w32(G_MEM, p + 12, 2600); rt_w32(G_MEM, p + 16, 2);
    strcpy((char *)GP(p + 20), "Service Pack 3");
    if (sz >= 156) rt_w16(G_MEM, p + 148, 3);
    RET(1, 1);
}
static const char *known_dlls[] = {"kernel32", "user32", "gdi32", "advapi32", "ddraw", "dsound", "dinput", "dinput8",
    "winmm", "ole32", "oleaut32", "version", "wsock32", "ws2_32", "mss32", "binkw32", "ntdll", "shell32", "comctl32", "imm32", "shfolder", "dbghelp", "d3d9",
    "vulkan-1", "steam_api", "dwmapi", "wininet", "msvfw32", "psapi", 0};
#define MODULE_BASE 0x70000000u
static int dll_index(const char *name)
{
    char b[64]; size_t k = 0; const char *s = strrchr(name, '\\'); s = s ? s + 1 : name;
    for (; s[k] && s[k] != '.' && k < sizeof b - 1; k++) b[k] = (char)(s[k] >= 'A' && s[k] <= 'Z' ? s[k] + 32 : s[k]);
    b[k] = 0;
    for (int j = 0; known_dlls[j]; j++) if (!strcmp(known_dlls[j], b)) return j;
    return -1;
}
uint32_t w32_load_library(Ctx *c, const char *name); uint32_t w32_image_module(const char *name);
const char *w32_image_name(uint32_t base); uint32_t w32_export(uint32_t base, const char *name, uint32_t ordinal);
IMPL(kernel32, GetModuleHandleA)
{
    uint32_t n = ARG(0);
    if (!n) RET(w32_image_base, 1);
    { uint32_t b = w32_image_module(GS(n)); if (b) RET(b, 1); }
    int k = dll_index(GS(n));
    if (k < 0) { w32_set_last_error(c, 126); RET(0, 1); }
    RET(MODULE_BASE + 0x100000u * (uint32_t)k, 1);
}
IMPL(kernel32, LoadLibraryA)
{
    if (getenv("W32_LIBLOG")) fprintf(stderr, "w32: LoadLibraryA(%s)\n", GS(ARG(0)));
    { uint32_t b = w32_load_library(c, GS(ARG(0))); if (b) RET(b, 1); }
    int k = dll_index(GS(ARG(0)));
    if (k < 0) { fprintf(stderr, "w32: LoadLibraryA(%s) -> not available\n", GS(ARG(0))); w32_set_last_error(c, 126); RET(0, 1); }
    RET(MODULE_BASE + 0x100000u * (uint32_t)k, 1);
}
IMPL(kernel32, LoadLibraryExA)
{
    if (getenv("W32_LIBLOG")) fprintf(stderr, "w32: LoadLibraryExA(%s, flags %x)\n", GS(ARG(0)), ARG(2));
    { uint32_t b = w32_load_library(c, GS(ARG(0))); if (b) RET(b, 3); }
    int k = dll_index(GS(ARG(0)));
    if (k < 0) { fprintf(stderr, "w32: LoadLibraryExA(%s) -> not available\n", GS(ARG(0))); w32_set_last_error(c, 126); RET(0, 3); }
    RET(MODULE_BASE + 0x100000u * (uint32_t)k, 3);
}
IMPL(kernel32, FreeLibrary) { RET(1, 1); }
IMPL(kernel32, GetProcAddress)
{
    uint32_t h = ARG(0), n = ARG(1); char ord[16]; const char *name = ord;
    if (n < 0x10000) snprintf(ord, sizeof ord, "#%u", n); else name = GS(n);
    if (w32_image_name(h)) { uint32_t a = n < 0x10000 ? w32_export(h, 0, n) : w32_export(h, name, 0); if (!a) w32_set_last_error(c, 127); RET(a, 2); }
    uint32_t k = (h - MODULE_BASE) / 0x100000u;
    if (h < MODULE_BASE || k >= sizeof known_dlls / sizeof *known_dlls - 1) { w32_set_last_error(c, 127); RET(0, 2); }
    extern int w32_thunk_implemented(uint32_t);
    uint32_t t = w32_thunk_for(known_dlls[k], name);
    if (!t || !w32_thunk_implemented(t)) { fprintf(stderr, "w32: GetProcAddress(%s, %s) -> not available\n", known_dlls[k], name); w32_set_last_error(c, 127); RET(0, 2); }
    RET(t, 2);
}
IMPL(kernel32, GetModuleFileNameA)
{
    uint32_t h = ARG(0), buf = ARG(1), cap = ARG(2);
    char path[512]; const char *img = w32_image_name(h);
    if (img) { snprintf(path, sizeof path, "C:\\GOG Games\\Dungeon Siege\\%s", img); for (char *p = path; *p; p++) if (*p == '/') *p = '\\'; }
    const char *s = (!h || h == w32_image_base) ? GS(w32_modname_a) : img ? path : "C:\\Windows\\system32\\unknown.dll";
    uint32_t n = (uint32_t)strlen(s);
    if (!cap) RET(0, 3);
    if (n >= cap) n = cap - 1;
    memcpy(GP(buf), s, n); rt_w8(G_MEM, buf + n, 0);
    RET(n, 3);
}
IMPL(kernel32, GetCommandLineA) { RET(w32_cmdline_a, 0); }
IMPL(kernel32, GetCommandLineW) { RET(w32_cmdline_w, 0); }
IMPL(kernel32, GetStartupInfoA) { uint32_t p = ARG(0); memset(GP(p), 0, 68); rt_w32(G_MEM, p, 68); RET(0, 1); }
IMPL(kernel32, GetEnvironmentStrings) { RET(w32_env_a, 0); }
IMPL(kernel32, GetEnvironmentStringsW) { RET(w32_env_w, 0); }
IMPL(kernel32, FreeEnvironmentStringsA) { RET(1, 1); }
IMPL(kernel32, FreeEnvironmentStringsW) { RET(1, 1); }
IMPL(kernel32, GetEnvironmentVariableA)
{
    const char *name = GS(ARG(0)); uint32_t buf = ARG(1), cap = ARG(2); size_t nl = strlen(name);
    for (uint32_t s = w32_env_a; G_MEM[s]; s += (uint32_t)strlen(GS(s)) + 1) {
        if (!strncasecmp(GS(s), name, nl) && GS(s)[nl] == '=') {
            const char *v = GS(s) + nl + 1; uint32_t n = (uint32_t)strlen(v);
            if (n + 1 > cap) RET(n + 1, 3);
            memcpy(GP(buf), v, n + 1); RET(n, 3);
        }
    }
    w32_set_last_error(c, 203); RET(0, 3);
}
IMPL(kernel32, SetHandleCount) { RET(ARG(0), 1); }
IMPL(kernel32, GetStdHandle) { RET(0, 1); }                  /* a GUI process: no console handles */
IMPL(kernel32, SetStdHandle) { RET(1, 2); }

/* ---- errors, threads, TLS ---- */
IMPL(kernel32, SetLastError) { w32_set_last_error(c, ARG(0)); RET(0, 1); }
IMPL(kernel32, GetLastError) { RET(rt_r32(G_MEM, c->fs_base + 0x34), 0); }
IMPL(kernel32, GetCurrentThread) { RET(0xfffffffeu, 0); }
IMPL(kernel32, GetCurrentThreadId) { RET(w32_tid(c), 0); }
IMPL(kernel32, GetCurrentProcess) { RET(0xffffffffu, 0); }
IMPL(kernel32, GetCurrentProcessId) { RET(0x40, 0); }
static uint64_t tls_used; static pthread_mutex_t tls_lock = PTHREAD_MUTEX_INITIALIZER;
IMPL(kernel32, TlsAlloc)
{
    pthread_mutex_lock(&tls_lock);
    int k = 0; while (k < 64 && (tls_used >> k & 1)) k++;
    if (k < 64) tls_used |= 1ull << k;
    pthread_mutex_unlock(&tls_lock);
    RET(k < 64 ? (uint32_t)k : 0xffffffffu, 0);
}
IMPL(kernel32, TlsGetValue)
{
    uint32_t k = ARG(0);
    if (k >= 64) { w32_set_last_error(c, 87); RET(0, 1); }
    w32_set_last_error(c, 0); RET(rt_r32(G_MEM, c->fs_base + 0xe10 + 4 * k), 1);
}
IMPL(kernel32, TlsSetValue)
{
    uint32_t k = ARG(0);
    if (k >= 64) { w32_set_last_error(c, 87); RET(0, 2); }
    rt_w32(G_MEM, c->fs_base + 0xe10 + 4 * k, ARG(1)); RET(1, 2);
}

/* ---- heaps and virtual memory ---- */
IMPL(kernel32, HeapCreate) { RET(heap_create(ARG(1), ARG(2)), 3); }
IMPL(kernel32, HeapDestroy) { RET(heap_destroy(ARG(0)), 1); }
IMPL(kernel32, HeapAlloc) { RET(heap_alloc(ARG(0), ARG(1), ARG(2)), 3); }
IMPL(kernel32, HeapFree) { RET(heap_free(ARG(0), ARG(2)), 3); }
IMPL(kernel32, GetProcessHeap) { RET(w32_process_heap, 0); }
IMPL(kernel32, VirtualAlloc) { uint32_t r = vm_alloc(ARG(0), ARG(1), ARG(2), ARG(3)); if (!r) w32_set_last_error(c, 8); RET(r, 4); }
IMPL(kernel32, VirtualFree) { RET(vm_free(ARG(0), ARG(1), ARG(2)), 3); }
IMPL(kernel32, VirtualQuery) { RET(vm_query(ARG(0), ARG(1)), 3); }
IMPL(kernel32, VirtualQueryEx) { RET(vm_query(ARG(1), ARG(2)), 4); }
IMPL(kernel32, VirtualProtect) { if (ARG(3)) rt_w32(G_MEM, ARG(3), 4); RET(1, 4); }
IMPL(kernel32, VirtualLock) { RET(1, 2); }
IMPL(kernel32, GlobalAlloc)
{
    uint32_t p = heap_alloc(w32_process_heap, (ARG(0) & 0x40) ? 8 : 0, ARG(1) ? ARG(1) : 1);   /* GMEM_ZEROINIT */
    RET(p, 2);
}
IMPL(kernel32, GlobalLock) { RET(ARG(0), 1); }
IMPL(kernel32, GlobalUnlock) { RET(1, 1); }
IMPL(kernel32, GlobalFree) { heap_free(w32_process_heap, ARG(0)); RET(0, 1); }
IMPL(kernel32, LocalFree) { heap_free(w32_process_heap, ARG(0)); RET(0, 1); }
IMPL(kernel32, IsBadReadPtr) { RET(ARG(1) && !vm_committed(ARG(0), ARG(1)), 2); }
IMPL(kernel32, IsBadWritePtr) { RET(ARG(1) && !vm_committed(ARG(0), ARG(1)), 2); }
IMPL(kernel32, IsBadCodePtr) { RET(!(ARG(0) >= w32_image_base && ARG(0) < w32_image_base + w32_image_size), 1); }
IMPL(kernel32, FlushInstructionCache) { RET(1, 3); }
IMPL(kernel32, GetSystemInfo)
{
    uint32_t p = ARG(0); memset(GP(p), 0, 36);
    rt_w32(G_MEM, p + 4, 4096); rt_w32(G_MEM, p + 8, 0x10000); rt_w32(G_MEM, p + 12, 0x7ffeffff);
    /* The real core count, as Windows and Wine report it: the game picks its locks by it (0x43ec79: one CPU -> plain
     * inc/dec, which is only safe when threads never run at the same time; ours do). The harness keeps 1. */
    uint32_t ncpu = 1;
    if (!w32_deterministic) ncpu = ds_cpu_count();
    rt_w32(G_MEM, p + 16, ncpu >= 32 ? 0xffffffffu : (1u << ncpu) - 1); rt_w32(G_MEM, p + 20, ncpu); rt_w32(G_MEM, p + 24, 586); rt_w32(G_MEM, p + 28, 0x10000);
    rt_w16(G_MEM, p + 32, 6); rt_w16(G_MEM, p + 34, 0x0f01);
    RET(0, 1);
}
IMPL(kernel32, GlobalMemoryStatusEx)                   /* MEMORYSTATUSEX: 2 GB of physical memory free for a 32-bit process */
{
    uint32_t p = ARG(0); uint64_t v[7] = {8ull << 30, 6ull << 30, 16ull << 30, 14ull << 30, 0x7ffe0000ull, 0x70000000ull, 0};
    rt_w32(G_MEM, p, 64); rt_w32(G_MEM, p + 4, 25); memcpy(GP(p + 8), v, 56); RET(1, 1);
}
IMPL(kernel32, GlobalMemoryStatus)
{
    uint32_t p = ARG(0), v[8] = {32, 20, 0x7fff0000u, 0x60000000u, 0x7fff0000u, 0x70000000u, 0x7ffe0000u, 0x70000000u};
    memcpy(GP(p), v, 32); RET(0, 1);
}

/* ---- critical sections (host recursive mutexes keyed by the guest address) ---- */
typedef struct CS { uint32_t addr; pthread_mutex_t m; } CS;
#define NCS 4096
static CS cstab[NCS]; static pthread_mutex_t cs_lock = PTHREAD_MUTEX_INITIALIZER;
static CS *cs_find(uint32_t a, int create)
{
    uint32_t k = (a * 2654435761u) % NCS;
    CS *free_slot = 0;
    pthread_mutex_lock(&cs_lock);
    for (int n = 0; n < NCS; n++, k = (k + 1) % NCS) {
        if (cstab[k].addr == a) { pthread_mutex_unlock(&cs_lock); return &cstab[k]; }
        if (!cstab[k].addr || cstab[k].addr == 1) {
            if (!free_slot) free_slot = &cstab[k];
            /* A deleted entry is not the end of a probe chain. Reusing it
             * before checking later entries can create two mutexes for one
             * live guest critical section, defeating mutual exclusion. */
            if (!cstab[k].addr) break;
        }
    }
    if (create && free_slot) {
        pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&free_slot->m, &at); pthread_mutexattr_destroy(&at); free_slot->addr = a;
        pthread_mutex_unlock(&cs_lock); return free_slot;
    }
    pthread_mutex_unlock(&cs_lock); return 0;
}
IMPL(kernel32, InitializeCriticalSection)
{
    uint32_t p = ARG(0); memset(GP(p), 0, 24); rt_w32(G_MEM, p + 4, 0xffffffffu);   /* LockCount -1 */
    cs_find(p, 1); RET(0, 1);
}
IMPL(kernel32, DeleteCriticalSection)
{
    CS *s = cs_find(ARG(0), 0);
    if (s) { pthread_mutex_lock(&cs_lock); pthread_mutex_destroy(&s->m); s->addr = 1; pthread_mutex_unlock(&cs_lock); }
    RET(0, 1);
}
IMPL(kernel32, EnterCriticalSection)
{
    uint32_t p = ARG(0); CS *s = cs_find(p, 1);
    void w32_crash_wait_begin(int, uint32_t, uint32_t), w32_crash_wait_end(void);
    if (pthread_mutex_trylock(&s->m)) { w32_crash_wait_begin(2, p, 0xffffffffu); pthread_mutex_lock(&s->m); w32_crash_wait_end(); }
    rt_w32(G_MEM, p + 8, rt_r32(G_MEM, p + 8) + 1); rt_w32(G_MEM, p + 12, w32_tid(c));
    RET(0, 1);
}
IMPL(kernel32, LeaveCriticalSection)
{
    uint32_t p = ARG(0); CS *s = cs_find(p, 1);
    uint32_t r = rt_r32(G_MEM, p + 8) - 1; rt_w32(G_MEM, p + 8, r); if (!r) rt_w32(G_MEM, p + 12, 0);
    pthread_mutex_unlock(&s->m);
    RET(0, 1);
}
IMPL(kernel32, InitializeCriticalSectionAndSpinCount)
{
    uint32_t p = ARG(0); memset(GP(p), 0, 24); rt_w32(G_MEM, p + 4, 0xffffffffu); cs_find(p, 1); RET(1, 2);
}
IMPL(kernel32, TryEnterCriticalSection)
{
    uint32_t p = ARG(0); CS *s = cs_find(p, 1);
    if (pthread_mutex_trylock(&s->m)) RET(0, 1);
    rt_w32(G_MEM, p + 8, rt_r32(G_MEM, p + 8) + 1); rt_w32(G_MEM, p + 12, w32_tid(c));
    RET(1, 1);
}
IMPL(kernel32, InterlockedExchangeAdd) { RET(__atomic_fetch_add((uint32_t *)GP(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }
IMPL(kernel32, InterlockedIncrement) { RET(__atomic_add_fetch((uint32_t *)GP(ARG(0)), 1u, __ATOMIC_SEQ_CST), 1); }
IMPL(kernel32, InterlockedDecrement) { RET(__atomic_sub_fetch((uint32_t *)GP(ARG(0)), 1u, __ATOMIC_SEQ_CST), 1); }
IMPL(kernel32, InterlockedExchange) { RET(__atomic_exchange_n((uint32_t *)GP(ARG(0)), ARG(1), __ATOMIC_SEQ_CST), 2); }
IMPL(kernel32, InterlockedCompareExchange)
{
    uint32_t previous = ARG(2);
    __atomic_compare_exchange_n((uint32_t *)GP(ARG(0)), &previous, ARG(1), 0,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    RET(previous, 3);
}

/* ---- time: real, or a deterministic clock for the harness (1 ms per query) ---- */
static uint64_t fake_us = 1000000;
static uint64_t now_us(void)
{
    if (w32_deterministic) return fake_us += 1000;
    return ds_clock_ns() / 1000;
}
static uint64_t filetime_now(void)          /* 100 ns units since 1601 */
{
    if (w32_deterministic) return 132000000000000000ull + (fake_us += 1000) * 10;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec + 11644473600ull) * 10000000ull + (uint64_t)ts.tv_nsec / 100;
}
IMPL(kernel32, GetTickCount) { RET((uint32_t)(now_us() / 1000), 0); }
IMPL(winmm, timeGetTime) { RET((uint32_t)(now_us() / 1000), 0); }
IMPL(kernel32, QueryPerformanceFrequency) { rt_w64(G_MEM, ARG(0), 1000000); RET(1, 1); }
IMPL(kernel32, QueryPerformanceCounter) { rt_w64(G_MEM, ARG(0), now_us()); RET(1, 1); }
IMPL(kernel32, GetSystemTimeAsFileTime) { rt_w64(G_MEM, ARG(0), filetime_now()); RET(0, 1); }
static void ft_to_st(uint64_t ft, uint32_t st)
{
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull); struct tm tm; gmtime_r(&t, &tm);
    uint16_t v[8] = {(uint16_t)(tm.tm_year + 1900), (uint16_t)(tm.tm_mon + 1), (uint16_t)tm.tm_wday, (uint16_t)tm.tm_mday,
                     (uint16_t)tm.tm_hour, (uint16_t)tm.tm_min, (uint16_t)tm.tm_sec, (uint16_t)(ft / 10000 % 1000)};
    memcpy(GP(st), v, 16);
}
static uint64_t st_to_ft(uint32_t st)
{
    uint16_t v[8]; memcpy(v, GP(st), 16);
    struct tm tm = {0}; tm.tm_year = v[0] - 1900; tm.tm_mon = v[1] - 1; tm.tm_mday = v[3]; tm.tm_hour = v[4]; tm.tm_min = v[5]; tm.tm_sec = v[6];
    return ((uint64_t)timegm(&tm) + 11644473600ull) * 10000000ull + v[7] * 10000ull;
}
static int64_t tz_offset_ft(void)          /* local - UTC, in 100 ns */
{
    if (w32_deterministic) return 0;
    time_t t = time(0); struct tm lt; localtime_r(&t, &lt); return (int64_t)lt.tm_gmtoff * 10000000;
}
IMPL(kernel32, GetSystemTime) { ft_to_st(filetime_now(), ARG(0)); RET(0, 1); }
IMPL(kernel32, GetLocalTime) { ft_to_st(filetime_now() + (uint64_t)tz_offset_ft(), ARG(0)); RET(0, 1); }
IMPL(kernel32, SystemTimeToFileTime) { rt_w64(G_MEM, ARG(1), st_to_ft(ARG(0))); RET(1, 2); }
IMPL(kernel32, FileTimeToSystemTime) { ft_to_st(rt_r64(G_MEM, ARG(0)), ARG(1)); RET(1, 2); }
IMPL(kernel32, FileTimeToLocalFileTime) { rt_w64(G_MEM, ARG(1), rt_r64(G_MEM, ARG(0)) + (uint64_t)tz_offset_ft()); RET(1, 2); }
IMPL(kernel32, CompareFileTime)
{
    uint64_t a = rt_r64(G_MEM, ARG(0)), b = rt_r64(G_MEM, ARG(1));
    RET(a < b ? 0xffffffffu : a > b ? 1 : 0, 2);
}
IMPL(kernel32, Sleep)
{
    uint32_t ms = ARG(0);
    void w32_crash_wait_begin(int, uint32_t, uint32_t), w32_crash_wait_end(void);
    if (w32_deterministic) fake_us += (uint64_t)ms * 1000; else if (ms) { w32_crash_wait_begin(3, 0, ms); usleep(ms * 1000); w32_crash_wait_end(); } else sched_yield();
    RET(0, 1);
}

/* ---- synchronisation objects ---- */
typedef struct Sync { pthread_mutex_t m; pthread_cond_t cv; int manual, signaled, count; uint32_t owner; char name[64]; int waiters, closed; } Sync;   /* same in core.c and threads.c */
static Sync *sync_new(int manual, int signaled)
{
    Sync *s = calloc(1, sizeof *s); pthread_mutex_init(&s->m, 0); pthread_cond_init(&s->cv, 0);
    s->manual = manual; s->signaled = signaled; return s;
}
static Sync *named[64]; static uint32_t named_h[64]; static int nnamed;
IMPL(kernel32, CreateEventA)
{
    uint32_t name = ARG(3);
    if (name) for (int k = 0; k < nnamed; k++) if (!strcmp(named[k]->name, GS(name))) { w32_set_last_error(c, 183); htab[(named_h[k] - 0x100) / 4].refs++; RET(named_h[k], 4); }
    Sync *s = sync_new((int)ARG(1), (int)ARG(2)); uint32_t h = h_new(H_EVENT, s);
    if (name && nnamed < 64) { snprintf(s->name, sizeof s->name, "%s", GS(name)); named[nnamed] = s; named_h[nnamed++] = h; }
    w32_set_last_error(c, 0); RET(h, 4);
}
/* an event for runtime components that hand the game a handle to wait on (dshow.c) */
uint32_t w32_event_new(int manual, int signaled) { return h_new(H_EVENT, sync_new(manual, signaled)); }
void w32_event_signal(uint32_t h, int on)
{
    HObj *o = h_get(h, H_EVENT); if (!o) return;
    Sync *s = o->p; pthread_mutex_lock(&s->m); s->signaled = on; if (on) pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->m);
}
IMPL(kernel32, CreateMutexA)
{
    uint32_t name = ARG(2);
    if (name) for (int k = 0; k < nnamed; k++) if (!strcmp(named[k]->name, GS(name))) { w32_set_last_error(c, 183); htab[(named_h[k] - 0x100) / 4].refs++; RET(named_h[k], 3); }
    Sync *s = sync_new(0, 0); s->count = ARG(1) ? 1 : 0; s->owner = ARG(1) ? w32_tid(c) : 0;
    uint32_t h = h_new(H_MUTEX, s);
    if (name && nnamed < 64) { snprintf(s->name, sizeof s->name, "%s", GS(name)); named[nnamed] = s; named_h[nnamed++] = h; }
    w32_set_last_error(c, 0); RET(h, 3);
}
IMPL(kernel32, CreateSemaphoreA)                       /* (attributes, initial, maximum, name) */
{
    uint32_t name = ARG(3);
    if (name) for (int k = 0; k < nnamed; k++) if (!strcmp(named[k]->name, GS(name))) { w32_set_last_error(c, 183); htab[(named_h[k] - 0x100) / 4].refs++; RET(named_h[k], 4); }
    Sync *s = sync_new(0, 0); s->count = (int)ARG(1); s->owner = ARG(2);          /* owner: the maximum count */
    uint32_t h = h_new(H_SEMAPHORE, s);
    if (name && nnamed < 64) { snprintf(s->name, sizeof s->name, "%s", GS(name)); named[nnamed] = s; named_h[nnamed++] = h; }
    w32_set_last_error(c, 0); RET(h, 4);
}
IMPL(kernel32, ReleaseSemaphore)                       /* (handle, release count, previous count out) */
{
    HObj *o = h_get(ARG(0), H_SEMAPHORE); if (!o) { w32_set_last_error(c, 6); RET(0, 3); }
    Sync *s = o->p; pthread_mutex_lock(&s->m);
    int prev = s->count, ok = (uint32_t)prev + ARG(1) <= s->owner;
    if (ok) { s->count += (int)ARG(1); pthread_cond_broadcast(&s->cv); }
    pthread_mutex_unlock(&s->m);
    if (ARG(2)) rt_w32(G_MEM, ARG(2), (uint32_t)prev);
    if (!ok) w32_set_last_error(c, 298);                   /* ERROR_TOO_MANY_POSTS */
    RET(ok, 3);
}
IMPL(kernel32, OpenMutexA)
{
    uint32_t name = ARG(2);
    for (int k = 0; k < nnamed; k++) if (!strcmp(named[k]->name, GS(name))) { htab[(named_h[k] - 0x100) / 4].refs++; RET(named_h[k], 3); }
    w32_set_last_error(c, 2); RET(0, 3);
}
IMPL(kernel32, SetEvent)
{
    HObj *o = h_get(ARG(0), H_EVENT); if (!o) RET(0, 1);
    Sync *s = o->p; pthread_mutex_lock(&s->m); s->signaled = 1; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->m);
    RET(1, 1);
}
IMPL(kernel32, ResetEvent)
{
    HObj *o = h_get(ARG(0), H_EVENT); if (!o) RET(0, 1);
    Sync *s = o->p; pthread_mutex_lock(&s->m); s->signaled = 0; pthread_mutex_unlock(&s->m);
    RET(1, 1);
}
IMPL(kernel32, ReleaseMutex)
{
    HObj *o = h_get(ARG(0), H_MUTEX); if (!o) RET(0, 1);
    Sync *s = o->p; pthread_mutex_lock(&s->m);
    int ok = s->owner == w32_tid(c) && s->count > 0;
    if (ok && --s->count == 0) { s->owner = 0; pthread_cond_broadcast(&s->cv); }
    pthread_mutex_unlock(&s->m);
    RET(ok, 1);
}
/* 0 = acquired, 0x102 = timeout. Thread handles are signalled when the thread ends (threads.c). */
int w32_wait_one(Ctx *c, uint32_t h, uint32_t ms)
{
    HObj *o = h_get(h, 0); if (!o) return -1;
    if (o->type != H_EVENT && o->type != H_MUTEX && o->type != H_THREAD && o->type != H_SEMAPHORE) return -1;
    Sync *s = o->p; uint32_t tid = w32_tid(c); int type = o->type;   /* o may be closed (and its slot reused) while we wait */
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000; dl.tv_nsec += (long)(ms % 1000) * 1000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
    void w32_crash_wait_begin(int, uint32_t, uint32_t), w32_crash_wait_end(void);
    if (ms) w32_crash_wait_begin(1, h, ms);
    pthread_mutex_lock(&s->m); s->waiters++;
    int r = 0;
    for (;;) {
        if (type == H_MUTEX && (s->count == 0 || s->owner == tid)) { s->owner = tid; s->count++; break; }
        if (type == H_SEMAPHORE && s->count > 0) { s->count--; break; }
        if ((type == H_EVENT || type == H_THREAD) && s->signaled) { if (type == H_EVENT && !s->manual) s->signaled = 0; break; }
        if (w32_deterministic && ms != 0) { s->waiters--; pthread_mutex_unlock(&s->m); rt_unhandled(c, rt_r32(G_MEM, c->esp), "wait would block on another thread (harness)"); }
        if (ms == 0 || (ms != 0xffffffffu && pthread_cond_timedwait(&s->cv, &s->m, &dl))) { r = 0x102; break; }
        if (ms == 0xffffffffu) pthread_cond_wait(&s->cv, &s->m);
    }
    int last = --s->waiters == 0 && s->closed;
    pthread_mutex_unlock(&s->m);
    if (last) { pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->cv); free(s); }
    if (ms) w32_crash_wait_end();
    return r;
}
IMPL(kernel32, WaitForSingleObject)
{
    int r = w32_wait_one(c, ARG(0), ARG(1));
    if (r < 0) { w32_set_last_error(c, 6); RET(0xffffffffu, 2); }
    RET((uint32_t)r, 2);
}
IMPL(kernel32, WaitForMultipleObjects)
{
    uint32_t n = ARG(0), hs = ARG(1), all = ARG(2), ms = ARG(3);
    if (all) { for (uint32_t k = 0; k < n; k++) if (w32_wait_one(c, rt_r32(G_MEM, hs + 4 * k), ms) == 0x102) RET(0x102, 4); RET(0, 4); }
    for (;;) {                                   /* any: poll (rarely used by this game) */
        for (uint32_t k = 0; k < n; k++) if (w32_wait_one(c, rt_r32(G_MEM, hs + 4 * k), 0) == 0) RET(k, 4);
        if (ms == 0) RET(0x102, 4);
        usleep(1000); if (ms != 0xffffffffu && --ms == 0) RET(0x102, 4);
    }
}
IMPL(kernel32, CloseHandle) { RET(h_close(ARG(0)) || ARG(0) == 0xffffffffu || ARG(0) == 0xfffffffeu, 1); }
__attribute__((weak)) void w32_file_closed(HObj *o) { (void)o; }
void w32_handle_closed(HObj *o)
{
    if (o->type == H_THREAD || o->type == H_PROCESS) return;       /* thread objects live as long as the process */
    if (o->type == H_EVENT || o->type == H_MUTEX || o->type == H_SEMAPHORE) {
        Sync *s = o->p; for (int k = 0; k < nnamed; k++) if (named[k] == s) { named[k] = named[--nnamed]; named_h[k] = named_h[nnamed]; }
        /* Windows keeps an object alive while threads wait on it (they go on waiting); destroying a condition variable
         * with waiters blocks forever in glibc (Fallout NV closes its worker threads' event on quit): the last waiter frees */
        pthread_mutex_lock(&s->m); int busy = s->waiters > 0; s->closed = 1; pthread_mutex_unlock(&s->m);
        if (!busy) { pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->cv); free(s); }
    } else w32_file_closed(o);
}

/* ---- process control, misc ---- */
IMPL(kernel32, ExitProcess) { w32_exit(c, ARG(0)); }
IMPL(kernel32, TerminateProcess) { if (ARG(0) == 0xffffffffu) w32_exit(c, ARG(1)); RET(0, 2); }
static uint32_t unhandled_filter;
IMPL(kernel32, SetUnhandledExceptionFilter) { uint32_t o = unhandled_filter; unhandled_filter = ARG(0); RET(o, 1); }
uint32_t w32_unhandled_filter(void) { return unhandled_filter; }
IMPL(kernel32, UnhandledExceptionFilter)
{
    /* called from the C runtime's outermost __except filter: run the game's top-level filter */
    if (unhandled_filter) { uint32_t a = ARG(0), r = w32_callback(c, unhandled_filter, 1, &a); if (r == 0xffffffffu) RET(r, 1); }
    uint32_t rec = rt_r32(G_MEM, ARG(0));
    fprintf(stderr, "recomp: unhandled exception %08x at %08x (info %08x %08x), thread %x\n", rt_r32(G_MEM, rec), rt_r32(G_MEM, rec + 12),
            rt_r32(G_MEM, rec + 20), rt_r32(G_MEM, rec + 24), w32_tid(c));
    {   /* the game is about to stop: leave a crash report with every thread's stack */
        extern const char *w32_crash_report(const char *kind, const char *reason, Ctx *c, uint32_t guest_pc);
        char why[160]; snprintf(why, sizeof why, "unhandled exception %08x (info %08x %08x)", rt_r32(G_MEM, rec), rt_r32(G_MEM, rec + 20), rt_r32(G_MEM, rec + 24));
        const char *path = w32_crash_report("crash", why, c, rt_r32(G_MEM, rec + 12));
        if (path) fprintf(stderr, "recomp: crash report %s\n", path);
    }
    RET(1, 1);
}
IMPL(kernel32, IsDebuggerPresent) { RET(0, 0); }
IMPL(kernel32, OutputDebugStringA) { if (getenv("W32_DEBUGSTRINGS")) fprintf(stderr, "[game] %s", GS(ARG(0))); RET(0, 1); }
IMPL(kernel32, DebugBreak) { rt_unhandled(c, rt_r32(G_MEM, c->esp), "DebugBreak"); }
/* this Mac's name as Windows gives one (a NetBIOS name: at most 15 characters, upper case): the game tells the machines
 * in a multiplayer game apart by it, so it must differ between Macs. DS_COMPUTERNAME overrides it (two copies on one
 * Mac, for tests). */
void w32_computer_name(char *out, size_t cap)
{
    const char *e = getenv("DS_COMPUTERNAME"); char h[256] = "";
    if (e && *e) snprintf(h, sizeof h, "%s", e); else gethostname(h, sizeof h);
    char *dot = strchr(h, '.'); if (dot) *dot = 0;
    size_t n = 0;
    for (const char *p = h; *p && n < 15 && n + 1 < cap; p++) {
        char ch = *p; if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-') out[n++] = ch;
    }
    if (!n) n = (size_t)snprintf(out, cap, "MAC"); else out[n] = 0;
}
IMPL(kernel32, GetComputerNameA)
{
    uint32_t buf = ARG(0), szp = ARG(1); char name[32]; w32_computer_name(name, sizeof name);
    uint32_t len = (uint32_t)strlen(name);
    if (rt_r32(G_MEM, szp) < len + 1) { rt_w32(G_MEM, szp, len + 1); w32_set_last_error(c, 111); RET(0, 2); }
    strcpy((char *)GP(buf), name); rt_w32(G_MEM, szp, len); RET(1, 2);
}
IMPL(advapi32, GetUserNameA)
{
    uint32_t buf = ARG(0), szp = ARG(1);
    const char *u = getenv("DS_USERNAME"); if (!u || !*u || strlen(u) > 63) u = "player";
    uint32_t len = (uint32_t)strlen(u);
    if (rt_r32(G_MEM, szp) < len + 1) { rt_w32(G_MEM, szp, len + 1); w32_set_last_error(c, 122); RET(0, 2); }
    strcpy((char *)GP(buf), u); rt_w32(G_MEM, szp, len + 1); RET(1, 2);
}
IMPL(kernel32, SetPriorityClass) { RET(1, 2); }
IMPL(kernel32, GetPriorityClass) { RET(0x20, 1); }
IMPL(kernel32, GetThreadPriority) { RET(0, 1); }

/* ---- thread context (the game's error reporter captures its own context) ---- */
IMPL(kernel32, GetThreadContext)
{
    uint32_t p = ARG(1), fl = rt_r32(G_MEM, p);
    if (fl & 1) {                                                        /* CONTEXT_CONTROL */
        rt_w32(G_MEM, p + 0xb4, c->ebp); rt_w32(G_MEM, p + 0xb8, rt_r32(G_MEM, c->esp)); rt_w32(G_MEM, p + 0xbc, 0x1b);
        rt_w32(G_MEM, p + 0xc0, flags_get(&c->f)); rt_w32(G_MEM, p + 0xc4, c->esp + 4 + 8); rt_w32(G_MEM, p + 0xc8, 0x23);
    }
    if (fl & 2) {                                                        /* CONTEXT_INTEGER */
        rt_w32(G_MEM, p + 0x9c, c->edi); rt_w32(G_MEM, p + 0xa0, c->esi); rt_w32(G_MEM, p + 0xa4, c->ebx);
        rt_w32(G_MEM, p + 0xa8, c->edx); rt_w32(G_MEM, p + 0xac, c->ecx); rt_w32(G_MEM, p + 0xb0, c->eax);
    }
    if (fl & 4) { rt_w32(G_MEM, p + 0x8c, 0); rt_w32(G_MEM, p + 0x90, 0x3b); rt_w32(G_MEM, p + 0x94, 0x23); rt_w32(G_MEM, p + 0x98, 0x23); }
    RET(1, 2);
}

/* ---- shell folders ---- */
IMPL(shfolder, SHGetFolderPathA)
{
    uint32_t csidl = ARG(1) & 0xff, create = ARG(1) & 0x8000, out = ARG(4); const char *p;
    switch (csidl) {
    case 0x05: p = "C:\\Users\\player\\Documents"; break;                    /* CSIDL_PERSONAL */
    case 0x1a: p = "C:\\Users\\player\\AppData\\Roaming"; break;           /* CSIDL_APPDATA */
    case 0x1c: p = "C:\\Users\\player\\AppData\\Local"; break;             /* CSIDL_LOCAL_APPDATA */
    case 0x23: p = "C:\\ProgramData"; break;                                  /* CSIDL_COMMON_APPDATA */
    case 0x2e: p = "C:\\Users\\Public\\Documents"; break;                    /* CSIDL_COMMON_DOCUMENTS */
    case 0x26: p = "C:\\Program Files"; break;
    case 0x24: p = "C:\\Windows"; break;
    case 0x25: p = "C:\\Windows\\system32"; break;
    default: fprintf(stderr, "w32: SHGetFolderPathA(csidl %#x) unknown\n", csidl); RET(0x80070057u, 5);
    }
    strcpy((char *)GP(out), p);
    if (create) {                                            /* create every level below C:\ */
        char host[2048], part[512]; size_t n = 0;
        for (const char *s = p; ; s++) {
            if (*s == '\\' || !*s) { part[n] = 0; if (n > 3 && !w32_host_path(part, host, sizeof host, 1)) mkdir(host, 0755); }
            if (!*s) break;
            if (n < sizeof part - 1) part[n++] = *s;
        }
    }
    RET(0, 5);
}
IMPL(kernel32, GetConsoleTitleA) { RET(0, 2); }
IMPL(kernel32, SetConsoleTitleA) { RET(1, 1); }

/* ---- thread objects: one per guest thread (TEB), signalled when the thread ends ---- */
typedef struct ThreadObj { Sync s; uint32_t teb, tid, exit_code, suspended; pthread_t pt; } ThreadObj;
static ThreadObj *thread_objs[MAX_THREADS];
ThreadObj *w32_thread_obj(uint32_t teb)
{
    uint32_t k = (teb - TEB_BASE) / 0x2000;
    if (k >= MAX_THREADS) return 0;
    if (!thread_objs[k]) {
        ThreadObj *t = calloc(1, sizeof *t); pthread_mutex_init(&t->s.m, 0); pthread_cond_init(&t->s.cv, 0);
        t->s.manual = 1; t->teb = teb; t->tid = rt_r32(G_MEM, teb + 0x24); t->exit_code = 0x103;   /* STILL_ACTIVE */
        thread_objs[k] = t;
    }
    return thread_objs[k];
}
IMPL(kernel32, DuplicateHandle)
{
    uint32_t h = ARG(1), out = ARG(3), r = 0;
    if (h == 0xfffffffeu) r = h_new(H_THREAD, w32_thread_obj(c->fs_base));
    else if (h == 0xffffffffu) r = h_new(H_PROCESS, 0);
    else { HObj *o = h_get(h, 0); if (!o) { w32_set_last_error(c, 6); RET(0, 7); } o->refs++; r = h; }
    if (out) rt_w32(G_MEM, out, r);
    if (ARG(6) & 1) h_close(h);                                  /* DUPLICATE_CLOSE_SOURCE */
    RET(1, 7);
}
void w32_now_systemtime(uint32_t st) { ft_to_st(filetime_now() + (uint64_t)tz_offset_ft(), st); }
/* for other modules (WINMM event callbacks) */
int w32_event_set(uint32_t h)
{
    HObj *o = h_get(h, H_EVENT); if (!o) return 0;
    Sync *s = o->p; pthread_mutex_lock(&s->m); s->signaled = 1; pthread_cond_broadcast(&s->cv); pthread_mutex_unlock(&s->m);
    return 1;
}
