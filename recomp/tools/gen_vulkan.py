#!/usr/bin/env python3
"""gen_vulkan.py <vk.xml> <commands.txt> <out.c>

Vulkan for 32-bit guest code: one stdcall thunk per command the game resolves (commands.txt, one name per line) and the
structure conversions they need, generated from the Khronos registry. The guest is MSVC x86: pointers, size_t and
dispatchable handles are 4 bytes; 64-bit members (VkDeviceSize, non-dispatchable handles) are 8 bytes and 8-aligned
(/Zp8); stack arguments are dwords, 64-bit ones two of them with no padding. The host is the native ABI, so host
structures are plain C and only the guest side needs offsets.

A structure whose guest and host layouts are the same (no pointers, size_t or dispatchable handles anywhere in it) is
"blittable": the host reads it in place in guest memory. Others are converted into a per-call arena (cin_*) and,
for output structures, written back (cout_*). pNext chains are converted by sType; an unknown sType is logged once
and left out of the chain. Commands listed in MANUAL are written by hand in runtime/win32/vulkan.c; their
conversions are still generated.
"""
import sys, re, xml.etree.ElementTree as ET

MANUAL = {
    'vkGetInstanceProcAddr', 'vkGetDeviceProcAddr', 'vkCreateInstance', 'vkDestroyInstance', 'vkCreateDevice', 'vkDestroyDevice',
    'vkEnumerateInstanceExtensionProperties', 'vkEnumerateInstanceLayerProperties', 'vkEnumerateDeviceExtensionProperties',
    'vkEnumerateDeviceLayerProperties', 'vkEnumeratePhysicalDevices', 'vkGetDeviceQueue', 'vkCreateWin32SurfaceKHR',
    'vkAllocateCommandBuffers', 'vkFreeCommandBuffers', 'vkAllocateMemory', 'vkFreeMemory', 'vkMapMemory', 'vkUnmapMemory',
    'vkFlushMappedMemoryRanges', 'vkInvalidateMappedMemoryRanges', 'vkQueueSubmit', 'vkQueuePresentKHR', 'vkWaitForFences',
    'vkGetFenceStatus', 'vkQueueWaitIdle', 'vkDeviceWaitIdle', 'vkCreateDebugUtilsMessengerEXT', 'vkDestroyDebugUtilsMessengerEXT',
    'vkCreateSwapchainKHR', 'vkGetSwapchainImagesKHR',
}
DISPATCHABLE = {'VkInstance', 'VkPhysicalDevice', 'VkDevice', 'VkQueue', 'VkCommandBuffer'}
# guest (size, align) of scalar types
SCALAR = {'char': (1, 1), 'uint8_t': (1, 1), 'int8_t': (1, 1), 'uint16_t': (2, 2), 'int16_t': (2, 2), 'uint32_t': (4, 4),
          'int32_t': (4, 4), 'float': (4, 4), 'uint64_t': (8, 8), 'int64_t': (8, 8), 'double': (8, 8), 'size_t': (4, 4),
          'int': (4, 4), 'HWND': (4, 4), 'HINSTANCE': (4, 4), 'HANDLE': (4, 4), 'DWORD': (4, 4), 'LPCWSTR': (4, 4),
          'HMONITOR': (4, 4)}

def api_ok(e):
    a = e.get('api'); return a is None or 'vulkan' in a.split(',')

class Member:
    def __init__(self, e):
        self.type = e.find('type').text; self.name = e.find('name').text
        text = (e.text or '') + ''.join((c.text or '') + (c.tail or '') for c in e if c.tag != 'comment') + ''.join(c.tail or '' for c in e if c.tag == 'comment')
        self.const = text.strip().startswith('const')
        tail = (e.find('type').tail or '')
        self.ptr = tail.count('*'); self.len = e.get('altlen') or e.get('len'); self.values = e.get('values')
        en = e.find('enum'); dims = [int(x) for x in re.findall(r'\[(\d+)\]', text)]
        if en is not None: self.array = en.text
        elif dims: self.array = 1
        else: self.array = None
        if dims: [setattr(self, 'array', self.array * d) for d in dims]
        self.optional = e.get('optional')

def main(xmlpath, cmdpath, out):
    root = ET.parse(xmlpath).getroot()
    types, aliases, enums_vals, handles, basetypes, bitmasks, enumtypes, funcptrs = {}, {}, {}, {}, {}, set(), set(), set()
    for t in root.find('types'):
        if not api_ok(t): continue
        cat, name = t.get('category'), t.get('name') or (t.find('name').text if t.find('name') is not None else None)
        if t.get('alias'): aliases[name] = t.get('alias'); continue
        if cat in ('struct', 'union'):
            types[name] = dict(union=cat == 'union', members=[Member(m) for m in t.findall('member') if api_ok(m)],
                               extends=(t.get('structextends') or '').split(',') if t.get('structextends') else [])
        elif cat == 'handle':
            handles[name] = t.find('type').text == 'VK_DEFINE_HANDLE'
        elif cat == 'basetype':
            bt = t.find('type'); basetypes[name] = bt.text if bt is not None else None
        elif cat == 'bitmask':
            bitmasks.add(name); basetypes[name] = t.find('type').text
        elif cat == 'enum': enumtypes.add(name)
        elif cat == 'funcpointer': funcptrs.add(name)
    for e in root.findall('enums'):
        for v in e.findall('enum'):
            if v.get('value') and v.get('name'): enums_vals[v.get('name')] = v.get('value')
    def real(t):
        while t in aliases: t = aliases[t]
        return t
    cmds = {}
    for c in root.find('commands'):
        if not api_ok(c): continue
        if c.get('alias'): cmds[c.get('name')] = ('alias', c.get('alias')); continue
        p = c.find('proto'); cmds[p.find('name').text] = c
    # which feature/extension brings in each type (pNext candidates are limited to the versions and extensions here)
    allowed_ext = {'VK_VERSION_1_0', 'VK_VERSION_1_1', 'VK_VERSION_1_2', 'VK_KHR_surface', 'VK_KHR_swapchain',
                   'VK_KHR_dedicated_allocation', 'VK_KHR_get_memory_requirements2', 'VK_KHR_bind_memory2',
                   'VK_KHR_get_physical_device_properties2', 'VK_KHR_win32_surface', 'VK_EXT_debug_utils'}
    origin = {}
    for f in list(root.findall('feature')) + list(root.find('extensions')):
        if not api_ok(f): continue
        for req in f.findall('require'):
            for ty in req.findall('type'): origin.setdefault(ty.get('name'), f.get('name'))
    want = [l.strip() for l in open(cmdpath) if re.match(r'^vk[A-Z]\w+$', l.strip()) and l.strip() in cmds]
    def cmd_def(n):
        c = cmds[n]
        if isinstance(c, tuple): c = cmds[c[1]]
        ret = c.find('proto').find('type').text
        return ret, [Member(p) for p in c.findall('param') if api_ok(p)]

    def is_struct(t): return real(t) in types
    def scalar_of(t):                                      # guest (size, align) of a non-struct, non-pointer type
        t = real(t)
        if t in SCALAR: return SCALAR[t]
        if t in handles: return (4, 4) if handles[t] else (8, 8)
        if t in enumtypes: return (4, 4)
        if t in basetypes: return scalar_of(basetypes[t]) if basetypes[t] else (4, 4)
        if t in funcptrs: return (4, 4)
        if t.startswith('PFN_'): return (4, 4)
        if t == 'VkBool32': return (4, 4)
        raise KeyError(t)
    layout_cache = {}
    def layout(t):                                         # guest (size, align, [offsets])
        t = real(t)
        if t in layout_cache: return layout_cache[t]
        s = types[t]; off = 0; al = 1; offs = []
        for m in s['members']:
            if m.ptr: sz, a = 4, 4
            elif is_struct(m.type): sz, a, _ = layout(m.type)
            else: sz, a = scalar_of(m.type)
            n = 1
            if m.array is not None: n = int(enums_vals.get(m.array, m.array)) if not isinstance(m.array, int) else m.array
            if s['union']: offs.append(0); off = max(off, sz * n)
            else:
                off = (off + a - 1) // a * a; offs.append(off); off += sz * n
            al = max(al, a)
        size = (off + al - 1) // al * al
        layout_cache[t] = (size, al, offs); return layout_cache[t]
    blit_cache = {}
    def blittable(t):
        t = real(t)
        if t in blit_cache: return blit_cache[t]
        ok = True
        for m in types[t]['members']:
            if m.ptr or real(m.type) == 'size_t' or real(m.type) in DISPATCHABLE or real(m.type).startswith('PFN_') or real(m.type) in funcptrs: ok = False
            elif is_struct(m.type) and not blittable(m.type): ok = False
        blit_cache[t] = ok; return ok

    # the closure of structures reachable from the commands, plus allowed pNext extensions of them
    need = set()
    def visit(t):
        t = real(t)
        if t in need or t not in types: return
        need.add(t)
        for m in types[t]['members']:
            if is_struct(m.type): visit(m.type)
    for n in want:
        ret, ps = cmd_def(n)
        for p in ps:
            if is_struct(p.type): visit(p.type)
    changed = True
    while changed:
        changed = False
        for t, s in types.items():
            if t in need or origin.get(t) not in allowed_ext: continue
            if any(real(x) in need for x in s['extends']): visit(t); changed = True
    need.discard('VkAllocationCallbacks')
    stype_of = {t: types[t]['members'][0].values for t in need if types[t]['members'] and types[t]['members'][0].name == 'sType' and types[t]['members'][0].values}

    o = []
    w = o.append
    w('/* Generated by tools/gen_vulkan.py from the Khronos registry (third_party/vulkan): do not edit. */\n')
    w('#include "vulkan_rt.h"\n\n')
    order = sorted(need)
    protos = []
    for t in order:
        if not blittable(t):
            protos.append('void cin_%s(VkArena *a, uint32_t g, %s *h);\nvoid cout_%s(uint32_t g, const %s *h);\n' % (t, t, t, t))
    protos.append('const void *cin_chain(VkArena *a, uint32_t g);\nvoid cout_chain(uint32_t g, const void *h);\n')

    for t in order:
        size, al, offs = layout(t)
        if blittable(t): continue
        s = types[t]; cin, cout = [], []
        for m, off in zip(s['members'], offs):
            rt = real(m.type); at = 'g + %d' % off; f = 'h->%s' % m.name
            if m.name == 'pNext':
                cin.append('    h->pNext = (void *)cin_chain(a, VKR32(%s));' % at); cout.append('    cout_chain(VKR32(%s), h->pNext);' % at); continue
            if m.array is not None:
                n = 'sizeof h->%s / sizeof *h->%s' % (m.name, m.name)
                if is_struct(rt) and not blittable(rt):
                    _, _, _ = layout(rt); esz = layout(rt)[0]
                    cin.append('    for (unsigned k = 0; k < %s; k++) cin_%s(a, %s + %d * k, &h->%s[k]);' % (n, rt, at, esz, m.name))
                    cout.append('    for (unsigned k = 0; k < %s; k++) cout_%s(%s + %d * k, &h->%s[k]);' % (n, rt, at, esz, m.name))
                else:
                    cin.append('    memcpy(%s, G_MEM + %s, sizeof %s);' % (f, at, f)); cout.append('    memcpy(G_MEM + %s, %s, sizeof %s);' % (at, f, f))
                continue
            if m.ptr == 0:
                if is_struct(rt):
                    if blittable(rt): cin.append('    memcpy(&%s, G_MEM + %s, sizeof %s);' % (f, at, f)); cout.append('    memcpy(G_MEM + %s, &%s, sizeof %s);' % (at, f, f))
                    else: cin.append('    cin_%s(a, %s, &%s);' % (rt, at, f)); cout.append('    cout_%s(%s, &%s);' % (rt, at, f))
                elif rt in DISPATCHABLE:
                    cin.append('    %s = (%s)vkh_host(VKR32(%s));' % (f, rt, at)); cout.append('    VKW32(%s, vkh_guest(%s));' % (at, f))
                elif rt.startswith('PFN_') or rt in funcptrs:
                    cin.append('    %s = 0;  /* guest function pointer: not callable from the host */' % f)
                elif rt in ('HWND', 'HINSTANCE', 'HANDLE', 'HMONITOR', 'LPCWSTR'):
                    cin.append('    %s = (void *)(uintptr_t)VKR32(%s);' % (f, at))
                else:
                    sz = scalar_of(rt)[0]
                    rd = {1: 'VKR8', 2: 'VKR16', 4: 'VKR32', 8: 'VKR64'}[sz]; wr = rd.replace('R', 'W')
                    if rt == 'float': cin.append('    memcpy(&%s, G_MEM + %s, 4);' % (f, at)); cout.append('    memcpy(G_MEM + %s, &%s, 4);' % (at, f))
                    else: cin.append('    %s = (%s)%s(%s);' % (f, rt, rd, at)); cout.append('    %s(%s, (uint64_t)%s);' % (wr, at, f))
                continue
            # pointers
            cnt = None
            if m.len:
                first = m.len.split(',')[0]
                if re.match(r'^\w+$', first) and first != 'null-terminated' and any(x.name == first for x in s['members']):
                    cnt = 'h->%s' % first
            if m.ptr == 2 and rt == 'char':                 # array of strings
                cin.append('    { uint32_t p = VKR32(%s); if (p && %s) { const char **v = vk_arena(a, sizeof *v * %s); for (uint32_t k = 0; k < %s; k++) v[k] = (const char *)VKPTR(VKR32(p + 4 * k)); %s = v; } else %s = 0; }' % (at, cnt, cnt, cnt, f, f))
            elif rt in ('void', 'char') or (not is_struct(rt) and rt not in DISPATCHABLE):
                cin.append('    %s = (void *)VKPTR(VKR32(%s));' % (f, at))   # scalars, bytes, strings: in place
            elif rt in DISPATCHABLE:
                c2 = cnt or '1'
                cin.append('    { uint32_t p = VKR32(%s); if (p) { %s *v = vk_arena(a, sizeof *v * (%s)); for (uint32_t k = 0; k < (%s); k++) v[k] = (%s)vkh_host(VKR32(p + 4 * k)); %s = v; } else %s = 0; }' % (at, rt, c2, c2, rt, f, f))
            elif blittable(rt):
                cin.append('    %s = (void *)VKPTR(VKR32(%s));' % (f, at))
            else:
                c2 = cnt or '1'; esz = layout(rt)[0]
                cin.append('    { uint32_t p = VKR32(%s); if (p) { %s *v = vk_arena(a, sizeof *v * (%s)); for (uint32_t k = 0; k < (%s); k++) cin_%s(a, p + %d * k, &v[k]); %s = v; } else %s = 0; }' % (at, rt, c2, c2, rt, esz, f, f))
        w('void cin_%s(VkArena *a, uint32_t g, %s *h)\n{\n    (void)a; memset(h, 0, sizeof *h);\n%s\n}\n' % (t, t, '\n'.join(cin)))
        w('void cout_%s(uint32_t g, const %s *h)\n{\n%s\n}\n' % (t, t, '\n'.join(cout) or '    (void)g; (void)h;'))
    # pNext chains: by sType
    w('const void *cin_chain(VkArena *a, uint32_t g)\n{\n    if (!g) return 0;\n    switch (VKR32(g)) {\n')
    for t in order:
        if t in stype_of:
            if blittable(t): continue
            w('    case %s: { %s *h = vk_arena(a, sizeof *h); cin_%s(a, g, h); return h; }\n' % (stype_of[t], t, t))
    w('    default: vk_unknown_stype(VKR32(g)); return cin_chain(a, VKR32(g + 4));\n    }\n}\n')
    w('void cout_chain(uint32_t g, const void *hv)\n{\n    const VkBaseOutStructure *h = hv;\n    while (g && h) {\n        if (VKR32(g) != (uint32_t)h->sType) { g = VKR32(g + 4); continue; }\n        switch (h->sType) {\n')
    for t in order:
        if t in stype_of and not blittable(t):
            w('        case %s: cout_%s(g, (const %s *)h); break;\n' % (stype_of[t], t, t))
    w('        default: break;\n        }\n        g = VKR32(g + 4); h = h->pNext;\n    }\n}\n\n')

    # thunks
    for n in want:
        ret, ps = cmd_def(n)
        if n in MANUAL:
            continue
        dw = 0; pre, call, post = [], [], []
        for p in ps:
            rt = real(p.type)
            if p.ptr == 0 and not is_struct(rt):
                sz = scalar_of(rt)[0]
                if rt in DISPATCHABLE: pre.append('    %s %s = (%s)vkh_host(ARG(%d));' % (rt, p.name, rt, dw)); dw += 1
                elif sz == 8: pre.append('    %s %s = (%s)((uint64_t)ARG(%d) | (uint64_t)ARG(%d) << 32);' % (rt, p.name, rt, dw, dw + 1)); dw += 2
                elif rt == 'float': pre.append('    float %s; { uint32_t v = ARG(%d); memcpy(&%s, &v, 4); }' % (p.name, dw, p.name)); dw += 1
                else: pre.append('    %s %s = (%s)ARG(%d);' % (rt, p.name, rt, dw)); dw += 1
                call.append(p.name); continue
            if p.ptr == 0 and is_struct(rt):
                raise SystemExit('%s: struct by value %s' % (n, p.name))
            g = 'ARG(%d)' % dw; dw += 1
            if rt == 'VkAllocationCallbacks': call.append('0'); continue
            cnt = None
            if p.len:
                first = p.len.split(',')[0]
                cp = [x for x in ps if x.name == first]
                if cp: cnt = '(%s ? *%s : 0)' % (first, first) if cp[0].ptr else first
            if is_struct(rt) and not blittable(rt):
                c2 = cnt or '1'; esz = layout(rt)[0]
                pre.append('    %s *%s = 0; { uint32_t p = %s; if (p) { %s = vk_arena(&a, sizeof *%s * (%s)); for (uint32_t k = 0; k < (%s); k++) cin_%s(&a, p + %d * k, &%s[k]); } }' % (rt, p.name, g, p.name, p.name, c2, c2, rt, esz, p.name))
                if not p.const:
                    post.append('    { uint32_t p = %s; if (p) for (uint32_t k = 0; k < (%s); k++) cout_%s(p + %d * k, &%s[k]); }' % (g, c2, rt, esz, p.name))
                call.append(p.name)
            elif rt in DISPATCHABLE and p.const:            # an input array of dispatchable handles
                c2 = cnt or '1'
                pre.append('    %s *%s = 0; { uint32_t p = %s; if (p) { %s = vk_arena(&a, sizeof *%s * (%s)); for (uint32_t k = 0; k < (%s); k++) %s[k] = (%s)vkh_host(VKR32(p + 4 * k)); } }' % (rt, p.name, g, p.name, p.name, c2, c2, p.name, rt))
                call.append(p.name)
            elif rt in DISPATCHABLE:
                raise SystemExit('%s: dispatchable handle pointer %s needs a manual thunk' % (n, p.name))
            else:                                           # blittable structs, scalars, non-dispatchable handles, void: in place
                pre.append('    %s%s *%s = (%s%s *)VKPTR(%s);' % ('const ' if p.const else '', 'void' if rt == 'void' else rt, p.name, 'const ' if p.const else '', 'void' if rt == 'void' else rt, g))
                call.append(p.name)
        w('IMPL(vulkan_1, %s)\n{\n    VkArena a = {0};\n' % n)
        w('\n'.join(pre) + '\n')
        if ret == 'void':
            w('    vkp.%s(%s);\n' % (n, ', '.join(call)))
            w('\n'.join(post) + ('\n' if post else ''))
            w('    vk_arena_free(&a); RET(0, %d);\n}\n' % dw)
        elif ret in ('VkResult', 'uint32_t', 'VkBool32'):
            w('    uint32_t r = (uint32_t)vkp.%s(%s);\n' % (n, ', '.join(call)))
            w('\n'.join(post) + ('\n' if post else ''))
            w('    vk_arena_free(&a); VK_TRACE("%s", r); RET(r, %d);\n}\n' % (n, dw))
        else:
            raise SystemExit('%s: return type %s' % (n, ret))
    # the dispatch table of host functions
    w('\nstruct VkHostProcs vkp;\nconst char *const vk_proc_names[] = {\n')
    for n in want: w('    "%s",\n' % n)
    w('    "vkMapMemory2KHR", "vkUnmapMemory2KHR", "vkGetPhysicalDeviceProperties2", "vkGetPhysicalDeviceFeatures2", 0};\n')
    open(out, 'w').write(''.join(o))
    # the header with the dispatch table type
    hdr = ['/* Generated by tools/gen_vulkan.py: host Vulkan entry points the thunks call. */\n',
           'struct VkHostProcs {\n']
    for n in want + ['vkMapMemory2KHR', 'vkUnmapMemory2KHR', 'vkGetPhysicalDeviceProperties2', 'vkGetPhysicalDeviceFeatures2']:
        hdr.append('    PFN_%s %s;\n' % (n, n))
    hdr.append('};\nextern struct VkHostProcs vkp;\nextern const char *const vk_proc_names[];\n')
    hdr += protos
    for t in order: hdr.append('#define VKG_%s %d\n' % (t, layout(t)[0]))
    open(out.replace('.c', '.h'), 'w').write(''.join(hdr))
    print('%d commands (%d by hand), %d structures (%d converted)' % (len(want), len([n for n in want if n in MANUAL]), len(need), len([t for t in need if not blittable(t)])))

if __name__ == '__main__': main(*sys.argv[1:4])
