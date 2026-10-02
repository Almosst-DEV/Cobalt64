// gfx_mmhold.h — build 0.0.514 A3: WHO HOLDS gVramMmLock, AND FOR HOW LONG. READ-ONLY INSTRUMENT.
//
// fc3 measured the fast copy's sampled verify WAITING 22.85 s for gVramMmLock (the MM_INDEX/MM_DATA window's mutex) and
// could attribute only part of the holders: ~15 s of hold time sat outside every instrumented scope. This header is the pure
// half of a per-taker hold-time record: every navi48_vram_read_mm / navi48_vram_write_mm call and both fast-copy verify
// readers note (after IOLockUnlock) how long they held the lock, under the TAKER of the calling thread - the innermost
// Navi48MmTakerScope open on that thread (Navi48Ttl.hpp), or OTHER when none is. The report prints count / total / max per
// taker on the bare `gfxneuter 63` read (navi48_fc_report).
//
// Nothing here takes a lock, reads the clock or touches a register: the caller hands in its thread identity (uintptr_t) and
// the hold time in nanoseconds. Counters are relaxed atomics (report-only numbers; nothing branches on them). The per-thread
// taker table is claimed by compare-and-swap; only the owning thread writes its slot's tag. A thread that finds no free slot
// is counted in `overflow` and its holds land under OTHER - attribution degrades, nothing else changes.
// Host-tested in tests/gfx_copyguard_test.cpp (T27).
#ifndef N48_GFX_MMHOLD_H
#define N48_GFX_MMHOLD_H

#include <stdint.h>

#define N48_MMT_OTHER      0u   // no scope open on this thread: verbs, ring-map setup, the lutfill thread, the IB dump, ...
#define N48_MMT_JUDGE      1u   // gfxsrc_policy's pass (the judge: gfxc_page walks and data reads, descriptor reads, profiles)
#define N48_MMT_DECIDE     2u   // gfxsrc_decide_frame outside the policy and the commit (its IB reads and scans)
#define N48_MMT_COMMIT     3u   // gfxsrc_commit_try
#define N48_MMT_CAPTURE    4u   // gcap_submission (the capture stream)
#define N48_MMT_FCVERIFY   5u   // the fast copy's verify readers (fc_vram_read_timed, fc_vram_read_sampled_timed)
#define N48_MMT_RESIDENCY  6u   // residency_copy_to_vram (the MM loop's writes and read-backs; its pre-flight reads)
// build 0.0.515 D2: DECIDE's SUB-TAGS, pushed only over a DECIDE (or DECIDE-sub) tag by n48_mmt_push_sub,
// so the same code reached from the judge or the commit keeps THEIR tag. DECIDE itself is then "decide, other".
#define N48_MMT_D_FILL     7u   // gfxsrc_identify_pgm's switch-33 fill read (gfxsrc_read_program before the memo)
#define N48_MMT_D_PGM      8u   // gfxsrc_identify_pgm after it: the memo's one-dword check and a re-identification's read
#define N48_MMT_D_IB       9u   // the IB gather's gfxc_read (IB bodies are host memory: the MM time is their page walk)
#define N48_MMT_D_TGT     10u   // colour-target walks: the item loop's gfxc_page, switch 54's per-draw walk, R5 / the ledger
#define N48_MMT_D_FENCE   11u   // the fence828 watch poll and the flight ring's owned-slot polls
// build 0.0.540 (; switch 96, gfx_perf540.h): JUDGE's SUB-TAGS, pushed only over a JUDGE (or JUDGE-sub) tag by
// n48_mmt_push_sub, and only while switch 96 is ON (Navi48Ttl.hpp Navi48PfSubScope; gfxc_page's ON path): OFF nothing is ever
// tagged with them, so every hold lands where 0.0.539 put it. JUDGE itself is then "judge, other".
#define N48_MMT_J_WALK    12u   // gfxc_page's page-table walk reads under the judge (the level reads, not the data)
#define N48_MMT_J_PGMID   13u   // gfxsrc_pgm_profile (program identification: the memo, the head-first and full reads)
#define N48_MMT_J_DESC    14u   // gfxsrc_desc_read (the descriptor records' reads)
#define N48_MMT_J_ASK     15u   // the provenance asks (gfxsrc_desc_tiled_ok / _okt, gfxsrc_desc_dcc_ok)
#define N48_MMT_N         16u
#define N48_MMT_LOOKUP     0xFFu   // "look the calling thread up" (navi48_vram_read_mm / write_mm)
#define N48_MMT_SLOTS      16u

typedef struct { uintptr_t thread; uint32_t tag, pad; } n48_mmt_slot;
typedef struct {
    n48_mmt_slot s[N48_MMT_SLOTS];
    uint32_t live;          // claimed slots: 0 = the lookup answers OTHER without scanning
    uint32_t pad;
    uint64_t overflow;      // pushes that found no free slot
} n48_mmt_table;

// The token a push returns and its pop consumes: 0x100 | the previous tag when this thread already had a slot (a nested
// scope), 0x200 when the push claimed the slot (the pop releases it), 0 when no slot could be claimed (the pop does nothing).
#define N48_MMT_TOK_NESTED  0x100u
#define N48_MMT_TOK_CLAIMED 0x200u

static inline n48_mmt_slot *n48_mmt_find(n48_mmt_table *t, uintptr_t me)
{
    if (!t || !me) return 0;
    for (uint32_t i = 0; i < N48_MMT_SLOTS; i++)
        if (__atomic_load_n(&t->s[i].thread, __ATOMIC_ACQUIRE) == me) return &t->s[i];
    return 0;
}

static inline uint32_t n48_mmt_push(n48_mmt_table *t, uintptr_t me, uint32_t tag)
{
    if (!t || !me || tag >= N48_MMT_N) return 0u;
    n48_mmt_slot *m = n48_mmt_find(t, me);
    if (m) {
        const uint32_t prev = __atomic_load_n(&m->tag, __ATOMIC_RELAXED);
        __atomic_store_n(&m->tag, tag, __ATOMIC_RELAXED);
        return N48_MMT_TOK_NESTED | (prev & 0xFFu);
    }
    for (uint32_t i = 0; i < N48_MMT_SLOTS; i++) {
        uintptr_t want = 0;
        if (__atomic_load_n(&t->s[i].thread, __ATOMIC_RELAXED) != 0) continue;
        if (__atomic_compare_exchange_n(&t->s[i].thread, &want, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            // only this thread ever looks this slot up (by its own identity), and it does so after this store
            __atomic_store_n(&t->s[i].tag, tag, __ATOMIC_RELAXED);
            __atomic_fetch_add(&t->live, 1u, __ATOMIC_RELEASE);
            return N48_MMT_TOK_CLAIMED;
        }
    }
    __atomic_fetch_add(&t->overflow, 1ull, __ATOMIC_RELAXED);
    return 0u;
}

static inline void n48_mmt_pop(n48_mmt_table *t, uintptr_t me, uint32_t tok)
{
    if (!t || !me || !tok) return;
    n48_mmt_slot *m = n48_mmt_find(t, me);
    if (!m) return;
    if (tok & N48_MMT_TOK_NESTED) { __atomic_store_n(&m->tag, tok & 0xFFu, __ATOMIC_RELAXED); return; }
    if (tok & N48_MMT_TOK_CLAIMED) {
        __atomic_store_n(&m->tag, N48_MMT_OTHER, __ATOMIC_RELAXED);
        __atomic_store_n(&m->thread, (uintptr_t)0, __ATOMIC_RELEASE);
        __atomic_fetch_sub(&t->live, 1u, __ATOMIC_RELEASE);
    }
}

static inline uint32_t n48_mmt_lookup(n48_mmt_table *t, uintptr_t me)
{
    if (!t || !__atomic_load_n(&t->live, __ATOMIC_ACQUIRE)) return N48_MMT_OTHER;
    const n48_mmt_slot *m = n48_mmt_find(t, me);
    const uint32_t tag = m ? __atomic_load_n(&m->tag, __ATOMIC_RELAXED) : N48_MMT_OTHER;
    return tag < N48_MMT_N ? tag : N48_MMT_OTHER;
}

typedef struct { uint64_t count[N48_MMT_N], ns[N48_MMT_N], max_ns[N48_MMT_N], dw[N48_MMT_N]; } n48_mmhold_stats;

// build 0.0.515 D2: a DECIDE sub-tag. Pushed ONLY when this thread's open tag is DECIDE or a DECIDE sub-tag (a sub-scope may
// nest another; its pop restores the outer one); anywhere else - no slot, the judge, the commit, the capture - it changes
// nothing and returns 0, which the pop ignores.
static inline uint32_t n48_mmt_is_decide(uint32_t tag) { return tag == N48_MMT_DECIDE || (tag >= N48_MMT_D_FILL && tag <= N48_MMT_D_FENCE); }
// build 0.0.540: the same rule for JUDGE's sub-tags - pushed only over JUDGE or a JUDGE sub-tag, never anywhere else.
static inline uint32_t n48_mmt_is_judge(uint32_t tag) { return tag == N48_MMT_JUDGE || (tag >= N48_MMT_J_WALK && tag < N48_MMT_N); }
static inline uint32_t n48_mmt_push_sub(n48_mmt_table *t, uintptr_t me, uint32_t sub)
{
    if (!t || !me || sub < N48_MMT_D_FILL || sub >= N48_MMT_N) return 0u;
    n48_mmt_slot *m = n48_mmt_find(t, me);
    if (!m) return 0u;
    const uint32_t outer = __atomic_load_n(&m->tag, __ATOMIC_RELAXED);
    if (sub <= N48_MMT_D_FENCE ? !n48_mmt_is_decide(outer) : !n48_mmt_is_judge(outer)) return 0u;
    return n48_mmt_push(t, me, sub);
}

static inline void n48_mmhold_note(n48_mmhold_stats *s, uint32_t tag, uint64_t ns)
{
    if (!s) return;
    if (tag >= N48_MMT_N) tag = N48_MMT_OTHER;
    __atomic_fetch_add(&s->count[tag], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->ns[tag], ns, __ATOMIC_RELAXED);
    uint64_t cur = __atomic_load_n(&s->max_ns[tag], __ATOMIC_RELAXED);
    while (ns > cur && !__atomic_compare_exchange_n(&s->max_ns[tag], &cur, ns, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { }
}
// build 0.0.515 D2: the same note, with the dwords the held call moved (navi48_vram_read_mm / write_mm pass theirs).
static inline void n48_mmhold_note_dw(n48_mmhold_stats *s, uint32_t tag, uint64_t ns, uint64_t dwords)
{
    if (!s) return;
    if (tag >= N48_MMT_N) tag = N48_MMT_OTHER;
    n48_mmhold_note(s, tag, ns);
    __atomic_fetch_add(&s->dw[tag], dwords, __ATOMIC_RELAXED);
}

// The report: two lines (each < 491 bytes at every counter's widest value, tests/gfx_copyguard_test.cpp T27d). The unique token
// `mmhold514` names the lines in a driver log. Per taker: acquires / total hold us / max hold us.
#define N48_MMHOLD_FMT1 "mmhold514: gVramMmLock HOLD by taker (acquires/total us/max us): judge %llu/%llu/%llu, decide %llu/%llu/%llu, " \
                        "commit %llu/%llu/%llu, capture %llu/%llu/%llu"
#define N48_MMHOLD_FMT2 "mmhold514: fcverify %llu/%llu/%llu, residency %llu/%llu/%llu, other %llu/%llu/%llu; taker-table overflow %llu"
#define N48_MMHOLD_T3(s, k) (unsigned long long)(s)->count[k], (unsigned long long)((s)->ns[k] / 1000ull), \
                            (unsigned long long)((s)->max_ns[k] / 1000ull)
// build 0.0.515 D2: line 1's `decide` stays 0.0.514's whole-DECIDE number - the sum over DECIDE and its sub-tags (max: the
// largest) - so fc4 and later runs compare; the split is line 3.
static inline uint64_t n48_mmhold_dsum(const n48_mmhold_stats *s, uint32_t which)
{
    uint64_t v = 0ull;
    for (uint32_t k = N48_MMT_DECIDE; k <= N48_MMT_D_FENCE; k++) {   // build 0.0.540: never JUDGE's sub-tags (12-15)
        if (k != N48_MMT_DECIDE && k < N48_MMT_D_FILL) continue;
        const uint64_t x = which == 0u ? s->count[k] : which == 1u ? s->ns[k] : s->max_ns[k];
        v = which == 2u ? (x > v ? x : v) : v + x;
    }
    return v;
}
// build 0.0.540: and line 1's `judge` is JUDGE plus its sub-tags (only ever non-zero while switch 96 is ON), so it stays the
// whole judge's number whichever way 96 is thrown.
static inline uint64_t n48_mmhold_jsum(const n48_mmhold_stats *s, uint32_t which)
{
    uint64_t v = 0ull;
    for (uint32_t k = 0; k < N48_MMT_N; k++) {
        if (!n48_mmt_is_judge(k)) continue;
        const uint64_t x = which == 0u ? s->count[k] : which == 1u ? s->ns[k] : s->max_ns[k];
        v = which == 2u ? (x > v ? x : v) : v + x;
    }
    return v;
}
#define N48_MMHOLD_ARGS1(s) (unsigned long long)n48_mmhold_jsum(s, 0u), (unsigned long long)(n48_mmhold_jsum(s, 1u) / 1000ull), \
                            (unsigned long long)(n48_mmhold_jsum(s, 2u) / 1000ull), (unsigned long long)n48_mmhold_dsum(s, 0u), \
                            (unsigned long long)(n48_mmhold_dsum(s, 1u) / 1000ull), (unsigned long long)(n48_mmhold_dsum(s, 2u) / 1000ull), \
                            N48_MMHOLD_T3(s, N48_MMT_COMMIT), N48_MMHOLD_T3(s, N48_MMT_CAPTURE)
#define N48_MMHOLD_ARGS2(s, ovf) N48_MMHOLD_T3(s, N48_MMT_FCVERIFY), N48_MMHOLD_T3(s, N48_MMT_RESIDENCY), \
                                 N48_MMHOLD_T3(s, N48_MMT_OTHER), (unsigned long long)(ovf)

// build 0.0.515 D2: the third line - DECIDE by site (acquires/total us/dwords); line 1's `decide` is the sum of these six.
#define N48_MMHOLD_FMT3 "mmhold515: decide by site (acquires/total us/dwords): fill %llu/%llu/%llu, memo+id %llu/%llu/%llu, " \
                        "ib-walk %llu/%llu/%llu, targets %llu/%llu/%llu, fence %llu/%llu/%llu, other %llu/%llu/%llu"
#define N48_MMHOLD_D3(s, k) (unsigned long long)(s)->count[k], (unsigned long long)((s)->ns[k] / 1000ull), \
                            (unsigned long long)(s)->dw[k]
#define N48_MMHOLD_ARGS3(s) N48_MMHOLD_D3(s, N48_MMT_D_FILL), N48_MMHOLD_D3(s, N48_MMT_D_PGM), N48_MMHOLD_D3(s, N48_MMT_D_IB), \
                            N48_MMHOLD_D3(s, N48_MMT_D_TGT), N48_MMHOLD_D3(s, N48_MMT_D_FENCE), N48_MMHOLD_D3(s, N48_MMT_DECIDE)

#endif /* N48_GFX_MMHOLD_H */
