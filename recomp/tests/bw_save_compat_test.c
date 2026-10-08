/* Only the verified base content CRC gets the expansion compatibility path. */
#include "../runtime/win32/hooks.c"
#include <assert.h>
uint8_t *G_MEM;
int native_hook(Ctx *c, uint32_t a) { (void)c; (void)a; return 0; }
int native_override(Ctx *c, uint32_t a) { (void)c; (void)a; return 0; }
int vm_committed(uint32_t a, uint32_t n) { (void)a; (void)n; return 0; }
int main(void)
{
    uint8_t memory[4096] = {0}; G_MEM = memory;
    Ctx initial = {.eax=0xabcdef00u, .ecx=123, .esp=512, .ebp=900};
    rt_w32(G_MEM, 512, 0x12345678u);
    const uint32_t original[] = {0, 0x790bfe9f, 0xca03370a, 0xd4ee4a9e, 0xffffffff};
    for (unsigned i=0; i<sizeof original/sizeof original[0]; i++) {
        Ctx c=initial; rt_w32(G_MEM, 516, original[i]);
        assert(!rt_override(&c, 0x181d35a));
        assert(!memcmp(&c, &initial, sizeof c));
    }
    Ctx c=initial; rt_w32(G_MEM, 516, 0xd4ee4a9f);
    assert(!rt_override(&c, 0x181d35b));
    assert(!memcmp(&c, &initial, sizeof c));
    assert(rt_override(&c, 0x181d35a));
    Ctx expected=initial; expected.eax |= 1; expected.esp += 8;
    assert(!memcmp(&c, &expected, sizeof c));
    assert(rt_r32(G_MEM, 512)==0x12345678u && rt_r32(G_MEM, 516)==0xd4ee4a9fu);
    puts("BW save compatibility: supported GOG CRC, unchanged fallback and bool/RET 4 ABI passed");
}
