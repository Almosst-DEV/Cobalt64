// gfx_keystone.h — X10: MAY THE ROOT KEYSTONE BE ARMED FOR *THIS* COMMITTING FRAME, AND MAY THE FRAME THEN RUN?
// Pure C, host-tested by tests/gfx_keystone_test.cpp (with planted defects); the kext compiles the SAME header.
// 0.0.374. DEFAULT-INERT: every clause below is unreachable unless COMMIT is armed (clause 1).
//
// ---------------------------------------------------------------------------------------------------------------------
// WHY THIS EXISTS — arm3 and the four closed doors
// ---------------------------------------------------------------------------------------------------------------------
// arm3 COMMITTED a frame: the IB was rewritten in place, read back clean, SPARED by the ring walk, and the CP ran it.
// 290 ms later the GPU faulted — GCVM_L2_PROTECTION_FAULT_STATUS 0x0024295d, VMID 2, walker error 6 (NACK), WRITE,
// FAULT ADDR -> VA 0x23f0580000 — with CP_IB1_BASE naming the committed IB.
//
// 0x23f0580000 is NOT a mistranslated address. It is `gRingMap.vaBase + XLAT12_GE_RING_POS_OFF` (0x00580000,
// xlat12_ib.h:217) — THE GE POSITION RING BASE THAT OUR OWN TRANSLATED STREAM PROGRAMS, the second of the three ring
// bases in the 15-dword NGG ring block (GE_POS_RING_BASE 0x309a0 = (base + 0x580000) >> 16). The GE wrote to it, the
// walker found root[511] EMPTY in WindowServer's table, and NACKed. Our own source records the same signature to the
// bit at AppleHardwareHook.cpp:7383 — "the GE had already walked the empty slot and NACKed ... The fix was correct and
// simply too late." arm3 is r96 again at a different offset, for the same reason.
//
// So the missing step is an ORDERING one: root[511] must be armed IN THE COMMITTING CONTEXT BEFORE THE DOORBELL.
// proved every path on an ARMED recipe is shut: `rootwritews` before `wskill` has no root to write (the pre-kill
// compositor never maps); after `wskill` the window between the root appearing and the committing frame is 154 ms,
// which no step list over ssh can hit; the mapVA `ARM NOW` trigger correctly refuses at G3 (root[0] is zero AT the
// transition, by construction — review change 3 closed that door after r96 deliberately); and
// render_drain_keystone() — the right write, in the right context, at writeTail before Apple's doorbell — is gated
// `gGfxNeuter != 1u`, so it runs only when the ring neuter is NOT armed, which every armed run must hold armed.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE CALL SITE THIS HEADER SERVES, AND WHY IT IS THE COMMIT GATE AND NOT writeTail
// ---------------------------------------------------------------------------------------------------------------------
// Two candidates were weighed:
//   (a) call render_drain_keystone() at writeTail independently of the suite route;
//   (b) call it from the COMMIT gate — hook_gfxCommitIB's TRANSLATE branch, after the one-shot token matched and
//       BEFORE Apple's original writes the INDIRECT_BUFFER packet (and therefore before writeTail and the doorbell).
// (b) wins on all four things this header cares about:
//   1. NARROWEST WINDOW. The TRANSLATE branch is unreachable unless gfxsrc_decide_frame returned TRANSLATE, which
//      requires gXdArm == COMMIT and n48_cm_gate == COMMIT; and the one-shot means at most ONE frame per boot can
//      reach it. (a) runs on every GFX writeTail and would need a COMMIT test bolted on from outside.
//   2. STRONGEST IDENTITY AT THE INSTANT. At (b) the frame's own ws_classify result is in hand: BOUND by owner, the
//      frame's VMID's hardware page-table base equals the bound root, on the VMID recorded for that binding. (a) has
//      only CONTEXT2 (a register) and `ws_vmid_is_ws_now(2u)` — a HARD-CODED VMID 2, which is the defect class
//      ("a VMID number is an accident of order, never an identity").
//   3. A REFUSAL HAS SOMEWHERE TO GO. At (b) a refusal falls through to the existing neuter path: the frame is NOPed
//      and the CP never runs it. At (a) the gate has already answered COMMIT and the IB is already rewritten; there
//      is no way left to un-commit.
//   4. IT DOES NOT TOUCH gGfxNeuter. The ring neuter stays armed, which is the whole point.
// The cost of (b), stated: it does nothing for the render-drain suite route on neuter-disarmed runs. Those are not
// armed runs, and armed runs are what arm3 broke.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT THE PREDICATE GUARANTEES
// ---------------------------------------------------------------------------------------------------------------------
//  * IT DOES NOTHING UNLESS COMMIT IS ARMED (clause 1). A judge-only run behaves exactly as 0.0.372 did.
//  * IT WRITES ONLY THE ROOT OF THE CONTEXT ws_ident HAS BOUND — and the binding is RE-VALIDATED AT THE INSTANT of
//    the write (clauses 4-8), never the copy ws_classify took earlier in the same call. The caller re-runs the whole
//    selection (liveness + snapshot + n48_ws_update_table) under gWsLock and feeds THOSE values in as `now_*`; the
//    values the frame was judged on come in as `frame_*`, and clause 6 refuses if they differ in any field.
//  * IT IS IDEMPOTENT (clauses 10-11). An entry already armed for this context is a LOGGED NO-OP that still lets the
//    frame run once the walks prove the mapping AND the entry's own recorded invalidate acknowledged for the VMID it
//    is armed on (0.0.374); a second ATTEMPT with no armed entry refuses outright.
//    K1 (0.0.410) / F1 (0.0.411): "ALREADY ARMED" IS PROVEN BY A LIVE READ of root[511] equal
//    to the recorded entry, not by the record's `wrote` latch. A record that says `wrote 1 withdrawn 0` while the live
//    slot differs (arm26's f16: zero, after the deferred withdrawal was applied) is a fail-closed REFUSAL
//    (REC_MISMATCH) — withdrew K1's re-arm write, so the keystone never repairs the record; the only writers stay
//    the first arm and hook_unmapVA's own re-arm. And while an unmap that did NOT decide DEFER is between its entry and
//    its exit the whole arm refuses (N48_KS_WITHDRAWING), because a write in that window is the hazard the withdrawal
//    exists to prevent. 0.0.423: a call that decides DEFER leaves root[511] standing and releases the
//    marker at once (n48_ks_withdraw_hold), so it no longer refuses a frame nothing threatens ('s over-refusal).
//  * EVERY REFUSAL HAS ITS OWN NAME AND ITS OWN LOG LINE (n48_ks_name).
//  * THE ARM IS PERMITTED ONLY IF THE MAPPING IS PROVEN AFTERWARDS (n48_ks_arm_permitted): the write must have read
//    back byte-identical, THE INVALIDATE FOR THE ARMED VMID MUST HAVE BEEN ACKNOWLEDGED (0.0.374), and
//    the post-write walk must resolve EVERY required VA through THAT context's own root. A committed frame never
//    runs against an unproven mapping - and a mapping the TLB was never told about is not proven, which neither the
//    read-back nor the walk can see.
//
// WITHDRAWAL is the existing one and is not re-implemented here: rootwrite_arm_context records `wrote` on the
// VmCtxObs BEFORE the write, so hook_unmapVA's withdrawal (root freed), hook_releaseVMContext's release observation
// and a WindowServer rebind (n48_ws_gone) all see and retire this entry exactly as they see `rootwritews`'s.
#ifndef N48_GFX_KEYSTONE_H
#define N48_GFX_KEYSTONE_H

#include <stdint.h>

/* The verdict. 0 is the only one that WRITES; N48_KS_ALREADY is the only other one that may let the frame run. */
enum {
    N48_KS_OK = 0,          /* write it */
    N48_KS_NOT_ARMED,       /* COMMIT is not armed — the whole path is inert (the judge-only case) */
    N48_KS_NO_TOKEN,        /* the one-shot token did not match this submission, or no gate answer stands */
    N48_KS_FRAME,           /* this frame was not judged WindowServer's by owner */
    N48_KS_NOT_BOUND,       /* ws_ident is not BOUND at the instant of the write */
    N48_KS_INCOMPLETE,      /* the re-validation's snapshot was incomplete: "exactly one" is unprovable */
    N48_KS_MOVED,           /* the binding at the instant is not the one this frame was judged on */
    N48_KS_VMID,            /* the recorded VMID is not this frame's, or that VMID's hardware base is not the root */
    N48_KS_TLB_VMID,        /* the bound VMID is one the invalidate CANNOT serve (out of the 16-VMID range) */
    N48_KS_NO_RECORD,       /* no live VM-context record IS the bound context (by seq AND ctx AND root) */
    N48_KS_REC_MISMATCH,    /* the record is not live, its identity did not re-read, or its root is not the bound root */
    N48_KS_ALREADY,         /* this context is ALREADY armed: a no-op, and the mapping is claimed */
    N48_KS_SPENT,           /* a second attempt this boot with nothing armed: refuse rather than retry */
    N48_KS_RINGMAP,         /* this boot has no ring mapping to point the PDE at */
    N48_KS_ARM_CAP,         /* the global blast-radius bound on armed contexts is already reached */
    N48_KS_WITHDRAWING,     /* 0.0.423: an unmap that did NOT decide DEFER is between its entry and its exit */
    N48_KS_REASONS
};

/* The evidence, gathered by the caller at the instant of the write. Nothing here is remembered across calls except
 * `attempts`, which is what makes the second call a no-op. */
typedef struct {
    /* 1. THE ARM. `arm_commit` is passed in rather than hard-coded so this header owns no copy of gfx_src_decide.h's
     *    enum; the kext static_asserts them equal. */
    uint32_t arm_now, arm_commit;
    uint32_t token_matched;      /* n48_cm_token_match said THIS submission is the verified rewrite */
    uint32_t gate_seq;           /* the gate answer's sequence; 0 = no answer stands */

    /* 2. THE FRAME, AS ws_classify JUDGED IT. These are the CACHED values — present only so clause 6 can refuse when
     *    the re-validation disagrees with them. Nothing is decided from them alone. */
    uint32_t frame_verdict;      /* N48_WSF_JUDGE (0) required */
    uint32_t frame_vmid;
    uint32_t frame_ws_seq;
    uint64_t frame_ws_ctx, frame_ws_root;

    /* 3. THE RE-VALIDATION, READ AGAIN AT THE INSTANT OF THE WRITE (the whole ws_ident selection re-run). */
    uint32_t now_state;          /* N48_WS_BOUND (1) required */
    uint32_t now_complete;       /* the context table and liveness pass did not overflow */
    uint32_t now_seq, now_vmid;
    uint64_t now_ctx, now_root;
    uint32_t now_hw_ok;          /* gfxc_vm_open_vmid(now_vmid) succeeded */
    uint64_t now_hw_root;        /* that VMID's GCVM_CONTEXTn_PAGE_TABLE_BASE, read now */

    /* 4. THE RECORD WE WOULD HAND TO rootwrite_arm_context. */
    uint32_t rec_found, rec_live, rec_id_ok, rec_seq;
    uint64_t rec_ctx, rec_root;
    uint32_t rec_wrote, rec_withdrawn;

    /* 4b. K1 — THE LIVE READ OF root[511] AT THIS INSTANT. `rec_wrote` is a SOFTWARE latch, and it can
     *     outlive the entry it describes: arm26's f16 had `wrote 1 withdrawn 0` while the deferred withdrawal had just
     *     been APPLIED and root[511] read ZERO, so clause 10 returned ALREADY over a slot that was not there and the
     *     walks then refused the frame. The ALREADY verdict is therefore allowed only when the live value EQUALS the
     *     entry the record says it wrote. The read is the SAME MM read rootwrite_arm_context's guard and hook_unmapVA's
     *     withdrawal already perform at root + kRingRootSlot*8 — no new mechanism, no new write. */
    uint32_t rec_slot_ok;        /* 1 only if the live root[511] read succeeded */
    uint64_t rec_slot_live;      /* its value; meaningful only when rec_slot_ok is 1 */
    uint64_t rec_written;        /* e->rootWritten: the entry the record claims is standing */

    /* 4c. 0.0.423 (KEYSTONE-A-PRIME) — THE PER-CONTEXT "AN UNMAP THAT DID NOT DECIDE DEFER" MARKER, read
     *     at THIS instant with a full barrier. `hook_unmapVA` increments it at ENTRY, before it reads gKsFlight or
     *     decides anything. A call that decides N48_KSD_DEFER clears nothing (its clear is gated on `!deferNow`), so it
     *     RELEASES the marker at once (n48_ks_withdraw_hold == 0) and the keystone is free to arm against a slot that
     *     call never touches. Every other call HOLDS it to its exit, after its own re-arm/free/refusal branch, so while
     *     it is NON-ZERO that call is between its entry and its exit (around its clear of root[511] and Apple's
     *     unmapVA).: arm26's f16 ran the keystone inside exactly that window, so a write here would land our PDE
     *     in Apple's table while the page is being unmapped - the hazard the withdrawal exists to prevent. The keystone
     *     therefore REFUSES while it is set: no write, no wait.
     *     0.0.418 (E4): it is a COUNTER, not a flag — two overlapping unmaps of one context must both be visible, so
     *     the first to finish leaves it NON-ZERO while the second is still in its window (n48_ks_withdrawing). */
    uint32_t rec_withdrawing;

    /* 5. IDEMPOTENCE and the mapping. */
    uint32_t attempts;           /* keystone-from-COMMIT attempts already made this boot */
    uint32_t ringmap_built;
    /* 6. The same global blast-radius bound render_drain_keystone() honours (gArmed / kArmMaxContexts): each write is
     *    guarded and self-withdrawing, so this bounds how many of Apple's page tables we can ever be standing in. */
    uint32_t armed_contexts, armed_cap;
} n48_ks_in;

/* 0.0.374 — WHAT THIS CLAUSE USED TO BE, AND WHY IT IS NOT THAT ANY MORE.
 * Until 0.0.373 this was `#define N48_KS_INVALIDATED_VMID 2u` and clause 8 refused any bound VMID but 2, because the
 * shared write path (rootwrite_arm_context) called navi48_gmc_flush_tlb_vmid(2, 0) with the 2 WRITTEN INTO THE CALL.
 * That made the clause a restatement of the defect it exists to prevent: "a VMID number is an accident of order,
 * never an identity". arm3 and arm5 bound VMID 2; hp4 bound VMID 3. On the VMID-3 side of that coin flip the keystone
 * refused before any write and the hardware window was spent for nothing.
 * The write path now takes the VMID as an argument (RootArmExtra::tlbVmid), so the invalidate covers whatever VMID
 * clauses 3/6/7 have ALREADY bound, re-read and proven in hardware. Clause 8 therefore no longer asks "is it 2"; it
 * asks only the one question that is still genuinely about the invalidate: CAN it serve this VMID AT ALL.
 * The boundary is the invalidate's own, not a guess: navi48_gmc_flush_tlb_vmid (Navi48Bringup.cpp:3033) returns false
 * for `vmid > 15` without touching a register, because gmc_flush_gpu_tlb builds PER_VMID_INVALIDATE_REQ as `1u << vmid`
 * into a 16-bit field (gmc_v12_0.cpp:1658). A VMID above 15 would shift out of the field: the request would be issued
 * for the WRONG VMID or for none, and the entry would be armed behind a TLB that never heard about it. The software
 * page walk CANNOT catch that case — it reads the table, not the TLB — so it has to stay a clause.
 * VMID 0 is refused one clause earlier, at clause 7's `now_vmid == 0`, and that is unchanged. */
#define N48_KS_MAX_INVALIDATE_VMID 15u

static inline uint32_t n48_ks_eval(const n48_ks_in *k)
{
    if (!k) return N48_KS_NOT_ARMED;
    /* 1. THE SWITCH. Everything after this line is unreachable in a judge-only run. */
    if (k->arm_now != k->arm_commit) return N48_KS_NOT_ARMED;
    /* 2. THE ONE-SHOT'S OWN IDENTITY: this submission is the frame the gate verified. */
    if (!k->token_matched || k->gate_seq == 0u) return N48_KS_NO_TOKEN;
    /* 3. THE FRAME WAS JUDGED WINDOWSERVER'S BY OWNER. */
    if (k->frame_verdict != 0u) return N48_KS_FRAME;
    /* 4-5. THE RE-VALIDATION STANDS ON ITS OWN. */
    if (!k->now_complete) return N48_KS_INCOMPLETE;
    if (k->now_state != 1u) return N48_KS_NOT_BOUND;
    if (k->now_root == 0u || k->now_ctx == 0u) return N48_KS_NOT_BOUND;
    /* 6. AND IT AGREES WITH WHAT THIS FRAME WAS JUDGED ON, IN EVERY FIELD. A cached decision is not evidence about
     *    the instant of the write; a decision that MOVED between the judgement and the write is a refusal. */
    if (k->now_seq != k->frame_ws_seq || k->now_ctx != k->frame_ws_ctx || k->now_root != k->frame_ws_root)
        return N48_KS_MOVED;
    /* 7. THE VMID: recorded, this frame's, and still naming the bound root in hardware. */
    if (k->now_vmid == 0u || k->now_vmid != k->frame_vmid) return N48_KS_VMID;
    if (!k->now_hw_ok || k->now_hw_root == 0u || k->now_hw_root != k->now_root) return N48_KS_VMID;
    /* 8. The invalidate the shared write path issues is for THIS VMID (threaded through RootArmExtra::tlbVmid since
     *    0.0.374). All that is left to check is that the invalidate can serve it at all. */
    if (k->now_vmid > N48_KS_MAX_INVALIDATE_VMID) return N48_KS_TLB_VMID;
    /* 9. THE RECORD IS THE BOUND CONTEXT — by create #, by object pointer, and by root. Not by CONTEXT2, not by
     *    "the first live record with a root", not by VMID. */
    if (!k->rec_found) return N48_KS_NO_RECORD;
    if (!k->rec_live || !k->rec_id_ok) return N48_KS_REC_MISMATCH;
    if (k->rec_seq != k->now_seq || k->rec_ctx != k->now_ctx || k->rec_root != k->now_root)
        return N48_KS_REC_MISMATCH;
    /* 9b. 0.0.411 — NO WRITE, NO WAIT, WHILE AN UNMAP THAT DID NOT DECIDE DEFER IS IN PROGRESS.
     *     showed the keystone running on the commit thread while hook_unmapVA, on another thread, was between its clear
     *     of root[511] and its re-arm. A write there would leave our PDE in Apple's page table during Apple's own
     *     unmapVA - exactly the hazard the withdrawal exists to prevent - so the marker (field 4c), re-read here under a
     *     full barrier, refuses the whole arm. 0.0.423: the marker counts ONLY unmaps that did not decide
     *     DEFER, so a call that leaves root[511] standing releases it at once and no longer over-refuses. This is
     *     checked BEFORE the ALREADY clause, because a "no-op" that lets the frame run against a slot being taken down is
     *     no safer than a write. */
    if (k->rec_withdrawing) return N48_KS_WITHDRAWING;
    /* 10. IDEMPOTENCE, first half: an entry already standing for this context is a NO-OP, not a second write.
     *     K1 — "STANDING" IS PROVEN AGAINST THE HARDWARE, NOT BY THE RECORD'S OWN LATCH. The live
     *     root[511] read (field 4b) is the same MM read rootwrite_arm_context's G4 and hook_unmapVA's withdrawal
     *     already perform. 0.0.411 — K1's RE-ARM IS WITHDRAWN. The three outcomes are now:
     *       - the live value equals the entry the record wrote  -> the entry IS standing: ALREADY, a no-op.
     *       - the live value DIFFERS (arm26's f16: zero, after the withdrawal was applied while `wrote` stayed 1)
     *         -> FAIL CLOSED: REC_MISMATCH.: the keystone's re-arm write is the thing that caused the hazard
     *         above, so a disagreement is never repaired here. The only writers stay the first arm and the unmap
     *         thread's own re-arm.
     *       - the slot could NOT be read, or the record claims an entry of zero -> nothing is proven, so the same
     *         fail-closed refusal. "We could not read" must not become "write", exactly as it must not become
     *         "the walks resolved" or "the TLB was told". */
    if (k->rec_wrote && !k->rec_withdrawn) {
        if (!k->rec_slot_ok || k->rec_written == 0u) return N48_KS_REC_MISMATCH;
        if (k->rec_slot_live != k->rec_written) return N48_KS_REC_MISMATCH;
        return N48_KS_ALREADY;
    }
    /* 11. IDEMPOTENCE, second half: we have been here before and nothing is armed, so the first attempt failed.
     *     Refuse rather than retry — a retry would be a second write into another process's page table on evidence
     *     that has already once proved insufficient. (0.0.410's K1 exception — a record that claimed `wrote &&
     *     !withdrawn` re-arming — is gone with the re-arm itself: that case now refuses at clause 10, above.) */
    if (k->attempts) return N48_KS_SPENT;
    /* 12. There must be something to point the PDE at. */
    if (!k->ringmap_built) return N48_KS_RINGMAP;
    /* 13. And the blast-radius bound, honoured here exactly as render_drain_keystone() honours it. */
    if (k->armed_cap == 0u || k->armed_contexts >= k->armed_cap) return N48_KS_ARM_CAP;
    return N48_KS_OK;
}

/* 1 only for the verdict that performs a write. */
static inline uint32_t n48_ks_write_needed(uint32_t v) { return v == N48_KS_OK ? 1u : 0u; }
/* 1 for the two verdicts under which the mapping is CLAIMED to exist. Claimed is not proven: see below. */
static inline uint32_t n48_ks_mapping_claimed(uint32_t v)
{
    return (v == N48_KS_OK || v == N48_KS_ALREADY) ? 1u : 0u;
}

/* THE ARM ITSELF — what must be true before the COMMITTED FRAME MAY RUN.
 *   verdict      n48_ks_eval's
 *   guard        rootwrite_arm_context's G-verdict (0 = every guard passed); ignored when no write was attempted
 *   write_status rootwrite_arm_context's RootArmResult.status: 2 = WRITTEN and the read-back MATCHED. 3 is a
 *                read-back MISMATCH and the function still returns "ok", so the status must be checked separately —
 *                that is the fail-open shape this clause exists to close.
 *   tlb_ack      1 ONLY IF the GFXHUB invalidate for the VMID THIS ENTRY IS ARMED ON was ACKNOWLEDGED. See the
 *                N48_KSA_TLB_NOACK clause below for what the caller must and must not put here.
 *   walk_bits    one bit per required VA, set only when the post-write walk resolved it through THAT context's own
 *                root to the physical address our carve says it must have
 *   walk_need    the required mask
 * A walk that did not run leaves its bit clear, so "we forgot to walk" and "it did not resolve" refuse identically.
 *
 * 0.0.374 — EVERY REFUSAL HERE NOW HAS ITS OWN NAME TOO. Until this half of the rule was a bare
 * bool, so the four different reasons a proven-looking arm could be refused all reached the log as "REFUSED". The
 * clauses and their order are UNCHANGED apart from the new one; n48_ks_arm_permitted is defined as
 * `n48_ks_arm_reason(...) == N48_KSA_OK`, so the predicate and the name can never disagree. */
enum {
    N48_KSA_OK = 0,            /* the committed frame may run */
    N48_KSA_NOT_CLAIMED,       /* the verdict does not claim a mapping at all */
    N48_KSA_GUARD,             /* rootwrite_arm_context refused at a guard */
    N48_KSA_STATUS,            /* the write did not read back byte-identical (3), or never landed (0/1) */
    N48_KSA_TLB_NOACK,         /* the invalidate for the armed VMID was NOT acknowledged — 0.0.374 */
    N48_KSA_NOTHING_TO_PROVE,  /* walk_need == 0: a gate with nothing to prove is not a gate */
    N48_KSA_WALK,              /* a required VA did not resolve through this context's own root */
    N48_KSA_WITHDRAWN,         /* 0.0.411: a withdrawal began, or root[511] changed, after the walks */
    N48_KSA_ARM_REASONS
};

/* 0.0.374 — WHY AN UNACKNOWLEDGED INVALIDATE MUST REFUSE, AND WHY THAT IS NEW.
 * THE KEYSTONE'S WHOLE PURPOSE is to guarantee that its root[511] write is VISIBLE TO THE GPU before the committing
 * frame's doorbell. The write itself is proven twice already — the read-back (write_status == 2) proves the bytes
 * landed in the page, and the four walks prove the PAGE TABLE resolves. NEITHER CAN SEE THE TLB. If the GFXHUB
 * invalidate was not acknowledged, the walker may still be serving the cached EMPTY root[511] that NACKed arm3, and
 * the frame would run on a guarantee we do not have.
 * This was masked until and is exposed BY. While clause 8 refused every bound VMID but 2 we only ever
 * armed on VMID 2, whose invalidate always ACKed in every run on record; retired that narrowing, so the keystone
 * can now arm on VMIDs we have never armed on, where nothing has ever been measured.
 * WHAT tlb_ack MUST BE. It is the ACK FOR THE VMID THE STANDING ENTRY IS ARMED ON, and it is 1 only when BOTH are
 * known: the invalidate returned true, AND it was issued for that VMID. "Not recorded" is 0 and refuses — the
 * fail-closed direction, and the same rule as walk_bits ("we forgot to walk" refuses like "it did not resolve").
 * IT APPLIES TO N48_KS_ALREADY AS WELL, AND THAT IS DELIBERATE. An idempotent re-arm performs NO invalidate at all
 * (AppleHardwareHook.cpp's no-op branch writes nothing and calls nothing), so on ALREADY there is no acknowledgement
 * from THIS call to look at; the one that matters belongs to whichever earlier write left the entry standing. Reading
 * a global "last arm" there would be worse than no gate — that result may be another context's, or another VMID's.
 * The caller therefore reads the ack RECORDED ON THE ENTRY, and an entry with nothing recorded refuses. Exempting
 * ALREADY would have been the fail-open answer: the entry a re-arm leaves standing is exactly the one whose
 * invalidate we have never checked. */
static inline uint32_t n48_ks_arm_reason(uint32_t verdict, uint32_t guard, uint32_t write_status,
                                         uint32_t tlb_ack, uint32_t walk_bits, uint32_t walk_need)
{
    if (!n48_ks_mapping_claimed(verdict)) return N48_KSA_NOT_CLAIMED;
    if (verdict == N48_KS_OK && guard != 0u) return N48_KSA_GUARD;
    if (verdict == N48_KS_OK && write_status != 2u) return N48_KSA_STATUS;
    if (!tlb_ack) return N48_KSA_TLB_NOACK;               /* 0.0.374: the bytes landed; the TLB never heard */
    if (walk_need == 0u) return N48_KSA_NOTHING_TO_PROVE;  /* a gate with nothing to prove is not a gate */
    if ((walk_bits & walk_need) != walk_need) return N48_KSA_WALK;
    return N48_KSA_OK;
}

static inline uint32_t n48_ks_arm_permitted(uint32_t verdict, uint32_t guard, uint32_t write_status,
                                            uint32_t tlb_ack, uint32_t walk_bits, uint32_t walk_need)
{
    return n48_ks_arm_reason(verdict, guard, write_status, tlb_ack, walk_bits, walk_need) == N48_KSA_OK ? 1u : 0u;
}

/* 0.0.411 — THE POST-WALK RE-CHECK, PURE so the host suite can drive it. The keystone gathers the
 * marker and root[511] BEFORE it writes, performs the four walks (a software page-table walk that reads VRAM through
 * the MM window and can take real time), and then must re-read BOTH: the marker, because hook_unmapVA can set it on
 * the client's thread the instant after the first read; and root[511], because a withdrawal that completed during the
 * walks leaves the slot ZERO or re-armed to a different value. Either change WITHDRAWS the arm and neuters the frame.
 * `slot_ok == 0` is the same fail-closed direction: the re-read could not prove the slot still holds `expected`, so
 * nothing is proven. `expected` is the entry this arm is standing on (e->rootWritten after the write, or the entry
 * the record already claims on the ALREADY path). No write, no wait: this only decides. */
static inline uint32_t n48_ks_post_walk_withdrawn(uint32_t marker_set, uint32_t slot_ok,
                                                  uint64_t slot_live, uint64_t expected)
{
    if (marker_set) return 1u;
    if (!slot_ok) return 1u;
    return (slot_live != expected) ? 1u : 0u;
}

/* =====================================================================================================================
 * 0.0.418 (an internal design note, E4) — THE WITHDRAWAL MARKER IS A COUNTER, NOT A FLAG (owed since 0.0.411).
 *
 * 0.0.411 made `VmCtxObs::ksWithdrawing` a single flag that hook_unmapVA SETS at entry and CLEARS at its one exit, and
 * the keystone refuses whenever it is set. But hook_unmapVA holds no lock across the clear -> Apple's unmapVA -> re-arm
 * span, so TWO overlapping unmaps of one context are possible: the second sets the flag, the FIRST finishes and clears
 * it, and the keystone is then let in while the second call is still inside its window - exactly the hazard
 * exists to close. The marker must count the withdrawals in progress.
 *
 * These three pure helpers are the arithmetic the kext performs atomically: `enter` is one more withdrawal in flight,
 * `leave` is one fewer and NEVER BELOW ZERO, and `withdrawing` is the predicate the keystone refuses on (non-zero, so a
 * count of 2 refuses exactly as a count of 1). Host-tested in gfx_keystone_test.cpp section 4d; the planted break is
 * the flag's own clear (`leave` returns 0 unconditionally), under which the first finishing withdrawal clears the
 * counter while the second is still in flight. 0.0.423: a call that decides DEFER clears nothing, so it
 * releases the counter early (n48_ks_withdraw_hold == 0) and the counter is also option A′'s marker. */
static inline uint32_t n48_ks_withdraw_enter(uint32_t cur) { return cur + 1u; }
static inline uint32_t n48_ks_withdraw_leave(uint32_t cur) { return cur ? cur - 1u : 0u; }
static inline uint32_t n48_ks_withdrawing(uint32_t cur)    { return cur != 0u ? 1u : 0u; }

static inline const char *n48_ks_arm_name(uint32_t r)
{
    static const char *const n[N48_KSA_ARM_REASONS] = {
        "PERMITTED - the committed frame may run",
        "REFUSED: the verdict claims no mapping at all",
        "REFUSED: rootwrite_arm_context refused at a guard - nothing was written",
        "REFUSED: the write did not read back byte-identical (status 2 is the only one that counts)",
        ("REFUSED: the GFXHUB TLB invalidate for the armed VMID was NOT ACKNOWLEDGED - the walker may still be "
         "serving a stale root[511], which is exactly what NACKed arm3"),
        "REFUSED: nothing was required to be proven - a gate with nothing to prove is not a gate",
        "REFUSED: a required VA did not resolve through this context's own root",
        "REFUSED: a withdrawal began (or root[511] changed) while the walks ran - the committed frame may not run",
    };
    return r < N48_KSA_ARM_REASONS ? n[r] : "?";
}

static inline const char *n48_ks_name(uint32_t v)
{
    static const char *const n[N48_KS_REASONS] = {
        "OK - the keystone is written",
        "REFUSED: COMMIT is not armed (this path is inert in a judge-only run)",
        "REFUSED: the one-shot token does not match this submission, or no gate answer stands",
        "REFUSED: this frame was not judged WindowServer's by owner",
        "REFUSED: ws_ident is NOT BOUND at the instant of the write",
        "REFUSED: the re-validation's snapshot was INCOMPLETE - 'exactly one' is unprovable",
        "REFUSED: the binding MOVED between the judgement and the write",
        "REFUSED: the VMID is not the recorded one, or its hardware page-table base is not the bound root",
        "REFUSED: the bound VMID is outside the 16-VMID range the TLB invalidate can serve",
        "REFUSED: no live context record IS the bound context",
        "REFUSED: the record is not live, did not re-validate, or its root is not the bound root",
        "NO-OP: this context is ALREADY armed (idempotent)",
        "REFUSED: a second attempt this boot with nothing armed - the first attempt failed",
        "REFUSED: this boot has no ring mapping to point the entry at",
        "REFUSED: the global bound on armed contexts is already reached",
        /* 0.0.426 (review follow-up, Part B2): SHORTENED so the per-commit `keystone: VERDICT` line stays under the
         * logger's 512-byte body cap at realistic widths WITHOUT dropping a field (the line is also split in two). The
         * meaning is unchanged: the marker is held by an unmap that is between its entry and its exit and did NOT defer. */
        "REFUSED: an unmap that did NOT DEFER holds the withdrawal marker (entry..exit)",
    };
    return v < N48_KS_REASONS ? n[v] : "?";
}


// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// 0.0.384 — THE WITHDRAW/RE-ARM WINDOW, AND WHY IT MUST BE DEFERRED WHILE A COMMITTED FRAME IS IN FLIGHT
// ═══════════════════════════════════════════════════════════════════════════════════════════════════════════════════
// THE MECHANISM, from arm10's own log. `hook_unmapVA` takes our entry DOWN on ANY unmap of an armed context: it clears
// root[511] to zero (low dword first), HDP-flushes, INVALIDATES the armed VMID's GFXHUB TLB, runs Apple's original
// unmapVA, and re-arms at the return if the page survived. That withdraw/re-arm pair is PREDICATE-FREE by design
// (r89: the root survived 10 of 11 unmaps, so guessing would be wrong more often than right) and it is what makes the
// teardown case safe — the page goes back to Apple's arena clean.
//
// It is also, for the duration of each pair, arm3's exact condition. arm10 logged THREE COMPLETE CYCLES between the
// COMMIT gate's OK and the first non-zero fault read — fires #5, #6 and #7 on the committing context, each
// `cleared … reads back 000000000000000000 (ZERO as intended); GFXHUB vmid-2 TLB invalidate ok` — WITH THE COMMITTED
// FRAME STILL EXECUTING. The fault that followed is bit-identical to arm3's: `0x0024295d` — VMID 2, walker error 6
// (NACK), PERMISSION_FAULTS 0x5 = VALID|WRITE in the FAILURE set (no valid translation at all), MAPPING_ERROR set,
// CID 0x14 = PA (the primitive assembler, the NGG position-ring writer), WRITE to VA 0x23f0580000 =
// `gRingMap.vaBase + XLAT12_GE_RING_POS_OFF` = GE_POS_RING_BASE. Every keystone proof on record — the read-back
// MATCHES, the TLB ack, all 16 `RESOLVES CORRECTLY` — was taken BEFORE the first withdrawal. CONFIRMED: the windows
// exist, are on the committing context, and bracket the fault. SUSPECTED: that PA's write fell INSIDE one rather than
// beside one — which is the single thing the in-window fault read (below) is added to settle.
//
// ---------------------------------------------------------------------------------------------------------------------
// THE SAFETY ENVELOPE — BOTH ENDS ARE REAL HAZARDS, AND THAT IS WHY THE ANSWER IS A BOUNDED DEFERRAL
// ---------------------------------------------------------------------------------------------------------------------
//   WITHDRAW WHILE A COMMITTED FRAME IS IN FLIGHT  =>  the walker finds root[511] absent, NACKs, and the engine hangs.
//       CONFIRMED twice: arm3 and arm10 (`channel 51 GFX is hung!`, `Engine PM4 : Failed to reset!`, then a
//       watchdog panic eight seconds after the commit).
//   NEVER WITHDRAW AT ALL  =>  a VALID PDE of ours is left standing in a page Apple has FREED. The page is the next
//       client's leaf page within ~4 s, and index 511 of that client's table would then translate through our PDE into
//       our ring. That is the hazard the withdrawal was written for and it is not negotiable.
//   BOUNDED DEFERRAL keeps both: while the frame is in flight the entry STAYS ARMED (nothing is cleared, nothing is
//       invalidated) and the request is recorded; the moment the window ends the next unmap of that context performs
//       the full withdraw/flush/invalidate exactly as today. The window is closed by the EARLIEST of an observed
//       end-of-pipe and a bounded timeout, so it can never be open indefinitely.
//
//   IF THE TIMEOUT FIRES WITH THE FRAME STILL RUNNING the withdrawal proceeds and we are back in exactly today's
//   position — the same hazard, no worse, just later (SUSPECTED, from the shape of the mechanism: the clear, the
//   flush and the invalidate are the same three operations in the same order, on the same entry).
//
//   THE ONE HAZARD THIS ADDS, STATED PLAINLY: if the very call whose withdrawal we deferred is the call in which
//   Apple FREES the root page, the page returns to the arena with our PDE still in it. We learn that only at the
//   RETURN (ctx+0x98+0x20 reads 0), by which time the page is gone and nothing can be written to it. The caller must
//   therefore COUNT AND LOG that case unconditionally and retire the record so nothing ever writes there again; the
//   residual risk is the un-cleaned PDE itself. It is bounded by the window (≤ the timeout, and at most one window per
//   committed frame) and it is the price of not hanging the engine. A reviewer should weigh exactly this trade.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT "IN FLIGHT" IS, IN STATE THAT ALREADY EXISTS
// ---------------------------------------------------------------------------------------------------------------------
//   START. `gfxsrc_commit_try`'s `if (live && reason == N48_CM_OK && n48_cm_shot_spend(&gXdShot, gXdCmToken.seq))` —
//          the one instant a rewritten IB has been written to the client and read back clean, and the instant the
//          existing code already stamps `gXdShotSpentUs = drain_now_us()`. The caller records the same stamp into its
//          own flight record (a dedicated one, so that a TORN read is detectable: see `start_us` below).
//   END, whichever comes FIRST:
//     (a) END OF PIPE OBSERVED — `n48_f828_watch.ever`, this header's neighbour in gfx_fence828.h, documented there as
//         "1 once ANY readable poll equalled `want`. STICKY: never cleared (M11)" and reached through
//         `n48_f828_state(...) == N48_F828_ST_MATCHED` / `ST_MATCHED_REVERTED` ("OURS - THE COMMITTED FRAME REACHED
//         END OF PIPE"). It is the GPU's own CACHE_FLUSH_AND_INV_TS RELEASE_MEM writing a value only we write.
//         IT IS CONSERVATIVE IN THE SAFE DIRECTION: the slot is re-read only on LATER JUDGED FRAMES
//         (`kXdF828Polls` = 8), so `ever` is set at the next judged frame AFTER retirement, never before it.
//         IT IS NOT ALWAYS AVAILABLE, and this is the honest limit of clause (a): it can only ever be 1 when the
//         fence828 switch (`gfxneuter 16 | 1 << 8`) was thrown, the rule found and rewrote the packet, the gate
//         committed that frame, and a later judged frame polled the slot. If judged frames stop arriving — which is
//         precisely what a hang does — `ever` stays 0 and clause (b) is the only end.
//         0.0.418 (E2): `ever` is STICKY and belongs to the last PROMOTED commit, so the answer must also carry that
//         commit's own FLIGHT (`n48_f828_eop_seen`): a later flight whose fence was refused never resets the latch and
//         must not be answered by it. The `eop` field below is therefore `ks_eop_seen()`, not `watch.ever` alone.
//     (b) A BOUNDED TIMEOUT. `now_us - start_us >= timeout_us`. SUSPECTED, and justified from the only measured
//         numbers this project has: arm8 and arm9 both matched at POLL 1, ~0.3 s after the commit, so 0.3 s is the
//         one measured commit-to-observed-retirement latency; arm10's hang was at +8 s. The caller's constant is
//         2 s — roughly six times the measured latency and a quarter of the measured time-to-hang.
//
// FAIL CLOSED ON UNCERTAINTY (clause order below): no input, no switch, nothing armed, no flight recorded, a TORN
// flight record, an unreadable clock, a clock that went backwards, or a zero timeout all answer WITHDRAW AS TODAY.
// A deferral is a decision to leave a VALID PDE standing, so it is granted only on evidence, never by omission —
// the same rule the rest of this header applies to `walk_bits` and to `tlb_ack`.
enum {
    N48_KSD_NOW_OFF = 0,          /* the switch is off: today's behaviour, unconditionally */
    N48_KSD_NOW_NOTHING,          /* nothing of ours is standing, so there is no withdrawal to defer */
    N48_KSD_NOW_NOT_IN_FLIGHT,    /* no committed frame is in flight */
    N48_KSD_NOW_TORN,             /* the flight record is not self-consistent, or the clock is not usable */
    N48_KSD_NOW_EOP,              /* END OF PIPE was observed: the window is over, withdraw now */
    N48_KSD_NOW_TIMEOUT,          /* the bounded deferral expired: withdraw now */
    N48_KSD_DEFER,                /* IN FLIGHT — do not clear, do not invalidate; record and apply at the end */
    N48_KSD_REASONS
};

typedef struct {
    uint32_t on;            /* the switch (`accel gfxneuter 22 | M << 8`); 0 is the default and is 0.0.383's path */
    uint32_t armed;         /* our entry is standing for this context: wrote && !withdrawn */
    uint32_t flight;        /* the flight record's `active`, written LAST by the producer */
    uint64_t start_us;      /* the flight record's stamp, written FIRST by the producer. 0 with flight=1 is TORN */
    uint32_t now_ok;        /* the clock was read at all */
    uint64_t now_us;
    uint32_t eop;           /* ks_eop_seen() for the committed frame: watch.ever AND the committed record's flight ==
                             * the current gKsFlight.seq (0.0.418 E2). 0 when the fence was never enabled, never
                             * promoted, or belongs to an earlier flight. */
    uint64_t timeout_us;    /* the bound. 0 is refused: an unbounded deferral is not a bounded one */
} n48_ksd_in;

static inline uint32_t n48_ksd_eval(const n48_ksd_in *d)
{
    if (!d) return N48_KSD_NOW_TORN;                       /* no input is not knowledge */
    if (!d->on) return N48_KSD_NOW_OFF;                    /* the default path, and it is FIRST */
    if (!d->armed) return N48_KSD_NOW_NOTHING;
    if (!d->flight) return N48_KSD_NOW_NOT_IN_FLIGHT;
    /* The producer writes `start_us` before `flight`, so `flight` without a stamp is a read that tore across the
     * producer's two stores. A clock we could not read, or one that went backwards, is the same kind of ignorance. */
    if (!d->now_ok || d->start_us == 0ull || d->now_us < d->start_us) return N48_KSD_NOW_TORN;
    if (d->timeout_us == 0ull) return N48_KSD_NOW_TORN;
    if (d->eop) return N48_KSD_NOW_EOP;                    /* the truer reason when both hold */
    if (d->now_us - d->start_us >= d->timeout_us) return N48_KSD_NOW_TIMEOUT;
    return N48_KSD_DEFER;
}

/* 1 for the ONE verdict that leaves root[511] standing. Everything else withdraws exactly as 0.0.383 did. */
static inline uint32_t n48_ksd_defer(uint32_t r) { return r == N48_KSD_DEFER ? 1u : 0u; }

/* 0.0.423 (KEYSTONE-A-PRIME) — MUST THIS UNMAP CALL HOLD THE WITHDRAWAL MARKER TO ITS EXIT?
 * A call that decides N48_KSD_DEFER clears NOTHING: hook_unmapVA's clear is gated on `!deferNow`, so the call never
 * touches root[511] and the keystone has no withdrawal to be kept out of. It therefore RELEASES the marker the
 * instant it decides (0), and the keystone may arm against a slot that call never touches — which is the whole of
 * option A′ and the fix for's POST-WALK over-refusal. Every OTHER verdict — the withdrawal that runs now, and
 * the fail-closed "nothing / switch off / torn / end-of-pipe / timeout" answers that withdraw exactly as 0.0.383 did
 * — HOLDS the marker for its whole length (1), exactly as 0.0.411 did for every call. So 1 = hold to the exit, 0 =
 * a deferral, release at once. Host-tested in gfx_keystone_test.cpp (H1 across all 7 verdicts, and H2's interleaving
 * model); the planted breaks D18 (always 1, today's over-refusal) and D19 (always 0, option (b)) must both be caught. */
static inline uint32_t n48_ks_withdraw_hold(uint32_t v) { return v == N48_KSD_DEFER ? 0u : 1u; }

/* =====================================================================================================================
 * 0.0.431 — THE PREVIOUS FLIGHT IS SAVED AND RESTORED, NOT CLEARED.
 *
 * THE DEFECT 0.0.430 LEFT. `gKsFlight` is ONE record, stamped by every commit at the gate BEFORE the keystone runs
 * (`gfxsrc_commit_try`, AHH:20729-20732). At budget > 1 a second candidate B overwrites the record of a committed
 * frame A that may still be executing. 0.0.430 then CLEARED `active` when the keystone refused B, on the true premise
 * that B never ran — but that also erased A's flight, and hook_unmapVA's deferral then answered NOT_IN_FLIGHT and let
 * an unmap WITHDRAW root[511] while A still ran: the arm3/arm10 NACK-and-hang class. It is a NEW way in:
 * before 0.0.430, B's leftover stamp incidentally covered A for its bound.
 *
 * THE FIX. At the stamp the OUTGOING record {at_us, seq, active} is SAVED together with the token seq doing the
 * stamping (`stamp_seq`). In the keystone's refusal branch, when THIS call's stamp is still the one showing, the save
 * is RESTORED (at_us, then seq, then a release fence, then active, then a SEQ_CST fence — the producer's own store
 * order, so the TORN clause can never see active=1 behind a zero stamp). A restored A is visible to the deferral, so
 * `n48_ksd_eval` answers EOP the moment A's end of pipe was observed (`ks_eop_seen` compares the committed record's
 * flight to `gKsFlight.seq`, which is A's again —'s gain, kept) and otherwise defers to A's own bound. With NO
 * earlier flight the saved record is {0, 0, 0}, so the restore leaves `active = 0` — exactly 0.0.430's clear.
 *
 * PURE, AND CALLED BY THE KEXT (source-pinned): the save and the restore decision live here so the host suite drives
 * the real functions, not a copy. `n48_ks_flight_save` takes the record that was showing and returns the record to
 * STASH (its fields are the previous flight; `stamp_seq` is who stamped over it). `n48_ks_flight_restore_ok` is the
 * "this call did the stamp" test in its only form a later stamp cannot fool: `call_seq` (THIS refusing call's token
 * seq), the saved `stamp_seq` and the live record's `seq` must all be equal, so neither a later stamp nor an aliased
 * stash can make an older call restore a flight that is not its own. It REPLACES `n48_ksd_flight_after_keystone`,
 * which the kext never called and which the 0.0.430 D22 test therefore tested vacuously. */
typedef struct {
    uint64_t at_us;      /* the previous flight's gate stamp; 0 when there was none */
    uint32_t seq;        /* the previous flight's token seq; 0 when there was none */
    uint32_t active;     /* the previous flight's `active`; 0 when there was none — this is 0.0.430's clear case */
    uint32_t stamp_seq;  /* the token seq of the commit that stamped OVER this record; 0 = never saved */
} n48_ks_flight;

/* PURE: the record to STASH when a commit whose token seq is `stamp_seq` publishes its own flight over `prev`. */
static inline n48_ks_flight n48_ks_flight_save(n48_ks_flight prev, uint32_t stamp_seq)
{
    prev.stamp_seq = stamp_seq;
    return prev;
}

/* PURE: may a refusal RESTORE the stash? `call_seq` is THIS referee's token seq and `cur_seq` the one showing in the
 * live record. Yes only when this call's stamp is the one that took the stash AND is still the one showing, so an
 * older call's refusal can never clobber a LATER commit's flight and a later stamp can never make it restore that
 * later call's stash by aliasing. */
static inline uint32_t n48_ks_flight_restore_ok(n48_ks_flight saved, uint32_t call_seq, uint32_t cur_seq)
{
    return (saved.stamp_seq != 0u && saved.stamp_seq == call_seq && call_seq == cur_seq) ? 1u : 0u;
}

/* 1 when this verdict ENDS a window that was open — the instant a recorded deferral must be applied. */
static inline uint32_t n48_ksd_ends_flight(uint32_t r)
{
    return (r == N48_KSD_NOW_EOP || r == N48_KSD_NOW_TIMEOUT) ? 1u : 0u;
}

static inline const char *n48_ksd_name(uint32_t r)
{
    static const char *const n[N48_KSD_REASONS] = {
        "WITHDRAW NOW: the in-flight deferral is OFF (default) - 0.0.383's path exactly",
        "WITHDRAW NOW: nothing of ours is standing in this context",
        "WITHDRAW NOW: no committed frame is in flight",
        "WITHDRAW NOW: the flight record is TORN or the clock is unusable - fail closed",
        "WITHDRAW NOW: END OF PIPE was observed for the committed frame - the window is over",
        "WITHDRAW NOW: the bounded deferral EXPIRED with no end-of-pipe - same hazard as 0.0.383, no worse",
        "DEFER: a committed frame is IN FLIGHT - root[511] stays armed and the request is recorded",
    };
    return r < N48_KSD_REASONS ? n[r] : "?";
}

/* =====================================================================================================================
 * 0.0.411 — THE KEYSTONE REPORT'S FORMATS, HERE SO THE HOST TEST CAN BOUND THEM.
 * The 0.0.410 lesson (and before it): a length claim written in a comment is not a check. These
 * lines are printed once per boot (keystone_report), and their worst case is measured in gfx_keystone_test.cpp against
 * the brief's 480-body-byte cap. The 480 cap is this project's instrument bound; the logger's own hard cap is
 * N48_LOG_CAP_BODY (491) and every one of these is below both at its widest arguments.
 * ===================================================================================================================== */
#define N48_KS_REPORT_BODY_CAP 480u
/* args: attempts, writes, noops, permits, blocked, stale, withdrawing, verdict, guard, status, tlbAck, walkBits,
 *       walkNeed, bracketOk, seq, vmid, root, armReason */
#define N48_KS_REPORT_FMT \
    "keystone: REPORT - attempts %u, writes %u, idempotent no-ops %u; arms PERMITTED %u, BLOCKED %u; " \
    "stale-record refusals %u, withdrawal-in-progress refusals %u. Last: verdict %u, guard %u, status %u, " \
    "TLB ack %u, walk bits %#x of %#x, bracket %u, create #%u VMID %u root %#llx; arm reason %u."
/* args: the last verdict's name, the last arm reason's name */
#define N48_KS_REPORT_NAMES_FMT "keystone:   last verdict name: %s; arm reason name: %s."
/* args: verdict index, its count, its name */
#define N48_KS_REPORT_TALLY_FMT "keystone:   verdict %u x%u - %s"

/* 0.0.426 (review follow-up, Part B2) — THE PER-COMMIT VERDICT LINES, HERE SO THE HOST TEST CAN BOUND THEM.
 * Through 0.0.425 the whole `keystone: VERDICT` record was ONE line, and with a long verdict name (verdict 15 was 130
 * bytes) its tail - the re-validation and the record half - was silently cut at the logger's 512-byte body cap./
 *'s exact failure mode, on the line a run reads to know whether a frame was withdrawn. It is now TWO lines, each
 * under the cap at its widest arguments (measured in gfx_keystone_test.cpp's report_lines), and NO FIELD WAS DROPPED:
 * line 1 carries the verdict, the arm/token identity and the frame; line 2 the re-validation, the record and the boot
 * counters. The VERDICT-15 name was also shortened (gfx_keystone.h n48_ks_name) since it dominated the width. */
/* args: verdict, name, arm, tokenMatched, gateSeq, frameVerdict, frameVmid, frameWsSeq, frameWsCtx, frameWsRoot */
#define N48_KS_VERDICT_L1_FMT \
    "keystone: VERDICT %u - %s. arm %u; token %u gate seq %u; frame: verdict %u VMID %u, WindowServer create #%u " \
    "ctx %#llx root %#llx."
/* args: now_state, now_complete, now_seq, now_ctx, now_root, now_vmid, now_hw_root, now_hw_ok, rec_found, rec_live,
 *       rec_id_ok, rec_seq, rec_root, rec_wrote, rec_withdrawn, attempts, ringmap_built */
#define N48_KS_VERDICT_L2_FMT \
    "keystone:   RE-VALIDATED AT THIS INSTANT: state %u complete %u create #%u ctx %#llx root %#llx VMID %u, that " \
    "VMID's hardware page-table base %#llx (open %u). Record: found %u live %u identity %u create #%u root %#llx, " \
    "wrote %u withdrawn %u. Attempts so far %u; ring map built %u."

/* =====================================================================================================================
 * build 0.0.497 — SWITCH 64: A BOUNDED WAIT AT THE KEYSTONE WHILE THE WITHDRAWAL MARKER IS HELD.
 *
 * WHY. RUN F's f1 was handed COMMIT and then refused VERDICT 15 because Apple's unmap of page-in copy
 * #6's temporary source map (`va 0x4000b8000 size 0x8000`) was between its ENTRY and its return when the keystone read
 * the marker - an unmap of a context in which NOTHING of ours was armed, so it could not DEFER and correctly held the
 * marker for its whole window (clause 9b). Its return line was logged between the keystone's two VERDICT lines: the
 * window closed a few log lines later. Refusing at once threw away the only commit of the arm.
 *
 * WHAT CHANGES, AND WHAT DOES NOT. With switch 64 ON the keystone, BEFORE it gathers any of its evidence (before
 * ks_revalidate, the record lookup, the marker's first read and the live root[511] read), polls the marker of the
 * frame's judged context for AT MOST N48_KS_WAIT_BOUND_US (and at most N48_KS_WAIT_MAX_POLLS short busy-waits, so a
 * clock that cannot be read still bounds it). Then EVERY check runs UNCHANGED - n48_ks_eval with its own fresh read of
 * the marker (clause 9b), the walks, the post-walk re-read (n48_ks_post_walk_withdrawn) and the bracket - all on
 * values read AFTER the wait. A marker still held at the bound is therefore today's VERDICT 15, fail-closed: the wait
 * decides NOTHING, it only moves the instant the unchanged checks are taken. KEYSTONE-A-PRIME's row "Withdraw: X<V"
 * (the unmap's exit before the keystone's evaluation) is the safe row; a release inside the bound turns RUN F's E<V<X
 * into exactly that row, and an unmap that re-enters after the release is caught by the unchanged clause 9b or the
 * post-walk re-read, as today.
 *
 * NO LOCK IS HELD WHILE WAITING (AppleHardwareHook.cpp, ks_wait_marker_bounded's comment): gXdLock is released at the
 * end of gfxsrc_decide_frame and gWsLock is taken only inside ks_revalidate, which runs after the wait. The pause is a
 * busy-wait (IODelay), never a sleep, and nothing here takes a lock.
 *
 * PURE: the loop below is the one the kext runs, through three callbacks (the marker read, the clock, the pause), so the
 * host suite drives the SAME loop with a scripted marker and clock (gfx_keystone_test.cpp section W).
 * ===================================================================================================================== */
#define N48_KS_WAIT_BOUND_US  2000u   /* ~2 ms, the brief's bound */
#define N48_KS_WAIT_POLL_US   5u      /* one pause */
#define N48_KS_WAIT_MAX_POLLS 400u    /* 400 x 5 us: the same ~2 ms when the clock is unusable */

enum {
    N48_KSW_OFF = 0,     /* switch 64 OFF: nothing is read, nothing waits - today's keystone */
    N48_KSW_NO_RECORD,   /* no live record is the frame's judged context: nothing to wait on (the checks decide) */
    N48_KSW_NOT_HELD,    /* the marker read 0 at the first look: no wait */
    N48_KSW_RELEASED,    /* the marker read 0 within the bound: the unchanged checks now run after the unmap's exit */
    N48_KSW_TIMEOUT,     /* the bound expired with the marker still held: the unchanged checks answer VERDICT 15 */
    N48_KSW_OUTCOMES
};

typedef struct {
    uint32_t (*marker)(void *ctx);   /* ONE full-barrier read of the context's ksWithdrawing counter (non-zero = held) */
    uint64_t (*now_us)(void *ctx);   /* monotonic microseconds; 0 = the clock could not be read */
    void     (*pause)(void *ctx);    /* ONE short busy-wait of N48_KS_WAIT_POLL_US; never sleeps, never takes a lock */
    void     *ctx;
} n48_ks_wait_io;

/* THE BOUND. 1 once `polls` has reached the poll cap, or (with a usable clock that has not gone backwards) once the
 * elapsed time has reached the time bound. Either alone ends the wait. */
/* build 0.0.526 item 3 (switch 81 M3): the same bound with the time bound and the poll cap as parameters; the fixed-bound
 * form below is this one at N48_KS_WAIT_BOUND_US / N48_KS_WAIT_MAX_POLLS. */
static inline uint32_t n48_ks_wait_bound_hit_b(uint32_t polls, uint32_t clock_ok, uint64_t start_us, uint64_t now_us,
                                               uint32_t bound_us, uint32_t max_polls)
{
    if (polls >= max_polls) return 1u;
    if (clock_ok && now_us >= start_us && now_us - start_us >= bound_us) return 1u;
    return 0u;
}
static inline uint32_t n48_ks_wait_bound_hit(uint32_t polls, uint32_t clock_ok, uint64_t start_us, uint64_t now_us)
{
    return n48_ks_wait_bound_hit_b(polls, clock_ok, start_us, now_us, N48_KS_WAIT_BOUND_US, N48_KS_WAIT_MAX_POLLS);
}

/* THE WAIT. `on` is switch 64 as latched for this pass; `have_rec` is 1 when the frame's judged context has a live
 * record. Returns one N48_KSW_* outcome; `*polls_out` is the number of pauses taken and `*waited_us` the elapsed time
 * (0 when the clock was unusable). OFF touches no callback at all. Every path out of the loop is either the marker
 * reading 0 (RELEASED) or the bound (TIMEOUT); the loop can take at most `max_polls` pauses (N48_KS_WAIT_MAX_POLLS in the
 * fixed-bound form below). */
static inline uint32_t n48_ks_wait_run_b(uint32_t on, uint32_t have_rec, const n48_ks_wait_io *io, uint32_t bound_us,
                                         uint32_t max_polls, uint32_t *polls_out, uint64_t *waited_us)
{
    uint32_t polls = 0u;
    uint64_t t0 = 0ull, now = 0ull;
    uint32_t out = N48_KSW_OFF;
    if (polls_out) *polls_out = 0u;
    if (waited_us) *waited_us = 0ull;
    if (!on) return N48_KSW_OFF;
    if (!have_rec || !io || !io->marker || !io->now_us || !io->pause) return N48_KSW_NO_RECORD;
    if (!io->marker(io->ctx)) return N48_KSW_NOT_HELD;
    t0 = io->now_us(io->ctx);
    now = t0;
    const uint32_t clock_ok = t0 ? 1u : 0u;
    for (;;) {
        if (n48_ks_wait_bound_hit_b(polls, clock_ok, t0, now, bound_us, max_polls)) { out = N48_KSW_TIMEOUT; break; }
        io->pause(io->ctx);
        polls++;
        now = clock_ok ? io->now_us(io->ctx) : 0ull;
        if (!io->marker(io->ctx)) { out = N48_KSW_RELEASED; break; }
    }
    if (polls_out) *polls_out = polls;
    if (waited_us) *waited_us = (clock_ok && now >= t0) ? now - t0 : 0ull;
    return out;
}
/* The fixed-bound wait (0.0.497): n48_ks_wait_run_b at N48_KS_WAIT_BOUND_US / N48_KS_WAIT_MAX_POLLS. build 0.0.526 item 3: the
 * kext calls the _b form with switch 81's bound in use (gfx_ks81.h n48_ks81_wait_bound_us: 2000 us except at M3). */
static inline uint32_t n48_ks_wait_run(uint32_t on, uint32_t have_rec, const n48_ks_wait_io *io,
                                       uint32_t *polls_out, uint64_t *waited_us)
{
    return n48_ks_wait_run_b(on, have_rec, io, N48_KS_WAIT_BOUND_US, N48_KS_WAIT_MAX_POLLS, polls_out, waited_us);
}

static inline const char *n48_ks_wait_name(uint32_t o)
{
    static const char *const n[N48_KSW_OUTCOMES] = { "off", "no-record", "not-held", "RELEASED", "TIMEOUT" };
    return o < N48_KSW_OUTCOMES ? n[o] : "?";
}

/* The per-wait line (one per wait that actually polled: RELEASED or TIMEOUT). args: outcome name, marker at the first
 * look (1 = held), polls, waited us, bound us, poll cap, create #, gate seq. */
#define N48_KS_WAIT_LINE_FMT \
    "keystone: WAIT (switch 64) - the withdrawal marker of create #%u was HELD at the keystone (token gate seq %u); " \
    "%s after %u poll(s) / %llu us (bound %u us, %u polls). Every check below runs UNCHANGED on values read now; a " \
    "marker still held is VERDICT 15."
/* The bare `gfxneuter 64` report line. args: state word, change word, asked, no-record, not-held, released, timeout,
 * bound us, poll cap, longest us, last outcome name, last us, last polls, members retired at a keystone withdrawal. */
#define N48_KS_WAIT_REPORT_FMT \
    "kswait64: switch 64 is %s%s. Waits: asked %llu, no-record %llu, not-held %llu, released %llu, TIMED OUT " \
    "%llu (bound %u us/%u polls); longest %llu us; last %s, %llu us/%u polls. Fill members retired at a keystone " \
    "withdrawal: %llu."

#endif
