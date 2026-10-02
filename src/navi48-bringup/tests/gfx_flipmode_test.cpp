// gfx_flipmode_test.cpp — build 0.0.518: flip mode (switch 74), gfx_flipmode.h against a model of
// the display (HUBP0's programmed address, SURFACE_FLIP_PENDING, SURFACE_EARLIEST_INUSE, VUPDATE latching once per frame), the
// two-destination scanout allowlist (scanout_copy.h), and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_flipmode_test.cpp -o /tmp/fm && /tmp/fm \
//         src/navi48-bringup/src/apple/DisplayPipeGuard.cpp src/navi48-bringup/src/Navi48Bringup.cpp \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_commit.h \
//         src/navi48-bringup/src/dcn/navi48_dcn.cpp
// Covers:
//   T1 the back-buffer selection (front A -> B, front B -> A, anything else FOREIGN);
//   T2 a run of presents: each waits for the previous flip's latch, copies into the buffer NOT scanned, flips to it; A/B alternate;
//      the wait histogram; the flip and copy allowlists see only A and B;
//   T3 the bounded wait: a flip that never latches HOLDS the present (no copy, no flip) after <= 2 frames + one poll;
//   T4 the foreign front: flip mode OFF, nothing written (no copy, no flip), the exact set released;
//   T5 the restore: front B -> copy B into A, flip A, verified; front A -> no copy, flip A; a foreign front is left alone unless
//      forced; a restore that does not verify flips to A once more; OFF and disengaged afterwards; a withdrawal's request is
//      served by the next present;
//   T6 ADDRESS_HIGH: a programmed HIGH dword that differs from the back buffer's, or a flip whose read-back changes it -> OFF +
//      restore;
//   T7 the allowlists: n48_fm_flip_ok / n48_fm_dst_ok; n48_scanout_flip_dst_check admits exactly A and B (B + 64 KiB, the
//      console reservation, past BAR0, a B shorter than the console: refused); n48_scanout_plan_tiled_to re-bases onto B, refuses
//      a source overlapping B and any third destination; n48_fm_buffers_ok, n48_fm_hubp_ok, n48_fm_test_n;
//   T8 OFF: a present with the switch OFF touches nothing; a failed copy holds the flip;
//   T9 the report lines fit the 491-byte log body at 20-digit counters;
//   T10 the kext glue: 73's hold is asked BEFORE flip mode in dpg_perform (and the OFF branch is 0.0.517's copy); the switch is OFF
//      at boot and turned ON in one place; the disarm restores; a withdrawal only flags; the other scanout writers hold while ON;
//      the readers (content read / FNV, thumbnail) follow EARLIEST_INUSE; the restore copies B into A (not A into B); the test
//      verb's dispatch; the mid-arm guard;
//   T11 the 0.0.518 review's F1-F3 and (build 0.0.520,  MEDIUM-2) a failed B->A copy or an unsettled A keeps
//      flip mode engaged with the {A, B} set, so 842 / switch OFF retry it, and a restore's 1 means A verified - all reached
//      through the real present/restore sequence (nothing engaged by hand).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_flipmode.h"
#include "scanout_copy.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}

// The live geometry: console A at VRAM 0 (MC 0x80_0000_0000), 1920x1080, RowBytes 7680; vramBase 0x800000; B from
// the allocator above it.
static const uint64_t kMc = 0x8000000000ull;
static const uint64_t kAOff = 0, kBOff = 0x02000000ull, kLen = 7680ull * 1080ull;   // B: the BAR0 pool (vramBase + 24 MiB up)
static const uint64_t kForeign = 0x8002030000ull;   // dcnflip's test plane of, i.e. "somebody else's" buffer

// ---- the display model --------------------------------------------------------------------------------------------------
struct Disp {
    uint64_t programmed = kMc, earliest = kMc, now = 0, highOverride = 0;
    bool stuck = false, readFail = false, flipFail = false, flipHighChanges = false;
    uint32_t copyStatus = 0;
    uint32_t bToAStatus = 0, settled = 1, settledAsks = 0;   // 0.0.519 F3
    std::vector<uint64_t> flips, copies;
    uint32_t bToA = 0, releases = 0, reads = 0;
    uint64_t latchedAt = 0;
    void tick(uint64_t us) {
        const uint64_t before = now / N48_FM_FRAME_US;
        now += us;
        if (!stuck && now / N48_FM_FRAME_US != before && earliest != programmed) { earliest = programmed; latchedAt = now; }
    }
};
static int m_read_front(void *c, uint32_t *p, uint64_t *e)
{
    Disp *d = static_cast<Disp *>(c); d->reads++;
    if (d->readFail) return -1;
    *e = d->earliest; *p = d->earliest != d->programmed ? 1u : 0u;
    return 0;
}
static int m_read_primary(void *c, uint64_t *mc)
{
    Disp *d = static_cast<Disp *>(c);
    if (d->readFail) return -1;
    *mc = d->highOverride ? ((d->highOverride << 32) | (d->programmed & 0xffffffffull)) : d->programmed;
    return 0;
}
static void m_delay(void *c, uint32_t us) { static_cast<Disp *>(c)->tick(us); }
static uint32_t m_copy_to(void *c, uint64_t dst) { Disp *d = static_cast<Disp *>(c); d->copies.push_back(dst); return d->copyStatus; }
static uint32_t m_copy_b_to_a(void *c) { Disp *d = static_cast<Disp *>(c); d->bToA++; return d->bToAStatus; }
static uint32_t m_a_settled(void *c) { Disp *d = static_cast<Disp *>(c); d->settledAsks++; return d->settled; }
static int m_flip(void *c, uint64_t mc)
{
    Disp *d = static_cast<Disp *>(c);
    if (d->flipFail) return -5;
    d->flips.push_back(mc);
    d->programmed = mc;
    if (d->flipHighChanges) d->highOverride = (mc >> 32) + 1u;
    return 0;
}
static void m_release(void *c) { static_cast<Disp *>(c)->releases++; }
static n48_fm_ops ops_of(Disp *d) { n48_fm_ops o { d, m_read_front, m_read_primary, m_delay, m_copy_to, m_copy_b_to_a, m_flip, m_release, m_a_settled }; return o; }
static n48_fm engaged_fm()
{
    n48_fm f {};
    f.on = 1u; f.engaged = 1u; f.haveB = 1u;
    f.aOff = kAOff; f.bOff = kBOff; f.len = kLen; f.aMc = kMc + kAOff; f.bMc = kMc + kBOff;
    return f;
}

// ---------------------------------------------------------------------------------------------------------------- T1
static void t1_select()
{
    uint64_t back = 1;
    const uint64_t A = kMc, B = kMc + kBOff;
    expect_u("T1 front A -> back B", n48_fm_select(A, A, B, &back) == 0u && back == B, 1u);
    expect_u("T1 front B -> back A", n48_fm_select(B, A, B, &back) == 0u && back == A, 1u);
    expect_u("T1 front neither (dcnflip's plane) -> FOREIGN, back 0", n48_fm_select(kForeign, A, B, &back) == 1u && back == 0u, 1u);
    expect_u("T1 front 0 -> FOREIGN", n48_fm_select(0u, A, B, &back), 1u);
    expect_u("T1 A == B (not engaged) -> FOREIGN", n48_fm_select(A, A, A, &back), 1u);
    expect_u("T1 B unset -> FOREIGN", n48_fm_select(A, A, 0u, &back), 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T2
static void t2_run()
{
    Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
    uint32_t cst = 99, flipped = 0, alternation = 1, dstsOk = 1;
    for (uint32_t i = 0; i < 12; i++) {
        const uint64_t frontBefore = d.earliest;
        const uint32_t r = n48_fm_present(&f, &o, &cst);
        if (r == N48_FM_P_FLIPPED) flipped++;
        const uint64_t want = (d.earliest == kMc) ? kMc + kBOff : kMc;   // after the wait, the back of the front
        (void)frontBefore;
        if (d.flips.empty() || d.flips.back() != want) alternation = 0;
        if (d.copies.back() != (want == kMc ? kAOff : kBOff)) dstsOk = 0;
        d.tick(5000);                                          // WindowServer's next present ~5 ms later
    }
    expect_u("T2 12 presents -> 12 flips", flipped == 12u && f.flips == 12u, 1u);
    expect_u("T2 each flip targets the buffer NOT being scanned (after the latch)", alternation, 1u);
    expect_u("T2 each copy lands in that buffer's VRAM offset", dstsOk, 1u);
    uint32_t ab = 1;
    for (size_t i = 0; i < d.flips.size(); i++) if (d.flips[i] != ((i & 1u) ? kMc : kMc + kBOff)) ab = 0;
    expect_u("T2 the flips alternate B, A, B, A, ... starting from front A", ab, 1u);
    expect_u("T2 flips to B = 6, to A = 6", f.flipsToB == 6u && f.flipsToA == 6u, 1u);
    uint64_t hs = 0; for (uint32_t b = 0; b < N48_FM_WAIT_BUCKETS; b++) hs += f.waitHist[b];
    expect_u("T2 every flip was preceded by a latched wait, all bucketed", f.waits == 12u && hs == 12u, 1u);
    expect_u("T2 the first present needed no wait (bucket 0); the rest waited for a latch (buckets 1..6)",
             f.waitHist[0] >= 1u && hs - f.waitHist[0] >= 10u, 1u);
    expect_u("T2 no wait exceeded two frames", f.waitMaxUs <= N48_FM_WAIT_MAX_US, 1u);
    expect_u("T2 no timeouts, no foreign fronts, no restores", f.timeouts + f.foreign + f.restores + f.restoreFails, 0u);
    expect_u("T2 flip mode still ON and engaged", f.on == 1u && f.engaged == 1u, 1u);
    uint32_t onlyAB = 1;
    for (uint64_t x : d.flips) if (!n48_fm_flip_ok(&f, x)) onlyAB = 0;
    for (uint64_t x : d.copies) if (!n48_fm_dst_ok(&f, x)) onlyAB = 0;
    expect_u("T2 every flip address and copy destination passed the A/B allowlists", onlyAB, 1u);
    expect_u("T2 bucket edges: 0 polls -> 0; 1 ms -> 1; 16.68 ms -> 5; 33 ms -> 6",
             n48_fm_wait_bucket(0, 0) == 0u && n48_fm_wait_bucket(3, 1000) == 1u && n48_fm_wait_bucket(9, N48_FM_FRAME_US) == 5u &&
             n48_fm_wait_bucket(9, 33000) == 6u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T3
static void t3_timeout()
{
    Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
    uint32_t cst = 0;
    expect_u("T3 first present flips to B", n48_fm_present(&f, &o, &cst), (uint64_t)N48_FM_P_FLIPPED);
    d.stuck = true;                                          // the VUPDATE never comes: B stays pending
    const uint64_t t0 = d.now; const size_t c0 = d.copies.size(), fl0 = d.flips.size();
    expect_u("T3 the next present is HELD", n48_fm_present(&f, &o, &cst), (uint64_t)N48_FM_P_HELD);
    expect_u("T3 ... after at most two frames plus one poll", d.now - t0 <= N48_FM_WAIT_MAX_US + N48_FM_POLL_US &&
             d.now - t0 >= N48_FM_WAIT_MAX_US, 1u);
    expect_u("T3 ... with no copy and no flip", d.copies.size() == c0 && d.flips.size() == fl0, 1u);
    expect_u("T3 ... counted as a timeout; flip mode stays ON", f.timeouts == 1u && f.on == 1u && f.engaged == 1u, 1u);
    d.stuck = false; d.tick(N48_FM_FRAME_US);
    expect_u("T3 once it latches, the next present flips to A", n48_fm_present(&f, &o, &cst) == N48_FM_P_FLIPPED &&
             d.flips.back() == kMc, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T4
static void t4_foreign()
{
    Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
    uint32_t cst = 0;
    d.programmed = d.earliest = kForeign;                    // somebody else put their buffer on HUBP0
    expect_u("T4 a foreign front turns flip mode OFF", n48_fm_present(&f, &o, &cst), (uint64_t)N48_FM_P_OFF);
    expect_u("T4 ... writes nothing (no copy, no flip)", d.copies.size() + d.flips.size() + d.bToA, 0u);
    expect_u("T4 ... counted, OFF, disengaged, the exact set released",
             f.foreign == 1u && f.on == 0u && f.engaged == 0u && d.releases == 1u && f.offWhy == N48_FM_OFF_FOREIGN, 1u);
    expect_u("T4 a later present asks nothing", n48_fm_present(&f, &o, &cst) == N48_FM_P_HELD && d.reads == 1u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T5
static void t5_restore()
{
    {   // front B
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst); d.tick(N48_FM_FRAME_US);   // B latched: front B
        expect_u("T5 setup: front B", d.earliest, kMc + kBOff);
        const uint32_t ok = n48_fm_restore(&f, &o, N48_FM_OFF_VERB, 0u);
        expect_u("T5 front B: B copied into A once, then flipped to A", d.bToA == 1u && d.flips.back() == kMc, 1u);
        expect_u("T5 ... verified (EARLIEST_INUSE == A, programmed == A)", ok == 1u && d.earliest == kMc && f.restores == 1u, 1u);
        expect_u("T5 ... OFF, disengaged, released, reason kept",
                 f.on == 0u && f.engaged == 0u && d.releases == 1u && f.offWhy == N48_FM_OFF_VERB, 1u);
    }
    {   // front A
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        const uint32_t ok = n48_fm_restore(&f, &o, N48_FM_OFF_DISARM, 0u);
        expect_u("T5 front A: no copy, flipped to A (unconditional), verified", ok == 1u && d.bToA == 0u && d.flips.size() == 1u &&
                 d.flips[0] == kMc, 1u);
    }
    {   // pending flip to B at the restore: settled first, then carried into A
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst);                 // flip to B, pending
        const uint32_t ok = n48_fm_restore(&f, &o, N48_FM_OFF_WITHDRAWAL, 0u);
        expect_u("T5 a pending flip to B is waited for, B copied into A, A verified", ok == 1u && d.bToA == 1u && d.earliest == kMc, 1u);
    }
    {   // foreign, not forced / forced
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        d.programmed = d.earliest = kForeign;
        expect_u("T5 foreign front, not forced: nothing written, counted failed",
                 n48_fm_restore(&f, &o, N48_FM_OFF_VERB, 0u) == 0u && d.flips.empty() && f.restoreFails == 1u && f.engaged == 0u, 1u);
        n48_fm g = engaged_fm();
        expect_u("T5 foreign front, FORCED: flipped to A and verified",
                 n48_fm_restore(&g, &o, N48_FM_OFF_FORCE, 1u) == 1u && d.flips.back() == kMc && d.earliest == kMc, 1u);
    }
    {   // a restore whose first flip does not verify flips once more (the dcnflip rule)
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        d.programmed = d.earliest = kMc + kBOff;              // front B, and the flip back to A never latches
        f.bHoldsPresent = 1u;                                 // 0.0.519: B holds a present (as after a present's flip to B)
        d.stuck = true;
        const uint32_t ok = n48_fm_restore(&f, &o, N48_FM_OFF_VERB, 0u);
        expect_u("T5 never latching: B copied into A, two flips to A, counted failed",
                 ok == 0u && d.bToA == 1u && d.flips.size() == 2u && f.restoreFails == 1u && f.on == 0u, 1u);
    }
    {   // not engaged: nothing to restore
        Disp d; n48_fm f {}; n48_fm_ops o = ops_of(&d);
        f.on = 1u;
        expect_u("T5 not engaged: OFF, no I/O", n48_fm_restore(&f, &o, N48_FM_OFF_VERB, 0u) == 1u && f.on == 0u && d.reads == 0u, 1u);
    }
    {   // a withdrawal: a flag, served by the next present
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst); d.tick(N48_FM_FRAME_US);   // front B
        expect_u("T5 withdrawal: recorded once", n48_fm_request_restore(&f, N48_FM_OFF_WITHDRAWAL) == 1u &&
                 n48_fm_request_restore(&f, N48_FM_OFF_WITHDRAWAL) == 0u && f.restoreRequests == 1u, 1u);
        const size_t c0 = d.copies.size();
        expect_u("T5 ... the next present restores instead of presenting", n48_fm_present(&f, &o, &cst), (uint64_t)N48_FM_P_OFF);
        expect_u("T5 ... B carried into A, A verified, no new detile, reason WITHDRAWAL",
                 d.bToA == 1u && d.earliest == kMc && d.copies.size() == c0 && f.offWhy == N48_FM_OFF_WITHDRAWAL &&
                 f.restoreReq == 0u, 1u);
        n48_fm off {};
        expect_u("T5 a withdrawal with flip mode OFF records nothing", n48_fm_request_restore(&off, N48_FM_OFF_WITHDRAWAL), 0u);
    }
}

// ---------------------------------------------------------------------------------------------------------------- T6
static void t6_high()
{
    {
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        d.highOverride = 0x81;                                // the programmed HIGH dword is not the back buffer's
        expect_u("T6 programmed HIGH != back's HIGH: OFF before any copy", n48_fm_present(&f, &o, &cst) == N48_FM_P_OFF &&
                 d.copies.empty() && f.highChanged == 1u && f.offWhy == N48_FM_OFF_HIGH, 1u);
    }
    {
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        d.flipHighChanges = true;                             // the flip's read-back shows a changed HIGH dword
        expect_u("T6 a flip that changes ADDRESS_HIGH: OFF + restore", n48_fm_present(&f, &o, &cst) == N48_FM_P_OFF &&
                 f.highChanged == 1u && f.offWhy == N48_FM_OFF_HIGH && f.on == 0u, 1u);
    }
    expect_u("T6 A and B at the live placement share the MC HIGH dword (0x80)",
             ((kMc + kAOff) >> 32) == 0x80u && ((kMc + kBOff + kLen - 1u) >> 32) == 0x80u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T7
static N48ScanoutGeom live_geom()
{
    N48ScanoutGeom g {};
    g.fbPhys = 0xC0000000ull; g.fbLen = kLen; g.width = 1920; g.height = 1080; g.rowBytes = 7680; g.depth = 32;
    g.bar0Phys = 0xC0000000ull; g.bar0Size = 0x10000000ull; g.vramBase = 0x800000ull; g.vramSize = 0x400000000ull;
    return g;
}
static void t7_allow()
{
    n48_fm f = engaged_fm();
    expect_u("T7 flip_ok: A yes, B yes", n48_fm_flip_ok(&f, kMc) && n48_fm_flip_ok(&f, kMc + kBOff), 1u);
    expect_u("T7 flip_ok: B+0x1000, the foreign plane, 0: no",
             n48_fm_flip_ok(&f, kMc + kBOff + 0x1000u) + n48_fm_flip_ok(&f, kForeign) + n48_fm_flip_ok(&f, 0u), 0u);
    expect_u("T7 dst_ok: A and B offsets only",
             n48_fm_dst_ok(&f, kAOff) && n48_fm_dst_ok(&f, kBOff) && !n48_fm_dst_ok(&f, kBOff + 4u) && !n48_fm_dst_ok(&f, 0x800000u), 1u);
    const N48ScanoutGeom g = live_geom();
    uint64_t dl = 0;
    expect_u("T7 flip_dst_check: A admitted (len = the console's)", n48_scanout_flip_dst_check(&g, kBOff, kLen, 0u, &dl) == kScanGeomOk &&
             dl == kLen, 1u);
    expect_u("T7 flip_dst_check: B admitted", n48_scanout_flip_dst_check(&g, kBOff, kLen, kBOff, &dl) == kScanGeomOk && dl == kLen, 1u);
    expect_u("T7 flip_dst_check: B + 64 KiB (outside B) refused NotConsole",
             n48_scanout_flip_dst_check(&g, kBOff, kLen, kBOff + 0x10000u, &dl), (uint64_t)kScanGeomNotConsole);
    expect_u("T7 flip_dst_check: a VRAM offset in the console reservation (not A) refused",
             n48_scanout_flip_dst_check(&g, kBOff, kLen, 0x400000u, &dl), (uint64_t)kScanGeomNotConsole);
    expect_u("T7 flip_dst_check: a B registered below vramBase refused",
             n48_scanout_flip_dst_check(&g, 0x10000u * 64u, kLen, 0x10000u * 64u, &dl), (uint64_t)kScanGeomNotConsole);
    expect_u("T7 flip_dst_check: a B past BAR0 refused",
             n48_scanout_flip_dst_check(&g, 0x0ffe0000ull, kLen, 0x0ffe0000ull, &dl), (uint64_t)kScanGeomOutsideBar0);
    expect_u("T7 flip_dst_check: a B shorter than the console refused",
             n48_scanout_flip_dst_check(&g, kBOff, kLen - 4u, kBOff, &dl), (uint64_t)kScanGeomShape);
    expect_u("T7 flip_dst_check: a misaligned B refused", n48_scanout_flip_dst_check(&g, kBOff + 0x100u, kLen, kBOff + 0x100u, &dl),
             (uint64_t)kScanGeomAlign);
    N48ScanoutTiledPlan p {};
    const uint64_t src = 0x10930000ull, srcLen = 0x870000ull;
    expect_u("T7 plan_tiled_to B: planned, re-based onto B, every byte inside B",
             n48_scanout_plan_tiled_to(&g, kBOff, kLen, kBOff, src, srcLen, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &p) == kScanTiledOk &&
             p.fbOff == kBOff && n48_scanout_tiled_dst_ok(&p, kLen), 1u);
    expect_u("T7 plan_tiled_to A: the console plan, unchanged",
             n48_scanout_plan_tiled_to(&g, kBOff, kLen, 0u, src, srcLen, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &p) == kScanTiledOk &&
             p.fbOff == 0u, 1u);
    expect_u("T7 plan_tiled_to a third destination: refused (Geom)",
             n48_scanout_plan_tiled_to(&g, kBOff, kLen, kBOff + 0x10000u, src, srcLen, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &p),
             (uint64_t)kScanTiledGeom);
    expect_u("T7 plan_tiled_to B with a source overlapping B: refused (Overlap)",
             n48_scanout_plan_tiled_to(&g, kBOff, kLen, kBOff, kBOff + 0x100000u, srcLen, 1920, 1080, 3, 0, 0, 0, 0, 1920, 1080, &p),
             (uint64_t)kScanTiledOverlap);
    {   // differential: into A, plan_tiled_to answers exactly what n48_scanout_plan_tiled answers, field for field
        const uint64_t srcs[] = { 0x10930000ull, 0x11200000ull, 0x20000000ull, 0x10930004ull, 0x10938000ull, 0x0ull, 0x00400000ull, 0x3fffe0000ull, 0x1ull << 40 };
        const uint64_t lens[] = { 0x870000ull, 0x10000ull, 0x0ull, 0x1000000ull };
        const uint32_t dims[][2] = { { 1920, 1080 }, { 256, 256 }, { 2560, 1440 }, { 0, 1080 }, { 70000, 10 }, { 1920, 0 } };
        const uint32_t rects[][6] = { { 0, 0, 0, 0, 1920, 1080 }, { 5, 7, 11, 13, 100, 50 }, { 0, 0, 1919, 1079, 5, 5 },
                                      { 1920, 0, 0, 0, 1, 1 }, { 0, 0, 1920, 0, 1, 1 }, { 3, 3, 0, 0, 0, 0 } };
        uint32_t cases = 0, same = 0, planned = 0;
        for (uint64_t so : srcs) for (uint64_t sl : lens) for (auto &dm : dims) for (uint32_t sw = 2; sw <= 3; sw++)
            for (auto &r : rects) {
                N48ScanoutTiledPlan pa {}, pb {};
                const uint32_t ra = n48_scanout_plan_tiled(&g, so, sl, dm[0], dm[1], sw, r[0], r[1], r[2], r[3], r[4], r[5], &pa);
                const uint32_t rb = n48_scanout_plan_tiled_to(&g, kBOff, kLen, 0u, so, sl, dm[0], dm[1], sw, r[0], r[1], r[2], r[3],
                                                              r[4], r[5], &pb);
                cases++; if (ra == kScanTiledOk) planned++;
                if (ra == rb && (ra != kScanTiledOk || std::memcmp(&pa, &pb, sizeof pa) == 0)) same++;
            }
        std::printf("      (differential over %u cases, %u planned)\n", cases, planned);
        expect_u("T7 plan_tiled_to(A) == n48_scanout_plan_tiled on every case (answer, and the plan when planned)",
                 cases == same && cases > 1000u && planned >= 40u, 1u);
    }
    expect_u("T7 buffers_ok: the live placement", n48_fm_buffers_ok(kAOff, kBOff, kLen, 0x10000000ull, 0x800000ull, kMc), 0u);
    expect_u("T7 buffers_ok: B overlapping the console reservation -> 3",
             n48_fm_buffers_ok(kAOff, 0x400000ull, kLen, 0x10000000ull, 0x800000ull, kMc), 3u);
    expect_u("T7 buffers_ok: B misaligned -> 2", n48_fm_buffers_ok(kAOff, kBOff + 0x1000u, kLen, 0x10000000ull, 0x800000ull, kMc), 2u);
    expect_u("T7 buffers_ok: B past BAR0 -> 4", n48_fm_buffers_ok(kAOff, 0x0ff80000ull, kLen, 0x10000000ull, 0x800000ull, kMc), 4u);
    expect_u("T7 buffers_ok: B ending above 256 MiB (BAR0 512 MiB) -> 5",
             n48_fm_buffers_ok(kAOff, 0x0ff80000ull, kLen, 0x20000000ull, 0x800000ull, kMc), 5u);
    expect_u("T7 buffers_ok: an MC base whose HIGH dword differs across B -> 6",
             n48_fm_buffers_ok(kAOff, kBOff, kLen, 0x10000000ull, 0x800000ull, 0x80fff00000ull), 6u);
    expect_u("T7 hubp_ok: the live plane (1920x1080 pitch 1920 fmt 8 linear)", n48_fm_hubp_ok(1920, 1080, 1920, 8, 0, 1920, 1080, 7680), 1u);
    expect_u("T7 hubp_ok: tiled / other pitch / other size / 64-bit format refused",
             n48_fm_hubp_ok(1920, 1080, 1920, 8, 27, 1920, 1080, 7680) + n48_fm_hubp_ok(1920, 1080, 2048, 8, 0, 1920, 1080, 7680) +
             n48_fm_hubp_ok(2560, 1440, 2560, 8, 0, 1920, 1080, 7680) + n48_fm_hubp_ok(1920, 1080, 1920, 12, 0, 1920, 1080, 7680), 0u);
    expect_u("T7 test_n: 1002 -> 2, 1240 -> 240, 1001/1241/30/0 -> 0",
             n48_fm_test_n(1002) == 2u && n48_fm_test_n(1240) == 240u && n48_fm_test_n(1001) == 0u && n48_fm_test_n(1241) == 0u &&
             n48_fm_test_n(30) == 0u && n48_fm_test_n(0) == 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T8
static void t8_off()
{
    Disp d; n48_fm f {}; n48_fm_ops o = ops_of(&d);
    uint32_t cst = 7;
    expect_u("T8 switch OFF: HELD, no read, no copy, no flip", n48_fm_present(&f, &o, &cst) == N48_FM_P_HELD && d.reads == 0u &&
             d.copies.empty() && d.flips.empty() && cst == 0u, 1u);
    n48_fm eOff = engaged_fm(); eOff.on = 0u;                 // engaged but the switch OFF (a present racing the OFF verb)
    expect_u("T8 switch OFF while still engaged: HELD, no read, no copy, no flip",
             n48_fm_present(&eOff, &o, &cst) == N48_FM_P_HELD && d.reads == 0u && d.copies.empty() && d.flips.empty(), 1u);
    n48_fm g = engaged_fm();
    d.copyStatus = 10;                                        // a fence timeout
    expect_u("T8 a failed copy HOLDS the flip (COPYFAIL, no flip), flip mode stays ON",
             n48_fm_present(&g, &o, &cst) == N48_FM_P_COPYFAIL && cst == 10u && d.flips.empty() && g.copyFails == 1u && g.on == 1u, 1u);
    d.copyStatus = 11;                                        // a copy made whose S2 readback differed: still a copy
    expect_u("T8 a readback-differed copy (11) still flips", n48_fm_present(&g, &o, &cst) == N48_FM_P_FLIPPED && d.flips.size() == 1u, 1u);
    Disp e; n48_fm h = engaged_fm(); n48_fm_ops oe = ops_of(&e);
    e.readFail = true;
    expect_u("T8 a HUBP read failure: OFF", n48_fm_present(&h, &oe, &cst) == N48_FM_P_OFF && h.readFails == 1u && h.on == 0u, 1u);
    Disp k; n48_fm j = engaged_fm(); n48_fm_ops ok = ops_of(&k);
    k.flipFail = true;
    expect_u("T8 a refused flip write: OFF (restore attempted), counted",
             n48_fm_present(&j, &ok, &cst) == N48_FM_P_OFF && j.flipFails == 1u && j.on == 0u && j.offWhy == N48_FM_OFF_FLIPFAIL, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T9
static void t9_lines()
{
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    int w = std::snprintf(b, sizeof b, N48_FM_REPORT1_FMT, "OFF (default)", " - REFUSED: a continuous arm stands, unchanged", 1u, M, M,
                          M, M, M, "destination not A/B", 10u);
    expect_u("T9 report line 1 <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_FM_REPORT2_FMT, M, M, M, M, M, M, M, M, M, M, M, M, 4294967295u, M, M);
    expect_u("T9 report line 2 <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_FM_REPORT3_FMT, M, M, M, M, M, M, M, M, M, M, M, M);
    expect_u("T9 report line 3 <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_FM_REPORT4_FMT, 4294967295u, M, M, M, M, M, 1u, 1u);
    expect_u("T9 (0.0.519) report line 4 <= 491", w > 0 && w <= 491, 1u);
    expect_u("T9 no report line names Apple's REFUSED FLIPS counter (accel-run.sh aborts on it)",
             std::strstr(N48_FM_REPORT1_FMT N48_FM_REPORT2_FMT N48_FM_REPORT3_FMT N48_FM_REPORT4_FMT, "REFUSED FLIPS") == nullptr, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T11
// build 0.0.519 (the 0.0.518 review): F1 a stuck latch ends, F2 bHoldsPresent, F3 a failed B -> A copy.
static void t11_review()
{
    {   // F1: the latch never comes (the OTG stopped): 3 consecutive timeouts restore to A and turn flip mode OFF
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst);                   // flip to B, pending
        d.stuck = true;
        const uint32_t r1 = n48_fm_present(&f, &o, &cst), r2 = n48_fm_present(&f, &o, &cst);
        expect_u("T11 F1: timeouts 1 and 2 HOLD (flip mode stays ON)", r1 == N48_FM_P_HELD && r2 == N48_FM_P_HELD && f.on == 1u &&
                 f.consecTimeouts == 2u, 1u);
        const size_t fl0 = d.flips.size(), c0 = d.copies.size();
        const uint32_t r3 = n48_fm_present(&f, &o, &cst);
        expect_u("T11 F1: the 3rd consecutive timeout restores: OFF (LATCH STUCK), a flip to A, no detile",
                 r3 == N48_FM_P_OFF && f.on == 0u && f.engaged == 0u && f.offWhy == N48_FM_OFF_STUCK && f.stuckRestores == 1u &&
                 d.flips.size() > fl0 && d.flips.back() == kMc && d.copies.size() == c0, 1u);
        expect_u("T11 F1: ... and a later present asks nothing", n48_fm_present(&f, &o, &cst), (uint64_t)N48_FM_P_HELD);
    }
    {   // F1: two timeouts, then a latch: the run resets and flip mode goes on
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst);
        d.stuck = true; (void)n48_fm_present(&f, &o, &cst); (void)n48_fm_present(&f, &o, &cst);
        d.stuck = false; d.tick(N48_FM_FRAME_US);
        expect_u("T11 F1: a latched wait resets the run", n48_fm_present(&f, &o, &cst) == N48_FM_P_FLIPPED && f.consecTimeouts == 0u, 1u);
        d.stuck = true; (void)n48_fm_present(&f, &o, &cst); (void)n48_fm_present(&f, &o, &cst);
        expect_u("T11 F1: ... so two more timeouts still only HOLD", f.on == 1u && f.stuckRestores == 0u && f.timeouts == 4u, 1u);
    }
    {   // F1: another writer reprograms HUBP0 while our flip is pending: the timed-out front is FOREIGN -> OFF, nothing written
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst);                   // our flip to B
        d.stuck = true; d.earliest = kForeign;                 // the plane now scans somebody else's buffer, B never latches
        const size_t fl0 = d.flips.size(), c0 = d.copies.size();
        expect_u("T11 F1: a timed-out foreign front takes the FOREIGN path at the FIRST timeout",
                 n48_fm_present(&f, &o, &cst) == N48_FM_P_OFF && f.offWhy == N48_FM_OFF_FOREIGN && f.timeoutForeign == 1u &&
                 f.foreign == 1u && f.on == 0u && f.engaged == 0u && d.releases == 1u, 1u);
        expect_u("T11 F1: ... writing nothing (no flip, no copy, no B->A)", d.flips.size() == fl0 && d.copies.size() == c0 && d.bToA == 0u, 1u);
    }
    {   // F2: B holds the A/B test's grey fill, not a present: the force restore does not copy it into A
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        d.programmed = d.earliest = kMc + kBOff; f.bHoldsPresent = 0u;
        expect_u("T11 F2: front B without a present: A is NOT overwritten, flipped to A and verified",
                 n48_fm_restore(&f, &o, N48_FM_OFF_FORCE, 1u) == 1u && d.bToA == 0u && f.restoreCopySkipped == 1u &&
                 d.flips.back() == kMc && d.earliest == kMc, 1u);
        Disp e; n48_fm g = engaged_fm(); n48_fm_ops oe = ops_of(&e);
        uint32_t cst = 0;
        (void)n48_fm_present(&g, &oe, &cst); e.tick(N48_FM_FRAME_US);   // a present landed in B and is on the glass
        expect_u("T11 F2: a completed detile into B sets bHoldsPresent", g.bHoldsPresent, 1u);
        expect_u("T11 F2: ... so the restore carries B into A", n48_fm_restore(&g, &oe, N48_FM_OFF_VERB, 0u) == 1u && e.bToA == 1u, 1u);
        Disp k; n48_fm h = engaged_fm(); n48_fm_ops ok = ops_of(&k);
        h.bHoldsPresent = 1u; k.copyStatus = 10u;             // front A: the next detile goes into B, and fails
        (void)n48_fm_present(&h, &ok, &cst);
        expect_u("T11 F2: a FAILED detile into B clears bHoldsPresent", h.bHoldsPresent, 0u);
    }
    {   // F3 + build 0.0.520: the B -> A copy fails: no flip to A, the front is kept, OFF - and STILL
        // ENGAGED with the {A, B} set, so the next restore retries. Reached through the real sequence only: engage (engaged_fm,
        // as fm_engage_locked leaves it), a present lands in B and latches, the restore's copy fails; nothing is set by hand.
        Disp d; n48_fm f = engaged_fm(); n48_fm_ops o = ops_of(&d);
        uint32_t cst = 0;
        (void)n48_fm_present(&f, &o, &cst); d.tick(N48_FM_FRAME_US);    // front B holding a present
        d.bToAStatus = 10u;                                              // the copy's fence timed out
        const size_t fl0 = d.flips.size();
        expect_u("T11 F3: a failed B->A copy: NOT VERIFIED and NO flip to A (front stays B)",
                 n48_fm_restore(&f, &o, N48_FM_OFF_VERB, 0u) == 0u && d.flips.size() == fl0 && d.earliest == kMc + kBOff, 1u);
        expect_u("T11 F3: ... OFF, counted, A copy pending",
                 f.on == 0u && f.restoreCopyFails == 1u && f.aCopyPending == 1u && f.offWhy == N48_FM_OFF_COPYFAIL, 1u);
        expect_u("T11 F3 (0.0.520): ... STILL ENGAGED and the exact set NOT released (the restore can be retried)",
                 f.engaged == 1u && d.releases == 0u, 1u);
        // `gfxneuter 842` while the copy still fails: a retry (the copy is attempted again), NOT a success, still engaged
        expect_u("T11 F3 (0.0.520): 842 while the copy still fails: retried (a 2nd copy), NOT VERIFIED, no flip, still engaged",
                 n48_fm_restore(&f, &o, N48_FM_OFF_FORCE, 1u) == 0u && d.bToA == 2u && d.flips.size() == fl0 &&
                 f.engaged == 1u && d.releases == 0u && f.restoreCopyFails == 2u, 1u);
        // then the copy works: the SAME engagement, no re-engage by hand; the copy's own fence proves the queue drained, then A
        d.bToAStatus = 0u;
        const uint32_t ok842 = n48_fm_restore(&f, &o, N48_FM_OFF_FORCE, 1u);
        expect_u("T11 F3 (0.0.520): 842 once the copy lands: 1 means HUBP0 VERIFIED on A (EARLIEST_INUSE and programmed == A)",
                 ok842 == 1u && d.earliest == kMc && d.programmed == kMc && d.flips.back() == kMc && f.aCopyPending == 0u, 1u);
        expect_u("T11 F3 (0.0.520): ... and only now disengaged, the set released once", f.engaged == 0u && d.releases == 1u, 1u);
        // the switch OFF (not the force verb) also retries: a fresh failure, then `gfxneuter 586`'s restore
        Disp e; n48_fm g = engaged_fm(); n48_fm_ops oe = ops_of(&e);
        (void)n48_fm_present(&g, &oe, &cst); e.tick(N48_FM_FRAME_US); e.bToAStatus = 10u;
        (void)n48_fm_restore(&g, &oe, N48_FM_OFF_DISARM, 0u);
        e.bToAStatus = 0u;
        expect_u("T11 F3 (0.0.520): a later non-forced restore (switch OFF / disarm) retries and verifies A",
                 g.engaged == 1u && n48_fm_restore(&g, &oe, N48_FM_OFF_VERB, 0u) == 1u && e.earliest == kMc && g.engaged == 0u, 1u);
    }
    {   // F3: A copy pending and a restore that does not copy (front A) must ask a_settled first. Reached by the real sequence:
        // a failed copy (pending set, still engaged), then the plane is found on A (another path's flip, modelled in the display).
        Disp e; n48_fm g = engaged_fm(); n48_fm_ops oe = ops_of(&e);
        uint32_t cst = 0;
        (void)n48_fm_present(&g, &oe, &cst); e.tick(N48_FM_FRAME_US); e.bToAStatus = 10u;
        (void)n48_fm_restore(&g, &oe, N48_FM_OFF_VERB, 0u);
        e.programmed = e.earliest = kMc;                                 // the display now scans A
        e.settled = 0u;
        const size_t fl0 = e.flips.size();
        expect_u("T11 F3: A copy pending and NOT settled: no flip to A, still engaged (0.0.520)",
                 g.aCopyPending == 1u && n48_fm_restore(&g, &oe, N48_FM_OFF_FORCE, 1u) == 0u && e.flips.size() == fl0 &&
                 e.settledAsks == 1u && g.restoreUnsettled == 1u && g.engaged == 1u && e.releases == 0u, 1u);
        e.settled = 1u;
        expect_u("T11 F3: ... settled: flipped to A, verified, pending cleared, disengaged",
                 n48_fm_restore(&g, &oe, N48_FM_OFF_FORCE, 1u) == 1u && e.flips.size() == fl0 + 1u && g.aCopyPending == 0u &&
                 g.engaged == 0u && e.releases == 1u, 1u);
    }
}

// ---------------------------------------------------------------------------------------------------------------- T10
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
static size_t at(const std::string &s, const char *n) { return s.find(n); }
static std::string fn_body(const std::string &s, const char *sig)
{
    const size_t a = s.find(sig);
    if (a == std::string::npos) return std::string();
    const size_t e = s.find("\n}\n", a);
    return s.substr(a, e == std::string::npos ? std::string::npos : e - a);
}
static void t10_glue(const char *dpgP, const char *bupP, const char *ahhP, const char *cmP, const char *dcnP)
{
    const std::string d = slurp(dpgP), b = slurp(bupP), s = slurp(ahhP), c = slurp(cmP), n = slurp(dcnP);
    expect_u("T10 the five sources read", !d.empty() && !b.empty() && !s.empty() && !c.empty() && !n.empty(), 1u);
    // dpg_perform: 73's hold first, then flip mode, then (OFF) 0.0.517's copy
    const std::string dp = fn_body(d, "static uint32_t dpg_perform(void *self, void *txn) {");
    const char *d73 = "    if (n48::hw_p73_on() && !n48::hw_p73_present(phys, gSh.perform)) return 0;\n";
    const char *dOn = "    if (navi48_fm_on()) {\n        if (navi48_fm_present(phys, len, surfW, surfH, swz, pw, ph, linearCopy, verifyNow, gSh.gcr != 0u, gSh.perform, cv, 11,\n                              &cst) != 0u)\n            return 0;\n    } else cst = !linearCopy\n";
    const char *dTiled = "        ? navi48_scanout_copy_tiled(phys, len, surfW, surfH, swz, 0, 0, pw, ph, cv, 11, verifyNow, gSh.gcr != 0u)\n"
                         "        : navi48_scanout_copy_vram(phys, len, pw, ph, stride, 0, 0, pw, ph, cv, 11, verifyNow);\n";
    expect_u("T10 dpg_perform: 73's hold, then flip mode, then the OFF branch's 0.0.517 copy (each exactly once)",
             !dp.empty() && count(dp, d73) == 1u && count(dp, dOn) == 1u && count(dp, dTiled) == 1u &&
             at(dp, d73) < at(dp, dOn) && at(dp, dOn) < at(dp, dTiled) &&
             at(dp, "    const bool linearCopy = (swz != 3u) || (gSh.forceLinear != 0u);") < at(dp, dOn), 1u);
    expect_u("T10 the shim has exactly two flip-mode calls (the question and the present)",
             count(d, "navi48_fm_on()") == 1u && count(d, "navi48_fm_present(") == 1u, 1u);
    // the switch: OFF at boot, ON in one place, only after a successful engage
    expect_u("T10 flip mode is OFF at boot (gFm zero-initialised)", count(b, "static n48_fm  gFm {};"), 1u);
    expect_u("T10 gFm.on is set to 1 in exactly one place, after a successful engage",
             count(b, "__atomic_store_n(&gFm.on, 1u") == 1u && count(b, "gFm.on = 1") == 0u &&
             count(b, "else if (fm_engage_locked(true) != 0u) { st = 12u;") == 1u &&
             at(b, "else if (fm_engage_locked(true) != 0u) { st = 12u;") < at(b, "__atomic_store_n(&gFm.on, 1u"), 1u);
    // the verb (AHH) and the mid-arm guard
    expect_u("T10 gfxneuter 74: the continuous mid-arm guard for M 1/2, M 3 always allowed, then navi48_fm_control",
             count(s, "        const bool contRefused74 = m != 3u && n48_cm_cont_switch_refused(74u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;\n"
                      "        st = navi48_fm_control(m, contRefused74 ? 1u : 0u);") == 1u, 1u);
    expect_u("T10 gfx_commit.h guards 74", count(c, "    case 74u:   /* build 0.0.518"), 1u);
    // disarm and withdrawal
    const char *dis = "            n48_fs_clear(&gFs);   // 0.0.404: the disarm closes the fill-set reservation's window too\n"
                      "            navi48_fm_disarm();";
    expect_u("T10 the commit disarm (`4 | 2 << 8`) restores A, right after the disarm itself",
             count(s, dis) == 1u && at(s, "did = n48_cm_shot_disarm(&gXdShot);") < at(s, dis), 1u);
    const std::string dm = fn_body(b, "void navi48_fm_disarm(void) {");
    expect_u("T10 navi48_fm_disarm restores under the lock with reason DISARM",
             count(dm, "if (gFm.on || gFm.engaged) fm_restore_locked(N48_FM_OFF_DISARM, 0u);") == 1u, 1u);
    // build 0.0.526 (switch 81, gfx_ks81.h): the withdrawal asks ks81_fm_withdrawal, which flags exactly as before unless 81
    // keeps flip mode through a KEYSTONE withdrawal with 73 ON (tests/gfx_ks81_test.cpp K4/K7).
    const char *wd = "        if (!(tokMatch && ksOk)) ks81_fm_withdrawal(tokMatch ? 1u : 0u, frHasPendingEntry ? 1u : 0u);";
    expect_u("T10 a withdrawal only flags (after the keystone answered), exactly once",
             count(s, wd) == 1u && at(s, wd) > at(s, "        const bool ksOk = frHasPendingEntry && commit_keystone_arm(wf, 1u, gXdCmGateSeq, kr, ks64);") &&
             count(s, "navi48_fm_note_withdrawal();") == 1u &&
             count(fn_body(s, "static __attribute__((noinline)) void ks81_fm_withdrawal(uint32_t tokMatch, uint32_t guardOk)"),
                   "        navi48_fm_note_withdrawal();\n") == 1u, 1u);
    expect_u("T10 the withdrawal hook is n48_fm_request_restore (a flag, no I/O)",
             count(b, "void navi48_fm_note_withdrawal(void) { (void)n48_fm_request_restore(&gFm, N48_FM_OFF_WITHDRAWAL); }"), 1u);
    // the restore copies B into A
    expect_u("T10 the restore copy is B -> A", count(b, "static uint32_t fm_io_copy_b_to_a(void *) { uint64_t us = 0; return fm_copy_linear(gFm.bOff, gFm.aOff, &us); }"), 1u);
    // the other scanout writers hold while ON
    const char *hold = "	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode;";
    const std::string cv = fn_body(b, "uint32_t navi48_scanout_copy_vram("), ct = fn_body(b, "uint32_t navi48_scanout_copy_tiled("),
                      cs = fn_body(b, "uint32_t navi48_scanout_copy_staged(");
    expect_u("T10 the legacy scanout writers (row, tiled, staged) each hold while flip mode is ON, before the lock",
             count(b, hold) == 3u && count(cv, hold) == 1u && count(ct, hold) == 1u && count(cs, hold) == 1u &&
             at(cv, hold) < at(cv, "	IOLockLock(gScanoutLock);") && at(ct, hold) < at(ct, "	IOLockLock(gScanoutLock);") &&
             at(cs, hold) < at(cs, "	IOLockLock(gScanoutLock);"), 1u);
    // the readers follow EARLIEST_INUSE
    const std::string sc = fn_body(b, "uint32_t navi48_scanout_control(uint64_t arg, uint64_t *out, unsigned count) {");
    const char *rd = "		if (mode == 3 || mode == 4) {\n			const uint64_t rd = fm_displayed_off(fbOff, g.fbLen);";
    expect_u("T10 the content read / FNV (3) and the thumbnail (4) read the displayed buffer, before either reads",
             count(sc, rd) == 1u && count(sc, "			fbOff = rd;\n") == 1u && at(sc, rd) < at(sc, "		if (mode == 3) {") &&
             at(sc, rd) < at(sc, "		if (mode == 4) {") && at(sc, rd) > at(sc, "		if (mode == 0) goto done;"), 1u);
    const std::string fdo = fn_body(b, "static uint64_t fm_displayed_off(uint64_t consoleOff, uint64_t consoleLen) {");
    expect_u("T10 fm_displayed_off answers B only when EARLIEST_INUSE names B",
             count(fdo, "return e == gFm.bMc ? gFm.bOff : consoleOff;") == 1u && count(fdo, "n48dcn::fmReadFront(&pending, &e)") == 1u, 1u);
    // the present-side copy goes through the two-destination plan; the flip through the bound device's exact set
    const std::string cto = fn_body(b, "static uint32_t fm_copy_tiled_to(");
    expect_u("T10 fm_copy_tiled_to plans with n48_scanout_plan_tiled_to and re-checks A/B at emission",
             count(cto, "n48_scanout_plan_tiled_to(&g, gFm.bOff, gFm.len, dstOff,") == 1u &&
             count(cto, "n48_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, plan.fbOff, &dstLen)") == 1u &&
             count(cto, "if (!n48_fm_dst_ok(&gFm, dstOff)) { v[0] = kScanStPlan; goto fin; }") == 1u, 1u);
    expect_u("T10 the flip is dcn41_hubp_program_flip(HUBP0, mc, vmid 0, tmz 0, immediate false) on the BOUND device",
             count(n, "	return dcn41_hubp_program_flip(&gDcn.d, 0u, mc, 0u, false, false);") == 1u &&
             count(n, "dcn41_dev_set_flip_exact(&gDcn.d, n ? ab : nullptr, n)") == 1u && count(n, "fmFlip") >= 1u &&
             count(fn_body(n, "int fmFlip(uint64_t mc, const char *tag) {"), "gLr") == 0u, 1u);
    // the test verb
    expect_u("T10 `dcnflip 1000 + N` dispatches the A/B test; dcnflip's own flip is refused while flip mode is ON",
             count(b, "	if (action == 76 && n48_fm_test_n(argScalar) != 0u) {") == 1u &&
             count(b, "	if (action == 76 && argScalar != 0u && navi48_fm_on()) {") == 1u &&
             at(b, "	if (action == 76 && argScalar != 0u && navi48_fm_on()) {") <
             at(b, "	if (action == 74 || action == 75 || action == 76 || action == 77) {"), 1u);
    const std::string tv = fn_body(b, "uint32_t navi48_fm_test(uint32_t n, uint64_t *out, unsigned count) {");
    // build 0.0.519
    expect_u("T10 (0.0.519 F4) the A/B test's CPU fill is followed by the HDP flush, after the store fence, before the flip",
             count(tv, "					amdgpu::storeFence();\n					amdgpu::amdgpu_hdp_flush(dev);") == 1u &&
             at(tv, "amdgpu::amdgpu_hdp_flush(dev);") < at(tv, "n48dcn::fmFlip(target, \"fmtest\")"), 1u);
    expect_u("T10 (0.0.519 F5) the CRC witness: enabled before the loop, read after each latch, disabled before the end flip",
             count(tv, "W.on = n48dcn::fmCrcBegin() == 0 ? 1u : 0u;") == 1u && count(tv, "n48dcn::fmCrcRead(2u, &crg, &cb)") == 1u &&
             count(tv, "if (W.on) n48dcn::fmCrcEnd();") == 1u &&
             at(tv, "W.on = n48dcn::fmCrcBegin()") < at(tv, "for (uint32_t i = 0; i < n && st == 0u; i++) {") &&
             at(tv, "n48dcn::fmCrcRead(2u, &crg, &cb)") > at(tv, "const uint32_t w = n48_fm_wait_latch(&o, N48_FM_RESTORE_WAIT_US, &e, &us, &polls);") &&
             at(tv, "if (W.on) n48dcn::fmCrcEnd();") < at(tv, "n48dcn::fmFlip(gFm.aMc, \"fmtestend\")") &&
             count(tv, "fmtest: CRC WITNESS %s") == 1u, 1u);
    expect_u("T10 (0.0.519 F5) the DCN CRC helpers are dcnflip's crc_enable/crc_read/crc_disable on OTG0",
             count(n, "	crc_enable(0u, aw, ah);") == 1u && count(n, "	crc_read(0u, rg, b);") == 1u && count(n, "	crc_disable(0u);") == 1u, 1u);
    expect_u("T10 (0.0.519 F2) B holds a present only after the engage's seed; the A/B test clears it before its fills",
             count(b, "	gFm.bHoldsPresent = seedB ? 1u : 0u;") == 1u && count(tv, "			gFm.bHoldsPresent = 0u;") == 1u &&
             at(tv, "			gFm.bHoldsPresent = 0u;") < at(tv, "px[k] = fill;"), 1u);
    expect_u("T10 (0.0.519 F3) the kext supplies a_settled (queue idle and the fence dword landed)",
             count(b, "	               fm_io_release, fm_io_a_settled };") == 1u &&
             count(b, "if (scanout_queue_check(dev, inst, &rptr) == kScanStOk && fence != 0u) { ok = 1u; break; }") == 1u, 1u);
    expect_u("T10 (0.0.520) while a failed restore left A/B engaged: ON is refused before any engage, and so is the A/B test",
             count(b, "		else if (gFm.engaged) { st = 12u; how = \" - ON REFUSED: a failed restore left A/B engaged (842 retries it), unchanged\"; }") == 1u &&
             at(b, "		else if (gFm.engaged) { st = 12u;") < at(b, "		else if (fm_engage_locked(true) != 0u) { st = 12u;") &&
             count(tv, "		else if (gFm.engaged) { st = 7u;") == 1u &&
             at(tv, "		else if (gFm.engaged) { st = 7u;") < at(tv, "fm_engage_locked(false)"), 1u);
    expect_u("T10 the A/B test refuses under an arm and while flip mode is ON, and ends on A verified",
             count(tv, "else if (n48::hw_cm_armed()) st = 2u;") == 1u && count(tv, "if (gFm.on) { st = 4u;") == 1u &&
             count(tv, "if (n48dcn::fmFlip(gFm.aMc, \"fmtestend\") != 0) continue;") == 1u, 1u);
}

int main(int argc, char **argv)
{
    std::printf("== T1 select ==\n"); t1_select();
    std::printf("== T2 run ==\n"); t2_run();
    std::printf("== T3 timeout ==\n"); t3_timeout();
    std::printf("== T4 foreign ==\n"); t4_foreign();
    std::printf("== T5 restore ==\n"); t5_restore();
    std::printf("== T6 ADDRESS_HIGH ==\n"); t6_high();
    std::printf("== T7 allowlists ==\n"); t7_allow();
    std::printf("== T8 OFF and failures ==\n"); t8_off();
    std::printf("== T9 lines ==\n"); t9_lines();
    std::printf("== T11 the 0.0.518 review (F1-F3) ==\n"); t11_review();
    std::printf("== T10 kext glue ==\n");
    t10_glue(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr,
             argc > 4 ? argv[4] : nullptr, argc > 5 ? argv[5] : nullptr);
    std::printf("gfx_flipmode: %d run, %d FAILED\n", gRun, gFail);
    return gFail ? 1 : 0;
}
