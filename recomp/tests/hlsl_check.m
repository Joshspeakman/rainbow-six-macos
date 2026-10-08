// hlsl_check <file.fx>...: parses each effect with runtime/win32/hlsl.c, compiles every pass to Metal and has the Metal
// compiler build it (the way the runtime will). Prints the techniques, passes and any errors (HLSL_DUMP=<technique> prints
// that technique's source); -v dumps the source of a
// failing pass. Build: clang -fobjc-arc -framework Metal -framework Foundation -I runtime/win32 tests/hlsl_check.m runtime/win32/hlsl.c
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "hlsl.h"

int main(int argc, char **argv)
{
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); int verbose = 0, bad = 0, total = 0;
        for (int a = 1; a < argc; a++) {
            if (!strcmp(argv[a], "-v")) { verbose = 1; continue; }
            NSData *d = [NSData dataWithContentsOfFile:@(argv[a])]; if (!d) { printf("%s: unreadable\n", argv[a]); bad++; continue; }
            HFx *fx = hlsl_parse(d.bytes, d.length);
            if (fx->err[0]) { printf("%s: parse error: %s\n", argv[a], fx->err); bad++; hlsl_free(fx); continue; }
            printf("%s: %d globals, %d structs, %d functions, %d techniques\n", argv[a], fx->nv, fx->ns, fx->nf, fx->nt);
            for (int t = 0; t < fx->nt; t++) for (int p = 0; p < fx->t[t].npass; p++) {
                total++;
                if (getenv("HLSL_DEBUG")) { fprintf(stderr, "compiling %s pass %d\n", fx->t[t].name, p); }
                HProgram *pr = hlsl_compile_pass(fx, t, p);
                if (getenv("HLSL_DEBUG")) fprintf(stderr, "  generated %zu bytes\n", pr->msl_len);
                NSString *err = nil;
                if (pr->err[0]) err = @(pr->err);
                else {
                    NSError *e = nil; MTLCompileOptions *o = [MTLCompileOptions new];
                    id<MTLLibrary> lib = [dev newLibraryWithSource:[[NSString alloc] initWithBytes:pr->msl length:pr->msl_len encoding:NSUTF8StringEncoding] options:o error:&e];
                    if (!lib || ![lib newFunctionWithName:@"vs_fx"] || ![lib newFunctionWithName:@"fs_fx"]) err = e.localizedDescription ?: @"functions missing";
                }
                printf("  %-40s pass %d: %s%s%s\n", fx->t[t].name, p, err ? "FAILED " : "ok", pr->vs_ffp ? " (fixed-function vertex)" : "", pr->ps_ffp ? " (fixed-function pixel)" : "");
                if (!err && getenv("HLSL_DUMP") && !strcmp(getenv("HLSL_DUMP"), fx->t[t].name)) printf("----- source -----\n%.*s\n------------------\n", (int)pr->msl_len, pr->msl);
                if (err) { bad++; printf("    %s\n", [[err componentsSeparatedByString:@"\n"] componentsJoinedByString:@"\n    "].UTF8String); if (verbose && pr->msl) printf("----- source -----\n%.*s\n------------------\n", (int)pr->msl_len, pr->msl); }
                hlsl_program_free(pr);
            }
            hlsl_free(fx);
        }
        printf("%d of %d passes failed\n", bad, total);
        return bad ? 1 : 0;
    }
}
