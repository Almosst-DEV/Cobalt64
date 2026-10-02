// native_s2_dal_test.cpp - build 0.0.604 (native step S2-DISPCLK): the DAL mailbox sender, the allowlist, the DID -> MHz table, the E1b / E2 / E3 / E4
// step runner, the sticky stop latch and the shared mailbox lock. It compiles the REAL kernel sources amd/smu_dal.cpp and amd/smu_v14_0.cpp on the
// host (a shim for the kernel KPI in tests/dalshim, IOSleep advancing a fake clock; the two -D flags turn the headers' x86 `sfence` inline asm into an empty
// statement, because the host is arm64) against a simulated PMFW + clock block + frame counter, so every
// decision is the kext's own code running, not a model of it. Each scenario runs in a forked child: the latch, the recorded DPM maxima and the busy
// flag are per-boot statics of the real code.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -pthread '-D__asm__=0;' '-D__volatile__(...)=' \
//       -I src/navi48-bringup/tests/dalshim -I src/navi48-bringup/src -I src/navi48-bringup/src/amd \
//       src/navi48-bringup/tests/native_s2_dal_test.cpp src/navi48-bringup/src/amd/smu_dal.cpp src/navi48-bringup/src/amd/smu_v14_0.cpp -o /tmp/native_s2_dal
//   /tmp/native_s2_dal .          (run from the repo root; the argument is the repo root the source pins read from)
//   tests/native_s2_dal_plant.sh plants breaks and shows every check that catches them.
// Covers:
//   P1  the DID -> MHz table (C1): 0x64 = 125 MHz, 0x24 = 500, 0x23 = 514.28, 0x41 = 272.72, edges; the VCO; decode_clocks on the E1 registers;
//   P2  the allowlist matrix (every message id, every level, the caps, clk 0 / 601 / 249, 0x5, 0x9 at level 1, above the E1b max, no E1b);
//   P3  verdict rules, plans, the latch, the pre-gate, frame arithmetic;
//   B1  the sender: gate, refusals write nothing, RESP == 0 refuses, ordering, timeouts, every reply code;
//   B2  E1b: the exact message list, the tables, SHORT / REFUSED / TIMEOUT / GLITCH, the kHz reading;
//   B3  E2 / E3 / E4: PASS with the exact messages and order (DISPCLK first, restore reversed), and every failure verdict with its restore behaviour;
//   B4  the latch, the busy flag, the level ordering, the allowlist inside a step;
//   B5  lock sharing: PPSMC and DAL serialise, recursion, allocation failure;
//   S   source pins: three WREG32 only, callers, IOSleep in every wait, bounded loops, the ABI 1.2 contract text, the tool, the version.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <map>
#include <new>
#include <cstdarg>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>

#include <kern/thread.h>       // the shims of tests/dalshim (the kernel headers are not on the include path of a host build)
#include <libkern/OSAtomic.h>
#include "smu_dal_pure.h"
#include "smu_dal.h"
#include "amdgpu_smu.h"
#include "native_s1b.h"
#include "Navi48NativeABI.h"
#include "../src/dcn/navi48_dcn.hpp"

using namespace n48dal;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) { std::ifstream f(path); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }

// =====================================================================================================================================
// The simulated machine
// =====================================================================================================================================
int gDalShimLockCount = 0, gDalShimLockAllocFail = 0;
static uint64_t gNowNs = 1000000000ull;
static pthread_mutex_t gTimeMu = PTHREAD_MUTEX_INITIALIZER;

struct Rule { uint32_t msg; uint32_t nth; bool hang; uint32_t resp; };   // nth: 1-based occurrence of that msg (0 = every one)
struct World {
    uint32_t reg[0x17000];
    // PMFW
    bool pending; uint32_t pmsg, parg; uint64_t due;
    uint32_t latencyMs;
    std::vector<Rule> rules;
    std::map<uint32_t, uint32_t> seenCount;
    std::vector<std::pair<uint32_t, uint32_t>> seen;   // every message the PMFW received
    std::vector<uint32_t> ppsmcSeen;
    uint32_t hardmin[2];
    std::vector<uint32_t> dpm[2];
    bool kHzReplies;                // GetDcModeMaxDpmFreq replies in kHz (0xB replies are never kHz: Linux masks them to 16 bits)
    bool fineGrained[2];            // the level-count reply of that clock carries bit 31
    bool acquired;                  // ScanoutQuery says the plane is acquired
    bool otherBelowOnRaise;         // a raise of one clock drops the OTHER clock below its start
    uint32_t smuVersion, ifVersion, headerVersion;
    uint32_t nLevelsOverride[2];   // 0 = the table's size
    // clocks
    uint32_t pll; uint32_t did[2];
    bool follow[2]; uint32_t moveClockOnMsg; bool chgDoneClearOnRaise; bool dppMovesWithDisp; bool regressOnRaise; bool dentistStale; bool chgDoneClear; bool restoreDidOverride; uint32_t restoreDid;
    uint32_t dentistFrozen;
    bool hm15Never; uint32_t hm15Polls; uint32_t hm15Count;
    bool raiseSeen; uint64_t raiseNs;
    // frames
    double fps; double fpsAfter; uint64_t fpsAfterDelayNs; bool useFpsAfter;
    double frameAcc; uint64_t lastFrameNs; bool lit; bool frameFail;
};
static World W;
static amdgpu::DeviceContext gDev;
static amdgpu::NativeS1bState gS1b;
static struct { bool present; uint32_t val; } gBootArg;
static std::vector<std::string> gLog;
static int gLongLine = 0;

static void frames_advance() {
    const uint64_t t = gNowNs;
    if (t > W.lastFrameNs) {
        double f = W.fps;
        if (W.useFpsAfter && W.raiseSeen && t >= W.raiseNs + W.fpsAfterDelayNs) f = W.fpsAfter;
        W.frameAcc += (double)(t - W.lastFrameNs) * f / 1e9;
        W.lastFrameNs = t;
    }
}
static uint32_t dentist_value() {
    uint32_t d = (W.did[1] << 24) | 0x007F0000u | (W.did[0] << 8) | W.did[0];
    if (W.chgDoneClear) d &= ~kDentistChgDone;
    if (W.dentistStale) return W.dentistFrozen;
    return d;
}
static void sync_regs() {
    W.reg[kRegPllReq] = W.pll; W.reg[kRegDfs0] = W.did[0]; W.reg[kRegDfs1] = W.did[1]; W.reg[kRegDentist] = dentist_value();
}
static uint32_t did_for_level(uint32_t mhz) {   // the coarsest DID whose clock is at least `mhz` (0x41 stays the floor)
    if (mhz * 1000u <= did_khz(vco_khz(W.pll), 0x41)) return 0x41;
    for (uint32_t d = 0x41; d >= 0x08; d--) if (did_khz(vco_khz(W.pll), d) >= mhz * 1000u) return d;
    return 0x08;
}
static uint32_t grant_level(int i, uint32_t mhz) { uint32_t best = 0; for (uint32_t l : W.dpm[i]) if (l >= mhz && (best == 0 || l < best)) best = l; return best ? best : W.dpm[i].back(); }
static void handle(uint32_t msg, uint32_t arg) {
    W.seen.push_back({ msg, arg });
    const uint32_t nth = ++W.seenCount[msg];
    if (W.moveClockOnMsg && msg == W.moveClockOnMsg) W.did[0] = 0x40;   // a query that (wrongly) moves a clock
    for (const Rule &r : W.rules) if (r.msg == msg && (r.nth == 0 || r.nth == nth)) {
        if (r.hang) return;                       // never answers: RESP stays 0
        W.reg[kRegResp] = r.resp; W.reg[kRegArg] = 0; return;
    }
    uint32_t ret = 0;
    switch (msg) {
    case kMsgGetSmuVersion: ret = W.smuVersion; break;
    case kMsgGetDriverIfVersion: ret = W.ifVersion; break;
    case kMsgGetMsgHeaderVersion: ret = W.headerVersion; break;
    case kMsgGetDpmFreqByIndex: {
        const int i = (int)(arg >> 16) - 6; const uint32_t idx = arg & 0xFFFF;
        if (i < 0 || i > 1) { W.reg[kRegResp] = kRespUnknownCmd; return; }
        // Linux reads the count reply as: bit 31 = fine-grained (2 levels), else the low 8 bits; real replies carry flag bits above (bit 28 here), and every
        // per-level reply carries garbage above bit 15 that dcn401_init_single_clock masks away.
        if (idx == 0xFF) ret = W.nLevelsOverride[i] ? W.nLevelsOverride[i] : (W.fineGrained[i] ? 0x80000000u : (0x10000000u | (uint32_t)W.dpm[i].size()));
        else ret = idx < W.dpm[i].size() ? (0x00120000u | W.dpm[i][idx]) : 0;
        break;
    }
    case kMsgGetDcModeMaxDpmFreq: { const int i = (int)(arg >> 16) - 6; ret = (i == 0 || i == 1) ? W.dpm[i].back() * (W.kHzReplies ? 1000u : 1u) : 0; break; }
    case kMsgSetHardMinByFreq: {
        const int i = (int)(arg >> 16) - 6; const uint32_t mhz = arg & 0xFFFF;
        if (i < 0 || i > 1) { W.reg[kRegResp] = kRespFailed; return; }
        const bool raise = mhz > 273u;
        if (raise && !W.raiseSeen) { W.raiseSeen = true; W.raiseNs = gNowNs; }
        if (raise && W.chgDoneClearOnRaise) W.chgDoneClear = true;
        W.hardmin[i] = mhz; W.hm15Count = 0;
        const uint32_t lvl = grant_level(i, mhz);
        uint32_t nd = did_for_level(lvl);
        if (!raise) nd = did_for_level(mhz);   // 0.0.606 (F1): a release honours the exact floor requested: 272 -> DID 0x41 (the boot clock), 273 -> DID 0x40 (281.25 MHz), as the DID arithmetic says
        if (!raise && W.restoreDidOverride) nd = W.restoreDid;
        if (raise && W.regressOnRaise) nd = 0x42;
        if (raise && !W.follow[i]) nd = W.did[i];
        W.did[i] = nd;
        if (raise && i == 0 && W.dppMovesWithDisp) W.did[1] = nd;
        if (raise && W.otherBelowOnRaise) W.did[1 - i] = 0x42;
        ret = lvl;
        break;
    }
    case kMsgReturnHardMinStatus: {
        W.hm15Count++;
        ret = (!W.hm15Never && W.hm15Count > W.hm15Polls) ? ((1u << 6) | (1u << 7)) : 0u;
        break;
    }
    default: W.reg[kRegResp] = kRespUnknownCmd; return;
    }
    W.reg[kRegArg] = ret; W.reg[kRegResp] = kRespOk;
}
static void service() {
    sync_regs();
    // PPSMC mailbox: TestMessage echoes param + 1; every other message answers OK with 0
    if (W.reg[0x16282] != 0u) {
        W.ppsmcSeen.push_back(W.reg[0x16282]);
        W.reg[0x16292] = W.reg[0x16292] + 1u; W.reg[0x1629A] = 1u; W.reg[0x16282] = 0u;
    }
    if (!W.pending && W.reg[kRegMsg] != 0u) { W.pending = true; W.pmsg = W.reg[kRegMsg]; W.parg = W.reg[kRegArg]; W.due = gNowNs + (uint64_t)W.latencyMs * 1000000ull; }
    if (W.pending && gNowNs >= W.due) { W.pending = false; W.reg[kRegMsg] = 0u; handle(W.pmsg, W.parg); }
    sync_regs();
}
void (*gSleepHook)() = nullptr;
extern "C" void dalshim_sleep(unsigned ms) {
    pthread_mutex_lock(&gTimeMu);
    gNowNs += (uint64_t)ms * 1000000ull; frames_advance(); service();
    pthread_mutex_unlock(&gTimeMu);
    if (gSleepHook) gSleepHook();
}
extern "C" void clock_get_uptime(uint64_t *t) { pthread_mutex_lock(&gTimeMu); gNowNs += 1000ull; *t = gNowNs; pthread_mutex_unlock(&gTimeMu); }
extern "C" void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
extern "C" thread_t current_thread(void) { return (thread_t)(uintptr_t)pthread_self(); }
extern "C" bool OSCompareAndSwap(UInt32 o, UInt32 n, volatile UInt32 *p) { return __sync_bool_compare_and_swap(p, o, n); }
extern "C" bool OSCompareAndSwapPtr(void *o, void *n, void *volatile *p) { return __sync_bool_compare_and_swap(p, o, n); }
extern "C" bool PE_parse_boot_argn(const char *name, void *out, int sz) {
    if (std::strcmp(name, "navi48-dalsmc") != 0 || !gBootArg.present || sz != 4) return false;
    std::memcpy(out, &gBootArg.val, 4); return true;
}
namespace amdgpu {
void n48_logf(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (std::strlen(buf) >= 512) gLongLine++;
    gLog.push_back(buf);
}
const NativeS1bState &native_s1b_state() { return gS1b; }
}
namespace n48dcn {
uint32_t scanStatus(struct n48n_scan_status *o) {
    if (W.frameFail) return 0xe00002d8u;
    pthread_mutex_lock(&gTimeMu); gNowNs += 1000ull; frames_advance(); pthread_mutex_unlock(&gTimeMu);
    std::memset(o, 0, sizeof(*o)); o->frame_count = (uint64_t)W.frameAcc; return 0;
}
bool gTrialBusy = false;
bool modeTrialBusy() { return gTrialBusy; }
uint32_t scanQuery(struct n48n_scan_query *o) {
    std::memset(o, 0, sizeof(*o)); o->flags = (W.lit ? N48N_SCANQ_LIT : 0u) | (W.acquired ? N48N_SCANQ_ACQUIRED : 0u); return 0;
}
}

static void world_reset(uint32_t level, bool bootArg = true) {
    W.~World(); new (&W) World();
    std::memset(W.reg, 0, sizeof(W.reg));
    W.reg[kRegResp] = kRespOk;   // the E1 reading: RESP = 1, MSG = 0, ARG = 0
    W.latencyMs = 2;
    W.dpm[0] = { 273, 300, 400, 540, 600 }; W.dpm[1] = { 273, 300, 400, 540, 600 };
    W.smuVersion = 0x00500A00u; W.ifVersion = 1; W.headerVersion = 0x2E;
    W.pll = 0x2d; W.did[0] = W.did[1] = 0x41; W.follow[0] = W.follow[1] = true;
    W.hm15Polls = 2;
    W.fps = 60.0; W.fpsAfter = 60.0; W.fpsAfterDelayNs = 3000000000ull;
    W.lit = true; W.lastFrameNs = gNowNs; W.frameAcc = 1000.0;
    sync_regs(); W.dentistFrozen = W.reg[kRegDentist];
    gDev.rmmio = W.reg; gDev.rmmioSize = sizeof(W.reg);
    gDev.ip.setBase(amdgpu::IPBlock::MP1, 1, 0x16200u);
    gS1b.gate = n48native::kGateOn; gS1b.positivePass = true;
    gBootArg.present = bootArg; gBootArg.val = level;
    gLog.clear();
}
static n48n_dal_result gRes;
static const n48n_dal_result &step(uint32_t s) {
    IOReturn rc = amdgpu::dal_run_step(gDev, s, &gRes);
    expect_u("dal_run_step returns Success for a well-formed step", (uint32_t)rc, 0);
    return gRes;
}
static uint32_t hm_param(uint32_t clk, uint32_t mhz) { return (clk << 16) | mhz; }
static void expect_verdict(const char *what, const n48n_dal_result &r, uint32_t v) {
    gRun++;
    if (r.verdict != v) { gFail++; std::printf("FAIL: %s: verdict %s(%u) want %s(%u) rc %#x failmsg %#x deny %u refuse %u flags %#x\n", what, verdict_name(r.verdict), r.verdict, verdict_name(v), v, r.rc, r.fail_msg, r.deny, r.refuse, r.flags); }
}

// =====================================================================================================================================
// P: the pure half
// =====================================================================================================================================
static Decoded dec_did(uint32_t d0, uint32_t d1, bool agree = true, bool done = true);
static void p1_did() {
    expect_u("DID 0x64 is divider 144", did_divider(0x64), 144);
    expect_u("DID 0x64 = 125 MHz at VCO 4500 (NOT 500)", did_khz(4500000, 0x64), 125000);
    expect_u("DID 0x24 = 500 MHz", did_khz(4500000, 0x24), 500000);
    expect_u("DID 0x23 = 514.28 MHz", did_khz(4500000, 0x23), 514285);
    expect_u("DID 0x22 = 529.41 MHz", did_khz(4500000, 0x22), 529411);
    expect_u("DID 0x21 = 545.45 MHz", did_khz(4500000, 0x21), 545454);
    expect_u("DID 0x41 = 272.72 MHz", did_khz(4500000, 0x41), 272727);
    expect_u("DID 0x3C = 300 MHz", did_khz(4500000, 0x3C), 300000);
    expect_u("DID 0x3D < 300 MHz", did_khz(4500000, 0x3D), 295081);
    expect_u("DID 0x40 = 281.25 MHz", did_khz(4500000, 0x40), 281250);
    expect_u("divider of 0x08", did_divider(0x08), 8);       expect_u("divider of 0x3F", did_divider(0x3F), 63);
    expect_u("divider of 0x40", did_divider(0x40), 64);      expect_u("divider of 0x5F", did_divider(0x5F), 126);
    expect_u("divider of 0x60", did_divider(0x60), 128);     expect_u("divider of 0x7D", did_divider(0x7D), 244);
    expect_u("divider of 0x7E", did_divider(0x7E), 248);     expect_u("divider of 0x7F", did_divider(0x7F), 512);
    expect_u("DID below 8 clamps to 8", did_divider(0x00), 8); expect_u("DID above 0x7F clamps", did_divider(0xFF), 512);
    expect_u("VCO from PLL_REQ 45", vco_khz(0x2d), 4500000);
    expect_u("VCO with a fraction (0x8000 = 0.5)", vco_khz(0x8000002d), 4550000);
    expect(dfs_did_ok(0x41) && !dfs_did_ok(0x07) && !dfs_did_ok(0x80) && !dfs_did_ok(0xFFFFFFFFu), "a DFS_CNTL value is a DID only in 8..0x7F");
    const Decoded d = decode_clocks(0x2d, 0x41, 0x41, 0x417f4141u);
    expect(d.ok && d.dentAgree && d.chgDone, "the E1 registers decode: readable, DENTIST agrees, CHG_DONE set");
    expect_u("E1 DISPCLK kHz", d.dispKhz, 272727); expect_u("E1 DPPCLK kHz", d.dppKhz, 272727); expect_u("E1 VCO", d.vcoKhz, 4500000);
    expect(!decode_clocks(0x2d, 0x41, 0x41, 0xFFFFFFFFu).ok, "an unreadable dword is not ok");
    expect(!decode_clocks(0x2d, 0x41, 0x0, 0x417f4141u).ok, "DID 0 is not ok");
    expect(!decode_clocks(0x0, 0x41, 0x41, 0x417f4141u).ok, "VCO 0 is not ok");
    expect(!decode_clocks(0x2d, 0x41, 0x41, 0x417f4140u).dentAgree && !decode_clocks(0x2d, 0x41, 0x41, 0x407f4141u).dentAgree, "DENTIST disagreeing on either divider is seen");
    expect(!decode_clocks(0x2d, 0x41, 0x41, 0x41174141u).chgDone && !decode_clocks(0x2d, 0x41, 0x41, 0x416f4141u).chgDone, "either CHG_DONE bit missing is seen");
    expect_u("level_count: 5 levels with flag bit 28 set", level_count(0x10000005u), 5); expect_u("level_count: bit 31 = fine-grained = 2 levels", level_count(0x80000000u), 2);
    expect_u("level_count: bit 31 wins over the low byte", level_count(0x80000009u), 2); expect_u("level_count: a zero low byte is 0 levels (the message failed)", level_count(0x10000100u), 0);
    expect_u("level_count: 0xFF", level_count(0xFFu), 255); expect(level_fine_grained(0x80000000u) && !level_fine_grained(0x10000005u), "fine-grained is bit 31 only");
    expect_u("level_mhz masks to 16 bits", level_mhz(0xABCD0258u), 0x258); expect_u("level_mhz of a plain MHz", level_mhz(540), 540);
    { const Decoded b = dec_did(0x41, 0x41);
      expect_u("restore_mask: raised clocks", restore_mask(1u << 6, b, dec_did(0x3C, 0x41)), 1u << 6);
      expect_u("restore_mask: a clock below its start is added", restore_mask(1u << 6, b, dec_did(0x3C, 0x42)), (1u << 6) | (1u << 7));
      expect_u("restore_mask: DISPCLK below start with nothing raised", restore_mask(0, b, dec_did(0x42, 0x41)), 1u << 6);
      expect_u("restore_mask: nothing raised, nothing low", restore_mask(0, b, dec_did(0x41, 0x41)), 0);
      expect_u("restore_mask: an unreadable reading adds nothing", restore_mask(0, b, decode_clocks(0x2d, 0xFFFFFFFFu, 0x41, 0x417f4141u)), 0);
      expect(at_start(b, dec_did(0x41, 0x41)) && !at_start(b, dec_did(0x40, 0x41)) && !at_start(b, dec_did(0x41, 0x40)), "at_start: both DIDs equal the start"); }
    expect_u("norm_mhz of 600", norm_mhz(600), 600);  expect_u("norm_mhz of 600000 kHz", norm_mhz(600000), 600);  expect_u("norm_mhz edge 19999", norm_mhz(19999), 19999);
    expect_u("norm_mhz edge 20000 is kHz", norm_mhz(20000), 20);
}

static void p2_allow() {
    // the three dwords and their disjointness
    expect_u("RESP dword", kRegResp, 0x16274); expect_u("ARG dword", kRegArg, 0x16273); expect_u("MSG dword", kRegMsg, 0x1628A);
    expect_u("RESP wait bound is 2 s", kRespWaitUs, 2000000); expect_u("PLL_REQ", kRegPllReq, 0x16E37); expect_u("DFS0", kRegDfs0, 0x16E69); expect_u("DFS1", kRegDfs1, 0x16E6C); expect_u("DENTIST", kRegDentist, 0x124);
    // every message id x level with a benign parameter: the exact allowed set
    for (uint32_t level = 0; level <= 6; level++) {
        for (uint32_t msg = 0; msg <= 0x40; msg++) {
            uint32_t param = 0;
            if (msg == kMsgGetDpmFreqByIndex) param = (6u << 16) | 0xFF;
            if (msg == kMsgGetDcModeMaxDpmFreq) param = 6u << 16;
            if (msg == kMsgSetHardMinByFreq) param = (6u << 16) | 300;
            const bool got = allow_check(level, msg, param, 600, 600) == kAllow;
            bool want = false;
            if (level >= 1 && level <= 4) {
                if (msg == 0x2 || msg == 0x3 || msg == 0x4 || msg == 0xB || msg == 0xC || msg == 0x15) want = true;
                if (msg == 0x9 && level >= 2) want = true;
            }
            if (got != want) { gFail++; std::printf("FAIL: allow_check(level %u, msg %#x) = %d, want %d\n", level, msg, got, want); }
            gRun++;
        }
    }
    // the refused message classes named by the design, at the top level
    for (uint32_t m : { 0x5u, 0x6u, 0x8u, 0xAu, 0xDu, 0xEu, 0x10u, 0x1Cu }) expect(allow_check(4, m, 0, 600, 600) == kRefMsg, "a message outside the allowlist is refused");
    // 0x9
    expect_u("clk 0 refused", allow_check(2, kMsgSetHardMinByFreq, hm_param(0, 300), 600, 600), kRefClk);
    expect_u("clk 5 refused", allow_check(2, kMsgSetHardMinByFreq, hm_param(5, 300), 600, 600), kRefClk);
    expect_u("clk 8 refused", allow_check(2, kMsgSetHardMinByFreq, hm_param(8, 300), 600, 600), kRefClk);
    expect_u("clk 6 allowed", allow_check(2, kMsgSetHardMinByFreq, hm_param(6, 300), 600, 600), kAllow);
    expect_u("clk 7 allowed", allow_check(3, kMsgSetHardMinByFreq, hm_param(7, 300), 600, 600), kAllow);
    expect_u("249 MHz refused", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 249), 600, 600), kRefLow);
    expect_u("250 MHz allowed", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 250), 600, 600), kAllow);
    expect_u("0 MHz refused", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 0), 600, 600), kRefLow);
    expect_u("601 MHz refused at level 4", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 601), 700, 700), kRefCap);
    expect_u("600 MHz allowed at level 4 (max 700)", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 600), 700, 700), kAllow);
    expect_u("321 MHz refused at level 2", allow_check(2, kMsgSetHardMinByFreq, hm_param(6, 321), 700, 700), kRefCap);
    expect_u("320 MHz allowed at level 2", allow_check(2, kMsgSetHardMinByFreq, hm_param(6, 320), 700, 700), kAllow);
    expect_u("321 MHz refused at level 3", allow_check(3, kMsgSetHardMinByFreq, hm_param(7, 321), 700, 700), kRefCap);
    expect_u("530 MHz refused at level 3 (the E4 value needs level 4)", allow_check(3, kMsgSetHardMinByFreq, hm_param(7, 530), 700, 700), kRefCap);
    expect_u("530 MHz allowed at level 4", allow_check(4, kMsgSetHardMinByFreq, hm_param(7, 530), 700, 700), kAllow);
    expect_u("0x9 at level 1 refused", allow_check(1, kMsgSetHardMinByFreq, hm_param(6, 300), 600, 600), kRefLevel);
    expect_u("above the E1b DISPCLK max refused", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 531), 530, 600), kRefAboveMax);
    expect_u("at the E1b DISPCLK max allowed", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 530), 530, 600), kAllow);
    expect_u("above the E1b DPPCLK max refused (DISPCLK max does not cover it)", allow_check(4, kMsgSetHardMinByFreq, hm_param(7, 531), 600, 530), kRefAboveMax);
    expect_u("E1b not run (max 0): 0x9 refused", allow_check(4, kMsgSetHardMinByFreq, hm_param(6, 300), 0, 600), kRefNoMax);
    expect_u("E1b not run for DPPCLK: refused", allow_check(4, kMsgSetHardMinByFreq, hm_param(7, 300), 600, 0), kRefNoMax);
    expect_u("extra parameter bits above the clk field refused", allow_check(4, kMsgSetHardMinByFreq, (1u << 24) | hm_param(6, 300), 600, 600), kRefClk);
    // queries
    expect_u("0x2 with a parameter refused", allow_check(1, kMsgGetSmuVersion, 1, 0, 0), kRefParam);
    expect_u("0x15 with a parameter refused", allow_check(2, kMsgReturnHardMinStatus, 1, 600, 600), kRefParam);
    expect_u("0xB idx 15 allowed", allow_check(1, kMsgGetDpmFreqByIndex, (7u << 16) | 15, 0, 0), kAllow);
    expect_u("0xB idx 16 refused", allow_check(1, kMsgGetDpmFreqByIndex, (7u << 16) | 16, 0, 0), kRefParam);
    expect_u("0xB idx 0xFF allowed", allow_check(1, kMsgGetDpmFreqByIndex, (6u << 16) | 0xFF, 0, 0), kAllow);
    expect_u("0xB idx 0xFE refused", allow_check(1, kMsgGetDpmFreqByIndex, (6u << 16) | 0xFE, 0, 0), kRefParam);
    expect_u("0xB clk 0 refused", allow_check(1, kMsgGetDpmFreqByIndex, (0u << 16) | 0xFF, 0, 0), kRefClk);
    expect_u("0xB clk 9 refused", allow_check(1, kMsgGetDpmFreqByIndex, (9u << 16) | 0xFF, 0, 0), kRefClk);
    expect_u("0xC clk 6 allowed", allow_check(1, kMsgGetDcModeMaxDpmFreq, 6u << 16, 0, 0), kAllow);
    expect_u("0xC clk 3 refused", allow_check(1, kMsgGetDcModeMaxDpmFreq, 3u << 16, 0, 0), kRefClk);
    expect_u("0xC with low bits refused", allow_check(1, kMsgGetDcModeMaxDpmFreq, (6u << 16) | 1, 0, 0), kRefParam);
    expect_u("level 0 refuses a query", allow_check(0, kMsgGetSmuVersion, 0, 0, 0), kRefLevel);
    expect_u("level 5 refuses a query", allow_check(5, kMsgGetSmuVersion, 0, 0, 0), kRefLevel);
    // the pre-send rule
    expect_u("RESP 0 refused before a send", resp_pre_check(0), kRefRespZero);
    expect_u("RESP all-ones refused", resp_pre_check(0xFFFFFFFFu), kRefRespZero);
    expect_u("RESP 1 passes", resp_pre_check(1), kAllow);
    expect_u("RESP 0xFC passes (Linux does not check the value)", resp_pre_check(0xFC), kAllow);
    expect(std::strcmp(resp_name(1), "OK") == 0 && std::strcmp(resp_name(0xFD), "RejectedPrereq") == 0, "reply names");
    expect_u("param_hardmin", param_hardmin(6, 272), (6u << 16) | 272);
}

static Decoded dec_did(uint32_t d0, uint32_t d1, bool agree, bool done) {
    uint32_t den = (d1 << 24) | 0x007F0000u | (d0 << 8) | d0;
    if (!agree) den ^= 1u;
    if (!done) den &= ~kDentistChgDone;
    return decode_clocks(0x2d, d0, d1, den);
}
static void p3_rules() {
    const Decoded base = dec_did(0x41, 0x41);
    const Plan e2 = plan_for(kE2), e3 = plan_for(kE3), e4 = plan_for(kE4);
    expect_u("E2 raises one clock", e2.nRaise, 1); expect_u("E2 clk", e2.raise[0].clk, 6); expect_u("E2 MHz", e2.raise[0].mhz, 300); expect_u("E2 dwell", e2.dwellMs, 10000);
    expect_u("E3 clk", e3.raise[0].clk, 7); expect_u("E3 MHz", e3.raise[0].mhz, 300); expect_u("E3 dwell", e3.dwellMs, 10000);
    expect_u("E4 raises two clocks", e4.nRaise, 2); expect_u("E4 DISPCLK first (Linux block order)", e4.raise[0].clk, 6); expect_u("E4 DPPCLK second", e4.raise[1].clk, 7);
    expect_u("E4 MHz a", e4.raise[0].mhz, 530); expect_u("E4 MHz b", e4.raise[1].mhz, 530); expect_u("E4 dwell", e4.dwellMs, 60000);
    expect_u("E4 DISPCLK threshold = DID 0x23 (514 MHz)", e4.dispKhzMin, 514000); expect_u("E4 DPPCLK threshold = DID 0x24 (500 MHz)", e4.dppKhzMin, 500000);
    expect(did_khz(4500000, 0x23) >= e4.dispKhzMin && did_khz(4500000, 0x24) < e4.dispKhzMin, "E4's DISPCLK threshold separates DID 0x23 from 0x24");
    expect(did_khz(4500000, 0x24) >= e4.dppKhzMin && did_khz(4500000, 0x25) < e4.dppKhzMin, "E4's DPPCLK threshold separates DID 0x24 from 0x25");
    expect(did_khz(4500000, 0x3C) >= e2.dispKhzMin && did_khz(4500000, 0x3D) < e2.dispKhzMin, "E2's threshold separates DID 0x3C from 0x3D");
    expect(e2.dispMoves && !e2.dppMoves && !e3.dispMoves && e3.dppMoves && e4.dispMoves && e4.dppMoves, "which clock may move in each step");
    expect_u("no plan for E1b", plan_for(kE1b).nRaise, 0);
    // judge_reading
    expect_u("E2: DISPCLK at DID 0x3C, DPPCLK unmoved -> PASS", judge_reading(e2, base, dec_did(0x3C, 0x41)), kPass);
    expect_u("E2: DISPCLK at DID 0x3D -> REFUSED (not reached)", judge_reading(e2, base, dec_did(0x3D, 0x41)), kRefused);
    expect_u("E2: DISPCLK unmoved -> REFUSED", judge_reading(e2, base, dec_did(0x41, 0x41)), kRefused);
    expect_u("E2: DISPCLK below its start -> REGRESSED", judge_reading(e2, base, dec_did(0x42, 0x41)), kRegressed);
    expect_u("E2: DPPCLK below its start -> REGRESSED", judge_reading(e2, base, dec_did(0x3C, 0x42)), kRegressed);
    expect_u("E2: DPPCLK moved up -> GLITCH", judge_reading(e2, base, dec_did(0x3C, 0x3C)), kGlitch);
    expect_u("E2: DENTIST disagrees -> GLITCH", judge_reading(e2, base, dec_did(0x3C, 0x41, false)), kGlitch);
    expect_u("E2: CHG_DONE missing -> GLITCH", judge_reading(e2, base, dec_did(0x3C, 0x41, true, false)), kGlitch);
    expect_u("E2: unreadable -> GLITCH", judge_reading(e2, base, decode_clocks(0x2d, 0xFFFFFFFFu, 0x41, 0x417f4141u)), kGlitch);
    expect_u("E3: DPPCLK raised, DISPCLK unmoved -> PASS", judge_reading(e3, base, dec_did(0x41, 0x3C)), kPass);
    expect_u("E3: DISPCLK moved -> GLITCH", judge_reading(e3, base, dec_did(0x3C, 0x3C)), kGlitch);
    expect_u("E4: 0x23 / 0x24 -> PASS", judge_reading(e4, base, dec_did(0x23, 0x24)), kPass);
    expect_u("E4: 0x21 / 0x21 -> PASS", judge_reading(e4, base, dec_did(0x21, 0x21)), kPass);
    expect_u("E4: DISPCLK 0x24 (500 < 514) -> REFUSED", judge_reading(e4, base, dec_did(0x24, 0x24)), kRefused);
    expect_u("E4: DPPCLK 0x25 (486 < 500) -> REFUSED", judge_reading(e4, base, dec_did(0x23, 0x25)), kRefused);
    // the dwell
    expect_u("dwell: back under the threshold -> REGRESSED", judge_dwell(e4, base, dec_did(0x2A, 0x24)), kRegressed);
    expect_u("dwell: DENTIST disagreement -> GLITCH", judge_dwell(e4, base, dec_did(0x23, 0x24, false)), kGlitch);
    expect_u("dwell: healthy -> PASS", judge_dwell(e2, base, dec_did(0x3C, 0x41)), kPass);
    // restore / baseline
    expect(restored_ok(base, dec_did(0x41, 0x41)) && restored_ok(base, dec_did(0x40, 0x41)) && !restored_ok(base, dec_did(0x42, 0x41)) && !restored_ok(base, dec_did(0x41, 0x42)), "restored_ok: at least the start value, both clocks");
    expect(baseline_ok(base) && !baseline_ok(dec_did(0x41, 0x41, false)) && !baseline_ok(dec_did(0x41, 0x41, true, false)) && !baseline_ok(dec_did(0x7F, 0x41)) && !baseline_ok(dec_did(0x08, 0x41)),
           "baseline_ok: readable, agreeing, CHG_DONE, both clocks in [250, 700] MHz");
    // E1b
    expect_u("E1b: 2 levels, max 540 -> PASS", judge_e1b(2, 540), kPass);
    expect_u("E1b: max 539 -> SHORT", judge_e1b(2, 539), kShort);
    expect_u("E1b: one level -> SHORT", judge_e1b(1, 600), kShort);
    expect(e1b_enables(2, 600, 600) && !e1b_enables(1, 600, 600) && !e1b_enables(2, 0, 600) && !e1b_enables(2, 600, 0), "what enables E2..E4 after E1b");
    // latch + pre-gate
    Latch l { false, 0, 0 };
    for (uint32_t v : { (uint32_t)kPass, (uint32_t)kDenied, (uint32_t)kShort, (uint32_t)kVNone }) { l = latch_apply(l, kE2, v); expect(!l.stopped, "PASS / DENIED / SHORT do not latch"); }
    for (uint32_t v : { (uint32_t)kRefused, (uint32_t)kTimeout, (uint32_t)kRegressed, (uint32_t)kGlitch }) { Latch t = latch_apply(Latch{ false, 0, 0 }, kE3, v); expect(t.stopped && t.firstVerdict == v && t.firstStep == kE3, "each stop verdict latches"); }
    l = latch_apply(l, kE3, kRefused); l = latch_apply(l, kE4, kGlitch); l = latch_apply(l, kE4, kPass);
    expect(l.stopped && l.firstVerdict == kRefused && l.firstStep == kE3, "the latch is sticky and keeps the first stop");
    for (uint32_t s = 1; s <= 4; s++) expect_u("a latched boot denies every step", pre_gate(true, 4, s, true, true, false), kDenyLatched);
    expect_u("bad step 0", pre_gate(true, 4, 0, true, false, false), kDenyBadStep);  expect_u("bad step 5", pre_gate(true, 4, 5, true, false, false), kDenyBadStep);
    expect_u("gate off", pre_gate(false, 4, 1, true, false, false), kDenyGate);
    expect_u("level 0", pre_gate(true, 0, 1, true, false, false), kDenyLevelArg);   expect_u("level 5", pre_gate(true, 5, 1, true, false, false), kDenyLevelArg);
    expect_u("E2 needs level 2", pre_gate(true, 1, kE2, true, false, false), kDenyLevel); expect_u("E3 needs level 3", pre_gate(true, 2, kE3, true, false, false), kDenyLevel);
    expect_u("E4 needs level 4", pre_gate(true, 3, kE4, true, false, false), kDenyLevel);
    expect_u("E1b at level 1", pre_gate(true, 1, kE1b, false, false, false), kDenyNone);
    expect_u("E2 without E1b", pre_gate(true, 4, kE2, false, false, false), kDenyNoE1b); expect_u("busy", pre_gate(true, 4, kE1b, false, false, true), kDenyBusy);
    expect_u("E4 at level 4 after E1b", pre_gate(true, 4, kE4, true, false, false), kDenyNone);
    expect_u("a higher level allows a lower step", pre_gate(true, 4, kE2, true, false, false), kDenyNone);
    // frames
    expect_u("frame_delta simple", frame_delta(10, 25), 15); expect_u("frame_delta wraps at 24 bits", frame_delta(0xFFFFFE, 3), 5);
    expect(frames_stalled(5, 5, 250) && !frames_stalled(5, 5, 249) && !frames_stalled(5, 6, 1000), "stall: no advance over >= 250 ms");
    expect(rate_ok(120, 2000, 600, 10000) && rate_ok(120, 2000, 594, 10000) && !rate_ok(120, 2000, 587, 10000) && rate_ok(120, 2000, 606, 10000) && !rate_ok(120, 2000, 613, 10000), "rate +-2 %");
    expect(!rate_ok(0, 2000, 600, 10000) && !rate_ok(120, 0, 600, 10000) && !rate_ok(120, 2000, 600, 0), "rate: zero reference or duration is not ok");
}
// =====================================================================================================================================
// B: the real code against the simulated machine. SCEN(name) { ... } runs its block in a forked child: the latch, the recorded DPM maxima and the busy
// flag are per-boot statics of the real code, so every scenario needs a fresh process.
// =====================================================================================================================================
static bool scen_enter(const char *name) {
    std::fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) { gFail = 0; gRun = 0; alarm(40); return true; }   // a scenario that hangs (an unbounded wait planted into the real code) is killed by SIGALRM and fails
    int st = 0; waitpid(pid, &st, 0);
    const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    gRun++;
    if (!ok) { gFail++; std::printf("FAIL: scenario \"%s\" did not pass (exit %d, signal %d)\n", name, WIFEXITED(st) ? WEXITSTATUS(st) : -1, WIFSIGNALED(st) ? WTERMSIG(st) : 0); }
    return false;
}
static void scen_exit() { std::fflush(stdout); _exit(gFail ? 1 : 0); }
#define SCEN(name) if (scen_enter(name)) for (bool scen_once = true; scen_once; scen_once = false, scen_exit())

static std::vector<std::pair<uint32_t, uint32_t>> e1b_expected() {
    std::vector<std::pair<uint32_t, uint32_t>> v = { { 2, 0 }, { 3, 0 }, { 4, 0 } };
    for (uint32_t clk = 6; clk <= 7; clk++) {
        v.push_back({ 0xB, (clk << 16) | 0xFF });
        for (uint32_t i = 0; i < 5; i++) v.push_back({ 0xB, (clk << 16) | i });
        if (clk == 6) v.push_back({ 0xC, clk << 16 });   // DISPCLK only: Linux never asks DPPCLK
    }
    return v;
}
static void check_no_stray(const char *what) {   // every message the PMFW saw is on the allowlist (independent of the sender's own check)
    for (auto &m : W.seen) {
        const bool ok = m.first == 2 || m.first == 3 || m.first == 4 || m.first == 0xB || m.first == 0xC || m.first == 0x15 ||
                        (m.first == 9 && ((m.second >> 16) == 6 || (m.second >> 16) == 7) && (m.second & 0xFFFF) >= 250 && (m.second & 0xFFFF) <= 600);
        if (!ok) { gFail++; std::printf("FAIL: %s: the PMFW received message %#x param %#x, which is not on the allowlist\n", what, m.first, m.second); }
        gRun++;
    }
}
static bool regs_untouched() { return W.reg[kRegMsg] == 0 && W.reg[kRegArg] == 0 && W.reg[kRegResp] == kRespOk; }
static const kern_return_t kNP = (kern_return_t)kIOReturnNotPermitted;

static void b1_sender() {
    amdgpu::DalMsg m;
    kern_return_t rc = 0;
    // -- the gate: nothing without the boot-arg, nothing off a native boot, nothing without S1b POSITIVE PASS, nothing at a bad level
    SCEN("no boot-arg") {
        world_reset(0, /*bootArg*/ false);
        rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m);
        expect(rc == kNP && m.refuse == kRefLevel, "no boot-arg: refused (level 0)");
        expect(W.seen.empty() && regs_untouched(), "no boot-arg: nothing written, the PMFW saw nothing");
    }
    SCEN("boot-arg 0") { world_reset(0); rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == kNP && m.refuse == kRefLevel && W.seen.empty(), "boot-arg 0: refused"); }
    SCEN("boot-arg 5") { world_reset(5); rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == kNP && W.seen.empty() && regs_untouched(), "boot-arg 5 (out of range): refused"); }
    SCEN("gate off") { world_reset(4); gS1b.gate = n48native::kGateOff; rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == kNP && W.seen.empty() && regs_untouched(), "not a native boot: refused at boot-arg 4"); }
    SCEN("gate refused") { world_reset(4); gS1b.gate = n48native::kGateRefused; rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == kNP && W.seen.empty(), "native gate refused: nothing sent"); }
    SCEN("no positive pass") { world_reset(4); gS1b.positivePass = false; rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == kNP && W.seen.empty() && regs_untouched(), "S1b not POSITIVE PASS: refused at boot-arg 4"); }
    // -- a good send: the reply, the elapsed time, one log line
    SCEN("good send") {
        world_reset(1); rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m);
        expect(rc == 0 && m.resp == kRespOk && m.refuse == 0 && m.preResp == kRespOk && m.arg == W.smuVersion && W.seen.size() == 1 && W.seen[0].first == 2, "a query at level 1 is sent and answered");
        expect(m.us >= 1000 && m.us < kRespWaitUs, "the elapsed time is the reply time");
        expect(!gLog.empty() && gLog.back().find("msg=0x02") != std::string::npos && gLog.back().find("resp=0x1 (OK)") != std::string::npos && gLog.back().find("us=") != std::string::npos && gLog.back().find("preresp=0x1") != std::string::npos,
               "one log line: msg, param, pre-RESP, RESP name, ARG, elapsed us");
    }
    // -- refusals write NOTHING (planted: clk 0, 601 and 249 MHz, 0x5, 0x9 at level 1, above the max)
    SCEN("refusals write nothing") {
        world_reset(4); (void)step(kE1b);                       // records max 600 / 600
        const size_t seenAfterE1b = W.seen.size();
        struct Case { uint32_t msg, param; const char *what; };
        const Case bad[] = { { 9, hm_param(0, 300), "clk 0" }, { 9, hm_param(6, 601), "601 MHz" }, { 9, hm_param(6, 249), "249 MHz" }, { 0x5, 0, "message 0x5" },
                             { 0x6, 0, "message 0x6" }, { 0x8, 0, "message 0x8" }, { 0xA, 0, "message 0xA" }, { 0xD, 0, "message 0xD" }, { 9, hm_param(7, 700), "700 MHz" },
                             { 0xB, (6u << 16) | 16, "index 16" }, { 0xC, (3u << 16), "0xC clk 3" }, { 0x2, 7, "0x2 with a parameter" } };
        for (const Case &c : bad) {
            const uint32_t before = W.reg[kRegArg];
            rc = amdgpu::smu_dal_send(gDev, c.msg, c.param, &m);
            char buf[96]; std::snprintf(buf, sizeof(buf), "%s is refused and nothing is written", c.what);
            expect(rc == kNP && m.refuse != 0 && W.seen.size() == seenAfterE1b && W.reg[kRegMsg] == 0 && W.reg[kRegArg] == before && W.reg[kRegResp] == kRespOk, buf);
        }
    }
    SCEN("0x9 at level 1") {
        world_reset(1); (void)step(kE1b);
        const size_t n = W.seen.size(); rc = amdgpu::smu_dal_send(gDev, 9, hm_param(6, 300), &m);
        expect(rc != 0 && m.refuse == kRefLevel && W.seen.size() == n && W.reg[kRegMsg] == 0, "0x9 at level 1 is refused");
    }
    SCEN("above the E1b max") {   // E1b recorded 530 for DISPCLK, level 4
        world_reset(4); W.dpm[0] = { 273, 400, 530 }; (void)step(kE1b);
        const size_t n = W.seen.size(); rc = amdgpu::smu_dal_send(gDev, 9, hm_param(6, 531), &m);
        expect(rc != 0 && m.refuse == kRefAboveMax && W.seen.size() == n && W.reg[kRegMsg] == 0, "a request above the E1b max is refused, nothing written");
    }
    SCEN("0x9 without E1b") { world_reset(4); rc = amdgpu::smu_dal_send(gDev, 9, hm_param(6, 300), &m); expect(rc != 0 && m.refuse == kRefNoMax && W.seen.empty() && regs_untouched(), "0x9 before E1b: refused (no recorded max)"); }
    // -- RESP == 0 before the send: refused, nothing written
    SCEN("RESP zero before the send") {
        world_reset(4); W.reg[kRegResp] = 0; W.reg[kRegArg] = 0x1234;
        rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m);
        expect(rc == (kern_return_t)kIOReturnNotReady && m.refuse == kRefRespZero && m.preResp == 0 && W.seen.empty() && W.reg[kRegMsg] == 0 && W.reg[kRegArg] == 0x1234 && W.reg[kRegResp] == 0,
               "RESP == 0 before the send: refused, MSG / ARG / RESP untouched");
    }
    // -- a timeout: bounded at 2 s (Linux's bound)
    SCEN("timeout") {
        world_reset(4); W.rules.push_back({ 2, 0, true, 0 });
        const uint64_t t0 = gNowNs; rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); const uint64_t dt = (gNowNs - t0) / 1000000ull;
        expect(rc == (kern_return_t)kIOReturnTimeout && m.resp == 0 && dt >= 2000 && dt <= 2020, "no reply: Timeout after ~2 s of IOSleep");
        expect(m.us >= 2000000 && m.us < 2020000, "the elapsed time of a timeout");
    }
    // -- every reply code maps
    const struct { uint32_t resp; kern_return_t rc; } codes[] = { { 0xFF, kIOReturnError }, { 0xFE, kIOReturnUnsupported }, { 0xFD, kIOReturnNotReady }, { 0xFC, kIOReturnBusy }, { 0x77, kIOReturnInternalError } };
    for (auto &c : codes) SCEN("reply code") { world_reset(4); W.rules.push_back({ 2, 0, false, c.resp }); rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == c.rc && m.resp == c.resp, "a reply code maps to its IOReturn"); }
    // -- the mailbox dwords outside the BAR
    SCEN("mailbox outside the BAR") {
        world_reset(4); gDev.rmmioSize = 0x16000u * 4u; rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m);
        expect(rc == (kern_return_t)kIOReturnNoDevice && m.refuse == kRefNoDevice && W.seen.empty(), "mailbox dwords outside the mapping: refused");
    }
    SCEN("log width") { world_reset(4); (void)step(kE1b); expect(gLongLine == 0, "no log line reaches 512 bytes"); }
}

static void b2_e1b() {
    SCEN("E1b pass") {
        world_reset(1);
        const n48n_dal_result &r = step(kE1b);
        expect_verdict("E1b PASS", r, kPass);
        expect(W.seen == e1b_expected(), "E1b sends exactly the message list of the design (0x2 0x3 0x4, then per clock 0xB 0xFF and each level, and 0xC for DISPCLK ONLY)");
        expect_u("nmsg", r.nmsg, e1b_expected().size());
        expect_u("smu version", r.smu_version, 0x00500A00u); expect_u("if version", r.if_version, 1); expect_u("header version", r.header_version, 0x2E);
        expect(r.dpm_count[0] == 5 && r.dpm_count[1] == 5 && r.dpm_max_mhz[0] == 600 && r.dpm_max_mhz[1] == 600 && r.dc_max_mhz[0] == 600 && r.dc_max_mhz[1] == 0 && r.dc_max_resp == 1, "the tables and maxima are returned (DISPCLK's 0xC only)");
        expect(r.dpm_mhz[0][0] == 273 && r.dpm_mhz[0][3] == 540 && r.dpm_mhz[1][4] == 600, "each level is returned in MHz");
        expect((r.flags & N48N_DAL_F_E1B_DONE) != 0 && (r.flags & N48N_DAL_F_UNDERFLOW_UNREAD) != 0 && (r.flags & N48N_DAL_F_VUPDATE_UNCOUNTED) != 0 && (r.flags & (N48N_DAL_F_LATCHED | N48N_DAL_F_LOW_MAX | N48N_DAL_F_ODM)) == 0,
               "flags: E1B_DONE, underflow / VUPDATE not read, not latched, not low");
        expect_u("underflow is reported as not read", r.underflow, N48N_DAL_UNDERFLOW_NA);
        expect(r.khz_before[0] == 272727 && r.khz_before[1] == 272727 && r.vco_khz == 4500000 && r.pll_req == 0x2d && r.dfs_before[0] == 0x41 && r.dentist_before == 0x417f4141u, "the baseline clocks are returned, computed with the C1 table");
        expect(r.latched == 0 && r.level == 1 && r.rc == 0 && r.fail_msg == 0 && r.step == 1, "level, no failure, step");
        for (auto &m : W.seen) if (m.first == 9) { gFail++; std::printf("FAIL: E1b sent a 0x9\n"); }
        const size_t n = W.seen.size(); const n48n_dal_result &r2 = step(kE1b); expect_verdict("E1b again", r2, kPass); expect_u("E1b again sends the same list", W.seen.size() - n, e1b_expected().size());
        check_no_stray("E1b");
        expect(gLongLine == 0, "no log line reaches 512 bytes");
    }
    SCEN("max 500") {
        world_reset(1); W.dpm[0] = { 273, 400, 500 };
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b max 500 -> SHORT", q, kShort);
        expect((q.flags & N48N_DAL_F_LOW_MAX) != 0 && (q.flags & N48N_DAL_F_ODM) != 0 && q.latched == 0, "SHORT (max 500): LOW_MAX and ODM flags, no latch");
        expect_u("E1b max 500 still enables E2 (the allowlist bounds by 500)", q.flags & N48N_DAL_F_E1B_DONE, N48N_DAL_F_E1B_DONE);
    }
    SCEN("max 530") { world_reset(1); W.dpm[0] = { 273, 400, 530 }; const n48n_dal_result &q = step(kE1b); expect_verdict("E1b max 530 -> SHORT", q, kShort); expect((q.flags & N48N_DAL_F_LOW_MAX) != 0 && (q.flags & N48N_DAL_F_ODM) == 0, "max 530: LOW_MAX but not ODM"); }
    SCEN("max 540") { world_reset(1); W.dpm[0] = { 273, 400, 540 }; const n48n_dal_result &q = step(kE1b); expect_verdict("E1b max 540 -> PASS", q, kPass); }
    SCEN("one level") {
        world_reset(2); W.dpm[0] = { 273 };
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b one level -> SHORT", q, kShort); expect((q.flags & N48N_DAL_F_E1B_DONE) == 0, "one level: E1b is not done");
        const n48n_dal_result &q2 = step(kE2); expect_verdict("E2 after a one-level E1b", q2, kDenied); expect_u("... denied for the missing E1b", q2.deny, kDenyNoE1b);
    }
    SCEN("kHz replies") {
        world_reset(1); W.kHzReplies = true;
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b with kHz replies", q, kPass);
        expect(q.dpm_max_mhz[0] == 600 && q.dpm_mhz[0][1] == 300 && q.dc_max_mhz[0] == 600 && q.dc_max_resp == 1 && (q.flags & N48N_DAL_F_UNIT_KHZ_SEEN) != 0, "a kHz GetDcModeMaxDpmFreq reply is read as MHz and flagged");
        expect(q.dpm_count[0] == 5 && q.dpm_mhz[0][3] == 540 && q.dc_max_mhz[1] == 0, "levels come from the low 16 bits, the count from the low 8 bits under flag bit 28; DPPCLK's 0xC is never asked");
    }
    SCEN("fine-grained") {
        world_reset(1); W.dpm[0] = { 273, 600 }; W.fineGrained[0] = true;
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b: a fine-grained DISPCLK (bit 31) has 2 levels", q, kPass);
        expect(q.dpm_count[0] == 2 && q.dpm_max_mhz[0] == 600 && q.dpm_mhz[0][0] == 273 && q.dpm_mhz[0][1] == 600 && (q.flags & N48N_DAL_F_FINE_GRAINED) != 0, "fine-grained: min and max, flagged");
    }
    SCEN("dc max non-OK") {
        world_reset(1); W.rules.push_back({ 0xC, 0, false, 0xFD });
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b: a non-OK GetDcModeMaxDpmFreq reply does not stop the step", q, kPass);
        expect(q.dc_max_resp == 0xFD && q.latched == 0 && q.rc == 0 && (q.flags & N48N_DAL_F_E1B_DONE) != 0 && q.dpm_max_mhz[0] == 600, "recorded (dc_max_resp), not latched, E1b done");
    }
    SCEN("dc max hangs") {
        world_reset(1); W.rules.push_back({ 0xC, 0, true, 0 });
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b: a GetDcModeMaxDpmFreq that never answers is a mailbox TIMEOUT", q, kTimeout); expect(q.latched == 1, "latched");
    }
    SCEN("0xFD on a level query") {
        world_reset(1); W.rules.push_back({ 0xB, 3, false, 0xFD });
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b 0xFD -> REFUSED", q, kRefused);
        expect(q.latched == 1 && (q.flags & N48N_DAL_F_LATCHED) && q.fail_msg == 0xB && q.rc == (uint32_t)kIOReturnNotReady && (q.flags & N48N_DAL_F_E1B_DONE) == 0, "REFUSED latches, names the message, E1b not done");
        const n48n_dal_result &q2 = step(kE1b); expect_verdict("after a stop even E1b is denied", q2, kDenied); expect_u("... because of the latch", q2.deny, kDenyLatched);
    }
    SCEN("hang") {
        world_reset(1); W.rules.push_back({ 2, 0, true, 0 });
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b hang -> TIMEOUT", q, kTimeout); expect(q.latched == 1 && q.nmsg == 1, "TIMEOUT latches, no further message"); expect_u("only the first message was sent", W.seen.size(), 1);
    }
    SCEN("17 levels") {
        world_reset(1); W.dpm[0] = { 273 }; W.nLevelsOverride[0] = 17;
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b with 17 levels -> GLITCH", q, kGlitch); expect(q.latched == 1, "GLITCH latches");
    }
    SCEN("a query moves a clock") {
        world_reset(1); W.moveClockOnMsg = 0xC;
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b: a clock moved during the queries -> GLITCH", q, kGlitch); expect(q.latched == 1 && (q.flags & N48N_DAL_F_E1B_DONE) == 0, "GLITCH latches, E1b not done");
    }
    SCEN("zero levels") { world_reset(1); W.rules.push_back({ 0xB, 1, false, 1 }); const n48n_dal_result &q = step(kE1b); expect_verdict("E1b: zero levels -> GLITCH", q, kGlitch); }
    SCEN("unreadable clock registers") {
        world_reset(1); W.pll = 0xFFFFFFFFu; sync_regs();
        const n48n_dal_result &q = step(kE1b); expect_verdict("unreadable clock registers -> DENIED", q, kDenied); expect(q.nmsg == 0 && W.seen.empty() && q.latched == 0, "nothing sent, no latch");
    }
    SCEN("no boot-arg") { world_reset(0, false); const n48n_dal_result &q = step(kE1b); expect_verdict("no boot-arg: E1b DENIED", q, kDenied); expect_u("... level arg", q.deny, kDenyLevelArg); expect(W.seen.empty() && regs_untouched() && q.latched == 0, "nothing sent"); }
    SCEN("no positive pass") { world_reset(3); gS1b.positivePass = false; const n48n_dal_result &q = step(kE1b); expect_verdict("no S1b POSITIVE PASS: DENIED", q, kDenied); expect_u("... gate", q.deny, kDenyGate); expect(W.seen.empty(), "nothing sent"); }
    SCEN("gate off") { world_reset(3); gS1b.gate = n48native::kGateOff; const n48n_dal_result &q = step(kE1b); expect_verdict("not a native boot: DENIED", q, kDenied); expect(W.seen.empty(), "nothing sent"); }
    SCEN("malformed") {
        world_reset(4);
        expect_u("a malformed step is BadArgument", (uint32_t)amdgpu::dal_run_step(gDev, 0, &gRes), (uint32_t)kIOReturnBadArgument);
        expect_u("a malformed step 5 is BadArgument", (uint32_t)amdgpu::dal_run_step(gDev, 5, &gRes), (uint32_t)kIOReturnBadArgument);
        expect_u("a null result is BadArgument", (uint32_t)amdgpu::dal_run_step(gDev, 1, nullptr), (uint32_t)kIOReturnBadArgument);
    }
}

// The messages a raise sequence sent, without the 0x15 polls.
static std::vector<std::pair<uint32_t, uint32_t>> hardmins() { std::vector<std::pair<uint32_t, uint32_t>> v; for (auto &m : W.seen) if (m.first == 9) v.push_back(m); return v; }
static void ready_at(uint32_t level) { world_reset(level); const n48n_dal_result &q = step(kE1b); expect_verdict("(setup) E1b", q, kPass); W.seen.clear(); W.seenCount.clear(); gLog.clear(); }

static void b3_e2() {
    SCEN("E2 pass") {
        ready_at(2);
        const n48n_dal_result &r = step(kE2);
        expect_verdict("E2 PASS", r, kPass);
        const auto hm = hardmins();
        expect(hm.size() == 2 && hm[0].second == hm_param(6, 300) && hm[1].second == hm_param(6, 272), "E2: SetHardMin (6<<16)|300, then the restore (6<<16)|272, nothing for DPPCLK");
        expect(r.khz_after[0] >= 300000 && r.khz_after[1] == 272727 && r.khz_before[0] == 272727 && r.khz_restored[0] >= 272727 && r.khz_restored[1] >= 272727, "E2: DISPCLK raised to >= 300 MHz, DPPCLK unmoved, both back");
        expect(r.dfs_after[0] <= 0x3C && r.dfs_after[1] == 0x41 && r.dwell_ms >= 10000 && r.dwell_ms <= 10300 && r.frame_after - r.frame_before >= 595 && r.frame_after - r.frame_before <= 605, "E2: DID <= 0x3C, a 10 s dwell, ~600 frames");
        expect((r.flags & N48N_DAL_F_RESTORED) != 0 && (r.flags & N48N_DAL_F_RESTORE_FAILED) == 0 && r.latched == 0 && r.target_mhz[0] == 300 && r.target_mhz[1] == 0, "E2: restored, not latched, targets");
        expect(r.hardmin_poll_us > 0 && r.grant_reply[0] == 300, "E2: the 0x15 poll time and the grant reply are returned");
        expect((r.flags & N48N_DAL_F_AT_START) != 0 && (r.flags & N48N_DAL_F_ABOVE_START) == 0 && r.khz_restored[0] == 272727 && r.khz_restored[1] == 272727, "E2 (0.0.606, F1): the 272 MHz release lands AT the start (DID 0x41): AT_START, not ABOVE_START");
        expect(r.dentist_after != 0 && (r.dentist_after & kDentistChgDone) == kDentistChgDone, "E2: DENTIST after carries CHG_DONE");
        check_no_stray("E2");
        size_t n15 = 0; for (auto &m : W.seen) if (m.first == 0x15) n15++;
        expect(n15 >= 6, "E2: ReturnHardMinStatus was polled until it reported done after each hard-min (the model needs 3 polls each)");
        const n48n_dal_result &r2 = step(kE2); expect_verdict("E2 again", r2, kPass);
        const n48n_dal_result &r3 = step(kE3); expect_verdict("E3 at level 2", r3, kDenied); expect_u("... level", r3.deny, kDenyLevel);
        expect(gLongLine == 0, "no log line reaches 512 bytes");
    }
    SCEN("E3 other clock regresses") {
        ready_at(3); W.otherBelowOnRaise = true;
        const n48n_dal_result &r = step(kE3); expect_verdict("DISPCLK regresses in E3", r, kRegressed);
        const auto h = hardmins();
        expect(h.size() == 3 && h[0].second == hm_param(7, 300) && h[1].second == hm_param(7, 272) && h[2].second == hm_param(6, 272), "E3: the restore covers DPPCLK (raised) and DISPCLK (below its start)");
    }
    SCEN("E3 pass") {
        ready_at(3);
        const n48n_dal_result &r3 = step(kE3); expect_verdict("E3 PASS", r3, kPass);
        const auto h3 = hardmins();
        expect(h3.size() == 2 && h3[0].second == hm_param(7, 300) && h3[1].second == hm_param(7, 272), "E3: DPPCLK only");
        expect(r3.khz_after[1] >= 300000 && r3.khz_after[0] == 272727, "E3: DPPCLK raised, DISPCLK unmoved"); check_no_stray("E3");
        const n48n_dal_result &r4 = step(kE4); expect_verdict("E4 at level 3", r4, kDenied); expect_u("... level", r4.deny, kDenyLevel);
    }
    SCEN("E4 pass") {
        ready_at(4);
        const n48n_dal_result &r4 = step(kE4); expect_verdict("E4 PASS", r4, kPass);
        const auto h4 = hardmins();
        expect(h4.size() == 4 && h4[0].second == hm_param(6, 530) && h4[1].second == hm_param(7, 530) && h4[2].second == hm_param(7, 272) && h4[3].second == hm_param(6, 272),
               "E4: DISPCLK first, then DPPCLK, restore in reverse (DPPCLK, DISPCLK)");
        expect(r4.khz_after[0] >= 514000 && r4.khz_after[1] >= 500000 && r4.dfs_after[0] <= 0x23 && r4.dfs_after[1] <= 0x24, "E4: DISPCLK DID <= 0x23, DPPCLK DID <= 0x24");
        expect(r4.dwell_ms >= 60000 && r4.dwell_ms <= 60500 && r4.frame_after - r4.frame_before >= 3595 && r4.frame_after - r4.frame_before <= 3605, "E4: a 60 s dwell");
        expect((r4.flags & N48N_DAL_F_RESTORED) != 0 && r4.target_mhz[0] == 530 && r4.target_mhz[1] == 530 && r4.latched == 0, "E4: restored");
        check_no_stray("E4");
        expect(gLongLine == 0, "no log line reaches 512 bytes");
    }
    SCEN("E2 restore at the start") {
        ready_at(2); W.restoreDidOverride = true; W.restoreDid = 0x41;
        const n48n_dal_result &r = step(kE2); expect_verdict("E2 restore back to DID 0x41", r, kPass);
        expect((r.flags & N48N_DAL_F_AT_START) != 0 && (r.flags & N48N_DAL_F_ABOVE_START) == 0 && (r.flags & N48N_DAL_F_RESTORED) != 0, "E2: AT_START (restored to the start value)");
    }
    SCEN("E2 restore lands above the start") {   // 0.0.606: the ABOVE_START branch stays covered by forcing the DID the old 273 request produced
        ready_at(2); W.restoreDidOverride = true; W.restoreDid = 0x40;
        const n48n_dal_result &r = step(kE2); expect_verdict("E2 restore forced to DID 0x40", r, kPass);
        expect((r.flags & N48N_DAL_F_ABOVE_START) != 0 && (r.flags & N48N_DAL_F_AT_START) == 0 && (r.flags & N48N_DAL_F_RESTORED) != 0 && r.khz_restored[0] == 281250, "E2: ABOVE_START at 281.25 MHz when a floor holds it up");
    }
    SCEN("F1 arithmetic") {   // the restore target is the FLOOR of the boot clock: 272 is at or below 272.727 MHz (DID 0x41), 273 is above it (DID 0x40)
        world_reset(2);
        expect_u("kRestoreMhz is 272", kRestoreMhz, 272);
        expect(did_for_level(kRestoreMhz) == 0x41 && did_for_level(273) == 0x40, "the model: a 272 MHz floor lands on DID 0x41, a 273 MHz floor on DID 0x40 (281.25 MHz)");
        expect(did_khz(vco_khz(W.pll), 0x41) <= kRestoreMhz * 1000u + 727u && did_khz(vco_khz(W.pll), 0x41) > kRestoreMhz * 1000u && did_khz(vco_khz(W.pll), 0x40) > 273u * 1000u, "272727 kHz is in (272, 273) MHz: 273 rounds UP past it");
    }
    SCEN("plane acquired") {
        ready_at(2); W.acquired = true; sync_regs();
        const n48n_dal_result &r = step(kE2); expect_verdict("E2 while the scanout plane is acquired", r, kDenied);
        expect(r.deny == kDenyAcquired && r.latched == 0 && W.seen.empty() && r.nmsg == 0, "DENIED (acquired): nothing sent, no latch");
    }
    SCEN("E2 at level 4") { ready_at(4); const n48n_dal_result &r2 = step(kE2); expect_verdict("E2 at level 4 (levels are cumulative)", r2, kPass); }
}

// One failure scenario: run E2 (level 2) after E1b with a knob set by `setup`, return the result.
template <class F> static const n48n_dal_result &fail_e2(F setup) { ready_at(2); setup(); sync_regs(); return step(kE2); }
static void b3_failures() {
    SCEN("0xFD on the raise") {
        const auto &r = fail_e2([] { W.rules.push_back({ 9, 1, false, 0xFD }); });
        expect_verdict("SetHardMin -> 0xFD", r, kRefused);
        expect(r.latched == 1 && r.fail_msg == 9 && r.rc == (uint32_t)kIOReturnNotReady && hardmins().size() == 1 && (r.flags & N48N_DAL_F_RESTORED) == 0 && (r.flags & N48N_DAL_F_RESTORE_FAILED) == 0, "REFUSED before any raise: latched, no restore, none needed");
        const auto &r2 = step(kE2); expect_verdict("E2 again after a stop", r2, kDenied); expect_u("... latched", r2.deny, kDenyLatched); expect_u("... E3", step(kE3).deny, kDenyLatched);
    }
    SCEN("0xFC on the raise") { const auto &r = fail_e2([] { W.rules.push_back({ 9, 1, false, 0xFC }); }); expect_verdict("SetHardMin -> 0xFC", r, kRefused); expect(r.latched == 1, "0xFC latches"); }
    SCEN("DFS does not follow") {
        const auto &r = fail_e2([] { W.follow[0] = false; });
        expect_verdict("OK but the DFS does not follow", r, kRefused);
        expect(r.latched == 1 && hardmins().size() == 2 && hardmins()[1].second == hm_param(6, 272) && (r.flags & N48N_DAL_F_RESTORED) != 0, "REFUSED (no effect): the restore ran and verified");
    }
    SCEN("regress") {
        const auto &r = fail_e2([] { W.regressOnRaise = true; });
        expect_verdict("a clock lands below its start", r, kRegressed); expect(r.latched == 1 && hardmins().size() == 2, "REGRESSED latches and restores");
    }
    SCEN("DPPCLK moves") {
        const auto &r = fail_e2([] { W.dppMovesWithDisp = true; });
        expect_verdict("DPPCLK moves in E2", r, kGlitch); expect(r.latched == 1 && hardmins().size() == 2 && (r.flags & N48N_DAL_F_RESTORED) != 0, "GLITCH latches and restores");
    }
    SCEN("DENTIST stale") { const auto &r = fail_e2([] { W.dentistFrozen = W.reg[kRegDentist]; W.dentistStale = true; }); expect_verdict("DENTIST does not follow", r, kGlitch); expect(r.latched == 1, "DENTIST disagreement latches"); }
    SCEN("CHG_DONE clear") { const auto &r = fail_e2([] { W.chgDoneClearOnRaise = true; }); expect_verdict("CHG_DONE missing after the raise", r, kGlitch); expect(r.latched == 1, "CHG_DONE missing latches"); }
    SCEN("0x15 never done") {   // a POLL timeout: the mailbox answers every poll, so the restore is still attempted
        const auto &r = fail_e2([] { W.hm15Never = true; });
        expect_verdict("ReturnHardMinStatus never reports done", r, kTimeout);
        expect(r.latched == 1 && r.fail_msg == 0x15 && (r.flags & N48N_DAL_F_POLL_TIMEOUT) != 0, "TIMEOUT latches, names 0x15, flags POLL_TIMEOUT");
        expect(hardmins().size() == 2 && hardmins()[1].second == hm_param(6, 272) && (r.flags & N48N_DAL_F_RESTORE_FAILED) == 0 && (r.flags & N48N_DAL_F_RESTORED) != 0, "after a poll timeout the restore IS attempted, and it verifies by readback");
        expect(r.hardmin_poll_us >= 1000000 && r.hardmin_poll_us < 1030000, "the 0x15 poll is bounded at 1 s");
    }
    SCEN("0x15 mailbox hang") {   // a MAILBOX timeout: nothing more is sent
        const auto &r = fail_e2([] { W.rules.push_back({ 0x15, 1, true, 0 }); });
        expect_verdict("a ReturnHardMinStatus message the mailbox never answers", r, kTimeout);
        expect(r.latched == 1 && r.fail_msg == 0x15 && hardmins().size() == 1 && (r.flags & N48N_DAL_F_RESTORE_FAILED) != 0 && (r.flags & N48N_DAL_F_RESTORED) == 0 && (r.flags & N48N_DAL_F_POLL_TIMEOUT) == 0, "mailbox timeout: no restore is attempted, and the flag says so (not a poll timeout)");
    }
    SCEN("other clock regresses in E2") {   // DPPCLK falls below its start when DISPCLK is raised: the restore must cover the clock the step never raised
        const auto &r = fail_e2([] { W.otherBelowOnRaise = true; });
        expect_verdict("DPPCLK regresses in E2", r, kRegressed);
        const auto h = hardmins();
        expect(h.size() == 3 && h[0].second == hm_param(6, 300) && h[1].second == hm_param(6, 272) && h[2].second == hm_param(7, 272), "the restore sends 272 MHz for DISPCLK (raised) AND for DPPCLK (below its start, never raised)");
        expect(r.latched == 1 && (r.flags & N48N_DAL_F_RESTORED) != 0 && (r.flags & N48N_DAL_F_RESTORE_FAILED) == 0 && r.khz_restored[1] >= 272727, "restored, and the DPPCLK reads at least its start value again");
    }
    SCEN("raise hangs") {
        const auto &r = fail_e2([] { W.rules.push_back({ 9, 1, true, 0 }); });
        expect_verdict("SetHardMin hangs", r, kTimeout); expect(r.latched == 1 && hardmins().size() == 1 && (r.flags & N48N_DAL_F_RESTORE_FAILED) != 0, "TIMEOUT on the raise: nothing further sent");
    }
    SCEN("frame stall") {
        const auto &r = fail_e2([] { W.useFpsAfter = true; W.fpsAfter = 0.0; W.fpsAfterDelayNs = 3000000000ull; });
        expect_verdict("the frame counter stalls in the dwell", r, kGlitch); expect(r.latched == 1 && hardmins().size() == 2 && (r.flags & N48N_DAL_F_RESTORED) != 0, "a stall is a GLITCH; the restore ran");
        expect(r.dwell_ms > 3000 && r.dwell_ms < 6000, "the stall ends the dwell early (~250 ms after the counter stopped), it is not left to the end-of-dwell rate check");
    }
    SCEN("rate drift") {
        const auto &r = fail_e2([] { W.useFpsAfter = true; W.fpsAfter = 55.0; W.fpsAfterDelayNs = 0; });
        expect_verdict("the frame rate drifts to 55 fps", r, kGlitch); expect(r.latched == 1, "rate drift latches");
    }
    SCEN("restore refused") {
        const auto &r = fail_e2([] { W.rules.push_back({ 9, 2, false, 0xFC }); });
        expect_verdict("the restore is refused", r, kRefused); expect(r.latched == 1 && (r.flags & N48N_DAL_F_RESTORE_FAILED) != 0 && (r.flags & N48N_DAL_F_RESTORED) == 0, "a failed restore turns PASS into REFUSED and flags it");
    }
    SCEN("restore hangs") {
        const auto &r = fail_e2([] { W.rules.push_back({ 9, 2, true, 0 }); });
        expect_verdict("the restore times out", r, kTimeout); expect(r.latched == 1 && (r.flags & N48N_DAL_F_RESTORE_FAILED) != 0, "a restore timeout is flagged");
    }
    SCEN("restore lands low") {
        const auto &r = fail_e2([] { W.restoreDidOverride = true; W.restoreDid = 0x42; });
        expect_verdict("the restore lands below the start value", r, kRegressed);
        expect(r.latched == 1 && (r.flags & N48N_DAL_F_RESTORE_FAILED) != 0 && r.khz_restored[0] < 272727, "REGRESSED + RESTORE_FAILED; the restored clock is reported");
    }
    // pre-flight and baseline refusals send nothing and do not latch
    SCEN("OTG not lit") { const auto &r = fail_e2([] { W.lit = false; }); expect_verdict("OTG not lit", r, kDenied); expect(r.deny == kDenyFrames && r.latched == 0 && W.seen.empty(), "DENIED (frames), nothing sent, no latch"); }
    SCEN("frames not running") { const auto &r = fail_e2([] { W.fps = 0.0; }); expect_verdict("frame counter not running", r, kDenied); expect(r.deny == kDenyFrames && r.latched == 0 && W.seen.empty(), "DENIED (frames): nothing sent"); }
    SCEN("scanout status unreadable") { const auto &r = fail_e2([] { W.frameFail = true; }); expect_verdict("scanout status unreadable", r, kDenied); expect(r.latched == 0 && W.seen.empty(), "DENIED: nothing sent"); }
    SCEN("baseline DENTIST disagrees") { const auto &r = fail_e2([] { W.dentistFrozen = W.reg[kRegDentist] ^ 1u; W.dentistStale = true; }); expect_verdict("baseline DENTIST disagrees", r, kDenied); expect(r.deny == kDenyBaseline && r.latched == 0 && W.seen.empty(), "DENIED (baseline): nothing sent"); }
    SCEN("baseline out of range") { const auto &r = fail_e2([] { W.did[0] = 0x7F; }); expect_verdict("baseline DISPCLK far outside [250, 700] MHz", r, kDenied); expect(r.deny == kDenyBaseline && W.seen.empty(), "DENIED (baseline)"); }
    // the allowlist inside a step: E1b max 260 -> E2's 300 is above it
    SCEN("allowlist inside a step") {
        world_reset(2); W.dpm[0] = { 250, 260 }; (void)step(kE1b); W.seen.clear();
        const auto &r = step(kE2); expect_verdict("300 MHz above an E1b max of 260", r, kDenied);
        expect(r.refuse == kRefAboveMax && r.latched == 0 && W.seen.empty() && r.nmsg == 0, "refused by the allowlist before anything is sent: DENIED, no latch");
        expect(gLongLine == 0, "no log line reaches 512 bytes");
    }
}

static std::vector<uint32_t> gHookVerdicts;
static void hook_second_step() { static bool in = false; if (in) return; in = true; n48n_dal_result r2; IOReturn rc = amdgpu::dal_run_step(gDev, kE1b, &r2); gHookVerdicts.push_back(rc == 0 ? r2.verdict : 0xFFFF); gHookVerdicts.push_back(r2.deny); in = false; }
static size_t polls15() { size_t n = 0; for (auto &m : W.seen) if (m.first == 0x15) n++; return n; }
static void b4_latch_busy() {
    SCEN("busy") {   // a second step started while the first sleeps is DENIED (busy) and does not disturb the first
        world_reset(1); gSleepHook = hook_second_step; gHookVerdicts.clear();
        const n48n_dal_result &r = step(kE1b); gSleepHook = nullptr; expect_verdict("the first step still passes", r, kPass);
        expect(gHookVerdicts.size() >= 2 && gHookVerdicts[0] == kDenied && gHookVerdicts[1] == kDenyBusy, "a step started during a step is DENIED busy");
        const n48n_dal_result &r2 = step(kE1b); expect_verdict("after the first ends the flag is free", r2, kPass);
    }
    SCEN("trial busy") {   // 0.0.605: a step while a mode trial runs is DENIED busy, writes no message, and leaves no busy flag behind
        world_reset(1); n48dcn::gTrialBusy = true;
        const n48n_dal_result &r = step(kE1b); expect(r.verdict == kDenied && r.deny == kDenyBusy && r.nmsg == 0, "a DAL step while a mode trial runs: DENIED busy, nothing sent");
        n48dcn::gTrialBusy = false;
        const n48n_dal_result &r2 = step(kE1b); expect_verdict("the DAL busy flag was released by the denial", r2, kPass);
        expect(!amdgpu::dal_busy(), "dal_busy() is false when no step runs");
    }
    SCEN("latch after E4") {   // a stop in E4 blocks every step afterwards
        ready_at(4); W.regressOnRaise = true;
        const n48n_dal_result &r = step(kE4); expect_verdict("E4 regresses", r, kRegressed); expect(r.latched == 1, "latched");
        for (uint32_t s = 1; s <= 4; s++) { const n48n_dal_result &q = step(s); expect(q.verdict == kDenied && q.deny == kDenyLatched && q.latched == 1 && (q.flags & N48N_DAL_F_LATCHED), "after a stop every step is DENIED (latched)"); }
        expect(W.seen.size() == hardmins().size() + polls15(), "nothing but hard-mins and their polls was ever sent");
    }
    SCEN("E4 DPPCLK refused") {   // the restore covers exactly the clock that was raised
        ready_at(4); W.rules.push_back({ 9, 2, false, 0xFD });
        const n48n_dal_result &r = step(kE4); expect_verdict("E4: DPPCLK refused after DISPCLK was raised", r, kRefused);
        const auto h = hardmins(); expect(h.size() == 3 && h[0].second == hm_param(6, 530) && h[1].second == hm_param(7, 530) && h[2].second == hm_param(6, 272), "the restore covers only DISPCLK (the clock that was raised)");
        expect((r.flags & N48N_DAL_F_RESTORED) != 0 && r.latched == 1, "restored and latched");
    }
    SCEN("E4 DISPCLK does not follow") {
        ready_at(4); W.follow[0] = false;
        const n48n_dal_result &r = step(kE4); expect_verdict("E4: DISPCLK does not follow", r, kRefused); expect(hardmins().size() == 4 && r.latched == 1, "both clocks restored");
    }
}

struct LockShared { volatile int t2done; volatile int t2started; };
static LockShared gLs;
static void *t2_dal(void *) { gLs.t2started = 1; amdgpu::DalMsg m; (void)amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); gLs.t2done = 1; return nullptr; }
static void *t2_ppsmc(void *) { gLs.t2started = 1; uint32_t echo = 0; (void)amdgpu::smu_test_message(gDev, &echo); gLs.t2done = 1; return nullptr; }

// =====================================================================================================================================
// P4 (0.0.607): the clock hold. H: the pure half (state machine, precondition matrix, raise / release against a fake interface with a failure injected at EVERY message);
// B6: the real smu_dal.cpp code against the simulated PMFW.
// =====================================================================================================================================
static void h1_state_machine() {
    // an independent table of the legal transitions (state, event) -> next; everything else is invalid
    struct T { uint32_t st, ev, nx; };
    const T legal[] = { { kHsIdle, kEvRaise, kHsRaising }, { kHsReleased, kEvRaise, kHsRaising }, { kHsRaising, kEvHeld, kHsHeld }, { kHsRaising, kEvNothingSent, kHsIdle }, { kHsRaising, kEvUnwound, kHsReleased },
                        { kHsRaising, kEvRaiseStuck, kHsStuck }, { kHsHeld, kEvRelease, kHsReleasing }, { kHsReleasing, kEvReleased, kHsReleased }, { kHsHeld, kEvStuck, kHsStuck }, { kHsReleasing, kEvStuck, kHsStuck } };
    uint32_t nLegal = 0, nBad = 0;
    for (uint32_t st = 0; st <= 7; st++) for (uint32_t ev = 0; ev <= 10; ev++) {
        uint32_t want = kHsInvalid;
        for (const T &t : legal) if (t.st == st && t.ev == ev) want = t.nx;
        const uint32_t got = hold_next(st, ev);
        if (got != want) { gFail++; std::printf("FAIL: hold_next(%u, %u) = %u, want %u\n", st, ev, got, want); }
        gRun++;
        if (want == kHsInvalid) nBad++; else nLegal++;
    }
    expect_u("the state machine has exactly ten legal transitions", nLegal, 10);
    expect(nBad == 8u * 11u - 10u, "every other (state, event) pair is invalid");
    // STUCK is terminal: no event leaves it
    for (uint32_t ev = 0; ev <= 10; ev++) expect_u("STUCK is terminal", hold_next(kHsStuck, ev), kHsInvalid);
    // a release can only be claimed from HELD (once): RELEASING and RELEASED refuse a second claim
    expect(hold_next(kHsReleasing, kEvRelease) == kHsInvalid && hold_next(kHsReleased, kEvRelease) == kHsInvalid && hold_next(kHsIdle, kEvRelease) == kHsInvalid && hold_next(kHsRaising, kEvRelease) == kHsInvalid, "the single release claim: only HELD -> RELEASING");
    expect_u("owner values", kOwnerIdle * 100 + kOwnerDal * 10 + kOwnerHold, 12);
    // the precondition matrix against an independent rule
    uint32_t n = 0;
    for (uint32_t level = 0; level <= 6; level++) for (int e1b = 0; e1b < 2; e1b++) for (int stop = 0; stop < 2; stop++) for (uint32_t hs = 0; hs <= 5; hs++) for (uint32_t ow = 0; ow <= 2; ow++) {
        uint32_t want = kHpOk;
        if (level != 4) want = kHpLevel; else if (!e1b) want = kHpNoE1b; else if (stop) want = kHpLatched; else if (hs != kHsIdle && hs != kHsReleased) want = kHpState; else if (ow != kOwnerIdle) want = kHpOwner;
        const uint32_t got = hold_pre_check(level, e1b != 0, stop != 0, hs, ow);
        if (got != want) { gFail++; std::printf("FAIL: hold_pre_check(level %u e1b %d stop %d state %u owner %u) = %u want %u\n", level, e1b, stop, hs, ow, got, want); }
        gRun++; n++;
    }
    expect_u("precondition cases enumerated", n, 7u * 2u * 2u * 6u * 3u);
    expect_u("only level 4, E1b done, no latch, IDLE / RELEASED and an idle mailbox may hold", hold_pre_check(4, true, false, kHsIdle, kOwnerIdle) + hold_pre_check(4, true, false, kHsReleased, kOwnerIdle), 0);
    expect(hold_clocks_ok(dec_did(0x21, 0x21), 514285, 500000) && hold_clocks_ok(dec_did(0x23, 0x24), 514285, 500000) && !hold_clocks_ok(dec_did(0x24, 0x24), 514285, 500000) && !hold_clocks_ok(dec_did(0x23, 0x25), 514285, 500000) &&
           !hold_clocks_ok(dec_did(0x21, 0x21, false), 514285, 500000) && !hold_clocks_ok(dec_did(0x21, 0x21, true, false), 514285, 500000) && !hold_clocks_ok(Decoded{}, 0, 0), "hold_clocks_ok: the need, DENTIST agreement, CHG_DONE, decodable");
}

// ---- the fake interface: a clock pair that follows the hard-mins, with one fault injected at message `failAt` ----
struct Fake {
    uint32_t did[2] = { 0x41, 0x41 };
    std::vector<std::pair<uint32_t, uint32_t>> msgs;    // (clk, mhz) in order
    int failAt = -1; uint32_t failKind = kHmFailed; bool failAcked = false;
    bool neverMove = false, noDrop = false;             // raises do not move the clocks; releases do not lower them
    uint64_t now = 1000000ull; uint32_t latch = 0, owner = 0;
    std::vector<uint32_t> logs;
};
static Fake *gF;
static uint32_t f_hm(void *, uint32_t clk, uint32_t mhz, bool *acked) {
    Fake &f = *gF; const int idx = (int)f.msgs.size(); f.msgs.push_back({ clk, mhz });
    const int i = (int)clk - 6;
    const bool raise = mhz >= 500u;
    auto apply = [&] { if (raise) { if (!f.neverMove) f.did[i] = 0x21; } else if (!f.noDrop) f.did[i] = 0x41; };
    if (idx == f.failAt) {
        *acked = f.failAcked;
        if (f.failKind == kHmPollTimeout) { *acked = true; apply(); }
        return f.failKind;
    }
    *acked = true; apply(); return kHmOk;
}
static bool f_dec(void *, Decoded *d) { *d = dec_did(gF->did[0], gF->did[1]); return true; }
static void f_sl(void *, uint32_t ms) { gF->now += (uint64_t)ms * 1000ull; }
static uint64_t f_now(void *) { return gF->now; }
static void f_latch(void *) { gF->latch++; }
static void f_log(void *, uint32_t code, uint32_t, uint32_t, uint32_t) { gF->logs.push_back(code); }
static HoldIo f_io() { return HoldIo{ nullptr, f_hm, f_dec, f_sl, f_now, f_latch, f_log }; }
struct Run { HoldCtl c; HoldRep rep; Decoded after; uint32_t rc; };
static HoldCtl fresh_ctl(Fake &f) { HoldCtl c{}; c.state = kHsIdle; c.owner = &f.owner; return c; }
static Run do_raise(Fake &f, bool e1b = true, uint32_t level = 4, bool stopped = false) {
    gF = &f; Run r{}; r.c = fresh_ctl(f); const HoldIo io = f_io();
    r.rc = hold_raise(io, r.c, r.rep, level, e1b, stopped, 514285, 500000, &r.after);
    return r;
}
static void h2_raise_release() {
    // ---- a clean raise and release ----
    { Fake f; Run r = do_raise(f);
      expect(r.rc == kHrOk && hold_state_now(r.c) == kHsHeld && f.owner == kOwnerHold && r.rep.state == kHsHeld && (r.rep.flags & kHfRaised), "raise: HELD, owner 2");
      expect(f.msgs.size() == 2 && f.msgs[0] == std::make_pair(6u, 530u) && f.msgs[1] == std::make_pair(7u, 530u), "raise: DISPCLK 530 first, then DPPCLK 530");
      expect(hold_clocks_ok(r.after, 514285, 500000) && r.rep.khzRaised[0] == r.after.dispKhz && r.rep.khzRaised[1] == r.after.dppKhz && r.rep.khzRaised[0] == 545454, "raise: the readback is at the need (545.45 MHz), reported");
      const HoldIo io = f_io(); HoldRep rep{};
      const uint32_t rr = hold_release(io, r.c, rep, false);
      expect(rr == kRlOk && hold_state_now(r.c) == kHsReleased && f.owner == kOwnerIdle && (rep.flags & kHfReleased), "release: RELEASED, owner freed");
      expect(f.msgs.size() == 4 && f.msgs[2] == std::make_pair(7u, 272u) && f.msgs[3] == std::make_pair(6u, 272u), "release: DPPCLK then DISPCLK to the 272 floor");
      expect(rep.khzReleased[0] == 272727 && rep.khzReleased[1] == 272727 && f.latch == 0, "release: back at the start, no latch");
      // a second release finds nothing to do and sends nothing
      HoldRep rep2{}; const uint32_t r2 = hold_release(io, r.c, rep2, false);
      expect(r2 == kRlNotHeld && f.msgs.size() == 4, "a second release is a no-op (single claim)");
      // RELEASED may raise again (a fresh cycle)
      HoldRep rep3{}; Decoded a{}; const uint32_t r3 = hold_raise(io, r.c, rep3, 4, true, false, 514285, 500000, &a);
      expect(r3 == kHrOk && hold_state_now(r.c) == kHsHeld, "RELEASED -> RAISING -> HELD again"); }
    // ---- the release claim is single: someone already in RELEASING makes hold_release a no-op ----
    { Fake f; Run r = do_raise(f); const HoldIo io = f_io(); HoldRep rep{};
      expect(hold_cas_state(r.c, kEvRelease), "(setup) another party claimed the release");
      const size_t n0 = f.msgs.size(); const uint32_t rr = hold_release(io, r.c, rep, false);
      expect(rr == kRlNotHeld && f.msgs.size() == n0 && hold_state_now(r.c) == kHsReleasing, "the second releaser sends nothing and leaves the state alone"); }
    // ---- restoreBad NEVER releases: HELD -> STUCK, no message, latched, the owner stays 2 ----
    { Fake f; Run r = do_raise(f); const HoldIo io = f_io(); HoldRep rep{}; const size_t n0 = f.msgs.size();
      const uint32_t rr = hold_release(io, r.c, rep, true);
      expect(rr == kRlBadRestore && hold_state_now(r.c) == kHsStuck && f.msgs.size() == n0 && f.latch == 1 && f.owner == kOwnerHold && (rep.flags & kHfStuck) && f.did[0] == 0x21 && f.did[1] == 0x21,
             "release with restoreBad: STUCK, no message, latched, clocks untouched, mailbox owner still 2");
      HoldRep rep2{}; const uint32_t r2 = hold_release(io, r.c, rep2, false);
      expect(r2 == kRlNotHeld && f.msgs.size() == n0, "STUCK never releases afterwards either");
      HoldRep rep3{}; Decoded a{}; expect(hold_raise(io, r.c, rep3, 4, true, false, 514285, 500000, &a) == kHrPre && rep3.pre == kHpState && f.msgs.size() == n0, "STUCK never raises again (state precondition)"); }
    // ---- refusals before any message ----
    { struct C { bool e1b; uint32_t level; bool stopped; uint32_t pre; } cs[] = { { false, 4, false, kHpNoE1b }, { true, 3, false, kHpLevel }, { true, 0, false, kHpLevel }, { true, 4, true, kHpLatched } };
      for (const C &c : cs) { Fake f; Run r = do_raise(f, c.e1b, c.level, c.stopped);
        expect(r.rc == kHrPre && r.rep.pre == c.pre && f.msgs.empty() && f.owner == kOwnerIdle && hold_state_now(r.c) == kHsIdle, "precondition refusal: nothing sent, state IDLE, owner free"); }
      Fake f; f.owner = kOwnerDal; Run r = do_raise(f);
      expect(r.rc == kHrPre && r.rep.pre == kHpOwner && f.msgs.empty() && f.owner == kOwnerDal, "a DAL step running (owner 1): refused, the owner word untouched");
      Fake g; gF = &g; HoldCtl c = fresh_ctl(g); c.owner = nullptr; HoldRep rep{}; const HoldIo io = f_io();
      expect(hold_raise(io, c, rep, 4, true, false, 514285, 500000, nullptr) == kHrNoIo, "a broken owner wiring refuses");
      HoldIo bad = io; bad.hard_min = nullptr; HoldCtl c2 = fresh_ctl(g); expect(hold_raise(bad, c2, rep, 4, true, false, 514285, 500000, nullptr) == kHrNoIo && g.msgs.empty(), "a missing interface refuses"); }
    // ---- a baseline that is not the boot state: unreadable / already at the need ----
    { Fake f; f.did[0] = f.did[1] = 0x21; Run r = do_raise(f);
      expect(r.rc == kHrBaseline && f.msgs.empty() && hold_state_now(r.c) == kHsIdle && f.owner == kOwnerIdle, "a clock already at the need (a floor this hold did not raise): refused, nothing sent"); }
    // ---- a raise failure at EVERY message, every kind ----
    struct FI { int at; uint32_t kind; bool acked; const char *what; uint32_t rc, state; size_t nmsg; uint32_t owner; uint32_t latch; };
    const FI raise_cases[] = {
        { 0, kHmFailed,      false, "msg 0 failed (mailbox alive)",         kHrMsg,     kHsIdle,     1, kOwnerIdle, 0 },   // nothing acknowledged: no floor, nothing to undo
        { 0, kHmPollTimeout, true,  "msg 0 poll timeout",                   kHrPoll,    kHsReleased, 2, kOwnerIdle, 0 },   // DISPCLK acked: unwound with (6, 272)
        { 0, kHmMailbox,     false, "msg 0 mailbox lost, not acked",        kHrMailbox, kHsStuck,    1, kOwnerHold, 1 },   // no message can undo anything: STUCK
        { 0, kHmMailbox,     true,  "msg 0 mailbox lost after the ack",     kHrMailbox, kHsStuck,    1, kOwnerHold, 1 },
        { 1, kHmFailed,      false, "msg 1 failed (DISPCLK already up)",    kHrMsg,     kHsReleased, 3, kOwnerIdle, 0 },   // unwind DISPCLK only
        { 1, kHmPollTimeout, true,  "msg 1 poll timeout (both up)",         kHrPoll,    kHsReleased, 4, kOwnerIdle, 0 },   // unwind DPPCLK then DISPCLK
        { 1, kHmMailbox,     false, "msg 1 mailbox lost (DISPCLK up)",      kHrMailbox, kHsStuck,    2, kOwnerHold, 1 },
        { 2, kHmFailed,      false, "the first unwind message failed",      kHrPoll,    kHsStuck,    0, kOwnerHold, 1 },   // (rc / nmsg filled below: this row injects into the unwind of a poll-timeout raise)
    };
    for (const FI &c : raise_cases) {
        Fake f; f.failAt = c.at; f.failKind = c.kind; f.failAcked = c.acked;
        if (c.at == 2) { // a poll timeout on msg 1 (both up) and then the FIRST unwind message (index 2) fails: the clock does not come off -> STUCK
            f.failAt = 2; f.failKind = kHmFailed;
            Fake g; g.failAt = 1; g.failKind = kHmPollTimeout; g.failAcked = true; gF = &g; HoldCtl cc = fresh_ctl(g); HoldRep rep{}; const HoldIo io = f_io();
            // inject two faults: msg 1 poll timeout, msg 2 (the unwind of DPPCLK) refused so DPPCLK stays up
            struct Two { static uint32_t hm(void *x, uint32_t clk, uint32_t mhz, bool *acked) { Fake &q = *gF; const size_t idx = q.msgs.size(); if (idx == 2) { q.msgs.push_back({ clk, mhz }); *acked = false; return kHmFailed; } return f_hm(x, clk, mhz, acked); } };
            HoldIo io2 = io; io2.hard_min = Two::hm; Decoded a{};
            const uint32_t rc = hold_raise(io2, cc, rep, 4, true, false, 514285, 500000, &a);
            expect(rc == kHrPoll && hold_state_now(cc) == kHsStuck && g.latch == 1 && g.owner == kOwnerHold && (rep.flags & kHfStuck) && g.did[1] == 0x21, "an unwind message that fails leaves DPPCLK up: STUCK, latched, owner stays 2");
            continue;
        }
        Run r = do_raise(f);
        char w[160]; std::snprintf(w, sizeof(w), "raise fault: %s: rc", c.what);
        expect_u(w, r.rc, c.rc);
        std::snprintf(w, sizeof(w), "raise fault: %s: final state", c.what); expect_u(w, hold_state_now(r.c), c.state);
        std::snprintf(w, sizeof(w), "raise fault: %s: messages sent", c.what); expect_u(w, f.msgs.size(), c.nmsg);
        std::snprintf(w, sizeof(w), "raise fault: %s: owner word", c.what); expect_u(w, f.owner, c.owner);
        std::snprintf(w, sizeof(w), "raise fault: %s: DAL latch", c.what); expect_u(w, f.latch, c.latch);
        if (c.state == kHsReleased || c.state == kHsIdle) expect(f.did[0] == 0x41 && f.did[1] == 0x41, "a failed raise that unwound leaves both clocks at the start");
        if (c.state == kHsReleased) {
            // the unwind restored only acknowledged clocks, DPPCLK before DISPCLK
            const auto &m = f.msgs;
            if (c.at == 0) expect(m[1] == std::make_pair(6u, 272u), "unwind of a lone DISPCLK");
            if (c.at == 1 && c.kind == kHmFailed) expect(m[2] == std::make_pair(6u, 272u), "unwind of DISPCLK only (DPPCLK was never acknowledged)");
            if (c.at == 1 && c.kind == kHmPollTimeout) expect(m[2] == std::make_pair(7u, 272u) && m[3] == std::make_pair(6u, 272u), "unwind DPPCLK first, then DISPCLK");
        }
    }
    // ---- the DFS never reaches the need after both raises were acknowledged: unwound ----
    { Fake f; f.neverMove = true; Run r = do_raise(f);
      expect(r.rc == kHrClocks && hold_state_now(r.c) == kHsReleased && f.owner == kOwnerIdle && f.msgs.size() == 4 && f.latch == 0, "clocks never reach the need: unwound (both acknowledged), RELEASED"); }
    // ---- release faults at EVERY message ----
    struct RI { int at; uint32_t kind; const char *what; uint32_t rc, state; uint32_t latch; uint32_t owner; };
    const RI rel_cases[] = {
        { 0, kHmFailed,      "release msg 0 (DPPCLK) failed: DPPCLK stays up", kRlMsg,     kHsStuck,    1, kOwnerHold },
        { 1, kHmFailed,      "release msg 1 (DISPCLK) failed: DISPCLK stays up", kRlMsg,   kHsStuck,    1, kOwnerHold },
        { 0, kHmPollTimeout, "release msg 0 poll timeout (the clock did drop)", kRlOk,     kHsReleased, 0, kOwnerIdle },
        { 1, kHmPollTimeout, "release msg 1 poll timeout (the clock did drop)", kRlOk,     kHsReleased, 0, kOwnerIdle },
        { 0, kHmMailbox,     "release mailbox lost at msg 0", kRlMailbox, kHsStuck, 1, kOwnerHold },
        { 1, kHmMailbox,     "release mailbox lost at msg 1", kRlMailbox, kHsStuck, 1, kOwnerHold },
    };
    for (const RI &c : rel_cases) {
        Fake f; Run r = do_raise(f); const size_t base = f.msgs.size(); f.failAt = (int)base + c.at; f.failKind = c.kind; f.failAcked = false;
        const HoldIo io = f_io(); HoldRep rep{}; const uint32_t rr = hold_release(io, r.c, rep, false);
        char w[160]; std::snprintf(w, sizeof(w), "%s: rc", c.what); expect_u(w, rr, c.rc);
        std::snprintf(w, sizeof(w), "%s: state", c.what); expect_u(w, hold_state_now(r.c), c.state);
        std::snprintf(w, sizeof(w), "%s: latch", c.what); expect_u(w, f.latch, c.latch);
        std::snprintf(w, sizeof(w), "%s: owner", c.what); expect_u(w, f.owner, c.owner);
        if (c.kind == kHmMailbox && c.at == 0) expect(f.msgs.size() == base + 1, "a mailbox that stopped answering ends the release: no further message");
    }
    { Fake f; Run r = do_raise(f); f.noDrop = true; const HoldIo io = f_io(); HoldRep rep{}; const uint32_t rr = hold_release(io, r.c, rep, false);
      expect(rr == kRlNotBack && hold_state_now(r.c) == kHsStuck && f.latch == 1 && f.owner == kOwnerHold && rep.khzReleased[0] == 545454, "the clocks did not come off the need within 500 ms: STUCK (an 'at or above the start' reading of a still-raised clock is not a release)"); }
    // the release verify is bounded: it never waits longer than kHoldReleaseVerifyMs of fake time
    { Fake f; Run r = do_raise(f); f.noDrop = true; const uint64_t t0 = f.now; const HoldIo io = f_io(); HoldRep rep{}; (void)hold_release(io, r.c, rep, false);
      expect((f.now - t0) / 1000ull <= kHoldReleaseVerifyMs + 20u, "the release readback is bounded"); }
    { Fake f; f.neverMove = true; const uint64_t t0 = f.now; Run r = do_raise(f); (void)r;
      expect((f.now - t0) / 1000ull <= kHoldSettleMs + kHoldReleaseVerifyMs + 40u, "the raise's DFS wait plus the unwind's readback are bounded"); }
    expect_u("the hold target is E4's 530 MHz", kHoldMhz, 530); expect_u("the release floor is 272 MHz", kRestoreMhz, 272);
}

// ---- B6: the real hold code against the simulated PMFW ----
static std::vector<uint32_t> gHookHold;
static void hook_hold_during_step() {
    static bool in = false; if (in) return; in = true;
    gHookHold.push_back(amdgpu::dal_hold_pre());
    n48n_dal_result unused; (void)unused;
    n48dal::HoldRep rep{}; n48dal::Decoded a{};
    gHookHold.push_back(amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a));
    gHookHold.push_back(rep.pre);
    in = false;
}
static void b6_hold() {
    SCEN("hold: raise, DAL step refused while held, release") {
        ready_at(4);
        expect_u("dal_hold_pre is 0 after E1b at level 4", amdgpu::dal_hold_pre(), 0);
        HoldRep rep{}; Decoded a{};
        const uint32_t rc = amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a);
        expect(rc == kHrOk && amdgpu::dal_hold_state() == kHsHeld && rep.state == kHsHeld, "the real raise: HELD");
        const auto h = hardmins();
        expect(h.size() == 2 && h[0].second == hm_param(6, 530) && h[1].second == hm_param(7, 530), "the PMFW saw SetHardMin (6<<16)|530 then (7<<16)|530");
        expect(a.dispKhz >= 514285 && a.dppKhz >= 500000 && W.did[0] == 0x21 && W.did[1] == 0x21 && rep.khzRaised[0] == 545454, "DFS 0x21 = 545.45 MHz on both clocks");
        expect(!amdgpu::dal_busy(), "dal_busy() is false while a hold is up (F9: the trial that owns the hold is not denied by it)");
        W.seen.clear();
        const n48n_dal_result &q = step(kE2);
        expect(q.verdict == kDenied && q.deny == kDenyBusy && q.nmsg == 0 && W.seen.empty(), "a DAL step while the hold is up: DENIED busy, nothing sent");
        const n48n_dal_result &q2 = step(kE1b);
        expect(q2.verdict == kDenied && q2.deny == kDenyBusy && W.seen.empty(), "E1b too");
        expect_u("a second raise while HELD is refused by the state precondition", amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a), kHrPre);
        expect_u("... reason: state", rep.pre, kHpState);
        HoldRep rr{};
        const uint32_t rel = amdgpu::dal_hold_release(gDev, false, &rr);
        expect(rel == kRlOk && amdgpu::dal_hold_state() == kHsReleased && rr.khzReleased[0] == 272727 && rr.khzReleased[1] == 272727, "the real release: RELEASED at the start");
        const auto h2 = hardmins();
        expect(h2.size() == 2 && h2[0].second == hm_param(7, 272) && h2[1].second == hm_param(6, 272), "the PMFW saw DPPCLK 272 then DISPCLK 272");
        check_no_stray("hold");
        const n48n_dal_result &q3 = step(kE2); expect_verdict("after the release the DAL step runs again (owner freed)", q3, kPass);
        expect_u("dal_hold_pre after RELEASED and a DAL step", amdgpu::dal_hold_pre(), 0);
        expect(gLongLine == 0, "no hold log line reaches 512 bytes");
        bool sawHeld = false, sawReleased = false; for (auto &l : gLog) { if (l.find("hold: HELD") != std::string::npos) sawHeld = true; if (l.find("hold: RELEASED") != std::string::npos) sawReleased = true; }
        expect(sawHeld && sawReleased, "the hold logs HELD and RELEASED");
    }
    SCEN("hold: preconditions") {
        world_reset(4);   // no E1b on this boot
        expect_u("no E1b: refused", amdgpu::dal_hold_pre(), kHpNoE1b);
        HoldRep rep{}; Decoded a{};
        expect(amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a) == kHrPre && W.seen.empty(), "no E1b: nothing sent");
        const n48n_dal_result &q = step(kE1b); expect_verdict("E1b", q, kPass);
        expect_u("E1b done: allowed", amdgpu::dal_hold_pre(), 0);
        gS1b.positivePass = false;
        expect_u("no S1b POSITIVE PASS: level 0", amdgpu::dal_hold_pre(), kHpLevel);
        gS1b.positivePass = true; gBootArg.val = 3;
        expect_u("boot-arg level 3: refused", amdgpu::dal_hold_pre(), kHpLevel);
        gBootArg.present = false;
        expect_u("no boot-arg: refused", amdgpu::dal_hold_pre(), kHpLevel);
        gBootArg.present = true; gBootArg.val = 4; W.seen.clear();
        expect_u("level 4 again", amdgpu::dal_hold_pre(), 0);
    }
    SCEN("hold: latch blocks") {
        ready_at(4); W.regressOnRaise = true;
        const n48n_dal_result &r = step(kE4); expect_verdict("E4 regresses", r, kRegressed);
        expect_u("a latched DAL blocks the hold", amdgpu::dal_hold_pre(), kHpLatched);
    }
    SCEN("hold: message 1 refused, nothing acknowledged") {
        ready_at(4); W.rules.push_back({ 9, 1, false, 0xFD });
        HoldRep rep{}; Decoded a{}; const uint32_t rc = amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a);
        expect(rc == kHrMsg && amdgpu::dal_hold_state() == kHsIdle && hardmins().size() == 1 && !amdgpu::dal_busy(), "0xFD on the first raise: IDLE, one message, no floor");
        const n48n_dal_result &q = step(kE2); expect_verdict("the mailbox owner is free again", q, kPass);
    }
    SCEN("hold: DPPCLK refused after DISPCLK, unwound") {
        ready_at(4); W.rules.push_back({ 9, 2, false, 0xFD });
        HoldRep rep{}; Decoded a{}; const uint32_t rc = amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a);
        const auto h = hardmins();
        expect(rc == kHrMsg && amdgpu::dal_hold_state() == kHsReleased && h.size() == 3 && h[2].second == hm_param(6, 272) && W.did[0] == 0x41 && W.did[1] == 0x41, "DPPCLK refused: DISPCLK unwound to 272, RELEASED, both clocks at the start");
        const n48n_dal_result &q = step(kE2); expect_verdict("the DAL step runs after the unwind", q, kPass);
    }
    SCEN("hold: the mailbox stops answering after the first raise") {
        ready_at(4); W.rules.push_back({ 9, 2, true, 0 });   // the second SetHardMin never answers
        HoldRep rep{}; Decoded a{}; const uint32_t rc = amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a);
        const size_t nHm = hardmins().size();
        expect(rc == kHrMailbox && amdgpu::dal_hold_state() == kHsStuck && (rep.flags & kHfMailboxLost) && (rep.flags & kHfStuck), "a dead mailbox: STUCK, no unwind attempt");
        expect(nHm == 2 && W.did[0] == 0x21, "no message after the mailbox stopped: the DISPCLK floor stays up (cold power cycle)");
        const n48n_dal_result &q = step(kE2); expect(q.verdict == kDenied && q.latched == 1, "DAL steps are latched");
        HoldRep r2{}; expect_u("STUCK never releases", amdgpu::dal_hold_release(gDev, false, &r2), kRlNotHeld);
    }
    SCEN("hold: the DFS never reaches the need") {
        ready_at(4); W.follow[0] = false;
        HoldRep rep{}; Decoded a{}; const uint32_t rc = amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a);
        expect(rc == kHrClocks && amdgpu::dal_hold_state() == kHsReleased && hardmins().size() == 4 && W.did[0] == 0x41 && W.did[1] == 0x41, "a clock that will not follow: unwound, RELEASED at the start");
    }
    SCEN("hold: the 0x15 poll never reports done") {   // bounded at 1 s per hard-min; the raise fails with POLL, and the unwind restores the acknowledged clock
        ready_at(4); W.hm15Never = true;
        const uint64_t t0 = gNowNs;
        HoldRep rep{}; Decoded a{}; const uint32_t rc = amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a);
        expect(rc == kHrPoll && amdgpu::dal_hold_state() == kHsReleased && W.did[0] == 0x41 && W.did[1] == 0x41, "a poll that never completes: POLL timeout, the acknowledged DISPCLK unwound, RELEASED at the start");
        expect((gNowNs - t0) / 1000000ull <= 2600ull, "and it took at most ~2 x the 1 s poll bound (raise + unwind) of fake time");
        expect(hardmins().size() == 2 && hardmins()[1].second == hm_param(6, 272), "one raise message, one unwind message");
    }
    SCEN("hold: restoreBad never releases") {
        ready_at(4);
        HoldRep rep{}; Decoded a{}; expect_u("raise", amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a), kHrOk); W.seen.clear();
        HoldRep rr{}; const uint32_t rel = amdgpu::dal_hold_release(gDev, true, &rr);
        expect(rel == kRlBadRestore && amdgpu::dal_hold_state() == kHsStuck && W.seen.empty() && W.did[0] == 0x21 && W.did[1] == 0x21, "restoreBad: STUCK, no message, clocks stay up");
        const n48n_dal_result &q = step(kE2); expect(q.verdict == kDenied && q.latched == 1, "DAL steps are latched after a STUCK hold");
        expect_u("the hold is not available again", amdgpu::dal_hold_pre(), kHpLatched);
    }
    SCEN("hold: the release does not bring the clocks down") {
        ready_at(4);
        HoldRep rep{}; Decoded a{}; expect_u("raise", amdgpu::dal_hold_raise(gDev, 514285, 500000, &rep, &a), kHrOk);
        W.restoreDidOverride = true; W.restoreDid = 0x21;   // the PMFW keeps the clocks up
        HoldRep rr{}; const uint32_t rel = amdgpu::dal_hold_release(gDev, false, &rr);
        expect(rel == kRlNotBack && amdgpu::dal_hold_state() == kHsStuck, "clocks still at 545 MHz after the release: STUCK (not a false RELEASED)");
    }
    SCEN("hold: refused while a DAL step runs") {
        world_reset(4);
        const n48n_dal_result &q0 = step(kE1b); expect_verdict("E1b", q0, kPass);   // (E2 is the step that runs during the hook: E1b clears the done flag while it runs)
        gSleepHook = hook_hold_during_step; gHookHold.clear(); W.seen.clear();
        const n48n_dal_result &q = step(kE2); gSleepHook = nullptr; expect_verdict("the step still passes", q, kPass);
        expect(gHookHold.size() >= 3 && gHookHold[0] == kHpOwner && gHookHold[1] == kHrPre && gHookHold[2] == kHpOwner, "a hold requested while a DAL step runs is refused (mailbox owner busy) and sends nothing of its own");
        expect(amdgpu::dal_hold_state() == kHsIdle && !amdgpu::dal_busy(), "the hold stayed IDLE; the step released the owner");
        for (auto &m : W.seen) expect(m.first != 9 || (m.second & 0xFFFF) != 530, "no 530 MHz SetHardMin during that E2 (the refused hold sent none)");
    }
}

static void b5_lock() {
    SCEN("lock basics") {
        // the PPSMC path takes the shared lock (before 0.0.604 it took none)
        world_reset(4); gDalShimLockCount = 0;
        uint32_t echo = 0; kern_return_t rc = amdgpu::smu_test_message(gDev, &echo);
        expect(rc == 0 && echo == 0xABCD0002u && gDalShimLockCount == 1 && W.ppsmcSeen.size() == 1, "a PPSMC message works and takes the shared lock once");
        gDalShimLockCount = 0; amdgpu::DalMsg m; rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m);
        expect(rc == 0 && gDalShimLockCount == 1, "a DAL message takes the shared lock once");
        // recursion: a holder can send both kinds without deadlock
        gDalShimLockCount = 0;
        { amdgpu::SmuSeq outer; expect(outer.held, "the shared lock is available"); amdgpu::SmuSeq inner; expect(inner.held, "recursive for the owner");
          rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m); expect(rc == 0, "a DAL send under a held lock (recursion)");
          rc = amdgpu::smu_test_message(gDev, &echo); expect(rc == 0, "a PPSMC send under a held lock (recursion)"); }
        expect_u("nested holders lock once (the recursion counts, it does not re-lock)", gDalShimLockCount, 1);
        { amdgpu::SmuSeq again; expect(again.held, "the lock is free again after the holders leave"); }
    }
    // mutual exclusion: while this thread holds the lock, another thread's DAL send / PPSMC send cannot complete
    for (int which = 0; which < 2; which++) SCEN("mutual exclusion") {
        world_reset(4); gLs.t2done = 0; gLs.t2started = 0; pthread_t t;
        { amdgpu::SmuSeq hold;
          pthread_create(&t, nullptr, which == 0 ? t2_dal : t2_ppsmc, nullptr);
          for (int i = 0; i < 200 && !gLs.t2started; i++) usleep(1000);
          usleep(60000);
          expect(gLs.t2started && !gLs.t2done, which == 0 ? "a DAL send waits while the lock is held elsewhere" : "a PPSMC send waits while the lock is held elsewhere"); }
        for (int i = 0; i < 2000 && !gLs.t2done; i++) usleep(1000);
        pthread_join(t, nullptr);
        expect(gLs.t2done, which == 0 ? "... and completes after the release (DAL)" : "... and completes after the release (PPSMC)");
    }
    // lock allocation failure: PPSMC runs unlocked as before 0.0.604; DAL refuses (its own child: the lock is allocated lazily)
    SCEN("lock allocation failure") {
        world_reset(4); gDalShimLockAllocFail = 1;
        uint32_t echo = 0; amdgpu::DalMsg m;
        kern_return_t rc = amdgpu::smu_test_message(gDev, &echo);
        expect(rc == 0 && W.ppsmcSeen.size() == 1, "no lock available: the PPSMC message still goes out (behaviour as before 0.0.604)");
        rc = amdgpu::smu_dal_send(gDev, kMsgGetSmuVersion, 0, &m);
        expect(rc == (kern_return_t)kIOReturnNoResources && m.refuse == kRefNoLock && W.seen.empty() && W.reg[kRegMsg] == 0, "no lock available: the DAL sender refuses and writes nothing");
        amdgpu::SmuSeq s; expect(!s.held, "SmuSeq reports that it holds nothing");
    }
}

// =====================================================================================================================================
// S: source pins
// =====================================================================================================================================
static std::string fn_body(const std::string &s, const std::string &head) {
    const size_t a = s.find(head); if (a == std::string::npos) return "";
    const size_t b = s.find("\n}\n", a); return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static void s_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string dal = slurp(K + "src/amd/smu_dal.cpp"), smu = slurp(K + "src/amd/smu_v14_0.cpp"), smuh = slurp(K + "src/amd/amdgpu_smu.h"), pure = slurp(K + "src/amd/smu_dal_pure.h");
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), engh = slurp(K + "src/amd/native_s1c.h"), cli = slurp(K + "src/Navi48NativeClient.cpp"), uc = slurp(K + "src/Navi48UserClient.cpp");
    const std::string abi = slurp(K + "src/Navi48NativeABI.h"), plist = slurp(K + "Info.plist"), mk = slurp(K + "Makefile"), doc = slurp(root + "/notes/design/NATIVE-S1C-ABI.md");
    const std::string tool = slurp(root + "/tools/native/n48dal.c"), bld = slurp(root + "/tools/native/build.sh"), dcnc = slurp(K + "src/dcn/navi48_dcn.cpp"), bringup = slurp(K + "src/Navi48Bringup.cpp");
    expect(!dal.empty() && !smu.empty() && !pure.empty() && !eng.empty() && !cli.empty() && !uc.empty() && !abi.empty() && !plist.empty() && !tool.empty() && !doc.empty(), "every source file the pins read exists (and the repo root argument is right)");
    // the sender writes exactly the three DAL dwords
    expect_u("smu_dal.cpp has exactly three WREG32 calls", count_of(dal, "WREG32("), 3);
    expect(dal.find("WREG32(dev, kRegResp, 0u);") != std::string::npos && dal.find("WREG32(dev, kRegArg, param);") != std::string::npos && dal.find("WREG32(dev, kRegMsg, msg);") != std::string::npos, "the three writes are RESP = 0, ARG = param, MSG = id");
    expect(dal.find("WREG32(dev, kRegResp, 0u);") < dal.find("WREG32(dev, kRegArg, param);") && dal.find("WREG32(dev, kRegArg, param);") < dal.find("WREG32(dev, kRegMsg, msg);"), "Linux's order: RESP, ARG, MSG");
    expect(dal.find("smu_send_msg") == std::string::npos && dal.find("WBAR0") == std::string::npos && dal.find("bar0_") == std::string::npos && dal.find("bar2") == std::string::npos, "smu_dal.cpp writes no other register, no VRAM, no doorbell, sends no PPSMC message");
    expect(count_of(dal, "RREG32(") >= 6, "the reads exist");
    // ordering inside the sender
    {
        const std::string b = fn_body(dal, "kern_return_t smu_dal_send(");
        const size_t pAllow = b.find("allow_check("), pLock = b.find("SmuSeq seq;"), pHeld = b.find("if (!seq.held)"), pPre = b.find("resp_pre_check(pre)"), pWr = b.find("WREG32(dev, kRegResp, 0u);");
        expect(pAllow != std::string::npos && pLock != std::string::npos && pHeld != std::string::npos && pPre != std::string::npos && pWr != std::string::npos, "the sender has every step");
        expect(pAllow < pLock && pLock < pHeld && pHeld < pPre && pPre < pWr, "allowlist, then the shared lock, then RESP == 0 pre-check, then the first register write");
        expect(b.find("RREG32(dev, kRegResp);") < pWr, "RESP is read before the first write");
        expect(b.find("IOSleep(1)") != std::string::npos && b.find("kRespWaitUs") != std::string::npos, "the RESP wait sleeps and is bounded by kRespWaitUs");
        expect(b.find("dal_gated_level()") != std::string::npos, "the sender gates on the native boot + boot-arg level");
    }
    // the gate
    {
        const std::string b = fn_body(dal, "uint32_t dal_gated_level()");
        expect(b.find("native_gate_ok()") != std::string::npos && b.find("v >= 1u && v <= kMaxLevel") != std::string::npos, "the gated level needs the native gate and a boot-arg in 1..4");
        expect(fn_body(dal, "static bool native_gate_ok()").find("kGateOn && s.positivePass") != std::string::npos, "the native gate is kGateOn AND S1b positivePass");
        expect(dal.find("\"navi48-dalsmc\"") != std::string::npos && count_of(dal, "PE_parse_boot_argn(") == 1, "the boot-arg is navi48-dalsmc, read in one place");
        expect(fn_body(dal, "static uint32_t boot_arg_level()").find("return 0u;") != std::string::npos, "an absent boot-arg is level 0");
    }
    // waits are bounded and sleep
    {
        const std::string b = fn_body(dal, "static uint32_t hard_min(");
        expect(b.find("kHardMinPollUs") != std::string::npos && b.find("IOSleep(1)") != std::string::npos && b.find("N48N_DAL_F_POLL_TIMEOUT") != std::string::npos && b.find("return 2u;") != std::string::npos && b.find("gRun.timedOut = true") == std::string::npos, "the 0x15 poll sleeps 1 ms, is bounded at kHardMinPollUs and reports a POLL timeout (return 2) without the mailbox-timeout flag");
        expect(pure.find("kRespWaitUs = 2000u * 1000u") != std::string::npos && pure.find("kHardMinPollUs = 1000u * 1000u") != std::string::npos, "the bounds are 2 s and 1 s");
        // every unbounded-looking loop in smu_dal.cpp has an IOSleep or a bound
        expect_u("smu_dal.cpp's loops: every `for (;;)` is one of the three bounded waits (the RESP wait, the 0x15 poll of hard_min, the hold's 0x15 poll)", count_of(dal, "for (;;)"), 3);
        expect(dal.find("IOSleep(2000)") != std::string::npos && dal.find("IOSleep(100)") != std::string::npos && dal.find("IOSleep(5)") != std::string::npos, "the reference window, the dwell tick and the settle polls sleep");
        expect(dal.find("kStallMs") != std::string::npos && dal.find("rate_ok(") != std::string::npos && dal.find("judge_dwell(") != std::string::npos, "the dwell checks stall, rate and the clock thresholds");
    }
    // the step runner: latch, gate, restore, E1b enabling
    {
        const std::string b = fn_body(dal, "IOReturn dal_run_step(");
        expect(b.find("pre_gate(native_gate_ok(), gated, step, gDal.e1bOk, gDal.latch.stopped, busy)") != std::string::npos, "the pre-gate reads the gate, the level, E1b and the latch");
        expect(b.find("gDal.latch = latch_apply(gDal.latch, step, gRun.verdict);") != std::string::npos, "the latch is applied to every verdict");
        expect(b.find("owner_cas(kOwnerIdle, kOwnerDal)") != std::string::npos && count_of(b, "owner_cas(kOwnerDal, kOwnerIdle)") == 2, "one step at a time (owner word 0 -> 1); the owner is released on the deny path and at the end");
        expect(b.find("if (gRun.verdict == kVNone) gRun.verdict = kGlitch;") != std::string::npos, "a path that decides nothing is not a pass");
        expect(b.find("N48N_DAL_F_UNDERFLOW_UNREAD") != std::string::npos && b.find("N48N_DAL_UNDERFLOW_NA") != std::string::npos, "the underflow gap is reported in every result");
        expect(b.find("gDal.e1bOk = false;") != std::string::npos, "a re-run of E1b clears the done flag first");
        const std::string r = fn_body(dal, "static void step_raise(");
        expect(r.find("plan.raise[i].clk, plan.raise[i].mhz, true") != std::string::npos && r.find("for (uint32_t i = plan.nRaise; i-- > 0u; ) order[n++] = plan.raise[i].clk;") != std::string::npos, "raise in plan order, restore in reverse");
        expect(r.find("if (gRun.timedOut && gRun.hardMinSent) {") != std::string::npos && r.find("RESTORE NOT ATTEMPTED") != std::string::npos, "no restore after a timeout");
        expect(r.find("kRestoreMhz, false") != std::string::npos && r.find("(mask & (1u << clk)) == 0u") != std::string::npos && r.find("restore_mask(gRun.raisedMask, b0, zb)") != std::string::npos && r.find("mask != 0u && !gRun.timedOut") != std::string::npos, "the restore covers the raised clocks and every clock below its start, at kRestoreMhz, unless the mailbox timed out");
        expect(r.find("frame_now(") != std::string::npos && r.find("scanQuery") != std::string::npos && r.find("kDenyFrames") != std::string::npos && r.find("N48N_SCANQ_ACQUIRED") != std::string::npos && r.find("kDenyAcquired") != std::string::npos, "the pre-flight reads the OTG through the scanout status code and refuses an acquired plane");
        const std::string e = fn_body(dal, "static void step_e1b(");
        expect(e.find("level_count(arg)") != std::string::npos && e.find("level_mhz(arg)") != std::string::npos && e.find("norm_mhz(arg)") != std::string::npos && count_of(e, "norm_mhz(") == 1 && e.find("if (clk == kClkDispclk) {") != std::string::npos && e.find("dal_msg_soft(kMsgGetDcModeMaxDpmFreq") != std::string::npos, "E1b reads the level count as Linux does, masks levels to 16 bits, asks 0xC for DISPCLK only and softly");
        expect(e.find("gDal.max[0] = mx[0]; gDal.max[1] = mx[1]; gDal.e1bOk = true;") != std::string::npos && count_of(dal, "gDal.max[0] =") == 1, "the DPM maxima are recorded in one place, by E1b");
    }
    // who calls what
    {
        size_t senders = 0; std::string other;
        const char *files[] = { "src/Navi48Bringup.cpp", "src/Navi48UserClient.cpp", "src/Navi48NativeClient.cpp", "src/amd/native_s1c.cpp", "src/dcn/navi48_dcn.cpp", "src/apple/Navi48Ttl.cpp", "src/amd/amdgpu_init.cpp", "src/amd/smu_v14_0.cpp" };
        for (const char *f : files) { const std::string t = slurp(K + f); senders += count_of(t, "smu_dal_send("); if (t.find("dal_run_step(") != std::string::npos && std::string(f) != "src/amd/native_s1c.cpp") other = f; }
        expect_u("nobody outside smu_dal.cpp calls smu_dal_send", senders, 0);
        expect(other.empty(), "dal_run_step is called only from native_s1c.cpp");
        expect_u("dal_run_step has one caller (n1c_dal_step)", count_of(eng, "dal_run_step("), 1);
        expect_u("n1c_dal_step has one caller (the N48N client)", count_of(cli, "n1c_dal_step("), 1);
        expect(bringup.find("smu_dal") == std::string::npos && bringup.find("dal_run_step") == std::string::npos && dcnc.find("smu_dal_send") == std::string::npos && dcnc.find("dal_run_step") == std::string::npos && count_of(dcnc, "smu_dal") == 1 && dcnc.find("#include \"amd/smu_dal.h\"") != std::string::npos && count_of(dcnc, "dal_busy()") == 2 && count_of(dcnc, "amdgpu::dal_hold_") == 6, "the interrupt handler's file and the display layer never call the DAL sender (0.0.605: the display layer includes smu_dal.h for the read-only dal_busy() only; 0.0.607 adds the four clock-hold wrappers mt_hold_pre / raise / release / state, plus the emergency-restore log line reading the state)");
        expect(cli.find("case N48N_SEL_DAL_STEP:") != std::string::npos && cli.find("if (!shape(2, 0, 0, sizeof(n48n_dal_result))) return kIOReturnBadArgument;") != std::string::npos, "selector 15: exactly two scalars in, a 256 B struct out");
        const std::string nb = fn_body(eng, "IOReturn n1c_dal_step(");
        expect(nb.find("if (!sess_hello()) return kIOReturnNotReady;") != std::string::npos && nb.find("flags != 0ull") != std::string::npos && nb.find("IOLockLock") == std::string::npos, "n1c_dal_step: Hello first, flags 0, and no client lock held across the step");
    }
    // lock sharing
    {
        const std::string b = fn_body(smu, "smu_send_msg_with_param(const DeviceContext &dev,\n                        uint32_t msgId, uint32_t param,\n                        uint32_t *outReturn)");
        expect(b.find("SmuSeq seq;") != std::string::npos && b.find("smu_send_msg_with_param_body(") != std::string::npos && b.find("SmuSeq seq;") < b.find("smu_send_msg_with_param_body("), "the public PPSMC entry takes the shared lock and then calls the untouched body");
        expect(smu.find("static __attribute__((noinline)) kern_return_t\nsmu_send_msg_with_param_body(") != std::string::npos, "the body is a separate noinline function (its disassembly is proved identical to the base)");
        expect(smuh.find("struct SmuSeq") != std::string::npos && smuh.find("bool smu_lock_enter();") != std::string::npos && smu.find("gSmuOwner") != std::string::npos && smu.find("gSmuDepth++") != std::string::npos, "the shared lock is recursive for its owner");
        expect(fn_body(uc, "IOReturn Navi48UserClient::doMetrics(").find("amdgpu::SmuSeq smuSeq;") != std::string::npos && fn_body(uc, "IOReturn Navi48UserClient::doMetrics(").find("amdgpu::SmuSeq smuSeq;") < fn_body(uc, "IOReturn Navi48UserClient::doMetrics(").find("amdgpu::smu_read_metrics("), "doMetrics holds the shared lock across smu_read_metrics");
        expect(fn_body(uc, "IOReturn Navi48UserClient::doPowerState(").find("amdgpu::SmuSeq smuSeq;") != std::string::npos && fn_body(uc, "IOReturn Navi48UserClient::doPowerState(").find("amdgpu::SmuSeq smuSeq;") < fn_body(uc, "IOReturn Navi48UserClient::doPowerState(").find("amdgpu::smu_set_power_state("), "doPowerState holds the shared lock across smu_set_power_state");
        expect(dal.find("#include \"amdgpu_smu.h\"") != std::string::npos, "the DAL sender shares the lock through amdgpu_smu.h");
    }
    // the ABI
    expect_u("N48N_SEL_DAL_STEP", N48N_SEL_DAL_STEP, 15); expect_u("selector count 1.2", N48N_SEL_COUNT_1_2, 16); expect_u("v1.1 count unchanged", N48N_SEL_COUNT_1_1, 15); expect_u("v1.0 count unchanged", N48N_SEL_COUNT, 9);
    expect_u("ABI major unchanged", N48N_ABI_VERSION, 1); expect_u("ABI minor 9 (1.9 since 0.0.612: BoImportHost; 1.8 since 0.0.610: the Metal nub selectors; 1.7 since 0.0.609; 1.6 was 0.0.608; 1.5 was 0.0.607; 1.4 grew the mode-trial result, the mode-trial selector arrived in 1.3, the DAL step selector in 1.2)", N48N_ABI_MINOR, 9);
    expect_u("result size", sizeof(n48n_dal_result), 256);
    expect(N48N_DAL_V_PASS == kPass && N48N_DAL_V_REFUSED == kRefused && N48N_DAL_V_TIMEOUT == kTimeout && N48N_DAL_V_REGRESSED == kRegressed && N48N_DAL_V_GLITCH == kGlitch && N48N_DAL_V_DENIED == kDenied && N48N_DAL_V_SHORT == kShort, "the ABI verdict values are the pure header's");
    expect(N48N_DAL_STEP_E1B == kE1b && N48N_DAL_STEP_E2 == kE2 && N48N_DAL_STEP_E3 == kE3 && N48N_DAL_STEP_E4 == kE4, "the ABI step numbers are the pure header's");
    expect(doc.find("## ABI 1.2 addendum") != std::string::npos && doc.find("N48N_SEL_DAL_STEP") != std::string::npos && doc.find("navi48-dalsmc") != std::string::npos && doc.find("n48dal e1b") != std::string::npos, "the contract has an ABI 1.2 addendum that names the selector, the boot-arg and the tool");
    expect(doc.find("the temperature stop rule (\"temps > E1 + 5 C\") is NOT implemented") != std::string::npos && doc.find("DISPCLK first, then DPPCLK**") != std::string::npos && doc.find("`dc_max_resp`") != std::string::npos && doc.find("`POLL_TIMEOUT`") != std::string::npos && doc.find("`ABOVE_START`") != std::string::npos,
           "the addendum documents the temperature deviation, the E4 order, dc_max_resp, POLL_TIMEOUT and AT_START / ABOVE_START");
    {   // the deny / refuse codes are distinct and the tool names the acquired denial
        const uint32_t d[] = { kDenyLatched, kDenyGate, kDenyLevel, kDenyNoE1b, kDenyBusy, kDenyBadStep, kDenyLevelArg, kDenyBaseline, kDenyFrames, kDenyAcquired };
        bool distinct = true; for (size_t i = 0; i < 10; i++) for (size_t j = i + 1; j < 10; j++) if (d[i] == d[j]) distinct = false;
        expect(distinct && kDenyAcquired == 10 && kDenyFrames == 9, "every deny code is distinct (acquired = 10)");
        expect(tool.find("case 10: return \"the scanout plane is acquired") != std::string::npos, "the tool names deny 10");
    }
    // version
    {
        const size_t p = engh.find("kN1cKextBuild = "); const int build = p == std::string::npos ? -1 : std::atoi(engh.c_str() + p + 16);
        const size_t v = plist.find("<string>0.0."); const int ver = v == std::string::npos ? -2 : std::atoi(plist.c_str() + v + 12);
        expect(build > 0 && build == ver, "kN1cKextBuild equals the Info.plist patch version"); expect_u("Info.plist is 0.0.620", (uint64_t)ver, 620); expect_u("the plist carries the version twice", count_of(plist, "0.0.620"), 2);
    }
    expect(mk.find("$(wildcard src/amd/*.cpp)") != std::string::npos, "the Makefile builds src/amd/*.cpp (smu_dal.cpp)");
    // the tool
    expect(tool.find("N48N_SEL_DAL_STEP") != std::string::npos && tool.find("e1b") != std::string::npos && tool.find("e4") != std::string::npos && tool.find("N48N_HELLO_F_MINOR") != std::string::npos, "n48dal drives the selector, needs the minor");
    expect(bld.find("n48dal:n48dal.c") != std::string::npos, "build.sh builds n48dal");
    // banned strings in the new code
    for (const std::string *t : { &dal, &pure }) expect(t->find("+0x280") == std::string::npos && t->find("+0x282") == std::string::npos && t->find("+0x299") == std::string::npos, "the banned pipe offsets do not appear");
    expect(pure.find("#include <stdint.h>") != std::string::npos && pure.find("#include <IOKit") == std::string::npos, "the pure header includes stdint only");
    expect(dal.find("std::") == std::string::npos, "no C++ library use in the kernel file");
}

// =====================================================================================================================================
int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    p1_did(); p2_allow(); p3_rules(); h1_state_machine(); h2_raise_release(); s_pins(root);
    const int pure = gRun, pureFail = gFail;
    std::printf("native_s2_dal_test: pure + source pins: %d checks, %d failed\n", pure, pureFail);
    b1_sender(); b2_e1b(); b3_e2(); b3_failures(); b4_latch_busy(); b6_hold(); b5_lock();
    std::printf("native_s2_dal_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
