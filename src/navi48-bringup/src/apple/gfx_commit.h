// gfx_commit.h — X1: THE COMMIT GATE. What must be true before a translated frame is allowed to reach the CP, expressed as data
// (0.0.352). Pure C, host-tested by tests/gfx_commit_test.cpp; the kext compiles the SAME header.
//
// WHY THIS EXISTS AS A SEPARATE HEADER, AND WHY IT IS DATA RATHER THAN CODE.
// `gfx_src_decide.h`'s n48_sd_action already says the rule in one line: at COMMIT, a frame translates only when the verdict is
// TRANSLATE **and** `commit_ok`. Its comment has said since 0.0.302 that "`commit_ok` is 1 only when the caller has ALREADY
// rewritten the IB and read it back", and until 0.0.351 the caller passed the literal `0u` — so the sentence was a promise, not a
// mechanism. This header is the mechanism. Everything the caller learns while rewriting is written into ONE zero-initialised
// struct, and `n48_cm_gate` refuses unless EVERY field was positively set to the value that means "verified". A field nobody
// filled is 0, and 0 refuses at every single rung. That is what "fail-closed by construction" means here: there is no path
// through this function that reaches N48_CM_OK by omission, and a caller that forgets a step cannot get a commit by default.
//
// THE MUTANT THIS IS BUILT AGAINST, in the project's own words: **"`commit_ok` with a rewrite that did not read back clean" MUST
// NEUTER.** It is the first thing tests/gfx_commit_test.cpp checks, exhaustively — every single-dword corruption of a real IB
// length, plus a short read, plus a read that crossed a page that was not host memory — and each one must come out of
// n48_sd_action as N48_SD_ACT_NEUTER, not as a translate. An untranslated (or half-translated) IB reaching the CP is what hung
// the engine in `live2`; the neuter is the fallback and it works.
//
// TWO ASYMMETRIES THIS DELIBERATELY DOES NOT INHERIT (both found by the review of the ladder):
//   1. `n48_xv_frame.target_vram` is fail-OPEN on an unresolvable target — AppleHardwareHook.cpp `if (ok && !isSys)
//      f.target_vram = 1;` sets no flag when the page walk FAILS, so a target we could not resolve does not refuse. Here the
//      opposite holds and is checked twice: `pages` must be non-zero and `sys_pages` must EQUAL it, so a page that did not
//      resolve, or resolved to VRAM, refuses. An unresolvable anything refuses.
//   2. The IB policy runs over IB 0 only (`gfxsrc_policy(vm, &f, f.ib[0].got)`), while a frame may carry up to four IBs and the
//      ladder judges the shape of all of them. A multi-IB frame let through would send IBs 1..3 to the CP **untranslated** —
//      exactly the shape that hung the engine. `nib` must be 1. Nothing else in the ladder says this.
#ifndef N48_GFX_COMMIT_H
#define N48_GFX_COMMIT_H

#include <stdint.h>
#include "gfx_src_decide.h"

/* Refusal reasons, in the order the gate tests them. 0 is the only value that commits. */
enum {
    N48_CM_OK = 0,
    N48_CM_NOT_ARMED,      /* the arm level is not COMMIT — DECIDE and OFF never rewrite anything */
    N48_CM_NO_BUFFERS,     /* the candidate IB and/or the read-back scratch is not allocated */
    N48_CM_NOT_TRANSLATE,  /* the verdict is not TRANSLATE: the ladder already refused this frame */
    N48_CM_MULTI_IB,       /* more than one IB — the rewrite covers IB 0 only (asymmetry 2 above) */
    N48_CM_LEN,            /* the IB is empty or longer than the scratch */
    N48_CM_NO_SEGMENTS,    /* no segment, or more than the table holds */
    N48_CM_SEG_REFUSED,    /* a segment's translate status is non-zero */
    N48_CM_SEG_LEN,        /* a segment's translation is not exactly as long as its input (the layout is IN PLACE) */
    N48_CM_COVER,          /* the segments do not tile [0, n) head to head */
    N48_CM_PAGE,           /* a page of the IB did not resolve, or is not host memory */
    N48_CM_WRITE_SHORT,    /* the in-place write did not place every dword */
    N48_CM_READ_SHORT,     /* the read-back did not return every dword */
    N48_CM_READ_VRAM,      /* the read-back crossed a page that was not host memory */
    N48_CM_MISMATCH,       /* a dword read back is not the dword we wrote — THE mutant */
    N48_CM_TOKEN,          /* the frame at the hook is not the frame whose IB we rewrote */
    /* 0.0.353 — X9. Appended, never inserted: the reason numbers are printed in `byReason[]` and quoted in the
     * record, so an existing one must never change index. */
    N48_CM_DEP_STALE,      /* a submission somewhere was dropped, so some surface holds bytes nobody wrote (gfx_dep.h) */
    /* 0.0.369 — the SEGMENT KIND. Appended, never inserted, for the same reason as the rung above. */
    N48_CM_SEG_KIND,       /* the frame does not declare a kind, or a segment does not have that kind's shape (below) */
    /* 0.0.401 (a item 3,b) — THE IDENTITY LUT IS NOT READY. Appended, never inserted, for the same reason
     * as the two rungs above: the reason numbers are printed in `byReason[]` and quoted in the record.
     * PRESENT ONLY WHILE `gfxneuter 32` IS ON. With the switch off `lut_switch` is 0 and this rung is unreachable, so
     * every path is byte for byte 0.0.400's. With it on, a PLANE-SHAPED candidate — an ENCODER frame that samples the
     * learned LUT VA in its slot-4 input list (`lut_plane`, set by the caller from the translator's own `in_va[]`) — is
     * refused until the deferred identity write has run and read back clean (`lut_ready`). The FILL is unaffected: it
     * does not sample the LUT, so the caller passes `lut_plane == 0` for it, and the one-shot's first commit still
     * lands. This rung REFUSES ONLY; it never writes and never widens another rung. See n48_cm_gate's own comment. */
    N48_CM_LUT_NOT_READY,  /* switch 32 on, a plane-shaped candidate, and the identity LUT has not been written yet */
    /* 0.0.404 (a item 2,b) — THE FIRST-SHOT FILL-SET RESERVATION. Appended, never inserted, for the same
     * reason as the three rungs above: the reason numbers are printed in `byReason[]` and quoted in the record.
     * PRESENT ONLY WHILE `gfxneuter 33` IS ON. With the switch off `fs_switch` is 0 and this rung is unreachable, so
     * every path is byte for byte 0.0.403's. With it on, and only while the arm's reservation window is open
     * (gfx_fillset.h's n48_fs_win_open — both fill-set members not yet committed and not expired), a TRANSLATE-eligible
     * frame that is NOT a reservable fill is refused here. A reservable fill is a ColorFill (in-force PS identity
     * `ws_B_ColorFill`) whose CB0 is a set member not yet committed; it passes and may spend a shot. This rung REFUSES
     * ONLY; it never writes and never widens another rung. See n48_cm_gate's own comment. */
    N48_CM_RESERVED_FOR_FILL, /* switch 33 on, the fill-set window is open, and this frame is not a reservable fill */
    /* 0.0.418 (an internal design note, E3) — REGION-MOVED. Appended, never inserted, for the same reason as the
     * rungs above. Set ONLY by the kext's commit-try (not by n48_cm_gate): the fence828 candidate was placed against a
     * ring region that has since been rebuilt or moved, so the frame is NEUTERED at source. Through 0.0.417 that path
     * dropped the candidate but left the verdict at COMMIT, and the executed, un-promoted fence then pinned every later
     * candidate at SLOT-PRE. The frame-level answer is a refusal, exactly as every other gate refusal neuters. */
    N48_CM_FENCE_REGION_MOVED,
    /* 0.0.420 (an internal design note P1) — RESERVED-FOR-PLANE. Appended, never inserted, for the same
     * reason as the rungs above. Set ONLY by n48_cm_gate's appended second-window rung. PRESENT ONLY WHILE `gfxneuter 35`
     * IS ON: with `fp_switch` 0 the rung is unreachable and the gate is 0.0.419's, dword for dword. With the switch on,
     * and only while the arm's SECOND window is open (gfx_fillset.h's n48_fs_plane_win_open — the fill window has closed,
     * no plane frame has committed and the second window has not expired), a TRANSLATE-eligible frame that is NOT
     * plane-shaped (in-force PS identity `ws_D_GPUPass`) is refused here. A plane-shaped frame passes and may spend a shot.
     * This rung REFUSES ONLY; it never writes and never widens another rung. */
    N48_CM_RESERVED_FOR_PLANE,
    /* R1 (an internal design note Q2) — MEMORY-DESTINATION. APPENDED AT THE TAIL, never inserted,
     * for the SAME reason as every rung above (the reason numbers are printed in `byReason[]` and quoted in the
     * record). The design names its GATE-FUNCTION check order as "after TOKEN, before DEP_STALE" - see
     * n48_cm_gate's own placement of the `if` below, which is where that order lives; the ENUM's numeric value
     * stays at the tail, matching this file's own append-only discipline for every prior addition. */
    N48_CM_MEMDST,         /* switch 42 ENFORCE, and n48_md_judge did not answer N48_MD_OK for this candidate */
    /* C5 part 1 (an internal design note Q1, gfx_flightring.h) — THE FLIGHT RING IS FULL. APPENDED AT THE TAIL,
     * never inserted, for the same reason as every rung above (the reason numbers are printed in `byReason[]` and
     * quoted in the record). Checked ONLY here — the primary gate is the caller's `live` predicate
     * (gfxsrc_commit_try), which folds in `!n48_fr_full(&gKsRing)` exactly as it already does for md_ok/dep_ok; this
     * is the same redundant second check every other "the world" rung above already has. `ring_full` is set from
     * n48_fr_full(), asked at the SAME instant the caller decides `live`, never recomputed here. */
    N48_CM_RING_FULL,      /* the flight ring (16 entries) has no FREE slot for this commit's own flight */
    /* build 0.0.456 item 1 ('s fallback: "a small rung refusing COMMIT when !gXdBuild.mib"). MIB-REQUIRED.
     * APPENDED AT THE TAIL, never inserted, for the same reason as every rung above (the reason numbers are printed in
     * `byReason[]` and quoted in the record). PRESENT ONLY WHILE `gfxneuter 53` IS ON: with `mib_req_switch` 0 this
     * rung is unreachable and the gate is 0.0.455's, dword for dword. With the switch on, a frame that is not a
     * genuine two-head MIB frame (`mib` 0, OR `mib` 1 with `nib` < 2) is refused HERE, beside MULTI_IB above — same
     * dependency (both are answered from `mib`/`nib` alone, before anything about the rewrite is known) — so the one
     * armed shot this switch guards can only ever land on a frame with two or more IBs. */
    N48_CM_MIB_REQUIRED,   /* switch 53 on, and this frame is not mib (or has fewer than 2 IBs) */
    /* C5 part 2 (an internal design note Q2, build 0.0.460) — FENCE REQUIRED IN CONTINUOUS MODE. Appended at the
     * tail, never inserted, for the same reason as every rung above (the reason numbers are printed in `byReason[]`
     * and quoted in the record). Present ONLY while THIS shot is a CONTINUOUS one (`cont_on`); with `cont_on` 0 (every
     * one-shot arm, and the default) the rung is unreachable and the gate is 0.0.451's, dword for dword — a fence-less
     * commit under a one-shot is unaffected (an internal review note "10a: acceptable once the push is fixed... Require a
     * fence in continuous mode" is what this rung is). Checked as a WORLD rung, beside RING_FULL: `cont_fence_ok` is
     * the caller's OWN pre-write reading of whether THIS frame carries an owned-slot fence candidate
     * (`gXdF828Pending`, sampled BEFORE the write, never recomputed here). The caller's `live` predicate folds in the
     * SAME reading first (gfxsrc_commit_try), exactly as RING_FULL's own redundant-second-check shape above. */
    N48_CM_CONT_NO_FENCE,  /* continuous mode, and this commit carries no owned-slot fence candidate */
    /* build 0.0.487 (an internal design note Q4 "A latent fail-open", Q6 item 5, contract C3) — COMPUTE-ELIDE-R1.
     * APPENDED AT THE TAIL, never inserted, for the same reason as every rung above (the reason numbers are printed in
     * `byReason[]` and quoted in the record). A frame in which switch 57 elided ANY of Apple's compute clears N
     * (`cs_elided` > 0, the policy's own count) commits ONLY with switch 42 on ENFORCE (`md_switch`) AND R1 clean
     * (`md_ok`): eliding N makes reachable a frame that, not enumerated by D4', would otherwise fall to `source_neuters`,
     * which asks nothing about memory, and Apple's triplet around N is judged ONLY by R1. With 57 OFF
     * `cs_elided` is 0 and the rung is unreachable - the gate is 0.0.485's, dword for dword. The caller's `live`
     * predicate (gfxsrc_commit_try, n48_cm_live) carries the SAME clause first, so the write never happens; this is the
     * redundant second check every "world" rung has. */
    N48_CM_CS_ELIDE_R1,    /* switch 57 elided a dispatch, and 42 is not ENFORCE or R1 did not answer clean */
    /* build 0.0.500 (an internal design note Q4) — DRAW-ELIDE-R1. APPENDED AT THE TAIL, never inserted, for the same
     * reason as every rung above. A frame in which switch 66 elided ANY draw (`draw_elided` > 0, the policy's own sum of
     * xlat12's `draw_elided`) commits ONLY with switch 42 on ENFORCE (`md_switch`) AND R1 clean (`md_ok`), exactly
     * COMPUTE-ELIDE-R1's rule: an elided draw makes reachable a frame whose memory-destination packets only R1 judges.
     * With 66 OFF `draw_elided` is 0 and the rung is unreachable - the gate is 0.0.499's, dword for dword. The caller's
     * `live` predicate (gfxsrc_commit_try, n48_cm_live) carries the SAME clause first; this is the redundant second check. */
    N48_CM_DRAW_ELIDE_R1,  /* switch 66 elided a draw, and 42 is not ENFORCE or R1 did not answer clean */
    N48_CM_REASONS
};

static inline const char *n48_cm_reason_name(uint32_t r)
{
    static const char *const n[N48_CM_REASONS] = {
        "COMMIT", "not-armed", "no-buffers", "not-translate", "multi-ib", "ib-length", "no-segments", "segment-refused",
        "segment-length", "coverage", "page-not-host", "write-short", "read-short", "read-not-host", "READ-BACK-MISMATCH",
        "frame-identity", "DEPENDENCY-STALE", "SEGMENT-KIND", "LUT-NOT-READY", "RESERVED-FOR-FILL",
        "FENCE-REGION-MOVED", "RESERVED-FOR-PLANE", "MEMORY-DESTINATION", "RING-FULL", "MIB-REQUIRED", "CONTINUOUS-NO-FENCE",
        "COMPUTE-ELIDE-R1", "DRAW-ELIDE-R1" };
    return r < N48_CM_REASONS ? n[r] : "?";
}

/* 0.0.369 — WHICH RECOGNISER PRODUCED THIS FRAME'S SEGMENTS, AND WHY THE GATE HAS TO BE TOLD.
 * Until 0.0.368 the coverage rung said `start == head + 2` unconditionally: Apple's encoder head (EVENT_WRITE 0x16,
 * ACQUIRE_MEM, EVENT_WRITE 0xE) is two dwords the draw policy does not translate, so a segment's body starts two dwords after
 * its head and anything else meant the rewrite would move the draw. The compositor's SETUP HALVES (1456
 * dwords) carry no such head anywhere: xlat12_headless.h's recogniser splits them into render passes whose `start == head` —
 * the pass's own CONTEXT_CONTROL, which the draw policy DOES translate. Both shapes are legitimate and they are NOT
 * interchangeable, so the frame must say which one it is and every segment must have that kind's shape.
 *
 * THE HEADLESS SHAPE IS THE TIGHTER OF THE TWO, not the looser one, and that is the whole reason it may be admitted:
 *   - no dword of Apple's survives in a headless segment. The recogniser emits `head == start`, so the translator's output
 *     covers the pass end to end and rewrites the CONTEXT_CONTROL into its proven form. In an encoder segment Apple's two
 *     head dwords are passed through untouched.
 *   - every value check still runs on it: the register allowlist, the operand rules, the per-stage program resolver, the
 *     interpolation guard and the descriptor rung all act inside xlat12_ib_translate_draw_ex, which does not know or care
 *     which recogniser found the segment.
 * WHAT THE RECOGNISER DOES NOT PROVE, stated here because the rung depends on it: it judges SHAPE, NOT PROVENANCE (it accepts
 * 12,521 of 20,000 shape-valid corruptions of a real setup half). A foreign stream in that shape is accepted BY IT. What makes
 * that acceptable is everything in this gate downstream: the bytes that reach the CP are read back and compared dword for
 * dword against the translator's OWN output, and the frame's identity is re-established at the hook. Neither may be weakened
 * on the strength of the recogniser, and the recogniser may not be relaxed on the strength of them.
 *
 * MIXED REFUSES. There is ONE kind per frame, not one per segment, so a frame carrying both shapes cannot be described and
 * refuses at N48_CM_SEG_KIND — whichever kind it claims, the other half's segments do not have that kind's shape. 0 is
 * `unset` and refuses first, like every other field in n48_cm_frame. */
enum {
    N48_CM_KIND_UNSET = 0,   /* nobody said — REFUSES */
    N48_CM_KIND_ENCODER,     /* xlat12_ib_segments found these: every start == head + 2 */
    N48_CM_KIND_HEADLESS     /* xlat12_ib_headless_passes found these: every start == head, plus the three rungs below */
};

/* 0.0.370 — THE KIND'S NAME, so a run can READ which recogniser carried a frame instead of inferring it.
 * hp9 had `asked 3, ACCEPTED 3` from the recogniser's own tally and three frames at TRANSLATE, and could only call the
 * tie SUSPECTED because no per-frame line named a kind. UNSET is printed as UNSET and is NEVER printed as ENCODER: the whole
 * point of `0 refuses` is that "nobody chose" stays distinguishable from "encoder", and a log that blurs the two would put the
 * fail-open reading back into the record even though the gate itself refuses. */
static inline const char *n48_cm_kind_name(uint32_t k)
{
    return k == N48_CM_KIND_ENCODER ? "ENCODER" : (k == N48_CM_KIND_HEADLESS ? "HEADLESS" : "UNSET");
}

/* 0.0.370 — THE COMMIT-REHEARSAL LINE'S FORMAT, HERE SO THE TEST CAN BOUND IT. `n48log` cuts a line at 512
 * bytes (amd/n48log.cpp: `char line[512]`; HWLOG prepends "AppleHardwareHook: " and appends "\n", 20 bytes), and lost an
 * X9 verdict to exactly that. Adding `seg_kind` put this line's worst case AT the cap, so it was shortened until it fits with
 * room: "pages %u host %u" and "mismatched vs what we gathered" say what the longer wording said. The X9 verdict and `ARM IS`
 * are at the END of the line, which is what truncation eats first — so this is bounded by a check, not by arithmetic in a
 * commit message.
 * args: va, n, nseg, seg_kind name, pages, sys_pages, got, back_sys_pages, mismatch, firstBad, probe reason, probe detail,
 *       dep reason, dep detail, dep_ok, arm */
#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u    /* the same number f3_reader.h defines, for the same reason; whichever is included first wins */
#endif
/* 0.0.391 ( condition 6,) — THE SPEND LINE'S FORMAT, HERE FOR THE SAME REASON THE REHEARSAL'S IS.
 * Through 0.0.390 it ended "NO OTHER FRAME can be handed COMMIT from this moment", which is true at budget 1 and A LIE
 * above it - n48_cm_shot_spend leaves the state ARMED while `spent < budget`, so the next frame that clears every rung
 * commits too. It now READS the budget and says which of the two worlds the boot is in. That made the line longer, and
 * `longer` is how and both happened, so it is BOUNDED BY A CHECK: the first draft of this wording measured
 * 508 bytes at worst-case arguments, 17 over the cap, and was shortened until gfx_commit_test.cpp accepted it.
 * 0.0.426 (MIB-COMMIT B9): `%u IB(s)` is added so the line names a multi-IB frame's count; `%u dw` is already the
 * frame's TOTAL dwords (`c.n` is sum(ib_n) for a MIB frame), so nib + total is what B9 asks the line to carry.
 * args: gate reason name, token seq, VA, dwords, nib, segments, seg_kind name, budget, spent, left, the budget sentence */
/* C5 part 1 (hygiene, an internal design note Q3) — THE OLD ENDING WAS A LIAR AT BUDGET > 1. Through 0.0.442 it
 * read "The arm level stays COMMIT only until this frame's submission returns", which is true only while `left == 0`
 * after this spend — at budget > 1 with budget left the arm level is put back to COMMIT for the NEXT frame the
 * instant this one's submission returns (xd_shot_finish only moves SPENT -> DONE, and n48_cm_shot_level only hands
 * out COMMIT again once THIS frame's exemption has read the level, which is the same "inside Apple's call" window
 * for every spend, not just the last). The true, budget-general statement is that the ring walk's exemption for
 * THIS frame reads the arm level inside Apple's own call, whatever happens to it afterwards. */
#define N48_CM_SPENT_FMT "gfx-commit: ONE-SHOT SPENT - the COMMIT gate answered %s for token seq %u (VA %#llx, %u dw, " \
                         "%u IB(s), %u segment(s), seg_kind %s). BUDGET: this arm %u, spent %u, %u left. THIS FRAME's " \
                         "own ring-walk exemption reads the arm level inside Apple's call, whatever the level becomes " \
                         "afterwards. %s"
#define N48_CM_SPENT_MORE "A FURTHER FRAME MAY STILL BE HANDED COMMIT: the budget is NOT exhausted."
#define N48_CM_SPENT_DONE "The budget is exhausted, so no other frame can be handed COMMIT from this moment."

/* C5 part 2 (an internal design note Q3, build 0.0.460) — THE THREE CONTINUOUS-MODE LOG LINES, HERE FOR THE
 * SAME REASON N48_CM_SPENT_FMT IS: so the host test can bound them against n48log's 512-byte cap ('s
 * lesson) before a run ever prints one.
 *   N48_CM_CONT_START_FMT — the ONE line printed at the first plane commit (n48_cm_shot_cont_start's own 1).
 *   N48_CM_CONT_STOP_FMT  — the stop line, naming stop_why, spent/N, and last-commit-minus-start; printed once,
 *                           from xd_shot_finish, for every continuous arm's own disarm.
 *   N48_CM_CONT_SUMMARY_FMT / N48_CM_CONT_SUMMARY2_FMT — Q3's periodic line (every 32 commits or 1 s), SPLIT IN
 *                           TWO for the same reason `fence828: VERDICT` is: the full field list
 *                           measured over the cap on one line. Line 1: commits this arm; keystone written/no-op/
 *                           refused this arm; walk-bits-OK this arm; fence placed/OURS(retired)/late(expired) this
 *                           arm. Line 2: EOP latency max/mean (us) this arm; exemption spared/refused (BOOT TOTALS
 *                           - gGn's own counters, which every `gfxneuter` read already prints); flight ring
 *                           OUT-OF-ORDER (boot total) and its own live-occupancy high water mark this arm;
 *                           keystone-defer deferrals (BOOT TOTAL - gKsD.deferrals); ledger un-feeds (BOOT TOTAL -
 *                           gXdLed.unfeedRemoved); lines this arm's own caps suppressed. Both lines share the
 *                           `CONTINUOUS SUMMARY` prefix so a grep for either finds both (the fence828 convention). */
#define N48_CM_CONT_START_FMT \
    "gfx-commit: CONTINUOUS START - the first plane commit for this continuous arm was token seq %u at %llu us; T " \
    "(%llu us) starts now; N %u, %u already spent before it."
#define N48_CM_CONT_STOP_FMT \
    "gfx-commit: CONTINUOUS STOP - %s (stop_why %u). spent %u of N %u; %llu us since the first plane commit (T " \
    "%llu us, 0 if T never started); last commit was %llu us before this stop."
#define N48_CM_CONT_SUMMARY_FMT \
    "gfx-commit: CONTINUOUS SUMMARY - %u commit(s) this arm; keystone written %llu no-op %llu refused %llu, walk-" \
    "bits OK %llu; fence placed %llu OURS %llu late %llu."
#define N48_CM_CONT_SUMMARY2_FMT \
    "gfx-commit: CONTINUOUS SUMMARY - EOP latency max %llu mean %llu us; exemption spared %llu refused %llu " \
    "(boot); out-of-order %llu (boot); ring occ max %u; deferrals %llu (boot); ledger un-feeds %llu (boot); %llu " \
    "line(s) suppressed, %llu of them refusal lines."

#define N48_CM_REHEARSAL_FMT "gfx-commit: REHEARSAL (no write) frame VA %#llx len %u, %u segment(s) seg_kind %s - pages %u " \
                             "host %u, read back %u dw crossing %u host page(s), mismatched vs what we gathered %u (first at " \
                             "%u); the rewrite probe answers %s (detail %#x). X9: the world is %s (detail %#llx), so dep_ok " \
                             "%u. ARM IS %u (2 = COMMIT); nothing was written."

/* build 0.0.457 item 3 — THE HONEST REHEARSAL'S ONE LINE (n48_cm_rehearse). Bounded under N48_LOG_CAP_BODY by
 * tests/gfx_commit_test.cpp with every field at its widest. Fields: rehearsal seq, judged frame, nib, nseg, seg_kind,
 * the REAL verdict, "WOULD-COMMIT" or "REFUSED at ", the rung (verdict name / gate reason / live clause), detail, the
 * live predicate's inputs (built, dep_ok and the X9/D4 reason, md enforce/ok, ring_full), the gate's own answer, switch
 * 53, the configured budget, switch 54 for this frame and its N48_TV_WHY_* bits, and whether a fill/plane window is on. */
#define N48_CM_WOULD_FMT "gfx-commit: WOULD #%llu frame %llu (NO WRITE, arm assumed COMMIT) %u IB %u seg %s, verdict " \
                         "%s -> %s%s (%#x). live: built %u dep_ok %u (%s) md %u/%u ring_full %u; gate %s; 53 %u; " \
                         "budget %u; 54 %u tv %#x. NOT modelled: keystone, WS rebind, fill/plane windows %u."

/* build 0.0.473 item 4 — THE would457 REPORT LINE'S FORMAT, HERE SO THE TEST CAN BOUND IT (it was 489 bytes at
 * 20-digit counters, 2 under the cap, with no check). Shortened, every number kept, and the CONTINUOUS-NO-FENCE count
 * (N48_CM_RH_CONT_NO_FENCE, appended at 0.0.472 and counted in gXdRh.by[] but never printed) added last, so the
 * per-answer counts now sum to the rehearsal count. args: rehearsals, then by[] for WOULD, VERDICT, NOT_BUILT,
 * NO_BUFFERS, DEP, MEMDST, RING, GATE, WINDOW, CONT_NO_FENCE, (build 0.0.487) CS_ELIDE and (build 0.0.495,
 * "arm forced to COMMIT" shortened to "armed" to stay under the cap) HEAPGEN, then (build 0.0.500) DRAW_ELIDE ("DE-R1")
 * last - with "(armed, real verdict)" cut to "(armed)" and "fill/plane-window" to "window", every number kept, to stay under. */
#define N48_CM_WOULD457_FMT "would457: rehearsals %llu (armed): WOULD-COMMIT %llu; " \
                            "refused: verdict %llu not-built %llu no-buffers %llu DEP-STALE %llu MEMDST %llu " \
                            "RING-FULL %llu gate %llu window %llu CONT-NO-FENCE %llu CS-ELIDE-R1 %llu " \
                            "HEAP-GEN %llu DE-R1 %llu. " \
                            "Not modelled: keystone, WS rebind."

/* build 0.0.473 item 4 — THE tvscan457 REPORT LINE'S FORMAT (switch 54), HERE FOR THE SAME REASON. With every
 * counter at 20 digits the 0.0.472 wording rendered 588 bytes, 97 over the cap, so its tail would have been cut
 * silently. Shortened, EVERY NUMBER KEPT, in the same order. args: on/off name, how, frames judged ON, refused,
 * over-cap, walk-short, unseen, table-over, unresolved, VRAM (the refusal count), N1 on/off name, item-loop, VRAM
 * targets seen. */
#define N48_TVSCAN457_FMT "tvscan457: switch 54 (colour-target scan, every item/draw) %s (%s). Judged ON %llu, refused " \
                          "%llu: over-cap %llu walk-short %llu unseen %llu table-over %llu unresolved %llu VRAM %llu " \
                          "(N1 %s) item-loop %llu; VRAM seen %llu (refused if N1 OFF). OFF: the 64-item cap."

/* One of Apple's encoder segments as the rewrite handled it. `head`/`start`/`end` are xlat12_ib_segment's, `status` is
 * xlat12_ib_translate_draw_ex's, `out_len` is the dwords it produced. */
typedef struct { uint32_t head, start, end, status, out_len; } n48_cm_seg;

/* 0.0.426 (an internal design note binding B7) — THE FRAME IDENTITY OVER EVERY IB.
 *
 * Through 0.0.425 the token carried IB 0's VA and length alone, so a multi-IB frame's identity was re-established over one
 * entry of four: the hook compared THIS submission's IB 0 with the rewritten frame's IB 0 and nothing checked IBs 1..3. With
 * `mib` 0 that is byte for byte 0.0.425's token (the arrays are not read and not compared). With `mib` 1 the token carries
 * EVERY IB's VA and declared length and the match compares every entry below `nib`, so "we rewrote SOME frame whose IB 0
 * matches" can never be read as "we rewrote THIS frame" while IBs 1..3 differ. `va`/`n` stay IB 0's, so a reader of the old
 * fields is unchanged. */
typedef struct {
    const void *info;
    uint64_t va;                 /* IB 0's VA */
    uint32_t stamp, n, nib, seq; /* `n` is IB 0's declared length */
    uint32_t mib;                /* 1: the arrays below are meaningful and must be compared */
    uint64_t ib_va[N48_XV_MAX_IBS];   /* B7: every IB's VA, [0] == va */
    uint32_t ib_n[N48_XV_MAX_IBS];    /* B7: every IB's declared dwords, [0] == n */
} n48_cm_token;

/* build 0.0.517 (the judge never stops) — THE NEXT TOKEN SEQ, NEVER 0. gXdCmSeq is 32-bit and every seq reader takes 0
 * as "none" (n48_cm_token_match, n48_fr_push/n48_fr_find, the cycle515 withdrawal queue, present73's pend_seq), so the plain
 * `++gXdCmSeq` would hand the 2^32-th rewrite seq 0 (its token never matches: a refused commit, fail-closed but a lost frame).
 * The wrap goes 0xFFFFFFFF -> 1, skipping 0. Identical to `cur + 1` for every cur below 0xFFFFFFFF. Every seq reader matches
 * seqs by equality only (never `<`), and every record holding one is a fixed table whose entries live for a few frames, so a
 * seq reused 2^32 - 1 rewrites later cannot meet its namesake. */
static inline uint32_t n48_cm_seq_next(uint32_t cur)
{
    const uint32_t n = cur + 1u;
    return n ? n : 1u;
}

static inline uint32_t n48_cm_token_match(const n48_cm_token *a, const n48_cm_token *b)
{
    if (!a || !b) return 0u;
    if (!a->seq || a->seq != b->seq) return 0u;
    if (a->info != b->info || a->va != b->va || a->stamp != b->stamp || a->n != b->n || a->nib != b->nib) return 0u;
    if (a->mib != b->mib) return 0u;
    if (!a->mib) return 1u;                     /* OFF: exactly 0.0.425's five-field identity */
    for (uint32_t k = 0; k < a->nib && k < N48_XV_MAX_IBS; k++)
        if (a->ib_va[k] != b->ib_va[k] || a->ib_n[k] != b->ib_n[k]) return 0u;
    return 1u;
}

/* Everything the caller learned while building, writing and reading back one frame's rewrite. Zero-initialised by the caller
 * on every frame; every field below must be POSITIVELY set to the value that means "verified", or the gate refuses. */
typedef struct {
    uint32_t arm;            /* N48_SD_ARM_* as the caller read it at the top of the frame */
    uint32_t verdict;        /* n48_xv_decide's answer for this frame */
    uint32_t buffers_ok;     /* 1 when the candidate IB and the read-back scratch both exist */
    uint32_t nib;            /* IBs in the submission */
    uint32_t n;              /* dwords of IB 0 */
    uint32_t cap;            /* dwords of scratch the caller holds */
    uint32_t nseg;
    n48_cm_seg seg[N48_XV_MAX_SEGS];
    uint32_t pages;          /* 4 KiB pages [va, va + 4n) crosses */
    uint32_t sys_pages;      /* of those, how many resolved through the submitter's page table AS HOST MEMORY */
    uint32_t wrote;          /* dwords the in-place write placed */
    uint32_t got;            /* dwords the read-back returned */
    uint32_t back_sys_pages; /* pages the read-back crossed that were host memory */
    uint32_t mismatch;       /* dwords that did not read back equal — COUNTED, never a bool (rule: keep the number) */
    uint32_t token_ok;       /* the hook's frame identity matched the rewrite's */
    /* 0.0.353 — X9. 1 only when n48_dep_ok() answered over a world the caller POSITIVELY sampled from the live
     * neuter counters. Zero-initialised means "nobody checked", and that refuses, like every other field here. */
    uint32_t dep_ok;
    /* 0.0.369 — the segment kind and, for HEADLESS only, the three things the caller must have OBSERVED before
     * it may claim that kind. All four are zero-initialised, and every one of them refuses at 0:
     *   seg_kind        N48_CM_KIND_* — 0 (unset) refuses, so a caller that never chose cannot be read as ENCODER.
     *   hl_ib_segments  what xlat12_ib_segments returned ON THIS SAME BUFFER. It MUST be 0: the headless recogniser is a
     *                   fallback, never an alternative, and a stream both of them accept is a stream neither understands.
     *   hl_ok           1 only when xlat12_ib_headless_passes' report came back XLAT12_HL_OK. A non-zero return with a
     *                   refusal in the report is not an acceptance.
     *   hl_total        the recogniser's *total (passes FOUND). It must equal nseg: `passes found` > `segments filled` means
     *                   the table truncated the stream and the dwords past it would reach the CP untranslated. */
    uint32_t seg_kind;
    uint32_t hl_ib_segments;
    uint32_t hl_ok;
    uint32_t hl_total;
    /* 0.0.401 (a item 3,b) — THE IDENTITY LUT'S THREE FLAGS. All zero-initialised, and the rung they
     * feed is UNREACHABLE unless `lut_switch` is 1, so a caller that leaves them all 0 gets 0.0.400's gate exactly:
     *   lut_switch  switch 32 is ON right now. The caller reads the one volatile global; 0 makes the rung absent.
     *   lut_plane   1 for a PLANE-SHAPED candidate: an ENCODER frame whose own translator input list sampled the LUT VA
     *               (the caller's `gXdBuild.lutPlane`, set only while the switch is on, from `xlat12_draw_stats.in_va[]`).
     *               A frame the policy did not run for, or one that does not sample the LUT, passes 0 and is unaffected.
     *   lut_ready   1 only after the deferred thread wrote the identity ramp and read it back clean. It is set once and
     *               never cleared in a boot; nothing but the thread's verified write may set it. */
    uint32_t lut_switch;
    uint32_t lut_plane;
    uint32_t lut_ready;
    /* 0.0.404 (a item 2,b) — THE FIRST-SHOT FILL-SET RESERVATION'S THREE FLAGS. All zero-initialised, and
     * the rung they feed is UNREACHABLE unless `fs_switch` is 1, so a caller that leaves them all 0 gets 0.0.403's gate
     * exactly:
     *   fs_switch   switch 33 is ON right now. The caller reads the one volatile global; 0 makes the rung absent.
     *   fs_open     1 while the reservation window is still open (gfx_fillset.h's n48_fs_win_open): the switch is on, an
     *               arm opened it, it has not expired, and at least one member is still uncommitted. 0 closes the rung
     *               and the frame follows today's rule — the plane after both fills, or after expiry.
     *   fs_reserve  1 for a RESERVABLE fill: a TRANSLATE-eligible ColorFill whose CB0 is an uncommitted set member. The
     *               caller sets it from n48_fs_step's N48_FS_RESERVE and NOWHERE else. A frame that is not one — including
     *               an UNIDENTIFIED frame — passes 0 and is refused while the window is open (FAIL-CLOSED). */
    uint32_t fs_switch;
    uint32_t fs_open;
    uint32_t fs_reserve;
    /* 0.0.420 (STEP10-PLAN P1) — THE SECOND WINDOW'S THREE FLAGS. All zero-initialised, and the rung they feed is
     * UNREACHABLE unless `fp_switch` is 1, so a caller that leaves them all 0 gets 0.0.419's gate exactly:
     *   fp_switch   switch 35 is ON right now. The caller reads the one volatile global; 0 makes the rung absent.
     *   fp_open     1 while the second window is still open (gfx_fillset.h's n48_fs_plane_win_open): the switch is on, an
     *               arm opened it, the FILL window has closed, no plane frame has committed and it has not expired. 0
     *               closes the rung and the frame follows today's rule.
     *   fp_reserve  1 for a PLANE-SHAPED frame: the in-force fragment identity is `ws_D_GPUPass`. The caller sets it from
     *               n48_fs_plane_step's N48_FS_RESERVE and NOWHERE else. A frame that is not one — including an
     *               UNIDENTIFIED frame — passes 0 and is refused while the window is open (FAIL-CLOSED). */
    uint32_t fp_switch;
    uint32_t fp_open;
    uint32_t fp_reserve;
    /* 0.0.426 (an internal design note binding B5) — THE PER-IB FRAME. All zero-initialised and read ONLY when `mib` is
     * 1, so a caller that leaves them all 0 gets 0.0.425's gate exactly (every rung below branches on `mib`).
     *   mib         1 when this frame's IBs were read into ONE concatenated buffer, each translated on its own.
     *   ib_n[k]     IB k's DECLARED dwords. LEN requires n == sum(ib_n[0..nib)).
     *   ib_nseg[k]  how many segments the recogniser found in IB k (the per-IB half of the coverage rung).
     *   *_ib[k]     the per-IB rewrite evidence: pages/sys_pages/wrote/got/back_sys_pages/mismatch, over IB k's OWN
     *               range. Index 0 is the same evidence the scalar fields above carry (the caller writes both), so a
     *               single-IB reader is unchanged. `detail` for these rungs is `(k << 16) | value`. */
    uint32_t mib;
    uint32_t ib_n[N48_XV_MAX_IBS];
    uint32_t ib_nseg[N48_XV_MAX_IBS];
    uint32_t pages_ib[N48_XV_MAX_IBS];
    uint32_t sys_pages_ib[N48_XV_MAX_IBS];
    uint32_t wrote_ib[N48_XV_MAX_IBS];
    uint32_t got_ib[N48_XV_MAX_IBS];
    uint32_t back_sys_pages_ib[N48_XV_MAX_IBS];
    uint32_t mismatch_ib[N48_XV_MAX_IBS];
    /* R1 (an internal design note Q2) — THE MEMORY-DESTINATION RUNG'S OWN ANSWER, exactly the dep_ok pattern
     * above: `md_switch` is 1 only under ENFORCE (gMdMode 1; SHADOW and OFF leave it 0, so this rung is
     * unreachable and the gate is 0.0.439's, dword for dword); `md_ok` is gXdBuild.md_ok, the caller's OWN reading
     * from n48_md_judge - never recomputed here. The PRIMARY gate is the caller's `live &= (!md_switch || md_ok)`
     * (gfxsrc_commit_try); this rung is the SAME redundant second check n48_cm_gate already has for dep_ok, so a
     * caller that forgets to fold md_ok into `live` still cannot reach N48_CM_OK on an ENFORCE-refused frame. */
    uint32_t md_switch;
    uint32_t md_ok;
    /* C5 part 1 (gfx_flightring.h) — 1 when the flight ring had no FREE slot at the instant this frame reached the
     * gate. Always asked (no switch: the ring exists unconditionally in this build, exactly as DEP_STALE's rung
     * has none). The caller's `live` predicate folds in the SAME reading first (gfxsrc_commit_try), so a caller
     * that forgot to fold it in still cannot reach N48_CM_OK on a full ring — the same redundant-second-check shape
     * as md_ok/dep_ok above. */
    uint32_t ring_full;
    /* build 0.0.456 item 1 — MIB-REQUIRED's own switch reading. 1 only while `gfxneuter 53` is ON;
     * the rung it feeds reads `mib`/`nib` above, already carried in this same struct, so no new per-frame evidence
     * is needed beyond this one flag. Zero-initialised means "switch off", and 0 makes the rung unreachable, exactly
     * as every other switch field in this struct already behaves. */
    uint32_t mib_req_switch;
    /* C5 part 2 (an internal design note Q2) — THE CONTINUOUS-MODE FENCE REQUIREMENT'S OWN TWO FLAGS, exactly
     * the ring_full/md_ok pattern: `cont_on` is 1 only while THIS shot (gXdShot) is a continuous arm — the caller's
     * OWN reading of `gXdShot.cont`, never recomputed here; `cont_fence_ok` is 1 only when this frame's OWN fence828
     * candidate was pending before the write (`gXdF828Pending`, sampled by the caller). Both 0 by default, so a
     * one-shot (or any frame the caller forgot to set these for) gets the rung unreachable — 0.0.451, dword for
     * dword. */
    uint32_t cont_on;
    uint32_t cont_fence_ok;
    /* build 0.0.487 (COMPUTE-ELIDE-R1, above) — how many of Apple's compute clears N switch 57 elided in THIS
     * frame's candidate (gXdBuild.csElided, the policy's own sum of xlat12's `cs_elided`, cleared at every pass top).
     * The caller's OWN reading, never recomputed here; 0 (switch 57 OFF, and every frame without N) leaves the rung
     * unreachable. APPENDED. */
    uint32_t cs_elided;
    /* build 0.0.495 (switch 62, gfx_heapgen.h) — 1 when the caller's hg_commit_judge refused THIS frame: a shader-heap
     * copy overlapping a substituted program started, completed or is in progress since the frame's verdict, or one of its
     * programs is poisoned. The caller's OWN reading; 0 (switch 62 OFF, latched) leaves the clause a tautology. APPENDED. */
    uint32_t heap_refuse;
    /* build 0.0.500 (DRAW-ELIDE-R1, above) — how many draws switch 66 elided in THIS frame's candidate (gXdBuild.drawElided,
     * the policy's own sum of xlat12's `draw_elided`, cleared at both pass tops). The caller's OWN reading; 0 (switch 66 OFF)
     * leaves the rung unreachable. APPENDED. */
    uint32_t draw_elided;
    /* build 0.0.505 (an internal design note C3) — bit k = IB k's first segment is a LEAD (switch 69: translated from
     * its first dword, start == head). The caller's OWN copy of the segment stage's answer (gXdBuild.leadMask, set only
     * from n48_mib_segment's *lead_mask and only with a non-zero answer); 0 (switch 69 OFF) makes every lead rung
     * unreachable and the tiling loop 0.0.504's, dword for dword. APPENDED. */
    uint32_t lead_mask;
} n48_cm_frame;

/* build 0.0.505 (CROSS-IB C3): is the segment whose head is `head` IB k's lead? IB k's offset is sum(ib_n[0..k)),
 * which the gate's LEN rung has already tied to `n`; only called with `mib` 1 (a lead_mask without mib refuses first). */
static inline uint32_t n48_cm_seg_is_lead(const n48_cm_frame *c, uint32_t head)
{
    if (!c->lead_mask) return 0u;
    uint32_t off = 0u;
    for (uint32_t k = 0; k < c->nib && k < N48_XV_MAX_IBS; k++) {
        if (((c->lead_mask >> k) & 1u) && head == off) return 1u;
        off += c->ib_n[k];
    }
    return 0u;
}

/* The gate. Returns N48_CM_OK only when every rung passed; otherwise the FIRST rung that refused, with *detail carrying the
 * number that names it (a segment index, a count, a length). The order is the order of dependence: nothing about a rewrite can
 * be judged before it exists, nothing about a read-back before the write, and the frame's identity last, because it is the one
 * thing the CALLER of the hook must re-establish rather than the builder. */
static inline uint32_t n48_cm_gate(const n48_cm_frame *c, uint32_t *detail)
{
    if (detail) *detail = 0u;
    if (!c) return N48_CM_NOT_ARMED;
    if (c->arm != N48_SD_ARM_COMMIT) return N48_CM_NOT_ARMED;
    if (!c->buffers_ok) return N48_CM_NO_BUFFERS;
    if (c->verdict != N48_XV_TRANSLATE) { if (detail) *detail = c->verdict; return N48_CM_NOT_TRANSLATE; }
    /* 0.0.426 (MIB-COMMIT B5) — MULTI_IB now refuses nib 0, nib > N48_XV_MAX_IBS, and nib > 1 WITHOUT `mib`. With `mib` 0
     * this is 0.0.425's `nib != 1u` refusal, so every OFF path is unchanged. */
    if (c->nib != 1u) {
        if (!c->mib || c->nib == 0u || c->nib > N48_XV_MAX_IBS) { if (detail) *detail = c->nib; return N48_CM_MULTI_IB; }
    }
    /* build 0.0.456 item 1 — MIB-REQUIRED, checked immediately beside MULTI_IB above: the
     * same two fields (`mib`, `nib`) answer both, before anything about the rewrite is known. OFF (switch 53 never
     * thrown) `mib_req_switch` is 0 and this is one comparison against 0 and nothing else — 0.0.455's gate, dword
     * for dword. ON, a frame that is not `mib` 1 with `nib` >= 2 is refused here, so the shot this switch guards can
     * only ever land on a genuine two-head frame. */
    if (c->mib_req_switch && (!c->mib || c->nib < 2u)) { if (detail) *detail = c->nib; return N48_CM_MIB_REQUIRED; }
    if (c->n == 0u || c->cap == 0u || c->n > c->cap) { if (detail) *detail = c->n; return N48_CM_LEN; }
    /* 0.0.426 (B5) — LEN also requires n == sum(ib_n[0..nib)). The concatenated frame's length IS the sum of the IB lengths,
     * so a frame whose halves do not add up to the buffer the rewrite covers refuses rather than being judged on part of it. */
    if (c->mib) {
        uint32_t sum = 0u;
        for (uint32_t k = 0; k < c->nib; k++) {
            if (c->ib_n[k] == 0u) { if (detail) *detail = (k << 16); return N48_CM_LEN; }
            sum += c->ib_n[k];
        }
        if (sum != c->n) { if (detail) *detail = sum; return N48_CM_LEN; }
    }
    if (c->nseg == 0u || c->nseg > N48_XV_MAX_SEGS) { if (detail) *detail = c->nseg; return N48_CM_NO_SEGMENTS; }
    /* 0.0.369 — WHICH KIND, and for HEADLESS what the caller must have observed to claim it. This is here,
     * before the tiling loop, because the loop's own start rule is the kind's rule and a loop that does not know the kind
     * would have to guess. The detail values are tagged so a run can tell the four refusals apart:
     *     0x0000000K  the frame declared kind K and K is not one this gate knows (0 = nobody chose)
     *     0x0001xxxx  HEADLESS, but xlat12_ib_segments returned xxxx (non-zero) on the same buffer
     *     0x00020000  HEADLESS, but the recogniser's report was not XLAT12_HL_OK
     *     0x0003xxxx  HEADLESS, but the recogniser found xxxx passes and the table holds nseg
     *     0x8kkkxxxx  segment kkk's start is xxxx, which is not this kind's start (the loop below) */
    if (c->seg_kind != N48_CM_KIND_ENCODER && c->seg_kind != N48_CM_KIND_HEADLESS) {
        if (detail) *detail = c->seg_kind;
        return N48_CM_SEG_KIND;
    }
    if (c->seg_kind == N48_CM_KIND_HEADLESS) {
        if (c->hl_ib_segments) { if (detail) *detail = 0x00010000u | (c->hl_ib_segments & 0xFFFFu); return N48_CM_SEG_KIND; }
        if (!c->hl_ok)         { if (detail) *detail = 0x00020000u;                                 return N48_CM_SEG_KIND; }
        if (c->hl_total != c->nseg) { if (detail) *detail = 0x00030000u | (c->hl_total & 0xFFFFu);  return N48_CM_SEG_KIND; }
    }
    /* 0.0.426 (MIB-COMMIT B2/B5) — a multi-IB frame is ENCODER only: the headless recogniser is not run per IB, so a caller
     * claiming HEADLESS over nib > 1 is claiming a kind no recogniser produced for it. Tagged apart from the kind rungs
     * above (0x0004xxxx) so a run can tell this refusal from "nobody chose". */
    if (c->mib && c->nib > 1u && c->seg_kind != N48_CM_KIND_ENCODER) {
        if (detail) *detail = 0x00040001u;
        return N48_CM_SEG_KIND;
    }
    /* build 0.0.505 (CROSS-IB C3) — THE LEAD MASK, before the tiling loop reads it (0x0005xxxx; no new reason code):
     *     0x00050001  bit 0 set: a lead on IB 0 would inherit the previous submission's state (CROSS-IB Q3 (c))
     *     0x00050003  a lead without `mib`: no per-IB layout to place it in (asked before the nib rung, which a single-IB
     *                 frame's any bit would otherwise answer first)
     *     0x00050002  a bit at or past nib: no such IB
     *     0x00050004  a lead in a frame whose kind is not ENCODER (the 0x0004 rung above already refuses a non-ENCODER
     *                 multi-IB frame; kept as its own rung so the lead's kind is never merely implied)
     * lead_mask 0 (switch 69 OFF) skips all four. */
    if (c->lead_mask) {
        if (c->lead_mask & 1u) { if (detail) *detail = 0x00050001u; return N48_CM_SEG_KIND; }
        if (!c->mib) { if (detail) *detail = 0x00050003u; return N48_CM_SEG_KIND; }
        if (c->nib >= 32u || (c->lead_mask >> c->nib)) { if (detail) *detail = 0x00050002u; return N48_CM_SEG_KIND; }
        if (c->seg_kind != N48_CM_KIND_ENCODER) { if (detail) *detail = 0x00050004u; return N48_CM_SEG_KIND; }
    }
    /* The segments must TILE [0, n): the first head at dword 0, each segment's end the next segment's head, the last end
     * exactly n, and each translation exactly as long as its input — the draw policy's layout is IN PLACE and *out_len == n
     * on success, so anything else means the rewrite would move the draw or truncate the stream. A dword of the IB that no
     * segment covers is a dword that would reach the CP untranslated. Where the BODY starts inside a segment is the KIND's
     * rule, checked on its own rung so that a mixed frame is named as a mixed frame rather than as a hole in the tiling:
     * ENCODER two dwords after the head (xlat12_ib_segments' EVENT_WRITE head, which is passed through untranslated),
     * HEADLESS at the head itself (the pass's CONTEXT_CONTROL, which IS translated). */
    uint32_t at = 0u;
    for (uint32_t k = 0; k < c->nseg; k++) {
        const n48_cm_seg *s = &c->seg[k];
        if (s->status) { if (detail) *detail = (k << 16) | (s->status & 0xFFFFu); return N48_CM_SEG_REFUSED; }
        if (s->head != at || s->end <= s->start || s->end > c->n) {
            if (detail) *detail = (k << 16) | (s->head & 0xFFFFu);
            return N48_CM_COVER;
        }
        /* build 0.0.505: a proven lead (the rungs above) starts AT its head, like HEADLESS; every other ENCODER
         * segment at head + 2, exactly as before. */
        if (s->start != ((c->seg_kind == N48_CM_KIND_HEADLESS || n48_cm_seg_is_lead(c, s->head)) ? s->head : s->head + 2u)) {
            if (detail) *detail = 0x80000000u | ((k & 0xFFFu) << 16) | (s->start & 0xFFFFu);
            return N48_CM_SEG_KIND;
        }
        if (s->out_len != s->end - s->start) { if (detail) *detail = (k << 16) | (s->out_len & 0xFFFFu); return N48_CM_SEG_LEN; }
        at = s->end;
    }
    if (at != c->n) { if (detail) *detail = at; return N48_CM_COVER; }
    /* 0.0.426 (MIB-COMMIT B5) — COVER PER IB. The loop above proved the segments tile [0, n) in order; this proves each IB
     * boundary off_k = sum(ib_n[0..k)) falls ON a segment boundary, so NO segment spans two IBs. With each IB segmented on
     * its own that is true by construction; the gate re-checks it here so the property is PROVEN and not merely asserted by
     * the caller (B2, H2). `detail` = (k << 16) | the offending segment end. */
    if (c->mib) {
        uint32_t off = 0u, segi = 0u;
        for (uint32_t k = 0; k < c->nib; k++) {
            off += c->ib_n[k];
            while (segi < c->nseg && c->seg[segi].end < off) segi++;
            if (segi >= c->nseg || c->seg[segi].end != off) {
                if (detail) *detail = (k << 16) | ((segi < c->nseg ? c->seg[segi].end : 0u) & 0xFFFFu);
                return N48_CM_COVER;
            }
            segi++;
        }
    }
    /* Every page of the IB resolved AND was host memory — both directions, and `pages` itself must be non-zero so that an
     * un-walked range cannot pass by having 0 == 0. This is the rung that does NOT inherit target_vram's fail-open shape.
     * 0.0.426 (B5/B6): with `mib` this is asked of EVERY IB's OWN range; the scalar rungs are then exactly 0.0.425's. */
    if (c->mib) {
        for (uint32_t k = 0; k < c->nib; k++) {
            const uint32_t tag = k << 16;
            if (c->pages_ib[k] == 0u || c->sys_pages_ib[k] != c->pages_ib[k]) {
                if (detail) *detail = tag | (c->sys_pages_ib[k] & 0xFFFFu); return N48_CM_PAGE;
            }
            if (c->wrote_ib[k] != c->ib_n[k]) { if (detail) *detail = tag | (c->wrote_ib[k] & 0xFFFFu); return N48_CM_WRITE_SHORT; }
            if (c->got_ib[k] != c->ib_n[k]) { if (detail) *detail = tag | (c->got_ib[k] & 0xFFFFu); return N48_CM_READ_SHORT; }
            if (c->back_sys_pages_ib[k] != c->pages_ib[k]) {
                if (detail) *detail = tag | (c->back_sys_pages_ib[k] & 0xFFFFu); return N48_CM_READ_VRAM;
            }
            if (c->mismatch_ib[k]) { if (detail) *detail = tag | (c->mismatch_ib[k] & 0xFFFFu); return N48_CM_MISMATCH; }
        }
    } else {
        if (c->pages == 0u || c->sys_pages != c->pages) { if (detail) *detail = c->sys_pages; return N48_CM_PAGE; }
        if (c->wrote != c->n) { if (detail) *detail = c->wrote; return N48_CM_WRITE_SHORT; }
        if (c->got != c->n) { if (detail) *detail = c->got; return N48_CM_READ_SHORT; }
        if (c->back_sys_pages != c->pages) { if (detail) *detail = c->back_sys_pages; return N48_CM_READ_VRAM; }
        if (c->mismatch) { if (detail) *detail = c->mismatch; return N48_CM_MISMATCH; }
    }
    if (!c->token_ok) return N48_CM_TOKEN;
    /* C5 part 1 (gfx_flightring.h) — RING-FULL, placed here (after TOKEN, before MEMDST/DEP_STALE): like MEMDST
     * below, this is a property of the WORLD the commit would run in (is there anywhere to record its flight?),
     * not of the rewrite, so it belongs with the other "world" rungs at the tail rather than among the per-frame
     * ones above. Unconditional (no switch): the ring exists whenever this gate does. */
    if (c->ring_full) return N48_CM_RING_FULL;
    /* C5 part 2 (an internal design note Q2) — FENCE REQUIRED IN CONTINUOUS MODE, placed beside RING_FULL for
     * the same reason: both are properties of the WORLD this commit would run in, not of the rewrite, unconditional
     * on nothing but the caller's own `cont_on` reading (no separate switch rung — `cont_on` IS the gate). */
    if (c->cont_on && !c->cont_fence_ok) return N48_CM_CONT_NO_FENCE;
    /* R1 (an internal design note Q2) — MEMORY-DESTINATION, PLACED HERE (after TOKEN, before DEP_STALE) BY THE
     * DESIGN'S OWN ORDER. Present only under ENFORCE (`md_switch`); SHADOW and OFF leave it 0 and this rung is
     * unreachable, so the gate is 0.0.439's, dword for dword. The PRIMARY gate is the caller's `live` predicate
     * (gfxsrc_commit_try); this is the redundant second check, exactly as N48_CM_DEP_STALE below is for dep_ok. */
    if (c->md_switch && !c->md_ok) return N48_CM_MEMDST;
    /* build 0.0.487 — COMPUTE-ELIDE-R1, beside MEMDST (the rung it depends on): an elided frame needs 42 ENFORCE and
     * R1 clean. With ENFORCE on and R1 not clean MEMDST has already answered; this adds SHADOW/OFF (md_switch 0). */
    if (c->cs_elided && !(c->md_switch && c->md_ok)) { if (detail) *detail = c->cs_elided; return N48_CM_CS_ELIDE_R1; }
    /* build 0.0.500 — DRAW-ELIDE-R1, beside COMPUTE-ELIDE-R1 and by the same rule: an elided draw needs 42 ENFORCE and R1
     * clean. With ENFORCE on and R1 not clean MEMDST has already answered; this adds SHADOW/OFF (md_switch 0). */
    if (c->draw_elided && !(c->md_switch && c->md_ok)) { if (detail) *detail = c->draw_elided; return N48_CM_DRAW_ELIDE_R1; }
    /* 0.0.353 — X9, AND IT IS LAST FOR A STATED REASON. Every rung above is about THIS rewrite; this one is about
     * the WORLD the rewrite would run in — whether any submission has been dropped, leaving a surface holding bytes nobody
     * wrote for a later frame to sample. Two consequences of putting it here, both deliberate:
     *   - It is NOT what stops the irreversible write. The write happens before this gate is ever called, so the caller must
     *     refuse on the same condition BEFORE it writes (AppleHardwareHook.cpp's `live` predicate carries n48_dep_ok). This
     *     rung is the second of two, and the one that ends up in the data.
     *   - The unarmed rehearsal's probe still measures every rung above it. A frame that reaches DEPENDENCY-STALE has been
     *     proved clean on pages, write, read-back, comparison and identity; a rung placed earlier would have hidden all of
     *     that behind a refusal that is true of the whole boot rather than of the frame. */
    if (!c->dep_ok) return N48_CM_DEP_STALE;
    /* 0.0.401 (a item 3,b) — THE IDENTITY LUT, APPENDED LAST FOR THE DEPENDENCY-STALE REASON ABOVE. It is
     * a property of the WORLD this frame would run in (is the LUT written yet?), not of the rewrite, so it belongs with
     * DEPENDENCY-STALE at the end rather than before the per-frame rungs. It is present ONLY while switch 32 is on: with
     * `lut_switch == 0` the whole rung is unreachable and the gate is 0.0.400's, dword for dword. It can only REFUSE, and
     * only a plane-shaped candidate; the one-shot's fill commit and every non-plane frame are untouched. */
    if (c->lut_switch && c->lut_plane && !c->lut_ready) { if (detail) *detail = 0x1u; return N48_CM_LUT_NOT_READY; }
    /* 0.0.404 (a item 2,b) — THE FIRST-SHOT FILL-SET RESERVATION, APPENDED LAST FOR THE SAME DEPENDENCY
     * REASON AS THE LUT RUNG ABOVE. It is a property of the WORLD this frame would run in (is the arm's fill-set window
     * still open?), not of the rewrite. It is present ONLY while switch 33 is on: with `fs_switch == 0` the whole rung is
     * unreachable and the gate is 0.0.403's, dword for dword. It can only REFUSE, and only a TRANSLATE-eligible frame
     * (the NOT-TRANSLATE rung above has already returned), so a non-fill cannot even reach it and the reserved fills pass.
     * `detail` stays 0 (the rung is a state test, not a count). */
    if (c->fs_switch && c->fs_open && !c->fs_reserve) return N48_CM_RESERVED_FOR_FILL;
    /* 0.0.420 (STEP10-PLAN P1) — THE SECOND WINDOW (hold a shot for the plane), APPENDED LAST FOR THE SAME DEPENDENCY
     * REASON. It is a property of the WORLD this frame would run in (has the fill window closed and no plane frame
     * committed yet?). It is present ONLY while switch 35 is on: with `fp_switch == 0` the whole rung is unreachable and
     * the gate is 0.0.419's, dword for dword. The two windows are mutually exclusive (the second opens only once the
     * fill window closes), so at most one of the two rungs can fire for a frame. `detail` stays 0 (a state test). */
    if (c->fp_switch && c->fp_open && !c->fp_reserve) return N48_CM_RESERVED_FOR_PLANE;
    return N48_CM_OK;
}

/* The one value n48_sd_action takes. Nothing else in the kext may compute it. */
static inline uint32_t n48_cm_commit_ok(const n48_cm_frame *c)
{
    uint32_t d = 0u;
    return n48_cm_gate(c, &d) == N48_CM_OK ? 1u : 0u;
}

/* build 0.0.457 item 3 — THE PRE-WRITE `live` PREDICATE, AS THE REHEARSAL ASKS IT. gfxsrc_commit_try
 * writes Apple's IB only when its inline `live` expression is true. Through 0.0.456/0.0.457 that expression was
 * `(arm == COMMIT) && (verdict == TRANSLATE) && gXdBuild.ok && c.buffers_ok && c.dep_ok && (gMdMode != ENFORCE ||
 * gXdBuild.md_ok) && !c.ring_full`, and this function was that expression clause for clause.
 *
 * build 0.0.472 item 1 (MERGE FIX) — THE EXPRESSION GREW AN EIGHTH CLAUSE AT 0.0.460 (C5 part 2, "Require a fence
 * in continuous mode") THAT THIS FUNCTION DID NOT FOLLOW. gfxsrc_commit_try's armed `live` line is now `... && !c.ring_full
 * && (!c.cont_on || c.cont_fence_ok)`, but this function's cherry-picked 0.0.457 body still stopped at `!ring_full` — a
 * literal drift of exactly the kind the project's citation rule warns about (a pin or a mirror that quietly stops matching
 * its target). It was NOT a silent WOULD-COMMIT bug: n48_cm_rehearse's gate call (`n48_cm_gate`, below) carries the SAME
 * cont_on/cont_fence_ok redundant check n48_cm_live was missing (N48_CM_CONT_NO_FENCE), and the rehearsal's probe copies
 * `c.cont_on`/`c.cont_fence_ok` from the real frame verbatim (AppleHardwareHook.cpp's `probe = c;`), so the gate always
 * caught what this function missed and WOULD-COMMIT was never wrongly reported. What WAS wrong: n48_cm_rehearse's
 * early-return ladder (below) named the wrong clause — a continuous frame with no fence fell through every named clause
 * and out the bottom as RING-FULL, because ring_full was the last clause this function knew about. Fixed by adding the
 * two parameters and the matching enum/ladder entry below; tests/gfx_commit_test.cpp's R1 now proves the two equal over
 * all 1024 combinations of the ten inputs, and R5 pins the armed line's CURRENT text (with the cont clause). */
/* build 0.0.487 — A NINTH CLAUSE, `(!cs_elided || (md_enforce && md_ok))` (COMPUTE-ELIDE-R1): the armed line in
 * gfxsrc_commit_try carries it verbatim, so an elided frame is refused BEFORE Apple's IB is written, and the rehearsal
 * names it (N48_CM_RH_CS_ELIDE). cs_elided 0 (switch 57 OFF) makes it a tautology: 0.0.485's predicate exactly. */
/* build 0.0.495 — A TENTH CLAUSE, `!heap_refuse` (switch 62, the shader-heap copy / substitution race): the armed line
 * carries it verbatim (`!c.heap_refuse`), so a frame whose programs a later copy could have rewritten is refused BEFORE
 * Apple's IB is written, and the rehearsal names it (N48_CM_RH_HEAPGEN). heap_refuse 0 (62 OFF) is 0.0.494's predicate. */
/* build 0.0.500 — AN ELEVENTH CLAUSE, `(!draw_elided || (md_enforce && md_ok))` (DRAW-ELIDE-R1, switch 66): the armed
 * line carries it verbatim, so a frame with an elided draw is refused BEFORE Apple's IB is written unless 42 is ENFORCE and
 * R1 clean, and the rehearsal names it (N48_CM_RH_DRAW_ELIDE). draw_elided 0 (66 OFF) is 0.0.499's predicate exactly. */
static inline uint32_t n48_cm_live(uint32_t arm, uint32_t verdict, uint32_t built, uint32_t buffers_ok, uint32_t dep_ok,
                                   uint32_t md_enforce, uint32_t md_ok, uint32_t ring_full,
                                   uint32_t cont_on, uint32_t cont_fence_ok, uint32_t cs_elided, uint32_t heap_refuse,
                                   uint32_t draw_elided)
{
    return (arm == N48_SD_ARM_COMMIT && verdict == N48_XV_TRANSLATE && built && buffers_ok && dep_ok &&
            (!md_enforce || md_ok) && !ring_full && (!cont_on || cont_fence_ok) &&
            (!cs_elided || (md_enforce && md_ok)) && !heap_refuse &&
            (!draw_elided || (md_enforce && md_ok))) ? 1u : 0u;
}

/* build 0.0.457 item 3 — THE WRITE THE REHEARSAL DID NOT MAKE, FROM THE TRANSLATED LENGTHS. Through 0.0.456 the
 * rehearsal probe set `wrote = c.n` and left every `wrote_ib[k]` 0, so on a two-head frame it could only ever answer
 * `write-short` (decide46's 7|5 lines). The armed path writes each IB's candidate over that IB's own range, and the
 * candidate of IB k is what its segments translated: per segment the two passed-through head dwords plus its out_len.
 * So `wrote_ib[k]` is the sum over the segments whose head lies in IB k's range, and `wrote` is IB 0's (the armed
 * path's own scalar), or the whole frame's for a single-IB frame. A segment whose start precedes its head contributes
 * nothing (the gate's COVER/SEG_KIND rungs name it; here it can only make the write SHORT, never long enough). */
static inline void n48_cm_rehearse_wrote(n48_cm_frame *c)
{
    if (!c) return;
    const uint32_t ns = c->nseg < N48_XV_MAX_SEGS ? c->nseg : N48_XV_MAX_SEGS;
    if (!c->mib) {
        uint32_t w = 0u;
        for (uint32_t s = 0; s < ns; s++)
            if (c->seg[s].start >= c->seg[s].head) w += (c->seg[s].start - c->seg[s].head) + c->seg[s].out_len;
        c->wrote = w;
        return;
    }
    uint32_t off = 0u;
    for (uint32_t k = 0; k < c->nib && k < N48_XV_MAX_IBS; k++) {
        const uint32_t end = off + c->ib_n[k];
        uint32_t w = 0u;
        for (uint32_t s = 0; s < ns; s++)
            if (c->seg[s].head >= off && c->seg[s].head < end && c->seg[s].start >= c->seg[s].head)
                w += (c->seg[s].start - c->seg[s].head) + c->seg[s].out_len;
        c->wrote_ib[k] = w;
        off = end;
    }
    c->wrote = c->wrote_ib[0];
}

/* build 0.0.457 item 3 — THE HONEST REHEARSAL'S ANSWER. `p` is the rehearsal probe: a copy of the frame's own
 * n48_cm_frame whose ONE forced field is `arm` = COMMIT (a rehearsal asks "if armed"), whose verdict is the frame's REAL
 * verdict, whose pages / read-back / mismatch are the rehearsal's own walk, and whose writes are n48_cm_rehearse_wrote's.
 * The answer is WOULD-COMMIT only when the armed path's own pre-write predicate (n48_cm_live, the SAME function) and
 * its gate (n48_cm_gate, the SAME function, which carries switch 53's rung) both pass and no fill/plane window is on
 * (those windows step only at COMMIT and are not modelled, so while either is on nothing is called WOULD-COMMIT).
 * Otherwise it names the FIRST rung that refuses: the live predicate's clauses in its own order, then the gate's reason
 * (*gate_out) with its detail. What it cannot model is named on the line, never assumed: the keystone in hook_gfxCommitIB,
 * a WindowServer rebind under the arm, and any rung whose input differs between DECIDE and COMMIT (the fence is placed
 * only at COMMIT; its one refusal, REGION-MOVED, needs a placed candidate). The first spend of an arm always has budget
 * (n48_cm_shot_budget_of reads 0 as 1), so the budget is printed, not a rung. Pure; the rehearsal writes nothing. */
enum {
    N48_CM_RH_WOULD = 0, N48_CM_RH_VERDICT, N48_CM_RH_NOT_ARMED, N48_CM_RH_NOT_BUILT, N48_CM_RH_NO_BUFFERS, N48_CM_RH_DEP,
    N48_CM_RH_MEMDST, N48_CM_RH_RING, N48_CM_RH_GATE, N48_CM_RH_WINDOW,
    /* build 0.0.472 item 1 — CONTINUOUS-NO-FENCE, NAMED DIRECTLY. Appended, never inserted (the values are printed
     * and compared by tests, exactly like the gate's own enum above). Before this, a continuous-mode probe with no fence
     * candidate fell through every clause n48_cm_live's early-return ladder named and out the bottom as N48_CM_RH_RING
     * (wrong name, right refusal — the gate's own redundant CONT_NO_FENCE check meant WOULD-COMMIT was never mis-reported,
     * only the NAMED reason was). See n48_cm_live's own comment. */
    N48_CM_RH_CONT_NO_FENCE,
    /* build 0.0.487 — n48_cm_live's ninth clause (COMPUTE-ELIDE-R1), APPENDED: switch 57 elided a dispatch in this
     * frame and 42 is not ENFORCE or R1 is not clean. */
    N48_CM_RH_CS_ELIDE,
    /* build 0.0.495 — n48_cm_live's tenth clause (switch 62), APPENDED. */
    N48_CM_RH_HEAPGEN,
    /* build 0.0.500 — n48_cm_live's eleventh clause (DRAW-ELIDE-R1, switch 66), APPENDED. */
    N48_CM_RH_DRAW_ELIDE,
    N48_CM_RH_REASONS
};
static inline const char *n48_cm_rh_name(uint32_t r)
{
    static const char *const n[N48_CM_RH_REASONS] = {
        "WOULD-COMMIT", "verdict", "not-armed", "not-built", "no-buffers", "DEPENDENCY-STALE", "MEMORY-DESTINATION",
        "RING-FULL", "gate", "fill/plane-window", "CONTINUOUS-NO-FENCE", "COMPUTE-ELIDE-R1", "SHADER-HEAP-GEN",
        "DRAW-ELIDE-R1" };
    return r < N48_CM_RH_REASONS ? n[r] : "?";
}
static inline uint32_t n48_cm_rehearse(const n48_cm_frame *p, uint32_t built, uint32_t md_enforce, uint32_t md_ok,
                                       uint32_t windows, uint32_t *gate_out, uint32_t *detail)
{
    uint32_t d = 0u;
    const uint32_t g = n48_cm_gate(p, &d);
    if (gate_out) *gate_out = g;
    if (detail) *detail = d;
    if (!p) return N48_CM_RH_NOT_ARMED;
    if (!n48_cm_live(p->arm, p->verdict, built, p->buffers_ok, p->dep_ok, md_enforce, md_ok, p->ring_full,
                      p->cont_on, p->cont_fence_ok, p->cs_elided, p->heap_refuse, p->draw_elided)) {
        if (p->arm != N48_SD_ARM_COMMIT) return N48_CM_RH_NOT_ARMED;
        if (p->verdict != N48_XV_TRANSLATE) { if (detail) *detail = p->verdict; return N48_CM_RH_VERDICT; }
        if (!built) return N48_CM_RH_NOT_BUILT;
        if (!p->buffers_ok) return N48_CM_RH_NO_BUFFERS;
        if (!p->dep_ok) return N48_CM_RH_DEP;
        if (md_enforce && !md_ok) return N48_CM_RH_MEMDST;
        if (p->ring_full) return N48_CM_RH_RING;
        /* build 0.0.472 item 1 — `(!cont_on || cont_fence_ok)`, named by its own test now that it is no longer
         * the last clause (build 0.0.487 appended one after it). */
        if (p->cont_on && !p->cont_fence_ok) return N48_CM_RH_CONT_NO_FENCE;
        /* build 0.0.487 — `(!cs_elided || (md_enforce && md_ok))`, named by its own test now that it is no longer
         * the last clause (build 0.0.495 appended one after it). */
        if (p->cs_elided && !(md_enforce && md_ok)) return N48_CM_RH_CS_ELIDE;
        /* build 0.0.495 — `!heap_refuse`, named by its own test now that it is no longer the last clause (build
         * 0.0.500 appended one after it). */
        if (p->heap_refuse) return N48_CM_RH_HEAPGEN;
        /* build 0.0.500 — the only clause left is `(!draw_elided || (md_enforce && md_ok))`, so a fall-through here can
         * only mean it failed. */
        return N48_CM_RH_DRAW_ELIDE;
    }
    if (g != N48_CM_OK) return N48_CM_RH_GATE;
    if (windows) return N48_CM_RH_WINDOW;
    return N48_CM_RH_WOULD;
}

/* The read-back comparator. Returns HOW MANY dwords differ (never a bool: a run that reports "1 mismatch" and a run that
 * reports "1040 mismatches" are different findings) and, when `first` is non-null, the index of the first. */
static inline uint32_t n48_cm_compare(const uint32_t *want, const uint32_t *back, uint32_t n, uint32_t *first)
{
    uint32_t bad = 0u, at = 0u;
    if (first) *first = 0u;
    if (!want || !back) return n ? n : 1u;      /* a comparison that could not be made is a full mismatch, never a pass */
    for (uint32_t i = 0; i < n; i++)
        if (want[i] != back[i]) { if (!bad) at = i; bad++; }
    if (first) *first = at;
    return bad;
}

/* Per-boot accounting for the commit path, printed on every read of the verb. */
typedef struct {
    uint64_t built, buildRefused;        /* rewrites assembled into the candidate buffer / builds that did not cover */
    uint64_t attempts;                   /* frames that reached the gate */
    uint64_t commits;                    /* gate == N48_CM_OK */
    uint64_t byReason[N48_CM_REASONS];
    uint64_t dryRuns, dryClean, dryDirty;/* the unarmed rehearsal: read-back of the UNCHANGED IB against our gathered copy */
    uint64_t writes, writeDwords, mismatchDwords;
    uint32_t lastReason, lastDetail, lastFirstBad;
    uint64_t lastVa; uint32_t lastN, lastNseg;
} n48_cm_stats;

/* =====================================================================================================================
 * 0.0.371 — THE ARM, AND THE ONE-SHOT THAT SPENDS IT. Everything below is NEW; nothing above it changed.
 *
 * WHY THIS IS HERE AND NOT IN AppleHardwareHook.cpp. found a PHANTOM LEVER: `accel gfxneuter 4` had refused since
 * 0.0.352 and `gXdArm` had exactly two assignments in the whole tree, DECIDE and OFF, so `N48_CM_OK` was unreachable for
 * every frame however clean the world — and the project's own prose had promoted the planned lever to an existing one.
 * The lever is built here, as DATA, for the same reason the gate above is data: a condition that lives in an `if` inside a
 * 19,000-line file is reviewed once; a condition that lives in a pure function is host-tested, mutated, and cannot rot.
 *
 * TWO SEPARATE THINGS, AND CONFLATING THEM IS THE DEFECT THIS SPLIT EXISTS TO PREVENT:
 *   1. n48_cm_arm_missing — MAY the arm be set at all? Asked ONCE, by the verb, against the live world. It is the
 *      pre-arm checklist expressed as a bitmask, and it names EVERY missing item rather than the first, because the point
 *      of the refusal is to tell the operator what to fix, not to stop at the first thing.
 *   2. n48_cm_shot_* — the ONE-SHOT. Having been set, how long does the arm last? Exactly one committed frame.
 * Neither may weaken the per-frame gate above. Arming makes `arm == COMMIT` TRUE; it does not make any other rung pass.
 * ===================================================================================================================== */

/* ---- 1. THE ARM GATE -------------------------------------------------------------------------------------------------
 * One bit per item. A bit SET means the item is MISSING. Zero-initialised means "nobody checked", and since every field of
 * n48_cm_arm_req is a positive assertion, a request nobody filled has every bit missing and refuses on all of them — the
 * same fail-closed-by-construction shape as n48_cm_frame.
 *
 * WHAT IS DELIBERATELY NOT HERE: the DROPPED-WRITE COUNTS. proved that `source-neuter` cannot be 0 in judge-only mode
 * BY CONSTRUCTION — at DECIDE every frame is neutered and each neutered submission is by definition a dropped write — so a
 * rung on the counts would make the arm un-settable before a boot had finished arming, which is n48_dep_may_drop_tvram's
 * reasoning one level up. The counts remain the authority over whether any given FRAME commits: the per-frame gate's
 * N48_CM_DEP_STALE rung and the caller's `live` predicate both carry n48_dep_ok, and neither is touched by this. So a
 * boot may be ARMED and still commit nothing, which is the correct direction. */
enum {
    N48_CM_ARM_DECIDE_FIRST = 0x0001u, /* the DECISION is not armed: `accel gfxneuter 3` has not run (or was refused) */
    N48_CM_ARM_BUFFERS      = 0x0002u, /* the candidate IB and/or the read-back scratch are not allocated */
    N48_CM_ARM_SAMPLED      = 0x0004u, /* the X9 world handed in was not freshly sampled — nothing in it may be believed */
    N48_CM_ARM_OBSERVERS    = 0x0008u, /* not all seven X9 observers are live (the mask itself is reported beside this) */
    N48_CM_ARM_EARLY        = 0x0010u, /* X9's EARLY observer is not live (named apart: it is E1's own consequence) */
    N48_CM_ARM_E1           = 0x0020u, /* RULE E1 did not PASS at arm time */
    N48_CM_ARM_N1           = 0x0040u, /* N1 (`target-in-vram` dropped from the ladder) is OFF */
    N48_CM_ARM_LATCH        = 0x0080u, /* the context latch is not ENFORCED this boot */
    N48_CM_ARM_DESCPORT     = 0x0100u, /* the descriptor port is OFF */
    N48_CM_ARM_RESPROV      = 0x0200u, /* residency provenance is OFF */
    N48_CM_ARM_HEADLESS     = 0x0400u, /* the headless setup-half recogniser is OFF */
    N48_CM_ARM_ALL          = 0x07FFu,
    N48_CM_ARM_ITEMS        = 11
};

static inline const char *n48_cm_arm_item_name(uint32_t bit)
{
    switch (bit) {
    case N48_CM_ARM_DECIDE_FIRST: return "the DECISION is not armed - run `accel gfxneuter 3` first";
    case N48_CM_ARM_BUFFERS:      return "the candidate IB and read-back scratch are not both allocated";
    case N48_CM_ARM_SAMPLED:      return "the X9 world was not freshly sampled - nothing in it may be believed";
    case N48_CM_ARM_OBSERVERS:    return "X9 does not have all seven observers live";
    case N48_CM_ARM_EARLY:        return "X9's EARLY observer is not live (RULE E1's consequence, )";
    case N48_CM_ARM_E1:           return "RULE E1 did not PASS at arm time";
    case N48_CM_ARM_N1:           return "N1 is OFF - `accel gfxneuter 12 | 1 << 8`";
    case N48_CM_ARM_LATCH:        return "the context latch is not ENFORCED (navi48-boot-chain bit 5, A.2)";
    case N48_CM_ARM_DESCPORT:     return "the descriptor port is OFF - `accel gfxneuter 10 | 1 << 8`";
    case N48_CM_ARM_RESPROV:      return "residency provenance is OFF - `accel gfxneuter 11 | 1 << 8`";
    case N48_CM_ARM_HEADLESS:     return "the headless recogniser is OFF - `accel gfxneuter 13 | 1 << 8`";
    default:                      return "?";
    }
}

/* What the verb OBSERVED, each field a positive assertion. `observers` is the live mask and `observers_required` what it
 * must equal — both carried so the refusal can print which bits are missing without this header knowing gfx_dep.h's
 * numbering (it does include it, but a rung that re-derived the requirement would be judging itself). */
typedef struct {
    uint32_t decide_armed;        /* the arm level is DECIDE right now */
    uint32_t buffers_ok;          /* gXdNew && gXdBack */
    uint32_t sampled;             /* the world handed in has `sampled == 1` */
    uint32_t observers;           /* the world's live observer mask */
    uint32_t observers_required;  /* N48_DEP_OBS_REQUIRED, passed in */
    uint32_t early_bit;           /* the EARLY bit's value within that mask, passed in (N48_DEP_OBS_EARLY) */
    uint32_t e1_pass;             /* RULE E1 returned PASS at arm time (gRd.preE1) */
    uint32_t n1_on;
    uint32_t latch_enforced;
    uint32_t descport_on;
    uint32_t resprov_on;
    uint32_t headless_on;
} n48_cm_arm_req;

/* EVERY missing item, not the first. 0 means the arm may be set. */
static inline uint32_t n48_cm_arm_missing(const n48_cm_arm_req *a)
{
    if (!a) return N48_CM_ARM_ALL;
    uint32_t m = 0u;
    if (a->decide_armed != 1u)   m |= N48_CM_ARM_DECIDE_FIRST;
    if (a->buffers_ok != 1u)     m |= N48_CM_ARM_BUFFERS;
    if (a->sampled != 1u)        m |= N48_CM_ARM_SAMPLED;
    /* The observer rungs are asked of the SAMPLED world only. An unsampled world's mask is not evidence of anything, so
     * it refuses at SAMPLED above AND at both rungs below - never at SAMPLED alone, which would let "0 observers" read as
     * "one thing to fix". */
    if (a->sampled != 1u || a->observers_required == 0u ||
        (a->observers & a->observers_required) != a->observers_required) m |= N48_CM_ARM_OBSERVERS;
    if (a->sampled != 1u || a->early_bit == 0u || !(a->observers & a->early_bit)) m |= N48_CM_ARM_EARLY;
    if (a->e1_pass != 1u)        m |= N48_CM_ARM_E1;
    if (a->n1_on != 1u)          m |= N48_CM_ARM_N1;
    if (a->latch_enforced != 1u) m |= N48_CM_ARM_LATCH;
    if (a->descport_on != 1u)    m |= N48_CM_ARM_DESCPORT;
    if (a->resprov_on != 1u)     m |= N48_CM_ARM_RESPROV;
    if (a->headless_on != 1u)    m |= N48_CM_ARM_HEADLESS;
    return m;
}

static inline uint32_t n48_cm_may_arm(const n48_cm_arm_req *a) { return n48_cm_arm_missing(a) == 0u ? 1u : 0u; }

/* ---- 2. THE ONE-SHOT ---------------------------------------------------------------------------------------------
 * ARMED -> SPENT -> DONE, and ARMED -> CANCELLED. The blast radius is exactly one frame because a NEW frame is handed
 * COMMIT only in ARMED, and the gate answering N48_CM_OK for one frame moves it to SPENT.
 *
 * WHY SPENT AND DONE ARE TWO STATES, WHICH IS THE ONE PLACE THIS DEPARTS FROM THE ONE-LINE DESCRIPTION "disarm the instant
 * the gate answers OK". The arm level is read a SECOND time, AFTER the gate, by something the gate knows nothing about:
 * the ring walk's exemption (gfx_neuter.h `n48_gfxn_exempt_why`, condition (1) `arm != N48_GFXN_ARM_COMMIT`), which runs
 * inside Apple's own `commitIndirectCommandBuffer` on the same thread and is what decides whether the committed IB is
 * spared from the NOP. Writing DECIDE into the arm level at the gate would therefore make the walk NOP the very IB we had
 * just rewritten and read back: the frame would commit on paper and draw nothing, and the run would score N/A on its own
 * headline. So:
 *   SPENT  is entered AT the gate's OK and is what stops the frame AFTER the budget's last: n48_cm_shot_level hands
 *          COMMIT out only in ARMED, so no later frame can be judged at COMMIT even before the level variable moves.
 *          0.0.381 puts a BUDGET in front of that transition - see 2a - and at the default budget of 1 the
 *          transition happens on the first spend, which is this paragraph unchanged.
 *   DONE   is entered when that frame's submission has RETURNED, and is when the caller writes DECIDE into the level.
 * The two are one frame apart on one thread, and the safety property "at most one frame is ever handed COMMIT" is carried
 * by SPENT, not by the level variable. n48_cm_shot_spend is idempotent for exactly this reason: it answers 1 once.
 *
 * WINDOWSERVER'S BINDING, AND WHY "ANY CHANGE CANCELS" WOULD BE WRONG HERE. The arm is set BEFORE `wskill`, and at that
 * moment ws_ident reads `state NONE ... binds 0 (rebinds 0), unbinds 0` (hp9's own log,): the compositor that will
 * draw the committed frame DOES NOT EXIST YET. So cancelling on any change of binding would cancel on the very bind the
 * run exists to catch. What must not survive is a binding being REPLACED or LOST under the arm — a second compositor, a
 * freed root — so the rule is: at most ONE new binding may appear after arming, and it must appear cleanly. A rebind, an
 * unbind, a `gone`, or a second bind all cancel; a counter that went DOWN cancels too, because a record that is not
 * monotone is not a record. */
enum {
    N48_CM_SHOT_OFF = 0,     /* never armed, or armed and finished: no frame may be handed COMMIT */
    N48_CM_SHOT_ARMED,       /* armed and unspent: the NEXT frame that clears every rung commits */
    N48_CM_SHOT_SPENT,       /* the gate answered OK for one frame; that frame is still in flight */
    N48_CM_SHOT_DONE,        /* that frame's submission returned; the level has been put back to DECIDE */
    N48_CM_SHOT_CANCELLED,   /* WindowServer's binding moved under the arm; the level has been put back to DECIDE */
    N48_CM_SHOT_STATES
};

/* C5 part 1 (hygiene, an internal design note Q3) — CHECKED AGAINST BUDGET > 1. "ARMED (one-shot)" and "SPENT
 * (one frame committed, in flight)" were true only at budget 1: with a budget of 2-4, ARMED may still accept MORE
 * than one frame before it moves to SPENT (n48_cm_shot_spend only moves the state once `spent >= budget`), and SPENT
 * means the budget is EXHAUSTED, not that exactly one frame was ever handed COMMIT this arm. Reworded to be true at
 * every budget; DONE/CANCELLED/OFF needed no change (DONE and CANCELLED are already budget-general, and OFF is
 * OFF at every budget). */
static inline const char *n48_cm_shot_name(uint32_t s)
{
    static const char *const n[N48_CM_SHOT_STATES] = {
        "OFF", "ARMED (unspent budget remains)", "SPENT (budget exhausted; the last frame is in flight)",
        "DONE (budget spent and disarmed)", "CANCELLED (WindowServer rebound)" };
    return s < N48_CM_SHOT_STATES ? n[s] : "?";
}

/* ---- 2a. THE N-FRAME BUDGET (0.0.381) -------------------------------------------------------------------
 * WHY A ONE-SHOT IS NOT ENOUGH, AND WHY THIS IS NOT A LOOSENING OF THE ONE-SHOT. measured the problem: COMMIT is a
 * one-shot that disarms the instant a frame commits, `arm9` spent it on `f1` at `sn 0`, and the frames anything later can
 * buy are `f15` and `f29` - 14 and 28 frames later, ~0.25 s and ~0.5 s at 60 Hz. No operator step list can re-arm by hand
 * inside that window, so a second-commit experiment cannot be run at all without this. The budget does NOT make any frame
 * commit that would not have committed: every rung of n48_cm_gate, n48_xv_decide, n48_dep_check and the ring exemption is
 * untouched. It changes exactly one thing: HOW MANY frames that clear all of them may do so before the arm is spent.
 *
 * DEFAULT N = 1, AND THAT IS TODAY BYTE FOR BYTE. `budget` 0 is read as 1 (a caller that forgot the field gets the
 * one-shot, never more), and at budget 1 the first accepted spend takes `spent` 0 -> 1, 1 >= 1, and the state moves
 * ARMED -> SPENT on that same call, exactly as 0.0.380's `n48_cm_shot_spend` did unconditionally.
 *
 * IDEMPOTENT PER TOKEN SEQ, NOT PER CALL - AND THIS IS THE DEFECT THE SPLIT EXISTS TO PREVENT. Until 0.0.380 the
 * idempotence was carried by the STATE: the second call for the same frame found SPENT and answered 0. With a budget
 * above 1 the state is still ARMED after the first spend, so a second call for the SAME frame would charge the budget
 * twice and halve it silently. `last_seq` is therefore the key: a spend is accepted once per distinct token seq, and a
 * repeat for the seq just charged answers 0 without moving anything. `spent != 0` guards the initial `last_seq` 0, so a
 * token seq of 0 is charged normally rather than mistaken for a repeat.
 *
 * A WASTED SPEND STILL COSTS BUDGET. `n48_cm_shot_finish`'s caller also calls it when the spent frame was neutered at the
 * hook instead (TRANSLATE WITHDRAWN). At budget 1 that put the arm away; at budget N the state is still ARMED and finish
 * answers 0, so the arm stands and the next frame may use the REMAINING budget. The charge is not refunded: a spend that
 * reached the client's memory and then lost its identity is exactly the event the bound is counting.
 *
 * THE CEILING. N48_CM_SHOT_BUDGET_MAX is 4 and the rule coerces anything above it back to 1 rather than accepting it.
 * The project has evidence for exactly three frames worth committing (`f1`, `f15`, `f29` - ) and 4 is one of
 * slack; every unit above that is another live rewrite of Apple's instruction stream bought with no measurement behind
 * it. The verb refuses an out-of-range N outright; this is the second of the two guards, not the first. */
#define N48_CM_SHOT_BUDGET_MAX 4u

/* C5 part 2 (an internal design note Q2, build 0.0.460) — THE CONTINUOUS ARM'S OWN CEILINGS. N (the frame
 * budget) up to 1022 (build 0.0.530 item 9, : approved raising 600 to the 10-bit field's top;
 * 1023 = 0x3FF is switch 41's CLEAR sentinel, so 1022 is the largest settable N) and T (the deadline, in 10-ms units) up to 500 (5.000 s) are the USER'S OWN DECISION for the 10a
 * run (N = 60, T = 5 s); the ceilings are the verb's second guard, exactly as N48_CM_SHOT_BUDGET_MAX is the one-
 * shot's: the verb refuses an out-of-range value outright, and this header refuses it again rather than trusting the
 * caller checked. A continuous shot's `budget` field is UNUSED (0): n48_cm_shot_budget_of reads `cont_n` instead
 * whenever `cont` is set, so the two ceilings can never be confused. */
#define N48_CM_CONT_N_MAX 1022u
#define N48_CM_CONT_T_MAX 500u   /* 10-ms units: 500 * 10 ms = 5.000 s */

/* build 0.0.473 item 1 — A LONGER WINDOW BEHIND THE SAME SWITCH (10b: the user watches for up to a minute).
 * Bit 28 of the switch-41 argument selects T's UNIT: clear, T is in 10-ms units exactly as 0.0.472 (1..500, the
 * field's own N48_CM_CONT_T_MAX ceiling above, UNCHANGED - the OFF identity); set, T is in 100-ms units (1..600 =
 * 0.1..60.0 s). N48_CM_CONT_T_MAX_US is the ONE cap, in microseconds, that BOTH unit paths are checked against
 * (n48_cm_cont_t_us below, and again by n48_cm_shot_arm_cont_us, the header's own second guard): 600 x 100 ms is
 * exactly it, 601 x 100 ms is over it. The 10-ms path keeps its own 500 ceiling as well, so every value it accepted or
 * refused in 0.0.472 it accepts or refuses here, with the same microseconds. The shot freezes microseconds
 * (`cont_t_us`), and every stop reads only that field, so the stop logic itself is unit-blind. */
#define N48_CM_CONT_T_MAX_US 60000000ull   /* 60.000 s: the single cap on T, in either unit */
#define N48_CM_CONT_T_UNIT_BIT 28u          /* switch 41: set = T in 100-ms units, clear = 10-ms units (0.0.472) */

/* T in microseconds for a switch-41 T field in the unit bit 28 selects; 0 means REFUSED (0, over the 10-ms path's
 * own 500 ceiling, or over N48_CM_CONT_T_MAX_US). Pure; the verb and the arm both ask it. */
static inline uint64_t n48_cm_cont_t_us(uint32_t t_field, uint32_t unit_100ms)
{
    if (t_field == 0u) return 0ull;
    if (!unit_100ms && t_field > N48_CM_CONT_T_MAX) return 0ull;
    const uint64_t us = (uint64_t)t_field * (unit_100ms ? 100000ull : 10000ull);
    if (us > N48_CM_CONT_T_MAX_US) return 0ull;
    return us;
}

/* The switch-41 argument, decoded exactly as the verb acts on it: `41 | N << 8 | T << 18 | U << 28`, N and T 10 bits
 * each, U the unit bit. N == 0x3FF clears the request (whatever T and U read); N and T both 0 is a bare read (U alone
 * is a read too); anything else SETS only when N is 1..N48_CM_CONT_N_MAX and n48_cm_cont_t_us accepts T in its unit,
 * and is otherwise REFUSED with nothing changed. Bits above 28 are ignored, as bits above 27 were in 0.0.472. */
/* The verb's one line: N, T's field and unit, T in seconds (s.mmm) and microseconds, what the verb did, whether a
 * continuous arm is requested, then the ceilings (N max, the 10-ms T max, the 100-ms T max, the unit bit). Bounded
 * under N48_LOG_CAP_BODY by tests/gfx_commit_test.cpp. */
#define N48_CM_SW41_FMT "gfx-commit: SWITCH 41 (continuous N/T) is N %u, T %u x %s ms = %llu.%03llu s (%llu us)%s%s. " \
                        "Ceilings: N 1..%u; T 1..%u x 10 ms, or 1..%llu x 100 ms with bit %u set (60 s max); N 0x3FF " \
                        "clears. `gfxneuter 4 | 1 << 8` reads these at the next arm; N 0 (default) arms a plain one-shot."
enum { N48_CM_SW41_READ = 0, N48_CM_SW41_SET, N48_CM_SW41_CLEAR, N48_CM_SW41_REFUSED };
typedef struct { uint32_t action, n, t, unit_100ms; uint64_t t_us; } n48_cm_sw41;
static inline n48_cm_sw41 n48_cm_sw41_decode(uint64_t arg)
{
    n48_cm_sw41 d;
    d.n = (uint32_t)((arg >> 8) & 0x3FFull);
    d.t = (uint32_t)((arg >> 18) & 0x3FFull);
    d.unit_100ms = (uint32_t)((arg >> N48_CM_CONT_T_UNIT_BIT) & 1ull);
    d.t_us = 0ull;
    if (d.n == 0x3FFu) { d.action = N48_CM_SW41_CLEAR; return d; }
    if (d.n == 0u && d.t == 0u) { d.action = N48_CM_SW41_READ; return d; }
    d.t_us = n48_cm_cont_t_us(d.t, d.unit_100ms);
    d.action = (d.n == 0u || d.n > N48_CM_CONT_N_MAX || d.t_us == 0ull) ? N48_CM_SW41_REFUSED : N48_CM_SW41_SET;
    if (d.action == N48_CM_SW41_REFUSED) d.t_us = 0ull;
    return d;
}

/* build 0.0.543 item D (a policy decision: "about 4,000 frames / 120 seconds")
 * - SWITCH 102, THE LONGER CONTINUOUS ARM. Switch 41's argument is UNCHANGED (N 10 bits, 1..N48_CM_CONT_N_MAX; 0x3FF its CLEAR; T
 * at most N48_CM_CONT_T_MAX_US): it cannot carry 4000 or 120 s. Switch 102 is a SECOND way to set the SAME request (the kext's
 * gXdContN / gXdContT / gXdContTUnit100 / gXdContTUs, plus gXdContExt = 1), with wider fields and its OWN ceilings:
 *   `102 | N << 8 | T << 20`   N bits [8:19] (12 bits: 1..N48_CM_CONT_N_MAX_EXT = 4000; 0xFFF CLEARS the request, whatever T),
 *                              T bits [20:30] (11 bits, ALWAYS 100-ms units: 1..1200 = 0.1..120.0 s), bit 31 and above MUST be 0.
 * N and T both 0 is a bare read. Anything else SETS only when N is 1..4000 and T is 1..1200 (and no bit above 30 is set), and is
 * otherwise REFUSED with nothing changed. The arm then goes through n48_cm_shot_arm_cont_ext_us (below), the header's own second
 * guard for these ceilings; switch 41's arm (n48_cm_shot_arm_cont_us) keeps N48_CM_CONT_N_MAX / N48_CM_CONT_T_MAX_US exactly. */
#define N48_CM_CONT_N_MAX_EXT 4000u
#define N48_CM_CONT_T_MAX_US_EXT 120000000ull   /* 120.000 s */
#define N48_CM_SW102_N_CLEAR 0xFFFu
#define N48_CM_SW102_T_MAX 1200u                /* 100-ms units: 1200 x 100 ms = N48_CM_CONT_T_MAX_US_EXT */
static inline n48_cm_sw41 n48_cm_sw102_decode(uint64_t arg)
{
    n48_cm_sw41 d;
    d.n = (uint32_t)((arg >> 8) & 0xFFFull);
    d.t = (uint32_t)((arg >> 20) & 0x7FFull);
    d.unit_100ms = 1u;
    d.t_us = 0ull;
    if (arg >> 31) { d.action = N48_CM_SW41_REFUSED; return d; }   /* a bit past the fields: never guessed, refused */
    if (d.n == N48_CM_SW102_N_CLEAR) { d.action = N48_CM_SW41_CLEAR; return d; }
    if (d.n == 0u && d.t == 0u) { d.action = N48_CM_SW41_READ; return d; }
    if (d.t >= 1u && d.t <= N48_CM_SW102_T_MAX) d.t_us = (uint64_t)d.t * 100000ull;
    d.action = (d.n == 0u || d.n > N48_CM_CONT_N_MAX_EXT || d.t_us == 0ull || d.t_us > N48_CM_CONT_T_MAX_US_EXT)
               ? N48_CM_SW41_REFUSED : N48_CM_SW41_SET;
    if (d.action == N48_CM_SW41_REFUSED) d.t_us = 0ull;
    return d;
}
/* The verb's line (bounded under N48_LOG_CAP_BODY by tests/gfx_commit_test.cpp): N, T's field, T in s.mmm and us, what the verb
 * did, whether a request stands and through which switch, then the ceilings. */
#define N48_CM_SW102_FMT "gfx-commit: SWITCH 102 (the longer continuous N/T) - the request is N %u, T %u x %s ms = %llu.%03llu s (%llu " \
                         "us), set by switch %u%s%s. Ceilings: N 1..%u, T 1..%u x 100 ms (%llu s); N 0xFFF clears. Switch 41 is " \
                         "unchanged (N 1..%u, %llu s)."

/* F2 (0.0.461 review) — THE HARD PRE-PLANE BOUND. Independent of switch 35 (arm-refusal F2(a) requires 35 ON, but
 * this is the SECOND, unconditional guard: a plane frame that never comes - WindowServer never binds, or binds and
 * never draws a GPUPass frame in this arm's window - must not leave a continuous arm standing forever with only N
 * as a backstop, since N alone (up to 1022,) could still take a very long time to exhaust if the cadence is slow.
 * 30 SECONDS: chosen to be well above every measured judged-frame cadence in this project's own logs (Q5: ~6.9
 * judged frames/s on an idle login screen, arm35; the slowest recorded boots still reach a first commit within
 * single-digit seconds - 's "~10.2-10.4 s before the plane commit" was measured FROM THE ARM, i.e.
 * comfortably under this bound already) while being short enough that an operator watching a MONITOR-level 10a run
 * is not left waiting minutes for a run that was never going to produce a plane. */
#define N48_CM_PREPLANE_BOUND_US 30000000ull

/* C5 part 2 (an internal design note Q2) — WHY THE CONTINUOUS ARM STOPPED. 0 (NONE) is the only value a
 * standing ARMED shot ever carries; every other value is written ONCE, by n48_cm_shot_spend (N reached, the SAME
 * transition point a one-shot's budget exhaustion already uses) or by n48_cm_shot_stop (every other cause), at the
 * SAME instant the state moves ARMED -> SPENT, and it is never cleared except by a fresh arm or a disarm — so DONE
 * and SPENT both carry the true reason for as long as the shot stands, for xd_shot_finish's own stop line. Appended
 * order matches the WORK item's own list (an internal design note Q2 "Disarm conditions"); the numeric values
 * are never printed anywhere but this shot's own report line (unlike N48_CM_REASONS, nothing else quotes them), so
 * there is no append-only constraint here — this is a fresh enum for a fresh build. */
enum {
    N48_CM_STOP_NONE = 0,       /* the shot is still ARMED, or was never armed */
    N48_CM_STOP_N,              /* N reached — n48_cm_shot_spend, budget exhausted (the SAME transition a one-shot uses) */
    N48_CM_STOP_T,              /* T reached — the top of gfxsrc_decide_frame, against drain_now_us() */
    N48_CM_STOP_WITHDRAWAL,     /* a withdrawal happened at hook_unmapVA while a flight was still live (NOW_TIMEOUT/TORN/OFF) */
    N48_CM_STOP_DEFER_HAZARD,   /* gKsD.freedUnderDefer or gKsD.latchedInWindow rose since the arm */
    N48_CM_STOP_VM_FAULT,       /* gVmFaultLatched rose since the arm */
    N48_CM_STOP_FENCE_LATE,     /* F5/F5b (0.0.462): a per-entry counter rose - 4 polls or 250 ms with no retirement */
    N48_CM_STOP_FENCE_CORRUPT,  /* a ring poll read a non-zero value that was not ours (CHANGED-BUT-NOT-OURS) */
    N48_CM_STOP_OUT_OF_ORDER,   /* the flight ring's own OUT-OF-ORDER count rose since the arm */
    N48_CM_STOP_FRAME_CAP,      /* the judged-frame cap (gXdFrameCap) was reached */
    /* F2 (0.0.461 review) — a HARD PRE-PLANE BOUND, independent of switch 35: no plane commit within
     * N48_CM_PREPLANE_BOUND_US of the arm. Appended, not inserted (the same "fresh enum" rule as every value
     * above: nothing outside this shot's own report line quotes these numbers). */
    N48_CM_STOP_PREPLANE_TIMEOUT,
    /* SMALLER (0.0.461 review) — WindowServer's binding moved under a CONTINUOUS arm (the one-shot's own
     * N48_CM_SHOT_CANCELLED state, generalised): named here so the continuous stop line can say CANCELLED
     * instead of a silent, unnamed disarm. */
    N48_CM_STOP_CANCELLED,
    N48_CM_STOP_REASONS
};

static inline const char *n48_cm_stop_name(uint32_t w)
{
    static const char *const n[N48_CM_STOP_REASONS] = {
        "none (still armed)", "N REACHED (the frame budget is spent)", "T REACHED (the deadline from the first plane commit)",
        "A WITHDRAWAL happened while a flight was still live",
        "A KEYSTONE-DEFER HAZARD (freed-under-defer or a fault latched in a withdrawal window)",
        "A LATCHED VM FAULT", "A FENCE HAS NOT RETIRED WITHIN ITS BOUND (4 polls or 250 ms)",
        "A FENCE READ CHANGED BUT NOT OURS (corrupted)", "AN ENTRY RETIRED OUT OF ORDER", "THE JUDGED-FRAME CAP",
        "NO PLANE COMMIT WITHIN THE PRE-PLANE BOUND", "CANCELLED (WindowServer rebound)"
    };
    return w < N48_CM_STOP_REASONS ? n[w] : "?";
}

/* WindowServer's binding as ws_ident counts it. Compared, never interpreted. */
typedef struct { uint64_t binds, rebinds, unbinds, gone; } n48_cm_ws_mark;

typedef struct {
    uint32_t state;
    uint32_t spent_seq;        /* the token seq that EXHAUSTED the budget (0 until SPENT) */
    uint64_t armed_at_us;
    n48_cm_ws_mark ws;         /* ws_ident's counters as they read AT ARM TIME */
    /* 0.0.381 - THE BUDGET. `budget` 0 means 1: see 2a. */
    uint32_t budget;           /* frames this arm may spend. Set AT the arm and never afterwards */
    uint32_t spent;            /* frames charged so far. 0 <= spent <= budget */
    uint32_t last_seq;         /* the token seq of the LAST accepted spend - the per-seq idempotence key */
    /* C5 part 2 (an internal design note Q2, build 0.0.460) — THE CONTINUOUS ARM. All five fields are 0 on a
     * one-shot arm (n48_cm_shot_arm zeroes them, and a disarmed/zero-initialised shot already reads 0), which is the
     * WHOLE of the OFF identity: nothing below reads any of these unless `cont` is 1.
     *   cont            1 while this shot is a CONTINUOUS arm (n48_cm_shot_arm_cont set it); 0 is a one-shot.
     *                   build 0.0.473 item 3: cleared again by n48_cm_shot_finish at SPENT -> DONE (the four
     *                   fields below are kept for the read line), so a finished continuous arm reads `cont` 0.
     *   cont_n          N, the frame budget (1..N48_CM_CONT_N_MAX), frozen in at the arm like a one-shot's `budget`.
     *   cont_t_us       T, in MICROSECONDS (the verb's units are 10 ms, or 100 ms with switch 41's bit 28 - 0.0.473;
     *                   at most N48_CM_CONT_T_MAX_US either way), frozen in at the arm.
     *   cont_start_us   0 until the FIRST PLANE COMMIT (gfxsrc_commit_try sets it, once, from `gXdBuild.plane`); T is
     *                   measured from this instant, never from `armed_at_us` ('s measured correction).
     *   stop_why        N48_CM_STOP_* — 0 while ARMED; written once, at the SAME instant the state moves to SPENT,
     *                   by whichever of n48_cm_shot_spend (N reached) or n48_cm_shot_stop (every other cause) fired. */
    uint32_t cont;
    uint32_t cont_n;
    uint64_t cont_t_us;
    uint64_t cont_start_us;
    uint32_t stop_why;
    /* build 0.0.543 item D (switch 102): 1 only on a continuous arm set through n48_cm_shot_arm_cont_ext_us; it selects the
     * ceiling n48_cm_shot_budget_of clamps `cont_n` against (N48_CM_CONT_N_MAX_EXT instead of N48_CM_CONT_N_MAX). Every other arm
     * writes 0, so a switch-41 arm and a one-shot read exactly as 0.0.542's. */
    uint32_t cont_ext;
} n48_cm_shot;

/* The budget as the shot will actually apply it: for a ONE-SHOT (`cont` 0), 0 and anything past N48_CM_SHOT_BUDGET_MAX
 * read as 1, which is 0.0.380 unchanged. For a CONTINUOUS shot (`cont` 1), `cont_n` is read instead — 0 or anything
 * past N48_CM_CONT_N_MAX also reads as 1, the same fail-safe direction, though the verb's own two guards (notes
 *'s pattern) mean a live continuous shot's `cont_n` is never actually out of range. `budget` is UNUSED while
 * `cont` is set (n48_cm_shot_arm_cont never touches it), so the two fields can never be read against each other.
 *
 * F1 (0.0.461 review) — PRE-PLANE, THE CEILING IS 4, NOT cont_n. The user's own decision (C5-CONTINUOUS.md Q2):
 * "until then [the first plane commit] the arm behaves as today's budget-4 one-shot; N counts every spend." Through
 * 0.0.460 this read `cont_n` from the instant of the arm, so up to N (then 600; 1022 since) non-plane frames could commit with no
 * deadline at all before T ever started (the review's own measurement: arms 29/31 committed no plane at all under
 * today's policy-divisor cadence). `cont_start_us == 0` is exactly "T has not started" (n48_cm_shot_cont_start's own
 * sentinel), so that is the one bit this function needs: pre-plane, N48_CM_SHOT_BUDGET_MAX (4); once a plane has
 * committed, `cont_n`. `spent` is a SINGLE counter throughout (nothing here resets it when the ceiling changes), so
 * the pre-plane spends are still charged against N exactly as the design asks.
 *
 * M2 (0.0.462 review) — THE PRE-PLANE CEILING IS min(4, N), NOT A BARE 4. A continuous arm with N < 4 (a small,
 * deliberately tight N) must never let the pre-plane phase spend MORE than N total before a plane has even
 * committed - 0.0.461 handed out a flat 4 regardless of N, so N = 2 could pre-plane-spend 4, already double the
 * whole arm's own budget, before T ever started. `n` below is `cont_n` clamped exactly as the post-plane branch
 * clamps it (0 or over-ceiling reads as 1), so a malformed `cont_n` is never let through by this comparison. */
static inline uint32_t n48_cm_shot_budget_of(const n48_cm_shot *sh)
{
    if (!sh) return 1u;
    if (sh->cont) {
        const uint32_t nmax = sh->cont_ext ? N48_CM_CONT_N_MAX_EXT : N48_CM_CONT_N_MAX;   /* 0.0.543 item D: switch 102's arm only */
        const uint32_t n = (sh->cont_n == 0u || sh->cont_n > nmax) ? 1u : sh->cont_n;
        if (sh->cont_start_us == 0ull) return n < N48_CM_SHOT_BUDGET_MAX ? n : N48_CM_SHOT_BUDGET_MAX;
        return n;
    }
    if (sh->budget == 0u || sh->budget > N48_CM_SHOT_BUDGET_MAX) return 1u;
    return sh->budget;
}

/* Frames this arm may still spend. 0 once the budget is exhausted or the shot is not ARMED. For the log only. */
static inline uint32_t n48_cm_shot_left(const n48_cm_shot *sh)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    const uint32_t b = n48_cm_shot_budget_of(sh);
    return sh->spent >= b ? 0u : b - sh->spent;
}

/* 1 when the binding has not been replaced or lost since the arm. See the paragraph above for why a FIRST bind is fine. */
static inline uint32_t n48_cm_shot_ws_ok(const n48_cm_ws_mark *at_arm, const n48_cm_ws_mark *now)
{
    if (!at_arm || !now) return 0u;
    if (now->rebinds != at_arm->rebinds) return 0u;
    if (now->unbinds != at_arm->unbinds) return 0u;
    if (now->gone != at_arm->gone) return 0u;
    if (now->binds < at_arm->binds || now->binds > at_arm->binds + 1u) return 0u;
    return 1u;
}

/* The arm level THIS frame may be judged at. Pure: it never mutates the one-shot, so a caller that forgets to act on a
 * cancellation still cannot get a second commit. OFF stays OFF; anything that is not a live one-shot at COMMIT is DECIDE. */
static inline uint32_t n48_cm_shot_level(const n48_cm_shot *sh, uint32_t arm, const n48_cm_ws_mark *now)
{
    if (arm == N48_SD_ARM_OFF) return N48_SD_ARM_OFF;
    if (arm != N48_SD_ARM_COMMIT) return arm;
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return N48_SD_ARM_DECIDE;
    if (!n48_cm_shot_ws_ok(&sh->ws, now)) return N48_SD_ARM_DECIDE;
    return N48_SD_ARM_COMMIT;
}

/* The budget is fixed AT the arm and nowhere else: a budget that could be raised under a standing arm would be a second
 * lever on the blast radius, reachable while frames are in flight. 0.0.380's callers passed no budget and got a one-shot;
 * `budget` 0 here preserves that exactly. */
static inline uint32_t n48_cm_shot_arm(n48_cm_shot *sh, const n48_cm_ws_mark *now, uint64_t at_us, uint32_t budget)
{
    if (!sh || !now) return 0u;
    sh->state = N48_CM_SHOT_ARMED;
    sh->spent_seq = 0u;
    sh->armed_at_us = at_us;
    sh->ws = *now;
    sh->budget = (budget == 0u || budget > N48_CM_SHOT_BUDGET_MAX) ? 1u : budget;
    sh->spent = 0u;
    sh->last_seq = 0u;
    /* C5 part 2 — A ONE-SHOT ARM IS NEVER CONTINUOUS. Zeroing these here (rather than trusting a caller to have
     * zero-initialised `*sh`) is what keeps a struct REUSED across arms from carrying a PRIOR continuous arm's N/T/
     * start/reason into a plain one-shot arm — the OFF identity depends on every one-shot arm reading `cont` 0. */
    sh->cont = 0u;
    sh->cont_n = 0u;
    sh->cont_t_us = 0ull;
    sh->cont_start_us = 0ull;
    sh->stop_why = N48_CM_STOP_NONE;
    sh->cont_ext = 0u;   /* build 0.0.543 item D */
    return 1u;
}

/* C5 part 2 (an internal design note Q2) — THE CONTINUOUS ARM. Mirrors n48_cm_shot_arm exactly (same
 * ARMED/ws/armed_at_us fields, same idempotence-key reset) except `budget` is left 0 (unused: n48_cm_shot_budget_of
 * reads `cont_n` while `cont` is set) and the five continuous fields above are filled instead. Refuses (0, no-op) on
 * an out-of-range N or T, exactly as the verb's own two guards intend — this is the header's OWN second guard,
 * never trusting that the caller already checked. `cont_start_us` starts at 0: T has not begun (the first plane
 * commit sets it, once), and until then this shot behaves exactly like a one-shot with a budget of `n` — the SAME
 * n48_cm_shot_spend counts every accepted frame against `cont_n` via n48_cm_shot_budget_of, whatever T is doing. */
/* C3 (0.0.461 review) — THE CONTINUOUS ARM'S OWN REFUSAL CHECKLIST, AS A PURE FUNCTION. Through 0.0.460 this was
 * six `if`s inline in the verb (AppleHardwareHook.cpp's `hw_hook_gfx_neuter`, switch 4) - exactly the shape
 * the project's own rule warns about ("a condition that lives in an `if` inside a 19,000-line file is reviewed
 * once"), and the review's own planted break (C3: deleting the switch-22 line) went uncaught because nothing
 * outside that one `if` could prove it was reachable. Moved here so the SAME function the kext calls is the one a
 * host test drives directly, with real booleans, in the real bit layout - a deleted `if` becomes a bit that never
 * sets, provably, rather than a line a reviewer has to notice is missing.
 * Bits (unchanged from 0.0.460's own numbering, so the verb's report line's bit-name table needs no edit):
 *   0x1  switch 22 (in-flight deferral) not ON     0x2  switch 16 (fence un-NOP) not ON
 *   0x4  switch 15 (X9-F forgiveness) ON            0x8  switch 19 (Exit C running budget) ON
 *   0x10 the frame cap has no room for N + margin   0x20 switch 35 (second window/plane) not ON (F2(a))
 *   0x40 a VM fault is ALREADY LATCHED at the arm (M5, 0.0.462 review) - see below
 * `capRoom` is the caller's OWN pre-computed boolean ("gXdFrameCap - gXdC.judged >= N + margin", including the
 * underflow guard) - this function does not read gXdFrameCap/gXdC.judged itself, so it stays free of every kext
 * global exactly like n48_cm_arm_missing above it.
 *
 * M5 (0.0.462 review) — `noFaultLatched`. 0.0.461's VM_FAULT stop only ever catches a fault that TRANSITIONS from
 * clear to latched AFTER the arm (the raw-status comparison n48_cm_shot_stop_if_rose's caller now does), by
 * design - it must never fire on a fault that already existed. The gap that fix left: a fault ALREADY latched at
 * arm time is then invisible for the REST of the arm too, since the hardware register is never cleared and the
 * transition can never re-fire. 0.0.461 called that "blinding the stop" and accepted it; 0.0.462 refuses the arm
 * instead (fail closed - the operator runs `accel faultclear` first, exactly as every other C16 vm-fault reader in
 * this file already expects), so a continuous run's OWN fault-detection window starts clean or does not start. */
static inline uint32_t n48_cm_cont_arm_missing(uint32_t deferOn, uint32_t fenceOn, uint32_t forgiveOn,
                                               uint32_t runBudgetOn, uint32_t capRoom, uint32_t planeWindowOn,
                                               uint32_t noFaultLatched)
{
    uint32_t m = 0u;
    if (!deferOn) m |= 0x1u;
    if (!fenceOn) m |= 0x2u;
    if (forgiveOn) m |= 0x4u;
    if (runBudgetOn) m |= 0x8u;
    if (!capRoom) m |= 0x10u;
    if (!planeWindowOn) m |= 0x20u;
    if (!noFaultLatched) m |= 0x40u;
    return m;
}

/* F5b (0.0.462 review) — FENCE-LATE, THE COMPARISON ITSELF, NOW PURE. 0.0.461 wrote `byPolls || byTime` inline in
 * the kext's ring poll loop, and the review's own planted break ("byPolls = false", i.e. only the time half ever
 * fires) went UNCAUGHT - nothing outside that one expression could prove the polls half was reachable on its own.
 * `polls`/`sinceUs` are the caller's OWN pre-computed readings (this entry's own poll count this arm, and
 * now - its own gate stamp); the function decides nothing about HOW they were gathered, only whether EITHER
 * threshold, alone, is enough. */
static inline uint32_t n48_cm_cont_fence_late(uint32_t polls, uint32_t pollThreshold, uint64_t sinceUs,
                                              uint64_t timeThresholdUs)
{
    if (polls >= pollThreshold) return 1u;
    if (sinceUs >= timeThresholdUs) return 1u;
    return 0u;
}

/* F6 (0.0.462 review) — THE CONTINUOUS STOP LINE'S THREE NUMBERS, NOW PURE. Replaces the inline arithmetic
 * xd_shot_finish carried (0.0.461's own fix for the THREE bugs this same block once had): "since the first plane
 * commit" and "last commit before this stop" are both measured from `stopNowUs` (the STOP's own instant, read
 * fresh by the caller - never a stale spend time); "spent" is `sh->spent` (n48_cm_shot_spend's own true count),
 * never a caller-side tracking counter that can under-count on the RING-FULL-at-spend edge case. A `stopNowUs`
 * or `lastCommitUs` before its counterpart (a torn or out-of-order clock read) answers 0, the same conservative
 * direction every other clock comparison in this header takes. */
static inline void n48_cm_stop_report(const n48_cm_shot *sh, uint64_t stopNowUs, uint64_t lastCommitUs,
                                      uint64_t *outSinceStart, uint64_t *outLastBeforeStop, uint32_t *outSpent)
{
    if (outSinceStart) *outSinceStart = (sh && sh->cont_start_us && stopNowUs >= sh->cont_start_us) ?
        stopNowUs - sh->cont_start_us : 0ull;
    if (outLastBeforeStop) *outLastBeforeStop = (lastCommitUs && stopNowUs >= lastCommitUs) ?
        stopNowUs - lastCommitUs : 0ull;
    if (outSpent) *outSpent = sh ? sh->spent : 0u;
}

/* F7 (0.0.462 review) — THE SUMMARY'S TIME-TRIGGER BASELINE, NOW PURE. Before the FIRST periodic summary has ever
 * printed (`lastSummaryUs` still 0), the baseline is T's own origin (`contStartUs`) - "first summary after 1 s
 * from the start" (the review's own words) - rather than requiring a summary to have already printed once before
 * the time trigger can ever fire (0.0.460's bug: on a short run, kXdContLogSummaryEvery (32) commits never
 * happens, so the count trigger never bootstraps the time one either, and no summary EVER prints). */
static inline uint64_t n48_cm_cont_summary_baseline(uint64_t lastSummaryUs, uint64_t contStartUs)
{
    return lastSummaryUs ? lastSummaryUs : contStartUs;
}

/* build 0.0.473 item 2 — THE SUMMARY IS DUE: every `everyCommits` commits (0 disables the count trigger) or
 * once `periodUs` has passed since the baseline above, whichever comes first. The SAME predicate the per-commit
 * trigger in gfxsrc_commit_try always had (commits, or 1 s from the baseline), now also asked at the top of every
 * judged frame while the arm stands, with the count trigger off, so a 60-s window with few or no commits still
 * prints a summary about once a second rather than only when a commit lands. A `nowUs` of 0, a baseline of 0 (T not
 * started) or a `nowUs` before the baseline (a torn clock) is never "due" by time. */
static inline uint32_t n48_cm_cont_summary_due(uint32_t commits, uint32_t lastSummaryCommit, uint32_t everyCommits,
                                               uint64_t nowUs, uint64_t lastSummaryUs, uint64_t contStartUs,
                                               uint64_t periodUs)
{
    if (everyCommits && commits - lastSummaryCommit >= everyCommits) return 1u;
    const uint64_t base = n48_cm_cont_summary_baseline(lastSummaryUs, contStartUs);
    if (!nowUs || !base || nowUs < base) return 0u;
    return (nowUs - base >= periodUs) ? 1u : 0u;
}

/* build 0.0.473 item 2 — THE PER-ARM LINE CAPS, ONE FUNCTION. A 60-s window at up to N48_CM_CONT_N_MAX commits
 * must not flood n48log or slow Apple's submit thread, so every line that prints once per commit (or once per
 * judged frame) while a continuous arm stands is printed in full only up to its cap, then COUNTED (`*suppressed`,
 * printed on the CONTINUOUS SUMMARY line). `cont` 0 (a one-shot, DECIDE, or a continuous shot already DONE - item 3
 * clears `cont` there) prints everything, exactly as 0.0.472 did: the OFF identity. Answers 1 when the line prints.
 *   N48_CM_CONT_LOG_FULL_COMMITS  the routine per-commit lines (SPENT, fence828 GATE OK / COMMITTED, the keystone's
 *                                 verdict/NO-OP/walk lines, TRANSLATED, RING EXEMPTION SPARED): the first K commits
 *   N48_CM_CONT_LOG_POLL_LINES    fence828 POLL (one per judged frame while a committed fence is watched)
 *   N48_CM_CONT_LOG_APPLY_LINES   kstone-defer APPLYING (one per applied deferral, ~one per flight)
 *   N48_CM_CONT_LOG_REFUSAL_LINES refusal and withdrawal lines past the full commits (keystone/guard/token/exemption
 *                                 refusals, REWRITE REFUSED, NOT_RUN expiries, unmap withdrawal/re-arm refusals),
 *                                 one shared budget per arm; their counts stay on every `gfxneuter` read. */
#define N48_CM_CONT_LOG_FULL_COMMITS 8u
#define N48_CM_CONT_LOG_POLL_LINES 64u
#define N48_CM_CONT_LOG_APPLY_LINES 8u
#define N48_CM_CONT_LOG_REFUSAL_LINES 64u
static inline uint32_t n48_cm_cont_log_take(uint32_t cont, uint32_t *printed, uint32_t cap, uint64_t *suppressed)
{
    if (!cont) return 1u;
    if (printed && *printed < cap) { (*printed)++; return 1u; }
    if (suppressed) (*suppressed)++;
    return 0u;
}
/* A ROUTINE per-commit line (the list under N48_CM_CONT_LOG_FULL_COMMITS above): prints when `cont` is 0 or THIS
 * commit is one of the arm's first K - `commit_full`, which the spend takes ONCE per commit from n48_cm_cont_log_take
 * against N48_CM_CONT_LOG_FULL_COMMITS, so every line of one commit is printed or suppressed together - and is
 * otherwise counted in `*suppressed`. Answers 1 when the line prints. */
static inline uint32_t n48_cm_cont_log_commit(uint32_t cont, uint32_t commit_full, uint64_t *suppressed)
{
    if (!cont || commit_full) return 1u;
    if (suppressed) (*suppressed)++;
    return 0u;
}

/* build 0.0.473 item 1 — THE ARM IN MICROSECONDS. The kext arms with the T the verb decoded (n48_cm_cont_t_us,
 * either unit); this re-checks it against N48_CM_CONT_T_MAX_US itself (the header's own second guard) and refuses 0.
 * n48_cm_shot_arm_cont below (10-ms units, 0.0.472's signature) is now exactly this with n48_cm_cont_t_us(t, 0). */
static inline uint32_t n48_cm_shot_arm_cont_us(n48_cm_shot *sh, const n48_cm_ws_mark *now, uint64_t at_us,
                                               uint32_t n, uint64_t t_us)
{
    if (!sh || !now) return 0u;
    if (n == 0u || n > N48_CM_CONT_N_MAX) return 0u;
    if (t_us == 0ull || t_us > N48_CM_CONT_T_MAX_US) return 0u;
    sh->state = N48_CM_SHOT_ARMED;
    sh->spent_seq = 0u;
    sh->armed_at_us = at_us;
    sh->ws = *now;
    sh->budget = 0u;
    sh->spent = 0u;
    sh->last_seq = 0u;
    sh->cont = 1u;
    sh->cont_n = n;
    sh->cont_t_us = t_us;
    sh->cont_start_us = 0ull;
    sh->stop_why = N48_CM_STOP_NONE;
    sh->cont_ext = 0u;   /* build 0.0.543 item D: a switch-41 arm keeps switch 41's ceilings */
    return 1u;
}

/* build 0.0.543 item D - THE LONGER ARM (switch 102). Refuses (0, the shot untouched) unless N is 1..N48_CM_CONT_N_MAX_EXT and
 * T is 1 us..N48_CM_CONT_T_MAX_US_EXT: the header's own second guard, never trusting the verb. Otherwise exactly
 * n48_cm_shot_arm_cont_us's arm (which it calls with an in-range N, so that guard passes too - see the clamp below) plus
 * `cont_ext` 1, which is what lets n48_cm_shot_budget_of honour an N above N48_CM_CONT_N_MAX. */
static inline uint32_t n48_cm_shot_arm_cont_ext_us(n48_cm_shot *sh, const n48_cm_ws_mark *now, uint64_t at_us,
                                                   uint32_t n, uint64_t t_us)
{
    if (!sh || !now) return 0u;
    if (n == 0u || n > N48_CM_CONT_N_MAX_EXT) return 0u;
    if (t_us == 0ull || t_us > N48_CM_CONT_T_MAX_US_EXT) return 0u;
    const uint64_t t_in = t_us > N48_CM_CONT_T_MAX_US ? N48_CM_CONT_T_MAX_US : t_us;
    if (!n48_cm_shot_arm_cont_us(sh, now, at_us, n > N48_CM_CONT_N_MAX ? N48_CM_CONT_N_MAX : n, t_in)) return 0u;
    sh->cont_n = n;
    sh->cont_t_us = t_us;
    sh->cont_ext = 1u;
    return 1u;
}

static inline uint32_t n48_cm_shot_arm_cont(n48_cm_shot *sh, const n48_cm_ws_mark *now, uint64_t at_us,
                                            uint32_t n, uint32_t t_10ms_units)
{
    /* 0.0.473: the 10-ms path, unchanged in what it accepts and what it freezes (n48_cm_cont_t_us(t, 0) is 0 for
     * t == 0 or t > N48_CM_CONT_T_MAX, else t * 10000), now through the one microsecond arm above. */
    if (!sh || !now) return 0u;
    if (n == 0u || n > N48_CM_CONT_N_MAX) return 0u;
    if (t_10ms_units == 0u || t_10ms_units > N48_CM_CONT_T_MAX) return 0u;
    return n48_cm_shot_arm_cont_us(sh, now, at_us, n, n48_cm_cont_t_us(t_10ms_units, 0u));
}

/* Charge one frame against the budget, and move ARMED -> SPENT only when the budget is exhausted. Answers 1 exactly once
 * PER DISTINCT TOKEN SEQ, so "the gate said OK twice for one frame" cannot charge twice; at budget 1 this is 0.0.380's
 * behaviour on every path, because the first accepted spend exhausts it and the state moves on that same call. */
static inline uint32_t n48_cm_shot_spend(n48_cm_shot *sh, uint32_t seq)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    if (sh->spent != 0u && seq == sh->last_seq) return 0u;   /* the SAME frame again: idempotent, and not charged */
    sh->last_seq = seq;
    sh->spent++;
    if (sh->spent >= n48_cm_shot_budget_of(sh)) {
        sh->state = N48_CM_SHOT_SPENT;
        sh->spent_seq = seq;
        /* C5 part 2 — N REACHED is recorded HERE, the SAME transition a one-shot's budget exhaustion already makes,
         * for a continuous shot only (`cont`): a one-shot never reads `stop_why` and this write is otherwise inert. */
        if (sh->cont) sh->stop_why = N48_CM_STOP_N;
    }
    return 1u;
}

/* C5 part 2 (an internal design note Q2, item 3 "THE STOPS") — EVERY STOP THAT IS NOT "N REACHED". Moves
 * ARMED -> SPENT, NEVER TO OFF — an in-flight frame keeps its ring-walk exemption exactly as a budget-exhausted
 * one-shot's does, because n48_cm_shot_level hands out COMMIT only in ARMED and SPENT already stops the next frame;
 * xd_shot_finish (the kext's own SPENT -> DONE, unchanged by this build) is what then writes DECIDE, at the next
 * point Apple's own call returns. Idempotent: answers 1 exactly once, only from ARMED, so a second stop call (two
 * conditions firing on the same judged frame) is a no-op and the FIRST cause recorded is the one that stands —
 * `stop_why` is written once and never overwritten by a later call.
 * `spent_seq` is deliberately left 0 (not touched): a stop that is not a spend was not exhausted BY any one frame's
 * token seq, and 0 already means "no seq" wherever spent_seq is read (n48_cm_shot_arm's own initial value). */
static inline uint32_t n48_cm_shot_stop(n48_cm_shot *sh, uint32_t why)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    sh->state = N48_CM_SHOT_SPENT;
    sh->stop_why = why;
    return 1u;
}

/* C5 part 2 — THE FIRST PLANE COMMIT STARTS T (an internal design note Q2: "T starts at the first plane
 * commit", the reviewer's own correction in  over "from armed_at_us", which arm35/arm32 measured as
 * ~10.2-10.4 s too early). Answers 1 the ONE time it actually sets `cont_start_us`, so the caller can print a
 * "continuous start" line exactly once; every other call — cont off, T already started, or this frame was not
 * plane-shaped (`is_plane` 0) — is a no-op. Idempotent by construction: `cont_start_us != 0` guards every call
 * after the first, so a plane committing twice can never move T's origin a second time. */
static inline uint32_t n48_cm_shot_cont_start(n48_cm_shot *sh, uint64_t at_us, uint32_t is_plane)
{
    if (!sh || !sh->cont || sh->cont_start_us != 0ull || !is_plane) return 0u;
    /* NEVER 0: 0 is the sentinel this field's own callers (n48_cm_shot_stop_if_t) read as "T has not started",
     * exactly the reason gfx_flightring.h's n48_fr_push coerces its own `at_us` away from 0 ("never 0: 0 on a live
     * entry is TORN"). A caller that hands in a genuine 0 (an unread clock, or a boot at uptime 0) still starts T
     * — one microsecond early is a harmless, conservative direction; treating a real start as "not started" is not. */
    sh->cont_start_us = at_us ? at_us : 1ull;
    return 1u;
}

/* C5 part 2 — THE JUDGED-FRAME-TOP COMPARISON, STATED ONCE. Every stop below N and T (a withdrawal inside a
 * flight, the two keystone-defer hazards, a latched VM fault, a fence gone late, a fence read corrupted, an
 * out-of-order retirement) is the SAME shape: some monotone counter, read again at this judged frame's top, is
 * higher than the snapshot taken AT THE ARM. Stating the comparison here, once, is what keeps the six call sites
 * in the kext from being six independent copies of `if (a > b)` — a stray `>=`, `<`, or a swapped (snapshot, now)
 * pair at any ONE of them is a mutation this file's own test catches, instead of a mutation six review passes
 * would each have to catch independently. STRICTLY RISING (`now > snapshot`), never merely different: a counter
 * that wrapped, or was sampled out of order across two reads, must never be misread as a rise. Only fires from
 * ARMED (n48_cm_shot_stop's own guard) and only for a CONTINUOUS shot — a one-shot never calls this. */
static inline uint32_t n48_cm_shot_stop_if_rose(n48_cm_shot *sh, uint64_t snapshot, uint64_t now, uint32_t why)
{
    if (!sh || !sh->cont) return 0u;
    if (now <= snapshot) return 0u;
    return n48_cm_shot_stop(sh, why);
}

/* C5 part 2 — THE T CHECK ITSELF, for the same one-comparison reason. Fires only once T has STARTED
 * (`cont_start_us != 0`, the first plane commit — see n48_cm_shot_cont_start above) AND has since ELAPSED
 * (`now_us - cont_start_us >= cont_t_us`). Before the first plane commit this always answers 0, whatever `now_us`
 * reads — the design's own "until then the arm behaves as today's budget-4 one-shot" (only N binds). A `now_us`
 * that reads BEFORE `cont_start_us` (a torn or out-of-order clock read) is not evidence of elapsed time and does
 * not stop — the same conservative direction n48_ksd_eval and n48_fr_defer_verdict already take for a bad clock. */
static inline uint32_t n48_cm_shot_stop_if_t(n48_cm_shot *sh, uint64_t now_us)
{
    if (!sh || !sh->cont || sh->cont_start_us == 0ull) return 0u;
    if (now_us < sh->cont_start_us) return 0u;
    if (now_us - sh->cont_start_us < sh->cont_t_us) return 0u;
    return n48_cm_shot_stop(sh, N48_CM_STOP_T);
}

/* F2 (0.0.461 review) — THE HARD PRE-PLANE BOUND. The mirror image of n48_cm_shot_stop_if_t: fires only while T
 * has NOT yet started (`cont_start_us == 0` — before the first plane commit) and `armed_at_us` is far enough in
 * the past. Once a plane commits this is permanently inert for the rest of the arm (n48_cm_shot_stop_if_t takes
 * over), so the two can never both fire for the same arm. Same conservative clock handling as every other stop_if
 * here: a `now_us` before `armed_at_us` is not evidence of elapsed time. */
static inline uint32_t n48_cm_shot_stop_if_preplane(n48_cm_shot *sh, uint64_t now_us)
{
    if (!sh || !sh->cont || sh->cont_start_us != 0ull) return 0u;
    if (now_us < sh->armed_at_us) return 0u;
    if (now_us - sh->armed_at_us < N48_CM_PREPLANE_BOUND_US) return 0u;
    return n48_cm_shot_stop(sh, N48_CM_STOP_PREPLANE_TIMEOUT);
}

/* SPENT -> DONE. Answers 1 exactly once; the caller writes DECIDE into the arm level on that 1 and logs it.
 *
 * build 0.0.473 item 3 (the 0.0.472 review's LOW) — `cont` IS CLEARED HERE, AT DONE. Through 0.0.472 a finished
 * continuous shot kept `cont` 1 until the next arm or disarm, so every later frame's gate input `c.cont_on` (read from
 * `gXdShot.cont`) still said "continuous", and every later rehearsal (whose probe is DECIDE-level, so no fence is ever
 * placed for it) was named CONTINUOUS-NO-FENCE instead of its real rung. The REFUSAL-SAFE DIRECTION: this runs only
 * on SPENT -> DONE, after the arm has already left ARMED (n48_cm_shot_spend / n48_cm_shot_stop), and nothing that
 * can hand out COMMIT reads `cont` - n48_cm_shot_level answers DECIDE for every state but ARMED, n48_cm_shot_spend
 * and n48_cm_shot_stop refuse every state but ARMED - so clearing it can only REMOVE a refusal (CONT_NO_FENCE) from
 * frames that are already DECIDE-level, never re-enable a commit. `cont_n`, `cont_t_us`, `cont_start_us` and
 * `stop_why` are kept (the verb's read line and the STOP line still name the arm that just ended); the next arm or
 * disarm overwrites them. A one-shot's `cont` is already 0, so for it this is a store of the same value. The caller
 * (xd_shot_finish) reads everything it needs about the continuous arm BEFORE this call. */
static inline uint32_t n48_cm_shot_finish(n48_cm_shot *sh)
{
    if (!sh || sh->state != N48_CM_SHOT_SPENT) return 0u;
    sh->state = N48_CM_SHOT_DONE;
    sh->cont = 0u;
    return 1u;
}

/* ARMED -> CANCELLED. Only from ARMED: a SPENT one-shot's frame is already in flight and its exemption still needs the
 * level, and a DONE one is over. Answers 1 exactly once. */
static inline uint32_t n48_cm_shot_cancel(n48_cm_shot *sh)
{
    if (!sh || sh->state != N48_CM_SHOT_ARMED) return 0u;
    sh->state = N48_CM_SHOT_CANCELLED;
    return 1u;
}

/* =====================================================================================================================
 * build 0.0.463 item 2 — THE MID-ARM SWITCH GUARD FOR SWITCHES 43, 44, 45, 46, 47, 48, 49, 50, 52, 53.
 *
 * WHY THIS IS ONE FUNCTION AND NOT TEN `if`s. Switches 22, 16, 19, 35, 37 and 38 each carry their OWN inline
 * `gXdShot.cont && gXdShot.state == N48_CM_SHOT_ARMED` guard, written once per verb handler in AppleHardwareHook.cpp
 * — exactly the shape the project's own rule warns about ("a condition that lives in an `if` inside a 27,000-line
 * file is reviewed once"). None of those ten policy-pass switches (43-50, 52, 53) is part of the arm's OWN
 * checklist (n48_cm_cont_arm_missing), so none of them needs to be ON or OFF for the arm to stand; what they share
 * is that EVERY one of them reshapes what the policy pass or the commit gate does to a frame, and a continuous
 * run's own measurements must not see that change happen under them — the same reasoning switch 37's own comment
 * (0.0.461 review, "SMALLER") already gives for itself. Putting the LIST and the PREDICATE in one pure function
 * means a verb handler that forgets to call it is a missing call a source pin can name, rather than a six-line `if`
 * a reviewer has to notice is absent from one handler out of ten.
 *
 * WHAT IS DELIBERATELY NOT HERE: the list does not include switch 41 (continuous mode itself — arming IS the
 * lever), or any switch already guarded elsewhere (3, 15, 16, 19, 22, 35, 37, 38) — this function's OWN list is
 * exactly the ten this build task names, checked by `n48_cm_cont_switch_guarded` below, and no other switch is
 * ever refused by it.
 *
 * build 0.0.471 item 2 adds an eleventh: switch 51 (0.0.470's no-sampler/class-10 table-shape lever) joins
 * this same list and calls this same function, for the same reason switch 52 or 53 does — it reshapes what a
 * draw's table step does without being part of the continuous arm's own checklist.
 *
 * build 0.0.472 item 2 adds a twelfth: switch 54 (0.0.457's colour-target scan over every item and every
 * draw) joins this same list for the same reason — it reshapes what the policy pass's target rung sees without
 * being part of the continuous arm's own checklist.
 *
 * build 0.0.480 adds a thirteenth: switch 55 (continuation units) joins for the same reason - it reshapes what the
 * policy pass translates (a unit instead of its constituents) without being part of the continuous arm's own checklist.
 * build 0.0.481 adds a fourteenth: switch 56 (a single segment refused for room retried through the unit path) joins
 * for the same reason - it reshapes what a single segment translates to.
 * build 0.0.485 adds a fifteenth: switch 58 (per-draw colour-target pages and the re-shape REPLACE in the producer
 * ledger, gfx_desc_port.h) joins for the same reason - it reshapes what the ledger can prove to a later frame's provenance
 * ask.
 * build 0.0.487 adds a sixteenth: switch 57 (the compute-N elide, an internal design note) joins for the same reason
 * - it reshapes what a segment carrying Apple's compute clear translates to, and which frames the gate may commit.
 * build 0.0.488 adds a seventeenth: switch 60 (the DCC T# strip, an internal design note) joins for the same reason - it
 * reshapes which table records a draw's descriptor step translates and what it asks the ledger to prove.
 * build 0.0.486 adds switch 59 (the backing-sourced static textures, an internal design note) for the same reason -
 * it reshapes which residency entries a draw's T# ask can be answered by, and what the placed T# says.
 * MERGE 0.0.489: the union of every build's entry (0.0.485's 58, 0.0.487's 57, 0.0.488's 60, 0.0.486's 59).
 * build 0.0.490 adds switch 42 (R1, the memory-destination rung): its mode decides whether md_ok gates
 * `live` and the gate's MEMDST rung (ENFORCE) or is only counted (SHADOW/OFF), so a flip under a continuous arm would change
 * which frames commit mid-measurement. The first switch below 43 in this list; a bare `42` read stays allowed.
 * build 0.0.494 adds switch 61 (the D4' image-union fold and the overflow attribution): it reshapes what
 * the D4' union holds and which count its overflow refuses with, so a flip under a continuous arm would change which
 * frames' dep rung refuses mid-measurement (PIN SWITCH-GUARD:61).
 * build 0.0.495 adds switch 62 (the shader-heap copy / substitution race): it changes which frames the commit
 * and the ring exemption refuse and what the residency copy writes, so a flip under a continuous arm would change which frames
 * commit mid-measurement (PIN SWITCH-GUARD:62).
 * build 0.0.496 adds switch 63 (the residency copy through SDMA): it changes how every residency copy reaches
 * VRAM (and binds the staging buffer and runs the positive control at its first ON), so it is changed only before an arm
 * (PIN SWITCH-GUARD:63).
 * build 0.0.497 adds switch 64 (the keystone's bounded wait and the fill-member retirement at a keystone
 * withdrawal): it changes which frames the keystone lets run and when the fill window closes, so it is changed only before an
 * arm (PIN SWITCH-GUARD:64).
 * build 0.0.498 adds switch 65 (end of pipe asked at the deferral's expiry): it changes whether an
 * expired deferral withdraws or retires a finished flight, so it is changed only before an arm (PIN SWITCH-GUARD:65).
 * build 0.0.500 (an internal design note Q4) adds switch 66 (the draw elide): it changes which draws a candidate
 * carries, so it is changed only before an arm (PIN SWITCH-GUARD:66).
 *
 *   switch_no  the verb's own selector. Only 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
 *              61, 62, 63, 64, 65 and 66 are ever refused here.
 *   is_read    1 when this call carries no attempted ON/OFF change (M 0, a bare read) — a read is ALWAYS allowed,
 *              at any arm state, matching every other mid-arm guard's own "read stays allowed" identity.
 *   cont       gXdShot.cont, the caller's OWN reading — never recomputed here.
 *   state      gXdShot.state, the caller's OWN reading — never recomputed here.
 * Refuses (1) only when NOT a read, the shot IS continuous, and the shot is standing ARMED — the exact predicate
 * `gfxneuter 3`'s own C5-part-2 guard uses (gfx_commit_test.cpp's PIN C5). A one-shot arm (`cont` 0) and a
 * SPENT/DONE/CANCELLED/OFF continuous shot are both unaffected (OFF identity: this answers 0 for every state but
 * ARMED, and 0 for every `cont` but 1). */
enum {
    N48_CM_CONT_SWITCH_43 = 43u, N48_CM_CONT_SWITCH_44 = 44u, N48_CM_CONT_SWITCH_45 = 45u,
    N48_CM_CONT_SWITCH_46 = 46u, N48_CM_CONT_SWITCH_47 = 47u, N48_CM_CONT_SWITCH_48 = 48u,
    N48_CM_CONT_SWITCH_49 = 49u, N48_CM_CONT_SWITCH_50 = 50u, N48_CM_CONT_SWITCH_51 = 51u,
    N48_CM_CONT_SWITCH_52 = 52u, N48_CM_CONT_SWITCH_53 = 53u, N48_CM_CONT_SWITCH_54 = 54u,
    N48_CM_CONT_SWITCH_55 = 55u, N48_CM_CONT_SWITCH_56 = 56u, N48_CM_CONT_SWITCH_57 = 57u, N48_CM_CONT_SWITCH_58 = 58u,
    N48_CM_CONT_SWITCH_59 = 59u,  /* build 0.0.486 */
    N48_CM_CONT_SWITCH_60 = 60u,  /* build 0.0.488 */
    N48_CM_CONT_SWITCH_42 = 42u,  /* build 0.0.490 */
    N48_CM_CONT_SWITCH_61 = 61u,  /* build 0.0.494 */
    N48_CM_CONT_SWITCH_62 = 62u,  /* build 0.0.495 */
    N48_CM_CONT_SWITCH_63 = 63u,  /* build 0.0.496 */
    N48_CM_CONT_SWITCH_64 = 64u,  /* build 0.0.497 */
    N48_CM_CONT_SWITCH_65 = 65u,  /* build 0.0.498 */
    N48_CM_CONT_SWITCH_66 = 66u,  /* build 0.0.500 */
    N48_CM_CONT_SWITCH_68 = 68u,  /* build 0.0.503 */
    N48_CM_CONT_SWITCH_69 = 69u   /* build 0.0.505 */
};

static inline uint32_t n48_cm_cont_switch_guarded(uint32_t switch_no)
{
    switch (switch_no) {
    case 43u: case 44u: case 45u: case 46u: case 47u:
    case 48u: case 49u: case 50u: case 51u: case 52u: case 53u: case 54u: case 55u: case 56u:
    case 57u:   /* build 0.0.487 */
    case 58u:   /* build 0.0.485 */
    case 59u:   /* build 0.0.486 */
    case 60u:   /* build 0.0.488 */
    case 42u:   /* build 0.0.490: R1's mode decides md_ok, `live` and the gate's MEMDST rung */
    case 61u:   /* build 0.0.494: the D4' image-union fold + overflow attribution (PIN SWITCH-GUARD:61) */
    case 62u:   /* build 0.0.495: the shader-heap copy / substitution race (PIN SWITCH-GUARD:62) */
    case 63u:   /* build 0.0.496: the residency copy through SDMA (PIN SWITCH-GUARD:63) */
    case 64u:   /* build 0.0.497: the keystone's bounded wait + fill-member retirement (PIN SWITCH-GUARD:64) */
    case 65u:   /* build 0.0.498: end of pipe asked at the deferral's expiry (PIN SWITCH-GUARD:65) */
    case 66u:   /* build 0.0.500 (an internal design note Q4): the draw elide for U/Y (and AO) (PIN SWITCH-GUARD:66) */
    case 67u:   /* build 0.0.501 (an internal design note Q3): PACK, deferred records one NOP per free run (PIN SWITCH-GUARD:67) */
    case 68u:   /* build 0.0.503 (an internal design note H1): the hybrid newUserClient refusal (PIN SWITCH-GUARD:68) */
    case 69u:   /* build 0.0.505 (an internal design note C1): the IB-0 disguise and the lead segments (PIN SWITCH-GUARD:69) */
    case 70u:   /* build 0.0.506 ( (1), gfx_unitdefer.h): the deferred room retry (PIN SWITCH-GUARD:70) */
    case 71u:   /* build 0.0.508 (, gfx_fence828.h): the last-candidate fence rule (PIN SWITCH-GUARD:71) */
    case 72u:   /* build 0.0.510 ( (b), gfx_heapgen.h): the copy waits for a committed frame's walk (PIN SWITCH-GUARD:72) */
    case 73u:   /* build 0.0.516 ( (a), gfx_present73.h): hold a present whose plane's last P did not commit (PIN SWITCH-GUARD:73) */
    case 74u:   /* build 0.0.518 (, gfx_flipmode.h): flip mode, double-buffered VUPDATE flips (PIN SWITCH-GUARD:74) */
    case 75u:   /* build 0.0.519 (, gfx_walknop75.h): a walk-NOPed flight retires at once (PIN SWITCH-GUARD:75) */
    case 76u:   /* build 0.0.522 (, gfx_spill.h): the kext spill tier for descriptor records (PIN SWITCH-GUARD:76) */
    case 77u:   /* build 0.0.523 (, gfx_rnforgive.h): the ring-neuter forgiveness (PIN SWITCH-GUARD:77) */
    case 78u:   /* build 0.0.523 (, gfx_copyguard.h): the copy-guard redo (PIN SWITCH-GUARD:78) */
    case 79u:   /* build 0.0.524 (an internal design note, gfx_t0src.h): S's texture-0 source, read-only (PIN SWITCH-GUARD:79) */
    case 80u:   /* build 0.0.525 (an internal design note, gfx_cycle80.h): cycle completeness, ON / SHADOW (PIN SWITCH-GUARD:80) */
    case 81u:   /* build 0.0.526: flip kept through a keystone withdrawal, M3, M4 (PIN SWITCH-GUARD:81) */
    case 82u:   /* build 0.0.527 (an internal design note, gfx_sk82.h): the byte-identical re-copy skip, MEASURE / SKIP (PIN SWITCH-GUARD:82) */
    case 83u:   /* build 0.0.528 (, gfx_tlb83.h): the TLB ack spin poll and its leaf lock (PIN SWITCH-GUARD:83) */
    case 84u:   /* build 0.0.529 (an internal design note, gfx_cg84.h): the granule copy guard + delta write (PIN SWITCH-GUARD:84) */
    case 85u:   /* build 0.0.530 (an internal design note, gfx_fs85.h): source fills, S1 twin / S2 / S3 (PIN SWITCH-GUARD:85) */
    case 86u:   /* build 0.0.531 ( lever 1, gfx_p86.h): the present-time retirement re-check (PIN SWITCH-GUARD:86) */
    case 87u:   /* build 0.0.531 item 4b: the text-element T# log, read-only (PIN SWITCH-GUARD:87) */
    case 88u:   /* build 0.0.533 (, an internal design note, gfx_heapgen.h): the range-precise heap-gen judge (PIN SWITCH-GUARD:88) */
    case 89u:   /* build 0.0.534 ( item 1, fastcopy89.h): the fast copy's full verify by an SDMA read-back (PIN SWITCH-GUARD:89) */
    case 90u:   /* build 0.0.534 ( item 2, gfx_ks90.h): the expiry check defers on a busy gXdLock (PIN SWITCH-GUARD:90) */
    case 91u:   /* build 0.0.535 ( fix 1, xlat12_ib.h XLAT12_EXTRA_NCLEAR): N's zero fill (PIN SWITCH-GUARD:91) */
    case 92u:   /* build 0.0.536 (, xlat12_ib.h XLAT12_EXTRA_RECT2D, gfx_rv92.h): RECTLIST clears (PIN SWITCH-GUARD:92) */
    case 93u:   /* build 0.0.537 (, xlat12_ib.h XLAT12_EXTRA_PWS): Apple's CB/DB barrier waits on gfx12 (PIN SWITCH-GUARD:93) */
    case 94u:   /* build 0.0.538 ( PLAN item 1, gfx_p94.h): present-time promotion without gXdLock (PIN SWITCH-GUARD:94) */
    case 95u:   /* build 0.0.538 ( PLAN item 2, gfx_p95.h): replay a held present (PIN SWITCH-GUARD:95) */
    case 96u:   /* build 0.0.540 (, gfx_perf540.h): perf540, the log-only hook/judge/present timers (PIN SWITCH-GUARD:96) */
    case 97u:   /* build 0.0.540 item 5 (xlat12_ib.h XLAT12_EXTRA_TBLCACHE): the verifier's table cache (PIN SWITCH-GUARD:97) */
    case 98u:   /* build 0.0.541: the provenance-ask walk cache, ON / SHADOW (PIN SWITCH-GUARD:98) */
    case 99u:   /* build 0.0.541 item 6: the witness-overflow relaxation, ON / SHADOW (PIN SWITCH-GUARD:99) */
    case 100u:  /* build 0.0.543 item A ( A, gfx_hm100.h): the per-call host-page map cache, ON / SHADOW (PIN SWITCH-GUARD:100) */
    case 101u:  /* build 0.0.543 item B ( B step 1, gfx_wc98.h): the walk-cache instability census (PIN SWITCH-GUARD:101) */
    case 102u:  /* build 0.0.543 item D (the 4,000 / 120 s policy): the longer continuous N/T (PIN SWITCH-GUARD:102) */
    case 103u:  /* build 0.0.544 ( item (1), gfx_stale103.h): the stale-input gate + instruments, ON / SHADOW (PIN SWITCH-GUARD:103) */
    case 104u:  /* build 0.0.547 ( fix (1), gfx_r5win104.h): the R5' BLIND window, ON / SHADOW (PIN SWITCH-GUARD:104) */
    case 105u:  /* build 0.0.547 ( fix (3), gfx_rectfb105.h): a RECTLIST fallback never commits, ON / SHADOW (PIN SWITCH-GUARD:105) */
    case 106u:  /* build 0.0.548 (, gfx_fillset.h n48_fs_learn): 33's members learned / shadowed (PIN SWITCH-GUARD:106) */
    case 107u:  /* build 0.0.550 ( PLAN step 1, gfx_lut107.h): the LUT learn retries; plane fails closed (PIN SWITCH-GUARD:107) */
    case 108u:  /* build 0.0.550 ( PLAN step 1, gfx_cgw108.h): the copy-guard wait under 37 (PIN SWITCH-GUARD:108) */
    case 109u:  /* build 0.0.552 ( PLAN (3), gfx_bb552.h): the resprov table's capacity + eviction, ON / SHADOW (PIN SWITCH-GUARD:109) */
    case 110u:  /* build 0.0.552 ( PLAN (2), gfx_bb552.h): the AN row, ON / SHADOW (PIN SWITCH-GUARD:110) */
    case 111u:  /* build 0.0.553 (, gfx_lutidx111.h): the LUT learn's entry, ON / SHADOW (PIN SWITCH-GUARD:111) */
    case 112u:  /* build 0.0.554 (, gfx_admit112.h): ADMIT STALE, ON / SHADOW (PIN SWITCH-GUARD:112) */
        return 1u;
    default:
        return 0u;
    }
}

static inline uint32_t n48_cm_cont_switch_refused(uint32_t switch_no, uint32_t is_read, uint32_t cont, uint32_t state)
{
    if (is_read) return 0u;
    if (!n48_cm_cont_switch_guarded(switch_no)) return 0u;
    if (!cont || state != N48_CM_SHOT_ARMED) return 0u;
    return 1u;
}

/* =====================================================================================================================
 * build 0.0.490 item 1 ( (B) item 9; the reviewer's correction of the brief) — THE ARM-VERB REFUSAL
 * FOR R1 ENFORCE. Under switch 42 ENFORCE a CLEAN R1 verdict discharges R4's wait/write refusal (gfxsrc_cprov_eval), which
 * makes the HEADLESS producers (switch 13's setup halves, R1-MEMDST.md Q1: each carries a live WAIT/RELEASE_MEM triplet)
 * eligible to commit for the first time - a path nobody has reviewed. A CONTINUOUS arm cannot commit them (they are
 * multi-segment single-IB frames: the owned-slot fence refuses them MULTI-SEGMENT and the gate's continuous fence rung is
 * asked before MEMDST), so only a ONE-SHOT arm is refused. The pre-arm checklist (n48_cm_arm_missing) REQUIRES switch 13 ON
 * for every arm, so "13 ON" alone cannot be the refusal's other half without making ENFORCE unarmable; the refusal is:
 *   a ONE-SHOT arm (no continuous request pending: `cont_requested` 0) while 42 is ENFORCE and 13 is ON.
 *   cont_requested  gXdContN != 0 - the arm about to be set is continuous (the verb's own branch condition).
 *   md_enforce      gMdMode == N48_MD_MODE_ENFORCE (298). SHADOW (810) and OFF (65322) answer 0 - OFF identity: with 42 not
 *                   ENFORCE this answers 0 for every input, so the arm verb behaves exactly as 0.0.489's.
 *   headless_on     gXdHeadless (switch 13).
 * 1 = refuse. The SAME function answers switch 42's own question "may ENFORCE be set while a one-shot arm stands?"
 * (cont_requested 0, md_enforce 1): otherwise arming a one-shot under SHADOW and then sending 298 would walk around it. */
static inline uint32_t n48_cm_arm_enforce_hl_refused(uint32_t cont_requested, uint32_t md_enforce, uint32_t headless_on)
{
    if (cont_requested) return 0u;
    return (md_enforce && headless_on) ? 1u : 0u;
}
/* ===================================================================================================================== */

/* The DISARM the verb performs (`4 | 2 << 8`). Always allowed, from any state, like N1's: going back to DECIDE can never
 * need a precondition. Answers 1 when it changed something. */
static inline uint32_t n48_cm_shot_disarm(n48_cm_shot *sh)
{
    if (!sh) return 0u;
    const uint32_t was = sh->state;
    sh->state = N48_CM_SHOT_OFF;
    sh->spent_seq = 0u;
    /* 0.0.381: the budget goes with the arm. It is re-supplied at the next arm, so a disarm can never leave a raised
     * budget standing for an arm that did not ask for one. */
    sh->budget = 0u;
    sh->spent = 0u;
    sh->last_seq = 0u;
    /* C5 part 2 — the continuous fields go with everything else: a disarm can never leave a standing N/T/start/
     * stop_why for the next arm (one-shot or continuous) to inherit. */
    sh->cont = 0u;
    sh->cont_n = 0u;
    sh->cont_t_us = 0ull;
    sh->cont_start_us = 0ull;
    sh->stop_why = N48_CM_STOP_NONE;
    return was == N48_CM_SHOT_ARMED || was == N48_CM_SHOT_SPENT ? 1u : 0u;
}

/* =====================================================================================================================
 * build 0.0.487 (an internal design note Q6 P4, Q7; contract C3) — SWITCH 57, THE COMPUTE-N ELIDE: the kext's two
 * pure pieces, here so tests/gfx_commit_test.cpp runs the SAME code the kext does.
 *
 * n48_cm_cs_is_n — THE PROGRAM ANSWER (xlat12's `ex->cs_is_n`, predicate part P4). 1 ONLY when THIS frame's own gather
 * found a program at exactly `want` (the first entry of `va[0..npgm)` equal to it - the gather dedupes VAs per frame),
 * scanned at the COMPUTE stage (N48_CM_CS_N_STAGE, gfx_capture_scan.h's "CS 6"), identified as Apple's BufferClear_CS
 * key (N48_CM_CS_N_KEY), key_class XLAT, and with bytes_are_ours: the bytes at that VA are OUR substituted gfx1201 port
 * (m4c-r18's SUBSTITUTE entry), never Apple's gfx10 N. It performs no read: every fact is the gather's. Anything else -
 * a VA the gather never saw, a different stage, key or class, Apple's own bytes, a relocated copy, a table overflow
 * (npgm past N48_XV_MAX_PGMS) - answers 0 and the dispatch is not elided. */
#define N48_CM_CS_N_KEY   0x9b226a39a78fe876ull
#define N48_CM_CS_N_STAGE 6u
static inline uint32_t n48_cm_cs_is_n(const n48_xv_program *pgm, const uint64_t *va, const uint32_t *stage, uint32_t npgm,
                                      uint64_t want)
{
    if (!pgm || !va || !stage || npgm > N48_XV_MAX_PGMS || !want) return 0u;
    for (uint32_t k = 0; k < npgm; k++) {
        if (va[k] != want) continue;
        return (stage[k] == N48_CM_CS_N_STAGE && pgm[k].key == N48_CM_CS_N_KEY && pgm[k].key_class == N48_XV_PGM_KEY_XLAT &&
                pgm[k].bytes_are_ours) ? 1u : 0u;
    }
    return 0u;
}

/* THE cselide57 REPORT LINE (one line per `gfxneuter 57` verb; bounded under N48_LOG_CAP_BODY by the test with every
 * counter at 20 digits). args: ON/OFF, how, the 42-mode note, segments the flag was set on, dispatches examined, elided,
 * refused by P2 / P3 / P4 / P5, frames whose candidate carried an elision, and of those the frames the gate's rule
 * (COMPUTE-ELIDE-R1: 42 ENFORCE and R1 clean) refused at the commit path. */
#define N48_CM_CSELIDE_FMT "cselide57: switch 57 %s (%s)%s. segs flagged %llu; dispatches seen %llu elided %llu; refused " \
                           "P2 %llu P3 %llu P4 %llu P5 %llu; frames with an elision %llu, refused COMPUTE-ELIDE-R1 %llu " \
                           "(needs 42 ENFORCE + R1 clean)."

/* build 0.0.535 ( fix 1; xlat12_ib.h XLAT12_EXTRA_NCLEAR) — SWITCH 91, THE ZERO FILL IN PLACE OF N. `91 | M << 8`:
 * M 1 ON (= 347), M 2 OFF (= 603, the default and the boot value), bare `91` reads. Acts only with switch 57 ON (the fill replaces
 * 57's elision) and only for a frame whose own binding is WindowServer's. Report args: state, how, note, segments flagged, fills,
 * bytes, packets, fell back (extent, room, run), the kext's check refused (segments: xlat12_ib_nclear_check or a page of the fill
 * that did not resolve to VRAM in the frame's own VM), pages walked, pages refused. The per-fill line (first N48_CM_NCLEAR_LINES per boot): frame, segment, VA range, bytes, packets, outcome. */
#define N48_CM_NCLEAR_LINES 16u
#define N48_CM_NCLEAR_FMT "nclear91: switch 91 %s (%s)%s. segs %llu; fills %llu (%llu B, %llu pkts); NOP kept: extent %llu room " \
                          "%llu run %llu barrier %llu max %llu; kext refused %llu (probes %llu bad %llu)."
/* build 0.0.535 fix round (e): THE FILL's PAGE WALK at the page table's own granularity. `probe` answers 1 when `va` resolves
 * to VRAM in the frame's VM and sets *big when the mapping there is a 64 KiB page: gfxc_page's own walk indexes the L1 table by
 * (va >> 16) & 0xfff - one entry per 64 KiB - and when that entry is itself the leaf (bit 63) the page is leafAddr + (va & 0xf000),
 * one contiguous 64 KiB mapping with one system bit, so one probe covers all sixteen 4 KiB pages of it; otherwise the entry points
 * at a sub-table of 4 KiB leaves and each 4 KiB page is probed. Every byte of [va, va + len) is covered. 1 = every probe resolved to
 * VRAM; `probes` (optional) counts the probes. Pure. */
typedef uint32_t (*n48_nc_probe_fn)(void *ctx, uint64_t va, uint32_t *big);
static inline uint32_t n48_nc_pages_walk(n48_nc_probe_fn probe, void *ctx, uint64_t va, uint64_t len, uint64_t *probes)
{
    if (!probe || !len) return 0u;
    const uint64_t end = va + len;
    for (uint64_t a = va & ~0xFFFull; a < end; ) {
        uint32_t big = 0u;
        if (probes) (*probes)++;
        if (!probe(ctx, a, &big)) return 0u;
        a = big ? (a & ~0xFFFFull) + 0x10000ull : (a & ~0xFFFull) + 0x1000ull;
    }
    return 1u;
}
#define N48_CM_NCLEAR_ONE_FMT "nclear91: frame %llu seg %u zero fill of VA [%#llx, %#llx) (%u bytes, %u DMA_DATA packets) - %s"

/* build 0.0.536 (; xlat12_ib.h XLAT12_EXTRA_RECT2D, gfx_rv92.h) — SWITCH 92, RECTLIST CLEARS THAT CLEAR. `92 | M << 8`:
 * M 1 ON (= 348), M 2 OFF (= 604, the default and the boot value), bare `92` reads. ON: (1) every segment of a WindowServer frame
 * carries XLAT12_EXTRA_RECT2D (VGT_GS_OUT_PRIM_TYPE = RECT_2D before every RECTLIST draw, TRISTRIP back before the next other draw,
 * today's output where there is no room), checked by the kext with xlat12_ib_rect2d_check; (2) the residency copy writes the v3
 * image of RectPosTexFast_VS (the blob's alternative entry, sc_alt_find) where it would write today's. Report args: state, how,
 * note, segments flagged, RECTLIST draws, RECT_2D written, TRISTRIP restored, no-room, fallbacks, kext refused, fixed-VS
 * substitutions, ON-but-no-alternative (today's image written). */
#define N48_CM_RECT92_FMT "rect92: switch 92 %s (%s)%s. segs %llu; RECTLIST draws %llu; RECT_2D written %llu, TRISTRIP restored %llu; " \
                          "no-room %llu, fallback %llu; kext refused %llu; fixed-VS substitutions %llu (ON, no alternative: %llu)."

/* build 0.0.537 (; xlat12_ib.h XLAT12_EXTRA_PWS) — SWITCH 93, APPLE'S CB/DB BARRIER GETS ITS WAIT BACK ON GFX12. `93 | M << 8`:
 * M 1 ON (= 349), M 2 OFF (= 605, the default and the boot value), bare `93` reads. ON: every segment of a WindowServer frame whose
 * translation starts at a packet the CP runs (a real EVENT_WRITE head, or a segment that starts at its own head) carries
 * XLAT12_EXTRA_PWS (Apple's barrier ACQUIRE_MEM -> RELEASE_MEM(PWS) + ACQUIRE_MEM(PWS) where the region before a draw has room; a
 * verbatim copy, counted, where it has not), and the kext checks each translated one with xlat12_ib_pws_check (every PWS release is
 * ours and directly followed by our PWS acquire; no PWS acquire without it). Report args: state, how, segments flagged, segments
 * not flagged because their head does not run (Apple's NOP disguise), barriers seen, converted, no-room (of them after the last
 * draw, of them the ones kept because Apple's buried fence828 packet follows in their region), fallback segments, other ACQUIRE_MEM shapes, pairing refusals; then (fix round item 1) THE COMMITTED TALLY - frames the COMMIT
 * gate answered yes for while 93 was ON, of them those with at least one conversion in their final candidate, and the conversions in
 * them (the counters before it are cumulative over every flagged segment, committed or not). */
#define N48_CM_PWS93_FMT "pws93: 93 %s (%s). segs %llu, head-not-run %llu; seen %llu, conv %llu, no-room %llu (tail %llu, fence %llu), " \
                         "fallback segs %llu; other %llu; pair-refused %llu. COMMITTED frames %llu, with conv %llu, conv %llu."
/* build 0.0.539 item 2 ( "to settle": which barriers convert on hardware), FIX ROUND item 1 (xhigh review, evidence
 * MUST-FIX) — THE PER-SEGMENT CONVERSION LINE. Once per frame the COMMIT gate answered yes for while switch 93 is ON (the kext's
 * pws93_commit), for a SELECTED frame only: the capsule chain's frames are judged frames ~69-165 (run11z), 140-164 (run11ac), 145-165
 * (run11ad), 150-162 (run11af), so the first-64-frames cap of the first build would have held none of them. n48_pws93_seg_pick admits
 * a frame with >= N48_PWS93_SEG_MIN_IBS IBs and >= N48_PWS93_SEG_MIN_SEGS rows with a conversion (the chain frames' shape; the small
 * one-IB frames never are), then logs the first N48_PWS93_SEG_FIRST admitted frames and after them every N48_PWS93_SEG_STRIDE-th (a
 * stride of 3: the chain frames recur every 4th frame, and 3 is coprime to it, so the sample walks across them), at most
 * N48_PWS93_SEG_FRAMES lines per arm (the arm's identity: gXdShot.armed_at_us; the rest counted). 64 lines reach admitted frame
 * 4 + 60 * 3 = ~184: past judged frame ~200 even if every committed frame from 16 on is admitted (~70% are, in the captures).
 * Each entry is ` k@head:conv`: the row k, its head's dword in Apple's IB (the frame's concatenated IBs, gXdBuild.seg[k].head - the
 * numbering of's 7376 / 9104 / 10768 / 12496), its conversions in the FINAL candidate (gPws93SegConv: 0 unless the row translated
 * with the flag). The line says whether k is a SEGMENT or a UNIT row (switch 55 formed units this frame: gXdBuild.units). The text
 * goes into b[cap] (NUL-terminated, never past cap); an entry that does not fit is left out and counted in *omitted (printed), and the
 * text then ends with " +". Pure. FMT args: judged frame, line, IBs, "seg" / "unit", conversions, rows with one, the text, left out.
 * The widest line fits N48_LOG_CAP_BODY (tests/gfx_pws93_test.cpp T9). */
#define N48_PWS93_SEG_FRAMES   64u
#define N48_PWS93_SEG_FIRST    4u
#define N48_PWS93_SEG_STRIDE   3u
#define N48_PWS93_SEG_MIN_IBS  2u
#define N48_PWS93_SEG_MIN_SEGS 6u
#define N48_PWS93_SEG_TEXT     310u
#define N48_PWS93_SEG_FMT "pws93 seg: committed frame %llu (line %u of 64; %u IBs; k = %s row): conv %llu in %u; k@head:conv%s (%u left out)"
/* The report's SECOND line (the bare `gfxneuter 93`; build 0.0.539, ; xlat12_ib.h XLAT12_PWS_SLOT_*): of `conv` above,
 * the conversions that took Apple's disabled top-of-pipe wait slot in place of pad (ds->pws_slot, summed over every flagged segment
 * like the first line's counters); then the per-segment lines printed this arm, the frames admitted this arm, and admitted frames not
 * printed since boot. The first line keeps 0.0.538's text byte for byte (at 20-digit fields it is at its 491-byte width). */
#define N48_PWS93_SEG_REPORT_FMT "pws93: slot reused %llu (of conv); per-segment lines this arm %u of 64 (frames admitted %u), admitted " \
    "but not printed %llu."
/* The selection state, per arm (the caller resets it when the arm changes). */
typedef struct { uint32_t admitted, lines; uint64_t notPrinted; } n48_pws93_sel;
/* 1 = log this committed frame (nib IBs, rows rows with a conversion). */
static inline uint32_t n48_pws93_seg_pick(n48_pws93_sel *s, uint32_t nib, uint32_t rows)
{
    if (nib < N48_PWS93_SEG_MIN_IBS || rows < N48_PWS93_SEG_MIN_SEGS) return 0u;
    const uint32_t a = s->admitted++;
    const uint32_t want = a < N48_PWS93_SEG_FIRST || ((a - N48_PWS93_SEG_FIRST) % N48_PWS93_SEG_STRIDE) == N48_PWS93_SEG_STRIDE - 1u;
    if (!want || s->lines >= N48_PWS93_SEG_FRAMES) { s->notPrinted++; return 0u; }
    s->lines++;
    return 1u;
}
static inline uint32_t n48_pws93_dec(char *t, uint32_t v)
{
    char d[12]; uint32_t dl = 0u, tl = 0u;
    do { d[dl++] = (char)('0' + v % 10u); v /= 10u; } while (v && dl < sizeof d);
    while (dl) t[tl++] = d[--dl];
    return tl;
}
static inline uint32_t n48_pws93_segtext(char *b, uint32_t cap, const uint16_t *conv, const uint32_t *head, uint32_t n, uint32_t *omitted)
{
    uint32_t at = 0u, wrote = 0u, left = 0u;
    if (omitted) *omitted = 0u;
    if (!b || cap < 4u) return 0u;
    b[0] = '\0';
    for (uint32_t k = 0; conv && head && k < n; k++) {
        if (!conv[k]) continue;
        char t[40]; uint32_t tl = 0u;
        t[tl++] = ' ';
        tl += n48_pws93_dec(&t[tl], k);
        t[tl++] = '@';
        tl += n48_pws93_dec(&t[tl], head[k]);
        t[tl++] = ':';
        tl += n48_pws93_dec(&t[tl], conv[k]);
        if (at + tl + 3u > cap) { left++; continue; }   /* keep room for " +" and the NUL */
        for (uint32_t j = 0; j < tl; j++) b[at++] = t[j];
        wrote++;
    }
    if (left) { b[at++] = ' '; b[at++] = '+'; }
    b[at] = '\0';
    if (omitted) *omitted = left;
    return wrote;
}

/* build 0.0.500 (an internal design note Q4) — SWITCH 66's VALUES. `66 | M << 8`: M 1 (= 322) the U/Y rows (the clock
 * composite), M 3 (= 834) the U/Y rows AND the AO row (the login panel material), M 2 (= 578) OFF (the default and the boot
 * value), bare `66` reads. The answer is the translator's row-class mask (xlat12_ib.h XLAT12_DE_CLASS_UY / _AO); 0 = OFF.
 * n48_cm_de_set: 1 and *rows set for M 1/2/3, 0 (unchanged) for anything else. Pure. */
/* build 0.0.512: M 7 (= 66 | 7 << 8 = 1858) the U/Y rows, the AO row AND the glass rows (xlat12_ib.h
 * XLAT12_DE_CLASS_GLASS: BD, and BA's narrow not-last case) - mask 0x7. M 1 and M 3 are unchanged (322 / 834 byte-identical). */
static inline uint32_t n48_cm_de_rows_of(uint32_t m)
{
    return m == 1u ? 0x1u : m == 3u ? 0x3u : m == 7u ? 0x7u : 0u;
}
static inline uint32_t n48_cm_de_set(uint32_t m, uint32_t *rows)
{
    if (m != 1u && m != 2u && m != 3u && m != 7u) return 0u;
    if (rows) *rows = n48_cm_de_rows_of(m);
    return 1u;
}
/* The kext's own backstop after a translation that carried the flag (the translator has the same one): 1 = refuse, when
 * the output's draw packets (xlat12_ib_count_draws) are not exactly `draws - elided`. Pure. */
static inline uint32_t n48_cm_de_backstop(uint32_t out_draws, uint32_t draws, uint32_t elided)
{
    return (elided > draws || out_draws != draws - elided) ? 1u : 0u;
}
/* THE drawelide66 REPORT LINE (one line per `gfxneuter 66` verb; bounded under N48_LOG_CAP_BODY by the test with every
 * counter at 20 digits). args: the rows ("OFF (default)", "ON U/Y", "ON U/Y+AO"), how, the note, segments flagged,
 * PROVENANCE refusals examined, elided, refused not-last / not-row / no-dcc / write-set / cap, backstop refusals, frames
 * with an elision and of those refused DRAW-ELIDE-R1 ("DE-R1"). The census line: four (ndw, fnv, count) of programs
 * refused not-row (the usable-OS view). */
/* build 0.0.501 (an internal design note Q3 C3) — THE unitpack67 REPORT LINE (one line per
 * `gfxneuter 67` verb; bounded under the 491-byte log body by tests/gfx_mib_units_checks.h at 20-digit counters). `67 | M << 8`: M 1 ON
 * (= 323), M 2 OFF (= 579, the default and the boot value), bare `67` reads (gfx_rasterarm.h n48_ra_set). args: ON/OFF, how,
 * the note (INERT without 55: the translator reads `pack` only under XLAT12_EXTRA_UNIT), and over units/retried singles that
 * translated with pack ON: NOPs opened (packed runs), records placed, dwords saved (xlat12_unit pk_saved: what each appended
 * record took less than a NOP of its own at the same place), and translations refused NO_ROOM with pack ON (exhaustion). */
#define N48_CM_UNITPACK_FMT "unitpack67: switch 67 %s%s%s. packed runs %llu records %llu dwords saved %llu; exhaustion " \
                            "refusals %llu."
#define N48_CM_DRAWELIDE_FMT "drawelide66: switch 66 %s%s%s. segs %llu seen %llu elided %llu; refused not-last %llu " \
                             "not-row %llu no-dcc %llu write-set %llu cap %llu; backstop %llu; frames %llu DE-R1 %llu."
/* The census half, printed right after it (the not-row programs: ndw/fnv and count, four slots). */
#define N48_CM_DRAWELIDE_CENSUS_FMT "drawelide66: would-elide census (refused not-row, by ndw/fnv): %u/%#x x%llu, " \
                                    "%u/%#x x%llu, %u/%#x x%llu, %u/%#x x%llu."
/* The per-elision line (the first 16 per boot): frame, segment (unit), the draw's dword in it, the row, the surface. */
#define N48_CM_DRAWELIDE_ONE_FMT "drawelide66: frame %llu seg %u draw@%u row %s surface VA %#llx mode %u (%s)"
static inline const char *n48_cm_de_row_name(uint32_t row)
{
    return row == 1u ? "U" : row == 2u ? "Y" : row == 3u ? "AO" : row == 4u ? "BD" : row == 5u ? "BA" : row == 6u ? "AN" : "?";   /* 0.0.512: BD, BA; 0.0.552: AN */
}
/* build 0.0.512: the report line's name for a row-class mask (gDrawElideOn). Pure. */
static inline const char *n48_cm_de_mask_name(uint32_t r)
{
    return r == 7u ? "ON U/Y+AO+glass" : r == 3u ? "ON U/Y+AO" : r == 1u ? "ON U/Y" : "OFF (default)";
}

#endif
