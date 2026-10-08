/* MSVCRT.dll: the few C runtime entry points the game's older DLLs import (The Sims: ijl10.dll, Intel's JPEG library
 * of 1999). The executables link their own C runtime; these are only for DLLs built against the system one. */
#include "w32.h"
#include <math.h>
#include <string.h>

/* data imports: an IAT slot that holds the variable's address, not a function's (loader.c w32_data_import) */
uint32_t w32_data_import(const char *canon_name)
{
    static uint32_t page;
    if (!page) page = vm_alloc(0, 0x1000, 0x3000, 4);                     /* before the process heap exists */
    if (!strcmp(canon_name, "msvcrt!_adjust_fdiv")) return page;          /* int: 0 = no Pentium FDIV workaround */
    return 0;
}

IMPL(msvcrt, malloc) { RETC(heap_alloc(w32_process_heap, 0, ARG(0))); }
IMPL(msvcrt, free) { if (ARG(0)) heap_free(w32_process_heap, ARG(0)); RETC(0); }
IMPL(msvcrt, op_new) { RETC(heap_alloc(w32_process_heap, 0, ARG(0))); }                     /* ??2@YAPAXI@Z */
IMPL(msvcrt, op_delete) { if (ARG(0)) heap_free(w32_process_heap, ARG(0)); RETC(0); }      /* ??3@YAXPAX@Z */
IMPL(msvcrt, strchr) { const char *s = GS(ARG(0)), *p = strchr(s, (int)(ARG(1) & 0xff)); RETC(p ? ARG(0) + (uint32_t)(p - s) : 0); }
IMPL(msvcrt, strrchr) { const char *s = GS(ARG(0)), *p = strrchr(s, (int)(ARG(1) & 0xff)); RETC(p ? ARG(0) + (uint32_t)(p - s) : 0); }
IMPL(msvcrt, exit) { w32_exit(c, ARG(0)); }

/* _initterm(begin, end): each non-null function pointer in [begin, end), in order */
IMPL(msvcrt, _initterm)
{
    uint32_t b = ARG(0), e = ARG(1), ret = rt_r32(G_MEM, c->esp);
    for (uint32_t p = b; p < e; p += 4) { uint32_t fn = rt_r32(G_MEM, p); if (fn) w32_callback(c, fn, 0, 0); }
    (void)ret; RETC(0);
}

/* _ftol: ST(0) popped and converted toward zero to a 64-bit integer in edx:eax */
IMPL(msvcrt, _ftol)
{
    double v = ST(0); FPOP();
    int64_t r = (v != v || fabs(v) >= 9223372036854775808.0) ? INT64_MIN : (int64_t)v;
    c->edx = (uint32_t)((uint64_t)r >> 32); RETC((uint32_t)r);
}

uint32_t w32_format(char *out, size_t cap, const char *f, uint32_t va);
IMPL(msvcrt, sprintf)
{
    char buf[2048]; uint32_t n = w32_format(buf, sizeof buf, GS(ARG(1)), c->esp + 12);
    memcpy(GP(ARG(0)), buf, n + 1); RETC(n);
}
IMPL(msvcrt, _vsnprintf)
{
    char buf[2048]; uint32_t n = w32_format(buf, sizeof buf, GS(ARG(2)), ARG(3)), cap = ARG(1);
    if (n < cap) { memcpy(GP(ARG(0)), buf, n + 1); RETC(n); }
    memcpy(GP(ARG(0)), buf, cap); RETC((uint32_t)-1);
}

/* _except_handler3: ijl10 guards its CPU checks with __try; the recompiled DLLs raise no exceptions there (rt_cpuid
 * reports the CPU), so a frame that is reached is reported and passed on (ExceptionContinueSearch) */
IMPL(msvcrt, _except_handler3)
{
    static int once; if (!once++) fprintf(stderr, "w32: msvcrt _except_handler3 reached (exception %08x): passed on\n", rt_r32(G_MEM, ARG(0)));
    RETC(1);
}
