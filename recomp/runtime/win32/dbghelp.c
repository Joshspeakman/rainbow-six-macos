/* DBGHELP: the engine's function-binding system (FuBi) demangles the exe's exported C++ names with
 * UnDecorateSymbolName. Symbol and stack-walk functions (crash reports only) report failure. */
#include "w32.h"
IMPL(dbghelp, UnDecorateSymbolName)
{
    uint32_t name = ARG(0), out = ARG(1), cap = ARG(2), flags = ARG(3);
    static FILE *log; if (!log && getenv("W32_UNDNAME_LOG")) log = fopen(getenv("W32_UNDNAME_LOG"), "w");
    if (log) { fprintf(log, "%08x %s\n", flags, GS(name)); fflush(log); }
    (void)flags; extern char *w32_undname(const char *);
    if (getenv("W32_TEST_NO_UNDNAME")) { if (cap) rt_w8(G_MEM, out, 0); RET(0, 4); }   /* test switch: provoke the game's error path */
    char *r = w32_undname(GS(name)); const char *s = r ? r : GS(name);   /* not understood: dbghelp returns the input */
    uint32_t n = (uint32_t)strlen(s);
    if (cap) { if (n >= cap) n = cap - 1; memcpy(GP(out), s, n); rt_w8(G_MEM, out + n, 0); }
    free(r);
    RET(n, 4);
}
IMPL(dbghelp, SymInitialize) { w32_set_last_error(c, 50); RET(0, 3); }
IMPL(dbghelp, SymCleanup) { RET(1, 1); }
IMPL(dbghelp, SymGetOptions) { RET(0, 0); }
IMPL(dbghelp, SymSetOptions) { RET(ARG(0), 1); }
IMPL(dbghelp, MiniDumpWriteDump) { RET(0, 7); }
IMPL(dbghelp, SymLoadModule) { RET(0, 6); }
IMPL(dbghelp, SymGetLineFromAddr) { RET(0, 4); }
IMPL(dbghelp, SymGetSymFromAddr) { RET(0, 4); }
IMPL(dbghelp, SymGetModuleInfo) { RET(0, 3); }
IMPL(dbghelp, StackWalk) { RET(0, 9); }
IMPL(dbghelp, SymGetModuleBase) { RET(0, 2); }
IMPL(dbghelp, SymFunctionTableAccess) { RET(0, 2); }
/* the 64-bit-address versions Dungeon Siege II looks up (DWORD64 arguments take two stack slots); stack walking and
 * symbols report nothing, as for DS1 */
IMPL(dbghelp, StackWalk64) { RET(0, 9); }
IMPL(dbghelp, SymFromAddr) { RET(0, 5); }                          /* (process, DWORD64 address, *displacement, symbol) */
IMPL(dbghelp, SymFunctionTableAccess64) { RET(0, 3); }             /* (process, DWORD64 base) */
IMPL(dbghelp, SymGetLineFromAddr64) { RET(0, 5); }
IMPL(dbghelp, SymGetModuleBase64) { c->edx = 0; RET(0, 3); }       /* DWORD64 result */
IMPL(dbghelp, SymGetModuleInfo64) { RET(0, 4); }
IMPL(dbghelp, SymLoadModule64) { c->edx = 0; RET(0, 7); }          /* (process, file, image, module, DWORD64 base, size) */
