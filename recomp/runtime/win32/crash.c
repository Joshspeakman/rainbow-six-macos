#include "platform.h"
/* Crash and hang reports for the native build.
 *
 * Every thread that runs game or runtime code registers here (name, its last Windows calls, what it is blocked on).
 * A report is a plain text file in <data>/CrashReports, written when
 *   - the game's code faults or hits something the recompiler cannot run (rt.c fault -> rt_fault_hook),
 *   - the runtime or the macOS host crashes (signals, abort, an Objective-C exception),
 *   - the game stops responding: no frame presented and no message pumped for DS_HANG_SECONDS (default 20), or the
 *     macOS UI thread stalls for 10 s. Hang reports are written while the game is still stuck, so they exist even if
 *     the user force-quits; if the game recovers, the file is renamed hang-recovered-*.
 * Each report names the reason, the x86 location, every thread's stack (recompiled functions are f_<x86 address>),
 * locks and waits, recent Windows calls, the Mac, the settings and the end of the log. */
#include "w32.h"
#include "edition.h"
#include <execinfo.h>
#include <signal.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <sys/utsname.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/uio.h>
#include <sys/sysinfo.h>
#endif
#include <sys/stat.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#else
#define SIGINFO SIGUSR2
#endif

typedef struct ThreadInfo {
    int used; pthread_t pt; char name[48]; Ctx **ctxp;
    uint16_t ring[32]; uint32_t rpos;                       /* last Windows calls (import indices) */
    volatile int wkind; volatile uint32_t wobj, wms; volatile uint64_t wsince;   /* current wait */
    void *bt[64]; volatile int nbt, bt_req;
} ThreadInfo;
enum { W_NONE, W_HANDLE, W_CS, W_SLEEP, W_MSG };
static ThreadInfo threads[256]; static pthread_mutex_t tlock = PTHREAD_MUTEX_INITIALIZER;
__thread ThreadInfo *w32_ti; static __thread int is_game_main;
extern __thread Ctx *w32_cur_ctx;
static pthread_key_t tkey;
static char report_dir[1100], log_path[1100], build_id[64], last_report[1300];
static int inited; static volatile int writing; static volatile uint64_t last_written;
static _Atomic uint64_t hb[4], hb_count[4];
static uint64_t t_start;      /* heartbeats: 0 frame, 1 message pump, 2 host UI, 3 shown on screen */

static uint64_t now_ns(void) { return ds_clock_ns(); }   /* stops while the Mac sleeps */
static void on_thread_exit(void *p) { ThreadInfo *t = p; if (t) t->used = 0; }

void w32_crash_thread(const char *name)
{
    if (!strcmp(name, "game main thread")) is_game_main = 1;
    if (w32_ti) { snprintf(w32_ti->name, sizeof w32_ti->name, "%s", name); return; }
    pthread_mutex_lock(&tlock);
    for (int k = 0; k < 256; k++) if (!threads[k].used) {
        ThreadInfo *t = &threads[k]; memset(t, 0, sizeof *t);
        t->pt = pthread_self(); snprintf(t->name, sizeof t->name, "%s", name); t->ctxp = &w32_cur_ctx; t->used = 1;
        w32_ti = t; if (inited) pthread_setspecific(tkey, t);
        break;
    }
    pthread_mutex_unlock(&tlock);
}
void w32_dbg_register(void) { w32_crash_thread("thread"); }        /* older call sites */
static const char *image_of(uint32_t a);
void w32_crash_thread_at(uint32_t start)                          /* a CreateThread thread, named by its code */
{
    char n[48]; const char *img = image_of(start);
    snprintf(n, sizeof n, "thread at x86 %08x (%s)", start, img ? img : "?");
    w32_crash_thread(n);
}
void w32_crash_wait_begin(int kind, uint32_t obj, uint32_t ms)
{
    ThreadInfo *t = w32_ti; if (!t) return;
    t->wobj = obj; t->wms = ms; t->wsince = now_ns(); t->wkind = kind;
}
void w32_crash_wait_end(void) { if (w32_ti) w32_ti->wkind = W_NONE; }
void w32_crash_record_call(uint32_t index) { ThreadInfo *t = w32_ti; if (t) t->ring[t->rpos++ & 31] = (uint16_t)index; }
void w32_crash_heartbeat(int which)
{
    if (which == 1 && !is_game_main) return;                  /* responsiveness = the game's main (window) thread */
    hb[which] = now_ns(); hb_count[which]++;
}

/* ---- safe reads of guest memory (it may be protected) ---- */
static int safe_read(const void *src, void *dst, size_t n)
{
#ifdef __APPLE__
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)src, n, (mach_vm_address_t)dst, &got) == KERN_SUCCESS && got == n;
#else
    struct iovec local = {dst, n}, remote = {(void *)src, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
#endif
}
static int g32(uint32_t a, uint32_t *v) { return G_MEM && safe_read(G_MEM + a, v, 4); }

/* nearest recompiled function at or below a guest address */
struct rt_fn { uint32_t addr; GuestFn fn; };
extern const struct rt_fn rt_fntab[]; extern const unsigned rt_fntab_n;
static uint32_t nearest_fn(uint32_t a)
{
    unsigned lo = 0, hi = rt_fntab_n;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (rt_fntab[m].addr <= a) lo = m + 1; else hi = m; }
    return lo ? rt_fntab[lo - 1].addr : 0;
}
struct rt_image { const char *name; uint32_t base, imp_first, imp_count; };
extern const struct rt_image rt_images[]; extern const unsigned rt_images_n;
static const char *image_of(uint32_t a)
{
    for (unsigned k = 0; k < rt_images_n; k++) {
        uint32_t b = rt_images[k].base, sz = k ? 0x01000000u : DS2_IMAGE_END - b;
        if (a - b < sz) return *rt_images[k].name ? rt_images[k].name : "DungeonSiege.exe";
    }
    return 0;
}
static void describe_guest(char *out, size_t cap, uint32_t a)
{
    const char *img = image_of(a); uint32_t f = img ? nearest_fn(a) : 0;
    if (img && f) snprintf(out, cap, "x86 %08x (%s, in function %08x +%#x)", a, img, f, a - f);
    else snprintf(out, cap, "x86 %08x", a);
}

/* ---- stacks of all threads ---- */
static void on_bt(int sig) { (void)sig; ThreadInfo *t = w32_ti; if (t && t->bt_req) { t->nbt = backtrace(t->bt, 64); t->bt_req = 0; } }
static void collect_stacks(void)
{
    signal(SIGUSR2, on_bt);
    for (int k = 0; k < 256; k++) {
        ThreadInfo *t = &threads[k]; if (!t->used) continue;
        if (t == w32_ti) { t->nbt = backtrace(t->bt, 64); continue; }
        t->nbt = 0; t->bt_req = 1;
        if (pthread_kill(t->pt, SIGUSR2)) t->bt_req = 0;
    }
    for (int spin = 0; spin < 100; spin++) {              /* up to 0.5 s */
        int pending = 0; for (int k = 0; k < 256; k++) if (threads[k].used && threads[k].bt_req) pending = 1;
        if (!pending) break;
        usleep(5000);
    }
}
static void write_stack(int fd, ThreadInfo *t)
{
    int shown = 0;
    for (int i = 0; i < t->nbt; i++) {
        Dl_info di; const char *nm = 0; long off = 0;
        if (dladdr(t->bt[i], &di) && di.dli_sname) { nm = di.dli_sname; off = (long)((char *)t->bt[i] - (char *)di.dli_saddr); }
        if (nm && (!strcmp(nm, "on_bt") || !strcmp(nm, "_sigtramp") || !strcmp(nm, "collect_stacks") || !strcmp(nm, "w32_crash_report") ||
                   !strcmp(nm, "crash_signal") || !strcmp(nm, "rt_fault_hook") || !strcmp(nm, "backtrace"))) continue;
        if (nm && (!strncmp(nm, "f_", 2) || !strncmp(nm, "b_", 2))) {
            uint32_t ga = (uint32_t)strtoul(nm + 2, 0, 16); const char *img = image_of(ga);
            dprintf(fd, "      %-28s  game code: function %08x (%s)\n", nm, ga, img ? img : "?");
        } else dprintf(fd, "      %s+%#lx\n", nm ? nm : "?", off);
        if (++shown >= 40) { dprintf(fd, "      ...\n"); break; }
    }
    if (!t->nbt) dprintf(fd, "      (no stack: the thread did not answer)\n");
}
static void write_wait(int fd, ThreadInfo *t, uint64_t now)
{
    uint64_t since = t->wsince; double secs = since && since < now ? (now - since) / 1e9 : 0;   /* may change under us */
    switch (t->wkind) {
    case W_HANDLE: dprintf(fd, "    waiting on Windows handle %#x for %.1f s (timeout %s)\n", t->wobj, secs, t->wms == 0xffffffffu ? "none" : "set"); break;
    case W_CS: {
        uint32_t owner = 0, count = 0; g32(t->wobj + 12, &owner); g32(t->wobj + 8, &count);
        dprintf(fd, "    waiting for critical section %08x for %.1f s; held by Windows thread %#x (lock count %u)\n", t->wobj, secs, owner, count);
        break; }
    case W_SLEEP: dprintf(fd, "    in Sleep(%u ms)\n", t->wms); break;
    case W_MSG: dprintf(fd, "    waiting for window messages (idle)\n"); break;
    default: dprintf(fd, "    running\n");
    }
}
static void write_calls(int fd, ThreadInfo *t)
{
    uint32_t end = t->rpos, n = end < 32 ? end : 32; if (!n) return;   /* a snapshot: the thread may still be running */
    uint16_t ring[32]; memcpy(ring, t->ring, sizeof ring);
    const char *w32_thunk_name(uint32_t index);
    dprintf(fd, "    last Windows calls (oldest first):");
    const char *prev = 0; int rep = 0, col = 0;
    for (uint32_t k = 0; k <= n; k++) {                   /* runs of the same call are folded: "Sleep x12" */
        const char *nm = k < n ? w32_thunk_name(ring[(end - n + k) & 31]) : 0;
        if (prev && nm && !strcmp(nm, prev)) { rep++; continue; }
        if (prev) { dprintf(fd, "%s %s", col++ % 4 ? "," : "\n      ", prev); if (rep > 1) dprintf(fd, " x%d", rep); }
        prev = nm; rep = 1;
    }
    dprintf(fd, "\n");
}
static void sysstr(const char *name, char *out, size_t cap) {
#ifdef __APPLE__
    size_t n = cap; if (sysctlbyname(name, out, &n, 0, 0)) snprintf(out, cap, "?");
#else
    struct utsname u; uname(&u);
    snprintf(out, cap, "%s", strstr(name, "model") ? u.machine : strstr(name, "product") ? u.sysname : u.release);
#endif
}

/* Writes a report and returns its path. kind: "crash", "hang". */
const char *w32_crash_report(const char *kind, const char *reason, Ctx *c, uint32_t guest_pc)
{
    if (!inited || writing) return 0;
    writing = 1;
    time_t tt = time(0); struct tm tm; localtime_r(&tt, &tm);
    char stamp[32]; strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", &tm);
    snprintf(last_report, sizeof last_report, "%s/%s-%s.txt", report_dir, kind, stamp);
    int fd = open(last_report, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { writing = 0; return 0; }
    collect_stacks();
    uint64_t now = now_ns();
    char when[64]; strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S %Z", &tm);
    int hang = !strcmp(kind, "hang");
    dprintf(fd, "Dungeon Siege Native - %s report\n%s\n\n", hang ? "not responding" : "crash", when);
    dprintf(fd, "WHAT HAPPENED\n  %s\n", reason);
    if (guest_pc) { char d[160]; describe_guest(d, sizeof d, guest_pc); dprintf(fd, "  where: %s\n", d); }
    dprintf(fd, "  %.0f s after launch; %llu frames presented (last %.1f s ago); messages last pumped %.1f s ago\n\n",
            (now - t_start) / 1e9, (unsigned long long)hb_count[0], hb[0] ? (now - hb[0]) / 1e9 : -1.0, hb[1] ? (now - hb[1]) / 1e9 : -1.0);

    char os[64], osb[64], model[64]; uint64_t mem = 0; size_t ms = sizeof mem;
    sysstr("kern.osproductversion", os, sizeof os); sysstr("kern.osversion", osb, sizeof osb); sysstr("hw.model", model, sizeof model);
#ifdef __APPLE__
    sysctlbyname("hw.memsize", &mem, &ms, 0, 0);
#else
    struct sysinfo info; if (!sysinfo(&info)) mem = (uint64_t)info.totalram * info.mem_unit; (void)ms;
#endif
    extern int w32_screen_w, w32_screen_h; extern double w32_screen_scale; void dsr_mode_size(uint32_t *, uint32_t *);
    uint32_t mw = 0, mh = 0; dsr_mode_size(&mw, &mh);
    dprintf(fd, "SYSTEM\n  build %s; host %s (%s); %s, %.0f GB; screen %dx%d @%.0fx; game display mode %ux%u\n",
            build_id, os, osb, model, mem / 1073741824.0, w32_screen_w, w32_screen_h, w32_screen_scale, mw, mh);
    dprintf(fd, "  settings:");
    extern char **environ; int any = 0;
    for (char **e = environ; *e; e++) if (!strncmp(*e, "DS_", 3) || !strncmp(*e, "W32_", 4) || !strncmp(*e, "DSR_", 4)) { dprintf(fd, " %s", *e); any = 1; }
    dprintf(fd, "%s\n\n", any ? "" : " (defaults)");

    if (c) {
        dprintf(fd, "GAME REGISTERS (x86, thread that failed)\n  eax %08x ebx %08x ecx %08x edx %08x esi %08x edi %08x ebp %08x esp %08x\n",
                c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->esp);
        dprintf(fd, "  stack (code addresses annotated):\n");
        for (int i = 0; i < 24; i++) {
            uint32_t v; if (!g32(c->esp + 4u * (uint32_t)i, &v)) { dprintf(fd, "    [esp+%02x] (unreadable)\n", 4 * i); break; }
            char d[160] = ""; if (image_of(v) && nearest_fn(v) && v - nearest_fn(v) < 0x4000) describe_guest(d, sizeof d, v);
            dprintf(fd, "    [esp+%02x] %08x  %s\n", 4 * i, v, d);
        }
        dprintf(fd, "\n");
    }

    dprintf(fd, "THREADS (the one that failed or is stuck is usually the game main thread)\n");
    uint32_t w32_tid(Ctx *);
    for (int k = 0; k < 256; k++) {
        ThreadInfo *t = &threads[k]; if (!t->used) continue;
        Ctx *tc = t->ctxp ? *t->ctxp : 0; uint32_t tid = 0;
        if (tc && tc->fs_base) g32(tc->fs_base + 0x24, &tid);
        dprintf(fd, "  [%s]\n", t->name);
        if (tid) dprintf(fd, "    Windows thread id %#x%s\n", tid, t == w32_ti ? " (this thread wrote the report)" : "");
        write_wait(fd, t, now);
        write_stack(fd, t);
        write_calls(fd, t);
    }
    dprintf(fd, "\nLOADED GAME CODE\n");
    for (unsigned k = 0; k < rt_images_n; k++) dprintf(fd, "  %08x  %s\n", rt_images[k].base, *rt_images[k].name ? rt_images[k].name : "DungeonSiege.exe");

    /* for developers: host addresses, to map onto exact x86 instructions with tools/symbolize-report.sh (atos) */
#ifdef __APPLE__
    const struct mach_header *mh0 = 0; intptr_t slide = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) if (strstr(_dyld_get_image_name(i), "DungeonSiegeNative")) { mh0 = _dyld_get_image_header(i); slide = _dyld_get_image_vmaddr_slide(i); break; }
    dprintf(fd, "\nHOST ADDRESSES (load address %p, slide %#lx)\n", (void *)mh0, (long)slide);
#else
    Dl_info executable = {0}; dladdr((void *)w32_crash_report, &executable);
    dprintf(fd, "\nHOST ADDRESSES (ELF load address %p; subtract this base for addr2line)\n", executable.dli_fbase);
#endif
    for (int k = 0; k < 256; k++) {
        ThreadInfo *t = &threads[k]; if (!t->used || !t->nbt) continue;
        dprintf(fd, "  %s:", t->name); for (int i = 0; i < t->nbt && i < 40; i++) dprintf(fd, " %p", t->bt[i]); dprintf(fd, "\n");
    }

    int lf = open(log_path, O_RDONLY);                       /* the end of the log */
    if (lf >= 0) {
        off_t end = lseek(lf, 0, SEEK_END), from = end > 6000 ? end - 6000 : 0; char buf[6001];
        lseek(lf, from, SEEK_SET); ssize_t n = read(lf, buf, 6000); close(lf);
        if (n > 0) {
            buf[n] = 0; char *p = buf; if (from) { char *nl = strchr(p, '\n'); if (nl) p = nl + 1; }
            dprintf(fd, "\nLOG (end of %s)\n%s\n", log_path, p);
        }
    }
    close(fd);
    fprintf(stderr, "DungeonSiegeNative: %s report written to %s\n", kind, last_report);
    last_written = now_ns(); writing = 0;
    return last_report;
}
const char *w32_crash_last_path(void) { return last_report[0] ? last_report : 0; }

/* rt.c: a fault in game code (called before it is turned into "game ended") */
void rt_fault_hook(Ctx *c, uint32_t pc, const char *what)
{
    if (!inited) return;
    char r[300]; snprintf(r, sizeof r, "The game's code stopped: %s.", what);
    w32_crash_report("crash", r, c, pc);
}

/* ---- signals: runtime or host crashes ---- */
static void crash_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    static volatile int in; if (in++) { signal(sig, SIG_DFL); raise(sig); return; }
    if (now_ns() - last_written > 3000000000ull) {          /* not already explained by a report just written */
        extern __thread const char *fault_what;
        char r[300];
        if (sig == SIGABRT) snprintf(r, sizeof r, "The program stopped itself (abort)%s%s.", fault_what ? ": " : "", fault_what ? fault_what : "");
        else snprintf(r, sizeof r, "The native host crashed: %s at address %p.", strsignal(sig), si ? si->si_addr : 0);
        w32_crash_report("crash", r, w32_cur_ctx, 0);
    }
    signal(sig, SIG_DFL); raise(sig);
}
void w32_crash_signal_from_seh(int sig, siginfo_t *si, void *uc) { crash_signal(sig, si, uc); }   /* seh.c: faults that are not the game's */
void w32_crash_runtime_fault(Ctx *c, const char *what)
{
    char r[300]; snprintf(r, sizeof r, "The runtime (not the game's code) crashed: %s.", what);
    w32_crash_report("crash", r, c, 0);
}

/* ---- the watchdog ---- */
static void *watchdog(void *arg)
{
    (void)arg; w32_crash_thread("crash watchdog");
    const char *e = getenv("DS_HANG_SECONDS"); double limit = e ? atof(e) : 20;
    char hang_path[1300] = ""; uint64_t hang_since = 0; int ui_reported = 0;
    for (;;) {
        sleep(1);
        if (limit <= 0) continue;
        /* Snapshot beats before reading the clock. A concurrently updated
         * beat newer than 'now' previously wrapped unsigned subtraction and
         * launched a costly false hang report while frames were flowing. */
        int started = hb_count[0] > 0;
        uint64_t beat[4]; for(int k=0;k<4;k++) beat[k]=hb[k];
        uint64_t now = now_ns();
        double frame_age = (now - beat[0]) / 1e9, pump_age = (now - beat[1]) / 1e9, ui_age = beat[2] ? (now - beat[2]) / 1e9 : 0;
        int stuck = started && frame_age > limit && pump_age > limit;
        if (stuck && !hang_since) {
            hang_since = beat[0] > beat[1] ? beat[0] : beat[1];            /* the last sign of life */
            char r[300]; snprintf(r, sizeof r, "The game stopped responding: no frame for %.0f s and no window messages handled for %.0f s.", frame_age, pump_age);
            const char *p = w32_crash_report("hang", r, 0, 0); snprintf(hang_path, sizeof hang_path, "%s", p ? p : "");
        } else if (!stuck && hang_since) {
            double secs = (now - hang_since) / 1e9;
            int fd = open(hang_path, O_WRONLY | O_APPEND);
            if (fd >= 0) { dprintf(fd, "\nRECOVERED: the game responded again after about %.0f s.\n", secs); close(fd); }
            char np[1400]; const char *b = strrchr(hang_path, '/');
            if (b) { snprintf(np, sizeof np, "%.*s/hang-recovered-%s", (int)(b - hang_path), hang_path, b + 6); rename(hang_path, np); }
            fprintf(stderr, "DungeonSiegeNative: the game responded again after %.0f s\n", secs);
            hang_since = 0;
        }
        double shown_age = beat[3] ? (now - beat[3]) / 1e9 : 0;     /* the game draws, but nothing reaches the screen */
        static int shown_reported;
        if (started && frame_age < 2 && shown_age > limit && !shown_reported) {
            shown_reported = 1;
            char r[200]; snprintf(r, sizeof r, "The game is running but nothing has been shown on screen for %.0f s (the display path is stuck).", shown_age);
            w32_crash_report("hang", r, 0, 0);
        }
        if (shown_age < 2) shown_reported = 0;
        if (ui_age > 10 && !ui_reported) { ui_reported = 1; w32_crash_report("hang", "The host window (UI thread) stopped responding for 10 s.", 0, 0); }
        if (ui_age < 2) ui_reported = 0;
    }
    return 0;
}

void w32_crash_init(const char *data_dir, const char *build)
{
    snprintf(report_dir, sizeof report_dir, "%s/CrashReports", data_dir); mkdir(report_dir, 0755);
    if (getenv("DS_LOG_FILE")) snprintf(log_path, sizeof log_path, "%s", getenv("DS_LOG_FILE"));
    else {
#ifdef __linux__
    snprintf(log_path, sizeof log_path, "%s/last-run.log", data_dir);
#else
    snprintf(log_path, sizeof log_path, "%s/DungeonSiegeNative.log", data_dir);
#endif
    }
    snprintf(build_id, sizeof build_id, "%s", build ? build : "?");
    pthread_key_create(&tkey, on_thread_exit);
    pthread_mutex_lock(&tlock); for (int k = 0; k < 256; k++) if (threads[k].used && threads[k].pt == pthread_self()) pthread_setspecific(tkey, &threads[k]); pthread_mutex_unlock(&tlock);
    t_start = now_ns(); inited = 1;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = crash_signal; sa.sa_flags = SA_SIGINFO | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, 0); sigaction(SIGILL, &sa, 0); sigaction(SIGFPE, &sa, 0); sigaction(SIGTRAP, &sa, 0);
    pthread_t t; pthread_create(&t, 0, watchdog, 0); pthread_detach(t);
}
/* Ctrl-T / kill -INFO: a report on demand (also printed to the log) */
static void on_info(int sig) { (void)sig; w32_crash_report("snapshot", "Requested (Ctrl-T / SIGINFO): the state of every thread.", 0, 0); }
void w32_dbg_on_siginfo(void) { signal(SIGINFO, on_info); }
void w32_dbg_dump_threads(void) { w32_crash_report("snapshot", "Requested: the state of every thread.", 0, 0); }
