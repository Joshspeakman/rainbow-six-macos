/* Queue order across head wrap, filtered removal and thread-slot reuse. */
#include "../runtime/win32/user32.c"
#include <assert.h>
uint8_t *G_MEM; int w32_deterministic=1;
uint32_t w32_tid(Ctx *c) { return c->eax; }
int main(void) {
 G_MEM=calloc(1,65536); assert(G_MEM); Ctx c={.eax=17};
 for(unsigned round=0;round<12;round++) {
  for(unsigned i=0;i<500;i++) w32_post(17,0,0x400+i,i,0);
  assert(peek(&c,1024,0,0x450,0x450,0)); assert(rt_r32(G_MEM,1032)==80);
  assert(peek(&c,1024,0,0x450,0x450,1));
  for(unsigned i=0;i<500;i++) if(i!=80) {
   assert(peek(&c,1024,0,0,0,1)); assert(rt_r32(G_MEM,1032)==i);
  }
  assert(!peek(&c,1024,0,0,0,1));
 }
 w32_post(17,0,0x400,99,0);w32_thread_queue_closed(17);
 c.eax=18;assert(!peek(&c,1024,0,0,0,1));
 w32_post(18,0,0x400,100,0);assert(peek(&c,1024,0,0,0,1));assert(rt_r32(G_MEM,1032)==100);
 free(G_MEM);puts("message queue: wrap, filtered order, no-remove and slot reuse passed");
}
