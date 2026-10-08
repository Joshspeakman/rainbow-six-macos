/* Running the game for real (not the harness): load, main thread context, entry point on a big-stack thread. */
#include "w32.h"
#include "edition.h"
#include <setjmp.h>
extern __thread jmp_buf *fault_jmp; extern __thread uint32_t fault_pc; extern __thread const char *fault_what;
extern __thread jmp_buf *w32_exit_jmp;
extern char w32_drive_c[1024], w32_game_overlay[1024];
static Ctx game_ctx;

int w32_prepare(const char *exe, const char *game_dir, const char *drive_c, const char *overlay)
{
    snprintf(w32_game_dir, sizeof w32_game_dir, "%s", game_dir);
    snprintf(w32_drive_c, sizeof w32_drive_c, "%s", drive_c);
    snprintf(w32_game_overlay, sizeof w32_game_overlay, "%s", overlay ? overlay : "");
    w32_deterministic = 0;
#if defined(FNV_EDITION) || defined(SIMS_EDITION)
    rt_cpuid_sse2 = 1;                         /* an SSE2 CPU: the code was compiled for one (tools/sse.py lifts it) */
#endif
    extern int w32_protect_memory; void w32_seh_init(void);
    w32_protect_memory = getenv("W32_NOPROTECT") ? 0 : 1;
    if (w32_load(exe)) return -1;
    w32_seh_init();
    uint32_t stk = vm_alloc(0, 1u << 20, 0x3000, 4);
    memset(&game_ctx, 0, sizeof game_ctx);
    game_ctx.fs_base = w32_new_thread_teb(stk, stk + (1u << 20));
    game_ctx.esp = stk + (1u << 20) - 16;
    game_ctx.esp -= 4; rt_w32(G_MEM, game_ctx.esp, 0xffeff000u);
    game_ctx.f.eflags = 0x202; game_ctx.fcw = 0x027f; game_ctx.mxcsr = 0x1f80;
    return 0;
}
/* Runs the game's entry point on the calling thread (give it a large stack). Returns the exit code. */
uint32_t w32_run(char *msg, size_t cap)
{
    { void w32_lab_thread(void); w32_lab_thread(); }
    jmp_buf ej, fj; w32_exit_jmp = &ej; fault_jmp = &fj;
    extern __thread Ctx *w32_cur_ctx; w32_cur_ctx = &game_ctx;
    if (setjmp(ej)) { snprintf(msg, cap, "exit %u", w32_exit_code); return w32_exit_code; }
    if (setjmp(fj)) { snprintf(msg, cap, "fault: %s at %08x", fault_what, fault_pc); return 0xc0000005u; }
    { void w32_crash_thread(const char *); w32_crash_thread("game main thread"); }
    { void w32_dbg_on_siginfo(void); w32_dbg_on_siginfo(); }          /* Ctrl-T / kill -INFO: every guest thread's stack */
    { void w32_init_images(Ctx *); w32_init_images(&game_ctx); }       /* DllMain of recompiled DLLs, then the game */
    rt_call(&game_ctx, w32_entry);
    snprintf(msg, cap, "returned %u", game_ctx.eax);
    return game_ctx.eax;
}
