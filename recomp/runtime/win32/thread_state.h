/* A running thread and each handle own references independently of its TEB slot. */
#ifndef W32_THREAD_STATE_H
#define W32_THREAD_STATE_H
#include "w32.h"
typedef struct Sync { pthread_mutex_t m; pthread_cond_t cv; int manual, signaled, count; uint32_t owner; char name[64]; int waiters, closed; } Sync;
typedef struct ThreadObj { Sync s; uint32_t teb, tid, exit_code, suspended; pthread_t pt; int started; unsigned refs; } ThreadObj;
ThreadObj *w32_thread_obj(uint32_t teb);
void w32_thread_retain(void *p);
void w32_thread_release(void *p);
void w32_thread_finish(uint32_t teb, uint32_t code);
#endif
