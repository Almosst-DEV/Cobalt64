// scanout_selftest_test.cpp — S7 of notes/design/SCANOUT-SELFTEST-FULL.md (0.0.414): the pure half of the
// full-geometry SDMA self-test, host-tested. Three things are pinned here, each with a mutant that must be
// caught (the pipeshim_verify_test style, not a vacuous pass):
//   1. the S5 sample-set generator — count, bounds, no duplicate, and every named row/column present;
//   2. the decode value -> (x,y) / POISON;
//   3. S2's 16x16 map encoding, reused unchanged (cell 0 = bit 0 of word 0).
// The kext compiles the SAME headers (src/apple/scanout_copy.h, src/apple/display_pipe_guard.h).
//
// Build and run on the host Mac (no kernel headers, no hardware, no PC):
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/scanout_selftest_test.cpp -o /tmp/scanoutfull && /tmp/scanoutfull

#include <cstdio>
#include <cstdint>
#include <vector>
#include <set>

#include "scanout_copy.h"
#include "display_pipe_guard.h"

static int gFail = 0, gRun = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        std::printf("FAIL  %-78s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else {
        std::printf("ok    %-78s %#llx\n", what, (unsigned long long)got);
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Mutants: one defect each. A mutant that passes every check means the check is vacuous.
// -------------------------------------------------------------------------------------------------------------------
static uint64_t count_mut_offbyone(uint32_t w, uint32_t h)
{   // the count is one too high: the last `at` returns 0 and the caller drops it, so a naive count check misses it
    return n48_tile_ss_count(w, h) + 1u;
}
static int at_mut_shift(uint32_t w, uint32_t h, uint64_t i, uint32_t *px, uint32_t *py)
{   // every x is one to the right: the right edge walks out of the surface
    uint32_t x = 0, y = 0;
    if (!n48_tile_ss_at(w, h, i, &x, &y)) return 0;
    if (px) *px = x + 1u;
    if (py) *py = y;
    return 1;
}
static int decode_mut_nopoison(uint32_t v, uint32_t *px, uint32_t *py)
{   // the POISON sentinel is not recognised: a never-written destination pixel decodes as a pixel
    if (px) *px = v & 0xffffu;
    if (py) *py = v >> 16;
    return 1;
}
static void map_mut_word0(uint64_t *map, uint32_t cell)
{   // every bit goes into word 0: the 256-bit packing is a lie
    if (map) map[0] |= (uint64_t)1u << (cell & 63u);
}
static uint32_t probe_mut_off(uint32_t x, uint32_t y)
{   // the uniform probe is the wrong constant by one: `scanout 8` would expect and write different pixels
    (void)x; (void)y;
    return N48_TILE_UNIFORM_PIXEL + 1u;
}

struct Fns {
    uint64_t (*count)(uint32_t, uint32_t);
    int (*at)(uint32_t, uint32_t, uint64_t, uint32_t *, uint32_t *);
    int (*decode)(uint32_t, uint32_t *, uint32_t *);
    void (*mapset)(uint64_t *, uint32_t);
    uint32_t (*probe)(uint32_t, uint32_t);
    const char *name;
};

static int checks(const Fns &fn)
{
    const int f0 = gFail;

    // --- the states' widths and the named rows/columns ---
    expect_u("10 named rows (0.0.514 B3: + 1439)", (uint64_t)N48_TILE_SS_ROWS, 10u);
    expect_u("5 named columns (0.0.514 B3: + 2559)", (uint64_t)N48_TILE_SS_COLS, 5u);
    expect_u("64 x 64 grid", (uint64_t)N48_TILE_SS_GRID, 64u);
    expect_u("row 0", n48_tile_ss_row(0), 0u);
    expect_u("row 8", n48_tile_ss_row(5), 128u);
    expect_u("row 1079", n48_tile_ss_row(8), 1079u);
    expect_u("row 1439 (the 1440p bottom edge)", n48_tile_ss_row(9), 1439u);
    expect_u("row past the end reads 0", n48_tile_ss_row(10), 0u);
    expect_u("col 1919", n48_tile_ss_col(3), 1919u);
    expect_u("col 2559 (the 1440p right edge)", n48_tile_ss_col(4), 2559u);
    expect_u("col past the end reads 0", n48_tile_ss_col(5), 0u);
    expect_u("row_of(127)", (uint64_t)n48_tile_ss_row_of(127), 1u);
    expect_u("row_of(126) is not a named row", (uint64_t)n48_tile_ss_row_of(126), 0u);
    expect_u("col_of(128)", (uint64_t)n48_tile_ss_col_of(128), 1u);

    // --- the decode ---
    {
        uint32_t x = 0, y = 0;
        expect_u("decode a probe pixel round-trips", (uint64_t)fn.decode(n48_tile_probe_pixel(1919, 1079), &x, &y), 1u);
        expect_u("decode x", x, 1919u);
        expect_u("decode y", y, 1079u);
        expect_u("the POISON is NOT decodeable", (uint64_t)fn.decode(N48_TILE_PROBE_POISON, &x, &y), 0u);
        // The poison must never collide with a probe pixel on either surface this test runs on.
        uint32_t collide = 0;
        for (uint32_t yy = 0; yy < 1080u; yy += 7u)
            for (uint32_t xx = 0; xx < 1920u; xx += 13u)
                if (n48_tile_probe_pixel(xx, yy) == N48_TILE_PROBE_POISON) collide++;
        expect_u("the poison is not a probe pixel anywhere on 1920x1080", (uint64_t)collide, 0u);
    }

    // --- the sample set, on BOTH geometries the kext runs ---
    // 0.0.514 B3: + the native 1440p geometry mode 6 now runs when Console says 2560x1440
    static const uint32_t dims[3][2] = { { 1920u, 1080u }, { 256u, 256u }, { 2560u, 1440u } };
    static const uint64_t want[3] = { 25533u, 6068u, 36719u };   // 2560x1440: 10 x 2560 + 5 x 1430 + 63 x 63
    for (unsigned d = 0; d < 3u; d++) {
        const uint32_t w = dims[d][0], h = dims[d][1];
        const uint64_t n = fn.count(w, h);
        if (d == 0) expect_u("sample count 1920x1080", n, want[0]);
        else if (d == 1) expect_u("sample count 256x256", n, want[1]);
        else expect_u("sample count 2560x1440 (10 rows x 2560 + 5 columns x 1430 + the grid off them)", n, want[2]);

        std::set<std::pair<uint32_t, uint32_t> > seen;
        uint32_t outOfBounds = 0, badAt = 0;
        uint32_t rowHit[N48_TILE_SS_ROWS] = { 0 }, colHit[N48_TILE_SS_COLS] = { 0 };
        for (uint64_t i = 0; i < n; i++) {
            uint32_t x = 0, y = 0;
            if (!fn.at(w, h, i, &x, &y)) { badAt++; continue; }
            if (x >= w || y >= h) outOfBounds++;
            seen.insert(std::make_pair(x, y));
            for (unsigned r = 0; r < N48_TILE_SS_ROWS; r++) if (y == n48_tile_ss_row(r)) rowHit[r]++;
            for (unsigned c = 0; c < N48_TILE_SS_COLS; c++) if (x == n48_tile_ss_col(c)) colHit[c]++;
        }
        expect_u("every sample has coordinates", (uint64_t)badAt, 0u);
        expect_u("no sample is past the surface", (uint64_t)outOfBounds, 0u);
        expect_u("no duplicate sample", (uint64_t)seen.size(), n);
        // A named row is sampled at every x. A named column is sampled at every y: the column segment covers y
        // values that are not named rows, and the row segment covers the R crossings, so the column totals h
        // (and the "no duplicate sample" check above proves the two segments do not overlap).
        uint32_t R = 0, C = 0;
        for (unsigned r = 0; r < N48_TILE_SS_ROWS; r++) if (n48_tile_ss_row(r) < h) { expect_u("row sampled at every x", rowHit[r], w); R++; }
        for (unsigned c = 0; c < N48_TILE_SS_COLS; c++) if (n48_tile_ss_col(c) < w) { expect_u("column sampled at every y", colHit[c], h); C++; }
        expect_u("the in-range named rows are the prefix of the list", (uint64_t)R, (uint64_t)n48_tile_ss_rows_in(h));
        expect_u("the in-range named columns are the prefix of the list", (uint64_t)C, (uint64_t)n48_tile_ss_cols_in(w));
        // The generator must refuse an index at the count and beyond.
        uint32_t x = 0, y = 0;
        expect_u("at(count) is refused", (uint64_t)fn.at(w, h, n, &x, &y), 0u);
        expect_u("at(count+1) is refused", (uint64_t)fn.at(w, h, n + 1u, &x, &y), 0u);
    }

    // --- D7 (0.0.417, notes/design/SDMA-DCC-NOPTE.md): the UNIFORM probe `scanout 8` writes, and the value
    //     every sampled position must then read back. Constant, and neither the self-naming probe nor POISON. ---
    {
        expect_u("the uniform probe value is 0xff00ff00", N48_TILE_UNIFORM_PIXEL, 0xff00ff00u);
        expect_u("the uniform probe is constant over 1920x1080",
                 (uint64_t)(fn.probe(0, 0) == N48_TILE_UNIFORM_PIXEL &&
                            fn.probe(1919, 1079) == fn.probe(0, 0) &&
                            fn.probe(959, 540) == fn.probe(0, 0)), 1u);
        expect_u("the uniform probe is not the self-naming probe at (1919,1079)",
                 (uint64_t)(fn.probe(1919, 1079) != n48_tile_probe_pixel(1919, 1079)), 1u);
        expect_u("the uniform probe is not the POISON", (uint64_t)(fn.probe(1, 1) != N48_TILE_PROBE_POISON), 1u);
    }

    // --- S2's map encoding, reused unchanged ---
    {
        expect_u("(0,0) -> cell 0", n48_dpg_cell16(0, 0, 1920, 1080), 0u);
        expect_u("last pixel -> cell 255", n48_dpg_cell16(1919, 1079, 1920, 1080), 255u);
        uint64_t map[N48_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
        fn.mapset(map, 0); fn.mapset(map, 64); fn.mapset(map, 255);
        expect_u("cell 0 lands in word 0 bit 0", (uint64_t)(n48_dpg_verify_map_get(map, 0) && (map[0] & 1u)), 1u);
        expect_u("cell 64 lands in word 1 and word 0 stays 1", (uint64_t)(n48_dpg_verify_map_get(map, 64) && map[0] == 1u && map[1] == 1u), 1u);
        expect_u("cell 255 lands in word 3 bit 63", (uint64_t)(n48_dpg_verify_map_get(map, 255) && ((map[3] >> 63) & 1u)), 1u);
    }

    return gFail - f0;
}

// 0.0.514 B3/B5: 0.0.513's sample set, re-implemented from its OWN tables (9 rows, 4 columns), so the 256x256 control and the
// 1920x1080 case can be proven to sample exactly the same pixels in exactly the same order as before.
static const uint32_t kR513[9] = { 0u, 1u, 7u, 8u, 127u, 128u, 1023u, 1024u, 1079u };
static const uint32_t kC513[4] = { 0u, 127u, 128u, 1919u };
static bool r513_of(uint32_t y) { for (uint32_t v : kR513) if (v == y) return true; return false; }
static bool c513_of(uint32_t x) { for (uint32_t v : kC513) if (v == x) return true; return false; }
static std::vector<std::pair<uint32_t, uint32_t> > set513(uint32_t w, uint32_t h)
{
    std::vector<std::pair<uint32_t, uint32_t> > v;
    uint32_t R = 0, C = 0;
    for (uint32_t r : kR513) if (r < h) R++;
    for (uint32_t c : kC513) if (c < w) C++;
    for (uint32_t r = 0; r < R; r++) for (uint32_t x = 0; x < w; x++) v.push_back(std::make_pair(x, kR513[r]));
    for (uint32_t c = 0; c < C; c++) for (uint32_t y = 0; y < h; y++) if (!r513_of(y)) v.push_back(std::make_pair(kC513[c], y));
    for (uint32_t cy = 0; cy < 64u; cy++) {
        const uint32_t y = (uint32_t)(((uint64_t)cy * h) / 64u);
        if (r513_of(y)) continue;
        for (uint32_t cx = 0; cx < 64u; cx++) {
            const uint32_t x = (uint32_t)(((uint64_t)cx * w) / 64u);
            if (c513_of(x)) continue;
            v.push_back(std::make_pair(x, y));
        }
    }
    return v;
}
static void test_b3_identity()
{
    static const uint32_t dims[2][2] = { { 1920u, 1080u }, { 256u, 256u } };
    for (unsigned d = 0; d < 2u; d++) {
        const uint32_t w = dims[d][0], h = dims[d][1];
        const std::vector<std::pair<uint32_t, uint32_t> > old = set513(w, h);
        uint64_t same = old.size() == n48_tile_ss_count(w, h) ? 1u : 0u;
        for (uint64_t i = 0; same && i < old.size(); i++) {
            uint32_t x = 0, y = 0;
            if (!n48_tile_ss_at(w, h, i, &x, &y) || x != old[i].first || y != old[i].second) same = 0u;
        }
        char what[128];
        std::snprintf(what, sizeof what, "B5 %ux%u: the sample set is 0.0.513's, pixel for pixel and in order", w, h);
        expect_u(what, same, 1u);
    }
    // 1440p: the new edges are sampled along their whole length
    uint64_t r1439 = 0, c2559 = 0;
    const uint64_t n = n48_tile_ss_count(2560u, 1440u);
    for (uint64_t i = 0; i < n; i++) {
        uint32_t x = 0, y = 0;
        if (!n48_tile_ss_at(2560u, 1440u, i, &x, &y)) break;
        if (y == 1439u) r1439++;
        if (x == 2559u) c2559++;
    }
    expect_u("B3 2560x1440: row 1439 sampled at every x", r1439, 2560u);
    expect_u("B3 2560x1440: column 2559 sampled at every y", c2559, 1440u);
    // mode 6's geometry pick (n48_live_dim): the live Console size, else 0.0.513's 1920x1080
    expect_u("B3 live 1920 -> 1920", n48_live_dim(1920u, 1920u), 1920u);
    expect_u("B3 live 1080 -> 1080", n48_live_dim(1080u, 1080u), 1080u);
    expect_u("B3 live 2560 -> 2560", n48_live_dim(2560u, 1920u), 2560u);
    expect_u("B3 live 1440 -> 1440", n48_live_dim(1440u, 1080u), 1440u);
    expect_u("B3 absent (0) -> the old constant", n48_live_dim(0u, 1920u), 1920u);
    expect_u("B3 absurd (> 16384) -> the old constant", n48_live_dim(40000u, 1080u), 1080u);
    // B4: the flush clamp over the live geometry (Navi48AccelPeer.cpp: cw = w < capW ? w : capW)
    auto clampW = [](uint32_t w, uint32_t live) { const uint32_t cap = n48_live_dim(live, 1920u); return w < cap ? w : cap; };
    expect_u("B4 1080p boot: a 2560-wide surface clamps to 1920 (0.0.513's answer)", clampW(2560u, 1920u), 1920u);
    expect_u("B4 1440p boot: a 2560-wide surface is copied whole", clampW(2560u, 2560u), 2560u);
    expect_u("B4 no Console property: the old 1920", clampW(2560u, 0u), 1920u);
}

int main()
{
    const Fns real = { n48_tile_ss_count, n48_tile_ss_at, n48_tile_decode, n48_dpg_verify_map_set, n48_tile_uniform_pixel, "real" };
    const Fns mutants[] = {
        { count_mut_offbyone,   n48_tile_ss_at,      n48_tile_decode, n48_dpg_verify_map_set, n48_tile_uniform_pixel, "count off by one" },
        { n48_tile_ss_count,    at_mut_shift,        n48_tile_decode, n48_dpg_verify_map_set, n48_tile_uniform_pixel, "at shifted right" },
        { n48_tile_ss_count,    n48_tile_ss_at,      decode_mut_nopoison, n48_dpg_verify_map_set, n48_tile_uniform_pixel, "decode ignores poison" },
        { n48_tile_ss_count,    n48_tile_ss_at,      n48_tile_decode, map_mut_word0,           n48_tile_uniform_pixel, "map all word 0" },
        { n48_tile_ss_count,    n48_tile_ss_at,      n48_tile_decode, n48_dpg_verify_map_set, probe_mut_off,          "uniform probe off by one" },
    };

    std::printf("--- real (scanout_copy.h / display_pipe_guard.h) ---\n");
    const int r0 = gRun;
    const int realFail0 = checks(real);
    const int fB = gFail;
    test_b3_identity();                     // 0.0.514 B3/B4/B5
    const int realFail = realFail0 + (gFail - fB);
    std::printf("\nREAL: %d check(s), %d failure(s)\n", gRun - r0, realFail);

    int caught = 0, total = 0;
    for (unsigned m = 0; m < sizeof(mutants) / sizeof(mutants[0]); m++) {
        std::printf("\n--- MUTANT %s ---\n", mutants[m].name);
        const int m0 = gRun;
        const int f = checks(mutants[m]);
        total++;
        if (f > 0) { caught++; std::printf("MUTANT %s CAUGHT: %d check(s) failed of %d\n", mutants[m].name, f, gRun - m0); }
        else std::printf("MUTANT %s ESCAPED (the test does not discriminate)\n", mutants[m].name);
    }

    const bool ok = (realFail == 0) && (caught == total);
    std::printf("\nREAL: %d/%d passed. non-vacuity: %d of %d mutants caught.\n", gRun - r0 - realFail, gRun - r0, caught, total);
    return ok ? 0 : 1;
}
