// gfx_tlb83.h — build 0.0.528. PURE (no kernel headers, no hardware): the host suite
// (tests/gfx_tlb83_test.cpp) drives the SAME poll the kext's gmc_flush_gpu_tlb calls, through a fake register and a fake clock.
//
// (CONFIRMED in code): the keystone's withdraw and re-arm (hook_unmapVA) and every keystone arm (rootwrite_arm_context) call
// navi48_gmc_flush_tlb_vmid -> amdgpu::gmc_flush_gpu_tlb, which writes the GFXHUB engine-17 invalidate request and waits for the
// ack with poll_reg (amdgpu_regs.h): read, and if the bit is not yet set IOSleep(1) (>= 1 ms + wake-up). Upstream
// gmc_v12_0_flush_vm_hub polls with udelay(1). The ack wait is REQUIRED ('s tlbAckAtWrite); only its manner changes.
//
// ITEM 1 — SWITCH 83 (default OFF): A TLB-ONLY SPIN POLL. ON: read the ack register; if the bit is not set, IODelay(1) and read
// again, for at most N48_TLB83_SPIN_US of spinning (by the clock) and at most N48_TLB83_SPIN_MAX_ITERS delays (so a clock that does
// not advance still falls back); then the EXISTING sleep loop (read, IOSleep(1), elapsed += 1000) for the remainder of the
// UNCHANGED 100 ms timeout, its elapsed count starting at the spin's elapsed time. The mask, the expected value, the timeout and
// the return semantics are poll_reg's: acked iff (value & mask) == expected was READ; a timeout returns 0 (false), exactly as
// poll_reg, after the elapsed count reached the timeout. poll_reg itself is NOT changed (the PSP/SMU waits keep sleeping); OFF,
// gmc_flush_gpu_tlb calls poll_reg exactly as 0.0.527. ON, the request write and the ack wait are taken under a LEAF lock
// (item 5: the kext has no lock that serialises the unmap thread's flushes against the commit thread's; upstream holds
// invalidate_lock around the same pair).
//
// ITEM 2 — COUNTERS (unconditional, decision-inert): per call, by mode: calls, acked, timeouts, the wait (us, max and sum); ON only:
// reads to the ack (histogram 1, 2-3, 4-10, 11-100, 101-1000, > 1000), spin us, calls that fell back to the sleep loop (and how many
// of those were then acked), lock contention. Nothing reads them to decide anything.
//
// ITEM 3 — THE `acked` LOG LINE IS CAPPED (log-only, UNSWITCHED — the only unswitched change of this build): the first
// N48_TLB83_ACK_LINES per boot, then a count only (printed on the bare-83 line). The timeout line stays uncapped.
//
// `83 | M << 8`: M 1 = ON (339), M 2 = OFF (595, the default and the boot value); bare `83` reads; any other M is refused
// unchanged. Joins the continuous mid-arm guard (n48_cm_cont_switch_refused).
#ifndef N48_GFX_TLB83_H
#define N48_GFX_TLB83_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define N48_TLB83_SWITCH 83u
enum { N48_TLB83_OFF = 0u, N48_TLB83_ON = 1u };
#define N48_TLB83_M_READ 0xFFu   /* bare 83: read only */
#define N48_TLB83_M_BAD  0xFEu   /* an unknown M: refused, unchanged */

#define N48_TLB83_TIMEOUT_US      100000u   /* gmc_flush_gpu_tlb's timeout, UNCHANGED (poll_reg's timeout_us argument) */
#define N48_TLB83_SPIN_US         2000u     /* the spin's bound by the clock */
#define N48_TLB83_SPIN_MAX_ITERS  2000u     /* ... and by count: each IODelay(1) is >= 1 us, so 2000 delays are >= 2000 us */
#define N48_TLB83_SLEEP_STEP_US   1000u     /* poll_reg's elapsed step per IOSleep(1), UNCHANGED */
#define N48_TLB83_ACK_LINES       32u       /* item 3: `acked` lines logged per boot */

/* The verb's M byte -> the mode to store (or READ / BAD). */
static inline uint32_t n48_tlb83_mode_of_m(uint32_t m)
{
    switch (m) {
    case 0u: return N48_TLB83_M_READ;
    case 1u: return N48_TLB83_ON;
    case 2u: return N48_TLB83_OFF;
    default: return N48_TLB83_M_BAD;
    }
}
static inline const char *n48_tlb83_mode_name(uint32_t mode)
{
    return mode == N48_TLB83_ON ? "ON (spin, then sleep)" : mode == N48_TLB83_OFF ? "OFF (poll_reg, default)" : "?";
}

/* ---- the poll (item 1) ------------------------------------------------------------------------------------------------------- */
/* The four primitives the poll needs. Kext: RREG32 of the ack register, the uptime in us, IODelay(1), IOSleep(1). Test: fakes. */
typedef struct {
    void *c;
    uint32_t (*rd)(void *c);
    uint64_t (*now_us)(void *c);
    void (*delay1)(void *c);
    void (*sleep1)(void *c);
} n48_tlb83_io;
typedef struct {
    uint32_t ok;        /* 1 = acked ((value & mask) == expected was read); 0 = timeout, exactly as poll_reg's false */
    uint32_t value;     /* the last value read (poll_reg's *outValue) */
    uint32_t reads;     /* register reads, the acking one included */
    uint32_t delays;    /* IODelay(1) calls in the spin */
    uint32_t sleeps;    /* IOSleep(1) calls after the fall-back */
    uint32_t fell;      /* 1 = the spin ended without an ack and the sleep loop ran */
    uint64_t spin_us;   /* the spin's elapsed time by the clock (0 when the first read acked) */
    uint64_t elapsed;   /* the timeout accounting at return: spin_us, then + 1000 per sleep (poll_reg's accounting) */
} n48_tlb83_res;

static inline uint32_t n48_tlb83_poll(const n48_tlb83_io *io, uint32_t mask, uint32_t expected, uint64_t timeout_us,
                                      n48_tlb83_res *r)
{
    r->ok = 0u; r->value = 0u; r->reads = 0u; r->delays = 0u; r->sleeps = 0u; r->fell = 0u; r->spin_us = 0ull; r->elapsed = 0ull;
    const uint64_t t0 = io->now_us(io->c);
    uint64_t spun = 0ull;
    /* THE SPIN: read, and if not acked, IODelay(1) - bounded by the clock AND by count. */
    for (;;) {
        const uint32_t v = io->rd(io->c);
        r->reads++;
        r->value = v;
        if ((v & mask) == expected) { r->ok = 1u; r->spin_us = spun; r->elapsed = spun; return 1u; }
        const uint64_t now = io->now_us(io->c);
        spun = now >= t0 ? now - t0 : (uint64_t)N48_TLB83_SPIN_US;   /* a clock that went back ends the spin */
        if (spun >= N48_TLB83_SPIN_US || spun >= timeout_us || r->delays >= N48_TLB83_SPIN_MAX_ITERS) break;
        io->delay1(io->c);
        r->delays++;
    }
    /* THE FALL-BACK: poll_reg's loop (timeout check, IOSleep(1), elapsed += 1000, read) for the rest of the SAME timeout. The
     * spin's last read is this loop's first read, so the loop starts at its timeout check. */
    r->fell = 1u;
    r->spin_us = spun;
    uint64_t elapsed = spun;
    for (;;) {
        if (elapsed >= timeout_us) { r->elapsed = elapsed; return 0u; }   /* poll_reg: `if (elapsed_us >= timeout_us) return false` */
        io->sleep1(io->c);
        r->sleeps++;
        elapsed += N48_TLB83_SLEEP_STEP_US;
        const uint32_t v = io->rd(io->c);
        r->reads++;
        r->value = v;
        if ((v & mask) == expected) { r->ok = 1u; r->elapsed = elapsed; return 1u; }
    }
}

/* ---- the counters (item 2) --------------------------------------------------------------------------------------------------- */
#define N48_TLB83_RB 6u   /* reads-to-ack buckets: 1, 2-3, 4-10, 11-100, 101-1000, > 1000 */
static inline uint32_t n48_tlb83_rbucket(uint32_t reads)
{
    if (reads <= 1u) return 0u;
    if (reads <= 3u) return 1u;
    if (reads <= 10u) return 2u;
    if (reads <= 100u) return 3u;
    if (reads <= 1000u) return 4u;
    return 5u;
}
typedef struct {
    uint64_t calls[2], acked[2], timeouts[2], waitMax[2], waitSum[2];   /* [N48_TLB83_OFF], [N48_TLB83_ON] */
    uint64_t rd[N48_TLB83_RB];                                         /* ON: reads to the ack, acked calls only */
    uint64_t spinUs, spinMax, fell, fellAcked, contended;              /* ON only */
    uint64_t ackLines, ackUnlogged;                                    /* item 3 */
} n48_tlb83_stats;
static inline void n48_tlb83_max(uint64_t *m, uint64_t v)
{
    uint64_t cur = __atomic_load_n(m, __ATOMIC_RELAXED);
    while (v > cur && !__atomic_compare_exchange_n(m, &cur, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
/* OFF (poll_reg): the call's outcome and its wait by the clock. */
static inline void n48_tlb83_note_off(n48_tlb83_stats *s, uint32_t ok, uint64_t wait_us)
{
    __atomic_fetch_add(&s->calls[N48_TLB83_OFF], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(ok ? &s->acked[N48_TLB83_OFF] : &s->timeouts[N48_TLB83_OFF], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->waitSum[N48_TLB83_OFF], wait_us, __ATOMIC_RELAXED);
    n48_tlb83_max(&s->waitMax[N48_TLB83_OFF], wait_us);
}
/* ON: the poll's result, its wait by the clock, and whether the lock was contended. */
static inline void n48_tlb83_note_on(n48_tlb83_stats *s, const n48_tlb83_res *r, uint64_t wait_us, uint32_t contended)
{
    __atomic_fetch_add(&s->calls[N48_TLB83_ON], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(r->ok ? &s->acked[N48_TLB83_ON] : &s->timeouts[N48_TLB83_ON], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->waitSum[N48_TLB83_ON], wait_us, __ATOMIC_RELAXED);
    n48_tlb83_max(&s->waitMax[N48_TLB83_ON], wait_us);
    if (r->ok) __atomic_fetch_add(&s->rd[n48_tlb83_rbucket(r->reads)], 1ull, __ATOMIC_RELAXED);
    __atomic_fetch_add(&s->spinUs, r->spin_us, __ATOMIC_RELAXED);
    n48_tlb83_max(&s->spinMax, r->spin_us);
    if (r->fell) {
        __atomic_fetch_add(&s->fell, 1ull, __ATOMIC_RELAXED);
        if (r->ok) __atomic_fetch_add(&s->fellAcked, 1ull, __ATOMIC_RELAXED);
    }
    if (contended) __atomic_fetch_add(&s->contended, 1ull, __ATOMIC_RELAXED);
}
/* Item 3: log this `acked` line? 1 for the first N48_TLB83_ACK_LINES per boot; after that 0 and counted. */
static inline uint32_t n48_tlb83_ack_line(n48_tlb83_stats *s)
{
    if (__atomic_fetch_add(&s->ackLines, 1ull, __ATOMIC_RELAXED) < N48_TLB83_ACK_LINES) return 1u;
    __atomic_fetch_add(&s->ackUnlogged, 1ull, __ATOMIC_RELAXED);
    return 0u;
}

/* Display caps for the terse kswin2 line (a printed cap means "at least"). */
static inline uint32_t n48_tlb83_c5(uint64_t v) { return v > 99999ull ? 99999u : (uint32_t)v; }
static inline uint32_t n48_tlb83_c7(uint64_t v) { return v > 9999999ull ? 9999999u : (uint32_t)v; }

/* THE BARE-83 LINES (three, so every counter prints at full width). Line 1: mode, how; ON calls, acked, timeouts; reads to the
 * ack (1, 2-3, 4-10, 11-100, 101-1000, > 1000); spin us total and max. Line 2: ON fell back to sleep (and acked after it), wait max
 * and sum, the lock contended; OFF (poll_reg) calls, acked, timeouts, wait max and sum. Line 3: the `acked` lines logged and not
 * logged (item 3), the lock's presence, the constants. */
#define N48_TLB83_REPORT1_FMT \
    "tlb83: switch 83 is %s%s. ON: calls %llu acked %llu timeouts %llu; reads-to-ack 1:%llu 2-3:%llu 4-10:%llu 11-100:%llu " \
    "101-1000:%llu >1000:%llu; spin %llu us max %llu"
#define N48_TLB83_REPORT1_ARGS(s, modeName, how) \
    (modeName), (how), (unsigned long long)(s)->calls[N48_TLB83_ON], (unsigned long long)(s)->acked[N48_TLB83_ON], \
    (unsigned long long)(s)->timeouts[N48_TLB83_ON], (unsigned long long)(s)->rd[0], (unsigned long long)(s)->rd[1], \
    (unsigned long long)(s)->rd[2], (unsigned long long)(s)->rd[3], (unsigned long long)(s)->rd[4], (unsigned long long)(s)->rd[5], \
    (unsigned long long)(s)->spinUs, (unsigned long long)(s)->spinMax
#define N48_TLB83_REPORT2_FMT \
    "tlb83: ON fell back to sleep %llu (acked %llu); wait max %llu sum %llu us; lock contended %llu. OFF (poll_reg): calls %llu " \
    "acked %llu timeouts %llu; wait max %llu sum %llu us"
#define N48_TLB83_REPORT2_ARGS(s) \
    (unsigned long long)(s)->fell, (unsigned long long)(s)->fellAcked, (unsigned long long)(s)->waitMax[N48_TLB83_ON], \
    (unsigned long long)(s)->waitSum[N48_TLB83_ON], (unsigned long long)(s)->contended, \
    (unsigned long long)(s)->calls[N48_TLB83_OFF], (unsigned long long)(s)->acked[N48_TLB83_OFF], \
    (unsigned long long)(s)->timeouts[N48_TLB83_OFF], (unsigned long long)(s)->waitMax[N48_TLB83_OFF], \
    (unsigned long long)(s)->waitSum[N48_TLB83_OFF]
#define N48_TLB83_REPORT3_FMT \
    "tlb83: `flush_gpu_tlb ... acked` lines: logged %llu of the first %u, not logged %llu (item 3, unswitched). Leaf lock %s. " \
    "Spin bound %u us / %u delays; timeout %u us (unchanged)."
#define N48_TLB83_REPORT3_ARGS(s, lockName) \
    (unsigned long long)((s)->ackLines < N48_TLB83_ACK_LINES ? (s)->ackLines : N48_TLB83_ACK_LINES), N48_TLB83_ACK_LINES, \
    (unsigned long long)(s)->ackUnlogged, (lockName), N48_TLB83_SPIN_US, N48_TLB83_SPIN_MAX_ITERS, N48_TLB83_TIMEOUT_US

#ifdef __cplusplus
}
#endif

#endif /* N48_GFX_TLB83_H */
