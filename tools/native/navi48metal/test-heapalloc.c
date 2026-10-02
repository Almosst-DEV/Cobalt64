// test-heapalloc.c: the N48Heap offset allocator (n48_heapalloc.h) against the numbers `mtlprobe heap` measured on an Apple-silicon Mac (Apple's AGXG16GFamilyHeap.
// Build/run on the host Mac:  cc -O1 -o /tmp/test-heapalloc test-heapalloc.c && /tmp/test-heapalloc
#include <stdio.h>
#include "n48_heapalloc.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define MIB (1ULL << 20)
int main(void) {
    uint64_t off, id, id2, id3;
    // Apple-silicon: 64 MiB heap, one 1000-byte buffer (alignment 256): used 1000, max(256) 67107840, max(16384) 67092480
    N48HeapAlloc h; n48ha_init(&h, 64 * MIB);
    CHECK("empty", n48ha_maxavail(&h, 256) == 64 * MIB && n48ha_maxavail(&h, 16384) == 64 * MIB, "whole heap at any alignment");
    CHECK("alloc 1000", n48ha_alloc(&h, 1000, 256, &off, &id) && off == 0 && h.used == 1000, "offset %llu used %llu", (unsigned long long)off, (unsigned long long)h.used);
    CHECK("max after 1000", n48ha_maxavail(&h, 256) == 67107840 && n48ha_maxavail(&h, 16384) == 67092480, "%llu %llu (Apple-silicon: 67107840 67092480)", (unsigned long long)n48ha_maxavail(&h, 256), (unsigned long long)n48ha_maxavail(&h, 16384));
    // Apple-silicon: texture 264192 B placed after the buffer: used 265192; makeAliasable (free) -> used 1000
    CHECK("alloc texture", n48ha_alloc(&h, 264192, 128, &off, &id2) && off == 1024 && h.used == 265192, "offset %llu used %llu (Apple-silicon: used 265192)", (unsigned long long)off, (unsigned long long)h.used);
    n48ha_free(&h, id2); CHECK("free texture", h.used == 1000, "used %llu", (unsigned long long)h.used);
    n48ha_free(&h, id2); CHECK("double free is a no-op", h.used == 1000, "used %llu", (unsigned long long)h.used);
    n48ha_free(&h, id); CHECK("free buffer", h.used == 0 && n48ha_maxavail(&h, 256) == 64 * MIB, "used %llu", (unsigned long long)h.used);
    // Apple-silicon: larger than the heap fails; exact fit succeeds and leaves nothing; 16 more bytes fail
    CHECK("larger than the heap", !n48ha_alloc(&h, 64 * MIB + 1, 256, &off, &id), "refused");
    CHECK("exact fit", n48ha_alloc(&h, 64 * MIB, 256, &off, &id) && h.used == 64 * MIB && n48ha_maxavail(&h, 16) == 0, "used %llu", (unsigned long long)h.used);
    CHECK("full", !n48ha_alloc(&h, 16, 256, &off, &id2), "refused");
    n48ha_free(&h, id); CHECK("reuse", h.used == 0 && n48ha_alloc(&h, 4096, 256, &off, &id) , "ok"); n48ha_free(&h, id);
    // Apple-silicon fragmentation: a(1000) c(1000), free a: used 1000, max(256) 67106816; a (64M - 1000) request fails; 60 MiB fits and leaves 4192256
    n48ha_alloc(&h, 1000, 256, &off, &id); n48ha_alloc(&h, 1000, 256, &off, &id2);
    CHECK("c at 1024", off == 1024 && h.used == 2000 && n48ha_maxavail(&h, 256) == 67106816, "offset %llu max(256) %llu (Apple-silicon: 67106816)", (unsigned long long)off, (unsigned long long)n48ha_maxavail(&h, 256));
    n48ha_free(&h, id); CHECK("after freeing a", h.used == 1000 && n48ha_maxavail(&h, 256) == 67106816, "used %llu max(256) %llu (Apple-silicon: 1000 67106816)", (unsigned long long)h.used, (unsigned long long)n48ha_maxavail(&h, 256));
    CHECK("no gap for 64M-1000", !n48ha_alloc(&h, 64 * MIB - 1000, 256, &off, &id3), "refused (Apple-silicon: nil)");
    CHECK("60 MiB fits", n48ha_alloc(&h, 60 * MIB, 256, &off, &id3) && n48ha_maxavail(&h, 256) == 4192256, "max(256) after %llu (Apple-silicon: 4192256)", (unsigned long long)n48ha_maxavail(&h, 256));
    CHECK("a freed hole is reused", n48ha_alloc(&h, 1000, 256, &off, &id) && off == 0, "offset %llu", (unsigned long long)off);
    n48ha_destroy(&h);
    // Apple-silicon: a 1000 B (rounds to 16 KiB) heap, one 100-byte buffer: max(16) 16272, max(256) 16128, max(16384) = 0 left? (Apple-silicon printed 16272 and 16128 for 16 and 256)
    n48ha_init(&h, 16384); n48ha_alloc(&h, 100, 256, &off, &id);
    CHECK("16 KiB heap, 100 B", h.used == 100 && n48ha_maxavail(&h, 16) == 16272 && n48ha_maxavail(&h, 256) == 16128, "max(16) %llu max(256) %llu (Apple-silicon: 16272 16128)", (unsigned long long)n48ha_maxavail(&h, 16), (unsigned long long)n48ha_maxavail(&h, 256));
    n48ha_destroy(&h);
    // churn: 100000 random alloc/free cycles keep the block list sorted, non-overlapping, and the sums exact
    n48ha_init(&h, 64 * MIB); srand(7); uint64_t ids[32] = {0}; uint64_t sizes[32] = {0}; uint64_t sum = 0; int bad = 0;
    for (int i = 0; i < 100000; i++) {
        int k = rand() % 32;
        if (ids[k]) { n48ha_free(&h, ids[k]); sum -= sizes[k]; ids[k] = 0; }
        else { uint64_t sz = 1 + (uint64_t)rand() % 3000000, al = (uint64_t)256 << (rand() % 3); if (n48ha_alloc(&h, sz, al, &off, &ids[k])) { sizes[k] = sz; sum += sz; if (off % al) bad++; } }
        if (h.used != sum) bad++;
        for (size_t j = 1; j < h.nblk; j++) if (h.blk[j].off < h.blk[j - 1].off + h.blk[j - 1].size) bad++;
        if (h.nblk && h.blk[h.nblk - 1].off + h.blk[h.nblk - 1].size > h.size) bad++;
    }
    CHECK("churn", bad == 0, "100000 operations, %d invariant violations", bad);
    n48ha_destroy(&h);
    printf(fails ? "FAIL test-heapalloc (%d)\n" : "PASS test-heapalloc\n", fails);
    return fails ? 1 : 0;
}
