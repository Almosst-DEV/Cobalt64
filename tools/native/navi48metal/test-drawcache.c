// test-drawcache.c: n48_drawcache.h (P5b per-draw redundancy decisions) on the host.
// Build/run on the host Mac:  cc -O1 -Wall -Wextra -Werror -o /tmp/test-drawcache test-drawcache.c && /tmp/test-drawcache
#include <stdio.h>
#include <stdlib.h>
#include "n48_drawcache.h"
static int fails;
#define CHECK(name, cond, ...) do { int ok_ = (cond); printf("%s %s: ", ok_ ? "ok  " : "FAIL", name); printf(__VA_ARGS__); printf("\n"); if (!ok_) fails++; } while (0)

// a stage signature like Navi48Device.m builds: layout, dsl, then buffer (handle, offset), texture (object, view), sampler
static void mk(n48dc_sig *s, uint64_t layout, uint64_t dsl, uint64_t buf, uint64_t off, uint64_t tex, uint64_t view, uint64_t smp) {
    n48dc_sig_begin(s, layout, dsl); n48dc_sig_push(s, buf); n48dc_sig_push(s, off); n48dc_sig_push(s, tex); n48dc_sig_push(s, view); n48dc_sig_push(s, smp);
}
static n48dc_rpkey rk(uint32_t na, uint32_t fetch) {
    n48dc_rpkey k; memset(&k, 0, sizeof k); k.na = na; k.fetch = fetch;
    for (uint32_t i = 0; i < na; i++) k.a[i] = (n48dc_att){ 44, 1, 2, 0, 2, 1, 0, 2 };
    return k;
}

int main(void) {
    n48dc_enc e; n48dc_sig a, b; uint64_t set;
    n48dc_enc_init(&e);

    // ---- 1. signature equality ----
    mk(&a, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("empty stage: no match", !n48dc_stage_match(&e.st[0], &a, &set), "miss");
    n48dc_stage_store(&e.st[0], &a, 0x5E7);
    mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("identical bindings: same set re-used", n48dc_stage_match(&e.st[0], &b, &set) && set == 0x5E7, "set %#llx", (unsigned long long)set);
    mk(&b, 1, 10, 0xB0, 256, 0xE0, 0xF0, 0xA0);
    CHECK("offset-only change: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 11, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("set-layout (dsl) change: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 2, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    CHECK("pipeline-layout change: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB0, 0, 0xE1, 0xF0, 0xA0);
    CHECK("texture swapped: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF1, 0xA0);
    CHECK("same texture, other view: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA1);
    CHECK("sampler swapped: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    mk(&b, 1, 10, 0xB1, 0, 0xE0, 0xF0, 0xA0);
    CHECK("buffer swapped: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    n48dc_sig_begin(&b, 1, 10); n48dc_sig_push(&b, 0xB0);
    CHECK("shorter signature: new set", !n48dc_stage_match(&e.st[0], &b, &set), "miss");
    CHECK("the other stage is independent", !n48dc_stage_match(&e.st[1], &a, &set), "fragment stage empty");
    { n48dc_sig big; n48dc_sig_begin(&big, 1, 10); for (int i = 0; i < N48DC_MAXW + 5; i++) n48dc_sig_push(&big, 7);
      n48dc_stage_store(&e.st[1], &big, 0x99);
      CHECK("over-long signature is never cached", big.over && !e.st[1].valid && !n48dc_stage_match(&e.st[1], &big, &set), "over %d valid %d", big.over, e.st[1].valid); }

    // ---- 2. reset at encoder start / layout change ----
    mk(&a, 1, 10, 0xB0, 0, 0xE0, 0xF0, 0xA0);
    n48dc_enc_init(&e); n48dc_enc_layout(&e, 1); n48dc_stage_store(&e.st[0], &a, 0x5E7);
    CHECK("same layout again keeps the cached set", (n48dc_enc_layout(&e, 1), n48dc_stage_match(&e.st[0], &a, &set)), "kept");
    n48dc_enc_layout(&e, 2);
    CHECK("another pipeline layout forgets the cached sets", !e.st[0].valid && !e.st[1].valid, "valid %d %d", e.st[0].valid, e.st[1].valid);
    n48dc_stage_store(&e.st[0], &a, 0x5E7); e.lastPipe = 5;
    n48dc_enc_reset_bound(&e);
    CHECK("reset (encoder start / new pass) forgets sets, layout and pipeline", !e.st[0].valid && e.lastPipe == 0 && e.layout == 0, "valid %d lastPipe %llu layout %llu", e.st[0].valid, (unsigned long long)e.lastPipe, (unsigned long long)e.layout);
    { n48dc_enc f; n48dc_enc_init(&f);
      CHECK("a fresh encoder has nothing cached", !f.st[0].valid && !f.st[1].valid && f.lastPipe == 0 && !f.pcValid, "fresh"); }

    // ---- 3. pipeline bind skipping ----
    n48dc_enc_init(&e);
    CHECK("first bind is done", !n48dc_enc_pipe_same(&e, 0x77), "bound");
    CHECK("same pipeline: skipped", n48dc_enc_pipe_same(&e, 0x77), "skipped");
    CHECK("other pipeline: done", !n48dc_enc_pipe_same(&e, 0x78), "bound");
    CHECK("then back to the first: done (not skipped)", !n48dc_enc_pipe_same(&e, 0x77), "bound");
    n48dc_enc_reset_bound(&e);
    CHECK("after a reset the same pipeline is bound again", !n48dc_enc_pipe_same(&e, 0x77), "bound");
    CHECK("counters", e.pipesSkipped == 1 && e.pipesBound == 4, "skipped %llu bound %llu", (unsigned long long)e.pipesSkipped, (unsigned long long)e.pipesBound);

    // ---- 4. pipelineForTopology cache ----
    { uint64_t p; int dummy1 = 1, dummy2 = 2;
      n48dc_enc_init(&e);
      CHECK("pc miss on empty", !n48dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p), "miss");
      n48dc_enc_pc_put(&e, 3, 0, 1, &dummy1, 0xAA);
      CHECK("pc hit", n48dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p) && p == 0xAA, "pipe %#llx", (unsigned long long)p);
      CHECK("pc topology differs", !n48dc_enc_pc_get(&e, 4, 0, 1, &dummy1, &p), "miss");
      CHECK("pc cull differs", !n48dc_enc_pc_get(&e, 3, 1, 1, &dummy1, &p), "miss");
      CHECK("pc front differs", !n48dc_enc_pc_get(&e, 3, 0, 0, &dummy1, &p), "miss");
      CHECK("pc pso differs", !n48dc_enc_pc_get(&e, 3, 0, 1, &dummy2, &p), "miss");
      n48dc_enc_pc_put(&e, 3, 0, 1, &dummy1, 0);
      CHECK("a failed (null) pipeline is not cached", !n48dc_enc_pc_get(&e, 3, 0, 1, &dummy1, &p), "miss"); }

    // ---- 5. render-pass key ----
    { n48dc_rpcache *c = (n48dc_rpcache *)calloc(1, sizeof *c); n48dc_rpkey k = rk(2, 0), k2;
      CHECK("rp miss on empty cache", n48dc_rp_find(c, &k) == 0, "miss");
      CHECK("rp add", n48dc_rp_add(c, &k, 0x100) && c->created == 1, "created %llu", (unsigned long long)c->created);
      k2 = rk(2, 0); CHECK("equal key hits", n48dc_rp_find(c, &k2) == 0x100 && c->hits == 1, "hits %llu", (unsigned long long)c->hits);
      k2 = rk(2, 1); CHECK("fetch differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(3, 0); CHECK("attachment count differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].format++;           CHECK("format differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].samples++;          CHECK("samples differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].loadOp++;           CHECK("loadOp differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].storeOp++;          CHECK("storeOp differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].stencilLoadOp++;    CHECK("stencilLoadOp differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[0].stencilStoreOp++;   CHECK("stencilStoreOp differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].initialLayout++;    CHECK("initialLayout differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[1].finalLayout++;      CHECK("finalLayout differs", n48dc_rp_find(c, &k2) == 0, "miss");
      k2 = rk(2, 0); k2.a[5].format = 99;        CHECK("garbage beyond na is ignored", n48dc_rp_find(c, &k2) == 0x100, "hit");
      for (uint32_t i = 1; i < N48DC_RP_CAP; i++) { k2 = rk(1, 0); k2.a[0].format = 1000 + i; n48dc_rp_add(c, &k2, 0x200 + i); }
      k2 = rk(1, 1); CHECK("cache full at the cap: add refused, caller keeps ownership", c->n == N48DC_RP_CAP && !n48dc_rp_add(c, &k2, 0x999) && c->uncached == 1, "n %u uncached %llu", c->n, (unsigned long long)c->uncached);
      k2 = rk(1, 0); k2.a[0].format = 1000 + 7; CHECK("entries still found when full", n48dc_rp_find(c, &k2) == 0x207, "hit");
      free(c); }

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "all passed", fails);
    return fails != 0;
}
