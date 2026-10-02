// n48_pool.h: P1 "memory pooling" decisions as plain C (no Vulkan, no Foundation; host test: test-pool.c). Navi48Device.m does the Vulkan calls and holds ONE mutex
// around every call into this header. Time is a parameter (ns) so the tests control it.
//
//  (a) FENCE.  `submitted` is a monotonic serial taken when a command buffer is handed to vkQueueSubmit; `completed` is the highest S such that every command buffer
//      with serial <= S has finished (min(pending) - 1, or `submitted` when nothing is pending). Anything freed is stamped with `submitted` at free time and may be
//      reused only when completed >= stamp. A serial that is never marked done (a hung GPU) pins `completed`: nothing newer is ever reused (safe, not live).
//  (b) SLABS.  Allocations <= N48P_SUB_MAX with alignment <= N48P_SLAB_SIZE are sub-allocated from 4 MiB slabs, one set of slabs per key (the caller's memory type
//      and map class). Free ranges are kept sorted and coalesced. A slab that has been empty for >= N48P_IDLE_NS is released by n48p_trim.
//  (c) RECYCLE.  Whole allocations of N48P_SUB_MAX < size <= N48P_REC_MAX are kept, after the fence has cleared, in a cache keyed by (key, size), total <= N48P_REC_CAP
//      (oldest evicted first); an entry unused for > N48P_IDLE_NS is released by n48p_trim.
//  The pool never calls Vulkan: memory it gives up goes to a release queue (n48p_pop_release) that the caller drains and vkFreeMemory's outside its mutex.
//  (d) A fence-gated FIFO of opaque 64-bit handles (descriptor pools): n48p_h_push / n48p_h_pop.
// Not reused: n48_heapalloc.h (N48Heap's budget allocator) stores USED blocks of one heap and has neither a fence nor coalescing of free ranges, so it does not fit.
#ifndef N48_POOL_H
#define N48_POOL_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define N48P_SLAB_SIZE   (4ULL << 20)
#define N48P_SUB_MAX     (256ULL << 10)
#define N48P_REC_MAX     (64ULL << 20)
#define N48P_REC_CAP     (256ULL << 20)
#define N48P_IDLE_NS     2000000000ULL
#define N48P_DEFER_MAX   (512ULL << 20)   // whole allocations waiting for the fence beyond this are freed at once (the kernel's idle wait protects them, as today)

typedef struct { uint64_t off, size; } n48p_rng;
typedef struct {
    uint64_t id, size, nlive, emptySince; uint32_t key; int empty;
    void *mem, *map; n48p_rng *fr; size_t nfr, cfr;
} n48p_slab;
typedef struct { uint32_t key; uint64_t size, lastUse; void *mem, *map; } n48p_rec;
typedef struct { int whole; uint64_t stamp, slab, off, size; uint32_t key; void *mem, *map; } n48p_def;
typedef struct { void *mem, *map; uint64_t size; uint32_t key; int slab; } n48p_rel;   // slab = 1: a whole slab, 0: a recycled whole allocation

typedef struct {
    int on;                                                                  // the P1 switch, latched once by the caller: when 0 every entry point below is a no-op / "not pooled"
    uint64_t submitted, completed; uint64_t *pend; size_t npend, cpend;     // (a)
    n48p_slab **sl; size_t nsl, csl; uint64_t slabCtr;                       // (b)
    n48p_rec *rc; size_t nrc, crc; uint64_t recBytes;                        // (c)
    n48p_def *df; size_t hd, nd, cd; uint64_t deferWhole;                    // deferred frees: FIFO, stamps non-decreasing
    n48p_rel *rl; size_t nrl, crl;                                           // release queue
    uint64_t *hv, *hs; size_t hh, hn, hc;                                    // (d) handle FIFO
    // statistics
    uint64_t subAlloc, subFree, subHit, slabNew, slabRel, recHit, recMiss, recRel, dpReuse, dpCreate, avoided, directAlloc;
} n48p_pool;

static inline void n48p_init(n48p_pool *p, int on) { memset(p, 0, sizeof *p); p->on = on; }
static inline void n48p_destroy(n48p_pool *p) {
    for (size_t i = 0; i < p->nsl; i++) { free(p->sl[i]->fr); free(p->sl[i]); }
    free(p->sl); free(p->rc); free(p->df); free(p->rl); free(p->pend); free(p->hv); free(p->hs); { int on = p->on; memset(p, 0, sizeof *p); p->on = on; }
}
#define N48P_GROW(arr, n, cap, T) do { if ((n) == (cap)) { (cap) = (cap) ? (cap) * 2 : 16; (arr) = (T *)realloc((arr), (cap) * sizeof(T)); } } while (0)

// ---- (a) fence ------------------------------------------------------------------------------------------------------------------
static inline void n48p_fence_recompute(n48p_pool *p) { p->completed = p->npend ? p->pend[0] - 1 : p->submitted; }
static inline uint64_t n48p_submit(n48p_pool *p) {   // call under the same lock that orders the vkQueueSubmit calls; returns the serial
    if (!p->on) return 0;
    uint64_t s = ++p->submitted; N48P_GROW(p->pend, p->npend, p->cpend, uint64_t); p->pend[p->npend++] = s; n48p_fence_recompute(p); return s;
}
static inline void n48p_done(n48p_pool *p, uint64_t serial) {   // idempotent
    if (!p->on) return;
    for (size_t i = 0; i < p->npend; i++) if (p->pend[i] == serial) { memmove(&p->pend[i], &p->pend[i + 1], (p->npend - i - 1) * sizeof *p->pend); p->npend--; break; }
    n48p_fence_recompute(p);
}

// ---- (b) slabs ------------------------------------------------------------------------------------------------------------------
static inline uint64_t n48p_alup(uint64_t v, uint64_t a) { if (a <= 1) return v; if (!(a & (a - 1))) return (v + a - 1) & ~(a - 1); return ((v + a - 1) / a) * a; }
static inline n48p_slab *n48p_find_slab(n48p_pool *p, uint64_t id) { for (size_t i = 0; i < p->nsl; i++) if (p->sl[i]->id == id) return p->sl[i]; return NULL; }
static inline uint64_t n48p_add_slab(n48p_pool *p, uint32_t key, uint64_t size, void *mem, void *map, uint64_t now) {
    n48p_slab *s = (n48p_slab *)calloc(1, sizeof *s);
    s->id = ++p->slabCtr; s->key = key; s->size = size; s->mem = mem; s->map = map; s->fr = (n48p_rng *)malloc(4 * sizeof *s->fr); s->cfr = 4; s->nfr = 1; s->fr[0] = (n48p_rng){ 0, size };
    s->empty = 1; s->emptySince = now;
    N48P_GROW(p->sl, p->nsl, p->csl, n48p_slab *); p->sl[p->nsl++] = s; p->slabNew++; return s->id;
}
// First fit inside one slab. Returns 1 and the offset, or 0.
static inline int n48p_slab_take(n48p_slab *s, uint64_t size, uint64_t align, uint64_t *off) {
    for (size_t i = 0; i < s->nfr; i++) {
        uint64_t b = s->fr[i].off, e = b + s->fr[i].size, c = n48p_alup(b, align);
        if (c + size > e) continue;
        int front = c > b, back = c + size < e;
        if (front && back) {
            if (s->nfr == s->cfr) { s->cfr *= 2; s->fr = (n48p_rng *)realloc(s->fr, s->cfr * sizeof *s->fr); }
            memmove(&s->fr[i + 2], &s->fr[i + 1], (s->nfr - i - 1) * sizeof *s->fr); s->nfr++;
            s->fr[i] = (n48p_rng){ b, c - b }; s->fr[i + 1] = (n48p_rng){ c + size, e - (c + size) };
        } else if (front) s->fr[i].size = c - b;
        else if (back) s->fr[i] = (n48p_rng){ c + size, e - (c + size) };
        else { memmove(&s->fr[i], &s->fr[i + 1], (s->nfr - i - 1) * sizeof *s->fr); s->nfr--; }
        s->nlive++; s->empty = 0; *off = c; return 1;
    }
    return 0;
}
// Returns the range to the slab's sorted free list and merges it with its neighbours.
static inline void n48p_slab_give(n48p_slab *s, uint64_t off, uint64_t size, uint64_t now) {
    size_t i = 0; while (i < s->nfr && s->fr[i].off < off) i++;
    int mprev = i > 0 && s->fr[i - 1].off + s->fr[i - 1].size == off, mnext = i < s->nfr && off + size == s->fr[i].off;
    if (mprev && mnext) { s->fr[i - 1].size += size + s->fr[i].size; memmove(&s->fr[i], &s->fr[i + 1], (s->nfr - i - 1) * sizeof *s->fr); s->nfr--; }
    else if (mprev) s->fr[i - 1].size += size;
    else if (mnext) { s->fr[i].off = off; s->fr[i].size += size; }
    else {
        if (s->nfr == s->cfr) { s->cfr *= 2; s->fr = (n48p_rng *)realloc(s->fr, s->cfr * sizeof *s->fr); }
        memmove(&s->fr[i + 1], &s->fr[i], (s->nfr - i) * sizeof *s->fr); s->fr[i] = (n48p_rng){ off, size }; s->nfr++;
    }
    if (s->nlive) s->nlive--;
    if (!s->nlive && s->nfr == 1 && s->fr[0].off == 0 && s->fr[0].size == s->size) { s->empty = 1; s->emptySince = now; }
}

// ---- release queue and the recycle cache ------------------------------------------------------------------------------------------
static inline void n48p_release(n48p_pool *p, void *mem, void *map, uint64_t size, uint32_t key, int slab) {
    N48P_GROW(p->rl, p->nrl, p->crl, n48p_rel); p->rl[p->nrl++] = (n48p_rel){ mem, map, size, key, slab };
}
// Pops one memory object the caller must now vkFreeMemory. Returns 1 and fills *o, or 0 when the queue is empty.
static inline int n48p_pop_release(n48p_pool *p, n48p_rel *o) {
    if (!p->nrl) return 0;
    *o = p->rl[0]; memmove(&p->rl[0], &p->rl[1], (p->nrl - 1) * sizeof *p->rl); p->nrl--; return 1;
}
static inline void n48p_rec_drop(n48p_pool *p, size_t i) {
    p->recBytes -= p->rc[i].size; n48p_release(p, p->rc[i].mem, p->rc[i].map, p->rc[i].size, p->rc[i].key, 0); p->recRel++;
    memmove(&p->rc[i], &p->rc[i + 1], (p->nrc - i - 1) * sizeof *p->rc); p->nrc--;
}
static inline void n48p_rec_put(n48p_pool *p, uint32_t key, uint64_t size, void *mem, void *map, uint64_t now) {
    if (size > N48P_REC_CAP) { n48p_release(p, mem, map, size, key, 0); p->recRel++; return; }
    while (p->nrc && p->recBytes + size > N48P_REC_CAP) { size_t o = 0; for (size_t i = 1; i < p->nrc; i++) if (p->rc[i].lastUse < p->rc[o].lastUse) o = i; n48p_rec_drop(p, o); }
    N48P_GROW(p->rc, p->nrc, p->crc, n48p_rec); p->rc[p->nrc++] = (n48p_rec){ key, size, now, mem, map }; p->recBytes += size;
}

// ---- deferred frees ------------------------------------------------------------------------------------------------------------
static inline void n48p_apply(n48p_pool *p, const n48p_def *d, uint64_t now) {
    if (d->whole) { p->deferWhole -= d->size; n48p_rec_put(p, d->key, d->size, d->mem, d->map, now); return; }
    n48p_slab *s = n48p_find_slab(p, d->slab); if (s) n48p_slab_give(s, d->off, d->size, now);
}
// Applies every deferred free whose stamp the fence has cleared (the queue is in stamp order, so the head decides).
static inline void n48p_reap(n48p_pool *p, uint64_t now) {
    while (p->hd < p->nd && p->df[p->hd].stamp <= p->completed) n48p_apply(p, &p->df[p->hd++], now);
    if (p->hd == p->nd) p->hd = p->nd = 0;
}
static inline size_t n48p_waiting(const n48p_pool *p) { return p->nd - p->hd; }
static inline void n48p_defer(n48p_pool *p, n48p_def d) {
    if (p->hd && p->nd == p->cd) { memmove(p->df, &p->df[p->hd], (p->nd - p->hd) * sizeof *p->df); p->nd -= p->hd; p->hd = 0; }
    N48P_GROW(p->df, p->nd, p->cd, n48p_def); p->df[p->nd++] = d;
}

// ---- allocation ----------------------------------------------------------------------------------------------------------------
enum { N48P_R_SUB = 1, N48P_R_REC, N48P_R_NEED_SLAB, N48P_R_NEED_WHOLE, N48P_R_DIRECT };
typedef struct { int kind; uint64_t slab, off; void *mem, *map; } n48p_res;
// SUB: *off in slab r->slab (mem/map are the slab's: bind at off, contents = map + off). REC: a recycled whole allocation. NEED_SLAB: the caller allocates a slab
// (N48P_SLAB_SIZE, key), calls n48p_add_slab and asks again. NEED_WHOLE: the caller allocates exactly `size`; give it back with n48p_free_whole.
// DIRECT: too large / over-aligned: the caller allocates and frees it itself, as before.
static inline int n48p_alloc(n48p_pool *p, uint32_t key, uint64_t size, uint64_t align, uint64_t now, n48p_res *r) {
    memset(r, 0, sizeof *r); if (align < 1) align = 1;
    if (!p->on) { r->kind = N48P_R_DIRECT; return r->kind; }
    n48p_reap(p, now);
    if (!size) { r->kind = N48P_R_DIRECT; p->directAlloc++; return r->kind; }
    if (size <= N48P_SUB_MAX && align <= N48P_SLAB_SIZE) {
        for (size_t i = 0; i < p->nsl; i++) {
            n48p_slab *s = p->sl[i]; uint64_t off;
            if (s->key == key && n48p_slab_take(s, size, align, &off)) { r->kind = N48P_R_SUB; r->slab = s->id; r->off = off; r->mem = s->mem; r->map = s->map; p->subAlloc++; p->subHit++; p->avoided++; return r->kind; }
        }
        r->kind = N48P_R_NEED_SLAB; return r->kind;
    }
    if (size > N48P_SUB_MAX && size <= N48P_REC_MAX) {
        for (size_t i = 0; i < p->nrc; i++) if (p->rc[i].key == key && p->rc[i].size == size) {
            r->kind = N48P_R_REC; r->mem = p->rc[i].mem; r->map = p->rc[i].map; p->recBytes -= size;
            memmove(&p->rc[i], &p->rc[i + 1], (p->nrc - i - 1) * sizeof *p->rc); p->nrc--; p->recHit++; p->avoided++; return r->kind;
        }
        p->recMiss++; r->kind = N48P_R_NEED_WHOLE; return r->kind;
    }
    r->kind = N48P_R_DIRECT; p->directAlloc++; return r->kind;
}
// Accounts for a sub-allocation that followed an n48p_add_slab (the first allocation of a new slab is a miss, not an avoided kernel allocation).
static inline void n48p_note_new_slab_use(n48p_pool *p) { if (p->avoided) p->avoided--; if (p->subHit) p->subHit--; }

// ---- frees ---------------------------------------------------------------------------------------------------------------------
static inline void n48p_free_sub(n48p_pool *p, uint64_t slab, uint64_t off, uint64_t size, uint64_t now) {
    if (!p->on) return;
    p->subFree++;
    n48p_def d = { 0, p->submitted, slab, off, size, 0, NULL, NULL };
    n48p_reap(p, now);
    if (d.stamp <= p->completed && p->hd == p->nd) n48p_apply(p, &d, now);   // nothing in flight and nothing queued ahead: reusable now
    else n48p_defer(p, d);
}
// Returns 1: the allocation now belongs to the pool (cached after the fence clears). 0: the caller must vkFreeMemory it now.
static inline int n48p_free_whole(n48p_pool *p, uint32_t key, uint64_t size, void *mem, void *map, uint64_t now) {
    if (!p->on) return 0;
    n48p_reap(p, now);
    if (size > N48P_REC_MAX) return 0;
    if (p->deferWhole + size > N48P_DEFER_MAX) return 0;
    n48p_def d = { 1, p->submitted, 0, 0, size, key, mem, map };
    if (d.stamp <= p->completed && p->hd == p->nd) n48p_rec_put(p, key, size, mem, map, now);
    else { p->deferWhole += size; n48p_defer(p, d); }
    return 1;
}

// ---- trimming ------------------------------------------------------------------------------------------------------------------
// Moves recycle entries unused for > N48P_IDLE_NS and slabs empty for >= N48P_IDLE_NS to the release queue. flushAll (out of memory): every cleared cached entry and empty slab.
static inline void n48p_trim_x(n48p_pool *p, uint64_t now, int flushAll) {
    n48p_reap(p, now);
    for (size_t i = 0; i < p->nrc;) { if (flushAll || (now > p->rc[i].lastUse && now - p->rc[i].lastUse > N48P_IDLE_NS)) n48p_rec_drop(p, i); else i++; }
    for (size_t i = 0; i < p->nsl;) {
        n48p_slab *s = p->sl[i];
        if (s->empty && (flushAll || (now >= s->emptySince && now - s->emptySince >= N48P_IDLE_NS))) {
            n48p_release(p, s->mem, s->map, s->size, s->key, 1); p->slabRel++; free(s->fr); free(s);
            memmove(&p->sl[i], &p->sl[i + 1], (p->nsl - i - 1) * sizeof *p->sl); p->nsl--;
        } else i++;
    }
}
static inline void n48p_trim(n48p_pool *p, uint64_t now) { n48p_trim_x(p, now, 0); }
static inline void n48p_flush(n48p_pool *p, uint64_t now) { n48p_trim_x(p, now, 1); }

// ---- (d) fence-gated handle FIFO (descriptor pools) ----------------------------------------------------------------------------------
// push: 0 when the list already holds `cap` handles (the caller destroys it). pop: the oldest handle whose stamp is cleared, or 0.
static inline int n48p_h_push(n48p_pool *p, uint64_t h, size_t cap) {
    if (!p->on || p->hn - p->hh >= cap) return 0;
    if (p->hh && p->hn == p->hc) { memmove(p->hv, &p->hv[p->hh], (p->hn - p->hh) * sizeof *p->hv); memmove(p->hs, &p->hs[p->hh], (p->hn - p->hh) * sizeof *p->hs); p->hn -= p->hh; p->hh = 0; }
    if (p->hn == p->hc) { p->hc = p->hc ? p->hc * 2 : 16; p->hv = (uint64_t *)realloc(p->hv, p->hc * sizeof *p->hv); p->hs = (uint64_t *)realloc(p->hs, p->hc * sizeof *p->hs); }
    p->hv[p->hn] = h; p->hs[p->hn] = p->submitted; p->hn++; return 1;
}
static inline uint64_t n48p_h_pop(n48p_pool *p) {
    if (p->on && p->hh < p->hn && p->hs[p->hh] <= p->completed) { uint64_t h = p->hv[p->hh++]; if (p->hh == p->hn) p->hh = p->hn = 0; p->dpReuse++; p->avoided++; return h; }
    return 0;
}
static inline size_t n48p_h_count(const n48p_pool *p) { return p->hn - p->hh; }

// ---- statistics ----------------------------------------------------------------------------------------------------------------
static inline int n48p_fmt(const n48p_pool *p, char *out, size_t cap) {
    char sk[160]; size_t o = 0; sk[0] = 0; uint32_t seen[16]; size_t ns = 0;
    for (size_t i = 0; i < p->nsl && ns < 16; i++) {
        uint32_t k = p->sl[i]->key; size_t j = 0; for (; j < ns; j++) if (seen[j] == k) break;
        if (j < ns) continue;
        seen[ns++] = k; unsigned n = 0; for (size_t q = 0; q < p->nsl; q++) n += p->sl[q]->key == k;
        int w = snprintf(sk + o, sizeof sk - o, "%s%u:%u", o ? "," : "", (unsigned)k, n); if (w < 0 || (size_t)w >= sizeof sk - o) break; o += (size_t)w;
    }
    return snprintf(out, cap, "T1 pool ON slabs(key:n)=[%s] new=%llu released=%llu live_sub=%llu recycle hit/miss=%llu/%llu (cached %zu, %llu KiB) descpool reuse/create=%llu/%llu (free %zu) deferred_waiting=%zu kernel_allocs_avoided=%llu fence submitted/completed=%llu/%llu",
        sk, (unsigned long long)p->slabNew, (unsigned long long)p->slabRel, (unsigned long long)(p->subAlloc - p->subFree), (unsigned long long)p->recHit, (unsigned long long)p->recMiss,
        p->nrc, (unsigned long long)(p->recBytes >> 10), (unsigned long long)p->dpReuse, (unsigned long long)p->dpCreate, n48p_h_count(p), n48p_waiting(p), (unsigned long long)p->avoided,
        (unsigned long long)p->submitted, (unsigned long long)p->completed);
}
#endif
