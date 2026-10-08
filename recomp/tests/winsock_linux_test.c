/* Exercise the real WinSock import boundary with loopback sockets, not game files. */
#include <assert.h>
#include <stdarg.h>
#include "../runtime/win32/winsock.c"
uint8_t memory[8192];
uint8_t *G_MEM = memory;
static uint32_t last_error;
void w32_set_last_error(Ctx *c, uint32_t error) { (void)c; last_error = error; }
static uint32_t call(void (*fn)(Ctx *), int n, ...) {
    Ctx c = {.esp = 256}; va_list ap; va_start(ap,n);
    for(int i=0;i<n;i++) rt_w32(G_MEM,c.esp+4+4*i,va_arg(ap,unsigned));
    va_end(ap); fn(&c); assert(c.esp==256+4+4*n); return c.eax;
}
static void set(uint32_t s, uint32_t level, uint32_t name, uint32_t value) {
    rt_w32(G_MEM,512,value); assert(call(imp_wsock32_ord21,5,s,level,name,512u,4u)==0);
}
static uint32_t get(uint32_t s, uint32_t level, uint32_t name) {
    rt_w32(G_MEM,520,4); assert(call(imp_wsock32_ord7,5,s,level,name,512u,520u)==0);
    assert(rt_r32(G_MEM,520)==4); return rt_r32(G_MEM,512);
}
int main(void) {
    uint32_t s=call(imp_wsock32_ord23,3,2u,2u,0u); assert(s!=INVALID_SOCKET_);
    set(s,0xffff,4,1); assert(get(s,0xffff,4)==1);
    set(s,0xffff,0x20,1); assert(get(s,0xffff,0x20)==1);
    set(s,0xffff,0x1006,250); assert(get(s,0xffff,0x1006)>=250 && get(s,0xffff,0x1006)<300);
    assert(get(s,0xffff,0x1008)==2);
    rt_w16(G_MEM,600,2); rt_w32(G_MEM,604,htonl(INADDR_LOOPBACK));
    assert(call(imp_wsock32_ord2,3,s,600u,16u)==0);
    rt_w32(G_MEM,620,16); assert(call(imp_wsock32_ord6,3,s,600u,620u)==0);
    memcpy(G_MEM+700,"DS2",4);
    assert(call(imp_wsock32_ord20,6,s,700u,4u,0u,600u,16u)==4);
    assert(call(imp_wsock32_ord17,6,s,710u,4u,0u,0u,0u)==4); assert(!memcmp(G_MEM+710,"DS2",4));
    assert(call(imp_wsock32_ord3,1,s)==0);
    s=call(imp_wsock32_ord23,3,2u,1u,0u); assert(s!=INVALID_SOCKET_);
    set(s,6,1,1); assert(get(s,6,1)==1);
    set(s,0xffff,8,1); assert(get(s,0xffff,8)==1);
    set(s,0xffff,0x80,(7u<<16)|1); assert(get(s,0xffff,0x80)==((7u<<16)|1));
    set(s,0xffff,0xff7f,1); assert(get(s,0xffff,0xff7f)==1);
    rt_w32(G_MEM,520,2); rt_w32(G_MEM,512,0xfeedbeef);
    assert(call(imp_wsock32_ord7,5,s,0xffffu,0x1008u,512u,520u)==SOCKET_ERROR_);
    assert(last_error==10014 && rt_r32(G_MEM,512)==0xfeedbeef);
    assert(call(imp_wsock32_ord21,5,s,0xffffu,0x1234u,512u,4u)==SOCKET_ERROR_ && last_error==10042);
    assert(call(imp_wsock32_ord3,1,s)==0);
    puts("WinSock options, guest bounds and UDP loopback passed");
}
