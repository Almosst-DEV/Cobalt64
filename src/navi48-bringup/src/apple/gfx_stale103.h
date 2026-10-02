// gfx_stale103.h — build 0.0.544 ( ranked item (1), from the clock-and-interaction study of RUN AU): SWITCH 103,
// "stale103", THE STALE-INPUT GATE, and the interaction instruments. PURE: compiled into the kext and into
// tests/gfx_stale103_test.cpp. Writes no register, no page table, nothing of Apple's.
//
// THE PROBLEM: a COMMITTED frame may sample a surface whose most recent producer frame we REFUSED
// (neutered). The provenance ask (gfxsrc_desc_tiled_ok / gfxsrc_desc_dcc_ok) answers from the ledger (any committed producer of
// that (ctx, VA, mode) this arm), resprov (a verified residency copy) or the frame-local list (an earlier segment of THIS frame):
// none of them asks whether a LATER producer of the same pixels was refused. U's clock composite reads 0x401160000, whose S
// producers are all refused (identity 88), and draws whatever that surface held: the dots.
//
// THE TABLE. N48_ST_ROWS rows keyed by PHYSICAL page (the base 4 KiB page of a colour target, as the frame's own walk resolved
// it; never the VA - Apple re-maps VAs mid-run,). Each row: the page, the VA it was last seen under (log only), the judged
// frame that last named it as a colour target, whether that frame was COMMITTED (EXEC) or not (NOT_EXEC), the last committed
// frame that named it (exec_seq), and a coverage field (N48_ST_COV_UNKNOWN: the feed does not know whether a write covered the
// whole surface; kept for a later build). Fed by every judged frame of an ARMED arm (the kext: the witness's IB-end target rows
// hVa/hPage plus every per-draw CB target, gfx_capture_scan.h n48_gcap_cbt_ib). ARM-SCOPED: the key is the arm's own
// armed_at_us; a feed or an ask under a different key finds nothing (a feed resets the table first). BOUNDED REPLACEMENT: when
// all rows are used, the row evicted is the OLDEST EXEC row (by frame); only when every row is NOT_EXEC is the oldest NOT_EXEC
// row evicted, and that loss is counted (`lost`: a refused writer the gate can no longer see - the fail-open direction, named).
//
// THE RULE, asked only AFTER a source already answered yes: the texture's page has a row, the row's last writer was NOT
// EXECUTED, and that writer is NEWER than the proof:
//   ledger proof      its producer is a committed frame; the newest committed write to THIS page the table knows is exec_seq,
//                     so the proof's frame is exec_seq (0 when this arm never committed a write to this page - the ledger entry
//                     then proves some other page or predates the arm: the refusal direction);
//   resprov proof     0 here, because the kext marks every RECORDED residency copy as an EXEC writer of its destination's
//                     base page at the last judged frame (st103_copy_note): a copy newer than the refused writer leaves the row
//                     EXEC (fresh); a refused writer after the copy is newer than it (stale);
//   frame-local proof this frame: never older than a writer - EXEMPT (same frame), counted.
// EXEMPTIONS (each counted, never silent): the texture page equals the page of the draw's OWN CB0 (a backdrop self-read: without
// it the login layer latches,), and a proof from earlier in the SAME frame (frame-local, or the frame-local list also
// holds the surface).
//
// `accel gfxneuter 103 | M << 8 [| payload << 12]`: M 1 ON (= 359: a STALE answer turns the yes into a no), M 2 OFF (= 615, the
// default and the boot value: nothing is fed, nothing asked, every yes stands), M 3 SHADOW (= 871: counted and logged, every yes
// stands), bare `103` reads; M 4 / M 5 set box A / box B (the committed-draw window, payload x0 | y0 << 12 | x1 << 24 |
// y1 << 36, 12 bits each, x0 < x1, y0 < y1), M 6 sets the re-open window (payload S seconds, 1..1200, after the arm's
// CONTINUOUS START; 0 cancels). Anything else is refused unchanged. Joins the continuous mid-arm guard (reads stay allowed).
//
// build 0.0.545 ('s MUST-FIX and SHOULD-FIX): ON MADE SAFE AND MEASURABLE.
//   (1) THE SIMULATED-ON TABLE. SHADOW feeds the real table with the gate's commitOk, so it records what SHADOW did, never what ON
//       would have: a refused frame's targets under ON would be NOT_EXEC and ON's cascade stays invisible. A second table
//       (the kext's gSt103Sim) is fed with n48_st_sim_exec(commitOk, a STALE hit against the simulated table this frame) and asked
//       beside the real one; its answers never change a verdict (SHADOW's keep is 1 for every verdict). Per frame it counts the
//       frames ON would refuse (P-shaped vs other), and simulated-ON delivered P frames vs actual; per plane/layer row the time
//       from NOT_EXEC back to EXEC (n48_st_mark's recovery statistics) and, at STOP, the rows still NOT_EXEC (n48_st_open).
//   (2) THE FRAME-LEVEL CB TARGET EXEMPTION (N48_ST_V_EXEMPT_FCB): a texture page that is ANY colour target of the SAME frame
//       (the per-draw pass's full set plus the IB-end rows, n48_st_fset, filled BEFORE the policy pass asks) is exempt. It breaks
//       the Z blur latch: Z reads the layer and writes the scratch, U reads the scratch and writes the layer; in ONE frame each
//       read is the other draw's target. ONLY when the set is PROVEN complete (no unseen target, no short walk, no overflow, every
//       target resolved: n48_st_fset_close); an incomplete set exempts nothing (the stricter ON).
//   (3) THE P POLICY (N48_ST_V_P_ONCE): a P-shaped frame (gfx_present73.h n48_p73_is_p_shape) is refused only for a page this arm
//       NEVER committed (exec_seq 0). A page once committed whose last writer was refused is counted, not refused, for P. The
//       clock (S1) is still refused: its page 0x13b6f000 was never committed this arm (every S frame refused) and U is not a P;
//       latch (b) is broken: a P plane / layer page that committed once cannot refuse a later P.
//   The key holds isSys (n48_st_key: bit 63), so one page number in system memory and in VRAM are two rows.
//
// build 0.0.546 ('s MUST-FIX and SHOULD-FIXes):
//   (1) P_ONCE MEMORY: a P frame's LEDGER proof counts as once-committed (n48_st_classify), and n48_st_slot never evicts a plane /
//       layer (role) row while a row without a role can go; when only role rows remain the oldest goes, counted evictRole, which
//       voids ON evidence at STOP like LOST (n48_st_void, both tables). Nothing asks either count (never fail closed,).
//   (2) the frame's colour-target set is INCOMPLETE when the frame had no proven reader or the IB loop skipped (or never read) an IB
//       (n48_st_fset_blind: N48_ST_FS_NOREAD / N48_ST_FS_IBSKIP).
//   (3) the verb's st103_report2 lines are LABELLED "(previous arm)" when no arm is scoped (N48_ST_PREV_FMT), never reset there.
#ifndef N48_GFX_STALE103_H
#define N48_GFX_STALE103_H

#include <stdint.h>
#include "gfx_p87.h"
#include "gfx_present73.h"

#define N48_ST_SWITCH 103u
enum { N48_ST_M_READ = 0u, N48_ST_M_ON = 1u, N48_ST_M_OFF = 2u, N48_ST_M_SHADOW = 3u,
       N48_ST_M_BOX_A = 4u, N48_ST_M_BOX_B = 5u, N48_ST_M_WIN = 6u };

/* ---- the verb ---- */
typedef struct { uint32_t m, ok; uint32_t x0, y0, x1, y1; uint32_t secs; } n48_st_arg;
#define N48_ST_WIN_MAX_S 1200u
static inline n48_st_arg n48_st_decode(uint64_t arg)
{
    n48_st_arg d = { 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    d.m = (uint32_t)((arg >> 8) & 0xFu);
    const uint64_t pay = arg >> 12;
    if (d.m <= N48_ST_M_SHADOW) { d.ok = pay == 0ull ? 1u : 0u; return d; }
    if (d.m == N48_ST_M_BOX_A || d.m == N48_ST_M_BOX_B) {
        d.x0 = (uint32_t)(pay & 0xFFFu); d.y0 = (uint32_t)((pay >> 12) & 0xFFFu);
        d.x1 = (uint32_t)((pay >> 24) & 0xFFFu); d.y1 = (uint32_t)((pay >> 36) & 0xFFFu);
        d.ok = ((pay >> 48) == 0ull && d.x0 < d.x1 && d.y0 < d.y1) ? 1u : 0u;
        return d;
    }
    if (d.m == N48_ST_M_WIN) {
        d.secs = (uint32_t)(pay & 0xFFFu);
        d.ok = ((pay >> 12) == 0ull && d.secs <= N48_ST_WIN_MAX_S) ? 1u : 0u;
        return d;
    }
    return d;   /* M 7..15: refused */
}
static inline uint64_t n48_st_box_arg(uint32_t m, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    return (uint64_t)N48_ST_SWITCH | ((uint64_t)(m & 0xFu) << 8) | ((uint64_t)(x0 & 0xFFFu) << 12) |
           ((uint64_t)(y0 & 0xFFFu) << 24) | ((uint64_t)(x1 & 0xFFFu) << 36) | ((uint64_t)(y1 & 0xFFFu) << 48);
}
/* 1 while the gate is fed and asked (ON or SHADOW); OFF and anything unknown are 0. */
static inline uint32_t n48_st_active(uint32_t mode) { return (mode == N48_ST_M_ON || mode == N48_ST_M_SHADOW) ? 1u : 0u; }
static inline const char *n48_st_mode_name(uint32_t m)
{
    return m == N48_ST_M_ON ? "ON (359: a stale input turns the provenance yes into a no)"
         : m == N48_ST_M_SHADOW ? "SHADOW (871: stale inputs counted and logged, every yes stands)"
         : "OFF (615, default: nothing fed, nothing asked)";
}

/* ---- the table ---- */
#define N48_ST_ROWS 256u
enum { N48_ST_NOT_EXEC = 0u, N48_ST_EXEC = 1u };
enum { N48_ST_COV_UNKNOWN = 0u, N48_ST_COV_FULL = 1u, N48_ST_COV_PARTIAL = 2u };
/* build 0.0.545: the row's role - a PLANE (a colour target of a P-shaped frame) or a LAYER (a page a P-shaped frame read). */
enum { N48_ST_ROLE_PLANE = 1u, N48_ST_ROLE_LAYER = 2u };
/* `page` holds the KEY (n48_st_key: the page with isSys in bit 63). ns_us: when the row last turned NOT_EXEC (the table's clock). */
typedef struct { uint64_t page, va, seq, exec_seq, ns_us; uint32_t verdict, cov, role; } n48_st_row;
typedef struct {
    uint64_t arm;                     /* the arm key the rows belong to (0: none) */
    uint32_t used;
    uint32_t role_now;                /* build 0.0.545: ORed into every row marked while set (the feed: PLANE for a P frame) */
    uint64_t now;                     /* build 0.0.545: the feed's clock (us), set once per judged frame */
    n48_st_row r[N48_ST_ROWS];
    uint64_t resets, markExec, markRef, unresolved, evictExec, lost, outOfOrder, trunc, frames;
    uint64_t recN, recMaxUs, recOver1s;   /* build 0.0.545: plane/layer rows NOT_EXEC -> EXEC: count, the longest, > 1 s */
    uint64_t evictRole;                   /* build 0.0.546: a plane/layer (role) row evicted - voids ON evidence like LOST */
} n48_st_tab;

static inline void n48_st_reset(n48_st_tab *t, uint64_t arm)
{
    if (!t) return;
    t->used = 0u;
    t->arm = arm;
    t->resets++;
}
/* The feed's first call per frame: a different arm key empties the table (arm-scoped). */
static inline void n48_st_arm_scope(n48_st_tab *t, uint64_t arm)
{
    if (t && t->arm != arm) n48_st_reset(t, arm);
}
/* THE KEY: the physical page, never the VA (the VA is kept in the row for the log only), with isSys in bit 63 (build
 * 0.0.545, SHOULD-FIX: a system page and a VRAM page may share a number). A page of 0 (did not resolve) is key 0. */
#define N48_ST_KEY_SYS (1ull << 63)
static inline uint64_t n48_st_key(uint64_t page, uint32_t sys) { return page ? (page | (sys ? N48_ST_KEY_SYS : 0ull)) : 0ull; }
static inline n48_st_row *n48_st_find(n48_st_tab *t, uint64_t page)
{
    if (!t || !page) return 0;
    for (uint32_t k = 0; k < t->used && k < N48_ST_ROWS; k++)
        if (t->r[k].page == page) return &t->r[k];
    return 0;
}
static inline const n48_st_row *n48_st_find_c(const n48_st_tab *t, uint64_t arm, uint64_t page)
{
    if (!t || !page || !arm || t->arm != arm) return 0;
    for (uint32_t k = 0; k < t->used && k < N48_ST_ROWS; k++)
        if (t->r[k].page == page) return &t->r[k];
    return 0;
}
/* The slot for a new row: a free one, else the OLDEST EXEC row WITHOUT a role, else the OLDEST NOT_EXEC row without a role (counted
 * `lost`), else - only role rows remain - the OLDEST role row (counted `evictRole`, and `lost` too when it was NOT_EXEC).
 * build 0.0.546: a plane/layer row is NEVER evicted while a row without a role can be.'s latch:
 * an evicted plane row next marked by a refused writer came back with exec_seq 0, and every later P reading it answered STALE.
 * evictRole > 0 voids ON evidence at STOP exactly as LOST does (n48_st_void); nothing asks it (never fail closed,). */
static inline n48_st_row *n48_st_slot(n48_st_tab *t)
{
    if (t->used < N48_ST_ROWS) return &t->r[t->used++];
    uint32_t ex = N48_ST_ROWS, ne = N48_ST_ROWS, ro = 0u;
    for (uint32_t k = 0; k < N48_ST_ROWS; k++) {
        const n48_st_row *r = &t->r[k];
        if (r->role) { if (r->seq < t->r[ro].seq || !t->r[ro].role) ro = k; }
        else if (r->verdict == N48_ST_EXEC) { if (ex == N48_ST_ROWS || r->seq < t->r[ex].seq) ex = k; }
        else if (ne == N48_ST_ROWS || r->seq < t->r[ne].seq) ne = k;
    }
    if (ex != N48_ST_ROWS) { t->evictExec++; return &t->r[ex]; }
    if (ne != N48_ST_ROWS) { t->lost++; return &t->r[ne]; }
    t->evictRole++;
    if (t->r[ro].verdict == N48_ST_NOT_EXEC) t->lost++;   /* a refused writer fell out with it */
    return &t->r[ro];
}
/* build 0.0.546: the STOP report's "ON EVIDENCE VOID" condition - a refused writer fell out (LOST) or a plane/layer row was
 * evicted (evictRole), in the real OR the simulated table. Read by the report only, never by an ask, the gate or the feed. */
static inline uint32_t n48_st_void(const n48_st_tab *real, const n48_st_tab *sim)
{
    return ((real && (real->lost || real->evictRole)) || (sim && (sim->lost || sim->evictRole))) ? 1u : 0u;
}
/* One colour target of judged frame `seq`, by KEY: `exec` 1 = the frame committed. A key of 0 (did not resolve) is counted, never
 * recorded. A mark older than the row's (frames out of order) is counted and ignored. build 0.0.545: a plane/layer row that
 * turns EXEC again records how long it was NOT_EXEC (t->now - ns_us). */
static inline void n48_st_mark_key(n48_st_tab *t, uint64_t va, uint64_t key, uint64_t seq, uint32_t exec, uint32_t cov)
{
    if (!t) return;
    if (!key) { t->unresolved++; return; }
    n48_st_row *r = n48_st_find(t, key);
    if (r && r->seq > seq) { t->outOfOrder++; return; }
    const uint32_t wasNot = (r && r->verdict == N48_ST_NOT_EXEC) ? 1u : 0u;
    if (!r) {
        r = n48_st_slot(t);
        r->page = key; r->exec_seq = 0ull; r->role = 0u; r->ns_us = 0ull;
    }
    r->role |= t->role_now;
    r->va = va; r->seq = seq; r->verdict = exec ? N48_ST_EXEC : N48_ST_NOT_EXEC; r->cov = cov;
    if (exec) {
        if (wasNot && r->role) {
            const uint64_t d = t->now > r->ns_us ? t->now - r->ns_us : 0ull;
            t->recN++;
            if (d > t->recMaxUs) t->recMaxUs = d;
            if (d > 1000000ull) t->recOver1s++;
        }
        r->exec_seq = seq; r->ns_us = 0ull; t->markExec++;
    } else {
        if (!wasNot) r->ns_us = t->now;
        t->markRef++;
    }
}
static inline void n48_st_mark(n48_st_tab *t, uint64_t va, uint64_t page, uint32_t sys, uint64_t seq, uint32_t exec, uint32_t cov)
{
    n48_st_mark_key(t, va, n48_st_key(page, sys), seq, exec, cov);
}
/* build 0.0.545: a P-shaped frame read this key (a LAYER): the row, if any, is tagged (for the recovery statistics). */
static inline void n48_st_role(n48_st_tab *t, uint64_t arm, uint64_t key, uint32_t role)
{
    if (!t || !key || !arm || t->arm != arm) return;
    n48_st_row *r = n48_st_find(t, key);
    if (r) r->role |= role;
}
/* build 0.0.545: the plane/layer rows STILL NOT_EXEC at `now` (a latch never comes back, so STOP must count these too). */
static inline void n48_st_open(const n48_st_tab *t, uint64_t now, uint64_t *n, uint64_t *max_us, uint64_t *over1s)
{
    uint64_t c = 0ull, mx = 0ull, o = 0ull;
    for (uint32_t k = 0; t && k < t->used && k < N48_ST_ROWS; k++) {
        const n48_st_row *r = &t->r[k];
        if (r->verdict != N48_ST_NOT_EXEC || !r->role) continue;
        const uint64_t d = now > r->ns_us ? now - r->ns_us : 0ull;
        c++;
        if (d > mx) mx = d;
        if (d > 1000000ull) o++;
    }
    if (n) *n = c;
    if (max_us) *max_us = mx;
    if (over1s) *over1s = o;
}

/* ---- the ask ---- */
enum { N48_ST_P_LEDGER = 0u, N48_ST_P_RESPROV = 1u, N48_ST_P_FRAMELOCAL = 2u, N48_ST_PROOFS = 3u };
static inline const char *n48_st_proof_name(uint32_t p)
{
    return p == N48_ST_P_LEDGER ? "ledger" : p == N48_ST_P_RESPROV ? "resprov" : p == N48_ST_P_FRAMELOCAL ? "frame-local" : "?";
}
enum {
    N48_ST_V_NOPAGE = 0u,   /* the texture's page did not resolve: not judged (counted) */
    N48_ST_V_NOROW,         /* no row this arm: no refused writer known */
    N48_ST_V_FRESH,         /* the last writer committed */
    N48_ST_V_OLDER,         /* the refused writer is not newer than the proof */
    N48_ST_V_EXEMPT_SELF,   /* stale, but the draw's own CB0 page: a backdrop self-read */
    N48_ST_V_EXEMPT_SAME,   /* stale, but proven earlier in the SAME frame */
    N48_ST_V_STALE,         /* the last writer was refused and is newer than the proof */
    N48_ST_V_EXEMPT_FCB,    /* build 0.0.545: stale, but a colour target of the SAME frame (the set proven complete) */
    N48_ST_V_P_ONCE,        /* build 0.0.545: stale, but the asker is a P frame and the page committed once this arm */
    N48_ST_VERDICTS
};
static inline const char *n48_st_verdict_name(uint32_t v)
{
    static const char *const n[N48_ST_VERDICTS] = { "no-page", "no-row", "fresh", "older", "exempt-self", "exempt-same-frame",
                                                    "STALE", "exempt-frame-cb", "p-once-committed" };
    return v < N48_ST_VERDICTS ? n[v] : "?";
}
typedef struct {
    uint64_t arm;          /* the asking frame's arm key */
    uint64_t frame;        /* the asking (consumer) frame */
    uint64_t va;           /* the texture's VA (the log; never the key) */
    uint64_t page;         /* the texture's base page, resolved through the asking frame's own VM (0 = did not resolve) */
    uint32_t proof;        /* N48_ST_P_* - which source said yes */
    uint32_t same_frame;   /* 1: the frame-local list also holds the surface (an earlier segment of this frame wrote it) */
    uint32_t cb_known;     /* 1: the draw's own CB0 was written in this translation (xlat12_tex_state bit 0, and bit 1 or no ext) */
    uint64_t cb_page;      /* its base page (0 = did not resolve) */
    uint32_t sys;          /* build 0.0.545: the texture's page is system memory (the key's bit 63) */
    uint32_t cb_sys;       /* build 0.0.545: the CB0 page is system memory */
    uint32_t is_p;         /* build 0.0.545: the asking frame is P-shaped (n48_p73_is_p_shape) */
    uint32_t fcb_ok;       /* build 0.0.545: fcb is this frame's COMPLETE colour-target key set (n48_st_fset.ok) */
    uint32_t fcb_n;
    const uint64_t *fcb;   /* build 0.0.545: the frame's colour-target KEYS (n48_st_key) */
} n48_st_q;
/* The proof's frame (see the file comment). */
static inline uint64_t n48_st_proof_seq(const n48_st_row *r, uint32_t proof, uint64_t frame)
{
    if (proof == N48_ST_P_FRAMELOCAL) return frame;
    if (proof == N48_ST_P_LEDGER) return r ? r->exec_seq : 0ull;
    return 0ull;   /* resprov: no frame number - the refusal direction */
}
static inline uint32_t n48_st_classify(const n48_st_tab *t, const n48_st_q *q, const n48_st_row **out)
{
    if (out) *out = 0;
    if (!q || !q->page) return N48_ST_V_NOPAGE;
    const uint64_t key = n48_st_key(q->page, q->sys);
    const n48_st_row *r = n48_st_find_c(t, q->arm, key);
    if (!r) return N48_ST_V_NOROW;
    if (out) *out = r;
    if (r->verdict == N48_ST_EXEC) return N48_ST_V_FRESH;
    if (q->proof != N48_ST_P_FRAMELOCAL && !(r->seq > n48_st_proof_seq(r, q->proof, q->frame))) return N48_ST_V_OLDER;
    if (q->cb_known && q->cb_page && n48_st_key(q->cb_page, q->cb_sys) == key) return N48_ST_V_EXEMPT_SELF;
    if (q->proof == N48_ST_P_FRAMELOCAL || q->same_frame) return N48_ST_V_EXEMPT_SAME;
    /* build 0.0.545 (2): any colour target of THIS frame - only from a set proven complete */
    if (q->fcb_ok && q->fcb)
        for (uint32_t k = 0; k < q->fcb_n; k++) if (q->fcb[k] == key) return N48_ST_V_EXEMPT_FCB;
    /* build 0.0.545 (3): a P frame refuses only a page this arm never committed. build 0.0.546: a LEDGER
     * proof is itself "committed this arm" (a committed producer of this (ctx, VA, mode) this arm), so it counts as once-committed
     * even when the table's row lost its exec_seq (an evicted row re-marked by a refused writer). */
    if (q->is_p && (r->exec_seq != 0ull || q->proof == N48_ST_P_LEDGER)) return N48_ST_V_P_ONCE;
    return N48_ST_V_STALE;
}
/* THE ONLY PLACE A VERDICT CHANGES: 1 = the source's yes stands, 0 = it becomes a no. Only ON, only STALE. */
static inline uint32_t n48_st_keep(uint32_t mode, uint32_t v)
{
    return (mode == N48_ST_M_ON && v == N48_ST_V_STALE) ? 0u : 1u;
}
typedef struct {
    uint64_t asks, v[N48_ST_VERDICTS], byProof[N48_ST_PROOFS], refused, shadowed, cbUnknown;
    uint32_t lines, exLines;
    uint64_t unarmed, pgReused;   /* build 0.0.545: asks with no arm (no walk paid); asks that reused the original's walk */
} n48_st_ctr;
#define N48_ST_LINES    64u    /* STALE lines per arm */
#define N48_ST_EX_LINES 16u    /* exemption lines per arm */
static inline void n48_st_count(n48_st_ctr *c, uint32_t mode, uint32_t v, uint32_t proof, uint32_t cb_known)
{
    if (!c) return;
    c->asks++;
    if (v < N48_ST_VERDICTS) c->v[v]++;
    if (v == N48_ST_V_STALE) {
        if (proof < N48_ST_PROOFS) c->byProof[proof]++;
        if (!cb_known) c->cbUnknown++;
        if (mode == N48_ST_M_ON) c->refused++; else c->shadowed++;
    }
}
/* Whether this verdict's line may be printed (and takes it). */
static inline uint32_t n48_st_line_take(n48_st_ctr *c, uint32_t v)
{
    if (!c) return 0u;
    if (v == N48_ST_V_STALE) { if (c->lines < N48_ST_LINES) { c->lines++; return 1u; } return 0u; }
    if (v == N48_ST_V_EXEMPT_SELF || v == N48_ST_V_EXEMPT_SAME || v == N48_ST_V_EXEMPT_FCB || v == N48_ST_V_P_ONCE) {
        if (c->exLines < N48_ST_EX_LINES) { c->exLines++; return 1u; }
    }
    return 0u;
}

/* ---- build 0.0.545 (2): THE FRAME'S COLOUR-TARGET SET, filled before the policy pass asks ---- */
#define N48_ST_FCB_MAX 40u   /* the 8 IB-end rows + the per-draw pass's 32 */
enum { N48_ST_FS_UNSEEN = 0x1u,   /* the per-draw pass could not see a target (n48_gcap_cbt.unseen) */
       N48_ST_FS_CBOVER = 0x2u,   /* more per-draw targets than N48_GCAP_CBT_MAX */
       N48_ST_FS_SHORT  = 0x4u,   /* a per-draw walk stopped before its IB's end */
       N48_ST_FS_UNRES  = 0x8u,   /* a target's page did not resolve */
       N48_ST_FS_OVER   = 0x10u,  /* more distinct targets than N48_ST_FCB_MAX */
       N48_ST_FS_NONE   = 0x20u,  /* the set was never filled for this frame */
       N48_ST_FS_NOREAD = 0x40u,  /* build 0.0.546: the frame's reader was not proven (!f.reader_ok): no IB was scanned */
       N48_ST_FS_IBSKIP = 0x80u };/* build 0.0.546: an IB the loop skipped (short read or walk) or never read (past the cap) */
typedef struct {
    uint64_t frame;
    uint32_t ok, why, n, is_p, stale, simStale, unres, over;
    uint64_t va[N48_ST_FCB_MAX], key[N48_ST_FCB_MAX];
} n48_st_fset;
/* The frame top: nothing is proven for `frame` yet (why NONE, ok 0); the per-frame STALE flags start clear. */
static inline void n48_st_fset_begin(n48_st_fset *s, uint64_t frame, uint32_t is_p)
{
    if (!s) return;
    s->frame = frame; s->ok = 0u; s->why = N48_ST_FS_NONE; s->n = 0u; s->is_p = is_p ? 1u : 0u;
    s->stale = 0u; s->simStale = 0u; s->unres = 0u; s->over = 0u;
}
/* One target (key 0 = did not resolve: never stored, the set is incomplete). Duplicates are stored once. */
static inline void n48_st_fset_add(n48_st_fset *s, uint64_t va, uint64_t key)
{
    if (!s) return;
    if (!key) { s->unres++; s->why |= N48_ST_FS_UNRES; return; }
    for (uint32_t k = 0; k < s->n; k++) if (s->key[k] == key) return;
    if (s->n >= N48_ST_FCB_MAX) { s->over++; s->why |= N48_ST_FS_OVER; return; }
    s->va[s->n] = va; s->key[s->n] = key; s->n++;
}
/* build 0.0.546: before the close, what the IB loop could not scan: no reader, or an IB it skipped. Only ever
 * adds a reason (toward no exemption). */
static inline void n48_st_fset_blind(n48_st_fset *s, uint32_t reader_ok, uint32_t ib_skip)
{
    if (!s) return;
    if (!reader_ok) s->why |= N48_ST_FS_NOREAD;
    if (ib_skip) s->why |= N48_ST_FS_IBSKIP;
}
/* After every target was added: the per-draw pass's own completeness. ok ONLY when nothing is missing (fail toward no exemption). */
static inline void n48_st_fset_close(n48_st_fset *s, uint32_t cbt_unseen, uint32_t cbt_n, uint32_t cbt_max, uint32_t walk_short)
{
    if (!s) return;
    s->why &= ~(uint32_t)N48_ST_FS_NONE;
    if (cbt_unseen) s->why |= N48_ST_FS_UNSEEN;
    if (cbt_n > cbt_max) s->why |= N48_ST_FS_CBOVER;
    if (walk_short) s->why |= N48_ST_FS_SHORT;
    s->ok = s->why == 0u ? 1u : 0u;
}
/* The feed's `truncated`: only REAL overflow - targets past the IB-end rows that the per-draw pass did NOT cover (its own set
 * incomplete: unseen, short walk or over its cap), per-draw targets past its cap, and targets past N48_ST_FCB_MAX. */
static inline uint64_t n48_st_trunc(const n48_st_fset *s, uint32_t tgt_n, uint32_t h_held, uint32_t cbt_n, uint32_t cbt_max)
{
    if (!s) return 0ull;
    uint64_t tr = s->over;
    if ((s->why & (N48_ST_FS_UNSEEN | N48_ST_FS_SHORT | N48_ST_FS_CBOVER | N48_ST_FS_IBSKIP)) && tgt_n > h_held) tr += tgt_n - h_held;
    if (cbt_n > cbt_max) tr += cbt_n - cbt_max;
    return tr;
}
/* What ON would have recorded for this frame's targets: committed AND no STALE answer (against the simulated table) this frame. */
static inline uint32_t n48_st_sim_exec(uint32_t commit_ok, uint32_t sim_stale) { return (commit_ok && !sim_stale) ? 1u : 0u; }
typedef struct {
    uint64_t frames, wouldP, wouldOther, wouldCommP, wouldCommOther, pActual, pSim;
    uint64_t fsOk, fsNo, fsUnseen, fsCbOver, fsShort, fsUnres, fsOver, cbtUnseen;
    uint64_t fsNoRead, fsIbSkip;   /* build 0.0.546 */
} n48_st_fr_ctr;
/* One fed frame: `sim_stale` is a STALE answer ON would have given (the simulated table in SHADOW, the real one in ON). */
static inline void n48_st_fr_count(n48_st_fr_ctr *c, const n48_st_fset *s, uint32_t commit_ok, uint32_t cbt_unseen)
{
    if (!c || !s) return;
    c->frames++;
    if (s->simStale) {
        if (s->is_p) { c->wouldP++; if (commit_ok) c->wouldCommP++; }
        else { c->wouldOther++; if (commit_ok) c->wouldCommOther++; }
    }
    if (s->is_p && commit_ok) { c->pActual++; if (n48_st_sim_exec(commit_ok, s->simStale)) c->pSim++; }
    if (s->ok) c->fsOk++; else c->fsNo++;
    if (s->why & N48_ST_FS_UNSEEN) c->fsUnseen++;
    if (s->why & N48_ST_FS_CBOVER) c->fsCbOver++;
    if (s->why & N48_ST_FS_SHORT) c->fsShort++;
    if (s->why & N48_ST_FS_UNRES) c->fsUnres++;
    if (s->why & N48_ST_FS_OVER) c->fsOver++;
    if (s->why & N48_ST_FS_NOREAD) c->fsNoRead++;
    if (s->why & N48_ST_FS_IBSKIP) c->fsIbSkip++;
    c->cbtUnseen += cbt_unseen;
}

/* ---- instrument 1: the committed-draw boxes (an uncapped-window tex531 mode, any program) ---- */
typedef struct { uint32_t x0, y0, x1, y1; } n48_st_box;
#define N48_ST_BOX_A_DEFAULT { 800u, 140u, 1120u, 290u }    /* the lock-screen clock at 1080p */
#define N48_ST_BOX_B_DEFAULT { 860u, 950u, 1060u, 1010u }   /* the password capsule at 1080p */
/* A window scissor (gfx10 TL/BR: X [14:0], Y [30:16]; BR exclusive) meeting a box (half-open): 1. */
static inline uint32_t n48_st_box_meets(const n48_st_box *b, uint32_t tl, uint32_t br)
{
    if (!b) return 0u;
    const uint32_t sx0 = tl & 0x7FFFu, sy0 = (tl >> 16) & 0x7FFFu, sx1 = br & 0x7FFFu, sy1 = (br >> 16) & 0x7FFFu;
    return (sx0 < b->x1 && b->x0 < sx1 && sy0 < b->y1 && b->y0 < sy1) ? 1u : 0u;
}
#define N48_ST_BOX_REC   32u     /* records stashed per judged frame */
#define N48_ST_BOX_LINES 1024u   /* box lines per arm (and per re-opened window) */
typedef struct {
    uint64_t ps;
    uint32_t at, tex, heap, seg, from, boxes, seen, cb0, cb0_ext, tl, br;
    uint32_t rec[8];
} n48_st_brec;
typedef struct {
    uint64_t frame;
    uint32_t n, lines;
    n48_st_brec r[N48_ST_BOX_REC];
    uint64_t seen, kept, dupes, over, noWin, frames, printed, suppressed, uncommitted;
} n48_st_boxlog;
/* One T# the translator read, with the draw's own state. Kept only when the window scissor is known and meets a box. */
static inline uint32_t n48_st_box_note(n48_st_boxlog *s, const n48_st_box *a, const n48_st_box *b, uint64_t frame, uint64_t ps,
                                       uint32_t at_i, uint32_t heap, const uint32_t rec[8], uint32_t seen, uint32_t cb0,
                                       uint32_t cb0_ext, uint32_t tl, uint32_t br, uint32_t seg, uint32_t from)
{
    if (!s || !rec) return 0u;
    s->seen++;
    if (s->frame != frame) { s->frame = frame; s->n = 0u; }
    if ((seen & 0xCu) != 0xCu) { s->noWin++; return 0u; }
    const uint32_t boxes = n48_st_box_meets(a, tl, br) | (n48_st_box_meets(b, tl, br) << 1);
    if (!boxes) return 0u;
    const uint32_t at = at_i & 0xFFFFFFu, tex = at_i >> 24;
    for (uint32_t k = 0; k < s->n; k++)
        if (s->r[k].seg == seg && s->r[k].at == at && s->r[k].tex == tex) { s->dupes++; return 0u; }
    if (s->n >= N48_ST_BOX_REC) { s->over++; return 0u; }
    n48_st_brec *e = &s->r[s->n++];
    e->ps = ps; e->at = at; e->tex = tex; e->heap = heap; e->seg = seg; e->from = from; e->boxes = boxes;
    e->seen = seen & 0xFu; e->cb0 = cb0; e->cb0_ext = cb0_ext; e->tl = tl; e->br = br;
    for (uint32_t k = 0; k < 8u; k++) e->rec[k] = rec[k];
    s->kept++;
    return 1u;
}
/* The frame's gate answered: how many kept records to print (committed frames only, capped per arm); the stash empties. */
static inline uint32_t n48_st_box_frame_end(n48_st_boxlog *s, uint64_t frame, uint32_t committed)
{
    if (!s || s->frame != frame || !s->n) return 0u;
    const uint32_t n = s->n;
    s->n = 0u;
    s->frames++;
    if (!committed) { s->uncommitted += n; return 0u; }
    const uint32_t room = s->lines < N48_ST_BOX_LINES ? N48_ST_BOX_LINES - s->lines : 0u;
    const uint32_t out = n < room ? n : room;
    s->lines += out; s->printed += out; s->suppressed += n - out;
    return out;
}
/* The draw packet the COMMITTED IB holds at the record's dword: 1 DRAWN, 2 NOPED (elided), 0 unknown. */
enum { N48_ST_D_UNKNOWN = 0u, N48_ST_D_DRAWN = 1u, N48_ST_D_NOPED = 2u };
static inline uint32_t n48_st_draw_class(uint32_t h)
{
    if (h == 0xFFFF1000u) return N48_ST_D_NOPED;
    if ((h >> 30) != 3u) return N48_ST_D_UNKNOWN;
    const uint32_t op = (h >> 8) & 0xFFu;
    if (op == 0x10u) return N48_ST_D_NOPED;
    if (op == 0x24u || op == 0x25u || op == 0x27u || op == 0x2Du || op == 0x35u) return N48_ST_D_DRAWN;
    return N48_ST_D_UNKNOWN;
}
static inline const char *n48_st_draw_name(uint32_t d)
{
    return d == N48_ST_D_DRAWN ? "DRAWN" : d == N48_ST_D_NOPED ? "NOPED" : "?";
}
static inline const char *n48_st_box_name(uint32_t boxes) { return boxes == 3u ? "AB" : boxes == 2u ? "B" : "A"; }

/* ---- instrument 2: the re-opened log window (START + S seconds, once per arm) ---- */
typedef struct { uint32_t secs, fired; uint32_t epoch; uint64_t fires; } n48_st_win;
/* 1 exactly once per arm: S set, the arm's START seen, S seconds after it, not yet fired. */
static inline uint32_t n48_st_win_due(n48_st_win *w, uint64_t start_us, uint64_t now_us)
{
    if (!w || !w->secs || w->fired || !start_us || now_us < start_us) return 0u;
    if (now_us - start_us < (uint64_t)w->secs * 1000000ull) return 0u;
    w->fired = 1u; w->epoch++; w->fires++;
    return 1u;
}
static inline void n48_st_win_arm(n48_st_win *w) { if (w) w->fired = 0u; }

/* ---- instrument 3: committed frames' IB-length signatures ---- */
#define N48_ST_SIG_LINES 1024u   /* per arm (and per re-opened window) */

/* ---- the lines (each bounded under n48log's 491-byte body cap by tests/gfx_stale103_test.cpp) ---- */
/* args: outcome (%s), frame, draw dword, identity (%#llx), its name (%s), texture VA, page, proof (%s), writer frame, last exec
 * frame, the writer's VA, the draw's CB0 VA, its note (%s), CB0 page, line, cap. */
#define N48_ST_LINE_FMT "stale103: %s F%llu dw %u id %#llx %s tex %#llx page %#llx proof %s; writer F%llu REFUSED (last exec F%llu, " \
    "VA %#llx); cb0 %#llx%s page %#llx; line %u/%u"
/* args: mode name, how, arm key, rows used, cap, marks exec, refused, unresolved, truncated, evicted exec, LOST refused, out of
 * order, resets, frames fed. */
#define N48_ST_REPORT_FMT "stale103: switch 103 %s%s; table arm %#llx rows %u/%u; marks exec %llu refused %llu unresolved %llu " \
    "truncated %llu; evicted exec %llu LOST-refused %llu out-of-order %llu resets %llu frames %llu"
/* args: asks, the seven verdict counts, STALE by proof (ledger, resprov, frame-local), ON refused, SHADOW counted, cb0 unknown,
 * lines, cap. */
#define N48_ST_REPORT2_FMT "stale103: asks %llu: no-page %llu no-row %llu fresh %llu older %llu exempt-self %llu exempt-same-frame " \
    "%llu STALE %llu (ledger %llu resprov %llu frame-local %llu); ON refused %llu SHADOW counted %llu; cb0 unknown %llu; lines %u/%u"
/* build 0.0.545. args: rows used, cap, marks exec, refused, unresolved, evicted exec, LOST, frames, the simulated table's
 * STALE / exempt-frame-cb / p-once answers. */
#define N48_ST_SIM_FMT "stale103: SIMULATED-ON table rows %u/%u marks exec %llu refused %llu unresolved %llu evicted exec %llu " \
    "LOST %llu frames %llu; answers STALE %llu exempt-frame-cb %llu p-once %llu"
/* args: ON would refuse frames P / other, of them committed P / other, P delivered actual / simulated-ON. */
#define N48_ST_WOULD_FMT "stale103: ON WOULD REFUSE frames P %llu other %llu (committed here P %llu other %llu); P frames delivered " \
    "actual %llu simulated-ON %llu"
/* args: the table's name, plane/layer NOT_EXEC -> EXEC count, longest us, > 1 s; still NOT_EXEC at STOP count, longest us, > 1 s. */
#define N48_ST_REC_FMT "stale103: %s plane/layer rows: back to EXEC %llu (longest %llu us, > 1 s %llu); STILL NOT_EXEC %llu " \
    "(longest %llu us, > 1 s %llu)"
/* args: frame target sets complete, incomplete (unseen, cb over, short walk, unresolved, over), cbt unseen total, exempt-frame-cb,
 * p-once, unarmed asks, reused walks, real-table truncated. */
#define N48_ST_FS_FMT "stale103: frame CB target sets complete %llu incomplete %llu (unseen %llu cb-over %llu short-walk %llu " \
    "unresolved %llu over %llu; cbt unseen targets %llu); real table exempt-frame-cb %llu p-once %llu; unarmed asks %llu; " \
    "walks reused %llu"
/* args: LOST in the simulated table, LOST in the real table, plane/layer rows evicted in the simulated table, in the real table.
 * Printed only when n48_st_void (build 0.0.546: evictRole voids like LOST). */
#define N48_ST_VOID_FMT "stale103: *** ON EVIDENCE VOID *** LOST simulated %llu real %llu, plane/layer rows evicted simulated %llu real " \
    "%llu: refused writers or role rows fell out of a full table (never fail closed on overflow, ); this arm cannot clear ON"
/* build 0.0.546. args: sets incomplete because no reader, because an IB was skipped; plane/layer rows evicted real, simulated. */
#define N48_ST_FS2_FMT "stale103: frame CB target sets incomplete for no reader %llu, an IB skipped %llu; plane/layer rows evicted " \
    "real %llu simulated %llu"
/* build 0.0.546: printed before st103_report2's lines when no arm is scoped (a mode change since the last
 * arm, or none yet): those lines then hold the PREVIOUS arm's counters (the tables' own counters are cumulative since boot). */
#define N48_ST_PREV_FMT "stale103: (previous arm) no arm scoped since the last mode change: the lines below are the PREVIOUS arm's " \
    "counters (they reset at the next arm's first frame; the tables' own counters are cumulative since boot)"
/* args: box A x0 y0 x1 y1, box B x0 y0 x1 y1, window seconds, fires, box seen, kept, no-window, over, printed, suppressed,
 * uncommitted, box lines, cap, sig lines, cap. */
#define N48_ST_REPORT3_FMT "stale103: boxes A %u,%u-%u,%u B %u,%u-%u,%u; window START+%u s (fired %llu); box T# seen %llu kept %llu " \
    "no-window %llu over %llu printed %llu suppressed %llu uncommitted %llu; lines box %u/%u sig %u/%u"
/* args: box name, frame, drawn (%s), seg, draw dword, identity name, identity, texture index, heap, base, W, H, fmt, sw, comp,
 * CB0 VA, its note, window x0 y0 x1 y1, the raw 8 dwords. */
#define N48_ST_BOX_FMT "stale103: box %s F%llu %s seg %u dw %u %s %#llx tex %u heap %u base %#llx %ux%u fmt %u sw %u comp %u " \
    "cb0 %#llx%s win %u,%u-%u,%u T# %08x %08x %08x %08x %08x %08x %08x %08x"
/* args: frame, the signature (%s), nib, line, cap. */
#define N48_ST_SIG_FMT "stale103: sig F%llu COMMITTED %s nib %u; line %u/%u"
/* args: seconds, frame, us after START, the reset caps' previous values (tex531 early, late, PROVENANCE, late540 gate early+late,
 * seg early+late), the window epoch. */
#define N48_ST_WIN_FMT "stale103: WINDOW RE-OPENED at START+%u s (frame %llu, %llu us after START): caps reset - tex531 %u+%u, " \
    "PROVENANCE %llu, late540 gate %u seg %u, dpled841, box, sig; epoch %u"

#endif /* N48_GFX_STALE103_H */
