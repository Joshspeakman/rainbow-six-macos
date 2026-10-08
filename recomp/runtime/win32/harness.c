/* Entry points for the start-up differential harness (tests/startup.py) and for running the game. */
#include "w32.h"
#include <setjmp.h>

extern __thread jmp_buf *fault_jmp; extern __thread uint32_t fault_pc; extern __thread const char *fault_what;
extern __thread jmp_buf *w32_exit_jmp;
extern FILE *w32_trace_file;
extern char w32_drive_c[1024], w32_game_overlay[1024];
void w32_trace_close(void) { if (w32_trace_file) fclose(w32_trace_file); w32_trace_file = 0; }

static Ctx main_ctx; static char result[512];
#define MAIN_STACK (1u << 20)

int w32_harness_prepare(const char *exe, const char *game_dir, const char *drive_c, const char *trace, int deterministic, Ctx *out)
{
    snprintf(w32_game_dir, sizeof w32_game_dir, "%s", game_dir);
    snprintf(w32_drive_c, sizeof w32_drive_c, "%s", drive_c);
    snprintf(w32_game_overlay, sizeof w32_game_overlay, "%s/GOG Games/Dungeon Siege", drive_c);   /* game folder stays pristine */
    w32_deterministic = deterministic;
    { extern char dsr_snapshot_path[1024]; const char *e = getenv("DSR_SNAPSHOT");
      snprintf(dsr_snapshot_path, sizeof dsr_snapshot_path, "%s", e ? e : ""); }
    if (w32_load(exe)) return -1;
    if (trace && *trace) w32_trace_file = fopen(trace, "wb");
    uint32_t stk = vm_alloc(0, MAIN_STACK, 0x3000, 4);
    memset(&main_ctx, 0, sizeof main_ctx);
    main_ctx.fs_base = w32_new_thread_teb(stk, stk + MAIN_STACK);
    main_ctx.esp = stk + MAIN_STACK - 16;
    main_ctx.esp -= 4; rt_w32(G_MEM, main_ctx.esp, 0xffeff000u);    /* entry returns here */
    main_ctx.f.eflags = 0x202; main_ctx.fcw = 0x027f; main_ctx.mxcsr = 0x1f80;
    if (out) *out = main_ctx;
    return 0;
}
uint8_t *w32_mem(void) { return G_MEM; }
uint32_t w32_entry_point(void) { return w32_entry; }
const char *w32_result(void) { return result; }

/* Native run of the entry point on a thread with a large host stack. */
static void *run_entry(void *arg)
{
    (void)arg; Ctx *c = &main_ctx;
    jmp_buf ej, fj; w32_exit_jmp = &ej; fault_jmp = &fj;
    if (setjmp(ej)) { snprintf(result, sizeof result, "exit %u", w32_exit_code); return 0; }
    if (setjmp(fj)) { snprintf(result, sizeof result, "fault: %s at %08x", fault_what, fault_pc); return 0; }
    { void w32_init_images(Ctx *); w32_init_images(c); }
    rt_call(c, w32_entry);
    snprintf(result, sizeof result, "returned %u", c->eax);
    return 0;
}
int w32_harness_run_native(void)
{
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
    pthread_t t; pthread_create(&t, &a, run_entry, 0); pthread_join(t, 0);
    w32_trace_close();
    return 0;
}

/* The emulator side: service import `index` for a context built from the emulator's registers.
 * Returns 0 normally, 1 if the process exited, 2 on a fault (message in w32_result()). */
int w32_harness_import(Ctx *c, uint32_t index)
{
    jmp_buf ej, fj; w32_exit_jmp = &ej; fault_jmp = &fj;
    if (setjmp(ej)) { snprintf(result, sizeof result, "exit %u", w32_exit_code); w32_trace_close(); return 1; }
    if (setjmp(fj)) { snprintf(result, sizeof result, "fault: %s at %08x", fault_what, fault_pc); w32_trace_close(); return 2; }
    rt_import(c, index);
    w32_exit_jmp = 0; fault_jmp = 0;
    return 0;
}
void w32_set_callback_hook(void (*hook)(Ctx *, uint32_t)) { w32_callback_hook = hook; }
