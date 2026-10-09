/* gfx_cgw108.h — build 0.0.550 ( PLAN step 1,, an internal design note FIX 2):
 * SWITCH 108 "cgwait", THE COPY-GUARD WAIT UNDER SWITCH 37. `108 | M << 8`: M 1 ON (= 364), M 2 OFF (= 620, the default and the
 * boot value); bare `108` reads. Mid-arm guarded. No SHADOW: switch 78 already counts every refusal ON would take on
 * (`IN_FLIGHT not redone under 37`), so a SHADOW mode would print that same count again.
 *
 * WHY. RUN AX/AY's frame 1 P was refused copy-guard IN_FLIGHT on a page new to its segment, with switch 78's redo declining it
 * `37-on` (run11as `cguard-ref: f1 seg 0/1 IN_FLIGHT ... new-in-seg ... redo 37-on`): with switch 37 ON the pass owns MM-window
 * priority (gfxsrc_policy opens Navi48MmPrioScope first), so the copier yields to the pass and a wait inside the pass is futile
 * (RING-NEUTER-FORGIVE.md: "IN_FLIGHT waits are futile under switch 37").'s review bounded the wait at 2 ms per frame and
 * only with 37 OFF.
 *
 * WHAT ON CHANGES, AND ONLY THIS. Switch 78's redo is live (ON implies it, whatever 78 says), and an IN_FLIGHT refusal under
 * switch 37 is WAITED ON instead of declined: for the wait only, the pass RELEASES its MM-window priority (Navi48MmPrioRelease,
 * Navi48Ttl.hpp: the scope's nesting drops to 0 so a copier does not yield, and is re-entered before the re-translate), within
 * the SAME per-frame budget 78 uses (N48_CG_REDO_WAIT_US = 2000 us per frame, gCgRedoF.waitUs, polled every 50 us). If the
 * copy cleared, the segment is undone and re-translated ONCE (78's n48_cg_redo_seq: seg_redone and N48_CG_REDO_FRAME_MAX hold),
 * and the unchanged counted navi48_cg_seg_check at its original site stays the ONLY admit. Not clear: refused as today. A
 * self-owned slot and an earlier segment's page stay ineligible exactly as 78 has them. OFF: 78 is 0.0.549's.
 *
 * Pure: no lock, no clock, no log, no register. Host-tested by tests/gfx_cgredo_test.cpp (cgw108 section). */
#ifndef N48_GFX_CGW108_H
#define N48_GFX_CGW108_H

#include <stdint.h>
#include "gfx_copyguard.h"   // N48_CG_RD_WAIT, N48_CG_REDO_WAIT_US

#define N48_CGW_SWITCH 108u
enum { N48_CGW_OFF = 0u, N48_CGW_ON = 1u };

/* The verb: M 1 ON, M 2 OFF; returns 1 when M was one of them (then *mode is set), 0 for any other M (refused, unchanged). */
static inline uint32_t n48_cgw_set(uint32_t m, uint32_t *mode)
{
    if (!mode) return 0u;
    if (m == 1u) { *mode = N48_CGW_ON; return 1u; }
    if (m == 2u) { *mode = N48_CGW_OFF; return 1u; }
    return 0u;
}
/* Switch 78's redo is live for this pass: 78 ON, or 108 ON. OFF (108 OFF): exactly 78. */
static inline uint32_t n48_cgw_redo_on(uint32_t on78, uint32_t mode) { return (on78 || mode == N48_CGW_ON) ? 1u : 0u; }
/* What the plan is told about switch 37: 108 ON tells it "no priority" (the wait releases it); OFF: 37 as it is. */
static inline uint32_t n48_cgw_mm37(uint32_t mm37, uint32_t mode) { return mode == N48_CGW_ON ? 0u : (mm37 ? 1u : 0u); }
/* Release the pass's MM-window priority for THIS wait: 108 ON, 37 ON, and the plan answered WAIT. */
static inline uint32_t n48_cgw_release(uint32_t mode, uint32_t mm37, uint32_t plan)
{
    return (mode == N48_CGW_ON && mm37 && plan == N48_CG_RD_WAIT) ? 1u : 0u;
}
/* This frame's remaining budget: what 78's wait is handed (never more than N48_CG_REDO_WAIT_US in a frame). */
static inline uint64_t n48_cgw_left(uint64_t waited_this_frame)
{
    return waited_this_frame < (uint64_t)N48_CG_REDO_WAIT_US ? (uint64_t)N48_CG_REDO_WAIT_US - waited_this_frame : 0ull;
}

/* build 0.0.551 (the 0.0.550 review's SHOULD 5): A WALL-CLOCK CAP BESIDE THE STEP COUNT. gfxsrc_cg_redo's wait (78's, and
 * 108's with the MM priority released) counts polls x N48_CG_REDO_POLL_US; a delay that oversleeps (preemption, a slow busy test)
 * would let the real time run past the budget. This is n48_cg_redo_wait with one more exit: the uptime (`now`, microseconds; the
 * kext's is mach_absolute_time based) since the wait began reaching `cap_us` ends it NOT clear (refused as a timeout). The time
 * charged is the larger of the step count and the elapsed wall time, so the frame's 2000 us budget is spent by real time too.
 * `now` null: exactly n48_cg_redo_wait (the step count alone). */
#define N48_CGW_WALL_CAP_US 2000u
typedef uint64_t (*n48_cgw_now_fn)(void *ud);
static inline int n48_cgw_wait(n48_cg_busy_fn busy, n48_cg_delay_fn delay, n48_cgw_now_fn now, void *ud, uint64_t left_us,
                               uint32_t step_us, uint64_t cap_us, uint64_t *waited)
{
    uint64_t w = 0ull, el = 0ull;
    int clear = 0;
    if (!busy || !delay || !step_us) { if (waited) *waited = 0ull; return 0; }
    const uint64_t t0 = now ? now(ud) : 0ull;
    for (;;) {
        if (!busy(ud)) { clear = 1; break; }
        if (!n48_cg_wait_more(w, left_us)) break;
        if (now) {
            const uint64_t t = now(ud);
            el = t > t0 ? t - t0 : 0ull;
            if (el >= cap_us) break;          /* the wall-clock cap: not clear, a timeout */
        }
        delay(ud, step_us);
        w += step_us;
    }
    if (now) { const uint64_t t = now(ud); el = t > t0 ? t - t0 : 0ull; }
    if (waited) *waited = el > w ? el : w;
    return clear;
}

typedef struct {
    uint64_t released, clear, timeouts, redone, thenOk, thenRefused, waitUsMax, waitUs;
} n48_cgw_stats;

#define N48_CGW_FMT \
    "cgwait108: `gfxneuter 108 | M << 8` is %s (364 ON, 620 OFF default; mid-arm guarded; ON makes 78's redo live; switch 78 is %s, " \
    "37 is %s)%s."
#define N48_CGW_FMT2 \
    "cgwait108: IN_FLIGHT waits with the MM priority released %llu: clear %llu, timed out %llu (max %llu us, total %llu us; budget " \
    "%u us per frame); redone after a wait %llu -> passed the full check %llu, refused %llu"

#endif /* N48_GFX_CGW108_H */
