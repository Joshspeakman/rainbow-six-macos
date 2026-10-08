/* Structured exception handling: guest access violations (host faults on guest pages that are reserved, free or
 * protected) and RaiseException are dispatched like Windows does: through the thread's SEH chain (fs:[0]), then
 * UnhandledExceptionFilter and the game's top-level filter. The game relies on this: archive views are reserved
 * PAGE_NOACCESS and filled by its exception handler on first touch, which then continues execution.
 * Supported outcomes: ExceptionContinueExecution (the faulting access is retried) and continue-search. "Execute the
 * __except block" needs a non-local jump into recompiled code and is reported as fatal for now. */
#include "w32.h"
#include <signal.h>
#include <sys/ucontext.h>
#include <setjmp.h>
#include <dlfcn.h>
#include "platform.h"

__thread Ctx *w32_cur_ctx;                     /* the guest context of this thread (set by thread entry points) */
static __thread uint32_t exc_stack;            /* per-thread guest stack for running exception handlers */
static __thread int exc_depth;
int w32_protect_memory;
extern uint32_t w32_unhandled_filter(void);

enum { EXCEPTION_ACCESS_VIOLATION = 0xC0000005u, EH_UNWINDING = 2 };
int vm_page_state(uint32_t);
static const char *where(uint32_t code) { return code == EXCEPTION_ACCESS_VIOLATION ? "access violation" : "exception"; }

/* Build EXCEPTION_RECORD (80 bytes) + CONTEXT (716 bytes) on the exception stack; returns the EXCEPTION_POINTERS address */
static uint32_t build(Ctx *c, uint32_t sp, uint32_t code, uint32_t flags, uint32_t addr, int n, const uint32_t *info, uint32_t *rec_out, uint32_t *ctx_out)
{
    uint32_t rec = sp - 0x400, cx = rec + 0x60, ptrs = cx + 0x2d0;
    memset(GP(rec), 0, 0x400 - 0x10);
    rt_w32(G_MEM, rec, code); rt_w32(G_MEM, rec + 4, flags); rt_w32(G_MEM, rec + 12, addr); rt_w32(G_MEM, rec + 16, (uint32_t)n);
    for (int k = 0; k < n && k < 15; k++) rt_w32(G_MEM, rec + 20 + 4 * (uint32_t)k, info[k]);
    rt_w32(G_MEM, cx, 0x1003f);                                                 /* CONTEXT_ALL */
    rt_w32(G_MEM, cx + 0x9c, c->edi); rt_w32(G_MEM, cx + 0xa0, c->esi); rt_w32(G_MEM, cx + 0xa4, c->ebx);
    rt_w32(G_MEM, cx + 0xa8, c->edx); rt_w32(G_MEM, cx + 0xac, c->ecx); rt_w32(G_MEM, cx + 0xb0, c->eax);
    rt_w32(G_MEM, cx + 0xb4, c->ebp); rt_w32(G_MEM, cx + 0xb8, addr); rt_w32(G_MEM, cx + 0xbc, 0x1b);
    rt_w32(G_MEM, cx + 0xc0, 0x202); rt_w32(G_MEM, cx + 0xc4, c->esp); rt_w32(G_MEM, cx + 0xc8, 0x23);
    rt_w32(G_MEM, cx + 0x90, 0x3b); rt_w32(G_MEM, cx + 0x94, 0x23); rt_w32(G_MEM, cx + 0x98, 0x23);
    rt_w32(G_MEM, ptrs, rec); rt_w32(G_MEM, ptrs + 4, cx);
    *rec_out = rec; *ctx_out = cx;
    return ptrs;
}
static uint32_t ensure_exc_stack(void)
{
    if (!exc_stack) exc_stack = vm_alloc(0, 0x40000, 0x3000, 4);
    return exc_stack + 0x40000 - 64 - (uint32_t)exc_depth * 0x9000;   /* nested exceptions get their own slice */
}
/* A Visual C++ C++-exception frame handler: "mov eax, FuncInfo; jmp __CxxFrameHandler" with a FuncInfo magic number */
static int cxx_frame_handler(uint32_t h)
{
    if (h < 0x1000 || G_MEM[h] != 0xb8 || G_MEM[h + 5] != 0xe9) return 0;
    uint32_t fi = rt_r32(G_MEM, h + 1);
    if (fi < 0x1000 || vm_page_state(fi) != 2) return 0;
    uint32_t magic = rt_r32(G_MEM, fi);
    return magic >= 0x19930520u && magic <= 0x19930522u;
}
/* Returns 1 if execution may continue (a handler returned ExceptionContinueExecution or the top-level filter
 * returned EXCEPTION_CONTINUE_EXECUTION). */
int w32_dispatch(Ctx *c, uint32_t code, uint32_t flags, uint32_t addr, int n, const uint32_t *info)
{
    if (exc_depth >= 6) { fprintf(stderr, "recomp: nested exceptions too deep (%s at %08x)\n", where(code), n > 1 ? info[1] : addr); return 0; }
    uint32_t sp = ensure_exc_stack(); exc_depth++;
    static int seh_log = -1; if (seh_log < 0) seh_log = getenv("W32_SEHLOG") != 0;
    if (seh_log) fprintf(stderr, "recomp: dispatch %08x at %08x info %08x %08x (thread %x, depth %d)\n", code, addr, n > 0 ? info[0] : 0, n > 1 ? info[1] : 0, w32_tid(c), exc_depth);
    if (seh_log) {                                                              /* the chain, with each frame's state word */
        int k = 0;
        for (uint32_t f = rt_r32(G_MEM, c->fs_base); f && f != 0xffffffffu && k < 12; f = rt_r32(G_MEM, f), k++)
            fprintf(stderr, "recomp:   chain %08x handler %08x state %08x%s\n", f, rt_r32(G_MEM, f + 4), rt_r32(G_MEM, f + 8), f < c->esp ? " (below esp)" : "");
        fprintf(stderr, "recomp:   esp %08x ebp %08x\n", c->esp, c->ebp);
    }
    uint32_t rec, cx, ptrs = build(c, sp, code, flags, addr, n, info, &rec, &cx);
    Ctx h = *c; h.esp = rec - 0x40;                                             /* handlers run below the records */
    int ok = 0;
    /* First touch of a reserved page (an archive view the game fills from its exception filter): C++ catch(...) frames
     * are passed over, as if built with /EHs semantics. Dungeon Siege II reads archive views inside catch(...) {
     * cleanup; throw; } blocks, which (with Visual C++ 7.1, where catch(...) also catches access violations) would
     * otherwise turn the page fill into a rethrown exception. */
    extern int vm_page_state(uint32_t);
    int skip_cxx = code == EXCEPTION_ACCESS_VIOLATION && n > 1 && vm_page_state(info[1]) == 1;
    for (uint32_t frame = rt_r32(G_MEM, c->fs_base); frame && frame != 0xffffffffu; frame = rt_r32(G_MEM, frame)) {
        uint32_t handler = rt_r32(G_MEM, frame + 4), a[4] = {rec, frame, cx, 0};
        if (skip_cxx && cxx_frame_handler(handler)) { if (seh_log) fprintf(stderr, "recomp:   frame %08x handler %08x: C++ frame passed over\n", frame, handler); continue; }
        h.esp = rec - 0x40;
        uint32_t r = w32_callback(&h, handler, 4, a);
        if (seh_log) fprintf(stderr, "recomp:   frame %08x handler %08x -> %u\n", frame, handler, r);
        if (r == 0) { ok = 1; break; }                                          /* ExceptionContinueExecution */
        if (r != 1) { fprintf(stderr, "recomp: SEH handler %08x returned %u (unsupported)\n", handler, r); break; }
    }
    if (!ok) {                                                                  /* no frame handled it: top-level filter */
        uint32_t f = w32_unhandled_filter();
        if (f) { uint32_t a = ptrs; h.esp = rec - 0x40; if (w32_callback(&h, f, 1, &a) == 0xffffffffu) ok = 1; }
    }
    if (ok && rt_r32(G_MEM, cx + 0xb8) != addr)
        fprintf(stderr, "recomp: an exception handler changed the instruction pointer (%08x -> %08x): not supported\n", addr, rt_r32(G_MEM, cx + 0xb8));
    exc_depth--;
    return ok;
}

/* ---- host faults ---- */
static struct sigaction old_segv, old_bus;
static void on_fault(int sig, siginfo_t *si, void *uc)
{
    uint8_t *a = (uint8_t *)si->si_addr; Ctx *c = w32_cur_ctx;
    if (c && G_MEM && a >= G_MEM && a < G_MEM + (1ull << 32)) {
        uint32_t ga = (uint32_t)(a - G_MEM);
        #if defined(__linux__) && defined(__x86_64__)
        ucontext_t *u = uc;
        uint32_t write = (u->uc_mcontext.gregs[REG_ERR] >> 1) & 1;
        void *host_pc = (void *)u->uc_mcontext.gregs[REG_RIP];
        /* On x86-64 the return address is on the stack, not in a link register. */
        void *host_lr = 0;
        /* glibc's IFUNC-selected memory routines have no exported symbol name.
         * They are leaf routines: accept the stack return address only when the
         * fault is in libc, then require that return address to be lifted code. */
        Dl_info lib = {0}, memlib = {0};
        if (dladdr(host_pc, &lib) && dladdr((void *)memmove, &memlib) &&
            lib.dli_fbase == memlib.dli_fbase &&
            (!lib.dli_sname || !strncmp(lib.dli_sname, "mem", 3) || !strncmp(lib.dli_sname, "__mem", 5)))
            host_lr = *(void **)u->uc_mcontext.gregs[REG_RSP];
#elif defined(__APPLE__) && defined(__aarch64__)
        struct __darwin_ucontext *u = uc; uint32_t esr = (uint32_t)u->uc_mcontext->__es.__esr;
        uint32_t write = (esr >> 6) & 1;
        void *host_pc = (void *)u->uc_mcontext->__ss.__pc;
        void *host_lr = (void *)u->uc_mcontext->__ss.__lr;
#else
#error Unsupported host fault context
#endif
        /* data abort: WnR */
        uint32_t info[2] = {write, ga};
        {   /* only faults raised by recompiled game code are the game's; anything else is a runtime bug */
            Dl_info di = {0}, dl = {0}; void *pc = host_pc, *lr = host_lr;
            #define LIFTED(d) ((d).dli_sname && (!strncmp((d).dli_sname, "f_", 2) || !strncmp((d).dli_sname, "b_", 2)))
            int ours = dladdr(pc, &di) && LIFTED(di);
            if (!ours && dladdr(lr, &dl) && LIFTED(dl)) ours = 1;              /* memmove/memset called by recompiled code (rep movs/stos) */
            /* a Windows function the runtime implements reading a game buffer that lies in a reserved archive view
             * (D3DX loading a texture straight from the archive): on Windows that fault reaches the game's exception
             * filter too, which fills the page */
            if (!ours && vm_page_state(ga) == 1) ours = 1;
            if (!ours) {
                fprintf(stderr, "recomp: runtime fault: %s of guest %08x in %s (thread %x)\n", write ? "write" : "read", ga,
                        di.dli_sname ? di.dli_sname : "?", w32_tid(c));
                char what[200]; snprintf(what, sizeof what, "%s of x86 address %08x in %s", write ? "write" : "read", ga, di.dli_sname ? di.dli_sname : "?");
                void w32_crash_runtime_fault(Ctx *, const char *); w32_crash_runtime_fault(c, what);
                abort();
            }
        }
        if (getenv("W32_SEHLOG")) {                    /* which recompiled function faulted (f_<guest address>) */
            Dl_info di = {0}; void *pc = host_pc;
            fprintf(stderr, "recomp: fault %s %08x in %s+%#lx host pc %p (thread %x)\n", write ? "write" : "read", ga,
                    dladdr(pc, &di) && di.dli_sname ? di.dli_sname : "?", dladdr(pc, &di) ? (long)((char *)pc - (char *)di.dli_saddr) : 0L, pc, w32_tid(c));
        }
        if (w32_dispatch(c, EXCEPTION_ACCESS_VIOLATION, 0, 0, 2, info)) {
            /* The host page (16 KB on Apple Silicon, 4 KB on x86 Linux) is now accessible as a whole, so the game will never fault on the other guest pages
             * (4 KB) in it: deliver those faults now, as touches would have, so its handler fills them too. */
            extern int vm_page_state(uint32_t);
            for (uint32_t p = ga & ~(ds_page_size() - 1); p < (ga & ~(ds_page_size() - 1)) + ds_page_size(); p += 0x1000) {
                if (p == (ga & ~0xfffu) || vm_page_state(p) != 1) continue;
                uint32_t ni[2] = {0, p};
                w32_dispatch(c, EXCEPTION_ACCESS_VIOLATION, 0, 0, 2, ni);
            }
            return;                                                             /* retry the access */
        }
        fprintf(stderr, "recomp: unhandled access violation (%s %08x) in thread %x\n", write ? "write to" : "read of", ga, w32_tid(c));
        rt_unhandled(c, ga, "unhandled access violation");
    }
    void w32_crash_signal_from_seh(int, siginfo_t *, void *);                 /* not the game's: report, then the default */
    w32_crash_signal_from_seh(sig, si, uc);
}
void w32_seh_init(void)
{
    static int done; if (done) return; done = 1;
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK; sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv); sigaction(SIGBUS, &sa, &old_bus);
}

/* ---- imports ---- */
IMPL(kernel32, RaiseException)
{
    uint32_t code = ARG(0), flags = ARG(1), n = ARG(2), args = ARG(3), info[15] = {0};
    for (uint32_t k = 0; k < n && k < 15; k++) info[k] = rt_r32(G_MEM, args + 4 * k);
    uint32_t ret = rt_r32(G_MEM, c->esp);
    if (w32_dispatch(c, code, flags & 1, ret, (int)(n < 15 ? n : 15), info)) RET(0, 4);
    static char buf[128]; snprintf(buf, sizeof buf, "unhandled exception %08x (raised)", code);
    rt_unhandled(c, ret, buf);
}
/* RtlUnwind(TargetFrame, TargetIp, ExceptionRecord, ReturnValue): call the handlers of the frames above the target
 * with EH_UNWINDING, then pop them. */
IMPL(kernel32, RtlUnwind)
{
    uint32_t target = ARG(0), recp = ARG(2), sp = ensure_exc_stack();
    uint32_t rec = recp ? recp : sp - 0x100;
    if (!recp) { memset(GP(rec), 0, 80); rt_w32(G_MEM, rec, 0xC0000027u); }
    rt_w32(G_MEM, rec + 4, rt_r32(G_MEM, rec + 4) | EH_UNWINDING);
    Ctx h = *c; exc_depth++;
    for (uint32_t frame = rt_r32(G_MEM, c->fs_base); frame && frame != 0xffffffffu && frame != target; ) {
        uint32_t next = rt_r32(G_MEM, frame), a[4] = {rec, frame, 0, 0};
        h.esp = sp - 0x200;
        w32_callback(&h, rt_r32(G_MEM, frame + 4), 4, a);
        frame = next; rt_w32(G_MEM, c->fs_base, frame);
    }
    exc_depth--;
    c->eax = ARG(3); c->esp += 4 + 16;
}

/* an exception was handled by an __except block further up: the dispatch in progress is abandoned */
void w32_seh_abandon(void) { exc_depth = 0; }

/* CPU exceptions raised by recompiled code (illegal instruction, int n, int3): dispatched like hardware exceptions */
int w32_raise_cpu_exception(Ctx *c, uint32_t pc, uint32_t code)
{
    if (getenv("W32_SEHLOG")) fprintf(stderr, "recomp: CPU exception %08x at %08x\n", code, pc);
    uint32_t info[2] = {0, 0};
    return w32_dispatch(c, code, 0, pc, code == 0xc0000005u ? 2 : 0, info);
}
