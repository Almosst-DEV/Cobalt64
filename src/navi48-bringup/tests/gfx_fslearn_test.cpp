// gfx_fslearn_test.cpp — build 0.0.548 (; apple/gfx_fillset.h n48_fs_learn): switch 33's LEARNED / SHADOW modes, the
// learned fill set, SWITCH 106 (`106 | M << 8`: 362 LEARNED, 618 OFF = TABLED, 874 SHADOW).
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/navi48-bringup/tests src/navi48-bringup/tests/gfx_fslearn_test.cpp -o /tmp/fsl && \
//         /tmp/fsl src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_fillset.h
// Covers:
//   L1  the REAL boots (fixture_fslearn_runs.h, gen_fslearn_fixture.py:'s 1080p run11aa..run11ao, run11ap-r, RUN AW run11aq,
//       RUN AX run11ar, plus run11a..run11z): each learns {P slot 0, P slot 4} of its own present73 record, in LEARNED and SHADOW;
//       0.0.549 (bound 30): the fallback fires on NO fixture boot and run11m (5th distinct CB0 at judged 22) learns without it; a
//       replay with the fills RESERVEd and committed and switch 35 ON modelled: the TOTAL refused (fill `refused` + `plane_refused`)
//       per boot, LEARNED never more than TABLED on any boot (a boot where it is is printed), fewer on RUN AX (whose twin drifted)
//   L2  the re-seat: a CB0 VA seen again (at a new VRAM) is not a new member (keyed by VA)
//   L3  a pid change resets the list (and LEARNED re-seats the open members UNLEARNED); frames of the new pid then learn
//   L4  the fallback fires at judged frame N48_FS_LEARN_BY (30) and that frame's FILL step PASSes (LEARNED); SHADOW records it and
//       changes nothing; 0.0.549 SHOULD: it also fires with all 5 learned while a member fill is RESERVEd but never committed
//   L10 0.0.549 MUST-FIX 2: at an UNKNOWN console size LEARNED seats members 0 and answers exactly as TABLED (everything PASSes)
//   L5  ORDERING: learn BEFORE the step - the frame that supplies member 1 is itself RESERVED, not refused (and member 0 on f1)
//   L6  SHADOW never changes a verdict or a byte of the fill-set state (random sequences, against a learner-free run)
//   L7  TABLED identity: the learner and its helpers touch nothing; n48_fs85_frame / commit / retire against a FROZEN 0.0.547
//       gfx_fillset.h + gfx_fs85.h (tests/frozen/*_01131813.h) over random scopes: every answer and every state byte equal; and
//       n48_fs85_member_of's new member-1 clause adds nothing while TABLED (the seated VA is twin slot 0)
//   L8  the would-differ count and the refused split; every new line <= 491 bytes at its widest
//   L9  source pins: the switch (TABLED at boot; M 1/2/3; the mid-arm guard), the glue's ORDER in gfxsrc_commit_try, gFsLJ set before
//       commit_try, the arm's open, the STOP line
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "gfx_fs85.h"
#include "gfx_present73.h"
#include "gfx_commit.h"
namespace fs547 {
#include "frozen/gfx_fs85_01131813.h"
}
#include "fixture_fslearn_runs.h"

static int gFail = 0, gPass = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    if (got == want) { gPass++; return; }
    gFail++;
    std::printf("FAIL %s: got %llu (%#llx), want %llu (%#llx)\n", what, (unsigned long long)got, (unsigned long long)got,
                (unsigned long long)want, (unsigned long long)want);
}
static uint64_t gRng = 0x2545f4914f6cdd1dull;
static uint32_t rnd(uint32_t n) { gRng ^= gRng << 13; gRng ^= gRng >> 7; gRng ^= gRng << 17; return (uint32_t)(gRng % n); }

static void armed(n48_fs *f, n48_fs_lrn *L, uint32_t mode, uint32_t geo)
{
    std::memset(f, 0, sizeof *f); std::memset(L, 0, sizeof *L);
    f->on = 1u; f->plane_on = 1u; L->mode = mode;
    n48_fs_open(f); n48_fs_seat_geo(f, geo);
    if (mode != N48_FS_MODE_TABLED) { n48_fs_learn_open(L); n48_fs_learn_seat(f, L); }
}
/* ONE frame in the kext's order: the learner (not TABLED), THEN n48_fs85_frame (85 off) - the fill step. Returns the fill step. */
static uint32_t frame_in_order(n48_fs *f, n48_fs_lrn *L, uint32_t pshape, uint32_t plane, uint64_t cb0, int32_t pid, uint32_t fr,
                               uint32_t eligible, uint32_t nseg, uint32_t ps_fill)
{
    if (L->mode != N48_FS_MODE_TABLED) {
        uint32_t idx = 0u;
        (void)n48_fs_learn(f, L, pshape, plane, cb0, pid, fr, &idx);
        (void)n48_fs_learn_compare(f, L, f->geo, eligible, (nseg == 1u && ps_fill) ? 1u : 0u, cb0, fr);
    }
    n48_fs85 x {}; n48_fs85_frame_out o {};
    n48_fs85_frame(f, &x, 0u, 1u, 1u, eligible, nseg, ps_fill, plane, cb0, 0u, &o);
    if (o.fs_step == N48_FS_REFUSE) n48_fs_learn_note_refuse(L, pshape, 1u);
    return o.fs_step;
}

// =====================================================================================================================
// L1 — the real boots.
// =====================================================================================================================
static void l1_real_boots()
{
    const uint32_t nruns = (uint32_t)(sizeof kFslRuns / sizeof kFslRuns[0]);
    uint32_t learned = 0u, n1080 = 0u, n1440 = 0u, fbOk = 1u, fallbacks = 0u, notWorse = 1u, better = 0u, m11 = 0u;
    uint64_t totT = 0ull, totL = 0ull, totPl = 0ull;
    for (uint32_t r = 0u; r < nruns; r++) {
        const FslRun &R = kFslRuns[r];
        if (R.n == 0u || R.nslots < 5u) { std::printf("  L1 %s: no frames/slots (skipped)\n", R.name); continue; }
        (R.p1440 ? n1440 : n1080)++;
        const uint32_t geo = R.p1440 ? N48_FS_GEO_1440 : N48_FS_GEO_1080;
        uint32_t okBoth = 1u, fbAt = 0u;
        for (uint32_t mode = N48_FS_MODE_LEARNED; mode <= N48_FS_MODE_SHADOW; mode++) {
            n48_fs f; n48_fs_lrn L; armed(&f, &L, mode, geo);
            for (uint32_t i = 0u; i < R.n; i++) {
                const FslFrame &F = R.f[i];
                (void)frame_in_order(&f, &L, n48_p73_is_p_shape(1u, F.nib, F.len0), 0u, F.cb0, F.pid, F.frame, 0u, 1u, 0u);
            }
            if (n48_fs_learn_member(&L, 0u) != R.slot[0] || n48_fs_learn_member(&L, 1u) != R.slot[4]) {
                okBoth = 0u;
                std::printf("  L1 %s mode %u: learned %#llx %#llx, slots %#llx %#llx\n", R.name, mode,
                            (unsigned long long)n48_fs_learn_member(&L, 0u), (unsigned long long)n48_fs_learn_member(&L, 1u),
                            (unsigned long long)R.slot[0], (unsigned long long)R.slot[4]);
            }
            fbAt = L.fallback ? L.fallback_at : 0u;
        }
        learned += okBoth;
        if (fbAt) { fallbacks++; fbOk = 0u; std::printf("  L1 %s: FALLBACK at %u (unexpected at bound %u)\n", R.name, fbAt, N48_FS_LEARN_BY); }
        /* The replay: a P-shaped frame whose CB0 is new is the surface's one-time ColorFill; everything is eligible; a
         * RESERVE commits at once. Count the start-up frames refused under the reservation, TABLED vs LEARNED. */
        uint64_t ref[2] = { 0ull, 0ull }, fref[2] = { 0ull, 0ull }, pref[2] = { 0ull, 0ull };
        for (uint32_t mi = 0u; mi < 2u; mi++) {
            n48_fs f; n48_fs_lrn L; armed(&f, &L, mi ? N48_FS_MODE_LEARNED : N48_FS_MODE_TABLED, geo);
            uint64_t seen[64]; uint32_t ns = 0u;
            for (uint32_t i = 0u; i < R.n; i++) {
                const FslFrame &F = R.f[i];
                const uint32_t ps = n48_p73_is_p_shape(1u, F.nib, F.len0);
                uint32_t fill = 0u;
                if (ps) { fill = 1u; for (uint32_t k = 0u; k < ns; k++) if (seen[k] == F.cb0) fill = 0u; if (fill && ns < 64u) seen[ns++] = F.cb0; }
                const uint32_t v = frame_in_order(&f, &L, ps, 0u, F.cb0, F.pid, F.frame, 1u, ps ? 1u : 2u, fill);
                if (v == N48_FS_RESERVE) (void)n48_fs_commit(&f, F.cb0, i + 1u);
            }
            /* 0.0.549 MUST-FIX 3: the TOTAL - the fill window's refusals AND the plane window's (35 ON: plane_on 1 in armed(), the
             * plane step in n48_fs85_frame), which opens as soon as the fill window closes or is lifted */
            fref[mi] = f.refused; pref[mi] = f.plane_refused;
            ref[mi] = (uint64_t)f.refused + (uint64_t)f.plane_refused;
        }
        std::printf("  L1 %-9s total refused TABLED %llu (fill %llu + plane %llu), LEARNED %llu (fill %llu + plane %llu)%s\n", R.name,
                    (unsigned long long)ref[0], (unsigned long long)fref[0], (unsigned long long)pref[0], (unsigned long long)ref[1],
                    (unsigned long long)fref[1], (unsigned long long)pref[1], ref[1] > ref[0] ? "  <-- LEARNED > TABLED" : "");
        if (ref[1] > ref[0]) { notWorse = 0u; std::printf("  L1 %s: LEARNED refused %llu > TABLED %llu (by %llu)\n", R.name, (unsigned long long)ref[1], (unsigned long long)ref[0], (unsigned long long)(ref[1] - ref[0])); }
        if (ref[1] < ref[0]) better++;
        totT += ref[0]; totL += ref[1]; totPl += pref[0] + pref[1];
        if (!std::strcmp(R.name, "run11m")) {
            n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_LEARNED, geo);
            uint32_t j5 = 0u;
            for (uint32_t i = 0u; i < R.n; i++) {
                const FslFrame &F = R.f[i];
                (void)frame_in_order(&f, &L, n48_p73_is_p_shape(1u, F.nib, F.len0), 0u, F.cb0, F.pid, F.frame, 0u, 1u, 0u);
                if (!j5 && L.n == 5u) j5 = f.judged;   /* the fill-set judged number of the frame that supplied member 1 */
            }
            std::printf("  L1 run11m: 5th distinct CB0 %#llx at frame %u = fill-set judged %u; fallback %u\n", (unsigned long long)L.va[4],
                        L.at[4], j5, L.fallback);
            m11 = (!L.fallback && L.n == 5u && j5 == 22u && n48_fs_learn_member(&L, 1u) == R.slot[4]) ? 1u : 0u;
        }
        if (!std::strcmp(R.name, "run11ar")) expect_u("L1 RUN AX: LEARNED refuses fewer start-up frames than TABLED", ref[1] < ref[0] ? 1u : 0u, 1u);
    }
    std::printf("  L1 boots: %u x 1080p + %u x 1440p; learned {slot 0, slot 4} on %u; fallbacks %u; LEARNED refused fewer on %u\n",
                n1080, n1440, learned, fallbacks, better);
    expect_u("L1 at least 16 x 1080p and the three 1440p boots are in the fixture", n1080 >= 16u && n1440 == 3u ? 1u : 0u, 1u);
    expect_u("L1 EVERY boot learns {P slot 0, P slot 4} (its own present73 record), in LEARNED and in SHADOW", learned, n1080 + n1440);
    std::printf("  L1 totals over the fixture: TABLED %llu, LEARNED %llu (plane_refused in them: %llu)\n", (unsigned long long)totT,
                (unsigned long long)totL, (unsigned long long)totPl);
    expect_u("L1 (0.0.549, bound 30) the fallback fires on NO fixture boot", fbOk && fallbacks == 0u ? 1u : 0u, 1u);
    expect_u("L1 (0.0.549) run11m: its 5th distinct CB0 (judged 22) is LEARNED as member 1, with no fallback", m11, 1u);
    expect_u("L1 the replay with 35 ON modelled: LEARNED never refuses more frames in TOTAL (fill + plane_refused) than TABLED", notWorse, 1u);
    expect_u("L1 the replay exercises the plane window (plane_refused > 0 somewhere)", totPl > 0ull ? 1u : 0u, 1u);
    /* GOLDEN (0.0.549, the frozen fixture, 24 logged frames per boot): the totals include plane_refused (without it they read
     * TABLED 424 / LEARNED 407). A change to the replay, the bound or the counting moves them. */
    expect_u("L1 GOLDEN totals over the 43 boots: TABLED 948, LEARNED 946 (fill + plane_refused)", totT == 948ull && totL == 946ull ? 1u : 0u, 1u);
}

// =====================================================================================================================
// L2 — the re-seat; L3 — the pid change; L4 — the fallback; L5 — ordering.
// =====================================================================================================================
static const uint64_t A = 0x400100000ull, B = 0x401100000ull, C = 0x402100000ull, D = 0x403000000ull, E = 0x403f00000ull,
                      G = 0x406700000ull;
static void l2_to_l5()
{
    {   // L2
        n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        const uint64_t seq[] = { A, B, A /* re-seated at a new VRAM: the same VA */, C, D, A, E, G };
        for (uint32_t i = 0u; i < 8u; i++) (void)frame_in_order(&f, &L, 1u, 0u, seq[i], 7, i + 1u, 0u, 1u, 0u);
        expect_u("L2 re-seat: the same VA again (new VRAM) is not a new member; member 1 is the 5th DISTINCT VA (E, not D or G)",
                 L.n == 5u && n48_fs_learn_member(&L, 0u) == A && n48_fs_learn_member(&L, 1u) == E && f.member_va[1] == E ? 1u : 0u, 1u);
        /* non-P frames, plane frames and a frame with no target are never counted */
        n48_fs g; n48_fs_lrn M; armed(&g, &M, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        (void)frame_in_order(&g, &M, 0u, 0u, 0x402000000ull, 7, 1u, 0u, 1u, 0u);   /* 1456-dw headless: not P-shaped */
        (void)frame_in_order(&g, &M, 1u, 1u, 0x405000000ull, 7, 2u, 0u, 1u, 0u);   /* a plane frame */
        (void)frame_in_order(&g, &M, 1u, 0u, 0ull, 7, 3u, 0u, 1u, 0u);             /* no colour target */
        expect_u("L2 a non-P frame, a PLANE frame and a target-less frame are not counted", M.n, 0u);
    }
    {   // L3
        n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        (void)frame_in_order(&f, &L, 1u, 0u, 0x400800000ull, 100, 1u, 0u, 1u, 0u);
        (void)frame_in_order(&f, &L, 1u, 0u, 0x401800000ull, 100, 2u, 0u, 1u, 0u);
        const uint64_t m0old = f.member_va[0];
        const uint64_t seq[] = { A, B, C, D, E };
        uint32_t ev = 0u, idx = 0u;
        ev = n48_fs_learn(&f, &L, 1u, 0u, seq[0], 200, 3u, &idx);
        const uint32_t firstOk = (ev & N48_FS_LEV_RESET) && (ev & N48_FS_LEV_NEW) && idx == 1u && L.resets == 1u && L.pid == 200 &&
                                 f.member_va[0] == A && f.member_va[1] == N48_FS_UNLEARNED ? 1u : 0u;
        for (uint32_t i = 1u; i < 5u; i++) (void)frame_in_order(&f, &L, 1u, 0u, seq[i], 200, 4u + i, 0u, 1u, 0u);
        expect_u("L3 a pid change RESETS the list (the old pid's 0x400800000 is gone); the new pid's frames learn {A, E}",
                 m0old == 0x400800000ull && firstOk && n48_fs_learn_member(&L, 0u) == A && n48_fs_learn_member(&L, 1u) == E ? 1u : 0u, 1u);
        /* a committed member is never re-seated by a reset */
        n48_fs g; n48_fs_lrn M; armed(&g, &M, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        (void)frame_in_order(&g, &M, 1u, 0u, A, 100, 1u, 1u, 1u, 1u);
        (void)n48_fs_commit(&g, A, 1u);
        (void)frame_in_order(&g, &M, 1u, 0u, B, 200, 2u, 0u, 1u, 0u);
        expect_u("L3 a member already COMMITTED keeps its VA across a reset", g.member_committed[0] && g.member_va[0] == A ? 1u : 0u, 1u);
    }
    {   // L4
        for (uint32_t mode = N48_FS_MODE_LEARNED; mode <= N48_FS_MODE_SHADOW; mode++) {
            n48_fs f; n48_fs_lrn L; armed(&f, &L, mode, N48_FS_GEO_1440);
            const uint64_t three[] = { A, B, C };
            const uint32_t BY = N48_FS_LEARN_BY;
            uint32_t refused = 0u, vBy = 99u, vAfter = 99u, plRef = 0u;
            for (uint32_t fr = 1u; fr <= BY + 5u; fr++) {
                const uint32_t v = frame_in_order(&f, &L, 1u, 0u, three[fr % 3u], 7, fr, 1u, 2u, 0u);   /* eligible, never a fill */
                if (fr < BY && v == N48_FS_REFUSE) refused++;
                if (fr == BY) { vBy = v; plRef = f.plane_refused; }
                if (fr == BY + 1u) vAfter = v;
            }
            if (mode == N48_FS_MODE_LEARNED)
                expect_u("L4 LEARNED: 3 distinct CB0s by judged 30 -> the FALLBACK fires AT 30 and that frame's FILL step PASSes (and every "
                         "later one's); frames 1..29 refused as today; 35 ON: the lifted fill window opens the PLANE window, which refuses "
                         "the non-plane frame at 30 (RESERVED-FOR-PLANE)", N48_FS_LEARN_BY == 30u && L.fallback && L.fallback_at == 30u &&
                         vBy == N48_FS_PASS && vAfter == N48_FS_PASS && refused == 29u && f.expired == 1u && f.expiry == 0u && plRef == 1u &&
                         f.plane_refused == 6u ? 1u : 0u, 1u);
            else
                expect_u("L4 SHADOW: the fallback is recorded at 30 and nothing changes (frame 30 still REFUSED)",
                         L.fallback && L.fallback_at == 30u && vBy == N48_FS_REFUSE && f.expired == 0u && f.plane_refused == 0u ? 1u : 0u, 1u);
        }
        /* 5 distinct by judged 30 (the 5th at 22, run11m's): no fallback */
        {
            n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
            const uint64_t five[] = { A, B, C, D, E };
            for (uint32_t fr = 1u; fr <= 29u; fr++) (void)frame_in_order(&f, &L, 1u, 0u, five[fr < 22u ? (fr % 4u) : 4u], 7, fr, 0u, 1u, 0u);
            expect_u("L4 the 5th distinct CB0 at judged 22 learns member 1: no fallback", !L.fallback && n48_fs_learn_member(&L, 1u) == E ? 1u : 0u, 1u);
        }
        /* 0.0.549 SHOULD: all 5 learned, member 1's fill RESERVEd but refused by an earlier rung (never committed, never retired): the
         * fill window would be held to 60 (RUN AX's pattern); the fallback fires at the bound and lifts it. */
        for (uint32_t mode = N48_FS_MODE_LEARNED; mode <= N48_FS_MODE_SHADOW; mode++) {
            n48_fs f; n48_fs_lrn L; armed(&f, &L, mode, N48_FS_GEO_1440);
            /* the 1st and 5th ARE the table's 1440p members, so SHADOW (the table seated) reserves the same two frames */
            const uint64_t t0 = n48_fs_member_va_of(N48_FS_GEO_1440, 0u), t1 = n48_fs_member_va_of(N48_FS_GEO_1440, 1u);
            const uint64_t five[] = { t0, B, C, D, t1 };
            uint32_t r1 = 99u, r5 = 99u, vBy = 99u;
            for (uint32_t fr = 1u; fr <= 5u; fr++) {
                const uint32_t v = frame_in_order(&f, &L, 1u, 0u, five[fr - 1u], 7, fr, 1u, 1u, 1u);   /* each its surface's fill */
                if (fr == 1u) { r1 = v; if (v == N48_FS_RESERVE) (void)n48_fs_commit(&f, t0, 1u); }
                if (fr == 5u) r5 = v;   /* RESERVEd, but the gate refused it on an earlier rung: no commit, no retire */
            }
            for (uint32_t fr = 6u; fr <= N48_FS_LEARN_BY + 3u; fr++) {
                const uint32_t v = frame_in_order(&f, &L, 1u, 0u, t0, 7, fr, 1u, 2u, 0u);
                if (fr == N48_FS_LEARN_BY) vBy = v;
            }
            const uint32_t open5 = L.n == 5u && r1 == N48_FS_RESERVE && r5 == N48_FS_RESERVE && !f.member_committed[1] && !f.member_retired[1];
            if (mode == N48_FS_MODE_LEARNED)
                expect_u("L4 (0.0.549 SHOULD) LEARNED: all 5 learned, member 1 RESERVEd but never committed -> the fallback fires AT 30 "
                         "and lifts the fill window (not held to 60)", open5 && L.fallback && L.fallback_at == N48_FS_LEARN_BY &&
                         vBy == N48_FS_PASS && f.expired == 1u && !n48_fs_win_open(&f) ? 1u : 0u, 1u);
            else
                expect_u("L4 (0.0.549 SHOULD) SHADOW: the same fallback is recorded at 30, nothing lifted",
                         open5 && L.fallback && L.fallback_at == N48_FS_LEARN_BY && vBy == N48_FS_REFUSE && f.expired == 0u &&
                         n48_fs_win_open(&f) ? 1u : 0u, 1u);
        }
    }
    {   // L5
        n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        const uint32_t v1 = frame_in_order(&f, &L, 1u, 0u, A, 7, 1u, 1u, 1u, 1u);
        if (v1 == N48_FS_RESERVE) (void)n48_fs_commit(&f, A, 1u);
        const uint64_t mid[] = { B, C, D };
        for (uint32_t i = 0u; i < 3u; i++) (void)frame_in_order(&f, &L, 1u, 0u, mid[i], 7, 2u + i, 1u, 1u, 1u);
        const uint32_t v5 = frame_in_order(&f, &L, 1u, 0u, E, 7, 5u, 1u, 1u, 1u);
        expect_u("L5 ORDERING: f1 (supplies member 0) and the frame that supplies member 1 are each RESERVED on that same frame",
                 v1 == N48_FS_RESERVE && v5 == N48_FS_RESERVE ? 1u : 0u, 1u);
        /* the wrong order (step, then learn) would have REFUSED that frame */
        n48_fs g; n48_fs_lrn M; armed(&g, &M, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        const uint64_t all[] = { A, B, C, D };
        for (uint32_t i = 0u; i < 4u; i++) (void)frame_in_order(&g, &M, 1u, 0u, all[i], 7, 1u + i, 0u, 1u, 0u);
        const uint32_t idf = n48_fs_identify_fill(&g, 1u, 1u, E);
        const uint32_t wrong = n48_fs_step(&g, 1u, idf, E);
        expect_u("L5 the mutant order (step before learn) REFUSES the member-1 frame", wrong, (uint32_t)N48_FS_REFUSE);
    }
}

// =====================================================================================================================
// L6 SHADOW; L7 TABLED identity.
// =====================================================================================================================
static uint64_t rnd_cb0()
{
    static const uint64_t v[] = { A, B, C, D, E, G, 0x404000000ull, 0x400800000ull, 0x404800000ull, 0x405800000ull, 0ull };
    return v[rnd(sizeof v / sizeof v[0])];
}
static void l6_l7()
{
    uint32_t mismS = 0u, mismT = 0u, frames = 0u, reserves = 0u, refuses = 0u, commits = 0u, retires = 0u, touched = 0u;
    while (frames < 60000u) {
        const uint32_t geo = rnd(3), pidA = 50 + rnd(3);
        n48_fs s, t; n48_fs_lrn Ls, Lt; armed(&s, &Ls, N48_FS_MODE_SHADOW, geo); armed(&t, &Lt, N48_FS_MODE_TABLED, geo);
        fs547::n48_fs z; std::memset(&z, 0, sizeof z); z.on = 1u; z.plane_on = 1u; fs547::n48_fs_open(&z); fs547::n48_fs_seat_geo(&z, geo);
        n48_fs85 xs {}, xt {}; fs547::n48_fs85 xz {};
        const uint32_t on85 = rnd(3) == 0u;
        xs.on = xt.on = xz.on = on85;
        n48_fs85_open(&xs); n48_fs85_open(&xt); fs547::n48_fs85_open(&xz);
        const uint32_t len = 10u + rnd(120);
        for (uint32_t i = 0u; i < len; i++, frames++) {
            const uint32_t el = rnd(3) != 0u, nseg = 1u + rnd(2) * rnd(3), psf = rnd(2), psp = rnd(4) == 0u, ps = rnd(3) != 0u;
            const uint32_t left = rnd(5), live = rnd(2), depf = rnd(2);
            const int32_t pid = rnd(40) == 0u ? (int32_t)(pidA + 1u) : (int32_t)pidA;
            const uint64_t cb0 = rnd_cb0();
            /* SHADOW: the learner runs first (as the kext); TABLED: a learn call must be a no-op (the kext does not even make it) */
            uint32_t idx = 0u;
            (void)n48_fs_learn(&s, &Ls, ps, psp, cb0, pid, frames, &idx);
            (void)n48_fs_learn_compare(&s, &Ls, s.geo, el, (nseg == 1u && psf) ? 1u : 0u, cb0, frames);
            n48_fs tb = t; n48_fs_lrn Ltb = Lt;
            if (n48_fs_learn(&t, &Lt, ps, psp, cb0, pid, frames, &idx) || n48_fs_learn_compare(&t, &Lt, t.geo, el, 1u, cb0, frames)) touched++;
            n48_fs_learn_note_refuse(&Lt, ps, 2u);
            if (std::memcmp(&tb, &t, sizeof t) || std::memcmp(&Ltb, &Lt, sizeof Lt)) touched++;
            const uint32_t onS = n48_fs85_active(&s, &xs), onT = n48_fs85_active(&t, &xt), onZ = fs547::n48_fs85_active(&z, &xz);
            n48_fs85_frame_out os {}, ot {}; fs547::n48_fs85_frame_out oz {};
            n48_fs85_frame(&s, &xs, onS, 1u, 1u, el, nseg, psf, psp, cb0, left, &os);
            n48_fs85_frame(&t, &xt, onT, 1u, 1u, el, nseg, psf, psp, cb0, left, &ot);
            fs547::n48_fs85_frame(&z, &xz, onZ, 1u, 1u, el, nseg, psf, psp, cb0, left, &oz);
            if (os.is_fill != oz.is_fill || os.fs_step != oz.fs_step || os.fp_step != oz.fp_step || os.s3 != oz.s3) mismS++;
            if (ot.is_fill != oz.is_fill || ot.fs_step != oz.fs_step || ot.fp_step != oz.fp_step || ot.s3 != oz.s3) mismT++;
            reserves += oz.fs_step == fs547::N48_FS_RESERVE; refuses += oz.fs_step == fs547::N48_FS_REFUSE;
            if (oz.fs_step == fs547::N48_FS_RESERVE && !live) {
                const uint32_t a = n48_fs85_retire(&s, &xs, cb0, 0u, depf, 7u, 3u), b = n48_fs85_retire(&t, &xt, cb0, 0u, depf, 7u, 3u);
                const uint32_t c = fs547::n48_fs85_retire(&z, &xz, cb0, 0u, depf, 7u, 3u);
                if (a != c) mismS++;
                if (b != c) mismT++;
                retires += c;
            } else if (oz.fs_step == fs547::N48_FS_RESERVE) {
                const uint32_t a = n48_fs85_commit(&s, &xs, cb0, frames), b = n48_fs85_commit(&t, &xt, cb0, frames);
                const uint32_t c = fs547::n48_fs85_commit(&z, &xz, cb0, frames);
                if (a != c) mismS++;
                if (b != c) mismT++;
                commits += c;
            }
            if (std::memcmp(&s, &z, sizeof s)) mismS++;
            if (std::memcmp(&t, &z, sizeof t)) mismT++;
            if (std::memcmp(&xs, &xz, sizeof xs) || std::memcmp(&xt, &xz, sizeof xt)) { mismS++; mismT++; }
        }
    }
    std::printf("  L6/L7 %u frames: reserves %u, refuses %u, commits %u, retires %u\n", frames, reserves, refuses, commits, retires);
    static_assert(sizeof(n48_fs) == sizeof(fs547::n48_fs), "the fill-set state's layout is 0.0.547's");
    static_assert(sizeof(n48_fs85) == sizeof(fs547::n48_fs85), "switch 85's state layout is 0.0.547's");
    expect_u("L6 SHADOW never changes a verdict or a byte of the fill-set / 85 state (vs FROZEN 0.0.547, 85 random)", mismS, 0u);
    expect_u("L7 TABLED: every answer and every state byte equal FROZEN 0.0.547's (85 random)", mismT, 0u);
    expect_u("L7 TABLED: n48_fs_learn / _compare / _note_refuse touch nothing and answer 0", touched, 0u);
    expect_u("L7 the random sequences exercise RESERVE, REFUSE, commit and retire", reserves && refuses && commits && retires ? 1u : 0u, 1u);
    expect_u("L7 the new member-1 clause adds nothing TABLED: the seated VA is twin slot 0 of its size",
             n48_fs_member_va_of(N48_FS_GEO_1080, 1u) == n48_fs85_twin_of(N48_FS_GEO_1080, 0u) &&
             n48_fs_member_va_of(N48_FS_GEO_1440, 1u) == n48_fs85_twin_of(N48_FS_GEO_1440, 0u) &&
             n48_fs_member_va_of(N48_FS_GEO_UNKNOWN, 1u) == 0ull ? 1u : 0u, 1u);
    {   /* LEARNED + 85: member 1 matches the learned VA (and never the UNLEARNED sentinel) */
        n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_LEARNED, N48_FS_GEO_1440);
        const uint32_t before = n48_fs85_member_of(&f, 1u, N48_FS_UNLEARNED);
        const uint64_t five[] = { A, B, C, D, E };
        for (uint32_t i = 0u; i < 5u; i++) (void)frame_in_order(&f, &L, 1u, 0u, five[i], 7, i + 1u, 0u, 1u, 0u);
        expect_u("L7 LEARNED + 85: member 1 matches its learned VA; the UNLEARNED sentinel never matches",
                 before == 0u && n48_fs85_member_of(&f, 1u, E) == 1u && n48_fs85_member_of(&f, 1u, N48_FS_UNLEARNED) == 0u ? 1u : 0u, 1u);
    }
}

// =====================================================================================================================
// L8 would-differ, the refused split, the line widths.
// =====================================================================================================================
static void l8()
{
    n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_SHADOW, N48_FS_GEO_1440);
    const FslRun *ar = nullptr;
    for (const FslRun &R : kFslRuns) if (!std::strcmp(R.name, "run11ar")) ar = &R;
    uint64_t seen[64]; uint32_t ns = 0u;
    for (uint32_t i = 0u; ar && i < ar->n; i++) {
        const FslFrame &F = ar->f[i];
        const uint32_t ps = n48_p73_is_p_shape(1u, F.nib, F.len0);
        uint32_t fill = 0u;
        if (ps) { fill = 1u; for (uint32_t k = 0u; k < ns; k++) if (seen[k] == F.cb0) fill = 0u; if (fill && ns < 64u) seen[ns++] = F.cb0; }
        uint32_t idx = 0u;
        (void)n48_fs_learn(&f, &L, ps, 0u, F.cb0, F.pid, F.frame, &idx);
        (void)n48_fs_learn_compare(&f, &L, f.geo, 1u, fill, F.cb0, F.frame);
        const uint32_t v = n48_fs_step(&f, 1u, n48_fs_identify_fill(&f, ps ? 1u : 2u, fill, F.cb0), F.cb0);
        if (v == N48_FS_REFUSE) n48_fs_learn_note_refuse(&L, ps, F.nib);
        if (v == N48_FS_RESERVE) (void)n48_fs_commit(&f, F.cb0, i + 1u);
    }
    std::printf("  L8 run11ar SHADOW: would-differ %u (first %u %u %u), refused single %llu multi %llu other %llu\n", L.wd,
                L.wd_first[0], L.wd_first[1], L.wd_first[2], (unsigned long long)L.ref_single, (unsigned long long)L.ref_multi,
                (unsigned long long)L.ref_other);
    expect_u("L8 RUN AX in SHADOW: the table and the learned set differ first on f15 (the twin's own fill: RESERVE vs REFUSE)",
             ar && L.wd >= 1u && L.wd_first[0] == 15u ? 1u : 0u, 1u);
    expect_u("L8 the refused split sums to the fill set's refused count, and multi-IB start-up frames are among them",
             L.ref_single + L.ref_multi + L.ref_other == f.refused && L.ref_multi > 0u ? 1u : 0u, 1u);
    /* widths */
    char b[2048]; uint32_t ok = 1u; int n;
    n = std::snprintf(b, sizeof b, N48_FSL_EVENT_FMT, "LEARNED", 4294967295u, ~0ull, 4294967295u, 4294967295u, -2147483647 - 1, " = member 1");
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  EVENT %d\n", n); }
    n = std::snprintf(b, sizeof b, N48_FSL_RESET_FMT, "LEARNED", -2147483647 - 1, 4294967295u, 4294967295u);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  RESET %d\n", n); }
    n = std::snprintf(b, sizeof b, N48_FSL_FALLBACK_FMT, "LEARNED", 4294967295u, 4294967295u, 4294967295u, N48_FSL_LIFTED_WORD);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  FALLBACK %d\n", n); }
    n = std::snprintf(b, sizeof b, N48_FSL_STOP_FMT, "LEARNED", ~0ull, ~0ull, 4294967295u, -2147483647 - 1, 4294967295u, ~0ull, ~0ull,
                      4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u,
                      "FIRED (window lifted)", 4294967295u, ~0ull, ~0ull, ~0ull, ~0ull);
    std::printf("  L8 STOP line at its widest: %d bytes\n", n);
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  STOP %d\n", n); }
    n = std::snprintf(b, sizeof b, N48_FSL_MODE_FMT, "OFF (TABLED, default)", "OFF - 106 does nothing", "`gfxneuter 106` REFUSED (unknown M), unchanged");
    if (n < 0 || (uint32_t)n > N48_LOG_CAP_BODY) { ok = 0u; std::printf("  MODE %d\n", n); }
    expect_u("L8 every fslearn line fits 491 bytes at its widest", ok, 1u);
}

// =====================================================================================================================
// L9 source pins.
// =====================================================================================================================
static std::string read_file(const char *p)
{
    std::string s; FILE *fp = p ? std::fopen(p, "rb") : nullptr;
    if (!fp) return s;
    char buf[65536]; size_t k;
    while ((k = std::fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, k);
    std::fclose(fp);
    return s;
}
static uint32_t count(const std::string &s, const char *n)
{
    uint32_t c = 0u; size_t p = 0u; const size_t l = std::strlen(n);
    while ((p = s.find(n, p)) != std::string::npos) { c++; p += l; }
    return c;
}
static std::string body_of(const std::string &s, const char *head)
{
    const size_t p = s.find(head);
    if (p == std::string::npos) return std::string();
    size_t b = s.find('{', p); int d = 0;
    for (size_t i = b; i < s.size(); i++) {
        if (s[i] == '{') d++;
        else if (s[i] == '}' && --d == 0) return s.substr(p, i + 1 - p);
    }
    return std::string();
}
static void l9_pins(const std::string &src, const std::string &hdr)
{
    expect_u("PIN the kext and gfx_fillset.h were read", src.size() > 1000000u && count(hdr, "n48_fs_learn(") >= 1u ? 1u : 0u, 1u);
    expect_u("PIN the learner state is defined once, all-zero (mode TABLED at boot)", count(src, "static n48_fs_lrn gFsL {};") == 1u &&
             count(src, "static n48_fs_lrn gFsL") == 1u && N48_FS_MODE_TABLED == 0u ? 1u : 0u, 1u);
    expect_u("PIN gFsL.mode's only writer is the 106 verb", count(src, "gFsL.mode = ") == 1u && count(src, "gFsL.mode = want; changed106 = 1;") == 1u ? 1u : 0u, 1u);
    const std::string verb = body_of(src, "} else if ((arg & 0xffull) == 106ull) {");
    expect_u("PIN the 106 verb: M 1 LEARNED, M 2 TABLED (OFF), M 3 SHADOW; refused while a continuous arm (the guard) or any arm scope stands; "
             "an unknown M refused", count(verb, "const uint32_t want = m == 1u ? (uint32_t)N48_FS_MODE_LEARNED : m == 2u ? (uint32_t)N48_FS_MODE_TABLED") == 1u &&
             count(verb, ": m == 3u ? (uint32_t)N48_FS_MODE_SHADOW : (uint32_t)N48_FS_MODES;") == 1u &&
             count(verb, "const bool contRefused106 = n48_cm_cont_switch_refused(106u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") == 1u &&
             count(verb, "const bool scopeRefused106 = m != 0u && gFs.opened != 0u;") == 1u &&
             verb.find("} else if (contRefused106 || scopeRefused106) {") < verb.find("gFsL.mode = want;") &&
             n48_cm_cont_switch_guarded(106u) == 1u && (106u | 1u << 8) == 362u && (106u | 2u << 8) == 618u && (106u | 3u << 8) == 874u ? 1u : 0u, 1u);
    const std::string v33 = body_of(src, "} else if ((arg & 0xffull) == 33ull) {");
    expect_u("PIN switch 33's verb is 0.0.547's (no mode in it)", count(v33, "gFsL") == 0u && count(v33, "} else if (m == 1u) {\n            gFs.on = 1u; changed = 1;") == 1u ? 1u : 0u, 1u);
    const std::string ct = body_of(src, "static uint32_t gfxsrc_commit_try(");
    const char *learnLine = "    if (fsActive && gFsL.mode != N48_FS_MODE_TABLED) fsl_frame(tgtVa, fsEligible, gXdBuild.nsegPre, gXdBuild.fill, gXdBuild.plane);";
    const size_t pl = ct.find(learnLine), ps = ct.find("    n48_fs85_frame(&gFs, &gFs85, fs85On, fsActive, fpActive, fsEligible,");
    const size_t pa = ct.find("    const uint32_t fsActive = (gFs.on && arm == N48_SD_ARM_COMMIT) ? 1u : 0u;");
    const size_t pr = ct.find("    if (fsStep == N48_FS_REFUSE) n48_fs_learn_note_refuse(&gFsL, gFsLJ.pShape, nib);");
    expect_u("PIN ORDER in gfxsrc_commit_try: fsActive, THEN the learner (TABLED not reached), THEN n48_fs85_frame (the step), THEN "
             "the refused split", pa != std::string::npos && pl != std::string::npos && ps != std::string::npos && pr != std::string::npos &&
             pa < pl && pl < ps && ps < pr && count(src, "fsl_frame(") == 2u ? 1u : 0u, 1u);
    const std::string df = body_of(src, "static uint32_t gfxsrc_decide_frame(");
    const size_t pj = df.find("    gFsLJ.pShape = pShape; gFsLJ.pid = pid;"), pc = df.find("? gfxsrc_commit_try(vm, info, ib0Va,");
    const size_t pp = df.find("        pShape = n48_p73_is_p_shape(shapeOk, f.nib, f.ib[0].len);");
    expect_u("PIN gFsLJ is set every frame after the P recogniser and right before commit_try", pj != std::string::npos &&
             pc != std::string::npos && pp != std::string::npos && pp < pj && pj < pc && count(src, "gFsLJ.pShape = ") == 1u ? 1u : 0u, 1u);
    const std::string fr = body_of(src, "static __attribute__((noinline)) void fsl_frame(");
    expect_u("PIN fsl_frame: the learn with THIS frame's gFsLJ, then the comparison; it steps nothing",
             count(fr, "n48_fs_learn(&gFs, &gFsL, gFsLJ.pShape, plane, tgtVa, gFsLJ.pid, frame, &idx);") == 1u &&
             count(fr, "n48_fs_learn_compare(&gFs, &gFsL, gFs.geo, eligible,") == 1u && count(fr, "n48_fs_step") == 0u &&
             count(fr, "n48_fs85_frame") == 0u && fr.find("n48_fs_learn(") < fr.find("n48_fs_learn_compare(") ? 1u : 0u, 1u);
    expect_u("PIN the arm: the learner opens (and LEARNED seats UNLEARNED) right after the geometry seat, only when not TABLED",
             count(src, "                if (gFs.on || gFs.plane_on) n48_fs_seat_geo(&gFs, fs_geo_now());   // build 0.0.544 4b: members by console size\n") == 1u &&
             count(src, "                if (gFs.on && gFsL.mode != N48_FS_MODE_TABLED) { n48_fs_learn_open(&gFsL); n48_fs_learn_seat(&gFs, &gFsL); }") == 1u &&
             src.find("n48_fs_seat_geo(&gFs, fs_geo_now());") < src.find("{ n48_fs_learn_open(&gFsL); n48_fs_learn_seat(&gFs, &gFsL); }") ? 1u : 0u, 1u);
    const std::string fin = body_of(src, "static void xd_shot_finish(const char *where)");
    expect_u("PIN the STOP line: fsl_arm_stop once, in xd_shot_finish (one-shot and continuous); TABLED prints nothing",
             count(fin, "    fsl_arm_stop();") == 1u && count(src, "fsl_arm_stop(") == 2u &&
             count(body_of(src, "static __attribute__((noinline)) void fsl_arm_stop(void)"), "if (gFsL.mode == N48_FS_MODE_TABLED) return;") == 1u ? 1u : 0u, 1u);
    expect_u("PIN (0.0.549) the fallback line never claims every later frame PASSes: LEARNED prints N48_FSL_LIFTED_WORD",
             count(fr, "gFsL.mode == N48_FS_MODE_LEARNED ? N48_FSL_LIFTED_WORD : \"would be lifted (SHADOW: unchanged)\"") == 1u &&
             count(src, "PASS from this frame)") == 0u && count(hdr, "plane window opens") >= 1u ? 1u : 0u, 1u);
    expect_u("PIN nothing of the learner writes a register or a page (no MMIO / page-table helper in fsl_*)",
             count(fr, "wreg") + count(fr, "WREG") + count(fr, "gfxc_write") + count(fr, "rootwrite") == 0u ? 1u : 0u, 1u);
}

// =====================================================================================================================
// L10 — 0.0.549 MUST-FIX 2: an UNKNOWN console size. TABLED seats members 0 (n48_fs_seat_geo): the fill window never opens and every
// fill step PASSes. LEARNED must do the same - through 0.0.548 n48_fs_learn_seat forced members = 2 and refused frames TABLED passes.
// =====================================================================================================================
static void l10_unknown_geo()
{
    uint32_t same = 1u, allPass = 1u, reserves = 0u, frames = 0u, seatedZero = 1u;
    for (uint32_t it = 0u; it < 400u; it++) {
        n48_fs t, l; n48_fs_lrn Lt, Ll;
        armed(&t, &Lt, N48_FS_MODE_TABLED, N48_FS_GEO_UNKNOWN);
        armed(&l, &Ll, N48_FS_MODE_LEARNED, N48_FS_GEO_UNKNOWN);
        if (l.members != 0u || t.members != 0u) seatedZero = 0u;
        const uint64_t pool[] = { A, B, C, D, E, G, 0x400800000ull, 0x404800000ull, 0ull };
        for (uint32_t fr = 1u; fr <= 90u; fr++) {
            const uint64_t cb0 = pool[rnd(9u)];
            const uint32_t ps = rnd(3u) != 0u, pl = rnd(6u) == 0u, el = rnd(4u) != 0u, nseg = rnd(3u) == 0u ? 2u : 1u, fill = rnd(2u);
            const int32_t pid = rnd(40u) == 0u ? 8 : 7;
            const uint32_t vt = frame_in_order(&t, &Lt, ps, pl, cb0, pid, fr, el, nseg, fill);
            const uint32_t vl = frame_in_order(&l, &Ll, ps, pl, cb0, pid, fr, el, nseg, fill);
            if (vt != vl) same = 0u;
            if (vl != N48_FS_PASS) allPass = 0u;
            if (ps && !pl && cb0) reserves++;
            frames++;
            if (rnd(8u) == 0u) { (void)n48_fs_plane_commit(&t); (void)n48_fs_plane_commit(&l); }
        }
        if (t.refused != l.refused || t.plane_refused != l.plane_refused || t.plane_judged != l.plane_judged || t.members != l.members ||
            t.judged != l.judged || l.members != 0u || Ll.fallback) same = 0u;
    }
    expect_u("L10 unknown console size: LEARNED seats members 0 (as TABLED); over 36000 random frames every fill step, the fill and plane "
             "refused counts and the plane window equal TABLED's; every LEARNED fill step PASSes; no fallback",
             seatedZero && same && allPass && reserves > 0u && frames == 36000u ? 1u : 0u, 1u);
    expect_u("L10 n48_fs_learn_members_of: 0 at UNKNOWN, 2 at 1080p and 1440p", n48_fs_learn_members_of(N48_FS_GEO_UNKNOWN) == 0u &&
             n48_fs_learn_members_of(N48_FS_GEO_1080) == N48_FS_MEMBERS && n48_fs_learn_members_of(N48_FS_GEO_1440) == N48_FS_MEMBERS ? 1u : 0u, 1u);
    /* the would-differ count must not fire at UNKNOWN (SHADOW's learned side sized as TABLED's) */
    n48_fs f; n48_fs_lrn L; armed(&f, &L, N48_FS_MODE_SHADOW, N48_FS_GEO_UNKNOWN);
    const uint64_t five[] = { A, B, C, D, E };
    for (uint32_t fr = 1u; fr <= 40u; fr++) (void)frame_in_order(&f, &L, 1u, 0u, five[fr % 5u], 7, fr, 1u, 1u, 1u);
    expect_u("L10 SHADOW at UNKNOWN: learns, but would-differ stays 0 (both sides have no members)", L.n == 5u && L.wd == 0u ? 1u : 0u, 1u);
}

int main(int argc, char **argv)
{
    const std::string src = read_file(argc > 1 ? argv[1] : nullptr);
    const std::string hdr = read_file(argc > 2 ? argv[2] : nullptr);
    l1_real_boots();
    l2_to_l5();
    l6_l7();
    l8();
    l9_pins(src, hdr);
    l10_unknown_geo();
    std::printf("%s: %d passed, %d failed\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    return gFail ? 1 : 0;
}
