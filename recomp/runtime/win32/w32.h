/* Win32 platform layer for the recompiled game: shared definitions. */
#ifndef W32_H
#define W32_H
#include "../rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

/* stdcall argument n (0-based) of the import being serviced; the return address is at [esp] */
#define ARG(n)     rt_r32(G_MEM, c->esp + 4 + 4 * (n))
#define RET(v, n)  do { c->eax = (uint32_t)(v); c->esp += 4 + 4 * (n); return; } while (0)   /* stdcall */
#define RETC(v)    do { c->eax = (uint32_t)(v); c->esp += 4; return; } while (0)              /* cdecl */
#define GP(a)      ((void *)(G_MEM + (uint32_t)(a)))
#define GS(a)      ((const char *)(G_MEM + (uint32_t)(a)))
#define GW(a)      ((const uint16_t *)(G_MEM + (uint32_t)(a)))
#define IMPL(dll, name) void imp_##dll##_##name(Ctx *c)

/* guest address-space layout */
#define PEB_ADDR      0x00010000u
#define PARAMS_ADDR   0x00011000u     /* command line, module path, environment block (64 KB) */
#define GDT_ADDR      0x0001f000u
#define TEB_BASE      0x00020000u     /* TEB k: TEB_BASE + k * 0x2000 (TEB page, then its TLS pointer array) */
#define MAX_THREADS   64
#define VM_LO         0x01000000u     /* VirtualAlloc arena */
#define VM_HI         0x7ffe0000u
#define THUNK_BASE    0xfff00000u     /* import thunks: THUNK_BASE + 16 * index (never mapped) */
#define MAX_THUNKS    4096

/* loader / process */
extern uint32_t w32_image_base, w32_image_size, w32_entry, w32_tls_index_addr, w32_tls_template, w32_tls_size, w32_tls_zero;
extern char     w32_game_dir[1024];                /* host path of the game folder ("C:\GOG Games\Dungeon Siege") */
extern int      w32_deterministic;                 /* harness: time and ids are deterministic */
int      w32_load(const char *exe_path);           /* map image, fill IAT, PEB, process heap; 0 on success */
uint32_t w32_new_thread_teb(uint32_t stack_lo, uint32_t stack_hi);   /* TEB guest address */
uint32_t w32_thunk_for(const char *dll, const char *name);           /* dynamic import (GetProcAddress) */
uint32_t w32_thunk_register(const char *name, void (*fn)(Ctx *));   /* internal entry point (COM methods) */
void     w32_exit(Ctx *c, uint32_t code) __attribute__((noreturn));
extern volatile int w32_exited;
extern uint32_t w32_exit_code;

/* virtual memory (guest) */
uint32_t vm_alloc(uint32_t addr, uint32_t size, uint32_t type, uint32_t prot);
int      vm_free(uint32_t addr, uint32_t size, uint32_t type);
int      vm_query(uint32_t addr, uint32_t out);                       /* writes MEMORY_BASIC_INFORMATION */
void     vm_register(uint32_t base, uint32_t size, uint32_t type);   /* image, low area */
int      vm_committed(uint32_t addr, uint32_t size);
uint64_t vm_hash(void);

/* heaps */
uint32_t heap_create(uint32_t initial, uint32_t max);
int      heap_destroy(uint32_t h);
uint32_t heap_alloc(uint32_t h, uint32_t flags, uint32_t size);
int      heap_free(uint32_t h, uint32_t p);
uint32_t heap_realloc(uint32_t h, uint32_t flags, uint32_t p, uint32_t size);
uint32_t heap_size(uint32_t h, uint32_t p);
extern uint32_t w32_process_heap;

/* handles */
enum { H_FREE, H_FILE, H_EVENT, H_MUTEX, H_THREAD, H_MAPPING, H_FIND, H_SNAPSHOT, H_PROCESS, H_SEMAPHORE };
typedef struct HObj { int type; int refs; void *p; } HObj;
uint32_t h_new(int type, void *p);
HObj    *h_get(uint32_t h, int type);
int      h_close(uint32_t h);

/* threads */
uint32_t w32_tid(Ctx *c);
void     w32_set_last_error(Ctx *c, uint32_t e);

/* text */
int  w32_mb_to_wide(uint32_t cp, const uint8_t *s, int n, uint16_t *out, int cap);
int  w32_wide_to_mb(uint32_t cp, const uint16_t *s, int n, uint8_t *out, int cap);
int  w32_host_path(const char *win, char *out, size_t cap, int for_create);  /* Windows path -> host path */
uint32_t w32_event_new(int manual, int signaled);     /* a Win32 event the runtime signals (core.c) */
void w32_event_signal(uint32_t h, int on);

/* guest callbacks from inside an import (window procedures, enumerators, thread starts) */
uint32_t w32_callback(Ctx *c, uint32_t fn, int nargs, const uint32_t *args);   /* stdcall; returns eax */
extern void (*w32_callback_hook)(Ctx *c, uint32_t fn);   /* harness: run the guest function elsewhere */

/* tracing (harness) */
void w32_trace_import(Ctx *c, uint32_t index);
#endif
