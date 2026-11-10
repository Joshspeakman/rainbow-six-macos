#include "w32.h"
#include "thread_state.h"
#include <assert.h>
#include <sys/mman.h>
#include <setjmp.h>
#include <unistd.h>
#include <stdatomic.h>
uint8_t *G_MEM; int w32_protect_memory;
__thread jmp_buf *fault_jmp; __thread uint32_t fault_pc; __thread const char *fault_what; __thread Ctx *w32_cur_ctx;
void w32_crash_thread_at(uint32_t x){(void)x;} void w32_crash_thread(const char *x){(void)x;}
void w32_crash_wait_begin(int a,uint32_t b,uint32_t c){(void)a;(void)b;(void)c;} void w32_crash_wait_end(void){}
void rt_unhandled(Ctx *c,uint32_t pc,const char *s){(void)c;(void)pc;fprintf(stderr,"%s\n",s);abort();}
static atomic_uint entered,gate,stack_addr;
void rt_call(Ctx *c,uint32_t fn){
 if(fn==2){stack_addr=rt_r32(G_MEM,c->fs_base+8);entered=1;while(!gate)usleep(1000);}
 uint32_t tls=rt_r32(G_MEM,c->fs_base+0x1000+8);
 assert(tls&&rt_r32(G_MEM,tls)==0x12345678&&rt_r32(G_MEM,tls+4)==0);
 c->eax=rt_r32(G_MEM,c->esp+4);
}
void imp_kernel32_CreateThread(Ctx *),imp_kernel32_GetExitCodeThread(Ctx *),imp_kernel32_ResumeThread(Ctx *);
int w32_wait_one(Ctx *,uint32_t,uint32_t);
static uint32_t sp;
static uint32_t create(Ctx *c,uint32_t fn,uint32_t code,uint32_t flags){
 c->esp=sp;memset(G_MEM+sp,0,32);rt_w32(G_MEM,sp+12,fn);rt_w32(G_MEM,sp+16,code);rt_w32(G_MEM,sp+20,flags);
 imp_kernel32_CreateThread(c);assert(c->eax);return c->eax;
}
static uint32_t result(Ctx *c,uint32_t h){c->esp=sp;rt_w32(G_MEM,sp+4,h);rt_w32(G_MEM,sp+8,sp+64);imp_kernel32_GetExitCodeThread(c);assert(c->eax);return rt_r32(G_MEM,sp+64);}
static void service(Ctx *c,void *arg){(void)arg;stack_addr=rt_r32(G_MEM,c->fs_base+8);entered=1;}
int w32_spawn_service(void (*)(Ctx *,void *),void *);
int main(void){
 G_MEM=mmap(0,1ull<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);assert(G_MEM!=MAP_FAILED);
 w32_process_heap=heap_create(0,0);sp=vm_alloc(0,1<<20,0x3000,4);Ctx c={.fs_base=w32_new_thread_teb(sp,sp+(1<<20))};
 uint32_t fixture=vm_alloc(0,4096,0x3000,4);assert(fixture);
 w32_tls_index_addr=fixture;rt_w32(G_MEM,fixture,2);w32_tls_template=fixture+4;
 rt_w32(G_MEM,fixture+4,0x12345678);w32_tls_size=4;w32_tls_zero=12;
 /* Retained handles must not limit the number of completed threads, or acquire
    a later thread's exit status when its TEB gets reused. */
 uint32_t handles[256];
 for(unsigned i=0;i<256;i++){handles[i]=create(&c,1,1000+i,0);assert(w32_wait_one(&c,handles[i],5000)==0);}
 for(unsigned i=0;i<256;i++){assert(result(&c,handles[i])==1000+i);assert(h_close(handles[i]));}
 uint32_t h=create(&c,1,42,4);assert(result(&c,h)==259);assert(w32_wait_one(&c,h,0)==0x102);
 c.esp=sp;rt_w32(G_MEM,sp+4,h);imp_kernel32_ResumeThread(&c);assert(c.eax==1);assert(w32_wait_one(&c,h,5000)==0);assert(result(&c,h)==42);assert(h_close(h));
 /* Last handle closed before completion: the running thread still owns t. */
 entered=0;gate=0;h=create(&c,2,99,0);for(int n=0;n<5000&&!entered;n++)usleep(1000);assert(entered);
 assert(h_close(h));uint32_t stack=stack_addr;gate=1;
 for(int n=0;n<5000&&vm_committed(stack,4096);n++)usleep(1000);assert(!vm_committed(stack,4096));
 for(int i=0;i<80;i++){
  entered=0;assert(!w32_spawn_service(service,0));for(int n=0;n<5000&&!entered;n++)usleep(1000);assert(entered);
  stack=stack_addr;for(int n=0;n<5000&&vm_committed(stack,4096);n++)usleep(1000);assert(!vm_committed(stack,4096));
 }
 /* Fill all live TEB slots, reject one more thread, then reclaim them. */
 for(int i=0;i<MAX_THREADS-1;i++)handles[i]=create(&c,1,77,4);
 c.esp=sp;memset(G_MEM+sp,0,32);rt_w32(G_MEM,sp+12,1);imp_kernel32_CreateThread(&c);assert(!c.eax);
 for(int i=0;i<MAX_THREADS-1;i++){
  c.esp=sp;rt_w32(G_MEM,sp+4,handles[i]);imp_kernel32_ResumeThread(&c);assert(c.eax==1);
  assert(w32_wait_one(&c,handles[i],5000)==0&&result(&c,handles[i])==77);assert(h_close(handles[i]));
 }
 h=create(&c,1,88,0);assert(w32_wait_one(&c,h,5000)==0&&result(&c,h)==88);assert(h_close(h));
 puts("threads: retained handles, exit status, suspension, early close and service churn passed");
}
