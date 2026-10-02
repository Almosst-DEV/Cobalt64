// gfx_p86_test.cpp — build 0.0.531 ( lever 1, lever 1): switch 86 (gfx_p86.h, the present-time retirement
// re-check), switch 87 (gfx_p87.h, the text-element T# log and the post-STOP readback), the glass instruments, and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_p86_test.cpp -o /tmp/p86 && /tmp/p86 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/Navi48AccelPeer.cpp \
//         src/navi48-bringup/src/Navi48Bringup.cpp src/xlat12/xlat12_ib.c src/xlat12/xlat12_dtable_rows.inc \
//         src/xlat12/xlat12_shader_ids.h
// Covers:
//   T1 PENDING + spared + the P's own fence OURS -> promoted (through n48_p73_retired) and copied, on the present that asked;
//   T2 the fence not yet written -> held (UNKNOWN), counted "not yet", nothing promoted; the next present after the write copies;
//   T3 a PENDING slot whose P was NOT spared (the final not seen, or withdrawn / refused / NOPED states) is never asked and never
//      promoted, whatever its fence reads;
//   T4 the try-lock busy -> held, counted, NO fence read;
//   T5 a promoted P older than the glass's P -> held OLDER (Part E unchanged);
//   T6 a fence from ANOTHER flight (another seq's entry COMMITTED and OURS; the P's own entry not yet) never promotes; a reused
//      ring slot (the P's entry freed, another seq in it) never promotes; n48_p86_fence_ours refuses a seq mismatch;
//   T7 the P's entry not live (RETIRED / EXPIRED / NOPED / NOT_RUN / fence-less) -> no read, no promotion;
//   T8 n48_p86_fence_ours answers exactly n48_fr_poll_entry's retirement over random entries and reads (seq matching);
//   T9 OFF IDENTITY: switch 86 OFF answers, reasons, slots and the whole table equal a FROZEN copy of 0.0.530's n48_p73_present /
//      n48_p73_should_copy over random sequences (gate / final / retired / non-P / invalidate / present / delivered); ON copies only
//      what OFF copies, plus presents whose P was spared and whose own fence read OURS;
//   T10 the UNKNOWN split sums to switch 73's unknown holds; the wall-time histogram buckets;
//   T11 every new log line fits the 491-byte body at maximal fields;
//   T12 switch 87: the eight identities equal the dtable rows and the shader-id numbering; the stash, the per-arm cap, uncommitted
//       frames never printed, a retry's re-read skipped; the T# decode;
//   T13 the readback: the plan never reads outside [off, off + len), refuses outside the aperture, above 64 KiB, zero, while armed;
//       the GEOM / DUMP arguments round-trip;
//   T14 the kext glue (source pins, with order): 86 and 87 OFF at boot; the present's lock is IOLockTryLock(gXdLock) and never
//       IOLockLock; the read is the expiry poll's (ks_x_read = navi48_vram_read_mm of one dword) over gKsRing; the verbs guard with
//       their own selectors; the timed entries are installed; the mmhold snapshot at START and STOP; switch 87's hooks.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_p86.h"
#include "gfx_p87.h"

#ifndef N48_LOG_CAP_BODY
#define N48_LOG_CAP_BODY 491u
#endif

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}

// ------------------------------------------------------------------------------------------------------------ the frozen 0.0.530 code
// n48_p73_present, n48_p73_c80_hold, n48_p73_c80_complete and n48_p73_should_copy exactly as gfx_present73.h has them at 96820b81
// (0.0.530), renamed. The OFF identity (T9) compares switch 86 OFF against THESE, not against the live header.
static uint32_t frz_c80_complete(uint32_t s1, uint32_t v, uint32_t s2, uint32_t seq)
{
    return (seq != 0u && s1 == seq && s2 == seq && v == N48_P73_C80V_COMPLETE) ? 1u : 0u;
}
static uint32_t frz_c80_hold(n48_p73 *t, const n48_p73_slot *s, uint32_t seq)
{
    const uint32_t on = n48_p73_ld32(&t->c80on);
    if (on == N48_P73_C80_OFF) return 0u;
    const uint32_t s1 = n48_p73_ld32(&s->c80seq);
    const uint32_t v = n48_p73_ld32(&s->c80verdict);
    const uint32_t s2 = n48_p73_ld32(&s->c80seq);
    if (frz_c80_complete(s1, v, s2, seq)) return 0u;
    if (on == N48_P73_C80_SHADOW) { t->c80WouldHold++; return 0u; }
    return 1u;
}
static uint32_t frz_present(n48_p73 *t, uint64_t key, uint32_t *reason, uint32_t *slot)
{
    const uint32_t i = n48_p73_find(t, key);
    if (slot) *slot = i;
    uint32_t why = N48_P73_HOLD_NOMATCH;
    if (i < N48_P73_SLOTS) {
        const uint32_t st = n48_p73_ld32(&t->s[i].state);
        if (st == N48_P73_ST_COMMITTED) {
            const uint32_t seq = t->s[i].seq;
            if (frz_c80_hold(t, &t->s[i], seq)) why = N48_P73_HOLD_INCOMPLETE;
            else if (!t->deliveredOk || n48_p73_seq_newer(seq, t->lastDelivered)) {
                t->copied++; t->decided = seq; t->decidedOk = 1u;
                if (reason) *reason = N48_P73_HOLDS;
                return 1u;
            }
            else why = N48_P73_HOLD_OLDER;
        } else
        why = st == N48_P73_ST_REFUSED ? N48_P73_HOLD_REFUSED : st == N48_P73_ST_WITHDRAWN ? N48_P73_HOLD_WITHDRAWN
                                                                                          : N48_P73_HOLD_UNKNOWN;
    }
    t->held[why]++;
    if (reason) *reason = why;
    return 0u;
}
static uint32_t frz_should_copy(uint32_t on, n48_p73 *t, uint64_t key, uint32_t *reason, uint32_t *slot)
{
    if (!on) return 1u;
    return frz_present(t, key, reason, slot);
}

// ------------------------------------------------------------------------------------------------------------ the model's kext side
// A fake io: the lock (busy or free), VRAM fence slots (a small map), a read counter, and the ring.
struct Fake {
    uint32_t busy = 0u, locked = 0u, reads = 0u, lockCalls = 0u, unlockCalls = 0u, readFails = 0u;
    uint64_t off[64] = {}; uint32_t val[64] = {}; uint32_t n = 0u;
    n48_fr_ring ring {};
};
static uint32_t f_trylock(void *c) { Fake *f = (Fake *)c; f->lockCalls++; if (f->busy || f->locked) return 0u; f->locked = 1u; return 1u; }
static void f_unlock(void *c) { Fake *f = (Fake *)c; f->unlockCalls++; f->locked = 0u; }
static uint32_t f_read(void *c, uint64_t o, uint32_t *v)
{
    Fake *f = (Fake *)c; f->reads++;
    if (!f->locked) { f->readFails++; return 0u; }   // the model insists every read happens under the lock
    if (f->readFails > 1000u) return 0u;
    for (uint32_t k = 0; k < f->n; k++) if (f->off[k] == o) { *v = f->val[k]; return 1u; }
    *v = 0u; return 1u;
}
static void f_write(Fake *f, uint64_t o, uint32_t v)
{
    for (uint32_t k = 0; k < f->n; k++) if (f->off[k] == o) { f->val[k] = v; return; }
    if (f->n < 64u) { f->off[f->n] = o; f->val[f->n] = v; f->n++; }
}
static n48_p86_io io_of(Fake *f) { n48_p86_io io = { &f_trylock, &f_unlock, &f_read, &f->ring, f }; return io; }

static const uint64_t KEY_A = 0x10930000ull, KEY_B = 0x11200000ull, KEY_C = 0x11a70000ull;
static uint64_t fence_off(uint32_t seq) { return 0x3f0000000ull + 4ull * (seq & 1023u); }
static uint32_t fence_want(uint32_t seq) { return 0x7a000000u | seq; }

// A P for plane `key` with gate seq `seq`: the gate (PENDING), the ring push + keystone COMMITTED mark, then (optionally) the hook's
// final SPARED. The fence is NOT written (the GPU has not finished).
static void p_submit(n48_p73 *t, Fake *f, uint64_t key, uint32_t seq, uint32_t spared)
{
    n48_p73_call_begin(t);
    (void)n48_p73_gate_p(t, key, 0x401800000ull, 1u, seq);
    uint32_t idx = N48_FR_CAPACITY;
    static uint64_t clock = 1000ull;
    if (n48_fr_full(&f->ring)) {   // the model's expiry: the oldest live entry EXPIRES and is reclaimed (as the kext's bound does)
        uint32_t o = 0u;
        for (uint32_t k = 1; k < N48_FR_CAPACITY; k++) if (f->ring.e[k].at_us < f->ring.e[o].at_us) o = k;
        f->ring.e[o].state = N48_FR_EXPIRED;
        (void)n48_fr_reclaim(&f->ring);
    }
    (void)n48_fr_push(&f->ring, seq, ++clock, seq, fence_off(seq), fence_want(seq), &idx);
    (void)n48_fr_mark_committed(&f->ring, seq);
    if (spared) (void)n48_p73_final(t, seq, 1u);
}
static void gpu_done(Fake *f, uint32_t seq) { f_write(f, fence_off(seq), fence_want(seq)); }

static uint32_t present86(n48_p73 *t, n48_p86 *c, Fake *f, uint64_t key, uint32_t on86, uint32_t *why)
{
    const n48_p86_io io = io_of(f);
    uint32_t w = N48_P73_HOLDS, i = N48_P73_SLOTS;
    const uint32_t copy = n48_p86_should_copy(1u, on86, t, c, key, &io, &w, &i);
    if (copy) n48_p73_delivered(t);
    if (why) *why = w;
    return copy;
}

static void fresh(n48_p73 *t, n48_p86 *c, Fake *f)
{
    std::memset((void *)t, 0, sizeof *t); std::memset((void *)c, 0, sizeof *c);
    Fake z; *f = z; n48_fr_reset(&f->ring);
    n48_p73_reset(t);
}

// ---------------------------------------------------------------------------------------------------------------- T1 .. T7
static void t_basic()
{
    static n48_p73 t; static n48_p86 c; static Fake f; uint32_t why = 0;
    // T1
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 5u, 1u);
    expect_u("T1 before the GPU finished: the P is PENDING (spared, not retired)", n48_p73_ld32(&t.s[0].state), N48_P73_ST_PENDING);
    gpu_done(&f, 5u);
    expect_u("T1 86 OFF: the present after the GPU finished is still HELD (0.0.530: retirement only at the next submission)",
             present86(&t, &c, &f, KEY_A, 0u, &why), 0u);
    expect_u("T1 ... reason UNKNOWN", why, N48_P73_HOLD_UNKNOWN);
    expect_u("T1 ... and 86 OFF asked nothing: no lock, no read", f.lockCalls + f.reads + c.asked, 0u);
    expect_u("T1 86 ON: the same present re-checks, reads OURS, promotes and COPIES", present86(&t, &c, &f, KEY_A, 1u, &why), 1u);
    expect_u("T1 ... promoted counted once", c.r[N48_P86_R_PROMOTED], 1u);
    expect_u("T1 ... one lock, one unlock, ONE fence read", f.lockCalls * 0x100u + f.unlockCalls * 0x10u + f.reads, 0x111u);
    expect_u("T1 ... the slot is COMMITTED with its own seq; pCommitted 1", n48_p73_ld32(&t.s[0].state) * 0x100u + t.s[0].seq * 0x10u +
             (uint32_t)t.pCommitted, N48_P73_ST_COMMITTED * 0x100u + 5u * 0x10u + 1u);
    expect_u("T1 ... the ring entry is NOT written (still COMMITTED for the ring's own poll)", f.ring.e[0].state, N48_FR_COMMITTED);
    expect_u("T1 ... the glass P is 5", t.lastDelivered, 5u);
    expect_u("T1 a second present of the same plane: COMMITTED already, no re-check (asked stays 1), held OLDER (not newer)",
             present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && why == N48_P73_HOLD_OLDER && c.asked == 1u ? 1u : 0u, 1u);
    // the ring's own poll later retires the entry: its p73_retired finds no PENDING slot (counted, nothing changes)
    const uint32_t retiredNow = n48_fr_poll_entry(&f.ring, 0u, 1u, fence_want(5u));
    expect_u("T1 the ring's later poll retires the entry; p73_retired then counts retireNoSlot and changes nothing",
             retiredNow == 1u && n48_p73_retired(&t, 5u) == 0u && t.retireNoSlot == 1u &&
             n48_p73_ld32(&t.s[0].state) == N48_P73_ST_COMMITTED ? 1u : 0u, 1u);

    // T2
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 7u, 1u);
    expect_u("T2 86 ON, fence not written: HELD UNKNOWN", present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && why == N48_P73_HOLD_UNKNOWN, 1u);
    expect_u("T2 ... counted 'not yet', nothing promoted, the slot PENDING, retired mark 0",
             c.r[N48_P86_R_NOTYET] == 1u && c.r[N48_P86_R_PROMOTED] == 0u && n48_p73_ld32(&t.s[0].state) == N48_P73_ST_PENDING &&
             t.s[0].retired == 0u ? 1u : 0u, 1u);
    f_write(&f, fence_off(7u), 0x12345678u);   // another value in the slot (not ours)
    expect_u("T2 a fence slot holding ANOTHER value: held", present86(&t, &c, &f, KEY_A, 1u, &why), 0u);
    gpu_done(&f, 7u);
    expect_u("T2 after the write: the next present copies", present86(&t, &c, &f, KEY_A, 1u, &why), 1u);

    // T3
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 9u, 0u);   // the final not seen yet
    gpu_done(&f, 9u);
    expect_u("T3 PENDING NOT spared (final not seen), fence OURS: 86 ON holds", present86(&t, &c, &f, KEY_A, 1u, &why), 0u);
    expect_u("T3 ... never asked, no lock, no read, no retired mark", c.asked + f.lockCalls + f.reads + t.s[0].retired, 0u);
    (void)n48_p73_final(&t, 9u, 0u);   // withdrawn
    expect_u("T3 WITHDRAWN: held WITHDRAWN, never asked", present86(&t, &c, &f, KEY_A, 1u, &why) == 0u &&
             why == N48_P73_HOLD_WITHDRAWN && c.asked == 0u ? 1u : 0u, 1u);
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, KEY_A, 0x401800000ull, 0u, 0u);   // refused
    expect_u("T3 REFUSED: held REFUSED, never asked", present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && why == N48_P73_HOLD_REFUSED &&
             c.asked == 0u ? 1u : 0u, 1u);
    // a P spared, then a new P for the same plane gated (PENDING, marks cleared): the new P was not spared
    p_submit(&t, &f, KEY_A, 11u, 1u);
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, KEY_A, 0x401800000ull, 1u, 12u);
    gpu_done(&f, 11u);
    expect_u("T3 a newer P (12, not spared) took the slot after P 11 was spared: 86 ON never promotes 11 or 12",
             present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && c.asked == 0u && n48_p73_ld32(&t.s[0].state) == N48_P73_ST_PENDING &&
             t.s[0].seq == 12u ? 1u : 0u, 1u);

    // T4
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 13u, 1u);
    gpu_done(&f, 13u);
    f.busy = 1u;
    expect_u("T4 try-lock busy: HELD", present86(&t, &c, &f, KEY_A, 1u, &why), 0u);
    expect_u("T4 ... counted busy, NO read, nothing promoted", c.r[N48_P86_R_BUSY] == 1u && f.reads == 0u &&
             n48_p73_ld32(&t.s[0].state) == N48_P73_ST_PENDING ? 1u : 0u, 1u);
    f.busy = 0u;
    expect_u("T4 lock free again: the next present copies", present86(&t, &c, &f, KEY_A, 1u, &why), 1u);

    // T5
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 20u, 1u);
    p_submit(&t, &f, KEY_B, 21u, 1u);
    gpu_done(&f, 21u);
    expect_u("T5 plane B's P 21 promoted and copied (the glass P 21)", present86(&t, &c, &f, KEY_B, 1u, &why) == 1u &&
             t.lastDelivered == 21u ? 1u : 0u, 1u);
    gpu_done(&f, 20u);
    expect_u("T5 plane A's P 20 is promoted at its present but is OLDER than the glass's 21: HELD OLDER",
             present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && why == N48_P73_HOLD_OLDER &&
             n48_p73_ld32(&t.s[0].state) == N48_P73_ST_COMMITTED && c.r[N48_P86_R_PROMOTED] == 2u ? 1u : 0u, 1u);

    // T6
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 30u, 1u);
    p_submit(&t, &f, KEY_B, 31u, 1u);
    gpu_done(&f, 31u);   // ANOTHER flight's fence is OURS (for 31); 30's is not
    expect_u("T6 plane A (P 30, fence not yet) while flight 31's fence is OURS: HELD, not promoted",
             present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && c.r[N48_P86_R_NOTYET] == 1u &&
             n48_p73_ld32(&t.s[0].state) == N48_P73_ST_PENDING ? 1u : 0u, 1u);
    // the P's entry freed and its ring slot reused by another seq whose fence is OURS
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 40u, 1u);
    f.ring.e[0].state = N48_FR_EXPIRED;   // the P's flight expired and its slot was reclaimed ...
    (void)n48_fr_reclaim(&f.ring);
    uint32_t idx = 0;   // ... and reused by seq 41, with the same fence slot and value
    (void)n48_fr_push(&f.ring, 41u, 2000ull, 41u, fence_off(40u), fence_want(40u), &idx);
    (void)n48_fr_mark_committed(&f.ring, 41u);
    gpu_done(&f, 40u);
    expect_u("T6 the P's ring slot reused by seq 41 (same fence slot and value): no entry for 40 -> HELD, counted",
             present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && c.r[N48_P86_R_NOENTRY] == 1u && f.reads == 0u ? 1u : 0u, 1u);
    {
        n48_fr_entry e {}; e.seq = 41u; e.state = N48_FR_COMMITTED; e.want = 0x55u;
        expect_u("T6 n48_p86_fence_ours: another seq's entry reading its own want is NOT this P's", n48_p86_fence_ours(&e, 40u, 1u, 0x55u), 0u);
        expect_u("T6 n48_p86_fence_ours: this P's own entry reading its want is", n48_p86_fence_ours(&e, 41u, 1u, 0x55u), 1u);
    }

    // T7
    const uint32_t states[] = { N48_FR_RETIRED, N48_FR_EXPIRED, N48_FR_NOPED, N48_FR_NOT_RUN, N48_FR_PENDING };
    for (uint32_t k = 0; k < 5u; k++) {
        fresh(&t, &c, &f);
        p_submit(&t, &f, KEY_A, 50u, 1u);
        f.ring.e[0].state = states[k];
        gpu_done(&f, 50u);
        char lbl[160]; std::snprintf(lbl, sizeof lbl, "T7 the P's entry %s: no read, no promotion, HELD", n48_fr_state_name(states[k]));
        expect_u(lbl, present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && f.reads == 0u && c.r[N48_P86_R_NOTLIVE] == 1u ? 1u : 0u, 1u);
    }
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 51u, 1u);
    f.ring.e[0].want = 0u;   // fence-less
    expect_u("T7 a fence-less entry (want 0): no read, no promotion", present86(&t, &c, &f, KEY_A, 1u, &why) == 0u && f.reads == 0u ? 1u : 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T8
static uint64_t gRng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 7; gRng ^= gRng << 17; return (uint32_t)(gRng >> 11); }
static void t_poll_equiv()
{
    uint32_t bad = 0u, both = 0u;
    for (uint32_t it = 0; it < 200000u; it++) {
        n48_fr_ring r; n48_fr_reset(&r);
        n48_fr_entry &e = r.e[3];
        e.seq = 1u + rnd() % 4u; e.state = rnd() % N48_FR_STATES; e.want = (rnd() & 3u) ? 0x100u + rnd() % 3u : 0u;
        const uint32_t got = rnd() & 1u, val = 0x100u + rnd() % 3u - (rnd() % 4u == 0u ? 0x100u : 0u);
        const uint32_t seq = e.seq;
        const uint32_t ours = n48_p86_fence_ours(&e, seq, got, val);
        const uint32_t poll = n48_fr_poll_entry(&r, 3u, got, val);
        if (ours != poll) bad++;
        both += ours & poll;
    }
    expect_u("T8 n48_p86_fence_ours == n48_fr_poll_entry's retirement over 200000 random entries/reads (seq matching)", bad, 0u);
    expect_u("T8 ... and the random set held retirements (non-vacuous)", both > 1000u ? 1u : 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T9 / T10
static void t_identity()
{
    static n48_p73 a, b; static n48_p86 ca, cb; static Fake fa, fb;
    uint32_t diffs = 0u, steps = 0u, onExtra = 0u, onExtraBad = 0u, onMissing = 0u, presents = 0u, offCopies = 0u;
    uint32_t reasons[N48_P73_HOLDS + 1u] = {};
    const uint64_t keys[4] = { KEY_A, KEY_B, KEY_C, 0x12340000ull };
    for (uint32_t run = 0; run < 400u; run++) {
        fresh(&a, &ca, &fa); fresh(&b, &cb, &fb);
        a.c80on = b.c80on = (run % 5u == 4u) ? N48_P73_C80_SHADOW : N48_P73_C80_OFF;
        uint32_t seq = (run % 7u == 6u) ? 0xFFFFFFF0u : 1u;
        std::vector<uint32_t> live;
        for (uint32_t st = 0; st < 300u; st++, steps++) {
            const uint32_t op = rnd() % 10u;
            const uint64_t key = keys[rnd() % 4u];
            if (op <= 1u) {   // a P, spared or not
                const uint32_t sp = rnd() & 1u;
                p_submit(&a, &fa, key, seq, sp); p_submit(&b, &fb, key, seq, sp);
                live.push_back(seq);
                seq = seq == 0xFFFFFFFFu ? 1u : seq + 1u;
            } else if (op == 2u && !live.empty()) {   // a GPU completion (both sides' fence memory)
                const uint32_t s = live[rnd() % live.size()]; gpu_done(&fa, s); gpu_done(&fb, s);
            } else if (op == 3u) {   // the ring's own judged-frame poll: every COMMITTED fenced entry reading OURS retires
                for (uint32_t side = 0; side < 2u; side++) {
                    Fake &f = side ? fb : fa; n48_p73 &t = side ? b : a;
                    for (uint32_t k = 0; k < N48_FR_CAPACITY; k++) {
                        if (f.ring.e[k].state != N48_FR_COMMITTED || !f.ring.e[k].want) continue;
                        uint32_t v = 0; const uint32_t r0 = f.reads; f.locked = 1u;   // the poll's read is not the present's
                        const uint32_t g = f_read(&f, f.ring.e[k].vram_off, &v);
                        f.locked = 0u; f.reads = r0;
                        const uint32_t sq = f.ring.e[k].seq;
                        if (n48_fr_poll_entry(&f.ring, k, g, v)) (void)n48_p73_retired(&t, sq);
                    }
                }
            } else if (op == 4u) {   // a non-P writer naming the plane
                const uint64_t k[1] = { key };
                n48_p73_call_begin(&a); (void)n48_p73_gate_nonp(&a, k, 1u);
                n48_p73_call_begin(&b); (void)n48_p73_gate_nonp(&b, k, 1u);
            } else if (op == 5u && rnd() % 2u == 0u) {   // a REFUSED P
                n48_p73_call_begin(&a); (void)n48_p73_gate_p(&a, key, 0x401800000ull, 0u, 0u);
                n48_p73_call_begin(&b); (void)n48_p73_gate_p(&b, key, 0x401800000ull, 0u, 0u);
            } else if (op == 5u && rnd() % 8u == 0u) {   // an unjudged frame
                n48_p73_call_begin(&a); (void)n48_p73_after_decide(&a);
                n48_p73_call_begin(&b); (void)n48_p73_after_decide(&b);
            } else if (op == 6u && !live.empty() && rnd() % 3u == 0u) {   // a late final (withdrawn) for the pending P
                const uint32_t s = live.back(); (void)n48_p73_final(&a, s, 0u); (void)n48_p73_final(&b, s, 0u);
            } else {   // a present: A through 86 OFF, B through the frozen 0.0.530 code
                presents++;
                uint32_t wa = 0, wb = 0, ia = 0, ib = 0;
                const n48_p86_io io = io_of(&fa);
                const uint32_t xa = n48_p86_should_copy(1u, 0u, &a, &ca, key, &io, &wa, &ia);
                const uint32_t xb = frz_should_copy(1u, &b, key, &wb, &ib);
                if (xa) n48_p73_delivered(&a);
                if (xb) n48_p73_delivered(&b);
                offCopies += xb;
                if (wb <= N48_P73_HOLDS) reasons[wb]++;
                if (xa != xb || wa != wb || ia != ib) diffs++;
            }
            // the whole table, after every step
            if (std::memcmp(a.s, b.s, sizeof a.s) || a.copied != b.copied || std::memcmp(a.held, b.held, sizeof a.held) ||
                a.lastDelivered != b.lastDelivered || a.deliveredOk != b.deliveredOk || a.pCommitted != b.pCommitted ||
                a.retireNoSlot != b.retireNoSlot || a.c80WouldHold != b.c80WouldHold)
                diffs++;
        }
        if (fa.lockCalls || fa.reads) diffs++;   // OFF asks nothing of the lock or VRAM
    }
    expect_u("T9 OFF IDENTITY: 86 OFF == frozen 0.0.530 (answer, reason, slot, table, counters) over 400 x 300 random steps", diffs, 0u);
    std::printf("      (T9 presents %u, OFF copies %u; held refused %u withdrawn %u unknown %u no-match %u older %u)\n", presents,
                offCopies, reasons[N48_P73_HOLD_REFUSED], reasons[N48_P73_HOLD_WITHDRAWN], reasons[N48_P73_HOLD_UNKNOWN],
                reasons[N48_P73_HOLD_NOMATCH], reasons[N48_P73_HOLD_OLDER]);
    expect_u("T9 ... non-vacuous: presents, copies, and every hold reason the model can reach",
             presents > 20000u && offCopies > 150u && reasons[N48_P73_HOLD_WITHDRAWN] > 10u && reasons[N48_P73_HOLD_UNKNOWN] > 10u &&
             reasons[N48_P73_HOLD_NOMATCH] > 10u && reasons[N48_P73_HOLD_OLDER] > 10u && reasons[N48_P73_HOLD_REFUSED] > 10u ? 1u : 0u, 1u);

    // ON vs OFF: ON copies every present OFF copies (none missed) plus presents whose P was spared and fence OURS
    for (uint32_t run = 0; run < 300u; run++) {
        fresh(&a, &ca, &fa); fresh(&b, &cb, &fb);
        uint32_t seq = 1u;
        std::vector<uint32_t> live;
        for (uint32_t st = 0; st < 200u; st++) {
            const uint32_t op = rnd() % 6u;
            const uint64_t key = keys[rnd() % 3u];
            if (op <= 1u) { const uint32_t sp = rnd() % 4u != 0u; p_submit(&a, &fa, key, seq, sp); p_submit(&b, &fb, key, seq, sp); live.push_back(seq++); }
            else if (op == 2u && !live.empty()) { const uint32_t s = live[rnd() % live.size()]; gpu_done(&fa, s); gpu_done(&fb, s); }
            else {
                // what the P in `key`'s slot is, before either side answers (the ground truth for ON's extra copy)
                const uint32_t i = n48_p73_find(&a, key);
                uint32_t pseq = 0u, truth = 0u;
                if (n48_p86_candidate(&a, i, &pseq)) {
                    uint32_t ix = 0;
                    if (n48_fr_find(&fa.ring, pseq, &ix) && fa.ring.e[ix].state == N48_FR_COMMITTED) {
                        uint32_t v = 0; fa.locked = 1u; (void)f_read(&fa, fa.ring.e[ix].vram_off, &v); fa.locked = 0u;
                        truth = v == fa.ring.e[ix].want ? 1u : 0u;
                    }
                }
                uint32_t wa = 0, wb = 0, ia = 0, ib = 0;
                const n48_p86_io ioa = io_of(&fa);
                const uint32_t xa = n48_p86_should_copy(1u, 1u, &a, &ca, key, &ioa, &wa, &ia);
                const uint32_t xb = frz_should_copy(1u, &b, key, &wb, &ib);
                if (xa) n48_p73_delivered(&a);
                if (xb) n48_p73_delivered(&b);
                // the two sides' tables diverge once ON promotes; resynchronise B to A so each present compares like with like
                if (xa && !xb) { onExtra++; if (!truth) onExtraBad++; }
                if (!xa && xb) onMissing++;
                b = a; fb.ring = fa.ring;
            }
        }
    }
    expect_u("T9 ON copies nothing OFF would not copy EXCEPT a spared P whose own fence read OURS (ground truth)", onExtraBad, 0u);
    expect_u("T9 ON never holds a present OFF copies", onMissing, 0u);
    expect_u("T9 ... non-vacuous: ON made extra copies", onExtra > 100u ? 1u : 0u, 1u);

    // T10: the UNKNOWN split sums to 73's unknown holds (both runs above left counters on `a`: check a fresh sequence)
    static n48_p73 t; static n48_p86 c; static Fake f; uint32_t why = 0;
    fresh(&t, &c, &f);
    p_submit(&t, &f, KEY_A, 3u, 1u);            // PENDING spared
    p_submit(&t, &f, KEY_B, 4u, 0u);            // PENDING not spared
    { const uint64_t k[1] = { KEY_C }; (void)n48_p73_claim(&t, KEY_C, 0ull); (void)n48_p73_gate_nonp(&t, k, 1u); }   // UNKNOWN
    (void)present86(&t, &c, &f, KEY_A, 0u, &why); (void)present86(&t, &c, &f, KEY_B, 0u, &why); (void)present86(&t, &c, &f, KEY_C, 0u, &why);
    (void)present86(&t, &c, &f, KEY_A, 0u, &why);
    expect_u("T10 the UNKNOWN split: spared 2, not spared 1, state UNKNOWN 1; sum == 73's unknown holds",
             c.unkPendSpared == 2u && c.unkPendOther == 1u && c.unkState == 1u &&
             c.unkPendSpared + c.unkPendOther + c.unkState == t.held[N48_P73_HOLD_UNKNOWN] ? 1u : 0u, 1u);
    n48_wt_hist h {};
    const uint64_t us[] = { 0u, 9u, 10u, 49u, 50u, 99u, 100u, 499u, 500u, 999u, 1000u, 4999u, 5000u, 19999u, 20000u, 99999u, 100000u, ~0ull >> 20 };
    for (uint64_t u : us) n48_wt_note(&h, u);
    expect_u("T10 wall-time buckets: 2 per bucket, 4 in the last; max; n",
             h.b[0] == 2u && h.b[1] == 2u && h.b[2] == 2u && h.b[3] == 2u && h.b[4] == 2u && h.b[5] == 2u && h.b[6] == 2u && h.b[7] == 2u &&
             h.b[8] == 2u && h.n == 18u && h.max_us == (~0ull >> 20) ? 1u : 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T11
static void t_widths()
{
    char b[2048];
    const unsigned long long M = ~0ull;
    int w;
    w = std::snprintf(b, sizeof b, N48_P86_FMT, "ON (a PENDING spared P's fence is re-read at the present)",
                      " - `gfxneuter 86` REFUSED it (unknown M), unchanged", M, M, M, M, M, M, M, M, M);
    expect_u("T11 present86 line 1 fits the 491-byte body", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P86_FMT2, M, M, M, M, M, 4294967295u);
    expect_u("T11 present86 line 2 fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    n48_wt_hist h {}; for (uint32_t k = 0; k < N48_WT_BUCKETS; k++) h.b[k] = M; h.n = 1u; h.sum_us = M; h.max_us = M;
    w = std::snprintf(b, sizeof b, N48_WT_FMT, N48_WT_ARGS("GFX submission (hook_gfxCommitIB)", &h));
    expect_u("T11 the wall-time line fits (longest name, 20-digit counters)", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P86_COPY_FMT, M, M, 7u, 4294967295u, 4294967295u, 4294967295u);
    expect_u("T11 the COPIED line (last gate seq) fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    // build 0.0.535 item 4: the draw's own CB0 (+ note), its window scissor (+ note) and the frame's first CB0 lead the line.
    w = std::snprintf(b, sizeof b, N48_P87_FMT, M, M, " inh", 4294967295u, 4294967295u, 4294967295u, 4294967295u, " inh", M, "UCF", 4294967295u, 4294967295u, 4294967295u, 4294967295u, M, 4294967295u,
                      4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 0xffffffffu, 0xffffffffu, 0xffffffffu,
                      0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu);
    expect_u("T11 tex531 T# line fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P87_REPORT_FMT, "ON (T# lines of committed frames)",
                      " - `gfxneuter 87` readback (see the imgdump531 lines)", M, M, M, M, M, M, M, 4294967295u, 4294967295u, M,
                      4294967295u, 4294967295u);
    expect_u("T11 tex531 report fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P87_REPORT2_FMT, M, M, M, M, M, M, M, M);
    expect_u("T11 tex531 report 2 fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P87_HDR_FMT, 4294967295u, 4294967295u, M, 4294967295u, 4294967295u, 4294967295u, 4294967295u,
                      4294967295u, 4294967295u);
    expect_u("T11 imgdump HDR fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    std::string hex(2u * N48_P87_DUMP_LINE, 'f');
    w = std::snprintf(b, sizeof b, N48_P87_HEX_FMT, 4294967295u, 0xffffffffu, hex.c_str());
    expect_u("T11 imgdump D line (128 bytes of hex) fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P87_END_FMT, 4294967295u, 4294967295u, 4294967295u, 0xffffffffu);
    expect_u("T11 imgdump END fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
    uint32_t worst = 0u;
    for (uint32_t d = 0; d < N48_P87_DS; d++) {
        w = std::snprintf(b, sizeof b, N48_P87_REF_FMT, M, 4294967295u, n48_p87_dump_name(d));
        if ((uint32_t)w > worst) worst = (uint32_t)w;
    }
    expect_u("T11 imgdump DUMP answer fits for every answer", worst <= N48_LOG_CAP_BODY, 1u);
    w = std::snprintf(b, sizeof b, N48_P87_GEOM_FMT, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u);
    expect_u("T11 imgdump GEOM fits", (uint32_t)w <= N48_LOG_CAP_BODY, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- helpers for files
static std::string slurp(const char *p)
{
    std::string s;
    if (!p) return s;
    FILE *f = std::fopen(p, "rb");
    if (!f) return s;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &h, const char *n)
{
    uint32_t c = 0u; size_t at = 0u; const size_t L = std::strlen(n);
    while (L && (at = h.find(n, at)) != std::string::npos) { c++; at += L; }
    return c;
}
static size_t at(const std::string &h, const char *n) { return h.find(n); }
static std::string between(const std::string &s, const char *from, const char *to)
{
    const size_t a = s.find(from);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find(to, a);
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

// ---------------------------------------------------------------------------------------------------------------- T12
static void t_p87(const char *rowsPath, const char *idsPath)
{
    const std::string rows = slurp(rowsPath), ids = slurp(idsPath);
    uint32_t okRows = 0u, okIds = 0u;
    for (uint32_t k = 0; k < N48_P87_IDS; k++) {
        char needle[96];
        std::snprintf(needle, sizeof needle, "{ %uu, 0x%08xu, ", kN48P87Ids[k].ndw, kN48P87Ids[k].fnv);
        if (count(rows, needle) == 1u) okRows++;
        // the identity number is the entry's 0-based index among xlat12_shader_ids.h's `{ "name", ...` rows
        std::snprintf(needle, sizeof needle, "%uu, 0x%08xu, {", kN48P87Ids[k].ndw, kN48P87Ids[k].fnv);
        size_t pos = ids.find(needle);
        if (pos != std::string::npos) {
            uint32_t idx = 0u; size_t q = 0u;
            while ((q = ids.find("\n    { \"", q)) != std::string::npos && q < pos) { idx++; q++; }
            if (idx - 1u == kN48P87Ids[k].ident) okIds++;
        }
    }
    expect_u("T12 the eight identities are the dtable rows' {ndw, fnv}", okRows, N48_P87_IDS);
    expect_u("T12 ... and their identity numbers are xlat12_shader_ids.h's indices (P 85, AN 62, AR 66, AT 68, AU 69, U 90, Z 95, UCF 9)",
             okIds, N48_P87_IDS);
    static n48_p87 s; std::memset((void *)&s, 0, sizeof s);
    const uint32_t rec[8] = { 0x04008000u, 0x0421c004u, 0x00efc07fu, 0x0f90fac6u, 0u, 0u, 0x00200000u, 0u };
    const uint64_t P = ((uint64_t)46u << 32) | 0xebaa377cu, S = ((uint64_t)85u << 32) | 0xd0a62abeu;
    expect_u("T12 OFF: nothing is kept", n48_p87_note(&s, 1u, P, 5u, 38u, rec), 0u);
    s.on = N48_P87_ON;
    expect_u("T12 ON: a P record is kept", n48_p87_note(&s, 1u, P, 5u, 38u, rec), 1u);
    expect_u("T12 ... S (identity 88) is not one of the eight", n48_p87_note(&s, 1u, S, 6u, 1u, rec), 0u);
    expect_u("T12 ... a retry's re-read (same id, draw, texture) is skipped", n48_p87_note(&s, 1u, P, 5u, 38u, rec) == 0u && s.dupes == 1u, 1u);
    expect_u("T12 an UNCOMMITTED frame prints nothing", n48_p87_frame_end(&s, 1u, 0u) == 0u && s.uncommitted == 1u && s.n == 0u, 1u);
    uint32_t printed = 0u;
    for (uint64_t fr = 2u; fr < 40u; fr++) {
        for (uint32_t d = 0; d < 3u; d++) (void)n48_p87_note(&s, fr, P, 100u + d, d, rec);
        printed += n48_p87_frame_end(&s, fr, 1u);
    }
    expect_u("T12 committed frames print at most 64 lines per arm, the rest counted", printed == N48_P87_LINES &&
             s.printed == 64u && s.suppressed == 38u * 3u - 64u ? 1u : 0u, 1u);
    n48_p87_arm_reset(&s);
    (void)n48_p87_note(&s, 50u, P, 1u, 1u, rec);
    expect_u("T12 a new arm: lines again", n48_p87_frame_end(&s, 50u, 1u), 1u);
    for (uint32_t d = 0; d < N48_P87_REC + 5u; d++) (void)n48_p87_note(&s, 51u, P, d, 0u, rec);
    expect_u("T12 at most N48_P87_REC records per frame (the rest counted over-frame)", s.n == N48_P87_REC && s.overFrame == 5u, 1u);
    (void)n48_p87_note(&s, 52u, P, 1u, 0u, rec);
    expect_u("T12 another frame's end never prints this frame's stash", n48_p87_frame_end(&s, 51u, 1u), 0u);
    const n48_p87_tsharp t = n48_p87_decode(rec);
    expect_u("T12 decode: base 0x400800000 | 0x04 << 40", t.base, (0x04ull << 40) | (0x04008000ull << 8));
    expect_u("T12 decode: W = (lo 2 | hi 12 << 2) + 1, H = [27:14] + 1", t.w * 0x10000u + t.h,
             ((((0x0421c004u >> 30) & 3u) | ((0x00efc07fu & 0xfffu) << 2)) + 1u) * 0x10000u + (((0x00efc07fu >> 14) & 0x3fffu) + 1u));
    expect_u("T12 decode: fmt [28:20], sw [24:20] of word 3, COMPRESSION_EN", t.fmt * 0x10000u + t.sw * 0x10u + t.comp,
             ((0x0421c004u >> 20) & 0x1ffu) * 0x10000u + ((0x0f90fac6u >> 20) & 0x1fu) * 0x10u + 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T13
static void t_dump()
{
    const uint64_t V = 16ull << 30;   // a 16 GiB aperture
    uint64_t o = 0; uint32_t l = 0;
    const uint64_t a = n48_p87_dump_arg(0x10930000ull, 0x10000u);
    expect_u("T13 DUMP arg round-trips (off, len)", n48_p87_dump_plan(a, 0u, V, &o, &l) == N48_P87_D_OK && o == 0x10930000ull && l == 0x10000u, 1u);
    expect_u("T13 refused while a commit arm stands", n48_p87_dump_plan(a, 1u, V, &o, &l), N48_P87_D_ARMED);
    expect_u("T13 refused: zero length", n48_p87_dump_plan(n48_p87_dump_arg(0x1000ull, 0u), 0u, V, &o, &l), N48_P87_D_ZERO);
    expect_u("T13 refused: above 64 KiB", n48_p87_dump_plan(n48_p87_dump_arg(0x1000ull, 0x10100u), 0u, V, &o, &l), N48_P87_D_BIG);
    expect_u("T13 refused: the aperture unknown", n48_p87_dump_plan(a, 0u, 0u, &o, &l), N48_P87_D_NOVRAM);
    expect_u("T13 refused: starts past the aperture", n48_p87_dump_plan(n48_p87_dump_arg(V, 0x100u), 0u, V, &o, &l), N48_P87_D_APERTURE);
    expect_u("T13 refused: ends one 256 B unit past the aperture",
             n48_p87_dump_plan(n48_p87_dump_arg(V - 0x100ull, 0x200u), 0u, V, &o, &l), N48_P87_D_APERTURE);
    expect_u("T13 allowed: ends exactly at the aperture", n48_p87_dump_plan(n48_p87_dump_arg(V - 0x200ull, 0x200u), 0u, V, &o, &l), N48_P87_D_OK);
    // exhaustive-ish: every OK plan lies inside the aperture and inside the cap
    uint32_t bad = 0u, oks = 0u;
    for (uint32_t it = 0; it < 200000u; it++) {
        const uint64_t off = ((uint64_t)rnd() << 8) % (V + 0x100000ull);
        const uint32_t len = (rnd() % 0x120u) << 8;
        const uint64_t vs = (it & 1u) ? V : (uint64_t)(rnd() % 0x100000u) << 8;
        if (n48_p87_dump_plan(n48_p87_dump_arg(off, len), 0u, vs, &o, &l) == N48_P87_D_OK) {
            oks++;
            if (!(l && l <= N48_P87_DUMP_MAX && o < vs && (uint64_t)l <= vs - o && o == (off & ~0xffull) && l == len)) bad++;
        }
    }
    expect_u("T13 every OK plan is inside [0, aperture) and <= 64 KiB (200000 random)", bad, 0u);
    expect_u("T13 ... non-vacuous", oks > 10000u ? 1u : 0u, 1u);
    static n48_p87 s; std::memset((void *)&s, 0, sizeof s);
    n48_p87_geom_set(&s, n48_p87_geom_arg(1920u, 1080u, 2u, 3u, 7u));
    expect_u("T13 GEOM round-trips (1920 x 1080, 4 bytes, swizzle 3, tag 7)",
             s.gw == 1920u && s.gh == 1080u && s.gbpp == 4u && s.gsw == 3u && s.gtag == 7u, 1u);
    n48_p87_geom_set(&s, n48_p87_geom_arg(160u, 28u, 9u, 0u, 1u));
    expect_u("T13 GEOM: an invalid bpp code reads 0", s.gbpp, 0u);
    expect_u("T13 the args equal tools/conductor/imgdump531.py pack's (geom 1920 1080 4 3 7; dump 0x10930000 0x10000)",
             n48_p87_geom_arg(1920u, 1080u, 2u, 3u, 7u) == 0x0703210e07800357ull && n48_p87_dump_arg(0x10930000ull, 0x10000u) == 0x0100001093000457ull ? 1u : 0u, 1u);
    expect_u("T13 the args carry the selector and M (87 | 3 << 8, 87 | 4 << 8)",
             (n48_p87_geom_arg(1u, 1u, 0u, 0u, 0u) & 0xffffull) == 0x357ull && (n48_p87_dump_arg(0u, 0x100u) & 0xffffull) == 0x457ull, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T14
static void t_glue(const char *ahhPath, const char *peerPath, const char *nbPath, const char *x12cPath)
{
    const std::string s = slurp(ahhPath), p = slurp(peerPath), nb = slurp(nbPath), xc = slurp(x12cPath);
    expect_u("T14 the four sources were read", !s.empty() && !p.empty() && !nb.empty() && !xc.empty(), 1u);
    // default OFF: zero-initialised, and only the verb writes `on`
    expect_u("T14 switch 86 and 87 are zero-initialised (OFF at boot), exactly once each",
             count(s, "static n48_p86 gP86 {};") == 1u && count(s, "static n48_p87 gP87 {};") == 1u, 1u);
    expect_u("T14 switch 86's mode is written only by its verb (ON and OFF)",
             count(s, "__atomic_store_n(&gP86.on,") == 2u && count(s, "gP86.on =") == 0u, 1u);
    expect_u("T14 switch 87's mode is written only by its verb (ON and OFF)",
             count(s, "__atomic_store_n(&gP87.on,") == 2u && count(s, "gP87.on =") == 0u, 1u);
    expect_u("T14 M values: 86 M 1 ON, M 2 OFF",
             count(s, "        } else if (m == 1u) {\n            __atomic_store_n(&gP86.on, (uint32_t)N48_P86_ON, __ATOMIC_RELEASE); changed = 1;\n"
                      "        } else if (m == 2u) {\n            __atomic_store_n(&gP86.on, (uint32_t)N48_P86_OFF, __ATOMIC_RELEASE); changed = 1;"), 1u);
    expect_u("T14 M values: N48_P87_M_ON 1, _OFF 2", N48_P87_M_ON * 0x10u + N48_P87_M_OFF, 0x12u);
    // the lock: IOLockTryLock only
    const std::string reg = between(s, "static n48_p86 gP86 {};", "uint32_t hw_p73_present(uint64_t phys, uint64_t presentNo)");
    expect_u("T14 the present's lock is IOLockTryLock(gXdLock) (once) and never IOLockLock in switch 86's glue",
             !reg.empty() && count(reg, "if (!gXdLock || !IOLockTryLock(gXdLock)) return 0u;") == 1u && count(reg, "IOLockLock(") == 0u &&
             count(reg, "IOLockUnlock(gXdLock);") == 1u, 1u);
    const std::string pres = between(s, "uint32_t hw_p73_present(uint64_t phys, uint64_t presentNo)", "void hw_p73_delivered()");
    expect_u("T14 hw_p73_present takes no lock itself (only through kP86Io's try-lock)",
             !pres.empty() && count(pres, "IOLockLock(") == 0u && count(pres, "IOLockTryLock(") == 0u, 1u);
    expect_u("T14 the io is {p86_trylock, p86_unlock, ks_x_read, gKsRing}: the expiry poll's reader over the flight ring",
             count(s, "static const n48_p86_io kP86Io = { &p86_trylock, &p86_unlock, &ks_x_read, &gKsRing, nullptr };"), 1u);
    expect_u("T14 ks_x_read is navi48_vram_read_mm of ONE dword (the poll's read)",
             count(s, "static uint32_t ks_x_read(void *, uint64_t off, uint32_t *val) { return navi48_vram_read_mm(off, val, 1) ? 1u : 0u; }"), 1u);
    expect_u("T14 ... and the expiry poll itself reads through it", count(s, "&ks_x_read, nullptr, gKsXRet"), 1u);
    expect_u("T14 the present answers through n48_p86_should_copy with switch 73's and 86's modes, once",
             count(s, "n48_p86_should_copy(gP73On ? 1u : 0u, p86_mode() == N48_P86_ON ? 1u : 0u, &gP73, &gP86, phys, &kP86Io,"), 1u);
    expect_u("T14 gfx_p86.h is the only caller of n48_p73_retired outside the two retirement sites (no new p73_retired site)",
             count(s, "p73_retired(") == 5u, 1u);
    // the verbs
    expect_u("T14 PIN SWITCH-GUARD:86 - the verb calls the continuous guard with its own selector, exactly once",
             count(s, "n48_cm_cont_switch_refused(86u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("T14 PIN SWITCH-GUARD:87 - the verb calls the continuous guard with its own selector, exactly once",
             count(s, "n48_cm_cont_switch_refused(87u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("T14 the DUMP asks the plan with the live arm state before any read",
             count(s, "const uint32_t d = n48_p87_dump_plan(arg, hw_cm_armed() || gXdShot.cont ? 1u : 0u, vramSize, &off, &len);") == 1u &&
             at(s, "const uint32_t d = n48_p87_dump_plan(") < at(s, "if (!navi48_vram_read_mm(off + at, gP87Buf, n / 4u)) {") &&
             at(s, "if (d != N48_P87_D_OK) { gP87.dumpRefused++; return d; }") < at(s, "if (!navi48_vram_read_mm(off + at, gP87Buf, n / 4u)) {"), 1u);
    expect_u("T14 the DUMP reads only [off, off + len): at < len, n = min(128, len - at)",
             count(s, "    for (uint32_t at = 0u; at < len; at += N48_P87_DUMP_LINE) {\n"
                      "        const uint32_t n = (len - at < N48_P87_DUMP_LINE) ? len - at : N48_P87_DUMP_LINE;"), 1u);
    // the timing instruments
    expect_u("T14 the timed GFX entry is the one installed, and it calls hook_gfxCommitIB",
             count(s, "copy[kVtHeaderWords + kPm4GfxCommitSlot] = reinterpret_cast<void *>(&hook_gfxCommitIB_timed);") == 1u &&
             count(s, "reinterpret_cast<void *>(&hook_gfxCommitIB);") == 0u &&
             count(s, "    const uint64_t rv = hook_gfxCommitIB(self, info);") == 1u, 1u);
    expect_u("T14 the timed pageTexture entry is the one installed, and it calls hook_page_texture",
             count(p, "copy[kVtHeader + kResPageTextureSlot] = reinterpret_cast<void *>(&hook_page_texture_timed);") == 1u &&
             count(p, "reinterpret_cast<void *>(&hook_page_texture);") == 0u &&
             count(p, "    const uint8_t rv = hook_page_texture(self, toVram, dst, src);\n    n48::hw_wt_pagein(t0);") == 1u, 1u);
    expect_u("T14 the judge is timed around its one call",
             count(s, "    const uint64_t wtDecide0 = wt_ticks();") == 1u &&
             at(s, "    const uint64_t wtDecide0 = wt_ticks();") < at(s, "    const uint32_t xdAction = gfxsrc_decide_frame(") &&
             at(s, "    wt_note_since(&gWtDecide, wtDecide0);") > at(s, "    const uint32_t xdAction = gfxsrc_decide_frame("), 1u);
    expect_u("T14 the mmhold snapshot at the continuous START (after its line) and STOP (after its line)",
             count(s, "navi48_mmhold_snapshot(\"START\");") == 1u && count(s, "navi48_mmhold_snapshot(\"STOP\");") == 1u &&
             at(s, "navi48_mmhold_snapshot(\"START\");") > at(s, "HWLOG(N48_CM_CONT_START_FMT,") &&
             at(s, "navi48_mmhold_snapshot(\"STOP\");") > at(s, "HWLOG(N48_CM_CONT_STOP_FMT,") &&
             count(nb, "void navi48_mmhold_snapshot(const char *where) {") == 1u, 1u);
    expect_u("T14 the COPIED line prints the last gate seq, and the gate is its only writer",
             count(s, "__atomic_load_n(&gXdCmGateSeqLast, __ATOMIC_RELAXED), gP73.copy_lines);") == 1u &&
             count(s, "__atomic_store_n(&gXdCmGateSeqLast,") == 1u, 1u);
    // switch 87's hooks
    expect_u("T14 87: the heap-index observer is handed to the translator beside tex_note",
             count(s, "            ex.tex_note_ix = &gfxsrc_tex_note_ix;") == 1u &&
             at(s, "            ex.tex_note_ix = &gfxsrc_tex_note_ix;") > at(s, "            ex.tex_note = &gfxsrc_tex_note;"), 1u);
    expect_u("T14 87: xlat12 calls tex_note_ix right after tex_note, with aidx[i]",
             count(xc, "if (ex->tex_note_ix) ex->tex_note_ix(ex->desc_ctx, ((uint64_t)a->ndw << 32) | a->fnv, (draw_at & 0xFFFFFFu) | (i << 24),\n"
                       "                                             aidx[i], rec);") == 1u, 1u);
    expect_u("T14 87: the frame's records are printed after the gate answered, with commitOk",
             count(s, "p87_frame_end(gXdC.judged + 1u, commitOk ? 1u : 0u, tgtVa);") == 1u &&
             at(s, "p87_frame_end(gXdC.judged + 1u, commitOk ? 1u : 0u, tgtVa);") >
                 at(s, "? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;"), 1u);
    expect_u("T14 87: OFF, the observer is one load and a return",
             count(s, "    if (__atomic_load_n(&gP87.on, __ATOMIC_ACQUIRE) != N48_P87_ON) return;   // OFF: one load"), 1u);
}

int main(int argc, char **argv)
{
    t_basic();
    t_poll_equiv();
    t_identity();
    t_widths();
    t_dump();
    if (argc >= 7) {
        t_p87(argv[5], argv[6]);
        t_glue(argv[1], argv[2], argv[3], argv[4]);
    } else {
        expect_u("the source files were given (AHH, peer, Navi48Bringup.cpp, xlat12_ib.c, dtable rows, shader ids)", 0u, 1u);
    }
    std::printf("%d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
