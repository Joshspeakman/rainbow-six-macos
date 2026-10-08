"""Compile and execute synthetic x86 switches, atomics and nonlocal guest returns."""
from pathlib import Path
from types import SimpleNamespace
import collections
import struct
import subprocess
import sys
import tempfile
import capstone

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from lift import Lifter
from compiler_compat import compatible_flags


def lifter(base, code, functions):
    l=Lifter.__new__(Lifter)
    l.TLO,l.THI,l.code=base,base+len(code),bytes(code)
    l.pe=SimpleNamespace(u32=lambda va: struct.unpack_from('<I',code,va-base)[0] if base<=va<l.THI-3 else None)
    l.md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32);l.md.detail=True
    l.funcs=set(functions);l.cache={};l.jt_cache={};l.imp_index={};l.scc_of={};l.tail_edges=collections.defaultdict(set)
    l.unsup=collections.Counter();l.internal=[];l.unres_jt=[];l.hooks={};l.overrides={};l.illegal_seh=False;l.guest_flow=True
    return l


code=bytearray(b'\xcc'*0x120)
code[:14]=bytes.fromhex('89c883f8027739ff248500114000')
for offset,value in ((0x20,10),(0x28,20),(0x30,30),(0x40,99)):
    code[offset:offset+6]=b'\xb8'+struct.pack('<I',value)+b'\xc3'
for offset,raw in ((0x50,'0fc1d8c3'),(0x58,'0fc1c0c3'),(0x60,'f00fc107c3'),(0x68,'8707c3')):
    data=bytes.fromhex(raw);code[offset:offset+len(data)]=data
code[0x80:0x8f]=bytes.fromhex('67e306b800000000c3b801000000c3')
struct.pack_into('<III',code,0x100,0x401020,0x401028,0x401030)
l=lifter(0x401000,code,[0x401000,0x401050,0x401058,0x401060,0x401068,0x401080])
functions={f:l.lift_function(f)[0] for f in sorted(l.funcs)}
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# CALL helper; RET. The helper discards its own return and returns directly to
# the outer caller. A host C CALL/RET pair cannot represent this guest flow.
code=bytearray(b'\xcc'*0x40)
code[:6]=bytes.fromhex('e81b000000c3')
code[0x20:0x29]=bytes.fromhex('b80700000083c404c3')
l=lifter(0x1800000,code,[0x1800000,0x1800005,0x1800020])
for f in sorted(l.funcs):
    source,callees=l.lift_function(f);functions[f]=source
    assert callees<=l.funcs
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# A relocated call pushes a provisional return, then adjusts it to the code
# immediately following its RET dispatcher. Discovery must compile that entry
# even though no direct branch names the final return address.
base=0x1800100
code=bytearray(b'\xcc'*0x60)
code[:10]=b'\x68'+struct.pack('<I',base+0x10)+b'\xe9'+struct.pack('<i',6)
code[0x10:0x1e]=b'\x68'+struct.pack('<I',base+0x50)+bytes.fromhex('814424040e000000c3')
code[0x1e:0x22]=bytes.fromhex('83c002c3')
code[0x50:0x56]=bytes.fromhex('b807000000c3')
l=lifter(base,code,[base]);queue=collections.deque(sorted(l.funcs))
while queue:
    f=queue.popleft();source,callees=l.lift_function(f);functions[f]=source
    for t in sorted(callees-l.funcs):
        l.funcs.add(t);queue.append(t)
assert base+0x1e in l.funcs
assert base+0x50 in l.funcs
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# An unbounded table's cases can be much farther away than 8 KiB (BW's
# shader setup has such a switch). Discover and dispatch the distant case.
base=0x402000
code=bytearray(b'\xcc'*0x5100)
code[:9]=bytes.fromhex('89c8ff248500214000')
for offset,value in ((0x30,40),(0x5000,41)):
    code[offset:offset+6]=b'\xb8'+struct.pack('<I',value)+b'\xc3'
struct.pack_into('<II',code,0x100,base+0x30,base+0x5000)
l=lifter(base,code,[base]);source,callees=l.lift_function(base)
assert base+0x5000 in callees
functions[base]=source
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# A CALL helper alters the return at the top of its stack. The destination
# contains no ordinary branch label and must be discovered from that change.
base=0x1800200
code=bytearray(b'\xcc'*0x40)
code[:5]=bytes.fromhex('e81b000000')
code[0x0a:0x10]=bytes.fromhex('b813000000c3')
code[0x20:0x25]=bytes.fromhex('83042405c3')
l=lifter(base,code,[base]);queue=collections.deque([base])
while queue:
    f=queue.popleft();source,callees=l.lift_function(f);functions[f]=source
    for t in sorted(callees-l.funcs):
        l.funcs.add(t);queue.append(t)
assert base+0x0a in l.funcs
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# A computed continuation follows an unconditional jump to a RET trampoline.
base=0x1800300
code=bytearray(b'\xcc'*0x40)
code[:17]=b'\x68'+struct.pack('<I',0x1234)+b'\x81\x04\x24'+struct.pack('<I',base+17-0x1234)+b'\xe9'+struct.pack('<i',0x30-17)
code[17:23]=bytes.fromhex('b817000000c3')
code[0x30]=0xc3
l=lifter(base,code,[base]);queue=collections.deque([base])
while queue:
    f=queue.popleft();source,callees=l.lift_function(f);functions[f]=source
    for t in sorted(callees-l.funcs):
        l.funcs.add(t);queue.append(t)
assert base+17 in l.funcs
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# The destination overlaps the immediate of an unreachable instruction after
# the jump. Only the stack expression identifies the correct decode boundary.
base=0x1800400
code=bytearray(b'\xcc'*0x40)
code[:18]=b'\x9c\x68'+struct.pack('<I',0x1234)+b'\x81\x04\x24'+struct.pack('<I',base+19-0x1234)+b'\xe9'+struct.pack('<i',0x30-18)
code[18]=0xb9
code[19:26]=bytes.fromhex('9db81d000000c3')
code[0x30]=0xc3
l=lifter(base,code,[base]);queue=collections.deque([base])
while queue:
    f=queue.popleft();source,callees=l.lift_function(f);functions[f]=source
    for t in sorted(callees-l.funcs):
        l.funcs.add(t);queue.append(t)
assert base+19 in l.funcs
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# A return computed elsewhere can enter a shared epilogue in the original
# .text. Its label must have a callable entry even when already inlined into
# another lifted function (and not just when it lives in a relocated section).
base=0x408000
code=bytearray(b'\xcc'*0x20)
code[:4]=bytes.fromhex('31c0eb0c')
code[0x10:0x16]=bytes.fromhex('b82a000000c3')
l=lifter(base,code,[base]);source,callees=l.lift_function(base)
assert base+0x10 in callees
functions[base]=source
functions[base+0x10]=l.lift_function(base+0x10)[0]
assert not l.internal and not l.unsup,(l.internal,l.unsup)

# A bytecode continuation enters a saved-flags prologue inside a linear block.
base=0x1800500
code=bytearray(bytes.fromhex('31c09c9dc3'))
l=lifter(base,code,[base]);source,callees=l.lift_function(base)
assert base+2 in callees
functions[base]=source
functions[base+2]=l.lift_function(base+2)[0]
assert not l.internal and not l.unsup,(l.internal,l.unsup)

driver=r'''
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
const struct rt_fn { uint32_t addr; GuestFn fn; } rt_fntab[]={FUNCTION_TABLE};
const unsigned rt_fntab_n=sizeof rt_fntab/sizeof rt_fntab[0];
const struct rt_imp { uint32_t iat; const char *name; } rt_imptab[]={{0,"unused"}};
const unsigned rt_imptab_n=0;
const struct rt_image { const char *name; uint32_t base, imp_first, imp_count; } rt_images[]={{"",0x400000,0,0}};
const unsigned rt_images_n=1;
static void run(Ctx *c,uint32_t pc) {
    c->esp=0x8000;rt_w32(G_MEM,c->esp,0xdeadbeef);rt_call(c,pc);assert(c->esp==0x8004);
}
static void flags(Ctx *c,uint32_t a,uint32_t b) {
    uint32_t r=a+b,f=flags_get(&c->f);
    assert(!!(f&F_CF)==((uint64_t)a+b>0xffffffffu));
    assert(!!(f&F_ZF)==(r==0));
    assert(!!(f&F_OF)==!!((~(a^b)&(a^r))&0x80000000u));
}
int main(void) {
    G_MEM=calloc(1,0x500000); assert(G_MEM);
    rt_w32(G_MEM,0x401100,0x401020);rt_w32(G_MEM,0x401104,0x401028);rt_w32(G_MEM,0x401108,0x401030);
    rt_w32(G_MEM,0x402100,0x402030);rt_w32(G_MEM,0x402104,0x407000);
    for(unsigned i=0;i<4;i++) {
        Ctx c={.ecx=i};run(&c,0x401000);assert(c.eax==(i<3?(i+1)*10:99));
    }
    const uint32_t pairs[][2]={{0,0},{0xffffffffu,1},{0x7fffffffu,1},{0x80000000u,0x80000000u},{19,27}};
    for(unsigned i=0;i<sizeof pairs/sizeof pairs[0];i++) {
        uint32_t a=pairs[i][0],b=pairs[i][1];Ctx c={.eax=a,.ebx=b};
        run(&c,0x401050);assert(c.eax==a+b && c.ebx==a);flags(&c,a,b);
        c.eax=a;run(&c,0x401058);assert(c.eax==a+a);flags(&c,a,a);
        c.eax=b;c.edi=0x1000;rt_w32(G_MEM,c.edi,a);run(&c,0x401060);
        assert(c.eax==a && rt_r32(G_MEM,c.edi)==a+b);flags(&c,a,b);
        c.eax=b;rt_w32(G_MEM,c.edi,a);run(&c,0x401068);
        assert(c.eax==a && rt_r32(G_MEM,c.edi)==b);
    }
    Ctx c={0};run(&c,0x1800000);assert(c.eax==7);
    c.ecx=0x10000;run(&c,0x401080);assert(c.eax==1);
    c.ecx=0x10001;run(&c,0x401080);assert(c.eax==0);
    run(&c,0x1800100);assert(c.eax==9);
    run(&c,0x1800200);assert(c.eax==19);
    run(&c,0x1800300);assert(c.eax==23);
    run(&c,0x1800400);assert(c.eax==29);
    run(&c,0x408010);assert(c.eax==42);
    c.eax=25;run(&c,0x1800502);assert(c.eax==25);
    for(unsigned i=0;i<2;i++) { c.ecx=i;run(&c,0x402000);assert(c.eax==40+i); }
    free(G_MEM);puts("lifted guest flow: switch, atomics, nonlocal and adjusted return passed");
}
'''
driver=driver.replace('FUNCTION_TABLE',','.join('{0x%x,f_%08x}'%(f,f) for f in sorted(functions)))
with tempfile.TemporaryDirectory() as d:
    path=Path(d);(path/'flow.c').write_text('#include "rt.h"\n'+'\n'.join(functions.values())+driver)
    subprocess.run(['clang','-O2',*compatible_flags('clang'),'-D_GNU_SOURCE','-DDS2_BROKEN_WORLD','-I',str(ROOT/'runtime'),str(path/'flow.c'),str(ROOT/'runtime/rt.c'),'-lm','-pthread','-o',str(path/'flow')],check=True)
    subprocess.run([str(path/'flow')],check=True)
