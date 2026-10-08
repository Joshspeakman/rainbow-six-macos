/* Synthetic compiler check: a byte update must preserve the other 24 bits
 * when the full register is stored, even if its low byte changes afterward. */
#include <stdint.h>
#include <string.h>
static uint8_t memory[65536];
static volatile uint32_t base_input = 0x1200, destination_input = 0x3000;
__attribute__((noinline)) static uint32_t partial(uint32_t base, uint32_t destination)
{
    uint32_t reg = base + 16;
    reg = (reg & 0xffffff00u) | memory[reg];
    memcpy(memory + destination, &reg, 4);
    return (reg & 0xffffff00u) | 8u;
}
int main(void)
{
    memory[0x1210] = 0x6b;
    uint32_t reg = partial(base_input, destination_input), saved;
    memcpy(&saved, memory + 0x3000, 4);
    return reg != 0x1208 || saved != 0x126b;
}
