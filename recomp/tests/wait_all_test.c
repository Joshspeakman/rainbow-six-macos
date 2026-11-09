/* WaitForMultipleObjects keeps Windows' contract: wait-all changes no object until every one is ready, one timeout for the
 * whole call, invalid handles and bad counts fail. The link fixture (guest memory, stubs) is the thread lifecycle test's. */
#define main thread_lifecycle_main
#include "thread_lifecycle_test.c"
#undef main
#include <pthread.h>
#include <time.h>
void imp_kernel32_WaitForMultipleObjects(Ctx *),imp_kernel32_CreateEventA(Ctx *),imp_kernel32_CreateMutexA(Ctx *),imp_kernel32_SetEvent(Ctx *),imp_kernel32_GetLastError(Ctx *);
uint32_t h_new(int,void *);
static uint32_t list,sp2,thr_teb;
static uint32_t call(Ctx *c,uint32_t s,void (*f)(Ctx *),uint32_t a,uint32_t b,uint32_t d,uint32_t e){
 c->esp=s;memset(G_MEM+s,0,32);rt_w32(G_MEM,s+4,a);rt_w32(G_MEM,s+8,b);rt_w32(G_MEM,s+12,d);rt_w32(G_MEM,s+16,e);f(c);return c->eax;
}
static uint32_t wmo(Ctx *c,const uint32_t *h,uint32_t n,uint32_t all,uint32_t ms){
 for(uint32_t i=0;i<n&&i<64;i++)rt_w32(G_MEM,list+4*i,h[i]);
 return call(c,sp,imp_kernel32_WaitForMultipleObjects,n,list,all,ms);
}
static uint32_t ev(Ctx *c,int manual,int on){uint32_t h=call(c,sp,imp_kernel32_CreateEventA,0,manual,on,0);assert(h);return h;}
static uint32_t err(Ctx *c){return call(c,sp,imp_kernel32_GetLastError,0,0,0,0);}
static uint32_t sem(int n){Sync *s=calloc(1,sizeof *s);pthread_mutex_init(&s->m,0);pthread_cond_init(&s->cv,0);s->count=n;uint32_t h=h_new(H_SEMAPHORE,s);assert(h);return h;}
static uint32_t mtx(Ctx *c){uint32_t h=call(c,sp,imp_kernel32_CreateMutexA,0,0,0,0);assert(h);return h;}
static Sync *obj(uint32_t h){HObj *o=h_get(h,0);assert(o);return o->p;}
static long now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000;}
struct later{uint32_t h,ms;};
static void *fire(void *p){struct later *l=p;usleep(l->ms*1000);Ctx t={.fs_base=thr_teb};call(&t,sp2,imp_kernel32_SetEvent,l->h,0,0,0);return 0;}
int main(void){
 G_MEM=mmap(0,1ull<<32,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANON|MAP_NORESERVE,-1,0);assert(G_MEM!=MAP_FAILED);
 w32_process_heap=heap_create(0,0);sp=vm_alloc(0,1<<20,0x3000,4);sp2=vm_alloc(0,1<<20,0x3000,4);list=vm_alloc(0,4096,0x3000,4);
 Ctx c={.fs_base=w32_new_thread_teb(sp,sp+(1<<20))};thr_teb=c.fs_base;
 /* a timed-out wait-all leaves an auto-reset event, a semaphore and a mutex as they were */
 uint32_t a=ev(&c,0,1),none=ev(&c,0,0),s=sem(1),m=mtx(&c);
 uint32_t hs[4]={a,s,m,none};
 assert(wmo(&c,hs,4,1,0)==0x102);
 assert(obj(a)->signaled==1&&obj(s)->count==1&&obj(m)->count==0&&obj(m)->owner==0);
 assert(wmo(&c,hs,4,1,20)==0x102);
 assert(obj(a)->signaled==1&&obj(s)->count==1&&obj(m)->count==0);
 assert(w32_wait_one(&c,a,0)==0);
 /* everything ready: the whole set is taken, a manual-reset event stays signalled */
 uint32_t b=ev(&c,0,1),man=ev(&c,1,1);uint32_t set[4]={b,s,m,man};
 assert(wmo(&c,set,4,1,0)==0);
 assert(obj(b)->signaled==0&&obj(s)->count==0&&obj(m)->count==1&&obj(m)->owner==w32_tid(&c)&&obj(man)->signaled==1);
 /* one that becomes ready later completes the set */
 uint32_t e1=ev(&c,0,1),e2=ev(&c,0,0);uint32_t two[2]={e1,e2};struct later l={e2,30};pthread_t t;
 assert(!pthread_create(&t,0,fire,&l));assert(wmo(&c,two,2,1,5000)==0);pthread_join(t,0);
 assert(obj(e1)->signaled==0&&obj(e2)->signaled==0);
 /* one timeout for the call: an object ready late must not restart the clock for the next */
 uint32_t e3=ev(&c,0,0),e4=ev(&c,0,0);uint32_t pair[2]={e3,e4};struct later l2={e3,200};
 long t0=now_ms();assert(!pthread_create(&t,0,fire,&l2));assert(wmo(&c,pair,2,1,300)==0x102);long took=now_ms()-t0;pthread_join(t,0);
 assert(took>=290&&took<450);assert(obj(e3)->signaled==1);
 /* failures */
 uint32_t bad[2]={e1,0xbad0};
 assert(wmo(&c,bad,2,1,0)==0xffffffffu&&err(&c)==6);
 assert(wmo(&c,bad,2,0,0)==0xffffffffu&&err(&c)==6);
 uint32_t twice[2]={e3,e3};
 assert(wmo(&c,twice,2,1,0)==0xffffffffu&&err(&c)==87);assert(obj(e3)->signaled==1);
 assert(wmo(&c,two,0,1,0)==0xffffffffu&&err(&c)==87);uint32_t many[65]={0};assert(wmo(&c,many,65,0,0)==0xffffffffu&&err(&c)==87);
 /* wait-any still reports the first ready object and consumes only it */
 uint32_t x=ev(&c,0,0),y=ev(&c,0,1),z=ev(&c,0,1);uint32_t any[3]={x,y,z};
 assert(wmo(&c,any,3,0,0)==1&&obj(y)->signaled==0&&obj(z)->signaled==1);
 puts("wait_all: no partial consumption, whole-set acquisition, single deadline, invalid handles passed");
}
