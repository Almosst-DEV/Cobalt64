// gfx_mib.h — 0.0.421 (notes/design/MIB-COMMIT.md binding B10). THE MIB-0 READ-ONLY CENSUS.
//
// Pure C++ (static inline), so the kext and its host suite (tests/gfx_mib_test.cpp) compile the SAME classifier and
// the SAME report format. NOTHING here changes a byte, gates a decision, takes a lock or writes a register: every
// counter is fed from facts the decide path ALREADY read, the report only prints them, and every counter moves with
// EVERY switch off — which is the whole point of an instrument that must be readable on a default boot.
//
// WHY. MIB-COMMIT classified the 106 IB boundaries of the captured 2- and 3-IB frames BY SCRIPT, over the 104
// bodies the capture keeps. MIB-0 asks the KEXT to do the same classification over the WHOLE boot, so the
// "50 clean / 27 NOP-headed / 29 mid-encoder" split stops being a one-capture sample. For every IB k >= 1 that the
// WindowServer gather reads, this records the START class of its body.
//
// THE CLASSES, each a property of the IB's OWN first dwords (MIB-COMMIT, VERIFIED there on the real bodies):
//   HEAD      dword 0/1/2 and 10/11 are the FULL encoder head — XLAT12_SEG_HEAD0, XLAT12_SEG_HEAD1,
//             XLAT12_SEG_ACQUIRE, then XLAT12_SEG_HEAD0 and XLAT12_SEG_EVENT_E eight dwords later. These are the
//             SAME five dwords xlat12_ib.c's `seg_start_at` requires, taken from xlat12_ib.h rather than copied, so
//             the classifier and the segmenter can never disagree about what a clean boundary is.
//   NOP-HEAD  dword 0 is 0xC0081000, the 10-dword PACKET3(NOP, 8) Apple writes OVER the EVENT_WRITE 0x16 +
//             ACQUIRE_MEM when the previous IB already ended a segment. The head is still there later
//             (dword 10/11 carry EVENT_WRITE 0xE), which is why the dword-0 test must come FIRST.
//   MID       anything else: the IB opens mid-encoder, in the same encoder's SET_CONTEXT_REG state and draws.
//   UNREAD    too short to classify (a short or failed read). It is counted APART so "no body" can never be read as
//             a class; the gather refuses such an IB before it is judged.
#ifndef N48_GFX_MIB_H
#define N48_GFX_MIB_H

#include <stdint.h>
#include "xlat12_ib.h"     // XLAT12_SEG_HEAD0/HEAD1/ACQUIRE/EVENT_E — the recogniser's own five dwords, not a copy
#include "gfx_fence828.h"  // N48_F828_REASONS: the final segment's find answer is tallied by its own reason index

/*: PACKET3(NOP, count 8) = 10 dw, over the EVENT_WRITE 0x16 + ACQUIRE_MEM. One dword, measured. */
#define N48_MIB_NOP_HEAD 0xC0081000u

enum {
    N48_MIB_START_HEAD = 0,
    N48_MIB_START_NOP_HEAD = 1,
    N48_MIB_START_MID = 2,
    N48_MIB_START_UNREAD = 3,
    N48_MIB_STARTS
};

static inline const char *n48_mib_start_name(uint32_t c)
{
    switch (c) {
    case N48_MIB_START_HEAD:     return "HEAD";
    case N48_MIB_START_NOP_HEAD: return "NOP-HEAD";
    case N48_MIB_START_MID:      return "MID";
    default:                     return "UNREAD";
    }
}

/* Classify the START of one IB body. Pure: reads d[0..n) and writes nothing.
 *
 * ORDER IS THE CONTRACT. The NOP-head test comes first because a NOP-headed IB still carries the EVENT_WRITE 0xE at
 * dword 10/11 (confirmed in the capture bodies), so a "is it a head?" test alone would misname it HEAD. A body too
 * short to hold the 12 dwords a head needs is UNREAD, never guessed; a body whose first three dwords look like a
 * head but whose tail does not is MID (not a boundary the segmenter recognises either). */
static inline uint32_t n48_mib_start_class(const uint32_t *d, uint32_t n)
{
    if (!d || n < 3u) return N48_MIB_START_UNREAD;
    if (d[0] == N48_MIB_NOP_HEAD) return N48_MIB_START_NOP_HEAD;
    if (d[0] == XLAT12_SEG_HEAD0 && d[1] == XLAT12_SEG_HEAD1 && d[2] == XLAT12_SEG_ACQUIRE) {
        if (n < 12u) return N48_MIB_START_UNREAD;
        if (d[10] == XLAT12_SEG_HEAD0 && d[11] == XLAT12_SEG_EVENT_E) return N48_MIB_START_HEAD;
    }
    return N48_MIB_START_MID;
}

/* ---- THE COUNTERS. One instance, fed by the decide path on EVERY boot, whatever switch is on or off. ---- */
#define N48_MIB_NIB 5u   /* nib 0..4; 0 is counted (a shape-refused frame) but is never a real IB count */

typedef struct {
    uint64_t ib_start[N48_MIB_STARTS];        /* IB k >= 1 bodies, by start class */
    uint64_t frames_by_nib[N48_MIB_NIB];      /* judged frames, by the submission's IB count */
    uint64_t policy_ns[N48_MIB_NIB];          /* gfxsrc_policy's wall time, nanoseconds, by nib */
    uint64_t policy_runs[N48_MIB_NIB];        /* ... and how many runs that is */
    uint64_t commits[N48_MIB_NIB];            /* frames the COMMIT gate answered yes for, by nib */
    uint64_t f828_answer[N48_F828_REASONS];   /* the frame's FINAL segment's n48_f828_find answer, by reason */
    uint64_t f828_offered;                    /* final segments the census actually looked at */
    /* 0.0.430: WHY n48_mib_segment REFUSED, read-only and fed on every MIB segment-stage run. */
    uint64_t seg_zero[N48_MIB_STARTS];        /* IBs the segmenter asked and which named no segment, by start class */
    uint64_t seg_overflow;                    /* frames whose segments did not fit the table (or whose run truncated) */
    uint64_t seg_max;                         /* the largest Sigma segments seen on any one frame (high-water) */
    uint64_t seg_frames;                      /* MIB segment-stage runs the census observed (the denominator) */
} n48_mib0;

/* 0.0.430 — THE SEGMENT STAGE'S REFUSAL REASONS, as one per-run record. The three reasons asked
 * the MIB-0 line to separate are exactly the three ways n48_mib_segment answers 0: an IB the segmenter ASKED which
 * named no segment (counted by that IB's own start class -'s "head / nop-head / mid"), an IB the table had no room
 * to ask at all, and a recogniser return below its own total (the table truncated a segment list). `max_segs` carries
 * the recognisers' own Sigma even on a refusal, so a frame that overflows says HOW BIG it was - the number a later table
 * resize needs - without the caller inferring it from a zero. */
typedef struct {
    uint32_t zero_by_class[N48_MIB_STARTS];   /* IBs asked and found empty, by that IB's n48_mib_start_class */
    uint32_t overflow;                        /* 1 if any IB was not asked for room, or its list truncated */
    uint32_t max_segs;                        /* the recognisers' Sigma t_k over this frame, refusal or not */
} n48_mib_seg_diag;

/* 0.0.430 — WHERE THE SINGLE-IB POLICY'S WALL TIME GOES, as five non-overlapping phases plus the
 * descriptor reads that happen INSIDE `xlat_ns`. Read-only: every value is a pair of uptime reads around work the
 * policy pass already does. `runs` is the number of single-IB policy passes that contributed, so the report can be
 * read as "N us over R runs" and never as a claim about a different nib. */
typedef struct {
    uint64_t setup_ns;    /* entry -> the per-segment loop: bookkeeping, the concatenation memcpy, the recognisers */
    uint64_t xlat_ns;     /* xlat12_ib_translate_draw_ex, every segment of the pass - descriptor reads INCLUDED */
    uint64_t desc_ns;     /* ... of which the descriptor reader (gfxsrc_desc_read) spent this; a subset of xlat_ns */
    uint64_t cons_ns;     /* n48_cp_build_consumer + n48_cp_merge_consumer (the read-set enumeration and its union) */
    uint64_t fence_ns;    /* the owned-slot fence's find/apply and its gate, per segment */
    uint64_t log_ns;      /* the per-segment `xdump:` line (and the prov line when it fires) */
    uint64_t runs;        /* single-IB policy passes that contributed to every counter above */
} n48_mib_pol;

/* 0.0.426 (notes/design/MIB-COMMIT.md binding B2) — THE SEGMENT'S CLIENT VA IN A CONCATENATED FRAME.
 *
 * In the multi-IB commit the IBs are read into ONE buffer at off_k = sum(len_j, j < k) and each IB is translated on its own,
 * so the translator's `ib_va` for a segment must be that IB's own VA plus 4 dwords for each dword the segment starts past
 * its IB's offset. The helper is pure so the kext call site and a host test agree on the one arithmetic that can silently
 * name the WRONG IB's memory: passing IB 1's bytes with IB 0's VA would point a descriptor or a record at the wrong client
 * page. `from >= off` always holds (a segment of IB k lies inside IB k's range), and the subtraction is unsigned for exactly
 * that reason - a `from < off` would be a caller bug the arithmetic must not paper over with a wrap. */
static inline uint64_t n48_mib_seg_va(uint64_t ib_va, uint32_t from, uint32_t off)
{
    return ib_va + 4ull * (uint64_t)(from - off);
}

/* 0.0.426 (MIB-COMMIT B1) — HOW MANY DWORDS OF IB k THE CONCATENATED BUFFER CAN HOLD.
 *
 * IB k lands at off_k = sum(len_j, j < k) with want = min(len_k, kXdIbDwords - off_k). When the frame's total exceeds the
 * buffer, the read comes back short and the verdict answers IB_SHORT with that IB's index (B1's `detail k`) - never a
 * silent truncation of an IB whose bytes would then be a mix. `n48_mib_want` is that min and it is pure so the call site
 * and the T8 host test agree on the arithmetic: off >= cap gives 0 (nothing fits), and a len that fits gives len.
 * A caller that dropped the cap would read len dwords, overrun the buffer and hide the short read - exactly what T8
 * plants. */
static inline uint32_t n48_mib_want(uint32_t len, uint32_t off, uint32_t cap)
{
    const uint32_t room = off < cap ? cap - off : 0u;
    return len < room ? len : room;
}

/* 0.0.432 (decide36b's 52 nop-head zero-segment exits) — A DISGUISED HEAD.
 *
 * MIB-COMMIT and n48_mib_start_class already know an IB k >= 1 can open with `N48_MIB_NOP_HEAD`, the 10-dword
 * PACKET3(NOP, 8) "over the EVENT_WRITE 0x16 + ACQUIRE_MEM". OVER is exact, not approximate: measured on arm32's F21
 * (this file's own real capture, notes/logs/runs/arm32/capture.bin via gen-mib-fixture.py), Apple rewrites ONLY dword
 * 0 - from XLAT12_SEG_HEAD0 to the NOP header - and leaves dwords 1..9 (the EVENT_WRITE's event index and the whole
 * ACQUIRE_MEM body) and dwords 10/11 (the trailing EVENT_WRITE 0xE that closes a head) BYTE FOR BYTE UNCHANGED. So a
 * NOP-headed IB's first 12 dwords are seg_start_at's own five-dword signature with exactly ONE dword disguised, and
 * the ten dwords "under" the NOP are not opaque padding - they are the real ACQUIRE_MEM + EVENT_WRITE that a clean
 * head would have run, elided only because the CP processor does not need to execute a barrier the previous IB's own
 * trailer already covered (MIB-COMMIT open question 6, still unresolved on hardware; this rule does not depend on
 * the answer - the bytes are provably a valid ACQUIRE_MEM+EVENT_WRITE pair either way).
 *
 * `xlat12_ib_segments` can never recognise this position: its `seg_start_at` requires dword 0 itself to read
 * XLAT12_SEG_HEAD0, unconditionally. `n48_mib_seg_disguised_head` is the ONE new check this brief adds, and it
 * accepts the disguise ONLY when every one of these holds, decoded exactly as xlat12_ib.c's `plen_at` decodes any
 * PM4 type-3 header (type bits 31:30, count bits 29:16, opcode bits 15:8 - cited by content, not copied):
 *   - dword 0 is type-3 and its opcode is NOP (0x10, xlat12_ib.c's own `OP_NOP`) - a non-NOP opcode at dword 0 is
 *     refused here exactly as it always was (s_k stays 0, unchanged);
 *   - its declared length is EXACTLY 10 - the one length that covers EVENT_WRITE(2 dw) + ACQUIRE_MEM(8 dw) and
 *     nothing more or less; any other length is refused, so this is "this EXACT disguise", never "any NOP";
 *   - that whole 10-dword body lies inside the IB (checked BEFORE the length-10 test, so it is not merely implied by
 *     it: a NOP whose declared body would run past the IB is refused on its own rung, never read past the end);
 *   - dwords 1, 2, 10 and 11 are EXACTLY XLAT12_SEG_HEAD1 / XLAT12_SEG_ACQUIRE / XLAT12_SEG_HEAD0 /
 *     XLAT12_SEG_EVENT_E - seg_start_at's own four remaining checks, unmodified.
 *
 * When it matches, the segment this rule reports is IDENTICAL in shape to what seg_start_at would have produced had
 * dword 0 truly been XLAT12_SEG_HEAD0: head = the IB's own offset, start = head + 2 (xlat12_ib_segments' own
 * convention, unchanged). That is the choice this brief asks for by name - "part of the first segment" over "an
 * explicit inert prefix" - and it is chosen because it is the ONLY one that leaves gfx_commit.h's UNMODIFIED checks
 * meaningful: the whole-frame tiling loop requires `s->head == at` with no gap, and the ENCODER kind rung requires
 * `s->start == s->head + 2` exactly; any head other than the IB's own offset fails the first as a coverage hole, and
 * any start other than head+2 fails the second as a corrupted kind - both would be the gate accepting something it
 * did not really check, not merely refusing more strictly. Putting the ten disguised dwords inside the first
 * segment's own [start, end) is not a fiction either: they are real, walkable ACQUIRE_MEM + EVENT_WRITE packets, so
 * a translator that later walks this span sees exactly the same bytes a clean head would have handed it.
 *
 * What follows the disguised head (if anything) is walked by `xlat12_ib_seg_probe` (already exported by xlat12_ib.h,
 * unmodified) from dword 2 - the real ACQUIRE_MEM's own header, a position seg_start_at never needed to be fooled
 * about - which either finds no further head (this IB is one segment, and the probe's own walk must reach the IB's
 * end cleanly or the whole disguise is refused: a truncated walk is never silently accepted as "no more segments"),
 * or finds the next segment's real, UNDISGUISED head, from which the ordinary `xlat12_ib_segments` takes over. */
/* build 0.0.505 (notes/design/CROSS-IB.md, owed item (6)): `exact` 1 replaces the length-10 rung with
 * `h == N48_MIB_NOP_HEAD` - the one dword Apple writes, 0xC0081000, predicate and shader-type bits included (a header
 * with the same opcode and count but any other low bit is refused). Only switch 69's path passes 1 (n48_mib_segment
 * with `xib` != 0, for IB 0 AND for IB k >= 1); `exact` 0 is the 0.0.504 rule, byte for byte, so switch 69 OFF is
 * unchanged. h == 0xC0081000 implies noplen == 10, so the exact rung IS the length-10 rung, tightened. */
static inline int n48_mib_seg_disguised_head_x(const uint32_t *ib, uint32_t off, uint32_t n, uint32_t exact)
{
    if (n < 1u) return 0;                                      /* need dword 0 just to decode a header at all */
    const uint32_t h = ib[off];
    if (((h >> 30) & 3u) != 3u) return 0;                      /* not a type-3 packet: never a NOP at all */
    if (((h >> 8) & 0xFFu) != 0x10u) return 0;                 /* not opcode NOP (xlat12_ib.c's OP_NOP == 0x10) */
    const uint32_t noplen = ((h >> 16) & 0x3FFFu) + 2u;        /* xlat12_ib.c's plen_at formula, by content */
    /* THE BOUNDS GUARD: the NOP's own declared body (`noplen` dwords) AND the two-dword trailing EVENT_WRITE this
     * rule must read right after it both have to lie inside the IB - checked with ARITHMETIC ONLY, before any dword
     * past `ib[off]` itself is read. A NOP that claims more than the IB holds is refused HERE, never read past `n`;
     * this is not merely implied by the length-10 check below, because that check has not run yet when this one
     * decides (a NOP whose declared length happens to equal 10 but whose IB is shorter than 12 dwords is refused by
     * THIS rung, independent of the one after it). */
    if ((uint64_t)noplen + 2u > (uint64_t)n) return 0;
    if (ib[off + 1u] != XLAT12_SEG_HEAD1 || ib[off + 2u] != XLAT12_SEG_ACQUIRE) return 0;
    /* Only a NOP of EXACTLY 10 dwords - EVENT_WRITE(2 dw) + ACQUIRE_MEM(8 dw), the one combination seg_start_at's
     * own pattern is built from - is the disguise this rule recognises. A shorter or longer NOP could still
     * coincidentally carry XLAT12_SEG_HEAD1/XLAT12_SEG_ACQUIRE at dwords 1/2 (just checked) without genuinely being
     * that pair's body, so this is a THIRD, independent guard, not a restatement of the two above. */
    if (exact ? h != N48_MIB_NOP_HEAD : noplen != 10u) return 0;
    return ib[off + noplen] == XLAT12_SEG_HEAD0 && ib[off + noplen + 1u] == XLAT12_SEG_EVENT_E;
}
static inline int n48_mib_seg_disguised_head(const uint32_t *ib, uint32_t off, uint32_t n)
{
    return n48_mib_seg_disguised_head_x(ib, off, n, 0u);
}

/* Mirrors xlat12_ib.c's `plen_at` + `is_draw` over `[from, to)` of `ib` (both GLOBAL indices into the concatenated
 * buffer), counting PM4 type-3 draw packets exactly as xlat12_ib_segments' own inner loop does (by content: the same
 * five DRAW_INDEX_* / DRAW_INDIRECT* opcodes as xlat12_ib.c's `is_draw`). Used ONLY for the disguised head's own
 * segment, whose [start, end) genuinely holds real packets (the comment above) rather than opaque padding, so this is
 * not a guess - it is the same walk the plain recogniser would have run had dword 0 not been disguised. A malformed
 * header or a packet that would run past `to` stops the count at that point rather than reading past it, which can
 * only ever UNDER-count draws, never read outside `[from, to)`. `*draws`/`*draw_at` are GLOBAL; the caller shifts
 * them into the same local-to-`off_k` convention every other segment already uses before the one shared shift loop
 * below adds `off_k` back. */
static inline void n48_mib_seg_draws(const uint32_t *ib, uint32_t from, uint32_t to, uint32_t *draws, uint32_t *draw_at)
{
    uint32_t d = 0u, at = 0u, i = from;
    while (i < to) {
        const uint32_t h = ib[i];
        uint32_t l;
        if (h == 0xFFFF1000u) l = 1u;                          /* XLAT12_IB_NOP (xlat12_ib.h), by value */
        else if (((h >> 30) & 3u) == 2u) l = 1u;                /* TYPE2 */
        else if (((h >> 30) & 3u) != 3u) break;                 /* not a packet: stop, never guess */
        else {
            l = ((h >> 16) & 0x3FFFu) + 2u;
            if ((uint64_t)i + (uint64_t)l > (uint64_t)to) break;
            const uint32_t op = (h >> 8) & 0xFFu;
            /* xlat12_ib.c's is_draw: OP_DRAW_INDEX_AUTO 0x2D, OP_DRAW_INDEX_2 0x27, OP_DRAW_INDEX_OFFSET_2 0x35,
             * OP_DRAW_INDIRECT 0x24, OP_DRAW_INDEX_INDIRECT 0x25 - cited by content, not copied. */
            if (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) { d = d + 1u; at = i; }
        }
        i += l;
    }
    if (draws) *draws = d;
    if (draw_at) *draw_at = at;
}

/* =====================================================================================================================
 * build 0.0.505 (notes/design/CROSS-IB.md Q4 C1; switch 69, DEFAULT OFF, requires 36) — THE CROSS-IB RULES.
 *
 * `xib` is switch 69's M, latched once per pass: bit 0 (N48_MIB_XIB_DISG0) offers the disguised head to IB 0 as well
 * (CROSS-IB Q1: IB 0 opens NOP-HEAD in 12 of 43 captured multi-IB zero-segment frames, so the "no previous IB" premise
 * below was wrong); bit 1 (N48_MIB_XIB_LEAD) gives an IB k >= 1 that opens MID one LEAD segment [off_k, first head),
 * translated from its first dword (start == head, the HEADLESS shape's tightness inside an ENCODER frame: Apple filled
 * its 64 KiB chunk and replayed the draw's whole prologue in the next chunk without the head, CROSS-IB Q1). 0 is the
 * 0.0.504 segment stage, byte for byte. */
#define N48_MIB_XIB_DISG0 0x1u
#define N48_MIB_XIB_LEAD  0x2u
#define N48_MIB_XIB_MASK  0x3u

/* The LEAD rule for ONE IB, over its own slice ib[off, off + n) (the caller has already found that the plain recogniser
 * and the disguise named nothing). Writes seg[0] = the lead {head 0, start 0, end fa} and seg[1..] = what
 * xlat12_ib_segments finds from the first head, all LOCAL to `off`; returns the row count and *t the recognisers'
 * total (equal on success), or 0 with *t 0 on any refusal. Refuses unless ALL of:
 *   - the IB's start class is MID (never HEAD, NOP-HEAD or UNREAD);
 *   - xlat12_ib_seg_probe's first packet-aligned head `fa` equals its first RAW match `fr` (no head hides in a payload),
 *     and, when there is no head at all (both XLAT12_SEG_NONE), the packet walk covered the IB exactly (`w == n`);
 *   - the lead [0, fa) holds at least one draw (n48_mib_seg_draws; the translator would refuse a 0-draw segment anyway);
 *   - the rest [fa, n), when there is one, tiles through xlat12_ib_segments with s == t, room in the table, and its
 *     last row ending at the IB's own end (xlat12_ib_segments ends its last row where its walk stopped).
 * Pure: reads ib, writes only seg[0 .. room) and *t. */
static inline uint32_t n48_mib_lead_try(const uint32_t *ib, uint32_t off, uint32_t n, xlat12_ib_segment *seg,
                                        uint32_t room, uint32_t *t)
{
    if (t) *t = 0u;
    if (!ib || !seg || !t || n == 0u || room == 0u) return 0u;
    if (n48_mib_start_class(&ib[off], n) != N48_MIB_START_MID) return 0u;
    uint32_t fa = 0u, fr = 0u, w = 0u;
    (void)xlat12_ib_seg_probe(&ib[off], n, &fa, &fr, &w);
    if (fa != fr) return 0u;                                  /* a raw head that is not packet-aligned: refuse */
    if (fa == XLAT12_SEG_NONE && w != n) return 0u;           /* no head, and the walk stopped short of the IB's end */
    const uint32_t end = (fa == XLAT12_SEG_NONE) ? n : fa;
    if (end == 0u) return 0u;                                 /* an empty lead (never MID; kept as its own rung) */
    uint32_t d = 0u, at = 0u;
    n48_mib_seg_draws(ib, off, off + end, &d, &at);
    if (d == 0u) return 0u;                                   /* a draw-less prefix: refuse */
    seg[0].head = 0u; seg[0].start = 0u; seg[0].end = end;
    seg[0].draws = d; seg[0].draw_at = (at >= off) ? (at - off) : 0u;
    uint32_t s = 1u, tt = 1u;
    if (end < n) {
        uint32_t mt = 0u;
        const uint32_t ms = (room > 1u) ? xlat12_ib_segments(&ib[off + end], n - end, &seg[1], room - 1u, &mt) : 0u;
        if (!ms || ms != mt) return 0u;
        if (seg[ms].end != n - end) return 0u;               /* the rest's walk stopped short of the IB's end */
        for (uint32_t j = 0u; j < ms; j++) { seg[1u + j].head += end; seg[1u + j].start += end; seg[1u + j].end += end; }
        s += ms; tt += mt;
    }
    *t = tt;
    return s;
}

/* Is the segment whose head is `head` (GLOBAL) a lead? Bit k of `lead_mask` names IB k, and IB k's lead is the one
 * segment whose head is IB k's own offset. `ib_off` may be null only with `lead_mask` 0. */
static inline uint32_t n48_mib_is_lead(uint32_t lead_mask, const uint32_t *ib_off, uint32_t nib, uint32_t head)
{
    if (!lead_mask || !ib_off) return 0u;
    for (uint32_t k = 0u; k < nib && k < 32u; k++)
        if (((lead_mask >> k) & 1u) && head == ib_off[k]) return 1u;
    return 0u;
}

/* 0.0.427 ( condition (3)) — THE MULTI-IB SEGMENT STAGE, AS ONE PURE FUNCTION.
 *
 * Through 0.0.426 gfxsrc_policy walked the concatenated frame's IBs inline. It is here so the kext and a host test run
 * the SAME code, and so the two things that can silently name the wrong memory - a wrong `ib_off`/`ib_n` and a wrong
 * `ib_va` - are exercised over REAL captured bodies instead of only reviewed.
 *
 * The frame's IBs are concatenated in `ib`: IB k begins at `ib_off[k]` and is `ib_n[k]` declared dwords long (B1). Each
 * IB is segmented on its OWN slice, `&ib[ib_off[k]]`, so NO segment can ever span an IB boundary (B2/H2); every segment
 * head/start/end is then shifted into the concatenated buffer's global index space. `ib_nseg[k]` receives how many
 * segments IB k produced. `draws`/`draw_at` are left as the recogniser returned them (local to that IB), exactly as the
 * inline 0.0.426 loop left them; nothing in the commit path reads them.
 *
 * 0.0.432: for IB k >= 1 ONLY, when the plain recogniser above found NOTHING AT ALL (`s_k == t_k == 0`,
 * never a table that already truncated), `n48_mib_seg_disguised_head` gets exactly one attempt to explain the zero as
 * a disguised head rather than a real MID/UNREAD start. Success REPLACES `s_k`/`t_k` with the disguised segment plus
 * whatever `xlat12_ib_segments` finds after it (both already converted to the SAME local-to-`off_k` convention the
 * plain path uses, so the one shift loop below is untouched); any failure along the way - the disguise itself, or
 * what follows it not tiling cleanly to the IB's end - leaves `s_k`/`t_k` exactly as the plain call left them, so a
 * frame this rule cannot explain is refused for the SAME reason it was refused before this brief. IB 0 is never
 * offered this rule (MIB-COMMIT: the disguise only appears where a LATER IB's head coincides with the previous
 * IB's own covered barrier - an IB 0 has no previous IB to elide against). build 0.0.505: CONTRADICTED by the data
 * (CROSS-IB.md Q1, correction 1: IB 0 opens NOP-HEAD in 12 of run10p's 43 captured multi-IB zero-segment frames); switch
 * 69 bit 0 offers it to IB 0 (the safety argument is's, unchanged: the translator writes only from `start`).
 *
 * Returns the total number of segments on success, else 0. A refusal is: `nib` 0 or a null/absent table; ANY IB produced
 * no segments; ANY IB's recogniser found more segments than it could fill (`s_k != t_k`, the table truncated - 0.0.426's
 * `s_k == 0 || s_k != t_k`); or the table cannot hold every segment (the truncation shows up as `s_k != t_k` on the IB
 * that overflows). `*totalOut` is the recognisers' own total and is set to 0 on refusal. Pure: reads `ib`, writes only
 * `segs[]`, `ib_nseg[]`, `*totalOut` and (0.0.430,(a)) `*diagOut`, which records WHY a refusal happened - the
 * zero-segment IB's own start class, whether the table overflowed, and the recognisers' Sigma even when it did not fit. */
/* build 0.0.505 (CROSS-IB C1): `xib` (switch 69's latched M, N48_MIB_XIB_*) and `lead_mask`. `xib` 0 is the
 * 0.0.504 stage exactly (the IB-0 premise above and the length-10 disguise rung). Bit 0 offers the disguise to k = 0
 * too; any non-zero `xib` asks the disguise for `h == N48_MIB_NOP_HEAD` exactly. Bit 1, for k >= 1 ONLY and
 * only when the plain call and the disguise both found nothing, applies n48_mib_lead_try; its rows are shifted like
 * every row and bit k of *lead_mask is set. Any failure leaves s_k/t_k as the plain call left them, so the census
 * below counts a failed lead by its class exactly as before. *lead_mask names the IBs whose lead FORMED, on a refused
 * frame too (the census reads it); the policy hands it to the gate only with a non-zero answer. */
static inline uint32_t n48_mib_segment(const uint32_t *ib, uint32_t nib,
                                       const uint32_t *ib_off, const uint32_t *ib_n,
                                       xlat12_ib_segment *segs, uint32_t maxSegs,
                                       uint32_t *ib_nseg, uint32_t *totalOut,
                                       n48_mib_seg_diag *diagOut, uint32_t xib, uint32_t *lead_mask)
{
    if (totalOut) *totalOut = 0u;
    if (diagOut) *diagOut = n48_mib_seg_diag {};
    if (lead_mask) *lead_mask = 0u;
    if (!ib || !ib_off || !ib_n || !segs || !ib_nseg || nib == 0u || maxSegs == 0u) return 0u;
    uint32_t ns = 0u, tot = 0u, ok = 1u, lm = 0u;
    for (uint32_t k = 0; k < nib; k++) {
        const uint32_t off_k = ib_off[k];
        const uint32_t n_k = ib_n[k];
        uint32_t s_k = 0u, t_k = 0u, asked = 0u;
        if (n_k && ns < maxSegs) {
            asked = 1u;
            s_k = xlat12_ib_segments(&ib[off_k], n_k, &segs[ns], maxSegs - ns, &t_k);
            // 0.0.432: THE ONE NOP-SKIP ATTEMPT — k >= 1, the plain call found nothing, and there is
            // still room to write the disguised segment. build 0.0.505: k = 0 as well under switch 69 bit 0.
            if ((k > 0u || (xib & N48_MIB_XIB_DISG0)) && s_k == 0u && t_k == 0u && ns < maxSegs &&
                n48_mib_seg_disguised_head_x(ib, off_k, n_k, xib ? 1u : 0u)) {
                uint32_t first_at = 0u, first_raw = 0u, walked = 0u;
                (void)xlat12_ib_seg_probe(&ib[off_k + 2u], n_k - 2u, &first_at, &first_raw, &walked);
                uint32_t localEnd = 0u, disguiseOk = 0u;
                if (first_at == XLAT12_SEG_NONE) {
                    if (walked == n_k - 2u) { localEnd = n_k; disguiseOk = 1u; }   /* the probe's walk reached the IB's own end */
                } else {
                    localEnd = 2u + first_at; disguiseOk = 1u;                    /* the next IB's own real, undisguised head */
                }
                if (disguiseOk) {
                    uint32_t s2 = 1u, t2 = 1u;
                    uint32_t dGlobal = 0u, atGlobal = 0u;
                    segs[ns].head = 0u; segs[ns].start = 2u; segs[ns].end = localEnd;
                    n48_mib_seg_draws(ib, off_k + 2u, off_k + localEnd, &dGlobal, &atGlobal);
                    segs[ns].draws = dGlobal;
                    segs[ns].draw_at = (atGlobal >= off_k) ? (atGlobal - off_k) : 0u;
                    if (localEnd < n_k) {
                        const uint32_t restOff = off_k + localEnd, restN = n_k - localEnd;
                        uint32_t moreTot = 0u;
                        const uint32_t moreS = (ns + 1u < maxSegs)
                            ? xlat12_ib_segments(&ib[restOff], restN, &segs[ns + 1u], maxSegs - ns - 1u, &moreTot) : 0u;
                        if (moreS && moreS == moreTot) {
                            for (uint32_t j = 0u; j < moreS; j++) {
                                segs[ns + 1u + j].head  += localEnd;
                                segs[ns + 1u + j].start += localEnd;
                                segs[ns + 1u + j].end   += localEnd;
                            }
                            s2 += moreS; t2 += moreTot;
                        } else {
                            disguiseOk = 0u;   /* what follows does not itself tile: refuse, exactly as before this brief */
                        }
                    }
                    if (disguiseOk) { s_k = s2; t_k = t2; }
                }
            }
            // build 0.0.505 (CROSS-IB C1): THE LEAD — k >= 1 only (a lead on IB 0 would inherit the previous
            // submission's state), switch 69 bit 1, and only when the plain call and the disguise both found nothing.
            if (k > 0u && (xib & N48_MIB_XIB_LEAD) && s_k == 0u && t_k == 0u && ns < maxSegs) {
                uint32_t lt = 0u;
                const uint32_t ls = n48_mib_lead_try(ib, off_k, n_k, &segs[ns], maxSegs - ns, &lt);
                if (ls && ls == lt) { s_k = ls; t_k = lt; lm |= 1u << k; }
            }
        }
        // 0.0.430: THE CENSUS REASONS, one decision each. An IB the table had no room for is an
        // OVERFLOW, never a zero-segment class - the recogniser was never asked, so no start class can be blamed. An IB
        // the recogniser DID ask and which named no segment is counted by its own start class (the head/nop-head/mid
        // split asked for). A return below the recognisers' own total is the truncation a full table caused; both
        // are the same overflow so the line has one reason for "the table was too small".
        if (diagOut) {
            if (n_k && !asked) diagOut->overflow = 1u;
            if (asked && s_k != t_k) diagOut->overflow = 1u;
            if (asked && s_k == 0u && t_k == 0u)
                diagOut->zero_by_class[n48_mib_start_class(&ib[off_k], n_k)]++;
        }
        for (uint32_t j = 0; j < s_k; j++) {
            segs[ns + j].head  += off_k;
            segs[ns + j].start += off_k;
            segs[ns + j].end   += off_k;
        }
        ib_nseg[k] = s_k;
        ns += s_k; tot += t_k;
        if (s_k == 0u || s_k != t_k) ok = 0u;   /* ns 0 in ANY IB answers segment-policy (B2) */
    }
    if (diagOut && tot > diagOut->max_segs) diagOut->max_segs = tot;   /* the recognisers' Sigma, refusal or not */
    if (lead_mask) *lead_mask = lm;
    if (!ok || ns == 0u) { if (totalOut) *totalOut = 0u; return 0u; }
    if (totalOut) *totalOut = tot;
    return ns;
}

/* =====================================================================================================================
 * build 0.0.480 (notes/design/CONTINUATION-UNITS.md Q1 and Q11 contract item C1; switch 55) — CONTINUATION UNITS.
 *
 * A segment CONTINUES the one before it when it draws into the colour target an earlier segment of the SAME IB set up:
 * a packet walk of [start, first draw) finds no SET_CONTEXT_REG covering gfx10 CB_COLOR0_VIEW (0x28c6c, context dword
 * 0x31b); a segment with no draw walks to its end. The walk is strict: a header that is not TYPE2/TYPE3, or a packet
 * running past the segment's end, answers 0 ("not a continuation"), which leaves the segment exactly as it is today.
 * The draw opcodes are xlat12_ib.c's `is_draw` set (cited by content: 0x2D, 0x27, 0x35, 0x24, 0x25), the same set
 * n48_mib_seg_draws counts. */
#define N48_MIB_CB0_VIEW_CTX 0x31Bu   /* gfx10 CB_COLOR0_VIEW 0x28c6c = context dword 0xa000 + 0x31b */
static inline int n48_mib_seg_continues(const uint32_t *ib, uint32_t from, uint32_t to)
{
    uint32_t i = from;
    while (i < to) {
        const uint32_t h = ib[i];
        if (h == 0xFFFF1000u || ((h >> 30) & 3u) == 2u) { i++; continue; }
        if (((h >> 30) & 3u) != 3u) return 0;
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u, op = (h >> 8) & 0xFFu;
        if ((uint64_t)i + (uint64_t)l > (uint64_t)to) return 0;
        if (op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) return 1;   /* reached its first draw */
        if (op == 0x69u && l >= 3u) {
            const uint32_t off = ib[i + 1u] & 0xFFFFu;
            if (N48_MIB_CB0_VIEW_CTX >= off && N48_MIB_CB0_VIEW_CTX < off + (l - 2u)) return 0;   /* it sets its own VIEW */
        }
        i += l;
    }
    return 1;
}

/* THE UNITS. `segs[0..ns)` is the segment stage's answer (global offsets, IB order); `ib_off`/`ib_n` the IBs (a
 * single-IB frame passes {0} / {n}). A unit = one segment that does not continue (or is its IB's first) plus every
 * continuation that follows it IN THE SAME IB, at most `maxCons` constituents (a continuation past the cap starts its
 * own unit and translates alone, as today). Unit j: head = its first constituent's head, start = head + 2 (the
 * ENCODER kind's rule: the first constituent's own start), end = its last constituent's end, draws the sum; its
 * constituents are segs[cfirst[j] .. cfirst[j] + ccount[j]). NO unit crosses an IB boundary: the owning IB is
 * compared for every merge. Returns the unit count (0 on bad arguments or a table too small). Pure. */
static inline uint32_t n48_mib_ib_of(const uint32_t *ib_off, const uint32_t *ib_n, uint32_t nib, uint32_t at)
{
    for (uint32_t k = 0; k < nib; k++) if (at >= ib_off[k] && at < ib_off[k] + ib_n[k]) return k;
    return 0xFFFFFFFFu;
}
/* build 0.0.505 (CROSS-IB C2): `lead_mask` is the segment stage's (0 with switch 69 OFF). A unit whose FIRST
 * constituent is a lead (n48_mib_is_lead: IB k's own offset, bit k set) starts at its head - translated from its first
 * dword, no Apple dword surviving - never head + 2. Every other unit keeps its first constituent's own start (0.0.504's
 * rule, byte for byte). A lead is always its IB's first segment, so it can never join a unit of an earlier IB. */
static inline uint32_t n48_mib_units(const uint32_t *ib, uint32_t nib, const uint32_t *ib_off, const uint32_t *ib_n,
                                     const xlat12_ib_segment *segs, uint32_t ns, uint32_t maxCons,
                                     xlat12_ib_segment *units, uint32_t *cfirst, uint32_t *ccount, uint32_t maxUnits,
                                     uint32_t lead_mask)
{
    if (!ib || !ib_off || !ib_n || !segs || !units || !cfirst || !ccount || nib == 0u || ns == 0u || maxCons == 0u) return 0u;
    uint32_t nu = 0u, prevIb = 0xFFFFFFFFu;
    for (uint32_t k = 0; k < ns; k++) {
        const uint32_t ibk = n48_mib_ib_of(ib_off, ib_n, nib, segs[k].start);
        if (ibk == 0xFFFFFFFFu || segs[k].end > ib_off[ibk] + ib_n[ibk]) return 0u;   /* a segment outside its IB */
        const int join = nu > 0u && ibk == prevIb && ccount[nu - 1u] < maxCons &&
                         n48_mib_seg_continues(ib, segs[k].start, segs[k].end);
        if (join) {
            units[nu - 1u].end = segs[k].end;
            units[nu - 1u].draws += segs[k].draws;
            ccount[nu - 1u]++;
        } else {
            if (nu >= maxUnits) return 0u;
            units[nu].head = segs[k].head; units[nu].end = segs[k].end;
            units[nu].start = n48_mib_is_lead(lead_mask, ib_off, nib, segs[k].head) ? segs[k].head : segs[k].start;
            units[nu].draws = segs[k].draws; units[nu].draw_at = segs[k].draw_at;
            cfirst[nu] = k; ccount[nu] = 1u;
            nu++;
        }
        prevIb = ibk;
    }
    return nu;
}

/* C2's one policy decision, shared by the kext (gfxsrc_unit_setup) and its host test: a unit is translated with
 * XLAT12_EXTRA_UNIT only when it has TWO OR MORE constituents (and no more than the translator's table holds). A single
 * segment is handed to the translator exactly as with switch 55 OFF - the ON-identity contract. */
static inline int n48_mib_unit_flag(uint32_t ccount)
{
    return ccount >= 2u && ccount <= XLAT12_UNIT_CONS_MAX;
}

/* P5 (design Q5, CONFIRMED there): switch 49 may credit the ACQUIRE_MEM at a segment's `start` only when it EXECUTES,
 * i.e. when the head at `head` is the real two-dword EVENT_WRITE (XLAT12_SEG_HEAD0, XLAT12_SEG_HEAD1) that the CP runs
 * before it. A NOP-disguised head (N48_MIB_NOP_HEAD) wraps that ACQUIRE_MEM in its body, so it never runs; any other
 * head dword is not positive evidence either. 1 = executes. */
static inline int n48_mib_head_executes(const uint32_t *ib, uint32_t head, uint32_t n)
{
    if (!ib || head + 2u > n) return 0;
    return ib[head] == XLAT12_SEG_HEAD0 && ib[head + 1u] == XLAT12_SEG_HEAD1;
}
/* build 0.0.537 (switch 93, xlat12_ib.h XLAT12_EXTRA_PWS) — THE HEAD GATE: 1 when the translation of segment/unit `s` starts at a
 * packet the CP runs - its head is the real EVENT_WRITE (n48_mib_head_executes: the segment starts at head + 2, right after it), or
 * it starts AT its own head (a lead, a headless segment: the IB's own first dword). 0 for Apple's NOP-disguised head, whose body holds
 * the ACQUIRE_MEM the translation starts at: the CP skips that body, so a converted pair there would lose its release and keep its
 * acquire. Only a segment this answers 1 for may carry XLAT12_EXTRA_PWS (gfxsrc_policy). Pure. */
static inline int n48_mib_start_runs(const uint32_t *ib, const xlat12_ib_segment *s, uint32_t n)
{
    return s && (n48_mib_head_executes(ib, s->head, n) || s->start == s->head);
}

/* =====================================================================================================================
 * build 0.0.481 (the review of 0.0.480: F2, F3 and the singles recommendation) - the unit path's
 * per-segment steps, PURE, so the host test (tests/gfx_mib_units_checks.h run_frame) runs the kext's OWN code in the
 * kext's order: segment stage, n48_mib_nseg_of, n48_mib_units, n48_mib_unit_setup, translate, n48_mib_retry_wanted +
 * n48_mib_retry_single (switch 56), the copy guard, the fence slice, the frame-local feed, n48_mib_unit_undo, the gate.
 *
 * F2 (mechanism CONFIRMED there): THE COUNT A ONE-SEGMENT RULE READS. gfx_fillset.h n48_fs_identify_fill (
 * K2: `nseg == 1` AND the ColorFill PS) and gfx_cp_build.h n48_cp_build_input_free (`nseg == 1`) are rules about the
 * SEGMENT STAGE's answer: gXdBuild.fill is sticky over the whole frame, and with switch 55 a single-IB frame of several
 * segments can become ONE unit, so the unit count would let a frame carrying one ColorFill draw among other segments
 * pass as a fill (K2's own failure). The kext records this count BEFORE the unit stage replaces segs[] (gXdBuild.nsegPre)
 * and both rules read it. With 55 OFF, or when no unit is formed, it is exactly gXdBuild.nseg (the same expression over
 * the same ns/total). */
static inline uint32_t n48_mib_nseg_of(uint32_t ns, uint32_t total, uint32_t maxSegs)
{
    return (ns == total && ns <= maxSegs) ? ns : 0u;
}

/* The caller-owned unit state for ONE translation (0.0.480's gfxsrc_unit_setup body, moved here unchanged but for F3).
 * F3: the pool journal `journal->jn` is emptied FIRST, before any return. The translator empties it itself only
 * in d_unit_reset, which runs AFTER translate_draw_ex's early returns (ERR_ARG, RING, TRUNCATED, DRAW_SHAPE for 0 or more
 * than XLAT12_MAX_DRAWS draws ...); a unit refused there would otherwise leave the PREVIOUS unit's placements in the
 * journal, and n48_mib_unit_undo would take back records an earlier TRANSLATED unit still points at. Called for EVERY
 * segment of a switch-55 pass (a single with `ncons` 0 sets nothing else up: its translation is exactly 55 OFF's).
 * `ncons` 1..XLAT12_UNIT_CONS_MAX sets XLAT12_EXTRA_UNIT over `cons[0..ncons)` (the constituent segments; heads relative
 * to `from`); `use_pool` 0 = the unit's own leftover only. Returns 1 when the unit flag was set. */
static inline uint32_t n48_mib_unit_setup(xlat12_draw_extra *ex, xlat12_unit *U, xlat12_pool *journal, int use_pool,
                                          uint32_t ncons, const xlat12_ib_segment *cons, uint32_t from,
                                          xlat12_unit_cons_fn fn, void *ctx)
{
    if (journal) journal->jn = 0u;   /* F3: the journal names THIS segment's placements only, from here on */
    if (U && U->spill) U->spill->jn = 0u;   /* build 0.0.522 (switch 76): F3 for the spill tier's journal too */
    if (!ex || !U || !cons || ncons == 0u || ncons > XLAT12_UNIT_CONS_MAX) return 0u;
    ex->flags |= XLAT12_EXTRA_UNIT;
    ex->unit = U;
    U->ncons = ncons;
    for (uint32_t j = 0; j < ncons; j++) U->cons_in[j] = j ? cons[j].head - from : 0u;
    U->pool = use_pool ? journal : 0;
    U->cons_fn = fn;
    U->cons_ctx = ctx;
    return 1u;
}

/* The after-step's undo (0.0.480's gfxsrc_unit_after, first statement): a segment translated through the unit path - a
 * unit, or a single RETRIED through it by switch 56 (`via_unit` 1 or 2) - whose FINAL status is a refusal (the
 * translator's own, or the copy guard's after it translated) takes back every record it placed in an EARLIER segment's
 * NOP run; the policy's restore covers only its own bytes. Returns the dwords restored. */
static inline uint32_t n48_mib_unit_undo(uint32_t via_unit, uint32_t st, xlat12_pool *pool)
{
    return (via_unit && st) ? xlat12_pool_undo(pool) : 0u;
}
/* build 0.0.522: the same after-step undo for BOTH tiers - the frame pool and the unit's spill
 * tier (U->spill, NULL = none: exactly n48_mib_unit_undo). Every kext site that takes back a refused unit's pool records calls
 * this instead, so a spill record can never outlive the refusal that made it (planted break: undo skipped). */
static inline uint32_t n48_mib_unit_undo2(uint32_t via_unit, uint32_t st, xlat12_pool *pool, const xlat12_unit *U)
{
    const uint32_t a = n48_mib_unit_undo(via_unit, st, pool);
    return a + ((U && U->spill) ? n48_mib_unit_undo(via_unit, st, U->spill) : 0u);
}

/* build 0.0.481 — SWITCH 56 ( verdict (D), "RETRY ONLY AFTER A ROOM REFUSAL"), DEFAULT OFF, REQUIRES 55.
 * A SINGLE segment - one constituent of the switch-55 unit table this pass formed (`unit_map`), so an ENCODER
 * segmentation and never HEADLESS - whose normal translation refused for ROOM, XLAT12_TDESC_NO_ROOM (0xF8: no pad run
 * carries a table placement) or XLAT12_REEMIT_NO_ROOM (0xFC: a re-emission did not fit its region), is translated ONCE
 * MORE as a one-constituent unit: compaction, deferred records, the frame pool. Every other single (translated, or
 * refused for any other reason - PROVENANCE, PAIR, SLOT_*, a TOO_LONG at any other site, the copy guard) and every unit is
 * untouched. 55 OFF, 56 OFF, no unit table, a non-ENCODER kind or more than one constituent: 0. */
static inline int n48_mib_retry_wanted(uint32_t on55, uint32_t on56, uint32_t unit_map, uint32_t kind_encoder,
                                       uint32_t ncons, uint32_t st, uint32_t err_op)
{
    if (!on55 || !on56 || !unit_map || !kind_encoder || ncons != 1u) return 0;
    return (st == (uint32_t)XLAT12_IB_ERR_DESC && err_op == (uint32_t)XLAT12_TDESC_NO_ROOM) ||
           (st == (uint32_t)XLAT12_IB_ERR_TOO_LONG && err_op == (uint32_t)XLAT12_REEMIT_NO_ROOM);
}

/* The retry itself. It must FULLY rewrite the segment: the first attempt left a partial output in `out`, so the whole
 * output is first filled with a SENTINEL, and after a translated retry no sentinel may survive in out[0..n) nor have
 * leaked into any dword this retry placed in the frame pool (the journal's ranges). A survivor refuses the retry
 * (XLAT12_IB_ERR_VERIFY, err_op N48_MIB_RETRY_SENTINEL_OP, err_in_dword = the dword, n for a pool dword) and undoes its
 * pool placements; the caller's restore puts Apple's bytes back. An input that itself holds the sentinel word is not
 * retried at all (`why` SENTINEL_IN; the first refusal `st0` and `ds` stand, untouched). `fn` is the translator call
 * (the kext's is xlat12_ib_translate_draw_ex over the m2tri profile); `ds` is re-zeroed and holds the retry's own stats.
 * Returns the segment's status; `why` says what happened (N48_MIB_RETRY_*). */
#define N48_MIB_RETRY_SENTINEL    0x5E471E56u   /* a TYPE-1 header (reserved): never a dword the translator emits */
#define N48_MIB_RETRY_SENTINEL_OP 0xE4u         /* err_op with XLAT12_IB_ERR_VERIFY: a sentinel survived the retry */
enum { N48_MIB_RETRY_NOT = 0u, N48_MIB_RETRY_OK = 1u, N48_MIB_RETRY_REFUSED = 2u, N48_MIB_RETRY_SENTINEL_IN = 3u,
       N48_MIB_RETRY_SENTINEL_LEFT = 4u };
typedef uint32_t (*n48_mib_xlat_fn)(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out,
                                    uint32_t *olen, xlat12_draw_stats *ds);
static inline uint32_t n48_mib_retry_sentinel_at(const uint32_t *out, uint32_t n, const xlat12_pool *journal)
{
    for (uint32_t k = 0; k < n; k++) if (out[k] == N48_MIB_RETRY_SENTINEL) return k;
    for (uint32_t j = 0; journal && j < journal->jn && j < XLAT12_UNIT_PEND_MAX; j++)
        for (uint32_t k = 0; k < journal->j[j].used; k++) if (journal->j[j].host[k] == N48_MIB_RETRY_SENTINEL) return n;
    return 0xFFFFFFFFu;
}
static inline uint32_t n48_mib_retry_single(n48_mib_xlat_fn fn, xlat12_draw_extra *ex, xlat12_unit *U, xlat12_pool *journal,
                                            int use_pool, const xlat12_ib_segment *seg, uint32_t from,
                                            const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen,
                                            xlat12_draw_stats *ds, uint32_t st0, uint32_t *why)
{
    uint32_t w = N48_MIB_RETRY_NOT;
    if (why) *why = w;
    if (!fn || !ex || !U || !seg || !in || !out || !olen || !ds || !why || n == 0u) return st0;
    for (uint32_t k = 0; k < n; k++) if (in[k] == N48_MIB_RETRY_SENTINEL) { *why = N48_MIB_RETRY_SENTINEL_IN; return st0; }
    if (!n48_mib_unit_setup(ex, U, journal, use_pool, 1u, seg, from, 0, 0)) return st0;
    for (uint32_t k = 0; k < n; k++) out[k] = N48_MIB_RETRY_SENTINEL;
    { unsigned char *z = (unsigned char *)ds; for (uint32_t k = 0; k < (uint32_t)sizeof *ds; k++) z[k] = 0u; }
    *olen = 0u;
    const uint32_t st = fn(ex, in, n, out, olen, ds);
    if (st) { *why = N48_MIB_RETRY_REFUSED; return st; }
    uint32_t bad = n48_mib_retry_sentinel_at(out, n, use_pool ? journal : 0);
    /* build 0.0.522 (switch 76): a sentinel in a dword this retry placed in the SPILL tier refuses the same way (dword n) */
    if (bad == 0xFFFFFFFFu && U->spill && n48_mib_retry_sentinel_at(out, 0u, U->spill) != 0xFFFFFFFFu) bad = n;
    if (bad != 0xFFFFFFFFu) {
        if (journal) (void)xlat12_pool_undo(journal);
        if (U->spill) (void)xlat12_pool_undo(U->spill);
        ds->err_op = N48_MIB_RETRY_SENTINEL_OP; ds->err_in_dword = bad; ds->err_reg = 0u;
        *why = N48_MIB_RETRY_SENTINEL_LEFT;
        return (uint32_t)XLAT12_IB_ERR_VERIFY;
    }
    *why = N48_MIB_RETRY_OK;
    return 0u;
}

/* The census label: with switch 55 ON a pass's segment table holds UNITS, so 0.0.474's mibseg-detail
 * line (its nseg, its string, its refused indices) and the WSF line index units, not Apple's segments. Both lines carry
 * this marker whenever the pass formed units, and nothing (byte-identical text) otherwise. */
#ifndef N48_UNITS_MARK
#define N48_UNITS_MARK " units"   /* the SAME text f3_reader.h defines for the WSF line (either header may come first) */
#endif
static inline const char *n48_mib_units_mark(uint32_t unit_map) { return unit_map ? N48_UNITS_MARK : ""; }

/* THE PER-UNIT DECIDE LINE (55 ON): constituents, compaction slack, record dwords in its own space and in the pool,
 * un-redirects, the inline invalidate, the HS pairs patched and the fence slice. Every field is bounded (at most 20
 * digits each), so the widest line is well under the logger's 491-byte body (a host test prints the maximum). */
#define N48_UNIT_FMT "unit480: frame %llu k %u IB %u cons %u st %u op %#x slack %u own %u pool %u placed %u unredir %u " \
                     "inv %u hs %u cb %u fence [%u,%u) %s"
#define N48_UNIT_ARGS(fr, k, ibk, cons, st, op, U, flo, fhi, why) \
    (unsigned long long)(fr), (unsigned)(k), (unsigned)(ibk), (unsigned)(cons), (unsigned)(st), (unsigned)(op), \
    (unsigned)(U)->slack, (unsigned)(U)->own_dw, (unsigned)(U)->pool_dw, (unsigned)(U)->placed, (unsigned)(U)->unredir, \
    (unsigned)(U)->inv_inline, (unsigned)(U)->hs_patched, (unsigned)(U)->cb_calls, (unsigned)(flo), (unsigned)(fhi), (why)

/* build 0.0.481: the unit480 line's last field, for a unit (`via_unit` 1) and for a single switch 56 retried through the
 * unit path (2, its `cons` field is 1). The host test bounds the line with the LONGEST of these four. */
#define N48_UNIT_WHY_OK      "translated"
#define N48_UNIT_WHY_REF     "refused (its bytes and its pool records restored)"
#define N48_UNIT_WHY_RETRY_OK  "single retried (switch 56), translated"
#define N48_UNIT_WHY_RETRY_REF "single retried (switch 56), refused (its bytes and its pool records restored)"
static inline const char *n48_mib_unit_why(uint32_t via_unit, uint32_t st)
{
    if (via_unit == 2u) return st ? N48_UNIT_WHY_RETRY_REF : N48_UNIT_WHY_RETRY_OK;
    return st ? N48_UNIT_WHY_REF : N48_UNIT_WHY_OK;
}

/* build 0.0.481 — the switch-56 read-out (`gfxneuter 56` and every gfxneuter report): the switch (and INERT when 55
 * is OFF), then per boot: retries asked (singles refused for room in a 55+56 pass), inputs that held the sentinel word
 * (not retried), retries translated / refused by the translator / refused because a sentinel survived, retried singles
 * refused later by the copy guard, the pool dwords undone for them, and the translated retries' placements. */
#define N48_RETRY481_FMT "retry481: room retry of singles (`gfxneuter 56 | M << 8`, needs 55) %s (%s). asked %llu, " \
                         "sentinel-in %llu, translated %llu, refused %llu, sentinel-left %llu, refused-later %llu " \
                         "(undone %llu dw); blocks %llu, own %llu dw, pool %llu dw"
static inline const char *n48_mib_retry_state(uint32_t on55, uint32_t on56)
{
    if (!on56) return "OFF (default)";
    return on55 ? "ON" : "ON but INERT (55 is OFF: nothing is retried)";
}

/* The switch-55 read-out (`gfxneuter 55` and every gfxneuter report): the switch, then per boot: policy passes with 55
 * ON, units of >= 2 constituents formed and their constituents, units translated / refused, deferred blocks placed and
 * their dwords in the units' own leftover and in the pool, un-redirects, inline invalidates, HS pairs patched,
 * per-constituent feeds, pool dwords undone after a refusal, pool runs not offered (table full), unit480 lines
 * suppressed past the cap; and P5's count of switch-49 credits withheld from a head that does not execute. */
#define N48_UNITS480_FMT "units480: continuation units (`gfxneuter 55 | M << 8`, CONTINUATION-UNITS.md) %s (%s). " \
                         "passes %llu, units formed %llu with %llu constituents, translated %llu, refused %llu; lines " \
                         "suppressed %llu; P5 switch-49 credits withheld (head does not execute) %llu; pool runs lost %llu"
#define N48_UNITS480B_FMT "units480b: deferred blocks placed %llu, dwords in own leftover %llu, in the pool %llu, " \
                          "un-redirects %llu, inline invalidates %llu, HS pairs patched %llu, per-constituent feeds %llu, " \
                          "pool dwords undone after a refusal %llu, pool runs offered %llu"

/* Every note function is a no-op on a null counter block, so a caller may pass one conditionally. */
static inline void n48_mib0_note_ib(n48_mib0 *m, uint32_t cls)
{
    if (m && cls < N48_MIB_STARTS) m->ib_start[cls]++;
}

static inline void n48_mib0_note_frame(n48_mib0 *m, uint32_t nib)
{
    if (m) m->frames_by_nib[nib < N48_MIB_NIB ? nib : N48_MIB_NIB - 1u]++;
}

static inline void n48_mib0_note_policy(n48_mib0 *m, uint32_t nib, uint64_t ns)
{
    if (!m) return;
    const uint32_t i = nib < N48_MIB_NIB ? nib : N48_MIB_NIB - 1u;
    m->policy_ns[i] += ns;
    m->policy_runs[i]++;
}

static inline void n48_mib0_note_commit(n48_mib0 *m, uint32_t nib)
{
    if (!m) return;
    m->commits[nib < N48_MIB_NIB ? nib : N48_MIB_NIB - 1u]++;
}

static inline void n48_mib0_note_f828(n48_mib0 *m, uint32_t why)
{
    if (!m) return;
    m->f828_offered++;
    if (why < N48_F828_REASONS) m->f828_answer[why]++;
}

/* f828 answers that were offered and did not come out OK, for the report. */
static inline uint64_t n48_mib0_f828_refused(const n48_mib0 *m)
{
    if (!m) return 0ull;
    const uint64_t ok = m->f828_answer[N48_F828_OK];
    return m->f828_offered > ok ? m->f828_offered - ok : 0ull;
}

/* 0.0.430 — ONE MIB SEGMENT-STAGE RUN'S REFUSAL REASONS. No-op on a null block or reason, so a caller
 * may pass the diag unconditionally. `seg_frames` counts the runs, which is the denominator the zero/overflow counts are
 * read against: a boot that threw switch 36 never lands here and its report shows every field 0 with 0 runs, which is
 * the honest reading of "the instrument saw nothing" rather than "the segmenter refused nothing". */
static inline void n48_mib0_note_seg(n48_mib0 *m, const n48_mib_seg_diag *d)
{
    if (!m || !d) return;
    m->seg_frames++;
    for (uint32_t i = 0; i < N48_MIB_STARTS; i++) m->seg_zero[i] += d->zero_by_class[i];
    if (d->overflow) m->seg_overflow++;
    if (d->max_segs > m->seg_max) m->seg_max = d->max_segs;
}

/* 0.0.430 — ONE SINGLE-IB POLICY PASS'S PHASES, folded into the accumulator. The caller passes its own
 * `runs` bump separately (one per contributing pass), so this only adds the seven durations. */
static inline void n48_mib0_note_pol(n48_mib_pol *p, const n48_mib_pol *one)
{
    if (!p || !one) return;
    p->setup_ns += one->setup_ns;
    p->xlat_ns  += one->xlat_ns;
    p->desc_ns  += one->desc_ns;
    p->cons_ns  += one->cons_ns;
    p->fence_ns += one->fence_ns;
    p->log_ns   += one->log_ns;
    p->runs     += one->runs;
}

/* The report is ONE line under 512 bytes at WIDEST numerics. Every value is clamped to 32 bits for the format, so
 * the width is bounded by construction rather than by an argument count: 19 numbers of at most 10 digits + about
 * 210 bytes of text = about 400 bytes, and the host suite measures the worst case. The unit of the policy figure is
 * microseconds (ns / 1000), which is the "policy µs by nib" of binding B10. */
static inline uint32_t n48_mib0_cap32(uint64_t v)
{
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
}

/* 0.0.426 (MIB-COMMIT B0/T9) — THE SWITCH'S ONE REPORT LINE, HERE SO A HOST TEST CAN BOUND IT. The kext prints it from
 * `gfxneuter 36`; the widest case is one 10-digit frame count. Kept under 480 bytes by construction and measured by
 * tests/gfx_mib_test.cpp, so a field added later cannot silently push it past n48log's 512-byte body cap. */
#define N48_MIB_REPORT_FMT \
    "mib-commit: `gfxneuter 36 | M << 8` is %s%s; ON, all IBs of a frame are read into one buffer, each is translated " \
    "on its own, the gate checks every IB, the token and the ring exemption span every IB, and the fence is offered " \
    "only in the final segment of the last IB. OFF, 0.0.425 byte for byte. Frames read this boot %llu."

#define N48_MIB0_FMT \
    "gfx-mib: MIB-0 always-on read-only census - IBk>=1 start: head %u nop-head %u mid %u unread %u; " \
    "final-seg n48_f828_find: ok %u refused %u looked %u; " \
    "policy us by nib [1]%u/%u [2]%u/%u [3]%u/%u [4]%u/%u; " \
    "commits by nib [1]%u [2]%u [3]%u [4]%u"

#define N48_MIB0_ARGS(m) \
    n48_mib0_cap32((m)->ib_start[N48_MIB_START_HEAD]), \
    n48_mib0_cap32((m)->ib_start[N48_MIB_START_NOP_HEAD]), \
    n48_mib0_cap32((m)->ib_start[N48_MIB_START_MID]), \
    n48_mib0_cap32((m)->ib_start[N48_MIB_START_UNREAD]), \
    n48_mib0_cap32((m)->f828_answer[N48_F828_OK]), \
    n48_mib0_cap32(n48_mib0_f828_refused(m)), \
    n48_mib0_cap32((m)->f828_offered), \
    n48_mib0_cap32((m)->policy_ns[1] / 1000ull), n48_mib0_cap32((m)->policy_runs[1]), \
    n48_mib0_cap32((m)->policy_ns[2] / 1000ull), n48_mib0_cap32((m)->policy_runs[2]), \
    n48_mib0_cap32((m)->policy_ns[3] / 1000ull), n48_mib0_cap32((m)->policy_runs[3]), \
    n48_mib0_cap32((m)->policy_ns[4] / 1000ull), n48_mib0_cap32((m)->policy_runs[4]), \
    n48_mib0_cap32((m)->commits[1]), n48_mib0_cap32((m)->commits[2]), \
    n48_mib0_cap32((m)->commits[3]), n48_mib0_cap32((m)->commits[4])

/* 0.0.430 — THE SEGMENT-STAGE REFUSALS, SPLIT ONTO THEIR OWN LINE.'s MIB-0 line was already at
 * ~470 bytes at its widest; adding four more numbers to it would push it past the logger's 512-byte body cap, so the
 * refusal reasons are a SECOND line, each bounded below by the same clamps and both measured by tests/gfx_mib_test.cpp.
 * The reasons are the ones could not tell apart in decide36's log: `n48_mib_segment` returned 0 for all 36 two-IB
 * policy runs and the logs could not say whether an IB named no segment or the 32-row table overflowed. */
#define N48_MIB0_SEG_FMT \
    "gfx-mib: MIB-0 segment refusals - zero-seg IB by start class: head %u nop-head %u mid %u unread %u; " \
    "table overflow %u; max Sigma segments seen %u; %u MIB segment-stage run(s)"

#define N48_MIB0_SEG_ARGS(m) \
    n48_mib0_cap32((m)->seg_zero[N48_MIB_START_HEAD]), \
    n48_mib0_cap32((m)->seg_zero[N48_MIB_START_NOP_HEAD]), \
    n48_mib0_cap32((m)->seg_zero[N48_MIB_START_MID]), \
    n48_mib0_cap32((m)->seg_zero[N48_MIB_START_UNREAD]), \
    n48_mib0_cap32((m)->seg_overflow), \
    n48_mib0_cap32((m)->seg_max), \
    n48_mib0_cap32((m)->seg_frames)

/* 0.0.430 — THE SINGLE-IB POLICY'S PHASES, ONE LINE. Read-only and printed by every read of the
 * `gfxneuter` report, like the MIB-0 census beside it. Phase durations are ns / 1000 (microseconds), clamped to 32
 * bits. `translate` INCLUDES `descriptor reads`, which is printed as a labelled subset so the two can never be summed.
 * Host-tested at its widest by tests/gfx_mib_test.cpp. */
#define N48_MIB0_POL_FMT \
    "gfx-mib: MIB-0 single-IB policy phases (us accumulated): setup %u, translate %u, consumer %u, fence %u, log %u, " \
    "descriptor reads %u (a subset of translate); %u run(s)"

#define N48_MIB0_POL_ARGS(p) \
    n48_mib0_cap32((p)->setup_ns / 1000ull), \
    n48_mib0_cap32((p)->xlat_ns / 1000ull), \
    n48_mib0_cap32((p)->cons_ns / 1000ull), \
    n48_mib0_cap32((p)->fence_ns / 1000ull), \
    n48_mib0_cap32((p)->log_ns / 1000ull), \
    n48_mib0_cap32((p)->desc_ns / 1000ull), \
    n48_mib0_cap32((p)->runs)

/* 0.0.434 (notes/design/PGMID-COPYGUARD.md Part 1): the SAME five phases (N48_MIB0_POL_ARGS is already
 * generic over any n48_mib_pol*), accumulated separately for nib >= 2 passes and printed on its own line so the
 * single-IB figure above is never read as describing a population it was never about. */
#define N48_MIB0_POL2_FMT \
    "gfx-mib: MIB-0 multi-IB (nib>=2) policy phases (us accumulated): setup %u, translate %u, consumer %u, fence %u, " \
    "log %u, descriptor reads %u (a subset of translate); %u run(s)"

/* =====================================================================================================================
 * build 0.0.505 (CROSS-IB C4) — THE `xib69:` CENSUS, READ-ONLY, EVERY BOOT, WHATEVER SWITCH 69 IS. Fed from every
 * MIB segment-stage run (switch 36 ON). WOULD = C1's pure predicate with BOTH rules (n48_mib_segment, `xib` 3): the kext
 * runs it into a scratch table when the real stage (switch 69's own M) answered 0 and M is not 3 - when the real stage
 * answered a table, or ran with M 3, its own answer IS the predicate's (a rule acts only on an IB the plain recogniser
 * left empty, and one such IB zeroes the frame). DID = the real stage's own answer. Lead segments' final statuses (after
 * the copy guard) are counted translated / refused. Nothing here changes a byte or a decision.
 *   runs          MIB segment-stage runs observed (the denominator)
 *   w_disg0       IB 0s the IB-0 disguise explains         d_disg0   ... in the real answer (bit 0 ON, frame accepted)
 *   w_lead        IBs k >= 1 whose lead formed              d_lead    ... in the real answer (bit 1 ON, frame accepted)
 *   w_rescued     frames the rules turn from 0 segments to a table         d_rescued ... in the real answer
 *   lead_ok / lead_refused   lead segments (or units that start with one) by final status */
typedef struct {
    uint64_t runs, w_disg0, w_lead, w_rescued, d_disg0, d_lead, d_rescued, lead_ok, lead_refused;
} n48_mib_xib;

static inline uint32_t n48_mib_popc(uint32_t v)
{
    uint32_t c = 0u;
    while (v) { c += v & 1u; v >>= 1; }
    return c;
}

/* One run. `cls0` is IB 0's n48_mib_start_class; `w_ns`/`w_lead_mask`/`w_nseg0` the predicate's answer (segments, the
 * leads that formed, IB 0's row count); `d_ns`/`d_lead_mask`/`d_nseg0`/`xib` the real stage's. An IB-0 disguise is
 * "IB 0 is NOP-HEAD and got rows" (the plain recogniser never matches a NOP-HEAD dword 0). A frame is rescued when the
 * answer is a table and some rule made a row (without the rules one empty IB zeroes the frame). */
static inline void n48_mib_xib_note(n48_mib_xib *x, uint32_t cls0, uint32_t w_ns, uint32_t w_lead_mask, uint32_t w_nseg0,
                                    uint32_t d_ns, uint32_t d_lead_mask, uint32_t d_nseg0, uint32_t xib)
{
    if (!x) return;
    x->runs++;
    const uint32_t wd0 = (cls0 == N48_MIB_START_NOP_HEAD && w_nseg0) ? 1u : 0u;
    x->w_disg0 += wd0;
    x->w_lead += n48_mib_popc(w_lead_mask);
    if (w_ns && (wd0 || w_lead_mask)) x->w_rescued++;
    const uint32_t dd0 = (d_ns && (xib & N48_MIB_XIB_DISG0) && cls0 == N48_MIB_START_NOP_HEAD && d_nseg0) ? 1u : 0u;
    const uint32_t dl = d_ns ? d_lead_mask : 0u;
    x->d_disg0 += dd0;
    x->d_lead += n48_mib_popc(dl);
    if (d_ns && (dd0 || dl)) x->d_rescued++;
}

/* The line: under the logger's 491-byte body at widest (nine counters clamped to 32 bits, as the MIB-0 lines are), measured
 * by tests/gfx_mib_xib_checks.h with the longest state and note. */
#define N48_XIB_FMT "xib69: cross-IB rules (`gfxneuter 69 | M << 8`: 1 IB-0 disguise, 2 lead, 3 both, 0xFF OFF) %s%s. " \
                    "MIB seg-stage runs %u; WOULD (both rules, any switch): IB-0 disguises %u, leads %u, frames rescued " \
                    "%u; DID: IB-0 disguises %u, leads %u, frames rescued %u; leads translated %u, refused %u"
#define N48_XIB_ARGS(x) \
    n48_mib0_cap32((x)->runs), n48_mib0_cap32((x)->w_disg0), n48_mib0_cap32((x)->w_lead), \
    n48_mib0_cap32((x)->w_rescued), n48_mib0_cap32((x)->d_disg0), n48_mib0_cap32((x)->d_lead), \
    n48_mib0_cap32((x)->d_rescued), n48_mib0_cap32((x)->lead_ok), n48_mib0_cap32((x)->lead_refused)
static inline const char *n48_mib_xib_state(uint32_t m)
{
    switch (m & N48_MIB_XIB_MASK) {
    case 1u: return "ON M 1 (IB-0 disguise)";
    case 2u: return "ON M 2 (lead)";
    case 3u: return "ON M 3 (IB-0 disguise + lead)";
    default: return "OFF (default)";
    }
}

#endif /* N48_GFX_MIB_H */
