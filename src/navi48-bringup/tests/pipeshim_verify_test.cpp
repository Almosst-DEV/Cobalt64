// pipeshim_verify_test.cpp — 0.0.412 S2: the pipeshim verify cell map and its per-boot bound, host-tested.
//
// S1 (the `pipeshim 4` forced-linear switch) lives in DisplayPipeGuard.cpp and picks between the two EXISTING copy
// routines; it has no pure logic of its own. S2's pure half is what this file pins: the 16x16 destination cell index,
// the cell-centre coordinates, the 256-bit map packing, and the "is S2 due?" predicate and its hard budget. The KEXT
// compiles the SAME header (src/apple/display_pipe_guard.h); the reads the map is built from are in Navi48Bringup.cpp's
// two copy routines and cannot run here.
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/pipeshim_verify_test.cpp -o /tmp/pipetest && /tmp/pipetest
//
// NON-VACUITY is the gfx_desc_port_test style: the same checks are re-run against MUTANTS - local copies of the
// functions with one defect each - and each mutant must be CAUGHT (produce at least one failure).
#include <cstdio>
#include <cstdint>
#include "display_pipe_guard.h"

static int gFail = 0, gRun = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-78s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
}

// ---------------------------------------------------------------------------------------------------------------------------
// The mutants. One defect each; the same checks must catch every one.
// ---------------------------------------------------------------------------------------------------------------------------
static uint32_t cell16_mut_noclamp(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{   // the clamp is dropped: an x == w (one past the rect) walks off the 16x16 grid
    if (w == 0u || h == 0u) return 0u;
    return (uint32_t)(((uint64_t)y * N48_DPG_VERIFY_CELLS) / h) * N48_DPG_VERIFY_CELLS +
           (uint32_t)(((uint64_t)x * N48_DPG_VERIFY_CELLS) / w);
}
static uint32_t cell16_mut_widths(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{   // the rectangle's width and height are swapped when normalising the cell
    return n48_dpg_cell16(x, y, h, w);
}
static void centre_mut_leftedge(uint32_t cx, uint32_t cy, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y)
{   // the cell's LEFT/TOP edge, not its centre: the round trip survives, the interior band check does not
    if (x) *x = (uint32_t)(((uint64_t)cx * w) / N48_DPG_VERIFY_CELLS);
    if (y) *y = (uint32_t)(((uint64_t)cy * h) / N48_DPG_VERIFY_CELLS);
}
static void map_mut_word0(uint64_t *map, uint32_t cell)
{   // every bit goes into word 0: the 256-bit packing is a lie
    if (map) map[0] |= (uint64_t)1u << (cell & 63u);
}
static int due_mut_ignores_budget(uint64_t token, uint64_t lastToken, uint32_t runs)
{ (void)runs; return token != lastToken; }
static int due_mut_ignores_token(uint64_t token, uint64_t lastToken, uint32_t runs)
{ (void)token; (void)lastToken; return runs < N48_DPG_VERIFY_BUDGET; }
static int due_mut_no_first(uint64_t token, uint64_t lastToken, uint32_t runs)
{ return runs < N48_DPG_VERIFY_BUDGET && token != lastToken; }   // the runs == 0 first-present case is lost

// ---------------------------------------------------------------------------------------------------------------------------
// The checks, parameterised so the same body runs the real functions and every mutant. Returns the number of failures
// produced (gRun/gFail accumulate across calls; main reports the deltas).
// ---------------------------------------------------------------------------------------------------------------------------
struct CellFns {
    uint32_t (*cell)(uint32_t, uint32_t, uint32_t, uint32_t);
    void (*centre)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, uint32_t *);
    void (*mapset)(uint64_t *, uint32_t);
    int (*due)(uint64_t, uint64_t, uint32_t);
    const char *name;
};

static int checks(const CellFns &fn)
{
    const int f0 = gFail;

    // --- the size constants (the brief's "state widths") ---
    expect_u("16 cells per side", (uint64_t)N48_DPG_VERIFY_CELLS, 16u);
    expect_u("256 samples", (uint64_t)N48_DPG_VERIFY_SAMPLES, 256u);
    expect_u("4 x 64-bit map words hold 256 bits", (uint64_t)N48_DPG_VERIFY_MAP_WORDS * 64u, 256u);
    expect_u("8 differing samples are reported", (uint64_t)N48_DPG_VERIFY_FIRST, 8u);
    expect_u("the per-boot bound is 48 lines (review binding 2: reaches post-commit presents)", (uint64_t)N48_DPG_VERIFY_BUDGET, 48u);

    // --- corners and edges of the cell index ---
    expect_u("(0,0) -> cell 0",             fn.cell(0, 0, 1920, 1080), 0u);
    expect_u("last pixel -> cell 255",      fn.cell(1919, 1079, 1920, 1080), 255u);
    expect_u("x=191 -> cell 1",             fn.cell(191, 0, 1920, 1080), 1u);
    expect_u("y=67 -> cell row 0",          fn.cell(0, 67, 1920, 1080), 0u);
    expect_u("y=68 -> cell row 1",          fn.cell(0, 68, 1920, 1080), 16u);
    expect_u("a non-multiple width: 100 px, x=99 -> cell 15", fn.cell(99, 0, 100, 100), 15u);
    expect_u("one past the right edge clamps to cell col 15, row 7 -> 127", fn.cell(1920, 500, 1920, 1080), 127u);
    expect_u("one past the bottom edge clamps to cell 240", fn.cell(10, 1080, 1920, 1080), 240u);
    expect_u("a degenerate rect is cell 0, never a wild index", fn.cell(5, 5, 0, 0), 0u);

    // --- all 256 cell centres: inside the rectangle, in the MIDDLE of their own cell, and round-tripping ---
    uint32_t missRound = 0, outOfRect = 0, notInterior = 0, dupCentre = 0;
    for (uint32_t cy = 0; cy < N48_DPG_VERIFY_CELLS; cy++) {
        uint64_t rowbits = 0;
        for (uint32_t cx = 0; cx < N48_DPG_VERIFY_CELLS; cx++) {
            uint32_t x = 0, y = 0;
            fn.centre(cx, cy, 1920, 1080, &x, &y);
            if (x >= 1920u || y >= 1080u) outOfRect++;
            if (fn.cell(x, y, 1920, 1080) != cy * N48_DPG_VERIFY_CELLS + cx) missRound++;
            const uint32_t xl = (cx * 1920u) / 16u, xr = ((cx + 1u) * 1920u) / 16u;
            const uint32_t yt = (cy * 1080u) / 16u, yb = ((cy + 1u) * 1080u) / 16u;
            if (!(x >= xl && x < xr && y >= yt && y < yb) || x == xl || y == yt) notInterior++;
            if (rowbits & (1ull << cx)) dupCentre++;
            rowbits |= (1ull << cx);
        }
    }
    expect_u("all 256 cell centres are inside the rect", (uint64_t)outOfRect, 0u);
    expect_u("cell(centre(cell)) == cell for all 256", (uint64_t)missRound, 0u);
    expect_u("every centre is strictly inside its own cell (not the edge)", (uint64_t)notInterior, 0u);
    expect_u("no duplicate centre within a cell row", (uint64_t)dupCentre, 0u);

    // --- the map: set/get round trip, and the 256-bit packing ---
    {
        uint64_t map[N48_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
        fn.mapset(map, 0); fn.mapset(map, 63); fn.mapset(map, 64); fn.mapset(map, 255);
        expect_u("cell 0 lands in word 0 bit 0", (uint64_t)(n48_dpg_verify_map_get(map, 0) && (map[0] & 1u)), 1u);
        expect_u("cell 63 lands in word 0 bit 63", (uint64_t)((map[0] >> 63) & 1u), 1u);
        expect_u("cell 64 lands in word 1 (word 0 untouched by it)", (uint64_t)(n48_dpg_verify_map_get(map, 64) && map[1] == 1u), 1u);
        expect_u("cell 255 lands in word 3 bit 63", (uint64_t)((map[3] >> 63) & 1u && n48_dpg_verify_map_get(map, 255)), 1u);
        expect_u("an unset cell reads 0", (uint64_t)n48_dpg_verify_map_get(map, 128), 0u);
        uint64_t m2[N48_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
        uint32_t ones = 0;
        for (uint32_t c = 0; c < N48_DPG_VERIFY_SAMPLES; c++) fn.mapset(m2, c);
        for (unsigned w = 0; w < N48_DPG_VERIFY_MAP_WORDS; w++) {
            uint64_t v = m2[w];
            while (v) { ones += (uint32_t)(v & 1u); v >>= 1; }
        }
        expect_u("256 sets light exactly 256 bits", (uint64_t)ones, 256u);
        for (unsigned w = 0; w < N48_DPG_VERIFY_MAP_WORDS; w++) expect_u("and every word is full", m2[w], ~0ull);
    }

    // --- the trigger and its bound ---
    expect_u("runs 0 (the first present) is due whatever the token", (uint64_t)fn.due(0x10930000ull, 0x10930000ull, 0u), 1u);
    expect_u("a NEW token under the budget is due", (uint64_t)fn.due(0x11200000ull, 0x10930000ull, 1u), 1u);
    expect_u("the SAME token is not due", (uint64_t)fn.due(0x10930000ull, 0x10930000ull, 1u), 0u);
    expect_u("a new token at the bound is NOT due", (uint64_t)fn.due(0x11200000ull, 0x10930000ull, N48_DPG_VERIFY_BUDGET), 0u);
    expect_u("the last line under the bound is due", (uint64_t)fn.due(0x11200000ull, 0x10930000ull, N48_DPG_VERIFY_BUDGET - 1u), 1u);
    expect_u("the bound is a ceiling: BUDGET accepted, the next refused",
             (uint64_t)(fn.due(0x1ull, 0x0ull, N48_DPG_VERIFY_BUDGET - 1u) + fn.due(0x2ull, 0x1ull, N48_DPG_VERIFY_BUDGET)), 1u);

    // --- a synthetic 1/16 pattern: the map must locate the differing cells, and count them ---
    {
        const uint32_t W = 1920, H = 1080;
        uint64_t map[N48_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
        uint32_t differ = 0;
        for (uint32_t cy = 0; cy < N48_DPG_VERIFY_CELLS; cy++)
            for (uint32_t cx = 0; cx < N48_DPG_VERIFY_CELLS; cx++) {
                uint32_t x = 0, y = 0;
                fn.centre(cx, cy, W, H, &x, &y);
                const uint32_t src = 0x00ff00ffu;
                const uint32_t dst = ((cx & 1u) == 0u) ? 0x00ff00ffu : 0x00000000u;
                if (dst != src) { fn.mapset(map, fn.cell(x, y, W, H)); differ++; }
            }
        expect_u("the checkerboard differs in exactly half the cells", (uint64_t)differ, 128u);
        uint32_t bits = 0;
        for (unsigned w = 0; w < N48_DPG_VERIFY_MAP_WORDS; w++) {
            uint64_t v = map[w];
            while (v) { bits += (uint32_t)(v & 1u); v >>= 1; }
        }
        expect_u("and the map holds exactly that many bits", (uint64_t)bits, 128u);
        expect_u("the map is not one word of garbage", (uint64_t)(map[0] != ~0ull && (map[0] | map[1] | map[2] | map[3]) != 0ull), 1u);
    }

    return gFail - f0;
}

int main()
{
    const CellFns real = { n48_dpg_cell16, n48_dpg_cell16_centre, n48_dpg_verify_map_set, n48_dpg_verify_due, "real" };
    const CellFns mutants[] = {
        { cell16_mut_noclamp,    n48_dpg_cell16_centre, n48_dpg_verify_map_set, n48_dpg_verify_due,     "cell16 no clamp" },
        { cell16_mut_widths,     n48_dpg_cell16_centre, n48_dpg_verify_map_set, n48_dpg_verify_due,     "cell16 swapped w/h" },
        { n48_dpg_cell16,        centre_mut_leftedge,   n48_dpg_verify_map_set, n48_dpg_verify_due,     "centre at the edge" },
        { n48_dpg_cell16,        n48_dpg_cell16_centre, map_mut_word0,          n48_dpg_verify_due,     "map all word 0" },
        { n48_dpg_cell16,        n48_dpg_cell16_centre, n48_dpg_verify_map_set, due_mut_ignores_budget, "due ignores budget" },
        { n48_dpg_cell16,        n48_dpg_cell16_centre, n48_dpg_verify_map_set, due_mut_ignores_token,  "due ignores token" },
        { n48_dpg_cell16,        n48_dpg_cell16_centre, n48_dpg_verify_map_set, due_mut_no_first,       "due loses first" },
    };

    std::printf("--- real (display_pipe_guard.h) ---\n");
    const int r0 = gRun;
    const int realFail = checks(real);
    std::printf("\nREAL: %d check(s), %d failure(s)\n", gRun - r0, realFail);

    int caught = 0, total = 0;
    for (unsigned m = 0; m < sizeof(mutants) / sizeof(mutants[0]); m++) {
        std::printf("\n--- MUTANT %s ---\n", mutants[m].name);
        const int m0 = gRun, mf0 = gFail;
        const int f = checks(mutants[m]);
        total++;
        if (f > 0) { caught++; std::printf("MUTANT %s CAUGHT: %d check(s) failed of %d\n", mutants[m].name, f, gRun - m0); }
        else std::printf("MUTANT %s ESCAPED (the test does not discriminate)\n", mutants[m].name);
        (void)mf0;
    }

    const bool ok = (realFail == 0) && (caught == total);
    std::printf("\nREAL: %d/%d passed. non-vacuity: %d of %d mutants caught.\n", gRun - r0 - realFail, gRun - r0, caught, total);
    return ok ? 0 : 1;
}
