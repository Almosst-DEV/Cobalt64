// test-xlate-corpus.c: the driver of test-xlate-corpus.sh (T3). Runs the REAL reader-loading code (n48_xlate.h: dlopen of an LLVM library, ten functions) and the REAL engine
// (worker thread on 64 MiB, crash markers, caps, atomic install) over a corpus of AIR bitcode with the REAL libn48xlate, and leaves the results in a directory for the comparer.
//   test-xlate-corpus run <corpus dir> <out dir> <libn48xlate.dylib> <reader: apple | path to a libLLVM.dylib> [overrides.txt]
//   test-xlate-corpus ll  <corpus dir> <out dir> <reader>       (only the reader: writes <sha>.ll, for the text comparison)
// <corpus dir> holds manifest.tsv (sha, function name, set, class, size) and <sha>.air.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "n48_xlate.h"

static uint8_t *readall(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)l + 1); if (!b || fread(b, 1, (size_t)l, f) != (size_t)l) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)l; return b;
}
int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: %s run|ll corpus out lib reader\n", argv[0]); return 2; }
    const int ll = !strcmp(argv[1], "ll");
    const char *corpus = argv[2], *out = argv[3], *lib = argv[4], *reader = argv[5];
    (void)mkdir(out, 0700);
    char mp[1100]; snprintf(mp, sizeof mp, "%s/manifest.tsv", corpus);
    FILE *m = fopen(mp, "r"); if (!m) { perror(mp); return 2; }
    const char *ov = !strcmp(reader, "apple") ? NULL : reader;
    n48x_engine eng; n48x_reader rd; char err[512];
    if (ll) { if (n48x_reader_load(&rd, N48X_GPUC_BASE, ov, err, sizeof err) != 0) { fprintf(stderr, "reader: %s\n", err); return 2; } fprintf(stderr, "reader: %s\n", rd.path); }
    else n48x_engine_init(&eng, NULL, N48X_GPUC_BASE, ov, lib, N48X_WORKER_STACK);
    char rp[1100]; snprintf(rp, sizeof rp, "%s/results.tsv", out);
    FILE *res = fopen(rp, "w");
    char line[1024]; int n = 0;
    while (fgets(line, sizeof line, m)) {
        char *f[5]; int k = 0; char *c = line;
        for (; k < 5; k++) { f[k] = c; char *t = strchr(c, '\t'); if (!t) { char *nl = strchr(c, '\n'); if (nl) *nl = 0; break; } *t = 0; c = t + 1; }
        char ap[1100]; snprintf(ap, sizeof ap, "%s/%s.air", corpus, f[0]);
        size_t an = 0; uint8_t *air = readall(ap, &an);
        if (!air) { fprintf(res, "%s\t-1\tno air\n", f[0]); continue; }
        err[0] = 0; int rc;
        if (ll) {
            char *text = NULL; size_t tl = 0;
            rc = n48x_reader_print(&rd, air, an, &text, &tl, err, sizeof err);
            if (rc == 0) { char lp[1100]; snprintf(lp, sizeof lp, "%s/%s.ll", out, f[0]); FILE *o = fopen(lp, "wb"); if (o) { fwrite(text, 1, tl, o); fclose(o); } free(text); }
        } else {
            rc = n48x_engine_run(&eng, out, f[0], f[1], air, an, 600000, err, sizeof err);
        }
        for (char *q = err; *q; q++) if (*q == '\t' || *q == '\n') *q = ' ';
        fprintf(res, "%s\t%d\t%s\n", f[0], rc, err);
        free(air); n++;
    }
    fclose(res); fclose(m);
    fprintf(stderr, "%s: %d inputs\n", argv[1], n);
    return 0;
}
