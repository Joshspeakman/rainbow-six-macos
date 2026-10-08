/* Synthetic push/jmp, push/ret, nested call and import-return regression. */
#include "../runtime/rt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void push(Ctx *c, uint32_t value) { c->esp -= 4; rt_w32(G_MEM,c->esp,value); }
static void ret(Ctx *c) { c->tc=rt_r32(G_MEM,c->esp); c->esp+=4; }
static void start(Ctx *c) {
    push(c,0x401100); c->tc=0x401200; /* synthetic call: push continuation; jmp callee */
}
static void continuation(Ctx *c) {
    assert(c->eax==11); c->eax+=2;
    push(c,0x401101); rt_call(c,0x401300); /* ordinary call into a ret-dispatch block */
    assert(c->eax==16);
    push(c,0x401400); c->tc=0xfff00000; /* synthetic call to an import */
}
static void callee(Ctx *c) { c->eax=11; ret(c); }
static void ret_branch(Ctx *c) { push(c,0x401350); ret(c); }
static void ret_destination(Ctx *c) { c->eax+=3; ret(c); }
static void after_import(Ctx *c) { assert(c->eax==23); c->eax+=1; push(c,0x401500); c->tc=0x10000000; }
static void foreign_dll(Ctx *c) { c->eax+=5; c->esp+=4; } /* existing DLLs use normal native calls */
static void after_dll(Ctx *c) { assert(c->eax==29); ret(c); }
struct rt_fn { uint32_t addr; GuestFn fn; };
const struct rt_fn rt_fntab[]={{0x401000,start},{0x401100,continuation},{0x401200,callee},
    {0x401300,ret_branch},{0x401350,ret_destination},{0x401400,after_import},
    {0x401500,after_dll},{0x10000000,foreign_dll}};
const unsigned rt_fntab_n=sizeof rt_fntab/sizeof rt_fntab[0];
const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[]={{0,"synthetic"}};
const unsigned rt_imptab_n=1;
const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[]={{"",0x400000,0,1}};
const unsigned rt_images_n=1;
void rt_import(Ctx *c,uint32_t index) { assert(index==0); c->eax+=7; c->esp+=4; }
int main(void) {
    G_MEM=calloc(1,0x10000); assert(G_MEM);
    Ctx c={.esp=0x8000}; push(&c,0xdeadbeef);
    rt_call(&c,0x401000);
    assert(c.eax==29 && c.esp==0x8000 && c.tc==0);
    free(G_MEM); puts("guest flow: relocated calls, ret branches, imports and DLL continuations passed");
}
