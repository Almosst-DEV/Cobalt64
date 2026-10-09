//
//  native_g1_pure.h - the PURE half of GPU-apps stage G1 (kext 0.0.623): recovering the native kernel GFX queue from a submission that never
//  retires, WITHOUT a reboot (an internal design note section 4, G1). No kernel header is included: tests/native_g1_test.cpp compiles
//  this file and drives the very decision functions and run templates the kext calls (native_s1c.cpp, n1c_g1_*), with a fake environment.
//
//  DEFAULT OFF: the three accel verbs exist only when the boot-arg navi48-g1=1 was latched (latch_value / action_admitted); with it absent the
//  user client and accelExperiment refuse actions 100..102 exactly as before (BadArgument), and nothing below is reached.
//
//  The verbs (accel user client = administrator only, Navi48UserClient::initWithTask):
//    100 hangtest <mode>      mode 1 (the only one): a kernel-owned test context on VMID 8 (n1c_open's own gates, exclusivity included) submits ONE
//                             indirect buffer holding a WAIT_REG_MEM on a never-matching memory dword (no timeout). Refused unless NO native session
//                             is open (a WindowServer session is named as such), and never while HUNG or while a test context already holds the GPU.
//                             It confirms the hang with a bounded 2 s wait and then latches HUNG through the ordinary latch path.
//    101 hangrecover <method> 0 auto (mesreset, then remap), 1 release (the CPU writes the awaited value: a harness control, not a GPU recovery),
//                             2 mesreset (MES RESET reset_legacy_gfx), 3 remap (MES REMOVE_QUEUE unmap + MQD/ring re-init + ADD_QUEUE map_legacy_kq),
//                             4 abandon (close the test context, leaking as HUNG does today). Every step is bounded; after each method a probe IB
//                             (WRITE_DATA under VMID 8 + the kernel seqno) must retire AND its dword must read back, and only then is HUNG cleared.
//    102 hangstat <page>      page 0 the summary, pages 1..8 one attempt each.
//
#pragma once
#include <stdint.h>

namespace n48native {
namespace g1 {

// ---- the verbs and their admission -----------------------------------------------------------------------------------------------------------
constexpr uint32_t kActHangTest = 100u, kActHangRecover = 101u, kActHangStat = 102u;
constexpr bool is_g1_action(uint32_t a) { return a >= kActHangTest && a <= kActHangStat; }
// The boot-arg latch (navi48-g1): 0 unset, 1 latched OFF, 2 latched ON; the first writer wins, it is never re-read.
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }
// The user client's action bound and accelExperiment's: 100..102 exist only with the latch ON.
constexpr bool action_admitted(bool latchOn, uint32_t a) { return latchOn && is_g1_action(a); }

enum Mode : uint32_t { kModeWaitMem = 1u };
enum Method : uint32_t { kMethAuto = 0u, kMethRelease = 1u, kMethMesReset = 2u, kMethRemap = 3u, kMethAbandon = 4u, kMethCount = 5u };
constexpr uint32_t kMaxAttempts = 8u;       // per held hang (the status page has one page per attempt)
constexpr uint32_t kStatPages = 1u + kMaxAttempts;
// The legal (action, argument) pairs. On a native boot these are also the native-boot exemption (they touch only the GFX queue the native path
// owns, never Apple's driver state): see native_exempt.
constexpr bool args_ok(uint32_t a, uint64_t arg) {
    return (a == kActHangTest && arg == (uint64_t)kModeWaitMem) || (a == kActHangRecover && arg < (uint64_t)kMethCount) ||
           (a == kActHangStat && arg < (uint64_t)kStatPages);
}
constexpr bool native_exempt(bool latchOn, uint32_t a, uint64_t arg) { return action_admitted(latchOn, a) && args_ok(a, arg); }

// ---- codes (one number space, so the status page is unambiguous) --------------------------------------------------------------------------------
enum Code : uint32_t {
    // hangtest decisions
    kTestOk = 0u, kTestOff = 1u, kTestBadMode = 2u, kTestActive = 3u, kTestWsSession = 4u, kTestSessionOpen = 5u, kTestHung = 6u,
    // hangtest outcomes
    kTestHeld = 10u,          // the IB did not retire in 2 s: HUNG is latched and the test context holds the GPU
    kTestNotHung = 11u,       // the IB retired: no hang (the context was closed normally; nothing latched)
    kTestOpenRefused = 12u,   // n1c_open refused (not a native boot, S1b not POSITIVE, exclusivity, ring not idle, ...)
    kTestSetupFailed = 13u,   // the test BO / mapping could not be made (closed normally)
    kTestSubmitFailed = 14u,  // the ring refused the submit (closed normally, or leaked if that latched HUNG)
    // hangrecover decisions
    kRecOk = 0u, kRecOff = 1u, kRecBadMethod = 2u, kRecNoTest = 3u, kRecNotHung = 4u, kRecTooMany = 5u,
    kRecMesTimedOut = 6u,     // 0.0.626 (F3): a MES frame timed out earlier in this boot; every MES method is refused (only release and abandon remain)
    kRecRemapNeedsReset = 7u, // 0.0.626 (F2): `remap` alone is refused unless the most recent RESET on this hang was acknowledged (act rc 0)
    // hangrecover outcomes
    kRecRecovered = 20u,      // a method's probe VERIFIED: HUNG cleared, the test context closed normally (BO freed, CONTEXT8 parked)
    kRecAllFailed = 21u,      // no method verified: HUNG stays latched (today's behaviour), the test context keeps holding the GPU
    kRecAbandoned = 22u,      // method 4: the test context was closed with HUNG latched (memory leaked, as today); reboot to use the GPU
    // one attempt
    kAttVerified = 30u, kAttActFailed = 31u, kAttProbeRefused = 32u, kAttProbeTimeout = 33u, kAttProbeData = 34u
};

// ---- bounds (every wait in G1 is one of these) ---------------------------------------------------------------------------------------------------
constexpr uint64_t kMs = 1000000ull;
constexpr uint64_t kHangConfirmNs = 2000ull * kMs;   // = native_s1c_pure.h kHangNs: the same 2 s that latches HUNG for a client
constexpr uint64_t kVerifyNs      = 1000ull * kMs;   // a probe IB of one WRITE_DATA retires in microseconds when the queue is healthy
// The MES calls are bounded inside mes_submit_pkt: RESET 2 s, REMOVE_QUEUE 1 s, ADD_QUEUE 2 s (mes_v12_1.cpp). The longest act is remap (1 + 2 s).
constexpr uint64_t kActMaxNs      = 3000ull * kMs;
constexpr uint64_t kStepMaxNs     = kActMaxNs + kVerifyNs + 2000ull * kMs;   // + one ring-space wait (ring_emit's 2 s bound) for the probe
constexpr uint32_t kMaxPlan       = 2u;
constexpr uint32_t kMesTimeoutRc  = 0xe00002d6u;   // kIOReturnTimeout: mes_submit_pkt's answer when the MES did not acknowledge within its bound (0.0.626 F2/F3)
constexpr uint64_t kRecoverMaxNs  = (uint64_t)kMaxPlan * kStepMaxNs;
static_assert(kHangConfirmNs == 2000000000ull, "the hang is confirmed with the client's 2 s rule");
static_assert(kRecoverMaxNs <= 20ull * 1000 * kMs, "a whole hangrecover stays under 20 s");

// ---- the decisions ------------------------------------------------------------------------------------------------------------------------------
// hangtest. Inputs: the latch; the mode; testHolds = a G1 test context holds the native session now; sessionOpen = a native session is open (the CAS
// flag); sessionIsWs = that session was admitted by the uid-88 (WindowServer) rule; hung = the boot-wide HUNG latch. The ORDER matters: a WindowServer
// session is reported as such (never folded into "a session is open"), and it is checked before anything that could let the call proceed.
constexpr uint32_t test_decide(bool latchOn, uint32_t mode, bool testHolds, bool sessionOpen, bool sessionIsWs, bool hung) {
    if (!latchOn) return kTestOff;
    if (mode != kModeWaitMem) return kTestBadMode;
    if (sessionIsWs) return kTestWsSession;
    if (testHolds) return kTestActive;
    if (sessionOpen) return kTestSessionOpen;
    if (hung) return kTestHung;
    return kTestOk;
}
// hangrecover. testHolds = the test context holds the session; hung = the HUNG latch; attempts = attempts already made on this hang.
// 0.0.626: mesTimedOut = a MES frame has timed out (kIOReturnTimeout, 0xe00002d6) at any time in this boot: the firmware may still own the queue, so NO further
// MES frame is sent (uses_mes methods are refused with kRecMesTimedOut; release and abandon send none). resetAcked = the most recent MES RESET on this hang was
// acknowledged (act rc 0); a standalone `remap` needs it (upstream amdgpu_gfx_mes_reset_queue_start only reinitialises the queue after a reset it got an answer
// to). There is NO override verb: the refusal is final for that state.
constexpr bool uses_mes(uint32_t method) { return method == kMethAuto || method == kMethMesReset || method == kMethRemap; }
constexpr uint32_t recover_decide(bool latchOn, uint32_t method, bool testHolds, bool hung, uint32_t attempts, uint32_t planLen, bool mesTimedOut, bool resetAcked) {
    if (!latchOn) return kRecOff;
    if (method >= kMethCount) return kRecBadMethod;
    if (!testHolds) return kRecNoTest;
    if (!hung) return kRecNotHung;
    if (attempts + planLen > kMaxAttempts) return kRecTooMany;
    if (mesTimedOut && uses_mes(method)) return kRecMesTimedOut;
    if (method == kMethRemap && !resetAcked) return kRecRemapNeedsReset;
    return kRecOk;
}

// ---- the plan: least invasive first -------------------------------------------------------------------------------------------------------------
// release < mesreset < remap. `release` is not a GPU recovery (it ends the WAIT_REG_MEM the test built), so `auto` never includes it: auto is what a
// real hang (a client's IB) would get. mesreset touches only the one queue, inside the MES firmware (upstream gfx_v12_0_reset_kgq). remap also drops
// and rebuilds the queue's MQD and ring (upstream's reinit_queue leg of amdgpu_gfx_mes_reset_queue_start).
struct Plan { uint32_t n; uint32_t m[kMaxPlan]; };
constexpr Plan plan_for(uint32_t method) {
    return method == kMethAuto ? Plan{ 2u, { kMethMesReset, kMethRemap } }
         : (method == kMethRelease || method == kMethMesReset || method == kMethRemap) ? Plan{ 1u, { method, 0u } }
         : Plan{ 0u, { 0u, 0u } };
}
constexpr uint32_t invasiveness(uint32_t m) { return m == kMethRelease ? 1u : m == kMethMesReset ? 2u : m == kMethRemap ? 3u : 0u; }
static_assert(plan_for(kMethAuto).n == 2u && invasiveness(plan_for(kMethAuto).m[0]) < invasiveness(plan_for(kMethAuto).m[1]),
              "auto tries the less invasive method first");

// ---- the latch-clear rule -------------------------------------------------------------------------------------------------------------------------
// HUNG is cleared ONLY when THIS attempt's probe retired (its seqno landed: everything before it in the ring completed or was discarded) AND the
// probe's own WRITE_DATA, executed under VMID 8, reads back. Anything less leaves the latch set.
constexpr bool may_clear(uint32_t attStatus, bool retired, bool dataOk) { return attStatus == kAttVerified && retired && dataOk; }
constexpr uint32_t attempt_status(bool actOk, bool probeSubmitted, bool retired, bool dataOk) {
    return !actOk ? kAttActFailed : !probeSubmitted ? kAttProbeRefused : !retired ? kAttProbeTimeout : !dataOk ? kAttProbeData : kAttVerified;
}

// ---- ring helpers -------------------------------------------------------------------------------------------------------------------------------
// True when ring index `idx` holds dwords of the live (unretired) range [liveStart, liveEnd) of the 64-bit write counter. Everything else in the ring
// was consumed (before liveStart) or is beyond the write pointer (never fetched), so G1 may overwrite it with NOPs: if a reset rewinds the read
// pointer, only NOPs run there (no stale user-fence RELEASE_MEM into a freed BO). A live range longer than the ring is refused by the caller.
constexpr bool in_live_range(uint32_t idx, uint64_t liveStart, uint64_t liveEnd, uint32_t mask) {
    return liveEnd > liveStart && (uint64_t)((idx - (uint32_t)(liveStart & mask)) & mask) < liveEnd - liveStart;
}
// remove_queue_after_reset rides on the unmap only after an ACKED reset and only from MES sched version 0x5a (mes_v12_0.c:762-766).
constexpr bool unmap_after_reset(bool resetAcked, uint32_t schedVersionMasked) { return resetAcked && schedVersionMasked >= 0x5au; }

// ---- the test IB ----------------------------------------------------------------------------------------------------------------------------------
// WAIT_REG_MEM (PM4 opcode 0x3C, Mesa sid.h:90-95; gfx_v12_0_wait_reg_mem): FUNCTION 3 (equal) | MEM_SPACE(1) << 4 | OPERATION 0 (wait) | ENGINE 0 (ME),
// then address lo/hi (dword aligned), reference, mask, poll interval. The reference is never written by anything but `hangrecover 1`.
constexpr uint32_t kP3WaitRegMem = 0x3Cu;
constexpr uint32_t kWaitRefNever = 0x6100D0E5u;      // the value the poll dword (zero-filled) never holds until `release` writes it
constexpr uint32_t kWaitPollInterval = 0x20u;
constexpr uint32_t kWaitDw = 7u;
constexpr uint32_t p3(uint32_t op, uint32_t countMinus1) { return (3u << 30) | ((countMinus1 & 0x3FFFu) << 16) | ((op & 0xFFu) << 8); }
inline uint32_t emit_wait_never(uint32_t *o, uint64_t pollVa) {
    o[0] = p3(kP3WaitRegMem, 5u);
    o[1] = 3u | (1u << 4);                           // equal, memory, wait, ME
    o[2] = (uint32_t)(pollVa & 0xFFFFFFFCull);
    o[3] = (uint32_t)(pollVa >> 32);
    o[4] = kWaitRefNever;
    o[5] = 0xFFFFFFFFu;
    o[6] = kWaitPollInterval;
    return kWaitDw;
}
// The test BO: 4 pages of VRAM mapped R|W|X|UC at kTestVa under VMID 8 (UC: the CP's polls and the CPU's BAR0 writes meet in memory, not in GL2).
//   page 0 +0x000 the hang IB, +0x800 the probe IB; page 1 +0x000 the poll dword, +0x008 the probe dword.
constexpr uint64_t kTestVa = 0x200000ull, kTestBytes = 0x4000ull;
constexpr uint64_t kHangIbOff = 0x0ull, kProbeIbOff = 0x800ull, kPollOff = 0x1000ull, kProbeOff = 0x1008ull;
constexpr uint32_t probe_magic(uint32_t attemptIdx) { return 0x61000000u | (attemptIdx & 0xFFu); }

// ---- the bounded wait ---------------------------------------------------------------------------------------------------------------------------
// EVERY G1 wait is this. `e` supplies now_ns() and relax(elapsedNs); `pred` is polled. Returns pred's value at the end; never runs past boundNs
// (plus one relax step).
template <class E, class P>
inline bool wait_until(E &e, P pred, uint64_t boundNs) {
    const uint64_t t0 = e.now_ns();
    for (;;) {
        if (pred()) return true;
        const uint64_t el = e.now_ns() - t0;
        if (el >= boundNs) return pred();
        e.relax(el);
    }
}

// ---- the records ----------------------------------------------------------------------------------------------------------------------------------
struct ProbeSub { uint32_t rc; uint64_t seq; uint32_t magic; };
struct Attempt {
    uint32_t status, method, actRc, probeRc;
    uint64_t actNs, verifyNs, probeSeq;
    bool retired, dataOk;
};
struct RecoverRep { uint32_t outcome; uint32_t n; Attempt a[kMaxPlan]; bool cleared; };
struct TestRep { uint32_t outcome; uint32_t rc; uint64_t seq; };

// ---- hangtest -------------------------------------------------------------------------------------------------------------------------------------
// `e` supplies: uint32_t open_test(); uint32_t setup_bo(); ProbeSub submit_hang(); uint64_t retired(); void latch_hang(); void close_test(const char *);
// plus now_ns / relax. The caller has run test_decide (kTestOk). The latch is taken only after the bounded confirmation, never before.
template <class E>
inline TestRep hangtest_run(E &e) {
    TestRep t{ kTestOpenRefused, 0u, 0ull };
    t.rc = e.open_test();
    if (t.rc != 0u) return t;
    t.rc = e.setup_bo();
    if (t.rc != 0u) { e.close_test("g1 hangtest: setup failed"); t.outcome = kTestSetupFailed; return t; }
    const ProbeSub ps = e.submit_hang();
    if (ps.rc != 0u) { t.rc = ps.rc; e.close_test("g1 hangtest: submit refused"); t.outcome = kTestSubmitFailed; return t; }
    t.seq = ps.seq;
    if (wait_until(e, [&]() { return e.retired() >= ps.seq; }, kHangConfirmNs)) {
        e.close_test("g1 hangtest: the IB retired (no hang)");
        t.outcome = kTestNotHung;
        return t;
    }
    e.latch_hang();
    t.outcome = kTestHeld;
    return t;
}

// ---- hangrecover ----------------------------------------------------------------------------------------------------------------------------------
// `e` supplies: uint32_t act(uint32_t method, bool resetAcked) (0 = the method's own steps were acked); ProbeSub submit_probe(uint32_t attemptIdx);
// uint64_t retired(); uint32_t probe_data(); void clear_latch(); void close_test(const char *); void note(const Attempt &); plus now_ns / relax.
// `firstIdx` numbers this call's attempts after the ones already made. The caller has run recover_decide (kRecOk). The order per method is fixed:
// act, probe, bounded wait, read back, THEN (only when may_clear) clear the latch and close the context normally.
template <class E>
inline RecoverRep recover_run(E &e, uint32_t method, uint32_t firstIdx) {
    RecoverRep r{};
    r.outcome = kRecAllFailed;
    const Plan p = plan_for(method);
    bool resetAcked = false;
    for (uint32_t i = 0; i < p.n && i < kMaxPlan; i++) {
        Attempt &a = r.a[r.n++];
        a = Attempt{};
        a.method = p.m[i];
        const uint64_t t0 = e.now_ns();
        a.actRc = e.act(a.method, resetAcked);
        a.actNs = e.now_ns() - t0;
        if (a.method == kMethMesReset && a.actRc == 0u) resetAcked = true;
        if (a.actRc != 0u) {
            a.status = attempt_status(false, false, false, false); e.note(a);
            // 0.0.626 (F2): in auto mode a failed or unacknowledged RESET ENDS the run (no REMOVE_QUEUE / remap), as upstream amdgpu_gfx_mes_reset_queue_start returns on a
            // reset failure; (F3) a MES timeout in ANY step ends it too: the firmware may still own the queue, so no further MES frame is sent in this run.
            if ((method == kMethAuto && a.method == kMethMesReset) || a.actRc == kMesTimeoutRc) break;
            continue;
        }
        const uint64_t t1 = e.now_ns();
        const ProbeSub ps = e.submit_probe(firstIdx + i);
        a.probeRc = ps.rc; a.probeSeq = ps.seq;
        if (ps.rc == 0u) {
            a.retired = wait_until(e, [&]() { return e.retired() >= ps.seq; }, kVerifyNs);
            a.dataOk = a.retired && e.probe_data() == ps.magic;
        }
        a.verifyNs = e.now_ns() - t1;
        a.status = attempt_status(true, ps.rc == 0u, a.retired, a.dataOk);
        e.note(a);
        if (may_clear(a.status, a.retired, a.dataOk)) {
            e.clear_latch();
            r.cleared = true;
            r.outcome = kRecRecovered;
            e.close_test("g1 hangrecover: recovered");
            return r;
        }
    }
    return r;
}

// ---- the status page (13 scalars, the accel extra-scalar budget) --------------------------------------------------------------------------------
struct Summary {
    uint32_t lastCode, phase;            // phase 0 idle, 1 holding a hang
    bool hung, testHolds;
    uint64_t hungSeq, emitted, retired, wc;
    uint32_t attempts, lastMethod, tests, recovered, failedRuns;
};
constexpr uint32_t kOutN = 13u;
inline void summary_out(const Summary &s, uint64_t *o) {
    o[0] = s.lastCode; o[1] = s.phase; o[2] = s.hung ? 1u : 0u; o[3] = s.testHolds ? 1u : 0u; o[4] = s.hungSeq; o[5] = s.emitted; o[6] = s.retired;
    o[7] = s.wc; o[8] = s.attempts; o[9] = s.lastMethod; o[10] = s.tests; o[11] = s.recovered; o[12] = s.failedRuns;
}
inline void attempt_out(const Attempt &a, uint64_t rptrAfter, uint64_t cpStatAfter, uint64_t *o) {
    o[0] = a.status; o[1] = a.method; o[2] = a.actRc; o[3] = a.actNs / 1000ull; o[4] = a.probeRc; o[5] = a.probeSeq; o[6] = a.retired ? 1u : 0u;
    o[7] = a.dataOk ? 1u : 0u; o[8] = a.verifyNs / 1000ull; o[9] = rptrAfter; o[10] = cpStatAfter; o[11] = 0u; o[12] = 0u;
}

} // namespace g1
} // namespace n48native
