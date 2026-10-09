/* gfx_admit112.h - build 0.0.554 (; an internal design note): SWITCH 112 "admit112", ADMIT STALE.
 *
 * `112 | M << 8`: M 1 ON (= 368), M 2 OFF (= 624, the default and the boot value), M 3 SHADOW (= 880); bare `112` reads. INERT unless
 * switches 10, 11, 18, 21 and 45 are ALL ON (nothing is fed, nothing is asked, the callback is not wired). The verb refuses 112 ON
 * while 103 is ON, and 103 ON while 112 is ON; the callback is not wired while 103 is ON in any of its own modes' ON.
 * 0.0.555 (the 0.0.554 review): a copy / lin EVER entry is admitted only when EVERY page of its recorded extent walks to its VRAM
 * (n48_ad_extent_walk, as resprov's ask does); a lapsed precondition WIPES the ledger (n48_ad_precond_gate: frame top, the ask, the
 * feeds); the drops compact in one pass; 112 ON and 110 ON refuse each other at the verbs.
 *
 * WHAT IT DOES. PROVENANCE REFUSED (xlat12 status 29, err_op 0xF7) is decided in ONE place that this switch touches: d_table_desc's image
 * loop (xlat12_ib.c), after every proof ask (ledger, resprov, frame-local) said no. That refusal protects CONTENT only (a texture read
 * whose bytes we cannot prove are the ones we wrote), and only for a TILED record; a DCC record is a layout question and is NEVER
 * admitted. ON admits a tiled, NOT-DCC, table-path record of an ALLOW-LISTED program (X, AN, U, BC, AF, BD: store-free, texels cannot
 * become addresses - ADMIT-STALE-112.md section 1, CONFIRMED on the objects) when its page resolves through the asking frame's VM and the
 * EVER-WRITTEN GUARD holds: this arm's own ledger of surfaces a committed frame or a recorded residency copy demonstrably wrote,
 * matched on (context, VA, mode, base page). `ok = proven || admitted` and every later rung still runs. SHADOW evaluates the SAME
 * question, counts it and returns 0, so its output is OFF's.
 *
 * THE EVER LEDGER (static, EVER_MAX entries, arm-scoped). FED ONLY: (1) at the committed-frame ledger feed, from the entries that
 * feed just wrote (src LEDGER); (2) at resprov RECORD, from a copy that RECORDED (src COPY, or LIN for a backing-sourced one). An
 * unrecorded copy, a neutered frame, a compute write and a CB1-7 target never feed it. DROPPED: on an unmapVA by range (the ledger's own
 * two-sided rule, fail closed), on a WindowServer drop or rebind, on an arm change, and WHOLE when an unmap event was lost. NEVER
 * cleared by the descriptor epoch, by switch 58's un-feed or by switch 109's eviction: that is the point of it. Oldest evicted, counted.
 *
 * Pure: no lock, no clock, no log. Host-tested by tests/gfx_admit112_test.cpp; the translator's half by src/xlat12/tests. */
#ifndef N48_GFX_ADMIT112_H
#define N48_GFX_ADMIT112_H

#include <stdint.h>
#include <string.h>
#include "gfx_desc_port.h"   /* n48_dl_unmap_keeps (the unmap rule), gfx_commit.h, ws_resprov.h (n48_rp_ent / n48_rp_copy / the T# matchers) */

enum { N48_AD_OFF = 0u, N48_AD_ON = 1u, N48_AD_SHADOW = 2u, N48_AD_MODES = 3u };
#define N48_AD_SWITCH   112u
#define N48_AD_EVER_MAX 256u
#define N48_AD_LINES    64u    /* log lines per arm, in all */
#define N48_AD_LINES_ADMIT  24u
#define N48_AD_LINES_REFUSE 24u
#define N48_AD_LINES_SEC    16u
#define N48_AD_SEC_US   1000000ull

/* The verb's M -> the mode. 0 is a read; anything else not listed answers N48_AD_MODES (refused, unchanged). */
static inline uint32_t n48_ad_mode_of_m(uint32_t m)
{
    return m == 1u ? (uint32_t)N48_AD_ON : m == 2u ? (uint32_t)N48_AD_OFF : m == 3u ? (uint32_t)N48_AD_SHADOW : (uint32_t)N48_AD_MODES;
}
static inline const char *n48_ad_mode_name(uint32_t mode)
{
    return mode == N48_AD_ON ? "ON" : mode == N48_AD_SHADOW ? "SHADOW" : "OFF (default)";
}

/* INERT unless every precondition holds: 10 (the descriptor path), 11 (resprov), 18 (the physical-page key), 21 (the deferred drain),
 * 45 (the frame-local list). Each argument is that switch's own value (non-zero = ON). 1 = all hold. */
static inline uint32_t n48_ad_preconds(uint32_t sw10, uint32_t sw11, uint32_t sw18, uint32_t sw21, uint32_t sw45)
{
    return (sw10 && sw11 && sw18 && sw21 && sw45) ? 1u : 0u;
}
/* 1 = the callback may be handed to the translator: 112 is not OFF, its preconditions hold, and 103 is not ON. `st103On` = 103 is ON. */
static inline uint32_t n48_ad_wire(uint32_t mode, uint32_t preconds, uint32_t st103On)
{
    return (mode == N48_AD_ON || mode == N48_AD_SHADOW) && preconds && !st103On ? 1u : 0u;
}
/* 1 = the EVER ledger is fed and scoped (the same condition without the 103 clause: SHADOW must be able to learn beside 103 SHADOW). */
static inline uint32_t n48_ad_feeding(uint32_t mode, uint32_t preconds)
{
    return (mode == N48_AD_ON || mode == N48_AD_SHADOW) && preconds ? 1u : 0u;
}
/* THE VERB EXCLUSION, both directions. 112 ON is refused while 103 is ON; 103 ON is refused while 112 is ON. Every other combination
 * (SHADOW with anything, OFF with anything, 103 SHADOW with 112 ON) is allowed. `newMode112` / `mode103On`, and `newIs103On` / `mode112`. */
static inline uint32_t n48_ad_verb_refuses_112(uint32_t newMode112, uint32_t st103On)
{
    return newMode112 == N48_AD_ON && st103On ? 1u : 0u;
}
static inline uint32_t n48_ad_verb_refuses_103(uint32_t newIs103On, uint32_t mode112)
{
    return newIs103On && mode112 == N48_AD_ON ? 1u : 0u;
}
/* build 0.0.555: the 110 exclusion (ADMIT-STALE-112.md section 2: "do not run 110 ON with 112 ON"), both directions. 110 ON elides AN
 * before the table step, so 112's AN admissions would be dead weight and the two would confound each other's counts. `mode112` is the
 * STORED mode (ON stays ON while a precondition is missing: "ON-pending" is ON here). SHADOW and OFF on either side are allowed. */
static inline uint32_t n48_ad_verb_refuses_112_for_110(uint32_t newMode112, uint32_t an110On)
{
    return newMode112 == N48_AD_ON && an110On ? 1u : 0u;
}
static inline uint32_t n48_ad_verb_refuses_110(uint32_t newIs110On, uint32_t mode112)
{
    return newIs110On && mode112 == N48_AD_ON ? 1u : 0u;
}

/* ---- the allow-list: the six programs whose objects were disassembled (ADMIT-STALE-112.md section 1) ---------------------------- */
#define N48_AD_IDS 6u
static const struct { uint32_t ndw, fnv; const char *name; uint32_t ident; } kN48AdIds[N48_AD_IDS] = {
    { 118u,  0xc5e80d66u, "X",  93u },   /* ws_X_narrow_blur_7_frag_lph */
    { 122u,  0x7b3a6dfeu, "AN", 62u },   /* ws_AN_TmuaXh_Isrc_Isrc */
    { 192u,  0x92c6ae13u, "U",  90u },   /* ws_U_TvcmXh_Isrc */
    { 60u,   0xd53dee91u, "BC", 77u },   /* ws_BC_TimgXh_IsrcCcl */
    { 173u,  0x1051f3f6u, "AF", 54u },   /* ws_AF_variable_blur_downsample_frag_lph */
    { 1271u, 0x3858ea3au, "BD", 78u },   /* ws_BD_glass_background_lph */
};
/* The allow-list index of a program identity (ndw << 32 | fnv), or N48_AD_IDS when it is not on the list. */
static inline uint32_t n48_ad_id_index(uint64_t ps_id)
{
    for (uint32_t k = 0; k < N48_AD_IDS; k++)
        if ((((uint64_t)kN48AdIds[k].ndw << 32) | kN48AdIds[k].fnv) == ps_id) return k;
    return N48_AD_IDS;
}
static inline const char *n48_ad_id_name(uint32_t ix) { return ix < N48_AD_IDS ? kN48AdIds[ix].name : "other"; }

/* ---- the verdicts and the proof-miss reasons -------------------------------------------------------------------------------------- */
/* would-admit / never-written / not-row (the program is not on the allow-list) / DCC / no-page (the base page did not resolve to VRAM)
 * / shape (an entry matched but its element size, or a backing-sourced entry's T#, disagrees). */
enum { N48_AD_V_ADMIT = 0u, N48_AD_V_NEVER = 1u, N48_AD_V_NOROW = 2u, N48_AD_V_DCC = 3u, N48_AD_V_NOPAGE = 4u, N48_AD_V_SHAPE = 5u,
       N48_AD_VERDICTS = 6u };
static inline const char *n48_ad_verdict_name(uint32_t v)
{
    static const char *const n[N48_AD_VERDICTS] = { "would-admit", "never-written", "not-row", "DCC", "no-page", "shape" };
    return v < N48_AD_VERDICTS ? n[v] : "?";
}
/* WHY the proof asks missed, read from the sources' own state (const scans; no counter moves): ledger-none (no source holds the
 * surface at all), ledger-moved (the ledger holds (ctx, VA, mode) on another base page), resprov-epoch (a residency entry matches and
 * only its arm / epoch is stale), resprov-shape (it matches but the element size or the backing T# disagrees), resprov-walk (it matches
 * and its base page no longer walks to its VRAM), none (an entry was found and no reason could be named). */
enum { N48_AD_M_LEDGER_NONE = 0u, N48_AD_M_LEDGER_MOVED = 1u, N48_AD_M_RP_EPOCH = 2u, N48_AD_M_RP_SHAPE = 3u, N48_AD_M_RP_WALK = 4u,
       N48_AD_M_NONE = 5u, N48_AD_MISSES = 6u };
static inline const char *n48_ad_miss_name(uint32_t w)
{
    static const char *const n[N48_AD_MISSES] = { "ledger-none", "ledger-moved", "resprov-epoch", "resprov-shape", "resprov-walk", "none" };
    return w < N48_AD_MISSES ? n[w] : "?";
}
typedef struct { uint32_t ledgerHas, ledgerMoved, rpHas, rpShape, rpEpoch, rpWalk; } n48_ad_missf;
static inline uint32_t n48_ad_miss_why(const n48_ad_missf *f)
{
    if (!f) return N48_AD_M_NONE;
    if (f->ledgerHas) return f->ledgerMoved ? N48_AD_M_LEDGER_MOVED : N48_AD_M_NONE;
    if (f->rpHas) return f->rpShape ? N48_AD_M_RP_SHAPE : f->rpEpoch ? N48_AD_M_RP_EPOCH : f->rpWalk ? N48_AD_M_RP_WALK : N48_AD_M_NONE;
    return N48_AD_M_LEDGER_NONE;
}

/* ---- the EVER ledger ---------------------------------------------------------------------------------------------------------------- */
enum { N48_AD_SRC_LEDGER = 1u, N48_AD_SRC_COPY = 2u, N48_AD_SRC_LIN = 3u };
static inline const char *n48_ad_src_name(uint32_t s)
{
    return s == N48_AD_SRC_LEDGER ? "committed-frame" : s == N48_AD_SRC_COPY ? "residency-copy" : s == N48_AD_SRC_LIN ? "backing-copy" : "-";
}
/* `page` is the base 4 KiB page as a VRAM OFFSET (the same key gfxsrc_rp_walk returns), never 0 in a live entry. `bytes` is the entry's
 * extent when the feed knew it (0 = none: the unmap rule then fails closed). `rp` carries, for a residency copy, exactly the fields
 * n48_rp_record_x stamps on its entry (the element size, and for a backing-sourced one lin / w / h / known) so the SAME matchers judge
 * the T#. `seq` orders eviction. */
typedef struct {
    n48_rp_ent rp;
    uint64_t ctx, va, page, bytes, seq;
    uint32_t mode, src, tok, pad;
} n48_ad_ent;
typedef struct {
    uint32_t n, pad;
    uint64_t arm, seq;
    n48_ad_ent e[N48_AD_EVER_MAX];
    /* counters; wiped with the boot's reset, never with the scope */
    uint64_t fedLedger, fedCopy, fedLin, refreshed, evicted, noPage, noArm, unrecorded;
    uint64_t dropUnmap, dropRebind, dropWs, dropScope, wipeLost, unmaps;
    uint64_t wipePre, dropPre;   /* build 0.0.555: precondition-lapse wipes (and the entries they dropped) */
} n48_ever;

/* Zero the ledger and every counter FIELD BY FIELD: n48_ever is ~30 KB and a `*ev = n48_ever {}` would build that temporary on the
 * caller's stack (the verb runs on Apple's kernel stack). The entries need no clearing: `n` 0 hides them. */
static inline void n48_ad_reset(n48_ever *ev)
{
    ev->n = 0u; ev->pad = 0u; ev->arm = 0ull; ev->seq = 0ull;
    ev->fedLedger = 0ull; ev->fedCopy = 0ull; ev->fedLin = 0ull; ev->refreshed = 0ull; ev->evicted = 0ull; ev->noPage = 0ull;
    ev->noArm = 0ull; ev->unrecorded = 0ull; ev->dropUnmap = 0ull; ev->dropRebind = 0ull; ev->dropWs = 0ull; ev->dropScope = 0ull;
    ev->wipeLost = 0ull; ev->unmaps = 0ull; ev->wipePre = 0ull; ev->dropPre = 0ull;
}
/* build 0.0.555 (0.0.554 review MUST-FIX 2): the precondition gate, called under gXdLock at the frame top, at the ask and at the
 * feeds. Switches 10 / 11 / 18 / 21 / 45 are not mid-arm guarded, so a precondition can lapse mid-arm (with 10 OFF gfxsrc_desc_unmap is
 * skipped and this ledger would miss unmaps). A lapse found here WIPES the ledger (counted: wipePre, dropPre); nothing said before the
 * lapse can vouch after it returns. Returns `preconds` (1 = they hold). */
static inline uint32_t n48_ad_precond_gate(n48_ever *ev, uint32_t preconds)
{
    if (preconds) return 1u;
    if (ev->n) { ev->dropPre += ev->n; ev->n = 0u; ev->wipePre++; }
    return 0u;
}
/* build 0.0.555 (review SHOULD): ONE order-preserving memmove of the tail (the decision returns the FIRST matching entry, so the
 * order is behaviour and swap-with-last would change it). The loops below never call this per entry: they compact in one pass. */
static inline void n48_ad_drop_at(n48_ever *ev, uint32_t k)
{
    if (!ev->n) return;
    if (ev->n > N48_AD_EVER_MAX) ev->n = N48_AD_EVER_MAX;
    if (k < ev->n) {
        const uint32_t tail = ev->n - 1u - k;
        if (tail) memmove(&ev->e[k], &ev->e[k + 1u], (size_t)tail * sizeof ev->e[0]);
    }
    ev->n--;
}
/* The arm scope: an arm different from the one the entries were fed under empties the ledger. `arm` 0 (no arm standing) is a scope with
 * no entries: it empties too and feeds nothing. Returns the entries dropped. */
static inline uint32_t n48_ad_scope(n48_ever *ev, uint64_t arm)
{
    if (ev->arm == arm) return 0u;
    const uint32_t d = ev->n;
    ev->n = 0u; ev->arm = arm; ev->dropScope += d;
    return d;
}
/* Unmaps were LOST (a busy-lock unmap no queue could hold): nothing can be said about what is still mapped, so the whole ledger goes. */
static inline uint32_t n48_ad_wipe_lost(n48_ever *ev)
{
    const uint32_t d = ev->n;
    ev->n = 0u; ev->wipeLost++;
    return d;
}
static inline int n48_ad_is_key(const n48_ad_ent *e, uint64_t ctx, uint64_t va, uint32_t mode, uint64_t page, uint32_t src)
{
    return e->ctx == ctx && e->va == va && e->mode == mode && e->page == page && e->src == src;
}
/* Insert or refresh one entry. Returns 1 when stored. A zero ctx, VA, mode or page is no key and is never stored (`noPage` counts a zero
 * page). Under `arm` 0, or an arm the ledger is not scoped to, nothing is stored (`noArm`). At capacity the OLDEST entry goes. */
static inline uint32_t n48_ad_put(n48_ever *ev, uint64_t arm, const n48_ad_ent *in)
{
    if (!arm || ev->arm != arm) { ev->noArm++; return 0u; }
    if (!in->ctx || !in->va || !in->mode) return 0u;
    if (!in->page || (in->page & 0xfffull)) { ev->noPage++; return 0u; }
    for (uint32_t k = 0; k < ev->n && k < N48_AD_EVER_MAX; k++)
        if (n48_ad_is_key(&ev->e[k], in->ctx, in->va, in->mode, in->page, in->src)) {
            ev->e[k] = *in; ev->e[k].seq = ++ev->seq;
            ev->refreshed++;
            return 1u;
        }
    if (ev->n >= N48_AD_EVER_MAX) {
        uint32_t old = 0u;
        for (uint32_t k = 1; k < ev->n; k++) if (ev->e[k].seq < ev->e[old].seq) old = k;
        n48_ad_drop_at(ev, old);
        ev->evicted++;
    }
    ev->e[ev->n] = *in; ev->e[ev->n].seq = ++ev->seq;
    ev->n++;
    return 1u;
}
/* FEED 1: a surface a COMMITTED frame's translated colour target wrote (one ledger entry the committed-frame feed just wrote). `page` is
 * the entry's base page already normalised to a VRAM offset by the caller (0 = it did not resolve to VRAM: not stored). */
static inline uint32_t n48_ad_feed_ledger(n48_ever *ev, uint64_t arm, uint64_t ctx, uint64_t va, uint32_t mode, uint64_t pageOff,
                                          uint64_t size, uint32_t tok)
{
    n48_ad_ent in {};
    in.ctx = ctx; in.va = va; in.mode = mode; in.page = pageOff; in.bytes = size; in.tok = tok; in.src = N48_AD_SRC_LEDGER;
    const uint32_t ok = n48_ad_put(ev, arm, &in);
    if (ok) ev->fedLedger++;
    return ok;
}
/* FEED 2: a residency copy resprov RECORDED. `why` is n48_rp_record_x's answer: ONLY N48_RP_REC_OK feeds (an unrecorded copy - refused,
 * full, unverified, a wrong owner - is counted in `unrecorded` and never stored). The entry's fields are the ones n48_rp_record_x stamped. */
static inline uint32_t n48_ad_feed_copy(n48_ever *ev, uint64_t arm, const n48_rp_copy *c, uint32_t why)
{
    if (!c || why != N48_RP_REC_OK) { ev->unrecorded++; return 0u; }
    n48_ad_ent in {};
    in.ctx = c->ctx; in.va = c->va; in.mode = c->mode; in.page = c->vram; in.bytes = c->bytes;
    in.src = c->lin ? N48_AD_SRC_LIN : N48_AD_SRC_COPY;
    in.rp.ctx = c->ctx; in.rp.va = c->va; in.rp.vram = c->vram; in.rp.bytes = c->bytes; in.rp.mode = c->mode;
    in.rp.arm = c->arm; in.rp.epoch = c->epoch; in.rp.elemBytes = c->elemBytes;
    in.rp.lin = c->lin ? 1u : 0u; in.rp.w = c->w; in.rp.h = c->h;
    in.rp.known = c->lin >= N48_RP_LIN_KNOWN0 ? c->lin - N48_RP_LIN_KNOWN0 + 1u : 0u;
    const uint32_t ok = n48_ad_put(ev, arm, &in);
    if (ok) { if (in.src == N48_AD_SRC_LIN) ev->fedLin++; else ev->fedCopy++; }
    return ok;
}
/* DROP: an unmapVA of context `ctx` (0: every context) over [va, va + size) - the ledger's own two-sided rule (n48_dl_unmap_keeps): an
 * entry is kept only when the unmap provably cannot have touched it; an unknown scope (size 0), a wrapping range and an entry without an
 * extent all fail closed exactly as the producer ledger's exact drop does. Returns the entries dropped. */
static inline uint32_t n48_ad_unmap_rng(n48_ever *ev, uint64_t ctx, uint64_t va, uint64_t size)
{
    ev->unmaps++;
    uint32_t d = 0u, w = 0u;
    const uint32_t n = ev->n < N48_AD_EVER_MAX ? ev->n : N48_AD_EVER_MAX;
    for (uint32_t k = 0; k < n; k++) {   /* ONE compaction pass, order preserved (was: a shift per dropped entry) */
        const n48_ad_ent *e = &ev->e[k];
        const int mine = (!ctx || e->ctx == ctx);
        if (mine && !n48_dl_unmap_keeps(va, size, e->va, e->bytes)) { ev->dropUnmap++; d++; }
        else { if (w != k) ev->e[w] = ev->e[k]; w++; }
    }
    if (ev->n) ev->n = w;
    return d;
}
/* DROP: WindowServer's binding is now `bound` (0: none) - every entry of any other context goes (resprov's own rebind sweep). */
static inline uint32_t n48_ad_rebind(n48_ever *ev, uint64_t bound)
{
    uint32_t d = 0u, w = 0u;
    const uint32_t n = ev->n < N48_AD_EVER_MAX ? ev->n : N48_AD_EVER_MAX;
    for (uint32_t k = 0; k < n; k++) {   /* ONE compaction pass, order preserved */
        if (!bound || ev->e[k].ctx != bound) { ev->dropRebind++; d++; }
        else { if (w != k) ev->e[w] = ev->e[k]; w++; }
    }
    if (ev->n) ev->n = w;
    return d;
}
/* DROP: WindowServer's binding was dropped (ws_note_gone): every entry goes. */
static inline uint32_t n48_ad_ws_gone(n48_ever *ev)
{
    const uint32_t d = ev->n;
    ev->n = 0u; ev->dropWs += d;
    return d;
}

/* ---- the decision ----------------------------------------------------------------------------------------------------------------- */
/* What the kext knows at the callback: the program identity, the asking frame's context, the record's VA, its translated gfx12 mode and
 * element size, the raw gfx10 record, and the base page as a VRAM offset (`pageOk` 0 = it did not resolve to VRAM). */
typedef struct {
    uint64_t ps_id, ctx, va, page;
    uint32_t mode, elemBytes, pageOk, pad;
    const uint32_t *rec;
    n48_rp_walk walk;   /* build 0.0.555: the asking frame's own page walk (gfxsrc_rp_walk) and its VM: a copy / lin entry's WHOLE extent is walked */
    const void *vm;
} n48_ad_q;
/* extWhy: why a copy / lin entry's extent walk refused (0 = none / not applicable). */
enum { N48_AD_X_NONE = 0u, N48_AD_X_MOVED = 1u, N48_AD_X_UNRESOLVED = 2u, N48_AD_X_NOWALK = 3u };
typedef struct { uint32_t verdict, idIx, clamp, src, matched, extWhy; } n48_ad_r;
/* build 0.0.555 (review MUST-FIX 1): walk EVERY page of a copy / lin entry's recorded extent through the asking frame's VM, exactly as
 * n48_rp_ok_t does (ws_resprov.h: `walk(vm, va + p, &v) != 1 || v != e->vram + p`), and refuse on a page that does not resolve to VRAM or
 * that lands elsewhere: Apple re-mapping a later page (section 843) must not let a stale read reach an invalid PTE. An extent of 0 bytes
 * still walks the base page. An absurd extent (over 4 GiB) is refused as unresolved rather than walked under gXdLock. Returns N48_AD_X_*. */
static inline __attribute__((noinline)) uint32_t n48_ad_extent_walk(const n48_ad_ent *e, const n48_ad_q *q)
{
    if (!q->walk || !q->vm) return N48_AD_X_NOWALK;
    if (e->bytes > 0x100000000ull) return N48_AD_X_UNRESOLVED;
    uint64_t p = 0ull;
    do {
        uint64_t v = ~0ull;
        if (q->walk(q->vm, e->va + p, &v) != 1) return N48_AD_X_UNRESOLVED;
        if (v != e->page + p) return N48_AD_X_MOVED;
        p += 4096ull;
    } while (p < e->bytes);
    return N48_AD_X_NONE;
}
/* THE DECISION. Const on the ledger: it moves no counter. The order is the spec's: the allow-list (not-row), the page (no-page), then the
 * ever-written guard (never-written; shape when entries matched and none agreed). N48_AD_V_ADMIT sets r->clamp (1 for a backing-sourced
 * entry, as n48_rp_ok_t does) and r->src. */
static inline uint32_t n48_ad_decide(const n48_ever *ev, uint64_t arm, const n48_ad_q *q, n48_ad_r *r)
{
    *r = n48_ad_r {};
    r->idIx = n48_ad_id_index(q->ps_id);
    if (r->idIx >= N48_AD_IDS) return r->verdict = N48_AD_V_NOROW;
    if (!q->pageOk || !q->page || (q->page & 0xfffull)) return r->verdict = N48_AD_V_NOPAGE;
    if (!arm || ev->arm != arm || !q->ctx || !q->mode) return r->verdict = N48_AD_V_NEVER;
    uint32_t shape = 0u;
    for (uint32_t k = 0; k < ev->n && k < N48_AD_EVER_MAX; k++) {
        const n48_ad_ent *e = &ev->e[k];
        if (e->ctx != q->ctx || e->va != q->va || e->mode != q->mode || e->page != q->page) continue;
        r->matched++;
        if (e->src == N48_AD_SRC_LEDGER) { r->src = e->src; r->clamp = 0u; r->extWhy = N48_AD_X_NONE; return r->verdict = N48_AD_V_ADMIT; }
        /* a residency entry: the element size must match and be non-zero; a backing-sourced one also needs the T# to match it */
        if (!q->elemBytes || e->rp.elemBytes != q->elemBytes) { shape = 1u; continue; }
        if (e->src == N48_AD_SRC_LIN) {
            if (!q->rec) { shape = 1u; continue; }
            const int tm = e->rp.known ? n48_rp_known_t_match(&e->rp, q->rec) : n48_rp_lin_t_match(&e->rp, q->rec);
            if (!tm) { shape = 1u; continue; }
            const uint32_t xw = n48_ad_extent_walk(e, q);
            if (xw) { if (!r->extWhy) r->extWhy = xw; continue; }
            r->src = e->src; r->clamp = 1u; r->extWhy = N48_AD_X_NONE;
            return r->verdict = N48_AD_V_ADMIT;
        }
        const uint32_t xw = n48_ad_extent_walk(e, q);
        if (xw) { if (!r->extWhy) r->extWhy = xw; continue; }
        r->src = e->src; r->clamp = 0u; r->extWhy = N48_AD_X_NONE;
        return r->verdict = N48_AD_V_ADMIT;
    }
    if (r->extWhy) return r->verdict = N48_AD_V_NOPAGE;   /* a matching entry whose recorded extent no longer walks to its VRAM */
    return r->verdict = shape ? N48_AD_V_SHAPE : N48_AD_V_NEVER;
}

/* ---- the counters, per frame and per second ----------------------------------------------------------------------------------------- */
typedef struct { uint32_t asked, would; } n48_ad_frame;
typedef struct {
    uint64_t asks, admitted, dccSeen, lines;
    uint64_t v[N48_AD_IDS + 1u][N48_AD_VERDICTS];   /* per allow-list identity (+ "other") x verdict */
    uint64_t miss[N48_AD_MISSES];
    uint64_t frames, framesAllWould, framesOnAdmit;
    uint32_t linesAdmit, linesRefuse, linesSec, pad;
    uint64_t extMoved, extUnresolved, extNoWalk;   /* build 0.0.555: refusals by the extent walk, by reason */
} n48_ad_ctr;
static inline void n48_ad_count(n48_ad_ctr *c, uint32_t idIx, uint32_t verdict, uint32_t missWhy, uint32_t countMiss)
{
    c->asks++;
    if (idIx > N48_AD_IDS) idIx = N48_AD_IDS;
    if (verdict < N48_AD_VERDICTS) c->v[idIx][verdict]++;
    if (verdict == N48_AD_V_ADMIT) c->admitted++;
    if (countMiss && missWhy < N48_AD_MISSES) c->miss[missWhy]++;
}
static inline void n48_ad_count_dcc(n48_ad_ctr *c, uint32_t idIx, uint32_t n)
{
    if (idIx > N48_AD_IDS) idIx = N48_AD_IDS;
    c->v[idIx][N48_AD_V_DCC] += n;
    c->dccSeen += n;
}
/* PER FRAME (SHADOW's upper bound: translation stops at the first refusal): a frame whose every asked record would be admitted. */
static inline void n48_ad_frame_begin(n48_ad_frame *f) { f->asked = 0u; f->would = 0u; }
static inline void n48_ad_frame_note(n48_ad_frame *f, uint32_t verdict)
{
    f->asked++;
    if (verdict == N48_AD_V_ADMIT) f->would++;
}
static inline void n48_ad_frame_end(n48_ad_ctr *c, const n48_ad_frame *f, uint32_t committedWithAdmit)
{
    if (f->asked) {
        c->frames++;
        if (f->would == f->asked) c->framesAllWould++;
    }
    if (committedWithAdmit) c->framesOnAdmit++;
}
/* PER SECOND (ON: admissions per identity and committed frames that carried one; SHADOW: would-admits): one window, flushed once its
 * second is over. `flush` 1 = the caller prints `snap` (a non-empty window) and the window has been reset. */
typedef struct { uint64_t startUs; uint32_t adm[N48_AD_IDS + 1u]; uint32_t frames, framesAdm, pad; } n48_ad_win;
static inline void n48_ad_win_add(n48_ad_win *w, uint32_t idIx, uint32_t n)
{
    if (idIx > N48_AD_IDS) idIx = N48_AD_IDS;
    w->adm[idIx] += n;
}
static inline uint32_t n48_ad_win_step(n48_ad_win *w, uint64_t nowUs, uint32_t committed, uint32_t committedWithAdmit, n48_ad_win *snap)
{
    if (!w->startUs) w->startUs = nowUs;
    if (committed) { w->frames++; if (committedWithAdmit) w->framesAdm++; }
    if (nowUs < w->startUs || nowUs - w->startUs < N48_AD_SEC_US) return 0u;
    uint32_t any = w->framesAdm;
    for (uint32_t k = 0; k <= N48_AD_IDS; k++) any |= w->adm[k];
    const uint64_t at = nowUs;
    if (any) *snap = *w;
    *w = n48_ad_win {};
    w->startUs = at;
    return any ? 1u : 0u;
}

/* ---- THE ASK: decide, count, account - the ONE function the kext's callback calls (and the tests drive) ------------------------------- */
/* `why` is the proof-miss reason the caller read (SHADOW only, on-list identities only; ignored otherwise). Returns 1 ONLY when `mode` is
 * ON and the decision is would-admit (`*clamp` = the entry's clamp request); SHADOW, OFF and every refusal answer 0 - OFF's answer.
 * `*logKind`: 0 = print nothing, 1 = an admit line is due (cap N48_AD_LINES_ADMIT per arm), 2 = a refusal line (cap N48_AD_LINES_REFUSE);
 * the caller prints it (with the line number `*logNo`) and this function has already spent the budget. Off-list programs never log. */
typedef struct { n48_ever *ev; n48_ad_ctr *ctr; n48_ad_frame *fr; n48_ad_win *win; uint32_t *frameAdm; } n48_ad_state;
static inline uint32_t n48_ad_ask(const n48_ad_state *S, uint32_t mode, uint64_t arm, const n48_ad_q *q, uint32_t why, uint32_t *clamp,
                                  n48_ad_r *r, uint32_t *logKind, uint32_t *logNo)
{
    *logKind = 0u; *logNo = 0u;
    if (clamp) *clamp = 0u;
    if (mode != N48_AD_ON && mode != N48_AD_SHADOW) { *r = n48_ad_r {}; return 0u; }
    const uint32_t v = n48_ad_decide(S->ev, arm, q, r);
    if (v == N48_AD_V_NOPAGE && r->extWhy) {
        if (r->extWhy == N48_AD_X_MOVED) S->ctr->extMoved++;
        else if (r->extWhy == N48_AD_X_UNRESOLVED) S->ctr->extUnresolved++;
        else S->ctr->extNoWalk++;
    }
    const uint32_t onList = r->idIx < N48_AD_IDS ? 1u : 0u;
    n48_ad_count(S->ctr, r->idIx, v, why, (mode == N48_AD_SHADOW && onList) ? 1u : 0u);
    n48_ad_frame_note(S->fr, v);
    if (v == N48_AD_V_ADMIT) n48_ad_win_add(S->win, r->idIx, 1u);
    const uint32_t adm = v == N48_AD_V_ADMIT ? 1u : 0u;
    uint32_t *lc = adm ? &S->ctr->linesAdmit : &S->ctr->linesRefuse;
    if (onList && *lc < (adm ? N48_AD_LINES_ADMIT : N48_AD_LINES_REFUSE)) {
        (*lc)++; S->ctr->lines++;
        *logKind = adm ? 1u : 2u; *logNo = *lc;
    }
    if (v == N48_AD_V_ADMIT && mode == N48_AD_ON) {
        (*S->frameAdm)++;
        if (clamp) *clamp = r->clamp;
        return 1u;
    }
    return 0u;   /* SHADOW, and every refusal: OFF's answer */
}

/* ---- the lines (each bounded under N48_LOG_CAP_BODY, 491, by the test at worst-case values) ----------------------------------------- */
/* The verb, line 1: mode, how, the preconditions and 103, whether it is wired. Line 2: the EVER ledger, its feeds and its drops (counts
 * print clamped to 32 bits: the lines are bounded at worst case, and a boot's counts are nowhere near it). */
#define N48_AD_FMT "admit112: switch 112 %s (368 ON: admit a tiled non-DCC table read of X/AN/U/BC/AF/BD once written this arm; 624 OFF; " \
    "880 SHADOW)%s; needs 10 %s 11 %s 18 %s 21 %s 45 %s, 103 %s -> %s."
#define N48_AD_EVER_FMT "admit112-ever: EVER %u/%u arm %llu; fed: ledger %u copy %u lin %u; refreshed %u evicted %u no-page %u no-arm %u " \
    "unrecorded-copies %u; dropped: unmap %u rebind %u ws %u arm %u; lost-unmap wipes %u; precondition-lapse wipes %u (%u entries); extent refused %u/%u/%u (moved/unresolved/no-walk)."
/* Lines 2 and 3: identity x verdict (would-admit / never-written / not-row / DCC / no-page / shape), three identities per line; the
 * second also carries the off-list row. Counts print clamped to 32 bits. */
#define N48_AD_ID3_FMT "admit112-ids%s: %s %u/%u/%u/%u/%u/%u, %s %u/%u/%u/%u/%u/%u, %s %u/%u/%u/%u/%u/%u (would-admit/never-written/" \
    "not-row/DCC/no-page/shape)"
/* Line 5: the proof misses, the frames, ON's admissions. */
#define N48_AD_MISS_FMT "admit112-why: asks %u (off the allow-list %u) would-admit/admitted %u; proof missed by: ledger-none %u ledger-moved %u " \
    "resprov-epoch %u resprov-shape %u resprov-walk %u none %u; frames that asked %u, every refused record would admit %u (an upper bound: " \
    "translation stops at the first refusal), committed frames with an admission %u; lines %u."
/* One event line (an admit / a would-admit, or a refusal), capped per arm: the frame, the program, the record, the guard's entry. */
#define N48_AD_EV_FMT "admit112: %s f%llu %s at_i %#x VA %#llx page %#llx mode %u bpe %u ctx %llu -> %s%s%s (line %u of %u)"
/* One per-second line: the window's second, admissions per identity (X AN U BC AF BD other), the committed frames it carried. */
#define N48_AD_SEC_FMT "admit112-sec: %s this second (ON: admissions, SHADOW: would-admit): X %u AN %u U %u BC %u AF %u BD %u other %u; " \
    "committed frames %u, of which with an admission %u (line %u of %u)"

#endif /* N48_GFX_ADMIT112_H */
