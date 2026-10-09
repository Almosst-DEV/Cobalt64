// gfx_cgredo.h — build 0.0.523 (an internal design note rev 2 item 10, switch 78): THE REDO's UNDO, PURE.
//
// What a refused segment's translate left behind, taken back EXACTLY as gfxsrc_defer_take (AppleHardwareHook.cpp) takes a
// deferred unit's first attempt back, so the redo re-translates from the state the first attempt started from:
//   1. the candidate bytes: Apple's input copied back over [from, to) of the candidate buffer (a refusal's restore);
//   2. the unit's records: n48_mib_unit_undo2 with the copy guard's status - the frame pool's journal AND 0.0.522's spill
//      tier's (U->spill), so no spill record of the first translation outlives it (reviewer item C1);
//   3. the frame-local list: only the ADDITIONS the attempt made are taken back (n48_mib_defer_undo_adds - an entry the
//      attempt REMOVED stays removed, the 0.0.508 LOW-1 rule), and the carry is restored from the snapshot.
// The two program memos (n48_pm_clear) are cleared by the kext beside this call. Nothing here touches a register, a page
// table or anything of Apple's: the candidate buffer, the pool/spill shadows and the list are all kext-owned.
#ifndef N48_GFX_CGREDO_H
#define N48_GFX_CGREDO_H

#include <stdint.h>
#include "gfx_copyguard.h"   // N48_SEG_COPY_OVERLAP
#include "gfx_mib.h"         // n48_mib_unit_undo2
#include "gfx_unitdefer.h"   // n48_mib_defer_snap, n48_mib_defer_copy, n48_mib_defer_undo_adds

// The snapshot taken BEFORE each segment's first translate while 78 is ON (static in the kext: ~3 KiB).
typedef struct {
    n48_mib_defer_snap fl;   // the frame-local list's entries and the carry
    uint32_t k;              // the segment it was taken for
    uint32_t valid;
} n48_cg_redo_pre;

static inline void n48_cg_redo_snap(n48_cg_redo_pre *p, uint32_t k, const n48_dl *fl, const xlat12_ud_carry *carry) {
    if (!p) return;
    n48_mib_defer_copy(&p->fl, fl, carry);
    p->k = k;
    p->valid = 1u;
}

// Returns the pool + spill dwords undone. `pre` must be the snapshot taken for THIS segment (else the list is left alone and
// the caller must not redo: n48_cg_redo_undo_ok answers that).
static inline int n48_cg_redo_undo_ok(const n48_cg_redo_pre *p, uint32_t k) { return p && p->valid && p->k == k; }
static inline uint32_t n48_cg_redo_undo(uint32_t *cand, const uint32_t *apple, uint32_t from, uint32_t to, int build,
                                        uint32_t isUnit, xlat12_pool *pool, const xlat12_unit *U,
                                        const n48_cg_redo_pre *pre, n48_dl *fl, xlat12_ud_carry *carry) {
    if (build && cand && apple && to > from)
        for (uint32_t i = from; i < to; i++) cand[i] = apple[i];           // Apple's bytes, exactly as a refusal leaves them
    const uint32_t undone = n48_mib_unit_undo2(isUnit, (uint32_t)N48_SEG_COPY_OVERLAP, pool, U);   // pool + spill tier
    if (pre && pre->valid) {
        if (fl) n48_mib_defer_undo_adds(&pre->fl, fl);
        if (carry) *carry = pre->fl.carry;
    }
    return undone;
}

#endif // N48_GFX_CGREDO_H
