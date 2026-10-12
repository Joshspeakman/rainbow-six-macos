/* Repeated and colliding handle lookups, including freed slot/address reuse. */
#include "../runtime/win32/vulkan.c"
#include <assert.h>
#include <time.h>
uint8_t *G_MEM; int w32_protect_memory;
int main(void) {
 G_MEM=mmap(0,1ull<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);assert(G_MEM!=MAP_FAILED);
 w32_process_heap=heap_create(0,0);uint32_t guests[4096];
 for(unsigned i=0;i<4096;i++)guests[i]=vkh_guest((void *)(uintptr_t)(0x100000+16*i));
 for(unsigned r=0;r<2;r++)for(unsigned i=0;i<4096;i++)assert(vkh_host(guests[i])==(void *)(uintptr_t)(0x100000+16*i));
 for(unsigned i=0;i<4096;i+=7) {
  vkh_forget((void *)(uintptr_t)(0x100000+16*i));assert(!vkh_host(guests[i]));
  uint32_t replacement=vkh_guest((void *)(uintptr_t)(0x200000+16*i));
  assert(vkh_host(replacement)==(void *)(uintptr_t)(0x200000+16*i));
 }
 struct timespec a,z;clock_gettime(CLOCK_MONOTONIC,&a);
 for(unsigned i=0;i<1000000;i++)assert(vkh_host(guests[4094])==(void *)(uintptr_t)(0x100000+16*4094));
 clock_gettime(CLOCK_MONOTONIC,&z);
 printf("Vulkan: live lookup, collisions and slot reuse passed; 1M lookups %.6f seconds\n",z.tv_sec-a.tv_sec+(z.tv_nsec-a.tv_nsec)*1e-9);
}
