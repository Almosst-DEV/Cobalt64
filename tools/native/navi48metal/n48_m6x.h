/* n48_m6x.h: bundle build 13 (M6 Stage 1b, an internal design note "Q2: Stage 1b build contract" item 12 and the review items R2, R3, R6): THE MONB'S SCANOUT FROM THE BUNDLE.
 * BUNDLE 15 (M6 Stage 2, "Stage 2 build contract (monitor A, instance 1)" item 12): the same decisions PER INSTANCE. The state is an array over the kernel instance numbers [DP, monitor A, monitor B]; every constant that was the monitor B's is a field of an
 * n48x_desc_t (n48x_desc(inst)): the slot size, the plane geometry (and so the copy rectangle), the slot tag, the kill file. The plan / completion functions are THREE-way (DP, monitor A, monitor B) over an enable MASK (bit i = instance i is enabled and
 * not OFF). The monitor A's enable comes from ITS constants and the Acquire's returned geometry (n48x_acq_geom_ok), never from the DP's scan query. A surface the table maps to instance i whose size is not geom(i) takes no slot (inst_geom_mismatch).
 * No Vulkan, no ObjC, no locking: the DECISIONS the glue in Navi48Device.m calls, so test-m6x.c drives them on the host.
 *
 * The scanout state is an array over INSTANCES [DP, monitor B] (instance 0 = the DP display, 2 = the monitor B; the monitor A, instance 1, is Stage 2). Instance 2 is enabled ONLY when ALL hold:
 *   - the kernel reports both latches (scan_query.flags N48N_SCANQ_M6 and N48N_SCANQ_M6FLIP),
 *   - the Mesa exports radv_darwin_n48n_call and radv_darwin_n48n_bo_handle are present (a bundle on an older dylib keeps instance 2 OFF),
 *   - /private/tmp/n48m-noflip2 is absent (the kill switch, polled in the 1 s tick; present = instance 2 is released and stays OFF for the process),
 *   - (bundle 15) the geometry is the descriptor's, cross-checked by the Acquire; no longer judged on the DP's query.
 * It is acquired LAZILY, at the first frame the table maps to the monitor B; 3 slots are allocated in visible VRAM and registered (SCANX_REGISTER). If anything fails - the pool, an allocation, a register, the Acquire - instance 2
 * goes OFF, releases what it took, and the DP is untouched.
 *
 * ROUTING (the surface table, n48_m6route.h):
 *   encode time   n48x_plan_inst: a surface the table maps to the monitor B plans instance 2; one it maps to the DP plans instance 0; an UNKNOWN one may go only TENTATIVELY to instance 0, NEVER to instance 2; an ambiguous one, or the
 *                 monitor A's, takes no slot.
 *   completion    n48x_complete_plan: present ONLY if the final instance (from the table as it is NOW) equals the slot's instance; otherwise the frame is DROPPED and counted (inst_mismatch). A table older than the kernel's last
 *                 PUBLISHED generation (Status2) is refreshed first: while stale the frame is retried (bounded), then dropped - a stale cache can never send a DP frame to the monitor B.
 *   R3 (bundle half)  a tentative instance-0 frame whose completion says MONB is not lost: the surface is remembered and re-copied to an instance-2 slot at the next 1 s keep-alive tick (or the next monitor B present), counted.
 *   R6            a frame planned TENTATIVELY (unknown surface) is never a damage-mode partial frame and never becomes the chain head: it is a FULL copy.
 *
 * THE KEEP-ALIVE: the monitor B pipe idles when nothing changes (queue 292), so a static monitor B is a static plane; the kernel's 5 s idle watchdog counts a Status2 call as activity. The 1 s tick MUST call Status2 while instance 2 is
 * active (n48x_tick_plan says so) - without it a static monitor B would be reverted to the bars after 5 s.
 */
#ifndef N48_M6X_H
#define N48_M6X_H
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include "n48_m6route.h"
#include "n48_dispflip.h"   // N48DF_R_* (the OFF reasons)

#define N48X_INST_MONA     1u
#define N48X_INST_MONB     2u
#define N48X_NOFLIP1_FILE  "/private/tmp/n48m-noflip1"
#define N48X_NOFLIP2_FILE  "/private/tmp/n48m-noflip2"
#define N48X_SCANQ_M6FLIP  (1u << 6)       /* == N48N_SCANQ_M6FLIP (n48_scanabi.h); the host test checks the equality */
#define N48X_SCANQ_M6FLIP1 (1u << 7)       /* == N48N_SCANQ_M6FLIP1: all three kernel latches are on (bundle 15) */
/* THE DESCRIPTORS: one row per HDMI instance, ONE lookup. slot_bytes = pitch * h rounded up to 64 KiB (the monitor A's 8,323,072 B, the monitor B's 14,745,600 B). */
typedef struct { uint32_t inst, tag, w, h, pitch_bytes; uint64_t slot_bytes; const char *killfile, *name; } n48x_desc_t;
static const n48x_desc_t n48x_descs[3] = {
    { 0, 0, 0, 0, 0, 0, NULL, "DP" },
    { N48X_INST_MONA, 0x10u, 1920u, 1080u, 7680u,  8323072ull,  N48X_NOFLIP1_FILE, "monitor A" },
    { N48X_INST_MONB, 0x20u, 2560u, 1440u, 10240u, 14745600ull, N48X_NOFLIP2_FILE, "monitor B" } };
static inline const n48x_desc_t *n48x_desc(uint32_t inst) { return (inst == N48X_INST_MONA || inst == N48X_INST_MONB) ? &n48x_descs[inst] : NULL; }
static inline uint64_t n48x_frame_bytes(const n48x_desc_t *d) { return (uint64_t)d->pitch_bytes * d->h; }   /* the copy size and rectangle: the INSTANCE'S, never the DP's */
#define N48X_REFRESH_TRIES 8               /* a stale table is re-read at most this many times per frame (n48m6 retry cadence) */

/* ---- enable --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { uint32_t inst; int kernel_m6, kernel_m6flip, kernel_m6flip1, have_call, have_handle, killfile; } n48x_enable_t;
/* instance 2 needs the two Stage-1b latches; instance 1 ALSO the third. The plane geometry is the descriptor's (not the DP's scan query); the Acquire cross-checks it (n48x_acq_geom_ok). */
static inline int n48x_enabled(const n48x_enable_t *e) { return n48x_desc(e->inst) != NULL && e->kernel_m6 && e->kernel_m6flip && (e->inst != N48X_INST_MONA || e->kernel_m6flip1) && e->have_call && e->have_handle && !e->killfile; }
static inline int n48x_id_ok(uint32_t inst, uint32_t id) { const n48x_desc_t *d = n48x_desc(inst); return d && id >= d->tag && id < d->tag + 3u; }   /* a slot id tagged for THIS instance */
/* SCANX_ACQUIRE's geometry word (ABI 1.12): w | h << 16 | pitch_px << 32. nout = the number of output words the call returned. The monitor A REQUIRES the fifth word and the match; the monitor B accepts a four-word answer (a 0.0.661 kernel) unchecked. */
static inline int n48x_acq_geom_ok(const n48x_desc_t *d, uint32_t nout, uint64_t word) {
    if (!d) return 0;
    if (nout < 5u) return d->inst == N48X_INST_MONB;
    return (word & 0xFFFFu) == d->w && ((word >> 16) & 0xFFFFu) == d->h && (word >> 32) * 4u == d->pitch_bytes;
}
/* a display surface the classifier accepts for instance i must have exactly the instance's geometry (the copy rectangle and the slot are the descriptor's) */
static inline int n48x_surface_geom_ok(uint32_t inst, uint32_t w, uint32_t h, uint32_t bpr) { const n48x_desc_t *d = n48x_desc(inst); return d && w == d->w && h == d->h && bpr == d->pitch_bytes; }
/* which instances may a classified surface of size w x h x bpr belong to? bit 0 = the DP (its plane), bit i = an enabled HDMI instance with that geometry. xon_mask bit i = instance i enabled and not OFF. */
static inline uint32_t n48x_classify_mask(uint32_t dp_w, uint32_t dp_h, uint32_t dp_pitch, uint32_t xon_mask, uint32_t w, uint32_t h, uint32_t bpr) {
    uint32_t m = (w == dp_w && h == dp_h && bpr == dp_pitch) ? 1u : 0u;
    for (uint32_t i = 1; i <= 2; i++) if (((xon_mask >> i) & 1u) && n48x_surface_geom_ok(i, w, h, bpr)) m |= 1u << i;
    return m;
}

/* ---- the visible-pool budget (review S4) -------------------------------------------------------------------------------------------------------------------------------------------------------------------
 * The BAR0 aperture is 256 MiB with no ReBAR (queue 70). RADV's heap size says nothing about what is LEFT: the budget is the kernel's FREE visible-VRAM figure (the allocator's bytes_free, what QueryInfo reports as vram_vis_free; SCANX_ACQUIRE
 * returns it in out[3], read in the same call) against the monitor B's slots plus a margin for everything else that allocates in that pool (textures, upload rings, the monitor A). The verdict is taken right after the Acquire and BEFORE the first monitor B
 * slot is allocated; failing it releases instance 2 (the kernel puts A back) and keeps it OFF, the DP untouched. An allocation that still fails afterwards does the same. */
#define N48X_MIB (1024ull * 1024ull)
typedef struct { uint64_t free_bytes, slot_bytes; uint32_t slots; uint64_t margin_bytes; } n48x_pool_t;
enum { N48X_POOL_OK = 0, N48X_POOL_NO_FIGURE = 1, N48X_POOL_TOO_SMALL = 2 };
static inline uint64_t n48x_pool_need(const n48x_pool_t *p) { return (uint64_t)p->slots * p->slot_bytes + p->margin_bytes; }
static inline int n48x_pool_verdict(const n48x_pool_t *p) {
    if (p->free_bytes == 0 || p->slot_bytes == 0 || p->slots == 0) return N48X_POOL_NO_FIGURE;     /* an unreported figure (an older kernel) is a refusal, never a guess */
    return n48x_pool_need(p) <= p->free_bytes ? N48X_POOL_OK : N48X_POOL_TOO_SMALL;
}
#define N48X_MARGIN_BYTES (32ull * N48X_MIB)

/* ---- THE ACQUIRE SEQUENCE, as a driver over operations (the glue supplies real Vulkan / selector ops, test-m6x.c fakes with failure injection) ------------------------------------------------------------------------
 * SCANX_ACQUIRE -> pool verdict (from its out[3] free-VRAM figure) -> (alloc + register) x 3 -> activate. ANY failure: instance 2 goes OFF through ops->off (which RELEASES it if it was acquired - the kernel puts A back) and, once nothing was submitted, the
 * slots are freed. The driver has NO operation that touches the DP: allocation failure leaves instance 2 OFF with the DP untouched BY CONSTRUCTION (and test-m6x.c counts it). Returns 0 = ACTIVE, else the OFF reason (N48DF_R_*). */
typedef struct {
    void *ctx;
    int  (*pool_verdict)(void *);                              /* N48X_POOL_* */
    int  (*k_acquire)(void *);                                 /* SCANX_ACQUIRE: 0 or -errno */
    void (*mark_acquired)(void *);
    int  (*geom_verdict)(void *);                              /* bundle 15: the Acquire's returned geometry equals the descriptor's (n48x_acq_geom_ok): 0 = yes */
    int  (*alloc_slot)(void *, int i);                         /* create + allocate + bind slot i in visible VRAM: 0 or -1 */
    int  (*register_slot)(void *, int i);                      /* SCANX_REGISTER slot i and check the tagged id: 0 or -1 */
    void (*off)(void *, int reason, const char *why);          /* fail closed: releases instance 2 if it was acquired */
    void (*free_slots)(void *);
    void (*activate)(void *);
} n48x_ops_t;
static inline int n48x_acquire_run(const n48x_ops_t *o) {
    if (o->k_acquire(o->ctx) != 0) { o->off(o->ctx, N48DF_R_ERROR, "SCANX_ACQUIRE refused"); return N48DF_R_ERROR; }
    o->mark_acquired(o->ctx);
    if (o->geom_verdict(o->ctx) != 0) { o->off(o->ctx, N48DF_R_GEOM, "the kernel's plane geometry for this instance is not the descriptor's"); return N48DF_R_GEOM; }
    if (o->pool_verdict(o->ctx) != N48X_POOL_OK) { o->off(o->ctx, N48DF_R_ALLOC, "the free visible-VRAM figure does not cover this display's slots"); return N48DF_R_ALLOC; }
    for (int i = 0; i < N48DF_MAX_SLOTS; i++) {
        if (o->alloc_slot(o->ctx, i) != 0 || o->register_slot(o->ctx, i) != 0) { o->off(o->ctx, N48DF_R_ALLOC, "a slot could not be allocated or registered"); o->free_slots(o->ctx); return N48DF_R_ALLOC; }
    }
    o->activate(o->ctx);
    return 0;
}

/* ---- encode-time plan --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { N48X_PLAN_SKIP = -1, N48X_PLAN_DP = 0, N48X_PLAN_MONA = 1, N48X_PLAN_MONB = 2 };   /* = the kernel instance numbers */
static inline int n48x_on(uint32_t xon_mask, int inst) { return (int)((xon_mask >> (uint32_t)inst) & 1u); }
/* xon_mask: bit i = instance i is ENABLED and not OFF. *tentative = 1 when the surface was unknown (instance 0 only, tentatively). With the latch OFF: build 10 (everything is the DP's). An UNKNOWN surface never goes to an HDMI instance. */
static inline int n48x_plan_inst(const n48m6_t *s, uint32_t sid, uint32_t xon_mask, int *tentative) {
    if (tentative) *tentative = 0;
    if (!s->latch) return N48X_PLAN_DP;
    const n48m6_ent_t *e = n48m6_find(s, sid);
    if (!e) { if (tentative) *tentative = 1; return N48X_PLAN_DP; }                 /* unknown: ONLY ever tentatively to instance 0, never to an HDMI instance */
    if (e->flags & N48M6_ENT_AMBIGUOUS) return N48X_PLAN_SKIP;
    if (e->inst == N48M6_INST_DP) return N48X_PLAN_DP;
    if (e->inst == N48M6_INST_MONA && n48x_on(xon_mask, N48X_PLAN_MONA)) return N48X_PLAN_MONA;
    if (e->inst == N48M6_INST_MONB && n48x_on(xon_mask, N48X_PLAN_MONB)) return N48X_PLAN_MONB;
    return N48X_PLAN_SKIP;                                                          /* an HDMI display's surface while that instance is OFF: no slot */
}
/* R-S2 (bundle 17): the acquire is asked for FIVE output words first (ABI 1.12). A 0.0.661 kernel answers the monitor B only with four, and refuses the five-word shape with kIOReturnBadArgument, which the call wrapper
 * (acd_kr_to_errno) turns into -EINVAL. ONLY that refusal is retried with four words, and only for the monitor B (the monitor A is a 1.12 feature: it never falls back). Any other error (-EBUSY, -ENOMEM, -EACCES, -ETIME,
 * -ECANCELED, -ENODEV, -ENOSYS: a hang, a lost device, a busy or absent kernel) is final: a second acquire after it could run against a kernel in an unknown state. */
static inline int n48x_acquire_retry4(int inst, int rc) { return inst == (int)N48X_INST_MONB && rc == -EINVAL; }
/* THE SIZE GATE (bundle 15, R-B1 in bundle 17): a surface planned for ANY instance must have EXACTLY that instance's geometry, or it takes no slot (counted inst_geom_mismatch). The DP's plan is judged against the DP plane
 * (dp_w x dp_h, dp_pitch bytes per row) too: an UNKNOWN surface is planned tentatively to the DP, and an monitor A-sized one (1920x1080, 8.3 MB) must never be copied at the DP's size (14.7 MB): that would over-read the surface. */
static inline int n48x_plan_geom_ok(int plan_inst, uint32_t w, uint32_t h, uint32_t bpr, uint32_t dp_w, uint32_t dp_h, uint32_t dp_pitch) {
    if (plan_inst == N48X_PLAN_DP) return dp_w != 0u && w == dp_w && h == dp_h && bpr == dp_pitch;
    return n48x_surface_geom_ok((uint32_t)plan_inst, w, h, bpr);
}

/* ---- completion-time plan ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { N48X_C_PRESENT = 0, N48X_C_SKIP = 1, N48X_C_RETRY = 2, N48X_C_DROP = 3, N48X_C_MISMATCH = 4, N48X_C_RECOPY = 5 };
/* slot_inst = the instance the slot belongs to (0, 1 or 2); tries = refresh attempts so far; stale = the cached table is older than the kernel's last published generation of ANY active HDMI instance; xon_mask as above.
 * MISMATCH: the final instance is another valid instance (counted as inst_mismatch, the slot released, nothing presented). RECOPY: the special mismatch of a TENTATIVE instance-0 frame whose surface is an enabled HDMI display's - the same
 * drop, plus the surface is remembered for a re-copy into that instance's slot (R3). */
static inline int n48x_complete_plan(const n48m6_t *s, uint32_t sid, int slot_inst, int tries, int stale, uint32_t xon_mask) {
    if (!s->latch) return N48X_C_PRESENT;                                           /* build 10 */
    const n48m6_ent_t *e = n48m6_find(s, sid);
    if (!e || stale) return tries < N48X_REFRESH_TRIES ? N48X_C_RETRY : N48X_C_DROP;
    if (e->flags & N48M6_ENT_AMBIGUOUS) return N48X_C_SKIP;
    if ((int)e->inst == slot_inst) return N48X_C_PRESENT;
    if (slot_inst == N48X_PLAN_DP && (e->inst == N48M6_INST_MONA || e->inst == N48M6_INST_MONB) && n48x_on(xon_mask, (int)e->inst)) return N48X_C_RECOPY;
    if (e->inst == N48M6_INST_DP || e->inst == N48M6_INST_MONA || e->inst == N48M6_INST_MONB) return N48X_C_MISMATCH;
    return N48X_C_SKIP;
}
static inline int n48x_may_present(int plan) { return plan == N48X_C_PRESENT; }

/* ---- the 1 s tick ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- */
enum { N48X_T_STATUS = 1u, N48X_T_RELEASE = 2u, N48X_T_REFRESH = 4u, N48X_T_RECOPY = 8u };
typedef struct { int x_active, killfile, status_gen_known; uint32_t status_gen; int recopy_pending; } n48x_tick_t;
/* What the tick does for instance 2: while ACTIVE it ALWAYS calls Status2 (the keep-alive); the kill file releases it; a published table the cache lacks is refreshed; a pending re-copy runs. */
static inline uint32_t n48x_tick_plan(const n48x_tick_t *t, const n48m6_t *s) {
    uint32_t a = 0;
    if (!t->x_active) return 0;
    if (t->killfile) return N48X_T_RELEASE;
    a |= N48X_T_STATUS;
    if (s->latch && t->status_gen_known && n48m6_table_stale(s, t->status_gen)) a |= N48X_T_REFRESH;
    if (t->recopy_pending) a |= N48X_T_RECOPY;
    return a;
}

/* ---- the per-instance counters and the re-copy memory ------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct {
    uint64_t inst_geom_mismatch, inst_mismatch, recopy_noted, recopy_done, recopy_fail, stale_retry, stale_drop, killed, acquire_fail, alloc_fail, status_calls, keepalive_calls, tentative_frames;
    uint32_t recopy_sid; int recopy_pending; uint64_t recopy_seq;   /* bundle 20 (F3): the sequence of the NOTED (dropped) frame: the re-copy presents with it, never with a fresh newest one */
} n48x_stats_t;
static inline void n48x_recopy_note(n48x_stats_t *x, uint32_t sid, uint64_t seq) { x->recopy_sid = sid; x->recopy_seq = seq; x->recopy_pending = 1; x->recopy_noted++; }
/* the re-copy ran (ok) or could not (no slot / surface gone): either way it is done once (a failed one is counted, the next monitor B present repaints the whole surface anyway) */
static inline void n48x_recopy_done(n48x_stats_t *x, int ok) { x->recopy_pending = 0; if (ok) x->recopy_done++; else x->recopy_fail++; }

/* ---- R6: a tentative frame is never a partial (damage-mode) frame and never the chain head ------------------------------------------------------------------------------------------------------------ */
/* kind: the frame's copy kind from the damage rectangle (0 = FULL, anything else partial). Returns the kind the plan may use. */
static inline int n48x_kind_for(int tentative, int kind_full, int kind) { return tentative ? kind_full : kind; }
/* the chain head may be set only by a NON-tentative frame */
static inline int n48x_may_chain(int tentative) { return !tentative; }
#endif
