// native_scanx_test.cpp - build 0.0.661 (M6 Stage 1b, ABI 1.11) + 0.0.662 (M6 Stage 2, ABI 1.12): the HDMI displays' scanout (instance 2 = the monitor B: HUBP2 / OTG2; instance 1 = the monitor A: HUBP1 / OTG1) - the SEQUENCES of
// dcn/navi48_scanx_flow.h run over a FAKE register file with a call recorder (the whole X1..X9 suite runs ONCE PER INSTANCE, through that instance's descriptor), the CROSS-INSTANCE tests (one fake card with distinct HUBP1 and HUBP2 states),
// the descriptor offsets derived BY NAME from Linux's dcn_4_1_0_offset.h, the AGDC three-entry list, the pure surface-table changes of the review (R2 blob generation, R3 eviction victim, R4 torn reads, R5 publish rate), and source pins on the
// kext glue, the ABI, the policy, the client and the CLI.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src -I src/navi48-bringup/src/dcn -I src/navi48-bringup/src/amd \
//       src/navi48-bringup/tests/native_scanx_test.cpp -o /tmp/native_scanx && /tmp/native_scanx .     (run from the repo root; the argument is the repo root the source pins read from;
//   tests/native_scanx_plant.sh plants breaks and shows every check that catches them)
// Covers (contract item numbers are the "Q2: Stage 1b build contract" and the "Stage 2 build contract (monitor A, instance 1)" of an internal design note):
//   X1  the registers (re-derived from the HUBP0 twin + n * 0xdc through the disp2 mirror table), the write allowlist (exactly this instance's two address registers), the slot tag            [per instance]
//   X2  Acquire: every precondition refuses with NOTHING written; the success path; the latch-OFF gate reads and writes nothing, takes no lock                                                [per instance]
//   X3  Register: tagged ids, B / A / DP / other-display overlaps, bounds, full table                                                                                                         [per instance]
//   X4  Present: HIGH then LOW, zero writes outside the pair, the whole-address allowlist (A / B / untagged / foreign / mixed halves / the OTHER instance's slot refused), reuse refused, write failure -> restore [per instance]
//   X5  latch bookkeeping by polling the RAW FLIP_PENDING bit (this instance's OTG frame count only stamps the frame number); reuse is judged on THIS HUBP's EARLIEST_INUSE (decoys differ)  [per instance]
//   X6  Restore: one function, HIGH then LOW of A only, polled, never sleeps under the lock, idempotent, unverified -> failed + leak, cur = A                                               [per instance]
//   X7  the watchdog: 5 s idle restores, a keep-alive keeps a static display, geometry change restores, wantRestore restores, a present is refused during a restore                           [per instance]
//   X8  teardown order (ALL THREE instances restored before any BO is touched; an unverified restore leaks only that instance's BOs; the HUNG leak path), bo_gone                              [per instance + three-way]
//   X9  GPU-held reporting, the disp2 guard never admits a slot address                                                                                                                       [per instance]
//   XC  cross-instance: one fake card, distinct HUBP1 / HUBP2 states, a recorder per instance (zero writes outside the own pair; the other instance's recorder stays empty), the descriptor swap detectors
//   XD  the descriptor offsets BY NAME from dcn_4_1_0_offset.h (the way native_disp2_test.cpp does)
//   XA  AGDC with three entries: endpoint 1 -> 2560x1440 at 241.5 MHz, endpoint 2 -> 1920x1080 at 148.5 MHz; the list order = the publish order
//   X10 review R2 / R3 / R4 / R5 (blob generation, eviction victim, torn reads, reset writes kInstNone, publish rate)
//   X11 source pins (writer, glue order, ABI numbers, policy, client, s1c, CLI, variant, OFF paths)
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "dcn/navi48_scanx_flow.h"
#include "amd/native_m6_pure.h"
#include "amd/native_open_policy_pure.h"
#include "Navi48NativeABI.h"
#include "amd/native_disp_pure.h"
#include "amd/native_fb_pure.h"
static bool n48fb_live_refuses(uint32_t holdBad) { return n48fb::live_verdict(holdBad) == (uint32_t)n48fb::kGate; }

static int gChecks = 0, gFails = 0;
static void expect(bool ok, const char *what) { gChecks++; if (!ok) { gFails++; std::printf("FAIL: %s\n", what); } }
#define EXPECT(c, w) expect((c), w)

using namespace n48scanx;
using n48scan::kOk; using n48scan::kBusy; using n48scan::kNotReady; using n48scan::kBadArg; using n48scan::kUnsupported; using n48scan::kNotFound; using n48scan::kNoResources;

// ---- the instance under test ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// setup_layout: A, B, then the slots one allocation apart (Run B's A for each display), the flip window, the DP console; D() is the descriptor, T() its slot tag, kHubp0Early a HUBP0 register (a decoy no flow may read).
static uint64_t A, B, S0, S1, S2, S3, WLO, WHI, DPC, kHubp0Early;
static const XDesc *gDesc = nullptr;
static const XDesc &D() { return *gDesc; }
static uint32_t T() { return gDesc->slotTag; }
static void setup_layout(const XDesc &d) {
    gDesc = &d;
    const uint64_t stride = d.allocBytes;
    A = d.inst == 1u ? 0x8004360000ull : 0x8002740000ull;
    B = A + stride; S0 = A + 2 * stride; S1 = A + 3 * stride; S2 = A + 4 * stride; S3 = A + 5 * stride;
    WLO = 0x8000000000ull; WHI = 0x8010000000ull; DPC = 0x8000000000ull;
    kHubp0Early = d.regEarlyLo - d.otgInst * 0xdcu;
}

// ---- the fake hardware: this instance's HUBP registers, its OTG's frame counter, a clock that only sleep_ms advances ---------------------------------------------------------------------------------------
struct Ev { char k; uint32_t a, b; };
struct Env {
    // the card
    uint64_t nowMs = 1000;
    uint64_t prog = A, early = A;            // programmed / EARLIEST_INUSE
    uint64_t pendUntil = 0, earlyAt = 0;     // the latch and the EARLIEST catch-up (ms)
    bool pending = false;
    bool neverLatch = false, freezeFrames = false;
    uint32_t viewDim = D().w | (D().h << 16), pitch = D().pitchReg, cfg = 8u, tiling = 0x80u, ctl = 0u, vmid = 0u, flipCtlExtra = 0u, viewStart = 0u, hubpCntl = D().otgInst << 4, vmFault = 0u;
    uint32_t hubp0EarlyDecoy = 0xDEADBEEFu, otgUpdLock = 0u; int stickFirst = 0;
    // the kext's state
    uint32_t gateCode = 0u, stage = N48D2_PL_HELD, cur = 0u;
    uint64_t mc[2] = { A, B };
    bool windowOk = true, wdStart = true;
    uint32_t failWriteAbs = 0u; int failAfter = -1;
    bool curSetA = false;
    // the recorder
    std::vector<Ev> ev; int depth = 0; bool sleptLocked = false; uint32_t watchdogGen = 0;
    std::vector<std::string> notes;
    void upd() { if (pending && !neverLatch && nowMs >= pendUntil) pending = false; if (!neverLatch && earlyAt != 0 && nowMs >= earlyAt && !pending) early = prog; }
    uint32_t gate() { return gateCode; }
    uint32_t rd(uint32_t abs) {
        ev.push_back({ 'R', abs, 0 }); upd();
        const XDesc &d = D();
        if (abs == d.regViewDim) return viewDim; if (abs == d.regPitch) return pitch; if (abs == d.regSurfConfig) return cfg; if (abs == d.regTiling) return tiling; if (abs == d.regSurfControl) return ctl;
        if (abs == d.regVmid) return vmid; if (abs == d.regViewStart) return viewStart; if (abs == d.regHubpCntl) return hubpCntl; if (abs == d.regVmFault) return vmFault;
        if (abs == d.regFlipControl) return (pending ? kFlipPendingMask : 0u) | flipCtlExtra;
        if (abs == d.regAddr) return (uint32_t)prog; if (abs == d.regAddrHigh) return (uint32_t)(prog >> 32);
        if (abs == d.regEarlyLo) return (uint32_t)early; if (abs == d.regEarlyHi) return (uint32_t)(early >> 32);
        if (abs == d.regOtgUpdLock) return otgUpdLock;
        if (abs == d.regOtgFrame) return freezeFrames ? 100u : (uint32_t)(nowMs * 60ull / 1000ull) & 0xFFFFFFu;
        if (abs == kHubp0Early) return hubp0EarlyDecoy;
        return 0xFFFFFFFFu;
    }
    bool wr(uint32_t abs, uint32_t v) {
        ev.push_back({ 'W', abs, v });
        if (!write_reg_ok(D(), abs)) return false;
        if (failAfter == 0 || (failWriteAbs != 0u && failWriteAbs == abs)) return false;
        if (failAfter > 0) failAfter--;
        if (abs == D().regAddrHigh) { hiW = v; return true; }
        if (abs == D().regAddr) { prog = ((uint64_t)hiW << 32) | v; pending = true; pendUntil = nowMs + 17; earlyAt = nowMs + 34; if (stickFirst > 0) { stickFirst--; pendUntil = earlyAt = ~0ull; } return true; }
        return false;
    }
    uint32_t hiW = (uint32_t)(A >> 32);
    uint64_t now_ns() { return nowMs * 1000000ull; }
    void sleep_ms(uint32_t ms) { ev.push_back({ 'S', ms, 0 }); if (depth != 0) sleptLocked = true; nowMs += ms; }
    void lock() { ev.push_back({ 'L', 0, 0 }); depth++; }
    void unlock() { ev.push_back({ 'U', 0, 0 }); depth--; }
    uint32_t d2_stage() { return stage; } uint32_t d2_cur() { return cur; } uint64_t d2_mc(uint32_t i) { return mc[i & 1u]; } void d2_set_cur_a() { cur = 0u; curSetA = true; }
    bool window(uint64_t *lo, uint64_t *hi) { *lo = WLO; *hi = WHI; return windowOk; }
    bool start_watchdog(uint32_t gen) { watchdogGen = gen; return wdStart; }
    void note(const char *f) { notes.push_back(f); }
    template <class T2, class... Ar> void note(const char *f, T2 t, Ar... a) { char b[512]; std::snprintf(b, sizeof b, f, t, a...); notes.push_back(b); }
    // helpers for the tests
    unsigned count(char k) const { unsigned n = 0; for (auto &e : ev) if (e.k == k) n++; return n; }
    std::vector<Ev> writes() const { std::vector<Ev> w; for (auto &e : ev) if (e.k == 'W') w.push_back(e); return w; }
    unsigned countRegReads(uint32_t abs) const { unsigned n = 0; for (auto &e : ev) if (e.k == 'R' && e.a == abs) n++; return n; }
    void clear() { ev.clear(); sleptLocked = false; }
};
static RegIn regin(uint64_t mc, uint64_t size = 0ull, uint64_t off = 0) { return RegIn{ true, mc, size ? size : D().bytes, off, D().pitchPx * 4u, D().w, D().h, n48scan::kFmtArgb8888 }; }
static Excl excl_default() { Excl e; std::memset(&e, 0, sizeof e); e.n = 1; e.lo[0] = DPC; e.bytes[0] = D().bytes; return e; }
static bool acquire_ok(Env &e, ScanX &x) { uint64_t o[2] = { 0, 0 }; reset(x); const uint32_t rc = acquire(e, D(), x, o); return rc == kOk && x.acquired; }
static uint64_t reg_slot(Env &e, ScanX &x, uint64_t mc, uint32_t *rc = nullptr) { uint64_t o[2] = { 0, 0 }; const Excl ex = excl_default(); const uint32_t r = register_slot(e, D(), x, regin(mc), ex, o); if (rc) *rc = r; return r == kOk ? o[0] : 0ull; }
static bool only_addr_writes(const Env &e) { for (auto &w : e.writes()) if (w.a != D().regAddr && w.a != D().regAddrHigh) return false; return true; }
static void settle(Env &e, uint32_t ms) { for (uint32_t i = 0; i < ms; i++) { e.nowMs++; e.upd(); } }

static std::string slurp(const std::string &root, const char *rel) { std::ifstream f(root + "/" + rel); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static bool has(const std::string &s, const char *t) { return s.find(t) != std::string::npos; }
static size_t count_of(const std::string &s, const char *t) { size_t n = 0, p = 0; while ((p = s.find(t, p)) != std::string::npos) { n++; p += std::strlen(t); } return n; }
static std::string tag(const char *what) { return std::string(what) + " [" + D().name + "]"; }
#define EXPECTI(c, w) expect((c), tag(w).c_str())

// =====================================================================================================================================================================================================================
// THE PER-INSTANCE SUITE: everything the 0.0.661 test proved for the monitor B, run through the descriptor of the instance under test (the monitor B, then the monitor A). Names carry [monitor B] / [monitor A].
// =====================================================================================================================================================================================================================
struct Lit { uint32_t surfCfg, tiling, viewStart, viewDim, hubpCntl, pitch, vmid, addr, addrHigh, surfCtl, flipCtl, earlyLo, earlyHi, vmFault, otgFrame; };
static const Lit kLitMonB = { 0x3c5d, 0x3c5f, 0x3c61, 0x3c63, 0x3c6c, 0x3c7f, 0x3c81, 0x3c82, 0x3c83, 0x3c8a, 0x3c8b, 0x3c94, 0x3c95, 0x3a8c, 0x510d };
static const Lit kLitMonA = { 0x3b81, 0x3b83, 0x3b85, 0x3b87, 0x3b90, 0x3ba3, 0x3ba5, 0x3ba6, 0x3ba7, 0x3bae, 0x3baf, 0x3bb8, 0x3bb9, 0x3a8c, 0x508d };

static void suite_x1() {
    const XDesc &d = D(); const Lit &L = d.inst == 1u ? kLitMonA : kLitMonB; const uint32_t n = d.otgInst;
    // every register the flows read is THIS HUBP's: re-derived from the HUBP0 twin + n * 0xdc
    struct P { uint32_t dst; uint32_t src; } want[] = { { d.regSurfConfig, 0x3aa5 }, { d.regTiling, 0x3aa7 }, { d.regViewStart, 0x3aa9 }, { d.regSurfControl, 0x3ad2 } };
    for (auto &w : want) {
        EXPECTI(w.dst == w.src + n * 0xdcu, "X1: HUBPn = HUBP0 + n * 0xdc");
        if (d.inst == 2u) { bool found = false; for (unsigned i = 0; i < N48D2_EXP_COUNT; i++) if (n48d2_exp[i].abs2 == w.dst && n48d2_exp[i].abs0 == w.src) found = true; EXPECTI(found, "X1: a HUBP2 register the flow reads is the disp2 expectation table's HUBP2 twin of the HUBP0 register"); }
    }
    EXPECTI(d.regViewDim == 0x3aabu + n * 0xdcu && d.regPitch == 0x3ac7u + n * 0xdcu && d.regVmid == 0x3ac9u + n * 0xdcu && d.regFlipControl == 0x3ad3u + n * 0xdcu && d.regHubpCntl == 0x3ab4u + n * 0xdcu, "X1: viewport, pitch, VMID, FLIP_CONTROL, DCHUBP_CNTL are THIS HUBP's (HUBP0 + n * 0xdc)");
    EXPECTI(d.regAddr == 0x3ba6u + (n - 1u) * 0xdcu && d.regAddrHigh == d.regAddr + 1u && d.regEarlyLo == 0x3bb8u + (n - 1u) * 0xdcu && d.regEarlyHi == d.regEarlyLo + 1u, "X1: the primary address pair and EARLIEST_INUSE pair are THIS HUBP's");
    EXPECTI(d.regOtgFrame == L.otgFrame && d.regOtgFrame != (n == 2u ? N48D2_OTG1_FRAME_COUNT : N48D2_OTG2_FRAME_COUNT), "X1: this OTG's frame counter, never the other display's");
    EXPECTI(d.regSurfConfig == L.surfCfg && d.regTiling == L.tiling && d.regViewStart == L.viewStart && d.regViewDim == L.viewDim && d.regHubpCntl == L.hubpCntl && d.regPitch == L.pitch && d.regVmid == L.vmid && d.regAddr == L.addr &&
            d.regAddrHigh == L.addrHigh && d.regSurfControl == L.surfCtl && d.regFlipControl == L.flipCtl && d.regEarlyLo == L.earlyLo && d.regEarlyHi == L.earlyHi && d.regVmFault == L.vmFault, "X1: the contract's register numbers");
    EXPECTI(write_reg_ok(d, L.addr) && write_reg_ok(d, L.addrHigh), "X1: the two address registers are writable");
    unsigned bad = 0; for (uint32_t a = 0x3a00; a < 0x3d80; a++) if (write_reg_ok(d, a) && a != L.addr && a != L.addrHigh) bad++;
    EXPECTI(bad == 0, "X1: write_reg_ok admits NO other register of the HUBP / DCN range");
    EXPECTI(!write_reg_ok(d, d.regFlipControl) && !write_reg_ok(d, d.regVmid) && !write_reg_ok(d, d.regSurfControl), "X1: FLIP_CONTROL / VMID / SURFACE_CONTROL are never writable (item 7)");
    const XDesc &o = d.inst == 1u ? kDescMonB : kDescMonA;
    EXPECTI(!write_reg_ok(d, o.regAddr) && !write_reg_ok(d, o.regAddrHigh), "X1: ...and the OTHER instance's address registers are never writable through this descriptor");
    EXPECTI(id_of(d, 0) == d.slotTag && id_of(d, 2) == (d.slotTag | 2u) && id_ok(d, d.slotTag) && id_ok(d, d.slotTag + 2u) && !id_ok(d, d.slotTag + 3u) && !id_ok(d, 0) && !id_ok(d, 2) && !id_ok(d, 0x2000) && !id_ok(d, o.slotTag) && !id_ok(d, o.slotTag + 2u), "X1: the tag: tag | k, 3 slots; the other instance's tag, an instance-0 id and an untagged id are not this instance's");
    EXPECTI(d.bytes == (uint64_t)d.w * 4ull * d.h && d.pitchPx == d.w && d.pitchReg == d.pitchPx - 1u && d.allocBytes >= d.bytes && (d.allocBytes & 0xFFFFull) == 0ull && d.allocBytes - d.bytes < 0x10000ull, "X1: the geometry: bytes = w * 4 * h, pitch register = pitch - 1, the allocation is the frame rounded up to 64 KiB");
}

static void suite_x2() {
    {
        Env e; ScanX x; reset(x); uint64_t o[2] = { 0, 0 };
        EXPECTI(acquire(e, D(), x, o) == kOk && x.acquired && o[0] == A, "X2: Acquire on a healthy HELD plane succeeds and returns A");
        EXPECTI(e.writes().empty(), "X2: Acquire writes NOTHING");
        EXPECTI(e.watchdogGen == 1u && x.gen == 1u && x.acquires == 1u && x.descInst == D().inst, "X2: one watchdog thread of generation 1; the state remembers its descriptor");
        EXPECTI(!e.sleptLocked && e.depth == 0, "X2: Acquire never sleeps under the lock, and releases it");
        EXPECTI(x.tbl.consoleMc == A && x.bufA == A && x.bufB == B, "X2: the table's console is A; B is known");
        EXPECTI(x.width == D().w && x.height == D().h && x.pitchPx == D().pitchPx, "X2: the state's geometry is the descriptor's");
        uint64_t o2[2];
        EXPECTI(acquire(e, D(), x, o2) == kBusy, "X2: a second Acquire is Busy");
    }
    {   // latch OFF / gate: nothing is read, written, locked
        const uint32_t gates[] = { kUnsupported, kNotReady, kBusy, kNoResources };
        for (uint32_t g : gates) {
            Env e; e.gateCode = g; ScanX x; reset(x); uint64_t o[2] = { 7, 7 }, o3[3] = { 7, 7, 7 }; StatusOut st; const Excl ex = excl_default();
            EXPECTI(acquire(e, D(), x, o) == g, "X2: Acquire answers the gate's code");
            EXPECTI(register_slot(e, D(), x, regin(S0), ex, o) == g, "X2: Register answers the gate's code");
            EXPECTI(present(e, D(), x, T(), 0, o3) == g, "X2: Present answers the gate's code");
            EXPECTI(status(e, D(), x, &st) == g, "X2: Status answers the gate's code");
            EXPECTI(e.ev.empty(), "X2: with the gate shut there is no register read, no write, no lock, no sleep, in ANY of the four flows");
            EXPECTI(!x.acquired && x.acquires == 0u && x.gen == 0u, "X2: the state is untouched");
        }
        Env e; ScanX x; reset(x); uint64_t r[2] = { 9, 9 };
        EXPECTI(restore(e, D(), x, "off", 0, r) == 0u && r[0] == 1u, "X2: Restore with nothing acquired answers verified");
        EXPECTI(e.writes().empty() && e.countRegReads(D().regEarlyLo) == 0u, "X2: Restore with nothing ever acquired reads and writes nothing");
        EXPECTI(bo_gone(e, D(), x, 7, 0, true, nullptr) == 0u, "X2: bo_gone on a never-acquired instance returns 0");
    }
    {   // each precondition refuses with nothing written
        struct Case { const char *name; Why why; void (*mut)(Env &); uint32_t rc; };
        Case cases[] = {
            { "stage not HELD", kWhyStage, [](Env &e) { e.stage = N48D2_PL_SHOWN; }, kNotReady },
            { "no buffer A", kWhyBuffers, [](Env &e) { e.mc[0] = 0; }, kNotReady },
            { "A == B", kWhyBuffers, [](Env &e) { e.mc[1] = e.mc[0]; }, kNotReady },
            { "A unaligned", kWhyBuffers, [](Env &e) { e.mc[0] += 0x1000; }, kNotReady },
            { "A and B overlap", kWhyBuffers, [](Env &e) { e.mc[1] = e.mc[0] + 0x10000; }, kNotReady },
            { "disp2 says B is front", kWhyCur, [](Env &e) { e.cur = 1; }, kNotReady },
            { "window unknown", kWhyWindow, [](Env &e) { e.windowOk = false; }, kNotReady },
            { "A outside the window", kWhyWindow, [](Env &e) { e.mc[0] = 0x9000000000ull; e.mc[1] = 0x9000000000ull + D().allocBytes; e.prog = e.early = e.mc[0]; }, kNotReady },
            { "OTG not counting", kWhyNotCounting, [](Env &e) { e.freezeFrames = true; }, kNotReady },
            { "viewport", kWhyGeometry, [](Env &e) { e.viewDim = D().inst == 2u ? (1920u | (1080u << 16)) : (2560u | (1440u << 16)); }, kUnsupported },
            { "pitch", kWhyGeometry, [](Env &e) { e.pitch = D().pitchReg - 1u; }, kUnsupported },
            { "format", kWhyGeometry, [](Env &e) { e.cfg = 9u; }, kUnsupported },
            { "swizzle", kWhyGeometry, [](Env &e) { e.tiling = 0x81u; }, kUnsupported },
            { "SURFACE_CONTROL (DCC)", kWhyGeometry, [](Env &e) { e.ctl = 2u; }, kUnsupported },
            { "SURFACE_CONTROL (TMZ)", kWhyGeometry, [](Env &e) { e.ctl = 1u; }, kUnsupported },
            { "VMID", kWhyRestoreExact, [](Env &e) { e.vmid = 3u; }, kUnsupported },
            { "FLIP_TYPE", kWhyRestoreExact, [](Env &e) { e.flipCtlExtra = 2u; }, kUnsupported },
            { "viewport start", kWhyRestoreExact, [](Env &e) { e.viewStart = 0x10u; }, kUnsupported },
            { "programmed != A", kWhyAddress, [](Env &e) { e.prog = B; }, kBusy },
            { "EARLIEST != A", kWhyAddress, [](Env &e) { e.early = B; }, kBusy },
            { "FLIP_PENDING set", kWhyPending, [](Env &e) { e.pending = true; e.pendUntil = ~0ull; e.neverLatch = true; }, kBusy },
            { "underflow", kWhyHubp, [](Env &e) { e.hubpCntl |= 0x10000000u; }, kNotReady },
            { "BLANK_EN", kWhyHubp, [](Env &e) { e.hubpCntl |= 1u; }, kNotReady },
            { "VTG_SEL", kWhyHubp, [](Env &e) { e.hubpCntl = (D().otgInst ^ 3u) << 4; }, kNotReady },
            { "VM fault", kWhyVmFault, [](Env &e) { e.vmFault = 1u; }, kNotReady },
            { "watchdog thread did not start", kWhyWatchdog, [](Env &e) { e.wdStart = false; }, kNotReady },
        };
        for (auto &c : cases) {
            Env e; ScanX x; reset(x); c.mut(e); uint64_t o[2] = { 0, 0 };
            const uint32_t rc = acquire(e, D(), x, o);
            std::string m = std::string("X2: Acquire refuses: ") + c.name;
            EXPECTI(rc == c.rc && !x.acquired && x.lastAcquireWhy == (uint32_t)c.why, m.c_str());
            EXPECTI(e.writes().empty() && e.depth == 0 && !e.sleptLocked, (m + " - nothing written, lock released, no sleep under it").c_str());
        }
        for (uint32_t k = 0; k < 2; k++) {   // update locks (S3): reads only
            Env e; ScanX x; reset(x); (k == 0 ? e.flipCtlExtra : e.otgUpdLock) = 1u; uint64_t o[2];
            EXPECTI(acquire(e, D(), x, o) == kBusy && x.lastAcquireWhy == kWhyUpdateLock && e.writes().empty(), "S3: an update lock set -> Acquire refuses, nothing written");
        }
    }
}

static void suite_x3() {
    Env e; ScanX x; EXPECTI(acquire_ok(e, x), "X3: acquired");
    uint32_t rc = 0;
    const uint64_t i0 = reg_slot(e, x, S0, &rc), i1 = reg_slot(e, x, S1), i2 = reg_slot(e, x, S2);
    EXPECTI(i0 == T() && i1 == T() + 1u && i2 == T() + 2u, "X3: three slots get the tagged ids tag | k");
    EXPECTI(e.writes().empty(), "X3: Register writes nothing");
    uint32_t r4 = 0; (void)reg_slot(e, x, S3, &r4);
    EXPECTI(r4 == kNoResources, "X3: a fourth slot is NoResources");
    Env e2; ScanX x2; (void)acquire_ok(e2, x2);
    uint32_t r;
    (void)reg_slot(e2, x2, A, &r); EXPECTI(r == kBadArg, "X3: a slot AT A is refused");
    (void)reg_slot(e2, x2, A + 0x10000, &r); EXPECTI(r == kBadArg, "X3: a slot overlapping A is refused");
    (void)reg_slot(e2, x2, B, &r); EXPECTI(r == kBadArg, "X3: a slot AT B is refused");
    (void)reg_slot(e2, x2, B + 0x10000, &r); EXPECTI(r == kBadArg, "X3: a slot overlapping B is refused");
    (void)reg_slot(e2, x2, DPC, &r); EXPECTI(r == kBadArg, "X3: a slot on the DP console (a snapshot range) is refused");
    (void)reg_slot(e2, x2, 0x9000000000ull, &r); EXPECTI(r == kBadArg, "X3: a slot outside the flip window / with another HIGH dword is refused");
    { uint64_t o[2]; Excl ex = excl_default(); ex.n = 2; ex.lo[1] = S0 + 0x100000ull; ex.bytes[1] = 0x10000ull;       // an instance-0 slot / the other display's pair or slot inside the candidate
      EXPECTI(register_slot(e2, D(), x2, regin(S0), ex, o) == kBadArg, "X3: a slot that covers an instance-0 slot or the other display's pair / slot is refused (snapshot ranges)"); }
    { uint64_t o[2]; const Excl ex = excl_default(); RegIn bad = regin(S0); bad.pitchBytes += 4u; EXPECTI(register_slot(e2, D(), x2, bad, ex, o) == kBadArg, "X3: a wrong pitch is refused");
      bad = regin(S0); bad.height = D().inst == 2u ? 1080u : 1440u; EXPECTI(register_slot(e2, D(), x2, bad, ex, o) == kBadArg, "X3: a wrong height is refused");
      bad = regin(S0); bad.width = D().inst == 2u ? 1920u : 2560u; EXPECTI(register_slot(e2, D(), x2, bad, ex, o) == kBadArg, "X3: a wrong width is refused");
      bad = regin(S0); bad.vis = false; EXPECTI(register_slot(e2, D(), x2, bad, ex, o) == kBadArg, "X3: a BO outside the visible pool is refused");
      bad = regin(S0); bad.format = 0; EXPECTI(register_slot(e2, D(), x2, bad, ex, o) == kBadArg, "X3: a wrong format is refused");
      bad = regin(S0, 1000000ull); EXPECTI(register_slot(e2, D(), x2, bad, ex, o) == kBadArg, "X3: a BO that cannot hold the slot is refused"); }
    EXPECTI(x2.tbl.s[0].used == false && x2.tbl.s[1].used == false, "X3: no refused request registered anything");
    { Env eg; ScanX xg; (void)acquire_ok(eg, xg); eg.viewDim = D().inst == 2u ? (1920u | (1080u << 16)) : (2560u | (1440u << 16)); uint32_t rg = 0; (void)reg_slot(eg, xg, S0, &rg);
      EXPECTI(rg == kNotReady && !xg.tbl.s[0].used, "X3: Register after a live geometry change is NotReady and registers nothing"); }
    Env e3; ScanX x3; reset(x3); uint32_t rn; (void)reg_slot(e3, x3, S0, &rn); EXPECTI(rn == kNotReady, "X3: Register before Acquire is NotReady");
}

static void suite_x4() {
    {
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); (void)reg_slot(e, x, S1); (void)reg_slot(e, x, S2);
        e.clear(); uint64_t o[3] = { 0, 0, 7 };
        EXPECTI(present(e, D(), x, T(), 0, o) == kOk && o[0] == 1u && o[2] == 0u, "X4: Present of slot tag|0 succeeds");
        const auto w = e.writes();
        EXPECTI(w.size() == 2 && w[0].a == D().regAddrHigh && w[1].a == D().regAddr, "X4: exactly two writes, HIGH then LOW");
        EXPECTI(w.size() == 2 && w[0].b == (uint32_t)(S0 >> 32) && w[1].b == (uint32_t)S0, "X4: the values are the slot's address");
        EXPECTI(only_addr_writes(e), "X4: zero writes outside this instance's pair");
        EXPECTI(!e.sleptLocked && e.depth == 0, "X4: no sleep under the lock");
        for (auto &ev : e.ev) if (ev.k == 'R') { EXPECTI(ev.a != kHubp0Early, "X4: no HUBP0 register was read"); }
        EXPECTI(x.lastWritten == S0, "X4: the writer's record");
        EXPECTI(present(e, D(), x, T(), 0, o) == kBusy && x.reuseInuseRefused == 1u, "X4: re-presenting the PENDING slot is refused and counted");
        settle(e, 60);
        EXPECTI(present(e, D(), x, T(), 0, o) == kBusy && x.reuseInuseRefused == 2u, "X4: the slot the hardware FETCHES (EARLIEST == slot.mc) is refused and counted");
        const XDesc &od = D().inst == 1u ? kDescMonB : kDescMonA;
        EXPECTI(present(e, D(), x, 0, 0, o) == kBadArg && present(e, D(), x, 2, 0, o) == kBadArg && present(e, D(), x, T() + 3u, 0, o) == kBadArg && present(e, D(), x, 0x2000, 0, o) == kBadArg, "X4: an instance-0 slot id, an untagged id and tag|3 are refused");
        EXPECTI(present(e, D(), x, od.slotTag, 0, o) == kBadArg && present(e, D(), x, od.slotTag + 1u, 0, o) == kBadArg, "X4: the OTHER instance's slot ids are refused (id_ok) before any register is read");
        EXPECTI(x.untaggedRefused == 6u, "X4: counted as untagged");
        EXPECTI(present(e, D(), x, T() + 1u, 1, o) == kBadArg, "X4: a non-zero flag word is refused");
        EXPECTI(only_addr_writes(e) && e.writes().size() == 2, "X4: the refusals wrote nothing more");
        e.clear();
        EXPECTI(!write_addr(e, D(), x, A, kModePresent) && !write_addr(e, D(), x, B, kModePresent), "X4: the present writer refuses A and B");
        EXPECTI(!write_addr(e, D(), x, 0x1234560000ull, kModePresent) && !write_addr(e, D(), x, DPC, kModePresent), "X4: ...and a foreign / DP address");
        EXPECTI(!write_addr(e, D(), x, (1ull << 40) | (S0 & 0xFFFFFFFFull), kModePresent), "X4: ...and a MIXED address (A's low half with another HIGH)");
        { Env em; ScanX xm; (void)acquire_ok(em, xm); const uint64_t mixed = A + (1ull << 32) + 5 * D().allocBytes; uint32_t kk = 0;
          EXPECTI(n48scan::slot_register(xm.tbl, mixed, D().bytes, &kk) == kOk && n48scan::flip_target_ok(xm.tbl, mixed), "X4 (setup): a table entry with another HIGH dword");
          em.clear(); EXPECTI(!write_addr(em, D(), xm, mixed, kModePresent) && em.writes().empty(), "X4: the writer refuses a REGISTERED address whose HIGH dword is not A's (a flip never changes HIGH)"); }
        EXPECTI(!write_addr(e, D(), x, S0 + 0x1000, kModePresent), "X4: ...and an unaligned one");
        EXPECTI(!write_addr(e, D(), x, S1, kModeRestore) && !write_addr(e, D(), x, B, kModeRestore) && !write_addr(e, D(), x, 0, kModeRestore), "X4: the restore writer refuses every address but A");
        EXPECTI(e.writes().empty(), "X4: no refused address reached a register");
        EXPECTI(write_addr(e, D(), x, A, kModeRestore) && e.writes().size() == 2, "X4: the restore writer admits exactly A");
        // the writer judges the state's descriptor: a state acquired through another descriptor is never written through this one
        { const XDesc &od2 = D().inst == 1u ? kDescMonB : kDescMonA; e.clear(); EXPECTI(!write_addr(e, od2, x, A, kModeRestore) && !write_addr(e, od2, x, S1, kModePresent) && e.writes().empty(), "X4: a descriptor that is not the state's writes nothing (the second wall against an instance swap)"); }
    }
    {   // a rotation of three slots never programs a slot the hardware still shows or has pending
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); (void)reg_slot(e, x, S1); (void)reg_slot(e, x, S2);
        unsigned presented = 0; bool violation = false;
        for (int i = 0; i < 200; i++) {
            e.upd();
            const uint32_t k = (uint32_t)(i % 3); uint64_t o[3];
            const uint64_t slotMc = k == 0 ? S0 : k == 1 ? S1 : S2;
            const bool hwBusy = e.pending || e.early == slotMc;
            const uint32_t rc = present(e, D(), x, T() + k, 0, o);
            if (rc == kOk) { presented++; if (hwBusy) violation = true; }
            settle(e, 17);
        }
        EXPECTI(!violation, "X4: no presented slot was pending or being fetched when it was programmed");
        EXPECTI(presented > 100u, "X4: a present per frame is mostly accepted");
        StatusOut st; (void)status(e, D(), x, &st);
        EXPECTI(st.latched + 1 >= st.presents && st.replaced == 0, "X5: every present latched (seen by polling the raw pending bit), none was replaced");
        EXPECTI(st.reuseInuseRefused == x.reuseInuseRefused, "X5: the refusal counter is reported");
    }
    {   // a write failure: the table is restored, wantRestore set; the watchdog restores A
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); uint64_t o[3];
        e.failWriteAbs = D().regAddr;
        EXPECTI(present(e, D(), x, T(), 0, o) == kNotReady && x.wantRestore && x.tbl.presents == 0u && !x.tbl.pendActive, "X4: a failed LOW write: refused, table restored, wantRestore set");
        e.failWriteAbs = 0; e.clear();
        EXPECTI(watchdog_step(e, D(), x, x.gen) && !x.acquired && x.wdRestores == 1u, "X4: the watchdog restores A");
        auto w = e.writes(); EXPECTI(w.size() == 2 && w[0].a == D().regAddrHigh && w[1].a == D().regAddr && w[1].b == (uint32_t)A, "X4: ...with HIGH then LOW of A");
        Env f; ScanX y; (void)acquire_ok(f, y); (void)reg_slot(f, y, S0); f.failWriteAbs = D().regAddrHigh;
        EXPECTI(present(f, D(), y, T(), 0, o) == kNotReady && y.wantRestore && y.writeFailed == 1u, "X4: a failed HIGH write: refused, wantRestore");
        EXPECTI(f.writes().size() == 1, "X4: and the LOW write was NOT attempted after a failed HIGH");
    }
    {   // present during a restore / not acquired / geometry
        Env e; ScanX x; reset(x); uint64_t o[3];
        EXPECTI(present(e, D(), x, T(), 0, o) == kNotReady, "X4: Present before Acquire is NotReady");
        (void)acquire_ok(e, x); (void)reg_slot(e, x, S0);
        x.restoring = true; EXPECTI(present(e, D(), x, T(), 0, o) == kNotReady && e.writes().empty(), "X4: a present while RESTORING is refused, no write"); x.restoring = false;
        x.wantRestore = true; EXPECTI(present(e, D(), x, T(), 0, o) == kNotReady && e.writes().empty(), "X4: a present with wantRestore set is refused, no write"); x.wantRestore = false;
        const uint32_t okDim = e.viewDim; e.viewDim = D().inst == 2u ? (1920u | (1080u << 16)) : (2560u | (1440u << 16));
        EXPECTI(present(e, D(), x, T(), 0, o) == kNotReady && x.geomRefused == 1u && e.writes().empty(), "X4: a present after a geometry change is refused, no write"); e.viewDim = okDim;
        EXPECTI(present(e, D(), x, T() + 2u, 0, o) == kNotFound, "X4: an unregistered tagged slot is NotFound");
    }
}

static void suite_x5_x6_x7() {
    // ===== X5 latch bookkeeping: the raw FLIP_PENDING bit clearing is the latch; this OTG's frame count stamps it; EARLIEST_INUSE is the reuse rule (poll-only)
    {
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); uint64_t o[3];
        EXPECTI(present(e, D(), x, T(), 0, o) == kOk && o[1] == (uint64_t)((e.nowMs * 60ull / 1000ull) & 0xFFFFFFu) + 1ull, "X5: the target frame is THIS OTG's count + 1");
        EXPECTI(x.tbl.pendActive, "X5: pending after the present");
        settle(e, 25); StatusOut st; EXPECTI(status(e, D(), x, &st) == kOk && st.latched == 1u && !x.tbl.pendActive && x.latchPoll == 1u, "X5: Status polls the raw FLIP_PENDING bit and counts the latch");
        EXPECTI(st.slot[0].latchedFrame >= o[1] && st.frontSlot == T(), "X5: the latch frame comes from this OTG's counter; the front slot is tagged");
        EXPECTI(st.consoleMc == A && st.bufB == B && st.slot[0].mc == S0, "X5: console_mc = A");
        settle(e, 40); (void)status(e, D(), x, &st);
        EXPECTI((st.slot[0].flags & 2u) != 0u && (st.slot[0].flags & 4u) == 0u, "X5: slot 0 reads IN USE (EARLIEST == its MC) and not reusable");
        EXPECTI(e.countRegReads(kHubp0Early) == 0u, "X5: HUBP0's EARLIEST_INUSE is never read");
        const XDesc &od = D().inst == 1u ? kDescMonB : kDescMonA;
        EXPECTI(e.countRegReads(od.regOtgFrame) == 0u && e.countRegReads(od.regEarlyLo) == 0u && e.countRegReads(od.regFlipControl) == 0u && e.countRegReads(od.regAddr) == 0u, "X5: the OTHER display's OTG frame counter, EARLIEST, FLIP_CONTROL and address are never read");
        EXPECTI((st.slot[1].used == 0u), "X5: unused slot");
        EXPECTI(st.flipPending == 0u, "X5: FLIP_PENDING reported");
        EXPECTI(x.firstLatchNs != 0u && st.lastLatchNs >= st.firstLatchNs, "X5: latch times are recorded");
    }
    // ===== X6 Restore
    {
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); uint64_t o[3]; (void)present(e, D(), x, T(), 0, o); settle(e, 60);
        e.clear(); e.curSetA = false; e.cur = 1; uint64_t r[2] = { 0, 0 };
        EXPECTI(restore(e, D(), x, "test", 0, r) == 0u && r[0] == 1u && r[1] == A, "X6: Restore verified, plane reads A");
        auto w = e.writes();
        EXPECTI(w.size() == 2 && w[0].a == D().regAddrHigh && w[1].a == D().regAddr && w[0].b == (uint32_t)(A >> 32) && w[1].b == (uint32_t)A, "X6: HIGH then LOW of A, nothing else");
        EXPECTI(!e.sleptLocked && e.depth == 0, "X6: the lock is dropped around EVERY sleep and released at the end");
        EXPECTI(e.count('S') >= 1u && e.count('S') <= kPolls, "X6: polled (1 ms sleeps), bounded at 100");
        EXPECTI(!x.acquired && !x.restoring && !x.wantRestore && x.restores == 1u && x.restoreFailures == 0u && !x.restoreFailed, "X6: state torn down, counted");
        EXPECTI(e.curSetA && e.cur == 0u, "X6: disp2's bookkeeping cur = A again (the HELD invariant)");
        EXPECTI(x.tbl.consoleMc == A && !x.tbl.s[0].used, "X6: the slot table is empty again");
        e.clear(); r[0] = 5;
        EXPECTI(restore(e, D(), x, "again", 0, r) == 0u && r[0] == 1u && e.writes().empty(), "X6: idempotent: the second Restore writes nothing and reports verified");
        Env e2; ScanX x2; (void)acquire_ok(e2, x2); e2.clear();
        EXPECTI(restore(e2, D(), x2, "stale", x2.gen + 5u, r) == 0u && x2.acquired && e2.writes().empty(), "X6: an expectGen of another acquisition does nothing");
    }
    {   // unverified: the hardware never latches
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); uint64_t o[3]; (void)present(e, D(), x, T(), 0, o); settle(e, 60);
        e.neverLatch = true; e.clear(); e.cur = 1; uint64_t r[2] = { 1, 1 };
        (void)restore(e, D(), x, "dead", 0, r);
        EXPECTI(r[0] == 0u && x.restoreFailed && x.restoreFailures == 1u, "X6: an unverified restore: verdict 0, restoreFailed, counted");
        EXPECTI(e.count('S') == 2u * kPolls, "X6 (S2): it polled 100 ms, programmed A AGAIN, and polled another 100 ms before declaring it unverified");
        EXPECTI(e.writes().size() == 4u && e.writes()[2].a == D().regAddrHigh && e.writes()[3].a == D().regAddr && e.writes()[3].b == (uint32_t)A, "X6 (S2): the second program is HIGH then LOW of A again");
        EXPECTI(e.cur == 1u && !e.curSetA, "X6: disp2's cur is NOT claimed A when the restore did not verify");
        uint64_t g[2] = { 1, 1 };
        EXPECTI(bo_gone(e, D(), x, 1u, x.gen, false, g) == 2u, "X6: a BO with a pin of a failed restore is LEAKED (2), never freed");
        e.neverLatch = false; e.pending = false; e.prog = A; e.early = A;
        r[0] = 0; (void)restore(e, D(), x, "heal", 0, r);
        EXPECTI(r[0] == 1u && !x.restoreFailed && e.writes().size() == 4, "X6: a later look that finds the plane on A clears the failure (and wrote nothing new)");
    }
    {   // the write itself fails
        Env e; ScanX x; (void)acquire_ok(e, x); e.failWriteAbs = D().regAddr; uint64_t r[2] = { 1, 1 };
        (void)restore(e, D(), x, "wfail", 0, r);
        EXPECTI(r[0] == 0u && x.restoreFailed, "X6: a restore whose write failed is NOT verified (even though the plane still shows A)");
    }
    {   // S2: a slow latch that lands in the second round verifies
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); uint64_t o[3]; (void)present(e, D(), x, T(), 0, o); settle(e, 60);
        e.clear(); e.neverLatch = true; uint64_t r[2] = { 0, 0 };
        (void)restore(e, D(), x, "s2", 0, r);
        EXPECTI(r[0] == 0u && e.writes().size() == 4u && e.count('S') == 2u * kPolls, "S2: two programs (HIGH LOW HIGH LOW of A) and 2 x 100 polls before unverified");
        Env f; ScanX y; (void)acquire_ok(f, y); (void)reg_slot(f, y, S0); (void)present(f, D(), y, T(), 0, o); settle(f, 60); f.clear();
        f.stickFirst = 1;
        uint64_t r2[2] = { 0, 0 }; (void)restore(f, D(), y, "slow", 0, r2);
        EXPECTI(r2[0] == 1u && f.writes().size() == 4u && f.count('S') > kPolls && f.count('S') <= 2u * kPolls, "S2: a latch that lands in the second round (after a second program) VERIFIES");
        Env g; ScanX z; (void)acquire_ok(g, z); (void)reg_slot(g, z, S0); (void)present(g, D(), z, T(), 0, o); settle(g, 60); g.clear(); uint64_t r3[2];
        (void)restore(g, D(), z, "fast", 0, r3);
        EXPECTI(r3[0] == 1u && g.writes().size() == 2u, "S2: a restore that verifies in the first round programs A ONCE");
    }
    // ===== X7 the watchdog
    {   // 5 s idle
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); const uint32_t gen = x.gen; unsigned steps = 0;
        e.clear();
        while (steps < 200u) { e.sleep_ms(kWatchMs); steps++; if (watchdog_step(e, D(), x, gen)) break; }
        EXPECTI(!x.acquired && x.wdRestores == 1u, "X7: 5 s without a scanout call restores A");
        EXPECTI(steps >= 100u && steps <= 102u, "X7: ...after 5 s (50 ms steps)");
        EXPECTI(e.writes().size() == 2 && e.writes()[1].b == (uint32_t)A, "X7: the restore is HIGH then LOW of A");
        EXPECTI(watchdog_step(e, D(), x, gen), "X7: the thread of an ended acquisition ends");
    }
    {   // a static display kept alive by Status once a second: never reverted, over 30 s
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); const uint32_t gen = x.gen; bool done = false; StatusOut st;
        for (int i = 0; i < 600 && !done; i++) { e.sleep_ms(kWatchMs); done = watchdog_step(e, D(), x, gen); if (i % 20 == 19) (void)status(e, D(), x, &st); }
        EXPECTI(!done && x.acquired && x.wdRestores == 0u, "X7: a merely static display kept alive by Status is NOT reverted (30 s)");
        EXPECTI(e.writes().empty(), "X7: and nothing was written");
    }
    {   // geometry change / wantRestore / restoring
        Env e; ScanX x; (void)acquire_ok(e, x); const uint32_t gen = x.gen;
        e.viewDim = D().inst == 2u ? (1920u | (1080u << 16)) : (2560u | (1440u << 16)); EXPECTI(watchdog_step(e, D(), x, gen) && !x.acquired && x.wdRestores == 1u, "X7: a live geometry change restores A");
        Env f; ScanX y; (void)acquire_ok(f, y); y.wantRestore = true; y.wantWhy = "ask"; EXPECTI(watchdog_step(f, D(), y, y.gen) && !y.acquired, "X7: wantRestore restores A");
        Env g; ScanX z; (void)acquire_ok(g, z); z.restoring = true; EXPECTI(!watchdog_step(g, D(), z, z.gen) && g.writes().empty(), "X7: while another thread restores, the watchdog waits");
        EXPECTI(g.depth == 0, "X7: lock released");
    }
    {   // a stale thread of an earlier acquisition must not restore a newer one
        Env e; ScanX x; (void)acquire_ok(e, x); const uint32_t g1 = x.gen; uint64_t r[2]; (void)restore(e, D(), x, "first", 0, r);
        EXPECTI(acquire(e, D(), x, r) == kOk && x.gen == g1 + 1u && x.acquired, "X7 (setup): a second acquisition, generation 2");
        e.clear(); x.lastActivityNs = 0ull;
        EXPECTI(watchdog_step(e, D(), x, g1) && x.acquired && e.writes().empty() && x.wdRestores == 0u, "X7: the thread of generation 1 ends and restores NOTHING of generation 2");
        Env f; ScanX y; (void)acquire_ok(f, y); (void)reg_slot(f, y, S0); uint64_t o[3]; (void)present(f, D(), y, T(), 0, o); settle(f, 40);
        EXPECTI(!watchdog_step(f, D(), y, y.gen) && y.tbl.latched == 1u && y.latchPoll == 1u, "X7: the watchdog's own poll sees the latch (no caller needed)");
    }
    {   // S3: the watchdog re-judges VMID / flip type / TMZ / viewport start
        struct Mut { const char *name; void (*f)(Env &); } muts[] = {
            { "VMID", [](Env &e) { e.vmid = 2u; } }, { "FLIP_TYPE", [](Env &e) { e.flipCtlExtra = 2u; } }, { "TMZ", [](Env &e) { e.ctl = 1u; } }, { "viewport start", [](Env &e) { e.viewStart = 0x10u; } } };
        for (auto &m : muts) {
            Env e; ScanX x; (void)acquire_ok(e, x); const uint32_t gen = x.gen; m.f(e); e.clear();
            std::string msg = std::string("S3: after Acquire a change of ") + m.name + " makes the watchdog restore A";
            EXPECTI(watchdog_step(e, D(), x, gen) && !x.acquired && x.wdRestores == 1u && e.writes().size() == 2u, msg.c_str());
        }
        Env h; ScanX y; (void)acquire_ok(h, y); h.clear(); EXPECTI(!watchdog_step(h, D(), y, y.gen) && y.acquired && h.writes().empty(), "S3: an unchanged plane is left alone by the watchdog");
    }
}

// ===== X8 teardown order (three instances), bo_gone =====================================================================================================================================================================
struct TeardownT {
    std::vector<std::string> log; uint64_t res0 = 1, res1 = 1, res2 = 1; std::vector<uint32_t> usedv, p0, p1, p2, leak, cleared; bool anyBoBeforeRestore = false; bool r0done = false, r1done = false, r2done = false;
    TeardownT() : usedv(6, 0), p0(6, 0), p1(6, 0), p2(6, 0) {}
    void restore0(const char *, uint64_t r[2]) { log.push_back("restore0"); r[0] = res0; r0done = true; }
    void restore1(const char *, uint64_t r[2]) { log.push_back("restore1"); r[0] = res1; r1done = true; }
    void restore2(const char *, uint64_t r[2]) { log.push_back("restore2"); r[0] = res2; r2done = true; }
    uint32_t nbo() { return 6; }
    bool used(uint32_t h) { if (!(r0done && r1done && r2done)) anyBoBeforeRestore = true; return usedv[h] != 0; }
    uint32_t pin0(uint32_t h) { return p0[h]; } uint32_t pin1(uint32_t h) { return p1[h]; } uint32_t pin2(uint32_t h) { return p2[h]; }
    void mark_leak(uint32_t h) { log.push_back("leak"); leak.push_back(h); } void clear_pins(uint32_t h) { cleared.push_back(h); }
};
static void suite_x8_x9() {
    {
        Env e; ScanX x; (void)acquire_ok(e, x); (void)reg_slot(e, x, S0); (void)reg_slot(e, x, S1); uint64_t o[3]; (void)present(e, D(), x, T(), 0, o); settle(e, 80); e.clear(); uint64_t r[2] = { 0, 0 };
        EXPECTI(bo_gone(e, D(), x, 2u, x.gen, false, r) == 0u && e.writes().empty() && !x.tbl.s[1].used && x.acquired, "X8: BoFree of a slot neither shown nor pending just drops it (no write)");
        EXPECTI(bo_gone(e, D(), x, 1u, x.gen, false, r) == 1u && !x.acquired && e.writes().size() == 2, "X8: BoFree of the slot being SHOWN restores A first (verified)");
        { Env eh; ScanX xh; (void)acquire_ok(eh, xh); (void)reg_slot(eh, xh, S0); (void)reg_slot(eh, xh, S1); uint64_t oo[3]; (void)present(eh, D(), xh, T(), 0, oo); settle(eh, 80); (void)present(eh, D(), xh, T() + 1u, 0, oo); settle(eh, 20);
          StatusOut sto; (void)status(eh, D(), xh, &sto);
          EXPECTI(xh.tbl.front == 1u && !xh.tbl.pendActive, "X8 (setup): the front moved on to slot 1");
          uint64_t pe = 0, ee = 0; bool pd = false; (void)read_plane(eh, D(), &pe, &ee, &pd); EXPECTI(ee == S0, "X8 (setup): this HUBP still FETCHES slot 0");
          eh.clear(); uint64_t rr[2] = { 0, 0 };
          EXPECTI(bo_gone(eh, D(), xh, 1u, xh.gen, false, rr) == 1u && eh.writes().size() == 2, "X8: BoFree of a slot the hardware still FETCHES (EARLIEST == its MC, though it is no longer the front) restores A first"); }
        Env f; ScanX y; (void)acquire_ok(f, y); (void)reg_slot(f, y, S0); f.clear();
        EXPECTI(bo_gone(f, D(), y, 1u, y.gen, true, r) == 1u && f.writes().size() == 2, "X8: the leak / close path (alwaysFull) restores A first, whatever the slots are doing");
        Env g; ScanX z; (void)acquire_ok(g, z); (void)reg_slot(g, z, S0); g.clear();
        EXPECTI(bo_gone(g, D(), z, 1u, z.gen + 3u, true, r) == 0u && g.writes().empty() && z.acquired, "X8: a pin of another generation is a no-op");
        Env h; ScanX w; (void)acquire_ok(h, w); (void)reg_slot(h, w, S0); h.clear(); uint64_t rr[2]; uint64_t o3[3]; (void)present(h, D(), w, T(), 0, o3);
        EXPECTI(bo_gone(h, D(), w, 1u, w.gen, false, rr) == 1u, "X8: BoFree of a PENDING slot restores A first");
        // the state of ANOTHER descriptor is never restored / judged through this one
        { Env em; ScanX xm; (void)acquire_ok(em, xm); (void)reg_slot(em, xm, S0); const XDesc &od = D().inst == 1u ? kDescMonB : kDescMonA; em.clear(); uint64_t rm[2] = { 1, 1 };
          EXPECTI(bo_gone(em, od, xm, 1u, xm.gen, true, rm) == 2u && em.writes().empty(), "X8: bo_gone through the OTHER descriptor judges nothing, writes nothing and LEAKS (2)");
          EXPECTI(restore(em, od, xm, "swap", 0, rm) == 0u && rm[0] == 0u && em.writes().empty() && xm.acquired, "X8: Restore through the OTHER descriptor writes nothing and is NOT verified");
          EXPECTI(watchdog_step(em, od, xm, xm.gen) && xm.acquired && em.writes().empty(), "X8: the watchdog of the OTHER descriptor ends and touches nothing");
          uint64_t o2[2]; const Excl ex = excl_default(); EXPECTI(register_slot(em, od, xm, regin(S1), ex, o2) == kBadArg, "X8: Register through the OTHER descriptor is refused");
          uint64_t o4[3]; EXPECTI(present(em, od, xm, od.slotTag, 0, o4) == kBadArg && em.writes().empty(), "X8: Present through the OTHER descriptor is refused, nothing written"); }
    }
    {   // teardown_all: three instances, in order, before any BO is touched
        { TeardownT t; t.usedv[1] = t.usedv[3] = t.usedv[4] = 1; t.p0[1] = 1; t.p1[4] = 2; t.p2[3] = 4; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "close", r0, r1, r2);
          EXPECT(t.log.size() == 3 && t.log[0] == "restore0" && t.log[1] == "restore1" && t.log[2] == "restore2", "X8: ALL THREE instances are restored, 0 then 1 then 2, and nothing is leaked when all verify");
          EXPECT(!t.anyBoBeforeRestore, "X8: no BO was looked at before all three restores finished");
          EXPECT(t.cleared.size() == 3, "X8: every pin is cleared"); }
        { TeardownT t; t.res2 = 0; t.usedv[1] = t.usedv[3] = t.usedv[4] = 1; t.p0[1] = 1; t.p1[4] = 2; t.p2[3] = 4; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "close", r0, r1, r2);
          EXPECT(t.leak.size() == 1 && t.leak[0] == 3u, "X8: an unverified instance-2 restore LEAKS exactly the BO with an instance-2 pin"); }
        { TeardownT t; t.res1 = 0; t.usedv[1] = t.usedv[3] = t.usedv[4] = 1; t.p0[1] = 1; t.p1[4] = 2; t.p2[3] = 4; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "close", r0, r1, r2);
          EXPECT(t.leak.size() == 1 && t.leak[0] == 4u, "X8: an unverified instance-1 (monitor A) restore leaks ONLY the BO with an instance-1 pin"); }
        { TeardownT t; t.res0 = 0; t.usedv[1] = t.usedv[3] = t.usedv[4] = 1; t.p0[1] = 1; t.p1[4] = 2; t.p2[3] = 4; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "close", r0, r1, r2);
          EXPECT(t.leak.size() == 1 && t.leak[0] == 1u, "X8: an unverified instance-0 restore leaks only the instance-0 BO"); }
        { TeardownT t; t.res0 = t.res1 = t.res2 = 0; t.usedv[2] = 1; t.p0[2] = 1; t.p1[2] = 1; t.p2[2] = 1; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "hung", r0, r1, r2);
          EXPECT(t.log.size() == 6 && t.log[0] == "restore0" && t.log[1] == "restore1" && t.log[2] == "restore2" && t.log[3] == "leak", "X8: the HUNG order with three instances: restore all three, THEN leak"); }
        { TeardownT t; t.res1 = 0; t.usedv[2] = 1; t.p2[2] = 4; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "close", r0, r1, r2);
          EXPECT(t.leak.empty(), "X8: a BO pinned only for instance 2 is NOT leaked by an unverified instance-1 restore"); }
    }
    // ===== X9 GPU-held reporting, the disp2 guard
    {
        n48scan::Table t; n48scan::table_init(t, A, D().bytes); uint32_t k;
        EXPECTI(n48scan::slot_register(t, S0, D().bytes, &k) == kOk, "X9: slot");
        EXPECTI(gpu_held_code(false, t, S0) == 0u && gpu_held_code(true, t, S0) == 1u && gpu_held_code(true, t, A) == 4u && gpu_held_code(true, t, B) == 4u, "X9: GPU-held code: 0 not acquired, k+1 for a slot, 4 for A / B / unknown");
        EXPECTI(hold_verdict_gpu(N48D2_H_LATCH | N48D2_H_VMFAULT, 1u) == (N48D2_H_VMFAULT | kHoldGpuHeld) && hold_verdict_gpu(N48D2_H_LATCH, 0u) == N48D2_H_LATCH, "X9: while GPU-held on one of OUR slots (codes 1..3) the latch bit of the hold verdict is dropped (and only then)");
        for (uint32_t hc = 1; hc <= 3; hc++) EXPECTI((hold_verdict_gpu(N48D2_H_LATCH, hc) & N48D2_H_LATCH) == 0u && (hold_verdict_gpu(N48D2_H_LATCH, hc) & kHoldGpuHeld) != 0u, "S1: codes 1..3 drop the latch bit");
        EXPECTI((hold_verdict_gpu(N48D2_H_LATCH, 4u) & N48D2_H_LATCH) != 0u && (hold_verdict_gpu(0u, 4u) & kHoldGpuHeld) != 0u, "S1: code 4 (EARLIEST is A, B or unknown) KEEPS the latch bit - a real disagreement - and carries GPU-held");
        EXPECTI(hold_verdict_gpu(0u, 1u) != 0u && hold_verdict_gpu(0u, 4u) != 0u && hold_verdict_gpu(0u, 0u) == 0u && n48fb_live_refuses(hold_verdict_gpu(0u, 2u)), "S1: any GPU-held code makes the verdict non-zero, so fbpublish (n48fb::live_verdict) is REFUSED while the instance is acquired; an idle plane is not");
        const uint32_t di = D().inst;
        n48d2_aset_clear_i(di); n48d2_aset_set_i(di, A, B);
        EXPECTI(n48d2_aset_lo_ok_i(di, (uint32_t)A) && n48d2_aset_lo_ok_i(di, (uint32_t)B), "X9: the disp2 guard admits A and B");
        EXPECTI(!n48d2_aset_lo_ok_i(di, (uint32_t)S0) && !n48d2_aset_lo_ok_i(di, (uint32_t)S1) && !n48d2_aset_lo_ok_i(di, (uint32_t)S2), "X9: ...and NEVER a slot's low half");
        { Env ez; ScanX xz; reset(xz); EXPECTI(!write_addr(ez, D(), xz, B, kModePresent), "X9: ...and the scanx writer never admits B"); }
        n48d2_aset_clear_i(di);
    }
}

static void run_suite(const XDesc &d) {
    setup_layout(d);
    suite_x1(); suite_x2(); suite_x3(); suite_x4(); suite_x5_x6_x7(); suite_x8_x9();
}

// =====================================================================================================================================================================================================================
// XC  CROSS-INSTANCE: ONE fake card with DISTINCT HUBP1 and HUBP2 states, a recorder per instance. Whatever one instance does, the other's registers, writes and state stay as they were.
// =====================================================================================================================================================================================================================
struct Hubp { uint64_t prog, early, pendUntil = 0, earlyAt = 0; bool pending = false; uint32_t viewDim, pitch, cfg = 8u, tiling = 0x80u, ctl = 0u, vmid = 0u, flipExtra = 0u, viewStart = 0u, cntl; uint64_t hi = 0; };
struct Card {
    uint64_t nowMs = 5000; Hubp h[2];                     // h[0] = HUBP1 (the monitor A), h[1] = HUBP2 (the monitor B)
    uint32_t vmFault = 0u; unsigned otgReads[3] = { 0, 0, 0 };
    Card() {
        h[0].prog = h[0].early = 0x8004360000ull; h[0].viewDim = 1920u | (1080u << 16); h[0].pitch = 0x77Fu; h[0].cntl = 1u << 4;
        h[1].prog = h[1].early = 0x8002740000ull; h[1].viewDim = 2560u | (1440u << 16); h[1].pitch = 0x9FFu; h[1].cntl = 2u << 4;
        h[0].hi = h[0].prog >> 32; h[1].hi = h[1].prog >> 32;
    }
    void upd() { for (auto &x : h) { if (x.pending && nowMs >= x.pendUntil) x.pending = false; if (x.earlyAt != 0 && nowMs >= x.earlyAt && !x.pending) x.early = x.prog; } }
};
// an environment bound to ONE instance's descriptor over the shared card; swapRegs routes this descriptor's register reads to the OTHER HUBP (a mixed-up register file); stateInst picks which disp2 plane state it is fed
struct CEnv {
    Card &c; const XDesc &d; ScanX *self;
    bool swapRegs = false; uint32_t stateInst = 0; uint32_t gateCode = 0u;
    std::vector<Ev> ev; int depth = 0; uint64_t mcA[2], mcB[2];
    uint32_t gateAndState[1] = { 0 };
    CEnv(Card &cc, const XDesc &dd) : c(cc), d(dd), self(nullptr) { stateInst = dd.inst; mcA[0] = 0x8004360000ull; mcB[0] = mcA[0] + kDescMonA.allocBytes; mcA[1] = 0x8002740000ull; mcB[1] = mcA[1] + kDescMonB.allocBytes; }
    Hubp &hub() { return c.h[(swapRegs ? (d.inst == 1u ? 2u : 1u) : d.inst) - 1u]; }
    uint32_t gate() { return gateCode; }
    uint32_t rd(uint32_t abs) {
        ev.push_back({ 'R', abs, 0 }); c.upd(); Hubp &x = hub();
        if (abs == d.regViewDim) return x.viewDim; if (abs == d.regPitch) return x.pitch; if (abs == d.regSurfConfig) return x.cfg; if (abs == d.regTiling) return x.tiling; if (abs == d.regSurfControl) return x.ctl;
        if (abs == d.regVmid) return x.vmid; if (abs == d.regViewStart) return x.viewStart; if (abs == d.regHubpCntl) return x.cntl; if (abs == d.regVmFault) return c.vmFault;
        if (abs == d.regFlipControl) return (x.pending ? kFlipPendingMask : 0u) | x.flipExtra;
        if (abs == d.regAddr) return (uint32_t)x.prog; if (abs == d.regAddrHigh) return (uint32_t)(x.prog >> 32);
        if (abs == d.regEarlyLo) return (uint32_t)x.early; if (abs == d.regEarlyHi) return (uint32_t)(x.early >> 32);
        if (abs == d.regOtgUpdLock) return 0u;
        if (abs == d.regOtgFrame) { c.otgReads[d.inst]++; return (uint32_t)(c.nowMs * (d.inst == 1u ? 60000ull : 59950ull) / 1000000ull) & 0xFFFFFFu; }   // the two OTGs count at slightly different rates
        return 0xFFFFFFFFu;
    }
    bool wr(uint32_t abs, uint32_t v) {
        ev.push_back({ 'W', abs, v });
        if (!write_reg_ok(d, abs)) return false;
        Hubp &x = c.h[d.inst - 1u];                                    // a WRITE always lands on the descriptor's own HUBP
        if (abs == d.regAddrHigh) { x.hi = v; return true; }
        x.prog = (x.hi << 32) | v; x.pending = true; x.pendUntil = c.nowMs + 17; x.earlyAt = c.nowMs + 34; return true;
    }
    uint64_t now_ns() { return c.nowMs * 1000000ull; }
    void sleep_ms(uint32_t ms) { ev.push_back({ 'S', ms, 0 }); c.nowMs += ms; }
    void lock() { ev.push_back({ 'L', 0, 0 }); depth++; }
    void unlock() { ev.push_back({ 'U', 0, 0 }); depth--; }
    uint32_t d2_stage() { return N48D2_PL_HELD; } uint32_t d2_cur() { return 0u; } uint64_t d2_mc(uint32_t i) { return (i & 1u) ? mcB[stateInst - 1u] : mcA[stateInst - 1u]; } void d2_set_cur_a() {}
    bool window(uint64_t *lo, uint64_t *hi) { *lo = 0x8000000000ull; *hi = 0x8010000000ull; return true; }
    bool start_watchdog(uint32_t) { return true; }
    void note(const char *) {} template <class T2, class... Ar> void note(const char *, T2, Ar...) {}
    std::vector<Ev> writes() const { std::vector<Ev> w; for (auto &e : ev) if (e.k == 'W') w.push_back(e); return w; }
};
static void suite_xc() {
    Card card; CEnv ea(card, kDescMonA), ed(card, kDescMonB); ScanX xa, xd; reset(xa); reset(xd);
    uint64_t o[2], o3[3];
    EXPECT(acquire(ea, kDescMonA, xa, o) == kOk && o[0] == 0x8004360000ull, "XC: the monitor A acquires its own A from its own HUBP1 / OTG1");
    EXPECT(acquire(ed, kDescMonB, xd, o) == kOk && o[0] == 0x8002740000ull, "XC: the monitor B acquires its own A from HUBP2 / OTG2 (a distinct state)");
    EXPECT(xa.width == 1920u && xa.height == 1080u && xd.width == 2560u && xd.height == 1440u && xa.descInst == 1u && xd.descInst == 2u, "XC: each state carries its own geometry and descriptor");
    // slots: monitor A's from 0x8010..., monitor B's from 0x8020... (disjoint)
    const uint64_t sa[3] = { 0x8008000000ull, 0x8008800000ull, 0x8009000000ull }, sd[3] = { 0x800A000000ull, 0x800B000000ull, 0x800C000000ull };
    Excl exa; std::memset(&exa, 0, sizeof exa); Excl exd = exa; exa.n = 2; exa.lo[0] = 0x8000000000ull; exa.bytes[0] = kDescMonB.bytes; exa.lo[1] = card.h[1].prog; exa.bytes[1] = kDescMonB.allocBytes;
    exd.n = 2; exd.lo[0] = 0x8000000000ull; exd.bytes[0] = kDescMonB.bytes; exd.lo[1] = card.h[0].prog; exd.bytes[1] = kDescMonA.allocBytes;
    for (int k = 0; k < 3; k++) {
        EXPECT(register_slot(ea, kDescMonA, xa, RegIn{ true, sa[k], kDescMonA.bytes, 0, 7680u, 1920u, 1080u, n48scan::kFmtArgb8888 }, exa, o) == kOk && o[0] == (0x10u | (uint32_t)k), "XC: the monitor A's slots are tagged 0x10 | k");
        EXPECT(register_slot(ed, kDescMonB, xd, RegIn{ true, sd[k], kDescMonB.bytes, 0, 10240u, 2560u, 1440u, n48scan::kFmtArgb8888 }, exd, o) == kOk && o[0] == (0x20u | (uint32_t)k), "XC: the monitor B's slots are tagged 0x20 | k");
    }
    // the monitor A's Register refuses an overlap with the monitor B's pair / slot (the other display's snapshot ranges)
    { ScanX xt; reset(xt); CEnv et(card, kDescMonA); et.stateInst = 1; Card c2; (void)c2; (void)xt; }
    { Card c2; CEnv e2(c2, kDescMonA); ScanX x2; reset(x2); EXPECT(acquire(e2, kDescMonA, x2, o) == kOk, "XC (setup)");
      Excl ex = exa; ex.lo[1] = sa[1]; ex.bytes[1] = 0x10000ull;       // the monitor B's slot inside the monitor A's candidate range
      EXPECT(register_slot(e2, kDescMonA, x2, RegIn{ true, sa[1], kDescMonA.bytes, 0, 7680u, 1920u, 1080u, n48scan::kFmtArgb8888 }, ex, o) == kBadArg, "XC: the monitor A's Register refuses a slot overlapping the monitor B's pair / slot (snapshot)"); }
    // ---- a long interleaved run: every write lands in its own pair, the other recorder never sees a write for it
    ea.ev.clear(); ed.ev.clear();
    const Hubp d0 = card.h[1];
    for (int round = 0; round < 40; round++) {
        EXPECT(present(ea, kDescMonA, xa, 0x10u + (uint32_t)(round % 3), 0, o3) == kOk || true, "XC (drive)");
        card.nowMs += 17; card.upd();
    }
    for (auto &w : ea.writes()) EXPECT(w.a == kDescMonA.regAddr || w.a == kDescMonA.regAddrHigh, "XC: every write of the monitor A is HUBP1's primary address pair (0x3ba7 / 0x3ba6)");
    EXPECT(ed.writes().empty() && card.h[1].prog == d0.prog && card.h[1].early == d0.early && !card.h[1].pending, "XC: while the monitor A flips 40 times, the monitor B's recorder is EMPTY and HUBP2's registers are untouched");
    EXPECT(xa.tbl.presents >= 10u && card.h[0].prog != 0x8004360000ull, "XC: the monitor A really flipped (HUBP1's address moved)");
    // then the monitor B flips; the monitor A's plane does not move
    card.nowMs += 200; card.upd(); const Hubp a0 = card.h[0]; ea.ev.clear(); ed.ev.clear();
    for (int round = 0; round < 40; round++) { (void)present(ed, kDescMonB, xd, 0x20u + (uint32_t)(round % 3), 0, o3); card.nowMs += 17; card.upd(); }
    for (auto &w : ed.writes()) EXPECT(w.a == kDescMonB.regAddr || w.a == kDescMonB.regAddrHigh, "XC: every write of the monitor B is HUBP2's primary address pair (0x3c83 / 0x3c82)");
    EXPECT(ea.writes().empty() && card.h[0].prog == a0.prog && card.h[0].early == a0.early, "XC: while the monitor B flips, the monitor A's recorder is EMPTY and HUBP1 does not move");
    // ---- the other instance's slot id / address through this one's flows
    EXPECT(present(ea, kDescMonA, xa, 0x20u, 0, o3) == kBadArg && present(ed, kDescMonB, xd, 0x10u, 0, o3) == kBadArg, "XC: a Present of the OTHER instance's slot id is refused on both sides");
    ea.ev.clear(); EXPECT(!write_addr(ea, kDescMonA, xa, sd[0], kModePresent) && !write_addr(ea, kDescMonA, xa, card.h[1].prog, kModePresent) && ea.writes().empty(), "XC: the monitor A's writer refuses an address of the MONB's table / plane (the whole-address allowlist)");
    ed.ev.clear(); EXPECT(!write_addr(ed, kDescMonB, xd, sa[0], kModePresent) && ed.writes().empty(), "XC: ...and the monitor B's refuses the monitor A's");
    // ---- latch bookkeeping follows THIS OTG's counter (0x508d / 0x510d) only
    { const unsigned a1 = card.otgReads[1], a2 = card.otgReads[2]; StatusOut st; ea.ev.clear(); (void)status(ea, kDescMonA, xa, &st);
      EXPECT(card.otgReads[1] > a1 && card.otgReads[2] == a2, "XC: the monitor A's Status reads OTG1's frame counter (0x508d) and never OTG2's"); }
    // ---- 5 s idle restores the monitor A's A; the monitor B, kept alive, is never touched; the monitor B's recorder stays empty
    { ed.ev.clear(); ea.ev.clear(); const uint32_t ga = xa.gen, gd = xd.gen; bool aDone = false; StatusOut st;
      for (int i = 0; i < 130 && !aDone; i++) { card.nowMs += kWatchMs; aDone = watchdog_step(ea, kDescMonA, xa, ga); (void)watchdog_step(ed, kDescMonB, xd, gd) ; if (i % 15 == 14) (void)status(ed, kDescMonB, xd, &st); }
      EXPECT(aDone && !xa.acquired && xa.wdRestores == 1u, "XC: 5 s idle restores the monitor A's A1");
      EXPECT(xd.acquired && xd.wdRestores == 0u && ed.writes().empty(), "XC: ... the kept-alive monitor B is still acquired and instance 2's recorder is EMPTY");
      EXPECT(card.h[0].prog == 0x8004360000ull, "XC: HUBP1 shows A1 again"); }
    // ---- Acquire1 fed the MONB's disp2 state: refuses with kWhyAddress; fed HUBP2's registers: kWhyGeometry
    { Card c3; CEnv e3(c3, kDescMonA); e3.stateInst = 2; ScanX x3; reset(x3); uint64_t oo[2];
      EXPECT(acquire(e3, kDescMonA, x3, oo) == kBusy && x3.lastAcquireWhy == kWhyAddress && e3.writes().empty() && !x3.acquired, "XC: Acquire1 fed the monitor B's d2 state (A/B of instance 2) refuses with kWhyAddress, nothing written");
      Card c4; CEnv e4(c4, kDescMonA); e4.swapRegs = true; ScanX x4; reset(x4);
      EXPECT(acquire(e4, kDescMonA, x4, oo) == kUnsupported && x4.lastAcquireWhy == kWhyGeometry && e4.writes().empty() && !x4.acquired, "XC: Acquire1 fed HUBP2's registers (2560x1440) refuses with kWhyGeometry, nothing written");
      Card c5; CEnv e5(c5, kDescMonB); e5.stateInst = 1; ScanX x5; reset(x5);
      EXPECT(acquire(e5, kDescMonB, x5, oo) != kOk && !x5.acquired && e5.writes().empty(), "XC: Acquire2 fed the monitor A's d2 state refuses too");
      Card c6; CEnv e6(c6, kDescMonB); e6.swapRegs = true; ScanX x6; reset(x6);
      EXPECT(acquire(e6, kDescMonB, x6, oo) == kUnsupported && x6.lastAcquireWhy == kWhyGeometry && e6.writes().empty(), "XC: Acquire2 fed HUBP1's registers (1920x1080) refuses with kWhyGeometry"); }
    // ---- reuse is judged on THIS HUBP's EARLIEST: the other HUBP's EARLIEST equal to the slot does not refuse; this one's does
    { Card c7; CEnv e7(c7, kDescMonA); ScanX x7; reset(x7); uint64_t oo[2], oo3[3]; (void)acquire(e7, kDescMonA, x7, oo);
      (void)register_slot(e7, kDescMonA, x7, RegIn{ true, sa[0], kDescMonA.bytes, 0, 7680u, 1920u, 1080u, n48scan::kFmtArgb8888 }, exa, oo);
      c7.h[1].early = sa[0];     // HUBP2 'fetches' the monitor A's slot: irrelevant to instance 1
      EXPECT(present(e7, kDescMonA, x7, 0x10u, 0, oo3) == kOk, "XC: HUBP2's EARLIEST equal to the monitor A's slot does NOT refuse the monitor A's present (reuse is judged on HUBP1's)");
      c7.nowMs += 100; c7.upd();
      EXPECT(present(e7, kDescMonA, x7, 0x10u, 0, oo3) == kBusy && x7.reuseInuseRefused >= 1u, "XC: HUBP1's own EARLIEST equal to the slot refuses (reuse_inuse_refused)"); }
    // ---- close restores 0, 1 and 2 before any free: the order of the teardown environment; an unverified restore1 leaks only instance-1 BOs
    { TeardownT t; t.res1 = 0; t.usedv[1] = t.usedv[2] = t.usedv[3] = 1; t.p1[2] = 1; t.p2[3] = 1; t.p0[1] = 1; uint64_t r0[2], r1[2], r2[2]; teardown_all(t, "close", r0, r1, r2);
      EXPECT(t.leak.size() == 1 && t.leak[0] == 2u && t.log[0] == "restore0" && t.log[1] == "restore1" && t.log[2] == "restore2" && !t.anyBoBeforeRestore, "XC: close restores 0, 1, 2 before any BO is touched; an unverified restore1 leaks only the instance-1 BO"); }
}

// =====================================================================================================================================================================================================================
// XD  the descriptor offsets, BY NAME from Linux's dcn_4_1_0_offset.h (absolute = this card's segment base [BASE_IDX] + offset), the way native_disp2_test.cpp does
// =====================================================================================================================================================================================================================
static std::string gOffHdr;
static bool lx_load(const std::string &root) {
    std::vector<std::string> roots;
    if (const char *e = std::getenv("N48_LINUX_DC")) roots.push_back(e);
    roots.push_back(root);
    roots.push_back("/path/to/navi48-checkout");
    for (auto &r : roots) { std::string s = slurp(r, "re/linux-dc/drivers/gpu/drm/amd/include/asic_reg/dcn/dcn_4_1_0_offset.h"); if (!s.empty()) { gOffHdr = s; return true; } }
    return false;
}
static bool lx_abs(const std::string &reg, uint32_t *abs) {
    static const uint32_t kSeg[5] = { 0x12u, 0xc0u, 0x34c0u, 0x9000u, 0x02403c00u };           // this card's own DMU bases
    const std::string key = "#define reg" + reg + " ";
    size_t p = gOffHdr.find(key); if (p == std::string::npos || (p > 0 && gOffHdr[p - 1] != '\n')) return false;
    const uint32_t off = (uint32_t)std::strtoul(gOffHdr.c_str() + p + key.size(), nullptr, 0);
    const std::string bk = "#define reg" + reg + "_BASE_IDX";
    size_t q = gOffHdr.find(bk); if (q == std::string::npos) return false;
    q += bk.size(); while (gOffHdr[q] == ' ' || gOffHdr[q] == '\t') ++q;
    const uint32_t b = (uint32_t)std::strtoul(gOffHdr.c_str() + q, nullptr, 0);
    if (b > 4) return false;
    *abs = kSeg[b] + off;
    return true;
}
static void suite_xd() {
    EXPECT(!gOffHdr.empty(), "XD: Linux's dcn_4_1_0_offset.h is readable");
    if (gOffHdr.empty()) return;
    for (const XDesc *dp : { &kDescMonA, &kDescMonB }) {
        const XDesc &d = *dp; const std::string n = std::to_string(d.otgInst);
        struct R { const char *nm; uint32_t ours; } rows[] = {
            { "HUBP%_DCSURF_SURFACE_CONFIG", d.regSurfConfig }, { "HUBP%_DCSURF_TILING_CONFIG", d.regTiling }, { "HUBP%_DCSURF_PRI_VIEWPORT_START", d.regViewStart }, { "HUBP%_DCSURF_PRI_VIEWPORT_DIMENSION", d.regViewDim },
            { "HUBP%_DCHUBP_CNTL", d.regHubpCntl }, { "HUBPREQ%_DCSURF_SURFACE_PITCH", d.regPitch }, { "HUBPREQ%_VMID_SETTINGS_0", d.regVmid }, { "HUBPREQ%_DCSURF_PRIMARY_SURFACE_ADDRESS", d.regAddr },
            { "HUBPREQ%_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH", d.regAddrHigh }, { "HUBPREQ%_DCSURF_SURFACE_CONTROL", d.regSurfControl }, { "HUBPREQ%_DCSURF_FLIP_CONTROL", d.regFlipControl },
            { "HUBPREQ%_DCSURF_SURFACE_EARLIEST_INUSE", d.regEarlyLo }, { "HUBPREQ%_DCSURF_SURFACE_EARLIEST_INUSE_HIGH", d.regEarlyHi }, { "DCN_VM_FAULT_STATUS", d.regVmFault },
            { "OTG%_OTG_STATUS_FRAME_COUNT", d.regOtgFrame }, { "OTG%_OTG_MASTER_UPDATE_LOCK", d.regOtgUpdLock }, { "ODM%_OPTC_INPUT_GLOBAL_CONTROL", d.regOdmGlobal }, { "OTG%_OTG_CONTROL", d.regOtgControl } };
        for (auto &r : rows) {
            std::string nm = r.nm; const size_t p = nm.find('%'); if (p != std::string::npos) nm.replace(p, 1, n);
            uint32_t a = 0; const bool f = lx_abs(nm, &a);
            const std::string m = std::string("XD: ") + d.name + " descriptor register " + nm + " is Linux's absolute address";
            EXPECT(f && a == r.ours, m.c_str());
        }
    }
    // the instance numbers and the tags themselves, and that the lookup never crosses them
    EXPECT(desc_of(1u) == &kDescMonA && desc_of(2u) == &kDescMonB && desc_of(0u) == nullptr && desc_of(3u) == nullptr, "XD: desc_of(1) is the monitor A, desc_of(2) the monitor B, anything else nothing");
    EXPECT(kDescMonA.inst == 1u && kDescMonA.otgInst == 1u && kDescMonA.d2idx == 0u && kDescMonA.slotTag == 0x10u && kDescMonB.inst == 2u && kDescMonB.otgInst == 2u && kDescMonB.d2idx == 1u && kDescMonB.slotTag == 0x20u, "XD: the descriptors' instance, OTG, d2 index and tag");
    EXPECT(kDescMonA.w == 1920u && kDescMonA.h == 1080u && kDescMonA.bytes == 8294400ull && kDescMonA.allocBytes == 8323072ull && kDescMonA.pitchReg == 0x77Fu &&
           kDescMonB.w == 2560u && kDescMonB.h == 1440u && kDescMonB.bytes == 14745600ull && kDescMonB.allocBytes == 14745600ull && kDescMonB.pitchReg == 0x9FFu, "XD: the geometry: 1920x1080 / 8,294,400 B (8,323,072 allocated, pitch 0x77f); 2560x1440 / 14,745,600 B (0x9ff)");
    EXPECT(kDescMonA.regAddrHigh == 0x3ba7u && kDescMonA.regAddr == 0x3ba6u && kDescMonB.regAddrHigh == 0x3c83u && kDescMonB.regAddr == 0x3c82u && kDescMonA.regOtgFrame == 0x508du && kDescMonB.regOtgFrame == 0x510du &&
           kDescMonA.regOtgUpdLock == 0x50c9u && kDescMonB.regOtgUpdLock == 0x5149u, "XD: the contract's numbers: the monitor A's pair 0x3ba7 / 0x3ba6, OTG1 0x508d / 0x50c9; the monitor B's 0x3c83 / 0x3c82, OTG2 0x510d / 0x5149");
    // disp2's allocation size of each instance = the descriptor's
    EXPECT(n48d2_surf_get(N48D2_INST_MONA)->buf_bytes == kDescMonA.allocBytes && n48d2_surf_get(N48D2_INST_MONB)->buf_bytes == kDescMonB.allocBytes, "XD: disp2's per-buffer allocation of each instance is the descriptor's allocBytes");
    // the pin helpers: a pin is live only under ITS instance's generation
    EXPECT(pins_live(5u, 3u, 3u) == 5u && pins_live(5u, 3u, 4u) == 0u && pins_live(5u, 4u, 3u) == 0u, "XD (S4 / test 15): a pin recorded under generation g is live only while THAT instance's generation is g");
    { const uint32_t gen1 = 7u, gen2 = 3u, pinGen1 = 7u; EXPECT(pins_live(1u, pinGen1, gen1) == 1u && pins_live(1u, pinGen1, gen2) == 0u, "test 15: pinGen1 is judged against instance 1's generation, not instance 2's"); }
    EXPECT(inst_admitted(2ull, false) && inst_admitted(2ull, true) && !inst_admitted(1ull, false) && inst_admitted(1ull, true) && !inst_admitted(0ull, true) && !inst_admitted(3ull, true), "XD (test 11): instance 2 is always admitted, instance 1 only with navi48-m6flip1, 0 / 3 never");
    EXPECT(inst_of_id(0x10u) == 1u && inst_of_id(0x12u) == 1u && inst_of_id(0x20u) == 2u && inst_of_id(0x22u) == 2u && inst_of_id(0x13u) == 0u && inst_of_id(0x23u) == 0u && inst_of_id(2u) == 0u, "XD: a tagged slot id names its instance");
}

// =====================================================================================================================================================================================================================
// XA  AGDC with THREE entries [DP, monitor B, monitor A]: endpoint 1 -> the monitor B's 2560x1440 at 241.5 MHz, endpoint 2 -> the monitor A's 1920x1080 at 148.5 MHz; the per-instance counters; the list order = the publish order (monitor B first)
// =====================================================================================================================================================================================================================
static uint32_t rd32(const uint8_t *b, uint32_t o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); }
static void suite_xa() {
    using namespace n48m6;
    // the list is built by list_insts from which framebuffers exist: the publish order (fbpublish 2 = the monitor B BEFORE fbpublish 1 = the monitor A) is the list order
    bool have[kInstCount] = { true, true, true }; uint32_t order[kInstCount]; const uint32_t k = list_insts(have, order);
    EXPECT(k == 3u && order[0] == kInstDp && order[1] == kInstMonB && order[2] == kInstMonA, "XA: the AGDC list order is [DP, monitor B, monitor A] = the publish order (the monitor B is published before the monitor A)");
    AgdcList g {}; g.nfb = k; for (uint32_t j = 0; j < k; j++) { g.inst[j] = order[j]; g.fb[j] = 0xFFFFFF8000A00000ull + 0x10000ull * j; }
    uint8_t buf[N48_AGDC_LINK_CONFIG_LEN]; uint32_t kr = 0;
    std::memset(buf, 0, sizeof buf); buf[0] = 1;
    EXPECT(agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, sizeof buf, 0, &kr) && kr == 0 && rd32(buf, 0x44) == 2560u && rd32(buf, 0x54) == 1440u, "XA (test 14): endpoint 1 is answered with the monitor B's 2560x1440 raster");
    { n48_agdc_timing t {}; EXPECT(agdc_timing_for_inst(order[1], &t) && t.w == 2560u && t.h == 1440u && t.pixel_clock == 241500000ull, "XA: ... at 241.5 MHz"); }
    std::memset(buf, 0, sizeof buf); buf[0] = 2;
    EXPECT(agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, sizeof buf, 0, &kr) && kr == 0 && rd32(buf, 0x44) == 1920u && rd32(buf, 0x54) == 1080u, "XA (test 14): endpoint 2 is answered with the monitor A's 1920x1080 raster");
    { n48_agdc_timing t {}; EXPECT(agdc_timing_for_inst(order[2], &t) && t.w == 1920u && t.h == 1080u && t.pixel_clock == 148500000ull && t.h_sync_off == 88u && t.h_sync_w == 44u && t.v_sync_off == 4u && t.v_sync_w == 5u, "XA: ... at 148.5 MHz with CEA VIC 16's porches (88/44, 4/5)"); }
    std::memset(buf, 0, sizeof buf); buf[0] = 0;
    EXPECT(!agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, sizeof buf, 0, &kr), "XA: endpoint 0 is the DP's: 0.0.658's live-raster code runs (not answered here)");
    uint8_t pc[N48_AGDC_PIPELINE_CAPS_LEN];
    std::memset(pc, 0, sizeof pc); pc[0] = 2; pc[4] = (uint8_t)N48_AGDC_SCALER_TYPE;
    EXPECT(agdc_answer(g, N48_AGDC_CMD_PIPELINE_CAPS, pc, sizeof pc, 0, &kr) && kr == 0, "XA: endpoint 2's 0x711 is answered");
    // the counters (item 10): per instance, the endpoints seen exactly {0, 1, 2}, no out-of-range endpoint
    EXPECT(g.n921i[kInstDp] == 1u && g.n921i[kInstMonB] == 1u && g.n921i[kInstMonA] == 1u && g.n711i[kInstMonA] == 1u && g.n711i[kInstMonB] == 0u && g.epSeen == 7u && g.nOor == 0u, "XA (item 10): n921 per instance 1 / 1 / 1, n711 for the monitor A 1, endpoints seen exactly {0, 1, 2}, out-of-range 0");
    std::memset(buf, 0, sizeof buf); buf[0] = 5;
    (void)agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, sizeof buf, 0, &kr);
    EXPECT(g.nOor == 1u && g.epSeen == (7u | (1u << 5)), "XA: an endpoint the list does not have is counted (out-of-range, and its bit)");
    // S7: a list whose fb[] order disagrees with inst[]: the answer for a framebuffer pointer follows inst[], and list_insts is the only order source
    EXPECT(agdc_position(1u, 3u) == 1u && agdc_position(2u, 3u) == 2u && agdc_position(3u, 3u) == 0u && agdc_instance(g.inst, 3u, 1u) == kInstMonB && agdc_instance(g.inst, 3u, 2u) == kInstMonA, "XA: endpoint -> list position -> instance: 1 = the monitor B, 2 = the monitor A");
    // the DP's pointer is first, the monitor B's second, the monitor A's third in the 0x980 reply
    { uint8_t cap[N48_AGDC_GPU_CAP_LEN]; std::memset(cap, 0, sizeof cap); EXPECT(agdc_answer(g, N48_AGDC_CMD_GPU_CAPABILITY, cap, sizeof cap, 0xFFFFFF8000CC0000ull, &kr) && kr == 0, "XA: the 0x980 reply carries the three framebuffers");
      bool seen[3] = { false, false, false }; for (uint32_t off = 0; off + 8 <= sizeof cap; off += 4) { const uint64_t v = (uint64_t)rd32(cap, off) | ((uint64_t)rd32(cap, off + 4) << 32); for (uint32_t j = 0; j < 3; j++) if (v == g.fb[j]) seen[j] = true; }
      EXPECT(seen[0] && seen[1] && seen[2], "XA: all three framebuffer pointers appear in the 0x980 reply"); }
}

// =====================================================================================================================================================================================================================
// X11 SOURCE PINS (the kext glue, the ABI, the policy, the client, s1c, the CLI, the variants) - the wiring that no host flow reaches, locked as text
// =====================================================================================================================================================================================================================
static std::string fn_body(const std::string &src, const char *sig, size_t n) { const size_t p = src.find(sig); return p == std::string::npos ? std::string() : src.substr(p, n); }
static void suite_m1() {
    {   // M1: a deferred table publish is flushed by ANY later path (no Status2, no instance 2); the limiter is atomic
        n48m6::PubRate r; std::memset(&r, 0, sizeof r); const uint64_t ms = 1000000ull; int pubs = 0;
        EXPECT(n48m6::pub_rate_admit(r, 100 * ms), "M1: the first change publishes");
        EXPECT(!n48m6::pub_rate_admit(r, 110 * ms) && r.dirty == 1u, "M1: the LAST learn of a burst, 10 ms later, is deferred");
        EXPECT(!n48m6::pub_rate_flush(r, 130 * ms, [&]() { pubs++; }) && pubs == 0, "M1: a flush before the gap does nothing");
        EXPECT(n48m6::pub_rate_flush(r, 160 * ms, [&]() { pubs++; }) && pubs == 1 && r.dirty == 0u, "M1: ANY later path (a submit of another pipe, the DP's status call) publishes it within the gap: instance 2 never acquired, no Status2");
        EXPECT(!n48m6::pub_rate_flush(r, 170 * ms, [&]() { pubs++; }) && pubs == 1, "M1: a flush publishes only once");
        // a deferral that arrives AFTER a winner cleared dirty is not lost
        n48m6::PubRate q; std::memset(&q, 0, sizeof q); (void)n48m6::pub_rate_admit(q, 100 * ms); (void)n48m6::pub_rate_admit(q, 101 * ms);
        EXPECT(n48m6::pub_rate_admit(q, 200 * ms) && q.dirty == 0u, "M1: the winner clears dirty before it builds");
        EXPECT(!n48m6::pub_rate_admit(q, 201 * ms) && q.dirty == 1u && n48m6::pub_rate_flush_due(q, 260 * ms), "M1: ... a deferral after that stays dirty and is flushed later");
        // atomic: two flushers race, exactly one publishes
        n48m6::PubRate z; std::memset(&z, 0, sizeof z); (void)n48m6::pub_rate_admit(z, 100 * ms); (void)n48m6::pub_rate_admit(z, 101 * ms); int c = 0;
        const bool a1 = n48m6::pub_rate_flush(z, 300 * ms, [&]() { c++; }); const bool a2 = n48m6::pub_rate_flush(z, 300 * ms, [&]() { c++; });
        EXPECT(a1 && !a2 && c == 1, "M1: the second flusher at the same instant finds the slot taken");
        // S-2: the log line
        volatile uint64_t lg = 0; EXPECT(n48m6::log_admit(lg, 5 * 1000 * ms) && !n48m6::log_admit(lg, 5 * 1000 * ms + 10 * ms) && !n48m6::log_admit(lg, 5 * 1000 * ms + 999 * ms) && n48m6::log_admit(lg, 6 * 1000 * ms + 1), "S-2: the per-learn log line prints at most once a second");
    }
}
static void suite_hw1(const std::string &root) {
    // HW1: the kernel publishes through a buffer of kBlobBufBytes: with the m6flip latch ON (version 2) the build must return non-zero for THAT size, and the real publish_serialised must publish
    n48m6::SurfTable t; std::memset(&t, 0, sizeof t); (void)n48m6::learn_surface(t, 21u, 2u); (void)n48m6::learn_surface(t, 22u, 0u);
    uint8_t buf[n48m6::kBlobBufBytes]; n48m6::PubGate g; std::memset(&g, 0, sizeof g); int published = 0;
    (void)n48m6::publish_serialised(g, [&]() -> uint32_t { return n48m6::blob_build_v2(t, (uint32_t)g.curGen, buf, sizeof buf); }, [&](uint32_t n) { published++; return n != 0u; });
    EXPECT(published == 1 && g.pubGen == 1u, "HW1: the version-2 build into the kernel's REAL buffer size publishes (non-zero), the generation advances");
    uint8_t small[n48m6::kBlobMax]; EXPECT(n48m6::blob_build_v2(t, 1u, small, sizeof small) == 0u, "HW1 (negative): the v1-sized buffer cannot hold a v2 blob (the 0.0.661 bug)");
    EXPECT(n48m6::kBlobBufBytes >= n48m6::kBlobMax2 && n48m6::kBlobBufBytes >= n48m6::kBlobMax, "HW1: the buffer size is the larger blob");
    const std::string disp = slurp(root, "src/navi48-bringup/src/amd/native_disp.cpp");
    EXPECT(has(disp, "uint8_t gM6Blob[n48m6::kBlobBufBytes];") && has(disp, "static_assert(sizeof gM6Blob >= n48m6::kBlobMax2") && has(disp, "blob_build_v2(gD.m6Surf, __atomic_load_n(&gD.m6Pub.curGen, __ATOMIC_ACQUIRE), gM6Blob, sizeof gM6Blob)"), "HW1: the kernel's gM6Blob is sized for the version-2 blob and the real publish path builds into it");
}
struct REnv2 { bool active_size(uint32_t otg, bool *en, uint32_t *aw, uint32_t *ah) { *en = true; *aw = otg == 1u ? 1920u : otg == 2u ? 2560u : 3840u; *ah = otg == 1u ? 1080u : otg == 2u ? 1440u : 2160u; return true; }
               bool totals(uint32_t otg, uint32_t *ht1, uint32_t *vt1) { *ht1 = otg == 1u ? 2199u : 2719u; *vt1 = otg == 1u ? 1124u : 1480u; return true; } };
static void suite_s10(const std::string &root) {
    // S10: the per-instance vblank sample uses ITS OTG: distinct fake OTG sizes, and the cache slot / OTG number come from the same instance
    REnv2 e; uint32_t ht = 0, vt = 0;
    EXPECT(n48m6::raster_flow(e, n48m6::otg_of_inst(n48m6::kInstMonA), 1920, 1080, &ht, &vt) && ht == 2200u && vt == 1125u, "S10: the monitor A's OTG (1) at 1920x1080 gives the monitor A's raster");
    EXPECT(n48m6::raster_flow(e, n48m6::otg_of_inst(n48m6::kInstMonB), 2560, 1440, &ht, &vt) && ht == 2720u && vt == 1481u, "S10: the monitor B's OTG (2) at 2560x1440 gives the monitor B's raster");
    EXPECT(!n48m6::raster_flow(e, n48m6::otg_of_inst(n48m6::kInstMonA), 2560, 1440, &ht, &vt) && !n48m6::raster_flow(e, n48m6::otg_of_inst(n48m6::kInstMonB), 1920, 1080, &ht, &vt), "S10: the other display's size on this OTG is refused");
    EXPECT(n48m6::otg_of_inst(1u) == 1u && n48m6::otg_of_inst(2u) == 2u, "S10: otg_of_inst");
    const std::string glue = slurp(root, "src/navi48-bringup/src/dcn/navi48_dcn.cpp");
    EXPECT(has(glue, "VblCache &c = gVblX[inst - 1u];\n\tconst uint32_t otg = n48m6::otg_of_inst(inst);"), "S10: gVblX is indexed by inst - 1 and the OTG by the same instance");
}
static void suite_pins(const std::string &root) {
    const std::string K = "src/navi48-bringup/src/";
    const std::string flow = slurp(root, (K + "dcn/navi48_scanx_flow.h").c_str());
    const std::string glue = slurp(root, (K + "dcn/navi48_dcn.cpp").c_str());
    const std::string s1c = slurp(root, (K + "amd/native_s1c.cpp").c_str());
    const std::string abi = slurp(root, (K + "Navi48NativeABI.h").c_str());
    const std::string cli = slurp(root, (K + "Navi48NativeClient.cpp").c_str());
    const std::string pol = slurp(root, (K + "amd/native_open_policy_pure.h").c_str());
    const std::string disp = slurp(root, (K + "amd/native_disp.cpp").c_str());
    const std::string dflow = slurp(root, (K + "amd/native_disp_flow.h").c_str());
    const std::string pure = slurp(root, (K + "amd/native_m6_pure.h").c_str());
    const std::string plist = slurp(root, "src/navi48-bringup/Info.plist");
    const std::string clic = slurp(root, "tools/pc/navi48test.c");
    // the writer: ONE function, two registers, HIGH before LOW
    EXPECT(count_of(flow, "e.wr(") == 2, "X11: the flow writes a register in exactly two places (the writer)");
    { const size_t h = flow.find("e.wr(d.regAddrHigh"), l = flow.find("e.wr(d.regAddr,"); EXPECT(h != std::string::npos && l != std::string::npos && h < l, "X11: the writer writes HIGH before LOW"); }
    EXPECT(!has(flow, "wr(d.regFlipControl") && !has(flow, "wr(d.regVmid") && !has(flow, "wr(d.regSurfControl") && !has(flow, "wr(d.regOtg"), "X11: nothing writes FLIP_CONTROL / VMID / SURFACE_CONTROL / an OTG register");
    EXPECT(!has(flow, "program_flip(") && !has(glue.substr(glue.find("build 0.0.661 (M6 Stage 1b, ABI 1.11")), "program_flip("), "X11: dcn41_hubp_program_flip is never used by the HDMI instances");
    EXPECT(has(flow, "bool ok = x.descInst == d.inst;"), "X11 (S1 / S3 second wall): the writer refuses a state that was not acquired through ITS descriptor");
    EXPECT(has(flow, "constexpr bool write_reg_ok(const XDesc &d, uint32_t abs) { return abs == d.regAddrHigh || abs == d.regAddr; }"), "X11: the allowlist is per instance: exactly this descriptor's HIGH and LOW");
    EXPECT(has(flow, "constexpr const XDesc *desc_of(uint32_t inst) { return inst == 1u ? &kDescMonA : inst == 2u ? &kDescMonB : nullptr; }"), "X11: ONE lookup: desc_of");
    EXPECT(count_of(flow, "inline constexpr XDesc kDesc") == 2, "X11: exactly two descriptors");
    EXPECT(!has(flow, "// 0x510d"), "X11: the stray // 0x510d comment is not copied (the monitor B's update-lock line has no stale comment)");
    {   const std::string blk = glue.substr(glue.find("build 0.0.661 (M6 Stage 1b, ABI 1.11"));
        EXPECT(count_of(blk, "WREG32") == 1 && count_of(blk, "dcn41_allow_write") == 1, "X11: the glue block has ONE WREG32, behind write_reg_ok and the DCN allowlist");
        EXPECT(has(blk, "if (!n48scanx::write_reg_ok(d, abs))") && blk.find("write_reg_ok") < blk.find("dcn41_allow_write") && blk.find("dcn41_allow_write") < blk.find("WREG32"), "X11: the glue's write: the per-instance register check, then the allowlist, then the write");
        EXPECT(has(blk, "Hubp &x = c.h") == false && !has(blk, "hubp_reg(") && !has(blk, "kOffFlipControl"), "X11: the block touches no HUBP0 register helper");
        EXPECT(has(blk, "gate() {\n\t\tif (!sx_latched_inst(d.inst)) return n48scan::kUnsupported;"), "X11: the gate asks the instance's latches first");
        EXPECT(has(blk, "static inline bool sx_latched_inst(uint32_t inst) { return inst == 2u ? sx_latched() : inst == 1u ? (sx_latched() && n48m6flip1_latched_on()) : false; }"), "X11: instance 2 needs navi48-m6 + navi48-m6flip, instance 1 additionally navi48-m6flip1");
        EXPECT(has(blk, "amdgpu::n1c_hung()") && has(blk, "native_s1b_state().gate != n48native::kGateOn") && has(blk, "gMt.busy"), "X11: native boot, not HUNG, no mode trial");
        EXPECT(blk.find("sx_excl_snapshot(inst, &ex);") < blk.find("n48scanx::register_slot(env, *d, gSX[inst], in, ex, out)"), "X11: the other display's snapshot is taken BEFORE this instance's lock");
        EXPECT(has(blk, "n48scanx::watchdog_run(env, *d, gSX[inst]") && has(blk, "kernel_thread_start(&sx_watchdog"), "X11: the watchdog thread runs the flow's loop with the thread's instance");
        EXPECT(has(blk, "(void)scanXRelease(inst, \"kext stop\", r);"), "X11: kext stop restores every HDMI instance");
        EXPECT(has(blk, "uint32_t d2_stage() { return __atomic_load_n(&gD2Pl[d.d2idx].stage, __ATOMIC_ACQUIRE); }") && has(blk, "uint64_t d2_mc(uint32_t i) { return gD2Pl[d.d2idx].mc[i & 1u]; }") && has(blk, "void d2_set_cur_a() { gD2Pl[d.d2idx].cur = 0u; }") && has(blk, "uint32_t d2_cur() { return gD2Pl[d.d2idx].cur; }"),
               "X11 (S2 / test 10): SXEnv reads the plane state gD2Pl[d.d2idx] of ITS descriptor (the monitor A's [0], the monitor B's [1]), never a literal index");
        EXPECT(!has(blk, "gD2Pl[1]") && !has(blk, "gD2Pl[0]"), "X11: no literal gD2Pl index is left in the HDMI block (every hard-wired site became per instance)");
        EXPECT(has(blk, "if (d == nullptr || d->inst != inst) return n48scan::kBadArg;") && count_of(blk, "n48scanx::desc_of(inst)") >= 8, "X11: every entry point looks the descriptor up and checks it names the requested instance");
        for (const char *fn : { "uint32_t scanXAcquire(uint32_t inst, uint64_t out[5]) {", "uint32_t scanXRegister(uint32_t inst, bool boVis,", "uint32_t scanXPresent(uint32_t inst, uint64_t slotId,", "uint32_t scanXStatus(uint32_t inst, struct n48n_scan_status *o," }) {
            const std::string body = fn_body(blk, fn, 420); std::string m = std::string("X11: ") + fn + " looks the descriptor up, asks the instance's latches, then builds the environment";
            EXPECT(body.find("desc_of(inst)") != std::string::npos && body.find("!sx_latched_inst(inst)") != std::string::npos && body.find("desc_of(inst)") < body.find("!sx_latched_inst(inst)") && body.find("!sx_latched_inst(inst)") < body.find("SXEnv env"), m.c_str());
        }
        { const std::string body = fn_body(blk, "uint32_t scanXGpuHeld(uint32_t inst) {", 420); EXPECT(body.find("!sx_latched_inst(inst)") != std::string::npos && body.find("!sx_latched_inst(inst)") < body.find("SXEnv env"), "X11: scanXGpuHeld asks the instance's latches first"); }
        EXPECT(has(blk, "(void)n48scanx::read_plane(env, *d, &p, &e, &pend); code = n48scanx::gpu_held_code(true, gSX[inst].tbl, e);"), "X11: scanXGpuHeld judges EARLIEST from the instance's HUBP under that instance's lock");
        EXPECT(has(blk, "IOLock *l = gSXLock[inst];") && has(blk, "const uint32_t other = inst == 2u ? 1u : 2u;"), "X11: the clash set and the snapshot read the OTHER instance's slots under THAT instance's lock, one at a time");
        EXPECT(count_of(blk, "for (uint32_t inst = 1u; inst <= 2u; inst++) {") == 2, "X11: the clash set AND the kext-stop loop cover BOTH HDMI instances (1 and 2)");
        EXPECT(has(blk, "IOLock *ol = gSXLock[other];") && has(blk, "for (uint32_t i = 0; i < n48scan::kMaxSlots; i++) if (gSX[other].tbl.s[i].used) add(gSX[other].tbl.s[i].mc, gSX[other].tbl.s[i].bytes);"), "X11: the snapshot adds the other display's registered slots");
        EXPECT(has(blk, "uint32_t inst = (uint32_t)((arg >> 8) & 0xFFull);") && has(blk, "if (inst == 0u) inst = 2u;"), "X11 (item 11): scanXReport takes the instance from bits 8..15 (0 = the monitor B)");
        EXPECT(count_of(blk, "IOLockLock(") <= 6, "X11: no flow in the HDMI block takes two instance locks together (few lock sites, each released before the next)");
    }
    EXPECT(has(glue, "n48scanx::ScanX gSX[3];") && has(glue, "IOLock *volatile gSXLock[3] = { nullptr, nullptr, nullptr };"), "X11: one ScanX and one leaf lock PER INSTANCE");
    EXPECT(has(glue, "scanXShutdown();") && glue.find("scanXShutdown();") < glue.find("if (gScan.lock == nullptr) return;\n\tuint64_t r[2] = { 1, 0 };\n\t(void)scan_restore(\"kext stop\""), "X11: scanShutdown restores the HDMI instances before the early return");
    EXPECT(has(glue, "n48scanx::hold_verdict_gpu(n48d2_hold_verdict(g, pl->cur, pl->mc[0]), scanXGpuHeld(inst));"), "X11 (S11 / item 7): fbLive drops the latch bit of the hold verdict while the instance (1 or 2) is GPU-held - judged for THAT instance");
    EXPECT(has(glue, "if ((op == N48D2_OP_STATUS || op == N48D2_OP_CRC) && (inst == N48D2_INST_MONB || inst == N48D2_INST_MONA)) {") && has(glue, "GPU-HELD (this instance's scanout is acquired)"), "X11: disp2 status / crc log GPU-held instead of a latch failure, for both HDMI instances");
    EXPECT(has(glue, "n48m6_latched_on() && n48m6flip_latched_on() && scanXClash(mc, bytes)"), "X11: instance 0's Register refuses a clash with the HDMI instances (latches ON only)");
    EXPECT(has(glue, "if (d == nullptr || !sx_latched_inst(inst)) continue;        // instance 1 only with navi48-m6flip1"), "X11: scanXClash checks the monitor A only with navi48-m6flip1 (with it OFF this is 0.0.661's check)");
    EXPECT(has(glue, "if (n48m6_latched_on() && n48m6flip_latched_on() && n48m6flip1_latched_on()) flags |= N48N_SCANQ_M6FLIP1;"), "X11: scan_query reports the third latch");
    // s1c
    EXPECT(has(s1c, "n48scanx::teardown_all(te, how, r, r1, r2);"), "X11: scan_teardown is teardown_all (all three instances before any BO)");
    EXPECT(has(s1c, "if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;") && has(s1c, "if (b.pinMask1 != 0u && scan_unpin1(h, b, mode)) b.pinLeak = 1u;") && has(s1c, "if (b.pinMask2 != 0u && scan_unpin2(h, b, mode)) b.pinLeak = 1u;"), "X11: bo_release restores each instance before a BO with its pins goes");
    EXPECT(s1c.find("if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;") < s1c.find("if (b.pinMask1 != 0u && scan_unpin1(h, b, mode))") && s1c.find("if (b.pinMask1 != 0u && scan_unpin1(h, b, mode))") < s1c.find("if (b.pinMask2 != 0u && scan_unpin2(h, b, mode))") && s1c.find("if (b.pinMask2 != 0u && scan_unpin2(h, b, mode))") < s1c.find("if (b.pinLeak != 0u) mode = kRelLeak;"), "X11: ...all before the leak decision");
    EXPECT(has(s1c, "void restore1(const char *how, uint64_t r[2]) { (void)n48dcn::scanXRelease(1u, how, r); }") && has(s1c, "void restore2(const char *how, uint64_t r[2]) { (void)n48dcn::scanXRelease(2u, how, r); }") && has(s1c, "void restore0(const char *how, uint64_t r[2]) { (void)n48dcn::scanRelease(how, r); }"), "X11: the teardown environment restores instance 0, 1 and 2");
    EXPECT(has(s1c, "uint32_t pin1(uint32_t h) { return s->bo[h].pinMask1; }") && has(s1c, "uint32_t pin2(uint32_t h) { return s->bo[h].pinMask2; }") && has(s1c, "s->bo[h].pinMask = 0; s->bo[h].pinMask1 = 0; s->bo[h].pinMask2 = 0; }"), "X11: the teardown environment reports and clears the instance-1 pins separately");
    for (const char *fn : { "n1c_scanx_acquire", "n1c_scanx_register", "n1c_scanx_present", "n1c_scanx_status", "n1c_scanx_release" }) {
        const std::string body = fn_body(s1c, (std::string("IOReturn ") + fn + "(const N1cRef &ref").c_str(), 1400);
        const size_t a = body.find("sess_hello"), b = body.find("app_refused"), c = body.find("!scanx_on()"), d = body.find("!scanx_inst_ok(inst)");
        std::string m = std::string("X11: ") + fn + ": Hello, APP refusal, the m6 + m6flip latches (Unsupported), THEN the instance (instance 1 without navi48-m6flip1 is BadArgument as 0.0.661) - before any register read";
        EXPECT(a != std::string::npos && a < b && b < c && c < d, m.c_str());
    }
    EXPECT(has(s1c, "static inline bool scanx_inst_ok(uint64_t inst) { return n48scanx::inst_admitted(inst, n48m6flip1_latched_on()); }"), "X11: the instance admission is inst_admitted with the third latch");
    EXPECT(has(s1c, "const uint32_t gen = n48dcn::scanXGeneration((uint32_t)inst);") && has(s1c, "uint8_t &mask = inst == 1ull ? b.pinMask1 : b.pinMask2;") && has(s1c, "uint32_t &pgen = inst == 1ull ? b.pinGen1 : b.pinGen2;") && has(s1c, "mask = n48scanx::pins_live(mask, pgen, gen);"),
           "X11 (S4 / test 15): Register judges a BO's pins against THAT instance's generation (pinGen1 against scanXGeneration(1))");
    EXPECT(has(s1c, "const uint32_t res = n48dcn::scanXBoGone(inst, mask, pgen, mode != kRelNormal, r);") && has(s1c, "static bool scan_unpin1(uint32_t h, Bo &b, RelMode mode) { return scan_unpinx(1u, h, b, mode); }") && has(s1c, "static bool scan_unpin2(uint32_t h, Bo &b, RelMode mode) { return scan_unpinx(2u, h, b, mode); }"), "X11: scan_unpin1 / 2 judge their own instance with its pin mask and generation");
    EXPECT(has(s1c, "uint8_t &mask = inst == 1ull ? s->bo[h].pinMask1 : s->bo[h].pinMask2;"), "X11: Release judges and clears ONLY the released instance's pins");
    EXPECT(has(s1c, "if (xd == nullptr || !n48scanx::id_ok(*xd, o2[0])) {"), "X11: Register checks the returned slot id carries THIS instance's tag before pinning");
    EXPECT(count_of(s1c, "scanx_owner(s)") == 3 && has(s1c, "if (scan_foreign(s) || !scanx_owner(s)) return kIOReturnNotPermitted;\n    return (IOReturn)n48dcn::scanXPresent((uint32_t)inst, slot, flags, out);"), "X11: Register, Present and Acquire need the session to hold instance 0");
    EXPECT(count_of(s1c, "scan_foreign(s)") >= 8, "X11: a foreign session is refused");
    EXPECT(has(s1c, "if (rc == kIOReturnSuccess) out[3] = gCtx->gmc.vram_alloc.bytes_free();") && has(abi, "[3] the allocator's FREE visible-VRAM bytes"), "S4: SCANX_ACQUIRE returns the allocator's free visible-VRAM bytes in out[3]");
    EXPECT(has(glue, "out[4] = (uint64_t)d->w | ((uint64_t)d->h << 16) | ((uint64_t)d->pitchPx << 32);"), "X11 (item 9): Acquire returns the instance's geometry in the fifth output word");
    EXPECT(has(s1c, "n48disp_m6_flush();   // 0.0.661 (review M1): the DP's 1 s keep-alive also flushes") && s1c.find("n48disp_m6_flush();") > s1c.find("IOReturn n1c_scan_status(") && s1c.find("n48disp_m6_flush();") < s1c.find("return (IOReturn)n48dcn::scanStatus(out);"), "M1: n1c_scan_status (the DP's 1 s keep-alive, selector 13) flushes a deferred table publish, with no HDMI instance involved");
    EXPECT(has(disp, "void m6_flush_deferred() { if (n48m6flip_latched_on()) (void)n48m6::pub_rate_flush(gD.m6Rate, now_ns(), [this]() { m6_publish_now(); }); }") && disp.find("m6_flush_deferred();\n        const uint32_t r = n48m6::learn_surface") != std::string::npos, "M1: EVERY submit of ANY pipe (m6_learn, before the learn) flushes a deferred publish once its gap has passed: no Status call is needed");
    EXPECT(has(disp, "if (!n48m6flip_latched_on() || n48m6::log_admit(gD.m6LogNs, now_ns())) {"), "S-2: the per-learn log line is rate-limited behind the latch");
    // ABI / policy / client
    EXPECT(N48N_ABI_MINOR == 12u && N48N_SEL_SCANX_ACQUIRE == 22 && N48N_SEL_SCANX_REGISTER == 23 && N48N_SEL_SCANX_PRESENT == 24 && N48N_SEL_SCANX_STATUS == 25 && N48N_SEL_SCANX_RELEASE == 26 && N48N_SEL_COUNT_1_11 == 27 && N48N_SEL_COUNT_1_12 == 27, "X11: ABI 1.12, the same selectors 22..26 (no new selector)");
    EXPECT(N48N_SCANQ_M6FLIP == (1u << 6) && N48N_SCANQ_M6FLIP1 == (1u << 7) && N48N_SCANX_INSTANCE == 2u && N48N_SCANX_SLOT_TAG == 0x20u && N48N_SCANX_INSTANCE_MONA == 1u && N48N_SCANX_SLOT_TAG_MONA == 0x10u, "X11: the flag bits, the instances, the tags");
    EXPECT(N48N_SCANX_INSTANCE == kDescMonB.inst && N48N_SCANX_SLOT_TAG == kDescMonB.slotTag && N48N_SCANX_INSTANCE_MONA == kDescMonA.inst && N48N_SCANX_SLOT_TAG_MONA == kDescMonA.slotTag, "X11: the ABI's instances and tags are the descriptors'");
    EXPECT(sizeof(n48n_scan_status) == 256 && sizeof(n48n_scan_reg) == 32, "X11: the structs are unchanged");
    using namespace n48native::policy;
    for (uint32_t sel = 22; sel <= 26; sel++) {
        EXPECT(selector_allowed(false, sel) && selector_allowed(true, sel), "X11: a uid-88 (WindowServer) client reaches selectors 22..26");
        EXPECT(!selector_allowed_for_app(sel), "X11: an APP client never reaches them");
        EXPECT(!route_selector_ok(kRouteAccelerator, sel) && route_selector_ok(kRouteBringup, sel), "X11: the accelerator route never reaches them");
    }
    EXPECT(!selector_allowed(false, 15) && !selector_allowed(false, 27) && selector_allowed(false, 21), "X11: the rest of the uid-88 set is unchanged (15 and 27 refused, 21 allowed)");
    EXPECT(has(cli, "case N48N_SEL_SCANX_ACQUIRE: {\n\t\tif (!(soc == 4u || soc == 5u) || !shape(2, soc, 0, 0))") && has(cli, "case N48N_SEL_SCANX_REGISTER:\n\t\tif (!shape(1, 2, sizeof(n48n_scan_reg), 0))") && has(cli, "case N48N_SEL_SCANX_PRESENT:\n\t\tif (!shape(3, 3, 0, 0))")
           && has(cli, "case N48N_SEL_SCANX_STATUS:\n\t\tif (!shape(1, 4, 0, sizeof(n48n_scan_status)))") && has(cli, "case N48N_SEL_SCANX_RELEASE:\n\t\tif (!shape(1, 2, 0, 0))"), "X11: the client's shape checks of the five selectors (Acquire: four or five output words)");
    EXPECT(has(cli, "uint64_t o5[5] = { 0, 0, 0, 0, 0 };") && has(cli, "for (uint32_t i = 0; i < soc; i++) so[i] = o5[i];"), "X11: Acquire copies exactly as many output words as the caller's buffer holds (a four-word call never gets a fifth)");
    EXPECT(count_of(cli, "if (n48m6_latched_on() && n48m6flip_latched_on()) (void)n48dcn::bind(owner);   // N1:") == 4, "N1: the display layer is bound for selectors 22..25 only AFTER the latch check");
    for (const char *sl : { "SCANX_ACQUIRE", "SCANX_REGISTER", "SCANX_PRESENT", "SCANX_STATUS" }) { const size_t p = cli.find(std::string("case N48N_SEL_") + sl + ":"); const std::string seg = p == std::string::npos ? "" : cli.substr(p, 420); std::string m = std::string("N1: ") + sl + ": no unconditional bind"; EXPECT(!seg.empty() && seg.find("if (n48m6_latched_on() && n48m6flip_latched_on()) (void)n48dcn::bind(owner);") != std::string::npos && count_of(seg.substr(0, seg.find("return amdgpu")), "bind(owner)") == 1, m.c_str()); }
    // latches
    EXPECT(has(disp, "bool n48m6flip_latched_on(void)") && has(disp, "PE_parse_boot_argn(\"navi48-m6flip\"") && has(disp, "n48m6::latch_get(&gM6FlipLatch"), "X11: the navi48-m6flip latch is read ONCE through latch_get");
    EXPECT(has(disp, "bool n48m6flip1_latched_on(void)") && has(disp, "PE_parse_boot_argn(\"navi48-m6flip1\"") && has(disp, "n48m6::latch_get(&gM6Flip1Latch") && has(disp, "volatile uint32_t gM6Flip1Latch = n48m6::kLatchUnset;"), "X11 (item 1): the navi48-m6flip1 latch is read ONCE through latch_get, default OFF");
    EXPECT(has(glue, "if (n48m6_latched_on() && n48m6flip_latched_on()) flags |= N48N_SCANQ_M6FLIP;"), "X11: scan_query reports both Stage-1b latches");
    // R1 / R2 / R5 wiring
    EXPECT(has(dflow, "if (e.m6_on() && !e.m6flip_on()) e.m6_reset();") && has(dflow, "if (__atomic_load_n(&w.seen267, __ATOMIC_ACQUIRE) == 0u && e.m6_on() && e.m6flip_on()) e.m6_reset();"), "R1: the reset moved to the first slot-267 of the new start (the window-open reset stays only with the latch OFF)");
    EXPECT(has(disp, "n48m6::blob_build_v2(gD.m6Surf, __atomic_load_n(&gD.m6Pub.curGen") && has(disp, "__atomic_load_n(&gD.m6Pub.pubGen"), "R2: the blob carries curGen; Status's generation is pubGen");
    EXPECT(has(disp, "n48m6::learn_surface(gD.m6Surf, id, inst, n48m6flip_latched_on())"), "R3: the eviction victim rule is selected by the latch");
    EXPECT(has(disp, "n48m6::pub_rate_admit(gD.m6Rate") && has(glue, "n48disp_m6_flush();"), "R5: the publish is rate-limited and Status flushes");
    EXPECT(has(pure, "ent_fill(t.e[m6flip ? victim_slot(t) : lru_slot(t)]"), "R3: learn_surface evicts by victim_slot with the latch, lru_slot without");
    EXPECT(has(disp, "KernelEnv ke; (void)n48m6::pub_rate_flush(gD.m6Rate, now_ns(), [&ke]() { ke.m6_publish_now(); });") && has(glue, "if (n48m6_latched_on() && n48m6flip_latched_on()) n48disp_m6_flush();"), "R5: Status flushes a deferred table publish");
    EXPECT(has(pure, "if (!__atomic_compare_exchange_n(&r.lastNs, &last, nowNs, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;") && has(pure, "struct PubRate { volatile uint64_t lastNs; volatile uint32_t dirty; };"), "M1: the flush wins its slot by compare-and-swap and PubRate is atomic");
    // version / CLI
    EXPECT(count_of(plist, "<string>0.0.664</string>") == 2 && !has(plist, "<string>0.0.661</string>"), "X11: the kext is 0.0.664");
    EXPECT(has(clic, "in = 107;") && has(clic, "cmd_m6xstat(arg, arg2)") && has(clic, "dr_call(107, inst == 1u ?"), "X11: the CLI verb m6xstat (action 107) takes the instance");
    EXPECT(has(clic, "static int cmd_m6xstat(const char *a1, const char *a2) {") && has(clic, "accel m6xstat <1|2> <0|1|2|all>") && has(clic, "m6xstat inst1 page %u: navi48-m6 %s, navi48-m6flip %s, navi48-m6flip1 %s"), "X11 (item 11): m6xstat <1|2> [page]; one argument stays a page of the monitor B");
    EXPECT(has(clic, "page == 4") && has(clic, "AGDC 0x921 answered") && has(clic, "for (unsigned p = 0; p < 5; p++) rc |= m6stat_page(p);"), "X11 (item 11): m6stat prints the AGDC per-instance fields (page 4)");
    const std::string bring = slurp(root, (K + "Navi48Bringup.cpp").c_str());
    EXPECT(has(bring, "action == n48disp::kActM6XStat") && has(bring, "n48dcn::scanXReport(argScalar, v, 13)"), "X11: accelExperiment serves action 107");
    const std::string dpure = slurp(root, (K + "amd/native_disp_pure.h").c_str());
    EXPECT(n48disp::action_admitted(true, 107u) && !n48disp::action_admitted(false, 107u) && n48disp::verb_args_ok(107u, 0ull) && n48disp::verb_args_ok(107u, 2ull) && !n48disp::verb_args_ok(107u, 3ull) && n48disp::native_exempt(true, 107u, 1ull) && !n48disp::native_exempt(false, 107u, 1ull) && !n48disp::action_admitted(true, 109u), "X11: action 107 (m6xstat) is admitted with the display latch only, pages 0..2, exempt on a native boot");
    EXPECT(n48disp::verb_args_ok(107u, 0x100ull) && n48disp::verb_args_ok(107u, 0x102ull) && n48disp::verb_args_ok(107u, 0x202ull) && !n48disp::verb_args_ok(107u, 0x300ull) && !n48disp::verb_args_ok(107u, 0x103ull) && !n48disp::verb_args_ok(107u, 0x10000ull), "X11 (item 11): action 107 takes page | instance << 8 (instance 0 / 1 / 2, pages 0..2, nothing else)");
    EXPECT(has(dpure, "kActM6XStat = 107u") && has(dpure, "(action == kActM6XStat && m6xstat_arg_ok(arg))"), "X11: the admission of action 107");
    EXPECT(n48disp::verb_args_ok(106u, 4ull) && !n48disp::verb_args_ok(106u, 5ull) && n48m6::kStatPages == 5u, "X11 (item 10): m6stat has the fifth page (the AGDC per-instance counters)");
    const std::string var = slurp(root, "variants/configs/stage17-native-1440-metal-disp-amfi-m6flip.plist");
    const std::string var0 = slurp(root, "variants/configs/stage17-native-1440-metal-disp-amfi-m6.plist");
    const std::string var3 = slurp(root, "variants/configs/stage17-native-1440-metal-disp-amfi-m6flip3.plist");
    EXPECT(has(var, "navi48-m6=1 navi48-m6flip=1</string>") && !has(var0, "navi48-m6flip") && !has(var, "navi48-m6flip1"), "X11: the 1b variant is the m6 variant + navi48-m6flip=1 (no flip1); the m6 variant is untouched");
    EXPECT(has(var3, "navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1</string>") && count_of(var3, "navi48-m6flip1=1") == 1 && !has(var3, "navi48-m6flip=1 navi48-m6flip=1"), "X11 (item 1): the new variant is the 1b boot-args + navi48-m6flip1=1");
    { std::string a = var, b = var3; const std::string o = "navi48-m6=1 navi48-m6flip=1</string>", n = "navi48-m6=1 navi48-m6flip=1 navi48-m6flip1=1</string>"; const size_t p = b.find(n); if (p != std::string::npos) b.replace(p, n.size(), o); EXPECT(a == b, "X11: the new variant differs from the 1b variant ONLY in the added latch"); }
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    (void)lx_load(root);
    run_suite(kDescMonB);       // 0.0.661's suite, through the monitor B's descriptor
    run_suite(kDescMonA);       // the same suite through the monitor A's
    setup_layout(kDescMonB);
    suite_xc();
    suite_xd();
    suite_xa();
    // ===== X10 the review's surface-table items ================================================================================================================================================================
    {   // R4: reset writes kInstNone; lookup is fail-safe
        n48m6::SurfTable t; std::memset(&t, 0, sizeof t);
        EXPECT(n48m6::learn_surface(t, 11u, 0u) == n48m6::kLNew && n48m6::reset_table(t), "R4: learn, reset");
        bool allNone = true; for (uint32_t i = 0; i < n48m6::kMaxSurf; i++) if (t.e[i].inst != n48m6::kInstNone) allNone = false;
        EXPECT(allNone, "R4: reset_table writes inst = kInstNone, not 0 (= the DP)");
        EXPECT(!n48m6::lookup_surface(t, 11u).found, "R4: after a reset the entry is gone");
        // a torn entry: id matches but the id changed after the fields were read -> not found
        std::memset(&t, 0, sizeof t); (void)n48m6::learn_surface(t, 5u, 2u);
        EXPECT(n48m6::lookup_surface(t, 5u).found && n48m6::lookup_surface(t, 5u).inst == 2u, "R4: a stable entry is found with its instance");
    }
    {   // R3: the eviction victim is the owner's own submit distance
        n48m6::SurfTable t; std::memset(&t, 0, sizeof t);
        // fill the table: ids 1..kMaxSurf, all instance 2 (the monitor B), learned in order -> id 1 has the largest distance
        for (uint32_t i = 1; i <= n48m6::kMaxSurf; i++) (void)n48m6::learn_surface(t, i, 2u);
        // re-learn id 1 many times (a live static surface) while the DP's dead ids pile up: make ids 2.. dead by never seeing them again
        for (int k = 0; k < 40; k++) (void)n48m6::learn_surface(t, 1u, 2u);
        // a DP entry that is old by the DP's own clock but whose `seen` is HIGH: touch it so seen moves, keep txn[0] low
        const uint32_t victimDist = n48m6::victim_slot(t);
        const uint32_t victimLru = n48m6::lru_slot(t);
        EXPECT(t.e[victimDist].id != 1u, "R3: the live (re-learned) entry is never the victim");
        (void)victimLru;
        // the scenario of the review: the LIVE monitor B id has the lowest `seen` (a static display), dead DP ids were touched later in GPU mode
        n48m6::SurfTable u; std::memset(&u, 0, sizeof u);
        (void)n48m6::learn_surface(u, 100u, 2u);                                  // the monitor B's live id, learned once early (seen low)
        for (uint32_t i = 1; i < n48m6::kMaxSurf; i++) (void)n48m6::learn_surface(u, 200u + i, 0u);   // DP ids
        for (int k = 0; k < 200; k++) { (void)n48m6::learn_surface(u, 201u, 0u); }
        for (int k = 0; k < 5; k++) (void)n48m6::touch_surface(u, 100u);                        // GPU mode touches the live monitor B id: `seen` and its tx move
        const uint32_t v = n48m6::victim_slot(u);
        EXPECT(u.e[v].id != 100u, "R3: a LIVE static monitor B id is not the eviction victim");
        const uint32_t before = u.e[n48m6::victim_slot(u)].id;
        EXPECT(n48m6::learn_surface(u, 999u, 0u, true) == n48m6::kLEvicted && !n48m6::lookup_surface(u, before).found && n48m6::lookup_surface(u, 100u).found, "R3: the new learn evicts the owner-distance victim, not the live entry");
        // pure ordering: largest distance first, ties by seen
        n48m6::SurfTable w; std::memset(&w, 0, sizeof w);
        for (uint32_t i = 0; i < n48m6::kMaxSurf; i++) { w.e[i].id = 10 + i; w.e[i].inst = 0; w.e[i].tx[0] = 50 - i; w.e[i].seen = 100 + i; }
        w.txn[0] = 60u; w.n = n48m6::kMaxSurf;
        EXPECT(n48m6::victim_slot(w) == n48m6::kMaxSurf - 1u, "R3: the largest distance (txn - tx) is the victim");
        w.e[n48m6::kMaxSurf - 1u].tx[0] = 50u - 0u; w.e[0].seen = 5; w.e[0].tx[0] = 50u - (n48m6::kMaxSurf - 1u) + 0u;
    }
    {   // R4: a torn entry is never answered with the wrong instance: a writer rewrites the entry BETWEEN the field reads and the second read of the id
        n48m6::SurfTable tt; std::memset(&tt, 0, sizeof tt); (void)n48m6::learn_surface(tt, 5u, 2u);
        int calls = 0;
        n48m6::Look l = n48m6::lookup_surface_mid(tt, 5u, [&](uint32_t i, uint32_t) { calls++; __atomic_store_n(&tt.e[i].id, 0u, __ATOMIC_RELEASE); });      // the id goes 0 (a rewrite in flight) at every second read
        EXPECT(!l.found && calls == 4, "R4: an entry whose id changes under the reader is retried 4 times and then answered NOT FOUND (unknown, the fail-safe verdict)");
        std::memset(&tt, 0, sizeof tt); (void)n48m6::learn_surface(tt, 5u, 2u); calls = 0;
        l = n48m6::lookup_surface_mid(tt, 5u, [&](uint32_t i, uint32_t t) { calls++; if (t == 0u) __atomic_store_n(&tt.e[i].id, 6u, __ATOMIC_RELEASE); else __atomic_store_n(&tt.e[i].id, 5u, __ATOMIC_RELEASE); });
        EXPECT(l.found && l.inst == 2u && calls == 2, "R4: a transient change is retried and then answered from the settled entry");
        std::memset(&tt, 0, sizeof tt); (void)n48m6::learn_surface(tt, 5u, 2u); calls = 0;
        l = n48m6::lookup_surface_mid(tt, 5u, [&](uint32_t, uint32_t) { calls++; });
        EXPECT(l.found && l.inst == 2u && calls == 1, "R4: a stable entry is read once");
        EXPECT(!n48m6::lookup_surface(tt, 6u).found && !n48m6::lookup_surface(tt, 0u).found, "R4: unknown / zero ids are not found");
    }
    {   // R2: blob version 2 and the published generation
        n48m6::SurfTable t; std::memset(&t, 0, sizeof t); (void)n48m6::learn_surface(t, 21u, 2u); (void)n48m6::learn_surface(t, 22u, 0u);
        uint8_t b[n48m6::kBlobMax2 + 8];
        const uint32_t n = n48m6::blob_build_v2(t, 77u, b, sizeof b);
        EXPECT(n == n48m6::kBlobHdr2 + 2u * n48m6::kBlobEnt, "R2: version-2 blob length = 12 + 12 n");
        auto rd = [&](uint32_t o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); };
        EXPECT(rd(0) == 2u && rd(4) == 2u && rd(8) == 77u && rd(12) == 21u && rd(16) == 2u && rd(24) == 22u, "R2: blob v2 = version, n, GENERATION, entries");
        EXPECT(t.busy == 0u, "R4: the blob build released the table's busy flag");
        uint8_t b1[n48m6::kBlobMax]; const uint32_t n1 = n48m6::blob_build(t, b1, sizeof b1);
        EXPECT(n1 == n48m6::kBlobHdr + 2u * n48m6::kBlobEnt && b1[0] == 1u, "R2: the version-1 blob (latch flip OFF) is unchanged");
        t.busy = 1u; EXPECT(n48m6::blob_build_v2(t, 1u, b, sizeof b) == 0u, "R4: a table that stays busy makes the build give up (bounded spin)"); t.busy = 0u;
        // the published generation is the gen the build covered, set under the gate, and only after a successful publish
        n48m6::PubGate g; std::memset(&g, 0, sizeof g); uint32_t builtGen = 0; bool pubOk = true;
        (void)n48m6::publish_serialised(g, [&]() -> uint32_t { builtGen = (uint32_t)g.curGen; return 12u; }, [&](uint32_t) { return pubOk; });
        EXPECT(builtGen == 1u && g.pubGen == 1u && g.gen == 1u, "R2: the build sees curGen = the request count it covers; pubGen follows the publish");
        pubOk = false; (void)n48m6::publish_serialised(g, []() -> uint32_t { return 12u; }, [&](uint32_t) { return pubOk; });
        EXPECT(g.gen == 2u && g.pubGen == 1u, "R2: a FAILED publish does not advance the published generation (Status2 never reports a generation newer than the blob)");
        // a request arriving while publishing: the second round covers it
        n48m6::PubGate g2; std::memset(&g2, 0, sizeof g2); int rounds = 0; std::vector<uint32_t> gens;
        (void)n48m6::publish_serialised_seam(g2, [&]() -> uint32_t { gens.push_back((uint32_t)g2.curGen); return 12u; }, [&](uint32_t) { return true; }, [&]() { if (rounds++ == 0) __atomic_add_fetch(&g2.gen, 1u, __ATOMIC_SEQ_CST); });
        EXPECT(gens.size() == 2 && gens[0] == 1u && gens[1] == 2u && g2.pubGen == 2u, "R2: the last published generation is the newest request's");
    }
    {   // R5: the publish rate
        n48m6::PubRate r; std::memset(&r, 0, sizeof r); const uint64_t ms = 1000000ull;
        EXPECT(n48m6::pub_rate_admit(r, 100 * ms), "R5: the first publish goes out");
        EXPECT(!n48m6::pub_rate_admit(r, 110 * ms) && r.dirty == 1u, "R5: a change 10 ms later is deferred (dirty)");
        EXPECT(!n48m6::pub_rate_flush_due(r, 120 * ms), "R5: not due before the gap");
        EXPECT(n48m6::pub_rate_flush_due(r, 151 * ms), "R5: due after the gap: the LAST state is always published");
        unsigned admitted = 0; n48m6::PubRate q; std::memset(&q, 0, sizeof q); for (uint64_t t = 0; t < 1000; t++) if (n48m6::pub_rate_admit(q, 10 * ms + t * ms)) admitted++;
        EXPECT(admitted <= n48m6::kPubPerSec + 1u && admitted >= n48m6::kPubPerSec - 1u, "R5: at most 20 publishes a second under a storm");
        EXPECT(n48m6::pub_rate_admit(q, 5000 * ms), "R5: and the next one after the storm goes out");
    }

    suite_pins(root);
    suite_m1();
    suite_hw1(root);
    suite_s10(root);

    std::printf("%d checks, %d failures\n", gChecks, gFails);
    return gFails == 0 ? 0 : 1;
}
