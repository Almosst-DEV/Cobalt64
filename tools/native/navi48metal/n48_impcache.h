// n48_impcache.h: P4 decisions as plain C (no Vulkan, no Foundation; host test: test-impcache.c). Navi48Device.m does the Vulkan / IOSurface calls and holds ONE mutex
// around every call into this header. Time is a parameter (ns) so the tests control it.
//
//  (A) IMPORT CACHE.  One host import (a VkDeviceMemory over the whole IOSurface allocation) is shared by all live textures of the same IOSurface. Key: (IOSurface id,
//      base address, import size). Each entry is refcounted by textures. When the last texture lets go the entry stays cached (unused) for N48IC_IDLE_NS, and the cache
//      keeps at most N48IC_CAP bytes of UNUSED entries (oldest unused evicted first). An entry whose id comes back with another base or size is marked stale: it can no
//      longer be matched, and is released as soon as its last texture lets go. Memory the cache gives up goes to a release queue; every item carries a fence stamp
//      (the P1 `submitted` serial at the moment it was given up, 0 when the pool is OFF) and n48ic_pop hands it out only when `completed` >= stamp. The cache never
//      calls Vulkan: the caller drains the queue (vkFreeMemory + CFRelease of the entry's owner) outside its mutex.
//  (B) CLASSIFY CACHE.  The display-surface verdict per IOSurface id (positive and negative), key (id, base, w, h, bpr, alloc), 64 entries, LRU. A lookup with the same id
//      but another key invalidates the entry (counted as a mismatch) and misses.
#ifndef N48_IMPCACHE_H
#define N48_IMPCACHE_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// ---- build 16 (app-fix round 1, P5) -------------------------------------------------------------------------------------------------
// F2: the import cache is ON by default (one import per IOSurface instead of one per texture wrapper). The file /private/tmp/n48m-noimpcache is a KILL file (killedStat = it exists); the old presence
// file /private/tmp/n48m-impcache is no longer read. n48ic_default_on is what Navi48Device.m's latch calls.
static inline int n48ic_default_on(int killedStat) { return killedStat ? 0 : 1; }
// F1: what a texture does when the import of its IOSurface is refused (out of the kernel's import budget, a bad range): NEVER return nil (SkyLight aborts on a nil texture: AbortWithTextureInfo).
// 0 = imported, go on; 1 = fall back to the GPU-only image (the "baseless" path: the captured content is blank, the process stays up). Applies to every process that gets an import failure.
#define N48F1_GO 0
#define N48F1_FALLBACK 1
static inline int n48f1_action(int vkResult) { return vkResult == 0 ? N48F1_GO : N48F1_FALLBACK; }
// Test hook N48M_TEST_IMPORT_FAIL_EVERY=<n> (root + N48M_ALLOW=1 only): every n-th import attempt fails as if the kernel refused it. every == 0: never. *ctr counts attempts.
static inline int n48f1_inject(unsigned every, unsigned long *ctr) { if (!every) return 0; return (++*ctr % every) == 0; }

#define N48IC_IDLE_NS  2000000000ULL
#define N48IC_CAP      (128ULL << 20)

typedef struct { uint32_t id; uint64_t base, size; void *mem, *owner; int refs, stale; uint64_t lastUnref; } n48ic_ent;
typedef struct { void *mem, *owner; uint64_t size, stamp; } n48ic_rel;
typedef struct {
    int on; n48ic_ent *e; size_t n, c; uint64_t cachedBytes, liveBytes;   // cachedBytes = unused entries; liveBytes = every entry (what the kernel holds for us)
    n48ic_rel *rl; size_t nrl, crl;
    uint64_t created, reused, evIdle, evCap, evStale, evFlush, mismatches;
} n48ic_cache;

static inline void n48ic_init(n48ic_cache *c, int on) { memset(c, 0, sizeof *c); c->on = on; }
static inline void n48ic_destroy(n48ic_cache *c) { free(c->e); free(c->rl); { int on = c->on; memset(c, 0, sizeof *c); c->on = on; } }
static inline void n48ic_release_ent(n48ic_cache *c, size_t i, uint64_t stamp) {   // moves entry i to the release queue
    if (c->nrl == c->crl) { c->crl = c->crl ? c->crl * 2 : 16; c->rl = (n48ic_rel *)realloc(c->rl, c->crl * sizeof *c->rl); }
    c->rl[c->nrl++] = (n48ic_rel){ c->e[i].mem, c->e[i].owner, c->e[i].size, stamp };
    c->liveBytes -= c->e[i].size;
    memmove(&c->e[i], &c->e[i + 1], (c->n - i - 1) * sizeof *c->e); c->n--;
}
// Unused entries unused for > N48IC_IDLE_NS go to the release queue.
static inline void n48ic_trim(n48ic_cache *c, uint64_t now, uint64_t stamp) {
    if (!c->on) return;
    for (size_t i = 0; i < c->n;) {
        if (c->e[i].refs == 0 && !c->e[i].stale && now > c->e[i].lastUnref && now - c->e[i].lastUnref > N48IC_IDLE_NS) { c->cachedBytes -= c->e[i].size; c->evIdle++; n48ic_release_ent(c, i, stamp); }
        else i++;
    }
}
// Every unused entry (out of kernel imports): returns how many were queued.
static inline size_t n48ic_flush(n48ic_cache *c, uint64_t stamp) {
    size_t k = 0;
    if (!c->on) return 0;
    for (size_t i = 0; i < c->n;) { if (c->e[i].refs == 0 && !c->e[i].stale) { c->cachedBytes -= c->e[i].size; c->evFlush++; k++; n48ic_release_ent(c, i, stamp); } else i++; }
    return k;
}
// 1 = hit (*mem is the shared import, one more reference taken). 0 = miss (the caller imports and calls n48ic_add). Entries of the same id with another base/size become stale.
static inline int n48ic_acquire(n48ic_cache *c, uint32_t id, uint64_t base, uint64_t size, uint64_t now, uint64_t stamp, void **mem) {
    n48ic_trim(c, now, stamp);
    int hit = 0;
    for (size_t i = 0; i < c->n;) {
        n48ic_ent *e = &c->e[i];
        if (e->id != id || e->stale) { i++; continue; }
        if (e->base == base && e->size == size) {
            if (e->refs == 0) c->cachedBytes -= e->size;
            e->refs++; *mem = e->mem; c->reused++; hit = 1; i++; continue;
        }
        c->mismatches++;
        if (e->refs == 0) { c->cachedBytes -= e->size; c->evStale++; n48ic_release_ent(c, i, stamp); }
        else { e->stale = 1; i++; }
    }
    return hit;
}
// A fresh import, one reference (the texture that made it).
static inline void n48ic_add(n48ic_cache *c, uint32_t id, uint64_t base, uint64_t size, void *mem, void *owner) {
    if (c->n == c->c) { c->c = c->c ? c->c * 2 : 16; c->e = (n48ic_ent *)realloc(c->e, c->c * sizeof *c->e); }
    c->e[c->n++] = (n48ic_ent){ id, base, size, mem, owner, 1, 0, 0 };
    c->liveBytes += size; c->created++;
}
// A texture lets go of `mem`. Returns 1 if the entry was found.
static inline int n48ic_unref(n48ic_cache *c, void *mem, uint64_t now, uint64_t stamp) {
    for (size_t i = 0; i < c->n; i++) if (c->e[i].mem == mem) {
        if (c->e[i].refs > 0) c->e[i].refs--;
        if (c->e[i].refs == 0) {
            if (c->e[i].stale) { c->evStale++; n48ic_release_ent(c, i, stamp); }
            else {
                c->e[i].lastUnref = now; c->cachedBytes += c->e[i].size;
                while (c->cachedBytes > N48IC_CAP) {   // oldest unused first
                    size_t o = c->n; for (size_t k = 0; k < c->n; k++) if (c->e[k].refs == 0 && !c->e[k].stale && (o == c->n || c->e[k].lastUnref < c->e[o].lastUnref)) o = k;
                    if (o == c->n) break;
                    c->cachedBytes -= c->e[o].size; c->evCap++; n48ic_release_ent(c, o, stamp);
                }
            }
        }
        return 1;
    }
    return 0;
}
// Pops one released import whose fence stamp the pool has cleared. 1 and *o, or 0.
static inline int n48ic_pop(n48ic_cache *c, uint64_t completed, n48ic_rel *o) {
    for (size_t i = 0; i < c->nrl; i++) if (c->rl[i].stamp <= completed) {
        *o = c->rl[i]; memmove(&c->rl[i], &c->rl[i + 1], (c->nrl - i - 1) * sizeof *c->rl); c->nrl--; return 1;
    }
    return 0;
}
static inline size_t n48ic_unused(const n48ic_cache *c) { size_t k = 0; for (size_t i = 0; i < c->n; i++) k += c->e[i].refs == 0 && !c->e[i].stale; return k; }

// ---- (B) classify cache -------------------------------------------------------------------------------------------------------
#define N48CC_N 64
typedef struct { int valid, verdict; uint32_t id; uint64_t base, w, h, bpr, alloc, use; } n48cc_ent;
typedef struct { n48cc_ent e[N48CC_N]; uint64_t tick, hits, misses, mismatches; } n48cc;
// 1 = hit (*verdict). 0 = miss (same id with another key: the entry is dropped and counted as a mismatch).
static inline int n48cc_lookup(n48cc *c, uint32_t id, uint64_t base, uint64_t w, uint64_t h, uint64_t bpr, uint64_t alloc, int *verdict) {
    for (int i = 0; i < N48CC_N; i++) if (c->e[i].valid && c->e[i].id == id) {
        n48cc_ent *e = &c->e[i];
        if (e->base == base && e->w == w && e->h == h && e->bpr == bpr && e->alloc == alloc) { e->use = ++c->tick; *verdict = e->verdict; c->hits++; return 1; }
        e->valid = 0; c->mismatches++;
    }
    c->misses++; return 0;
}
static inline void n48cc_put(n48cc *c, uint32_t id, uint64_t base, uint64_t w, uint64_t h, uint64_t bpr, uint64_t alloc, int verdict) {
    int v = -1;
    for (int i = 0; i < N48CC_N; i++) if (!c->e[i].valid) { v = i; break; }
    if (v < 0) { v = 0; for (int i = 1; i < N48CC_N; i++) if (c->e[i].use < c->e[v].use) v = i; }
    c->e[v] = (n48cc_ent){ 1, verdict, id, base, w, h, bpr, alloc, ++c->tick };
}
#endif
