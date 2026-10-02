// gfx_rpunmapq.h — build 0.0.453 item 6 (reviewer findings on 0.0.452). Pure C11 (atomics only, no kernel
// header, no IOLock), so the kext and tests/gfx_rpunmapq_test.cpp compile the IDENTICAL code.
//
// THE BUG THIS CLOSES (0.0.452, gfxsrc_desc_unmap's "lock busy" branch, AppleHardwareHook.cpp):
//   (a) that branch pushed into a bounded array (`gXdRpUnmapQ`/`gXdRpUnmapQN`) WITHOUT gXdLock, precisely because
//       gXdLock was busy - held by whichever thread is running gfxsrc_rp_drain_locked() RIGHT NOW, which reads
//       `gXdRpUnmapQN`, walks the array, then resets the count to 0. A push and a drain can interleave on the
//       array/count with no ordering at all: a push's `gXdRpUnmapQ[n] = entry` and `n++` are two separate,
//       unsynchronised memory operations, so a drain that reads `n` between them, or two pushes that both read the
//       same stale `n`, can silently drop an entry (a lost push) or have two writers land on the SAME slot.
//   (b) the overflow fallback remembered ONE context (`gXdRpUnmapDirtyCtx = ctxSeq`): if a SECOND, DIFFERENT
//       context overflowed the bounded queue before the next locked drain, its assignment overwrote the first's,
//       and the first context's entries were never wiped at all - a fail-OPEN (a stale ledger entry answers a
//       later ask as if the unmap that should have dropped it never happened).
//
// THE FIX. A dedicated spinlock - NEVER gXdLock itself, so the busy branch never blocks on the very lock it found
// busy - guards a tiny, fixed critical section (a handful of field writes) on BOTH sides: n48_rpuq_push (the busy
// branch, no gXdLock) and n48_rpuq_take (the drain, already under gXdLock - the ONE consumer, never concurrent with
// another drain by construction). Holding the SAME lock on both sides makes "push completes AND THEN is drained,
// or has not yet started AND is drained next time" the only two orderings - a push can never be observed half
// written, and can never be silently reset out from under it. Overflow is now a single STICKY BOOLEAN, not a
// remembered context: n48_rpuq_take always reports it and always clears it, so the caller's fail-closed action
// (wipe the WHOLE ledger, not one context) covers every context that overflowed since the last drain, however many
// there were.
//
// THE PRE-0.0.455 CAP (8): decide45 (notes/logs/runs/decide45/, build 0.0.455 item 5) hit this queue's own
// overflow 1,379 times in one boot - the resprov ledger was RECORDED 8, dropped 8, proven 0 (decide44 had proven
// 28), because EVERY overflow wiped the WHOLE ledger (item 6b's own fail-closed fallback), refusing PROVENANCE for
// every Family A segment even though almost none of the wiped entries were ever touched by the unmap that
// overflowed. TWO FIXES: (a) N48_RPUQ_CAP raised to 256 (measure), so an overflow is rare in the first place; (b)
// when one still happens, the queue ALSO remembers a per-context BOUNDING RANGE of what could not be queued
// (`dirty[]`), so the caller's fail-closed action can be `n48_rp_unmap_rng` (drop only ledger entries the bound
// could overlap) instead of a blind `n48_rp_unmap(&gXdRp, 0u)` (drop everything). If MORE DISTINCT CONTEXTS
// overflow than `dirty[]` can name (N48_RPUQ_DIRTY_CTX_MAX), tracking itself gives up and the caller falls back to
// the ORIGINAL whole-ledger wipe - fail-closed, never fail-open, for whatever this queue's own bookkeeping cannot
// vouch for. WHAT THIS DOES NOT CHANGE: gXdLock's own scope, or the drain's own per-entry unmap call
// (n48_rp_unmap_rng) - only how entries (and now bounding ranges) get from the busy branch into the drain safely.
#ifndef N48_GFX_RPUNMAPQ_H
#define N48_GFX_RPUNMAPQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N48_RPUQ_CAP 256u
// build 0.0.455 item 5: the number of DISTINCT overflowing contexts `dirty[]` can name before tracking itself
// gives up (see the header banner above). Small on purpose - a real boot overflows at most a handful of distinct
// WindowServer contexts at once even at CAP 8 (decide45's 1,379 overflows were almost all the SAME context), and a
// bounded array keeps this struct's own size small and its walk O(1) in the push's own critical section.
#define N48_RPUQ_DIRTY_CTX_MAX 4u

typedef struct { uint32_t ctx; uint64_t va, size; } n48_rpuq_entry;
// build 0.0.455 item 5: one context's own UNION of every unmap it could not be queued for since the last
// drain - [vaMin, vaEnd). Never empty (see n48_rpuq_push's own comment: a size-0 unmap still claims one byte, so
// vaEnd > vaMin always holds for a tracked entry - n48_rp_unmap_rng's OWN "size 0 = whole context" convention is
// never triggered by accident here).
typedef struct { uint32_t ctx; uint64_t vaMin, vaEnd; } n48_rpuq_dirty;

typedef struct {
    volatile uint32_t lock;         // 0 free, 1 held. NEVER gXdLock - a dedicated lock the busy branch can take.
    uint32_t n;                     // entries queued right now; touched only while `lock` is held
    n48_rpuq_entry q[N48_RPUQ_CAP];
    uint32_t overflow;              // item 6b: ANY overflow since the last take() - not a remembered context
    n48_rpuq_dirty dirty[N48_RPUQ_DIRTY_CTX_MAX];   // item 5: each overflowing context's own bounding range
    uint32_t dirtyN;                // item 5: contexts currently held in dirty[] (0..N48_RPUQ_DIRTY_CTX_MAX)
    uint32_t dirtyOverflow;         // item 5: 1 = a context overflowed with no room left in dirty[] to track it
    struct { uint64_t pushed, overflowed, drained, wipeAsked, lockSpins, wholeWipes, boundedWipes; } s;
} n48_rpuq;

static inline void n48_rpuq_init(n48_rpuq *q)
{
    if (!q) return;
    q->lock = 0u; q->n = 0u; q->overflow = 0u; q->dirtyN = 0u; q->dirtyOverflow = 0u;
    for (uint32_t i = 0; i < N48_RPUQ_DIRTY_CTX_MAX; i++) { q->dirty[i].ctx = 0u; q->dirty[i].vaMin = 0ull; q->dirty[i].vaEnd = 0ull; }
    q->s.pushed = 0u; q->s.overflowed = 0u; q->s.drained = 0u; q->s.wipeAsked = 0u; q->s.lockSpins = 0u;
    q->s.wholeWipes = 0u; q->s.boundedWipes = 0u;
}

// A plain CAS spinlock. The critical sections it ever guards are a handful of field writes (never a wait on Apple,
// on gXdLock, or on I/O), so an unbounded spin here is the same cost class as any other kernel spinlock - real
// contention is a handful of retries at worst. `s.lockSpins` is counted, never used to give up: BOTH push and take
// MUST eventually get this lock, or an entry (push) or a reset (take) is lost - the exact bug this header exists
// to close.
static inline void n48_rpuq_lock(n48_rpuq *q)
{
    uint32_t expected;
    for (;;) {
        expected = 0u;
        if (__atomic_compare_exchange_n(&q->lock, &expected, 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
        __atomic_fetch_add(&q->s.lockSpins, 1u, __ATOMIC_RELAXED);
    }
}
static inline void n48_rpuq_unlock(n48_rpuq *q) { __atomic_store_n(&q->lock, 0u, __ATOMIC_RELEASE); }

// The BUSY branch: gXdLock is held by someone else, so this takes ONLY the dedicated lock above. Returns 1 if the
// entry was queued, 0 if the queue was full - either way `q->overflow` is left correct (set on the 0 path, and
// NEVER cleared here: only take() clears it, so two different overflowing contexts before the next drain both
// leave it set, and the drain's fail-closed wipe covers both).
static inline int n48_rpuq_push(n48_rpuq *q, uint32_t ctx, uint64_t va, uint64_t size)
{
    int ok;
    n48_rpuq_lock(q);
    if (q->n < N48_RPUQ_CAP) {
        q->q[q->n].ctx = ctx; q->q[q->n].va = va; q->q[q->n].size = size;
        q->n++; q->s.pushed++; ok = 1;
    } else {
        q->overflow = 1u; q->s.overflowed++; ok = 0;
        // build 0.0.455 item 5: remember (or extend) THIS context's own bounding range, so the drain can drop
        // only what it must, not the whole ledger. `end`: a size-0 unmap still claims one byte (never an empty
        // range - see n48_rpuq_dirty's own comment on why that matters for n48_rp_unmap_rng's convention).
        const uint64_t end = size ? va + size : va + 1ull;
        uint32_t found = 0;
        for (uint32_t i = 0; i < q->dirtyN; i++) {
            if (q->dirty[i].ctx == ctx) {
                if (va < q->dirty[i].vaMin) q->dirty[i].vaMin = va;
                if (end > q->dirty[i].vaEnd) q->dirty[i].vaEnd = end;
                found = 1;
                break;
            }
        }
        if (!found) {
            if (q->dirtyN < N48_RPUQ_DIRTY_CTX_MAX) {
                q->dirty[q->dirtyN].ctx = ctx; q->dirty[q->dirtyN].vaMin = va; q->dirty[q->dirtyN].vaEnd = end;
                q->dirtyN++;
            } else {
                q->dirtyOverflow = 1u;   // a NEW context and dirty[] has no room left - the drain must wipe everything
            }
        }
    }
    n48_rpuq_unlock(q);
    return ok;
}

// The DRAIN, already under gXdLock (the one consumer). Pops ONE entry (FIFO - q->q[0], the rest shifted down) per
// call, atomically with respect to a concurrent push (both sides hold the SAME lock). A caller drains the whole
// queue with `while (n48_rpuq_pop(q, &ent)) { ...apply ent...; }`. Deliberately ONE entry per lock/unlock, not a
// snapshot-into-a-local-array: the stack budget for the whole submit chain is +0x40 (notes' own D6 rule,
// gfx_mmprio_test.cpp's T6), and `n48_rpuq_entry ent;` (24 bytes) fits it where `n48_rpuq_entry ent[N48_RPUQ_CAP]`
// (6144 bytes at build 0.0.455's CAP of 256, an earlier draft of this header used a 192-byte version at CAP
// 8) would not - unmap events are not the per-draw hot path, so the O(1)-amortised shift and the up-to-CAP
// lock/unlock pairs cost nothing that matters here, whatever CAP is.
static inline int n48_rpuq_pop(n48_rpuq *q, n48_rpuq_entry *out)
{
    int got = 0;
    n48_rpuq_lock(q);
    if (q->n) {
        *out = q->q[0];
        for (uint32_t i = 1; i < q->n; i++) q->q[i - 1] = q->q[i];
        q->n--;
        got = 1;
    }
    n48_rpuq_unlock(q);
    if (got) q->s.drained++;
    return got;
}
// Reads and clears the sticky overflow flag (item 6b) - a separate, tiny critical section from n48_rpuq_pop's own,
// called once per drain AFTER the pop loop empties the queue (so an overflow a push set while THIS drain was
// already running is still seen: it can only have happened before the pop loop observed an empty queue, because
// both sides hold the same lock).
static inline uint32_t n48_rpuq_take_overflow(n48_rpuq *q)
{
    n48_rpuq_lock(q);
    const uint32_t o = q->overflow;
    q->overflow = 0u;
    n48_rpuq_unlock(q);
    if (o) q->s.wipeAsked++;
    return o;
}
// build 0.0.455 item 5: reads and clears the dirty-context bounding-range table - a separate, tiny critical
// section from n48_rpuq_pop's and n48_rpuq_take_overflow's own, called ONCE per drain, same discipline (any
// context that overflowed while THIS drain was already running is still caught: it can only have happened before
// this call observed the table, because both sides hold the same lock). `out` must have room for at least
// N48_RPUQ_DIRTY_CTX_MAX entries; `*outN` is set to how many were filled (0 when nothing overflowed, or when
// every overflow this cycle went through `*wholeWipe` instead). `*wholeWipe` is 1 when MORE DISTINCT CONTEXTS
// overflowed than `dirty[]` could track - the caller MUST fall back to wiping every context in that case (this
// queue's own bookkeeping cannot vouch for a bound), and `*outN` entries (if any) should still be applied too,
// since they name real, separately-tracked bounds even when the wipe also covers them redundantly.
static inline void n48_rpuq_take_dirty(n48_rpuq *q, n48_rpuq_dirty *out, uint32_t *outN, int *wholeWipe)
{
    n48_rpuq_lock(q);
    const uint32_t n = q->dirtyN;
    for (uint32_t i = 0; i < n; i++) out[i] = q->dirty[i];
    *outN = n;
    *wholeWipe = (int)q->dirtyOverflow;
    q->dirtyN = 0u; q->dirtyOverflow = 0u;
    for (uint32_t i = 0; i < N48_RPUQ_DIRTY_CTX_MAX; i++) { q->dirty[i].ctx = 0u; q->dirty[i].vaMin = 0ull; q->dirty[i].vaEnd = 0ull; }
    n48_rpuq_unlock(q);
}

#ifdef __cplusplus
}
#endif
#endif /* N48_GFX_RPUNMAPQ_H */
