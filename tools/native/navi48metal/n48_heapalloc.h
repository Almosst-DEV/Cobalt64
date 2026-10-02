// n48_heapalloc.h: the offset allocator of N48Heap (Navi48Device.m) as plain C, so it can be tested without Vulkan (test-heapalloc.c, run on the host Mac against
// numbers measured on an Apple-silicon Mac by `mtlprobe heap`). First fit by offset; a block is (offset, requested size); the next block may start at the first
// `align`-aligned offset at or after the previous block's end; usedSize is the sum of the requested sizes (not rounded).
#ifndef N48_HEAPALLOC_H
#define N48_HEAPALLOC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint64_t off, size, id; } N48HeapBlk;
typedef struct { uint64_t size, used, ctr; N48HeapBlk *blk; size_t nblk, cap; } N48HeapAlloc;
static inline uint64_t n48ha_alup(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
static inline void n48ha_init(N48HeapAlloc *h, uint64_t size) { memset(h, 0, sizeof *h); h->size = size; }
static inline void n48ha_destroy(N48HeapAlloc *h) { free(h->blk); h->blk = NULL; h->nblk = h->cap = 0; }
// returns 1 and fills *off / *id, or 0 when no gap fits
static inline int n48ha_alloc(N48HeapAlloc *h, uint64_t sz, uint64_t al, uint64_t *off, uint64_t *id) {
    uint64_t prevEnd = 0, at = UINT64_MAX; size_t pos = 0;
    for (size_t i = 0; i <= h->nblk; i++) {
        uint64_t gapEnd = i < h->nblk ? h->blk[i].off : h->size, cand = n48ha_alup(prevEnd, al);
        if (cand <= gapEnd && gapEnd - cand >= sz) { at = cand; pos = i; break; }
        if (i < h->nblk) prevEnd = h->blk[i].off + h->blk[i].size;
    }
    if (at == UINT64_MAX) return 0;
    if (h->nblk == h->cap) { h->cap = h->cap ? h->cap * 2 : 64; h->blk = (N48HeapBlk *)realloc(h->blk, h->cap * sizeof *h->blk); }
    memmove(&h->blk[pos + 1], &h->blk[pos], (h->nblk - pos) * sizeof *h->blk);
    h->blk[pos] = (N48HeapBlk){ at, sz, ++h->ctr }; h->nblk++; h->used += sz; *off = at; *id = h->ctr; return 1;
}
// frees by block id (a second free of the same id is a no-op)
static inline void n48ha_free(N48HeapAlloc *h, uint64_t id) {
    for (size_t i = 0; i < h->nblk; i++) if (h->blk[i].id == id) { h->used -= h->blk[i].size; memmove(&h->blk[i], &h->blk[i + 1], (h->nblk - i - 1) * sizeof *h->blk); h->nblk--; return; }
}
static inline uint64_t n48ha_maxavail(const N48HeapAlloc *h, uint64_t al) {
    if (al < 1) al = 1;
    if (al & (al - 1)) { uint64_t p = 1; while (p < al) p <<= 1; al = p; }
    uint64_t best = 0, prevEnd = 0;
    for (size_t i = 0; i <= h->nblk; i++) {
        uint64_t gapEnd = i < h->nblk ? h->blk[i].off : h->size, cand = n48ha_alup(prevEnd, al);
        if (cand < gapEnd && gapEnd - cand > best) best = gapEnd - cand;
        if (i < h->nblk) prevEnd = h->blk[i].off + h->blk[i].size;
    }
    return best;
}
#endif
