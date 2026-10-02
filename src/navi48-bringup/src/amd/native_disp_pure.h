//
//  native_disp_pure.h - the pure half of #11 step 11h.2 (kext 0.0.613): every DECISION of the display pipe, judged from arguments alone. No kernel header:
//  tests/native_disp_test.cpp compiles this very file (and amd/native_disp_flow.h, the sequencing built on it) and drives it, with planted breaks
//  (tests/native_disp_plant.sh).
//
//  Design: notes/design/NATIVE-S4-M11H.md, section 3.4 (the aux alternative: Navi48DisplayPipe in the aux kext 0.0.3) and the "11h.1 RE facts" section, which
//  CORRECTS the memo and is binding where they differ. Layout facts come ONLY from that note, the memo, tools/native/ioaccel-layout (summary.md and the
//  vtables) and the repo's own pure headers; nothing here was re-derived from an Apple binary. Every offset below is a NAMED CONSTANT with its source.
//
//  Everything is behind boot-arg navi48-metal-disp=1 (latched once, at the first use). OFF = 0.0.612: the ops table the bring-up kext publishes is the
//  120-byte ABI-1 table (ops_shape), the action bound stays 82 (action_admitted), no native exemption is added (native_exempt), the factory mask is the
//  boot-arg's (fact_mask), and no other decision in this file is reached.
//
#pragma once
#include <stdint.h>
#include "../Navi48MetalOps.h"
#include "native_metal_pure.h"   // the IOAccelConfig store table (accel_layout_ok reads the SAME constants the populate hook writes)

namespace n48disp {

// ---- return codes -------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kIoOk = 0u;
constexpr uint32_t kWillPerform = 0xE00002D8u;   // kIOReturnNotReady: the ONLY submit value that both reaches performTransaction and keeps the event machine's fence out (M11H section 0)

// The verb statuses (out[0] of every display verb; the IOReturn of the verb itself stays success, like fbname: a refusal is readable from the scalars).
enum Status : uint32_t {
    kOk = 0, kBadArg = 1, kOff = 2, kNoAccel = 3, kAccelLayout = 4, kFactsOff = 5, kHasPipes = 6, kNoPipe = 7, kPipeNotOurs = 8, kPipeMismatch = 9,
    kProbeFailed = 10, kAlready = 11, kWriteFailed = 12, kNoDisplayMachine = 13, kCapsFailed = 14, kBusy = 15, kNoEventMachine = 16,
    kNullPipe = 17, kStatusCount = 18     // 0.0.615: kNullPipe = the display machine counts a pipe but its slot holds NULL (the family's init failed): reboot before any WindowServer restart
};
constexpr const char *status_name(uint32_t s) {
    return s == kOk ? "OK" : s == kBadArg ? "bad argument" : s == kOff ? "display is OFF (boot-arg navi48-metal-disp is not 1)" :
           s == kNoAccel ? "no Navi48Accelerator under our nub (publish the nub first)" : s == kAccelLayout ? "the accelerator object failed its positive controls" :
           s == kFactsOff ? "the resource fact bits are not on" : s == kHasPipes ? "the display machine already holds pipes that are not a verified Navi48DisplayPipe" :
           s == kNoPipe ? "no pipe exists" : s == kPipeNotOurs ? "the pipe is not a Navi48DisplayPipe of ours" :
           s == kPipeMismatch ? "the pipe does not belong to this accelerator / display machine / RDNA4FB" :
           s == kProbeFailed ? "requestProbe did not succeed" : s == kAlready ? "already adopted (verified again)" : s == kWriteFailed ? "the write did not read back" :
           s == kNoDisplayMachine ? "accel+0x378 is not a Navi48DisplayMachine" : s == kCapsFailed ? "the capabilities property could not be set" :
           s == kBusy ? "another display verb is running" : s == kNoEventMachine ? "accel+0x380 is not a Navi48EventMachine" :
           s == kNullPipe ? "the family stored a NULL pipe (its init failed): reboot before any WindowServer restart" : "unknown";
}

// ---- the boot-arg latch (same shape as native_open_policy_pure.h's metal-ws latch) -------------------------------------------------------------------------
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }

// ---- the verbs (accel actions) ------------------------------------------------------------------------------------------------------------------
// Actions 0..82 exist in 0.0.612. The new ones open ONLY when the boot-arg is latched ON; with it OFF the bound is exactly 82 again.
constexpr uint32_t kLastOldAction = 82u;
constexpr uint32_t kActFbname = 78u;                               // existing (0.0.308), dcn/navi48_fbname.cpp
constexpr uint32_t kActAdopt = 83u, kActArm = 84u, kActStat = 85u, kActStamps = 86u, kActShortcut = 87u;
constexpr uint32_t kActAgdc = 88u;                                 // 0.0.614: `pipeagdc`, the native AGDC service (amd/native_agdc_pure.h; answered by DisplayPipeGuard.cpp, not by n48disp_verb)
constexpr uint32_t kActVbl = 89u;                                  // 0.0.618: `pipevbl [0|1]`, the vblank-timestamp switch (V2)
constexpr uint32_t kActReload = 90u;                               // 0.0.619: `pipereload [0|1]`, the operator restart window (R1): 0 (the default) opens it, 1 only reads it
constexpr uint32_t kLastAction = 90u;
constexpr bool action_admitted(bool latchOn, uint32_t action) { return action <= kLastOldAction || (latchOn && action <= kLastAction); }
constexpr bool is_new_action(uint32_t action) { return action > kLastOldAction && action <= kLastAction; }
constexpr bool is_pipe_verb(uint32_t action) { return (action >= kActAdopt && action <= kActShortcut) || action == kActVbl || action == kActReload; }   // the verbs n48disp_verb answers (83..87, 89 and 90; 88 is not one of them)
// The native-boot exemption (B5). On a native boot every accel verb but action 0 is refused (native_s1b_refuse) except the table in dcn/navi48_scanout_pure.h.
// With the latch ON this adds EXACTLY: fbname 0|1, the five display verbs and pipeagdc 0|1 with their legal arguments. None of them touches GFX/VM state or a register:
// fbname swaps an OSMetaClass name pointer, adopt asks the accelerator for its normal probe, arm writes one config byte, stat/stamps only read, shortcut flips a flag,
// pipeagdc (0.0.614) constructs Apple's AGDC object on a vtable copy (memory only: no GPU register, no page table) and starts it.
constexpr bool verb_args_ok(uint32_t action, uint64_t arg) {       // the legal (action, argument) pairs of fbname and the five display verbs
    return (action == kActFbname && arg <= 1ull) || (action == kActAdopt && arg == 0ull) || (action == kActArm && arg <= 1ull) ||
           (action == kActStat && arg <= 4ull) || (action == kActStamps && arg == 0ull) || (action == kActShortcut && arg <= 1ull) ||
           (action == kActAgdc && arg <= 1ull) || (action == kActVbl && arg <= 1ull) || (action == kActReload && arg <= 1ull);
}
constexpr bool native_exempt(bool latchOn, uint32_t action, uint64_t arg) { return latchOn && verb_args_ok(action, arg); }

// ---- the ops table the bring-up kext publishes (ABI 2 only when the latch is ON) -----------------------------------------------------------------------------
// OFF: the ABI-1 shape (abi 1, 120 bytes, flags 0): the aux kext then takes every 0.0.2 default. ON: abi 2, 144 bytes, N48_DISP_F_ON.
struct OpsShape { uint32_t abi, size, dispFlags; };
constexpr OpsShape ops_shape(bool latchOn) {
    return latchOn ? OpsShape{ 2u, N48_METAL_OPS_V2, N48_DISP_F_ON } : OpsShape{ 1u, N48_METAL_OPS_MIN, 0u };
}

// ---- the factory mask -------------------------------------------------------------------------------------------------------------------------------------
// The display pipe's init asks the accelerator for a resource object (IOAccelDisplayPipe::init -> createResource(NULL, 0x40), which stores it and sets a bit on it with no NULL
// check: 11h.1 correction 1), so RESOURCE, SYSMEMORY and VIDMEMORY must be on BEFORE the pipe is created. adopt turns them on (an OR onto the boot-arg's mask, only with the latch ON).
constexpr uint32_t kNeedFacts = N48_FACT_RESOURCE | N48_FACT_SYSMEMORY | N48_FACT_VIDMEMORY;
constexpr bool facts_ok(uint32_t mask) { return (mask & kNeedFacts) == kNeedFacts; }
constexpr uint32_t fact_mask(bool latchOn, uint32_t baseMask, uint32_t runtimeBits) { return latchOn ? (baseMask | (runtimeBits & kNeedFacts)) : baseMask; }

// ---- layout constants (each with its source) -----------------------------------------------------------------------------------------------------------------
// IOAccelDisplayPipe (11h.1 F1; size 0x318 from ioaccel-layout/summary.md)
constexpr uint32_t kPipeSize = 0x318u;
constexpr uint32_t kPipeAccel = 0x88u, kPipeDm = 0x90u, kPipeFb = 0x98u, kPipeFbRes = 0xe0u;   // init, 0x145cb31a / b321 / b32b / b414
constexpr uint32_t kPipeActive = 0x298u;                                                      // isActive (0x145cc042); slot 267 sets it (11h.1)
// IOGraphicsAccelerator2 (the memo, section 3.2 and section 0; size 0xdd8 from ioaccel-layout)
constexpr uint32_t kAccelSize = 0xdd8u;
constexpr uint32_t kAccelProvider = 0x368u, kAccelDm = 0x378u, kAccelEm = 0x380u;
constexpr uint32_t kAccelCfgBase = 0xc88u;                                                    // the IOAccelConfig: +0x47 is accel+0xccf, +0x28 is accel+0xcb0 (the memo)
constexpr uint32_t kAccelPipeGate = kAccelCfgBase + n48metal::kOffPipeUC;                     // 0xccf: read only by the type-4 user-client gate (0x145c01a8)
constexpr uint32_t kAccelCfgF0 = kAccelCfgBase + n48metal::kOffF0;                            // 0xc90: our populate stores 0x480000 here
constexpr uint32_t kAccelCfgF4 = kAccelCfgBase + n48metal::kOffF4;                            // 0xcb0: our populate stores 0x100000000008 here (low dword 8 = max pipes)
static_assert(kAccelPipeGate == 0xccfu && kAccelCfgF4 == 0xcb0u, "the config base matches the memo's accel+0xccf / accel+0xcb0");
// IOAccelDisplayMachine (the memo section 3.2 / 11h.1 correction 4)
constexpr uint32_t kDmPipes = 0x88u, kDmCount = 0x108u;
// IOAccelEventMachineFast2 (size 0xd30 from ioaccel-layout). SUSPECTED layout: the 11h.1 note names em+0x30 (the stamp count) and +0xf8 / +0xfc (completed / submitted) but no
// array base or stride for per-stamp records, so `pipe stamps` reads exactly these three words and walks nothing.
constexpr uint32_t kEmSize = 0xd30u;
constexpr uint32_t kEmCount = 0x30u, kEmDone = 0xf8u, kEmSub = 0xfcu;
// The display transaction (11h.1 F2; size 0x1b8)
constexpr uint32_t kTxnPlanes = 0x38u, kTxnDirty = 0x48u, kTxnStatus = 0x58u;
constexpr uint32_t kPlaneSurf = 0x20u, kPlaneRes = 0x30u;                                      // entry+0x20: IOSurface*[2]; entry+0x30: IOAccelResource2*[2]
constexpr uint64_t kDirtyPlane0 = 1ull;                                                        // txn+0x48 bit 0: plane 0 args present
// IOSurface, for this build only (11h.1 F2)
constexpr uint32_t kSurfW = 0x58u, kSurfH = 0x60u, kSurfBpr = 0x68u, kSurfBpe = 0x70u, kSurfElemW = 0x72u, kSurfBase = 0x78u, kSurfFmt = 0x80u, kSurfPlanes = 0x98u;
constexpr uint32_t kSurfUnk88 = 0x88u;      // named by the 11h.1 note with +0x78 / +0x98 ("log and refuse nonzero"), but given no meaning in its table: LOGGED, never decided on
constexpr uint32_t kSurfFlags = 0x3dau;     // bit 2 (mask 4) = sysMemOnly (resource type 0x80, else 0xC0)
// IOAccelResource2 (11h.1 F2 / F3)
constexpr uint32_t kResShortcutFlag = 0xeu, kResPrepared = 0x70u, kResSysMem = 0x80u, kResW = 0xb0u, kResH = 0xb2u, kResBpr = 0xc0u, kResDc = 0xe0u;
constexpr uint8_t  kResShortcutBit = 0x10u;
constexpr uint32_t kDcSurf = 0x10u;         // the device cache's IOSurface
constexpr uint32_t kSmFlags = 0xcu, kSmLen = 0x40u, kSmMd = 0xd0u;   // IOAccelSysMemory: +0xc bit 4 guards IOAccelVidMemoryList::moveMemoryToHead; +0x40 length; +0xd0 the memory descriptor
constexpr uint8_t  kSmBit4 = 0x04u;

// ---- kernel pointers and field ranges ----------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kKernelHalf = 0xffffff7000000000ull;       // the same bound DisplayPipeGuard.cpp's dpg_kptr uses
constexpr bool kptr_ok(uint64_t p) { return p >= kKernelHalf; }
// An offset+width read inside an object of a known size (the layout size from ioaccel-layout), overflow-safe.
constexpr bool in_object(uint64_t off, uint64_t width, uint64_t size) { return width != 0ull && off <= size && width <= size - off; }
static_assert(in_object(kDmCount, 4, 0x178) && !in_object(0x178, 1, 0x178), "in_object edge cases");

// ---- the accelerator's positive controls ---------------------------------------------------------------------------------------------------------------------
// accel+0xc88 is where OUR populate hook wrote the IOAccelConfig: +0x08 = 0x480000 and +0x28's low dword = 8 are the two values it stored (native_metal_pure.h's table). If both are
// there, accel+0xccf is the +0x47 byte; the byte itself must be 0 or 1 (0 = headless / disarmed, 1 = armed).
constexpr bool accel_layout_ok(uint32_t atCfgF0, uint32_t atCfgF4Low, uint8_t pipeGate) {
    return atCfgF0 == 0x480000u && atCfgF4Low == 8u && pipeGate <= 1u;
}
static_assert(n48metal::kCfgStores[1].off == n48metal::kOffF0 && n48metal::kCfgStores[1].value == 0x480000ull, "accel_layout_ok's +0x08 constant is the populate table's");
static_assert(n48metal::kCfgStores[6].off == n48metal::kOffF4 && (n48metal::kCfgStores[6].value & 0xffffffffull) == 8ull, "accel_layout_ok's +0x28 constant is the populate table's");

// ---- adopt: what the read-only verification found (filled by the kernel side, judged here) ----------------------------------------------------------------
struct PipeProbe {
    bool     accelOk;      // the accelerator is a Navi48Accelerator whose provider is our published nub
    bool     dmReadable;   // accel+0x378 is a kernel pointer whose class is Navi48DisplayMachine
    uint32_t count;        // dm+0x108
    bool     havePipe;     // dm+0x88[0] is a kernel pointer
    bool     nullPipe;     // 0.0.615: dm+0x108 >= 1 and dm+0x88[0] reads as exactly NULL (found_framebuffer stores a failed init's NULL and still counts it: 11h.1 correction 4)
    bool     classOurs;    // its class name is Navi48DisplayPipe
    bool     traced;       // the aux kext's newDisplayPipe trace named this pointer as its own (b == 1)
    bool     backAccel, backDm, backFb;   // pipe+0x88 == the accelerator, +0x90 == the display machine, +0x98 == the RDNA4FB service
    bool     fbOurs;       // the framebuffer the registry names is RDNA4FB / AMDRDNA4FB
};
constexpr bool pipe_ok(const PipeProbe &p) {
    return p.accelOk && p.dmReadable && p.count == 1u && p.havePipe && p.classOurs && p.traced && p.backAccel && p.backDm && p.backFb && p.fbOurs;
}
// The first failing check, as a status (kOk only when pipe_ok).
constexpr uint32_t pipe_verdict(const PipeProbe &p) {
    return !p.accelOk ? kNoAccel : !p.dmReadable ? kNoDisplayMachine : p.count == 0u ? kNoPipe : p.nullPipe ? kNullPipe : !p.havePipe ? kNoPipe : (!p.classOurs || !p.traced) ? kPipeNotOurs :
           (p.count != 1u || !p.backAccel || !p.backDm || !p.backFb || !p.fbOurs) ? kPipeMismatch : kOk;
}
// Before adopt asks for the probe: a display machine that already holds pipes is refused unless they are already the verified pipe (then the verb reports "already").
constexpr uint32_t adopt_precheck(const PipeProbe &before) {
    return !before.accelOk ? kNoAccel : !before.dmReadable ? kNoDisplayMachine : before.count == 0u ? kOk : before.nullPipe ? kNullPipe : pipe_ok(before) ? kAlready : kHasPipes;
}

// ---- arm --------------------------------------------------------------------------------------------------------------------------------------------------------------
// `pipe arm 1|0`. Arm 0 is ALWAYS allowed, before every other test (it is the recovery path: it must work with the pipe gone, the facts off, anything). Arm 1 needs the latch, the
// resource facts, a pipe that exists AND is verified as ours right now, and the accelerator's positive controls.
struct ArmIn { bool latchOn, factsOk, pipeOk, accelLayoutOk; };
constexpr uint32_t arm_decide(uint64_t arg, const ArmIn &a) {
    return arg > 1ull ? kBadArg : arg == 0ull ? kOk : !a.latchOn ? kOff : !a.factsOk ? kFactsOff : !a.pipeOk ? kNoPipe : !a.accelLayoutOk ? kAccelLayout : kOk;
}

// ---- 0.0.615: the crash-loop guard (G1), the PCI-device gate (P1) and the verb flags word (W1) ---------------------------------------------------------------------------
// G1: while armed, slot 267 (initFramebufferResource) runs once per displayModeDidChange, i.e. once per WindowServer start that reaches the pipe (a healthy start produces 1-2).
// 0.0.617 (K3): THREE calls inside 120 s trip the guard WHETHER OR NOT performs happened (m11h5-3: WindowServer crashed in its Metal compositor AFTER presenting, so the 0.0.615 rule "no perform since the
// arm" never tripped and the crash loop ran into the userspace watchdog). idx = the 0-based index of this call since the arm, oldestNs = the uptime stamp of the call two before it (the
// third-most-recent call including this one, stamped by the same caller).
constexpr uint32_t kGuardCalls = 3u;
constexpr uint64_t kGuardWindowNs = 120ull * 1000000000ull;
constexpr bool guard_trip(uint32_t idx, uint64_t nowNs, uint64_t oldestNs) {
    return idx + 1u >= kGuardCalls && nowNs >= oldestNs && nowNs - oldestNs <= kGuardWindowNs;
}
// 0.0.617 (K1/K2/K4): why the pipe was disarmed WITHOUT the operator. Kept until the next explicit `pipearm 1` and shown by `pipe stat` ("auto-disarmed: <cause>").
enum AutoCause : uint32_t { kAdNone = 0, kAdGuard = 1, kAdWsClose = 2, kAdHung = 3, kAdCount = 4 };
constexpr const char *auto_cause_name(uint32_t c) {
    return c == kAdGuard ? "WindowServer restart loop (slot 267 x3 within 120 s)" : c == kAdWsClose ? "the WindowServer GPU client closed while armed" : c == kAdHung ? "the GPU was declared HUNG while armed" : "none";
}
// ---- 0.0.619 (R1): the operator restart window --------------------------------------------------------------------------------------------------------------------------
// A DELIBERATE WindowServer restart (`accel pipereload`, then `killall -9 WindowServer`) looks to K1 (the uid-88 client closes while armed) and K3 (three slot-267 calls inside 120 s) exactly like a crash.
// The verb opens a ONE-SHOT window of 15 s on the uptime clock. Inside it: (a) the next uid-88 client close while armed does NOT disarm, (b) slot-267 calls are NOT counted by the K3 guard.
// The window closes when the first slot-267 of the NEW WindowServer has been seen after a tolerated close AND the pipe is still armed, or when 15 s have passed. The HUNG latch (K2) is never
// affected, and while HUNG the window is not honoured at all (a close or a slot-267 then takes the 0.0.617 path). Anything unreadable fails towards containment: a clock that went backwards = no window.
constexpr uint64_t kReloadWindowNs = 15ull * 1000000000ull;
constexpr bool reload_live(uint32_t open, uint64_t openNs, uint64_t nowNs) { return open != 0u && nowNs >= openNs && nowNs - openNs < kReloadWindowNs; }   // exactly 15 s = expired
constexpr bool reload_honoured(bool live, bool hung) { return live && !hung; }
enum ReloadEv : uint32_t { kRwOpened = 0, kRwTolerated = 1, kRwClosed267 = 2, kRwExpired = 3, kRwCount = 4 };
// The state reported by the verb: 0 closed, 1 open and waiting for the close, 2 open and the close has been tolerated (waiting for the new client's first slot 267).
constexpr uint32_t reload_state(bool live, bool closeSeen) { return live ? (closeSeen ? 2u : 1u) : 0u; }
constexpr const char *reload_state_name(uint32_t s) { return s == 1u ? "OPEN, waiting for the old client's close" : s == 2u ? "OPEN, close tolerated, waiting for the new client's first slot 267" : "closed"; }

// P1: the aux kext's display-machine walk is handed the PCI device ONLY with the latch on AND the resource facts on: a pipe created without them dereferences a NULL resource (11h.1 correction 1),
// and any requestProbe (ours or anybody's) before `pipeadopt` would create one. Otherwise the ops hook answers NULL and the aux walk falls back to the nub (0.0.612 behaviour: finds nothing).
constexpr bool pci_admit(bool latchOn, uint32_t factoryMask) { return latchOn && facts_ok(factoryMask); }
// W1: the flags word every pipe verb reports. Bit 5 (32) = the BAR0 kernel mapping is write-combined (perform's CPU copy runs ~17x faster than through an uncached mapping, an earlier analysis).
// 0.0.617 (K4): bit 64 = auto-disarmed since the last explicit arm, bits 8..11 = the AutoCause.
constexpr uint32_t kFlagOn = 1u, kFlagAdopted = 2u, kFlagArmed = 4u, kFlagCaps = 8u, kFlagShortcut = 16u, kFlagBar0Wc = 32u, kFlagAutoDisarmed = 64u, kFlagCauseShift = 8u;
constexpr uint32_t disp_flags(bool on, bool adopted, bool armed, bool caps, bool shortcut, bool bar0Wc, uint32_t autoCause = 0u) {
    return (on ? kFlagOn : 0u) | (adopted ? kFlagAdopted : 0u) | (armed ? kFlagArmed : 0u) | (caps ? kFlagCaps : 0u) | (shortcut ? kFlagShortcut : 0u) | (bar0Wc ? kFlagBar0Wc : 0u) |
           (autoCause != 0u && autoCause < kAdCount ? (kFlagAutoDisarmed | (autoCause << kFlagCauseShift)) : 0u);
}
// K1: the WindowServer GPU client (admitted by the uid-88 rule, i.e. NOT an administrator) closed or died. Only that client disarms: operator tools (root) open and close N48N all the time.
constexpr bool ws_close_disarms(bool latchOn, bool adminClient) { return latchOn && !adminClient; }

// ---- the submit / isComplete results (slot 279 / 278) ---------------------------------------------------------------------------------------------------------
// submit passes the transaction status through when it is nonzero (a transaction whose prepare failed must never reach perform), else the "will perform" code (11h.1 F2).
constexpr uint32_t submit_result(bool statusKnown, uint32_t txnStatus) { return (statusKnown && txnStatus != 0u) ? txnStatus : kWillPerform; }
constexpr uint64_t iscomplete_result() { return 1ull; }   // bool true: the copy is synchronous (pipeshim's proven dpg_isComplete)

// ---- perform: the v1 CPU copy of plane 0 into the console buffer ------------------------------------------------------------------------------------------------
// Only 32 bpp BGRA, baseOffset 0, one plane, 4 bytes per element (11h.1 F2: the family never records the pixel format in the resource; it is read from IOSurface+0x80).
constexpr uint32_t kFmtBGRA = 0x42475241u;   // the FourCC 'BGRA' as an integer (kCVPixelFormatType_32BGRA)
constexpr bool source_admitted(uint64_t baseOffset, uint32_t planeCount, uint32_t pixelFormat, uint32_t bytesPerElement) {
    return baseOffset == 0ull && planeCount <= 1u && pixelFormat == kFmtBGRA && bytesPerElement == 4u;
}
constexpr uint32_t kScratchBytes = 16384u;    // one row at a time: width * 4 must fit
enum BoundsVerdict : uint32_t { kBOk = 0, kBBadArg = 1, kBGeom = 2, kBStride = 3, kBDest = 4, kBSrc = 5 };
struct BoundsIn {
    uint64_t width, height;           // the surface's (32 bpp, so a row is width * 4 bytes)
    uint64_t srcBytesPerRow, srcBytes;   // the source stride and the source's length (sysmem+0x40)
    uint64_t consoleWidth, consoleHeight, consoleStride, consoleBytes;   // getConsoleInfo: width, height, rowBytes, rowBytes * height
};
// Every product is 64-bit from u32-range operands (the caller passes values read as at most 64 bits; anything above 2^32 is refused up front so a product cannot wrap).
constexpr uint32_t bounds_check(const BoundsIn &b) {
    if (b.width == 0ull || b.height == 0ull || b.srcBytesPerRow == 0ull || b.srcBytes == 0ull || b.consoleWidth == 0ull || b.consoleHeight == 0ull || b.consoleStride == 0ull || b.consoleBytes == 0ull)
        return kBBadArg;
    if (b.width > 0xffffffffull || b.height > 0xffffffffull || b.srcBytesPerRow > 0xffffffffull || b.consoleStride > 0xffffffffull || b.consoleHeight > 0xffffffffull) return kBBadArg;
    if (b.width != b.consoleWidth || b.height > b.consoleHeight) return kBGeom;                 // width equals the console width; never taller than the console
    const uint64_t row = b.width * 4ull;
    if (row > kScratchBytes) return kBGeom;
    if (b.srcBytesPerRow < row || b.consoleStride < row) return kBStride;                       // neither stride may be narrower than one row
    if (b.width * b.height * 4ull > b.consoleBytes) return kBDest;                              // w*h*4 within the console
    if ((b.height - 1ull) * b.consoleStride + row > b.consoleBytes) return kBDest;              // and every destination row, at the CONSOLE's stride
    if ((b.height - 1ull) * b.srcBytesPerRow + row > b.srcBytes) return kBSrc;                  // and every source row, at the source's stride
    return kBOk;
}
// The reason a perform did (or did not) copy. Counted per reason; perform itself always returns success so the 4-deep ring drains (a refused frame freezes the glass, never WindowServer).
enum PerfReason : uint32_t {
    kPfCopied = 0, kPfDisarmed = 1, kPfNoPlane = 2, kPfBadTxn = 3, kPfBadArr = 4, kPfBadSurf = 5, kPfBadRes = 6, kPfBadSrc = 7, kPfFormat = 8, kPfMismatch = 9,
    kPfNoConsole = 10, kPfGeom = 11, kPfStride = 12, kPfDest = 13, kPfSrcRange = 14, kPfBusy = 15, kPfNoScratch = 16, kPfSrcRead = 17, kPfDstWrite = 18,
    kPfNotPrepared = 19, kPfScanOwned = 20, kPfCount = 21     // 0.0.617 (K6): kPfScanOwned = a native client holds the scanout plane (its bundle flips): the v1 copy is skipped, nothing is read or wired.  0.0.616 (19): 0.0.616: the source memory descriptor is not in the prepared cache (perform never wires: IOGMD panics on an unwired readBytes, run m11h4-1)
};
constexpr const char *perf_name(uint32_t r) {
    return r == kPfCopied ? "copied" : r == kPfDisarmed ? "disarmed (completed, not copied)" : r == kPfNoPlane ? "no plane 0 in the transaction" : r == kPfBadTxn ? "txn is not a kernel pointer" :
           r == kPfBadArr ? "plane array is not a kernel pointer" : r == kPfBadSurf ? "IOSurface / resource pointer unusable" : r == kPfBadRes ? "resource fields unreadable" :
           r == kPfBadSrc ? "source memory unreadable" : r == kPfFormat ? "source not admitted (base / planes / format / element)" :
           r == kPfMismatch ? "resource and IOSurface geometry disagree" : r == kPfNoConsole ? "console region unknown" : r == kPfGeom ? "geometry refused (width / height)" :
           r == kPfStride ? "stride refused" : r == kPfDest ? "destination rows past the console" : r == kPfSrcRange ? "source rows past the source" : r == kPfBusy ? "a copy was already running" :
           r == kPfNoScratch ? "no scratch buffer" : r == kPfSrcRead ? "the source read came back short" : r == kPfDstWrite ? "the console write was refused" :
           r == kPfNotPrepared ? "the source memory descriptor was not prepared by submit (not in the cache)" :
           r == kPfScanOwned ? "the scanout plane is owned by a native client (the v1 copy is skipped)" : "unknown";
}
constexpr uint32_t perf_reason_from_bounds(uint32_t bv) {
    return bv == kBOk ? kPfCopied : bv == kBGeom || bv == kBBadArg ? kPfGeom : bv == kBStride ? kPfStride : bv == kBDest ? kPfDest : kPfSrcRange;
}
// The IOSurface and the resource must tell the same story (11h.1 reads them at independent offsets): a disagreement means an offset is wrong, so nothing is copied.
constexpr bool geometry_agrees(uint64_t sw, uint64_t sh, uint64_t sbpr, uint64_t rw, uint64_t rh, uint64_t rbpr) { return sw == rw && sh == rh && sbpr == rbpr; }


// ---- 0.0.616: the prepared-descriptor cache ---------------------------------------------------------------------------------------------------------------------------
// Run m11h4-1 (the first armed run) panicked in perform: IOMemoryDescriptor::readBytes on the plane's SysMemory descriptor (sysmem+0xd0) that nobody had prepare()d ("IOGMD: not wired for the
// IODMACommand"). perform runs on the accelerator workloop under the command gate and must never wire; so it may read ONLY a descriptor that OUR code prepared earlier, from a thread context
// that may block: the submitTransaction hook (slot 279; reached through transaction_queue_gated, which IOCommandGate::runAction runs in the CALLER's thread, 0x145cd130 -> 0x145cf034).
// The cache is 8 entries of {descriptor, state, busy, last use}. All state is atomic (the hooks take no lock). Protocol per entry (SEQ_CST on the pair that must not both miss each other):
//   Empty -> Filling (a claimant, CAS) -> Ready (md stored first, state last: release)        perform: busy++ ; state still Ready and md unchanged ? use : busy-- and refuse ; busy-- when done
//   Ready -> Retiring (evictor / teardown, CAS) ; busy must read 0 AFTER the CAS, else Ready again (a perform got in first) ; only then complete() + release() the old descriptor.
// Either perform sees Retiring (refuses) or the evictor sees busy != 0 (leaves it): never both miss. A DEAD entry (a teardown that could not drain, or the HUNG latch) is Retiring for good.
constexpr uint32_t kMdCacheN = 8u;
enum MdState : uint32_t { kMdEmpty = 0, kMdFilling = 1, kMdReady = 2, kMdRetiring = 3 };
struct MdEntry { uint64_t md; uint32_t state; uint32_t busy; uint64_t last; };
struct MdCache {
    MdEntry e[kMdCacheN];
    uint64_t tick;
    uint64_t prepared, prepFail, hits, misses, evicted, evictBlocked, full, tornDown, leaked, drainFail;   // statistics only
};
// perform: take entry i busy if md is Ready there. -1 = not in the cache (a refusal: the descriptor is never touched).
inline int mdc_acquire(MdCache &c, uint64_t md) {
    if (md == 0ull) return -1;
    for (uint32_t i = 0; i < kMdCacheN; ++i) {
        if (__atomic_load_n(&c.e[i].state, __ATOMIC_ACQUIRE) != kMdReady || __atomic_load_n(&c.e[i].md, __ATOMIC_ACQUIRE) != md) continue;
        __atomic_add_fetch(&c.e[i].busy, 1u, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&c.e[i].state, __ATOMIC_SEQ_CST) == kMdReady && __atomic_load_n(&c.e[i].md, __ATOMIC_ACQUIRE) == md) {
            __atomic_store_n(&c.e[i].last, __atomic_add_fetch(&c.tick, 1ull, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            return (int)i;
        }
        __atomic_sub_fetch(&c.e[i].busy, 1u, __ATOMIC_RELEASE);                      // lost the race with an evictor: refuse
    }
    return -1;
}
inline void mdc_release(MdCache &c, int i) { if (i >= 0 && (uint32_t)i < kMdCacheN) __atomic_sub_fetch(&c.e[i].busy, 1u, __ATOMIC_RELEASE); }
// src_read's own assertion: md is in the cache, Ready, and held busy by a perform.
inline bool mdc_held(MdCache &c, uint64_t md) {
    if (md == 0ull) return false;
    for (uint32_t i = 0; i < kMdCacheN; ++i)
        if (__atomic_load_n(&c.e[i].state, __ATOMIC_ACQUIRE) == kMdReady && __atomic_load_n(&c.e[i].md, __ATOMIC_ACQUIRE) == md && __atomic_load_n(&c.e[i].busy, __ATOMIC_ACQUIRE) != 0u) return true;
    return false;
}
// submit: a hit refreshes the LRU stamp.
inline bool mdc_touch(MdCache &c, uint64_t md) {
    for (uint32_t i = 0; i < kMdCacheN; ++i)
        if (__atomic_load_n(&c.e[i].state, __ATOMIC_ACQUIRE) == kMdReady && __atomic_load_n(&c.e[i].md, __ATOMIC_ACQUIRE) == md) { __atomic_store_n(&c.e[i].last, __atomic_add_fetch(&c.tick, 1ull, __ATOMIC_RELAXED), __ATOMIC_RELAXED); return true; }
    return false;
}
// Ready -> Retiring, only if nobody is inside a perform on it. true = the caller now owns the entry (no perform can enter, none is inside).
inline bool mdc_retire(MdCache &c, uint32_t i) {
    uint32_t exp = kMdReady;
    if (!__atomic_compare_exchange_n(&c.e[i].state, &exp, (uint32_t)kMdRetiring, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return false;
    if (__atomic_load_n(&c.e[i].busy, __ATOMIC_SEQ_CST) != 0u) { __atomic_store_n(&c.e[i].state, (uint32_t)kMdReady, __ATOMIC_SEQ_CST); return false; }
    return true;
}

// ---- slot 62 (calculateIOSurfaceDeviceCacheVRAMBytes, type 0xC0 surfaces) and the "already prepared" shortcut (11h.1 F3) ------------------------------------------------
// fact 6: the shortcut (res+0xe |= 0x10; res+0x70 = res+0x80) must not run when the SysMemory object's own +0xc bit 4 is set: family prepare() tests that bit (testb $0x4, 0xc(%rsi), re-read at
// 0x145a8ffb) to guard a call into IOAccelVidMemoryList::moveMemoryToHead through the SAME object the shortcut would bypass. SUSPECTED until the shortcut's call site is verified on a live boot;
// the gate fails CLOSED (skip the shortcut) whenever the bit cannot be read.
constexpr bool shortcut_allowed(bool sysMemFlagsKnown, bool bit4Set) { return sysMemFlagsKnown && !bit4Set; }
struct Res62In { bool smKnown; uint64_t smLen; bool surfKnown; uint64_t surfBpr; bool flagsKnown; bool bit4; bool switchOn; };
struct Res62Plan { bool handled; uint64_t allocSize, bytesPerRow; bool shortcut; bool shortcutRefused; };
// TWO independent decisions. (1) handled (the outputs): needs BOTH the allocation size (the SysMemory's length, sysmem+0x40) and the IOSurface's bytes per row (IOSurface+0x68, through the device cache at
// res+0xe0); with either unreadable the hook says "not handled" and the aux default stands (0.0.612's slot-62 behaviour), never a guess. (2) the shortcut: the switch AND shortcut_allowed, which needs only the
// SysMemory's own flags byte: it does not wait for the IOSurface to be readable (an IOSurface that is not reachable yet must not cost the shortcut, and the shortcut needs nothing from it).
constexpr Res62Plan res62_plan(const Res62In &in) {
    return Res62Plan{ in.smKnown && in.smLen != 0ull && in.surfKnown && in.surfBpr != 0ull,
                      (in.smKnown && in.smLen != 0ull && in.surfKnown && in.surfBpr != 0ull) ? in.smLen : 0ull,
                      (in.smKnown && in.smLen != 0ull && in.surfKnown && in.surfBpr != 0ull) ? in.surfBpr : 0ull,
                      in.switchOn && shortcut_allowed(in.flagsKnown, in.bit4), in.switchOn && !shortcut_allowed(in.flagsKnown, in.bit4) };
}

// ---- the stand-in memory object slot 267 returns (11h.1 F1) ------------------------------------------------------------------------------------------------------
// The family stores it at res+0x88 and calls prepare(), which maps through slot 39 (returns 0 -> map() and prepare() return false, safely). Destroy tests +0xd bit 0x10 (set: it skips
// pruneOrphanedMappings), writes 0 at +0x38, and calls slot 5 (release). +0xc gets an `orb $0x20` from init. The vtable pointer at +0 is set by the kernel side.
constexpr uint32_t kObjSize = 0x80u, kObjFlagC = 0x0cu, kObjFlagD = 0x0du, kObjZero38 = 0x38u;
constexpr uint8_t  kObjFlagDValue = 0x10u;
constexpr uint32_t kObjVtSlots = 48u, kObjSlotRelease = 5u, kObjSlotMap = 39u;
static_assert(kObjSlotRelease * 8u == 0x28u && kObjSlotMap * 8u == 0x138u, "release is the call through +0x28, map the call through +0x138");
static_assert(kObjVtSlots > kObjSlotMap && kObjZero38 + 8u <= kObjSize, "the stand-in covers every slot and field the family touches");
inline void standin_fill(uint8_t *o) {     // everything but the vtable pointer at +0
    for (uint32_t i = 8u; i < kObjSize; ++i) o[i] = 0u;
    o[kObjFlagC] = 0u;
    o[kObjFlagD] = kObjFlagDValue;
}
// 0 = well formed; else the 1-based failed check (host test / mutants).
inline uint32_t standin_check(const uint8_t *o) {
    if (o[kObjFlagD] != kObjFlagDValue) return 1u;                   // the prune-skip bit
    for (uint32_t i = 0; i < 8u; ++i) if (o[kObjZero38 + i] != 0u) return 2u;
    return 0u;
}

// ---- the arm state and the pipe table ------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kMaxPipes = 8u;     // accel+0xcb0's low dword is 8 (our populate hook): the family never makes more
// A traced pointer is recorded once; the table is append-only per boot. idx < kMaxPipes or -1.
inline int pipe_table_find(const uint64_t *t, uint32_t n, uint64_t p) {
    for (uint32_t i = 0; i < n && i < kMaxPipes; ++i) if (t[i] == p && p != 0ull) return (int)i;
    return -1;
}

// ---- the perform duration ------------------------------------------------------------------------------------------------------------------------------------------
struct Dur { uint64_t n, sum, min, max; };
inline void dur_note(Dur &d, uint64_t ns) { if (d.n == 0ull || ns < d.min) d.min = ns; if (ns > d.max) d.max = ns; d.sum += ns; d.n += 1ull; }
constexpr uint64_t dur_avg(const Dur &d) { return d.n ? d.sum / d.n : 0ull; }
// 0.0.617 (K6): the interval between consecutive submitTransaction calls (min / avg / max ns). `last` is the previous call's uptime stamp (0 = none yet, so the first call records nothing).
struct Ival { uint64_t last; Dur d; };
inline void ival_note(Ival &v, uint64_t nowNs) {
    const uint64_t prev = __atomic_exchange_n(&v.last, nowNs, __ATOMIC_ACQ_REL);
    if (prev != 0ull && nowNs >= prev) dur_note(v.d, nowNs - prev);
}
inline void ival_reset(Ival &v) { __atomic_store_n(&v.last, 0ull, __ATOMIC_RELEASE); v.d = Dur{}; }

// ---- 0.0.618 (V1/V2): the vblank timestamps CoreDisplay reads (notes/design/NATIVE-S5-PACING.md section 2 and 4 fix 1) ----------------------------------------------------------------
// CONFIRMED (AMDRadeonX6000 executeTransaction 0xbdcdbd0..0xbdcdc0e, `movq 0x30(%rax),%rcx; movq %rcx,0x178(%r14)` and `movq 0x40(%rax),%rcx; movq %rcx,0x188(%r14)`): Apple's driver writes the
// transaction's +0x178 (the vblank time) and +0x188 (the next vblank time) before completion, in mach_absolute_time units. The family's sendNotification forwards them as the TransactionPerformed event's
// +0x40 / +0x50, from which CoreDisplay calls SetVBLInfo(base, period = +0x50 - +0x40). +0x190 (the event's +0x58) is copied by Apple from the same record (0xbdcdbf8) but its meaning is not established:
// we leave it alone. The transaction object is 0x1b8 bytes (kTxnSize); both words lie inside it.
constexpr uint32_t kTxnSize = 0x1b8u, kTxnVblTime = 0x178u, kTxnVblNext = 0x188u;
static_assert(in_object(kTxnVblTime, 8, kTxnSize) && in_object(kTxnVblNext, 8, kTxnSize) && kTxnVblNext == kTxnVblTime + 0x10u, "the two timestamp words lie inside the transaction");
constexpr const char kTxnClass[] = "IOAccelDisplayPipeTransaction2";      // the transaction object's class (or a subclass of it): the identity check before any write
constexpr uint64_t kVblPeriodMinNs = 1000000ull, kVblPeriodMaxNs = 100000000ull;   // 1000 Hz .. 10 Hz: anything outside is not a refresh period and is never written
// The refresh period of a raster: hTotal * vTotal pixels at pixHz. 0 = not computable / outside the sane range.
constexpr uint64_t vbl_period_ns(uint32_t hTot, uint32_t vTot, uint64_t pixHz) {
    if (hTot == 0u || vTot == 0u || pixHz == 0ull || pixHz > 4000000000ull) return 0ull;
    const uint64_t p = (uint64_t)hTot * (uint64_t)vTot * 1000000000ull / pixHz;          // hTot, vTot < 2^32 each is not reached: the product of real totals is < 2^26
    return (p >= kVblPeriodMinNs && p <= kVblPeriodMaxNs) ? p : 0ull;
}
// Nanoseconds from a position sample (vertical line v, horizontal pixel h of the OTG counter) to the NEXT start of vertical blank (line vBlankStart, in the same counter coordinates the position uses;
// the counter wraps at vTot). 0 when the sample is exactly at the start. false = the sample is not coherent (a zero total, a position outside the raster, a blank start outside it, a bad period).
constexpr bool vbl_delay_ns(uint32_t v, uint32_t h, uint32_t vBlankStart, uint32_t hTot, uint32_t vTot, uint64_t periodNs, uint64_t *delayNs) {
    if (hTot == 0u || vTot == 0u || v >= vTot || h >= hTot || vBlankStart >= vTot || periodNs < kVblPeriodMinNs || periodNs > kVblPeriodMaxNs || !delayNs) return false;
    const uint64_t total = (uint64_t)hTot * (uint64_t)vTot;
    const uint64_t cur = (uint64_t)v * hTot + h, start = (uint64_t)vBlankStart * hTot;
    const uint64_t rem = cur <= start ? start - cur : total - (cur - start);          // pixels still to scan before the blank starts (0 = at the start)
    *delayNs = rem * periodNs / total;                                                // rem < total < 2^26 and periodNs <= 1e8: well inside 64 bits
    return true;
}
// mach_absolute_time units from nanoseconds with the timebase ratio abs * numer / denom = ns, i.e. abs = ns * denom / numer. Split so that nothing overflows (x86: 1:1). false = numer or denom 0 / overflow.
constexpr bool ns_to_abs(uint64_t ns, uint32_t numer, uint32_t denom, uint64_t *out) {
    if (numer == 0u || denom == 0u || !out) return false;
    const uint64_t q = ns / numer, r = ns % numer;
    if (q > ~0ull / denom) return false;
    const uint64_t hi = q * denom, lo = r * denom / numer;                            // r < numer < 2^32, denom < 2^32: r * denom < 2^64
    if (hi > ~0ull - lo) return false;
    *out = hi + lo;
    return true;
}
struct VblPlan { bool ok; uint64_t t, next, periodAbs; };
// nowAbs = mach_absolute_time at the position sample; delayNs / periodNs from vbl_delay_ns / vbl_period_ns. t = the next vblank start, next = t + one period (both mach_absolute_time units).
// ok only when the period converts to a nonzero count and nothing wraps: P = 0 and next <= t are NEVER written.
constexpr VblPlan vbl_plan(uint64_t nowAbs, uint64_t delayNs, uint64_t periodNs, uint32_t numer, uint32_t denom) {
    VblPlan p { false, 0ull, 0ull, 0ull };
    uint64_t dAbs = 0, pAbs = 0;
    if (!ns_to_abs(delayNs, numer, denom, &dAbs) || !ns_to_abs(periodNs, numer, denom, &pAbs) || pAbs == 0ull) return p;
    if (dAbs > ~0ull - nowAbs) return p;
    const uint64_t t = nowAbs + dAbs;
    if (pAbs > ~0ull - t) return p;
    p.ok = true; p.t = t; p.next = t + pAbs; p.periodAbs = pAbs;
    return p;
}
// What one stamp attempt did. Index = counter slot (pipe stat page 4).
enum VblReason : uint32_t { kVblWrote = 0, kVblOff = 1, kVblDisarmed = 2, kVblBadTxn = 3, kVblNotTxn = 4, kVblHung = 5, kVblNoTiming = 6, kVblBadMath = 7, kVblWriteFail = 8, kVblCount = 9 };
constexpr const char *vbl_name(uint32_t r) {
    return r == kVblWrote ? "timestamps written" : r == kVblOff ? "switch OFF" : r == kVblDisarmed ? "pipe not armed" : r == kVblBadTxn ? "transaction is not a kernel pointer" :
           r == kVblNotTxn ? "object is not an IOAccelDisplayPipeTransaction2" : r == kVblHung ? "HUNG latch set" : r == kVblNoTiming ? "no coherent OTG timing sample" :
           r == kVblBadMath ? "timestamp arithmetic refused" : r == kVblWriteFail ? "write refused" : "unknown";
}

// ---- stamps (`pipe stamps`, READ-ONLY, SUSPECTED layout) -------------------------------------------------------------------------------------------------------------
constexpr uint32_t kStampMaxSlots = 64u;
constexpr uint32_t stamp_walk_bound(uint32_t requested) { return requested > kStampMaxSlots ? kStampMaxSlots : requested; }
constexpr bool stamp_hazard(uint32_t completed, uint32_t submitted) { return submitted > completed; }   // submitted ahead of completed: waitForStamp would block a mode change
constexpr bool stamp_reads_in_bounds() { return in_object(kEmCount, 4, kEmSize) && in_object(kEmDone, 4, kEmSize) && in_object(kEmSub, 4, kEmSize); }
static_assert(stamp_reads_in_bounds(), "the three stamp words lie inside the event machine object");

} // namespace n48disp
