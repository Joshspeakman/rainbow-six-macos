/* sdp_glsl_check: translate every unique shader in Fallout NV's .sdp packages to GLSL with the vendored MojoShader
 * and compile it in a real OpenGL compatibility-profile context (a hidden SDL window). Reports translation and
 * compile failures; with -v, the first errors of each.
 *   cc -O1 -I third_party/mojoshader tools/sdp_glsl_check.c <mojoshader objects> $(pkg-config --cflags --libs sdl2 glew) -lm
 *   ./sdp_glsl_check [-v] [-p profile] <game>/Data/Shaders/*.sdp */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <GL/glew.h>
#include <SDL2/SDL.h>
#include "mojoshader.h"

static uint64_t fnv1a(const uint8_t *p, size_t n) { uint64_t h = 1469598103934665603ull; while (n--) h = (h ^ *p++) * 1099511628211ull; return h; }
int main(int argc, char **argv)
{
    int verbose = 0, a = 1; const char *profile = MOJOSHADER_PROFILE_GLSL120;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-v")) verbose = 1;
        else if (!strcmp(argv[a], "-p") && a + 1 < argc) profile = argv[++a];
    }
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_Window *w = SDL_CreateWindow("sdp_glsl_check", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext gl = w ? SDL_GL_CreateContext(w) : 0;
    if (!gl) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 1; }
    glewExperimental = GL_TRUE; glewInit();
    printf("GL %s, GLSL %s, profile %s\n", glGetString(GL_VERSION), glGetString(GL_SHADING_LANGUAGE_VERSION), profile);
    static uint64_t seen[65536]; int nseen = 0, total = 0, uniq = 0, bad_tr = 0, bad_cc = 0;
    for (; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb"); if (!fp) continue;
        fseek(fp, 0, SEEK_END); long n = ftell(fp); rewind(fp);
        uint8_t *buf = malloc(n); if (fread(buf, 1, n, fp) != (size_t)n) { fclose(fp); continue; } fclose(fp);
        uint32_t cnt; memcpy(&cnt, buf + 4, 4); long off = 12;
        for (uint32_t k = 0; k < cnt && off + 0x104 <= n; k++) {
            char name[0x101]; memcpy(name, buf + off, 0x100); name[0x100] = 0;
            for (int j = 0; j < 0x100; j++) if ((uint8_t)name[j] == 0xfd) { name[j] = 0; break; }
            uint32_t sz; memcpy(&sz, buf + off + 0x100, 4); const uint8_t *code = buf + off + 0x104; off += 0x104 + sz; total++;
            uint64_t h = fnv1a(code, sz); int dup = 0;
            for (int s = 0; s < nseen; s++) if (seen[s] == h) { dup = 1; break; }
            if (dup) continue;
            if (nseen < 65536) seen[nseen++] = h;
            uniq++;
            const MOJOSHADER_parseData *pd = MOJOSHADER_parse(profile, "main", code, sz, 0, 0, 0, 0, 0, 0, 0);
            if (pd->error_count) { bad_tr++; if (verbose) printf("TRANSLATE %s: %s\n", name, pd->errors[0].error); MOJOSHADER_freeParseData(pd); continue; }
            GLuint sh = glCreateShader(pd->shader_type == MOJOSHADER_TYPE_PIXEL ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
            const GLchar *src = pd->output; GLint len = pd->output_len;
            glShaderSource(sh, 1, &src, &len); glCompileShader(sh);
            GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                bad_cc++;
                if (verbose) { char log[2048]; glGetShaderInfoLog(sh, sizeof log, 0, log); printf("COMPILE %s: %.300s\n", name, log); }
            }
            glDeleteShader(sh); MOJOSHADER_freeParseData(pd);
        }
        free(buf);
    }
    printf("%d entries, %d unique shaders: %d failed to translate, %d failed to compile, %d ok\n", total, uniq, bad_tr, bad_cc, uniq - bad_tr - bad_cc);
    return 0;
}
