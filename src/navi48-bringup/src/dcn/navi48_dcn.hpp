//
//  navi48_dcn.hpp — the kext's seam onto the DCN 4.1 display layer (src/dcn41).
//
//  Everything here is display-only and read-mostly. The ONE thing that can write a
//  register is dcn41's own wreg callback, which this file implements, and that callback
//  refuses anything the write allowlist (src/dcn41/dcn41_allow.h) does not permit.
//
#ifndef Navi48Dcn_hpp
#define Navi48Dcn_hpp

#include <stdint.h>

class Navi48Bringup;
namespace amdgpu { struct BringupContext; }   // 0.0.635: disp2's plane op
struct n48_sf_dcn;   // build 0.0.542: apple/scanout_full.h
struct n48n_scan_query;    // build 0.0.603: Navi48NativeABI.h
struct n48n_scan_status;
struct n48n_mode_result;   // build 0.0.605: Navi48NativeABI.h

namespace n48dcn {

// Bind to the bring-up service and arm the display layer: reads the DMU segment bases
// out of the card's own IP discovery table, arms the write allowlist against them, and
// initialises the dcn41 device. Idempotent; returns 0 on success, non-zero status
// otherwise. No register is written.
uint32_t bind(Navi48Bringup *owner);

// `accel dcnstate` — read-only. Reports the allowlist counters, the allowlist's own
// on-hardware self-test, which OTG is lit, and the DCN interrupt counters.
uint32_t state(uint64_t arg, uint64_t *out, unsigned outCount);

// `accel dcnvbl <n>` — T2-VBL. 0 disable everything we enabled, 1 enable
// VUPDATE_NO_LOCK on the lit OTG, 2 enable VSTARTUP on the lit OTG. The only writes are
// the source's own enable/ack bits, and both are inside the allowlist.
uint32_t vbl(uint64_t arg, uint64_t *out, unsigned outCount);

// `accel dcnflip <n>` — T3-FLIP. 0 restore the console plane unconditionally (the escape hatch,
// idempotent, safe at any time), 1 the minimal machine-verified flip (one frame, CRC witness,
// immediate flip back), 2 the same but holding the test pattern ~5 s for the user to see. The
// geometry comes from the HUBP's own viewport/pitch/format registers, never from a constant; the
// buffer is inside the frame-buffer window the hardware reports; and the restore runs on EVERY
// path, including the interrupt watchdog described in the .cpp.
uint32_t flip(uint64_t arg, uint64_t *out, unsigned outCount);

// `accel dcnmode <n>` — the first mode-set: a SAME-MODE RE-TIMING of the lit output.
//   0        read-only: capture and decode the OTG timing set, and restore the boot-time capture if
//            the live registers have drifted from it. THE EMERGENCY COMMAND.
//   1        capture, write the identical values back, re-capture, require every dword to match.
//   2..30    the same with that many seconds of dwell, with a frame-counter stall watchdog.
// Nothing is computed, so nothing can be computed wrongly: the values written are the values read.
uint32_t mode(uint64_t arg, uint64_t *out, unsigned outCount);

// Called from Navi48Bringup::handleInterrupt for EVERY drained IH entry. Returns true if
// the entry was a DCE (display) one and has been classified, acknowledged and counted, in
// which case the caller must not also count it as irqOther.
bool ihEntry(uint32_t clientId, uint32_t srcId, uint32_t srcData0);

// build 0.0.514 B2: the LIT OTG's raster, READ-ONLY (0.0.515: through attach()'s read-only device, never bind's) (OTG_CONTROL master enable, OTG_H/V_BLANK_START_END,
// OTG_H/V_TOTAL - no register written), with its pixel clock and porches taken from the sink's EDID row that has the SAME active
// size and totals (src/dcn41/dcn41_modes.h). 1 = a lit OTG was read AND an EDID row matched; 0 = unknown (the caller falls back).
uint32_t liveRaster(uint32_t *hAct, uint32_t *vAct, uint32_t *hTot, uint32_t *vTot, uint32_t *hFront, uint32_t *hSync,
                    uint32_t *vFront, uint32_t *vSync, uint64_t *pixelClockHz);

// build 0.0.515: build the READ-ONLY raster device liveRaster reads through (navi48_liveraster.h),
// once, from the device context and the IP-discovery DMU bases. No register is read or written here, and nothing is armed:
// liveRaster no longer depends on bind() (DCN verbs 74-77). Called from Navi48Bringup::start, before any hook can call
// liveRaster. Idempotent.
void attach(Navi48Bringup *owner);

// build 0.0.518: flip mode's HUBP0 access, all through the BOUND device (bind() must have
// succeeded; every function answers DCN41_E_UNINIT otherwise). fmHubpGeom reads HUBP0's viewport, pitch, format and SW_MODE
// (refuses unless OTG0 is the lit OTG); fmReadFront = dcn41_hubp_is_flip_pending(0); fmReadPrimary = the programmed address;
// fmFlip = dcn41_hubp_program_flip(0, mc, vmid 0, tmz 0, immediate false) under `tag`; fmSetExact restricts flips to {a, b}
// (n 2) or lifts the restriction (n 0); fmFrameCount = OTG0's frame counter; fmTestFlipHeld = dcnflip's pattern is on screen.
uint32_t fmArmed();
uint32_t fmTestFlipHeld();
int fmHubpGeom(uint32_t *vpW, uint32_t *vpH, uint32_t *pitchPx, uint32_t *fmt, uint32_t *swMode);
int fmReadFront(uint32_t *pending, uint64_t *earliest);
int fmReadPrimary(uint64_t *mc);
int fmFlip(uint64_t mc, const char *tag);
int fmSetExact(uint64_t a, uint64_t b, uint32_t n);
int fmFrameCount(uint32_t *fc);
uint64_t fmAllowCounts(uint64_t *refused);
// build 0.0.519: the A/B test's witness, dcnflip's own OTG CRC on OTG0 (refused unless
// OTG0 is the lit OTG): fmCrcBegin enables it over the active raster (crc_enable, the same writes dcnflip makes) and waits 3
// frames; fmCrcRead waits `frames` frames (bounded, wait_frames) then reads CRC0 R/G and B; fmCrcEnd disables it (crc_disable).
int fmCrcBegin();
int fmCrcRead(uint32_t frames, uint32_t *rg, uint32_t *b);
void fmCrcEnd();

// build 0.0.542 (apple/scanout_full.h, `accel scanout full`): what HUBP0 is scanning out, through the READ-ONLY device attach()
// built at start() (navi48_liveraster.h n48lr_scan_surface; no register is written, no bind() needed). 1 = every read made.
uint32_t roScanSurface(n48_sf_dcn *s);

// build 0.0.603 (native S2a; contract: the "ABI 1.1 addendum" of an internal design note, design an internal design note + Review).
// The native scanout path behind the N48N selectors 9..14. All of it needs bind() to have succeeded and a NATIVE boot (Acquire checks the
// S1b gate), and none of it runs unless a native client calls it. Return values are IOReturn codes (n48scan::k*); 0 = success.
//   LOCK ORDER: the native client's lock (gCliLock) is taken BEFORE the scanout lock, never after; the interrupt handler takes only the
//   scanout lock; nothing sleeps under it.
uint32_t scanQuery(struct n48n_scan_query *o);                       // read-only, valid before Acquire
uint32_t scanAcquire(uint64_t out[2], uint32_t sess);                // out: [0] console MC, [1] extended OTG frame count. sess = the native session (n1c_session_seq; 0 = none): only a row-120 HOLD (ABI 1.7) lets a session Acquire while the mode trial is busy
uint32_t scanRegister(bool boVis, uint64_t boMc, uint64_t boSize, uint64_t offset, uint32_t pitchBytes, uint32_t width, uint32_t height,
                      uint32_t format, uint64_t out[2]);            // out: [0] slot id, [1] slot MC; every bound is checked before any state changes
uint32_t scanPresent(uint64_t slot, uint64_t flags, uint64_t out[3]); // out: [0] present id, [1] target frame, [2] VUPDATE count at program
uint32_t scanStatus(struct n48n_scan_status *o);
// THE console restore (one function): programs the console, waits for the latch by polling, verifies, tears the state down. Idempotent.
// out (may be null): [0] verified 1/0, [1] HUBP0's programmed address after.
uint32_t scanRelease(const char *why, uint64_t out[2]);
// A BO hosting scanout slots is going away (BoFree, the HUNG leak, the close of the session): n48scan::unpin_plan decides; alwaysFull (leak /
// close) or any slot that is shown or pending restores the console FIRST. Returns 0 = nothing to do or slots just dropped; 1 = the console was
// restored and VERIFIED; 2 = the restore did NOT verify (now or earlier this acquisition): the caller must LEAK the BO, never free it.
// pinGen: the scanGeneration() the pins were recorded under; a stale pin is a no-op. out (may be null) receives [verified, plane] when a restore ran.
uint32_t scanBoGone(uint32_t pinMask, uint32_t pinGen, bool alwaysFull, uint64_t *out);
// Kext stop: the restore, then a bounded wait for the watchdog threads to finish (call BEFORE the registers are unmapped).
void scanShutdown();
// build 0.0.605 (native S2d, ABI 1.3; 0.0.606 ABI 1.4: rows 1 / 2 and the flags word): the timed mode trial behind N48N selector 16. Runs one trial (row 1, 2, 50 or 120, dwell ms, N48N_MODE_TF_* flags), blocking, and always fills *o (the verdict word is inside);
// returns 0 or an IOReturn only for a null out / no lock. Needs bind() and a native boot (else DENIED). Everything it writes is inside the DCN allowlist; nothing it writes stays: see
// dcn/navi48_modetrial_pure.h. `dcnmode 0` and the kext stop restore the same golden copy.
uint32_t modeTrial(uint64_t row, uint64_t dwellMs, uint64_t flags, struct n48n_mode_result *o);
// True while a mode trial runs (the DAL step refuses to start then).
bool modeTrialBusy();
// build 0.0.609 (ABI 1.7): the HELD mode of row 120 (selectors 17 / 18) and the session hook. modeHold runs the row-120 trial on a kernel thread and returns with the mode UP (*o = the entry snapshot, verdict HELD) or, when the trial
// was denied or failed, with its ordinary result; modeRelease ends the hold and returns the final result. Both return 0 or an IOReturn-like n48scan code only for a null out / no lock / a timeout; the verdict is inside *o.
// modeHoldSessionClosed(seq): a native session closed (called by n1c_close after its console restore); atomic words only.
uint32_t modeHold(uint32_t sess, uint64_t maxMs, uint64_t flags, struct n48n_mode_result *o);
uint32_t modeRelease(uint64_t flags, struct n48n_mode_result *o);
void modeHoldSessionClosed(uint32_t sess);
// 0.0.614 (native AGDC, amd/native_agdc_pure.h): true while a row-120 hold is launching or up (the claim to the runner's end). ONE atomic load of each word: no lock, no register, callable from anywhere.
bool modeHoldActive();
uint32_t scanGeneration();
// build 0.0.622 (multi-monitor track, stage M1; dcn/navi48_dispread.h, an internal design note): the three instruments behind accel verbs 91 / 92 / 93. Each fills out[0..12] (13 scalars) and returns the
// N48DR_ status (out[0]'s low byte). ddcRead needs bind() (its writes go through the DCN allowlist); dmubRing and dispCensus read through the read-only device attach() built and need nothing else.
uint32_t ddcRead(uint64_t arg, uint64_t *out, unsigned outCount);
uint32_t scdcRead(uint64_t arg, uint64_t *out, unsigned outCount);   // 0.0.633: verb 99, READ-ONLY SCDC read over the same DC_I2C engine as ddcRead (same busy flag)
uint32_t dmubRing(uint64_t arg, uint64_t *out, unsigned outCount);
uint32_t dispCensus(uint64_t arg, uint64_t *out, unsigned outCount);
// build 0.0.624 (stage M1.5; dcn/navi48_dispread.h): verb 94 `region4read`. READ-ONLY: DMCUB register reads (dcn41_dmub_probe) for the window base, then navi48_vram_read_mm. Same 13-scalar contract.
uint32_t region4Read(uint64_t arg, uint64_t *out, unsigned outCount);
// build 0.0.625 (stages M2 / M3; dcn/navi48_dmubcmd.h): verbs 95 `dmubsend`, 96 `dmubmode`, 97 `dmubctx`. The FIRST code that sends to the display firmware; ALL behind boot-arg navi48-dmubcmd=1 (default OFF: N48DR_CMD_OFF,
// nothing touched). Same 13-scalar contract. dmubSend needs bind() (its one register write, DMCUB_INBOX1_WPTR, goes through the DCN write allowlist); VRAM only through navi48_vram_write_mm.
bool dmubCmdLatchedOn();   // 0.0.626 (F4): the navi48-dmubcmd latch; the dispatcher asks it before bind()
uint32_t dmubSend(uint64_t arg, uint64_t *out, unsigned outCount);
uint32_t dmubMode(uint64_t arg, uint64_t *out, unsigned outCount);
uint32_t dmubCtx(uint64_t arg, uint64_t *out, unsigned outCount);
// build 0.0.631 (stage M4d; dcn/navi48_disp2.h): verb 98 `disp2 timing|connect|off|status` - the OTG1 -> DIG2 test pattern. Behind boot-arg navi48-disp2=1 (default OFF: N48D2_OFF, nothing touched).
// Same 13-scalar contract. Needs bind() (every write: the instance guard, then the DCN write allowlist).
bool disp2LatchedOn();     // the navi48-disp2 latch; the dispatcher asks it before bind()
uint32_t disp2(uint64_t arg, uint64_t *out, unsigned outCount, amdgpu::BringupContext *ctx = nullptr);   // 0.0.635: ctx = the bring-up context (the plane op's buffers come from its VRAM allocator, through amdgpu::n1c_d2_alloc / n1c_d2_free)
// build 0.0.652 (M5): what `fbpublish 2` (Navi48DisplayNub.cpp) reads from the display layer. disp2Held: the plane is HELD (stage HELD or the pair pinned): atomic loads only. fbView: the software facts (NO register is read).
// fbLive: the plane's live gates, READS ONLY (the busy flag of disp2): returns N48D2_OK / N48D2_BUSY / N48D2_NO_DEVICE; *holdBad = n48d2_hold_verdict over them (0 = buffer A is the front buffer and the plane is healthy), gates = the N48D2_GATES dwords.
// fbEdid: a display's EDID blocks 0 and 1 (256 bytes) over its DDC line (the monitor B: 3; 0.0.658: the monitor A: 2) through the same engine sequence as `accel ddcread` (the DC_I2C engine's registers only, via the DCN allowlist); returns an N48DR_ status.
// 0.0.658: fbView / fbLive take the disp2 INSTANCE (2 = the monitor B, 1 = the monitor A): the software facts and the live gates of THAT instance's plane; fbEdid takes the DDC line (the caller got it from n48disp_geom_for_index).
struct FbView { bool held, pinned, devOk; uint64_t mc[2]; uint64_t offA, bar0Phys, bar0Size; uint32_t cur; uint64_t bufBytes; };      // bufBytes (0.0.658): the plane buffer's allocation size of the instance (the aperture must lie inside it)
bool disp2Held();
bool disp2HeldInst(uint32_t inst);      // 0.0.655: HELD is per instance (disp2Held() = the monitor B's)
void fbView(FbView *v, uint32_t inst);
uint32_t fbLive(uint32_t inst, uint32_t *holdBad, uint32_t *gates);
uint32_t fbEdid(uint32_t ddcLine, uint8_t out[256]);
// 0.0.617 (K6): true from a native client's scanout Acquire until the console restore completed (release, watchdog, IRQ storm guard, N48N close). ONE atomic load: no lock, no register, callable from the display
// pipe's hooks (the transaction path); it never waits on the scanout lock.
bool scanActive();
// 0.0.618 (V1): ONE read-only timing sample of the lit OTG for the display pipe's vblank timestamps. No register is written; nothing is armed (it reads through the read-only device attach() built, like liveRaster).
// *periodNs = the refresh period (hTotal * vTotal / pixel clock), *delayNs = nanoseconds from the sample to the next start of vertical blank, *nowAbs = mach_absolute_time at the position read.
// The totals, blank start and pixel clock are re-read at most every 250 ms (cached); the position is read on every call. false = no coherent sample (no device, no lit OTG, a mode trial running, a
// position outside the raster, a period outside 1 ms .. 100 ms): the caller writes nothing.
bool vblSample(uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs);
// 0.0.659 (M6 Stage 1a): the same sample for the monitor A (inst 1, OTG1) / the monitor B (inst 2, OTG2) from THAT display's own OTG; w, h, pixHz are the geometry table's (the raster must be lit with exactly that active size). Reads only.
bool vblSampleInst(uint32_t inst, uint32_t w, uint32_t h, uint64_t pixHz, uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs);
// ---- 0.0.661 (M6 Stage 1b, ABI 1.11, behind the latches navi48-m6=1 AND navi48-m6flip=1) + 0.0.662 (M6 Stage 2, ABI 1.12, instance 1 also behind navi48-m6flip1=1): the HDMI displays' scanout - INSTANCE 2 = the monitor B (HUBP2 / OTG2) and
// INSTANCE 1 = the monitor A (HUBP1 / OTG1), poll-only. The sequences are dcn/navi48_scanx_flow.h (host-tested over a fake HUBPn, one XDesc per instance). Every function takes the INSTANCE (1 or 2; anything else is BadArgument).
// With a latch OFF every function below touches NOTHING (no register, no lock) and the selectors answer Unsupported (instance 1 without navi48-m6flip1: BadArgument at the client, exactly 0.0.661's answer). Slot ids are TAGGED (instance 1: 0x10 | k, instance 2: 0x20 | k).
uint32_t scanXAcquire(uint32_t inst, uint64_t out[5]);                  // out: [0] A (the instance's console MC), [1] extended OTG frame count, [2] the M6 table generation, [3] 0 (the native client fills the free visible-VRAM figure), [4] geometry: w | h << 16 | pitchPx << 32
uint32_t scanXRegister(uint32_t inst, bool boVis, uint64_t boMc, uint64_t boSize, uint64_t offset, uint32_t pitchBytes, uint32_t width, uint32_t height, uint32_t format, uint64_t out[2]);   // out: [0] tagged slot id, [1] slot MC
uint32_t scanXPresent(uint32_t inst, uint64_t slotId, uint64_t flags, uint64_t out[3]);   // out: [0] present id, [1] target frame, [2] 0
uint32_t scanXStatus(uint32_t inst, struct n48n_scan_status *o, uint64_t out[4]);      // out: [0] table generation, [1] reuse_inuse_refused, [2] restores | restoreFailures << 32, [3] write refusals | write failures << 32
uint32_t scanXRelease(uint32_t inst, const char *why, uint64_t out[2]);                // THE restore of A (Restore); idempotent; out: [0] verified 1/0, [1] plane MC after
uint32_t scanXBoGone(uint32_t inst, uint32_t pinMask, uint32_t pinGen, bool alwaysFull, uint64_t *out);   // 0 nothing / dropped, 1 restored and verified, 2 NOT verified: LEAK the BO
uint32_t scanXGeneration(uint32_t inst);                                 // the acquisition generation of that instance (0 for any other number)
uint32_t scanXGpuHeld(uint32_t inst);                                    // 0 = not GPU-held (or not instance 1 / 2, or a latch OFF: nothing read); 1..3 = the instance is acquired and slot k+1 is what its HUBP fetches; 4 = acquired, EARLIEST is not a slot (n48scanx::gpu_held_code). disp2's status / crc / fbLive report it instead of a latch failure
bool scanXClash(uint64_t mc, uint64_t bytes);                           // true when [mc, mc + bytes) overlaps an HDMI display's A, B or a registered slot (instance 0's Register asks it with the latches ON; the monitor A's only with navi48-m6flip1)
uint32_t scanXReport(uint64_t arg, uint64_t *v, unsigned n);            // accel `m6xstat [page]` (action 107): READ-ONLY report of an HDMI instance's scanout state; arg = page | instance << 8 (instance 0 = the monitor B)
void scanXShutdown();                                                   // kext stop: the restores, then a bounded wait for the watchdog threads
// `dcnflip 0` on a native boot: the same restore, and it also acts when nothing is acquired but HUBP0 is off the console learned earlier.
void scanEscape(const char *why);
void scanLogState();

}  // namespace n48dcn

#endif /* Navi48Dcn_hpp */
