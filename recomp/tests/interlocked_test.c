/* Test the Win32 ABI and contended compare/exchange used by Broken World. */
#include <assert.h>
#include "../runtime/win32/core.c"
static _Alignas(64) uint8_t memory[4096];
uint8_t *G_MEM = memory;

static uint32_t compare(unsigned thread, uint32_t exchange, uint32_t expected)
{
    Ctx c = {.esp = 256 + thread * 64};
    uint32_t sp = c.esp;
    rt_w32(G_MEM, sp + 4, 1024);
    rt_w32(G_MEM, sp + 8, exchange);
    rt_w32(G_MEM, sp + 12, expected);
    imp_kernel32_InterlockedCompareExchange(&c);
    assert(c.esp == sp + 16);
    return c.eax;
}
static void *worker(void *arg)
{
    unsigned thread = (unsigned)(uintptr_t)arg;
    for (int i = 0; i < 10000; i++) {
        uint32_t old = compare(thread, 0, 0), seen;
        while ((seen = compare(thread, old + 1, old)) != old) old = seen;
    }
    return 0;
}
int main(void)
{
    rt_w32(G_MEM, 1024, 17);
    assert(compare(0, 33, 0) == 17 && rt_r32(G_MEM, 1024) == 17);
    assert(compare(0, 33, 17) == 17 && rt_r32(G_MEM, 1024) == 33);
    rt_w32(G_MEM, 1024, 0);
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; i++) assert(!pthread_create(&threads[i], 0, worker, (void *)(uintptr_t)i));
    for (unsigned i = 0; i < 4; i++) assert(!pthread_join(threads[i], 0));
    assert(rt_r32(G_MEM, 1024) == 40000);
    puts("InterlockedCompareExchange: ABI, success, failure and contention passed");
}
