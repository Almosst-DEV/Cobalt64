// native_g1_test.cpp - kext 0.0.623 (GPU-apps stage G1: hang recovery): the pure half (amd/native_g1_pure.h) driven with a fake environment, plus
// source pins that tie the REAL kernel code (amd/native_s1c.cpp, Navi48Bringup.cpp, Navi48UserClient.cpp, Navi48NativeClient.cpp, mes_v12_1.cpp) to it.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_g1_test.cpp -o /tmp/native_g1 && /tmp/native_g1 .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_g1_plant.sh plants breaks in the real code and
//    demands that this suite FAILS on each)
// Covers:
//   G1  admission: the boot-arg latch, actions 100..102 only with the latch ON, the legal (action, argument) pairs, the native-boot exemption;
//   G2  hangtest refusals: OFF, bad mode, a WindowServer session (by NAME, before anything else that could proceed), a test already holding, any
//       session open, HUNG; admitted only with all clear;
//   G3  hangrecover refusals: OFF, bad method, no test context, not HUNG, the attempt budget;
//   G4  the plan: least invasive first, auto = mesreset then remap, release never in auto, abandon has no plan;
//   G5  the bounded wait: a predicate that never holds returns false within the bound (a runaway is detected, not hung on);
//   G6  hangtest_run: the latch is taken only after the 2 s confirmation; a retiring IB closes normally; every failure closes;
//   G7  recover_run: HUNG is cleared ONLY after this attempt's probe retired AND read back, and the context is closed AFTER the clear; act failure means
//       no probe; a wrong readback or a timeout never clears; resetAcked reaches remap; every run stays inside kRecoverMaxNs;
//   G8  helpers: the live-range test (wrap), remove_queue_after_reset gating, the WAIT_REG_MEM encoding, probe magics, the status pages;
//   G9  source pins (reachability and order) on the kernel code: the gate inputs, the single latch-clear site, the remap order, no register writes and no
//       unbounded loop in the G1 block, the WindowServer note, the dispatch before the native refusal, the MES frames, the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "native_g1_pure.h"

using namespace n48native::g1;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) {
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: %s\n", what); }
}
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) {
    std::ifstream f(path);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}
static size_t count_of(const std::string &s, const std::string &needle) {
    size_t n = 0, p = 0;
    while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); }
    return n;
}
// True when every needle is found, each after the previous one.
static bool in_order(const std::string &s, std::initializer_list<const char *> needles) {
    size_t p = 0;
    for (const char *n : needles) { const size_t q = s.find(n, p); if (q == std::string::npos) return false; p = q + std::strlen(n); }
    return true;
}
static std::string between(const std::string &s, const std::string &a, const std::string &b) {
    const size_t i = s.find(a);
    if (i == std::string::npos) return std::string();
    const size_t j = s.find(b, i + a.size());
    return j == std::string::npos ? std::string() : s.substr(i, j - i);
}

// ---- the fake environment -----------------------------------------------------------------------------------------------------------------------------
struct Ev { std::string what; uint64_t t; uint32_t a; };
struct Fake {
    uint64_t t = 1000;                 // ns
    uint64_t relaxes = 0;
    bool runaway = false;
    std::vector<Ev> ev;
    // the GPU model
    uint64_t emitted = 0, retiredSlot = 0;
    bool hungIb = true;                // the hang IB never retires unless released / reset
    // scripted per attempt (by the method order of the run)
    std::vector<uint32_t> actRc;       // one per act call, default 0
    std::vector<bool> probeRetires;    // one per probe, default true
    std::vector<bool> probeDataOk;     // one per probe, default true
    std::vector<uint32_t> probeRc;     // one per probe, default 0
    size_t nAct = 0, nProbe = 0;
    uint32_t lastMagic = 0;
    bool probeLanded = false, dataGood = false;
    uint64_t probeSeq = 0, probeAt = 0;
    // hangtest script
    uint32_t openRc = 0, setupRc = 0, submitRc = 0;
    bool hangRetiresAfter = false; uint64_t hangRetireAtNs = 0;
    std::vector<bool> resetAckedSeen;

    void log(const char *w, uint32_t a = 0) { ev.push_back(Ev{ w, t, a }); }
    uint64_t now_ns() { return t; }
    void relax(uint64_t el) {
        relaxes++;
        t += el < 5000000ull ? 1000ull : 1000000ull;
        if (relaxes > 2000000ull) { runaway = true; }
    }
    uint64_t retired() {
        if (runaway) return ~0ull;     // break a runaway loop so the suite can report it
        uint64_t r = retiredSlot;
        if (hangRetiresAfter && t >= hangRetireAtNs && r < 1) r = 1;
        if (probeLanded && t >= probeAt + 50000ull) r = probeSeq;
        return r > emitted ? emitted : r;
    }
    // hangtest
    uint32_t open_test() { log("open"); return openRc; }
    uint32_t setup_bo() { log("setup"); return setupRc; }
    ProbeSub submit_hang() { log("submit_hang"); if (submitRc) return ProbeSub{ submitRc, 0, 0 }; emitted = 1; return ProbeSub{ 0, 1, 0 }; }
    void latch_hang() { log("latch"); }
    void close_test(const char *) { log("close"); }
    // hangrecover
    uint32_t act(uint32_t m, bool resetAcked) {
        log("act", m);
        resetAckedSeen.push_back(resetAcked);
        const uint32_t rc = nAct < actRc.size() ? actRc[nAct] : 0u;
        nAct++;
        t += 100000ull;
        return rc;
    }
    ProbeSub submit_probe(uint32_t idx) {
        log("probe", idx);
        const size_t k = nProbe++;
        const uint32_t rc = k < probeRc.size() ? probeRc[k] : 0u;
        if (rc) return ProbeSub{ rc, 0, probe_magic(idx) };
        emitted++;
        probeSeq = emitted; probeAt = t;
        probeLanded = k < probeRetires.size() ? probeRetires[k] : true;
        dataGood = k < probeDataOk.size() ? probeDataOk[k] : true;
        lastMagic = probe_magic(idx);
        return ProbeSub{ 0, emitted, lastMagic };
    }
    uint32_t probe_data() { log("data"); return dataGood && probeLanded ? lastMagic : 0xBADu; }
    void clear_latch() { log("clear"); }
    void note(const Attempt &a) { log("note", a.status); }
    size_t idx(const char *w, size_t from = 0) const { for (size_t i = from; i < ev.size(); i++) if (ev[i].what == w) return i; return (size_t)-1; }
    size_t count(const char *w) const { size_t n = 0; for (const Ev &e : ev) if (e.what == w) n++; return n; }
};

// ---- G1 ---------------------------------------------------------------------------------------------------------------------------------------------
static void g1_admission() {
    expect_u("latch: absent -> OFF", latch_value(false, 1), kLatchOff);
    expect_u("latch: =0 -> OFF", latch_value(true, 0), kLatchOff);
    expect_u("latch: =2 -> OFF", latch_value(true, 2), kLatchOff);
    expect_u("latch: =1 -> ON", latch_value(true, 1), kLatchOn);
    expect(!latch_is_on(kLatchUnset) && !latch_is_on(kLatchOff) && latch_is_on(kLatchOn), "only the ON latch is on");
    expect(kActHangTest == 100u && kActHangRecover == 101u && kActHangStat == 102u, "the actions are 100, 101, 102");
    bool anyOff = false, wrongOn = false;
    for (uint32_t a = 0; a < 300; a++) {
        anyOff |= action_admitted(false, a);
        wrongOn |= action_admitted(true, a) != (a >= 100u && a <= 102u);
        for (uint64_t arg = 0; arg < 12; arg++) anyOff |= native_exempt(false, a, arg);
    }
    expect(!anyOff, "with the latch OFF no action and no (action, arg) pair is admitted or exempt");
    expect(!wrongOn, "with the latch ON exactly 100..102 are admitted");
    expect(args_ok(100, 1) && !args_ok(100, 0) && !args_ok(100, 2), "hangtest: mode 1 only");
    expect(args_ok(101, 0) && args_ok(101, 4) && !args_ok(101, 5), "hangrecover: methods 0..4");
    expect(args_ok(102, 0) && args_ok(102, 8) && !args_ok(102, 9), "hangstat: pages 0..8");
    expect(native_exempt(true, 101, 3) && !native_exempt(true, 101, 9) && !native_exempt(true, 99, 0) && !native_exempt(true, 103, 0), "the exemption is exactly the legal pairs");
}

// ---- G2 / G3 ------------------------------------------------------------------------------------------------------------------------------------------
static void g2_test_decide() {
    expect_u("ok with everything clear", test_decide(true, 1, false, false, false, false), kTestOk);
    expect_u("OFF wins", test_decide(false, 1, false, false, false, false), kTestOff);
    expect_u("bad mode", test_decide(true, 2, false, false, false, false), kTestBadMode);
    expect_u("a WindowServer session is refused BY NAME", test_decide(true, 1, false, true, true, false), kTestWsSession);
    expect_u("a WindowServer session is refused even when HUNG", test_decide(true, 1, false, true, true, true), kTestWsSession);
    expect_u("WindowServer reported even if the open flag reads stale", test_decide(true, 1, false, false, true, false), kTestWsSession);
    expect_u("a test context already holds", test_decide(true, 1, true, true, false, true), kTestActive);
    expect_u("an administrator session is refused", test_decide(true, 1, false, true, false, false), kTestSessionOpen);
    expect_u("HUNG is refused", test_decide(true, 1, false, false, false, true), kTestHung);
    uint32_t admitted = 0;
    for (int m = 0; m < 4; m++) for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) for (int c = 0; c < 2; c++) for (int d = 0; d < 2; d++) for (int l = 0; l < 2; l++)
        if (test_decide(l, (uint32_t)m, a, b, c, d) == kTestOk) { admitted++; expect(l && m == 1 && !a && !b && !c && !d, "admitted only with all clear"); }
    expect_u("exactly one input combination admits", admitted, 1);
}
static void g3_recover_decide() {
    expect_u("ok", recover_decide(true, 0, true, true, 0, 2, false, false), kRecOk);
    expect_u("OFF", recover_decide(false, 0, true, true, 0, 2, false, false), kRecOff);
    expect_u("bad method", recover_decide(true, 5, true, true, 0, 0, false, false), kRecBadMethod);
    expect_u("no test context (a real client's hang is not G1's)", recover_decide(true, 2, false, true, 0, 1, false, false), kRecNoTest);
    expect_u("not hung", recover_decide(true, 2, true, false, 0, 1, false, false), kRecNotHung);
    expect_u("budget: 7 made + 2 planned > 8", recover_decide(true, 0, true, true, 7, 2, false, false), kRecTooMany);
    expect_u("budget: 7 made + 1 planned fits", recover_decide(true, 2, true, true, 7, 1, false, false), kRecOk);
    expect_u("abandon is admitted at a full budget", recover_decide(true, 4, true, true, 8, plan_for(4).n, false, false), kRecOk);
    // 0.0.626 F3: after any MES timeout in this boot every MES method is refused with its own status; release and abandon remain
    for (uint32_t m : { kMethAuto, kMethMesReset, kMethRemap }) expect_u("F3: MES timed out earlier: every MES method is refused", recover_decide(true, m, true, true, 0, plan_for(m).n, true, true), kRecMesTimedOut);
    expect_u("F3: release is still allowed after a MES timeout", recover_decide(true, 1, true, true, 0, 1, true, true), kRecOk);
    expect_u("F3: abandon is still allowed after a MES timeout", recover_decide(true, 4, true, true, 8, plan_for(4).n, true, true), kRecOk);
    expect(kRecMesTimedOut != kRecRemapNeedsReset && kRecMesTimedOut != kRecTooMany && kRecMesTimedOut != kRecNotHung && kRecRemapNeedsReset != kRecTooMany, "F2/F3: the new refusals have distinct codes");
    // 0.0.626 F2: a standalone remap needs the most recent RESET acked (no override: recover_decide has no parameter that skips it)
    expect_u("F2: remap without an acked RESET is refused", recover_decide(true, 3, true, true, 0, 1, false, false), kRecRemapNeedsReset);
    expect_u("F2: remap after an acked RESET is allowed", recover_decide(true, 3, true, true, 0, 1, false, true), kRecOk);
    expect_u("F2: mesreset and auto do not need a prior RESET", recover_decide(true, 2, true, true, 0, 1, false, false) + recover_decide(true, 0, true, true, 0, 2, false, false), kRecOk);
    expect_u("F2: the older refusals keep their order (not hung before the remap rule)", recover_decide(true, 3, true, false, 0, 1, false, false), kRecNotHung);
}

// ---- G4 ------------------------------------------------------------------------------------------------------------------------------------------------
static void g4_plan() {
    const Plan a = plan_for(kMethAuto);
    expect(a.n == 2 && a.m[0] == kMethMesReset && a.m[1] == kMethRemap, "auto = mesreset, then remap");
    expect(invasiveness(kMethRelease) < invasiveness(kMethMesReset) && invasiveness(kMethMesReset) < invasiveness(kMethRemap), "release < mesreset < remap");
    for (uint32_t i = 0; i < a.n; i++) expect(a.m[i] != kMethRelease, "auto never includes release (a harness control, not a recovery)");
    expect(plan_for(kMethRelease).n == 1 && plan_for(kMethRelease).m[0] == kMethRelease, "release alone");
    expect(plan_for(kMethMesReset).n == 1 && plan_for(kMethRemap).n == 1, "single methods");
    expect(plan_for(kMethAbandon).n == 0 && plan_for(9).n == 0, "abandon / unknown: no plan");
}

// ---- G5 ------------------------------------------------------------------------------------------------------------------------------------------------
static void g5_wait() {
    {
        Fake f; const uint64_t t0 = f.t;
        const bool r = wait_until(f, [&]() { return f.runaway; }, kVerifyNs);   // never true unless the wait runs away (then it breaks the loop so the suite can say so)
        expect(!r, "a never-true predicate returns false");
        expect(!f.runaway, "the wait ended by itself (no runaway)");
        expect(f.t - t0 >= kVerifyNs && f.t - t0 <= kVerifyNs + 1000000ull, "it ran to the bound and not more than one step past it");
    }
    {
        Fake f; int n = 0;
        expect(wait_until(f, [&]() { return ++n >= 3; }, kVerifyNs) && n == 3, "a predicate that becomes true ends the wait at once");
    }
    {
        Fake f; const uint64_t t0 = f.t;
        expect(!wait_until(f, [&]() { return f.runaway; }, kHangConfirmNs) && !f.runaway && f.t - t0 >= kHangConfirmNs && f.t - t0 <= kHangConfirmNs + 1000000ull,
               "the 2 s hang confirmation is bounded");
    }
    {   // zero bound: one look, no relax
        Fake f;
        expect(!wait_until(f, [&]() { return f.runaway; }, 0) && f.relaxes == 0, "a zero bound looks once and never relaxes");
    }
}

// ---- G6 ------------------------------------------------------------------------------------------------------------------------------------------------
static void g6_hangtest() {
    {
        Fake f;
        const TestRep r = hangtest_run(f);
        expect_u("held: outcome", r.outcome, kTestHeld);
        expect(!f.runaway, "held: no runaway");
        const size_t s = f.idx("submit_hang"), l = f.idx("latch");
        expect(s != (size_t)-1 && l != (size_t)-1 && s < l, "held: the latch comes after the submit");
        expect(l != (size_t)-1 && f.ev[l].t - f.ev[s].t >= kHangConfirmNs, "held: the latch is taken only after the full 2 s confirmation");
        expect_u("held: latched once", f.count("latch"), 1);
        expect_u("held: the context is NOT closed (it holds the hang for hangrecover)", f.count("close"), 0);
    }
    {
        Fake f; f.hangRetiresAfter = true; f.hangRetireAtNs = 1000 + 300000000ull;
        const TestRep r = hangtest_run(f);
        expect_u("no hang: outcome", r.outcome, kTestNotHung);
        expect(f.count("latch") == 0 && f.count("close") == 1, "no hang: nothing latched, closed normally");
    }
    {
        Fake f; f.openRc = 0xe00002d8u;
        const TestRep r = hangtest_run(f);
        expect(r.outcome == kTestOpenRefused && r.rc == 0xe00002d8u && f.count("setup") == 0 && f.count("close") == 0, "open refused: nothing else runs, nothing to close");
    }
    {
        Fake f; f.setupRc = 0xe00002bdu;
        const TestRep r = hangtest_run(f);
        expect(r.outcome == kTestSetupFailed && f.count("submit_hang") == 0 && f.count("close") == 1 && f.count("latch") == 0, "setup failed: closed, no submit");
    }
    {
        Fake f; f.submitRc = 0xe00002d6u;
        const TestRep r = hangtest_run(f);
        expect(r.outcome == kTestSubmitFailed && f.count("close") == 1 && f.count("latch") == 0, "submit refused: closed, no latch by the run");
    }
}

// ---- G7 ------------------------------------------------------------------------------------------------------------------------------------------------
static bool clear_after_verified_probe(const Fake &f) {
    const size_t c = f.idx("clear");
    if (c == (size_t)-1) return false;
    // the last probe and its data read precede the clear, and the close follows it
    size_t p = (size_t)-1, d = (size_t)-1;
    for (size_t i = 0; i < c; i++) { if (f.ev[i].what == "probe") p = i; if (f.ev[i].what == "data") d = i; }
    const size_t cl = f.idx("close", c);
    return p != (size_t)-1 && d != (size_t)-1 && p < d && d < c && cl != (size_t)-1 && cl > c;
}
static void g7_recover() {
    {   // mesreset works
        Fake f; f.emitted = 1;
        const RecoverRep r = recover_run(f, kMethMesReset, 0);
        expect(r.outcome == kRecRecovered && r.cleared && r.n == 1 && r.a[0].status == kAttVerified, "mesreset verified: recovered");
        expect(clear_after_verified_probe(f), "mesreset: probe, data, THEN clear, THEN close");
        expect_u("mesreset: cleared once", f.count("clear"), 1);
        expect(f.ev[f.idx("probe")].t - f.ev[f.idx("act")].t >= 100000ull, "the probe follows the act");
    }
    {   // auto: mesreset acked but its probe times out; remap acked and verified
        Fake f; f.emitted = 1; f.probeRetires = { false, true };
        const RecoverRep r = recover_run(f, kMethAuto, 0);
        expect(r.outcome == kRecRecovered && r.n == 2, "auto: the second method recovered");
        expect(r.a[0].method == kMethMesReset && r.a[0].status == kAttProbeTimeout && r.a[1].method == kMethRemap && r.a[1].status == kAttVerified, "auto: mesreset timed out, remap verified");
        expect(f.resetAckedSeen.size() == 2 && !f.resetAckedSeen[0] && f.resetAckedSeen[1], "auto: remap is told the reset was acked");
        expect_u("auto: cleared once", f.count("clear"), 1);
        expect(clear_after_verified_probe(f), "auto: the clear follows the second probe's data read");
        expect(f.idx("clear") > f.idx("probe", f.idx("act", f.idx("act") + 1)), "auto: no clear between the first method and the second probe");
    }
    {   // all fail
        Fake f; f.emitted = 1; f.probeRetires = { false, false };
        const uint64_t t0 = f.t;
        const RecoverRep r = recover_run(f, kMethAuto, 3);
        expect(r.outcome == kRecAllFailed && !r.cleared && f.count("clear") == 0 && f.count("close") == 0, "all fail: HUNG stays, the context stays");
        expect(f.t - t0 <= kRecoverMaxNs && !f.runaway, "all fail: the whole run is inside kRecoverMaxNs");
        expect(f.ev[f.idx("probe")].a == 3 && f.ev[f.idx("probe", f.idx("probe") + 1)].a == 4, "attempt indices continue after the ones already made");
    }
    {   // retired but the data is wrong
        Fake f; f.emitted = 1; f.probeDataOk = { false };
        const RecoverRep r = recover_run(f, kMethMesReset, 0);
        expect(r.outcome == kRecAllFailed && r.a[0].status == kAttProbeData && f.count("clear") == 0, "a probe that retired with wrong data never clears");
    }
    {   // act not acked: no probe
        Fake f; f.emitted = 1; f.actRc = { 0xe00002d6u, 0xe00002d6u };
        const RecoverRep r = recover_run(f, kMethAuto, 0);
        expect(r.outcome == kRecAllFailed && f.count("probe") == 0 && r.a[0].status == kAttActFailed, "an unacked act submits no probe");
        expect(r.n == 1 && f.count("act") == 1 && f.resetAckedSeen.size() == 1, "F2: auto STOPS after a failed RESET: no REMOVE_QUEUE / remap act is made");
        expect(f.count("clear") == 0, "unacked acts never clear");
    }
    {   // F2: a failed RESET that is not a timeout also ends auto
        Fake f; f.emitted = 1; f.actRc = { 0xe00002bcu };
        const RecoverRep r = recover_run(f, kMethAuto, 0);
        expect(r.n == 1 && f.count("act") == 1 && f.count("probe") == 0 && r.outcome == kRecAllFailed, "F2: auto stops after any failed RESET, not only a timeout");
    }
    {   // F2: an ACKED reset whose probe fails still goes on to remap (the firmware answered)
        Fake f; f.emitted = 1; f.probeRetires = { false, true };
        const RecoverRep r = recover_run(f, kMethAuto, 0);
        expect(r.n == 2 && f.count("act") == 2, "F2: an acked RESET with a failing probe still reaches remap");
    }
    {   // F3: a MES timeout in a standalone method also ends the run at once (nothing after it)
        Fake f; f.emitted = 1; f.actRc = { kMesTimeoutRc };
        const RecoverRep r = recover_run(f, kMethRemap, 0);
        expect(r.n == 1 && f.count("probe") == 0 && f.count("act") == 1 && f.count("clear") == 0, "F3: a timed-out remap act ends the run, no probe, no further act");
    }
    {   // F2: standalone mesreset failing makes no second act either
        Fake f; f.emitted = 1; f.actRc = { 0xe00002d6u, 0u };
        const RecoverRep r = recover_run(f, kMethMesReset, 0);
        expect(r.n == 1 && f.count("act") == 1, "a standalone mesreset runs exactly one act");
    }
    {   // probe refused by the ring
        Fake f; f.emitted = 1; f.probeRc = { 0xe00002d6u };
        const RecoverRep r = recover_run(f, kMethRemap, 0);
        expect(r.a[0].status == kAttProbeRefused && f.count("clear") == 0 && f.count("data") == 0, "a refused probe never clears and is never read");
    }
    {   // release (the harness control)
        Fake f; f.emitted = 1;
        const RecoverRep r = recover_run(f, kMethRelease, 0);
        expect(r.outcome == kRecRecovered && f.ev[f.idx("act")].a == kMethRelease && clear_after_verified_probe(f), "release: verified the same way");
    }
    {   // abandon / unknown: the run does nothing
        Fake f;
        const RecoverRep r = recover_run(f, kMethAbandon, 0);
        expect(r.n == 0 && r.outcome == kRecAllFailed && f.ev.empty(), "abandon has no plan in recover_run (the kernel handles it by closing)");
    }
    expect(may_clear(kAttVerified, true, true) && !may_clear(kAttVerified, false, true) && !may_clear(kAttVerified, true, false) && !may_clear(kAttProbeData, true, true),
           "may_clear: verified AND retired AND data");
    expect_u("attempt_status: act", attempt_status(false, true, true, true), kAttActFailed);
    expect_u("attempt_status: refused", attempt_status(true, false, true, true), kAttProbeRefused);
    expect_u("attempt_status: timeout", attempt_status(true, true, false, true), kAttProbeTimeout);
    expect_u("attempt_status: data", attempt_status(true, true, true, false), kAttProbeData);
    expect_u("attempt_status: verified", attempt_status(true, true, true, true), kAttVerified);
}

// ---- G8 ------------------------------------------------------------------------------------------------------------------------------------------------
static void g8_helpers() {
    const uint32_t mask = 0x3FFFu;   // a 16 Ki-dword ring
    expect(in_live_range(100, 100, 120, mask) && in_live_range(119, 100, 120, mask) && !in_live_range(120, 100, 120, mask) && !in_live_range(99, 100, 120, mask), "live range: [start, end)");
    expect(in_live_range(0x3FFF, 0x3FFE, 0x4002, mask) && in_live_range(1, 0x3FFE, 0x4002, mask) && !in_live_range(2, 0x3FFE, 0x4002, mask) && !in_live_range(0x3FFD, 0x3FFE, 0x4002, mask), "live range: across the wrap");
    expect(!in_live_range(5, 7, 7, mask), "an empty live range protects nothing");
    expect(in_live_range(5, 0x10005, 0x10006, mask), "the 64-bit counter is reduced by the mask");
    expect(unmap_after_reset(true, 0x5a) && !unmap_after_reset(true, 0x59) && !unmap_after_reset(false, 0x80), "remove_queue_after_reset: acked reset AND sched >= 0x5a");
    uint32_t ib[8] = { 0 };
    expect_u("WAIT_REG_MEM: 7 dwords", emit_wait_never(ib, 0x201000ull), 7);
    expect_u("WAIT_REG_MEM header (PACKET3 0x3C, count 5)", ib[0], 0xC0053C00u);
    expect_u("WAIT_REG_MEM: equal | memory | wait | ME", ib[1], 0x13u);
    expect(ib[2] == 0x201000u && ib[3] == 0u && ib[4] == kWaitRefNever && ib[5] == 0xFFFFFFFFu && ib[6] == kWaitPollInterval, "WAIT_REG_MEM: addr, ref, mask, interval");
    expect(kWaitRefNever != 0u, "the reference is not the zero the poll dword is filled with");
    expect(kTestVa == 0x200000ull && kTestBytes == 0x4000ull && kHangIbOff < kProbeIbOff && kProbeIbOff + 20u <= 0x1000u && kPollOff == 0x1000ull && kProbeOff == 0x1008ull,
           "the test BO layout: two IBs in page 0, poll and probe dwords in page 1");
    expect(probe_magic(0) != probe_magic(1) && probe_magic(7) != 0u, "each attempt has its own magic");
    uint64_t o[kOutN];
    Summary s{ 21u, 1u, true, true, 5u, 6u, 4u, 99u, 2u, 0u, 3u, 1u, 2u };
    summary_out(s, o);
    expect(o[0] == 21 && o[1] == 1 && o[2] == 1 && o[3] == 1 && o[4] == 5 && o[5] == 6 && o[6] == 4 && o[7] == 99 && o[8] == 2 && o[9] == 0 && o[10] == 3 && o[11] == 1 && o[12] == 2, "summary page");
    Attempt a{ kAttProbeTimeout, kMethRemap, 0u, 0u, 2000000ull, 1000000000ull, 9ull, false, false };
    attempt_out(a, 7u, 0x80000000u, o);
    expect(o[0] == kAttProbeTimeout && o[1] == kMethRemap && o[3] == 2000 && o[5] == 9 && o[6] == 0 && o[8] == 1000000 && o[9] == 7 && o[10] == 0x80000000u, "attempt page");
    expect(kOutN == 13u, "13 scalars: the accel extra budget");
}

// ---- G9: source pins --------------------------------------------------------------------------------------------------------------------------------
static void g9_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), hdr = slurp(K + "src/amd/native_s1c.h"), brg = slurp(K + "src/Navi48Bringup.cpp"),
                      ucl = slurp(K + "src/Navi48UserClient.cpp"), ncl = slurp(K + "src/Navi48NativeClient.cpp"), mes = slurp(K + "src/amd/mes_v12_1.cpp"),
                      mesh = slurp(K + "src/amd/amdgpu_mes.h"), plist = slurp(K + "Info.plist"), pure = slurp(K + "src/amd/native_g1_pure.h");
    expect(!eng.empty() && !brg.empty() && !ucl.empty() && !ncl.empty() && !mes.empty() && !mesh.empty() && !plist.empty() && !pure.empty(), "the sources were read");
    const std::string g1 = between(eng, "// ==== 0.0.623: GPU-apps stage G1", "\n} // namespace amdgpu");
    expect(!g1.empty(), "the G1 block exists in native_s1c.cpp");
    const std::string verb = between(g1, "IOReturn n1c_g1_verb(", "\n}\n");
    expect(!verb.empty(), "n1c_g1_verb exists");
    // the boot-arg
    expect(g1.find("PE_parse_boot_argn(\"navi48-g1\", &v, sizeof(v))") != std::string::npos, "the switch is boot-arg navi48-g1, latched once");
    // hangtest: the gate inputs come from the real state and the run is reached only after an OK decision
    expect(verb.find("const bool open = __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) != 0u;") != std::string::npos &&
           verb.find("const bool ws = open && __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;") != std::string::npos,
           "hangtest's inputs: the open flag and the WindowServer note");
    expect(verb.find("test_decide(n1c_g1_latched_on(), (uint32_t)arg, gG1.holds, open, ws, hung_now())") != std::string::npos, "hangtest decides with the latch, the holds flag, open, ws and HUNG");
    expect(in_order(verb, { "test_decide(", "if (d != kTestOk) {", "} else {", "hangtest_run(e)" }), "hangtest_run is reached only in the OK branch");
    expect_u("hangtest_run is called once", count_of(eng, "hangtest_run("), 1);
    expect(in_order(verb, { "recover_decide(", "if (d != kRecOk) {", "} else if ((uint32_t)arg == kMethAbandon) {", "} else {", "recover_run(e, (uint32_t)arg, gG1.attempts)" }),
           "recover_run is reached only after an OK decision, abandon separately");
    expect_u("recover_run is called once", count_of(eng, "recover_run("), 1);
    // the single latch-clear site
    expect_u("HUNG is set false in exactly one place in native_s1c.cpp", count_of(eng, "gHang.hung = false"), 1);
    const std::string clr = between(g1, "    void clear_latch() {", "\n    }\n");
    expect(clr.find("gHang.hung = false;") != std::string::npos, "that place is G1Env::clear_latch");
    expect_u("clear_latch is only DEFINED in the kernel (the pure recover_run is its only caller)", count_of(eng, "clear_latch("), 1);
    expect_u("the pure header calls clear_latch once", count_of(pure, "e.clear_latch();"), 1);
    expect(in_order(pure, { "a.retired = wait_until(", "a.dataOk = a.retired && e.probe_data() == ps.magic;", "if (may_clear(a.status, a.retired, a.dataOk)) {", "e.clear_latch();", "e.close_test(\"g1 hangrecover: recovered\");" }),
           "recover_run: wait, read back, may_clear, clear, close - in that order");
    expect(in_order(pure, { "if (wait_until(e, [&]() { return e.retired() >= ps.seq; }, kHangConfirmNs)) {", "e.latch_hang();" }), "hangtest_run: the latch only after the bounded confirmation");
    // the hang is latched through the client path
    const std::string lh = between(g1, "    void latch_hang() {", "\n    }\n");
    expect(lh.find("hang_from_wait();") != std::string::npos, "latch_hang uses hang_from_wait (the client path, which announces Navi48,Ready = 0)");
    expect_u("the G1 block touches gHang only in clear_latch (3 stores): the latch is SET only through hang_from_wait", count_of(g1, "gHang."), 3);
    expect_u("clear_latch holds those 3", count_of(clr, "gHang."), 3);
    expect_u("nothing in native_s1c.cpp stores hung = true directly (the pure hang_detect / hang_latch_wait do)", count_of(eng, "hung = true"), 0);
    // remap: the unmap must ack before the ring and MQD are re-initialised and the queue re-added
    const std::string act = between(g1, "    uint32_t act(uint32_t method, bool resetAcked) {", "\n    }\n");
    expect(in_order(act, { "mes_remove_hw_queue_flags(dev, mes, kMESQueueType_GFX, 0u, 0u, cp.doorbell_index, fl);", "if (r != kIOReturnSuccess) return (uint32_t)r;",
                           "ring[i] = cp_p3_nop1();", "*cp.rptr_cpu = 0u;", "gWc = 0ull; cp.wptr = 0u; gG1.liveStart = 0ull;", "r = cp_gfx_mqd_init(dev, cp);",
                           "if (r == kIOReturnSuccess) r = cp_map_gfx_kgq_mes(dev, cp, mes);" }),
           "remap: unmap acked, THEN ring/rptr/wptr/counters, THEN the MQD, THEN ADD_QUEUE");
    expect(in_order(act, { "if (method == kMethMesReset) {", "g1_scrub_ring();", "mes_reset_legacy_gfx_queue(dev, mes, 0u, 0u, kNativeVmid, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);" }),
           "mesreset: the ring outside the live range is scrubbed before the RESET");
    // containment: no register write by the block itself (its callees write the doorbells, GRBM_GFX_CNTL, TLB-invalidate, CONTEXT8 and HDP registers: see the native_s1c.cpp header), no loop without a bound, no reset of anything but the one queue, nothing of DCN / PSP / SMU
    expect_u("the G1 block writes no register (WREG32)", count_of(g1, "WREG32"), 0);
    expect_u("the G1 block has no for (;;)", count_of(g1, "for (;;)"), 0);
    expect_u("the G1 block's only while is do { } while (false)", count_of(g1, "while ("), count_of(g1, "} while (false);"));
    for (const char *bad : { "GRBM_SOFT_RESET", "SOFT_RESET", "psp_", "smu_", "dcn41_", "n48dcn::", "mode1", "CP_VMID_RESET", "IOSleep(1000" })
        expect(g1.find(bad) == std::string::npos, "the G1 block names no GRBM soft reset / PSP / SMU / DCN / VMID reset");
    expect_u("every G1 wait is the pure bounded wait (the kernel has none of its own)", count_of(g1, "wait_until("), 0);
    // the WindowServer note
    // 0.0.627 (G2): the note is set by the open itself, from the client's WindowServer-session flag, under gCliLock with the open; the close clears it before the open count drops
    expect(ncl.find("wsSession = d.reason == n48native::policy::kReasonWindowServer;") != std::string::npos && in_order(ncl, { "amdgpu::n1c_open(*owner->bringupContext(), uc->wsSession, &ref);", "uc->opened = true;", "if (!uc->attach(attachTo))" }) &&
           ncl.find("n1c_note_ws_session") == std::string::npos, "the native client opens with its WindowServer flag (the note is part of the open)");
    { const std::string op = between(eng, "IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {", "\n}\n");
      expect(in_order(op, { "IOLockLock(gCliLock);", "__atomic_store_n(&s->open, true, __ATOMIC_RELEASE);", "if (ws) __atomic_store_n(&gSessWs, 1u, __ATOMIC_SEQ_CST);", "IOLockUnlock(gCliLock);" }), "n1c_open sets the note for a WindowServer open, under the client lock"); }
    expect(in_order(eng, { "if (s->ws) __atomic_store_n(&gSessWs, 0u, __ATOMIC_SEQ_CST);", "if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag);\n    else __atomic_sub_fetch(&gOpenFlag, 1u, __ATOMIC_SEQ_CST);\n    IOLockUnlock(gCliLock);\n    IOLockUnlock(sl);" }), "n1c_close clears the note before the open flag (count) drops");
    // dispatch: before the native refusal, behind native_exempt with the latch; the user client bound
    expect(in_order(brg, { "if (n48native::g1::is_g1_action(action)) {", "if (!n48native::g1::native_exempt(amdgpu::n1c_g1_latched_on(), action, argScalar)) return kIOReturnBadArgument;",
                           "amdgpu::n1c_g1_verb(action, argScalar, gBringup, v);", "if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;" }),
           "accelExperiment: the G1 dispatch is gated by native_exempt and sits before the native refusal");
    expect(in_order(ucl, { "if (!n48native::g1::action_admitted(amdgpu::n1c_g1_latched_on(), action) && !n48native::g2::action_admitted(amdgpu::n1c_multi_latched_on(), action)) {",
                           "if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;", "}" }),
           "the user client: only latched G1 actions (and 0.0.627's latched G2 stat verb) skip the old bound");
    expect(ucl.find("clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) != kIOReturnSuccess") != std::string::npos, "the accel client stays administrator-only");
    // the MES frames
    expect(mes.find("return mes_remove_hw_queue_flags(dev, mes, queue_type, pipe_id, queue_id, doorbell_offset,\n                                     kRemoveQueueFlag_unmap_legacy_queue);") != std::string::npos,
           "mes_remove_hw_queue still sends unmap_legacy_queue (the teardown frame is unchanged)");
    const std::string rst = between(mes, "mes_reset_legacy_gfx_queue(const DeviceContext &dev, MESContext &mes,", "\n}\n");
    expect(rst.find("MESSchOp::RESET,") != std::string::npos && rst.find("pkt.flags              = kResetFlag_reset_legacy_gfx;") != std::string::npos &&
           rst.find("pkt.queue_type         = kMESQueueType_GFX;") != std::string::npos && rst.find("/*timeout_us=*/2000000);") != std::string::npos &&
           rst.find("WREG32") == std::string::npos, "the RESET frame: opcode RESET, reset_legacy_gfx, GFX, 2 s bound, no register write");
    expect(mesh.find("constexpr uint32_t RESET                     = 8;") != std::string::npos && mesh.find("constexpr uint32_t kResetFlag_reset_legacy_gfx = 1u << 3;") != std::string::npos,
           "RESET is opcode 8 and reset_legacy_gfx is bit 3 (mes_v12_api_def.h)");
    // 0.0.626 F1: mes_submit_pkt waits for THIS frame's own, incrementing fence value
    { const std::string sp = between(mes, "mes_submit_pkt(const DeviceContext &dev, MESContext &mes, MESPipe pipe,", "mes_query_sched_status(const DeviceContext");
      expect(!sp.empty(), "F1: mes_submit_pkt found");
      expect(sp.find("const uint64_t fence_value = 1;") == std::string::npos && sp.find("fence_value = 1;") == std::string::npos, "F1: the constant fence value 1 is gone");
      expect(sp.find("__atomic_add_fetch(&gMesFenceSeq, 1u, __ATOMIC_SEQ_CST) & 0x7FFFFFFFu; } while (fence_seq == 0u);") != std::string::npos && sp.find("const uint64_t fence_value = fence_seq;") != std::string::npos,
             "F1: the fence value is an incrementing per-call sequence in 1..0x7fffffff (never 0, never bit 31)");
      expect(in_order(sp, { "fence_seq", "st->fence_value = fence_value;", "qst->fence_value = fence_value;", "if ((v & 0xFFFFFFFFull) == fence_value) {" }), "F1: both frames carry the value and the wait compares against THAT value");
      expect(sp.find("== 1)") == std::string::npos && sp.find("== 1u)") == std::string::npos, "F1: nothing in the wait compares against a literal 1");
      expect_u("F1: the sequence is bumped in exactly one place", count_of(mes, "__atomic_add_fetch(&gMesFenceSeq"), 1);
      expect(in_order(sp, { "return kIOReturnTimeout;" }) && sp.rfind("__atomic_store_n(&gMesTimeoutSeen, 1u, __ATOMIC_SEQ_CST);") < sp.rfind("return kIOReturnTimeout;") && sp.find("__atomic_store_n(&gMesTimeoutSeen") != std::string::npos,
             "F3: the timeout path sets the sticky flag before it returns");
      expect(mes.find("bool mes_timeout_seen() { return __atomic_load_n(&gMesTimeoutSeen, __ATOMIC_SEQ_CST) != 0u; }") != std::string::npos && mesh.find("bool mes_timeout_seen();") != std::string::npos, "F3: the flag is readable through mes_timeout_seen()");
      expect_u("F3: the flag is set in exactly one place", count_of(mes, "__atomic_store_n(&gMesTimeoutSeen, 1u"), 1); }
    // 0.0.626 F2 / F3: the kernel glue feeds the decision from the real state
    expect(verb.find("recover_decide(n1c_g1_latched_on(), (uint32_t)arg, gG1.holds, hung_now(), gG1.attempts, p.n, mes_timeout_seen(), gG1.resetAcked)") != std::string::npos, "F2/F3: recover_decide gets mes_timeout_seen() and the most recent RESET's verdict");
    expect(g1.find("gG1.resetAcked = (r == kIOReturnSuccess);") != std::string::npos && g1.find("gG1.resetAcked = true;") == std::string::npos, "F2: resetAcked is the MOST RECENT RESET's verdict (never sticky-true)");
    expect(pure.find("if ((method == kMethAuto && a.method == kMethMesReset) || a.actRc == kMesTimeoutRc) break;") != std::string::npos, "F2/F3: recover_run ends auto after a failed RESET and any run after a MES timeout");
    // 0.0.626: the register-write claim of 0.0.623 was wrong and is corrected where it was made
    expect(eng.find("No MMIO register write.") == std::string::npos && eng.find("\"No MMIO register write\", which was wrong") != std::string::npos, "the false 'No MMIO register write' comment is corrected");
    // the version
    expect(count_of(plist, "<string>0.0.664</string>") == 2 && plist.find("0.0.625") == std::string::npos, "Info.plist is 0.0.664, carried twice");
    expect(hdr.find("constexpr uint32_t kN1cKextBuild = 664;") != std::string::npos, "the native client reports build 660");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    g1_admission();
    g2_test_decide();
    g3_recover_decide();
    g4_plan();
    g5_wait();
    g6_hangtest();
    g7_recover();
    g8_helpers();
    g9_pins(root);
    std::printf("native_g1: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
