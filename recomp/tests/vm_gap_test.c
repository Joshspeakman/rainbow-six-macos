/* Compare address placement with the original exhaustive search, then exercise
 * allocation, fragmentation, commit/decommit and released-memory reuse. */
#include <assert.h>
#include "../runtime/win32/vm.c"

uint8_t *G_MEM;
int w32_protect_memory;
static uint32_t rng = 0x842981adu;
static uint32_t random32(void) { rng ^= rng << 13; rng ^= rng >> 17; return rng ^= rng << 5; }

static uint32_t reference_gap(uint32_t span, int top)
{
    if (!span || span > VM_HI - VM_LO) return 0;
    if (top) {
        for (uint32_t b = (VM_HI - span) & ~0xffffu; b >= VM_LO; b -= 0x10000)
            if (range_free(b, span)) return b;
    } else {
        for (uint32_t b = VM_LO; b + span <= VM_HI; b += 0x10000)
            if (range_free(b, span)) return b;
    }
    return 0;
}
static void reset_regions(void)
{
    for (int k = 0; k < nR; k++) free(R[k].commit);
    nR = 0;
}
static void placement(void)
{
    unsigned checks = 0;
    for (int map = 0; map < 128; map++) {
        reset_regions();
        /* Sorted nonoverlapping page-granular regions, including partial 64 KB
         * granules and reservations outside the automatic allocation arena. */
        uint32_t b = 0x1000;
        while (b < VM_HI + 0x100000u) {
            uint32_t size = (1 + random32() % 2048) * 0x1000;
            insert(b, size, 0x20000, 4);
            b += size + (random32() % 96) * 0x1000;
        }
        for (unsigned j = 0; j < 8; j++) {
            uint32_t span = (1 + random32() % 128) * 0x10000;
            for (int top = 0; top < 2; top++) {
                assert(find_gap(span, top) == reference_gap(span, top));
                checks++;
            }
        }
    }
    reset_regions();
    for (int top = 0; top < 2; top++) {
        assert(find_gap(0, top) == 0);
        assert(find_gap(0xffffffffu, top) == 0);
        assert(find_gap(VM_HI - VM_LO, top) == VM_LO);
    }
    insert(VM_LO, VM_HI - VM_LO, 0x20000, 4);
    assert(!find_gap(0x10000, 0) && !find_gap(0x10000, 1));
    reset_regions();
    insert(VM_LO, 0x1000, 0x20000, 4);
    insert(VM_HI - 0x1000, 0x1000, 0x20000, 4);
    assert(find_gap(0x10000, 0) == VM_LO + 0x10000);
    assert(find_gap(0x10000, 1) == VM_HI - 0x20000);
    reset_regions();
    printf("vm gap: %u fragmented-map placements match exhaustive search\n", checks);
}
static void allocation(void)
{
    G_MEM = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    assert(G_MEM != MAP_FAILED);
    w32_protect_memory = 1;
    /* A low mapped image and a BW-sized image overlap the allocation arena. */
    vm_register(0x10000, 0x30000, 0x1000000);
    vm_register(0x400000, 0x1a8a000, 0x1000000);
    uint32_t addresses[160] = {0};
    for (unsigned step = 0; step < 4000; step++) {
        unsigned slot = random32() % 160;
        if (addresses[slot]) {
            assert(vm_free(addresses[slot], 0, 0x8000));
            addresses[slot] = 0;
        } else {
            uint32_t size = (1 + random32() % 64) * 0x1000;
            int top = random32() & 1;
            uint32_t expected = reference_gap((size + 0xffff) & ~0xffffu, top);
            uint32_t a = vm_alloc(0, size, 0x3000 | (top ? 0x100000 : 0), 4);
            assert(a == expected && a);
            assert(vm_committed(a, size));
            assert(!G_MEM[a] && !G_MEM[a + size - 1]);
            G_MEM[a] = 0x53; G_MEM[a + size - 1] = 0x92;
            addresses[slot] = a;
        }
    }
    for (unsigned i = 0; i < 160; i++) if (addresses[i]) assert(vm_free(addresses[i], 0, 0x8000));
    uint32_t a = vm_alloc(0, 0x3000, 0x2000, 4);
    assert(a && !vm_committed(a, 0x3000));
    assert(vm_alloc(a + 0x1000, 0x1000, 0x1000, 4) == a + 0x1000);
    assert(vm_page_state(a) == 1 && vm_page_state(a + 0x1000) == 2);
    G_MEM[a + 0x1000] = 0x71;
    assert(vm_free(a + 0x1000, 0x1000, 0x4000));
    assert(vm_alloc(a + 0x1000, 0x1000, 0x1000, 4) == a + 0x1000);
    assert(!G_MEM[a + 0x1000]);
    assert(!vm_alloc(a, 0x1000, 0x3000, 4));
    assert(vm_free(a, 0, 0x8000));
    assert(vm_alloc(a, 0x3000, 0x3000, 4) == a);
    assert(vm_query(a, 0x10000) == 28);
    assert(rt_r32(G_MEM, 0x10000) == a && rt_r32(G_MEM, 0x10010) == 0x1000);
    assert(vm_free(a, 0, 0x8000));
    reset_regions();
    munmap(G_MEM, 1ull << 32);
    free(R);
    puts("vm gap: 4000 allocate/free steps, commit/decommit and reuse passed");
}
int main(void) { placement(); allocation(); return 0; }
