// gfx_unitdefer.h - build 0.0.506 ( (1),; notes/design/UNIT-ROOM.md, CONTINUATION-UNITS.md):
// SWITCH 70, THE DEFERRED ROOM RETRY. DEFAULT OFF, REQUIRES 55 (units). PURE: the kext's gfxsrc_policy and the host test
// (tests/gfx_mib_units_checks.h run_frame) call the same functions in the same order.
//
// WHY. A unit is translated while the frame pool still holds only the free runs of the units BEFORE it. Frame b's IB0
// six-constituent unit k1 (and frame a's room-refused units) refuse IB_ERR_DESC / XLAT12_TDESC_NO_ROOM (0xF8) with a 10-dword
// pool run in reach, while at the frame's end the pool holds runs of 10/37/103/81 dwords. With switch 70 ON such a unit
// is not refused on the spot: it is RECORDED, its bytes are put back (Apple's, exactly as a refusal leaves them), nothing
// downstream (copy guard, frame-local feed, fence, read-set unions, counters, the unit480 line) sees that first attempt, and
// after the frame's last unit it is translated ONCE more with the pool as it then is. Whatever that retry answers is the unit's
// FINAL status, and everything downstream of the translate call sees only it; a refusal stays a refusal exactly as today.
//
// THE HAZARD: the retry runs after units the GPU runs AFTER the deferred one. Their outputs are in switch 45's
// frame-local list, so a retry asked against the LIVE list could be "proven" by writes that happen later on the GPU. So the list
// (and the user-data carry) are SNAPSHOT before the unit's first attempt and the retry runs against the snapshot:
//   - n48_mib_defer_pre copies the list's entries (n, e[0..n)) and the carry before EVERY first attempt of a pass that may defer;
//   - n48_mib_defer_take (the unit refused for room, under the cap) keeps that copy as the unit's snapshot AND takes the refused
//     attempt's per-constituent ADDITIONS back out of the live list, so they can vouch for no later unit (a unit's output is
//     evidence only once it has translated - the same rule the per-segment feed follows). build 0.0.508 (LOW-1): only the
//     additions - an entry the attempt REMOVED (a differing re-feed drops it) stays removed (n48_mib_defer_undo_adds);
//   - n48_mib_defer_load puts the snapshot back into the list (and the carry) right before the retry.
// Only the ENTRIES are snapshot and restored: the list's counters (asked, proven, clears ...) are per-boot statistics and are
// never rolled back. The carry: xlat12_ib_translate_draw_ex zeroes `*ex->ud_carry` at the top of every call (xlat12_ib.c, BUILD
// TASK 0.0.449 item 1: "zeroed here instead, unconditionally when in use"), so no value can cross units today; it is snapshot and
// restored anyway so the retry sees what the first attempt saw even if that ever changes.
// After the deferred pass the live list holds the last retry's snapshot plus that retry's feeds; nothing reads it again before
// the next pass clears it (gfxsrc_policy's n48_dl_clear(&gXdFrameLocal) at the pass top).
//
// BOUNDS. At most N48_MIB_DEFER_MAX units per pass are deferred (static storage, no stack); past the cap a unit stays refused on
// the spot (counted `capped`). Each deferred unit is retried exactly once: n48_mib_defer_wanted answers 0 in the deferred pass.
// Only XLAT12_IB_ERR_DESC with err_op XLAT12_TDESC_NO_ROOM, and only a unit (via_unit 1) or a switch-56 single retried through
// the unit path (via_unit 2); every other refusal - PROVENANCE, PAIR, SLOT_*, REEMIT room (0xFC), the copy guard - is final.
#ifndef GFX_UNITDEFER_H
#define GFX_UNITDEFER_H
#include <stdint.h>
#include "xlat12_ib.h"       // xlat12_ud_carry, XLAT12_IB_ERR_DESC, XLAT12_TDESC_NO_ROOM
#include "gfx_desc_port.h"   // n48_dl, n48_dl_ent, N48_DL_MAX: the frame-local list's own types, not a copy

#define N48_MIB_DEFER_MAX  4u            /* deferred units per pass (run10p's maximum is 2 per frame) */
#define N48_MIB_DEFER_NONE 0xFFFFFFFFu
/* build 0.0.511 (see THE ORDERING HOLE below): the write set of this pass's deferred units, the failed asks of the current
 * attempt, and the earlier retries' new frame-local entries. All static (inside gUnitDefer), never stack. Each bound fails CLOSED:
 * a write set or an ask list cut short defers FEWER consumers (they stay refused, as through 0.0.510); an addition list cut short
 * proves LESS at a later retry. */
#define N48_MIB_DEFER_WALL_MAX 128u      /* write-set VAs per pass */
#define N48_MIB_DEFER_ASK_MAX  16u       /* failed asks remembered per attempt */
#define N48_MIB_DEFER_ADDS_MAX 128u      /* earlier retries' new frame-local entries per pass (2 x N48_DL_MAX) */
/* build 0.0.512 (C1, the 0.0.511 review's MEDIUM-1): the KILL SET - (VA, unit, kind) records of every VA a unit
 * changed AFTER a deferral of this pass: a pass-0 unit (N48_MIB_KILL_PASS0) that added, dropped or changed a frame-local entry, or
 * whose input names it as a colour/depth target; a retry (N48_MIB_KILL_RETRY) that removed or changed an entry it was handed.
 * Full: FAIL-CLOSED (every later retry is handed its snapshot with NO augmentation and none of its entries that a retry or a
 * pass-0 unit could have touched - see n48_mib_defer_load_aug). */
#define N48_MIB_DEFER_KILL_MAX 128u
enum { N48_MIB_KILL_PASS0 = 0u, N48_MIB_KILL_RETRY = 1u };
enum { N48_MIB_DEFER_WHY_ROOM = 0u, N48_MIB_DEFER_WHY_PROV = 1u };

typedef struct { uint32_t n; n48_dl_ent e[N48_DL_MAX]; xlat12_ud_carry carry; } n48_mib_defer_snap;
typedef struct {
    uint32_t on;                                /* switch 70, latched ONCE per pass (n48_mib_defer_begin) */
    uint32_t pass;                              /* the kext's loop pass: 0 = every segment, 1 = the deferred ones (static, not stack) */
    uint32_t n;                                 /* units deferred this pass */
    uint32_t capped;                            /* room refusals this pass that found the table full (stayed refused) */
    uint32_t k[N48_MIB_DEFER_MAX];              /* their unit indices, in the order deferred (= GPU order) */
    n48_mib_defer_snap pre;                     /* the list/carry before the CURRENT segment's first attempt */
    n48_mib_defer_snap snap[N48_MIB_DEFER_MAX]; /* each deferred unit's own snapshot */
    /* build 0.0.511 (the ordering hole): */
    uint32_t why[N48_MIB_DEFER_MAX];            /* N48_MIB_DEFER_WHY_ROOM / _PROV, per deferral */
    uint32_t wk[N48_MIB_DEFER_MAX];             /* a PROVENANCE deferral: the earlier deferred unit whose write set held the ask */
    uint64_t wva[N48_MIB_DEFER_MAX];            /* ... and the VA it asked */
    uint32_t nwall, wall_over;                  /* the write set of this pass's deferred units (and VAs it could not hold) */
    uint64_t wall_va[N48_MIB_DEFER_WALL_MAX];
    uint32_t wall_k[N48_MIB_DEFER_WALL_MAX];    /* the deferred unit that writes it (the first one recorded) */
    uint32_t nask, ask_over;                    /* the CURRENT attempt's failed provenance asks (cleared before each attempt) */
    uint64_t ask[N48_MIB_DEFER_ASK_MAX];
    uint32_t prov_capped;                       /* provenance deferrals refused by the cap this pass (the unit stayed refused) */
    uint32_t nadds, adds_over;                  /* this pass's earlier retries' NEW frame-local entries, in retry (= GPU) order */
    n48_dl_ent adds[N48_MIB_DEFER_ADDS_MAX];
    n48_mib_defer_snap loaded;                  /* pass 1: the list as the current retry was handed it (snapshot + augmentation) */
    /* build 0.0.512 (C1): each adds[] entry's producing retry (its unit index), and the kill set. */
    uint32_t adds_k[N48_MIB_DEFER_ADDS_MAX];
    uint32_t nkill, kill_over;
    uint64_t kill_va[N48_MIB_DEFER_KILL_MAX];
    uint32_t kill_k[N48_MIB_DEFER_KILL_MAX];
    uint8_t kill_kind[N48_MIB_DEFER_KILL_MAX];
    uint32_t conflicts;                         /* load_aug: VAs dropped as ambiguous (snapshot vs an earlier retry), this pass */
    uint32_t dropped_c2;                        /* C2: live entries dropped at a deferral (the deferred unit rewrites them), this pass */
} n48_mib_defer;

static inline void n48_mib_defer_copy(n48_mib_defer_snap *to, const n48_dl *fl, const xlat12_ud_carry *carry)
{
    const uint32_t n = (fl && fl->n <= N48_DL_MAX) ? fl->n : 0u;
    to->n = n;
    for (uint32_t i = 0; i < n; i++) to->e[i] = fl->e[i];
    if (carry) to->carry = *carry;
    else { xlat12_ud_carry z; unsigned char *p = (unsigned char *)&z; for (uint32_t i = 0; i < (uint32_t)sizeof z; i++) p[i] = 0u; to->carry = z; }
}
static inline void n48_mib_defer_put(const n48_mib_defer_snap *from, n48_dl *fl, xlat12_ud_carry *carry)
{
    const uint32_t n = from->n <= N48_DL_MAX ? from->n : 0u;
    if (fl) { for (uint32_t i = 0; i < n; i++) fl->e[i] = from->e[i]; fl->n = n; }
    if (carry) *carry = from->carry;
}

/* The pass top: latch the switch, forget the previous pass's deferrals. */
static inline void n48_mib_defer_begin(n48_mib_defer *d, uint32_t on70)
{
    d->on = on70 ? 1u : 0u; d->pass = 0u; d->n = 0u; d->capped = 0u;
    d->nwall = 0u; d->wall_over = 0u; d->nask = 0u; d->ask_over = 0u; d->prov_capped = 0u;   /* build 0.0.511 */
    d->nadds = 0u; d->adds_over = 0u; d->loaded.n = 0u;
    d->nkill = 0u; d->kill_over = 0u; d->conflicts = 0u; d->dropped_c2 = 0u;   /* build 0.0.512 (C1/C2) */
}

/* Is THIS refusal one the pass may defer? `pass` 0 = the frame's normal pass, 1 = the deferred pass (never defers again). */
static inline int n48_mib_defer_wanted(uint32_t on70, uint32_t unit_map, uint32_t pass, uint32_t via_unit, uint32_t st,
                                       uint32_t err_op)
{
    if (!on70 || !unit_map || pass != 0u || (via_unit != 1u && via_unit != 2u)) return 0;
    return st == (uint32_t)XLAT12_IB_ERR_DESC && err_op == (uint32_t)XLAT12_TDESC_NO_ROOM;
}

/* Before a first attempt (pass 0, the switch latched ON, a pass that formed units): the list and carry as they stand. */
static inline void n48_mib_defer_pre(n48_mib_defer *d, const n48_dl *fl, const xlat12_ud_carry *carry)
{
    n48_mib_defer_copy(&d->pre, fl, carry);
}

/* build 0.0.508 (LOW-1 of the 0.0.506 review) — THE ROLLBACK AT A DEFERRAL TAKES BACK ONLY WHAT THE REFUSED
 * ATTEMPT ADDED. Through 0.0.507 n48_mib_defer_take put the whole list back to `pre` (n48_mib_defer_put), which undid the
 * attempt's REMOVALS as well: a differing re-feed drops an existing (ctx, va) entry (n48_dl_set: `l->reFeedDropped++; ...
 * l->n--;`), and restoring it let a LATER unit be proven against the producer's now-stale entry (wrong pixels, 70 ON only).
 * FAIL-CLOSED: the live list keeps an entry only when an IDENTICAL entry (ctx, va, page, size, mode, tok, arm_ep) stood in
 * `pre`, and keeps it with pre's own value (a matching re-feed's `flags` refresh is the attempt's too). So:
 *   an entry the attempt ADDED            -> not in pre            -> dropped (as before);
 *   an entry the attempt REMOVED          -> not in the live list  -> stays removed (the fix);
 *   an entry the attempt REPLACED (58 ON) -> differs from pre's     -> dropped: neither the old nor the new producer's proof.
 * The list only ever shrinks here, never gains an entry, and its order is the live list's (pre's order, minus removals). */
static inline uint32_t n48_mib_defer_ent_same(const n48_dl_ent *a, const n48_dl_ent *b)
{
    return a->ctx == b->ctx && a->va == b->va && a->page == b->page && a->size == b->size && a->mode == b->mode &&
           a->tok == b->tok && a->arm_ep == b->arm_ep;
}
static inline void n48_mib_defer_undo_adds(const n48_mib_defer_snap *pre, n48_dl *fl)
{
    if (!fl) return;
    const uint32_t n = fl->n <= N48_DL_MAX ? fl->n : 0u;
    const uint32_t pn = pre->n <= N48_DL_MAX ? pre->n : 0u;
    uint32_t w = 0u;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = 0u;
        while (j < pn && !n48_mib_defer_ent_same(&fl->e[i], &pre->e[j])) j++;
        if (j < pn) fl->e[w++] = pre->e[j];
    }
    fl->n = w;
}

/* After a translate (and switch 56's retry), in either pass: 1 = deferred (the caller restores the unit's bytes and skips
 * everything downstream for this pass), 0 = the refusal (or success) stands as today - always 0 in the deferred pass (`pass`
 * 1), so a retry is never deferred again. `cap` <= N48_MIB_DEFER_MAX. */
/* ---- build 0.0.511 ( (IB0), (1)) — THE ORDERING HOLE, closed. ------------------------------------------
 * Frame b (run10t F90/F107): IB0's six-constituent unit k1 WRITES 0x401428000 (Apple's CB0_BASE at IB0 3059) and is deferred for
 * ROOM; its refused attempt's feeds are taken back (n48_mib_defer_take), so the single k2 that SAMPLES 0x401428000 (identity 93 at
 * 5940) asks against a list without it and its PROVENANCE refusal was FINAL: k1 translated at its retry, k2 was never asked again
 * (`..P.`). Fail-closed, but it cost frame b. The rule (the harness `FIX70`, scratchpad f86/fu2.cpp, reproduced here):
 *   - THE WRITE SET of a deferred unit (either kind): the VAs its Apple input names as colour/depth targets (gfx10 context
 *     registers CB_COLOR0..7_BASE = 0xA318 + 15i and DB_Z/STENCIL_READ/WRITE_BASE = 0xA010..0xA013, value << 8) plus the
 *     frame-local entries its refused attempt ADDED (before they are taken back). n48_mib_defer_wall_note, at the deferral.
 *   - In pass 0 a unit refused PROVENANCE (0xF7) whose attempt had a FAILED ask (gfxsrc_desc_tiled_ok / _dcc_ok answered 0) for
 *     a VA in the write set of an EARLIER unit of this pass that was deferred is deferred too (n48_mib_defer_take_prov): the same
 *     mechanics as a room deferral - its snapshot is `pre` (the list at its first attempt), its additions are taken back, the
 *     caller restores Apple's bytes and undoes its pool records. A unit is checked against the write sets recorded BEFORE it
 *     (its own is recorded only once it is deferred). Singles too (k2 is one): only the unit table (55) is required.
 *   - The cap (N48_MIB_DEFER_MAX per pass) counts both kinds; at the cap the consumer stays refused (`prov_capped`).
 *   - Pass 1 retries in frame (GPU) order. Each retry's list is its snapshot AUGMENTED with the NEW frame-local entries of the
 *     retries before it (n48_mib_defer_load_aug): only for a VA the snapshot lacks, never replacing an entry. A retry that
 *     translates contributes the entries its list gained over what it was handed (n48_mib_defer_retry_adds, pass 1 only: a
 *     pass-0 unit's feeds - a unit the GPU runs AFTER a deferred one - never reach a retry). A retry refused again is final.
 * Everything here is switch 70's: with 70 OFF nothing is recorded and nothing is deferred (d->on 0). */

/* The current attempt's failed asks: cleared before each attempt (the translate, and switch 56's retry), noted by the ask
 * callbacks while 70 is latched ON in pass 0. */
static inline void n48_mib_defer_ask_clear(n48_mib_defer *d) { d->nask = 0u; }
static inline void n48_mib_defer_ask_note(n48_mib_defer *d, uint64_t va)
{
    if (!d->on || d->pass != 0u) return;
    if (d->nask < N48_MIB_DEFER_ASK_MAX) d->ask[d->nask++] = va;
    else d->ask_over++;
}
static inline void n48_mib_defer_wall_add(n48_mib_defer *d, uint64_t va, uint32_t k)
{
    for (uint32_t i = 0; i < d->nwall && i < N48_MIB_DEFER_WALL_MAX; i++) if (d->wall_va[i] == va) return;
    if (d->nwall >= N48_MIB_DEFER_WALL_MAX) { d->wall_over++; return; }
    d->wall_va[d->nwall] = va; d->wall_k[d->nwall] = k; d->nwall++;
}
/* The colour/depth targets Apple's input [from, to) of the whole frame `in` (n dwords) names: the harness's wscan. It walks PM4
 * from `from` (NOP 0xFFFF1000 and type-2 are one dword; any other non-type-3 header ends the walk) and reads every
 * SET_CONTEXT_REG (opcode 0x69) packet that starts before `to` and ends within 64 dwords of it (and inside the buffer). */
static inline void n48_mib_defer_wscan(n48_mib_defer *d, const uint32_t *in, uint32_t n, uint32_t from, uint32_t to, uint32_t k)
{
    if (!in || to > n) return;
    uint32_t i = from;
    while (i < to) {
        const uint32_t h = in[i];
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) break;
        const uint32_t op = (h >> 8) & 0xFFu, cnt = (h >> 16) & 0x3FFFu;
        if (op == 0x69u && i + 2u + cnt <= to + 64u && i + 2u + cnt <= n) {
            const uint32_t r0 = 0xA000u + (in[i + 1] & 0xFFFFu);
            for (uint32_t q = 0; q < cnt; q++) {
                const uint32_t r = r0 + q, v = in[i + 2u + q];
                const int cb = r >= 0xA318u && r <= 0xA381u && ((r - 0xA318u) % 15u) == 0u;
                const int db = r >= 0xA010u && r <= 0xA013u;
                if ((cb || db) && v) n48_mib_defer_wall_add(d, (uint64_t)v << 8, k);
            }
        }
        i += cnt + 2u;
    }
}
/* At a deferral (either kind), BEFORE the rollback: the unit's write set = its input's targets + its attempt's ADDITIONS (live
 * entries with no identical entry in `pre`). */
/* build 0.0.512: every colour/depth target Apple's input [from, to) names, walked exactly as n48_mib_defer_wscan walks it, handed
 * one by one to `fn` (the write set, C2's drop, C1's pass-0 kills all read the SAME walk). */
typedef void (*n48_mib_defer_tfn)(void *ctx, uint64_t va);
static inline void n48_mib_defer_targets(const uint32_t *in, uint32_t n, uint32_t from, uint32_t to, n48_mib_defer_tfn fn, void *ctx)
{
    if (!in || to > n) return;
    uint32_t i = from;
    while (i < to) {
        const uint32_t h = in[i];
        if (h == 0xFFFF1000u || (h >> 30) == 2u) { i++; continue; }
        if ((h >> 30) != 3u) break;
        const uint32_t op = (h >> 8) & 0xFFu, cnt = (h >> 16) & 0x3FFFu;
        if (op == 0x69u && i + 2u + cnt <= to + 64u && i + 2u + cnt <= n) {
            const uint32_t r0 = 0xA000u + (in[i + 1] & 0xFFFFu);
            for (uint32_t q = 0; q < cnt; q++) {
                const uint32_t r = r0 + q, v = in[i + 2u + q];
                const int cb = r >= 0xA318u && r <= 0xA381u && ((r - 0xA318u) % 15u) == 0u;
                const int db = r >= 0xA010u && r <= 0xA013u;
                if ((cb || db) && v) fn(ctx, (uint64_t)v << 8);
            }
        }
        i += cnt + 2u;
    }
}
/* build 0.0.512 (C2, the 0.0.511 review's MEDIUM-2): drop every live entry for `va` (all contexts). */
static inline uint32_t n48_mib_defer_fl_drop(n48_dl *fl, uint64_t va)
{
    if (!fl) return 0u;
    const uint32_t n = fl->n <= N48_DL_MAX ? fl->n : 0u;
    uint32_t w = 0u, dropped = 0u;
    for (uint32_t i = 0; i < n; i++) { if (fl->e[i].va == va) { dropped++; continue; } fl->e[w++] = fl->e[i]; }
    fl->n = w;
    return dropped;
}
typedef struct { n48_mib_defer *d; n48_dl *fl; } n48_mib_defer_c2ctx;
static inline void n48_mib_defer_c2_fn(void *c, uint64_t va)
{
    n48_mib_defer_c2ctx *x = (n48_mib_defer_c2ctx *)c;
    x->d->dropped_c2 += n48_mib_defer_fl_drop(x->fl, va);
}
static inline void n48_mib_defer_wall_note(n48_mib_defer *d, const uint32_t *in, uint32_t n, uint32_t from, uint32_t to,
                                           uint32_t k, n48_dl *fl)
{
    n48_mib_defer_wscan(d, in, n, from, to, k);
    const uint32_t fn = (fl && fl->n <= N48_DL_MAX) ? fl->n : 0u;
    const uint32_t pn = d->pre.n <= N48_DL_MAX ? d->pre.n : 0u;
    for (uint32_t q = 0; q < fn; q++) {
        uint32_t j = 0u;
        while (j < pn && !n48_mib_defer_ent_same(&fl->e[q], &d->pre.e[j])) j++;
        if (j == pn) n48_mib_defer_wall_add(d, fl->e[q].va, k);
    }
    /* build 0.0.512 (C2): the deferred unit REWRITES its targets at its position in the GPU order, and nothing of this pass
     * can vouch for that write until its retry. So an OLDER entry for a surface it writes (an earlier unit's) must not prove a
     * later pass-0 reader: every live entry for a VA its input names as a target is dropped HERE, before the rollback (which only
     * ever shrinks the list further). A later reader then fails its ask on a VA in this write set and is deferred as a consumer
     * (n48_mib_defer_take_prov*) - fail-closed. The deferred unit's own snapshot is `pre`, untouched: its retry still sees what it
     * saw. Its attempt's ADDITIONS are taken back by n48_mib_defer_undo_adds as before. */
    n48_mib_defer_c2ctx c2 = { d, fl };
    n48_mib_defer_targets(in, n, from, to, &n48_mib_defer_c2_fn, &c2);
}
/* Does a failed ask of the current attempt name a VA in the write set? The LAST such ask answers (the harness's order);
 * `*va`/`*wk` its VA and the deferred unit that writes it. */
static inline uint32_t n48_mib_defer_wall_hit(const n48_mib_defer *d, uint64_t *va, uint32_t *wk)
{
    uint32_t hit = 0u;
    for (uint32_t a = 0; a < d->nask && a < N48_MIB_DEFER_ASK_MAX; a++)
        for (uint32_t i = 0; i < d->nwall && i < N48_MIB_DEFER_WALL_MAX; i++)
            if (d->wall_va[i] == d->ask[a]) { hit = 1u; if (va) *va = d->ask[a]; if (wk) *wk = d->wall_k[i]; break; }
    return hit;
}
/* Record a deferral: the snapshot is `pre` (the list at the first attempt), the attempt's additions are taken back. */
static inline void n48_mib_defer_record(n48_mib_defer *d, uint32_t k, uint32_t why, uint32_t wk, uint64_t wva, n48_dl *fl)
{
    d->k[d->n] = k;
    d->why[d->n] = why; d->wk[d->n] = wk; d->wva[d->n] = wva;
    d->snap[d->n] = d->pre;
    d->n++;
    n48_mib_defer_undo_adds(&d->pre, fl);   /* build 0.0.508 (LOW-1): take back the refused attempt's ADDITIONS only */
}

static inline uint32_t n48_mib_defer_take(n48_mib_defer *d, uint32_t pass, uint32_t k, uint32_t unit_map, uint32_t via_unit,
                                          uint32_t st, uint32_t err_op, uint32_t cap, n48_dl *fl)
{
    if (!n48_mib_defer_wanted(d->on, unit_map, pass, via_unit, st, err_op)) return 0u;
    if (cap > N48_MIB_DEFER_MAX) cap = N48_MIB_DEFER_MAX;
    if (d->n >= cap) { d->capped++; return 0u; }
    n48_mib_defer_record(d, k, N48_MIB_DEFER_WHY_ROOM, N48_MIB_DEFER_NONE, 0ull, fl);
    return 1u;
}
/* build 0.0.511: the kext's room deferral - n48_mib_defer_take, with the unit's write set recorded first (`in` its whole
 * Apple input of n dwords, [from, to) the unit). Same answer as n48_mib_defer_take in every case. */
static inline uint32_t n48_mib_defer_take_w(n48_mib_defer *d, uint32_t pass, uint32_t k, uint32_t unit_map, uint32_t via_unit,
                                            uint32_t st, uint32_t err_op, uint32_t cap, n48_dl *fl, const uint32_t *in,
                                            uint32_t n, uint32_t from, uint32_t to)
{
    if (!n48_mib_defer_wanted(d->on, unit_map, pass, via_unit, st, err_op)) return 0u;
    if (cap > N48_MIB_DEFER_MAX) cap = N48_MIB_DEFER_MAX;
    if (d->n >= cap) { d->capped++; return 0u; }
    n48_mib_defer_wall_note(d, in, n, from, to, k, fl);
    n48_mib_defer_record(d, k, N48_MIB_DEFER_WHY_ROOM, N48_MIB_DEFER_NONE, 0ull, fl);
    return 1u;
}
/* build 0.0.511: THE CONSUMER's deferral, asked BEFORE the room deferral (the harness's order). 1 = deferred (the caller
 * restores Apple's bytes and undoes the pool records, exactly as for a room deferral); 0 = the refusal stands. Never in pass 1. */
static inline uint32_t n48_mib_defer_prov_wanted(uint32_t on70, uint32_t unit_map, uint32_t pass, uint32_t st, uint32_t err_op)
{
    return on70 && unit_map && pass == 0u && st == (uint32_t)XLAT12_IB_ERR_DESC && err_op == (uint32_t)XLAT12_TDESC_PROVENANCE;
}
static inline uint32_t n48_mib_defer_take_prov(n48_mib_defer *d, uint32_t pass, uint32_t k, uint32_t unit_map, uint32_t st,
                                               uint32_t err_op, uint32_t cap, n48_dl *fl, const uint32_t *in, uint32_t n,
                                               uint32_t from, uint32_t to)
{
    if (!n48_mib_defer_prov_wanted(d->on, unit_map, pass, st, err_op)) return 0u;
    uint64_t va = 0ull; uint32_t wk = N48_MIB_DEFER_NONE;
    if (!n48_mib_defer_wall_hit(d, &va, &wk)) return 0u;    /* not a consumer of a deferred unit's writes: final, as before */
    if (cap > N48_MIB_DEFER_MAX) cap = N48_MIB_DEFER_MAX;
    if (d->n >= cap) { d->prov_capped++; return 0u; }       /* the cap counts both kinds: the consumer stays refused */
    n48_mib_defer_wall_note(d, in, n, from, to, k, fl);     /* its own write set, for the consumers AFTER it */
    n48_mib_defer_record(d, k, N48_MIB_DEFER_WHY_PROV, wk, va, fl);
    return 1u;
}

/* build 0.0.512 (C3, the 0.0.511 review's LOW-1): the consumer rule keyed on the REFUSING VA (the translator's ds.prov_va for
 * this PROVENANCE refusal), never on any failed ask of the attempt (an ask that did not refuse - switch 60's strip, the glass
 * probe - could name a write-set VA and waste a cap slot). 0 when `refuse_va` is not in an earlier deferred unit's write set. */
static inline uint32_t n48_mib_defer_wall_hit_va(const n48_mib_defer *d, uint64_t refuse_va, uint32_t *wk)
{
    if (!refuse_va) return 0u;
    for (uint32_t i = 0; i < d->nwall && i < N48_MIB_DEFER_WALL_MAX; i++)
        if (d->wall_va[i] == refuse_va) { if (wk) *wk = d->wall_k[i]; return 1u; }
    return 0u;
}
static inline uint32_t n48_mib_defer_take_prov_va(n48_mib_defer *d, uint32_t pass, uint32_t k, uint32_t unit_map, uint32_t st,
                                                  uint32_t err_op, uint64_t refuse_va, uint32_t cap, n48_dl *fl, const uint32_t *in,
                                                  uint32_t n, uint32_t from, uint32_t to)
{
    if (!n48_mib_defer_prov_wanted(d->on, unit_map, pass, st, err_op)) return 0u;
    uint32_t wk = N48_MIB_DEFER_NONE;
    if (!n48_mib_defer_wall_hit_va(d, refuse_va, &wk)) return 0u;   /* the refusing surface is no deferred unit's write: final */
    if (cap > N48_MIB_DEFER_MAX) cap = N48_MIB_DEFER_MAX;
    if (d->n >= cap) { d->prov_capped++; return 0u; }
    n48_mib_defer_wall_note(d, in, n, from, to, k, fl);
    n48_mib_defer_record(d, k, N48_MIB_DEFER_WHY_PROV, wk, refuse_va, fl);
    return 1u;
}

/* In the deferred pass: which deferral (if any) is unit k. */
static inline uint32_t n48_mib_defer_index(const n48_mib_defer *d, uint32_t k)
{
    for (uint32_t i = 0; i < d->n && i < N48_MIB_DEFER_MAX; i++) if (d->k[i] == k) return i;
    return N48_MIB_DEFER_NONE;
}

/* Right before the retry: the list and carry exactly as they stood at the unit's first attempt. */
static inline void n48_mib_defer_load(const n48_mib_defer *d, uint32_t i, n48_dl *fl, xlat12_ud_carry *carry)
{
    if (i < d->n && i < N48_MIB_DEFER_MAX) n48_mib_defer_put(&d->snap[i], fl, carry);
}
/* build 0.0.511: the kext's load - the snapshot, then the EARLIER retries' new entries for every VA the snapshot lacks (in
 * the order they were fed; never replacing an entry; while the list has room), then `loaded` = what the retry is handed.
 * Returns the entries added. */
/* build 0.0.512 (C1, the 0.0.511 review's MEDIUM-1: (i) the EARLIEST retry's entry won, (ii) a retry's removals were not
 * propagated, (iii) an entry a pass-0 unit between the producer and the reader dropped was resurrected). The rule, per VA X, for
 * the retry of unit ki (retries run in GPU order, so every adds[] entry is an EARLIER retry's):
 *   - adds[] holds only the LATEST retry's entry for X (n48_mib_defer_done deletes a superseded one before appending);
 *   - a kill (X, p) is a change to X at unit p after a deferral: a pass-0 unit's (added / dropped / changed / its input names X as a
 *     target) or a retry's (an entry it was handed removed or changed);
 *   - an adds[] entry for X from retry pA is SKIPPED when any kill (X, p) has pA < p < ki (something the GPU runs between the
 *     producer and this retry touched X: its value is not the producer's any more - (ii), (iii));
 *   - a snapshot entry for X that an earlier retry wrote or changed (an adds[] entry or a retry kill at pA < ki) with NO pass-0
 *     kill (X, p0), pA < p0 < ki, after it is AMBIGUOUS (the snapshot's producer ran before that retry): it is dropped and the
 *     retry's entry is NOT added either (`conflicts`) - fail-closed where 0.0.511 kept the snapshot's older entry;
 *   - the kill set full (kill_over): no augmentation at all, and every snapshot entry for a VA any kill or adds[] entry names is
 *     dropped - fail-closed.
 * Then `loaded` = what the retry is handed. Returns the entries added. */
static inline uint32_t n48_mib_defer_killed_between(const n48_mib_defer *d, uint64_t va, uint32_t lo, uint32_t hi, uint32_t kind_mask)
{
    for (uint32_t q = 0; q < d->nkill && q < N48_MIB_DEFER_KILL_MAX; q++)
        if (d->kill_va[q] == va && d->kill_k[q] > lo && d->kill_k[q] < hi && ((kind_mask >> d->kill_kind[q]) & 1u)) return 1u;
    return 0u;
}
static inline uint32_t n48_mib_defer_load_aug(n48_mib_defer *d, uint32_t i, n48_dl *fl, xlat12_ud_carry *carry)
{
    n48_mib_defer_load(d, i, fl, carry);
    uint32_t added = 0u;
    const uint32_t ki = (i < d->n && i < N48_MIB_DEFER_MAX) ? d->k[i] : 0u;
    const uint32_t na = d->nadds <= N48_MIB_DEFER_ADDS_MAX ? d->nadds : N48_MIB_DEFER_ADDS_MAX;
    const uint32_t nk = d->nkill <= N48_MIB_DEFER_KILL_MAX ? d->nkill : N48_MIB_DEFER_KILL_MAX;
    if (fl && d->kill_over) { d->conflicts += fl->n; fl->n = 0u; }   /* the kill set overflowed: nothing of this pass is trusted */
    else if (fl) {
        /* (1) the snapshot: an entry for a VA an earlier retry wrote or changed, with no pass-0 change to it after that retry,
         *     is ambiguous - dropped (and (2) does not add the retry's entry for it either) */
        const uint32_t n0 = fl->n <= N48_DL_MAX ? fl->n : 0u;
        uint32_t w = 0u;
        for (uint32_t q = 0; q < n0; q++) {
            const uint64_t va = fl->e[q].va;
            uint32_t lastRetry = 0u;   /* 1 + the latest earlier retry that touched va; 0 = none */
            for (uint32_t a = 0; a < na; a++)
                if (d->adds[a].va == va && d->adds_k[a] < ki && d->adds_k[a] + 1u > lastRetry) lastRetry = d->adds_k[a] + 1u;
            for (uint32_t c = 0; c < nk; c++)
                if (d->kill_va[c] == va && d->kill_kind[c] == N48_MIB_KILL_RETRY && d->kill_k[c] < ki && d->kill_k[c] + 1u > lastRetry)
                    lastRetry = d->kill_k[c] + 1u;
            if (lastRetry && !n48_mib_defer_killed_between(d, va, lastRetry - 1u, ki, 1u << N48_MIB_KILL_PASS0)) { d->conflicts++; continue; }
            fl->e[w++] = fl->e[q];
        }
        fl->n = w;
        /* (2) the earlier retries' entries (the latest per VA): only for a VA the snapshot never held, and only when nothing the GPU
         *     runs between that retry and this one touched it */
        const uint32_t sn = d->snap[i < N48_MIB_DEFER_MAX ? i : 0u].n <= N48_DL_MAX ? d->snap[i < N48_MIB_DEFER_MAX ? i : 0u].n : 0u;
        for (uint32_t a = 0; a < na; a++) {
            const uint64_t va = d->adds[a].va;
            if (d->adds_k[a] >= ki) continue;
            if (n48_mib_defer_killed_between(d, va, d->adds_k[a], ki, (1u << N48_MIB_KILL_PASS0) | (1u << N48_MIB_KILL_RETRY))) continue;
            uint32_t held = 0u;
            for (uint32_t q = 0; q < sn && !held; q++) if (d->snap[i].e[q].va == va) held = 1u;
            for (uint32_t q = 0; q < fl->n && q < N48_DL_MAX && !held; q++) if (fl->e[q].va == va) held = 1u;
            if (held) continue;
            if (fl->n < N48_DL_MAX) { fl->e[fl->n++] = d->adds[a]; added++; }
        }
    }
    n48_mib_defer_copy(&d->loaded, fl, carry);
    return added;
}
static inline void n48_mib_defer_kill_add(n48_mib_defer *d, uint64_t va, uint32_t k, uint32_t kind)
{
    for (uint32_t q = 0; q < d->nkill && q < N48_MIB_DEFER_KILL_MAX; q++)
        if (d->kill_va[q] == va && d->kill_k[q] == k && d->kill_kind[q] == kind) return;
    if (d->nkill >= N48_MIB_DEFER_KILL_MAX) { d->kill_over++; return; }
    d->kill_va[d->nkill] = va; d->kill_k[d->nkill] = k; d->kill_kind[d->nkill] = (uint8_t)kind; d->nkill++;
}
/* The entries of `before` with no identical entry in `after`, and of `after` with none in `before`: their VAs are killed at unit k. */
static inline void n48_mib_defer_kill_diff(n48_mib_defer *d, const n48_mib_defer_snap *before, const n48_dl *after, uint32_t k,
                                           uint32_t kind, uint32_t added_too)
{
    const uint32_t bn = before->n <= N48_DL_MAX ? before->n : 0u;
    const uint32_t an = (after && after->n <= N48_DL_MAX) ? after->n : 0u;
    for (uint32_t q = 0; q < bn; q++) {
        uint32_t j = 0u;
        while (j < an && !n48_mib_defer_ent_same(&before->e[q], &after->e[j])) j++;
        if (j == an) n48_mib_defer_kill_add(d, before->e[q].va, k, kind);
    }
    for (uint32_t q = 0; added_too && q < an; q++) {
        uint32_t j = 0u;
        while (j < bn && !n48_mib_defer_ent_same(&after->e[q], &before->e[j])) j++;
        if (j == bn) n48_mib_defer_kill_add(d, after->e[q].va, k, kind);
    }
}
typedef struct { n48_mib_defer *d; uint32_t k; } n48_mib_defer_kctx;
static inline void n48_mib_defer_kill_fn(void *c, uint64_t va)
{
    n48_mib_defer_kctx *x = (n48_mib_defer_kctx *)c;
    n48_mib_defer_kill_add(x->d, va, x->k, N48_MIB_KILL_PASS0);
}
/* build 0.0.511: after a segment's FINAL status, in either pass: a RETRY (pass 1) that translated contributes every live entry
 * with no identical entry in what it was handed. Pass 0 contributes nothing (a unit the GPU runs after a deferred one must never
 * vouch for its retry). */
static inline void n48_mib_defer_retry_adds(n48_mib_defer *d, uint32_t pass, const n48_dl *fl, uint32_t st)
{
    if (pass != 1u || st || !fl) return;
    const uint32_t ln = d->loaded.n <= N48_DL_MAX ? d->loaded.n : 0u;
    for (uint32_t q = 0; q < fl->n && q < N48_DL_MAX; q++) {
        uint32_t j = 0u;
        while (j < ln && !n48_mib_defer_ent_same(&fl->e[q], &d->loaded.e[j])) j++;
        if (j < ln) continue;
        if (d->nadds < N48_MIB_DEFER_ADDS_MAX) { d->adds_k[d->nadds] = 0u; d->adds[d->nadds++] = fl->e[q]; }
        else d->adds_over++;
    }
}
/* build 0.0.512 (C1) — THE KEXT's CALL after every segment's FINAL status (either pass), in place of retry_adds:
 *   pass 0, a unit k translated or refused AFTER a deferral of this pass (d->n > 0): kill every VA its feeds added, dropped or
 *     changed (the live list against `pre`, the list before its first attempt) and every VA its input names as a target;
 *   pass 1, the retry of unit k (either status): kill every VA of an entry it was handed (`loaded`) that it removed or changed;
 *     then, translated, hand its NEW entries on - deleting every older adds[] entry for the same VA first (the latest retry wins),
 *     and killing the VA when adds[] has no room (a stale older entry must not stand in for it).
 * With 70 OFF (d->on 0) nothing is recorded. */
static inline void n48_mib_defer_done(n48_mib_defer *d, uint32_t pass, uint32_t k, const n48_dl *fl, uint32_t st,
                                      const uint32_t *in, uint32_t n, uint32_t from, uint32_t to)
{
    if (!d->on || !fl) return;
    if (pass == 0u) {
        if (!d->n) return;
        n48_mib_defer_kill_diff(d, &d->pre, fl, k, N48_MIB_KILL_PASS0, 1u);
        n48_mib_defer_kctx c = { d, k };
        n48_mib_defer_targets(in, n, from, to, &n48_mib_defer_kill_fn, &c);
        return;
    }
    if (pass != 1u) return;
    n48_mib_defer_kill_diff(d, &d->loaded, fl, k, N48_MIB_KILL_RETRY, 0u);
    if (st) return;
    const uint32_t ln = d->loaded.n <= N48_DL_MAX ? d->loaded.n : 0u;
    for (uint32_t q = 0; q < fl->n && q < N48_DL_MAX; q++) {
        uint32_t j = 0u;
        while (j < ln && !n48_mib_defer_ent_same(&fl->e[q], &d->loaded.e[j])) j++;
        if (j < ln) continue;
        uint32_t w = 0u;   /* the latest retry wins: every older entry for this VA goes */
        for (uint32_t a = 0; a < d->nadds && a < N48_MIB_DEFER_ADDS_MAX; a++)
            if (d->adds[a].va != fl->e[q].va) { d->adds[w] = d->adds[a]; d->adds_k[w] = d->adds_k[a]; w++; }
        d->nadds = w;
        if (d->nadds < N48_MIB_DEFER_ADDS_MAX) { d->adds[d->nadds] = fl->e[q]; d->adds_k[d->nadds] = k; d->nadds++; }
        else { d->adds_over++; n48_mib_defer_kill_add(d, fl->e[q].va, k, N48_MIB_KILL_RETRY); }
    }
}

/* THE defer70 REPORT LINE (one line per `gfxneuter 70` verb; bounded under the 491-byte log body by
 * tests/gfx_mib_units_checks.h at 20-digit counters). `70 | M << 8`: M 1 ON (= 326), M 2 OFF (= 582, the default and the boot
 * value), bare `70` reads. args: ON/OFF, how, the note (INERT without 55), passes that deferred, units deferred, retried,
 * retried and translated, retried and refused (and of those still NO_ROOM), refusals past the cap, pool dwords undone at a
 * deferral. */
#define N48_DEFER70_FMT "defer70: switch 70 (deferred room retry, needs 55) %s%s%s. passes %llu deferred %llu retried %llu " \
                        "translated %llu refused %llu (still no-room %llu); capped %llu; undone %llu dw."

/* build 0.0.511: the second report line (the ordering hole's counts) and the per-deferral / per-retry lines (capped 16 per boot
 * together). Bounded by tests/gfx_mib_units_checks.h at 20-digit counters. */
#define N48_DEFER70B_FMT "defer70: ordering (0.0.511): provenance deferrals %llu (capped %llu), their retries translated %llu " \
                         "refused %llu; entries from earlier retries %llu; write set full %llu, asks over %llu, additions over %llu."
/* build 0.0.512 (C1/C2): the third report line - snapshot entries dropped as ambiguous at a retry's load, live entries dropped at
 * a deferral (the deferred unit rewrites them), kill records that did not fit (each one made a retry's list EMPTY). */
#define N48_DEFER70C_FMT "defer70: kill set (0.0.512): ambiguous entries dropped at a retry %llu; entries dropped at a deferral %llu; " \
                         "kill set full %llu."
#define N48_DEFER70_ROOM_FMT "defer70: frame %llu k%u deferred room (line %u of at most 16)"
#define N48_DEFER70_PROV_FMT "defer70: frame %llu k%u deferred provenance on %#llx (writes-of k%u) (line %u of at most 16)"
#define N48_DEFER70_RETRY_FMT "defer70: frame %llu retry k%u (%s, +%u earlier-retry entries) %s op %#x (line %u of at most 16)"

#endif /* GFX_UNITDEFER_H */
