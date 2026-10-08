/* Optional, bounded in-memory trace. Never writes files on the frame path. */
#ifndef DS_FRAME_TRACE_H
#define DS_FRAME_TRACE_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct {
    uint64_t ready, present, done;
    uint32_t width, height;
} DSFrameSample;
typedef struct { DSFrameSample *samples; size_t count, capacity; const char *path; } DSFrameTrace;
static void ds_trace_init(DSFrameTrace *t) {
    t->path=getenv("DS_FRAME_TRACE");
    if(t->path && *t->path) {
        t->capacity=262144; t->samples=calloc(t->capacity,sizeof *t->samples);
        if(!t->samples) t->capacity=0;
    }
}
static void ds_trace_add(DSFrameTrace *t,uint64_t ready,uint64_t present,uint64_t done,uint32_t w,uint32_t h) {
    if(t->count<t->capacity) t->samples[t->count++]=(DSFrameSample){ready,present,done,w,h};
}
static void ds_trace_save(DSFrameTrace *t) {
    if(!t->samples) return;
    FILE *f=fopen(t->path,"w");
    if(f) {
        fputs("frame,time_s,interval_ms,submit_to_present_ms,swap_ms,width,height\n",f);
        uint64_t first=t->count?t->samples[0].done:0, previous=first;
        for(size_t i=0;i<t->count;i++) {
            DSFrameSample *s=&t->samples[i];
            fprintf(f,"%zu,%.9f,%.6f,%.6f,%.6f,%u,%u\n",i+1,(s->done-first)/1e9,
                    (s->done-previous)/1e6,s->ready?(s->present-s->ready)/1e6:0,
                    (s->done-s->present)/1e6,s->width,s->height);
            previous=s->done;
        }
        fclose(f); fprintf(stderr,"host: saved %zu frame timings to %s\n",t->count,t->path);
    } else perror(t->path);
    free(t->samples);
}
#endif
