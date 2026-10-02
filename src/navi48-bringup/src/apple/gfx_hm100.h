// gfx_hm100.h — build 0.0.543 item A ( ranked change A, item 2): SWITCH 100, "hostmap100", THE PER-CALL
// HOST-PAGE MAP CACHE.
//
// WHY.'s per-call breakdown (AR/AR2/AS): HOST-PAGE MAPS are 3.5-3.7 ms of a ~15 ms GFX hook call (22-24%): gfxc_read_core and
// gfxc_write_sys create an IOMemoryDescriptor, prepare, map, copy, release, complete and release PER 4 KiB HOST PAGE, ~42k per arm
// (run11ak `host maps (n/us) read 30807/3520879 write 11261/1386065`, ~117 us each), and perf540's shadow said 57% of them would hit
// a cache that lives for one call (`shadow host-page cache would-hit per call 23836 any 25926 of 42068`).
//
// WHAT. ONE table of at most N48_HM_N {physical page, descriptor, map, kernel VA} entries, OPEN only for the duration of ONE GFX hook
// call (hook_gfxCommitIB_timed: an RAII scope opened at its first statement and closed by the destructor on every return), and
// usable ONLY by the thread that opened it. Keyed by the PHYSICAL page the caller's own page walk just resolved - never by the VA:
// the walk and the RAM-range guard (n48_rt_refuse) still run on EVERY access, before the cache is asked, so a VA remapped mid-call
// resolves to its new page and finds (or maps) THAT page. An entry is inserted only after the guard passed (n48_hm_get refuses to
// look up or map a guard-refused page at all); a full table falls back to today's map-per-page path (that map is released after its
// one use); every entry is released at the scope's close. Maps are made kIODirectionOutIn so a later WRITE of the same page (the
// commit's gfxc_write_sys) may use them; n48_hm_insert refuses any other direction.
//
// MODES (`accel gfxneuter 100 | M << 8`): M 1 ON (= 356) serves reads and writes from the table; M 2 OFF (= 612, the default and the
// boot value) never opens a scope, so gfxc_read_core / gfxc_write_sys are 0.0.542's code; M 3 SHADOW (= 868) keeps the table (so it
// measures the hit rate and the map count) but USES a fresh map for every access, compares the dwords a read got against the cached
// map's, and after a write compares the cached map's view with what was written; every difference is counted (the first N48_HM_DIS
// recorded). Joins the continuous mid-arm guard (gfx_commit.h), so it is constant for an arm.
//
// NOT DONE HERE (a noted follow-up, per the brief): reading host pages with IOMappedRead64 (no map at all). The tree does not prove
// that no system IOMapper (DART/VT-d remapping) sits between a physical address and the page, so that step is left out.
//
// PURE: no kext symbol, no register, nothing of Apple's is written. The map/release are callbacks. Compiled into the kext and into
// tests/gfx_hm100_test.cpp (fake physical memory, randomized remap schedules, owner/non-owner threads, every exit path).
#ifndef N48_GFX_HM100_H
#define N48_GFX_HM100_H

#include <stdint.h>

enum { N48_HM_ON = 1u, N48_HM_OFF = 2u, N48_HM_SHADOW = 3u };   /* the switch's M, held as is; N48_HM_OFF at boot */
#define N48_HM_SWITCH 100u
#define N48_HM_N   128u     /* entries per call (a full table falls back to the uncached path) */
#define N48_HM_DIS 8u       /* SHADOW differences recorded (the first N48_HM_DIS since the counters were reset) */
/* The direction a cached map was made with. Only OUTIN may be cached: a write may later use the entry. */
enum { N48_HM_DIR_IN = 1u, N48_HM_DIR_OUTIN = 3u };

/* The switch's setter: M 1, 2, 3 set the mode (1 = changed); anything else 0 and *mode untouched. */
static inline int n48_hm_set(uint32_t m, uint32_t *mode)
{
    if (m != N48_HM_ON && m != N48_HM_OFF && m != N48_HM_SHADOW) return 0;
    if (mode) *mode = m;
    return 1;
}
static inline const char *n48_hm_mode_name(uint32_t mode)
{
    return mode == N48_HM_ON ? "ON (356: a GFX call's host pages are mapped once and reused within the call)"
         : mode == N48_HM_SHADOW ? "SHADOW (868: the table is kept, every access uses a fresh map and compares)"
         : "OFF (612, default: one IOMemoryDescriptor map per host page per access)";
}

typedef struct {
    uint64_t page;        /* the physical page (4 KiB aligned) */
    uint64_t kva;         /* the map's kernel virtual address (page base) */
    void *md, *map;       /* opaque to this header: the kext's IOMemoryDescriptor and IOMemoryMap */
    uint32_t dir, used;
} n48_hm_ent;

typedef struct { uint64_t page; uint32_t off, dw, cached, fresh, write, pad; } n48_hm_dis;

/* Counters. EVERY FIELD A uint64_t (n48_hm_sub is element-wise). */
typedef struct {
    uint64_t scopes;       /* GFX calls that opened a scope (ON or SHADOW) */
    uint64_t nested;       /* a call found THE scope already open (another thread's concurrent call, or this thread's own) */
    uint64_t lookups;      /* host-page accesses by the owner inside its scope */
    uint64_t hits;         /* answered from the table (ON) / would have been (SHADOW) */
    uint64_t maps;         /* maps made by the owner inside its scope (misses, SHADOW's fresh maps and fallbacks) */
    uint64_t inserted;     /* ... kept in the table */
    uint64_t full;         /* a miss the full table could not keep (mapped and released after its one use) */
    uint64_t released;     /* entries released at a scope's close */
    uint64_t guard;        /* accesses refused by the RAM-range guard before any lookup */
    uint64_t notowner;     /* accesses while ANOTHER thread's scope was open (uncached, today's path) */
    uint64_t mapfail;      /* map callback failures */
    uint64_t dirrefused;   /* inserts refused for a direction that is not OUTIN (never expected) */
    uint64_t compared;     /* SHADOW: accesses compared (reads and writes) */
    uint64_t differ;       /* SHADOW: ... that differed */
    uint64_t wr_hits;      /* writes answered from the table (ON) / that found an entry (SHADOW) */
} n48_hm_st;
#define N48_HM_ST_WORDS (sizeof(n48_hm_st) / sizeof(uint64_t))

typedef struct {
    n48_hm_ent e[N48_HM_N];
    uint32_t n;            /* entries in use (e[0..n)) */
    uint32_t open;         /* 0 idle, 2 being claimed, 1 open */
    uint32_t mode;         /* latched at the scope's open */
    uint32_t pad;
    uintptr_t owner;       /* the thread that opened it */
    n48_hm_st st;
    n48_hm_dis dis[N48_HM_DIS];
    uint32_t ndis, pad2;
} n48_hm;

static inline void n48_hm_sub(n48_hm_st *out, const n48_hm_st *cur, const n48_hm_st *snap)
{
    const uint64_t *a = (const uint64_t *)cur, *b = (const uint64_t *)snap;
    uint64_t *o = (uint64_t *)out;
    for (uint32_t i = 0; i < (uint32_t)N48_HM_ST_WORDS; i++) o[i] = a[i] >= b[i] ? a[i] - b[i] : 0ull;
}
static inline void n48_hm_reset(n48_hm *c)
{
    const n48_hm_st z = {};
    c->st = z;
    c->ndis = 0u;
}

/* Open the call's scope for `owner` in `mode`: 1 = opened (the caller closes it); 0 = OFF (nothing counted) or the one scope is
 * already open (counted `nested`; the caller opens nothing and closes nothing). */
static inline uint32_t n48_hm_open(n48_hm *c, uint32_t mode, uintptr_t owner)
{
    if (mode != N48_HM_ON && mode != N48_HM_SHADOW) return 0u;
    uint32_t idle = 0u;
    if (!__atomic_compare_exchange_n(&c->open, &idle, 2u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        __atomic_fetch_add(&c->st.nested, 1ull, __ATOMIC_RELAXED);
        return 0u;
    }
    c->n = 0u;
    c->mode = mode;
    c->owner = owner;
    c->st.scopes++;
    __atomic_store_n(&c->open, 1u, __ATOMIC_RELEASE);
    return 1u;
}
/* Is the open scope the caller's? (OFF: `open` is never 1.) */
static inline uint32_t n48_hm_mine(const n48_hm *c, uintptr_t self)
{
    return (__atomic_load_n(&c->open, __ATOMIC_ACQUIRE) == 1u && c->owner == self) ? 1u : 0u;
}

/* The kext's two IOKit steps. mk: map ONE physical 4 KiB page in direction `dir` (N48_HM_DIR_*): 1 = *md, *map, *kva filled (the
 * page base's kernel VA). rel: complete and release what mk made (the same direction). */
typedef uint32_t (*n48_hm_mk_fn)(void *ctx, uint64_t page, uint32_t dir, void **md, void **map, uint64_t *kva);
typedef void (*n48_hm_rel_fn)(void *ctx, void *md, void *map, uint32_t dir);

/* Close the scope: release EVERY entry (in insertion order), then publish closed. Returns the entries released. Only the owner
 * closes (a non-owner call is refused and changes nothing). */
static inline uint32_t n48_hm_close(n48_hm *c, uintptr_t self, n48_hm_rel_fn rel, void *ctx)
{
    if (__atomic_load_n(&c->open, __ATOMIC_ACQUIRE) != 1u || c->owner != self) return 0u;
    uint32_t r = 0u;
    for (uint32_t i = 0; i < c->n && i < N48_HM_N; i++) {
        n48_hm_ent *e = &c->e[i];
        if (!e->used) continue;
        if (rel) rel(ctx, e->md, e->map, e->dir);
        e->used = 0u; e->md = 0; e->map = 0; e->kva = 0ull; e->page = 0ull;
        r++;
    }
    c->st.released += r;
    c->n = 0u;
    c->owner = 0u;
    __atomic_store_n(&c->open, 0u, __ATOMIC_RELEASE);
    return r;
}

/* The owner's lookup: the entry for `page`, or null. */
static inline const n48_hm_ent *n48_hm_find(const n48_hm *c, uint64_t page)
{
    for (uint32_t i = 0; i < c->n && i < N48_HM_N; i++)
        if (c->e[i].used && c->e[i].page == page) return &c->e[i];
    return 0;
}
/* The owner's insert of a map it just made: 1 = kept (released at close); 0 = the table is full or the direction is not OUTIN
 * (the caller releases it after its one use). */
static inline uint32_t n48_hm_insert(n48_hm *c, uint64_t page, uint32_t dir, void *md, void *map, uint64_t kva)
{
    if (dir != N48_HM_DIR_OUTIN) { c->st.dirrefused++; return 0u; }
    if (c->n >= N48_HM_N) { c->st.full++; return 0u; }
    n48_hm_ent *e = &c->e[c->n];
    e->page = page; e->kva = kva; e->md = md; e->map = map; e->dir = dir; e->used = 1u;
    c->n++;
    c->st.inserted++;
    return 1u;
}

/* One host page, as the caller will use it: `kva` the page base's kernel VA; `rel` 1 = the caller releases (md, map, dir) after its
 * one use (an uncached map); `shadow_kva` (SHADOW, owner, entry present) the cached map to compare against, else 0. */
typedef struct { uint64_t kva, shadow_kva; void *md, *map; uint32_t dir, rel; } n48_hm_use;

/* THE ACCESS. Called for a HOST page after the caller's walk resolved it, with the RAM-range guard's verdict for THAT page
 * (`guard_rc` 0 = passed). 1 = `*u` names a mapping to use; 0 = refused (the guard, or the map failed) - nothing to release.
 *   guard_rc != 0          refused BEFORE any lookup, insert or map (counted `guard` for the owner).
 *   not the owner's scope  today's path: a fresh map in `dir_uncached` (the caller's own direction, exactly 0.0.542's), released
 *                          after use (counted `notowner` when another thread's scope is open).
 *   owner, ON              a hit is used as is; a miss maps OUTIN and inserts it (a full table: used once and released).
 *   owner, SHADOW          always a fresh OUTIN map for the use; a hit also names the cached map (shadow_kva) for the compare; a
 *                          miss's fresh map is kept (the table measures what ON would hold) unless the table is full.
 * `write` says whether the caller will write the page (counted; the direction is OUTIN for every owner map either way). */
static inline uint32_t n48_hm_get(n48_hm *c, uintptr_t self, uint64_t page, uint32_t guard_rc, uint32_t write, uint32_t dir_uncached,
                                  n48_hm_mk_fn mk, void *ctx, n48_hm_use *u)
{
    u->kva = 0ull; u->shadow_kva = 0ull; u->md = 0; u->map = 0; u->dir = dir_uncached; u->rel = 0u;
    const uint32_t mine = n48_hm_mine(c, self);
    if (guard_rc) { if (mine) c->st.guard++; return 0u; }
    if (!mine) {
        if (__atomic_load_n(&c->open, __ATOMIC_ACQUIRE) == 1u) __atomic_fetch_add(&c->st.notowner, 1ull, __ATOMIC_RELAXED);
        if (!mk(ctx, page, dir_uncached, &u->md, &u->map, &u->kva)) return 0u;
        u->rel = 1u;
        return 1u;
    }
    c->st.lookups++;
    const n48_hm_ent *e = n48_hm_find(c, page);
    if (e) { c->st.hits++; if (write) c->st.wr_hits++; }
    if (e && c->mode == N48_HM_ON) { u->kva = e->kva; u->dir = e->dir; return 1u; }
    u->dir = N48_HM_DIR_OUTIN;
    if (!mk(ctx, page, N48_HM_DIR_OUTIN, &u->md, &u->map, &u->kva)) { c->st.mapfail++; return 0u; }
    c->st.maps++;
    if (e) { u->shadow_kva = e->kva; u->rel = 1u; return 1u; }   /* SHADOW hit: use the fresh map, compare against the entry */
    if (!n48_hm_insert(c, page, N48_HM_DIR_OUTIN, u->md, u->map, u->kva)) u->rel = 1u;
    return 1u;
}

/* SHADOW's compare of `n` dwords at byte offset `off` of the page: `got` what the use read or wrote, `cached` the cached map's page
 * base (volatile reads). Counts and records the first difference; 1 = equal. */
static inline uint32_t n48_hm_compare(n48_hm *c, uint64_t page, uint32_t off, const uint32_t *got, uint64_t cached, uint32_t n,
                                      uint32_t write)
{
    c->st.compared++;
    const volatile uint32_t *cv = (const volatile uint32_t *)(uintptr_t)(cached + off);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t v = cv[i];
        if (v != got[i]) {
            c->st.differ++;
            if (c->ndis < N48_HM_DIS) {
                n48_hm_dis *d = &c->dis[c->ndis++];
                d->page = page; d->off = off + i * 4u; d->dw = i; d->cached = v; d->fresh = got[i]; d->write = write;
            }
            return 0u;
        }
    }
    return 1u;
}

/* The report (the verb, the CONTINUOUS STOP): mode, how, then the counters (the STOP's are this arm's deltas). Each line <= 491
 * bytes at maximal fields (tests/gfx_hm100_test.cpp). */
#define N48_HM_FMT "hostmap100: switch 100 is %s%s; calls scoped %llu nested %llu; host accesses %llu hits %llu (writes %llu) maps " \
    "%llu kept %llu"
#define N48_HM_ARGS(mode, how, s) n48_hm_mode_name(mode), (how), (unsigned long long)(s)->scopes, (unsigned long long)(s)->nested, \
    (unsigned long long)(s)->lookups, (unsigned long long)(s)->hits, (unsigned long long)(s)->wr_hits, \
    (unsigned long long)(s)->maps, (unsigned long long)(s)->inserted
/* The second line (always printed right after the first): the fallbacks, refusals and SHADOW's compare. */
#define N48_HM_FMT2 "hostmap100:   full %llu released %llu; guard-refused %llu other-thread %llu map-failed %llu dir-refused %llu; " \
    "SHADOW compared %llu differ %llu"
#define N48_HM_ARGS2(s) (unsigned long long)(s)->full, (unsigned long long)(s)->released, (unsigned long long)(s)->guard, \
    (unsigned long long)(s)->notowner, (unsigned long long)(s)->mapfail, (unsigned long long)(s)->dirrefused, \
    (unsigned long long)(s)->compared, (unsigned long long)(s)->differ
#define N48_HM_DIS_FMT "hostmap100: SHADOW DIFFERENCE #%u of at most %u: page %#llx +%#x (dword %u of the access, a %s): cached map " \
    "%#010x, fresh map %#010x (the fresh map's value was used)"
#define N48_HM_DIS_ARGS(i, d) (i), N48_HM_DIS, (unsigned long long)(d)->page, (d)->off, (d)->dw, (d)->write ? "write" : "read", \
    (d)->cached, (d)->fresh

#endif /* N48_GFX_HM100_H */
