/* DSR command ring between the game's DirectDraw layer (producer) and the Metal renderer thread (consumer).
 * Layout: header, then a ring of DSR_RING_SIZE bytes of command records (dsr_proto.h).
 * Positions are free-running 32-bit byte counts (the ring size divides 2^32); ring offset = pos % DSR_RING_SIZE. A record never wraps:
 * if it doesn't fit before the end, the producer writes a DSR_WRAP header (or nothing, if < 8 bytes remain) and
 * continues at offset 0. All cross-process fields are naturally aligned and accessed with acquire/release semantics. */
#ifndef DSR_SHM_H
#define DSR_SHM_H
#include <stdint.h>
#define DSR_SHM_MAGIC   0x31525344u   /* "DSR1" */
#define DSR_RING_SIZE   (64u << 20)
#define DSR_HEADER_SIZE 16384u        /* keeps the ring and readback area 16 KB (arm64 page) aligned */
#define DSR_READBACK_SIZE (64u << 20)  /* GPU->CPU readbacks (Lock of a rendered surface) land here: a full screen at 4K and more */
#define DSR_READBACK_OFFSET (DSR_HEADER_SIZE + DSR_RING_SIZE)
#define DSR_SHM_SIZE    (DSR_HEADER_SIZE + DSR_RING_SIZE + DSR_READBACK_SIZE)
#define DSR_WRAP        0xffffu       /* record op: skip to the start of the ring */
#define DSR_QUERY_SLOTS 1024u         /* occlusion query results, in the header after the struct: u32 count[slots], then
                                         u32 serial[slots] (the serial of the DSR_QUERY end the count belongs to) */
#define DSR_QUERY_OFFSET 8192u
typedef struct {
    uint32_t magic, version;
    volatile uint32_t producer_pid;    /* set by ddraw.dll when it attaches (Windows pid) */
    volatile uint32_t host_ready;      /* the renderer: 1 once it is consuming */
    volatile uint32_t write_pos;       /* producer: bytes published */
    uint8_t pad0[64 - 20];
    volatile uint32_t read_pos;        /* consumer: bytes fully consumed (space may be reused) */
    volatile uint32_t frames_submitted;/* producer: PRESENT records published */
    volatile uint32_t frames_done;     /* consumer: presents whose GPU work completed */
    volatile uint32_t context_id;      /* the renderer: CAContext id of the presentation layer (0 = none yet) */
    volatile uint32_t host_alive;      /* the renderer: heartbeat counter */
    volatile uint32_t producer_alive;  /* ddraw.dll: heartbeat counter (per frame) */
    volatile uint32_t quit;            /* producer sets 1 on process detach */
    uint8_t pad1[64 - 28];
    volatile float view_w, view_h;     /* injector: window content size in points */
    volatile uint32_t view_seq;        /* injector: bumped when view_w/h change */
    uint8_t pad2[64 - 12];
    volatile uint32_t readback_done;   /* the renderer: cookie of the last completed DSR_READBACK (pixels in the readback area, rows tightly packed) */
    volatile int32_t  phase_nudge_us;  /* the renderer -> ddraw: shift the game's cadence deadlines by this much (consumed and
                                          zeroed by ddraw) to keep frame completion mid-way between display refreshes */
    volatile uint32_t cursor_hide;     /* injector: Wine has hidden the pointer and the game is the active app. macOS only
                                          hides the pointer over surfaces of the process that hid it, and the game's pixels
                                          come from the renderer's layer, so the renderer mirrors this (see the renderer.m). */
    volatile uint32_t consumer_waiting; /* Linux: wake the renderer on new commands */
} DsrShmHeader;
#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
static inline void dsr_wake(volatile uint32_t *p) {
    syscall(SYS_futex,p,FUTEX_WAKE_PRIVATE,1,0,0,0);
}
static inline void dsr_wait(volatile uint32_t *p,uint32_t expected,long ns) {
    struct timespec timeout={ns/1000000000,ns%1000000000};
    syscall(SYS_futex,p,FUTEX_WAIT_PRIVATE,expected,&timeout,0,0);
}
#endif
#endif
