#include "../runtime/rt.h"
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
extern GuestFn rt_lookup(uint32_t);
struct rt_fn { uint32_t addr; GuestFn fn; };
extern const struct rt_fn rt_fntab[];extern const unsigned rt_fntab_n;
static void *check(void *arg) {
 uint32_t seed=(uintptr_t)arg+1;
 for(unsigned i=0;i<200000;i++) {
  seed=seed*1664525u+1013904223u;unsigned k=(seed>>8)%rt_fntab_n;
  assert(rt_lookup(rt_fntab[k].addr)==rt_fntab[k].fn);
  assert(!rt_lookup(rt_fntab[k].addr+1));
 }
 return 0;
}
int main(void) {
 assert(!rt_lookup(0));assert(!rt_lookup(0xffffffff));
 pthread_t t[4];for(unsigned k=0;k<4;k++)assert(!pthread_create(&t[k],0,check,(void *)(uintptr_t)k));
 for(unsigned k=0;k<4;k++)pthread_join(t[k],0);
 struct timespec a,b;clock_gettime(CLOCK_MONOTONIC,&a);check((void *)8);clock_gettime(CLOCK_MONOTONIC,&b);
 printf("dispatch: 1.6M threaded hit/miss checks passed; 400k lookups %.6f seconds\n",b.tv_sec-a.tv_sec+(b.tv_nsec-a.tv_nsec)*1e-9);
}
