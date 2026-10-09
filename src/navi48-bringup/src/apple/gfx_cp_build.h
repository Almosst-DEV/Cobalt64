/* gfx_cp_build.h — 0.0.393: THE CONSUMER LIST'S THREE LOOPS, IN ONE PLACE.
 *
 * WHY THIS HEADER EXISTS. Through 0.0.392 the consumer's pointer list was built by three loops inside
 * AppleHardwareHook.cpp's gfxsrc_policy, and tests/gfx_dep_test.cpp carried a SECOND copy of those loops ("the mirror").
 * measured the cost of that arrangement: the mirror was pinned only to four source strings, so replacing the real
 * fail-open `if (!fixed[q]) { gXpIn.over = 1u; continue; }` with `if (!fixed[q]) { continue; }` left the mirror's 476
 * checks passing, 0 failed. A test that no mutation of the REAL code can break is not testing the real code.
 *
 * THE FIX, AND IT IS THE FIRST OF THE TWO NAMES: move the loops into a header the kext calls once. There is now ONE
 * copy - this file - and it is compiled into both the kext (AppleHardwareHook.cpp) and the host suite (gfx_dep_test.cpp
 * includes it). The three loops, the `over` expression and the `ptr_inherit` carry are byte for byte 0.0.392's; moving
 * them changes no output, no counter, and no verdict.
 *
 * C++ ONLY (the value-initialisation below), and deliberately NOT included by gfx_dep.h: gfx_dep.h is also compiled as C
 * by src/xlat12/xlat12_ib.c, and this header depends on xlat12_ib.h. Only the two C++ call sites include it.
 */
#ifndef N48_GFX_CP_BUILD_H
#define N48_GFX_CP_BUILD_H

#include "gfx_dep.h"
#include "xlat12_ib.h"   /* xlat12_draw_stats and XLAT12_ABI_PTR_MAX: the translator's own export, no copy of its layout */

/* Build the consumer the rule evaluates, from the translator's OWN draw stats. EXACTLY 0.0.392's three loops, in the same
 * order, with the same `over` rules:
 *   - the image inputs, clamped to N48_CP_IN_MAX;
 *   - the three fixed heap pages (class-19 table, image heap, S#) that R3 always checks;
 *   - the fragment stage's declared ABI pointer pages (XLAT12_ABI_PTR_MAX of them at most);
 *   - the vertex stage's declared ABI pointer pages, gathered by condition 4 and kept in their OWN array so neither
 *     stage's list can push the other's out;
 *   - R4's own wait and memory-write counts.
 * `over` keeps ONLY the "the list did not fit / the program's ABI is unknown" causes; an inherited slot is carried
 * separately in `ptr_inherit` and refuses under its own name. `enumerated` is POSITIVELY set to 1: a builder that ran is
 * the only way the rule may speak, and a consumer nobody built still reads `enumerated 0` and refuses. */
static inline void n48_cp_build_consumer(n48_cp_consumer *c, const xlat12_draw_stats *ds)
{
    if (!c || !ds) return;
    *c = n48_cp_consumer {};
    c->n = ds->in_n > N48_CP_IN_MAX ? N48_CP_IN_MAX : ds->in_n;
    /* 0.0.438 (FINDING 4 INTERIM REFUSAL; an internal design note Finding 4) — a segment whose
     * translate reports MORE THAN ONE DRAW exported only its LAST draw's inputs (xlat12_ib.c's d_in_clear runs on
     * EVERY draw, never accumulating), so an earlier draw's own pointer was never checked. Until D4' lands (per-draw
     * accumulation), such a segment's own consumer list must be treated as INCOMPLETE - the same `over` a list that
     * did not fit or an unknown ABI already sets - so the dep rung can never pass a multi-draw segment on the last
     * draw's inputs alone. A single-draw segment (ds->draws == 1, every commit to date) is unchanged. */
    c->over = (ds->in_over || ds->in_n > N48_CP_IN_MAX || !ds->in_ptr_known || !ds->in_vptr_known ||
               ds->draws > 1u) ? 1u : 0u;
    c->ptr_inherit = ds->in_ptr_inherit;
    for (uint32_t q = 0; q < c->n; q++) {
        c->va[q] = ds->in_va[q]; c->mode[q] = ds->in_mode[q]; c->proven[q] = ds->in_proven[q];
    }
    const uint64_t fixed[3] = { ds->in_tbl_va, ds->in_img_va, ds->in_samp_va };
    for (uint32_t q = 0; q < 3u; q++) {
        if (!fixed[q]) { c->over = 1u; continue; }
        if (c->nptr >= N48_CP_PTR_MAX) { c->over = 1u; break; }
        c->ptr[c->nptr++] = fixed[q];
    }
    for (uint32_t q = 0; q < ds->in_nptr && q < XLAT12_ABI_PTR_MAX; q++) {
        if (c->nptr >= N48_CP_PTR_MAX) { c->over = 1u; break; }
        c->ptr[c->nptr++] = ds->in_ptr[q];
    }
    for (uint32_t q = 0; q < ds->in_nvptr && q < XLAT12_ABI_PTR_MAX; q++) {
        if (c->nptr >= N48_CP_PTR_MAX) { c->over = 1u; break; }
        c->ptr[c->nptr++] = ds->in_vptr[q];
    }
    c->waits = ds->r4_waits;
    c->memwrites = ds->r4_memwrites;
    c->enumerated = 1u;
}

/* 0.0.421 (an internal design note H4) — THE FRAME'S READ-SET IS THE UNION OF EVERY SEGMENT'S.
 *
 * THE HAZARD. The kext captures the consumer list per SEGMENT (`n48_cp_build_consumer(&gXpIn, &ds)` inside the
 * policy's segment loop), and through 0.0.420 the gate read whatever `gXpIn` the LAST segment left. A multi-segment
 * frame was therefore judged on its last segment's reads only: an input an EARLY segment binds — and a held-back
 * frame may have left unwritten — was invisible to R1-R4. Every committed frame to date is one segment, so the
 * fail-open has never fired; MIB (multi-IB) and R1's three-segment producers are the first multi-segment commits.
 *
 * THE FIX, and the two properties it is built on. This merges ONE segment's list INTO the frame's accumulator:
 *
 *   1. SINGLE-SEGMENT IDENTITY. On a zeroed accumulator the merge appends every input and every pointer in the
 *      same order the builder produced them, and copies the same four scalars, so the result is FIELD FOR FIELD the
 *      one segment's own consumer. A one-segment frame is therefore judged exactly as 0.0.420 judged it — the
 *      existing suites are the proof.
 *   2. NOTHING IS EVER REMOVED. It does not dedupe: a page named by two segments is asked about twice, which can
 *      only make the rule refuse more, never less. That is also why the fixed heap page the builder already names
 *      twice stays named twice (the reason N48_CP_PTR_MAX was RAISED, NOT DEDUPED).
 *
 * A segment whose list did not fit, or whose programs' ABI was unknown, carries `over`; an accumulator any segment
 * sets `over` on is INCOMPLETE and n48_cp_eval answers N48_CP_LIST_OVER. Appending past either cap sets `over` for
 * the same reason. A segment the caller could not enumerate at all is not passed here; the kext marks the
 * accumulator UNPROVEN for it (see AppleHardwareHook.cpp's segment feed), which is the fail-closed direction.
 *
 * Parameter names are `dst`/`seg`, NOT `c`, on purpose: gfx_dep_test.cpp pins that every use of the consumer's pointer
 * array in this header is the builder's own store, so a read of the union's source array must not be counted as one. */
static inline void n48_cp_merge_consumer(n48_cp_consumer *dst, const n48_cp_consumer *seg)
{
    if (!dst || !seg) return;
    if (seg->enumerated != 1u) { dst->over = 1u; return; }   /* a segment nobody enumerated: the union is incomplete */
    dst->enumerated = 1u;
    if (seg->over) dst->over = 1u;
    if (seg->input_free) dst->input_free = 1u;
    dst->ptr_inherit += seg->ptr_inherit;
    dst->waits += seg->waits;
    dst->memwrites += seg->memwrites;
    for (uint32_t q = 0u; q < seg->n; q++) {
        if (dst->n >= N48_CP_IN_MAX) { dst->over = 1u; break; }
        dst->va[dst->n] = seg->va[q];
        dst->mode[dst->n] = seg->mode[q];
        dst->proven[dst->n] = seg->proven[q];
        dst->n++;
    }
    for (uint32_t q = 0u; q < seg->nptr; q++) {
        if (dst->nptr >= N48_CP_PTR_MAX) { dst->over = 1u; break; }
        dst->ptr[dst->nptr++] = seg->ptr[q];
    }
}

/* 0.0.406 — THE INPUT-FREE FILL CONSUMER, BUILT UNDER THE SAME ONE-COPY RULE AS THE LOOPS ABOVE.: a
 * `ws_B_ColorFill` carries no descriptor table, so it never runs the table step, so `ds.in_abi` is 0 and the builder above
 * is never called for it - and the fill then falls to `n48_dep_check`'s `source_neuters` rung, which is >= 1 from f3 on
 * every measured boot, so only f1 ever commits. This builder is the second shape: the frame is input-free EXACTLY when
 *
 *     nseg == 1  AND  the in-force PS identity is ws_B_ColorFill  AND  the translator saw no descriptor table
 *                AND  no image reads  AND  its single pointer page is named (the PS_2/3 colour pointer, AFTER any
 *                fill-colour retarget)  AND  the bound context's root[511] is already WRITTEN (the sixth condition)
 *
 * and then it is enumerated with ZERO image inputs and ONE pointer page. The single pointer page is where the fail-closed
 * edge is: without a named VA there is no page to resolve, so this refuses and the frame stays on 0.0.389's path.
 *
 * Why the retarget VA and not Apple's original: measured the pair as a plain 64-bit VA of a 16-byte float4 that
 * `ws_B_ColorFill` LOADS, and the kext can only NAME that VA when the retarget wrote it (`ds.fill_color_new`, our
 * arena). An untargeted fill's pointer VA is not exported by the translator anywhere else, so this builder is handed 0 and
 * declines - fail-closed, never a guessed page. OFF (switch 33), the kext never calls this and `enumerated` stays 0.
 *
 * THE SIXTH CONDITION (0.0.407). root[511] - the page table entry that makes the arena pointer page
 * resolvable at all - is written by the KEYSTONE INSIDE THE FIRST COMMIT, after translation. So on the first fill (f1)
 * the named pointer page CANNOT resolve: if the fill were enumerated input-free there, R3 would refuse it `R3-pointer-page`,
 * it would be retired, root[511] would never be written, and every later fill would refuse the same way -'s mistake
 * exactly, re-introduced through the R3 page instead of the verb gate. `root_written` is the kext's own reading of that
 * state (`gfxsrc_fillcolor_root_written`), and requiring it here means f1 falls through to 0.0.389's rung (source_neuters
 * 0 before it) and COMMITS as arm22 did, so the keystone writes root[511] and the twin fill is then enumerated and clean.
 * It is a POSITIVE condition: a caller that cannot answer it passes 0 and the frame stays off this path.
 *
 * 0.0.438 (FINDING 4 INTERIM REFUSAL; an internal design note Finding 4) — `draws` is the frame's ONE
 * segment's own draw count (xlat12_draw_stats.draws, `nseg == 1` already requires exactly one). A segment's translate
 * exports only its LAST draw's inputs, and this builder's whole premise is that the frame reads NOTHING beyond the
 * one named pointer page - a premise a GPUPass-then-ColorFill segment (or any other multi-draw segment whose last
 * draw happens to bind the fill program) would silently violate, because the EARLIER draw's own reads were never
 * exported at all. Refusing whenever `draws != 1` keeps this builder fail-closed until D4' lands: a single-draw
 * fill segment (every committed fill measured so far, per an internal design note) is unchanged.
 *
 * Returns 1 when the consumer was built, 0 (leaving `*c` zeroed) otherwise. Pure; the kext and the host suite compile the
 * SAME function. */
static inline uint32_t n48_cp_build_input_free(n48_cp_consumer *c, uint32_t nseg, uint32_t ps_fill,
                                                uint32_t has_table, uint32_t n_img, uint64_t ps_ptr_va,
                                                uint32_t root_written, uint32_t draws)
{
    if (!c) return 0u;
    *c = n48_cp_consumer {};
    if (nseg != 1u || !ps_fill || has_table || n_img != 0u || !ps_ptr_va || !root_written || draws != 1u) return 0u;
    c->input_free = 1u;
    c->n = 0u;                       /* no image inputs: R1 and R2 are structurally vacuous for this shape */
    c->ptr[c->nptr++] = ps_ptr_va;   /* the ONE pointer page: PS_2/3, after any fill-colour retarget */
    c->enumerated = 1u;
    return 1u;
}

/* =====================================================================================================================
 * D4' (an internal design note item 3/4/G, an internal design note Q5) — THE D4' CONSUMER'S OWN BUILD AND MERGE.
 * Entirely separate from n48_cp_build_consumer / n48_cp_merge_consumer above: gated ENTIRELY by switch 40, called
 * from a DIFFERENT branch of gfxsrc_policy's segment loop (the `else if (gXpOn && gD4On)` sibling of the ds.in_abi
 * capture), and never touching gXpAcc/gXpIn or their caps. ===================================================== */

/* Build ONE segment's D4' consumer from the translator's rs_* export (xlat12_draw_stats, accumulated over EVERY draw
 * of the segment - xlat12_ib.c's d_readset_accum). `enumerated` is ALWAYS 1 here (a call happened); `over` is 1 when
 * ANY draw's program had no admitted row (ds->rs_declined) or a list overflowed this header's or xlat12's own
 * storage (ds->rs_over, or more entries than N48_CP_D4_IN_MAX/PTR_MAX can hold) - a declined or overflowed segment
 * still returns `enumerated 1, over 1` so the caller's merge marks the frame's union incomplete, never silently
 * absent. `waits`/`memwrites` are carried from ds->r4_waits/r4_memwrites UNCONDITIONALLY - the R1-MEMDST.md Q5
 * binding: those counters are already accumulated over the whole segment by xlat12_ib.c's d_region regardless of
 * this flag, so this builder need not (and must not) re-derive them. */
/* D4-PRIME item 4 — THE ARENA-RANGE DECLINE, BY VA, BEFORE ANY WALK. `arenaVaBase`/`arenaLen` are the caller's OWN
 * `gRingMap.vaBase`/`gRingMap.bytes` - our ring-region mapping, the arena - handed in as PLAIN DATA so this stays a
 * pure function: NOTHING here ever walks a page table or needs root[511], which the project's rule treats as
 * live only AFTER the first commit (a rule that needs it to RESOLVE before commit 1 refuses the very fill it exists
 * for -). A pointer VA landing in the arena is declined by address alone; `arenaLen` 0 (the caller has no
 * ring map yet, e.g. before the first commit) makes the range empty and this check inert, never a false decline. */
static inline uint32_t n48_cp_readset_in_arena(uint64_t va, uint64_t arenaVaBase, uint64_t arenaLen)
{
    if (!arenaLen) return 0u;
    return (va >= arenaVaBase && va < arenaVaBase + arenaLen) ? 1u : 0u;
}

/* Parameter named `rs4`, NOT `c`, on purpose: gfx_dep_test.cpp's C5 parity check pins that every pointer-array
 * dereference through the OTHER builder's own local variable above is its STORE pattern (append-then-increment),
 * never a bare read - a text-matching invariant scoped to that one variable name. Sharing it here would fold this
 * function's OWN indexed store/read (read back for the arena check) into that unrelated count. */
static inline void n48_cp_build_consumer_d4(n48_cp_consumer_d4 *rs4, const xlat12_draw_stats *ds,
                                            uint64_t arenaVaBase, uint64_t arenaLen)
{
    if (!rs4 || !ds) return;
    *rs4 = n48_cp_consumer_d4 {};
    rs4->over = (ds->rs_declined || ds->rs_over || ds->rs_n > N48_CP_D4_IN_MAX || ds->rs_nptr > N48_CP_D4_PTR_MAX) ? 1u : 0u;
    rs4->n = ds->rs_n > N48_CP_D4_IN_MAX ? N48_CP_D4_IN_MAX : ds->rs_n;
    for (uint32_t q = 0; q < rs4->n; q++) {
        rs4->va[q] = ds->rs_va[q]; rs4->mode[q] = ds->rs_mode[q]; rs4->proven[q] = ds->rs_proven[q];
        if (n48_cp_readset_in_arena(rs4->va[q], arenaVaBase, arenaLen)) rs4->over = 1u;
    }
    rs4->nptr = ds->rs_nptr > N48_CP_D4_PTR_MAX ? N48_CP_D4_PTR_MAX : ds->rs_nptr;
    for (uint32_t q = 0; q < rs4->nptr; q++) {
        rs4->ptr[q] = ds->rs_ptr[q];
        if (n48_cp_readset_in_arena(rs4->ptr[q], arenaVaBase, arenaLen)) rs4->over = 1u;
    }
    rs4->waits = ds->r4_waits;
    rs4->memwrites = ds->r4_memwrites;
    rs4->enumerated = 1u;
}

/* Merge ONE segment's D4' consumer into the frame's accumulator, DEDUPING THE POINTER LIST BY PAGE (D4-PRIME item 4:
 * "dedupes by page and ORs unproven" - here every accumulated pointer is asked the SAME physical R3 hazard question
 * regardless of which segment named it, so a page two segments both name is asked once, not twice; this can only
 * make the rule refuse EQUALLY often, never less, because the hazard answer for a page does not depend on how many
 * times it was named). IMAGE inputs are NOT deduped (mirroring n48_cp_merge_consumer's own "nothing is ever removed"
 * for images: R1/R2 are asked per named surface, and a surface named twice is not a different fact). Caps are
 * N48_CP_D4_IN_MAX/PTR_MAX (16/64) - the D4-PRIME item 4/G raise while switch 40 is ON; this type is never built or
 * merged while it is OFF, so there is no separate "OFF" cap to thread through. */
/* build 0.0.494 — THE IMAGE-UNION FOLD, switch 61 (`gfxneuter 61 | M << 8`, M 1 ON = 317, M 2 OFF =
 * 573, OFF by default and at boot). replayed the kext's own merge over run10h's wallpaper composite d: 13 units name
 * 24 image entries over 6 distinct surfaces, the append-only loop below hits N48_CP_D4_IN_MAX (16) and the union is marked
 * `over` although every one of those 24 entries is a repeat of one of 6 facts. `fold` (the caller's LATCHED switch 61,
 * never the live switch) makes an image entry whose va AND mode both equal an entry already in the union FOLD into it
 * instead of appending: the survivor's `proven` becomes the AND of both (fail-closed: 1 only when BOTH are exactly 1, so a
 * surface one unit could not prove stays unproven for R1, whichever unit named it first). Anything else - a different va,
 * or the same va read in a different mode - appends exactly as before, and the 16-entry cap still sets `over`; the cap
 * is asked only for an entry that did NOT fold (a duplicate never needs a free slot). The fold is asked BEFORE the cap for
 * that reason. Pointer handling is unchanged (already deduped by page). A segment that did not enumerate, or arrived
 * `over`, marks the union `over` exactly as before, fold or no fold.
 * fold 0: every statement below is the pre-0.0.494 body, in the same order (the fold branch is skipped whole), so the
 * union is byte-identical - gfx_dep_test.cpp's OFF-identity check compares it against a frozen copy of that body.
 * Returns the number of image entries folded (0 whenever `fold` is 0). Pure; writes nothing but `*dst`. */
static inline uint32_t n48_cp_merge_dedup_img(n48_cp_consumer_d4 *dst, const n48_cp_consumer_d4 *seg, uint32_t fold)
{
    uint32_t folded = 0u;
    if (!dst || !seg) return 0u;
    if (seg->enumerated != 1u) { dst->over = 1u; return 0u; }
    dst->enumerated = 1u;
    if (seg->over) dst->over = 1u;
    dst->waits += seg->waits;
    dst->memwrites += seg->memwrites;
    for (uint32_t q = 0u; q < seg->n && q < N48_CP_D4_IN_MAX; q++) {
        if (fold) {
            uint32_t hit = N48_CP_D4_IN_MAX;
            for (uint32_t k = 0u; k < dst->n && k < N48_CP_D4_IN_MAX; k++)
                if (dst->va[k] == seg->va[q] && dst->mode[k] == seg->mode[q]) { hit = k; break; }
            if (hit < N48_CP_D4_IN_MAX) {
                dst->proven[hit] = (dst->proven[hit] == 1u && seg->proven[q] == 1u) ? 1u : 0u;
                folded++;
                continue;
            }
        }
        if (dst->n >= N48_CP_D4_IN_MAX) { dst->over = 1u; break; }
        dst->va[dst->n] = seg->va[q]; dst->mode[dst->n] = seg->mode[q]; dst->proven[dst->n] = seg->proven[q];
        dst->n++;
    }
    for (uint32_t q = 0u; q < seg->nptr && q < N48_CP_D4_PTR_MAX; q++) {
        const uint64_t page = seg->ptr[q] & ~0xFFFull;
        int dup = 0;
        for (uint32_t k = 0u; k < dst->nptr && k < N48_CP_D4_PTR_MAX; k++)
            if ((dst->ptr[k] & ~0xFFFull) == page) { dup = 1; break; }
        if (dup) continue;
        if (dst->nptr >= N48_CP_D4_PTR_MAX) { dst->over = 1u; break; }
        dst->ptr[dst->nptr++] = seg->ptr[q];
    }
    return folded;
}

/* The pre-0.0.494 entry point, kept for every caller that has no switch 61 to pass: fold OFF. */
static inline void n48_cp_merge_dedup(n48_cp_consumer_d4 *dst, const n48_cp_consumer_d4 *seg)
{
    (void)n48_cp_merge_dedup_img(dst, seg, 0u);
}

/* =====================================================================================================================
 * D5 (D4-PRIME-FIXES.md item 5,  (B)) — THE PER-FRAME D4' ANSWER, isolated from gXpD4's boot-cumulative
 * counters (asks/enumerated/clause[]/declRow/declOverflow/evalUs/frameLines) so it can be reset EVERY frame without
 * touching them. Through 0.0.440 lastEnum/lastClause/lastUnproven were reset only INSIDE gfxsrc_cprov_eval's D4'
 * branch itself, so a frame that took a DIFFERENT branch (the table-ABI consumer enumerated it that frame, or gD4On
 * went off) left the PREVIOUS judged frame's answer sitting there - and gfx_dep.h's own N48_DEP_ID_CPROV identity
 * reads `d4_enumerated` (== gXpD4.st.lastEnum) UNCONDITIONALLY every frame, so a stale `lastEnum 1` while
 * `d4_enabled` (gD4On) is 0 that frame trips the CPROV bit on a frame that never touched D4' at all.
 * ===================================================================================================================== */
typedef struct {
    uint32_t lastEnum, lastClause;
    uint64_t lastUnproven;
} n48_cp_d4_state;

/* Pure: zeroes the per-frame answer. Called ONCE, at the very top of gfxsrc_cprov_eval, BEFORE `if (!gXpOn) return;`
 * - even an OFF pass (gXpOn 0) must not let a previous pass's D4' answer survive into THIS frame's identity read. */
static inline void n48_cp_d4_frame_begin(n48_cp_d4_state *s)
{
    if (!s) return;
    s->lastEnum = 0u; s->lastClause = 0u; s->lastUnproven = 0ull;
}

/* =====================================================================================================================
 * D2 (D4-PRIME-FIXES.md item 2) — THE WHOLE D4' JUDGING BODY, AS ONE PURE FUNCTION.
 *
 * Through 0.0.441 the D4' branch's page walks, the R1 discharge and the call to n48_cp_eval_hz_d4 lived inline in
 * gfxsrc_cprov_eval, with the timer wrapped around the eval call alone (0.0.440's own bug: the walks ran OUTSIDE
 * the timed region). This function holds all three, so a caller that times ONE call to it times the walks too, and
 * makes it host-testable: gfx_dep_test.cpp's shape runs this exact routing, not a copy of it.
 *
 * `resolve`/`resolveCtx` mirror the kext's own `gfxc_page(vm, va, ...)` call, abstracted to a plain callback so this
 * stays pure; `vmOk` is the caller's own `vm.ok` (a page walk with no live page table cannot resolve anything, the
 * SAME fail-closed reading the inline code gave it). Skips every walk when the union is already `over` or was never
 * enumerated (a pure clause below has already refused it - the same "no walk for a row already refused" discipline
 * R1 (gfx_memdst.h) applies). `mdEnforce`/`mdOk` are the SAME R1 discharge the table-ABI consumer's own rung takes
 * (an internal design note Q2 item C): under R1 ENFORCE with a clean memory-destination scan, R4's wait/write
 * refusal is redundant and is discharged for the D4' consumer exactly as it already is for the table-ABI one.
 *
 * THE ROUTING (D4-PRIME-FIXES.md item 2's own name, "a declined union keeps today's rung"):
 *   - COMPLETE (`enumerated == 1 && over == 0`): judged for real - `clause` is n48_cp_eval_hz_d4's own answer,
 *     `*enumForRung` 1 (this frame speaks for the CPROV identity, N48_DEP_ID_CPROV).
 *   - OVER, WITH a table segment present this frame (`hasTableSeg`, the caller's own `gXpInFrame == judged + 1`):
 *     `clause` LIST_OVER, `*enumForRung` 1 - 0.0.438's OWN answer for a frame the table path could not enumerate
 *     either; D4' does not WORSEN that frame's verdict, it only WIDENS coverage to frames the table path never
 *     touches at all.
 *   - OVER, WITHOUT a table segment: `clause` NOT_ENUM, `*unproven` 1, `*enumForRung` 0 - a frame neither path can
 *     prove is UNKNOWN to the CPROV identity rather than a wrong answer, so the dep rung falls through to
 *     `source_neuters` (gfx_dep.h's own older rung) instead of refusing on a union this build could not build.
 *
 * `overAttr` (build 0.0.494, switch 61 latched): 1 makes the OVER-with-table-segment branch name its count (1)
 * instead of 0. 0 is 0.0.493's routing exactly.
 *
 * `ccD4` is mutated in place (its own `resolved`/`ptr_resolved`/`ptr_page`, and `waits`/`memwrites` under the R1
 * discharge) - the caller passes its own file-scope accumulator, exactly as the inline code operated on it directly. */
typedef int (*n48_cp_page_resolve_fn)(void *ctx, uint64_t va, uint64_t *page);

static inline uint32_t n48_cp_d4_judge(n48_cp_consumer_d4 *ccD4, uint32_t hasTableSeg, uint32_t vmOk,
                                       void *resolveCtx, n48_cp_page_resolve_fn resolve,
                                       const n48_cp_ring *r, const n48_dep_witness *wt, n48_r5_ring *r5,
                                       uint32_t mdEnforce, uint32_t mdOk,
                                       uint64_t *unproven, uint64_t *stale, uint32_t *enumForRung,
                                       uint32_t overAttr)
{
    if (unproven) *unproven = 0ull;
    if (stale) *stale = 0ull;
    if (enumForRung) *enumForRung = 0u;
    if (!ccD4) return N48_CP_NOT_ENUM;
    if (ccD4->enumerated == 1u && !ccD4->over) {
        if (vmOk && resolve) {
            for (uint32_t q = 0; q < ccD4->n && q < N48_CP_D4_IN_MAX; q++) {
                uint64_t page = 0ull;
                ccD4->resolved[q] = resolve(resolveCtx, ccD4->va[q] & ~(uint64_t)0xFFFull, &page) ? 1u : 0u;
            }
            for (uint32_t q = 0; q < ccD4->nptr && q < N48_CP_D4_PTR_MAX; q++) {
                uint64_t page = 0ull;
                ccD4->ptr_resolved[q] = resolve(resolveCtx, ccD4->ptr[q] & ~(uint64_t)0xFFFull, &page) ? 1u : 0u;
                ccD4->ptr_page[q] = ccD4->ptr_resolved[q] ? page : 0ull;
            }
        } else {
            ccD4->over = 1u;   /* no usable page table (or no resolver at all): nothing here could be resolved */
        }
    }
    if (ccD4->enumerated == 1u && mdEnforce && mdOk) { ccD4->waits = 0u; ccD4->memwrites = 0u; }
    const uint32_t complete = (ccD4->enumerated == 1u && !ccD4->over) ? 1u : 0u;
    uint32_t clause;
    if (complete) {
        clause = n48_cp_eval_hz_d4(ccD4, r, wt, r5, unproven, stale);
        if (enumForRung) *enumForRung = 1u;
    } else if (hasTableSeg) {
        clause = N48_CP_LIST_OVER;
        /* build 0.0.494 — THE ATTRIBUTION FIX, switch 61 ON only (`overAttr`, the caller's LATCHED
         * switch 61). Through 0.0.493 this branch refused with a count of 0, and gfx_dep.h's CPROV identity reads
         * `clause != 0 && unproven == 0` as a malformed rule ("unaccounted", N48_DEP_ID_CPROV) - so a GENUINE overflow
         * never refused under its own name. With 61 ON the incomplete union counts as one unproven input, and the dep
         * rung refuses it as consumer-inputs-unproven. Still a refusal either way; OFF, this is 0.0.493's branch. */
        if (overAttr && unproven) *unproven = 1ull;
        if (enumForRung) *enumForRung = 1u;
    } else {
        clause = N48_CP_NOT_ENUM;
        if (unproven) *unproven = 1ull;
        if (enumForRung) *enumForRung = 0u;
    }
    return clause;
}

#endif /* N48_GFX_CP_BUILD_H */
