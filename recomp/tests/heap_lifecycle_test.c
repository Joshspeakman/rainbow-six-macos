#include "w32.h"
#include <assert.h>
#include <sys/mman.h>
uint8_t *G_MEM; int w32_protect_memory;
int main(void) {
 G_MEM=mmap(0,1ull<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0); assert(G_MEM!=MAP_FAILED);
 for(unsigned n=(1u<<20)-80;n<=(1u<<20)+80;n++) {
  uint32_t h=heap_create(0,0),p=heap_alloc(h,0,n),q=heap_alloc(h,0,2u<<20);
  assert(p&&q&&vm_committed(p,n)&&vm_committed(q,2u<<20));
  memset(G_MEM+p,0x5a,n); memset(G_MEM+q,0x3c,2u<<20);
  assert(G_MEM[p+n-1]==0x5a); assert(heap_free(h,p)&&heap_free(h,q)&&heap_destroy(h));
 }
 uint32_t h=heap_create(0,0),p=heap_alloc(h,0,64);assert(p);memset(G_MEM+p,0xa5,64);
 const uint32_t bad[]={0xffffffffu,0xfffffff0u,0x80000000u};
 for(unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
  assert(!heap_realloc(h,0,p,bad[i]));assert(!heap_realloc(h,8,p,bad[i]));
  assert(heap_size(h,p)==64&&G_MEM[p+63]==0xa5);
 }
 assert(heap_free(h,p)&&heap_destroy(h));
 for(unsigned i=0;i<1024;i++){h=heap_create(0,0);assert(h&&heap_destroy(h));}
 uint32_t heaps[256];for(int i=0;i<256;i++){heaps[i]=heap_create(0,0);assert(heaps[i]);}
 assert(!heap_create(0,0));assert(heap_destroy(heaps[123]));heaps[123]=heap_create(0,0);assert(heaps[123]);
 for(int i=0;i<256;i++)assert(heap_destroy(heaps[i]));
 puts("heap: segment boundaries, overflowing realloc, table exhaustion and slot reuse passed");
}
