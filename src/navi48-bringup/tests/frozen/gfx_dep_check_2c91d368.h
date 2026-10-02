// gfx_dep_check_2c91d368.h — build 0.0.541 item 6: n48_dep_check EXACTLY as committed at 2c91d368 (0.0.540), renamed
// frozen540_dep_check. tests/gfx_wo99_test.cpp compares the live rule against it (OFF identity). Extracted by git show; never edit.
#ifndef N48_FROZEN_DEP_CHECK_2C91D368_H
#define N48_FROZEN_DEP_CHECK_2C91D368_H
static inline uint32_t frozen540_dep_check(const n48_dep_world *w, uint64_t *detail)
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
    if (w->witness_over) { if (detail) *detail = w->witness_over; return N48_DEP_WITNESS_OVER; }
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
#endif
