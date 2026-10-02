// gfx_rasterarm_pd_test.cpp — build 0.0.453 item 2 (Fix D, inv-f84/REPORT.txt): switch 50, a SEPARATE
// switch from switch 27 (gfx_rasterarm.h / gfx_rasterarm_test.cpp).
//
// WHY A SEPARATE TEST FILE, NOT AN EXTRA CHECK IN gfx_rasterarm_test.cpp: that file's site_checks() asserts, as an
// invariant of switch 27 ITSELF, that the armed path's two lines (`ex.flags |= n48_ra_flags(raOn, XLAT12_EXTRA_RASTER);`
// and `ex.rsrc3_gs = n48_ra_rsrc3(raOn, XLAT12_RSRC3_GS_CU_EN);`) appear EXACTLY ONCE and unchanged, and that ON,
// XLAT12_EXTRA_RASTER_PER_DRAW is NOT set. Folding Fix D into switch 27 would fail that existing, already-hardware-
// validated test and silently redefine what "switch 27 ON" has always meant. The underlying translator mechanism
// (XLAT12_EXTRA_RASTER_PER_DRAW) is already exhaustively host-tested in src/xlat12/tests/test_xlat12_ib.c's
// test_raster_per_draw) — including the exact "planted break: the per-translation rule
// restored" comparison the build brief for item 2 asks for (its cases A/C/D run the SAME two-render-pass and
// one-draw-double-VIEW streams through the OLD per-translation rule (`exR`) and the per-draw rule (`exRD`) side by
// side, and show the old rule accepting exactly the shapes-f84's Fix D exists to refuse-or-admit correctly).
// This file only checks the KEXT WIRES the flag — the same "a header nobody calls is the failure mode this project
// has paid for" check gfx_rasterarm_test.cpp performs for switch 27, done here for switch 50.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 src/navi48-bringup/tests/gfx_rasterarm_pd_test.cpp \
//         -o /tmp/rpdtest && /tmp/rpdtest src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

static int gFail = 0, gRun = 0;

static void ck(const char *what, int got)
{
    gRun++;
    if (!got) { gFail++; printf("  FAIL %s\n", what); } else printf("  ok   %s\n", what);
}

static char *slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return nullptr; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return nullptr; }
    b[n] = 0; fclose(f); if (len) *len = n;
    return b;
}

static uint32_t count_of(const char *hay, const char *needle)
{
    uint32_t c = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle)) c++;
    return c;
}

int main(int argc, char **argv)
{
    printf("gfx_rasterarm_pd (0.0.453 item 2, Fix D): switch 50, wired separately from switch 27\n");
    if (argc < 2) { printf("  SKIP: no AppleHardwareHook.cpp argument - THE CALL SITE IS NOT CHECKED\n"); return 1; }
    long n = 0;
    char *src = slurp(argv[1], &n);
    if (!src) { printf("  FAIL: cannot read %s\n", argv[1]); return 1; }

    // switch 27's own two lines are UNTOUCHED - exactly one occurrence each, byte for byte.
    ck("switch 27's flags line is unchanged and appears exactly once",
       count_of(src, "ex.flags   |= n48_ra_flags(raOn, XLAT12_EXTRA_RASTER);") == 1u);
    ck("switch 27's rsrc3_gs line is unchanged and appears exactly once",
       count_of(src, "ex.rsrc3_gs = n48_ra_rsrc3(raOn, XLAT12_RSRC3_GS_CU_EN);") == 1u);

    // switch 50 is a SEPARATE block that sets BOTH flags together, and assigns rsrc3_gs AFTER switch 27's own line
    // (so switch 27 OFF + switch 50 ON is not clobbered back to 0 by switch 27's unconditional assignment).
    ck("gfxsrc_policy ORs in BOTH XLAT12_EXTRA_RASTER and XLAT12_EXTRA_RASTER_PER_DRAW under switch 50",
       count_of(src, "ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW;") == 1u);
    ck("switch 50's rsrc3_gs assignment exists exactly once",
       count_of(src, "ex.rsrc3_gs = XLAT12_RSRC3_GS_CU_EN;\n                gRaPd.applied++;") == 1u);
    {
        const char *p27 = strstr(src, "ex.rsrc3_gs = n48_ra_rsrc3(raOn, XLAT12_RSRC3_GS_CU_EN);");
        const char *p50 = strstr(src, "ex.flags |= XLAT12_EXTRA_RASTER | XLAT12_EXTRA_RASTER_PER_DRAW;");
        ck("switch 50's block comes AFTER switch 27's rsrc3_gs assignment in gfxsrc_policy",
           p27 && p50 && p50 > p27);
    }
    ck("the verb selector 50 exists", count_of(src, "(arg & 0xffull) == 50ull") == 1u);
    // build 0.0.453 item 2: a DISTINCT local name (`fpd`, not `f`) so this call does not collide with
    // gfx_rasterarm_test.cpp's own "the verb uses n48_ra_set" text anchor (which counts "n48_ra_set(m, &f)" and
    // must stay at exactly 1 - switch 27's own call).
    ck("the verb reuses n48_ra_set (the same generic mode-byte rule as switch 27), with its OWN local",
       count_of(src, "n48_ra_set(m, &fpd)") == 1u);
    ck("switch 27's own n48_ra_set anchor is undisturbed (still exactly 1 occurrence of &f, not &fpd)",
       count_of(src, "n48_ra_set(m, &f)") == 1u);
    ck("there is exactly one writer of gRaPerDrawOn", count_of(src, "gRaPerDrawOn = fpd;") == 1u);
    ck("the report line is printed by the common report",
       count_of(src, "rd_report_line(\"gfxneuter report\");") == 1u);
    ck("switch 50 is declared OFF by default", count_of(src, "static volatile uint32_t gRaPerDrawOn { 0u };") == 1u);

    free(src);
    printf("\nchecks run %d, failures %d\n", gRun, gFail);
    const int ok = (gFail == 0);
    printf("gfx_rasterarm_pd: %s\n", ok ? "N48-RASTERARM-PD-TEST-PASS" : "N48-RASTERARM-PD-TEST-FAIL");
    return ok ? 0 : 1;
}
