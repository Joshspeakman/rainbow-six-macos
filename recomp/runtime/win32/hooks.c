/* Native features and fixes added to the recompiled game at fixed x86 addresses. tools/lift.py lists them:
 *   HOOKS:     run before the instruction at the address (guest registers spilled to the context), then the original
 *              continues;
 *   OVERRIDES: at a function's first instruction; return 1 when the whole call (including the return) was done here,
 *              0 to run the original.
 * The DS1 port's hooks (draw distance, Skrit messages, shadows, paper doll, expansion) are the model for these. */
#include "w32.h"
#include "edition.h"
#include "ext.h"

static float gf(uint32_t a) { float v; uint32_t u = rt_r32(G_MEM, a); memcpy(&v, &u, 4); return v; }
static void sf(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); rt_w32(G_MEM, a, u); }

/* View distance (DS_DRAW_DISTANCE, percent; the launcher's View Distance): each area's mood limits how far the world
 * is seen with its fog (near, far and the low-detail pair) and, in some, a frustum (width: the field of view; height
 * and depth: distances). The mood reader (0x909d.., MoodSetting from gas) has just read both blocks into its locals;
 * the distances are scaled before they are stored, like the SeeFar mod's longer fog. Moods without a frustum use the
 * world's (100000 deep), so only their fog is moved out. */
static void mood_read(Ctx *c)
{
    static float f = -1; if (f < 0) { const char *e = getenv("DS_DRAW_DISTANCE"); f = e && atof(e) > 100.5 ? (float)(atof(e) / 100.0) : 1.0f; }
    uint32_t bp = c->ebp;
    if (getenv("DS_HOOKLOG")) fprintf(stderr, "hook: mood frustum %d %.1f/%.1f/%.1f fog %d %.1f..%.1f low %.1f..%.1f (x%.2f)\n", G_MEM[bp - 0x184],
                                      gf(bp - 0x180), gf(bp - 0x17c), gf(bp - 0x178), G_MEM[bp - 0x174], gf(bp - 0x170), gf(bp - 0x16c),
                                      gf(bp - 0x168), gf(bp - 0x164), f);
    if (f <= 1.001f) return;
    if (G_MEM[bp - 0x184]) { sf(bp - 0x17c, gf(bp - 0x17c) * f); sf(bp - 0x178, gf(bp - 0x178) * f); }
    if (G_MEM[bp - 0x174]) for (uint32_t o = 0x170; o >= 0x164; o -= 4) sf(bp - o, gf(bp - o) * f);
}

int vm_committed(uint32_t addr, uint32_t size);
static int readable(uint32_t p) { return p >= 0x10000 && vm_committed(p, 1); }
/* printf of a guest format string with guest cdecl arguments starting at stack address ap (%s %d %i %u %x %c %f %%) */
__attribute__((unused)) static void guest_format(char *out, size_t cap, uint32_t fmt, uint32_t ap)
{
    size_t n = 0; const char *f = (const char *)GP(fmt);
    for (; *f && n + 1 < cap; f++) {
        if (*f != '%') { out[n++] = *f; continue; }
        char spec[16] = "%"; int k = 1; f++;
        while (*f && strchr("-+ #0123456789.l", *f) && k < 12) spec[k++] = *f++;
        if (!*f) break;
        spec[k++] = *f; spec[k] = 0; int w = 0;
        switch (*f) {
        case 's': { uint32_t p = rt_r32(G_MEM, ap); ap += 4; w = snprintf(out + n, cap - n, spec, readable(p) ? (const char *)GP(p) : "(?)"); break; }
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'c': w = snprintf(out + n, cap - n, spec, rt_r32(G_MEM, ap)); ap += 4; break;
        case 'f': case 'g': { double v; memcpy(&v, GP(ap), 8); ap += 8; w = snprintf(out + n, cap - n, spec, v); break; }
        case '%': out[n] = '%'; w = 1; break;
        default: w = 0;
        }
        if (w > 0) n += (size_t)w < cap - n ? (size_t)w : cap - n - 1;
    }
    out[n] = 0;
}
int native_hook(Ctx *c, uint32_t addr);   /* native.c */
void rt_hook(Ctx *c, uint32_t addr)
{
    if (native_hook(c, addr)) return;
    switch (addr) {
    case DS2_MOOD_READ: mood_read(c); break;
    default:   /* development trace hooks (tools/lift.py DS_TRACE_HOOKS) */
        {   /* W32_SNAP=<hook address>[:<call number>] W32_SNAP_FILE=<path>: one call's entry state for tests/replay.py */
            static long calls; const char *e = getenv("W32_SNAP");
            if (e && strtoul(e, 0, 16) == addr) {
                const char *k = strchr(e, ':'); long want = k ? atol(k + 1) : 1;   /* :0 = every call (up to 64), file.<n> */
                const char *file = getenv("W32_SNAP_FILE") ? getenv("W32_SNAP_FILE") : "snapshot.bin";
                extern void vm_snapshot(const char *, const Ctx *) __attribute__((weak));   /* vm.c (absent in unit tests) */
                if (!vm_snapshot) return;
                if (++calls == want) vm_snapshot(file, c);
                else if (!want && calls <= 64) { char p[1100]; snprintf(p, sizeof p, "%s.%ld", file, calls); vm_snapshot(p, c); }
            }
        }
        if (getenv("DS_HOOKTRACE")) fprintf(stderr, "hook %08x: eax %08x ecx %08x edx %08x ebx %08x esi %08x edi %08x | ret %08x args %08x %08x %08x %08x\n",
                                            addr, c->eax, c->ecx, c->edx, c->ebx, c->esi, c->edi, rt_r32(G_MEM, c->esp), rt_r32(G_MEM, c->esp + 4),
                                            rt_r32(G_MEM, c->esp + 8), rt_r32(G_MEM, c->esp + 12), rt_r32(G_MEM, c->esp + 16));
        if (getenv("DS_HOOKTRACE") && atoi(getenv("DS_HOOKTRACE")) >= 2)   /* and 80 bytes at the first two stack arguments */
            for (int k = 1; k <= 2; k++) {
                uint32_t p = rt_r32(G_MEM, c->esp + 4 * (uint32_t)k); if (p < 0x10000) continue;
                fprintf(stderr, "   arg%d:", k); for (uint32_t j = 0; j < 80; j++) fprintf(stderr, " %02x", G_MEM[p + j]); fprintf(stderr, "\n");
            }
    }
}

/* UIFrontend::TransitionToMain (0x44c2d3) ends by disabling the main menu's Multiplayer button: the GOG 2.3 release
 * shipped without multiplayer (its online service, GameSpy, is gone). The native build plays LAN and internet games
 * itself (dpnet.c), so that one call (returning to 0x44c37f) is skipped; DS2_NO_MULTIPLAYER=1 keeps the original. */
static int keep_multiplayer_button(Ctx *c)
{
    static int off = -1; if (off < 0) off = getenv("DS2_NO_MULTIPLAYER") != 0;
    if (off || rt_r32(G_MEM, c->esp) != 0x0044c37fu) return 0;
    c->esp += 4;                                          /* thiscall, no arguments: just the return */
    return 1;
}
int native_override(Ctx *c, uint32_t addr);   /* native.c */
int rt_override(Ctx *c, uint32_t addr)
{
    if (native_override(c, addr)) return 1;
    switch (addr) {
#ifdef DS2_BROKEN_WORLD
    case 0x0181d35au:
        /* BW's original allowlist omits the supported GOG base content CRC.
         * That base profile writes this CRC; without it Convert Hero
         * reads the save, then silently hides it. Keep all other CRCs on the
         * original path, as well as the caller's version/format validation.
         * This function returns bool in AL and uses RET 4. */
        if (rt_r32(G_MEM, c->esp + 4) != 0xd4ee4a9fu) return 0;
        c->eax = (c->eax & 0xffffff00u) | 1u;
        c->esp += 8;
        return 1;
#elif defined(SIMS_EDITION)
    case 0x00467020u:
        /* The Sims: crash reporting (crashpad with a Backtrace upload URL) starts crashpad_handler.exe and stops the
         * game when it can't; the port writes its own crash reports. cdecl (url, handler path, database name). */
        c->eax = 0; c->esp += 4;
        return 1;
#elif defined(DF_EDITION)
    case 0xffffffffu: break;
#else
    case 0x007663b0u: return keep_multiplayer_button(c);
#endif
    }
    return 0;
}
