// n48_drawcache.h: P5b "per-draw redundancy" decisions as plain C (no Vulkan, no Foundation; host test: test-drawcache.c). Navi48Device.m does the Vulkan calls.
//  (A) DESCRIPTOR-SET SIGNATURE.  A signature is the exact list of 64-bit words a stage's descriptor set is built from (set layout handle first, then per slot the
//      object / handle / offset the slot holds). Two draws with equal signatures would fill byte-identical sets, so the second may re-use the first's VkDescriptorSet.
//      The encoder keeps the last signature + set per stage; ANY difference (an offset-only change, another texture, another layout) means a new set.
//  (B) LAST-STATE CACHES per render encoder: last bound VkPipeline, and the last (topology, cull, front, pso) -> VkPipeline answer.
//  (C) RENDER-PASS CACHE (device-wide): key = every field of every attachment description + attachment count + fetch shape; handles are never removed.
#ifndef N48_DRAWCACHE_H
#define N48_DRAWCACHE_H
#include <stdint.h>
#include <string.h>

#define N48DC_MAXW 256   // longest cacheable signature (words); a longer one is never cached (always a fresh set)

typedef struct { uint32_t n; int over; uint64_t w[N48DC_MAXW]; } n48dc_sig;
static inline void n48dc_sig_begin(n48dc_sig *s, uint64_t layout, uint64_t dsl) { s->n = 0; s->over = 0; s->w[s->n++] = layout; s->w[s->n++] = dsl; }
static inline void n48dc_sig_push(n48dc_sig *s, uint64_t v) { if (s->n >= N48DC_MAXW) { s->over = 1; return; } s->w[s->n++] = v; }

typedef struct { int valid; uint32_t n; uint64_t w[N48DC_MAXW]; uint64_t set; } n48dc_stage;
static inline void n48dc_stage_reset(n48dc_stage *t) { t->valid = 0; t->n = 0; t->set = 0; }
// YES when the stage holds a set built from exactly this signature (then *set is that set).
static inline int n48dc_stage_match(const n48dc_stage *t, const n48dc_sig *s, uint64_t *set) {
    if (s->over || !t->valid || t->n != s->n || memcmp(t->w, s->w, (size_t)s->n * sizeof s->w[0]) != 0) return 0;
    *set = t->set; return 1;
}
static inline void n48dc_stage_store(n48dc_stage *t, const n48dc_sig *s, uint64_t set) {
    if (s->over) { n48dc_stage_reset(t); return; }
    t->valid = 1; t->n = s->n; memcpy(t->w, s->w, (size_t)s->n * sizeof s->w[0]); t->set = set;
}

typedef struct {
    uint64_t layout;                 // pipeline layout the cached sets were bound with
    n48dc_stage st[2];               // 0 = vertex set (index 0), 1 = fragment set (index 1)
    uint64_t lastPipe;               // last VkPipeline bound in the current render pass (0 = none)
    int pcValid; uint32_t pcTopo, pcCull, pcFront; const void *pcPso; uint64_t pcPipe;   // last pipelineForTopology answer
    uint64_t draws, setsReused, setsAllocated, pipesSkipped, pipesBound, pcHits;
} n48dc_enc;
static inline void n48dc_enc_init(n48dc_enc *e) { memset(e, 0, sizeof *e); }
// Encoder start, new render pass, any barrier / meta operation: forget what is bound.
static inline void n48dc_enc_reset_bound(n48dc_enc *e) { e->lastPipe = 0; e->layout = 0; n48dc_stage_reset(&e->st[0]); n48dc_stage_reset(&e->st[1]); }
// A draw binds a pipeline with this layout: a different layout forgets the cached sets.
static inline void n48dc_enc_layout(n48dc_enc *e, uint64_t layout) { if (e->layout != layout) { e->layout = layout; n48dc_stage_reset(&e->st[0]); n48dc_stage_reset(&e->st[1]); } }
// YES when vkCmdBindPipeline may be skipped (same pipeline as the last one bound in this pass).
static inline int n48dc_enc_pipe_same(n48dc_enc *e, uint64_t pipe) { if (e->lastPipe == pipe && pipe) { e->pipesSkipped++; return 1; } e->lastPipe = pipe; e->pipesBound++; return 0; }
static inline int n48dc_enc_pc_get(n48dc_enc *e, uint32_t topo, uint32_t cull, uint32_t front, const void *pso, uint64_t *pipe) {
    if (e->pcValid && e->pcTopo == topo && e->pcCull == cull && e->pcFront == front && e->pcPso == pso) { *pipe = e->pcPipe; e->pcHits++; return 1; }
    return 0;
}
static inline void n48dc_enc_pc_put(n48dc_enc *e, uint32_t topo, uint32_t cull, uint32_t front, const void *pso, uint64_t pipe) {
    e->pcValid = pipe != 0; e->pcTopo = topo; e->pcCull = cull; e->pcFront = front; e->pcPso = pso; e->pcPipe = pipe;
}

// ---- (C) render-pass cache ----
#define N48DC_RP_CAP 64
typedef struct { uint32_t format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initialLayout, finalLayout; } n48dc_att;
typedef struct { uint32_t na, fetch; n48dc_att a[8]; } n48dc_rpkey;
static inline int n48dc_key_eq(const n48dc_rpkey *x, const n48dc_rpkey *y) {
    if (x->na != y->na || x->fetch != y->fetch) return 0;
    for (uint32_t i = 0; i < x->na && i < 8; i++) {
        const n48dc_att *p = &x->a[i], *q = &y->a[i];
        if (p->format != q->format || p->samples != q->samples || p->loadOp != q->loadOp || p->storeOp != q->storeOp || p->stencilLoadOp != q->stencilLoadOp ||
            p->stencilStoreOp != q->stencilStoreOp || p->initialLayout != q->initialLayout || p->finalLayout != q->finalLayout) return 0;
    }
    return 1;
}
typedef struct { uint32_t n; n48dc_rpkey k[N48DC_RP_CAP]; uint64_t h[N48DC_RP_CAP]; uint64_t hits, created, uncached; } n48dc_rpcache;
static inline uint64_t n48dc_rp_find(n48dc_rpcache *c, const n48dc_rpkey *k) {
    for (uint32_t i = 0; i < c->n; i++) if (n48dc_key_eq(&c->k[i], k)) { c->hits++; return c->h[i]; }
    return 0;
}
// YES when the handle was entered (the caller must then never destroy it); NO when the cache is full (the caller keeps ownership as before).
static inline int n48dc_rp_add(n48dc_rpcache *c, const n48dc_rpkey *k, uint64_t h) {
    if (c->n >= N48DC_RP_CAP || !h) { c->uncached++; return 0; }
    c->k[c->n] = *k; c->h[c->n] = h; c->n++; c->created++; return 1;
}
#endif
