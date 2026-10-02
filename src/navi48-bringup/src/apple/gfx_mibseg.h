// gfx_mibseg.h — 0.0.446 ("the operator's first stop"): THE PER-SEGMENT STATUS COUNTER FOR TWO-IB FRAMES.
// Pure C that also compiles as C++; host-tested by tests/gfx_mib_test.cpp (every bucket, a planted miscount, the
// line widths) and by tests/gfx_dep_test.cpp (arm32 F48's REAL 12 segments through the real translator).
//
// WHY. decide41 could say THAT two-IB segments refused and, from `desc-prov` lines, that some refused
// TDESC_PROVENANCE - but 0.0.445 had no count of what EVERY segment of a nib >= 2 frame answered, split by IB, so
// "why do two-IB segments refuse" could only be read off capped per-event lines. This counts, for every segment of
// every frame with nib >= 2 that reached the policy's segment loop, the segment's FINAL status (after the copy-guard
// check, exactly what gfxsrc_policy stores in f->seg_status[k]), bucketed:
//   0                       translated
//   XLAT12_IB_ERR_DESC      split by the translator's OWN err_op, EVERY ONE OF XLAT12_TDESC_*'s ten codes (F1-FA)
//                           named individually, PLUS the three inline-path codes xlat12_ib.c also stores under
//                           XLAT12_IB_ERR_DESC (0xFD half-converted, 0xFE inherited-unknown, 0xFF index out of
//                           range) - no lump. A `op` this file does not recognise (should never occur: these 13
//                           are the whole set xlat12_ib.c ever writes to err_op under ERR_DESC) falls to a
//                           DEFENSIVE "unknown" bucket, counted and logged apart so it can never masquerade as one
//                           of the thirteen named codes.
//   XLAT12_IB_ERR_PAIR      the program-pair resolver refused
//   N48_SEG_COPY_OVERLAP    the kext's copy-guard refusal (gfx_copyguard.h)
//   anything else           "other" - and, build 0.0.447 (reviewer review of 0.0.446, item 6), also fed into
//                           n48_mibseg_other_hist below: a per-status HISTOGRAM naming EVERY xlat12_status /
//                           XLAT12_IB_ERR_* code this file knows of (ERR_ARG..ERR_CAPACITY, ERR_UNLISTED..
//                           ERR_INTERP - 18 codes), combined across IB 0 and IB >= 1 (the seg[2][...] bucket
//                           counts above stay split by IB; the histogram does not, to keep its report line under
//                           the 512-byte cap without needing four more lines), plus one "unnamed" slot for a
//                           status genuinely outside that set.
// with SEPARATE totals for IB 0 and IB >= 1, plus frames whose EVERY segment translated, and the most recent frame
// with at least one refused segment as a compact per-segment string (at most 16 segments).
//
// build 0.0.447 (reviewer review of 0.0.446, item 6) — TWO LABEL FIXES: `lastOther`/`lastDescUnknownOp`
// (0.0.446's `lastDescOtherOp`, renamed with the bucket it now tracks) are PER-IB (`[2]`, index 0 = IB 0, 1 = IB
// >= 1) - 0.0.446 had ONE field for both IBs, so FMT_A's "last raw status" printed on the IB 0 report line could
// in fact be IB >= 1's most recent OTHER segment, and FMT_B named the ONE shared `lastDescOtherOp` with no IB of
// its own. Each report line now prints only its OWN IB's last value.
#ifndef N48_GFX_MIBSEG_H
#define N48_GFX_MIBSEG_H

#include <stdint.h>
#include "xlat12_ib.h"      // XLAT12_IB_ERR_DESC / _PAIR and the XLAT12_TDESC_* detail codes
#include "gfx_copyguard.h"  // N48_SEG_COPY_OVERLAP, the kext-only copy-guard status

#ifdef __cplusplus
extern "C" {
#endif

enum {
    N48_MIBSEG_OK = 0,
    N48_MIBSEG_DESC_PROVENANCE,        /* XLAT12_TDESC_PROVENANCE  0xF7 */
    N48_MIBSEG_DESC_SLOT_UNSEEN,       /* XLAT12_TDESC_SLOT_UNSEEN 0xF1 */
    N48_MIBSEG_DESC_SLOT_SHARED,       /* XLAT12_TDESC_SLOT_SHARED 0xF2 */
    N48_MIBSEG_DESC_SAMP_OVERRIDE,     /* XLAT12_TDESC_SAMP_OVERRIDE 0xF3 */
    N48_MIBSEG_DESC_READ,              /* XLAT12_TDESC_READ        0xF4 */
    N48_MIBSEG_DESC_UNSTABLE,          /* XLAT12_TDESC_UNSTABLE    0xF5 */
    N48_MIBSEG_DESC_NOT_APPLE,         /* XLAT12_TDESC_NOT_APPLE   0xF6 */
    N48_MIBSEG_DESC_NO_ROOM,           /* XLAT12_TDESC_NO_ROOM     0xF8 */
    N48_MIBSEG_DESC_REDIRECTED,        /* XLAT12_TDESC_REDIRECTED  0xF9 */
    N48_MIBSEG_DESC_TOO_MANY,          /* XLAT12_TDESC_TOO_MANY    0xFA */
    N48_MIBSEG_DESC_INLINE_MIXED,      /* xlat12_ib.c err_op 0xFD: half gfx12, half gfx10 (already converted by an earlier draw) */
    N48_MIBSEG_DESC_INLINE_INHERITED,  /* xlat12_ib.c err_op 0xFE: an inline record slot not written by THIS translation */
    N48_MIBSEG_DESC_INLINE_RANGE,      /* xlat12_ib.c err_op 0xFF: the inline record's slot range runs past D_PS_UDN */
    N48_MIBSEG_DESC_UNKNOWN,           /* DEFENSIVE: an err_op under ERR_DESC that is none of the thirteen above */
    N48_MIBSEG_PAIR,
    N48_MIBSEG_COPY_OVERLAP,
    N48_MIBSEG_OTHER,
    N48_MIBSEG_BUCKETS
};

/* One letter per bucket, in enum order, for the mibseg-last string. */
#define N48_MIBSEG_LETTERS ".PUSVRBNMETGIH?ACX"
#define N48_MIBSEG_LAST_MAX 16u

/* build 0.0.447 (item 6) — THE NON-DESC, NON-PAIR STATUS HISTOGRAM. Every xlat12_status (xlat12.h) and
 * XLAT12_IB_ERR_* (xlat12_ib.h) code that is NOT 0 (OK), XLAT12_IB_ERR_DESC or XLAT12_IB_ERR_PAIR - the codes that
 * land in N48_MIBSEG_OTHER above - gets its OWN named slot here, so a run can tell "18 segments refused DRAW_SHAPE"
 * from "18 segments refused VERIFY" instead of reading one combined `other` count. Slot N48_MIBSEG_OHIST_N (the
 * last) is for a status genuinely outside this set (defensive; xlat12 defines no such code today). Combined across
 * IB 0 and IB >= 1 - see this file's own banner for why. */
enum {
    N48_MIBSEG_OHIST_ARG = 0, N48_MIBSEG_OHIST_BAD_PACKET, N48_MIBSEG_OHIST_TRUNCATED, N48_MIBSEG_OHIST_RANGE,
    N48_MIBSEG_OHIST_BAD_INDEX, N48_MIBSEG_OHIST_UNKNOWN_REG, N48_MIBSEG_OHIST_LEGACY_VS, N48_MIBSEG_OHIST_VS_MODE_UNKNOWN,
    N48_MIBSEG_OHIST_MEMLOADED, N48_MIBSEG_OHIST_CAPACITY, N48_MIBSEG_OHIST_UNLISTED, N48_MIBSEG_OHIST_REG_OPERAND,
    N48_MIBSEG_OHIST_COND_EXEC, N48_MIBSEG_OHIST_TOO_LONG, N48_MIBSEG_OHIST_DRAW_SHAPE, N48_MIBSEG_OHIST_VERIFY,
    N48_MIBSEG_OHIST_RING, N48_MIBSEG_OHIST_INTERP,
    N48_MIBSEG_OHIST_N   /* the "unnamed" slot */
};

typedef struct {
    uint64_t seg[2][N48_MIBSEG_BUCKETS];   /* [0] = IB 0, [1] = IB >= 1 */
    uint64_t frames;                       /* nib >= 2 frames with at least one segment */
    uint64_t framesAllOk;                  /* ... of which EVERY segment translated */
    uint64_t framesNoSeg;                  /* nib >= 2 frames the segment stage gave no segment */
    uint32_t lastOther[2];                 /* [ib] raw status of the most recent OTHER segment on that IB (0 = none yet) */
    uint32_t lastDescUnknownOp[2];         /* [ib] err_op of the most recent DESC_UNKNOWN segment on that IB (0 = none yet) */
    uint64_t otherHist[N48_MIBSEG_OHIST_N + 1u];   /* combined across both IBs - see this file's own banner */
    /* The most recent frame with at least one refused segment. */
    uint64_t lastFrame;
    uint32_t lastNib, lastNseg, lastShown;
    uint8_t  lastBucket[N48_MIBSEG_LAST_MAX], lastIb[N48_MIBSEG_LAST_MAX];
} n48_mibseg;

/* The bucket of one segment's final status `st` with the translator's own err_op `op` (read only for DESC). */
static inline uint32_t n48_mibseg_bucket(uint32_t st, uint32_t op)
{
    if (st == 0u) return N48_MIBSEG_OK;
    if (st == (uint32_t)XLAT12_IB_ERR_DESC) {
        switch (op) {
        case XLAT12_TDESC_PROVENANCE:  return N48_MIBSEG_DESC_PROVENANCE;
        case XLAT12_TDESC_SLOT_UNSEEN: return N48_MIBSEG_DESC_SLOT_UNSEEN;
        case XLAT12_TDESC_SLOT_SHARED: return N48_MIBSEG_DESC_SLOT_SHARED;
        case XLAT12_TDESC_SAMP_OVERRIDE: return N48_MIBSEG_DESC_SAMP_OVERRIDE;
        case XLAT12_TDESC_READ:        return N48_MIBSEG_DESC_READ;
        case XLAT12_TDESC_UNSTABLE:    return N48_MIBSEG_DESC_UNSTABLE;
        case XLAT12_TDESC_NOT_APPLE:   return N48_MIBSEG_DESC_NOT_APPLE;
        case XLAT12_TDESC_NO_ROOM:     return N48_MIBSEG_DESC_NO_ROOM;
        case XLAT12_TDESC_REDIRECTED:  return N48_MIBSEG_DESC_REDIRECTED;
        case XLAT12_TDESC_TOO_MANY:    return N48_MIBSEG_DESC_TOO_MANY;
        case 0xFDu:                    return N48_MIBSEG_DESC_INLINE_MIXED;
        case 0xFEu:                    return N48_MIBSEG_DESC_INLINE_INHERITED;
        case 0xFFu:                    return N48_MIBSEG_DESC_INLINE_RANGE;
        default:                       return N48_MIBSEG_DESC_UNKNOWN;
        }
    }
    if (st == (uint32_t)XLAT12_IB_ERR_PAIR) return N48_MIBSEG_PAIR;
    if (st == (uint32_t)N48_SEG_COPY_OVERLAP) return N48_MIBSEG_COPY_OVERLAP;
    return N48_MIBSEG_OTHER;
}

/* build 0.0.447 (item 6) — the histogram slot for a raw status that landed in N48_MIBSEG_OTHER. Every code
 * xlat12.h / xlat12_ib.h can put there (never 0, XLAT12_IB_ERR_DESC or XLAT12_IB_ERR_PAIR - n48_mibseg_bucket
 * already routes those away) gets its own slot; anything else is N48_MIBSEG_OHIST_N (defensive). */
static inline uint32_t n48_mibseg_other_idx(uint32_t st)
{
    switch (st) {
    case XLAT12_ERR_ARG:             return N48_MIBSEG_OHIST_ARG;
    case XLAT12_ERR_BAD_PACKET:      return N48_MIBSEG_OHIST_BAD_PACKET;
    case XLAT12_ERR_TRUNCATED:       return N48_MIBSEG_OHIST_TRUNCATED;
    case XLAT12_ERR_RANGE:           return N48_MIBSEG_OHIST_RANGE;
    case XLAT12_ERR_BAD_INDEX:       return N48_MIBSEG_OHIST_BAD_INDEX;
    case XLAT12_ERR_UNKNOWN_REG:     return N48_MIBSEG_OHIST_UNKNOWN_REG;
    case XLAT12_ERR_LEGACY_VS:       return N48_MIBSEG_OHIST_LEGACY_VS;
    case XLAT12_ERR_VS_MODE_UNKNOWN: return N48_MIBSEG_OHIST_VS_MODE_UNKNOWN;
    case XLAT12_ERR_MEMLOADED:       return N48_MIBSEG_OHIST_MEMLOADED;
    case XLAT12_ERR_CAPACITY:        return N48_MIBSEG_OHIST_CAPACITY;
    case (uint32_t)XLAT12_IB_ERR_UNLISTED:    return N48_MIBSEG_OHIST_UNLISTED;
    case (uint32_t)XLAT12_IB_ERR_REG_OPERAND: return N48_MIBSEG_OHIST_REG_OPERAND;
    case (uint32_t)XLAT12_IB_ERR_COND_EXEC:   return N48_MIBSEG_OHIST_COND_EXEC;
    case (uint32_t)XLAT12_IB_ERR_TOO_LONG:    return N48_MIBSEG_OHIST_TOO_LONG;
    case (uint32_t)XLAT12_IB_ERR_DRAW_SHAPE:  return N48_MIBSEG_OHIST_DRAW_SHAPE;
    case (uint32_t)XLAT12_IB_ERR_VERIFY:      return N48_MIBSEG_OHIST_VERIFY;
    case (uint32_t)XLAT12_IB_ERR_RING:        return N48_MIBSEG_OHIST_RING;
    case (uint32_t)XLAT12_IB_ERR_INTERP:      return N48_MIBSEG_OHIST_INTERP;
    default:                         return N48_MIBSEG_OHIST_N;
    }
}

/* One frame. `nib` < 2 is not this counter's population and is ignored. `st[k]`/`op[k]`/`ib[k]` for k < nseg are the
 * segment's final status, the translator's err_op, and the IB that owns the segment (0 = IB 0). */
static inline void n48_mibseg_note_frame(n48_mibseg *m, uint64_t frame, uint32_t nib, uint32_t nseg,
                                         const uint32_t *st, const uint32_t *op, const uint8_t *ib)
{
    if (!m || nib < 2u) return;
    if (!nseg || !st) { m->framesNoSeg++; return; }
    m->frames++;
    uint32_t bad = 0u;
    for (uint32_t k = 0; k < nseg; k++) {
        const uint32_t o = op ? op[k] : 0u;
        const uint32_t b = n48_mibseg_bucket(st[k], o);
        const uint32_t side = (ib && ib[k]) ? 1u : 0u;
        m->seg[side][b]++;
        if (b == N48_MIBSEG_OTHER) { m->lastOther[side] = st[k]; m->otherHist[n48_mibseg_other_idx(st[k])]++; }
        if (b == N48_MIBSEG_DESC_UNKNOWN) m->lastDescUnknownOp[side] = o;
        if (b != N48_MIBSEG_OK) bad++;
    }
    if (!bad) { m->framesAllOk++; return; }
    m->lastFrame = frame; m->lastNib = nib; m->lastNseg = nseg;
    m->lastShown = nseg < N48_MIBSEG_LAST_MAX ? nseg : N48_MIBSEG_LAST_MAX;
    for (uint32_t k = 0; k < m->lastShown; k++) {
        m->lastBucket[k] = (uint8_t)n48_mibseg_bucket(st[k], op ? op[k] : 0u);
        m->lastIb[k] = ib ? ib[k] : (uint8_t)0u;
    }
}

/* The compact string: one letter per shown segment (N48_MIBSEG_LETTERS), a '|' where the owning IB changes.
 * At most 16 letters + 15 separators + NUL; `cap` below that truncates (always NUL-terminated). */
#define N48_MIBSEG_LAST_STR (2u * N48_MIBSEG_LAST_MAX)
static inline void n48_mibseg_last_str(const n48_mibseg *m, char *buf, uint32_t cap)
{
    if (!buf || !cap) return;
    uint32_t w = 0u;
    buf[0] = '\0';
    if (!m || !m->lastShown) return;
    for (uint32_t k = 0; k < m->lastShown && k < N48_MIBSEG_LAST_MAX; k++) {
        if (k && m->lastIb[k] != m->lastIb[k - 1u]) { if (w + 1u < cap) buf[w++] = '|'; }
        const uint32_t b = m->lastBucket[k] < N48_MIBSEG_BUCKETS ? m->lastBucket[k] : N48_MIBSEG_OTHER;
        if (w + 1u < cap) buf[w++] = N48_MIBSEG_LETTERS[b];
    }
    buf[w] = '\0';
}

static inline uint32_t n48_mibseg_cap32(uint64_t v) { return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v; }

/* build 0.0.474 item 2 (10B-COVERAGE.md Q4 item 4 contract (2)) — LOG-ONLY. Through 0.0.473 the ONLY per-
 * segment string this file produced was `n48_mibseg_last_str` over the boot's most recent REFUSED nib>=2 frame
 * (gMibSeg.last*): a 10b run with several such frames per boot could see only the last one. These two functions
 * are the SAME two pieces of that string/detail, but built directly from ONE frame's own raw arrays (never
 * touching the aggregate `n48_mibseg` struct, so calling them changes no counter n48_mibseg_note_frame already
 * owns) - so the kext can print ONE line for EVERY judged nib>=2 frame, not only the last refused one. Pure,
 * host-tested (tests/gfx_mib_test.cpp), same discipline as every function in this file. */

/* The SAME compact letter string as n48_mibseg_last_str (N48_MIBSEG_LETTERS, '|' at an IB change), built straight
 * from this frame's own `st[]`/`op[]`/`ib[]` (length `nseg`, truncated to N48_MIBSEG_LAST_MAX exactly as the
 * aggregate's own `lastShown` is). `buf` must hold at least N48_MIBSEG_LAST_STR + 1 bytes; always NUL-terminated. */
static inline void n48_mibseg_frame_str(const uint32_t *st, const uint32_t *op, const uint8_t *ib, uint32_t nseg,
                                        char *buf, uint32_t cap)
{
    if (!buf || !cap) return;
    uint32_t w = 0u;
    buf[0] = '\0';
    if (!st) return;
    const uint32_t shown = nseg < N48_MIBSEG_LAST_MAX ? nseg : N48_MIBSEG_LAST_MAX;
    for (uint32_t k = 0; k < shown; k++) {
        if (k && ib && ib[k] != ib[k - 1u]) { if (w + 1u < cap) buf[w++] = '|'; }
        const uint32_t b = n48_mibseg_bucket(st[k], op ? op[k] : 0u);
        if (w + 1u < cap) buf[w++] = N48_MIBSEG_LETTERS[b < N48_MIBSEG_BUCKETS ? b : N48_MIBSEG_OTHER];
    }
    buf[w < cap ? w : cap - 1u] = '\0';
}

/* build 0.0.474 item 2 (contract (2)) — up to `cap` REFUSED segments' own (index, err_op, err_in_dword), in
 * segment order. `op[k]` IS the TOO_LONG "site" the contract asks for when `st[k] == XLAT12_IB_ERR_TOO_LONG`: the
 * translator's own err_op is 0xFF for the extra-ring-state block (xlat12_ib.c's S10) or 0xFC for the re-emission
 * room wall (S11/S12, XLAT12_REEMIT_NO_ROOM) - the two are already distinguished by this single field, so there is
 * no separate "site" to compute. Returns the count written (<= cap); `*total_out` is the frame's TRUE refused-
 * segment count, which can exceed `cap` (never silent - rule 72). Pure: reads st[]/op[]/dw[], writes only the
 * three out arrays (each must hold >= cap entries) and *total_out. */
#define N48_MIBSEG_DETAIL_CAP 8u
static inline uint32_t n48_mibseg_refused_list(const uint32_t *st, const uint32_t *op, const uint32_t *dw, uint32_t nseg,
                                               uint32_t *idx_out, uint32_t *op_out, uint32_t *dw_out, uint32_t cap,
                                               uint32_t *total_out)
{
    uint32_t shown = 0u, total = 0u;
    if (st) {
        for (uint32_t k = 0; k < nseg; k++) {
            if (!st[k]) continue;
            total++;
            if (shown < cap) {
                if (idx_out) idx_out[shown] = k;
                if (op_out) op_out[shown] = op ? op[k] : 0u;
                if (dw_out) dw_out[shown] = dw ? dw[k] : 0u;
                shown++;
            }
        }
    }
    if (total_out) *total_out = total;
    return shown;
}

/* build 0.0.481: `nseg %u%s` - the %s is gfx_mib.h's n48_mib_units_mark: " units" when the
 * pass formed switch-55 units (nseg, the string and every refused index then count UNITS, not Apple's segments), "" (the
 * 0.0.474 text, byte for byte) otherwise.
 * args: judged frame, nib, nseg, the units marker, the frame string (n48_mibseg_frame_str), refused-shown, refused-total, then
 * N48_MIBSEG_DETAIL_CAP (idx, err_op, err_in_dword) triples, zero-padded past `refused-shown`. Measured at widest
 * numerics by tests/gfx_mib_test.cpp against the SAME N48_LOG_CAP_BODY bound (f3_reader.h) every other line here
 * uses. */
#define N48_MIBSEG_DETAIL_FMT \
    "mibseg-detail: frame %llu nib %u nseg %u%s: %s refused %u of %u: r0 %u/%#x/%u r1 %u/%#x/%u r2 %u/%#x/%u " \
    "r3 %u/%#x/%u r4 %u/%#x/%u r5 %u/%#x/%u r6 %u/%#x/%u r7 %u/%#x/%u"

/* THE REPORT LINES, HERE SO THE HOST TEST CAN BOUND THEM (; 0.0.389's tgtsample lines were
 * cut at 512 bytes with no marker). Every number is clamped to 32 bits, so the width is bounded by construction and
 * tests/gfx_mib_test.cpp measures it at widest numerics against the same 480-byte bound the MIB-0 lines use.
 * build 0.0.447 (item 6): every XLAT12_TDESC_* code (F1-FA) and the three inline-path codes (0xFD/0xFE/0xFF)
 * now has its own field - no "other-desc" lump - and each line prints its OWN IB's `lastOther`/`lastDescUnknownOp`
 * (the label fix: 0.0.446 printed one shared value on the IB 0 line only). */
/* build 0.0.447 (item 6): split into FIVE lines (was two, A/B) so NONE of them risks the 512-byte cap now
 * that every DESC code is named individually - measured at widest numerics by tests/gfx_mib_test.cpp, same as
 * every other line here. A0 is the frame-level summary; A1/A2 are IB 0's segment detail in two halves; B1/B2 are
 * IB >= 1's, in the SAME two halves, so a reader can align A1<->B1 and A2<->B2 field for field. */
// build 0.0.482 (item 3): the SAME n48_mib_units_mark marker (0.0.481's,  merge note) on these FIVE
// 0.0.446 aggregate lines, placed between "mibseg" and its own colon (never inside the numeric fields, which the
// aggregate never splits by pass) - "" (byte-identical to 0.0.474/0.0.481 text) unless the kext's gUnitsFormedBoot
// latched at least once this boot; the caller supplies the marker as the format's FIRST vararg, before *_ARGS_*.
#define N48_MIBSEG_FMT_A0 \
    "mibseg%s: nib>=2 frames with segments %u, EVERY segment translated %u, no segment %u."
#define N48_MIBSEG_ARGS_A0(m) \
    n48_mibseg_cap32((m)->frames), n48_mibseg_cap32((m)->framesAllOk), n48_mibseg_cap32((m)->framesNoSeg)

#define N48_MIBSEG_FMT_A1 \
    "mibseg%s: IB 0 segments: ok %u; DESC provenance %u slot-unseen %u slot-shared %u samp-override %u read %u " \
    "unstable %u not-apple %u"
#define N48_MIBSEG_ARGS_A1(m) \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_OK]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_PROVENANCE]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_SLOT_UNSEEN]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_SLOT_SHARED]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_SAMP_OVERRIDE]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_READ]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_UNSTABLE]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_NOT_APPLE])

#define N48_MIBSEG_FMT_A2 \
    "mibseg%s: IB 0 segments (cont.): no-room %u redirected %u too-many %u inline-mixed %u inline-inherited %u " \
    "inline-range %u unknown-op %#x (count %u); PAIR %u; COPY-OVERLAP %u; other %u (last raw status on THIS ib %#x)"
#define N48_MIBSEG_ARGS_A2(m) \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_NO_ROOM]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_REDIRECTED]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_TOO_MANY]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_INLINE_MIXED]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_INLINE_INHERITED]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_INLINE_RANGE]), \
    (m)->lastDescUnknownOp[0], n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_DESC_UNKNOWN]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_PAIR]), n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_COPY_OVERLAP]), \
    n48_mibseg_cap32((m)->seg[0][N48_MIBSEG_OTHER]), (m)->lastOther[0]

#define N48_MIBSEG_FMT_B1 \
    "mibseg%s: IB>=1 segments: ok %u; DESC provenance %u slot-unseen %u slot-shared %u samp-override %u read %u " \
    "unstable %u not-apple %u"
#define N48_MIBSEG_ARGS_B1(m) \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_OK]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_PROVENANCE]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_SLOT_UNSEEN]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_SLOT_SHARED]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_SAMP_OVERRIDE]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_READ]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_UNSTABLE]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_NOT_APPLE])

#define N48_MIBSEG_FMT_B2 \
    "mibseg%s: IB>=1 segments (cont.): no-room %u redirected %u too-many %u inline-mixed %u inline-inherited %u " \
    "inline-range %u unknown-op %#x (count %u); PAIR %u; COPY-OVERLAP %u; other %u (last raw status on THIS ib %#x)"
#define N48_MIBSEG_ARGS_B2(m) \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_NO_ROOM]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_REDIRECTED]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_TOO_MANY]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_INLINE_MIXED]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_INLINE_INHERITED]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_INLINE_RANGE]), \
    (m)->lastDescUnknownOp[1], n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_DESC_UNKNOWN]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_PAIR]), n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_COPY_OVERLAP]), \
    n48_mibseg_cap32((m)->seg[1][N48_MIBSEG_OTHER]), (m)->lastOther[1]

/* build 0.0.447 (item 6) — THE OTHER-STATUS HISTOGRAM LINE. Combined across IB 0 and IB >= 1 (this file's own
 * banner explains why: one line, not four, and still under the 480-byte bound at the widest numerics). */
#define N48_MIBSEG_FMT_C \
    "mibseg-other: err-arg %u bad-packet %u truncated %u range %u bad-index %u unknown-reg %u legacy-vs %u " \
    "vs-mode-unknown %u memloaded %u capacity %u unlisted %u reg-operand %u cond-exec %u too-long %u " \
    "draw-shape %u verify %u ring %u interp %u unnamed %u (combined IB 0 + IB>=1)"
#define N48_MIBSEG_ARGS_C(m) \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_ARG]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_BAD_PACKET]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_TRUNCATED]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_RANGE]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_BAD_INDEX]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_UNKNOWN_REG]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_LEGACY_VS]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_VS_MODE_UNKNOWN]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_MEMLOADED]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_CAPACITY]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_UNLISTED]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_REG_OPERAND]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_COND_EXEC]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_TOO_LONG]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_DRAW_SHAPE]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_VERIFY]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_RING]), n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_INTERP]), \
    n48_mibseg_cap32((m)->otherHist[N48_MIBSEG_OHIST_N])

/* args: judged frame (u32-clamped), nib, nseg, shown, the n48_mibseg_last_str string. */
#define N48_MIBSEG_LAST_FMT \
    "mibseg-last: most recent nib>=2 frame with a refused segment: judged frame %u, nib %u, %u segment(s), first %u " \
    "in order ('|' = next IB): %s. Key: .=ok P=provenance U=slot-unseen S=slot-shared V=samp-override R=read " \
    "B=unstable N=not-apple M=no-room E=redirected T=too-many G=inline-range I=inline-inherited H=inline-mixed " \
    "?=unknown-desc A=PAIR C=COPY-OVERLAP X=other."

#ifdef __cplusplus
}
#endif
#endif /* N48_GFX_MIBSEG_H */
