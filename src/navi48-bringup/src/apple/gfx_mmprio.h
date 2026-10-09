// gfx_mmprio.h — 0.0.433 (an internal design note). MM-WINDOW PRIORITY, THE PURE DECISION LOGIC.
//
// decide36b measured single-IB policy runs at 19.25 ms/run, with descriptor reads alone 11.7 ms/run
// (60.7% of it) — and 0.0.433's design memo established that cost is ~99% WAITING for gVramMmLock, not reading
// (uncontended, the same reads cost about 0.1 ms). The most likely contender is our OWN residency copier, which
// pushed 492 MiB through the same MM window that boot, releasing and re-taking the lock every 64 dwords
// (Navi48AccelPeer.cpp:1297-1312). This header is the fix's PURE half: which thread should back off, for how long,
// and whether it rode the bound out. The reader itself (gfxc_read, gfxc_page, gfxsrc_desc_read, n48_dp_read,
// gfxsrc_pgm_profile, xlat12) is untouched — this only changes who gets gVramMmLock first.
//
// Pure C++ (static inline), so the kext and its host suite (tests/gfx_mmprio_test.cpp) run the SAME arithmetic.
// Nothing here takes a lock, calls IODelay or reads the clock: every function is handed scalars the caller already
// has — a thread identity as `uintptr_t` so this header needs no kernel headers, and elapsed microseconds already
// computed from clock_get_uptime by the caller. THE INSTRUMENT IS ALWAYS ON; the switch (`accel gfxneuter 37`) only
// gates whether a non-owner thread actually yields — the owner's own lock wait is timed unconditionally, so the
// `mmprio:` report line is an honest A/B whichever way the switch is thrown.
#ifndef N48_GFX_MMPRIO_H
#define N48_GFX_MMPRIO_H

#include <stdint.h>

// MM-PRIORITY: "bounded at 4 ms". One place, so the kext and the host test can never disagree about the cap.
#define N48_MMPRIO_BOUND_US 4000ull

// 0.0.433 — nesting counts navi48_mm_prio_enter/exit calls (Navi48MmPrioScope's ctor/dtor, opened as gfxsrc_policy's
// FIRST statement so it is exact across every return path). 0 means no policy pass is in flight, and "the owner" is
// meaningful only while it is > 0 — whichever thread opened the outermost (0 -> 1) scope.
static inline uint32_t n48_mmprio_enter_nesting(uint32_t nesting)
{
    return nesting < 0xFFFFFFFFu ? nesting + 1u : nesting;   // saturate rather than wrap; never observed in practice
}

// T4: an exit with no matching enter (or one exit too many) must never underflow past 0 into a huge nesting count —
// it stays 0, and a caller that mismatched enter/exit has that to notice, not a wrapped counter that silently masks it.
static inline uint32_t n48_mmprio_exit_nesting(uint32_t nesting)
{
    return nesting > 0u ? nesting - 1u : 0u;
}

// Is THIS call the owner's own? True only while a pass is active (nesting > 0) AND the caller IS the thread that
// opened the outermost scope. False for every caller while nesting == 0 (no pass, no owner) — which is what keeps
// every OTHER caller of navi48_vram_read_mm/write_mm (the residency copier, self-tests, everything outside a policy
// pass) completely untouched: neither this predicate nor n48_mmprio_should_yield below ever fires for them.
static inline int n48_mmprio_is_owner(uint32_t nesting, uintptr_t owner, uintptr_t caller)
{
    return (nesting > 0u) && (caller == owner);
}

// T1/T2: should a NON-owner caller spin before IOLockLock at all? Gated by the switch (T1: off -> never) AND by a
// pass being active; never true for the owner itself (T2's break drops the `!isOwner` test).
static inline int n48_mmprio_should_yield(uint32_t on, uint32_t nesting, int isOwner)
{
    return on && (nesting > 0u) && !isOwner;
}

// Keep spinning? The same three conditions PLUS the bound, re-checked every iteration so a pass that ends mid-spin
// (nesting drops to 0) or a switch flipped off mid-spin stops the loop immediately rather than riding out the cap.
static inline int n48_mmprio_keep_spinning(uint32_t on, uint32_t nesting, uint64_t elapsedUs)
{
    return on && (nesting > 0u) && (elapsedUs < N48_MMPRIO_BOUND_US);
}

// T3: did this spin end because it hit the 4 ms cap (as opposed to the pass ending or the switch going off)?
static inline int n48_mmprio_bound_hit(uint64_t elapsedUs)
{
    return elapsedUs >= N48_MMPRIO_BOUND_US;
}

// ---- Counters. One instance, fed on every navi48_vram_read_mm/write_mm call, whatever the switch is. Units are
// whole microseconds throughout (what IODelay and the 4 ms bound are already expressed in — no ns/us conversion at
// report time, unlike gMibPol). ----
typedef struct {
    uint64_t owner_wait_us;     // the owner's OWN IOLockLock wait, accumulated — ALWAYS timed, switch or no switch
    uint64_t owner_acquires;    // ... over how many owner lock acquisitions
    uint64_t owner_max_us;      // the single longest owner wait seen
    uint64_t yield_count;       // non-owner calls that took the yield path (switch on, a pass active)
    uint64_t yield_us;          // ... accumulated spin time
    uint64_t yield_max_us;      // the single longest yield spin seen
    uint64_t bound_hits;        // of those, how many rode the spin out to the 4 ms cap
} n48_mmprio_stats;

static inline void n48_mmprio_note_owner_wait(n48_mmprio_stats *s, uint64_t us)
{
    if (!s) return;
    s->owner_wait_us += us;
    s->owner_acquires++;
    if (us > s->owner_max_us) s->owner_max_us = us;
}

static inline void n48_mmprio_note_yield(n48_mmprio_stats *s, uint64_t us, int boundHit)
{
    if (!s) return;
    s->yield_count++;
    s->yield_us += us;
    if (us > s->yield_max_us) s->yield_max_us = us;
    if (boundHit) s->bound_hits++;
}

static inline uint32_t n48_mmprio_cap32(uint64_t v)
{
    return v > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)v;
}

// THE SWITCH'S OWN LINE — printed only when `accel gfxneuter 37 | M << 8` itself is called (M 1 on, M 0xFF off, bare
// reads), mirroring gfx_mib.h's N48_MIB_REPORT_FMT. Two %s only, so it is bounded under 512 bytes by construction;
// tests/gfx_mmprio_test.cpp (T7) measures it at its widest anyway.
#define N48_MMPRIO_SWITCH_FMT \
    "mmprio-sw: MM-window priority for the policy pass is %s%s. ON, a non-owner caller of navi48_vram_read_mm/" \
    "write_mm yields before IOLockLock while the switch is on and a policy pass is active, bounded at 4 ms; OFF, " \
    "every caller goes straight to IOLockLock, 0.0.432 byte for byte. The reader itself (gfxc_read, gfxc_page, " \
    "gfxsrc_desc_read, n48_dp_read, gfxsrc_pgm_profile, xlat12) is untouched either way."

// THE ALWAYS-ON COUNTERS LINE, printed beside the MIB-0 single-IB policy phases line it explains the
// wait behind. Printed on every read of the `gfxneuter` report, whatever switch was thrown — the instrument is
// always on; only the yielding itself is gated. Numerics clamped to 32 bits; worst case is 7 ten-digit numbers plus
// about 100 bytes of text, measured by tests/gfx_mmprio_test.cpp (T7).
#define N48_MMPRIO_FMT \
    "mmprio: owner wait %u us / %u acquire(s), max %u us; other-thread yields %u, %u us, max %u us, bound hits %u"

#define N48_MMPRIO_ARGS(s) \
    n48_mmprio_cap32((s)->owner_wait_us), n48_mmprio_cap32((s)->owner_acquires), n48_mmprio_cap32((s)->owner_max_us), \
    n48_mmprio_cap32((s)->yield_count), n48_mmprio_cap32((s)->yield_us), n48_mmprio_cap32((s)->yield_max_us), \
    n48_mmprio_cap32((s)->bound_hits)

#endif /* N48_GFX_MMPRIO_H */
