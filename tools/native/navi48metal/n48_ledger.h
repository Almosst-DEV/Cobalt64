// n48_ledger.h: bundle build 16, app-fix round 1 (an internal design note section P5): pure C, no Foundation, no Vulkan, no IOSurface (host test: test-ledger.c).
//  (A) THE LIVE-IMPORT LEDGER (Step 0, logging only). One entry per live IOSurface-backed texture: its surface id, the import bytes it stands for, pixel format, plane count, birth time and the
//      number of command buffers that have touched it and not finished yet. A snapshot gives: live textures, distinct live surface ids, bytes (per texture and per distinct surface), textures
//      with NO command buffer in flight (a WindowServer that keeps textures nothing draws with is the "held for no reason" signature), and the top 5 groups by (size, pixel format, planes) with the
//      age of the oldest member. Navi48Device.m holds ONE mutex around every call.
//  (B) THE USE-COUNT POLICY (F3). Our texture used to call IOSurfaceIncrementUseCount for its whole life. Apple's own Metal does not (measured on the host Mac's M4, macOS 27.0.1, IOSurfaceGetUseCount /
//      IOSurfaceIsInUse stay 0 at every point: fresh surface, after newTexture, encoded, committed, completed, texture released), so a decoder pool that recycles only surfaces "not in use" never
//      recycled the ones WindowServer had wrapped: a Safari/YouTube 1080p video made new surfaces all the time and WindowServer imported every one of them. The policy now: a surface is use-counted
//      ONLY while a command buffer that touched a texture of it is in flight. n48uc_list is that command buffer's set of held surfaces (one count per surface per command buffer); the
//      increment/decrement are callbacks so the test can count them. n48uc_release is idempotent and is called from every end of a command buffer's life.
#ifndef N48_LEDGER_H
#define N48_LEDGER_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ---- (A) ledger ----------------------------------------------------------------------------------------------------------------
#define N48L_TOP 5
typedef struct { uintptr_t key; uint32_t sid, pf; uint64_t bytes, born; uint32_t planes; int inflight, nobase; } n48l_ent;
typedef struct { n48l_ent *e; size_t n, c; uint64_t added, removed, doubleAdd, delMissing, delInflight, touchMissing, untouchMissing, untouchUnder; } n48l;
typedef struct { uint64_t bytes, oldestAgeNs; uint32_t pf, planes, count; } n48l_grp;
typedef struct {
    uint32_t live, nobase, distinct, idle, inflightTex;   // idle = live textures with NO command buffer in flight
    uint64_t texBytes, distinctBytes;                    // texBytes: sum over textures (a shared import counts once per texture); distinctBytes: the largest import of each distinct surface id
    uint64_t oldestAgeNs; uint32_t ngrp; n48l_grp top[N48L_TOP];
} n48l_snap;

static inline void n48l_init(n48l *L) { memset(L, 0, sizeof *L); }
static inline void n48l_destroy(n48l *L) { free(L->e); memset(L, 0, sizeof *L); }
static inline size_t n48l_find(const n48l *L, uintptr_t key) { for (size_t i = 0; i < L->n; i++) if (L->e[i].key == key) return i; return (size_t)-1; }
// A texture came alive. Returns 1 when added (0: the key was already live; counted, nothing added).
static inline int n48l_add(n48l *L, uintptr_t key, uint32_t sid, uint64_t bytes, uint32_t pf, uint32_t planes, int nobase, uint64_t now) {
    if (n48l_find(L, key) != (size_t)-1) { L->doubleAdd++; return 0; }
    if (L->n == L->c) { size_t nc = L->c ? L->c * 2 : 64; n48l_ent *ne = (n48l_ent *)realloc(L->e, nc * sizeof *ne); if (!ne) return 0; L->e = ne; L->c = nc; }
    L->e[L->n++] = (n48l_ent){ key, sid, pf, bytes, now, planes, 0, nobase };
    L->added++; return 1;
}
// The texture died. Returns 1 when it was live.
static inline int n48l_del(n48l *L, uintptr_t key) {
    size_t i = n48l_find(L, key);
    if (i == (size_t)-1) { L->delMissing++; return 0; }
    if (L->e[i].inflight > 0) L->delInflight++;
    L->e[i] = L->e[--L->n]; L->removed++; return 1;
}
static inline void n48l_touch(n48l *L, uintptr_t key) { size_t i = n48l_find(L, key); if (i == (size_t)-1) L->touchMissing++; else L->e[i].inflight++; }
static inline void n48l_untouch(n48l *L, uintptr_t key) {
    size_t i = n48l_find(L, key);
    if (i == (size_t)-1) L->untouchMissing++; else if (L->e[i].inflight <= 0) L->untouchUnder++; else L->e[i].inflight--;
}
static int n48l_cmp_sid(const void *a, const void *b) { const n48l_ent *x = (const n48l_ent *)a, *y = (const n48l_ent *)b; return x->sid < y->sid ? -1 : x->sid > y->sid ? 1 : x->bytes < y->bytes ? -1 : x->bytes > y->bytes; }
static int n48l_cmp_grp(const void *a, const void *b) {
    const n48l_ent *x = (const n48l_ent *)a, *y = (const n48l_ent *)b;
    if (x->bytes != y->bytes) return x->bytes < y->bytes ? -1 : 1;
    if (x->pf != y->pf) return x->pf < y->pf ? -1 : 1;
    return x->planes < y->planes ? -1 : x->planes > y->planes;
}
static inline void n48l_snapshot(const n48l *L, uint64_t now, n48l_snap *s) {
    memset(s, 0, sizeof *s);
    s->live = (uint32_t)L->n;
    if (!L->n) return;
    n48l_ent *t = (n48l_ent *)malloc(L->n * sizeof *t);
    if (!t) return;
    memcpy(t, L->e, L->n * sizeof *t);
    for (size_t i = 0; i < L->n; i++) {
        const n48l_ent *e = &t[i];
        s->texBytes += e->bytes; if (e->nobase) s->nobase++;
        if (e->inflight > 0) s->inflightTex++; else s->idle++;
        uint64_t age = now > e->born ? now - e->born : 0; if (age > s->oldestAgeNs) s->oldestAgeNs = age;
    }
    qsort(t, L->n, sizeof *t, n48l_cmp_sid);   // distinct ids: runs of one sid; the largest import of the run counts
    for (size_t i = 0; i < L->n;) { size_t j = i; uint64_t mx = 0; while (j < L->n && t[j].sid == t[i].sid) { if (t[j].bytes > mx) mx = t[j].bytes; j++; } s->distinct++; s->distinctBytes += mx; i = j; }
    qsort(t, L->n, sizeof *t, n48l_cmp_grp);   // groups: runs of one (bytes, pf, planes); keep the N48L_TOP biggest by count (ties: more bytes first)
    for (size_t i = 0; i < L->n;) {
        size_t j = i; n48l_grp g = { t[i].bytes, 0, t[i].pf, t[i].planes, 0 };
        while (j < L->n && t[j].bytes == t[i].bytes && t[j].pf == t[i].pf && t[j].planes == t[i].planes) { uint64_t age = now > t[j].born ? now - t[j].born : 0; if (age > g.oldestAgeNs) g.oldestAgeNs = age; g.count++; j++; }
        s->ngrp++;
        int at = -1;
        for (int k = 0; k < N48L_TOP; k++) { if (s->top[k].count == 0 || g.count > s->top[k].count || (g.count == s->top[k].count && g.bytes > s->top[k].bytes)) { at = k; break; } }
        if (at >= 0) { for (int k = N48L_TOP - 1; k > at; k--) s->top[k] = s->top[k - 1]; s->top[at] = g; }
        i = j;
    }
    free(t);
}
// One line (no newline) for the 10 s tick. impTotal = the bundle's running import total (what the kernel holds for this process).
static inline int n48l_fmt(const n48l_snap *s, uint64_t impTotal, const n48l *L, char *out, size_t cap) {
    int n = snprintf(out, cap, "T1 ledger: surface textures live=%u (no-cpu-mapping %u) distinct_surface_ids=%u tex_bytes=%llu KiB distinct_bytes=%llu KiB import_total=%llu KiB idle(no command buffer in flight)=%u in_flight=%u oldest=%.1fs added/removed=%llu/%llu top%d(size_B pf planes x count oldest_s):",
        s->live, s->nobase, s->distinct, (unsigned long long)(s->texBytes >> 10), (unsigned long long)(s->distinctBytes >> 10), (unsigned long long)(impTotal >> 10), s->idle, s->inflightTex, (double)s->oldestAgeNs / 1e9,
        (unsigned long long)L->added, (unsigned long long)L->removed, N48L_TOP);
    for (int k = 0; k < N48L_TOP && s->top[k].count && n > 0 && (size_t)n < cap; k++)
        n += snprintf(out + n, cap - (size_t)n, " [%llu 0x%08x p%u x%u %.1fs]", (unsigned long long)s->top[k].bytes, s->top[k].pf, s->top[k].planes, s->top[k].count, (double)s->top[k].oldestAgeNs / 1e9);
    if ((L->doubleAdd || L->delMissing || L->delInflight || L->touchMissing || L->untouchMissing || L->untouchUnder) && n > 0 && (size_t)n < cap)
        n += snprintf(out + n, cap - (size_t)n, " ANOMALY doubleAdd=%llu delMissing=%llu delWhileInflight=%llu touchMissing=%llu untouchMissing=%llu untouchUnder=%llu",
            (unsigned long long)L->doubleAdd, (unsigned long long)L->delMissing, (unsigned long long)L->delInflight, (unsigned long long)L->touchMissing, (unsigned long long)L->untouchMissing, (unsigned long long)L->untouchUnder);
    return n;
}

// ---- (B) the use-count policy (F3) -----------------------------------------------------------------------------------------------
#define N48UC_INFLIGHT 1   // default: a surface is use-counted only while a command buffer that touched a texture of it is in flight
#define N48UC_NONE     0   // kill file /private/tmp/n48m-nousecount: Apple-identical, never use-counted (diagnosis)
// killedStat: 1 when the kill file exists. Anything else is the default.
static inline int n48uc_mode(int killedStat) { return killedStat ? N48UC_NONE : N48UC_INFLIGHT; }
typedef struct { void **s; size_t n, c; } n48uc_list;
typedef void (*n48uc_fn)(void *surface, void *ctx);
// The command buffer touched a texture of `surface`. 1 = newly held (inc was called), 0 = already held by this command buffer or the policy is NONE.
static inline int n48uc_take(n48uc_list *l, int mode, void *surface, n48uc_fn inc, void *ctx) {
    if (mode != N48UC_INFLIGHT || !surface) return 0;
    for (size_t i = 0; i < l->n; i++) if (l->s[i] == surface) return 0;
    if (l->n == l->c) { size_t nc = l->c ? l->c * 2 : 8; void **ns = (void **)realloc(l->s, nc * sizeof *ns); if (!ns) return 0; l->s = ns; l->c = nc; }
    l->s[l->n++] = surface; inc(surface, ctx); return 1;
}
// The command buffer is done with the GPU (completed, failed to submit, dropped, or deallocated). Releases every held surface exactly once; returns how many. Idempotent.
static inline size_t n48uc_release(n48uc_list *l, n48uc_fn dec, void *ctx) {
    size_t k = l->n;
    for (size_t i = 0; i < k; i++) dec(l->s[i], ctx);
    l->n = 0;
    return k;
}
static inline void n48uc_free(n48uc_list *l) { free(l->s); l->s = NULL; l->n = l->c = 0; }
#endif
