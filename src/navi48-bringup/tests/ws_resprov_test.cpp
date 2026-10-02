// ws_resprov_test.cpp - : the residency copy as a provenance source (ws_resprov.h). Build and run from the repo root:
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/ws_resprov_test.cpp -o /tmp/rptest && /tmp/rptest
// A. the two address equations against the VENDORED addrlib (ws_resprov_golden.h, tools/addrlib-golden/), every texel of seven
//    surfaces, six gfx10 configs in the proven class and four outside it;
// B. the re-tile: a bijection over the whole allocation, round trip gfx10 -> gfx12 -> gfx10 exact at every dword, and SAMPLE
//    EQUIVALENCE (a gfx10-laid image re-tiled reads back right at every texel under gfx12's equation); double re-tiling and no
//    re-tiling both caught; the-style quadrant probe shown NOT to catch the missing re-tile (failure mode #7);
// C. the shape proof and the provenance table, then planted defects: an unverified copy, a non-WindowServer copy, an entry that
//    survives unmap or rebind, the wrong mode, a stale epoch, a VA that no longer walks to the copied VRAM.
// T450 = RESPROV PART 1 additions (MIB-A1-PROVENANCE.md Q3 holes 1/3/4):
// A2. the three new (mode, bpp) equations (4KB_D_X at 8/64 bpp, 256B_D at 8 bpp) against the same vendored addrlib, in and
//     out of n48_rp_g10_cfg_ok's proven class, and xlat12_desc.h's SW_MODE 2 -> gfx12 mode 1 mapping;
// B2. n48_rp_retile_kind for the three new kinds: bijection, round trip, sample equivalence, non-multiple-of-block sizes,
//     planted breaks (the transpose/XOR removed, the wrong bpp, the wrong block size);
// G.  hole 1 (a failed re-copy must drop the old entry BEFORE the refusal return) and hole 4 (the DECIDE -> COMMIT carry,
//     selector off byte-identical to n48_rp_ok), each with its planted break.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <vector>
#include "ws_resprov.h"
#include "../../xlat12/xlat12_desc.h"
#include "ws_resprov_golden.h"

static int gFail = 0, gChecks = 0;
#define CHECK(c, ...) do { gChecks++; if (!(c)) { gFail++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint64_t fnv(uint64_t h, uint64_t v) { for (int b = 0; b < 8; b++) { h ^= (v >> (8 * b)) & 0xff; h *= 0x100000001b3ull; } return h; }
static uint64_t hash_g10(uint32_t w, uint32_t h) { uint64_t f = 0xcbf29ce484222325ull; const uint32_t pb = (w + 31) / 32;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) f = fnv(f, n48_rp_g10_off(x, y, pb)); return f; }
static uint64_t hash_g12(uint32_t w, uint32_t h) { uint64_t f = 0xcbf29ce484222325ull; const uint32_t pb = (w + 31) / 32;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) f = fnv(f, n48_rp_g12_off(x, y, pb)); return f; }

// ---- A ---------------------------------------------------------------------------------------------------------------------
static void test_equations() {
    for (const auto &g : kG12) {
        CHECK(g.pitch == ((g.w + 31) / 32) * 32, "gfx12 pitch %u for width %u", g.pitch, g.w);
        CHECK(hash_g12(g.w, g.h) == g.fnv, "gfx12 4KB_2D %ux%u disagrees with addrlib", g.w, g.h);
    }
    unsigned inClass = 0, outClass = 0, outDiffers = 0;
    uint32_t lastGb = 0; int differsThisGb = 0;
    for (size_t i = 0; i < sizeof(kG10) / sizeof(kG10[0]); i++) {
        const auto &g = kG10[i];
        CHECK(g.pitch == ((g.w + 31) / 32) * 32, "gfx10 pitch %u for width %u", g.pitch, g.w);
        if (n48_rp_g10_cfg_ok(g.gb)) {
            inClass++;
            CHECK(hash_g10(g.w, g.h) == g.fnv, "gfx10 4KB_D_X gb %#x %ux%u disagrees with addrlib", g.gb, g.w, g.h);
        } else {
            outClass++;
            if (g.gb != lastGb) { lastGb = g.gb; differsThisGb = 0; }
            if (hash_g10(g.w, g.h) != g.fnv && !differsThisGb) { differsThisGb = 1; outDiffers++; }
        }
    }
    CHECK(inClass == 42u, "in-class rows %u (6 configs x 7 surfaces)", inClass);
    CHECK(outClass == 28u && outDiffers == 4u, "every out-of-class config (4) has a surface our equation gets wrong: %u of %u rows, %u configs",
          outClass, outClass, outDiffers);
    // This card's register (0x08200545) and a typical Navi21 value are in the class; 1/2/4/8 pipes and a 512 B interleave are not.
    CHECK(n48_rp_g10_cfg_ok(0x08200545u) && n48_rp_g10_cfg_ok(0x444u), "the card's and Navi21's configs");
    CHECK(!n48_rp_g10_cfg_ok(0x40u) && !n48_rp_g10_cfg_ok(0x141u) && !n48_rp_g10_cfg_ok(0x242u) && !n48_rp_g10_cfg_ok(0x343u) &&
          !n48_rp_g10_cfg_ok(0x44cu) && !n48_rp_g10_cfg_ok(0x146u) && !n48_rp_g10_cfg_ok(0x645u), "out-of-class configs refused");
    // The layouts are NOT the same: F3's 28x40 texture, texel by texel.
    unsigned differ = 0, blockDiffer = 0;
    for (uint32_t y = 0; y < 40; y++) for (uint32_t x = 0; x < 28; x++) differ += n48_rp_g10_off(x, y, 1) != n48_rp_g12_off(x, y, 1);
    for (uint32_t y = 0; y < 32; y++) for (uint32_t x = 0; x < 32; x++) blockDiffer += n48_rp_g10_off(x, y, 1) != n48_rp_g12_off(x, y, 1);
    CHECK(differ == 672u && blockDiffer == 512u, "28x40: %u of 1120 texels differ (addrlib: 672); 32x32 block: %u of 1024 (512)", differ, blockDiffer);
    // The ledger's mode is xlat12_desc.h's by-name map of 22.
    uint32_t mapped = 0xffu;
    for (unsigned i = 0; i < XLAT12_SWMODE_MAP_COUNT; i++) if (kXlat12SwModeG10ToG12[i][0] == N48_RP_G10_4KB_D_X) mapped = kXlat12SwModeG10ToG12[i][1];
    CHECK(mapped == N48_RP_G12_4KB_2D, "xlat12_desc.h maps 22 to %u", mapped);
}

// ---- A2 (T450 item 1/2) ------------------------------------------------------------------------------------------------------
// Generic hashers over one of the three NEW kinds' off() functions, mirroring hash_g10/hash_g12 above. `pb` is pitch IN BLOCKS
// (golden's stored pitch is in ELEMENTS; divide by the kind's block width, matching n48_rp_kind_geom).
static void kind_geom(uint32_t kind, uint32_t &bw, uint32_t &bh) {
    uint32_t eb; n48_rp_kind_geom(kind, &bw, &bh, &eb); (void)eb;
}
static uint64_t hash_g10_kind(uint32_t kind, uint32_t w, uint32_t h, uint32_t pitchElems) {
    uint32_t bw, bh; kind_geom(kind, bw, bh); const uint32_t pb = pitchElems / bw;
    uint64_t f = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint32_t off = kind == N48_RP_KIND_4KB_D_X_8 ? n48_rp_g10_off_4kbdx8(x, y, pb) :
                              kind == N48_RP_KIND_4KB_D_X_64 ? n48_rp_g10_off_4kbdx64(x, y, pb) : n48_rp_g10_off_256d8(x, y, pb);
        f = fnv(f, off);
    }
    return f;
}
static uint64_t hash_g12_kind(uint32_t kind, uint32_t w, uint32_t h, uint32_t pitchElems) {
    uint32_t bw, bh; kind_geom(kind, bw, bh); const uint32_t pb = pitchElems / bw;
    uint64_t f = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint32_t off = kind == N48_RP_KIND_4KB_D_X_8 ? n48_rp_g12_off_4kbdx8(x, y, pb) :
                              kind == N48_RP_KIND_4KB_D_X_64 ? n48_rp_g12_off_4kbdx64(x, y, pb) : n48_rp_g12_off_256d8(x, y, pb);
        f = fnv(f, off);
    }
    return f;
}
static void test_equations_new() {
    // gfx12 side: every one of the three new golden arrays, every dim, against the matching kind's off() function.
    for (const auto &g : kG12_4kbdx8)
        CHECK(hash_g12_kind(N48_RP_KIND_4KB_D_X_8, g.w, g.h, g.pitch) == g.fnv, "gfx12 4KB_2D@8bpp %ux%u disagrees with addrlib", g.w, g.h);
    for (const auto &g : kG12_4kbdx64)
        CHECK(hash_g12_kind(N48_RP_KIND_4KB_D_X_64, g.w, g.h, g.pitch) == g.fnv, "gfx12 4KB_2D@64bpp %ux%u disagrees with addrlib", g.w, g.h);
    for (const auto &g : kG12_256d8)
        CHECK(hash_g12_kind(N48_RP_KIND_256B_D_8, g.w, g.h, g.pitch) == g.fnv, "gfx12 256B_2D@8bpp %ux%u disagrees with addrlib", g.w, g.h);
    // gfx10 side: in-class configs must match, out-of-class configs need not (and for 4KB_D_X, at least one must differ -
    //'s original property, now proven again at 8 and 64 bpp). 256B_D is measured CONFIG-INDEPENDENT (no pipe/bank XOR
    // at all: it has no "_X" suzzle suffix), so its out-of-class rows are expected to MATCH too - a genuine difference from
    // 4KB_D_X's out-of-class behaviour, not a bug in the test.
    auto check_g10_array = [](auto &arr, uint32_t kind, const char *name, bool expectOutClassDiffers) {
        unsigned inClass = 0, outClass = 0, outDiffers = 0;
        uint32_t lastGb = 0; int differsThisGb = 0;
        for (size_t i = 0; i < sizeof(arr) / sizeof(arr[0]); i++) {
            const auto &g = arr[i];
            if (n48_rp_g10_cfg_ok(g.gb)) {
                inClass++;
                CHECK(hash_g10_kind(kind, g.w, g.h, g.pitch) == g.fnv, "%s gb %#x %ux%u disagrees with addrlib", name, g.gb, g.w, g.h);
            } else {
                outClass++;
                if (g.gb != lastGb) { lastGb = g.gb; differsThisGb = 0; }
                if (hash_g10_kind(kind, g.w, g.h, g.pitch) != g.fnv && !differsThisGb) { differsThisGb = 1; outDiffers++; }
            }
        }
        CHECK(inClass == 42u, "%s in-class rows %u (6 configs x 7 surfaces)", name, inClass);
        if (expectOutClassDiffers)
            CHECK(outClass == 28u && outDiffers == 4u, "%s every out-of-class config (4) has a surface our equation gets wrong: %u configs", name, outDiffers);
        else
            CHECK(outClass == 28u && outDiffers == 0u, "%s is config-independent: no out-of-class config differs (got %u)", name, outDiffers);
    };
    check_g10_array(kG10_4kbdx8, N48_RP_KIND_4KB_D_X_8, "gfx10 4KB_D_X@8bpp", true);
    check_g10_array(kG10_4kbdx64, N48_RP_KIND_4KB_D_X_64, "gfx10 4KB_D_X@64bpp", true);
    check_g10_array(kG10_256d8, N48_RP_KIND_256B_D_8, "gfx10 256B_D@8bpp", false);
    // xlat12_desc.h's SW_MODE map: 2 (ADDR_SW_256B_D) -> 1 (ADDR3_256B_2D).
    uint32_t mapped256 = 0xffu;
    for (unsigned i = 0; i < XLAT12_SWMODE_MAP_COUNT; i++) if (kXlat12SwModeG10ToG12[i][0] == N48_RP_G10_256B_D) mapped256 = kXlat12SwModeG10ToG12[i][1];
    CHECK(mapped256 == N48_RP_G12_256B_2D, "xlat12_desc.h maps 2 to %u", mapped256);
    // xlat12_desc.h's FORMAT map: format 1 ("8_UNORM", 1 byte/element = 8 bpp) and format 71 ("16_16_16_16_FLOAT", 8
    // bytes/element = 64 bpp) - the two formats item 1's census names, neither is 32 bpp.
    CHECK(kXlat12FormatG10ToG12[1][0] == 1u && kXlat12FormatG10ToG12[1][1] == 1u, "format 1 maps to gfx12 1 (8_UNORM)");
    uint32_t f71 = 0xffffu;
    for (unsigned i = 0; i < XLAT12_FORMAT_MAP_COUNT; i++) if (kXlat12FormatG10ToG12[i][0] == 71u) f71 = kXlat12FormatG10ToG12[i][1];
    CHECK(f71 == 57u, "format 71 maps to gfx12 57 (16_16_16_16_FLOAT), got %u", f71);
    // n48_rp_kind dispatch: exactly the four golden-tested (mode, bpp) pairs, everything else refused.
    CHECK(n48_rp_kind(N48_RP_G10_4KB_D_X, 32u) == N48_RP_KIND_4KB_D_X_32, "4KB_D_X@32bpp kind");
    CHECK(n48_rp_kind(N48_RP_G10_4KB_D_X, 8u) == N48_RP_KIND_4KB_D_X_8, "4KB_D_X@8bpp kind");
    CHECK(n48_rp_kind(N48_RP_G10_4KB_D_X, 64u) == N48_RP_KIND_4KB_D_X_64, "4KB_D_X@64bpp kind");
    CHECK(n48_rp_kind(N48_RP_G10_256B_D, 8u) == N48_RP_KIND_256B_D_8, "256B_D@8bpp kind");
    CHECK(n48_rp_kind(N48_RP_G10_256B_D, 32u) == N48_RP_KIND_NONE, "256B_D@32bpp not covered");
    CHECK(n48_rp_kind(N48_RP_G10_4KB_D_X, 16u) == N48_RP_KIND_NONE, "4KB_D_X@16bpp not covered");
    CHECK(n48_rp_kind(1u, 32u) == N48_RP_KIND_NONE, "an unrelated SW_MODE not covered");
    // n48_rp_kind_bytes/n48_rp_kind_pitch_bytes: reduce to the existing n48_rp_bytes/n48_rp_pitch_bytes for kind 0, and match
    // every golden row's own pitch column (in bytes: elements * bpp/8) for the three new kinds.
    for (const auto &d : { std::pair<uint32_t,uint32_t>{28,40}, {100,70}, {33,33}, {1,1} }) {
        CHECK(n48_rp_kind_bytes(N48_RP_KIND_4KB_D_X_32, d.first, d.second) == n48_rp_bytes(d.first, d.second), "kind 0 bytes == n48_rp_bytes");
        CHECK(n48_rp_kind_pitch_bytes(N48_RP_KIND_4KB_D_X_32, d.first) == n48_rp_pitch_bytes(d.first), "kind 0 pitch == n48_rp_pitch_bytes");
    }
    for (const auto &g : kG10_4kbdx8) CHECK(n48_rp_kind_pitch_bytes(N48_RP_KIND_4KB_D_X_8, g.w) == 1u * g.pitch, "4kbdx8 pitch bytes");
    for (const auto &g : kG10_4kbdx64) CHECK(n48_rp_kind_pitch_bytes(N48_RP_KIND_4KB_D_X_64, g.w) == 8u * g.pitch, "4kbdx64 pitch bytes");
    for (const auto &g : kG10_256d8) CHECK(n48_rp_kind_pitch_bytes(N48_RP_KIND_256B_D_8, g.w) == 1u * g.pitch, "256d8 pitch bytes");
}

// ---- B ---------------------------------------------------------------------------------------------------------------------
static uint32_t img(uint32_t x, uint32_t y) { return 0x80000000u | (y << 14) | x; }
// A gfx10-laid allocation of img, padding texels included (they carry their own unique values too).
static std::vector<uint32_t> lay_g10(uint32_t w, uint32_t h) {
    const uint32_t pb = (w + 31) / 32, rows = (h + 31) / 32;
    std::vector<uint32_t> v((size_t)(n48_rp_bytes(w, h) / 4), 0u);
    for (uint32_t y = 0; y < rows * 32; y++) for (uint32_t x = 0; x < pb * 32; x++) v[n48_rp_g10_off(x, y, pb) / 4] = img(x, y);
    return v;
}
static unsigned sample_wrong(const std::vector<uint32_t> &a, uint32_t w, uint32_t h) {
    const uint32_t pb = (w + 31) / 32; unsigned bad = 0;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) bad += a[n48_rp_g12_off(x, y, pb) / 4] != img(x, y);
    return bad;
}
static void test_retile() {
    const uint32_t dims[][2] = { {28, 40}, {100, 70}, {256, 256}, {33, 33}, {1, 1}, {40, 300} };
    for (const auto &d : dims) {
        const uint32_t w = d[0], h = d[1]; const uint64_t bytes = n48_rp_bytes(w, h); const size_t nd = (size_t)(bytes / 4);
        std::vector<uint32_t> src = lay_g10(w, h), dst(nd, 0xdeadbeefu), back(nd, 0xdeadbeefu), twice(nd, 0u);
        CHECK(n48_rp_retile(src.data(), dst.data(), w, h, bytes, 1) == 1, "retile %ux%u", w, h);
        // bijection: every destination dword written once (the sentinel is not a value img() produces)
        std::vector<uint32_t> hits(nd, 0u); const uint32_t pb = (w + 31) / 32, rows = (h + 31) / 32;
        for (uint32_t y = 0; y < rows * 32; y++) for (uint32_t x = 0; x < pb * 32; x++) hits[n48_rp_g12_off(x, y, pb) / 4]++;
        unsigned notOnce = 0, sentinel = 0;
        for (size_t i = 0; i < nd; i++) { notOnce += hits[i] != 1u; sentinel += dst[i] == 0xdeadbeefu; }
        CHECK(notOnce == 0 && sentinel == 0, "%ux%u: %u dword(s) not hit exactly once, %u left unwritten", w, h, notOnce, sentinel);
        CHECK(sample_wrong(dst, w, h) == 0, "%ux%u: sampled under gfx12 after the re-tile, wrong texels %u", w, h, sample_wrong(dst, w, h));
        CHECK(n48_rp_retile(dst.data(), back.data(), w, h, bytes, 0) == 1 && back == src, "%ux%u: gfx10 -> gfx12 -> gfx10 not exact", w, h);
        if (w >= 2 && h >= 2) {
            CHECK(sample_wrong(src, w, h) > 0, "%ux%u: NOT re-tiling must be caught (sampled raw)", w, h);
            n48_rp_retile(dst.data(), twice.data(), w, h, bytes, 1);
            CHECK(sample_wrong(twice, w, h) > 0, "%ux%u: DOUBLE re-tiling must be caught", w, h);
        }
    }
    // Failure mode #7:'s probe (4 solid quadrants, sampled well inside each) passes WITHOUT the re-tile on 64x64.
    {
        const uint32_t w = 64, h = 64, pb = 2; std::vector<uint32_t> raw((size_t)(n48_rp_bytes(w, h) / 4));
        auto quad = [](uint32_t x, uint32_t y) { return (x < 32 ? 0u : 1u) | (y < 32 ? 0u : 2u); };
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) raw[n48_rp_g10_off(x, y, pb) / 4] = quad(x, y);
        unsigned probeBad = 0, fullBad = 0;
        const uint32_t probes[4][2] = { {16, 16}, {48, 16}, {16, 48}, {48, 48} };
        for (const auto &p : probes) probeBad += raw[n48_rp_g12_off(p[0], p[1], pb) / 4] != quad(p[0], p[1]);
        std::vector<uint32_t> big = lay_g10(w, h);
        fullBad = sample_wrong(big, w, h);
        CHECK(probeBad == 0 && fullBad > 0, "'s quadrant probe is blind to the missing re-tile (probe %u wrong, every-texel %u wrong)", probeBad, fullBad);
    }
    // bad arguments write nothing
    uint32_t a[1024] = { 0 }, b[1024] = { 0 };
    CHECK(n48_rp_retile(a, b, 28, 40, 4096, 1) == 0 && n48_rp_retile(a, b, 0, 40, 8192, 1) == 0 &&
          n48_rp_retile(nullptr, b, 28, 40, 8192, 1) == 0, "bad arguments refused");
    // page-out symmetry (build 0.0.450 item 2, CONDUCTOR FIX: PER GFX10 MODE, not one shared flag - see
    // n48_rp_pageout_refuse's own banner). mode 22 (4KB_D_X) unchanged from 0.0.449, gated on `retiled4kbdx` alone.
    CHECK(n48_rp_pageout_refuse(1u, 0u, 0x36u) == 1 && n48_rp_pageout_refuse(0u, 0u, 0x36u) == 0 && n48_rp_pageout_refuse(1u, 0u, 0x3bu) == 0,
          "4KB_D_X page-out refused exactly once its OWN flag is set, whatever the 256B_D flag is");
    CHECK(n48_rp_pageout_refuse(1u, 0u, N48_RP_G10_4KB_D_X | (1u << 5)) == 1,
          "4KB_D_X page-out refused regardless of which bpp variant set retiled4kbdx (mode-only check)");
    // THE FIX ITSELF: a 256B_D page-out is allowed after ONLY a 32bpp (4KB_D_X) re-tile - retiled256bd is 0, so
    // switch 46 OFF can never make this refuse (nothing but the 256B_D kind, itself switch-46-only, sets it).
    CHECK(n48_rp_pageout_refuse(1u, 0u, N48_RP_G10_256B_D | (1u << 5)) == 0,
          "256B_D page-out ALLOWED after only a 32bpp re-tile (retiled256bd 0) - switch 46 OFF byte-identical to 0.0.449");
    // ... and refused once a REAL 256B_D re-tile happened (retiled256bd set - reachable only under switch 46).
    CHECK(n48_rp_pageout_refuse(0u, 1u, N48_RP_G10_256B_D | (1u << 5)) == 1,
          "256B_D page-out refused once a 256B_D re-tile actually happened");
    CHECK(n48_rp_pageout_refuse(0u, 1u, N48_RP_G10_4KB_D_X | (1u << 5)) == 0,
          "4KB_D_X page-out UNAFFECTED by a 256B_D-only re-tile (the two flags are independent)");
    // PLANTED BREAK: "one shared flag" - a single flag set by EITHER kind, exactly the first pass's (over-broad) bug.
    {
        auto pageout_shared_flag = [](uint32_t everRetiled, uint32_t surf) -> int {
            const uint32_t m = surf & 0x1fu;
            return everRetiled != 0u && (m == N48_RP_G10_4KB_D_X || m == N48_RP_G10_256B_D);
        };
        // the exact reviewer-flagged scenario: only the 32bpp path ran (switch 46 OFF), asking about a 256B_D resource.
        const int real = n48_rp_pageout_refuse(1u, 0u, N48_RP_G10_256B_D | (1u << 5));
        const int shared = pageout_shared_flag(1u, N48_RP_G10_256B_D | (1u << 5));
        const int caught = (real != shared) ? 1 : 0;
        std::printf("  planted %-66s %s (real %d, one-shared-flag %d)\n",
                    "item 2: one shared flag wrongly refuses 256B_D after only a 32bpp re-tile", caught ? "CAUGHT" : "MISSED", real, shared);
        CHECK(caught != 0, "item 2's planted break (one shared flag instead of per-mode) is caught");
    }
}

// ---- B2 (T450 item 2) --------------------------------------------------------------------------------------------------------
// The g10 off() function under test for one of the three NEW kinds, replaceable by planted defects (same shape as the real
// n48_rp_g10_off_* functions so a defect can stand in for exactly one of them).
typedef uint32_t (*OffFn)(uint32_t, uint32_t, uint32_t);
static uint32_t g10off_real(uint32_t kind, uint32_t x, uint32_t y, uint32_t pb) {
    return kind == N48_RP_KIND_4KB_D_X_8 ? n48_rp_g10_off_4kbdx8(x, y, pb) :
           kind == N48_RP_KIND_4KB_D_X_64 ? n48_rp_g10_off_4kbdx64(x, y, pb) : n48_rp_g10_off_256d8(x, y, pb);
}
static uint32_t g12off_real(uint32_t kind, uint32_t x, uint32_t y, uint32_t pb) {
    return kind == N48_RP_KIND_4KB_D_X_8 ? n48_rp_g12_off_4kbdx8(x, y, pb) :
           kind == N48_RP_KIND_4KB_D_X_64 ? n48_rp_g12_off_4kbdx64(x, y, pb) : n48_rp_g12_off_256d8(x, y, pb);
}
// P1 THE TRANSPOSE (XOR term) REMOVED: drop the two pipe/bank XOR partners, keeping only the first of each pair (4KB_D_X kinds
// only - 256B_D never had one, so this defect is not applicable there and is skipped for it in the scenario below).
static uint32_t g10off_no_xor(uint32_t kind, uint32_t x, uint32_t y, uint32_t pb) {
    if (kind == N48_RP_KIND_4KB_D_X_8) {
        const uint32_t in = (N48_RP_B(x,0)<<0)|(N48_RP_B(x,1)<<1)|(N48_RP_B(x,2)<<2)|(N48_RP_B(y,1)<<3)|(N48_RP_B(y,0)<<4)|
                            (N48_RP_B(y,2)<<5)|(N48_RP_B(x,3)<<6)|(N48_RP_B(y,3)<<7)|
                            (N48_RP_B(x,7)<<8)|(N48_RP_B(x,4)<<9)|(N48_RP_B(x,6)<<10)|(N48_RP_B(x,5)<<11);
        return in + ((((y >> 6) * pb) + (x >> 6)) << 12);
    }
    if (kind == N48_RP_KIND_4KB_D_X_64) {
        const uint32_t in = (N48_RP_B(x,0)<<3)|(N48_RP_B(y,0)<<4)|(N48_RP_B(x,1)<<5)|(N48_RP_B(x,2)<<6)|(N48_RP_B(y,1)<<7)|
                            (N48_RP_B(x,6)<<8)|(N48_RP_B(x,3)<<9)|(N48_RP_B(x,5)<<10)|(N48_RP_B(x,4)<<11);
        return in + ((((y >> 4) * pb) + (x >> 5)) << 12);
    }
    return g10off_real(kind, x, y, pb);
}
// P2 THE WRONG BPP: treat the element as 4 bytes (32 bpp) regardless of kind, so the allocation size n48_rp_kind_bytes
// expects and the size the buffer is actually walked at disagree.
// P3 THE WRONG BLOCK SIZE: use the 32x32 4KB_D_X-32bpp off() function for every kind (wrong block shape/size entirely).
static uint32_t g12off_wrong_block(uint32_t /*kind*/, uint32_t x, uint32_t y, uint32_t pb) { return n48_rp_g12_off(x, y, pb); }

static uint8_t img_byte(uint32_t eb, uint32_t x, uint32_t y, uint32_t k) {
    if (eb == 1u) return (uint8_t)((x * 131u + y * 197u + 77u) & 0xffu);
    // eb == 8: an (almost certainly) unique 64-bit tag per texel, little-endian.
    const uint64_t v = 0x8000000000000000ull | ((uint64_t)(y & 0xfffffu) << 20) | (uint64_t)(x & 0xfffffu);
    return (uint8_t)((v >> (8u * k)) & 0xffu);
}
static void test_retile_new() {
    struct { uint32_t kind; const char *name; } kinds[] = {
        { N48_RP_KIND_4KB_D_X_8,  "4KB_D_X@8bpp"  },
        { N48_RP_KIND_4KB_D_X_64, "4KB_D_X@64bpp" },
        { N48_RP_KIND_256B_D_8,   "256B_D@8bpp"   },
    };
    const uint32_t dims[][2] = { {28, 40}, {100, 70}, {33, 33}, {1, 1}, {256, 256} };
    for (const auto &kd : kinds) {
        uint32_t bw, bh, eb; n48_rp_kind_geom(kd.kind, &bw, &bh, &eb);
        for (const auto &d : dims) {
            const uint32_t w = d[0], h = d[1]; const uint64_t bytes = n48_rp_kind_bytes(kd.kind, w, h);
            std::vector<uint8_t> src(bytes, 0u), dst(bytes, 0xefu), back(bytes, 0xefu);
            const uint32_t pb = (w + bw - 1u) / bw, rows = (h + bh - 1u) / bh;
            for (uint32_t y = 0; y < rows * bh; y++) for (uint32_t x = 0; x < pb * bw; x++) {
                const uint32_t off = g10off_real(kd.kind, x, y, pb);
                for (uint32_t k = 0; k < eb; k++) src[off + k] = img_byte(eb, x, y, k);
            }
            CHECK(n48_rp_retile_kind(kd.kind, src.data(), dst.data(), w, h, bytes, 1) == 1, "%s retile %ux%u", kd.name, w, h);
            // bijection over the WHOLE allocation (padding included): every destination byte hit exactly once. This alone
            // guarantees full coverage (no sentinel needed - a byte's initial fill value would collide with a real img_byte()
            // output for SOME (x, y) at this element width, so "still equals the fill value" is not a safe unwritten-check;
            // the round trip below (back == src) is the coverage proof that also depends on real content).
            std::vector<uint32_t> hits(bytes, 0u); unsigned notOnce = 0;
            for (uint32_t y = 0; y < rows * bh; y++) for (uint32_t x = 0; x < pb * bw; x++) {
                const uint32_t off = g12off_real(kd.kind, x, y, pb);
                for (uint32_t k = 0; k < eb; k++) hits[off + k]++;
            }
            for (uint64_t i = 0; i < bytes; i++) notOnce += hits[i] != 1u;
            CHECK(notOnce == 0, "%s %ux%u: %u byte(s) of the destination not hit exactly once", kd.name, w, h, notOnce);
            // sample equivalence: every texel, read through the gfx12 offset, matches what was written through gfx10's
            unsigned wrong = 0;
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
                const uint32_t off = g12off_real(kd.kind, x, y, pb);
                for (uint32_t k = 0; k < eb; k++) wrong += dst[off + k] != img_byte(eb, x, y, k);
            }
            CHECK(wrong == 0, "%s %ux%u: sampled under gfx12 after the re-tile, wrong byte(s) %u", kd.name, w, h, wrong);
            CHECK(n48_rp_retile_kind(kd.kind, dst.data(), back.data(), w, h, bytes, 0) == 1 && back == src,
                  "%s %ux%u: gfx10 -> gfx12 -> gfx10 not exact", kd.name, w, h);
        }
    }
    // bad arguments / uncovered kind write nothing
    uint8_t a[4096] = { 0 }, b[4096] = { 0 };
    CHECK(n48_rp_kind_bytes(N48_RP_KIND_4KB_D_X_8, 28, 40) == 4096u, "sanity: 28x40 at 8bpp/4KB_D_X is one 64x64 block");
    CHECK(n48_rp_retile_kind(N48_RP_KIND_4KB_D_X_8, a, b, 28, 40, 2048, 1) == 0, "wrong byte count refused");
    CHECK(n48_rp_retile_kind(N48_RP_KIND_NONE, a, b, 28, 40, 4096, 1) == 0, "N48_RP_KIND_NONE refused");
    CHECK(n48_rp_retile_kind(N48_RP_KIND_4KB_D_X_8, nullptr, b, 28, 40, 4096, 1) == 0, "null src refused");
    CHECK(n48_rp_retile_kind(N48_RP_KIND_4KB_D_X_8, a, b, 0, 40, 4096, 1) == 0, "zero width refused");
}

// Planted breaks for the three new equations: re-run the SAME sample-equivalence proof but writing with g10off_real and
// reading with a defective g12-side function (or vice versa), which is exactly what a wrong equation would do in the field
// ('s failure mode: the copy moves bytes byte-for-byte gfx10-correct, but a wrong re-tile equation reads them back wrong).
static unsigned retile_defect_scenario(uint32_t kind, OffFn badG10, OffFn badG12) {
    uint32_t bw, bh, eb; n48_rp_kind_geom(kind, &bw, &bh, &eb);
    const uint32_t w = 128, h = 128; const uint64_t bytes = n48_rp_kind_bytes(kind, w, h);
    const uint32_t pb = (w + bw - 1u) / bw;
    std::vector<uint8_t> laidWrong(bytes, 0u);
    // Lay out with the REAL gfx10 equation (this models the copy - it is always correct), then read back as gfx12 would
    // through the (possibly defective) g12 offset - this is what a wrong re-tile equation looks like on hardware.
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint32_t off = g10off_real(kind, x, y, pb);
        for (uint32_t k = 0; k < eb; k++) laidWrong[off + k] = img_byte(eb, x, y, k);
    }
    unsigned wrong = 0;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint32_t off = badG12 ? badG12(x, y, pb) : g12off_real(kind, x, y, pb);
        // A defective offset function may point outside the (correctly-sized) allocation entirely - that is itself
        // evidence the equation is wrong, not something to read out of bounds.
        for (uint32_t k = 0; k < eb; k++) {
            if ((uint64_t)off + k >= bytes) { wrong++; continue; }
            wrong += laidWrong[off + k] != img_byte(eb, x, y, k);
        }
    }
    (void)badG10;
    return wrong;
}
static void test_planted_t450_retile() {
    struct { const char *name; unsigned failed; } p[] = {
        { "P1 4KB_D_X@8bpp: the pipe/bank XOR removed",
          retile_defect_scenario(N48_RP_KIND_4KB_D_X_8, nullptr, [](uint32_t x, uint32_t y, uint32_t pb) { return g10off_no_xor(N48_RP_KIND_4KB_D_X_8, x, y, pb); }) },
        { "P1 4KB_D_X@64bpp: the pipe/bank XOR removed",
          retile_defect_scenario(N48_RP_KIND_4KB_D_X_64, nullptr, [](uint32_t x, uint32_t y, uint32_t pb) { return g10off_no_xor(N48_RP_KIND_4KB_D_X_64, x, y, pb); }) },
        { "P3 4KB_D_X@8bpp: the 32x32 4KB_D_X-32bpp block/equation used instead",
          retile_defect_scenario(N48_RP_KIND_4KB_D_X_8, nullptr, [](uint32_t x, uint32_t y, uint32_t pb) { return g12off_wrong_block(0u, x, y, pb); }) },
        { "P3 256B_D@8bpp: the 32x32 4KB_D_X-32bpp block/equation used instead",
          retile_defect_scenario(N48_RP_KIND_256B_D_8, nullptr, [](uint32_t x, uint32_t y, uint32_t pb) { return g12off_wrong_block(0u, x, y, pb); }) },
    };
    unsigned caught = 0;
    for (const auto &x : p) {
        std::printf("  planted %-66s %s (%u byte(s) wrong)\n", x.name, x.failed ? "CAUGHT" : "MISSED", x.failed);
        caught += x.failed ? 1u : 0u;
    }
    CHECK(caught == sizeof(p) / sizeof(p[0]), "T450 retile planted defects caught %u", caught);
    // P2 the wrong bpp: n48_rp_kind_bytes for the WRONG kind disagrees with the real allocation size, so the retile itself
    // must already refuse (it never gets as far as scrambling texels).
    const uint64_t real8 = n48_rp_kind_bytes(N48_RP_KIND_4KB_D_X_8, 64, 64), real32 = n48_rp_kind_bytes(N48_RP_KIND_4KB_D_X_32, 64, 64);
    CHECK(real8 != real32, "8bpp and 32bpp allocations for the same surface are different sizes (%llu vs %llu)",
          (unsigned long long)real8, (unsigned long long)real32);
    uint8_t s64[4096] = { 0 }, d64[4096] = { 0 };
    CHECK(n48_rp_retile_kind(N48_RP_KIND_4KB_D_X_32, s64, d64, 64, 64, real8, 1) == 0,
          "P2 the wrong bpp: an 8bpp-sized buffer refused by the 32bpp kind (size mismatch caught)");
    std::printf("  planted %-66s %s\n", "P2 4KB_D_X: an 8bpp allocation size fed to the 32bpp kind", "CAUGHT (0 check(s) fail)");
}

// ---- C ---------------------------------------------------------------------------------------------------------------------
//: F3's texture as hp6 read it - res+0x1dc never written (0), the swizzle in the record the hardware is told
// (*(res+0x180)+0x40 = 22 | 2D), 28x40, rowBytes 128 (the 4KB_D_X pitch: 32 elements), 8 KB.
static n48_rp_shape good_shape() {
    n48_rp_shape s; std::memset(&s, 0, sizeof s);
    /*: hp7's copy #594 VERBATIM (driverlog-stream.txt:13989): res+0x1dc 0x20, *(res+0x180)+0x40 0x36. The two DISAGREE in
     * the swizzle bits and the hardware does not care (0xbdf9141 cmoveq), so this must be accepted. 0.0.365 refused it. */
    s.surf = 0x20u; s.hasMask = 1; s.maskOk = 1; s.maskSurf = 0x36u;
    s.w = 28; s.h = 40; s.depth = 1; s.rowBytes = 128; s.bytes = 0x2000; s.gbRead = 1; s.gb = 0x08200545u;
    return s;
}
// The shape proof under test, replaceable by planted defects.
static uint32_t shape_real(const n48_rp_shape *s) { return n48_rp_shape_check(s); }
// P1 THE WRONG FIELD: reads res+0x1dc only (0.0.364's proof: every hp6 copy read 0 there).
static uint32_t shape_wrong_field(const n48_rp_shape *s) { n48_rp_shape k = *s; k.hasMask = 0; return n48_rp_shape_check(&k); }
// P2 THE OLD PITCH RULE: rowBytes == 4 x width.
static uint32_t shape_old_pitch(const n48_rp_shape *s) {
    const uint32_t r = n48_rp_shape_check(s);
    if (r != N48_RP_SHAPE_OK && r != N48_RP_SHAPE_BPE) return r;
    return s->rowBytes == 4u * s->w ? r == N48_RP_SHAPE_BPE ? N48_RP_SHAPE_OK : r : N48_RP_SHAPE_BPE;
}
// STRICTER THAN THE HARDWARE: 0.0.365's MASK cross-check, which refused every texture in hp7 (res+0x1dc == 0x20 always).
static uint32_t shape_strict_mask(const n48_rp_shape *s) {
    if (s->hasMask && s->surf != 0u && (s->surf & 0x7fu) != (s->maskSurf & 0x7fu)) return N48_RP_SHAPE_MASK;
    return n48_rp_shape_check(s);
}
//b THE OTHER DIRECTION: an res+0x180 we could NOT read is trusted anyway (the fail-closed half must still hold).
static uint32_t shape_trust_unreadable(const n48_rp_shape *s) { n48_rp_shape k = *s; if (k.hasMask) k.maskOk = 1u; return n48_rp_shape_check(&k); }
static int shape_scenario(uint32_t (*chk)(const n48_rp_shape *)) {
    int bad = 0;
#define S(c) do { if (!(c)) bad++; } while (0)
    n48_rp_shape s = good_shape();
    S(chk(&s) == N48_RP_SHAPE_OK);                                                   // hp7's #594 VERBATIM: must be ACCEPTED
    s = good_shape(); s.surf = 22u | (1u << 5); S(chk(&s) == N48_RP_SHAPE_OK);       // both records written and agreeing
    s = good_shape(); s.surf = 27u | (1u << 5); S(chk(&s) == N48_RP_SHAPE_OK);       //: a disagreement is a WARNING
    s = good_shape(); s.surf = 22u | (2u << 5); S(chk(&s) == N48_RP_SHAPE_OK);       //: a TYPE disagreement too
    s = good_shape(); s.maskSurf = 0u; S(chk(&s) == N48_RP_SHAPE_MODE);              // the record the HARDWARE reads says linear
    s = good_shape(); s.maskSurf = 22u | (2u << 5); S(chk(&s) == N48_RP_SHAPE_TYPE); // its [6:5] is RESOURCE_TYPE: 3D refused
    s = good_shape(); s.hasMask = 0; S(chk(&s) == N48_RP_SHAPE_MODE);                // no record, 0x1dc 0x20: mode 0, refused
    s = good_shape(); s.hasMask = 0; s.surf = 22u | (1u << 5); S(chk(&s) == N48_RP_SHAPE_OK);       // no record: 0x1dc (the cmov)
    s = good_shape(); s.maskOk = 0; s.maskSurf = 0xffffffffu; S(chk(&s) == N48_RP_SHAPE_MASK);      // record UNREADABLE: refuse
    s = good_shape(); s.rowBytes = 112; S(chk(&s) == N48_RP_SHAPE_BPE);              // 4 x 28: not a 4KB_D_X pitch
    s = good_shape(); s.w = 32; s.rowBytes = 128; S(chk(&s) == N48_RP_SHAPE_OK);     // aligned width: both rules agree
    s = good_shape(); s.w = 100; s.h = 70; s.rowBytes = 512; s.bytes = n48_rp_bytes(100, 70); S(chk(&s) == N48_RP_SHAPE_OK);
    s = good_shape(); s.w = 100; s.h = 70; s.rowBytes = 400; s.bytes = n48_rp_bytes(100, 70); S(chk(&s) == N48_RP_SHAPE_BPE);
#undef S
    return bad;
}
static void test_shape() {
    n48_rp_shape s = good_shape();
    CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_OK, "hp7's copy #594 (0x1dc 0x20, record 0x36, 28x40 rowBytes 128, 8 KB) ACCEPTED");
    CHECK(n48_rp_swz_dword(&s) == 0x36u, "the swizzle dword is *(res+0x180)+0x40 when res+0x180 is set");
    CHECK(n48_rp_mask_warn(&s) == N48_RP_WARN_SWZ, "and 0x20 vs 0x36 is a swizzle WARNING, not a refusal (%u)", n48_rp_mask_warn(&s));
    s.hasMask = 0; s.surf = 0x3bu; CHECK(n48_rp_swz_dword(&s) == 0x3bu, "and res+0x1dc when it is not (setupHwCBRegs' cmov)");
    CHECK(n48_rp_mask_warn(&s) == 0u, "no record, no warning");
    s = good_shape(); s.surf = 0x36u; CHECK(n48_rp_mask_warn(&s) == 0u, "two records that agree warn about nothing");
    s = good_shape(); s.surf = 0x16u; CHECK(n48_rp_mask_warn(&s) == N48_RP_WARN_TYPE, "type bits alone");
    s = good_shape(); s.maskOk = 0; CHECK(n48_rp_mask_warn(&s) == 0u, "an unreadable record is compared with nothing");
    s = good_shape(); s.maskSurf = 27u | (1u << 5); CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_MODE, "64KB_R_X refused");
    s = good_shape(); s.maskSurf = 22u | (2u << 5); CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_TYPE, "3D refused");
    s = good_shape(); s.surf = 22u | (2u << 5); CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_OK, "a type disagreement no longer refuses");
    s = good_shape(); s.maskSurf = 0x80u | 22u | (1u << 5); CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_OK, "bits above [6:0] are not read");
    s = good_shape(); s.rowBytes = 64; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_BPE, "2 bytes per element refused (same 8 KB!)");
    s = good_shape(); s.rowBytes = 112; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_BPE, "unpadded pitch 112 refused");
    s = good_shape(); s.rowBytes = 256; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_BPE, "gfx10 linear pitch 256 refused");
    s = good_shape(); s.depth = 2; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_DEPTH, "depth 2 refused");
    s = good_shape(); s.bytes = 0x3000; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_BYTES, "mips/slices (bytes) refused");
    s = good_shape(); s.gbRead = 0; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_CFG_UNREAD, "unread config refused");
    s = good_shape(); s.gb = 0x343u; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_CFG_CLASS, "8-pipe config refused");
    s = good_shape(); s.w = 0; CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_DIMS, "zero width refused");
    // The pitch: 28x40 -> 128, and every width the vendored addrlib was asked (kG10 pitch is in elements, 32 bpp).
    CHECK(n48_rp_pitch_bytes(28) == 128u, "28 -> 128 bytes, got %u", n48_rp_pitch_bytes(28));
    CHECK(n48_rp_pitch_bytes(0) == 0u && n48_rp_pitch_bytes(N48_RP_MAX_DIM + 1u) == 0u, "out-of-range widths have no pitch");
    for (const auto &g : kG10)
        CHECK(n48_rp_pitch_bytes(g.w) == 4u * g.pitch, "addrlib gb %#x width %u pitch %u elements vs %u bytes", g.gb, g.w, g.pitch,
              n48_rp_pitch_bytes(g.w));
    CHECK(shape_scenario(&shape_real) == 0, "the real shape proof holds every scenario (%d failed)", shape_scenario(&shape_real));
}

// build 0.0.450 item 1 (switch 46): n48_rp_shape_check_kind generalises n48_rp_shape_check over the four
// golden-tested kinds - reduces to it EXACTLY for N48_RP_KIND_4KB_D_X_32, and admits the three new kinds T450 golden-
// tested (4KB_D_X@8/64bpp, 256B_D@8bpp) using each kind's OWN pitch/bytes formula (n48_rp_kind_pitch_bytes/n48_rp_kind_bytes),
// so "recording each entry's gfx12 mode" (the brief) has a shape check that only accepts the kind it claims.
static void test_shape_kind() {
    // Identity: for the 32bpp kind, on the SAME good_shape() fixture n48_rp_shape_check already covers, the two
    // functions must agree on every outcome - the generalisation changes nothing for the kind 0.0.449 already served.
    {
        n48_rp_shape s = good_shape();
        CHECK(n48_rp_shape_check_kind(&s, N48_RP_KIND_4KB_D_X_32) == n48_rp_shape_check(&s),
              "kind check reduces to the plain check for N48_RP_KIND_4KB_D_X_32 (OK case)");
        s.rowBytes = 64;
        CHECK(n48_rp_shape_check_kind(&s, N48_RP_KIND_4KB_D_X_32) == n48_rp_shape_check(&s),
              "kind check reduces to the plain check for N48_RP_KIND_4KB_D_X_32 (BPE refusal)");
    }
    // Each new kind, golden shape accepted.
    struct { uint32_t kind, mode; const char *name; } kinds[3] = {
        { N48_RP_KIND_4KB_D_X_8,  N48_RP_G10_4KB_D_X, "4KB_D_X@8bpp"  },
        { N48_RP_KIND_4KB_D_X_64, N48_RP_G10_4KB_D_X, "4KB_D_X@64bpp" },
        { N48_RP_KIND_256B_D_8,   N48_RP_G10_256B_D,  "256B_D@8bpp"   },
    };
    for (const auto &k : kinds) {
        n48_rp_shape s = good_shape();
        s.surf = k.mode | (1u << 5); s.hasMask = 0; s.maskOk = 0; s.maskSurf = 0;
        s.rowBytes = n48_rp_kind_pitch_bytes(k.kind, s.w);
        s.bytes = n48_rp_kind_bytes(k.kind, s.w, s.h);
        CHECK(n48_rp_shape_check_kind(&s, k.kind) == N48_RP_SHAPE_OK, "%s golden shape accepted", k.name);
        // wrong mode for this kind hypothesis refuses MODE
        n48_rp_shape wrongMode = s; wrongMode.surf = (k.mode == N48_RP_G10_4KB_D_X ? N48_RP_G10_256B_D : N48_RP_G10_4KB_D_X) | (1u << 5);
        CHECK(n48_rp_shape_check_kind(&wrongMode, k.kind) == N48_RP_SHAPE_MODE, "%s: wrong mode refused", k.name);
        // another kind's pitch/bytes fed to THIS kind's check refuses (no cross-kind confusion, e.g. 8bpp bytes on 64bpp)
        for (const auto &other : kinds) {
            if (other.kind == k.kind) continue;
            n48_rp_shape mix = s;
            mix.rowBytes = n48_rp_kind_pitch_bytes(other.kind, mix.w);
            mix.bytes = n48_rp_kind_bytes(other.kind, mix.w, mix.h);
            if (mix.rowBytes == s.rowBytes && mix.bytes == s.bytes) continue;   // coincidental match at this w/h: skip
            const uint32_t rs = n48_rp_shape_check_kind(&mix, k.kind);
            CHECK(rs == N48_RP_SHAPE_BPE || rs == N48_RP_SHAPE_BYTES, "%s: %s's pitch/bytes refused (got %u)", k.name, other.name, rs);
        }
    }
    // unread config still refuses CFG_UNREAD for a new kind (the same fail-closed rule as the 32bpp kind).
    {
        n48_rp_shape s = good_shape();
        s.surf = N48_RP_G10_256B_D | (1u << 5); s.hasMask = 0;
        s.rowBytes = n48_rp_kind_pitch_bytes(N48_RP_KIND_256B_D_8, s.w);
        s.bytes = n48_rp_kind_bytes(N48_RP_KIND_256B_D_8, s.w, s.h);
        s.gbRead = 0;
        CHECK(n48_rp_shape_check_kind(&s, N48_RP_KIND_256B_D_8) == N48_RP_SHAPE_CFG_UNREAD, "256B_D: unread config still refuses (fail-closed)");
    }
    n48_rp_shape zero {};
    CHECK(n48_rp_shape_check_kind(nullptr, N48_RP_KIND_4KB_D_X_8) == N48_RP_SHAPE_MODE, "null shape refused");
    CHECK(n48_rp_shape_check_kind(&zero, N48_RP_KIND_NONE) == N48_RP_SHAPE_MODE, "N48_RP_KIND_NONE refused");
}

// build 0.0.451 item 1 (S1, review of 0.0.450) - THE REVIEWER'S OWN SHAPE HARNESS (shapes.c), decide42's real
// inputs, replayed here so the suite proves the SAME answer shapes.c prints. P's 0x400003000/0x400032000 (256B_D,
// 8 bpp) are now OK; P's 0x40023c000 (4KB_D_X, 8 bpp) needs 0x4000 but Apple allocated only 0x3000 - SMALLER than
// the layout - refused BYTES_SMALL; AL's two (4KB_D_X, 64 bpp) are unaffected (already exact, no rounding needed).
static void test_shape_page_rounding() {
    struct { const char *n; uint32_t swz, w, h, row; uint64_t bytes; } r[5] = {
        { "0x400003000 P s10",  0x22, 128, 16, 128, 0x1000 }, { "0x400032000 P s14",  0x22, 141, 30, 144, 0x2000 },
        { "0x40023c000 P s42",  0x36, 209, 41, 256, 0x3000 }, { "0x400025000 AL s22", 0x36,  15, 16, 256, 0x1000 },
        { "0x400031000 AL s23", 0x36,  20, 13, 256, 0x1000 },
    };
    // Per-row: which kind hypothesis to try (matching each row's TRUE format - P's s42 is 8 bpp, AL's are 64 bpp -
    // exactly as shapes.c prints all three 4KB_D_X kind candidates and the brief's own per-row expectation names
    // the one that matters), and whether this row is expected to refuse BYTES_SMALL.
    const uint32_t tryKind[5] = { N48_RP_KIND_256B_D_8, N48_RP_KIND_256B_D_8, N48_RP_KIND_4KB_D_X_8,
                                  N48_RP_KIND_4KB_D_X_64, N48_RP_KIND_4KB_D_X_64 };
    const int expectSmall[5] = { 0, 0, 1, 0, 0 };
    for (int ri = 0; ri < 5; ri++) {
        const auto &row = r[ri];
        n48_rp_shape s {};
        s.hasMask = 1; s.maskOk = 1; s.maskSurf = row.swz; s.surf = 0x20;
        s.w = row.w; s.h = row.h; s.depth = 1; s.rowBytes = row.row; s.bytes = row.bytes; s.gbRead = 1; s.gb = 0x08200545u;
        const uint32_t chk32 = n48_rp_shape_check(&s);
        const uint32_t kindChk = n48_rp_shape_check_kind(&s, tryKind[ri]);
        if (expectSmall[ri]) {
            CHECK(kindChk == N48_RP_SHAPE_BYTES_SMALL, "%s: refused BYTES_SMALL (needs 0x4000, Apple gave 0x3000) (got %u)", row.n, kindChk);
        } else {
            CHECK(kindChk == N48_RP_SHAPE_OK, "%s: accepted (%s) (32bpp check %u, kind check %u)", row.n,
                  row.bytes == n48_rp_bytes(row.w, row.h) ? "exact" : "page-rounded", chk32, kindChk);
        }
    }
    // PLANTED BREAK ("exact-bytes again"): reverting to an EXACT-ONLY comparison (no page-rounding admitted) must
    // refuse P's two 256B_D rows, which shapes.c and the fix above both accept.
    {
        n48_rp_shape s {};
        s.hasMask = 1; s.maskOk = 1; s.maskSurf = 0x22; s.surf = 0x20;
        s.w = 128; s.h = 16; s.depth = 1; s.rowBytes = 128; s.bytes = 0x1000; s.gbRead = 1; s.gb = 0x08200545u;
        const uint32_t real = n48_rp_shape_check_kind(&s, N48_RP_KIND_256B_D_8);
        const uint64_t exact = n48_rp_kind_bytes(N48_RP_KIND_256B_D_8, s.w, s.h);
        const uint32_t exactOnly = (s.bytes != exact) ? N48_RP_SHAPE_BYTES : N48_RP_SHAPE_OK;
        const int caught = (real != exactOnly) ? 1 : 0;
        std::printf("  planted %-66s %s (real %u, exact-only %u)\n", "item 1: exact-bytes-only misses P's page-rounded 256B_D allocation",
                    caught ? "CAUGHT" : "MISSED", real, exactOnly);
        CHECK(caught != 0, "item 1's planted break (exact bytes again) is caught");
    }
    // PLANTED BREAK ("accepting a smaller allocation"): a mutant that also accepts bytes < exact (e.g. treating
    // ANY mismatch as fine to retile with whatever memory exists) must be caught by comparing against the REAL,
    // safe refusal on P's undersized s42 row.
    {
        n48_rp_shape s {};
        s.hasMask = 1; s.maskOk = 1; s.maskSurf = 0x36; s.surf = 0x20;
        s.w = 209; s.h = 41; s.depth = 1; s.rowBytes = 256; s.bytes = 0x3000; s.gbRead = 1; s.gb = 0x08200545u;
        const uint32_t real = n48_rp_shape_check_kind(&s, N48_RP_KIND_4KB_D_X_8);
        // the planted mutant: admit ANY byte count that is not LARGER than the rounded size (i.e. never refuses
        // for being too small - the exact defect the brief names).
        const uint64_t exact = n48_rp_kind_bytes(N48_RP_KIND_4KB_D_X_8, s.w, s.h), rounded = n48_rp_round4096(exact);
        const uint32_t acceptsSmall = (s.bytes > rounded) ? N48_RP_SHAPE_BYTES : N48_RP_SHAPE_OK;
        const int caught = (real != acceptsSmall) ? 1 : 0;
        std::printf("  planted %-66s %s (real %u, accepts-small %u)\n", "item 1: accepting a smaller-than-layout allocation",
                    caught ? "CAUGHT" : "MISSED", real, acceptsSmall);
        CHECK(caught != 0, "item 1's planted break (accepting a smaller allocation) is caught");
    }
}

// A fake VM: VA page -> VRAM page, as the kext's walk through the frame's own root answers.
struct FakeVm { uint64_t va[4], vram[4]; int n; };
static int fake_walk(const void *vm, uint64_t va, uint64_t *vram) {
    const FakeVm *f = static_cast<const FakeVm *>(vm);
    for (int i = 0; i < f->n; i++) if (f->va[i] == (va & ~0xfffull)) { *vram = f->vram[i]; return 1; }
    return 0;
}
// build 0.0.451 item 2 (S4): the fixed element size every test below asks with, matching good_copy()'s own
// N48_RP_G12_4KB_2D-mode, 32bpp-kind fixture (ws_resprov.h: "the 32 bpp path stores 4") - kept as ONE named
// constant so every existing D1-D8/J/hole1/hole4/Q2c test (none of which is ABOUT item 2) asks with the SAME value
// it recorded, unaffected by the new elemBytes comparison; test_shape_kind_elem below is the one that varies it.
static const uint32_t kElemBytes = 4u;
static n48_rp_copy good_copy() {
    n48_rp_copy c; std::memset(&c, 0, sizeof c);
    c.copied = 1; c.retiled = 1; c.bytes = 0x2000; c.compared = 0x800; c.mismatched = 0; c.ownerWs = 1; c.ctx = 6;
    c.vaOk = 1; c.va = 0x400006000ull; c.contiguous = 1; c.vram = 0x10079000ull; c.mode = N48_RP_G12_4KB_2D; c.arm = 2; c.epoch = 7;
    c.elemBytes = kElemBytes;   // build 0.0.451 item 2: the 32bpp kind's own element size (ws_resprov.h: "the 32 bpp path stores 4")
    return c;
}
static const FakeVm kVm = { { 0x400006000ull, 0x400007000ull }, { 0x10079000ull, 0x1007a000ull }, 2 };

// The operations under test, replaceable by planted defects.
struct Ops {
    const char *name;
    uint32_t (*record)(n48_rp *, const n48_rp_copy *);
    int (*ok)(n48_rp *, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, n48_rp_walk, const void *);
    void (*unmap)(n48_rp *, uint64_t);
    void (*rebind)(n48_rp *, uint64_t);
};
static uint32_t rec_real(n48_rp *t, const n48_rp_copy *c) { return n48_rp_record(t, c); }
static int ok_real(n48_rp *t, uint64_t c, uint64_t v, uint32_t m, uint32_t eb, uint32_t a, uint32_t e, n48_rp_walk w, const void *vm) { return n48_rp_ok(t, c, v, m, eb, a, e, w, vm); }
static void unmap_real(n48_rp *t, uint64_t c) { n48_rp_unmap(t, c); }
static void rebind_real(n48_rp *t, uint64_t c) { n48_rp_rebind(t, c); }
// D1: records an unverified copy (ignores the read-back)
static uint32_t rec_unverified(n48_rp *t, const n48_rp_copy *c) { n48_rp_copy k = *c; k.mismatched = 0; k.compared = k.bytes / 4; return n48_rp_record(t, &k); }
// D2: records any process's copy
static uint32_t rec_any_owner(n48_rp *t, const n48_rp_copy *c) { n48_rp_copy k = *c; k.ownerWs = 1; return n48_rp_record(t, &k); }
// D3: an unmap that does nothing
static void unmap_noop(n48_rp *t, uint64_t c) { (void)t; (void)c; }
// D4: a rebind that does nothing
static void rebind_noop(n48_rp *t, uint64_t c) { (void)t; (void)c; }
// D5: answers for any mode
static int ok_any_mode(n48_rp *t, uint64_t c, uint64_t v, uint32_t m, uint32_t eb, uint32_t a, uint32_t e, n48_rp_walk w, const void *vm) {
    (void)m; for (uint32_t k = 0; k < t->n; k++) if (t->e[k].ctx == c && t->e[k].va == v) return n48_rp_ok(t, c, v, t->e[k].mode, eb, a, e, w, vm); return 0; }
// D6: ignores the epoch
static int ok_no_epoch(n48_rp *t, uint64_t c, uint64_t v, uint32_t m, uint32_t eb, uint32_t a, uint32_t e, n48_rp_walk w, const void *vm) {
    (void)e; for (uint32_t k = 0; k < t->n; k++) if (t->e[k].ctx == c && t->e[k].va == v) return n48_rp_ok(t, c, v, m, eb, a, t->e[k].epoch, w, vm); return 0; }
// D7: never walks the VA (trusts the record)
static int walk_trust(const void *vm, uint64_t va, uint64_t *vram) { (void)vm; (void)va; (void)vram; return 1; }
static int ok_no_walk(n48_rp *t, uint64_t c, uint64_t v, uint32_t m, uint32_t eb, uint32_t a, uint32_t e, n48_rp_walk w, const void *vm) {
    (void)w; (void)eb;
    for (uint32_t k = 0; k < t->n; k++)
        if (t->e[k].ctx == c && t->e[k].va == v && t->e[k].mode == m && t->e[k].arm == a && t->e[k].epoch == e) {
            uint64_t dummy; (void)walk_trust(vm, v, &dummy); return 1;
        }
    return 0;
}
// D8: a byte-for-byte (un-re-tiled) copy of a 4KB_D_X resource is ledgered as 4KB_2D
static uint32_t rec_not_retiled(n48_rp *t, const n48_rp_copy *c) { n48_rp_copy k = *c; k.retiled = 1; return n48_rp_record(t, &k); }

// Scenario checks: returns the number of failed checks for these ops (0 = all hold).
static int scenario(const Ops &o) {
    int bad = 0; n48_rp t; std::memset(&t, 0, sizeof t);
#define S(c) do { if (!(c)) bad++; } while (0)
    n48_rp_copy c = good_copy();
    S(o.record(&t, &c) == N48_RP_REC_OK && t.n == 1);
    S(o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 7, &fake_walk, &kVm) == 1);                 // the positive
    S(o.ok(&t, 6, 0x400006000ull, 3, kElemBytes, 2, 7, &fake_walk, &kVm) == 0);                 // wrong mode (64KB_2D)
    S(o.ok(&t, 9, 0x400006000ull, 2, kElemBytes, 2, 7, &fake_walk, &kVm) == 0);                 // another context
    S(o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 8, &fake_walk, &kVm) == 0);                 // the epoch moved
    S(o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 3, 7, &fake_walk, &kVm) == 0);                 // the arm changed
    { FakeVm moved = kVm; moved.vram[1] = 0x20000000ull;                            // second page no longer our VRAM
      S(o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 7, &fake_walk, &moved) == 0); }
    { FakeVm gone = kVm; gone.n = 1;                                                 // second page unmapped
      S(o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 7, &fake_walk, &gone) == 0); }
    n48_rp_copy u = good_copy(); u.va = 0x400010000ull; u.vram = 0x10100000ull; u.mismatched = 1;
    S(o.record(&t, &u) == N48_RP_REC_UNVERIFIED);                                    // an unverified copy
    n48_rp_copy s = good_copy(); s.va = 0x400020000ull; s.vram = 0x10200000ull; s.ownerWs = 0;
    S(o.record(&t, &s) == N48_RP_REC_NOT_WS);                                         // SecurityAgent's / the draw client's copy
    n48_rp_copy r = good_copy(); r.va = 0x400030000ull; r.vram = 0x10300000ull; r.retiled = 0;
    S(o.record(&t, &r) == N48_RP_REC_NOT_RETILED);                                    // byte-for-byte copy claimed as gfx12
    S(t.n == 1);
    o.unmap(&t, 9); S(t.n == 1);                                                      // another context's unmap: kept
    o.unmap(&t, 6); S(t.n == 0 && o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 7, &fake_walk, &kVm) == 0);   // survives unmap?
    S(o.record(&t, &c) == N48_RP_REC_OK);
    o.rebind(&t, 6); S(t.n == 1);                                                     // same binding: kept
    o.rebind(&t, 11); S(t.n == 0 && o.ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 7, &fake_walk, &kVm) == 0);  // survives rebind?
    S(o.record(&t, &c) == N48_RP_REC_OK); o.rebind(&t, 0); S(t.n == 0);             // WindowServer gone: all go
#undef S
    return bad;
}

static void test_table() {
    n48_rp t; std::memset(&t, 0, sizeof t);
    n48_rp_copy c = good_copy();
    CHECK(n48_rp_record(&t, &c) == 0, "F3's copy recorded");
    CHECK(n48_rp_record(&t, &c) == 0 && t.n == 1, "the same VA again supersedes, n %u", t.n);
    n48_rp_copy o = good_copy(); o.va = 0x400100000ull; o.vram = 0x1007a000ull; o.bytes = 0x1000; o.compared = 0x400;
    CHECK(n48_rp_record(&t, &o) == 0 && t.n == 1 && t.e[0].va == 0x400100000ull, "a copy over the same VRAM supersedes");
    n48_rp_copy f; std::memset(&f, 0, sizeof f);
    CHECK(n48_rp_record(&t, &f) == N48_RP_REC_NOT_COPIED, "a zeroed copy is not a copy");
    c = good_copy(); c.compared = 0x7ff; CHECK(n48_rp_record(&t, &c) == N48_RP_REC_UNVERIFIED, "a short read-back is unverified");
    c = good_copy(); c.ctx = 0; CHECK(n48_rp_record(&t, &c) == N48_RP_REC_NO_CTX, "no context key");
    c = good_copy(); c.vaOk = 0; CHECK(n48_rp_record(&t, &c) == N48_RP_REC_NO_VA, "no VA");
    c = good_copy(); c.va = 0x400006100ull; CHECK(n48_rp_record(&t, &c) == N48_RP_REC_NO_VA, "unaligned VA");
    c = good_copy(); c.contiguous = 0; CHECK(n48_rp_record(&t, &c) == N48_RP_REC_SPLIT, "split VRAM");
    c = good_copy(); c.mode = 0; CHECK(n48_rp_record(&t, &c) == N48_RP_REC_MODE, "linear is not a tiled proof");
    CHECK(n48_rp_ok(&t, 6, 0x400006000ull, 2, kElemBytes, 2, 7, nullptr, &kVm) == 0, "no walk, no answer");
    // capacity: the 33rd distinct entry is refused, not recorded (build 0.0.552: n48_rp_record is switch 109 OFF: N48_RP_CAP_OFF)
    std::memset(&t, 0, sizeof t);
    for (uint32_t i = 0; i < N48_RP_CAP_OFF; i++) { c = good_copy(); c.va = 0x400000000ull + 0x10000ull * i; c.vram = 0x10000000ull + 0x10000ull * i; n48_rp_record(&t, &c); }
    c = good_copy(); c.va = 0x500000000ull; c.vram = 0x30000000ull;
    CHECK(n48_rp_record(&t, &c) == N48_RP_REC_FULL && t.n == N48_RP_CAP_OFF && N48_RP_CAP_OFF == 32u, "full table refuses");

    const Ops real = { "real", &rec_real, &ok_real, &unmap_real, &rebind_real };
    CHECK(scenario(real) == 0, "the real table holds every scenario (%d failed)", scenario(real));
    const Ops planted[] = {
        { "D1 an unverified copy (mismatched read-back) is ledgered", &rec_unverified, &ok_real, &unmap_real, &rebind_real },
        { "D2 a non-WindowServer copy is ledgered", &rec_any_owner, &ok_real, &unmap_real, &rebind_real },
        { "D3 an entry survives the context's unmapVA", &rec_real, &ok_real, &unmap_noop, &rebind_real },
        { "D4 an entry survives WindowServer's rebind", &rec_real, &ok_real, &unmap_real, &rebind_noop },
        { "D5 the wrong gfx12 mode is answered yes", &rec_real, &ok_any_mode, &unmap_real, &rebind_real },
        { "D6 a stale epoch is answered yes", &rec_real, &ok_no_epoch, &unmap_real, &rebind_real },
        { "D7 the VA is trusted without walking it to the copied VRAM", &rec_real, &ok_no_walk, &unmap_real, &rebind_real },
        { "D8 an un-re-tiled 4KB_D_X copy is ledgered as 4KB_2D", &rec_not_retiled, &ok_real, &unmap_real, &rebind_real },
    };
    unsigned caught = 0;
    for (const auto &p : planted) {
        const int f = scenario(p);
        std::printf("  planted %-66s %s (%d check(s) fail)\n", p.name, f ? "CAUGHT" : "MISSED", f);
        caught += f ? 1u : 0u;
    }
    CHECK(caught == sizeof(planted) / sizeof(planted[0]), "planted defects caught %u", caught);
    std::printf("ws_resprov: planted defects %u of %zu caught\n", caught, sizeof(planted) / sizeof(planted[0]));
}

// ---- G1 (T450 hole 1): the supersede fix ---------------------------------------------------------------------------------------
// The OLD (pre-T450) n48_rp_record: identical except the supersede loop only runs on the SUCCESS path, after the refusal
// return - exactly the bug MIB-A1-PROVENANCE.md Q3 hole 1 names.
static uint32_t rec_old_order(n48_rp *t, const n48_rp_copy *c) {
    uint32_t why = N48_RP_REC_OK;
    if (!c || c->copied != 1u) why = N48_RP_REC_NOT_COPIED;
    else if (!c->bytes || c->mismatched || c->compared * 4ull < c->bytes) why = N48_RP_REC_UNVERIFIED;
    else if (c->ownerWs != 1u) why = N48_RP_REC_NOT_WS;
    else if (!c->ctx) why = N48_RP_REC_NO_CTX;
    else if (c->vaOk != 1u || !c->va || (c->va & 0xfffull) || c->va >= (1ull << 48) || c->va + c->bytes > (1ull << 48))
        why = N48_RP_REC_NO_VA;
    else if (c->contiguous != 1u || (c->vram & 0xfffull)) why = N48_RP_REC_SPLIT;
    else if (!c->mode || c->mode > 7u) why = N48_RP_REC_MODE;
    else if (c->mode == N48_RP_G12_4KB_2D && c->retiled != 1u) why = N48_RP_REC_NOT_RETILED;
    if (why) { if (t) t->refused[why]++; return why; }   // OLD ORDER: returns BEFORE ever touching the table
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
        const n48_rp_ent *e = &t->e[k];
        const int sameVa = e->ctx == c->ctx && e->va == c->va;
        const int overVram = e->vram < c->vram + c->bytes && c->vram < e->vram + e->bytes;
        if (sameVa || overVram) n48_rp_drop_at(t, k); else k++;
    }
    if (t->n >= N48_RP_MAX) { t->refused[N48_RP_REC_FULL]++; return N48_RP_REC_FULL; }
    n48_rp_ent *e = &t->e[t->n++];
    e->ctx = c->ctx; e->va = c->va; e->vram = c->vram; e->bytes = c->bytes; e->mode = c->mode;
    e->arm = c->arm; e->epoch = c->epoch; e->elemBytes = c->elemBytes;
    t->recorded++;
    return N48_RP_REC_OK;
}
// An older good entry, then a failed re-copy of the SAME VA (wrote bytes, but the read-back was mismatched): the older
// entry must no longer answer.
static unsigned hole1_scenario(uint32_t (*rec)(n48_rp *, const n48_rp_copy *)) {
    unsigned bad = 0;
#define S(c) do { if (!(c)) bad++; } while (0)
    n48_rp t; std::memset(&t, 0, sizeof t);
    n48_rp_copy good = good_copy();
    S(rec(&t, &good) == N48_RP_REC_OK && t.n == 1);
    S(n48_rp_ok(&t, good.ctx, good.va, good.mode, good.elemBytes, good.arm, good.epoch, &fake_walk, &kVm) == 1);   // the older entry answers
    n48_rp_copy failed = good_copy(); failed.mismatched = 1;   // same VA/VRAM/ctx: wrote bytes, but verification failed
    S(rec(&t, &failed) == N48_RP_REC_UNVERIFIED);
    S(n48_rp_ok(&t, good.ctx, good.va, good.mode, good.elemBytes, good.arm, good.epoch, &fake_walk, &kVm) == 0);   // must NOT still answer
    S(t.n == 0);
#undef S
    return bad;
}
static void test_hole1_supersede() {
    CHECK(hole1_scenario(&n48_rp_record) == 0, "hole 1: n48_rp_record drops the stale entry before refusing (%u failed)",
          hole1_scenario(&n48_rp_record));
    const unsigned caught = hole1_scenario(&rec_old_order);
    std::printf("  planted %-66s %s (%u check(s) fail)\n", "P6 (hole 1) the old order: supersede runs AFTER the refusal return",
                caught ? "CAUGHT" : "MISSED", caught);
    CHECK(caught != 0, "hole 1's planted break (the old order) is caught");
}

// ---- G2 (T450 hole 4): the DECIDE -> COMMIT carry ------------------------------------------------------------------------------
// gfx_src_decide.h: enum { N48_SD_ARM_OFF = 0, N48_SD_ARM_DECIDE = 1, N48_SD_ARM_COMMIT = 2 }. Passed in literally, as every
// other file that mirrors these constants does (gfx_neuter.h N48_GFXN_ARM_COMMIT, ws_valid.h N48_WSV_ARM_COMMIT) - this
// header is not included here either.
static const uint32_t kDecide = 1u, kCommit = 2u;
typedef int (*OkCarryFn)(n48_rp *, uint64_t, uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, n48_rp_walk, const void *, uint32_t, uint32_t, uint32_t);
static int okcarry_real(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, uint32_t arm, uint32_t epoch, n48_rp_walk w,
                        const void *vm, uint32_t carry, uint32_t da, uint32_t ca) {
    return n48_rp_ok_carry(t, ctx, va, mode, elemBytes, arm, epoch, w, vm, carry, da, ca);
}
// P7: a defective carry that stops checking the epoch once the arm-carry condition holds (models "the carry forgets the
// epoch"). Everything else is identical to the real n48_rp_ok_carry.
static int okcarry_no_epoch(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, uint32_t arm, uint32_t /*epoch*/, n48_rp_walk w,
                            const void *vm, uint32_t carry, uint32_t da, uint32_t ca) {
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode || e->elemBytes != elemBytes) continue;
        const int armOk = (e->arm == arm) || (carry != 0u && arm == ca && e->arm == da);
        if (!armOk) return 0;   // the epoch is never compared - the defect
        if (!w || !vm) return 0;
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) { uint64_t v = ~0ull; if (w(vm, va + p, &v) != 1 || v != e->vram + p) return 0; }
        return 1;
    }
    return 0;
}
// build 0.0.451 item 5 (Q2c, reviewer's planted break, NOT caught): the arm-pair clause reduced to `carry !=
// 0u` alone - FAIL-OPEN, since it then admits an entry recorded at ANY arm level, asked at ANY OTHER arm level,
// as long as the caller merely turned the carry selector on (never checking `arm == ca` or `e->arm == da` at all).
// Epoch is still compared correctly (this models ONLY the arm-pair reduction, not P8/hole 4's separate defect).
static int okcarry_failopen_armpair(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, uint32_t arm, uint32_t epoch,
                                    n48_rp_walk w, const void *vm, uint32_t carry, uint32_t /*da*/, uint32_t /*ca*/) {
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode || e->elemBytes != elemBytes) continue;
        const int armOk = (e->arm == arm) || (carry != 0u);   // THE DEFECT: no arm-pair check at all
        if (!armOk || e->epoch != epoch) return 0;
        if (!w || !vm) return 0;
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) { uint64_t v = ~0ull; if (w(vm, va + p, &v) != 1 || v != e->vram + p) return 0; }
        return 1;
    }
    return 0;
}
static void test_hole4_carry() {
    n48_rp t; std::memset(&t, 0, sizeof t);
    n48_rp_copy c = good_copy(); c.arm = kDecide; c.epoch = 7;
    CHECK(n48_rp_record(&t, &c) == N48_RP_REC_OK, "recorded while the arm level was DECIDE");
    // selector OFF is byte-identical to n48_rp_ok for every argument combination below.
    for (uint32_t askArm : { kDecide, kCommit }) {
        const int carry0 = n48_rp_ok_carry(&t, c.ctx, c.va, c.mode, c.elemBytes, askArm, 7, &fake_walk, &kVm, 0, kDecide, kCommit);
        const int plain = n48_rp_ok(&t, c.ctx, c.va, c.mode, c.elemBytes, askArm, 7, &fake_walk, &kVm);
        CHECK(carry0 == plain, "carry off == n48_rp_ok when asked at arm %u (carry=%d plain=%d)", askArm, carry0, plain);
    }
    CHECK(n48_rp_ok_carry(&t, c.ctx, c.va, c.mode, c.elemBytes, kCommit, 7, &fake_walk, &kVm, 0, kDecide, kCommit) == 0,
          "selector off: a DECIDE entry does not answer at COMMIT (today's behaviour, unchanged)");
    // carry ON, same epoch: the DECIDE entry now answers at COMMIT.
    CHECK(n48_rp_ok_carry(&t, c.ctx, c.va, c.mode, c.elemBytes, kCommit, 7, &fake_walk, &kVm, 1, kDecide, kCommit) == 1,
          "carry on, same epoch: carried from DECIDE to COMMIT");
    CHECK(n48_rp_ok_carry(&t, c.ctx, c.va, c.mode, c.elemBytes, kDecide, 7, &fake_walk, &kVm, 1, kDecide, kCommit) == 1,
          "carry on: still answers at its own recorded arm level too");
    // refused after an epoch change.
    CHECK(n48_rp_ok_carry(&t, c.ctx, c.va, c.mode, c.elemBytes, kCommit, 8, &fake_walk, &kVm, 1, kDecide, kCommit) == 0,
          "carry on, epoch moved: refused");
    // refused after anything superseded it (an ordinary table lookup already enforces this - a dropped entry is gone).
    n48_rp_unmap(&t, c.ctx);
    CHECK(n48_rp_ok_carry(&t, c.ctx, c.va, c.mode, c.elemBytes, kCommit, 7, &fake_walk, &kVm, 1, kDecide, kCommit) == 0,
          "carry on, superseded (unmapped): refused");
    // planted break: a carry that forgets the epoch wrongly answers 1 across an epoch change.
    n48_rp t2; std::memset(&t2, 0, sizeof t2);
    n48_rp_copy c2 = good_copy(); c2.arm = kDecide; c2.epoch = 7;
    n48_rp_record(&t2, &c2);
    const int real7 = okcarry_real(&t2, c2.ctx, c2.va, c2.mode, c2.elemBytes, kCommit, 8, &fake_walk, &kVm, 1, kDecide, kCommit);
    const int broken7 = okcarry_no_epoch(&t2, c2.ctx, c2.va, c2.mode, c2.elemBytes, kCommit, 8, &fake_walk, &kVm, 1, kDecide, kCommit);
    CHECK(real7 == 0, "the real carry refuses the epoch change (control)");
    const int caught = (broken7 != real7) ? 1 : 0;
    std::printf("  planted %-66s %s (broken answers %d, real answers %d)\n", "P8 (hole 4) the carry forgets the epoch",
                caught ? "CAUGHT" : "MISSED", broken7, real7);
    CHECK(caught != 0, "hole 4's planted break (the carry forgets the epoch) is caught");

    // build 0.0.451 item 5 (Q2c): an entry recorded at an ARM LEVEL THAT IS NEITHER decideArm NOR commitArm
    // (arm 9, an arbitrary value - e.g. a future arm this file does not otherwise use), then asked AT commitArm
    // with the carry ON. The real rule must refuse it (e->arm(9) != arm(commit) and e->arm(9) != decideArm(1), so
    // NEITHER clause of `(e->arm == arm) || (carry != 0u && arm == ca && e->arm == da)` holds) - a fail-open mutant
    // that reduces the second clause to bare `carry != 0u` would wrongly admit it on the carry switch alone.
    n48_rp t3; std::memset(&t3, 0, sizeof t3);
    n48_rp_copy c3 = good_copy(); c3.arm = 9u; c3.epoch = 7;
    CHECK(n48_rp_record(&t3, &c3) == N48_RP_REC_OK, "Q2c setup: recorded at an arm level that is neither DECIDE nor COMMIT");
    const int q2cReal = okcarry_real(&t3, c3.ctx, c3.va, c3.mode, c3.elemBytes, kCommit, 7, &fake_walk, &kVm, 1, kDecide, kCommit);
    const int q2cBroken = okcarry_failopen_armpair(&t3, c3.ctx, c3.va, c3.mode, c3.elemBytes, kCommit, 7, &fake_walk, &kVm, 1, kDecide, kCommit);
    CHECK(q2cReal == 0, "Q2c the real carry refuses an off-pair arm level even with carry on and the epoch unchanged (control)");
    const int q2cCaught = (q2cBroken != q2cReal) ? 1 : 0;
    std::printf("  planted %-66s %s (broken answers %d, real answers %d)\n",
                "Q2c the arm-pair clause reduced to carry != 0u (FAIL-OPEN)", q2cCaught ? "CAUGHT" : "MISSED", q2cBroken, q2cReal);
    CHECK(q2cCaught != 0, "Q2c's planted break (the arm-pair clause reduced to carry != 0u) is caught");
}

// ---- D: the pending queue ------------------------------------------------------------------------------------------------
// A model of hw_resprov_note_copy + gfxsrc_rp_drain_locked over a fake lock, with the drop policy replaceable.
struct Led { n48_rp t; n48_rp_pq q; n48_rp_clog g; uint64_t copies; };
static n48_rp_copy copy_n(uint32_t i) {
    n48_rp_copy c = good_copy(); c.va = 0x400000000ull + 0x10000ull * i; c.vram = 0x10000000ull + 0x10000ull * i; c.ctx = 0; c.ownerWs = 0;
    return c;
}
static void drain(Led &L) {
    n48_rp_pend it[N48_RP_PEND];
    const uint32_t n = n48_rp_pq_take(&L.q, it, N48_RP_PEND);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t w = 0;
        if (!n48_rp_pend_fresh(&it[i], &L.g, &w)) { L.q.raced++; continue; }
        n48_rp_pend_who(&it[i], it[i].bound ? 6u : 0u);
        n48_rp_record(&L.t, &it[i].c);
    }
}
typedef void (*NoteFn)(Led &, const n48_rp_copy &, int busy, int32_t pid);
static void note_real(Led &L, const n48_rp_copy &c0, int busy, int32_t pid) {
    L.copies++;
    n48_rp_pend it; std::memset(&it, 0, sizeof it);
    it.clr = n48_rp_clr_mark(&L.g); it.copyNo = L.copies; it.c = c0; it.pid = pid; it.bound = 1; it.wsSeq = 6; it.wsPid = 1175;
    if (busy) { (void)n48_rp_pq_push(&L.q, &it); return; }
    drain(L);
    n48_rp_pend_who(&it, 6u);
    n48_rp_record(&L.t, &it.c);
}
// P3 A DROPPED RECORD: a busy lock loses the copy (0.0.364's behaviour).
static void note_drop(Led &L, const n48_rp_copy &c0, int busy, int32_t pid) { if (busy) { L.copies++; return; } note_real(L, c0, 0, pid); }
// P5 NO RACE CHECK AT ALL: a queued copy is recorded after a clear it did not see.
static void drain_no_gen(Led &L) {
    const uint64_t now = n48_rp_clr_mark(&L.g);
    for (auto &x : L.q.s) if (x.state == N48_RP_PS_READY) x.clr = now;
    drain(L);
}
// THE TABLE-WIDE CHECK (0.0.365's): ANY clear event since the copy refuses it, whatever it covered. hp7: raced 391/424.
static void drain_tablewide(Led &L) {
    n48_rp_pend it[N48_RP_PEND];
    const uint32_t n = n48_rp_pq_take(&L.q, it, N48_RP_PEND);
    const uint64_t now = n48_rp_clr_mark(&L.g);
    for (uint32_t i = 0; i < n; i++) {
        if (it[i].clr != now) { L.q.raced++; continue; }
        n48_rp_pend_who(&it[i], it[i].bound ? 6u : 0u);
        n48_rp_record(&L.t, &it[i].c);
    }
}
static int queue_scenario(NoteFn note, void (*drn)(Led &)) {
    int bad = 0;
#define S(c) do { if (!(c)) bad++; } while (0)
    static Led L; std::memset(&L, 0, sizeof L);
    // hp6's pattern: a third of the copies find the lock busy. Every verified WindowServer copy must end up recorded.
    for (uint32_t i = 0; i < 12; i++) note(L, copy_n(i), (i % 3) == 1, 1175);
    drn(L);
    S(L.t.n == 12 && L.t.recorded == 12);
    S(L.q.dropped == 0 && L.q.raced == 0);
    // Order: two queued copies of the SAME VA - the later one (other VRAM) must be what the table holds.
    std::memset(&L, 0, sizeof L);
    n48_rp_copy a = copy_n(1), b = copy_n(1); b.vram = 0x20000000ull;
    note(L, a, 1, 1175); note(L, b, 1, 1175); drn(L);
    S(L.t.n == 1 && L.t.e[0].vram == 0x20000000ull);
    // A clear COVERING THIS VA that lands after the copy and before the drain: the queued copy must NOT enter the table.
    std::memset(&L, 0, sizeof L);
    n48_rp_copy c2 = copy_n(2);
    note(L, c2, 1, 1175);
    n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 6u, c2.va, c2.bytes);   // this context unmapped exactly this range
    drn(L);
    S(L.t.n == 0 && L.q.raced == 1);
    // THE FIX: the SAME sequence with an unmap of a DIFFERENT range of the same context must RECORD it.
    std::memset(&L, 0, sizeof L);
    n48_rp_copy c2b = copy_n(2);
    note(L, c2b, 1, 1175);
    n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 6u, c2b.va + 0x100000ull, 0x1000ull);
    drn(L);
    S(L.t.n == 1 && L.q.raced == 0 && L.t.e[0].va == c2b.va);
    // ... and an unmap of ANOTHER context over the same numeric range must record it too.
    std::memset(&L, 0, sizeof L);
    note(L, copy_n(2), 1, 1175);
    n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 9u, copy_n(2).va, copy_n(2).bytes);
    drn(L);
    S(L.t.n == 1 && L.q.raced == 0);
    // A PARTIAL overlap of this context's unmap still refuses (one page of the range is enough).
    std::memset(&L, 0, sizeof L);
    n48_rp_copy c2c = copy_n(2);
    note(L, c2c, 1, 1175);
    n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 6u, c2c.va + c2c.bytes - 0x1000ull, 0x4000ull);
    drn(L);
    S(L.t.n == 0 && L.q.raced == 1);
    // An unmap of UNKNOWN extent (size 0) on this context refuses: fail closed.
    std::memset(&L, 0, sizeof L);
    note(L, copy_n(2), 1, 1175);
    n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 6u, 0ull, 0ull);
    drn(L);
    S(L.t.n == 0 && L.q.raced == 1);
    // A WindowServer drop covers everything, whatever the VA.
    std::memset(&L, 0, sizeof L);
    note(L, copy_n(2), 1, 1175);
    n48_rp_clr_push(&L.g, N48_RP_CLR_WS, 0u, 0ull, 0ull);
    drn(L);
    S(L.t.n == 0 && L.q.raced == 1);
    // More clear events than the log holds: the copy cannot be judged, so it is refused.
    std::memset(&L, 0, sizeof L);
    note(L, copy_n(2), 1, 1175);
    for (uint32_t i = 0; i <= N48_RP_CLR; i++) n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 9u, 0x900000000ull, 0x1000ull);
    drn(L);
    S(L.t.n == 0 && L.q.raced == 1);
    // hp7's PRESSURE: 1061 unmaps and 829 rebinds of OTHER ranges around one queued copy - which must still be recorded.
    std::memset(&L, 0, sizeof L);
    n48_rp_copy c3 = copy_n(4);
    note(L, c3, 1, 1175);
    for (uint32_t i = 0; i < 40u; i++) n48_rp_clr_push(&L.g, N48_RP_CLR_UNMAP, 6u, 0x800000000ull + 0x1000ull * i, 0x1000ull);
    drn(L);
    S(L.t.n == 1 && L.q.raced == 0);
    // A queued copy of another process stays refused as not WindowServer's.
    std::memset(&L, 0, sizeof L);
    note(L, copy_n(3), 1, 1292); drn(L);
    S(L.t.n == 0 && L.t.refused[N48_RP_REC_NOT_WS] == 1);
#undef S
    return bad;
}
static void test_queue() {
    static n48_rp_pq q; std::memset(&q, 0, sizeof q);
    n48_rp_pend it; std::memset(&it, 0, sizeof it);
    for (uint32_t i = 0; i < N48_RP_PEND; i++) { it.copyNo = i; CHECK(n48_rp_pq_push(&q, &it) == 1, "push %u", i); }
    CHECK(n48_rp_pq_push(&q, &it) == 0 && q.dropped == 1 && q.queued == N48_RP_PEND, "a full queue drops and COUNTS it");
    n48_rp_pend out[N48_RP_PEND];
    CHECK(n48_rp_pq_take(&q, out, N48_RP_PEND) == N48_RP_PEND && q.drained == N48_RP_PEND, "take drains every ready slot");
    bool ordered = true; for (uint32_t i = 0; i < N48_RP_PEND; i++) if (out[i].copyNo != i) ordered = false;
    CHECK(ordered, "drained in push order");
    CHECK(n48_rp_pq_take(&q, out, N48_RP_PEND) == 0, "empty after the drain");
    q.s[3].state = N48_RP_PS_FILL; CHECK(n48_rp_pq_take(&q, out, N48_RP_PEND) == 0, "a slot being filled is left alone");
    q.s[3].state = N48_RP_PS_FREE;
    it.pid = 1175; it.bound = 1; it.wsPid = 1175; n48_rp_pend_who(&it, 6u); CHECK(it.c.ownerWs == 1 && it.c.ctx == 6, "WHO: WindowServer");
    it.bound = 0; n48_rp_pend_who(&it, 6u); CHECK(it.c.ownerWs == 0 && it.c.ctx == 0, "WHO: not bound");
    it.bound = 1; it.pid = 0; n48_rp_pend_who(&it, 6u); CHECK(it.c.ownerWs == 0, "WHO: no pid");
    CHECK(queue_scenario(&note_real, &drain) == 0, "the real queue holds every scenario (%d failed)", queue_scenario(&note_real, &drain));
}
// ---- E: the clear log itself, and the PROVENANCE refusal line -------------------------------------------------------
static void test_clog() {
    static n48_rp_clog g; std::memset(&g, 0, sizeof g);
    uint32_t w = 0xff;
    CHECK(n48_rp_clr_mark(&g) == 0ull, "an empty log is at 0");
    CHECK(n48_rp_clr_clean(&g, 0ull, 6u, 0x400006000ull, 0x2000ull, &w) == 1 && w == N48_RP_RACE_NONE, "nothing published: clean");
    const uint64_t at = n48_rp_clr_mark(&g);
    n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 6u, 0x400000000ull, 0x1000ull);
    CHECK(n48_rp_clr_clean(&g, at, 6u, 0x400006000ull, 0x2000ull, &w) == 1, "a disjoint unmap of the same context is clean");
    CHECK(n48_rp_clr_clean(&g, at, 6u, 0x400000000ull, 0x1000ull, &w) == 0 && w == N48_RP_RACE_VA, "its own range is not");
    CHECK(n48_rp_clr_clean(&g, at + 1ull, 6u, 0x400000000ull, 0x1000ull, &w) == 1, "an event BEFORE the copy does not count");
    n48_rp_clr_push(&g, N48_RP_CLR_WS, 0u, 0ull, 0ull);
    CHECK(n48_rp_clr_clean(&g, at, 6u, 0x400006000ull, 0x2000ull, &w) == 0 && w == N48_RP_RACE_WS, "a WindowServer drop covers all");
    // The log wraps: anything older than N48_RP_CLR events cannot be judged.
    std::memset(&g, 0, sizeof g);
    const uint64_t old0 = n48_rp_clr_mark(&g);
    for (uint32_t i = 0; i < N48_RP_CLR; i++) n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 9u, 0x900000000ull, 0x1000ull);
    CHECK(n48_rp_clr_clean(&g, old0, 6u, 0x400006000ull, 0x2000ull, &w) == 1, "exactly N48_RP_CLR events are still readable");
    n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 9u, 0x900000000ull, 0x1000ull);
    CHECK(n48_rp_clr_clean(&g, old0, 6u, 0x400006000ull, 0x2000ull, &w) == 0 && w == N48_RP_RACE_WRAP, "one more and it wrapped");
    // A slot caught mid-write (stamp 0) is unknown, so it refuses.
    std::memset(&g, 0, sizeof g);
    const uint64_t at2 = n48_rp_clr_mark(&g);
    n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 9u, 0x900000000ull, 0x1000ull);
    g.s[0].stamp = 0ull;
    CHECK(n48_rp_clr_clean(&g, at2, 6u, 0x400006000ull, 0x2000ull, &w) == 0 && w == N48_RP_RACE_TORN, "a torn slot refuses");
    // An unknown context on EITHER side is treated as covering.
    std::memset(&g, 0, sizeof g);
    const uint64_t at3 = n48_rp_clr_mark(&g);
    n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 0u, 0x400006000ull, 0x2000ull);
    CHECK(n48_rp_clr_clean(&g, at3, 6u, 0x400006000ull, 0x2000ull, &w) == 0, "an unmap with no context covers every context");
    std::memset(&g, 0, sizeof g);
    const uint64_t at4 = n48_rp_clr_mark(&g);
    n48_rp_clr_push(&g, N48_RP_CLR_UNMAP, 6u, 0x400006000ull, 0x2000ull);
    CHECK(n48_rp_clr_clean(&g, at4, 0u, 0x400006000ull, 0x2000ull, &w) == 0, "a copy with no context is covered by any unmap");
    CHECK(n48_rp_clr_clean(&g, at4, 6u, 0x400006000ull, 0ull, &w) == 0, "a copy of unknown extent is covered too");
}

// D3: the refusal line must NAME the surface VA and fit the kext's line cap. The defect is hp7's line, which named nothing.
#define PROV_NOVA_FMT "gfx-xlat:   descriptor PROVENANCE REFUSED (segment %u)"
static int prov_line_check(const char *line, uint64_t va) {
    char hex[32];
    std::snprintf(hex, sizeof hex, "%#llx", (unsigned long long)va);
    int bad = 0;
    if (std::strlen(line) >= N48_RP_PROV_LINE_CAP) bad++;      // the kext truncates at 512 bytes
    if (!std::strstr(line, hex)) bad++;                        // the VA itself, not just a count
    return bad;
}
static void test_prov_line() {
    char buf[2048];
    std::snprintf(buf, sizeof buf, N48_RP_PROV_FMT,
                  N48_RP_PROV_ARGS(0x400006000ull, 2u, 2u, 0x7f1234567800ull, 0xa33284799e5b1178ull, 41, 6ull, 725ull, 3u));
    CHECK(prov_line_check(buf, 0x400006000ull) == 0, "the refusal line names F3's VA and fits the cap: %s", buf);
    CHECK(std::strstr(buf, "a33284799e5b1178") != nullptr, "and the fragment program's key");
    // Worst case: every field at its widest.
    std::snprintf(buf, sizeof buf, N48_RP_PROV_FMT,
                  N48_RP_PROV_ARGS(0xffffffffffffffffull, 31u, 2u, 0xffffffffffffffffull, 0xffffffffffffffffull, -2147483647 - 1,
                                   0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffu));
    CHECK(std::strlen(buf) < N48_RP_PROV_LINE_CAP, "worst case %zu bytes < %u", std::strlen(buf), N48_RP_PROV_LINE_CAP);
    char bad[2048];
    std::snprintf(bad, sizeof bad, PROV_NOVA_FMT, 3u);
    const int caught = prov_line_check(bad, 0x400006000ull);
    std::printf("  planted %-66s %s (%d check(s) fail)\n", "D3 a PROVENANCE refusal line with no VA (hp7's)",
                caught ? "CAUGHT" : "MISSED", caught);
    CHECK(caught != 0, "a refusal line without the VA is caught");
}

static void test_planted_794() {
    struct { const char *name; int failed; } p[] = {
        { "P1 the swizzle read from res+0x1dc only (the wrong field)", shape_scenario(&shape_wrong_field) },
        { "P2 the old pitch rule rowBytes == 4 x width", shape_scenario(&shape_old_pitch) },
        { "P3 a busy lock drops the copy's record", queue_scenario(&note_drop, &drain) },
        { "P5 a queued copy is recorded after a clear it missed", queue_scenario(&note_real, &drain_no_gen) },
        { "D1 the MASK cross-check that is stricter than the hardware", shape_scenario(&shape_strict_mask) },
        { "D1b an unreadable res+0x180 record is trusted anyway", shape_scenario(&shape_trust_unreadable) },
        { "D2 a TABLE-WIDE generation check instead of per-VA", queue_scenario(&note_real, &drain_tablewide) },
    };
    unsigned caught = 0;
    for (const auto &x : p) {
        std::printf("  planted %-66s %s (%d check(s) fail)\n", x.name, x.failed ? "CAUGHT" : "MISSED", x.failed);
        caught += x.failed ? 1u : 0u;
    }
    CHECK(caught == sizeof(p) / sizeof(p[0]), " planted defects caught %u", caught);
    std::printf("ws_resprov: planted defects %u of %zu caught\n", caught, sizeof(p) / sizeof(p[0]));
}

// build 0.0.451 item 2 (S4) - THE TWO COLLISIONS FROM shapes.c REFUSE AT THE ASK. The shape check alone
// cannot tell a 128 bpp 4KB_D_X resource with w <= 16 from a real 64 bpp one (both satisfy the SAME pitch
// formula), nor a 16 bpp resource with w <= 32 from a real 32 bpp one - so a SECOND, independent check at the ASK
// (elemBytes) is what actually refuses the wrong match, exactly as the brief requires.
static void test_elem_ask_collisions() {
    // Collision 1: recorded as the 64 bpp kind (elemBytes 8, e.g. a REAL 64bpp resource), asked with a 128bpp T#'s
    // own elemBytes (16) - the shapes.c scenario "128bpp 4KB_D_X, w <= 16 also passes the 64bpp kind's pitch".
    { n48_rp t; std::memset(&t, 0, sizeof t);
      n48_rp_copy c = good_copy(); c.elemBytes = 8u;   // the 64bpp kind
      CHECK(n48_rp_record(&t, &c) == N48_RP_REC_OK, "collision 1 setup: recorded as the 64bpp kind");
      CHECK(n48_rp_ok(&t, c.ctx, c.va, c.mode, 8u, c.arm, c.epoch, &fake_walk, &kVm) == 1,
            "collision 1: the SAME elemBytes (64bpp asking about its own real 64bpp copy) still proves");
      CHECK(n48_rp_ok(&t, c.ctx, c.va, c.mode, 16u, c.arm, c.epoch, &fake_walk, &kVm) == 0,
            "collision 1: a 128bpp T# (elemBytes 16) asking about the SAME (mode, VA) the 64bpp kind recorded is REFUSED"); }
    // Collision 2: recorded as the 32 bpp kind (elemBytes 4), asked with a 16bpp T#'s own elemBytes (2) - "a 16bpp
    // resource with w <= 32 also passes the 32bpp path".
    { n48_rp t; std::memset(&t, 0, sizeof t);
      n48_rp_copy c = good_copy();   // elemBytes already kElemBytes == 4 (the 32bpp kind)
      CHECK(n48_rp_record(&t, &c) == N48_RP_REC_OK, "collision 2 setup: recorded as the 32bpp kind");
      CHECK(n48_rp_ok(&t, c.ctx, c.va, c.mode, 2u, c.arm, c.epoch, &fake_walk, &kVm) == 0,
            "collision 2: a 16bpp T# (elemBytes 2) asking about the SAME (mode, VA) the 32bpp kind recorded is REFUSED"); }
    // PLANTED BREAK ("the comparison removed"): a mutant n48_rp_ok that never checks elemBytes at all would wrongly
    // prove collision 1's mismatched ask.
    { n48_rp t; std::memset(&t, 0, sizeof t);
      n48_rp_copy c = good_copy(); c.elemBytes = 8u;
      n48_rp_record(&t, &c);
      auto ok_no_elem_check = [](n48_rp *tt, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t /*eb*/, uint32_t arm,
                                 uint32_t epoch, n48_rp_walk w, const void *vm) -> int {
          tt->asked++;
          if (!ctx) return 0;
          for (uint32_t k = 0; k < tt->n && k < N48_RP_MAX; k++) {
              const n48_rp_ent *e = &tt->e[k];
              if (e->ctx != ctx || e->va != va || e->mode != mode) continue;   // NO elemBytes check at all
              if (e->arm != arm || e->epoch != epoch) return 0;
              if (!w || !vm) return 0;
              for (uint64_t p = 0; p < e->bytes; p += 4096ull) { uint64_t v = ~0ull; if (w(vm, va + p, &v) != 1 || v != e->vram + p) return 0; }
              return 1;
          }
          return 0;
      };
      const int real = n48_rp_ok(&t, c.ctx, c.va, c.mode, 16u, c.arm, c.epoch, &fake_walk, &kVm);
      const int broken = ok_no_elem_check(&t, c.ctx, c.va, c.mode, 16u, c.arm, c.epoch, &fake_walk, &kVm);
      const int caught = (real != broken) ? 1 : 0;
      std::printf("  planted %-66s %s (real %d, no-elem-check %d)\n", "item 2: the comparison removed (elemBytes never checked)",
                  caught ? "CAUGHT" : "MISSED", real, broken);
      CHECK(caught != 0, "item 2's planted break (the comparison removed) is caught"); }
}

// build 0.0.451 item 9 (reviewer-confirmed via decide43): n48_rp_unmap_rng is RANGE-SCOPED, not context-wide.
static void test_unmap_range_scoped() {
    n48_rp t; std::memset(&t, 0, sizeof t);
    n48_rp_copy c = good_copy();   // va 0x400006000, bytes 0x2000 -> [0x400006000, 0x400008000)
    CHECK(n48_rp_record(&t, &c) == N48_RP_REC_OK, "setup: recorded");
    // an UNRELATED unmap (a different range entirely, e.g. the copy's own SOURCE buffer at a far VA) keeps the entry.
    n48_rp_unmap_rng(&t, c.ctx, 0x400100000ull, 0x1000ull);
    CHECK(t.n == 1 && n48_rp_ok(&t, c.ctx, c.va, c.mode, c.elemBytes, c.arm, c.epoch, &fake_walk, &kVm) == 1,
          "an unrelated unmap (a different VA range) KEEPS the entry");
    // an unmap of a DIFFERENT context, even overlapping, keeps it (context still matters).
    n48_rp_unmap_rng(&t, c.ctx + 1u, c.va, c.bytes);
    CHECK(t.n == 1, "an unmap of a different context, even overlapping the range, keeps the entry");
    // an OVERLAPPING unmap (the copy's own range, or any range that intersects it) drops it.
    n48_rp_unmap_rng(&t, c.ctx, c.va, c.bytes);
    CHECK(t.n == 0 && n48_rp_ok(&t, c.ctx, c.va, c.mode, c.elemBytes, c.arm, c.epoch, &fake_walk, &kVm) == 0,
          "an overlapping unmap DROPS the entry");
    // size 0 means "the whole context" (scope unknown) - drops everything of that context, matching n48_rp_unmap.
    n48_rp_record(&t, &c);
    n48_rp_unmap_rng(&t, c.ctx, 0ull, 0ull);
    CHECK(t.n == 0, "size 0 (unknown scope) drops the whole context, matching the old context-wide behaviour");
    // PLANTED BREAK ("the context-wide drop"): the OLD n48_rp_unmap must differ from the range-scoped fix for an
    // unrelated unmap - exactly decide43's own AL #60/#61 scenario (a copy's SOURCE buffer unmapped elsewhere).
    { n48_rp t2; std::memset(&t2, 0, sizeof t2);
      n48_rp_copy c2 = good_copy();
      n48_rp_record(&t2, &c2);
      n48_rp t3 = t2;   // a second, identical table to compare the OLD behaviour against
      n48_rp_unmap_rng(&t2, c2.ctx, 0x400100000ull, 0x1000ull);   // the fix: unrelated range, kept
      n48_rp_unmap(&t3, c2.ctx);                                   // the OLD context-wide behaviour: dropped regardless
      const int caught = (t2.n != t3.n) ? 1 : 0;
      std::printf("  planted %-66s %s (range-scoped n=%u, context-wide n=%u)\n",
                  "item 9: the context-wide drop (address ignored)", caught ? "CAUGHT" : "MISSED", t2.n, t3.n);
      CHECK(caught != 0, "item 9's planted break (the context-wide drop) is caught"); }
}

// build 0.0.452 item 4 (K5, reviewer test gap NOT CAUGHT by 0.0.451's own tests): the overlap check must be
// EXCLUSIVE at both ends of the half-open range [va, va+size) - an unmap that only TOUCHES an entry's range at its
// very boundary (ends exactly where the entry starts, or starts exactly where the entry ends) must NOT drop it.
// n48_rp_unmap_rng's real overlap test is `e->va < va + size && va < e->va + e->bytes` (strict `<` both sides);
// the brief names the wrong shape explicitly: `e->va <= va + size && va <= e->va + e->bytes` would drop a
// boundary-touching entry it must keep.
static void test_unmap_boundary_exclusive() {
    n48_rp_copy c = good_copy();   // va 0x400006000, bytes 0x2000 -> [0x400006000, 0x400008000)
    // unmap [0x400008000, 0x400009000) starts exactly where the entry ENDS - touches, must survive.
    { n48_rp t; std::memset(&t, 0, sizeof t);
      n48_rp_record(&t, &c);
      n48_rp_unmap_rng(&t, c.ctx, 0x400008000ull, 0x1000ull);
      CHECK(t.n == 1 && n48_rp_ok(&t, c.ctx, c.va, c.mode, c.elemBytes, c.arm, c.epoch, &fake_walk, &kVm) == 1,
            "K5 an unmap starting exactly where the entry ends (touching, not overlapping) KEEPS the entry"); }
    // unmap [0x400004000, 0x400006000) ends exactly where the entry STARTS - touches, must survive.
    { n48_rp t; std::memset(&t, 0, sizeof t);
      n48_rp_record(&t, &c);
      n48_rp_unmap_rng(&t, c.ctx, 0x400004000ull, 0x2000ull);
      CHECK(t.n == 1 && n48_rp_ok(&t, c.ctx, c.va, c.mode, c.elemBytes, c.arm, c.epoch, &fake_walk, &kVm) == 1,
            "K5 an unmap ending exactly where the entry starts (touching, not overlapping) KEEPS the entry"); }
    // PLANTED BREAK: the brief's own named wrong shape (<=  on both sides) drops both boundary-touching cases.
    { n48_rp t; std::memset(&t, 0, sizeof t);
      n48_rp_record(&t, &c);
      const n48_rp_ent *e = &t.e[0];
      const uint64_t va = 0x400008000ull, size = 0x1000ull;
      const int brokenOverlaps = (e->va <= va + size) && (va <= e->va + e->bytes);   // the named wrong shape
      const int fixedOverlaps  = (e->va <  va + size) && (va <  e->va + e->bytes);   // the real n48_rp_unmap_rng check
      const int caught = (brokenOverlaps != 0 && fixedOverlaps == 0) ? 1 : 0;
      std::printf("  planted %-66s %s\n", "K5: inclusive (<=) boundary check drops a touching entry",
                  caught ? "CAUGHT" : "MISSED");
      CHECK(caught != 0, "K5's planted break (inclusive <= boundary) is caught"); }
}

// build 0.0.452 item 4 (K7, reviewer test gap NOT CAUGHT by 0.0.451's own tests): gcap_census_step
// (AppleHardwareHook.cpp) must charge `*censusBudget` only for a NON-duplicate row - the dup check has to run
// BEFORE the decrement. This test does not link AppleHardwareHook.cpp (it has no host-buildable harness here);
// it replicates the exact per-row order gcap_census_step uses (dedup-then-decrement) against a tiny local model,
// then plants the described break (decrement moved BEFORE the dup check) and proves the two orders diverge on a
// frame with a repeated (va, idx, sgpr) key - which is exactly what would let a break survive undetected.
static void test_census_dup_not_charged_to_budget() {
    struct Row { uint64_t va; uint32_t idx, sgpr; };
    const Row rows[3] = { {0x1000ull, 1u, 2u}, {0x1000ull, 1u, 2u} /* exact dup of row 0 */, {0x2000ull, 3u, 4u} };
    // FIXED order (matches gcap_census_step: dup check, then continue; decrement only for a real new row).
    {
        uint64_t seenVa[8]; uint32_t seenIdx[8], seenSgpr[8], seenN = 0, budget = 2u, attempted = 0, dupN = 0;
        for (uint32_t k = 0; k < 3; k++) {
            if (!budget) continue;
            attempted++;
            int dup = 0;
            for (uint32_t z = 0; z < seenN; z++)
                if (seenVa[z] == rows[k].va && seenIdx[z] == rows[k].idx && seenSgpr[z] == rows[k].sgpr) { dup = 1; break; }
            if (dup) { dupN++; continue; }
            budget--;
            seenVa[seenN] = rows[k].va; seenIdx[seenN] = rows[k].idx; seenSgpr[seenN] = rows[k].sgpr; seenN++;
        }
        // 3 rows, 1 duplicate, budget 2: both distinct rows fit - the dup must NOT have consumed budget.
        CHECK(attempted == 3u && dupN == 1u && budget == 0u && seenN == 2u,
              "K7 fixed order: the duplicate row is not charged to censusBudget (both distinct rows still fit, "
              "attempted %u dup %u budget %u seen %u)", attempted, dupN, budget, seenN);
    }
    // BROKEN order (the planted break: decrement moved BEFORE the dup check, exactly as the brief describes).
    {
        uint64_t seenVa[8]; uint32_t seenIdx[8], seenSgpr[8], seenN = 0, budget = 2u, dupN = 0, budgetOut = 0;
        for (uint32_t k = 0; k < 3; k++) {
            if (!budget) { budgetOut++; continue; }
            budget--;   // BROKEN: charged before the dup check even looks at the row
            int dup = 0;
            for (uint32_t z = 0; z < seenN; z++)
                if (seenVa[z] == rows[k].va && seenIdx[z] == rows[k].idx && seenSgpr[z] == rows[k].sgpr) { dup = 1; break; }
            if (dup) { dupN++; continue; }
            seenVa[seenN] = rows[k].va; seenIdx[seenN] = rows[k].idx; seenSgpr[seenN] = rows[k].sgpr; seenN++;
        }
        // same 3 rows, budget 2: the broken order runs out of budget on the (charged) duplicate and NEVER reaches
        // row 2 (the third, distinct row) - it is starved by a row that should have been free.
        const int caught = (budgetOut != 0u && seenN < 2u) ? 1 : 0;
        std::printf("  planted %-66s %s (fixed seenN=2, broken seenN=%u, budgetOut=%u, dup still seen %u)\n",
                    "K7: censusBudget decrement moved before the dup check", caught ? "CAUGHT" : "MISSED", seenN, budgetOut, dupN);
        CHECK(caught != 0, "K7's planted break (dup charged to budget) is caught"); }
}

// ==== L. build 0.0.486, GENERALISED BY 0.0.492 - the backing-sourced copy (switch 59; ws_resprov.h section 6) =============
// L1 goldens: the generic gfx12 2D equation n48_g12_off2d reproduces EVERY golden row of the vendored addrlib (kG12, kG12_4kbdx8,
//    kG12_4kbdx64, kG12_256d8, kG12_256d64, kG12_64k32 and kG12Real = the 41 records' own (mode, bpp, dims)) and equals the four
//    hand-written gfx12 functions; and n48_rp_lin_to_g12 (the STREAMED conversion, chunk by chunk) lands every texel of a LINEAR
//    source (0x100 header, padded pitch in ELEMENTS) at addrlib's offset for every row - the wallpaper's 1920x1080 in 9 chunks.
// L2 round trip through a reference reader (gfx12 -> linear over the golden-tested equation) at every record shape; the header and
//    the row padding never reach the image.
// L3 n48_rp_lin_check: every refusal driven on its own; the FORMAT PAIRING (L3b, the table bytes); the VRAM-mode map against
//    xlat12_desc.h's kXlat12SwModeG10ToG12 (L3c); the VRAM pitch against kG10Pitch (L3d); the STREAMED copy's bounds (L3e).
// L4 the ask per mode (a T# of the VRAM side's own SW_MODE only); OFF identity of n48_rp_ok/_carry/_t (0.0.483) and of
//    n48_rp_record (0.0.491) for every non-lin copy. L5 the colour-target drop. L6 the step-0 / report line widths. L7 the COPIED
//    suffixes. L8 the switch gate and the gfx10 re-tiles' refusal (n48_rp_lin_blocks_old).
// L9 REACHABILITY in the kext's order for ALL 41 run10f records: fields -> check -> gate -> streamed conversion -> n48_rp_copy as
//    the kext fills it -> record -> the translator's own ask with a T# of the record's shape -> proven + clamp.
// L10 THE 41 RECORDS' VERDICTS (fixture_run10f_backing.h): which convert (mode, bpp, gfx12 length, chunks), which refuse and why.
#include "fixture_run10f_backing.h"
static const char *kPeerPfx = "Navi48AccelPeer: ";
static const uint64_t kMiB = 1ull << 20;
static uint32_t blk_of_m12(uint32_t m12) { return m12 == 1u ? 8u : m12 == 2u ? 12u : m12 == 3u ? 16u : 0u; }
static uint32_t log2u(uint32_t v) { uint32_t l = 0; while ((1u << l) < v) l++; return l; }
// The accepted format pair for an element size (the only three: kN48RpFmt).
static void fmt_for(uint32_t eb, uint32_t &res, uint32_t &bk) {
    res = eb == 1u ? 0x02u : eb == 4u ? 0x0fu : 0x2bu;
    bk = eb == 1u ? 0x63u : eb == 4u ? 0x65u : 0x68u;
}
static uint32_t g10_of_m12(uint32_t m12) { return m12 == 1u ? 2u : m12 == 2u ? 22u : 27u; }
// A synthetic record that passes n48_rp_lin_check for (gfx12 mode, element size, w, h), with a 0x100 header and pitch w + pad.
static n48_rp_lin_in lin_synth(uint32_t m12, uint32_t eb, uint32_t w, uint32_t h, uint32_t pad, uint64_t off) {
    n48_rp_lin_in in {};
    uint32_t fr = 0, fb = 0; fmt_for(eb, fr, fb);
    in.nibble = 8; in.fmtRes = fr; in.w = w; in.h = h; in.depth = 1;
    in.rowBytes = eb * n48_rp_lin_g10_pitch(blk_of_m12(m12), log2u(eb), w);
    in.bytes = 1ull << 30; in.recOk = 1; in.bw = w; in.bh = h; in.rec04 = 1; in.fmtBk = fb; in.off = off; in.pitch = w + pad;
    in.swzVram = g10_of_m12(m12) | (1u << 5); in.swzBk = 0x601; in.srcLen = 1ull << 30; in.mdLen = 1ull << 30; in.dstLen = 1ull << 30;
    return in;
}
static n48_rp_lin_in fx_in(const n48_fx_backing &r) {
    n48_rp_lin_in in {};
    in.nibble = r.nibble; in.fmtRes = r.fmtRes; in.w = r.w; in.h = r.h; in.depth = r.depth; in.rowBytes = r.rowBytes;
    in.firstMip = r.firstMip; in.hasE0 = r.hasE0; in.bytes = r.bytes; in.backingOffset = r.backingOffset; in.recOk = r.recOk;
    in.bw = r.bw; in.bh = r.bh; in.rec04 = r.rec04; in.fmtBk = r.fmtBk; in.off = r.off; in.pitch = r.pitch; in.swzVram = r.swzVram;
    in.swzBk = r.swzBk; in.srcLen = r.srcLen; in.mdLen = r.mdLen; in.dstLen = r.dstLen;
    return in;
}
// Where did each linear texel land? Every element carries its index (eb >= 4: dword 0 = 0x80000000 | idx, the rest idx-derived;
// eb 1: three conversions carry the index a byte at a time). false on any texel not landing exactly once, or a non-zero pad byte.
typedef int (*ConvFn)(const n48_rp_lin_plan *, uint32_t, uint32_t, const uint8_t *, uint64_t, uint8_t *, uint64_t);
static bool lin_landing(const n48_rp_lin_plan &p, uint32_t w, uint32_t h, std::vector<uint32_t> &where, ConvFn conv) {
    const uint32_t eb = p.bpe;
    std::vector<uint8_t> src((size_t)p.srcEnd + 64, 0xEEu), dst((size_t)p.g12Bytes, 0xCDu);
    where.assign((size_t)w * h, 0xFFFFFFFFu);
    if (eb >= 4u) {
        for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
            const uint32_t i = y * w + x, v = 0x80000000u | i;
            uint8_t *s = &src[(size_t)(p.srcOff + y * p.pitchBytes + (uint64_t)x * eb)];
            std::memcpy(s, &v, 4);
            for (uint32_t k = 4; k < eb; k++) s[k] = (uint8_t)(i * 7u + k);
        }
        if (!conv(&p, w, h, src.data(), src.size(), dst.data(), dst.size())) return false;
        for (size_t d = 0; d + eb <= dst.size(); d += eb) {
            uint32_t v; std::memcpy(&v, &dst[d], 4);
            if (!v) { for (uint32_t k = 4; k < eb; k++) if (dst[d + k]) return false; continue; }
            const uint32_t i = v & 0x7fffffffu;
            if (!(v >> 31) || i >= w * h || where[i] != 0xFFFFFFFFu) return false;
            for (uint32_t k = 4; k < eb; k++) if (dst[d + k] != (uint8_t)(i * 7u + k)) return false;
            where[i] = (uint32_t)d;
        }
    } else {
        std::vector<uint32_t> idx(dst.size(), 0);
        for (uint32_t plane = 0; plane < 3; plane++) {
            for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++)
                src[(size_t)(p.srcOff + y * p.pitchBytes + x)] = (uint8_t)(((y * w + x + 1u) >> (8 * plane)) & 0xffu);
            if (!conv(&p, w, h, src.data(), src.size(), dst.data(), dst.size())) return false;
            for (size_t d = 0; d < dst.size(); d++) idx[d] |= (uint32_t)dst[d] << (8 * plane);
        }
        for (size_t d = 0; d < dst.size(); d++) {
            if (!idx[d]) continue;
            const uint32_t i = idx[d] - 1u;
            if (i >= w * h || where[i] != 0xFFFFFFFFu) return false;
            where[i] = (uint32_t)d;
        }
    }
    for (uint32_t i = 0; i < w * h; i++) if (where[i] == 0xFFFFFFFFu) return false;
    return true;
}
static uint64_t lin_where_fnv(const std::vector<uint32_t> &where, uint32_t w, uint32_t h) {
    uint64_t f = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) f = fnv(f, where[(size_t)y * w + x]);
    return f;
}
static int conv_real(const n48_rp_lin_plan *p, uint32_t w, uint32_t h, const uint8_t *s, uint64_t sn, uint8_t *d, uint64_t dn) {
    return n48_rp_lin_to_g12(p, w, h, s, sn, d, dn);
}
static uint64_t eq_fnv(uint32_t blk, uint32_t bl, uint32_t w, uint32_t h, uint32_t pitchElems) {
    uint32_t bwl = 0, bhl = 0; (void)n48_g12_blk(blk, bl, &bwl, &bhl);
    const uint32_t pb = pitchElems >> bwl;
    uint64_t f = 0xcbf29ce484222325ull;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) f = fnv(f, n48_g12_off2d(blk, bl, x, y, pb));
    return f;
}
struct GRow { uint32_t m12, bpp, w, h, pitch; uint64_t fnv; };
static std::vector<GRow> all_golden_rows() {
    std::vector<GRow> v;
    for (const auto &g : kG12) v.push_back({ 2, 32, g.w, g.h, g.pitch, g.fnv });
    for (const auto &g : kG12_4kbdx8) v.push_back({ 2, 8, g.w, g.h, g.pitch, g.fnv });
    for (const auto &g : kG12_4kbdx64) v.push_back({ 2, 64, g.w, g.h, g.pitch, g.fnv });
    for (const auto &g : kG12_256d8) v.push_back({ 1, 8, g.w, g.h, g.pitch, g.fnv });
    for (const auto &g : kG12_256d64) v.push_back({ 1, 64, g.w, g.h, g.pitch, g.fnv });
    for (const auto &g : kG12_64k32) v.push_back({ 3, 32, g.w, g.h, g.pitch, g.fnv });
    for (const auto &g : kG12Real) v.push_back({ g.m12, g.bpp, g.w, g.h, g.pitch, g.fnv });
    return v;
}
static void test_lin_golden() {
    const std::vector<GRow> rows = all_golden_rows();
    unsigned badEq = 0, badPitch = 0, badLand = 0, landed = 0;
    for (const auto &g : rows) {
        const uint32_t blk = blk_of_m12(g.m12), bl = log2u(g.bpp / 8u);
        uint32_t bwl = 0, bhl = 0;
        if (!n48_g12_blk(blk, bl, &bwl, &bhl)) { badEq++; continue; }
        if (((g.w + (1u << bwl) - 1u) >> bwl << bwl) != g.pitch) badPitch++;          // addrlib's pitch = w aligned to the block
        if (eq_fnv(blk, bl, g.w, g.h, g.pitch) != g.fnv) badEq++;
        // the streamed conversion lands each texel of a linear source (0x100 header, pitch w + 3 ELEMENTS) at addrlib's offset
        const n48_rp_lin_in in = lin_synth(g.m12, g.bpp / 8u, g.w, g.h, 3u, 0x100u);
        n48_rp_lin_plan p {};
        const uint32_t why = n48_rp_lin_check(&in, &p);
        std::vector<uint32_t> where;
        if (why != N48_RP_LIN_OK || p.g12Mode != g.m12 || !lin_landing(p, g.w, g.h, where, &conv_real) ||
            lin_where_fnv(where, g.w, g.h) != g.fnv) { badLand++; std::printf("FAIL L1 landing m12 %u bpp %u %ux%u (why %u)\n", g.m12, g.bpp, g.w, g.h, why); }
        else landed++;
    }
    CHECK(badEq == 0u, "L1: the generic gfx12 equation reproduces every golden row (%zu rows): %u wrong", rows.size(), badEq);
    CHECK(badPitch == 0u, "L1: addrlib's gfx12 pitch is the width aligned to the equation's block width: %u wrong", badPitch);
    CHECK(badLand == 0u, "L1: linear -> gfx12 (streamed) lands every texel at addrlib's offset: %u of %zu rows wrong", badLand, rows.size());
    // the generic equation IS the four hand-written gfx12 functions (randomized over the whole 16384-wide domain)
    uint64_t seed = 0x492492ull; unsigned diff = 0;
    auto rnd = [&seed]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(seed >> 33); };
    for (int i = 0; i < 200000; i++) {
        const uint32_t x = rnd() % 16384u, y = rnd() % 16384u, pb = 1u + rnd() % 512u;
        if (n48_g12_off2d(12, 2, x, y, pb) != n48_rp_g12_off(x, y, pb)) diff++;
        if (n48_g12_off2d(12, 0, x, y, pb) != n48_rp_g12_off_4kbdx8(x, y, pb)) diff++;
        if (n48_g12_off2d(12, 3, x, y, pb) != n48_rp_g12_off_4kbdx64(x, y, pb)) diff++;
        if (n48_g12_off2d(8, 0, x, y, pb) != n48_rp_g12_off_256d8(x, y, pb)) diff++;
    }
    CHECK(diff == 0u, "L1: n48_g12_off2d == the four golden-tested hand-written gfx12 functions (800000 points): %u differ", diff);
    // every supported (block, element size) is a bijection of the block's address bits onto its coordinate bits
    unsigned badBlk = 0;
    for (uint32_t blk = 8; blk <= 16; blk += 4) for (uint32_t bl = 0; bl <= 4; bl++) {
        uint32_t bwl = 0, bhl = 0;
        if (!n48_g12_blk(blk, bl, &bwl, &bhl)) { badBlk++; continue; }
        std::vector<uint8_t> seen(1u << blk, 0);
        for (uint32_t y = 0; y < (1u << bhl); y++) for (uint32_t x = 0; x < (1u << bwl); x++) {
            const uint32_t o = n48_g12_inblk(blk, bl, x, y);
            if ((o & ((1u << bl) - 1u)) || o >= (1u << blk) || seen[o]) badBlk++; else seen[o] = 1;
        }
    }
    CHECK(badBlk == 0u, "L1: every block/element pair is a bijection onto element-aligned offsets: %u faults", badBlk);
    std::printf("  L1 goldens: %zu rows (7 tables incl. the records' own dims), %u landed through the streamed conversion\n", rows.size(), landed);
}
// L2: the reference reader (gfx12 -> linear) over the golden-tested equation, at every distinct record shape.
static void g12_to_lin(const n48_rp_lin_plan &p, uint32_t w, uint32_t h, const uint8_t *g, uint8_t *lin) {
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        const uint64_t d = n48_g12_off2d(p.blkLog2, p.bpeLog2, x, y, p.pbk);
        for (uint32_t k = 0; k < p.bpe; k++) lin[p.srcOff + y * p.pitchBytes + (uint64_t)x * p.bpe + k] = g[d + k];
    }
}
static void test_lin_roundtrip() {
    unsigned shapes = 0, bad = 0;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) {
        const n48_fx_backing &r = kRun10fBacking[i];
        bool dup = false;
        for (uint32_t j = 0; j < i; j++) if (kRun10fBacking[j].w == r.w && kRun10fBacking[j].h == r.h && kRun10fBacking[j].fmtRes == r.fmtRes &&
                                             kRun10fBacking[j].swzVram == r.swzVram && kRun10fBacking[j].pitch == r.pitch) dup = true;
        if (dup) continue;
        shapes++;
        const n48_rp_lin_in in = fx_in(r);
        n48_rp_lin_plan p {};
        if (n48_rp_lin_check(&in, &p) != N48_RP_LIN_OK) { bad++; continue; }
        std::vector<uint8_t> src((size_t)p.srcEnd), g((size_t)p.g12Bytes), back((size_t)p.srcEnd, 0);
        for (size_t k = 0; k < src.size(); k++) src[k] = (uint8_t)((k * 2654435761u) >> 13);
        if (n48_rp_lin_to_g12(&p, r.w, r.h, src.data(), src.size(), g.data(), g.size()) != 1) { bad++; continue; }
        g12_to_lin(p, r.w, r.h, g.data(), back.data());
        unsigned wrong = 0;
        for (uint32_t y = 0; y < r.h; y++) for (uint64_t b = 0; b < (uint64_t)r.w * p.bpe; b++) {
            const uint64_t at = p.srcOff + y * p.pitchBytes + b;
            if (back[at] != src[at]) wrong++;
        }
        std::vector<uint8_t> src2 = src, g2((size_t)p.g12Bytes);
        for (uint64_t k = 0; k < p.srcOff; k++) src2[k] ^= 0xFFu;
        for (uint32_t y = 0; y + 1 < r.h; y++)
            for (uint64_t b = (uint64_t)r.w * p.bpe; b < p.pitchBytes; b++) src2[p.srcOff + y * p.pitchBytes + b] ^= 0xFFu;
        const bool padOk = n48_rp_lin_to_g12(&p, r.w, r.h, src2.data(), src2.size(), g2.data(), g2.size()) == 1 && g2 == g;
        if (wrong || !padOk) { bad++; std::printf("FAIL L2 #%u %ux%u: %u byte(s) wrong, header/padding leak %d\n", r.serial, r.w, r.h, wrong, !padOk); }
    }
    CHECK(bad == 0u && shapes >= 10u, "L2: every distinct record shape (%u) round-trips through the reference reader; header and row padding never reach the image (%u bad)", shapes, bad);
    std::printf("  L2 round trip: %u distinct record shapes, %u bad\n", shapes, bad);
}
// L3: every refusal of n48_rp_lin_check, one field at a time from a passing record (the mask, run10f #12).
static const n48_fx_backing &fx_serial(uint32_t s) { for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) if (kRun10fBacking[i].serial == s) return kRun10fBacking[i]; return kRun10fBacking[0]; }
static uint32_t lin_why(const n48_rp_lin_in &in) { n48_rp_lin_plan p {}; return n48_rp_lin_check(&in, &p); }
static void test_lin_refusals() {
    struct { const char *name; uint32_t want; void (*mut)(n48_rp_lin_in &); } cases[] = {
        { "nibble 7 (not Stretch/SurfaceCopy of this class)", N48_RP_LIN_NIBBLE, [](n48_rp_lin_in &i) { i.nibble = 7; } },
        { "nibble 3 (memcpy flavour)", N48_RP_LIN_NIBBLE, [](n48_rp_lin_in &i) { i.nibble = 3; } },
        { "record unreadable", N48_RP_LIN_REC, [](n48_rp_lin_in &i) { i.recOk = 0; } },
        { "VRAM side SW 0 (linear)", N48_RP_LIN_VRAM_SWZ, [](n48_rp_lin_in &i) { i.swzVram = 0x20; } },
        { "VRAM side SW 24 (64KB_Z_X: not a mode this path writes)", N48_RP_LIN_VRAM_SWZ, [](n48_rp_lin_in &i) { i.swzVram = 0x38; } },
        { "VRAM side SW 1 (256B_S)", N48_RP_LIN_VRAM_SWZ, [](n48_rp_lin_in &i) { i.swzVram = 0x21; } },
        { "VRAM side type 2 (3D)", N48_RP_LIN_VRAM_SWZ, [](n48_rp_lin_in &i) { i.swzVram = 0x56; } },
        { "backing swizzle 22 (NOT LINEAR)", N48_RP_LIN_NOT_LINEAR, [](n48_rp_lin_in &i) { i.swzBk = 0x400 | (22u << 4); } },
        { "backing swizzle 1", N48_RP_LIN_NOT_LINEAR, [](n48_rp_lin_in &i) { i.swzBk = 0x410; } },
        { "backing swizzle 33", N48_RP_LIN_NOT_LINEAR, [](n48_rp_lin_in &i) { i.swzBk = 0x400 | (33u << 4); } },
        { "backing type 2 (3D)", N48_RP_LIN_BK_TYPE, [](n48_rp_lin_in &i) { i.swzBk = 0x800; } },
        { "backing type 0 (1D)", N48_RP_LIN_BK_TYPE, [](n48_rp_lin_in &i) { i.swzBk = 0x000; } },
        { "format: strict equality (0x63, 0x63)", N48_RP_LIN_FMT, [](n48_rp_lin_in &i) { i.fmtRes = 0x63; } },
        { "format: equal texture indices (0x2, 0x2)", N48_RP_LIN_FMT, [](n48_rp_lin_in &i) { i.fmtBk = 0x02; } },
        { "format: bpe differs (0x2, 0x65)", N48_RP_LIN_FMT, [](n48_rp_lin_in &i) { i.fmtBk = 0x65; } },
        { "format: unknown resource index 0x1", N48_RP_LIN_FMT, [](n48_rp_lin_in &i) { i.fmtRes = 0x01; } },
        { "format: unknown backing index 0x62", N48_RP_LIN_FMT, [](n48_rp_lin_in &i) { i.fmtBk = 0x62; } },
        { "backing width differs", N48_RP_LIN_DIMS, [](n48_rp_lin_in &i) { i.bw = 192; } },
        { "backing height differs", N48_RP_LIN_DIMS, [](n48_rp_lin_in &i) { i.bh = 64; } },
        { "width 0", N48_RP_LIN_DIMS, [](n48_rp_lin_in &i) { i.w = 0; i.bw = 0; } },
        { "two slices (depth 2)", N48_RP_LIN_SLICES, [](n48_rp_lin_in &i) { i.depth = 2; } },
        { "first paged level 1 (a mip beyond 0)", N48_RP_LIN_MIP, [](n48_rp_lin_in &i) { i.firstMip = 1; } },
        { "res+0xe0 set", N48_RP_LIN_E0, [](n48_rp_lin_in &i) { i.hasE0 = 1; } },
        { "rowBytes names another element size (1280)", N48_RP_LIN_BPE, [](n48_rp_lin_in &i) { i.rowBytes = 1280; } },
        { "rowBytes the width unaligned (209)", N48_RP_LIN_BPE, [](n48_rp_lin_in &i) { i.rowBytes = 209; } },
        { "backing offset res+0xf8 != 0", N48_RP_LIN_BKOFF, [](n48_rp_lin_in &i) { i.backingOffset = 0x1000; } },
        { "pitch below width", N48_RP_LIN_PITCH, [](n48_rp_lin_in &i) { i.pitch = i.w - 1u; } },
        { "pitch 0", N48_RP_LIN_PITCH, [](n48_rp_lin_in &i) { i.pitch = 0; } },
        { "extent past srcLen", N48_RP_LIN_SRC, [](n48_rp_lin_in &i) { i.srcLen = 0x2000; } },
        { "extent past the backing descriptor", N48_RP_LIN_SRC, [](n48_rp_lin_in &i) { i.mdLen = 0x2000; } },
        { "extent past res+0x230", N48_RP_LIN_SRC, [](n48_rp_lin_in &i) { i.bytes = 0x2000; } },
        { "offset past the source", N48_RP_LIN_SRC, [](n48_rp_lin_in &i) { i.off = 0x3001; } },
        { "offset near 2^64 (wraps)", N48_RP_LIN_SRC, [](n48_rp_lin_in &i) { i.off = ~0ull - 8u; } },
        { "pitch x height past the source", N48_RP_LIN_SRC, [](n48_rp_lin_in &i) { i.pitch = 400; } },
        { "one block row past the chunk bound (64KB, 16384 wide)", N48_RP_LIN_BAND, [](n48_rp_lin_in &i) {
            i.swzVram = 0x3b; i.w = i.bw = 16384; i.h = i.bh = 2; i.pitch = 16384; i.rowBytes = 16384; i.off = 0;
            i.srcLen = i.mdLen = i.bytes = 1ull << 26; i.dstLen = 1ull << 30; } },
        { "gfx12 length past dstLen", N48_RP_LIN_DST, [](n48_rp_lin_in &i) { i.dstLen = 0x3fff; } },
        { "gfx12 length past the 64 MiB total (4KB_2D 16384x16384 8bpp: 1 MiB bands, 256 MiB)", N48_RP_LIN_DST, [](n48_rp_lin_in &i) {
            i.swzVram = 0x36; i.w = i.bw = 16384; i.h = i.bh = 16384; i.pitch = 16384; i.rowBytes = 16384; i.off = 0;
            i.srcLen = i.mdLen = i.bytes = 1ull << 29; i.dstLen = 1ull << 30; } },
    };
    unsigned bad = 0;
    for (auto &c : cases) {
        n48_rp_lin_in in = fx_in(fx_serial(12));
        c.mut(in);
        n48_rp_lin_plan p {};
        const uint32_t got = n48_rp_lin_check(&in, &p);
        if (got != c.want) { bad++; std::printf("FAIL L3 %s: got %u (%s) want %u\n", c.name, got, n48_rp_lin_name(got), c.want); }
        if (got != N48_RP_LIN_OK && (p.g12Bytes || p.g12Mode || p.chunkRows)) { bad++; std::printf("FAIL L3 %s: a refusal left a plan\n", c.name); }
    }
    gChecks++; if (bad) gFail++;
    std::printf("  L3 refusals: %zu driven, %u wrong\n", sizeof(cases) / sizeof(cases[0]), bad);
    // both linear swizzles pass (SW 0 = 0x400, SW 32 = 0x601), on the same record
    n48_rp_lin_in a = fx_in(fx_serial(12)); a.swzBk = 0x400; n48_rp_lin_in b = a; b.swzBk = 0x601;
    CHECK(lin_why(a) == N48_RP_LIN_OK && lin_why(b) == N48_RP_LIN_OK, "L3: backing SW 0 and SW 32 (LINEAR_GENERAL) both pass");
    // the band re-checks its own bounds: a band that did not come from the chunk for this plan writes nothing
    n48_rp_lin_plan p {}; const n48_rp_lin_in m = fx_in(fx_serial(12)); (void)n48_rp_lin_check(&m, &p);
    n48_rp_lin_lut l; (void)n48_rp_lin_lut_fill(&p, &l);
    const uint64_t need = (uint64_t)(m.h - 1u) * p.pitchBytes + m.w;
    std::vector<uint8_t> s1((size_t)need - 1u, 1), d1((size_t)p.bandBytes, 0xAB);
    CHECK(n48_rp_lin_band(&p, &l, m.w, m.h, 0, 1, s1.data(), s1.size(), d1.data(), d1.size()) == 0 && d1[0] == 0xAB,
          "L3: the band refuses a source one byte short, and writes nothing");
    std::vector<uint8_t> s2((size_t)need, 1), d2((size_t)p.bandBytes - 1u, 0xAB);
    CHECK(n48_rp_lin_band(&p, &l, m.w, m.h, 0, 1, s2.data(), s2.size(), d2.data(), d2.size()) == 0, "L3: ...a destination one byte short");
    std::vector<uint8_t> d3((size_t)p.bandBytes, 0xAB);
    CHECK(n48_rp_lin_band(&p, &l, m.w, m.h, 1, 1, s2.data(), s2.size(), d3.data(), d3.size()) == 0, "L3: ...and a block row past the image");
}
// L3b: THE FORMAT PAIRING. Accepted: exactly (0x2, 0x63), (0xf, 0x65), (0x2b, 0x68) - equal element size from dword +8 [11:9],
// the backing's row a raw element, the resource's not. And the rows ARE Apple's table's bytes when the binary is in the tree.
static void test_lin_format() {
    unsigned acc = 0, wrong = 0;
    for (uint32_t r = 0; r < 256; r++) for (uint32_t b = 0; b < 256; b++) {
        uint32_t bl = 99;
        const int ok = n48_rp_lin_fmt_pair(r, b, &bl);
        const bool want = (r == 0x02 && b == 0x63) || (r == 0x0f && b == 0x65) || (r == 0x2b && b == 0x68);
        if (ok != (want ? 1 : 0)) wrong++;
        if (ok) { acc++; if (bl != (r == 0x02 ? 0u : r == 0x0f ? 2u : 3u)) wrong++; }
    }
    CHECK(wrong == 0u && acc == 3u, "L3b: over all 65536 index pairs exactly the three pairs are accepted, with 1/4/8 B (%u accepted, %u wrong)", acc, wrong);
    const char *bin = "re/tahoe-26.6.2-x86_64/out/kexts/com.apple.kext.AMDRadeonX6000";
    FILE *f = std::fopen(bin, "rb");
    if (!f) { std::printf("  L3b SKIP: %s not in this tree (the rows are then checked only against their own comments)\n", bin); return; }
    unsigned rowsBad = 0;
    for (uint32_t i = 0; i < N48_RP_NFMT; i++) {
        uint32_t e[4] = { 0, 0, 0, 0 };
        if (std::fseek(f, 0x18ae00L + 16L * kN48RpFmt[i].idx, SEEK_SET) != 0 || std::fread(e, 4, 4, f) != 4) { rowsBad++; continue; }
        if (e[0] != (uint32_t)kN48RpFmt[i].idx << 8 || e[1] != kN48RpFmt[i].d1 || e[2] != kN48RpFmt[i].d2) {
            rowsBad++; std::printf("FAIL L3b row %#x: binary %08x %08x %08x\n", kN48RpFmt[i].idx, e[0], e[1], e[2]);
        }
    }
    std::fclose(f);
    CHECK(rowsBad == 0u, "L3b: all %u kN48RpFmt rows equal _ati_format_info_table's bytes in the X6000 binary (%u differ)", N48_RP_NFMT, rowsBad);
    std::printf("  L3b format pairing: 3 of 65536 pairs accepted; %u table rows byte-checked against the binary\n", N48_RP_NFMT);
}
// L3c: the VRAM-mode map IS xlat12_desc.h's (the T# the translator places names the layout written), one-to-one, block sizes right.
static void test_lin_modes() {
    unsigned bad = 0;
    for (uint32_t i = 0; i < N48_RP_LIN_NMODES; i++) {
        uint32_t x12 = 0xFFu;
        for (uint32_t k = 0; k < XLAT12_SWMODE_MAP_COUNT; k++) if (kXlat12SwModeG10ToG12[k][0] == kN48RpLinModes[i].g10) x12 = kXlat12SwModeG10ToG12[k][1];
        if (x12 != kN48RpLinModes[i].g12 || n48_rp_lin_g12_mode(kN48RpLinModes[i].g10) != x12) bad++;
        if (n48_rp_lin_g10_for_g12(kN48RpLinModes[i].g12) != kN48RpLinModes[i].g10) bad++;   // one-to-one
        if (kN48RpLinModes[i].blkLog2 != blk_of_m12(kN48RpLinModes[i].g12)) bad++;
    }
    for (uint32_t g10 = 0; g10 < 32; g10++) if (n48_rp_lin_g12_mode(g10) && g10 != 2u && g10 != 22u && g10 != 27u) bad++;
    CHECK(bad == 0u, "L3c: 2 -> 1, 22 -> 2, 27 -> 3 exactly as kXlat12SwModeG10ToG12, one-to-one, blocks 256 B / 4 KiB / 64 KiB (%u bad)", bad);
}
// L3d: the VRAM side's pitch rule against addrlib (kG10Pitch), and every record's rowBytes.
static void test_lin_g10pitch() {
    unsigned bad = 0;
    for (const auto &g : kG10Pitch) {
        const uint32_t m12 = n48_rp_lin_g12_mode(g.m10);
        if (!m12 || n48_rp_lin_g10_pitch(blk_of_m12(m12), log2u(g.bpp / 8u), g.w) != g.pitch) bad++;
    }
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) {
        const n48_fx_backing &r = kRun10fBacking[i];
        uint32_t bl = 0;
        const uint32_t m12 = n48_rp_lin_g12_mode(r.swzVram & 0x1fu);
        if (!m12 || !n48_rp_lin_fmt_pair(r.fmtRes, r.fmtBk, &bl) ||
            r.rowBytes != (1u << bl) * n48_rp_lin_g10_pitch(blk_of_m12(m12), bl, r.w) || r.vpitch != n48_rp_lin_g10_pitch(blk_of_m12(m12), bl, r.w)) bad++;
    }
    CHECK(bad == 0u, "L3d: the VRAM pitch equals addrlib's (%zu rows) and every record's rowBytes and +0x34 (%u bad)", sizeof(kG10Pitch) / sizeof(kG10Pitch[0]), bad);
}
// L3e: THE STREAMED COPY'S BOUNDS, over every record: the chunks tile [0, g12Bytes) exactly, in order; every chunk's gfx12 bytes
// and source span are at most N48_RP_MAX_BYTES; every source span lies inside the proven extent [srcOff, srcEnd); nothing past the
// image is a chunk. The wallpaper takes 9 chunks of one 960 KiB block row each.
static unsigned chunk_audit(const n48_rp_lin_plan &p, uint32_t w, uint32_t h, unsigned *nChunks) {
    unsigned bad = 0, n = 0;
    uint64_t at = 0;
    while (at < p.g12Bytes && n < 100000u) {
        uint32_t j0 = 0, nr = 0; uint64_t sf = 0, sl = 0, df = 0, dl = 0;
        if (!n48_rp_lin_chunk(&p, w, h, at, &j0, &nr, &sf, &sl, &df, &dl)) { bad++; break; }
        // build 0.0.544 4a: a ONE-band chunk may reach N48_RP_LIN_BAND_MAX; a chunk of several bands stays within 1 MiB
        const uint64_t lim = p.chunkRows == 1u ? (uint64_t)N48_RP_LIN_BAND_MAX : kMiB;
        if (df != at || !dl || dl > lim || sl > lim || sf < p.srcOff || sf + sl > p.srcEnd || nr > p.chunkRows) bad++;
        // any byte inside the chunk names the same chunk
        uint32_t j1 = 0, n1 = 0; uint64_t a = 0, b = 0, c = 0, d = 0;
        if (!n48_rp_lin_chunk(&p, w, h, at + dl - 1u, &j1, &n1, &a, &b, &c, &d) || j1 != j0 || c != df) bad++;
        at += dl; n++;
    }
    if (at != p.g12Bytes) bad++;
    uint32_t j = 0, nr = 0; uint64_t a = 0, b = 0, c = 0, d = 0;
    if (n48_rp_lin_chunk(&p, w, h, p.g12Bytes, &j, &nr, &a, &b, &c, &d)) bad++;
    *nChunks = n;
    return bad;
}
static void test_lin_stream() {
    unsigned bad = 0, total = 0, maxc = 0;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) {
        const n48_fx_backing &r = kRun10fBacking[i];
        const n48_rp_lin_in in = fx_in(r);
        n48_rp_lin_plan p {};
        if (n48_rp_lin_check(&in, &p) != N48_RP_LIN_OK) { bad++; continue; }
        unsigned n = 0; bad += chunk_audit(p, r.w, r.h, &n); total += n; if (n > maxc) maxc = n;
    }
    CHECK(bad == 0u, "L3e: every record's chunks tile its gfx12 image, each <= 1 MiB of gfx12 bytes and of source, inside the extent (%u bad)", bad);
    n48_rp_lin_plan w {}; const n48_rp_lin_in wi = fx_in(fx_serial(9)); (void)n48_rp_lin_check(&wi, &w);
    unsigned wn = 0; (void)chunk_audit(w, 1920, 1080, &wn);
    CHECK(w.g12Mode == 3u && w.g12Bytes == 0x870000ull && w.bandBytes == 0xF0000ull && w.chunkRows == 1u && wn == 9u,
          "L3e: the wallpaper streams as 9 chunks of one 64KB_2D block row (0xF0000 B) each, 0x870000 in all (%u chunks)", wn);
    uint32_t j = 0, nr = 0; uint64_t sf = 0, sl = 0, df = 0, dl = 0;
    CHECK(n48_rp_lin_chunk(&w, 1920, 1080, 0x870000ull - 1u, &j, &nr, &sf, &sl, &df, &dl) == 1 && j == 8u && df == 0x780000ull &&
          sf == 1024ull * 7680u && sl == 55ull * 7680u + 7680u, "L3e: the last chunk reads only the image's last 56 rows (1024..1079)");
    // synthetic: a band exactly AT the bound passes, one element wider refuses.
    // build 0.0.544 4a DELIBERATE re-baseline: the bound on ONE block row is N48_RP_LIN_BAND_MAX (2 MiB), so the
    // edge moved from 2048/2049 wide (1 MiB) to 4096/4097 wide (2 MiB); 2049 wide (a 17-block, 1.0625 MiB row) now streams.
    n48_rp_lin_in at = lin_synth(3, 4, 4096, 128, 0, 0); at.srcLen = at.mdLen = at.bytes = 1ull << 26;
    n48_rp_lin_in over = lin_synth(3, 4, 4097, 128, 0, 0); over.srcLen = over.mdLen = over.bytes = 1ull << 26;
    n48_rp_lin_in was = lin_synth(3, 4, 2049, 128, 0, 0); was.srcLen = was.mdLen = was.bytes = 1ull << 26;
    CHECK(lin_why(at) == N48_RP_LIN_OK && lin_why(over) == N48_RP_LIN_BAND && lin_why(was) == N48_RP_LIN_OK,
          "L3e: a 4096-wide 64KB_2D 32bpp image (band = 2 MiB exactly) streams; 4097 wide refuses BAND; 2049 wide streams (0.0.544)");
    // the GFX12 bound binding (not the source's): a 256B_2D 8-byte image 9 wide (a 512-byte band, a 288-byte source band) and
    // 16384 tall streams in chunks of exactly 2048 bands = 1 MiB; one band more per chunk would be past the bound
    n48_rp_lin_in nb = lin_synth(1, 8, 9, 16384, 0, 0); nb.srcLen = nb.mdLen = nb.bytes = 1ull << 26;
    n48_rp_lin_plan np {}; unsigned nn = 0;
    const uint32_t nwhy = n48_rp_lin_check(&nb, &np);
    const unsigned nbad = nwhy == N48_RP_LIN_OK ? chunk_audit(np, 9, 16384, &nn) : 1u;
    CHECK(nwhy == N48_RP_LIN_OK && np.chunkRows == 2048u && nbad == 0u && nn == 2u,
          "L3e: when the gfx12 band binds, a chunk is exactly 1 MiB (2048 bands of 512 B, 2 chunks): rows %u, %u chunks, %u bad",
          np.chunkRows, nn, nbad);
    std::printf("  L3e streamed copy: %u chunks over 41 records (max %u per copy); wallpaper %u chunks of %#llx\n", total, maxc, wn,
                (unsigned long long)w.bandBytes);
}
// L4: the ask.
static const uint32_t kAvatarT10[8] = { 0x04006200u, 0xc3800000u, 0x8023c023u, 0x99670f2eu, 0u, 0x00400070u, 0u, 0u };   // decide42 slot 36 (VA 0x400620000)
// 209x41 8_UNORM at VA 0x40023c000, dst_sel (0,0,0,X), SW 22, TYPE 9, one level - the mask's shape as decoded decide42 slot 42
static const uint32_t kMaskT10[8]   = { 0x040023c0u, 0x00100000u, 0x800a0034u, 0x91600800u, 0u, 0x00000000u, 0u, 0u };
struct LinVm { uint64_t va0, vram0, bytes; };
static int lin_walk(const void *vm, uint64_t va, uint64_t *vram) {
    const LinVm *v = static_cast<const LinVm *>(vm);
    if (va < v->va0 || va >= v->va0 + v->bytes) return 0;
    *vram = v->vram0 + ((va - v->va0) & ~0xfffull);
    return 1;
}
static n48_rp_copy lin_copy(uint64_t va, uint64_t vram, uint64_t bytes, uint32_t w, uint32_t h, uint32_t eb, uint32_t mode = N48_RP_G12_4KB_2D) {
    n48_rp_copy c; std::memset(&c, 0, sizeof c);
    c.copied = 1; c.retiled = 1; c.bytes = bytes; c.compared = bytes / 4; c.ownerWs = 1; c.ctx = 5; c.vaOk = 1; c.va = va;
    c.contiguous = 1; c.vram = vram; c.mode = mode; c.arm = 1; c.epoch = 3; c.elemBytes = eb; c.lin = 1; c.w = w; c.h = h;
    return c;
}
static void t10_set_wh(uint32_t *t, uint32_t w, uint32_t h) {
    t[1] = (t[1] & ~0xC0000000u) | (((w - 1u) & 3u) << 30);
    t[2] = (t[2] & ~0x0FFFFFFFu) | (((w - 1u) >> 2) & 0xfffu) | (((h - 1u) & 0x3fffu) << 14);
}
// A T# of a record's own shape: its VA, the gfx10 FORMAT its resource format names (0xf -> 56 8_8_8_8, 0x2 -> 1 8_UNORM, 0x2b -> 71
// 16_16_16_16_FLOAT), the VRAM side's SW_MODE, TYPE 9, the given MAX_MIP/LAST_LEVEL. SUSPECTED as the T#s Apple writes for them
// (the avatar's and the mask's match decide42's; the wallpaper's and the LUTs' are not captured).
static void t10_for(uint32_t *t, uint64_t va, uint32_t fmtRes, uint32_t g10, uint32_t w, uint32_t h, uint32_t levels) {
    const uint32_t f10 = fmtRes == 0x0fu ? 56u : fmtRes == 0x02u ? 1u : 71u;
    std::memset(t, 0, 8 * sizeof(uint32_t));
    t[0] = (uint32_t)(va >> 8); t[1] = (uint32_t)((va >> 40) & 0xffu) | (f10 << 20);
    t10_set_wh(t, w, h);
    t[3] = (levels << 16) | (g10 << 20) | (9u << 28) | 0xfacu;   // dst_sel bits, LAST_LEVEL, SW_MODE, TYPE 9
    t[5] = levels << 4;                                          // MAX_MIP
}
static void test_lin_ask() {
    n48_rp_ent probe {}; probe.lin = 1; probe.w = 144; probe.h = 144; probe.mode = 2;
    CHECK(n48_rp_lin_t_match(&probe, kAvatarT10) == 1, "L4: decide42's avatar T# (144x144, TYPE 9, SW 22, MAX_MIP 7 LAST 7) matches a 144x144 mode-2 entry");
    probe.w = 209; probe.h = 41;
    CHECK(n48_rp_lin_t_match(&probe, kMaskT10) == 1, "L4: the mask T# (209x41, 1 level) matches a 209x41 entry");
    const LinVm vm = { 0x400720000ull, 0x13a4e000ull, 0x19000ull };
    n48_rp t {};
    n48_rp_copy c = lin_copy(0x400720000ull, 0x13a4e000ull, 0x19000ull, 144, 144, 4);
    CHECK(n48_rp_record(&t, &c) == N48_RP_REC_OK && t.n == 1 && t.e[0].lin == 1 && t.e[0].w == 144 && t.e[0].h == 144,
          "L4: a lin copy records with its width/height");
    uint32_t cl = 9;
    CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, kAvatarT10, &cl) == 1 && cl == 1u,
          "L4: n48_rp_ok_t proves the avatar T# and hands the mip clamp");
    CHECK(t.linProven == 1u, "L4: counted linProven");
    CHECK(n48_rp_ok(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm) == 0 && t.linNoT == 1u, "L4: n48_rp_ok (no T#) REFUSES a lin entry (linNoT)");
    CHECK(n48_rp_ok_carry(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm, 1, 1, 2) == 0 && t.linNoT == 2u, "L4: n48_rp_ok_carry (no T#) REFUSES a lin entry too");
    struct { const char *name; void (*mut)(uint32_t *); } bads[] = {
        { "a 192x64 T# at the same VA", [](uint32_t *x) { t10_set_wh(x, 192, 64); } },
        { "width 145", [](uint32_t *x) { t10_set_wh(x, 145, 144); } },
        { "height 143", [](uint32_t *x) { t10_set_wh(x, 144, 143); } },
        { "TYPE 8 (1D)", [](uint32_t *x) { x[3] = (x[3] & 0x0FFFFFFFu) | (8u << 28); } },
        { "BASE_LEVEL 1", [](uint32_t *x) { x[3] |= 1u << 12; } },
        { "BASE_ARRAY 1", [](uint32_t *x) { x[4] |= 1u << 16; } },
        { "DEPTH 1", [](uint32_t *x) { x[4] |= 1u; } },
        { "SW_MODE 21", [](uint32_t *x) { x[3] = (x[3] & ~(0x1Fu << 20)) | (21u << 20); } },
        { "SW_MODE 27 against a mode-2 entry", [](uint32_t *x) { x[3] = (x[3] & ~(0x1Fu << 20)) | (27u << 20); } },
        { "LAST_LEVEL 7 > MAX_MIP 0", [](uint32_t *x) { x[5] &= ~0xF0u; } },
    };
    for (auto &b : bads) {
        uint32_t x[8]; std::memcpy(x, kAvatarT10, sizeof x); b.mut(x);
        const uint64_t before = t.linTRefused;
        uint32_t c2 = 7;
        CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, x, &c2) == 0 && t.linTRefused == before + 1 && c2 == 0u,
              "L4: %s refuses (linTRefused), no clamp", b.name);
    }
    CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, nullptr, &cl) == 0, "L4: no T# at all refuses a lin entry");
    CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 1, 1, 3, &lin_walk, &vm, 0, 1, 2, kAvatarT10, &cl) == 0, "L4: the element size still refuses");
    CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 4, 1, 4, &lin_walk, &vm, 0, 1, 2, kAvatarT10, &cl) == 0, "L4: a moved epoch still refuses");
    const LinVm vm2 = { 0x400720000ull, 0x13a4e000ull, 0x15000ull };
    CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm2, 0, 1, 2, kAvatarT10, &cl) == 0,
          "L4: the walk re-proves EVERY page of the gfx12 length (0x19000), not res+0x230's");
    // 0.0.492: the other two modes - a mode-3 (64KB_2D) entry answers only a SW 27 T#, a mode-1 (256B_2D) entry only a SW 2 T#
    n48_rp t3 {};
    n48_rp_copy c3 = lin_copy(0x405800000ull, 0x13028000ull, 0x870000ull, 1920, 1080, 4, 3u);
    const LinVm vm3 = { 0x405800000ull, 0x13028000ull, 0x870000ull };
    CHECK(n48_rp_record(&t3, &c3) == N48_RP_REC_OK, "L4: a mode-3 lin copy (the wallpaper, 0x870000 B) records");
    uint32_t w27[8], w22[8]; t10_for(w27, 0x405800000ull, 0x0f, 27, 1920, 1080, 0); t10_for(w22, 0x405800000ull, 0x0f, 22, 1920, 1080, 0);
    CHECK(n48_rp_ok_t(&t3, 5, 0x405800000ull, 3, 4, 1, 3, &lin_walk, &vm3, 0, 1, 2, w27, &cl) == 1 && cl == 1u, "L4: ...proven for a 1920x1080 SW 27 T#");
    CHECK(n48_rp_ok_t(&t3, 5, 0x405800000ull, 3, 4, 1, 3, &lin_walk, &vm3, 0, 1, 2, w22, &cl) == 0, "L4: ...refused for a SW 22 T# of the same dims");
    n48_rp t1 {};
    n48_rp_copy c1 = lin_copy(0x4000be000ull, 0x13e69000ull, 0x2000ull, 256, 1, 8, 1u);
    const LinVm vm1 = { 0x4000be000ull, 0x13e69000ull, 0x2000ull };
    uint32_t l2[8]; t10_for(l2, 0x4000be000ull, 0x2b, 2, 256, 1, 0);
    CHECK(n48_rp_record(&t1, &c1) == N48_RP_REC_OK && n48_rp_ok_t(&t1, 5, 0x4000be000ull, 1, 8, 1, 3, &lin_walk, &vm1, 0, 1, 2, l2, &cl) == 1,
          "L4: a mode-1 LUT entry (256x1, 8 B) is proven for a SW 2 TYPE 9 T#");
    uint32_t l1d[8]; std::memcpy(l1d, l2, sizeof l1d); l1d[3] = (l1d[3] & 0x0FFFFFFFu) | (8u << 28);
    CHECK(n48_rp_ok_t(&t1, 5, 0x4000be000ull, 1, 8, 1, 3, &lin_walk, &vm1, 0, 1, 2, l1d, &cl) == 0, "L4: ...and refused for a TYPE 8 (1D) T# (SUSPECTED shape of the real LUT T#s)");
    // the record's lin clause: any other gfx12 mode, a copy not re-tiled, no dims
    n48_rp tr {};
    n48_rp_copy bad4 = lin_copy(0x400720000ull, 0x13a4e000ull, 0x19000ull, 144, 144, 4, 4u);
    n48_rp_copy badR = lin_copy(0x400720000ull, 0x13a4e000ull, 0x19000ull, 144, 144, 4, 3u); badR.retiled = 0;
    n48_rp_copy badW = lin_copy(0x400720000ull, 0x13a4e000ull, 0x19000ull, 0, 144, 4, 1u);
    CHECK(n48_rp_record(&tr, &bad4) == N48_RP_REC_MODE && n48_rp_record(&tr, &badR) == N48_RP_REC_MODE && n48_rp_record(&tr, &badW) == N48_RP_REC_MODE,
          "L4: a lin copy in mode 4, one not re-tiled, or one without dims refuses MODE");
}
// The 0.0.483 n48_rp_ok / n48_rp_ok_carry, verbatim (git show fe2f769:src/navi48-bringup/src/apple/ws_resprov.h), for OFF identity.
static int old_rp_ok(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t askElemBytes, uint32_t arm,
                     uint32_t epoch, n48_rp_walk walk, const void *vm)
{
    t->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode) continue;
        if (e->elemBytes != askElemBytes) { t->elemMismatch++; return 0; }
        if (e->arm != arm || e->epoch != epoch) { t->stale++; return 0; }
        if (!walk || !vm) { t->walkRefused++; return 0; }
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) {
            uint64_t v = ~0ull;
            if (walk(vm, va + p, &v) != 1 || v != e->vram + p) { t->walkRefused++; return 0; }
        }
        t->proven++;
        return 1;
    }
    return 0;
}
static int old_rp_ok_carry(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t askElemBytes,
                           uint32_t arm, uint32_t epoch, n48_rp_walk walk, const void *vm, uint32_t carry,
                           uint32_t decideArm, uint32_t commitArm)
{
    t->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode) continue;
        if (e->elemBytes != askElemBytes) { t->elemMismatch++; return 0; }
        const int armOk = (e->arm == arm) || (carry != 0u && arm == commitArm && e->arm == decideArm);
        if (!armOk || e->epoch != epoch) { t->stale++; return 0; }
        if (!walk || !vm) { t->walkRefused++; return 0; }
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) {
            uint64_t v = ~0ull;
            if (walk(vm, va + p, &v) != 1 || v != e->vram + p) { t->walkRefused++; return 0; }
        }
        t->proven++;
        return 1;
    }
    return 0;
}
// The 0.0.491 n48_rp_record, verbatim but its comments (git show 61d1b42:src/navi48-bringup/src/apple/ws_resprov.h), for OFF identity.
static uint32_t old_rp_record(n48_rp *t, const n48_rp_copy *c)
{
    uint32_t why = N48_RP_REC_OK;
    if (!c || c->copied != 1u) why = N48_RP_REC_NOT_COPIED;
    else if (!c->bytes || c->mismatched || c->compared * 4ull < c->bytes) why = N48_RP_REC_UNVERIFIED;
    else if (c->ownerWs != 1u) why = N48_RP_REC_NOT_WS;
    else if (!c->ctx) why = N48_RP_REC_NO_CTX;
    else if (c->vaOk != 1u || !c->va || (c->va & 0xfffull) || c->va >= (1ull << 48) || c->va + c->bytes > (1ull << 48))
        why = N48_RP_REC_NO_VA;
    else if (c->contiguous != 1u || (c->vram & 0xfffull)) why = N48_RP_REC_SPLIT;
    else if (!c->mode || c->mode > 7u) why = N48_RP_REC_MODE;
    else if (c->mode == N48_RP_G12_4KB_2D && c->retiled != 1u) why = N48_RP_REC_NOT_RETILED;
    else if (c->lin && (c->mode != N48_RP_G12_4KB_2D || !c->w || !c->h)) why = N48_RP_REC_MODE;
    if (c && c->copied == 1u && t) {
        for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
            const n48_rp_ent *e = &t->e[k];
            const int sameVa = c->ctx != 0ull && e->ctx == c->ctx && e->va == c->va;
            const int overVram = c->bytes != 0ull && e->vram < c->vram + c->bytes && c->vram < e->vram + e->bytes;
            if (sameVa || overVram) n48_rp_drop_at(t, k); else k++;
        }
    }
    if (why) { if (t) t->refused[why]++; return why; }
    if (t->n >= N48_RP_MAX) { t->refused[N48_RP_REC_FULL]++; return N48_RP_REC_FULL; }
    n48_rp_ent *e = &t->e[t->n++];
    e->ctx = c->ctx; e->va = c->va; e->vram = c->vram; e->bytes = c->bytes; e->mode = c->mode;
    e->arm = c->arm; e->epoch = c->epoch; e->elemBytes = c->elemBytes;
    e->lin = c->lin ? 1u : 0u; e->w = c->w; e->h = c->h; e->known = 0u;   // 0.0.493: the field was `pad2`
    t->recorded++;
    return N48_RP_REC_OK;
}
static uint64_t rp_counters_hash(const n48_rp &t) {
    uint64_t f = 0xcbf29ce484222325ull;
    const uint64_t v[] = { t.asked, t.proven, t.walkRefused, t.stale, t.elemMismatch, t.recorded, t.dropped };
    for (uint64_t x : v) f = fnv(f, x);
    return f;
}
static uint64_t rp_table_hash(const n48_rp &t) {
    uint64_t f = rp_counters_hash(t);
    f = fnv(f, t.n);
    for (uint32_t k = 0; k < t.n && k < N48_RP_MAX; k++) {
        const n48_rp_ent &e = t.e[k];
        const uint64_t v[] = { e.ctx, e.va, e.vram, e.bytes, e.mode, e.arm, e.epoch, e.elemBytes, e.lin, e.w, e.h };
        for (uint64_t x : v) f = fnv(f, x);
    }
    for (uint32_t r = 0; r < N48_RP_REC_REASONS; r++) f = fnv(f, t.refused[r]);
    return f;
}
static void test_lin_off_identity() {
    // Randomized: tables of NON-lin entries (every entry any path but switch 59 records), random asks. The new n48_rp_ok,
    // n48_rp_ok_carry and n48_rp_ok_t (with any T#) answer and count EXACTLY as the 0.0.483 code does, and never clamp.
    uint64_t seed = 0x486486486ull; unsigned diffs = 0, clamps = 0, asks = 0, recDiffs = 0, recs = 0;
    auto rnd = [&seed]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(seed >> 33); };
    const FakeVm vm = { { 0x400006000ull, 0x400007000ull, 0x400720000ull, 0x400721000ull }, { 0x10079000ull, 0x1007a000ull, 0x13a4e000ull, 0x13a4f000ull }, 4 };
    for (int round = 0; round < 20000; round++) {
        n48_rp a {}, b {}, c {}, d {}, e {}, oa {};
        const uint32_t n = rnd() % 6;
        for (uint32_t k = 0; k < n; k++) {
            n48_rp_copy cp = good_copy();
            cp.va = (rnd() & 1) ? 0x400006000ull : 0x400720000ull; cp.vram = cp.va == 0x400006000ull ? 0x10079000ull : 0x13a4e000ull;
            cp.ctx = 5 + (rnd() & 1); cp.mode = rnd() % 9; cp.arm = 1 + (rnd() & 1); cp.epoch = rnd() & 1; cp.elemBytes = 1u << (rnd() % 4);
            cp.bytes = (rnd() & 1) ? 0x1000 : 0x2000; cp.compared = cp.bytes / 4; cp.retiled = rnd() & 1;
            if ((rnd() % 7) == 0) cp.mismatched = 1;
            // 0.0.492 OFF identity of the RECORD: for every copy switch 59 does not take (lin 0), n48_rp_record == 0.0.491's
            const uint32_t w1 = n48_rp_record(&a, &cp), w2 = old_rp_record(&oa, &cp);
            recs++;
            if (w1 != w2 || rp_table_hash(a) != rp_table_hash(oa)) recDiffs++;
        }
        b = a; c = a; d = a; e = a;
        const uint64_t ctx = 5 + (rnd() % 3), va = (rnd() & 1) ? 0x400006000ull : 0x400720000ull;
        const uint32_t mode = 1 + (rnd() % 3), eb = 1u << (rnd() % 4), arm = 1 + (rnd() & 1), ep = rnd() & 1, carry = rnd() & 1;
        uint32_t cl = 0;
        const int r1 = n48_rp_ok(&a, ctx, va, mode, eb, arm, ep, &fake_walk, &vm), r2 = old_rp_ok(&b, ctx, va, mode, eb, arm, ep, &fake_walk, &vm);
        const int r3 = n48_rp_ok_carry(&c, ctx, va, mode, eb, arm, ep, &fake_walk, &vm, carry, 1, 2);
        const int r4 = old_rp_ok_carry(&d, ctx, va, mode, eb, arm, ep, &fake_walk, &vm, carry, 1, 2);
        const int r5 = n48_rp_ok_t(&e, ctx, va, mode, eb, arm, ep, &fake_walk, &vm, carry, 1, 2, kAvatarT10, &cl);
        asks++;
        if (r1 != r2 || rp_counters_hash(a) != rp_counters_hash(b)) diffs++;
        if (r3 != r4 || rp_counters_hash(c) != rp_counters_hash(d)) diffs++;
        if (r5 != r4 || rp_counters_hash(e) != rp_counters_hash(d)) diffs++;
        if (cl) clamps++;
    }
    CHECK(diffs == 0u && clamps == 0u, "L4 OFF identity: %u randomized asks, %u answer/counter differences from 0.0.483, %u clamps", asks, diffs, clamps);
    CHECK(recDiffs == 0u, "L4 OFF identity: %u randomized non-lin records, %u verdict/table differences from 0.0.491's n48_rp_record", recs, recDiffs);
    // the switch-OFF pure gates: nothing is taken, no gfx10 re-tile is refused, no new page-out is refused
    unsigned gate = 0;
    for (uint32_t r = 0; r < N48_RP_LIN_REASONS; r++) gate += n48_rp_lin_take(r, 0u) ? 1u : 0u;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) gate += n48_rp_lin_blocks_old(0u, kRun10fBacking[i].nibble, 1u, kRun10fBacking[i].swzBk) ? 1u : 0u;
    for (uint32_t s = 0; s < 0x80u; s++) gate += n48_rp_pageout_refuse_64krx(0u, s) ? 1u : 0u;
    CHECK(gate == 0u, "L4 OFF identity: with switch 59 OFF nothing is taken, no gfx10 re-tile refused, no page-out refused (%u)", gate);
    std::printf("  L4 OFF identity: %u asks vs 0.0.483 (%u differences, %u clamps); %u records vs 0.0.491 (%u differences)\n", asks, diffs, clamps, recs, recDiffs);
}
// L5: the committed colour-target drop (unchanged by 0.0.492: it keys on the entry's own [va, va + bytes) = the gfx12 length).
static void test_lin_drop() {
    n48_rp t {};
    n48_rp_copy lin = lin_copy(0x400720000ull, 0x13a4e000ull, 0x19000ull, 144, 144, 4);
    n48_rp_copy other = good_copy(); other.ctx = 5;
    (void)n48_rp_record(&t, &other); (void)n48_rp_record(&t, &lin);
    CHECK(t.n == 2, "L5: two entries");
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400800000ull, 0x10000) == 0 && t.n == 2, "L5: a disjoint target keeps both");
    CHECK(n48_rp_lin_drop_target(&t, 6, 0x400720000ull, 0x1000) == 0 && t.n == 2, "L5: another context's target keeps both");
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400006000ull, 0x2000) == 0 && t.n == 2, "L5: a target over the NON-lin entry drops nothing");
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400600000ull, 0x120001ull) == 1 && t.n == 1 && t.e[0].lin == 0 && t.linLedDropped == 1,
          "L5: a target whose extent reaches the lin entry's first byte drops it (and only it)");
    (void)n48_rp_record(&t, &lin);
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400738000ull, 0x1000) == 1 && t.n == 1, "L5: a target inside the entry's last page drops it");
    (void)n48_rp_record(&t, &lin);
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400739000ull, 0x1000) == 0 && t.n == 2, "L5: a target starting at the entry's END keeps it");
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400721000ull, 0) == 1 && t.n == 1, "L5: an extent-less target based inside drops it");
    (void)n48_rp_record(&t, &lin);
    const uint64_t ne = t.linLedNoExtent;
    CHECK(n48_rp_lin_drop_target(&t, 5, 0x400700000ull, 0) == 0 && t.n == 2 && t.linLedNoExtent == ne + 1,
          "L5: an extent-less target based BELOW is counted (linLedNoExtent), not dropped");
    const LinVm vm = { 0x400720000ull, 0x13a4e000ull, 0x19000ull };
    (void)n48_rp_lin_drop_target(&t, 5, 0x400720000ull, 0x19000);
    uint32_t cl = 0;
    CHECK(n48_rp_ok_t(&t, 5, 0x400720000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, kAvatarT10, &cl) == 0, "L5: a dropped entry answers no");
    // 0.0.492: a wallpaper-sized entry is dropped by a target in its LAST chunk (the gfx12 length, not res+0x230's)
    n48_rp tw {};
    n48_rp_copy wp = lin_copy(0x405800000ull, 0x13028000ull, 0x870000ull, 1920, 1080, 4, 3u);
    (void)n48_rp_record(&tw, &wp);
    CHECK(n48_rp_lin_drop_target(&tw, 5, 0x405800000ull + 0x7f0000ull, 0x1000) == 1, "L5: a target past res+0x230 but inside the gfx12 length drops the wallpaper's entry");
}
// L6: every line under the 491-byte body cap, prefix included, at its widest.
static void test_lin_lines() {
    char buf[2048];
    const uint64_t M = ~0ull;
    const uint32_t U = 0xffffffffu;
    uint32_t widest = 0;
    for (uint32_t r = 0; r < N48_RP_LIN_REASONS; r++) if (std::strlen(n48_rp_lin_name(r)) > std::strlen(n48_rp_lin_name(widest))) widest = r;
    int n = std::snprintf(buf, sizeof buf, "%s" N48_RP_BK1_FMT, kPeerPfx,
                          N48_RP_BK1_ARGS(M, (void *)M, U, 0xffu, 0xffffu, 0xffffu, U, U, 0xffu, 0xffu, 0xffu, M, M, 1, M, M, M, M, M, widest, 0));
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "L6: step-0 line 1 worst case %d bytes < %u", n, N48_RP_LOG_BODY_CAP);
    std::printf("  L6 widths: line1 %d", n);
    n = std::snprintf(buf, sizeof buf, "%s" N48_RP_BK2_FMT, kPeerPfx,
                      N48_RP_BK2_ARGS(M, (void *)M, 0, 0xffffu, 0xffffu, 0xffffu, 0xffu, 0xffu, M, U, 0xffffu, 0xffffu, 0xffffu, 0xffffu, U, U));
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "L6: step-0 line 2 worst case %d bytes < %u", n, N48_RP_LOG_BODY_CAP);
    std::printf(" line2 %d", n);
    const uint32_t d[16] = { U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U };
    n = std::snprintf(buf, sizeof buf, "%s" N48_RP_BK3_FMT, kPeerPfx, N48_RP_BK3_ARGS(M, M, 0, d));
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "L6: step-0 line 3 worst case %d bytes < %u", n, N48_RP_LOG_BODY_CAP);
    std::printf(" line3 %d", n);
    n = std::snprintf(buf, sizeof buf, "%s" N48_RP_BK4_FMT, kPeerPfx, N48_RP_BK4_ARGS(M, U, M, 0, d, M, M, M, M, 0));
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "L6: step-0 line 4 (middle row, non-zero count) worst case %d bytes < %u", n, N48_RP_LOG_BODY_CAP);
    std::printf(" line4 %d", n);
    n = std::snprintf(buf, sizeof buf, "AppleHardwareHook: " N48_RP_LIN_REPORT_FMT, "OFF (default)", N48_RP_LIN_REFUSED_TXT,
                      N48_RP_LIN_INERT_TXT, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                      (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M, (unsigned long long)M,
                      (unsigned long long)M);
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "L6: switch 59's report line worst case %d bytes < %u", n, N48_RP_LOG_BODY_CAP);
    std::printf(" p59 %d", n);
    n = std::snprintf(buf, sizeof buf, "AppleHardwareHook: " N48_RP_LIN_REPORT2_FMT, (unsigned long long)M, (unsigned long long)M);
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "L6: switch 59's second report line worst case %d bytes < %u", n, N48_RP_LOG_BODY_CAP);
    std::printf(" p59b %d (cap %u)\n", n, N48_RP_LOG_BODY_CAP);
    for (uint32_t r = 0; r <= N48_RP_LIN_REASONS; r++) CHECK(n48_rp_lin_name(r) && n48_rp_lin_name(r)[0], "L6: reason %u named", r);
    std::snprintf(buf, sizeof buf, N48_RP_BK2_FMT, N48_RP_BK2_ARGS(1ull, (void *)0x1000, 1, 144u, 144u, 1u, 0u, 0x2au, 0ull, 0u, 0u, 0u, 144u, 144u, 0x36u, 0x400u));
    CHECK(std::strstr(buf, "+0x3a 144") && std::strstr(buf, "+0x44 0x400 (backing SW 0 type 1") && std::strstr(buf, "+0x08 0 "),
          "L6: line 2 names the backing's pitch, swizzle/type and offset: %s", buf);
    // step 0's middle row and extent for the avatar and the mask
    uint32_t row = 0; uint64_t mid = 0, e0 = 0, e1 = 0;
    const n48_rp_lin_in av = fx_in(fx_serial(11)), mk = fx_in(fx_serial(12));
    CHECK(n48_rp_lin_step0_geom(&av, &row, &mid, &e0, &e1) == 1 && row == 72u && mid == 72ull * 576u && e0 == 0u && e1 == 82944u,
          "L6: the avatar's middle row is y 72 at +%#llx, extent [0, 82944)", (unsigned long long)mid);
    CHECK(n48_rp_lin_step0_geom(&mk, &row, &mid, &e0, &e1) == 1 && row == 20u && mid == 0x100u + 20u * 256u && e0 == 0x100u && e1 == 0x100u + 40u * 256u + 209u,
          "L6: the mask's middle row is y 20 past the 0x100 header, extent [0x100, %#llx)", (unsigned long long)e1);
    n48_rp_lin_in unk = mk; unk.fmtRes = 0x01;
    CHECK(n48_rp_lin_step0_geom(&unk, &row, &mid, &e0, &e1) == 0, "L6: an unknown element size reads nothing (n/a)");
}
// L7: the COPIED line's suffixes.
static void test_lin_instruments() {
    const size_t oldLen = std::strlen(N48_RP_RETILE_SUFFIX_OLD);
    const uint32_t kinds[] = { N48_RP_KIND_4KB_D_X_32, N48_RP_KIND_4KB_D_X_8, N48_RP_KIND_4KB_D_X_64, N48_RP_KIND_256B_D_8, N48_RP_KIND_NONE };
    for (uint32_t lin = 0; lin < 2; lin++)
        for (uint32_t k : kinds) CHECK(std::strlen(n48_rp_retile_suffix(lin, k)) <= oldLen, "L7: suffix (lin %u kind %u) is no longer than the old one", lin, k);
    unsigned longer = 0, unnamed = 0;
    for (uint32_t m = 0; m <= 4; m++) for (uint32_t b = 0; b <= 5; b++) {
        const char *sfx = n48_rp_lin_suffix(m, b);
        if (std::strlen(sfx) > oldLen) longer++;
        if (m >= 1 && m <= 3 && b <= 4 && (!std::strstr(sfx, "LINEAR") || !std::strstr(sfx, m == 1 ? "256B_2D" : m == 2 ? "4KB_2D" : "64KB_2D"))) unnamed++;
    }
    CHECK(longer == 0u && unnamed == 0u, "L7: every backing-sourced suffix names LINEAR and its gfx12 mode, none longer than the old one (%u longer, %u unnamed)", longer, unnamed);
    CHECK(std::strstr(n48_rp_lin_suffix(3, 2), "32bpp") && std::strstr(n48_rp_lin_suffix(1, 3), "64bpp") && std::strstr(n48_rp_lin_suffix(2, 0), "8bpp"),
          "L7: the element size is named");
    CHECK(std::strstr(n48_rp_retile_suffix(0, N48_RP_KIND_256B_D_8), "256B_D 8bpp") && std::strstr(n48_rp_retile_suffix(0, N48_RP_KIND_256B_D_8), "ADDR3_256B_2D"),
          "L7: a 256B_D re-tile names 256B_D and ADDR3_256B_2D");
    n48_rp_shape s {};
    s.surf = 0x20; s.hasMask = 1; s.maskOk = 1; s.maskSurf = 0x36; s.w = 209; s.h = 41; s.depth = 1; s.rowBytes = 256; s.bytes = 0x3000;
    s.gbRead = 1; s.gb = 0x08200545u;
    const uint32_t r32 = n48_rp_shape_check(&s), r8 = n48_rp_shape_check_kind(&s, N48_RP_KIND_4KB_D_X_8), r64 = n48_rp_shape_check_kind(&s, N48_RP_KIND_4KB_D_X_64);
    uint32_t logged = r32; logged = n48_rp_shape_further(logged, r8); logged = n48_rp_shape_further(logged, r64);
    CHECK(logged == N48_RP_SHAPE_BYTES_SMALL, "L7: the per-copy shape reports the check that actually refused (%u, want 10)", logged);
}
// L8: the switch gate and the priority rule.
static void test_lin_gate() {
    CHECK(n48_rp_lin_take(N48_RP_LIN_OK, 1u) == 1, "L8: check OK and switch 59 ON: taken");
    CHECK(n48_rp_lin_take(N48_RP_LIN_OK, 0u) == 0, "L8: switch 59 OFF: NOT taken, whatever the check says");
    for (uint32_t r = 1; r < N48_RP_LIN_REASONS; r++) CHECK(n48_rp_lin_take(r, 1u) == 0, "L8: refusal %u is never taken", r);
    // item 3: the gfx10 re-tiles refuse every record whose backing is LINEAR (SW 0 or 32), nibbles 0/1/7/8, only with 59 ON
    unsigned blocked = 0;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) blocked += n48_rp_lin_blocks_old(1u, kRun10fBacking[i].nibble, kRun10fBacking[i].recOk, kRun10fBacking[i].swzBk) ? 1u : 0u;
    CHECK(blocked == N48_FX_BACKING_N, "L8: with 59 ON the gfx10 re-tiles refuse all 41 run10f records (every backing is linear): %u", blocked);
    unsigned bad = 0;
    for (uint32_t nib = 0; nib < 16; nib++) for (uint32_t sw = 0; sw < 64; sw++) for (uint32_t ok = 0; ok < 2; ok++) {
        const int want = (nib == 0 || nib == 1 || nib == 7 || nib == 8) && ok && (sw == 0 || sw == 32);
        if (n48_rp_lin_blocks_old(1u, nib, ok, 0x400u | (sw << 4)) != want) bad++;
        if (n48_rp_lin_blocks_old(0u, nib, ok, 0x400u | (sw << 4)) != 0) bad++;
    }
    CHECK(bad == 0u, "L8: the predicate over every nibble x backing swizzle x record readability, both switch states (%u wrong)", bad);
    // F3's window (run10f #1) is exactly the resource 0.0.491 re-tiled as gfx10 4KB_D_X 32bpp and RECORDED: its shape passes today's
    // check, so with 59 OFF it is re-tiled as before, and with 59 ON the re-tile is refused and the backing-sourced copy takes it.
    const n48_fx_backing &f3 = fx_serial(1);
    n48_rp_shape s {};
    s.surf = 0x20; s.hasMask = 1; s.maskOk = 1; s.maskSurf = f3.swzVram; s.w = f3.w; s.h = f3.h; s.depth = f3.depth; s.rowBytes = f3.rowBytes;
    s.bytes = f3.bytes; s.gbRead = 1; s.gb = 0x08200545u;
    CHECK(n48_rp_shape_check(&s) == N48_RP_SHAPE_OK && n48_rp_lin_blocks_old(1u, f3.nibble, 1u, f3.swzBk) == 1 &&
          n48_rp_lin_blocks_old(0u, f3.nibble, 1u, f3.swzBk) == 0 && lin_why(fx_in(f3)) == N48_RP_LIN_OK,
          "L8: F3's 0x400006000: gfx10 re-tile OFF-identical, refused with 59 ON, and the backing-sourced copy takes it");
}
// L9: REACHABILITY in the kext's order, for every run10f record.
static int lin_chain(const n48_fx_backing &r, uint32_t sw59, uint32_t *clamp, uint32_t *recWhy, uint32_t *modeOut) {
    const n48_rp_lin_in in = fx_in(r);
    n48_rp_lin_plan p {};
    const uint32_t why = n48_rp_lin_check(&in, &p);
    *clamp = 0; *recWhy = 99; *modeOut = 0;
    if (!n48_rp_lin_take(why, sw59)) return -1;                     // the plain copy: nothing below runs
    std::vector<uint8_t> src((size_t)p.srcEnd, 0x5A), dst((size_t)p.g12Bytes);
    if (!n48_rp_lin_to_g12(&p, r.w, r.h, src.data(), src.size(), dst.data(), dst.size())) return -2;
    // n48_rp_copy exactly as residency_copy_to_vram fills it (rc.bytes = wBytes = the gfx12 length; rc.mode = rtMode =
    // rp_lin_mark_pageout's return = p.g12Mode; rp_lin_fill_copy: lin, w, h, elemBytes = p.bpe)
    n48_rp_copy rc {};
    rc.copied = 1u; rc.retiled = 1u; rc.bytes = p.g12Bytes; rc.compared = p.g12Bytes / 4; rc.mismatched = 0;
    rc.vaOk = 1; rc.va = r.va; rc.contiguous = 1u; rc.vram = 0x13000000ull; rc.mode = p.g12Mode;
    rc.lin = 1u; rc.w = r.w; rc.h = r.h; rc.elemBytes = p.bpe;
    rc.ownerWs = 1u; rc.ctx = 5; rc.arm = 1; rc.epoch = 3;
    static n48_rp t; t = n48_rp {};
    *recWhy = n48_rp_record(&t, &rc);
    const LinVm vm = { r.va, 0x13000000ull, p.g12Bytes };
    // the ask exactly as xlat12_ib.c's table step makes it: the TRANSLATED record's SW_MODE, the gfx10 FORMAT's element size
    uint32_t t10[8], g12[8];
    t10_for(t10, r.va, r.fmtRes, r.swzVram & 0x1fu, r.w, r.h, r.fmtRes == 0x0fu && r.w == 144u ? 7u : 0u);
    if (xlat12_img_desc_g10_to_g12(t10, g12) != XLAT12_DESC_OK) return -3;
    const uint32_t mode = (g12[3] >> 20) & 0x1Fu;
    *modeOut = mode;
    const uint32_t eb = xlat12_format_elem_bytes((t10[1] >> 20) & 0x1FFu);
    const uint64_t sva = ((uint64_t)(t10[1] & 0xFFu) << 40) | ((uint64_t)t10[0] << 8);
    return n48_rp_ok_t(&t, 5, sva, mode, eb, 1, 3, &lin_walk, &vm, 0, 1, 2, t10, clamp);
}
static void test_lin_reach() {
    unsigned proven = 0, off = 0, bad = 0;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) {
        uint32_t cl = 0, rw = 0, md = 0;
        const int on = lin_chain(kRun10fBacking[i], 1u, &cl, &rw, &md);
        if (on == 1 && rw == N48_RP_REC_OK && cl == 1u && md == n48_rp_lin_g12_mode(kRun10fBacking[i].swzVram & 0x1fu)) proven++;
        else { bad++; std::printf("FAIL L9 #%u: chain %d rec %u clamp %u mode %u\n", kRun10fBacking[i].serial, on, rw, cl, md); }
        if (lin_chain(kRun10fBacking[i], 0u, &cl, &rw, &md) == -1 && rw == 99u) off++;
    }
    CHECK(bad == 0u && proven == N48_FX_BACKING_N, "L9: all 41 records: check, switch, streamed convert, record (reason 0), the translator's ask with the record's own T#: proven with the clamp, in the VRAM side's gfx12 mode (%u proven)", proven);
    CHECK(off == N48_FX_BACKING_N, "L9: switch 59 OFF: nothing is converted or recorded for any record (%u)", off);
    n48_fx_backing nl = fx_serial(11); nl.swzBk = 0x400 | (22u << 4);
    uint32_t cl = 0, rw = 0, md = 0;
    CHECK(lin_chain(nl, 1u, &cl, &rw, &md) == -1, "L9: a non-linear backing is never taken");
}
// L10: the 41 records' verdicts.
static void test_lin_fixture() {
    struct Want { uint32_t serial, g12Mode, bpe; uint64_t g12Bytes; uint32_t chunks; };
    static const Want kLut = { 0, 1, 8, 0x2000, 1 };
    static const Want kWant[] = {
        { 1, 2, 4, 0x2000, 1 },      /* F3's window 28x40 8_8_8_8, VRAM 4KB_D_X, SW 32 backing */
        { 2, 1, 1, 0x800, 1 },       /* P's 0x400003000 128x16, VRAM 256B_D, 0x100 header */
        { 3, 1, 1, 0x1200, 1 },      /* P's 0x400032000 141x30, pitch 192 */
        { 5, 1, 1, 0xa00, 1 },       /* 73x27 */
        { 6, 2, 8, 0x1000, 1 },      /* 0x400031000 20x13 fp16, VRAM 4KB_D_X */
        { 7, 2, 8, 0x1000, 1 },      /* AL's 0x400025000 15x16 fp16 */
        { 9, 3, 4, 0x870000, 9 },    /* THE WALLPAPER 0x405800000 1920x1080, VRAM 64KB_R_X, streamed */
        { 10, 2, 1, 0x4000, 1 },     /* 0x400238000 241x48 */
        { 11, 2, 4, 0x19000, 1 },    /* THE AVATAR 144x144 (0x400640000 this boot) */
        { 12, 2, 1, 0x4000, 1 },     /* THE MASK 0x40023c000 209x41 */
    };
    unsigned ok = 0, bad = 0, luts = 0;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) {
        const n48_fx_backing &r = kRun10fBacking[i];
        const Want *w = &kLut;
        for (const auto &k : kWant) if (k.serial == r.serial) w = &k;
        if (w == &kLut) luts++;
        const n48_rp_lin_in in = fx_in(r);
        n48_rp_lin_plan p {};
        const uint32_t why = n48_rp_lin_check(&in, &p);
        unsigned n = 0;
        if (why == N48_RP_LIN_OK) (void)chunk_audit(p, r.w, r.h, &n);
        if (why == N48_RP_LIN_OK && p.g12Mode == w->g12Mode && p.bpe == w->bpe && p.g12Bytes == w->g12Bytes && n == w->chunks &&
            (w != &kLut || (r.w == 256u && r.h == 1u && r.fmtRes == 0x2bu))) ok++;
        else { bad++; std::printf("FAIL L10 #%u VA %#llx: why %u (%s) mode %u bpe %u len %#llx chunks %u\n", r.serial, r.va, why, n48_rp_lin_name(why),
                                  p.g12Mode, p.bpe, (unsigned long long)p.g12Bytes, n); }
    }
    CHECK(bad == 0u && ok == N48_FX_BACKING_N && luts == 31u,
          "L10: all 41 run10f records convert as predicted (10 named + 31 fp16 LUTs 256x1 -> ADDR3_256B_2D 64bpp 0x2000): %u ok, %u bad", ok, bad);
    // the 0.0.486 verdicts on hardware, for the record: 4 (SW 32 'not linear') x4, 3 (VRAM not 4KB_D_X) x35, 6 (format) x2
    unsigned c4 = 0, c3 = 0, c6 = 0;
    for (uint32_t i = 0; i < N48_FX_BACKING_N; i++) { c4 += kRun10fBacking[i].why486 == 4u; c3 += kRun10fBacking[i].why486 == 3u; c6 += kRun10fBacking[i].why486 == 6u; }
    CHECK(c4 == 4u && c3 == 35u && c6 == 2u, "L10: the fixture is RUN A's: 0.0.486 said 4 x%u, 3 x%u, 6 x%u", c4, c3, c6);
    std::printf("  L10 run10f: %u of %u records convert (0.0.486 on hardware: 0); the refusals 0.0.486 logged were 4 x%u, 3 x%u, 6 x%u\n",
                ok, N48_FX_BACKING_N, c4, c3, c6);
}
static void test_lin492() {
    test_lin_golden();
    test_lin_roundtrip();
    test_lin_refusals();
    test_lin_format();
    test_lin_modes();
    test_lin_g10pitch();
    test_lin_stream();
    test_lin_ask();
    test_lin_off_identity();
    test_lin_drop();
    test_lin_lines();
    test_lin_instruments();
    test_lin_gate();
    test_lin_reach();
    test_lin_fixture();
}

// ---- K. build 0.0.493: THE KNOWN-ASSET ROW FOR NIBBLE-4 TILED IMAGES (ws_resprov.h section 7) ---------------------------------
// The real lines, verbatim: run10g driverlog-stream.txt lines 13087 and 13090-13093 (the copy and its first 32 source dwords), 9825 (a
// 1x1 nibble-4 "pointer buffer" of the same boot), and the consumer T# run10d capdec/regions/F00044-USER2-4000b01a0.bin +0.
static const char *kK53Copied = "Navi48AccelPeer: residency-copy: COPIED #53 resource=0xffffff9038eae800 type=0x40 nibble=4 bytes=0x1000 "
    "backingOffset=0 -> VRAM [0x1002f000,0x10030000) via the MM window in 1 chunk(s), 2012 us; read-back 1024 dword(s), 0 MISMATCHED; "
    "first source dwords 4a5462c2 aa6b7f98; GPU VA 0x400024000 (source map 0x4000bb000); surface SW_MODE 0 type 1 32x32 rowBytes 4096; "
    "swz res+0x1dc 0x20 res+0x180 0 *(res+0x180)+0x40 n/a 0 (SW_MODE 0 type 0)";
static const char *kK53Src[4] = {
    "Navi48AccelPeer: residency-copy: #53 source+0 4a5462c2 aa6b7f98 a77a81af b2937193 b9798677 c7af474f 1c377805 7842809a",
    "Navi48AccelPeer: residency-copy: #53 source+0x20 7a7b2a9a 3996a78e aacb6fc3 8a984fb0 c3bf9452 0740bcbd a17dcfaa 5e5b8632",
    "Navi48AccelPeer: residency-copy: #53 source+0x40 80c2b981 80c194dd 93304498 75b46da5 8583b86b ce8deb45 94423421 2eef509e",
    "Navi48AccelPeer: residency-copy: #53 source+0x60 606b6f3c 112848b3 6a8785b1 94addb6f d7b0668e b2d258ed 858d7479 a65a8b28",
};
static const char *kK1Copied = "Navi48AccelPeer: residency-copy: COPIED #1 resource=0xffffff9038ea1000 type=0x40 nibble=4 bytes=0x10000 "
    "backingOffset=0 -> VRAM [0x10000000,0x10010000) via the MM window in 1 chunk(s), 31868 us; read-back 16384 dword(s), 0 MISMATCHED; "
    "first source dwords 000b0000 00000004; GPU VA 0x4000c0000 (source map 0x4000e0000); surface SW_MODE 0 type 1 1x1 rowBytes 65536; "
    "swz res+0x1dc 0x20 res+0x180 0 *(res+0x180)+0x40 n/a 0 (SW_MODE 0 type 0)";
static const uint32_t kAzT10[8] = { 0x04000240u, 0xc3800000u, 0x8007c007u, 0x99600f2eu, 0x00000000u, 0x00400000u, 0u, 0u };
static const uint32_t kKGb = 0x08200545u;   // this card's GB_ADDR_CONFIG (section 1's banner), in the proven class
// The COPIED line's facts, parsed as the kext's own format prints them (type = res+0x14, nibble, bytes = res+0x230, backingOffset =
// res+0xf8, w x h = res+0xb0/+0xb2, rowBytes = res+0xb8).
static bool k_parse_copied(const char *l, n48_rp_known_in *in, uint64_t *va) {
    const char *p = std::strstr(l, "type=0x"); const char *q = std::strstr(l, " nibble="); const char *b = std::strstr(l, " bytes=0x");
    const char *o = std::strstr(l, "backingOffset="); const char *v = std::strstr(l, "GPU VA 0x"); const char *s = std::strstr(l, "type 1 ");
    const char *rb = std::strstr(l, "rowBytes ");
    if (!p || !q || !b || !o || !v || !s || !rb) return false;
    unsigned t = 0, n = 0, w = 0, h = 0, r = 0; unsigned long long by = 0, bo = 0, a = 0;
    if (std::sscanf(p, "type=0x%x", &t) != 1 || std::sscanf(q, " nibble=%u", &n) != 1 || std::sscanf(b, " bytes=0x%llx", &by) != 1 ||
        std::sscanf(o, "backingOffset=%lli", &bo) != 1 || std::sscanf(v, "GPU VA 0x%llx", &a) != 1 ||
        std::sscanf(s, "type 1 %ux%u", &w, &h) != 2 || std::sscanf(rb, "rowBytes %u", &r) != 1) return false;
    in->nibble = n; in->resType = t; in->bytes = by; in->backingOffset = bo; in->w = w; in->h = h; in->rowBytes = r; *va = a;
    return true;
}
static bool k_parse_src(uint32_t *key) {
    for (int i = 0; i < 4; i++) {
        const char *p = std::strstr(kK53Src[i], "source+");
        unsigned off = 0; int adv = 0;
        if (!p || std::sscanf(p, "source+%i%n", &off, &adv) != 1 || off != (unsigned)i * 0x20u) return false;
        p += adv;
        for (int j = 0; j < 8; j++) { unsigned x = 0; int a2 = 0; if (std::sscanf(p, " %x%n", &x, &a2) != 1) return false; key[i * 8 + j] = x; p += a2; }
    }
    return true;
}
// A 4 KiB source whose first 32 dwords are the captured key and whose rest is a deterministic pattern (the captures hold no more).
static void k_source(uint32_t *src) {
    uint32_t key[32]; (void)k_parse_src(key);
    for (uint32_t i = 0; i < 1024u; i++) src[i] = i < 32u ? key[i] : 0x9e3779b9u * (i + 1u) ^ 0x5bd1e995u;
}
static void test_known_row() {
    n48_rp_known_in in {}; uint64_t va = 0; uint32_t key[32] = { 0 };
    CHECK(k_parse_copied(kK53Copied, &in, &va) && k_parse_src(key), "K1: the run10g #53 lines parse");
    const n48_rp_known_row &r = kN48RpKnownNib4[0];
    CHECK(in.nibble == r.nibble && in.resType == r.resType && in.bytes == r.bytes && in.backingOffset == r.backingOffset &&
          in.w == r.w && in.h == r.h && in.rowBytes == r.rowBytes && va == 0x400024000ull,
          "K1: the row's facts ARE the COPIED line's (nibble 4, type 0x40, 0x1000 B, offset 0, 32x32, rowBytes 4096, VA 0x400024000)");
    CHECK(std::memcmp(key, r.key, sizeof key) == 0, "K1: the row's key IS the line's 32 source dwords");
    CHECK(n48_rp_known_row_for(&in) == 0u, "K1: the real line's facts select row 0");
    unsigned dup = 0, zero = 0;
    for (uint32_t i = 0; i < 32; i++) { zero += r.key[i] == 0u; for (uint32_t j = i + 1; j < 32; j++) dup += r.key[i] == r.key[j]; }
    CHECK(dup == 0u && zero == 0u, "K1: the key is 32 distinct non-zero dwords (%u equal pairs, %u zero)", dup, zero);
    // the consumer T#: FORMAT 56, 32x32, SW 22, TYPE 9, levels 0 - the row's g10Sw / fmt10 / dims
    const uint32_t fmt = (kAzT10[1] >> 20) & 0x1ffu, sw = (kAzT10[3] >> 20) & 0x1fu;
    const uint32_t w = (((kAzT10[1] >> 30) & 3u) | ((kAzT10[2] & 0xfffu) << 2)) + 1u, h = ((kAzT10[2] >> 14) & 0x3fffu) + 1u;
    CHECK(fmt == r.fmt10 && sw == r.g10Sw && w == r.w && h == r.h && (kAzT10[3] >> 28) == 9u && ((kAzT10[5] >> 4) & 0xfu) == 0u &&
          xlat12_format_elem_bytes(fmt) == r.elemBytes, "K1: the run10d T# states the row's FORMAT 56 (4 B), SW 22, 32x32, TYPE 9, one level");
    uint32_t g12[8] = { 0 };
    CHECK(xlat12_img_desc_g10_to_g12(kAzT10, g12) == XLAT12_DESC_OK && ((g12[3] >> 20) & 0x1fu) == r.g12Mode,
          "K1: the translator maps that T#'s SW 22 to the row's gfx12 mode %u", r.g12Mode);
    CHECK(N48_RP_KNOWN_N == sizeof(kN48RpKnownNib4) / sizeof(kN48RpKnownNib4[0]), "K1: one row, counted");
}
static uint32_t swap34(uint32_t a) { return (a & ~0x18u) | (((a >> 3) & 1u) << 4) | (((a >> 4) & 1u) << 3); }
static void test_known_convert() {
    // golden: every texel of 32x32 at 4 B: dst at the gfx12 4KB_2D offset holds what src had at the gfx10 4KB_D_X offset
    std::vector<uint32_t> src(1024), dst(1024, 0xdeadbeefu), back(1024, 0u);
    for (uint32_t y = 0; y < 32; y++) for (uint32_t x = 0; x < 32; x++) src[n48_rp_g10_off(x, y, 1) >> 2] = (y << 16) | x | 0xa5000000u;
    CHECK(n48_rp_known_convert(0, 1, kKGb, src.data(), dst.data(), 0x1000) == N48_RP_KN_OK, "K2: the row converts under the card's config");
    unsigned bad = 0, badGen = 0, badSwap = 0;
    for (uint32_t y = 0; y < 32; y++) for (uint32_t x = 0; x < 32; x++) {
        const uint32_t want = (y << 16) | x | 0xa5000000u;
        if (dst[n48_rp_g12_off(x, y, 1) >> 2] != want) bad++;
        if (n48_g12_off2d(12u, 2u, x, y, 1u) != n48_rp_g12_off(x, y, 1)) badGen++;          // the generic (L1-golden) gfx12 equation
        if (swap34(n48_rp_g10_off(x, y, 1)) != n48_rp_g12_off(x, y, 1)) badSwap++;          // inside the block: bits 3 and 4 swap
    }
    CHECK(bad == 0u, "K2: every texel lands at its gfx12 ADDR3_4KB_2D offset (%u wrong)", bad);
    CHECK(badGen == 0u && badSwap == 0u, "K2: the equations agree with the generic gfx12 one (%u) and differ only by bits 3/4 (%u)", badGen, badSwap);
    CHECK(n48_rp_retile(dst.data(), back.data(), 32, 32, 0x1000, 0) == 1 && back == src, "K2: round trip gfx12 -> gfx10 restores every dword");
    uint64_t f = 0xcbf29ce484222325ull; for (uint32_t v : dst) f = fnv(f, v);
    // a REGRESSION pin (first computed by this build; the per-texel check above is the golden, this only freezes it)
    CHECK(f == 0xe001a175efc00c25ull, "K2: the converted image's hash is the pinned one");
    std::printf("  K2 converted-image fnv %#018llx\n", (unsigned long long)f);
    // the same on the real-keyed source: the key's 32 dwords (texels x 0..7, y 0..3 of gfx10's first 128 B) are moved, not kept
    std::vector<uint32_t> ks(1024), kd(1024);
    k_source(ks.data());
    CHECK(n48_rp_known_convert(0, 1, kKGb, ks.data(), kd.data(), 0x1000) == N48_RP_KN_OK && std::memcmp(kd.data(), ks.data(), 128) != 0,
          "K2: the keyed source converts and its first 128 bytes are re-ordered (a skipped conversion would leave them)");
    // refusals of the convert step
    CHECK(n48_rp_known_convert(0, 0, kKGb, src.data(), dst.data(), 0x1000) == N48_RP_KN_CFG, "K2: an unread config refuses");
    CHECK(n48_rp_known_convert(0, 1, 0x00000442u, src.data(), dst.data(), 0x1000) == N48_RP_KN_CFG, "K2: a 4-pipe config (outside the class) refuses");
    CHECK(n48_rp_known_convert(0, 1, kKGb, src.data(), dst.data(), 0x2000) == N48_RP_KN_CONVERT, "K2: a different length refuses");
    CHECK(n48_rp_known_convert(1, 1, kKGb, src.data(), dst.data(), 0x1000) == N48_RP_KN_CONVERT, "K2: a row past the table refuses");
}
static n48_rp_copy k_copy() {
    n48_rp_copy c; std::memset(&c, 0, sizeof c);
    c.copied = 1; c.retiled = 1; c.bytes = 0x1000; c.compared = 0x400; c.ownerWs = 1; c.ctx = 5; c.vaOk = 1; c.va = 0x400024000ull;
    c.contiguous = 1; c.vram = 0x1002f000ull; c.mode = N48_RP_G12_4KB_2D; c.arm = 1; c.epoch = 3; c.elemBytes = 4; c.lin = N48_RP_LIN_KNOWN0;
    c.w = 32; c.h = 32;
    return c;
}
static void test_known_ask() {
    const LinVm vm = { 0x400024000ull, 0x1002f000ull, 0x1000ull };
    n48_rp t {};
    n48_rp_copy c = k_copy();
    CHECK(n48_rp_record(&t, &c) == N48_RP_REC_OK && t.n == 1 && t.e[0].lin == 1u && t.e[0].known == 1u && t.e[0].w == 32u,
          "K3: a known-asset copy records as lin, known = row + 1");
    uint32_t cl = 9;
    CHECK(n48_rp_ok_t(&t, 5, 0x400024000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, kAzT10, &cl) == 1 && cl == 1u && t.knownProven == 1u &&
          t.linProven == 1u, "K3: the real run10d T# is ANSWERED (clamp handed, knownProven)");
    CHECK(n48_rp_ok(&t, 5, 0x400024000ull, 2, 4, 1, 3, &lin_walk, &vm) == 0 && n48_rp_ok_carry(&t, 5, 0x400024000ull, 2, 4, 1, 3, &lin_walk, &vm, 1, 1, 2) == 0,
          "K3: the asks without a T# refuse it (it is a lin entry)");
    struct { const char *name; void (*mut)(uint32_t *); } bads[] = {
        { "SW_MODE 21 (4KB_S_X, same gfx12 mode)", [](uint32_t *x) { x[3] = (x[3] & ~(0x1Fu << 20)) | (21u << 20); } },
        { "SW_MODE 23 (4KB_R_X, same gfx12 mode)", [](uint32_t *x) { x[3] = (x[3] & ~(0x1Fu << 20)) | (23u << 20); } },
        { "SW_MODE 6 (4KB_D, same gfx12 mode)", [](uint32_t *x) { x[3] = (x[3] & ~(0x1Fu << 20)) | (6u << 20); } },
        { "BASE_LEVEL 1", [](uint32_t *x) { x[3] |= 1u << 12; } },
        { "BASE_ARRAY 1", [](uint32_t *x) { x[4] |= 1u << 16; } },
        { "DEPTH 1", [](uint32_t *x) { x[4] |= 1u; } },
        { "TYPE 8", [](uint32_t *x) { x[3] = (x[3] & 0x0FFFFFFFu) | (8u << 28); } },
        { "32x16", [](uint32_t *x) { t10_set_wh(x, 32, 16); } },
        { "64x64", [](uint32_t *x) { t10_set_wh(x, 64, 64); } },
        { "FORMAT 71 (16_16_16_16_FLOAT)", [](uint32_t *x) { x[1] = (x[1] & ~(0x1FFu << 20)) | (71u << 20); } },
        { "LAST_LEVEL 1 > MAX_MIP 0", [](uint32_t *x) { x[3] |= 1u << 16; } },
    };
    for (auto &b : bads) {
        uint32_t x[8]; std::memcpy(x, kAzT10, sizeof x); b.mut(x);
        const uint64_t before = t.knownTRefused;
        uint32_t c2 = 7;
        CHECK(n48_rp_ok_t(&t, 5, 0x400024000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, x, &c2) == 0 && t.knownTRefused == before + 1 && c2 == 0u,
              "K4: a T# with %s refuses (knownTRefused), no clamp", b.name);
    }
    const uint64_t em = t.elemMismatch;
    CHECK(n48_rp_ok_t(&t, 5, 0x400024000ull, 2, 8, 1, 3, &lin_walk, &vm, 0, 1, 2, kAzT10, &cl) == 0 && t.elemMismatch == em + 1,
          "K4: an ask at 8 bytes per element refuses (elemMismatch)");
    CHECK(n48_rp_ok_t(&t, 5, 0x400024000ull, 2, 4, 1, 3, &lin_walk, &vm, 0, 1, 2, nullptr, &cl) == 0, "K4: no T# refuses");
    // n48_rp_lin_t_match alone would ADMIT a SW 22 T# of another FORMAT: the row's check is what refuses it
    n48_rp_ent probe = t.e[0]; probe.known = 0;
    uint32_t f71[8]; std::memcpy(f71, kAzT10, sizeof f71); f71[1] = (f71[1] & ~(0x1FFu << 20)) | (71u << 20);
    CHECK(n48_rp_lin_t_match(&probe, f71) == 1 && n48_rp_known_t_match(&t.e[0], f71) == 0, "K4: the FORMAT check is the row's own");
    // the record clause: a known copy that is not exactly its row
    struct { const char *name; void (*mut)(n48_rp_copy *); } rb[] = {
        { "gfx12 mode 1", [](n48_rp_copy *x) { x->mode = 1; } },
        { "gfx12 mode 3", [](n48_rp_copy *x) { x->mode = 3; } },
        { "width 16", [](n48_rp_copy *x) { x->w = 16; } },
        { "element size 8", [](n48_rp_copy *x) { x->elemBytes = 8; } },
        { "0x2000 bytes", [](n48_rp_copy *x) { x->bytes = 0x2000; x->compared = 0x800; } },
        { "row 1 (past the table)", [](n48_rp_copy *x) { x->lin = N48_RP_LIN_KNOWN0 + 1u; } },
    };
    for (auto &b : rb) { n48_rp tr {}; n48_rp_copy x = k_copy(); b.mut(&x);
        CHECK(n48_rp_record(&tr, &x) != N48_RP_REC_OK && tr.n == 0, "K4: a known copy with %s is NOT recorded", b.name); }
    // facts: every non-row resource selects nothing (nothing is read for it)
    n48_rp_known_in one {}; uint64_t va = 0;
    CHECK(k_parse_copied(kK1Copied, &one, &va) && n48_rp_known_row_for(&one) == N48_RP_KNOWN_NONE, "K4: the real 1x1 pointer buffer (#1, 0x10000 B) selects no row");
    n48_rp_known_in base {}; (void)k_parse_copied(kK53Copied, &base, &va);
    struct { const char *name; void (*mut)(n48_rp_known_in *); } fb[] = {
        { "bytes 0x2000", [](n48_rp_known_in *x) { x->bytes = 0x2000; } }, { "32x16", [](n48_rp_known_in *x) { x->h = 16; } },
        { "16x32", [](n48_rp_known_in *x) { x->w = 16; } }, { "rowBytes 128", [](n48_rp_known_in *x) { x->rowBytes = 128; } },
        { "backing offset 0x100", [](n48_rp_known_in *x) { x->backingOffset = 0x100; } }, { "nibble 1", [](n48_rp_known_in *x) { x->nibble = 1; } },
        { "type 0xc0", [](n48_rp_known_in *x) { x->resType = 0xc0; } },
    };
    for (auto &b : fb) { n48_rp_known_in x = base; b.mut(&x); CHECK(n48_rp_known_row_for(&x) == N48_RP_KNOWN_NONE, "K4: facts with %s select no row", b.name); }
    // the key: any one dword changed refuses (all 32 positions), a short read refuses
    uint32_t src[1024]; k_source(src);
    CHECK(n48_rp_known_key_ok(0, src, sizeof src) == 1, "K4: the real key matches");
    unsigned keyBad = 0;
    for (uint32_t i = 0; i < 32; i++) { uint32_t s2[32]; std::memcpy(s2, src, sizeof s2); s2[i] ^= 1u << (i % 32); keyBad += n48_rp_known_key_ok(0, s2, sizeof s2) == 0; }
    CHECK(keyBad == 32u, "K4: one dword changed (one bit, at each of the 32 positions) refuses: %u of 32", keyBad);
    CHECK(n48_rp_known_key_ok(0, src, 124) == 0 && n48_rp_known_key_ok(1, src, sizeof src) == 0, "K4: a short source or a row past the table refuses");
    // page-out table
    uint64_t tab[4] = { 0, 0, 0, 0 };
    CHECK(n48_rp_known_pageout(tab, 4, 0x1000) == 0 && n48_rp_known_res_add(tab, 4, 0x1000) == 1 && n48_rp_known_pageout(tab, 4, 0x1000) == 1 &&
          n48_rp_known_pageout(tab, 4, 0x2000) == 0, "K5: a remembered resource's page-out is refused, another's is not");
    CHECK(n48_rp_known_res_add(tab, 4, 0x1000) == 1 && tab[1] == 0, "K5: adding twice keeps one slot");
    for (uint64_t r = 2; r <= 4; r++) (void)n48_rp_known_res_add(tab, 4, r * 0x1000);
    CHECK(n48_rp_known_res_add(tab, 4, 0x9000) == 0 && n48_rp_known_pageout(tab, 4, 0x9000) == 0, "K5: a full table refuses (the conversion is then refused)");
    CHECK(n48_rp_known_res_add(tab, 4, 0) == 0, "K5: a null resource is never added");
}
// The 0.0.492 n48_rp_ok_t and n48_rp_record, verbatim but comments (git show c3d1147:src/navi48-bringup/src/apple/ws_resprov.h;
// the entry field `pad2` is now `known`), for the 0.0.493 OFF identity over lin and non-lin entries.
static int old492_rp_ok_t(n48_rp *t, uint64_t ctx, uint64_t va, uint32_t mode, uint32_t askElemBytes, uint32_t arm,
                          uint32_t epoch, n48_rp_walk walk, const void *vm, uint32_t carry, uint32_t decideArm,
                          uint32_t commitArm, const uint32_t *t10, uint32_t *clamp)
{
    if (clamp) *clamp = 0u;
    t->asked++;
    if (!ctx) return 0;
    for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; k++) {
        const n48_rp_ent *e = &t->e[k];
        if (e->ctx != ctx || e->va != va || e->mode != mode) continue;
        if (e->lin && !n48_rp_lin_t_match(e, t10)) { t->linTRefused++; return 0; }
        if (e->elemBytes != askElemBytes) { t->elemMismatch++; return 0; }
        const int armOk = (e->arm == arm) || (carry != 0u && arm == commitArm && e->arm == decideArm);
        if (!armOk || e->epoch != epoch) { t->stale++; return 0; }
        if (!walk || !vm) { t->walkRefused++; return 0; }
        for (uint64_t p = 0; p < e->bytes; p += 4096ull) {
            uint64_t v = ~0ull;
            if (walk(vm, va + p, &v) != 1 || v != e->vram + p) { t->walkRefused++; return 0; }
        }
        t->proven++;
        if (e->lin) { t->linProven++; if (clamp) *clamp = 1u; }
        return 1;
    }
    return 0;
}
static uint32_t old492_rp_record(n48_rp *t, const n48_rp_copy *c)
{
    uint32_t why = N48_RP_REC_OK;
    if (!c || c->copied != 1u) why = N48_RP_REC_NOT_COPIED;
    else if (!c->bytes || c->mismatched || c->compared * 4ull < c->bytes) why = N48_RP_REC_UNVERIFIED;
    else if (c->ownerWs != 1u) why = N48_RP_REC_NOT_WS;
    else if (!c->ctx) why = N48_RP_REC_NO_CTX;
    else if (c->vaOk != 1u || !c->va || (c->va & 0xfffull) || c->va >= (1ull << 48) || c->va + c->bytes > (1ull << 48))
        why = N48_RP_REC_NO_VA;
    else if (c->contiguous != 1u || (c->vram & 0xfffull)) why = N48_RP_REC_SPLIT;
    else if (!c->mode || c->mode > 7u) why = N48_RP_REC_MODE;
    else if (c->mode == N48_RP_G12_4KB_2D && c->retiled != 1u) why = N48_RP_REC_NOT_RETILED;
    else if (c->lin && (!n48_rp_lin_g10_for_g12(c->mode) || c->retiled != 1u || !c->w || !c->h)) why = N48_RP_REC_MODE;
    if (c && c->copied == 1u && t) {
        for (uint32_t k = 0; k < t->n && k < N48_RP_MAX; ) {
            const n48_rp_ent *e = &t->e[k];
            const int sameVa = c->ctx != 0ull && e->ctx == c->ctx && e->va == c->va;
            const int overVram = c->bytes != 0ull && e->vram < c->vram + c->bytes && c->vram < e->vram + e->bytes;
            if (sameVa || overVram) n48_rp_drop_at(t, k); else k++;
        }
    }
    if (why) { if (t) t->refused[why]++; return why; }
    if (t->n >= N48_RP_MAX) { t->refused[N48_RP_REC_FULL]++; return N48_RP_REC_FULL; }
    n48_rp_ent *e = &t->e[t->n++];
    e->ctx = c->ctx; e->va = c->va; e->vram = c->vram; e->bytes = c->bytes; e->mode = c->mode;
    e->arm = c->arm; e->epoch = c->epoch; e->elemBytes = c->elemBytes;
    e->lin = c->lin ? 1u : 0u; e->w = c->w; e->h = c->h; e->known = 0u;
    t->recorded++;
    return N48_RP_REC_OK;
}
static uint64_t k_hash(const n48_rp &t) {
    uint64_t f = 0xcbf29ce484222325ull;
    const uint64_t v[] = { t.asked, t.proven, t.walkRefused, t.stale, t.elemMismatch, t.recorded, t.dropped, t.linNoT, t.linTRefused,
                           t.linProven, t.knownTRefused, t.knownProven, t.n };
    for (uint64_t x : v) f = fnv(f, x);
    for (uint32_t k = 0; k < t.n && k < N48_RP_MAX; k++) {
        const n48_rp_ent &e = t.e[k];
        const uint64_t w[] = { e.ctx, e.va, e.vram, e.bytes, e.mode, e.arm, e.epoch, e.elemBytes, e.lin, e.w, e.h, e.known };
        for (uint64_t x : w) f = fnv(f, x);
    }
    for (uint32_t r = 0; r < N48_RP_REC_REASONS; r++) f = fnv(f, t.refused[r]);
    return f;
}
// K5: OFF IDENTITY - with switch 59 OFF no copy is ever `lin` >= 2 (rp_known_prepare returns before reading, the T16/T17 pins), so
// every table and ask is 0.0.492's: 20,000 randomized rounds of lin 0/1 records and T# asks against the verbatim 0.0.492 code.
static void test_known_off_identity() {
    uint64_t seed = 0x493493493ull; unsigned diffs = 0, rounds = 0, knownSet = 0, proven = 0, linProven = 0, tRefused = 0;
    auto rnd = [&seed]() { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (uint32_t)(seed >> 33); };
    const LinVm vm = { 0x400024000ull, 0x1002f000ull, 0x2000ull };
    for (int round = 0; round < 20000; round++) {
        n48_rp a {}, b {};
        const uint32_t n = rnd() % 6;
        for (uint32_t k = 0; k < n; k++) {
            n48_rp_copy cp = k_copy();
            cp.lin = rnd() % 2u;                                   // 0 or 1: never a known asset with the switch off
            cp.va = (rnd() & 1) ? 0x400024000ull : 0x400025000ull; cp.vram = 0x1002f000ull + (cp.va - 0x400024000ull);
            cp.mode = (rnd() & 1) ? 2u : 1 + rnd() % 3; cp.elemBytes = 1u << (rnd() % 4); cp.w = (rnd() & 1) ? 32 : 144; cp.h = cp.w;
            cp.epoch = rnd() & 1; cp.retiled = (rnd() % 5) ? 1u : 0u; cp.bytes = (rnd() & 1) ? 0x1000 : 0x2000; cp.compared = cp.bytes / 4;
            if (n48_rp_record(&a, &cp) != old492_rp_record(&b, &cp)) diffs++;
        }
        for (uint32_t k = 0; k < a.n; k++) knownSet += a.e[k].known != 0u;
        uint32_t x[8]; std::memcpy(x, kAzT10, sizeof x);
        if ((rnd() % 4) == 0) t10_set_wh(x, 144, 144);
        if ((rnd() % 4) == 0) x[3] = (x[3] & ~(0x1Fu << 20)) | ((rnd() % 32u) << 20);
        if ((rnd() % 4) == 0) x[1] = (x[1] & ~(0x1FFu << 20)) | (71u << 20);
        uint64_t va = (rnd() & 1) ? 0x400024000ull : 0x400025000ull;
        uint32_t mode = 1 + rnd() % 3, eb = 1u << (rnd() % 4), ep = rnd() & 1;
        const uint32_t carry = rnd() & 1;
        if (a.n && (rnd() & 1)) { const n48_rp_ent &e0 = a.e[rnd() % a.n]; va = e0.va; mode = e0.mode; eb = e0.elemBytes; ep = e0.epoch; }
        uint32_t c1 = 5, c2 = 6;
        const uint32_t *tp = (rnd() % 7) ? x : nullptr;       // the same T# (or none) for both sides
        const int r1 = n48_rp_ok_t(&a, 5, va, mode, eb, 1, ep, &lin_walk, &vm, carry, 1, 2, tp, &c1);
        const int r2 = old492_rp_ok_t(&b, 5, va, mode, eb, 1, ep, &lin_walk, &vm, carry, 1, 2, tp, &c2);
        rounds++;
        proven += r1 == 1; linProven += (r1 == 1 && c1 == 1u); tRefused += (uint32_t)(a.linTRefused != 0u);
        if (r1 != r2 || c1 != c2 || k_hash(a) != k_hash(b)) diffs++;
    }
    CHECK(proven > 100u && linProven > 50u && tRefused > 100u, "K5 OFF identity is not vacuous: %u proven (%u lin), %u with a T# refusal",
          proven, linProven, tRefused);
    CHECK(diffs == 0u && knownSet == 0u, "K5 OFF identity: %u randomized rounds of lin 0/1 records + T# asks vs verbatim 0.0.492: %u differences, "
          "%u entries with `known` set", rounds, diffs, knownSet);
    std::printf("  K5 OFF identity: %u rounds vs 0.0.492, %u differences (%u proven, %u lin proven)\n", rounds, diffs, proven, linProven);
}
// K6: REACHABILITY in the kext's order on the REAL line: copy (facts from COPIED #53) -> key (its 32 source dwords) -> convert ->
// the write (the stream's whole image, read back) -> record (n48_rp_copy exactly as residency_copy_to_vram + rp_lin_fill_copy fill
// it) -> the translator's ask with the real run10d T#. Returns 1 = proven; <= 0 = the step that stopped it.
static int known_chain(const char *copied, const uint32_t *src, uint32_t sw59, uint32_t gb, uint32_t *clamp, uint32_t *recWhy) {
    *clamp = 0; *recWhy = 99;
    n48_rp_known_in in {}; uint64_t va = 0;
    if (!k_parse_copied(copied, &in, &va)) return -9;
    if (!sw59) return -1;                                                // rp_known_prepare: switch 59 OFF, nothing read
    const uint32_t row = n48_rp_known_row_for(&in);                      // FACTS
    if (row == N48_RP_KNOWN_NONE) return -2;
    if (!n48_rp_known_key_ok(row, src, in.bytes)) return -3;             // KEY
    std::vector<uint32_t> dst(1024);
    if (n48_rp_known_convert(row, 1u, gb, src, dst.data(), in.bytes) != N48_RP_KN_OK) return -4;   // CONVERT
    std::vector<uint32_t> vram(dst);                                     // the write loop: 256-byte batches, each read back
    uint64_t compared = 0, mismatched = 0;
    for (uint32_t i = 0; i < 1024u; i++) { compared++; mismatched += vram[i] != dst[i]; }
    n48_rp_copy rc {};
    rc.copied = 1u; rc.retiled = 1u; rc.bytes = in.bytes; rc.compared = compared; rc.mismatched = mismatched;
    rc.vaOk = 1; rc.va = va; rc.contiguous = 1u; rc.vram = 0x1002f000ull;
    rc.mode = N48_RP_G12_4KB_2D;                                         // rp_lin_mark_pageout's return for a known stream
    rc.lin = N48_RP_LIN_KNOWN0 + row; rc.w = in.w; rc.h = in.h; rc.elemBytes = 4u;   // rp_lin_fill_copy
    rc.ownerWs = 1u; rc.ctx = 5; rc.arm = 1; rc.epoch = 3;               // hw_resprov_note_copy
    static n48_rp t; t = n48_rp {};
    *recWhy = n48_rp_record(&t, &rc);                                    // RECORD
    const LinVm vm = { va, 0x1002f000ull, in.bytes };
    uint32_t g12[8];
    if (xlat12_img_desc_g10_to_g12(kAzT10, g12) != XLAT12_DESC_OK) return -5;
    const uint32_t mode = (g12[3] >> 20) & 0x1Fu, eb = xlat12_format_elem_bytes((kAzT10[1] >> 20) & 0x1FFu);
    const uint64_t sva = ((uint64_t)(kAzT10[1] & 0xFFu) << 40) | ((uint64_t)kAzT10[0] << 8);
    // the texel the consumer reads at (x, y) is the source's gfx10 texel (x, y)
    for (uint32_t y = 0; y < 32; y++) for (uint32_t x = 0; x < 32; x++)
        if (vram[n48_rp_g12_off(x, y, 1) >> 2] != src[n48_rp_g10_off(x, y, 1) >> 2]) return -6;
    return n48_rp_ok_t(&t, 5, sva, mode, eb, 1, 3, &lin_walk, &vm, 0, 1, 2, kAzT10, clamp);   // ASK
}
static void test_known_reach() {
    uint32_t src[1024]; k_source(src);
    uint32_t cl = 0, rw = 0;
    CHECK(known_chain(kK53Copied, src, 1u, kKGb, &cl, &rw) == 1 && rw == N48_RP_REC_OK && cl == 1u,
          "K6: REACHABILITY on the real line: facts -> key -> convert -> write/read-back -> record (reason 0) -> the real T#'s ask PROVEN");
    CHECK(known_chain(kK53Copied, src, 0u, kKGb, &cl, &rw) == -1 && rw == 99u, "K6: switch 59 OFF: nothing past the switch");
    CHECK(known_chain(kK1Copied, src, 1u, kKGb, &cl, &rw) == -2, "K6: the real 1x1 pointer buffer stops at FACTS");
    uint32_t s2[1024]; std::memcpy(s2, src, sizeof s2); s2[31] ^= 0x80000000u;
    CHECK(known_chain(kK53Copied, s2, 1u, kKGb, &cl, &rw) == -3 && rw == 99u, "K6: one key dword changed stops at KEY: nothing recorded");
    CHECK(known_chain(kK53Copied, src, 1u, 0x00000442u, &cl, &rw) == -4 && rw == 99u, "K6: an out-of-class config stops at CONVERT");
}
// K7: the lines fit the kext's 491-byte body cap at their widest; the suffix is no longer than the old one.
static void test_known_lines() {
    char buf[2048];
    const uint64_t M = ~0ull;
    int n = std::snprintf(buf, sizeof buf, "%s" N48_RP_KNOWN_FMT, kPeerPfx, N48_RP_KNOWN_ARGS(0u, (void *)M, M, M));
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP && std::strstr(buf, "row 0 \"AZ 32x32 noise"), "K7: the conversion line names the row, %d bytes", n);
    uint32_t widest = 0;
    for (uint32_t r = 0; r < N48_RP_KN_REASONS; r++) if (std::strlen(n48_rp_known_why(r)) > std::strlen(n48_rp_known_why(widest))) widest = r;
    n = std::snprintf(buf, sizeof buf, "%s" N48_RP_KNOWN_REFUSED_FMT, kPeerPfx, 0u, kN48RpKnownNib4[0].name, (void *)M, n48_rp_known_why(widest), widest);
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "K7: the refusal line %d bytes", n);
    n = std::snprintf(buf, sizeof buf, "AppleHardwareHook: " N48_RP_LIN_REPORT3_FMT, 0xffffffffu, (unsigned long long)M, (unsigned long long)M,
                      (unsigned long long)M, (unsigned long long)M, (unsigned long long)M);
    CHECK(n > 0 && (unsigned)n < N48_RP_LOG_BODY_CAP, "K7: switch 59's third report line %d bytes", n);
    CHECK(std::strlen(N48_RP_KNOWN_SUFFIX) <= std::strlen(N48_RP_RETILE_SUFFIX_OLD), "K7: the COPIED suffix is no longer than the old one");
    for (uint32_t r = 0; r <= N48_RP_KN_REASONS; r++) CHECK(n48_rp_known_why(r) && n48_rp_known_why(r)[0], "K7: reason %u named", r);
}
static void test_known493() {
    test_known_row();
    test_known_convert();
    test_known_ask();
    test_known_off_identity();
    test_known_reach();
    test_known_lines();
}

// build 0.0.544 item 4a: THE 1440p WALLPAPER. run11ap-r's `resprov: backing #9 ... 2560x1440 ... switch-59 check 15
// (block row past the chunk bound)` -> `not recorded (reason 2)`. A synthetic 2560x1440 SW27 (ADDR_SW_64KB_R_X -> ADDR3_64KB_2D) 32bpp
// image with the 1080p record's own backing shape (+0x44 0x400 LINEAR, pitch = width, fmt 0x0f / 0x65): the check plans ONE 1.25 MiB
// band per chunk; the conversion lands every texel exactly once and DETILES back to the source image through the gfx12 equation;
// the copy records (reason 0) and the translator's ask with the record's own T# is proven. And the 1080p wallpaper's plan and bytes
// are exactly 0.0.543's (the FNV below was computed with 0.0.543's ws_resprov.h, `git show 93845f86:...`).
static const uint64_t kLin1080Fnv = 0xeec2fb738730ba9full;   // 0.0.543's ws_resprov.h (93845f86), the same source pattern
static uint64_t lin_img_fnv(const n48_rp_lin_plan &p, uint32_t w, uint32_t h, std::vector<uint8_t> *keep) {
    std::vector<uint8_t> src((size_t)p.srcEnd), dst((size_t)p.g12Bytes);
    for (uint64_t i = 0; i < src.size(); i++) src[(size_t)i] = (uint8_t)((i * 2654435761ull) >> 13);
    if (!n48_rp_lin_to_g12(&p, w, h, src.data(), src.size(), dst.data(), dst.size())) return 0ull;
    uint64_t f = 0xcbf29ce484222325ull;
    for (uint64_t i = 0; i + 8u <= dst.size(); i += 8u) { uint64_t v; std::memcpy(&v, &dst[(size_t)i], 8); f = fnv(f, v); }
    if (keep) *keep = dst;
    return f;
}
static void test_lin_1440p() {
    const n48_fx_backing &w1080 = fx_serial(9);
    n48_rp_lin_in in = fx_in(w1080);
    in.w = in.bw = 2560; in.h = in.bh = 1440; in.pitch = 2560; in.rowBytes = 2560u * 4u;
    in.srcLen = in.mdLen = in.bytes = 2560ull * 1440ull * 4ull;   // 14,745,600 B (run11ap-r's copy)
    in.dstLen = 1ull << 26;
    n48_rp_lin_plan p {};
    const uint32_t why = n48_rp_lin_check(&in, &p);
    CHECK(why == N48_RP_LIN_OK && p.g12Mode == 3u && p.bandBytes == 0x140000ull && p.chunkRows == 1u && p.rowsBlk == 12u &&
          p.g12Bytes == 0xF00000ull, "4a: 2560x1440 SW27 32bpp plans (why %u): one 0x140000 band per chunk, 12 bands, 0xF00000 in all",
          why);
    unsigned n = 0; const unsigned bad = why == N48_RP_LIN_OK ? chunk_audit(p, 2560, 1440, &n) : 1u;
    CHECK(bad == 0u && n == 12u, "4a: 12 chunks tile the image, each <= 2 MiB of gfx12 bytes and of source (%u bad, %u chunks)", bad, n);
    std::vector<uint32_t> where;
    CHECK(why == N48_RP_LIN_OK && lin_landing(p, 2560, 1440, where, conv_real), "4a: every 2560x1440 texel lands exactly once, pads zero");
    // detile(tile(img)) == img
    std::vector<uint8_t> src((size_t)p.srcEnd), dst((size_t)p.g12Bytes);
    for (uint64_t i = 0; i < src.size(); i++) src[(size_t)i] = (uint8_t)((i * 40503u) >> 7);
    const int conv = n48_rp_lin_to_g12(&p, 2560, 1440, src.data(), src.size(), dst.data(), dst.size());
    unsigned diff = 0;
    for (uint32_t y = 0; conv && y < 1440u; y++) for (uint32_t x = 0; x < 2560u; x++) {
        const uint64_t g = n48_g12_off2d(16u, 2u, x, y, p.pbk);
        if (std::memcmp(&dst[(size_t)g], &src[(size_t)((uint64_t)y * p.pitchBytes + (uint64_t)x * 4u)], 4) != 0) diff++;
    }
    CHECK(conv == 1 && diff == 0u, "4a: detile(tile(img)) == img at every 2560x1440 texel (%u differ)", diff);
    n48_rp_copy c = lin_copy(0x405000000ull, 0x13000000ull, p.g12Bytes, 2560, 1440, 4, p.g12Mode);
    static n48_rp t; t = n48_rp {};
    const uint32_t rec = n48_rp_record(&t, &c);
    uint32_t t10[8], g12[8], cl = 0;
    t10_for(t10, 0x405000000ull, 0x0fu, 27u, 2560, 1440, 0u);
    const LinVm vm = { 0x405000000ull, 0x13000000ull, p.g12Bytes };
    const int ok = xlat12_img_desc_g10_to_g12(t10, g12) == XLAT12_DESC_OK &&
                   n48_rp_ok_t(&t, 5, 0x405000000ull, (g12[3] >> 20) & 0x1Fu, 4u, 1, 3, &lin_walk, &vm, 0, 1, 2, t10, &cl) == 1;
    CHECK(rec == N48_RP_REC_OK && ok, "4a: the 1440p copy RECORDS (reason %u) and the ask with its own T# is proven", rec);
    // 1080p unchanged
    n48_rp_lin_plan q {};
    const n48_rp_lin_in i1080 = fx_in(w1080);
    CHECK(n48_rp_lin_check(&i1080, &q) == N48_RP_LIN_OK && q.bandBytes == 0xF0000ull && q.chunkRows == 1u && q.g12Bytes == 0x870000ull,
          "4a: 1920x1080: the same plan as 0.0.543 (0xF0000 bands, one per chunk, 0x870000)");
    const uint64_t f1080 = lin_img_fnv(q, 1920, 1080, nullptr);
    CHECK(f1080 == kLin1080Fnv, "4a: 1920x1080: the gfx12 bytes are 0.0.543's (fnv %#llx, want %#llx)", (unsigned long long)f1080,
          (unsigned long long)kLin1080Fnv);
    // the band limit's new edge, at 1440p's neighbours
    n48_rp_lin_in k4 = in; k4.w = k4.bw = 3840; k4.pitch = 3840; k4.rowBytes = 3840u * 4u; k4.h = k4.bh = 256;
    k4.srcLen = k4.mdLen = k4.bytes = 3840ull * 256ull * 4ull;
    n48_rp_lin_in k5 = k4; k5.w = k5.bw = 5120; k5.pitch = 5120; k5.rowBytes = 5120u * 4u;
    k5.srcLen = k5.mdLen = k5.bytes = 5120ull * 256ull * 4ull;
    CHECK(lin_why(k4) == N48_RP_LIN_OK && lin_why(k5) == N48_RP_LIN_BAND, "4a: 3840 wide (1.875 MiB rows) streams, 5120 wide refuses BAND");
}

// ---- build 0.0.552 ( PLAN (3); switch 109, gfx_bb552.h): THE TABLE'S CAPACITY AND SUPERSEDED-ENTRY EVICTION ----------
//   R1 the 33rd..128th distinct copy is RECORDED under ON (and each answers yes), refused FULL under OFF and SHADOW; the 129th is
//      refused FULL under ON when nothing is epoch-stale; SHADOW counts the FULLs ON would have recorded; OFF == n48_rp_record.
//   R2 eviction under ON takes ONLY an epoch-stale entry (the oldest), never one that can answer; the evicted entry never answers yes
//      again (at its own epoch or the new one); every other entry still answers.
//   R3 a superseded entry (unmapped, or re-copied at the same VA onto new VRAM) is gone under ON at 128 entries and never answers yes
//      afterwards, even through a walk that still lands on its old VRAM.
//   R4 verification is unchanged: every refused copy shape is refused with the SAME reason in all three modes, the table is changed
//      identically (the supersede drop only), nothing evicted, nothing recorded - even with an evictable table.
static const uint64_t kR109Va = 0x400000000ull, kR109Vram = 0x10000000ull;
static int r109_walk(const void *vm, uint64_t va, uint64_t *vram) {   // identity: VA page -> VRAM page, over 256 slots of 64 KiB
    (void)vm;
    if (va < kR109Va || va >= kR109Va + 0x10000ull * 256u) return 0;
    *vram = kR109Vram + ((va - kR109Va) & ~0xfffull);
    return 1;
}
static uint64_t gR109OldVram = 0ull;   // R3: the OLD backing of a re-copied VA
static n48_rp_copy r109_copy(uint32_t i, uint32_t epoch) {
    n48_rp_copy c = good_copy(); c.va = kR109Va + 0x10000ull * i; c.vram = kR109Vram + 0x10000ull * i; c.epoch = epoch; return c;
}
static int r109_ask(n48_rp *t, uint32_t i, uint32_t epoch, n48_rp_walk w = &r109_walk) {
    static const int vm = 1;
    return n48_rp_ok(t, 6, kR109Va + 0x10000ull * i, N48_RP_G12_4KB_2D, kElemBytes, 2, epoch, w, &vm);
}
static uint32_t r109_count_va(const n48_rp *t, uint64_t va) { uint32_t n = 0; for (uint32_t k = 0; k < t->n; k++) n += t->e[k].va == va; return n; }
static void r109_fill(n48_rp *t, uint32_t on, uint32_t upto, uint32_t epoch) {
    for (uint32_t i = 0; i < upto; i++) { const n48_rp_copy c = r109_copy(i, epoch); (void)n48_rp_record_x(t, &c, on, epoch); }
}
static void test_rp109() {
    static n48_rp t, u, v;
    CHECK(N48_RP_CAP_OFF == 32u && N48_RP_CAP_ON == 128u && N48_RP_MAX >= N48_RP_CAP_ON && sizeof(n48_rp_ent) == 64u,
          "R0 capacities 32 / 128, storage %u >= 128, entry 64 bytes (the table is 8 KiB)", N48_RP_MAX);
    // R1: OFF / SHADOW / ON over 130 distinct copies
    for (uint32_t on = 0; on < 3u; on++) {
        t = n48_rp {};
        uint32_t ok = 0, full = 0, fullFrom = 0;
        for (uint32_t i = 0; i < 130u; i++) {
            const n48_rp_copy c = r109_copy(i, 7);
            const uint32_t why = n48_rp_record_x(&t, &c, on, 7);
            if (why == N48_RP_REC_OK) ok++; else if (why == N48_RP_REC_FULL) { if (!full) fullFrom = i; full++; }
        }
        const uint32_t cap = on == 1u ? 128u : 32u;
        CHECK(ok == cap && full == 130u - cap && fullFrom == cap && t.n == cap && t.refused[N48_RP_REC_FULL] == 130u - cap &&
              t.evicted == 0u, "R1 mode %u: %u recorded, the %uth.. refused FULL (%u), nothing evicted (ok %u full %u n %u)",
              on, cap, cap + 1u, 130u - cap, ok, full, t.n);
        uint32_t yes = 0; for (uint32_t i = 0; i < 130u; i++) yes += r109_ask(&t, i, 7) == 1;
        CHECK(yes == cap && r109_ask(&t, cap - 1u, 7) == 1 && (cap == 128u || r109_ask(&t, 32u, 7) == 0),
              "R1 mode %u: exactly the %u recorded answer yes (%u)", on, cap, yes);
        CHECK(t.fullOnWould == (on == 2u ? 98u : 0u), "R1 mode %u: SHADOW counts the 98 FULLs ON would have recorded (%llu)", on,
              (unsigned long long)t.fullOnWould);
    }
    { t = n48_rp {}; u = n48_rp {};   // OFF == n48_rp_record, table and counters, over a sequence with supersedes and FULLs
      for (uint32_t i = 0; i < 60u; i++) { n48_rp_copy c = r109_copy((i * 7u) % 45u, 7); if (i % 11u == 3u) c.mismatched = 1;
                                          (void)n48_rp_record(&t, &c); (void)n48_rp_record_x(&u, &c, 0u, 9u); }
      CHECK(!std::memcmp(&t, &u, sizeof t), "R1 OFF: n48_rp_record_x(.., 0, any epoch) is n48_rp_record byte for byte (table and counters)"); }
    { t = n48_rp {}; u = n48_rp {};   // SHADOW == OFF but for fullOnWould
      for (uint32_t i = 0; i < 60u; i++) { n48_rp_copy c = r109_copy((i * 7u) % 45u, 7);
                                          (void)n48_rp_record_x(&t, &c, 0u, 7u); (void)n48_rp_record_x(&u, &c, 2u, 7u); }
      const uint64_t w = u.fullOnWould; u.fullOnWould = 0u;
      CHECK(!std::memcmp(&t, &u, sizeof t) && w > 0u, "R1 SHADOW: OFF's table and counters exactly, plus fullOnWould %llu", (unsigned long long)w); }
    // R2: eviction takes only an epoch-stale entry, the oldest
    { t = n48_rp {}; r109_fill(&t, 1u, 128u, 7u);
      t.e[5].epoch = 6u; t.e[90].epoch = 5u;   // two entries whose epoch has moved on (recorded before the last two bumps)
      const uint64_t va5 = t.e[5].va, va90 = t.e[90].va;
      uint32_t es = 0, as = 0, oc = 0; n48_rp_census(&t, 7u, 2u, 6u, &es, &as, &oc);
      CHECK(es == 2u && as == 0u && oc == 0u && n48_rp_evict_pick(&t, 7u) == 5u && n48_rp_on_would_fit(&t, 7u) == 1u,
            "R2 census: 2 epoch-stale, 0 arm-stale, 0 other-ctx; the pick is the OLDEST stale (index 5)");
      CHECK(r109_ask(&t, 5, 7) == 0 && r109_ask(&t, 5, 8) == 0 && r109_ask(&t, 90, 7) == 0,
            "R2 an entry whose epoch has moved on answers no at the current epoch and every later one, before any eviction");
      const n48_rp_copy c = r109_copy(200, 7);
      CHECK(n48_rp_record_x(&t, &c, 1u, 7u) == N48_RP_REC_OK && t.n == 128u && t.evicted == 1u && r109_count_va(&t, va5) == 0u &&
            r109_count_va(&t, va90) == 1u, "R2 ON at 128: the 129th copy is recorded by evicting the stale entry 5 (only it)");
      uint32_t yes = 0; for (uint32_t i = 0; i < 128u; i++) if (i != 5u && i != 90u) yes += r109_ask(&t, i, 7) == 1;
      CHECK(yes == 126u && r109_ask(&t, 200, 7) == 1, "R2 every entry that could answer still answers (126 + the new one)");
      CHECK(r109_ask(&t, 5, 6) == 0 && r109_ask(&t, 5, 7) == 0 && r109_ask(&t, 5, 8) == 0 && r109_count_va(&t, va5) == 0u,
            "R2 the evicted entry never answers yes afterwards, at any epoch - its own (6) included");
      const n48_rp_copy c2 = r109_copy(201, 7);
      CHECK(n48_rp_record_x(&t, &c2, 1u, 7u) == N48_RP_REC_OK && t.evicted == 2u && r109_count_va(&t, va90) == 0u,
            "R2 the next copy evicts the other stale entry (90)");
      const n48_rp_copy c3 = r109_copy(202, 7);
      CHECK(n48_rp_record_x(&t, &c3, 1u, 7u) == N48_RP_REC_FULL && t.evicted == 2u && t.n == 128u && r109_ask(&t, 202, 7) == 0,
            "R2 with no stale entry left, ON refuses FULL (a live entry is never evicted)");
      // a whole-table epoch move: every entry evictable; OFF evicts nothing, SHADOW counts, ON evicts one per record
      u = t; v = t;
      const n48_rp_copy c4 = r109_copy(203, 8);
      CHECK(n48_rp_record_x(&u, &c4, 0u, 8u) == N48_RP_REC_FULL && u.evicted == t.evicted && u.fullOnWould == t.fullOnWould,
            "R2 OFF after an epoch move: FULL, nothing evicted");
      CHECK(n48_rp_record_x(&v, &c4, 2u, 8u) == N48_RP_REC_FULL && v.evicted == t.evicted && v.fullOnWould == t.fullOnWould + 1u,
            "R2 SHADOW after an epoch move: FULL, counted as ON would record");
      const uint64_t va0 = t.e[0].va;
      CHECK(n48_rp_record_x(&t, &c4, 1u, 8u) == N48_RP_REC_OK && t.evicted == 3u && r109_count_va(&t, va0) == 0u &&
            r109_ask(&t, 203, 8) == 1, "R2 ON after an epoch move: the oldest entry goes, the new copy answers at epoch 8"); }
    // R3: superseded entries under ON at 128 entries
    { t = n48_rp {}; r109_fill(&t, 1u, 128u, 7u);
      n48_rp_unmap_rng(&t, 6, kR109Va + 0x10000ull * 40u, 0x2000ull);
      CHECK(t.n == 127u && r109_count_va(&t, kR109Va + 0x10000ull * 40u) == 0u && r109_ask(&t, 40, 7) == 0,
            "R3 ON: an unmap of entry 40's range drops it; it never answers yes afterwards (walk still resolves)");
      const n48_rp_copy c = r109_copy(129, 7);
      CHECK(n48_rp_record_x(&t, &c, 1u, 7u) == N48_RP_REC_OK && t.evicted == 0u && t.n == 128u,
            "R3 ... and its slot takes a new copy with no eviction");
      // a re-copy of entry 41's VA onto NEW VRAM: the old entry goes, the new one answers; a walk onto the OLD VRAM answers no
      n48_rp_copy r = r109_copy(41, 7); r.vram = 0x30000000ull;
      const uint64_t oldVram = t.e[40].vram;   // index 40 is slot 41 now (slot 40 was dropped)
      CHECK(t.e[40].va == kR109Va + 0x10000ull * 41u, "R3 (setup) index 40 holds slot 41");
      CHECK(n48_rp_record_x(&t, &r, 1u, 7u) == N48_RP_REC_OK && r109_count_va(&t, r.va) == 1u,
            "R3 ON: a re-copy of slot 41 onto new VRAM leaves exactly one entry for that VA");
      static const int vm = 1;
      gR109OldVram = oldVram;
      const int oldYes = n48_rp_ok(&t, 6, r.va, N48_RP_G12_4KB_2D, kElemBytes, 2, 7,
                                   [](const void *, uint64_t va, uint64_t *vr) -> int { *vr = gR109OldVram + (va - (kR109Va + 0x10000ull * 41u));
                                       return 1; }, &vm);
      const int newYes = n48_rp_ok(&t, 6, r.va, N48_RP_G12_4KB_2D, kElemBytes, 2, 7,
                                   [](const void *, uint64_t va, uint64_t *vr) -> int { *vr = 0x30000000ull + (va - (kR109Va + 0x10000ull * 41u)); return 1; }, &vm);
      CHECK(oldYes == 0 && newYes == 1, "R3 ... the superseded entry never answers yes through its OLD backing (%d); the new one does (%d)",
            oldYes, newYes); }
    // R4: verification unchanged in every mode, with an evictable table
    { struct Bad { const char *what; uint32_t why; } bads[] = {
          { "not copied", N48_RP_REC_NOT_COPIED }, { "mismatched", N48_RP_REC_UNVERIFIED }, { "short read-back", N48_RP_REC_UNVERIFIED },
          { "zero bytes", N48_RP_REC_UNVERIFIED }, { "not WindowServer's", N48_RP_REC_NOT_WS }, { "no context", N48_RP_REC_NO_CTX },
          { "no VA", N48_RP_REC_NO_VA }, { "split VRAM", N48_RP_REC_SPLIT }, { "linear mode", N48_RP_REC_MODE },
          { "not re-tiled", N48_RP_REC_NOT_RETILED } };
      for (uint32_t b = 0; b < sizeof bads / sizeof bads[0]; b++) {
          n48_rp_copy c = r109_copy(250, 8);
          switch (b) {
          case 0: c.copied = 0; break;          case 1: c.mismatched = 3; break;   case 2: c.compared = 0x7ff; break;
          case 3: c.bytes = 0; break;           case 4: c.ownerWs = 0; break;      case 5: c.ctx = 0; break;
          case 6: c.vaOk = 0; break;            case 7: c.contiguous = 0; break;   case 8: c.mode = 0; break;
          default: c.retiled = 0; break;
          }
          uint32_t whys[3]; n48_rp tabs[1];
          t = n48_rp {}; r109_fill(&t, 1u, 128u, 7u);   // every entry epoch 7; asked at epoch 8: all evictable
          u = t; v = t; tabs[0] = t;
          whys[0] = n48_rp_record(&tabs[0], &c);
          whys[1] = n48_rp_record_x(&t, &c, 1u, 8u);
          whys[2] = n48_rp_record_x(&u, &c, 2u, 8u);
          const uint32_t w0 = n48_rp_record_x(&v, &c, 0u, 8u);
          CHECK(whys[0] == bads[b].why && whys[1] == bads[b].why && whys[2] == bads[b].why && w0 == bads[b].why &&
                !std::memcmp(&t, &tabs[0], sizeof t) && !std::memcmp(&u, &tabs[0], sizeof u) && !std::memcmp(&v, &tabs[0], sizeof v) &&
                t.evicted == 0u && r109_count_va(&t, c.va) == 0u,
                "R4 %s: refused %u in OFF / ON / SHADOW alike, the table identical to 0.0.551's record, nothing evicted or recorded "
                "(%u %u %u %u)", bads[b].what, bads[b].why, whys[0], whys[1], whys[2], w0);
      } }
}

int main() {
    test_lin_1440p();   // build 0.0.544 item 4a
    test_known493();
    test_lin492();
    test_equations();
    test_equations_new();
    test_retile();
    test_retile_new();
    test_planted_t450_retile();
    test_shape();
    test_shape_kind();
    test_shape_page_rounding();
    test_table();
    test_hole1_supersede();
    test_hole4_carry();
    test_elem_ask_collisions();
    test_unmap_range_scoped();
    test_unmap_boundary_exclusive();
    test_census_dup_not_charged_to_budget();
    test_queue();
    test_clog();
    test_prov_line();
    test_planted_794();
    test_rp109();   // build 0.0.552 (switch 109)
    std::printf("ws_resprov: %d check(s), %d failed\n", gChecks, gFail);
    if (!gFail) std::printf("N48-RESPROV-TEST-PASS\n");
    return gFail ? 1 : 0;
}
