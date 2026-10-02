// Host twin of the runtime layout gate: reads the tables layout_gate_host.py resolved from the EXTRACTED family binary and OUR linked kext, and runs
// n48accel::gate_compare -- the very function Navi48Accel.cpp runs in probe() -- printing MATCH / FAIL per class.
//   input, per class:  "class <ours> <family> <nslots> <textLo> <textHi> <novr>"  then novr slot numbers, then nslots+1 "ours" values, then nslots+1 "family" values
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "n48accel_pure.h"
int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "r") : stdin;
    if (!f) { fprintf(stderr, "cannot open input\n"); return 2; }
    char tag[16], ours[64], fam[64];
    int bad = 0, classes = 0; unsigned total = 0;
    unsigned long long lo, hi; unsigned n, novr;
    while (fscanf(f, "%15s %63s %63s %u %llx %llx %u", tag, ours, fam, &n, &lo, &hi, &novr) == 7) {
        uint16_t *ovr = (uint16_t *)calloc(novr + 1, sizeof(uint16_t));
        uintptr_t *o = (uintptr_t *)calloc(n + 1, sizeof(uintptr_t)), *fm = (uintptr_t *)calloc(n + 1, sizeof(uintptr_t));
        for (unsigned i = 0; i < novr; ++i) { unsigned v; if (fscanf(f, "%u", &v) != 1) return 2; ovr[i] = (uint16_t)v; }
        for (unsigned i = 0; i <= n; ++i) { unsigned long long v; if (fscanf(f, "%llx", &v) != 1) return 2; o[i] = (uintptr_t)v; }
        for (unsigned i = 0; i <= n; ++i) { unsigned long long v; if (fscanf(f, "%llx", &v) != 1) return 2; fm[i] = (uintptr_t)v; }
        const n48accel::GateResult r = n48accel::gate_compare(o, fm, n, ovr, novr, (uintptr_t)lo, (uintptr_t)hi);
        ++classes;
        if (r.v == n48accel::kGateOk) { printf("MATCH  %-22s vs %-26s %3u slots, %2u overrides, %3u inherited (exact address equality)\n", ours, fam, n, novr, n - novr); total += r.compared; }
        else { printf("FAIL   %-22s vs %-26s slot %u verdict %u ours %#llx family %#llx\n", ours, fam, r.slot, (unsigned)r.v, (unsigned long long)r.ours, (unsigned long long)r.fam); ++bad; }
        free(ovr); free(o); free(fm);
    }
    printf("%s: %d classes, %u slots compared\n", bad ? "MISMATCH" : "ALL MATCH", classes, total);
    return bad ? 1 : 0;
}
