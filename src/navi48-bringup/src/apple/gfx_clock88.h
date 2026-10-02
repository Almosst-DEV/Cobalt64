// gfx_clock88.h - build 0.0.512 Part B: INSTRUMENTATION FOR THE CLOCK's SOURCE. Pure, header-only,
// host-testable (tests/gfx_clock88_test.cpp). NO BEHAVIOUR CHANGE: every function here decides only what to PRINT; none writes a
// register, a page table, VRAM, a translated dword or anything a rule reads.
//
// WHAT IT OBSERVES: identity 88 = ws_S_TimgXhu_Idfr (kDTableAbi {85, 0xd0a62abe}: class 19 table s0, ONE texture
// s8, one sampler s10) writes the clock's frozen distance surface (run10t 0x402380000) from an input no captured packet names
// (0x402230000 / 0x401380000 in the harness, SUSPECTED). Three instruments:
//   B1  per FRAME holding an S draw (first N48_C88_LINES per boot; 0.0.513: frames of the shape 8736|14352 only; 0.0.521: frames the
//       CONTENT scan judged the producer, any shape, n48_c88_take_frame): the frame's IB shape, wall clock + uptime, S's draw dword (relative to its
//       translation's input) and its CB0 base (the last CB_COLOR0_BASE / _EXT Apple's stream wrote before that draw), and the
//       FULL raw gfx10 T# (8 dwords) of every texture S samples, as the translator's table step read it (xlat12_draw_extra
//       tex_note: the same double read, before the port), plus each T#'s surface VA.
//   B2  a WATCH on the first S input surface (at most N48_C88_WATCH VAs per boot): every residency copy whose destination GPU VA
//       range overlaps it (the copy's MM-window and SDMA chunks alike: the copier's own COPIED / `via SDMA` lines carry the copy
//       number) and every MM-window write (outside a residency copy's own scope) whose VRAM range overlaps the surface's first
//       page's VRAM run, at most N48_C88_WATCH_LINES lines per boot, with time and the process.
//   B3  counters of judged frames per IB-shape class 8736|14352 and 8736|14544 (0.0.521: SHAPE counters only - found the
//       run10v producer at 8736|14160 and frame b at 8736|14352) and, from 0.0.521, of CONTENT producer frames (below).
#ifndef N48_GFX_CLOCK88_H
#define N48_GFX_CLOCK88_H
#include <stdint.h>

#define N48_C88_PS_ID       ((85ull << 32) | 0xd0a62abeull)   /* ws_S_TimgXhu_Idfr's kDTableAbi identity (ndw << 32 | fnv) */
#define N48_C88_LINES       32u     /* B1: frames logged per boot (0.0.521: 8 -> 32; content producers include the start-up S frames) */
#define N48_C88_TEX         2u      /* B1: textures kept per frame (S samples one; a second is kept if a row ever has two) */
#define N48_C88_WATCH       4u      /* B2: watched surfaces per boot (0.0.521: 2 -> 4, for the same reason) */
#define N48_C88_WATCH_LINES 16u     /* B2: watch lines per boot */
#define N48_C88_EXT_MIN     0x10000ull       /* B2: the watched extent, 64 KiB granular, at least 64 KiB ... */
#define N48_C88_EXT_MAX     0x2000000ull     /* ... at most 32 MiB */
#define N48_C88_EXT_UNKNOWN 0x1000ull        /* build 0.0.546: element size unknown -> the FIRST PAGE only, never an estimate */

/* B1: one frame's stash. `frame` is the policy pass's judged-frame number; a stash for another frame is replaced whole. */
typedef struct {
    uint64_t frame;
    uint32_t n;                                /* textures kept */
    uint32_t seg, at[N48_C88_TEX], tex[N48_C88_TEX];
    uint32_t rec[N48_C88_TEX][8];
    uint64_t va[N48_C88_TEX], cb0;
    uint64_t wall_s, up_ms; uint32_t wall_us;
    uint32_t printed;                          /* this frame's line went out (or was capped) */
} n48_c88_frame;

/* B1: keep the observer's record when it is S's and this frame has room. 1 = kept (the caller fills cb0/time on the first). */
static inline uint32_t n48_c88_note(n48_c88_frame *s, uint64_t frame, uint64_t ps_id, uint32_t at_i, const uint32_t rec[8],
                                    uint64_t va, uint32_t seg)
{
    if (!s || !rec || ps_id != N48_C88_PS_ID) return 0u;
    if (s->frame != frame) {                   /* a new frame: the old stash is gone, printed or not */
        s->frame = frame; s->n = 0u; s->printed = 0u; s->cb0 = 0ull; s->seg = seg;
    }
    const uint32_t at = at_i & 0xFFFFFFu, tex = at_i >> 24;
    for (uint32_t i = 0; i < s->n; i++) if (s->at[i] == at && s->tex[i] == tex && s->seg == seg) return 0u;   /* a retry's re-read */
    if (s->n >= N48_C88_TEX || (s->n && (s->seg != seg || s->at[0] != at))) return 0u;   /* the first S draw of the frame only */
    s->at[s->n] = at; s->tex[s->n] = tex; s->va[s->n] = va;
    for (uint32_t k = 0; k < 8u; k++) s->rec[s->n][k] = rec[k];
    s->n++;
    return 1u;
}

/* B1: the CB0 base in force at input dword `upto` of `in[0..n)`: the last SET_CONTEXT_REG value of CB_COLOR0_BASE (gfx10 context
 * dword 0x318: VA >> 8) and CB_COLOR0_BASE_EXT (0x390: VA [47:40]) before it. 0 when neither was written. Walks PM4 lengths
 * (type 3: count + 2; type 2 and the 0xFFFF1000 filler: 1). Pure, read-only over `in`. */
static inline uint64_t n48_c88_cb0(const uint32_t *in, uint32_t n, uint32_t upto)
{
    uint64_t base = 0ull, ext = 0ull;
    if (!in) return 0ull;
    for (uint32_t i = 0; i < upto && i < n;) {
        const uint32_t h = in[i];
        if (h == 0xFFFF1000u || (h >> 30) != 3u) { i++; continue; }
        const uint32_t l = ((h >> 16) & 0x3FFFu) + 2u;
        if (((h >> 8) & 0xFFu) == 0x69u && i + 1u < n) {
            const uint32_t off = in[i + 1u] & 0xFFFFu;
            for (uint32_t k = 0; k + 2u < l && i + 2u + k < n; k++) {
                if (off + k == 0x318u) base = in[i + 2u + k];
                else if (off + k == 0x390u) ext = in[i + 2u + k] & 0xFFu;
            }
        }
        i += l;
    }
    return (base << 8) | (ext << 40);
}

/* B2: the watched extent of a gfx10 image T#: width x height x elem_bytes (WIDTH = w1 [31:30] | w2 [13:0] << 2, + 1; HEIGHT =
 * w2 [29:14] + 1: the gfx10 SQ_IMG_RSRC field positions, SUSPECTED from Mesa's gfx10 layout, not re-derived here), rounded up to
 * 64 KiB and clamped to [N48_C88_EXT_MIN, N48_C88_EXT_MAX]. An estimate for a WATCH, never for a write. Pure.
 * build 0.0.546 ( instrument defect): an UNKNOWN element size (0: the format is not in xlat12_format_elem_bytes) is no
 * longer assumed to be 16 B - the extent is unknown and the watch covers the surface's FIRST PAGE only (N48_C88_EXT_UNKNOWN). */
static inline uint64_t n48_c88_extent(const uint32_t rec[8], uint32_t elem_bytes)
{
    if (!rec) return N48_C88_EXT_MIN;
    if (!elem_bytes) return N48_C88_EXT_UNKNOWN;
    const uint64_t w = (uint64_t)(((rec[1] >> 30) & 3u) | ((rec[2] & 0x3FFFu) << 2)) + 1ull;
    const uint64_t h = (uint64_t)((rec[2] >> 14) & 0xFFFFu) + 1ull;
    uint64_t e = w * h * (uint64_t)elem_bytes;
    e = (e + 0xFFFFull) & ~0xFFFFull;
    return e < N48_C88_EXT_MIN ? N48_C88_EXT_MIN : e > N48_C88_EXT_MAX ? N48_C88_EXT_MAX : e;
}

/* B2: the watch list. VA ranges for copies; VRAM ranges (the surface's first page resolved through the frame's own page tables,
 * then assumed contiguous for the extent: SUSPECTED, labelled so on every line) for MM-window writes. */
typedef struct {
    uint32_t n, lines;
    uint64_t va[N48_C88_WATCH], len[N48_C88_WATCH], vram[N48_C88_WATCH];
    uint32_t vramOk[N48_C88_WATCH];
    uint64_t unlogged;                         /* hits past the line cap */
    uint64_t mmRunEnd;                         /* the MM hook's last hit's end: a contiguous batch run logs once */
    /* build 0.0.546 ( instrument defect: watches survived unmapVA): the context the watch's VA belongs to (0 = unknown:
     * any context's unmap kills it), and DEAD - its VA range was unmapped, so a later resource at that VA is not this surface. */
    uint32_t ctx[N48_C88_WATCH], dead[N48_C88_WATCH];
    uint32_t deadLines;                        /* DEAD lines per boot (capped at N48_C88_WATCH_LINES) */
    uint64_t kills;                            /* watches killed by unmapVA */
} n48_c88_watch;

static inline uint32_t n48_c88_overlaps(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen)
{
    return (alen && blen && a < b + blen && b < a + alen) ? 1u : 0u;
}
/* 1 = added or RE-ARMED (a DEAD watch at the same VA takes the new surface; index in *idx), 0 = already watched (index in *idx) or
 * no room / bad input. `ctx` is the surface's context (0 = unknown). */
static inline uint32_t n48_c88_watch_add_ctx(n48_c88_watch *w, uint32_t ctx, uint64_t va, uint64_t len, uint64_t vram,
                                             uint32_t vramOk, uint32_t *idx)
{
    if (!w || !va || !len) return 0u;
    for (uint32_t i = 0; i < w->n && i < N48_C88_WATCH; i++) {
        if (w->va[i] != va) continue;
        if (idx) *idx = i;
        if (!__atomic_load_n(&w->dead[i], __ATOMIC_ACQUIRE)) return 0u;
        w->len[i] = len; w->vram[i] = vram; w->vramOk[i] = vramOk ? 1u : 0u; w->ctx[i] = ctx;
        __atomic_store_n(&w->dead[i], 0u, __ATOMIC_RELEASE);   /* live again only after the slot holds the new surface */
        return 1u;
    }
    if (w->n >= N48_C88_WATCH) return 0u;
    const uint32_t i = w->n;
    w->va[i] = va; w->len[i] = len; w->vram[i] = vram; w->vramOk[i] = vramOk ? 1u : 0u; w->ctx[i] = ctx; w->dead[i] = 0u;
    __atomic_store_n(&w->n, i + 1u, __ATOMIC_RELEASE);   /* published after the slot: a reader never sees a half-filled one */
    if (idx) *idx = i;
    return 1u;
}
static inline uint32_t n48_c88_watch_add(n48_c88_watch *w, uint64_t va, uint64_t len, uint64_t vram, uint32_t vramOk, uint32_t *idx)
{
    return n48_c88_watch_add_ctx(w, 0u, va, len, vram, vramOk, idx);
}
/* build 0.0.546: an unmapVA of [va, va + size) in context `ctx` (size 0 = the whole space): every LIVE watch of that context
 * (or of an unknown one) whose VA range it overlaps turns DEAD. Returns the mask of watches killed by THIS call. */
static inline uint32_t n48_c88_unmap(n48_c88_watch *w, uint32_t ctx, uint64_t va, uint64_t size)
{
    if (!w) return 0u;
    uint32_t m = 0u;
    const uint32_t n = __atomic_load_n(&w->n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < N48_C88_WATCH; i++) {
        if (__atomic_load_n(&w->dead[i], __ATOMIC_ACQUIRE)) continue;
        if (w->ctx[i] && ctx && w->ctx[i] != ctx) continue;
        if (size && !n48_c88_overlaps(va, size, w->va[i], w->len[i])) continue;
        if (__atomic_exchange_n(&w->dead[i], 1u, __ATOMIC_ACQ_REL)) continue;   /* another unmap killed it first */
        __atomic_add_fetch(&w->kills, 1ull, __ATOMIC_RELAXED);
        m |= 1u << i;
    }
    return m;
}
/* The LIVE watched surface a GPU-VA range [va, va + len) overlaps, or -1. Read-only. */
static inline int n48_c88_hit_va(const n48_c88_watch *w, uint64_t va, uint64_t len)
{
    if (!w) return -1;
    const uint32_t n = __atomic_load_n(&w->n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < N48_C88_WATCH; i++)
        if (!__atomic_load_n(&w->dead[i], __ATOMIC_ACQUIRE) && n48_c88_overlaps(va, len, w->va[i], w->len[i])) return (int)i;
    return -1;
}
/* The LIVE watched surface a VRAM range overlaps (only surfaces whose first page resolved), or -1. Read-only. */
static inline int n48_c88_hit_vram(const n48_c88_watch *w, uint64_t off, uint64_t len)
{
    if (!w) return -1;
    const uint32_t n = __atomic_load_n(&w->n, __ATOMIC_ACQUIRE);
    for (uint32_t i = 0; i < n && i < N48_C88_WATCH; i++)
        if (!__atomic_load_n(&w->dead[i], __ATOMIC_ACQUIRE) && w->vramOk[i] && n48_c88_overlaps(off, len, w->vram[i], w->len[i]))
            return (int)i;
    return -1;
}
/* A hit's line budget: 1 = log it (counted), 0 = past the cap (counted in `unlogged`). */
/* Atomic: the copier threads and the MM-window writers race for it; the cap holds exactly. Returns the line's number (1-based). */
static inline uint32_t n48_c88_take_line(n48_c88_watch *w)
{
    if (!w) return 0u;
    const uint32_t ln = __atomic_add_fetch(&w->lines, 1u, __ATOMIC_RELAXED);
    if (ln > N48_C88_WATCH_LINES) {
        __atomic_store_n(&w->lines, N48_C88_WATCH_LINES, __ATOMIC_RELAXED);
        __atomic_add_fetch(&w->unlogged, 1ull, __ATOMIC_RELAXED);
        return 0u;
    }
    return ln;
}

/* ---- build 0.0.521 Part B: THE PRODUCER BY CONTENT, NOT BY IB SHAPE. ---------------------------------------
 * retracted: IB-length shapes do not identify frames across boots. In run10v the producer is F51, shape 8736|14160
 * (S's draw at IB1 13059 into CB0 0x401560000), while the 8736|14352 frames there are FRAME b (F55); 0.0.513's shape gate made
 * clock88 blind to the one producer of the boot. From 0.0.521 a frame is THE PRODUCER when Apple's own stream holds a DRAW whose
 * in-force fragment program is S (identity 88, ws_S_TimgXhu_Idfr) while a colour target 0 is bound (a non-zero CB_COLOR0_BASE was
 * written earlier in the frame), whatever its IB lengths. How S is recognised, with no new read of any program: the frame's PS
 * VAs at such draws are looked up in the kext's program memo (gXdMemo, n48_sd_memo_peek_key: read-only, the key the identity
 * step already stored for that VA) and compared with S's shader-cache key over APPLE's bytes, N48_C88_S_KEY - the key the
 * generated rows carry for ws_S_TimgXhu_Idfr (src/xlat12/xlat12_adopt_rows.json `"names": ["ws_S_TimgXhu_Idfr"]`, `"keys":
 * ["0xbaf5d840d22a0707"]`; tests/gfx_clock88_test.cpp pins the two equal) and the kext's own PROVENANCE lines print for identity
 * 88 (`program VA 0x40002c100 key 0xbaf5d840d22a0707 identity 88`, run10p..run10v). A PS VA the memo does not hold (evicted,
 * another epoch) is counted `unkeyed` and never makes a frame the producer (the counter can only UNDER-count). PRINT ONLY, as
 * everything in this header: nothing a rule reads, no write of any kind. */
#define N48_C88_S_KEY     0xbaf5d840d22a0707ull   /* ws_S_TimgXhu_Idfr's shader-cache key (APPLE's gfx10 bytes) */
#define N48_C88_CLINES    128u    /* clock88c (content producer) lines per boot, in all */
#define N48_C88_DETAIL_S  128u    /* Part C: mibseg-detail lines per boot for producer frames, a cap of their own, in all */
/* PER-TARGET BUDGETS. By content, S draws in MANY frames: the capture's first 160 frames hold 32 (run10t), 41 (run10u) and 17
 * (run10v) producer frames, nearly all start-up draws into a few surfaces (0x400460000, 0x400400000, 0x400600000, 0x401100000);
 * the clock's SDF draw (run10t F86 0x402380000, run10u F95 0x402540000, run10v F51 0x401560000) came LAST in every one. A plain
 * per-boot cap would be spent on the start-up surfaces before the clock (as found for B1), so each log is budgeted per
 * distinct CB0 target: N48_C88_TGT_MAX targets per boot (more: counted `over`, not logged), and per target at most
 * N48_C88_TGT_CT clock88c lines, N48_C88_TGT_DETAIL producer mibseg-detail lines and N48_C88_TGT_B1 S T# frames (B1/B2). */
#define N48_C88_TGT_MAX    32u
#define N48_C88_TGT_CT     4u
#define N48_C88_TGT_DETAIL 4u
#define N48_C88_TGT_B1     2u
enum { N48_C88_TGT_K_CT = 0u, N48_C88_TGT_K_DETAIL = 1u, N48_C88_TGT_K_B1 = 2u, N48_C88_TGT_KINDS = 3u };
typedef struct {
    uint32_t n, over;
    uint64_t va[N48_C88_TGT_MAX];
    uint32_t used[N48_C88_TGT_MAX][N48_C88_TGT_KINDS];
} n48_c88_tgt;
/* 1 = the target `va` still has budget of `kind` (`per` lines): counted. 0 = spent, a zero VA, a bad kind, or the table full
 * (`over` counted). Pure: writes only the caller's table. */
static inline uint32_t n48_c88_tgt_take(n48_c88_tgt *t, uint64_t va, uint32_t kind, uint32_t per)
{
    if (!t || !va || kind >= N48_C88_TGT_KINDS) return 0u;
    uint32_t i = 0u;
    while (i < t->n && i < N48_C88_TGT_MAX && t->va[i] != va) i++;
    if (i >= t->n || i >= N48_C88_TGT_MAX) {
        if (t->n >= N48_C88_TGT_MAX) { t->over++; return 0u; }
        i = t->n; t->va[i] = va;
        for (uint32_t k = 0; k < N48_C88_TGT_KINDS; k++) t->used[i][k] = 0u;
        t->n++;
    }
    if (t->used[i][kind] >= per) return 0u;
    t->used[i][kind]++;
    return 1u;
}

/* The key of a PS VA, as the caller's program memo holds it (0 = the memo does not say). */
typedef uint64_t (*n48_c88_keyfn)(void *ctx, uint64_t va);
typedef struct {
    uint64_t frame;                                 /* the judged frame this scan belongs to */
    uint32_t cbLo, cbExt, psLo, psHi;               /* Apple's register state, carried across the frame's IBs in order */
    uint32_t ibs, stops;                            /* IBs walked; walks that stopped early */
    uint32_t asks, unkeyed;                         /* key lookups (one per change of the PS drawn with CB0 bound); answered 0 */
    uint64_t lastPs, lastKey; uint32_t lastOk;      /* the last lookup (the next draw of the same PS asks nothing) */
    /* the answer */
    uint32_t producer, sDraws, sIb, sAt;
    uint64_t sCb0, sPs;
} n48_c88_ct;

static inline void n48_c88_ct_begin(n48_c88_ct *c, uint64_t frame)
{
    if (!c) return;
    unsigned char *p = (unsigned char *)c;
    for (uint32_t i = 0; i < (uint32_t)sizeof *c; i++) p[i] = 0u;
    c->frame = frame;
}
/* One IB of the frame (`ib` its index), in frame order: PM4 walked as n48_c88_cb0 walks it (type-2 and the 0xFFFF1000 filler one
 * dword; any other non-type-3 header, or a packet running past n, ends the walk: counted in `stops`). Tracks SPI_SHADER_PGM_LO_PS /
 * _HI_PS (SET_SH_REG 0x76 / SET_SH_REG_INDEX 0x9b, absolute 0x2c08 / 0x2c09: VA = LO << 8 | (HI & 0xff) << 40, as
 * gfx_capture_scan.h reads them) and CB_COLOR0_BASE / _EXT (SET_CONTEXT_REG 0x69, context 0x318 / 0x390). At every draw
 * (xlat12_ib.c's is_draw: 0x2D, 0x27, 0x35, 0x24, 0x25) with CB_COLOR0_BASE non-zero, the in-force PS VA's key is asked of `key`
 * (once per change of that VA); a draw whose key is N48_C88_S_KEY makes the frame THE PRODUCER: counted in `sDraws`, and the first
 * one's IB, dword, CB0 and PS VA kept. A frame holds 35+ distinct programs (run10v F51), so no table of them is kept. Pure,
 * read-only over `in`; `key` is the caller's read-only lookup. */
static inline void n48_c88_ct_walk(n48_c88_ct *c, const uint32_t *in, uint32_t n, uint32_t ib, n48_c88_keyfn key, void *ctx)
{
    if (!c || !in) return;
    c->ibs++;
    uint32_t i = 0u;
    while (i < n) {
        const uint32_t h = in[i];
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) { c->stops++; return; }
        const uint32_t op = (h >> 8) & 0xFFu, cnt = (h >> 16) & 0x3FFFu, l = cnt + 2u;
        if (l > n - i) { c->stops++; return; }
        if ((op == 0x76u || op == 0x9Bu) && cnt >= 1u) {
            const uint32_t r0 = 0x2C00u + (in[i + 1u] & 0xFFFFu);
            for (uint32_t q = 0; q < cnt; q++) {
                if (r0 + q == 0x2C08u) c->psLo = in[i + 2u + q];
                else if (r0 + q == 0x2C09u) c->psHi = in[i + 2u + q];
            }
        } else if (op == 0x69u && cnt >= 1u) {
            const uint32_t o = in[i + 1u] & 0xFFFFu;
            for (uint32_t q = 0; q < cnt; q++) {
                if (o + q == 0x318u) c->cbLo = in[i + 2u + q];
                else if (o + q == 0x390u) c->cbExt = in[i + 2u + q] & 0xFFu;
            }
        } else if ((op == 0x2Du || op == 0x27u || op == 0x35u || op == 0x24u || op == 0x25u) && c->cbLo) {
            const uint64_t ps = ((uint64_t)c->psLo << 8) | ((uint64_t)(c->psHi & 0xFFu) << 40);
            if (!c->lastOk || ps != c->lastPs) {
                c->lastPs = ps; c->lastOk = 1u; c->asks++;
                c->lastKey = key ? key(ctx, ps) : 0ull;
                if (!c->lastKey) c->unkeyed++;
            }
            if (c->lastKey == N48_C88_S_KEY) {
                if (!c->producer) {
                    c->producer = 1u; c->sIb = ib; c->sAt = i; c->sPs = ps;
                    c->sCb0 = ((uint64_t)c->cbLo << 8) | ((uint64_t)c->cbExt << 40);
                }
                c->sDraws++;
            }
        }
        i += l;
    }
}

/* B1/B2 GATE. build 0.0.513 spent the S lines (B1) and the watch (B2) only on the producer's SHAPE 8736|14352;
 * showed that shape is frame b in run10v. build 0.0.521: spent only on a frame the CONTENT scan judged THE PRODUCER
 * (n48_c88_ct_walk), any shape. Returns the line's number (1-based) and counts it in *lines, or 0 (not the producer: nothing
 * consumed; the cap reached: nothing more). Pure: writes only the caller's line counter. */
static inline uint32_t n48_c88_take_frame(uint32_t *lines, uint32_t producer)
{
    if (!lines || !producer) return 0u;
    if (*lines >= N48_C88_LINES) return 0u;
    return ++*lines;
}
/* Part C (build 0.0.521): the mibseg-detail line's per-boot budget. A PRODUCER frame is logged under its own cap
 * (N48_C88_DETAIL_S, counted in *slines) and, once that is spent, under the general one; any other frame under the general cap
 * (`cap`, counted in *lines) as before. Returns 2 (the producer cap), 1 (the general cap) or 0 (not logged). Pure. */
static inline uint32_t n48_c88_detail_take(uint64_t *lines, uint64_t cap, uint64_t *slines, uint32_t producer)
{
    if (!lines || !slines) return 0u;
    if (producer && *slines < N48_C88_DETAIL_S) { (*slines)++; return 2u; }
    if (*lines < cap) { (*lines)++; return 1u; }
    return 0u;
}

/* B3: the two IB-SHAPE classes (8736|14352, 8736|14544) - kept as SHAPE counters only (0.0.521: they name no frame's content;
 *) - and the CONTENT producer's own count (n48_c88_ct_walk), all judged frames and those judged while armed. */
typedef struct { uint64_t p14352, b14544, p14352Armed, b14544Armed, prod, prodArmed, prodLast; } n48_c88_shapes;
static inline void n48_c88_prod_note(n48_c88_shapes *s, uint32_t producer, uint64_t frame, uint32_t armed)
{
    if (!s || !producer) return;
    s->prod++; if (armed) s->prodArmed++;
    s->prodLast = frame;
}
static inline void n48_c88_shape_note(n48_c88_shapes *s, uint32_t nib, const uint32_t *len, uint32_t armed)
{
    if (!s || !len || nib != 2u || len[0] != 8736u) return;
    if (len[1] == 14352u) { s->p14352++; if (armed) s->p14352Armed++; }
    else if (len[1] == 14544u) { s->b14544++; if (armed) s->b14544Armed++; }
}

/* THE LINES (each bounded under N48_LOG_CAP_BODY by tests/gfx_clock88_test.cpp at worst-case values). */
/* B1, one line per kept texture: frame, shape (up to 3 IB lengths, nib), wall s.us, uptime ms, segment, S's draw dword, CB0, the
 * texture index, its surface VA, its 8 raw gfx10 dwords, the watch index (or -1) and the per-boot count. */
#define N48_C88_FMT "clock88: frame %llu shape %u|%u|%u nib %u wall %llu.%06u up %llu ms seg %u S draw@%u CB0 %#llx tex%u VA %#llx " \
                    "T# %08x %08x %08x %08x %08x %08x %08x %08x; watch %d (line %u of %u)"
/* B2, a residency copy onto a watched surface. */
#define N48_C88_COPY_FMT "clock88w: watch %u VA [%#llx,+%#llx) HIT by residency copy #%llu: dst VA %#llx bytes %#llx VRAM %#llx " \
                         "(SDMA chunks %u, MM chunks %u); wall %llu.%06u up %llu ms; pid %d '%s' (line %u of %u)"
/* B2, an MM-window write (outside any residency copy's own scope) onto a watched surface's VRAM run. */
#define N48_C88_MM_FMT "clock88w: watch %u VRAM [%#llx,+%#llx) (VA %#llx; contiguous VRAM SUSPECTED) HIT by an MM-window write at " \
                       "VRAM %#llx, %u dword(s), first %08x; wall %llu.%06u up %llu ms; pid %d '%s' (line %u of %u)"
/* build 0.0.546, B2: a watch killed by unmapVA. args: watch, its VA, extent, the unmapping context, the unmap's VA, size,
 * kills this boot, line, cap. */
#define N48_C88_DEAD_FMT "clock88w: watch %u VA [%#llx,+%#llx) DEAD: ctx %u unmapVA [%#llx,+%#llx) - a later resource at this VA is " \
                         "not this surface; kills %llu; line %u of %u"
/* B2, the watch being set. */
#define N48_C88_ADD_FMT "clock88w: watch %u SET on S's texture %u surface VA %#llx extent %#llx (w %u h %u eb %u), VRAM %#llx%s"
/* B3 and the totals, on the drawelide66 report. */
/* 0.0.521: the two IB-length classes are labelled SHAPE counters (: a shape names no frame's content); the S T# lines (B1)
 * are now spent on content producers. */
#define N48_C88_REPORT_FMT "clock88: SHAPE counters (IB lengths only, not content) 8736|14352 %llu (armed %llu), 8736|14544 %llu " \
                           "(armed %llu); S T# frames logged %u of %u, last %llu; watches %u (%#llx, %#llx), watch lines %u of " \
                           "%u (+%llu past the cap)."
/* 0.0.521 Part B: the CONTENT producer's totals (a second report line). */
#define N48_C88_CREPORT_FMT "clock88: CONTENT producer frames (a draw by S = identity 88, key %#llx, with CB0 bound) %llu (armed " \
                            "%llu), last %llu; producer lines %u of %u; mibseg-detail lines for producer frames %llu of %u; " \
                            "CB0 targets %u of %u (over %u)."
/* 0.0.521 Part B: one line per content-producer frame (N48_C88_CLINES per boot), printed after the frame's IB walk, whatever
 * the translator later does (run10v F51's S draw was never reached by a translation: this line still names it). */
#define N48_C88_CT_FMT "clock88c: frame %llu PRODUCER by content: S draws %u (first IB %u dword %u, CB0 %#llx, PS VA %#llx); " \
                       "shape %u|%u|%u nib %u; key asks %u (unkeyed %u), IBs walked %u (stopped %u); armed %u; " \
                       "up %llu ms (line %u of %u)"

#endif
