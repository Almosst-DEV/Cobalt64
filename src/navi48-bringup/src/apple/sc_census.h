// sc_census.h — INSTRUMENT (0.0.268, , test A): fingerprint every terminated
// program in a scanned resource, hit or miss, so a WindowServer run leaves a list of the
// shaders it uploaded and which of them our cache does not hold. Milestone 4's work list.
//
// Why a census beside sc_scan_window: the library only calls back on MATCHES, and a code
// longer than the cache's own longest key (gSc.max_key_dwords) lands in "no terminator"
// before any key is computed. So an unknown compositor shader would be counted and never
// named. The census walks the SAME owned grid starts with its own, larger extent cap and
// reports, for each start whose first dword is non-zero and which reaches SC_TERMINATOR:
//   - the UNMASKED key: sc_key(code, dwords, no mask) — a stable fingerprint of the bytes,
//     NOT necessarily the cache's key (cache entries may mask relocated dwords);
//   - the size in dwords, the first dword, and the cache's own verdict (sc_lookup).
// Nothing is written anywhere: it reads the window the scan already read.
//
// A PURE FUNCTION over a buffer (no kernel types, no globals), so tests/sc_census_test.cpp
// drives it on the host with the identical code the kext runs. `c` may be NULL, in which
// case no lookup is made and status is SC_MISS with miss_reason 0.

#ifndef NAVI48_SC_CENSUS_H
#define NAVI48_SC_CENSUS_H

#include <stddef.h>
#include <stdint.h>
#include "shadercache.h"

typedef struct sc_census_item {
    uint64_t offset;        // resource offset of the program's first dword (base + o)
    uint64_t key;           // unmasked FNV-1a-64 over u32le(dwords) || code
    uint32_t dwords;        // through and including SC_TERMINATOR
    uint32_t first;         // the first dword
    int      status;        // sc_lookup's status (SC_OK hit, SC_MISS, or a refusal)
    uint32_t miss_reason;   // sc_match.miss_reason when status is SC_MISS
    uint64_t cache_key;     // sc_match.key (what the cache keyed on, masked or not)
} sc_census_item;

typedef struct sc_census_counts {
    uint32_t starts;        // owned grid starts visited
    uint32_t empty;         // first dword zero
    uint32_t unterminated;  // no SC_TERMINATOR within max_dwords and the bytes available
    uint32_t programs;      // reported to the callback
} sc_census_counts;

// callback: return 0 to continue, nonzero to stop this window
typedef int (*sc_census_fn)(void *ctx, const sc_census_item *item);

static inline uint32_t sc_census_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// buf/len: the window; owned: the leading bytes this window owns (grid starts in [0, owned));
// base: the window's resource offset; grid: the grid (0 or <4 is treated as 4).
static inline void sc_census_window(const sc_cache *c, const uint8_t *buf, size_t len,
                                    size_t owned, uint32_t grid, uint32_t max_dwords,
                                    uint64_t base, sc_census_fn cb, void *ctx,
                                    sc_census_counts *counts)
{
    size_t o;
    if (!buf || !counts) return;
    if (grid < 4u) grid = 4u;
    if (owned > len) owned = len;
    for (o = 0; o + 4u <= len && o < owned; o += grid) {
        sc_census_item it;
        uint32_t dw = 0;
        counts->starts++;
        it.first = sc_census_rd32(buf + o);
        if (it.first == 0) { counts->empty++; continue; }
        if (sc_extent(buf + o, len - o, max_dwords, &dw) != SC_OK || dw == 0) {
            counts->unterminated++;
            continue;
        }
        it.offset = base + (uint64_t)o;
        it.dwords = dw;
        it.key = sc_key(buf + o, dw, 0, 0);
        it.status = SC_MISS;
        it.miss_reason = 0;
        it.cache_key = 0;
        if (c) {
            sc_match m;
            m.key = 0;          // sc_lookup zeroes m on every path except SC_E_ARG
            m.miss_reason = 0;
            it.status = sc_lookup(c, buf + o, len - o, &m);
            it.miss_reason = (it.status == SC_MISS) ? m.miss_reason : 0;
            it.cache_key = m.key;
        }
        counts->programs++;
        if (cb && cb(ctx, &it)) break;
    }
}

#endif // NAVI48_SC_CENSUS_H
