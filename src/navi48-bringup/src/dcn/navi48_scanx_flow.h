//
//  navi48_scanx_flow.h - build 0.0.661 (M6 Stage 1b, ABI 1.11, behind the boot-arg latches navi48-m6=1 AND navi48-m6flip=1) + 0.0.662 (M6 Stage 2, ABI 1.12, plus navi48-m6flip1=1): the SEQUENCES of the HDMI displays' scanout (the monitor B, "instance 2": HUBP2 / OTG2, and the monitor A, "instance 1": HUBP1 / OTG1) for the GPU-composited
//  desktop, written against an environment E so tests/native_scanx_test.cpp runs the code the kext runs over a fake HUBP2 register file and a call recorder. C++17, no kernel header. The contract is the last section
//  ("Q2: Stage 1b build contract") of an internal design note, items 4..11; the decisions it reuses are navi48_scanout_pure.h's (slot table, reuse rule, restore verification) UNCHANGED.
//
//  0.0.659's glue escapes (c06 / c08) were sequencing in glue that no test reached: THIS FILE is the only place the sequencing lives (the kext glue in dcn/navi48_dcn.cpp supplies the environment and nothing else).
//
//  0.0.662 (Stage 2, "Stage 2 build contract (monitor A, instance 1)" of an internal design note, section 2): EVERYTHING that was hard-wired to the monitor B is now a field of an XDesc (one constexpr per instance: kDescMonA, kDescMonB; ONE lookup, desc_of(inst)). Every flow takes
//  the descriptor (`const XDesc &d`) next to the environment; one ScanX and one leaf lock exist PER INSTANCE and the glue never holds two of them. The monitor B's descriptor carries exactly 0.0.661's constants (the monitor B's behaviour is unchanged).
//
//  WHAT THIS WRITES: exactly two registers PER INSTANCE - HUBPREQn_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH then ..._ADDRESS (instance 2: 0x3c83 then 0x3c82; instance 1: 0x3ba7 then 0x3ba6, d.addrHigh / d.addrLow) - through ONE guarded writer
//  (write_addr) that judges the WHOLE 64-bit address:
//    present mode  - only a registered slot of THIS instance's table (never A, never B, never an untagged / foreign address, never the other instance's slot);
//    restore mode  - exactly buffer A (this instance's console), nothing else.
//  Never FLIP_CONTROL / VMID_SETTINGS / SURFACE_CONTROL, never a register of another HUBP, never dcn41_hubp_program_flip (it also does read-modify-writes of those three). The instances are POLL-ONLY: no interrupt source is enabled
//  for OTG1 / OTG2 (the single IRQ source stays the DP's VUPDATE); latches are seen by polling FLIP_PENDING (n48scan::latch_observe) every 50 ms, in Status and in Present.
//
//  Environment contract (the kext's glue is dcn/navi48_dcn.cpp; the host test's is tests/native_scanx_test.cpp):
//      uint32_t E::gate()                  0 = go on; else the IOReturn to refuse with. Judged FIRST, before any register read: the latches ON (navi48-m6, navi48-m6flip, and navi48-m6flip1 for instance 1), a native boot, the display layer armed, no mode trial
//      uint32_t E::rd(abs)                 ONE register read (absolute dword; 0xFFFFFFFF = dead); never writes
//      bool     E::wr(abs, v)              THE write; the kext judges the address (only d.addrHigh / d.addrLow: write_reg_ok(d, abs)) and the DCN write allowlist again; false = refused, nothing reached the card
//      uint64_t E::now_ns();  void E::sleep_ms(uint32_t)    sleeps are NEVER called with the lock held
//      void     E::lock(); void E::unlock()   this instance's scanout lock (a leaf: nothing else is locked under it; the glue never holds two instances' locks)
//      uint32_t E::d2_stage(); uint32_t E::d2_cur(); uint64_t E::d2_mc(uint32_t i); void E::d2_set_cur_a()   disp2's plane state of THIS instance (gD2Pl[d.d2idx]; stage HELD = 6; A = mc 0, B = mc 1; cur 0 = A)
//      bool     E::window(uint64_t *lo, uint64_t *hi)     the scanout window [lo, hi)
//      bool     E::start_watchdog(uint32_t gen)           starts the watchdog thread of this acquisition (false = it did not start)
//      void     E::note(const char *fmt, ...)            a log line
//
#ifndef N48_SCANX_FLOW_H
#define N48_SCANX_FLOW_H

#include <stdint.h>
#include "navi48_scanout_pure.h"
#include "navi48_disp2.h"

namespace n48scanx {

// ---- the instance descriptor: EVERYTHING that differs between the displays (0.0.662) ----------------------------------------------------------------------------------------------------------------------
// One constexpr per instance, ONE lookup (desc_of). The register numbers are ABSOLUTE dwords (segment 2 at 0x34c0 + the DCN offset); HUBP1 = HUBP0 + 0xdc, HUBP2 = HUBP0 + 2 * 0xdc; tests/native_scanx_test.cpp derives every one of them BY NAME
// from Linux's dcn_4_1_0_offset.h (the way native_disp2_test.cpp does) and from the HUBP0 twin + stride. DCN_VM_FAULT_STATUS (0x3a8c) is GLOBAL: one fault cannot be attributed to one instance.
struct XDesc {
    uint32_t inst;                // 1 = the monitor A (HUBP1 / OTG1), 2 = the monitor B (HUBP2 / OTG2): the kernel instance number (n48m6::kInstMonA / kInstMonB)
    uint32_t otgInst;             // OTG / HUBP number: DCHUBP_CNTL's VTG_SEL must name it
    uint32_t d2idx;               // index into the glue's gD2Pl[] (disp2's plane state): the monitor A's is [0], the monitor B's [1] (d2i())
    uint32_t slotTag;             // slot ids of this instance are tag | k (0x10 monitor A, 0x20 monitor B; an instance-0 slot id is 0..2: it can never be mistaken for either)
    uint32_t w, h, pitchPx;       // the plane geometry
    uint32_t pitchReg;            // the expected HUBPREQn_DCSURF_SURFACE_PITCH value (pitchPx - 1)
    uint64_t bytes;               // frame bytes (w * 4 * h)
    uint64_t allocBytes;          // disp2's per-buffer allocation (64 KiB multiples): what exclusions use
    uint32_t regSurfConfig, regTiling, regViewStart, regViewDim, regHubpCntl, regPitch, regVmid, regAddr, regAddrHigh, regSurfControl, regFlipControl, regEarlyLo, regEarlyHi;
    uint32_t regVmFault, regOtgFrame, regOtgUpdLock, regOdmGlobal, regOtgControl;
    const char *killName;         // the bundle's kill file for this instance (documentation; the bundle polls it)
    const char *name;
};
// the monitor B: exactly 0.0.661's constants (the update-lock register is N48D2_OTG2_OTG_MASTER_UPDATE_LOCK = 0x5149; the frame count register is the other one; the stray comment 0.0.661 carried on the lock-mask line is NOT copied)
inline constexpr XDesc kDescMonB = {
    2u, 2u, 1u, 0x20u, 2560u, 1440u, 2560u, 0x9FFu, 14745600ull, 14745600ull,
    0x3c5du, 0x3c5fu, 0x3c61u, N48D2_HUBP2_DCSURF_PRI_VIEWPORT_DIMENSION, N48D2_HUBP2_DCHUBP_CNTL, N48D2_HUBPREQ2_DCSURF_SURFACE_PITCH, N48D2_HUBPREQ2_VMID_SETTINGS_0,
    N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS, N48D2_HUBPREQ2_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x3c8au, N48D2_HUBPREQ2_DCSURF_FLIP_CONTROL,
    N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE, N48D2_HUBPREQ2_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
    N48D2_DCN_VM_FAULT_STATUS, N48D2_OTG2_FRAME_COUNT, N48D2_OTG2_OTG_MASTER_UPDATE_LOCK, N48D2_ODM2_OPTC_INPUT_GLOBAL_CONTROL, N48D2_OTG2_CONTROL,
    "/private/tmp/n48m-noflip2", "monitor B" };
// the monitor A: 1920x1080, pitch 7680 B (0x77f), A/B = 127 x 64 KiB = 8,323,072 B each (Run B), HUBP1 / OTG1. 0x3b81 / 0x3b83 / 0x3b85 / 0x3bae have no N48D2_ names yet (their expected values are the census table in navi48_disp2.h).
inline constexpr XDesc kDescMonA = {
    1u, 1u, 0u, 0x10u, 1920u, 1080u, 1920u, 0x77Fu, 8294400ull, 8323072ull,
    0x3b81u, 0x3b83u, 0x3b85u, N48D2_HUBP1_DCSURF_PRI_VIEWPORT_DIMENSION, N48D2_HUBP1_DCHUBP_CNTL, N48D2_HUBPREQ1_DCSURF_SURFACE_PITCH, N48D2_HUBPREQ1_VMID_SETTINGS_0,
    N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS, N48D2_HUBPREQ1_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, 0x3baeu, N48D2_HUBPREQ1_DCSURF_FLIP_CONTROL,
    N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE, N48D2_HUBPREQ1_DCSURF_SURFACE_EARLIEST_INUSE_HIGH,
    N48D2_DCN_VM_FAULT_STATUS, N48D2_OTG1_FRAME_COUNT, N48D2_OTG1_OTG_MASTER_UPDATE_LOCK, N48D2_ODM1_OPTC_INPUT_GLOBAL_CONTROL, N48D2_OTG1_CONTROL,
    "/private/tmp/n48m-noflip1", "monitor A" };
// THE lookup: nullptr for any instance this file does not own (0 = the DP is instance-0 code, anything else is unknown).
constexpr const XDesc *desc_of(uint32_t inst) { return inst == 1u ? &kDescMonA : inst == 2u ? &kDescMonB : nullptr; }
// which instance a TAGGED slot id belongs to (0 = none): the glue's cross-instance defence and the tests
constexpr uint32_t inst_of_id(uint64_t id) {
    return (id >= (uint64_t)kDescMonA.slotTag && id < (uint64_t)(kDescMonA.slotTag + n48scan::kMaxSlots)) ? kDescMonA.inst :
           (id >= (uint64_t)kDescMonB.slotTag && id < (uint64_t)(kDescMonB.slotTag + n48scan::kMaxSlots)) ? kDescMonB.inst : 0u;
}

constexpr uint32_t kPolls       = 100u;                    // the restore polls 1 ms each: up to 100 ms
constexpr uint32_t kWatchMs     = 50u;                     // the watchdog's poll period
constexpr uint32_t kCountWaitMs = 40u;                     // Acquire: the OTG must have counted over this long (2+ frames at 60 Hz)
constexpr uint32_t kMaxExcl     = 16u;                     // ranges the glue hands to Register: the DP console + 3 slots, the OTHER display's pair + 3 slots (8 used; 16 leaves room)

constexpr bool id_ok(const XDesc &d, uint64_t id) { return id >= (uint64_t)d.slotTag && id < (uint64_t)(d.slotTag + n48scan::kMaxSlots); }
constexpr uint32_t slot_of(const XDesc &d, uint64_t id) { return (uint32_t)(id - d.slotTag); }
constexpr uint32_t id_of(const XDesc &d, uint32_t k) { return d.slotTag | k; }

constexpr uint32_t kFlipUpdLockMask = 0x1u;                // DCSURF_FLIP_CONTROL bit 0: the surface update lock (review S3)
constexpr uint32_t kFlipPendingMask = 0x100u;              // DCSURF_FLIP_CONTROL.SURFACE_FLIP_PENDING
constexpr uint32_t kFlipTypeMask    = 0x2u;                // DCSURF_FLIP_CONTROL.SURFACE_FLIP_TYPE
constexpr uint64_t kMc48            = 0x0000FFFFFFFFFFFFull;
// THE write allowlist, by register AND instance: this instance's address HIGH / LOW and nothing else (the glue's wr judges it again; the DCN allowlist is range-coarse, so this is the only per-instance guard).
constexpr bool write_reg_ok(const XDesc &d, uint32_t abs) { return abs == d.regAddrHigh || abs == d.regAddr; }

// ---- the state --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Excl { uint32_t n; uint64_t lo[kMaxExcl], bytes[kMaxExcl]; };   // ranges a new slot must not overlap (the DP console and slots, the other display's pair and slots): a snapshot the glue took BEFORE the lock
struct ScanX {
    bool acquired, restoring, wantRestore, restoreFailed, haveConsole;
    const char *wantWhy;
    uint64_t bufA, bufB;                       // this instance's A (the console) and B
    uint32_t width, height, pitchPx;
    uint32_t gen;
    uint32_t descInst;                         // 0.0.662: the instance whose descriptor Acquire used: a flow called with another descriptor refuses (the second wall against an instance swap)
    n48scan::Table tbl;
    n48scan::FcExt fc;
    uint64_t presentSeq, latchPoll, refused, geomRefused, wdRestores, acquires, restores, restoreFailures;
    uint64_t reuseInuseRefused, writeRefused, writeFailed, untaggedRefused, acquireRefused;
    uint64_t firstLatchNs, lastLatchNs, lastActivityNs, lastRestoreUs;
    uint64_t lastWritten;                      // the last address the writer wrote (diagnostic)
    uint32_t lastAcquireWhy;                   // the first failed Acquire precondition (1.. see Why)
    volatile uint32_t watchdogAlive;
};
// Why Acquire refused (the first failing precondition): logged and reported by the CLI.
enum Why : uint32_t { kWhyNone = 0, kWhyBusy = 1, kWhyStage = 2, kWhyBuffers = 3, kWhyCur = 4, kWhyWindow = 5, kWhyNotCounting = 6, kWhyGeometry = 7, kWhyRestoreExact = 8,
                      kWhyAddress = 9, kWhyPending = 10, kWhyHubp = 11, kWhyVmFault = 12, kWhyWatchdog = 13, kWhyDead = 14, kWhyUpdateLock = 15 };

// ABI 1.12: which instance numbers selectors 22..26 admit. 2 (the monitor B) always (the caller has judged navi48-m6 + navi48-m6flip before); 1 (the monitor A) only with navi48-m6flip1 as well. Anything else (0 = the DP has its own selectors 9..14) is refused.
constexpr bool inst_admitted(uint64_t inst, bool flip1Latched) { return inst == 2ull || (inst == 1ull && flip1Latched); }
// the pins a BO carries for an instance are live only under the generation they were recorded under (a stale pin belongs to an acquisition that has ended). `curGen` MUST be THAT instance's generation (scanXGeneration(inst)).
constexpr uint8_t pins_live(uint8_t mask, uint32_t pinGen, uint32_t curGen) { return pinGen == curGen ? mask : (uint8_t)0; }
// a state is only ever driven through the descriptor it was acquired with (0.0.662, the second wall against an instance swap: the glue's table lookup is the first)
constexpr bool bound(const ScanX &x, const XDesc &d) { return !x.acquired || x.descInst == d.inst; }
inline void reset(ScanX &x) { char *p = reinterpret_cast<char *>(&x); for (unsigned i = 0; i < sizeof x; i++) p[i] = 0; }

// ---- the writer: the ONLY code that writes a register ---------------------------------------------------------------------------------------------------------------------------------------------
enum WMode : uint32_t { kModePresent = 1, kModeRestore = 2 };
// The whole 64-bit address is judged (the disp2 guard judges the two halves separately, which would admit a mixed address). present: a registered slot of THIS table, never A, never B;
// restore: exactly A. Both: 48-bit, the same HIGH dword as A (a flip never changes HIGH), 64 KiB aligned. HIGH first, then LOW. false = nothing (or only HIGH, with the SAME value as before) reached the card.
template <class E>
inline bool write_addr(E &e, const XDesc &d, ScanX &x, uint64_t mc, WMode mode) {
    bool ok = x.descInst == d.inst;                              // 0.0.662: the state was acquired through THIS descriptor (a state / descriptor mix-up writes nothing)
    if (!ok) ok = false;
    else if (mode == kModeRestore) ok = x.bufA != 0ull && mc == x.bufA;
    else if (mode == kModePresent) ok = mc != 0ull && mc != x.bufA && mc != x.bufB && n48scan::flip_target_ok(x.tbl, mc);
    else ok = false;
    if (ok && ((mc >> 48) != 0ull || (mc >> 32) != (x.bufA >> 32) || (mc & 0xFFFFull) != 0ull)) ok = false;
    if (!ok) { x.writeRefused++; e.note("scanx: writer REFUSED %#llx (mode %u): not an address this mode may write", (unsigned long long)mc, (unsigned)mode); return false; }
    if (!e.wr(d.regAddrHigh, (uint32_t)(mc >> 32))) { x.writeFailed++; return false; }
    if (!e.wr(d.regAddr, (uint32_t)mc)) { x.writeFailed++; return false; }
    x.lastWritten = mc;
    return true;
}

// ---- reads ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Geom { uint32_t w, h, pitchPx, fmt, sw; bool dcc; };
template <class E> inline void read_geom(E &e, const XDesc &d, Geom *g) {
    const uint32_t dim = e.rd(d.regViewDim), pitch = e.rd(d.regPitch), cfg = e.rd(d.regSurfConfig), tile = e.rd(d.regTiling), ctl = e.rd(d.regSurfControl);
    g->w = dim & 0xFFFFu; g->h = (dim >> 16) & 0xFFFFu; g->pitchPx = (pitch & 0xFFFFu) + 1u; g->fmt = cfg & 0x7Fu; g->sw = tile & 0x1Fu; g->dcc = (ctl & 0x2u) != 0u;
}
inline bool geom_good(const ScanX &x, const Geom &g) {
    return n48scan::geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) == n48scan::kGeomOk && g.w == x.width && g.h == x.height && g.pitchPx == x.pitchPx;
}
// EARLIEST_INUSE (48 bits) and the programmed address and the pending bit; false when a read was dead.
template <class E> inline bool read_plane(E &e, const XDesc &d, uint64_t *programmed, uint64_t *early, bool *pend) {
    const uint32_t pl = e.rd(d.regAddr), ph = e.rd(d.regAddrHigh), el = e.rd(d.regEarlyLo), eh = e.rd(d.regEarlyHi), fc = e.rd(d.regFlipControl);
    *programmed = (((uint64_t)ph << 32) | pl) & kMc48;
    *early = (((uint64_t)(eh & 0xFFFFu) << 32) | el) & kMc48;
    *pend = (fc & kFlipPendingMask) != 0u;
    return !(pl == 0xFFFFFFFFu && ph == 0xFFFFFFFFu) && el != 0xFFFFFFFFu && fc != 0xFFFFFFFFu;
}
template <class E> inline uint64_t frame_now(E &e, const XDesc &d, ScanX &x) {
    const uint32_t raw = e.rd(d.regOtgFrame);
    return raw == 0xFFFFFFFFu ? x.fc.v : n48scan::fc_extend(x.fc, raw);
}
// One observation: resolves a pending Present into a latch by the RAW pending bit clearing (n48scan::latch_observe). Caller holds the lock.
template <class E> inline bool poll_locked(E &e, const XDesc &d, ScanX &x) {
    const uint64_t fcNow = frame_now(e, d, x);
    if (!x.tbl.pendActive) return false;
    const uint32_t fcr = e.rd(d.regFlipControl);
    if (fcr == 0xFFFFFFFFu) return false;                       // a dead read is "still pending", never a latch
    if (!n48scan::latch_observe(x.tbl, (fcr & kFlipPendingMask) != 0u, fcNow)) return false;
    const uint64_t t = e.now_ns();
    if (x.firstLatchNs == 0ull) x.firstLatchNs = t;
    x.lastLatchNs = t;
    x.latchPoll++;
    return true;
}

// ---- Acquire: reads only; any mismatch refuses with nothing written ---------------------------------------------------------------------------------------------------------------------------
// out[0] = A (the console), out[1] = the extended OTG frame count of THIS instance. The first failing precondition is x.lastAcquireWhy.
template <class E>
inline uint32_t acquire(E &e, const XDesc &d, ScanX &x, uint64_t out[2]) {
    using namespace n48scan;
    const uint32_t g = e.gate();
    if (g != 0u) return g;                                       // latch OFF / not native / not armed: NO register read, no state change
    uint64_t lo = 0, hi = 0;
    uint32_t fc0 = 0;
    e.lock();
    x.lastAcquireWhy = kWhyNone;
    if (x.acquired || x.restoring) { x.lastAcquireWhy = kWhyBusy; x.acquireRefused++; e.unlock(); return kBusy; }
    const uint64_t A = e.d2_mc(0), B = e.d2_mc(1);
    if (e.d2_stage() != N48D2_PL_HELD) { x.lastAcquireWhy = kWhyStage; x.acquireRefused++; e.unlock(); e.note("scanx: Acquire(%s) refused: the plane is not HELD (stage %u)", d.name, (unsigned)e.d2_stage()); return kNotReady; }
    if (A == 0ull || B == 0ull || A == B || (A & 0xFFFFull) != 0ull || (B & 0xFFFFull) != 0ull || (A >> 48) != 0ull || (B >> 48) != 0ull || ranges_overlap(A, d.allocBytes, B, d.allocBytes)) {
        x.lastAcquireWhy = kWhyBuffers; x.acquireRefused++; e.unlock(); e.note("scanx: Acquire(%s) refused: buffers A %#llx / B %#llx are not a valid pair", d.name, (unsigned long long)A, (unsigned long long)B); return kNotReady; }
    if (e.d2_cur() != 0u) { x.lastAcquireWhy = kWhyCur; x.acquireRefused++; e.unlock(); e.note("scanx: Acquire(%s) refused: disp2 bookkeeping says B is the front", d.name); return kNotReady; }
    if (!e.window(&lo, &hi) || hi == 0ull || A < lo || A + d.bytes > hi) { x.lastAcquireWhy = kWhyWindow; x.acquireRefused++; e.unlock(); e.note("scanx: Acquire(%s) refused: A is outside the flip window", d.name); return kNotReady; }
    fc0 = e.rd(d.regOtgFrame);
    e.unlock();
    e.sleep_ms(kCountWaitMs);                                    // the OTG must COUNT: judged over a real interval, with the lock dropped
    e.lock();
    uint32_t rc = kOk;
    do {
        if (x.acquired || x.restoring) { x.lastAcquireWhy = kWhyBusy; rc = kBusy; break; }
        if (e.d2_stage() != N48D2_PL_HELD || e.d2_mc(0) != A || e.d2_mc(1) != B) { x.lastAcquireWhy = kWhyStage; rc = kNotReady; break; }
        const uint32_t fc1 = e.rd(d.regOtgFrame);
        if (fc0 == 0xFFFFFFFFu || fc1 == 0xFFFFFFFFu || ((fc0 ^ fc1) & kFcMask) == 0u) { x.lastAcquireWhy = kWhyNotCounting; rc = kNotReady; break; }
        Geom gm; read_geom(e, d, &gm);
        const uint32_t dim = e.rd(d.regViewDim), pitch = e.rd(d.regPitch), cfg = e.rd(d.regSurfConfig), tile = e.rd(d.regTiling), ctl = e.rd(d.regSurfControl);
        if (dim != (d.w | (d.h << 16)) || pitch != d.pitchReg || cfg != kFmtArgb8888 || (tile & 0x1Fu) != 0u || ctl != 0u ||
            geom_check(gm.w, gm.h, gm.pitchPx, gm.fmt, gm.sw, gm.dcc) != kGeomOk) { x.lastAcquireWhy = kWhyGeometry; rc = kUnsupported; break; }
        const uint32_t vmid = e.rd(d.regVmid), fcr = e.rd(d.regFlipControl), vps = e.rd(d.regViewStart);
        if (!restore_exact(vmid & 0xFu, (ctl & 0x1u) != 0u, (fcr & kFlipTypeMask) >> 1, vps)) { x.lastAcquireWhy = kWhyRestoreExact; rc = kUnsupported; break; }
        uint64_t prog = 0, early = 0; bool pend = true;
        const bool rd = read_plane(e, d, &prog, &early, &pend);
        if (!rd) { x.lastAcquireWhy = kWhyDead; rc = kNotReady; break; }
        if (prog != A || early != A) { x.lastAcquireWhy = kWhyAddress; rc = kBusy; break; }
        if (pend) { x.lastAcquireWhy = kWhyPending; rc = kBusy; break; }
        if ((fcr & kFlipUpdLockMask) != 0u || (e.rd(d.regOtgUpdLock) & 0x101u) != 0u) { x.lastAcquireWhy = kWhyUpdateLock; rc = kBusy; break; }   // S3: the surface update lock and this OTG's master update lock must both be clear (READS only)
        const uint32_t hc = e.rd(d.regHubpCntl);
        if ((hc & N48D2_HUBP_UNDERFLOW_MASK) != 0u || (hc & N48D2_HUBP_BLANK_EN_MASK) != 0u || ((hc >> 4) & 0xFu) != d.otgInst) { x.lastAcquireWhy = kWhyHubp; rc = kNotReady; break; }
        if (e.rd(d.regVmFault) != 0u) { x.lastAcquireWhy = kWhyVmFault; rc = kNotReady; break; }
        // all preconditions hold: take the plane (still no write)
        x.bufA = A; x.bufB = B; x.haveConsole = true;
        x.width = d.w; x.height = d.h; x.pitchPx = d.pitchPx;
        table_init(x.tbl, A, d.bytes);
        x.fc = FcExt{ 0, false };
        (void)n48scan::fc_extend(x.fc, fc1);
        x.presentSeq = 0; x.latchPoll = 0; x.refused = 0; x.geomRefused = 0; x.wdRestores = 0;
        x.reuseInuseRefused = 0; x.untaggedRefused = 0; x.firstLatchNs = 0; x.lastLatchNs = 0;
        x.wantRestore = false; x.wantWhy = nullptr; x.restoreFailed = false;
        x.lastActivityNs = e.now_ns();
        x.gen++; x.acquires++;
        x.descInst = d.inst;
        x.acquired = true;
        if (!e.start_watchdog(x.gen)) { x.acquired = false; x.lastAcquireWhy = kWhyWatchdog; rc = kNotReady; break; }
        out[0] = A; out[1] = x.fc.v;
        e.note("scanx: ACQUIRED(%s) gen %u: instance %u plane %ux%u pitch %u px, console (A) %#llx, B %#llx, frame %llu (OTG%u counted %u -> %u)", d.name, x.gen, (unsigned)d.inst, (unsigned)d.w, (unsigned)d.h, (unsigned)d.pitchPx,
               (unsigned long long)A, (unsigned long long)B, (unsigned long long)x.fc.v, (unsigned)d.otgInst, fc0 & kFcMask, fc1 & kFcMask);
    } while (false);
    if (rc != kOk) { x.acquireRefused++; e.note("scanx: Acquire(%s) refused (reason %u, status %#x): nothing was written", d.name, (unsigned)x.lastAcquireWhy, (unsigned)rc); }
    e.unlock();
    return rc;
}

// ---- Register ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct RegIn { bool vis; uint64_t boMc, boSize, offset; uint32_t pitchBytes, width, height, format; };
// out[0] = the tagged slot id (d.slotTag | k), out[1] = the slot MC.
template <class E>
inline uint32_t register_slot(E &e, const XDesc &d, ScanX &x, const RegIn &in, const Excl &ex, uint64_t out[2]) {
    using namespace n48scan;
    const uint32_t g = e.gate();
    if (g != 0u) return g;
    e.lock();
    uint32_t rc = kOk;
    do {
        if (!x.acquired || x.restoring || x.wantRestore) { rc = kNotReady; break; }
        if (!bound(x, d)) { rc = kBadArg; break; }
        x.lastActivityNs = e.now_ns();
        Geom gm; read_geom(e, d, &gm);
        if (!geom_good(x, gm)) { rc = kNotReady; break; }
        if (in.width != gm.w || in.format != kFmtArgb8888) { rc = kBadArg; break; }
        uint64_t lo = 0, hi = 0;
        if (!e.window(&lo, &hi)) { rc = kNotReady; break; }
        const Fb fb { lo, hi, x.bufA, d.bytes };                  // the slot shares A's HIGH dword and cannot overlap A (reg_bounds)
        uint64_t mc = 0, bytes = 0;
        rc = reg_bounds(BoRef{ in.vis, in.boMc, in.boSize }, in.offset, in.pitchBytes, in.height, gm.pitchPx * 4u, gm.h, fb, &mc, &bytes);
        if (rc != kOk) break;
        if (ranges_overlap(mc, bytes, x.bufB, d.allocBytes) || ranges_overlap(mc, bytes, x.bufA, d.allocBytes)) { rc = kBadArg; break; }   // never B, and never A's whole allocation (not just the frame): 0.0.662 judges the ALLOCATION size
        bool clash = false;
        for (uint32_t i = 0; i < ex.n && i < kMaxExcl; i++) if (ex.bytes[i] != 0ull && ranges_overlap(mc, bytes, ex.lo[i], ex.bytes[i])) clash = true;   // the DP console / slots, the OTHER display's pair and slots
        if (clash) { rc = kBadArg; break; }
        uint32_t slot = kNoSlot;
        rc = slot_register(x.tbl, mc, bytes, &slot);
        if (rc != kOk) break;
        out[0] = id_of(d, slot); out[1] = mc;
        e.note("scanx: REGISTER slot %#x = MC %#llx (%llu bytes, BO offset %#llx)", (unsigned)id_of(d, slot), (unsigned long long)mc, (unsigned long long)bytes, (unsigned long long)in.offset);
    } while (false);
    e.unlock();
    return rc;
}

// ---- Present: the only new write of the instance apart from the restore ---------------------------------------------------------------------------------------------------------------------------
// out[0] = present id, [1] = target frame, [2] = 0 (no VUPDATE source). The slot id must carry THIS descriptor's tag (id_ok): the other instance's slot, an instance-0 slot id and any untagged value are refused before the lock.
template <class E>
inline uint32_t present(E &e, const XDesc &d, ScanX &x, uint64_t slotId, uint64_t flags, uint64_t out[3]) {
    using namespace n48scan;
    const uint32_t g = e.gate();
    if (g != 0u) return g;
    if (flags != 0ull) return kBadArg;
    if (!id_ok(d, slotId)) { e.lock(); x.untaggedRefused++; x.refused++; e.unlock(); return kBadArg; }     // an instance-0 slot id (0..2) or any untagged value is never presented here
    const uint32_t k = slot_of(d, slotId);
    e.lock();
    uint32_t rc = kOk;
    do {
        if (!x.acquired) { rc = kNotReady; break; }
        if (!bound(x, d)) { x.refused++; rc = kBadArg; break; }
        if (x.restoring || x.wantRestore) { x.refused++; rc = kNotReady; break; }
        x.lastActivityNs = e.now_ns();
        Geom gm; read_geom(e, d, &gm);
        if (!geom_good(x, gm)) {
            x.geomRefused++; x.refused++; rc = kNotReady; break; }
        (void)poll_locked(e, d, x);                                  // resolve the previous Present first: a latch not seen yet is not "replaced"
        if (!x.tbl.s[k].used) { rc = kNotFound; break; }
        uint64_t prog = 0, early = 0; bool pend = true;
        const bool rd = read_plane(e, d, &prog, &early, &pend);
        if (!rd) { x.refused++; rc = kNotReady; break; }
        // the new refusal: a slot that is pending, or that the hardware is still fetching (this instance's EARLIEST_INUSE == slot.mc), is never programmed again
        if ((x.tbl.pendActive && x.tbl.pendSlot == k) || early == x.tbl.s[k].mc) { x.reuseInuseRefused++; x.refused++; rc = kBusy; break; }
        const Table saved = x.tbl;
        const uint64_t fcNow = frame_now(e, d, x);
        const uint64_t id = x.presentSeq + 1ull;
        const uint64_t target = fcNow + 1ull;
        const uint64_t mc = present_begin(x.tbl, k, id, target);
        if (mc == 0ull || !flip_target_ok(x.tbl, mc) || mc == x.bufA || mc == x.bufB) { x.tbl = saved; x.refused++; rc = kBadArg; break; }
        if (!write_addr(e, d, x, mc, kModePresent)) {
            x.tbl = saved; x.refused++;
            x.wantRestore = true; x.wantWhy = "a present write failed (fail toward A)";    // the plane may hold a half-written address: the watchdog restores A
            rc = kNotReady; break;
        }
        x.presentSeq = id;
        out[0] = id; out[1] = target; out[2] = 0ull;
    } while (false);
    e.unlock();
    return rc;
}

// ---- Status ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct SlotOut { uint64_t mc, latchedFrame; uint32_t used, flags, presents, latches; };
struct StatusOut {
    uint32_t acquired, flags, frontSlot, pendingSlot;            // slot ids are TAGGED (d.slotTag | k) or n48scan::kNoSlot
    uint64_t frameCount, consoleMc, planeMc, earliestMc;
    uint64_t presents, latched, replaced, repeats, latchPoll, refused, firstLatchNs, lastLatchNs, idleMs, wdRestores, geomRefused;
    uint64_t reuseInuseRefused, writeRefused, writeFailed, untaggedRefused, restores, restoreFailures, acquires, bufB;
    uint32_t flipPending, gen, restoreFailed;
    SlotOut slot[n48scan::kMaxSlots];
};
constexpr uint32_t kStRestoring = 1u, kStWantRestore = 4u;     // = N48N_SCANST_RESTORING / _WANT_RESTORE
template <class E>
inline uint32_t status(E &e, const XDesc &d, ScanX &x, StatusOut *o) {
    using namespace n48scan;
    const uint32_t g = e.gate();
    if (g != 0u) return g;
    char *p = reinterpret_cast<char *>(o); for (unsigned i = 0; i < sizeof *o; i++) p[i] = 0;
    e.lock();
    if (x.acquired && !x.restoring) { x.lastActivityNs = e.now_ns(); (void)poll_locked(e, d, x); }       // a status call IS the keep-alive: it counts as activity
    uint64_t prog = 0, early = 0; bool pend = false;
    const bool rd = read_plane(e, d, &prog, &early, &pend);
    const bool earlyOk = rd;                                     // judged on THIS instance's own reads, never another HUBP's
    const uint32_t raw = e.rd(d.regOtgFrame);
    o->acquired = x.acquired ? 1u : 0u;
    o->flags = (x.restoring ? kStRestoring : 0u) | (x.wantRestore ? kStWantRestore : 0u);
    o->frontSlot = x.tbl.front == kNoSlot ? kNoSlot : id_of(d, x.tbl.front);
    o->pendingSlot = x.tbl.pendActive ? id_of(d, x.tbl.pendSlot) : kNoSlot;
    o->frameCount = x.acquired ? (raw == 0xFFFFFFFFu ? x.fc.v : fc_extend(x.fc, raw)) : (uint64_t)(raw & kFcMask);
    o->consoleMc = x.haveConsole ? x.bufA : prog; o->planeMc = prog; o->earliestMc = early;
    o->presents = x.tbl.presents; o->latched = x.tbl.latched; o->replaced = x.tbl.replaced; o->repeats = x.tbl.repeats;
    o->latchPoll = x.latchPoll; o->refused = x.refused; o->firstLatchNs = x.firstLatchNs; o->lastLatchNs = x.lastLatchNs;
    const uint64_t t = e.now_ns();
    o->idleMs = (x.acquired && t >= x.lastActivityNs) ? (t - x.lastActivityNs) / 1000000ull : 0ull;
    o->wdRestores = x.wdRestores; o->geomRefused = x.geomRefused;
    o->reuseInuseRefused = x.reuseInuseRefused; o->writeRefused = x.writeRefused; o->writeFailed = x.writeFailed; o->untaggedRefused = x.untaggedRefused;
    o->restores = x.restores; o->restoreFailures = x.restoreFailures; o->acquires = x.acquires; o->bufB = x.bufB;
    o->flipPending = pend ? 1u : 0u; o->gen = x.gen; o->restoreFailed = x.restoreFailed ? 1u : 0u;
    for (uint32_t i = 0; i < kMaxSlots; i++) {
        const Slot &s = x.tbl.s[i];
        SlotOut &so = o->slot[i];
        so.mc = s.mc; so.latchedFrame = s.latchedFrame; so.used = s.used ? 1u : 0u; so.presents = s.presents; so.latches = s.latches;
        if (!s.used) continue;
        so.flags = ((x.tbl.pendActive && x.tbl.pendSlot == i) ? 1u : 0u) | ((earlyOk && early == s.mc) ? 2u : 0u) | (slot_reusable(x.tbl, i, early, earlyOk) ? 4u : 0u);   // = N48N_SCANSLOT_*
    }
    e.unlock();
    return kOk;
}

// ---- Restore (to A): ONE function for every path ------------------------------------------------------------------------------------------------------------------------------------------------
// Callers: Release, the N48N close / clientDied, BoFree of a slot that is shown or pending, the HUNG leak path, kext stop and the watchdog (5 s idle, a geometry change, wantRestore).
// Writes HIGH then LOW of A through the writer in restore mode (it admits exactly A), then polls up to 100 ms, dropping the lock around EVERY sleep, until FLIP_PENDING is clear and EARLIEST = programmed = A.
// Not verified: restoreFailed is set (the caller LEAKS, never frees, every BO with a pin of THIS instance). Idempotent: nothing acquired = no write. out (may be null): [0] verified 1/0, [1] the programmed address after.
// It finishes by setting disp2's bookkeeping cur = A (only when verified) so the HELD invariant holds again.
template <class E>
inline uint32_t restore(E &e, const XDesc &d, ScanX &x, const char *why, uint32_t expectGen, uint64_t *out) {
    using namespace n48scan;
    e.lock();
    if (x.haveConsole && x.descInst != d.inst) {                 // 0.0.662: this state belongs to ANOTHER descriptor: no register of this one is read or written, and the verdict is NOT verified (the caller leaks)
        e.unlock(); e.note("scanx: RESTORE(%s) refused: the state was acquired through instance %u", d.name, (unsigned)x.descInst);
        if (out) { out[0] = 0; out[1] = 0; }
        return 0u;
    }
    if (expectGen != 0u && x.gen != expectGen) { e.unlock(); if (out) { out[0] = 1; out[1] = 0; } return 0u; }
    if (x.restoring) {                                           // another thread is restoring: wait for it (the lock is dropped around every wait), bounded at 2 s
        for (uint32_t i = 0; i < 2000u && x.restoring; i++) { e.unlock(); e.sleep_ms(1u); e.lock(); }
        uint64_t prog = 0, early = 0; bool pend = true;
        const bool rd = read_plane(e, d, &prog, &early, &pend);
        const bool ok = !x.restoring && rd && restore_verified(prog, early, pend, x.bufA);
        e.unlock();
        if (out) { out[0] = ok ? 1u : 0u; out[1] = prog; }
        return 0u;
    }
    if (!x.acquired) {                                           // nothing to do; the verdict is still honest (an earlier failed restore reads NOT verified until the plane is back on A)
        uint64_t prog = 0, early = 0; bool pend = true;
        bool ok = true;
        if (x.haveConsole) {
            const bool rd = read_plane(e, d, &prog, &early, &pend);
            ok = rd && restore_verified(prog, early, pend, x.bufA);
            if (rd) x.restoreFailed = !ok;
        }
        e.unlock();
        if (out) { out[0] = ok ? 1u : 0u; out[1] = prog; }
        return 0u;
    }
    x.restoring = true;
    const uint64_t A = x.bufA;
    const uint64_t t0 = e.now_ns();
    const uint64_t pres = x.tbl.presents, lat = x.tbl.latched, rep = x.tbl.replaced;
    bool wrote = write_addr(e, d, x, A, kModeRestore);
    if (!wrote) e.note("scanx: RESTORE(%s): programming A %#llx failed", d.name, (unsigned long long)A);
    uint64_t prog = 0, early = 0; bool pend = true, rd = false, verified = false;
    // Review S2: like instance 0 (n48scan::kRestoreAttempts), A is programmed a SECOND time when the first 100 polls did not verify it, and polled again, before the restore is declared unverified.
    for (uint32_t attempt = 1; attempt <= kRestoreAttempts && !verified; attempt++) {
        if (attempt > 1u) { wrote = write_addr(e, d, x, A, kModeRestore) || wrote; e.note("scanx: RESTORE(%s): A not verified after %u polls: programmed again (attempt %u)", d.name, (unsigned)kPolls, (unsigned)attempt); }
        for (uint32_t i = 0; i < kPolls; i++) {
            e.unlock();
            e.sleep_ms(1u);                                          // a latch happens within a frame; polled, never waited for by interrupt
            e.lock();
            rd = read_plane(e, d, &prog, &early, &pend);
            if (wrote && rd && restore_verified(prog, early, pend, A)) { verified = true; break; }
        }
    }
    rd = read_plane(e, d, &prog, &early, &pend);
    verified = wrote && rd && restore_verified(prog, early, pend, A);
    table_init(x.tbl, A, d.bytes);
    x.acquired = false;
    x.wantRestore = false; x.wantWhy = nullptr;
    x.restores++;
    if (!verified) x.restoreFailures++;
    x.restoreFailed = !verified;
    if (verified) e.d2_set_cur_a();                              // the HELD invariant: disp2's bookkeeping says A is the front again
    x.restoring = false;
    x.lastRestoreUs = (e.now_ns() - t0) / 1000ull;
    e.note("scanx: RESTORE(%s) (%s) -> A %#llx: plane reads %#llx, EARLIEST_INUSE %#llx, pending %d -> %s in %llu us; this session: presents %llu latched %llu replaced %llu", d.name, why ? why : "?",
           (unsigned long long)A, (unsigned long long)prog, (unsigned long long)early, pend ? 1 : 0, verified ? "VERIFIED" : "*** NOT VERIFIED - the BOs with this display's slots are LEAKED ***",
           (unsigned long long)x.lastRestoreUs, (unsigned long long)pres, (unsigned long long)lat, (unsigned long long)rep);
    e.unlock();
    if (out) { out[0] = verified ? 1u : 0u; out[1] = prog; }
    return 0u;
}

// ---- the watchdog: one thread per acquisition (the kext starts it; the test drives watchdog_step) -------------------------------------------------------------------------------------------
// One 50 ms round: poll (counts latches without any caller), judge the geometry, and restore on a requested restore, a geometry change or 5 s without a scanout call. true = the thread is done.
template <class E>
inline bool watchdog_step(E &e, const XDesc &d, ScanX &x, uint32_t myGen) {
    e.lock();
    if (!x.acquired || x.gen != myGen) { e.unlock(); return true; }
    if (x.descInst != d.inst) { e.unlock(); return true; }       // 0.0.662: a state of another descriptor is never polled or restored through this one
    if (x.restoring) { e.unlock(); return false; }
    (void)poll_locked(e, d, x);
    Geom gm; read_geom(e, d, &gm);
    // S3: what made the restore EXACT at Acquire (VMID 0, FLIP_TYPE 0, TMZ 0, viewport start 0) is judged again every round: any change -> restore A (READS only)
    const uint32_t wv = e.rd(d.regVmid), wf = e.rd(d.regFlipControl), wc = e.rd(d.regSurfControl), ws = e.rd(d.regViewStart);
    const bool exactBad = wv == 0xFFFFFFFFu || wf == 0xFFFFFFFFu || !n48scan::restore_exact(wv & 0xFu, (wc & 0x1u) != 0u, (wf & kFlipTypeMask) >> 1, ws);
    const char *why = nullptr;
    if (x.wantRestore) why = x.wantWhy ? x.wantWhy : "requested";
    else if (!geom_good(x, gm)) why = "live plane geometry changed";
    else if (exactBad) why = "live plane VMID / flip type / TMZ / viewport start changed";
    else if (n48scan::idle_expired(e.now_ns(), x.lastActivityNs)) why = "idle watchdog: 5 s without a scanout call";
    if (why) x.wdRestores++;
    e.unlock();
    if (why) { e.note("scanx: WATCHDOG(%s) restoring A: %s", d.name, why); (void)restore(e, d, x, why, myGen, nullptr); return true; }
    return false;
}
template <class E>
inline void watchdog_run(E &e, const XDesc &d, ScanX &x, uint32_t myGen) {
    for (;;) {
        e.sleep_ms(kWatchMs);
        if (watchdog_step(e, d, x, myGen)) break;
    }
}

// ---- a BO hosting this instance's slots goes away (BoFree, the HUNG leak, the close of the session) ----------------------------------------------------------------------------------------------
// Same plan as n48dcn::scanBoGone, judged with THIS instance's EARLIEST: 0 = nothing to do or the slots were just dropped; 1 = A was restored and VERIFIED; 2 = not verified (now or earlier this acquisition): the caller LEAKS the BO.
// pinMask bit k = slot k (id 0x20 | k); pinGen = the generation the pins were recorded under; a stale pin is a no-op. out (may be null) receives [verified, plane] when a restore ran.
template <class E>
inline uint32_t bo_gone(E &e, const XDesc &d, ScanX &x, uint32_t pinMask, uint32_t pinGen, bool alwaysFull, uint64_t *out) {
    using namespace n48scan;
    e.lock();
    if (x.haveConsole && x.descInst != d.inst) { e.unlock(); return 2u; }      // 0.0.662: a state of another descriptor: nothing of this instance is judged, the BO is LEAKED (fail safe)
    bool full = false;
    uint32_t rc = 0u;
    if (!x.acquired && x.gen == pinGen && x.restoreFailed) rc = 2u;
    if (x.acquired && x.gen == pinGen) {
        uint64_t prog = 0, early = 0; bool pend = true;
        const bool ok = !x.restoring && read_plane(e, d, &prog, &early, &pend);
        const UnpinPlan plan = unpin_plan(x.tbl, (uint8_t)pinMask, alwaysFull || x.restoring, early, ok);
        full = plan.fullRelease;
        if (!full) {
            for (uint32_t i = 0; i < kMaxSlots; i++) {
                if (((plan.dropMask >> i) & 1u) == 0u) continue;
                (void)slot_unregister(x.tbl, i, early, ok);
                e.note("scanx: slot %#x (%s) unregistered (its BO was freed while neither shown nor pending)", (unsigned)id_of(d, i), d.name);
            }
        }
    }
    e.unlock();
    if (!full) return rc;
    uint64_t r[2] = { 1, 0 };
    (void)restore(e, d, x, "a BO with one of this display's slots is being released", pinGen, r);
    if (out) { out[0] = r[0]; out[1] = r[1]; }
    return r[0] != 0ull ? 1u : 2u;
}

// ---- the close / release / HUNG teardown: ALL THREE instances are restored before any BO is freed, unpinned or leaked ---------------------------------------------------------------------------
// T provides: void restore0(const char *how, uint64_t r[2])  (instance 0: the DP console), void restore1(...) (instance 1, the monitor A's Restore), void restore2(...) (instance 2, the monitor B's Restore),
//             uint32_t nbo(); bool used(h); uint32_t pin0(h); uint32_t pin1(h); uint32_t pin2(h); void mark_leak(h); void clear_pins(h).
// A BO with a pin of an instance whose restore did NOT verify is marked for LEAK (only that instance's pins decide); every pin is cleared at the end.
template <class T>
inline void teardown_all(T &t, const char *how, uint64_t r0[2], uint64_t r1[2], uint64_t r2[2]) {
    r0[0] = 1; r0[1] = 0; r1[0] = 1; r1[1] = 0; r2[0] = 1; r2[1] = 0;
    t.restore0(how, r0);
    t.restore1(how, r1);
    t.restore2(how, r2);                                         // before ANY BO is touched
    for (uint32_t h = 1; h < t.nbo(); h++) {
        if (!t.used(h)) continue;
        if (r0[0] == 0ull && t.pin0(h) != 0u) t.mark_leak(h);
        if (r1[0] == 0ull && t.pin1(h) != 0u) t.mark_leak(h);
        if (r2[0] == 0ull && t.pin2(h) != 0u) t.mark_leak(h);
        t.clear_pins(h);
    }
}

// ---- disp2 interplay (item 11) ------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// While an HDMI instance (1 or 2) is ACQUIRED its plane is GPU-held: EARLIEST_INUSE legitimately differs from A. disp2's `status` and `crc` ops and fbLive report "GPU-held, slot k" instead of a latch failure.
// gpu_held_code: 0 = not acquired; 1..3 = slot k+1 is what the hardware fetches; 4 = acquired, EARLIEST is not a slot (A, B or unknown). The latch bit of the hold verdict is dropped while GPU-held.
constexpr uint32_t gpu_held_code(bool acquired, const n48scan::Table &t, uint64_t early) {
    if (!acquired) return 0u;
    for (uint32_t i = 0; i < n48scan::kMaxSlots; i++) if (t.s[i].used && t.s[i].mc == early) return i + 1u;
    return 4u;
}
// Review S1: the latch bit is dropped ONLY for codes 1..3 (EARLIEST_INUSE is one of OUR slots: the GPU legitimately holds the plane); code 4 (EARLIEST is A, B or unknown) keeps it - that is a real disagreement. And while the instance is
// acquired at all (any code), the verdict carries kHoldGpuHeld: fbpublish (judge: n48fb::live_verdict != 0) is REFUSED, a published framebuffer must never sit on a GPU-driven plane.
constexpr uint32_t kHoldGpuHeld = 0x100u;
constexpr uint32_t hold_verdict_gpu(uint32_t bad, uint32_t heldCode) {
    return heldCode == 0u ? bad : (((heldCode >= 1u && heldCode <= 3u) ? (bad & ~(uint32_t)N48D2_H_LATCH) : bad) | kHoldGpuHeld);
}

}  // namespace n48scanx

#endif
