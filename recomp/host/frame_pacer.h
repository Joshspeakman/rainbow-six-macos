/* One presentation clock; VSync owns pacing when its rate matches the cap. */
#ifndef DS_FRAME_PACER_H
#define DS_FRAME_PACER_H
#include <stdint.h>
#include <math.h>
typedef struct { double period, next; } DSFramePacer;
static void ds_pacer_init(DSFramePacer *p,double fps,double refresh,int vsync) {
    p->period=isfinite(fps) && fps>0 && !(vsync && fps>=refresh-.5)?1e9/fps:0;
    p->next=0;
}
static uint64_t ds_pacer_deadline(DSFramePacer *p,uint64_t now) {
    if(!p->period) return now;
    /* No catch-up bursts after slow frames. Retain fractional nanoseconds. */
    if(!p->next || (double)now>p->next) p->next=(double)now;
    return (uint64_t)p->next;
}
static void ds_pacer_advance(DSFramePacer *p) { p->next+=p->period; }
#endif
