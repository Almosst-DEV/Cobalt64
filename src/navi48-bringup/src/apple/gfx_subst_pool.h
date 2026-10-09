// gfx_subst_pool.h — build 0.0.484 (an internal design note Q2 K2, (2)). THE RECOGNITION POOL'S BUILD, pure.
//
// gfxsrc_xlat_open (AppleHardwareHook.cpp) renders, once, the image every substitutable shader-cache entry writes over
// Apple's program, so a byte compare at a program VA can say "these bytes are already OURS" (n48_xd_ours_find,
// gfx_subst_caps.h). Through 0.0.483 its loop stopped at the cap (`gXdSubstN < kXdSubstMax`) and said nothing about the
// entries past it: r17 holds 90 substitutable entries against a cap of 96, r18 will hold 92-93, and a blob that grew
// past the cap would silently leave its last programs unrecognisable - they would read program-unknown on hardware
// with no line saying why. The loop is here now, and it COUNTS every substitutable entry it never rendered because the
// pool was full (`dropped`), which the kext prints on its open line and in its boot guard.
//
// Same order, same refusals as the loop it replaces: entries in blob index order, a non-substitutable entry skipped,
// sc_subst_image into row n of the pool, SC_E_CAPACITY counted as `tooBig`, any other refusal as `refused`, and only a
// rendered image (nb > 0) takes a row. With `pool` NULL nothing is rendered and nothing is written to `rows`: the call
// only counts `substitutable` and `dropped` - the boot guard's census, which must not allocate the 672 KiB pool on a
// boot that never arms the decision. Census `dropped` assumes every substitutable entry renders, so it is an UPPER
// BOUND on the real build's (exact whenever nothing is refused; a refused entry frees its row for a later one).
#ifndef N48_GFX_SUBST_POOL_H
#define N48_GFX_SUBST_POOL_H

#include "gfx_subst_caps.h"
#include "shadercache.h"

typedef struct {
    uint32_t substitutable;   // entries carrying SC_F_SUBSTITUTE, all of them, rendered or not
    uint32_t tooBig;          // sc_subst_image refused SC_E_CAPACITY: the image is wider than one N48_XD_SUBST_BYTES row
    uint32_t refused;         // any other refusal (or an empty image)
    uint32_t dropped;         // substitutable entries NEVER TRIED because `cap` rows were already rendered
} n48_xd_subst_counts;

// Renders into pool rows [0, return) and fills rows[] alike; `cap` is the pool's row count (the kext passes
// kXdSubstMax = N48_XD_SUBST_MAX, the rows its IOMalloc holds). `k` is zeroed first.
static inline uint32_t n48_xd_subst_build(const sc_cache *c, uint8_t *pool, n48_xd_subst_row *rows, uint32_t cap,
                                          n48_xd_subst_counts *k)
{
    uint32_t n = 0;
    k->substitutable = 0u; k->tooBig = 0u; k->refused = 0u; k->dropped = 0u;
    const uint32_t ne = sc_entry_count(c);
    for (uint32_t i = 0; i < ne; i++) {
        sc_match m;
        memset(&m, 0, sizeof m);
        if (sc_entry_at(c, i, &m) != SC_OK) continue;
        if (!(m.flags & SC_F_SUBSTITUTE)) continue;
        k->substitutable++;
        if (n >= cap) { k->dropped++; continue; }
        if (!pool) { n++; continue; }                     // census only: this entry WOULD take a row
        uint32_t nb = 0;
        const int rst = sc_subst_image(c, &m, pool + (size_t)n * N48_XD_SUBST_BYTES, N48_XD_SUBST_BYTES, &nb);
        if (rst != SC_OK || !nb) { if (rst == SC_E_CAPACITY) k->tooBig++; else k->refused++; continue; }
        if (rows) { rows[n].key = m.key; rows[n].bytes = nb; rows[n].stage = m.stage; }
        n++;
    }
    return n;
}

#endif /* N48_GFX_SUBST_POOL_H */
