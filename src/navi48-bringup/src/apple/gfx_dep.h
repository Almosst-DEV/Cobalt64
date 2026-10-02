// gfx_dep.h — X9: THE DEPENDENCY BETWEEN FRAMES, expressed as data (0.0.353). Pure C, host-tested by
// tests/gfx_dep_test.cpp; the kext compiles the SAME header.
//
// THE HAZARD, IN ONE SENTENCE. The ladder in gfx_xlat_verdict.h is handed ONE frame and judges it alone. If we neuter a frame
// that renders into a surface and then COMMIT a later frame that SAMPLES that surface, the later frame reads bytes nobody
// wrote — Apple's stale contents or unwritten memory — and draws a silently wrong picture. No counter moves. That is the worst
// failure class this project has, and it is not reachable by adding a condition to n48_xv_decide, because that function cannot
// see the other frames.
//
// ---------------------------------------------------------------------------------------------------------------------
// WHAT IS **NOT** BUILT HERE, AND WHY. READ THIS BEFORE EXTENDING IT.
// ---------------------------------------------------------------------------------------------------------------------
// The obvious X9 is "refuse a consumer whose producer was refused": a set of refused surfaces, and a per-frame check of the
// surfaces that frame samples. THE SET SIDE IS SOUND. THE CHECK SIDE IS NOT, AND CANNOT BE MADE SO FROM WHAT THIS CODEBASE
// KNOWS. Both halves of the reason are worth writing down, because both are the difference between a guard and a decoration:
//
//   1. WE CANNOT ENUMERATE WHAT A FRAME READS. The route exists and it is a SAMPLER, not a decision procedure. The capture
//      path walks one level below each user-data pointer (AppleHardwareHook.cpp, the N48_GCAP_USER2 regions) and the offline
//      intersection reads those blobs in 8-dword strides as image descriptors (tools/m4-xlat/capintersect.py, `srd_bases`).
//      Every level of it is "this looks like a VA" — n48_gcap_is_va tests alignment and a plausible high byte, nothing more —
//      it stops at ONE level of indirection, it is capped (16 children per pointer, 64 dwords per table, 64 scan items per
//      frame in the decide path), and it is blind to a descriptor the shader builds for itself. It was enough to prove a
//      POSITIVE: found every colour target it looked for inside the descriptor tables, and sharpened that to
//      6-10 of 46 targets demonstrably sampled after a bind. It is NOT enough to prove a NEGATIVE, and "this frame samples
//      nothing we refused" IS a negative. An incomplete read-set fails OPEN in exactly the hazard's direction: the one
//      descriptor we did not see is the one that reads the surface we emptied.
//      Making it complete means knowing Apple's Metal descriptor ABI and following descriptor dataflow through the shader's
//      SGPRs — a compiler-grade analysis. That is a concept this codebase does not have, and this header does not pretend to.
//
//   2. WE CANNOT ENUMERATE WHAT A REFUSED FRAME WOULD HAVE WRITTEN, EITHER. The decide path reads CB_COLORn_BASE and nothing
//      else (N48_GCAP_CB). Depth is scanned but not consumed here; storage images, DMA_DATA destinations and anything a
//      compute dispatch writes are not read at all. So even the producer side of a VA-keyed set is a subset of the truth.
//
// WHAT SURVIVES BOTH, AND IS THEREFORE WHAT THIS HEADER GATES ON: **a frame may be committed only when NOTHING HAS BEEN LEFT
// UNWRITTEN.** That needs no read-set and no write-set — only the count of submissions that did not execute, which the kext
// has always had (`gGs.neuteredSubs` at the source hook, `gGn.ibs` at the ring). [0.0.358: THE NEXT SENTENCE WAS FALSE IN
// BOTH HALVES - , notes/M4-X9-COMPLETE.md, and the v2 block at the end of this header is the correction. Kept so
// the record shows what the 0.0.353-0.0.357 check believed.] "It is sound because staleness cannot exist without a dropped
// write, and every dropped write is counted." It is honest about its own cost: it says plainly that COMMIT
// may not be armed while the ladder is refusing frames that draw, and it opens by itself as the refusals go away. Every
// imprecision in it — an alias, a surface rewritten later by someone else, a refused frame that drew nothing — pushes towards
// REFUSING MORE, never towards committing. That is the only direction that is safe to be wrong in.
//
// THE WITNESS TABLE BELOW IS AN INSTRUMENT AND IS **NOT** AN INPUT TO THE CHECK. It records which surfaces a refused frame
// named, keyed by PHYSICAL page where that resolved — the identity that survives two processes sharing one IOSurface under
// different VAs, which is the reason a VA-keyed set could not be scoped per process. It exists so that a later, reviewed
// narrowing of `target-in-vram` ( says that narrowing is a separate decision, and it is) can be MEASURED rather than
// argued. Nothing in n48_dep_check reads it except its overflow, and that only to refuse.
//
// FAIL-CLOSED BY CONSTRUCTION, the same property gfx_commit.h has: `sampled` must be POSITIVELY set by the caller from the
// live counters. A zero-initialised n48_dep_world is a world nobody looked at, and it REFUSES — it does not read as a clean
// world with every counter at zero. That inversion is the first planted defect tests/gfx_dep_test.cpp checks.
#ifndef N48_GFX_DEP_H
#define N48_GFX_DEP_H

#include <stdint.h>
/* 0.0.395 (notes/design/R5-REDESIGN.md v2): the R5′ predicate decodes a held-back frame's colour targets with the capture
 * scanner (fixed walk, under its own flag) and its hazard set is keyed by resolved physical page. Both are pure headers. */
#include "gfx_capture_scan.h"
#include "gfx_hazard.h"

/* 0.0.358: the observers v2 requires, each set by n48_dep_fill from LIVE install state and from nothing else. */
#define N48_DEP_OBS_SRC      0x01u  /* the source hook is installed on the render drain's GFX channel */
#define N48_DEP_OBS_RING     0x02u  /* the ring walk holds Apple's GFX writeTail */
#define N48_DEP_OBS_SDMA     0x04u  /* the SDMA drain is live */
#define N48_DEP_OBS_STALL    0x08u  /* the CP stall detector is armed and read the CP's pointers in this fill */
#define N48_DEP_OBS_EARLY    0x10u  /* the GFX ring held no INDIRECT_BUFFER of any VMID when the ring walk armed */
/* 0.0.367 — the two observers the safety review requires before N1 may ship with the descriptor port. */
#define N48_DEP_OBS_QUEUE    0x20u  /* C13: the TTL's engine-queue slot is on Apple's path and its type tally is intact */
#define N48_DEP_OBS_FAULT    0x40u  /* C16: THIS gather read GCVM_L2_PROTECTION_FAULT_STATUS_LO32 */
#define N48_DEP_OBS_REQUIRED 0x7Fu

/* Refusal reasons, in the order the check tests them. 0 is the only value that lets a commit proceed. */
enum {
    N48_DEP_OK = 0,
    N48_DEP_NOT_SAMPLED,     /* the world record was never filled — a zero world is NOT a clean world */
    N48_DEP_SOURCE_NEUTER,   /* a submission was NOPed at commitIndirectCommandBuffer: its writes did not happen */
    N48_DEP_RING_NEUTER,     /* an INDIRECT_BUFFER packet was NOPed in the ring: same, one layer down */
    N48_DEP_NEUTER_OTHER,    /* a frame was dropped by a path that is counted but not attributed (walk stopped, raced) */
    N48_DEP_TARGET_UNKNOWN,  /* a refused frame named a colour target whose page did not resolve: we cannot even name it */
    N48_DEP_WITNESS_OVER,    /* the witness table overflowed: the record of what is stale is INCOMPLETE */
    /* 0.0.358 — X9 v2, the complete count. APPENDED, never inserted: the indices are printed. */
    N48_DEP_NOT_OBSERVED,      /* an observer the count relies on is not live: a class nobody watches REFUSES */
    N48_DEP_NONMONOTONE,       /* a counter went DOWN between two fills: a reset lost what it had counted */
    N48_DEP_UNACCOUNTED,       /* an accounting identity did not add up: some outcome has no bucket */
    N48_DEP_GFX_ESCAPED,       /* GFX work reached the CP untranslated while the neuter was armed */
    N48_DEP_GFX_NATIVE,        /* GFX work ran as Apple wrote it because nothing was armed */
    N48_DEP_SDMA_UNTRANSLATED, /* an SDMA IB the drain did not translate, or translated and could not verify */
    N48_DEP_SDMA_WAIT_REMOVED, /* a TLB-invalidate acknowledgement wait was neutered instead of kept */
    N48_DEP_RING_RESET,        /* a ring's write pointer went backwards */
    N48_DEP_ENGINE_STALL,      /* the CP sat short of its write pointer past the threshold, or could not be read */
    /* 0.0.367 — C13 and C16. APPENDED, never inserted: the indices are printed. */
    N48_DEP_COMPUTE_QUEUE,     /* an engine queue, or a queue operation, that no observer of ours watches */
    N48_DEP_VM_FAULT,          /* the GPU's VM fault status is latched: a faulted write is a dropped write */
    /* 0.0.390 ( part 5 (iv)) — THE PER-CONSUMER POSITIVE PROVENANCE RULE'S OWN TWO. APPENDED, never inserted:
     * the indices are printed. They are reachable ONLY when the world carries `consumer_enumerated` 1, which the fill sets
     * only when `accel gfxneuter 28 | 1 << 8` is ON - so on a default build these two can never be returned. */
    N48_DEP_NEUTER_WRITESET,   /* R5: a neutered frame since the arm has no KNOWN write-set (or the ring lost one) */
    N48_DEP_CONSUMER_UNPROVEN, /* R1-R4: an input THIS consumer reads that no rule could prove */
    /* 0.0.395 (notes/design/R5-REDESIGN.md v2 "R5′"): HOLD-BACK R5′'s BLIND BUCKET. A frame noted since the arm that is
     * neither POSITIVELY `bounded` (outside the consumer's address space) nor POSITIVELY `readable` (every IB walked to
     * its declared length, targets and memory destinations recorded without truncation and resolved, and a DISPATCH
     * bounded by its compute V#) is BLIND, and blind > 0 REFUSES. APPENDED, never inserted: indices are printed. It is
     * reachable ONLY on the path that carries the new switch (30), so a 30-OFF world can never return it. */
    N48_DEP_NEUTER_UNREADABLE, /* R5′: a held-back frame whose DESTINATION SET could not be READ */
    N48_DEP_REASONS
};

static inline const char *n48_dep_reason_name(uint32_t r)
{
    static const char *const n[N48_DEP_REASONS] = {
        "clean", "not-sampled", "source-neuter", "ring-neuter", "neuter-other", "target-unknown", "witness-overflow",
        "not-observed", "counter-went-down", "unaccounted", "gfx-escaped", "gfx-native", "sdma-untranslated",
        "sdma-wait-removed", "ring-reset", "engine-stall", "compute-queue", "vm-fault",
        "neuter-unknown-writeset", "consumer-inputs-unproven", "neuter-unreadable-writeset" };
    return r < N48_DEP_REASONS ? n[r] : "?";
}

/* Everything that can have left a surface holding bytes we did not write, as the kext read it from its own live counters at
 * the moment the frame was judged. Zero-initialised on every frame; `sampled` must be set to 1 or nothing below is believed. */
typedef struct {
    uint32_t sampled;          /* 1 when the caller filled this record from the live counters — REQUIRED */
    uint64_t source_neuters;   /* gGs.neuteredSubs: submissions NOPed at the source hook */
    uint64_t ring_neuters;     /* gGn.ibs: INDIRECT_BUFFER packets NOPed in the ring */
    uint64_t neuter_other;     /* gGn.walkStopFrames + gGn.raced: frames the ring neuter could not account for */
    uint64_t targets_unknown;  /* refused frames whose colour target did not resolve through the submitter's page table */
    uint64_t witness_over;     /* distinct stale surfaces the witness table could not store */
    /* 0.0.358 — v2. Appended. Filled ONLY by n48_dep_fill below; each refuses at a count of ONE. */
    uint32_t observers;          /* N48_DEP_OBS_* bits set from LIVE install state; must equal N48_DEP_OBS_REQUIRED */
    uint64_t nonmonotone;        /* fills that saw some counter DECREASE since the previous fill (latched, per boot) */
    uint64_t unaccounted;        /* N48_DEP_ID_* bits: identities that did not add up in this fill */
    uint64_t gfx_escaped;        /* GFX work that reached the CP untranslated while armed (C4, C5, unwalked ring work) */
    uint64_t gfx_native;         /* GFX work that ran as Apple wrote it because nothing was armed (C6, C7) */
    uint64_t sdma_untranslated;  /* SDMA IBs not translated or not verified (C9) */
    uint64_t sdma_waits_removed; /* TLB-invalidate ACK polls neutered rather than kept (C10) */
    uint64_t ring_resets;        /* GFX or SDMA ring write pointer went backwards (C11) */
    uint64_t engine_stalls;      /* CP parked short of WPTR past the threshold, or its pointers unreadable (C12's onset) */
    /* 0.0.367 — C13 and C16. Appended; each refuses at a count of ONE. */
    uint64_t compute_queues;     /* engine queues / queue operations outside the four types our observers cover (C13) */
    uint64_t vm_faults;          /* gathers that read a LATCHED GPU VM fault (C16) */
    /* 0.0.372 — X9-F. An INSTRUMENT, never an input to any rung: how much of `source_neuters` this fill
     * SUBTRACTED because gfx_forgive.h's rule granted it. 0 on every fill of every default build, because the switch it
     * needs is OFF by default and the fill subtracts nothing without a POSITIVE grant (see the fill). */
    uint64_t forgiven;
    /* 0.0.390 — PER-CONSUMER POSITIVE PROVENANCE. `consumer_enumerated` is 1 ONLY when the switch is on
     * AND this consumer's inputs were POSITIVELY enumerated by the rule (gfx_dep.h's n48_cp_eval, from the translator's own
     * input list). While it is 0 - the zero-initialised value, and every fill of every default build - the check runs the
     * `source_neuters` rung exactly as 0.0.389 ran it. While it is 1 that ONE rung is replaced by these two, and no other
     * rung moves: the observers, monotonicity, identity, ring-neuter, neuter-other, target-unknown, witness-overflow,
     * escaped, native, SDMA, ring-reset, stall, queue and vm_fault rungs are all still asked, in the same order.
     * `stale_overwrites` is an INSTRUMENT and gates nothing (: a later neutered writer of an R1 surface is tolerated
     * and counted - the bytes are ours, the loss is window content). */
    uint32_t consumer_enumerated;
    uint64_t neuter_unknown_writeset;   /* R5's U: neutered frames since the arm whose write-set is not known */
    uint64_t consumer_inputs_unproven;  /* R1-R4: inputs of THIS consumer no rule could prove */
    uint64_t stale_overwrites;          /* INSTRUMENT: R1 surfaces a later neutered frame overwrote. NEVER a rung */
    /* 0.0.395 (notes/design/R5-REDESIGN.md v2 "Switch and identity"): WHICH R5 ANSWER THE CHECK READS. `r5_mode` 1 means
     * the new switch (30) is ON and the R5′ rule's BLIND bucket, carried in `r5_blind`, is the answer; 0 means the
     * 0.0.394 rule, carried in `neuter_unknown_writeset`, is. Only the new switch moves this, and while it is 0 both new
     * fields are 0 and n48_dep_check below is byte for byte 0.0.394's. */
    uint32_t r5_mode;
    uint64_t r5_blind;                  /* R5′'s BLIND count; refuses under N48_DEP_NEUTER_UNREADABLE */
    /* build 0.0.523 (notes/design/RING-NEUTER-FORGIVE.md rev 2 item 3, switch 77) — AN INSTRUMENT, never an input to
     * any rung: how much of `ring_neuters` this fill SUBTRACTED because gfx_rnforgive.h's drain granted it. 0 on every fill
     * while 77 is OFF (the default), because the fill subtracts nothing without a positive, fitting, clause-0 grant. */
    uint64_t rn_forgiven;
    /* build 0.0.541 item 6 — THE WITNESS-OVERFLOW RELAXATION. `wo_mode` is the switch's mode as the
     * fill copied it (N48_DEP_WO_OFF 0 - the zero-initialised value and every fill while 99 is OFF - SHADOW, or ON), and
     * `cp_rows_free` is 1 ONLY when THIS frame's consumer was judged by an evaluator that never reads the witness ROWS
     * (n48_cp_eval_hz, n48_cp_eval_fill_hz, n48_cp_d4_judge -> n48_cp_eval_hz_d4: each only null-checks `wt`); the VA-keyed
     * n48_cp_eval (30 OFF) reads them through n48_cp_in_witness and leaves it 0. Only ON with n48_dep_wo_relaxable() true skips
     * the `witness_over` rung, and only that rung; OFF and SHADOW ask it exactly as 0.0.540 did. */
    uint32_t wo_mode;
    uint32_t cp_rows_free;
} n48_dep_world;

/* build 0.0.541 item 6 (switch 99): the world's `wo_mode` values (the switch's M 1 / 3; M 2 OFF is stored as 0). */
enum { N48_DEP_WO_OFF = 0u, N48_DEP_WO_ON = 1u, N48_DEP_WO_SHADOW = 3u };
/* May the witness-overflow rung be skipped for this world? ALL of: the consumer rule answered for THIS frame (28 ON and a
 * positive enumeration), R5′ is the rule in force (30 ON: r5_mode 1, so R3 asks the physical hazard FILTER, which cannot
 * overflow - n48_hz_add sets the filter bits before its row-capacity check - and errs toward refusing), the evaluator that
 * judged THIS frame read no witness row (cp_rows_free), and R5′'s BLIND bucket is 0 (a frame whose targets were truncated
 * past N48_CP_TGT_MAX or did not resolve is BLIND and refuses first, at N48_DEP_NEUTER_UNREADABLE). */
static inline uint32_t n48_dep_wo_relaxable(const n48_dep_world *w)
{
    return (w && w->consumer_enumerated == 1u && w->r5_mode == 1u && w->cp_rows_free == 1u && w->r5_blind == 0u) ? 1u : 0u;
}

/* Returns N48_DEP_OK only when the world was sampled AND every count of a dropped write is zero; otherwise the FIRST reason,
 * with *detail carrying the count that names it.
 * build 0.0.541 item 6: the body is n48_dep_check_m; `relax` 1 skips the witness-overflow rung (only that one, only in its
 * place: after every rung above it, R5′'s blind bucket included) when n48_dep_wo_relaxable(w). n48_dep_check asks it with
 * relax = (wo_mode == ON): OFF and SHADOW are 0.0.540's rule; the instruments ask both answers through n48_dep_check_m. */
static inline uint32_t n48_dep_check_m(const n48_dep_world *w, uint64_t *detail, uint32_t relax)
{
    if (detail) *detail = 0u;
    if (!w) return N48_DEP_NOT_SAMPLED;
    if (w->sampled != 1u) { if (detail) *detail = w->sampled; return N48_DEP_NOT_SAMPLED; }
    /* 0.0.358 — v2, in the order notes/M4-X9-COMPLETE.md fixes: whether the count can be believed at all first
     * (observers, monotonicity, identities), then the counts. A world filled by v1 code (observers 0) refuses here. */
    if (w->observers != N48_DEP_OBS_REQUIRED) {
        if (detail) *detail = (uint64_t)(N48_DEP_OBS_REQUIRED & ~w->observers) | ((uint64_t)w->observers << 32);
        return N48_DEP_NOT_OBSERVED;
    }
    if (w->nonmonotone) { if (detail) *detail = w->nonmonotone; return N48_DEP_NONMONOTONE; }
    if (w->unaccounted) { if (detail) *detail = w->unaccounted; return N48_DEP_UNACCOUNTED; }
    /* 0.0.390 — THE ONE RUNG THE RULE REPLACES, AND IT REPLACES NOTHING ELSE. measured this rung
     * refusing 237/249/250 of 256 judged frames in arm13/14/15 at `source-neuter`, from a boot-global count with one `++`
     * and no other writer. When the consumer's own inputs have been enumerated, the question "did ANY submission anywhere
     * drop a write" is replaced by the two the enumeration can answer: is every neutered frame since the arm's write-set
     * KNOWN, and is every input THIS consumer reads PROVEN. Both must be zero, and each is fail-closed at a count of one.
     * `consumer_enumerated` 0 (the default build, every fill) falls through to 0.0.389's rung, byte for byte. */
    if (w->consumer_enumerated == 1u) {
        /* 0.0.395: THE ONE PLACE THE NEW SWITCH MOVES THE DECISION. `r5_mode` is set ONLY by the new switch; while it
         * is 0 this block is 0.0.394's, line for line, and `r5_blind` is 0. While it is 1 the new rule's BLIND bucket
         * answers FIRST and under its own APPENDED reason; the R5 count in `neuter_unknown_writeset` is then an
         * instrument and does not gate. `consumer_inputs_unproven` (R1-R4) follows in both modes, unchanged. */
        if (w->r5_mode == 1u) {
            if (w->r5_blind) { if (detail) *detail = w->r5_blind; return N48_DEP_NEUTER_UNREADABLE; }
        } else if (w->neuter_unknown_writeset) {
            if (detail) *detail = w->neuter_unknown_writeset; return N48_DEP_NEUTER_WRITESET;
        }
        if (w->consumer_inputs_unproven) { if (detail) *detail = w->consumer_inputs_unproven; return N48_DEP_CONSUMER_UNPROVEN; }
    } else if (w->source_neuters) { if (detail) *detail = w->source_neuters; return N48_DEP_SOURCE_NEUTER; }
    if (w->ring_neuters) { if (detail) *detail = w->ring_neuters; return N48_DEP_RING_NEUTER; }
    if (w->neuter_other) { if (detail) *detail = w->neuter_other; return N48_DEP_NEUTER_OTHER; }
    if (w->targets_unknown) { if (detail) *detail = w->targets_unknown; return N48_DEP_TARGET_UNKNOWN; }
    if (w->witness_over && !(relax && n48_dep_wo_relaxable(w))) { if (detail) *detail = w->witness_over; return N48_DEP_WITNESS_OVER; }
    if (w->gfx_escaped) { if (detail) *detail = w->gfx_escaped; return N48_DEP_GFX_ESCAPED; }
    if (w->gfx_native) { if (detail) *detail = w->gfx_native; return N48_DEP_GFX_NATIVE; }
    if (w->sdma_untranslated) { if (detail) *detail = w->sdma_untranslated; return N48_DEP_SDMA_UNTRANSLATED; }
    if (w->sdma_waits_removed) { if (detail) *detail = w->sdma_waits_removed; return N48_DEP_SDMA_WAIT_REMOVED; }
    if (w->ring_resets) { if (detail) *detail = w->ring_resets; return N48_DEP_RING_RESET; }
    if (w->engine_stalls) { if (detail) *detail = w->engine_stalls; return N48_DEP_ENGINE_STALL; }
    if (w->compute_queues) { if (detail) *detail = w->compute_queues; return N48_DEP_COMPUTE_QUEUE; }
    if (w->vm_faults) { if (detail) *detail = w->vm_faults; return N48_DEP_VM_FAULT; }
    return N48_DEP_OK;
}
static inline uint32_t n48_dep_check(const n48_dep_world *w, uint64_t *detail)
{
    return n48_dep_check_m(w, detail, (w && w->wo_mode == N48_DEP_WO_ON) ? 1u : 0u);
}

/* 0.0.367 — N1'S ONE CONDITION. `gXdTvramDrop` (the rung that refuses a colour target in VRAM) is a RUNTIME
 * SWITCH, default OFF, and the verb may only turn it ON when the world it was handed is one X9 would believe: sampled, and
 * EVERY observer live - including C13 and C16. Nothing else may decide it, and it is pure so the defect "the switch enabled
 * without the observers" is a host-tested mutant rather than a code review. It does NOT ask whether the counts are clean:
 * a dirty count already refuses at n48_dep_check, and refusing the switch on a count would make the switch un-settable
 * before a boot has finished arming. What it refuses is a world nobody was watching. */
static inline uint32_t n48_dep_may_drop_tvram(const n48_dep_world *w)
{
    if (!w || w->sampled != 1u) return 0u;
    return w->observers == N48_DEP_OBS_REQUIRED ? 1u : 0u;
}

/* The one value gfx_commit.h's `dep_ok` may take. Nothing else in the kext may compute it. */
static inline uint32_t n48_dep_ok(const n48_dep_world *w)
{
    uint64_t d = 0u;
    return n48_dep_check(w, &d) == N48_DEP_OK ? 1u : 0u;
}

/* ---- THE WITNESS: which surfaces a refused frame left unwritten. An instrument, never an input (see the header comment). ----
 *
 * Keyed by PHYSICAL page when the target resolved, by VA when it did not. Physical is the right identity: two processes can
 * hold one IOSurface at two different VAs, so a VA-keyed set cannot be scoped per process without becoming unsound, and a
 * page-keyed one does not need to be. A target that did NOT resolve is still recorded — with `resolved` 0 — and is ALSO
 * counted in the world's `targets_unknown`, because a stale surface we cannot name is worse than one we can, not better.
 * This is the deliberate inverse of n48_xv_frame.target_vram's fail-open `if (ok && !isSys)`. */
#define N48_DEP_ROWS 16u
/* 0.0.390 ( part 5 (iii)) — THE STORAGE, WHICH IS NOT THE CAP. arm13's 16-row witness OVERFLOWED 99 TIMES and
 * arm18's 28, and `witness_over` is a REFUSING rung one below the one the rule replaces - so a widened witness is not an
 * instrument nicety, it is the third rung did not list. The rows here are storage; the CAP IN FORCE is `wt->rows`, and
 * 0 - the zero-initialised value, and the value on every witness of every default build - means N48_DEP_ROWS, i.e. 16,
 * 0.0.389's behaviour row for row and overflow for overflow. Only a witness whose `rows` the caller POSITIVELY set (which
 * the kext does only under `accel gfxneuter 28 | 1 << 8`, on a SECOND, ARM-SCOPED witness instance, never on the lifetime
 * one) sees more. */
#define N48_DEP_ROWS_MAX 64u

typedef struct {
    uint64_t va;          /* the colour target's VA in the submitter's address space (the first one seen for this key) */
    uint64_t page;        /* its physical page, 0 when the walk failed */
    uint64_t hits;        /* refused frames that named this surface */
    uint32_t resolved;    /* 1 when `page` came from a successful walk */
    uint32_t is_sys;      /* 1 when that page is host memory rather than device VRAM */
    uint32_t verdict;     /* the n48_xv_decide reason that refused the FIRST frame to name it */
    int32_t  pid;         /* the submitter of that first frame */
    uint64_t ctx;         /* 0.0.390: the submitting context of that first frame; 0 = the caller supplied none */
} n48_dep_row;

typedef struct {
    n48_dep_row row[N48_DEP_ROWS_MAX];
    uint32_t used;
    uint64_t over;        /* distinct surfaces that did not fit — feeds the world's witness_over, never silent (rule 72) */
    uint64_t notes;       /* calls */
    uint64_t dups;        /* calls that matched an existing row */
    uint64_t unresolved;  /* calls whose page walk failed — feeds the world's targets_unknown */
    /* 0.0.390: the cap in force (0 = N48_DEP_ROWS) and the ARM this witness's rows were recorded under.
     * `arm_seq` 0 is "not scoped" and nothing is ever re-based; n48_dep_arm_scope is the only writer of either. */
    uint32_t rows;
    uint32_t arm_seq;
    uint64_t rebases;     /* times the scope moved and the rows were dropped — an instrument, printed, never a rung */
} n48_dep_witness;

/* The cap in force. 0 is 0.0.389's 16, and a value past the storage is clamped rather than trusted. */
static inline uint32_t n48_dep_rows_cap(const n48_dep_witness *wt)
{
    if (!wt || wt->rows == 0u) return N48_DEP_ROWS;
    return wt->rows > N48_DEP_ROWS_MAX ? N48_DEP_ROWS_MAX : wt->rows;
}

/* 0.0.390 ( part 5 (iii)) — ARM SCOPING. What is stale is what was dropped SINCE THE ARM; a surface a frame
 * refused before this arm has been rewritten by Apple's own compositor many times over. When `arm_seq` moves, the rows and
 * the THREE COUNTS THE WORLD READS FROM THIS WITNESS (over, unresolved, and the caller's truncation count, which it keeps)
 * are re-based. THAT IS ONLY SOUND ON A SEPARATE INSTANCE: the kext keeps the lifetime witness untouched and feeds a second,
 * arm-scoped one beside it, so nothing this function does can erase a count the lifetime instrument already published.
 * `seq` 0 is not a scope and does nothing. Returns 1 when it re-based. */
static inline uint32_t n48_dep_arm_scope(n48_dep_witness *wt, uint32_t seq)
{
    if (!wt || seq == 0u || wt->arm_seq == seq) return 0u;
    wt->arm_seq = seq;
    wt->used = 0u; wt->over = 0u; wt->unresolved = 0u; wt->dups = 0u; wt->notes = 0u;
    for (uint32_t i = 0; i < N48_DEP_ROWS_MAX; i++) {
        n48_dep_row *r = &wt->row[i];
        r->va = 0ull; r->page = 0ull; r->hits = 0ull; r->resolved = 0u; r->is_sys = 0u; r->verdict = 0u; r->pid = 0;
        r->ctx = 0ull;
    }
    wt->rebases++;
    return 1u;
}

/* 0.0.390: the same note with the submitting context recorded on the row. Matching is UNCHANGED - physical page when the
 * walk resolved, VA when it did not - because a context-sensitive key would change which rows 0.0.389 recorded. `ctx` is
 * carried so a reader can say WHOSE surface a row is, and so R3 can be read beside it; it is not part of the key. */
static inline void n48_dep_note_ctx(n48_dep_witness *wt, uint64_t ctx, uint64_t va, uint64_t page, uint32_t resolved,
                                    uint32_t is_sys, uint32_t verdict, int32_t pid)
{
    if (!wt) return;
    const uint32_t cap = n48_dep_rows_cap(wt);
    wt->notes++;
    if (!resolved) wt->unresolved++;
    for (uint32_t i = 0; i < wt->used && i < cap; i++) {
        const int same = resolved ? (wt->row[i].resolved && wt->row[i].page == page)
                                  : (!wt->row[i].resolved && wt->row[i].va == va);
        if (same) { wt->row[i].hits++; wt->dups++; return; }
    }
    if (wt->used >= cap) { wt->over++; return; }
    n48_dep_row *r = &wt->row[wt->used++];
    r->va = va; r->page = page; r->hits = 1u;
    r->resolved = resolved ? 1u : 0u; r->is_sys = is_sys ? 1u : 0u;
    r->verdict = verdict; r->pid = pid; r->ctx = ctx;
}

static inline void n48_dep_note(n48_dep_witness *wt, uint64_t va, uint64_t page, uint32_t resolved, uint32_t is_sys,
                                uint32_t verdict, int32_t pid)
{
    n48_dep_note_ctx(wt, 0ull, va, page, resolved, is_sys, verdict, pid);
}

/* =====================================================================================================================
 * 0.0.390 — PER-CONSUMER POSITIVE PROVENANCE, R1-R5. Pure; host-tested by tests/gfx_dep_test.cpp.
 * =====================================================================================================================
 * WHAT THIS IS FOR. The rung above ("nothing has been left unwritten anywhere") is a BOOT-GLOBAL count, and measured
 * it refusing 237/249/250 of 256 judged frames in three consecutive runs. It is not a bad rule; it is a rule about the
 * world when the hazard is about ONE CONSUMER'S INPUTS. enumerated arm13's plane frame from its own capture: the
 * consumer carries ZERO `WAIT_REG_MEM`, `WRITE_DATA`, `COPY_DATA`, live `RELEASE_MEM` and `DISPATCH`, and its only inputs
 * are two textures and CPU-written heaps. For that shape every hazard class is empty or visual-only, and the rule below
 * asks the five questions that make that a CHECK rather than an observation.
 *
 * WHAT IT IS **NOT**, AND THE FOUR DEAD DESIGNS IT AVOIDS:
 *   - it is NOT a global discharge, so X9-C's F1 circularity ("a neutered frame named the page after a committed one wrote
 *     it") cannot arise: the question is asked of the INTERSECTION with THIS consumer's reads;
 *   - a DISPATCH in a neutered frame is UNKNOWN and REFUSES (X9-C's F3, compute drops forgiven, inverted);
 *   - the consumer's reads are ENUMERATED rather than assumed absent ("reads nothing", inverted);
 *   - there is no count and no owner, so the baseline's "blind to whose drops" and the narrowing's "the first drop is a
 *     draw" do not apply.
 *   NOT AVOIDED, WRITTEN DOWN: X9-C's F2 (the unknown set U) in general. It is only BOUNDED - by the ring below, which
 *   REFUSES when it overflows, and by R5, which refuses on any frame whose write-set is not known.
 *
 * FAIL-CLOSED BY CONSTRUCTION, three times over: `enumerated` must be POSITIVELY 1 (a zero consumer is not a consumer with
 * no inputs, it is a consumer nobody read); a ring that overflowed counts as unknown; and every "proven" flag must be set
 * by the caller from a POSITIVE answer, never inferred from the absence of a refusal. */

#define N48_CP_IN_MAX   4u    /* tiled/linear image inputs of one consumer (arm13's plane frame binds TWO) */
/* 0.0.392: 8 -> 16. The first-pass review of 0.0.391 fed arm13's REAL captured plane frame
 * (f15, and f14) through the real translator and built this list as the kext's three loops build it: the list is NINE
 * entries, not the eight the cap held - 3 fixed heap pages (the class-19 table, the image heap, the S#) plus GPUPass's
 * 3 declared fragment pointers (s0:s1, s10:s11, s12:s13) plus ViewportToNDC's 3 declared vertex pointers (slots 4/6/8).
 * The ninth set `over`, and `over` is the FIRST clause n48_cp_eval asks, so the real plane frame was refused at
 * `list-overflow` before R1-R4 could speak: the null run confirmed. 16 is headroom over the measured nine, and a
 * list that still does not fit stays UNKNOWN (`over`), exactly as before. RAISED, NOT DEDUPED: the table page is
 * genuinely named twice (once as the fixed table heap, once as GPUPass's own s0:s1), and removing the duplicate would
 * be a behavioural change to a list whose every entry the rule checks one by one. */
#define N48_CP_PTR_MAX  16u   /* raw-pointer pages the consumer's ABI declares, plus its table and S# heaps */
#define N48_CP_TGT_MAX  8u    /* colour targets one neutered frame can name (the kext's own kXdTgtHold is 8) */
/* 0.0.391: 4 -> 16. The adversarial review of 0.0.390 ran this scan over the REAL captured LUT
 * producers (src/xlat12/tests/fixture_wsgc1_headless.h, kWsgc1F5Setup / kWsgc1F10Setup -'s f4/f9/f13 shape, the
 * frames that sit between our fill and the plane frame) and found more destinations than the record could hold, so every
 * one of them was UNKNOWN and R5 refused before R1-R4 could speak: a CONFIRMED null run. Measured here, with the walk
 * fixed below: SIX each (three WRITE_DATA + three RELEASE_MEM), all of them the fence page 0x400001000. 16 is headroom
 * over the measured six; a frame that still does not fit is UNKNOWN, exactly as before. */
/* 0.0.403: 16 -> 64. The capture replay of arm23 found f77 - a real 22,336-dword, 4-IB, 3-DISPATCH
 * WindowServer submission - carrying 14 PM4 destinations plus 6 DISPATCH-V# destinations (the 2 per dispatch
 * added) = 20, so at 16 its destination record was TRUNCATED (`memw_ok 0`) and the frame was BLIND. then refused the
 * plane frames after it with `neuter-unreadable-writeset`, on a frame that names NONE of the consumer's inputs: a capacity
 * reading, not a hazard, and exactly what the R5′ memo forbade ("no bookkeeping capacity is ever a refusal cause"). 64 is
 * headroom over the measured 20. FAIL-CLOSED IS UNCHANGED: a frame that still does not fit keeps `memw_ok 0` and stays
 * BLIND, and `n48_cp_scan_frame` still returns 0 with `nmemw` past the cap. */
#define N48_CP_MEMW_MAX 64u   /* decoded memory-destination writes one neutered frame can name */
#define N48_CP_RING     32u   /* neutered frames since the arm the ring holds (: the window is ~13 small IBs) */

/* ONE NEUTERED FRAME, as the decide path saw it at the `action != TRANSLATE` site. */
typedef struct {
    uint64_t ctx;           /* the submitting context */
    uint32_t complete;      /* R5: 1 only when the write-set is KNOWN - got == len == walk, the scan stayed under its cap,
                             * every program was identified, and none of them stores. POSITIVELY set or the frame is U */
    uint32_t has_dispatch;  /* 1 when the frame carried a DISPATCH: always U, whatever `complete` says */
    /* 0.0.393: A POSITIVE READING THAT PUT THIS SUBMISSION OUTSIDE THE CONSUMER'S ADDRESS
     * SPACE. Set by the caller (the decide path) from a reading that positively places the frame elsewhere - a page
     * root or pid read and different from WindowServer's. A frame the kext never read (`shapeOk 0`, `f.nib 0`) is U
     * ONLY when this is 0; with it 1 the frame cannot have written into the consumer's address space, so its unknown
     * write-set is not U. 0 is "no such reading" and stays U (the fail-closed default, and the value every caller
     * that does not compute it passes). */
    uint32_t out_of_scope;
    uint32_t ntgt, nmemw;
    uint64_t tgt[N48_CP_TGT_MAX];    /* its colour targets */
    uint64_t memw[N48_CP_MEMW_MAX];  /* its decoded WRITE_DATA / RELEASE_MEM / DMA_DATA destinations */
    /* 0.0.397 (the reviewer's clause): 1 when the PM4 walk met a DRAW packet. A held-back frame that
     * DRAWS but records no colour target and no memory destination has an INHERITED destination (CB_COLORn_BASE was
     * written by an earlier frame), so its write-set is not known and R5′ must refuse it. APPENDED. */
    uint32_t has_draw;
} n48_cp_frame;

/* 0.0.393 — IS A JUDGED SUBMISSION POSITIVELY OUTSIDE THE CONSUMER'S ADDRESS SPACE? Pure, so the
 * decide path's one reading and the host test ask the SAME question. A page root read and different is a positive reading;
 * so is a pid read and different. Anything not POSITIVELY read as outside is in scope (0), which keeps the frame U - the
 * safe direction: a submission we cannot place elsewhere may have written where the consumer reads. */
static inline uint32_t n48_cp_scope_out(uint64_t frame_root, uint64_t consumer_root, int32_t frame_pid, int32_t consumer_pid)
{
    if (frame_root != 0ull && consumer_root != 0ull && frame_root != consumer_root) return 1u;
    if (frame_pid > 0 && consumer_pid > 0 && frame_pid != consumer_pid) return 1u;
    return 0u;
}

typedef struct {
    n48_cp_frame f[N48_CP_RING];
    uint32_t n;
    uint32_t arm_seq;       /* the arm these rows belong to; a move re-bases, exactly as the witness's scope does */
    uint64_t over;          /* frames that did not fit: the record is INCOMPLETE, so they count as UNKNOWN */
    uint64_t notes;         /* frames noted since the arm */
    uint64_t unknown;       /* ... of those, frames whose write-set is not known (R5's U) */
    uint64_t rebases;
} n48_cp_ring;

static inline uint32_t n48_cp_arm_scope(n48_cp_ring *r, uint32_t seq)
{
    if (!r || seq == 0u || r->arm_seq == seq) return 0u;
    r->arm_seq = seq; r->n = 0u; r->over = 0u; r->notes = 0u; r->unknown = 0u;
    r->rebases++;
    return 1u;
}

/* Record one neutered frame. A frame that does not fit is counted in `over`, which n48_cp_unknown folds into U.
 *
 * 0.0.393: A FRAME POSITIVELY READ AS OUTSIDE THE CONSUMER'S ADDRESS SPACE IS NOT U. ``
 * measured a fourth null-run cause on all three captures: a SecurityAgent submission the kext never read (`shapeOk 0`,
 * `f.nib 0`, verdict `shape`) reached this site, `complete` was 0, and it incremented `unknown` permanently - so
 * `n48_dep_check` answered `neuter-unknown-writeset` at the consumer in 3 of 3 runs before R1-R4 could speak. The frame
 * was in a DIFFERENT page root (arm13: SecurityAgent `0x3d6c15000` vs WindowServer `0x3d6c00000`), so it cannot have
 * written where the consumer reads. `out_of_scope` is that reading, and it is the caller's POSITIVE answer; a frame with
 * no such reading still counts, because "we did not look" is not "it is elsewhere". */
static inline void n48_cp_note(n48_cp_ring *r, const n48_cp_frame *f)
{
    if (!r || !f) return;
    r->notes++;
    const uint32_t unknown = (f->complete != 1u || f->has_dispatch || f->ntgt > N48_CP_TGT_MAX ||
                              f->nmemw > N48_CP_MEMW_MAX) ? 1u : 0u;
    if (unknown && f->out_of_scope != 1u) r->unknown++;
    if (r->n >= N48_CP_RING) { if (f->out_of_scope != 1u) r->over++; return; }
    r->f[r->n++] = *f;
}

/* R5's answer: frames since the arm whose write-set is not known, PLUS the ones the ring could not keep. */
static inline uint64_t n48_cp_unknown(const n48_cp_ring *r) { return r ? r->unknown + r->over : 1ull; }

/* 0.0.390 ( part 5 (iii)) — THE MEMORY-DESTINATION DECODE FOR ONE NEUTERED FRAME. Pure, READ-ONLY, one linear
 * pass over dwords the caller already read. It fills `memw[]` and `has_dispatch`; the caller fills `ctx`, `tgt[]` and
 * `complete`. Returns 1 only when the walk covered EXACTLY `n` dwords and met no nested IB - anything else is a frame
 * whose write-set is NOT known, and the caller must leave `complete` 0.
 *
 * THE LAYOUTS, each taken from code this project has already validated rather than from a manual:
 *   WRITE_DATA  0x37  body[0] DST_SEL [11:8]; NON-ZERO is a memory destination (xlat12_ib.c's own `operand_ok` says
 *                     exactly that), address = body[1] (dword-aligned) | body[2] << 32
 *   RELEASE_MEM 0x49  address = d[h+3] | d[h+4] << 32 — gfx_fence828.h's own `r->va` relative to its `rel_at`,
 *                     CONFIRMED on hardware by (the dword it rewrites at +5 is DATA_LO, one past the pair)
 *   DMA_DATA    0x50  body[0] DST_SEL [21:20]; 0 or 3 is memory, address = body[3] (& ~3) | body[4] << 32 —
 *                     f3_reader.h's own decode
 *   COPY_DATA   0x40  body[0] DST_SEL [11:8]; NON-ZERO is not a register (xlat12_ib.c's `operand_ok` reads exactly this
 *                     field and checks the register form only when it is 0), address = body[3] (& ~3) | body[4] << 32.
 *                     0.0.391 ( condition 3, item 8b): the translator's own R4 counts COPY_DATA as a
 *                     memory write and this scan had no branch for it, so a neutered frame that COPY_DATA-wrote a
 *                     consumer's heap page came out `complete` and tripped no R3 - a fail-open on the hazard R3 exists
 *                     for. A non-zero DST_SEL that is GDS or a perf counter is recorded as a destination too, which can
 *                     only make R3 refuse more often: the safe direction, and the one this rule is built in.
 * More destinations than the ring can hold sets `nmemw` past the cap, which n48_cp_note reads as UNKNOWN. */
static inline uint32_t n48_cp_scan_frame(const uint32_t *d, uint32_t n, n48_cp_frame *f)
{
    uint32_t i = 0u;
    if (!f) return 0u;
    f->nmemw = 0u; f->has_dispatch = 0u; f->has_draw = 0u;
    for (uint32_t k = 0; k < N48_CP_MEMW_MAX; k++) f->memw[k] = 0ull;
    if (!d || n == 0u) return 0u;
    while (i < n) {
        const uint32_t h = d[i], type = h >> 30;
        /* 0.0.391 ( condition 1, found by the host check that condition asks for): PACKET3(NOP, 0x3FFF) is the
         * ONE-DWORD filler this silicon uses (xlat12_ib.h XLAT12_IB_NOP 0xFFFF1000, `one dword, proven on this silicon`;
         * xlat12_ib.c's own plen_at answers 1 for it before it looks at the count field). Sized by the count field it
         * claims 16385 dwords, so through 0.0.390 this walk answered 0 on any frame carrying one - and BOTH captured LUT
         * producers end in exactly that dword. The cap raise alone would not have made either of them `complete`. */
        if (h == 0xFFFF1000u) { i++; continue; }
        if (type == 2u) { i++; continue; }                 /* the one-dword type-2 NOP */
        if (type != 3u) return 0u;                         /* not a packet we can size: the walk stops here */
        const uint32_t total = 2u + ((h >> 16) & 0x3FFFu), op = (h >> 8) & 0xFFu;
        if (total > n - i) return 0u;
        const uint32_t *b = &d[i + 1u];
        uint64_t dst = 0ull;
        uint32_t isMem = 0u;
        if (op == 0x15u || op == 0x16u) f->has_dispatch = 1u;                 /* DISPATCH_DIRECT / _INDIRECT */
        /* 0.0.397: A DRAW IS RECORDED, not decoded. The opcodes are exactly xlat12_ib.c's `is_draw`
         * (DRAW_INDEX_AUTO 0x2D, DRAW_INDEX_2 0x27, DRAW_INDEX_OFFSET_2 0x35, DRAW_INDIRECT 0x24,
         * DRAW_INDEX_INDIRECT 0x25). A draw writes the colour target bound by an EARLIER frame's CB_COLORn_BASE, which
         * this frame never rewrites, so a draw with no recorded target and no memory destination has an INHERITED
         * destination - see n48_r5_bucket. Sizing is unchanged: the branch only sets a flag. */
        else if (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) f->has_draw = 1u;
        else if (op == 0x3Fu || op == 0x33u) return 0u;                       /* a nested IB: its content is unseen */
        else if (op == 0x37u && total >= 5u) {                                /* WRITE_DATA */
            if (((b[0] >> 8) & 0xFu) != 0u) { isMem = 1u; dst = ((uint64_t)b[2] << 32) | (uint64_t)(b[1] & ~3u); }
        } else if (op == 0x49u && total >= 6u) {                              /* RELEASE_MEM */
            isMem = 1u; dst = ((uint64_t)d[i + 4u] << 32) | (uint64_t)d[i + 3u];
        } else if (op == 0x50u && total >= 6u) {                              /* DMA_DATA */
            const uint32_t ds = (b[0] >> 20) & 3u;
            if (ds == 0u || ds == 3u) { isMem = 1u; dst = ((uint64_t)b[4] << 32) | (uint64_t)(b[3] & ~3u); }
        } else if (op == 0x40u && total >= 6u) {                              /* COPY_DATA (0.0.391, condition 3) */
            if (((b[0] >> 8) & 0xFu) != 0u) { isMem = 1u; dst = ((uint64_t)b[4] << 32) | (uint64_t)(b[3] & ~3u); }
        }
        if (isMem) {
            /* D4-PRIME-FIXES.md item 9 (R5' BLIND latch),  — DEDUPE BY PAGE BEFORE THE 64 CAP. A
             * destination whose page this scan ALREADY named is not a different fact (the same direction
             * gfx_cp_build.h's n48_cp_merge_dedup already dedupes the frame-level union in) - only a genuinely NEW
             * page may push the count toward the cap, so N writes to FEWER than N48_CP_MEMW_MAX distinct pages never
             * truncate this list into BLIND, even when N itself is large. */
            uint32_t dup = 0u;
            for (uint32_t q = 0; q < f->nmemw && q < N48_CP_MEMW_MAX; q++)
                if ((f->memw[q] & ~(uint64_t)0xFFFull) == (dst & ~(uint64_t)0xFFFull)) { dup = 1u; break; }
            if (!dup) {
                if (f->nmemw < N48_CP_MEMW_MAX) f->memw[f->nmemw++] = dst;
                else { f->nmemw = N48_CP_MEMW_MAX + 1u; return 0u; }   /* a genuinely new page past the cap = not a known write-set */
            }
        }
        i += total;
    }
    return i == n ? 1u : 0u;
}

/* =====================================================================================================================
 * 0.0.395 (notes/design/R5-REDESIGN.md v2, "R5′", "Capacity — (a)", "DISPATCH — the claim v1 got wrong", and )
 * — THE THREE BUCKETS, THE HAZARD PAGE SET, AND THE COMPUTE CLAUSE. Pure; host-tested by tests/gfx_dep_test.cpp.
 * =====================================================================================================================
 * WHAT R5′ ASKS. Every frame noted at the `action != N48_SD_ACT_TRANSLATE` site since the arm goes into exactly one of
 * three buckets, each a scalar counter:
 *
 *   - BOUNDED  — `n48_cp_scope_out` answered 1: a POSITIVE reading (a page root or pid, read, and different) placing the
 *     submission outside the consumer's address space. It cannot have written where the consumer reads.
 *   - READABLE — every IB was read and walked to its declared length (`got == len == walk`), the PM4 scan covered exactly
 *     those dwords with no nested IB, targets and decoded memory destinations were recorded WITHOUT TRUNCATION and every
 *     destination page RESOLVED, and, under a DISPATCH, the compute clause below holds. Its destination PHYSICAL PAGES
 *     enter the hazard set.
 *   - BLIND    — anything else. `blind > 0` REFUSES, under the appended N48_DEP_NEUTER_UNREADABLE.
 *
 * THE MEMO'S FOUR DELIBERATE CHANGES FROM 0.0.394's R5, each a mutant below:
 *   1. `pgmOk` LEAVES THE COMPLETENESS TEST. Programs never named a render target, context registers did; measured
 *      563 `program-unknown` neutered frames and the memo retires the whole class. A frame whose programs are unknown is
 *      still READABLE if its IBs, targets and destinations are.
 *   2. TARGETS AND MEMORY DESTINATIONS ARE PHYSICALLY KEYED. A destination that does not resolve makes the frame BLIND
 *      rather than silently absent.
 *   3. THE WALK AND THE CAP ARE FIXED (gfx_capture_scan.h's N48_GCAP_F_FILLER, cap 512, and the memory-destination scan
 *      run PER IB rather than only under `oneIb`). These are what make `blind` rare rather than universal.
 *   4. A DISPATCH IS BOUNDED BY ITS V# (n48_r5_dispatch_vsharp): the compute user-data slots 0-3 hold a readable V# (a
 *      non-zero base and a non-zero extent), which is a POSITIVE reading of where the stores go. A dispatch whose user
 *      data is not a readable V# is BLIND.  disassembled the one compute program behind arm20's dispatches
 *      (a 9-dword buffer clear through its own V#) and PREDICTED the window reads blind 0 - SUSPECTED until an armed
 *      run; tests/gfx_dep_test.cpp's T5 confirms it offline on the real f21/f27/f33 bodies with resolution supplied.
 *
 * THE MEMO'S RESIDUE, WRITTEN DOWN RATHER THAN HIDDEN: a compute shader that SYNTHESISES an address from constants cannot
 * be excluded from the frame at DECIDE; the V# bound reads where the program was HANDED a destination, not everywhere it
 * could compute one. excludes it for THIS program's ISA. */

enum {
    N48_R5_BOUNDED = 0,
    N48_R5_READABLE = 1,
    N48_R5_BLIND = 2
};

typedef struct { uint64_t page; uint64_t va; } n48_r5_dst;

typedef struct {
    uint32_t out_of_scope;    /* n48_cp_scope_out answered 1: BOUNDED */
    /* 0.0.396 ( binding fix 1): THE SUBMISSION DECLARED MORE IBs THAN WERE READ. `f.nib` is clamped at
     * N48_XV_MAX_IBS; through 0.0.395 a frame with IBs 5+ was READABLE over the first four, i.e. its unread tail could
     * have written anything. Any declared count above what was read is a TRUNCATED record, so the frame is BLIND. */
    uint32_t ib_over;
    uint32_t ib_ok;           /* every IB was read and walked to its DECLARED length (got == len == walk) */
    uint32_t walk_ok;         /* every IB's PM4 scan covered exactly its dwords, with no nested IB */
    uint32_t targets_ok;      /* every colour target recorded; 0 when our own cap truncated the list */
    uint32_t memw_ok;         /* every memory destination recorded; 0 when our own cap truncated the list */
    uint32_t tgts_resolved;   /* every colour target's page resolved */
    uint32_t memw_resolved;   /* every memory destination's page resolved */
    uint32_t scan_over;       /* the gcap scan crossed its item cap: an unexamined item may be a target */
    uint32_t has_dispatch;    /* the frame carried DISPATCH_DIRECT / _INDIRECT */
    uint32_t dispatch_ok;     /* ... and the compute clause holds: user-data slots 0-3 hold a readable V# */
    /* 0.0.397 (the reviewer's clause): the frame carried a DRAW. A DRAW with no recorded colour target
     * (ntgt 0) and no decoded memory destination (nmemw 0) has an INHERITED destination and is BLIND. */
    uint32_t has_draw;
    uint32_t ntgt, nmemw;
    uint32_t n_unresolved;    /* destinations (targets + memory) whose page did not resolve - an instrument */
    n48_r5_dst tgt[N48_CP_TGT_MAX];
    n48_r5_dst memw[N48_CP_MEMW_MAX];
} n48_r5_frame;

/* THE BUCKET. Every clause is a POSITIVE reading; a zero-initialised frame (every flag 0) is BLIND, not readable.
 * 0.0.397 (the reviewer's clause): the INHERITED-DESTINATION clause. A frame that DRAWS but recorded
 * no colour target and no memory destination is drawing into whatever CB_COLORn_BASE an earlier frame bound - a page
 * this frame never names - so its write-set is not known and it is BLIND. named the path; zero of the 37
 * fixture frames take it, and the host suite plants a synthetic draw to prove the clause is live. */
static inline uint32_t n48_r5_bucket(const n48_r5_frame *x)
{
    if (!x) return N48_R5_BLIND;
    if (x->ib_over != 0u) return N48_R5_BLIND;       /* more IBs declared than read: the record is truncated */
    if (x->out_of_scope == 1u) return N48_R5_BOUNDED;
    if (x->ib_ok != 1u || x->walk_ok != 1u) return N48_R5_BLIND;
    if (x->targets_ok != 1u || x->memw_ok != 1u) return N48_R5_BLIND;
    if (x->tgts_resolved != 1u || x->memw_resolved != 1u) return N48_R5_BLIND;
    if (x->scan_over) return N48_R5_BLIND;
    if (x->has_dispatch && x->dispatch_ok != 1u) return N48_R5_BLIND;
    if (x->has_draw && x->ntgt == 0u && x->nmemw == 0u) return N48_R5_BLIND;   /* inherited destination */
    return N48_R5_READABLE;
}

/* build 0.0.547 item 2 ( open question: "which clause fired is NOT established") — WHICH CLAUSE MADE A FRAME BLIND.
 * An INSTRUMENT: it mirrors n48_r5_bucket clause for clause, in the same order, and answers the FIRST failing one; it decides
 * nothing (the bucket above is unchanged and is still the only thing n48_r5_note counts `blind` from). N48_R5_BC_NONE exactly when
 * the bucket is not BLIND (tests/gfx_wo99_test.cpp R2 checks the equivalence over random records). ib_ok and walk_ok, which the
 * bucket tests on one line, are split so a short or unread IB is told apart from a walk that stopped. */
enum {
    N48_R5_BC_NONE = 0,        /* BOUNDED or READABLE */
    N48_R5_BC_IB_OVER,         /* more IBs declared than read */
    N48_R5_BC_IB_UNREAD,       /* an IB not read to its declared length (short read, no reader, not the proven shape) */
    N48_R5_BC_WALK,            /* an IB's PM4 walk did not cover its dwords (nested IB, short packet, unsizable dword) */
    N48_R5_BC_TGT_TRUNC,       /* colour targets past N48_CP_TGT_MAX, or the item scan crossed its cap */
    N48_R5_BC_MEMW_TRUNC,      /* memory destinations past N48_CP_MEMW_MAX */
    N48_R5_BC_TGT_UNRESOLVED,  /* a colour target's page did not resolve */
    N48_R5_BC_MEMW_UNRESOLVED, /* a memory destination's page did not resolve */
    N48_R5_BC_SCAN_OVER,       /* the gcap scan crossed its item cap (with every list otherwise whole) */
    N48_R5_BC_DISPATCH,        /* a DISPATCH without a readable V# */
    N48_R5_BC_INHERITED,       /* a DRAW with no recorded destination */
    N48_R5_BC_N
};
static inline uint32_t n48_r5_blind_clause(const n48_r5_frame *x)
{
    if (!x) return N48_R5_BC_IB_UNREAD;
    if (x->ib_over != 0u) return N48_R5_BC_IB_OVER;
    if (x->out_of_scope == 1u) return N48_R5_BC_NONE;
    if (x->ib_ok != 1u) return N48_R5_BC_IB_UNREAD;
    if (x->walk_ok != 1u) return N48_R5_BC_WALK;
    if (x->targets_ok != 1u) return N48_R5_BC_TGT_TRUNC;
    if (x->memw_ok != 1u) return N48_R5_BC_MEMW_TRUNC;
    if (x->tgts_resolved != 1u) return N48_R5_BC_TGT_UNRESOLVED;
    if (x->memw_resolved != 1u) return N48_R5_BC_MEMW_UNRESOLVED;
    if (x->scan_over) return N48_R5_BC_SCAN_OVER;
    if (x->has_dispatch && x->dispatch_ok != 1u) return N48_R5_BC_DISPATCH;
    if (x->has_draw && x->ntgt == 0u && x->nmemw == 0u) return N48_R5_BC_INHERITED;
    return N48_R5_BC_NONE;
}
/* build 0.0.548 item A (the 0.0.547 review: switch 104 ON NO-GO as built) — THE CLAUSES A WINDOW MUST NEVER CLEAR. A BLIND note
 * whose clause can hide a write of DESCRIPTORS or ADDRESSES - a memory-destination write we could not record (memw-truncated) or
 * resolve (memw-unresolved), an IB we did not read or walk whole (ib-over, ib-unread, walk: anything in the unread part), a DISPATCH
 * without a readable V# (R5-REDESIGN's compute clause: a DISPATCH handed the consumer's table heap), or a DRAW with no recorded
 * destination (inherited) - is counted in the ring's `latched_blind` (n48_r5_note), which only n48_r5_scope resets: switch 104's
 * window (gfx_r5win104.h n48_r5w_blind) refuses while it is > 0. The same reason R3 refuses a held-back write to a raw-pointer page
 * (n48_cp_eval_hz below: R3 bytes can be addresses). memw-unresolved is NOT in the review's list; it is latched here because an
 * unresolved memory destination is exactly memw-truncated's hazard (a write we cannot place in the hazard set). The WINDOWED clauses
 * are the colour-target ones (targets-truncated, target-unresolved) and scan-over (the item scan's cap, every list otherwise whole). */
static inline uint32_t n48_r5_bc_latched(uint32_t c)
{
    return (c == N48_R5_BC_IB_OVER || c == N48_R5_BC_IB_UNREAD || c == N48_R5_BC_WALK || c == N48_R5_BC_MEMW_TRUNC ||
            c == N48_R5_BC_MEMW_UNRESOLVED || c == N48_R5_BC_DISPATCH || c == N48_R5_BC_INHERITED) ? 1u : 0u;
}
/* build 0.0.549 MUST-FIX 1 (the 0.0.548 review): DECIDE "LATCHED" FROM THE FRAME'S OWN FIELDS, NOT ITS FIRST FAILING
 * CLAUSE. n48_r5_blind_clause answers only the FIRST failing clause in bucket order, so a frame that is BOTH targets-truncated (windowed,
 * earlier in the order) AND memw-truncated or DISPATCH-unreadable (latched, later) was counted windowed through 0.0.548: the windowed
 * clause MASKED the descriptor / address hazard. This reads every latched field independently: the frame latches if ANY of ib_over,
 * !ib_ok, !walk_ok, !memw_ok, !memw_resolved, (has_dispatch && !dispatch_ok), or an inherited draw (has_draw, ntgt 0, nmemw 0) holds.
 * A null frame latches (fail-closed). n48_r5_note calls it only in its BLIND branch; n48_r5_bc_latched stays for the census/report
 * (which still counts by first clause) and names the same seven clauses. */
static inline uint32_t n48_r5_frame_latched(const n48_r5_frame *x)
{
    if (!x) return 1u;
    if (x->ib_over != 0u) return 1u;
    if (x->ib_ok != 1u || x->walk_ok != 1u) return 1u;
    if (x->memw_ok != 1u || x->memw_resolved != 1u) return 1u;
    if (x->has_dispatch && x->dispatch_ok != 1u) return 1u;
    if (x->has_draw && x->ntgt == 0u && x->nmemw == 0u) return 1u;
    return 0u;
}
static inline const char *n48_r5_blind_clause_name(uint32_t c)
{
    static const char *const n[N48_R5_BC_N] = { "none", "ib-over", "ib-unread", "walk", "targets-truncated", "memw-truncated",
                                                 "target-unresolved", "memw-unresolved", "scan-over", "dispatch", "inherited" };
    return c < (uint32_t)N48_R5_BC_N ? n[c] : "?";
}

/* THE COMPUTE CLAUSE. Read COMPUTE_USER_DATA_0..3 from one IB - the four dwords of the first compute V# - and
 * answer whether they form a READABLE V#: a non-zero base and a non-zero extent. From AMD's gfx10 buffer descriptor:
 * DWORD0 = base[31:0], DWORD1[15:0] = base[47:32], DWORD2 = num_records (the extent). The register is COMPUTE_USER_DATA_0
 * = GC offset 0x2e40 (`b[0] == 0x240` in a SET_SH_REG body; confirms COMPUTE_PGM_LO 0x2e0c in the same encoding),
 * and the V# occupies slots 0..3. Requires ALL FOUR slots written; anything less is not a V# we read, so it is BLIND.
 * `vbase`/`vext` are out-parameters: 0.0.397 the caller records both as destinations of the frame, so
 * their physical pages enter the hazard set - the bound derived and 0.0.395/0.0.396 threw away. */
static inline uint32_t n48_r5_dispatch_vsharp(const uint32_t *d, uint32_t n, uint64_t *vbase, uint64_t *vext)
{
    uint32_t have = 0u, slot[4] = { 0u, 0u, 0u, 0u };
    if (vbase) *vbase = 0ull;
    if (vext) *vext = 0ull;
    if (!d || n == 0u) return 0u;
    uint32_t i = 0u;
    while (i < n) {
        const uint32_t h = d[i], type = h >> 30;
        if (h == 0xFFFF1000u) { i++; continue; }
        if (type == 2u) { i++; continue; }
        if (type != 3u) break;
        const uint32_t total = 2u + ((h >> 16) & 0x3FFFu), op = (h >> 8) & 0xFFu;
        if (total > n - i) break;
        const uint32_t *b = &d[i + 1u];
        const uint32_t nb = total - 1u;
        if (op == 0x76u && nb >= 2u) {
            const uint32_t reg0 = 0x2c00u + (b[0] & 0xffffu);
            for (uint32_t k = 1u; k < nb; k++) {
                const uint32_t reg = reg0 + (k - 1u);
                if (reg >= 0x2e40u && reg <= 0x2e43u) {
                    slot[reg - 0x2e40u] = b[k];
                    have |= 1u << (reg - 0x2e40u);
                }
            }
        }
        i += total;
    }
    if (have != 0xFu) return 0u;
    const uint64_t base = (((uint64_t)(slot[1] & 0xffffu)) << 32) | (uint64_t)slot[0];
    const uint64_t ext  = (uint64_t)slot[2];
    if (vbase) *vbase = base;
    if (vext) *vext = ext;
    return (base != 0ull && ext != 0ull) ? 1u : 0u;
}

/* Read one IB into `buf` (returns the dwords read), and resolve one VA to a physical page (returns 1 on success). The two
 * callbacks are what let the SAME builder run in the host suite (a fixture map) and in the read-callback form of
 * `n48_r5_build` below, which is condition 4's lesson: a second copy of the loops is a test no mutation of the real
 * code can break. 0.0.396 ( binding fix 3): THE KEXT NO LONGER CALLS `n48_r5_build` - it hands each body it has
 * ALREADY read to `n48_r5_build_ib`, so no IB is read twice. The suite still calls `n48_r5_build`, and it runs the SAME
 * `n48_r5_build_ib`/`n48_r5_resolve` pair the kext runs, so a break in either is caught by the suite. */
typedef uint32_t (*n48_r5_read_fn)(void *ud, uint32_t k, uint32_t *buf, uint32_t cap);
typedef uint32_t (*n48_r5_resolve_fn)(void *ud, uint64_t va, uint64_t *page);

/* 0.0.396 ( binding fix 3): ONE IB BODY -> THE FRAME'S R5′ RECORD, IN PLACE, ACCUMULATING. `ib`/`got` are dwords
 * the CALLER ALREADY READ (the kext's decide loop hands its own gXdIb the moment gfxc_read filled it), so the kext never
 * re-reads an IB for R5′. Targets and memory destinations are recorded as VAs here; n48_r5_resolve below fills the physical
 * pages. The only place the three code facts meet; a break in it is caught by the suite through n48_r5_build. */
/* D4-PRIME-FIXES.md item 9 (R5' BLIND latch),  — the SAME by-page dedupe as n48_cp_scan_frame's own
 * append above, for n48_r5_build_ib's three memw-appending sites (the dispatch V#'s base/extent pages and the
 * per-IB memw sweep). Needed here too because this function ACCUMULATES across every IB of one frame (its own
 * doc), so two IBs naming the same page - or a dispatch's V# page duplicating a page the PM4 sweep already named -
 * must not count twice toward the 64-slot cap either. */
static inline int n48_r5_memw_has_page(const n48_r5_frame *x, uint64_t va)
{
    const uint64_t page = va & ~(uint64_t)0xFFFull;
    for (uint32_t q = 0; q < x->nmemw && q < N48_CP_MEMW_MAX; q++)
        if ((x->memw[q].va & ~(uint64_t)0xFFFull) == page) return 1;
    return 0;
}

static inline void n48_r5_build_ib(n48_r5_frame *x, const uint32_t *ib, uint32_t got, n48_gcap_item *items,
                                   uint32_t cap, uint64_t startVa, uint32_t flags, uint32_t *scan_over,
                                   uint64_t *max_items)
{
    if (!x || !ib || got == 0u) { if (x) { x->ib_ok = 0u; x->walk_ok = 0u; } return; }
    uint32_t total = 0u;
    const uint32_t walked = n48_gcap_scan_ex(ib, got, startVa, items, cap, &total, flags);
    /* THE WALK MUST COVER EXACTLY THE DWORDS THE READ RETURNED. Any other answer means the scanner stopped at a
     * packet it could not size - the one-dword filler, under flags 0 - and its target list is INCOMPLETE. */
    if (walked != got) x->walk_ok = 0u;
    if (total > cap) { x->scan_over = 1u; x->targets_ok = 0u; }
    if (scan_over && total > cap) *scan_over = 1u;
    if (max_items && (uint64_t)total > *max_items) *max_items = total;
    const uint32_t stored = total < cap ? total : cap;
    for (uint32_t i = 0u; i < stored; i++) {
        if (items[i].kind != N48_GCAP_CB || !items[i].va) continue;
        if (x->ntgt < N48_CP_TGT_MAX) { x->tgt[x->ntgt].va = items[i].va; x->tgt[x->ntgt].page = 0ull; x->ntgt++; }
        else x->targets_ok = 0u;
    }
    n48_cp_frame t;
    const uint32_t swept = n48_cp_scan_frame(ib, got, &t);
    if (!swept) {
        if (t.nmemw > N48_CP_MEMW_MAX) x->memw_ok = 0u;   /* our cap truncated the destination list */
        else x->walk_ok = 0u;                              /* a nested IB, a short packet, an unread walk */
    }
    /* 0.0.397: the DRAW flag, from the same PM4 walk that records the destinations. A draw with no
     * destination recorded anywhere in the frame is the inherited-destination case n48_r5_bucket refuses. */
    if (t.has_draw) x->has_draw = 1u;
    if (t.has_dispatch) {
        /* 0.0.396 ( binding fix 2): EVERY DISPATCHING IB MUST HOLD A READABLE V#. Through 0.0.395 `dispatch_ok`
         * was a UNION - one good V# set it and no later IB could clear it, so a frame whose second dispatching IB had an
         * unreadable V# was still READABLE. `has_dispatch == 0` marks the FIRST dispatching IB; after that the answer is
         * an AND, so any unreadable V# on ANY dispatching IB leaves `dispatch_ok` 0 and the bucket BLIND. */
        uint64_t vb = 0ull, ve = 0ull;
        const uint32_t vok = n48_r5_dispatch_vsharp(ib, got, &vb, &ve);
        if (x->has_dispatch == 0u) x->dispatch_ok = vok;
        else if (!vok) x->dispatch_ok = 0u;
        x->has_dispatch = 1u;
        /* 0.0.397: THE BOUND DERIVED AND 0.0.395/0.0.396 THREW AWAY. A readable V# is a POSITIVE
         * reading of where the dispatch's stores go, so its base page and the first page at the end of its extent are
         * DESTINATIONS of this frame: recorded as VAs here, resolved by n48_r5_resolve like any PM4 destination, and added
         * to the hazard set by n48_r5_note. `vb` and `vb + ve` land in the same page in the captured bodies (extent 0x400),
         * so this is the base page; recording the extent's page keeps the claim literal if a future body's extent crosses
         * a page. A V# that does not fit the record sets memw_ok 0, i.e. BLIND - the safe direction. */
        if (vok) {
            if (n48_r5_memw_has_page(x, vb)) { /* already named: not a different fact */ }
            else if (x->nmemw < N48_CP_MEMW_MAX) { x->memw[x->nmemw].va = vb; x->memw[x->nmemw].page = 0ull; x->nmemw++; }
            else x->memw_ok = 0u;
            if (n48_r5_memw_has_page(x, vb + ve)) { /* already named */ }
            else if (x->nmemw < N48_CP_MEMW_MAX) { x->memw[x->nmemw].va = vb + ve; x->memw[x->nmemw].page = 0ull; x->nmemw++; }
            else x->memw_ok = 0u;
        }
    }
    for (uint32_t i = 0u; i < t.nmemw && i < N48_CP_MEMW_MAX; i++) {
        if (n48_r5_memw_has_page(x, t.memw[i])) continue;   /* already named: not a different fact */
        if (x->nmemw < N48_CP_MEMW_MAX) { x->memw[x->nmemw].va = t.memw[i]; x->memw[x->nmemw].page = 0ull; x->nmemw++; }
        else x->memw_ok = 0u;
    }
}

/* Fill every recorded destination's PHYSICAL page. A callback that is absent or refuses makes the frame BLIND (FAIL-CLOSED):
 * the two `*_resolved` flags are cleared and `n_unresolved` counts it. Called once per frame, after all its IBs are built. */
static inline void n48_r5_resolve(n48_r5_frame *x, n48_r5_resolve_fn res, void *ud)
{
    if (!x) return;
    x->tgts_resolved = 1u; x->memw_resolved = 1u; x->n_unresolved = 0u;
    for (uint32_t i = 0u; i < x->ntgt && i < N48_CP_TGT_MAX; i++) {
        uint64_t page = 0ull;
        const uint32_t ok = (res && res(ud, x->tgt[i].va & ~0xFFFull, &page)) ? 1u : 0u;
        x->tgt[i].page = ok ? page : 0ull;
        if (!ok) { x->tgts_resolved = 0u; x->n_unresolved++; }
    }
    for (uint32_t i = 0u; i < x->nmemw && i < N48_CP_MEMW_MAX; i++) {
        uint64_t page = 0ull;
        const uint32_t ok = (res && res(ud, x->memw[i].va & ~0xFFFull, &page)) ? 1u : 0u;
        x->memw[i].page = ok ? page : 0ull;
        if (!ok) { x->memw_resolved = 0u; x->n_unresolved++; }
    }
}

/* Zero the record (the caller's `out_of_scope` reading is preserved) so a reused frame can never inherit a previous
 * frame's reads. Every flag starts 0 - BLIND - and each is set only by a POSITIVE reading. Written field by field so the
 * header stays valid C as well as C++ (no compound literals). */
static inline void n48_r5_reset(n48_r5_frame *x)
{
    if (!x) return;
    const uint32_t oos = x->out_of_scope;
    x->ib_over = 0u; x->ib_ok = 0u; x->walk_ok = 0u; x->targets_ok = 0u; x->memw_ok = 0u;
    x->tgts_resolved = 0u; x->memw_resolved = 0u; x->scan_over = 0u;
    x->has_dispatch = 0u; x->dispatch_ok = 0u; x->has_draw = 0u;
    x->ntgt = 0u; x->nmemw = 0u; x->n_unresolved = 0u;
    for (uint32_t i = 0u; i < N48_CP_TGT_MAX; i++) { x->tgt[i].page = 0ull; x->tgt[i].va = 0ull; }
    for (uint32_t i = 0u; i < N48_CP_MEMW_MAX; i++) { x->memw[i].page = 0ull; x->memw[i].va = 0ull; }
    x->out_of_scope = oos;
}

/* THE SUITE-FACING BUILDER: read each IB with the caller's callback, then resolve. `declared` is the submission's IB count
 * BEFORE the N48_XV_MAX_IBS clamp, so a frame with IBs 5+ is BLIND (`ib_over`) instead of silently READABLE over the first
 * four. The suite passes the fixture's own IB count; the kext does not call this at all (see n48_r5_build_ib). The suite
 * passes N48_GCAP_F_FILLER to reproduce the fixed walk and 0 to reproduce 0.0.394's stopped walk, which is T1's non-vacuity. */
static inline void n48_r5_build(n48_r5_frame *x, uint32_t nib, uint32_t declared, n48_r5_read_fn rd, n48_r5_resolve_fn res,
                                void *ud, uint32_t *ib, uint32_t ibcap, n48_gcap_item *items, uint32_t cap,
                                uint64_t startVa, uint32_t flags, uint32_t *scan_over, uint64_t *max_items)
{
    if (!x) return;
    n48_r5_reset(x);
    x->ib_over = (declared > nib) ? 1u : 0u;
    x->ib_ok = (nib >= 1u) ? 1u : 0u;
    x->walk_ok = x->ib_ok; x->targets_ok = x->ib_ok; x->memw_ok = x->ib_ok;
    if (!rd || !res || !ib || !items) { x->ib_ok = 0u; x->walk_ok = 0u; return; }
    for (uint32_t k = 0; k < nib && k < 16u; k++) {
        const uint32_t got = rd(ud, k, ib, ibcap);
        if (!got) { x->ib_ok = 0u; x->walk_ok = 0u; continue; }
        n48_r5_build_ib(x, ib, got, items, cap, startVa, flags, scan_over, max_items);
    }
    n48_r5_resolve(x, res, ud);
}

/* THE ARM-SCOPED R5′ RING: three scalar buckets, the instrumentation arm21 lines 1-3 print, and the hazard page set. */
typedef struct {
    n48_hazard hz;
    uint64_t bounded, readable, blind, dispatch_blind;
    uint64_t noted, rebases;
    uint64_t dest, unresolved_dest, raced;
    /* 0.0.403: frames whose destination record OUR cap truncated (`memw_ok 0`) - the reading that made
     * arm23's f77 BLIND and that no existing counter named. APPENDED, so the counters a previous boot printed keep their
     * meaning. Reset with the rest of the scope (n48_r5_scope) and printed on the hazard line. */
    uint64_t over_cap;
    uint64_t max_per_ib_items, truncated, stopped_short;
    uint32_t cap;
    uint32_t arm_seq;
    /* build 0.0.547 items 1-2 (; switch 104 "r5win", gfx_r5win104.h). APPENDED; reset with the scope. `last_blind`
     * is the frame number (`namer`, the judged frame) of the most recent BLIND note in this scope, 0 none - the ONLY input the
     * switch's window reads (n48_r5w_blind). `blind_clause[c]` counts BLIND notes by n48_r5_blind_clause (an instrument; sums to
     * `blind`). `first_lines` is the kext's first-BLIND line budget for this scope. None of the three is read by the bucket, the
     * hazard set or n48_r5_unknown: `blind` counts exactly as 0.0.546 counted it. */
    uint64_t last_blind;
    uint64_t blind_clause[N48_R5_BC_N];
    uint32_t first_lines;
    /* build 0.0.548 item A: BLIND notes whose clause n48_r5_bc_latched names (descriptor / address hazards). APPENDED; reset ONLY
     * by n48_r5_scope, never by switch 104's window. Read by n48_r5w_blind (ON) and the report; `blind` counts exactly as before. */
    uint64_t latched_blind;
} n48_r5_ring;

static inline void n48_r5_scope(n48_r5_ring *r, uint32_t seq)
{
    if (!r || seq == 0u || r->arm_seq == seq) return;
    r->arm_seq = seq;
    r->bounded = r->readable = r->blind = r->dispatch_blind = 0ull;
    r->noted = r->dest = r->unresolved_dest = r->raced = 0ull;
    r->over_cap = 0ull;
    r->max_per_ib_items = r->truncated = r->stopped_short = 0ull;
    r->cap = 512u;
    r->last_blind = 0ull; r->first_lines = 0u;   /* build 0.0.547: the window and the clause census are per scope, as `blind` */
    r->latched_blind = 0ull;                       /* build 0.0.548 item A: the latched count is per scope too, and only here */
    for (uint32_t c = 0u; c < (uint32_t)N48_R5_BC_N; c++) r->blind_clause[c] = 0ull;
    n48_hz_scope(&r->hz, seq);
    r->rebases++;
}

/* Note one held-back frame. A READABLE frame's destination pages enter the hazard set; a BLIND frame's do not, and it
 * cannot reach the decision's item-(d) question anyway because blind > 0 already refuses. `namer` is the judged frame. */
static inline void n48_r5_note(n48_r5_ring *r, const n48_r5_frame *x, uint64_t namer)
{
    if (!r || !x) return;
    r->noted++;
    r->unresolved_dest += (uint64_t)x->n_unresolved;
    const uint32_t b = n48_r5_bucket(x);
    if (b == N48_R5_BOUNDED) { r->bounded++; return; }
    if (b == N48_R5_BLIND) {
        r->blind++;
        /* build 0.0.547 (switch 104): THIS BLIND frame's number, written in the same branch as the increment, so a window
         * can never read a count without its frame (ORDER: the note runs after the judged frame's own gate, and the gate of every
         * LATER frame reads it; tests/gfx_wo99_test.cpp R1). The clause census is an instrument. */
        r->last_blind = namer;
        {
            const uint32_t c = n48_r5_blind_clause(x);
            r->blind_clause[c < (uint32_t)N48_R5_BC_N ? c : (uint32_t)N48_R5_BC_IB_UNREAD]++;
            /* build 0.0.548 item A / 0.0.549 MUST-FIX 1: a frame ANY of whose fields can hide a descriptor / address write latches,
             * decided from the frame itself (n48_r5_frame_latched), never from the first failing clause `c` (an instrument only) */
            if (n48_r5_frame_latched(x)) r->latched_blind++;
        }
        /* 0.0.403: the BLIND cause that is OUR capacity, not the frame's content. Counted here, in the
         * BLIND branch, so a BOUNDED frame (whose record is irrelevant) never inflates it. */
        if (x->memw_ok != 1u) r->over_cap++;
        if (x->has_dispatch && x->dispatch_ok != 1u) r->dispatch_blind++;
        return;
    }
    r->readable++;
    for (uint32_t i = 0u; i < x->ntgt && i < N48_CP_TGT_MAX; i++) { n48_hz_add(&r->hz, x->tgt[i].page, x->tgt[i].va, namer); r->dest++; }
    for (uint32_t i = 0u; i < x->nmemw && i < N48_CP_MEMW_MAX; i++) { n48_hz_add(&r->hz, x->memw[i].page, x->memw[i].va, namer); r->dest++; }
}

/* 0.0.397: NOTE ONE HELD-BACK FRAME AND ITS THREE PER-FRAME INSTRUMENTS, SCOPE FIRST.
 * `n48_r5_scope` re-bases EVERY counter of the ring, so the three increments that belong to THIS frame must come after
 * it. Through 0.0.396 the kext incremented stopped_short/truncated/max_per_ib_items and only THEN scoped, so the first
 * held-back frame of every scope lost all three (: F2 could read 0 falsely). The kext calls THIS, so the
 * ordering lives in exactly one place and the host suite can plant the 0.0.396 order as a mutant. */
static inline void n48_r5_note_counted(n48_r5_ring *r, uint32_t seq, const n48_r5_frame *x, uint64_t namer,
                                       uint32_t stopped_short, uint32_t truncated, uint64_t max_items)
{
    if (!r) return;
    n48_r5_scope(r, seq);                                  /* MUST precede the three increments: it re-bases them */
    if (stopped_short) r->stopped_short++;
    if (truncated) r->truncated++;
    if (max_items > r->max_per_ib_items) r->max_per_ib_items = max_items;
    n48_r5_note(r, x, namer);
}

/* R5′'s answer: the BLIND bucket. blind > 0 REFUSES. */
static inline uint64_t n48_r5_unknown(const n48_r5_ring *r) { return r ? r->blind : 1ull; }

/* THE CONSUMER, as the translator's own input list and the caller's page walk gave it. Every `*_ok` is a POSITIVE answer. */
typedef struct {
    uint32_t enumerated;    /* 1 ONLY when the translator's input list reached here and the program's ABI row is known */
    uint32_t over;          /* the translator's list, or the ABI's pointer list, did not fit: INCOMPLETE, refuse */
    uint32_t n;             /* image inputs */
    uint64_t va[N48_CP_IN_MAX];
    uint32_t mode[N48_CP_IN_MAX];       /* gfx12 SW_MODE; 0 = linear */
    uint32_t proven[N48_CP_IN_MAX];     /* R1: a ledger entry OF THIS ARM vouches for this surface */
    uint32_t resolved[N48_CP_IN_MAX];   /* R2: the base page resolved through THIS consumer's VM at DECIDE */
    uint32_t nptr;
    uint64_t ptr[N48_CP_PTR_MAX];       /* R3: table heap, sampler heap, S#, and every raw-pointer page the ABI declares */
    uint32_t ptr_resolved[N48_CP_PTR_MAX];
    /* 0.0.395 (R5′ "Keying is PHYSICAL, not VA"): the RESOLVED PHYSICAL PAGE of each pointer, which the physical-keyed R3
     * asks `n48_hz_hit` about. Filled by the same `gfxc_page` call that sets `ptr_resolved`, and read ONLY by
     * n48_cp_eval_hz. The VA-keyed n48_cp_eval does not read it, so at 30 OFF this is never consulted. */
    uint64_t ptr_page[N48_CP_PTR_MAX];
    /* 0.0.391: declared pointer slots THIS submission never wrote, so their value is whatever an
     * earlier one left in the SGPR pair - a page this rule cannot name, let alone resolve. It REFUSES, as a list that did
     * not fit refuses; what changes is that it no longer refuses under the OTHER one's name.
     * CORRECTED 0.0.393: the old measurement here said only PS user-data 0..3 are written, but it was taken on
     * tests/fixture_arm16_ws.h, which is FRAME 1 - the FILL (ViewportToNDC + ColorFill), NOT a plane frame. The real
     * GPUPass PLANE frame (arm13 f15, dword 0x3cd, decoded in) writes fragment user-data slots 0 THROUGH 13 in one
     * packet, so s10:s11 and s12:s13 are NOT inherited on the real path: ptr_inherit is 0 there. The stale sentence sat
     * here and misled two readers. */
    uint32_t ptr_inherit;
    uint32_t waits;         /* R4: waits the consumer carries (`WAIT_REG_MEM` and friends) */
    uint32_t memwrites;     /* R4: memory-destination writes it carries, EXCLUDING the NOP-wrapped dead fence */
    /* 0.0.406 — AN INPUT-FREE CONSUMER: the `ws_B_ColorFill` shape. A ColorFill carries NO image inputs
     * (n 0) and NO class-19 table; its only dereferenced input is the 16-byte float4 its SPI_SHADER_USER_DATA_PS_2/_3 pair
     * names (a CPU-written buffer, or OUR arena after the retarget). measured that such a frame never enumerates
     * as a consumer at all (no table step -> `in_abi` 0 -> `enumerated` 0), so `n48_dep_check` fell to 0.0.389's
     * `source_neuters` rung and EVERY later fill was refused from f3 on, while only f1 (no held-back frame before it) ever
     * committed. This flag is set ONLY by gfx_cp_build.h's n48_cp_build_input_free, which is called ONLY while switch 33 is
     * on and ONLY for nseg 1 + the ColorFill PS + no descriptor table + no image reads + one named pointer VA. It is the
     * ONE thing that lets n48_cp_eval_fill_hz judge such a consumer (R3's hazard question only). APPENDED, so no existing
     * field moves; 0 on every default build and on every non-fill consumer, where the rule above is byte for byte 0.0.405's. */
    uint32_t input_free;
} n48_cp_consumer;

/* =====================================================================================================================
 * D4' (notes/design/D4-PRIME.md, notes/design/R1-MEMDST.md Q5) — A SEPARATE, WIDER CONSUMER, gated ENTIRELY by switch
 * 40 (`gfxneuter 40 | M<<8`). n48_cp_consumer and every function above (n48_cp_eval, n48_cp_eval_hz,
 * n48_cp_build_input_free/n48_cp_eval_fill_hz) are UNTOUCHED by this section - same caps (N48_CP_IN_MAX/PTR_MAX),
 * same behaviour, whatever switch 40 does. This type exists ONLY to widen coverage to the programs
 * src/xlat12/xlat12_readset.h admits (ColorFill, Tex_PS's inline image, the STRICT-OK-NONE rows, Family A's G/I/V/L)
 * that the table-ABI path (kDTableAbi: GPUPass/UberCompositeFragment only) never enumerates.
 * ===================================================================================================================== */
#define N48_CP_D4_IN_MAX  16u   /* D4-PRIME item 4/G: the raised image-input cap while switch 40 is ON */
#define N48_CP_D4_PTR_MAX 64u   /* D4-PRIME item 4/G: the raised pointer cap while switch 40 is ON */

typedef struct {
    uint32_t enumerated;   /* 1 only when this FRAME's D4' read-set was POSITIVELY built (every draw of every merged
                             * segment admitted) - a frame D4' never touched stays 0, exactly like n48_cp_consumer's
                             * own `enumerated`. */
    uint32_t over;         /* a draw's program had no admitted row, a list did not fit, or a merged segment declined:
                             * INCOMPLETE, refuse (N48_CP_LIST_OVER). */
    uint32_t n;
    uint64_t va[N48_CP_D4_IN_MAX];
    uint32_t mode[N48_CP_D4_IN_MAX], proven[N48_CP_D4_IN_MAX], resolved[N48_CP_D4_IN_MAX];
    uint32_t nptr;
    uint64_t ptr[N48_CP_D4_PTR_MAX];
    uint32_t ptr_resolved[N48_CP_D4_PTR_MAX];
    uint64_t ptr_page[N48_CP_D4_PTR_MAX];   /* R3's physical hazard question, filled by the same gfxc_page walk n48_cp_consumer's own ptr_page is */
    uint32_t waits, memwrites;   /* R1-MEMDST.md Q5 binding: copied from xlat12_draw_stats' r4_waits/r4_memwrites, unconditionally */
} n48_cp_consumer_d4;

enum {
    N48_CP_OK = 0,
    N48_CP_NOT_ENUM,     /* nobody enumerated this consumer: not a clean consumer, an unread one */
    N48_CP_LIST_OVER,    /* the input or pointer list overflowed: the read-set is INCOMPLETE */
    N48_CP_R1_TILED,     /* a tiled input with no ledger entry of this arm */
    N48_CP_R2_PAGE,      /* an input whose base page did not resolve through the consumer's own VM */
    N48_CP_R3_PTR_PAGE,  /* a raw-pointer page that did not resolve */
    N48_CP_R3_WITNESS,   /* a raw-pointer page a neutered frame since the arm named as a colour target */
    N48_CP_R3_MEMDST,    /* ... or as a decoded memory-write destination */
    N48_CP_R4_WAIT,      /* the consumer carries a wait: the census says this shape does not, so this is a new shape */
    N48_CP_R4_MEMDST,    /* the consumer carries a memory-destination write past the dead fence */
    /* 0.0.391. APPENDED, never inserted: the read-out prints the clause tally BY INDEX, so a
     * value that moved would make every recorded run's numbers mean something else. */
    N48_CP_R3_PTR_INHERITED, /* a pointer the program's ABI declares that THIS submission did not write: value unknown */
    N48_CP_REASONS
};

static inline const char *n48_cp_reason_name(uint32_t r)
{
    static const char *const n[N48_CP_REASONS] = {
        "clean", "not-enumerated", "list-overflow", "R1-tiled-unproven", "R2-input-page", "R3-pointer-page",
        "R3-in-witness", "R3-neutered-write-destination", "R4-consumer-wait", "R4-consumer-write",
        "R3-pointer-inherited" };
    return r < N48_CP_REASONS ? n[r] : "?";
}

/* True when `page` (a 4 KiB-aligned base page or a VA reduced to one) is named by any row of the arm-scoped witness. */
static inline int n48_cp_in_witness(const n48_dep_witness *wt, uint64_t va)
{
    if (!wt) return 1;                       /* no witness is not "no rows": refuse */
    const uint32_t cap = n48_dep_rows_cap(wt);
    for (uint32_t i = 0; i < wt->used && i < cap; i++)
        if ((wt->row[i].va & ~0xFFFull) == (va & ~0xFFFull)) return 1;
    return 0;
}

static inline int n48_cp_is_memdst(const n48_cp_ring *r, uint64_t va)
{
    if (!r) return 1;
    for (uint32_t i = 0; i < r->n && i < N48_CP_RING; i++)
        for (uint32_t k = 0; k < r->f[i].nmemw && k < N48_CP_MEMW_MAX; k++)
            if ((r->f[i].memw[k] & ~0xFFFull) == (va & ~0xFFFull)) return 1;
    return 0;
}

/* THE RULE. Returns N48_CP_OK or the FIRST clause that refused, with *unproven carrying how many inputs failed it and
 * *stale the count of R1 surfaces a later neutered frame overwrote (tolerated, never gating -, step 8's price). */
static inline uint32_t n48_cp_eval(const n48_cp_consumer *c, const n48_cp_ring *r, const n48_dep_witness *wt,
                                   uint64_t *unproven, uint64_t *stale)
{
    if (unproven) *unproven = 0ull;
    if (stale) *stale = 0ull;
    if (!c || !r || !wt) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->enumerated != 1u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->over || c->n > N48_CP_IN_MAX || c->nptr > N48_CP_PTR_MAX) {
        if (unproven) *unproven = c->over ? (uint64_t)c->over : 1ull;
        return N48_CP_LIST_OVER;
    }
    if (c->n == 0u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }   /* a consumer that reads nothing was not read */

    /* R1 — every TILED input must carry a ledger entry of THIS arm. */
    { uint64_t bad = 0ull;
      for (uint32_t i = 0; i < c->n; i++) if (c->mode[i] && c->proven[i] != 1u) bad++;
      if (bad) { if (unproven) *unproven = bad; return N48_CP_R1_TILED; } }
    /* R2 — every input's base page must resolve through the consumer's own VM. Today a LINEAR T# passes with `lin++` and
     * no page check at all (xlat12_ib.c's `if (!mode && !proven) lin++`); this is the rung that closes it. */
    { uint64_t bad = 0ull;
      for (uint32_t i = 0; i < c->n; i++) if (c->resolved[i] != 1u) bad++;
      if (bad) { if (unproven) *unproven = bad; return N48_CP_R2_PAGE; } }
    /* R3 — heap pages, S# and every raw-pointer page the ABI declares: resolvable, never in the arm-scoped witness, and
     * never a decoded memory-write destination of a neutered frame since the arm. */
    { uint64_t noPage = 0ull, inWt = 0ull, dst = 0ull;
      /* 0.0.391: asked FIRST of the three, and asked HERE rather than beside `over` at the head
       * of the rule, so R1 and R2 get to speak about this consumer before it. An inherited pointer is a page we cannot
       * name; naming it is R3's whole job, so this is R3's refusal and it carries R3's name. */
      if (c->ptr_inherit) { if (unproven) *unproven = (uint64_t)c->ptr_inherit; return N48_CP_R3_PTR_INHERITED; }
      for (uint32_t i = 0; i < c->nptr; i++) {
          if (c->ptr_resolved[i] != 1u) { noPage++; continue; }
          if (n48_cp_in_witness(wt, c->ptr[i])) inWt++;
          if (n48_cp_is_memdst(r, c->ptr[i])) dst++;
      }
      if (noPage) { if (unproven) *unproven = noPage; return N48_CP_R3_PTR_PAGE; }
      if (inWt) { if (unproven) *unproven = inWt; return N48_CP_R3_WITNESS; }
      if (dst) { if (unproven) *unproven = dst; return N48_CP_R3_MEMDST; } }
    /* R4 — the consumer itself carries no wait and no memory-destination write. Vacuous for the shape enumerated,
     * and a RUNG anyway: the day a consumer of another shape reaches here, it refuses instead of being assumed. */
    if (c->waits) { if (unproven) *unproven = c->waits; return N48_CP_R4_WAIT; }
    if (c->memwrites) { if (unproven) *unproven = c->memwrites; return N48_CP_R4_MEMDST; }

    /* TOLERATED AND COUNTED: a neutered frame since the arm that OVERWROTE an input we proved. The bytes were ours when
     * the ledger recorded them; what was lost is window content, which is step 8's price and not a safety property. */
    if (stale) {
        uint64_t st = 0ull;
        for (uint32_t i = 0; i < r->n && i < N48_CP_RING; i++)
            for (uint32_t k = 0; k < r->f[i].ntgt && k < N48_CP_TGT_MAX; k++)
                for (uint32_t j = 0; j < c->n; j++)
                    if ((r->f[i].tgt[k] & ~0xFFFull) == (c->va[j] & ~0xFFFull)) st++;
        *stale = st;
    }
    return N48_CP_OK;
}

/* 0.0.395 (R5-REDESIGN.md v2, "What the consumer may conclude — (d)" and "Keying is PHYSICAL, not VA"): R1-R4 ASKED WITH
 * A PHYSICAL R3. Identical to n48_cp_eval above except R3, which is asked of the ARM-SCOPED HAZARD PAGE SET
 * (gfx_hazard.h) by the consumer's pointer's RESOLVED PHYSICAL PAGE instead of the old witness's VAs:
 *
 *   - an R3 pointer page a held-back frame named as ANY destination - a colour target OR a decoded memory-destination
 *     write - REFUSES under the SAME clause name, R3-neutered-write-destination (N48_CP_R3_MEMDST). "Any destination" is
 *     the whole point: the old rule asked the witness (targets) and the ring (memory destinations) as two VA questions;
 *     the new one asks one physical question of the union, which is what catches a held-back frame writing a page THIS
 *     consumer reads through a DIFFERENT VA (the aliasing hole VA keying left open).
 *   - a consumer's IMAGE INPUT named as a colour target is TOLERATED and counted in `stale` exactly as before: texels are
 *     data, never addresses, and R2 has proved the page resolves, so the read cannot fault.
 *
 * The old VA-keyed n48_cp_eval is UNTOUCHED and is what the kext calls while the new switch is OFF; `r5` is read by
 * nothing on that path, so the decision there is byte for byte 0.0.394's. */
static inline uint32_t n48_cp_eval_hz(const n48_cp_consumer *c, const n48_cp_ring *r, const n48_dep_witness *wt,
                                      n48_r5_ring *r5, uint64_t *unproven, uint64_t *stale)
{
    if (unproven) *unproven = 0ull;
    if (stale) *stale = 0ull;
    if (!c || !r || !wt || !r5) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->enumerated != 1u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->over || c->n > N48_CP_IN_MAX || c->nptr > N48_CP_PTR_MAX) {
        if (unproven) *unproven = c->over ? (uint64_t)c->over : 1ull;
        return N48_CP_LIST_OVER;
    }
    if (c->n == 0u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }

    { uint64_t bad = 0ull;
      for (uint32_t i = 0; i < c->n; i++) if (c->mode[i] && c->proven[i] != 1u) bad++;
      if (bad) { if (unproven) *unproven = bad; return N48_CP_R1_TILED; } }
    { uint64_t bad = 0ull;
      for (uint32_t i = 0; i < c->n; i++) if (c->resolved[i] != 1u) bad++;
      if (bad) { if (unproven) *unproven = bad; return N48_CP_R2_PAGE; } }
    { uint64_t noPage = 0ull, dst = 0ull;
      if (c->ptr_inherit) { if (unproven) *unproven = (uint64_t)c->ptr_inherit; return N48_CP_R3_PTR_INHERITED; }
      for (uint32_t i = 0; i < c->nptr; i++) {
          if (c->ptr_resolved[i] != 1u || c->ptr_page[i] == 0ull) { noPage++; continue; }
          if (n48_hz_hit(&r5->hz, c->ptr_page[i])) dst++;
      }
      if (noPage) { if (unproven) *unproven = noPage; return N48_CP_R3_PTR_PAGE; }
      if (dst) { if (unproven) *unproven = dst; return N48_CP_R3_MEMDST; } }
    if (c->waits) { if (unproven) *unproven = c->waits; return N48_CP_R4_WAIT; }
    if (c->memwrites) { if (unproven) *unproven = c->memwrites; return N48_CP_R4_MEMDST; }
    if (stale) {
        uint64_t st = 0ull;
        for (uint32_t i = 0; i < r->n && i < N48_CP_RING; i++)
            for (uint32_t k = 0; k < r->f[i].ntgt && k < N48_CP_TGT_MAX; k++)
                for (uint32_t j = 0; j < c->n; j++)
                    if ((r->f[i].tgt[k] & ~0xFFFull) == (c->va[j] & ~0xFFFull)) st++;
        *stale = st;
    }
    return N48_CP_OK;
}

/* =====================================================================================================================
 * D4' (notes/design/D4-PRIME.md item 5/6, notes/design/R1-MEMDST.md Q5) — R1-R4 OVER THE D4' CONSUMER. IDENTICAL to
 * n48_cp_eval_hz above (same clause order and names, same physical R3 over the r5 hazard set), with ONE deliberate
 * difference: `c->n == 0 && c->nptr == 0` is NOT refused as N48_CP_NOT_ENUM. For the table-ABI consumer that shape
 * means "nobody built this consumer"; for D4' it can ALSO mean the STRICT-OK-NONE proof (Const_PS_gfx1201,
 * RectPosTexFast_VS_gfx1201: xlat12_readset.h proves, from the program's own code, that it performs no memory read at
 * all) - and the ONLY other way to reach `enumerated 1, over 0, n 0, nptr 0` here is every draw's row being admitted
 * and contributing nothing, because a declined draw sets `over` in the builder (gfx_cp_build.h's
 * n48_cp_build_consumer_d4), never a silent zero. R1/R2/R3/R4 below are then vacuous for it (their loops iterate zero
 * times) and the verdict is N48_CP_OK, by construction rather than by omission - the same shape the input-free fill's
 * R1/R2 are already vacuous for. Binding (R1-MEMDST.md Q5): D4-PRIME's own T3 expected answer named
 * R4-consumer-write, but `waits` is asked BEFORE `memwrites` below (as it already is in n48_cp_eval_hz above) - a
 * consumer carrying both refuses R4-consumer-WAIT first. */
static inline uint32_t n48_cp_eval_hz_d4(const n48_cp_consumer_d4 *c, const n48_cp_ring *r, const n48_dep_witness *wt,
                                         n48_r5_ring *r5, uint64_t *unproven, uint64_t *stale)
{
    if (unproven) *unproven = 0ull;
    if (stale) *stale = 0ull;
    if (!c || !r || !wt || !r5) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->enumerated != 1u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->over || c->n > N48_CP_D4_IN_MAX || c->nptr > N48_CP_D4_PTR_MAX) {
        if (unproven) *unproven = c->over ? (uint64_t)c->over : 1ull;
        return N48_CP_LIST_OVER;
    }
    { uint64_t bad = 0ull;
      for (uint32_t i = 0; i < c->n; i++) if (c->mode[i] && c->proven[i] != 1u) bad++;
      if (bad) { if (unproven) *unproven = bad; return N48_CP_R1_TILED; } }
    { uint64_t bad = 0ull;
      for (uint32_t i = 0; i < c->n; i++) if (c->resolved[i] != 1u) bad++;
      if (bad) { if (unproven) *unproven = bad; return N48_CP_R2_PAGE; } }
    { uint64_t noPage = 0ull, dst = 0ull;
      for (uint32_t i = 0; i < c->nptr; i++) {
          if (c->ptr_resolved[i] != 1u || c->ptr_page[i] == 0ull) { noPage++; continue; }
          if (n48_hz_hit(&r5->hz, c->ptr_page[i])) dst++;
      }
      if (noPage) { if (unproven) *unproven = noPage; return N48_CP_R3_PTR_PAGE; }
      if (dst) { if (unproven) *unproven = dst; return N48_CP_R3_MEMDST; } }
    if (c->waits) { if (unproven) *unproven = c->waits; return N48_CP_R4_WAIT; }
    if (c->memwrites) { if (unproven) *unproven = c->memwrites; return N48_CP_R4_MEMDST; }
    if (stale) {
        uint64_t st = 0ull;
        for (uint32_t i = 0; i < r->n && i < N48_CP_RING; i++)
            for (uint32_t k = 0; k < r->f[i].ntgt && k < N48_CP_TGT_MAX; k++)
                for (uint32_t j = 0; j < c->n; j++)
                    if ((r->f[i].tgt[k] & ~0xFFFull) == (c->va[j] & ~0xFFFull)) st++;
        *stale = st;
    }
    return N48_CP_OK;
}

/* 0.0.406: THE INPUT-FREE FILL CONSUMER, JUDGED BY R3's HAZARD QUESTION ONLY.'s whole point: a
 * ColorFill has no image inputs, so R1 and R2 are structurally vacuous for it, and its only dereferenced input is ONE
 * pointer page - the page of the PS_2/3 colour pointer AFTER any fill-colour retarget (our arena page, which no held-back
 * frame can name). So the honest rule for it is R3's physical hazard question and nothing else; R5′ (blind) lives in the
 * world's `r5_blind`, which n48_dep_check asks BEFORE this clause's count (in r5_mode). This is a SEPARATE function so the
 * two reviewed evaluators above are untouched: with `input_free` 0 - every consumer of every switch-off build - neither
 * this function nor its clause exists on the call path.
 *
 * FAIL-CLOSED. Only a consumer the BUILDER marked input_free is judged here; anything else reads NOT_ENUM, so a caller
 * that hands this a plain zero consumer can never get a clean answer. A malformed input-free consumer - one carrying image
 * inputs, or not exactly ONE pointer page - is NOT the shape and refuses rather than being read past. A pointer page that
 * did not resolve, or that a held-back frame named as ANY destination, refuses. There is no `stale` count to take: the
 * stale instrument is over image inputs, and this consumer has none. */
static inline uint32_t n48_cp_eval_fill_hz(const n48_cp_consumer *c, n48_r5_ring *r5,
                                            uint64_t *unproven, uint64_t *stale)
{
    if (unproven) *unproven = 0ull;
    if (stale) *stale = 0ull;
    if (!c || !r5) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->enumerated != 1u || c->input_free != 1u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->over || c->n > N48_CP_IN_MAX || c->nptr > N48_CP_PTR_MAX) {
        if (unproven) *unproven = c->over ? (uint64_t)c->over : 1ull;
        return N48_CP_LIST_OVER;
    }
    /* An input-free consumer reads no images, and names exactly ONE pointer page. Either being wrong is not this shape. */
    if (c->n != 0u || c->nptr != 1u) { if (unproven) *unproven = 1ull; return N48_CP_NOT_ENUM; }
    if (c->ptr_inherit) { if (unproven) *unproven = (uint64_t)c->ptr_inherit; return N48_CP_R3_PTR_INHERITED; }
    if (c->ptr_resolved[0] != 1u || c->ptr_page[0] == 0ull) { if (unproven) *unproven = 1ull; return N48_CP_R3_PTR_PAGE; }
    if (n48_hz_hit(&r5->hz, c->ptr_page[0])) { if (unproven) *unproven = 1ull; return N48_CP_R3_MEMDST; }
    return N48_CP_OK;
}


/* =====================================================================================================================
 * 0.0.358 — X9 v2: THE COMPLETE COUNT. Design: notes/M4-X9-COMPLETE.md, and the classes C1-C15 of its.
 * =====================================================================================================================
 * WHY v1 WAS BLIND. v1's world was five counters the kext copied by hand (gfxsrc_dep_world). A counter nobody feeds reads 0,
 * and 0 reads as clean: that is how SecurityAgent's VMID-3 IB (C4, `gGs.refused[2]`, never read), the neutered TLB-ACK polls
 * (C10, `gDx.pollsNeutered`), SDMA-drain failures (C9), native GFX (C6/C7) and ring resets (C11) all stayed invisible.
 *
 * WHAT v2 CHANGES, AND WHY IT IS FAIL-CLOSED BY CONSTRUCTION:
 *   1. The kext only GATHERS: raw counters into n48_dep_src.v[] by index, raw install states into scalars. The MAPPING from
 *      raw counters to the world - where every v1 blind spot was born - is n48_dep_fill below, pure and host-tested.
 *   2. OBSERVERS. Each class is fed by one observer (the source hook, the ring walk, the SDMA drain, the stall detector) and
 *      each observer's bit is set from its LIVE install state, in the fill, and nowhere else. A world whose observers are
 *      not all live refuses N48_DEP_NOT_OBSERVED - "nobody was watching" can never read as "nothing happened". EARLY covers
 *      the time before the ring walk existed: its bit needs the arm-time walk of the ring's contents to have found no IB.
 *   3. IDENTITIES. Every outcome counter must add up to the counter of events it partitions; a code path that forgets to
 *      count its outcome breaks one and refuses N48_DEP_UNACCOUNTED rather than disappearing.
 *   4. MONOTONICITY. Counters only go up. A fill that sees one go down (a reset, a wrap, a re-zeroed struct) LATCHES
 *      N48_DEP_NONMONOTONE for the rest of the boot, because what the reset erased is gone.
 * Every imprecision still pushes towards refusing: source refusals are counted as escaped even when the ring walk NOPs the
 * IB later (the source cannot know), IBs found while disarmed count as native even if the old suite route translated them.
 *
 * 0.0.367 CHANGES ONE LINE OF THE PARAGRAPH THAT STOOD HERE. v2 said "NOT COVERED, NAMED: C13 (compute user
 * queues: the kext has no observer on them)". That was true of the counters v2 gathered and NOT true of what the kext can
 * see: Apple reaches a hardware queue only through our TTL, and the TTL's slot 36 is live from load to unload. C13 now has
 * a POSITIVELY LIVE observer (N48_DEP_OBS_QUEUE) and a class that refuses at one, and C16 (GPU VM faults) has both too.
 * STILL NOT COVERED, NAMED: C14 (a CPU-written tiled surface in Apple's swizzle: not a GPU event, not countable; it belongs
 * to the T# rule, notes/M4-DESC-TABLE.md, and the residency-provenance ledger now answers the one case that matters).
 * THE RESIDUE C13 DOES NOT CLOSE, written down rather than hidden: the KIQ packet emulator SETTLES (gfxmap_poller's quiet
 * exit), so a KIQ packet submitted after it stops is not decoded. That is why C13's liveness rests on slot 36 and not on the
 * emulator: no queue can be started without slot 36, and the emulator's two counters are an ADDITIONAL refusing input for
 * the window in which it ran. SUSPECTED, not confirmed: that Apple's AMDRadeonX6000 has no route to a hardware queue that
 * bypasses the TTL (slots 20/34/41 all answer UNSUPPORTED and none has ever been called in a logged boot). */

/* The raw counters, by index. APPENDED, never inserted (the verb prints them by index). */
enum {
    /* the source hook, hook_gfxCommitIB */
    N48_DEPC_SRC_CALLS = 0,          /* gGs.calls: every entry */
    N48_DEPC_SRC_NEUTERED,           /* gGs.neuteredSubs: VMID-2 submissions NOPed at the source */
    N48_DEPC_SRC_NEUTERED_OTHER,     /* gGs.neuteredOtherSubs: other-VMID submissions NOPed at the source (0.0.358) */
    N48_DEPC_SRC_TRANSLATED,         /* gGs.translated */
    N48_DEPC_SRC_PASS_DISARMED,      /* gGs.passDisarmed: disarmed, another channel, or a user-half info */
    N48_DEPC_SRC_REFUSED1,           /* gGs.refused[1..5]: passed to Apple for a shape reason (not IB-list, not VMID 2 ... */
    N48_DEPC_SRC_REFUSED2,           /* ... and not neuterable at another VMID either, count, template, no template) */
    N48_DEPC_SRC_REFUSED3,
    N48_DEPC_SRC_REFUSED4,
    N48_DEPC_SRC_REFUSED5,
    /* the ring walk, hook_gfxWriteTail */
    N48_DEPC_RING_IBS_FOUND,         /* INDIRECT_BUFFERs of ANY VMID in walked frames */
    N48_DEPC_RING_IBS_ARMED,         /* ... of those, in frames walked while the neuter was armed */
    N48_DEPC_RING_IBS_NATIVE,        /* ... and while it was not */
    N48_DEPC_RING_FRAMES_NEUTERED,   /* frames walked armed with at least one IB (each goes to the ring NOP) */
    N48_DEPC_RING_WALK_STOPS,        /* frames whose walk stopped on a header it could not size: the tail was not seen */
    N48_DEPC_RING_UNUSABLE,          /* frames skipped because the ring was not walkable */
    N48_DEPC_RING_BACKWARDS,         /* GFX ring write pointer went backwards (0.0.358) */
    N48_DEPC_RING_PUBLISHED_EARLY,   /* the CP was told about dwords the walk had not reached (gPub.publishedEarly) */
    N48_DEPC_GN_FRAMES,              /* gGn.frames: frames the ring NOP handled */
    N48_DEPC_GN_IBS,                 /* gGn.ibs: IB packets NOPed and read back */
    N48_DEPC_GN_MISMATCH,            /* gGn.mismatches: NOP written, read back different */
    N48_DEPC_GN_NOT_IB,              /* gGn.notIb */
    N48_DEPC_GN_OVER,                /* gGn.overCap */
    N48_DEPC_GN_WALK_STOP,           /* gGn.walkStopFrames */
    N48_DEPC_GN_RACED,               /* gGn.raced */
    /* the SDMA drain */
    N48_DEPC_DX_IBS,
    N48_DEPC_DX_CHANGED, N48_DEPC_DX_UNCHANGED, N48_DEPC_DX_REFUSED, N48_DEPC_DX_UNREADABLE, N48_DEPC_DX_OVERCAP,
    N48_DEPC_DX_WRITE_FAIL, N48_DEPC_DX_VERIFY_BAD,
    N48_DEPC_DX_BEYOND_CAP, N48_DEPC_DX_OVERRUNS,
    N48_DEPC_DX_POLLS_NEUTERED, N48_DEPC_DX_POLLS_KEPT, N48_DEPC_DX_BACKWARDS,
    /* the stall detector */
    N48_DEPC_STALL_SAMPLES, N48_DEPC_STALLS, N48_DEPC_STALL_UNREADABLE,
    /* the witness (v1's two refusing counts) */
    N48_DEPC_WT_UNRESOLVED, N48_DEPC_WT_OVER, N48_DEPC_WT_TRUNC,
    /* 0.0.359: the ring walk SPARED a committed frame's IB (gfx_neuter.h n48_gfxn_nop_list). Reachable only
     * with COMMIT armed; it is a bucket of the ring NOP identity, and each one must be backed by a source translation. */
    N48_DEPC_GN_EXEMPT,
    /* 0.0.367 — C13, THE ENGINE-QUEUE CENSUS. Apple cannot put work on a hardware queue without asking our TTL:
     * slot 36 startEngineQueue is the ONLY route to a ring (5561 recorded calls across every logged boot carry exactly the four
     * types 7 SDMA-QUEUE0 / 8 KIQ / 9 Apple GFX / 10 SDMA-QUEUE1), slot 41 sendRequestToMES answers UNSUPPORTED (0 calls ever
     * logged) and slot 20 submitFrame answers UNSUPPORTED (0 calls ever logged). Each of the four types has an observer: 7/10
     * the SDMA drain, 8 the KIQ packet emulator, 9 the source hook + the ring walk. A FIFTH type has none, so it refuses.
     * On the KIQ ring itself, upstream's gfx_v12_0_kiq_map_queues (ref/linux-amdgpu/gfx_v12_0.c:314-326) encodes
     * AMDGPU_RING_TYPE_COMPUTE as engine_sel 0 and GFX as 4; our emulator maps 4 and only 4, so a MAP_QUEUES at engine_sel 0
     * is a compute queue Apple believes is live and no engine fetches - a silent drop, and it refuses. (engine_sel 1, Apple's
     * ringType 5, is neither: 349 of them appear in the logs, never mapped, and it is counted apart as an instrument.) */
    N48_DEPC_Q_STARTS,           /* TTL slot 36 startEngineQueue calls */
    N48_DEPC_Q_KNOWN_TYPE,       /* ... of a type an observer of ours covers (7, 8, 9, 10) */
    N48_DEPC_Q_UNKNOWN_TYPE,     /* ... of any other type: an engine queue nobody watches */
    N48_DEPC_Q_MES_REQUESTS,     /* TTL slot 41 sendRequestToMES: a queue operation we refuse and cannot see */
    N48_DEPC_Q_TTL_SUBMITS,      /* TTL slot 20 submitFrame: a submission we refuse and cannot see */
    N48_DEPC_Q_KIQ_COMPUTE,      /* KIQ MAP_QUEUES/UNMAP_QUEUES at engine_sel 0 (COMPUTE, upstream's encoding) */
    N48_DEPC_Q_KIQ_UNKNOWN,      /* KIQ TYPE3 packets the emulator did not decode (its `default:` branch) */
    /* 0.0.367 — C16, THE GPU VM FAULT. Read-only, from the SAME registers the M4-WS-VMID-VALID instrument reads
     * (GCVM_L2_PROTECTION_FAULT_STATUS_LO32, GC BASE_IDX 0 + 0x15d0). First-fault latched and NEVER cleared here. */
    N48_DEPC_VM_FAULTS,          /* gathers whose read of STATUS_LO32 was non-zero (latched, never cleared) */
    N48_DEPC_COUNT
};

/* Everything the fill needs, as the kext read it. Zero-initialised; `gathered` must be POSITIVELY set. */
typedef struct {
    uint32_t gathered;       /* 1 when the kext filled this from its live state - REQUIRED, or the world is NOT_SAMPLED */
    uint32_t src_install;    /* gGs.installState (1 installed) */
    uint32_t ring_state;     /* gRenderDrainState (1 armed, 2 armed and wrote) */
    uint32_t ring_hooked;    /* 1 when gGfxRingHooked is set */
    uint32_t sdma_state;     /* gDrainState (2 live) */
    uint32_t stall_armed;    /* 1 when the stall detector was armed with the ring walk */
    uint32_t stall_read_ok;  /* 1 when THIS gather's own read of CP_RB0_RPTR/WPTR was not all-ones */
    uint32_t pre_walked;     /* 1 when render_drain_arm walked the ring's contents [0, wptr) */
    uint32_t pre_wrapped;    /* ... and the ring had wrapped, so [0, wptr) is not all it ever held */
    uint32_t pre_stopped;    /* ... and the walk stopped before the end */
    uint32_t pre_ibs;        /* ... INDIRECT_BUFFERs of any VMID it found */
    /* 0.0.368 - RULE E1, gfx_e1.h. `pre_e1` is 1 ONLY when n48_e1_eval returned N48_E1_PASS at arm time on a
     * ring that held exactly ONE IB and that IB was proven to be our own clear-state NOP page. It is the ONLY thing that may
     * widen EARLY, it widens nothing else, and 0 (the zero-initialised value, and the value on every refusal) leaves EARLY
     * exactly where 0.0.367 left it: not live, X9 v2 refusing every frame. `pre_e1_clause` is the verdict for the log. */
    uint32_t pre_e1;         /* 1 when RULE E1 PASSED at arm time - POSITIVELY set, never inferred */
    uint32_t pre_e1_clause;  /* n48_e1_eval's return: 0 PASS, otherwise the clause that refused (instrument only) */
    uint32_t inflight;       /* source-hook calls entered and not yet attributed (1 when gathered inside the hook) */
    uint32_t snap_ok;        /* 1 when the ring and drain counters were read under their own locks */
    /* 0.0.367 - C13's and C16's liveness, each POSITIVELY set by the gather from live state and nothing else. */
    uint32_t q_hook_live;    /* 1 when our TTL is the object Apple calls AND slot 36 has been entered at least once */
    uint32_t q_tally_ok;     /* 1 when the queue-type tally did not overflow (a lost start is an unwatched queue) */
    uint32_t fault_read_ok;  /* 1 when THIS gather's own read of GCVM_L2_PROTECTION_FAULT_STATUS_LO32 happened */
    /* 0.0.372 — X9-F, THE PRE-BASELINE FORGIVENESS, AND THE THREE THINGS THE FILL DEMANDS BEFORE IT WILL
     * SUBTRACT ANYTHING. The RULE is gfx_forgive.h's (pure, host-tested by tests/gfx_forgive_test.cpp, evaluated by the
     * kext's gather under its own monotone latch); what arrives here is only its VERDICT and its AMOUNT, so this fill
     * cannot be the place a forgiveness is invented. All three are zero-initialised, and the zeroes are the default
     * build's values on every fill: `fg_enabled` 0 (the switch is OFF at boot and OFF unless an operator set it),
     * `fg_clause` 0 is N48_FG_PASS but `fg_forgive` 0 means there is nothing to subtract, so the arithmetic below is
     * byte-identical to 0.0.371's. */
    uint32_t fg_enabled;     /* 1 when `accel gfxneuter 15 | 1 << 8` is ON. DEFAULT 0 */
    uint32_t fg_clause;      /* n48_fg_step's verdict; only 0 (N48_FG_PASS) may subtract anything */
    uint64_t fg_forgive;     /* the amount that verdict authorised */
    /* 0.0.390 — PER-CONSUMER POSITIVE PROVENANCE, AND THE SAME ARRANGEMENT X9-F HAS: the RULE is n48_cp_eval
     * above (pure, host-tested), and what arrives here is only its VERDICT, its COUNT and R5's U - so this fill cannot be
     * the place a provenance is invented. Every one of these is zero-initialised and every zero is the default build's
     * value on every fill: `cp_enabled` 0 (the switch is OFF at boot and OFF unless an operator set it), so
     * `consumer_enumerated` stays 0 and n48_dep_check runs 0.0.389's `source_neuters` rung.
     * THE THREE ARM-SCOPED WITNESS COUNTS ARE DELIBERATELY **NOT** IN v[]: v[] is monotonicity-checked, and a counter that
     * is re-based at each arm goes DOWN by design. Putting them there would latch N48_DEP_NONMONOTONE for the boot. */
    uint32_t cp_enabled;     /* 1 when `accel gfxneuter 28 | 1 << 8` is ON. DEFAULT 0 */
    uint32_t cp_enumerated;  /* 1 when n48_cp_eval ran over a POSITIVELY enumerated consumer for THIS frame */
    uint32_t cp_clause;      /* its verdict; 0 is N48_CP_OK */
    uint64_t cp_unproven;    /* the count that clause named (R1-R4) */
    uint64_t cp_unknown_ws;  /* n48_cp_unknown(): R5's U - neutered frames since the arm with no known write-set */
    uint64_t cp_stale;       /* the instrument: R1 surfaces a later neutered frame overwrote */
    uint32_t cp_scoped;      /* 1 when the ARM-SCOPED witness is in force, so the three counts below may be believed */
    uint64_t cp_wt_unresolved, cp_wt_over, cp_wt_trunc;
    /* 0.0.395 (R5-REDESIGN.md v2 "Switch and identity"): the new switch's answer, carried beside the old one. `r5_mode`
     * is 1 ONLY when `accel gfxneuter 30 | 1 << 8` is ON, and only then does n48_dep_check read `r5_blind`. With 30 OFF
     * both are 0 and the world is 0.0.394's, whatever the instruments did. */
    uint32_t r5_mode;
    uint64_t r5_blind;
    /* D4' (notes/design/D4-PRIME.md item 6) — THE SAME ACCOUNTING-IDENTITY GUARD cp_enabled/cp_enumerated ALREADY
     * GIVES THE TABLE-ABI CONSUMER, for the D4' fallback: `d4_enumerated` is 1 ONLY when n48_cp_eval_hz_d4 actually
     * judged THIS frame's consumer (gfxsrc_cprov_eval's own D4' branch), and it may be 1 only while `d4_enabled` (the
     * switch, `accel gfxneuter 40 | 1 << 8`) is also 1 - a D4'-built consumer with the switch OFF is a wiring defect,
     * not a fact about this frame, and refuses at N48_DEP_UNACCOUNTED exactly as an unaccountable cp_* combination
     * already does. Zero-initialised, and zero on every default build. */
    uint32_t d4_enabled;
    uint32_t d4_enumerated;
    uint64_t v[N48_DEPC_COUNT];
    /* build 0.0.523 (notes/design/RING-NEUTER-FORGIVE.md rev 2 item 3, switch 77) — THE RING-NEUTER FORGIVENESS, THE
     * X9-F ARRANGEMENT AGAIN: the RULE is gfx_rnforgive.h's drain (pure, host-tested by tests/gfx_rnforgive_test.cpp), and
     * what arrives here is only its switch, its verdict and its boot-monotone AMOUNT. SCALARS, APPENDED, NEVER IN v[]: v[]
     * is monotonicity-checked and `ring_neuters` is v[N48_DEPC_GN_IBS] itself, which is never decremented (gGn.ibs is
     * untouched, so the RING_NOP identity still adds up). All three zero-initialised; with 77 OFF they stay 0 and the fill
     * is 0.0.522's byte for byte. */
    uint32_t rn_enabled;     /* 1 when `accel gfxneuter 77 | 1 << 8` is ON. DEFAULT 0 */
    uint32_t rn_clause;      /* n48_rn_verdict's answer; only 0 may subtract anything */
    uint64_t rn_forgive;     /* the drain's boot-monotone forgiven IB total */
    /* build 0.0.541 item 6 (switch 99): the switch's mode (N48_DEP_WO_*) and THIS frame's evaluator reading no witness row
     * (gfxsrc_cprov_eval's gXp.lastRowsFree). SCALARS, APPENDED, never v[]. 0 and 0 while 99 is OFF: the world is 0.0.540's. */
    uint32_t wo_mode;
    uint32_t cp_rows_free;
} n48_dep_src;

/* The monotonicity memory: one per caller of the fill (the gate and the verb each keep their own). */
typedef struct {
    uint64_t last[N48_DEPC_COUNT];
    uint32_t have;
    uint32_t first_idx;      /* the first counter ever seen to decrease */
    uint64_t decreases;      /* fills that saw a decrease - LATCHED, never cleared */
} n48_dep_mono;

/* Identity bits for n48_dep_world.unaccounted. */
#define N48_DEP_ID_SOURCE    0x01u  /* calls == neutered + neutered-other + translated + disarmed + refused[1..5] + inflight */
#define N48_DEP_ID_RING_IBS  0x02u  /* IBs found == found armed + found native */
#define N48_DEP_ID_RING_NOP  0x04u  /* IBs found armed == NOPed + mismatched + not-an-IB + over the cap */
#define N48_DEP_ID_RING_FR   0x08u  /* frames with an IB walked armed == frames the ring NOP handled */
#define N48_DEP_ID_DRAIN     0x10u  /* drain IBs == changed + unchanged + refused + unreadable + over cap + write fail + verify */
#define N48_DEP_ID_SNAPSHOT  0x20u  /* the ring or drain counters could not be read consistently */
#define N48_DEP_ID_STALL     0x40u  /* stalls + unreadable samples > samples: the detector's own counts do not add up */
#define N48_DEP_ID_EXEMPT    0x80u  /* 0.0.359: more ring exemptions than source translations - a spared IB nobody committed */
#define N48_DEP_ID_QUEUE    0x100u  /* 0.0.367: queue starts == starts of a covered type + starts of any other type */
#define N48_DEP_ID_FORGIVE  0x200u  /* 0.0.372: an X9-F grant that is not backed by a PASS verdict, or does not fit */
#define N48_DEP_ID_CPROV    0x400u  /* 0.0.390: a consumer enumeration with the switch off, or a clause and its count that
                                     * disagree - a refusing clause with a zero count would otherwise commit */
#define N48_DEP_ID_RNFORGIVE 0x800u /* 0.0.523: a switch-77 grant with the switch off, a non-zero verdict, or more than fits */

/* 0.0.427 ( condition (1)) — HOW MANY IBs ONE COMMITTED FRAME MAY SPARE. The ring exemption is counted in IBs
 * (N48_DEPC_GN_EXEMPT is the fifth bucket of N48_DEPC_RING_IBS_ARMED, alongside the NOPed/mismatched/not-an-IB/over-cap
 * IBs), and one committed frame spares either all of its IBs or none (gfx_neuter.h B8). The submission shape allows at
 * most N48_GFXN_EX_MAX_IBS = 4, so the guard below is the tight bound "no more than 4 spared IBs per translated frame".
 * At one IB per frame - every boot through 0.0.426, and every OFF build - it is 0.0.426's `GN_EXEMPT > SRC_TRANSLATED`
 * exactly. The kext static_asserts this against gfx_neuter.h's N48_GFXN_EX_MAX_IBS so the two cannot drift. */
#define N48_DEP_EXEMPT_MAX_PER_FRAME 4u

static inline uint64_t n48_dep_identities(const n48_dep_src *s)
{
    const uint64_t *v = s->v;
    uint64_t bad = 0u;
    const uint64_t srcOut = v[N48_DEPC_SRC_NEUTERED] + v[N48_DEPC_SRC_NEUTERED_OTHER] + v[N48_DEPC_SRC_TRANSLATED] +
                            v[N48_DEPC_SRC_PASS_DISARMED] + v[N48_DEPC_SRC_REFUSED1] + v[N48_DEPC_SRC_REFUSED2] +
                            v[N48_DEPC_SRC_REFUSED3] + v[N48_DEPC_SRC_REFUSED4] + v[N48_DEPC_SRC_REFUSED5];
    if (v[N48_DEPC_SRC_CALLS] != srcOut + s->inflight) bad |= N48_DEP_ID_SOURCE;
    if (v[N48_DEPC_RING_IBS_FOUND] != v[N48_DEPC_RING_IBS_ARMED] + v[N48_DEPC_RING_IBS_NATIVE]) bad |= N48_DEP_ID_RING_IBS;
    /* 0.0.359: a spared committed IB is the fifth bucket of an armed IB. It is 0 unless COMMIT is armed, so with
     * COMMIT off this identity is 0.0.358's exactly (tests/gfx_dep_test.cpp, the frozen-fill property). */
    if (v[N48_DEPC_RING_IBS_ARMED] != v[N48_DEPC_GN_IBS] + v[N48_DEPC_GN_MISMATCH] + v[N48_DEPC_GN_NOT_IB] + v[N48_DEPC_GN_OVER] +
                                      v[N48_DEPC_GN_EXEMPT])
        bad |= N48_DEP_ID_RING_NOP;
    /* 0.0.427 ( condition (1)): the exemption is counted in IBs, so the bound is per-frame IBs, not frames
     * (N48_DEP_EXEMPT_MAX_PER_FRAME above). At nib 1 this is 0.0.426's guard exactly; a spared 2-IB frame (GN_EXEMPT 2,
     * SRC_TRANSLATED 1) is now clean instead of tripping the identity. */
    if (v[N48_DEPC_GN_EXEMPT] > v[N48_DEPC_SRC_TRANSLATED] * (uint64_t)N48_DEP_EXEMPT_MAX_PER_FRAME)
        bad |= N48_DEP_ID_EXEMPT;
    if (v[N48_DEPC_RING_FRAMES_NEUTERED] != v[N48_DEPC_GN_FRAMES]) bad |= N48_DEP_ID_RING_FR;
    if (v[N48_DEPC_DX_IBS] != v[N48_DEPC_DX_CHANGED] + v[N48_DEPC_DX_UNCHANGED] + v[N48_DEPC_DX_REFUSED] +
                             v[N48_DEPC_DX_UNREADABLE] + v[N48_DEPC_DX_OVERCAP] + v[N48_DEPC_DX_WRITE_FAIL] +
                             v[N48_DEPC_DX_VERIFY_BAD])
        bad |= N48_DEP_ID_DRAIN;
    if (s->snap_ok != 1u) bad |= N48_DEP_ID_SNAPSHOT;
    if (v[N48_DEPC_STALLS] + v[N48_DEPC_STALL_UNREADABLE] > v[N48_DEPC_STALL_SAMPLES]) bad |= N48_DEP_ID_STALL;
    /* 0.0.367: every engine-queue start lands in exactly one bucket. A start the tally forgot to classify has no bucket and
     * would otherwise disappear - the same way C4's refused[2] disappeared in v1. */
    if (v[N48_DEPC_Q_STARTS] != v[N48_DEPC_Q_KNOWN_TYPE] + v[N48_DEPC_Q_UNKNOWN_TYPE]) bad |= N48_DEP_ID_QUEUE;
    /* 0.0.372: X9-F's own identity. A grant must be backed by a PASS verdict, must come with the switch ON,
     * and must FIT INSIDE the class it forgives - a grant larger than the count would otherwise wrap the subtraction and
     * read as a clean world. Any of the three broken refuses at N48_DEP_UNACCOUNTED, which is tested BEFORE
     * N48_DEP_SOURCE_NEUTER, so a malformed grant can never produce a commit. */
    if (s->fg_forgive != 0u &&
        (s->fg_enabled != 1u || s->fg_clause != 0u ||
         s->fg_forgive > v[N48_DEPC_SRC_NEUTERED] + v[N48_DEPC_SRC_NEUTERED_OTHER]))
        bad |= N48_DEP_ID_FORGIVE;
    /* 0.0.390: the rule's own identity, and it is the guard that makes the replaced rung safe to write. An
     * enumeration without the switch, a scope without the switch, a clause out of range, a REFUSING clause whose count is
     * zero (which would fall straight through the replaced rung into a commit) or a PASSING clause with a count - any of
     * the five refuses at N48_DEP_UNACCOUNTED, which is tested BEFORE the rung, so a malformed rule can never commit. */
    if ((s->cp_enumerated == 1u && s->cp_enabled != 1u) ||
        (s->cp_scoped == 1u && s->cp_enabled != 1u) ||
        s->cp_clause >= (uint32_t)N48_CP_REASONS ||
        (s->cp_clause != 0u && s->cp_unproven == 0u) ||
        (s->cp_clause == 0u && s->cp_unproven != 0u) ||
        /* 0.0.395: the new switch's own two guards. An R5′ answer without the switch would let a rule nobody enabled
         * refuse (safe) but would also mean the wiring is wrong; an R5′ count while the switch is OFF would change the
         * decision through the world's r5_blind. Either refuses at UNACCOUNTED, which is tested BEFORE the rung. */
        (s->r5_mode == 1u && s->cp_enabled != 1u) ||
        (s->r5_mode == 0u && s->r5_blind != 0u) ||
        /* D4' (D4-PRIME.md item 6): a D4'-built consumer while the switch is OFF. */
        (s->d4_enumerated == 1u && s->d4_enabled != 1u))
        bad |= N48_DEP_ID_CPROV;
    /* build 0.0.523 (RING-NEUTER-FORGIVE.md rev 2 item 3): switch 77's own identity, X9-F's shape. A grant must come with
     * the switch ON and a verdict of 0, and must FIT INSIDE the raw ring-NOP count - a larger one would wrap the subtraction
     * and read clean. Any of the three refuses at N48_DEP_UNACCOUNTED, tested BEFORE N48_DEP_RING_NEUTER. */
    if (s->rn_forgive != 0u &&
        (s->rn_enabled != 1u || s->rn_clause != 0u || s->rn_forgive > v[N48_DEPC_GN_IBS]))
        bad |= N48_DEP_ID_RNFORGIVE;
    return bad;
}

/* THE FILL: raw counters and install states -> the world n48_dep_check judges. Pure. `m` is the caller's monotonicity memory;
 * a null `m` means monotonicity was not checked, which refuses. */
static inline void n48_dep_fill(const n48_dep_src *s, n48_dep_mono *m, n48_dep_world *w)
{
    if (!w) return;
    { uint8_t *p = (uint8_t *)w; for (uint32_t i = 0; i < (uint32_t)sizeof(*w); i++) p[i] = 0u; }
    if (!s || s->gathered != 1u) return;                      /* sampled stays 0: NOT_SAMPLED */
    const uint64_t *v = s->v;
    w->sampled = 1u;

    uint32_t obs = 0u;
    if (s->src_install == 1u) obs |= N48_DEP_OBS_SRC;
    if ((s->ring_state == 1u || s->ring_state == 2u) && s->ring_hooked == 1u) obs |= N48_DEP_OBS_RING;
    if (s->sdma_state == 2u) obs |= N48_DEP_OBS_SDMA;
    if (s->stall_armed == 1u && s->stall_read_ok == 1u) obs |= N48_DEP_OBS_STALL;
    /* 0.0.368: EARLY's rung, plus RULE E1 as its ONE widening. The walk must still have run, not wrapped and not
     * stopped - E1 does not relax any of those. Beyond that, EARLY is live when the ring held NO IB (0.0.358's rule, byte for
     * byte) or when it held EXACTLY ONE and E1 PASSED on it. Both halves of the E1 arm are required, so removing either one
     * is caught: `pre_ibs == 1u` is not implied by `pre_e1` here, and `pre_e1` is not implied by anything. */
    if (s->pre_walked == 1u && !s->pre_wrapped && !s->pre_stopped &&
        (s->pre_ibs == 0u || (s->pre_ibs == 1u && s->pre_e1 == 1u))) obs |= N48_DEP_OBS_EARLY;
    /* 0.0.367: C13 is live only when the TTL queue slot is on Apple's path AND the tally that classifies its types is intact.
     * C16 is live only when THIS gather read the fault register - a fault status nobody read must never read as "no fault". */
    if (s->q_hook_live == 1u && s->q_tally_ok == 1u) obs |= N48_DEP_OBS_QUEUE;
    if (s->fault_read_ok == 1u) obs |= N48_DEP_OBS_FAULT;
    w->observers = obs;

    if (!m) {
        w->nonmonotone = 1u;
    } else {
        uint32_t down = 0u, first = 0u;
        if (m->have)
            for (uint32_t i = 0; i < N48_DEPC_COUNT; i++)
                if (v[i] < m->last[i]) { if (!down) first = i; down = 1u; }
        if (down) { if (!m->decreases) m->first_idx = first; m->decreases++; }
        for (uint32_t i = 0; i < N48_DEPC_COUNT; i++) m->last[i] = v[i];
        m->have = 1u;
        w->nonmonotone = m->decreases;
    }
    w->unaccounted = n48_dep_identities(s);

    /* v1's five, same sources; the source neuter now also counts the other-VMID submissions it NOPs. */
    /* 0.0.372 — X9-F, AND IT IS THE ONLY LINE IN THIS HEADER A FORGIVENESS MAY TOUCH. The subtraction needs
     * ALL FOUR of: the switch positively ON, a verdict of exactly N48_FG_PASS, a non-zero amount, and an amount that fits
     * inside the class. Anything else leaves `source_neuters` exactly as 0.0.371 computed it - and the malformed cases
     * have already refused at N48_DEP_UNACCOUNTED above, so this is the second of two guards, not the first. `forgiven`
     * is recorded so the log can print what was subtracted; nothing reads it. */
    {
        uint64_t sn = v[N48_DEPC_SRC_NEUTERED] + v[N48_DEPC_SRC_NEUTERED_OTHER];
        if (s->fg_enabled == 1u && s->fg_clause == 0u && s->fg_forgive != 0u && s->fg_forgive <= sn) {
            w->forgiven = s->fg_forgive;
            sn -= s->fg_forgive;
        }
        w->source_neuters = sn;
    }
    w->ring_neuters = v[N48_DEPC_GN_IBS];
    w->neuter_other = v[N48_DEPC_GN_WALK_STOP] + v[N48_DEPC_GN_RACED];
    /* 0.0.390 ( part 5 (iii)): WHICH WITNESS THESE TWO RUNGS READ. Off - the default, every fill of every
     * default build - the LIFETIME witness, byte for byte as 0.0.389 read it. On, the ARM-SCOPED one: what is stale is
     * what was dropped SINCE THE ARM, and arm13's lifetime witness overflowed 99 times (arm18's 28) which refuses at
     * `witness_over` one rung below the one the rule replaces. Both counts still refuse at ONE. */
    {
        const uint32_t scoped = (s->cp_enabled == 1u && s->cp_scoped == 1u) ? 1u : 0u;
        w->targets_unknown = scoped ? s->cp_wt_unresolved : v[N48_DEPC_WT_UNRESOLVED];
        w->witness_over = scoped ? (s->cp_wt_over + s->cp_wt_trunc) : (v[N48_DEPC_WT_OVER] + v[N48_DEPC_WT_TRUNC]);
    }
    /* 0.0.390: the rule's own three, and the instrument. `consumer_enumerated` needs BOTH the switch and a positive
     * enumeration; without it the three below stay 0 and n48_dep_check never reaches the replaced rung. */
    w->consumer_enumerated = (s->cp_enabled == 1u && s->cp_enumerated == 1u) ? 1u : 0u;
    if (w->consumer_enumerated) {
        w->neuter_unknown_writeset = s->cp_unknown_ws;
        w->consumer_inputs_unproven = s->cp_unproven;
        w->stale_overwrites = s->cp_stale;
        /* 0.0.395: the new switch's answer. With r5_mode 0 these two are 0 and the check above is 0.0.394's. */
        w->r5_mode = s->r5_mode;
        w->r5_blind = s->r5_blind;
    }
    /* build 0.0.523 (notes/design/RING-NEUTER-FORGIVE.md rev 2 item 3, switch 77) — THE RING-NEUTER FORGIVENESS, AFTER the
     * consumer block above set `consumer_enumerated` and `r5_mode` (a subtraction placed before it would read 0 there and never
     * fire: fail-closed, and the frame-21 test catches it). SIX guards, X9-F's four plus two: the switch positively ON, a
     * verdict of exactly 0, a non-zero amount, an amount that fits inside the raw count, AND the consumer path live (only the
     * consumer path's R1/R3 rules cover a NOPed committed frame's writes: its un-feed and its R5′ note) AND R5′ in force (the
     * grant's note went into R5′'s hazard set, which only r5_mode 1 asks). Anything else leaves `ring_neuters` exactly as
     * 0.0.522 computed it, and the malformed cases have already refused at N48_DEP_UNACCOUNTED above. `ring_neuters` is the
     * ONLY world field this touches; v[N48_DEPC_GN_IBS] (gGn.ibs) is never decremented. */
    if (s->rn_enabled == 1u && s->rn_clause == 0u && s->rn_forgive != 0u && s->rn_forgive <= w->ring_neuters &&
        w->consumer_enumerated == 1u && w->r5_mode == 1u) {
        w->rn_forgiven = s->rn_forgive;
        w->ring_neuters -= s->rn_forgive;
    }
    /* build 0.0.541 item 6 (switch 99): copied, AFTER the consumer block (cp_rows_free is believed only with the consumer
     * rule answering). Any other mode value is OFF. */
    w->wo_mode = (s->wo_mode == N48_DEP_WO_ON || s->wo_mode == N48_DEP_WO_SHADOW) ? s->wo_mode : N48_DEP_WO_OFF;
    w->cp_rows_free = (w->consumer_enumerated == 1u && s->cp_rows_free == 1u) ? 1u : 0u;
    /* v2's six. */
    w->gfx_escaped = v[N48_DEPC_SRC_REFUSED1] + v[N48_DEPC_SRC_REFUSED2] + v[N48_DEPC_SRC_REFUSED3] +
                     v[N48_DEPC_SRC_REFUSED4] + v[N48_DEPC_SRC_REFUSED5] +
                     v[N48_DEPC_GN_MISMATCH] + v[N48_DEPC_GN_NOT_IB] + v[N48_DEPC_GN_OVER] +
                     v[N48_DEPC_RING_WALK_STOPS] + v[N48_DEPC_RING_UNUSABLE] + v[N48_DEPC_RING_PUBLISHED_EARLY];
    w->gfx_native = v[N48_DEPC_SRC_PASS_DISARMED] + v[N48_DEPC_RING_IBS_NATIVE];
    w->sdma_untranslated = v[N48_DEPC_DX_REFUSED] + v[N48_DEPC_DX_UNREADABLE] + v[N48_DEPC_DX_OVERCAP] +
                           v[N48_DEPC_DX_WRITE_FAIL] + v[N48_DEPC_DX_VERIFY_BAD] + v[N48_DEPC_DX_BEYOND_CAP] +
                           v[N48_DEPC_DX_OVERRUNS];
    w->sdma_waits_removed = v[N48_DEPC_DX_POLLS_NEUTERED];
    w->ring_resets = v[N48_DEPC_RING_BACKWARDS] + v[N48_DEPC_DX_BACKWARDS];
    w->engine_stalls = v[N48_DEPC_STALLS] + v[N48_DEPC_STALL_UNREADABLE];
    /* 0.0.367's two. C13 gathers every route by which work could reach an engine we do not watch; C16 the latched fault. */
    w->compute_queues = v[N48_DEPC_Q_UNKNOWN_TYPE] + v[N48_DEPC_Q_MES_REQUESTS] + v[N48_DEPC_Q_TTL_SUBMITS] +
                        v[N48_DEPC_Q_KIQ_COMPUTE] + v[N48_DEPC_Q_KIQ_UNKNOWN];
    w->vm_faults = v[N48_DEPC_VM_FAULTS];
}

/* ---- THE STALL DETECTOR (C12's onset, notes/M4-X9-COMPLETE.md `engine_stalls`) ----------------------------------
 * Sampled under the render drain's lock at every GFX writeTail entry and at every gather. The CP is PARKED while
 * CP_RB0_RPTR != CP_RB0_WPTR; a parked position (one RPTR value) that has not moved for `threshold_us` is ONE stall, counted
 * once however long it stays. WPTR moving on while RPTR stays put is still parked - that is exactly C4's signature (RPTR
 * 0x62d, WPTR 0x780 at k15). A read of all-ones on either pointer is counted as unreadable, which also refuses. */
typedef struct {
    uint32_t armed;          /* set when the ring walk arms; the STALL observer needs it */
    uint32_t parked;         /* the last readable sample had RPTR != WPTR (modulo the ring, 0.0.359) */
    uint32_t rptr;           /* the parked position */
    uint32_t counted;        /* that position has been counted */
    uint64_t since_us;       /* when it was first seen */
    uint64_t samples, stalls, unreadable;
    uint32_t ring;           /* 0.0.359: the CP ring in dwords (n48_cp_ring_dwords); 0 = unknown -> raw compare */
} n48_stall;

/* ---- 0.0.359: THE RING POINTERS, COMPARED THE WAY THE CP COUNTS THEM ----------------------------------------
 * hp1 (0.0.358) printed `0xe380 / 0x8e380 (CP BEHIND)` and `0x5e00 / 0xe5e00 (CP BEHIND)` with the CP caught up: CP_RB0_RPTR
 * reads the dword offset INTO the ring (it wraps at the ring size), CP_RB0_WPTR reads Apple's unwrapped dword count (hp1's
 * `gfx-pub: writeTail EXIT` lines: WPTR 0x100/0x180 = Apple's ring+0x58 exactly; 0x8e380 and 0xe5e00 exceed the 0x20000-dword
 * ring). Upstream compares the two only after masking both with the ring's dword mask (ref/linux-amdgpu/amdgpu_ring.c:627-628,
 * `& ring->buf_mask`, buf_mask = ring_size / 4 - 1 at :347), and its 64-bit-pointer rings never mask the wptr itself (:348-349).
 * So "caught up" is equality MODULO THE RING. The raw compare that 0.0.358 made in the CLI, the kext's control line and this
 * header's stall detector reads BEHIND forever once WPTR passes the ring size - a lying string, and in n48_stall_note a
 * latent false `engine-stall` refusal (tests/gfx_dep_test.cpp plants it back). A ring of 0 or not a power of two is not
 * trusted: the raw compare is kept, which can only read BEHIND more often (fail-closed for the detector). */
static inline uint32_t n48_ring_caught_up(uint32_t rptr, uint32_t wptr, uint32_t ring)
{
    if (ring == 0u || (ring & (ring - 1u)) != 0u) return rptr == wptr ? 1u : 0u;
    return ((rptr ^ wptr) & (ring - 1u)) == 0u ? 1u : 0u;
}

/* The CP's ring in dwords from CP_RB0_CNTL.RB_BUFSZ ([5:0], gc_12_0_0_sh_mask.h CP_RB0_CNTL__RB_BUFSZ_MASK 0x3f), which upstream
 * programs as order_base_2(ring_size_bytes / 8) (ref/linux-amdgpu/gfx_v12_0.c:2767-2768): dwords = 2 << RB_BUFSZ (hp1: 0x00f00e90
 * -> 16 -> 0x20000). Trusted only when it EQUALS the size Apple's ring object holds (`apple_dw`, ring+0x30): two independent
 * readings of one ring. Anything else returns 0, and 0 means "unknown - raw compare". */
static inline uint32_t n48_cp_ring_dwords(uint32_t cp_rb0_cntl, uint32_t apple_dw)
{
    const uint32_t bufsz = cp_rb0_cntl & 0x3Fu;
    if (bufsz > 30u) return 0u;
    const uint32_t dw = 2u << bufsz;
    return dw == apple_dw ? dw : 0u;
}

/* "The CP was told about dwords past `mark` (and not past Apple's own `wptr`)", in the unwrapped dword space CP_RB0_WPTR reads
 * in. Both of 0.0.358's uses - the writeTail publish instrument (`gfx-pub`, X9's published-before-the-hook) and the ring
 * neuter's race witness (gGn.raced, X9's neuter-other) - compared that UNWRAPPED register with a position reduced MODULO the
 * ring, so after the first wrap neither could ever fire: two blind X9 counters. Here in 32-bit serial arithmetic (the gap is
 * one frame, far below 2^31); identical to the old expressions while nothing has wrapped (tests/gfx_dep_test.cpp). */
static inline uint32_t n48_cp_told_past(uint32_t cp_wptr, uint64_t mark, uint64_t wptr)
{
    if (cp_wptr == 0xFFFFFFFFu || wptr <= mark) return 0u;
    const int32_t ahead  = (int32_t)(cp_wptr - (uint32_t)mark);
    const int32_t within = (int32_t)((uint32_t)wptr - cp_wptr);
    return (ahead > 0 && within >= 0) ? 1u : 0u;
}

static inline void n48_stall_note(n48_stall *s, uint32_t rptr, uint32_t wptr, uint64_t now_us, uint64_t threshold_us)
{
    if (!s) return;
    s->samples++;
    if (rptr == 0xFFFFFFFFu || wptr == 0xFFFFFFFFu) { s->unreadable++; return; }
    if (n48_ring_caught_up(rptr, wptr, s->ring)) { s->parked = 0u; s->counted = 0u; return; }
    if (!s->parked || rptr != s->rptr) { s->parked = 1u; s->rptr = rptr; s->since_us = now_us; s->counted = 0u; return; }
    if (!s->counted && now_us >= s->since_us && now_us - s->since_us >= threshold_us) { s->stalls++; s->counted = 1u; }
}

#endif
