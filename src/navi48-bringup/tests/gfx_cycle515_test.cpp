// gfx_cycle515_test.cpp — build 0.0.515: gfx_cycle515.h over run10u's REAL stream, and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_cycle515_test.cpp -o /tmp/cy515 && /tmp/cy515 src/navi48-bringup/src/apple/AppleHardwareHook.cpp
// Covers:
//   T1 the C write-set scan on two real IBs (F105 IB0, a P; F104 IB0, a2's IB0) equals the generator's, and is KNOWN;
//      the UNKNOWN directions (a header that is not PM4, a packet past the body, a LOAD_CONTEXT_REG naming CB_COLOR0_BASE,
//      a LOAD_CONTEXT_REG_INDEX, a set that does not fit);
//   T2 run10u F1..F171 replayed (the gate's own answers, the write sets, P's layer by role): under rule B exactly the P's
//      whose cycle had a refused writer are DIRTY - F125 (a2 F124 write-short) and F141 (b F139 write-short) in the settled
//      window, and every P F17..F97 (the early window) - and the complete cycles F101..F169 are CLEAN; under rule A (the
//      brief's reset: only a committed P) EVERY determined P is DIRTY (the lock-out); the 1040-dword fills with no D draw
//      are UNDETERMINED;
//   T3 THE FLIP-BOOKKEEPING FINDING on the same stream: the plane a refused P leaves on the glass. F125 -> plane 0x401800000
//      last written by F117 (complete); F141 -> 0x403800000 by F129 (complete); every early P -> a plane NO gated P wrote this
//      boot (pre-arm VRAM: older AND incomplete);
//   T4 a post-gate withdrawal dirties the withdrawn frame's write set (and a mismatched seq is only counted); the table's
//      eviction and full paths; an unknown refused writer dirties every layer;
//   T5 Part B: only a committed frame's status-0 segments count, per row; a retry overwrites the first attempt;
//   T6 the report and per-frame lines fit the 491-byte log body at 20-digit counters;
//   T7 the kext glue (AppleHardwareHook.cpp): each call exists once, in the order the frame runs, and every helper returns
//      void and writes no decision (the instrument cannot gate: A2 is not built).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_cycle515.h"
#include "fixture_cycle515_run10u.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-86s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-86s %#llx\n", what, (unsigned long long)got);
}

static const uint64_t kX = 0x400800000ull, kXp = 0x404800000ull, kLut = 0x400240000ull;   // kLut: a placeholder VA

// ---------------------------------------------------------------------------------------------------------------- T1
static void t1_scan()
{
    static n48_cy c; std::memset(&c, 0, sizeof c);
    n48_cy_frame_begin(&c, 1u);
    n48_cy_ws_scan(&c, kCy515F105Ib0, 1040u);
    expect_u("T1 F105 IB0 (P): KNOWN", c.ws_unknown | c.ws_over, 0u);
    expect_u("T1 F105 IB0 (P): the generator's count", c.nws, kCy515F105Ib0Nws);
    uint32_t same = c.nws == kCy515F105Ib0Nws;
    for (uint32_t i = 0; same && i < c.nws; i++) same = c.ws[i] == kCy515F105Ib0Ws[i];
    expect_u("T1 F105 IB0 (P): the generator's set, in order (its plane 0x402800000)", same && c.ws[0] == 0x402800000ull, 1u);
    n48_cy_frame_begin(&c, 1u);
    n48_cy_ws_scan(&c, kCy515F104Ib0, 2448u);
    same = c.nws == kCy515F104Ib0Nws && !c.ws_unknown;
    for (uint32_t i = 0; same && i < c.nws; i++) same = c.ws[i] == kCy515F104Ib0Ws[i];
    expect_u("T1 F104 IB0 (a2): the generator's set, KNOWN, and it names X'", same && c.ws[0] == kXp, 1u);
    // the UNKNOWN directions
    uint32_t b[16];
    n48_cy_frame_begin(&c, 1u); b[0] = 0x12345678u; n48_cy_ws_scan(&c, b, 1u);
    expect_u("T1 a non-PM4 header -> UNKNOWN", c.ws_unknown, 1u);
    n48_cy_frame_begin(&c, 1u); b[0] = 0xC0056900u; b[1] = 0x318u; n48_cy_ws_scan(&c, b, 2u);
    expect_u("T1 a packet past the body -> UNKNOWN", c.ws_unknown, 1u);
    n48_cy_frame_begin(&c, 1u);
    b[0] = 0xC0036100u; b[1] = 0x1000u; b[2] = 0u; b[3] = 0x318u; b[4] = 1u;   // LOAD_CONTEXT_REG naming CB_COLOR0_BASE
    n48_cy_ws_scan(&c, b, 5u);
    expect_u("T1 a LOAD_CONTEXT_REG naming CB_COLOR0_BASE -> UNKNOWN", c.ws_unknown, 1u);
    n48_cy_frame_begin(&c, 1u);
    b[0] = 0xC0036100u; b[1] = 0x00437478u; b[2] = 4u; b[3] = 0u; b[4] = 0u;    // run10t F107's count-0 packet
    n48_cy_ws_scan(&c, b, 5u);
    expect_u("T1 a count-0 LOAD_CONTEXT_REG (names nothing) stays KNOWN", c.ws_unknown, 0u);
    n48_cy_frame_begin(&c, 1u); b[0] = 0xC0009F00u; b[1] = 0u; n48_cy_ws_scan(&c, b, 2u);
    expect_u("T1 a LOAD_CONTEXT_REG_INDEX -> UNKNOWN", c.ws_unknown, 1u);
    n48_cy_frame_begin(&c, 1u);
    for (uint32_t i = 0; i < N48_CY_WS_MAX + 1u; i++) n48_cy_ws_add(&c, 0x500000000ull + ((uint64_t)i << 16));
    expect_u("T1 a set that does not fit -> overflow", c.ws_over, 1u);
    n48_cy_frame_begin(&c, 0u);
    expect_u("T1 no reader -> UNKNOWN from the frame top", c.ws_unknown, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T2/T3
struct Replay {
    uint32_t kind[172], dA[172], dB[172];
    uint64_t byA[172], byB[172], layer[172];
    uint64_t plane[172];            // a P's own colour target
};
static void replay(n48_cy *c, Replay *r)
{
    std::memset(r, 0, sizeof *r);
    for (uint32_t i = 0; i < kCy515Run10uN; i++) {
        const cy515_fx &x = kCy515Run10u[i];
        n48_cy_frame_begin(c, x.captured);
        for (uint32_t w = 0; w < x.nws; w++) n48_cy_ws_add(c, x.ws[w]);
        if (x.unknown) c->ws_unknown = 1u;
        const uint32_t isP = x.nib == 1u && x.len[0] == 1040u;
        if (isP && (x.s4 == 1u || x.s4 == 7u)) {   // the translator's input list of D: {layer tiled, LUT linear} (TEST ASSUMPTION)
            const uint64_t va[2] = { x.s4 == 1u ? kX : kXp, kLut };
            const uint32_t mode[2] = { 3u, 0u };
            n48_cy_note_inputs(c, va, mode, 2u, 0u);
        }
        const n48_cy_judge j = n48_cy_frame(c, x.f, x.committed, isP, 1u, x.committed ? x.f : 0u);
        if (x.f < 172u) {
            r->kind[x.f] = j.kind; r->dA[x.f] = j.dirtyA; r->dB[x.f] = j.dirtyB; r->byA[x.f] = j.byA; r->byB[x.f] = j.byB;
            r->layer[x.f] = j.layer; r->plane[x.f] = isP && x.nws ? x.ws[0] : 0ull;
        }
    }
}
static void t2_run10u()
{
    static n48_cy c; std::memset(&c, 0, sizeof c);
    static Replay r;
    replay(&c, &r);
    const uint32_t dirtyB[] = { 17, 22, 27, 32, 37, 41, 45, 49, 54, 71, 80, 85, 89, 93, 97, 125, 141 };
    const uint32_t cleanB[] = { 101, 105, 109, 113, 117, 121, 129, 133, 137, 145, 149, 153, 157, 161, 165, 169 };
    const uint32_t undet[] = { 1, 3, 6, 8, 14, 23, 28 };
    uint32_t okD = 0, okC = 0, okU = 0, okA = 0;
    for (uint32_t f : dirtyB) okD += (r.kind[f] == N48_CY_DIRTY && r.dB[f]) ? 1u : 0u;
    for (uint32_t f : cleanB) okC += (r.kind[f] != N48_CY_NOTP && r.kind[f] != N48_CY_UNDET && !r.dB[f]) ? 1u : 0u;
    for (uint32_t f : undet) okU += r.kind[f] == N48_CY_UNDET ? 1u : 0u;
    for (uint32_t f : dirtyB) okA += r.dA[f];
    for (uint32_t f : cleanB) okA += r.dA[f];
    expect_u("T2 rule B: the 17 P's with a refused writer in their cycle are DIRTY", okD, 17u);
    expect_u("T2 rule B: the 16 complete cycles F101..F169 are CLEAN", okC, 16u);
    expect_u("T2 the 1040-dword frames with no D draw (F1 3 6 8 14 23 28) are UNDETERMINED", okU, 7u);
    expect_u("T2 rule A (reset only by a committed P): ALL 33 determined P's DIRTY - the lock-out", okA, 33u);
    expect_u("T2 F125 samples X", r.layer[125], kX);
    expect_u("T2 F125 dirtied by F124 (a2, write-short)", r.byB[125], 124u);
    expect_u("T2 F141 samples X", r.layer[141], kX);
    expect_u("T2 F141 dirtied by F139 (b, write-short)", r.byB[141], 139u);
    expect_u("T2 F97 samples X' dirtied by F95 (the producer, not-translate)", (r.layer[97] == kXp) && r.byB[97] == 95u, 1u);
    expect_u("T2 F129 samples X' CLEAN (its cycle F126-F128 committed)", r.layer[129] == kXp && !r.dB[129], 1u);
    expect_u("T2 F133 samples X CLEAN under B (its cycle F130-F132 committed) though F124 dirtied X", r.layer[133] == kX && !r.dB[133], 1u);
    expect_u("T2 no non-P frame is judged", r.kind[124] == N48_CY_NOTP && r.kind[100] == N48_CY_NOTP && r.kind[99] == N48_CY_NOTP, 1u);
    expect_u("T2 counters: P seen 40 = undetermined 7 + determined 33", c.pSeen == 40u && c.pUndet == 7u && c.pCleanB + c.pDirtyB == 33u, 1u);
    expect_u("T2 counters: rule B DIRTY 17, of them committed today 16 (F17 was refused)", c.pDirtyB == 17u && c.pDirtyBCommitted == 16u, 1u);
    expect_u("T2 counters: lines capped at 8", c.lines, 8u);
    expect_u("T2 the table is never full and both layers stay tracked (dirty intermediates are evicted, counted)",
             c.tableFull == 0u && n48_cy_find(&c, kX) && n48_cy_find(&c, kXp) && n48_cy_find(&c, kX)->layer && n48_cy_find(&c, kXp)->layer, 1u);
    // T3: the plane a refused P leaves on the glass (dpg_perform presents the transaction's plane whatever the gate said).
    uint64_t lastWriter[3] = { 0, 0, 0 };
    const uint64_t planes[3] = { 0x401800000ull, 0x402800000ull, 0x403800000ull };
    uint64_t shown125 = ~0ull, shown141 = ~0ull, shownEarlyNone = 0, early = 0;
    for (uint32_t f = 1; f <= 171; f++) {
        if (r.kind[f] != N48_CY_CLEAN && r.kind[f] != N48_CY_DIRTY) continue;
        int pi = -1;
        for (int q = 0; q < 3; q++) if (r.plane[f] == planes[q]) pi = q;
        if (pi < 0) continue;
        const bool wouldCommit = !r.dB[f] && kCy515Run10u[f - 1].committed;
        if (r.dB[f]) {
            if (f == 125) shown125 = lastWriter[pi];
            if (f == 141) shown141 = lastWriter[pi];
            if (f < 101) { early++; if (!lastWriter[pi]) shownEarlyNone++; }
        }
        if (wouldCommit) lastWriter[pi] = f;
    }
    expect_u("T3 F125 refused: plane 0x401800000 shows F117's picture (older, complete)", shown125, 117u);
    expect_u("T3 F141 refused: plane 0x403800000 shows F129's picture (older, complete)", shown141, 129u);
    expect_u("T3 EVERY early refused P (F22..F97 with a plane) shows a plane no gated P wrote: pre-arm VRAM", shownEarlyNone == early && early >= 14u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T4
static void t4_edges()
{
    static n48_cy c; std::memset(&c, 0, sizeof c);
    const uint64_t va[2] = { kX, kLut }; const uint32_t mode[2] = { 3u, 0u };
    // learn X as a layer (a clean P)
    n48_cy_frame_begin(&c, 1u); n48_cy_note_inputs(&c, va, mode, 2u, 0u);
    n48_cy_judge j = n48_cy_frame(&c, 1, 1u, 1u, 1u, 11u);
    expect_u("T4 a first-sight clean P", j.kind == N48_CY_CLEAN && c.pFirstSight == 1u, 1u);
    // a committed writer of X, then WITHDRAWN at the keystone -> X dirty
    n48_cy_frame_begin(&c, 1u); n48_cy_ws_add(&c, kX);
    (void)n48_cy_frame(&c, 2, 1u, 0u, 1u, 12u);
    n48_cy_withdraw(&c, 99u);
    expect_u("T4 a withdrawal naming another seq is only counted", c.withdrawUnmatched == 1u && c.withdrawn == 0u, 1u);
    n48_cy_withdraw(&c, 12u);
    expect_u("T4 the withdrawn frame's writes are refused writes", c.withdrawn, 1u);
    n48_cy_frame_begin(&c, 1u); n48_cy_note_inputs(&c, va, mode, 2u, 0u);
    j = n48_cy_frame(&c, 3, 1u, 1u, 1u, 13u);
    expect_u("T4 ... so the next P over X is DIRTY (by f2)", j.kind == N48_CY_DIRTY && j.byB == 2u, 1u);
    // an unknown refused writer dirties every layer
    n48_cy_frame_begin(&c, 0u);
    (void)n48_cy_frame(&c, 4, 0u, 0u, 1u, 0u);
    n48_cy_frame_begin(&c, 1u); n48_cy_note_inputs(&c, va, mode, 2u, 0u);
    j = n48_cy_frame(&c, 5, 1u, 1u, 1u, 15u);
    expect_u("T4 an UNKNOWN refused writer dirties every layer (by f4)", j.dirtyB && j.byB == 4u && c.unknownRefused == 1u, 1u);
    // the wrong-shaped list is undetermined
    const uint32_t both[2] = { 3u, 3u };
    n48_cy_frame_begin(&c, 1u); n48_cy_note_inputs(&c, va, both, 2u, 0u);
    j = n48_cy_frame(&c, 6, 1u, 1u, 1u, 16u);
    expect_u("T4 two tiled inputs: UNDETERMINED", j.kind, N48_CY_UNDET);
    n48_cy_frame_begin(&c, 1u); n48_cy_note_inputs(&c, va, mode, 2u, 1u);
    expect_u("T4 an overflowed list: UNDETERMINED", n48_cy_frame(&c, 7, 1u, 1u, 1u, 17u).kind, N48_CY_UNDET);
    // eviction: fill the table with refused non-layer writes; the layer survives
    for (uint32_t i = 0; i < 3u * N48_CY_MAX; i++) {
        n48_cy_frame_begin(&c, 1u); n48_cy_ws_add(&c, 0x600000000ull + ((uint64_t)i << 16));
        (void)n48_cy_frame(&c, 100 + i, 0u, 0u, 1u, 0u);
    }
    expect_u("T4 dirty non-layer entries are evicted (counted), never a layer", c.evictDirty > 0u && n48_cy_find(&c, kX) != 0, 1u);
    // DECIDE (not judged): dirt tracked, no P counted
    const uint64_t seen = c.pSeen;
    n48_cy_frame_begin(&c, 1u); n48_cy_note_inputs(&c, va, mode, 2u, 0u);
    (void)n48_cy_frame(&c, 900, 0u, 1u, 0u, 0u);
    expect_u("T4 a P under DECIDE is not counted", c.pSeen, seen);
}

// ---------------------------------------------------------------------------------------------------------------- T5
static void t5_partb()
{
    static n48_de515 d; std::memset(&d, 0, sizeof d);
    static n48_de515_seg s[4]; std::memset(s, 0, sizeof s);
    const uint32_t at0[2] = { 1043u, 1116u }, va0[2] = { 0x4010800u, 0x4010800u }; const uint8_t row0[2] = { 1u, 2u };   // U, Y
    const uint32_t at1[2] = { 1020u, 1060u }, va1[2] = { 0x4014980u, 0x4014980u }; const uint8_t row1[2] = { 3u, 3u };   // AO AO
    const uint32_t at3[1] = { 14525u }, va3[1] = { 0x4023800u }; const uint8_t row3[1] = { 5u };                        // BA
    n48_de515_seg_set(&s[0], 2u, at0, va0, row0);
    n48_de515_seg_set(&s[1], 2u, at1, va1, row1);
    n48_de515_seg_set(&s[3], 1u, at3, va3, row3);
    uint32_t st[4] = { 0u, 0u, 0u, 7u };   // seg 3 refused: its elision must not count
    n48_de515_one one[4]; uint32_t n = 0;
    expect_u("T5 a committed frame: 4 elisions from its status-0 segments", n48_de515_commit(&d, s, st, 4u, one, 4u, &n), 4u);
    expect_u("T5 per row: U 1, Y 1, AO 2, BA 0", d.byRow[1] == 1u && d.byRow[2] == 1u && d.byRow[3] == 2u && d.byRow[5] == 0u, 1u);
    expect_u("T5 the first four handed back in segment order", n == 4u && one[0].at == 1043u && one[2].at == 1020u && one[3].row == 3u, 1u);
    expect_u("T5 the unproven VA (<< 8)", one[0].va, 0x401080000ull);
    // a retry overwrites the first attempt (the deferred unit's first, refused attempt had 2; the retry 1)
    n48_de515_seg_set(&s[1], 1u, at1, va1, row1);
    st[3] = 0u;
    (void)n48_de515_commit(&d, s, st, 4u, one, 4u, &n);
    expect_u("T5 a retry's record replaces the first attempt's (AO +1, not +2); BA counted once seg 3 is 0", d.byRow[3] == 3u && d.byRow[5] == 1u, 1u);
    expect_u("T5 frames with an elision 2, elisions 8", d.frames == 2u && d.elisions == 8u, 1u);
    static n48_de515_seg z[2]; std::memset(z, 0, sizeof z);
    const uint32_t st2[2] = { 0u, 0u };
    expect_u("T5 no elision: not a frame with an elision", (n48_de515_commit(&d, z, st2, 2u, one, 4u, &n), d.frames), 2u);
}

// ---------------------------------------------------------------------------------------------------------------- T6
static void t6_lines()
{
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    int w = std::snprintf(b, sizeof b, N48_CY_FMT, " - `gfxneuter 73 | M << 8` REFUSED: nothing to switch, unchanged",
                          M, M, M, M, M, M, M, M);
    expect_u("T6 cyc515 report line 1 <= 491 bytes at 20-digit counters", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_CY2_FMT, M, M, M, M, M, M, M, 8u);
    expect_u("T6 cyc515 report line 2 <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_CY_LINE_FMT, M, M, M, "DIRTY", M, "DIRTY", M, "refused");
    expect_u("T6 cyc515 P line <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_DE515_FMT, M, M, M, M, M, M, M, 8u);
    expect_u("T6 drawelide515 report line <= 491", w > 0 && w <= 491, 1u);
    const char *shape = "15520|15520|15520|15520";   // 4 IBs of 5 digits: longer than any 47-byte shape the kext can build
    w = std::snprintf(b, sizeof b, N48_DE515_LINE_FMT, M, shape, 4294967295u, 4294967295u, 4294967295u, "BA", M,
                      4294967295u, 4294967295u, "BA", M, 4294967295u, 4294967295u, "BA", M, 4294967295u, 4294967295u, "BA", M);
    expect_u("T6 drawelide515 frame line <= 491", w > 0 && w <= 491, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T7
static std::string slurp(const char *p)
{
    std::string s; FILE *f = p ? std::fopen(p, "rb") : nullptr;
    if (!f) return s;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &h, const char *n)
{
    uint32_t c = 0;
    for (size_t a = h.find(n); a != std::string::npos; a = h.find(n, a + 1)) c++;
    return c;
}
static std::string body_of(const std::string &src, const char *head)
{
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = src.find("\n}\n", a);
    return b == std::string::npos ? std::string() : src.substr(a, b - a);
}
static size_t at(const std::string &s, const char *n) { return s.find(n); }
static void t7_glue(const char *path)
{
    const std::string s = slurp(path);
    expect_u("T7 AppleHardwareHook.cpp read", !s.empty(), 1u);
    const char *cBegin = "    cy515_begin(0u);   // build 0.0.515";
    const char *cKnown = "if (f.reader_ok && n <= f.nib) cy515_known();";
    const char *cIb = "            cy515_ib(dst, got, len, f.ib[k].walk);";
    const char *cCont = "            if (got != len || f.ib[k].walk != len) continue;";
    const char *cPol = "        if (ds.in_abi && !cgRefused) cy515_inputs(ds.in_va, ds.in_mode, ds.in_n, ds.in_over);";
    const char *cDe = "            de515_seg(k, ds.draw_elided, ds.de_at, ds.de_va8, ds.de_row);";
    const char *cAct = "    const uint32_t action = n48_sd_action(arm, shapeOk, verdict, commitOk);\n";
    // build 0.0.545 DELIBERATE re-baseline: the P recogniser is one predicate (gfx_present73.h n48_p73_is_p_shape, computed once
    // per frame into pShape after the IB loop); the value handed to cy515_judge is the same expression (tests/gfx_stale103_test.cpp S22).
    const char *cJudge = "    cy515_judge(gXdC.judged + 1u, commitOk ? 1u : 0u, pShape,\n"
                         "                arm == N48_SD_ARM_COMMIT ? 1u : 0u, gXdCmGateSeq, tgtVa, &f);\n";
    const char *cKs = "        const bool ksOk = frHasPendingEntry && commit_keystone_arm(wf, 1u, gXdCmGateSeq, kr, ks64);\n"
                      "        if (!(tokMatch && ksOk)) cy515_withdraw_queue(here.seq);";
    const char *all[] = { cBegin, cKnown, cIb, cPol, cDe, cJudge, cKs };
    uint32_t once = 0;
    for (const char *x : all) once += count(s, x) == 1u ? 1u : 0u;
    expect_u("T7 each of the seven call sites exists exactly once", once, 7u);
    expect_u("T7 order: frame top -> reader known -> IB scan (before the short-read continue)",
             at(s, cBegin) < at(s, cKnown) && at(s, cKnown) < at(s, cIb) && at(s, cIb) + std::strlen(cIb) < at(s, cCont) &&
             at(s, cCont) - at(s, cIb) < 400u, 1u);   // RE-BASELINED 0.0.546: 200 -> 400 (switch 103's IB-skip flag line sits between them)
    // the policy (gfxsrc_policy) is defined ABOVE gfxsrc_decide_frame and is called from it after the IB loop; the judge runs
    // right after the action the gate's answer made
    const size_t pol = at(s, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp,");
    const size_t dec = at(s, "static uint32_t gfxsrc_decide_frame(");
    expect_u("T7 the input capture and the elision record are inside gfxsrc_policy",
             pol != std::string::npos && dec != std::string::npos && pol < at(s, cPol) && at(s, cPol) < dec &&
             pol < at(s, cDe) && at(s, cDe) < dec, 1u);
    expect_u("T7 the judge is the statement right after the action", at(s, cJudge) != std::string::npos &&
             at(s, cJudge) > at(s, cAct) && at(s, cJudge) - at(s, cAct) < 400u &&
             at(s, cIb) < at(s, cJudge) && at(s, "gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);") < at(s, cJudge), 1u);
    // the helpers return void and write nothing a decision reads
    const char *heads[] = { "static __attribute__((noinline)) void cy515_begin(uint32_t known)",
                            "static __attribute__((noinline)) void cy515_ib(const uint32_t *body, uint32_t got, uint32_t len, uint32_t walk)",
                            "static __attribute__((noinline)) void cy515_inputs(const uint64_t *va, const uint32_t *mode, uint32_t n, uint32_t over)",
                            "static __attribute__((noinline)) void de515_seg(uint32_t k, uint32_t n, const uint32_t *at, const uint32_t *va8, const uint8_t *row)",
                            "static __attribute__((noinline)) void cy515_judge(uint64_t frame, uint32_t committed, uint32_t isP, uint32_t armCommit,",
                            "static void cy515_withdraw_queue(uint32_t seq)", "static __attribute__((noinline)) void cy515_known(void)" };
    uint32_t clean = 0;
    for (const char *h : heads) {
        const std::string b = body_of(s, h);
        const bool ok = !b.empty() && !count(b, "gXdCm") && !count(b, "commitOk") && !count(b, "action") && !count(b, "verdict") &&
                        !count(b, "gXdShot") && !count(b, "return 1") && !count(b, "gXdBuild.seg[k].status =") &&
                        !count(b, "gXdBuild.ok") && !count(b, "f->seg_status") && !count(b, "WREG") && !count(b, "reg_write");
        clean += ok ? 1u : 0u;
    }
    expect_u("T7 the seven helpers are void and write no decision, verdict, gate or register", clean, 7u);
    // every mention of the instrument's state is inside a helper body, the report, or its own declaration line: no decision,
    // gate or verdict reads it (the whole file is searched)
    {
        const char *owners[] = { "static __attribute__((noinline)) void cy515_begin(uint32_t known)",
                                 "static __attribute__((noinline)) void cy515_known(void)",
                                 "static __attribute__((noinline)) void cy515_ib(const uint32_t *body, uint32_t got, uint32_t len, uint32_t walk)",
                                 "static __attribute__((noinline)) void cy515_inputs(const uint64_t *va, const uint32_t *mode, uint32_t n, uint32_t over)",
                                 "static __attribute__((noinline)) void de515_seg(uint32_t k, uint32_t n, const uint32_t *at, const uint32_t *va8, const uint8_t *row)",
                                 "static __attribute__((noinline)) void cy515_judge(uint64_t frame, uint32_t committed, uint32_t isP, uint32_t armCommit,",
                                 "static void cy515_withdraw_queue(uint32_t seq)", "static void cy515_report_line(const char *how)",
                                 "static void de515_report_line(void)",
                                 // build 0.0.552 (switch 110): a read-only accessor for 110's report line (log only).
                                 "static uint64_t de515_an_committed(void)",
                                 // build 0.0.525 (switch 80, notes/design/CYCLE80.md): the ONLY decision readers, by name. Both read
                                 // the write set: c80_heldback to ADD held-back writes (C3's 0.0.525 finding: more holds, never fewer),
                                 // c80_judge (fix pass, the HIGH review's SHOULD) to credit committed writers from the union. Since the fix
                                 // pass (MF-3) the P's layer is switch 80's OWN count of plane-shaped pairs, not in_ok / in_layer.
                                 "static __attribute__((noinline)) void c80_judge(const uint64_t *hVa, const uint64_t *hPage, const uint32_t *hRes,",
                                 "static __attribute__((noinline)) void c80_heldback(const n48_r5_frame *r, uint32_t trunc, uint64_t ctx)" };
        uint32_t stray = 0, total = 0;
        for (const char *tok : { "gCy515", "gDe515" }) {
            for (size_t p = s.find(tok); p != std::string::npos; p = s.find(tok, p + 1)) {
                total++;
                bool inside = false;
                for (const char *o : owners) {
                    const size_t a2 = s.find(o);
                    const size_t b2 = a2 == std::string::npos ? a2 : s.find("\n}\n", a2);
                    if (a2 != std::string::npos && b2 != std::string::npos && p > a2 && p < b2) inside = true;
                }
                const size_t ls = s.rfind('\n', p) + 1;
                const std::string line = s.substr(ls, s.find('\n', p) - ls);
                const size_t cm = line.find("//");
                const std::string code = cm == std::string::npos ? line : line.substr(0, cm);
                if (cm != std::string::npos && p >= ls + cm) inside = true;   // a comment names it
                if (code.rfind("static ", 0) == 0 && (code.find(" gCy515") != std::string::npos || code.find(" gDe515") != std::string::npos) &&
                    code.find('(') == std::string::npos) inside = true;   // a declaration
                if (!inside) { stray++; std::printf("  stray: %s\n", line.c_str()); }
            }
        }
        expect_u("T7 no decision reads the instrument: every gCy515/gDe515 is in a helper, the report or a declaration", total > 0u && stray == 0u, 1u);
    }
    // build 0.0.516: 73 became switch 73 (gfx_present73.h, tested in gfx_present73_test.cpp); its bare read still prints
    // this instrument's lines, and the instrument is still read by nothing else (the stray check above).
    expect_u("T7 `gfxneuter 73` still prints the read-only instrument on every 73 verb",
             count(s, "} else if ((arg & 0xffull) == 73ull) {") == 1u &&
             count(s, "        cy515_report_line(\" - `gfxneuter 73` (the read-only instrument)\");") == 1u, 1u);
    expect_u("T7 bare 66 prints Part B", count(s, "    de515_report_line();   // build 0.0.515 Part B"), 1u);
}

int main(int argc, char **argv)
{
    std::printf("== T1 the write-set scan on real bytes ==\n"); t1_scan();
    std::printf("== T2/T3 run10u replayed ==\n"); t2_run10u();
    std::printf("== T4 edges ==\n"); t4_edges();
    std::printf("== T5 Part B ==\n"); t5_partb();
    std::printf("== T6 line bounds ==\n"); t6_lines();
    std::printf("== T7 kext glue ==\n"); t7_glue(argc > 1 ? argv[1] : nullptr);
    std::printf("gfx_cycle515: %d run, %d FAILED\n", gRun, gFail);
    return gFail ? 1 : 0;
}
