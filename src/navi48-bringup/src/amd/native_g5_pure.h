//
//  native_g5_pure.h - the PURE half of GPU-apps stage G5 Stage 1 (kext 0.0.650): an application credit that keeps WindowServer from queuing behind app jobs
//  (an internal design note, "G5 design" and "G5 Stage 1 build spec"). No kernel header is included: tests/native_g5_test.cpp compiles this file and
//  drives the very functions the kext calls (amd/native_s1c.cpp ring_submit_app_g5, n1c_g5_stat; Navi48Bringup.cpp accelExperiment).
//
//  DEFAULT OFF: boot-arg navi48-g5=1 (effective only with navi48-multisession=1). OFF, n1c_submit calls ring_submit exactly as 0.0.642 and writes no G5 state.
//  ON:
//    * WindowServer (the session admitted by the uid-88 rule, found by Session::ws / its session id, NEVER by its slot number) is never gated and never waits here.
//      Its submit only stamps "WindowServer was active now".
//    * every other session (an allow-listed app, a root tool) is admitted to the ring only when (admit(), in this order)
//        1. there is ring space for the submit PLUS a reserve of kReserveDw dwords that only WindowServer may use;
//        2. while WindowServer is ACTIVE (it submitted within kWsWindowNs): fewer than K app jobs are in flight (K = navi48-g5credit, 1..4, default 1), so the
//           app jobs ahead of any WindowServer emission are at most K;
//        3. no other app has been waiting longer (strict first-come order; ties to the lower slot).
//      The admission verdict and the seqno are taken in ONE hold of gRingLock; the wait for a verdict of Wait happens OUTSIDE it.
//    * a wait is bounded: at kWaitCapNs the stall path runs once (G2's recovery may poison a wedged app that holds the credit), at kG5WaitBoundNs the app's
//      Submit returns Timeout.
//
#pragma once
#include <stdint.h>

#include "native_g2_pure.h"   // OwnerLog, kOwnerN, kOwnerKernel, kMaxSessions, latch_value; (via native_s1c_pure.h) ring_has_space, kWaitCapNs, kTimeout

namespace n48native {
namespace g5 {

using g2::kMaxSessions;

// ---- constants ---------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kWsWindowNs = 100000000ull;      // WindowServer counts as active for 100 ms after its last submit (it submits ~93/s even when idle: gaps of ~11 ms)
constexpr uint32_t kReserveDw  = 1024u;             // 25 % of the 4096-dword ring, usable only by WindowServer while G5 is ON
constexpr uint32_t kCreditDefault = 1u, kCreditMax = 4u;
// An app Submit waits at most this long for its turn: G2's 2 s ring bound plus one recovery's bound (native_s1c.cpp kRecoverWaitNs = 6 s).
constexpr uint64_t kRecoverBoundNs = 6000000000ull;
constexpr uint64_t kG5WaitBoundNs  = s1c::kWaitCapNs + kRecoverBoundNs;
static_assert(kG5WaitBoundNs == 8000000000ull, "the spec: 8 s");
// A since[] entry older than the wait bound plus 1 s is a leaked entry (its owner is long gone): ignored, so it heals itself.
constexpr uint64_t kSinceStaleNs = kG5WaitBoundNs + 1000000000ull;
static_assert(kReserveDw + 16u + s1c::submit_dwords(64u, true) < 4096u, "the reserve plus one largest submit fit the 4096-dword ring");

// ---- the switch and K ----------------------------------------------------------------------------------------------------------------------------
// navi48-g5=1 latches ON (g2::latch_value); it is effective only when navi48-multisession=1 is latched ON as well.
constexpr bool on_effective(bool g5LatchOn, bool multiLatchOn) { return g5LatchOn && multiLatchOn; }
// navi48-g5credit=<1..4>; absent or out of range = 1.
constexpr uint32_t credit_value(bool present, uint32_t v) { return (present && v >= 1u && v <= kCreditMax) ? v : kCreditDefault; }

// ---- is WindowServer active? -----------------------------------------------------------------------------------------------------------------------
// wsOpen: a WindowServer session is open. lastWsNs: the time of its last submit (0 = never). A clock that goes backwards (lastWsNs > now) is NOT active.
constexpr bool ws_active(bool wsOpen, uint64_t lastWsNs, uint64_t now, uint64_t windowNs) {
    return wsOpen && lastWsNs != 0ull && now >= lastWsNs && now - lastWsNs < windowNs;
}

// ---- how many app jobs are in flight? ----------------------------------------------------------------------------------------------------------------
// Counts the seqnos q in (retired, emitted] whose owner is an APP (any session that is neither the kernel probe nor WindowServer), stopping at `cap`.
// FAIL CLOSED: more than kOwnerN outstanding returns cap (the log wrapped), and an entry that does not carry its own seqno counts as an app job.
// wsId = the WindowServer session id (0 = none open: nobody is skipped as WindowServer).
inline uint32_t app_in_flight(const g2::OwnerLog &l, uint64_t retired, uint64_t emitted, uint32_t wsId, uint32_t cap) {
    if (cap == 0u || retired >= emitted) return 0u;
    if (emitted - retired > (uint64_t)g2::kOwnerN) return cap;
    uint32_t n = 0u;
    for (uint64_t q = retired + 1ull; q <= emitted; q++) {
        const g2::OwnerEnt &e = l.e[q % g2::kOwnerN];
        if (e.seq == q) {
            if (e.slot == g2::kOwnerKernel) continue;
            if (wsId != 0u && e.sessId == wsId) continue;
        }
        if (++n >= cap) return cap;
    }
    return n;
}
// WindowServer's seqnos outstanding (the stat page). An entry that does not carry its seqno is not counted; more than kOwnerN outstanding returns kOwnerN.
inline uint32_t ws_outstanding(const g2::OwnerLog &l, uint64_t retired, uint64_t emitted, uint32_t wsId) {
    if (wsId == 0u || retired >= emitted) return 0u;
    if (emitted - retired > (uint64_t)g2::kOwnerN) return g2::kOwnerN;
    uint32_t n = 0u;
    for (uint64_t q = retired + 1ull; q <= emitted; q++) {
        const g2::OwnerEnt &e = l.e[q % g2::kOwnerN];
        if (e.seq == q && e.slot != g2::kOwnerKernel && e.sessId == wsId) n++;
    }
    return n;
}

// ---- first come, first served ------------------------------------------------------------------------------------------------------------------------
// since[slot] = the time that slot's Submit began waiting (0 = not waiting), guarded by gRingLock. True when ANOTHER slot has been waiting longer than this one
// (a slot that is not waiting yet counts as starting now). Ties go to the lower slot. An entry older than staleNs, or in the future, is ignored.
inline bool older_waiter(const uint64_t *since, uint32_t mySlot, uint64_t now, uint64_t staleNs) {
    const uint64_t mine = (mySlot < kMaxSessions && since[mySlot] != 0ull) ? since[mySlot] : now;
    for (uint32_t j = 0; j < kMaxSessions; j++) {
        if (j == mySlot) continue;
        const uint64_t t = since[j];
        if (t == 0ull || t > now || now - t > staleNs) continue;
        if (t < mine || (t == mine && j < mySlot)) return true;
    }
    return false;
}
inline void since_mark(uint64_t *since, uint32_t slot, uint64_t now) { if (slot < kMaxSessions && since[slot] == 0ull) since[slot] = now; }
inline void since_clear(uint64_t *since, uint32_t slot) { if (slot < kMaxSessions) since[slot] = 0ull; }

// ---- the admission --------------------------------------------------------------------------------------------------------------------------------------
enum Verdict : uint32_t { kAdmit = 0u, kWait = 1u };
// on: G5 is ON. isWs: the submitting session is WindowServer. wsActive: ws_active(). K: the credit. inUse: app_in_flight(). freeDw / needDw: ring space and the submit's size.
// reserveDw: kReserveDw. olderWaiter: older_waiter(). The ORDER is the contract: a WindowServer submit and the OFF switch never wait.
constexpr uint32_t admit(bool on, bool isWs, bool wsActive, uint32_t K, uint32_t inUse, uint32_t freeDw, uint32_t needDw, uint32_t reserveDw, bool olderWaiter) {
    if (!on || isWs) return kAdmit;
    if (!s1c::ring_has_space(freeDw, needDw + reserveDw)) return kWait;
    if (wsActive && inUse >= K) return kWait;
    if (olderWaiter) return kWait;
    return kAdmit;
}
// Everything the kernel feeds the verdict, read under ONE hold of gRingLock.
struct AdmitIn {
    bool on, isWs;
    bool wsOpen; uint64_t lastWsNs, now;
    uint32_t K;
    uint64_t retired, emitted;
    uint32_t wsId;
    uint32_t freeDw, needDw;
    const uint64_t *since; uint32_t slot;
};
// The composite the kernel calls inside gRingLock (ring_submit_app_g5) and the host simulation drives.
inline uint32_t admission(const g2::OwnerLog &log, const AdmitIn &a) {
    const bool act = ws_active(a.wsOpen, a.lastWsNs, a.now, kWsWindowNs);
    const uint32_t inUse = (a.on && !a.isWs && act) ? app_in_flight(log, a.retired, a.emitted, a.wsId, a.K) : 0u;
    const bool older = (a.on && !a.isWs) ? older_waiter(a.since, a.slot, a.now, kSinceStaleNs) : false;
    return admit(a.on, a.isWs, act, a.K, inUse, a.freeDw, a.needDw, kReserveDw, older);
}

// ---- the wait loop's decision (after the lock is dropped) ---------------------------------------------------------------------------------------
enum WaitStep : uint32_t { kKeepWaiting = 0u, kStallNow = 1u, kGiveUp = 2u };
// elapsed: time since this Submit first got a Wait. stallDone: stall_from_wait() already ran once for this Submit.
constexpr uint32_t wait_step(uint64_t elapsedNs, bool stallDone) {
    return elapsedNs >= kG5WaitBoundNs ? (uint32_t)kGiveUp : (!stallDone && elapsedNs >= s1c::kWaitCapNs) ? (uint32_t)kStallNow : (uint32_t)kKeepWaiting;
}

// ---- the counters -----------------------------------------------------------------------------------------------------------------------------------------
struct Stat { uint64_t delayed, delayNsTotal, delayNsMax, timeouts, admitted; };
// An app Submit was admitted; waitedNs > 0 when it had to wait first.
inline void stat_admit(Stat &s, uint64_t waitedNs) {
    s.admitted++;
    if (waitedNs != 0ull) { s.delayed++; s.delayNsTotal += waitedNs; if (waitedNs > s.delayNsMax) s.delayNsMax = waitedNs; }
}
inline void stat_timeout(Stat &s) { s.timeouts++; }

// ---- the stat page (accel 103 `sessstat 5`) -----------------------------------------------------------------------------------------------------------
constexpr uint32_t kStatPage = 5u;     // 0..3 sessions, 4 the global page (G2); 0.0.640-0.0.641 claim bits of page 4 o[2] only; page 5 is G5's
constexpr uint64_t kNever = ~0ull;     // o[11] when WindowServer never submitted
// Page 5 exists only with G5 ON. OFF it is BadArgument as before 0.0.650.
constexpr bool args_ok(bool g5on, uint32_t a, uint64_t arg) { return g5on && a == g2::kActSessStat && arg == (uint64_t)kStatPage; }
struct StatView { bool wsActive, wsOpen; uint32_t K, appInFlight, wsOutstanding; Stat st; uint64_t sinceWsMs; };
constexpr uint64_t stat_flags(bool wsActive, bool wsOpen) { return 1ull | (wsActive ? 2ull : 0ull) | (wsOpen ? 4ull : 0ull); }
inline void stat_out(const StatView &v, uint64_t *o) {
    o[0] = g2::kStatOk; o[1] = kStatPage; o[2] = stat_flags(v.wsActive, v.wsOpen); o[3] = v.K; o[4] = v.appInFlight; o[5] = v.wsOutstanding;
    o[6] = v.st.delayed; o[7] = v.st.delayNsTotal / 1000000ull; o[8] = v.st.delayNsMax / 1000000ull; o[9] = v.st.timeouts; o[10] = v.st.admitted;
    o[11] = v.sinceWsMs; o[12] = kReserveDw;
}
static_assert(g2::kOutN == 13u, "the G5 page uses all 13 scalars");

} // namespace g5
} // namespace n48native
