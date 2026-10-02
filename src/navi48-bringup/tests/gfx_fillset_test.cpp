// gfx_fillset_test.cpp — 0.0.404/0.0.405 (a items 1-7,b,): THE FIRST-SHOT FILL-SET RESERVATION'S
// PURE RULE, offline. The property under test is the onea item 7 lists, frame by frame:
//
//     fill A, fill B, plane, plane  -> the shots go A, B, plane
//     fill A, plane x200            -> expiry, then the plane
//     a non-fill under the reservation -> REFUSED
//     OFF                           -> today's order, always PASS
//
// and 0.0.405's two corrections, over the two pure predicates the kext now calls:
//   K2  n48_fs_identify_fill: exactly ONE segment AND the ColorFill PS AND an uncommitted member CB0. A TWO-SEGMENT frame
//       carrying one ColorFill draw is NOT a fill; a one-segment ColorFill on a member CB0 IS.
//   K1  n48_fs_bypass_divisor: only an IDENTIFIED member fill may bypass the policy stride; a non-fill frame under an open
//       window is sampled at today's rate.
//
// and the two properties the whole design rests on:
//   * FAIL-CLOSED: an UNIDENTIFIED frame is NOT a fill, so it is refused while the window is open;
//   * the reservation closes when BOTH members have committed OR after exactly N48_FS_EXPIRE_JUDGED judged frames, and the
//     frame that triggers expiry PASSES (expiry lifts the reservation).
//
// NON-VACUITY. The real header was broken and restored by hand during the build (the count is in the report); in addition
// a defect is planted in COPIES of every rule - the step, the identification, the divisor bypass and the retirement - and
// each one must be CAUGHT by the same checks.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_fillset_test.cpp -o /tmp/fstest && /tmp/fstest
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "gfx_fillset.h"
#include "gfx_dep.h"     // 0.0.408: the retirement is keyed on the dependency world's own reason, so the
                         // suite now uses the REAL reason code the twin fill produces, never a sentinel

static int gFail = 0, gRun = 0, gQuiet = 0;

static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) std::printf("FAIL  %-74s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want);
    } else if (!gQuiet) {
        std::printf("ok    %-74s %#llx\n", what, (unsigned long long)got);
    }
}

// The two surfaces measured; A is f1's, B is the f13-18 twin.
static const uint64_t kA = 0x400800000ull;
static const uint64_t kB = 0x404800000ull;
static const uint64_t kOther = 0x401800000ull;   // f3's fill: a real ColorFill, but NOT a set member

// 0.0.408: the retirement is no longer a comparison of the GATE's answer to a rung constant. The caller
// passes a dependency-FAILURE flag (nonzero iff the frame's dependency reading was NOT `N48_DEP_OK`, i.e. `!c.dep_ok`) and
// the dependency world's OWN reason, recorded verbatim for the report line. `kDepReason` is the REAL reason the twin fill
// produces (: its consumer never enumerates, so `n48_dep_check` answers source-neuter). `kClean` is a clean reading.
static const uint32_t kDepReason = (uint32_t)N48_DEP_SOURCE_NEUTER;
static const uint32_t kClean     = (uint32_t)N48_DEP_OK;

typedef uint32_t (*StepFn)(n48_fs *, uint32_t, uint32_t, uint64_t);
typedef uint32_t (*CommitFn)(n48_fs *, uint64_t, uint64_t);
typedef uint32_t (*RetireFn)(n48_fs *, uint64_t, uint32_t, uint32_t);
// 0.0.420 (STEP10-PLAN P1): the second window's two entry points, substitutable so the "never closes" break is caught.
typedef uint32_t (*PlaneStepFn)(n48_fs *, uint32_t, uint32_t);
typedef uint32_t (*PlaneCommitFn)(n48_fs *);

static n48_fs fs_on(uint32_t on)
{
    n48_fs f; std::memset(&f, 0, sizeof f);
    f.on = on;
    if (on) n48_fs_open(&f);
    return f;
}

// ---------------------------------------------------------------------------------------------------------------------
// item 7's scenarios, plus the fail-closed properties, over the step/commit functions handed in.
// ---------------------------------------------------------------------------------------------------------------------
static void checks_rule(StepFn step, CommitFn commit)
{
    // ---- OFF IS TODAY: every frame passes, nothing is refused, the window is never open. ----
    {
        n48_fs f = fs_on(0u);
        expect_u("OFF: the window is not open", n48_fs_win_open(&f), 0u);
        expect_u("OFF: fill A passes", step(&f, 1u, 1u, kA), N48_FS_PASS);
        expect_u("OFF: fill B passes", step(&f, 1u, 1u, kB), N48_FS_PASS);
        expect_u("OFF: a plane passes", step(&f, 1u, 0u, kOther), N48_FS_PASS);
        expect_u("OFF: an unidentified frame passes", step(&f, 1u, 0u, kA), N48_FS_PASS);
        expect_u("OFF: nothing was refused", f.refused, 0u);
        expect_u("OFF: nothing expired", f.expiry, 0u);
    }

    // ---- OPEN: the fixed set is in force and the window is open. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("OPEN: two members", f.members, 2u);
        expect_u("OPEN: member 0 is A", f.member_va[0], kA);
        expect_u("OPEN: member 1 is B", f.member_va[1], kB);
        expect_u("OPEN: the window is open", n48_fs_win_open(&f), 1u);
        expect_u("OPEN: nothing committed yet", f.committed_n, 0u);
        n48_fs_clear(&f);
        expect_u("CLOSED: the disarm closes the window", n48_fs_win_open(&f), 0u);
        expect_u("CLOSED: the members are still seated (a read-back)", f.members, 2u);
    }

    // ---- The switch turned OFF with the window still marked open must be today's order: the `on` clause is load-bearing. ----
    {
        n48_fs f = fs_on(1u);
        f.on = 0u;   // 33|0xFF clears `opened` too on the real path; this pins that the `on` test is what decides
        expect_u("off-after-open: win_open is 0", n48_fs_win_open(&f), 0u);
        expect_u("off-after-open: a fill PASSES", step(&f, 1u, 1u, kA), N48_FS_PASS);
        expect_u("off-after-open: a non-fill PASSES", step(&f, 1u, 0u, kOther), N48_FS_PASS);
        expect_u("off-after-open: nothing was refused", f.refused, 0u);
    }

    // ---- item 7 (a): fill A, fill B, plane, plane -> shots go A, B, plane. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("a: fill A is RESERVED", step(&f, 1u, 1u, kA), N48_FS_RESERVE);
        expect_u("a:   and it commits (seq 1)", commit(&f, kA, 1u), 1u);
        expect_u("a: fill B is RESERVED (the window is still open)", step(&f, 1u, 1u, kB), N48_FS_RESERVE);
        expect_u("a:   and it commits (seq 2)", commit(&f, kB, 2u), 1u);
        expect_u("a: both members committed", f.committed_n, 2u);
        expect_u("a: the window is CLOSED now", n48_fs_win_open(&f), 0u);
        expect_u("a: the plane PASSES (the third shot)", step(&f, 1u, 0u, kOther), N48_FS_PASS);
        expect_u("a: a second plane PASSES", step(&f, 1u, 0u, kA), N48_FS_PASS);
        expect_u("a: nothing was refused", f.refused, 0u);
        expect_u("a: the per-member token seqs are recorded", f.member_seq[0], 1u);
        expect_u("a:   member B's too", f.member_seq[1], 2u);
    }

    // ---- item 7 (b): fill A, plane xN -> expiry, then the plane. N is N48_FS_EXPIRE_JUDGED (60 since). ----
    {
        n48_fs f = fs_on(1u);
        expect_u("b: fill A is RESERVED", step(&f, 1u, 1u, kA), N48_FS_RESERVE);
        (void)commit(&f, kA, 1u);
        uint32_t refusedBefore = 0u, passAt = 0u;
        for (uint32_t i = 1u; i <= N48_FS_EXPIRE_JUDGED + 2u; i++) {
            const uint32_t v = step(&f, 1u, 0u, kOther);
            if (v == N48_FS_REFUSE) { refusedBefore++; continue; }
            passAt = i;   // the first PASS is the expiry frame
            break;
        }
        expect_u("b: the window expired during the planes", f.expired, 1u);
        expect_u("b:   and it was counted", f.expiry, 1u);
        expect_u("b: exactly N-1 planes were refused before expiry", refusedBefore, N48_FS_EXPIRE_JUDGED - 1u);
        expect_u("b: the Nth plane is the one that PASSES", passAt, N48_FS_EXPIRE_JUDGED);
        expect_u("b: judged is capped at the expiry bound", f.judged, N48_FS_EXPIRE_JUDGED);
        expect_u("b: the NEXT plane PASSES too (today's rule)", step(&f, 1u, 0u, kOther), N48_FS_PASS);
        expect_u("b: and the window reads closed", n48_fs_win_open(&f), 0u);
    }

    // ---- item 7 (c): a non-fill under the reservation is REFUSED and counted. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("c: a plane under the reservation is REFUSED", step(&f, 1u, 0u, kOther), N48_FS_REFUSE);
        expect_u("c:   and counted", f.refused, 1u);
        expect_u("c: an UNIDENTIFIED frame is REFUSED (fail-closed)", step(&f, 1u, 0u, kA), N48_FS_REFUSE);
        expect_u("c:   and counted", f.refused, 2u);
        expect_u("c: a fill on a NON-member surface is REFUSED", step(&f, 1u, 1u, kOther), N48_FS_REFUSE);
        expect_u("c:   and counted", f.refused, 3u);
        expect_u("c: a fill on an UNCOMMITTED member is RESERVED", step(&f, 1u, 1u, kA), N48_FS_RESERVE);
        (void)commit(&f, kA, 1u);
        expect_u("c:   the same fill again is REFUSED", step(&f, 1u, 1u, kA), N48_FS_REFUSE);
        expect_u("c:   and counted", f.refused, 4u);
        expect_u("c: the second member is still reservable", step(&f, 1u, 1u, kB), N48_FS_RESERVE);
    }

    // ---- The eligible clause: a NON-TRANSLATE frame is the gate's business, not the reservation's. It passes and is not
    //      counted as a reservation refusal, but it DOES count as a judged frame (the spec counts judged frames). ----
    {
        n48_fs f = fs_on(1u);
        expect_u("eligible: a non-eligible frame PASSES", step(&f, 0u, 1u, kA), N48_FS_PASS);
        expect_u("eligible:   and is not a reservation refusal", f.refused, 0u);
        expect_u("eligible:   but was judged", f.judged, 1u);
    }

    // ---- COMMIT is idempotent per member and refuses an unknown member. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("commit: A commits", commit(&f, kA, 9u), 1u);
        expect_u("commit: A does NOT commit twice", commit(&f, kA, 9u), 0u);
        expect_u("commit: an unknown surface does not commit", commit(&f, kOther, 9u), 0u);
        expect_u("commit: B still commits", commit(&f, kB, 10u), 1u);
        expect_u("commit: two members", f.committed_n, 2u);
        expect_u("commit: A's seq is the first", f.member_seq[0], 9u);
        expect_u("commit: B's seq is the second", f.member_seq[1], 10u);
    }

    // ---- A null state and a zero-member state read as PASS, the direction that can never spend a shot. ----
    {
        expect_u("null: step is PASS", step(nullptr, 1u, 1u, kA), N48_FS_PASS);
        expect_u("null: commit answers 0", commit(nullptr, kA, 1u), 0u);
        expect_u("null: win_open is 0", n48_fs_win_open(nullptr), 0u);
        n48_fs f; std::memset(&f, 0, sizeof f); f.on = 1u; f.opened = 1u;   // nobody seated the members
        expect_u("zero members: win_open is 0", n48_fs_win_open(&f), 0u);
        expect_u("zero members: step is PASS", step(&f, 1u, 1u, kA), N48_FS_PASS);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.405 — THE TWO THINGS 0.0.404 GOT WRONG, over the two PURE predicates the kext now calls.
//   K2  n48_fs_identify_fill: a reservable fill is ONE segment, the ColorFill PS, and an uncommitted member CB0. 0.0.404
//       admitted on the sticky per-frame identity alone, so a TWO-SEGMENT frame carrying one ColorFill draw was a fill.
//   K1  n48_fs_bypass_divisor: only a frame IDENTIFIED AS A MEMBER FILL may skip the policy stride. 0.0.404 skipped it for
//       every eligible frame while the window was open, so a NON-fill frame was admitted to the policy the stride exists to
//       sample away. A non-fill under an open window must read 0 (today's rate).
// ---------------------------------------------------------------------------------------------------------------------
typedef uint32_t (*IdentFn)(const n48_fs *, uint32_t, uint32_t, uint64_t);
typedef uint32_t (*BypassFn)(const n48_fs *, uint32_t, uint64_t);

static void checks_ident(IdentFn ident, BypassFn bypass)
{
    // ---- K2: the strict member-fill identification. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("K2 a ONE-segment ColorFill on member A IS a fill", ident(&f, 1u, 1u, kA), 1u);
        expect_u("K2 a ONE-segment ColorFill on member B IS a fill", ident(&f, 1u, 1u, kB), 1u);
        expect_u("K2 a TWO-SEGMENT frame carrying one ColorFill is NOT a fill", ident(&f, 2u, 1u, kA), 0u);
        expect_u("K2 a ZERO-segment frame is NOT a fill", ident(&f, 0u, 1u, kA), 0u);
        expect_u("K2 a NON-ColorFill one-segment frame is NOT a fill", ident(&f, 1u, 0u, kA), 0u);
        expect_u("K2 a ColorFill on a NON-member CB0 is NOT a fill", ident(&f, 1u, 1u, kOther), 0u);
        expect_u("K2 a member ALREADY committed is NOT a fill again",
                 (n48_fs_commit(&f, kA, 7u), ident(&f, 1u, 1u, kA)), 0u);
    }
    {
        n48_fs f = fs_on(0u);
        expect_u("K2 OFF: nothing is a fill even one-segment ColorFill on a member", ident(&f, 1u, 1u, kA), 0u);
        expect_u("K2 null: nothing is a fill", ident(nullptr, 1u, 1u, kA), 0u);
    }

    // ---- K1: the divisor bypass, restricted to identified member fills. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("K1 open window + identified fill on member A BYPASSES the divisor", bypass(&f, 1u, kA), 1u);
        expect_u("K1 open window + identified fill on member B BYPASSES the divisor", bypass(&f, 1u, kB), 1u);
        expect_u("K1 a NON-known-fill frame under an open window is sampled at today's rate (no bypass)",
                 bypass(&f, 0u, kA), 0u);
        expect_u("K1 an identified fill on a NON-member does not bypass", bypass(&f, 1u, kOther), 0u);
        (void)n48_fs_commit(&f, kA, 1u);
        (void)n48_fs_commit(&f, kB, 2u);
        expect_u("K1 both members committed: the window is closed, no bypass", bypass(&f, 1u, kA), 0u);
    }
    {
        n48_fs f = fs_on(1u);
        f.expired = 1u;
        expect_u("K1 an expired window: no bypass", bypass(&f, 1u, kA), 0u);
    }
    {
        n48_fs f = fs_on(0u);
        expect_u("K1 OFF: no bypass (the divisor is today's)", bypass(&f, 1u, kA), 0u);
        expect_u("K1 OFF: a non-fill does not bypass either", bypass(&f, 0u, kA), 0u);
        expect_u("K1 null: no bypass", bypass(nullptr, 1u, kA), 0u);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.406 — THE RETIREMENT.'s decisive measurement: the twin fill's consumer never enumerates, so
// its RESERVE frame never goes live (the dependency never clears); without a retirement the member is never committed, the
// window never closes, and RESERVED-FOR-FILL refuses the plane frame arm21/arm22 actually committed. These checks pin the
// three parts: the retirement closes the window; WITHOUT it the plane is refused (the counterfactual measured); and a
// retired member is not reservable again and cannot bypass the divisor.
//
// 0.0.408: the CONDITION is now the dependency READOUT (a failed dependency retires; a clean one leaves the
// member seated for a retry), not a comparison of the gate's answer to a rung. Run once for the real `n48_fs_retire`, and
// once per RETIRE mutant, so the retirement cannot be a no-op nobody notices.
// ---------------------------------------------------------------------------------------------------------------------
static void checks_retire(RetireFn retire)
{
    // ---- A committed A + a RESERVED-not-live B: without retirement the window stays open and the plane is refused. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("L2 fill A RESERVES", n48_fs_step(&f, 1u, 1u, kA), N48_FS_RESERVE);
        (void)n48_fs_commit(&f, kA, 1u);
        expect_u("L2 fill B RESERVES (window still open)", n48_fs_step(&f, 1u, 1u, kB), N48_FS_RESERVE);
        // NO retire yet: this is's measured state, and the plane must be refused by the reservation rung.
        expect_u("L2 COUNTERFACTUAL: without retirement the window stays OPEN", n48_fs_win_open(&f), 1u);
        expect_u("L2 COUNTERFACTUAL: and the plane IS refused (RESERVED-FOR-FILL's shape)",
                 n48_fs_step(&f, 1u, 0u, kOther), N48_FS_REFUSE);
        // M3 (0.0.407,): an EARLY plane candidate (f14-20 overlapping the twin fill's f13-18) may be refused once
        // while the window is open. That is a COST the prediction allows (refused-under-reservation >= 1), not a
        // falsifier - the same plane PASSES once the member's outcome closes the window (checked just below).
        expect_u("M3 the early plane's refusal is COUNTED (refused-under-reservation >= 1)", f.refused, 1u);
        // Now the retirement, as the kext does it for a RESERVE frame whose DEPENDENCY FAILED (`!c.dep_ok`, dep reason recorded).
        expect_u("L2 the not-live member is RETIRED (dependency failed)", retire(&f, kB, 1u, kDepReason), 1u);
        expect_u("L2   and the retirement is counted", f.retired, 1u);
        expect_u("L2   and the member is marked", f.member_retired[1], 1u);
        expect_u("L2   a second retirement of B does nothing", retire(&f, kB, 1u, kDepReason), 0u);
        expect_u("L2 A committed + B retired CLOSES the window", n48_fs_win_open(&f), 0u);
        expect_u("L2   so the plane now PASSES", n48_fs_step(&f, 1u, 0u, kOther), N48_FS_PASS);
        expect_u("L2 a retired member is NOT reservable again", n48_fs_identify_fill(&f, 1u, 1u, kB), 0u);
        expect_u("L2   and its CB0 does not bypass the divisor", n48_fs_bypass_divisor(&f, 1u, kB), 0u);
        expect_u("L2   and with the window CLOSED a fill on it PASSES (today's rule)", n48_fs_step(&f, 1u, 1u, kB), N48_FS_PASS);
    }
    // ---- N1 (0.0.408,): ONLY A FAILED DEPENDENCY RETIRES, AND ITS OWN REASON IS RECORDED. A clean reading means the
    //      member is left seated for a retry (the failure protected against, now expressed as the DEP readout). ----
    {
        n48_fs f = fs_on(1u);
        expect_u("N1 a CLEAN dependency does NOT retire (the member is left for a retry)",
                 retire(&f, kA, 0u, kClean), 0u);
        expect_u("N1   nothing was retired", f.retired, 0u);
        expect_u("N1   and the member is still RESERVABLE", n48_fs_identify_fill(&f, 1u, 1u, kA), 1u);
        expect_u("N1 a FAILED dependency DOES retire", retire(&f, kA, 1u, kDepReason), 1u);
        expect_u("N1   and the dependency's OWN reason is recorded for the report line", f.last_retire_reason, kDepReason);
    }
    // ---- Retirement is scoped: an unknown surface, a committed member, the switch off and null all refuse to retire. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("L2 an unknown surface cannot retire", retire(&f, kOther, 1u, kDepReason), 0u);
        (void)n48_fs_commit(&f, kA, 1u);
        expect_u("L2 a COMMITTED member cannot retire", retire(&f, kA, 1u, kDepReason), 0u);
        n48_fs off = fs_on(0u);
        expect_u("L2 OFF: nothing retires", retire(&off, kA, 1u, kDepReason), 0u);
        expect_u("L2 null: nothing retires", retire(nullptr, kA, 1u, kDepReason), 0u);
        n48_fs zero; std::memset(&zero, 0, sizeof zero); zero.on = 1u; zero.opened = 1u;   // nobody seated the members
        expect_u("L2 zero members: nothing retires", retire(&zero, kA, 1u, kDepReason), 0u);
    }
    // ---- Retiring B while B is the ONLY member left leaves the window OPEN for the other member's fill. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("L2 retire B before A is done", retire(&f, kB, 1u, kDepReason), 1u);
        expect_u("L2   the window is still OPEN for A", n48_fs_win_open(&f), 1u);
        expect_u("L2   a fill on the RETIRED member B is REFUSED while the window is open",
                 n48_fs_step(&f, 1u, 1u, kB), N48_FS_REFUSE);
        expect_u("L2   A still RESERVES", n48_fs_step(&f, 1u, 1u, kA), N48_FS_RESERVE);
        (void)n48_fs_commit(&f, kA, 2u);
        expect_u("L2   A committed + B retired -> window CLOSED", n48_fs_win_open(&f), 0u);
    }
}


// ---------------------------------------------------------------------------------------------------------------------
// 0.0.410 — THE COMMIT POINT. The corrected caller commits a reserved member at the TRANSLATED point,
// AFTER the keystone proved the arm - not at the gate RESERVE. arm26's f16 was committed at the gate and then WITHDRAWN
// by the keystone, so a member was marked committed whose commit never reached the ring and the reservation closed on a
// shot that did not happen. The driver below encodes the corrected call site; the mutant is the old gate shape.
// ---------------------------------------------------------------------------------------------------------------------
typedef void (*K2DriveFn)(n48_fs &, const uint64_t *, const uint32_t *, uint32_t);
static void drive_k2_real(n48_fs &f, const uint64_t *cb0, const uint32_t *permitted, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (n48_fs_step(&f, 1u, 1u, cb0[i]) == N48_FS_RESERVE && permitted[i])
            (void)n48_fs_commit(&f, cb0[i], (uint64_t)i + 1u);   // only when the keystone PERMITTED
}
static void drive_k2_mut(n48_fs &f, const uint64_t *cb0, const uint32_t *permitted, uint32_t n)
{
    (void)permitted;
    for (uint32_t i = 0; i < n; i++)
        if (n48_fs_step(&f, 1u, 1u, cb0[i]) == N48_FS_RESERVE)
            (void)n48_fs_commit(&f, cb0[i], (uint64_t)i + 1u);   // the old gate: commits before the keystone answers
}
static void checks_k2(K2DriveFn drive)
{
    // commit 1 = fill A, the keystone WITHDRAWS; commit 2 = fill B, the keystone PERMITS.
    n48_fs f = fs_on(1u);
    const uint64_t cb0[2] = { kA, kB };
    const uint32_t perm[2] = { 0u, 1u };
    drive(f, cb0, perm, 2u);
    expect_u("K2 fill A was RESERVED at the gate but the keystone WITHDREW it", f.member_committed[0], 0u);
    expect_u("K2   ...so fill A stays UNCOMMITTED", f.member_va[0], kA);
    expect_u("K2 fill B's keystone PERMITTED, so it IS committed", f.member_committed[1], 1u);
    expect_u("K2   ...and committed_n counts only the commit that reached the ring", f.committed_n, 1u);
    expect_u("K2   ...and the window is still open on the withdrawn member", n48_fs_win_open(&f), 1u);
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.420 (notes/design/STEP10-PLAN.md P1) — THE SECOND WINDOW: HOLD A SHOT FOR THE PLANE. Once the fill
// window closes ('s retirement, both members committed, or expiry), a TRANSLATE-eligible frame that is NOT
// plane-shaped (in-force PS `ws_D_GPUPass`) is refused until one plane frame commits or 60 judged frames pass. These are
// the plan's two named scenarios - arm29's order (fills seq 1/5, the "second pair" refused, a plane admitted) and arm32's
// order (switch 35 OFF: today's rule) - plus the OFF identity, the expiry, and the independence from switch 33.
// ---------------------------------------------------------------------------------------------------------------------
static n48_fs fs_plane(uint32_t on, uint32_t plane_on)
{
    n48_fs f; std::memset(&f, 0, sizeof f);
    f.on = on; f.plane_on = plane_on;
    if (on || plane_on) n48_fs_open(&f);
    return f;
}

static void checks_plane(PlaneStepFn step, PlaneCommitFn commit)
{
    // ---- OFF IS TODAY: neither window fires, nothing is refused, no commit lands. ----
    {
        n48_fs f = fs_plane(0u, 0u);
        expect_u("P OFF: the plane window is not open", n48_fs_plane_win_open(&f), 0u);
        expect_u("P OFF: a non-plane frame passes", step(&f, 1u, 0u), N48_FS_PASS);
        expect_u("P OFF: a plane frame passes", step(&f, 1u, 1u), N48_FS_PASS);
        expect_u("P OFF: nothing was refused", f.plane_refused, 0u);
        expect_u("P OFF: no commit lands", commit(&f), 0u);
        expect_u("P OFF: the state reads off", std::strcmp(n48_fs_plane_state(&f), "off") == 0, 1u);
    }
    // ---- THE SWITCH TURNED OFF WITH THE SCOPE STILL OPEN must be today's order: the `plane_on` clause is load-bearing. ----
    {
        n48_fs f = fs_plane(1u, 1u);
        f.plane_on = 0u;   // `gfxneuter 35 | 0xFF`: the switch clears, `opened` stays
        expect_u("P off-after-arm: win_open is 0", n48_fs_plane_win_open(&f), 0u);
        expect_u("P off-after-arm: a non-plane frame PASSES", step(&f, 1u, 0u), N48_FS_PASS);
        expect_u("P off-after-arm: nothing was refused", f.plane_refused, 0u);
    }
    // ---- arm29's order: f1 fill A (seq 1), f5 fill B (seq 5), the "second pair" refused, a plane admitted. ----
    {
        n48_fs f = fs_plane(1u, 1u);
        expect_u("arm29: while the FILL window is open the plane window is NOT", n48_fs_plane_win_open(&f), 0u);
        expect_u("arm29: f1 fill A RESERVES (fill window)", n48_fs_step(&f, 1u, 1u, kA), N48_FS_RESERVE);
        (void)n48_fs_commit(&f, kA, 1u);
        expect_u("arm29: f5 fill B RESERVES", n48_fs_step(&f, 1u, 1u, kB), N48_FS_RESERVE);
        (void)n48_fs_commit(&f, kB, 5u);
        expect_u("arm29: the FILL window is CLOSED", n48_fs_win_open(&f), 0u);
        expect_u("arm29: the PLANE window is now OPEN", n48_fs_plane_win_open(&f), 1u);
        expect_u("arm29: the second pair's first frame (IB 0x400750000) is REFUSED", step(&f, 1u, 0u), N48_FS_REFUSE);
        expect_u("arm29: the second pair's second frame is REFUSED", step(&f, 1u, 0u), N48_FS_REFUSE);
        expect_u("arm29: both refusals are counted", f.plane_refused, 2u);
        expect_u("arm29: a PLANE frame (PS ws_D_GPUPass) RESERVES", step(&f, 1u, 1u), N48_FS_RESERVE);
        expect_u("arm29: its commit CLOSES the window", commit(&f), 1u);
        expect_u("arm29: the window is closed", n48_fs_plane_win_open(&f), 0u);
        expect_u("arm29: a later non-plane frame PASSES (today's rule)", step(&f, 1u, 0u), N48_FS_PASS);
        expect_u("arm29: the state reads committed", std::strcmp(n48_fs_plane_state(&f), "committed") == 0, 1u);
        expect_u("arm29: a second plane commit answers 0", commit(&f), 0u);
    }
    // ---- arm32's order UNCHANGED: switch 35 OFF, so after the fills close a non-plane frame PASSES as 0.0.419 did. ----
    {
        n48_fs f = fs_plane(1u, 0u);
        expect_u("arm32(SW35 off): the plane window is never open", n48_fs_plane_win_open(&f), 0u);
        (void)n48_fs_commit(&f, kA, 1u);
        (void)n48_fs_commit(&f, kB, 5u);
        expect_u("arm32(SW35 off): the fill window is CLOSED", n48_fs_win_open(&f), 0u);
        expect_u("arm32(SW35 off): a non-plane frame PASSES (0.0.419's order)", step(&f, 1u, 0u), N48_FS_PASS);
        expect_u("arm32(SW35 off): nothing was refused", f.plane_refused, 0u);
    }
    // ---- INDEPENDENT OF SWITCH 33: with the fill set off the fill window is closed from the arm, so the plane window
    //      opens at once and reserves the first shot. This is P1's "hold a shot for the plane" without the fills. ----
    {
        n48_fs f = fs_plane(0u, 1u);
        expect_u("SW33 off, SW35 on: the plane window opens at the arm", n48_fs_plane_win_open(&f), 1u);
        expect_u("SW33 off, SW35 on: a non-plane frame is REFUSED", step(&f, 1u, 0u), N48_FS_REFUSE);
        expect_u("SW33 off, SW35 on: a plane frame RESERVES", step(&f, 1u, 1u), N48_FS_RESERVE);
        expect_u("SW33 off, SW35 on: the commit closes it", commit(&f), 1u);
        expect_u("SW33 off, SW35 on: the window is closed", n48_fs_plane_win_open(&f), 0u);
    }
    // ---- EXPIRY: the second window's own 60-judged-frame bound, the backstop for a boot with no eligible plane frame.
    //      The fill window is closed by retiring both members (a failed dependency), then the planes flood. ----
    {
        n48_fs f = fs_plane(1u, 1u);
        expect_u("P expiry: retire A", n48_fs_retire(&f, kA, 1u, kDepReason), 1u);
        expect_u("P expiry: retire B", n48_fs_retire(&f, kB, 1u, kDepReason), 1u);
        expect_u("P expiry: the FILL window is CLOSED", n48_fs_win_open(&f), 0u);
        expect_u("P expiry: the PLANE window is OPEN", n48_fs_plane_win_open(&f), 1u);
        uint32_t refusedBefore = 0u, passAt = 0u;
        for (uint32_t i = 1u; i <= N48_FS_PLANE_EXPIRE_JUDGED + 2u; i++) {
            const uint32_t v = step(&f, 1u, 0u);
            if (v == N48_FS_REFUSE) { refusedBefore++; continue; }
            passAt = i;   // the first PASS is the expiry frame
            break;
        }
        expect_u("P expiry: the window expired", f.plane_expired, 1u);
        expect_u("P expiry:   counted once", f.plane_expiry, 1u);
        expect_u("P expiry: exactly N frames were refused before it", refusedBefore, N48_FS_PLANE_EXPIRE_JUDGED);
        expect_u("P expiry: the N+1th frame is the one that PASSES", passAt, N48_FS_PLANE_EXPIRE_JUDGED + 1u);
        expect_u("P expiry: judged is capped at the bound", f.plane_judged, N48_FS_PLANE_EXPIRE_JUDGED);
        expect_u("P expiry: the state reads expired", std::strcmp(n48_fs_plane_state(&f), "expired") == 0, 1u);
        expect_u("P expiry: the NEXT frame PASSES too (today's rule)", step(&f, 1u, 0u), N48_FS_PASS);
    }
    // ---- The eligible clause: a NON-TRANSLATE frame is the gate's business, not the window's. It passes un-counted but
    //      IS judged (the spec counts judged frames). ----
    {
        n48_fs f = fs_plane(0u, 1u);
        expect_u("P eligible: a non-eligible frame PASSES", step(&f, 0u, 0u), N48_FS_PASS);
        expect_u("P eligible:   not a window refusal", f.plane_refused, 0u);
        expect_u("P eligible:   but judged", f.plane_judged, 1u);
    }
    // ---- Null and unarmed states read as PASS, the direction that can never spend a shot. ----
    {
        expect_u("P null: step is PASS", step(nullptr, 1u, 1u), N48_FS_PASS);
        expect_u("P null: commit answers 0", commit(nullptr), 0u);
        expect_u("P null: win_open is 0", n48_fs_plane_win_open(nullptr), 0u);
        n48_fs z; std::memset(&z, 0, sizeof z); z.plane_on = 1u;   // the switch on but nobody opened the scope
        expect_u("P unarmed: win_open is 0", n48_fs_plane_win_open(&z), 0u);
        expect_u("P unarmed: step is PASS", step(&z, 1u, 0u), N48_FS_PASS);
        expect_u("P unarmed: the state reads closed", std::strcmp(n48_fs_plane_state(&z), "closed") == 0, 1u);
    }
}

// THE PLANTED BREAKS. Each is a plausible way to write the second window wrong; each must be CAUGHT by checks_plane.
// P1: THE WINDOW NEVER CLOSES - expiry is dropped and a non-plane frame is refused for the rest of the arm. This is the
//     failure the brief names ("the window never closes -> FAIL"): the plane's shot is held forever, so a boot whose
//     eligible plane frame never reaches the gate loses every later frame. The arm29 "later frame PASSES" and the whole
//     expiry block must fail.
static uint32_t mp1_never_closes(n48_fs *f, uint32_t eligible, uint32_t is_plane)
{
    if (!f || !f->plane_on || !f->opened) return N48_FS_PASS;
    if (f->plane_committed) return N48_FS_PASS;
    f->plane_judged++;
    if (!eligible) return N48_FS_PASS;
    if (is_plane) return N48_FS_RESERVE;
    f->plane_refused++;
    return N48_FS_REFUSE;
}
// P2: THE SWITCH IS IGNORED - it fires on `opened` alone, so with `gfxneuter 35` OFF a frame is refused and the default
//     path is no longer 0.0.419's. The byte-identity clause, broken.
static uint32_t mp2_ignores_switch(n48_fs *f, uint32_t eligible, uint32_t is_plane)
{
    if (!f || !f->opened) return N48_FS_PASS;
    if (f->plane_committed || f->plane_expired) return N48_FS_PASS;
    if (n48_fs_win_open(f)) return N48_FS_PASS;
    if ((uint32_t)f->plane_judged + 1u > N48_FS_PLANE_EXPIRE_JUDGED) {
        f->plane_expired = 1u; f->plane_expiry++; return N48_FS_PASS;
    }
    f->plane_judged++;
    if (!eligible) return N48_FS_PASS;
    if (is_plane) return N48_FS_RESERVE;
    f->plane_refused++;
    return N48_FS_REFUSE;
}

// ---------------------------------------------------------------------------------------------------------------------
// THE ONE REPORT LINE fits the brief's 480-body-byte cap at its widest arguments. Not mutated: it is a width proof.
// ---------------------------------------------------------------------------------------------------------------------
static void report_width()
{
    char b[4096];
    const int n = std::snprintf(b, sizeof b, N48_FILLSET_FMT, "OFF (default)", "`gfxneuter 33` CHANGED it",
                                N48_FS_MEMBERS, 0xffffffffffffffffull, "uncommitted", 0xffffffffu,
                                0xffffffffffffffffull, "uncommitted", 0xffffffffu,
                                N48_FS_MEMBERS, N48_FS_MEMBERS, 0xffffffffffffffffull, 0xffffffffffffffffull,
                                0xffffffffu, "OPEN", 0xffffffffffffffffull, "neuter-unreadable-writeset",
                                // 0.0.420 (P1): the second window's five fields at THEIR worst case. The state word's
                                // longest is n48_fs_plane_state's "committed" (9 B); the four counters are uint16_t, so
                                // 65535 (5 digits) is the type's own bound - the exact `%u` worst case in the format.
                                "committed", 0xffffu, 0xffffu, 0xffffu, 0xffffu);
    if (!gQuiet) std::printf("      fillset line worst case: %d bytes (cap %u)\n", n, N48_FILLSET_BODY_CAP);
    expect_u("the fillset line fits under 480 body bytes", n > 0 && (unsigned)n < N48_FILLSET_BODY_CAP, 1);
    // Every field item 6 asks for must still be named - a line shortened until it fits by dropping one would pass above.
    // added the retirement count and the retiring reason; both are named too, so the line cannot lose
    // either. The worst-case filler above is the LONGEST string n48_dep_reason_name can return. 0.0.420 (P1)
    // adds the second window's five fields, named here so a future shortening cannot drop one.
    const char *fmt = N48_FILLSET_FMT;
    bool all = true;
    for (const char *t : { "fillset:", "member(s)", "committed %u of %u", "refused-under-reservation", "expired", "judged",
                           "retired", "retire reason", "plane win", "committed %u, refused %u, expired %u, judged %u" })
        if (!std::strstr(fmt, t)) all = false;
    expect_u("and it names the members, the commits, the refusals, the expiry, the retirement, its reason and the plane window",
             all, 1);
}

// ---------------------------------------------------------------------------------------------------------------------
// THE PLANTED DEFECTS. Each is a plausible way to write the rule wrong; each must be CAUGHT by checks_rule.
// ---------------------------------------------------------------------------------------------------------------------
// M1: THE RESERVATION NEVER REFUSES - the fail-open shape. Every frame passes while the window is open, so the plane
//     spends the first shot and the twin fill is never reached.
static uint32_t m1_step(n48_fs *f, uint32_t e, uint32_t isf, uint64_t cb0)
{ (void)f; (void)e; (void)isf; (void)cb0; return N48_FS_PASS; }
// M2: EXPIRY NEVER FIRES - `judged` counts but the window never closes on its own, so a boot whose twin fill never comes
//     refuses the plane forever.
static uint32_t m2_step(n48_fs *f, uint32_t eligible, uint32_t is_fill, uint64_t cb0)
{
    if (!f || !f->on || !f->opened) return N48_FS_PASS;
    if (f->expired || f->committed_n >= f->members) return N48_FS_PASS;
    f->judged++;
    if (!eligible) return N48_FS_PASS;
    if (is_fill)
        for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++)
            if (!f->member_committed[m] && f->member_va[m] == cb0) return N48_FS_RESERVE;
    f->refused++;
    return N48_FS_REFUSE;
}
// M3: OFF IS NOT HONOURED - the rule fires with the switch off, so the default path is not 0.0.403's (the byte-identity
//     clause, broken).
static uint32_t m3_step(n48_fs *f, uint32_t eligible, uint32_t is_fill, uint64_t cb0)
{
    if (!f || !f->opened) return N48_FS_PASS;
    if (f->expired || f->committed_n >= f->members) return N48_FS_PASS;
    if (f->judged + 1u > N48_FS_EXPIRE_JUDGED) { f->expired = 1u; f->expiry++; return N48_FS_PASS; }
    f->judged++;
    if (!eligible) return N48_FS_PASS;
    if (is_fill)
        for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++)
            if (!f->member_committed[m] && f->member_va[m] == cb0) return N48_FS_RESERVE;
    f->refused++;
    return N48_FS_REFUSE;
}
// M4: A FILL IS ADMITTED ON ITS IDENTITY ALONE - the CB0 membership test dropped, so a ColorFill painting a surface we did
//     NOT reserve spends a shot the design needed for the twin.
static uint32_t m4_step(n48_fs *f, uint32_t eligible, uint32_t is_fill, uint64_t cb0)
{
    if (!f || !f->on || !f->opened) return N48_FS_PASS;
    if (f->expired || f->committed_n >= f->members) return N48_FS_PASS;
    if (f->judged + 1u > N48_FS_EXPIRE_JUDGED) { f->expired = 1u; f->expiry++; return N48_FS_PASS; }
    f->judged++;
    if (!eligible) return N48_FS_PASS;
    (void)cb0;
    if (is_fill) return N48_FS_RESERVE;
    f->refused++;
    return N48_FS_REFUSE;
}
// M5: THE ELIGIBLE CLAUSE IS DROPPED - a NON-TRANSLATE frame is refused under the reservation, changing the gate's own
//     reason for a frame the reservation has no business speaking about.
static uint32_t m5_step(n48_fs *f, uint32_t eligible, uint32_t is_fill, uint64_t cb0)
{
    (void)eligible;
    if (!f || !f->on || !f->opened) return N48_FS_PASS;
    if (f->expired || f->committed_n >= f->members) return N48_FS_PASS;
    if (f->judged + 1u > N48_FS_EXPIRE_JUDGED) { f->expired = 1u; f->expiry++; return N48_FS_PASS; }
    f->judged++;
    if (is_fill)
        for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++)
            if (!f->member_committed[m] && f->member_va[m] == cb0) return N48_FS_RESERVE;
    f->refused++;
    return N48_FS_REFUSE;
}

// M6 (0.0.405,): the identification DROPS the single-segment clause - the 0.0.404 shape, where a multi-segment frame
//     carrying one ColorFill draw is taken as a fill (fail-open).
static uint32_t m6_ident(const n48_fs *f, uint32_t nseg, uint32_t ps_fill, uint64_t cb0)
{
    (void)nseg;
    if (!f || !f->on || !f->opened) return 0u;
    if (f->expired || f->committed_n >= f->members) return 0u;
    if (!ps_fill) return 0u;
    for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++)
        if (!f->member_committed[m] && f->member_va[m] == cb0) return 1u;
    return 0u;
}
// M7 (0.0.405,): the bypass DROPS the identified-fill flag - every frame whose CB0 is a member bypasses the divisor
//     while the window is open, the 0.0.404 shape, so a non-fill frame is admitted to the policy the stride exists to skip.
static uint32_t m7_bypass(const n48_fs *f, uint32_t frame_fill, uint64_t cb0)
{
    (void)frame_fill;
    if (!f || !f->on || !f->opened) return 0u;
    if (f->expired || f->committed_n >= f->members) return 0u;
    for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++)
        if (!f->member_committed[m] && f->member_va[m] == cb0) return 1u;
    return 0u;
}

// M8 (0.0.406,): THE RETIREMENT IS A NO-OP - a member whose RESERVE frame did not go live is left for a retry.
//     This is the exact arm22 defect measured: the twin fill never commits, the window never closes, and
//     RESERVED-FOR-FILL refuses the plane frame that arm21/arm22 actually committed.
static uint32_t m8_retire(n48_fs *f, uint64_t cb0, uint32_t dep_failed, uint32_t dep_reason)
{ (void)f; (void)cb0; (void)dep_failed; (void)dep_reason; return 0u; }
// M9 (0.0.408,): THE RETIREMENT IGNORES THE DEPENDENCY - a CLEAN dependency retires a member that should be left
//     seated for a retry, throwing away a reserved shot. This is the 0.0.406 shape M2 was written to prevent, now keyed on
//     the readout rather than on the gate's answer.
static uint32_t m9_retire(n48_fs *f, uint64_t cb0, uint32_t dep_failed, uint32_t dep_reason)
{
    (void)dep_failed; (void)dep_reason;
    if (!f || !f->on) return 0u;
    for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++) {
        if (!f->member_committed[m] && !f->member_retired[m] && f->member_va[m] == cb0) {
            f->member_retired[m] = 1u; f->retired_n++; f->retired++; return 1u;
        }
    }
    return 0u;
}

// =====================================================================================================================
// build 0.0.497 — RUN F's REAL SEQUENCE THROUGH THE KEXT'S OWN ORDER (run10k
// driverlog-stream): switch 33 ON and switch 35 ON (the plane window), members {0x400800000, 0x404800000}.
//   f1  (10406-10416) one-segment ColorFill of 0x400800000: RESERVE; the gate answered COMMIT (live) so the gate RECORDED
//       it (gFsPend); the keystone then WITHDREW it (VERDICT 15) - the refusal branch: 64 ON retires the member.
//   f2  (10737-10740) 0x400800000, TRANSLATE-eligible, NOT a fill, dependency clean: the gate said RESERVED-FOR-FILL.
//   f16 (12938-12941) the twin's frame, 0x404800000: RESERVE (a fill of an open member), NOT live - its dependency failed
//       (source-neuter) - so n48_fs_retire gives the twin up (the fillset line at 41621 reads both members RETIRED).
//   f17 (13100-13107) 0x404800000, eligible, NOT a fill (the twin is retired), dependency clean: RESERVED-FOR-FILL in RUN F.
// The gate's rung order is modelled exactly (gfx_commit.h n48_cm_gate): RESERVED-FOR-FILL = `fs_switch && fs_open &&
// !fs_reserve`, then RESERVED-FOR-PLANE = `fp_switch && fp_open && !fp_reserve`; fs_open / fp_open are read AFTER the
// frame's own step, exactly as the kext fills c.fs_open / c.fp_open. The kext's keystone refusal branch calls
// n48_fs_retire_withdrawn only when switch 64 is ON (latched), and otherwise just drops the record.
// =====================================================================================================================
typedef uint32_t (*RetireWFn)(n48_fs *, uint64_t, uint32_t);
enum { RF_PASS = 0, RF_FILL = 1, RF_PLANE = 2 };
struct RfFrame { uint32_t fill, plane; uint64_t cb0; uint32_t live, depFailed; };
static uint32_t rf_gate(n48_fs &f, const RfFrame &fr)
{
    const uint32_t isFill = n48_fs_identify_fill(&f, 1u, fr.fill, fr.cb0);
    const uint32_t fsStep = n48_fs_step(&f, 1u, isFill, fr.cb0);
    const uint32_t fsOpen = n48_fs_win_open(&f);
    const uint32_t fpStep = n48_fs_plane_step(&f, 1u, fr.plane);
    const uint32_t fpOpen = n48_fs_plane_win_open(&f);
    const uint32_t fsReserve = fsStep == N48_FS_RESERVE, fpReserve = fpStep == N48_FS_RESERVE;
    if (fsReserve && !fr.live) (void)n48_fs_retire(&f, fr.cb0, fr.depFailed, kDepReason);
    if (f.on && fsOpen && !fsReserve) return RF_FILL;
    if (f.plane_on && fpOpen && !fpReserve) return RF_PLANE;
    return RF_PASS;
}
// f1 through the gate (RESERVE, live) and then the keystone's refusal branch.
static void rf_f1_withdrawn(n48_fs &f, uint32_t sw64, RetireWFn retireW)
{
    const RfFrame f1 { 1u, 0u, kA, 1u, 0u };
    const uint32_t g = rf_gate(f, f1);
    expect_u("RF f1: the reserved fill of 0x400800000 passes the reservation rung (RESERVE)", g, RF_PASS);
    const uint32_t pendActive = (g == RF_PASS) ? 1u : 0u; const uint64_t pendCb0 = kA;   // the gate RECORDED it (gFsPend), it did not commit it
    if (sw64 && pendActive) (void)retireW(&f, pendCb0, kDepReason);   // the kext's refusal branch, VERBATIM in shape
}
static void checks_runf(RetireWFn retireW)
{
    const RfFrame f2 { 0u, 0u, kA, 1u, 0u }, f16 { 1u, 0u, kB, 0u, 1u }, f17 { 0u, 0u, kB, 1u, 0u };
    // ---- 64 OFF: TODAY. The positive control reproduces RUN F's two logged refusals. ----
    {
        n48_fs f = fs_on(1u); f.plane_on = 1u; n48_fs_open(&f);
        rf_f1_withdrawn(f, 0u, retireW);
        expect_u("RF 64 OFF: after f1's withdrawal member 0x400800000 is neither committed nor retired",
                 f.member_committed[0] + f.member_retired[0], 0u);
        expect_u("RF 64 OFF: f2 is RESERVED-FOR-FILL (RUN F 10738, the positive control)", rf_gate(f, f2), RF_FILL);
        (void)rf_gate(f, f16);
        expect_u("RF 64 OFF: f16 retires the twin (dependency source-neuter)", f.member_retired[1], 1u);
        expect_u("RF 64 OFF: f17 is RESERVED-FOR-FILL (RUN F 13102): the withdrawn member still holds the window",
                 rf_gate(f, f17), RF_FILL);
    }
    // ---- 64 ON: (B). ----
    {
        n48_fs f = fs_on(1u); f.plane_on = 1u; n48_fs_open(&f);
        rf_f1_withdrawn(f, 1u, retireW);
        expect_u("RF 64 ON (B): member 0x400800000 is RETIRED at the keystone withdrawal", f.member_retired[0], 1u);
        expect_u("RF 64 ON (B): ...and is NOT committed", f.member_committed[0], 0u);
        expect_u("RF 64 ON (B): ...committed_n stays 0 (never counted committed)", f.committed_n, 0u);
        expect_u("RF 64 ON (B): ...no seq recorded for it", f.member_seq[0], 0u);
        expect_u("RF 64 ON (B): ...the twin is untouched (only the withdrawn member moves)", f.member_retired[1] + f.member_committed[1], 0u);
        expect_u("RF 64 ON (B): ...a later commit of 0x400800000 cannot mark it committed", n48_fs_commit(&f, kA, 9u), 0u);
        expect_u("RF 64 ON (B): ...and a later ColorFill of it is no longer a reservable fill",
                 n48_fs_identify_fill(&f, 1u, 1u, kA), 0u);
        expect_u("RF 64 ON (B): ...the recorded reason is the withdrawn frame's own (source-neuter)", f.last_retire_reason,
                 kDepReason);
        // THE BRIEF EXPECTED f2 TO PASS THE RESERVATION RUNG HERE. It does not, on RUN F's real two-member set: the twin
        // 0x404800000 is still open (committed 0 + retired 1 < 2 members), so the window stays open and f2 - a non-fill -
        // is refused exactly as today. This check records the TRUE answer so a reader cannot mistake (B) for more than it is.
        expect_u("RF 64 ON (B): f2 is STILL RESERVED-FOR-FILL - the twin member holds the window (NOT the brief's PASS)",
                 rf_gate(f, f2), RF_FILL);
        (void)rf_gate(f, f16);
        expect_u("RF 64 ON (B): f16 retires the twin; with both members terminal the fill window CLOSES", n48_fs_win_open(&f), 0u);
        // f17 now passes the FILL rung (the liveness (B) buys on RUN F's sequence) - and meets switch 35's plane window,
        // which opened when the fill window closed and holds the shot for a plane-shaped frame (P1's design).
        expect_u("RF 64 ON (B): f17 passes the FILL rung, and is held RESERVED-FOR-PLANE by switch 35 (not plane-shaped)",
                 rf_gate(f, f17), RF_PLANE);
        n48_fs g = fs_on(1u); n48_fs_open(&g);   // the same with switch 35 OFF: f17 passes both rungs
        rf_f1_withdrawn(g, 1u, retireW); (void)rf_gate(g, f2); (void)rf_gate(g, f16);
        expect_u("RF 64 ON (B), 35 OFF: f17 PASSES the reservation rungs (its own rungs and the keystone still apply)",
                 rf_gate(g, f17), RF_PASS);
    }
    // ---- 64 ON: where the brief's f2 PASS does hold - the twin is already terminal when f1 is withdrawn. ----
    {
        n48_fs f = fs_on(1u); n48_fs_open(&f);
        (void)n48_fs_commit(&f, kB, 1u);   // the twin committed earlier (the shape the brief's expectation assumes)
        rf_f1_withdrawn(f, 1u, retireW);
        expect_u("RF 64 ON (B), twin already committed: the window CLOSES at f1's withdrawal", n48_fs_win_open(&f), 0u);
        expect_u("RF 64 ON (B), twin already committed: f2 PASSES the reservation rung", rf_gate(f, f2), RF_PASS);
        expect_u("RF 64 ON (B), twin already committed: committed_n is the twin's 1 only", f.committed_n, 1u);
    }
    // ---- the pure function on its own: only the named member, never a committed one, OFF inert. ----
    {
        n48_fs f = fs_on(1u);
        expect_u("RW retire the TWIN by its CB0: answers 1", retireW(&f, kB, kDepReason), 1u);
        expect_u("RW   ...the twin is retired", f.member_retired[1], 1u);
        expect_u("RW   ...member 0x400800000 is untouched", f.member_retired[0] + f.member_committed[0], 0u);
        expect_u("RW a second retirement of the same member answers 0", retireW(&f, kB, kDepReason), 0u);
        expect_u("RW a CB0 that is no member answers 0 and moves nothing", retireW(&f, kOther, kDepReason), 0u);
        expect_u("RW   ...retired_n is 1", f.retired_n, 1u);
        n48_fs c = fs_on(1u);
        (void)n48_fs_commit(&c, kA, 4u);
        expect_u("RW a COMMITTED member is never retired", retireW(&c, kA, kDepReason), 0u);
        expect_u("RW   ...it stays committed with its seq", c.member_committed[0] == 1u && c.member_seq[0] == 4u, 1u);
        n48_fs o = fs_on(0u);
        expect_u("RW switch 33 OFF: never retires", retireW(&o, kA, kDepReason), 0u);
    }
}
// Copies of n48_fs_retire_withdrawn with the two fail-open defects the brief names, so the checks above are shown to
// catch them in-suite (plant.py plants the same two in the real header).
static uint32_t mw_commits(n48_fs *f, uint64_t cb0, uint32_t reason)
{
    (void)reason;
    return n48_fs_commit(f, cb0, 1u);   // the withdrawn member marked COMMITTED
}
static uint32_t mw_wrong_member(n48_fs *f, uint64_t cb0, uint32_t reason)
{
    if (!f || !f->on) return 0u;
    for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++) {
        if (!f->member_committed[m] && !f->member_retired[m] && f->member_va[m] != cb0) {   // the WRONG member
            f->member_retired[m] = 1u; f->retired_n++; f->retired++; f->last_retire_reason = reason; return 1u;
        }
    }
    return 0u;
}

/* build 0.0.515 D1: THE FILL READ ON THE DIVISOR. The kext reads a fragment program for the identified-fill
 * flag only when n48_fs_fill_read_wanted says so; the flag's one reader is the divisor's skip
 *     skip = !(armCommit && n48_fs_bypass_divisor(&gFs, flag, cb0)) && ((eligible++ % every) != 0)
 * (AppleHardwareHook.cpp, `else if` before `gXdC.policySkippedSample++`). The differential drives that expression over a mixed
 * stream of fill / non-fill, member / non-member frames twice - once with the flag as 0.0.514 computed it (read whenever switch
 * 33 is on), once as 0.0.515 does - and requires the SAME run/skip answer for every frame at every = 1 (the verdicts and
 * actions downstream read nothing else of the flag), and that at every = 8 the flag still sets for a fill. */
typedef uint32_t (*ReadFn)(uint32_t, uint32_t, uint32_t, uint32_t);
static uint32_t d1_forced(uint32_t on, uint32_t stage, uint32_t ff, uint32_t every) { (void)every; return (on && stage == 0u && !ff) ? 1u : 0u; }
static void checks_d1(ReadFn want)
{
    expect_u("D1 every 1: no read (switch 33 on, the PS, flag clear)", want(1u, 0u, 0u, 1u), 0u);
    expect_u("D1 every 0 (treated as 1): no read", want(1u, 0u, 0u, 0u), 0u);
    expect_u("D1 every 8: the read", want(1u, 0u, 0u, 8u), 1u);
    expect_u("D1 every 2: the read", want(1u, 0u, 0u, 2u), 1u);
    expect_u("D1 switch 33 off: no read", want(0u, 0u, 0u, 8u), 0u);
    expect_u("D1 not the fragment stage: no read", want(1u, 1u, 0u, 8u), 0u);
    expect_u("D1 flag already set this frame: no read", want(1u, 0u, 1u, 8u), 0u);
    // the differential over the divisor's own expression
    for (uint32_t every : { 1u, 8u }) {
        n48_fs f {};
        f.on = 1u;
        n48_fs_open(&f);   // the reservation an arm opens: its members are kN48FsMemberVa[]
        const uint64_t cb0s[2] = { f.member_va[0], f.member_va[1 % N48_FS_MEMBERS] };
        uint64_t elOld = 0, elNew = 0;
        uint32_t same = 1u, setNew = 0u, runs = 0u;
        for (uint32_t i = 0; i < 400u; i++) {
            const uint32_t isFill = (i % 3u) == 0u, member = (i % 5u) != 4u, arm = (i % 7u) != 0u;
            const uint64_t cb0 = member ? cb0s[i & 1u] : 0x401800000ull;
            const uint32_t flagOld = (f.on && isFill) ? 1u : 0u;                              // 0.0.514: read whenever 33 is on
            const uint32_t flagNew = (want(f.on, 0u, 0u, every) && isFill) ? 1u : 0u;         // 0.0.515
            const bool skipOld = !(arm && n48_fs_bypass_divisor(&f, flagOld, cb0)) && ((elOld++ % every) != 0u);
            const bool skipNew = !(arm && n48_fs_bypass_divisor(&f, flagNew, cb0)) && ((elNew++ % every) != 0u);
            if (every == 1u && skipOld != skipNew) same = 0u;
            if (!skipNew) runs++;
            if (flagNew) setNew++;
        }
        char lbl[160];
        if (every == 1u) {
            expect_u("D1 differential at 1 in 1: every frame's run/skip identical to 0.0.514's (400 frames)", same, 1u);
            std::snprintf(lbl, sizeof lbl, "D1 ... and the policy runs for all of them (runs %u)", runs);
            expect_u(lbl, runs, 400u);
            expect_u("D1 ... and the flag is never computed (no read)", setNew, 0u);
        } else {
            std::snprintf(lbl, sizeof lbl, "D1 at 1 in 8 the fill flag still sets for fills (%u of 400 frames)", setNew);
            expect_u(lbl, setNew > 0u, 1u);
        }
    }
}

int main()
{
    std::printf("== gfx_fillset: the first-shot fill-set reservation, and the defects it exists for ==\n");
    checks_rule(&n48_fs_step, &n48_fs_commit);
    checks_ident(&n48_fs_identify_fill, &n48_fs_bypass_divisor);
    checks_retire(&n48_fs_retire);
    checks_k2(&drive_k2_real);
    checks_plane(&n48_fs_plane_step, &n48_fs_plane_commit);
    checks_runf(&n48_fs_retire_withdrawn);   // build 0.0.497 (B)
    report_width();
    checks_d1(&n48_fs_fill_read_wanted);   // build 0.0.515 D1
    const int realFail = gFail, realRun = gRun;
    std::printf("-- %d check(s), %d failure(s)\n", realRun, realFail);

    struct Mut { const char *what; StepFn step; CommitFn commit; };
    const Mut mut[] = {
        { "M1 the reservation NEVER REFUSES (the fail-open shape)", &m1_step, &n48_fs_commit },
        { "M2 EXPIRY NEVER FIRES (the plane is refused forever)", &m2_step, &n48_fs_commit },
        { "M3 OFF is not honoured (the default is not 0.0.403's)", &m3_step, &n48_fs_commit },
        { "M4 a fill is admitted on identity ALONE (CB0 ignored)", &m4_step, &n48_fs_commit },
        { "M5 the ELIGIBLE clause is dropped (non-TRANSLATE frames refused)", &m5_step, &n48_fs_commit },
    };
    int caught = 0;
    for (const Mut &m : mut) {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_rule(m.step, m.commit);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    // K1/K2 mutants: the two 0.0.404 defects, planted in COPIES of the pure predicates.
    struct Mut2 { const char *what; IdentFn ident; BypassFn bypass; };
    const Mut2 mut2[] = {
        { "M6 the identification IGNORES the segment count (a multi-segment ColorFill is a fill)", &m6_ident, &n48_fs_bypass_divisor },
        { "M7 the bypass IGNORES the identified-fill flag (every member-CB0 frame bypasses)", &n48_fs_identify_fill, &m7_bypass },
    };
    for (const Mut2 &m : mut2) {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_ident(m.ident, m.bypass);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    // L2 mutants (0.0.406/0.0.408,): the retirement is a no-op, or it ignores the dependency readout.
    struct Mut3 { const char *what; RetireFn retire; };
    const Mut3 mut3[] = {
        { "M8 the retirement is a NO-OP (a not-live member never gives up)", &m8_retire },
        { "M9 the retirement IGNORES the dependency (a clean reading retires)", &m9_retire },
    };
    for (const Mut3 &m : mut3) {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_retire(m.retire);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    // K2 mutant (0.0.410,): the commit is taken at the GATE RESERVE again, so a keystone-withdrawn commit is
    // counted - the exact arm26 f16 shape. It is a DRIVER mutant (the header is unchanged), the way the K2 defect is.
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_k2(&drive_k2_mut);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n",
                    "M10 the commit is taken at the GATE (a withdrawn commit counts)", f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    // P1/P2 (0.0.420): the second window's own planted breaks - the window never closes (the brief's own break) and the
    // switch is ignored. Both run through the REAL n48_fs_plane_commit, so only the step is mutated.
    struct Mut4 { const char *what; PlaneStepFn step; };
    const Mut4 mut4[] = {
        { "P1 the second window NEVER CLOSES (the plane's shot is held forever)", &mp1_never_closes },
        { "P2 the second window IGNORES ITS SWITCH (breaks the default)",         &mp2_ignores_switch },
    };
    for (const Mut4 &m : mut4) {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_plane(m.step, &n48_fs_plane_commit);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    // build 0.0.497 (B): the keystone-withdrawal retirement's two fail-open defects.
    struct Mut5 { const char *what; RetireWFn retire; };
    const Mut5 mut5[] = {
        { "W1 the withdrawn member is marked COMMITTED (fail-open)", &mw_commits },
        { "W2 the WRONG member is retired (fail-open)",             &mw_wrong_member },
    };
    for (const Mut5 &m : mut5) {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_runf(m.retire);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", m.what, f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    // build 0.0.515 D1: the planted break - the fill read forced back on (0.0.514's condition).
    {
        gQuiet = 1; gFail = 0; gRun = 0;
        checks_d1(&d1_forced);
        const int f = gFail, r = gRun;
        gQuiet = 0;
        std::printf("mutant %-56s %s (%d of %d checks fail)\n", "D1 the fill read forced back on at 1 in 1", f ? "CAUGHT" : "NOT CAUGHT", f, r);
        if (f) caught++;
    }
    const int nmut = (int)(sizeof(mut) / sizeof(mut[0])) + (int)(sizeof(mut2) / sizeof(mut2[0])) +
                     (int)(sizeof(mut3) / sizeof(mut3[0])) + 1 + (int)(sizeof(mut4) / sizeof(mut4[0])) +
                     (int)(sizeof(mut5) / sizeof(mut5[0])) + 1;
    std::printf("mutants caught %d/%d\n", caught, nmut);
    std::printf("%s\n", (realFail == 0 && caught == nmut) ? "gfx_fillset: PASS" : "gfx_fillset: FAIL");
    return (realFail || caught != nmut) ? 1 : 0;
}
