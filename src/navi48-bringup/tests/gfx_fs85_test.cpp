// gfx_fs85_test.cpp — build 0.0.530 (notes/design/SRCFILL85.md items 1-8, its replay, tests and planted breaks; apple/gfx_fs85.h):
// switch 85 "SOURCE FILLS" (S1 twin member, S2 retire on any non-live reserve, S3 one source fill in the plane window).
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/navi48-bringup/tests src/navi48-bringup/tests/gfx_fs85_test.cpp -o /tmp/fs85 && \
//         /tmp/fs85 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_commit.h \
//         src/navi48-bringup/src/apple/gfx_fs85.h
// Covers:
//   F1  OFF IDENTITY: the kext's dispatchers with switch 85 not in force (85 OFF; or 85 ON with 33 or 35 OFF) against a FROZEN
//       0.0.529 gfx_fillset.h (tests/frozen/gfx_fillset_ab336a80.h), 70000 random judged frames over many arm scopes: every answer
//       and every byte of the state equal
//   F2  S1: member 0 unchanged; member 1 = any twin; 0x403800000 and member 0's VA are not twins; no duplicate commit
//   F3  S2: any non-live RESERVE retires (dependency clean or not), the gate reason recorded, the dependency's kept; a live never
//   F4  S3: admits only a single-segment ColorFill of an uncommitted source, only with a member retired, only at left >= 2, at most
//       once per scope; the plane window stays open after it; only a GPUPass commit closes it
//   F5  exactly one window counter (judged / plane_judged) advances per judged frame while a window is open
//   F6  the four REAL-capture replays (fixture_fs85_runs.h, gen_fs85_fixture.py): a positive control (85 OFF reproduces each boot's
//       own `fillset:` line and every logged RESERVED-FOR-* answer), then the design's expectations with 85 ON
//   F7  the report line's worst case <= 491 bytes; the invariants (budget, N/T, pre-plane bound, both expiries) untouched
//   F8  source pins: the switch (OFF at boot, 37's shape, the continuous guard), the wiring and its ORDER in gfxsrc_commit_try, the
//       keystone's commit / withdrawal / exits, the arm
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "gfx_fs85.h"
#include "gfx_commit.h"
#include "gfx_dep.h"
namespace fs529 {
#include "frozen/gfx_fillset_ab336a80.h"
}
#include "fixture_fs85_runs.h"

static int gFail = 0, gPass = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    if (got == want) { gPass++; return; }
    gFail++;
    std::printf("FAIL %s: got %llu (%#llx), want %llu (%#llx)\n", what, (unsigned long long)got, (unsigned long long)got,
                (unsigned long long)want, (unsigned long long)want);
}

static const uint64_t kM0 = 0x400800000ull, kM1 = 0x404800000ull, kT5 = 0x405800000ull, kT6 = 0x406800000ull,
                      k403 = 0x403800000ull, kOther = 0x401800000ull;

// =====================================================================================================================
// F7 (part): THE INVARIANTS the design forbids touching (item 5, the brief's X3).
// =====================================================================================================================
static_assert(N48_CM_SHOT_BUDGET_MAX == 4u, "N48_CM_SHOT_BUDGET_MAX is a fixed policy limit");
static_assert(N48_CM_CONT_N_MAX == 1022u, "N ceiling: 1022 per  (item 9), and nothing in switch 85 touches it");
static_assert(N48_CM_PREPLANE_BOUND_US == 30000000ull, "the 30 s pre-plane bound unchanged");
static_assert(N48_FS_EXPIRE_JUDGED == 60u && N48_FS_PLANE_EXPIRE_JUDGED == 60u, "both 60-frame expiries unchanged");
static_assert(sizeof(n48_fs) == sizeof(fs529::n48_fs), "the fill-set state's layout is 0.0.529's");

static n48_fs85 x_on() { n48_fs85 x {}; x.on = 1u; return x; }
static n48_fs fs_armed(uint32_t on33, uint32_t on35)
{
    n48_fs f; std::memset(&f, 0, sizeof f);
    f.on = on33; f.plane_on = on35; n48_fs_open(&f);
    return f;
}

// =====================================================================================================================
// F1 — OFF IDENTITY over 70000 random judged frames.
// =====================================================================================================================
static uint64_t gRng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(uint32_t n) { gRng ^= gRng << 13; gRng ^= gRng >> 7; gRng ^= gRng << 17; return (uint32_t)(gRng % n); }
static uint64_t rnd_cb0()
{
    static const uint64_t v[] = { kM0, kM1, kT5, kT6, k403, kOther, 0x402800000ull, 0x400240000ull, 0ull };
    return v[rnd(sizeof v / sizeof v[0])];
}
static void checks_off_identity()
{
    uint32_t frames = 0u, mism = 0u, reserves = 0u, refuses = 0u, commits = 0u, retires = 0u, s3seen = 0u;
    for (uint32_t scope = 0u; frames < 70000u; scope++) {
        // 85's own switch varies (OFF, or ON-but-inert because 33 or 35 is OFF): every such combination must be 0.0.529.
        const uint32_t mode = scope % 4u;   // 0: 85 OFF, 33+35 ON; 1: 85 ON, 33 OFF; 2: 85 ON, 35 OFF; 3: 85 OFF, both random
        const uint32_t on33 = mode == 1u ? 0u : (mode == 3u ? rnd(2) : 1u);
        const uint32_t on35 = mode == 2u ? 0u : (mode == 3u ? rnd(2) : 1u);
        n48_fs a; std::memset(&a, 0, sizeof a); fs529::n48_fs b; std::memset(&b, 0, sizeof b);
        a.on = on33; a.plane_on = on35; b.on = on33; b.plane_on = on35;
        n48_fs85 x {}; x.on = (mode == 1u || mode == 2u) ? 1u : 0u;
        n48_fs_open(&a); fs529::n48_fs_open(&b); n48_fs85_open(&x);
        const uint32_t len = 20u + rnd(200);
        for (uint32_t i = 0u; i < len; i++, frames++) {
            const uint32_t fsA = rnd(4) != 0u, fpA = rnd(4) != 0u, el = rnd(3) != 0u, nseg = 1u + rnd(2) * rnd(3);
            const uint32_t psf = rnd(2), psp = rnd(3) == 0u, left = rnd(5), live = rnd(2), depf = rnd(2);
            const uint64_t cb0 = rnd_cb0();
            const uint32_t on = n48_fs85_active(&a, &x);
            if (on) { mism++; continue; }
            n48_fs85_frame_out o {};
            n48_fs85_frame(&a, &x, on, fsA ? (a.on ? 1u : 0u) : 0u, fpA ? (a.plane_on ? 1u : 0u) : 0u, el, nseg, psf, psp, cb0, left, &o);
            const uint32_t bf = fs529::n48_fs_identify_fill(&b, nseg, psf, cb0);
            const uint32_t bs = (fsA && b.on) ? fs529::n48_fs_step(&b, el, bf, cb0) : (uint32_t)fs529::N48_FS_PASS;
            const uint32_t bp = (fpA && b.plane_on) ? fs529::n48_fs_plane_step(&b, el, psp) : (uint32_t)fs529::N48_FS_PASS;
            if (o.is_fill != bf || o.fs_step != bs || o.fp_step != bp || o.s3) mism++;
            if (o.s3) s3seen++;
            reserves += (bs == fs529::N48_FS_RESERVE) + (bp == fs529::N48_FS_RESERVE);
            refuses += (bs == fs529::N48_FS_REFUSE) + (bp == fs529::N48_FS_REFUSE);
            // the gate's exits, in the kext's order: a non-live RESERVE retires; a spent one commits or is withdrawn
            if (bs == fs529::N48_FS_RESERVE && !live) {
                const uint32_t ra = n48_fs85_retire(&a, &x, cb0, 0u, depf, 7u, 3u);
                const uint32_t rb = fs529::n48_fs_retire(&b, cb0, depf, 7u);
                if (ra != rb) mism++;
                retires += rb;
            } else if (bs == fs529::N48_FS_RESERVE) {
                if (rnd(3) == 0u) {
                    const uint32_t ra = n48_fs85_retire_withdrawn(&a, &x, cb0, 9u), rb = fs529::n48_fs_retire_withdrawn(&b, cb0, 9u);
                    if (ra != rb) mism++;
                    retires += rb;
                } else {
                    const uint32_t ca = n48_fs85_commit(&a, &x, cb0, frames), cbb = fs529::n48_fs_commit(&b, cb0, frames);
                    if (ca != cbb) mism++;
                    commits += cbb;
                }
            }
            if (bp == fs529::N48_FS_RESERVE && rnd(2)) { (void)n48_fs_plane_commit(&a); (void)fs529::n48_fs_plane_commit(&b); }
            // stray calls the kext can make with a CB0 that is not reserved (a later keystone, a stale record)
            if (rnd(16) == 0u) {
                const uint64_t c2 = rnd_cb0();
                if (n48_fs85_commit(&a, &x, c2, 5u) != fs529::n48_fs_commit(&b, c2, 5u)) mism++;
            }
            if (rnd(16) == 0u) {
                const uint64_t c2 = rnd_cb0();
                const uint32_t dfl = rnd(2);
                if (n48_fs85_retire(&a, &x, c2, 0u, dfl, 4u, 2u) != fs529::n48_fs_retire(&b, c2, dfl, 4u)) mism++;
            }
            if (std::memcmp(&a, &b, sizeof a) != 0) mism++;
            if (n48_fs_win_open(&a) != fs529::n48_fs_win_open(&b) ||
                n48_fs_plane_win_open(&a) != fs529::n48_fs_plane_win_open(&b)) mism++;
        }
        const n48_fs85 fresh = [] { n48_fs85 z {}; return z; }();
        if (x.twin_va != fresh.twin_va || x.rnl != 0u || x.s3_n != 0u || x.ref_left + x.ref_noretire + x.ref_cap != 0u) mism++;
    }
    std::printf("  F1 OFF identity: %u judged frames, %u RESERVE, %u REFUSE, %u commits, %u retirements, %u mismatches\n",
                frames, reserves, refuses, commits, retires, mism);
    expect_u("F1 OFF IDENTITY: 70000 frames, the dispatchers equal the frozen 0.0.529 functions, answer and byte", mism, 0u);
    expect_u("F1 ...and never flag an S3 admission", s3seen, 0u);
    expect_u("F1 non-vacuous: frames judged", frames >= 70000u ? 1u : 0u, 1u);
    expect_u("F1 non-vacuous: RESERVE, REFUSE, commits and retirements all seen",
             (reserves > 1000u && refuses > 1000u && commits > 100u && retires > 100u) ? 1u : 0u, 1u);
}

// =====================================================================================================================
// F2 — S1, THE TWIN MEMBER.
// =====================================================================================================================
static void checks_s1()
{
    n48_fs f = fs_armed(1u, 1u);
    expect_u("S1 member 0 matches its own VA", n48_fs85_member_of(&f, 0u, kM0), 1u);
    expect_u("S1 member 0 does NOT match a twin", n48_fs85_member_of(&f, 0u, kT5), 0u);
    expect_u("S1 member 1 matches 0x404800000", n48_fs85_member_of(&f, 1u, kM1), 1u);
    expect_u("S1 member 1 matches 0x405800000", n48_fs85_member_of(&f, 1u, kT5), 1u);
    expect_u("S1 member 1 matches 0x406800000", n48_fs85_member_of(&f, 1u, kT6), 1u);
    expect_u("S1 member 1 does NOT match 0x403800000 (a Z-boot plane target)", n48_fs85_member_of(&f, 1u, k403), 0u);
    expect_u("S1 member 1 does NOT match member 0's VA", n48_fs85_member_of(&f, 1u, kM0), 0u);
    uint32_t dup = 0u;
    for (uint32_t t = 0u; t < N48_FS85_TWINS; t++) {
        if (kN48Fs85Twin[t] == kM0 || kN48Fs85Twin[t] == k403) dup++;
        for (uint32_t u = t + 1u; u < N48_FS85_TWINS; u++) if (kN48Fs85Twin[t] == kN48Fs85Twin[u]) dup++;
    }
    expect_u("S1 the twin table holds exactly the three measured sources, none twice, never member 0's VA or 0x403800000",
             (N48_FS85_TWINS == 3u && dup == 0u) ? 1u : 0u, 1u);
    // identify / step / commit with a twin
    n48_fs85 x = x_on();
    expect_u("S1 a single-segment ColorFill of 0x405800000 is a reservable fill", n48_fs85_identify_fill(&f, 1u, 1u, kT5), 1u);
    expect_u("S1 ...but not a two-segment one (K2)", n48_fs85_identify_fill(&f, 2u, 1u, kT5), 0u);
    expect_u("S1 ...and not 0x403800000", n48_fs85_identify_fill(&f, 1u, 1u, k403), 0u);
    expect_u("S1 the fill step RESERVEs the twin fill", n48_fs85_step(&f, 1u, 1u, kT5), N48_FS_RESERVE);
    expect_u("S1 the commit marks member 1", n48_fs85_commit(&f, &x, kT5, 26u), 1u);
    expect_u("S1 ...recording the twin it committed as", x.twin_va, kT5);
    expect_u("S1 ...and its seq", x.twin_seq, 26u);
    expect_u("S1 a second twin can no longer commit member 1", n48_fs85_commit(&f, &x, kT6, 27u), 0u);
    expect_u("S1 ...nor RESERVE", n48_fs85_step(&f, 1u, 1u, kT6), N48_FS_REFUSE);
    // DUPLICATE COMMIT: member 0's VA commits once only, however the table is edited.
    {
        n48_fs g = fs_armed(1u, 1u); n48_fs85 y = x_on();
        expect_u("S1 dup: member 0's fill commits", n48_fs85_commit(&g, &y, kM0, 1u), 1u);
        expect_u("S1 dup: a SECOND fill of member 0's VA is not a reservable fill", n48_fs85_identify_fill(&g, 1u, 1u, kM0), 0u);
        expect_u("S1 dup: ...the step REFUSES it", n48_fs85_step(&g, 1u, 1u, kM0), N48_FS_REFUSE);
        expect_u("S1 dup: ...and it cannot commit a second member", n48_fs85_commit(&g, &y, kM0, 2u), 0u);
        expect_u("S1 dup: committed_n stays 1", g.committed_n, 1u);
    }
    // retire_withdrawn with a twin
    {
        n48_fs g = fs_armed(1u, 1u); n48_fs85 y = x_on();
        expect_u("S1 a withdrawn twin retires member 1", n48_fs85_retire_withdrawn(&g, &y, kT6, 3u), 1u);
        expect_u("S1 ...member 1 retired", g.member_retired[1], 1u);
        expect_u("S1 ...member 0 untouched", g.member_retired[0] + g.member_committed[0], 0u);
    }
    // 85 not in force: 0.0.529's fixed pair (a twin is not a member)
    {
        n48_fs g = fs_armed(1u, 0u); n48_fs85 y = x_on();
        expect_u("S1 inert with 35 OFF: 0x405800000 does not commit (0.0.529)", n48_fs85_commit(&g, &y, kT5, 1u), 0u);
        n48_fs h = fs_armed(1u, 1u); n48_fs85 z {};
        expect_u("S1 85 OFF: 0x405800000 does not commit (0.0.529)", n48_fs85_commit(&h, &z, kT5, 1u), 0u);
        expect_u("S1 85 OFF: 0x405800000 does not retire at a withdrawal", n48_fs85_retire_withdrawn(&h, &z, kT5, 1u), 0u);
    }
}

// =====================================================================================================================
// F3 — S2, RETIRE ON ANY NON-LIVE RESERVE.
// =====================================================================================================================
static void checks_s2()
{
    {
        n48_fs f = fs_armed(1u, 1u); n48_fs85 x = x_on();
        expect_u("S2 AB2 f20 shape: a clean-dependency non-live RESERVE of member 0 RETIRES under 85",
                 n48_fs85_retire(&f, &x, kM0, 0u, 0u, (uint32_t)N48_DEP_OK, (uint32_t)N48_CM_WRITE_SHORT), 1u);
        expect_u("S2 ...member 0 retired", f.member_retired[0], 1u);
        expect_u("S2 ...the gate's reason is recorded in the new field", x.rnl_gate, (uint32_t)N48_CM_WRITE_SHORT);
        expect_u("S2 ...last_retire_reason keeps the DEPENDENCY's meaning", f.last_retire_reason, (uint32_t)N48_DEP_OK);
        expect_u("S2 ...counted retired-not-live", x.rnl, 1u);
    }
    {
        n48_fs f = fs_armed(1u, 1u); n48_fs85 x = x_on();
        expect_u("S2 a LIVE frame never retires", n48_fs85_retire(&f, &x, kM0, 1u, 1u, 3u, 0u), 0u);
        expect_u("S2 ...nothing retired", f.retired_n + x.rnl, 0u);
        expect_u("S2 a twin's non-live RESERVE retires member 1", n48_fs85_retire(&f, &x, kT6, 0u, 0u, 0u, 1u), 1u);
        expect_u("S2 a CB0 that is no member retires nothing", n48_fs85_retire(&f, &x, kOther, 0u, 1u, 0u, 1u), 0u);
    }
    {
        n48_fs f = fs_armed(1u, 1u); n48_fs85 x {};
        expect_u("S2 85 OFF: a clean dependency does NOT retire (0.0.529: a retry)",
                 n48_fs85_retire(&f, &x, kM0, 0u, 0u, (uint32_t)N48_DEP_OK, 1u), 0u);
        expect_u("S2 85 OFF: a failed dependency does (0.0.529)", n48_fs85_retire(&f, &x, kM0, 0u, 1u, 5u, 1u), 1u);
        expect_u("S2 85 OFF: the new field is untouched", x.rnl + x.rnl_gate, 0u);
    }
}

// =====================================================================================================================
// F4 — S3, ONE SOURCE FILL IN THE PLANE WINDOW.
// =====================================================================================================================
// a scope whose fill window is closed with member 0 RETIRED and member 1 COMMITTED as 0x404800000 (Z2's shape at f26)
static void z2_shape(n48_fs &f, n48_fs85 &x)
{
    f = fs_armed(1u, 1u); x = x_on(); n48_fs85_open(&x);
    (void)n48_fs85_commit(&f, &x, kM1, 4u);
    (void)n48_fs85_retire_withdrawn(&f, &x, kM0, (uint32_t)N48_DEP_SOURCE_NEUTER);
}
static uint32_t pstep(n48_fs &f, n48_fs85 &x, uint32_t el, uint32_t plane, uint32_t fill, uint64_t cb0, uint32_t left, uint32_t *s3)
{
    return n48_fs85_plane_step(&f, &x, el, plane, fill, cb0, left, s3);
}
static void checks_s3()
{
    n48_fs f; n48_fs85 x; uint32_t s3 = 0u;
    z2_shape(f, x);
    expect_u("S3 the plane window is open (fill window closed)", n48_fs_plane_win_open(&f), 1u);
    expect_u("S3 left 1: REFUSED", pstep(f, x, 1u, 0u, 1u, kT6, 1u, &s3), N48_FS_REFUSE);
    expect_u("S3 ...counted left<2", x.ref_left, 1u);
    expect_u("S3 left 0: REFUSED", pstep(f, x, 1u, 0u, 1u, kT6, 0u, &s3), N48_FS_REFUSE);
    expect_u("S3 a NON-fill of a source: REFUSED (never admitted)", pstep(f, x, 1u, 0u, 0u, kT6, 4u, &s3), N48_FS_REFUSE);
    expect_u("S3 ...no admission", x.s3_n, 0u);
    expect_u("S3 a fill of a NON-source (0x403800000): REFUSED", pstep(f, x, 1u, 0u, 1u, k403, 4u, &s3), N48_FS_REFUSE);
    expect_u("S3 a fill of the committed twin 0x404800000: REFUSED", pstep(f, x, 1u, 0u, 1u, kM1, 4u, &s3), N48_FS_REFUSE);
    expect_u("S3 a non-eligible fill: PASS (an earlier rung refuses it)", pstep(f, x, 0u, 0u, 1u, kT6, 4u, &s3), N48_FS_PASS);
    expect_u("S3 left 2: ADMITTED (RESERVE)", pstep(f, x, 1u, 0u, 1u, kT6, 2u, &s3), N48_FS_RESERVE);
    expect_u("S3 ...flagged as a source fill", s3, 1u);
    expect_u("S3 ...counted once", x.s3_n, 1u);
    expect_u("S3 ...the window stays OPEN", n48_fs_plane_win_open(&f), 1u);
    expect_u("S3 a SECOND candidate is REFUSED (cap 1), even at left 4", pstep(f, x, 1u, 0u, 1u, kT5, 4u, &s3), N48_FS_REFUSE);
    expect_u("S3 ...counted cap", x.ref_cap, 1u);
    expect_u("S3 ...and flagged 0", s3, 0u);
    // the admitted fill's commit keeps the window open
    x.s3_pend = 1u; x.s3_pend_cb0 = kT6;
    expect_u("S3 the admitted fill commits at the keystone", n48_fs85_s3_commit(&x, 12u), 1u);
    expect_u("S3 ...its seq", x.s3_seq, 12u);
    expect_u("S3 ...the plane window is STILL open (only a GPUPass commit closes it)", n48_fs_plane_win_open(&f), 1u);
    expect_u("S3 ...a GPUPass frame RESERVEs", pstep(f, x, 1u, 1u, 0u, kOther, 1u, &s3), N48_FS_RESERVE);
    expect_u("S3 ...and is not flagged a source fill", s3, 0u);
    (void)n48_fs_plane_commit(&f);
    expect_u("S3 ...its commit closes the window", n48_fs_plane_win_open(&f), 0u);
    // no retirement: never
    {
        n48_fs g = fs_armed(1u, 1u); n48_fs85 y = x_on();
        (void)n48_fs85_commit(&g, &y, kM0, 1u); (void)n48_fs85_commit(&g, &y, kM1, 2u);   // AB's shape: both committed
        expect_u("S3 no member retired (AB, Z3): REFUSED", pstep(g, y, 1u, 0u, 1u, kT6, 4u, &s3), N48_FS_REFUSE);
        expect_u("S3 ...counted no-retire", y.ref_noretire, 1u);
        expect_u("S3 ...no admission", y.s3_n, 0u);
    }
    // the kext's frame order: a TWO-segment frame carrying a ColorFill is NOT a fill (K2) and is never admitted
    {
        n48_fs g; n48_fs85 y; z2_shape(g, y);
        n48_fs85_frame_out o {};
        n48_fs85_frame(&g, &y, 1u, 1u, 1u, 1u, 2u, 1u, 0u, kT6, 4u, &o);
        expect_u("S3 frame: nseg 2 + ColorFill + a source CB0 is REFUSED", o.fp_step, N48_FS_REFUSE);
        expect_u("S3 frame: ...never admitted", o.s3 + y.s3_n, 0u);
        n48_fs85_frame(&g, &y, 1u, 1u, 1u, 1u, 1u, 1u, 0u, kT6, 2u, &o);
        expect_u("S3 frame: nseg 1 + ColorFill + a source CB0 + left 2 is ADMITTED", o.fp_step, N48_FS_RESERVE);
        expect_u("S3 frame: ...flagged", o.s3, 1u);
    }
    // a fresh scope resets the cap
    n48_fs85_open(&x);
    expect_u("S3 a new arm scope starts with no admission and no counters",
             x.s3_n + x.s3_committed + x.ref_left + x.ref_cap + x.ref_noretire + x.rnl + (uint32_t)(x.twin_va != 0ull), 0u);
    // 85 not in force: the plane step is 0.0.529's (a fill is refused, never admitted)
    {
        n48_fs g; n48_fs85 y; z2_shape(g, y); y.on = 0u;
        n48_fs85_frame_out o {};
        n48_fs85_frame(&g, &y, n48_fs85_active(&g, &y), 1u, 1u, 1u, 1u, 1u, 0u, kT6, 4u, &o);
        expect_u("S3 85 OFF: the source fill is RESERVED-FOR-PLANE (0.0.529)", o.fp_step, N48_FS_REFUSE);
        expect_u("S3 85 OFF: ...not flagged", o.s3, 0u);
    }
}

// =====================================================================================================================
// F5 — EXACTLY ONE WINDOW COUNTER ADVANCES PER JUDGED FRAME.
// =====================================================================================================================
static void checks_one_counter()
{
    uint32_t bad = 0u, both = 0u, frames = 0u, fillExp = 0u;
    for (uint32_t on = 0u; on < 2u; on++) {
        for (uint32_t scope = 0u; scope < 400u; scope++) {
            n48_fs f = fs_armed(1u, 1u); n48_fs85 x = x_on(); x.on = on;
            for (uint32_t i = 0u; i < 160u; i++, frames++) {
                const uint32_t fillOpen = n48_fs_win_open(&f), planeOpen = n48_fs_plane_win_open(&f);
                const uint32_t j0 = f.judged, p0 = f.plane_judged, e0 = f.expired, pe0 = f.plane_expired;
                const uint64_t cb0 = rnd_cb0();
                n48_fs85_frame_out o {};
                n48_fs85_frame(&f, &x, n48_fs85_active(&f, &x), 1u, 1u, rnd(3) != 0u, 1u + rnd(2), rnd(2), rnd(4) == 0u, cb0,
                               rnd(5), &o);
                const uint32_t dj = f.judged - j0, dp = (uint32_t)f.plane_judged - p0;
                if (dj && dp) both++;
                const uint32_t planeExpiring = !fillOpen && planeOpen && !pe0 && f.plane_expired;
                if (fillOpen && !e0 && f.expired) fillExp++;
                if ((fillOpen || planeOpen) && !planeExpiring && dj + dp != 1u) bad++;
                // the kext's exits, sometimes, so windows close by commit and retirement too
                if (o.fs_step == N48_FS_RESERVE) {
                    if (rnd(2)) (void)n48_fs85_commit(&f, &x, cb0, i); else (void)n48_fs85_retire(&f, &x, cb0, 0u, 1u, 1u, 1u);
                }
                if (o.fp_step == N48_FS_RESERVE && !o.s3 && rnd(3) == 0u) (void)n48_fs_plane_commit(&f);
            }
        }
    }
    std::printf("  F5 one counter: %u frames, %u fill expiries crossed, %u frames with the wrong count, %u with both\n",
                frames, fillExp, bad, both);
    expect_u("F5 exactly one of judged / plane_judged advances per frame while a window is open", bad, 0u);
    expect_u("F5 never both", both, 0u);
    expect_u("F5 non-vacuous: fill-window expiries crossed (the frame where the order matters)", fillExp > 50u ? 1u : 0u, 1u);
}

// =====================================================================================================================
// F6 — THE FOUR REAL-CAPTURE REPLAYS. The kext's order per frame (gfxsrc_commit_try, then hook_gfxCommitIB's keystone):
//   judged (commit_try runs) iff a WindowServer frame (`ws`, the dpled vrd is not `shape`) - VERIFIED by the positive control
//   arm COMMIT iff the real arm was COMMIT AND the replayed shot is still ARMED (budget 4, the real n48_cm_shot functions)
//   left = n48_cm_shot_left, read before anything else; n48_fs85_frame; the gate's two POLICY rungs (fs, then fp);
//   eligible = the real verdict TRANSLATE, OR (85 ON replay only, THE DESIGN'S HYPOTHESIS, "a plane reading S becomes
//     TRANSLATE once S's fill commits") a plane frame whose every logged PROVENANCE-REFUSED surface is a fill CB0 committed here;
//   the other rungs: the real answer. COMMIT -> OK. RESERVED-FOR-* (the gate's LAST two rungs, so every other rung passed:
//     CONFIRMED by n48_cm_gate's order) -> OK. Any other real answer -> refused, NOT live. A hypothesised frame -> OK (SUSPECTED).
//   a non-live RESERVE -> n48_fs85_retire; an OK -> n48_cm_shot_spend -> the keystone: the REAL verdict for that frame's seq
//     (15 = withdrawn -> n48_fs85_retire_withdrawn under switch 64, which all four boots had ON), else commit.
// =====================================================================================================================
enum { K_M0 = 1, K_M1, K_S3, K_PLANE, K_OTHER };
struct Slot { uint32_t f, kind, withdrawn, t_ms; };
struct Replay {
    n48_fs fs; n48_fs85 x; uint32_t nslot; Slot slot[8];
    uint32_t judgedMismatch;   // positive control: a logged RESERVED-FOR-* answer the replay does not give, or vice versa
    uint32_t hyp;              // frames made eligible by the hypothesis
};
static const char *kind_name(uint32_t k)
{
    return k == K_M0 ? "m0" : k == K_M1 ? "m1/twin" : k == K_S3 ? "S3" : k == K_PLANE ? "PLANE" : "other";
}
static Replay replay(const n48_fs85_fx *fx, uint32_t n, uint32_t on85, uint32_t hyp, const char *tag, int print)
{
    Replay r; std::memset(&r, 0, sizeof r);
    r.fs.on = 1u; r.fs.plane_on = 1u; n48_fs_open(&r.fs);
    r.x.on = on85; n48_fs85_open(&r.x);
    n48_cm_shot sh; std::memset(&sh, 0, sizeof sh); sh.state = N48_CM_SHOT_ARMED; sh.budget = 4u;
    uint64_t committedFill[8]; uint32_t ncf = 0u;
    for (uint32_t i = 0u; i < n; i++) {
        const n48_fs85_fx &q = fx[i];
        if (!q.ws) continue;
        const uint32_t arm = (q.arm == 2u && sh.state == N48_CM_SHOT_ARMED) ? 1u : 0u;
        uint32_t eligible = q.translate, hypo = 0u;
        if (!eligible && hyp && q.plane && q.nib == 1u && q.nprov > 0u && q.nprov <= 4u) {
            uint32_t all = 1u;
            for (uint32_t k = 0u; k < q.nprov; k++) {
                uint32_t hit = 0u;
                for (uint32_t c = 0u; c < ncf; c++) if (committedFill[c] == q.prov[k]) hit = 1u;
                if (!hit) all = 0u;
            }
            if (all) { eligible = 1u; hypo = 1u; r.hyp++; }
        }
        const uint32_t left = n48_fs85_active(&r.fs, &r.x) ? n48_cm_shot_left(&sh) : 0u;
        const uint32_t nseg = (q.nib == 1u && q.nps == 1u) ? 1u : 2u;   // the fixture's single-draw proxy for nsegPre == 1
        n48_fs85_frame_out o {};
        n48_fs85_frame(&r.fs, &r.x, n48_fs85_active(&r.fs, &r.x), arm, arm, eligible, nseg, q.fill, q.plane, q.cb0, left, &o);
        const uint32_t fsOpen = arm ? n48_fs_win_open(&r.fs) : 0u, fpOpen = arm ? n48_fs_plane_win_open(&r.fs) : 0u;
        const uint32_t fsRes = o.fs_step == N48_FS_RESERVE, fpRes = o.fp_step == N48_FS_RESERVE;
        uint32_t window = 0u;   // 1 RESERVED-FOR-FILL, 2 RESERVED-FOR-PLANE
        if (eligible && arm) {
            if (fsOpen && !fsRes) window = 1u;
            else if (fpOpen && !fpRes) window = 2u;
        }
        // positive control bookkeeping: a real window answer must be the replay's, and a replay window refusal must be real
        // unless the real gate refused the frame on an EARLIER rung (any other answer, gate 3)
        if (!on85 && !hyp && q.translate) {
            if ((q.gate == 1u || q.gate == 2u) && window != q.gate) r.judgedMismatch++;
            if (window && q.gate == 0u) r.judgedMismatch++;
        }
        if (!eligible || !arm) continue;
        const uint32_t restOk = hypo ? 1u : (q.gate == 0u || q.gate == 1u || q.gate == 2u) ? 1u : 0u;
        const uint32_t live = restOk;   // a real answer outside the three is a rung above the policy rungs: not live
        if (fsRes && !live)
            (void)n48_fs85_retire(&r.fs, &r.x, q.cb0, 0u, q.dep_clean ? 0u : 1u, q.dep_clean ? (uint32_t)N48_DEP_OK : 1u,
                                  (uint32_t)N48_CM_WRITE_SHORT);
        if (window || !restOk) continue;
        const uint32_t seq = q.seq ? q.seq : 1000u + q.f;
        if (!n48_cm_shot_spend(&sh, seq)) continue;
        const uint32_t withdrawn = (!hypo && q.ks == 15u) ? 1u : 0u;
        uint32_t kind = K_OTHER;
        if (fsRes) kind = n48_fs85_member_of(&r.fs, 0u, q.cb0) ? K_M0 : K_M1;
        else if (fpRes && o.s3) kind = K_S3;
        else if (fpRes) kind = K_PLANE;
        if (r.nslot < 8u) r.slot[r.nslot++] = Slot { q.f, kind, withdrawn, q.t_ms };
        if (withdrawn) {
            if (fsRes) (void)n48_fs85_retire_withdrawn(&r.fs, &r.x, q.cb0, (uint32_t)N48_DEP_SOURCE_NEUTER);
            continue;
        }
        if (fsRes && n48_fs85_commit(&r.fs, &r.x, q.cb0, seq) && ncf < 8u) committedFill[ncf++] = q.cb0;
        if (fpRes && !o.s3) (void)n48_fs_plane_commit(&r.fs);
        if (fpRes && o.s3) {
            r.x.s3_pend = 1u; r.x.s3_pend_cb0 = q.cb0;
            if (n48_fs85_s3_commit(&r.x, seq) && ncf < 8u) committedFill[ncf++] = q.cb0;
        }
    }
    if (print) {
        std::printf("  %-4s 85 %-3s%s:", tag, on85 ? "ON" : "OFF", hyp ? "+hyp" : "    ");
        for (uint32_t s = 0u; s < r.nslot; s++)
            std::printf(" [slot %u f%u %s%s %u.%03us]", s + 1u, r.slot[s].f, kind_name(r.slot[s].kind),
                        r.slot[s].withdrawn ? " WITHDRAWN" : "", r.slot[s].t_ms / 1000u, r.slot[s].t_ms % 1000u);
        std::printf("\n       fillset: m0 %s seq %u, m1 %s seq %u; committed %u, refused %llu, expired %llu, judged %u, retired %llu;"
                    " plane committed %u refused %u expired %u judged %u; srcfill85: twin %#llx rnl %u s3 %u (%#llx) refused %u/%u/%u\n",
                    r.fs.member_committed[0] ? "committed" : r.fs.member_retired[0] ? "retired" : "uncommitted",
                    (uint32_t)r.fs.member_seq[0],
                    r.fs.member_committed[1] ? "committed" : r.fs.member_retired[1] ? "retired" : "uncommitted",
                    (uint32_t)r.fs.member_seq[1], r.fs.committed_n, (unsigned long long)r.fs.refused,
                    (unsigned long long)r.fs.expiry, r.fs.judged, (unsigned long long)r.fs.retired, r.fs.plane_committed,
                    r.fs.plane_refused, r.fs.plane_expiry, r.fs.plane_judged, (unsigned long long)r.x.twin_va, r.x.rnl, r.x.s3_n,
                    (unsigned long long)r.x.s3_va, r.x.ref_left, r.x.ref_noretire, r.x.ref_cap);
    }
    return r;
}
#define NFX(a) (uint32_t)(sizeof(a) / sizeof((a)[0]))
struct FsLine { uint32_t m0c, m0r, m0seq, m1c, m1r, m1seq, committed, refused, expiry, judged, retired, pc, pr, pe, pj; };
static void expect_line(const char *tag, const Replay &r, const FsLine &w)
{
    char l[160];
    const uint32_t got[15] = { r.fs.member_committed[0], r.fs.member_retired[0], (uint32_t)r.fs.member_seq[0], r.fs.member_committed[1],
                               r.fs.member_retired[1], (uint32_t)r.fs.member_seq[1], r.fs.committed_n, (uint32_t)r.fs.refused,
                               (uint32_t)r.fs.expiry, r.fs.judged, (uint32_t)r.fs.retired, r.fs.plane_committed, r.fs.plane_refused,
                               r.fs.plane_expiry, r.fs.plane_judged };
    const uint32_t want[15] = { w.m0c, w.m0r, w.m0seq, w.m1c, w.m1r, w.m1seq, w.committed, w.refused, w.expiry, w.judged, w.retired,
                                w.pc, w.pr, w.pe, w.pj };
    static const char *const nm[15] = { "m0 committed", "m0 retired", "m0 seq", "m1 committed", "m1 retired", "m1 seq", "committed",
                                        "refused-under-reservation", "expired", "judged", "retired", "plane committed",
                                        "plane refused", "plane expired", "plane judged" };
    for (uint32_t k = 0u; k < 15u; k++) {
        std::snprintf(l, sizeof l, "F6 POSITIVE CONTROL %s (85 OFF): the replay reproduces the boot's own fillset line - %s", tag, nm[k]);
        expect_u(l, got[k], want[k]);
    }
    std::snprintf(l, sizeof l, "F6 POSITIVE CONTROL %s: every logged RESERVED-FOR-* answer is the replay's, per frame", tag);
    expect_u(l, r.judgedMismatch, 0u);
}
static uint32_t slot_of(const Replay &r, uint32_t f, uint32_t kind)
{
    for (uint32_t s = 0u; s < r.nslot; s++) if (r.slot[s].f == f && r.slot[s].kind == kind) return s + 1u;
    return 0u;
}
static void checks_replays()
{
    // ---- POSITIVE CONTROL: 85 OFF, the REAL verdicts (no hypothesis), against each boot's final `fillset:` line, verbatim:
    // Z2  run11i: "0x400800000 retired seq 0, 0x404800000 committed seq 4; committed 1 of 2; refused-under-reservation 5, expired 0,
    //             judged 17, window closed; retired 1, retire reason source-neuter; plane win closed: committed 0, refused 4,
    //             expired 1, judged 60."
    // Z3  run11j: "0x400800000 committed seq 6, 0x404800000 committed seq 4; committed 2 of 2; refused-under-reservation 6, expired 0,
    //             judged 17 ...; retired 0 ...; plane win closed: committed 1, refused 1, expired 0, judged 5."
    // AB  run11l: "0x400800000 committed seq 5, 0x404800000 committed seq 4; committed 2 of 2; refused-under-reservation 5, expired 0,
    //             judged 17 ...; retired 0 ...; plane win closed: committed 1, refused 2, expired 0, judged 14."
    // AB2 run11m: "0x400800000 uncommitted seq 0, 0x404800000 uncommitted seq 0; committed 0 of 2; refused-under-reservation 7,
    //             expired 1, judged 60, window expired; retired 0 ...; plane win closed: committed 0, refused 12, expired 1, judged 60."
    const Replay z2o = replay(kZ2, NFX(kZ2), 0u, 0u, "Z2", 1), z3o = replay(kZ3, NFX(kZ3), 0u, 0u, "Z3", 1),
                 abo = replay(kAB, NFX(kAB), 0u, 0u, "AB", 1), ab2o = replay(kAB2, NFX(kAB2), 0u, 0u, "AB2", 1);
    expect_line("Z2", z2o, FsLine { 0, 1, 0, 1, 0, 4, 1, 5, 0, 17, 1, 0, 4, 1, 60 });
    expect_line("Z3", z3o, FsLine { 1, 0, 6, 1, 0, 4, 2, 6, 0, 17, 0, 1, 1, 0, 5 });
    expect_line("AB", abo, FsLine { 1, 0, 5, 1, 0, 4, 2, 5, 0, 17, 0, 1, 2, 0, 14 });
    expect_line("AB2", ab2o, FsLine { 0, 0, 0, 0, 0, 0, 0, 7, 1, 60, 0, 0, 12, 1, 60 });
    // ---- THE DESIGN'S EXPECTATIONS, 85 ON with the provenance hypothesis. ----
    const Replay z2 = replay(kZ2, NFX(kZ2), 1u, 1u, "Z2", 1), z3 = replay(kZ3, NFX(kZ3), 1u, 1u, "Z3", 1),
                 ab = replay(kAB, NFX(kAB), 1u, 1u, "AB", 1), ab2 = replay(kAB2, NFX(kAB2), 1u, 1u, "AB2", 1);
    // Z3: unchanged. f14 m1 commit, f20 m0 commit, plane f25 in slot 3. S3 never fires (no retirement).
    expect_u("F6 Z3 85 ON: f14 is member 1 in slot 1", slot_of(z3, 14u, K_M1), 1u);
    expect_u("F6 Z3 85 ON: f20 is member 0 in slot 2", slot_of(z3, 20u, K_M0), 2u);
    expect_u("F6 Z3 85 ON: plane f25 in slot 3", slot_of(z3, 25u, K_PLANE), 3u);
    expect_u("F6 Z3 85 ON: S3 never fires", z3.x.s3_n, 0u);
    expect_u("F6 Z3 85 ON: the same first three slots as 85 OFF",
             (slot_of(z3o, 14u, K_M1) == 1u && slot_of(z3o, 20u, K_M0) == 2u && slot_of(z3o, 25u, K_PLANE) == 3u) ? 1u : 0u, 1u);
    // AB: unchanged. Plane f36 in slot 3.
    expect_u("F6 AB 85 ON: plane f36 in slot 3", slot_of(ab, 36u, K_PLANE), 3u);
    expect_u("F6 AB 85 ON: S3 never fires", ab.x.s3_n, 0u);
    expect_u("F6 AB 85 OFF: plane f36 in slot 3 (the boot as it ran)", slot_of(abo, 36u, K_PLANE), 3u);
    // Z2: f15 slot 1, f21 slot 2 (withdrawn, retired), f26 admitted by S3 in slot 3, plane f31 slot 4 at f1+7.26 s.
    expect_u("F6 Z2 85 ON: f15 member 1 in slot 1", slot_of(z2, 15u, K_M1), 1u);
    expect_u("F6 Z2 85 ON: f21 member 0 in slot 2", slot_of(z2, 21u, K_M0), 2u);
    expect_u("F6 Z2 85 ON: ...withdrawn and retired", (z2.nslot >= 2u && z2.slot[1].withdrawn && z2.fs.member_retired[0]) ? 1u : 0u, 1u);
    expect_u("F6 Z2 85 ON: f26 (0x406800000) admitted by S3 in slot 3", slot_of(z2, 26u, K_S3), 3u);
    expect_u("F6 Z2 85 ON: plane f31 in slot 4", slot_of(z2, 31u, K_PLANE), 4u);
    expect_u("F6 Z2 85 ON: ...at f1+7.26 s (within 100 ms)",
             (z2.nslot >= 4u && z2.slot[3].t_ms + 100u >= 7260u && z2.slot[3].t_ms <= 7360u) ? 1u : 0u, 1u);
    // AB2: f20 retired by S2. f26 0x405800000 is member 1 and commits in slot 1. Plane f30 takes slot 2 at f1+6.74 s.
    expect_u("F6 AB2 85 ON: f20's member 0 is retired by S2 (retired-not-live 1)",
             (ab2.fs.member_retired[0] && ab2.x.rnl == 1u) ? 1u : 0u, 1u);
    expect_u("F6 AB2 85 ON: f26 (0x405800000) is member 1 in slot 1", slot_of(ab2, 26u, K_M1), 1u);
    expect_u("F6 AB2 85 ON: ...recorded as the twin", ab2.x.twin_va, kT5);
    expect_u("F6 AB2 85 ON: plane f30 in slot 2", slot_of(ab2, 30u, K_PLANE), 2u);
    expect_u("F6 AB2 85 ON: ...at f1+6.74 s (within 100 ms)",
             (ab2.nslot >= 2u && ab2.slot[1].t_ms + 100u >= 6740u && ab2.slot[1].t_ms <= 6840u) ? 1u : 0u, 1u);
    expect_u("F6 AB2 85 ON: the f14 fill of 0x403800000 spends nothing (not a twin)", slot_of(ab2, 14u, K_M1), 0u);
    // the void boots as they ran: no plane slot at all (85 OFF)
    uint32_t planes = 0u;
    for (uint32_t s = 0u; s < z2o.nslot; s++) planes += z2o.slot[s].kind == K_PLANE;
    for (uint32_t s = 0u; s < ab2o.nslot; s++) planes += ab2o.slot[s].kind == K_PLANE;
    expect_u("F6 the void boots replayed with 85 OFF commit no plane (the positive control's other half)", planes, 0u);
    // the invariant on the real data: an S3 fill always leaves a slot
    expect_u("F6 Z2 85 ON: after the S3 fill at least one slot is left (it was admitted at left >= 2)",
             z2.nslot >= 3u && z2.slot[2].kind == K_S3 ? 1u : 0u, 1u);
}

// =====================================================================================================================
// F7 — THE REPORT LINE'S WORST CASE.
// =====================================================================================================================
static void checks_width()
{
    size_t gate = 0u;
    const char *gw = "none";
    for (uint32_t r = 0u; r < N48_CM_REASONS; r++) if (std::strlen(n48_cm_reason_name(r)) > gate) { gate = std::strlen(n48_cm_reason_name(r)); gw = n48_cm_reason_name(r); }
    const char *states[] = { "OFF (default)", "ON", "ON (INERT: 33 and 35 must both be ON)" };
    const char *st = states[0];
    for (const char *s : states) if (std::strlen(s) > std::strlen(st)) st = s;
    const char *ch = "`gfxneuter 85` REFUSED it (unknown M), unchanged";
    char b[1024];
    const int n = std::snprintf(b, sizeof b, N48_FS85_FMT, st, ch, ~0ull, ~0u, ~0u, gw, ~0u, ~0ull, ~0u, ~0u, ~0u, ~0u);
    std::printf("  F7 srcfill85 line worst case: %d bytes (cap %u)\n", n, N48_FS85_BODY_CAP);
    expect_u("F7 the srcfill85 line fits 491 bytes at every field's widest", (n > 0 && (unsigned)n <= N48_FS85_BODY_CAP) ? 1u : 0u, 1u);
    n48_fs f = fs_armed(1u, 1u); n48_fs85 x {};
    expect_u("F7 state word OFF", std::strcmp(n48_fs85_state(&f, &x), "OFF (default)") == 0 ? 1u : 0u, 1u);
    x.on = 1u; expect_u("F7 state word ON", std::strcmp(n48_fs85_state(&f, &x), "ON") == 0 ? 1u : 0u, 1u);
    f.plane_on = 0u; expect_u("F7 state word INERT", std::strcmp(n48_fs85_state(&f, &x), states[2]) == 0 ? 1u : 0u, 1u);
}

// =====================================================================================================================
// F8 — SOURCE PINS.
// =====================================================================================================================
static std::string read_file(const char *p)
{
    std::string s; FILE *fp = std::fopen(p, "rb");
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
// the header's CODE: every /* */ and // comment removed, so the invariant check below reads what the compiler reads
static std::string strip_comments(const std::string &s)
{
    std::string o; o.reserve(s.size());
    for (size_t i = 0u; i < s.size();) {
        if (s.compare(i, 2, "/*") == 0) { const size_t e = s.find("*/", i + 2u); i = e == std::string::npos ? s.size() : e + 2u; continue; }
        if (s.compare(i, 2, "//") == 0) { const size_t e = s.find('\n', i); i = e == std::string::npos ? s.size() : e; continue; }
        o += s[i++];
    }
    return o;
}
static void checks_pins(const std::string &src, const std::string &cm, const std::string &hdrRaw)
{
    const std::string hdr = strip_comments(hdrRaw);
    expect_u("PIN gfx_fs85.h was read (its code)", count(hdr, "n48_fs85_plane_step(") >= 1u ? 1u : 0u, 1u);
    expect_u("PIN the kext was read", src.size() > 1000000u ? 1u : 0u, 1u);
    // the switch
    expect_u("PIN 85 is defined once, all-zero (OFF AT BOOT)", count(src, "static n48_fs85 gFs85 {};"), 1u);
    expect_u("PIN no other initialiser of gFs85", count(src, "static n48_fs85 gFs85"), 1u);
    const std::string verb = body_of(src, "} else if ((arg & 0xffull) == 85ull) {");
    expect_u("PIN the selector `(arg & 0xffull) == 85ull` branch exists exactly once",
             count(src, "} else if ((arg & 0xffull) == 85ull) {"), 1u);
    expect_u("PIN gFs85.on is written ONLY by its verb, M 1 on / M 0xFF off",
             (count(src, "gFs85.on =") == 2u && count(verb, "gFs85.on = 0u; changed = 1;") == 1u &&
              count(verb, "gFs85.on = 1u; changed = 1;") == 1u && count(verb, "} else if (m == 0xFFu) {") == 1u &&
              count(verb, "} else if (m == 1u) {") == 1u) ? 1u : 0u, 1u);
    expect_u("PIN SWITCH-GUARD:85 - the verb calls the continuous guard with its own selector, exactly once",
             count(src, "n48_cm_cont_switch_refused(85u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("PIN ...and 85 is in the guard's list", n48_cm_cont_switch_guarded(85u), 1u);
    expect_u("PIN ...and a refused / unknown M changes nothing (st 6, switch 37's shape)",
             (count(verb, "} else if (contRefused85) {\n            st = 6;") == 1u && count(verb, "st = 6;") == 2u) ? 1u : 0u, 1u);
    expect_u("PIN the verb prints the ONE bounded line", count(verb, "HWLOG(N48_FS85_FMT,") == 1u && count(src, "N48_FS85_FMT") == 1u ? 1u : 0u, 1u);
    expect_u("PIN the verb touches no forgiveness / budget switch", (count(verb, "gXdForgive") + count(verb, "gXdRunBudget")), 0u);
    // the wiring in gfxsrc_commit_try, and its ORDER
    const std::string ct = body_of(src, "static uint32_t gfxsrc_commit_try(");
    const char *on = "const uint32_t fs85On = n48_fs85_active(&gFs, &gFs85);";
    const char *lf = "const uint32_t fs85Left = fs85On ? n48_cm_shot_left(&gXdShot) : 0u;";
    const char *fr = "n48_fs85_frame(&gFs, &gFs85, fs85On, fsActive, fpActive, fsEligible, gXdBuild.nsegPre, gXdBuild.fill, gXdBuild.plane, tgtVa,\n"
                     "                   fs85Left, &fs85F);";
    const char *fsS = "const uint32_t fsStep = fs85F.fs_step;";
    const char *fpS = "const uint32_t fpStep = fs85F.fp_step;";
    const char *sp = "const bool spent = live && reason == N48_CM_OK && n48_cm_shot_spend(&gXdShot, gXdCmToken.seq);";
    const char *rt = "if (c.fs_reserve && !live) (void)n48_fs85_retire(&gFs, &gFs85, tgtVa, live ? 1u : 0u, !c.dep_ok, (uint32_t)gXdGateDep.reason, reason);";
    const char *pp = "if (c.fp_reserve && !fs85F.s3) { gFsPlanePend = 1u; }";
    const char *ps = "if (c.fp_reserve && fs85F.s3) { gFs85.s3_pend = 1u; gFs85.s3_pend_cb0 = tgtVa; }";
    const char *fa = "const uint32_t fpActive = (gFs.plane_on && arm == N48_SD_ARM_COMMIT) ? 1u : 0u;";
    const size_t pOn = ct.find(on), pLf = ct.find(lf), pFr = ct.find(fr), pFs = ct.find(fsS), pFp = ct.find(fpS), pSp = ct.find(sp),
                 pRt = ct.find(rt), pPp = ct.find(pp), pPs = ct.find(ps), pFa = ct.find(fa);
    const size_t np = std::string::npos;
    expect_u("PIN each wiring statement is in gfxsrc_commit_try exactly once",
             (count(ct, on) == 1u && count(ct, lf) == 1u && count(ct, fr) == 1u && count(ct, fsS) == 1u && count(ct, fpS) == 1u &&
              count(ct, sp) == 1u && count(ct, rt) == 1u && count(ct, pp) == 1u && count(ct, ps) == 1u && count(ct, fa) == 1u &&
              count(src, "n48_fs85_frame(") == 1u && count(src, "n48_cm_shot_left(&gXdShot)") >= 1u) ? 1u : 0u, 1u);
    expect_u("PIN ORDER: 85's `left` is read BEFORE the frame's steps, which run BEFORE n48_cm_shot_spend",
             (pOn != np && pLf != np && pFr != np && pSp != np && pFa != np && pFa < pFr && pOn < pLf && pLf < pFr && pFr < pFs &&
              pFs < pFp && pFp < pSp) ? 1u : 0u, 1u);
    expect_u("PIN ORDER: the S2 retirement is after the gate and before the spend", (pRt != np && pRt < pSp) ? 1u : 0u, 1u);
    expect_u("PIN ORDER: the S3 record and the plane record are made at the spend, apart",
             (pPp != np && pPs != np && pSp < pPp && pPp < pPs) ? 1u : 0u, 1u);
    expect_u("PIN the kext never calls a 0.0.529 fill-set step / identify / commit / retire directly (the dispatchers do)",
             count(src, "n48_fs_identify_fill(") + count(src, "n48_fs_step(") + count(src, "n48_fs_plane_step(") +
             count(src, "n48_fs_commit(") + count(src, "n48_fs_retire(") + count(src, "n48_fs_retire_withdrawn("), 0u);
    // the keystone's commit / withdrawal / exits and the arm
    const std::string hook = body_of(src, "static uint64_t hook_gfxCommitIB(");
    expect_u("PIN the S3 commit is taken at the keystone's TRANSLATED point, exactly once",
             (count(hook, "if (gFs85.s3_pend) (void)n48_fs85_s3_commit(&gFs85, seq);") == 1u &&
              count(src, "n48_fs85_s3_commit(") == 1u) ? 1u : 0u, 1u);
    expect_u("PIN the S3 record is dropped on every exit that drops the plane record (hook entry, withdrawal, token mismatch, ring full)",
             (count(hook, "gFs85.s3_pend = 0u;") == 3u && count(ct, "gFs85.s3_pend = 0u;") == 1u) ? 1u : 0u, 1u);
    expect_u("PIN the fill commit and the withdrawal go through the S1 dispatchers",
             (count(hook, "if (gFsPend.active) { (void)n48_fs85_commit(&gFs, &gFs85, gFsPend.cb0, seq); gFsPend.active = 0u; }") == 1u &&
              count(hook, "n48_fs85_retire_withdrawn(&gFs, &gFs85, gFsPend.cb0, (uint32_t)N48_DEP_SOURCE_NEUTER)") == 1u) ? 1u : 0u, 1u);
    const size_t pOpen = src.find("if (gFs.on || gFs.plane_on) n48_fs_open(&gFs);"), p85 = src.find("n48_fs85_open(&gFs85);");
    expect_u("PIN the arm opens 85's scope right after the fill-set's, exactly once",
             (count(src, "n48_fs85_open(&gFs85);") == 1u && pOpen != np && p85 != np && pOpen < p85 && p85 - pOpen < 120u) ? 1u : 0u, 1u);
    // gfx_commit.h: the guard case and the untouched invariants
    expect_u("PIN gfx_commit.h: case 85u is in n48_cm_cont_switch_guarded (PIN SWITCH-GUARD:85)",
             count(cm, "case 85u:") == 1u && count(cm, "(PIN SWITCH-GUARD:85)") == 1u ? 1u : 0u, 1u);
    // gfx_fs85.h never names the budget, N/T, the pre-plane bound or the expiries' values
    expect_u("PIN gfx_fs85.h reads no budget / N / T / pre-plane bound",
             count(hdr, "n48_cm_shot_budget_of") + count(hdr, "N48_CM_SHOT_BUDGET_MAX") + count(hdr, "N48_CM_CONT_N_MAX") +
             count(hdr, "N48_CM_PREPLANE_BOUND_US") + count(hdr, "cont_n") + count(hdr, "cont_t_us"), 0u);
    { const std::string pfx = "0x2" "8";
      const std::string f0 = pfx + "0", f2 = pfx + "2", f9 = std::string("0x2" "9") + "9";
      expect_u("PIN neither the verb nor gfx_fs85.h names a forbidden display-pipe offset",
               count(verb, f0.c_str()) + count(verb, f2.c_str()) + count(verb, f9.c_str()) + count(hdr, f0.c_str()) +
               count(hdr, f2.c_str()) + count(hdr, f9.c_str()), 0u); }
}

// =====================================================================================================================
// build 0.0.544 item 4b: THE MEMBERS AND TWINS BY CONSOLE SIZE. run11ap-r (2560x1440, judge-only): the five
// P-slot CB0s `present73: P CB0 VA 0x400100000` ... 0x404000000, and the plane frame (identity 39) refused PROVENANCE on exactly
// 0x400100000 and 0x404000000 - the 1440p member 0 and twin. 1080p is 0.0.543's set (geo 0 = the zero-initialised state).
// =====================================================================================================================
static n48_fs fs_geo(uint32_t geo, uint32_t on33, uint32_t on35)
{
    n48_fs f; std::memset(&f, 0, sizeof f);
    f.on = on33; f.plane_on = on35; n48_fs_open(&f); n48_fs_seat_geo(&f, geo);   // the kext's order: open, then re-seat
    return f;
}
static void checks_geo(const std::string &src)
{
    expect_u("4b geo: 1920x1080 -> 1080", n48_fs_geo_of(1920u, 1080u), N48_FS_GEO_1080);
    expect_u("4b geo: 2560x1440 -> 1440", n48_fs_geo_of(2560u, 1440u), N48_FS_GEO_1440);
    expect_u("4b geo: 3840x2160 -> unknown", n48_fs_geo_of(3840u, 2160u), N48_FS_GEO_UNKNOWN);
    expect_u("4b geo: 0x0 -> unknown", n48_fs_geo_of(0u, 0u), N48_FS_GEO_UNKNOWN);
    const n48_fs a = fs_geo(N48_FS_GEO_1080, 1u, 1u), z = fs_armed(1u, 1u);
    expect_u("4b 1080p: the members are 0.0.543's (kN48FsMemberVa) and a zeroed state is 1080p",
             (a.members == 2u && a.member_va[0] == kM0 && a.member_va[1] == kM1 && z.member_va[0] == kM0 && z.member_va[1] == kM1 &&
              std::memcmp(&a, &z, sizeof a) == 0) ? 1u : 0u, 1u);
    n48_fs b = fs_geo(N48_FS_GEO_1440, 1u, 1u);
    const uint64_t k1440m0 = 0x400100000ull, k1440m1 = 0x404000000ull;
    expect_u("4b 1440p: the members are run11ap-r's first surface and twin (0x400100000, 0x404000000)",
             (b.members == 2u && b.member_va[0] == k1440m0 && b.member_va[1] == k1440m1) ? 1u : 0u, 1u);
    expect_u("4b 1440p: a single-segment ColorFill of 0x400100000 is a reservable fill", n48_fs_identify_fill(&b, 1u, 1u, k1440m0), 1u);
    expect_u("4b 1440p: ...and of the twin 0x404000000", n48_fs_identify_fill(&b, 1u, 1u, k1440m1), 1u);
    expect_u("4b 1440p: a ColorFill of the 1080p surface 0x400800000 is NOT", n48_fs_identify_fill(&b, 1u, 1u, kM0), 0u);
    expect_u("4b 1440p: the step RESERVEs the 0x400100000 fill", n48_fs_step(&b, 1u, 1u, k1440m0), (uint64_t)N48_FS_RESERVE);
    expect_u("4b 1440p: the fill commits member 0", n48_fs_commit(&b, k1440m0, 7u), 1u);
    expect_u("4b 1440p: ...and a 0x404000000 fill then commits member 1", n48_fs_step(&b, 1u, 1u, k1440m1) == N48_FS_RESERVE &&
             n48_fs_commit(&b, k1440m1, 8u) == 1u && b.committed_n == 2u ? 1u : 0u, 1u);
    // switch 85's S1 at 1440p: member 1 matches only the measured twin; the empty slots never match (not even a 0 CB0)
    n48_fs c = fs_geo(N48_FS_GEO_1440, 1u, 1u);
    expect_u("4b 85 S1 1440p: member 1 matches 0x404000000", n48_fs85_member_of(&c, 1u, k1440m1), 1u);
    expect_u("4b 85 S1 1440p: member 1 does NOT match the 1080p twins", n48_fs85_member_of(&c, 1u, kM1) |
             n48_fs85_member_of(&c, 1u, kT5) | n48_fs85_member_of(&c, 1u, kT6), 0u);
    expect_u("4b 85 S1 1440p: an empty twin slot never matches a zero CB0", n48_fs85_member_of(&c, 1u, 0ull), 0u);
    const n48_fs d = fs_geo(N48_FS_GEO_1080, 1u, 1u);
    expect_u("4b 85 S1 1080p: the three twins are 0.0.543's", n48_fs85_member_of(&d, 1u, kM1) & n48_fs85_member_of(&d, 1u, kT5) &
             n48_fs85_member_of(&d, 1u, kT6), 1u);
    // an unknown size: no members - the fill window never opens (switch 33 OFF's rule), and nothing matches
    n48_fs u = fs_geo(N48_FS_GEO_UNKNOWN, 1u, 1u);
    uint32_t pass = 1u;
    for (uint32_t i = 0; i < 100u; i++) if (n48_fs_step(&u, 1u, 1u, rnd_cb0()) != N48_FS_PASS) pass = 0u;
    expect_u("4b unknown size: no members, the window closed, every frame PASSes the fill rung, no member matches",
             (u.members == 0u && !n48_fs_win_open(&u) && pass && !n48_fs85_member_of(&u, 0u, 0ull) && !n48_fs85_member_of(&u, 1u, 0ull))
                 ? 1u : 0u, 1u);
    // n48_fs_open alone is 0.0.543's (1080p) whatever geo held; the re-seat is what moves the set
    n48_fs o; std::memset(&o, 0xA5, sizeof o); o.on = 1u; o.plane_on = 1u; n48_fs_open(&o);
    expect_u("4b n48_fs_open alone seats 0.0.543's 1080p set even over a garbage state",
             (o.geo == N48_FS_GEO_1080 && o.members == 2u && o.member_va[0] == kM0 && o.member_va[1] == kM1) ? 1u : 0u, 1u);
    // the kext: the arm opens the window, then re-seats it by the console size (right after 85's open), and nowhere else
    const size_t np = std::string::npos;
    const size_t po = src.find("if (gFs.on || gFs.plane_on) n48_fs_open(&gFs);"), p85 = src.find("n48_fs85_open(&gFs85);"),
                 pg = src.find("if (gFs.on || gFs.plane_on) n48_fs_seat_geo(&gFs, fs_geo_now());");
    expect_u("4b PIN the arm re-seats the members from the console size right after the opens, exactly once",
             (po != np && p85 != np && pg != np && po < p85 && p85 < pg && pg - p85 < 400u &&
              src.find("n48_fs_seat_geo(") == pg + std::strlen("if (gFs.on || gFs.plane_on) ") &&
              src.find("n48_fs_seat_geo(", pg + 40u) == np && src.find("gFs.geo =") == np) ? 1u : 0u, 1u);
    expect_u("4b PIN fs_geo_now reads navi48_console_size through n48_fs_geo_of, else unknown",
             src.find("return navi48_console_size(w, h) ? n48_fs_geo_of(w, h) : (uint32_t)N48_FS_GEO_UNKNOWN;") != np ? 1u : 0u, 1u);
}

int main(int argc, char **argv)
{
    std::printf("== gfx_fs85: switch 85 SOURCE FILLS (notes/design/SRCFILL85.md)\n");
    checks_off_identity();
    checks_s1();
    checks_s2();
    checks_s3();
    checks_one_counter();
    checks_replays();
    checks_width();
    if (argc >= 4) checks_pins(read_file(argv[1]), read_file(argv[2]), read_file(argv[3]));
    if (argc >= 2) checks_geo(read_file(argv[1]));   // build 0.0.544 item 4b
    else { std::printf("FAIL: usage: %s AppleHardwareHook.cpp gfx_commit.h gfx_fs85.h\n", argv[0]); gFail++; }
    std::printf("gfx_fs85: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
