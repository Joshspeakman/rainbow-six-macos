"""import_gap.py <image>... : the image's imports that no IMPL(dll, name) in runtime/win32 implements (names
canonicalised as loader.c does: lower-case DLL, no stdcall decoration, ordinals as ordN)."""
import sys, os, re, glob, collections
sys.path.insert(0, os.path.dirname(__file__)); from pe import PE
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
impl = set()
for f in glob.glob(os.path.join(HERE, 'runtime', 'win32', '*.c')):
    for d, n in re.findall(r'^IMPL\((\w+),\s*(\w+)\)', open(f, errors='replace').read(), re.M): impl.add('%s!%s' % (d.lower(), n))
def canon(dll, fn):
    d = dll.split('.')[0].lower().replace('-', '_')
    if d.startswith('d3dx9_'): d = 'd3dx9_29'                  # loader.c: one D3DX9 for every version
    if fn.startswith('#'): return '%s!ord%s' % (d, fn[1:])
    if fn[:4] in ('??2@', '??3@'): return '%s!%s' % (d, 'op_new' if fn[2] == '2' else 'op_delete')
    if fn.startswith('_') and '@' in fn: fn = fn[1:]
    return '%s!%s' % (d, fn.split('@')[0])
skip = set(a.lower() for a in os.environ.get('GAP_SKIP_DLLS', '').split(',') if a)
for p in sys.argv[1:]:
    pe = PE(p); by = collections.defaultdict(list)
    for va, name in pe.imports.items():
        dll, fn = name.split('!', 1)
        if dll.lower() in skip: continue
        if canon(dll, fn) not in impl: by[dll].append(fn)
    print('== %s: %d imports, %d not implemented' % (os.path.basename(p), len(pe.imports), sum(map(len, by.values()))))
    for d in sorted(by): print('  %-18s %3d  %s' % (d, len(by[d]), ' '.join(sorted(by[d]))))
