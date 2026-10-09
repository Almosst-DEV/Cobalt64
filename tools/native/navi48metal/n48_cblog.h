// n48_cblog.h: pure bookkeeping of the Stage 0 cross-queue RAW-inversion counter (an internal design note, 0b). Host test: test-cblog.c.
// A command buffer is COMMITTED (registered here with the IOSurface ids it writes and reads, stamped with the commit time) and later SUBMITTED (removed).
// At each submit of X, for every sid X reads (samples or loads), every OTHER entry that is still registered (= committed, not yet submitted), was committed
// before X, and writes that sid is one inversion: X may run on the GPU before a producer the app ordered ahead of it. No Vulkan, no ObjC, no locking
// (the bundle holds a mutex around every call). sids are IOSurfaceGetID values, the same ids the DTrace transaction log prints.
#ifndef N48_CBLOG_H
#define N48_CBLOG_H
#include <stdint.h>
#include <string.h>

#define N48CB_TAB 512      // live (committed, not yet submitted) command buffers; the oldest is evicted when full
#define N48CB_MAXW 8
#define N48CB_MAXR 16
#define N48CB_MAXINV 32    // inversions reported per submit (the count is exact; only the detail list is capped)

typedef struct { uint64_t id, queue, commit_ns; uint32_t nw, nr; uint32_t w[N48CB_MAXW], r[N48CB_MAXR]; int live; } n48cb_ent_t;
typedef struct { uint64_t reader_id, reader_queue, reader_sub, writer_id, writer_queue, writer_commit; uint32_t sid; } n48cb_inv_t;
typedef struct {
    n48cb_ent_t e[N48CB_TAB];
    uint64_t sub_seq;                       // submit sequence (1-based), assigned by n48cb_submit
    uint64_t commits, submits, drops, evicted, no_commit;   // no_commit: submitted without a registered commit (logging was off at commit time, or evicted)
    uint64_t inv_total, inv_cbs;            // inversions (reader,writer,sid triples) and the number of submits that had at least one
    uint64_t iv_submits, iv_inv;            // interval counters (n48cb_take_interval)
} n48cb_t;

static inline void n48cb_init(n48cb_t *t) { memset(t, 0, sizeof *t); }

// Adds sid to a deduplicated list. Returns the new count (a full list drops the sid: documented limit, never overflows).
static inline uint32_t n48cb_addsid(uint32_t *a, uint32_t n, uint32_t cap, uint32_t sid) {
    if (!sid) return n;
    for (uint32_t i = 0; i < n; i++) if (a[i] == sid) return n;
    if (n < cap) a[n++] = sid;
    return n;
}
static inline int n48cb_has(const uint32_t *a, uint32_t n, uint32_t sid) { for (uint32_t i = 0; i < n; i++) if (a[i] == sid) return 1; return 0; }

static inline n48cb_ent_t *n48cb_find(n48cb_t *t, uint64_t id) {
    for (int i = 0; i < N48CB_TAB; i++) if (t->e[i].live && t->e[i].id == id) return &t->e[i];
    return NULL;
}

// Register a commit. A second commit of the same id replaces the first (a re-commit is not an error).
static inline void n48cb_commit(n48cb_t *t, uint64_t id, uint64_t queue, uint64_t now, const uint32_t *w, uint32_t nw, const uint32_t *r, uint32_t nr) {
    n48cb_ent_t *s = n48cb_find(t, id);
    if (!s) {
        int fr = -1, old = 0;
        for (int i = 0; i < N48CB_TAB; i++) { if (!t->e[i].live) { fr = i; break; } if (t->e[i].commit_ns < t->e[old].commit_ns) old = i; }
        if (fr < 0) { fr = old; t->evicted++; }
        s = &t->e[fr];
    }
    memset(s, 0, sizeof *s);
    s->live = 1; s->id = id; s->queue = queue; s->commit_ns = now;
    s->nw = nw > N48CB_MAXW ? N48CB_MAXW : nw; memcpy(s->w, w, s->nw * sizeof w[0]);
    s->nr = nr > N48CB_MAXR ? N48CB_MAXR : nr; memcpy(s->r, r, s->nr * sizeof r[0]);
    t->commits++;
}

// Submit of cb `id` (the authoritative write/read sets are passed again: encoding may have added surfaces since the commit snapshot). Returns the number of
// inversions found for this submit; the first `maxinv` are described in inv[]. *seq_out = this submit's sequence number. Removes the entry.
static inline uint32_t n48cb_submit(n48cb_t *t, uint64_t id, uint64_t queue, uint64_t now, const uint32_t *w, uint32_t nw, const uint32_t *r, uint32_t nr,
                                    n48cb_inv_t *inv, uint32_t maxinv, uint64_t *seq_out) {
    (void)w; (void)nw;
    n48cb_ent_t *x = n48cb_find(t, id);
    uint64_t xc;
    if (x) xc = x->commit_ns; else { xc = now; t->no_commit++; }   // unknown commit: treated as committed now (conservative: fewer inversions)
    uint64_t sq = ++t->sub_seq; if (seq_out) *seq_out = sq;
    uint32_t n = 0;
    for (uint32_t k = 0; k < nr; k++) {
        uint32_t sid = r[k]; if (!sid) continue;
        for (int i = 0; i < N48CB_TAB; i++) {
            const n48cb_ent_t *e = &t->e[i];
            if (!e->live || e->id == id) continue;
            if (!(e->commit_ns < xc || (e->commit_ns == xc && e->id < id))) continue;   // only producers committed EARLIER
            if (!n48cb_has(e->w, e->nw, sid)) continue;
            if (n < maxinv && inv) { n48cb_inv_t *o = &inv[n]; o->reader_id = id; o->reader_queue = queue; o->reader_sub = sq; o->writer_id = e->id; o->writer_queue = e->queue; o->writer_commit = e->commit_ns; o->sid = sid; }
            n++;
        }
    }
    if (x) x->live = 0;
    t->submits++; t->iv_submits++;
    if (n) { t->inv_total += n; t->inv_cbs++; t->iv_inv += n; }
    return n;
}

// A command buffer that will never be submitted (encode error, cancelled): forget it so it cannot be counted as a pending producer.
static inline void n48cb_drop(n48cb_t *t, uint64_t id) { n48cb_ent_t *x = n48cb_find(t, id); if (x) { x->live = 0; t->drops++; } }

static inline void n48cb_take_interval(n48cb_t *t, uint64_t *submits, uint64_t *inv) { *submits = t->iv_submits; *inv = t->iv_inv; t->iv_submits = 0; t->iv_inv = 0; }
#endif
