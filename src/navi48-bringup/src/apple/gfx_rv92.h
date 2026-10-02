// gfx_rv92.h — build 0.0.536 ( defect 1; switch 92, gfx_commit.h N48_CM_RECT92_FMT). WHICH IMAGE THE RESIDENCY COPY
// WRITES for Apple's gShaderCode_gfx10_RectPosTexFast_VS, pure.
//
// The blob (re/cache/m4c-r20, tools/gfx-cache-build-r20.py) holds TWO substitutable entries for this one Apple program: today's
// (the one sc_lookup verifies, exactly as in m4c-r19) and an ALTERNATIVE whose image reads the vertex ID from v3 - an entry a
// lookup never verifies on its own (its stored Apple bytes run 32 marker dwords past the program, where every real resource holds
// the slot's zero padding). shadercache_hit and ic_hit (Navi48AccelPeer.cpp) render the match this function returns:
//   switch 92 OFF, or any other key         -> `m`, today's image, byte for byte (the function does not even look for an alternative)
//   ON and key N48_RV92_KEY                  -> sc_alt_find(c, m) (shadercache.c): the ONE alternative whose Apple prefix equals the
//                                               bytes `m` verified, same flags / stage / capacity; none (a blob without one) or more
//                                               than one -> `m`, counted N48_RV92_NOALT
// Nothing here writes anything; the caller's refusal ladder (graphics opt-in, capacity, segment, VRAM guard) is unchanged.
#ifndef N48_GFX_RV92_H
#define N48_GFX_RV92_H

#include <stdint.h>
#include "shadercache.h"

#define N48_RV92_KEY   0xe3619022d1d4f224ull   // AMDRadeonX6000MTLDriver (internal):gShaderCode_gfx10_RectPosTexFast_VS
enum { N48_RV92_OLD = 0u, N48_RV92_NEW = 1u, N48_RV92_NOALT = 2u };

static inline const sc_match *n48_rv92_pick(const sc_cache *c, const sc_match *m, uint32_t on, sc_match *alt, uint32_t *why)
{
    if (why) *why = N48_RV92_OLD;
    if (!on || !c || !m || !alt || m->key != N48_RV92_KEY) return m;
    if (sc_alt_find(c, m, alt) != SC_OK) { if (why) *why = N48_RV92_NOALT; return m; }
    if (why) *why = N48_RV92_NEW;
    return alt;
}

#endif /* N48_GFX_RV92_H */
