/* Compare deadline accuracy and thread CPU cost with the previous frame limiter's wait.
 * clang -O2 recomp/tests/frame_wait_test.c -o /tmp/frame_wait_test && /tmp/frame_wait_test
 * The renderer/game workload is deliberately excluded: this measures only the pacing cost. */
#include "../runtime/win32/frame_wait.h"
#include <mach/mach_time.h>
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom / 1000;
}
static uint64_t cpu_ns(void)
{
    struct timespec t; assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static int compare(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void measure(int fps, int legacy)
{
    enum { N = 180 }; uint64_t late[N], start = now_us(), cpu = cpu_ns();
    for (int i = 0; i < N; i++) {
        uint64_t target = start + (uint64_t)(i + 1) * 1000000 / fps;
        if (legacy) {
            uint64_t t;
            while ((t = now_us()) < target) if (target - t > 2000) usleep(1000);
        } else ds_frame_wait_until(target, now_us);
        uint64_t t = now_us(); assert(t >= target); late[i] = t - target;
    }
    double used = (cpu_ns() - cpu) / 1e6;
    qsort(late, N, sizeof *late, compare);
    printf("%s %d fps: CPU %.3f ms/frame; deadline lateness median %llu us, p99 %llu us, max %llu us\n",
        legacy ? "previous" : "new", fps, used / N,
        (unsigned long long)late[N / 2], (unsigned long long)late[N * 99 / 100], (unsigned long long)late[N - 1]);
    /* Scheduling noise can make us late; it must never release early. Report latency rather than
     * encoding a machine-dependent performance threshold as a flaky correctness assertion. */
}
int main(void)
{
    setbuf(stdout, NULL); pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (int fps = 60; fps <= 120; fps *= 2) { measure(fps, 1); measure(fps, 0); }
    return 0;
}
