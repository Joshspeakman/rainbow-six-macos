/* Threads: every guest thread gets a guest stack, a TEB (with static TLS) and a host pthread with a large stack for
 * the recompiled code. In harness (deterministic) mode threads are created but never started: the emulator side is
 * single-threaded, so the comparison ends at the first wait that would depend on another thread. */
#include "w32.h"
#include <setjmp.h>
#include <unistd.h>

typedef struct Sync { pthread_mutex_t m; pthread_cond_t cv; int manual, signaled, count; uint32_t owner; char name[64]; int waiters, closed; } Sync;   /* same in core.c and threads.c */
typedef struct ThreadObj { Sync s; uint32_t teb, tid, exit_code, suspended; pthread_t pt; } ThreadObj;
ThreadObj *w32_thread_obj(uint32_t teb);
extern __thread jmp_buf *fault_jmp; extern __thread uint32_t fault_pc; extern __thread const char *fault_what;
static __thread jmp_buf *thread_exit_jmp; static __thread uint32_t thread_exit_code;

typedef struct Start { Ctx c; uint32_t fn; ThreadObj *t; } Start;
static void *thread_main(void *arg)
{
    Start *st = arg; ThreadObj *t = st->t;
    { void w32_crash_thread_at(uint32_t); w32_crash_thread_at(st->fn); }
    pthread_mutex_lock(&t->s.m);
    while (t->suspended) pthread_cond_wait(&t->s.cv, &t->s.m);
    pthread_mutex_unlock(&t->s.m);
    jmp_buf ej, fj; thread_exit_jmp = &ej; fault_jmp = &fj;
    extern __thread Ctx *w32_cur_ctx; w32_cur_ctx = &st->c;
    uint32_t code;
    if (setjmp(ej)) code = thread_exit_code;
    else if (setjmp(fj)) { fprintf(stderr, "recomp: thread %x fault: %s at %08x\n", t->tid, fault_what, fault_pc); code = 0xc0000005u; }
    else { rt_call(&st->c, st->fn); code = st->c.eax; }
    pthread_mutex_lock(&t->s.m);
    t->exit_code = code; t->s.signaled = 1; pthread_cond_broadcast(&t->s.cv);
    pthread_mutex_unlock(&t->s.m);
    free(st);
    return 0;
}
IMPL(kernel32, CreateThread)
{
    uint32_t stack = ARG(1), fn = ARG(2), param = ARG(3), flags = ARG(4), tidp = ARG(5);
    uint32_t size = stack ? (stack + 0xffff) & ~0xffffu : (1u << 20);
    if (size < (256u << 10)) size = 256u << 10;
    uint32_t lo = vm_alloc(0, size, 0x3000, 4);
    if (!lo) { w32_set_last_error(c, 8); RET(0, 6); }
    uint32_t teb = w32_new_thread_teb(lo, lo + size);
    if (!teb) { w32_set_last_error(c, 8); RET(0, 6); }
    ThreadObj *t = w32_thread_obj(teb);
    t->suspended = (flags & 4) || w32_deterministic;            /* CREATE_SUSPENDED; harness: never runs */
    Start *st = calloc(1, sizeof *st); st->fn = fn; st->t = t;
    st->c.fs_base = teb; st->c.fcw = 0x027f; st->c.mxcsr = 0x1f80; st->c.f.eflags = 0x202;
    st->c.esp = lo + size - 16;
    st->c.esp -= 4; rt_w32(G_MEM, st->c.esp, param);
    st->c.esp -= 4; rt_w32(G_MEM, st->c.esp, 0xffeff000u);      /* the start routine returns here (host return) */
    uint32_t h = h_new(H_THREAD, t);
    if (tidp) rt_w32(G_MEM, tidp, t->tid);
    if (!w32_deterministic) {
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_create(&t->pt, &a, thread_main, st); pthread_detach(t->pt);
    }
    RET(h, 6);
}
IMPL(kernel32, ExitThread)
{
    thread_exit_code = ARG(0);
    if (thread_exit_jmp) longjmp(*thread_exit_jmp, 1);
    w32_exit(c, ARG(0));                                         /* the main thread: the process ends */
}
IMPL(kernel32, ResumeThread)
{
    HObj *o = h_get(ARG(0), H_THREAD); if (!o) RET(0xffffffffu, 1);
    ThreadObj *t = o->p; pthread_mutex_lock(&t->s.m);
    uint32_t prev = t->suspended;
    if (t->suspended && !w32_deterministic) { t->suspended--; pthread_cond_broadcast(&t->s.cv); }
    pthread_mutex_unlock(&t->s.m);
    RET(prev, 1);
}
IMPL(kernel32, SuspendThread) { fprintf(stderr, "w32: SuspendThread of a running thread is not supported\n"); RET(0, 1); }
IMPL(kernel32, TerminateThread) { fprintf(stderr, "w32: TerminateThread not supported\n"); RET(0, 2); }
IMPL(kernel32, GetExitCodeThread)
{
    HObj *o = h_get(ARG(0), H_THREAD); if (!o) RET(0, 2);
    ThreadObj *t = o->p; rt_w32(G_MEM, ARG(1), t->exit_code); RET(1, 2);
}

/* A host-side service thread that runs guest code (Miles timer and end-of-sound callbacks): it gets its own TEB, guest
 * stack and exception handling, like a thread the game created. */
typedef struct Svc { void (*fn)(Ctx *, void *); void *arg; Ctx c; } Svc;
static void *svc_main(void *p)
{
    { void w32_crash_thread(const char *); w32_crash_thread("runtime service (audio and timer callbacks)"); }
    Svc *s = p;
    jmp_buf fj; fault_jmp = &fj;
    extern __thread Ctx *w32_cur_ctx; w32_cur_ctx = &s->c;
    if (setjmp(fj)) { fprintf(stderr, "recomp: service thread fault: %s at %08x\n", fault_what, fault_pc); return 0; }
    s->fn(&s->c, s->arg);
    return 0;
}
int w32_spawn_service(void (*fn)(Ctx *, void *), void *arg)
{
    uint32_t size = 1u << 20, lo = vm_alloc(0, size, 0x3000, 4);
    uint32_t teb = lo ? w32_new_thread_teb(lo, lo + size) : 0;
    if (!teb) return -1;
    Svc *s = calloc(1, sizeof *s); s->fn = fn; s->arg = arg;
    s->c.fs_base = teb; s->c.fcw = 0x027f; s->c.mxcsr = 0x1f80; s->c.f.eflags = 0x202; s->c.esp = lo + size - 64;
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t; pthread_create(&t, &a, svc_main, s); pthread_detach(t);
    return 0;
}
