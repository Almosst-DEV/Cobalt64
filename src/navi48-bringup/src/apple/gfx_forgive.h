// gfx_forgive.h — X9-F: THE ONE PRE-BASELINE DROPPED-WRITE FORGIVENESS X9 MAY GRANT (0.0.372).
// Design and the whole evaluation of the alternatives: notes/M4-PREARM-FORGIVENESS.md. Pure C, host-tested by
// tests/gfx_forgive_test.cpp; the kext compiles the SAME header. DEFAULT OFF (`accel gfxneuter 15 | 1 << 8`).
//
// THE HAZARD THIS TOUCHES, AND WHY IT DESERVES ITS OWN HEADER. X9 (gfx_dep.h) refuses a commit while ANY dropped write is
// outstanding, because a frame we COMMIT may sample a surface a frame we DROPPED should have written — the GPU then reads
// bytes nobody wrote and draws a silently wrong picture, with no counter moving. That is the worst failure class this
// project has, and this header WEAKENS that check. It is therefore built the way E1 (gfx_e1.h) is built: every input is
// POSITIVELY set by the caller, a zero-initialised request refuses, every clause names itself, and the one thing it may
// ever do is forgive the `source-neuter` count accumulated BEFORE a latched baseline. It widens nothing else.
//
// WHAT IS PROVABLE HERE AND WHAT IS NOT — the two sentences from gfx_dep.h that bound every possible rule:
//   R  WE CANNOT ENUMERATE WHAT A FRAME READS. So "the committed frame samples nothing we dropped" is a NEGATIVE this
//      codebase cannot prove, and an incomplete read-set fails OPEN in exactly the hazard's direction.
//   W  WE CANNOT ENUMERATE WHAT A DROPPED FRAME WOULD HAVE WRITTEN. The decide path reads CB_COLORn_BASE only: no depth,
//      no DMA_DATA destination, and nothing a compute dispatch writes — which is what Apple's own BufferClear_CS IS.
// So the only sound shape is "those stale surfaces are UNREACHABLE", never "they are not sampled". Every clause below
// serves that shape. An arm-time or bind-time baseline ALONE does not (notes/M4-PREARM-FORGIVENESS.md(a),(b)), and
// neither does "the context is dead" alone, because a shared IOSurface outlives the context that drew into it and a
// compositor sampling exactly those surfaces is what a compositor is ((c), failure mode 1).
//
// THE ELEVEN CLAUSES, in one sentence each. The conjunction is the rule; no clause is sufficient alone.
//   1  THE SWITCH IS ON. Default OFF, so nothing changes unless an operator asked for it.
//   2  A BASELINE WAS LATCHED AT A BIND of the committing context, and that context is BOUND NOW with the same `seq`.
//      The bind is the last instant before the committing context can submit anything, and it is a logged event.
//   3  THE BINDING HAS NOT MOVED since the baseline: binds/rebinds/unbinds/gone all equal their baseline values.
//   4  THE COMMITTING CONTEXT HAS NEVER HAD A FRAME DROPPED — `base_own` is 0 and it appears in no census row. The
//      frames whose surfaces this frame is likeliest to sample are exactly its own, and they must all have RUN.
//   5  ZERO DROPS OF ANY CLASS SINCE THE BASELINE (`now_total == base_total`). This is what closes the recycled-page
//      case: a clear of a re-allocated page either ran, or has not been submitted, and no clear has been dropped.
//   6  EVERY PRE-BASELINE DROP IS ATTRIBUTED to exactly one owning VM context, under a COMPLETE context table and a
//      census that did not overflow. A drop with no owner cannot be forgiven by an owner-based rule.
//   7  THE ACCOUNTING IDENTITY: the census rows plus the unattributed count equal the baseline total exactly. An
//      outcome with no bucket refuses (gfx_dep.h's own rule 3), it does not disappear.
//   8  EVERY OWNING CONTEXT IS DEAD NOW, on positive evidence from our own hooks: released (releaseVMContext) or its
//      root freed (unmapVA). "Nobody looked" is not "it is gone".
//   9  EVERY OWNING CONTEXT WAS RELEASED BEFORE THE COMMITTING CONTEXT WAS CREATED (`creates_at_release <
//      base_ctx_seq`, from VmCtxObs::createsAtRelease). This is the clause that repairs clause 8's shared-IOSurface
//      hole BY CONSTRUCTION rather than by measurement: two contexts that never coexisted never held one mapping at
//      one time. Its residue is SUSPECTED S1 in the memo and is named there.
//  10  THE OTHER CUMULATIVE CLASSES ARE 0 (ring-neuter, walk-stopped/raced, unresolved target, witness overflow), read
//      NOW, which for monotone counters implies they were 0 at the baseline too. This rule reasons about ONE class.
//  11  THE MONOTONE LATCH (n48_fg_step): a grant that is ever refused afterwards is WITHDRAWN for the rest of the
//      boot and can never be re-granted, and the granted amount may never change. Turning the switch back off
//      withdraws too — going back is always allowed, coming back is not.
//
// DIRECTION OF ERROR. Every clause is a conjunct. A counter that cannot be read refuses; an input the kext forgets to
// fill refuses; a census that lost a row refuses at the identity. The most this can do is subtract a number the caller
// positively justified from ONE class; gfx_dep.h's fill refuses the subtraction outright unless the verdict is PASS and
// the amount fits inside the class (N48_DEP_ID_FORGIVE).
//
// 0.0.381 — THIS FILE NOW HOLDS A SECOND, SEPARATELY-NAMED GRANT SOURCE: THE RUNNING BUDGET ("EXIT C"),
// `accel gfxneuter 19 | N << 8`, N = 0 AT BOOT AND BY DEFAULT. It is at the END of this file, behind its own clause
// enum (N48_RB_*), its own rule (n48_rb_eval) and a copy of clause 11's latch shared with X9-F. IT IS WEAKER THAN
// EVERYTHING ABOVE AND IT SAYS SO IN ITS OWN HEADER BLOCK: it forgives POST-baseline drops, including the committing
// compositor's OWN, it has neither clause 5 nor clause 9, and what stands in their place is a BOUND, not a proof.
// Read that block before anything else in this file is taken to apply to it. The two are disjoint by construction -
// X9-F requires `now_total == base_total`, the running budget exists only for `now_total > base_total` - and the kext
// breaks the tie explicitly anyway, X9-F first, so that a change to either can never make both fill the socket.
// AND IT IS NOT "ONE FORGIVEN FRAME PER BOOT": its latch freezes the BOUND, not the amount, for a reason set out at
// n48_rb_step, so while the grant stands EVERY judged frame at or under N clears the rung. What limits how many of them
// COMMIT is the COMMIT arm's own frame budget in gfx_commit.h, which is 1 by default. The two are ONE unit.
#ifndef N48_GFX_FORGIVE_H
#define N48_GFX_FORGIVE_H

#include <stdint.h>

/* Contexts the pre-baseline census may name. More than this REFUSES (N48_FG_CENSUS_INCOMPLETE): a census that did not
 * see every owner is not a census. hp9 had exactly ONE owner for all 583 pre-arm drops. */
#define N48_FG_ROWS 8u

/* Positive evidence that a context is gone, as OUR OWN hooks observed it. At least one bit is required; 0 means nobody
 * observed a teardown, which is not the same as "it is still there" and refuses either way. */
#define N48_FG_DEAD_RELEASED   0x1u   /* releaseVMContext ran for it (VmCtxObs::state == 2) */
#define N48_FG_DEAD_ROOT_FREED 0x2u   /* its root was freed at unmapVA (the record's root reads 0 through our own map) */

/* The verdict. 0 is the ONLY value that forgives anything. Every other value names the clause that refused, in the order
 * the clauses are tested. APPENDED, never inserted: the indices are printed. */
enum {
    N48_FG_PASS = 0,
    N48_FG_NOT_GATHERED,      /* the request was never filled from live state - a zero request is NOT a clean world */
    N48_FG_DISABLED,          /* the switch is OFF (the default): no forgiveness is even considered */
    N48_FG_WITHDRAWN,         /* clause 11: a grant was withdrawn earlier this boot and can never be re-granted */
    N48_FG_NO_BASELINE,       /* clause 2: no baseline was latched */
    N48_FG_BASE_CTX,          /* clause 2: the baseline names no context, or not the one bound now */
    N48_FG_NOT_BOUND,         /* clause 2: ws_ident is not BOUND right now */
    N48_FG_BINDING_MOVED,     /* clause 3: binds/rebinds/unbinds/gone are not the baseline's */
    N48_FG_NOTHING_TO_FORGIVE,/* the baseline total is 0: the world is already clean and no grant is needed */
    N48_FG_POST_BASELINE,     /* clause 5: the count has moved since the baseline (or moved DOWN, which is worse) */
    N48_FG_OWN_DROPS,         /* clause 4: the committing context has had a frame dropped */
    N48_FG_OTHER_CLASSES,     /* clause 10: another cumulative class was already dirty at the baseline */
    N48_FG_CENSUS_INCOMPLETE, /* clause 6: the context table overflowed, the census overflowed, or a drop went unrecorded */
    N48_FG_UNATTRIBUTED,      /* clause 6: a pre-baseline drop had no unique owning context */
    N48_FG_ACCOUNT,           /* clause 7: the census does not add up to the baseline total */
    N48_FG_ROW_EMPTY,         /* clause 6: a census row names no context or no drops - an unfilled row is not evidence */
    N48_FG_CTX_ALIVE,         /* clause 8: an owning context is not observed dead */
    N48_FG_CTX_NO_EVIDENCE,   /* clause 8: it is MARKED dead with no positive evidence, or with no release ordering */
    N48_FG_COEXISTED,         /* clause 9: an owning context was still live when the committing context was created */
    N48_FG_AMOUNT_MOVED,      /* clause 11: a later grant is not the amount first granted - the baseline moved */
    N48_FG_CLAUSES
};

static inline const char *n48_fg_clause_name(uint32_t c)
{
    static const char *const n[N48_FG_CLAUSES] = {
        "PASS", "not-gathered", "switch-OFF", "WITHDRAWN", "no-baseline", "baseline-context", "not-bound",
        "binding-moved", "nothing-to-forgive", "dropped-since-the-baseline", "the-committing-context's-own-drops",
        "another-class-already-dirty", "census-incomplete", "unattributed-drop", "accounting-identity", "empty-census-row",
        "an-owning-context-is-ALIVE", "no-teardown-evidence", "the-contexts-COEXISTED", "granted-amount-moved" };
    return c < N48_FG_CLAUSES ? n[c] : "?";
}

/* One owning context of the PRE-BASELINE drops, as the census froze it and as the kext reads its lifetime NOW. */
typedef struct {
    uint32_t seq;                /* its createVMContext number - part of the identity, never 0 for a filled row */
    uint32_t dead;               /* 1 when our records say it is gone (POSITIVELY set; 0 refuses at CTX_ALIVE) */
    uint32_t dead_evidence;      /* N48_FG_DEAD_* bits; 0 refuses at CTX_NO_EVIDENCE */
    uint32_t creates_at_release; /* VmCtxObs::createsAtRelease - gVmCtxCreates at the instant of its release */
    uint64_t drops;              /* pre-baseline source-neutered submissions attributed to it */
} n48_fg_row;

/* Everything X9-F judges. Zero-initialised; `gathered` must be POSITIVELY set or the rule refuses NOT_GATHERED. */
typedef struct {
    uint32_t gathered;
    uint32_t enabled;            /* the switch (gXdForgive). 0 is the default and the boot value */

    /* --- the baseline, latched ONCE at the committing context's BIND and never recomputed --------------------- */
    uint32_t base_valid;
    uint32_t base_ctx_seq;       /* the create # of the context the baseline was latched FOR */
    uint64_t base_total;         /* neuteredSubs + neuteredOtherSubs AT that instant - what may be forgiven */
    uint64_t base_own;           /* neuteredSubs AT that instant - MUST be 0 (clause 4) */
    uint64_t base_binds, base_rebinds, base_unbinds, base_gone;

    /* --- the world NOW ---------------------------------------------------------------------------------------- */
    uint64_t now_total;          /* the same sum now - clause 5 requires it to be UNCHANGED */
    /* Clause 10, read NOW rather than at the baseline, which is STRICTLY STRONGER and needs no unlocked read at the
     * latch: every one of these counters is monotone over the boot (gfx_dep.h's N48_DEP_NONMONOTONE latches a decrease
     * for good), so 0 now implies 0 then. ring-neuter + walk-stopped/raced + unresolved targets + witness overflow. */
    uint64_t other_classes;
    uint32_t ws_bound;           /* 1 when ws_ident's state is N48_WS_BOUND right now */
    uint32_t ws_seq;             /* the bound context's create # right now */
    uint64_t binds, rebinds, unbinds, gone;

    /* --- the FROZEN pre-baseline census ---------------------------------------------------------------------- */
    uint32_t census_complete;    /* 1 when every counted drop was recorded under a complete WindowServer snapshot */
    uint32_t ctx_table_complete; /* 1 when gVmCtxOverflow == 0: no context exists that we hold no record of */
    uint32_t rows;               /* filled rows of row[] */
    uint32_t census_over;        /* owning contexts that did not fit N48_FG_ROWS - MUST be 0 */
    uint64_t attributed;         /* drops the census attributed, as the kext counted them */
    uint64_t unattributed;       /* drops with 0 or >1 owning records - MUST be 0 */
    n48_fg_row row[N48_FG_ROWS];
} n48_fg_in;

/* THE RULE. Pure. Returns N48_FG_PASS and sets *forgive to the amount, or the first clause that refused with *forgive 0.
 * Note that N48_FG_DISABLED and N48_FG_NOTHING_TO_FORGIVE are refusals in the sense that they grant nothing; neither is a
 * defect, and n48_fg_step does not withdraw a standing grant on NOTHING_TO_FORGIVE alone (see there). */
static inline uint32_t n48_fg_eval(const n48_fg_in *f, uint64_t *forgive)
{
    if (forgive) *forgive = 0u;
    if (!f || f->gathered != 1u) return N48_FG_NOT_GATHERED;
    if (f->enabled != 1u) return N48_FG_DISABLED;

    /* Clause 2: the baseline, and that it belongs to the context bound RIGHT NOW. A baseline for some other context
     * would forgive a prefix that the committing context's own frames may well be inside. */
    if (f->base_valid != 1u) return N48_FG_NO_BASELINE;
    if (f->base_ctx_seq == 0u) return N48_FG_BASE_CTX;
    if (f->ws_bound != 1u) return N48_FG_NOT_BOUND;
    if (f->ws_seq != f->base_ctx_seq) return N48_FG_BASE_CTX;

    /* Clause 3: the binding has not moved. The baseline is latched AT the bind, so every counter must still read what
     * it read then - including `binds`, which already includes that bind. */
    if (f->binds != f->base_binds || f->rebinds != f->base_rebinds || f->unbinds != f->base_unbinds ||
        f->gone != f->base_gone)
        return N48_FG_BINDING_MOVED;

    if (f->base_total == 0u) return N48_FG_NOTHING_TO_FORGIVE;   /* already clean: no grant, and none needed */

    /* Clause 5: nothing has been dropped since the baseline. Equality, not `<=`: a count that moved UP is a drop this
     * rule does not reason about, and a count that moved DOWN is a reset that erased what it had counted. */
    if (f->now_total != f->base_total) return N48_FG_POST_BASELINE;

    /* Clause 4: the committing context's own frames must ALL have run. */
    if (f->base_own != 0u) return N48_FG_OWN_DROPS;

    /* Clause 10: exactly one class may be forgiven, and the others must be clean - now, hence also at the baseline. */
    if (f->other_classes != 0u) return N48_FG_OTHER_CLASSES;

    /* Clause 6: the census must be able to name every owner. */
    if (f->census_complete != 1u || f->ctx_table_complete != 1u) return N48_FG_CENSUS_INCOMPLETE;
    if (f->census_over != 0u || f->rows == 0u || f->rows > N48_FG_ROWS) return N48_FG_CENSUS_INCOMPLETE;
    if (f->unattributed != 0u) return N48_FG_UNATTRIBUTED;

    /* Clause 7: the accounting identity, both halves. The rows must sum to what the kext says it attributed, AND that
     * plus the unattributed count must be the baseline total. Either half broken means an outcome has no bucket. */
    {
        uint64_t sum = 0u;
        for (uint32_t i = 0; i < f->rows && i < N48_FG_ROWS; i++) {
            const n48_fg_row *r = &f->row[i];
            if (r->seq == 0u || r->drops == 0u) return N48_FG_ROW_EMPTY;
            sum += r->drops;
        }
        if (sum != f->attributed) return N48_FG_ACCOUNT;
        if (f->attributed + f->unattributed != f->base_total) return N48_FG_ACCOUNT;
    }

    /* Clauses 4 (again, per row), 8 and 9, in the order that names the most useful thing first. */
    for (uint32_t i = 0; i < f->rows && i < N48_FG_ROWS; i++) {
        const n48_fg_row *r = &f->row[i];
        if (r->seq == f->base_ctx_seq) return N48_FG_OWN_DROPS;        /* the committing context owns a drop */
        if (r->dead != 1u) return N48_FG_CTX_ALIVE;
        if (r->dead_evidence == 0u || r->creates_at_release == 0u) return N48_FG_CTX_NO_EVIDENCE;
        /* Clause 9: released BEFORE the committing context was created. `creates_at_release` is the number of creates
         * that had happened at the release, so `< base_ctx_seq` means create #base_ctx_seq did not exist yet. */
        if (r->creates_at_release >= f->base_ctx_seq) return N48_FG_COEXISTED;
    }

    if (forgive) *forgive = f->base_total;
    return N48_FG_PASS;
}

/* ---- CLAUSE 11: THE MONOTONE LATCH ----------------------------------------------------------------------------------
 * A forgiveness is granted at most once per boot and can never come back after it is withdrawn. `withdrawn` is never
 * cleared, by anything. A standing grant is withdrawn by ANY later non-PASS verdict, without exception, and the verdict
 * that did it is kept in `first_clause`:
 *   - any clause refusing (an owner came back to life, the census stopped adding up, the binding moved, ...);
 *   - a later evaluation that would grant a DIFFERENT amount, which means the baseline moved under us;
 *   - NOTHING_TO_FORGIVE, which after a non-zero grant can only mean the baseline was re-latched - also a move;
 *   - the switch being turned OFF. Going back to refusing is always allowed; coming back is not.
 * `forgive` is the amount this call authorises: the first granted amount while the grant stands, 0 otherwise. */
typedef struct {
    uint32_t granted;      /* 1 once a grant has been made and not withdrawn */
    uint32_t withdrawn;    /* 1 once withdrawn - NEVER cleared */
    uint32_t clause;       /* the last verdict, for the log */
    uint32_t first_clause; /* the clause that withdrew a standing grant (0 while none has) */
    uint64_t amount;       /* the amount first granted */
    uint64_t grants, refusals, withdrawals, calls;
} n48_fg_latch;

/* 0.0.381: the latch is now SHARED by two grant sources (X9-F below, and the running budget at the end of
 * this file), because clause 11 is the property that must hold for EVERY grant this kext can make and duplicating it
 * would mean two copies to keep right. It is split in two halves so neither source can skip one:
 *   begin  counts the call and answers 1 when the latch is already withdrawn - the caller must then NOT evaluate;
 *   apply  performs the whole transition from a clause the caller has computed.
 * The clause numbering differs between the two rules, so the three clauses `apply` itself needs are passed in. PASS is 0
 * in BOTH enums by construction, and the typedef below fails to compile if that ever stops being true. */
static inline uint32_t n48_fg_latch_begin(n48_fg_latch *l, uint32_t withdrawn_clause)
{
    if (!l) return 1u;
    l->calls++;
    if (l->withdrawn) { l->clause = withdrawn_clause; l->refusals++; return 1u; }
    return 0u;
}

static inline uint32_t n48_fg_latch_apply(n48_fg_latch *l, uint32_t c, uint64_t want,
                                          uint32_t withdrawn_clause, uint32_t moved_clause, uint64_t *forgive)
{
    if (!l) return withdrawn_clause;
    if (c == 0u) {                                        /* PASS, in either rule's numbering */
        if (l->granted && want != l->amount) {            /* the baseline moved under a standing grant */
            l->withdrawn = 1u; l->granted = 0u; l->withdrawals++;
            l->clause = moved_clause; l->first_clause = moved_clause;
            return moved_clause;
        }
        if (!l->granted) { l->granted = 1u; l->amount = want; l->grants++; }
        l->clause = 0u;
        if (forgive) *forgive = l->amount;
        return 0u;
    }
    if (l->granted) {                                     /* a standing grant lost its evidence: withdraw for the boot */
        l->withdrawn = 1u; l->granted = 0u; l->withdrawals++;
        l->clause = c; l->first_clause = c;
        return withdrawn_clause;
    }
    l->clause = c; l->refusals++;
    return c;
}

static inline uint32_t n48_fg_step(n48_fg_latch *l, const n48_fg_in *f, uint64_t *forgive)
{
    uint64_t want = 0u;
    if (forgive) *forgive = 0u;
    if (!l) return N48_FG_NOT_GATHERED;
    if (n48_fg_latch_begin(l, N48_FG_WITHDRAWN)) return N48_FG_WITHDRAWN;
    const uint32_t c = n48_fg_eval(f, &want);
    return n48_fg_latch_apply(l, c, want, N48_FG_WITHDRAWN, N48_FG_AMOUNT_MOVED, forgive);
}

/* =====================================================================================================================
 * 0.0.381 — EXIT C: THE RUNNING BUDGET. THE SECOND GRANT SOURCE, AND THE ONLY ONE THAT FORGIVES DROPS THE
 * COMMITTING COMPOSITOR ITSELF MADE. DEFAULT N = 0, WHICH IS OFF, AND N = 0 IS THE BOOT VALUE.
 * =====================================================================================================================
 *
 * WHY THIS IS A NEW RULE AND NOT A VALUE IN THE OLD ONE (CONFIRMED against this file). X9-F above has
 * exactly one non-zero `*forgive =` assignment, `f->base_total`, and it is reachable only past
 * `if (f->now_total != f->base_total) return N48_FG_POST_BASELINE;`. A RUNNING budget is post-baseline drops BY
 * DEFINITION, so X9-F answers POST_BASELINE and hands the socket 0, on every input, forever. The socket in gfx_dep.h
 * (`fg_enabled` / `fg_clause` / `fg_forgive`, the four-way-guarded subtraction, N48_DEP_ID_FORGIVE tested BEFORE
 * N48_DEP_SOURCE_NEUTER) needs ZERO change and gets none: it is value-driven and it already accepts this.
 *
 * ---------------------------------------------------------------------------------------------------------------------
 * THE PRICE. FORGIVING THE COMPOSITOR'S OWN DROPPED FRAMES IS THE WHOLE OF EXIT C. READ THIS BEFORE THE CLAUSES.
 * ---------------------------------------------------------------------------------------------------------------------
 * X9-F's clause 4 - "the committing context has NEVER had a frame dropped" - exists because the frames whose surfaces the
 * committing frame is likeliest to sample are exactly its own. THIS RULE DOES NOT HAVE THAT CLAUSE, and there is no
 * version of it that does and still works. The attribution machinery to exclude them is present and correct
 * (`fg_note_drop_locked` attributes every drop to `wf.ownerSeq` under gWsLock, and X9-F's own eval excludes own drops
 * twice), so being sighted is free - AND BEING SIGHTED KILLS IT. `arm9`'s own `dpled841` lines, measured:
 *
 *      frame    ns (WindowServer's own)    sn (all source-neuters)    NOT the compositor's own
 *      f15              11                          13                          2
 *      f29              24                          27                          3
 *      f256            250                         254                          4
 *
 * A budget that excluded the committing side could forgive at most 2 at `f15` against a requirement of 13, and THE GATE
 * WOULD REFUSE EXACTLY AS IT DOES TODAY. So: to buy `f15` at all, this rule must forgive the compositor's own dropped
 * frames - the surfaces the next frame is likeliest to sample. That is the price, it is the whole price, and it is not
 * avoidable. The rule therefore REFUSES TO GRANT AT ALL unless it has been told, positively, how large that share is
 * (`own_known`, N48_RB_NOT_SIGHTED): no grant may be made without the number that says how bad it is. The number is
 * reported and never acted on, which is the honest shape - acting on it is what does not work.
 *
 * ---------------------------------------------------------------------------------------------------------------------
 * WHAT CLAUSE 5 WAS PROTECTING, AND WHERE THAT PROTECTION IS LOST. THIS IS THE SHARPEST OBJECTION.
 * ---------------------------------------------------------------------------------------------------------------------
 * X9-F's clause 5 is `now_total == base_total`: ZERO drops of any class since the baseline. It is what closes the
 * RECYCLED-PAGE case, in this file's own words above: "a clear of a re-allocated page either ran, or has not been
 * submitted, and no clear has been dropped". That argument rests entirely on the equality. THE RULE BELOW HAS NO SUCH
 * EQUALITY - its whole purpose is to grant while `now_total > base_total` - SO THE RECYCLED-PAGE ARGUMENT IS GONE AND
 * THERE IS NO REPLACEMENT FOR IT HERE. The exact line where it is lost is the absence of a
 * `if (b->now_total != b->base_total) return ...;` between N48_RB_BINDING_MOVED and N48_RB_NOTHING_TO_FORGIVE: put that
 * line back and this rule becomes X9-F with extra steps and grants nothing, ever. What replaces it is NOT a safety
 * argument but a BOUND: at most N drops may be outstanding, N is a small measured number, and the rule refuses outright
 * rather than granting a partial amount (N48_RB_OVER_BUDGET). A bound is a smaller claim than a proof and it is stated
 * here as one. The recycled-page case is ACCEPTED, not discharged.
 *
 * Clause 9 (COEXISTED) is also gone, and by definition: it repaired clause 8's shared-IOSurface hole by proving the
 * owning contexts never coexisted with the committing one, and the contexts this rule forgives are alive and coexisting
 * with it right now. Exit C ships without the strongest clause X9-F has. Clauses 6, 7 and 8 (the census, the accounting
 * identity, the owners being dead) are absent because they are OWNER-based and this rule is COUNT-based: they would not
 * make a count-based grant sounder, and every non-load-bearing conjunct here is a way for the run to produce no evidence
 * while a standing grant is withdrawn for the boot. Clauses 1, 2, 3 and 10 are kept, unchanged in substance.
 *
 * ---------------------------------------------------------------------------------------------------------------------
 * THE HAZARD, AND THAT IT IS SILENT. NOTHING THIS PROJECT HAS CAN TELL US WHETHER IT BIT.
 * ---------------------------------------------------------------------------------------------------------------------
 * The stale surface is MAPPED - it is a live IOSurface with a page table entry - and only its CONTENTS are wrong. So the
 * first-order outcome is NOT a fault: the consumer's `global_load` / `s_load_b128` returns whatever those bytes held
 * before, the frame draws, the fence retires, and NO COUNTER MOVES. STALE OR GARBAGE PIXELS ARE THE LIKELY OUTCOME AND
 * THEY ARE INVISIBLE TO EVERY INSTRUMENT THIS PROJECT HAS: `dpg_probe_watch` proves presence, not correctness;
 * `fence828` proves execution, not correctness; `gpuRestart` and the channel reset rounds are camouflaged by `wskill`.
 * Escalation is possible only where the stale bytes are DEREFERENCED - a dropped clear of a RE-ALLOCATED page whose
 * previous tenant's bytes are then read as a descriptor, an indirect draw argument or a vertex-buffer address, at which
 * point the stale value becomes an ADDRESS. That is a GPU VM fault, and `arm3` is the CONFIRMED precedent
 * for what follows one: GCVM_L2_PROTECTION_FAULT_STATUS 0x0024295d, VMID 2, WRITE to 0x23f0580000, CP_IB1_BASE naming
 * the committed IB -> Apple hung channel 51 -> six resets that did not recover -> Apple's own watchdog PANICKED THE BOX
 * 120 s later. `arm5` committed and kept running, so the chain is NOT automatic - but when it fires it is short and it
 * ends in a panic we cannot catch. The one detector that could CONFIRM the hazard costs no hardware write and lives in
 * the kext: intersect the X9 witness table's physical pages with the descriptor port's own PROVENANCE ask
 * on the committed frame. A hit is CONFIRMED; AN EMPTY INTERSECTION PROVES NOTHING, because we still cannot enumerate
 * what a frame reads.
 *
 * ---------------------------------------------------------------------------------------------------------------------
 * THE BOUND N, AND WHY 16 (from `arm9`'s own dpled841 lines). PRESERVED HERE BECAUSE IT IS THE WHOLE OF
 * THE SAFETY CASE AND A LATER READER WILL OTHERWISE RE-PICK IT FROM NOTHING.
 *   N <= 12   BUYS NOTHING. `f1` already commits at `sn 0` today; the gate still refuses at `f15`'s 13. A run at N <= 12
 *             reproduces `arm9` and proves only that the lever is inert.
 *   N = 16    CLEARS `f15`'s MEASURED 13 WITH THREE OF SLACK. The slack is load-bearing, not decoration: `sn` is read by
 *             the gather and the frame commits after it, so a drop landing in between must not turn a PASS into an
 *             over-budget refusal.
 *   N = 32    BUYS ONLY `f29`, FOR 2.1x THE STALE-SURFACE EXPOSURE, and found that in three boots of ~840 judged
 *             frames each not one frame reaching VERDICT TRANSLATE targeted a presented plane - so neither `f15` nor
 *             `f29` produces a picture. It is a second sample of the same class. Second run at most, never the first.
 *   N >= 64   RECKLESS: a quarter of a boot's entire drop budget, chosen against no measurement. This is the ceiling the
 *             rule itself refuses above (N48_RB_MAX), so a typo in the lever's high byte cannot reach it.
 *   N >= 254  NOT A BOUND AT ALL - it deletes the rung, which is never wanted.
 * ===================================================================================================================== */

/* The rule's own ceiling.: >= 64 is reckless and >= 254 deletes the rung. The verb refuses above this too; a
 * rule that trusts its caller's bound is not a bound. */
#define N48_RB_MAX 64u

enum {
    N48_RB_PASS = 0,
    N48_RB_NOT_GATHERED,       /* the request was never filled from live state - a zero request is NOT a clean world */
    N48_RB_DISABLED,           /* clause 1: N is 0. THE DEFAULT, THE BOOT VALUE, AND "OFF" */
    N48_RB_WITHDRAWN,          /* clause 11: a grant was withdrawn earlier this boot and can never be re-granted */
    N48_RB_BUDGET_INSANE,      /* N is above N48_RB_MAX: the rule refuses rather than trusting its caller */
    N48_RB_NO_BASELINE,        /* clause 2: no baseline was latched */
    N48_RB_BASE_CTX,           /* clause 2: the baseline names no context, or not the one bound now */
    N48_RB_NOT_BOUND,          /* clause 2: ws_ident is not BOUND right now */
    N48_RB_BINDING_MOVED,      /* clause 3: binds/rebinds/unbinds/gone are not the baseline's */
    N48_RB_NOTHING_TO_FORGIVE, /* the world is already clean: nothing to subtract, and the latch is NOT frozen at 0 */
    N48_RB_OVER_BUDGET,        /* THE BOUND: more is outstanding than N. Refuses OUTRIGHT - see below */
    N48_RB_OTHER_CLASSES,      /* clause 10: another cumulative class is dirty; this rule reasons about ONE class */
    N48_RB_NOT_SIGHTED,        /* THE PRICE WAS NOT MEASURED: no grant without the compositor's own share */
    N48_RB_AMOUNT_MOVED,       /* clause 11: a later grant is not the amount first granted */
    N48_RB_CLAUSES
};

static inline const char *n48_rb_clause_name(uint32_t c)
{
    static const char *const n[N48_RB_CLAUSES] = {
        "PASS", "not-gathered", "N-is-0-(OFF,-the-default)", "WITHDRAWN", "N-above-the-ceiling", "no-baseline",
        "baseline-context", "not-bound", "binding-moved", "nothing-to-forgive", "OVER-BUDGET",
        "another-class-already-dirty", "the-price-was-not-measured", "granted-amount-moved" };
    return c < N48_RB_CLAUSES ? n[c] : "?";
}

/* PASS must be 0 in both rules, because n48_fg_latch_apply tests `c == 0u` for both. This fails to compile otherwise. */
typedef char n48_fg_pass_is_zero[(N48_FG_PASS == 0 && N48_RB_PASS == 0) ? 1 : -1];

/* Everything the running budget judges. Zero-initialised; `gathered` must be POSITIVELY set, and `budget` 0 is OFF. */
typedef struct {
    uint32_t gathered;
    uint64_t budget;             /* N, from `accel gfxneuter 19 | N << 8`. 0 IS THE DEFAULT AND THE BOOT VALUE */

    /* Clause 2 and clause 3, unchanged in substance from X9-F: the baseline's identity is still what says WHICH
     * compositor this grant is for, even though the AMOUNT no longer comes from the baseline. */
    uint32_t base_valid;
    uint32_t base_ctx_seq;
    uint64_t base_total;         /* REPORTED ONLY. Kept so the log can show how far past the baseline we are */
    uint64_t base_binds, base_rebinds, base_unbinds, base_gone;
    uint32_t ws_bound;
    uint32_t ws_seq;
    uint64_t binds, rebinds, unbinds, gone;

    uint64_t now_total;          /* the source-neuter count the socket is about to read, BEFORE any subtraction */
    uint64_t other_classes;      /* clause 10: ring-neuter + walk-stopped/raced + unresolved target + witness overflow */

    /* THE PRICE, MEASURED. Neither of these ever refuses on its VALUE; `own_known` refuses on its ABSENCE. */
    uint32_t own_known;          /* 1 when the two counts below were read under the census lock. 0 REFUSES */
    uint64_t own_ctx;            /* drops the census attributed to the COMMITTING CONTEXT - context granularity */
    uint64_t own_proc;           /* gGs.neuteredSubs: WindowServer at PROCESS granularity */
} n48_rb_in;

/* THE RULE. Pure. Returns N48_RB_PASS and sets *forgive to the amount, or the first clause that refused with *forgive 0.
 *
 * WHY IT REFUSES OUTRIGHT OVER BUDGET INSTEAD OF GRANTING N OF A LARGER NUMBER. Granting a partial amount would buy
 * nothing - the socket would subtract N and the gate would still refuse at N48_DEP_SOURCE_NEUTER on the remainder - and
 * it would do real harm: clause 11 FREEZES the first granted amount, so a partial grant at one frame makes every later
 * frame's different amount N48_RB_AMOUNT_MOVED and WITHDRAWS the grant for the boot. A rule that cannot buy the frame
 * must not spend the boot's one grant on it. */
static inline uint32_t n48_rb_eval(const n48_rb_in *b, uint64_t *forgive)
{
    if (forgive) *forgive = 0u;
    if (!b || b->gathered != 1u) return N48_RB_NOT_GATHERED;

    /* Clause 1: the switch, which here IS the bound. N = 0 is off and is what a boot that nobody touched has. */
    if (b->budget == 0u) return N48_RB_DISABLED;
    if (b->budget > N48_RB_MAX) return N48_RB_BUDGET_INSANE;

    /* Clause 2: the baseline, and that it belongs to the context bound RIGHT NOW. The amount no longer comes from the
     * baseline, but the IDENTITY still must: a grant made against some other compositor's bind is a grant for a world
     * this one never saw. */
    if (b->base_valid != 1u) return N48_RB_NO_BASELINE;
    if (b->base_ctx_seq == 0u) return N48_RB_BASE_CTX;
    if (b->ws_bound != 1u) return N48_RB_NOT_BOUND;
    if (b->ws_seq != b->base_ctx_seq) return N48_RB_BASE_CTX;

    /* Clause 3: the binding has not moved. */
    if (b->binds != b->base_binds || b->rebinds != b->base_rebinds || b->unbinds != b->base_unbinds ||
        b->gone != b->base_gone)
        return N48_RB_BINDING_MOVED;

    /* ****** HERE IS WHERE X9-F's CLAUSE 5 WOULD BE, AND IT IS NOT HERE. ******
     * X9-F reads, at exactly this point:   if (f->now_total != f->base_total) return N48_FG_POST_BASELINE;
     * That equality is what closes the recycled-page case, and this rule cannot have it and still do anything. The
     * protection is LOST AT THIS LINE and the bound below is what stands in its place. See the header block. */

    if (b->now_total == 0u) return N48_RB_NOTHING_TO_FORGIVE;   /* already clean: no grant, and none needed */
    if (b->now_total > b->budget) return N48_RB_OVER_BUDGET;    /* THE BOUND */

    /* Clause 10: exactly one class may be forgiven, and the others must be clean. Every one of them is monotone. */
    if (b->other_classes != 0u) return N48_RB_OTHER_CLASSES;

    /* THE PRICE MUST HAVE BEEN MEASURED. The values are never judged - `own_ctx` may be the whole grant and this still
     * passes - but a caller that could not read them has not shown what it is buying, and does not get the grant. */
    if (b->own_known != 1u) return N48_RB_NOT_SIGHTED;

    if (forgive) *forgive = b->now_total;
    return N48_RB_PASS;
}

/* CLAUSE 11 FOR THE RUNNING BUDGET, ON THE SAME LATCH MACHINERY X9-F USES - WITH ONE DELIBERATE DIFFERENCE THAT A
 * REVIEWER MUST SEE, BECAUSE IT IS THE PLACE THIS IS WEAKER THAN IT FIRST LOOKS.
 *
 * WHAT THE LATCH FREEZES IS **THE BOUND**, NOT THE AMOUNT. X9-F can freeze the amount because its amount is
 * `base_total`, a CONSTANT: every later evaluation of an unchanged world asks for the same number, so AMOUNT_MOVED fires
 * only when the baseline really moved. THE RUNNING BUDGET'S AMOUNT IS `now_total`, WHICH ONLY EVER GROWS. Freezing it
 * would make the SECOND judged frame after a grant ask for a larger number, fire AMOUNT_MOVED, and withdraw the grant
 * for the boot - and since the gather runs on nearly every judged frame and not only on the one that commits, the boot's
 * one grant would be spent on whatever frame happened to be judged first (arm9: `f2`, at `sn 1`) and Exit C would never
 * reach `f15` at all. So the frozen quantity is N: the bound may never move under a standing grant, and what is
 * forgiven is re-bounded by it on every single call (n48_rb_eval refuses OVER_BUDGET).
 *
 * WHAT THIS COSTS, STATED PLAINLY. It is NOT "one forgiven frame per boot". While the grant stands, EVERY judged frame
 * whose outstanding count is at or under N passes X9's source-neuter rung. HOW MANY OF THEM COMMIT is bounded elsewhere
 * and not here: by n48_cm_gate's other rungs, and above all by the COMMIT arm's own frame budget (gfx_commit.ha),
 * which is 1 by default and which is the thing that actually limits the blast radius. If that budget is wrong, this is
 * wrong. The two are one unit and must be reviewed as one.
 *
 * WHAT IS STILL MONOTONE, AND IT IS MOST OF IT: the grant is made ONCE (`grants` counts 1 however many frames it
 * covers); ANY later non-PASS verdict withdraws it for the rest of the boot and `withdrawn` is never cleared - which
 * includes the world drifting past N (OVER_BUDGET), so the budget CLOSES ITSELF permanently the moment it stops being
 * able to buy anything; N changing in either direction under a standing grant is AMOUNT_MOVED and withdraws; and
 * turning the lever off (N = 0 -> DISABLED) withdraws. Going back is always allowed; coming back is not. */
static inline uint32_t n48_rb_step(n48_fg_latch *l, const n48_rb_in *b, uint64_t *forgive)
{
    uint64_t want = 0u, frozen = 0u;
    if (forgive) *forgive = 0u;
    if (!l) return N48_RB_NOT_GATHERED;
    if (n48_fg_latch_begin(l, N48_RB_WITHDRAWN)) return N48_RB_WITHDRAWN;
    const uint32_t c = n48_rb_eval(b, &want);
    /* The latch is handed THE BOUND to freeze, never the amount. On a refusal it is handed 0, which the latch does not
     * look at. `want` is what this call authorises and it is <= the bound by n48_rb_eval's own OVER_BUDGET rung. */
    const uint32_t r = n48_fg_latch_apply(l, c, c == N48_RB_PASS ? b->budget : 0u,
                                          N48_RB_WITHDRAWN, N48_RB_AMOUNT_MOVED, &frozen);
    if (r == N48_RB_PASS && forgive) *forgive = want <= frozen ? want : 0u;   /* belt and braces over the rung above */
    return r;
}

#endif /* N48_GFX_FORGIVE_H */
