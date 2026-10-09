// native_g5_test.cpp - kext 0.0.650 (GPU-apps stage G5 Stage 1: the application credit; an internal design note "G5 design" and "G5 Stage 1 build spec"): the pure half
// (amd/native_g5_pure.h) driven directly and through a simulation of the ring (WindowServer every 10.7 ms, three apps with 50 ms jobs, the GPU retiring in FIFO order) that calls
// the very function the kernel calls (g5::admission), plus source pins that tie the REAL kernel code (amd/native_s1c.cpp, Navi48Bringup.cpp, tools/pc/navi48test.c) to it.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_g5_test.cpp -o /tmp/native_g5 && /tmp/native_g5 .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_g5_plant.sh plants breaks in the real code and demands that
//    this suite FAILS on each)
// Covers:
//   T1  the switch (effective only with multisession), K (1..4, default 1, clamped), the stat page's admission (page 5 only with G5 ON; the G2 pages unchanged);
//   T2  ws_active: 0, the 100 ms boundary, a clock that goes backwards, WindowServer closed;
//   T3  app_in_flight: kernel entries skipped, WindowServer skipped (by session id, whatever its slot), an overwritten entry counted (fail closed), depth over kOwnerN = cap;
//   T4  older_waiter: ties to the lower slot, a slot that is not waiting yet counts as newest, stale and future entries ignored (a leaked entry heals itself);
//   T5  admit: the exhaustive truth table against an independent reference, and the ORDER of the checks;
//   T6  WindowServer on slot 1 (slot 0 DEAD or an app) is never gated (admission composite);
//   T7  the wait loop's decision (wait_step), the counters, the stat page layout;
//   T8  the ORDERING simulation: app jobs ahead of every WindowServer emission <= K, WindowServer never Waits, first-come fairness, no starvation, throughput kept,
//       the ring reserve, WindowServer silent = no throttling, WindowServer on slot 1;
//   T9  OFF identity: with the switch off (on=false) the simulation equals a model with no gate; the kernel's OFF path is the 0.0.642 text;
//   T10 source pins (order, reachability, lock discipline) on the kernel code, the CLI, the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <deque>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "native_g5_pure.h"

using namespace n48native;
using namespace n48native::g5;

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
static bool in_order(const std::string &s, std::initializer_list<const char *> needles) {
    size_t p = 0;
    for (const char *n : needles) { const size_t q = s.find(n, p); if (q == std::string::npos) return false; p = q + std::strlen(n); }
    return true;
}
static std::string fn_body(const std::string &s, const char *head) {
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return b == std::string::npos ? std::string() : s.substr(a, b - a);
}

// ---- T1 ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t1_switch() {
    expect(on_effective(true, true), "T1: G5 ON needs both latches");
    expect(!on_effective(true, false), "T1: navi48-g5=1 without navi48-multisession=1 is OFF");
    expect(!on_effective(false, true) && !on_effective(false, false), "T1: no navi48-g5 = OFF");
    expect(g2::latch_value(true, 1u) == g2::kLatchOn && g2::latch_value(true, 2u) == g2::kLatchOff && g2::latch_value(false, 1u) == g2::kLatchOff, "T1: the latch value rule is G2's (=1 only)");
    expect_u("T1: K absent = 1", credit_value(false, 3u), 1u);
    expect_u("T1: K 1", credit_value(true, 1u), 1u);
    expect_u("T1: K 2", credit_value(true, 2u), 2u);
    expect_u("T1: K 3", credit_value(true, 3u), 3u);
    expect_u("T1: K 4", credit_value(true, 4u), 4u);
    expect_u("T1: K 0 clamps to 1", credit_value(true, 0u), 1u);
    expect_u("T1: K 5 clamps to 1", credit_value(true, 5u), 1u);
    expect_u("T1: K huge clamps to 1", credit_value(true, 0xFFFFFFFFu), 1u);
    // the stat page: 5 exists only with G5 ON; the G2 pages are untouched
    expect(args_ok(true, 103u, 5ull), "T1: sessstat 5 with G5 ON");
    expect(!args_ok(false, 103u, 5ull), "T1: sessstat 5 with G5 OFF is BadArgument as before");
    expect(!args_ok(true, 103u, 4ull) && !args_ok(true, 103u, 6ull) && !args_ok(true, 103u, 0ull), "T1: only page 5 is G5's");
    expect(!args_ok(true, 104u, 5ull) && !args_ok(true, 102u, 5ull), "T1: only action 103");
    expect(!g2::native_exempt(true, 103u, 5ull) && g2::native_exempt(true, 103u, 4ull) && g2::native_exempt(true, 103u, 0ull), "T1: G2's own admission is unchanged (page 5 is not G2's)");
    expect_u("T1: kStatPage", kStatPage, 5u);
}

// ---- T2 ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t2_ws_active() {
    const uint64_t W = kWsWindowNs;
    expect_u("T2: the window is 100 ms", W, 100000000ull);
    expect(!ws_active(true, 0ull, 1000ull, W), "T2: never submitted = not active");
    expect(ws_active(true, 1000ull, 1000ull, W), "T2: submitted this very instant = active");
    expect(ws_active(true, 1000ull, 1000ull + W - 1ull, W), "T2: one ns inside the window = active");
    expect(!ws_active(true, 1000ull, 1000ull + W, W), "T2: exactly at the window = not active");
    expect(!ws_active(true, 1000ull, 1000ull + W + 1ull, W), "T2: past the window = not active");
    expect(!ws_active(true, 5000ull, 4999ull, W), "T2: a clock that goes backwards = not active");
    expect(!ws_active(false, 1000ull, 1001ull, W), "T2: no WindowServer session open = not active");
    expect(ws_active(true, 1ull, W, W), "T2: first ns of the clock");
}

// ---- T3 ---------------------------------------------------------------------------------------------------------------------------------------------------------
static g2::OwnerLog gLog;
static void log_reset() { std::memset(&gLog, 0, sizeof(gLog)); }
static void note(uint64_t seq, uint8_t slot, uint32_t id) { g2::owner_note(gLog, seq, slot, id, 0ull); }
static void t3_in_flight() {
    log_reset();
    for (uint64_t q = 1; q <= 10; q++) note(q, 1u, 20u);   // ten app jobs of session 20
    expect_u("T3: nothing outstanding", app_in_flight(gLog, 10, 10, 0, 4), 0);
    expect_u("T3: one outstanding", app_in_flight(gLog, 9, 10, 0, 4), 1);
    expect_u("T3: three outstanding", app_in_flight(gLog, 7, 10, 0, 4), 3);
    expect_u("T3: stops at the cap", app_in_flight(gLog, 0, 10, 0, 4), 4);
    expect_u("T3: cap 1", app_in_flight(gLog, 0, 10, 0, 1), 1);
    expect_u("T3: cap 0 counts nothing", app_in_flight(gLog, 0, 10, 0, 0), 0);
    expect_u("T3: retired above emitted counts nothing", app_in_flight(gLog, 12, 10, 0, 4), 0);
    // kernel probes are skipped
    log_reset(); note(1, 0u, 5u); note(2, g2::kOwnerKernel, 0u); note(3, 1u, 20u);
    expect_u("T3: a kernel probe is not an app job", app_in_flight(gLog, 0, 3, 5u, 8), 1);
    expect_u("T3: ... with no WindowServer id either", app_in_flight(gLog, 0, 3, 0u, 8), 2);
    // WindowServer is skipped by session id, whatever slot it has
    log_reset(); note(1, 1u, 5u); note(2, 0u, 20u); note(3, 1u, 5u); note(4, 2u, 21u);
    expect_u("T3: WindowServer on slot 1 (id 5) is skipped, the apps on slots 0 and 2 count", app_in_flight(gLog, 0, 4, 5u, 8), 2);
    expect_u("T3: with WindowServer on slot 0 (id 20) the others count", app_in_flight(gLog, 0, 4, 20u, 8), 3);
    expect_u("T3: no WindowServer open: everybody counts", app_in_flight(gLog, 0, 4, 0u, 8), 4);
    // an overwritten / never written entry counts as an app job (fail closed)
    log_reset(); note(1, 1u, 20u); note(2, 1u, 20u); note(3, 1u, 20u);
    gLog.e[2 % g2::kOwnerN].seq = 2 + g2::kOwnerN;   // seqno 2's slot now carries a newer seqno
    expect_u("T3: an entry that does not carry its seqno counts as an app job", app_in_flight(gLog, 0, 3, 99u, 8), 3);
    log_reset(); note(1, 0u, 5u); gLog.e[1].seq = 77;   // a WindowServer seqno whose entry was overwritten
    expect_u("T3: ... even where the owner would have been WindowServer", app_in_flight(gLog, 0, 1, 5u, 8), 1);
    log_reset();
    expect_u("T3: an entry never written (seq 0) counts", app_in_flight(gLog, 0, 2, 5u, 8), 2);
    // depth over kOwnerN = cap
    log_reset();
    expect_u("T3: depth over kOwnerN returns the cap", app_in_flight(gLog, 0, g2::kOwnerN + 1ull, 5u, 3), 3);
    for (uint64_t q = 1; q <= g2::kOwnerN; q++) note(q, 0u, 5u);   // all WindowServer
    expect_u("T3: depth exactly kOwnerN is scanned (all WindowServer = 0)", app_in_flight(gLog, 0, g2::kOwnerN, 5u, 3), 0);
    expect_u("T3: depth kOwnerN + 1 is fail-closed even if every entry were WindowServer's", app_in_flight(gLog, 0, g2::kOwnerN + 1ull, 5u, 3), 3);
    // WindowServer's own count
    log_reset(); note(1, 0u, 5u); note(2, 1u, 20u); note(3, 0u, 5u); note(4, g2::kOwnerKernel, 0u);
    expect_u("T3: WindowServer seqnos outstanding", ws_outstanding(gLog, 0, 4, 5u), 2);
    expect_u("T3: ... only the unretired ones", ws_outstanding(gLog, 1, 4, 5u), 1);
    expect_u("T3: ... none when no WindowServer is open", ws_outstanding(gLog, 0, 4, 0u), 0);
    expect_u("T3: ... a wrapped log reports kOwnerN", ws_outstanding(gLog, 0, g2::kOwnerN + 5ull, 5u), g2::kOwnerN);
}

// ---- T4 ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t4_older() {
    const uint64_t S = kSinceStaleNs, now = 100ull * S;
    uint64_t sc[kMaxSessions] = { 0, 0, 0, 0 };
    expect(!older_waiter(sc, 2, now, S), "T4: nobody waits");
    sc[1] = now - 10; expect(older_waiter(sc, 2, now, S), "T4: a slot that is not waiting yet is the newest: an older waiter exists");
    sc[2] = now - 5; expect(older_waiter(sc, 2, now, S), "T4: another slot waits longer");
    sc[2] = now - 20; expect(!older_waiter(sc, 2, now, S), "T4: I wait longer than the other");
    uint64_t sc2[kMaxSessions] = { 0, 0, 0, 0 };
    sc2[1] = now - 10; sc2[2] = now - 10;
    expect(!older_waiter(sc2, 1, now, S), "T4: tie, I am the lower slot: nobody is older");
    expect(older_waiter(sc2, 2, now, S), "T4: tie, I am the higher slot: the lower slot goes first");
    // stale and future entries are ignored
    uint64_t sc3[kMaxSessions] = { 1, 0, 0, 0 };   // slot 0 began waiting at ns 1, "now" is 100 stale periods later: a leaked entry
    expect(!older_waiter(sc3, 2, now, S), "T4: a leaked entry older than the stale bound is ignored (self-healing)");
    sc3[0] = now - S; expect(older_waiter(sc3, 2, now, S), "T4: exactly at the stale bound it still counts");
    sc3[0] = now - S - 1ull; expect(!older_waiter(sc3, 2, now, S), "T4: one ns past the stale bound it is ignored");
    sc3[0] = now + 5; expect(!older_waiter(sc3, 2, now, S), "T4: an entry in the future (clock went backwards) is ignored");
    uint64_t sc4[kMaxSessions] = { 0, now + 5, now + 10, 0 };   // both in the future: the other one would be the older of the two if it were honoured
    expect(!older_waiter(sc4, 2, now, S), "T4: a future entry is ignored even when this slot's own entry is later still");
    // helpers
    uint64_t h[kMaxSessions] = { 0, 0, 0, 0 };
    since_mark(h, 1, 50); since_mark(h, 1, 99);
    expect_u("T4: since_mark keeps the FIRST time", h[1], 50);
    since_clear(h, 1);
    expect_u("T4: since_clear", h[1], 0);
    since_mark(h, kMaxSessions, 5); since_clear(h, kMaxSessions);
    expect(h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0, "T4: an out-of-range slot touches nothing");
}

// ---- T5 ---------------------------------------------------------------------------------------------------------------------------------------------------------
// An independent reference for admit(): written as a decision list, not as the production's early returns.
static uint32_t ref_admit(bool on, bool isWs, bool wsActive, uint32_t K, uint32_t inUse, uint32_t freeDw, uint32_t needDw, uint32_t reserve, bool older) {
    const bool gated = on && !isWs;
    if (!gated) return kAdmit;
    const bool spaceOk = (uint64_t)freeDw >= (uint64_t)needDw + reserve + 16ull;
    const bool creditOk = !wsActive || inUse < K;
    return (spaceOk && creditOk && !older) ? kAdmit : kWait;
}
static void t5_admit() {
    unsigned waits = 0, admits = 0;
    const uint32_t frees[] = { 0u, 100u, 1039u, 1062u, 1063u, 1064u, 4095u };   // need 23 + 1024 + 16 = 1063 is the edge
    const uint32_t Ks[] = { 1u, 2u, 4u };
    const uint32_t inUses[] = { 0u, 1u, 2u, 4u, 9u };
    for (int on = 0; on < 2; on++) for (int ws = 0; ws < 2; ws++) for (int act = 0; act < 2; act++) for (int old = 0; old < 2; old++)
        for (uint32_t f : frees) for (uint32_t k : Ks) for (uint32_t u : inUses) {
            const uint32_t got = admit(on, ws, act, k, u, f, 23u, kReserveDw, old), want = ref_admit(on, ws, act, k, u, f, 23u, kReserveDw, old);
            if (got == kWait) waits++; else admits++;
            if (got != want) { gFail++; std::printf("FAIL: T5: admit(on %d ws %d act %d K %u inUse %u free %u older %d) = %u, reference %u\n", on, ws, act, k, u, f, old, got, want); }
            gRun++;
        }
    expect(waits > 100 && admits > 100, "T5: the table exercises both verdicts");
    // the order and the individual rules, as named cases
    expect_u("T5: OFF never waits", admit(false, false, true, 1, 9, 0, 23, kReserveDw, true), kAdmit);
    expect_u("T5: WindowServer never waits (ring full, credit spent, older waiter)", admit(true, true, true, 1, 9, 0, 23, kReserveDw, true), kAdmit);
    expect_u("T5: an app with room, nobody waiting, WindowServer idle", admit(true, false, false, 1, 0, 4095, 23, kReserveDw, false), kAdmit);
    expect_u("T5: the reserve: free = need + reserve + slack exactly admits", admit(true, false, false, 1, 0, 23 + 1024 + 16, 23, kReserveDw, false), kAdmit);
    expect_u("T5: ... one dword less waits", admit(true, false, false, 1, 0, 23 + 1024 + 16 - 1, 23, kReserveDw, false), kWait);
    expect_u("T5: the credit: inUse == K waits while WindowServer is active", admit(true, false, true, 1, 1, 4095, 23, kReserveDw, false), kWait);
    expect_u("T5: ... inUse == K - 1 admits", admit(true, false, true, 2, 1, 4095, 23, kReserveDw, false), kAdmit);
    expect_u("T5: ... inUse > K waits", admit(true, false, true, 1, 3, 4095, 23, kReserveDw, false), kWait);
    expect_u("T5: ... WindowServer idle: the credit is unlimited", admit(true, false, false, 1, 50, 4095, 23, kReserveDw, false), kAdmit);
    expect_u("T5: an older waiter blocks (WindowServer idle, room, credit free)", admit(true, false, false, 1, 0, 4095, 23, kReserveDw, true), kWait);
    expect_u("T5: the reserve still applies with WindowServer idle", admit(true, false, false, 1, 0, 500, 23, kReserveDw, false), kWait);
    expect_u("T5: an overflowing need waits (ring_has_space rejects need > 0xFFFF)", admit(true, false, false, 1, 0, 0xFFFFFFFFu, 0xFFFFu, kReserveDw, false), kWait);
    expect_u("T5: the reserve is a quarter of the 4096-dword ring", kReserveDw, 1024u);
}

// ---- T6 ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t6_ws_slot1() {
    log_reset();
    uint64_t sc[kMaxSessions] = { 0, 0, 0, 0 };
    sc[0] = 5; sc[2] = 6;   // two apps waiting
    for (uint64_t q = 1; q <= 6; q++) note(q, 0u, 20u);   // slot 0 is an APP (slot 0 is not WindowServer here), six app jobs in flight
    // WindowServer lives on slot 1 (session id 9): never gated, whatever the credit, the reserve and the queue say
    AdmitIn ws { true, true, true, 1000ull, 1001ull, 1u, 0ull, 6ull, 9u, 0u, 23u, sc, 1u };
    expect_u("T6: WindowServer on slot 1 is never gated (credit spent, ring full, older waiters)", admission(gLog, ws), kAdmit);
    ws.slot = 0u;
    expect_u("T6: ... and slot 0 as an app with isWs false is what the credit gates", admission(gLog, AdmitIn{ true, false, true, 1000ull, 1001ull, 1u, 0ull, 6ull, 9u, 4095u, 23u, sc, 0u }), kWait);
    // the app on slot 0 with WindowServer on slot 1 counts as an app; WindowServer's own seqnos are skipped
    log_reset(); note(1, 1u, 9u); note(2, 1u, 9u); note(3, 1u, 9u);
    uint64_t none[kMaxSessions] = { 0, 0, 0, 0 };
    expect_u("T6: only WindowServer's own jobs in flight: an app is admitted (they are not app jobs)", admission(gLog, AdmitIn{ true, false, true, 1000ull, 1001ull, 1u, 0ull, 3ull, 9u, 4095u, 23u, none, 0u }), kAdmit);
    expect_u("T6: the same log, WindowServer id unknown (0): they WOULD be app jobs (fail closed)", admission(gLog, AdmitIn{ true, false, true, 1000ull, 1001ull, 1u, 0ull, 3ull, 0u, 4095u, 23u, none, 0u }), kWait);
    expect_u("T6: OFF: the composite admits an app with everything adverse", admission(gLog, AdmitIn{ false, false, true, 1000ull, 1001ull, 1u, 0ull, 3ull, 9u, 0u, 23u, sc, 0u }), kAdmit);
    expect_u("T6: WindowServer closed: not active, the credit is unlimited, the reserve still applies", admission(gLog, AdmitIn{ true, false, false, 1000ull, 1001ull, 1u, 0ull, 3ull, 0u, 4095u, 23u, none, 0u }), kAdmit);
    expect_u("T6: ... reserve", admission(gLog, AdmitIn{ true, false, false, 1000ull, 1001ull, 1u, 0ull, 3ull, 0u, 1000u, 23u, none, 0u }), kWait);
}

// ---- T7 ---------------------------------------------------------------------------------------------------------------------------------------------------------
static void t7_wait_and_stat() {
    expect_u("T7: the wait bound is 8 s", kG5WaitBoundNs, 8000000000ull);
    expect_u("T7: the stale bound is the wait bound + 1 s", kSinceStaleNs, 9000000000ull);
    expect_u("T7: 0 ns: keep waiting", wait_step(0, false), kKeepWaiting);
    expect_u("T7: just under the cap: keep waiting", wait_step(s1c::kWaitCapNs - 1ull, false), kKeepWaiting);
    expect_u("T7: at the cap: the stall path runs", wait_step(s1c::kWaitCapNs, false), kStallNow);
    expect_u("T7: ... only once", wait_step(s1c::kWaitCapNs + 1ull, true), kKeepWaiting);
    expect_u("T7: ... past the cap, not yet run: runs", wait_step(s1c::kWaitCapNs + 3000000000ull, false), kStallNow);
    expect_u("T7: just under 8 s: keep waiting", wait_step(kG5WaitBoundNs - 1ull, true), kKeepWaiting);
    expect_u("T7: at 8 s: give up", wait_step(kG5WaitBoundNs, true), kGiveUp);
    expect_u("T7: at 8 s even if the stall path never ran: give up", wait_step(kG5WaitBoundNs, false), kGiveUp);
    // counters
    Stat s {};
    stat_admit(s, 0ull);
    expect(s.admitted == 1 && s.delayed == 0 && s.delayNsTotal == 0 && s.delayNsMax == 0, "T7: an undelayed admit counts only as admitted");
    stat_admit(s, 90000000ull); stat_admit(s, 30000000ull);
    expect(s.admitted == 3 && s.delayed == 2 && s.delayNsTotal == 120000000ull && s.delayNsMax == 90000000ull, "T7: delayed admits, total and maximum");
    stat_timeout(s);
    expect_u("T7: a timeout", s.timeouts, 1);
    // the page
    StatView v {}; v.wsActive = true; v.wsOpen = true; v.K = 2; v.appInFlight = 2; v.wsOutstanding = 5; v.st = s; v.sinceWsMs = 7;
    uint64_t o[g2::kOutN] = { 0 };
    stat_out(v, o);
    expect(o[0] == g2::kStatOk && o[1] == 5 && o[2] == 7 && o[3] == 2 && o[4] == 2 && o[5] == 5 && o[6] == 2 && o[7] == 120 && o[8] == 90 && o[9] == 1 && o[10] == 3 && o[11] == 7 && o[12] == 1024,
           "T7: sessstat 5: ok, page, flags, K, in flight, WindowServer outstanding, delayed, total ms, max ms, timeouts, admitted, ms since WindowServer, reserve");
    expect_u("T7: flags: WindowServer idle + closed", stat_flags(false, false), 1);
    expect_u("T7: flags: active", stat_flags(true, false), 3);
    expect_u("T7: flags: open", stat_flags(false, true), 5);
    expect_u("T7: the page has 13 scalars", g2::kOutN, 13);
}

// ---- T8: the ordering simulation ------------------------------------------------------------------------------------------------------------------------------
// The ring as the kernel sees it: seqnos in one in-order queue, the GPU retiring them FIFO, a job's ring dwords freed when it retires. WindowServer submits every 10.7 ms
// (1 ms of GPU each), each app runs `appGpuNs` jobs and, like gpuload, submits its next job only after its previous one retired. Time moves in 100 us ticks. The verdicts come
// from g5::admission, the function ring_submit_app_g5 calls. mode 0 = G5 ON, mode 1 = G5 OFF (admission with on=false), mode 2 = no gate at all (a model).
struct Job { uint64_t seq; int owner; uint64_t gpuNs; uint32_t dw; };
struct Sim {
    std::vector<std::pair<uint64_t, int>> admits;   // (tick time, app index) in order
    uint32_t maxAhead = 0, wsEmits = 0, wsWaits = 0, maxAppInFlight = 0;
    uint64_t adm[kMaxSessions] = { 0, 0, 0, 0 };
    uint64_t totalAdm = 0;
};
static Sim simulate(int mode, uint32_t K, uint32_t wsSlot, bool wsTalks, uint32_t nApps, uint64_t durationNs, uint64_t appGpuNs, uint32_t ringUsedStart = 0u) {
    constexpr uint64_t kTick = 100000ull, kWsGap = 10700000ull, kWsGpu = 1000000ull;
    constexpr uint32_t kWsDw = 20u, kRing = 4095u, wsId = 77u;
    log_reset();
    Sim out;
    uint64_t since[kMaxSessions] = { 0, 0, 0, 0 };
    std::deque<Job> q;
    uint64_t em = 0, re = 0, headLeft = 0, lastWs = 0, nextWs = 0;
    uint32_t used = ringUsedStart;
    bool busy[kMaxSessions] = { false, false, false, false };
    std::vector<uint32_t> appSlots;
    for (uint32_t s = 0; s < kMaxSessions && appSlots.size() < nApps; s++) if (s != wsSlot) appSlots.push_back(s);
    const uint32_t appDw = s1c::submit_dwords(1u, true);
    for (uint64_t t = 0; t < durationNs; t += kTick) {
        // the GPU
        uint64_t rem = kTick;
        while (!q.empty() && rem > 0) {
            if (headLeft == 0) headLeft = q.front().gpuNs;
            const uint64_t take = std::min(rem, headLeft);
            headLeft -= take; rem -= take;
            if (headLeft != 0) break;
            re = q.front().seq; used -= q.front().dw;
            if (q.front().owner >= 0) busy[q.front().owner] = false;
            q.pop_front();
        }
        const uint64_t now = t + 1ull;
        uint32_t appsQueued = 0;
        for (const Job &j : q) if (j.owner >= 0) appsQueued++;
        out.maxAppInFlight = std::max(out.maxAppInFlight, appsQueued);
        // WindowServer
        if (wsTalks && t >= nextWs) {
            nextWs += kWsGap;
            const uint32_t verdict = mode == 2 ? (uint32_t)kAdmit : admission(gLog, AdmitIn{ mode == 0, true, true, lastWs, now, K, re, em, wsId, kRing - used, kWsDw, since, wsSlot });
            if (verdict == kWait) out.wsWaits++;
            out.wsEmits++;
            out.maxAhead = std::max(out.maxAhead, appsQueued);   // app jobs ahead of this emission, counted from the queue itself
            lastWs = now;
            em++; g2::owner_note(gLog, em, (uint8_t)wsSlot, wsId, 0ull);
            q.push_back(Job{ em, -1, kWsGpu, kWsDw }); used += kWsDw;
        }
        // the apps, in slot order
        for (uint32_t a : appSlots) {
            if (busy[a]) continue;
            const uint32_t verdict = mode == 2 ? (uint32_t)kAdmit : admission(gLog, AdmitIn{ mode == 0, false, wsTalks, lastWs, now, K, re, em, wsId, kRing - used, appDw, since, a });
            if (verdict == kAdmit) {
                since_clear(since, a);
                em++; g2::owner_note(gLog, em, (uint8_t)a, 100u + a, 0ull);
                q.push_back(Job{ em, (int)a, appGpuNs, appDw }); used += appDw; busy[a] = true;
                out.adm[a]++; out.totalAdm++; out.admits.push_back({ t, (int)a });
            } else since_mark(since, a, now);
        }
    }
    return out;
}
static void t8_ordering() {
    const uint64_t D = 4000000000ull, J = 50000000ull;
    for (uint32_t K = 1; K <= 3; K++) {
        const Sim g = simulate(0, K, 0u, true, 3u, D, J);
        char w[160];
        std::snprintf(w, sizeof w, "T8: K=%u: at every WindowServer emission the app jobs ahead of it are <= K (max %u)", K, g.maxAhead);
        expect(g.maxAhead <= K, w);
        std::snprintf(w, sizeof w, "T8: K=%u: the bound is reached (the test is not vacuous; max %u)", K, g.maxAhead);
        expect(g.maxAhead == K, w);
        expect(g.wsWaits == 0 && g.wsEmits > 300, "T8: WindowServer never gets Wait and keeps emitting every 10.7 ms");
        expect(g.maxAppInFlight <= K, "T8: the credit holds at every instant, not only at WindowServer's emissions");
        uint64_t lo = ~0ull, hi = 0;
        // the three apps live on slots 1, 2, 3 (slot 0 is WindowServer's)
        for (uint32_t i = 1; i <= 3; i++) { lo = std::min(lo, g.adm[i]); hi = std::max(hi, g.adm[i]); }
        std::snprintf(w, sizeof w, "T8: K=%u: per-app admissions differ by at most 1 (min %llu max %llu)", K, (unsigned long long)lo, (unsigned long long)hi);
        expect(hi - lo <= 1ull && lo > 10ull, w);
        const Sim free_ = simulate(2, K, 0u, true, 3u, D, J);
        expect(g.totalAdm * 100ull >= free_.totalAdm * 95ull, "T8: the GPU stays busy: total app throughput >= 95 % of the ungated model");
    }
    // without the gate the apps queue up: the gate demonstrably acts
    const Sim ungated = simulate(2, 1u, 0u, true, 3u, D, J);
    expect(ungated.maxAhead >= 3u, "T8: (model) with no gate three app jobs sit ahead of WindowServer");
    // WindowServer silent: the credit is unlimited, everybody runs, nobody starves
    const Sim quiet = simulate(0, 1u, 0u, false, 3u, D, J);
    const Sim quietFree = simulate(2, 1u, 0u, false, 3u, D, J);
    expect(quiet.maxAppInFlight == 3u && quiet.admits == quietFree.admits, "T8: WindowServer silent: no throttling beyond the reserve (identical to the ungated model)");
    // WindowServer on slot 1 (slot 0 is an app): same bound, same fairness
    for (uint32_t K = 1; K <= 2; K++) {
        const Sim g = simulate(0, K, 1u, true, 3u, D, J);
        expect(g.maxAhead == K && g.wsWaits == 0, "T8: WindowServer on slot 1: <= K app jobs ahead, never Wait");
        uint64_t lo = ~0ull, hi = 0;
        for (uint32_t i : { 0u, 2u, 3u }) { lo = std::min(lo, g.adm[i]); hi = std::max(hi, g.adm[i]); }
        expect(hi - lo <= 1ull && lo > 10ull, "T8: WindowServer on slot 1: the apps on slots 0, 2, 3 are served alike");
    }
    // two apps, one app, K above the number of apps
    const Sim one = simulate(0, 1u, 0u, true, 1u, D, J);
    expect(one.maxAhead <= 1u && one.adm[1] > 60u, "T8: one app is served back to back (K=1 costs it nothing: it waits for its own fence anyway)");
    const Sim k4 = simulate(0, 4u, 0u, true, 3u, D, J);
    expect(k4.maxAhead <= 3u && k4.admits == ungated.admits, "T8: K=4 with three apps never gates");
    // short jobs: 2 ms jobs from three apps beside WindowServer
    const Sim shortJobs = simulate(0, 1u, 0u, true, 3u, D, 2000000ull);
    expect(shortJobs.maxAhead <= 1u && shortJobs.wsWaits == 0, "T8: 2 ms jobs: still <= K ahead");
    // the ring reserve: with free space under need + reserve + slack an app never gets in, WindowServer keeps going
    const Sim full = simulate(0, 1u, 0u, true, 3u, 1000000000ull, J, 4095u - 1000u);
    expect(full.totalAdm == 0ull && full.wsWaits == 0 && full.wsEmits > 80u, "T8: ring reserve: apps wait while only the reserve is left, WindowServer still submits");
    const Sim fullFree = simulate(2, 1u, 0u, true, 3u, 1000000000ull, J, 4095u - 1000u);
    expect(fullFree.totalAdm > 0ull, "T8: (model) without the gate the apps would have taken the reserve");
    // a leaked since[] entry (a waiter that died) must not wedge the others: the entry is ignored once stale
    {
        log_reset();
        uint64_t sc[kMaxSessions] = { 0, 0, 0, 0 };
        sc[2] = 5ull;   // leaked at the start of time
        const uint64_t late = 5ull + kSinceStaleNs + 1000ull;
        expect_u("T8: a leaked waiter older than the stale bound does not block the next app", admission(gLog, AdmitIn{ true, false, false, 0ull, late, 1u, 0ull, 0ull, 0u, 4095u, 23u, sc, 1u }), kAdmit);
        expect_u("T8: ... a fresh one does", admission(gLog, AdmitIn{ true, false, false, 0ull, 1000ull, 1u, 0ull, 0ull, 0u, 4095u, 23u, sc, 1u }), kWait);
        since_clear(sc, 2);
        expect_u("T8: ... and a cleared one (every exit clears) does not", admission(gLog, AdmitIn{ true, false, false, 0ull, 1000ull, 1u, 0ull, 0ull, 0u, 4095u, 23u, sc, 1u }), kAdmit);
    }
}

// ---- T9: OFF identity -------------------------------------------------------------------------------------------------------------------------------------------
static void t9_off() {
    const uint64_t D = 3000000000ull, J = 50000000ull;
    for (uint32_t wsSlot : { 0u, 1u }) for (int talks = 0; talks < 2; talks++) for (uint32_t K : { 1u, 3u }) {
        const Sim off = simulate(1, K, wsSlot, talks != 0, 3u, D, J), model = simulate(2, K, wsSlot, talks != 0, 3u, D, J);
        expect(off.admits == model.admits && off.maxAhead == model.maxAhead && off.wsWaits == 0, "T9: on=false is identical to a model with no gate (every admission time, app and order)");
    }
    const Sim off = simulate(1, 1u, 0u, true, 3u, D, J), on = simulate(0, 1u, 0u, true, 3u, D, J);
    expect(off.maxAhead >= 3u && on.maxAhead < off.maxAhead, "T9: (sanity) the OFF run is the ungated one and ON differs from it");
    // the pure decision with on=false, whatever else is adverse
    bool allAdmit = true;
    for (int ws = 0; ws < 2; ws++) for (int act = 0; act < 2; act++) for (int old = 0; old < 2; old++) for (uint32_t f : { 0u, 4095u }) for (uint32_t u : { 0u, 9u })
        if (admit(false, ws, act, 1, u, f, 23u, kReserveDw, old) != kAdmit) allAdmit = false;
    expect(allAdmit, "T9: admit(on=false) is Admit for every input");
}

// ---- T10: source pins -------------------------------------------------------------------------------------------------------------------------------------------
// The text of ring_submit as of 0.0.642 (the OFF path calls it unchanged).
static const char *kRingSubmit642 =
"static uint32_t ring_submit(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {\n"
"    IOLockLock(gRingLock);\n"
"    const uint64_t seq = emitted_now() + 1ull;\n"
"    const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, s->vmid, hasFence,\n"
"                                    fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);\n"
"    if (n == 0u) { IOLockUnlock(gRingLock); return (uint32_t)kIOReturnInternalError; }\n"
"    const uint32_t er = ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);\n"
"    IOLockUnlock(gRingLock);\n"
"    if (er == kTimeout) { (void)stall_from_wait(); return kTimeout; }   // 0.0.629 (R2): blames only when the hang clock expired; Timeout either way\n"
"    if (er != kOk) return er;\n"
"    *seqOut = seq;\n"
"    return kOk;";
static void t10_pins(const std::string &root) {
    const std::string eng = slurp(root + "/src/navi48-bringup/src/amd/native_s1c.cpp"), pure = slurp(root + "/src/navi48-bringup/src/amd/native_g5_pure.h"),
        hdr = slurp(root + "/src/navi48-bringup/src/amd/native_s1c.h"), brg = slurp(root + "/src/navi48-bringup/src/Navi48Bringup.cpp"),
        cli = slurp(root + "/tools/pc/navi48test.c"), plist = slurp(root + "/src/navi48-bringup/Info.plist");
    expect(!eng.empty() && !pure.empty() && !hdr.empty() && !brg.empty() && !cli.empty() && !plist.empty(), "T10: the sources were read");
    // OFF identity: ring_submit is the 0.0.642 text, and the OFF branch is the old call
    expect(fn_body(eng, "static uint32_t ring_submit(Session *s,") == kRingSubmit642, "T10: ring_submit is byte-for-byte the 0.0.642 function");
    const std::string sub = fn_body(eng, "IOReturn n1c_submit(");
    expect(!sub.empty() && in_order(sub, { "const uint32_t gate = gpu_gate(s);", "id_live(s->ctxUsed, N48N_MAX_CTX, v.ctx)", "resolve_fence(s, v.fenceHandle, v.fenceOffset, &fenceAddr);", "uint32_t er;",
                                          "if (!g5_on()) er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);   // G5 OFF: exactly 0.0.642",
                                          "else if (s->ws) { __atomic_store_n(&gWsLastNs, now_ns(), __ATOMIC_RELEASE); er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut); }",
                                          "else er = ring_submit_app_g5(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);", "if (er != kOk) { rc = (IOReturn)er; break; }" }),
           "T10: n1c_submit keeps gpu_gate, the IB checks and resolve_fence, then: OFF -> ring_submit unchanged; ON + WindowServer -> stamp + ring_submit; ON + other -> ring_submit_app_g5");
    expect_u("T10: ring_submit_app_g5 is reached from exactly one place (the definition and one call)", count_of(eng, "ring_submit_app_g5("), 2);
    expect_u("T10: the app path is reached only after !g5_on() and s->ws were both false (an else-if chain)", count_of(sub, "else er = ring_submit_app_g5("), 1);
    expect_u("T10: ring_submit call sites: the definition, two in n1c_submit, the two G1 kernel paths", count_of(eng, "ring_submit("), 5);
    expect_u("T10: the G1 kernel paths still call ring_submit on gSess", count_of(eng, "ring_submit(&gSess, &va, &by, 1u, false, 0ull, &seq)"), 2);
    expect_u("T10: gWsLastNs is stored in exactly one place (WindowServer's branch)", count_of(eng, "__atomic_store_n(&gWsLastNs,"), 1);
    expect_u("T10: gG5Since is written only through since_mark / since_clear", count_of(eng, "gG5Since[s->slot] ="), 0);
    expect_u("T10: the OFF path writes nothing: the only users of gG5 are the app path, the stat page", count_of(eng, "stat_admit(gG5") + count_of(eng, "stat_timeout(gG5") + count_of(eng, "v.st = gG5;"), 3);
    // the app path
    const std::string ap = fn_body(eng, "static uint32_t ring_submit_app_g5(");
    expect(!ap.empty(), "T10: ring_submit_app_g5 exists");
    expect(in_order(ap, { "const uint32_t need = submit_dwords(nIbs, hasFence);", "IOLockLock(gRingLock);", "g5_fill_in_locked(s, K, need);", "admission(gOwners, gG5In) == kAdmit", "since_clear(gG5Since, s->slot); waiting = false;",
                          "const uint64_t seq = emitted_now() + 1ull;", "build_submit(", "ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);", "stat_admit(gG5, waitedNs);", "IOLockUnlock(gRingLock);" }),
           "T10: the admission verdict comes BEFORE the seqno, inside the same lock hold, and the ring write follows");
    expect_u("T10: exactly one seqno assignment in the app path", count_of(ap, "emitted_now() + 1ull"), 1);
    expect(ap.find("va, by, nIbs, s->vmid, hasFence,") != std::string::npos && ap.find("kNativeVmid") == std::string::npos, "T10: the app path builds the Submit on the SESSION'S VMID (s->vmid)");
    {   // between the admission and the seqno / the write there is no unlock
        const size_t a = ap.find("admission(gOwners, gG5In)"), b = ap.find("emitted_now() + 1ull"), c = ap.find("ring_put_locked(");
        std::string mid = (a != std::string::npos && c != std::string::npos && a < c) ? ap.substr(a, c - a) : std::string("IOLockUnlock");
        const std::string errExit = "if (n == 0u) { IOLockUnlock(gRingLock); rc = (uint32_t)kIOReturnInternalError; break; }";   // the only unlock before the write: it leaves the function
        const size_t ee = mid.find(errExit); if (ee != std::string::npos) mid.erase(ee, errExit.size());
        expect(a != std::string::npos && b != std::string::npos && c != std::string::npos && a < b && b < c && mid.find("IOLockUnlock") == std::string::npos && mid.find("IOLockLock") == std::string::npos,
               "T10: the admission, the seqno and the ring write are ONE hold of gRingLock (no unlock or relock in between)");
    }
    expect_u("T10: the app path never waits for ring space under the lock (it calls ring_put_locked only after the admission proved the room)", count_of(ap, "ring_put_locked("), 1);
    // no sleep under the lock
    expect_u("T10: one IODelay and one IOSleep in the app path (the single sleep line)", count_of(ap, "IODelay(1)") * 10 + count_of(ap, "IOSleep(1)"), 11);
    expect_u("T10: no stall check, no sleep before the wait path's unlock", count_of(ap, "if (el < 5000000ull) IODelay(1); else IOSleep(1);"), 1);
    {
        const size_t mk = ap.find("since_mark(gG5Since, s->slot, now);"), un = ap.find("IOLockUnlock(gRingLock);", mk), sl = ap.find("IODelay(1)"), hn = ap.find("hung_now()", un);
        expect(mk != std::string::npos && un != std::string::npos && sl != std::string::npos && hn != std::string::npos && mk < un && un < hn && hn < sl, "T10: the wait path: mark, UNLOCK, then the checks, then (only then) the sleep");
        expect(ap.substr(mk, un - mk).find("IODelay") == std::string::npos && ap.substr(mk, un - mk).find("IOSleep") == std::string::npos && ap.substr(mk, un - mk).find("stall_from_wait") == std::string::npos,
               "T10: nothing between since_mark and the unlock sleeps or runs a recovery");
    }
    expect(in_order(ap, { "IOLockUnlock(gRingLock);\n        if (hung_now()) { rc = hang_rc(true, false); break; }", "if (sess_poisoned(s)) { rc = n48native::g2::poison_rc(true); break; }", "const uint32_t step = wait_step(el, stallDone);",
                          "if (step == kGiveUp) { timedOut = true; rc = kTimeout; break; }", "if (step == kStallNow) { stallDone = true; (void)stall_from_wait(); continue; }", "IODelay(1)" }),
           "T10: after the unlock: HUNG -> Aborted, poisoned -> Aborted, then the bounded wait (stall path once, Timeout at 8 s)");
    expect_u("T10: stall_from_wait runs in two places: the app's own ring Timeout and the once-only stall step", count_of(ap, "stall_from_wait()"), 2);
    expect(in_order(ap, { "    if (waiting) {", "IOLockLock(gRingLock);", "since_clear(gG5Since, s->slot);", "if (timedOut) stat_timeout(gG5);", "IOLockUnlock(gRingLock);", "return rc;" }),
           "T10: the single exit clears this slot's gG5Since (under gRingLock) on every path that left while waiting, counts the timeout, then returns");
    expect_u("T10: the function has one return", count_of(ap, "return "), 1);
    expect(ap.find("gCliLock") == std::string::npos && ap.find("IOLockLock(s->lock)") == std::string::npos, "T10: the app path takes no other lock (the session lock is the caller's)");
    const std::string fill = fn_body(eng, "__attribute__((noinline)) static void g5_fill_in_locked(");
    expect(ap.find("const uint32_t K = g5_credit();") != std::string::npos && fill.find("gG5In = n48native::g5::AdmitIn{ true, false,") != std::string::npos && ap.find("g5_fill_in_locked(s, K, need);") != std::string::npos, "T10: the app path runs with G5 ON and isWs = false");
    expect(!fill.empty() && in_order(fill, { "sysmem_rmb();", "ring_free64(rp, gWc, mask)", "gG5Since, s->slot };" }) && fill.find("IOLockLock") == std::string::npos && fill.find("IOSleep") == std::string::npos,
           "T10: the verdict's inputs are read in one place, under the caller's hold of gRingLock (a helper that takes no lock and never sleeps)");
    // the pure side
    expect(pure.find("constexpr uint64_t kRecoverBoundNs = 6000000000ull;") != std::string::npos && eng.find("constexpr uint64_t kRecoverWaitNs = 6000000000ull;") != std::string::npos,
           "T10: the 8 s bound is kWaitCapNs + the kernel's kRecoverWaitNs (both 6 s)");
    expect(in_order(pure, { "if (!on || isWs) return kAdmit;", "ring_has_space(freeDw, needDw + reserveDw)", "if (wsActive && inUse >= K) return kWait;", "if (olderWaiter) return kWait;", "return kAdmit;" }),
           "T10: admit's order: OFF / WindowServer, the reserve, the credit, the older waiter");
    // the latch, K, the stat page
    const std::string g5on = fn_body(eng, "static bool g5_on() {");
    expect(in_order(g5on, { "PE_parse_boot_argn(\"navi48-g5\"", "latch_value(present, v)", "return n48native::g5::on_effective(n48native::g2::latch_is_on(l), multi_on());" }), "T10: g5_on(): latched once from navi48-g5, effective only with multisession");
    expect(in_order(fn_body(eng, "static uint32_t g5_credit() {"), { "PE_parse_boot_argn(\"navi48-g5credit\"", "n48native::g5::credit_value(present, v)" }), "T10: g5_credit(): navi48-g5credit through the clamp");
    const std::string st = fn_body(eng, "IOReturn n1c_g5_stat(");
    expect(in_order(st, { "if (!g5_on())", "IOLockLock(gRingLock);", "app_in_flight(gOwners, re, em, wsId, n48native::g2::kOwnerN)", "ws_outstanding(gOwners, re, em, wsId)", "v.st = gG5;", "IOLockUnlock(gRingLock);", "stat_out(v, o);" }) && st.find("gCliLock") == std::string::npos,
           "T10: the stat page reads o[4], o[5] and the counters in ONE hold of gRingLock and never takes gCliLock");
    expect(brg.find("const bool g5page = n48native::g5::args_ok(amdgpu::n1c_g5_latched_on(), action, argScalar);") != std::string::npos
           && in_order(brg, { "const bool g5page = n48native::g5::args_ok(", "if (!g5page && !n48native::g2::native_exempt(amdgpu::n1c_multi_latched_on(), action, argScalar)) return kIOReturnBadArgument;", "g5page ? amdgpu::n1c_g5_stat(v) : amdgpu::n1c_g2_stat(argScalar, v);" }),
           "T10: accelExperiment: page 5 only through g5::args_ok(latch), everything else through G2's native_exempt as before");
    expect(hdr.find("bool n1c_g5_latched_on();") != std::string::npos && hdr.find("IOReturn n1c_g5_stat(uint64_t *o);") != std::string::npos, "T10: declared");
    // WindowServer's id: set and cleared beside gSessWs
    expect(in_order(eng, { "if (ws) __atomic_store_n(&gSessWs, 1u, __ATOMIC_SEQ_CST);", "if (ws) __atomic_store_n(&gWsId, s->id, __ATOMIC_SEQ_CST);" }), "T10: n1c_open records WindowServer's session id beside gSessWs");
    expect(in_order(eng, { "if (s->ws) __atomic_store_n(&gSessWs, 0u, __ATOMIC_SEQ_CST);", "if (s->ws) __atomic_store_n(&gWsId, 0u, __ATOMIC_SEQ_CST);" }), "T10: n1c_close clears it");
    expect_u("T10: gWsId is written in exactly those two places", count_of(eng, "__atomic_store_n(&gWsId,"), 2);
    // the CLI
    expect(cli.find("hangstat [0..8]|sessstat [0..5]|") != std::string::npos && cli.find("} else if (c == 0 && out[4] == 5) {") != std::string::npos && cli.find("amdgpu::n1c_g5_stat") != std::string::npos
           && cli.find("credit-wait timeouts") != std::string::npos && cli.find("WindowServer ring reserve") != std::string::npos, "T10: navi48test: sessstat 5 is accepted and printed");
    // the lock map still stands; the stack rule
    expect(eng.find("// THE LOCK MAP (0.0.627, G2). Order: a session's own lock -> gCliLock -> gRingLock -> gHangLock; gTlbLock is a leaf") != std::string::npos, "T10: the lock map comment is intact");
    expect(ap.find("OwnerLog ") == std::string::npos && ap.find("uint32_t buf[") == std::string::npos, "T10: no big object on the app path's stack");
    // banned strings (the pieces are joined here so this file does not carry them; tools/pc/navi48test.c already carries two lines from before 0.0.642: not checked here), the version
    const std::string b1 = std::string("pipe+0x2") + "80", b2 = std::string("+0x2") + "82", b3 = std::string("+0x2") + "99";
    for (const std::string *f : { &eng, &pure, &hdr, &brg }) expect(f->find(b1) == std::string::npos && f->find(b2) == std::string::npos && f->find(b3) == std::string::npos, "T10: no banned pipe offsets");
    expect(count_of(plist, "<string>0.0.664</string>") == 2 && plist.find("0.0.642") == std::string::npos, "T10: Info.plist is 0.0.664, carried twice");
    expect(hdr.find("constexpr uint32_t kN1cKextBuild = 664;") != std::string::npos, "T10: the native client reports build 660");
    expect(plist.find("<string>0.0.664</string>") != std::string::npos && eng.find("0.0.642\"") == std::string::npos, "T10: no stale version string in the engine");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    t1_switch(); t2_ws_active(); t3_in_flight(); t4_older(); t5_admit(); t6_ws_slot1(); t7_wait_and_stat(); t8_ordering(); t9_off(); t10_pins(root);
    std::printf("native_g5: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
