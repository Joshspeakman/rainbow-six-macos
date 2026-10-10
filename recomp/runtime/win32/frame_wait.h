/* A short final spin keeps frame deadlines precise without burning a core for the last 2 ms.
 * The deadline and clock are in microseconds. Short sleeps avoid large timer-coalescing delays. */
#ifndef DS_FRAME_WAIT_H
#define DS_FRAME_WAIT_H
#include <stdint.h>
#include <unistd.h>
static inline void ds_frame_wait_until(uint64_t deadline, uint64_t (*clock_us)(void))
{
    uint64_t now;
    while ((now = clock_us()) < deadline) {
        uint64_t left = deadline - now;
        if (left > 250) usleep((useconds_t)(left > 1200 ? 1000 : left - 200));
    }
}
#endif
