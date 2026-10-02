/* gfx_r3hit.h — build 0.0.550: THE R3 HAZARD-HIT INSTRUMENT. LOG-ONLY, EVERY MODE, NO SWITCH.
 *
 * WHY.: in RUN AX/AY reason 19 (R3, D4′ `R3-memdst`) became an ARM LATCH - n48_hz_add only ever sets bits - and from sec 81
 * (AX) / sec 23 (AY) every composite was refused while P copies kept committing. Every refusal named ONE pointer page. Nothing
 * printed WHICH page, whether the hit was an exact row of the hazard table or only the 1024-bit filter's two bits (a false
 * positive is possible there, gfx_hazard.h), or which held-back frame had put the page into the set.
 *
 * WHAT IT RECORDS. (1) The NAMER'S IDENTITY: the hazard set's exact rows are append-only within an arm scope, and every row a
 * note appends was named by that note's frame, so a table parallel to the rows (N48_HZ_ROWS entries) is filled right after each
 * n48_r5_note_counted with the frame's identity (pid, verdict key, CB0, first IB length, IB count, verdict) for the rows
 * [used_before, used_after) - or [0, used_after) when the note moved the scope (n48_hz_scope re-based the set). (2) THE HIT: for a
 * frame whose R3 clause answered R3-neutered-write-destination, every pointer page the filter answers (n48_hz_probe - the
 * NON-counting question, so the set's own queries/hits counters do not move) is printed with EXACT/FILTER-ONLY, the row's hits,
 * the VA it was named at, the namer frame and the namer's identity. Capped per arm scope and per frame; the count is not.
 *
 * Pure: no lock, no clock, no log. Host-tested by tests/gfx_wo99_test.cpp (r3hit section). */
#ifndef N48_GFX_R3HIT_H
#define N48_GFX_R3HIT_H

#include <stdint.h>
#include "gfx_hazard.h"

#define N48_R3H_LINES_SCOPE 64u   /* hit lines per arm scope */
#define N48_R3H_LINES_FRAME 4u    /* hit lines per frame */

typedef struct {
    uint64_t frame;     /* the namer's judged-frame number (the row's `namer`, recorded again as a cross-check) */
    uint64_t key;       /* the namer frame's verdict key (the program key its tally row names) */
    uint64_t cb0;       /* the namer frame's first colour target VA (0 = none) */
    int32_t pid;
    uint32_t len0;      /* its first IB's dwords */
    uint32_t nib;
    uint32_t verdict;
    uint32_t valid;
} n48_r3_id;

/* After a note: stamp `id` on the rows the note appended. seq_before/seq_after are the set's arm_seq around the note. */
static inline void n48_r3_id_note(n48_r3_id *tab, uint32_t used_before, uint32_t seq_before, uint32_t used_after, uint32_t seq_after,
                                  const n48_r3_id *id)
{
    if (!tab || !id) return;
    uint32_t start = used_before;
    if (seq_after != seq_before) {                       /* the scope moved: every row from 0 is this scope's */
        start = 0u;
        for (uint32_t i = 0u; i < N48_HZ_ROWS; i++) tab[i].valid = 0u;
    }
    for (uint32_t i = start; i < used_after && i < N48_HZ_ROWS; i++) { tab[i] = *id; tab[i].valid = 1u; }
}
/* The exact row holding `page`, or N48_HZ_ROWS when only the filter (or nothing) holds it. */
static inline uint32_t n48_r3_row_of(const n48_hazard *hz, uint64_t page)
{
    if (!hz || !page) return N48_HZ_ROWS;
    const uint64_t key = page & ~0xFFFull;
    for (uint32_t i = 0u; i < hz->used && i < N48_HZ_ROWS; i++) if (hz->row[i].page == key) return i;
    return N48_HZ_ROWS;
}
/* The per-frame / per-scope line cap. Returns 1 when a line may be printed (and takes it). */
typedef struct { uint32_t seq, scope_lines; uint64_t frame; uint32_t frame_lines; uint64_t hits, exact, filter_only, suppressed; } n48_r3h_cap;
static inline uint32_t n48_r3h_take(n48_r3h_cap *c, uint32_t seq, uint64_t frame)
{
    if (!c) return 0u;
    if (c->seq != seq) { c->seq = seq; c->scope_lines = 0u; }
    if (c->frame != frame) { c->frame = frame; c->frame_lines = 0u; }
    if (c->scope_lines >= N48_R3H_LINES_SCOPE || c->frame_lines >= N48_R3H_LINES_FRAME) { c->suppressed++; return 0u; }
    c->scope_lines++; c->frame_lines++;
    return 1u;
}

/* frame, pointer index / count, VA, page, EXACT|FILTER-ONLY, row hits, row VA, namer frame, identity (valid word, pid, key, CB0,
 * len, nib, verdict), totals. <= 491 bytes at maximal fields (tests/gfx_wo99_test.cpp). */
#define N48_R3H_FMT \
    "r3hit: f%llu ptr %u/%u VA %#llx page %#llx %s (hits %llu, named at VA %#llx) namer f%llu [%s pid %d key %#llx CB0 %#llx " \
    "%u dw x%u verdict %u]; hits %llu (exact %llu, filter-only %llu), suppressed %llu"

#endif /* N48_GFX_R3HIT_H */
