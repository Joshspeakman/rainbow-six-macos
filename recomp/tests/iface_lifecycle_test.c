#include "../runtime/win32/ddraw.c"
#include <assert.h>
#include <sys/mman.h>
uint8_t *G_MEM; int w32_protect_memory;
int main(void){
 G_MEM=mmap(0,1ull<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);assert(G_MEM!=MAP_FAILED);
 w32_process_heap=heap_create(0,0);uint32_t first=0;
 for(int i=0;i<100000;i++){
  uint32_t g=make_iface(3,(void *)0x1234);assert(g&&obj(g,3)==(void *)0x1234);if(!i)first=g;
  assert(g==first);obj_drop(g);assert(heap_size(w32_process_heap,g)==0xffffffffu);
 }
 /* Occupy every host slot: failure must neither use reserved slot 0 nor leak. */
 uint32_t *all=calloc(65535,sizeof *all);assert(all);
 for(int i=0;i<65535;i++){all[i]=make_iface(3,(void *)0x1234);assert(all[i]);}
 assert(!make_iface(3,(void *)0x1234));
 for(int i=0;i<65535;i++)obj_drop(all[i]);free(all);
 assert(make_iface(3,(void *)0x1234));puts("interfaces: guest allocation reclamation and host table exhaustion passed");
}
