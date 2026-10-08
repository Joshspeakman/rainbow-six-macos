/* W32_PROFILE=<file>: a sampling profiler for finding where the recompiled game spends its time (no perf needed).
 * SIGPROF fires every millisecond of process CPU time and lands in a running thread; the handler records that thread's
 * id and the interrupted instruction pointer, 16-byte records (u32 tid, u32 ms since start, u64 rip) appended to <file> with
 * write() (async-signal-safe) every 4096 samples. tools/profile.py maps the addresses to function names with nm. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <time.h>

typedef struct { uint32_t tid, ms; uint64_t rip; } Sample;    /* ms: since start; the first record (tid ~0) holds prof_init's address */
static Sample buf[4096];
static _Atomic uint32_t nbuf;
static int fd = -1; static struct timespec t0;

static void on_prof(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    ucontext_t *uc = uc_;
    uint32_t k = nbuf++;
    if (k >= 4096) return;                       /* another thread is flushing: drop this sample */
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    buf[k].tid = (uint32_t)syscall(SYS_gettid); buf[k].ms = (uint32_t)((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
#if defined(__x86_64__)
    buf[k].rip = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    buf[k].rip = (uint64_t)uc->uc_mcontext.pc;
#endif
    if (k == 4095) { ssize_t w = write(fd, buf, sizeof buf); (void)w; nbuf = 0; }
}
__attribute__((constructor)) static void prof_init(void)
{
    const char *path = getenv("W32_PROFILE"); if (!path || !*path) return;
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644); if (fd < 0) return;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    Sample m = {0xffffffffu, 0, (uint64_t)(uintptr_t)&prof_init}; ssize_t w = write(fd, &m, sizeof m); (void)w;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = on_prof; sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    struct itimerval it = {{0, 1000}, {0, 1000}}; setitimer(ITIMER_PROF, &it, 0);
}
