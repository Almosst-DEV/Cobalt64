// gfx_cg84.h — build 0.0.529 (notes/design/CG84.md items 1-12): SWITCH 84, THE GRANULE COPY GUARD AND THE
// DELTA RESIDENCY WRITE. THE PURE HALF (the reader half - granule masks, n48_cg_rec_hit, n48_cg_check_fx - is gfx_copyguard.h's).
// Here: the selector, the counted-verdict rule and its counters, and the writer's keys: eligibility, the G0 plan, the post-close
// update, the invalidations, the diff, the delta scope and the delta write loop. No register, no page table, nothing of Apple's;
// plain C that also compiles as C++, __atomic builtins only, no libc beyond stdint (the gfx_copyguard.h convention). Compiled into
// the kext (Navi48Bringup.cpp owns the storage and the leaf lock gD84Lock; Navi48AccelPeer.cpp calls it from the copier) and into
// tests/gfx_cg84_test.cpp.
//
// THE SELECTOR: `gfxneuter 84 | M << 8`: M1 SHADOW (340), M2 OFF (596, the default and the boot value), M3 ON (852). Bare 84 reads;
// any other M is refused unchanged (st 11). A continuous arm refuses a change (st 5).
//   OFF     0.0.528: the page check, full copies, no key, no scratch read, no instrument.
//   SHADOW  the counted check is the page check and every copy is 0.0.528's (MM or fast copy, from the backing); keys are kept in
//           "sampled trust" (the plan-time source image, whatever the copy's verify was), every residency copy's slot and BEGIN/END
//           carry the WOULD-BE delta as dlo/dhi, and an UNCOUNTED fine check (granules, dlo/dhi) runs after the counted one.
//   ON      the counted check is the fine check; an eligible copy with a valid key writes only [ds, de) (a delta, MM loop, from the
//           plan-time image, every dword read back) under a scope of exactly that range; a key is established only by a full copy
//           through the MM path from the same image, fully read back.
//
// THE KEY INVARIANT (a delta rests on it): VRAM [vram, vram + bytes) == the key's shadow, from the post-close update that made the
// key VALID (inval0 = the inval counter read then) until any of: an overlapping navi48_cg_open (the counter moves, bumped AFTER the
// opener's slot is published,; the key's own pending copier is skipped), a page-out of the resource, an unmapVA overlapping
// its VA, a context release, a WindowServer drop, a committed frame whose CB/DB target base lies in the key's VA range (or whose
// targets were not all held), poison over the range, or a state change (the switch-82 snapshot: 11/46/59/62/63, kernsub, the
// shadercache arm, the arm level, the ring neuter, WindowServer's binding). THE READ ORDERS (gfx_sk82.h D1, re-proved by
// tests/gfx_cg84_test.cpp's schedule walk): the G0 plan reads the counter THEN scans the slots; the post-close update scans the slots
// THEN reads the counter. DEVIATION D1 (from CG84.md item 11's ordering list, reported): the contract lists "the inval snapshot is
// taken before the copier's own open" as a planted break; in this protocol the snapshot IS taken at G0, before the own open, and it
// is the INVERSE (the snapshot after the G0 slot scan, i.e. after the own open) that admits a foreign write (the test's schedule
// walk finds it); the own open never moves the counter (the copier-thread marker), so the two differ only in that race.
#ifndef N48_GFX_CG84_H
#define N48_GFX_CG84_H

#include <stdint.h>
#include "gfx_copyguard.h"   // n48_cg_slot / n48_cg_poison / n48_cg_overlap / N48_CG_OK
#include "gfx_sk82.h"        // n48_sk82_snap / n48_sk82_snap_eq (the state snapshot, reused)

// ---- the switch -------------------------------------------------------------------------------------------------------------
#define N48_CG84_SWITCH  84u
#define N48_CG84_SHADOW  1u      // M1 (340)
#define N48_CG84_OFF     2u      // M2 (596): OFF, the default and the boot value
#define N48_CG84_ON      3u      // M3 (852)
#define N48_CG84_M_READ  0x100u  // bare 84
#define N48_CG84_M_BAD   0x101u  // any other M: refused, unchanged (st 11)
static inline uint32_t n48_cg84_mode_of_m(uint32_t m) {
    return m == 0u ? N48_CG84_M_READ : m == 1u ? N48_CG84_SHADOW : m == 2u ? N48_CG84_OFF : m == 3u ? N48_CG84_ON : N48_CG84_M_BAD;
}
static inline const char *n48_cg84_mode_name(uint32_t mode) {
    return mode == N48_CG84_SHADOW ? "SHADOW(340)" : mode == N48_CG84_ON ? "ON(852)" : "OFF(596)";
}
// The recorder's `fine` latch (navi48_cg_seg_begin): only ON.
static inline uint32_t n48_cg84_fine_of(uint32_t mode) { return mode == N48_CG84_ON ? 1u : 0u; }
// Which of the two answers is COUNTED (the segment's admit): the fine one only under ON. SHADOW's fine answer is an instrument.
static inline uint32_t n48_cg84_counted(uint32_t mode, uint32_t pageR, uint32_t fineR) {
    return mode == N48_CG84_ON ? fineR : pageR;
}

// ---- the reader's counters (CG84.md item 9, line 1) ---------------------------------------------------------------------------
typedef struct { uint64_t fine, admitted, refused, fullNotes; } n48_cg84_stats;
// One segment check under SHADOW or ON: `pageR` the page answer (SHADOW: the counted one; ON: an uncounted instrument), `fineR` the
// granule answer (SHADOW: the uncounted instrument over dlo/dhi; ON: the counted one).
static inline void n48_cg84_note(n48_cg84_stats *s, uint32_t pageR, uint32_t fineR) {
    s->fine++;
    if (pageR != N48_CG_OK && fineR == N48_CG_OK) s->admitted++;
    if (fineR != N48_CG_OK) s->refused++;
}
#define N48_CG84_FMT "cg84: M%u fine %llu admitted-by-granule %llu refused %llu full-page-notes %llu"

// ---- the writer: capacities ---------------------------------------------------------------------------------------------------
#define N48_D84_KEYS      4u
#define N48_D84_MAX_BYTES 0x8000u   // eligibility: size <= 32 KiB (the shadow and the scratch are this size)
#define N48_D84_GRAN      16u       // the delta's alignment: one copy-guard granule
#define N48_D84_LOG_FIRST 32u       // DELTA lines: the first 32, then every 256th
#define N48_D84_LOG_EVERY 256u

// ---- eligibility (CG84.md item 6) ---------------------------------------------------------------------------------------------
enum {
    N48_D84_C_NOKEY = 0u,   // no valid key yet (the copy establishes one)
    N48_D84_C_EST,          // (a counter, not a cause: copies that established a key)
    N48_D84_C_INVAL,        // the key's counter moved since it was established, or an overlapping scope was open at G0
    N48_D84_C_POISON,       // a poison row overlaps the key's range
    N48_D84_C_BIG,          // not eligible: type, SW_MODE, size, segments, the switch-59 image, a VA, the VRAM guard
    N48_D84_C_SC,           // the key's establishing census found programs, the copy bumped switch 62, or it was re-tiled
    N48_D84_C_BUSY,         // the scratch was owned by another copy, or a second copy of the same key began
    N48_D84_C_STATE,        // the switch-82 state snapshot changed since the key was established
    N48_D84_CAUSES
};
typedef struct {
    uint32_t type;        // res+0x14
    uint32_t swz;         // n48_rp_swz_dword (the record the hardware reads; res+0x1dc without one)
    uint32_t swz1dc;      // res+0x1dc
    uint32_t maskBad;     // res+0x180 set but not a readable kernel pointer
    uint64_t bytes;       // res+0x230
    uint32_t dsegs;       // the pre-flight's destination segment count
    uint32_t linBytes;    // switch 59's prepared image length (0 = none)
    uint32_t vaOk;        // the destination map's GPU VA was read
    uint32_t keyOk;       // res, dstMem and md are kernel pointers (the caller checked)
} n48_d84_elig;
// 1 = eligible. SW_MODE 0 in BOTH swizzle dwords makes every re-tile impossible (n48_rp_shape_check refuses MODE), so "no retile"
// is decided here, before the scope, never after it.
static inline uint32_t n48_d84_eligible(const n48_d84_elig *e) {
    return e && e->type == 0x40u && (e->swz & 0x1fu) == 0u && (e->swz1dc & 0x1fu) == 0u && !e->maskBad && e->bytes &&
           e->bytes <= N48_D84_MAX_BYTES && !(e->bytes & (N48_D84_GRAN - 1u)) && e->dsegs == 1u && e->linBytes == 0u &&
           e->vaOk && e->keyOk;
}

// ---- the key ------------------------------------------------------------------------------------------------------------------
typedef struct { uint64_t res, dstMem, vram, bytes, boff, md, va; int64_t pid; } n48_d84_id;
static inline int n48_d84_id_eq(const n48_d84_id *a, const n48_d84_id *b) {
    return a->res == b->res && a->dstMem == b->dstMem && a->vram == b->vram && a->bytes == b->bytes && a->boff == b->boff &&
           a->md == b->md && a->va == b->va && a->pid == b->pid;
}
enum { N48_D84_S_FREE = 0u, N48_D84_S_INVALID = 1u, N48_D84_S_VALID = 2u };
typedef struct {
    uint32_t state;        // FREE / INVALID (the id and range are held: overlapping opens still move `inval`) / VALID
    uint32_t pend;         // a copy of it is between its G0 and its post-close
    uintptr_t pthr;        // that copy's thread: the COPIER-THREAD MARKER (its own open does not move `inval`)
    uint64_t pgen;         // moved on every plan, reset and eviction: a stale post-close never updates
    n48_d84_id id;         // vram/bytes are read lock-free by the bump (written before the state leaves FREE)
    uint64_t inval;        // bumped (atomic) by every invalidation
    uint64_t inval0;       // VALID: the `inval` value the shadow is exact for
    uint64_t lru;
    uint32_t programs;     // the establishing copy's census/scan found programs (or it was tainted): never a delta
    uint32_t pad;
    n48_sk82_snap snap;    // the state the shadow was established under
    uint8_t *shadow;       // N48_D84_MAX_BYTES
} n48_d84_key;

enum { N48_D84_K_NONE = 0u, N48_D84_K_EST = 1u, N48_D84_K_DELTA = 2u };
typedef struct {
    uintptr_t thr;         // the copier that owns the scratch (0 = free)
    uint32_t key, kind;    // the key; ESTABLISH or DELTA (what the copy is, or under SHADOW would be)
    uint32_t mode;         // the switch-84 mode latched at G0
    uint32_t act;          // 1 = this copy ACTS (ON): the source is the scratch, the fast copy is declined
    uint64_t pgen, c0;     // the key's pgen and the counter read at G0 (the INVAL SNAPSHOT)
    uint64_t ds, de;       // the delta within the resource (EST: 0, bytes)
    uint64_t lo, hi;       // the scope the copy opens ([vram + ds, vram + de) for an acting DELTA, else the full range)
    uint64_t dlo, dhi;     // the instrument range for the slot / BEGIN / END (the would-be delta)
    // the copy's result, before its scope closes (n48_d84_result) and its census (n48_d84_census)
    uint32_t resOk, programs;
    uint64_t compared, mismatched, wBytes;
    n48_sk82_snap snap;
    uint32_t taint;        // a scope overlapping the key was open (or torn, or untracked) at G0, poison, or (ON) an unknown write within
                           // 100 ms: this copy can never update the key
    uint32_t pad;
    uint64_t c1;           // the counter the update read (S1: END publishes it as inval0)
} n48_d84_pend;

typedef struct {
    uint64_t considered, eligible, delta, full, cause[N48_D84_CAUSES], bytes, max, mism, appleOutside;
    uint64_t wouldDelta, established, updated, rejected, invalAll, invalRes, invalUnmap, invalCommit, bumps, readFail, noKey;
    uint64_t foreign, quiet, quietWould, deltaPrograms, deltaUpd, unknowns, commits, probes, probeBad;   // 0.0.529 fix pass
} n48_d84_stats;

#define N48_D84_SEEN 16u   // CG84.md R6: a resource claims a key only on its SECOND eligible copy within the last 16 new ones
typedef struct {
    n48_d84_key k[N48_D84_KEYS];
    n48_d84_pend p;
    uint8_t *scratch;
    uint64_t tick;
    n48_d84_stats st;
    uint64_t seen[N48_D84_SEEN];   // res ^ vram of recent eligible copies that held no key
    uint32_t seenAt, pad;
} n48_d84_tab;
// R6: has this resource been copied (eligible, keyless) recently? If not, remember it and answer 0: a copy seen once never claims a
// key, reads nothing and costs nothing (only a re-copied table - USER2, the S# heap - takes one of the four keys).
static inline uint32_t n48_d84_seen_before(n48_d84_tab *t, const n48_d84_id *id) {
    const uint64_t h = (id->res ^ (id->vram << 1)) | 1ull;
    for (uint32_t i = 0; i < N48_D84_SEEN; i++) if (t->seen[i] == h) { t->seen[i] = 0ull; return 1u; }
    t->seen[t->seenAt++ % N48_D84_SEEN] = h;
    return 0u;
}

// The world the plan and the update read: the copy guard's slots, untracked count and poison (the kext's own), and a STEP hook the
// host test uses to run a foreign writer between two reads (null in the kext).
typedef struct {
    const n48_cg_slot *slots; uint32_t nslots;
    const uint32_t *untracked;
    const n48_cg_poison *poison;
    void (*step)(void *ctx, uint32_t at); void *stepCtx;
} n48_d84_world;
enum { N48_D84_AT_G0_C0 = 1u, N48_D84_AT_G0_SCAN = 2u, N48_D84_AT_UPD_SCAN = 3u, N48_D84_AT_UPD_C1 = 4u };
static inline void n48_d84_step(const n48_d84_world *w, uint32_t at) { if (w && w->step) w->step(w->stepCtx, at); }

// A mode change: every key FREE (its pgen moves, so a pending copy's update finds nothing to update). The PENDING RECORD is kept: its
// copier still owns the scratch (it may be writing from it) until its own post-close releases it.
static inline void n48_d84_reset(n48_d84_tab *t) {
    for (uint32_t i = 0; i < N48_D84_KEYS; i++) {
        n48_d84_key *k = &t->k[i];
        __atomic_store_n(&k->state, N48_D84_S_FREE, __ATOMIC_SEQ_CST);
        k->pgen++; k->inval0 = 0ull; k->lru = 0ull; k->programs = 0u;
        __atomic_fetch_add(&k->inval, 1ull, __ATOMIC_SEQ_CST);
    }
}

// ---- the invalidations ----------------------------------------------------------------------------------------------------------
// A navi48_cg_open over [lo, hi) by thread `me`, AFTER its slot (or the untracked count) is published: every held key (not FREE)
// whose range overlaps moves, except the key whose pending copier is `me` (its own scope). LOCK-FREE (any thread, the open path).
static inline uint32_t n48_d84_bump(n48_d84_key *keys, uint32_t n, uint64_t lo, uint64_t hi, uintptr_t me) {
    uint32_t moved = 0u;
    for (uint32_t i = 0; i < n; i++) {
        n48_d84_key *k = &keys[i];
        if (__atomic_load_n(&k->state, __ATOMIC_SEQ_CST) == N48_D84_S_FREE) continue;
        const uint64_t v = __atomic_load_n(&k->id.vram, __ATOMIC_SEQ_CST), b = __atomic_load_n(&k->id.bytes, __ATOMIC_SEQ_CST);
        if (!n48_cg_overlap(v, v + b, lo, hi)) continue;
        if (__atomic_load_n(&k->pend, __ATOMIC_SEQ_CST) && __atomic_load_n(&k->pthr, __ATOMIC_SEQ_CST) == me) continue;
        __atomic_fetch_add(&k->inval, 1ull, __ATOMIC_SEQ_CST);
        moved++;
    }
    return moved;
}
// Under the lock: invalidate every key (a context release, a WindowServer drop, a frame whose targets were not all held), the keys
// of one resource (a page-out), the keys whose VA range overlaps [va, va + size) (an unmapVA; size 0 = everything), or whose VA
// range holds a target base (a committed frame's CB/DB).
static inline void n48_d84_kill(n48_d84_key *k) {
    if (__atomic_load_n(&k->state, __ATOMIC_SEQ_CST) == N48_D84_S_VALID) __atomic_store_n(&k->state, N48_D84_S_INVALID, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&k->inval, 1ull, __ATOMIC_SEQ_CST);
}
static inline void n48_d84_inval_all(n48_d84_tab *t) {
    for (uint32_t i = 0; i < N48_D84_KEYS; i++) if (t->k[i].state != N48_D84_S_FREE) n48_d84_kill(&t->k[i]);
    t->st.invalAll++;
}
static inline void n48_d84_inval_res(n48_d84_tab *t, uint64_t res) {
    for (uint32_t i = 0; i < N48_D84_KEYS; i++)
        if (t->k[i].state != N48_D84_S_FREE && t->k[i].id.res == res) { n48_d84_kill(&t->k[i]); t->st.invalRes++; }
}
static inline void n48_d84_inval_va(n48_d84_tab *t, uint64_t va, uint64_t size) {
    for (uint32_t i = 0; i < N48_D84_KEYS; i++) {
        n48_d84_key *k = &t->k[i];
        if (k->state == N48_D84_S_FREE) continue;
        if (!size || n48_cg_overlap(k->id.va, k->id.va + k->id.bytes, va, va + size)) { n48_d84_kill(k); t->st.invalUnmap++; }
    }
}
// A committed frame: `tva` its held CB/DB target base VAs, `complete` 0 when it had more targets than were held.
static inline void n48_d84_inval_targets(n48_d84_tab *t, const uint64_t *tva, uint32_t nt, uint32_t complete) {
    if (!complete) { n48_d84_inval_all(t); return; }
    for (uint32_t i = 0; i < N48_D84_KEYS; i++) {
        n48_d84_key *k = &t->k[i];
        if (k->state == N48_D84_S_FREE) continue;
        for (uint32_t j = 0; j < nt; j++)
            if (tva[j] < k->id.va + k->id.bytes) { n48_d84_kill(k); t->st.invalCommit++; break; }   // MF-2: ANY base at or below the end
    }
}

// ---- the diff and the delta ---------------------------------------------------------------------------------------------------
// [ds, de) covers every differing byte, 16-byte aligned: ds = first differing byte & ~15, de = (last differing byte + 16) & ~15
// (clamped to n, which is a multiple of 16 for every eligible copy). An EMPTY diff answers [0, 16): SKIP stays forbidden.
static inline void n48_d84_diff(const uint8_t *src, const uint8_t *shadow, uint64_t n, uint64_t *ds, uint64_t *de) {
    uint64_t first = n, last = 0ull;
    for (uint64_t i = 0; i < n; i++) if (src[i] != shadow[i]) { first = i; break; }
    if (first == n) { *ds = 0ull; *de = n < N48_D84_GRAN ? n : N48_D84_GRAN; return; }
    for (uint64_t i = n; i-- > first; ) if (src[i] != shadow[i]) { last = i; break; }
    *ds = first & ~(uint64_t)(N48_D84_GRAN - 1u);
    uint64_t e = (last + N48_D84_GRAN) & ~(uint64_t)(N48_D84_GRAN - 1u);
    *de = e > n ? n : e;
}
// The delta's scope: exactly the bytes the loop writes.
static inline void n48_d84_scope(uint64_t vram, uint64_t ds, uint64_t de, uint64_t *lo, uint64_t *hi) {
    *lo = vram + ds; *hi = vram + de;
}
// THE DELTA WRITE LOOP (CG84.md item 6: the MM loop only, every dword read back). `wr`/`rd` are navi48_vram_write_mm / read_mm in the
// kext (a fake VRAM in the test); the bytes come from `img` (the plan-time source image, the scratch). Batches of at most 64 dwords
// over [ds, de). Returns 0 when a write or read-back batch was refused (a partial write: the caller poisons), else 1. `buf`/`back`
// (64 dwords each) are the caller's: the kext's are file-scope statics (one delta at a time: the scratch's owner), never stack.
typedef int (*n48_d84_wr_fn)(void *ctx, uint64_t vram, const uint32_t *src, uint32_t dwords);
typedef int (*n48_d84_rd_fn)(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords);
static inline int n48_d84_copy_loop(void *ctx, n48_d84_wr_fn wr, n48_d84_rd_fn rd, const uint8_t *img, uint64_t vram, uint64_t ds,
                                    uint64_t de, uint64_t *compared, uint64_t *mismatched, uint32_t *buf, uint32_t *back) {
    for (uint64_t pos = ds; pos < de; ) {
        const uint64_t left = de - pos;
        const uint32_t take = (uint32_t)(left >= 256u ? 256u : left);
        const uint32_t cnt = take / 4u;
        if (!cnt) return 0;
        for (uint32_t i = 0; i < cnt; i++) {
            const uint8_t *b = img + pos + 4u * i;
            buf[i] = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        }
        if (!wr(ctx, vram + pos, buf, cnt)) return 0;
        if (!rd(ctx, vram + pos, back, cnt)) return 0;
        for (uint32_t i = 0; i < cnt; i++) if (back[i] != buf[i]) (*mismatched)++;
        *compared += cnt;
        pos += (uint64_t)cnt * 4u;
    }
    return 1;
}

// ---- G0: THE PLAN -------------------------------------------------------------------------------------------------------------------
typedef struct {
    uint32_t act;          // 1 = ON acts on this copy (ESTABLISH through the MM path from the scratch, or a DELTA)
    uint32_t kind;         // N48_D84_K_EST / N48_D84_K_DELTA / N48_D84_K_NONE (no key: not eligible, busy)
    uint64_t ds, de;       // the (would-be) delta within the resource
    uint64_t lo, hi;       // the scope to open
    uint64_t dlo, dhi;     // the instrument range
    uint32_t cause;        // why not a delta (N48_D84_CAUSES = it is one)
    uint32_t outside;      // the delta is not inside Apple's dirty union (a count only)
} n48_d84_plan_out;

static inline int32_t n48_d84_find(const n48_d84_tab *t, const n48_d84_id *id) {
    for (uint32_t i = 0; i < N48_D84_KEYS; i++) if (t->k[i].state != N48_D84_S_FREE && n48_d84_id_eq(&t->k[i].id, id)) return (int32_t)i;
    return -1;
}
// The key a NEW resource may take: a FREE one first, else the least-recently used INVALID one, else a VALID one idle for more than
// N48_D84_STALE plans (CG84.md R6: four keys must not churn - a valid key of a re-copied table is never evicted by a passer-by).
// No copy may be pending on it. -1: none.
#define N48_D84_STALE 256u
static inline int32_t n48_d84_victim(const n48_d84_tab *t) {
    int32_t v = -1;
    for (uint32_t i = 0; i < N48_D84_KEYS; i++) {
        const n48_d84_key *k = &t->k[i];
        if (k->pend) continue;
        if (k->state == N48_D84_S_FREE) return (int32_t)i;
        if (k->state == N48_D84_S_VALID && t->tick - k->lru <= N48_D84_STALE) continue;
        if (v < 0 || k->lru < t->k[v].lru) v = (int32_t)i;
    }
    return v;
}
static inline int n48_d84_slots_open(const n48_d84_world *w, uint64_t lo, uint64_t hi) {
    if (w->untracked && __atomic_load_n(w->untracked, __ATOMIC_SEQ_CST)) return 1;
    return n48_cg_slot_scan(w->slots, w->nslots, lo, hi) != N48_CG_SLOT_NONE;
}

// ---- 0.0.529 FIX PASS: THE PLAN IN THREE STEPS (S1): CLAIM under the lock, the source READ with the lock dropped (into the scratch,
// which the claim made this thread's), DECIDE under the lock again. D1 order kept: the marker (claim), the read, c0, the slots.
#define N48_D84_QUIET_US 100000u   // MF-1: an unknown write (Apple SDMA, an un-NOPed client IB, a committed frame) in the last 100 ms
typedef uint32_t (*n48_d84_progs_fn)(void *ctx, const uint8_t *img, uint64_t bytes, uint64_t ds, uint64_t de);
typedef struct {
    uint32_t mode;             // switch 84's mode (SHADOW or ON; OFF never plans)
    uint32_t elig;             // n48_d84_eligible
    int64_t wsPid;             // MF-5: the bound WindowServer pid (-1: none bound)
    uint64_t nowUs, lastUnknownUs;   // MF-1: the uptime now and of the last unknown write (0: never)
    n48_d84_progs_fn progs; void *progsCtx;   // MF-3: shadercache's own candidate predicate over scratch[ds, de)
    uint64_t cgLo, cgHi, dirtyLo, dirtyHi;
} n48_d84_in;
static inline void n48_d84_out_full(n48_d84_plan_out *o, const n48_d84_in *in, const n48_d84_id *id) {
    o->act = 0u; o->kind = N48_D84_K_NONE; o->ds = 0ull; o->de = id->bytes; o->lo = in->cgLo; o->hi = in->cgHi;
    o->dlo = in->cgLo; o->dhi = in->cgHi; o->cause = N48_D84_C_BIG; o->outside = 0u;
}
// (1) CLAIM (under the lock). -1: the copy is 0.0.528's (o says why). Else the key index; the marker is set, and the scratch is this
// thread's (p.thr) until its post-close or n48_d84_unclaim.
static inline int32_t n48_d84_claim(n48_d84_tab *t, const n48_d84_in *in, const n48_d84_id *id, uintptr_t me, n48_d84_plan_out *o) {
    n48_d84_out_full(o, in, id);
    t->st.considered++;
    if (in->mode != N48_CG84_SHADOW && in->mode != N48_CG84_ON) return -1;
    if (!in->elig || !t->scratch) { t->st.full++; t->st.cause[N48_D84_C_BIG]++; return -1; }
    if (in->wsPid < 0 || id->pid != in->wsPid) {             // MF-5: only the bound WindowServer's tables ever take a key
        t->st.full++; t->st.foreign++; t->st.cause[N48_D84_C_BIG]++; return -1;
    }
    t->st.eligible++;
    if (t->p.thr) { t->st.full++; t->st.cause[N48_D84_C_BUSY]++; o->cause = N48_D84_C_BUSY; return -1; }
    int32_t ki = n48_d84_find(t, id);
    if (ki >= 0 && t->k[ki].pend) {                          // a second copy of the same key: neither may trust the other
        n48_d84_kill(&t->k[ki]);
        t->st.full++; t->st.cause[N48_D84_C_BUSY]++; o->cause = N48_D84_C_BUSY; return -1;
    }
    if (ki < 0) {
        if (!n48_d84_seen_before(t, id)) { t->st.full++; t->st.cause[N48_D84_C_NOKEY]++; o->cause = N48_D84_C_NOKEY; return -1; }
        ki = n48_d84_victim(t);
        if (ki < 0) { t->st.full++; t->st.noKey++; t->st.cause[N48_D84_C_NOKEY]++; o->cause = N48_D84_C_NOKEY; return -1; }
        n48_d84_key *nk = &t->k[ki];
        // a new key: its id and range are written BEFORE it leaves FREE, so every later opener that overlaps moves its counter
        nk->pgen++; nk->programs = 0u; nk->inval0 = 0ull;
        __atomic_store_n(&nk->id.vram, id->vram, __ATOMIC_SEQ_CST);
        __atomic_store_n(&nk->id.bytes, id->bytes, __ATOMIC_SEQ_CST);
        nk->id = *id;
        __atomic_fetch_add(&nk->inval, 1ull, __ATOMIC_SEQ_CST);
        __atomic_store_n(&nk->state, N48_D84_S_INVALID, __ATOMIC_SEQ_CST);
    }
    n48_d84_key *k = &t->k[ki];
    k->lru = ++t->tick;
    // THE MARKER (this copy's own open will not move the counter) and the scratch's owner
    __atomic_store_n(&k->pthr, me, __ATOMIC_SEQ_CST);
    __atomic_store_n(&k->pend, 1u, __ATOMIC_SEQ_CST);
    t->p.thr = me; t->p.key = (uint32_t)ki; t->p.lo = t->p.hi = 0ull;   // no scope matches a claim that has not decided
    return ki;
}
// The read failed (or the claim went stale): release the scratch and the marker, the key invalidated (under the lock).
static inline void n48_d84_unclaim(n48_d84_tab *t, int32_t ki, uintptr_t me) {
    if (ki < 0 || t->p.thr != me) return;
    n48_d84_key *k = &t->k[ki % (int32_t)N48_D84_KEYS];
    n48_d84_kill(k); k->pgen++;
    __atomic_store_n(&k->pend, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&k->pthr, (uintptr_t)0, __ATOMIC_SEQ_CST);
    t->p.thr = 0u;
}
// (3) DECIDE (under the lock), after the source is in the scratch: THE INVAL SNAPSHOT (c0), THEN the slots, then poison. Returns act.
// MF-4: act = ON, no programs, not tainted, and a delta / an establishing copy (NOKEY or INVAL); every other cause is 0.0.528's copy.
static inline uint32_t n48_d84_decide(n48_d84_tab *t, const n48_d84_world *w, const n48_d84_in *in, const n48_d84_id *id,
                                      const n48_sk82_snap *snap, int32_t ki, uintptr_t me, n48_d84_plan_out *o) {
    n48_d84_out_full(o, in, id);
    n48_d84_key *k = &t->k[ki % (int32_t)N48_D84_KEYS];
    if (t->p.thr != me || !k->pend || k->pthr != me || k->state == N48_D84_S_FREE) {   // a mode change reset it meanwhile
        n48_d84_unclaim(t, ki, me);
        t->st.full++; t->st.cause[N48_D84_C_BUSY]++; o->cause = N48_D84_C_BUSY; return 0u;
    }
    const uint8_t *src = t->scratch;
    const uint64_t c0 = __atomic_load_n(&k->inval, __ATOMIC_SEQ_CST);
    n48_d84_step(w, N48_D84_AT_G0_C0);
    const int open = n48_d84_slots_open(w, id->vram, id->vram + id->bytes);
    n48_d84_step(w, N48_D84_AT_G0_SCAN);
    const int pois = n48_cg_poison_overlaps(w->poison, id->vram, id->vram + id->bytes);
    const int quiet = in->lastUnknownUs && in->nowUs >= in->lastUnknownUs && in->nowUs - in->lastUnknownUs < N48_D84_QUIET_US;
    uint32_t cause = N48_D84_CAUSES;
    if (pois) { cause = N48_D84_C_POISON; n48_d84_kill(k); }
    else if (k->programs) cause = N48_D84_C_SC;                              // MF-4: BEFORE the state test
    else if (k->state != N48_D84_S_VALID) cause = N48_D84_C_NOKEY;
    else if (c0 != k->inval0 || open) { cause = N48_D84_C_INVAL; __atomic_store_n(&k->state, N48_D84_S_INVALID, __ATOMIC_SEQ_CST); }
    else if (!n48_sk82_snap_eq(&k->snap, snap)) cause = N48_D84_C_STATE;
    uint32_t taint = (open || pois) ? 1u : 0u;
    if (quiet) {                                                             // MF-1
        if (in->mode == N48_CG84_ON) {
            taint = 1u;
            if (cause == N48_D84_CAUSES) { cause = N48_D84_C_INVAL; __atomic_store_n(&k->state, N48_D84_S_INVALID, __ATOMIC_SEQ_CST); }
            t->st.quiet++;
        } else if (cause == N48_D84_CAUSES) t->st.quietWould++;            // SHADOW: priced, not acted on
    }
    uint64_t ds = 0ull, de = id->bytes;
    uint32_t progs = 0u;
    if (cause == N48_D84_CAUSES) {
        n48_d84_diff(src, k->shadow, id->bytes, &ds, &de);
        progs = in->progs ? in->progs(in->progsCtx, src, id->bytes, ds, de) : 1u;   // MF-3 (no predicate: fail-closed)
        if (progs) t->st.deltaPrograms++;
    }
    k->pgen++;
    n48_d84_pend *p = &t->p;
    p->thr = me; p->key = (uint32_t)ki; p->mode = in->mode; p->pgen = k->pgen; p->c0 = c0;
    p->ds = ds; p->de = de; p->kind = (cause == N48_D84_CAUSES && !progs) ? N48_D84_K_DELTA : N48_D84_K_EST;
    if (p->kind == N48_D84_K_EST) { p->ds = 0ull; p->de = id->bytes; }
    p->resOk = 0u; p->programs = 0u; p->compared = p->mismatched = p->wBytes = 0ull; p->snap = *snap; p->c1 = 0ull;
    p->taint = taint;
    p->act = (in->mode == N48_CG84_ON && !k->programs && !taint && !progs &&
              (cause == N48_D84_CAUSES || cause == N48_D84_C_NOKEY || cause == N48_D84_C_INVAL)) ? 1u : 0u;
    o->act = p->act; o->kind = p->kind; o->ds = p->ds; o->de = p->de; o->cause = cause;
    o->dlo = id->vram + p->ds; o->dhi = id->vram + p->de;
    if (p->act && p->kind == N48_D84_K_DELTA) n48_d84_scope(id->vram, p->ds, p->de, &o->lo, &o->hi);
    p->lo = o->lo; p->hi = o->hi; p->dlo = o->dlo; p->dhi = o->dhi;
    if (p->kind == N48_D84_K_DELTA) {
        o->outside = (in->dirtyHi <= in->dirtyLo || ds < in->dirtyLo || de > in->dirtyHi) ? 1u : 0u;
        if (o->outside) t->st.appleOutside++;
        if (p->act) {
            t->st.delta++; t->st.bytes += de - ds;
            if (de - ds > t->st.max) t->st.max = de - ds;
        } else {
            t->st.wouldDelta++; t->st.full++;
        }
    } else {
        t->st.full++; t->st.cause[cause < N48_D84_CAUSES ? cause : N48_D84_C_SC]++;
    }
    return o->act;
}
// The three steps in one (the host tests; the kext drops the lock around `rd`, navi48_d84_plan).
typedef int (*n48_d84_src_fn)(void *ctx, uint8_t *dst, uint64_t n);
static inline uint32_t n48_d84_plan(n48_d84_tab *t, const n48_d84_world *w, const n48_d84_in *in, const n48_d84_id *id,
                                    const n48_sk82_snap *snap, n48_d84_src_fn rd, void *rdCtx, uintptr_t me, n48_d84_plan_out *o) {
    const int32_t ki = n48_d84_claim(t, in, id, me, o);
    if (ki < 0) return 0u;
    if (!rd || !rd(rdCtx, t->scratch, id->bytes)) {
        n48_d84_unclaim(t, ki, me);
        t->st.full++; t->st.readFail++; t->st.cause[N48_D84_C_BIG]++; return 0u;
    }
    return n48_d84_decide(t, w, in, id, snap, ki, me, o);
}

// ---- before the scope closes: the copy's result; and the census (shadercache's programs / the switch-62 bump) -------------------
static inline void n48_d84_result(n48_d84_tab *t, uintptr_t me, uint32_t ok, uint64_t compared, uint64_t mismatched, uint64_t wBytes) {
    if (!t->p.thr || t->p.thr != me) return;
    t->p.resOk = ok ? 1u : 0u; t->p.compared = compared; t->p.mismatched = mismatched; t->p.wBytes = wBytes;
}
static inline void n48_d84_census(n48_d84_tab *t, uintptr_t me, uint32_t programs) {
    if (!t->p.thr || t->p.thr != me) return;
    t->p.programs += programs;
}

// ---- AFTER the scope closed: the update (slots THEN the counter), and the scratch's release ---------------------------------------
// The shadow becomes the scratch ONLY when: the copy is this thread's pending one with an unchanged pgen; it closed clean and reported
// 0 mismatches; under ON it ACTED (a copy that did not act wrote the backing, not the scratch) and read back every byte it wrote
// (compared x 4 >= the span); under SHADOW any read-back count ("sampled trust"); no programs; not tainted (a scope open or poison at
// G0, an unknown write within 100 ms under ON); no poison and no scope open now; the counter equal to the G0 snapshot; for a delta,
// the key still VALID at inval0 == c0.
static inline uint32_t n48_d84_close_ok(const n48_d84_pend *p, uint32_t wrote, uint32_t failed, uint32_t mismatch, int open,
                                        int pois, uint64_t c1) {
    if (!wrote || failed || mismatch || open || pois || p->taint) return 0u;
    if (!p->resOk || p->mismatched || p->programs) return 0u;
    if (!p->act && p->mode != N48_CG84_SHADOW) return 0u;
    const uint64_t span = p->de - p->ds;
    if (p->act && p->compared * 4ull < span) return 0u;
    if (p->kind == N48_D84_K_EST && p->wBytes != span) return 0u;
    return c1 == p->c0 ? 1u : 0u;
}
// S1: the update in three steps too. (1) BEGIN (under the lock): the checks. 0: the key was rejected and the scratch released (or the
// scope was not the pending copy's). 1: the caller copies the scratch into the key's shadow with the lock DROPPED (the key is still
// pending on this thread: no plan reads that shadow, no victim takes that key, a reset only moves its pgen), then (3) END under the lock.
static inline uint32_t n48_d84_closed_begin(n48_d84_tab *t, const n48_d84_world *w, uintptr_t me, uint64_t lo, uint64_t hi,
                                            uint32_t wrote, uint32_t failed, uint32_t mismatch) {
    n48_d84_pend *p = &t->p;
    if (!p->thr || p->thr != me || p->lo != lo || p->hi != hi) return 0u;
    n48_d84_key *k = &t->k[p->key % N48_D84_KEYS];
    if (k->pgen == p->pgen && k->state != N48_D84_S_FREE) {
        const int open = n48_d84_slots_open(w, k->id.vram, k->id.vram + k->id.bytes);    // SLOTS, THEN the counter
        n48_d84_step(w, N48_D84_AT_UPD_SCAN);
        const uint64_t c1 = __atomic_load_n(&k->inval, __ATOMIC_SEQ_CST);
        n48_d84_step(w, N48_D84_AT_UPD_C1);
        const int pois = n48_cg_poison_overlaps(w->poison, k->id.vram, k->id.vram + k->id.bytes);
        uint32_t ok = n48_d84_close_ok(p, wrote, failed, mismatch, open, pois, c1);
        if (ok && p->kind == N48_D84_K_DELTA && (k->state != N48_D84_S_VALID || k->inval0 != p->c0)) ok = 0u;
        if (p->act && p->kind == N48_D84_K_DELTA) t->st.mism += p->mismatched;
        if (ok) { p->c1 = c1; return 1u; }
        if (p->programs) k->programs = 1u;
        n48_d84_kill(k);
        t->st.rejected++;
        k->pgen++;
    }
    __atomic_store_n(&k->pend, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&k->pthr, (uintptr_t)0, __ATOMIC_SEQ_CST);
    p->thr = 0u;
    return 0u;
}
// (2) the copy (the lock dropped; this thread owns both buffers).
static inline void n48_d84_shadow_copy(n48_d84_tab *t) {
    n48_d84_key *k = &t->k[t->p.key % N48_D84_KEYS];
    for (uint64_t i = 0; i < k->id.bytes && i < N48_D84_MAX_BYTES; i++) k->shadow[i] = t->scratch[i];
}
// (3) END (under the lock): publish VALID at inval0 = c1 - unless a mode change reset the key meanwhile, or MF-6's probe found VRAM
// different from the shadow (`probeBad`: the key is invalidated instead).
static inline uint32_t n48_d84_closed_end(n48_d84_tab *t, uintptr_t me, uint32_t probeBad) {
    n48_d84_pend *p = &t->p;
    if (!p->thr || p->thr != me) return 0u;
    n48_d84_key *k = &t->k[p->key % N48_D84_KEYS];
    uint32_t upd = 0u;
    if (k->pgen == p->pgen && k->state != N48_D84_S_FREE && !probeBad) {
        k->inval0 = p->c1; k->snap = p->snap; k->programs = 0u;
        if (k->state != N48_D84_S_VALID) t->st.established++;
        __atomic_store_n(&k->state, N48_D84_S_VALID, __ATOMIC_SEQ_CST);
        t->st.updated++;
        if (p->act && p->kind == N48_D84_K_DELTA) t->st.deltaUpd++;
        upd = 1u;
    } else if (k->state != N48_D84_S_FREE) {
        n48_d84_kill(k); t->st.rejected++;
    }
    k->pgen++;
    __atomic_store_n(&k->pend, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&k->pthr, (uintptr_t)0, __ATOMIC_SEQ_CST);
    p->thr = 0u;
    return upd;
}
// The three in one (the host tests).
static inline uint32_t n48_d84_closed(n48_d84_tab *t, const n48_d84_world *w, uintptr_t me, uint64_t lo, uint64_t hi, uint32_t wrote,
                                      uint32_t failed, uint32_t mismatch) {
    if (!n48_d84_closed_begin(t, w, me, lo, hi, wrote, failed, mismatch)) return 0u;
    n48_d84_shadow_copy(t);
    return n48_d84_closed_end(t, me, 0u);
}
// MF-6: THE INVARIANT PROBE, after every 32nd DELTA update under ON: read the whole key range back and compare it with the shadow.
#define N48_D84_PROBE_EVERY 32u
static inline uint32_t n48_d84_probe_due(uint64_t deltaUpdatesSoFar) {
    return ((deltaUpdatesSoFar + 1ull) % N48_D84_PROBE_EVERY) == 0u ? 1u : 0u;
}
// One read-back batch at resource offset `off`: the first differing byte offset, or ~0 when equal.
static inline uint64_t n48_d84_probe_cmp(const uint8_t *shadow, uint64_t off, const uint32_t *vr, uint32_t dwords) {
    for (uint32_t i = 0; i < dwords; i++) {
        const uint8_t *s = shadow + off + 4u * i;
        const uint32_t w = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
        if (w != vr[i]) {
            for (uint32_t b = 0; b < 4u; b++) if (s[b] != (uint8_t)(vr[i] >> (8u * b))) return off + 4u * i + b;
        }
    }
    return ~0ull;
}
// S2 / MF-1 / MF-2: LOCK-FREE, every non-FREE key's counter moves (a commit, an unmap, a page-out, a context release, a WindowServer
// drop; under ON an Apple SDMA submission or an un-NOPed client IB). c0 != inval0 then forces INVAL at the next plan.
static inline uint32_t n48_d84_bump_all(n48_d84_key *keys, uint32_t n) {
    uint32_t moved = 0u;
    for (uint32_t i = 0; i < n; i++)
        if (__atomic_load_n(&keys[i].state, __ATOMIC_SEQ_CST) != N48_D84_S_FREE) { __atomic_fetch_add(&keys[i].inval, 1ull, __ATOMIC_SEQ_CST); moved++; }
    return moved;
}
// A copy that planned and then returned before opening its scope (a refusal between G0 and the scope): release the scratch and
// invalidate the key (nothing is known about what it wrote).
static inline void n48_d84_abandon(n48_d84_tab *t, uintptr_t me) {
    n48_d84_pend *p = &t->p;
    if (!p->thr || p->thr != me) return;
    n48_d84_key *k = &t->k[p->key % N48_D84_KEYS];
    n48_d84_kill(k); k->pgen++;
    __atomic_store_n(&k->pend, 0u, __ATOMIC_SEQ_CST);
    __atomic_store_n(&k->pthr, (uintptr_t)0, __ATOMIC_SEQ_CST);
    p->thr = 0u;
}

// ---- lines (each <= 491 bytes at every field's widest; tests/gfx_cg84_test.cpp measures the real strings) ----------------------
#define N48_D84_FMT \
    "d84: delta %llu full %llu (nokey/est/inval/poison/big/sc %llu/%llu/%llu/%llu/%llu/%llu) bytes %llu max %llu mism %llu " \
    "apple-outside %llu"
#define N48_D84_ARGS(s) \
    (unsigned long long)(s)->delta, (unsigned long long)(s)->full, (unsigned long long)(s)->cause[N48_D84_C_NOKEY], \
    (unsigned long long)(s)->established, (unsigned long long)(s)->cause[N48_D84_C_INVAL], \
    (unsigned long long)(s)->cause[N48_D84_C_POISON], (unsigned long long)(s)->cause[N48_D84_C_BIG], \
    (unsigned long long)(s)->cause[N48_D84_C_SC], (unsigned long long)(s)->bytes, (unsigned long long)(s)->max, \
    (unsigned long long)(s)->mism, (unsigned long long)(s)->appleOutside
#define N48_D84_FMT2 \
    "d84: %s%s; considered %llu elig %llu would %llu busy %llu state %llu upd %llu rej %llu; inval all %llu res %llu " \
    "unmap %llu commit %llu bumps %llu; keys %u/%u/%u (%s)"
#define N48_D84_FMT3 \
    "d84: unknown writes %llu (ON: every key moved) quiet-window full %llu (SHADOW would %llu); commits %llu; foreign pid %llu; " \
    "programs in a delta %llu; delta updates %llu; probes %llu, MISMATCHED %llu"
#define N48_D84_PROBE_FMT \
    "d84: SHADOW-VRAM MISMATCH - key %u resource %#llx VRAM [%#llx,%#llx) differs from its shadow at +%#llx (probe %llu, delta " \
    "update %llu); every key invalidated. STOP the run: a delta may have left stale bytes in VRAM"
#define N48_D84_DELTA_FMT \
    "residency-copy: DELTA #%llu resource=%p VRAM [%#llx,%#llx) of [%#llx,%#llx) (%llu B), %llu dword(s) read back, %llu " \
    "MISMATCHED, %llu us (delta #%llu)"

#endif // N48_GFX_CG84_H
