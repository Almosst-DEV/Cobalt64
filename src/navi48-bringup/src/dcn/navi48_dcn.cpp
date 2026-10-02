//
//  navi48_dcn.cpp — the kext side of Track D stage 1: our own DCN 4.1 display control.
//
//  Three jobs, and nothing else:
//    1. arm the DCN 4.1 layer (src/dcn41) against the DMU segment bases THIS card's IP
//       discovery table reports, with the write allowlist armed against the same bases;
//    2. classify, acknowledge and count DCE interrupts out of our IH ring (T2-VBL);
//    3. answer the two read-mostly verbs `dcnstate` and `dcnvbl`.
//
//  SAFETY. The only path from this file to a register write is dcn_wreg(), and dcn_wreg()
//  calls dcn41_allow_write() first and returns without touching hardware on false. There
//  is no second path: the dcn41 layer holds no pointer to the card, only this callback.
//  The allowlist refuses everything outside the explicit DCN 4.1 display ranges, and
//  refuses the SMN indirect windows and the named SMU/PSP/SMUIO blocks before it even
//  consults those ranges (src/dcn41/dcn41_allow.h). Nothing in this file reaches power,
//  voltage, clock-limit or firmware-ROM state, and T2 writes exactly one field:
//  OTG_GLOBAL_SYNC_STATUS's interrupt enable/ack bits on the lit OTG.
//
#include "navi48_dcn.hpp"
#include "Navi48Bringup.hpp"
#include "ipdiscovery.hpp"
#include "amd/amdgpu_regs.h"
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>            // build 0.0.603: kernel_thread_start, the scanout watchdog (the kext's established out-of-lock deferral)
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>         // build 0.0.607: PE_parse_boot_argn (navi48-row120)
#include "amd/smu_dal.h"            // build 0.0.605: amdgpu::dal_busy() - the mode trial and the DAL step refuse each other
#include "amd/native_s1b.h"         // build 0.0.603: native_s1b_state() - the scanout path exists on native boots only
#include "Navi48NativeABI.h"        // build 0.0.603: n48n_scan_query / n48n_scan_status (filled here)
#include "navi48_scanout_pure.h"    // build 0.0.603: the decisions (host-tested)
#include "amd/native_disp_pure.h"     // 0.0.618 (V1): vbl_period_ns / vbl_delay_ns (the vblank timing sample)
#include "navi48_modetrial_pure.h"   // build 0.0.605 (native S2d): the timed mode trial - decisions AND sequences, host-tested through a Hw interface

extern "C" {
#include "dcn41.h"
#include "dcn41_allow.h"
#include "dcn41_otg_timing.h"
#include "dcn41_modes.h"      // build 0.0.514 B2: the sink's EDID timings (pixel clock, porches) for liveRaster
}
#include "navi48_liveraster.h"   // build 0.0.515: the read-only raster device (no bind dependency)

namespace {

// How many DCE IH entries we log verbatim per enable. Counting is not witnessing, so the
// first few are kept in full; after that only the counters move (a 60 Hz source would
// otherwise write 60 lines a second into a 2 MiB ring).
constexpr uint32_t kDceLogBudget = 12;

// Storm guard. A source we enabled must never be able to spin the work loop indefinitely:
// past this many entries since the enable, the handler disables it itself and says so. At
// the expected 60 Hz this is 55 minutes; at any storm rate it trips in well under a second.
constexpr uint64_t kDceStormCap = 200000;

// The first REAL timing change: how many lines to add to OTG_V_TOTAL. Compiled in, not caller-chosen.
// 1481 -> 1501 lines takes the lit output from 59.951 Hz to 59.152 Hz. Chosen because:
//   * only v_total moves, so h_total and the pixel clock are untouched and the HORIZONTAL line rate
//     stays 88.79 kHz - the DP link, its symbol clock and the PHY see no change whatsoever;
//   * the active area is untouched (the guard in dcn41_otg_timing_adjust_v_total refuses anything
//     that would collide with the vertical blank start);
//   * 59.152 Hz is well inside the SINK-A's OWN advertised vertical range of 48..180 Hz, read
//     from its EDID range descriptor (edid-capture.txt, descriptor 0xfd:
//     0x30 = 48, 0xb4 = 180);
//   * it is 8 frames in 10 seconds away from the old rate, so the frame counter can prove it.
constexpr int32_t kVTotalDeltaLines = 20;

struct DcnState {
	Navi48Bringup       *owner      { nullptr };
	amdgpu::DeviceContext *dev      { nullptr };
	bool                 armed      { false };
	uint32_t             initStatus { 0xFFFFFFFFu };
	uint32_t             seg[DCN41_NUM_SEGS] { 0, 0, 0, 0, 0 };
	struct dcn41_dev     d          {};
	struct dcn41_allow_state allow  {};
	const char          *tag        { "?" };

	// what we enabled, so `dcnvbl 0` and the storm guard can undo exactly that
	bool     srcEnabled { false };
	uint8_t  srcKind    { 0 };            // enum dcn41_irq_kind
	uint8_t  srcInst    { 0 };
	uint64_t enabledAtNs { 0 };
	uint64_t entriesAtEnable { 0 };
	bool     stormTripped { false };

	// IH counters
	uint64_t dceEntries { 0 };
	uint64_t dceByKind[DCN41_IRQ_KIND_COUNT] { };
	uint64_t dceVupdate[DCN41_NUM_PIPES] { };
	uint64_t dceVstartup[DCN41_NUM_PIPES] { };
	uint64_t dceUnclassified { 0 };
	uint64_t ackFailures { 0 };
	uint64_t firstEntryNs { 0 }, lastEntryNs { 0 };
	uint32_t logBudget { 0 };

	// ---- T3-FLIP state ----
	bool     flipPrepared { false };
	uint64_t origMc       { 0 };      // the console plane address, saved before the first flip
	uint64_t testMc       { 0 };      // our test buffer, MC space
	uint64_t testVramOff  { 0 };      // the same buffer as a VRAM byte offset (for the BAR0 fill)
	uint64_t testBytes    { 0 };
	uint32_t fbWidth      { 0 };      // ALL FOUR read from the HUBP's own registers, never assumed
	uint32_t fbHeight     { 0 };
	uint32_t fbPitchPx    { 0 };
	uint32_t fbPixFmt     { 0 };
	uint32_t fbSwMode     { 0xFFFFFFFFu };
	uint32_t flipHubp     { 0 };
	uint32_t flipOtg      { 0 };
	bool     flipHeld     { false };  // our buffer is on screen RIGHT NOW
	uint32_t heldFrames   { 0 };
	uint32_t heldFrameCap { 0 };
	uint64_t flipsIn      { 0 }, flipsOut { 0 }, watchdogRestores { 0 };

	// ---- the same-mode re-timing ----
	bool     goldenValid  { false };    // the timing as it was BEFORE we ever wrote it, this boot
	struct dcn41_otg_timing golden {};
	uint64_t retimings    { 0 }, retimingStalls { 0 }, goldenRestores { 0 };
};

DcnState gDcn;

// ---- build 0.0.603 (native S2a): the native scanout state. Everything below is inert unless a native N48N client Acquires. ----
// LOCK ORDER (kext-wide, pinned by tests/native_s2a_test.cpp): the native client's lock (amd/native_s1c.cpp) -> gScan.lock, never the other way. The
// interrupt handler takes ONLY gScan.lock; nothing sleeps while holding it (the restore drops it around every wait), so ihEntry can
// never wait on a sleeper. One field is read WITHOUT the lock: `active` (ihEntry's cheap test).
struct ScanState {
	IOLock  *lock        { nullptr };
	volatile uint32_t active { 0 };     // 1 from Acquire until the restore completes
	bool     acquired    { false };
	bool     restoring   { false };
	bool     haveConsole { false };     // the console MC was learned at a first Acquire this boot (kept across releases for `dcnflip 0`)
	bool     geomWarned  { false };
	bool     wantRestore { false };     // set by the IRQ handler (which may not sleep): the watchdog thread does the restore
	const char *wantWhy  { nullptr };
	uint64_t consoleMc   { 0 };
	uint64_t consoleBytes { 0 };
	uint32_t width { 0 }, height { 0 }, pitchPx { 0 };
	uint32_t gen         { 0 };         // bumped at every Acquire; a pin recorded under an older one is stale
	n48scan::Table tbl   {};
	n48scan::FcExt fc    {};
	n48scan::Storm storm {};
	uint64_t presentSeq { 0 }, vupdates { 0 }, latchIrq { 0 }, latchPoll { 0 }, refused { 0 }, geomRefused { 0 };
	uint64_t wdRestores { 0 }, stormTrips { 0 }, acquires { 0 }, restores { 0 }, restoreFailures { 0 };
	bool     restoreFailed { false };   // the last restore was NOT verified: the plane may still be scanning a client buffer, so pinned BOs must be LEAKED, never freed
	volatile uint32_t watchdogAlive { 0 };   // watchdog threads not yet finished (Navi48Bringup::stop waits for 0 before it unmaps the registers)
	uint64_t firstLatchNs { 0 }, lastLatchNs { 0 }, lastActivityNs { 0 };
};
ScanState gScan;

// ---- build 0.0.605 (native S2d): the timed mode trial. State only; the Hw callbacks and the selector are at the end of this file. ----
// LOCK RANK: gMtLock is a LEAF (nothing that takes another lock is called under it: only register I/O, the clock, the log, IOSleep / IODelay). 0.0.606: the trial's hardware sequences (the DP1 resync,
// the OTG update lock, the restore) HOLD it across their bounded waits (IODelay of a few microseconds, IOSleep(60) in the blank), so the watchdog's restore and the trial thread can never interleave; the
// longest hold is one resync with its retry (~1 s). Everything that can contend for it (the watchdog, `dcnmode 0`, request_abort, the kext stop) only ever blocks briefly on it, in a context that may sleep.
// The scanout lock is taken WITHOUT it (mt_plane_acquired);
// scanAcquire reads gMt.busy with a plain atomic load under the scanout lock, so it never waits on gMtLock. The interrupt handler never touches either.
// 0.0.609 (HELD mode, row 120): LOCK ORDER, all four of them (pinned by tests/native_s2d_test.cpp): the native client lock -> scan lock -> [nothing]; gMtLock is a LEAF and is never held together with the scan lock
// (in either order) nor with the client lock; SmuSeq (the PMFW mailbox, taken by the clock hold's raise / release inside the amd layer) is never held with any of them, and the hold's raise / release run with NO lock held.
// The hold runs on ONE kernel thread that is the trial thread (mt_hold_runner); its end goes through THE restore (restore_run), whose FIRST step (hold_begin_end -> mt_scan_release -> scan_restore) takes the scan
// lock with no other lock held; only THEN does it take gMtLock for the timing restore. scanAcquire (scan lock held) reads the hold's atomic words only. The client's close takes the client lock -> scan lock (its own
// plane restore), drops the scan lock, and then only stores atomic words (modeHoldSessionClosed): no DAL call, no sleep and no gMtLock anywhere under the client lock. No DAL / PMFW call is made under any of the locks.
n48mt::State gMt;
IOLock *gMtLock = nullptr;
volatile uint32_t gMtHoldAlive = 0;   // 0.0.609: hold runner threads not yet finished (scanShutdown waits for 0 before the registers are unmapped)
volatile uint32_t gMtWdAlive = 0;   // watchdog threads not yet finished (scanShutdown waits for 0 before the registers are unmapped)

// build 0.0.515: the read-only raster device (navi48_liveraster.h), built by attach() at start().
n48lr_ro gLr {};
// Its read callback: a plain BAR5 read through the device context, no allowlist bookkeeping (it is not the armed layer).
uint32_t lr_rreg(void *cookie, uint32_t abs_dword) {
	return amdgpu::RREG32(*static_cast<amdgpu::DeviceContext *>(cookie), abs_dword);
}

uint64_t now_ns() {
	uint64_t abs = mach_absolute_time();
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

// ---- the dcn41 device callbacks -------------------------------------------------------
// rreg is ungated (a read cannot change the card) but counted; wreg is the allowlist gate.

uint32_t dcn_rreg(void *cookie, uint32_t abs_dword) {
	auto *s = static_cast<DcnState *>(cookie);
	dcn41_allow_note_read(&s->allow);
	return amdgpu::RREG32(*s->dev, abs_dword);
}

void dcn_wreg(void *cookie, uint32_t abs_dword, uint32_t value) {
	auto *s = static_cast<DcnState *>(cookie);
	if (!dcn41_allow_write(&s->allow, abs_dword, value, s->tag)) {
		const char *why = nullptr;
		const int reason = dcn41_allow_classify(abs_dword, s->allow.mmio_dwords, nullptr, &why);
		N48LOG("dcn: REFUSED write %#010x = %#010x (tag %s) — allowlist says %s: %s",
		       abs_dword, value, s->tag, dcn41_allow_reason_name(reason), why ? why : "(no text)");
		return;
	}
	amdgpu::WREG32(*s->dev, abs_dword, value);
}

void dcn_udelay(void *cookie, uint32_t us) {
	(void)cookie;
	IODelay(us);
}

// ---- which OTG is lit -----------------------------------------------------------------
// Read, never assumed: T1 found OTG0, but the census is a measurement of one
// boot and the GOP is free to pick another pipe. -1 when none is master-enabled.
int lit_otg() {
	if (!gDcn.armed) return -1;
	for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
		bool enabled = false;
		uint32_t w = 0, h = 0;
		if (dcn41_otg_get_active_size(&gDcn.d, i, &enabled, &w, &h) == DCN41_OK && enabled)
			return (int)i;
	}
	return -1;
}

}  // namespace

namespace n48dcn {

// build 0.0.515: NO bind dependency. 0.0.514 went through lit_otg(), which answers -1 unless
// gDcn.armed (set only by bind, i.e. only after a DCN verb 74-77 ran), so a native-1440p boot's first LINKCFG reply used the CEA
// fallback. The raster now comes from the READ-ONLY device attach() built at start() (navi48_liveraster.h: its write callback
// writes nothing; the two readers read OTG_CONTROL, OTG_H/V_BLANK_START_END and OTG_H/V_TOTAL only). Same reads, same answers.
uint32_t liveRaster(uint32_t *hAct, uint32_t *vAct, uint32_t *hTot, uint32_t *vTot, uint32_t *hFront, uint32_t *hSync,
                    uint32_t *vFront, uint32_t *vSync, uint64_t *pixelClockHz) {
	return n48lr_live(&gLr, hAct, vAct, hTot, vTot, hFront, hSync, vFront, vSync, pixelClockHz);
}

// build 0.0.542: `accel scanout full`'s display read - the read-only device only (never gDcn, never a write).
uint32_t roScanSurface(n48_sf_dcn *s) { return n48lr_scan_surface(&gLr, s); }

// build 0.0.515: the read-only raster device, built ONCE at start() - no register I/O (dcn41_dev_init only checks its
// arguments) - from the same device context and IP-discovery DMU bases bind() uses. Nothing here arms the display layer.
void attach(Navi48Bringup *owner) {
	if (!owner || gLr.state != N48LR_NONE) return;
	amdgpu::DeviceContext *dev = owner->deviceContext();
	if (!dev || !dev->rmmio || dev->rmmioSize == 0) {
		N48LOG("dcn: read-only raster path NOT BUILT - no mapped BAR5 in the device context (the LINKCFG reply keeps the CEA fallback)");
		return;
	}
	const IpDiscovery &disc = owner->ipDisc();
	IpDiscovery::IpEntry e {};
	if (!disc.isValid() || !disc.findIp(IpDiscovery::HwDmu, 0, e) || e.numBases < DCN41_NUM_SEGS) {
		N48LOG("dcn: read-only raster path NOT BUILT - no DMU (hwId 271) entry with %u segments in discovery", DCN41_NUM_SEGS);
		return;
	}
	uint32_t seg[DCN41_NUM_SEGS];
	for (uint32_t i = 0; i < DCN41_NUM_SEGS; i++) seg[i] = e.bases[i];
	const uint32_t ok = n48lr_ro_build(&gLr, dev, lr_rreg, seg, (uint32_t)(dev->rmmioSize / 4));
	N48LOG("dcn: read-only raster path %s (dcn41_dev_init %d; segs %08x %08x %08x %08x %08x; no register read or written here)",
	       ok ? "BUILT" : "NOT BUILT", gLr.init_rc, seg[0], seg[1], seg[2], seg[3], seg[4]);
}

static uint32_t bind_locked(Navi48Bringup *owner);
static void mt_golden_at_bind();                       // build 0.0.605: defined with the mode-trial glue at the end of this file
static bool mt_emergency_restore(const char *why);     // ditto: `dcnmode 0` on a native boot restores the FULL golden set
// build 0.0.603: bind() is serialised - the N48N selectors and the legacy accel verbs can both reach it concurrently on a native boot.
uint32_t bind(Navi48Bringup *owner) {
	if (__atomic_load_n(&gDcn.armed, __ATOMIC_ACQUIRE)) return 0;
	static IOLock *bindLock = nullptr;
	if (bindLock == nullptr) {
		IOLock *nl = IOLockAlloc();
		if (nl != nullptr && !OSCompareAndSwapPtr(nullptr, nl, (void *volatile *)&bindLock)) IOLockFree(nl);
	}
	if (bindLock == nullptr) return 1;
	IOLockLock(bindLock);
	const uint32_t r = bind_locked(owner);
	IOLockUnlock(bindLock);
	return r;
}
static uint32_t bind_locked(Navi48Bringup *owner) {
	if (gDcn.armed) return 0;
	if (!owner) return 1;

	gDcn.owner = owner;
	gDcn.dev = owner->deviceContext();
	if (!gDcn.dev || !gDcn.dev->rmmio || gDcn.dev->rmmioSize == 0) {
		N48LOG("dcn: no mapped BAR5 — display layer stays INERT");
		return 2;
	}

	// The segment bases come from the card, not from a constant: IP discovery hwId 271
	// (DMU) instance 0, the same entry Navi48Ttl::dumpIpTable prints.
	const IpDiscovery &disc = owner->ipDisc();
	if (!disc.isValid()) {
		N48LOG("dcn: IP discovery is not valid — display layer stays INERT (no segment bases)");
		return 3;
	}
	IpDiscovery::IpEntry e {};
	if (!disc.findIp(IpDiscovery::HwDmu, 0, e) || e.numBases < DCN41_NUM_SEGS) {
		N48LOG("dcn: no DMU (hwId 271) entry with %u segments in discovery — display layer stays INERT",
		       DCN41_NUM_SEGS);
		return 4;
	}
	for (uint32_t i = 0; i < DCN41_NUM_SEGS; i++) gDcn.seg[i] = e.bases[i];

	const uint32_t mmioDwords = (uint32_t)(gDcn.dev->rmmioSize / 4);

	// Arm the allowlist FIRST. If it refuses the bases, nothing else is armed and every
	// write stays refused — deny by default, all the way up.
	const int ar = dcn41_allow_init(&gDcn.allow, gDcn.seg, mmioDwords);
	if (ar != DCN41_ALLOW_OK) {
		N48LOG("dcn: the write allowlist REFUSED to arm (%s) for segs %08x %08x %08x %08x %08x, "
		       "BAR5 %u dwords — display layer stays INERT",
		       dcn41_allow_reason_name(ar), gDcn.seg[0], gDcn.seg[1], gDcn.seg[2], gDcn.seg[3],
		       gDcn.seg[4], mmioDwords);
		gDcn.initStatus = 5;
		return 5;
	}

	const int dr = dcn41_dev_init(&gDcn.d, &gDcn, dcn_rreg, dcn_wreg, dcn_udelay, gDcn.seg,
	                              mmioDwords, 0);
	if (dr != DCN41_OK) {
		N48LOG("dcn: dcn41_dev_init failed %d — display layer stays INERT", dr);
		gDcn.initStatus = 6;
		return 6;
	}
	// Flip addresses are confined to the display's own frame-buffer window as the hardware
	// reports it (T1: BASE 0x8000, TOP 0x83fb -> MC 0x80_0000_0000..0x83_ffff_ffff).
	{
		const uint32_t fbBase = amdgpu::RREG32(*gDcn.dev, gDcn.seg[2] + 0x475u);
		const uint32_t fbTop  = amdgpu::RREG32(*gDcn.dev, gDcn.seg[2] + 0x476u);
		if (fbTop > fbBase)
			(void)dcn41_dev_set_scanout_window(&gDcn.d, (uint64_t)fbBase << 24,
			                                   ((uint64_t)fbTop + 1u) << 24);
	}

	gDcn.armed = true;
	gDcn.initStatus = 0;
	N48LOG("dcn: display layer ARMED — segs %08x %08x %08x %08x %08x, BAR5 %u dwords, "
	       "allowlist %u ranges / %u writable dwords (deny by default)",
	       gDcn.seg[0], gDcn.seg[1], gDcn.seg[2], gDcn.seg[3], gDcn.seg[4], mmioDwords,
	       dcn41_allow_range_count(), dcn41_allow_total_dwords());
	// build 0.0.603 (S2a review MUST-FIX): on a NATIVE boot the golden timing copy is taken HERE, at bind - before any native
	// call can write a display register - so `dcnmode 0` (exempt from the native refusal) always restores the boot capture, not whatever
	// the first dcnmode call happened to find. Reads only; a non-native boot takes it lazily in mode(), exactly as before.
	if (!gDcn.goldenValid && amdgpu::native_s1b_state().gate == n48native::kGateOn) {
		const int gOtg = lit_otg();
		if (gOtg >= 0 && dcn41_otg_read_timing(&gDcn.d, (uint32_t)gOtg, &gDcn.golden) == DCN41_OK) {
			gDcn.goldenValid = true;
			N48LOG("dcn: GOLDEN timing capture taken at bind on a native boot (OTG %d)", gOtg);
		}
	}
	// build 0.0.605 (S2d review MUST-FIX): the FULL golden copy - every register a mode trial can write - is taken here on a native boot, read-only, before any native
	// call can write a display register. `dcnmode 0` and the trial's restore both put THIS copy back.
	mt_golden_at_bind();
	return 0;
}

// ---- T3-FLIP ---------------------------------------------------------------------------
//
// GEOMETRY IS READ, NOT ASSUMED. This matters more here than anywhere else: the lit RASTER is
// 2560x1440 (T1) but the PLANE the HUBP fetches is 1920x1080 at pitch 1920, ARGB8888,
// SW_MODE 0 (linear), and the DPP scaler (DSCL_MODE 1) upscales it. A buffer shaped like the raster
// would be read as 1920x1080 at pitch 1920 anyway, because those registers are the HUBP's and we do
// not touch them. So the buffer is sized and strided from HUBP0's OWN viewport, pitch and format
// registers, every one of them re-read on each prepare.
//
// THE RESTORE IS GUARANTEED ON EVERY PATH:
//   1. one exit path - every failure inside flip() goes to the same restore;
//   2. `flipHeld` persists in module state, and dcnstate, dcnvbl AND dcnflip all restore first if it
//      is set, so the very next call of any of the three puts the console back;
//   3. the hold is IOSleep in <= 250 ms slices under a hard cap, so no argument can hold longer;
//   4. an INDEPENDENT INTERRUPT WATCHDOG: the flip enables VUPDATE_NO_LOCK (proven on hardware in
//     ) and counts frames while held; past heldFrameCap the IH handler itself reprograms the
//      console address and says so. That is a real watchdog on the work loop, not a promise;
//   5. `dcnflip 0` restores unconditionally and is idempotent, callable at any time.
// A panic is the one thing none of these survive, and a panic reboots, after which the GOP relights.

/* Absolute address of an OTG-instanced register from its instance-0 offset. OTG stride is 0x80
 * (regOTG0_OTG_CRC_CNTL 0x1b65, regOTG1_OTG_CRC_CNTL 0x1be5, dcn_4_1_0_offset.h). */
uint32_t otg_reg(uint32_t off0, uint32_t otg) { return gDcn.seg[2] + off0 + 0x80u * otg; }
/* HUBP/HUBPREQ stride is 0xdc (regHUBP0_DCSURF_SURFACE_CONFIG 0x5e5, HUBP1 0x6c1). */
uint32_t hubp_reg(uint32_t off0, uint32_t hubp) { return gDcn.seg[2] + off0 + 0xdcu * hubp; }

/* dcn_4_1_0_offset.h, instance 0, BASE_IDX 2 (absolutes for OTG0/HUBP0 in the comments). */
enum : uint32_t {
    kOffCrcCntl      = 0x1b65,   /* 0x5025 */
    kOffCrcWinAX     = 0x1b66,   /* 0x5026 */
    kOffCrcWinAY     = 0x1b67,   /* 0x5027 */
    kOffCrcWinBX     = 0x1b68,   /* 0x5028 */
    kOffCrcWinBY     = 0x1b69,   /* 0x5029 */
    kOffCrcDataRG    = 0x1b6a,   /* 0x502a */
    kOffCrcDataB     = 0x1b6b,   /* 0x502b */
    kOffSurfConfig   = 0x05e5,   /* HUBP0_DCSURF_SURFACE_CONFIG        0x3aa5 */
    kOffTilingConfig = 0x05e7,   /* HUBP0_DCSURF_TILING_CONFIG         0x3aa7 */
    kOffViewportDim  = 0x05eb,   /* HUBP0_DCSURF_PRI_VIEWPORT_DIMENSION 0x3aab */
    kOffSurfPitch    = 0x0607,   /* HUBPREQ0_DCSURF_SURFACE_PITCH      0x3ac7 */
    kOffVmidSettings = 0x0609,   /* HUBPREQ0_VMID_SETTINGS_0 (VMID bits 3:0) - build 0.0.603 */
    kOffViewportStart = 0x05e9,  /* HUBP0_DCSURF_PRI_VIEWPORT_START - build 0.0.603 */
    kOffFlipControl  = 0x0613,   /* HUBPREQ0_DCSURF_FLIP_CONTROL       (SURFACE_FLIP_PENDING bit 8) - build 0.0.603 */
    kOffSurfControl  = 0x0612,   /* HUBPREQ0_DCSURF_SURFACE_CONTROL    (PRIMARY_SURFACE_DCC_EN bit 1) - build 0.0.603 */
};

void crc_enable(uint32_t otg, uint32_t w, uint32_t h) {
    gDcn.tag = "crcset";
    /* optc1_configure_crc order: the four windows, then CONT_EN + SELECT + EN in one update. */
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinAX, otg), (0u & 0x7FFFu) | ((w & 0x7FFFu) << 16));
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinAY, otg), (0u & 0x7FFFu) | ((h & 0x7FFFu) << 16));
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinBX, otg), (0u & 0x7FFFu) | ((w & 0x7FFFu) << 16));
    dcn_wreg(&gDcn, otg_reg(kOffCrcWinBY, otg), (0u & 0x7FFFu) | ((h & 0x7FFFu) << 16));
    uint32_t v = dcn_rreg(&gDcn, otg_reg(kOffCrcCntl, otg));
    v &= ~(0x00000001u | 0x00000010u | 0x00700000u);
    v |= 0x00000010u;                   /* OTG_CRC_CONT_EN  (mask 0x10,  shift 4)  */
    v |= (0u << 20) & 0x00700000u;      /* OTG_CRC0_SELECT = 0                      */
    v |= 0x00000001u;                   /* OTG_CRC_EN       (mask 0x1,   shift 0)  */
    dcn_wreg(&gDcn, otg_reg(kOffCrcCntl, otg), v);
}

void crc_disable(uint32_t otg) {
    gDcn.tag = "crcoff";
    uint32_t v = dcn_rreg(&gDcn, otg_reg(kOffCrcCntl, otg));
    v &= ~(0x00000001u | 0x00000010u);
    dcn_wreg(&gDcn, otg_reg(kOffCrcCntl, otg), v);
}

void crc_read(uint32_t otg, uint32_t *rg, uint32_t *b) {
    if (rg) *rg = dcn_rreg(&gDcn, otg_reg(kOffCrcDataRG, otg));
    if (b)  *b  = dcn_rreg(&gDcn, otg_reg(kOffCrcDataB, otg));
}

/* Wait n frames on the lit OTG by watching its own frame counter; bounded, never an open loop. */
void wait_frames(uint32_t otg, uint32_t n) {
    uint32_t start = 0, cur = 0;
    if (dcn41_otg_get_frame_count(&gDcn.d, otg, &start) != DCN41_OK) { IOSleep(20 * n); return; }
    for (uint32_t i = 0; i < 40u * n; i++) {          /* 40 x 1 ms per frame is ~2.4x a 60 Hz frame */
        IOSleep(1);
        if (dcn41_otg_get_frame_count(&gDcn.d, otg, &cur) != DCN41_OK) return;
        if (dcn41_frame_delta(start, cur) >= n) return;
    }
}

/* Measure the refresh over `ms` by the OTG's own frame counter and a monotonic clock. Returns
 * millihertz; 0 if anything went wrong. This is the witness that a timing change is REAL. */
uint64_t measure_refresh_mhz(uint32_t otg, uint32_t ms, uint32_t *frames_out) {
    uint32_t f0 = 0, f1 = 0;
    if (frames_out) *frames_out = 0;
    if (dcn41_otg_get_frame_count(&gDcn.d, otg, &f0) != DCN41_OK) return 0;
    const uint64_t t0 = now_ns();
    IOSleep(ms);
    if (dcn41_otg_get_frame_count(&gDcn.d, otg, &f1) != DCN41_OK) return 0;
    const uint64_t t1 = now_ns();
    const uint32_t frames = dcn41_frame_delta(f0, f1);
    if (frames_out) *frames_out = frames;
    if (t1 <= t0) return 0;
    return dcn41_refresh_mhz(f0, f1, t1 - t0);
}

/* Program the plane address. Returns the dcn41 status. */
int flip_program(uint64_t mc, const char *tag) {
    gDcn.tag = tag;
    return dcn41_hubp_program_flip(&gDcn.d, gDcn.flipHubp, mc, 0u, false, false);
}

/* THE restore. Unconditional, idempotent, and the only way the console address is put back. */
void flip_restore(const char *why) {
    if (!gDcn.flipPrepared || gDcn.origMc == 0) return;
    const int r = flip_program(gDcn.origMc, "flipback");
    gDcn.flipHeld = false;
    gDcn.heldFrames = 0;
    gDcn.flipsOut++;
    N48LOG("dcn-flip: RESTORE (%s) -> console plane %#llx, status %d",
           why, (unsigned long long)gDcn.origMc, r);
}

void flip_restore_if_held(const char *why) {
    if (gDcn.flipHeld) {
        gDcn.watchdogRestores++;
        flip_restore(why);
    }
}

/* Read the plane geometry off the hardware and build the test buffer. Idempotent. */
uint32_t flip_prepare() {
    if (gDcn.flipPrepared) return 0;
    if (!gDcn.armed) return 1;

    const int otg = lit_otg();
    if (otg < 0) { N48LOG("dcn-flip: no OTG is master-enabled — REFUSING"); return 2; }
    gDcn.flipOtg = (uint32_t)otg;
    gDcn.flipHubp = (uint32_t)otg;      /* T1: OTG0 + HUBP0; HUBP_VTG_SEL ties HUBPn to OTGn */

    uint64_t cur = 0;
    if (dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &cur) != DCN41_OK || cur == 0) {
        N48LOG("dcn-flip: could not read HUBP%u's primary address (got %#llx) — REFUSING",
               gDcn.flipHubp, (unsigned long long)cur);
        return 3;
    }
    gDcn.origMc = cur;

    const uint32_t dim   = dcn_rreg(&gDcn, hubp_reg(kOffViewportDim,  gDcn.flipHubp));
    const uint32_t pitch = dcn_rreg(&gDcn, hubp_reg(kOffSurfPitch,    gDcn.flipHubp));
    const uint32_t cfg   = dcn_rreg(&gDcn, hubp_reg(kOffSurfConfig,   gDcn.flipHubp));
    const uint32_t tile  = dcn_rreg(&gDcn, hubp_reg(kOffTilingConfig, gDcn.flipHubp));
    gDcn.fbWidth   = dim & 0xFFFFu;             /* PRI_VIEWPORT_WIDTH  */
    gDcn.fbHeight  = (dim >> 16) & 0xFFFFu;     /* PRI_VIEWPORT_HEIGHT */
    gDcn.fbPitchPx = (pitch & 0xFFFFu) + 1u;    /* hubp401 writes surface_pitch - 1 */
    gDcn.fbPixFmt  = cfg & 0x7Fu;               /* SURFACE_PIXEL_FORMAT */
    gDcn.fbSwMode  = (tile >> 0) & 0x1Fu;       /* SW_MODE */

    if (gDcn.fbWidth == 0 || gDcn.fbHeight == 0 || gDcn.fbPitchPx < gDcn.fbWidth ||
        gDcn.fbWidth > 8192u || gDcn.fbHeight > 8192u || gDcn.fbPitchPx > 16384u) {
        N48LOG("dcn-flip: implausible geometry %ux%u pitch %u — REFUSING",
               gDcn.fbWidth, gDcn.fbHeight, gDcn.fbPitchPx);
        return 4;
    }
    if (gDcn.fbPixFmt != 8u && gDcn.fbPixFmt != 10u) {
        N48LOG("dcn-flip: SURFACE_PIXEL_FORMAT %u is not a 32-bit ARGB/ABGR format — REFUSING "
               "(the pattern writer only knows 8888)", gDcn.fbPixFmt);
        return 5;
    }

    amdgpu::BringupContext *bc = gDcn.owner ? gDcn.owner->bringupContext() : nullptr;
    if (!bc || !bc->gmc.vram_alloc.is_inited()) { N48LOG("dcn-flip: no VRAM allocator"); return 6; }
    const uint64_t bytes = (uint64_t)gDcn.fbPitchPx * gDcn.fbHeight * 4ull;
    amdgpu::VRAMAllocation a {};
    if (!bc->gmc.vram_alloc.alloc(bytes, 65536, &a)) {
        N48LOG("dcn-flip: could not allocate %llu bytes for the test plane", (unsigned long long)bytes);
        return 7;
    }
    gDcn.testMc = a.gpu_va;
    gDcn.testBytes = bytes;
    // gmc_vram_alloc_init: alloc_base_mc = gmc.vram_start + vram byte offset, and
    // "gpu_va - vram_start = BAR0 offset". Use the allocator's OWN base, and refuse if the two
    // records of where VRAM starts disagree rather than filling the wrong memory.
    if (bc->gmc.vram_start != gDcn.dev->vramMcBase || a.gpu_va < bc->gmc.vram_start) {
        N48LOG("dcn-flip: gmc.vram_start %#llx vs dev.vramMcBase %#llx vs gpu_va %#llx disagree — REFUSING",
               (unsigned long long)bc->gmc.vram_start, (unsigned long long)gDcn.dev->vramMcBase,
               (unsigned long long)a.gpu_va);
        bc->gmc.vram_alloc.free(a);
        return 10;
    }
    gDcn.testVramOff = a.gpu_va - bc->gmc.vram_start;

    /* The buffer must be inside the frame-buffer window the hardware reports, and inside the BAR0
     * aperture we are about to write it through. Both checked before a single byte is written. */
    if (gDcn.testVramOff + bytes > gDcn.dev->bar0Size || !gDcn.dev->bar0) {
        N48LOG("dcn-flip: the allocation (vram+%#llx, %llu bytes) is outside the %zu-byte BAR0 "
               "aperture — REFUSING", (unsigned long long)gDcn.testVramOff,
               (unsigned long long)bytes, gDcn.dev->bar0Size);
        return 8;
    }
    if (gDcn.testMc == gDcn.origMc) { N48LOG("dcn-flip: allocation aliases the console — REFUSING"); return 9; }

    /* The pattern. Linear ARGB8888 (SW_MODE 0 confirmed above), so a row is fbPitchPx dwords and
     * memory order is the screen order: four full-width horizontal bands, top to bottom
     * RED, GREEN, BLUE, WHITE, each a quarter of the plane's height. The scaler stretches the
     * 1920x1080 plane over the 2560x1440 raster, so the user sees four bands filling the screen. */
    static const uint32_t kBand[4] = { 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu };
    volatile uint32_t *px = (volatile uint32_t *)(gDcn.dev->bar0 + gDcn.testVramOff);
    for (uint32_t y = 0; y < gDcn.fbHeight; y++) {
        const uint32_t c = kBand[(y * 4u) / gDcn.fbHeight > 3u ? 3u : (y * 4u) / gDcn.fbHeight];
        volatile uint32_t *row = px + (uint64_t)y * gDcn.fbPitchPx;
        for (uint32_t x = 0; x < gDcn.fbPitchPx; x++) row[x] = c;
    }
    amdgpu::storeFence();

    gDcn.flipPrepared = true;
    N48LOG("dcn-flip: PREPARED — plane read from HUBP%u: %ux%u pitch %u px, SURFACE_PIXEL_FORMAT %u, "
           "SW_MODE %u; console plane %#llx; test plane %#llx (vram+%#llx, %llu bytes) filled with "
       "4 bands R/G/B/W; OTG %u",
       gDcn.flipHubp, gDcn.fbWidth, gDcn.fbHeight, gDcn.fbPitchPx, gDcn.fbPixFmt, gDcn.fbSwMode,
       (unsigned long long)gDcn.origMc, (unsigned long long)gDcn.testMc,
       (unsigned long long)gDcn.testVramOff, (unsigned long long)bytes, gDcn.flipOtg);
    return 0;
}

// ---- the IH path ----------------------------------------------------------------------

// build 0.0.603: defined with the rest of the scanout code at the end of this file. Called with scan.active set; takes the scanout
// lock, acknowledges the interrupt inside it, does the latch / storm bookkeeping. Returns true when it acknowledged (*ar = the status).
static bool scanIrq(const struct dcn41_irq &irq, uint64_t nowNs, int *ar);
void scanEscape(const char *why);     // the public functions below are declared in navi48_dcn.hpp; repeated here because ihEntry precedes them
void scanLogState();

bool ihEntry(uint32_t clientId, uint32_t srcId, uint32_t srcData0) {
	if (clientId != DCN41_IH_CLIENT_DCE) return false;
	if (!gDcn.armed) {
		// Still ours: report it as handled so the caller does not double-count, but we
		// cannot acknowledge it without the register layer.
		gDcn.dceEntries++;
		gDcn.dceUnclassified++;
		return true;
	}

	const struct dcn41_irq irq = dcn41_ih_to_irq(clientId, srcId, srcData0);
	const uint64_t t = now_ns();
	gDcn.dceEntries++;
	if (gDcn.firstEntryNs == 0) gDcn.firstEntryNs = t;
	gDcn.lastEntryNs = t;

	if (irq.kind == DCN41_IRQ_NONE) {
		gDcn.dceUnclassified++;
		if (gDcn.logBudget > 0) {
			gDcn.logBudget--;
			N48LOG("dcn-irq: DCE entry src %u data %08x did not classify", srcId, srcData0);
		}
		return true;
	}

	if (irq.kind < DCN41_IRQ_KIND_COUNT) gDcn.dceByKind[irq.kind]++;
	if (irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK && irq.inst < DCN41_NUM_PIPES)
		gDcn.dceVupdate[irq.inst]++;
	if (irq.kind == DCN41_IRQ_VSTARTUP && irq.inst < DCN41_NUM_PIPES)
		gDcn.dceVstartup[irq.inst]++;

	// T3-FLIP's INDEPENDENT WATCHDOG. While our test plane is on screen the flip enables
	// VUPDATE_NO_LOCK, so this runs once per frame on the work loop: past the cap the handler
	// reprograms the console address itself, without any userspace call. This is the answer to
	// "a wedged step still restores the original plane address".
	if (gDcn.flipHeld && irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK) {
		gDcn.heldFrames++;
		if (gDcn.heldFrames > gDcn.heldFrameCap) {
			gDcn.watchdogRestores++;
			N48LOG("dcn-flip: WATCHDOG — held %u frames, cap %u; restoring the console plane from "
			       "the interrupt handler", gDcn.heldFrames, gDcn.heldFrameCap);
			flip_restore("interrupt watchdog");
		}
	}

	// Acknowledge BEFORE anything else, as amdgpu_dm_irq_handler does (it calls
	// dc_interrupt_ack first for every DCN source). The ack is a register write and goes
	// through the allowlist like every other.
	// build 0.0.603: while a native client holds the plane, the ack AND the scanout bookkeeping run under the scanout lock (the
	// ack is a read-modify-write of OTG_GLOBAL_SYNC_STATUS, which Acquire / restore also modify); otherwise exactly the old two lines.
	int ar = 0;
	if (__atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u || !scanIrq(irq, t, &ar)) {
		gDcn.tag = "irqack";
		ar = dcn41_irq_ack(&gDcn.d, irq);
	}
	if (ar != DCN41_OK) gDcn.ackFailures++;

	if (gDcn.logBudget > 0) {
		gDcn.logBudget--;
		N48LOG("dcn-irq: kind %u inst %u (src %u data %08x) ack %d — entry %llu at %llu ns",
		       irq.kind, irq.inst, srcId, srcData0, ar,
		       (unsigned long long)gDcn.dceEntries, (unsigned long long)t);
	}

	// Storm guard: disable what we enabled rather than let it spin the work loop.
	// build 0.0.603: not while a native client holds the plane - that path has the RATE guard (scanIrq), because the cumulative
	// cap trips after ~28 minutes at 120 Hz.
	if (gDcn.srcEnabled && !gDcn.stormTripped && __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) == 0u &&
	    gDcn.dceEntries - gDcn.entriesAtEnable > kDceStormCap) {
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "stormoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
		gDcn.stormTripped = true;
		N48LOG("dcn-irq: STORM GUARD — %llu entries since the enable exceeded the cap %llu; "
		       "kind %u inst %u DISABLED by the handler",
		       (unsigned long long)(gDcn.dceEntries - gDcn.entriesAtEnable),
		       (unsigned long long)kDceStormCap, off.kind, off.inst);
	}
	return true;
}

// ---- the verbs ------------------------------------------------------------------------

// `accel dcnstate` — read-only.
//
// arg 1 additionally runs the allowlist's ON-HARDWARE self-test. It uses a SEPARATE
// allowlist state so the live counters stay clean, and it is genuinely harmless: a refused
// write touches nothing, and the one legal address it exercises is classified, not written.
uint32_t state(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnstate");

	const int otg = lit_otg();
	uint32_t frameCount = 0, gss = 0, pos = 0;
	if (otg >= 0) {
		(void)dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &frameCount);
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, (uint32_t)otg, &st) == DCN41_OK) gss = st.raw;
		struct dcn41_otg_position p {};
		if (dcn41_otg_get_position(&gDcn.d, (uint32_t)otg, &p) == DCN41_OK)
			pos = (p.vertical_count << 16) | (p.horizontal_count & 0xFFFFu);
	}

	N48LOG("dcn-state: armed %d (init status %u) lit OTG %d, frame count %u, "
	       "GLOBAL_SYNC_STATUS %#010x, position %#010x",
	       gDcn.armed ? 1 : 0, gDcn.initStatus, otg, frameCount, gss, pos);
	N48LOG("dcn-state: allowlist %u ranges, writes %llu (allowed %llu refused %llu), reads %llu; "
	       "refusal reasons unarmed %llu window %llu indirect %llu block %llu range %llu; "
	       "last refused %#010x (%s)",
	       dcn41_allow_range_count(), (unsigned long long)gDcn.allow.writes_seen,
	       (unsigned long long)gDcn.allow.allowed, (unsigned long long)gDcn.allow.refused,
	       (unsigned long long)gDcn.allow.reads_seen,
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_UNARMED],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_WINDOW],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_INDIRECT],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_BLOCK],
	       (unsigned long long)gDcn.allow.by_reason[DCN41_ALLOW_RANGE],
	       gDcn.allow.last_refused_abs,
	       dcn41_allow_reason_name((int)gDcn.allow.last_refused_reason));
	for (uint32_t i = 0; i < gDcn.allow.n_allow_samples; i++)
		N48LOG("dcn-state:   allowed sample %u: seq %u %#010x = %#010x tag %s",
		       i, gDcn.allow.allow[i].seq, gDcn.allow.allow[i].abs_dword,
		       gDcn.allow.allow[i].value, gDcn.allow.allow[i].tag);
	for (uint32_t i = 0; i < gDcn.allow.n_refuse_samples; i++)
		N48LOG("dcn-state:   REFUSED sample %u: seq %u %#010x = %#010x tag %s reason %s",
		       i, gDcn.allow.refuse[i].seq, gDcn.allow.refuse[i].abs_dword,
		       gDcn.allow.refuse[i].value, gDcn.allow.refuse[i].tag,
		       dcn41_allow_reason_name((int)gDcn.allow.refuse[i].reason));

	const uint64_t spanNs = (gDcn.lastEntryNs > gDcn.firstEntryNs)
	                      ? gDcn.lastEntryNs - gDcn.firstEntryNs : 0;
	N48LOG("dcn-state: DCE IH entries %llu (unclassified %llu, ack failures %llu) over %llu ns; "
	       "VSTARTUP %llu VUPDATE_NO_LOCK %llu PFLIP %llu HPD %llu HPD_RX %llu VLINE0 %llu OUTBOX %llu",
	       (unsigned long long)gDcn.dceEntries, (unsigned long long)gDcn.dceUnclassified,
	       (unsigned long long)gDcn.ackFailures, (unsigned long long)spanNs,
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_VSTARTUP],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_VUPDATE_NO_LOCK],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_PFLIP],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_HPD],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_HPD_RX],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_VLINE0],
	       (unsigned long long)gDcn.dceByKind[DCN41_IRQ_DMCUB_OUTBOX]);
	N48LOG("dcn-state: per-OTG VUPDATE %llu %llu %llu %llu; VSTARTUP %llu %llu %llu %llu; "
	       "source %s (kind %u inst %u), storm guard %s",
	       (unsigned long long)gDcn.dceVupdate[0], (unsigned long long)gDcn.dceVupdate[1],
	       (unsigned long long)gDcn.dceVupdate[2], (unsigned long long)gDcn.dceVupdate[3],
	       (unsigned long long)gDcn.dceVstartup[0], (unsigned long long)gDcn.dceVstartup[1],
	       (unsigned long long)gDcn.dceVstartup[2], (unsigned long long)gDcn.dceVstartup[3],
	       gDcn.srcEnabled ? "ENABLED" : "off", gDcn.srcKind, gDcn.srcInst,
	       gDcn.stormTripped ? "TRIPPED" : "clear");

	scanLogState();   // build 0.0.603: one line, and only when a native client has ever taken the plane

	if (arg == 1) {
		// The allowlist, exercised against this card's own addresses, in the kernel.
		struct dcn41_allow_state probe {};
		const int pr = dcn41_allow_init(&probe, gDcn.seg, (uint32_t)(gDcn.dev ? gDcn.dev->rmmioSize / 4 : 0));
		const uint32_t legal   = gDcn.seg[2] + 0x1b88u;   // OTG0_OTG_GLOBAL_SYNC_STATUS
		const uint32_t smu     = 0x00016282u;             // regMP1_SMN_C2PMSG_66, the SMU mailbox
		const uint32_t indirect = 0x0000000eu;            // BIF_BX1_PCIE_INDEX2
		const uint32_t stray   = 0x0000a000u;             // GC BASE_IDX 1, one past the top range
		const bool okLegal = dcn41_allow_write(&probe, legal, 0u, "selftest");
		const bool okSmu   = dcn41_allow_write(&probe, smu, 0u, "selftest");
		const bool okInd   = dcn41_allow_write(&probe, indirect, 0u, "selftest");
		const bool okStray = dcn41_allow_write(&probe, stray, 0u, "selftest");
		N48LOG("dcn-selftest: init %s; legal %#010x -> %s; SMU %#010x -> %s; indirect %#010x -> %s; "
		       "stray %#010x -> %s; allowed %llu refused %llu (indirect %llu block %llu range %llu)",
		       dcn41_allow_reason_name(pr), legal, okLegal ? "ALLOWED" : "refused",
		       smu, okSmu ? "*** ALLOWED — BUG ***" : "refused",
		       indirect, okInd ? "*** ALLOWED — BUG ***" : "refused",
		       stray, okStray ? "*** ALLOWED — BUG ***" : "refused",
		       (unsigned long long)probe.allowed, (unsigned long long)probe.refused,
		       (unsigned long long)probe.by_reason[DCN41_ALLOW_INDIRECT],
		       (unsigned long long)probe.by_reason[DCN41_ALLOW_BLOCK],
		       (unsigned long long)probe.by_reason[DCN41_ALLOW_RANGE]);
		// NOTE: the legal address was classified, not written — dcn41_allow_write only decides.
		if (out && outCount > 12)
			out[12] = (uint64_t)((okLegal ? 1u : 0u) | (okSmu ? 2u : 0u) | (okInd ? 4u : 0u) |
			                     (okStray ? 8u : 0u));
	}

	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = gDcn.initStatus;
		if (outCount > 2)  out[2]  = (uint64_t)(int64_t)otg;
		if (outCount > 3)  out[3]  = frameCount;
		if (outCount > 4)  out[4]  = gss;
		if (outCount > 5)  out[5]  = gDcn.dceEntries;
		if (outCount > 6)  out[6]  = gDcn.dceByKind[DCN41_IRQ_VSTARTUP];
		if (outCount > 7)  out[7]  = gDcn.dceByKind[DCN41_IRQ_VUPDATE_NO_LOCK];
		if (outCount > 8)  out[8]  = spanNs;
		if (outCount > 9)  out[9]  = gDcn.allow.allowed;
		if (outCount > 10) out[10] = gDcn.allow.refused;
		if (outCount > 11) out[11] = (uint64_t)gDcn.srcEnabled << 8 |
		                             (uint64_t)gDcn.srcKind << 4 | (uint64_t)gDcn.srcInst;
	}
	return gDcn.armed ? 0u : (gDcn.initStatus == 0xFFFFFFFFu ? 1u : gDcn.initStatus);
}

// `accel dcnvbl <n>` — T2-VBL.
//   0  disable whatever we enabled (idempotent, safe to call when nothing is enabled)
//   1  enable VUPDATE_NO_LOCK on the lit OTG
//   2  enable VSTARTUP on the lit OTG
// One source at a time, by construction: an enable disables the previous one first.
uint32_t vbl(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnvbl");
	if (!gDcn.armed) {
		N48LOG("dcn-vbl: display layer is not armed (status %u) — REFUSING", gDcn.initStatus);
		if (out && outCount > 0) out[0] = 0;
		return gDcn.initStatus;
	}
	if (arg > 2) {
		N48LOG("dcn-vbl: unknown argument %llu (0 off, 1 VUPDATE_NO_LOCK, 2 VSTARTUP) — REFUSING",
		       (unsigned long long)arg);
		return 2;
	}

	// Always disable what is currently on first: one source at a time is the whole point of
	// T2, and it also makes `dcnvbl 0` the unconditional escape hatch.
	if (gDcn.srcEnabled) {
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "irqoff";
		const int r = dcn41_irq_set(&gDcn.d, off, false);
		N48LOG("dcn-vbl: disabled kind %u inst %u -> %d", off.kind, off.inst, r);
		gDcn.srcEnabled = false;
	}

	const int otg = lit_otg();
	uint32_t gssBefore = 0, gssAfter = 0, frameCount = 0;
	int rc = 0;
	if (otg >= 0) {
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, (uint32_t)otg, &st) == DCN41_OK) gssBefore = st.raw;
		(void)dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &frameCount);
	}

	if (arg == 0) {
		gDcn.stormTripped = false;
		N48LOG("dcn-vbl: all DCN interrupt sources are off (lit OTG %d, GLOBAL_SYNC_STATUS %#010x, "
		       "frame count %u); DCE entries so far %llu",
		       otg, gssBefore, frameCount, (unsigned long long)gDcn.dceEntries);
	} else {
		if (otg < 0) {
			N48LOG("dcn-vbl: no OTG is master-enabled — REFUSING to enable a source with no lit pipe");
			return 3;
		}
		struct dcn41_irq on {
			(uint8_t)(arg == 1 ? DCN41_IRQ_VUPDATE_NO_LOCK : DCN41_IRQ_VSTARTUP),
			(uint8_t)otg
		};
		// Reset the measurement window so a rate can be computed from this enable alone.
		gDcn.firstEntryNs = 0;
		gDcn.lastEntryNs = 0;
		gDcn.entriesAtEnable = gDcn.dceEntries;
		gDcn.logBudget = kDceLogBudget;
		gDcn.tag = "irqset";
		rc = dcn41_irq_set(&gDcn.d, on, true);
		struct dcn41_otg_irq_status st {};
		if (dcn41_otg_irq_status(&gDcn.d, (uint32_t)otg, &st) == DCN41_OK) gssAfter = st.raw;
		if (rc == DCN41_OK) {
			gDcn.srcEnabled = true;
			gDcn.srcKind = on.kind;
			gDcn.srcInst = on.inst;
			gDcn.enabledAtNs = now_ns();
			gDcn.stormTripped = false;
		}
		N48LOG("dcn-vbl: ENABLE kind %u (%s) on OTG %d -> %d; GLOBAL_SYNC_STATUS %#010x -> %#010x "
		       "(VSTARTUP_INT_EN %u, VUPDATE_NO_LOCK_INT_EN %u); frame count %u; "
		       "entries at enable %llu",
		       on.kind, arg == 1 ? "VUPDATE_NO_LOCK" : "VSTARTUP", otg, rc, gssBefore, gssAfter,
		       st.vstartup_int_en, st.vupdate_no_lock_int_en, frameCount,
		       (unsigned long long)gDcn.entriesAtEnable);
		// The enable is a read-modify-write of ONE register; if the allowlist refused it, the
		// read-back above shows the enable bit still clear and the refusal is already logged
		// and counted. That distinction is exactly what T2's falsifier needs.
	}

	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = (uint64_t)(int64_t)rc;
		if (outCount > 2)  out[2]  = (uint64_t)(int64_t)otg;
		if (outCount > 3)  out[3]  = frameCount;
		if (outCount > 4)  out[4]  = gssBefore;
		if (outCount > 5)  out[5]  = gssAfter;
		if (outCount > 6)  out[6]  = gDcn.dceEntries;
		if (outCount > 7)  out[7]  = gDcn.entriesAtEnable;
		if (outCount > 8)  out[8]  = gDcn.allow.allowed;
		if (outCount > 9)  out[9]  = gDcn.allow.refused;
		if (outCount > 10) out[10] = gDcn.allow.last_refused_abs;
		if (outCount > 11) out[11] = (uint64_t)gDcn.srcEnabled << 8 |
		                             (uint64_t)gDcn.srcKind << 4 | (uint64_t)gDcn.srcInst;
		if (outCount > 12) out[12] = gDcn.srcEnabled ? gDcn.enabledAtNs : 0;
	}
	return (rc == DCN41_OK) ? 0u : (uint32_t)(0x80u | (uint32_t)(-rc & 0x7fu));
}


// `accel dcnflip <n>` — T3-FLIP.
//   0        restore the console plane unconditionally and report. Idempotent, allocates nothing,
//            safe to run at any time from any state. THE EMERGENCY COMMAND.
//   1        phase (a): flip in, one frame, machine witnesses, flip back immediately.
//   2..30     phase (b): the same, holding the test pattern for that many SECONDS so a person can
//            see it, then the same unconditional restore.
//
// Everything between the flip in and the restore runs INSIDE THIS KERNEL CALL, so a dropped ssh
// session cannot strand the display: the caller dying does not stop a kernel call that has already
// begun. On top of that the interrupt watchdog (see ihEntry) restores from the work loop if the hold
// overruns its frame cap, and `flipHeld` makes the next dcnstate/dcnvbl/dcnflip restore as well.
uint32_t flip(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnflip");

	uint64_t addrBack = 0;
	uint32_t st = 0;
	uint32_t c0rg = 0, c0b = 0, c1rg = 0, c1b = 0, t0rg = 0, t0b = 0, t1rg = 0, t1b = 0;
	uint32_t r0rg = 0, r0b = 0;
	uint32_t aw = 0, ah = 0, witness = 0, holdMs = 0, framesHeld = 0;
	bool pendNow = false, pendAfter = false, otgEnabled = false;
	uint64_t earliest = 0;
	clock_sec_t s0 = 0, s1 = 0; clock_nsec_t n0 = 0, n1 = 0;

	if (!gDcn.armed) {
		N48LOG("dcn-flip: display layer is not armed (status %u) — REFUSING", gDcn.initStatus);
		st = gDcn.initStatus ? gDcn.initStatus : 1u;
		goto report;
	}

	if (arg == 0) {
		// Restore only. If we never prepared, there is nothing of ours on screen; still read the
		// plane back so the caller can verify it.
		if (gDcn.flipPrepared) flip_restore("explicit dcnflip 0");
		// build 0.0.603: on a native boot the plane may belong to the N48N scanout path (which never sets flipPrepared): the same
		// shared restore the client's Release / close / death and the watchdog run. A no-op unless a native client took the plane.
		scanEscape("explicit dcnflip 0");
		(void)dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &addrBack);
		N48LOG("dcn-flip: dcnflip 0 — plane address now %#llx (console was %#llx); held %d, "
		       "flips in %llu out %llu, watchdog restores %llu",
		       (unsigned long long)addrBack, (unsigned long long)gDcn.origMc,
		       gDcn.flipHeld ? 1 : 0, (unsigned long long)gDcn.flipsIn,
		       (unsigned long long)gDcn.flipsOut, (unsigned long long)gDcn.watchdogRestores);
		goto report;
	}
	if (arg > 30u) {
		N48LOG("dcn-flip: hold %llu is above the 30 s cap — REFUSING", (unsigned long long)arg);
		st = 0x20u;
		goto report;
	}
	holdMs = (arg == 1u) ? 0u : (uint32_t)arg * 1000u;

	st = flip_prepare();
	if (st) goto report;

	// The CRC window is the OTG's own active raster (2560x1440 on this machine) — the CRC is taken
	// at the timing generator, AFTER the DPP scaler, so it is in raster space, not plane space.
	(void)dcn41_otg_get_active_size(&gDcn.d, gDcn.flipOtg, &otgEnabled, &aw, &ah);
	if (!otgEnabled || aw == 0 || ah == 0) {
		N48LOG("dcn-flip: OTG %u reports no active size — REFUSING", gDcn.flipOtg);
		st = 0x21u;
		goto report;
	}
	crc_enable(gDcn.flipOtg, aw, ah);
	wait_frames(gDcn.flipOtg, 3);
	crc_read(gDcn.flipOtg, &c0rg, &c0b);
	wait_frames(gDcn.flipOtg, 2);
	crc_read(gDcn.flipOtg, &c1rg, &c1b);

	// The completion witness AND the watchdog: VUPDATE_NO_LOCK, proven on hardware in.
	gDcn.heldFrames = 0;
	gDcn.heldFrameCap = (holdMs + 4000u) / 16u;
	{
		struct dcn41_irq on { (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK, (uint8_t)gDcn.flipOtg };
		gDcn.tag = "irqset";
		(void)dcn41_irq_set(&gDcn.d, on, true);
		gDcn.srcEnabled = true; gDcn.srcKind = on.kind; gDcn.srcInst = on.inst;
	}

	// ---- THE FLIP. flipHeld is set BEFORE the write, so every restore path already knows. ----
	gDcn.flipHeld = true;
	clock_get_calendar_nanotime(&s0, &n0);
	st = (uint32_t)(int32_t)(-flip_program(gDcn.testMc, "flipin"));
	gDcn.flipsIn++;
	(void)dcn41_hubp_is_flip_pending(&gDcn.d, gDcn.flipHubp, &pendNow, &earliest);
	N48LOG("dcn-flip: FLIP IN to %#llx at wall %llu.%09u — status %u, flip pending now %d, "
	       "hold %u ms, watchdog cap %u frames",
	       (unsigned long long)gDcn.testMc, (unsigned long long)s0, (unsigned)n0, st,
	       pendNow ? 1 : 0, holdMs, gDcn.heldFrameCap);

	wait_frames(gDcn.flipOtg, 2);
	(void)dcn41_hubp_is_flip_pending(&gDcn.d, gDcn.flipHubp, &pendAfter, &earliest);
	crc_read(gDcn.flipOtg, &t0rg, &t0b);
	wait_frames(gDcn.flipOtg, 2);
	crc_read(gDcn.flipOtg, &t1rg, &t1b);

	// ---- the hold, in slices, abortable by the watchdog ----
	for (uint32_t done = 0; done < holdMs && gDcn.flipHeld; done += 250u) IOSleep(250);

	// ---- THE RESTORE. Unconditional. ----
	//: read the watchdog counter BEFORE the restore zeroes it, or the log reports 0 frames
	// after a fifteen-second hold and reads as if the watchdog never counted.
	framesHeld = gDcn.heldFrames;
	flip_restore(arg == 1u ? "phase (a), one frame" : "end of the visible hold");
	clock_get_calendar_nanotime(&s1, &n1);
	wait_frames(gDcn.flipOtg, 3);
	crc_read(gDcn.flipOtg, &r0rg, &r0b);
	(void)dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &addrBack);

	{
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "irqoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
	}
	crc_disable(gDcn.flipOtg);

	witness = (pendNow ? 1u : 0u) | (pendAfter ? 0u : 2u)
	        | ((earliest == gDcn.testMc) ? 4u : 0u)
	        | ((addrBack == gDcn.origMc) ? 8u : 0u)
	        | (((c0rg != t0rg) || (c0b != t0b)) ? 16u : 0u)
	        | (((r0rg == c0rg) && (r0b == c0b)) ? 32u : 0u)
	        | ((c0rg == c1rg && c0b == c1b) ? 64u : 0u);
	N48LOG("dcn-flip: VISIBLE WINDOW wall %llu.%09u -> %llu.%09u (%u ms requested); held frames %u",
	       (unsigned long long)s0, (unsigned)n0, (unsigned long long)s1, (unsigned)n1, holdMs,
	       framesHeld);
	N48LOG("dcn-flip: CRC console %08x/%08x and %08x/%08x; test %08x/%08x and %08x/%08x; "
	       "restored %08x/%08x", c0rg, c0b, c1rg, c1b, t0rg, t0b, t1rg, t1b, r0rg, r0b);
	N48LOG("dcn-flip: WITNESS %#x — pending-now %d, pending-cleared %d, earliest-inuse==test %d, "
	       "address-restored %d, CRC-changed %d, CRC-returned %d, console-CRC-stable %d; "
	       "plane now %#llx (console %#llx); allowlist allowed %llu refused %llu",
	       witness, (witness & 1) ? 1 : 0, (witness & 2) ? 1 : 0, (witness & 4) ? 1 : 0,
	       (witness & 8) ? 1 : 0, (witness & 16) ? 1 : 0, (witness & 32) ? 1 : 0,
	       (witness & 64) ? 1 : 0, (unsigned long long)addrBack, (unsigned long long)gDcn.origMc,
	       (unsigned long long)gDcn.allow.allowed, (unsigned long long)gDcn.allow.refused);
	if (addrBack != gDcn.origMc) {
		N48LOG("dcn-flip: *** THE PLANE DID NOT RESTORE *** reprogramming once more");
		flip_restore("restore verification failed");
		(void)dcn41_hubp_read_primary_addr(&gDcn.d, gDcn.flipHubp, &addrBack);
		N48LOG("dcn-flip: after the second restore the plane reads %#llx", (unsigned long long)addrBack);
	}

report:
	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = st;
		if (outCount > 2)  out[2]  = (uint64_t)gDcn.flipOtg | ((uint64_t)gDcn.flipHubp << 8);
		if (outCount > 3)  out[3]  = gDcn.origMc;
		if (outCount > 4)  out[4]  = gDcn.testMc;
		if (outCount > 5)  out[5]  = witness;
		if (outCount > 6)  out[6]  = (uint64_t)c0rg | ((uint64_t)c0b << 32);
		if (outCount > 7)  out[7]  = (uint64_t)t0rg | ((uint64_t)t0b << 32);
		if (outCount > 8)  out[8]  = (uint64_t)r0rg | ((uint64_t)r0b << 32);
		if (outCount > 9)  out[9]  = gDcn.allow.allowed;
		if (outCount > 10) out[10] = gDcn.allow.refused;
		if (outCount > 11) out[11] = (uint64_t)gDcn.fbWidth | ((uint64_t)gDcn.fbHeight << 16)
		                           | ((uint64_t)gDcn.fbPitchPx << 32) | ((uint64_t)gDcn.fbPixFmt << 48);
		if (outCount > 12) out[12] = addrBack;
	}
	return st;
}


// ---- the same-mode re-timing: the first mode-set -----------------------------------------------
//
// `accel dcnmode <n>`:
//   0       READ-ONLY report, plus the emergency: if the live timing has drifted from the capture
//           taken before we ever wrote anything this boot, write that capture back and say so.
//   1       capture, write the identical values back, re-capture, require every dword to match.
//   2..30   the same with that many seconds of dwell between the write and the verification, so a
//           person can watch for a flicker and so the frame counter can be sampled across a window.
//
// WHY THIS IS THE SAFEST POSSIBLE FIRST MODE-SET. Nothing is computed. The values written are the
// values read one instruction earlier, so the raster cannot change: the failure mode "we computed a
// timing the monitor cannot show" does not exist here. What it does exercise is everything else a
// real mode-set needs - the register set, Linux's write order, the double buffering and the latch -
// on a live, lit, already-link-trained DisplayPort output. src/dcn41/dcn41_otg_timing.c holds the
// register table; its host tests prove that writing a capture back leaves a register model
// byte-identical, that each write carries the value read from that address, that the order is
// Linux's optc1_program_timing order, and that the write set never touches OTG_CONTROL, the update
// lock, the interrupt enables, the CRC or the flip registers.
//
// NOT DONE HERE, deliberately: the OTG update lock is NOT taken. Linux takes it when values change,
// to make them latch together; identical values have nothing to tear, and a lock left set would
// freeze the pipe. Taking it is a separate, later, opt-in step.
uint32_t mode(uint64_t arg, uint64_t *out, unsigned outCount) {
	if (gDcn.initStatus == 0xFFFFFFFFu && gDcn.owner) (void)bind(gDcn.owner);
	flip_restore_if_held("entry to dcnmode");

	struct dcn41_otg_timing before {}, after {};
	struct dcn41_timing_decoded db {}, da {};
	struct dcn41_otg_timing toWrite {};
	uint32_t st = 0, diffs = 0xFFFFFFFFu, fw = 0, fr = 0, dwellMs = 0;
	uint32_t f0 = 0, f1 = 0, stalls = 0, frames0 = 0, frames1 = 0, frames2 = 0;
	uint64_t rate0 = 0, rate1 = 0, rate2 = 0;
	bool realChange = false;
	int otg = -1, rcW = 0;

	if (!gDcn.armed) {
		N48LOG("dcn-mode: display layer is not armed (status %u) — REFUSING", gDcn.initStatus);
		st = gDcn.initStatus ? gDcn.initStatus : 1u;
		goto report;
	}
	// 0 read-only; 1..30 the same-mode re-timing with (arg) seconds of dwell; 101..130 the REAL
	// change (v_total + kVTotalDeltaLines) with (arg - 100) seconds of dwell. Anything else refused.
	realChange = (arg >= 101u && arg <= 130u);
	if (arg > 30u && !realChange) {
		N48LOG("dcn-mode: argument %llu is not 0, 1..30 or 101..130 — REFUSING",
		       (unsigned long long)arg);
		st = 0x20u;
		goto report;
	}
	otg = lit_otg();
	if (otg < 0) {
		N48LOG("dcn-mode: no OTG is master-enabled — REFUSING");
		st = 2u;
		goto report;
	}
	dwellMs = realChange ? (uint32_t)(arg - 100u) * 1000u
	                     : ((arg <= 1u) ? 0u : (uint32_t)arg * 1000u);

	if (dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &before) != DCN41_OK) {
		N48LOG("dcn-mode: could not read OTG %d's timing — REFUSING", otg);
		st = 3u;
		goto report;
	}
	dcn41_otg_timing_decode(&before, &db);

	// The golden capture: the timing as it was BEFORE this kext ever wrote a timing register this
	// boot. Taken once, on the first dcnmode call of any kind, and never overwritten.
	if (!gDcn.goldenValid) {
		gDcn.golden = before;
		gDcn.goldenValid = true;
		N48LOG("dcn-mode: GOLDEN capture taken (OTG %d, before any timing write this boot)", otg);
	}

	N48LOG("dcn-mode: OTG %d timing — %ux%u active, h_total %u v_total %u, h sync %u v sync %u, "
	       "blank h %u..%u v %u..%u, vstartup %u vupdate %u/%u vready %u, master_en %u, lock held %u, "
	       "frame count %u",
	       otg, db.h_active, db.v_active, db.h_total, db.v_total, db.h_sync_width, db.v_sync_width,
	       db.h_blank_start, db.h_blank_end, db.v_blank_start, db.v_blank_end, db.vstartup,
	       db.vupdate_offset, db.vupdate_width, db.vready_offset, db.master_en, db.update_lock_held,
	       before.frame_count);
	for (uint32_t i = 0; i < DCN41_TIMING_W_COUNT; i++)
		N48LOG("dcn-mode:   W %-24s %#010x = %#010x", dcn41_timing_w_name(i),
		       gDcn.seg[2] + dcn41_timing_w_offset(i) + 0x80u * (uint32_t)otg, before.w[i]);
	for (uint32_t i = 0; i < DCN41_TIMING_R_COUNT; i++)
		N48LOG("dcn-mode:   R %-24s %#010x = %#010x", dcn41_timing_r_name(i),
		       gDcn.seg[2] + dcn41_timing_r_offset(i) + 0x80u * (uint32_t)otg, before.r[i]);

	if (arg == 0u) {
		// build 0.0.605: on a native boot the FULL golden set (every register a mode trial writes, taken at bind) is restored first - it also ends a running trial, which
		// restores and answers ABORT. Then the timing capture is re-read, so the drift check below judges the machine as it is now. A non-native boot skips all of this.
		if (mt_emergency_restore("dcnmode 0")) {
			(void)dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &before);
			dcn41_otg_timing_decode(&before, &db);
		}
		// Read-only, plus the emergency restore if the live timing has drifted from the golden one.
		uint32_t gw = 0, gr = 0;
		const uint32_t drift = dcn41_otg_timing_diff(&gDcn.golden, &before, &gw, &gr);
		if (drift != 0 && gDcn.goldenValid && gDcn.golden.otg == (uint32_t)otg) {
			N48LOG("dcn-mode: *** the live timing differs from the golden capture in %u dword(s) "
			       "(first writable %s, first read-only %s) — RESTORING the golden capture ***",
			       drift, dcn41_timing_w_name(gw), dcn41_timing_r_name(gr));
			gDcn.tag = "modegold";
			rcW = dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &gDcn.golden);
			gDcn.goldenRestores++;
			N48LOG("dcn-mode: golden restore status %d", rcW);
		} else {
			N48LOG("dcn-mode: dcnmode 0 — read-only; the live timing matches the golden capture "
			       "(%u dwords differ)", drift);
		}
		diffs = drift;
		goto report;
	}

	// ---- THE WRITE ----
	// Either the identical set (the proven same-mode re-timing) or, for arg 101..130, the same set
	// with ONE field changed: OTG_V_TOTAL, plus kVTotalDeltaLines lines. See the header comment.
	f0 = before.frame_count;
	toWrite = before;
	if (realChange) {
		const int adj = dcn41_otg_timing_adjust_v_total(&toWrite, kVTotalDeltaLines);
		if (adj != DCN41_OK) {
			N48LOG("dcn-mode: the v_total adjustment (%d lines) was REFUSED by the guard (%d) — "
			       "nothing written", kVTotalDeltaLines, adj);
			st = 0x50u;
			goto report;
		}
		// Measure the refresh we are about to change, so the claim is a comparison and not an assertion.
		rate0 = measure_refresh_mhz((uint32_t)otg, 2000, &frames0);
		N48LOG("dcn-mode: BEFORE the change — measured %llu mHz over %u frames in 2 s; "
		       "OTG_V_TOTAL %#010x -> %#010x (v_total %u -> %u)",
		       (unsigned long long)rate0, frames0, before.w[DCN41_TW_V_TOTAL],
		       toWrite.w[DCN41_TW_V_TOTAL], (before.w[DCN41_TW_V_TOTAL] & 0x7FFFu) + 1u,
		       (toWrite.w[DCN41_TW_V_TOTAL] & 0x7FFFu) + 1u);
	}
	gDcn.tag = realChange ? "modereal" : "modeset";
	rcW = dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &toWrite);
	gDcn.retimings++;
	N48LOG("dcn-mode: WROTE %s timing set to OTG %d (%u registers) -> status %d; dwell %u ms",
	       realChange ? "a CHANGED" : "the identical", otg, DCN41_TIMING_W_COUNT, rcW, dwellMs);
	if (rcW != DCN41_OK) {
		st = 0x30u;
		// fall through to the unconditional restore below
	}

	wait_frames((uint32_t)otg, 2);

	// With the change in, measure again: this is the whole proof that the hardware really changed.
	if (realChange) {
		rate1 = measure_refresh_mhz((uint32_t)otg, 2000, &frames1);
		N48LOG("dcn-mode: WITH the change — measured %llu mHz over %u frames in 2 s (was %llu mHz)",
		       (unsigned long long)rate1, frames1, (unsigned long long)rate0);
	}

	// ---- the dwell, with a frame-counter STALL WATCHDOG ----
	// The values are identical, so there is no bad state to restore from; the real hazard is the OTG
	// stopping. That is what this watches for, and its answer is to write the capture back at once
	// and say so, rather than sit through the rest of the dwell.
	if (dwellMs) {
		uint32_t last = 0;
		(void)dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &last);
		for (uint32_t done = 0; done < dwellMs; done += 250u) {
			IOSleep(250);
			uint32_t now = 0;
			if (dcn41_otg_get_frame_count(&gDcn.d, (uint32_t)otg, &now) != DCN41_OK) break;
			if (dcn41_frame_delta(last, now) == 0) {
				stalls++;
				gDcn.retimingStalls++;
				N48LOG("dcn-mode: *** STALL WATCHDOG — the frame counter did not advance in 250 ms "
				       "(stuck at %u) — writing the capture back NOW ***", now);
				gDcn.tag = "modestall";
				(void)dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &before);   /* the ORIGINAL */
				break;
			}
			last = now;
		}
	}

	// ---- THE UNCONDITIONAL RESTORE: write the saved capture back on every path ----
	gDcn.tag = "moderestore";
	(void)dcn41_otg_write_timing(&gDcn.d, (uint32_t)otg, &before);
	wait_frames((uint32_t)otg, 2);

	if (dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &after) != DCN41_OK) {
		N48LOG("dcn-mode: could not re-read OTG %d's timing", otg);
		st = st ? st : 4u;
		goto report;
	}
	dcn41_otg_timing_decode(&after, &da);
	diffs = dcn41_otg_timing_diff(&before, &after, &fw, &fr);
	f1 = after.frame_count;
	N48LOG("dcn-mode: VERIFY — %u dword(s) differ after the write-back (first writable %s, first "
	       "read-only %s); active %ux%u -> %ux%u; frame count %u -> %u (delta %u); stalls %u; "
	       "allowlist allowed %llu refused %llu",
	       diffs, diffs ? dcn41_timing_w_name(fw) : "none", diffs ? dcn41_timing_r_name(fr) : "none",
	       db.h_active, db.v_active, da.h_active, da.v_active, f0, f1,
	       dcn41_frame_delta(f0, f1), stalls,
	       (unsigned long long)gDcn.allow.allowed, (unsigned long long)gDcn.allow.refused);
	if (diffs != 0) {
		for (uint32_t i = 0; i < DCN41_TIMING_W_COUNT; i++)
			if (before.w[i] != after.w[i])
				N48LOG("dcn-mode:   DIFFERS %-24s %#010x -> %#010x", dcn41_timing_w_name(i),
				       before.w[i], after.w[i]);
		for (uint32_t i = 0; i < DCN41_TIMING_R_COUNT; i++)
			if (before.r[i] != after.r[i])
				N48LOG("dcn-mode:   DIFFERS(ro) %-24s %#010x -> %#010x", dcn41_timing_r_name(i),
				       before.r[i], after.r[i]);
		if (st == 0) st = 0x40u;
	}
	if (realChange) {
		rate2 = measure_refresh_mhz((uint32_t)otg, 2000, &frames2);
		N48LOG("dcn-mode: AFTER the restore — measured %llu mHz over %u frames in 2 s "
		       "(before %llu, with the change %llu)", (unsigned long long)rate2, frames2,
		       (unsigned long long)rate0, (unsigned long long)rate1);
	}
	if (dcn41_frame_delta(f0, f1) == 0) {
		N48LOG("dcn-mode: *** the frame counter did not advance across the whole operation — the OTG "
		       "may have stopped ***");
		if (st == 0) st = 0x41u;
	}

report:
	if (out) {
		if (outCount > 0)  out[0]  = gDcn.armed ? 1u : 0u;
		if (outCount > 1)  out[1]  = st;
		if (outCount > 2)  out[2]  = (uint64_t)(int64_t)otg;
		if (outCount > 3)  out[3]  = diffs;
		if (outCount > 4)  out[4]  = (uint64_t)db.h_active | ((uint64_t)db.v_active << 16)
		                           | ((uint64_t)db.h_total << 32) | ((uint64_t)db.v_total << 48);
		if (outCount > 5)  out[5]  = (uint64_t)da.h_active | ((uint64_t)da.v_active << 16)
		                           | ((uint64_t)da.h_total << 32) | ((uint64_t)da.v_total << 48);
		if (outCount > 6)  out[6]  = f0;
		if (outCount > 7)  out[7]  = f1;
		if (outCount > 8)  out[8]  = dcn41_frame_delta(f0, f1);
		if (outCount > 9)  out[9]  = gDcn.allow.allowed;
		if (outCount > 10) out[10] = gDcn.allow.refused;
		if (outCount > 11) out[11] = (uint64_t)stalls | ((uint64_t)gDcn.retimings << 16)
		                           | ((uint64_t)gDcn.goldenRestores << 32);
		if (outCount > 12) out[12] = (uint64_t)(rate0 & 0xFFFFFu) | ((uint64_t)(rate1 & 0xFFFFFu) << 20)
		                           | ((uint64_t)(rate2 & 0xFFFFFu) << 40)
		                           | ((uint64_t)(db.update_lock_held ? 1u : 0u) << 60)
		                           | ((uint64_t)(da.master_en ? 1u : 0u) << 61)
		                           | ((uint64_t)(realChange ? 1u : 0u) << 62);
	}
	return st;
}

// ---- build 0.0.518: flip mode (switch 74) and its unarmed A/B test ------------------------------
//
// Every access below goes through gDcn.d, the BOUND device (bind() above: allowlist armed first, then dcn41_dev_init with
// dcn_wreg as the only writer). The 0.0.515 read-only raster device (gLr) is never used here. HUBP0 only (T1: OTG0 + HUBP0);
// fmHubpGeom refuses unless OTG0 is the lit one. The one register-writing call is fmFlip ->
// dcn41_hubp_program_flip(HUBP0, mc, vmid 0, tmz 0, immediate false), whose writes are the ones dcnflip's 975 allowlisted
// writes already exercised, and which dcn41 refuses for any address outside the exact set {A, B} while flip mode holds it.

uint32_t fmArmed() { return gDcn.armed ? 1u : 0u; }

uint32_t fmTestFlipHeld() { return gDcn.flipHeld ? 1u : 0u; }

int fmHubpGeom(uint32_t *vpW, uint32_t *vpH, uint32_t *pitchPx, uint32_t *fmt, uint32_t *swMode) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (lit_otg() != 0) return DCN41_E_INST;
	const uint32_t dim   = dcn_rreg(&gDcn, hubp_reg(kOffViewportDim,  0u));
	const uint32_t pitch = dcn_rreg(&gDcn, hubp_reg(kOffSurfPitch,    0u));
	const uint32_t cfg   = dcn_rreg(&gDcn, hubp_reg(kOffSurfConfig,   0u));
	const uint32_t tile  = dcn_rreg(&gDcn, hubp_reg(kOffTilingConfig, 0u));
	*vpW = dim & 0xFFFFu; *vpH = (dim >> 16) & 0xFFFFu;
	*pitchPx = (pitch & 0xFFFFu) + 1u;          /* hubp401 writes surface_pitch - 1 */
	*fmt = cfg & 0x7Fu; *swMode = tile & 0x1Fu;
	return DCN41_OK;
}

int fmReadFront(uint32_t *pending, uint64_t *earliest) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	bool p = true;
	const int rc = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &p, earliest);
	*pending = p ? 1u : 0u;
	return rc;
}

int fmReadPrimary(uint64_t *mc) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	return dcn41_hubp_read_primary_addr(&gDcn.d, 0u, mc);
}

int fmFlip(uint64_t mc, const char *tag) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	gDcn.tag = tag;
	return dcn41_hubp_program_flip(&gDcn.d, 0u, mc, 0u, false, false);
}

// The exact flip set {A, B} (n 2) or none (n 0). Also seeds the device's request record from the programmed address when no
// flip was ever requested this boot (a software field only: Linux's hubp->request_address), so the pending test compares
// EARLIEST_INUSE against what the hardware was last told, not against 0.
int fmSetExact(uint64_t a, uint64_t b, uint32_t n) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (n != 0u && !gDcn.d.hubp_request_valid[0]) {
		uint64_t cur = 0;
		const int r = dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur);
		if (r != DCN41_OK || cur == 0) return r != DCN41_OK ? r : DCN41_E_ADDR;
		gDcn.d.hubp_request_addr[0] = cur;
		gDcn.d.hubp_request_valid[0] = 1;
	}
	const uint64_t ab[2] = { a, b };
	return dcn41_dev_set_flip_exact(&gDcn.d, n ? ab : nullptr, n);
}

int fmFrameCount(uint32_t *fc) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	return dcn41_otg_get_frame_count(&gDcn.d, 0u, fc);
}

uint64_t fmAllowCounts(uint64_t *refused) {
	if (refused) *refused = gDcn.allow.refused;
	return gDcn.allow.allowed;
}

// build 0.0.519: the A/B test's CRC witness. OTG0 only (fmFlip and fmFrameCount are HUBP0/OTG0), the
// same crc_enable/crc_read/crc_disable dcnflip uses (: the CRC is taken at the timing generator over the active raster).
int fmCrcBegin() {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (lit_otg() != 0) return DCN41_E_INST;
	bool en = false;
	uint32_t aw = 0, ah = 0;
	if (dcn41_otg_get_active_size(&gDcn.d, 0u, &en, &aw, &ah) != DCN41_OK || !en || aw == 0 || ah == 0) return DCN41_E_INST;
	crc_enable(0u, aw, ah);
	wait_frames(0u, 3u);
	return DCN41_OK;
}

int fmCrcRead(uint32_t frames, uint32_t *rg, uint32_t *b) {
	if (!gDcn.armed) return DCN41_E_UNINIT;
	if (frames) wait_frames(0u, frames);
	crc_read(0u, rg, b);
	return DCN41_OK;
}

void fmCrcEnd() {
	if (!gDcn.armed) return;
	crc_disable(0u);
}

// ---- build 0.0.603: native scanout (S2a). The plane is taken only while a native N48N client holds it ------------------------------
//
// WHAT THIS WRITES: HUBP0's flip registers through dcn41_hubp_program_flip (the sequence proven at, addresses confined to
// the console and the registered slots by n48scan::flip_target_ok on top of dcn41's window), and the VUPDATE_NO_LOCK enable/ack bits of the
// lit OTG through dcn41_irq_set / dcn41_irq_ack (the allowlist gates all of it). Nothing else.
//
// THE CONSOLE IS RESTORED BY ONE FUNCTION, scanRestore, shared by: ScanoutRelease, the N48N close / clientDied / stop path, a BoFree of a
// buffer the hardware is showing, `dcnflip 0` (scanEscape), and the watchdog thread (5 s idle, a rate-guard trip requested from the IRQ
// handler). It programs the console address, waits for the latch by POLLING (no interrupt needed), verifies, and only then drops the state.
//
// LOCKS: see ScanState. Every function here that takes gScan.lock does register I/O only under it; the waits are outside it.
static IOLock *scan_lock() {
	if (gScan.lock == nullptr) {
		IOLock *l = IOLockAlloc();
		if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gScan.lock)) IOLockFree(l);
	}
	return gScan.lock;
}

struct ScanGeom { uint32_t w, h, pitchPx, fmt, sw; bool dcc; };
// HUBP0's plane geometry, read from the hardware (5 register reads). Caller holds the lock or is single-threaded.
static void scan_read_geom(ScanGeom *g) {
	const uint32_t dim   = dcn_rreg(&gDcn, hubp_reg(kOffViewportDim,  0u));
	const uint32_t pitch = dcn_rreg(&gDcn, hubp_reg(kOffSurfPitch,    0u));
	const uint32_t cfg   = dcn_rreg(&gDcn, hubp_reg(kOffSurfConfig,   0u));
	const uint32_t tile  = dcn_rreg(&gDcn, hubp_reg(kOffTilingConfig, 0u));
	const uint32_t ctl   = dcn_rreg(&gDcn, hubp_reg(kOffSurfControl,  0u));
	g->w = dim & 0xFFFFu; g->h = (dim >> 16) & 0xFFFFu;
	g->pitchPx = (pitch & 0xFFFFu) + 1u;        /* hubp401 writes surface_pitch - 1 */
	g->fmt = cfg & 0x7Fu; g->sw = tile & 0x1Fu;
	g->dcc = (ctl & 0x2u) != 0u;                /* HUBPREQ0_DCSURF_SURFACE_CONTROL.PRIMARY_SURFACE_DCC_EN */
}

// The four fields that make the restore exact (n48scan::restore_exact). Acquire only.
struct ScanExact { uint32_t vmid; bool tmz; uint32_t flipType; uint32_t vpStart; };
static void scan_read_exact(ScanExact *x) {
	x->vmid     = dcn_rreg(&gDcn, hubp_reg(kOffVmidSettings, 0u)) & 0xFu;                 /* HUBPREQ0_VMID_SETTINGS_0.VMID */
	x->tmz      = (dcn_rreg(&gDcn, hubp_reg(kOffSurfControl, 0u)) & 0x1u) != 0u;          /* DCSURF_SURFACE_CONTROL.PRIMARY_SURFACE_TMZ */
	x->flipType = (dcn_rreg(&gDcn, hubp_reg(kOffFlipControl, 0u)) >> 1) & 0x1u;           /* DCSURF_FLIP_CONTROL.SURFACE_FLIP_TYPE */
	x->vpStart  = dcn_rreg(&gDcn, hubp_reg(kOffViewportStart, 0u));                       /* HUBP0_DCSURF_PRI_VIEWPORT_START (x and y) */
}

// One observation of HUBP0: resolves a pending Present into a latch. Caller holds gScan.lock. The latch is the RAW DCSURF_FLIP_CONTROL.SURFACE_FLIP_PENDING
// bit clearing (a VUPDATE accepted the address); EARLIEST_INUSE is read too but only feeds the reuse rule (see n48scan::latch_observe).
static bool scan_poll_locked(bool byIrq) {
	uint32_t raw = 0;
	const uint64_t fcNow = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK ? n48scan::fc_extend(gScan.fc, raw) : gScan.fc.v;
	if (!gScan.tbl.pendActive) return false;
	const uint32_t fcr = dcn_rreg(&gDcn, hubp_reg(kOffFlipControl, 0u));
	if (fcr == 0xFFFFFFFFu) return false;                               // a dead read is "still pending", never a latch
	if (!n48scan::latch_observe(gScan.tbl, (fcr & 0x100u) != 0u, fcNow)) return false;
	const uint64_t t = now_ns();
	if (gScan.firstLatchNs == 0) gScan.firstLatchNs = t;
	gScan.lastLatchNs = t;
	if (byIrq) gScan.latchIrq++; else gScan.latchPoll++;
	return true;
}

// The interrupt path (see the declaration before ihEntry). VUPDATE_NO_LOCK: one per frame; here it is the latch witness and the rate guard's input.
static bool scanIrq(const struct dcn41_irq &irq, uint64_t nowNs, int *ar) {
	IOLock *l = gScan.lock;
	if (l == nullptr) return false;
	IOLockLock(l);
	if (!gScan.acquired) { IOLockUnlock(l); return false; }        // released between the caller's test and the lock: the caller acks
	gDcn.tag = "irqack";
	*ar = dcn41_irq_ack(&gDcn.d, irq);
	if (n48scan::storm_note(gScan.storm, nowNs)) {
		// Disable what we enabled; the restore itself sleeps, so the WATCHDOG THREAD performs it (never this handler).
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "stormoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
		gDcn.stormTripped = true;
		gScan.stormTrips++;
		gScan.wantRestore = true; gScan.wantWhy = "IRQ storm guard (rate)";
		N48LOG("n48scan: STORM GUARD - more than %u DCE interrupts in %llu ms; source disabled by the handler, console restore requested from the watchdog",
		       n48scan::kStormLimit, (unsigned long long)(n48scan::kStormWindowNs / 1000000ull));
	}
	if (irq.kind == DCN41_IRQ_VUPDATE_NO_LOCK && irq.inst == 0u) {
		gScan.vupdates++;
		if (!gScan.restoring && !scan_poll_locked(true)) n48scan::note_vupdate_no_latch(gScan.tbl);
	}
	IOLockUnlock(l);
	return true;
}

// Restore the console plane. Idempotent. `force`: also act when nothing is acquired but HUBP0 is not on the console recorded earlier
// (the `dcnflip 0` escape after a restore that failed). `expectGen` nonzero: only if that acquisition is still the current one.
// out[0] = verified (1/0), out[1] = HUBP0's programmed address after. Returns 0.
static uint32_t scan_restore(const char *why, bool force, uint32_t expectGen, uint64_t *out) {
	using namespace n48scan;
	IOLock *l = gScan.lock;
	if (l == nullptr) { if (out) { out[0] = 1; out[1] = 0; } return 0; }
	IOLockLock(l);
	if (expectGen != 0u && gScan.gen != expectGen) { IOLockUnlock(l); if (out) { out[0] = 1; out[1] = 0; } return 0; }
	if (gScan.restoring) {
		// Another thread is restoring: wait for it (the lock is dropped around every wait), bounded at 2 s.
		for (uint32_t i = 0; i < 2000u && gScan.restoring; i++) { IOLockUnlock(l); IOSleep(1); IOLockLock(l); }
		uint64_t cur = 0;
		const bool ok = !gScan.restoring && gDcn.armed && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK && cur == gScan.consoleMc;
		IOLockUnlock(l);
		if (out) { out[0] = ok ? 1u : 0u; out[1] = cur; }
		return 0;
	}
	uint64_t plane = 0;
	const bool planeOk = gDcn.armed && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane) == DCN41_OK;
	const bool need = gScan.acquired || (force && gScan.haveConsole && planeOk && plane != gScan.consoleMc);
	if (!need) {
		// Nothing to do. The verdict is still honest: a console learned earlier that the plane is NOT on (a failed restore, or a stray
		// flip) reads as NOT verified, so an idempotent second Release never turns a failure into a success.
		const bool ok = !gScan.haveConsole || (planeOk && plane == gScan.consoleMc);
		if (gScan.haveConsole && planeOk) gScan.restoreFailed = !ok;
		IOLockUnlock(l);
		if (out) { out[0] = ok ? 1u : 0u; out[1] = planeOk ? plane : 0; }
		return 0;
	}
	gScan.restoring = true;
	const uint64_t consoleMc = gScan.consoleMc;
	const uint64_t t0 = now_ns();
	uint64_t pres = gScan.tbl.presents, lat = gScan.tbl.latched, rep = gScan.tbl.replaced, rpt = gScan.tbl.repeats;
	bool verified = false;
	// The machine (n48scan::restore_next) decides every step; this loop only performs them. The lock is held for register I/O only and is
	// dropped around every sleep, so the interrupt handler can never wait on a sleeper.
	RestoreSm sm;
	restore_begin(sm, consoleMc);
	RestoreAct act = restore_next(sm, false, 0ull, 0ull, true);
	while (act != kActDone) {
		if (act == kActProgram) {
			gDcn.tag = "scanback";
			const int pr = dcn41_hubp_program_flip(&gDcn.d, 0u, consoleMc, 0u, false, false);
			if (pr != DCN41_OK) N48LOG("n48scan: RESTORE attempt %u: programming the console %#llx returned %d", sm.attempts, (unsigned long long)consoleMc, pr);
			act = kActPoll;
			continue;
		}
		IOLockUnlock(l);
		IOSleep(1);                       // a VUPDATE latches within one frame; polled, never waited for by interrupt
		IOLockLock(l);
		bool pend = true; uint64_t early = 0, cur = 0;
		const bool rd = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK;
		act = restore_next(sm, rd, cur, early, pend);
	}
	const uint32_t attempts = sm.attempts;
	// (3) verification read and the state teardown, under the lock
	bool pend = true; uint64_t early = 0, cur = 0;
	const bool rd = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK && dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) == DCN41_OK;
	verified = rd && restore_verified(cur, early, pend, consoleMc);
	if (gDcn.srcEnabled && gDcn.srcKind == (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK && gDcn.srcInst == 0u) {
		struct dcn41_irq off { gDcn.srcKind, gDcn.srcInst };
		gDcn.tag = "scanirqoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
	}
	table_init(gScan.tbl, consoleMc, gScan.consoleBytes);
	gScan.acquired = false;
	gScan.wantRestore = false; gScan.wantWhy = nullptr;
	gScan.restores++;
	if (!verified) gScan.restoreFailures++;
	gScan.restoreFailed = !verified;
	__atomic_store_n(&gScan.active, 0u, __ATOMIC_RELEASE);
	gScan.restoring = false;
	N48LOG("n48scan: RESTORE (%s) -> console %#llx: plane reads %#llx, EARLIEST_INUSE %#llx, pending %d -> %s after %u attempt(s) in %llu us; "
	       "this session: presents %llu latched %llu replaced %llu repeats %llu",
	       why, (unsigned long long)consoleMc, (unsigned long long)cur, (unsigned long long)early, pend ? 1 : 0,
	       verified ? "VERIFIED" : "*** NOT VERIFIED - run `dcnflip 0` again, then reboot if it persists ***", attempts,
	       (unsigned long long)((now_ns() - t0) / 1000ull), (unsigned long long)pres, (unsigned long long)lat, (unsigned long long)rep, (unsigned long long)rpt);
	IOLockUnlock(l);
	if (out) { out[0] = verified ? 1u : 0u; out[1] = cur; }
	return 0;
}

// The watchdog thread: started by Acquire, one per acquisition. IRQ-INDEPENDENT: it polls the plane itself every 50 ms (which also keeps the
// Status counters honest when no interrupt arrives), restores after 5 s without a scanout call, and performs the restores the IRQ handler
// requests. It ends when its acquisition ends. (The pattern is the kext's other pollers: kernel_thread_start, IOSleep, fall off the end.)
static void scan_watchdog_loop(uint32_t myGen) {
	for (;;) {
		IOSleep(50);
		IOLock *l = gScan.lock;
		if (l == nullptr) break;
		IOLockLock(l);
		if (!gScan.acquired || gScan.gen != myGen) { IOLockUnlock(l); break; }
		if (gScan.restoring) { IOLockUnlock(l); continue; }
		(void)scan_poll_locked(false);
		// The live plane must still be the one acquired (covers --hold, where no Present would notice): every 50 ms.
		ScanGeom g {};
		scan_read_geom(&g);
		const bool geomBad = n48scan::geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) != n48scan::kGeomOk || g.w != gScan.width || g.h != gScan.height || g.pitchPx != gScan.pitchPx;
		const char *why = nullptr;
		if (gScan.wantRestore) why = gScan.wantWhy ? gScan.wantWhy : "requested";
		else if (geomBad) why = "live plane geometry changed";
		else if (n48scan::idle_expired(now_ns(), gScan.lastActivityNs)) why = "idle watchdog: 5 s without a scanout call";
		if (why) gScan.wdRestores++;
		IOLockUnlock(l);
		if (why) { N48LOG("n48scan: WATCHDOG restoring the console: %s", why); (void)scan_restore(why, false, myGen, nullptr); break; }
	}
}
static void scan_watchdog(void *arg, wait_result_t) {
	scan_watchdog_loop((uint32_t)(uintptr_t)arg);
	__atomic_fetch_sub(&gScan.watchdogAlive, 1u, __ATOMIC_RELEASE);   // the thread touches nothing of ours after this
}

uint32_t scanGeneration() { return gScan.gen; }
bool scanActive() { return __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) != 0u; }

// 0.0.618 (V1): the vblank timing sample (see navi48_dcn.hpp). Reads only: OTG_CONTROL / H,V_TOTAL / OTG_STATUS_POSITION / OTG_V_BLANK_START_END / DP_DTO0, through gLr (its write callback writes nothing).
// The raster constants are cached for 250 ms (a mode change shows within that, and an incoherent position - outside the cached raster - refuses the sample on its own). The cache words are
// plain 32/64-bit stores: perform runs on one thread at a time and a torn cache can only fail the coherence checks.
namespace {
struct VblCache { uint64_t atNs; uint64_t periodNs; uint32_t otg, hTot, vTot, valid; };
VblCache gVbl {};
constexpr uint64_t kVblCacheNs = 250ull * 1000000ull;
bool vbl_refresh(uint64_t nowNs) {
	gVbl.valid = 0u;
	struct dcn41_dev *d = &gLr.d;
	int otg = -1;
	for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
		bool en = false; uint32_t w = 0u, h = 0u;
		if (dcn41_otg_get_active_size(d, i, &en, &w, &h) != DCN41_OK) return false;
		if (en) { otg = (int)i; break; }
	}
	if (otg < 0) return false;
	uint32_t ht1 = 0u, vt1 = 0u;
	if (dcn41_otg_get_totals(d, (uint32_t)otg, &ht1, &vt1) != DCN41_OK) return false;
	const uint32_t hTot = ht1 + 1u, vTot = vt1 + 1u;
	// The pixel clock: the EDID row whose totals equal the OTG's (liveRaster's rule) first; else DP_DTO0's phase (scanQuery's rule: OTG0 only, enable bit 4).
	uint64_t pixHz = 0ull;
	uint32_t ha, va, ht, vt, hf, hs, vf, vs; uint64_t pc = 0;
	if (n48lr_live(&gLr, &ha, &va, &ht, &vt, &hf, &hs, &vf, &vs, &pc) != 0u && ht == hTot && vt == vTot) pixHz = pc;
	if (pixHz == 0ull && otg == 0) {
		uint32_t prc = 0, phase = 0;
		const uint32_t a0 = dcn41_abs(d, 0x80u, 1u), a1 = dcn41_abs(d, 0x81u, 1u);
		if (a0 != DCN41_BAD_OFFSET && a1 != DCN41_BAD_OFFSET) {
			prc = d->rreg(d->cookie, a0); phase = d->rreg(d->cookie, a1);
			if ((prc & 0x10u) != 0u) pixHz = phase;
		}
	}
	const uint64_t p = n48disp::vbl_period_ns(hTot, vTot, pixHz);
	if (p == 0ull) return false;
	gVbl.otg = (uint32_t)otg; gVbl.hTot = hTot; gVbl.vTot = vTot; gVbl.periodNs = p; gVbl.atNs = nowNs; gVbl.valid = 1u;
	return true;
}
}  // namespace

bool vblSample(uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs) {
	if (!periodNs || !delayNs || !nowAbs || gLr.state != N48LR_READY) return false;
	if (modeTrialBusy()) return false;                                  // the raster is being reprogrammed: nothing coherent to read
	const uint64_t nowNs = now_ns();
	if (!gVbl.valid || nowNs - gVbl.atNs > kVblCacheNs || nowNs < gVbl.atNs) { if (!vbl_refresh(nowNs)) return false; }
	uint32_t vbs = 0, vbe = 0, h = 0, v = 0;
	const uint64_t abs0 = mach_absolute_time();
	if (dcn41_otg_get_scanoutpos(&gLr.d, gVbl.otg, &vbs, &vbe, &h, &v) != DCN41_OK) return false;
	uint64_t delay = 0;
	if (!n48disp::vbl_delay_ns(v, h, vbs, gVbl.hTot, gVbl.vTot, gVbl.periodNs, &delay)) { gVbl.valid = 0u; return false; }   // incoherent (a mode change?): re-read the raster next time
	*periodNs = gVbl.periodNs; *delayNs = delay; *nowAbs = abs0;
	return true;
}   // 0.0.617 (K6): lock-free

void scanEscape(const char *why) {
	if (gScan.lock == nullptr) return;
	(void)scan_restore(why, true, 0u, nullptr);
}

void scanLogState() {
	if (gScan.acquires == 0) return;
	N48LOG("dcn-state: n48scan acquires %llu restores %llu (failed %llu) acquired %d gen %u console %#llx; this session presents %llu latched %llu replaced %llu "
	       "repeats %llu vupdates %llu (latched by irq %llu poll %llu) refused %llu geom-refused %llu watchdog restores %llu storm trips %llu",
	       (unsigned long long)gScan.acquires, (unsigned long long)gScan.restores, (unsigned long long)gScan.restoreFailures, gScan.acquired ? 1 : 0, gScan.gen,
	       (unsigned long long)gScan.consoleMc, (unsigned long long)gScan.tbl.presents, (unsigned long long)gScan.tbl.latched, (unsigned long long)gScan.tbl.replaced,
	       (unsigned long long)gScan.tbl.repeats, (unsigned long long)gScan.vupdates, (unsigned long long)gScan.latchIrq, (unsigned long long)gScan.latchPoll,
	       (unsigned long long)gScan.refused, (unsigned long long)gScan.geomRefused, (unsigned long long)gScan.wdRestores, (unsigned long long)gScan.stormTrips);
}

// ScanoutQuery: read-only, valid before Acquire. 0 or an IOReturn.
uint32_t scanQuery(struct n48n_scan_query *o) {
	using namespace n48scan;
	if (!gDcn.armed) return kNotReady;
	IOLock *l = scan_lock();
	if (l == nullptr) return kNoResources;
	IOLockLock(l);
	bzero(o, sizeof(*o));
	const int otg = lit_otg();
	ScanGeom g {};
	scan_read_geom(&g);
	uint32_t flags = 0;
	if (otg >= 0) flags |= N48N_SCANQ_LIT;
	if (amdgpu::native_s1b_state().gate == n48native::kGateOn) flags |= N48N_SCANQ_NATIVE;
	if (gScan.acquired) flags |= N48N_SCANQ_ACQUIRED;
	if (otg == 0 && geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) == kGeomOk) flags |= N48N_SCANQ_GEOM_OK;
	struct dcn41_otg_timing tm {};
	struct dcn41_timing_decoded td {};
	if (otg >= 0 && dcn41_otg_read_timing(&gDcn.d, (uint32_t)otg, &tm) == DCN41_OK) {
		dcn41_otg_timing_decode(&tm, &td);
		o->h_active = td.h_active; o->v_active = td.v_active; o->h_total = td.h_total; o->v_total = td.v_total;
	}
	// The pixel clock: DP_DTO0 (OTG0_PIXEL_RATE_CNTL.DP_DTO0_ENABLE and DP_DTO0_PHASE, dcn_4_1_0_offset.h BASE_IDX 1: 0x80 / 0x81; the
	// census read 0x00011090 and phase 241,500,000). The DPDTO0_INT term of the dcn401 formula is taken as 0 (the review's reading of this
	// card); the check against the EDID row below is what says whether that held.
	const uint32_t prc = dcn_rreg(&gDcn, gDcn.seg[1] + 0x80u);
	const uint32_t phase = dcn_rreg(&gDcn, gDcn.seg[1] + 0x81u);
	uint64_t pixHz = ((prc & 0x10u) != 0u && otg == 0) ? phase : 0ull;
	if (pixHz != 0ull) flags |= N48N_SCANQ_DTO_VALID;
	else {
		uint32_t ha, va, ht, vt, hf, hs, vf, vs; uint64_t pc = 0;
		if (otg >= 0 && liveRaster(&ha, &va, &ht, &vt, &hf, &hs, &vf, &vs, &pc) != 0u) pixHz = pc;
	}
	o->pix_clk_khz = (uint32_t)(pixHz / 1000ull);
	o->refresh_mhz = (uint32_t)refresh_mhz(pixHz, td.h_total, td.v_total);
	o->pitch_px = g.pitchPx; o->hubp_format = g.fmt; o->sw_mode = g.sw; o->otg = otg >= 0 ? (uint32_t)otg : 0xFFFFFFFFu;
	o->dcc_en = g.dcc ? 1u : 0u;
	o->flags = flags;
	uint32_t raw = 0;
	const bool haveFc = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK;
	o->frame_count = gScan.acquired ? (haveFc ? fc_extend(gScan.fc, raw) : gScan.fc.v) : (uint64_t)raw;
	uint64_t plane = 0, early = 0; bool pend = false;
	(void)dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane);
	(void)dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early);
	o->console_mc = gScan.haveConsole ? gScan.consoleMc : plane;
	o->plane_mc = plane; o->earliest_mc = early;
	o->acquired = gScan.acquired ? 1u : 0u;
	o->plane_w = g.w; o->plane_h = g.h;
	IOLockUnlock(l);
	return kOk;
}

uint32_t scanAcquire(uint64_t out[2], uint32_t sess) {
	using namespace n48scan;
	if (!gDcn.armed) return kNotReady;
	if (amdgpu::native_s1b_state().gate != n48native::kGateOn) return kNotReady;      // native boots only
	IOLock *l = scan_lock();
	if (l == nullptr) return kNoResources;
	IOLockLock(l);
	uint32_t rc = kOk;
	bool irqOn = false;
	do {
		if (gScan.acquired || gScan.restoring) { rc = kBusy; break; }
		if (__atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u && !n48mt::hold_allows_acquire(gMt, sess)) { N48LOG("n48scan: Acquire refused: a mode trial is running"); rc = kBusy; break; }   // 0.0.605; 0.0.609: except while a row-120 hold is UP and this session may take it (atomic words only, never the trial engine lock)
		if (gDcn.flipHeld || gDcn.srcEnabled) { N48LOG("n48scan: Acquire refused: dcnflip's pattern or a DCN interrupt source is already in use"); rc = kBusy; break; }
		if (lit_otg() != 0) { N48LOG("n48scan: Acquire refused: OTG0 is not the lit OTG"); rc = kNotReady; break; }
		ScanGeom g {};
		scan_read_geom(&g);
		const uint32_t gw = geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc);
		if (gw != kGeomOk) {
			N48LOG("n48scan: Acquire refused: plane %ux%u pitch %u px fmt %u SW_MODE %u DCC_EN %d fails the geometry check (reason %u: 1 dims 2 format 3 tiling 4 DCC 5 pitch)",
			       g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc ? 1 : 0, gw);
			rc = kUnsupported; break;
		}
		ScanExact ex {};
		scan_read_exact(&ex);
		if (!restore_exact(ex.vmid, ex.tmz, ex.flipType, ex.vpStart)) {
			N48LOG("n48scan: Acquire refused: the console plane is not restorable exactly (VMID %u, TMZ %d, FLIP_TYPE %u, PRI_VIEWPORT_START %#x; all must be 0)", ex.vmid, ex.tmz ? 1 : 0, ex.flipType, ex.vpStart);
			rc = kUnsupported; break;
		}
		if (gDcn.d.scanout_hi == 0ull) { N48LOG("n48scan: Acquire refused: no frame-buffer window was read at bind (fail closed)"); rc = kNotReady; break; }
		uint64_t cur = 0;
		if (dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &cur) != DCN41_OK || cur == 0ull) { N48LOG("n48scan: Acquire refused: HUBP0's primary address is unreadable (%#llx)", (unsigned long long)cur); rc = kNotReady; break; }
		if (cur < gDcn.d.scanout_lo || cur >= gDcn.d.scanout_hi) { N48LOG("n48scan: Acquire refused: the console %#llx is outside the flip window, so it could not be restored", (unsigned long long)cur); rc = kNotReady; break; }
		if (gScan.haveConsole && cur != gScan.consoleMc) {
			N48LOG("n48scan: Acquire refused: HUBP0 reads %#llx, not the console %#llx learned earlier - run `dcnflip 0` first", (unsigned long long)cur, (unsigned long long)gScan.consoleMc);
			rc = kBusy; break;
		}
		// HUBP0 must be showing what it was last told, with nothing pending: seed the request record from the programmed address (as fmSetExact).
		gDcn.d.hubp_request_addr[0] = cur; gDcn.d.hubp_request_valid[0] = 1;
		bool pend = true; uint64_t early = 0;
		if (dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) != DCN41_OK || pend || early != cur) {
			N48LOG("n48scan: Acquire refused: a flip is pending or HUBP0 is not fetching its programmed address (pending %d, EARLIEST_INUSE %#llx, programmed %#llx)",
			       pend ? 1 : 0, (unsigned long long)early, (unsigned long long)cur);
			rc = kBusy; break;
		}
		// The VUPDATE source (the latch witness). One source at a time, as dcnvbl.
		struct dcn41_irq on { (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
		gDcn.tag = "scanirq";
		if (dcn41_irq_set(&gDcn.d, on, true) != DCN41_OK) { N48LOG("n48scan: Acquire refused: enabling VUPDATE_NO_LOCK on OTG0 failed"); rc = kNotReady; break; }
		irqOn = true;
		gDcn.srcEnabled = true; gDcn.srcKind = on.kind; gDcn.srcInst = on.inst;
		gDcn.enabledAtNs = now_ns(); gDcn.entriesAtEnable = gDcn.dceEntries; gDcn.stormTripped = false;
		gDcn.firstEntryNs = 0; gDcn.lastEntryNs = 0; gDcn.logBudget = kDceLogBudget;

		gScan.haveConsole = true;
		gScan.consoleMc = cur;
		gScan.consoleBytes = (uint64_t)g.pitchPx * 4ull * g.h;
		gScan.width = g.w; gScan.height = g.h; gScan.pitchPx = g.pitchPx;
		table_init(gScan.tbl, cur, gScan.consoleBytes);
		gScan.fc = FcExt{ 0, false };
		gScan.storm = Storm{ 0, 0, false };
		gScan.presentSeq = 0; gScan.vupdates = 0; gScan.latchIrq = 0; gScan.latchPoll = 0; gScan.refused = 0; gScan.geomRefused = 0;
		gScan.wdRestores = 0; gScan.stormTrips = 0;
		gScan.firstLatchNs = 0; gScan.lastLatchNs = 0; gScan.geomWarned = false;
		gScan.wantRestore = false; gScan.wantWhy = nullptr;
		uint32_t raw = 0;
		if (dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK) (void)fc_extend(gScan.fc, raw);
		gScan.lastActivityNs = now_ns();
		gScan.restoreFailed = false;
		gScan.gen++;
		gScan.acquires++;
		gScan.acquired = true;
		__atomic_store_n(&gScan.active, 1u, __ATOMIC_RELEASE);
		thread_t th = nullptr;
		__atomic_fetch_add(&gScan.watchdogAlive, 1u, __ATOMIC_ACQ_REL);
		const kern_return_t kr = kernel_thread_start(&scan_watchdog, (void *)(uintptr_t)gScan.gen, &th);
		if (kr != KERN_SUCCESS) {
			__atomic_fetch_sub(&gScan.watchdogAlive, 1u, __ATOMIC_ACQ_REL);
			N48LOG("n48scan: Acquire refused: the watchdog thread did not start (kr %#x)", kr);
			gScan.acquired = false;
			__atomic_store_n(&gScan.active, 0u, __ATOMIC_RELEASE);
			rc = kNotReady; break;
		}
		thread_deallocate(th);
		irqOn = false;   // owned by the acquisition now
		n48mt::hold_take_owner(gMt, sess);   // 0.0.609: a HANDOFF hold with no owner passes to this session (atomic words only); a no-op when no hold is up
		out[0] = cur; out[1] = gScan.fc.v;
		N48LOG("n48scan: ACQUIRED gen %u: plane %ux%u pitch %u px ARGB8888 linear, console %#llx (%llu bytes), window [%#llx, %#llx), VUPDATE_NO_LOCK enabled on OTG0, frame %llu",
		       gScan.gen, g.w, g.h, g.pitchPx, (unsigned long long)cur, (unsigned long long)gScan.consoleBytes,
		       (unsigned long long)gDcn.d.scanout_lo, (unsigned long long)gDcn.d.scanout_hi, (unsigned long long)gScan.fc.v);
	} while (false);
	if (rc != kOk && irqOn) {
		struct dcn41_irq off { (uint8_t)DCN41_IRQ_VUPDATE_NO_LOCK, 0u };
		gDcn.tag = "scanirqoff";
		(void)dcn41_irq_set(&gDcn.d, off, false);
		gDcn.srcEnabled = false;
	}
	IOLockUnlock(l);
	return rc;
}

uint32_t scanRegister(bool boVis, uint64_t boMc, uint64_t boSize, uint64_t offset, uint32_t pitchBytes, uint32_t width, uint32_t height,
                      uint32_t format, uint64_t out[2]) {
	using namespace n48scan;
	IOLock *l = gScan.lock;
	if (l == nullptr) return kNotReady;
	IOLockLock(l);
	uint32_t rc = kOk;
	do {
		if (!gScan.acquired || gScan.restoring || gScan.wantRestore) { rc = kNotReady; break; }
		gScan.lastActivityNs = now_ns();
		ScanGeom g {};
		scan_read_geom(&g);
		if (geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) != kGeomOk || g.w != gScan.width || g.h != gScan.height || g.pitchPx != gScan.pitchPx) { rc = kNotReady; break; }
		if (width != g.w || format != kFmtArgb8888) { rc = kBadArg; break; }
		const Fb fb { gDcn.d.scanout_lo, gDcn.d.scanout_hi, gScan.consoleMc, gScan.consoleBytes };
		uint64_t mc = 0, bytes = 0;
		rc = reg_bounds(BoRef{ boVis, boMc, boSize }, offset, pitchBytes, height, g.pitchPx * 4u, g.h, fb, &mc, &bytes);
		if (rc != kOk) break;
		uint32_t slot = kNoSlot;
		rc = slot_register(gScan.tbl, mc, bytes, &slot);
		if (rc != kOk) break;
		out[0] = slot; out[1] = mc;
		N48LOG("n48scan: REGISTER slot %u = MC %#llx (%llu bytes, BO offset %#llx)", slot, (unsigned long long)mc, (unsigned long long)bytes, (unsigned long long)offset);
	} while (false);
	IOLockUnlock(l);
	return rc;
}

uint32_t scanPresent(uint64_t slotIn, uint64_t flags, uint64_t out[3]) {
	using namespace n48scan;
	if (flags != 0ull || slotIn >= kMaxSlots) return kBadArg;
	IOLock *l = gScan.lock;
	if (l == nullptr) return kNotReady;
	IOLockLock(l);
	uint32_t rc = kOk;
	do {
		if (!gScan.acquired) { rc = kNotReady; break; }
		if (gScan.restoring || gScan.wantRestore) { gScan.refused++; rc = kNotReady; break; }
		gScan.lastActivityNs = now_ns();
		ScanGeom g {};
		scan_read_geom(&g);
		if (geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) != kGeomOk || g.w != gScan.width || g.h != gScan.height || g.pitchPx != gScan.pitchPx) {
			gScan.geomRefused++; gScan.refused++;
			if (!gScan.geomWarned) { gScan.geomWarned = true; N48LOG("n48scan: Present refused: the live plane %ux%u pitch %u fmt %u SW_MODE %u DCC %d is no longer the acquired geometry", g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc ? 1 : 0); }
			rc = kNotReady; break;
		}
		(void)scan_poll_locked(false);                      // resolve the previous Present first: a latch that the IRQ has not delivered yet is not "replaced"
		if (!gScan.tbl.s[slotIn].used) { rc = kNotFound; break; }
		const Table saved = gScan.tbl;
		uint32_t raw = 0;
		const uint64_t fcNow = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK ? fc_extend(gScan.fc, raw) : gScan.fc.v;
		const uint64_t id = gScan.presentSeq + 1ull;
		const uint64_t target = fcNow + 1ull;
		const uint64_t mc = present_begin(gScan.tbl, (uint32_t)slotIn, id, target);
		if (mc == 0ull || !flip_target_ok(gScan.tbl, mc)) { gScan.tbl = saved; gScan.refused++; rc = kBadArg; break; }
		gDcn.tag = "scanflip";
		const int fr = dcn41_hubp_program_flip(&gDcn.d, 0u, mc, 0u, false, false);
		if (fr != DCN41_OK) { gScan.tbl = saved; gScan.refused++; N48LOG("n48scan: Present of slot %llu refused by the flip layer (%d)", (unsigned long long)slotIn, fr); rc = kNotReady; break; }
		gScan.presentSeq = id;
		out[0] = id; out[1] = target; out[2] = gScan.vupdates;
	} while (false);
	IOLockUnlock(l);
	return rc;
}

uint32_t scanStatus(struct n48n_scan_status *o) {
	using namespace n48scan;
	IOLock *l = gScan.lock;
	if (l == nullptr || !gDcn.armed) return kNotReady;
	IOLockLock(l);
	bzero(o, sizeof(*o));
	if (gScan.acquired && !gScan.restoring) { gScan.lastActivityNs = now_ns(); (void)scan_poll_locked(false); }
	uint64_t plane = 0, early = 0; bool pend = false;
	const bool earlyOk = dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK;
	(void)dcn41_hubp_read_primary_addr(&gDcn.d, 0u, &plane);
	uint32_t raw = 0;
	const bool haveFc = dcn41_otg_get_frame_count(&gDcn.d, 0u, &raw) == DCN41_OK;
	o->acquired = gScan.acquired ? 1u : 0u;
	o->flags = (gScan.restoring ? N48N_SCANST_RESTORING : 0u) | (gScan.storm.tripped ? N48N_SCANST_STORM : 0u) | (gScan.wantRestore ? N48N_SCANST_WANT_RESTORE : 0u);
	o->front_slot = gScan.tbl.front;
	o->pending_slot = gScan.tbl.pendActive ? gScan.tbl.pendSlot : kNoSlot;
	o->frame_count = gScan.acquired ? (haveFc ? fc_extend(gScan.fc, raw) : gScan.fc.v) : (uint64_t)raw;
	o->console_mc = gScan.haveConsole ? gScan.consoleMc : plane; o->plane_mc = plane; o->earliest_mc = early;
	o->presents = gScan.tbl.presents; o->latched = gScan.tbl.latched; o->replaced = gScan.tbl.replaced; o->repeats = gScan.tbl.repeats;
	o->vupdates = gScan.vupdates; o->latch_irq = gScan.latchIrq; o->latch_poll = gScan.latchPoll; o->refused = gScan.refused;
	o->first_latch_ns = gScan.firstLatchNs; o->last_latch_ns = gScan.lastLatchNs;
	const uint64_t t = now_ns();
	o->idle_ms = (gScan.acquired && t >= gScan.lastActivityNs) ? (t - gScan.lastActivityNs) / 1000000ull : 0ull;
	o->watchdog_restores = gScan.wdRestores; o->storm_trips = gScan.stormTrips; o->geom_refused = gScan.geomRefused;
	for (uint32_t i = 0; i < kMaxSlots; i++) {
		const Slot &s = gScan.tbl.s[i];
		n48n_scan_slot &d = o->slot[i];
		d.mc = s.mc; d.latched_frame = s.latchedFrame; d.used = s.used ? 1u : 0u; d.presents = s.presents; d.latches = s.latches;
		if (!s.used) continue;
		d.flags = ((gScan.tbl.pendActive && gScan.tbl.pendSlot == i) ? N48N_SCANSLOT_PENDING : 0u) | ((earlyOk && early == s.mc) ? N48N_SCANSLOT_INUSE : 0u) |
		          (slot_reusable(gScan.tbl, i, early, earlyOk) ? N48N_SCANSLOT_REUSABLE : 0u);
	}
	IOLockUnlock(l);
	return kOk;
}

uint32_t scanRelease(const char *why, uint64_t out[2]) { return scan_restore(why, false, 0u, out); }

// A BO hosting scanout slots is going away (BoFree, the HUNG leak, the close of the session). n48scan::unpin_plan decides: alwaysFull (leak / close)
// or ANY slot that is shown (EARLIEST_INUSE) or pending forces the full restore FIRST, otherwise the slots are just dropped. pinGen is the
// scanGeneration() the pins were recorded under; a stale pin (an earlier acquisition) is a no-op. Returns 1 when the console was restored here.
// out (may be null) receives the restore's [verified, plane] when it ran.
uint32_t scanBoGone(uint32_t pinMask, uint32_t pinGen, bool alwaysFull, uint64_t *out) {
	using namespace n48scan;
	IOLock *l = gScan.lock;
	if (l == nullptr) return 0u;
	IOLockLock(l);
	bool full = false;
	uint32_t rc = 0u;
	if (!gScan.acquired && gScan.gen == pinGen && gScan.restoreFailed) rc = 2u;   // an earlier restore did not verify: the plane may still scan this BO
	if (gScan.acquired && gScan.gen == pinGen) {
		uint64_t early = 0; bool pend = false;
		const bool ok = !gScan.restoring && dcn41_hubp_is_flip_pending(&gDcn.d, 0u, &pend, &early) == DCN41_OK;
		const UnpinPlan plan = unpin_plan(gScan.tbl, (uint8_t)pinMask, alwaysFull || gScan.restoring, early, ok);
		full = plan.fullRelease;
		if (!full) {
			for (uint32_t i = 0; i < kMaxSlots; i++) {
				if (((plan.dropMask >> i) & 1u) == 0u) continue;
				(void)slot_unregister(gScan.tbl, i, early, ok);
				N48LOG("n48scan: slot %u unregistered (its BO was freed while neither shown nor pending)", i);
			}
		}
	}
	IOLockUnlock(l);
	if (!full) return rc;
	uint64_t r[2] = { 1, 0 };
	(void)scan_restore("a BO with a scanout slot is being released", false, pinGen, r);
	if (out) { out[0] = r[0]; out[1] = r[1]; }
	return r[0] != 0ull ? 1u : 2u;
}

// ---- build 0.0.605 (native S2d): the timed mode trial - the kext half ------------------------------------------------------------------------------
//
// WHAT THIS WRITES: through n48mt::Hw.wr = mt_wr = dcn41_allow_write + WREG32, i.e. THROUGH THE DCN ALLOWLIST, exactly the registers of navi48_modetrial_tables.h (kCap): the DP DTO
// trio, the OTG timing + global sync set, the DP1 MSA timing, and for row 120 the HUBP0 DLG/TTU/prefetch set. Nothing else, and never a clock: the clocks are only READ (DFS / DENTIST,
// decoded with the C1 DID table), and a row whose DML needs more than the readback says is DENIED. Every decision and every sequence (deny gate, plan, Linux write order, dwell, THE
// restore, the watchdog body, the latch) is in dcn/navi48_modetrial_pure.h and is driven by tests/native_s2d_test.cpp; this block only supplies the hardware.
static bool mt_ensure_lock() {
	if (gMtLock == nullptr) {
		IOLock *l = IOLockAlloc();
		if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gMtLock)) IOLockFree(l);
	}
	return gMtLock != nullptr;
}
static void mt_lock(void *) { IOLockLock(gMtLock); }
static void mt_unlock(void *) { IOLockUnlock(gMtLock); }
static uint32_t mt_rd(void *, uint32_t abs) {
	if (!gDcn.armed || gDcn.dev == nullptr) return 0xFFFFFFFFu;
	return dcn_rreg(&gDcn, abs);
}
// The allowlisted write, with the answer dcn_wreg does not give: false = the allowlist refused it (nothing reached the hardware).
static bool mt_wr(void *, uint32_t abs, uint32_t v) {
	if (!gDcn.armed || gDcn.dev == nullptr) return false;
	gDcn.tag = "modetrial";
	if (!dcn41_allow_write(&gDcn.allow, abs, v, gDcn.tag)) {
		const char *why = nullptr;
		const int reason = dcn41_allow_classify(abs, gDcn.allow.mmio_dwords, nullptr, &why);
		N48LOG("mode-trial: REFUSED write %#010x = %#010x - allowlist says %s: %s", abs, v, dcn41_allow_reason_name(reason), why ? why : "(no text)");
		return false;
	}
	amdgpu::WREG32(*gDcn.dev, abs, v);
	return true;
}
static void mt_sleep(void *, uint32_t ms) { /*nolock*/ IOSleep(ms); }   // never called with the scanout lock held; 0.0.606: the resync's msleep(60) runs under gMtLock (see the lock rank above)
static void mt_delay_us(void *, uint32_t us) { IODelay(us); }         // 0.0.606: the polls of the sequences (a few us to 200 us each)
static uint64_t mt_now_us(void *) { return now_ns() / 1000ull; }
static bool mt_frame(void *, uint32_t *fc) { return gDcn.armed && dcn41_otg_get_frame_count(&gDcn.d, 0u, fc) == DCN41_OK; }
static bool mt_gate_ok(void *) {
	const amdgpu::NativeS1bState &s = amdgpu::native_s1b_state();
	return s.gate == n48native::kGateOn && s.positivePass;
}
static bool mt_armed(void *) { return gDcn.armed && gDcn.dev != nullptr; }
static bool mt_plane_acquired(void *) {
	IOLock *l = scan_lock();
	if (l == nullptr) return true;    // fail closed
	IOLockLock(l);
	const bool a = gScan.acquired || gScan.restoring;
	IOLockUnlock(l);
	return a;
}
static bool mt_in_use(void *) { return gDcn.flipHeld || gDcn.srcEnabled || amdgpu::dal_busy(); }   // 0.0.605: a running DAL step counts as "in use"
// OTG0 is the ONLY lit OTG, and HUBP0 scans the 2560x1440 linear ARGB8888 plane the DML goldens were computed for.
static bool mt_otg_ok(void *) {
	if (!gDcn.armed) return false;
	uint32_t lit = 0, mask = 0;
	for (uint32_t i = 0; i < DCN41_NUM_PIPES; i++) {
		bool en = false; uint32_t w = 0, h = 0;
		if (dcn41_otg_get_active_size(&gDcn.d, i, &en, &w, &h) == DCN41_OK && en) { lit++; mask |= 1u << i; }
	}
	if (lit != 1u || mask != 1u) return false;
	ScanGeom g {};
	scan_read_geom(&g);
	return g.w == 2560u && g.h == 1440u && g.pitchPx == 2560u && n48scan::geom_check(g.w, g.h, g.pitchPx, g.fmt, g.sw, g.dcc) == n48scan::kGeomOk;
}
static bool mt_clocks(void *, n48dal::Decoded *d) {
	if (gDcn.dev == nullptr) return false;
	*d = n48dal::decode_clocks(amdgpu::RREG32(*gDcn.dev, n48dal::kRegPllReq), amdgpu::RREG32(*gDcn.dev, n48dal::kRegDfs0),
	                           amdgpu::RREG32(*gDcn.dev, n48dal::kRegDfs1), amdgpu::RREG32(*gDcn.dev, n48dal::kRegDentist));
	return true;
}
static void mt_log(void *, uint32_t code, uint64_t a, uint64_t b, uint64_t c, uint64_t d);
static bool mt_start_watchdog(void *, uint32_t trialId);
// 0.0.607, P4: the clock hold. These four talk to the PMFW through the amd/ layer and SLEEP: they are called by the trial thread, `dcnmode 0` and the kext stop with NO lock held (not gMtLock, not the scanout
// lock), never by the watchdog. They take no lock of this file and reference neither (the host test pins that).
static uint32_t mt_hold_pre(void *) { return amdgpu::dal_hold_pre(); }
static uint32_t mt_hold_raise(void *, uint32_t needDispKhz, uint32_t needDppKhz, n48dal::HoldRep *rep, n48dal::Decoded *after) {
	if (gDcn.dev == nullptr) return n48dal::kHrNoIo;
	return amdgpu::dal_hold_raise(*gDcn.dev, needDispKhz, needDppKhz, rep, after);
}
static uint32_t mt_hold_release(void *, bool restoreBad, n48dal::HoldRep *rep) {
	if (gDcn.dev == nullptr) return n48dal::kRlNotHeld;
	return amdgpu::dal_hold_release(*gDcn.dev, restoreBad, rep);
}
static uint32_t mt_hold_state(void *) { return amdgpu::dal_hold_state(); }
// 0.0.609: the scanout plane back to the console before a hold's timing is restored. scan_restore takes the scan lock itself and sleeps with it dropped; called with NO lock held (the pure engine guarantees it).
// 0 = nothing was acquired and the console is on the plane, 1 = acquired and the console is back and VERIFIED, 2 = the console is NOT verified back (this put-back, or an earlier one that failed).
static uint32_t mt_scan_release(void *) {
	const bool acq = mt_plane_acquired(nullptr);
	uint64_t o[2] = { 1, 0 };
	(void)scan_restore("row-120 hold ending", true, 0u, o);   // force: also acts when an EARLIER put-back (owner close, idle watchdog) failed and left acquired false with the plane off the console
	return o[0] != 1ull ? 2u : (acq ? 1u : 0u);
}
static const n48mt::Hw kMtHw = { nullptr, mt_rd, mt_wr, mt_sleep, mt_now_us, mt_frame, mt_lock, mt_unlock, mt_gate_ok, mt_armed, mt_plane_acquired, mt_in_use, mt_otg_ok, mt_clocks,
                                 mt_start_watchdog, mt_log, mt_delay_us, mt_hold_pre, mt_hold_raise, mt_hold_release, mt_hold_state, mt_scan_release };
// 0.0.607: the timing table. Row 120 stays DENIED (kProd.row120 = false) unless the boot-arg navi48-row120=1 is present.
static n48mt::Timing mt_timing() {
	n48mt::Timing t = n48mt::kProd;
	uint32_t v = 0;
	t.row120 = PE_parse_boot_argn("navi48-row120", &v, sizeof(v)) && v == 1u;
	return t;
}
static void mt_watchdog(void *arg, wait_result_t) {
	n48mt::watchdog_body(kMtHw, gMt, mt_timing(), (uint32_t)(uintptr_t)arg);
	__atomic_fetch_sub(&gMtWdAlive, 1u, __ATOMIC_RELEASE);      // the thread touches nothing of ours after this
}
static bool mt_start_watchdog(void *, uint32_t trialId) {
	thread_t th = nullptr;
	__atomic_fetch_add(&gMtWdAlive, 1u, __ATOMIC_ACQ_REL);
	const kern_return_t kr = kernel_thread_start(&mt_watchdog, (void *)(uintptr_t)trialId, &th);
	if (kr != KERN_SUCCESS) {
		__atomic_fetch_sub(&gMtWdAlive, 1u, __ATOMIC_ACQ_REL);
		N48LOG("mode-trial: the watchdog thread did not start (kr %#x): the trial is refused", kr);
		return false;
	}
	thread_deallocate(th);
	return true;
}
static void mt_log(void *, uint32_t code, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
	using namespace n48mt;
	switch (code) {
	case kLogWrite:    N48LOG("mode-trial: WRITE   %#010x %#010x -> %#010x %s", (uint32_t)a, (uint32_t)b, (uint32_t)c, d ? "ok" : "*** REFUSED ***"); break;
	case kLogRestore:  N48LOG("mode-trial: RESTORE %#010x %#010x -> %#010x %s", (uint32_t)a, (uint32_t)b, (uint32_t)c, d ? "ok" : "*** REFUSED ***"); break;
	case kLogDeny:     N48LOG("mode-trial: DENIED row %llu, reason %llu (nothing written)", (unsigned long long)b, (unsigned long long)a); break;
	case kLogWatchdog: N48LOG("mode-trial: *** WATCHDOG restored %llu registers (mismatch %llu, %llu pass(es)) ***", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c); break;
	case kLogVerdict:  N48LOG("mode-trial: verdict %llu flags %#llx mismatch %llu trial rate %llu mHz", (unsigned long long)a, (unsigned long long)b, (unsigned long long)c, (unsigned long long)d); break;
	case kLogSeq:      N48LOG("mode-trial: SEQ     %#010x %#010x -> %#010x %s", (uint32_t)a, (uint32_t)b, (uint32_t)c, d ? "ok" : "*** REFUSED ***"); break;   // 0.0.606
	case kLogSeqEnd: {  // which: 1 blank 2 unblank 3 lock 4 unlock 5 pending 6 latch 7 drr 8 uf-clear 9 stuck-lock 10 resync
		static const char *const kSeqName[] = { "?", "blank", "unblank", "lock", "unlock", "pending", "latch", "drr", "uf-clear", "stuck-lock", "resync" };
		N48LOG("mode-trial: %s: rc %llu in %llu us (%llu)", a < 11ull ? kSeqName[a] : "?", (unsigned long long)b, (unsigned long long)c, (unsigned long long)d);
		break;
	}
	default: break;
	}
}

// Bind time (native boots only): the full golden copy. Read-only. Never after a write (golden_take refuses), never over an earlier copy.
static void mt_golden_at_bind() {
	if (amdgpu::native_s1b_state().gate != n48native::kGateOn || !mt_ensure_lock()) return;
	if (n48mt::golden_take(kMtHw, gMt, true)) N48LOG("dcn: GOLDEN mode-trial register copy taken at bind on a native boot (%u registers, before any native write)", n48mt::kCapN);
	else N48LOG("dcn: the mode-trial golden copy was NOT taken at bind (a register read as all-ones, or a write already happened); the first trial retries it read-only");
}

// `dcnmode 0` on a native boot: end a running trial (it restores and answers ABORT), then put every register of the golden set that differs back. True when it acted (a golden copy exists).
static bool mt_emergency_restore(const char *why) {
	if (amdgpu::native_s1b_state().gate != n48native::kGateOn || !mt_ensure_lock() || !gDcn.armed) return false;
	n48mt::request_abort(kMtHw, gMt);
	n48mt::hold_abort_scan(kMtHw, gMt);    // 0.0.609: a running hold's scanout plane goes back FIRST (no lock held here)
	const bool idle = n48mt::wait_idle(kMtHw, gMt, 20000u, 50u);
	if (!gMt.everWrote) {                  // 0.0.605 review: no trial ever wrote anything this boot, so there is nothing of ours to put back (as mt_shutdown)
		// 0.0.607: but a hold raised by a trial that died before its first write is still HELD: nothing was written, the display is at 60 Hz, so it comes off after 3 s of the 60 Hz rate reads back.
		uint32_t prc0 = 0u;
		const bool healthy0 = n48mt::health_now(kMtHw, &prc0);
		const uint32_t rel0 = n48mt::emergency_release(kMtHw, gMt, mt_timing(), idle, 0u, healthy0, 3000u);
		if (rel0 != n48dal::kRlNotHeld) N48LOG("mode-trial: %s: clock hold release (nothing written) -> %u; hold state %s", why, rel0, n48dal::hold_state_name(amdgpu::dal_hold_state()));
		return false;
	}
	n48mt::Recover rc {};
	n48mt::emergency_recover(kMtHw, gMt, &rc);   // 0.0.606: the golden restore, then the DP1 resync when the display back end is unhealthy
	N48LOG("mode-trial: %s: full golden restore - %s, %u register(s) written, %u still differ, trial %s", why, rc.have ? "copy present" : "NO COPY (nothing touched)", rc.restored, rc.bad, idle ? "idle" : "STILL RUNNING");
	// 0.0.607 (P4 caller `dcnmode 0`): a clock hold a dead trial left HELD is released after the golden restore + resync VERIFIED, including 3 s of the 60 Hz rate. A restore that did not verify never releases.
	if (rc.have) {
		const uint32_t rel = n48mt::emergency_release(kMtHw, gMt, mt_timing(), idle, rc.bad, rc.healthyAfter, 3000u);
		if (rel != n48dal::kRlNotHeld) N48LOG("mode-trial: %s: clock hold release -> %u (%s); hold state %s", why, rel, rel == n48dal::kRlOk ? "released" : rel == n48dal::kRlBadRestore ? "NOT released: the restore did not verify" : "FAILED", n48dal::hold_state_name(amdgpu::dal_hold_state()));
	}
	if (rc.have) N48LOG("mode-trial: %s: display back end %s before; resync %s (rc %u); %s after, DIO_ERROR_COUNT growth %u since", why, rc.healthyBefore ? "healthy" : "UNHEALTHY", rc.resynced ? "ran" : "not needed", rc.rc, rc.healthyAfter ? "healthy" : "STILL UNHEALTHY", rc.dioGrowth);
	return rc.have;
}

// The selector body (n1c_mode_trial). Success unless the arguments are malformed; the verdict is inside *o.
uint32_t modeTrial(uint64_t row, uint64_t dwellMs, uint64_t flags, struct n48n_mode_result *o) {
	if (o == nullptr || !mt_ensure_lock()) return n48scan::kNoResources;
	n48mt::run_trial(kMtHw, gMt, mt_timing(), row > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)row, dwellMs > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)dwellMs, flags > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)flags, o);
	N48LOG("mode-trial: row %u dwell %u ms -> %s (verdict %u, deny %u, flags %#x): wrote %u, skipped %u; watchdog %u; latched %u",
	       o->row, o->dwell_req_ms, o->verdict == N48N_MODE_V_PASS ? "PASS" : o->verdict == N48N_MODE_V_DENIED ? "DENIED" : "FAILED", o->verdict, o->deny, o->flags, o->nwrite, o->nskip, o->wd_fired, o->latched);
	N48LOG("mode-trial: OTG %u -> %u mHz (expected %u), after %u mHz; DIO fifo %u -> %u count %u -> %u; registers still differing %u",
	       o->rate_before_mhz, o->rate_trial_mhz, o->rate_expect_mhz, o->rate_after_mhz, o->fifo_before, o->fifo_after, o->errcnt_before, o->errcnt_after, o->mismatch);
	N48LOG("mode-trial: underflow HUBP %#x/%#x/%#x OPTC %#x/%#x/%#x (before/settle/after), max status %u timeout %u, %u samples, %u clears, first at %u ms",
	       o->ext.uf_hubp_before, o->ext.uf_hubp_settle, o->ext.uf_hubp_after, o->ext.uf_optc_before, o->ext.uf_optc_settle, o->ext.uf_optc_after, o->ext.uf_hubp_max, o->ext.uf_timeout_max, o->ext.uf_samples, o->ext.uf_clears, o->ext.uf_first_ms);
	N48LOG("mode-trial: resync runs %u attempts %u rc %u/%u blank %u us unblank %u us DIO growth %u restore rc %u / %u; lock rc %u wait %u us pending %#x vert max %u db %#x/%#x/%#x drr %u/%u/%u",
	       o->ext.rs_runs, o->ext.rs_attempts, o->ext.rs_rc_first, o->ext.rs_rc_last, o->ext.rs_blank_us, o->ext.rs_unblank_us, o->ext.rs_dio_errs, o->ext.rs_restore_rc, o->ext.rs_restore_dio_errs,
	       o->ext.lk_rc, o->ext.lk_wait_us, o->ext.lk_pending_seen, o->ext.lk_vert_max, o->ext.lk_db_before, o->ext.lk_db_during, o->ext.lk_db_after, o->ext.lk_drr_before, o->ext.lk_drr_during, o->ext.lk_drr_after);
	if (o->row == N48N_MODE_ROW_120 || o->ext.hold_state != 0u || o->ext.hold_pre != 0u)
		N48LOG("mode-trial: clock hold %s: raise rc %u (pre %u) in %u ms, DFS %u/%u kHz; release rc %u in %u ms, DFS %u/%u kHz; %u DAL msgs; sampled %u (lost %u), lowest %u/%u kHz; flags %#x",
		       n48dal::hold_state_name(o->ext.hold_state), o->ext.hold_rc, o->ext.hold_pre, o->ext.hold_raise_ms, o->ext.hold_khz_raised[0], o->ext.hold_khz_raised[1], o->ext.hold_rel_rc, o->ext.hold_release_ms,
		       o->ext.hold_khz_released[0], o->ext.hold_khz_released[1], o->ext.hold_msgs, o->ext.hold_samples, o->ext.hold_lost, o->ext.hold_dfs_min[0], o->ext.hold_dfs_min[1], o->ext.hold_flags);
	return n48scan::kOk;
}

bool modeTrialBusy() { return __atomic_load_n(&gMt.busy, __ATOMIC_SEQ_CST) != 0u; }

// ---- 0.0.609: the HELD mode (row 120): the launcher, the runner thread, the release and the session hook -----------------------------------------------------------------------------------------
// The whole trial runs on ONE kernel thread (mt_hold_runner) with a HoldCfg; the selector's caller only starts it and waits for the entry snapshot (or the end, when the trial was denied or failed). Nothing here
// takes a lock but through mt_lock / mt_unlock (the copies of the result buffers); the waits are IOSleep with nothing held.
static n48mt::HoldCfg gMtHoldCfg { 0u, 0u };
static void mt_hold_runner(void *, wait_result_t) {
	const n48mt::HoldCfg cfg = gMtHoldCfg;
	n48mt::run_trial(kMtHw, gMt, mt_timing(), N48N_MODE_ROW_120, cfg.maxMs, 0u, &gMt.heldFinal, &cfg);
	N48LOG("mode-hold: ENDED verdict %u (end %u, flags %#x, hold_flags %#x): held %u ms, %u register(s) still differ, latched %u", gMt.heldFinal.verdict, gMt.heldFinal.hold_end, gMt.heldFinal.flags, gMt.heldFinal.hold_flags,
	       gMt.heldFinal.dwell_done_ms, gMt.heldFinal.mismatch, gMt.heldFinal.latched);
	n48mt::hold_runner_done(gMt);                                   // the result buffer is final; the hold slot is free
	__atomic_fetch_sub(&gMtHoldAlive, 1u, __ATOMIC_RELEASE);        // the thread touches nothing of ours after this
}
static void mt_result_denied(struct n48n_mode_result *o, uint32_t why) {
	n48mt::detail::zero(o);
	o->row = N48N_MODE_ROW_120;
	n48mt::detail::deny(kMtHw, gMt, o, why);
}
static void mt_copy_result(struct n48n_mode_result *o, const struct n48n_mode_result &src) { mt_lock(nullptr); *o = src; mt_unlock(nullptr); }
uint32_t modeHold(uint32_t sess, uint64_t maxMs, uint64_t flags, struct n48n_mode_result *o) {
	if (o == nullptr || !mt_ensure_lock()) return n48scan::kNoResources;
	const uint64_t want = maxMs == 0ull ? (uint64_t)N48N_MODE_DEFAULT_HOLD_MS : maxMs;
	const n48mt::HoldCfg cfg { want > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)want, flags > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)flags };
	if (!n48mt::hold_cfg_ok(cfg)) { mt_result_denied(o, N48N_MODE_D_BAD_DWELL); return n48scan::kOk; }
	if (!n48mt::hold_launch_claim(gMt)) { mt_result_denied(o, N48N_MODE_D_BUSY); return n48scan::kOk; }   // one hold at a time, from the claim until its runner is done
	n48mt::hold_launch_prepare(gMt, sess, cfg);
	gMtHoldCfg = cfg;
	__atomic_fetch_add(&gMtHoldAlive, 1u, __ATOMIC_ACQ_REL);
	thread_t th = nullptr;
	const kern_return_t kr = kernel_thread_start(&mt_hold_runner, nullptr, &th);
	if (kr != KERN_SUCCESS) {
		__atomic_fetch_sub(&gMtHoldAlive, 1u, __ATOMIC_ACQ_REL);
		n48mt::hold_runner_done(gMt);
		N48LOG("mode-hold: the hold thread did not start (kr %#x)", kr);
		mt_result_denied(o, N48N_MODE_D_WATCHDOG);
		return n48scan::kOk;
	}
	thread_deallocate(th);
	N48LOG("mode-hold: STARTED by session %u: max %u ms, flags %#x (HANDOFF %d)", sess, cfg.maxMs, cfg.flags, (cfg.flags & N48N_HOLD_F_HANDOFF) != 0u ? 1 : 0);
	// Wait (bounded, ~90 s: the pre-hold phases take ~12 s) for the hold to be UP or for the trial to have ended without one.
	for (uint32_t i = 0; i < 9000u; i++) {
		if (n48mt::ld(gMt.held) != 0u) { mt_copy_result(o, gMt.heldEntry); return n48scan::kOk; }
		if (n48mt::ld(gMt.runnerDone) != 0u) { mt_copy_result(o, gMt.heldFinal); return n48scan::kOk; }
		/*nolock*/ IOSleep(10);
	}
	N48LOG("mode-hold: the hold neither entered nor ended in 90 s (it continues on its own; the watchdog bounds it)");
	return n48scan::kBusy;
}
// ModeRelease: end a running hold (unless QUERY), wait for the whole end sequence, hand back the FINAL result; DENIED (not held) when no hold ever ran this boot.
uint32_t modeRelease(uint64_t flags, struct n48n_mode_result *o) {
	if (o == nullptr || !mt_ensure_lock()) return n48scan::kNoResources;
	if ((flags & ~(uint64_t)N48N_REL_F_MASK) != 0ull) return n48scan::kBadArg;
	const bool query = (flags & N48N_REL_F_QUERY) != 0ull;
	if (n48mt::ld(gMt.held) != 0u && query) { mt_copy_result(o, gMt.heldEntry); return n48scan::kOk; }
	if (n48mt::ld(gMt.launching) == 0u) {                            // no hold is running: the last one's final result, if there was one
		if (gMt.heldFinal.verdict == 0u) { mt_result_denied(o, N48N_MODE_D_NOT_HELD); return n48scan::kOk; }
		mt_copy_result(o, gMt.heldFinal);
		return n48scan::kOk;
	}
	if (query) { mt_result_denied(o, N48N_MODE_D_NOT_HELD); return n48scan::kOk; }   // a hold is starting but is not up yet
	if (n48mt::hold_request_release(gMt)) N48LOG("mode-hold: RELEASE requested");
	for (uint32_t i = 0; i < 4000u; i++) {                           // the end sequence is ~10 s at the worst
		if (n48mt::ld(gMt.runnerDone) != 0u && n48mt::ld(gMt.launching) == 0u) { mt_copy_result(o, gMt.heldFinal); return n48scan::kOk; }
		/*nolock*/ IOSleep(10);
	}
	return n48scan::kBusy;
}
// The native client's close (n1c_close, AFTER its scanout plane was put back, under the client lock): atomic words only.
void modeHoldSessionClosed(uint32_t sess) { n48mt::hold_session_closed(kMtHw, gMt, sess); }
bool modeHoldActive() { return n48mt::ld(gMt.launching) != 0u || n48mt::ld(gMt.held) != 0u; }

// Kext stop: end a running trial (restore + ABORT), put the golden set back, wait (bounded) for the watchdog threads. Called first thing in scanShutdown, before the registers are unmapped.
static void mt_shutdown() {
	if (gMtLock == nullptr) return;
	n48mt::request_abort(kMtHw, gMt);
	n48mt::hold_abort_scan(kMtHw, gMt);    // 0.0.609: as in mt_emergency_restore
	const bool idle = n48mt::wait_idle(kMtHw, gMt, 20000u, 50u);
	if (gMt.everWrote) {
		uint32_t restored = 0; bool have = false;
		const uint32_t bad = n48mt::restore_from_golden(kMtHw, gMt, &restored, &have);
		// 0.0.607 (P4 caller: the kext stop): after a VERIFIED golden restore (registers equal and the back end healthy) a clock hold still HELD is released; otherwise it stays (STUCK).
		uint32_t prc = 0u;
		const bool healthy = n48mt::health_now(kMtHw, &prc);
		if (have) (void)n48mt::emergency_release(kMtHw, gMt, mt_timing(), idle, bad, healthy, 0u);
	}
	for (uint32_t i = 0; i < 2000u && (__atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE) != 0u || __atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE) != 0u); i++) /*nolock*/ IOSleep(1);
	if (__atomic_load_n(&gMtWdAlive, __ATOMIC_ACQUIRE) != 0u) N48LOG("mode-trial: kext stop: a watchdog thread is still alive after 2 s");
	if (__atomic_load_n(&gMtHoldAlive, __ATOMIC_ACQUIRE) != 0u) N48LOG("mode-trial: kext stop: a hold thread is still alive after 2 s");
}

// Kext stop: restore the console (idempotent) and wait, bounded at 2 s, for every watchdog thread to finish before the caller unmaps the registers.
void scanShutdown() {
	mt_shutdown();                           // build 0.0.605: the mode trial ends and the golden set is put back first (before the scan-lock early return: a trial needs no scan lock)
	if (gScan.lock == nullptr) return;
	uint64_t r[2] = { 1, 0 };
	(void)scan_restore("kext stop", true, 0u, r);
	for (uint32_t i = 0; i < 2000u && __atomic_load_n(&gScan.watchdogAlive, __ATOMIC_ACQUIRE) != 0u; i++) /*nolock*/ IOSleep(1);
	if (__atomic_load_n(&gScan.watchdogAlive, __ATOMIC_ACQUIRE) != 0u) N48LOG("n48scan: kext stop: a watchdog thread is still alive after 2 s");
}

}  // namespace n48dcn
