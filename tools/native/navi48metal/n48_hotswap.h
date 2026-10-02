// n48_hotswap.h - pure state machine for pipeline HOT-SWAP (native #12 hot-swap; no Metal, no Vulkan, no I/O; host test test-hotswap.c).
//
// A fallback pipeline object (spvcache miss in WindowServer) registers one n48hs_entry per object. A watcher thread feeds it file
// observations (stat() of <sha>.spv / <sha>.meta.json in the lookup directories) every tick; the entry answers BUILD exactly when the
// files are complete and STABLE (same size+mtime on two consecutive ticks and older than N48HS_MIN_AGE_NS: a half-copied file never
// builds). The builder reports back with build_done(ok): ok -> SWAPPED (the caller publishes the real object with n48hs_install, one
// atomic compare-and-swap, once), failure -> backoff and up to N48HS_MAX_ATTEMPTS tries, then GAVEUP (terminal).
//
// Swap safety bookkeeping (deferred destroy): users[0] counts holders of the FALLBACK generation (command buffers that encoded with
// it), users[1] holders of the real one. acquire() always hands out generation 1 once SWAPPED, 0 before; a holder keeps the generation
// it got. The fallback's Vulkan objects may be destroyed only when destroy_ok() says so: swapped AND users[0] == 0 AND not yet destroyed
// (it answers yes exactly once). In the bundle the same invariant is carried by ARC: every command buffer retains the pipeline object it
// encoded with (n48Retain), the fallback object keeps its VkPipeline/layout/module alive until its dealloc, and dealloc cannot run while a
// command buffer holds it.
#ifndef N48_HOTSWAP_H
#define N48_HOTSWAP_H
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#define N48HS_MAXKEY        2                      // render: vertex + fragment; compute: kernel
#define N48HS_MIN_AGE_NS    400000000LL            // newest file must be at least this old
#define N48HS_META_GRACE_NS 5000000000LL           // a render pipeline proceeds without a .meta.json after the .spv has been stable this long
#define N48HS_MAX_ATTEMPTS  4

typedef struct { int present; uint64_t size; int64_t mtime_ns; } n48hs_obs;
typedef struct { n48hs_obs spv, meta; } n48hs_key_obs;
typedef enum { N48HS_WAIT = 0, N48HS_BUILDING, N48HS_SWAPPED, N48HS_GAVEUP } n48hs_state;
typedef enum { N48HS_NONE = 0, N48HS_BUILD } n48hs_action;

typedef struct {
    int nkeys, meta_required;
    n48hs_state st;
    n48hs_key_obs last[N48HS_MAXKEY]; int have_last;
    int attempts; int64_t next_try_ns;
    uint32_t users[2];                  // [0] fallback generation, [1] real generation
    int fb_retired, fb_destroyed;
} n48hs_entry;

static inline void n48hs_init(n48hs_entry *e, int nkeys, int meta_required) {
    memset(e, 0, sizeof *e);
    e->nkeys = nkeys < 1 ? 1 : nkeys > N48HS_MAXKEY ? N48HS_MAXKEY : nkeys; e->meta_required = meta_required;
}
static inline int n48hs_obs_eq(const n48hs_obs *a, const n48hs_obs *b) { return a->present == b->present && a->size == b->size && a->mtime_ns == b->mtime_ns; }
static inline int n48hs_spv_ok(const n48hs_obs *o) { return o->present && o->size >= 20 && (o->size & 3) == 0; }   // same sanity as the lookup

// One watcher tick. now_ns is wall-clock (same clock as mtime_ns). Returns BUILD at most once per attempt (state -> BUILDING).
static inline n48hs_action n48hs_tick(n48hs_entry *e, int64_t now_ns, const n48hs_key_obs *cur) {
    if (e->st != N48HS_WAIT) return N48HS_NONE;
    int same = e->have_last, all_spv = 1, all_meta = 1; int64_t newest = 0;
    for (int k = 0; k < e->nkeys; k++) {
        if (!n48hs_obs_eq(&e->last[k].spv, &cur[k].spv) || !n48hs_obs_eq(&e->last[k].meta, &cur[k].meta)) same = 0;
        if (!n48hs_spv_ok(&cur[k].spv)) all_spv = 0; else if (cur[k].spv.mtime_ns > newest) newest = cur[k].spv.mtime_ns;
        if (!cur[k].meta.present || !cur[k].meta.size) all_meta = 0; else if (cur[k].meta.mtime_ns > newest) newest = cur[k].meta.mtime_ns;
    }
    if (e->have_last && !same) { e->attempts = 0; e->next_try_ns = 0; }   // the files changed: a new content, a fresh try budget
    memcpy(e->last, cur, sizeof(n48hs_key_obs) * (size_t)e->nkeys); e->have_last = 1;
    if (!all_spv || !same) return N48HS_NONE;
    if (now_ns - newest < N48HS_MIN_AGE_NS) return N48HS_NONE;
    if (!all_meta && (e->meta_required || now_ns - newest < N48HS_META_GRACE_NS)) return N48HS_NONE;
    if (now_ns < e->next_try_ns) return N48HS_NONE;
    e->st = N48HS_BUILDING; e->attempts++;
    return N48HS_BUILD;
}
// The builder finished. Returns 1 when the entry is now SWAPPED (publish the real object), 0 otherwise.
static inline int n48hs_build_done(n48hs_entry *e, int ok, int64_t now_ns) {
    if (e->st != N48HS_BUILDING) return 0;
    if (ok) { e->st = N48HS_SWAPPED; e->fb_retired = 1; return 1; }
    if (e->attempts >= N48HS_MAX_ATTEMPTS) { e->st = N48HS_GAVEUP; return 0; }
    e->st = N48HS_WAIT; e->next_try_ns = now_ns + (1000000000LL << e->attempts);
    return 0;
}
// A user (command buffer / encoder) picks a generation. After the swap new users always get 1; a user keeps the generation it was given.
static inline int n48hs_acquire(n48hs_entry *e) { int g = e->st == N48HS_SWAPPED ? 1 : 0; e->users[g]++; return g; }
static inline void n48hs_release(n48hs_entry *e, int gen) { if (gen >= 0 && gen <= 1 && e->users[gen]) e->users[gen]--; }
// 1 exactly once: swapped, nobody holds the fallback generation any more.
static inline int n48hs_destroy_ok(n48hs_entry *e) {
    if (!e->fb_retired || e->fb_destroyed || e->users[0]) return 0;
    e->fb_destroyed = 1; return 1;
}

// Publication slot: the real object pointer, written once (0 -> v) by the watcher, read lock-free by the encoders.
static inline int n48hs_install(_Atomic uintptr_t *slot, uintptr_t v) { uintptr_t exp = 0; return atomic_compare_exchange_strong(slot, &exp, v); }
static inline uintptr_t n48hs_load(_Atomic uintptr_t *slot) { return atomic_load_explicit(slot, memory_order_acquire); }
#endif
