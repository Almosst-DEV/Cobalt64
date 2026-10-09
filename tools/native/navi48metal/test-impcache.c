// test-impcache.c: n48_impcache.h (P4 import cache + classify cache decisions) on the host.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-impcache test-impcache.c && /tmp/test-impcache
#include <stdio.h>
#include "n48_impcache.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define MIB (1ULL << 20)
#define SEC 1000000000ULL
#define M(n) ((void *)(uintptr_t)(n))

int main(void) {
    n48ic_cache c; void *m; n48ic_rel o;

    // ---- 1. refcount: one import shared by two live textures -----------------------------------------------------------------
    n48ic_init(&c, 1);
    CHECK("miss on an empty cache", !n48ic_acquire(&c, 10, 0x1000, 8 * MIB, 0, 0, &m), "miss");
    n48ic_add(&c, 10, 0x1000, 8 * MIB, M(0xA1), M(0xB1));
    CHECK("second texture of the same surface hits and gets the same memory", n48ic_acquire(&c, 10, 0x1000, 8 * MIB, 1, 0, &m) && m == M(0xA1) && c.reused == 1 && c.created == 1, "reused %llu created %llu", (unsigned long long)c.reused, (unsigned long long)c.created);
    n48ic_unref(&c, M(0xA1), 2, 0);
    CHECK("one texture still alive: entry in use, nothing cached or released", c.cachedBytes == 0 && c.nrl == 0 && c.e[0].refs == 1, "refs %d cached %llu", c.e[0].refs, (unsigned long long)c.cachedBytes);
    n48ic_unref(&c, M(0xA1), 3, 0);
    CHECK("last texture gone: kept (unused) rather than released", c.cachedBytes == 8 * MIB && c.nrl == 0 && c.n == 1 && c.e[0].refs == 0, "cached %llu released %zu", (unsigned long long)c.cachedBytes, c.nrl);

    // ---- 2. 2 s expiry ---------------------------------------------------------------------------------------------------------
    n48ic_trim(&c, 3 + 2 * SEC, 0);
    CHECK("exactly 2 s idle is not yet expired", c.nrl == 0 && c.n == 1, "released %zu", c.nrl);
    CHECK("the next frame's texture within 2 s reuses it (and the entry leaves the unused set)", n48ic_acquire(&c, 10, 0x1000, 8 * MIB, 3 + SEC, 0, &m) && c.cachedBytes == 0 && c.reused == 2, "cached %llu", (unsigned long long)c.cachedBytes);
    n48ic_unref(&c, M(0xA1), 3 + SEC, 0);
    n48ic_trim(&c, 3 + SEC + 2 * SEC + 1, 0);
    CHECK("unused > 2 s after the LAST unref: released", c.nrl == 1 && c.n == 0 && c.cachedBytes == 0 && c.liveBytes == 0 && c.evIdle == 1, "released %zu idle %llu", c.nrl, (unsigned long long)c.evIdle);
    CHECK("the release carries the entry's memory and owner", n48ic_pop(&c, 0, &o) && o.mem == M(0xA1) && o.owner == M(0xB1) && o.size == 8 * MIB, "mem %p owner %p", o.mem, o.owner);
    CHECK("expired entry no longer matches (new import needed)", !n48ic_acquire(&c, 10, 0x1000, 8 * MIB, 4 * SEC, 0, &m), "miss");
    n48ic_destroy(&c);

    // ---- 3. byte cap and eviction order ------------------------------------------------------------------------------------------
    n48ic_init(&c, 1);
    for (int i = 0; i < 4; i++) { n48ic_add(&c, 100 + i, 0x10000 * (i + 1), 40 * MIB, M(0x100 + i), M(0x200 + i)); }   // 160 MiB live
    n48ic_unref(&c, M(0x100), 10, 0); n48ic_unref(&c, M(0x101), 20, 0); n48ic_unref(&c, M(0x102), 30, 0);   // 120 MiB cached: under the cap
    CHECK("120 MiB unused <= 128 MiB cap: nothing evicted", c.cachedBytes == 120 * MIB && c.nrl == 0, "cached %llu", (unsigned long long)c.cachedBytes);
    n48ic_unref(&c, M(0x103), 40, 0);   // 160 MiB > cap: oldest first
    CHECK("160 MiB unused > cap: oldest unused (0x100) evicted first, then stop under the cap", c.nrl == 1 && c.rl[0].mem == M(0x100) && c.cachedBytes == 120 * MIB && c.evCap == 1, "released %zu first %p cached %llu", c.nrl, c.rl[0].mem, (unsigned long long)c.cachedBytes);
    CHECK("the evicted one no longer hits, the others do", !n48ic_acquire(&c, 100, 0x10000, 40 * MIB, 41, 0, &m) && n48ic_acquire(&c, 101, 0x20000, 40 * MIB, 41, 0, &m) && m == M(0x101), "ok");
    n48ic_unref(&c, M(0x101), 42, 0);   // refreshed: now newest; unused = 102(30) 103(40) 101(42) = 120
    n48ic_add(&c, 104, 0x50000, 40 * MIB, M(0x104), M(0x204)); n48ic_unref(&c, M(0x104), 50, 0);   // 160 -> evict oldest = 0x102 (t30)
    CHECK("eviction follows lastUnref (0x102 at t30 goes before 0x103 at t40 and the refreshed 0x101)", c.nrl == 2 && c.rl[1].mem == M(0x102), "second released %p", c.rl[1].mem);
    n48ic_destroy(&c);
    n48ic_init(&c, 1);
    n48ic_add(&c, 1, 0x1000, 40 * MIB, M(1), M(1)); n48ic_add(&c, 2, 0x2000, 40 * MIB, M(2), M(2)); n48ic_add(&c, 3, 0x3000, 40 * MIB, M(3), M(3)); n48ic_add(&c, 4, 0x4000, 40 * MIB, M(4), M(4));
    n48ic_unref(&c, M(1), 1, 0); n48ic_unref(&c, M(2), 2, 0); n48ic_unref(&c, M(3), 3, 0); n48ic_unref(&c, M(4), 4, 0);
    CHECK("a LIVE entry is never evicted by the cap (only unused ones)", c.nrl == 1 && c.rl[0].mem == M(1) && c.liveBytes == 120 * MIB, "released %zu live %llu", c.nrl, (unsigned long long)c.liveBytes);
    n48ic_destroy(&c);

    // ---- 4. id reuse with another key ----------------------------------------------------------------------------------------------
    n48ic_init(&c, 1);
    n48ic_add(&c, 7, 0x1000, 8 * MIB, M(0x71), M(0x72)); n48ic_unref(&c, M(0x71), 5, 0);
    CHECK("same id, different size: unused entry released, miss", !n48ic_acquire(&c, 7, 0x1000, 16 * MIB, 6, 0, &m) && c.nrl == 1 && c.mismatches == 1 && c.n == 0, "released %zu mismatches %llu", c.nrl, (unsigned long long)c.mismatches);
    n48ic_add(&c, 7, 0x1000, 16 * MIB, M(0x81), M(0x82));   // live (refs 1)
    CHECK("same id, different base while the old is LIVE: old goes stale (still held), miss", !n48ic_acquire(&c, 7, 0x9000, 16 * MIB, 7, 0, &m) && c.n == 1 && c.e[0].stale && c.nrl == 1, "stale %d", c.e[0].stale);
    CHECK("a stale entry never matches even with its own key", !n48ic_acquire(&c, 7, 0x1000, 16 * MIB, 8, 0, &m), "miss");
    n48ic_unref(&c, M(0x81), 9, 0);
    CHECK("the stale entry is released the moment its last texture lets go (not cached)", c.n == 0 && c.nrl == 2 && c.cachedBytes == 0 && c.evStale == 2, "n %zu released %zu", c.n, c.nrl);
    n48ic_destroy(&c);

    // ---- 5. fence gating of the release queue ------------------------------------------------------------------------------------------
    n48ic_init(&c, 1);
    n48ic_add(&c, 5, 0x1000, 4 * MIB, M(0x51), M(0x52)); n48ic_unref(&c, M(0x51), 1, 0);
    n48ic_trim(&c, 1 + 3 * SEC, 7);   // given up while serial 7 was the newest submitted
    CHECK("a release stamped 7 is not handed out while completed is 6", c.nrl == 1 && !n48ic_pop(&c, 6, &o), "held back");
    CHECK("... and is handed out once completed reaches 7", n48ic_pop(&c, 7, &o) && o.mem == M(0x51), "popped");
    n48ic_destroy(&c);

    // ---- 6. flush (out of kernel imports) ------------------------------------------------------------------------------------------
    n48ic_init(&c, 1);
    n48ic_add(&c, 1, 0x1000, MIB, M(1), M(1)); n48ic_add(&c, 2, 0x2000, MIB, M(2), M(2)); n48ic_unref(&c, M(1), 1, 0);
    CHECK("flush releases the unused entry only", n48ic_flush(&c, 0) == 1 && c.nrl == 1 && c.n == 1 && c.e[0].mem == M(2) && c.evFlush == 1, "n %zu", c.n);
    n48ic_destroy(&c);

    // ---- 7. OFF path -------------------------------------------------------------------------------------------------------------------
    n48ic_init(&c, 0);
    n48ic_trim(&c, 100 * SEC, 0);
    CHECK("OFF: trim and flush are no-ops, nothing is ever queued", n48ic_flush(&c, 0) == 0 && c.nrl == 0 && c.n == 0 && c.on == 0, "off");
    n48ic_destroy(&c);

    // ---- 8. classify cache -------------------------------------------------------------------------------------------------------------
    static n48cc cc; int v = -1;
    memset(&cc, 0, sizeof cc);
    CHECK("classify: first sight misses", !n48cc_lookup(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, &v) && cc.misses == 1, "miss");
    n48cc_put(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, 1);
    n48cc_put(&cc, 51, 0x2000, 2560, 1440, 10240, 14745600, 0);
    CHECK("classify: positive verdict cached", n48cc_lookup(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, &v) && v == 1, "v %d", v);
    CHECK("classify: negative verdict cached", n48cc_lookup(&cc, 51, 0x2000, 2560, 1440, 10240, 14745600, &v) && v == 0, "v %d", v);
    CHECK("classify: hits counted", cc.hits == 2 && cc.misses == 1, "hits %llu misses %llu", (unsigned long long)cc.hits, (unsigned long long)cc.misses);
    CHECK("classify: same id, other size -> mismatch, miss, entry dropped", !n48cc_lookup(&cc, 50, 0x1000, 2560, 1600, 10240, 16384000, &v) && cc.mismatches == 1, "mismatch %llu", (unsigned long long)cc.mismatches);
    CHECK("classify: ... and the old key no longer hits (it was invalidated)", !n48cc_lookup(&cc, 50, 0x1000, 2560, 1440, 10240, 14745600, &v), "miss");
    n48cc_put(&cc, 50, 0x1000, 2560, 1600, 10240, 16384000, 0);
    CHECK("classify: same id, other base -> mismatch", !n48cc_lookup(&cc, 50, 0x7000, 2560, 1600, 10240, 16384000, &v) && cc.mismatches == 2, "mismatch %llu", (unsigned long long)cc.mismatches);
    CHECK("classify: same id, other pitch -> mismatch", (n48cc_put(&cc, 52, 0x1000, 1024, 768, 4096, 3145728, 1), !n48cc_lookup(&cc, 52, 0x1000, 1024, 768, 4352, 3145728, &v)) && cc.mismatches == 3, "mismatch %llu", (unsigned long long)cc.mismatches);
    memset(&cc, 0, sizeof cc);
    n48cc_put(&cc, 60, 0x1000, 2560, 1440, 10240, 14745600, 1);
    CHECK("classify: same id, only the alloc size differs -> mismatch", !n48cc_lookup(&cc, 60, 0x1000, 2560, 1440, 10240, 14745600 + 16384, &v) && cc.mismatches == 1, "mismatch %llu", (unsigned long long)cc.mismatches);
    memset(&cc, 0, sizeof cc);
    for (uint32_t i = 0; i < N48CC_N; i++) n48cc_put(&cc, 1000 + i, 0x1000, 1024, 768, 4096, 3145728, (int)(i & 1));
    n48cc_lookup(&cc, 1000, 0x1000, 1024, 768, 4096, 3145728, &v);   // id 1000 becomes the most recently used
    n48cc_put(&cc, 2000, 0x1000, 1024, 768, 4096, 3145728, 1);       // 65th id: evicts the least recently used = 1001
    CHECK("classify: bounded to 64 ids, LRU evicted (1001 gone, the touched 1000 and the new 2000 stay)",
          !n48cc_lookup(&cc, 1001, 0x1000, 1024, 768, 4096, 3145728, &v) && n48cc_lookup(&cc, 1000, 0x1000, 1024, 768, 4096, 3145728, &v) && n48cc_lookup(&cc, 2000, 0x1000, 1024, 768, 4096, 3145728, &v) && n48cc_lookup(&cc, 1063, 0x1000, 1024, 768, 4096, 3145728, &v), "ok");

    // ---- 4. build 16: F2 default, F1 decision, the failure-injection hook -------------------------------------------------------------
    CHECK("F2: the cache is ON by default (no kill file)", n48ic_default_on(0) == 1, "on");
    CHECK("F2: the kill file turns it off", n48ic_default_on(1) == 0, "off");
    {   n48ic_cache d; n48ic_init(&d, n48ic_default_on(0)); void *mm;
        n48ic_add(&d, 5, 0x1000, MIB, M(0xC1), M(0xD1));
        CHECK("F2: a default-initialised cache shares the import of one surface between two wrappers", d.on && n48ic_acquire(&d, 5, 0x1000, MIB, 1, 0, &mm) && mm == M(0xC1) && d.reused == 1, "on %d reused %llu", d.on, (unsigned long long)d.reused);
        n48ic_cache o; n48ic_init(&o, n48ic_default_on(1));
        CHECK("F2: with the kill file the cache is off (the glue then imports per wrapper as before) and trim/flush do nothing", o.on == 0 && n48ic_flush(&o, 0) == 0, "on %d", o.on);
    }
    CHECK("F1: an imported texture goes on", n48f1_action(0) == N48F1_GO, "go");
    CHECK("F1: ANY refused import falls back to the GPU-only image, never to nil", n48f1_action(-2) == N48F1_FALLBACK && n48f1_action(-1) == N48F1_FALLBACK && n48f1_action(-1000072003) == N48F1_FALLBACK, "fallback");
    { unsigned long c = 0; int hits = 0; for (int i = 0; i < 100; i++) hits += n48f1_inject(4, &c);
      CHECK("F1 hook: every 4th attempt fails (25 of 100), 0 = never", hits == 25 && (c = 0, n48f1_inject(0, &c) == 0 && c == 0), "hits %d", hits); }
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
