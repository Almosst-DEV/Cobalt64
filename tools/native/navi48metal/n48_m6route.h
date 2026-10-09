/* n48_m6route.h: bundle build 11 (M6 Stage 1a, an internal design note "M6 study", section 3): WHICH DISPLAY a display surface belongs to, no Vulkan, no ObjC, no locking.
 *
 * Until build 10 the bundle called an IOSurface a DISPLAY surface when its size matched HUBP0's plane (n48s_classify). With three displays the monitor B's 2560x1440 surfaces match the DP's plane, so their
 * frames would be copied into a DP scanout slot and flipped onto the DP. With the kernel latch navi48-m6=1 the kernel learns, from the display transactions themselves, which pipe (= which display)
 * each IOSurface ID is presented on, and publishes the table as the registry property "Navi48,M6Surf" of the Metal nub (the channel "Navi48,Ready" already uses; the shipped Mesa exports no
 * passthrough and WindowServer's N48N connection is exclusive, so there is no selector the bundle can call). The latch itself arrives in scan_query.flags (N48N_SCANQ_M6).
 *
 * Property layout (little endian, mirrors src/navi48-bringup/src/amd/native_m6_pure.h; the host test pins the constants): u32 version (1), u32 n, then n x { u32 id, u32 inst, u32 flags }.
 *   inst: 0 = the DP display (the only instance this bundle presents), 1 = the monitor A, 2 = the monitor B.   flags bit 0: the ID was seen on TWO pipes (AMBIGUOUS: never presented).
 *
 * Two decisions, both pure:
 *   n48m6_gate(sid)  at ENCODE time (the frame is about to take a scanout slot and copy): a surface the table maps to another display, or flags ambiguous, takes NO slot (the frame is completed without a
 *                    present); a surface the table does not know yet is let through TENTATIVELY (the kernel learns an ID when WindowServer submits the transaction, which is after the encode of that frame).
 *   n48m6_final(sid) at COMPLETION time, just before the present: ONLY a surface the table maps to instance 0 (not ambiguous) is presented. An unknown one is refreshed from the kernel and retried a few
 *                    times by the caller; still unknown = no present (counted).
 * With the latch OFF both return PRESENT for everything: build 10's behaviour exactly.
 *
 * Build 12 (queue 290 S3): the completion decision moved BEFORE n48df_complete_held and no longer sleeps or reads the registry: n48m6_complete_plan (below) says PRESENT / SKIP / RETRY / DROP.
 */
#ifndef N48_M6ROUTE_H
#define N48_M6ROUTE_H
#include <stdint.h>
#include <string.h>

#define N48M6_BLOB_VERSION 1u
#define N48M6_BLOB_VERSION2 2u            /* bundle 13 / kernel 0.0.661 with navi48-m6flip: u32 version (2), u32 n, u32 GENERATION, then n entries (review R2) */
#define N48M6_BLOB_HDR     8u
#define N48M6_BLOB_HDR2    12u
#define N48M6_BLOB_ENT     12u
#define N48M6_MAX_SURF     16u
#define N48M6_ENT_AMBIGUOUS 1u
#define N48M6_INST_DP      0u
#define N48M6_INST_MONA    1u
#define N48M6_INST_MONB    2u
#define N48M6_NINST        3u
#define N48M6_PROP_LATCH   "Navi48,M6"
#define N48M6_PROP_SURF    "Navi48,M6Surf"
#define N48M6_SCANQ_FLAG   (1u << 5)      /* == N48N_SCANQ_M6 (n48_scanabi.h); the host test checks the equality */

enum { N48M6_PRESENT = 0, N48M6_TENTATIVE = 1, N48M6_SKIP_OTHER = 2, N48M6_SKIP_AMBIG = 3, N48M6_SKIP_UNKNOWN = 4 };

typedef struct { uint32_t id, inst, flags; } n48m6_ent_t;
typedef struct {
    int latch;                          /* the kernel reported N48N_SCANQ_M6: this boot routes by IOSurface ID */
    int have_table;                     /* a table was parsed at least once */
    uint32_t n;
    n48m6_ent_t e[N48M6_MAX_SURF];
    /* counters (written by the caller under its own lock) */
    uint64_t gate_skip[N48M6_NINST];    /* frames that took no slot at the gate: the surface is mapped to instance 1 / 2 (index 0 unused) */
    uint64_t gate_ambig, gate_tentative, gate_ok;
    uint64_t fin_skip[N48M6_NINST], fin_ambig, fin_unknown, fin_ok;
    uint64_t refreshes, refresh_fail;
    uint32_t gen;                       /* bundle 13 (R2): the generation inside the cached blob (version 2); 0 for a version-1 blob */
    int have_gen;                       /* a version-2 blob was stored */
    uint64_t store_refused;             /* reads refused because their blob generation was OLDER than the cached one (out-of-order refresh) */
} n48m6_t;

static inline uint32_t n48m6_rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* Replace the table from the property bytes. 0 = parsed; -1 = refused (the old table is KEPT): wrong version, a count above the table, or a length that does not hold exactly n entries.
 * Bundle 13 (R2): version 2 also carries the table's GENERATION (the PubGate request count the build covered). A version-2 blob whose generation is OLDER than the one already cached is refused with -2 (the old table is kept):
 * two refreshes (the 10 s synchronous one and the asynchronous one) can complete out of order, and the older read must never overwrite a newer one. */
static inline int n48m6_parse(n48m6_t *s, const uint8_t *b, size_t len) {
    if (!s || !b || len < N48M6_BLOB_HDR) return -1;
    const uint32_t ver = n48m6_rd32(b);
    if (ver != N48M6_BLOB_VERSION && ver != N48M6_BLOB_VERSION2) return -1;
    const uint32_t hdr = ver == N48M6_BLOB_VERSION2 ? N48M6_BLOB_HDR2 : N48M6_BLOB_HDR;
    if (len < hdr) return -1;
    const uint32_t n = n48m6_rd32(b + 4);
    if (n > N48M6_MAX_SURF || len != (size_t)hdr + (size_t)n * N48M6_BLOB_ENT) return -1;
    const uint32_t gen = ver == N48M6_BLOB_VERSION2 ? n48m6_rd32(b + 8) : 0u;
    n48m6_ent_t tmp[N48M6_MAX_SURF]; memset(tmp, 0, sizeof tmp);
    for (uint32_t i = 0; i < n; i++) {
        tmp[i].id = n48m6_rd32(b + hdr + i * N48M6_BLOB_ENT);
        tmp[i].inst = n48m6_rd32(b + hdr + i * N48M6_BLOB_ENT + 4);
        tmp[i].flags = n48m6_rd32(b + hdr + i * N48M6_BLOB_ENT + 8);
        if (tmp[i].id == 0 || tmp[i].inst >= N48M6_NINST) return -1;          /* a corrupt entry rejects the whole table */
    }
    if (ver == N48M6_BLOB_VERSION2 && s->have_gen && (int32_t)(gen - s->gen) < 0) { s->store_refused++; return -2; }
    memcpy(s->e, tmp, sizeof s->e); s->n = n; s->have_table = 1;
    if (ver == N48M6_BLOB_VERSION2) { s->gen = gen; s->have_gen = 1; }
    return 0;
}
/* The refresh paths parse into a scratch table OUTSIDE the lock and store under it: n48m6_store is that store (bundle 13, R2). 0 = stored; -2 = the scratch blob is older than the cached table (kept). A version-1 scratch always stores. */
static inline int n48m6_store(n48m6_t *dst, const n48m6_t *src) {
    if (src->have_gen && dst->have_gen && (int32_t)(src->gen - dst->gen) < 0) { dst->store_refused++; return -2; }
    memcpy(dst->e, src->e, sizeof dst->e); dst->n = src->n; dst->have_table = 1;
    if (src->have_gen) { dst->gen = src->gen; dst->have_gen = 1; }
    return 0;
}
/* Is the cached table older than what the kernel says it has PUBLISHED (Status2's generation, the last published one)? 0 = no information (a status of 0 / no flip latch) or current. */
static inline int n48m6_table_stale(const n48m6_t *s, uint32_t status_gen) { return status_gen != 0u && (!s->have_gen || (int32_t)(status_gen - s->gen) > 0); }
static inline const n48m6_ent_t *n48m6_find(const n48m6_t *s, uint32_t sid) {
    if (!sid) return 0;
    for (uint32_t i = 0; i < s->n && i < N48M6_MAX_SURF; i++) if (s->e[i].id == sid) return &s->e[i];
    return 0;
}
static inline uint32_t n48m6_count_inst(const n48m6_t *s, uint32_t inst) { uint32_t c = 0; for (uint32_t i = 0; i < s->n && i < N48M6_MAX_SURF; i++) if (s->e[i].inst == inst) c++; return c; }
static inline uint32_t n48m6_count_ambig(const n48m6_t *s) { uint32_t c = 0; for (uint32_t i = 0; i < s->n && i < N48M6_MAX_SURF; i++) if (s->e[i].flags & N48M6_ENT_AMBIGUOUS) c++; return c; }

/* Encode-time decision. Counts into s. */
static inline int n48m6_gate(n48m6_t *s, uint32_t sid) {
    if (!s->latch) return N48M6_PRESENT;                                      /* build 10 */
    const n48m6_ent_t *e = n48m6_find(s, sid);
    if (!e) { s->gate_tentative++; return N48M6_TENTATIVE; }
    if (e->flags & N48M6_ENT_AMBIGUOUS) { s->gate_ambig++; return N48M6_SKIP_AMBIG; }
    if (e->inst != N48M6_INST_DP) { s->gate_skip[e->inst]++; return N48M6_SKIP_OTHER; }
    s->gate_ok++;
    return N48M6_PRESENT;
}
/* Completion-time decision. UNKNOWN is returned WITHOUT counting (the caller refreshes the table and asks again; it calls n48m6_final_count once the answer is final). */
static inline int n48m6_final_peek(const n48m6_t *s, uint32_t sid) {
    if (!s->latch) return N48M6_PRESENT;
    const n48m6_ent_t *e = n48m6_find(s, sid);
    if (!e) return N48M6_SKIP_UNKNOWN;
    if (e->flags & N48M6_ENT_AMBIGUOUS) return N48M6_SKIP_AMBIG;
    if (e->inst != N48M6_INST_DP) return N48M6_SKIP_OTHER;
    return N48M6_PRESENT;
}
static inline int n48m6_final_count(n48m6_t *s, uint32_t sid, int v) {
    if (!s->latch) return v;
    if (v == N48M6_PRESENT) s->fin_ok++;
    else if (v == N48M6_SKIP_AMBIG) s->fin_ambig++;
    else if (v == N48M6_SKIP_UNKNOWN) s->fin_unknown++;
    else { const n48m6_ent_t *e = n48m6_find(s, sid); if (e && e->inst < N48M6_NINST) s->fin_skip[e->inst]++; }
    return v;
}
static inline int n48m6_final(n48m6_t *s, uint32_t sid) { return n48m6_final_count(s, sid, n48m6_final_peek(s, sid)); }
/* The present may go ahead only for this verdict. */
static inline int n48m6_may_present(int v) { return v == N48M6_PRESENT; }
/* bundle 12 (queue 290 S3): THE COMPLETION PLAN. Decided BEFORE n48df_complete_held (which releases the slot and advances last_seq), from the table as it is, WITHOUT sleeping and WITHOUT a registry read:
 *   PRESENT  the surface is mapped to instance 0: go on to complete_held and the present
 *   SKIP     the kernel maps it to another display or saw it on two pipes: release the slot, never present (n48df_skip_content)
 *   RETRY    the kernel has not seen the ID yet and fewer than N48M6_RETRIES tries were made: refresh the table ASYNCHRONOUSLY and complete the frame again after N48M6_RETRY_NS on the present queue (it blocks nothing:
 *            later frames complete meanwhile and, being newer, make this one stale - the existing rule - if they present first)
 *   DROP     still unknown after the last try: release the slot, never present (counted as unknown)
 * 0.0.659's bundle 11 slept 3 ms x 8 with the lock dropped INSIDE the completion, after the slot had already been released: ~24 ms per unknown frame on the serial present queue. */
enum { N48M6_C_PRESENT = 0, N48M6_C_SKIP = 1, N48M6_C_RETRY = 2, N48M6_C_DROP = 3 };
#define N48M6_RETRIES   8
#define N48M6_RETRY_NS  3000000ull
static inline int n48m6_complete_plan(const n48m6_t *s, uint32_t sid, int tries) {
    if (!s->latch) return N48M6_C_PRESENT;                                    /* build 10 */
    const int v = n48m6_final_peek(s, sid);
    if (v == N48M6_PRESENT) return N48M6_C_PRESENT;
    if (v == N48M6_SKIP_UNKNOWN) return tries < N48M6_RETRIES ? N48M6_C_RETRY : N48M6_C_DROP;
    return N48M6_C_SKIP;
}
/* The retry plan of the completion path (build 11's constants; build 12 waits with dispatch_after, never nanosleep): how many refresh attempts an UNKNOWN surface gets and how long each waits (the kernel learns the ID when WindowServer submits the transaction). */
#endif
