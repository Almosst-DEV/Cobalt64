// sc_census_test.cpp — the test-A shader census (0.0.268) on synthetic windows.
//
// Build and run on the host Mac:
//     clang -std=c11 -O1 -c src/shadercache/shadercache.c -o /tmp/shadercache.o && \
//     clang++ -std=c++17 -Wall -Wextra -O1 -I src/navi48-bringup/src/apple -I src/shadercache \
//         src/navi48-bringup/tests/sc_census_test.cpp /tmp/shadercache.o -o /tmp/census && /tmp/census
//
// It compiles the SAME header the kext compiles. The cache lookup itself is the library's
// own tested function; here `c` is NULL, so every item reports SC_MISS with reason 0.

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "shadercache.h"
}
#include "sc_census.h"
#include "gfx_subst_pool.h"        // build 0.0.484: K1/K2 and the recognition pool's build (n48_xd_subst_build)
#include "fixture_glass_bd_be.h"   // build 0.0.484: glass BD/BE's real images and two blobs (gen_fixture_glass.py)

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-66s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-66s %#llx\n", what, (unsigned long long)got);
}

static void put32(std::vector<uint8_t> &b, size_t off, uint32_t v)
{
    for (int i = 0; i < 4; i++) b[off + i] = (uint8_t)(v >> (8 * i));
}

struct Seen { std::vector<sc_census_item> items; int stopAfter = 0; };
static int collect(void *ctx, const sc_census_item *it)
{
    Seen *s = static_cast<Seen *>(ctx);
    s->items.push_back(*it);
    return (s->stopAfter && (int)s->items.size() >= s->stopAfter) ? 1 : 0;
}

// =============================================================================================================
// build 0.0.484 (an internal design note Q2 K1/K2 + the substitution cap, Q6 test (c)). The kext's own shader-cache
// library (shadercache.c, the object the kext links) over fixture_glass_bd_be.h's blobs:
//   K1  the residency writer's sc_subst_render into a buffer of kScMaxSubstBytes (N48_SC_MAX_SUBST_BYTES) renders each
//       glass value to its full 5376 bytes - and refuses SC_E_CAPACITY at the OLD 2048 (glass could never be written).
//   K2  the recognition pool's REAL build (n48_xd_subst_build, the loop gfxsrc_xlat_open runs) renders both glass images
//       into N48_XD_SUBST_BYTES rows, byte-identical to what the writer writes - nothing `tooBig`.
//   CAP a blob with 130 substitutable entries against the 128-entry cap: 128 rendered, exactly 2 DROPPED and counted; the
//       boot census (pool NULL) says the same; and the real r17 blob drops nothing.
// =============================================================================================================
static void glass_apple(uint32_t seed, uint32_t L, std::vector<uint8_t> &out)
{
    out.assign((size_t)L * 4u, 0);
    for (uint32_t i = 0; i + 1u < L; i++) put32(out, (size_t)i * 4u, n48_glass_apple_word(seed, i));
    put32(out, (size_t)(L - 1u) * 4u, SC_TERMINATOR);
}
static void test_glass_capacity()
{
    sc_cache c {};
    const uint8_t *blob = reinterpret_cast<const uint8_t *>(kGlassBlobW);
    expect_u("G K1 the glass fixture blob opens (SC_OK)", (uint64_t)(int64_t)sc_open(&c, blob, N48_GLASS_BLOB_BYTES), 0);
    expect_u("G K1 kScMaxSubstBytes (N48_SC_MAX_SUBST_BYTES) >= glass's 5376-byte allocation", N48_SC_MAX_SUBST_BYTES >= N48_GLASS_CAPACITY, 1);
    static uint8_t out[N48_SC_MAX_SUBST_BYTES];
    static uint8_t old2048[2048];
    const struct { const char *nm; uint32_t seed, L; const uint32_t *img; } g[2] = {
        { "BD", N48_GLASS_BD_SEED, N48_GLASS_BD_APPLE_L, kGlassBdImage }, { "BE", N48_GLASS_BE_SEED, N48_GLASS_BE_APPLE_L, kGlassBeImage } };
    for (uint32_t k = 0; k < 2u; k++) {
        std::vector<uint8_t> apple; glass_apple(g[k].seed, g[k].L, apple);
        sc_match m {};
        const int lk = sc_lookup(&c, apple.data(), apple.size(), &m);
        char what[160];
        std::snprintf(what, sizeof what, "G K1 %s: Apple's L%u program is found and VERIFIED (a SUBSTITUTE entry, capacity 5376)", g[k].nm, g[k].L);
        expect_u(what, lk == SC_OK && m.verified == 1u && (m.flags & SC_F_SUBSTITUTE) && m.capacity_bytes == N48_GLASS_CAPACITY, 1);
        uint32_t nb = 0;
        const int rs = sc_subst_render(&c, &m, 0, out, sizeof out, &nb);
        std::snprintf(what, sizeof what, "G K1 %s: sc_subst_render into kScMaxSubstBytes renders the full 5376 bytes, = the fixture image", g[k].nm);
        expect_u(what, rs == SC_OK && nb == N48_GLASS_CAPACITY && !std::memcmp(out, g[k].img, N48_GLASS_CAPACITY), 1);
        nb = 0;
        std::snprintf(what, sizeof what, "G K1 %s: at the OLD 2048-byte gScOut the write REFUSES SC_E_CAPACITY (why glass never landed)", g[k].nm);
        expect_u(what, sc_subst_render(&c, &m, 0, old2048, sizeof old2048, &nb) == SC_E_CAPACITY && nb == 0u, 1);
        std::snprintf(what, sizeof what, "G K1 %s: one dword less than 5376 refuses too (the bound is exact, not slack)", g[k].nm);
        expect_u(what, sc_subst_render(&c, &m, 0, out, N48_GLASS_CAPACITY - 4u, &nb) == SC_E_CAPACITY, 1);
    }
    // K2: the recognition pool's REAL build over the same blob
    static uint8_t pool[(size_t)N48_XD_SUBST_MAX * N48_XD_SUBST_BYTES];
    static n48_xd_subst_row rows[N48_XD_SUBST_MAX];
    n48_xd_subst_counts k {};
    const uint32_t n = n48_xd_subst_build(&c, pool, rows, N48_XD_SUBST_MAX, &k);
    expect_u("G K2 the pool build renders BOTH glass images (2 rows, 0 over the row, 0 refused, 0 dropped)",
             n == 2u && k.substitutable == 2u && k.tooBig == 0u && k.refused == 0u && k.dropped == 0u, 1);
    int same = n == 2u;
    for (uint32_t i = 0; i < n && i < 2u; i++) {
        const uint32_t *img = nullptr;
        for (uint32_t j = 0; j < 2u; j++) {
            std::vector<uint8_t> apple; glass_apple(g[j].seed, g[j].L, apple);
            if (rows[i].key == sc_key(apple.data(), g[j].L, nullptr, 0)) img = g[j].img;
        }
        same = same && img && rows[i].bytes == N48_GLASS_CAPACITY && rows[i].stage == SC_STAGE_FRAGMENT &&
               !std::memcmp(pool + (size_t)i * N48_XD_SUBST_BYTES, img, N48_GLASS_CAPACITY);
    }
    expect_u("G K2 each pool row holds its entry's 5376-byte image, byte-identical to what the writer (K1) writes", same, 1);
    uint64_t key = 0;
    std::vector<uint8_t> appleBd; glass_apple(N48_GLASS_BD_SEED, N48_GLASS_BD_APPLE_L, appleBd);
    expect_u("G K2 and n48_xd_ours_find over that pool recognises BD's image read back at the full K3 window, by BD's key",
             n48_xd_ours_find(kGlassBdImage, N48_XD_PGM_DWORDS, pool, rows, n, &key) == 1 &&
             key == sc_key(appleBd.data(), N48_GLASS_BD_APPLE_L, nullptr, 0), 1);
    // K2 at the OLD 2048-byte row: the build counts both as over the row and renders nothing (the program-unknown cause)
    static uint8_t oldRow[2048];
    uint32_t nbOld = 0;
    sc_match m0 {};
    expect_u("G K2 at the OLD 2048-byte row sc_subst_image refuses SC_E_CAPACITY (tooBig)",
             sc_entry_at(&c, 0, &m0) == SC_OK && sc_subst_image(&c, &m0, oldRow, sizeof oldRow, &nbOld) == SC_E_CAPACITY, 1);
}
static void test_subst_cap()
{
    sc_cache c {};
    const uint8_t *blob = reinterpret_cast<const uint8_t *>(kCapBlobW);
    expect_u("CAP the cap fixture blob opens (SC_OK)", (uint64_t)(int64_t)sc_open(&c, blob, N48_CAP_BLOB_BYTES), 0);
    expect_u("CAP the recognition cap is 128 (N48_XD_SUBST_MAX)", N48_XD_SUBST_MAX, 128);
    static uint8_t pool[(size_t)N48_XD_SUBST_MAX * N48_XD_SUBST_BYTES];
    static n48_xd_subst_row rows[N48_XD_SUBST_MAX];
    n48_xd_subst_counts k {};
    const uint32_t n = n48_xd_subst_build(&c, pool, rows, N48_XD_SUBST_MAX, &k);
    expect_u("CAP 130 substitutable entries (and 2 known-only, not counted)", k.substitutable, N48_CAPBLOB_SUBST);
    expect_u("CAP exactly the cap is rendered", n, N48_XD_SUBST_MAX);
    expect_u("CAP the 2 entries past the cap are COUNTED as dropped", k.dropped, N48_CAPBLOB_SUBST - N48_XD_SUBST_MAX);
    expect_u("CAP nothing over the row, nothing refused", k.tooBig + k.refused, 0);
    n48_xd_subst_counts kc {};
    const uint32_t nc = n48_xd_subst_build(&c, nullptr, nullptr, N48_XD_SUBST_MAX, &kc);
    expect_u("CAP the boot census (no pool) sees the same: 128 fit, 2 dropped, 130 substitutable",
             nc == N48_XD_SUBST_MAX && kc.dropped == 2u && kc.substitutable == N48_CAPBLOB_SUBST, 1);
    n48_xd_subst_counts k96 {};
    (void)n48_xd_subst_build(&c, pool, rows, 96u, &k96);
    expect_u("CAP at the OLD cap of 96 the same blob drops 34 - the count scales with the cap, it is not a constant", k96.dropped, 34);
    // the REAL r17 blob the kext embeds today (suites.sh runs from the tree root): nothing dropped at 128
    std::FILE *f = std::fopen("re/cache/m4c-r17/cache.bin", "rb");
    expect_u("CAP re/cache/m4c-r17/cache.bin opens (run from the tree root)", f != nullptr, 1);
    if (!f) return;
    std::vector<uint8_t> r17(256u * 1024u);
    const size_t len = std::fread(r17.data(), 1, r17.size(), f);
    std::fclose(f);
    sc_cache c17 {};
    expect_u("CAP r17 opens (SC_OK)", (uint64_t)(int64_t)sc_open(&c17, r17.data(), len), 0);
    n48_xd_subst_counts k17 {};
    const uint32_t n17 = n48_xd_subst_build(&c17, pool, rows, N48_XD_SUBST_MAX, &k17);
    std::printf("      r17: %u substitutable, %u rendered, %u over the row, %u refused, %u dropped\n", k17.substitutable, n17,
                k17.tooBig, k17.refused, k17.dropped);
    expect_u("CAP r17: 90 substitutable, all 90 rendered at 5376-byte rows, 0 dropped",
             k17.substitutable == 90u && n17 == 90u && k17.tooBig == 0u && k17.refused == 0u && k17.dropped == 0u, 1);
}

int main()
{
    const uint32_t grid = 0x100;
    std::vector<uint8_t> w(0x800, 0);
    // program A at 0x000: 3 dwords + terminator = 4 dwords
    put32(w, 0x000, 0x11111111); put32(w, 0x004, 0x22222222); put32(w, 0x008, 0x33333333);
    put32(w, 0x00c, SC_TERMINATOR);
    // 0x100: zero first dword -> empty
    // program B at 0x200: 1 dword + terminator = 2 dwords
    put32(w, 0x200, 0xdeadbeef); put32(w, 0x204, SC_TERMINATOR);
    // 0x300: non-zero, no terminator before the end of the window... except program C at 0x400
    put32(w, 0x300, 0x44444444);
    // program C at 0x400 is ALSO reached by the 0x300 start (its terminator is the first one after 0x300)
    put32(w, 0x400, 0x55555555); put32(w, 0x404, SC_TERMINATOR);
    // 0x700: non-zero, and nothing after it - unterminated
    put32(w, 0x700, 0x66666666);

    Seen s; sc_census_counts n{};
    sc_census_window(nullptr, w.data(), w.size(), w.size(), grid, 0x1000, 0x10000, collect, &s, &n);
    expect_u("grid starts visited (0x800 / 0x100)", n.starts, 8);
    expect_u("empty starts (0x100 0x500 0x600)", n.empty, 3);
    expect_u("unterminated starts (0x700)", n.unterminated, 1);
    expect_u("programs reported (0x000 0x200 0x300 0x400)", n.programs, 4);
    expect_u("items collected", s.items.size(), 4);
    if (s.items.size() == 4) {
        expect_u("A offset includes base", s.items[0].offset, 0x10000);
        expect_u("A dwords through the terminator", s.items[0].dwords, 4);
        expect_u("A first dword", s.items[0].first, 0x11111111);
        expect_u("A key == sc_key(code, 4, no mask)", s.items[0].key, sc_key(w.data(), 4, nullptr, 0));
        expect_u("B dwords", s.items[1].dwords, 2);
        expect_u("0x300 start runs to C's terminator: (0x404-0x300)/4+1 dwords", s.items[2].dwords, (0x404 - 0x300) / 4 + 1);
        expect_u("C dwords", s.items[3].dwords, 2);
        expect_u("no cache -> status SC_MISS", (uint64_t)(int64_t)s.items[0].status, (uint64_t)(int64_t)SC_MISS);
        expect_u("distinct code -> distinct keys (A vs B)", s.items[0].key != s.items[1].key, 1);
    }

    // the extent cap: with max_dwords 3, A (4 dwords) is unterminated
    Seen s2; sc_census_counts n2{};
    sc_census_window(nullptr, w.data(), w.size(), w.size(), grid, 3, 0, collect, &s2, &n2);
    expect_u("cap 3 dwords: A becomes unterminated", n2.programs < n.programs, 1);
    expect_u("cap 3 dwords: B (2) still reported first", s2.items.empty() ? 0 : s2.items[0].offset, 0x200);

    // ownership: only starts in [0, owned) are visited, but a program may run into the lookahead
    Seen s3; sc_census_counts n3{};
    sc_census_window(nullptr, w.data(), 0x410, 0x400, grid, 0x1000, 0, collect, &s3, &n3);
    expect_u("owned 0x400 of 0x410: starts visited", n3.starts, 4);
    expect_u("owned 0x400: 0x300's program reaches the terminator in the lookahead", n3.programs, 3);

    // the callback can stop the window
    Seen s4; s4.stopAfter = 2; sc_census_counts n4{};
    sc_census_window(nullptr, w.data(), w.size(), w.size(), grid, 0x1000, 0, collect, &s4, &n4);
    expect_u("callback stop after 2", s4.items.size(), 2);

    // a window shorter than one dword visits nothing
    sc_census_counts n5{};
    sc_census_window(nullptr, w.data(), 3, 3, grid, 0x1000, 0, collect, &s4, &n5);
    expect_u("3-byte window: no starts", n5.starts, 0);

    test_glass_capacity();   // build 0.0.484
    test_subst_cap();

    std::printf("\n%d of %d checks passed%s\n", gRun - gFail, gRun, gFail ? " — FAILURES ABOVE" : "");
    return gFail ? 1 : 0;
}
