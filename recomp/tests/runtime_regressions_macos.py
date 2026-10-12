#!/usr/bin/env python3
"""Run synthetic runtime checks with ASan/UBSan on macOS; no game assets."""
from pathlib import Path
import json, os, subprocess, tempfile, sys
ROOT = Path(__file__).resolve().parents[2]

def check(repo):
    name = repo.name
    log = ROOT / 'build/runtime-regressions.log'
    log.parent.mkdir(parents=True, exist_ok=True)
    results = []
    with log.open('w') as out, tempfile.TemporaryDirectory(prefix='mac-runtime-') as temp:
        temp = Path(temp)
        runtime = repo / 'recomp/runtime'
        tests = repo / 'recomp/tests'
        common = ['clang', '-O1', '-g', '-ffp-contract=off', '-ffunction-sections', '-fdata-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-mllvm', '-asan-globals=0',
            '-I'+str(runtime), '-I'+str(runtime/'win32'), '-I'+str(repo/'recomp/third_party/vulkan/include')]
        cases = [('vm_gap', []), ('heap_lifecycle', ['heap','vm']), ('thread_lifecycle', ['core','threads','loader','heap','vm']),
            ('path_race',['files']), ('iface_lifecycle',['heap','vm']), ('dp8_large',['dp8proto']),
            ('message_queue',[]), ('pixel_conversion',[]), ('audio_mix',[]),
            ('vulkan_lifecycle',['vulkan_gen','heap','vm']), ('vulkan_lookup',['vulkan_gen','heap','vm'])]
        fixture=temp/'dispatch_fixture.c'
        fixture.write_text('#include "rt.h"\nstatic void fn(Ctx *c){(void)c;}\nconst struct rt_fn {uint32_t addr;GuestFn fn;} rt_fntab[]={\n'+
            ''.join('{0x%x,fn},\n'%(0x400000+i*20 if i<32768 else 0x10000000+i*36) for i in range(65536))+
            '};\nconst unsigned rt_fntab_n=65536;\n')
        cases.append(('dispatch_perf',[]))
        if (tests / 'df_frame_test.c').exists(): cases.extend([('df_frame1',[]), ('df_frame2',[])])
        for case, sources in cases:
            base = 'df_frame' if case.startswith('df_frame') else case
            test = tests / (base+'_test.c')
            if not test.exists() or (len(sys.argv)>1 and case not in sys.argv[1:]): continue
            extra=[]
            if case=='audio_mix' and (runtime/'win32/eax_reverb.c').exists(): extra.append(str(runtime/'win32/eax_reverb.c'))
            if case=='dispatch_perf': extra += [str(runtime/'rt.c'), str(fixture)]
            if case.startswith('df_frame'): extra += ['-DDF_EDITION','-DDF'+case[-1]+'_EDITION']
            exe=temp/case
            cmd=common+[str(test)]+[str(runtime/'win32'/(s+'.c')) for s in sources]+extra+[
                '-Wl,-dead_strip','-pthread','-lm','-framework','AudioToolbox','-o',str(exe)]
            out.write('\nCASE '+case+'\n'+' '.join(cmd)+'\n'); out.flush()
            try:
                subprocess.run(cmd,stdout=out,stderr=out,check=True,timeout=180)
                env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
                for mode in (['0','1'] if case=='dispatch_perf' else ['normal','drop'] if case=='dp8_large' else ['normal']):
                    e=dict(env)
                    if case=='dispatch_perf': e['RT_DISPATCH_CACHE']=mode
                    if mode=='drop': e['DP8_DROP']='10'
                    subprocess.run([str(exe)],stdout=out,stderr=out,check=True,timeout=180,env=e)
                results.append({'case':case,'passed':True})
            except Exception as e:
                results.append({'case':case,'passed':False,'error':str(e)})
        out.flush()
    return name,results


if __name__ == "__main__":
    name, results = check(ROOT)
    for result in results: print(result["case"], "PASS" if result["passed"] else "FAIL: " + result["error"])
    print("Details: build/runtime-regressions.log")
    sys.exit(not all(result["passed"] for result in results))
