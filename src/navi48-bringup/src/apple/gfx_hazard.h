/* gfx_hazard.h — 0.0.395, an internal design note v2 "Capacity — (a)" and "Keying is PHYSICAL, not VA":
 * THE ARM-SCOPED HAZARD PAGE SET, keyed by the RESOLVED PHYSICAL PAGE.
 *
 * WHY THIS HEADER EXISTS, IN ONE PARAGRAPH. Through 0.0.394 R5's memory-destination question ("did a held-back frame
 * write a page THIS consumer points at?") was answered by walking `n48_cp_ring`'s per-frame rows and comparing VAs
 * (`n48_cp_is_memdst`). A VA-keyed answer is unsound across two processes that share one IOSurface under different VAs,
 * and the 32-row ring OVERFLOWED 831 TIMES on arm20's own boot, so the record it answered from was
 * incomplete in exactly the fail-open direction. The memo replaces the ring's role with a page set:
 *
 *   - a 64-ROW EXACT TABLE (page, VA, hits, first namer) that is an INSTRUMENT and the only COUNT source (arm21 line 4:
 *     "The count comes from the exact table's `hits`, never the filter, which answers membership only"); and
 *   - a 1024-BIT TWO-HASH FILTER that is the MEMBERSHIP AUTHORITY. A filter cannot overflow, so no capacity can refuse
 *     for that reason; its only imprecision is a FALSE POSITIVE, which refuses MORE (the safe direction), never fewer.
 *     The memo measured occupancy at ~23 target pages on the real window = ~4% of 1024 bits.
 *
 * FAIL-CLOSED BY CONSTRUCTION, three ways, and each is a mutant the R5′ suite plants:
 *   1. a NULL/absent set answers HIT (no set is not an empty set);
 *   2. a page of 0 - an UNRESOLVED destination - answers HIT, so an unresolved destination can never read as "not named";
 *   3. membership needs BOTH hash bits set, and adding a page sets both. There is no branch that clears a bit.
 *
 * Pure C (`gfx_dep.h` is compiled as C by some of its consumers and this file is included from it), no allocation, no
 * static state of its own. Host-tested by tests/gfx_dep_test.cpp.
 *
 * 0.0.397: `n48_hz_hit` is the ONE membership question and it now COUNTS itself, into `queries`/`hits`.
 * Through 0.0.396 neither was ever incremented, so arm21 line 2 printed `queries 0, hits 0` however often R3 asked -
 * a liar. The counting is on the passed struct, so the header still owns no state; the answer itself is unchanged.
 *
 * WHAT IS **NOT** HERE. The per-frame decode - what a colour target is, what a memory destination is, how a dispatch's
 * V# is read - lives in gfx_dep.h beside `n48_cp_scan_frame`, which this file does not include. This file owns only the
 * set and the two hashes. */
#ifndef N48_GFX_HAZARD_H
#define N48_GFX_HAZARD_H

#include <stdint.h>

#define N48_HZ_ROWS        64u     /* the exact table: instrument and the ONLY count source (arm21 line 4) */
#define N48_HZ_FILTER_BITS 1024u   /* the membership authority: cannot overflow, false positives only */
#define N48_HZ_FILTER_WORDS (N48_HZ_FILTER_BITS / 32u)

typedef struct {
    uint64_t page;    /* the RESOLVED physical page (4 KiB aligned); the key */
    uint64_t va;      /* the VA the first namer named it at - an instrument row, never part of the key */
    uint64_t hits;    /* frames that named this page */
    uint64_t namer;   /* the judged-frame number of the first namer (0 = none supplied) */
} n48_hz_row;

typedef struct {
    n48_hz_row row[N48_HZ_ROWS];
    uint32_t used;
    uint64_t over;      /* pages that did not fit the exact table. INSTRUMENT ONLY: the filter still answers them */
    uint64_t added;     /* pages added (including duplicates) */
    uint64_t dups;      /* adds of a page an exact row already holds */
    uint64_t queries;   /* membership questions asked */
    uint64_t hits;      /* ... answered HIT */
    uint32_t filter[N48_HZ_FILTER_WORDS];   /* 1024 bits; bit set = present. THE AUTHORITY */
    uint32_t arm_seq;   /* the arm the set belongs to; 0 = not scoped, and nothing is re-based */
    uint64_t rebases;   /* scope moves - an instrument, printed, never a rung */
} n48_hazard;

/* The two hashes. A plain 64-bit finaliser each, applied to the page; deterministic, no state, no allocation. */
static inline uint32_t n48_hz_h1(uint64_t p)
{
    p ^= p >> 33; p *= 0xff51afd7ed558ccdull; p ^= p >> 33; p *= 0xc4ceb9fe1a85ec53ull; p ^= p >> 33;
    return (uint32_t)(p & (uint64_t)(N48_HZ_FILTER_BITS - 1u));
}

static inline uint32_t n48_hz_h2(uint64_t p)
{
    p ^= p >> 29; p *= 0x9e3779b97f4a7c15ull; p ^= p >> 32; p *= 0xbf58476d1ce4e5b9ull; p ^= p >> 29;
    return (uint32_t)((p >> 11) & (uint64_t)(N48_HZ_FILTER_BITS - 1u));
}

/* A scope move (a new arm) re-bases the whole set, exactly as the witness and the ring are re-based. `seq` 0 is not a
 * scope and does nothing. Returns 1 when it re-based. */
static inline uint32_t n48_hz_scope(n48_hazard *hz, uint32_t seq)
{
    if (!hz || seq == 0u || hz->arm_seq == seq) return 0u;
    hz->arm_seq = seq;
    hz->used = 0u; hz->over = 0u; hz->added = 0u; hz->dups = 0u; hz->queries = 0u; hz->hits = 0u;
    for (uint32_t i = 0; i < N48_HZ_ROWS; i++) {
        hz->row[i].page = 0ull; hz->row[i].va = 0ull; hz->row[i].hits = 0ull; hz->row[i].namer = 0ull;
    }
    for (uint32_t i = 0; i < N48_HZ_FILTER_WORDS; i++) hz->filter[i] = 0u;
    hz->rebases++;
    return 1u;
}

/* Add one RESOLVED destination page. `page` 0 (unresolved) is deliberately NOT added: an unresolved destination is
 * handled by the caller as a blind frame, and adding page 0 would poison every later membership question with a hit on
 * the zero page. Both filter bits are set; the exact row is appended while there is room and otherwise counted in `over`
 * - which never refuses, because the filter remains the authority and cannot overflow. */
static inline void n48_hz_add(n48_hazard *hz, uint64_t page, uint64_t va, uint64_t namer)
{
    if (!hz || page == 0ull) return;
    const uint64_t key = page & ~0xFFFull;
    hz->added++;
    hz->filter[n48_hz_h1(key) >> 5] |= 1u << (n48_hz_h1(key) & 31u);
    hz->filter[n48_hz_h2(key) >> 5] |= 1u << (n48_hz_h2(key) & 31u);
    for (uint32_t i = 0; i < hz->used && i < N48_HZ_ROWS; i++)
        if (hz->row[i].page == key) { hz->row[i].hits++; hz->dups++; return; }
    if (hz->used >= N48_HZ_ROWS) { hz->over++; return; }
    n48_hz_row *r = &hz->row[hz->used++];
    r->page = key; r->va = va; r->hits = 1ull; r->namer = namer;
}

/* The membership ANSWER, without counting - the same two bits, for a caller that is an instrument and must not move the
 * counters (0.0.397,: the report's own filter probe is read-only). Pure; no set -> HIT; page 0 -> HIT. */
static inline uint32_t n48_hz_probe(const n48_hazard *hz, uint64_t page)
{
    if (!hz) return 1u;
    if (page == 0ull) return 1u;
    const uint32_t b1 = n48_hz_h1(page & ~0xFFFull), b2 = n48_hz_h2(page & ~0xFFFull);
    const uint32_t w1 = hz->filter[b1 >> 5], w2 = hz->filter[b2 >> 5];
    return ((w1 & (1u << (b1 & 31u))) && (w2 & (1u << (b2 & 31u)))) ? 1u : 0u;
}

/* Membership. FAIL-CLOSED: no set -> HIT; page 0 -> HIT; otherwise membership is BOTH bits set. A false positive can
 * only come from the set being MORE populated than the truth, so it refuses more - the direction this rule is built in.
 * 0.0.397: NON-CONST - the question counts itself, through `n48_hz_probe` so the answer is defined once.
 * `queries` moves on every ask of a present set, `hits` on every HIT (including page 0's fail-closed HIT). A null set
 * answers HIT and cannot count, because there is no set to count on. The counters are written under the caller's lock
 * (the decide path's `gXdLock`); an instrument that must not move them uses `n48_hz_probe` instead. */
static inline uint32_t n48_hz_hit(n48_hazard *hz, uint64_t page)
{
    if (!hz) return 1u;
    const uint32_t r = n48_hz_probe(hz, page);
    hz->queries++;
    if (r) hz->hits++;
    return r;
}

/* The exact table's count for a page, and 0 when the page is not an exact row. `EXACT` beside it says whether the filter
 * holds the page at all; FILTER-ONLY is a page the filter answers but the table lost to `over`, which has no number. */
static inline uint64_t n48_hz_count(const n48_hazard *hz, uint64_t page, uint32_t *exact)
{
    if (exact) *exact = 0u;
    if (!hz) return 0ull;
    const uint64_t key = page & ~0xFFFull;
    for (uint32_t i = 0; i < hz->used && i < N48_HZ_ROWS; i++)
        if (hz->row[i].page == key) { if (exact) *exact = 1u; return hz->row[i].hits; }
    return 0ull;
}

/* Bits set of 1024 - the occupancy the memo measured at ~4%. An instrument. */
static inline uint32_t n48_hz_bits(const n48_hazard *hz)
{
    if (!hz) return 0u;
    uint32_t n = 0u;
    for (uint32_t i = 0; i < N48_HZ_FILTER_WORDS; i++) {
        uint32_t w = hz->filter[i];
        while (w) { n += (w & 1u); w >>= 1; }
    }
    return n;
}

#endif /* N48_GFX_HAZARD_H */
