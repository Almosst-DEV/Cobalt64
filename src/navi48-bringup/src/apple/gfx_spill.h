// gfx_spill.h — build 0.0.522 (notes/APPLE-DRIVER-VERDICT.md "FIRST BUILD",): THE KEXT SPILL TIER FOR
// DESCRIPTOR RECORDS, its slice table. Pure C, host-tested by tests/gfx_spill_test.cpp (with planted defects); the kext compiles
// the SAME header. DEFAULT-INERT: a zero-initialised table is unarmed, every slice FREE, and n48_sp_begin offers nothing.
//
// WHAT IT IS. A unit's deferred descriptor records (xlat12_ib.h XLAT12_EXTRA_UNIT P3) need only {host, va, len} runs to land in
// (xlat12_pool_run): nothing requires them INSIDE the IB. The kext reserves ONE fixed row of N48_SP_SLICES slices of
// N48_SP_SLICE_DW dwords in the root[511] ring-region relocation arena (gfx_reloc.h, n48_reloc_place, key N48_SP_KEY) at the arm,
// and each judged frame takes ONE free slice as a second xlat12_pool (xlat12_unit.spill) that the translator tries LAST. The
// records are built in a host SHADOW of the slice; after the frame's final statuses and BEFORE the IB write the kext MM-writes the
// used dwords into VRAM and reads them back (a mismatch refuses the frame).
//
// THE SLICE LIFECYCLE (the safety property: a slice is never rewritten while a frame that may still execute reads it):
//   FREE        nothing references it.
//   BUILDING    taken by the current judged frame (n48_sp_begin); its translation may place records into the shadow.
//   INFLIGHT    the frame was stamped at the gate (n48_sp_tag with the flight ring's seq and a real owned-slot fence).
//   QUARANTINE  its flight EXPIRED (we gave up waiting: the frame may still run). Freed only at DISARM (n48_sp_disarm).
//   LEAKED      a stamped frame WITHOUT a fence (it can never retire by fence): never freed in this first build; counted.
// Transitions that FREE a slice, and the only ones:
//   BUILDING -> FREE    the frame was never stamped (n48_sp_begin of the NEXT pass, or disarm): no ring entry exists for it,
//                       so hook_gfxCommitIB's flight-ring guard refuses its keystone and the frame is neutered at the source.
//   INFLIGHT -> FREE    its flight RETIRED (its own fence read OURS) or was NOPED (switch 75's walk proof) - n48_sp_sync from
//                       the ring; or a keystone REFUSAL of the frame's OWN seq - n48_sp_request from hook_gfxCommitIB,
//                       applied under gXdLock by the next n48_sp_begin.
//   QUARANTINE -> FREE  disarm only.
// A ringmap rebuild or a move of the ring VA base (n48_sp_begin's vaBase/carve compare) EMPTIES the table: the row belongs to a
// mapping that no longer names it. NOT_RUN is NEVER a reason to free, from the ring or from the hook (fix pass M1): an unproven
// NOT_RUN (75 OFF, a failed proof, a token mismatch on the SHARED gXdCmToken.seq, a SPARED record overwritten by a later walk) may
// be a frame the CP runs. Such a slice waits for EXPIRED -> QUARANTINE; a PROVEN NOP frees as NOPED.
#ifndef N48_GFX_SPILL_H
#define N48_GFX_SPILL_H

#include <stdint.h>
#include "xlat12_ib.h"        /* xlat12_pool, XLAT12_IB_NOP, XLAT12_RELOC_ALIGN */
#include "gfx_flightring.h"   /* n48_fr_ring and its states (RETIRED / NOPED / EXPIRED) */

#ifdef __cplusplus
extern "C" {
#endif

#define N48_SP_SLICES     8u
#define N48_SP_SLICE_DW   512u
#define N48_SP_SLICE_BYTES (N48_SP_SLICE_DW * 4u)                  /* 2 KiB */
#define N48_SP_ROW_BYTES  (N48_SP_SLICES * N48_SP_SLICE_BYTES)     /* 16 KiB of the 56 KiB arena */
#define N48_SP_KEY        0x5350494C4C353232ull                    /* "SPILL522": our own arena key, never a shader-cache key */
#define N48_SP_NONE       0xFFFFFFFFu

enum { N48_SP_FREE = 0, N48_SP_BUILDING, N48_SP_INFLIGHT, N48_SP_QUARANTINE, N48_SP_LEAKED, N48_SP_STATES };
/* hook requests (n48_sp_request): why an INFLIGHT slice's frame can never run */
enum { N48_SP_REQ_NONE = 0, N48_SP_REQ_KS_REFUSED, N48_SP_REQ_KINDS };

static inline const char *n48_sp_state_name(uint32_t s)
{
    static const char *const n[N48_SP_STATES] = { "FREE", "BUILDING", "INFLIGHT", "QUARANTINE", "LEAKED" };
    return s < N48_SP_STATES ? n[s] : "?";
}

typedef struct {
    uint32_t armed;                    /* a row is reserved for the current arm */
    uint64_t rowVa, rowVram;           /* slice 0's GPU VA and VRAM offset */
    uint64_t vaBase, carve;            /* the ring region (gRingMap.vaBase / .base) the row belongs to */
    uint32_t cur;                      /* the BUILDING slice, N48_SP_NONE when none */
    volatile uint32_t st[N48_SP_SLICES];
    uint32_t seq[N48_SP_SLICES];
    volatile uint32_t req[N48_SP_SLICES];   /* written by hook_gfxCommitIB (no lock), consumed under gXdLock */
    /* read-only counters (the `spill76:` line) */
    uint64_t arms, takes, noFree, unstamped, tagged, leaked, freedRetired, freedNoped, freedKs, quarantined,
             unquarantined, empties, frames, dwSum;
    uint32_t dwMax;
} n48_sp_table;

static inline uint64_t n48_sp_slice_va(const n48_sp_table *t, uint32_t s)   { return t->rowVa + (uint64_t)s * N48_SP_SLICE_BYTES; }
static inline uint64_t n48_sp_slice_vram(const n48_sp_table *t, uint32_t s) { return t->rowVram + (uint64_t)s * N48_SP_SLICE_BYTES; }

/* Every slice FREE, the row forgotten, unarmed (a ringmap rebuild / base move, or the first arm). */
static inline void n48_sp_empty(n48_sp_table *t)
{
    if (!t) return;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++) { t->st[s] = N48_SP_FREE; t->seq[s] = 0u; t->req[s] = N48_SP_REQ_NONE; }
    t->armed = 0u; t->cur = N48_SP_NONE; t->rowVa = 0ull; t->rowVram = 0ull; t->vaBase = 0ull; t->carve = 0ull;
    t->empties++;
}

/* The arm reserved the row: slice 0 at GPU VA `rowVa`, VRAM offset `rowVram`, under ring region (vaBase, carve). The same row as
 * before keeps every slice's state (an INFLIGHT or QUARANTINE slice of an earlier arm is still protected); a DIFFERENT row empties
 * the table first (the old row's memory belongs to a mapping or placement that no longer names it). Returns 1 when armed. */
static inline uint32_t n48_sp_arm(n48_sp_table *t, uint64_t rowVa, uint64_t rowVram, uint64_t vaBase, uint64_t carve)
{
    if (!t || !rowVa || !vaBase || (rowVa & (uint64_t)(XLAT12_RELOC_ALIGN - 1u)) || rowVa >= (1ull << 48)) return 0u;
    if (t->rowVa != rowVa || t->rowVram != rowVram || t->vaBase != vaBase || t->carve != carve) {
        for (uint32_t s = 0; s < N48_SP_SLICES; s++) if (t->st[s] != N48_SP_FREE) return 0u;   /* a frame may still read the old row */
        n48_sp_empty(t);
        t->rowVa = rowVa; t->rowVram = rowVram; t->vaBase = vaBase; t->carve = carve;
    }
    t->armed = 1u; t->arms++;
    return 1u;
}

/* The arm ended: nothing more is taken; QUARANTINE ends ( "EXPIRED quarantines until disarm"); a BUILDING slice was never
 * stamped. INFLIGHT and LEAKED slices keep their state (their frames may still run). */
static inline void n48_sp_disarm(n48_sp_table *t)
{
    if (!t) return;
    t->armed = 0u;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++) {
        if (t->st[s] == N48_SP_QUARANTINE) { t->st[s] = N48_SP_FREE; t->seq[s] = 0u; t->unquarantined++; }
        else if (t->st[s] == N48_SP_BUILDING) { t->st[s] = N48_SP_FREE; t->seq[s] = 0u; t->unstamped++; }
    }
    t->cur = N48_SP_NONE;
}

/* The flight ring's own verdicts for every INFLIGHT slice (called under gXdLock, before any reclamation can erase them):
 * RETIRED or NOPED frees; EXPIRED quarantines; anything else (PENDING, COMMITTED, NOT_RUN, absent) keeps it. Returns the
 * slices whose state changed. */
static inline uint32_t n48_sp_sync(n48_sp_table *t, const n48_fr_ring *r)
{
    uint32_t n = 0u;
    if (!t || !r) return 0u;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++) {
        if (t->st[s] != N48_SP_INFLIGHT) continue;
        uint32_t idx = 0u;
        if (!n48_fr_find(r, t->seq[s], &idx)) continue;
        const uint32_t es = r->e[idx].state;
        if (es == N48_FR_RETIRED)      { t->st[s] = N48_SP_FREE; t->req[s] = N48_SP_REQ_NONE; t->freedRetired++; n++; }
        else if (es == N48_FR_NOPED)   { t->st[s] = N48_SP_FREE; t->req[s] = N48_SP_REQ_NONE; t->freedNoped++; n++; }
        else if (es == N48_FR_EXPIRED) { t->st[s] = N48_SP_QUARANTINE; t->req[s] = N48_SP_REQ_NONE; t->quarantined++; n++; }
    }
    return n;
}

/* hook_gfxCommitIB (no gXdLock): the frame of token seq `seq` can never run - its keystone refused it, or the ring walk refused
 * and NOPed it (NOT_RUN of its OWN seq). Only records the request on the INFLIGHT slice carrying exactly that seq; the state
 * changes at the next n48_sp_begin. Returns 1 when a slice carries it. */
static inline uint32_t n48_sp_request(n48_sp_table *t, uint32_t seq, uint32_t why)
{
    if (!t || !seq || why == N48_SP_REQ_NONE || why >= N48_SP_REQ_KINDS) return 0u;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++)
        if (t->st[s] == N48_SP_INFLIGHT && t->seq[s] == seq) {
            __atomic_store_n(&t->req[s], why, __ATOMIC_RELEASE);
            return 1u;
        }
    return 0u;
}

/* The top of a judged frame's policy pass (under gXdLock). (1) hook requests are applied; (2) the ring's verdicts (sync);
 * (3) a slice still BUILDING from an earlier pass was never stamped: FREE; (4) a ringmap rebuild or base move empties the table;
 * (5) when `on` (switch 76, latched for this pass), the table is armed and `row_ok` (the arena still holds the row where it was
 * reserved), the first FREE slice becomes BUILDING and is returned; none free -> N48_SP_NONE (the tier offers nothing: NO_ROOM
 * as today). */
static inline uint32_t n48_sp_begin(n48_sp_table *t, const n48_fr_ring *r, uint32_t on, uint32_t built, uint64_t vaBase,
                                    uint64_t carve, uint32_t row_ok)
{
    if (!t) return N48_SP_NONE;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++) {
        const uint32_t q = __atomic_load_n(&t->req[s], __ATOMIC_ACQUIRE);
        if (q == N48_SP_REQ_NONE) continue;
        if (t->st[s] == N48_SP_INFLIGHT) {
            t->st[s] = N48_SP_FREE;
            t->freedKs++;
        }
        t->req[s] = N48_SP_REQ_NONE;
    }
    (void)n48_sp_sync(t, r);
    if (t->cur != N48_SP_NONE && t->cur < N48_SP_SLICES && t->st[t->cur] == N48_SP_BUILDING) {
        t->st[t->cur] = N48_SP_FREE; t->unstamped++;
    }
    t->cur = N48_SP_NONE;
    if (t->rowVa && (!built || vaBase != t->vaBase || carve != t->carve)) n48_sp_empty(t);
    if (!on || !t->armed || !row_ok) return N48_SP_NONE;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++)
        if (t->st[s] == N48_SP_FREE) { t->st[s] = N48_SP_BUILDING; t->seq[s] = 0u; t->cur = s; t->takes++; return s; }
    t->noFree++;
    return N48_SP_NONE;
}

/* The BUILDING slice as the translator's second pool: one run of the whole slice over the shadow (NOP-filled). */
static inline void n48_sp_pool_init(const n48_sp_table *t, uint32_t s, xlat12_pool *pl, uint32_t *shadow)
{
    if (!pl) return;
    pl->nrun = 0u; pl->lost = 0u; pl->jn = 0u;
    if (!t || s >= N48_SP_SLICES || !shadow) return;
    for (uint32_t k = 0; k < N48_SP_SLICE_DW; k++) shadow[k] = XLAT12_IB_NOP;
    pl->run[0].host = shadow; pl->run[0].va = n48_sp_slice_va(t, s); pl->run[0].len = N48_SP_SLICE_DW;
    pl->nrun = 1u;
}

/* The dwords the frame's final translations left in the slice: the run's cursor, proven consistent with the slice (host,
 * VA and length move together, every used dword inside the slice). N48_SP_NONE when anything disagrees (the frame refuses). */
static inline uint32_t n48_sp_used(const n48_sp_table *t, uint32_t s, const xlat12_pool *pl, const uint32_t *shadow)
{
    if (!t || !pl || !shadow || s >= N48_SP_SLICES || pl->nrun != 1u) return N48_SP_NONE;
    const xlat12_pool_run *pr = &pl->run[0];
    if (pr->host < shadow || pr->host > shadow + N48_SP_SLICE_DW) return N48_SP_NONE;
    const uint32_t used = (uint32_t)(pr->host - shadow);
    if (pr->len != N48_SP_SLICE_DW - used) return N48_SP_NONE;
    if (pr->va != n48_sp_slice_va(t, s) + 4ull * used) return N48_SP_NONE;
    return used;
}

/* 1 when [va, va + 4*ndw) lies inside slice s of the row (the kext's own check before every MM write). */
static inline uint32_t n48_sp_in_slice(const n48_sp_table *t, uint32_t s, uint64_t va, uint32_t ndw)
{
    if (!t || s >= N48_SP_SLICES) return 0u;
    const uint64_t a = n48_sp_slice_va(t, s);
    return (va >= a && ndw <= N48_SP_SLICE_DW && va + 4ull * ndw <= a + N48_SP_SLICE_BYTES) ? 1u : 0u;
}

/* The gate stamped the current frame (the flight ring's push succeeded) with token seq `seq`; `fenced` = its entry carries a real
 * owned-slot fence (want != 0). BUILDING -> INFLIGHT, or LEAKED for a fence-less frame (never freed in this first build).
 * Returns the slice, or N48_SP_NONE when there is no BUILDING slice. */
static inline uint32_t n48_sp_tag(n48_sp_table *t, uint32_t seq, uint32_t fenced)
{
    if (!t || t->cur >= N48_SP_SLICES || t->st[t->cur] != N48_SP_BUILDING || !seq) return N48_SP_NONE;
    const uint32_t s = t->cur;
    t->seq[s] = seq; t->req[s] = N48_SP_REQ_NONE;
    if (fenced) { t->st[s] = N48_SP_INFLIGHT; t->tagged++; }
    else { t->st[s] = N48_SP_LEAKED; t->leaked++; }
    t->cur = N48_SP_NONE;
    return s;
}

/* THE FLUSH (the kext's MM window through `wr`/`rd`, at most 64 dwords per call): write shadow[0..used) to slice s's VRAM, then
 * read EVERY written dword back into `back` (64 dwords) - a different buffer - and compare. Returns 0 when every dword read back
 * equal; 1 a write or read the window refused; 2 a mismatch; 3 bad arguments or a range outside the slice. Nothing is compared
 * against itself: the read-back is what proves the records the GPU will fetch are the ones the translator placed. */
typedef uint32_t (*n48_sp_io_fn)(void *ctx, uint64_t vram, uint32_t *buf, uint32_t ndw);
static inline uint32_t n48_sp_flush(const n48_sp_table *t, uint32_t s, const uint32_t *shadow, uint32_t used,
                                    n48_sp_io_fn wr, n48_sp_io_fn rd, void *ctx, uint32_t *back)
{
    if (!t || s >= N48_SP_SLICES || !shadow || !wr || !rd || !back || used == 0u || used > N48_SP_SLICE_DW) return 3u;
    if (!n48_sp_in_slice(t, s, n48_sp_slice_va(t, s), used)) return 3u;
    const uint64_t vram = n48_sp_slice_vram(t, s);
    for (uint32_t i = 0; i < used; i += 64u) {
        const uint32_t nn = (used - i) < 64u ? (used - i) : 64u;
        if (!wr(ctx, vram + 4ull * i, (uint32_t *)&shadow[i], nn)) return 1u;
    }
    for (uint32_t i = 0; i < used; i += 64u) {
        const uint32_t nn = (used - i) < 64u ? (used - i) : 64u;
        if (!rd(ctx, vram + 4ull * i, back, nn)) return 1u;
        for (uint32_t k = 0; k < nn; k++) if (back[k] != shadow[i + k]) return 2u;
    }
    return 0u;
}

/* The current frame placed nothing in its slice (every spill record was undone, or none was ever needed): BUILDING -> FREE at
 * once, never stamped. Returns 1 when a slice was released. */
static inline uint32_t n48_sp_release_unused(n48_sp_table *t)
{
    if (!t || t->cur >= N48_SP_SLICES || t->st[t->cur] != N48_SP_BUILDING) { if (t) t->cur = N48_SP_NONE; return 0u; }
    t->st[t->cur] = N48_SP_FREE; t->seq[t->cur] = 0u; t->cur = N48_SP_NONE;
    return 1u;
}

/* 1 when the spill tier holds the arena: armed, or any slice not FREE (fix pass M3b: arena placements are refused then). */
static inline uint32_t n48_sp_busy(const n48_sp_table *t)
{
    if (!t) return 0u;
    if (t->armed) return 1u;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++) if (t->st[s] != N48_SP_FREE) return 1u;
    return 0u;
}

/* Slices in each state (for the report). */
static inline void n48_sp_count(const n48_sp_table *t, uint32_t out[N48_SP_STATES])
{
    for (uint32_t i = 0; i < N48_SP_STATES; i++) out[i] = 0u;
    if (!t) return;
    for (uint32_t s = 0; s < N48_SP_SLICES; s++) if (t->st[s] < N48_SP_STATES) out[t->st[s]]++;
}

#define N48_SP_FMT "spill76: %s%s - row VA %#llx vram+%#llx %s; slices FREE %u BUILDING %u INFLIGHT %u QUARANTINE %u LEAKED %u; " \
                   "arms %llu takes %llu no-free %llu frames-used %llu dw sum %llu max %u; tagged %llu leaked(no fence) %llu " \
                   "unstamped %llu; freed retired %llu noped %llu keystone %llu; quarantined %llu released %llu; " \
                   "empties %llu"

#ifdef __cplusplus
}
#endif

#endif
