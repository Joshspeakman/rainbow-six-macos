/* undname_test <reference.txt>: compare w32_undname with UnDecorateSymbolName output (name \t len \t result) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *w32_undname(const char *);
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "r"); char line[8192]; int n = 0, ok = 0, shown = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0; char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue;
        const char *want = t2 + 1, *name = line; n++;
        char *got = w32_undname(name);
        const char *g = got ? got : name;                 /* not understood: undname returns the input */
        if (!strcmp(g, want)) ok++;
        else if (shown++ < 15) printf("MISMATCH %s\n   want: [%s]\n   got:  [%s]\n", name, want, g);
        free(got);
    }
    printf("%d/%d identical\n", ok, n);
    return ok != n;
}
