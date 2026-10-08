"""gen_imptab.py <out.c> <files...>: table of implemented imports from IMPL(dll, name) definitions."""
import sys, re
names = []
for p in sys.argv[2:]:
    for m in re.finditer(r'^IMPL\((\w+),\s*(\w+)\)', open(p).read(), re.M): names.append((m.group(1).lower(), m.group(2)))
names.sort()
with open(sys.argv[1], 'w') as o:
    o.write('#include "w32.h"\n')
    for d, n in names: o.write('void imp_%s_%s(Ctx *c);\n' % (d, n))
    o.write('const struct impdef { const char *name; void (*fn)(Ctx *); } w32_impls[] = {\n')
    for d, n in names: o.write('    {"%s!%s", imp_%s_%s},\n' % (d, n, d, n))
    o.write('};\nconst unsigned w32_nimpls = %d;\n' % len(names))
print('%d imports implemented' % len(names))
