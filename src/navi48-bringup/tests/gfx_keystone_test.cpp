// gfx_keystone_test.cpp — X10's safety proof, offline. The properties under test are the project's own
// specification of the COMMIT-time root keystone:
//
//     1. IT DOES NOTHING UNLESS COMMIT IS ARMED.
//     2. IT WRITES ONLY THE ROOT OF THE CONTEXT ws_ident HAS BOUND, RE-VALIDATED AT THE INSTANT OF THE WRITE.
//     3. IT IS IDEMPOTENT.
//     4. THE COMMITTED FRAME MAY RUN ONLY IF THE MAPPING IS PROVEN AFTERWARDS.
//     5. A PRESENT WHOSE READBACK DISAGREED IS NOT A REFUSAL.
//
// Every clause is checked BOTH ways: falsified on its own it must refuse, and a permitted verdict must imply all of
// them. The second half is checked over the whole 16384-case cross product of the fourteen PERTURBATIONS (clause 9b,
// the withdrawal marker, has its own named block and is not one of the cross-product perturbations), not by example.
//
// 0.0.374: clause 8 CHANGED MEANING. It used to refuse any bound VMID but 2, because the shared write
// path hard-coded navi48_gmc_flush_tlb_vmid(2, 0); the VMID is now threaded through RootArmExtra::tlbVmid, so the
// clause asks only whether the invalidate can serve the VMID at all (it refuses vmid > 15). Section 4 below is the
// NON-VACUITY proof for that: every check in its first loop but one FAILS against the old hard-coded rule.
//
// 0.0.411: clause 9b (a withdrawal in progress refuses the arm) and the post-walk re-check are new.
// With them, K1's re-arm write is gone: a stale record refuses as REC_MISMATCH. Section 5b and the report-line and
// source-pin sections below are their named checks and their non-vacuity proofs (D14/D15/D16).
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). The mutants are run against
// the SAME checks - D1..D13 as before, plus D14 (the stale re-arm), D15 (the marker ignored), D16 (the post-walk
// re-check never withdrawing), D17 (0.0.418 E4: the withdrawal counter cleared as a flag), D18-D21 (0.0.423 A-prime:
// the hold and the marker's set point) and D23 (0.0.431: the refusal CLEARS the flight instead of restoring the
// previous one) - and each must be CAUGHT by at least one named check. D10 IS the 0.0.374 rule verbatim, D14 is
// 0.0.410's K1 clause verbatim, D17 is the 0.0.411 decrement verbatim and D23 is 0.0.430's clear verbatim, so the
// counts they fail are NON-VACUITY proofs. D22 was retired with its subject (`n48_ksd_flight_after_keystone`), which
// the kext never called; D23 replaces it on the function the kext actually calls.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_keystone_test.cpp -o /tmp/kstest && \
//         /tmp/kstest src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "gfx_keystone.h"
#include "gfx_flightring.h"   // C5 part 2 (build 0.0.460): the H2 extension below drives the REAL ring, two flights
#include "display_pipe_guard.h"
#include "rootwrite_guards.h"   // 0.0.410: the K1 test drives the REAL guard G4 with the live value

static int gFail = 0, gRun = 0, gQuiet = 0, gFailOnly = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) printf("  FAIL %-62s got %llu want %llu\n", what, (unsigned long long)got,
                            (unsigned long long)want);
        else if (gFailOnly) printf("        FAIL %s\n", what);
    } else if (!gQuiet) printf("  ok   %-62s %llu\n", what, (unsigned long long)got);
}

// ---------------------------------------------------------------------------------------------------------------------
// THE GOOD INPUT: arm3's own numbers. WindowServer is create #5, ctx 0xffffff9518fa1580, root
// 0x3d6c00000, VMID 2; the frame is ws#1, stamp 0x2, judged N48_WSF_JUDGE; the one-shot's token matched and the gate
// answered COMMIT for seq 1. Nothing has been armed yet this boot.
// ---------------------------------------------------------------------------------------------------------------------
static const uint64_t kWsCtx = 0xffffff9518fa1580ull, kWsRoot = 0x3d6c00000ull;
// 0.0.410: the root[511] entry this boot's arm writes. arm26's own value, from the deferred-withdrawal
// run: the record claims THIS is standing, and the live read at the instant must equal it for ALREADY to be returned.
static const uint64_t kWsEntry = 0x10000003cba90001ull;

static n48_ks_in good()
{
    n48_ks_in k {};
    k.arm_now = 2u; k.arm_commit = 2u;            /* N48_SD_ARM_COMMIT */
    k.token_matched = 1u; k.gate_seq = 1u;
    k.frame_verdict = 0u;                          /* N48_WSF_JUDGE */
    k.frame_vmid = 2u; k.frame_ws_seq = 5u; k.frame_ws_ctx = kWsCtx; k.frame_ws_root = kWsRoot;
    k.now_state = 1u;                              /* N48_WS_BOUND */
    k.now_complete = 1u; k.now_seq = 5u; k.now_vmid = 2u; k.now_ctx = kWsCtx; k.now_root = kWsRoot;
    k.now_hw_ok = 1u; k.now_hw_root = kWsRoot;
    k.rec_found = 1u; k.rec_live = 1u; k.rec_id_ok = 1u; k.rec_seq = 5u;
    k.rec_ctx = kWsCtx; k.rec_root = kWsRoot;
    k.rec_wrote = 0u; k.rec_withdrawn = 0u;
    k.attempts = 0u; k.ringmap_built = 1u;
    k.armed_contexts = 0u; k.armed_cap = 8u;
    return k;
}

// The fourteen independent ways the good input can go bad, each the falsification of exactly one clause, each with
// the verdict the rule owes it.
struct Perturb { const char *name; uint32_t want; void (*apply)(n48_ks_in &); };
static const Perturb kP[] = {
    { "COMMIT not armed",                    N48_KS_NOT_ARMED,    [](n48_ks_in &k){ k.arm_now = 1u; } },
    { "the one-shot token did not match",    N48_KS_NO_TOKEN,     [](n48_ks_in &k){ k.token_matched = 0u; } },
    { "no gate answer stands",               N48_KS_NO_TOKEN,     [](n48_ks_in &k){ k.gate_seq = 0u; } },
    { "the frame is not WindowServer's",     N48_KS_FRAME,        [](n48_ks_in &k){ k.frame_verdict = 5u; } },
    { "the re-validation is INCOMPLETE",     N48_KS_INCOMPLETE,   [](n48_ks_in &k){ k.now_complete = 0u; } },
    { "ws_ident is NOT BOUND now",           N48_KS_NOT_BOUND,    [](n48_ks_in &k){ k.now_state = 2u; } },
    { "the binding MOVED (another ctx)",     N48_KS_MOVED,        [](n48_ks_in &k){ k.now_ctx = kWsCtx + 0x1000ull;
                                                                                    k.rec_ctx = kWsCtx + 0x1000ull; } },
    { "the VMID is not the recorded one",    N48_KS_VMID,         [](n48_ks_in &k){ k.now_vmid = 3u; } },
    { "that VMID no longer names the root",  N48_KS_VMID,         [](n48_ks_in &k){ k.now_hw_root = kWsRoot + 0x1000ull; } },
    { "no record IS the bound context",      N48_KS_NO_RECORD,    [](n48_ks_in &k){ k.rec_found = 0u; } },
    { "the record's root is another one",    N48_KS_REC_MISMATCH, [](n48_ks_in &k){ k.rec_root = kWsRoot + 0x1000ull; } },
    { "this boot has no ring mapping",       N48_KS_RINGMAP,      [](n48_ks_in &k){ k.ringmap_built = 0u; } },
    { "the armed-context bound is reached",  N48_KS_ARM_CAP,      [](n48_ks_in &k){ k.armed_contexts = 8u; } },
    /* 0.0.374: clause 8's remaining job - a VMID the TLB invalidate cannot serve. Both fields move together
     * so clause 7 passes and clause 8 is the one being falsified. */
    { "a VMID the invalidate cannot serve", N48_KS_TLB_VMID,      [](n48_ks_in &k){ k.now_vmid = 16u;
                                                                                    k.frame_vmid = 16u; } },
};
static constexpr uint32_t kNP = (uint32_t)(sizeof(kP) / sizeof(kP[0]));

typedef uint32_t (*EvalFn)(const n48_ks_in *);
typedef uint32_t (*PermitFn)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef uint32_t (*BucketFn)(uint32_t);
/* 0.0.384: the in-flight withdrawal deferral's rule, as a substitutable function so the two new
 * planted defects below can replace it exactly as D1..D10 replace the other three. */
typedef uint32_t (*DeferFn)(const n48_ksd_in *);
/* 0.0.411: the post-walk re-check, substitutable so a mutant that never withdraws must be caught. */
typedef uint32_t (*PostFn)(uint32_t, uint32_t, uint64_t, uint64_t);
static PostFn gPostWalk = &n48_ks_post_walk_withdrawn;
/* 0.0.418 (E4): the withdrawal counter's decrement, substitutable so the flag's own clear (D17) is a planted break. */
typedef uint32_t (*WithdrawFn)(uint32_t);
static WithdrawFn gWithdrawLeave = &n48_ks_withdraw_leave;
/* 0.0.431: the refusal's flight update, substitutable so D23 (0.0.430's clear instead of the restore)
 * is a planted break. It is declared with the H2 extension below, where the previous-flight model lives. */

// ---------------------------------------------------------------------------------------------------------------------
// THE CHECKS
// ---------------------------------------------------------------------------------------------------------------------
static uint64_t gSweep = 0, gSweepNonVacuous = 0, gInert = 0, gInertNonVacuous = 0;

static uint64_t gDSweep = 0, gDSweepDefer = 0, gDInert = 0, gDInertNonVacuous = 0;

static void checks(EvalFn ev, PermitFn pm, BucketFn bk, DeferFn df)
{
    // 0.0.374. Every check outside section 4b is about a clause OTHER than the invalidate's ACK, so it
    // hands the rule an ACKNOWLEDGED invalidate and its meaning is exactly what it was before. Section 4b is the
    // only place the ack is falsified. Naming the value rather than writing 1u at nineteen call sites is what makes
    // "these checks are unchanged" readable instead of asserted.
    const uint32_t ack = 1u;

    // ---- 1. THE POSITIVE CONTROL. Without it every refusal below is vacuous. ----
    {
        const n48_ks_in k = good();
        ck("POSITIVE CONTROL: arm3's own inputs -> OK", ev(&k), N48_KS_OK);
        ck("POSITIVE CONTROL: OK means WRITE", n48_ks_write_needed(ev(&k)), 1u);
        ck("POSITIVE CONTROL: written, verified, all four walks -> PERMITTED",
           pm(ev(&k), 0u, 2u, ack, 0xFu, 0xFu), 1u);
    }

    // ---- 2. EACH CLAUSE, FALSIFIED ON ITS OWN, WITH ITS OWN NAME ----
    for (uint32_t i = 0; i < kNP; i++) {
        n48_ks_in k = good();
        kP[i].apply(k);
        char b[128];
        snprintf(b, sizeof(b), "REFUSES: %s", kP[i].name);
        ck(b, ev(&k), kP[i].want);
        snprintf(b, sizeof(b), "  ...and writes NOTHING: %s", kP[i].name);
        ck(b, n48_ks_write_needed(ev(&k)), 0u);
        snprintf(b, sizeof(b), "  ...and NEVER permits the frame: %s", kP[i].name);
        ck(b, pm(ev(&k), 0u, 2u, ack, 0xFu, 0xFu), 0u);
    }

    // ---- 3. THE CACHED-IDENTITY SHAPE, NAMED. The re-validation must be able to CONTRADICT the cached judgement in
    //         every field on its own. A rule that trusts wf passes all three of these. ----
    {
        n48_ks_in k = good(); k.now_seq = 7u; k.rec_seq = 7u;
        ck("CACHED IDENTITY: a re-validation naming another create # REFUSES", ev(&k), N48_KS_MOVED);
        k = good(); k.now_root = kWsRoot + 0x2000ull; k.rec_root = kWsRoot + 0x2000ull;
        k.now_hw_root = kWsRoot + 0x2000ull;
        ck("CACHED IDENTITY: a re-validation naming another root REFUSES", ev(&k), N48_KS_MOVED);
        k = good(); k.frame_vmid = 3u;
        ck("CACHED IDENTITY: the frame's VMID and the binding's must agree", ev(&k), N48_KS_VMID);
    }

    // ---- 4. THE TLB CLAUSE, AS OF 0.0.374. Until 0.0.373 the shared write path hard-coded
    //         navi48_gmc_flush_tlb_vmid(2, 0) and this clause refused every bound VMID but 2 - which is the
    //         defect ("a VMID number is an accident of order, never an identity") written into a safety rule. The
    //         VMID is now threaded through RootArmExtra::tlbVmid, so the invalidate covers whatever clauses 3/6/7
    //         bound, and clause 8 asks only what is still genuinely about the invalidate: CAN IT SERVE THIS VMID.
    //         navi48_gmc_flush_tlb_vmid (Navi48Bringup.cpp:3033) refuses `vmid > 15` because gmc_flush_gpu_tlb sets
    //         PER_VMID_INVALIDATE_REQ to `1u << vmid` in a 16-bit field (amd/gmc_v12_0.cpp:1658). ----
    {
        // NON-VACUITY FOR THE THREADED PATH. EVERY check in this loop except v == 2 FAILED under the old rule; if
        // the VMID were still hard-coded to 2, fourteen of these fifteen would return N48_KS_TLB_VMID.
        for (uint32_t v = 1u; v <= N48_KS_MAX_INVALIDATE_VMID; v++) {
            n48_ks_in k = good(); k.now_vmid = v; k.frame_vmid = v;
            char b[128];
            snprintf(b, sizeof(b), "ACCEPTS bound VMID %u - the invalidate is threaded, not hard-coded", v);
            ck(b, ev(&k), N48_KS_OK);
            snprintf(b, sizeof(b), "  ...and WRITES for VMID %u", v);
            ck(b, n48_ks_write_needed(ev(&k)), 1u);
        }
        // The number the old rule lost the coin flip on: AppleHardwareHook.cpp records "hp3,: VMID 2 was the
        // draw client's, WindowServer drew on VMID 3".
        {   n48_ks_in k = good(); k.now_vmid = 3u; k.frame_vmid = 3u;
            ck("VMID 3 (WindowServer's in) is ACCEPTED, where 0.0.373 refused it", ev(&k), N48_KS_OK);
            ck("  ...and the frame is PERMITTED once the walks pass", pm(ev(&k), 0u, 2u, ack, 0xFu, 0xFu), 1u); }
        // THE REFUSAL THAT REMAINS, AND ITS BOUNDARY - BOTH SIDES.
        {   n48_ks_in k = good(); k.now_vmid = 15u; k.frame_vmid = 15u;
            ck("BOUNDARY: VMID 15 is the last one the invalidate can serve -> OK", ev(&k), N48_KS_OK); }
        {   n48_ks_in k = good(); k.now_vmid = 16u; k.frame_vmid = 16u;
            ck("BOUNDARY: VMID 16 is one too far -> REFUSED", ev(&k), N48_KS_TLB_VMID);
            ck("  ...and writes NOTHING", n48_ks_write_needed(ev(&k)), 0u);
            ck("  ...and never permits the frame", pm(ev(&k), 0u, 2u, ack, 0xFu, 0xFu), 0u); }
        {   n48_ks_in k = good(); k.now_vmid = 32u; k.frame_vmid = 32u;
            ck("REFUSES a VMID that would shift clean out of PER_VMID_INVALIDATE_REQ", ev(&k), N48_KS_TLB_VMID);
            ck("  ...and writes NOTHING", n48_ks_write_needed(ev(&k)), 0u); }
        {   n48_ks_in k = good(); k.now_vmid = 0xFFFFFFFFu; k.frame_vmid = 0xFFFFFFFFu;
            ck("REFUSES an ABSURD VMID rather than wrapping it", ev(&k), N48_KS_TLB_VMID); }
        // VMID 0 is still refused - one clause EARLIER (clause 7), and that is deliberately unchanged.
        {   n48_ks_in k = good(); k.now_vmid = 0u; k.frame_vmid = 0u;
            ck("VMID 0 is still refused, and at clause 7 - not at clause 8", ev(&k), N48_KS_VMID); }
    }

    // ---- 4b. THE INVALIDATE'S ACKNOWLEDGEMENT (0.0.374) ----
    //          Clause 8 asks whether the invalidate CAN serve this VMID. This asks whether it DID. They are different
    //          questions and only the first one existed before: `n48_ks_arm_permitted` required verdict + guard +
    //          status 2 + the four walks, and NOT the ack - so a frame whose invalidate timed out was armed anyway.
    //          The read-back proves the BYTES landed in the page and the walks prove the PAGE TABLE resolves; neither
    //          reads the TLB, which is the only thing that decides whether the GPU's walker sees our root[511].
    //          NON-VACUITY: every check in this block PASSES trivially against the PRE- rule in the `want 1` cases
    //          and FAILS against it in every `want 0` case - see mutant D10, which is that rule verbatim.
    {
        const n48_ks_in g = good();
        const uint32_t v = ev(&g);
        ck("TLB ACK: OK + written + verified + all four walks + ACKED -> PERMITTED",
           pm(v, 0u, 2u, 1u, 0xFu, 0xFu), 1u);
        ck("TLB ACK: the SAME arm with the invalidate NOT acknowledged -> REFUSED",
           pm(v, 0u, 2u, 0u, 0xFu, 0xFu), 0u);
        ck("TLB ACK: ...and the reason is named, not a bare refusal",
           n48_ks_arm_reason(v, 0u, 2u, 0u, 0xFu, 0xFu), (uint64_t)N48_KSA_TLB_NOACK);
        ck("TLB ACK: a perfect read-back does NOT substitute for the ack",
           pm(v, 0u, 2u, 0u, 0xFu, 0xFu), 0u);
        ck("TLB ACK: four resolving walks do NOT substitute for the ack either",
           pm(v, 0u, 2u, 0u, 0xFu, 0xFu), 0u);
        // THE IDEMPOTENT PATH IS GATED TOO, AND THAT IS THE DECISION THIS BLOCK RECORDS. A no-op re-arm issues no
        // invalidate at all, so its ack is the one RECORDED ON THE ENTRY by whichever write left it standing; an
        // entry with nothing recorded is 0 and refuses. Exempting ALREADY would have left the fail-open exactly where
        // it is most likely to bite - unmapVA's re-arm rewrites the entry behind a TLB flush of its own.
        n48_ks_in a = good(); a.rec_wrote = 1u;
        a.rec_slot_ok = 1u; a.rec_written = kWsEntry; a.rec_slot_live = kWsEntry;   // K1: the live slot still holds it
        const uint32_t va = ev(&a);
        ck("TLB ACK: the idempotent NO-OP is ALREADY", va, (uint64_t)N48_KS_ALREADY);
        ck("TLB ACK: ALREADY + walks + a RECORDED ack -> PERMITTED", pm(va, 0u, 0u, 1u, 0xFu, 0xFu), 1u);
        ck("TLB ACK: ALREADY with NO recorded ack -> REFUSED (the gate is not OK-only)",
           pm(va, 0u, 0u, 0u, 0xFu, 0xFu), 0u);
        ck("TLB ACK: ...named on the ALREADY path too",
           n48_ks_arm_reason(va, 0u, 0u, 0u, 0xFu, 0xFu), (uint64_t)N48_KSA_TLB_NOACK);
        // ORDER: the ack is checked AFTER the write's own evidence, so a refused write is still reported as such and
        // the log does not blame the TLB for a guard that never let the write happen.
        ck("TLB ACK: a guard refusal outranks the ack",
           n48_ks_arm_reason(v, 4u, 2u, 0u, 0xFu, 0xFu), (uint64_t)N48_KSA_GUARD);
        ck("TLB ACK: a read-back mismatch outranks the ack",
           n48_ks_arm_reason(v, 0u, 3u, 0u, 0xFu, 0xFu), (uint64_t)N48_KSA_STATUS);
        ck("TLB ACK: a verdict that claims nothing outranks everything",
           n48_ks_arm_reason(N48_KS_MOVED, 0u, 2u, 1u, 0xFu, 0xFu), (uint64_t)N48_KSA_NOT_CLAIMED);
        // THE PREDICATE AND THE NAME CAN NEVER DISAGREE, over every combination either one can see.
        uint64_t disagree = 0;
        for (uint32_t vv = 0; vv < N48_KS_REASONS; vv++)
            for (uint32_t gd = 0; gd <= 4u; gd++)
                for (uint32_t st = 0; st <= 3u; st++)
                    for (uint32_t ak = 0; ak <= 1u; ak++)
                        for (uint32_t w = 0; w <= 0xFu; w++)
                            for (uint32_t nd = 0; nd <= 0xFu; nd++)
                                if (n48_ks_arm_permitted(vv, gd, st, ak, w, nd) !=
                                    (n48_ks_arm_reason(vv, gd, st, ak, w, nd) == N48_KSA_OK ? 1u : 0u)) disagree++;
        ck("TLB ACK: the predicate and the named reason agree on every input", disagree, 0u);
        // AND EVERY NAME IS A REAL STRING (the same rule n48_ks_name is held to).
        uint64_t unnamed = 0;
        for (uint32_t r = 0; r < N48_KSA_ARM_REASONS; r++)
            if (!n48_ks_arm_name(r) || n48_ks_arm_name(r)[0] == '?') unnamed++;
        ck("TLB ACK: every arm reason has its own name", unnamed, 0u);
    }

    // ---- 5. IDEMPOTENCE, BOTH HALVES ----
    {
        n48_ks_in k = good(); k.rec_wrote = 1u;
        k.rec_slot_ok = 1u; k.rec_written = kWsEntry; k.rec_slot_live = kWsEntry;   // K1: proven standing
        ck("IDEMPOTENT: an entry already standing is a NO-OP", ev(&k), N48_KS_ALREADY);
        ck("IDEMPOTENT: ...and nothing is written a second time", n48_ks_write_needed(ev(&k)), 0u);
        ck("IDEMPOTENT: ...and the mapping is still CLAIMED", n48_ks_mapping_claimed(ev(&k)), 1u);
        ck("IDEMPOTENT: ...and the frame may run only if the walks pass", pm(ev(&k), 0u, 0u, ack, 0xFu, 0xFu), 1u);
        ck("IDEMPOTENT: ...and NOT if they do not", pm(ev(&k), 0u, 0u, ack, 0xDu, 0xFu), 0u);

        k = good(); k.rec_wrote = 1u; k.rec_withdrawn = 1u; k.attempts = 1u;
        ck("A WITHDRAWN entry with an attempt spent REFUSES rather than retrying", ev(&k), N48_KS_SPENT);
        ck("  ...and writes NOTHING", n48_ks_write_needed(ev(&k)), 0u);
        ck("  ...and never permits the frame", pm(ev(&k), 0u, 2u, ack, 0xFu, 0xFu), 0u);

        k = good(); k.attempts = 1u;
        ck("A SECOND attempt with nothing armed REFUSES (the first one failed)", ev(&k), N48_KS_SPENT);
        ck("  ...and writes NOTHING", n48_ks_write_needed(ev(&k)), 0u);

        // The real sequence: write once, then the state the write leaves behind must be a no-op.
        n48_ks_in first = good();
        ck("SEQUENCE: the first call writes", n48_ks_write_needed(ev(&first)), 1u);
        n48_ks_in second = good();
        second.rec_wrote = 1u; second.attempts = 1u;   // exactly what the first call leaves
        second.rec_slot_ok = 1u; second.rec_written = kWsEntry; second.rec_slot_live = kWsEntry;
        ck("SEQUENCE: the second call is a logged NO-OP, not a second write",
           n48_ks_write_needed(ev(&second)), 0u);
        ck("SEQUENCE: and it is ALREADY, not SPENT", ev(&second), N48_KS_ALREADY);
    }

    // ---- 5b. K1 (0.0.410,) / F1 (0.0.411,) — ALREADY IS PROVEN BY A LIVE READ, AND A DISAGREEMENT IS A
    //          FAIL-CLOSED REFUSAL, NEVER A WRITE. --------------------------------------------------------------------
    // THE DEFECT: arm26's f16. The record said `wrote 1 withdrawn 0` (so 0.0.410's clause 10 was going to re-arm it)
    // while the deferred withdrawal had just been applied and root[511] read ZERO. withdrew the re-arm itself:
    // the live read makes the verdict REC_MISMATCH, a refusal with no write. The four outcomes are checked side by side
    // so a mutant that trusts the record or that re-arms the stale case is unmistakable.
    {
        n48_ks_in k = good();
        k.rec_wrote = 1u; k.rec_slot_ok = 1u; k.rec_written = kWsEntry; k.rec_slot_live = 0ull;
        k.attempts = 1u;                                // the first (successful) arm already spent an attempt
        ck("F1 STALE: wrote && !withdrawn but the live root[511] is ZERO -> REC_MISMATCH", ev(&k), N48_KS_REC_MISMATCH);
        ck("F1 STALE: ...and it writes NOTHING (K1's re-arm is withdrawn)", n48_ks_write_needed(ev(&k)), 0u);
        ck("F1 STALE: ...and never permits the frame", n48_ks_arm_permitted(ev(&k), 0u, 2u, ack, 0xFu, 0xFu), 0u);

        n48_ks_in eq = k; eq.rec_slot_live = kWsEntry;   // live == recorded -> the entry is standing
        ck("F1 LIVE == RECORDED: ALREADY (a logged no-op)", ev(&eq), N48_KS_ALREADY);
        ck("F1 LIVE == RECORDED: ...and writes NOTHING", n48_ks_write_needed(ev(&eq)), 0u);

        n48_ks_in unread = k; unread.rec_slot_ok = 0u;   // the read failed -> nothing is proven
        ck("F1 UNREAD: the slot could not be read -> REC_MISMATCH, never a write",
           ev(&unread), N48_KS_REC_MISMATCH);
        ck("F1 UNREAD: ...and writes NOTHING", n48_ks_write_needed(ev(&unread)), 0u);

        // A NON-ZERO FOREIGN VALUE: still a disagreement with the record, so still REC_MISMATCH and no write.
        n48_ks_in fo = k; fo.rec_slot_live = 0x0000000deadbe000ull;
        ck("F1 FOREIGN: a different live value -> REC_MISMATCH", ev(&fo), N48_KS_REC_MISMATCH);
        ck("F1 FOREIGN: ...and it writes NOTHING", n48_ks_write_needed(ev(&fo)), 0u);

        // THE MARKER (F1's new clause). A withdrawal in progress refuses the whole arm, no write and no wait, and it
        // is checked BEFORE the ALREADY clause so a no-op can never let a frame run against a slot being taken down.
        n48_ks_in mk = good(); mk.rec_withdrawing = 1u;
        ck("F1 MARKER: a withdrawal in progress -> N48_KS_WITHDRAWING", ev(&mk), N48_KS_WITHDRAWING);
        ck("F1 MARKER: ...and writes NOTHING", n48_ks_write_needed(ev(&mk)), 0u);
        ck("F1 MARKER: ...and never permits the frame", n48_ks_arm_permitted(ev(&mk), 0u, 2u, ack, 0xFu, 0xFu), 0u);
        {
            n48_ks_in m2 = good(); m2.rec_wrote = 1u; m2.rec_slot_ok = 1u; m2.rec_written = kWsEntry;
            m2.rec_slot_live = kWsEntry; m2.rec_withdrawing = 1u;
            ck("F1 MARKER: ...and it refuses even the ALREADY case", ev(&m2), N48_KS_WITHDRAWING);
        }

        // 0.0.418 (E4, notes/design/BUILD-0.0.418.md) — THE MARKER IS A COUNTER, NOT A FLAG. Two overlapping unmaps of
        // one context are possible on hook_unmapVA's lock-free clear -> Apple's unmapVA -> re-arm span; as a flag the
        // first to finish cleared the second's marker and the keystone was let into that second call's window. The
        // arithmetic is driven through the substitutable decrement, so D17 (the flag's own clear) is caught here.
        {
            uint32_t cnt = 0u;
            cnt = n48_ks_withdraw_enter(cnt);                      // hook_unmapVA #1 enters
            ck("E4 COUNTER: one withdrawal -> the keystone refuses", n48_ks_withdrawing(cnt), 1u);
            cnt = n48_ks_withdraw_enter(cnt);                      // hook_unmapVA #2 enters (overlapping)
            ck("E4 COUNTER: two overlapping withdrawals -> 2", cnt, 2u);
            cnt = gWithdrawLeave(cnt);                             // #1 finishes FIRST
            ck("E4 COUNTER: the FIRST finishing leaves it NONZERO", n48_ks_withdrawing(cnt), 1u);
            ck("E4 COUNTER: ...and the count is 1, not 0", cnt, 1u);
            cnt = gWithdrawLeave(cnt);                             // #2 finishes
            ck("E4 COUNTER: the second finishing -> 0", cnt, 0u);
            ck("E4 COUNTER: ...and the keystone may proceed", n48_ks_withdrawing(cnt), 0u);
            cnt = gWithdrawLeave(cnt);                             // a stray extra leave
            ck("E4 COUNTER: never below zero", cnt, 0u);
            ck("E4 COUNTER: leave at zero stays zero", n48_ks_withdraw_leave(0u), 0u);
        }

        // THE POST-WALK RE-CHECK (F1). Through the substitutable function, so a mutant that never withdraws is caught.
        ck("F1 POST-WALK: marker clear, root[511] unchanged -> NOT withdrawn",
           gPostWalk(0u, 1u, kWsEntry, kWsEntry), 0u);
        ck("F1 POST-WALK: the marker was set during the walks -> WITHDRAWN",
           gPostWalk(1u, 1u, kWsEntry, kWsEntry), 1u);
        ck("F1 POST-WALK: root[511] changed during the walks -> WITHDRAWN",
           gPostWalk(0u, 1u, 0ull, kWsEntry), 1u);
        ck("F1 POST-WALK: root[511] could not be re-read -> WITHDRAWN (fail closed)",
           gPostWalk(0u, 0u, 0ull, kWsEntry), 1u);

        // THE EXISTING GUARD G4 STILL REFUSES A FOREIGN SLOT. Independent of the keystone (which no longer reaches
        // it for this case), this is the property that made "no stale re-arm" safe: a foreign value is never written.
        {
            RootWriteInputs in;
            memset(&in, 0, sizeof(in));
            in.ctxSeq = k.rec_seq; in.ctxState = 1u; in.ctxIdentityOk = 1u;
            in.root = kWsRoot; in.arenaBot = 0x0ull; in.arenaTop = ~0ull;
            in.rootEnt0 = 0x0000003d00000001ull;          // a live root PDE pointing into the arena range
            in.rootEnt511 = 0x0000000deadbe000ull;        // the live foreign value
            in.ringMapBuilt = 1u; in.ringMapProbes = 1u; in.l1Off = 0x400000ull;
            in.clearWitnessKnown = 1u; in.rootAtTransition = kWsRoot; in.ent0AtTransition = 0u;
            ck("F1 GUARD: the EXISTING guard refuses a foreign slot at G4 (slot is not zero)",
               rootwrite_guard_check(&in), (uint64_t)kRootWriteG4);
            in.rootEnt511 = 0ull;                          // the same inputs with the slot ZERO
            ck("F1 GUARD: the SAME guard passes a zero slot - which is why the FIRST arm can write",
               rootwrite_guard_check(&in), (uint64_t)kRootWriteOk);
        }
    }

    // ---- 6. THE WALKS: THE ARM MAY NOT PROCEED IF EITHER REQUIRED VA FAILS ----
    {
        const n48_ks_in g = good();
        const uint32_t v = ev(&g);
        ck("WALKS: OK + written + verified + all four -> PERMITTED", pm(v, 0u, 2u, ack, 0xFu, 0xFu), 1u);
        ck("WALKS: bit 0 (SPI_ATTRIBUTE_RING_BASE) clear -> REFUSED", pm(v, 0u, 2u, ack, 0xEu, 0xFu), 0u);
        ck("WALKS: bit 1 (GE_POS_RING_BASE, arm3's fault VA) clear -> REFUSED", pm(v, 0u, 2u, ack, 0xDu, 0xFu), 0u);
        ck("WALKS: bit 2 (GE_PRIM_RING_BASE) clear -> REFUSED", pm(v, 0u, 2u, ack, 0xBu, 0xFu), 0u);
        ck("WALKS: bit 3 (the descriptor page) clear -> REFUSED", pm(v, 0u, 2u, ack, 0x7u, 0xFu), 0u);
        ck("WALKS: NO walk ran at all -> REFUSED", pm(v, 0u, 2u, ack, 0x0u, 0xFu), 0u);
        ck("WALKS: nothing required is not a gate -> REFUSED", pm(v, 0u, 2u, ack, 0xFu, 0x0u), 0u);
        ck("WRITE-BACK: status 3 (read back MISMATCHED) -> REFUSED", pm(v, 0u, 3u, ack, 0xFu, 0xFu), 0u);
        ck("WRITE-BACK: status 1 (permitted, not performed) -> REFUSED", pm(v, 0u, 1u, ack, 0xFu, 0xFu), 0u);
        ck("GUARD: a non-zero G-verdict -> REFUSED", pm(v, 4u, 2u, ack, 0xFu, 0xFu), 0u);
        ck("A REFUSING verdict is never permitted, whatever the walks say",
           pm(N48_KS_MOVED, 0u, 2u, ack, 0xFu, 0xFu), 0u);
    }

    // ---- 7. THE CROSS PRODUCT: A PERMITTED VERDICT IMPLIES EVERY CLAUSE ----
    {
        uint64_t okCases = 0;
        for (uint32_t m = 0; m < (1u << kNP); m++) {
            n48_ks_in k = good();
            for (uint32_t i = 0; i < kNP; i++) if (m & (1u << i)) kP[i].apply(k);
            const uint32_t v = ev(&k);
            gSweep++;
            if (m) gSweepNonVacuous++;
            if (v == N48_KS_OK) {
                okCases++;
                if (m != 0u) {
                    gFail++; gRun++;
                    if (!gQuiet) printf("  FAIL cross product: mask %#x returned OK with a clause falsified\n", m);
                }
            }
            // and nothing but OK may ever write
            if (n48_ks_write_needed(v) && v != N48_KS_OK) {
                gFail++; gRun++;
                if (!gQuiet) printf("  FAIL cross product: mask %#x writes on verdict %u\n", m, v);
            }
        }
        ck("CROSS PRODUCT: exactly ONE case of the whole product is OK (the unperturbed one)", okCases, 1u);
    }

    // ---- 8. INERTNESS: with COMMIT never armed, NOTHING happens, for any input whatsoever ----
    {
        uint64_t bad = 0;
        for (uint32_t arm = 0; arm <= 1u; arm++) {          /* N48_SD_ARM_OFF and _DECIDE */
            for (uint32_t m = 0; m < (1u << kNP); m++) {
                n48_ks_in k = good();
                for (uint32_t i = 0; i < kNP; i++) if (m & (1u << i)) kP[i].apply(k);
                n48_ks_in armed = k; armed.arm_now = 2u;    /* the same case, with COMMIT armed */
                k.arm_now = arm;
                const uint32_t v = ev(&k);
                gInert++;
                if (ev(&armed) != N48_KS_NOT_ARMED) gInertNonVacuous++;   /* the arm is what decided it */
                if (v != N48_KS_NOT_ARMED) bad++;
                if (n48_ks_write_needed(v) || n48_ks_mapping_claimed(v)) bad++;
                for (uint32_t st = 0; st <= 3u; st++)
                    for (uint32_t w = 0; w <= 0xFu; w++)
                        if (pm(v, 0u, st, ack, w, 0xFu)) bad++;
            }
        }
        ck("INERT: with COMMIT not armed, every case is NOT_ARMED and nothing writes or permits", bad, 0u);
    }

    // ---- 9. THE PRESENT BUCKETS (display_pipe_guard.h) ----
    {
        ck("PRESENT: status 0 is ok", bk(0u), (uint64_t)N48_DPG_PRESENT_OK);
        ck("PRESENT: status 11 (kScanStReadback) is NOT a refusal", bk(11u), (uint64_t)N48_DPG_PRESENT_READBACK);
        ck("PRESENT: status 10 (fence) IS a refusal", bk(10u), (uint64_t)N48_DPG_PRESENT_REFUSED);
        ck("PRESENT: status 12 (plan) IS a refusal", bk(12u), (uint64_t)N48_DPG_PRESENT_REFUSED);
        ck("PRESENT: the two sentinels the shim uses ARE refusals", bk(0xffffffffu),
           (uint64_t)N48_DPG_PRESENT_REFUSED);
        ck("PRESENT: ...and the other one", bk(0xfffffffeu), (uint64_t)N48_DPG_PRESENT_REFUSED);
        uint64_t ok = 0, rb = 0, ref = 0;
        for (uint32_t st = 0; st < 16u; st++) {
            switch (bk(st)) {
            case N48_DPG_PRESENT_OK: ok++; break;
            case N48_DPG_PRESENT_READBACK: rb++; break;
            default: ref++; break;
            }
        }
        ck("PRESENT: of the 16 scanout statuses exactly one is ok", ok, 1u);
        ck("PRESENT: exactly one is a readback disagreement", rb, 1u);
        ck("PRESENT: the other fourteen are refusals", ref, 14u);
        // arm3's own numbers: fifteen copies, fourteen clean, one whose readback differed.
        ck("PRESENT: arm3's boot DELIVERED 15", n48_dpg_delivered(14u, 1u), 15u);
    }

    // ---- 6. 0.0.384 — THE WITHDRAW/RE-ARM WINDOW'S IN-FLIGHT DEFERRAL ----
    //
    // THE PROPERTY, in the project's own words: a withdrawal that arrives while a committed frame is IN FLIGHT is
    // DEFERRED (root[511] is left standing, nothing is invalidated) and applied the moment the window ends; one that
    // arrives OUT of flight applies immediately; the timeout is a bound, not a hint; and a torn record or an unusable
    // clock withdraws exactly as 0.0.383 did. Both directions are checked, and the default (switch OFF) is proved
    // over the WHOLE cross product rather than by example - because "default = today's behaviour" is the one claim
    // a reviewer cannot re-derive from a log.
    {
        const uint64_t kT = 2000000ull;              /* the kext's kKsFlightUs */
        const uint64_t kStart = 1000000ull;          /* an arbitrary non-zero commit stamp */
        auto flight = [&](void) {
            n48_ksd_in d {};
            d.on = 1u; d.armed = 1u; d.flight = 1u;
            d.start_us = kStart; d.now_ok = 1u; d.now_us = kStart + 1000ull;   /* 1 ms in */
            d.eop = 0u; d.timeout_us = kT;
            return d;
        };

        // 6a. THE POSITIVE CONTROL. Without it every refusal below is vacuous.
        {
            const n48_ksd_in d = flight();
            ck("DEFER: POSITIVE CONTROL - in flight, 1 ms in, no end of pipe -> DEFER", df(&d), N48_KSD_DEFER);
            ck("DEFER: ...and that verdict LEAVES root[511] STANDING", n48_ksd_defer(df(&d)), 1u);
            ck("DEFER: ...and it does NOT end the flight", n48_ksd_ends_flight(df(&d)), 0u);
        }

        // 6b. THE DEFAULT IS TODAY'S BEHAVIOUR, and it is the FIRST clause after the null check.
        {
            n48_ksd_in d = flight();
            d.on = 0u;
            ck("DEFER: the switch OFF -> WITHDRAW NOW (0.0.383's path)", df(&d), N48_KSD_NOW_OFF);
            ck("DEFER: ...and nothing is ever deferred with it off", n48_ksd_defer(df(&d)), 0u);
        }

        // 6c. A WITHDRAWAL ARRIVING OUT OF FLIGHT APPLIES IMMEDIATELY.
        {
            n48_ksd_in d = flight();
            d.flight = 0u;
            ck("DEFER: nothing committed is in flight -> WITHDRAW NOW", df(&d), N48_KSD_NOW_NOT_IN_FLIGHT);
            ck("DEFER: ...immediately, not deferred", n48_ksd_defer(df(&d)), 0u);
        }
        {
            n48_ksd_in d = flight();
            d.armed = 0u;
            ck("DEFER: nothing of ours is standing -> WITHDRAW NOW", df(&d), N48_KSD_NOW_NOTHING);
            ck("DEFER: ...immediately, not deferred", n48_ksd_defer(df(&d)), 0u);
        }

        // 6d. END OF PIPE ENDS THE WINDOW, and it is the reason reported when it and the timeout both hold.
        {
            n48_ksd_in d = flight();
            d.eop = 1u;
            ck("DEFER: end of pipe OBSERVED -> WITHDRAW NOW", df(&d), N48_KSD_NOW_EOP);
            ck("DEFER: ...and that ENDS the flight (a recorded request is applied)", n48_ksd_ends_flight(df(&d)), 1u);
            d.now_us = kStart + kT + 1000ull;        /* the bound has also passed */
            ck("DEFER: end of pipe WINS over the timeout when both hold", df(&d), N48_KSD_NOW_EOP);
        }

        // 6e. THE TIMEOUT IS A BOUND, AND THE BOUNDARY IS CHECKED ON BOTH SIDES.
        {
            n48_ksd_in d = flight();
            d.now_us = kStart + kT - 1ull;
            ck("DEFER: one microsecond INSIDE the bound -> still DEFER", df(&d), N48_KSD_DEFER);
            d.now_us = kStart + kT;
            ck("DEFER: exactly AT the bound -> WITHDRAW NOW (timeout)", df(&d), N48_KSD_NOW_TIMEOUT);
            ck("DEFER: ...and that ENDS the flight", n48_ksd_ends_flight(df(&d)), 1u);
            d.now_us = kStart + kT + 8000000ull;     /* arm10's hang was at +8 s */
            ck("DEFER: long past the bound -> WITHDRAW NOW (timeout)", df(&d), N48_KSD_NOW_TIMEOUT);
        }

        // 6f. TORN STATE AND AN UNUSABLE CLOCK WITHDRAW AS TODAY. Each is its own falsification.
        {
            n48_ksd_in d = flight(); d.start_us = 0ull;
            ck("DEFER: TORN - `flight` set with no stamp behind it -> WITHDRAW NOW", df(&d), N48_KSD_NOW_TORN);
            ck("DEFER: ...and it is NOT deferred", n48_ksd_defer(df(&d)), 0u);
        }
        {
            n48_ksd_in d = flight(); d.now_ok = 0u;
            ck("DEFER: the clock could not be read -> WITHDRAW NOW", df(&d), N48_KSD_NOW_TORN);
        }
        {
            n48_ksd_in d = flight(); d.now_us = kStart - 1ull;
            ck("DEFER: the clock went BACKWARDS -> WITHDRAW NOW", df(&d), N48_KSD_NOW_TORN);
        }
        {
            n48_ksd_in d = flight(); d.timeout_us = 0ull;
            ck("DEFER: a ZERO bound is not a bound -> WITHDRAW NOW", df(&d), N48_KSD_NOW_TORN);
        }
        ck("DEFER: a null input is not knowledge -> WITHDRAW NOW", df(nullptr), N48_KSD_NOW_TORN);
        ck("DEFER: ...and a null input never defers", n48_ksd_defer(df(nullptr)), 0u);

        // 6g. THE SEQUENCE arm10 ACTUALLY LOGGED: three fires inside the window, then one after it. This is the
        //     mechanism check - fires #5/#6/#7 on the committing context each cleared root[511] and invalidated
        //     VMID 2's TLB with the frame still executing. Under the rule all three defer and the fourth applies.
        {
            const uint64_t at[4] = { kStart + 100000ull, kStart + 400000ull, kStart + 900000ull,
                                     kStart + kT + 1ull };
            uint32_t deferred = 0, applied = 0, pending = 0;
            for (int i = 0; i < 4; i++) {
                n48_ksd_in d = flight();
                d.now_us = at[i];
                const uint32_t r = df(&d);
                if (n48_ksd_defer(r)) { deferred++; pending = 1u; }
                else if (pending) { applied++; pending = 0u; }
            }
            ck("DEFER: arm10's three in-window fires are ALL deferred", deferred, 3u);
            ck("DEFER: ...and the first fire after the window APPLIES the request", applied, 1u);
            ck("DEFER: ...leaving nothing pending", pending, 0u);
        }

        // 6h. THE CROSS PRODUCT. 512 cases over the nine things that can be false, checked BOTH ways: DEFER implies
        //     every one of them holds, and with the switch OFF every single case answers NOT-A-DEFERRAL.
        gDSweep = gDSweepDefer = gDInert = gDInertNonVacuous = 0;
        {
            uint64_t implied = 0, deferrals = 0;
            for (uint32_t m = 0; m < 512u; m++) {
                n48_ksd_in d = flight();
                if (m & 1u)   d.armed = 0u;
                if (m & 2u)   d.flight = 0u;
                if (m & 4u)   d.start_us = 0ull;
                if (m & 8u)   d.now_ok = 0u;
                if (m & 16u)  d.now_us = d.start_us ? d.start_us - 1ull : 0ull;
                if (m & 32u)  d.timeout_us = 0ull;
                if (m & 64u)  d.eop = 1u;
                if (m & 128u) d.now_us = kStart + kT + 1ull;
                if (m & 256u) d.now_us = kStart + kT;
                const uint32_t r = df(&d);
                gDSweep++;
                if (n48_ksd_defer(r)) {
                    deferrals++; gDSweepDefer++;
                    /* THE IMPLICATION, the half a single example cannot prove. */
                    if (d.on && d.armed && d.flight && d.start_us && d.now_ok && d.now_us >= d.start_us &&
                        d.timeout_us && !d.eop && (d.now_us - d.start_us) < d.timeout_us)
                        implied++;
                }
                n48_ksd_in off = d; off.on = 0u;
                const uint32_t ro = df(&off);
                gDInert++;
                if (!n48_ksd_defer(ro)) gDInertNonVacuous++;
            }
            ck("DEFER: the cross product produced deferrals at all (non-vacuous)", deferrals > 0u, 1u);
            ck("DEFER: EVERY deferral implies all nine conditions", implied, deferrals);
            ck("DEFER: with the switch OFF not ONE of the 512 cases defers", gDInertNonVacuous, gDInert);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// 0.0.423 (KEYSTONE-A-PRIME) — H1 AND H2. THE MARKER'S EARLY RELEASE AND THE INTERLEAVING MODEL.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// H1: `n48_ks_withdraw_hold` must RELEASE the withdrawal marker for exactly ONE of the seven deferral verdicts
// (N48_KSD_DEFER, which clears nothing) and HOLD it for the other six — the fail-closed "withdraw as today" answers,
// which carry exactly the same danger as the withdrawal itself. It is substitutable so the two degenerate holds (D18,
// always 1 = 0.0.411's over-refusal; D19, always 0 = option (b)) are planted breaks.
typedef uint32_t (*HoldFn)(uint32_t);
static uint32_t hold_always_one(uint32_t)  { return 1u; }
static uint32_t hold_always_zero(uint32_t) { return 0u; }

static void h1_withdraw_hold(HoldFn hold)
{
    uint32_t releases = 0;
    for (uint32_t r = 0; r < N48_KSD_REASONS; r++) {
        const uint32_t want = (r == N48_KSD_DEFER) ? 0u : 1u;
        char b[200];
        snprintf(b, sizeof(b), "H1 HOLD: verdict %u (%s) -> hold %u", r, n48_ksd_name(r), want);
        ck(b, hold(r), want);
        if (hold(r) == 0u) releases++;
    }
    ck("H1 HOLD: exactly ONE of the seven deferral verdicts releases the marker", releases, 1u);
    ck("H1 HOLD: that one is N48_KSD_DEFER", hold(N48_KSD_DEFER), 0u);
    ck("H1 HOLD: an out-of-range verdict HOLDS (never releases by accident)", hold(0xFFFFFFFFu), 1u);
}

// H2: AN EXHAUSTIVE INTERLEAVING MODEL. One unmap call — entry E, decision D, clear C, Apple's unmapVA A, re-arm R,
// exit X — interleaved with one keystone attempt — flight start G, eval V, write W, the four walks, post-walk re-check
// P — and the flight's end F, over EVERY order consistent with each thread's program order (5544 of them). The keystone
// steps go through the REAL `n48_ks_eval` and `n48_ks_post_walk_withdrawn`; the unmap's decision goes through the REAL
// `n48_ksd_eval`. The marker is set at E (EARLY, the real rule) or at C (LATE, option (a) as the design words it:
// "mark late", so a call pre-empted between D and its late mark lets the readers see 0). `hold` is the substitutable
// `n48_ks_withdraw_hold`. A HAZARD is a PERMITTED frame with a clear that begins after the post-walk check P and before
// the flight's end F (the design's "clear between V and the end of the flight": a clear between V and P is caught by P,
// and a clear re-armed before P leaves the slot armed when the frame runs). The ONE exception the design names is the
// post-P clear with the deferral OFF, which is today's behaviour and is asserted against the unconditional hold
// (hold_always_one) rather than against the new rule.
enum { EV_E, EV_D, EV_C, EV_A, EV_R, EV_X, EV_G, EV_V, EV_W, EV_K, EV_P, EV_F };

typedef struct {
    uint32_t verdictV, permit, withdrew;   // withdrew = a clear happened (the call did not decide DEFER)
    int ePos, dPos, cPos, vPos, pPos, fPos;
    uint32_t postWithdrawn;
    /* 0.0.426 (review follow-up, Part B1) — THE held / !held GUARDS.: "Every call releases the marker exactly
     * once". `enters` counts n48_ks_withdraw_enter calls and `releases` the leave calls; the guard is that they are EQUAL
     * for every ordering. An unconditional double release (a leave at BOTH D and X, or a release on a verdict the call
     * did not hold) makes releases != enters, and the check in h2_safety fails. */
    uint32_t enters, releases;
} H2Run;

static H2Run h2_run(const int *order, int n, uint32_t on, uint32_t setLate, HoldFn hold)
{
    H2Run r;
    memset(&r, 0, sizeof(r));
    r.ePos = r.dPos = r.cPos = r.vPos = r.pPos = r.fPos = -1;
    uint32_t marker = 0u, flight = 0u, eop = 0u, withdraw = 0u, held = 1u, claimed = 0u, post = 0u;
    const uint32_t wrote = 1u;                 // the standing entry every fixture begins from (VERDICT 11)
    uint64_t slot = kWsEntry;
    for (int i = 0; i < n; i++) {
        switch (order[i]) {
        case EV_G: flight = 1u; break;
        case EV_F: flight = 0u; eop = 1u; r.fPos = i; break;
        case EV_E:
            r.ePos = i;
            if (!setLate) { marker = n48_ks_withdraw_enter(marker); r.enters++; }
            break;
        case EV_D: {
            r.dPos = i;
            n48_ksd_in d;
            memset(&d, 0, sizeof(d));
            d.on = on; d.armed = wrote;
            d.flight = flight; d.start_us = 1000ull; d.now_ok = 1u; d.now_us = 2000ull;
            d.eop = eop; d.timeout_us = 2000ull;
            const uint32_t v = n48_ksd_eval(&d);
            held = hold(v);
            withdraw = (v != N48_KSD_DEFER) ? 1u : 0u;
            if (!setLate && !held) { marker = n48_ks_withdraw_leave(marker); r.releases++; }
            break;
        }
        case EV_C:
            if (withdraw) {
                slot = 0ull; r.withdrew = 1u; r.cPos = i;
                if (setLate) { marker = n48_ks_withdraw_enter(marker); r.enters++; }   // option (a): the LATE mark, at the clear
            }
            break;
        case EV_A: break;   // Apple's unmapVA: the safety model has the root SURVIVE (the free case is the residual)
        case EV_R: if (withdraw) slot = kWsEntry; break;
        case EV_X:
            if (setLate) { if (withdraw && marker) { marker = n48_ks_withdraw_leave(marker); r.releases++; } }
            else if (held) { marker = n48_ks_withdraw_leave(marker); r.releases++; }
            break;
        case EV_V: {
            r.vPos = i;
            n48_ks_in k;
            memset(&k, 0, sizeof(k));
            k.arm_now = 2u; k.arm_commit = 2u; k.token_matched = 1u; k.gate_seq = 1u;
            k.frame_verdict = 0u; k.frame_vmid = 2u; k.frame_ws_seq = 5u;
            k.frame_ws_ctx = kWsCtx; k.frame_ws_root = kWsRoot;
            k.now_state = 1u; k.now_complete = 1u; k.now_seq = 5u; k.now_vmid = 2u;
            k.now_ctx = kWsCtx; k.now_root = kWsRoot; k.now_hw_ok = 1u; k.now_hw_root = kWsRoot;
            k.rec_found = 1u; k.rec_live = 1u; k.rec_id_ok = 1u; k.rec_seq = 5u;
            k.rec_ctx = kWsCtx; k.rec_root = kWsRoot;
            k.rec_wrote = wrote; k.rec_withdrawn = 0u;
            k.rec_slot_ok = 1u; k.rec_slot_live = slot; k.rec_written = kWsEntry;
            k.rec_withdrawing = marker;
            k.attempts = 0u; k.ringmap_built = 1u; k.armed_contexts = 0u; k.armed_cap = 8u;
            r.verdictV = n48_ks_eval(&k);
            claimed = n48_ks_mapping_claimed(r.verdictV);
            if (n48_ks_write_needed(r.verdictV)) slot = kWsEntry;   // W: only the OK verdict writes
            break;
        }
        case EV_W: break;   // the write is the effect n48_ks_eval decides; folded into EV_V
        case EV_K: break;   // the four walks resolve whenever the slot is our entry
        case EV_P:
            r.pPos = i;
            post = n48_ks_post_walk_withdrawn(marker, 1u, slot, kWsEntry);
            r.postWithdrawn = post;
            break;
        }
    }
    r.permit = (claimed && !post) ? 1u : 0u;
    return r;
}

typedef struct { uint64_t total, hazards, known, other, preempt, relBad, entTotal; } H2Counts;
static H2Counts gH2On {}, gH2Off {};

static void h2_enum_rec(int ui, int ki, int fi, int *order, int n, uint32_t on, uint32_t setLate, HoldFn hold,
                        H2Counts *c)
{
    static const int umap[6] = { EV_E, EV_D, EV_C, EV_A, EV_R, EV_X };
    static const int kst[5]  = { EV_G, EV_V, EV_W, EV_K, EV_P };
    if (ui == 6 && ki == 5 && fi == 1) {
        const H2Run r = h2_run(order, n, on, setLate, hold);
        c->total++;
        /* 0.0.426 (Part B1): the held / !held guard. Every call must enter and release the marker the SAME number of times;
         * an unconditional release (at both D and X, or on a verdict the call did not hold) breaks this. */
        c->entTotal += r.enters;
        if (r.releases != r.enters) c->relBad++;
        /* A HAZARD: a PERMITTED frame with a clear that BEGINS AFTER the post-walk check and before the flight ends.
         * That is the only clear a permitted frame can meet: a clear between V and P is caught by P (so permit is 0),
         * and a clear that is re-armed before P leaves root[511] armed when the frame starts, which is safe — the
         * keystone runs before the doorbell, so the frame executes after P. This is the design's "clear between V and
         * the end of the flight" narrowed to the clears the post-walk check cannot see. */
        if (r.permit && r.cPos >= 0 && r.pPos >= 0 && r.fPos >= 0 && r.cPos > r.pPos && r.cPos < r.fPos) {
            c->hazards++;
            /* THE ONE NAMED EXCEPTION: a clear that begins after the post-walk check, with the deferral OFF. With the
             * switch off n48_ks_withdraw_hold answers 1 for every verdict, so A′ IS today here - the assertion below
             * that this count equals hold_always_one's is what says A′ changed nothing on that path. */
            if (!on && r.ePos > r.pPos) c->known++;
            else {
                c->other++;
                /* THE PRE-EMPTED-MARK SHAPE (option (a)): a decision landed before P but its mark had not, and the
                 * clear fell after P. D20's check demands this shape exists. */
                if (r.dPos >= 0 && r.pPos >= 0 && r.dPos < r.pPos && r.cPos > r.pPos) c->preempt++;
            }
        }
        return;
    }
    if (ui < 6) { order[n] = umap[ui]; h2_enum_rec(ui + 1, ki, fi, order, n + 1, on, setLate, hold, c); }
    if (ki < 5) { order[n] = kst[ki];  h2_enum_rec(ui, ki + 1, fi, order, n + 1, on, setLate, hold, c); }
    if (fi < 1) { order[n] = EV_F;      h2_enum_rec(ui, ki, fi + 1, order, n + 1, on, setLate, hold, c); }
}

static H2Counts h2_enum(uint32_t on, uint32_t setLate, HoldFn hold)
{
    H2Counts c;
    memset(&c, 0, sizeof(c));
    int order[16];
    h2_enum_rec(0, 0, 0, order, 0, on, setLate, hold, &c);
    return c;
}

// A named fixture: a hand-built ordering with the verdict and permit the design owes it.
static void h2_fixture(const char *name, const int *ev, int n, uint32_t on, uint32_t setLate, HoldFn hold,
                       uint32_t wantPermit, uint32_t wantWithdrawing)
{
    const H2Run r = h2_run(ev, n, on, setLate, hold);
    char b[200];
    if (wantPermit) {
        snprintf(b, sizeof(b), "H2 FIXTURE: %s must COMMIT", name);
        ck(b, r.permit, 1u);
    }
    if (wantWithdrawing) {
        snprintf(b, sizeof(b), "H2 FIXTURE: %s must REFUSE with VERDICT 15 (withdrawal in progress)", name);
        ck(b, r.verdictV, (uint64_t)N48_KS_WITHDRAWING);
        snprintf(b, sizeof(b), "H2 FIXTURE: %s writes NOTHING and never permits", name);
        ck(b, r.permit, 0u);
    }
    if (!wantPermit && !wantWithdrawing) {
        snprintf(b, sizeof(b), "H2 FIXTURE: %s must REFUSE (the accepted residual)", name);
        ck(b, r.permit, 0u);
        ck("H2 FIXTURE: ...and the POST-WALK re-check is what withdraws it", r.postWithdrawn, 1u);
    }
}

static void h2_fixtures(uint32_t setLate, HoldFn hold)
{
    // arm34 seq 5 must COMMIT, under BOTH attributions leaves open (which call set the marker is INFERRED).
    { const int ev[] = { EV_G, EV_V, EV_E, EV_D, EV_P, EV_F };          // V < E < D < P: #32 decided before P
      h2_fixture("arm34 seq 5 (#32's deferral decided BEFORE the post-walk check)", ev, 6, 1u, setLate, hold, 1u, 0u); }
    { const int ev[] = { EV_G, EV_E, EV_D, EV_V, EV_P, EV_F };          // E < D < V < P: the #31 attribution
      h2_fixture("arm34 seq 5 (attribution #31, the deferral already released)", ev, 6, 1u, setLate, hold, 1u, 0u); }
    // arm27 seq 5 is the same deferred-only shape and must COMMIT.
    { const int ev[] = { EV_G, EV_V, EV_E, EV_D, EV_P, EV_F };
      h2_fixture("arm27 seq 5 (the only nearby unmap was DEFERRED)", ev, 6, 1u, setLate, hold, 1u, 0u); }
    // arm34 seq 6 and arm28/30/32/33 (: all five VERDICT 15s were REAL withdrawals) must REFUSE. The deferral was
    // ON in those runs; the call withdrew because no committed frame was in flight, so D answers NOT_IN_FLIGHT.
    { const int ev[] = { EV_E, EV_D, EV_V, EV_C, EV_R, EV_X, EV_F };
      h2_fixture("arm34 seq 6 / arm28/30/32/33 (a REAL withdrawal)", ev, 7, 1u, setLate, hold, 0u, 1u); }
    // (arm26 f16): the keystone ran while hook_unmapVA was inside a withdrawal; it must refuse.
    { const int ev[] = { EV_E, EV_D, EV_V, EV_C, EV_R, EV_X, EV_F };
      h2_fixture(" arm26 f16 (the keystone ran inside a withdrawal)", ev, 7, 1u, setLate, hold, 0u, 1u); }
    // THE ACCEPTED RESIDUAL: a DEFERRED call's post-walk check lands between its entry and its decision, so the
    // marker is still held and the frame is refused although nothing threatens it. This is the residual A′ accepts.
    { const int ev[] = { EV_G, EV_E, EV_P, EV_D, EV_F };
      h2_fixture("the accepted residual (a deferred call's P between its E and its D)", ev, 5, 1u, setLate, hold, 0u, 0u); }
}

static void h2_safety(uint32_t setLate, HoldFn hold)
{
    // THE PROPERTY, deferral ON: never PERMITTED with a clear between V and the end of the flight.
    gH2On = h2_enum(1u, setLate, hold);
    ck("H2 SAFETY: the enumeration is COMPLETE (5544 orderings)", gH2On.total, 5544u);
    ck("H2 SAFETY (deferral ON): NOT ONE ordering permits with a clear after the post-walk check", gH2On.hazards, 0u);
    // 0.0.426 (Part B1): THE held / !held GUARD, pinned. For EVERY ordering every unmap call ENTERS and RELEASES the marker
    // the SAME number of times — the property's review states as "every call releases the marker exactly once". An
    // unconditional double release (a leave at D AND at X, or a leave on a verdict the call did not hold) makes
    // releases > enters here and this check fails. Non-vacuous: the deferral-ON enumeration holds marker entries.
    ck("H2 GUARD: every call enters and releases the marker the SAME number of times", gH2On.relBad, 0u);
    ck("H2 GUARD: ...and the enumeration is NOT VACUOUS (calls that held the marker)", gH2On.entTotal > 0u ? 1u : 0u, 1u);
    // deferral OFF: the one exception, the post-P clear, is all that is left — and it is today's behaviour.
    gH2Off = h2_enum(0u, setLate, hold);
    ck("H2 SAFETY (deferral OFF): every hazard is the known post-P hole (no NEW one)", gH2Off.other, 0u);
    ck("H2 SAFETY (deferral OFF): the known post-P hole is NON-VACUOUS", gH2Off.known > 0u ? 1u : 0u, 1u);
    const H2Counts today = h2_enum(0u, setLate, hold_always_one);
    ck("H2 SAFETY (deferral OFF): the known hole is EXACTLY today's behaviour (same count)", gH2Off.known, today.known);
    ck("H2 SAFETY (deferral OFF): ...and today has no OTHER hazard either", today.other, 0u);
}

static void h2_checks(uint32_t setLate, HoldFn hold)
{
    h1_withdraw_hold(hold);
    h2_fixtures(setLate, hold);
    h2_safety(setLate, hold);
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// 0.0.431 — H2 EXTENSION: THE REFUSAL RESTORES THE PREVIOUS FLIGHT, IT DOES NOT CLEAR IT.
// 0.0.430's rule ("a frame it REFUSES was never submitted, so it is not in flight") is right about THIS frame and was
// WRONG about the record. `gKsFlight` is ONE record and the gate stamps it BEFORE the keystone runs; at budget > 1 the
// refused candidate B stamped OVER a committed frame A that may still be executing. Clearing `active` then made
// hook_unmapVA answer NOT_IN_FLIGHT and let an unmap WITHDRAW root[511] under A - the arm3/arm10 NACK-and-hang class,
// and a NEW way in, because before 0.0.430 B's leftover stamp incidentally covered A for its bound.
// The fix STASHES the outgoing record at the stamp (`n48_ks_flight_save`) and RESTORES it at the refusal when its
// own stamp is still the one showing (`n48_ks_flight_restore_ok`). This model interleaves F_A (A's end of pipe
// observed), G (B's stamp), V (B's keystone refusal) and Q (the update) with U (an unmap's decision through the REAL
// `n48_ksd_eval`). A HAZARD is a clear (U answering anything but DEFER) that lands BEFORE A's end of pipe with A
// present. The real restore must find ZERO; the planted D23 (0.0.430's clear) must find at least one. Controls:
// F_A before G -> NOW_EOP (the restored seq answers A's own sticky latch,'s gain kept); no earlier flight ->
// NOT_IN_FLIGHT (identical to 0.0.430's clear); A past its limit -> TIMEOUT.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
enum { EV_FA, EV_PG, EV_PV, EV_PQ, EV_PU };

typedef n48_ks_flight (*FlightUpdateFn)(n48_ks_flight, uint32_t, n48_ks_flight);
/* THE REAL UPDATE: restore the stash when THIS call's stamp is the one showing; otherwise leave the live record. */
static n48_ks_flight flight_update_real(n48_ks_flight saved, uint32_t callSeq, n48_ks_flight cur)
{
    return n48_ks_flight_restore_ok(saved, callSeq, cur.seq) ? saved : cur;
}
/* D23 — 0.0.430 VERBATIM: clear `active` on the refusal, whatever the stash said. */
static n48_ks_flight flight_update_d23(n48_ks_flight, uint32_t, n48_ks_flight cur)
{
    cur.active = 0u;
    return cur;
}
static FlightUpdateFn gFlightUpdate = &flight_update_real;

typedef struct {
    uint32_t verdictU, withdrew, hazard;
    int uPos, faPos;
} PfaRun;

static PfaRun pfa_run(const int *order, int n, uint32_t prevActive, uint64_t nowOffset)
{
    const uint64_t atA = 1000000ull, atB = 1500000ull, kT = 2000000ull;
    const uint32_t seqA = 7u, seqB = 8u;
    n48_ks_flight prev;
    memset(&prev, 0, sizeof(prev));
    prev.at_us = atA; prev.seq = seqA; prev.active = prevActive;
    n48_ks_flight saved;
    memset(&saved, 0, sizeof(saved));
    n48_ks_flight cur = prev;                 // before the stamp the record is A's, or empty with no A
    uint32_t faSeen = 0u;
    const uint32_t committedFlight = seqA;    // A is the last PROMOTED commit; eop only answers for the flight it belongs to
    PfaRun r;
    memset(&r, 0, sizeof(r));
    r.uPos = r.faPos = -1;
    for (int i = 0; i < n; i++) {
        switch (order[i]) {
        case EV_FA: faSeen = 1u; r.faPos = i; break;
        case EV_PG: saved = n48_ks_flight_save(prev, seqB);   // THE STASH of the outgoing record
                    cur.at_us = atB; cur.seq = seqB; cur.active = 1u; cur.stamp_seq = 0u; break;
        case EV_PV: break;                                     // the keystone refuses B; the refusal branch runs at Q
        case EV_PQ: cur = gFlightUpdate(saved, seqB, cur); break;   // THE UPDATE (restore, or D23's clear)
        case EV_PU: {
            r.uPos = i;
            n48_ksd_in d;
            memset(&d, 0, sizeof(d));
            d.on = 1u; d.armed = 1u;
            d.flight = cur.active;
            d.start_us = cur.at_us;
            d.now_ok = 1u; d.now_us = cur.at_us + nowOffset;
            d.eop = (cur.active && cur.seq == committedFlight && faSeen) ? 1u : 0u;
            d.timeout_us = kT;
            r.verdictU = n48_ksd_eval(&d);
            r.withdrew = n48_ksd_defer(r.verdictU) ? 0u : 1u;
            break;
        }
        }
    }
    r.hazard = (prevActive && r.withdrew && r.faPos >= 0 && r.uPos < r.faPos) ? 1u : 0u;
    return r;
}

typedef struct { uint64_t total, hazards; } PfaCounts;

static void pfa_enum_rec(int gi, int fi, int ui, int *order, int n, uint32_t prevActive, uint64_t nowOffset,
                         PfaCounts *c)
{
    static const int seq3[3] = { EV_PG, EV_PV, EV_PQ };   /* the committing thread's program order G < V < Q */
    if (gi == 3 && fi == 1 && ui == 1) {
        const PfaRun r = pfa_run(order, n, prevActive, nowOffset);
        c->total++;
        if (r.hazard) c->hazards++;
        return;
    }
    if (gi < 3) { order[n] = seq3[gi]; pfa_enum_rec(gi + 1, fi, ui, order, n + 1, prevActive, nowOffset, c); }
    if (fi < 1) { order[n] = EV_FA;    pfa_enum_rec(gi, fi + 1, ui, order, n + 1, prevActive, nowOffset, c); }
    if (ui < 1) { order[n] = EV_PU;    pfa_enum_rec(gi, fi, ui + 1, order, n + 1, prevActive, nowOffset, c); }
}

static PfaCounts pfa_enum(uint32_t prevActive, uint64_t nowOffset)
{
    PfaCounts c;
    memset(&c, 0, sizeof(c));
    int order[8];
    pfa_enum_rec(0, 0, 0, order, 0, prevActive, nowOffset, &c);
    return c;
}

static void pfa_checks()
{
    const uint64_t kT = 2000000ull;
    // THE REAL RESTORE, with A present: over EVERY ordering not one clear lands before A's end of pipe.
    const PfaCounts on = pfa_enum(1u, 1000ull);
    ck(": the previous-flight enumeration is COMPLETE (20 orderings)", on.total, 20u);
    ck(" RESTORE: A present -> ZERO clears before A's end of pipe", on.hazards, 0u);
    // NON-VACUITY: with no earlier flight there is nothing to protect, so no ordering is a hazard.
    const PfaCounts none = pfa_enum(0u, 1000ull);
    ck(": with NO earlier flight not one ordering is a hazard (nothing to protect)", none.hazards, 0u);
    // CONTROL 1: A's end of pipe was observed BEFORE the stamp -> the restored seq answers A's own latch: NOW_EOP.
    { const int ev[] = { EV_FA, EV_PG, EV_PV, EV_PQ, EV_PU };
      const PfaRun r = pfa_run(ev, 5, 1u, 1000ull);
      ck(" CONTROL: A's EOP before the stamp -> NOW_EOP (the restored seq keeps's gain)", r.verdictU,
         (uint64_t)N48_KSD_NOW_EOP); }
    // CONTROL 2: NO earlier flight -> the restore leaves active 0: 0.0.430's clear exactly.
    { const int ev[] = { EV_PG, EV_PV, EV_PQ, EV_PU };
      const PfaRun r = pfa_run(ev, 4, 0u, 1000ull);
      ck(" CONTROL: no earlier flight -> NOT_IN_FLIGHT (identical to 0.0.430's clear)", r.verdictU,
         (uint64_t)N48_KSD_NOW_NOT_IN_FLIGHT); }
    // CONTROL 3: A past its bound with no end of pipe -> the bounded timeout, unchanged.
    { const int ev[] = { EV_PG, EV_PV, EV_PQ, EV_PU };
      const PfaRun r = pfa_run(ev, 4, 1u, kT + 1ull);
      ck(" CONTROL: A past its limit -> TIMEOUT (the bound still ends the window)", r.verdictU,
         (uint64_t)N48_KSD_NOW_TIMEOUT); }
}

// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// C5 part 2 (build 0.0.460, notes/design/C5-CONTINUOUS.md Q6) — H2 EXTENDED TO TWO COMMITS PLUS A REFUSAL.
//
// h2_checks/pfa_checks above drive gfx_keystone.h's own SINGLE-RECORD primitives (n48_ks_flight,
// n48_ks_flight_save/_restore_ok) — the pre-ring mechanism this file keeps exercising for the reasons
// gfx_flightring.h's own header states ("the pure save/restore helpers stay in gfx_keystone.h, unused by the kext
// from this build on"). What the KEXT actually runs today is the ring (gfx_flightring.h), where's single
// shared record became TWO INDEPENDENT ENTRIES (Q1: "there is no longer a single record for a later stamp to
// clobber"). This drives the REAL ring functions, in the REAL sequence gfxsrc_commit_try/hook_gfxCommitIB/
// hook_unmapVA use — push, commit-mark, a SECOND push, that second commit's OWN keystone REFUSAL (freeing only
// its own entry), the first commit's retirement, then the withdrawal verdict — and proves two things's old
// model asserted about ONE record are still true of the RING's two: (1) a later commit's own refusal never
// touches an earlier, still-live entry; (2) the refusal's "newest flight" restore (0.0.446  fix (1))
// still answers NOW_EOP for the surviving (now newest again) commit once IT retires, not NOT_IN_FLIGHT.
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════════
static void h2_two_commits_plus_refusal()
{
    n48_fr_ring r; n48_fr_reset(&r);
    const uint32_t seqA = 500u, seqB = 501u;
    const uint64_t atA = 1000ull, atB = 1200ull;   // B's gate stamp lands AFTER A's, real ordering
    const uint32_t ordA = 9u, ordB = 10u;
    const uint64_t voA = 0x1000ull, voB = 0x2000ull;
    const uint32_t wantA = 0xAAAAu, wantB = 0xBBBBu;

    // Step 1: A is pushed at its gate stamp and the keystone proves it will run (PENDING -> COMMITTED), exactly as
    // gfxsrc_commit_try's push and hook_gfxCommitIB's n48_fr_commit_mark do for the first of two commits in flight.
    uint32_t idxA = N48_FR_CAPACITY;
    ck("H2-2C: A pushes", n48_fr_push(&r, seqA, atA, ordA, voA, wantA, &idxA), 1u);
    uint32_t lastFlight = 0u;
    ck("H2-2C: A's keystone marks it COMMITTED (n48_fr_commit_mark)", n48_fr_commit_mark(&r, seqA, &lastFlight), 1u);
    ck("H2-2C: 'the newest flight' now names A", lastFlight, seqA);

    // Step 2: a SECOND commit, B, reaches the gate stamp and is pushed while A is STILL COMMITTED and in flight -
    // budget > 1 / a continuous arm's own shape (Q1: "AT BUDGET > 1 a SECOND push here adds a SECOND entry rather
    // than overwriting the first").
    uint32_t idxB = N48_FR_CAPACITY;
    ck("H2-2C: B pushes as a SECOND, independent entry", n48_fr_push(&r, seqB, atB, ordB, voB, wantB, &idxB), 1u);
    ck("H2-2C: A's own entry is UNCHANGED by B's push (still COMMITTED)", r.e[idxA].state, (uint32_t)N48_FR_COMMITTED);
    ck("H2-2C: the ring now reports 2 live (non-FREE) entries", (uint64_t)(r.e[idxA].state != N48_FR_FREE) +
       (uint64_t)(r.e[idxB].state != N48_FR_FREE), 2ull);
    // A THIRD commit's push must be able to see BOTH A and B still live - the withdrawal decision the whole
    // mechanism exists for. n48_fr_defer_verdict is asked here exactly as hook_unmapVA asks it.
    { uint32_t bSeq = 0u; uint64_t bAt = 0ull;
      const uint32_t v = n48_fr_defer_verdict(&r, 1u, 1u, 1500ull, 100000ull, &bSeq, &bAt);
      ck("H2-2C: with A COMMITTED and B PENDING, the ring DEFERS (both live)", v, (uint64_t)N48_KSD_DEFER); }

    // Step 3: B's OWN keystone attempt is REFUSED (the arm3/ class, or simply a mismatched frame at HEAD) -
    // hook_gfxCommitIB's refusal branch frees ONLY the entry the refused commit itself pushed (item 2).
    ck("H2-2C: B's refusal frees ONLY its own entry", n48_fr_free_by_seq(&r, seqB), 1u);
    ck("H2-2C: B's slot is FREE", r.e[idxB].state, (uint32_t)N48_FR_FREE);
    ck("H2-2C: A's entry is STILL UNTOUCHED (still COMMITTED) - THE PROPERTY, ON THE RING",
       r.e[idxA].state, (uint32_t)N48_FR_COMMITTED);
    // 0.0.446 ( fix (1)): B was the newest push, so freeing it must RESTORE A as "the newest flight" -
    // the ring's own generalisation of's single-record stash/restore this file's pfa_checks() proves above.
    ck("H2-2C: freeing B RESTORES A as the ring's own newest (newestSeq)", r.newestSeq, seqA);
    ck("H2-2C: ...and A's own newestRetired is NOT falsely set by B's free", r.newestRetired, 0u);
    { uint32_t bSeq = 0u; uint64_t bAt = 0ull;
      const uint32_t v = n48_fr_defer_verdict(&r, 1u, 1u, 1500ull, 100000ull, &bSeq, &bAt);
      ck("H2-2C: after B's refusal, the ring still DEFERS on A alone (never withdraws early)", v, (uint64_t)N48_KSD_DEFER);
      ck("H2-2C: ...and names A as the blocking entry", bSeq, seqA); }

    // Step 4: A's own owned-slot fence reads OURS - end of pipe observed for A's flight, exactly as the ring poll
    // in gfxsrc_decide_frame retires it.
    ck("H2-2C: A retires by its own fence", n48_fr_poll_entry(&r, idxA, 1u, wantA), 1u);
    ck("H2-2C: A's slot is RETIRED", r.e[idxA].state, (uint32_t)N48_FR_RETIRED);
    // THE PROPERTY THIS EXTENSION EXISTS TO PROVE: with nothing live, the verdict must be NOW_EOP (A's own
    // retirement, correctly attributed after surviving B's refusal) - NOT NOT_IN_FLIGHT, which is what 0.0.430's
    // clear-not-restore defect (D23, above) would have produced on the single-record model, and is exactly what a
    // ring that failed to restore A as "newest" after B's free would produce here too.
    { uint32_t bSeq = 0u; uint64_t bAt = 0ull;
      const uint32_t v = n48_fr_defer_verdict(&r, 1u, 1u, 1500ull, 100000ull, &bSeq, &bAt);
      ck("H2-2C: with A retired and B long refused, the verdict is NOW_EOP (attributed to A)", v,
         (uint64_t)N48_KSD_NOW_EOP); }

    // NON-VACUITY: the SAME sequence, but B is never refused and instead ALSO retires - the verdict must still be
    // NOW_EOP (both ended cleanly), proving the NOW_EOP branch above is reached because A specifically retired,
    // not because the ring happens to be empty of PENDING/COMMITTED entries for any reason.
    {
        n48_fr_ring r2; n48_fr_reset(&r2);
        uint32_t ia = N48_FR_CAPACITY, ib = N48_FR_CAPACITY, lf = 0u;
        n48_fr_push(&r2, seqA, atA, ordA, voA, wantA, &ia);
        n48_fr_commit_mark(&r2, seqA, &lf);
        n48_fr_push(&r2, seqB, atB, ordB, voB, wantB, &ib);
        n48_fr_commit_mark(&r2, seqB, &lf);
        ck("H2-2C non-vacuity: B COMMITS instead of being refused", r2.e[ib].state, (uint32_t)N48_FR_COMMITTED);
        n48_fr_poll_entry(&r2, ia, 1u, wantA);
        n48_fr_poll_entry(&r2, ib, 1u, wantB);
        uint32_t bSeq = 0u; uint64_t bAt = 0ull;
        const uint32_t v = n48_fr_defer_verdict(&r2, 1u, 1u, 1500ull, 100000ull, &bSeq, &bAt);
        ck("H2-2C non-vacuity: with BOTH retired (no refusal), the verdict is ALSO NOW_EOP", v, (uint64_t)N48_KSD_NOW_EOP);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// THE PLANTED DEFECTS. Each is a plausible way to write this wrong; each must be CAUGHT.
// ---------------------------------------------------------------------------------------------------------------------

/* D1 — the write picks any live record instead of the BOUND context (clause 9's comparisons dropped). */
static uint32_t ev_D1(const n48_ks_in *k)
{
    n48_ks_in c = *k;
    c.rec_seq = c.now_seq; c.rec_ctx = c.now_ctx; c.rec_root = c.now_root;   /* "close enough" */
    if (!c.rec_found) { c.rec_found = 1u; c.rec_live = 1u; c.rec_id_ok = 1u; }
    return n48_ks_eval(&c);
}
/* D2 — the decision runs on the CACHED ws_classify result: the re-validation is copied from the frame. */
static uint32_t ev_D2(const n48_ks_in *k)
{
    n48_ks_in c = *k;
    c.now_seq = c.frame_ws_seq; c.now_ctx = c.frame_ws_ctx; c.now_root = c.frame_ws_root;
    c.now_vmid = c.frame_vmid; c.now_hw_root = c.frame_ws_root; c.now_hw_ok = 1u;
    c.now_state = 1u; c.now_complete = 1u;
    c.rec_seq = c.now_seq; c.rec_ctx = c.now_ctx; c.rec_root = c.now_root;
    return n48_ks_eval(&c);
}
/* D3 — the arm clause is dropped: the keystone writes in a judge-only run too. */
static uint32_t ev_D3(const n48_ks_in *k)
{
    n48_ks_in c = *k; c.arm_now = c.arm_commit;
    return n48_ks_eval(&c);
}
/* D4 — non-idempotent: an entry already standing is written AGAIN, and a spent attempt is retried. */
static uint32_t ev_D4(const n48_ks_in *k)
{
    n48_ks_in c = *k; c.rec_wrote = 0u; c.rec_withdrawn = 0u; c.attempts = 0u;
    return n48_ks_eval(&c);
}
/* D13 — 0.0.410: THE ALREADY VERDICT TRUSTS THE RECORD. This is the pre-K1 clause 10 verbatim - it
 *       answers ALREADY whenever `wrote && !withdrawn`, without asking the hardware. arm26's f16 was exactly that
 *       record: the deferred withdrawal had applied and root[511] was ZERO, the old rule said ALREADY, and the walks
 *       then found nothing and neutered the frame. If the K1 checks cannot tell this apart from the real rule, they are
 *       not testing K1 at all. */
static uint32_t ev_D13(const n48_ks_in *k)
{
    n48_ks_in c = *k;
    if (c.rec_wrote && !c.rec_withdrawn) return N48_KS_ALREADY;
    return n48_ks_eval(&c);
}
/* D9 — 0.0.374: clause 8's remaining job is dropped, so a VMID the invalidate CANNOT serve is armed anyway.
 *      This is the failure the retirement of the `!= 2` rule could have introduced: the entry goes into Apple's page
 *      table behind a TLB that was never told, and the software walk cannot see it because it reads the table. */
static uint32_t ev_D9(const n48_ks_in *k)
{
    n48_ks_in c = *k;
    if (c.now_vmid > N48_KS_MAX_INVALIDATE_VMID) { c.now_vmid = 2u; c.frame_vmid = 2u; }
    return n48_ks_eval(&c);
}
/* D5 — the arm proceeds although the post-write walk failed. */
static uint32_t pm_D5(uint32_t v, uint32_t g, uint32_t st, uint32_t a, uint32_t, uint32_t)
{
    return n48_ks_arm_permitted(v, g, st, a, 0xFu, 0xFu);
}
/* D6 — the readback status is counted as a refusal (the 0.0.372 behaviour). */
static uint32_t bk_D6(uint32_t cst) { return cst == 0u ? N48_DPG_PRESENT_OK : N48_DPG_PRESENT_REFUSED; }
/* D7 — the read-back MISMATCH is treated as a successful write (the fail-open shape rootwrite_arm_context invites:
 *      it returns kRootWriteOk with status 3). */
static uint32_t pm_D7(uint32_t v, uint32_t g, uint32_t, uint32_t a, uint32_t w, uint32_t need)
{
    return n48_ks_arm_permitted(v, g, 2u, a, w, need);
}
/* D8 — "no walk ran" is read as "nothing to disprove": an empty requirement permits. */
static uint32_t pm_D8(uint32_t v, uint32_t g, uint32_t st, uint32_t a, uint32_t w, uint32_t need)
{
    if (need == 0u) return n48_ks_mapping_claimed(v);
    return n48_ks_arm_permitted(v, g, st, a, w, need);
}

static uint32_t ev_real(const n48_ks_in *k) { return n48_ks_eval(k); }
static uint32_t pm_real(uint32_t v, uint32_t g, uint32_t st, uint32_t a, uint32_t w, uint32_t n) {
    return n48_ks_arm_permitted(v, g, st, a, w, n);
}

/* D10 - 0.0.374. THE DEFECT THIS CLAUSE EXISTS TO CATCH: a frame whose TLB invalidate FAILED is armed
 *       anyway. This is the PRE- rule verbatim - it never looked at the ack - so the mutant is not a caricature,
 *       it IS the code that shipped. The entry lands in Apple's page table, reads back byte-identical, and all four
 *       walks resolve; the GPU's walker keeps serving the cached EMPTY root[511] that NACKed arm3, because nothing
 *       ever told it otherwise. Neither the read-back nor the walk can see that: they read the table, not the TLB. */
static uint32_t pm_D10(uint32_t v, uint32_t g, uint32_t st, uint32_t, uint32_t w, uint32_t n) {
    return n48_ks_arm_permitted(v, g, st, 1u, w, n);
}
static uint32_t bk_real(uint32_t c) { return n48_dpg_present_bucket(c); }
static uint32_t df_real(const n48_ksd_in *d) { return n48_ksd_eval(d); }
/* D11 — 0.0.384: IT WITHDRAWS IN FLIGHT ANYWAY. The deferral is computed and then thrown away, which is
 *       what every version of this file before 0.0.384 did unconditionally: clear root[511], HDP-flush, invalidate the
 *       armed VMID's TLB - with the committed frame still executing. That is arm3's and arm10's condition to the bit
 *       (GCVM_L2_PROTECTION_FAULT_STATUS_LO32 0x0024295d, VMID 2, NACK, CID 0x14 = PA, WRITE to GE_POS_RING_BASE).
 *       If the checks above cannot tell this apart from the real rule, they are not testing the deferral at all. */
static uint32_t df_D11(const n48_ksd_in *d)
{
    const uint32_t r = n48_ksd_eval(d);
    return r == N48_KSD_DEFER ? N48_KSD_NOW_NOT_IN_FLIGHT : r;
}
/* D12 — 0.0.384: THE DEFERRAL IS NEVER APPLIED. The window never closes: end of pipe and the bound are
 *       both ignored, so once a frame has committed the withdrawal is deferred for the rest of the boot and our VALID
 *       PDE is left standing in a page Apple will free and hand to the next client as a leaf page within ~4 s. This is
 *       the OTHER end of the envelope and it is exactly as unacceptable as D11. */
static uint32_t df_D12(const n48_ksd_in *d)
{
    const uint32_t r = n48_ksd_eval(d);
    return (r == N48_KSD_NOW_EOP || r == N48_KSD_NOW_TIMEOUT) ? N48_KSD_DEFER : r;
}
/* D14 — 0.0.411: THE STALE RECORD IS RE-ARMED. This is 0.0.410's K1 clause VERBATIM: a record that
 *       says `wrote && !withdrawn` whose live root[511] does not match falls through to the arm path and WRITES.
 *       withdrew exactly this write, because it can land inside hook_unmapVA's withdrawal window (our PDE live during
 *       Apple's own unmapVA) and leaves the record inconsistent. Caught by the F1 STALE checks, which demand
 *       REC_MISMATCH and no write. */
static uint32_t ev_D14(const n48_ks_in *k)
{
    n48_ks_in c = *k;
    if (c.rec_wrote && !c.rec_withdrawn) {
        if (!c.rec_slot_ok) return N48_KS_REC_MISMATCH;
        if (c.rec_written != 0u && c.rec_slot_live == c.rec_written) return N48_KS_ALREADY;
        return N48_KS_OK;                       /* 0.0.410 K1: re-arm the stale record */
    }
    return n48_ks_eval(&c);
}
/* D15 — 0.0.411: THE WITHDRAWAL MARKER IS IGNORED. The keystone writes even while hook_unmapVA is
 *       between its clear and its re-arm -'s exact hazard. Caught by the F1 MARKER checks, which demand
 *       N48_KS_WITHDRAWING. */
static uint32_t ev_D15(const n48_ks_in *k)
{
    n48_ks_in c = *k; c.rec_withdrawing = 0u;
    return n48_ks_eval(&c);
}
/* D16 — 0.0.411: THE POST-WALK RE-CHECK NEVER WITHDRAWS. A withdrawal that begins during the four walks
 *       is missed, so a frame may run against a root[511] being taken down - the second half of F1's rule. */
static uint32_t post_never(uint32_t, uint32_t, uint64_t, uint64_t) { return 0u; }
/* D17 — 0.0.418 (E4): THE WITHDRAWAL COUNTER IS CLEARED AS A FLAG. This is the 0.0.411 decrement (set it to 0), under
 *       which the FIRST of two overlapping withdrawals wipes the marker while the second is still in its window and the
 *       keystone is let in. Caught by the E4 COUNTER checks, which demand NONZERO after the first leave. */
static uint32_t leave_flag(uint32_t) { return 0u; }

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.411 — SOURCE PINS. The rule above is pure and proven; these three properties are about the CALLER
// in AppleHardwareHook.cpp, which cannot be linked here. They are text anchors against the real file (argv[1]), the same
// idiom gfx_rasterarm_test.cpp and gfx_ringidle_test.cpp use. Not mutated - they are facts about that file.
//   * hook_unmapVA SETS the marker exactly once, and the set is BEFORE its first read of gKsFlight;
//   * it CLEARS the marker exactly once, and the clear is AFTER every branch (re-arm, root-freed, deferred-lost);
//   * commit_keystone_arm READS the marker with the same full barrier, exactly once.
// A context we never recorded returns before the marker exists: it has no record for the keystone to consult, so that
// exit is neither marked nor cleared (the early return precedes the set).
// ---------------------------------------------------------------------------------------------------------------------
static char *pin_slurp(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (!b) { fclose(f); return nullptr; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return nullptr; }
    b[n] = 0; fclose(f); if (len) *len = n;
    return b;
}
static uint32_t pin_count(const char *hay, const char *needle)
{
    uint32_t c = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle)) c++;
    return c;
}
// 0.0.423: the marker's release string appears twice — the DEFER release (first) and the exit release
// (last). The exit-ordering pins must anchor on the LAST, or they would be satisfied by the early one.
static const char *pin_last(const char *hay, const char *needle)
{
    const char *last = nullptr;
    for (const char *p = hay; (p = strstr(p, needle)) != nullptr; p += strlen(needle)) last = p;
    return last;
}
// =====================================================================================================================
// build 0.0.497 — SECTION W: SWITCH 64's BOUNDED WAIT AT THE KEYSTONE, driven through the REAL
// n48_ks_wait_run with a scripted marker and clock, then the REAL n48_ks_eval / n48_ks_post_walk_withdrawn on the values
// the kext re-reads AFTER the wait. RUN F's own frame (run10k driverlog-stream 10406-10416): f1, WindowServer create #5
// ctx 0xffffff950a72a800 root 0x3d6c00000 VMID 2, token 1 gate seq 1, record wrote 0 withdrawn 0, attempts 0, armed 0 of 8;
// an unmap of the SAME context (fire #6, va 0x4000b8000 size 0x8000, "Nothing of ours is armed") held the marker from its
// ENTRY (10410) to its return (10412), and the keystone read it at 10411: VERDICT 15.
// =====================================================================================================================
struct WScript {
    uint32_t releaseAfter;   // the marker reads 0 once this many pauses have been taken (UINT32_MAX = never)
    uint64_t t0, step;       // the clock: t0 at the first read, + step per pause (t0 0 = unusable clock)
    uint32_t reenterAt;      // 0 = never; else the marker reads HELD again at/after this many pauses (a second unmap)
    uint32_t pauses, markerReads, clockReads;
    uint32_t runaway;        // set by the harness if the loop ran past any bound it could have had
};
static uint32_t w_marker(void *c)
{
    WScript *w = static_cast<WScript *>(c);
    w->markerReads++;
    if (w->runaway) return 0u;                        // the harness's own escape: end a loop that has no bound
    if (w->reenterAt && w->pauses >= w->reenterAt) return 1u;
    return w->pauses >= w->releaseAfter ? 0u : 1u;
}
static uint64_t w_now(void *c)
{
    WScript *w = static_cast<WScript *>(c);
    w->clockReads++;
    return w->t0 ? w->t0 + (uint64_t)w->pauses * w->step : 0ull;
}
static void w_pause(void *c)
{
    WScript *w = static_cast<WScript *>(c);
    w->pauses++;
    if (w->pauses > 100000u) w->runaway = 1u;         // 250x the poll cap: only an UNBOUNDED loop gets here
}
static uint32_t w_run(WScript &sc, uint32_t on, uint32_t haveRec, uint32_t *polls, uint64_t *us)
{
    const n48_ks_wait_io io { &w_marker, &w_now, &w_pause, &sc };
    return n48_ks_wait_run(on, haveRec, &io, polls, us);
}
// RUN F's f1 as the keystone sees it AFTER the wait: every field but the marker is the frame's own.
static n48_ks_in w_runf(uint32_t markerNow)
{
    n48_ks_in k = good();
    k.frame_ws_ctx = k.now_ctx = k.rec_ctx = 0xffffff950a72a800ull;
    k.rec_withdrawing = markerNow;
    return k;
}
static uint32_t w_eval(uint32_t markerNow) { const n48_ks_in k = w_runf(markerNow); return n48_ks_eval(&k); }
static const char *ks_widest_name(const char *(*f)(uint32_t), uint32_t n, uint32_t *lenOut);
static void w_checks()
{
    printf("\nW. build 0.0.497: switch 64's bounded wait at the keystone, RUN F's f1\n");
    const uint32_t NEVER = 0xffffffffu;
    // W1 OFF IDENTITY: the wait reads NOTHING and waits for nothing; the keystone then sees what it sees today.
    {
        WScript sc { 0u, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 7u; uint64_t us = 7u;
        const uint32_t o = w_run(sc, 0u, 1u, &polls, &us);
        ck("W1 OFF: the outcome is OFF", o, N48_KSW_OFF);
        ck("W1 OFF: NO marker read, NO clock read, NO pause (nothing is touched)", sc.markerReads + sc.clockReads + sc.pauses, 0u);
        ck("W1 OFF: polls and waited are reported 0", polls + us, 0u);
        ck("W1 64 OFF (positive control, today): RUN F's f1 with the marker HELD is VERDICT 15", w_eval(1u),
           N48_KS_WITHDRAWING);
    }
    // W2/W3 no wait when there is nothing to wait for.
    {
        WScript sc { 0u, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        ck("W2 NOT HELD: the marker reads 0 at the first look -> no wait", w_run(sc, 1u, 1u, &polls, &us), N48_KSW_NOT_HELD);
        ck("W2   ...no pause taken", sc.pauses, 0u);
        WScript sn { NEVER, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        ck("W3 NO RECORD: nothing to wait on -> no wait", w_run(sn, 1u, 0u, &polls, &us), N48_KSW_NO_RECORD);
        ck("W3   ...the marker is never even read", sn.markerReads + sn.pauses, 0u);
    }
    // W4 RUN F, (A): the marker is RELEASED at the unmap's return (here: after 3 pauses, 15 us), then the UNCHANGED checks
    // run on the fresh read (0) and answer OK - the write the keystone was waiting to make.
    {
        WScript sc { 3u, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        const uint32_t o = w_run(sc, 1u, 1u, &polls, &us);
        ck("W4 RUN F (A): the marker released at the unmap's return -> RELEASED", o, N48_KSW_RELEASED);
        ck("W4   ...after exactly 3 polls", polls, 3u);
        ck("W4   ...15 us waited", us, 15u);
        ck("W4   ...the UNCHANGED n48_ks_eval on the re-read (marker 0) -> keystone OK (the write)",
           w_eval(0u), N48_KS_OK);
        ck("W4   ...and the unchanged post-walk re-read still withdraws if the marker is held again after the walks",
           n48_ks_post_walk_withdrawn(1u, 1u, kWsEntry, kWsEntry), 1u);
        ck("W4   ...and passes when it is not", n48_ks_post_walk_withdrawn(0u, 1u, kWsEntry, kWsEntry), 0u);
    }
    // W5 a marker NEVER released: the TIME bound ends the wait at 2 ms, and the unchanged checks give today's VERDICT 15.
    {
        WScript sc { NEVER, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        const uint32_t o = w_run(sc, 1u, 1u, &polls, &us);
        ck("W5 never released: TIMEOUT", o, N48_KSW_TIMEOUT);
        ck("W5   ...no runaway (the loop has a bound)", sc.runaway, 0u);
        ck("W5   ...waited exactly the bound (2000 us at 5 us/poll)", us, N48_KS_WAIT_BOUND_US);
        ck("W5   ...within the poll cap", polls <= N48_KS_WAIT_MAX_POLLS, 1u);
        ck("W5   ...the unchanged n48_ks_eval on the re-read (still held) -> VERDICT 15 (fail-closed)",
           w_eval(1u), N48_KS_WITHDRAWING);
    }
    // W6 the TIME half of the bound decides on a coarse clock (100 us per pause): 20 polls, not the poll cap.
    {
        WScript sc { NEVER, 1000u, 100u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        ck("W6 coarse clock: TIMEOUT", w_run(sc, 1u, 1u, &polls, &us), N48_KSW_TIMEOUT);
        ck("W6   ...the TIME bound decided: exactly 20 polls", polls, 20u);
        ck("W6   ...and 2000 us", us, 2000u);
    }
    // W7 the POLL half of the bound decides when the clock cannot be read (0): exactly the poll cap.
    {
        WScript sc { NEVER, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 7u;
        ck("W7 unusable clock: TIMEOUT", w_run(sc, 1u, 1u, &polls, &us), N48_KSW_TIMEOUT);
        ck("W7   ...the POLL bound decided: exactly the cap", polls, N48_KS_WAIT_MAX_POLLS);
        ck("W7   ...no runaway", sc.runaway, 0u);
        ck("W7   ...waited reported 0 (no clock)", us, 0u);
    }
    // W8 a stuck clock (it reads, but never advances) - the poll cap still ends it.
    {
        WScript sc { NEVER, 1000u, 0u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        ck("W8 stuck clock: TIMEOUT at the poll cap", w_run(sc, 1u, 1u, &polls, &us), N48_KSW_TIMEOUT);
        ck("W8   ...exactly the cap, no runaway", polls == N48_KS_WAIT_MAX_POLLS && !sc.runaway, 1u);
    }
    // W9 a release on the LAST poll inside the bound is a release, one poll later is a timeout (the exact boundary).
    {
        WScript a { 399u, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        ck("W9 released at poll 399 (1995 us): RELEASED", w_run(a, 1u, 1u, &polls, &us), N48_KSW_RELEASED);
        WScript b { 401u, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        ck("W9 released at poll 401 (past 2000 us): TIMEOUT", w_run(b, 1u, 1u, &polls, &us), N48_KSW_TIMEOUT);
        ck("W9   ...stopped at the bound, not at the release", polls, 400u);
    }
    // W10 a SECOND unmap re-enters the window after a release: the wait has already answered RELEASED, and it is the
    // UNCHANGED checks that catch it - clause 9b if it is held at the evaluation, the post-walk re-read if after it.
    {
        WScript sc { 2u, 1000u, 5u, 0u, 0u, 0u, 0u, 0u };
        uint32_t polls = 0u; uint64_t us = 0u;
        ck("W10 released, then a second unmap enters: the wait says RELEASED", w_run(sc, 1u, 1u, &polls, &us), N48_KSW_RELEASED);
        ck("W10   ...held at the evaluation -> VERDICT 15", w_eval(1u), N48_KS_WITHDRAWING);
        ck("W10   ...held only after the walks -> the post-walk re-read WITHDRAWS",
           n48_ks_post_walk_withdrawn(1u, 1u, kWsEntry, kWsEntry), 1u);
    }
    // W11 the per-wait line and the bare-read report line fit the 480-byte body cap at their widest.
    {
        char b[2048];
        const uint32_t U = 4294967295u;
        const unsigned long long L = 0xffffffffffffffffull;
        const char *wn = ks_widest_name(&n48_ks_wait_name, N48_KSW_OUTCOMES, nullptr);
        const int n1 = std::snprintf(b, sizeof b, N48_KS_WAIT_LINE_FMT, U, U, wn, U, L, U, U);
        const int n2 = std::snprintf(b, sizeof b, N48_KS_WAIT_REPORT_FMT, "OFF (default)",
                                     " - `gfxneuter 64` REFUSED: a continuous arm stands, unchanged", L, L, L, L, L, U, U, L,
                                     wn, L, U, L);
        if (!gQuiet) printf("      %-30s %4d bytes\n      %-30s %4d bytes\n", "WAIT line", n1, "kswait64 report", n2);
        ck("W11 the WAIT line fits the 480-byte cap", n1 > 0 && (unsigned)n1 <= N48_KS_REPORT_BODY_CAP, 1u);
        ck("W11 the kswait64 report line fits the 480-byte cap", n2 > 0 && (unsigned)n2 <= N48_KS_REPORT_BODY_CAP, 1u);
    }
}

// Section W's kext half: REACHABILITY IN THE KEXT'S OWN ORDER. The latch is read ONCE per pass in hook_gfxCommitIB and
// handed to the keystone call; inside commit_keystone_arm the wait comes after the inert exit and BEFORE every piece of
// evidence (ks_revalidate, the marker's first read, the walks, the post-walk re-read), and the post-walk re-read is the
// unchanged statement. The (B) retirement sits inside the refusal branch, gated by the latch, before the branch drops
// gFsPend - and it never commits.
static void w_source_pins(const char *src)
{
    const char *latch   = "const uint32_t ks64 = gKsWaitOn ? 1u : 0u;";
    const char *call    = "commit_keystone_arm(wf, 1u, gXdCmGateSeq, kr, ks64);";
    const char *fnHead  = "static bool commit_keystone_arm(const WsFrame &wf, uint32_t tokenMatched, uint32_t gateSeq, KsResult &kr, uint32_t ksWait) {";
    const char *inert   = "if (k.arm_now != k.arm_commit) {";
    const char *wait    = "ks_wait_marker_bounded(wf, ksWait, gateSeq);";
    const char *reval   = "ks_revalidate(w, complete);";
    const char *mkPre   = "k.rec_withdrawing = __atomic_load_n(&e->ksWithdrawing, __ATOMIC_SEQ_CST);";
    const char *walk    = "rootwrite_walk_one(w.root, va, startVa, fbStart, vramSize, \"AFTER the keystone\", walkLog)";
    const char *mkPost  = "const uint32_t mk = __atomic_load_n(&e->ksWithdrawing, __ATOMIC_SEQ_CST);";
    const char *postIf  = "    uint32_t postWithdrawn = 0u;\n    if (e) {\n";
    const char *postSet = "        postWithdrawn = n48_ks_post_walk_withdrawn(mk, pOk, pLive, e->rootWritten);";
    const char *postUse = "if (postWithdrawn && kr.armReason == N48_KSA_OK) { kr.armReason = N48_KSA_WITHDRAWN; gKsWithdrawing++; }";
    const char *waitFn  = "__attribute__((noinline)) static void ks_wait_marker_bounded(const WsFrame &wf, uint32_t on, uint32_t gateSeq)\n{\n    if (!on) return;\n";
    // build 0.0.526 item 3: the same pure loop with switch 81's bound in use (gfx_ks81.h; 2000 us / 400 polls except at M3).
    const char *waitRun = "const uint32_t o = n48_ks_wait_run_b(1u, rw ? 1u : 0u, &io, bUs, n48_ks81_wait_max_polls(bUs), &polls, &us);";
    const char *pause   = "static void ks_wait_cb_pause(void *) { IODelay(N48_KS_WAIT_POLL_US); }";
    // build 0.0.530 (SRCFILL85.md item 8): the call site dispatches through n48_fs85_retire_withdrawn, which IS
    // n48_fs_retire_withdrawn verbatim while switch 85 is not in force (gfx_fs85_test proves the identity).
    const char *retire  = "if (ks64 && gFsPend.active && n48_fs85_retire_withdrawn(&gFs, &gFs85, gFsPend.cb0, (uint32_t)N48_DEP_SOURCE_NEUTER)) {";
    const char *refuse  = "if (tokMatch && !ksOk) {";
    const char *commitB = "if (tokMatch && ksOk) {";
    const char *drop    = "gFsPend.active = 0u;";
    ck("PIN 497 A: switch 64 is latched exactly once", pin_count(src, latch), 1u);
    ck("PIN 497 A: gKsWaitOn has exactly 5 mentions (definition, latch, report, the verb's read and its write) - no stray reader",
       pin_count(src, "gKsWaitOn"), 5u);
    ck("PIN 497 A: the keystone is called with the latched value, exactly once", pin_count(src, call), 1u);
    const char *pl = strstr(src, latch), *pc = strstr(src, call);
    ck("PIN 497 A: ORDER - the latch precedes the keystone call", pl && pc && pl < pc, 1u);
    const char *pf = strstr(src, fnHead);
    ck("PIN 497 A: commit_keystone_arm takes the latch as `ksWait`, exactly once", pin_count(src, fnHead), 1u);
    ck("PIN 497 A: the wait is called exactly once", pin_count(src, wait), 1u);
    ck("PIN 497 A: the wait function returns first when OFF (OFF identity: nothing read, nothing waited)", pin_count(src, waitFn), 1u);
    ck("PIN 497 A: the wait runs the PURE loop, exactly once", pin_count(src, waitRun), 1u);
    ck("PIN 497 A: the pause is a busy-wait (IODelay), never a sleep", pin_count(src, pause), 1u);
    const char *pi = pf ? strstr(pf, inert) : nullptr, *pw = pf ? strstr(pf, wait) : nullptr,
               *pr = pf ? strstr(pf, reval) : nullptr, *pm = pf ? strstr(pf, mkPre) : nullptr,
               *pk = pf ? strstr(pf, walk) : nullptr, *pp = pf ? strstr(pf, mkPost) : nullptr;
    ck("PIN 497 A: ORDER - inert exit < WAIT < ks_revalidate < the marker's first read < the walks < the post-walk read",
       pi && pw && pr && pm && pk && pp && pi < pw && pw < pr && pr < pm && pm < pk && pk < pp, 1u);
    ck("PIN 497 A: the post-walk re-read is the unchanged statement, exactly once", pin_count(src, mkPost), 1u);
    ck("PIN 497 A: ...under the unchanged unconditional `if (e)`", pin_count(src, postIf), 1u);
    ck("PIN 497 A: ...decided by the unchanged pure call, exactly once", pin_count(src, postSet), 1u);
    ck("PIN 497 A: ...and it still withdraws the arm", pin_count(src, postUse), 1u);
    ck("PIN 497 B: the retirement is called exactly once in the kext", pin_count(src, "n48_fs85_retire_withdrawn("), 1u);
    ck("PIN 497 B / 530: the kext never calls the 0.0.529 function directly (the dispatcher does)",
       pin_count(src, "n48_fs_retire_withdrawn("), 0u);
    ck("PIN 497 B: ...gated by the latch and the gate's recorded member, with the recorded CB0", pin_count(src, retire), 1u);
    ck("PIN 497 B: the member is never COMMITTED at a withdrawal: n48_fs85_commit(&gFs stays the commit branch's one call",
       pin_count(src, "n48_fs85_commit(&gFs, ") == 1u && pin_count(src, "n48_fs_commit(") == 0u ? 1u : 0u, 1u);
    const char *pR = strstr(src, refuse), *pC = strstr(src, commitB), *pT = strstr(src, retire);
    const char *pD = pR ? strstr(pR, drop) : nullptr;
    ck("PIN 497 B: ORDER - inside the refusal branch, before that branch drops gFsPend, before the commit branch",
       pR && pC && pT && pD && pR < pT && pT < pD && pD < pC, 1u);
}


// =====================================================================================================================
// build 0.0.498 — SWITCH 65's REACHABILITY IN THE KEXT'S OWN ORDER (the pure behaviour and RUN
// H/H2's sequences are gfx_flightring_test's section X). In hook_unmapVA: the latch is read once per call, the check is
// reached only at verdict NOW_TIMEOUT with the latch set, AFTER the verdict and BEFORE every consumer of it (kdEop,
// deferNow, the marker's release, APPLYING, withdrawnWhileLive, the withdrawal block). In ks_eop_at_expiry: the poll and the
// retirement bookkeeping run inside the gXdLock try-lock, the unlock precedes the re-scan/re-clock/re-verdict, and the
// function writes nothing but our ring and counters (no MM write, no register, no flush).
static void x_source_pins(const char *src)
{
    const char *latch  = "const uint32_t ks65 = gKsEopExpOn ? 1u : 0u;";
    const char *call   = "        if (ks65 && kdv == N48_KSD_NOW_TIMEOUT)\n"
                         "            ks_eop_at_expiry(self, e->seq, e->unmapFires + 1u, &kdv, &kdAnyLiveAtScan, &kdNowUs, &kdNowOk,\n"
                         "                             &kdBlockingSeq, &kdBlockingAtUs);\n";
    const char *head   = "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {";
    const char *verd   = "kdv = n48_fr_defer_verdict(&gKsRing, gKsDeferOn ? 1u : 0u, kdNowOk, kdNowUs, kKsFlightUs,";
    const char *eop    = "kdEop = (kdv == N48_KSD_NOW_EOP) ? 1u : 0u;";
    const char *defer  = "const uint32_t deferNow = n48_ksd_defer(kdv);";
    const char *hold   = "const uint32_t held = n48_ks_withdraw_hold(kdv);";
    const char *apply  = "if (kdArmed && !deferNow && e->ksDeferPending) {";
    const char *wwl    = "gKsD.withdrawnWhileLive++;";
    const char *wdraw  = "if (e->wrote && !e->withdrawn && !deferNow) {";
    ck("PIN 498: switch 65 is latched exactly once", pin_count(src, latch), 1u);
    ck("PIN 498: gKsEopExpOn has exactly 5 mentions (definition, latch, report, the verb's read and write) - no stray reader",
       pin_count(src, "gKsEopExpOn"), 5u);
    ck("PIN 498: the expiry check is called exactly once, gated by the latch AND verdict NOW_TIMEOUT", pin_count(src, call), 1u);
    ck("PIN 498: ks_eop_at_expiry has exactly one caller", pin_count(src, "ks_eop_at_expiry(self, "), 1u);
    const char *pH = strstr(src, head);
    const char *pL = pH ? strstr(pH, latch) : nullptr, *pV = pH ? strstr(pH, verd) : nullptr,
               *pC = pH ? strstr(pH, call) : nullptr, *pE = pH ? strstr(pH, eop) : nullptr,
               *pD = pH ? strstr(pH, defer) : nullptr, *pK = pH ? strstr(pH, hold) : nullptr,
               *pA = pH ? strstr(pH, apply) : nullptr, *pW = pH ? strstr(pH, wwl) : nullptr,
               *pX = pH ? strstr(pH, wdraw) : nullptr;
    ck("PIN 498: ORDER in hook_unmapVA - latch < verdict < EXPIRY CHECK < kdEop < deferNow < marker release < APPLYING < "
       "withdrawnWhileLive < the withdrawal", pL && pV && pC && pE && pD && pK && pA && pW && pX && pL < pV && pV < pC &&
       pC < pE && pE < pD && pD < pK && pK < pA && pA < pW && pW < pX, 1u);
    // Inside ks_eop_at_expiry.
    const char *fhead  = "__attribute__((noinline)) static void ks_eop_at_expiry(const void *self, uint32_t ctxSeq, uint32_t fire, uint32_t *kdv,";
    const char *lock   = "    if (gXdLock && IOLockTryLock(gXdLock)) {\n        locked = 1u;\n";
    const char *poll   = "n = n48_fr_expiry_poll(&gKsRing, 1u, *nowOk, *nowUs, kKsFlightUs, &ks_x_read, nullptr, gKsXRet, N48_FR_CAPACITY, &o);";
    const char *unlock = "        IOLockUnlock(gXdLock);\n    } else {\n        gKsX.busy++;\n    }\n";
    const char *rever  = "    if (n) {\n"
                         "        __atomic_thread_fence(__ATOMIC_ACQUIRE);\n"
                         "        *anyLive = n48_fr_any_live(&gKsRing);\n"
                         "        *nowUs = drain_now_us();\n"
                         "        *nowOk = *nowUs ? 1u : 0u;\n"
                         "        *kdv = n48_fr_defer_verdict(&gKsRing, gKsDeferOn ? 1u : 0u, *nowOk, *nowUs, kKsFlightUs, blkSeq, blkAtUs);\n"
                         "    }\n";
    const char *outc   = "const uint32_t xo = n48_fr_expiry_outcome(locked, *kdv);";
    const char *rd     = "static uint32_t ks_x_read(void *, uint64_t off, uint32_t *val) { return navi48_vram_read_mm(off, val, 1) ? 1u : 0u; }";
    ck("PIN 498: the definition exists exactly once", pin_count(src, fhead), 1u);
    ck("PIN 498: the pure poll is called exactly once in the kext", pin_count(src, "n48_fr_expiry_poll("), 1u);
    ck("PIN 498: ...with the owned-slot MM read (one dword) as its reader", pin_count(src, rd), 1u);
    ck("PIN 498: the re-verdict is hook_unmapVA's own scan, clock, verdict, only when something retired", pin_count(src, rever), 1u);
    const char *pF = strstr(src, fhead);
    const char *pEnd = pF ? strstr(pF, "\n}\n") : nullptr;
    const char *pLk = pF ? strstr(pF, lock) : nullptr, *pP = pF ? strstr(pF, poll) : nullptr,
               *pU = pF ? strstr(pF, unlock) : nullptr, *pR = pF ? strstr(pF, rever) : nullptr,
               *pO = pF ? strstr(pF, outc) : nullptr;
    ck("PIN 498: ORDER in ks_eop_at_expiry - try-lock < poll < unlock < re-verdict < outcome, all in the one function",
       pF && pEnd && pLk && pP && pU && pR && pO && pLk < pP && pP < pU && pU < pR && pR < pO && pO < pEnd, 1u);
    // No write of any kind but our ring and counters inside the function.
    uint32_t bad = 0u;
    if (pF && pEnd) {
        const size_t len = (size_t)(pEnd - pF);
        const char *forbid[] = { "navi48_vram_write_mm", "navi48_reg_write32", "WREG32", "navi48_hdp_flush_now",
                                 "navi48_gmc_flush_tlb_vmid", "rootwrite_", "n48_fr_expire(", "n48_fr_reclaim" };
        for (const char *f : forbid) {
            const char *q = strstr(pF, f);
            if (q && (size_t)(q - pF) < len) bad++;
        }
    } else bad = 99u;
    ck("PIN 498: ks_eop_at_expiry writes no VRAM, register, flush or page table and does not expire/reclaim", bad, 0u);
}

static void source_pins(const char *path)
{
    long n = 0;
    char *src = pin_slurp(path, &n);
    if (!src) { printf("  SKIP source pins: cannot read %s\n", path); gFail++; gRun++; return; }
    const char *set    = "__atomic_fetch_add(&e->ksWithdrawing, 1u, __ATOMIC_SEQ_CST);";
    const char *clr    = "ks_withdraw_leave_atomic(&e->ksWithdrawing);";
    const char *dec    = "n48_ks_withdraw_leave(cur)";
    const char *ld     = "__atomic_load_n(&e->ksWithdrawing, __ATOMIC_SEQ_CST)";
    // C5 part 1 (notes/design/C5-CONTINUOUS.md Q1) — the single gKsFlight read is gone; hook_unmapVA's own read of
    // the flight ring is this call, the first (and only, in this function) call to n48_fr_defer_verdict.
    const char *flight = "kdv = n48_fr_defer_verdict(&gKsRing";
    const char *early  = "if (!e || e->state != 1)";
    const char *rearm  = "unmapva: RE-ARMED for ctx";
    const char *freed  = "unmapva: *** ROOT FREED BY THIS CALL ***";
    const char *lost   = "kstone-defer: *** DEFERRED WITHDRAWAL LOST TO A FREE ***";
    const char *entryrd = "navi48_vram_read_mm(rootOff + (uint64_t)kRingRootSlot * 8u, d, 2)";
    // C5 part 1: hook_unmapVA no longer calls n48_ksd_eval directly - it calls the ring-wide n48_fr_defer_verdict,
    // which is what `flight` above already anchors on (the FIRST such call in the file, inside hook_unmapVA).
    const char *eval   = flight;
    // 0.0.427 (the follow-up) — THE GUARDS THEMSELVES. `clr` above is the BARE call, so it counts 2
    // whether or not the two guards are there: deleting `if (!held)` or `if (held)` (making the release unconditional)
    // would still satisfy every pin above. These two anchors name the guards, so removing either guard makes its pin 0.
    const char *guardHold = "if (!held) ks_withdraw_leave_atomic(&e->ksWithdrawing);";   /* the DEFER release */
    const char *guardHeld = "if (held) ks_withdraw_leave_atomic(&e->ksWithdrawing);";     /* the EXIT release */
    const char *ps = strstr(src, set), *pc = strstr(src, clr), *pcLast = pin_last(src, clr);
    ck("PIN: hook_unmapVA SETS the marker exactly once", pin_count(src, set), 1u);
    // 0.0.423 (KEYSTONE-A-PRIME): TWO releases — the DEFER release at the decision, and the exit release
    // for every call that held the marker. One without the other would be either's over-refusal or a leak.
    ck("PIN: hook_unmapVA RELEASES the marker exactly twice (the DEFER release, then the exit)",
       pin_count(src, clr), 2u);
    // 0.0.418 (E4): the decrement is saturating — it uses n48_ks_withdraw_leave, which returns 0 at 0 — so the counter
    // can never wrap to UINT32_MAX if a leave were ever seen without its enter.
    ck("PIN: the withdrawal decrement is SATURATING (never below zero)", pin_count(src, dec), 1u);
    // TWO reads, both with the same full barrier: one before the rule runs (gates the write) and one after the four
    // walks (F1's post-walk re-check). A third would be a stray reader; a single one would be half the rule.
    ck("PIN: commit_keystone_arm READS the marker exactly twice, full barrier both", pin_count(src, ld), 2u);
    ck("PIN: the unrecorded-context early exit precedes the set", early && ps && strstr(src, early) < ps, 1u);
    ck("PIN: the SET is before the first flight-ring read", ps && strstr(src, flight) && ps < strstr(src, flight), 1u);
    // 0.0.423: the deferral decision and the DEFER release both sit ABOVE the entry read of root[511], so a
    // deferring call releases the marker before it pays the MM window - the whole point of moving the block.
    // C5 part 1: n48_ksd_eval is gone from hook_unmapVA (the ring generalises it); the ring-wide verdict call is the
    // anchor now.
    ck("PIN: n48_fr_defer_verdict(&gKsRing comes BEFORE the entry read of root[511]",
       strstr(src, eval) && strstr(src, entryrd) && strstr(src, eval) < strstr(src, entryrd), 1u);
    ck("PIN: the DEFER release comes BEFORE the entry read of root[511]",
       pc && strstr(src, entryrd) && pc < strstr(src, entryrd), 1u);
    // The EXIT release is the LAST occurrence (the DEFER release is the first), and it must sit after every branch.
    ck("PIN: the EXIT release is after the re-arm branch", pcLast && strstr(src, rearm) && pcLast > strstr(src, rearm), 1u);
    ck("PIN: the EXIT release is after the root-freed branch", pcLast && strstr(src, freed) && pcLast > strstr(src, freed), 1u);
    ck("PIN: the EXIT release is after the deferred-withdrawal-lost branch",
       pcLast && strstr(src, lost) && pcLast > strstr(src, lost), 1u);
    // 0.0.427: the guards exist, each exactly once, and in their places. Deleting either one - the planted
    // break named - makes the matching count 0 and FAILS; an unconditional release would double-release the
    // DEFER path (or leak the exit one).
    ck("PIN: the DEFER release is guarded by `if (!held)`", pin_count(src, guardHold), 1u);
    ck("PIN: the EXIT release is guarded by `if (held)`",   pin_count(src, guardHeld), 1u);
    ck("PIN: the `if (!held)` DEFER release precedes the entry read of root[511]",
       strstr(src, guardHold) && strstr(src, entryrd) && strstr(src, guardHold) < strstr(src, entryrd), 1u);
    ck("PIN: the `if (held)` EXIT release is after the re-arm branch",
       strstr(src, guardHeld) && strstr(src, rearm) && strstr(src, guardHeld) > strstr(src, rearm), 1u);
    // C5 part 1 (notes/design/C5-CONTINUOUS.md Q1, item 2) — THE FLIGHT RING REPLACES 0.0.431's STASH/RESTORE.
    // A keystone refusal now frees ONLY its own entry; there is no shared record left for a later stamp to
    // clobber, so there is nothing to restore. These pins prove: the push happens exactly once, at the gate stamp,
    // BEFORE the keystone runs; a refusal frees its own entry (by `here.seq`, the SAME seq the push used) exactly
    // once, inside the refusal branch, before the commit branch; a commit marks its own entry COMMITTED exactly
    // once, inside the commit branch; and every trace of the old single-record stash/restore (`gKsFlightSaved`,
    // `n48_ks_flight_restore_ok`, `gKsFlight`) is GONE from the kext (the pure helpers stay in gfx_keystone.h,
    // unused by the kext from this build on, still exercised by this file's own H2 interleaving model above).
    const char *flightPush      = "n48_fr_push(&gKsRing, gXdCmToken.seq";
    const char *keystoneArmCall = "commit_keystone_arm(wf, 1u, gXdCmGateSeq, kr, ks64);";   /* 0.0.497: + the latched switch 64 */
    const char *refuseBranch    = "if (tokMatch && !ksOk) {";
    const char *commitBranch    = "if (tokMatch && ksOk) {";
    const char *freeOwnEntry    = "n48_fr_free_by_seq(&gKsRing, here.seq);";
    // 0.0.446 ( fix (1)): the commit mark is n48_fr_commit_mark, which also names gKsLastFlightSeq -
    // the SAME PENDING -> COMMITTED transition (it calls n48_fr_mark_committed), now the one site the newest flight is set.
    const char *markCommitted   = "n48_fr_commit_mark(&gKsRing, seq, &gKsLastFlightSeq);";
    ck("PIN C5: the flight is PUSHED to the ring exactly once (the gate stamp)", pin_count(src, flightPush), 1u);
    ck("PIN C5: ... and the push is BEFORE the keystone arm call (KEYSTONE-A-PRIME's ordering)",
       strstr(src, flightPush) && strstr(src, keystoneArmCall) && strstr(src, flightPush) < strstr(src, keystoneArmCall),
       1u);
    ck("PIN C5: a keystone refusal FREES ITS OWN ENTRY exactly once (`here.seq`, the push's own seq)",
       pin_count(src, freeOwnEntry), 1u);
    ck("PIN C5: ... inside the refusal branch, before the commit branch",
       strstr(src, refuseBranch) && strstr(src, freeOwnEntry) && strstr(src, commitBranch) &&
       strstr(src, refuseBranch) < strstr(src, freeOwnEntry) &&
       strstr(src, freeOwnEntry) < strstr(src, commitBranch), 1u);
    ck("PIN C5: a commit MARKS ITS OWN ENTRY COMMITTED exactly once, inside the commit branch",
       pin_count(src, markCommitted), 1u);
    ck("PIN C5: ... and it is after the commit branch opens",
       strstr(src, commitBranch) && strstr(src, markCommitted) && strstr(src, commitBranch) < strstr(src, markCommitted),
       1u);
    // 0.0.384-0.0.431's single flight record declaration is gone (comments may still name the old field, by history,
    // as several just above this function's own pins do - the struct's DECLARATION is the fact that matters).
    ck("PIN C5: 0.0.384-0.0.431's single flight record's declaration is GONE (`} gKsFlight {};`)",
       pin_count(src, "} gKsFlight {};"), 0u);
    ck("PIN C5: the stash's declaration is GONE (`n48_ks_flight gKsFlightSaved`)",
       pin_count(src, "n48_ks_flight gKsFlightSaved"), 0u);
    ck("PIN C5: the restore call is GONE (`n48_ks_flight_restore_ok(gKsFlightSaved`)",
       pin_count(src, "n48_ks_flight_restore_ok(gKsFlightSaved"), 0u);

    // =====================================================================================================
    // 0.0.444 (C5-RING-REVIEW.md (B)) — THE PART-2 FIXES' OWN WIRING, PINNED THE SAME WAY.
    // =====================================================================================================
    // Item 1: the fence handoff is the PURE function, not a stale re-read of gXdF828Pending, at the push site.
    const char *handoffCall = "n48_f828_ring_handoff(gXdF828GateOk, gXdF828GateSeq, gXdCmToken.seq";
    ck("PIN item1: the push site calls the PURE fence handoff exactly once", pin_count(src, handoffCall), 1u);
    ck("PIN item1: ... and it is BEFORE the ring push", strstr(src, handoffCall) && strstr(src, flightPush) &&
       strstr(src, handoffCall) < strstr(src, flightPush), 1u);
    ck("PIN item1: the OLD stale read (`gXdF828Pending ? gXdF828Cand.ordinal`) is GONE",
       pin_count(src, "gXdF828Pending ? gXdF828Cand.ordinal"), 0u);

    // Item 2: hook_unmapVA's ring presence scan runs BEFORE the clock read that feeds n48_fr_defer_verdict.
    // C5 part 2 (build 0.0.460, notes/design/C5-CONTINUOUS.md Q2): the scan's own return is now CAPTURED
    // (`kdAnyLiveAtScan`), not discarded, for the continuous build's "a withdrawal inside a flight" stop - the
    // scan statement itself did not move, only its exact text (the anchor below is re-pinned to that text, not
    // weakened: it is still a literal, ordered pair of strings that must both be present and in this order).
    const char *anyLiveCall = "kdAnyLiveAtScan = n48_fr_any_live(&gKsRing);";
    const char *clockRead   = "kdNowUs = drain_now_us();";
    ck("PIN item2: hook_unmapVA scans the ring BEFORE reading the clock", pin_count(src, anyLiveCall), 1u);
    ck("PIN item2: ... and the scan precedes the clock read", strstr(src, anyLiveCall) && strstr(src, clockRead) &&
       strstr(src, anyLiveCall) < strstr(src, clockRead), 1u);

    // Item 4: the token-mismatch site moves PENDING -> NOT_RUN, never frees.
    const char *mismatchNotRun = "n48_fr_mark_not_run(&gKsRing, gXdCmToken.seq);";
    ck("PIN item4: the token-mismatch site marks NOT_RUN, exactly once", pin_count(src, mismatchNotRun), 1u);
    ck("PIN item4: the OLD free-by-the-shared-token-seq call is GONE",
       pin_count(src, "n48_fr_free_by_seq(&gKsRing, gXdCmToken.seq);"), 0u);

    // Item 5(i): a ring-push failure forces the return to 0 and never prints SPENT.
    ck("PIN item5i: a failed push sets `ringPushed = false`", pin_count(src, "ringPushed = false;"), 1u);
    ck("PIN item5i: the function's own return ties to it", pin_count(src, "return (spent && ringPushed) ? 1u : 0u;"), 1u);
    ck("PIN item5i: RING-FULL is the forced reason on that path", pin_count(src, "reason = N48_CM_RING_FULL;"), 1u);

    // Item 5(ii): the keystone site refuses to run without this seq's own PENDING ring entry.
    // 0.0.446 ( fix (4)): the same predicate, answered by the pure n48_fr_keystone_guard (host-tested in
    // gfx_flightring_test.cpp) so the refusal can name which of the two refused.
    const char *frGate = "n48_fr_keystone_guard(&gKsRing, tokMatch ? 1u : 0u, here.seq, &frGuardState)";
    ck("PIN item5ii: the keystone gate asks for this seq's own ring entry", pin_count(src, frGate), 1u);
    ck("PIN item5ii: ... and it is folded into `ksOk`, BEFORE `commit_keystone_arm` runs",
       strstr(src, frGate) && strstr(src, keystoneArmCall) && strstr(src, frGate) < strstr(src, keystoneArmCall), 1u);

    // Item 6: expiry and reclamation run once per judged frame.
    ck("PIN item6: n48_fr_expire runs against the ring", pin_count(src, "n48_fr_expire(&gKsRing, 1u, frMaintNow"), 1u);
    // 0.0.446 ( fix (3)): reclamation holds back what a pending tgtsample AFTER still needs.
    ck("PIN item6: n48_fr_reclaim_hold runs against the ring", pin_count(src, "n48_fr_reclaim_hold(&gKsRing, frHold, frNHold);"), 1u);

    // =====================================================================================================
    // 0.0.446 ( fixes (1)-(4)) — THE KEXT WIRING OF THE PURE FIXES, PINNED (supplementary: each fix's
    // behaviour is proven with a planted break in gfx_flightring_test.cpp; these pins prove the kext calls it).
    // =====================================================================================================
    ck("PIN 446 fix1: gKsLastFlightSeq is ASSIGNED nowhere in the kext (only n48_fr_commit_mark writes it)",
       pin_count(src, "gKsLastFlightSeq = "), 0u);
    ck("PIN 446 fix1: ... and the mark is inside the commit branch, after the keystone arm call",
       strstr(src, keystoneArmCall) && strstr(src, markCommitted) && strstr(src, keystoneArmCall) < strstr(src, markCommitted), 1u);
    ck("PIN 446 fix2: the slot-order poll loop is GONE",
       pin_count(src, "for (uint32_t fi = 0; fi < N48_FR_CAPACITY; fi++)"), 0u);
    ck("PIN 446 fix2: the poll walks n48_fr_next_poll (ordinal order), seeded and advanced",
       pin_count(src, "n48_fr_next_poll(&gKsRing, &frPollCursor)"), 2u);
    {
        const char *afterCall = "gfxsrc_ts_after(vm);";
        const char *reclaim   = "n48_fr_reclaim_hold(&gKsRing, frHold, frNHold);";
        const char *expire    = "n48_fr_expire(&gKsRing, 1u, frMaintNow";
        ck("PIN 446 fix3: expiry runs AFTER the tgtsample AFTER call", strstr(src, afterCall) && strstr(src, expire) &&
           strstr(src, afterCall) < strstr(src, expire), 1u);
        ck("PIN 446 fix3: reclamation runs AFTER the tgtsample AFTER call", strstr(src, afterCall) && strstr(src, reclaim) &&
           strstr(src, afterCall) < strstr(src, reclaim), 1u);
        ck("PIN 446 fix3: the bare (hold-less) reclaim is GONE from the kext", pin_count(src, "n48_fr_reclaim(&gKsRing"), 0u);
    }
    {
        const char *guardIf   = "if (frGuard != N48_FR_GUARD_OK) {";
        const char *guardLine = "HWLOG(N48_FR_GUARD_REFUSED_FMT, here.seq";
        const char *ksLine    = "gfx-commit: COMMIT WITHDRAWN AT THE KEYSTONE";
        ck("PIN 446 fix4: the guard refusal has its own line, exactly once", pin_count(src, guardLine), 1u);
        ck("PIN 446 fix4: ... chosen by the guard's own answer, inside the refusal branch, before the keystone line",
           strstr(src, refuseBranch) && strstr(src, guardIf) && strstr(src, guardLine) && strstr(src, ksLine) &&
           strstr(src, refuseBranch) < strstr(src, guardIf) && strstr(src, guardIf) < strstr(src, guardLine) &&
           strstr(src, guardLine) < strstr(src, ksLine) && strstr(src, ksLine) < strstr(src, commitBranch), 1u);
    }

    // Item 8: the DEFERRED/APPLYING/LOST-TO-A-FREE lines all name gKsLastFlightSeq (the newest flight).
    ck("PIN item8: the DEFERRED/APPLYING/LOST-TO-A-FREE lines all name the newest flight",
       pin_count(src, "gKsLastFlightSeq, kdBlockingSeq"), 3u);

    // Item 9a: the ledger un-feed sites QUEUE (no lock held there); the queue drains under gXdLock next frame.
    ck("PIN item9a: all three un-feed sites queue, none apply the ledger touch directly",
       pin_count(src, "n48_dl_unfeed_queue(&gXdLedUnfeedQ"), 3u);
    ck("PIN item9a: the queue is drained exactly once, under gXdLock, at the next frame's top",
       pin_count(src, "n48_dl_unfeed_drain(&gXdLed, &gXdLedUnfeedQ);"), 1u);
    w_source_pins(src);   // build 0.0.497: switch 64's reachability, in the kext's own order
    x_source_pins(src);   // build 0.0.498: switch 65's reachability, in the kext's own order
    free(src);
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.411 — THE REPORT LINES, MEASURED AT THEIR WIDEST ARGUMENTS. and both recorded a
// length claim in prose that was false; the formats now live in gfx_keystone.h and are measured here against the
// brief's 480-body-byte cap. A %u is passed 4294967295, a %#x 4294967295, a %#llx the 18-byte maximum, and every %s
// the longest name the function it comes from can return.
// ---------------------------------------------------------------------------------------------------------------------
static const char *ks_widest_name(const char *(*f)(uint32_t), uint32_t n, uint32_t *lenOut)
{
    const char *w = "";
    uint32_t lw = 0;
    for (uint32_t i = 0; i <= n; i++) {
        const char *s = f(i);
        const uint32_t l = (uint32_t)std::strlen(s);
        if (l > lw) { lw = l; w = s; }
    }
    if (lenOut) *lenOut = lw;
    return w;
}
static void report_lines()
{
    char b[2048];
    const uint32_t U = 4294967295u;
    const char *wv = ks_widest_name(&n48_ks_name, N48_KS_REASONS, nullptr);
    const char *wa = ks_widest_name(&n48_ks_arm_name, N48_KSA_ARM_REASONS, nullptr);
    struct { const char *what; int n; } L[] = {
        { "REPORT line 1",   std::snprintf(b, sizeof b, N48_KS_REPORT_FMT, U, U, U, U, U, U, U, U, U, U, U, U, U, U,
                                            U, U, 0xffffffffffffffffull, U) },
        { "REPORT names",    std::snprintf(b, sizeof b, N48_KS_REPORT_NAMES_FMT, wv, wa) },
        { "REPORT tally",    std::snprintf(b, sizeof b, N48_KS_REPORT_TALLY_FMT, U, U, wv) },
        // 0.0.426 (Part B2): the per-commit VERDICT lines, at their widest (the widest verdict name, every number max).
        { "VERDICT line 1",  std::snprintf(b, sizeof b, N48_KS_VERDICT_L1_FMT, U, wv, U, U, U, U, U, U,
                                            0xffffffffffffffffull, 0xffffffffffffffffull) },
        { "VERDICT line 2",  std::snprintf(b, sizeof b, N48_KS_VERDICT_L2_FMT, U, U, U, 0xffffffffffffffffull,
                                            0xffffffffffffffffull, U, 0xffffffffffffffffull, U, U, U, U, U,
                                            0xffffffffffffffffull, U, U, U, U) },
    };
    for (auto &l : L) {
        if (!gQuiet) printf("      %-30s %4d bytes\n", l.what, l.n);
        ck(l.what, (l.n > 0 && (unsigned)l.n <= N48_KS_REPORT_BODY_CAP) ? 1u : 0u, 1u);
    }
}

int main(int argc, char **argv)
{
    printf("gfx_keystone_test - X10: the COMMIT-time root keystone\n\n");
    checks(ev_real, pm_real, bk_real, df_real);
    h2_checks(0u, &n48_ks_withdraw_hold);   // 0.0.423: H1 across all 7 verdicts, and H2's interleavings
    pfa_checks();   // 0.0.431: H2 extension - the refusal RESTORES the previous flight, it does not clear
    h2_two_commits_plus_refusal();   // C5 part 2 (build 0.0.460): H2 extended to two commits plus a refusal, on the ring
    w_checks();   // build 0.0.497: switch 64's bounded wait, RUN F's f1
    report_lines();
    if (argc > 1) source_pins(argv[1]);
    else printf("  SKIP source pins (no AppleHardwareHook.cpp argument)\n");
    const int realFail = gFail, realRun = gRun;
    printf("\n%d check(s) on the real header, %d failed.\n", realRun, realFail);
    printf("  cross product: %llu case(s), %llu non-vacuous (at least one clause falsified)\n",
           (unsigned long long)gSweep, (unsigned long long)gSweepNonVacuous);
    printf("  inert sweep:   %llu case(s) with COMMIT not armed, %llu non-vacuous (the SAME case with COMMIT armed\n"
           "                 returns something other than NOT_ARMED, so the arm is what decided it)\n",
           (unsigned long long)gInert, (unsigned long long)gInertNonVacuous);
    printf("  deferral:      %llu case(s), %llu deferred; switch-OFF sweep %llu case(s), %llu answered NOT-A-DEFERRAL\n"
           "                 (that last pair IS the proof that the default is 0.0.383's behaviour)\n",
           (unsigned long long)gDSweep, (unsigned long long)gDSweepDefer,
           (unsigned long long)gDInert, (unsigned long long)gDInertNonVacuous);

    struct Mut { const char *name; EvalFn ev; PermitFn pm; BucketFn bk; DeferFn df; PostFn pw; WithdrawFn lv; };
    const Mut muts[] = {
        { "D1 the write picks ANY live record, not the BOUND context", ev_D1, pm_real, bk_real, df_real, nullptr, nullptr },
        { "D2 the decision runs on the CACHED identity, not a re-read", ev_D2, pm_real, bk_real, df_real, nullptr, nullptr },
        { "D3 the COMMIT-armed clause is dropped",                     ev_D3, pm_real, bk_real, df_real, nullptr, nullptr },
        { "D4 non-idempotent: it writes a second time",                ev_D4, pm_real, bk_real, df_real, nullptr, nullptr },
        { "D5 the arm proceeds although a post-write walk FAILED",     ev_real, pm_D5, bk_real, df_real, nullptr, nullptr },
        { "D6 the readback status is counted as a REFUSAL",            ev_real, pm_real, bk_D6, df_real, nullptr, nullptr },
        { "D7 a read-back MISMATCH is treated as a good write",        ev_real, pm_D7, bk_real, df_real, nullptr, nullptr },
        { "D8 an empty walk requirement PERMITS",                      ev_real, pm_D8, bk_real, df_real, nullptr, nullptr },
        { "D9 a VMID the TLB invalidate cannot serve is armed anyway",  ev_D9,  pm_real, bk_real, df_real, nullptr, nullptr },
        { "D10 a frame whose invalidate FAILED is armed anyway",        ev_real, pm_D10,  bk_real, df_real, nullptr, nullptr },
        /* 0.0.384 — the two ends of the deferral's safety envelope. */
        { "D11 a withdrawal IN FLIGHT is taken anyway (arm3/arm10)",    ev_real, pm_real, bk_real, df_D11, nullptr, nullptr },
        { "D12 a deferred withdrawal is NEVER applied",                 ev_real, pm_real, bk_real, df_D12, nullptr, nullptr },
        /* 0.0.410 — the ALREADY verdict trusts the software record over the live root[511]. */
        { "D13 ALREADY trusts the record, never the live root[511]",    ev_D13,  pm_real, bk_real, df_real, nullptr, nullptr },
        /* 0.0.411 — the two halves of the withdrawal-window rule: the stale re-arm, and the marker. */
        { "D14 (F1) a STALE record is RE-ARMED (0.0.410's K1 write)",   ev_D14,  pm_real, bk_real, df_real, nullptr, nullptr },
        { "D15 (F1) the withdrawal marker is IGNORED",                  ev_D15,  pm_real, bk_real, df_real, nullptr, nullptr },
        { "D16 (F1) the post-walk re-check NEVER withdraws",            ev_real, pm_real, bk_real, df_real, post_never, nullptr },
        /* 0.0.418 (E4) — the counter cleared as a flag: the first of two overlapping withdrawals wipes it. */
        { "D17 (E4) the withdrawal counter is CLEARED AS A FLAG",       ev_real, pm_real, bk_real, df_real, nullptr, leave_flag },
    };
    const int nm = (int)(sizeof(muts) / sizeof(muts[0]));
    int caught = 0;
    printf("\nPLANTED DEFECTS (each must be caught by at least one named check):\n");
    for (int i = 0; i < nm; i++) {
        gFail = 0; gRun = 0; gQuiet = 1;
        gSweep = gSweepNonVacuous = gInert = gInertNonVacuous = 0;
        gDSweep = gDSweepDefer = gDInert = gDInertNonVacuous = 0;
        gPostWalk = muts[i].pw ? muts[i].pw : &n48_ks_post_walk_withdrawn;
        gWithdrawLeave = muts[i].lv ? muts[i].lv : &n48_ks_withdraw_leave;
        checks(muts[i].ev, muts[i].pm, muts[i].bk, muts[i].df);
        gPostWalk = &n48_ks_post_walk_withdrawn;
        gWithdrawLeave = &n48_ks_withdraw_leave;
        const int f = gFail;
        printf("  %-60s %s (%d check(s) fail)\n", muts[i].name, f ? "CAUGHT" : "*** NOT CAUGHT ***", f);
        if (f) caught++;
    }
    // ═══ 0.0.423 (KEYSTONE-A-PRIME) — D18-D21, THE A′ PLANTED BREAKS. Each mutates the marker rule (the
    // hold, or the marker's SET point) and must be caught by at least one named H1/H2 check. The failing check names are
    // printed (gFailOnly) so each break's signature is in the log, not only a count. D20 additionally demands the
    // PRE-EMPTED-MARK shape be present (option (a): a decision landed before P but its mark had not).
    struct HMut { const char *name; HoldFn hold; uint32_t setLate; uint32_t needOther, needPreempt; };
    const HMut hm[] = {
        { "D18 hold is ALWAYS 1 (today: a DEFERRING unmap over-refuses)",          hold_always_one,       0u, 0u, 0u },
        { "D19 hold is ALWAYS 0 (option (b): every unmap releases at decision)",   hold_always_zero,      0u, 1u, 0u },
        { "D20 option (a) as worded: the marker is set LATE, at the clear",        &n48_ks_withdraw_hold, 1u, 1u, 1u },
        { "D21 option (b) as worded, switch 22 OFF",                               hold_always_zero,      0u, 1u, 0u },
    };
    const int nh = (int)(sizeof(hm) / sizeof(hm[0]));
    printf("\nH2 PLANTED DEFECTS (0.0.423, ; each must be caught by a named check):\n");
    for (int i = 0; i < nh; i++) {
        gFail = 0; gRun = 0; gQuiet = 1; gFailOnly = 1;
        gH2On = H2Counts {}; gH2Off = H2Counts {};
        h2_checks(hm[i].setLate, hm[i].hold);
        const int f = gFail;
        const bool need = (!hm[i].needOther || gH2Off.other > 0u) && (!hm[i].needPreempt || gH2Off.preempt > 0u);
        const bool ok = (f != 0) && need;
        printf("  %-72s %s (%d fail; hazard: other %llu, pre-empted-mark %llu)\n", hm[i].name,
               ok ? "CAUGHT" : "*** NOT CAUGHT ***", f, (unsigned long long)gH2Off.other,
               (unsigned long long)gH2Off.preempt);
        if (ok) caught++;
    }
    // ═══ 0.0.431 — D23, THE 0.0.430 CLEAR PLANT. It clears the flight record on a refusal instead of
    // restoring the previous flight, so an unmap can WITHDRAW root[511] before the earlier committed frame's end of
    // pipe. The hazard check must fail. Mutation is done by substituting the update function, exactly as D16/D17
    // substitute the post-walk and the counter.
    printf("\n PLANTED DEFECT (0.0.431, ; must be caught by a named check):\n");
    {
        gFail = 0; gRun = 0; gQuiet = 1; gFailOnly = 1;
        gFlightUpdate = &flight_update_d23;
        pfa_checks();
        gFlightUpdate = &flight_update_real;
        const int f = gFail;
        printf("  %-72s %s (%d fail)\n", "D23 the refusal CLEARS the flight (0.0.430) instead of restoring it",
               f ? "CAUGHT" : "*** NOT CAUGHT ***", f);
        if (f) caught++;
    }
    gQuiet = 0;
    gFailOnly = 0;
    const int mutTotal = nm + nh + 1;
    printf("\nmutants caught %d/%d\n", caught, mutTotal);
    const bool pass = realFail == 0 && caught == mutTotal;
    printf("gfx_keystone: %s\n", pass ? "N48-KEYSTONE-TEST-PASS" : "FAIL");
    return pass ? 0 : 1;
}
