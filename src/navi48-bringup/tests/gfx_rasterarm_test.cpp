// gfx_rasterarm_test.cpp — 0.0.389 (notes 880 H6 (e), 881): Gap 1's switch, offline.
//
// TWO THINGS ARE CHECKED AND THEY ARE DIFFERENT KINDS OF FACT.
//
//   1. THE ARITHMETIC (gfx_rasterarm.h): OFF returns zero for both fields, so the armed path's `ex` is byte for
//      byte what 0.0.388 built; ON returns exactly the caller's XLAT12_EXTRA_RASTER and XLAT12_RSRC3_GS_CU_EN and
//      nothing else - in particular NEVER XLAT12_EXTRA_PREAMBLE. The mode byte writes the flag only for 1 and 2.
//
//   2. THE CALL SITE (src/apple/AppleHardwareHook.cpp, passed as argv[1]): that `gfxsrc_policy` really does OR the
//      helper's flags into the `ex` it hands the translator and really does assign the helper's rsrc3_gs, and that
//      the `gfx-xlat:` frame line really does print both. A header nobody calls is the failure mode this project
//      has paid for ('s phantom lever), so the wiring is checked as text against the real file. Run without an
//      argument, section 2 is SKIPPED and says so - it never silently passes.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). SEVEN mutants.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 src/navi48-bringup/tests/gfx_rasterarm_test.cpp \
//         -o /tmp/ratest && /tmp/ratest src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include "gfx_rasterarm.h"
#include "xlat12_ib.h"

static int gFail = 0, gRun = 0, gQuiet = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) printf("  FAIL %-64s got %#llx want %#llx\n", what, (unsigned long long)got,
                            (unsigned long long)want);
    } else if (!gQuiet) printf("  ok   %-64s %#llx\n", what, (unsigned long long)got);
}

// ---------------------------------------------------------------------------------------------------------------------
// The mutable copy of the two rules, so a planted defect can be run against the SAME checks.
// ---------------------------------------------------------------------------------------------------------------------
static int gD = 0;   // 0 = the real rules

static uint32_t ra_flags(uint32_t on, uint32_t raster)
{
    switch (gD) {
    case 1: return raster;                                   // ignores the switch: always on
    case 2: return 0u;                                       // ignores the switch: never on
    case 3: return on ? (raster | XLAT12_EXTRA_PREAMBLE) : 0u;  // also sets the preamble delta
    default: return n48_ra_flags(on, raster);
    }
}
static uint32_t ra_rsrc3(uint32_t on, uint32_t v)
{
    switch (gD) {
    case 4: return v;                                        // ignores the switch
    case 5: return on ? XLAT12_EXTRA_RASTER : 0u;            // the wrong constant
    default: return n48_ra_rsrc3(on, v);
    }
}
static int ra_set(uint32_t m, uint32_t *f)
{
    switch (gD) {
    case 6: if (f) *f = (m == 1u) ? 1u : 0u; return 1;       // treats every mode byte as a write (a bare read turns it OFF)
    case 7: if (f) *f = 1u; return (m == 1u || m == 2u);     // mode 2 turns it ON
    default: return n48_ra_set(m, f);
    }
}

static void rule_checks()
{
    ck("OFF: flags are 0 (0.0.388's armed path, dword for dword)", ra_flags(0u, XLAT12_EXTRA_RASTER), 0u);
    ck("OFF: rsrc3_gs is 0", ra_rsrc3(0u, XLAT12_RSRC3_GS_CU_EN), 0u);
    ck("ON: flags are exactly XLAT12_EXTRA_RASTER", ra_flags(1u, XLAT12_EXTRA_RASTER), XLAT12_EXTRA_RASTER);
    ck("ON: the preamble delta is NOT set", ra_flags(1u, XLAT12_EXTRA_RASTER) & XLAT12_EXTRA_PREAMBLE, 0u);
    ck("ON: the per-draw raster flag is NOT set", ra_flags(1u, XLAT12_EXTRA_RASTER) & XLAT12_EXTRA_RASTER_PER_DRAW, 0u);
    ck("ON: rsrc3_gs is the clean controls' 0xfffffdfd", ra_rsrc3(1u, XLAT12_RSRC3_GS_CU_EN), XLAT12_RSRC3_GS_CU_EN);
    ck("the value comes from xlat12_ib.h, not a copy here", XLAT12_RSRC3_GS_CU_EN, 0xFFFFFDFDu);
    ck("XLAT12_EXTRA_RASTER is 0x4 (r64's `flags 0x4`)", XLAT12_EXTRA_RASTER, 0x4u);
    // the mode byte
    { uint32_t f = 0u;
      ck("mode 1 writes the flag", (uint64_t)ra_set(1u, &f), 1u);
      ck("mode 1 leaves it ON", f, 1u);
      ck("mode 2 writes the flag", (uint64_t)ra_set(2u, &f), 1u);
      ck("mode 2 leaves it OFF", f, 0u);
      f = 1u;
      ck("a bare read does not write", (uint64_t)ra_set(0u, &f), 0u);
      ck("a bare read leaves an ON switch ON", f, 1u);
      ck("mode 3 does not write (26's M 3 must not reach 27)", (uint64_t)ra_set(3u, &f), 0u);
      ck("mode 3 leaves it unchanged", f, 1u);
      ck("mode 255 does not write", (uint64_t)ra_set(255u, &f), 0u); }
    // the verb's scalar: 283 = 27 | 1 << 8, and the selector byte is 27
    ck("283 & 0xff == 27", 283u & 0xffu, 27u);
    ck("283 >> 8 == 1 (ON)", 283u >> 8, 1u);
    ck("539 >> 8 == 2 (OFF)", 539u >> 8, 2u);
    // 0.0.389: the two blocks' dword costs, so a room calculation in the notes cannot drift from the code
    ck("the raster block is 12 dwords", XLAT12_RASTER_DWORDS, 12u);
    ck("PA_SC_HISZ_CONTROL sits at 0x28bbc (ctx 0x2ef)", XLAT12_G12_PA_SC_HISZ_CONTROL, 0x28000u + 0x2efu * 4u);
    ck("PA_SC_HISZ_CONTROL = ROUND(2)", XLAT12_PA_SC_HISZ_CONTROL, 2u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 2. THE CALL SITE. Text anchors against the real AppleHardwareHook.cpp. Not mutated - it is a fact about that file.
// ---------------------------------------------------------------------------------------------------------------------
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

static void site_checks(const char *path)
{
    long n = 0;
    char *src = slurp(path, &n);
    if (!src) { printf("  SKIP section 2: cannot read %s - the call site is NOT checked\n", path); gFail++; gRun++; return; }
    ck("the hook includes gfx_rasterarm.h", count_of(src, "#include \"gfx_rasterarm.h\""), 1u);
    ck("the armed path ORs the helper's flags into ex", count_of(src, "ex.flags   |= n48_ra_flags(raOn, XLAT12_EXTRA_RASTER);"), 1u);
    ck("the armed path assigns the helper's rsrc3_gs", count_of(src, "ex.rsrc3_gs = n48_ra_rsrc3(raOn, XLAT12_RSRC3_GS_CU_EN);"), 1u);
    ck("the frame line prints the flags in force", count_of(src, "n48_ra_flags(gRaOn, XLAT12_EXTRA_RASTER), n48_ra_rsrc3(gRaOn, XLAT12_RSRC3_GS_CU_EN),"), 1u);
    ck("the verb selector 27 exists", count_of(src, "(arg & 0xffull) == 27ull"), 1u);
    ck("the verb uses n48_ra_set", count_of(src, "n48_ra_set(m, &f)"), 1u);
    ck("there is exactly one writer of gRaOn", count_of(src, "gRaOn = f;"), 1u);
    ck("the report line exists and is printed by the common report",
       count_of(src, "ra_report_line(\"gfxneuter report\");"), 1u);
    ck("the armed path never sets the preamble delta", count_of(src, "ex.flags |= XLAT12_EXTRA_PREAMBLE"), 0u);
    free(src);
}

int main(int argc, char **argv)
{
    printf("gfx_rasterarm (0.0.389, notes 881): Gap 1 on the armed path\n");
    rule_checks();
    if (argc > 1) site_checks(argv[1]);
    else printf("  SKIP section 2 (no AppleHardwareHook.cpp argument): THE CALL SITE IS NOT CHECKED\n");
    const int base = gFail;
    const uint32_t baseRun = (uint32_t)gRun;
    // planted defects
    static const char *kD[] = { "", "flags ignore the switch (always on)", "flags ignore the switch (never on)",
                                "flags also set the preamble delta", "rsrc3_gs ignores the switch",
                                "rsrc3_gs uses the wrong constant", "a bare read is treated as a write",
                                "mode 2 turns the switch ON" };
    uint32_t caught = 0;
    gQuiet = 1;
    printf("  planted defects:\n");
    for (int d = 1; d <= 7; d++) {
        gD = d; gFail = 0; gRun = 0;
        rule_checks();
        gD = 0;
        if (gFail) caught++;
        printf("    D%d %-52s %s (%d check(s) fail)\n", d, kD[d], gFail ? "CAUGHT" : "*** NOT CAUGHT ***", gFail);
    }
    gQuiet = 0;
    printf("\nchecks run %u, failures %d; mutants caught %u/7\n", baseRun, base, caught);
    const int ok = (base == 0) && (caught == 7u);
    printf("gfx_rasterarm: %s\n", ok ? "N48-RASTERARM-TEST-PASS" : "N48-RASTERARM-TEST-FAIL");
    return ok ? 0 : 1;
}
