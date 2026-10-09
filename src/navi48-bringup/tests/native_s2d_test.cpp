// native_s2d_test.cpp - build 0.0.605 (native step S2d): the timed mode trial. It compiles the REAL sequences of dcn/navi48_modetrial_pure.h (the deny gate, the plan, the write
// order, the dwell, THE restore, the watchdog body, the latch) on the host and drives them against a model of the display hardware: a register file whose DP DTO, OTG_V_TOTAL and
// OTG_H_TOTAL decide how fast a virtual OTG frame counter runs, so "the rate came back" and "the rate never moved" are computed by the same arithmetic the kext measures with. The
// kext's own file (dcn/navi48_dcn.cpp) supplies only the Hw callbacks; source pins below check its wiring (exemptions, golden at bind, dcnmode 0, scanAcquire, shutdown, the single write).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/dcn -I src/dcn41 src/navi48-bringup/tests/native_s2d_test.cpp -x c++ src/dcn41/dcn41_allow.c -o /tmp/native_s2d
//   /tmp/native_s2d .          (run from the repo root; the argument is the repo root the source pins read from)
//   tests/native_s2d_plant.sh plants breaks and shows every check that catches them.
// Covers:
//   T1  the generated tables: 56 registers, unique, every one inside the dcn41 write allowlist (the real allowlist code), Linux write order, row 50 = the DTO trio only (no DLG/TTU),
//       row 120 = all; the 60 Hz golden equals the census values of the live hardware; DTO convention pixclk = INT * modulo + phase; the three refresh rates;
//   T2  clock sufficiency with the real DID -> MHz table: 272.73 MHz refuses 120 and admits 50; the exact 514.285 / 500.000 edge; DID 0x64 = 125 MHz; DENTIST disagreement; unreadable;
//   T3  the deny matrix: every reason, its order, and that a denied trial WRITES NOTHING;
//   T4  a whole trial against the model: PASS rows 50 and 120 (Linux order, RMW mask, restore reversed and bit-exact, rate windows >= 2 s, dwell held), and every failure verdict
//       (RATE, FIFO, STREAM, STALL, WRITE, RESTORE, ABORT) with its restore, the sticky latch, the busy flag;
//   T5  the watchdog thread: a hung trial thread is restored by the watchdog (stale heartbeat and deadline), once, never twice, the trial answers WATCHDOG; started before the first write;
//   T6  restore_from_golden (`dcnmode 0`): puts back exactly the drifted registers in reverse order, no copy = no write;
//   S   source pins: the ABI 1.3 text, the exemption table, the single WREG32 behind the allowlist, golden at bind, dcnmode 0, scanAcquire, shutdown, the selector wiring, the tool, the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <cmath>
#include <sstream>
#include "navi48_modetrial_pure.h"
#include "Navi48NativeABI.h"
extern "C" {
#include "dcn41_allow.h"
}

using namespace n48mt;

static int gFail = 0, gRun = 0;
static std::string g_root = ".";
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }
// The body of the function whose signature starts with `sig` (brace matched from the first '{' after it).
static std::string fn_body(const std::string &s, const std::string &sig) {
    const size_t a = s.find(sig);
    if (a == std::string::npos) return "";
    const size_t b = s.find('{', a);
    if (b == std::string::npos) return "";
    int d = 0;
    for (size_t i = b; i < s.size(); i++) { if (s[i] == '{') d++; else if (s[i] == '}') { if (--d == 0) return s.substr(a, i - a + 1); } }
    return "";
}

// ---- the hardware model ------------------------------------------------------------------------------------------------------------------------------------
static std::set<uint32_t> gAllWr;   // every address any test's engine tried to write
static std::mutex gAllWrMu;
static thread_local bool tIsTrial = false;
static thread_local bool tIsWd = false;   // 0.0.607: the model's watchdog threads (a clock message from one is a defect)
static thread_local int tLock = 0;
struct Env;
struct Model {
    std::mutex mu; std::condition_variable cv;
    std::map<uint32_t, uint32_t> reg, init;
    std::vector<std::pair<uint32_t, uint32_t>> writes;      // accepted writes, in order
    std::vector<std::string> ev;                            // "wdstart", "write"
    uint64_t vnow = 5000000ull; double frames = 100000.0;
    uint64_t reads = 0;
    bool gate = true, armed = true, planeAcq = false, inUse = false, otgOk = true, clkRead = true;
    n48dal::Decoded clk{};
    std::set<uint32_t> refuseWr, ignoreWr, unreadable;
    bool refuseRestoreDto = false, ignoreDto = false, fifoSteady = false, dioOnWrite = false, dioOnRestore = false, steerStuck = false, digStuck = false, fifoBitStuck = false, streamDownOnDto = false, stallOnDto = false, wdStartFail = false, dtoStaysAfterRestore = false;
    bool hangAfterWrites = false; std::atomic<bool> hung{false}; bool release = false;
    std::vector<std::thread> wds;
    Env *env = nullptr;
    // ---- 0.0.607: the clock hold and row 120 ----
    std::vector<std::string> trace;                         // every ACCEPTED register write ("W:<abs>") and every clock-hold message ("HM:<clk>:<mhz>"), in order
    uint32_t did[2] = { 0x41, 0x41 };                        // the DFS DIDs the hold's messages move (m.clk is decoded from them)
    int holdFailAt = -1; uint32_t holdFailKind = 1u; bool holdFailAcked = false, holdNeverMove = false, holdNoDrop = false;   // one injected fault: message index, n48dal::HmRc kind
    int holdMsgs = 0, holdCalls = 0, holdUnderLock = 0, holdFromWd = 0, holdLatches = 0;
    bool drrIgnore = false;                                  // the DRR mode write does not take (row 120 step 3)
    uint64_t clkDropAtUs = 0; bool clkDropped = false;       // the DFS readback falls under the need at this virtual time
    bool abortDuringRaise = false;
    // ---- 0.0.609: the HELD mode. The trial thread PAUSES (real time, virtual time frozen) at its first sleep once the hold is UP, until pauseHeld is cleared: the test thread acts on a frozen machine. scan_release is modelled.
    std::atomic<bool> pauseHeld{false}, inPause{false}; bool hangWhenHeld = false;
    uint32_t *heldWord = nullptr;
    bool scanAcq = false; uint32_t scanRc = 1; int scanCalls = 0, scanUnderLock = 0, scanFromWd = 0;
    bool clkRead2 = true, clkUnreadableOnDrop = false, clkBack = false, neverActiveLate = false, streamDown = false;   // neverActiveLate: the stream fault starts with the first write (the baseline reads stay clean)
    // ---- 0.0.606: the underflow registers, the DP1 stream / steer / DIG FIFO, the OTG lock and the double-buffered V_TOTAL ----
    // P1
    bool hubpClearIgnored = false, optcClearIgnored = false, strobeSticky = false;
    uint64_t ufAtUs = 0; bool ufFired = false; uint32_t ufHubp = 0x10000000u, ufOptc = 0x2400u;   // an underflow event scheduled at a virtual time (0 = none); the bits it sets
    bool ufOnRestore = false, ufOnDto = false;
    // 0.0.608: OPTC underflow bit 10 raised at one exact point of a row-120 trial (each fires once; the DTO having changed, or having changed and come back, tells the apply from the restore from the verify window)
    bool optcAtApMsa = false, optcAtApUnblank = false, optcAtRsLatch = false, optcAtVerify = false, optcSoftReset = false, dtoEverChanged = false, dioOnHoldRaise = false; int optcFired = 0;
    void fireOptc() { reg[kRegOptcInGlobal] |= uf::kOptcOccurred | (optcSoftReset ? uf::kOptcSoftReset : 0u); optcFired++; }
    // P3
    bool vidDynamic = false;   // the stream model takes over from the register value at the kext's first write of DP1_DP_VID_STREAM_CNTL (tests that set the raw register keep working)
    bool vidEn = true, statusStuck = false, neverActive = false, digNeverDone = false, steerEarlyIsFatal = true, noStatusDrop = false;
    uint64_t vidChangeAt = 0, digChangeAt = 0; bool digDone = false;
    uint32_t vidStatusDelayUs = 20u, vidRiseDelayUs = 10u, digDelayUs = 20u;
    // P2
    bool lockStatusNever = false, lockStuck = false, noLatch = false, posClamp = false, lockReq = false;
    uint32_t uaAddr = 0; uint64_t uaFrom = 0, uaTo = 0;   // a register that reads all-ones inside a virtual-time window
    bool lockLeaks = false, drrWhilePending = false;   // the lock does not hold a V_TOTAL write; the DRR mode was written while a latch was still pending
    uint32_t vtLatchOverride = 0;   // a V_TOTAL register value the hardware latches INSTEAD of the one written (0 = none)
    int digFailCount = 0; bool digSkipThis = false;   // the next N DIG FIFO resets never complete (a one-shot fault for the retry test)
    bool vtPending = false, vtEffValid = false; uint32_t vtEff = 0, vtPend = 0;
    // -- the hardware --
    bool streamDownUntilRestore = false;
    bool dtoChanged() { return reg[0x141] != init[0x141]; }
    uint32_t drrMode() { return (reg[kRegDbCtrl] >> 24) & 3u; }
    uint32_t vtNow() { return vtEffValid ? vtEff : (reg[kCap[kIdxVTotal].abs] & 0x7FFF); }
    double rate() {
        const uint32_t dtoInt = (reg[0x12f] >> 1) & 0xF, ht = (reg[kCap[kIdxHTotal].abs] & 0x7FFF) + 1, vt = vtNow() + 1;
        return ((double)dtoInt * reg[0x142] + reg[0x141]) / ((double)ht * vt);
    }
    void syncClk();
    void fireUf() { reg[kRegHubpCntl] |= ufHubp; reg[kRegOptcInGlobal] |= ufOptc; }
    void advance(uint64_t us) {
        vnow += us;
        const double f0 = frames;
        if (!(stallOnDto && dtoChanged())) frames += rate() * (double)us / 1e6;
        if (fifoSteady && dtoChanged()) reg[kRegPixelRateCntl] += 0x10000u;
        if (vtPending && (!lockReq || lockLeaks) && !noLatch && std::floor(frames) > std::floor(f0)) { reg[kCap[kIdxVTotal].abs] = (reg[kCap[kIdxVTotal].abs] & ~0x7FFFu) | (vtLatchOverride != 0 ? vtLatchOverride : vtPend); vtLatchOverride = 0; vtEffValid = false; vtPending = false; }   // latched at the frame start
        if (ufAtUs != 0 && !ufFired && vnow >= ufAtUs) { ufFired = true; fireUf(); }
        if (clkDropAtUs != 0 && !clkDropped && vnow >= clkDropAtUs) { clkDropped = true; if (clkUnreadableOnDrop) clkRead = false; else { did[0] = 0x3C; syncClk(); } }   // DISPCLK falls to 300 MHz behind the trial's back (or the readback goes dark)
        if (clkUnreadableOnDrop && clkDropped && !clkBack && vnow >= clkDropAtUs + 400000ull) { clkRead = true; clkBack = true; }   // ... for 400 ms
    }
    uint32_t vidReg() {   // DP1_DP_VID_STREAM_CNTL: ENABLE and STATUS follow the request with a delay
        if (!vidDynamic) return reg[kRegDp1Vid];
        if (streamDown) return reg[kRegDp1Vid] & ~0x10001u;   // the DP stream is down (streamDownOnDto with the resync model on)
        uint32_t v = reg[kRegDp1Vid] & ~0x10001u;
        if (vidEn) v |= 1u;
        const bool settled = vnow >= vidChangeAt;
        bool status = settled ? vidEn : !vidEn;
        if (vidEn && settled && neverActive && !(neverActiveLate && writes.empty())) status = false;
        if (!vidEn && noStatusDrop) status = true;
        if (statusStuck) status = true;
        if (status) v |= 0x10000u;
        return v;
    }
};
static Model *M(void *c) { return static_cast<Model *>(c); }
static uint32_t h_rd(void *c, uint32_t a) {
    Model *m = M(c); std::lock_guard<std::mutex> g(m->mu); m->reads++;
    if (m->unreadable.count(a)) return 0xFFFFFFFFu;
    if (m->uaAddr == a && m->vnow >= m->uaFrom && m->vnow < m->uaTo) return 0xFFFFFFFFu;
    if (a == kRegDp1Vid) return m->vidReg();
    if (a == kRegStatusPos && m->optcAtRsLatch && m->dtoEverChanged && !m->dtoChanged() && m->optcFired == 0) m->fireOptc();   // the restore's V_TOTAL latch witness (the DTO is back, the OTG registers are being read back)
    if (a == kRegDigFifoCtrl0) { uint32_t v = m->reg[a] & ~kDigResetDone; if ((m->reg[a] & kDigReset) != 0u ? (m->vnow >= m->digChangeAt && !m->digNeverDone && !m->digSkipThis) : false) v |= kDigResetDone; return v; }
    if (a == kRegMasterLock) return (m->reg[a] & ~kLockStatus) | ((m->lockReq && !m->lockStatusNever) ? kLockStatus : 0u);
    if (a == kRegDbCtrl) return (m->reg[a] & ~kDbPendingMask) | (m->vtPending ? 0x20u : 0u);
    if (a == kRegPipeUpd) return m->vtPending ? 0x10u : 0u;
    if (a == kRegStatusPos) {
        const uint32_t lines = m->vtNow() + 1u; double fr = m->frames - std::floor(m->frames);
        uint32_t c2 = (uint32_t)(fr * lines); if (m->posClamp && c2 > 1480u) c2 = 1480u;
        return c2 & kVertMask;
    }
    auto it = m->reg.find(a); return it == m->reg.end() ? 0u : it->second;
}
static bool h_wr(void *c, uint32_t a, uint32_t v) {
    Model *m = M(c); std::lock_guard<std::mutex> g(m->mu);
    { std::lock_guard<std::mutex> g2(gAllWrMu); gAllWr.insert(a); }
    if (m->refuseWr.count(a) || (a == 0x141 && m->refuseRestoreDto && v == m->init[0x141])) return false;
    m->writes.push_back({ a, v }); m->ev.push_back("write"); { char b[32]; std::snprintf(b, sizeof(b), "W:%x=%x", a, v); m->trace.push_back(b); }
    if (m->ignoreWr.count(a) || (a == 0x141 && m->ignoreDto)) return true;
    if (a == kRegHubpCntl) {
        uint32_t st = m->reg[a] & 0x70F00000u;
        if ((v & (uf::kHubpClear | uf::kHubpTimeoutClear)) != 0u && !m->hubpClearIgnored) { if (v & uf::kHubpClear) st &= ~0x70000000u; if (v & uf::kHubpTimeoutClear) st &= ~0x00F00000u; }
        m->reg[a] = (v & ~(0x70F00000u | uf::kHubpClear | uf::kHubpTimeoutClear)) | st | (m->strobeSticky ? (v & (uf::kHubpClear | uf::kHubpTimeoutClear)) : 0u);
        return true;
    }
    if (a == kRegOptcInGlobal) {
        uint32_t st = m->reg[a] & uf::kOptcSeen;
        if ((v & uf::kOptcClear) != 0u && !m->optcClearIgnored) st = 0u;
        m->reg[a] = (v & ~(uf::kOptcSeen | uf::kOptcClear | uf::kOptcDbPending)) | st | (m->strobeSticky ? (v & uf::kOptcClear) : 0u);
        return true;
    }
    if (a == kRegDp1Vid) {
        const bool en = (v & 1u) != 0u;
        if (en && m->optcAtApUnblank && m->dtoChanged() && m->optcFired == 0) m->fireOptc();   // the apply's unblank (the DTO is at 120 Hz)
        m->vidDynamic = true;
        if (en != m->vidEn) { m->vidEn = en; m->vidChangeAt = m->vnow + (en ? m->vidRiseDelayUs : m->vidStatusDelayUs); }
        m->reg[a] = v & ~0x10000u;
        return true;
    }
    if (a == kRegDp1Steer) {
        const bool resetNow = (v & kSteerReset) != 0u, wasReset = (m->reg[a] & kSteerReset) != 0u;
        if (resetNow && !wasReset && m->steerEarlyIsFatal && (m->vidReg() & 0x10000u) != 0u && !m->vidEn) m->statusStuck = true;   // Linux: a steer reset while STATUS has not dropped leaves it stuck
        m->reg[a] = (v & ~0x74u) | ((resetNow || wasReset) ? 0u : (m->reg[a] & 0x10u)) | (resetNow ? 4u : 0u);   // the overflow flag clears with a steer reset (model assumption)
        return true;
    }
    if (a == kRegDigFifoCtrl0) {
        const bool resetNow = (v & kDigReset) != 0u, wasReset = (m->reg[a] & kDigReset) != 0u;
        if (resetNow != wasReset) m->digChangeAt = m->vnow + m->digDelayUs;
        if (resetNow && !wasReset) { m->digSkipThis = m->digFailCount > 0; if (m->digSkipThis) m->digFailCount--; }
        m->reg[a] = (v & ~kDigNever) | (m->reg[a] & 0x30000000u);
        return true;
    }
    if (a == kRegMasterLock) {
        const bool req = (v & kLockReq) != 0u;
        if (!req && m->lockStuck) { m->reg[a] = (v | kLockReq) & ~kLockStatus; return true; }   // the request stays set
        m->lockReq = req; m->reg[a] = (v & ~kLockStatus);
        if (!req && m->lockReq == false && m->vtPending && !m->noLatch) { /* the latch happens at the next frame start (advance) */ }
        return true;
    }
    if (a == kRegDbCtrl) { if (m->vtPending) m->drrWhilePending = true; if (m->drrIgnore) return true; m->reg[a] = v & ~kDbPendingMask; return true; }
    if (a == kCap[kIdxVTotal].abs && (m->drrMode() == 2u || m->lockReq)) {   // double-buffered: the new total waits for a frame start with the lock released
        if (!m->vtPending) { m->vtEff = m->reg[a] & 0x7FFFu; m->vtEffValid = true; }
        m->vtPending = true; m->vtPend = v & 0x7FFFu; m->reg[a] = v;
        return true;
    }
    if (a == 0x141 && m->dtoStaysAfterRestore && v == m->init[0x141]) return true;   // the restore of the DTO does not take
    if (a >= 0x5746u && a <= 0x5749u && m->optcAtApMsa && m->dtoChanged() && m->optcFired == 0) m->fireOptc();   // the apply's MSA (before the unblank)
    if (a == 0x141 && v != m->init[0x141]) m->dtoEverChanged = true;
    const bool wasChanged = m->reg[0x141] != m->init[0x141];
    m->reg[a] = v;
    if (a == kCap[kIdxVTotal].abs) { m->vtEffValid = false; m->vtPending = false; }
    if (a == 0x141) {
        if (m->ufOnDto && v != m->init[0x141]) m->fireUf();
        if (m->ufOnRestore && v == m->init[0x141] && wasChanged) m->fireUf();
        const bool changed = v != m->init[0x141];
        if (!changed && wasChanged && m->streamDownUntilRestore) m->streamDown = false;   // 0.0.609: a stream fault raised while HELD clears when the timing is restored
        if (changed && m->dioOnWrite) m->reg[kRegPixelRateCntl] += 0x10000u;                                   // the write transition
        if (!changed && wasChanged && m->dioOnRestore) m->reg[kRegPixelRateCntl] += 0x10000u;                  // the restore transition
        if (!changed && wasChanged && m->steerStuck) m->reg[kRegDp1Steer] |= 0x10u;
        if (!changed && wasChanged && m->digStuck) m->reg[kRegDigFifoCtrl0] |= 0x10000000u;
        if (!changed && wasChanged && m->fifoBitStuck) m->reg[kRegPixelRateCntl] |= 0x4000u;
        if (m->streamDownOnDto) { m->reg[kRegDpStream[1]] = changed ? 0x00000200u : 0x00010201u; m->streamDown = changed; }
    }
    return true;
}
static void h_sleep(void *c, uint32_t ms) {
    Model *m = M(c);
    if (tIsTrial && m->pauseHeld.load() && (m->heldWord != nullptr ? __atomic_load_n(m->heldWord, __ATOMIC_SEQ_CST) : 0u) != 0u) { m->inPause = true; while (m->pauseHeld.load()) std::this_thread::sleep_for(std::chrono::microseconds(50)); m->inPause = false; }
    {
        std::unique_lock<std::mutex> g(m->mu);
        // The trial thread's sleeps are the clock. The watchdog thread's sleeps advance it only while the trial thread is hung (else the watchdog would run the clock away from the trial).
        if (tIsTrial || m->hung.load()) m->advance((uint64_t)ms * 1000ull);
        if (tIsTrial && m->hangWhenHeld && (m->heldWord != nullptr ? __atomic_load_n(m->heldWord, __ATOMIC_SEQ_CST) : 0u) != 0u && tLock == 0) {
            m->hung = true;
            m->cv.wait(g, [m] { return m->release; });
            m->hung = false; m->hangWhenHeld = false;
        }
        if (tIsTrial && m->hangAfterWrites && !m->writes.empty() && tLock == 0) {
            m->hung = true;
            m->cv.wait(g, [m] { return m->release; });
            m->hung = false; m->hangAfterWrites = false;
        }
    }
    std::this_thread::sleep_for(std::chrono::microseconds(30));
}
static uint64_t h_now(void *c) { Model *m = M(c); std::lock_guard<std::mutex> g(m->mu); return m->vnow; }
static bool h_frame(void *c, uint32_t *fc) { Model *m = M(c); std::lock_guard<std::mutex> g(m->mu); if (m->optcAtVerify && m->dtoEverChanged && !m->dtoChanged() && m->optcFired == 0) m->fireOptc(); *fc = (uint32_t)(uint64_t)m->frames & 0xFFFFFFu; return true; }   // (the verify window: the DTO is back)
static std::mutex gEng;                         // the engine lock (the kext's gMtLock)
static void h_lock(void *) { gEng.lock(); tLock++; }
static void h_unlock(void *) { tLock--; gEng.unlock(); }
static void h_delay(void *c, uint32_t us) { Model *m = M(c); std::unique_lock<std::mutex> g(m->mu); if (tIsTrial || m->hung.load()) m->advance(us); }
static bool h_gate(void *c) { return M(c)->gate; }
static bool h_armed(void *c) { return M(c)->armed; }
static bool h_plane(void *c) { return M(c)->planeAcq; }
static bool h_inuse(void *c) { return M(c)->inUse; }
static bool h_otg(void *c) { return M(c)->otgOk; }
static bool h_clocks(void *c, n48dal::Decoded *d) { Model *m = M(c); *d = m->clk; return m->clkRead; }
static void h_log(void *, uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) {}
static bool h_wd(void *c, uint32_t id);
// 0.0.609: the scanout plane back to the console. Called with NO engine lock (tLock == 0) or the model records a violation; the trace gets "SCAN" at the call, so the order against the restore's writes and the clock messages is checkable.
static uint32_t h_scan(void *c) {
    Model *m = M(c); std::lock_guard<std::mutex> g(m->mu);
    m->scanCalls++; if (tLock != 0) m->scanUnderLock++; if (tIsWd) m->scanFromWd++;
    m->trace.push_back("SCAN");
    if (!m->scanAcq) return 0u;
    const uint32_t rc = m->scanRc; if (rc == 1u) m->scanAcq = false; return rc;
}
static uint32_t h_hpre(void *c);
static uint32_t h_hraise(void *c, uint32_t nd, uint32_t np, n48dal::HoldRep *rep, n48dal::Decoded *after);
static uint32_t h_hrel(void *c, bool restoreBad, n48dal::HoldRep *rep);
static uint32_t h_hstate(void *c);

struct Env {
    Model m; State st; Timing tm = kProd; n48n_mode_result r; Hw hw;
    n48dal::HoldCtl hc{}; uint32_t hOwner = 0, hLevel = 4; bool hE1b = true, hStopped = false;   // 0.0.607: the model of smu_dal.cpp's hold state (the REAL hold_raise / hold_release run on it)
    Env() {
        hc.state = n48dal::kHsIdle; hc.owner = &hOwner;
        std::memset(&st, 0, sizeof(st)); std::memset(&r, 0, sizeof(r)); tm.row120 = true;   // row 120 is enabled for the engine tests; kProd keeps it off
        m.env = this; m.heldWord = &st.held;
        hw = Hw{ &m, h_rd, h_wr, h_sleep, h_now, h_frame, h_lock, h_unlock, h_gate, h_armed, h_plane, h_inuse, h_otg, h_clocks, h_wd, h_log, h_delay, h_hpre, h_hraise, h_hrel, h_hstate, h_scan };
        // the live 60 Hz hardware: the 60 Hz golden under each register's mask, junk in every bit the write never touches (so a whole-dword restore would be caught)
        uint32_t x = 0x9E3779B9u;
        for (uint32_t i = 0; i < kCapN; i++) {
            x = x * 1664525u + 1013904223u;
            const uint32_t v = (tab::kGold60[i] & kCap[i].mask) | (x & ~kCap[i].mask);
            m.reg[kCap[i].abs] = v; m.init[kCap[i].abs] = v;
        }
        m.reg[kRegPixelRateCntl] = 0x00011090u;
        for (uint32_t d = 0; d < 4; d++) m.reg[kRegDpStream[d]] = d == kLitDp ? 0x00010201u : 0u;
        m.reg[kRegDp1Steer] = 1u; m.reg[kRegDigFifoCtrl0] = 0x1Du;          // the census: steer ENABLE, DIG FIFO ENABLE with READ_START_LEVEL 7, PIXEL_PER_CYCLE 0
        m.reg[kRegHubpCntl] = 0x000F0002u; m.reg[kRegOptcInGlobal] = 0u;      // the census: no underflow, HUBP with no outstanding requests
        m.reg[kRegDbCtrl] = 0u; m.reg[kRegMasterLock] = 0u; m.reg[kRegGlobalCtrl2] = 0u; m.reg[kRegPipeUpd] = 0u;
        m.clk = n48dal::decode_clocks(0x2d, 0x41, 0x41, 0x417f4141u);       // the E1 census: 272.73 MHz
        m.did[0] = m.did[1] = 0x41;
    }
    ~Env() { { std::lock_guard<std::mutex> g(m.mu); m.release = true; } m.cv.notify_all(); join(); }
    void join() { for (auto &t : m.wds) if (t.joinable()) t.join(); m.wds.clear(); }
    void fast_clocks() { m.clk = fast_dec(0x23, 0x24); }
    static n48dal::Decoded fast_dec(uint32_t dispDid, uint32_t dppDid) {
        // DENTIST agreeing with the two DFS DIDs and both CHG_DONE bits set
        const uint32_t dent = (dppDid << 24) | 0x00180000u | dispDid;
        return n48dal::decode_clocks(0x2d, dispDid, dppDid, dent);
    }
    void bind() { (void)golden_take(hw, st, true); }
    const Timing *timing() { return &tm; }
};
static bool h_wd(void *c, uint32_t id) {
    Model *m = M(c);
    if (m->wdStartFail) return false;
    { std::lock_guard<std::mutex> g(m->mu); m->ev.push_back("wdstart"); m->trace.push_back("WD"); }
    Env *e = m->env;
    m->wds.emplace_back([e, id] { tIsWd = true; watchdog_body(e->hw, e->st, e->tm, id); });
    return true;
}
// ---- 0.0.607: the clock hold on the model. hold_raise / hold_release are the REAL functions (smu_dal_pure.h); this is only the PMFW + DFS side of them.
void Model::syncClk() { clk = n48dal::decode_clocks(0x2d, did[0], did[1], (did[1] << 24) | 0x00180000u | did[0]); }
static void hnote(void *c) { Model *m = M(c); m->holdCalls++; if (tLock != 0) m->holdUnderLock++; if (tIsWd) m->holdFromWd++; }
static uint32_t hio_hm(void *c, uint32_t clk, uint32_t mhz, bool *acked) {
    Model *m = M(c); std::lock_guard<std::mutex> g(m->mu);
    const int idx = m->holdMsgs++; { char b[32]; std::snprintf(b, sizeof(b), "HM:%u:%u", clk, mhz); m->trace.push_back(b); }
    m->advance(2000ull);                                    // a mailbox round trip (~2 ms)
    if (m->abortDuringRaise && idx == 1) { m->abortDuringRaise = false; Env *ee = m->env; ee->st.abortReq = true; }   // S4: an abort lands while the hold is being raised
    const int i = (int)clk - 6; const bool raise = mhz >= 500u;
    auto apply = [&] { if (raise) { if (!m->holdNeverMove) m->did[i] = 0x21; } else if (!m->holdNoDrop) m->did[i] = 0x41; m->syncClk(); };
    if (idx == m->holdFailAt) { *acked = m->holdFailAcked; if (m->holdFailKind == n48dal::kHmPollTimeout) { *acked = true; apply(); } return m->holdFailKind; }
    *acked = true; apply(); return n48dal::kHmOk;
}
static bool hio_dec(void *c, n48dal::Decoded *d) { Model *m = M(c); std::lock_guard<std::mutex> g(m->mu); *d = m->clk; return m->clkRead2 && m->clkRead; }
static void hio_sl(void *c, uint32_t ms) { h_sleep(c, ms); }
static uint64_t hio_now(void *c) { return h_now(c); }
static void hio_latch(void *c) { Model *m = M(c); std::lock_guard<std::mutex> g(m->mu); m->holdLatches++; }
static n48dal::HoldIo hio(Model *m) { return n48dal::HoldIo{ m, hio_hm, hio_dec, hio_sl, hio_now, hio_latch, nullptr }; }
static uint32_t h_hpre(void *c) { Env *e = M(c)->env; return n48dal::hold_pre_check(e->hLevel, e->hE1b, e->hStopped, n48dal::hold_state_now(e->hc), n48dal::hold_owner_now(e->hc)); }
static uint32_t h_hraise(void *c, uint32_t nd, uint32_t np, n48dal::HoldRep *rep, n48dal::Decoded *after) {
    hnote(c); Env *e = M(c)->env; const uint32_t rc = n48dal::hold_raise(hio(M(c)), e->hc, *rep, e->hLevel, e->hE1b, e->hStopped, nd, np, after);
    if (rc == 0u && M(c)->dioOnHoldRaise) { std::lock_guard<std::mutex> g(M(c)->mu); M(c)->reg[kRegPixelRateCntl] += 0x10000u; }   // 0.0.608: the clock change costs one DIO error
    return rc;
}
static uint32_t h_hrel(void *c, bool restoreBad, n48dal::HoldRep *rep) { hnote(c); Env *e = M(c)->env; return n48dal::hold_release(hio(M(c)), e->hc, *rep, restoreBad); }
static uint32_t h_hstate(void *c) { Env *e = M(c)->env; return n48dal::hold_state_now(e->hc); }
// One trial on this thread (the trial thread of the model's clock).
static void run(Env &e, uint32_t row, uint32_t dwell) { tIsTrial = true; run_trial(e.hw, e.st, e.tm, row, dwell, &e.r); tIsTrial = false; e.join(); }
static size_t cap_pos(uint32_t abs) { for (uint32_t i = 0; i < kCapN; i++) if (kCap[i].abs == abs) return i; return (size_t)-1; }
static bool all_init(Env &e) { for (uint32_t i = 0; i < kCapN; i++) if (e.m.reg[kCap[i].abs] != e.m.init[kCap[i].abs]) return false; return true; }

// ---- T1: the tables ---------------------------------------------------------------------------------------------------------------------------------------
static void t1_tables() {
    expect_u("56 registers in the capture set", kCapN, 56);
    std::set<uint32_t> seen;
    bool uniq = true; for (uint32_t i = 0; i < kCapN; i++) uniq = uniq && seen.insert(kCap[i].abs).second;
    expect(uniq, "every capture address is unique");
    // every register the trial can write is inside the REAL dcn41 write allowlist (armed with the segment bases the card reports)
    struct dcn41_allow_state al; std::memset(&al, 0, sizeof(al));
    const uint32_t seg[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x02403c00u };
    expect_u("the allowlist arms with the card's segment bases", (uint64_t)dcn41_allow_init(&al, seg, 262144u), (uint64_t)DCN41_ALLOW_OK);
    bool allOk = true; for (uint32_t i = 0; i < kCapN; i++) allOk = allOk && dcn41_allow_write(&al, kCap[i].abs, 0u, "t1");
    expect(allOk, "every register of kCap is admitted by the real DCN write allowlist (nothing the trial writes is outside it)");
    for (uint32_t a : { 0x16282u, 0x16292u, 0x1629Au, 0x1628Au, 0x16273u, 0x16274u, 0x0u, 0x16E69u })
        expect(!dcn41_allow_write(&al, a, 0u, "t1"), "an SMU / DAL mailbox / indirect-window / clock dword stays refused");
    // the witnesses are never written
    for (uint32_t w : { kRegPixelRateCntl, kRegDpStream[0], kRegDpStream[1], kRegDpStream[2], kRegDpStream[3], kRegDp1Steer }) expect(seen.count(w) == 0, "a read-only witness is not in the capture set");
    // Linux's order: the DTO trio, then optc1_program_timing, then global sync, MSA, HUBP
    expect_u("the DTO trio leads", cap_find("DP_DTO0_PHASE") + cap_find("DP_DTO0_MODULO") * 10 + cap_find("OTG_PIXEL_RATE_DIV") * 100, 0 + 10 + 200);
    expect(cap_find("OTG0_OTG_H_TOTAL") < cap_find("OTG0_OTG_V_TOTAL") && cap_find("OTG0_OTG_V_TOTAL") < cap_find("OTG0_OTG_V_TOTAL_MIN") && cap_find("OTG0_OTG_V_TOTAL_MAX") < cap_find("OTG0_OTG_V_SYNC_A") &&
           cap_find("OTG0_OTG_V_BLANK_START_END") < cap_find("OTG0_OTG_V_SYNC_A_CNTL") && cap_find("OTG0_OTG_V_SYNC_A_CNTL") < cap_find("OTG0_OTG_VSTARTUP_PARAM"), "optc1_program_timing order (H, V_TOTAL, MIN/MAX, V_SYNC, V_BLANK, V_SYNC_CNTL) then global sync");
    expect(cap_find("OTG0_OTG_VSTARTUP_PARAM") < cap_find("OTG0_OTG_VUPDATE_PARAM") && cap_find("OTG0_OTG_VUPDATE_PARAM") < cap_find("OTG0_OTG_VREADY_PARAM") && cap_find("OTG0_OTG_VREADY_PARAM") < cap_find("OTG0_OTG_PSTATE_REGISTER") &&
           cap_find("OTG0_OTG_PSTATE_REGISTER") < cap_find("VTG0_CONTROL") && cap_find("VTG0_CONTROL") < cap_find("OTG0_OTG_H_TIMING_CNTL") && cap_find("OTG0_OTG_H_TIMING_CNTL") < cap_find("DP1_DP_MSA_TIMING_PARAM1") &&
           cap_find("DP1_DP_MSA_TIMING_PARAM4") < cap_find("HUBPREQ0_BLANK_OFFSET_0"), "optc401_program_global_sync order, then the MSA, then the HUBP set");
    // rows
    const RowInfo r50 = row_info(50), r120 = row_info(120), r60 = row60();
    expect_u("row 50 writes exactly the DTO trio", r50.n, 3);
    expect(r50.idx[0] == 0 && r50.idx[1] == 1 && r50.idx[2] == 2, "row 50 = PHASE, MODULO, RATE_DIV");
    bool noDlg = true; for (uint32_t k = 0; k < r50.n; k++) noDlg = noDlg && kCap[r50.idx[k]].abs < 0x200u;
    expect(noDlg, "row 50 writes NO DLG / TTU / timing register (the live 60 Hz block stays: the safe direction)");
    expect_u("row 120 writes the whole set bar OTG_H_TIMING_CNTL (census c120-pre-1)", r120.n, kCapN - 1u);
    { bool has = false; for (uint32_t k = 0; k < r120.n; k++) has = has || kCap[r120.idx[k]].abs == 0x4feeu; expect(!has && cap_has_abs(0x4feeu) && kCap[18].abs == 0x4feeu, "row 120 never writes 0x4fee (it stays in the capture set, out of every write set)"); for (uint32_t rr : { 1u, 2u, 50u }) { const RowInfo q = row_info(rr); bool h = false; for (uint32_t k = 0; k < q.n; k++) h = h || kCap[q.idx[k]].abs == 0x4feeu; expect(!h, "no other row writes 0x4fee either"); } }
    bool asc = true; for (uint32_t k = 1; k < r120.n; k++) asc = asc && r120.idx[k - 1] < r120.idx[k];
    expect(asc, "row 120's index list is ascending = the write order");
    expect_u("row 0 is not a row", row_info(0).row, 0); expect_u("row 60 is not a trial row", row_info(60).row, 0); expect_u("row 51 is not a row", row_info(51).row, 0);
    // the 60 Hz golden equals the census of the live hardware (an internal design note s2d0-census-1) on every validated register
    struct { const char *n; uint32_t v; } cen[] = { { "DP_DTO0_PHASE", 0x0e64ff60u }, { "DP_DTO0_MODULO", 0x2aea5400u }, { "OTG0_OTG_H_TOTAL", 0x00000a9fu }, { "OTG0_OTG_H_BLANK_START_END", 0x00700a70u },
        { "OTG0_OTG_H_SYNC_A", 0x00200000u }, { "OTG0_OTG_V_TOTAL", 0x000005c8u }, { "OTG0_OTG_V_BLANK_START_END", 0x002605c6u }, { "OTG0_OTG_V_SYNC_A", 0x00050000u },
        { "DP1_DP_MSA_TIMING_PARAM1", 0x0aa005c9u }, { "DP1_DP_MSA_TIMING_PARAM2", 0x00700026u }, { "DP1_DP_MSA_TIMING_PARAM3", 0x00200005u }, { "DP1_DP_MSA_TIMING_PARAM4", 0x0a0005a0u } };
    for (auto &c : cen) { const uint32_t i = cap_find(c.n); expect(i != 0xFFFFFFFFu && (tab::kGold60[i] & kCap[i].mask) == (c.v & kCap[i].mask), c.n); }
    expect_u("the 60 Hz golden's DPDTO0_INT is 0 (the census)", tab::kGold60[kIdxRateDiv] & 0x1Eu, 0);
    // DTO convention: pixclk = INT * modulo + phase
    expect_u("dcn401 DTO convention: 0 * 720M + 241.5M", dto_pixclk_hz(0, 720000000u, 241500000u), 241500000u);
    expect_u("... and a nonzero integer part adds modulo units", dto_pixclk_hz(1, 720000000u, 5u), 720000005ull);
    expect_u("the golden 60 Hz phase register IS the pixel clock in Hz", tab::kGold60[kIdxDtoPhase], 241500000u);
    expect_u("the golden 50 Hz phase register IS the pixel clock in Hz", tab::kGold50[kIdxDtoPhase], 201000000u);
    expect_u("the golden 120 Hz phase register IS the pixel clock in Hz", tab::kGold120[kIdxDtoPhase], 497750000u);
    expect_u("all rows share the 720 MHz DTO reference (the census modulo)", tab::kGold60[kIdxDtoModulo] == 720000000u && tab::kGold50[kIdxDtoModulo] == 720000000u && tab::kGold120[kIdxDtoModulo] == 720000000u, 1);
    expect_u("the goldens' OTG_H_TOTAL / V_TOTAL registers hold total - 1", (tab::kGold120[kIdxVTotal] & 0x7FFF) + 1, 1525);
    expect_u("the 60 Hz raster is 2720 x 1481", ((tab::kGold60[kIdxHTotal] & 0x7FFF) + 1) * 10000ull + ((tab::kGold60[kIdxVTotal] & 0x7FFF) + 1), 2720ull * 10000 + 1481);
    // the refresh each row must show, millihertz (integer)
    expect_u("60 Hz row: 59.950 Hz", expect_mhz(r60), 59950); expect_u("50 Hz row: 49.896 Hz", expect_mhz(r50), 49896); expect_u("120 Hz row: 119.997 Hz", expect_mhz(r120), 119997);
    expect(rate_within(49896, 49896, 10) && rate_within(49500, 49896, 10) && !rate_within(49300, 49896, 10) && rate_within(50300, 49896, 10) && !rate_within(50500, 49896, 10), "1 % window around 49.9 Hz");
    expect(!rate_within(59950, 49896, 10) && !rate_within(0, 49896, 10) && !rate_within(49896, 0, 10), "the 60 Hz rate is not the 50 Hz rate; zero is never within");
    // the RMW is Linux's: only the mask bits change
    expect_u("rmw keeps the bits outside the mask", rmw(0xAAAAAAAAu, 0x0000001Eu, 0xFFFFFFFFu), 0xAAAAAABEu);
    expect_u("rmw with a full mask is the value", rmw(0x12345678u, 0xFFFFFFFFu, 0xCAFEBABEu), 0xCAFEBABEu);
}

// ---- T2: clock sufficiency (the real DID table) ----------------------------------------------------------------------------------------------------------------
static void t2_clocks() {
    const RowInfo r50 = row_info(50), r120 = row_info(120);
    const n48dal::Decoded live = n48dal::decode_clocks(0x2d, 0x41, 0x41, 0x417f4141u);
    expect_u("E1 census: DISPCLK DID 0x41 at VCO 4500 MHz is 272.727 MHz", live.dispKhz, 272727); expect_u("... DPPCLK the same", live.dppKhz, 272727);
    expect(live.dentAgree && live.chgDone && live.ok, "the census DENTIST value agrees with the DFS readback and has CHG_DONE");
    expect(clocks_ok(r50, live), "row 50 (needs 204.5 / 202.2 MHz) is admitted at 272.73 MHz");
    expect(!clocks_ok(r120, live), "row 120 (needs 514.3 / 500.0 MHz) is REFUSED at 272.73 MHz");
    expect_u("row 120 needs DISPCLK 514285 kHz", r120.needDispKhz, 514285); expect_u("... and DPPCLK 500000 kHz", r120.needDppKhz, 500000);
    expect_u("DID 0x23 = 514285 kHz (the DML edge)", n48dal::did_khz(4500000u, 0x23), 514285); expect_u("DID 0x24 = 500000 kHz", n48dal::did_khz(4500000u, 0x24), 500000);
    expect_u("DID 0x64 = 125 MHz, NOT 500 (design C1)", n48dal::did_khz(4500000u, 0x64), 125000);
    expect(clocks_ok(r120, Env::fast_dec(0x23, 0x24)), "row 120 is admitted at exactly DISPCLK 514285 / DPPCLK 500000 kHz (>=, not >)");
    expect(!clocks_ok(r120, Env::fast_dec(0x24, 0x24)), "DISPCLK 500 MHz is short of 514.285");
    expect(!clocks_ok(r120, Env::fast_dec(0x23, 0x25)), "DPPCLK 486.5 MHz (DID 0x25) is short of 500");
    expect(!clocks_ok(r120, Env::fast_dec(0x64, 0x64)), "the DID 0x64 trap: 125 MHz, refused");
    expect(clocks_ok(r120, Env::fast_dec(0x22, 0x22)) && clocks_ok(r120, Env::fast_dec(0x21, 0x21)), "faster clocks (529.4 / 545.5 MHz) admit row 120");
    n48dal::Decoded d = Env::fast_dec(0x23, 0x24);
    d.dentAgree = false; expect(!clocks_ok(r120, d), "DENTIST disagreeing with the DFS view refuses");
    d = Env::fast_dec(0x23, 0x24); d.chgDone = false; expect(!clocks_ok(r120, d), "CHG_DONE missing refuses");
    d = Env::fast_dec(0x23, 0x24); d.ok = false; expect(!clocks_ok(r120, d), "an undecodable readback refuses");
    expect(!clocks_ok(r50, n48dal::decode_clocks(0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu)), "all-ones registers refuse even the low row");
    expect(clocks_ok(r50, Env::fast_dec(0x4c, 0x4c)) && !clocks_ok(r50, Env::fast_dec(0x4d, 0x4d)), "row 50: DID 0x4c (204545 kHz, the DML edge) is admitted, DID 0x4d (200000 kHz) is refused");
    // end to end: the engine at the live clocks. 0.0.607: row 120 no longer denies on the DFS readback before the trial - it raises its own clocks (the hold) and judges the readback after the raise.
    Env e; e.bind(); e.st.step50Passed = true;
    run(e, 120, 0);
    expect_u("engine: row 120 without its prerequisites (resync, row-2 proof) -> DENIED", e.r.verdict, N48N_MODE_V_DENIED); expect_u("... reason STEP_DOWN (the resync prerequisite)", e.r.deny, N48N_MODE_D_STEP_DOWN);
    expect_u("... nothing written", e.m.writes.size(), 0); expect_u("... DISPCLK reported (the live 272.73 MHz)", e.r.disp_khz, 272727); expect_u("... need reported", e.r.need_disp_khz, 514285);
    expect(e.m.holdCalls == 0 && e.m.holdMsgs == 0, "... and no clock message was sent for a denied trial");
    expect_u("... a prerequisite refusal does not latch", e.r.latched, 0);
}

// ---- T3: the deny matrix ------------------------------------------------------------------------------------------------------------------------------------
static Pre ok_pre(uint32_t row = 50) {
    Pre p{}; p.row = row; p.dwellMs = 1000; p.gateOk = p.armed = p.goldenValid = p.otgOk = p.baselineOk = p.streamOk = p.driftFree = p.clocksOk = p.framesOk = true; p.step50Passed = true; return p;
}
static void t3_deny() {
    expect_u("all fine: go", deny_check(ok_pre(50)), 0); expect_u("all fine, row 120: go", deny_check(ok_pre(120)), 0);
    Pre p = ok_pre(); p.row = 60; expect_u("row 60", deny_check(p), N48N_MODE_D_BAD_ROW);
    p = ok_pre(); p.row = 0; expect_u("row 0", deny_check(p), N48N_MODE_D_BAD_ROW);
    p = ok_pre(); p.dwellMs = 30000; expect_u("dwell 30000 is the maximum and passes", deny_check(p), 0);
    p = ok_pre(); p.dwellMs = 30001; expect_u("dwell 30001", deny_check(p), N48N_MODE_D_BAD_DWELL);
    p = ok_pre(); p.latched = true; expect_u("latched", deny_check(p), N48N_MODE_D_LATCHED);
    p = ok_pre(); p.gateOk = false; expect_u("gate", deny_check(p), N48N_MODE_D_GATE);
    p = ok_pre(); p.busy = true; expect_u("busy", deny_check(p), N48N_MODE_D_BUSY);
    p = ok_pre(); p.armed = false; expect_u("not armed", deny_check(p), N48N_MODE_D_NOT_ARMED);
    p = ok_pre(); p.planeAcquired = true; expect_u("plane acquired", deny_check(p), N48N_MODE_D_ACQUIRED);
    p = ok_pre(); p.inUse = true; expect_u("in use", deny_check(p), N48N_MODE_D_IN_USE);
    p = ok_pre(); p.goldenValid = false; expect_u("no golden", deny_check(p), N48N_MODE_D_GOLDEN);
    p = ok_pre(); p.otgOk = false; expect_u("otg", deny_check(p), N48N_MODE_D_OTG);
    p = ok_pre(); p.baselineOk = false; expect_u("baseline", deny_check(p), N48N_MODE_D_BASELINE);
    p = ok_pre(); p.streamOk = false; expect_u("stream", deny_check(p), N48N_MODE_D_STREAM);
    p = ok_pre(); p.driftFree = false; expect_u("drift", deny_check(p), N48N_MODE_D_DRIFT);
    p = ok_pre(); p.clocksOk = false; expect_u("clocks", deny_check(p), N48N_MODE_D_CLOCKS);
    p = ok_pre(120); p.step50Passed = false; expect_u("row 120 before a passed row 50", deny_check(p), N48N_MODE_D_STEP_DOWN);
    p = ok_pre(50); p.step50Passed = false; expect_u("row 50 never needs a step down", deny_check(p), 0);
    p = ok_pre(120); p.step50Passed = false; p.clocksOk = false; expect_u("clocks are answered before the step-down rule", deny_check(p), N48N_MODE_D_CLOCKS);
    p = ok_pre(120); p.row120Off = true; expect_u("row 120 hard-denied", deny_check(p), N48N_MODE_D_ROW120_OFF);
    p = ok_pre(50); p.row120Off = true; expect_u("the row-120 switch does not touch row 50", deny_check(p), 0);
    expect(!kProd.row120, "PRODUCTION timing has row 120 OFF (the one-line constant)"); expect_u("deny code 19", N48N_MODE_D_ROW120_OFF, 19);
    { Env e; e.bind(); e.fast_clocks(); e.st.step50Passed = true; e.tm = kProd; run(e, 120, 0);
      expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_ROW120_OFF && e.m.writes.empty() && e.m.reads == kCapN + kExtN && e.st.busy == 0, "engine with the production timing: row 120 DENIED (ROW120_OFF), nothing read or written after the bind, even with fast clocks and a row-50 pass"); }
    { Env e; e.bind(); e.tm = kProd; run(e, 50, 0); expect_u("... and row 50 still runs with the production timing", e.r.verdict, N48N_MODE_V_PASS); }
    p = ok_pre(); p.nothing = true; expect_u("nothing to write", deny_check(p), N48N_MODE_D_NOTHING);
    p = ok_pre(); p.framesOk = false; expect_u("frames", deny_check(p), N48N_MODE_D_FRAMES);
    p = ok_pre(); p.latched = true; p.gateOk = false; p.clocksOk = false; expect_u("first reason wins: latched before gate before clocks", deny_check(p), N48N_MODE_D_LATCHED);
    // verdict precedence and the latch set
    Obs o{ false, false, false, false, false, false, false, false };
    expect_u("nothing wrong: PASS", judge(o), N48N_MODE_V_PASS);
    o = Obs{ false, false, false, false, false, false, false, true }; expect_u("rate only", judge(o), N48N_MODE_V_RATE);
    o = Obs{ false, false, false, false, false, false, true, true }; expect_u("fifo beats rate", judge(o), N48N_MODE_V_FIFO);
    o = Obs{ false, false, false, false, false, true, true, true }; expect_u("stream beats fifo", judge(o), N48N_MODE_V_STREAM);
    o = Obs{ false, false, false, false, true, true, true, true }; expect_u("stall beats stream", judge(o), N48N_MODE_V_STALL);
    o = Obs{ false, false, false, true, true, true, true, true }; expect_u("write beats stall", judge(o), N48N_MODE_V_WRITE);
    o = Obs{ false, false, true, true, true, true, true, true }; expect_u("abort beats write", judge(o), N48N_MODE_V_ABORT);
    o = Obs{ false, true, true, true, true, true, true, true }; expect_u("watchdog beats abort", judge(o), N48N_MODE_V_WATCHDOG);
    o = Obs{ true, true, true, true, true, true, true, true }; expect_u("a restore that did not verify beats everything", judge(o), N48N_MODE_V_RESTORE);
    for (uint32_t v = N48N_MODE_V_RATE; v <= N48N_MODE_V_ABORT; v++) expect(is_failure(v), "every failure verdict latches");
    expect(!is_failure(N48N_MODE_V_PASS) && !is_failure(N48N_MODE_V_DENIED) && !is_failure(0), "PASS and DENIED do not latch");
    // the watchdog's timing rule
    expect(!wd_due(1000, 5000, 900, 3000) && wd_due(5000, 5000, 4999, 3000) && wd_due(4000, 5000, 900, 3000) && !wd_due(3899, 5000, 900, 3000), "watchdog: due at the deadline or when the heartbeat is stale");
    expect_u("watchdog window = settle + measure + dwell + 2 s", wd_window_ms(kProd, 10000), 500 + 3000 + 10000 + 2000);
    expect(kProd.measureMs >= 2000u && kProd.slackMs == 2000u && kProd.settleMs > 0 && kProd.tolPermille == 10u, "production timing: rate windows >= 2 s, slack 2 s, tolerance 1 %");
    // a denied ENGINE run writes nothing, for each hardware-side reason
    struct Case { const char *what; uint32_t why; void (*set)(Env &); } cases[] = {
        { "gate", N48N_MODE_D_GATE, [](Env &e) { e.m.gate = false; } }, { "not armed", N48N_MODE_D_NOT_ARMED, [](Env &e) { e.m.armed = false; } },
        { "plane acquired", N48N_MODE_D_ACQUIRED, [](Env &e) { e.m.planeAcq = true; } }, { "in use", N48N_MODE_D_IN_USE, [](Env &e) { e.m.inUse = true; } },
        { "otg", N48N_MODE_D_OTG, [](Env &e) { e.m.otgOk = false; } },
        { "unreadable register", N48N_MODE_D_BASELINE, [](Env &e) { e.m.unreadable.insert(kCap[5].abs); } },
        { "baseline: another mode (V_TOTAL)", N48N_MODE_D_BASELINE, [](Env &e) { e.m.reg[kCap[kIdxVTotal].abs] = 0x5f4; } },
        { "baseline: DIO FIFO error already set", N48N_MODE_D_BASELINE, [](Env &e) { e.m.reg[kRegPixelRateCntl] |= 0x4000u; } },
        { "baseline: steer FIFO overflow flag already set", N48N_MODE_D_BASELINE, [](Env &e) { e.m.reg[kRegDp1Steer] |= 0x10u; } },
        { "baseline: DIG FIFO error bits already set", N48N_MODE_D_BASELINE, [](Env &e) { e.m.reg[kRegDigFifoCtrl0] |= 0x20000000u; } },
        { "baseline: DTO disabled", N48N_MODE_D_BASELINE, [](Env &e) { e.m.reg[kRegPixelRateCntl] &= ~0x10u; } },
        { "stream inactive", N48N_MODE_D_STREAM, [](Env &e) { e.m.reg[kRegDpStream[1]] = 0x00000201u; } },
        { "another DP stream enabled", N48N_MODE_D_STREAM, [](Env &e) { e.m.reg[kRegDpStream[0]] = 0x00010201u; } },
        { "clocks unreadable", N48N_MODE_D_CLOCKS, [](Env &e) { e.m.clkRead = false; } },
        { "watchdog thread refused", N48N_MODE_D_WATCHDOG, [](Env &e) { e.m.wdStartFail = true; } },
    };
    for (auto &c : cases) {
        Env e; e.bind(); c.set(e);
        run(e, 50, 1000);
        char msg[160];
        std::snprintf(msg, sizeof(msg), "engine denies (%s): DENIED with the right reason", c.what);
        expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == c.why, msg);
        std::snprintf(msg, sizeof(msg), "engine denies (%s): NOTHING written", c.what); expect(e.m.writes.empty(), msg);
        std::snprintf(msg, sizeof(msg), "engine denies (%s): no latch, busy released", c.what); expect(e.r.latched == 0 && e.st.busy == 0 && !e.st.latched, msg);
    }
    { Env e; e.bind(); run(e, 51, 0); expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_BAD_ROW && e.m.writes.empty() && e.m.reads == kCapN + kExtN, "row 51: BAD_ROW, only the bind's capture read anything, nothing written"); }
    { Env e; e.bind(); run(e, 50, 30001); expect(e.r.deny == N48N_MODE_D_BAD_DWELL && e.m.writes.empty(), "dwell 30001: BAD_DWELL, nothing written"); }
    { Env e; e.bind(); e.st.latched = true; run(e, 50, 100); expect(e.r.deny == N48N_MODE_D_LATCHED && e.m.writes.empty() && e.r.latched == 1 && (e.r.flags & N48N_MODE_F_LATCHED), "latched: LATCHED, nothing written"); }
    { Env e; e.bind(); run(e, 120, 0); expect(e.r.deny == N48N_MODE_D_STEP_DOWN && e.m.writes.empty() && e.m.holdMsgs == 0, "row 120 at the live clocks and no row-50 pass: STEP_DOWN (0.0.607: the clocks are the hold's business, not a pre-trial refusal), nothing written, nothing sent to the PMFW"); }
    { Env e; e.bind(); e.fast_clocks(); run(e, 120, 0); expect(e.r.deny == N48N_MODE_D_STEP_DOWN && e.m.writes.empty() && e.m.holdMsgs == 0, "row 120 with fast clocks but no row-50 pass: STEP_DOWN, nothing written"); }
    { Env e; e.bind(); e.m.reg[kCap[cap_find("DP_DTO0_PHASE")].abs] ^= 0x100u; run(e, 50, 0); expect(e.r.deny == N48N_MODE_D_BASELINE && e.m.writes.empty(), "a moved DTO phase is not the 60 Hz census: BASELINE"); }
    { Env e; e.st.step50Passed = e.st.resyncPassed = e.st.row2Proven = true; e.bind(); e.m.reg[kCap[cap_find("HUBPREQ0_NOM_PARAMETERS_1")].abs] ^= 0x1u; run(e, 120, 0);
      expect(e.r.deny == N48N_MODE_D_DRIFT && e.m.writes.empty() && e.m.holdMsgs == 0 && e.r.bad_abs == kCap[cap_find("HUBPREQ0_NOM_PARAMETERS_1")].abs, "a DLG register that drifted from the golden copy denies row 120 (DRIFT) and names it; the clocks were never raised"); }
    { Env e; e.bind(); e.m.reg[kCap[cap_find("HUBPREQ0_NOM_PARAMETERS_1")].abs] ^= 0x1u; run(e, 50, 0); expect(e.r.verdict == N48N_MODE_V_PASS, "the same drift does not matter to row 50 (it never writes the DLG block)"); }
    { Env e; e.m.unreadable.insert(kCap[9].abs); e.bind(); run(e, 50, 0); expect(e.r.deny == N48N_MODE_D_GOLDEN || e.r.deny == N48N_MODE_D_BASELINE, "a register that reads all-ones makes no golden copy"); expect(!e.st.goldenValid, "... and no copy is recorded"); }
}

// ---- T4: whole trials --------------------------------------------------------------------------------------------------------------------------------------
static void t4_trials() {
    // ---- PASS, row 50 ----
    {
        Env e; e.bind(); run(e, 50, 4000);
        const n48n_mode_result &r = e.r;
        expect_u("row 50 PASS", r.verdict, N48N_MODE_V_PASS); expect_u("... deny 0", r.deny, 0); expect_u("... no latch", r.latched, 0);
        expect_u("... exactly ONE register written (the DTO phase; modulo and INT already hold the row's values)", r.nwrite, 1); expect_u("... two skipped", r.nskip, 2);
        expect_u("... it is DP_DTO0_PHASE", r.first_write_abs, 0x141u);
        expect_u("... the model saw 1 write + 1 restore write", e.m.writes.size(), 2);
        expect(e.m.writes.size() == 2 && e.m.writes[0].second == 201000000u && e.m.writes[1].second == e.m.init[0x141], "the write is 201,000,000 and the restore is the census 241.5 MHz");
        expect(r.flags & N48N_MODE_F_RESTORED, "RESTORED"); expect(r.flags & N48N_MODE_F_REGS_OK, "REGS_OK"); expect(r.flags & N48N_MODE_F_RATE_TRIAL_OK, "RATE_TRIAL_OK"); expect(r.flags & N48N_MODE_F_RATE_AFTER_OK, "RATE_AFTER_OK");
        expect(r.flags & N48N_MODE_F_DLG_LIVE, "DLG_LIVE flag (row 50 keeps the live 60 Hz DLG/TTU)"); expect(r.flags & N48N_MODE_F_GOLDEN_AT_BIND, "GOLDEN_AT_BIND"); expect(r.flags & N48N_MODE_F_STEP_DOWN_DONE, "STEP_DOWN_DONE");
        expect(!(r.flags & N48N_MODE_F_UNDERFLOW_UNREAD) && !(r.flags & N48N_MODE_F_UNDERFLOW_SEEN), "0.0.606: both underflow registers read fine and showed nothing: no UNREAD, no SEEN"); expect(!(r.flags & N48N_MODE_F_WATCHDOG) && !(r.flags & N48N_MODE_F_RESTORE_FAILED), "no watchdog, no restore failure");
        expect(r.rate_trial_mhz >= 49400 && r.rate_trial_mhz <= 50400, "the measured trial rate is 49.9 Hz within 1 %");
        expect(r.rate_after_mhz >= 59350 && r.rate_after_mhz <= 60550, "the rate after the restore is 59.95 Hz within 1 %");
        expect(r.rate_before_mhz >= 59350 && r.rate_before_mhz <= 60550, "the baseline was 59.95 Hz");
        expect(r.ms_trial >= 2000 && r.ms_after >= 2000, "both rate windows are at least 2 s");
        expect_u("the windows are 3 s", r.measure_ms, 3000);
        expect(r.frames_trial >= 140 && r.frames_trial <= 160, "about 150 frames in the 50 Hz window");
        expect(r.dwell_done_ms >= 4000, "the dwell was held (4000 ms asked)");
        expect(r.fifo_before == 0 && r.fifo_after == 0 && r.errcnt_before == 1 && r.errcnt_after == 1, "DIO FIFO error 0 and error count 1 unchanged");
        expect(r.dto_phase[0] == e.m.init[0x141] && r.dto_phase[1] == 201000000u && r.dto_phase[2] == e.m.init[0x141], "DTO phase before / during / after");
        expect(all_init(e), "every register of the golden set is BIT-EXACT the same after the trial (junk bits kept)");
        expect_u("the golden copy holds every capture register", r.golden_n, kCapN); expect(e.st.step50Passed && !e.st.latched && e.st.busy == 0, "state: step50 passed, no latch, busy released");
        expect(e.st.doneId == e.st.trialId, "the watchdog is told the trial is over");
    }
    // ---- row 120 (0.0.607): the whole sequence, with the clock hold, is t12_row120() below ----
    // ---- every failure verdict, its restore, the latch ----
    struct Fail { const char *what; uint32_t verdict; void (*set)(Env &); bool regsClean; };
    Fail fails[] = {
        { "RATE: the DTO write does not change the pixel clock", N48N_MODE_V_RATE, [](Env &e) { e.m.ignoreDto = true; }, true },
        { "FIFO: DIO_ERROR_COUNT keeps growing during the trial", N48N_MODE_V_FIFO, [](Env &e) { e.m.fifoSteady = true; }, true },
        { "STREAM: the DP stream drops when the DTO changes", N48N_MODE_V_STREAM, [](Env &e) { e.m.streamDownOnDto = true; }, true },
        { "STALL: the frame counter stops when the DTO changes", N48N_MODE_V_STALL, [](Env &e) { e.m.stallOnDto = true; }, true },
    };
    for (auto &f : fails) {
        Env e; e.bind(); f.set(e); run(e, 50, 3000);
        char msg[200];
        std::snprintf(msg, sizeof(msg), "%s: verdict", f.what); expect_u(msg, e.r.verdict, f.verdict);
        std::snprintf(msg, sizeof(msg), "%s: restored, registers exact, rate back", f.what); expect((e.r.flags & N48N_MODE_F_RESTORED) && all_init(e) && e.r.mismatch == 0, msg);
        std::snprintf(msg, sizeof(msg), "%s: latched, flag and state", f.what); expect(e.r.latched == 1 && (e.r.flags & N48N_MODE_F_LATCHED) && e.st.latched && e.st.latchVerdict == f.verdict, msg);
        std::snprintf(msg, sizeof(msg), "%s: the dwell was not held after the failure", f.what); expect(e.r.dwell_done_ms < 3000, msg);
        std::snprintf(msg, sizeof(msg), "%s: row 50 did not count as passed", f.what); expect(!e.st.step50Passed && !(e.r.flags & N48N_MODE_F_STEP_DOWN_DONE), msg);
        e.m.writes.clear();
        run(e, 50, 100);
        std::snprintf(msg, sizeof(msg), "%s: the next trial is DENIED (latched) and writes nothing", f.what); expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_LATCHED && e.m.writes.empty(), msg);
        e.m.writes.clear(); run(e, 120, 100); expect(e.r.deny == N48N_MODE_D_LATCHED, "the latch refuses row 120 too");
    }
    // the same four failures with NO dwell: the checks right after the rate window must catch them on their own
    for (auto &f : fails) {
        Env e; e.bind(); f.set(e); run(e, 50, 0);
        char msg[200]; std::snprintf(msg, sizeof(msg), "%s (dwell 0): verdict", f.what); expect_u(msg, e.r.verdict, f.verdict);
        std::snprintf(msg, sizeof(msg), "%s (dwell 0): restored exactly", f.what); expect((e.r.flags & N48N_MODE_F_RESTORED) && all_init(e), msg);
    }
    { // 0.0.607: a clock that ALREADY reads at (or above) the row's need is a floor this hold did not raise: the hold refuses the baseline, the trial is DENIED (HOLD), nothing is written or sent
        Env e; e.bind(); e.st.step50Passed = e.st.resyncPassed = e.st.row2Proven = true; e.m.clk = Env::fast_dec(0x24, 0x23); run(e, 120, 0);
        expect(e.r.deny == N48N_MODE_D_HOLD && e.r.ext.hold_rc == n48dal::kHrBaseline && e.m.writes.empty() && e.m.holdMsgs == 0, "engine: DPPCLK already above its need (DISPCLK 500 MHz) is a baseline the hold refuses");
        Env f; f.bind(); f.st.step50Passed = f.st.resyncPassed = f.st.row2Proven = true; f.m.clk = Env::fast_dec(0x23, 0x25); run(f, 120, 0);
        expect(f.r.deny == N48N_MODE_D_HOLD && f.r.ext.hold_rc == n48dal::kHrBaseline && f.m.writes.empty() && f.m.holdMsgs == 0, "engine: DISPCLK already above its need is refused too");
    }
    { // the write transition's DIO errors are REPORTED, not judged; only growth during measure + dwell is
        Env e; e.bind(); e.m.dioOnWrite = true; run(e, 50, 1000);
        expect(e.r.verdict == N48N_MODE_V_PASS && e.r.dio_write_errs == 1 && e.r.dio_trial_errs == 0 && e.r.dio_restore_errs == 0 && !(e.r.flags & N48N_MODE_F_HEALTH_BAD), "a DIO error at the WRITE transition alone: PASS, counted separately (write 1, trial 0, restore 0)");
        expect(e.r.errcnt_before == 1 && e.r.errcnt_after == 2, "the totals still show 1 -> 2");
    }
    { // the restore transition IS judged (MUST 1)
        struct R { const char *what; void (*set)(Env &); } rs[] = {
            { "DIO_ERROR_COUNT grows at the restore", [](Env &e) { e.m.dioOnRestore = true; } },
            { "DP1_DP_STEER_FIFO overflow flag stuck after the restore", [](Env &e) { e.m.steerStuck = true; } },
            { "DIG1_DIG_FIFO_CTRL0 error bits stuck after the restore", [](Env &e) { e.m.digStuck = true; } },
            { "DIO_FIFO_ERROR stuck after the restore", [](Env &e) { e.m.fifoBitStuck = true; } } };
        for (auto &x : rs) {
            Env e; e.bind(); x.set(e); run(e, 50, 500);
            char msg[200];
            std::snprintf(msg, sizeof(msg), "%s: verdict RESTORE, never PASS", x.what); expect_u(msg, e.r.verdict, N48N_MODE_V_RESTORE);
            std::snprintf(msg, sizeof(msg), "%s: HEALTH_BAD, RESTORE_FAILED, latched, row 50 not passed", x.what);
            expect((e.r.flags & N48N_MODE_F_HEALTH_BAD) && (e.r.flags & N48N_MODE_F_RESTORE_FAILED) && e.r.latched == 1 && !e.st.step50Passed && !(e.r.flags & N48N_MODE_F_STEP_DOWN_DONE), msg);
            std::snprintf(msg, sizeof(msg), "%s: the registers themselves were restored", x.what); expect(all_init(e) && e.r.mismatch == 0 && (e.r.flags & N48N_MODE_F_REGS_OK), msg);
        }
        Env e; e.bind(); e.m.dioOnRestore = true; run(e, 50, 500);
        expect(e.r.dio_restore_errs == 1 && e.r.dio_write_errs == 0 && e.r.dio_trial_errs == 0, "the restore-transition growth is reported in its own field");
        Env f; f.bind(); f.m.digStuck = true; run(f, 50, 500); expect((f.r.dig_fifo_after & 0x30000000u) != 0, "the raw DIG FIFO word is reported");
        Env g; g.bind(); f.m.digStuck = false; run(g, 50, 500); expect(g.r.verdict == N48N_MODE_V_PASS && g.r.dig_fifo_after == 0x1Du && g.r.steer_after == 1, "a healthy restore reports 0x1d / 1 and passes");
    }
    { // ABORT: the VERIFY window still runs after the abort (SHOULD 3), and a restore-transition error shows as RESTORE
        for (int variant = 0; variant < 2; variant++) {
            Env e; e.bind(); if (variant) e.m.dioOnRestore = true;
            std::thread t([&e] { run(e, 50, 20000); });
            for (int i = 0; i < 20000; i++) { { std::lock_guard<std::mutex> g(e.m.mu); if (e.m.vnow > 5000000ull + 2000000ull + 500000ull + 3000000ull + 1000000ull) break; } std::this_thread::sleep_for(std::chrono::microseconds(100)); }
            request_abort(e.hw, e.st);
            t.join(); e.join();
            if (!variant) {
                expect(e.r.verdict == N48N_MODE_V_ABORT && (e.r.flags & N48N_MODE_F_RESTORED) && (e.r.flags & N48N_MODE_F_RATE_AFTER_OK) && e.r.ms_after >= 2000 && e.r.frames_after > 100, "ABORT with a clean restore: the VERIFY window ran (>= 2 s, 60 Hz) and the verdict is ABORT, not RESTORE");
                expect(!e.st.abortReq, "the abort request is cleared once served");
            } else expect(e.r.verdict == N48N_MODE_V_RESTORE && e.r.dio_restore_errs == 1 && (e.r.flags & N48N_MODE_F_RATE_AFTER_OK), "ABORT while the restore transition errs: RESTORE (the worst news wins), the rate window still ran");
        }
    }
    { // WRITE: the allowlist refuses the DTO phase itself -> nothing changed, the refused index is still in the restore list
        Env e; e.bind(); e.m.refuseWr.insert(0x141); run(e, 50, 3000);
        expect_u("WRITE verdict", e.r.verdict, N48N_MODE_V_WRITE); expect(e.st.writeFailed, "state records the failed write"); expect_u("the refused write is counted as attempted", e.r.nwrite, 1);
        expect(e.st.nWritten == 1 && e.st.written[0] == kIdxDtoPhase, "the refused register is in the restore list (recorded BEFORE the write is issued)");
        expect(all_init(e) && (e.r.flags & N48N_MODE_F_RESTORED) && e.r.latched == 1, "restored, latched"); expect_u("no measurement was taken after a failed write", e.r.frames_trial, 0);
    }
    // (the mid-row WRITE refusal of row 120 is a case of t12_row120's failure matrix)
    { // RESTORE: a restore write is dropped by the hardware -> mismatch, RESTORE verdict, two passes, the register named
        Env e; e.bind(); e.m.dtoStaysAfterRestore = true; run(e, 50, 1000);
        expect_u("RESTORE verdict when the DTO restore does not take", e.r.verdict, N48N_MODE_V_RESTORE);
        expect(e.r.flags & N48N_MODE_F_RESTORE_FAILED, "RESTORE_FAILED flag"); expect(!(e.r.flags & N48N_MODE_F_RESTORED), "not RESTORED");
        expect(e.r.mismatch == 1 && e.r.bad_abs == 0x141u && e.r.bad_have == 201000000u && e.r.bad_want == e.m.init[0x141], "one register differs, and it is named with both values");
        expect_u("two restore passes were tried", e.r.restore_tries, 2); expect(e.r.latched == 1, "latched"); expect(!(e.r.flags & N48N_MODE_F_RATE_AFTER_OK), "the 60 Hz rate did not come back");
    }
    { // RESTORE: the allowlist refuses the restore write itself (the forward write was accepted)
        Env e; e.bind(); e.m.refuseRestoreDto = true; run(e, 50, 500);
        expect_u("RESTORE verdict when the restore write is refused", e.r.verdict, N48N_MODE_V_RESTORE);
        expect(e.r.mismatch == 1 && e.r.bad_abs == 0x141u && (e.r.flags & N48N_MODE_F_RESTORE_FAILED) && e.r.latched == 1 && e.r.restore_tries == 2, "the DTO phase is named as still differing; two passes; latched");
    }
    { // control: a clean row 50 passes
        Env e; e.bind(); run(e, 50, 500); expect_u("control: a clean row 50 passes", e.r.verdict, N48N_MODE_V_PASS);
    }
    { // ABORT: dcnmode 0 / the kext stop asks during the dwell
        Env e; e.bind();
        std::thread t([&e] { run(e, 50, 20000); });
        // wait (real time) until the trial is in its dwell (it has written), then ask
        for (int i = 0; i < 20000; i++) { { std::lock_guard<std::mutex> g(e.m.mu); if (e.m.vnow > 5000000ull + 2000000ull + 500000ull + 3000000ull + 1000000ull) break; } std::this_thread::sleep_for(std::chrono::microseconds(100)); }
        request_abort(e.hw, e.st);
        t.join(); e.join();
        expect_u("ABORT verdict", e.r.verdict, N48N_MODE_V_ABORT); expect(all_init(e) && (e.r.flags & N48N_MODE_F_RESTORED), "restored"); expect(e.r.dwell_done_ms < 15000, "the dwell was cut short");
        expect(e.r.latched == 1, "an abort latches");
    }
    { // dwell 0 works, dwell 30000 is held
        Env e; e.bind(); run(e, 50, 0); expect(e.r.verdict == N48N_MODE_V_PASS && e.r.dwell_done_ms == 0, "dwell 0: PASS, no hold");
        Env f; f.bind(); run(f, 50, 30000); expect(f.r.verdict == N48N_MODE_V_PASS && f.r.dwell_done_ms >= 30000, "dwell 30000: PASS and held for 30 s");
    }
    { // busy: a second trial while one runs is denied and does not disturb it (the first is hung after its writes)
        Env e; e.bind(); e.m.hangAfterWrites = true;
        std::thread t([&e] { run(e, 50, 500); });
        for (int i = 0; i < 20000 && !e.m.hung.load(); i++) std::this_thread::sleep_for(std::chrono::microseconds(100));
        n48n_mode_result r2; std::memset(&r2, 0, sizeof(r2));
        run_trial(e.hw, e.st, e.tm, 50, 100, &r2);
        expect(r2.verdict == N48N_MODE_V_DENIED && r2.deny == N48N_MODE_D_BUSY, "a second trial while one runs: DENIED busy");
        expect_u("... and the first trial's busy flag is still set", __atomic_load_n(&e.st.busy, __ATOMIC_SEQ_CST), 1);
        { std::lock_guard<std::mutex> g(e.m.mu); e.m.release = true; } e.m.cv.notify_all();
        t.join(); e.join();
        expect_u("... the first trial still finishes", e.r.verdict == N48N_MODE_V_WATCHDOG || e.r.verdict == N48N_MODE_V_PASS || e.r.verdict == N48N_MODE_V_RATE, 1);
        expect_u("... and releases busy", __atomic_load_n(&e.st.busy, __ATOMIC_SEQ_CST), 0);
    }
    { // golden_take: never over a copy, never after a write
        Env e; e.bind(); const uint32_t g0 = e.st.golden[3]; e.m.reg[kCap[3].abs] ^= 0xFFu; expect(golden_take(e.hw, e.st, false) && e.st.golden[3] == g0, "a second golden_take keeps the first copy");
        Env f; f.st.everWrote = true; expect(!golden_take(f.hw, f.st, true) && !f.st.goldenValid, "no golden copy is taken after a write");
        Env g; run(g, 50, 500); expect(g.st.goldenValid && !g.st.goldenAtBind && !(g.r.flags & N48N_MODE_F_GOLDEN_AT_BIND) && g.r.verdict == N48N_MODE_V_PASS, "without a bind the first trial takes the copy (read-only, before any write)");
    }
}

// ---- T5: the watchdog thread ----------------------------------------------------------------------------------------------------------------------------------
static void wd_case(bool deadlineOnly) {
    Env e; e.bind(); e.m.hangAfterWrites = true;
    if (deadlineOnly) e.tm.staleMs = 100000000u;       // only the deadline can fire
    std::thread t([&e] { run(e, 50, 1000); });
    // the watchdog must restore while the trial thread is hung
    bool restored = false;
    for (int i = 0; i < 50000 && !restored; i++) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        std::lock_guard<std::mutex> g(gEng);
        restored = e.st.restoreDone && e.st.wdFired;
    }
    const bool hungAtThatPoint = e.m.hung.load();
    bool regsBack = false; { std::lock_guard<std::mutex> g(e.m.mu); regsBack = true; for (uint32_t i = 0; i < kCapN; i++) regsBack = regsBack && e.m.reg[kCap[i].abs] == e.m.init[kCap[i].abs]; }
    const size_t wrAtRelease = e.m.writes.size();
    { std::lock_guard<std::mutex> g(e.m.mu); e.m.release = true; } e.m.cv.notify_all();
    t.join(); e.join();
    const char *w = deadlineOnly ? "deadline" : "stale heartbeat";
    char msg[160];
    std::snprintf(msg, sizeof(msg), "watchdog (%s): it restored while the trial thread was still hung", w); expect(restored && hungAtThatPoint, msg);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): the registers were back BEFORE the trial thread woke", w); expect(regsBack, msg);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): the trial answers WATCHDOG", w); expect_u(msg, e.r.verdict, N48N_MODE_V_WATCHDOG);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): WATCHDOG flag, wd_fired 1, latched", w); expect((e.r.flags & N48N_MODE_F_WATCHDOG) && e.r.wd_fired == 1 && e.r.latched == 1, msg);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): the restore ran exactly ONCE (1 write + 1 restore write, none after the trial thread woke)", w); expect(wrAtRelease == 2 && e.m.writes.size() == 2, msg);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): registers exact after the trial", w); expect(all_init(e) && e.r.mismatch == 0, msg);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): the 60 Hz rate verified afterwards", w); expect(e.r.flags & N48N_MODE_F_RATE_AFTER_OK, msg);
    std::snprintf(msg, sizeof(msg), "watchdog (%s): busy released", w); expect_u(msg, __atomic_load_n(&e.st.busy, __ATOMIC_SEQ_CST), 0);
}
static void t5_watchdog() {
    wd_case(false); wd_case(true);
    // exclusivity of the claim
    {
        Env e; e.bind();
        // put the state in "wrote something" by hand
        e.st.nWritten = 1; e.st.written[0] = kIdxDtoPhase; e.m.reg[0x141] = 201000000u;
        expect(restore_run(e.hw, e.st, true), "the first claim runs the restore");
        expect(!restore_run(e.hw, e.st, false), "the second claim does NOT run it again");
        expect(!restore_run(e.hw, e.st, true), "nor a third");
        expect(e.m.reg[0x141] == e.m.init[0x141] && e.m.writes.size() == 1 && e.st.wdFired && e.st.regsOk, "one restore write, verified, wdFired recorded");
        expect(restore_wait(e.hw, e.st, 100, 10), "restore_wait sees restoreDone");
    }
    // both parties race for the claim: exactly one performs it
    for (int round = 0; round < 50; round++) {
        Env e; e.bind(); e.st.nWritten = 1; e.st.written[0] = kIdxDtoPhase; e.m.reg[0x141] = 201000000u;
        std::atomic<int> ran{0};
        std::thread a([&] { if (restore_run(e.hw, e.st, true)) ran++; }), b([&] { if (restore_run(e.hw, e.st, false)) ran++; });
        a.join(); b.join();
        if (ran.load() != 1 || e.m.writes.size() != 1) { expect(false, "a raced claim ran the restore other than exactly once"); break; }
        if (round == 49) expect(true, "50 raced claims: exactly one restore each");
    }
    // the watchdog body ends when its trial is over and never touches the hardware then
    {
        Env e; e.bind(); e.st.trialId = 7; e.st.doneId = 7; e.st.nWritten = 1; e.st.written[0] = kIdxDtoPhase; e.m.reg[0x141] = 201000000u;
        watchdog_body(e.hw, e.st, e.tm, 7);
        expect(e.m.writes.empty() && e.m.reg[0x141] == 201000000u, "a watchdog whose trial is done exits without restoring");
        Env f; f.bind(); f.st.trialId = 8; watchdog_body(f.hw, f.st, f.tm, 7);
        expect(f.m.writes.empty(), "a watchdog of an older trial exits when a newer one has begun");
    }
}

// ---- T6: `dcnmode 0` (restore_from_golden) ----------------------------------------------------------------------------------------------------------------
static void t6_full_restore() {
    Env e; e.bind();
    // perturb: a few masked bits in three registers, in three different groups (a crashed trial's leftovers)
    const uint32_t a = cap_find("DP_DTO0_PHASE"), b = cap_find("OTG0_OTG_V_TOTAL"), c = cap_find("HUBPREQ0_DST_DIMENSIONS");
    e.m.reg[kCap[a].abs] = 201000000u; e.m.reg[kCap[b].abs] = 0x5f4; e.m.reg[kCap[c].abs] ^= 0xF0u;
    uint32_t restored = 0; bool have = false;
    const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have);
    expect(have && bad == 0 && restored == 3, "dcnmode 0: exactly the three drifted registers are written back, none still differs");
    expect(all_init(e), "the golden set is bit-exact again");
    expect(e.m.writes.size() == 3 && cap_pos(e.m.writes[0].first) == c && cap_pos(e.m.writes[1].first) == b && cap_pos(e.m.writes[2].first) == a, "in REVERSE Linux order (HUBP, then timing, then the DTO)");
    e.m.writes.clear(); restored = 9;
    expect(restore_from_golden(e.hw, e.st, &restored, &have) == 0 && restored == 0 && e.m.writes.empty(), "a second dcnmode 0 on a restored machine writes nothing");
    Env n; have = true; restored = 9;
    expect(restore_from_golden(n.hw, n.st, &restored, &have) == 0 && !have && restored == 0 && n.m.writes.empty(), "no golden copy: nothing is written, and the caller is told");
    // a register that will not take the restore is reported
    Env s; s.bind(); s.m.reg[kCap[a].abs] = 201000000u; s.m.ignoreWr.insert(kCap[a].abs);
    expect(restore_from_golden(s.hw, s.st, &restored, &have) == 1, "a register that does not restore is counted");
    // running trial + dcnmode 0: the abort ends it
    request_abort(s.hw, s.st); expect(wait_idle(s.hw, s.st, 100, 10), "wait_idle returns at once when nothing runs");
    Env t; t.bind(); __atomic_store_n(&t.st.busy, 1u, __ATOMIC_SEQ_CST); request_abort(t.hw, t.st); expect(t.st.abortReq, "request_abort marks a running trial"); expect(!wait_idle(t.hw, t.st, 50, 10), "wait_idle is bounded when the trial does not finish");
    Env u; u.bind(); request_abort(u.hw, u.st); expect(!u.st.abortReq, "request_abort on an idle machine marks nothing (it would abort the NEXT trial)");
}

// ============================ 0.0.606: T7..T12 - the underflow read (P1), the DP1 resync (P3), the OTG update lock (P2), the wrapper, `dcnmode 0` =====================================
// The model above implements the registers of these sequences with their timing (STATUS drops 20 us after ENABLE = 0 and a steer reset before it leaves it stuck, the DIG FIFO reset completes after
// 20 us, MASTER_UPDATE_LOCK raises its status, V_TOTAL is double-buffered under DRR mode 2 / the lock and latches at a frame start, VERT_COUNT follows the effective total, the underflow bits
// are write-1 cleared), so a wrong order, a missing unlock or a broken field mask fails a check here. Hardware unknowns are model assumptions and are named where they matter.
typedef std::vector<std::pair<uint32_t, uint32_t>> Writes;
static void runf(Env &e, uint32_t row, uint32_t dwell, uint32_t tflags) { tIsTrial = true; run_trial(e.hw, e.st, e.tm, row, dwell, tflags, &e.r); tIsTrial = false; e.join(); }
struct Held { Env &e; explicit Held(Env &x) : e(x) { tIsTrial = true; e.hw.lock(e.hw.ctx); } ~Held() { e.hw.unlock(e.hw.ctx); tIsTrial = false; } };
static Writes wr_at(const Env &e, std::set<uint32_t> addrs) { Writes o; for (auto &w : e.m.writes) if (addrs.count(w.first)) o.push_back(w); return o; }
static bool only_in(const Env &e, std::set<uint32_t> allowed) { for (auto &w : e.m.writes) if (!allowed.count(w.first)) return false; return true; }
static size_t count_addr(const Env &e, uint32_t a) { size_t n = 0; for (auto &w : e.m.writes) n += w.first == a; return n; }
static const std::set<uint32_t> kP3Regs = { kRegDp1Vid, kRegDp1Steer, kRegDigFifoCtrl0 };
// The 3 writes of a blank at index i of the write log, and the 10 writes of an unblank: the order and the field values of Linux's sequence (design P3).
static bool match_blank(const Writes &w, size_t i) {
    return i + 3 <= w.size() && w[i].first == kRegDp1Vid && (w[i].second & 0x301u) == 0x201u &&                                  // DIS_DEFER = 2, ENABLE still 1
           w[i + 1].first == kRegDp1Vid && (w[i + 1].second & 0x301u) == 0x200u &&                                              // ENABLE = 0
           w[i + 2].first == kRegDp1Steer && (w[i + 2].second & 0x3u) == 0x3u;                                                  // THEN STEER_FIFO_RESET = 1
}
static bool match_unblank(const Writes &w, size_t i) {
    if (i + 10 > w.size()) return false;
    const auto V = [&](size_t k) { return w[i + k].second; }; const auto A = [&](size_t k) { return w[i + k].first; };
    return A(0) == kRegDp1Vid && (V(0) & 0x301u) == 0x200u &&                                                                  // ENABLE = 0
           A(1) == kRegDp1Steer && (V(1) & 0x2u) != 0u && A(2) == kRegDp1Steer && (V(2) & 0x3u) == 0x1u &&                     // STEER_RESET 1, then 0
           A(3) == kRegDp1Steer && (V(3) & 0x3u) == 0x1u &&                                                                     // STEER_ENABLE = 1
           A(4) == kRegDp1Vid && (V(4) & 0x301u) == 0x201u &&                                                                   // ENABLE = 1 with DIS_DEFER = 2
           A(5) == kRegDigFifoCtrl0 && (V(5) & 0x7Cu) == 0x1Cu &&                                                               // READ_START_LEVEL = 7
           A(6) == kRegDigFifoCtrl0 && (V(6) & 0x2u) == 0x2u && A(7) == kRegDigFifoCtrl0 && (V(7) & 0x2u) == 0u &&             // DIG_FIFO_RESET 1, then 0
           A(8) == kRegDigFifoCtrl0 && (V(8) & 0x1u) == 0x1u &&                                                                 // DIG_FIFO_ENABLE = 1
           A(9) == kRegDp1Vid && (V(9) & 0x301u) == 0x201u;                                                                     // ENABLE = 1
}
// Every write to a P3 register is field-exact: DP1_DP_STEER_FIFO never carries ACK / INT / OVERFLOW / DONE (0x74), DIG1_DIG_FIFO_CTRL0 never carries ERROR / RESET_DONE (0x30100000) and never
// changes PIXEL_PER_CYCLE (0x300, 0 in the census), DP1_DP_VID_STREAM_CNTL never carries STATUS (bit 16).
static bool p3_field_exact(const Env &e) {
    for (auto &w : e.m.writes) {
        if (w.first == kRegDp1Steer && (w.second & 0x74u) != 0u) return false;
        if (w.first == kRegDigFifoCtrl0 && ((w.second & 0x30100000u) != 0u || (w.second & 0x300u) != (0x1Du & 0x300u))) return false;
        if (w.first == kRegDp1Vid && (w.second & 0x10000u) != 0u) return false;
    }
    return true;
}
static bool stream_up(Env &e) { return stream_active(h_rd(&e.m, kRegDp1Vid)); }

// ---- T7: P1, the underflow registers -----------------------------------------------------------------------------------------------------------------------------------
static void t7_underflow() {
    // decode
    expect_u("HUBP UNDERFLOW_STATUS is bits 30:28", uf::hubp_status(0x50000000u), 5); expect_u("HUBP TIMEOUT_STATUS is bits 23:20", uf::hubp_timeout(0x00A00000u), 0xA);
    expect(!uf::hubp_bad(0x000F0002u) && uf::hubp_bad(0x10000002u) && uf::hubp_bad(0x00100002u) && uf::hubp_bad(0xFFFFFFFFu), "the census HUBP word (0x000F0002) is clean; a status, a timeout status or an unreadable word is an underflow");
    expect(!uf::optc_bad(0u) && uf::optc_bad(0x400u) && uf::optc_bad(0x800u) && uf::optc_bad(0x2000u) && uf::optc_bad(0xFFFFFFFFu), "OPTC: OCCURRED (b10), INT (b11), CURRENT (b13) and an unreadable word are underflows");
    expect(!uf::optc_bad(0x1u) && !uf::optc_bad(0x1000u) && !uf::optc_bad(0x80000000u), "OPTC: INPUT_SOFT_RESET, CLEAR and DOUBLE_BUFFER_PENDING alone are not underflows");
    expect_u("hubp clear = the control bits as read, status written 0, both strobes set", uf::hubp_clear_value(0x100F0002u), 0x840F0002u);
    expect_u("hubp clear drops a timeout status too", uf::hubp_clear_value(0x00A00002u), 0x84000002u);
    expect_u("optc clear = the strobe only (status and pending written 0)", uf::optc_clear_value(0x80002C00u), 0x1000u);
    expect(uf::clear_refused(0x401u) && !uf::clear_refused(0x400u) && !uf::clear_refused(0xFFFFFFFFu), "an OPTC clear is refused exactly when INPUT_SOFT_RESET (b0) is set");
    expect_u("unstrobe drops the strobes", uf::hubp_unstrobe_value(0x840F0002u), 0x000F0002u); expect_u("unstrobe (optc)", uf::optc_unstrobe_value(0x1001u), 0x1u);
    // the verdict word and its precedence
    Obs o{}; o.underflow = true; expect_u("underflow alone: UNDERFLOW (11)", judge(o), N48N_MODE_V_UNDERFLOW); expect_u("verdict codes 11 / 13 / 14", N48N_MODE_V_UNDERFLOW * 10000 + N48N_MODE_V_RESYNC * 100 + N48N_MODE_V_LOCK, 111314);
    o = Obs{}; o.underflow = o.fifoBad = true; expect_u("underflow beats the DIO FIFO", judge(o), N48N_MODE_V_UNDERFLOW);
    o = Obs{}; o.underflow = o.rateBad = true; expect_u("underflow beats the rate", judge(o), N48N_MODE_V_UNDERFLOW);
    o = Obs{}; o.underflow = o.streamLost = true; expect_u("a lost stream beats underflow", judge(o), N48N_MODE_V_STREAM);
    o = Obs{}; o.underflow = o.stalled = true; expect_u("a stalled counter beats underflow", judge(o), N48N_MODE_V_STALL);
    o = Obs{}; o.underflow = o.resyncFail = true; expect_u("a failed resync beats underflow", judge(o), N48N_MODE_V_RESYNC);
    o = Obs{}; o.resyncFail = o.stalled = true; expect_u("a failed resync beats a stall", judge(o), N48N_MODE_V_RESYNC);
    o = Obs{}; o.lockFail = o.resyncFail = true; expect_u("a failed lock beats a failed resync", judge(o), N48N_MODE_V_LOCK);
    o = Obs{}; o.lockFail = o.writeFailed = true; expect_u("a refused write beats a failed lock", judge(o), N48N_MODE_V_WRITE);
    o = Obs{}; o.underflow = o.aborted = true; expect_u("an abort beats underflow", judge(o), N48N_MODE_V_ABORT);
    o = Obs{}; o.underflow = true; o.restoreBad = true; expect_u("a restore that did not verify beats underflow", judge(o), N48N_MODE_V_RESTORE);
    for (uint32_t v : { 11u, 12u, 13u, 14u }) expect(is_failure(v), "the 0.0.606 verdicts latch"); expect(is_failure(15u) && !is_failure(16u), "0.0.607: verdict 15 (HOLD) latches; 16 is not a verdict");
    expect_u("the clock-hold verdicts are 12 (CLOCK_LOST) and 15 (HOLD)", N48N_MODE_V_CLOCK_LOST * 100 + N48N_MODE_V_HOLD, 1215);

    // engine: a clean row-50 trial reads both registers, writes neither, reports the reads
    { Env e; e.bind(); run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_PASS && e.r.ext.uf_hubp_before == 0x000F0002u && e.r.ext.uf_optc_before == 0u && e.r.ext.uf_samples >= 4 && e.r.ext.uf_clears == 0, "clean trial: PASS, both registers read (census values), samples taken, nothing cleared");
      expect(count_addr(e, kRegHubpCntl) == 0 && count_addr(e, kRegOptcInGlobal) == 0, "a clean trial writes neither underflow register"); }
    // a status set before the trial is cleared ONCE with the strobes and the trial goes on
    { Env e; e.bind(); e.m.reg[kRegHubpCntl] |= 0x10000000u; e.m.reg[kRegOptcInGlobal] |= 0x400u; run(e, 50, 500);
      const Writes h = wr_at(e, { kRegHubpCntl }), oc = wr_at(e, { kRegOptcInGlobal });
      expect(e.r.verdict == N48N_MODE_V_PASS && h.size() == 1 && oc.size() == 1 && h[0].second == 0x840F0002u && oc[0].second == 0x1000u, "a stale sticky status is cleared once (HUBP 0x840F0002, OPTC 0x1000) and the trial runs");
      expect(e.r.ext.uf_hubp_before == 0x100F0002u && e.r.ext.uf_optc_before == 0x400u && e.r.ext.uf_clears == 2 && !(e.r.flags & N48N_MODE_F_UNDERFLOW_SEEN), "the raw before values are reported; the pre-trial status is not a judged event"); }
    // not clearable: DENIED (UNDERFLOW), the only writes are the two clear attempts, no DTO write, no latch
    { Env e; e.bind(); e.m.reg[kRegHubpCntl] |= 0x10000000u; e.m.hubpClearIgnored = true; run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_UNDERFLOW && e.r.latched == 0 && count_addr(e, 0x141) == 0 && only_in(e, { kRegHubpCntl }), "an underflow status that will not clear denies the trial (UNDERFLOW); nothing but the clear was written"); }
    // (item 6) a stale status with ANOTHER reason to deny: nothing is written at all; the clear waits for every other answer
    { Env e; e.bind(); e.m.reg[kRegHubpCntl] |= 0x10000000u; e.m.reg[kRegDpStream[0]] = 0x10201u; run(e, 50, 500);
      expect(e.r.deny == N48N_MODE_D_STREAM && e.m.writes.empty() && !(e.r.flags & N48N_MODE_F_UF_CLEARED), "DENIED for another reason with a stale underflow status: NOTHING is written (the clear waits)"); }
    { Env e; e.bind(); e.m.reg[kRegHubpCntl] |= 0x10000000u; e.m.wdStartFail = true; run(e, 50, 500);
      expect(e.r.deny == N48N_MODE_D_WATCHDOG && (e.r.flags & N48N_MODE_F_UF_CLEARED) && e.r.ext.uf_clears == 1 && only_in(e, { kRegHubpCntl }), "a denial after the clear wrote (the watchdog thread refused) says so: DENIED + UF_CLEARED, and the clear is the only write"); }
    { Env e; e.bind(); run(e, 50, 500); expect(!(e.r.flags & N48N_MODE_F_UF_CLEARED), "a clean trial writes no clear"); }
    // the OPTC clear is refused while INPUT_SOFT_RESET is set: no write to the register at all
    { Env e; e.bind(); e.m.reg[kRegOptcInGlobal] = 0x401u; run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_UNDERFLOW && count_addr(e, kRegOptcInGlobal) == 0 && e.r.ext.uf_clear_refused == 1 && (e.r.flags & N48N_MODE_F_UF_CLEAR_REFUSED), "OPTC underflow with INPUT_SOFT_RESET set: the clear is REFUSED (no write), the trial is denied, the refusal is reported"); }
    // the clear never sets INPUT_SOFT_RESET (b0) when it was clear, and keeps the control bits it read
    { Env e; e.bind(); e.m.reg[kRegOptcInGlobal] = 0x00002400u; e.m.reg[kRegHubpCntl] |= 0x70000000u; run(e, 50, 500);
      bool b0 = false; for (auto &w : wr_at(e, { kRegOptcInGlobal })) b0 = b0 || (w.second & 1u) != 0u;
      expect(!b0 && count_addr(e, kRegOptcInGlobal) >= 1 && e.r.verdict == N48N_MODE_V_PASS, "no write to ODM0_OPTC_INPUT_GLOBAL_CONTROL ever sets INPUT_SOFT_RESET"); }
    // an unreadable register: DENIED, flagged, never written
    { Env e; e.bind(); e.m.unreadable.insert(kRegHubpCntl); run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_UNDERFLOW && (e.r.flags & N48N_MODE_F_UNDERFLOW_UNREAD) && count_addr(e, kRegHubpCntl) == 0 && count_addr(e, kRegOptcInGlobal) == 0, "an unreadable HUBP status denies the trial and is never written (UNREAD flag)"); }
    // strobes that read back set (a level): written back 0, detection stays live
    { Env e; e.bind(); e.m.reg[kRegHubpCntl] |= 0x10000000u; e.m.strobeSticky = true; run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_PASS && e.r.ext.uf_clear_stuck >= 1 && (e.m.reg[kRegHubpCntl] & 0x84000000u) == 0u, "a strobe that reads back set is written back 0 (stuck count reported)"); }
    // during the rate window: UNDERFLOW, restored, latched, the window is cut short
    { Env e; e.bind(); e.m.ufAtUs = 5000000ull + 2000000ull + 500000ull + 1000000ull; run(e, 50, 3000);
      expect_u("underflow in the rate window: verdict UNDERFLOW", e.r.verdict, N48N_MODE_V_UNDERFLOW);
      expect(e.r.fail_phase == 4 && (e.r.flags & N48N_MODE_F_UNDERFLOW_SEEN) && e.r.latched == 1 && (e.r.flags & N48N_MODE_F_RESTORED) && all_init(e) && e.r.ms_trial < 2500, "phase 4, SEEN, latched, restored bit-exact, the window ended early");
      expect(e.r.ext.uf_hubp_max == 1 && e.r.ext.uf_first_ms > 0 && (e.r.ext.uf_hubp_or & 0x10000000u) && (e.r.ext.uf_optc_or & 0x400u) && e.r.ext.uf_clears >= 1, "max HUBP status 1, the first event's time, the OR of the bits, the restore-entry clear");
      expect(!(e.r.flags & N48N_MODE_F_HEALTH_BAD) && e.r.ext.uf_hubp_after == 0x000F0002u && e.r.ext.uf_optc_after == 0u, "the status was cleared at the restore entry: the after-restore read is clean (one event is not counted twice)");
      Env f; f.bind(); f.m.ufAtUs = 5000000ull + 2000000ull + 500000ull + 1000000ull; run(f, 50, 3000); run(f, 50, 100); expect(f.r.deny == N48N_MODE_D_LATCHED, "the latch refuses the next trial"); }
    // during the dwell
    { Env e; e.bind(); e.m.ufAtUs = 5000000ull + 2000000ull + 500000ull + 3000000ull + 1500000ull; run(e, 50, 6000);
      expect(e.r.verdict == N48N_MODE_V_UNDERFLOW && e.r.fail_phase == 5 && e.r.dwell_done_ms < 4000 && all_init(e), "underflow in the dwell: UNDERFLOW, phase 5, the dwell is cut short, restored"); }
    // the write transition's underflow is REPORTED (settle read), cleared, and not judged
    { Env e; e.bind(); e.m.ufOnDto = true; run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_PASS && e.r.ext.uf_hubp_settle == 0x100F0002u && (e.r.ext.uf_optc_settle & 0x2400u) == 0x2400u && !(e.r.flags & N48N_MODE_F_UNDERFLOW_SEEN) && e.r.ext.uf_hubp_max == 1, "an underflow at the write transition: reported after the settle, cleared, PASS (only new events are judged)"); }
    // the restore transition's underflow IS judged: the restore did not verify
    { Env e; e.bind(); e.m.ufOnRestore = true; run(e, 50, 500);
      expect(e.r.verdict == N48N_MODE_V_RESTORE && (e.r.flags & N48N_MODE_F_RESTORE_FAILED) && (e.r.flags & N48N_MODE_F_UNDERFLOW_SEEN) && e.r.latched == 1 && (e.r.ext.uf_hubp_after & 0x70000000u) != 0u && all_init(e), "an underflow in the restore transition: verdict RESTORE (registers are back, the verdict is not PASS), latched"); }
    // an unreadable register mid-trial is an underflow (fail closed)
    { Env e; e.bind(); e.m.uaAddr = kRegOptcInGlobal; e.m.uaFrom = 5000000ull + 2000000ull + 500000ull + 1000000ull; e.m.uaTo = e.m.uaFrom + 300000ull; run(e, 50, 3000);
      expect(e.r.verdict == N48N_MODE_V_UNDERFLOW && (e.r.flags & N48N_MODE_F_UNDERFLOW_UNREAD) && e.r.fail_phase == 4, "an underflow register that reads all-ones mid-trial ends the trial (UNDERFLOW, UNREAD)"); }
    // the deny matrix: ordering of the new reasons
    Pre p = ok_pre(); p.ufOk = false; expect_u("underflow baseline", deny_check(p), N48N_MODE_D_UNDERFLOW);
    p = ok_pre(); p.baselineOk = false; p.ufOk = false; expect_u("baseline is answered before underflow", deny_check(p), N48N_MODE_D_BASELINE);
    p = ok_pre(); p.ufOk = false; p.streamOk = false; expect_u("underflow is answered before the stream", deny_check(p), N48N_MODE_D_UNDERFLOW);
    expect_u("deny codes 20 / 21", N48N_MODE_D_UNDERFLOW * 100 + N48N_MODE_D_LOCK, 2021);
}

// ---- T8: P3, the DP1 resync (row 1) ---------------------------------------------------------------------------------------------------------------------------------------
static void t8_resync() {
    // the clean standalone resync: Linux's order and field values, nothing else
    {
        Env e; e.bind(); runf(e, 1, 1000, 0);
        const Writes w = wr_at(e, { kRegDp1Vid, kRegDp1Steer, kRegDigFifoCtrl0 });
        expect_u("row 1 PASS", e.r.verdict, N48N_MODE_V_PASS);
        expect(w.size() == 13 && e.m.writes.size() == 13 && only_in(e, kP3Regs), "row 1 writes 13 registers: 3 (blank) + 10 (unblank), all of them P3 registers and no others");
        expect(match_blank(e.m.writes, 0), "blank: DIS_DEFER = 2, ENABLE = 0, THEN STEER_FIFO_RESET = 1");
        expect(match_unblank(e.m.writes, 3), "unblank: ENABLE = 0, steer reset pulse, steer enable, ENABLE + DIS_DEFER 2, level 7, DIG reset pulse, DIG enable, ENABLE = 1");
        expect(p3_field_exact(e), "every write is field-exact: no ACK / INT / OVERFLOW / DONE, no ERROR / RESET_DONE, PIXEL_PER_CYCLE untouched, no STATUS bit");
        expect(!e.st.blanked && stream_up(e) && e.st.resyncPassed && !e.st.step50Passed && !e.st.latched && e.r.nwrite == 0 && e.r.nskip == 0, "the stream is up, `blanked` is clear, the resync counts (not the row-50 step-down), nothing latched, no kCap register written");
        expect(e.r.ext.rs_runs == 1 && e.r.ext.rs_attempts == 2 && e.r.ext.rs_rc_first == 0 && e.r.ext.rs_rc_last == 0 && (e.r.flags & N48N_MODE_F_RESYNC_RUN) && !(e.r.flags & N48N_MODE_F_STILL_BLANKED), "one run, two attempts (blank + unblank), every step ok, flagged");
        expect(e.r.ext.rs_blank_us >= 60000u && e.r.ext.rs_blank_us < 200000u && e.r.ext.rs_unblank_us > 200u && e.r.ext.rs_stream_after == 0x10201u && e.r.ext.rs_steer_after == 1u && e.r.ext.rs_dig_after == 0x1Du, "the blank took the msleep(60); the unblank took its delays; the registers after are the census values");
        expect(e.r.rate_trial_mhz >= 59350 && e.r.rate_trial_mhz <= 60550 && (e.r.flags & N48N_MODE_F_RESTORED) && all_init(e) && e.r.ext.uf_samples >= 4, "60 Hz throughout, restored, underflow sampled");
        expect(e.r.errcnt_before == 1 && e.r.errcnt_after == 1 && e.r.ext.rs_dio_errs == 0, "DIO_ERROR_COUNT stays at the census baseline of 1");
    }
    // DIO growth is judged against the BASELINE (1), never required 0: a trial starts fine with a count of 1 (above); growth during the window fails; growth across the resync itself is only reported
    { Env e; e.bind(); e.m.reg[kRegPixelRateCntl] = 0x00051090u;   // a baseline of 5, not 1
      runf(e, 1, 500, 0); expect(e.r.verdict == N48N_MODE_V_PASS && e.r.errcnt_before == 5 && e.r.errcnt_after == 5, "a DIO_ERROR_COUNT baseline of 5 is not a failure: only growth is"); }
    // failure branches. (noStatusDrop) the stream STATUS never drops: blank fails, unblank cannot proceed: RESYNC in the trial, then the restore cannot unblank either -> RESTORE + STILL_BLANKED
    {
        Env e; e.bind(); e.m.noStatusDrop = true; runf(e, 1, 500, 0);
        expect(e.r.ext.rs_rc_first == N48N_MODE_SEQ_STATUS_STUCK && !e.st.blanked && stream_up(e) && !(e.r.flags & N48N_MODE_F_STILL_BLANKED) && e.r.verdict == N48N_MODE_V_RESYNC && e.r.latched == 1, "STATUS stuck: the wait times out (STATUS_STUCK) but the LAST unblank attempt follows Linux past it to the final ENABLE = 1: the stream is UP, not blanked, and the timeout still reaches the verdict (RESYNC, latched)");
        expect(e.m.writes.size() > 3 && e.m.writes[1].first == kRegDp1Vid && e.m.writes[2].first == kRegDp1Vid, "no STEER_FIFO_RESET is written while STATUS has not dropped (the Linux ordering: the wait comes first; a failed blank ends there and only the FORCED last unblank writes it later)");
        // the fault clears, `dcnmode 0` (restore_from_golden) unblanks
        e.m.noStatusDrop = false; uint32_t restored = 0; bool have = false; tIsTrial = true;
        const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have); tIsTrial = false;
        expect(have && bad == 0 && !e.st.blanked && stream_up(e), "after the fault clears, `dcnmode 0` / the kext stop leave the stream up");
        // the same fault, the recovery path itself: a blanked stream and a STATUS that will not drop is brought back by the forced last attempt
        Env f; f.bind(); f.st.everWrote = true; f.m.vidDynamic = true; f.m.vidEn = false; f.m.reg[kRegDp1Vid] = 0x200u; f.m.vidChangeAt = 0; f.m.noStatusDrop = true; f.st.blanked = true;
        tIsTrial = true; const uint32_t bad2 = restore_from_golden(f.hw, f.st, &restored, &have); tIsTrial = false;
        expect(stream_up(f) && !f.st.blanked && bad2 == 0, "dcnmode 0 / the kext stop with STATUS stuck at 1: the stream is enabled again anyway");
        expect(p3_field_exact(e), "still field-exact under failure");
    }
    // (neverActive) the unblank ends without an active stream: NOT_ACTIVE
    { Env e; e.bind(); e.m.neverActive = true; runf(e, 1, 500, 0);
      expect(e.r.ext.rs_rc_first == N48N_MODE_SEQ_NOT_ACTIVE && e.r.verdict == N48N_MODE_V_RESTORE && (e.r.flags & N48N_MODE_F_STILL_BLANKED) && e.r.latched == 1, "the stream never reports active: NOT_ACTIVE, blanked stays, RESTORE, latched"); }
    // (digNeverDone) DIG_FIFO RESET_DONE never comes: DIG_RESET, and the reset is always released again
    { Env e; e.bind(); e.m.digNeverDone = true; runf(e, 1, 500, 0);
      expect(e.r.ext.rs_rc_first == N48N_MODE_SEQ_DIG_RESET && e.r.latched == 1 && e.r.verdict == N48N_MODE_V_RESYNC && stream_up(e) && !e.st.blanked, "DIG FIFO RESET_DONE never comes: DIG_RESET is reported (RESYNC, latched) but the last attempt carried on: the stream is up");
      bool released = (e.m.reg[kRegDigFifoCtrl0] & kDigReset) == 0u; expect(released, "DIG_FIFO_RESET is written back 0 even when the wait failed"); }
    // one-shot fault: the first unblank fails, the retry works: PASS, three attempts, the first failure remembered
    { Env e; e.bind(); e.m.digFailCount = 1; runf(e, 1, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_PASS && e.r.ext.rs_attempts == 3 && e.r.ext.rs_rc_first == N48N_MODE_SEQ_DIG_RESET && e.r.ext.rs_rc_last == 0 && !e.st.blanked && stream_up(e), "ONE retry: a failed unblank is tried once more and passes (3 attempts, first rc DIG_RESET, last ok)"); }
    // two faults: both attempts fail
    { Env e; e.bind(); e.m.digFailCount = 2; runf(e, 1, 500, 0); expect(e.r.ext.rs_rc_last == N48N_MODE_SEQ_DIG_RESET && e.r.verdict == N48N_MODE_V_RESYNC, "two failures: no third attempt at the unblank, the resync fails (the verdict says so)"); expect(e.r.ext.rs_attempts == 3 && stream_up(e), "exactly one retry each (blank 1 + unblank 2); the forced last attempt left the stream up, so the restore had nothing to unblank"); }
    // unblank_locked itself: without `force` a stuck STATUS ends it before the final ENABLE = 1; with it the sequence carries on
    { Env e; e.bind(); e.m.vidDynamic = true; e.m.vidEn = false; e.m.reg[kRegDp1Vid] = 0x200u; e.m.vidChangeAt = 0; e.m.noStatusDrop = true; e.st.blanked = true; Held h(e);
      expect(unblank_locked(e.hw, e.st, false) == N48N_MODE_SEQ_STATUS_STUCK && !stream_up(e) && e.st.blanked, "unblank (not the last attempt) stops at a stuck STATUS: the stream stays disabled");
      expect(unblank_locked(e.hw, e.st, true) == N48N_MODE_SEQ_STATUS_STUCK && stream_up(e) && !e.st.blanked && e.m.writes.back().first == kRegDp1Vid && (e.m.writes.back().second & 1u), "unblank (last attempt) records the timeout and still ends with ENABLE = 1: the stream is up"); }
    // a write refused before anything moved: the blank is retried once, the stream is untouched, the verdict is RESYNC (a clean restore, not RESTORE)
    { Env e; e.bind(); e.m.refuseWr.insert(kRegDp1Vid); runf(e, 1, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_RESYNC && e.r.ext.rs_rc_first == N48N_MODE_SEQ_WRITE && e.r.ext.rs_attempts == 2 && !e.st.blanked && stream_up(e) && e.m.writes.empty() && (e.r.flags & N48N_MODE_F_RESTORED), "the very first write refused: retried once, nothing moved, RESYNC, restore clean"); }
    // a refused steer write after the stream stopped: blanked stays, the restore cannot bring it back
    { Env e; e.bind(); e.m.refuseWr.insert(kRegDp1Steer); runf(e, 1, 500, 0);
      expect(e.r.ext.rs_rc_first == N48N_MODE_SEQ_WRITE && e.st.blanked && e.r.verdict == N48N_MODE_V_RESTORE && (e.r.flags & N48N_MODE_F_STILL_BLANKED), "a refused STEER_FIFO write mid-blank: the stream is left blanked, reported, RESTORE"); }
    // NOT_ENABLED (Linux returns): the primitive writes nothing
    { Env e; e.bind(); e.m.vidDynamic = true; e.m.vidEn = false; e.m.reg[kRegDp1Vid] = 0x200u; e.m.vidChangeAt = 0; Held h(e);
      expect(blank_locked(e.hw, e.st) == N48N_MODE_SEQ_NOT_ENABLED && e.m.writes.empty() && !e.st.blanked, "blank on a stream that is not enabled returns at once (Linux), writes nothing, blanks nothing"); }
    // the unblank alone works from any state
    { Env e; e.bind(); e.m.vidDynamic = true; e.m.vidEn = false; e.m.reg[kRegDp1Vid] = 0x200u; e.m.vidChangeAt = 0; e.st.blanked = true; Held h(e);
      expect(unblank_locked(e.hw, e.st) == N48N_MODE_SEQ_OK && !e.st.blanked && match_unblank(e.m.writes, 0), "unblank from a disabled stream: the full sequence, the stream is active, `blanked` clears"); }
    // deny: the same rules as any trial
    { Env e; e.bind(); e.m.reg[kRegDpStream[0]] = 0x10201u; runf(e, 1, 100, 0); expect(e.r.deny == N48N_MODE_D_STREAM && e.m.writes.empty(), "row 1 with another DP stream enabled: STREAM, nothing written (DP1 is the only stream)"); }
    { Env e; e.bind(); e.st.latched = true; runf(e, 1, 100, 0); expect(e.r.deny == N48N_MODE_D_LATCHED && e.m.writes.empty(), "row 1 is latched like any trial"); }
    { Env e; e.bind(); e.st.blanked = true; runf(e, 1, 100, 0); expect(e.r.deny == N48N_MODE_D_DRIFT && e.m.writes.empty(), "a stream a failed trial left blanked denies the next trial (drift: run dcnmode 0)"); }
    { Env e; e.bind(); runf(e, 1, 100, 0x2u); expect(e.r.deny == N48N_MODE_D_BAD_ROW && e.m.writes.empty(), "an unknown flag bit is refused"); }
    { Env e; e.bind(); runf(e, 1, 100, N48N_MODE_TF_RESYNC); expect(e.r.verdict == N48N_MODE_V_PASS, "TF_RESYNC on row 1 is redundant but accepted"); }
    // the kProd timing: the resync is available (row 1 has no hard deny) and takes no clock
    { Env e; e.bind(); e.tm = kProd; runf(e, 1, 100, 0); expect(e.r.verdict == N48N_MODE_V_PASS, "row 1 runs with the production timing (row 120 stays hard-denied)"); }
    // watchdog window
    expect_u("the watchdog window carries the resync's time", wd_window_ms(kProd, 1000, true), wd_window_ms(kProd, 1000) + 4000u);
    expect_u("... and nothing without it", wd_window_ms(kProd, 1000, false), 500u + 3000u + 1000u + 2000u);
}

// ---- T9: P2, the OTG update lock, the DRR mode and the V_TOTAL latch (row 2) -----------------------------------------------------------------------------------------------------
static bool lock_paired(Env &e) {   // every lock request has its unlock, in order, and the lock register ends clear
    int depth = 0; for (auto &w : e.m.writes) { if (w.first != kRegMasterLock) continue; if (w.second & 1u) depth++; else depth--; if (depth < 0 || depth > 1) return false; }
    return depth == 0 && !e.m.lockReq && (e.m.reg[kRegMasterLock] & 1u) == 0u;
}
static void t9_lock() {
    expect_u("V_TOTAL of row 2: 1500 (1501 lines)", kRow2VTotalReg, 1500); expect_u("row 2 gold V_TOTAL", kGoldVt.v[kIdxVTotal], 1500u);
    bool same = true; for (uint32_t i = 0; i < kCapN; i++) if (i != kIdxVTotal) same = same && kGoldVt.v[i] == tab::kGold60[i];
    expect(same, "row 2 = the 60 Hz golden with V_TOTAL changed and nothing else");
    const RowInfo r2 = row_info(2);
    expect(r2.row == 2 && r2.n == 1 && r2.idx[0] == kIdxVTotal && r2.vTotal == 1501 && r2.hTotal == 2720 && r2.dtoInt == 0 && r2.phase == 241500000u, "row 2 writes the OTG V_TOTAL register and nothing else (V_TOTAL_MIN/MAX, the DTO, the MSA and the DLG block stay)");
    expect(expect_mhz(r2) >= 59150 && expect_mhz(r2) <= 59155, "row 2 expects 59.15 Hz");
    expect(!rate_within(59950, expect_mhz(r2), 6) && rate_within(59480, expect_mhz(r2), 6) && rate_within(58830, expect_mhz(r2), 6) && rate_within(59950, expect_mhz(r2), 20), "row 2's 0.6 % window tells 59.15 from 59.95 even with a frame of quantisation; 1 % cannot");
    expect(rate_within(59950, expect_mhz(row60()), 6) && !rate_within(59480, expect_mhz(row60()), 6), "and the restore window (0.6 % around 59.95) refuses a stuck 59.15 Hz");
    expect(lock_usable(0u, 0u, 0u, 0u) && lock_usable(0x02000000u, 0u, 0u, 0u) && !lock_usable(0x01000000u, 0u, 0u, 0u) && !lock_usable(0x03000000u, 0u, 0u, 0u), "lock precondition: DRR mode 0 or 2 only");
    expect(!lock_usable(0x100u, 0u, 0u, 0u) && !lock_usable(0x1u, 0u, 0u, 0u) && !lock_usable(0x20u, 0u, 0u, 0u) && !lock_usable(0x10u, 0u, 0u, 0u) && !lock_usable(0x200u, 0u, 0u, 0u), "UPDATE_INSTANTLY (b8) or any pending bit (b0 b4 b5 b9) denies");
    expect(!lock_usable(0u, 1u, 0u, 0u) && !lock_usable(0u, 0x100u, 0u, 0u) && !lock_usable(0u, 0u, 0u, 0x10u) && lock_usable(0u, 0u, 0u, 0x1u), "the lock already held / its status set / a register update pending denies; a flip pending (b0) does not");
    expect(!lock_usable(0xFFFFFFFFu, 0u, 0u, 0u) && !lock_usable(0u, 0xFFFFFFFFu, 0u, 0u) && !lock_usable(0u, 0u, 0xFFFFFFFFu, 0u) && !lock_usable(0u, 0u, 0u, 0xFFFFFFFFu), "an unreadable register denies");
    // deny ordering
    Pre p = ok_pre(); p.lockOk = false; expect_u("lock precondition", deny_check(p), N48N_MODE_D_LOCK);
    p = ok_pre(); p.lockOk = false; p.driftFree = false; expect_u("drift is answered before the lock precondition", deny_check(p), N48N_MODE_D_DRIFT);
    p = ok_pre(); p.lockOk = false; p.clocksOk = false; expect_u("the lock precondition is answered before the clocks", deny_check(p), N48N_MODE_D_LOCK);
    p = ok_pre(2); p.needResync = true; p.resyncPassed = false; expect_u("row 2 needs a passed resync", deny_check(p), N48N_MODE_D_STEP_DOWN);
    p = ok_pre(2); p.needResync = true; p.resyncPassed = true; expect_u("row 2 with a passed resync goes", deny_check(p), 0);
    p = ok_pre(1); expect_u("row 1 is a row", deny_check(p), 0); p = ok_pre(3); expect_u("row 3 is not", deny_check(p), N48N_MODE_D_BAD_ROW);

    // the clean isolation trial
    {
        Env e; e.bind(); e.st.resyncPassed = true; runf(e, 2, 3000, 0);
        const n48n_mode_result &r = e.r;
        expect_u("row 2 PASS", r.verdict, N48N_MODE_V_PASS);
        expect(r.nwrite == 1 && r.nskip == 0 && r.first_write_abs == kCap[kIdxVTotal].abs && r.last_write_abs == kCap[kIdxVTotal].abs, "one kCap register written: OTG0_OTG_V_TOTAL");
        // the whole write log, in order
        const Writes &w = e.m.writes; const size_t n = w.size();
        std::vector<uint32_t> want = { kRegDp1Vid, kRegDp1Vid, kRegDp1Steer, kRegDbCtrl, kRegGlobalCtrl2, kRegMasterLock, kCap[kIdxVTotal].abs, kRegMasterLock };
        for (int k = 0; k < 10; k++) want.push_back(k == 0 || k == 4 || k == 9 ? kRegDp1Vid : (k >= 1 && k <= 3) ? kRegDp1Steer : kRegDigFifoCtrl0);
        const std::vector<uint32_t> restoreHead = { kRegDp1Vid, kRegDp1Vid, kRegDp1Steer, kRegGlobalCtrl2, kRegMasterLock, kCap[kIdxVTotal].abs, kRegMasterLock, kRegDbCtrl, kRegGlobalCtrl2 };
        want.insert(want.end(), restoreHead.begin(), restoreHead.end());
        for (int k = 0; k < 10; k++) want.push_back(k == 0 || k == 4 || k == 9 ? kRegDp1Vid : (k >= 1 && k <= 3) ? kRegDp1Steer : kRegDigFifoCtrl0);
        bool seq = n == want.size(); for (size_t k = 0; seq && k < n; k++) seq = w[k].first == want[k];
        expect(seq, "apply: blank, DRR mode, lock select, LOCK, V_TOTAL, UNLOCK, unblank; restore: blank, lock select, LOCK, V_TOTAL, UNLOCK, DRR mode (last register), lock select, unblank");
        if (seq) {
            expect(match_blank(w, 0) && match_unblank(w, 8) && match_blank(w, 18) && match_unblank(w, 27), "the blank / unblank inside are Linux's sequences (both times)");
            expect(w[3].second == 0x02000000u && w[4].second == 0u && w[5].second == 1u && w[6].second == kRow2VTotalReg && w[7].second == 0u, "DRR mode = 2 (0x02000000), select = OTG0, lock request 1, V_TOTAL = 1500, lock request 0");
            const size_t b = 18; expect(w[b + 3].second == 0u && w[b + 4].second == 1u && w[b + 5].second == tab::kGold60[kIdxVTotal] && w[b + 6].second == 0u && w[b + 7].second == 0u, "restore: select, lock 1, V_TOTAL = the golden 1480, lock 0, DRR mode = the golden 0");
        }
        expect(lock_paired(e) && count_addr(e, kRegMasterLock) == 4, "two locks, two unlocks, in order, and the lock register ends clear");
        expect(count_addr(e, kCap[kIdxVTotal].abs + 1) == 0 && count_addr(e, kCap[kIdxVTotal].abs + 2) == 0 && count_addr(e, 0x141) == 0 && count_addr(e, 0x142) == 0 && count_addr(e, 0x12f) == 0 && count_addr(e, kCap[cap_find("DP1_DP_MSA_TIMING_PARAM1")].abs) == 0, "V_TOTAL_MIN / V_TOTAL_MAX, the DTO and the MSA are never written");
        expect(p3_field_exact(e), "the resync writes are field-exact here too");
        expect(only_in(e, { kRegDp1Vid, kRegDp1Steer, kRegDigFifoCtrl0, kRegDbCtrl, kRegGlobalCtrl2, kRegMasterLock, kCap[kIdxVTotal].abs }), "row 2 writes nothing outside its seven registers");
        expect(r.ext.lk_rc == 0 && r.ext.lk_drr_before == 0 && r.ext.lk_drr_during == 2 && r.ext.lk_drr_after == 0 && r.ext.lk_lock_after == 0 && (e.m.reg[kRegDbCtrl] & kDbDrrMask) == 0u, "DRR mode 0 -> 2 -> 0; the lock register clear afterwards");
        expect((r.flags & N48N_MODE_F_LOCK_USED) && (r.flags & N48N_MODE_F_LATCH_CONFIRMED) && !(r.flags & N48N_MODE_F_LOCK_STILL_HELD) && r.ext.lk_confirmed == 2, "LOCK_USED, the latch confirmed on the way up and on the way down");
        expect(r.ext.lk_vert_max > 1480 && r.ext.lk_vert_max <= 1500 && r.ext.lk_vert_samples > 100 && (r.ext.lk_pending_seen & 0x20u) != 0u, "the witness saw VERT_COUNT past the old maximum (1480); the TIMING_DB_UPDATE_PENDING bit was seen and cleared");
        expect(r.vtotal_reg[0] == tab::kGold60[kIdxVTotal] && r.vtotal_reg[1] == kRow2VTotalReg && r.vtotal_reg[2] == tab::kGold60[kIdxVTotal] && r.ext.lk_expect_lines == 1501, "V_TOTAL register before / during / after");
        expect(r.measure_ms == 8000 && r.ms_trial >= 7900 && r.ms_after >= 7900, "row 2's rate windows are 8 s (trial and after-restore)");
        expect(r.rate_trial_mhz >= 58800 && r.rate_trial_mhz <= 59520 && r.rate_after_mhz >= 59590 && r.rate_after_mhz <= 60310 && r.rate_expect_mhz >= 59150 && r.rate_expect_mhz <= 59155, "the trial ran at 59.15 Hz (0.6 %), the restore is back at 59.95 Hz (0.6 %)");
        expect((r.flags & N48N_MODE_F_LOCK_HELD_PROVEN) && r.ext.lk_hold_max > 100 && r.ext.lk_hold_max <= 1480, "the lock-hold witness: VERT_COUNT stayed <= 1480 for two frames while blanked and locked (LOCK_HELD_PROVEN)");
        expect(all_init(e) && (r.flags & N48N_MODE_F_RESTORED) && r.mismatch == 0 && r.latched == 0 && !e.st.lockHeld && !e.st.blanked && !e.st.drrWritten && !e.st.selWritten, "restored bit-exact, no lock held, nothing left blanked, the ownership flags clear");
        expect(r.ext.lk_db_before == 0u && r.ext.lk_db_during == 0x02000000u && r.ext.lk_db_after == 0u, "DOUBLE_BUFFER_CONTROL: 0 before, only the DRR mode after the unlock (pending clear), 0 after the restore");
    }
    // deny rules of row 2
    { Env e; e.bind(); runf(e, 2, 100, 0); expect(e.r.deny == N48N_MODE_D_STEP_DOWN && e.m.writes.empty(), "row 2 before a passed resync: STEP_DOWN, nothing written"); }
    struct Pre2 { const char *what; void (*set)(Env &); } d2[] = {
        { "UPDATE_INSTANTLY set", [](Env &e) { e.m.reg[kRegDbCtrl] = 0x100u; } }, { "a pending bit set", [](Env &e) { e.m.vtPending = true; } },
        { "the lock already requested", [](Env &e) { e.m.reg[kRegMasterLock] = 1u; e.m.lockReq = true; } }, { "DRR mode 1", [](Env &e) { e.m.reg[kRegDbCtrl] = 0x01000000u; } },
        { "unreadable DOUBLE_BUFFER_CONTROL", [](Env &e) { e.m.unreadable.insert(kRegDbCtrl); } }, { "unreadable MASTER_UPDATE_LOCK", [](Env &e) { e.m.unreadable.insert(kRegMasterLock); } } };
    for (auto &c : d2) {
        Env e; e.bind(); e.st.resyncPassed = true; c.set(e); runf(e, 2, 100, 0);
        char msg[160]; std::snprintf(msg, sizeof(msg), "row 2 (%s): DENIED (LOCK), nothing written, no latch", c.what);
        expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_LOCK && e.m.writes.empty() && e.r.latched == 0, msg);
    }
    { Env e; e.m.reg[kRegDbCtrl] = 0x02000000u; e.m.init[kRegDbCtrl] = 0x02000000u; e.bind(); e.st.resyncPassed = true; runf(e, 2, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_PASS && (e.m.reg[kRegDbCtrl] & kDbDrrMask) == 0x02000000u && e.r.ext.lk_drr_after == 2, "DRR mode already 2 is accepted; the restore puts back the golden (2), not 0"); }
    // failure branches
    struct L { const char *what; void (*set)(Env &); uint32_t verdict; uint32_t rc; bool writesV; } lf[] = {
        { "the lock status never rises", [](Env &e) { e.m.lockStatusNever = true; }, N48N_MODE_V_LOCK, N48N_MODE_SEQ_LOCK_TIMEOUT, false },
        { "the pending bits never clear (no latch)", [](Env &e) { e.m.noLatch = true; }, N48N_MODE_V_RESTORE, N48N_MODE_SEQ_PENDING_TIMEOUT, true },
        { "the latch witness never passes 1480", [](Env &e) { e.m.posClamp = true; }, N48N_MODE_V_LOCK, N48N_MODE_SEQ_LATCH, true },
        { "DRR mode write refused", [](Env &e) { e.m.refuseWr.insert(kRegDbCtrl); }, N48N_MODE_V_RESTORE, N48N_MODE_SEQ_WRITE, false },
        { "DRR mode does not read back", [](Env &e) { e.m.ignoreWr.insert(kRegDbCtrl); }, N48N_MODE_V_LOCK, N48N_MODE_SEQ_DRR, false },
        { "the lock does not hold the write (it latches under the lock)", [](Env &e) { e.m.lockLeaks = true; }, N48N_MODE_V_LOCK, N48N_MODE_SEQ_HOLD, true },
        { "the lock select write refused", [](Env &e) { e.m.refuseWr.insert(kRegGlobalCtrl2); }, N48N_MODE_V_RESTORE, N48N_MODE_SEQ_WRITE, false } };
    for (auto &c : lf) {
        Env e; e.bind(); e.st.resyncPassed = true; c.set(e); runf(e, 2, 500, 0);
        char msg[200];
        std::snprintf(msg, sizeof(msg), "row 2 (%s): verdict", c.what); expect_u(msg, e.r.verdict, c.verdict);
        std::snprintf(msg, sizeof(msg), "row 2 (%s): the apply's lock code", c.what); expect_u(msg, e.r.ext.lk_rc, c.rc);
        std::snprintf(msg, sizeof(msg), "row 2 (%s): latched, the lock is NOT left held, `blanked` recovered or reported", c.what);
        expect(e.r.latched == 1 && !e.m.lockReq && (e.m.reg[kRegMasterLock] & 1u) == 0u && !e.st.lockHeld, msg);
        std::snprintf(msg, sizeof(msg), "row 2 (%s): unlocks pair with locks", c.what); expect(lock_paired(e), msg);
        std::snprintf(msg, sizeof(msg), "row 2 (%s): the apply itself released the lock (no stuck-lock clear was needed at the restore entry)", c.what); expect(e.r.ext.lk_cleared == 0, msg);
        std::snprintf(msg, sizeof(msg), "row 2 (%s): no measurement after a failed apply", c.what); expect(e.r.frames_trial == 0, msg);
    }
    // a refused V_TOTAL write: the unlock still happens (lock pairing on every exit)
    { Env e; e.bind(); e.st.resyncPassed = true; e.m.refuseWr.insert(kCap[kIdxVTotal].abs); runf(e, 2, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_WRITE && lock_paired(e) && !e.st.lockHeld && !e.st.blanked && stream_up(e) && (e.m.reg[kRegDbCtrl] & kDbDrrMask) == 0u, "V_TOTAL write refused: verdict WRITE, the lock is released, DRR mode restored, the stream unblanked"); }
    // a mode that latches the WRONG total (1521 lines = 58.4 Hz): the witness passes (VERT_COUNT went past 1480) but the rate refuses it
    { Env e; e.bind(); e.st.resyncPassed = true; e.m.vtLatchOverride = 1520u; runf(e, 2, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_RATE && (e.r.flags & N48N_MODE_F_LATCH_CONFIRMED) && e.r.rate_trial_mhz < 58600 && all_init(e), "a V_TOTAL that latches a wrong total is caught by the rate (RATE) though the witness passed, restored"); }
    expect(!rate_within(59950, expect_mhz(row_info(2)), kProd.vtTolPermille) && rate_within(59480, expect_mhz(row_info(2)), kProd.vtTolPermille) && kProd.vtTolPermille == 6u, "the production tolerance of row 2 is 0.6 %");
    expect_u("the 8 s window is in the row-2 watchdog window", wd_window_ms(kProd, 1000, true, kProd.vtMeasureMs), 500u + 8000u + 1000u + 2000u + 4000u);
    // (item 5) a lockHeld flag with the request already 0 is cleared, so the boot is not DRIFT-denied for ever
    { Env e; e.bind(); e.st.lockHeld = true; uint32_t restored = 0; bool have = false; tIsTrial = true; const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have); tIsTrial = false;
      expect(have && bad == 0 && !e.st.lockHeld && e.m.writes.empty(), "a stale `lockHeld` (request already 0) is cleared without a write"); }
    // (item 3) restore_from_golden waits for a pending latch before it writes the DRR mode back
    // (covered by the crash test below: the golden V_TOTAL write under DRR mode 2 is pending when the DRR write comes)
    // a stuck lock request from an earlier failure is cleared at EVERY restore entry
    { Env e; e.bind(); e.m.lockReq = true; e.m.reg[kRegMasterLock] = 1u; e.st.nWritten = 0; uint32_t restored = 0; bool have = false; tIsTrial = true;
      const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have); tIsTrial = false;
      expect(have && bad == 0 && !e.m.lockReq && (e.m.reg[kRegMasterLock] & 1u) == 0u && e.st.rep.lkCleared == 1, "dcnmode 0 / kext stop: a stuck lock request is cleared first"); }
    { Env e; e.bind(); e.m.lockReq = true; e.m.reg[kRegMasterLock] = 1u; e.st.nWritten = 0; { Held h(e); restore_locked(e.hw, e.st); }
      expect(!e.m.lockReq && e.st.rep.lkCleared == 1 && e.st.restoreDone, "the trial / watchdog restore clears a stuck lock request at its entry"); }
    { Env e; e.bind(); e.st.resyncPassed = true; e.m.lockStuck = true; runf(e, 2, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_RESTORE && (e.r.flags & N48N_MODE_F_LOCK_STILL_HELD) && e.st.lockHeld && e.r.latched == 1, "a lock that will not release: the apply fails, the restore cannot clear it, RESTORE + LOCK_STILL_HELD");
      e.m.lockStuck = false; uint32_t restored = 0; bool have = false; tIsTrial = true; const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have); tIsTrial = false;
      expect(have && bad == 0 && !e.m.lockReq && !e.st.lockHeld, "once the fault clears, `dcnmode 0` releases the lock"); }
    // the real order of the PC steps: a row-1 resync that PASSES is what unlocks row 2 (no hand-set flag)
    { Env e; e.bind(); runf(e, 1, 500, 0); expect(e.r.verdict == N48N_MODE_V_PASS && e.st.resyncPassed, "row 1 passes and records it");
      runf(e, 2, 500, 0); expect(e.r.verdict == N48N_MODE_V_PASS && e.r.deny == 0 && lock_paired(e), "row 2 runs after a passed row 1 on the same boot"); }
    // a lock select that is not OTG0 at boot is put back (the trial writes OTG0's select and the restore returns the golden field)
    { Env e; e.m.reg[kRegGlobalCtrl2] = 0x02000000u; e.m.init[kRegGlobalCtrl2] = 0x02000000u; e.bind(); e.st.resyncPassed = true; runf(e, 2, 500, 0);
      expect(e.r.verdict == N48N_MODE_V_PASS && (e.m.reg[kRegGlobalCtrl2] & kSelMask) == 0x02000000u && e.r.ext.lk_sel_before == 0x02000000u, "the lock select is written OTG0 under the lock and restored to its golden field afterwards");
      bool selWritten0 = false; for (auto &w : wr_at(e, { kRegGlobalCtrl2 })) selWritten0 = selWritten0 || (w.second & kSelMask) == 0u; expect(selWritten0, "(the trial did write select = 0 first)"); }
    // dcnmode 0 after a crash mid-row-2: DRR mode 2 and a shifted V_TOTAL come back (DRR mode last)
    { Env e; e.bind(); e.m.reg[kRegDbCtrl] = 0x02000000u; e.m.reg[kCap[kIdxVTotal].abs] = 0x5dcu; e.m.reg[kRegGlobalCtrl2] = 0x02000000u; uint32_t restored = 0; bool have = false; tIsTrial = true;
      const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have); tIsTrial = false;
      expect(have && bad == 0 && restored == 3 && (e.m.reg[kRegDbCtrl] & kDbDrrMask) == 0u && (e.m.reg[kRegGlobalCtrl2] & kSelMask) == 0u && e.m.reg[kCap[kIdxVTotal].abs] == tab::kGold60[kIdxVTotal], "dcnmode 0 puts back V_TOTAL, the DRR mode and the lock select (3 writes)");
      expect(!e.m.drrWhilePending, "the DRR mode is written back only after the pending V_TOTAL latch completed");
      expect(e.m.writes.size() == 3 && e.m.writes[0].first == kCap[kIdxVTotal].abs && e.m.writes[1].first == kRegDbCtrl && e.m.writes[2].first == kRegGlobalCtrl2, "in the order: the golden set (V_TOTAL) first, then the DRR mode, then the lock select"); }
}

// ---- T10: the wrapper on row 50, the watchdog and `dcnmode 0` recovery ------------------------------------------------------------------------------------------------------
static void t10_wrapper() {
    { Env e; e.bind(); runf(e, 50, 100, N48N_MODE_TF_RESYNC); expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_STEP_DOWN && e.m.writes.empty(), "row 50 with the resync before a passed resync: STEP_DOWN, nothing written"); }
    { Env e; e.bind(); runf(e, 120, 100, N48N_MODE_TF_RESYNC); expect(e.r.deny == N48N_MODE_D_BAD_ROW && e.m.writes.empty(), "row 120 takes no flags"); }
    {
        Env e; e.bind(); e.st.resyncPassed = true; runf(e, 50, 1000, N48N_MODE_TF_RESYNC);
        const Writes &w = e.m.writes;
        expect_u("row 50 + resync PASS", e.r.verdict, N48N_MODE_V_PASS);
        expect(w.size() == 28 && match_blank(w, 0) && w[3].first == 0x141 && w[3].second == 201000000u && match_unblank(w, 4) && match_blank(w, 14) && w[17].first == 0x141 && w[17].second == e.m.init[0x141] && match_unblank(w, 18),
               "the DTO write and its restore are each enclosed in blank ... unblank (28 writes)");
        expect(p3_field_exact(e) && only_in(e, { 0x141, kRegDp1Vid, kRegDp1Steer, kRegDigFifoCtrl0 }) && all_init(e) && (e.r.flags & N48N_MODE_F_RESYNC_RUN) && e.r.ext.rs_runs == 2 && e.r.ext.tflags == 1, "field-exact; only the DTO and the P3 registers; restored; the flag word is echoed");
        expect(e.r.rate_trial_mhz >= 49400 && e.r.rate_trial_mhz <= 50400 && e.r.rate_after_mhz >= 59350 && e.r.rate_after_mhz <= 60550 && e.st.step50Passed, "49.9 Hz in the trial, 59.95 Hz after; the step-down is done");
        expect(e.r.dio_restore_errs == 0 && e.r.ext.rs_restore_rc == 0 && e.r.ext.rs_dio_errs == 0, "no DIO growth");
    }
    // the same trial when the DIO count grows ACROSS the resyncs: reported, not judged; growth after them is judged
    { Env e; e.bind(); e.st.resyncPassed = true; e.m.dioOnWrite = true; runf(e, 50, 500, N48N_MODE_TF_RESYNC);
      expect(e.r.verdict == N48N_MODE_V_PASS && e.r.dio_write_errs == 1, "the write transition's DIO growth stays reported-only with the wrapper"); }
    // the wrapper leaves the ordinary row-50 sequence alone when the flag is absent
    { Env e; e.bind(); e.st.resyncPassed = true; run(e, 50, 500); expect(e.m.writes.size() == 2 && e.r.ext.rs_runs == 0 && !(e.r.flags & N48N_MODE_F_RESYNC_RUN), "without TF_RESYNC row 50 is the 0.0.605 trial: one write, one restore write, no resync"); }
    // a failed resync inside a row-50 trial: no DTO write happens if the blank failed before touching anything
    { Env e; e.bind(); e.st.resyncPassed = true; e.m.refuseWr.insert(kRegDp1Vid); runf(e, 50, 500, N48N_MODE_TF_RESYNC);
      expect(e.r.verdict == N48N_MODE_V_RESYNC && count_addr(e, 0x141) == 0 && e.r.nwrite == 0 && stream_up(e) && (e.r.flags & N48N_MODE_F_RESTORED), "the blank failed before anything moved: the DTO is never written, RESYNC"); }
    // the watchdog restores a wrapped trial (blank, DTO, unblank) while the trial thread is hung, once
    {
        Env e; e.bind(); e.st.resyncPassed = true; e.m.hangAfterWrites = true;
        std::thread t([&e] { runf(e, 50, 1000, N48N_MODE_TF_RESYNC); });
        bool restored = false;
        for (int i = 0; i < 100000 && !restored; i++) { std::this_thread::sleep_for(std::chrono::microseconds(100)); std::lock_guard<std::mutex> g(gEng); restored = e.st.restoreDone && e.st.wdFired; }
        const bool hungThen = e.m.hung.load(); const bool streamThen = stream_up(e); const bool blankedThen = e.st.blanked;
        { std::lock_guard<std::mutex> g(e.m.mu); e.m.release = true; } e.m.cv.notify_all(); t.join(); e.join();
        expect(restored && hungThen && streamThen && !blankedThen, "the watchdog restored the wrapped trial while the trial thread was hung: stream up, `blanked` clear");
        expect(e.r.verdict == N48N_MODE_V_WATCHDOG && e.r.wd_fired == 1 && all_init(e) && count_addr(e, 0x141) == 2 && e.m.writes.size() == 28, "verdict WATCHDOG, registers exact, the DTO written once and restored once (28 writes: the restore ran ONCE)");
    }
    // dcnmode 0 (emergency_recover): a stream left blanked comes back through restore_from_golden; nothing more is needed
    {
        Env e; e.bind(); e.st.everWrote = true; e.m.vidDynamic = true; e.m.vidEn = false; e.m.reg[kRegDp1Vid] = 0x200u; e.m.vidChangeAt = 0; e.st.blanked = true;
        Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false;
        expect(rc.have && rc.bad == 0 && !e.st.blanked && rc.healthyAfter && !rc.resynced && stream_up(e), "dcnmode 0: a blanked stream is unblanked by the golden restore; the back end is healthy, no second resync");
    }
    // dcnmode 0 with a sick back end (steer overflow flag): the golden restore, then the resync, then a second look
    {
        Env e; e.bind(); e.st.everWrote = true; e.m.reg[kRegDp1Steer] |= 0x10u;
        Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false;
        expect(rc.have && rc.bad == 0 && !rc.healthyBefore && rc.resynced && rc.rc == N48N_MODE_SEQ_OK && rc.healthyAfter && match_blank(e.m.writes, 0) && match_unblank(e.m.writes, 3) && p3_field_exact(e) && only_in(e, kP3Regs), "dcnmode 0 with the steer overflow flag set: restore, then the DP1 resync (Linux order, field-exact), then healthy");
    }
    // the resync never writes the DIG error bits (b29:28) or RESET_DONE even when the register reads them set
    { Env e; e.bind(); e.st.everWrote = true; e.m.reg[kRegDigFifoCtrl0] |= 0x30000000u; Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false;
      expect(rc.resynced && !rc.healthyBefore && p3_field_exact(e), "dcnmode 0 with the DIG FIFO error bits set: the resync runs and every write is still field-exact (never writes the error bits back)"); }
    // ... a DIO_FIFO_ERROR alone also triggers it; a healthy back end does not
    { Env e; e.bind(); e.st.everWrote = true; e.m.reg[kRegPixelRateCntl] |= 0x4000u; Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false; expect(rc.resynced && !rc.healthyBefore, "DIO_FIFO_ERROR set: the resync runs"); }
    { Env e; e.bind(); e.st.everWrote = true; Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false; expect(rc.have && rc.healthyBefore && !rc.resynced && e.m.writes.empty(), "a healthy machine: no write at all"); }
    { Env e; e.st.everWrote = true; Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false; expect(!rc.have && e.m.writes.empty() && !rc.resynced, "no golden copy: nothing is touched and no resync runs"); }
    // dcnmode 0 with the resync failing: reported, not hidden
    { Env e; e.bind(); e.st.everWrote = true; e.m.reg[kRegDp1Steer] |= 0x10u; e.m.refuseWr.insert(kRegDp1Steer); Recover rc; tIsTrial = true; emergency_recover(e.hw, e.st, &rc); tIsTrial = false;
      expect(rc.resynced && rc.rc != N48N_MODE_SEQ_OK && !rc.healthyAfter, "a resync that fails is reported (rc, still unhealthy)"); }
    // the ownership flags: `blanked` and `lockHeld` are set before the write that makes them true
    { Env e; e.bind(); e.m.refuseWr.insert(kRegDp1Steer); Held h(e); (void)blank_locked(e.hw, e.st); expect(e.st.blanked, "a blank whose last write failed still owns `blanked` (recorded before the write that stops the stream)"); }
    { Env e; e.bind(); e.m.refuseWr.insert(kRegMasterLock); Held h(e); (void)lock_locked(e.hw, e.st); expect(e.st.lockHeld && e.st.selWritten, "a refused lock request still owns `lockHeld`; the select is recorded before it is written"); }
    { Env e; e.bind(); e.m.refuseWr.insert(kRegDbCtrl); Held h(e); (void)drr_set_locked(e.hw, e.st, 2); expect(e.st.drrWritten, "a refused DRR write still owns `drrWritten`"); }
}

// ---- T11: the addresses the engine writes -------------------------------------------------------------------------------------------------------------------------------------
static void t11_addresses() {
    struct dcn41_allow_state al; std::memset(&al, 0, sizeof(al)); const uint32_t seg[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x02403c00u }; (void)dcn41_allow_init(&al, seg, 262144u);
    const uint32_t pw[] = { kRegHubpCntl, kRegOptcInGlobal, kRegDbCtrl, kRegMasterLock, kRegGlobalCtrl2, kRegDp1Vid, kRegDp1Steer, kRegDigFifoCtrl0 };
    for (uint32_t a : pw) expect(dcn41_allow_write(&al, a, 0u, "t11"), "every register of the underflow / lock / resync sequences is admitted by the real DCN write allowlist");
    // read-only witnesses of the sequences are not written by any test's engine; the writes of ALL tests are the capture set plus exactly these eight
    std::set<uint32_t> ok(pw, pw + sizeof(pw) / sizeof(pw[0])); for (uint32_t i = 0; i < kCapN; i++) ok.insert(kCap[i].abs);
    bool inside = true; { std::lock_guard<std::mutex> g(gAllWrMu); for (uint32_t a : gAllWr) inside = inside && ok.count(a) != 0; }
    expect(inside, "across every test the engine only ever tried to write the capture set and the eight P1 / P2 / P3 registers (never DP0 / DP2 / DP3, DIG_BE, the PHY, the link, a clock, a pipe-update-status or STATUS_POSITION)");
    for (uint32_t a : { kRegDpStream[0], kRegDpStream[2], kRegDpStream[3], kRegPipeUpd, kRegStatusPos, kRegPixelRateCntl }) { std::lock_guard<std::mutex> g(gAllWrMu); expect(gAllWr.count(a) == 0, "a witness / other-stream register was never written"); }
}

// ---- S: source pins ----------------------------------------------------------------------------------------------------------------------------------------------
// ---- T12 (0.0.607): row 120 end to end, the clock hold, and a failure injected at EVERY phase ------------------------------------------------------------
// The engine is the REAL run_trial / restore; hold_raise / hold_release are the REAL functions (smu_dal_pure.h) driven against a model of the PMFW + DFS. Prerequisites are earned the way a run earns them.
static bool prep120(Env &e) {
    e.bind(); run(e, 50, 300);
    if (e.r.verdict != N48N_MODE_V_PASS) return false;
    run(e, 1, 100); if (e.r.verdict != N48N_MODE_V_PASS) return false;
    run(e, 2, 300); if (e.r.verdict != N48N_MODE_V_PASS) return false;
    return e.st.step50Passed && e.st.resyncPassed && e.st.row2Proven && e.st.latched == false;
}
static long tpos(const std::vector<std::string> &t, const std::string &x, size_t from = 0) { for (size_t i = from; i < t.size(); i++) if (t[i] == x) return (long)i; return -1; }
static long tposp(const std::vector<std::string> &t, const std::string &prefix, size_t from = 0) { for (size_t i = from; i < t.size(); i++) if (t[i].compare(0, prefix.size(), prefix) == 0) return (long)i; return -1; }
static std::string hx(uint32_t v) { char b[16]; std::snprintf(b, sizeof(b), "%x", v); return b; }
static bool is_dto(uint32_t a) { return a == 0x141u || a == 0x142u || a == 0x12fu; }
static bool is_msa(uint32_t a) { return a >= 0x5746u && a <= 0x5749u; }
static uint32_t trace_abs(const std::string &t) { return t.size() > 2 && t[0] == 'W' ? (uint32_t)std::strtoul(t.c_str() + 2, nullptr, 16) : 0u; }
static bool in_cap(uint32_t a) { return cap_pos(a) != (size_t)-1; }
static bool healthy_end(Env &e) { return !e.m.lockReq && e.m.vidEn && (e.m.reg[kRegDp1Vid] & 1u) != 0u; }

static void t12_row120() {
    // ---- the clean trial, with the exact order of every step ----
    {
        Env e; expect(prep120(e), "(setup) row 50, the resync and the row-2 lock-hold proof all PASS on this boot");
        const uint32_t vt0 = e.m.reg[kCap[kIdxVTotal].abs];
        e.m.writes.clear(); e.m.trace.clear(); e.m.ev.clear(); e.m.holdMsgs = e.m.holdCalls = 0;
        run(e, 120, 1500);
        const n48n_mode_result &r = e.r; const auto &t = e.m.trace;
        expect_u("row 120 PASS", r.verdict, N48N_MODE_V_PASS); expect_u("... deny 0", r.deny, 0); expect_u("... no latch", r.latched, 0);
        expect(r.rate_trial_mhz >= 118800 && r.rate_trial_mhz <= 121200 && r.rate_expect_mhz >= 119990 && r.rate_expect_mhz <= 120000, "the measured trial rate is 119.998 Hz within 1 %");
        expect(r.rate_after_mhz >= 59350 && r.rate_after_mhz <= 60550 && (r.flags & N48N_MODE_F_RATE_AFTER_OK) && (r.flags & N48N_MODE_F_RATE_TRIAL_OK), "the 60 Hz rate is back and verified");
        expect(r.nwrite > 20 && r.nwrite <= kCapN - 1u && r.nwrite + r.nskip == kCapN - 1u, "the whole DLG/TTU + timing + DTO + MSA set bar OTG_H_TIMING_CNTL is written (written + skipped = the row's 55 registers)");
        { bool w4fee = false; for (auto &t : e.m.trace) if (t.compare(0, 6, "W:4fee") == 0) w4fee = true; expect(!w4fee, "row 120 (apply and restore) never writes 0x4fee"); }
        expect(all_init(e) && r.mismatch == 0 && (r.flags & N48N_MODE_F_RESTORED) && (r.flags & N48N_MODE_F_REGS_OK), "after row 120 every register is bit-exact the golden copy");
        expect(healthy_end(e) && !(r.flags & N48N_MODE_F_STILL_BLANKED) && !(r.flags & N48N_MODE_F_LOCK_STILL_HELD), "the stream is up and the OTG lock is released");
        expect((r.flags & N48N_MODE_F_LOCK_USED) && (r.flags & N48N_MODE_F_LATCH_CONFIRMED) && r.ext.lk_drr_during == 2 && r.ext.lk_drr_after == 0, "DRR mode 2 while locked, back to 0 after; the V_TOTAL latch was confirmed");
        expect(r.ext.lk_vert_max > 1480 && r.ext.lk_vert_max <= 1524, "the latch witness saw VERT_COUNT past the old 1480 maximum");
        // the clock hold, reported
        expect((r.flags & N48N_MODE_F_HOLD_HELD) && (r.flags & N48N_MODE_F_HOLD_RELEASED) && !(r.flags & N48N_MODE_F_HOLD_STUCK), "flags: HOLD_HELD and HOLD_RELEASED, not STUCK");
        expect(r.ext.hold_state == n48dal::kHsReleased && r.ext.hold_rc == 0 && r.ext.hold_rel_rc == 0 && r.ext.hold_pre == 0 && r.ext.hold_msgs == 4, "hold state RELEASED, rc 0, 4 DAL messages");
        expect(r.ext.hold_khz_raised[0] == 545454 && r.ext.hold_khz_raised[1] == 545454 && r.ext.hold_khz_released[0] == 272727 && r.ext.hold_khz_released[1] == 272727, "DFS 545.45 MHz after the raise, 272.73 MHz after the release");
        expect(r.disp_khz == 545454 && r.dpp_khz == 545454 && r.need_disp_khz == 514285, "the result's clock words are the readback AFTER the raise");
        expect(r.ext.hold_samples >= 12 && r.ext.hold_lost == 0 && r.ext.hold_dfs_min[0] >= 514285 && r.ext.hold_dfs_min[1] >= 500000, "the DFS was sampled every 250 ms through the rate window and the dwell: never under the need");
        expect(e.m.holdCalls == 2 && e.m.holdUnderLock == 0 && e.m.holdFromWd == 0, "the hold was raised and released once each, never under the engine lock, never from the watchdog");
        // step order, from the write trace
        const long hmA = tpos(t, "HM:6:530"), hmB = tpos(t, "HM:7:530"), wd = tpos(t, "WD"), vid1 = tposp(t, "W:5706="), drr1 = tposp(t, "W:501c="), sel1 = tposp(t, "W:5050="), lk1 = tpos(t, "W:5049=1");
        expect(hmA >= 0 && hmB > hmA && wd > hmB && vid1 > wd, "(1) the clocks: DISPCLK 530 then DPPCLK 530, BEFORE the watchdog and the first display write");
        expect(drr1 > vid1 && sel1 > drr1 && lk1 > sel1, "(2) DP1 blank, (3) DRR mode 2, the lock select, the lock request - in that order");
        const long unlk = tpos(t, "W:5049=0", (size_t)lk1 + 1);
        expect(unlk > lk1, "the unlock follows the lock");
        long firstOtg = -1, lastOtg = -1, firstDto = -1, firstMsa = -1; std::vector<uint32_t> applyOtg;
        for (size_t i = (size_t)lk1; i < t.size() && (long)i < unlk; i++) { const uint32_t a = trace_abs(t[i]); if (in_cap(a)) { if (firstOtg < 0) firstOtg = (long)i; lastOtg = (long)i; applyOtg.push_back(a); if (is_dto(a) || is_msa(a)) firstOtg = -2; } }
        expect(firstOtg >= 0 && lastOtg > firstOtg && !applyOtg.empty(), "(4) the OTG timing / global-sync / VTG / HUBP registers are written between the lock and the unlock, and NONE of them is a DTO or MSA register");
        const std::string dtoApply = "W:141=" + hx(tab::kGold120[kIdxDtoPhase]);
        firstDto = tpos(t, dtoApply); firstMsa = tposp(t, "W:5746=");
        expect(firstDto > unlk && firstMsa > firstDto, "(5)-(7) after the unlock: the DTO (pixel clock moves AFTER the unlock), then the MSA");
        // no DTO / MSA write inside the lock
        bool clean = true; for (long i = lk1; i < unlk; i++) { const uint32_t a = trace_abs(t[i]); if (is_dto(a) || is_msa(a)) clean = false; }
        expect(clean, "no DTO or MSA write while the OTG lock is held");
        const long unb = tpos(t, "W:5706=10201", (size_t)firstMsa);   // the unblank's last ENABLE (STATUS is read-only: written 0)
        expect(unb > firstMsa || tposp(t, "W:5706=", (size_t)firstMsa) > firstMsa, "(8) the DP1 unblank comes after the MSA");
        // the restore, in the P5 order: DTO golden first, then lock, OTG/HUBP in reverse, unlock, then the MSA, DRR, unblank
        const std::string dtoRest = "W:141=" + hx(e.m.init[0x141]);
        const long rDto = tpos(t, dtoRest, (size_t)firstMsa), rLock = tpos(t, "W:5049=1", (size_t)rDto), rUnlk = tpos(t, "W:5049=0", (size_t)rLock + 1), rMsa = tposp(t, "W:5746=", (size_t)rUnlk);
        expect(rDto > firstMsa && rLock > rDto && rUnlk > rLock && rMsa > rUnlk, "(12) the restore: DTO golden FIRST (before the lock), then lock, OTG/HUBP, unlock, then the MSA");
        std::vector<uint32_t> restOtg; for (long i = rLock; i < rUnlk; i++) { const uint32_t a = trace_abs(t[i]); if (in_cap(a)) restOtg.push_back(a); }
        std::vector<uint32_t> rev(applyOtg.rbegin(), applyOtg.rend());
        expect(restOtg == rev, "the OTG/HUBP restore writes are the apply's writes in REVERSE order");
        bool dtoBefore = true; for (long i = rDto; i < rLock; i++) { const uint32_t a = trace_abs(t[i]); if (in_cap(a) && !is_dto(a)) dtoBefore = false; }
        expect(dtoBefore, "between the DTO restore and the lock request only DTO registers are written");
        const long rDrr = tposp(t, "W:501c=", (size_t)rMsa);
        expect(rDrr > rMsa, "the DRR mode goes back after the MSA");
        long lastW = -1; for (size_t i = 0; i < t.size(); i++) if (t[i][0] == 'W') lastW = (long)i;
        const long relA = tpos(t, "HM:7:272"), relB = tpos(t, "HM:6:272");
        expect(relA > lastW && relB > relA, "(14) the clocks are released ONLY after the last register write of the restore: DPPCLK then DISPCLK to 272");
        // every recorded write / restore of this trial passes the real allowlist
        struct dcn41_allow_state al; std::memset(&al, 0, sizeof(al)); const uint32_t seg[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x02403c00u }; (void)dcn41_allow_init(&al, seg, 262144u);
        bool allowed = true; for (auto &w : e.m.writes) allowed = allowed && dcn41_allow_write(&al, w.first, w.second, "t12");
        expect(allowed, "every write of the trial (sequences included) passes the real DCN allowlist");
        // apply values are rmw(live, mask, golden) for the OTG group
        bool val = true; for (long i = lk1; i < unlk; i++) { const uint32_t a = trace_abs(t[i]); const size_t pf = cap_pos(a); if (pf == (size_t)-1) continue;
            const uint32_t v = (uint32_t)std::strtoul(t[i].c_str() + t[i].find('=') + 1, nullptr, 16); val = val && v == ((e.m.init[a] & ~kCap[pf].mask) | (tab::kGold120[pf] & kCap[pf].mask)); }
        expect(val, "each OTG/HUBP write is the read-modify-write of the live value under the golden mask");
        expect(e.m.reg[kCap[kIdxVTotal].abs] == vt0 && e.st.doneId == e.st.trialId && e.st.busy == 0, "state after: V_TOTAL golden, trial done, busy released") ;
    }

    // ---- a failure injected at EVERY phase: the restore always runs; the clocks are released ONLY when the restore verified ----
    enum Hd { kRel, kStuck, kIdle };   // the expected hold state after the trial: RELEASED (clocks came off), STUCK (clocks stay up), IDLE (a raise that never sent anything)
    struct FC { const char *what; uint32_t verdict; uint32_t deny; Hd hold; bool regsExact; bool wrote; void (*set)(Env &); };
    const uint32_t kSeqStart = 0;   // (documentation only: the timeline is trial start + ~2.65 s for the rate window)
    (void)kSeqStart;
    const FC cases[] = {
        // ---- phase 1: the clock hold (nothing is written; the hold unwinds itself) ----
        { "hold: the first hard-min fails (mailbox alive)",        N48N_MODE_V_DENIED, N48N_MODE_D_HOLD, kIdle, true, false, [](Env &e) { e.m.holdFailAt = 0; e.m.holdFailKind = n48dal::kHmFailed; } },
        { "hold: DPPCLK's hard-min fails after DISPCLK was raised",  N48N_MODE_V_DENIED, N48N_MODE_D_HOLD, kRel,  true, false, [](Env &e) { e.m.holdFailAt = 1; e.m.holdFailKind = n48dal::kHmFailed; } },
        { "hold: the 0x15 poll times out (both raised, unwound)",    N48N_MODE_V_DENIED, N48N_MODE_D_HOLD, kRel,  true, false, [](Env &e) { e.m.holdFailAt = 1; e.m.holdFailKind = n48dal::kHmPollTimeout; } },
        { "hold: the mailbox stops answering (STUCK, latched)",      N48N_MODE_V_DENIED, N48N_MODE_D_HOLD, kStuck, true, false, [](Env &e) { e.m.holdFailAt = 1; e.m.holdFailKind = n48dal::kHmMailbox; } },
        { "hold: the DFS never reaches the need (unwound)",          N48N_MODE_V_DENIED, N48N_MODE_D_HOLD, kRel,  true, false, [](Env &e) { e.m.holdNeverMove = true; } },
        // ---- phase 2/3: blank, DRR mode, lock ----
        { "blank: STATUS never drops",                               N48N_MODE_V_RESTORE, 0, kStuck, true, true, [](Env &e) { e.m.noStatusDrop = true; } },
        { "DRR mode write does not take",                            N48N_MODE_V_LOCK,    0, kRel,  true, true, [](Env &e) { e.m.drrIgnore = true; } },
        { "lock: UPDATE_LOCK_STATUS never rises",                    N48N_MODE_V_LOCK,    0, kRel,  true, true, [](Env &e) { e.m.lockStatusNever = true; } },
        // ---- phase 4: an OTG / HUBP write refused mid-way ----
        { "OTG/HUBP: a mid-set write is refused (PREFETCH_SETTINGS)", N48N_MODE_V_WRITE,  0, kRel,  true, true, [](Env &e) { e.m.refuseWr.insert(kCap[cap_find("HUBPREQ0_PREFETCH_SETTINGS")].abs); } },
        { "OTG/HUBP: the first write is refused (V_TOTAL)",          N48N_MODE_V_WRITE,   0, kRel,  true, true, [](Env &e) { e.m.refuseWr.insert(kCap[kIdxVTotal].abs); } },
        // ---- phase 5: unlock / latch ----
        { "latch: VERT_COUNT never passes the old maximum (the witness fails)", N48N_MODE_V_LOCK, 0, kRel, true, true, [](Env &e) { e.m.posClamp = true; } },
        { "unlock: the lock request will not clear",                 N48N_MODE_V_RESTORE, 0, kStuck, false, true, [](Env &e) { e.m.lockStuck = true; } },
        { "latch: V_TOTAL never latches",                            N48N_MODE_V_RESTORE, 0, kStuck, true, true, [](Env &e) { e.m.noLatch = true; } },
        // ---- phases 6/7: DTO, MSA ----
        { "DTO write refused",                                       N48N_MODE_V_WRITE,   0, kRel,  true, true, [](Env &e) { e.m.refuseWr.insert(0x141); } },
        { "DTO write accepted but the pixel clock does not move",    N48N_MODE_V_RATE,    0, kRel,  true, true, [](Env &e) { e.m.ignoreDto = true; } },
        { "MSA write refused",                                       N48N_MODE_V_WRITE,   0, kRel,  true, true, [](Env &e) { e.m.refuseWr.insert(0x5746); } },
        // ---- phase 8: unblank ----
        { "unblank: the stream never reads active",                  N48N_MODE_V_RESTORE, 0, kStuck, true, true, [](Env &e) { e.m.neverActive = true; e.m.neverActiveLate = true; } },
        // ---- phases 9-11: settle, measure, dwell ----
        { "underflow in the rate window",                            N48N_MODE_V_UNDERFLOW, 0, kRel, true, true, [](Env &e) { e.m.ufAtUs = e.m.vnow + 4000000ull; } },
        { "DIO error count keeps growing",                           N48N_MODE_V_FIFO,    0, kRel,  true, true, [](Env &e) { e.m.fifoSteady = true; } },
        { "the DP stream drops (the unblank then finds it down)",   N48N_MODE_V_RESYNC,  0, kRel,  true, true, [](Env &e) { e.m.streamDownOnDto = true; } },
        { "the frame counter stalls",                                N48N_MODE_V_STALL,   0, kRel,  true, true, [](Env &e) { e.m.stallOnDto = true; } },
        { "the DFS readback falls under the need (CLOCK_LOST)",      N48N_MODE_V_CLOCK_LOST, 0, kRel, true, true, [](Env &e) { e.m.clkDropAtUs = e.m.vnow + 4000000ull; } },
        { "the DFS readback becomes unreadable at the sample",       N48N_MODE_V_CLOCK_LOST, 0, kRel, true, true, [](Env &e) { e.m.clkDropAtUs = e.m.vnow + 4000000ull; e.m.clkUnreadableOnDrop = true; } },
        // ---- phase 12/13: the restore itself ----
        { "restore: the DTO restore does not take",                  N48N_MODE_V_RESTORE, 0, kStuck, false, true, [](Env &e) { e.m.dtoStaysAfterRestore = true; } },
        { "restore: the DTO restore write is refused",               N48N_MODE_V_RESTORE, 0, kStuck, false, true, [](Env &e) { e.m.refuseRestoreDto = true; } },
        { "restore: the DIG FIFO error bits stay set after the resync", N48N_MODE_V_RESTORE, 0, kStuck, true, true, [](Env &e) { e.m.digStuck = true; } },
        { "restore: DIO_FIFO_ERROR stays set after the resync",      N48N_MODE_V_RESTORE, 0, kStuck, true, true, [](Env &e) { e.m.fifoBitStuck = true; } },
        // ---- phase 14: the release ----
        { "release: the first release message fails (DPPCLK stays up)", N48N_MODE_V_HOLD, 0, kStuck, true, true, [](Env &e) { e.m.holdFailAt = 2; e.m.holdFailKind = n48dal::kHmFailed; } },
        { "release: the mailbox stops answering",                    N48N_MODE_V_HOLD,    0, kStuck, true, true, [](Env &e) { e.m.holdFailAt = 2; e.m.holdFailKind = n48dal::kHmMailbox; } },
        { "release: the clocks do not come off the need",            N48N_MODE_V_HOLD,    0, kStuck, true, true, [](Env &e) { e.m.holdNoDrop = true; } },
    };
    for (const FC &c : cases) {
        Env e; expect(prep120(e), "(setup) prerequisites");
        e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0; e.m.hangAfterWrites = false;
        c.set(e);
        // the hold's own message counter starts at 0 for the trial: the raise is messages 0 and 1, the release 2 and 3
        run(e, 120, 600);
        char w[220];
        std::snprintf(w, sizeof(w), "row 120 [%s]: verdict", c.what); expect_u(w, e.r.verdict, c.verdict);
        if (c.verdict == N48N_MODE_V_DENIED) { std::snprintf(w, sizeof(w), "row 120 [%s]: deny", c.what); expect_u(w, e.r.deny, c.deny); }
        std::snprintf(w, sizeof(w), "row 120 [%s]: latch rule (a failure verdict latches, a denial does not)", c.what);
        expect(is_failure(e.r.verdict) ? (e.r.latched == 1 && e.st.latched) : (e.r.latched == 0 && !e.st.latched), w);
        std::snprintf(w, sizeof(w), "row 120 [%s]: %s", c.what, c.wrote ? "registers written" : "NOTHING written");
        expect(c.wrote ? (e.r.flags & N48N_MODE_F_WROTE) != 0 : (e.m.writes.empty() && !(e.r.flags & N48N_MODE_F_WROTE)), w);
        std::snprintf(w, sizeof(w), "row 120 [%s]: registers %s", c.what, c.regsExact ? "bit-exact the golden copy (the restore ALWAYS ran)" : "reported as still differing");
        if (c.regsExact) expect(all_init(e) && e.r.mismatch == 0, w);
        if (std::string(c.what).find("PREFETCH_SETTINGS") != std::string::npos) {
            std::snprintf(w, sizeof(w), "row 120 [%s]: the refused register is the last in the restore list (recorded BEFORE the write), nothing after it and no DTO / MSA register was written", c.what);
            bool later = false; for (auto &wr : e.m.writes) if (in_cap(wr.first) && cap_pos(wr.first) > cap_find("HUBPREQ0_PREFETCH_SETTINGS")) later = true;
            expect(e.st.nWritten >= 1 && e.st.written[e.st.nWritten - 1] == cap_find("HUBPREQ0_PREFETCH_SETTINGS") && !later, w);
        }
        if (std::string(c.what).find("the first write is refused") != std::string::npos) {
            std::snprintf(w, sizeof(w), "row 120 [%s]: the refused first write is the ONE recorded register", c.what);
            expect(e.st.nWritten == 1 && e.st.written[0] == kIdxVTotal, w);
        }
        std::snprintf(w, sizeof(w), "row 120 [%s]: no clock message from the watchdog, none under the engine lock", c.what); expect(e.m.holdFromWd == 0 && e.m.holdUnderLock == 0, w);
        const uint32_t hs = n48dal::hold_state_now(e.hc);
        std::snprintf(w, sizeof(w), "row 120 [%s]: hold state %s", c.what, c.hold == kRel ? "RELEASED" : c.hold == kStuck ? "STUCK" : "IDLE");
        expect_u(w, hs, c.hold == kRel ? n48dal::kHsReleased : c.hold == kStuck ? n48dal::kHsStuck : n48dal::kHsIdle);
        std::snprintf(w, sizeof(w), "row 120 [%s]: the clocks are %s in the model", c.what, c.hold == kRel || c.hold == kIdle ? "back at the start" : "still up");
        if (c.hold != kStuck) expect(e.m.did[0] == 0x41 && e.m.did[1] == 0x41, w);
        std::snprintf(w, sizeof(w), "row 120 [%s]: the mailbox owner word follows the state", c.what);
        expect_u(w, e.hOwner, c.hold == kStuck ? n48dal::kOwnerHold : n48dal::kOwnerIdle);
        // a release message is sent only when the restore verified: never after a RESTORE verdict, never with a stuck raise
        const bool releaseSent = tpos(e.m.trace, "HM:7:272") >= 0 || tpos(e.m.trace, "HM:6:272") >= 0;
        if (e.r.verdict == N48N_MODE_V_RESTORE) { std::snprintf(w, sizeof(w), "row 120 [%s]: RESTORE verdict: NO release message was sent (restoreBad never releases)", c.what); expect(!releaseSent, w); }
        if (c.hold == kRel && c.verdict != N48N_MODE_V_DENIED) { std::snprintf(w, sizeof(w), "row 120 [%s]: the release messages came after the last register write", c.what);
            long lastW = -1; for (size_t i = 0; i < e.m.trace.size(); i++) if (e.m.trace[i][0] == 'W') lastW = (long)i;
            expect(tpos(e.m.trace, "HM:7:272") > lastW && tpos(e.m.trace, "HM:6:272") > lastW, w); }
        if (c.hold == kStuck) { std::snprintf(w, sizeof(w), "row 120 [%s]: HOLD_STUCK reported and the DAL latch set", c.what); expect((e.r.flags & N48N_MODE_F_HOLD_STUCK) && e.m.holdLatches >= 1, w); }
        if (c.hold == kRel && c.verdict != N48N_MODE_V_DENIED) { std::snprintf(w, sizeof(w), "row 120 [%s]: HOLD_RELEASED reported", c.what); expect((e.r.flags & N48N_MODE_F_HOLD_RELEASED) && !(e.r.flags & N48N_MODE_F_HOLD_STUCK), w); }
        if (c.regsExact && c.hold != kStuck) { std::snprintf(w, sizeof(w), "row 120 [%s]: the stream is up and the lock is released", c.what); expect(healthy_end(e), w); }
        if (e.r.verdict != N48N_MODE_V_DENIED) {
            std::snprintf(w, sizeof(w), "row 120 [%s]: the next trial is refused by the latch, and touches no clock", c.what);
            const int msgs0 = e.m.holdMsgs; e.m.writes.clear(); run(e, 120, 100);
            expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_LATCHED && e.m.writes.empty() && e.m.holdMsgs == msgs0, w);
        }
    }
    // the restore's FIFO health of a WRAPPED trial is judged from the end of its resync (0.0.606 design): a DIO count step at the DTO restore is REPORTED there, not judged
    { Env e; expect(prep120(e), "(setup)"); e.m.dioOnRestore = true; run(e, 120, 300);
      expect(e.r.verdict == N48N_MODE_V_PASS && e.r.ext.rs_restore_dio_errs == 1 && n48dal::hold_state_now(e.hc) == n48dal::kHsReleased, "row 120: a DIO error-count step at the DTO restore is absorbed by the restore's resync and reported (rs_restore_dio_errs 1), as for the wrapped row 50"); }
    // row 120's prerequisite from a row-2 report: PASS with BOTH the lock-hold witness and the confirmed latch, and nothing else
    expect(row2_proof(N48N_MODE_V_PASS, 2, true, true) && !row2_proof(N48N_MODE_V_PASS, 2, false, true) && !row2_proof(N48N_MODE_V_PASS, 2, true, false) && !row2_proof(N48N_MODE_V_LOCK, 2, true, true) &&
           !row2_proof(N48N_MODE_V_PASS, 1, true, true) && !row2_proof(N48N_MODE_V_PASS, 50, true, true) && !row2_proof(N48N_MODE_V_PASS, 120, true, true), "row2_proof: only a row-2 PASS with the lock-hold witness AND the latch confirmation");
    // S4: an abort that lands while the hold is being raised: released at 60 Hz, nothing written, DENIED (ABORTED)
    { Env e; expect(prep120(e), "(setup)"); e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0; e.m.abortDuringRaise = true; run(e, 120, 300);
      expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == N48N_MODE_D_ABORTED && e.m.writes.empty() && n48dal::hold_state_now(e.hc) == n48dal::kHsReleased && e.m.did[0] == 0x41 && e.hOwner == 0 && e.st.busy == 0 && e.m.holdMsgs == 4,
             "abort during the raise: DENIED (ABORTED), the hold released at 60 Hz (4 messages), nothing written"); }
    // M1 + generic: no write mask of any row covers an enable bit of OTG / VTG / HUBP / DP registers, unless allow-listed
    {
        expect((kCap[cap_find("VTG0_CONTROL")].mask & 0x80000000u) == 0u && kCap[cap_find("VTG0_CONTROL")].mask == 0x7fff7fffu, "VTG0_CONTROL's write mask excludes VTG0_ENABLE (bit 31): FP2 + VCOUNT_INIT only, as optc1_set_vtg_params");
        const uint32_t rows[] = { 1, 2, 50, 120 };
        struct En { const char *name; uint32_t bits; }; 
        // enable bits by register name: OTG_CONTROL / DP stream / HUBP blank are not in kCap at all; the ones that are in kCap and carry an enable are listed with their bit
        const En enables[] = { { "VTG0_CONTROL", 0x80000000u }, { "OTG0_OTG_H_SYNC_A_CNTL", 0u }, { "OTG0_OTG_V_SYNC_A_CNTL", 0u } };
        for (const En &en : enables) expect((kCap[cap_find(en.name)].mask & en.bits) == 0u, "an enable bit is outside the write mask");
        bool ok = true;
        for (uint32_t r : rows) { const RowInfo ri = row_info(r);
            for (uint32_t k = 0; k < ri.n; k++) { const uint32_t i = ri.idx[k]; const std::string nm = kCap[i].name;
                const bool enName = nm.find("_ENABLE") != std::string::npos || nm.find("_EN") == nm.size() - 3 || nm.find("MASTER_EN") != std::string::npos || nm.find("STREAM_CNTL") != std::string::npos || nm.find("OTG_CONTROL") != std::string::npos || nm.find("DCHUBP_CNTL") != std::string::npos;
                if (enName) ok = false; } }
        expect(ok, "no register written by any row is an enable / stream-control / OTG_CONTROL / DCHUBP_CNTL register");
        // the golden values never set an enable the mask could carry: VTG0_CONTROL bit 31 of every golden
        expect((tab::kGold60[cap_find("VTG0_CONTROL")] & 0x80000000u) == 0u && (tab::kGold50[cap_find("VTG0_CONTROL")] & 0x80000000u) == 0u && (tab::kGold120[cap_find("VTG0_CONTROL")] & 0x80000000u) == 0u, "the goldens carry no VTG0_ENABLE either");
        // the sequence writers' fields: DP1_DP_VID_STREAM_CNTL ENABLE and DIG FIFO ENABLE are written ONLY by the named P3 field writes (never by a kCap write)
        bool notCap = !cap_has_abs(kRegDp1Vid) && !cap_has_abs(kRegDigFifoCtrl0) && !cap_has_abs(kRegDp1Steer) && !cap_has_abs(0x5003u);
        expect(notCap, "OTG_CONTROL (master enable), the DP1 stream / steer / DIG FIFO registers are never in the golden capture set");
        // the model: a full row 120 leaves VTG0_CONTROL bit 31 exactly as it was (a live value with bit 31 set survives, junk included)
        Env e; expect(prep120(e), "(setup)"); e.m.reg[0x39f0] |= 0x80000000u; e.m.init[0x39f0] = e.m.reg[0x39f0]; e.m.trace.clear(); run(e, 120, 100);
        bool wroteBit = false; for (auto &t : e.m.trace) if (t.compare(0, 6, "W:39f0") == 0) { const uint32_t v = (uint32_t)std::strtoul(t.c_str() + 7, nullptr, 16); if ((v & 0x80000000u) == 0u) wroteBit = true; }
        expect(!wroteBit && (e.m.reg[0x39f0] & 0x80000000u) != 0u, "row 120 with VTG0_ENABLE set live: every VTG0_CONTROL write keeps bit 31 and it is still set afterwards");
    }
    // ---- a STUCK hold is not available again; RELEASED is ----
    { Env e; expect(prep120(e), "(setup)"); run(e, 120, 300); expect_u("a clean row 120 PASSes", e.r.verdict, N48N_MODE_V_PASS);
      run(e, 120, 300); expect(e.r.verdict == N48N_MODE_V_PASS && (e.r.flags & N48N_MODE_F_HOLD_RELEASED) && e.r.ext.hold_msgs == 4, "a second row 120 after a released hold PASSes again (RELEASED -> RAISING -> HELD -> RELEASED)"); }
    // ---- refusals before any clock message: prerequisites, the hold precondition, F6 / F7 / P1 baselines, the switch ----
    struct PC { const char *what; uint32_t deny; void (*set)(Env &); };
    const PC pcs[] = {
        { "no row-50 pass",                     N48N_MODE_D_STEP_DOWN, [](Env &e) { e.st.step50Passed = false; } },
        { "no row-1 resync pass",               N48N_MODE_D_STEP_DOWN, [](Env &e) { e.st.resyncPassed = false; } },
        { "no row-2 proof (LOCK_HELD_PROVEN)",  N48N_MODE_D_ROW2,      [](Env &e) { e.st.row2Proven = false; } },
        { "E1b not run on this boot",           N48N_MODE_D_HOLD,      [](Env &e) { e.hE1b = false; } },
        { "boot-arg level below 4",             N48N_MODE_D_HOLD,      [](Env &e) { e.hLevel = 3; } },
        { "the DAL latch is set",               N48N_MODE_D_HOLD,      [](Env &e) { e.hStopped = true; } },
        { "a DAL step holds the mailbox",       N48N_MODE_D_HOLD,      [](Env &e) { e.hOwner = n48dal::kOwnerDal; } },
        { "the hold is STUCK from an earlier trial", N48N_MODE_D_HOLD, [](Env &e) { e.hc.state = n48dal::kHsStuck; } },
        { "F6: UPDATE_INSTANTLY is set",        N48N_MODE_D_LOCK,      [](Env &e) { e.m.reg[kRegDbCtrl] = 0x100u; } },
        { "F6: the lock is already held",       N48N_MODE_D_LOCK,      [](Env &e) { e.m.reg[kRegMasterLock] = 1u; e.m.lockReq = true; } },
        { "F7: the DIO FIFO error is set",      N48N_MODE_D_BASELINE,  [](Env &e) { e.m.reg[kRegPixelRateCntl] |= 0x4000u; } },
        { "F7: the steer FIFO overflow is set", N48N_MODE_D_BASELINE,  [](Env &e) { e.m.reg[kRegDp1Steer] |= 0x10u; } },
        { "P1: an underflow that will not clear", N48N_MODE_D_UNDERFLOW, [](Env &e) { e.m.reg[kRegHubpCntl] |= 0x10000000u; e.m.hubpClearIgnored = true; } },
        { "the display layer is not armed",     N48N_MODE_D_NOT_ARMED, [](Env &e) { e.m.armed = false; } },
        { "the scanout plane is acquired",      N48N_MODE_D_ACQUIRED,  [](Env &e) { e.m.planeAcq = true; } },
        { "a register drifted from the golden", N48N_MODE_D_DRIFT,     [](Env &e) { e.m.reg[kCap[cap_find("HUBPREQ0_NOM_PARAMETERS_1")].abs] ^= 1u; } },
        { "the watchdog thread will not start", N48N_MODE_D_WATCHDOG,  [](Env &e) { e.m.wdStartFail = true; } },
    };
    for (const PC &c : pcs) {
        Env e; expect(prep120(e), "(setup) prerequisites");
        e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0;
        c.set(e); run(e, 120, 300);
        char w[200];
        std::snprintf(w, sizeof(w), "row 120 refused [%s]: DENIED with the right reason", c.what); expect(e.r.verdict == N48N_MODE_V_DENIED && e.r.deny == c.deny, w);
        // (the watchdog case is refused AFTER the raise, so the hold is raised and released again; every other case must not have sent a single clock message)
        if (c.deny != N48N_MODE_D_WATCHDOG) {
            bool onlyUfClear = true; for (auto &wr : e.m.writes) onlyUfClear = onlyUfClear && wr.first == kRegHubpCntl;   // a denied trial may have written the P1b underflow clear strobe and nothing else
            std::snprintf(w, sizeof(w), "row 120 refused [%s]: no clock message, nothing written (bar the P1b clear strobe), no latch", c.what);
            expect(e.m.holdMsgs == 0 && e.m.holdCalls == 0 && (c.deny == N48N_MODE_D_UNDERFLOW ? onlyUfClear : e.m.writes.empty()) && e.r.latched == 0, w); }
        else { std::snprintf(w, sizeof(w), "row 120 refused [%s]: the raise is undone at once (RELEASED, clocks at the start), nothing written", c.what);
               expect(n48dal::hold_state_now(e.hc) == n48dal::kHsReleased && e.m.did[0] == 0x41 && e.m.did[1] == 0x41 && e.m.writes.empty() && e.hOwner == 0, w); }
        std::snprintf(w, sizeof(w), "row 120 refused [%s]: busy released", c.what); expect(e.st.busy == 0, w);
    }
    { Env e; expect(prep120(e), "(setup)"); e.hE1b = false; e.m.trace.clear(); run(e, 120, 100);
      expect(e.r.ext.hold_pre == n48dal::kHpNoE1b, "the refusal names the hold precondition that failed (ext.hold_pre = no E1b)"); }
    { Env e; expect(prep120(e), "(setup)"); e.tm = kProd; e.m.trace.clear(); e.m.writes.clear(); e.m.holdMsgs = 0; run(e, 120, 100);
      expect(e.r.deny == N48N_MODE_D_ROW120_OFF && e.m.holdMsgs == 0 && e.m.writes.size() == 0, "with the production timing row 120 stays DENIED (the boot-arg is off) and nothing is sent"); }
    // ---- the watchdog: a hung trial thread; the watchdog restores REGISTERS ONLY, the trial thread releases the clocks once ----
    for (int deadlineOnly = 0; deadlineOnly < 2; deadlineOnly++) {
        Env e; expect(prep120(e), "(setup) prerequisites"); e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0; e.m.hangAfterWrites = true;
        if (deadlineOnly) e.tm.staleMs = 100000000u;
        std::thread t([&e] { run(e, 120, 1000); });
        bool restored = false;
        for (int i = 0; i < 80000 && !restored; i++) { std::this_thread::sleep_for(std::chrono::microseconds(100)); std::lock_guard<std::mutex> g(gEng); restored = e.st.restoreDone && e.st.wdFired; }
        const bool hung = e.m.hung.load();
        int msgsWhileHung = 0; { std::lock_guard<std::mutex> g(e.m.mu); msgsWhileHung = e.m.holdMsgs; }
        const uint32_t stateWhileHung = n48dal::hold_state_now(e.hc);
        { std::lock_guard<std::mutex> g(e.m.mu); e.m.release = true; } e.m.cv.notify_all();
        t.join(); e.join();
        char w[200]; const char *nm = deadlineOnly ? "deadline" : "stale heartbeat";
        std::snprintf(w, sizeof(w), "row 120 watchdog (%s): it restored while the trial thread was hung", nm); expect(restored && hung, w);
        std::snprintf(w, sizeof(w), "row 120 watchdog (%s): the clocks were still HELD, untouched, while the watchdog restored (2 raise messages only)", nm); expect(stateWhileHung == n48dal::kHsHeld && msgsWhileHung == 2, w);
        std::snprintf(w, sizeof(w), "row 120 watchdog (%s): verdict WATCHDOG, registers exact", nm); expect(e.r.verdict == N48N_MODE_V_WATCHDOG && all_init(e) && e.r.mismatch == 0 && e.r.wd_fired == 1, w);
        std::snprintf(w, sizeof(w), "row 120 watchdog (%s): the TRIAL thread then released the clocks, once, after the verified restore; the watchdog sent no clock message", nm);
        expect(n48dal::hold_state_now(e.hc) == n48dal::kHsReleased && e.m.holdMsgs == 4 && e.m.holdCalls == 2 && e.m.holdFromWd == 0 && (e.r.flags & N48N_MODE_F_HOLD_RELEASED), w);
    }
    // ---- an ABORT during the dwell (dcnmode 0 / the kext stop): restored, then released ----
    { Env e; expect(prep120(e), "(setup)"); e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0;
      const uint64_t v0 = e.m.vnow;
      std::thread t([&e] { run(e, 120, 20000); });
      for (int i = 0; i < 40000; i++) { { std::lock_guard<std::mutex> g(e.m.mu); if (e.m.vnow > v0 + 2000000ull + 500000ull + 3000000ull + 1500000ull) break; } std::this_thread::sleep_for(std::chrono::microseconds(100)); }
      request_abort(e.hw, e.st); t.join(); e.join();
      expect(e.r.verdict == N48N_MODE_V_ABORT && all_init(e) && n48dal::hold_state_now(e.hc) == n48dal::kHsReleased && e.m.holdFromWd == 0 && e.m.did[0] == 0x41 && (e.r.flags & N48N_MODE_F_HOLD_RELEASED), "ABORT during the dwell: restored, the clocks released after it, verdict ABORT"); }
    // ---- dcnmode 0 / the kext stop: a hold a dead trial left HELD is released only after a verified restore ----
    { Env e; expect(prep120(e), "(setup)"); e.m.trace.clear(); e.m.holdMsgs = 0;
      n48dal::HoldRep rep{}; n48dal::Decoded a{}; tIsTrial = true;
      expect_u("(setup) a raise that nobody releases (the trial thread died)", n48dal::hold_raise(hio(&e.m), e.hc, rep, 4, true, false, 514285, 500000, &a), n48dal::kHrOk);
      Recover rc{}; emergency_recover(e.hw, e.st, &rc);
      const uint32_t rel = emergency_release(e.hw, e.st, e.tm, true, rc.bad, rc.healthyAfter, 3000u);
      tIsTrial = false; e.join();
      expect(rel == n48dal::kRlOk && n48dal::hold_state_now(e.hc) == n48dal::kHsReleased && e.m.did[0] == 0x41 && e.hOwner == 0, "dcnmode 0: after the golden restore, the resync check and 3 s of 60 Hz, the stray hold is RELEASED");
      // not idle (a trial still runs): nothing happens
      Env f; expect(prep120(f), "(setup)"); n48dal::HoldRep r2{}; n48dal::Decoded a2{}; tIsTrial = true; (void)n48dal::hold_raise(hio(&f.m), f.hc, r2, 4, true, false, 514285, 500000, &a2); const int n0 = f.m.holdMsgs;
      expect(emergency_release(f.hw, f.st, f.tm, false, 0u, true, 0u) == n48dal::kRlNotHeld && f.m.holdMsgs == n0 && n48dal::hold_state_now(f.hc) == n48dal::kHsHeld, "a running trial owns its hold: emergency_release leaves it alone");
      // a restore that did not verify: STUCK, no message
      expect(emergency_release(f.hw, f.st, f.tm, true, 3u, true, 0u) == n48dal::kRlBadRestore && f.m.holdMsgs == n0 && n48dal::hold_state_now(f.hc) == n48dal::kHsStuck, "registers still differ: NEVER released (STUCK, no message)");
      Env g; expect(prep120(g), "(setup)"); tIsTrial = true; n48dal::HoldRep r3{}; n48dal::Decoded a3{}; (void)n48dal::hold_raise(hio(&g.m), g.hc, r3, 4, true, false, 514285, 500000, &a3); const int n1 = g.m.holdMsgs;
      expect(emergency_release(g.hw, g.st, g.tm, true, 0u, false, 0u) == n48dal::kRlBadRestore && g.m.holdMsgs == n1, "an unhealthy back end: NEVER released");
      Env h; expect(prep120(h), "(setup)"); tIsTrial = true; n48dal::HoldRep r4{}; n48dal::Decoded a4{}; (void)n48dal::hold_raise(hio(&h.m), h.hc, r4, 4, true, false, 514285, 500000, &a4); h.m.reg[0x141] = 201000000u; const int n2 = h.m.holdMsgs;   // the 60 Hz rate does not read back
      expect(emergency_release(h.hw, h.st, h.tm, true, 0u, true, 3000u) == n48dal::kRlBadRestore && h.m.holdMsgs == n2, "dcnmode 0: the 60 Hz rate did not read back: NEVER released");
      tIsTrial = false; f.join(); g.join(); h.join();
      Env k; expect_u("nothing to release when no hold exists", emergency_release(k.hw, k.st, k.tm, true, 0u, true, 0u), n48dal::kRlNotHeld); }
    // ---- restore_from_golden after a row-120 trial writes the DTO group FIRST ----
    { Env e; expect(prep120(e), "(setup)"); run(e, 120, 100); expect_u("(setup) clean", e.r.verdict, N48N_MODE_V_PASS);
      e.st.golden[kIdxDtoPhase] ^= 0u; e.m.writes.clear(); e.m.reg[0x141] = tab::kGold120[kIdxDtoPhase]; e.m.reg[kCap[cap_find("HUBPREQ0_VBLANK_PARAMETERS_0")].abs] ^= 0x5u; e.m.reg[kCap[kIdxHTotal].abs] ^= 0x1u; e.m.reg[0x5747] ^= 0x3u;
      uint32_t restored = 0; bool have = false; const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have);
      expect(have && bad == 0 && restored >= 4, "dcnmode 0 after row 120: every drifted register is put back");
      long pd = -1, po = -1, pm = -1; for (size_t i = 0; i < e.m.writes.size(); i++) { const uint32_t a = e.m.writes[i].first; if (a == 0x141 && pd < 0) pd = (long)i; if (in_cap(a) && !is_dto(a) && !is_msa(a) && po < 0) po = (long)i; if (a == 0x5747 && pm < 0) pm = (long)i; }
      expect(pd >= 0 && po > pd && pm > po, "the emergency restore after a row-120 trial writes DTO, then the OTG/HUBP registers, then the MSA"); }
    // ---- static properties of the engine source: DAL calls never under the lock, the watchdog never touches a clock ----
    {
        const std::string pure = slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_modetrial_pure.h");
        // every clock call of the engine is in run_trial or emergency_release, and at each of them the last lock statement before it is an UNLOCK (or there is none)
        size_t nCalls = 0; bool allFree = true;
        for (const char *tok : { "hw.hold_raise(", "hw.hold_release(", "hw.hold_pre(", "hw.hold_state(" }) {
            size_t pos = 0;
            while ((pos = pure.find(tok, pos)) != std::string::npos) {
                nCalls++;
                const size_t lk = pure.rfind("hw.lock(hw.ctx)", pos), ul = pure.rfind("hw.unlock(hw.ctx)", pos);
                if (lk != std::string::npos && (ul == std::string::npos || ul < lk)) allFree = false;
                pos += std::strlen(tok);
            }
        }
        expect(nCalls >= 6 && allFree, "no clock call of the engine (hold_raise / hold_release / hold_pre / hold_state) sits after a lock statement without an unlock in between (no DAL call under the engine lock)");
        for (const char *fn : { "inline void restore_locked(", "inline bool restore_run(", "inline void watchdog_body(", "inline Apply apply_120_locked(", "inline Apply apply_locked(", "inline uint32_t restore_from_golden(", "inline void restore_group_locked(",
                                "inline void write_group_locked(", "inline uint32_t wrap_blank_locked(", "inline uint32_t wrap_unblank_locked(", "inline void emergency_recover(" }) {
            const std::string b = fn_body(pure, fn);
            expect(!b.empty() && b.find("hw.hold_") == std::string::npos && b.find("hold_raise") == std::string::npos && b.find("hold_release") == std::string::npos && b.find("n48dal::hold") == std::string::npos && b.find("emergency_release") == std::string::npos, (std::string("no clock code in ") + fn).c_str());
        }
        expect(fn_body(pure, "inline uint32_t emergency_release(").find("hw.hold_release(hw.ctx, bad, &st.rep.hold)") != std::string::npos, "emergency_release releases with restoreBad = its own verification result");
        const std::string rt = fn_body(pure, "inline void run_trial(const Hw &hw, State &st, const Timing &tm, uint32_t row, uint32_t dwellMs, uint32_t tflags, n48n_mode_result *r, const HoldCfg *hc = nullptr) {");
        expect(rt.find("o.holdStuck = hw.hold_release(hw.ctx, o.restoreBad, &st.rep.hold) != n48dal::kRlOk;") != std::string::npos && rt.find("o.holdStuck = hw.hold_release(") > rt.find("o.restoreBad = !restoreDone"), "the release is issued with restoreBad, after it is computed");
        expect(count_of(rt, "hw.hold_release(") == 2 && count_of(rt, "hw.hold_raise(") == 1, "run_trial: one raise, two release sites (the release proper and the undo of a denied trial)");
    }
}

static void s_pins(const std::string &root) {
    const std::string src = root + "/src/navi48-bringup/src/";
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), dcnh = slurp(src + "dcn/navi48_dcn.hpp"), eng = slurp(src + "amd/native_s1c.cpp"), engh = slurp(src + "amd/native_s1c.h"),
                      cli = slurp(src + "Navi48NativeClient.cpp"), boot = slurp(src + "Navi48Bringup.cpp"), pure = slurp(src + "dcn/navi48_modetrial_pure.h"), tabh = slurp(src + "dcn/navi48_modetrial_tables.h"),
                      abi = slurp(src + "Navi48NativeABI.h"), plist = slurp(root + "/src/navi48-bringup/Info.plist"), tool = slurp(root + "/tools/native/n48mode.c"), bs = slurp(root + "/tools/native/build.sh"),
                      gen = slurp(root + "/tools/native/gen_modetrial_tables.py"), gi = slurp(root + "/.gitignore");
    expect(!dcn.empty() && !dcnh.empty() && !eng.empty() && !engh.empty() && !cli.empty() && !boot.empty() && !pure.empty() && !tabh.empty() && !abi.empty() && !plist.empty() && !tool.empty() && !gen.empty(), "the sources are readable");
    // ABI 1.3
    expect_u("N48N_SEL_MODE_TRIAL is 16", N48N_SEL_MODE_TRIAL, 16); expect_u("selector count 1.3", N48N_SEL_COUNT_1_3, 17); expect_u("selectors 17 / 18 (1.7)", N48N_SEL_MODE_HOLD * 100 + N48N_SEL_MODE_RELEASE, 1718); expect_u("selector count 1.7", N48N_SEL_COUNT_1_7, 19); expect_u("selector count 1.2 unchanged", N48N_SEL_COUNT_1_2, 16);
    expect_u("selector count 1.1 unchanged", N48N_SEL_COUNT_1_1, 15); expect_u("selector count 1.0 unchanged", N48N_SEL_COUNT, 9); expect_u("ABI major unchanged", N48N_ABI_VERSION, 1); expect_u("ABI minor 12 (1.12 since 0.0.662; 1.11 since 0.0.661; 1.10 since 0.0.640: the VRAM budget words; 1.9 since 0.0.612: BoImportHost; 1.8 since 0.0.610: the Metal nub selectors 19 / 20; 1.7 since 0.0.609: the HELD mode, selectors 17 / 18; 1.6 was 0.0.608: the row-120 transition reads live in the result's former reserved[8]; 1.5 was 0.0.607; the struct sizes are unchanged)", N48N_ABI_MINOR, 12);
    expect_u("mode result size (512 B since ABI 1.4; the first 256 B are the 1.3 layout)", sizeof(n48n_mode_result), 512); expect_u("the extension starts at 256", __builtin_offsetof(n48n_mode_result, ext), 256); expect_u("the extension is 256 B", sizeof(n48n_mode_ext), 256); expect_u("result offsets", __builtin_offsetof(n48n_mode_result, rate_before_mhz) * 1000 + __builtin_offsetof(n48n_mode_result, fifo_before), 48080);
    expect_u("result offsets 2", __builtin_offsetof(n48n_mode_result, mismatch) * 1000 + __builtin_offsetof(n48n_mode_result, dto_phase), 128160);
    expect_u("the ABI: health flag bit 12, the 208 counters and the 1.6 words at 224", N48N_MODE_F_HEALTH_BAD * 1000ull + __builtin_offsetof(n48n_mode_result, dio_write_errs) + __builtin_offsetof(n48n_mode_result, uf_optc_rs_dto), 4096000 + 208 + 224);
    expect_u("rows", N48N_MODE_ROW_50 * 1000 + N48N_MODE_ROW_120, 50120); expect_u("max dwell", N48N_MODE_MAX_DWELL_MS, 30000); expect_u("the engine's dwell bound is the ABI's", kMaxDwellMs, N48N_MODE_MAX_DWELL_MS);
    expect(abi.find("ABI 1.3 addendum") != std::string::npos && abi.find("N48N_SEL_MODE_TRIAL") != std::string::npos && abi.find("never raises a clock") != std::string::npos, "the ABI header carries the 1.3 contract text");
    // the exemption table is untouched: exactly (74,0), (76,0), (77,0)
    for (uint32_t act : { 74u, 76u, 77u }) expect(n48scan::accel_exempt(act, 0), "exempt: the three (action, 0) pairs");
    for (uint32_t act : { 74u, 76u, 77u }) expect(!n48scan::accel_exempt(act, 1) && !n48scan::accel_exempt(act, 1002) && !n48scan::accel_exempt(act, 101), "not exempt: any nonzero argument");
    expect(!n48scan::accel_exempt(75, 0) && !n48scan::accel_exempt(73, 0) && !n48scan::accel_exempt(78, 0), "not exempt: other actions");
    expect(boot.find("n48scan::accel_exempt(action, argScalar)") != std::string::npos, "the exemption call site is unchanged");
    // the engine header is pure
    expect(pure.find("#include <IOKit") == std::string::npos && pure.find("#include <kern") == std::string::npos && pure.find("IOSleep") == std::string::npos && pure.find("IOLock") == std::string::npos && pure.find("RREG32") == std::string::npos && pure.find("WREG32") == std::string::npos,
           "the engine header names no kernel header and does no register access of its own (Hw.rd / Hw.wr only)");
    expect(tabh.find("GENERATED by tools/native/gen_modetrial_tables.py") != std::string::npos && gen.find("--check") != std::string::npos, "the tables are generated and the generator can check them");
    // the kext glue
    const std::string mt0 = dcn.substr(dcn.find("// ---- build 0.0.605 (native S2d): the timed mode trial - the kext half")), mt = mt0.substr(0, mt0.find("// build 0.0.661 (M6 Stage 1b, ABI 1.11"));   // 0.0.661: the instance-2 scanout block that ends the file is its own (tests/native_scanx_test.cpp pins it)
    expect(dcn.find("// ---- build 0.0.605 (native S2d): the timed mode trial - the kext half") != std::string::npos, "the glue block exists");
    expect_u("exactly ONE WREG32 in the whole mode-trial glue", count_of(mt.substr(0, mt.find("uint32_t modeTrial(")), "WREG32("), 1);
    const std::string wr = fn_body(mt, "static bool mt_wr(void *, uint32_t abs, uint32_t v) {");
    expect(!wr.empty() && wr.find("if (!dcn41_allow_write(&gDcn.allow, abs, v, gDcn.tag)) {") != std::string::npos && wr.find("dcn41_allow_write(") < wr.find("WREG32(") && wr.rfind("return false;") < wr.find("WREG32("),
           "the only write is behind dcn41_allow_write, which returns before the WREG32 on a refusal");
    expect(wr.find("gDcn.armed") != std::string::npos, "and needs the display layer armed");
    expect(mt.find("gCliLock") == std::string::npos, "the glue never names the client lock (its lock is a leaf)");
    expect(mt.find("gMtLock") != std::string::npos && count_of(mt, "IOLockLock(") == 2 && count_of(mt, "IOLockUnlock(") == 2, "two lock statements in the glue: the leaf lock's wrapper and mt_plane_acquired's scanout-lock read (each with its unlock)");
    const std::string pa = fn_body(mt, "static bool mt_plane_acquired(void *) {");
    expect(pa.find("IOLockLock(l);") != std::string::npos && pa.find("IOLockUnlock(l);") != std::string::npos && pa.find("IOSleep") == std::string::npos, "the plane check takes the scanout lock briefly and does not sleep under it");
    expect(fn_body(mt, "static bool mt_gate_ok(void *) {").find("s.gate == n48native::kGateOn && s.positivePass") != std::string::npos, "the trial gate is native + S1b POSITIVE PASS");
    expect(fn_body(mt, "static bool mt_otg_ok(void *) {").find("mask != 1u") != std::string::npos && fn_body(mt, "static bool mt_otg_ok(void *) {").find("g.w == 2560u && g.h == 1440u && g.pitchPx == 2560u") != std::string::npos, "OTG0 must be the only lit OTG, plane 2560x1440");
    expect(fn_body(mt, "static bool mt_clocks(void *, n48dal::Decoded *d) {").find("kRegDfs0") != std::string::npos && mt.find("SetHardMin") == std::string::npos && mt.find("smu_dal_send") == std::string::npos, "the clocks are only READ (DFS / DENTIST); no DAL message can be sent from here");
    expect(mt.find("kernel_thread_start(&mt_watchdog") != std::string::npos && mt.find("thread_deallocate(th)") != std::string::npos && mt.find("gMtWdAlive") != std::string::npos, "the watchdog is a kernel thread that is counted");
    expect(fn_body(mt, "static void mt_shutdown() {").find("request_abort") != std::string::npos && fn_body(mt, "static void mt_shutdown() {").find("restore_from_golden") != std::string::npos && fn_body(mt, "static void mt_shutdown() {").find("gMtWdAlive") != std::string::npos,
           "kext stop: abort, put the golden set back, wait for the watchdog threads");
    const std::string sd = fn_body(dcn, "void scanShutdown() {");
    expect(!sd.empty() && sd.find("mt_shutdown();") != std::string::npos && sd.find("mt_shutdown();") < sd.find("if (gScan.lock == nullptr) return;"), "scanShutdown ends the trial BEFORE its scan-lock early return");
    // golden at bind, before any native call can write
    const std::string bl = fn_body(dcn, "static uint32_t bind_locked(Navi48Bringup *owner) {");
    expect(!bl.empty() && bl.find("mt_golden_at_bind();") != std::string::npos && bl.find("mt_golden_at_bind();") > bl.find("GOLDEN timing capture taken at bind") && bl.find("mt_golden_at_bind();") > bl.find("gDcn.armed = true;"), "the full golden copy is taken in bind_locked, after arming and after the timing golden");
    expect(fn_body(mt, "static void mt_golden_at_bind() {").find("kGateOn") != std::string::npos && fn_body(mt, "static void mt_golden_at_bind() {").find("golden_take(kMtHw, gMt, true)") != std::string::npos, "at bind on native boots only, through golden_take");
    // dcnmode 0
    const std::string md = fn_body(dcn, "uint32_t mode(uint64_t arg, uint64_t *out, unsigned outCount) {");
    const size_t z = md.find("if (arg == 0u) {");
    expect(!md.empty() && z != std::string::npos && md.find("mt_emergency_restore(\"dcnmode 0\")") > z && md.find("mt_emergency_restore(\"dcnmode 0\")") < md.find("the live timing differs from the golden capture"), "dcnmode 0 restores the FULL golden set inside its arg == 0 branch, before the timing drift check");
    expect_u("mt_emergency_restore is called from exactly one place in mode()", count_of(md, "mt_emergency_restore("), 1);
    const std::string er = fn_body(mt, "static bool mt_emergency_restore(const char *why) {");
    expect(er.find("if (!gMt.everWrote) {") != std::string::npos && er.find("emergency_recover(") != std::string::npos && er.find("wait_idle") < er.find("gMt.everWrote") && er.find("gMt.everWrote") < er.find("emergency_recover("), "the emergency restore needs everWrote, tested after the running trial ended and before any restore (0.0.607: without it only a still-HELD clock hold is looked at)");
    expect(fn_body(mt, "static bool mt_in_use(void *) {").find("amdgpu::dal_busy()") != std::string::npos, "a running DAL step makes the trial DENIED (in use)");
    expect(dcn.find("bool modeTrialBusy() { return __atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u; }") != std::string::npos && dcnh.find("bool modeTrialBusy();") != std::string::npos, "modeTrialBusy() reads the trial's busy flag");
    { const std::string dal = slurp(src + "amd/smu_dal.cpp"), dalh = slurp(src + "amd/smu_dal.h");
      expect(dal.find("const bool busy = !gotBusy || n48dcn::modeTrialBusy();") != std::string::npos && dal.find("if (gotBusy) owner_cas(kOwnerDal, kOwnerIdle);") != std::string::npos && dal.find("bool dal_busy() {") != std::string::npos && dalh.find("bool dal_busy();") != std::string::npos, "the DAL step refuses while a trial runs, releases only its own flag, and exposes dal_busy()"); }
    expect(er.find("kGateOn") != std::string::npos && er.find("emergency_recover(") != std::string::npos && er.find("request_abort") < er.find("emergency_recover(") && er.find("wait_idle") < er.find("emergency_recover(") && er.find("restore_from_golden") == std::string::npos, "the emergency restore acts on native boots only, ends a running trial first, then restores (the golden restore and the resync are inside emergency_recover)");
    // scanAcquire refuses while a trial runs
    const std::string sa = fn_body(dcn, "uint32_t scanAcquire(uint64_t out[2], uint32_t sess) {");
    expect(!sa.empty() && sa.find("__atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u") != std::string::npos && sa.find("__atomic_load_n(&gMt.busy") > sa.find("if (gScan.acquired || gScan.restoring)") && sa.find("__atomic_load_n(&gMt.busy") < sa.find("scan_read_geom(&g)"),
           "scanAcquire refuses (Busy) while a trial runs, under the scanout lock, before it reads the plane");
    // selector wiring
    expect(cli.find("case N48N_SEL_MODE_TRIAL:") != std::string::npos && cli.find("shape(3, 0, 0, sizeof(n48n_mode_result))") != std::string::npos && cli.find("amdgpu::n1c_mode_trial(r, si[0], si[1], si[2],") != std::string::npos, "the client dispatches selector 16 with its exact shape");
    const std::string ms = fn_body(eng, "IOReturn n1c_mode_trial(");
    expect(ms.find("if (!sess_hello(ref)) return kIOReturnNotReady;") != std::string::npos && ms.find("(flags & ~(uint64_t)N48N_MODE_TF_MASK) != 0ull") != std::string::npos && ms.find("n48dcn::modeTrial(row, dwellMs, flags, out)") != std::string::npos && ms.find("IOLockLock") == std::string::npos, "n1c_mode_trial: Hello first, only the TF flags are accepted, no client lock held while the trial runs");
    expect(dcnh.find("uint32_t modeTrial(uint64_t row, uint64_t dwellMs, uint64_t flags, struct n48n_mode_result *o);") != std::string::npos && engh.find("IOReturn n1c_mode_trial(") != std::string::npos, "declared");
    expect(fn_body(dcn, "uint32_t modeTrial(uint64_t row, uint64_t dwellMs, uint64_t flags, struct n48n_mode_result *o) {").find("n48mt::run_trial(kMtHw, gMt, mt_timing(),") != std::string::npos, "the selector runs the engine on the real Hw with the boot-arg timing table (production timing, row 120 only with navi48-row120=1)");
    // 0.0.606: the delay callback, the flags, the rows, the ABI text
    { const std::string dl = fn_body(mt, "static void mt_delay_us(void *, uint32_t us) {");
      expect(dl.find("IODelay(us);") != std::string::npos && count_of(mt, "IODelay(") == 1 && mt.find("kMtHw = {") != std::string::npos && mt.find("mt_start_watchdog, mt_log, mt_delay_us, mt_hold_pre, mt_hold_raise, mt_hold_release, mt_hold_state, mt_scan_release };") != std::string::npos, "the sequences' delay is IODelay in exactly one glue function, wired into the Hw table");
      expect(fn_body(dcn, "uint32_t modeTrial(uint64_t row, uint64_t dwellMs, uint64_t flags, struct n48n_mode_result *o) {").find("flags > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)flags, o);") != std::string::npos, "the selector hands the flags word to the engine");
      expect(abi.find("ABI 1.4 addendum") != std::string::npos && abi.find("N48N_MODE_ROW_RESYNC") != std::string::npos && abi.find("N48N_MODE_ROW_VTOTAL") != std::string::npos && abi.find("N48N_MODE_TF_RESYNC") != std::string::npos && abi.find("Row 120 stays hard-denied") != std::string::npos && abi.find("ABI 1.5 addendum") != std::string::npos && abi.find("navi48-row120=1") != std::string::npos, "the header carries the 1.4 and 1.5 contract texts and the new rows");
      expect_u("rows 1 / 2", N48N_MODE_ROW_RESYNC * 10 + N48N_MODE_ROW_VTOTAL, 12); expect_u("the flag mask is the resync flag", N48N_MODE_TF_MASK, N48N_MODE_TF_RESYNC);
      expect_u("verdicts 11 12 13 14", N48N_MODE_V_UNDERFLOW * 1000000ull + N48N_MODE_V_CLOCK_LOST * 10000 + N48N_MODE_V_RESYNC * 100 + N48N_MODE_V_LOCK, 11121314ull);
      expect_u("the deny codes 20 / 21", N48N_MODE_D_UNDERFLOW * 100 + N48N_MODE_D_LOCK, 2021);
      expect(!kProd.row120 && n48mt::kProd.wrapExtraMs == 4000u, "row 120 is denied in the production constant (the boot-arg turns it on in mt_timing()); the wrapper's watchdog slack is 4 s");
    }
    // the tool
    expect(tool.find("N48N_SEL_MODE_TRIAL") != std::string::npos && tool.find("--resync") != std::string::npos && tool.find("\"resync\"") != std::string::npos && tool.find("\"vtotal\"") != std::string::npos && tool.find("N48N_MODE_ROW_VTOTAL") != std::string::npos && tool.find("--dwell") != std::string::npos && tool.find("N48N_MODE_V_PASS") != std::string::npos && tool.find("; WATCH THE MONITOR)") != std::string::npos, "n48mode names the selector, --dwell, the verdicts and the watch warning");
    expect(tool.find("case N48N_MODE_D_STEP_DOWN") != std::string::npos && tool.find("case N48N_MODE_D_CLOCKS") != std::string::npos && tool.find("dcnmode 0") != std::string::npos, "the tool explains the deny reasons and the emergency verb");
    expect(tool.find("not run") != std::string::npos && tool.find("x->lk_hold_max == 0u") != std::string::npos && tool.find("FIRST COUNT PAST THE OLD MAXIMUM") != std::string::npos && tool.find("expected raster") == std::string::npos && tool.find("r.lk_hold_ran") != std::string::npos && tool.find("r.uf_optc_rs_latch") != std::string::npos,
           "n48mode (0.0.608): prints 'not run' for a row-120 lock-hold witness that never ran, 'first count past the old maximum' in the latch line (no 'expected raster'), and the transition reads");
    expect(bs.find("n48mode:n48mode.c") != std::string::npos && gi.find("tools/native/n48mode") != std::string::npos, "build.sh builds n48mode and .gitignore ignores the binary");
    // version
    {
        const size_t p = engh.find("kN1cKextBuild = "); const int build = p == std::string::npos ? -1 : std::atoi(engh.c_str() + p + 16);
        const size_t v = plist.find("<string>0.0."); const int ver = v == std::string::npos ? -2 : std::atoi(plist.c_str() + v + 12);
        expect(build > 0 && build == ver, "kN1cKextBuild equals the Info.plist patch version"); expect_u("Info.plist is 0.0.664", (uint64_t)ver, 664); expect_u("the plist carries the version twice", count_of(plist, "0.0.664"), 2);
    }
    // no log line of the glue can be cut by the 512-byte logger: literal length + 12 bytes per conversion at the widest
    {
        size_t widest = 0, lines = 0, pos = 0;
        while ((pos = mt.find("N48LOG(\"", pos)) != std::string::npos) {
            const size_t q = pos + 8, e = mt.find('"', q);
            std::string lit; for (size_t k = q; k < mt.size() && mt[k] != '"'; k++) { if (mt[k] == '\\') k++; lit += mt[k]; }
            size_t conv = 0; for (size_t k = 0; k + 1 < lit.size(); k++) if (lit[k] == '%' && lit[k + 1] != '%') conv++;
            const size_t w = lit.size() + conv * 12 + 20; if (w > widest) widest = w; lines++; pos = e;
        }
        // the log lines are in the glue AND in mt_log's switch (which sits inside the block too)
        std::printf("native_s2d_test: widest mode-trial log line at the widest fields: %zu bytes (limit 511), %zu lines\n", widest, lines);
        expect(lines >= 8 && widest <= 511, "every mode-trial log line fits the 512-byte logger");
    }
    // banned strings
    { const std::string b1 = std::string("+0x") + "280", b2 = std::string("+0x") + "282", b3 = std::string("+0x") + "299";   // spelled in two pieces so this file itself carries none of them
      expect(dcn.find(b1) == std::string::npos && dcn.find(b2) == std::string::npos && dcn.find(b3) == std::string::npos && pure.find(b1) == std::string::npos && pure.find(b2) == std::string::npos && pure.find(b3) == std::string::npos, "banned strings absent"); }
}

// ---- T13 (0.0.608): row 120's blanked-transition underflow: recorded and CLEARED at both ends, judged only on a visible picture -------------------------------------
// Design: an internal design note root cause of the first 120 Hz run's switch-back flag). OPTC bit 10 is raised at ONE exact point of a row-120 trial on the model.
static void t13_transitions() {
    const std::string optcW = "W:" + hx(kRegOptcInGlobal) + "=";
    auto fresh = [](Env &e, void (*set)(Env &)) { expect(prep120(e), "(setup) prerequisites"); e.m.dtoEverChanged = false; e.m.optcFired = 0; e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0; set(e); };
    // 1. the apply's transition (raised at the MSA, still blanked): recorded, cleared before the unblank, the trial PASSES with nothing judged
    { Env e; fresh(e, [](Env &e) { e.m.optcAtApMsa = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect_u("t13 apply transition: fired once", e.m.optcFired, 1);
      expect_u("t13 apply transition: PASS (the blanked transition is reported, not judged)", r.verdict, N48N_MODE_V_PASS);
      expect((r.uf_optc_ap_trans & 0x400u) != 0u, "t13 apply transition: uf_optc_ap_trans holds bit 10");
      expect((r.ext.uf_optc_settle & 0x400u) == 0u && (r.ext.uf_optc_after & uf::kOptcSeen) == 0u && !(r.flags & N48N_MODE_F_UNDERFLOW_SEEN), "t13 apply transition: the settle read after the unblank and the after-restore read are clean, UNDERFLOW_SEEN not set");
      expect(r.ext.uf_clears >= 1 && (r.flags & N48N_MODE_F_UF_CLEARED), "t13 apply transition: a clear strobe was written");
      expect(r.uf_optc_rs_dto == 0u && r.uf_optc_rs_latch == 0u, "t13 apply transition: the restore's reads are clean");
      // order (behaviour): the OPTC clear is written after the apply's MSA and before the apply's unblank
      const auto &t = e.m.trace; const long msa = tposp(t, "W:5746="), clr = tposp(t, optcW, (size_t)msa), unb = tposp(t, "W:5706=", (size_t)msa);
      expect(msa >= 0 && clr > msa && unb > clr, "t13 apply transition: the OPTC clear comes after the MSA and BEFORE the unblank");
    }
    // 2. the restore's latch transition (raised at the restore's V_TOTAL latch witness, still blanked): recorded, cleared before the unblank
    { Env e; fresh(e, [](Env &e) { e.m.optcAtRsLatch = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect_u("t13 restore latch: fired once", e.m.optcFired, 1);
      expect_u("t13 restore latch: PASS", r.verdict, N48N_MODE_V_PASS);
      expect((r.uf_optc_rs_latch & 0x400u) != 0u && (r.uf_optc_rs_dto & uf::kOptcSeen) == 0u && r.uf_optc_ap_trans == 0u, "t13 restore latch: uf_optc_rs_latch holds bit 10, the DTO-step read and the apply's read are clean");
      expect((r.ext.uf_optc_after & uf::kOptcSeen) == 0u && !(r.flags & N48N_MODE_F_UNDERFLOW_SEEN) && (r.flags & N48N_MODE_F_RESTORED) && !(r.flags & N48N_MODE_F_RESTORE_FAILED), "t13 restore latch: the after-restore read is clean, the restore counts as verified");
      const auto &t = e.m.trace; const long rDto = tpos(t, "W:141=" + hx(e.m.init[0x141]), (size_t)tposp(t, "W:5746=")), rLock = tpos(t, "W:5049=1", (size_t)rDto), rUnlk = tpos(t, "W:5049=0", (size_t)rLock + 1);
      const long clr = tposp(t, optcW, (size_t)rUnlk), rMsa = tposp(t, "W:5746=", (size_t)rUnlk), unb = tposp(t, "W:5706=", (size_t)rUnlk);
      expect(rDto > 0 && rUnlk > rLock && clr > rUnlk && rMsa > clr && unb > clr, "t13 restore latch: the OPTC clear is written after the restore's unlock / latch witness and BEFORE the restore's unblank");
    }
    // 3. the DTO step's read is recorded and NOT cleared: the bit is still there at the latch read
    { Env e; fresh(e, [](Env &e) { e.m.ufOnRestore = true; e.m.ufHubp = 0u; e.m.ufOptc = uf::kOptcOccurred; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect_u("t13 restore DTO step: PASS (cleared at the latch, still blanked)", r.verdict, N48N_MODE_V_PASS);
      expect((r.uf_optc_rs_dto & 0x400u) != 0u && (r.uf_optc_rs_latch & 0x400u) != 0u, "t13 restore DTO step: recorded after the DTO step, NOT cleared there (the latch read still sees it)");
      expect((r.ext.uf_optc_after & uf::kOptcSeen) == 0u, "t13 restore DTO step: clean after the restore");
      const auto &t = e.m.trace; const long rDto = tpos(t, "W:141=" + hx(e.m.init[0x141]), (size_t)tposp(t, "W:5746=")), rLock = tpos(t, "W:5049=1", (size_t)rDto);
      expect(rDto > 0 && rLock > rDto && tposp(t, optcW, (size_t)rDto) > rLock, "t13 restore DTO step: no OPTC write between the DTO restore and the lock");
    }
    // 4. the verify window (after the restore's unblank): JUDGED -> RESTORE
    { Env e; fresh(e, [](Env &e) { e.m.optcAtVerify = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect_u("t13 verify window: RESTORE", r.verdict, N48N_MODE_V_RESTORE);
      expect((r.ext.uf_optc_after & 0x400u) != 0u && (r.flags & N48N_MODE_F_RESTORE_FAILED) && r.latched == 1, "t13 verify window: the after-restore read holds bit 10, RESTORE_FAILED, latched");
    }
    // 5. after the apply's unblank on row 120 (a visible picture): the settle read is JUDGED -> UNDERFLOW
    { Env e; fresh(e, [](Env &e) { e.m.optcAtApUnblank = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect_u("t13 after the unblank: UNDERFLOW", r.verdict, N48N_MODE_V_UNDERFLOW);
      expect((r.ext.uf_optc_settle & 0x400u) != 0u && (r.flags & N48N_MODE_F_UNDERFLOW_SEEN), "t13 after the unblank: the settle read holds bit 10, UNDERFLOW_SEEN");
    }
    // 6. a refused clear (INPUT_SOFT_RESET set) at either transition must not PASS
    { Env e; fresh(e, [](Env &e) { e.m.optcAtRsLatch = true; e.m.optcSoftReset = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect(r.verdict != N48N_MODE_V_PASS && (r.flags & N48N_MODE_F_UF_CLEAR_REFUSED) && r.ext.uf_clear_refused >= 1, "t13 refused clear at the restore latch (soft reset set): the clear is refused and the trial does NOT pass");
      expect_u("t13 refused clear at the restore latch: RESTORE (the after-restore read still holds the bit)", r.verdict, N48N_MODE_V_RESTORE); }
    { Env e; fresh(e, [](Env &e) { e.m.optcAtApMsa = true; e.m.optcSoftReset = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect(r.verdict != N48N_MODE_V_PASS && (r.flags & N48N_MODE_F_UF_CLEAR_REFUSED), "t13 refused clear at the apply's transition: does NOT pass");
      expect((r.flags & N48N_MODE_F_UNDERFLOW_SEEN) && (r.ext.uf_optc_settle & 0x400u) != 0u, "t13 refused clear at the apply's transition: the judged settle read saw the bit (the bit persists, so the restore verdict outranks UNDERFLOW)"); }
    // 7. row 2 / row 50 are unchanged: their settle read stays REPORTED (a blip from the write transition is not a verdict), and none of the 120 fields is written
    { Env e; e.bind(); e.m.ufOnDto = true; e.m.ufHubp = 0u; e.m.ufOptc = uf::kOptcOccurred; run(e, 50, 300); const n48n_mode_result &r = e.r;
      expect_u("t13 row 50: a blip at the DTO write is still only reported (PASS)", r.verdict, N48N_MODE_V_PASS);
      expect(r.uf_optc_rs_dto == 0u && r.uf_optc_rs_latch == 0u && r.uf_optc_ap_trans == 0u && r.dio_hold_errs == 0u && r.dio_post_unblank_errs == 0u && r.lk_hold_ran == 0u, "t13 row 50: none of the ABI 1.6 row-120 words is written"); }
    // 8. the lock-hold witness runs in row 120's apply (blanked, old clock, before the unlock) and its result is stored, never judged
    { Env e; fresh(e, [](Env &) {}); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect_u("t13 lock-hold witness: PASS", r.verdict, N48N_MODE_V_PASS);
      expect(r.lk_hold_ran == 1u && r.lk_hold_rc == N48N_MODE_SEQ_OK && r.ext.lk_hold_max > 100u && r.ext.lk_hold_max <= 1480u, "t13 lock-hold witness: ran, code OK, max VERT_COUNT within the old 1480 maximum");
    }
    { Env e; fresh(e, [](Env &e) { e.m.lockLeaks = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect(r.lk_hold_ran == 1u && r.lk_hold_rc == N48N_MODE_SEQ_HOLD && r.ext.lk_hold_max > 1480u, "t13 lock-hold witness (a leaking lock): the witness ran and FAILED");
      expect_u("t13 lock-hold witness failure is stored, not judged: no LOCK verdict from it", r.verdict == N48N_MODE_V_LOCK ? 1u : 0u, 0u); }
    // 9. the DIO attribution: one error at the clock raise, one more across the apply, reported and never judged
    { Env e; fresh(e, [](Env &) {}); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect(r.dio_hold_errs == 0u && r.dio_post_unblank_errs == 0u && r.verdict == N48N_MODE_V_PASS, "t13 DIO attribution: a clean trial reports 0 / 0"); }
    { Env e; fresh(e, [](Env &e) { e.m.dioOnHoldRaise = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect(r.dio_hold_errs == 1u && r.dio_post_unblank_errs == 0u && r.dio_write_errs == 1u && r.verdict == N48N_MODE_V_PASS, "t13 DIO attribution: an error at the clock raise shows in dio_hold_errs (1) and not in dio_post_unblank_errs (0, which counts from the raise); reported, never judged (PASS)"); }
    { Env e; fresh(e, [](Env &e) { e.m.dioOnWrite = true; }); run(e, 120, 600); const n48n_mode_result &r = e.r;
      expect(r.dio_hold_errs == 0u && r.dio_post_unblank_errs == 1u, "t13 DIO attribution: an error at the DTO write shows in dio_post_unblank_errs (1), not in dio_hold_errs");
      expect(r.dio_write_errs == 1u, "t13 DIO attribution: and in the 0.0.606 dio_write_errs, unchanged"); }
    // 10. source pins of the ORDER (the behaviour tests above drive the real functions; these pin the text they depend on)
    { const std::string h = slurp(g_root + "/src/navi48-bringup/src/dcn/navi48_modetrial_pure.h");
      const std::string ap = fn_body(h, "inline Apply apply_120_locked("), rs = fn_body(h, "inline void restore_locked(");
      const size_t aMsa = ap.find("write_group_locked(hw, st, kGrpMsa"), aClr = ap.find("uf_clear_locked(hw, st)"), aUnb = ap.find("wrap_unblank_locked(hw, st)");
      expect(aMsa != std::string::npos && aClr > aMsa && aUnb > aClr, "pin: the apply's transition clear is after the MSA and before the unblank");
      const size_t rLat = rs.find("latch_lower_locked("), rClr = rs.find("uf_clear_locked(hw, st)", rLat), rUnb = rs.find("wrap_unblank_locked(hw, st, true)");
      expect(rLat != std::string::npos && rClr > rLat && rUnb > rClr, "pin: the restore's transition clear is after latch_lower and before wrap_unblank(true)");
      expect(rs.find("SUSPECTED") != std::string::npos && rs.find("vblank end 82 -> 38") != std::string::npos, "pin: the geometry-shrink hypothesis is recorded as SUSPECTED at the restore split");
      expect(rs.find("kUfAfter") == std::string::npos, "pin: the restore itself does not touch the judged after-restore kind (run_trial reads it after the verify window)"); }
}


// ---- T14 (0.0.609): the HELD mode of row 120 --------------------------------------------------------------------------------------------------------------------
// The REAL run_trial runs on a thread with a HoldCfg (as the kext's mt_hold_runner does); the test thread plays the launcher, the scanout client (hold_allows_acquire / hold_take_owner), the client session's close
// (hold_session_closed), ModeRelease (hold_request_release) and dcnmode 0 / the kext stop (request_abort + hold_abort_scan). The trial thread PAUSES on a frozen model once the hold is up, so every action lands at a
// known moment. h_scan records where the scanout plane's put-back sits in the trace against the restore's register writes and the clock messages.
struct HoldRig {
    Env &e; HoldCfg cfg; HoldCfg live; std::thread t; bool started = false;
    explicit HoldRig(Env &env) : e(env), cfg{ 120000u, 0u }, live{ 120000u, 0u } {}
    void start(uint32_t sess, uint32_t maxMs, uint32_t flags = 0u) {
        cfg = HoldCfg{ maxMs, flags }; live = cfg;
        expect(hold_launch_claim(e.st), "hold: the launch slot is claimed");
        hold_launch_prepare(e.st, sess, cfg);
        e.m.pauseHeld = true;
        started = true;
        t = std::thread([this] { tIsTrial = true; run_trial(e.hw, e.st, e.tm, 120, live.maxMs, 0u, &e.st.heldFinal, &live); tIsTrial = false; hold_runner_done(e.st); });
    }
    // Wait (real time) until the hold is UP and the trial thread is paused on the frozen model, or the runner is done. True = held and paused.
    bool wait_held() { for (int i = 0; i < 400000; i++) { if (ld(e.st.held) != 0u && e.m.inPause.load()) return true; if (ld(e.st.runnerDone) != 0u) return false; std::this_thread::sleep_for(std::chrono::microseconds(100)); } return false; }
    bool wait_done() { for (int i = 0; i < 400000; i++) { if (ld(e.st.runnerDone) != 0u) return true; std::this_thread::sleep_for(std::chrono::microseconds(100)); } return false; }
    void go() { e.m.pauseHeld = false; }
    const n48n_mode_result &fin() const { return e.st.heldFinal; }
    ~HoldRig() { e.m.pauseHeld = false; { std::lock_guard<std::mutex> g(e.m.mu); e.m.release = true; } e.m.cv.notify_all(); if (started && t.joinable()) t.join(); e.join(); }
};
static size_t trace_size(Env &e) { std::lock_guard<std::mutex> g(e.m.mu); return e.m.trace.size(); }
// The trace from position `from`: the first "SCAN" is before the first register write and the first clock message of the end sequence.
static bool scan_first(const std::vector<std::string> &t, size_t from) {
    const long sc = tpos(t, "SCAN", from), w = tposp(t, "W:", from), hm = tposp(t, "HM:", from);
    return sc >= 0 && (w < 0 || sc < w) && (hm < 0 || sc < hm);
}
static void hold_setup(Env &e) { expect(prep120(e), "(setup) row 50, the resync and the row-2 lock-hold proof all PASS on this boot"); e.m.writes.clear(); e.m.trace.clear(); e.m.holdMsgs = e.m.holdCalls = 0; e.m.scanCalls = 0; }

static void t14_hold() {
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 1\n");
    // ---- 1. the clean hold: entered only after the judged-PASS apply, ended by ModeRelease, scanout FIRST, then timing, then clocks ----
    {
        Env e; hold_setup(e); e.m.scanAcq = true; HoldRig h(e);
        h.start(1u, 60000u);
        { const bool up = h.wait_held(); if (!up && std::getenv("T14TRACE")) std::fprintf(stderr, "hold1: not up: verdict %u deny %u flags %#x fail_phase %u\n", h.fin().verdict, h.fin().deny, h.fin().flags, h.fin().fail_phase); expect(up, "hold 1: the hold is UP (held = 1) and the trial thread is paused on the frozen model"); }
        const n48n_mode_result en = [&] { std::lock_guard<std::mutex> g(gEng); return e.st.heldEntry; }();
        expect_u("hold 1: the entry snapshot's verdict is HELD", en.verdict, N48N_MODE_V_HELD);
        expect((en.flags & N48N_MODE_F_WROTE) && (en.flags & N48N_MODE_F_RATE_TRIAL_OK) && (en.flags & N48N_MODE_F_HOLD_HELD) && !(en.flags & N48N_MODE_F_UNDERFLOW_SEEN), "hold 1: the entry: registers written, the 120 Hz rate window judged OK, the clock hold raised, no underflow");
        expect(en.rate_trial_mhz > 119000u && en.rate_trial_mhz < 121000u && (en.hold_flags & N48N_HOLD_FL_ENTERED) && en.dwell_req_ms == 60000u && en.hold_end == 0u, "hold 1: the entry reports the 120 Hz rate, ENTERED, the max hold time and no end reason");
        expect(__atomic_load_n(&e.st.busy, __ATOMIC_SEQ_CST) == 1u && ld(e.st.holdOn) == 1u && ld(e.st.ending) == 0u && e.st.heldFinal.verdict == 0u, "hold 1: busy stays 1 while held, holdOn 1, not ending, the final result is not written yet");
        expect(!all_init(e) && e.m.rate() > 119.0 && e.m.rate() < 121.0, "hold 1: the display registers ARE at 120 Hz while held");
        // a second trial or hold cannot start now
        { n48n_mode_result r2; std::memset(&r2, 0, sizeof(r2)); run_trial(e.hw, e.st, e.tm, 120, 300, &r2); expect(r2.verdict == N48N_MODE_V_DENIED && r2.deny == N48N_MODE_D_BUSY, "hold 1: a second trial is DENIED (busy) while a hold is up and touched nothing"); expect(!hold_launch_claim(e.st), "hold 1: a second hold cannot claim the slot"); }
        expect(hold_allows_acquire(e.st, 1u) && !hold_allows_acquire(e.st, 2u) && !hold_allows_acquire(e.st, 0u), "hold 1: only the owning session (1) may Acquire; another and session 0 may not");
        const size_t n0 = trace_size(e);
        expect(hold_request_release(e.st), "hold 1: ModeRelease accepted (a hold is running)");
        expect(!hold_allows_acquire(e.st, 1u), "hold 1: after the release request no Acquire is allowed");
        h.go(); expect(h.wait_done(), "hold 1: the runner ends");
        const n48n_mode_result &f = h.fin(); const auto &t = e.m.trace;
        expect_u("hold 1: FINAL verdict PASS", f.verdict, N48N_MODE_V_PASS); expect_u("hold 1: hold_end = RELEASED", f.hold_end, N48N_HOLD_END_RELEASED);
        expect(f.latched == 0 && (f.flags & N48N_MODE_F_RESTORED) && (f.flags & N48N_MODE_F_REGS_OK) && (f.flags & N48N_MODE_F_RATE_AFTER_OK) && (f.flags & N48N_MODE_F_HOLD_RELEASED) && !(f.flags & N48N_MODE_F_HOLD_STUCK), "hold 1: restored, registers exact, 60 Hz back, clocks RELEASED, no latch");
        expect(all_init(e) && f.mismatch == 0 && f.rate_after_mhz > 59000u && f.rate_after_mhz < 61000u, "hold 1: the machine is the boot machine again at 60 Hz");
        expect((f.hold_flags & N48N_HOLD_FL_SCAN_ACTED) && !(f.hold_flags & N48N_HOLD_FL_SCAN_BAD), "hold 1: the scanout plane was put back first and verified (hold_flags)");
        expect_u("hold 1: scan_release called exactly once by the end sequence (idempotent second call is a no-op but is not made)", e.m.scanCalls >= 1 ? 1u : 0u, 1u);
        expect(scan_first(t, n0), "hold 1: ORDER: the scanout plane goes back BEFORE the first restore write and BEFORE the clock release");
        const long lastW = [&] { long l = -1; for (size_t i = n0; i < t.size(); i++) if (t[i].compare(0, 2, "W:") == 0) l = (long)i; return l; }(), hm = tposp(t, "HM:", n0);
        expect(lastW > 0 && hm > lastW, "hold 1: ORDER: the clock release messages come AFTER the last register restore (scanout, timing, clocks)");
        expect_u("hold 1: no scan_release under an engine lock, none from the watchdog thread", (uint64_t)(e.m.scanUnderLock + e.m.scanFromWd), 0u);
        expect(ld(e.st.held) == 0u && ld(e.st.holdOn) == 0u && ld(e.st.ending) == 1u && __atomic_load_n(&e.st.busy, __ATOMIC_SEQ_CST) == 0u && ld(e.st.launching) == 0u, "hold 1: held 0, holdOn 0, busy 0 and the slot free after the end");
        expect(!hold_allows_acquire(e.st, 1u), "hold 1: no Acquire after the end");
        expect(e.m.holdUnderLock == 0 && e.m.holdFromWd == 0, "hold 1: no clock call under an engine lock or from the watchdog");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 2\n");
    // ---- 2. every ordinary trial leaves the hold words alone (OFF identity of the state machine) ----
    {
        Env e; expect(prep120(e), "(setup)"); e.m.scanCalls = 0; run(e, 120, 600);
        expect_u("off: an ordinary row-120 trial PASSES", e.r.verdict, N48N_MODE_V_PASS);
        expect(e.m.scanCalls == 0 && ld(e.st.held) == 0u && ld(e.st.holdOn) == 0u && ld(e.st.ending) == 0u && ld(e.st.endReason) == 0u && e.r.hold_end == 0u && e.r.hold_flags == 0u && e.st.heldEntry.verdict == 0u, "off: no scan_release call, no hold word set, hold_end / hold_flags 0 in the result");
        expect(e.r.dwell_done_ms >= 600u && e.r.dwell_done_ms < 900u, "off: the fixed dwell still runs to its bound");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 3\n");
    // ---- 3. Acquire rules (hold_allows_acquire / hold_take_owner / hold_session_closed) ----
    {
        Env e; e.bind(); State &st = e.st;
        expect(!hold_allows_acquire(st, 1u), "acquire: not allowed with no hold up");
        stz(st.held, 1u); stz(st.ownerSeq, 5u); stz(st.launching, 1u);
        expect(hold_allows_acquire(st, 5u) && !hold_allows_acquire(st, 6u) && !hold_allows_acquire(st, 0u), "acquire: held, owner 5: session 5 yes, 6 and 0 no");
        for (int k = 0; k < 4; k++) {
            stz(st.ending, k == 0); stz(st.releaseReq, k == 1); stz(st.ownerGone, k == 2); if (k == 3) { stz(st.held, 0u); }
            expect(!hold_allows_acquire(st, 5u), "acquire: refused when ending / release requested / owner gone / not held");
        }
        stz(st.ending, 0u); stz(st.releaseReq, 0u); stz(st.ownerGone, 0u); stz(st.held, 1u);
        // a HANDOFF hold: issuing session 5 closes -> owner 0 with a deadline; any session may take it once
        stz(st.handoff, 1u);
        hold_session_closed(e.hw, st, 7u); expect(ld(st.ownerSeq) == 5u && ld(st.ownerGone) == 0u, "handoff: a close of some other session changes nothing");
        hold_session_closed(e.hw, st, 5u);
        expect(ld(st.ownerSeq) == 0u && ld(st.ownerGone) == 0u && __atomic_load_n(&st.handoffDeadlineUs, __ATOMIC_SEQ_CST) > e.m.vnow, "handoff: the issuing session's close leaves the hold ownerless with a deadline (NOT ended)");
        expect(hold_allows_acquire(st, 9u) && hold_allows_acquire(st, 10u) && !hold_allows_acquire(st, 0u), "handoff: an ownerless handoff hold may be Acquired by any real session");
        hold_take_owner(st, 9u);
        expect(ld(st.ownerSeq) == 9u && ld(st.handoff) == 0u && (ld(st.holdFlags) & N48N_HOLD_FL_TOOK_OVER), "handoff: the taker becomes the owner, the hold stops being a handoff, TOOK_OVER is recorded");
        hold_take_owner(st, 10u); expect(ld(st.ownerSeq) == 9u, "handoff: a second taker does not displace the owner");
        expect(hold_allows_acquire(st, 9u) && !hold_allows_acquire(st, 10u), "handoff: after the takeover only the owner may Acquire");
        hold_session_closed(e.hw, st, 9u); expect(ld(st.ownerGone) == 1u, "handoff: the TAKER's close ends the hold (ownerGone)");
        // no hold running: a close is ignored
        Env f; f.bind(); stz(f.st.ownerSeq, 3u); hold_session_closed(f.hw, f.st, 3u); expect(ld(f.st.ownerGone) == 0u && ld(f.st.ownerSeq) == 3u, "session close: ignored when no hold is launching");
        hold_take_owner(f.st, 4u); expect(ld(f.st.ownerSeq) == 3u, "take_owner: nothing when no hold is up");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 4\n");
    // ---- 4. the owning session's close / death ends the hold: full restore, clocks released, scanout first ----
    {
        Env e; hold_setup(e); e.m.scanAcq = true; HoldRig h(e); h.start(3u, 60000u);
        expect(h.wait_held(), "close: hold up"); const size_t n0 = trace_size(e);
        hold_session_closed(e.hw, e.st, 3u);
        h.go(); expect(h.wait_done(), "close: the runner ends"); const n48n_mode_result &f = h.fin();
        expect(f.verdict == N48N_MODE_V_PASS && f.hold_end == N48N_HOLD_END_OWNER_CLOSED && (f.flags & N48N_MODE_F_RESTORED) && (f.flags & N48N_MODE_F_HOLD_RELEASED) && all_init(e) && f.latched == 0, "close: PASS, end = OWNER_CLOSED, restored, clocks released, registers exact");
        expect(scan_first(e.m.trace, n0) && e.m.scanUnderLock == 0, "close: the scanout plane went back first, outside every lock");
    }
    {   // a HANDOFF hold whose issuing session closes stays up; the taker's close ends it
        Env e; hold_setup(e); e.m.scanAcq = true; HoldRig h(e); h.start(4u, 60000u, N48N_HOLD_F_HANDOFF);
        expect(h.wait_held(), "handoff: hold up"); expect((e.st.heldEntry.hold_flags & N48N_HOLD_FL_HANDOFF) != 0u, "handoff: the entry reports HANDOFF");
        hold_session_closed(e.hw, e.st, 4u);
        expect(ld(e.st.held) == 1u && ld(e.st.ownerGone) == 0u && hold_end_check(e.hw, e.st) == 0u, "handoff: after the issuing session closed the hold is still UP and nothing asks it to end");
        expect(hold_allows_acquire(e.st, 8u), "handoff: the next session may Acquire");
        hold_take_owner(e.st, 8u); hold_session_closed(e.hw, e.st, 8u);
        expect_u("handoff: the taker's close asks the hold to end (OWNER_CLOSED)", hold_end_check(e.hw, e.st), kHeOwnerClosed);
        h.go(); expect(h.wait_done(), "handoff: the runner ends");
        expect(h.fin().verdict == N48N_MODE_V_PASS && h.fin().hold_end == N48N_HOLD_END_OWNER_CLOSED && (h.fin().hold_flags & N48N_HOLD_FL_TOOK_OVER) && all_init(e), "handoff: PASS, OWNER_CLOSED, TOOK_OVER, restored");
    }
    {   // ... and with no taker the hold ends by itself 30 s after the issuing session's close
        Env e; hold_setup(e); HoldRig h(e); h.start(4u, 120000u, N48N_HOLD_F_HANDOFF);
        expect(h.wait_held(), "no taker: hold up"); hold_session_closed(e.hw, e.st, 4u); const uint64_t t0 = e.m.vnow;
        h.go(); expect(h.wait_done(), "no taker: the runner ends"); const n48n_mode_result &f = h.fin();
        expect(f.verdict == N48N_MODE_V_PASS && f.hold_end == N48N_HOLD_END_NO_TAKER && all_init(e), "no taker: PASS, end = NO_TAKER, restored");
        const uint64_t waited = (e.m.vnow - t0) / 1000ull;
        expect(waited >= kHandoffMs && waited < kHandoffMs + 15000u, "no taker: it waited the 30 s handoff window (virtual) and then restored");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 5\n");
    // ---- 5. the max hold time ----
    {
        Env e; hold_setup(e); HoldRig h(e); h.start(1u, 1000u); h.e.m.pauseHeld = false;
        expect(h.wait_done(), "max: the runner ends"); const n48n_mode_result &f = h.fin();
        expect(f.verdict == N48N_MODE_V_PASS && f.hold_end == N48N_HOLD_END_MAX_TIME && f.dwell_done_ms >= 1000u && f.dwell_done_ms < 1300u && all_init(e) && (f.flags & N48N_MODE_F_HOLD_RELEASED), "max 1 s: PASS, end = MAX_TIME, held 1.0..1.3 s, restored, clocks released");
        expect(!f.wd_fired && !(f.flags & N48N_MODE_F_WATCHDOG), "max: the loop's own bound ended it, the watchdog did not fire");
    }
    {   // the bounds
        Env e; expect(prep120(e), "(setup)"); n48n_mode_result r; HoldCfg c{ 300001u, 0u }; expect(!hold_cfg_ok(c), "bound: 300001 ms is refused"); c = HoldCfg{ 300000u, 0u }; expect(hold_cfg_ok(c), "bound: 300000 ms is allowed");
        expect(!hold_cfg_ok(HoldCfg{ 999u, 0u }) && hold_cfg_ok(HoldCfg{ 1000u, 0u }) && !hold_cfg_ok(HoldCfg{ 5000u, 2u }) && hold_cfg_ok(HoldCfg{ 5000u, 1u }), "bound: min 1 s; only the HANDOFF flag is accepted");
        expect_u("bound: the default is 120 s and the ABI max 300 s", kHoldDefaultMs * 1000ull + kHoldMaxMs, 120000ull * 1000 + 300000);
        c = HoldCfg{ 300001u, 0u }; std::memset(&r, 0, sizeof(r)); run_trial(e.hw, e.st, e.tm, 120, 300001u, 0u, &r, &c);
        expect(r.verdict == N48N_MODE_V_DENIED && r.deny == N48N_MODE_D_BAD_DWELL, "bound: a hold over 300 s is DENIED (bad dwell)");
        c = HoldCfg{ 5000u, 0u }; std::memset(&r, 0, sizeof(r)); const size_t w0 = e.m.writes.size(); run_trial(e.hw, e.st, e.tm, 120, 6000u, 0u, &r, &c);
        expect(r.verdict == N48N_MODE_V_DENIED && r.deny == N48N_MODE_D_BAD_DWELL && e.m.writes.size() == w0, "bound: the dwell argument must equal the max hold time (denied, nothing written)");
        std::memset(&r, 0, sizeof(r)); run_trial(e.hw, e.st, e.tm, 50, 5000u, 0u, &r, &c);
        expect(r.verdict == N48N_MODE_V_DENIED && r.deny == N48N_MODE_D_BAD_ROW && e.m.writes.size() == w0, "bound: a hold is row 120 only (row 50 with a hold is denied, nothing written)");
        expect(ld(e.st.holdOn) == 0u && ld(e.st.held) == 0u && e.m.scanCalls == 0, "bound: a denied hold leaves no hold word set");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 6\n");
    // ---- 6. the deny matrix applies to a hold (HELD is entered only after the ordinary gate) ----
    {
        struct { const char *n; uint32_t deny; void (*set)(Env &); } cases[] = {
            { "latched", N48N_MODE_D_LATCHED, [](Env &e) { e.st.latched = true; } }, { "row 120 off", N48N_MODE_D_ROW120_OFF, [](Env &e) { e.tm.row120 = false; } },
            { "no step-down", N48N_MODE_D_STEP_DOWN, [](Env &e) { e.st.step50Passed = false; } }, { "no row-2 proof", N48N_MODE_D_ROW2, [](Env &e) { e.st.row2Proven = false; } },
            { "plane acquired", N48N_MODE_D_ACQUIRED, [](Env &e) { e.m.planeAcq = true; } },
        };
        for (auto &c : cases) {
            Env e; hold_setup(e); c.set(e); const size_t w0 = e.m.writes.size(); HoldRig h(e); h.start(1u, 5000u); h.go(); expect(h.wait_done(), "deny: runner ends");
            char msg[120]; std::snprintf(msg, sizeof(msg), "deny (%s): a hold is DENIED with the trial's own reason and never enters HELD", c.n);
            expect(h.fin().verdict == N48N_MODE_V_DENIED && h.fin().deny == c.deny && ld(e.st.held) == 0u && e.st.heldEntry.verdict == 0u && e.m.writes.size() == w0, msg);
        }
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 7\n");
    // ---- 7. a hold whose apply / rate window is not clean never enters ----
    {
        Env e; hold_setup(e); e.m.optcAtApUnblank = true; HoldRig h(e); h.start(1u, 5000u); h.go(); expect(h.wait_done(), "no entry (underflow judged after the apply's unblank): runner ends");
        expect(ld(e.st.held) == 0u && e.st.heldEntry.verdict == 0u && h.fin().verdict == N48N_MODE_V_UNDERFLOW && !(h.fin().hold_flags & N48N_HOLD_FL_ENTERED) && h.fin().latched == 1 && all_init(e), "no entry: an underflow judged at the settle read: UNDERFLOW, restored, latched, HELD never published");
      expect(h.fin().dwell_done_ms == 0u && h.fin().hold_end == 0u, "no entry: a hold that did not enter never dwells (dwell_done_ms 0, no end reason)");
    }
    { Env e; hold_setup(e); e.m.dtoStaysAfterRestore = false; e.m.ignoreDto = true; HoldRig h(e); h.start(1u, 5000u); h.go(); expect(h.wait_done(), "no entry (rate never moved): runner ends");
      expect(ld(e.st.held) == 0u && h.fin().verdict == N48N_MODE_V_RATE && e.st.heldEntry.verdict == 0u && h.fin().dwell_done_ms == 0u, "no entry: a rate window outside the tolerance: RATE, HELD never published, no dwell"); }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 8\n");
    // ---- 8. failures detected by the sampler while HELD: each ends the hold with the full restore ----
    struct Fail { const char *n; uint32_t verdict; void (*set)(Env &); };
    const Fail fails[] = {
        { "underflow", N48N_MODE_V_UNDERFLOW, [](Env &e) { std::lock_guard<std::mutex> g(e.m.mu); e.m.ufAtUs = e.m.vnow + 600000ull; e.m.ufHubp = 0u; e.m.ufOptc = uf::kOptcOccurred; } },
        { "DIO error count growth", N48N_MODE_V_FIFO, [](Env &e) { std::lock_guard<std::mutex> g(e.m.mu); e.m.reg[kRegPixelRateCntl] += 0x10000u; } },
        { "DP stream lost", N48N_MODE_V_STREAM, [](Env &e) { std::lock_guard<std::mutex> g(e.m.mu); e.m.streamDown = true; e.m.streamDownUntilRestore = true; } },
        { "DFS clock fell under the need", N48N_MODE_V_CLOCK_LOST, [](Env &e) { std::lock_guard<std::mutex> g(e.m.mu); e.m.clkDropAtUs = e.m.vnow + 600000ull; } },
    };
    for (const Fail &fl : fails) {
        Env e; hold_setup(e); e.m.scanAcq = true; HoldRig h(e); h.start(1u, 60000u);
        char msg[160]; std::snprintf(msg, sizeof(msg), "sampler (%s): hold up", fl.n); expect(h.wait_held(), msg);
        const size_t n0 = trace_size(e); fl.set(e); h.go(); expect(h.wait_done(), "sampler: runner ends");
        const n48n_mode_result &f = h.fin();
        std::snprintf(msg, sizeof(msg), "sampler (%s): the verdict is the sampled failure, end = FAILURE, latched", fl.n); expect(f.verdict == fl.verdict && f.hold_end == N48N_HOLD_END_FAILURE && f.latched == 1, msg);
        std::snprintf(msg, sizeof(msg), "sampler (%s): registers restored exactly, 60 Hz back, ended within ~1 s of the fault", fl.n); expect(all_init(e) && f.mismatch == 0 && (f.flags & N48N_MODE_F_RATE_AFTER_OK) && f.dwell_done_ms < 2500u, msg);
        std::snprintf(msg, sizeof(msg), "sampler (%s): the scanout plane went back FIRST", fl.n); expect(scan_first(e.m.trace, n0) && e.m.scanUnderLock == 0, msg);
        std::snprintf(msg, sizeof(msg), "sampler (%s): the clock hold released only if the restore verified (a failed hold that restored still releases)", fl.n); expect((f.flags & N48N_MODE_F_HOLD_RELEASED) && !(f.flags & N48N_MODE_F_HOLD_STUCK), msg);
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 9\n");
    // ---- 9. dcnmode 0 / the kext stop during HELD: request_abort + hold_abort_scan, then the same restore ----
    for (int viaStop = 0; viaStop < 2; viaStop++) {
        Env e; hold_setup(e); e.m.scanAcq = true; HoldRig h(e); h.start(1u, 60000u);
        expect(h.wait_held(), "abort: hold up"); const size_t n0 = trace_size(e);
        request_abort(e.hw, e.st);
        // the pause holds the trial thread: hold_abort_scan puts the plane back from THIS thread (the emergency caller), before the trial thread wakes
        hold_abort_scan(e.hw, e.st);
        expect(e.m.scanCalls >= 1 && e.m.scanAcq == false && ld(e.st.ending) == 1u, "abort: the emergency caller put the scanout plane back FIRST (ending set) while the trial thread was still paused");
        expect(all_init(e) == false, "abort: ... and the timing is still 120 Hz at that point (scanout first, timing second)");
        h.go(); expect(h.wait_done(), "abort: runner ends"); const n48n_mode_result &f = h.fin();
        expect(f.verdict == N48N_MODE_V_ABORT && f.hold_end == N48N_HOLD_END_ABORT && f.latched == 1 && all_init(e) && (f.flags & N48N_MODE_F_RESTORED), "abort: ABORT, end = ABORT, latched, restored");
        expect(scan_first(e.m.trace, n0), "abort: the scan release is before every restore write");
        // the caller's own sequence afterwards finds nothing left to do (no clock hold is still held, the golden set is back)
        if (viaStop) { uint32_t restored = 0; bool have = false; const uint32_t bad = restore_from_golden(e.hw, e.st, &restored, &have); expect(have && bad == 0 && restored == 0, "kext stop: restore_from_golden after the ended hold writes nothing and nothing differs");
                       expect_u("kext stop: emergency_release finds no clock hold to release", emergency_release(e.hw, e.st, e.tm, true, bad, true, 0u), n48dal::kRlNotHeld); }
        else { Recover rc{}; emergency_recover(e.hw, e.st, &rc); expect(rc.have && rc.bad == 0 && rc.restored == 0 && rc.healthyAfter, "dcnmode 0: emergency_recover after the ended hold writes nothing, the back end is healthy");
               expect_u("dcnmode 0: emergency_release finds no clock hold to release", emergency_release(e.hw, e.st, e.tm, true, rc.bad, rc.healthyAfter, 3000u), n48dal::kRlNotHeld); }
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 10\n");
    // ---- 10. the scanout put-back did not verify: RESTORE, latched, the clock hold stays up (never released on a bad restore) ----
    {
        Env e; hold_setup(e); e.m.scanAcq = true; e.m.scanRc = 2u; HoldRig h(e); h.start(1u, 60000u);
        expect(h.wait_held(), "scan bad: hold up"); hold_request_release(e.st); h.go(); expect(h.wait_done(), "scan bad: runner ends"); const n48n_mode_result &f = h.fin();
        expect(f.verdict == N48N_MODE_V_RESTORE && f.latched == 1 && (f.hold_flags & N48N_HOLD_FL_SCAN_BAD) && (f.flags & N48N_MODE_F_RESTORE_FAILED), "scan bad: RESTORE, latched, SCAN_BAD");
        expect(all_init(e) && (f.flags & N48N_MODE_F_HOLD_HELD) && (f.flags & N48N_MODE_F_HOLD_STUCK) && !(f.flags & N48N_MODE_F_HOLD_RELEASED), "scan bad: the timing was still restored (the safe direction) but the clock hold is STUCK, not released");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 11\n");
    // ---- 11. the watchdog: a hold thread that stops beating is restored by the watchdog, scanout first, once ----
    {
        Env e; hold_setup(e); e.m.scanAcq = true; e.m.hangWhenHeld = true; e.m.pauseHeld = false; HoldRig h(e); h.start(1u, 60000u); h.e.m.pauseHeld = false;
        bool restored = false;
        for (int i = 0; i < 400000 && !e.m.hung.load(); i++) std::this_thread::sleep_for(std::chrono::microseconds(100));   // the hold thread is hung (the hold is up)
        const size_t n0 = trace_size(e);
        for (int i = 0; i < 400000 && !restored; i++) { std::this_thread::sleep_for(std::chrono::microseconds(100)); std::lock_guard<std::mutex> g(gEng); restored = e.st.restoreDone && e.st.wdFired; }
        expect(restored && e.m.hung.load(), "watchdog: it restored while the hold thread was hung");
        expect(scan_first(e.m.trace, n0) && e.m.scanFromWd >= 1 && e.m.scanUnderLock == 0, "watchdog: the scanout plane was put back first, by the watchdog's restore, outside the engine lock");
        const size_t wr = e.m.writes.size();
        { std::lock_guard<std::mutex> g(e.m.mu); e.m.release = true; } e.m.cv.notify_all();
        expect(h.wait_done(), "watchdog: the runner ends after the thread wakes"); const n48n_mode_result &f = h.fin();
        expect(f.verdict == N48N_MODE_V_WATCHDOG && f.wd_fired == 1 && f.latched == 1 && f.hold_end == N48N_HOLD_END_WATCHDOG && all_init(e) && e.m.writes.size() == wr, "watchdog: WATCHDOG verdict, end = WATCHDOG, restored exactly once");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 12\n");
    // ---- 12. an end that starts while a client still holds the plane: the plane is checked by hold_begin_end ONLY for holds ----
    {
        Env e; e.bind(); e.st.nWritten = 1; e.st.written[0] = kIdxDtoPhase; e.m.reg[0x141] = 201000000u; e.m.scanAcq = true;
        expect(restore_run(e.hw, e.st, false), "plain restore_run"); expect(e.m.scanCalls == 0, "a restore that is not a hold's never touches the scanout plane");
        Env f; f.bind(); stz(f.st.holdOn, 1u); f.st.nWritten = 1; f.st.written[0] = kIdxDtoPhase; f.m.reg[0x141] = 201000000u; f.m.scanAcq = true;
        expect(restore_run(f.hw, f.st, false) && f.m.scanCalls == 1 && ld(f.st.ending) == 1u, "a hold's restore_run puts the plane back first (one call) and sets ending");
        expect(!restore_run(f.hw, f.st, true) && f.m.scanCalls == 2, "a second party (the watchdog) also calls it, harmlessly: idempotent, and does not restore twice");
    }
    if (std::getenv("T14TRACE")) std::fprintf(stderr, "t14 block 13\n");
    // ---- 12b. a ModeRelease that arrives after the claim but BEFORE run_trial sets holdOn is latched, not dropped ----
    {
        Env e; hold_setup(e); HoldRig h(e);
        expect(hold_launch_claim(e.st), "early release: claimed");
        expect(ld(e.st.holdOn) == 0u && hold_request_release(e.st) && ld(e.st.releaseReq) == 1u, "early release: accepted and latched between the claim and the prepare, holdOn still 0");
        hold_launch_prepare(e.st, 1u, HoldCfg{ 60000u, 0u });
        expect(ld(e.st.releaseReq) == 1u, "early release: the prepare keeps the latched request");
        e.m.pauseHeld = true; h.cfg = HoldCfg{ 60000u, 0u }; h.live = h.cfg; h.started = true;
        h.t = std::thread([&h, &e] { tIsTrial = true; run_trial(e.hw, e.st, e.tm, 120, h.live.maxMs, 0u, &e.st.heldFinal, &h.live); tIsTrial = false; hold_runner_done(e.st); });
        expect(h.wait_held(), "early release: the hold still enters"); h.go(); expect(h.wait_done(), "early release: the runner ends");
        expect(h.fin().verdict == N48N_MODE_V_PASS && h.fin().hold_end == N48N_HOLD_END_RELEASED && h.fin().dwell_done_ms < 500u && all_init(e), "early release: the hold ended by the latched release at its first poll (RELEASED, restored)");
        expect(ld(e.st.releaseReq) == 0u && ld(e.st.launching) == 0u, "early release: the latch is cleared with the slot");
    }
    // ---- 13. ModeRelease with no hold, and the launch claim ----
    {
        State st; std::memset(&st, 0, sizeof(st));
        expect(!hold_request_release(st), "release: nothing running -> false (the kext answers DENIED not-held / the last result)");
        expect(ld(st.releaseReq) == 0u, "release: nothing running -> nothing latched");
        expect(hold_launch_claim(st) && !hold_launch_claim(st), "launch: one claim at a time"); hold_runner_done(st); expect(hold_launch_claim(st), "launch: the slot is free again after the runner is done");
    }
    // ---- 14. nothing in the scanout path assumes 60 Hz (the numbers a 120 Hz VUPDATE stream meets) ----
    {
        const uint64_t frameNs120 = 1000000000ull / 120ull;
        expect(n48scan::kStormLimit >= 4u * 120u && n48scan::kStormWindowNs == 1000000000ull, "120 Hz: the IRQ storm guard (1000 classified IRQs per second) is more than 8x the 120 VUPDATEs per second");
        n48scan::Storm sm{ 0, 0, false }; bool tripped = false; uint64_t now = 1000000000ull;
        for (int i = 0; i < 120 * 30; i++) { now += frameNs120; tripped = tripped || n48scan::storm_note(sm, now); }
        expect(!tripped, "120 Hz: thirty seconds of 120 VUPDATEs per second never trip the storm guard");
        n48scan::Storm s2{ 0, 0, false }; bool t2 = false; now = 1000000000ull;
        for (int i = 0; i < 2000; i++) { now += 500000ull; t2 = t2 || n48scan::storm_note(s2, now); }
        expect(t2, "120 Hz: a real storm (2000 IRQs per second) still trips it");
        expect(n48scan::kIdleNs >= 100ull * frameNs120 && !n48scan::idle_expired(1000000000ull + 4000000000ull, 1000000000ull) && n48scan::idle_expired(1000000000ull + 5000000000ull, 1000000000ull), "120 Hz: the 5 s idle deadline is 600 frames, not a frame count");
        expect(n48scan::kRestorePolls * 1000ull >= 10ull * frameNs120 / 1000ull && n48scan::kRestoreAttempts == 2u, "120 Hz: the console restore polls 200 x ~1 ms = 24 frames per attempt, twice");
        n48scan::FcExt fe{ 0, false }; uint32_t raw = 0xFFFFC0u; uint64_t last = n48scan::fc_extend(fe, raw); bool steady = true;
        for (int i = 0; i < 400; i++) { raw = (raw + 6u) & 0xFFFFFFu; const uint64_t v = n48scan::fc_extend(fe, raw); steady = steady && v == last + 6u; last = v; }   // 50 ms polls at 120 Hz across the 24-bit wrap
        expect(steady, "120 Hz: the 24-bit frame counter extends without a jump across its wrap at 6 frames per 50 ms poll");
        expect_u("120 Hz: the refresh the DTO and the totals give for the row-120 raster", n48scan::refresh_mhz(497750000ull, 2720u, 1525u), 119997);
    }
}


// ---- S14 (0.0.609): source pins of the HELD mode: the order and the lock discipline the behaviour tests above depend on ---------------------------------------------------------------------
// Both positions found and the first before the second (a bare `find() < find()` is true when the RIGHT one is missing, and `>` when the LEFT one is: a removed line would pass).
static bool ord(size_t a, size_t b) { return a != std::string::npos && b != std::string::npos && a < b; }
static void s_pins_hold(const std::string &root) {
    const std::string src = root + "/src/navi48-bringup/src/";
    const std::string dcn = slurp(src + "dcn/navi48_dcn.cpp"), dcnh = slurp(src + "dcn/navi48_dcn.hpp"), eng = slurp(src + "amd/native_s1c.cpp"), engh = slurp(src + "amd/native_s1c.h"), cli = slurp(src + "Navi48NativeClient.cpp"),
                      pure = slurp(src + "dcn/navi48_modetrial_pure.h"), abi = slurp(src + "Navi48NativeABI.h"), mode = slurp(root + "/tools/native/n48mode.c"), scan = slurp(root + "/tools/native/n48scan.c");
    expect(!dcn.empty() && !eng.empty() && !pure.empty() && !abi.empty() && !mode.empty() && !scan.empty() && !cli.empty(), "hold pins: the sources are readable");
    // ABI 1.7
    expect_u("ABI: selectors 17 / 18 and the count", N48N_SEL_MODE_HOLD * 10000ull + N48N_SEL_MODE_RELEASE * 100 + N48N_SEL_COUNT_1_7, 171800ull + 190000ull * 0 + 19 + 0);
    expect_u("ABI: verdict HELD 16, deny NOT_HELD 25", N48N_MODE_V_HELD * 100ull + N48N_MODE_D_NOT_HELD, 1625);
    expect_u("ABI: the result's hold words sit in the former reserved slots (204 / 252), the size is unchanged", __builtin_offsetof(n48n_mode_result, hold_end) * 1000ull + __builtin_offsetof(n48n_mode_result, hold_flags), 204252ull);
    expect(N48N_MODE_MAX_HOLD_MS == 300000u && N48N_MODE_DEFAULT_HOLD_MS == 120000u && N48N_HOLD_F_MASK == N48N_HOLD_F_HANDOFF && N48N_REL_F_MASK == N48N_REL_F_QUERY, "ABI: max hold 300 s, default 120 s, the flag masks");
    expect(abi.find("ABI 1.7 addendum") != std::string::npos && abi.find("N48N_SEL_MODE_HOLD") != std::string::npos && abi.find("HANDOFF") != std::string::npos && abi.find("scanout plane back to the console FIRST") != std::string::npos, "ABI: the header carries the 1.7 contract text");
    // the engine: order and lock discipline
    { const std::string rr = fn_body(pure, "inline bool restore_run("), hb = fn_body(pure, "inline void hold_begin_end("), ha = fn_body(pure, "inline bool hold_allows_acquire("), hs = fn_body(pure, "inline void hold_session_closed("), ht = fn_body(pure, "inline void hold_take_owner("),
                       rt = fn_body(pure, "inline void run_trial(const Hw &hw, State &st, const Timing &tm, uint32_t row, uint32_t dwellMs, uint32_t tflags, n48n_mode_result *r, const HoldCfg *hc = nullptr) {");
      expect(!rr.empty() && ord(rr.find("hold_begin_end(hw, st);"), rr.find("hw.lock(hw.ctx);")), "pin: every restore of a hold puts the scanout plane back BEFORE it takes the engine lock");
      expect(!hb.empty() && ord(hb.find("stz(st.ending, 1u);"), hb.find("hw.scan_release(hw.ctx)")) && ord(hb.find("hw.scan_release(hw.ctx)"), hb.find("hw.lock")) && ord(hb.find("if (ld(st.holdOn) == 0u) return;"), hb.find("stz(st.ending, 1u);")), "pin: hold_begin_end sets `ending`, then calls scan_release with no lock held (the lock is only taken afterwards, to record a bad restore); a no-op for a non-hold");
      expect(!ha.empty() && ha.find("hw.lock") == std::string::npos && ha.find("Hw") == std::string::npos && !ht.empty() && ht.find("hw.lock") == std::string::npos, "pin: hold_allows_acquire / hold_take_owner touch atomic words only (no engine lock: scanAcquire calls them under the scanout lock)");
      expect(!hs.empty() && hs.find("hw.lock") == std::string::npos && hs.find("hw.sleep") == std::string::npos, "pin: hold_session_closed takes no lock and never sleeps (the client's close calls it under its own lock)");
      const size_t meas = rt.find("// ---- MEASURE (4) ----"), ent = rt.find("stz(st.held, 1u);"), snap = rt.find("st.heldEntry = *r;"), rst = rt.find("// ---- RESTORE (6)");
      expect(ord(meas, ent) && ord(snap, ent) && ord(ent, rst), "pin: the hold is published (held = 1) after the rate window and before the restore, with the snapshot copied first");
      expect(ord(rt.find("(r->flags & N48N_MODE_F_RATE_TRIAL_OK) != 0u"), ent) && ord(rt.find("!anyFail()"), ent) && ord(rt.find("st.rep.ufSeen"), ent) && ord(rt.find("holdEnter = !ufJudged;"), ent), "pin: HELD needs no failure, the rate window judged OK and no underflow judged at the settle read");
      expect(rt.find("(hc == nullptr || holdEnter)") != std::string::npos && rt.find("hend = hold_end_check(hw, st)") != std::string::npos, "pin: a hold trial that did not enter never dwells; the dwell loop of a hold asks hold_end_check every poll");
      expect(count_of(rt, "stz(st.holdOn, 0u)") == 2 && rt.rfind("stz(st.held, 0u); stz(st.holdOn, 0u);") != std::string::npos && rt.rfind("stz(st.held, 0u); stz(st.holdOn, 0u);") < rt.rfind("__atomic_store_n(&st.busy, 0u, __ATOMIC_RELEASE);"), "pin: holdOn is cleared on both exits (denied and finished) and held / holdOn drop BEFORE the final busy release");
      expect(rt.find("wd_window_ms(tm, dwellMs + (hc != nullptr ? kHoldEndSlackMs : 0u), wrap, mms)") != std::string::npos, "pin: the watchdog's window carries the max hold time and the end sequence's slack");
      expect(fn_body(pure, "inline void request_abort(").find("hold_") == std::string::npos, "pin: request_abort itself is unchanged (the emergency callers add hold_abort_scan)");
      expect(pure.find("uint32_t (*scan_release)(void *) = nullptr;") != std::string::npos && pure.find("Takes the scanout lock and SLEEPS: called with NO lock held") != std::string::npos, "pin: the Hw callback is declared with its lock contract"); }
    // the kext glue
    { const std::string sa = fn_body(dcn, "uint32_t scanAcquire(uint64_t out[2], uint32_t sess) {"), mh = fn_body(dcn, "uint32_t modeHold(uint32_t sess, uint64_t maxMs, uint64_t flags, struct n48n_mode_result *o) {"),
                       mr = fn_body(dcn, "uint32_t modeRelease(uint64_t flags, struct n48n_mode_result *o) {"), ru = fn_body(dcn, "static void mt_hold_runner(void *, wait_result_t) {"), ss = fn_body(dcn, "static uint32_t mt_scan_release(void *) {"),
                       er = fn_body(dcn, "static bool mt_emergency_restore(const char *why) {"), sh = fn_body(dcn, "static void mt_shutdown() {"), cl = fn_body(dcn, "void modeHoldSessionClosed(uint32_t sess) {");
      expect(!sa.empty() && sa.find("&& !n48mt::hold_allows_acquire(gMt, sess)") != std::string::npos && sa.find("gMtLock") == std::string::npos && sa.find("mt_lock") == std::string::npos && ord(sa.find("gScan.acquired = true;"), sa.find("hold_take_owner(gMt, sess);")), "pin: scanAcquire refuses while busy EXCEPT a hold this session may take; it never takes gMtLock; ownership passes only after the acquisition succeeded");
      expect(!mh.empty() && mh.find("IOLockLock") == std::string::npos && ord(mh.find("hold_launch_claim(gMt)"), mh.find("hold_launch_prepare(gMt, sess, cfg)")) && ord(mh.find("hold_launch_prepare(gMt, sess, cfg)"), mh.find("kernel_thread_start(&mt_hold_runner")), "pin: modeHold claims the slot, sets the owner words, THEN starts the thread; no lock statement");
      expect(mh.find("N48N_MODE_D_BAD_DWELL") != std::string::npos && mh.find("hold_cfg_ok(cfg)") != std::string::npos && mh.find("N48N_MODE_D_BUSY") != std::string::npos && mh.find("gMtHoldAlive") != std::string::npos, "pin: modeHold validates the bound, answers busy, counts its thread");
      expect(!ru.empty() && ru.find("n48mt::run_trial(kMtHw, gMt, mt_timing(), N48N_MODE_ROW_120, cfg.maxMs, 0u, &gMt.heldFinal, &cfg);") != std::string::npos && ord(ru.find("run_trial("), ru.find("hold_runner_done(gMt);")) && ord(ru.find("hold_runner_done(gMt);"), ru.find("gMtHoldAlive")), "pin: the runner thread runs the real trial as row 120 with the hold config into the persistent result, then frees the slot, then uncounts itself");
      expect(!mr.empty() && mr.find("IOLockLock") == std::string::npos && mr.find("hold_request_release(gMt)") != std::string::npos && mr.find("N48N_MODE_D_NOT_HELD") != std::string::npos && ord(mr.find("hold_request_release(gMt)"), mr.find("for (uint32_t i = 0; i < 4000u; i++) {")) && mr.find("n48mt::ld(gMt.runnerDone) != 0u && n48mt::ld(gMt.launching) == 0u") != std::string::npos, "pin: modeRelease takes no lock statement, requests the release, WAITS (bounded) for the runner to be done and the slot free, answers not-held when no hold ran");
      expect(!ss.empty() && ss.find("scan_restore(") != std::string::npos && ss.find("IOLock") == std::string::npos && ss.find("mt_plane_acquired(nullptr)") != std::string::npos && ss.find("scan_restore(\"row-120 hold ending\", true, 0u, o);") != std::string::npos && ss.find("return o[0] != 1ull ? 2u : (acq ? 1u : 0u);") != std::string::npos, "pin: the scanout put-back is scan_restore (the ONE console restore), takes no lock of its own");
      expect(dcn.find("mt_hold_state, mt_scan_release };") != std::string::npos, "pin: the callback is wired into the Hw table");
      for (const std::string *f : { &er, &sh }) expect(ord(f->find("request_abort(kMtHw, gMt);"), f->find("hold_abort_scan(kMtHw, gMt);")) && ord(f->find("hold_abort_scan(kMtHw, gMt);"), f->find("wait_idle(")), "pin: dcnmode 0 and the kext stop put a running hold's scanout plane back right after the abort request, before they wait");
      expect(sh.find("|| __atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE) != 0u); i++)") != std::string::npos, "pin: the kext stop waits for the hold thread as well (in its wait loop, not only in the log line)");
      expect(cl.find("n48mt::hold_session_closed(kMtHw, gMt, sess);") != std::string::npos && cl.find("mt_lock") == std::string::npos && cl.find("IOSleep") == std::string::npos, "pin: the session hook is the pure function only (no lock, no sleep)");
      expect(dcn.find("0.0.609 (HELD mode, row 120): LOCK ORDER") != std::string::npos && dcn.find("gMtLock is a LEAF and is never held together with the scan lock") != std::string::npos, "pin: the lock order is written down where the locks are");
      const std::string mt0 = dcn.substr(dcn.find("// ---- build 0.0.605 (native S2d): the timed mode trial - the kext half")), mt = mt0.substr(0, mt0.find("// build 0.0.661 (M6 Stage 1b, ABI 1.11"));   // 0.0.661: the instance-2 scanout block that ends the file is its own (tests/native_scanx_test.cpp pins it)
      expect(mt.find("gCliLock") == std::string::npos && mt.find("SetHardMin") == std::string::npos, "pin: the glue names neither the client lock nor a DAL message (0.0.609 additions included)");
      expect(dcnh.find("uint32_t modeHold(uint32_t sess, uint64_t maxMs, uint64_t flags, struct n48n_mode_result *o);") != std::string::npos && dcnh.find("uint32_t modeRelease(uint64_t flags, struct n48n_mode_result *o);") != std::string::npos && dcnh.find("void modeHoldSessionClosed(uint32_t sess);") != std::string::npos && dcnh.find("uint32_t scanAcquire(uint64_t out[2], uint32_t sess);") != std::string::npos, "pin: declared"); }
    // the client layer
    { const std::string mh = fn_body(eng, "IOReturn n1c_mode_hold("), mr = fn_body(eng, "IOReturn n1c_mode_release("), cl = fn_body(eng, "void n1c_close("), sa = fn_body(eng, "IOReturn n1c_scan_acquire(");
      expect(!mh.empty() && mh.find("if (!sess_hello(ref)) return kIOReturnNotReady;") != std::string::npos && mh.find("N48N_HOLD_F_MASK") != std::string::npos && mh.find("IOLockLock") == std::string::npos && mh.find("sess_lock(") == std::string::npos && mh.find("n48dcn::modeHold(ref.id,") != std::string::npos, "pin: n1c_mode_hold: Hello first, only the HANDOFF flag, no client or session lock held while the launch waits, the caller's session id is passed");
      expect(!mr.empty() && mr.find("if (!sess_hello(ref)) return kIOReturnNotReady;") != std::string::npos && mr.find("N48N_REL_F_MASK") != std::string::npos && mr.find("IOLockLock") == std::string::npos, "pin: n1c_mode_release: Hello first, only the QUERY flag, no client lock");
      const size_t td = cl.find("scan_teardown(s, how, tr);"), hk = cl.find("n48dcn::modeHoldSessionClosed(s->id);"), hg = cl.find("bool leak = hung_now();");
      expect(!cl.empty() && ord(td, hk) && ord(hk, hg), "pin: the close puts the console back FIRST, then tells the hold its owner (THIS session's id, 0.0.627) is gone (atomic words only), before anything is freed");
      expect(sa.find("n48dcn::scanAcquire(out, s->id)") != std::string::npos && ord(sa.find("IOLockLock(gCliLock)"), sa.find("n48dcn::scanAcquire(")), "pin: Acquire passes the session id, under the client lock");
      expect(eng.find("__atomic_store_n(&s->id, ++gSessSeqCounter, __ATOMIC_SEQ_CST);") != std::string::npos, "pin: every open gets a new session id");
      expect(engh.find("IOReturn n1c_mode_hold(const N1cRef &ref, uint64_t maxMs, uint64_t flags, n48n_mode_result *out);") != std::string::npos && engh.find("IOReturn n1c_mode_release(const N1cRef &ref, uint64_t flags, n48n_mode_result *out);") != std::string::npos, "pin: declared");
      expect(cli.find("case N48N_SEL_MODE_HOLD:") != std::string::npos && cli.find("shape(2, 0, 0, sizeof(n48n_mode_result))") != std::string::npos && cli.find("amdgpu::n1c_mode_hold(r, si[0], si[1],") != std::string::npos && cli.find("case N48N_SEL_MODE_RELEASE:") != std::string::npos && cli.find("shape(1, 0, 0, sizeof(n48n_mode_result))") != std::string::npos && cli.find("amdgpu::n1c_mode_release(r, si[0],") != std::string::npos, "pin: the client dispatches selectors 17 / 18 with their exact shapes"); }
    // the tools
    expect(mode.find("N48N_SEL_MODE_HOLD") != std::string::npos && mode.find("N48N_SEL_MODE_RELEASE") != std::string::npos && mode.find("\"hold120\"") != std::string::npos && mode.find("--run") != std::string::npos && mode.find("--max-ms") != std::string::npos && mode.find("\"release\"") != std::string::npos &&
           mode.find("N48N_HOLD_F_HANDOFF") != std::string::npos && mode.find("execvp(") != std::string::npos && mode.find("FINAL result of the hold") != std::string::npos && mode.find("RC_EXCLUSIVE") != std::string::npos, "pin: n48mode has hold120 [--max-ms] [--run CMD] and release, with the handoff, the exec and the final report");
    expect(mode.find("IOServiceClose(c);\n        usleep(300000);") != std::string::npos && mode.find("open_session(&c, h, 15000)") != std::string::npos, "pin: n48mode --run closes its (exclusive) session before the command starts and re-opens one to end the hold");
    expect(scan.find("--hold120") != std::string::npos && scan.find("N48N_SEL_MODE_HOLD") != std::string::npos && scan.find("N48N_SEL_MODE_RELEASE") != std::string::npos && scan.find("&& holdOk;") != std::string::npos && scan.find("const uint64_t hin[2] = { 60000u, 0 };") != std::string::npos, "pin: n48scan --hold120 holds the mode, scans, releases and judges the final verdict");
    // banned strings
    { const std::string b1 = std::string("+0x") + "280", b2 = std::string("+0x") + "282", b3 = std::string("+0x") + "299";
      for (const std::string *f : { &dcn, &eng, &pure, &abi, &cli }) expect(f->find(b1) == std::string::npos && f->find(b2) == std::string::npos && f->find(b3) == std::string::npos, "hold pins: banned strings absent from the sources"); }
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : "."; g_root = root; setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc > 2 && !std::strcmp(argv[2], "t14")) { t14_hold(); std::printf("native_s2d_test (t14 only): %d checks, %d failed\n", gRun, gFail); return gFail == 0 ? 0 : 1; }
    t1_tables(); t2_clocks(); t3_deny(); t4_trials(); t5_watchdog(); t6_full_restore(); t7_underflow(); t8_resync(); t9_lock(); t10_wrapper(); t11_addresses(); t12_row120(); t13_transitions(); t14_hold(); s_pins(root); s_pins_hold(root);
    std::printf("native_s2d_test: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
