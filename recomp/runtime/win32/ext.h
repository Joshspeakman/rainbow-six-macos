/* Engine extensions for Legends of Aranna (runtime/win32/ext.c). */
#ifndef EXT_H
#define EXT_H
#include "w32.h"

void ext_add(const char *mangled, void (*fn)(Ctx *));   /* register before ext_install() */
int ext_install(void);                                    /* rebuild the export table FuBi reads */
uint32_t ext_export(const char *mangled);                 /* a base-engine export's address (0 if absent) */
uint32_t ext_thiscall(Ctx *c, uint32_t fn, uint32_t self, int nargs, const uint32_t *args);
double ext_thiscall_f(Ctx *c, uint32_t fn, uint32_t self, int nargs, const uint32_t *args);   /* float/double result */
double ext_call_f(Ctx *c, uint32_t fn, int nargs, const uint32_t *args);

/* inside an implementation: thiscall methods get `this` in ecx and pop their stack arguments (like stdcall);
 * free functions are cdecl */
#define THIS        (c->ecx)
#define ARGF(n)     ({ uint32_t _u = ARG(n); float _f; memcpy(&_f, &_u, 4); _f; })
#define RETF(v, n)  do { FPUSH((double)(v)); c->esp += 4 + 4 * (n); return; } while (0)
#define RETCF(v)    do { FPUSH((double)(v)); c->esp += 4; return; } while (0)
#endif
