// gfx_flipmode.h — build 0.0.518: FLIP MODE, switch 74. DEFAULT OFF.
//
// THE PROBLEM: the display shim (DisplayPipeGuard.cpp dpg_perform) detiles every present straight into the ONE buffer
// the display is scanning (the console, "A"), so a copy that is still running when the raster passes shows a torn frame. Copying
// inside the vertical blank is out (41 lines x 11.26 us ~= 0.46 ms against a 0.55-0.75 ms detile). FLIP MODE double-buffers: a
// second console-shaped buffer "B" (gmc.vram_alloc, allocated once), the present detiles into whichever of A/B the display is
// NOT scanning (the back buffer) and then programs HUBP0's primary address to it with a VUPDATE-latched flip
// (dcn41_hubp_program_flip(..., immediate=false)): the hardware switches buffers between frames, never mid-frame (
// proved the primitive: SURFACE_FLIP_PENDING set, then cleared after one VUPDATE, SURFACE_EARLIEST_INUSE following).
//
// THIS FILE IS PURE (no kernel headers, no hardware): the decisions and their ORDER live here and the host test
// (tests/gfx_flipmode_test.cpp) drives the SAME functions against a model of the display. The kext (Navi48Bringup.cpp) supplies
// the I/O as n48_fm_ops callbacks: the bound, allowlisted n48dcn device for every register access, the SDMA copy for pixels.
//
// PER PRESENT (n48_fm_present; dpg_perform asks it only AFTER switch 73's hold check, which stays first):
//   1. a pending restore request (a withdrawal) is served first: restore, flip mode OFF, the present is held;
//   2. the BOUNDED WAIT: poll SURFACE_FLIP_PENDING on HUBP0 every N48_FM_POLL_US for at most N48_FM_WAIT_MAX_US (two frames);
//      still pending -> HOLD (return, no copy, no flip) and count a timeout. The previous flip has not latched, so neither buffer
//      is free to be written;
//   3. front = SURFACE_EARLIEST_INUSE as the hardware reports it after the latch; neither A nor B -> flip mode OFF, logged, and
//      NOTHING is written (somebody else owns the plane);
//   4. back = the other buffer; the programmed address's HIGH dword must already equal back's (both buffers lie below 256 MiB of
//      VRAM, so the HIGH dword never changes and the LOW write is the atomic latch);
//   5. the SDMA detile into back (the existing COPY_TILED_SUB_WINDOW path with the destination as a parameter; its allowlists admit
//      exactly A and B); a failed copy HOLDS (no flip, the glass keeps its last picture);
//   6. flip(back) through the exact-set device (dcn41 refuses any address but A and B), then the programmed address is read
//      back: it must equal back and its HIGH dword must be unchanged, else flip mode OFF + restore.
// RESTORE (n48_fm_restore; on switch OFF, the force-restore verb, a commit-arm disarm, a withdrawal, and every error above but
// the foreign front): settle any pending flip; if front is B copy B into A; flip to A; verify SURFACE_EARLIEST_INUSE == A and the
// programmed address == A (one more flip to A if not, as dcnflip's restore does); flip mode OFF.
//
// build 0.0.519 (the 0.0.518 review):
//   F1 (MEDIUM-1) a stuck latch ends: after a timed-out wait the EARLIEST_INUSE it read last is checked - neither A nor B takes
//      the FOREIGN path (flip mode OFF, nothing written); N48_FM_STUCK_TIMEOUTS consecutive timeouts restore to A and turn flip
//      mode OFF (N48_FM_OFF_STUCK). A latched wait resets the run.
//   F2 (L1) the restore copies B into A only when `bHoldsPresent` - set when a present's detile into B completed (or the engage
//      seeded B from A), cleared before any other write into B (a present's copy into B, the A/B test's grey fills).
//   F3 (L2) a failed or timed-out B -> A copy does NOT flip to A: the front stays as it is, flip mode goes OFF, it is counted
//      (restoreCopyFails) and `aCopyPending` is set; no later restore flips to A until the kext's `a_settled` answers that no
//      write into A can still be running (SDMA0 QUEUE0 idle and its last fence landed).
//
// build 0.0.520 ( MEDIUM-2, the 0.0.519 review): a restore that FAILS on F3 (the B -> A copy failed, or A is not
//   settled) leaves flip mode OFF (on = 0) but ENGAGED with the device's exact flip set {A, B} still in place, instead of
//   disengaging with B on the glass. So the next restore - the switch OFF, the commit disarm, or the force verb `gfxneuter 842` -
//   retries it (the copy again, or a_settled then the flip to A), and only a restore that VERIFIED HUBP0 on A (or wrote nothing
//   because the front was foreign) disengages. A restore's 1 therefore always means HUBP0 was read back on A, or nothing of ours
//   was engaged. While so engaged, ON and the A/B test are refused by the kext (the restore must finish first).
#ifndef N48_GFX_FLIPMODE_H
#define N48_GFX_FLIPMODE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N48_FM_SWITCH          74u
#define N48_FM_FRAME_US        16681u                      /* one frame of the lit 59.95 Hz raster (2720 x 1481 at 241.5 MHz) */
#define N48_FM_WAIT_MAX_US     (2u * N48_FM_FRAME_US)      /* the per-present bound: two frames */
#define N48_FM_RESTORE_WAIT_US (3u * N48_FM_FRAME_US)      /* the restore's settle and verify waits */
#define N48_FM_POLL_US         100u
#define N48_FM_HIGH_LIMIT      0x10000000ull               /* both buffers end below 256 MiB of VRAM */
#define N48_FM_WAIT_BUCKETS    7u
#define N48_FM_TEST_ARG_BASE   1000u                       /* `dcnflip 1000 + N`: the unarmed A/B test, N flips */
#define N48_FM_TEST_MIN        2u
#define N48_FM_TEST_MAX        240u
#define N48_FM_STUCK_TIMEOUTS  3u                          /* 0.0.519 F1: consecutive latch timeouts that restore to A */

/* Why flip mode went OFF (the last reason is kept for the report). */
enum {
    N48_FM_OFF_NONE = 0, N48_FM_OFF_VERB = 1, N48_FM_OFF_FORCE = 2, N48_FM_OFF_DISARM = 3, N48_FM_OFF_WITHDRAWAL = 4,
    N48_FM_OFF_FOREIGN = 5, N48_FM_OFF_HIGH = 6, N48_FM_OFF_FLIPFAIL = 7, N48_FM_OFF_READFAIL = 8, N48_FM_OFF_NOTTILED = 9,
    N48_FM_OFF_DST = 10, N48_FM_OFF_STUCK = 11, N48_FM_OFF_COPYFAIL = 12, N48_FM_OFF_COUNT = 13
};
static inline const char *n48_fm_off_name(uint32_t w)
{
    static const char *const n[N48_FM_OFF_COUNT] = { "none", "switch OFF", "force-restore verb", "commit-arm disarm",
        "withdrawal", "FOREIGN FRONT", "ADDRESS_HIGH", "flip write failed", "HUBP read failed", "plane not tiled",
        "destination not A/B", "LATCH STUCK (3 consecutive timeouts)", "B->A copy failed (front kept)" };
    return w < N48_FM_OFF_COUNT ? n[w] : "?";
}

typedef struct n48_fm {
    uint32_t on;           /* switch 74 */
    uint32_t engaged;      /* A and B known, the device's exact flip set is {A, B} */
    uint32_t haveB;        /* B allocated (once per boot) */
    uint32_t offWhy;       /* N48_FM_OFF_* of the last time flip mode went OFF */
    uint32_t restoreReq;   /* 0 or the N48_FM_OFF_* a withdrawal asked for; served by the next present or verb */
    uint32_t waitMaxUs;
    uint64_t aOff, bOff, len;   /* VRAM byte offsets of A and B, and the console-shaped length both hold */
    uint64_t aMc, bMc;          /* the same in MC space (vram_start + offset) */
    uint64_t front;             /* the last SURFACE_EARLIEST_INUSE read */
    uint64_t presents, flips, flipsToA, flipsToB, waits, waitHist[N48_FM_WAIT_BUCKETS], timeouts, foreign, highChanged,
             copyFails, flipFails, readFails, restores, restoreFails, restoreCopies, restoreRequests, engages, engageRefused,
             writersHeld, testRuns;
    /* build 0.0.519 */
    uint32_t consecTimeouts;    /* F1: latch timeouts in a row (a latched wait resets it) */
    uint32_t bHoldsPresent;     /* F2: B holds a real present (a completed detile into B, or the engage's seed from A) */
    uint32_t aCopyPending;      /* F3: a B -> A copy failed; A may still be written until the kext's a_settled says not */
    uint64_t stuckRestores, timeoutForeign, restoreCopyFails, restoreCopySkipped, restoreUnsettled;
} n48_fm;

/* The I/O the kext supplies. Every register access goes through the bound, allowlisted n48dcn device (HUBP0). */
typedef struct n48_fm_ops {
    void *ctx;
    int (*read_front)(void *ctx, uint32_t *pending, uint64_t *earliest);   /* SURFACE_FLIP_PENDING / EARLIEST_INUSE; 0 ok */
    int (*read_primary)(void *ctx, uint64_t *mc);                          /* the programmed PRIMARY_SURFACE_ADDRESS; 0 ok */
    void (*delay_us)(void *ctx, uint32_t us);
    uint32_t (*copy_to)(void *ctx, uint64_t dstOff);   /* this present's detile into dstOff: scanout status (0 / 11 = done) */
    uint32_t (*copy_b_to_a)(void *ctx);                /* the restore copy: 0 = done */
    int (*flip)(void *ctx, uint64_t mc);               /* dcn41_hubp_program_flip(0, mc, vmid 0, tmz 0, immediate false); 0 ok */
    void (*release)(void *ctx);                        /* disengaged: clear the device's exact flip set */
    uint32_t (*a_settled)(void *ctx);                  /* 0.0.519 F3: 1 = no copy into A can still be running (queue idle, fence in) */
} n48_fm_ops;

/* ---- the pure decisions --------------------------------------------------------------------------------------------------- */

/* The wait histogram: 0 latched at the first read (no wait), then <= 1, 2, 4, 8, 17 (one frame), 34 ms (two frames). */
static inline uint32_t n48_fm_wait_bucket(uint32_t polls, uint32_t us)
{
    if (polls == 0u) return 0u;
    if (us <= 1000u) return 1u;
    if (us <= 2000u) return 2u;
    if (us <= 4000u) return 3u;
    if (us <= 8000u) return 4u;
    if (us <= N48_FM_FRAME_US) return 5u;
    return 6u;
}

/* The back buffer: the one of {A, B} the display is NOT scanning. 0 = selected; 1 = the front is neither (FOREIGN). */
static inline uint32_t n48_fm_select(uint64_t front, uint64_t aMc, uint64_t bMc, uint64_t *backMc)
{
    if (backMc) *backMc = 0u;
    if (aMc == 0u || bMc == 0u || aMc == bMc) return 1u;
    if (front == aMc) { if (backMc) *backMc = bMc; return 0u; }
    if (front == bMc) { if (backMc) *backMc = aMc; return 0u; }
    return 1u;
}

/* The flip allowlist: exactly A or B. */
static inline uint32_t n48_fm_flip_ok(const n48_fm *f, uint64_t mc)
{
    return (f && f->aMc && f->bMc && f->aMc != f->bMc && (mc == f->aMc || mc == f->bMc)) ? 1u : 0u;
}

/* The copy-destination allowlist: exactly A's or B's VRAM offset. */
static inline uint32_t n48_fm_dst_ok(const n48_fm *f, uint64_t dstOff)
{
    return (f && f->len && f->aOff != f->bOff && (dstOff == f->aOff || dstOff == f->bOff)) ? 1u : 0u;
}

/* VRAM offset of an MC address that is A or B (0 when neither: callers check n48_fm_flip_ok first). */
static inline uint64_t n48_fm_off_of(const n48_fm *f, uint64_t mc)
{
    return mc == f->aMc ? f->aOff : (mc == f->bMc ? f->bOff : 0u);
}

/* Is the pair (A, B) usable? 0 = yes, else a reason: 1 empty, 2 misaligned (B not 64 KiB aligned, lengths not dword), 3 B
 * overlaps A or the console reservation [0, vramBase), 4 past BAR0, 5 not below 256 MiB (the HIGH dword could change), 6 the MC
 * HIGH dwords differ. */
static inline uint32_t n48_fm_buffers_ok(uint64_t aOff, uint64_t bOff, uint64_t len, uint64_t bar0Size, uint64_t vramBase,
                                         uint64_t mcBase)
{
    if (len == 0u || bar0Size == 0u) return 1u;
    if ((bOff & 0xffffull) || (aOff & 3u) || (len & 3u)) return 2u;
    if (bOff < vramBase || (aOff < bOff + len && bOff < aOff + len)) return 3u;
    if (aOff > bar0Size || len > bar0Size - aOff || bOff > bar0Size || len > bar0Size - bOff) return 4u;
    if (aOff + len > N48_FM_HIGH_LIMIT || bOff + len > N48_FM_HIGH_LIMIT) return 5u;
    if (((mcBase + aOff) >> 32) != ((mcBase + bOff) >> 32) || ((mcBase + aOff + len - 1u) >> 32) != ((mcBase + aOff) >> 32) ||
        ((mcBase + bOff + len - 1u) >> 32) != ((mcBase + bOff) >> 32))
        return 6u;
    return 0u;
}

/* Does HUBP0 scan a buffer shaped like the console? (viewport = console size, pitch = RowBytes/4, 32-bit format 8 or 10,
 * SW_MODE 0 linear.) B is allocated console-shaped, so this is what makes it a valid plane. 1 = yes. */
static inline uint32_t n48_fm_hubp_ok(uint32_t vpW, uint32_t vpH, uint32_t pitchPx, uint32_t fmt, uint32_t swMode,
                                      uint32_t conW, uint32_t conH, uint32_t rowBytes)
{
    return (vpW && vpH && vpW == conW && vpH == conH && rowBytes && (rowBytes & 3u) == 0u && pitchPx == rowBytes / 4u &&
            (fmt == 8u || fmt == 10u) && swMode == 0u) ? 1u : 0u;
}

/* The unarmed A/B test's argument: `dcnflip 1000 + N`, N in [2, 240]. Returns N, or 0 when the argument is not a test. */
static inline uint32_t n48_fm_test_n(uint64_t arg)
{
    if (arg < (uint64_t)N48_FM_TEST_ARG_BASE + N48_FM_TEST_MIN || arg > (uint64_t)N48_FM_TEST_ARG_BASE + N48_FM_TEST_MAX)
        return 0u;
    return (uint32_t)(arg - N48_FM_TEST_ARG_BASE);
}

/* ---- the bounded wait ----------------------------------------------------------------------------------------------------- */
enum { N48_FM_W_LATCHED = 0, N48_FM_W_TIMEOUT = 1, N48_FM_W_READFAIL = 2 };

/* Poll until no flip is pending, at most maxUs. *front is the last EARLIEST_INUSE, *us the time waited, *polls the delays. */
static inline uint32_t n48_fm_wait_latch(const n48_fm_ops *o, uint32_t maxUs, uint64_t *front, uint32_t *us, uint32_t *polls)
{
    uint32_t t = 0u, n = 0u, pending = 1u;
    uint64_t e = 0u;
    for (;;) {
        if (o->read_front(o->ctx, &pending, &e) != 0) { *front = e; *us = t; *polls = n; return N48_FM_W_READFAIL; }
        if (!pending) { *front = e; *us = t; *polls = n; return N48_FM_W_LATCHED; }
        if (t >= maxUs) { *front = e; *us = t; *polls = n; return N48_FM_W_TIMEOUT; }
        o->delay_us(o->ctx, N48_FM_POLL_US);
        t += N48_FM_POLL_US;
        n++;
    }
}

/* ---- the restore ---------------------------------------------------------------------------------------------------------- */
/* Back to A. `force` (the force-restore verb) flips to A even when the front is foreign; otherwise a foreign front is left
 * alone (nothing written) and the restore counts as failed. Returns 1 when EARLIEST_INUSE == A and the programmed address == A
 * were both read back. Always leaves flip mode OFF (with `why`) and disengaged. */
static inline uint32_t n48_fm_restore(n48_fm *f, const n48_fm_ops *o, uint32_t why, uint32_t force)
{
    uint64_t front = 0u, prim = 0u;
    uint32_t us = 0u, polls = 0u, ok = 0u;
    __atomic_store_n(&f->on, 0u, __ATOMIC_RELEASE);
    f->offWhy = why;
    __atomic_store_n(&f->restoreReq, 0u, __ATOMIC_RELEASE);
    if (!f->engaged) return 1u;                        /* nothing of ours was ever on the plane */
    (void)n48_fm_wait_latch(o, N48_FM_RESTORE_WAIT_US, &front, &us, &polls);
    f->front = front;
    if (front != f->aMc && front != f->bMc && !force) {
        f->restoreFails++;
        f->engaged = 0u;
        o->release(o->ctx);
        return 0u;
    }
    if (front == f->bMc && f->bHoldsPresent) {         /* the picture on the glass is a present in B: carry it into A first */
        f->restoreCopies++;
        if (o->copy_b_to_a(o->ctx) != 0u) {            /* 0.0.519 F3: a failed copy never flips to A; the front is kept */
            f->restoreCopyFails++;
            f->restoreFails++;
            f->aCopyPending = 1u;
            f->offWhy = N48_FM_OFF_COPYFAIL;
            /* build 0.0.520: STAY ENGAGED (on is already 0) and KEEP the {A, B} exact flip set, so the
             * next restore (switch OFF, the disarm, `gfxneuter 842`) retries this restore instead of finding nothing engaged. */
            return 0u;
        }
        f->aCopyPending = 0u;                          /* this copy's own fence landed: every earlier write into A is done */
    } else if (front == f->bMc) {
        f->restoreCopySkipped++;                       /* 0.0.519 F2: B holds no present (a test fill): A keeps its own */
    }
    if (f->aCopyPending) {                             /* 0.0.519 F3: an earlier failed copy may still be writing A */
        if (!o->a_settled || !o->a_settled(o->ctx)) {
            f->restoreUnsettled++;
            f->restoreFails++;
            return 0u;                                 /* build 0.0.520: stay engaged, keep the set: retried */
        }
        f->aCopyPending = 0u;
    }
    for (uint32_t attempt = 0u; attempt < 2u && !ok; attempt++) {
        if (o->flip(o->ctx, f->aMc) != 0) continue;
        f->flipsToA++;
        (void)n48_fm_wait_latch(o, N48_FM_RESTORE_WAIT_US, &front, &us, &polls);
        f->front = front;
        ok = (front == f->aMc && o->read_primary(o->ctx, &prim) == 0 && prim == f->aMc) ? 1u : 0u;
    }
    if (ok) f->restores++; else f->restoreFails++;
    f->engaged = 0u;
    o->release(o->ctx);
    return ok;
}

/* ---- the present ---------------------------------------------------------------------------------------------------------- */
enum { N48_FM_P_FLIPPED = 0, N48_FM_P_COPYFAIL = 1, N48_FM_P_HELD = 2, N48_FM_P_OFF = 3 };

/* One present, flip mode ON and ENGAGED (the kext engages before asking). *cstOut is the copy's scanout status when a copy was
 * made (FLIPPED, COPYFAIL), else 0. HELD and OFF made no copy; OFF also turned flip mode off (restoring unless the front was
 * foreign). */
static inline uint32_t n48_fm_present(n48_fm *f, const n48_fm_ops *o, uint32_t *cstOut)
{
    uint64_t front = 0u, back = 0u, prim = 0u, after = 0u;
    uint32_t us = 0u, polls = 0u, w;
    *cstOut = 0u;
    f->presents++;
    const uint32_t req = __atomic_load_n(&f->restoreReq, __ATOMIC_ACQUIRE);
    if (req) { (void)n48_fm_restore(f, o, req, 0u); return N48_FM_P_OFF; }
    if (!f->on || !f->engaged) return N48_FM_P_HELD;
    w = n48_fm_wait_latch(o, N48_FM_WAIT_MAX_US, &front, &us, &polls);
    if (w == N48_FM_W_READFAIL) { f->readFails++; (void)n48_fm_restore(f, o, N48_FM_OFF_READFAIL, 0u); return N48_FM_P_OFF; }
    if (w == N48_FM_W_TIMEOUT) {
        f->timeouts++;
        f->consecTimeouts++;
        f->front = front;
        /* 0.0.519 F1: the EARLIEST_INUSE the wait read last. Neither A nor B: somebody else owns the plane - the FOREIGN path
         * below (flip mode OFF, nothing written), exactly as a latched foreign front takes it. */
        if (n48_fm_select(front, f->aMc, f->bMc, &back) != 0u) {
            f->timeoutForeign++;
            f->foreign++;
            __atomic_store_n(&f->on, 0u, __ATOMIC_RELEASE);
            f->offWhy = N48_FM_OFF_FOREIGN;
            f->engaged = 0u;
            o->release(o->ctx);
            return N48_FM_P_OFF;
        }
        if (f->consecTimeouts >= N48_FM_STUCK_TIMEOUTS) {   /* 0.0.519 F1: a latch that never comes: back to A, OFF */
            f->stuckRestores++;
            f->consecTimeouts = 0u;
            (void)n48_fm_restore(f, o, N48_FM_OFF_STUCK, 0u);
            return N48_FM_P_OFF;
        }
        return N48_FM_P_HELD;
    }
    f->consecTimeouts = 0u;
    f->waits++;
    f->waitHist[n48_fm_wait_bucket(polls, us)]++;
    if (us > f->waitMaxUs) f->waitMaxUs = us;
    f->front = front;
    if (n48_fm_select(front, f->aMc, f->bMc, &back) != 0u) {
        f->foreign++;                                  /* neither A nor B: leave the plane alone */
        __atomic_store_n(&f->on, 0u, __ATOMIC_RELEASE);
        f->offWhy = N48_FM_OFF_FOREIGN;
        f->engaged = 0u;
        o->release(o->ctx);
        return N48_FM_P_OFF;
    }
    if (!n48_fm_flip_ok(f, back) || !n48_fm_dst_ok(f, n48_fm_off_of(f, back))) {
        (void)n48_fm_restore(f, o, N48_FM_OFF_DST, 0u);
        return N48_FM_P_OFF;
    }
    if (o->read_primary(o->ctx, &prim) != 0) { f->readFails++; (void)n48_fm_restore(f, o, N48_FM_OFF_READFAIL, 0u); return N48_FM_P_OFF; }
    if ((prim >> 32) != (back >> 32)) { f->highChanged++; (void)n48_fm_restore(f, o, N48_FM_OFF_HIGH, 0u); return N48_FM_P_OFF; }
    if (back == f->bMc) f->bHoldsPresent = 0u;         /* 0.0.519 F2: B is being rewritten; it holds a present only once done */
    *cstOut = o->copy_to(o->ctx, n48_fm_off_of(f, back));
    if (*cstOut != 0u && *cstOut != 11u) { f->copyFails++; return N48_FM_P_COPYFAIL; }
    if (back == f->bMc) f->bHoldsPresent = 1u;
    if (o->flip(o->ctx, back) != 0) { f->flipFails++; (void)n48_fm_restore(f, o, N48_FM_OFF_FLIPFAIL, 0u); return N48_FM_P_OFF; }
    f->flips++;
    if (back == f->aMc) f->flipsToA++; else f->flipsToB++;
    if (o->read_primary(o->ctx, &after) != 0 || after != back || (after >> 32) != (prim >> 32)) {
        f->highChanged++;
        (void)n48_fm_restore(f, o, N48_FM_OFF_HIGH, 0u);
        return N48_FM_P_OFF;
    }
    return N48_FM_P_FLIPPED;
}

/* A withdrawal (token / guard / keystone) asks for a restore; the next present or verb serves it. Lock-free on the submit path:
 * only a flag. Returns 1 when a request was recorded. */
static inline uint32_t n48_fm_request_restore(n48_fm *f, uint32_t why)
{
    if (!__atomic_load_n(&f->on, __ATOMIC_ACQUIRE)) return 0u;
    uint32_t expect = 0u;
    if (__atomic_compare_exchange_n(&f->restoreReq, &expect, why, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&f->restoreRequests, 1ull, __ATOMIC_RELAXED);
        return 1u;
    }
    return 0u;
}

/* ---- the report (bare `gfxneuter 74`): three lines, each under the 491-byte log body at 20-digit counters ------------------- */
#define N48_FM_REPORT1_FMT "flipmode74: switch 74 (flip mode) is %s%s; engaged %u; A MC %#llx B MC %#llx (vram+%#llx, " \
    "%llu bytes); front %#llx; last OFF: %s; restore pending %u"
#define N48_FM_REPORT2_FMT "flipmode74: OUR flips %llu (to B %llu, to A %llu) of %llu presents; latch waits %llu " \
    "[0:%llu <=1ms:%llu <=2:%llu <=4:%llu <=8:%llu <=17:%llu <=34:%llu] max %u us; held: timeouts %llu, copy failures %llu"
#define N48_FM_REPORT3_FMT "flipmode74: foreign fronts %llu, ADDRESS_HIGH changes %llu, flip-write failures %llu, read failures " \
    "%llu; restores verified %llu failed %llu (B->A copies %llu, requests %llu); engages %llu refused %llu; other scanout " \
    "writers held %llu; A/B tests %llu"
/* build 0.0.519. args: consecutive timeouts now, stuck restores, timed-out foreign fronts, B->A copy failures,
 * B->A copies skipped (B held no present), restores refused (A not settled), B holds a present, A copy pending. */
#define N48_FM_REPORT4_FMT "flipmode74: (0.0.519) latch timeouts in a row %u; stuck-latch restores %llu; foreign fronts after a " \
    "timeout %llu; B->A copy failures %llu (front kept), skipped (B held no present) %llu; restores refused (A not settled) " \
    "%llu; B holds a present %u; A copy pending %u"

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_FLIPMODE_H */
