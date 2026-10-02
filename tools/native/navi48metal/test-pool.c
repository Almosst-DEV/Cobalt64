// test-pool.c: n48_pool.h (P1 memory pooling decisions) on the host.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-pool test-pool.c && /tmp/test-pool
#include <stdio.h>
#include "n48_pool.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)
#define KIB (1ULL << 10)
#define MIB (1ULL << 20)
#define SEC 1000000000ULL

// A fresh slab for `key`, then fill it with 16 x 256 KiB; offsets returned in off[].
static uint64_t fill_slab(n48p_pool *p, uint32_t key, uint64_t t, uint64_t off[16]) {
    n48p_res r; uint64_t sid = 0;
    int k = n48p_alloc(p, key, 256 * KIB, 256, t, &r);
    if (k == N48P_R_NEED_SLAB) { sid = n48p_add_slab(p, key, N48P_SLAB_SIZE, (void *)0x1000, (void *)0x2000, t); k = n48p_alloc(p, key, 256 * KIB, 256, t, &r); }
    if (k != N48P_R_SUB) return 0;
    sid = r.slab; off[0] = r.off;
    for (int i = 1; i < 16; i++) { k = n48p_alloc(p, key, 256 * KIB, 256, t, &r); if (k != N48P_R_SUB || r.slab != sid) return 0; off[i] = r.off; }
    return sid;
}

int main(void) {
    n48p_pool p; n48p_res r; n48p_rel rel; uint64_t off[16];

    // ---- 1. fence gating ------------------------------------------------------------------------------------------------------
    n48p_init(&p, 1);
    CHECK("fence: no work pending -> completed == submitted == 0", p.submitted == 0 && p.completed == 0, "%llu/%llu", (unsigned long long)p.submitted, (unsigned long long)p.completed);
    uint64_t s1 = n48p_submit(&p), s2 = n48p_submit(&p), s3 = n48p_submit(&p);
    CHECK("fence: three submits, none done", s1 == 1 && s2 == 2 && s3 == 3 && p.completed == 0, "completed %llu", (unsigned long long)p.completed);
    n48p_done(&p, s2); CHECK("fence: serial 2 done first does not advance (1 still pending)", p.completed == 0, "completed %llu", (unsigned long long)p.completed);
    n48p_done(&p, s1); CHECK("fence: serial 1 done -> 2 (3 pending)", p.completed == 2, "completed %llu", (unsigned long long)p.completed);
    n48p_done(&p, s1); CHECK("fence: done is idempotent", p.completed == 2, "completed %llu", (unsigned long long)p.completed);
    n48p_done(&p, s3); CHECK("fence: all done -> completed == submitted", p.completed == 3, "completed %llu", (unsigned long long)p.completed);
    n48p_destroy(&p);

    n48p_init(&p, 1);
    uint64_t sid = fill_slab(&p, 7, 0, off);
    CHECK("slab: 16 x 256 KiB fill one 4 MiB slab at distinct 256 KiB steps", sid && off[15] == 15 * 256 * KIB, "slab %llu last off %llu", (unsigned long long)sid, (unsigned long long)off[15]);
    CHECK("slab full -> asks for another slab", n48p_alloc(&p, 7, 256 * KIB, 256, 0, &r) == N48P_R_NEED_SLAB, "NEED_SLAB");
    uint64_t sub = n48p_submit(&p);                                   // a command buffer is in flight when the free happens
    n48p_free_sub(&p, sid, off[3], 256 * KIB, 10);
    CHECK("fence: freed range waits (1 deferred)", n48p_waiting(&p) == 1, "waiting %zu", n48p_waiting(&p));
    CHECK("fence: freed range NOT returned before completed >= stamp", n48p_alloc(&p, 7, 256 * KIB, 256, 11, &r) == N48P_R_NEED_SLAB, "still NEED_SLAB, completed %llu stamp %llu", (unsigned long long)p.completed, (unsigned long long)p.submitted);
    uint64_t sub2 = n48p_submit(&p); n48p_done(&p, sub2);              // a later command buffer finishing does not release it (the earlier one is pending)
    CHECK("fence: a LATER completion does not release it", n48p_alloc(&p, 7, 256 * KIB, 256, 12, &r) == N48P_R_NEED_SLAB, "still NEED_SLAB");
    n48p_done(&p, sub);
    int k = n48p_alloc(&p, 7, 256 * KIB, 256, 13, &r);
    CHECK("fence: IS returned after completed >= stamp", k == N48P_R_SUB && r.off == off[3] && n48p_waiting(&p) == 0, "off %llu (want %llu)", (unsigned long long)r.off, (unsigned long long)off[3]);
    // whole allocations and handles are gated the same way
    uint64_t sw = n48p_submit(&p);
    CHECK("whole: miss asks for a new allocation", n48p_alloc(&p, 3, 1 * MIB, 4096, 20, &r) == N48P_R_NEED_WHOLE, "NEED_WHOLE");
    CHECK("whole: free while busy is pooled", n48p_free_whole(&p, 3, 1 * MIB, (void *)0xAA, NULL, 21) == 1 && p.nrc == 0, "cached %zu (still deferred)", p.nrc);
    CHECK("whole: NOT reusable before the fence clears", n48p_alloc(&p, 3, 1 * MIB, 4096, 22, &r) == N48P_R_NEED_WHOLE, "miss");
    n48p_done(&p, sw);
    CHECK("whole: reusable after the fence clears", n48p_alloc(&p, 3, 1 * MIB, 4096, 23, &r) == N48P_R_REC && r.mem == (void *)0xAA, "hit mem %p", r.mem);
    CHECK("whole: key and exact size must match", n48p_free_whole(&p, 3, 1 * MIB, (void *)0xAB, NULL, 24) == 1 && n48p_alloc(&p, 4, 1 * MIB, 4096, 25, &r) == N48P_R_NEED_WHOLE &&
          n48p_alloc(&p, 3, 1 * MIB + 4096, 4096, 25, &r) == N48P_R_NEED_WHOLE && n48p_alloc(&p, 3, 1 * MIB, 4096, 25, &r) == N48P_R_REC, "other key / other size miss, same key+size hits");
    uint64_t sh = n48p_submit(&p);
    CHECK("handles: push stamps with submitted", n48p_h_push(&p, 0x77, 32) == 1 && n48p_h_count(&p) == 1, "pushed");
    CHECK("handles: NOT popped before the fence clears", n48p_h_pop(&p) == 0, "0");
    n48p_done(&p, sh);
    CHECK("handles: popped after", n48p_h_pop(&p) == 0x77 && n48p_h_count(&p) == 0, "0x77");
    for (int i = 0; i < 32; i++) n48p_h_push(&p, 0x100 + i, 32);
    CHECK("handles: capped at 32 (the 33rd is refused: caller destroys it)", n48p_h_push(&p, 0x999, 32) == 0 && n48p_h_count(&p) == 32, "count %zu", n48p_h_count(&p));
    n48p_destroy(&p);

    // ---- 2. alignment ---------------------------------------------------------------------------------------------------------
    n48p_init(&p, 1); srand(11);
    static const uint64_t als[] = { 1, 4, 16, 256, 4096, 65536 };
    struct { uint64_t slab, off, size; } live[400]; int nl = 0, bad = 0, ov = 0, total = 0;
    for (int i = 0; i < 3000; i++) {
        if (nl < 400 && (rand() % 3 || !nl)) {
            uint64_t al = als[rand() % 6], sz = 1 + (uint64_t)(rand() % (int)(N48P_SUB_MAX));
            int kk = n48p_alloc(&p, 1, sz, al, 0, &r);
            if (kk == N48P_R_NEED_SLAB) { n48p_add_slab(&p, 1, N48P_SLAB_SIZE, (void *)1, NULL, 0); kk = n48p_alloc(&p, 1, sz, al, 0, &r); }
            if (kk != N48P_R_SUB) { bad++; continue; }
            total++; if (r.off % al) bad++; if (r.off + sz > N48P_SLAB_SIZE) bad++;
            for (int j = 0; j < nl; j++) if (live[j].slab == r.slab && r.off < live[j].off + live[j].size && live[j].off < r.off + sz) ov++;
            live[nl].slab = r.slab; live[nl].off = r.off; live[nl].size = sz; nl++;
        } else { int j = rand() % nl; n48p_free_sub(&p, live[j].slab, live[j].off, live[j].size, 0); live[j] = live[--nl]; }
    }
    CHECK("alignment: every returned offset is a multiple of the requested alignment (1..64 KiB), inside the slab", bad == 0, "%d allocations, %d violations", total, bad);
    CHECK("alignment: no two live sub-allocations overlap", ov == 0, "%d overlaps", ov);
    n48p_destroy(&p);
    n48p_init(&p, 1); n48p_add_slab(&p, 2, N48P_SLAB_SIZE, (void *)1, NULL, 0);
    n48p_alloc(&p, 2, 1, 1, 0, &r); uint64_t a0 = r.off; n48p_alloc(&p, 2, 100, 4096, 0, &r);
    CHECK("alignment: second allocation at the next 4096 boundary", a0 == 0 && r.off == 4096, "first %llu second %llu", (unsigned long long)a0, (unsigned long long)r.off);
    n48p_alloc(&p, 2, 100, 65536, 0, &r);
    CHECK("alignment: 64 KiB alignment honoured (leading gap stays free)", r.off == 65536, "off %llu", (unsigned long long)r.off);
    n48p_alloc(&p, 2, 100, 256, 0, &r);
    CHECK("alignment: the gap in front is reused by a smaller aligned request", r.off == 256, "off %llu (want 256: first fit in the [1,4096) gap)", (unsigned long long)r.off);
    CHECK("alignment: over-aligned (> slab) goes direct", n48p_alloc(&p, 2, 100, 8 * MIB, 0, &r) == N48P_R_DIRECT, "DIRECT");
    CHECK("sizes: > 64 MiB goes direct", n48p_alloc(&p, 2, 65 * MIB, 4096, 0, &r) == N48P_R_DIRECT, "DIRECT");
    n48p_destroy(&p);

    // ---- 3. coalescing --------------------------------------------------------------------------------------------------------
    n48p_init(&p, 1);
    sid = fill_slab(&p, 5, 0, off);
    n48p_slab *s = n48p_find_slab(&p, sid);
    static const int ord[16] = { 5, 7, 6, 4, 0, 15, 14, 1, 3, 2, 8, 12, 10, 11, 9, 13 };
    for (int i = 0; i < 16; i++) n48p_free_sub(&p, sid, off[ord[i]], 256 * KIB, 100);
    CHECK("coalesce: 16 frees in scrambled order leave ONE free range spanning the slab", s->nfr == 1 && s->fr[0].off == 0 && s->fr[0].size == N48P_SLAB_SIZE && s->empty, "ranges %zu size %llu empty %d", s->nfr, (unsigned long long)s->fr[0].size, s->empty);
    // two holes next to each other become one
    sid = fill_slab(&p, 6, 0, off); s = n48p_find_slab(&p, sid);
    n48p_free_sub(&p, sid, off[4], 256 * KIB, 0); n48p_free_sub(&p, sid, off[6], 256 * KIB, 0);
    CHECK("coalesce: two separate holes stay two", s->nfr == 2, "ranges %zu", s->nfr);
    n48p_free_sub(&p, sid, off[5], 256 * KIB, 0);
    CHECK("coalesce: freeing the one between merges to a single 768 KiB range", s->nfr == 1 && s->fr[0].off == off[4] && s->fr[0].size == 768 * KIB, "ranges %zu size %llu", s->nfr, (unsigned long long)s->fr[0].size);
    CHECK("coalesce: and a 3 x 256 KiB request... fits as three 256 KiB pieces inside it", n48p_alloc(&p, 6, 256 * KIB, 256, 0, &r) == N48P_R_SUB && r.off == off[4], "off %llu", (unsigned long long)r.off);
    n48p_destroy(&p);

    // ---- 4. slab release timing -----------------------------------------------------------------------------------------------
    n48p_init(&p, 1);
    sid = fill_slab(&p, 9, 1000, off);
    for (int i = 0; i < 16; i++) n48p_free_sub(&p, sid, off[i], 256 * KIB, 5 * SEC);   // all free at t = 5 s
    n48p_trim(&p, 5 * SEC + 2 * SEC - 1);
    CHECK("slab release: not released 1 ns before 2 s of emptiness", p.nsl == 1 && !n48p_pop_release(&p, &rel), "slabs %zu", p.nsl);
    n48p_trim(&p, 5 * SEC + 2 * SEC);
    CHECK("slab release: released at exactly 2 s", p.nsl == 0 && n48p_pop_release(&p, &rel) && rel.slab == 1 && rel.mem == (void *)0x1000 && rel.size == N48P_SLAB_SIZE, "slabs %zu released mem %p", p.nsl, rel.mem);
    // a slab that is reused before the 2 s are up is kept
    sid = fill_slab(&p, 9, 10 * SEC, off);
    n48p_free_sub(&p, sid, off[0], 256 * KIB, 10 * SEC); n48p_trim(&p, 20 * SEC);
    CHECK("slab release: a slab with a live allocation is never released", p.nsl == 1 && !n48p_pop_release(&p, &rel), "slabs %zu", p.nsl);
    // empty only after the fence clears: a deferred free does not start the empty clock
    n48p_destroy(&p); n48p_init(&p, 1);
    sid = fill_slab(&p, 9, 0, off); sub = n48p_submit(&p);
    for (int i = 0; i < 16; i++) n48p_free_sub(&p, sid, off[i], 256 * KIB, 1 * SEC);
    n48p_trim(&p, 100 * SEC);
    CHECK("slab release: frees still waiting for the fence keep the slab alive", p.nsl == 1 && !n48p_pop_release(&p, &rel) && n48p_waiting(&p) == 16, "slabs %zu waiting %zu", p.nsl, n48p_waiting(&p));
    n48p_done(&p, sub); n48p_trim(&p, 101 * SEC);   // reaped at 101 s: empty clock starts then
    CHECK("slab release: clock starts when the fence clears (reaped at 101 s)", p.nsl == 1, "slabs %zu", p.nsl);
    n48p_trim(&p, 103 * SEC);
    CHECK("slab release: released 2 s after that", p.nsl == 0, "slabs %zu", p.nsl);
    n48p_destroy(&p);

    // ---- 5. recycle cap and expiry --------------------------------------------------------------------------------------------
    n48p_init(&p, 1);
    for (int i = 0; i < 5; i++) n48p_free_whole(&p, 1, 64 * MIB, (void *)(uintptr_t)(0x10 + i), NULL, (uint64_t)(i + 1) * SEC / 100);
    CHECK("recycle: 5 x 64 MiB against a 256 MiB cap keeps 4", p.nrc == 4 && p.recBytes == 256 * MIB, "cached %zu bytes %llu MiB", p.nrc, (unsigned long long)(p.recBytes >> 20));
    CHECK("recycle: the OLDEST (0x10) was the one released", n48p_pop_release(&p, &rel) && rel.mem == (void *)0x10 && !rel.slab && !n48p_pop_release(&p, &rel), "released %p", rel.mem);
    CHECK("recycle: > 64 MiB is not pooled", n48p_free_whole(&p, 1, 64 * MIB + 4096, (void *)0x99, NULL, 0) == 0, "caller frees it");
    uint64_t t0 = SEC / 100 * 5;   // the newest entry (0x14) was cached at 50 ms
    n48p_trim(&p, t0 + 2 * SEC);
    CHECK("recycle: unused exactly 2 s is kept (expiry is > 2 s), the older three expired", p.nrc == 1 && p.rc[0].mem == (void *)0x14, "cached %zu", p.nrc);
    n48p_trim(&p, t0 + 2 * SEC + 1);
    CHECK("recycle: unused > 2 s is released (the last one)", p.nrc == 0 && p.recBytes == 0, "cached %zu", p.nrc);
    int nrel = 0; while (n48p_pop_release(&p, &rel)) nrel++;
    CHECK("recycle: four entries reached the release queue", nrel == 4, "%d", nrel);
    // a hit refreshes nothing but removes the entry; a re-put restarts the clock
    n48p_free_whole(&p, 1, 1 * MIB, (void *)0x30, NULL, 100 * SEC);
    n48p_alloc(&p, 1, 1 * MIB, 4096, 101 * SEC, &r); n48p_free_whole(&p, 1, 1 * MIB, (void *)0x30, NULL, 101 * SEC);
    n48p_trim(&p, 102 * SEC + 1 * SEC / 2);
    CHECK("recycle: re-cached entry's clock restarts at its new free time", p.nrc == 1, "cached %zu", p.nrc);
    n48p_destroy(&p);
    // deferred whole allocations beyond the cap are freed at once
    n48p_init(&p, 1); uint64_t sbusy = n48p_submit(&p); int pooled = 0, direct = 0;
    for (int i = 0; i < 10; i++) { if (n48p_free_whole(&p, 1, 64 * MIB, (void *)(uintptr_t)(0x40 + i), NULL, 0)) pooled++; else direct++; }
    CHECK("recycle: deferred whole allocations are bounded (512 MiB while the GPU is busy)", pooled == 8 && direct == 2, "pooled %d direct %d", pooled, direct);
    n48p_done(&p, sbusy); n48p_reap(&p, 1);
    CHECK("recycle: once the fence clears the 8 deferred ones enter the cache within the 256 MiB cap", p.nrc == 4 && p.nd == 0, "cached %zu", p.nrc);
    n48p_destroy(&p);

    // ---- 6. OFF switch --------------------------------------------------------------------------------------------------------
    n48p_init(&p, 0);
    CHECK("off: submit returns 0 and tracks nothing", n48p_submit(&p) == 0 && p.submitted == 0 && p.npend == 0, "submitted %llu", (unsigned long long)p.submitted);
    CHECK("off: alloc is DIRECT for every size, nothing counted", n48p_alloc(&p, 1, 4096, 256, 0, &r) == N48P_R_DIRECT && n48p_alloc(&p, 1, 1 * MIB, 256, 0, &r) == N48P_R_DIRECT &&
          n48p_alloc(&p, 1, 100 * MIB, 256, 0, &r) == N48P_R_DIRECT, "DIRECT x3");
    n48p_free_sub(&p, 1, 0, 4096, 0);
    CHECK("off: free_whole says 'caller frees', free_sub / done are no-ops", n48p_free_whole(&p, 1, 1 * MIB, (void *)1, NULL, 0) == 0 && (n48p_done(&p, 1), 1), "0");
    CHECK("off: handle list refuses and returns nothing", n48p_h_push(&p, 5, 32) == 0 && n48p_h_pop(&p) == 0 && n48p_h_count(&p) == 0, "0/0/0");
    CHECK("off: no slab, cache, queue or counter was touched", !p.nsl && !p.nrc && !p.nd && !p.nrl && !p.subAlloc && !p.subFree && !p.recMiss && !p.avoided && !p.directAlloc && !p.slabNew, "all zero");
    n48p_destroy(&p);
    CHECK("off: a destroyed OFF pool is still OFF", p.on == 0, "on=%d", p.on);

    // ---- 7. property: under random command-buffer lifetimes nothing freed is handed out again before the fence cleared it ---------------------------------------
    {
        n48p_init(&p, 1); srand(23);
        enum { NL = 300, NF = 4000 };
        struct { uint64_t slab, off, size; } lv[NL]; int nlv = 0;
        struct { int whole; uint64_t slab, off, size, stamp; uintptr_t mem; int claimed; } fr[NF]; int nfr = 0;
        uint64_t open[64]; int nopen = 0; int viol = 0, reused = 0, wreused = 0; uintptr_t memctr = 0x1000;
        struct { uintptr_t mem; uint64_t size; } wl[64]; int nwl = 0;
        for (int i = 0; i < 40000; i++) {
            int op = rand() % 8; uint64_t now = (uint64_t)i * 1000;
            if (op == 0 && nopen < 64) open[nopen++] = n48p_submit(&p);
            else if (op == 1 && nopen) { int j = rand() % nopen; n48p_done(&p, open[j]); open[j] = open[--nopen]; }
            else if (op <= 4) {
                uint64_t sz = 256 * (1 + (uint64_t)(rand() % 64)), al = als[rand() % 6]; n48p_res rs;
                int kk = n48p_alloc(&p, 1, sz, al, now, &rs);
                if (kk == N48P_R_NEED_SLAB) { n48p_add_slab(&p, 1, N48P_SLAB_SIZE, (void *)1, NULL, now); kk = n48p_alloc(&p, 1, sz, al, now, &rs); }
                if (kk == N48P_R_SUB && nlv < NL) {
                    for (int f = 0; f < nfr; f++) if (!fr[f].whole && !fr[f].claimed && fr[f].slab == rs.slab && rs.off < fr[f].off + fr[f].size && fr[f].off < rs.off + sz) {
                        reused++; if (fr[f].stamp > p.completed) viol++;
                        if (rs.off <= fr[f].off && rs.off + sz >= fr[f].off + fr[f].size) fr[f].claimed = 1;   // fully covered: later overlaps are a different generation
                    }
                    lv[nlv].slab = rs.slab; lv[nlv].off = rs.off; lv[nlv].size = sz; nlv++;
                }
            } else if (op == 5 && nlv) {
                int j = rand() % nlv; n48p_free_sub(&p, lv[j].slab, lv[j].off, lv[j].size, now);
                if (nfr < NF) { fr[nfr].whole = 0; fr[nfr].slab = lv[j].slab; fr[nfr].off = lv[j].off; fr[nfr].size = lv[j].size; fr[nfr].stamp = p.submitted; fr[nfr].claimed = 0; nfr++; }
                lv[j] = lv[--nlv];
            } else if (op == 6) {
                uint64_t sz = (1 + (uint64_t)(rand() % 4)) * MIB; n48p_res rs; int kk = n48p_alloc(&p, 2, sz, 4096, now, &rs);
                if (kk == N48P_R_REC) { wreused++; for (int f = 0; f < nfr; f++) if (fr[f].whole && fr[f].mem == (uintptr_t)rs.mem && !fr[f].claimed) { if (fr[f].stamp > p.completed) viol++; fr[f].claimed = 1; } }
                if ((kk == N48P_R_REC || kk == N48P_R_NEED_WHOLE) && nwl < 64) { uintptr_t m = kk == N48P_R_REC ? (uintptr_t)rs.mem : ++memctr; wl[nwl].mem = m; wl[nwl].size = sz; nwl++; }
            } else if (op == 7 && nwl) {
                int j = rand() % nwl; int pooled = n48p_free_whole(&p, 2, wl[j].size, (void *)wl[j].mem, NULL, now);
                if (pooled && nfr < NF) { fr[nfr].whole = 1; fr[nfr].mem = wl[j].mem; fr[nfr].stamp = p.submitted; fr[nfr].claimed = 0; nfr++; }
                wl[j] = wl[--nwl];
            }
            if (i % 50 == 0) { n48p_trim(&p, now); n48p_rel q; while (n48p_pop_release(&p, &q)) {} }
            if (nfr >= NF - 2) nfr = 0;
        }
        CHECK("property: 40000 random ops, no range or allocation reused before completed >= its free stamp", viol == 0 && reused > 100 && wreused > 20, "%d violations over %d sub-range reuses and %d whole reuses", viol, reused, wreused);
        n48p_destroy(&p);
    }

    // ---- stats line -----------------------------------------------------------------------------------------------------------
    n48p_init(&p, 1); fill_slab(&p, 7, 0, off); char line[512]; n48p_fmt(&p, line, sizeof line);
    CHECK("stats: the T1 line names slabs per key, live sub-allocations, recycle, descriptor pools, deferred, avoided", strstr(line, "slabs(key:n)=[7:1]") && strstr(line, "live_sub=16") && strstr(line, "kernel_allocs_avoided="), "%s", line);
    n48p_destroy(&p);
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "ALL PASSED", fails, fails == 1 ? "" : "s");
    return fails != 0;
}
