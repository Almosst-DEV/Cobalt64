/* gfx_lutidx111.h — build 0.0.553: SWITCH 111 "lutidx", LEARN THE PLANE LUT FROM THE HEAP ENTRY THE DRAW NAMES.
 * `111 | M << 8`: M 1 ON (= 367), M 2 OFF (= 623, the default and the boot value), M 3 SHADOW (= 879); bare `111` reads. Mid-arm
 * guarded (the continuous guard, and any standing arm). Inert unless switch 32 is ON (it only changes WHICH record 32's learn reads).
 *
 * WHY (from run11aq / run11ar / run11av). gfxsrc_lut_learn reads a FIXED image-table entry, `imgVa + N48_LUT_SLOT4_OFF`
 * (gfx_lutfill.h: `(4u * 32u)`), but GPUPass names its LUT PER DRAW: its ABI (re/pc-26.6.2/xlat/windowserver/work/D/GPUPass.abi.json)
 * gives s6 = "texture location 1 (combinedLUT): image table index", which is kDTableAbi row {83, 0xd3d36bb9}'s tex[1] = 6, i.e. the
 * translator's input q == 1. No-cursor boots put the LUT at entry 4 (run11aq: `tex 1 heap 4 base 0x401140000`), so the fixed read
 * happened to be right; hardware-cursor boots put it at entry 3 (run11ar: `tex 1 heap 3 base 0x402000000`; run11av's heap page:
 * entry 3 `04020200 c1600000 80000fff c0000204`, entry 4 a 2560x1440 fmt 56 PLANE record `04022000 c3800000 ...`), so the fixed
 * read refused `not-32_FLOAT` and the planes stayed black.
 *
 * WHAT ON CHANGES, AND ONLY THIS: the learn reads entry `in_idx[1]` (the translator's export: the index input 1 was read through)
 * instead of entry 4, and ONLY when ALL of these hold:
 *   - the draw's table ABI row is GPUPass's ({83, 0xd3d36bb9}) AND the segment's in-force fragment program is ws_D_GPUPass;
 *   - the input list is complete (`in_over` 0) and has input 1 (`in_n` >= 2);
 *   - input 1 is LINEAR (`in_mode[1] == 0`) with a non-zero VA;
 * and the record read there is then accepted only if, beyond EVERY existing check (the decode, the instrument's one shape, the
 * producer cross-check, the page walk, VRAM-only, the destination guard, all gfxsrc_lut_learn's own and unchanged):
 *   - its TYPE is 1D_ARRAY (32_FLOAT 16384 x 3 is already the instrument's shape);
 *   - its decoded VA == in_va[1] (the record the kext reads is the record the translator read for this draw's LUT).
 * ON with any clause failing does NOT learn from that frame (and does not spend switch 32's one-shot): it never falls back to entry
 * 4. Entry 4 through ON (`in_idx[1] == 4`) is the SAME address as the old read: imgVa + sext32(4 << 5) = imgVa + 0x80.
 * SHADOW: the learn is OFF's exactly (entry 4); a read-only PEEK of the named entry logs what ON would learn. Never a page walk,
 * never a write, never the learn.
 * OFF: this header is never asked; every read is 0.0.552's.
 *
 * Pure: no lock, no clock, no log, no register. Host-tested by tests/gfx_lutfill_test.cpp (section 111) and pinned in the hook by
 * tests/gfx_commit_test.cpp (111 section). */
#ifndef N48_GFX_LUTIDX111_H
#define N48_GFX_LUTIDX111_H

#include <stdint.h>
#include "gfx_lutfill.h"

#define N48_LI_SWITCH 111u
enum { N48_LI_OFF = 0u, N48_LI_ON = 1u, N48_LI_SHADOW = 2u, N48_LI_MODES = 3u };
/* GPUPass: kDTableAbi row {83, 0xd3d36bb9} (xlat12_dtable_rows.inc: `{ 83u, 0xd3d36bb9u, 0u, 2u, { 4u, 6u, 0u }, ...`), and its
 * combinedLUT is the row's SECOND texture (tex[1] = s6), which the translator exports as input q == 1. */
#define N48_LI_GP_NDW 83u
#define N48_LI_GP_FNV 0xd3d36bb9u
#define N48_LI_Q      1u
#define N48_LI_PEEKS_MAX 16u   /* SHADOW: peeks per arm scope */
#define N48_LI_LINES     8u    /* ON: skip lines per arm scope (the counts are not capped) */

/* The verb's M -> the mode. 0 is a read; anything else not listed answers N48_LI_MODES (refused, unchanged). */
static inline uint32_t n48_li_mode_of_m(uint32_t m)
{
    return m == 1u ? (uint32_t)N48_LI_ON : m == 2u ? (uint32_t)N48_LI_OFF : m == 3u ? (uint32_t)N48_LI_SHADOW : (uint32_t)N48_LI_MODES;
}
static inline const char *n48_li_mode_name(uint32_t mode)
{
    return mode == N48_LI_ON ? "ON" : mode == N48_LI_SHADOW ? "SHADOW" : "OFF (default)";
}

/* The image-table entry's byte offset from the heap base, EXACTLY as the translator reads it (xlat12_ib.c d_tbl_off(aidx, 5):
 * `(uint64_t)(int64_t)(int32_t)(v << sh)`, s_lshl + s_ashr 31). idx 4 -> 0x80 == N48_LUT_SLOT4_OFF. */
static inline uint64_t n48_li_slot_off(uint32_t idx) { return (uint64_t)(int64_t)(int32_t)(idx << 5); }

/* Why ON may not read the draw's own index (0 = it may). */
enum { N48_LI_WHY_OK = 0u, N48_LI_WHY_NOT_GP = 1u, N48_LI_WHY_LIST = 2u, N48_LI_WHY_MODE = 3u, N48_LI_WHY_VA = 4u, N48_LI_WHYS = 5u };
static inline const char *n48_li_why_name(uint32_t w)
{
    return w == N48_LI_WHY_OK ? "OK" : w == N48_LI_WHY_NOT_GP ? "not-a-GPUPass-draw" : w == N48_LI_WHY_LIST ? "input-list-incomplete"
         : w == N48_LI_WHY_MODE ? "input-1-not-linear" : w == N48_LI_WHY_VA ? "input-1-VA-zero" : "?";
}

/* THE SELECTION: which entry the learn reads, and the VA its record must decode to. `use` 0 = the old read (entry 4). */
typedef struct { uint32_t use, idx, why, pad; uint64_t off, va; } n48_li_sel;

/* The actions of the pick. */
enum { N48_LI_ACT_LEGACY = 0u,   /* OFF / SHADOW: the learn reads entry 4 exactly as 0.0.552 */
       N48_LI_ACT_USE = 1u,      /* ON: the learn reads entry in_idx[1] (sel->use 1) */
       N48_LI_ACT_SKIP = 2u };   /* ON: a clause failed - no read, no learn from this frame (never entry 4) */

/* The draw facts -> the selection. `gp` = the draw's ABI row is GPUPass's AND the segment's program is ws_D_GPUPass. The arrays are
 * the translator's own export (xlat12_draw_stats in_mode / in_va / in_idx, XLAT12_DRAW_IN_MAX entries). SHADOW fills the selection
 * (sel->why, idx, off, va) for its peek but NEVER sets use. */
static inline uint32_t n48_li_pick(uint32_t mode, uint32_t gp, uint32_t in_n, uint32_t in_over, const uint32_t *in_mode,
                                   const uint64_t *in_va, const uint32_t *in_idx, n48_li_sel *sel)
{
    if (!sel) return N48_LI_ACT_LEGACY;
    sel->use = 0u; sel->idx = 4u; sel->why = N48_LI_WHY_OK; sel->pad = 0u; sel->off = N48_LUT_SLOT4_OFF; sel->va = 0ull;
    if (mode != N48_LI_ON && mode != N48_LI_SHADOW) return N48_LI_ACT_LEGACY;
    uint32_t why = N48_LI_WHY_OK;
    if (!gp) why = N48_LI_WHY_NOT_GP;
    else if (in_over || in_n <= N48_LI_Q || !in_mode || !in_va || !in_idx) why = N48_LI_WHY_LIST;
    else if (in_mode[N48_LI_Q] != 0u) why = N48_LI_WHY_MODE;
    else if (in_va[N48_LI_Q] == 0ull) why = N48_LI_WHY_VA;
    sel->why = why;
    if (why == N48_LI_WHY_OK) {
        sel->idx = in_idx[N48_LI_Q];
        sel->off = n48_li_slot_off(sel->idx);
        sel->va = in_va[N48_LI_Q];
    }
    if (mode == N48_LI_SHADOW) return N48_LI_ACT_LEGACY;   /* SHADOW never changes what the learn reads */
    if (why != N48_LI_WHY_OK) return N48_LI_ACT_SKIP;
    sel->use = 1u;
    return N48_LI_ACT_USE;
}

/* The offset the learn reads at: the draw's own entry when ON selected it, else entry 4 (0.0.552's read, byte for byte). */
static inline uint64_t n48_li_learn_off(const n48_li_sel *sel)
{
    return (sel && sel->use) ? sel->off : (uint64_t)N48_LUT_SLOT4_OFF;
}
/* The index the lines print for that read. */
static inline uint32_t n48_li_learn_idx(const n48_li_sel *sel) { return (sel && sel->use) ? sel->idx : 4u; }

/* ON's EXTRA acceptance of a record that already decoded green and passed the instrument's shape: TYPE 1D_ARRAY, and the decoded
 * VA equals the VA the translator read for input 1. Returns N48_LUT_OK, N48_LUT_NOT_1D or N48_LUT_INVA. `want_va` 0 refuses. */
static inline uint32_t n48_li_accept(const uint32_t rec[N48_LUT_RECORD_DWORDS], const n48_lut_desc *d, uint64_t want_va)
{
    if (!rec || !d) return N48_LUT_ABSENT;
    n48_lut_fields f;
    n48_lut_fields_of(rec, &f);
    if (f.type != N48_LUT_TYPE_1D_ARRAY) return N48_LUT_NOT_1D;
    if (!want_va || d->va != want_va || f.va != want_va) return N48_LUT_INVA;
    return N48_LUT_OK;
}
/* The acceptance the learn applies after the instrument's shape: ON's extra clauses only when the selection is in use. */
static inline uint32_t n48_li_accept_sel(const n48_li_sel *sel, const uint32_t rec[N48_LUT_RECORD_DWORDS], const n48_lut_desc *d)
{
    return (sel && sel->use) ? n48_li_accept(rec, d, sel->va) : (uint32_t)N48_LUT_OK;
}

/* ---- the lines (each <= 491 bytes at maximal fields: tests/gfx_commit_test.cpp 111 R6) ---- */
/* The verb's ONE line. */
#define N48_LI_FMT \
    "lutidx111: `gfxneuter 111 | M << 8` is %s (367 ON, 623 OFF, 879 SHADOW; mid-arm guarded; switch 32 %s)%s; " \
    "ON reads %u (idx 4 %u, other %u), skips %u (not GPUPass %u, list %u, not linear %u, VA 0 %u); " \
    "SHADOW peeks %u (would learn %u, refuse %u, skip %u); last idx %u."
/* ON: a frame whose own index ON would not read (capped N48_LI_LINES per arm scope). */
#define N48_LI_SKIP_FMT \
    "lutidx111: f%llu ON SKIPPED the LUT learn (entry 4 NOT read): %s; in_n %u over %u in_mode[1] %u in_va[1] %#llx idx %u; " \
    "skipped %u"
/* SHADOW: what ON would learn from the draw's own entry (capped N48_LI_PEEKS_MAX per arm scope). */
#define N48_LI_PEEK_FMT \
    "lutidx111: SHADOW f%llu: ON would %s: idx %u (the learn read idx 4); in_va[1] %#llx; T# fmt %u type %u %u x %u x %u slice(s), " \
    "base %#llx; %s; peek %u of %u"

#endif /* N48_GFX_LUTIDX111_H */
