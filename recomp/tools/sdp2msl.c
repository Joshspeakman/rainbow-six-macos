/* sdp2msl: translate every shader in Fallout NV .sdp packages to MSL with MojoShader; report failures, write MSL. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "mojoshader.h"
int main(int argc, char **argv)
{
    const char *outdir = argv[1]; int ok = 0, bad = 0, total = 0;
    for (int a = 2; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb"); if (!fp) continue;
        fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp);
        uint8_t *buf = malloc(n); fread(buf, 1, n, fp); fclose(fp);
        uint32_t cnt; memcpy(&cnt, buf + 4, 4); long off = 12;
        for (uint32_t k = 0; k < cnt && off + 0x104 <= n; k++) {
            char name[0x101]; memcpy(name, buf + off, 0x100); name[0x100] = 0; for (int j = 0; j < 0x100; j++) if ((uint8_t)name[j] == 0xfd) { name[j] = 0; break; }
            uint32_t sz; memcpy(&sz, buf + off + 0x100, 4); const uint8_t *code = buf + off + 0x104; off += 0x104 + sz;
            total++;
            const MOJOSHADER_parseData *pd = MOJOSHADER_parse(MOJOSHADER_PROFILE_METAL, "main", code, sz, 0, 0, 0, 0, 0, 0, 0);
            if (pd->error_count) { bad++; printf("FAIL %s %s: %s\n", strrchr(argv[a], '/') + 1, name, pd->errors[0].error); }
            else {
                ok++;
                char p[1024]; const char *base = strrchr(argv[a], '/'); snprintf(p, sizeof p, "%s/%s_%s.metal", outdir, base ? base + 1 : argv[a], name);
                FILE *o = fopen(p, "w"); if (o) { fwrite(pd->output, 1, pd->output_len, o); fclose(o); }
            }
            MOJOSHADER_freeParseData(pd);
        }
        free(buf);
    }
    printf("%d shaders: %d translated to MSL, %d failed\n", total, ok, bad);
    return 0;
}
