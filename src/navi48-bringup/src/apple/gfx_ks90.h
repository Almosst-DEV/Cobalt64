// gfx_ks90.h — build 0.0.534 ( RUN AF, ranked item 2): SWITCH 90, THE EXPIRY CHECK DEFERS ON A BUSY gXdLock.
//
// WHY. Switch 65 (0.0.498, AppleHardwareHook.cpp ks_eop_at_expiry) asks the owned fence slots at the deferral's expiry, but only when
// IOLockTryLock(gXdLock) succeeds; busy = nothing read and 0.0.497's withdrawal. In RUN AF a judge starved of the MM window by
// the 4 s wallpaper verify held gXdLock at the first unmap after the stall: `gXdLock BUSY, nothing read: WITHDRAWN AT EXPIRY`
// -> stop_why 3, although the flight HAD retired (the next line: `flightring: RETIRED token seq 4 ... OURS`).
//
// WHAT CHANGES (switch 90 ON): on exactly that branch (65 asked, the lock BUSY, the verdict still N48_KSD_NOW_TIMEOUT), the verdict
// becomes N48_KSD_DEFER while the OLDEST live flight's age is under N48_KS90_CAP_MULT x the flight bound (kKsFlightUs = 2 s, so 4 s
// from that flight's own commit stamp). Every other live entry is younger, so the cap holds for all of them. hook_unmapVA then takes
// the ordinary deferral: n48_ks_withdraw_hold(N48_KSD_DEFER) = 0 releases the marker at once (KEYSTONE-A-PRIME's A'), the clear is
// gated on `!deferNow` so root[511] is not written, and the request is recorded for the next unmap. Past the cap, with no live entry
// (blocking stamp 0), or with an unusable clock: NONE - today's withdrawal, unchanged. OFF, the lock taken, or any verdict other
// than NOW_TIMEOUT: the verdict is returned unchanged.
// `accel gfxneuter 90 | M << 8`: M 1 ON (= 346), M 2 OFF (= 602, the default and the boot value), bare `90` reads. OFF = 0.0.533.
#ifndef N48_GFX_KS90_H
#define N48_GFX_KS90_H

#include <stdint.h>
#include "gfx_keystone.h"

#ifdef __cplusplus
extern "C" {
#endif

#define N48_KS90_SWITCH   90u
#define N48_KS90_CAP_MULT 2u     // the hard cap per flight: 2 x the flight bound from the oldest live flight's stamp

enum { N48_KS90_NONE = 0, N48_KS90_DEFER = 1, N48_KS90_CAPPED = 2, N48_KS90_TORN = 3 };

// `locked`: ks_eop_at_expiry took gXdLock. `kdv`: the verdict after its poll. `oldest_at_us`: n48_fr_defer_verdict's blocking stamp
// (the OLDEST live entry's at_us; 0 = no live entry). Returns the verdict hook_unmapVA acts on; `*what` (optional) says which case.
static inline uint32_t n48_ks90_kdv(uint32_t on, uint32_t locked, uint32_t kdv, uint32_t now_ok, uint64_t now_us,
                                    uint64_t oldest_at_us, uint64_t bound_us, uint32_t *what)
{
    if (what) *what = N48_KS90_NONE;
    if (!on || locked || kdv != N48_KSD_NOW_TIMEOUT) return kdv;
    if (!now_ok || !oldest_at_us || !bound_us || now_us < oldest_at_us) {   // no flight to wait for, or no age: fail-closed
        if (what) *what = N48_KS90_TORN;
        return kdv;
    }
    if (now_us - oldest_at_us >= (uint64_t)N48_KS90_CAP_MULT * bound_us) {  // past the hard cap: today's withdrawal
        if (what) *what = N48_KS90_CAPPED;
        return kdv;
    }
    if (what) *what = N48_KS90_DEFER;
    return N48_KSD_DEFER;
}

typedef struct { uint64_t busy, deferred, capped, torn; uint32_t lines; } n48_ks90_stats;
static inline void n48_ks90_count(n48_ks90_stats *st, uint32_t what)
{
    if (!st) return;
    st->busy++;
    if (what == N48_KS90_DEFER) st->deferred++;
    else if (what == N48_KS90_CAPPED) st->capped++;
    else if (what == N48_KS90_TORN) st->torn++;
}
// The per-deferral line (args: ctx, create #, fire #, oldest seq, its age us, cap us, deferred count) and the report line
// (args: ON/OFF, why, busy checks, deferred, capped withdrawals, torn withdrawals). Both <= 491 bytes (tested).
#define N48_KS90_LINE_FMT \
    "ksexp90: EXPIRY CHECK ctx %p (create #%u, fire #%u): gXdLock BUSY, nothing read - DEFERRED (switch 90), oldest live seq %u " \
    "at %llu of %llu us cap: marker released, root[511] left standing, nothing written; deferred-busy %llu."
#define N48_KS90_REPORT_FMT \
    "ksexp90: defer on a busy gXdLock at the expiry (`gfxneuter 90`) is %s%s. Busy checks (65 ON) %llu: deferred-busy %llu, " \
    "capped-withdrawals %llu (past 2 x the flight bound), torn-withdrawals %llu. Needs switch 65 ON to act."

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_KS90_H */
