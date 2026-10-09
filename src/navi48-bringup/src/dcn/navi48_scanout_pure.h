//
//  navi48_scanout_pure.h - the PURE half of native-stack step S2a (kext 0.0.603): the decisions behind the native scanout selectors
//  of the N48N client (an internal design note and its Review; the ABI addendum is the last section of NATIVE-S1C-ABI.md).
//  No kernel header is included, so tests/native_s2a_test.cpp compiles this on the host Mac and drives the exact functions the kext calls
//  (dcn/navi48_dcn.cpp for the plane, amd/native_s1c.cpp for the BO side, Navi48Bringup.cpp for the exemption table).
//
//  Contents: return codes, the buffer bounds check, the slot table with its reuse rule and the pending / latched / replaced / repeat
//  accounting, the 24-bit OTG frame counter extended to 64 bits, the restore verification, the exact flip-target set, the rate-based
//  IRQ storm guard, the idle deadline, the refresh arithmetic, the geometry check and the native-boot accel exemption table.
//
#pragma once
#include <stdint.h>

namespace n48scan {

// ---- return codes: the kIOReturn values (the kext static_asserts them against IOReturn.h) ------------------------------------
constexpr uint32_t kOk = 0u;
constexpr uint32_t kBadArg = 0xe00002c2u, kNotFound = 0xe00002f0u, kUnsupported = 0xe00002c7u, kNoResources = 0xe00002beu;
constexpr uint32_t kNotReady = 0xe00002d8u, kBusy = 0xe00002d5u, kNotPermitted = 0xe00002e2u, kNoDevice = 0xe00002c0u;

// ---- limits ------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxSlots      = 3u;                 // three buffers: front, pending, and one being drawn
constexpr uint32_t kNoSlot        = 0xFFFFFFFFu;
constexpr uint64_t kBufAlign      = 65536ull;           // 64 KiB: the alignment flip mode uses (design 1.2; the true DCN minimum is not established)
constexpr uint64_t kIdleNs        = 5000000000ull;      // 5 s without any scanout call while the plane is taken: restore the console
constexpr uint32_t kStormLimit    = 1000u;              // classified DCE IRQs per window (120 Hz is 120)
constexpr uint64_t kStormWindowNs = 1000000000ull;
constexpr uint32_t kFcMask        = 0x00FFFFFFu;        // OTG_STATUS_FRAME_COUNT.OTG_FRAME_COUNT is 24 bits
constexpr uint32_t kFmtArgb8888   = 8u;                 // HUBP SURFACE_PIXEL_FORMAT 8 = ARGB8888: dword 0xAARRGGBB, bytes B,G,R,A = B8G8R8A8_UNORM
constexpr uint32_t kMaxDim        = 8192u;
constexpr uint32_t kMaxPitchPx    = 16384u;

// ---- the native-boot accel exemption (decision D-S2-2) ----------------------------------------------------------------------
// On a native boot every accel verb but action 0 is refused (native_s1b_refuse). EXACTLY these (action, arg) pairs are exempt, and they
// are placed BEFORE that refusal: dcnstate 0 (read-only), dcnflip 0 (the console restore), dcnmode 0 (read-only, and the capture restore).
// Matching the action alone would also admit dcnflip 1..30 (a test pattern) and dcnflip 1002.. (flip mode's A/B test), dcnmode 1..130.
constexpr bool accel_exempt(uint32_t action, uint64_t arg) {
    return arg == 0ull && (action == 74u || action == 76u || action == 77u);
}

// ---- refresh arithmetic ----------------------------------------------------------------------------------------------------
// millihertz = pixel clock (Hz) * 1000 / (h_total * v_total); 0 when anything is zero. pix_hz up to ~2e9 stays well inside 64 bits.
constexpr uint64_t refresh_mhz(uint64_t pixHz, uint32_t hTotal, uint32_t vTotal) {
    return (pixHz == 0ull || hTotal == 0u || vTotal == 0u) ? 0ull : (pixHz * 1000ull) / ((uint64_t)hTotal * (uint64_t)vTotal);
}

// ---- the plane geometry the kernel is willing to scan a client buffer through --------------------------------------------------
enum GeomWhy : uint32_t { kGeomOk = 0, kGeomDims = 1, kGeomFormat = 2, kGeomTiling = 3, kGeomDcc = 4, kGeomPitch = 5 };
// A linear (SW_MODE 0), uncompressed (PRIMARY_SURFACE_DCC_EN clear) ARGB8888 plane with a plausible viewport and pitch.
constexpr uint32_t geom_check(uint32_t w, uint32_t h, uint32_t pitchPx, uint32_t fmt, uint32_t swMode, bool dccEn) {
    if (w == 0u || h == 0u || w > kMaxDim || h > kMaxDim) return kGeomDims;
    if (pitchPx < w || pitchPx > kMaxPitchPx) return kGeomPitch;
    if (fmt != kFmtArgb8888) return kGeomFormat;
    if (swMode != 0u) return kGeomTiling;
    if (dccEn) return kGeomDcc;
    return kGeomOk;
}

// ---- what makes the restore EXACT (Acquire refuses otherwise) ------------------------------------------------------------------
// The flip layer writes VMID_SETTINGS_0.VMID = 0, PRIMARY_SURFACE_TMZ = 0 and SURFACE_FLIP_TYPE = 0 on every flip, including the restore.
// If the console plane was scanned with anything else, "restore" would not put it back as found. And a nonzero PRI_VIEWPORT_START would make the
// fetch start inside the buffer (bounded fetch is only proven from 0). So Acquire requires all four to be 0.
constexpr bool restore_exact(uint32_t vmid, bool tmz, uint32_t flipType, uint32_t viewportStart) {
    return vmid == 0u && !tmz && flipType == 0u && viewportStart == 0u;
}

// ---- the buffer bounds (ScanoutRegister) ------------------------------------------------------------------------------------
// fb = the scanout window the flip layer was armed with ([lo, hi) in MC space; hi 0 = none, which FAILS CLOSED here) and the console.
struct Fb { uint64_t lo, hi; uint64_t consoleMc, consoleBytes; };
struct BoRef { bool vis; uint64_t mc; uint64_t size; };   // vis: the BO lives in the BAR0-visible pool (kind == kBoVis); mc: its MC base
// Every check runs before any state changes. On kOk *mcOut is the buffer's MC address and *bytesOut its length.
//   * visible pool only (a hi-pool or GTT BO is refused: it is not what the console shares a HIGH dword with);
//   * the geometry the client states equals the live plane (pitch in bytes, height);
//   * offset + pitch*height inside the BO, overflow-safe (va - start + offset + pitch*h*4 <= size in the review's words);
//   * 64 KiB aligned start, inside the frame-buffer window, one HIGH dword for start and last byte, equal to the console's;
//   * no overlap with the console buffer.
inline uint32_t reg_bounds(const BoRef &bo, uint64_t offset, uint32_t pitchBytes, uint32_t height, uint32_t livePitchBytes, uint32_t liveHeight,
                           const Fb &fb, uint64_t *mcOut, uint64_t *bytesOut) {
    if (!bo.vis) return kBadArg;
    if (height == 0u || pitchBytes == 0u || (pitchBytes & 3u) != 0u) return kBadArg;
    if (pitchBytes != livePitchBytes || height != liveHeight) return kBadArg;
    const uint64_t bytes = (uint64_t)pitchBytes * (uint64_t)height;            // <= 2^16 * 2^14 * 4: no overflow
    if (offset > bo.size || bytes > bo.size - offset) return kBadArg;
    if (bo.mc > ~0ull - offset) return kBadArg;
    const uint64_t start = bo.mc + offset;
    if ((start & (kBufAlign - 1ull)) != 0ull) return kBadArg;
    if (start > ~0ull - bytes) return kBadArg;
    const uint64_t end = start + bytes;                                         // exclusive
    if (fb.hi == 0ull || start < fb.lo || end > fb.hi) return kBadArg;
    if ((start >> 32) != (fb.consoleMc >> 32) || ((end - 1ull) >> 32) != (fb.consoleMc >> 32)) return kBadArg;   // HIGH must never change under a flip
    if (fb.consoleBytes != 0ull && start < fb.consoleMc + fb.consoleBytes && fb.consoleMc < end) return kBadArg;
    *mcOut = start; *bytesOut = bytes;
    return kOk;
}

// ---- the frame counter, 24 bits extended to 64 -----------------------------------------------------------------------------
struct FcExt { uint64_t v; bool init; };
// Feed every raw read, in order (the kext calls it under the scanout lock). The first read seeds the value; later reads add the
// forward distance modulo 2^24. A raw value that is more than half a wrap BEHIND the last one (a stale read, or a counter reset) does not
// move the value: a backward step must never be turned into a 2^24-frame jump.
inline uint64_t fc_extend(FcExt &e, uint32_t raw) {
    raw &= kFcMask;
    if (!e.init) { e.v = raw; e.init = true; return e.v; }
    const uint32_t d = (raw - (uint32_t)(e.v & kFcMask)) & kFcMask;
    if (d >= (kFcMask + 1u) / 2u) return e.v;
    e.v += d;
    return e.v;
}

// ---- the slot table --------------------------------------------------------------------------------------------------------
struct Slot { bool used; uint64_t mc; uint64_t bytes; uint64_t latchedFrame; uint32_t presents; uint32_t latches; };
struct Table {
    Slot s[kMaxSlots];
    uint64_t consoleMc, consoleBytes;
    bool pendActive;                 // a Present has been programmed and not yet seen to latch
    uint32_t pendSlot;
    uint64_t pendMc, pendId, pendTarget;
    uint32_t front;                  // the slot last seen latched, else kNoSlot
    uint64_t presents, latched, replaced, repeats;
    uint64_t firstLatchFrame, lastLatchFrame;
};
inline void table_init(Table &t, uint64_t consoleMc, uint64_t consoleBytes) {
    for (uint32_t i = 0; i < kMaxSlots; i++) t.s[i] = Slot{ false, 0, 0, 0, 0, 0 };
    t.consoleMc = consoleMc; t.consoleBytes = consoleBytes;
    t.pendActive = false; t.pendSlot = kNoSlot; t.pendMc = 0; t.pendId = 0; t.pendTarget = 0;
    t.front = kNoSlot;
    t.presents = t.latched = t.replaced = t.repeats = 0;
    t.firstLatchFrame = t.lastLatchFrame = 0;
}
inline bool ranges_overlap(uint64_t aLo, uint64_t aBytes, uint64_t bLo, uint64_t bBytes) { return aLo < bLo + bBytes && bLo < aLo + aBytes; }
// A new slot: lowest free index. NoResources when full, BadArg when the range aliases another slot or the console (an alias would let
// "slot reuse is safe" be true of one name and false of the memory).
inline uint32_t slot_register(Table &t, uint64_t mc, uint64_t bytes, uint32_t *slotOut) {
    if (bytes == 0ull || mc == 0ull) return kBadArg;
    if (ranges_overlap(mc, bytes, t.consoleMc, t.consoleBytes ? t.consoleBytes : 1ull)) return kBadArg;
    uint32_t freeIdx = kNoSlot;
    for (uint32_t i = 0; i < kMaxSlots; i++) {
        if (!t.s[i].used) { if (freeIdx == kNoSlot) freeIdx = i; continue; }
        if (ranges_overlap(mc, bytes, t.s[i].mc, t.s[i].bytes)) return kBadArg;
    }
    if (freeIdx == kNoSlot) return kNoResources;
    t.s[freeIdx] = Slot{ true, mc, bytes, 0, 0, 0 };
    *slotOut = freeIdx;
    return kOk;
}
// Reuse is safe ONLY when the slot is none of: the pending flip, the FRONT buffer (the one last latched: it is on screen from the next frame,
// and EARLIEST_INUSE only catches up to it later), or what the hardware is still fetching (EARLIEST_INUSE - the old front, until that moves).
// A free slot is trivially reusable. `earliestValid` false (the read failed) means "unknown": NOT reusable.
constexpr bool slot_reusable(const Table &t, uint32_t slot, uint64_t earliestMc, bool earliestValid) {
    return slot >= kMaxSlots ? false
         : !t.s[slot].used ? true
         : (!earliestValid ? false
            : (earliestMc != t.s[slot].mc && t.front != slot && !(t.pendActive && t.pendSlot == slot)));
}
// Drop a slot (BoFree of a pinned BO) only when it is reusable; false = the caller must restore the console first.
inline bool slot_unregister(Table &t, uint32_t slot, uint64_t earliestMc, bool earliestValid) {
    if (slot >= kMaxSlots || !t.s[slot].used) return true;
    if (!slot_reusable(t, slot, earliestMc, earliestValid)) return false;
    if (t.front == slot) t.front = kNoSlot;
    t.s[slot] = Slot{ false, 0, 0, 0, 0, 0 };
    return true;
}
// The exact flip-target set: the console and the registered slots, nothing else (dcn41's own window still applies underneath).
inline bool flip_target_ok(const Table &t, uint64_t mc) {
    if (mc == 0ull) return false;
    if (mc == t.consoleMc) return true;
    for (uint32_t i = 0; i < kMaxSlots; i++) if (t.s[i].used && t.s[i].mc == mc) return true;
    return false;
}
// Present bookkeeping, called immediately before the plane address is programmed (after the caller resolved the previous latch with
// latch_observe). A STILL-pending earlier Present (no VUPDATE has accepted it) is overwritten by this one and will never be shown: replaced. Returns the slot's MC to program, or 0 when the slot is not registered.
inline uint64_t present_begin(Table &t, uint32_t slot, uint64_t id, uint64_t targetFrame) {
    if (slot >= kMaxSlots || !t.s[slot].used) return 0ull;
    if (t.pendActive) t.replaced++;
    t.pendActive = true; t.pendSlot = slot; t.pendMc = t.s[slot].mc; t.pendId = id; t.pendTarget = targetFrame;
    t.s[slot].presents++;
    t.presents++;
    return t.s[slot].mc;
}
// One observation of the plane (from the VUPDATE handler, from Present / Status, or from the watchdog poll). `rawFlipPending` is
// DCSURF_FLIP_CONTROL.SURFACE_FLIP_PENDING: set when the address is written, CLEARED by the VUPDATE that accepts it (: "latched 120/120").
// The pending Present is LATCHED when that bit is clear; the latched frame is the (extended) OTG frame counter of the observation. It is NOT
// keyed on EARLIEST_INUSE: that follows the latch by a few lines to a frame and is what the REUSE rule reads (slot_reusable), so a Present
// made in the next frame is never mis-counted as having replaced a picture the display had in fact already accepted.
inline bool latch_observe(Table &t, bool rawFlipPending, uint64_t frameNow) {
    if (!t.pendActive || rawFlipPending) return false;
    Slot &s = t.s[t.pendSlot];
    s.latchedFrame = frameNow; s.latches++;
    if (t.latched == 0ull) t.firstLatchFrame = frameNow;
    t.lastLatchFrame = frameNow;
    t.latched++;
    t.front = t.pendSlot;
    t.pendActive = false;
    return true;
}
// A VUPDATE that latched nothing while a picture is already on screen is a repeat of the front buffer.
inline void note_vupdate_no_latch(Table &t) { if (t.front != kNoSlot) t.repeats++; }

// ---- restore verification ---------------------------------------------------------------------------------------------------
// The console is back only when the programmed address, EARLIEST_INUSE and the pending flag all agree with the console.
constexpr bool restore_verified(uint64_t plane, uint64_t earliest, bool hwPending, uint64_t consoleMc) {
    return consoleMc != 0ull && plane == consoleMc && earliest == consoleMc && !hwPending;
}

// ---- the restore state machine ------------------------------------------------------------------------------------------------
// The kext's scan_restore runs THIS machine against the real hardware, so the host test drives the same steps against a fake plane:
// program the console; poll until the hardware reports it latched and in use (200 polls of ~1 ms, 12 frames at 60 Hz); if it never does, program
// it ONCE more and poll again; then stop, verified or not. It never reports Done before the console has been programmed at least once.
constexpr uint32_t kRestorePolls = 200u, kRestoreAttempts = 2u;
enum RestoreAct : uint32_t { kActProgram = 1, kActPoll = 2, kActDone = 3 };
struct RestoreSm { uint32_t attempts; uint32_t polls; uint64_t consoleMc; bool verified; };
inline void restore_begin(RestoreSm &m, uint64_t consoleMc) { m.attempts = 0; m.polls = 0; m.consoleMc = consoleMc; m.verified = false; }
// First call (haveObs false): the console must be programmed. After every program the caller polls; each poll feeds its observation here.
inline RestoreAct restore_next(RestoreSm &m, bool haveObs, uint64_t plane, uint64_t earliest, bool hwPending) {
    if (m.attempts == 0u) { m.attempts = 1u; m.polls = 0u; return kActProgram; }   // the FIRST answer is always "program the console", whatever was observed
    if (haveObs && restore_verified(plane, earliest, hwPending, m.consoleMc)) { m.verified = true; return kActDone; }
    if (++m.polls < kRestorePolls) return kActPoll;
    if (m.attempts < kRestoreAttempts) { m.attempts++; m.polls = 0u; return kActProgram; }
    return kActDone;
}

// ---- a BO that hosts scanout slots goes away (BoFree, the HUNG leak, the close of the session) ---------------------------------------
// alwaysFull: leak / close - the console goes back FIRST, whatever the slots are doing. Otherwise a slot that is neither what the hardware is
// still fetching nor pending is simply dropped, and ANY slot that is (BoFree-while-front) forces the full restore before the BO is freed.
struct UnpinPlan { uint8_t dropMask; bool fullRelease; };
inline UnpinPlan unpin_plan(const Table &t, uint8_t pinMask, bool alwaysFull, uint64_t earliestMc, bool earliestValid) {
    if (alwaysFull) return UnpinPlan{ 0u, true };
    uint8_t drop = 0u;
    for (uint32_t i = 0; i < kMaxSlots; i++) {
        if (((pinMask >> i) & 1u) == 0u) continue;
        if (!slot_reusable(t, i, earliestMc, earliestValid)) return UnpinPlan{ 0u, true };
        drop = (uint8_t)(drop | (1u << i));
    }
    return UnpinPlan{ drop, false };
}

// ---- the rate-based IRQ storm guard ------------------------------------------------------------------------------------------
// More than kStormLimit classified DCE interrupts inside one kStormWindowNs window. Replaces the cumulative cap (which trips after
// ~28 minutes at 120 Hz) while the plane is taken. Returns true exactly once: on the interrupt that exceeds the limit.
struct Storm { uint64_t winStart; uint32_t n; bool tripped; };
inline bool storm_note(Storm &s, uint64_t nowNs) {
    if (s.n == 0u || nowNs < s.winStart || nowNs - s.winStart >= kStormWindowNs) { s.winStart = nowNs; s.n = 0u; }
    if (s.n < 0xFFFFFFFFu) s.n++;
    if (!s.tripped && s.n > kStormLimit) { s.tripped = true; return true; }
    return false;
}

// ---- the idle deadline -------------------------------------------------------------------------------------------------------
constexpr bool idle_expired(uint64_t nowNs, uint64_t lastNs) { return nowNs >= lastNs && nowNs - lastNs >= kIdleNs; }

// ---- the lock order, as data (pinned by the host test) -----------------------------------------------------------------------
// gCliLock (the native client) is ALWAYS taken before the scanout (DCN) lock, never after; the interrupt handler takes only the DCN lock;
// nothing sleeps while holding the DCN lock. Rank numbers: a thread may only take a lock of a HIGHER rank than any it holds.
constexpr uint32_t kRankCliLock = 1u, kRankScanLock = 2u;
constexpr bool lock_order_ok(uint32_t held, uint32_t taking) { return taking > held; }

} // namespace n48scan
