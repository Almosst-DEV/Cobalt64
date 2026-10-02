// gfx_wc98.h — build 0.0.541: SWITCH 98, "walkcache98", THE PROVENANCE-ASK WALK CACHE.
//
// WHY. RUN AQ2 (run11aj) measured the GFX hook's time going to provenance ASKS doing uncached page walks: `asks (n/us) tiled
// 40122/21950538`, `walks ask 2960049/5942201/0/21873752` (calls/levels/fails/us: 2.0 levels per walk, i.e. almost every walk ends
// at a 64 KiB L1 leaf). The walks are ws_resprov.h's proof loop (`for (p = 0; p < e->bytes; p += 4096) walk(vm, va + p ...)`),
// which re-walks EVERY 4 KiB page of a residency entry at every ask; run11aj's table held the 1920x1080x4 wallpaper entry
// (`COPIED #63 ... bytes=0x7e9000 ... GPU VA 0x405800000`, RECORDED at the arm's start), 2025 pages per proof. 16 consecutive
// pages of a 64 KiB leaf re-read the same root and L1 entries 16 times.
//
// WHAT. A small direct-mapped cache of gfxc_page answers, consulted ONLY inside an OPEN SCOPE, keyed by the walk's whole input:
// the VM (its root's VRAM offset, START, the aperture base and size: the VMID's registers ARE what selects these) and the VA's
// granule - its 64 KiB granule when the walk ended at an L1 leaf (every page of that granule reads the same two entries and the
// walk's own formula `(leafAddr + (va & 0xf000)) & ~0xfff` gives its page), else its 4 KiB page. It stores the leaf, the page
// and the SYSTEM bit. A failed walk is NEVER stored. An entry answers only in the scope that stored it, only while the
// generation counter still holds the value read BEFORE its walk, and only while no remap is in flight (`in` == `out`).
//
// THE SCOPE (DEVIATION from the brief's "one policy pass", reported): ONE provenance ask (gfxsrc_desc_tiled_ok, which _okt calls).
// Apple writes its page tables with SDMA PTEPDE packets (b: `AMDGFX10SDMAChannel::writeWritePTEPDECommand`;:
// `0000000c d6c09b80 ...`), executed by the SDMA engine at a time no CPU hook brackets, so no generation bump can be proven to
// precede a PTE's landing; a pass-long entry could outlive a landing. Within one ask, a cached answer is one this same ask
// read moments earlier - exactly the staleness today's uncached proof loop already has (it walks its 2025 pages one after the
// other, never as a snapshot). The per-ask scope still removes 15 of every 16 walks of a 64 KiB-leaf surface.
//
// MODES (`accel gfxneuter 98 | M << 8`): M 1 ON (= 354) answers hits from the cache; M 2 OFF (= 610, the default and the boot
// value) never opens a scope, so gfxc_page is 0.0.540's walk; M 3 SHADOW (= 866) computes both answers, USES THE UNCACHED ONE,
// and counts every disagreement (the first N48_WC_DIS recorded with VA, both pages, both SYSTEM bits, both leaves). Joins the
// continuous mid-arm guard (gfx_commit.h), so it is constant for an arm.
//
// PURE: no kext symbol, no register, no page table, nothing of Apple's is written. The walk is a callback. Compiled into the kext
// and into tests/gfx_wc98_test.cpp (an oracle page table, randomized remap schedules, every bump site, planted stale entries).
#ifndef N48_GFX_WC98_H
#define N48_GFX_WC98_H

#include <stdint.h>

enum { N48_WC_ON = 1u, N48_WC_OFF = 2u, N48_WC_SHADOW = 3u };   /* the switch's M, held as is; N48_WC_OFF at boot */
#define N48_WC_SWITCH 98u
#define N48_WC_N   256u     /* entries, direct-mapped (a power of two) */
#define N48_WC_DIS 8u       /* SHADOW disagreements recorded (the first N48_WC_DIS since the counters were reset) */

/* The switch's setter: M 1, 2, 3 set the mode (1 = changed); anything else 0 and *mode untouched. */
static inline int n48_wc_set(uint32_t m, uint32_t *mode)
{
    if (m != N48_WC_ON && m != N48_WC_OFF && m != N48_WC_SHADOW) return 0;
    if (mode) *mode = m;
    return 1;
}
static inline const char *n48_wc_mode_name(uint32_t mode)
{
    return mode == N48_WC_ON ? "ON (354: an ask's page walks answer from its own cache)"
         : mode == N48_WC_SHADOW ? "SHADOW (866: the walk's answer used, disagreements counted)"
         : "OFF (610, default: every walk is 0.0.540's)";
}

/* ---- THE GENERATION: every bump site (the kext's, listed in the report line) moves `gen`; a remap bracket also moves `in` at its
 * start and `out` at its end, so `in != out` means a remap is in flight (unmatched brackets read as in flight: fail-safe). ---- */
enum {
    N48_WC_B_UNMAP_IN = 0u, N48_WC_B_UNMAP_OUT,   /* hook_unmapVA: entry / every return (RAII), any context */
    N48_WC_B_MAP_IN, N48_WC_B_MAP_OUT,            /* hook_mapVA: entry / every return (RAII), any context */
    N48_WC_B_CG_OPEN, N48_WC_B_CG_END,            /* navi48_cg_open / navi48_cg_close: every residency copy and every MM write */
    N48_WC_B_KS_WITHDRAW,                         /* the keystone's root[511] clear (hook_unmapVA) */
    N48_WC_B_KS_ARM,                              /* the keystone's root[511] write (rootwrite_arm_context, every caller) */
    N48_WC_B_REBIND,                              /* WindowServer's binding moved (gWs.gen) */
    N48_WC_B_CTX_RELEASE,                         /* hook_releaseVMContext */
    N48_WC_B_WSV_BASE,                            /* ws-valid's context base write */
    N48_WC_B_SDMA_SUBMIT,                         /* hook_submitCommandBuffer (Apple's SDMA work, PTEPDE included) */
    N48_WC_B_N
};
typedef struct {
    uint64_t gen;
    uint64_t in, out;
    uint64_t site[N48_WC_B_N];
    /* build 0.0.542 (the 0.0.541 review's SHOULD-FIX): padded to whole 64-byte lines; the kext's one instance,
     * gN48WcGen, is alignas(64), so no other global (gN48Pf540On sat in its first line in 0.0.541) shares a line with it. */
    uint8_t pad[(64u - ((3u + N48_WC_B_N) * 8u) % 64u) % 64u];
} n48_wc_gen;
#ifdef __cplusplus
static_assert(sizeof(n48_wc_gen) % 64u == 0u, "n48_wc_gen fills whole cache lines");
#endif
static inline void n48_wc_bump(n48_wc_gen *g, uint32_t site)
{
    __atomic_fetch_add(&g->gen, 1ull, __ATOMIC_SEQ_CST);
    if (site < N48_WC_B_N) __atomic_fetch_add(&g->site[site], 1ull, __ATOMIC_RELAXED);
}
/* A remap's start: in-flight first, then the generation. */
static inline void n48_wc_enter(n48_wc_gen *g, uint32_t site)
{
    __atomic_fetch_add(&g->in, 1ull, __ATOMIC_SEQ_CST);
    n48_wc_bump(g, site);
}
/* A remap's end: the generation first, then out of flight. */
static inline void n48_wc_leave(n48_wc_gen *g, uint32_t site)
{
    n48_wc_bump(g, site);
    __atomic_fetch_add(&g->out, 1ull, __ATOMIC_SEQ_CST);
}
static inline uint64_t n48_wc_bumps(const n48_wc_gen *g)
{
    uint64_t s = 0ull;
    for (uint32_t i = 0; i < N48_WC_B_N; i++) s += __atomic_load_n(&g->site[i], __ATOMIC_RELAXED);
    return s;
}
/* The stable view a walk is bracketed by: 1 = no remap in flight; *gen the generation. */
static inline uint32_t n48_wc_view(n48_wc_gen *g, uint64_t *gen)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    const uint64_t o = __atomic_load_n(&g->out, __ATOMIC_SEQ_CST);
    *gen = __atomic_load_n(&g->gen, __ATOMIC_SEQ_CST);
    const uint64_t i = __atomic_load_n(&g->in, __ATOMIC_SEQ_CST);
    return i == o ? 1u : 0u;
}

/* ---- build 0.0.543 item B ( B, "98 v2 step 1"): SWITCH 101, THE INSTABILITY CENSUS. No behaviour change: the
 * unstable decision above (`!still0 || !still1 || g0 != g1`) is untouched; with the census ON each unstable walk is ATTRIBUTED to
 * the bracket classes that were in flight at either view (a class's own IN / OUT site counts differ) or whose sites moved between
 * the two views, each counted separately (a walk can name several classes; `multi` counts those). NONE: nothing attributable
 * (e.g. the generation moved by a site whose count this snapshot missed). OTHER holds the keystone, rebind, ctx-release,
 * ws-valid and SDMA sites; a ctx release in flight (its bracket shares ONE site) is OTHER when the global pair is unbalanced and no
 * other class is in flight. ---- */
enum { N48_WC_K_MAP = 0u, N48_WC_K_UNMAP, N48_WC_K_CG, N48_WC_K_OTHER, N48_WC_K_N };
#define N48_WC_K_NONE_BIT (1u << N48_WC_K_N)
static inline uint32_t n48_wc_site_class(uint32_t site)
{
    if (site == N48_WC_B_MAP_IN || site == N48_WC_B_MAP_OUT) return N48_WC_K_MAP;
    if (site == N48_WC_B_UNMAP_IN || site == N48_WC_B_UNMAP_OUT) return N48_WC_K_UNMAP;
    if (site == N48_WC_B_CG_OPEN || site == N48_WC_B_CG_END) return N48_WC_K_CG;
    return N48_WC_K_OTHER;
}
/* Counters. EVERY FIELD A uint64_t. */
typedef struct {
    uint64_t unstable;               /* unstable walks seen with the census ON */
    uint64_t inflight[N48_WC_K_N];   /* ... a class in flight at either view */
    uint64_t moved[N48_WC_K_N];      /* ... a class's sites moved between the views */
    uint64_t any[N48_WC_K_N];        /* ... either (the per-class unstable count) */
    uint64_t none, multi;
} n48_wc_cen;
#define N48_WC_CEN_WORDS (sizeof(n48_wc_cen) / sizeof(uint64_t))
static inline void n48_wc_sites(n48_wc_gen *g, uint64_t *out)
{
    for (uint32_t i = 0; i < N48_WC_B_N; i++) out[i] = __atomic_load_n(&g->site[i], __ATOMIC_SEQ_CST);
}
/* The attribution of ONE unstable walk from the two site snapshots and the two views' stability: a class bitmask (bit
 * N48_WC_K_*), or N48_WC_K_NONE_BIT when nothing is attributable. *inflight_m: the classes in flight at either view. Pure. */
static inline uint32_t n48_wc_attr(const uint64_t *s0, const uint64_t *s1, uint32_t still0, uint32_t still1, uint32_t *inflight_m)
{
    static const uint32_t pin[3] = { N48_WC_B_MAP_IN, N48_WC_B_UNMAP_IN, N48_WC_B_CG_OPEN };
    static const uint32_t pout[3] = { N48_WC_B_MAP_OUT, N48_WC_B_UNMAP_OUT, N48_WC_B_CG_END };
    static const uint32_t pk[3] = { N48_WC_K_MAP, N48_WC_K_UNMAP, N48_WC_K_CG };
    uint32_t fl = 0u;
    for (uint32_t j = 0; j < 3u; j++)
        if (s0[pin[j]] != s0[pout[j]] || s1[pin[j]] != s1[pout[j]]) fl |= 1u << pk[j];
    if (!fl && (!still0 || !still1)) fl |= 1u << N48_WC_K_OTHER;   /* the global pair unbalanced, no bracket class: a ctx release */
    uint32_t m = fl;
    for (uint32_t i = 0; i < N48_WC_B_N; i++) if (s1[i] != s0[i]) m |= 1u << n48_wc_site_class(i);
    if (inflight_m) *inflight_m = fl;
    return m ? m : N48_WC_K_NONE_BIT;
}
/* Count one attribution (`inflight_m` the in-flight half alone, `m` the whole answer). */
static inline void n48_wc_cen_note(n48_wc_cen *c, uint32_t m, uint32_t inflight_m)
{
    c->unstable++;
    if (m & N48_WC_K_NONE_BIT) { c->none++; return; }
    uint32_t k = 0u;
    for (uint32_t j = 0; j < N48_WC_K_N; j++) {
        if (!(m & (1u << j))) continue;
        k++;
        c->any[j]++;
        if (inflight_m & (1u << j)) c->inflight[j]++;
        else c->moved[j]++;
    }
    if (k > 1u) c->multi++;
}

/* ---- the key: the walk's whole input ---- */
typedef struct { uint64_t root_off, start_va, fb_start, vram_size, ok; } n48_wc_vmk;   /* ok: GfxcVm.ok (a walk of a !ok vm fails) */

typedef struct {
    uint64_t gran;        /* va >> 16 (big) or va >> 12 (small) */
    n48_wc_vmk k;
    uint64_t leaf, page;  /* the leaf; the page (small only: big derives it from the leaf per VA) */
    uint64_t gen, scope;
    uint32_t big, sys, valid, pad;
} n48_wc_ent;

typedef struct { uint64_t va, cpage, wpage, cleaf, wleaf; uint32_t csys, wsys, wok, pad; } n48_wc_dis;

/* Counters. EVERY FIELD A uint64_t (n48_wc_sub is element-wise). */
typedef struct {
    uint64_t scopes;       /* asks that opened a scope (ON or SHADOW) */
    uint64_t scopes_hit;   /* ... of which at least one lookup hit */
    uint64_t nested;       /* an ask found THE scope already open - there is one for the whole kext, so this is almost always a
                              CONCURRENT ask on another thread, not a nest on this one; that ask opens nothing and walks uncached */
    uint64_t lookups;      /* walks asked inside a scope */
    uint64_t hits;         /* answered from the cache (ON) / would have been (SHADOW) */
    uint64_t misses;       /* walked */
    uint64_t stored;       /* misses stored */
    uint64_t fails;        /* walks that failed (never stored) */
    uint64_t unstable;     /* walks whose generation moved or with a remap in flight (not stored, no hit) */
    uint64_t compared;     /* SHADOW: hits compared with a fresh walk */
    uint64_t disagree;     /* SHADOW: ... that differed */
} n48_wc_st;
#define N48_WC_ST_WORDS (sizeof(n48_wc_st) / sizeof(uint64_t))

typedef struct {
    n48_wc_ent e[N48_WC_N];
    uint64_t scope;        /* the open scope's id (a new id per scope: every older entry is dead) */
    uintptr_t owner;       /* the thread that opened it */
    uint32_t open, mode;   /* mode latched at the scope's open */
    n48_wc_st st;
    n48_wc_dis dis[N48_WC_DIS];
    uint32_t ndis, pad;
    /* build 0.0.543 item B (switch 101): the census. `census` is written only by the kext's switch-101 verb (0 = OFF: nothing
     * below is read or written by a walk); the two snapshots are the owner thread's (static storage: never on the walk's stack);
     * `last_m` is the last walk's attribution (0 = stable or census OFF), read by the kext right after n48_wc_page for perf540. */
    uint32_t census, last_m;
    uint64_t cs0[N48_WC_B_N], cs1[N48_WC_B_N];
    n48_wc_cen cen;
} n48_wc;

static inline void n48_wc_sub(n48_wc_st *out, const n48_wc_st *cur, const n48_wc_st *snap)
{
    const uint64_t *a = (const uint64_t *)cur, *b = (const uint64_t *)snap;
    uint64_t *o = (uint64_t *)out;
    for (uint32_t i = 0; i < (uint32_t)N48_WC_ST_WORDS; i++) o[i] = a[i] >= b[i] ? a[i] - b[i] : 0ull;
}
static inline void n48_wc_reset(n48_wc *c)
{
    const n48_wc_st z = {};
    c->st = z;
    c->ndis = 0u;
}

/* Open a scope for `owner` in `mode`: 1 = opened (the caller closes it); 0 = OFF (nothing counted) or the kext's one scope is
 * already open - by another thread's concurrent ask, or (rarely) this thread's own - counted as `nested`. */
static inline uint32_t n48_wc_scope_open(n48_wc *c, uint32_t mode, uintptr_t owner)
{
    if (mode != N48_WC_ON && mode != N48_WC_SHADOW) return 0u;
    uint32_t idle = 0u;
    /* claim (0 -> 2), fill, publish (1): a reader that sees 1 sees this scope's owner */
    if (!__atomic_compare_exchange_n(&c->open, &idle, 2u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&c->st.nested, 1ull, __ATOMIC_RELAXED);
        return 0u;
    }
    c->scope++;
    c->mode = mode;
    c->owner = owner;
    c->st.scopes++;
    __atomic_store_n(&c->open, 1u, __ATOMIC_RELEASE);
    return 1u;
}
static inline void n48_wc_scope_close(n48_wc *c, uint32_t hit)
{
    if (hit) c->st.scopes_hit++;
    c->scope++;                    /* no entry of the closed scope can match any later lookup */
    c->owner = 0u;
    __atomic_store_n(&c->open, 0u, __ATOMIC_RELEASE);
}
/* Is the open scope the caller's? (gfxc_page's one test: OFF, `open` is never 1.) */
static inline uint32_t n48_wc_mine(const n48_wc *c, uintptr_t self)
{
    return (__atomic_load_n(&c->open, __ATOMIC_ACQUIRE) == 1u && c->owner == self) ? 1u : 0u;
}

static inline uint32_t n48_wc_ix(const n48_wc_vmk *k, uint64_t gran, uint32_t big)
{
    const uint64_t h = (gran * 0x9e3779b97f4a7c15ull) ^ (k->root_off * 0x100000001b3ull) ^ (k->start_va >> 12) ^ (uint64_t)big;
    return (uint32_t)((h * 0xff51afd7ed558ccdull) >> 56) & (N48_WC_N - 1u);
}
static inline uint32_t n48_wc_same(const n48_wc_ent *e, const n48_wc_vmk *k, uint64_t gran, uint32_t big, uint64_t gen,
                                   uint64_t scope)
{
    return e->valid && e->big == big && e->gran == gran && e->gen == gen && e->scope == scope && e->k.root_off == k->root_off &&
           e->k.start_va == k->start_va && e->k.fb_start == k->fb_start && e->k.vram_size == k->vram_size && e->k.ok == k->ok;
}
/* The walk's own page for `va` under a 64 KiB L1 leaf (gfxc_page_walk: `pageBase = (leafAddr + (va & 0xf000ull)) & ~0xfffull`). */
static inline uint64_t n48_wc_big_page(uint64_t leaf, uint64_t va)
{
    return ((leaf & 0x0000FFFFFFFFF000ull) + (va & 0xf000ull)) & ~0xfffull;
}
/* A lookup at (gen, scope): the 64 KiB granule first, then the 4 KiB page. 1 = hit. */
static inline uint32_t n48_wc_probe(const n48_wc *c, const n48_wc_vmk *k, uint64_t va, uint64_t gen, uint64_t *page,
                                    uint32_t *sys, uint64_t *leaf)
{
    const uint64_t gb = va >> 16, gs = va >> 12;
    const n48_wc_ent *e = &c->e[n48_wc_ix(k, gb, 1u)];
    if (n48_wc_same(e, k, gb, 1u, gen, c->scope)) {
        *page = n48_wc_big_page(e->leaf, va); *sys = e->sys; *leaf = e->leaf;
        return 1u;
    }
    e = &c->e[n48_wc_ix(k, gs, 0u)];
    if (n48_wc_same(e, k, gs, 0u, gen, c->scope)) {
        *page = e->page; *sys = e->sys; *leaf = e->leaf;
        return 1u;
    }
    return 0u;
}
/* Store a SUCCESSFUL walk. `levels` is the walk's own level count (2 = it ended at the L1 leaf: a 64 KiB granule, stored as such
 * only when START is 64 KiB aligned so every page of the granule has the same root index; 3 = a sub-table leaf: its 4 KiB page).
 * Anything else is not stored. 1 = stored. */
static inline uint32_t n48_wc_store(n48_wc *c, const n48_wc_vmk *k, uint64_t va, uint32_t levels, uint64_t gen, uint64_t page,
                                    uint32_t sys, uint64_t leaf)
{
    uint32_t big;
    if (levels == 2u && (k->start_va & 0xffffull) == 0ull && n48_wc_big_page(leaf, va) == page) big = 1u;
    else if (levels == 2u || levels == 3u) big = 0u;
    else return 0u;
    const uint64_t gran = big ? (va >> 16) : (va >> 12);
    n48_wc_ent *e = &c->e[n48_wc_ix(k, gran, big)];
    e->gran = gran; e->k = *k; e->leaf = leaf; e->page = page; e->gen = gen; e->scope = c->scope; e->big = big; e->sys = sys;
    e->valid = 1u;
    return 1u;
}

/* The walk: 1 = resolved (*page, *sys, *leaf, *levels filled), 0 = not. The kext's is gfxc_page_walk (through gfxc_page_pf while
 * switch 96 is ON) over the asking frame's own vm. */
typedef uint32_t (*n48_wc_walk_fn)(void *ctx, uint64_t va, uint64_t *page, uint32_t *sys, uint64_t *leaf, uint32_t *levels);

/* ONE gfxc_page inside an open scope. Returns the answer the caller USES: ON a hit's, otherwise the walk's. *hit says a lookup hit
 * (ON: answered from it; SHADOW: would have been). *dis_ix is N48_WC_DIS, or the index of a disagreement just recorded (the caller
 * logs it). The walk is bracketed by two views of the generation; an unstable bracket stores nothing. */
static inline uint32_t n48_wc_page(n48_wc *c, n48_wc_gen *g, const n48_wc_vmk *k, uint64_t va, n48_wc_walk_fn walk, void *ctx,
                                   uint64_t *page, uint32_t *sys, uint64_t *leaf, uint32_t *hit, uint32_t *dis_ix)
{
    *hit = 0u; *dis_ix = N48_WC_DIS;
    c->st.lookups++;
    uint64_t g0 = 0ull, cp = 0ull, cl = 0ull;
    uint32_t cs = 0u;
    const uint32_t still0 = n48_wc_view(g, &g0);
    const uint32_t cen = c->census;   /* build 0.0.543 item B: read once per walk; 0 = the census reads and writes nothing */
    if (cen) { c->last_m = 0u; n48_wc_sites(g, c->cs0); }
    const uint32_t h = still0 ? n48_wc_probe(c, k, va, g0, &cp, &cs, &cl) : 0u;
    if (h) { c->st.hits++; *hit = 1u; }
    if (h && c->mode == N48_WC_ON) { *page = cp; *sys = cs; *leaf = cl; return 1u; }
    uint64_t wp = 0ull, wl = 0ull;
    uint32_t ws = 0u, lv = 0u;
    const uint32_t ok = walk(ctx, va, &wp, &ws, &wl, &lv) ? 1u : 0u;
    uint64_t g1 = 0ull;
    const uint32_t still1 = n48_wc_view(g, &g1);
    if (cen) n48_wc_sites(g, c->cs1);
    *page = wp; *sys = ws; *leaf = wl;
    if (h) {   /* SHADOW: compare, use the walk's */
        c->st.compared++;
        if (!ok || wp != cp || ws != cs || wl != cl) {
            c->st.disagree++;
            if (c->ndis < N48_WC_DIS) {
                n48_wc_dis *d = &c->dis[c->ndis];
                d->va = va; d->cpage = cp; d->wpage = wp; d->cleaf = cl; d->wleaf = wl; d->csys = cs; d->wsys = ws; d->wok = ok;
                *dis_ix = c->ndis;
                c->ndis++;
            }
        }
        return ok;
    }
    c->st.misses++;
    if (!ok) { c->st.fails++; return 0u; }   /* never cached */
    if (!still0 || !still1 || g0 != g1) {
        c->st.unstable++;
        if (cen) {   /* build 0.0.543 item B: attribution only; the answer and the (non-)store are unchanged */
            uint32_t fl = 0u;
            c->last_m = n48_wc_attr(c->cs0, c->cs1, still0, still1, &fl);
            n48_wc_cen_note(&c->cen, c->last_m, fl);
        }
        return 1u;
    }
    if (n48_wc_store(c, k, va, lv, g0, wp, ws, wl)) c->st.stored++;
    return 1u;
}

/* The report (the verb, the CONTINUOUS STOP): mode, how, then the counters (the STOP's are this arm's deltas), the generation's
 * bumps by site and the disagreements. Each line <= 491 bytes at maximal fields (tests/gfx_wc98_test.cpp). */
#define N48_WC_FMT "walkcache98: switch 98 is %s%s; asks scoped %llu (with a hit %llu, nested %llu); walks asked %llu hits %llu " \
    "misses %llu stored %llu failed %llu unstable %llu; SHADOW compared %llu disagree %llu"
#define N48_WC_ARGS(mode, how, s) n48_wc_mode_name(mode), (how), (unsigned long long)(s)->scopes, \
    (unsigned long long)(s)->scopes_hit, (unsigned long long)(s)->nested, (unsigned long long)(s)->lookups, \
    (unsigned long long)(s)->hits, (unsigned long long)(s)->misses, (unsigned long long)(s)->stored, \
    (unsigned long long)(s)->fails, (unsigned long long)(s)->unstable, (unsigned long long)(s)->compared, \
    (unsigned long long)(s)->disagree
#define N48_WC_GEN_FMT "walkcache98: generation %llu, bumps %llu (unmapVA in %llu out %llu, mapVA in %llu out %llu, copy open %llu " \
    "END %llu, keystone withdraw %llu arm %llu, rebind %llu, ctx release %llu, ws-valid base %llu, SDMA submit %llu); remaps in " \
    "flight now %llu"
#define N48_WC_GEN_ARGS(g) (unsigned long long)__atomic_load_n(&(g)->gen, __ATOMIC_RELAXED), \
    (unsigned long long)n48_wc_bumps(g), (unsigned long long)(g)->site[N48_WC_B_UNMAP_IN], \
    (unsigned long long)(g)->site[N48_WC_B_UNMAP_OUT], (unsigned long long)(g)->site[N48_WC_B_MAP_IN], \
    (unsigned long long)(g)->site[N48_WC_B_MAP_OUT], (unsigned long long)(g)->site[N48_WC_B_CG_OPEN], \
    (unsigned long long)(g)->site[N48_WC_B_CG_END], (unsigned long long)(g)->site[N48_WC_B_KS_WITHDRAW], \
    (unsigned long long)(g)->site[N48_WC_B_KS_ARM], (unsigned long long)(g)->site[N48_WC_B_REBIND], \
    (unsigned long long)(g)->site[N48_WC_B_CTX_RELEASE], (unsigned long long)(g)->site[N48_WC_B_WSV_BASE], \
    (unsigned long long)(g)->site[N48_WC_B_SDMA_SUBMIT], \
    (unsigned long long)(__atomic_load_n(&(g)->in, __ATOMIC_RELAXED) - __atomic_load_n(&(g)->out, __ATOMIC_RELAXED))
/* build 0.0.543 item B (switch 101): the census line (the verb; the CONTINUOUS STOP right after 98's own STOP line, this arm's
 * deltas): per class in flight at a view / sites moved between the views / either, then none and multi, then each bracket class's
 * IN - OUT now. <= 491 bytes at maximal fields (tests/gfx_wc98_test.cpp). */
#define N48_WC_CEN_FMT "walkcache98 census (switch 101 %s)%s: unstable %llu; mapVA in-flight %llu moved %llu any %llu; unmapVA " \
    "in-flight %llu moved %llu any %llu"
#define N48_WC_CEN_ARGS(on, how, c) (on) ? "ON" : "OFF", (how), (unsigned long long)(c)->unstable, \
    (unsigned long long)(c)->inflight[N48_WC_K_MAP], (unsigned long long)(c)->moved[N48_WC_K_MAP], \
    (unsigned long long)(c)->any[N48_WC_K_MAP], (unsigned long long)(c)->inflight[N48_WC_K_UNMAP], \
    (unsigned long long)(c)->moved[N48_WC_K_UNMAP], (unsigned long long)(c)->any[N48_WC_K_UNMAP]
/* The second line (always right after the first). */
#define N48_WC_CEN_FMT2 "walkcache98 census:   copy guard in-flight %llu moved %llu any %llu; other in-flight %llu moved %llu any " \
    "%llu; none %llu multi %llu; in flight now map %llu unmap %llu cg %llu"
#define N48_WC_CEN_ARGS2(c, g) \
    (unsigned long long)(c)->inflight[N48_WC_K_CG], (unsigned long long)(c)->moved[N48_WC_K_CG], \
    (unsigned long long)(c)->any[N48_WC_K_CG], (unsigned long long)(c)->inflight[N48_WC_K_OTHER], \
    (unsigned long long)(c)->moved[N48_WC_K_OTHER], (unsigned long long)(c)->any[N48_WC_K_OTHER], \
    (unsigned long long)(c)->none, (unsigned long long)(c)->multi, \
    (unsigned long long)((g)->site[N48_WC_B_MAP_IN] - (g)->site[N48_WC_B_MAP_OUT]), \
    (unsigned long long)((g)->site[N48_WC_B_UNMAP_IN] - (g)->site[N48_WC_B_UNMAP_OUT]), \
    (unsigned long long)((g)->site[N48_WC_B_CG_OPEN] - (g)->site[N48_WC_B_CG_END])
static inline void n48_wc_cen_sub(n48_wc_cen *out, const n48_wc_cen *cur, const n48_wc_cen *snap)
{
    const uint64_t *a = (const uint64_t *)cur, *b = (const uint64_t *)snap;
    uint64_t *o = (uint64_t *)out;
    for (uint32_t i = 0; i < (uint32_t)N48_WC_CEN_WORDS; i++) o[i] = a[i] >= b[i] ? a[i] - b[i] : 0ull;
}
/* The switch's setter: M 1 ON, M 2 OFF (1 = changed); anything else 0 and *on untouched. */
#define N48_WC_CEN_SWITCH 101u
static inline int n48_wc_cen_set(uint32_t m, uint32_t *on)
{
    if (m != 1u && m != 2u) return 0;
    if (on) *on = m == 1u ? 1u : 0u;
    return 1;
}

/* One SHADOW disagreement (the first N48_WC_DIS): index, VA, then cached page / SYSTEM / leaf vs the walk's (and whether it
 * resolved at all). */
#define N48_WC_DIS_FMT "walkcache98: SHADOW DISAGREEMENT #%u of at most %u: VA %#llx cached page %#llx sys %u leaf %#llx; walked %s " \
    "page %#llx sys %u leaf %#llx (the walked answer was used)"
#define N48_WC_DIS_ARGS(i, d) (i), N48_WC_DIS, (unsigned long long)(d)->va, (unsigned long long)(d)->cpage, (d)->csys, \
    (unsigned long long)(d)->cleaf, (d)->wok ? "ok" : "UNRESOLVED", (unsigned long long)(d)->wpage, (d)->wsys, \
    (unsigned long long)(d)->wleaf

#endif /* N48_GFX_WC98_H */
