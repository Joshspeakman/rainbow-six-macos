/* audiocheck <reference.wav> <output.wav> <seconds>... : verify recorded game audio (W32_AUDIO_DUMP) against an
   independent decode of the same file: find where test's windows at t1/t2 s (3 s long) occur in ref; print lag, correlation,
   gain and residual SNR after the best scale. 16-bit stereo 44.1 kHz WAV (44-byte header). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static float *load(const char *p, long *n)
{
    FILE *f = fopen(p, "rb"); fseek(f, 0, SEEK_END); long b = ftell(f) - 44; fseek(f, 44, SEEK_SET);
    short *s = malloc(b); fread(s, 1, b, f); fclose(f); *n = b / 4; float *m = malloc(sizeof(float) * *n);
    for (long i = 0; i < *n; i++) m[i] = (s[2 * i] + s[2 * i + 1]) / 65536.0f; free(s); return m;
}
int main(int argc, char **argv)
{
    long nr, nt; float *r = load(argv[1], &nr), *t = load(argv[2], &nt);
    const int D = 16, W = 3 * 44100;
    long ndr = nr / D; float *rd = calloc(ndr, 4); for (long i = 0; i < ndr; i++) { for (int k = 0; k < D; k++) rd[i] += r[i * D + k]; rd[i] /= D; }
    for (int a = 3; a < argc; a++) {
        long t0 = (long)(atof(argv[a]) * 44100); if (t0 + W > nt) { printf("t=%s beyond test\n", argv[a]); continue; }
        int wd = W / D; float *td = calloc(wd, 4); for (int i = 0; i < wd; i++) { for (int k = 0; k < D; k++) td[i] += t[t0 + i * D + k]; td[i] /= D; }
        double tt = 0; for (int i = 0; i < wd; i++) tt += td[i] * td[i];
        double best = -1; long bl = 0;
        for (long l = 0; l + wd < ndr; l++) {
            double xy = 0, yy = 0; for (int i = 0; i < wd; i++) { xy += td[i] * rd[l + i]; yy += rd[l + i] * rd[l + i]; }
            double cc = xy / sqrt(tt * yy + 1e-20); if (cc > best) { best = cc; bl = l; }
        }
        /* refine at full rate */
        double fb = -1; long fl = 0;
        for (long l = bl * D - 64; l <= bl * D + 64; l++) {
            if (l < 0 || l + W > nr) continue;
            double xy = 0, xx = 0, yy = 0; for (int i = 0; i < W; i++) { xy += t[t0 + i] * r[l + i]; xx += t[t0 + i] * t[t0 + i]; yy += r[l + i] * r[l + i]; }
            double cc = xy / sqrt(xx * yy + 1e-20); if (cc > fb) { fb = cc; fl = l; }
        }
        double xy = 0, yy = 0, xx = 0; for (int i = 0; i < W; i++) { xy += t[t0 + i] * r[fl + i]; yy += r[fl + i] * r[fl + i]; xx += t[t0 + i] * t[t0 + i]; }
        double g = xy / yy, e = 0; for (int i = 0; i < W; i++) { double d = t[t0 + i] - g * r[fl + i]; e += d * d; }
        printf("test %6.2f s  ->  ref %8.4f s   offset %+9.4f s   corr %.5f   gain %.4f (%.1f dB)   residual SNR %.1f dB\n",
               t0 / 44100.0, fl / 44100.0, (fl - t0) / 44100.0, fb, g, 20 * log10(fabs(g)), 10 * log10(xx / (e + 1e-20)));
    }
}
