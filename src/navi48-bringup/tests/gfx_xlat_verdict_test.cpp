// gfx_xlat_verdict_test.cpp — the translate-or-neuter decision's order and its tally (designed 0.0.282).
//     clang++ -std=c++17 -Wall -Wextra -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_xlat_verdict_test.cpp -o /tmp/xvtest && /tmp/xvtest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_xlat_verdict.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-72s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-72s %#llx\n", what, (unsigned long long)got);
}

// A frame that translates: one IB of 3744 dwords read and walked in full on host pages, a VS and a PS that are ours, three segments
// accepted, descriptors translated, a host colour target, budget left. Each test breaks one thing (or two, for the order).
static n48_xv_frame good()
{
    n48_xv_frame f {};
    f.armed = 1; f.shape_ok = 1; f.reader_ok = 1; f.budget_left = 4;
    f.nib = 1; f.ib[0].len = 3744; f.ib[0].got = 3744; f.ib[0].walk = 3744; f.ib[0].vram_pages = 0;
    f.npgm = 2;
    f.pgm[0] = { 0x3f629a1c50bce538ull, N48_XV_PGM_KEY_XLAT, 1, 0 };   // plane__vertex (substituted in place, not relocated)
    f.pgm[1] = { 0xa4b3621042a4a4abull, N48_XV_PGM_KEY_XLAT, 1, 0 };   // fca15jbebia3 (substituted in place, not relocated)
    f.nseg = 3;
    return f;
}

int main()
{
    uint64_t key = 0; uint32_t det = 0;
    n48_xv_frame f = good();
    expect_u("good frame -> TRANSLATE", n48_xv_decide(&f, &key, &det), N48_XV_TRANSLATE);
    f = good(); f.armed = 0; f.shape_ok = 0;
    expect_u("not armed wins over everything", n48_xv_decide(&f, &key, &det), N48_XV_ARMED_OFF);
    f = good(); f.shape_ok = 0; f.reader_ok = 0;
    expect_u("shape before reader", n48_xv_decide(&f, &key, &det), N48_XV_SHAPE);
    f = good(); f.reader_ok = 0; f.ib[0].got = 0;
    expect_u("reader before IB reads", n48_xv_decide(&f, &key, &det), N48_XV_NO_READER);
    f = good(); f.nib = 2; f.ib[1].len = 128; f.ib[1].got = 64; f.ib[1].walk = 64;
    expect_u("second IB short", n48_xv_decide(&f, &key, &det), N48_XV_IB_SHORT);
    expect_u("  detail = IB 1", det, 1);
    f = good(); f.ib[0].walk = 1728;
    expect_u("walk shorter than the length", n48_xv_decide(&f, &key, &det), N48_XV_IB_WALK);
    f = good(); f.ib[0].vram_pages = 1;
    expect_u("IB page in VRAM", n48_xv_decide(&f, &key, &det), N48_XV_IB_NOT_HOST);
    f = good(); f.pgm[1].key_class = N48_XV_PGM_KEY_NO_XLAT; f.pgm[0].key_class = N48_XV_PGM_KEY_UNKNOWN; f.pgm[0].key = 0x1234;
    expect_u("an UNKNOWN program is reported before a known untranslatable one", n48_xv_decide(&f, &key, &det), N48_XV_PGM_UNKNOWN);
    expect_u("  key names it", key, 0x1234);
    f = good(); f.pgm[1].key_class = N48_XV_PGM_KEY_NO_XLAT; f.pgm[0].bytes_are_ours = 0;
    expect_u("no translation before not-substituted", n48_xv_decide(&f, &key, &det), N48_XV_PGM_NO_XLAT);
    expect_u("  key names fca15jbebia3", key, 0xa4b3621042a4a4abull);
    f = good(); f.pgm[0].bytes_are_ours = 0; f.seg_status[0] = 6;
    expect_u("Apple's bytes still at the VA: never translate the IB around them", n48_xv_decide(&f, &key, &det), N48_XV_PGM_NOT_OURS);
    f = good(); f.seg_status[2] = 6; f.seg_detail[2] = 0xb004; f.desc_status = 1;
    expect_u("segment policy before descriptors", n48_xv_decide(&f, &key, &det), N48_XV_SEG_POLICY);
    expect_u("  detail = segment 2 | register 0xb004", det, (2u << 16) | 0xb004u);
    f = good(); f.nseg = 0;
    expect_u("no segment at all is a policy refusal, not a pass", n48_xv_decide(&f, &key, &det), N48_XV_SEG_POLICY);
    f = good(); f.desc_status = 5; f.target_vram = 1;
    expect_u("descriptor before target", n48_xv_decide(&f, &key, &det), N48_XV_DESC);
    f = good(); f.target_vram = 1; f.budget_left = 0;
    expect_u("VRAM target before budget", n48_xv_decide(&f, &key, &det), N48_XV_TARGET_VRAM);
    f = good(); f.budget_left = 0;
    expect_u("budget spent", n48_xv_decide(&f, &key, &det), N48_XV_BUDGET);
    // 0.0.387 — THE PLANTED OVERFLOW. Until now this frame answered `program-unknown` with key 0,
    // which is how 285 of arm9's 517 and 282 of arm13's 531 program-unknown frames were read as a shader wall when not
    // one of their programs had been examined. The new reason must fire AND the old one must not.
    f = good(); f.npgm = N48_XV_MAX_PGMS + 1;   // the sentinel the gather sets when it runs out of table
    key = 0xdeadbeefull; det = 0xdeadu;
    expect_u("PLANTED OVERFLOW: more programs than the table holds -> program-table-overflow",
             n48_xv_decide(&f, &key, &det), N48_XV_PGM_TABLE_OVERFLOW);
    expect_u("PLANTED OVERFLOW: it is NOT program-unknown",
             n48_xv_decide(&f, &key, &det) == N48_XV_PGM_UNKNOWN ? 1u : 0u, 0u);
    expect_u("PLANTED OVERFLOW: no key is invented (none was read)", key, 0);
    expect_u("PLANTED OVERFLOW: no detail is invented", det, 0);
    expect_u("PLANTED OVERFLOW: its name is its own", n48_xv_reason_name(N48_XV_PGM_TABLE_OVERFLOW)[0], 'p');
    {
        const char *nm = n48_xv_reason_name(N48_XV_PGM_TABLE_OVERFLOW);
        const char *un = n48_xv_reason_name(N48_XV_PGM_UNKNOWN);
        expect_u("PLANTED OVERFLOW: the name differs from program-unknown's", std::strcmp(nm, un) != 0 ? 1u : 0u, 1u);
        expect_u("PLANTED OVERFLOW: the name is exactly `program-table-overflow`",
                 std::strcmp(nm, "program-table-overflow") == 0 ? 1u : 0u, 1u);
    }
    // The overflow rung must come BEFORE the per-program rungs, and must not be reachable from a frame that fits: a
    // frame with exactly MAX programs, ALL of them unknown, is a genuine `program-unknown` and must stay one.
    f = good(); f.npgm = N48_XV_MAX_PGMS;
    for (uint32_t k = 0; k < N48_XV_MAX_PGMS; k++) { f.pgm[k].key_class = N48_XV_PGM_KEY_UNKNOWN; f.pgm[k].key = 0x900u + k; }
    expect_u("a FULL but not overflowing table is still program-unknown", n48_xv_decide(&f, &key, &det), N48_XV_PGM_UNKNOWN);
    expect_u("  and it names the first unknown program's key", key, 0x900u);
    // ...and the boundary itself: MAX programs fit, MAX + 1 does not.
    f = good(); f.npgm = N48_XV_MAX_PGMS;
    for (uint32_t k = 0; k < N48_XV_MAX_PGMS; k++) f.pgm[k] = { 0x111u + k, N48_XV_PGM_KEY_XLAT, 1, 0 };
    expect_u("MAX programs, all ours: the table is not overflowing and the frame translates",
             n48_xv_decide(&f, &key, &det), N48_XV_TRANSLATE);
    f.npgm = N48_XV_MAX_PGMS + 1;
    expect_u("one more than MAX: program-table-overflow, even with every stored program ours",
             n48_xv_decide(&f, &key, &det), N48_XV_PGM_TABLE_OVERFLOW);
    // The bound itself, so a later edit that shrinks it back is a test failure and not a silent regression.
    expect_u("the program table holds 64 (0.0.387; measured max 33 per frame over arm9 and arm13)", N48_XV_MAX_PGMS, 64);
    expect_u("the program table is bounded by what one frame's IBs can name (32 per IB x 4 IBs)",
             N48_XV_MAX_PGMS <= 32u * N48_XV_MAX_IBS ? 1u : 0u, 1u);

    f = good(); f.npgm = 0;
    expect_u("an IB with no program still needs its segments accepted (draw-free frames translate)", n48_xv_decide(&f, &key, &det), N48_XV_TRANSLATE);

    static n48_xv_tally t {};
    n48_xv_count(&t, 1058, N48_XV_PGM_NO_XLAT, 0xa4b3621042a4a4abull);
    n48_xv_count(&t, 1058, N48_XV_PGM_NO_XLAT, 0xa4b3621042a4a4abull);
    n48_xv_count(&t, 1058, N48_XV_TRANSLATE, 0);
    n48_xv_count(&t, 732, N48_XV_TRANSLATE, 0);
    expect_u("tally: 3 rows", t.used, 3);
    expect_u("tally: repeated (pid, verdict, key) accumulates", t.row[0].frames, 2);
    expect_u("tally: by verdict TRANSLATE 2", t.by_verdict[N48_XV_TRANSLATE], 2);
    for (uint32_t k = 0; k < N48_XV_TALLY_ROWS + 5; k++) n48_xv_count(&t, 1, N48_XV_PGM_UNKNOWN, 0x1000u + k);
    expect_u("tally: full table", t.used, N48_XV_TALLY_ROWS);
    expect_u("tally: overflow counted (3 + 69 - 64 = 8)", t.overflow, 8);
    expect_u("tally: by verdict still complete", t.by_verdict[N48_XV_PGM_UNKNOWN], N48_XV_TALLY_ROWS + 5);
    for (uint32_t r = 0; r < N48_XV_REASONS; r++)
        if (n48_xv_reason_name(r)[0] == '?') expect_u("every reason has a name", r, 0xffff);
    expect_u("reason names: 15 (0.0.387 adds program-table-overflow)", N48_XV_REASONS, 15);

    //: THE `descriptor` RUNG IS REFUSE-ONLY. The kext now assigns desc_status (it never did before: the
    // rung was vacuous), so the property that makes that safe to ship ON is checked exhaustively here: over a lattice of 12288
    // frames covering every earlier rung's outcomes, making desc_status non-zero (a) never yields TRANSLATE, and (b) changes the
    // verdict only from TRANSLATE / target-in-vram / budget to `descriptor` - never from any other refusal, never to anything else.
    // The planted defect is the vacuous rung itself (desc_status ignored, as 0.0.355 behaved): it must violate (a).
    {
        uint64_t frames = 0, moved = 0, badTranslate = 0, badMove = 0, mutantTranslate = 0;
        const uint32_t descs[3] = { 1u, 0x13027u, 0x1302du };
        for (uint32_t m = 0; m < (1u << 11) * 3u * 2u; m++) {
            n48_xv_frame g = good();
            uint32_t b = m;
            g.armed = b & 1u; b >>= 1; g.shape_ok = b & 1u; b >>= 1; g.reader_ok = b & 1u; b >>= 1;
            if (b & 1u) g.ib[0].got = 100; b >>= 1;
            if (b & 1u) g.ib[0].walk = 100; b >>= 1;
            g.ib[0].vram_pages = b & 1u; b >>= 1;
            g.pgm[1].bytes_are_ours = b & 1u; b >>= 1;
            if (!(b & 1u)) g.nseg = 0; b >>= 1;
            if (b & 1u) { g.seg_status[1] = 6; g.seg_detail[1] = 0xb004; } b >>= 1;
            g.target_vram = b & 1u; b >>= 1;
            g.budget_left = (b & 1u) ? 4u : 0u; b >>= 1;
            const uint32_t pc = b % 3u; b /= 3u;
            g.pgm[0].key_class = pc == 0u ? N48_XV_PGM_KEY_XLAT : pc == 1u ? N48_XV_PGM_KEY_UNKNOWN : N48_XV_PGM_KEY_NO_XLAT;
            g.pgm[0].relocated = b & 1u;
            g.desc_status = 0;
            uint64_t k0 = 0; uint32_t d0 = 0;
            const uint32_t v0 = n48_xv_decide(&g, &k0, &d0);
            for (uint32_t q = 0; q < 3u; q++) {
                n48_xv_frame h = g;
                h.desc_status = descs[q];
                uint64_t k1 = 0; uint32_t d1 = 0;
                const uint32_t v1 = n48_xv_decide(&h, &k1, &d1);
                frames++;
                if (v1 == N48_XV_TRANSLATE) badTranslate++;
                if (v1 != v0) {
                    moved++;
                    if (!(v1 == N48_XV_DESC && (v0 == N48_XV_TRANSLATE || v0 == N48_XV_TARGET_VRAM || v0 == N48_XV_BUDGET))) badMove++;
                }
                n48_xv_frame mu = h;          // the mutant: the rung as 0.0.355 had it - desc_status never looked at
                mu.desc_status = 0;
                uint64_t k2 = 0; uint32_t d2 = 0;
                if (n48_xv_decide(&mu, &k2, &d2) == N48_XV_TRANSLATE) mutantTranslate++;
            }
        }
        std::printf("descriptor rung property: %llu frame(s), %llu verdict(s) moved, %llu moved wrongly, %llu translated with a "
                    "descriptor-reading program; the vacuous-rung mutant translates %llu of them\n", (unsigned long long)frames,
                    (unsigned long long)moved, (unsigned long long)badMove, (unsigned long long)badTranslate,
                    (unsigned long long)mutantTranslate);
        expect_u("desc_status non-zero never yields TRANSLATE", badTranslate, 0);
        expect_u("desc_status only moves TRANSLATE/target-in-vram/budget to descriptor", badMove, 0);
        expect_u("some verdicts DO move (the property is not vacuous)", moved > 0u ? 1u : 0u, 1);
        expect_u("PLANTED DEFECT: the vacuous rung translates descriptor-reading frames (caught)", mutantTranslate > 0u ? 1u : 0u, 1);
    }
    // build 0.0.474 item 3 (10B-COVERAGE.md Q4 item 4 contract (3)) — every unknown program key the gather
    // saw for a frame, not only n48_xv_decide's own first one. REACHABILITY: this drives the REAL n48_xv_decide
    // over a frame with several unknown keys (so the verdict itself, unmodified, names only the FIRST one) and
    // then n48_xv_unknown_keys over the SAME frame, and checks it finds every one of them.
    {
        n48_xv_frame f = good();
        f.npgm = 10;
        for (uint32_t k = 0; k < 10u; k++) {
            f.pgm[k].key = 0x1000ull + k;
            f.pgm[k].key_class = (k % 3u == 0u) ? N48_XV_PGM_KEY_UNKNOWN : N48_XV_PGM_KEY_XLAT;
            f.pgm[k].bytes_are_ours = 1;
        }
        // unknown at k = 0, 3, 6, 9 -> 4 unknown keys total, first is 0x1000.
        uint64_t key = 0; uint32_t det = 0;
        const uint32_t v = n48_xv_decide(&f, &key, &det);
        expect_u("U1 the real verdict is program-unknown", v, N48_XV_PGM_UNKNOWN);
        expect_u("U2 ... naming only the FIRST unknown key (0x1000)", key, 0x1000ull);
        uint64_t keys[N48_XV_UNKNOWN_CAP] = {0}; uint32_t total = 0;
        const uint32_t shown = n48_xv_unknown_keys(&f, keys, N48_XV_UNKNOWN_CAP, &total);
        expect_u("U3 n48_xv_unknown_keys finds all 4, not only the first", total, 4);
        expect_u("U4 ... and shows all 4 (under the cap of 8)", shown, 4);
        expect_u("U5 ... key[0] == 0x1000", keys[0], 0x1000ull);
        expect_u("U6 ... key[1] == 0x1003", keys[1], 0x1003ull);
        expect_u("U7 ... key[2] == 0x1006", keys[2], 0x1006ull);
        expect_u("U8 ... key[3] == 0x1009", keys[3], 0x1009ull);
        // the cap: more unknown keys than N48_XV_UNKNOWN_CAP must truncate `shown` but keep `total` honest (rule 72).
        n48_xv_frame g {}; g.armed = 1; g.shape_ok = 1; g.reader_ok = 1; g.nib = 1;
        g.ib[0].len = g.ib[0].got = g.ib[0].walk = 16;
        g.npgm = N48_XV_MAX_PGMS;
        for (uint32_t k = 0; k < N48_XV_MAX_PGMS; k++) { g.pgm[k].key = 0x2000ull + k; g.pgm[k].key_class = N48_XV_PGM_KEY_UNKNOWN; }
        uint64_t keys2[N48_XV_UNKNOWN_CAP] = {0}; uint32_t total2 = 0;
        const uint32_t shown2 = n48_xv_unknown_keys(&g, keys2, N48_XV_UNKNOWN_CAP, &total2);
        expect_u("U9 the cap: shown is exactly N48_XV_UNKNOWN_CAP, never more", shown2, N48_XV_UNKNOWN_CAP);
        expect_u("U10 ... while total counts every one of the 64 (never silent)", total2, N48_XV_MAX_PGMS);
        // an empty frame: no crash, 0/0.
        n48_xv_frame e {};
        uint64_t keys3[N48_XV_UNKNOWN_CAP] = {0}; uint32_t total3 = 5;   // poisoned, must be overwritten to 0
        const uint32_t shown3 = n48_xv_unknown_keys(&e, keys3, N48_XV_UNKNOWN_CAP, &total3);
        expect_u("U11 an empty frame -> shown 0", shown3, 0);
        expect_u("U12 ... total 0", total3, 0);
        // the widest line, under the same N48_LOG_CAP_BODY (491) f3_reader.h's kext-wide bound uses.
        char b[1024];
        int n = std::snprintf(b, sizeof(b), N48_XV_UNKNOWN_FMT, 0xffffffffffffffffull, 0xffffffffu, 0xffffffffu,
                              0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
                              0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull);
        std::printf("      pgm-unknown-all line worst case: %d bytes (cap 491)\n", n);
        expect_u("U13 the pgm-unknown-all line fits under the 491-byte log cap", n > 0 && (unsigned)n <= 491u, 1);
    }
    std::printf("%d/%d passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
