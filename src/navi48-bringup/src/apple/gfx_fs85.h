/* gfx_fs85.h — build 0.0.530 (notes/design/SRCFILL85.md items 1-8; ): SWITCH 85 "SOURCE FILLS",
 * PURE PARTS. Stop armed runs going void at start-up.
 *
 * WHY (the design's Facts 2-4, CONFIRMED on the four boots run11i/j/l/m). In both void runs (Z2 run11i, AB2 run11m) no
 * plane-shaped frame (`ws_D_GPUPass`, xlat12_shader_ids.h identity 39) was ever eligible to commit, because the plane's
 * SOURCE surfaces were never proven: their one-time start-up fills (: each surface is cleared exactly once) did not
 * commit. Z2: the 0x400800000 fill was withdrawn at the keystone and retired, and the 0x406800000 fill was then refused
 * RESERVED-FOR-PLANE. AB2: the twin fill targets 0x403800000 / 0x405800000, neither in kN48FsMemberVa, and the clean-but-not-
 * live 0x400800000 fill was never retired ("a clean one is a retry"), so the fill window held to its expiry.
 *
 * WHAT SWITCH 85 CHANGES (only while 85, 33 AND 35 are all ON - n48_fs85_active; otherwise every entry point below is the
 * 0.0.529 function it replaces, called verbatim):
 *   S1 (item 2) — fill member 1 matches ANY CB0 in kN48Fs85Twin (the measured second plane sources). Member 0 is unchanged.
 *      n48_fs85_member_of is the one predicate identify / step / commit / retire / retire_withdrawn use. The struct keeps its
 *      two members, so the `fillset:` line's format is unchanged; the VA member 1 actually committed as is kept here.
 *   S2 (item 3) — a RESERVE frame that did not go live retires its member whatever the dependency said (dep_failed = 1),
 *      and the GATE's reason is recorded in a new field; `last_retire_reason` keeps its dependency semantics.
 *   S3 (item 4) — one SOURCE FILL in the plane window: n48_fs85_plane_step answers RESERVE (not REFUSE) for an eligible
 *      single-segment ColorFill whose CB0 is member 0 or a twin and not committed this scope, when some member is retired,
 *      at least 2 shots are left (read BEFORE the rewrite and the spend, under gXdLock), and it is the scope's first such
 *      admission (cap 1). The plane window stays open after it: only a GPUPass commit closes it.
 *
 * THE INVARIANT (item 5). Nothing here reads or writes n48_cm_shot_budget_of, N48_CM_SHOT_BUDGET_MAX, N/T,
 * N48_CM_PREPLANE_BOUND_US or either 60-frame expiry. S3 admits only at left >= 2, so at least one slot is always left for a
 * plane after an S3 fill; the bound without a plane stays min(4, N) spends or 30 s.
 *
 * FAIL-OPEN ANALYSIS (item 6). S1 and S3 only lift a POLICY refusal (RESERVED-FOR-FILL / RESERVED-FOR-PLANE, the gate's last
 * two rungs): the frame still passes every safety rung and would have committed with 33/35 OFF. S2 only closes the fill window
 * earlier. Nothing here sets `live`, dep_ok or a keystone field.
 *
 * OFF IDENTITY (item 8). The kext calls the dispatchers below (n48_fs85_frame / _retire / _retire_withdrawn / _commit);
 * with n48_fs85_active 0 each calls the 0.0.529 n48_fs_* function exactly, and gfx_fillset.h is untouched. */
#ifndef N48_GFX_FS85_H
#define N48_GFX_FS85_H

#include <stdint.h>
#include "gfx_fillset.h"

/* S1: the measured second plane sources (design Fact 3). 0x403800000 is EXCLUDED on purpose: it is a plane target in
 * Z-shaped boots (f8/f10), so admitting its fill would spend a slot on a frame no plane reads. */
#define N48_FS85_TWINS 3u
static const uint64_t kN48Fs85Twin[N48_FS85_TWINS] = { 0x404800000ull, 0x405800000ull, 0x406800000ull };
/* build 0.0.544 item 4b: the twins by console size (gfx_fillset.h N48_FS_GEO_*). At 2560x1440 the one measured second
 * plane source is 0x404000000 (run11ap-r: identity 39 refused PROVENANCE on 0x400100000 and 0x404000000 only); the other two slots are
 * EMPTY (0, never matched) until a 1440p boot measures more. An unknown size has no twins (member 1 then matches nothing). */
static const uint64_t kN48Fs85Twin1440[N48_FS85_TWINS] = { 0x404000000ull, 0ull, 0ull };
static inline uint64_t n48_fs85_twin_of(uint32_t geo, uint32_t t)
{
    if (t >= N48_FS85_TWINS) return 0ull;
    return geo == N48_FS_GEO_1080 ? kN48Fs85Twin[t] : geo == N48_FS_GEO_1440 ? kN48Fs85Twin1440[t] : 0ull;
}

/* S3's floor: an S3 source fill is admitted only while at least this many shots are left, so one stays for the plane. */
#define N48_FS85_S3_LEFT_MIN 2u
/* S3's cap: source fills admitted in the plane window per arm scope. */
#define N48_FS85_S3_CAP 1u

typedef struct {
    uint32_t on;            /* `gfxneuter 85 | M << 8`: M 1 on, M 0xFF off. OFF AT BOOT. Inert unless 33 AND 35 are ON. */
    /* per arm scope (n48_fs85_open) */
    uint64_t twin_va;       /* S1: the CB0 member 1 committed as (0 = none yet) */
    uint32_t twin_seq;      /* ...and its token seq */
    uint32_t rnl;           /* S2: members retired because their RESERVE frame did not go live (any dependency) */
    uint32_t rnl_gate;      /* S2: the GATE's reason (gfx_commit.h N48_CM_*) at the last such retirement */
    uint32_t s3_n;          /* S3: plane-window source fills admitted (<= N48_FS85_S3_CAP) */
    uint64_t s3_va;         /* S3: the admitted fill's CB0 */
    uint32_t s3_seq;        /* S3: its token seq once the keystone proved it (0 until then) */
    uint32_t s3_committed;  /* S3: 1 once the admitted fill committed */
    uint32_t s3_pend;       /* S3: the gate spent a shot on the admitted fill; the keystone decides */
    uint64_t s3_pend_cb0;
    uint32_t ref_left;      /* S3 candidates refused: fewer than N48_FS85_S3_LEFT_MIN shots left */
    uint32_t ref_noretire;  /* S3 candidates refused: no member is retired */
    uint32_t ref_cap;       /* S3 candidates refused: the cap was already used */
} n48_fs85;

/* Is switch 85 in force? 85 AND 33 AND 35, all ON (the design's "inert unless 33 and 35 are both ON"). Pure. */
static inline uint32_t n48_fs85_active(const n48_fs *f, const n48_fs85 *x)
{
    return (f && x && x->on && f->on && f->plane_on) ? 1u : 0u;
}

/* A fresh arm scope (called beside n48_fs_open). Keeps `on`; clears every per-scope field. */
static inline void n48_fs85_open(n48_fs85 *x)
{
    if (!x) return;
    x->twin_va = 0ull; x->twin_seq = 0u; x->rnl = 0u; x->rnl_gate = 0u;
    x->s3_n = 0u; x->s3_va = 0ull; x->s3_seq = 0u; x->s3_committed = 0u; x->s3_pend = 0u; x->s3_pend_cb0 = 0ull;
    x->ref_left = 0u; x->ref_noretire = 0u; x->ref_cap = 0u;
}

/* S1: does `cb0` name member m? Member 0: its own VA, unchanged. Member 1: any twin. Pure. */
static inline uint32_t n48_fs85_member_of(const n48_fs *f, uint32_t m, uint64_t cb0)
{
    if (!f || m >= N48_FS_MEMBERS) return 0u;
    if (m == 0u) return (f->member_va[0] && f->member_va[0] == cb0) ? 1u : 0u;   /* 0.0.544 4b: an unseated member (0) never matches */
    /* build 0.0.548 (switch 106 LEARNED): member 1 also matches its own seated VA. TABLED this adds nothing: the seated VA is
     * kN48FsMemberVa[1] / kN48FsMemberVa1440[1], each already twin slot 0 of its size (or 0 for an unknown size, never matched);
     * an unlearned slot (N48_FS_UNLEARNED) never matches. */
    if (f->member_va[1] && f->member_va[1] != N48_FS_UNLEARNED && f->member_va[1] == cb0) return 1u;
    for (uint32_t t = 0u; t < N48_FS85_TWINS; t++) {   /* build 0.0.544 4b: by console size; an empty slot never matches */
        const uint64_t tw = n48_fs85_twin_of(f->geo, t);
        if (tw && tw == cb0) return 1u;
    }
    return 0u;
}

/* The open member `cb0` names, or N48_FS_MEMBERS for none (uncommitted, unretired, S1 match). */
static inline uint32_t n48_fs85_open_member(const n48_fs *f, uint64_t cb0)
{
    for (uint32_t m = 0u; m < f->members && m < N48_FS_MEMBERS; m++)
        if (!f->member_committed[m] && !f->member_retired[m] && n48_fs85_member_of(f, m, cb0)) return m;
    return N48_FS_MEMBERS;
}

/* S1 versions of n48_fs_identify_fill / n48_fs_step: the SAME bodies with n48_fs85_member_of for the member match. */
static inline uint32_t n48_fs85_identify_fill(const n48_fs *f, uint32_t nseg, uint32_t ps_fill, uint64_t cb0)
{
    if (!f || !f->on || !f->opened) return 0u;
    if (f->expired || f->committed_n + f->retired_n >= f->members) return 0u;
    if (nseg != 1u || !ps_fill) return 0u;
    return n48_fs85_open_member(f, cb0) < N48_FS_MEMBERS ? 1u : 0u;
}

static inline uint32_t n48_fs85_step(n48_fs *f, uint32_t eligible, uint32_t is_fill, uint64_t cb0)
{
    if (!f || !f->on || !f->opened) return N48_FS_PASS;
    if (f->expired || f->committed_n + f->retired_n >= f->members) return N48_FS_PASS;
    if (f->judged + 1u > N48_FS_EXPIRE_JUDGED) {
        f->expired = 1u;
        f->expiry++;
        return N48_FS_PASS;
    }
    f->judged++;
    if (!eligible) return N48_FS_PASS;
    if (is_fill && n48_fs85_open_member(f, cb0) < N48_FS_MEMBERS) return N48_FS_RESERVE;
    f->refused++;
    return N48_FS_REFUSE;
}

/* S3: has `cb0` already committed this scope (member 0 at its VA, member 1 at the twin it committed as, or the S3 fill)? */
static inline uint32_t n48_fs85_committed_va(const n48_fs *f, const n48_fs85 *x, uint64_t cb0)
{
    if (f->member_committed[0] && f->member_va[0] == cb0) return 1u;
    if (f->member_committed[1] && x->twin_va == cb0) return 1u;
    if (x->s3_committed && x->s3_va == cb0) return 1u;
    return 0u;
}

/* S3: is `cb0` a SOURCE (member 0's VA or a twin)? */
static inline uint32_t n48_fs85_is_source(const n48_fs *f, uint64_t cb0)
{
    return (n48_fs85_member_of(f, 0u, cb0) || n48_fs85_member_of(f, 1u, cb0)) ? 1u : 0u;
}

/* S3: the plane window's step. The SAME body as n48_fs_plane_step, plus ONE admission: an eligible non-plane frame that is a
 * single-segment ColorFill (`raw_fill`: nsegPre == 1 AND the ColorFill PS - NOT n48_fs_identify_fill, which reads 0 once the
 * fill window is closed) of a source not committed this scope answers RESERVE when a member is retired, `left` (the shots
 * left, n48_cm_shot_left read BEFORE the rewrite and the spend) is at least N48_FS85_S3_LEFT_MIN, and the cap is unused. The
 * admission is counted at once (cap 1 per scope, whether or not the frame then commits) and flagged in *s3 so the caller
 * records it as a SOURCE fill, never as the plane commit that closes this window. A candidate refused by one of the three
 * conditions is counted under its first failing condition and REFUSED exactly as today. */
static inline uint32_t n48_fs85_plane_step(n48_fs *f, n48_fs85 *x, uint32_t eligible, uint32_t is_plane, uint32_t raw_fill,
                                           uint64_t cb0, uint32_t left, uint32_t *s3)
{
    if (s3) *s3 = 0u;
    if (!n48_fs_plane_win_open(f)) return N48_FS_PASS;
    if ((uint32_t)f->plane_judged + 1u > N48_FS_PLANE_EXPIRE_JUDGED) {
        f->plane_expired = 1u;
        f->plane_expiry++;
        return N48_FS_PASS;
    }
    f->plane_judged++;
    if (!eligible) return N48_FS_PASS;
    if (is_plane) return N48_FS_RESERVE;
    if (x && raw_fill && n48_fs85_is_source(f, cb0) && !n48_fs85_committed_va(f, x, cb0)) {
        if (f->retired_n == 0u) x->ref_noretire++;
        else if (x->s3_n >= N48_FS85_S3_CAP) x->ref_cap++;
        else if (left < N48_FS85_S3_LEFT_MIN) x->ref_left++;
        else {
            x->s3_n++;
            x->s3_va = cb0;
            if (s3) *s3 = 1u;
            return N48_FS_RESERVE;
        }
    }
    f->plane_refused++;
    return N48_FS_REFUSE;
}

/* THE FRAME'S ORDER, in one place so the host suite drives the kext's own sequence: identify, the FILL step, THEN the plane
 * step (the plane window opens only once the fill window closes, so exactly one of the two counters advances per frame).
 * OFF (on 0): the three 0.0.529 calls, verbatim - n48_fs_identify_fill, n48_fs_step (when fs_active), n48_fs_plane_step
 * (when fp_active). */
typedef struct { uint32_t is_fill, fs_step, fp_step, s3; } n48_fs85_frame_out;
static inline void n48_fs85_frame(n48_fs *f, n48_fs85 *x, uint32_t on, uint32_t fs_active, uint32_t fp_active,
                                  uint32_t eligible, uint32_t nseg, uint32_t ps_fill, uint32_t ps_plane, uint64_t cb0,
                                  uint32_t left, n48_fs85_frame_out *o)
{
    if (!o) return;
    o->s3 = 0u;
    if (!on) {
        o->is_fill = n48_fs_identify_fill(f, nseg, ps_fill, cb0);
        o->fs_step = fs_active ? n48_fs_step(f, eligible, o->is_fill, cb0) : (uint32_t)N48_FS_PASS;
        o->fp_step = fp_active ? n48_fs_plane_step(f, eligible, ps_plane) : (uint32_t)N48_FS_PASS;
        return;
    }
    o->is_fill = n48_fs85_identify_fill(f, nseg, ps_fill, cb0);
    o->fs_step = fs_active ? n48_fs85_step(f, eligible, o->is_fill, cb0) : (uint32_t)N48_FS_PASS;
    o->fp_step = fp_active ? n48_fs85_plane_step(f, x, eligible, ps_plane, (nseg == 1u && ps_fill) ? 1u : 0u, cb0, left,
                                                 &o->s3)
                           : (uint32_t)N48_FS_PASS;
}

/* S1 commit: the SAME body as n48_fs_commit with the S1 match; member 1 records the twin it committed as. OFF: n48_fs_commit. */
static inline uint32_t n48_fs85_commit(n48_fs *f, n48_fs85 *x, uint64_t cb0, uint64_t seq)
{
    if (!n48_fs85_active(f, x)) return n48_fs_commit(f, cb0, seq);
    const uint32_t m = n48_fs85_open_member(f, cb0);
    if (m >= N48_FS_MEMBERS) return 0u;
    f->member_committed[m] = 1u;
    f->member_seq[m] = seq;
    f->committed_n++;
    if (m == 1u) { x->twin_va = cb0; x->twin_seq = (uint32_t)seq; }
    return 1u;
}

/* S2: a RESERVE frame that did NOT go live. OFF: n48_fs_retire, verbatim. ON: retire the S1 member whatever the dependency
 * said (dep_failed = 1), record the dependency reason in `last_retire_reason` (its 0.0.408 meaning) and the GATE's reason
 * in `rnl_gate`. A LIVE frame never retires (the caller's own condition, re-checked here). */
static inline uint32_t n48_fs85_retire(n48_fs *f, n48_fs85 *x, uint64_t cb0, uint32_t live, uint32_t dep_failed,
                                       uint32_t dep_reason, uint32_t gate_reason)
{
    if (!n48_fs85_active(f, x)) return n48_fs_retire(f, cb0, dep_failed, dep_reason);
    if (live) return 0u;
    const uint32_t m = n48_fs85_open_member(f, cb0);
    if (m >= N48_FS_MEMBERS) return 0u;
    f->member_retired[m] = 1u;
    f->retired_n++;
    f->retired++;
    f->last_retire_reason = dep_reason;
    x->rnl++;
    x->rnl_gate = gate_reason;
    return 1u;
}

/* S1 retire at a keystone withdrawal: n48_fs_retire_withdrawn's body with the S1 match. OFF: n48_fs_retire_withdrawn. */
static inline uint32_t n48_fs85_retire_withdrawn(n48_fs *f, n48_fs85 *x, uint64_t cb0, uint32_t reason)
{
    if (!n48_fs85_active(f, x)) return n48_fs_retire_withdrawn(f, cb0, reason);
    const uint32_t m = n48_fs85_open_member(f, cb0);
    if (m >= N48_FS_MEMBERS) return 0u;
    f->member_retired[m] = 1u;
    f->retired_n++;
    f->retired++;
    f->last_retire_reason = reason;
    return 1u;
}

/* S3's commit, at the keystone's TRANSLATED point (the same instant n48_fs_commit / n48_fs_plane_commit run). Consumes the
 * pending record; answers 1 when the admitted fill is newly committed. A withdrawal just clears `s3_pend` (the caller). */
static inline uint32_t n48_fs85_s3_commit(n48_fs85 *x, uint64_t seq)
{
    if (!x || !x->s3_pend) return 0u;
    x->s3_pend = 0u;
    if (x->s3_committed || x->s3_n == 0u || x->s3_pend_cb0 != x->s3_va) return 0u;
    x->s3_committed = 1u;
    x->s3_seq = (uint32_t)seq;
    return 1u;
}

/* THE ONE REPORT LINE (item 7), printed by `gfxneuter 85` only; its worst case is measured by gfx_fs85_test (<= 491 bytes).
 * Args: state, change word, twin VA, twin seq, retired-not-live, the last S2 gate reason ("none" before one), S3 admitted,
 * S3 VA, S3 seq, refused left<2, refused no-retire, refused cap. */
#define N48_FS85_FMT \
    "srcfill85: `gfxneuter 85 | M << 8` is %s - %s. twin %#llx seq %u; retired-not-live %u (gate %s); " \
    "plane-window source fills %u (%#llx seq %u); refused: left<2 %u, no-retire %u, cap %u."
#define N48_FS85_BODY_CAP 491u

static inline const char *n48_fs85_state(const n48_fs *f, const n48_fs85 *x)
{
    if (!x || !x->on) return "OFF (default)";
    return n48_fs85_active(f, x) ? "ON" : "ON (INERT: 33 and 35 must both be ON)";
}

#endif /* N48_GFX_FS85_H */
