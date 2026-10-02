//
//  DisplayPipeGuard.cpp — 0.0.286 display brief).
//
//  Four pieces, each behind its own verb and all default OFF:
//
//  A. `pipeguard 1` — the SAFETY CORE for route b. Every AMD display-pipe entry point that can reach the display hardware
//     (the reviewer's list, an internal review note) is replaced on the LIVE pipe objects the adoption created, by
//     a per-instance vtable copy (the object's own vptr swapped, Apple's static tables untouched), after every guarded slot
//     is checked against its static target plus the X6000 slide:
//       pipe  (AMDRadeonX6000_AMDAccelDisplayPipe, 306 slots): initFramebufferResource -> NULL (the reserveFrameBuffer
//             VidMemory over VRAM offset 0 is never made, so +0x298 "active" stays 0); validate/perform/submitTransaction ->
//             kIOReturnNotPermitted; isTransactionComplete -> true; begin/signalTransactionComplete and
//             enable/disableTransactionInterrupt -> no-op; destroyFramebufferResource -> counted pass-through (it only
//             releases what init made, bdcca94-bdccb23, no register access).
//       display (AMDRadeonX6000_AMDNavi21Display at pipe+0x330, 78 slots): slot 51 (0x198)
//             getDisplayPipeTransactionFlip -> false, which executeTransaction turns into 0xe00002bc with no flip
//             (bdcd2fb). This is also the sink AMDGFX10Hardware::disableTransactions reaches (SUSPECTED same object).
//     executeTransaction (0xbdcd09c) is non-virtual and called only from validateTransaction (bdcd049) and
//     performTransaction (bdcde2b); processTransactionInterrupt / displayFrameStartInterruptHandler are only registered by
//     initializeTransaction on the IOFramebuffer ('txni', 'fstr', bdccd15/bdccd91), which RDNA4FB never raises;
//     reserveFrameBuffer is called only from initFramebufferResource (bdcc98a). Plus the GFX-ring WRITE_DATA guard in
//     AppleHardwareHook.cpp. Arming refuses unless ALL of it installs.
//  B. `agdc 1` — the AGDC nub: a genuine AppleGraphicsDeviceControl object (Apple's own constructor and allocator, its own
//     start/user client), whose one pure virtual (slot 266, vendor_doDeviceAttribute) and two ud2 destructors (slots 0/1,
//     13d3e566/13d3e56c) are ours through a vtable copy. Answers kAGDCVendorInfo and kAGDCGPUCapability as AMD's code does;
//     everything else kIOReturnUnsupported. Refused unless boot-arg navi48-agdc=1 AND the safety core is armed on every pipe.
//  C. `fbbench` — Part A: in-kernel write throughput into RDNA4FB's scanout per cache mode (writes back what it read).
//  D. `fbwc` — Part A fix: RDNA4FB's getVRAMRange (IOFramebuffer slot 310, byte 0x9b0) returns a descriptor whose doMap
//     (IOGeneralMemoryDescriptor slot 70) adds kIOMapWriteCombineCache to a default-cache mapping. Boot-arg navi48-fbwc=1
//     arms it when RDNA4FB publishes, before WindowServer maps the framebuffer.
//
#include "../amd/n48log.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include <kern/clock.h>
#include "Navi48Ttl.hpp"
#include "AppleHardwareHook.hpp"
#include "display_pipe_guard.h"
#include "../dcn/navi48_dcn.hpp"   // build 0.0.514 B2: n48dcn::liveRaster (read-only)
#include "../amd/native_disp.h"      // 0.0.614: n48disp_latched_on (the native AGDC route is behind boot-arg navi48-metal-disp=1)
#include "../amd/native_agdc_flow.h"  // 0.0.614: the native publish sequence (host-tested, tests/native_agdc_test.cpp)
#include "sdma_gcr.h"   // 0.0.416 (notes/design/SDMA-GCR.md): the GCR_REQ dword count for the control line (G3)

#define DPGLOG(fmt, ...) ::amdgpu::n48_logf("DisplayPipeGuard: " fmt "\n", ##__VA_ARGS__)

// From Navi48AccelPeer.cpp / Navi48Bringup.cpp.
void **navi48_accel_vtable_copy(void);
uintptr_t navi48_x6000_slide(uint32_t *reason);
IOService *navi48_bringup_pci(void);
IOService *navi48_bringup_service(void);

static constexpr uintptr_t kDpgKernelHalf = 0xffffff7000000000ull;
static inline bool dpg_kptr(const void *p) { return reinterpret_cast<uintptr_t>(p) >= kDpgKernelHalf; }
static const char *dpg_class(const void *obj) {
    if (!dpg_kptr(obj)) return "(not a kernel pointer)";
    const OSMetaClass *m = static_cast<const OSObject *>(obj)->getMetaClass();
    const char *n = m ? m->getClassName() : nullptr;
    return n ? n : "(no class)";
}

// Is this class name OUR framebuffer?
//
// It used to be `strcmp(cls, "RDNA4FB")` at seven separate sites, which was fine while the class had
// one name. It no longer does: CoreDisplay resolves a framebuffer's GPU vendor by a case-sensitive
// substring search on the class name (notes/M4-CAPABILITIES-STRUCT.md), so src/RDNA4FB now ships
// BOTH `RDNA4FB` (the default) and `AMDRDNA4FB` (selected by boot-arg rdna4-amdname=1), and either
// may be the live one. One missed site would silently stop the AGDC nub publishing and hand us a
// FALSE NEGATIVE on exactly the test this is for, so every site goes through here.
//
// This is an ALLOWLIST, not a substring test, and that matters: `strstr(cls, "AMD")` would now also
// accept Apple's own AMDFramebuffer and AMDRadeonX6000 classes. Only these two names are ours.
static bool dpg_is_our_fb_name(const char *cls) {
    if (!cls) return false;
    return !strcmp(cls, "RDNA4FB") || !strcmp(cls, "AMDRDNA4FB");
}
static bool dpg_is_our_fb(const void *obj) {
    return dpg_kptr(obj) && dpg_is_our_fb_name(dpg_class(obj));
}
static IOLock *gDpgLock { nullptr };
// True iff obj's class is `want` or derives from it, by walking the OSMetaClass superclass chain (bounded). Used for
// pipe+0xe0, which is AMDRadeonX6000_AMDGFX10Resource on this card - a subclass of IOAccelResource2 whose base-class
// fields (the ones init_framebuffer_resource writes) sit at the same offsets (tready1.
static bool dpg_is_kind_of(const void *obj, const char *want) {
    if (!dpg_kptr(obj)) return false;
    const OSMetaClass *m = static_cast<const OSObject *>(obj)->getMetaClass();
    for (int i = 0; m && i < 16; i++) {
        const char *n = m->getClassName();
        if (n && !strcmp(n, want)) return true;
        m = m->getSuperClass();
    }
    return false;
}
// An object is only asked for its class after its vtable's getMetaClass slot (7) is shown to be the expected function, so no
// virtual call is ever made through a pointer that is not the object we think it is.
static bool dpg_is(void *obj, uintptr_t slide, uintptr_t staticGetMeta, const char *cls) {
    if (!dpg_kptr(obj) || !slide) return false;
    void **vt = *reinterpret_cast<void ***>(obj);
    if (!dpg_kptr(vt) || reinterpret_cast<uintptr_t>(vt[7]) - slide != staticGetMeta) return false;
    return !strcmp(dpg_class(obj), cls);
}
static constexpr uintptr_t kDpgPipeGetMeta = 0x0bdcc3b8, kDpgDispGetMeta = 0x0be535e2;
static constexpr uintptr_t kDpgEmGetMeta = 0x0bdce58a;        // AMDRadeonX6000_AMDAccelEventMachine::getMetaClass (accel+0x380)
static constexpr uintptr_t kDpgNavi21HwVtable = 0x0bf40100;   // __ZTV32AMDRadeonX6000_AMDNavi21Hardware (vptr = +16)
static inline void dpg_lock_init() { if (!gDpgLock) gDpgLock = IOLockAlloc(); }

// =====================================================================================================================
// A. The safety core
// =====================================================================================================================
static constexpr uintptr_t kDpgAccelDisplayMachineOff = 0x378;          // IOGraphicsAccelerator2::start 145bd0b3
static constexpr uintptr_t kDpgDmFbCountOff = 0x108;                     // getFramebufferCount 1458fe92
static constexpr uintptr_t kDpgDmPipeArrayOff = 0x88;                    // getDisplayPipe 1458fff0
static constexpr uintptr_t kDpgDmD1 = 0x0bdcb5e6, kDpgDmGetMeta = 0x0bdcb630;
static constexpr uintptr_t kDpgPipeDisplayOff = 0x330;                   // executeTransaction bdcd2b6
static constexpr unsigned  kDpgMaxPipes = 8;
static constexpr unsigned  kVtHdr = 2;

typedef void *(*DpgInitFbFn)(void *, uint32_t, void *);
typedef void  (*DpgDestroyFbFn)(void *, uint32_t, void *);
typedef void  (*DpgIrqFn)(void *);           /* 0.0.329: slots 273/274, chained back to Apple  */

static struct {
    uint32_t armed, pipes, displays, fbCount, lastStatus;
    uint64_t initFb, destroyFb, enableIrq, disableIrq, validate, perform, isComplete, submit, begin, signal, flip;
    uint32_t logged;
    uintptr_t slide;
    void *pipe[kDpgMaxPipes], *display[kDpgMaxPipes];
    void **pipeOrigVt[kDpgMaxPipes], **dispOrigVt[kDpgMaxPipes];
    void **pipeCopy, **dispCopy;
    DpgDestroyFbFn origDestroy;
    DpgIrqFn origEnableIrq, origDisableIrq;   /* 0.0.329 */
    uint64_t chainedEnable, chainedDisable, chainRefused;
} gPg {};

// =====================================================================================================================
// A2. The AMD event-machine census (accel+0x380) - 0.0.289, the safety core extended (this session's brief).
// =====================================================================================================================
// accel+0x380 = AMDRadeonX6000_AMDAccelEventMachine, whose concrete base is IAF2's IOAccelEventMachineFast. IAF2 calls its
// slots 40/41/48/54/55/74/75 on OUR pipe's resources during readiness and every transaction, OUTSIDE the pipe/display
// vtables the safety core swaps. Disassembled (display_pipe_guard.h): each is pure IOAccelEvent memory
// bookkeeping - initEvent/cleanEvent memzero, copyEvent 0x40-byte memcpy, mergeEvent/testEventUnlocked read stamp values
// in memory, enableEventStampInterrupts a refcount + a self virtual on the armed 0xE0014042 path our 0xE00002D8 submit
// avoids - so NONE touches a gfx1201 register or a ring. The census confirms that on live hardware: a per-instance vtable
// copy (the object's vptr swapped, IAF2's static table untouched) whose seven called slots are PASS-THROUGH counters,
// installed only after every slot is checked against its static target plus the SAME shared X6000/IAF2 slide the safety
// core already proves. Behaviour is unchanged; the counts (and the WRITE_DATA->DCN guard reading 0) are the proof.
typedef void (*EmEventFn)(void *, void *);          // initEvent / cleanEvent (self, IOAccelEvent*)
typedef bool (*EmTestFn)(void *, void *);           // testEventUnlocked (self, IOAccelEvent*) -> bool
typedef void (*EmPairFn)(void *, void *, void *);   // copyEvent / mergeEvent (self, IOAccelEvent*, IOAccelEvent*)
typedef void (*EmTermFn)(void *);                   // deviceTerminatedUnlocked (self)
typedef void (*EmEnStFn)(void *, const void *);     // enableEventStampInterrupts (self, const IOAccelEvent*)

static struct {
    uint32_t armed, lastStatus;
    uintptr_t slide;
    void *machine;
    void **copy, **origVt;
    uint64_t hit[N48_DPG_N_EM_GUARD];   // indexed like kN48EmGuardSlots (0 D1, 1 getMetaClass, 2 init .. 8 enableStamp)
    EmEventFn origInit, origClean;
    EmTestFn origTest;
    EmPairFn origCopy, origMerge;
    EmTermFn origTerm;
    EmEnStFn origEnSt;
} gEm {};

// 0.0.291 (this session's brief): DISARM the event-machine census only. When set, dpg_arm_locked leaves
// accel+0x380 carrying its NATIVE IAF2 vtable (no counting trampolines on slots 40/41/48/54/55/74/75) while the safety
// core - the pipe/display vtable swaps, the initFramebufferResource/validate/perform/submit refusals, the FLIPS refusal
// and the GFX-ring WRITE_DATA->DCN guard - stays FULLY armed. It isolates cause 2: whether the census
// pass-throughs on the fence-read path (testEventUnlocked/mergeEvent) interfered with Metal's present-fence completion.
// Set/cleared ONLY by `emcensus` (action 72) and ONLY before pipeguard arms; the flag is read at arm time and by
// navi48_pipeguard_armed_all (which then does not require dpg_em_verified). Default 0 = census armed, as 0.0.289/0.0.290.
static uint32_t gEmDisarm = 0;

static void em_init(void *self, void *ev)          { gEm.hit[2]++; if (gEm.origInit)  gEm.origInit(self, ev); }
static void em_clean(void *self, void *ev)         { gEm.hit[3]++; if (gEm.origClean) gEm.origClean(self, ev); }
static bool em_test(void *self, void *ev)          { gEm.hit[4]++; return gEm.origTest ? gEm.origTest(self, ev) : true; }
static void em_copy(void *self, void *a, void *b)  { gEm.hit[5]++; if (gEm.origCopy)  gEm.origCopy(self, a, b); }
static void em_merge(void *self, void *a, void *b) { gEm.hit[6]++; if (gEm.origMerge) gEm.origMerge(self, a, b); }
static void em_term(void *self)                    { gEm.hit[7]++; if (gEm.origTerm) gEm.origTerm(self); }
static void em_enst(void *self, const void *ev)    { gEm.hit[8]++; if (gEm.origEnSt) gEm.origEnSt(self, ev); }

// Forward decls the census uses (defined lower).
static bool dpg_vt_matches(void **vt, uintptr_t slide, const n48_dpg_slot *s, unsigned n, unsigned *bad);
static void **dpg_make_copy(void **vt, unsigned slots);

// 0 armed, 14 not the AMD event machine, 15 a slot's identity, 16 allocation, 17 vptr swap read-back failed.
static uint32_t dpg_arm_event_machine(void *accel, uintptr_t slide) {
    if (gEm.armed) return 0;
    void *m = *reinterpret_cast<void **>(static_cast<char *>(accel) + 0x380);
    if (!dpg_is(m, slide, kDpgEmGetMeta, "AMDRadeonX6000_AMDAccelEventMachine")) {
        DPGLOG("pipeguard: event-machine census REFUSED - accel+0x380 %p is not the AMD event machine (%s)", m, dpg_class(m));
        return 14;
    }
    void **vt = *reinterpret_cast<void ***>(m);
    unsigned bad = 0;
    if (!dpg_vt_matches(vt, slide, kN48EmGuardSlots, N48_DPG_N_EM_GUARD, &bad)) {
        const n48_dpg_slot &s = kN48EmGuardSlots[bad - 1];
        DPGLOG("pipeguard: event-machine census REFUSED - slot %u (%s) = %#lx minus slide, want %#x", s.slot, s.name,
               (unsigned long)(reinterpret_cast<uintptr_t>(vt[s.slot]) - slide), s.target);
        return 15;
    }
    void **copy = dpg_make_copy(vt, N48_DPG_EM_SLOTS);
    if (!copy) return 16;
    gEm.origInit  = reinterpret_cast<EmEventFn>(vt[40]);
    gEm.origClean = reinterpret_cast<EmEventFn>(vt[41]);
    gEm.origTest  = reinterpret_cast<EmTestFn>(vt[48]);
    gEm.origCopy  = reinterpret_cast<EmPairFn>(vt[54]);
    gEm.origMerge = reinterpret_cast<EmPairFn>(vt[55]);
    gEm.origTerm  = reinterpret_cast<EmTermFn>(vt[74]);
    gEm.origEnSt  = reinterpret_cast<EmEnStFn>(vt[75]);
    copy[kVtHdr + 40] = reinterpret_cast<void *>(&em_init);
    copy[kVtHdr + 41] = reinterpret_cast<void *>(&em_clean);
    copy[kVtHdr + 48] = reinterpret_cast<void *>(&em_test);
    copy[kVtHdr + 54] = reinterpret_cast<void *>(&em_copy);
    copy[kVtHdr + 55] = reinterpret_cast<void *>(&em_merge);
    copy[kVtHdr + 74] = reinterpret_cast<void *>(&em_term);
    copy[kVtHdr + 75] = reinterpret_cast<void *>(&em_enst);
    gEm.machine = m; gEm.origVt = vt; gEm.copy = copy; gEm.slide = slide;
    __asm__ __volatile__("sfence" ::: "memory");
    *reinterpret_cast<void ***>(m) = copy + kVtHdr;
    gEm.armed = (*reinterpret_cast<void ***>(m) == copy + kVtHdr) ? 1u : 0u;
    DPGLOG("pipeguard: event-machine census %s - accel+0x380 %p, slots 40 initEvent / 41 cleanEvent / 48 testEventUnlocked "
           "/ 54 copyEvent / 55 mergeEvent / 74 deviceTerminated / 75 enableEventStampInterrupts wrapped as pass-through "
           "counters; each is pure IOAccelEvent memory bookkeeping (settled from the bytes",
           gEm.armed ? "ARMED" : "vptr swap FAILED", m);
    return gEm.armed ? 0 : 17;
}

// True iff the event-machine census is still live (the object still carries our copy).
static bool dpg_em_verified(void) {
    if (!gEm.armed || !gEm.machine || !gEm.copy) return false;
    return *reinterpret_cast<void *const *>(gEm.machine) == gEm.copy + kVtHdr;
}

// =====================================================================================================================
// E. The pipe SHIM (0.0.288) - the same guarded vtable copy, but PARTICIPATING instead of refusing.
//
// The safety core's leaves refuse every transaction. The shim is a mode flag on those same functions: with gSh.mode 0
// (the default) nothing changes and the machine still refuses; with mode >= 1 the leaves let IOAcceleratorFamily2's own
// IOAccelDisplayPipe state machine run the queue, while the flip (display slot 51) stays false and no AMD code that could
// reach the ring is re-enabled. No second vtable is installed and no new object is created, so `pipeguard`'s identity
// checks (slot 7 getMetaClass, our copy's address) hold unchanged.
//
// The lifecycle, CONFIRMED from IOAcceleratorFamily2's bytes this session (addresses are unslid, the binary's own):
//   transaction_queue_gated 0x145cf034: if pipe+0x2a4 == 0 -> set it and call vendor slot 273 (*0x888); then vendor slot
//     279 submitTransaction (*0x8b8, 0x145cf069) and store its return in txn+0x58 (0x145cf06f); park the txn in the
//     4-deep ring at pipe+0x118 + (pipe+0x23c & 3) * 0x48, entry +0x40 (0x145cf097); call the AMD event machine
//     accel+0x380 slot 54 (*0x1b0, 0x145cf0b3); sfence; `incl 0x23c` (0x145cf0bc); TAIL-CALL pipe+0xc0 -> *0x1d8
//     (0x145cf0c2-0x145cf0dd).
//   pipe+0xc0 is the IOInterruptEventSource whose action is event_interrupt_gated (setup_workloop 0x145ce5c6 creates it,
//     0x145ce5d9 stores it at +0xc0), so EVERY QUEUE PUSH SCHEDULES THE DRAIN ITSELF on the pipe workloop. Liveness needs
//     no display VBL and no kext timer. (pipe+0xc8 = vbl_interrupt_gated 0x145ce645, pipe+0xd0 =
//     transaction_interrupt_gated 0x145ce6b1, which tail-jumps into event_interrupt_gated at 0x145cedb0.)
//   event_interrupt_gated 0x145ce7ea: drains while pipe+0x23c > pipe+0x238; entry index is pipe+0x238 & 3; the AMD event
//     machine's fence check (accel+0x380 slot 48, *0x180 at 0x145ce8b4) runs ONLY when txn+0x58 == 0xE0014042
//     (0x145ce895). Then pipe+0x248 = txn (0x145ce903) and:
//       *** 0x145ce90e-0x145ce91e: the PERFORM BLOCK IS ENTERED ONLY WHEN txn+0x58 IS 0xE0014042 OR 0xE00002D8. ***
//     A shim submitTransaction that returns 0 therefore makes vendor performTransaction (slot 277, *0x8a8 at 0x145ce97e)
//     NEVER RUN: the transaction is retired and completed with no present at all. This corrects the design memo, which
//     reads the 0x145ce895 test as the only gate. Returning kIOReturnNotReady (0xE00002D8) is the value that reaches
//     perform AND still skips the event machine's fence check.
//   The retire index: `incl 0x238(%rbx)` at 0x145ceb2b (success, reached from `testl %r14d,%r14d; je` 0x145cea3d/40) and
//     at 0x145cead3 (the error path). Those two instructions are the ONLY writes to pipe+0x238 in the whole binary
//     (exhaustive objdump grep). completeTransaction 0x145cf27a NEVER touches it - it clears pipe+0x248 (0x145cf37f),
//     moves the txn to the free list, calls vendor slot 284 and wakes pipe+0xb0 -> *0x1e8 (0x145cf3b6). So the ring slot
//     wait_for_queue_slot_gated throttles on (0x145cf0f4: 0x23c - 0x238 == 4) is freed when perform RETURNS 0, before
//     isTransactionComplete (0x145ceb47) and before completeTransaction (0x145ceb5b) - which is why a synchronous present
//     cannot fill the ring, and why Hazard 4 needs no timer.
//
// Mode 1 = participate + census only: perform reads the transaction's plane array and logs it, and presents NOTHING.
// Mode 2 = participate + present: perform additionally copies plane 0 into RDNA4FB's scanout on SDMA0 QUEUE0 through
//   navi48_scanout_copy_vram ('s proven path), which still refuses until the scanout positive control passed.
// =====================================================================================================================
static constexpr uintptr_t kShimVidSegment = 0x0bdf6c3eull;   // AMDRadeonX6000_AMDAccelVidMemory::getPhysicalSegment
static constexpr unsigned  kShimVidSegSlot = 0x158 / 8;       // 43
static constexpr unsigned  kShimPlaneStride = 0x70;           // make_flip_transaction 0x145cce1d `imulq $0x70`
typedef uint64_t (*ShimPhysSegFn)(void *, uint64_t, uint64_t *);

// IEEE-754 single -> truncated int, in integer arithmetic only: kernel code must never load a float into an SSE
// register (the FPU state is not saved across a kernel entry). Saturates at +/-0x40000000 and returns 0 for NaN/inf.
static int dpg_f32_int(uint32_t bits) {
    const uint32_t exp = (bits >> 23) & 0xffu;
    const uint32_t man = bits & 0x7fffffu;
    const bool neg = (bits >> 31) != 0;
    if (exp == 0xffu) return 0;
    if (exp < 127u) return 0;                       // |x| < 1
    const int e = (int)exp - 127;
    if (e > 30) return neg ? -0x40000000 : 0x40000000;
    const uint32_t m = man | 0x800000u;             // 1.mantissa in Q23
    const uint32_t mag = (e >= 23) ? (m << (e - 23)) : (m >> (23 - e));
    return neg ? -(int)mag : (int)mag;
}

static struct {
    uint32_t mode, lastStatus;
    uint64_t validate, perform, submit, isComplete, census, planes;
    uint64_t copyOk, copyRefused, lastCopySt, lastPlaneW, lastPlaneH, lastPhys, lastLen, lastUs;
    // 0.0.373 - THE READBACK BUCKET. Through 0.0.372 a present whose status was kScanStReadback (11) was
    // counted in copyRefused, and it is NOT a refusal: the copy was built, submitted and FENCED, the pixels reached
    // the scanout, and what disagreed was OUR OWN detile-equation verification of rows 0 and h-1 afterwards. arm3
    // paid for that: the run reported "presents ok 6 / refused 1" against fifteen real SDMA tiled copies,
    // and the visible output was treated as the thing that needed explaining. A verification mismatch now has its
    // own name and its own counter; `delivered` is copyOk + copyReadback, and copyRefused means what it says - the
    // copy was never made. ('s defect, closed.)
    uint64_t copyReadback;
    // 0.0.412: force a swizzle-3 plane down the EXISTING linear row path (the one swizzle 0 already
    // uses). DEFAULT 0, set only by `pipeshim 4`, cleared by every other argument, so 0.0.411's tiled call is untouched.
    uint32_t forceLinear;
    // 0.0.416 (notes/design/SDMA-GCR.md, G3): prepend the SDMA GCR_REQ (GL2 write-back + invalidate) to the same
    // submission as the tiled copy. DEFAULT 0, set only by `pipeshim 5`, cleared by every other argument, so ARG 2
    // (and every pre-0.0.416 caller) emits exactly the 0.0.415 packet.
    uint32_t gcr;
    // 0.0.412: how many map lines this boot has printed, and the content token of the last one. The
    // token is the presented plane's VRAM offset (`phys`), which the present path already reads. Bounded by the pure
    // N48_DPG_VERIFY_BUDGET; a re-assert of the same mode does NOT reset either field.
    uint32_t verifyRuns;
    uint64_t lastVerifyToken;
    uint32_t logged;
    uint32_t descLogged;       // 0.0.345: the arrangement line, first four presents only
    uint32_t beats;            // 0.0.348: the HEARTBEAT, one line every 25th present, budget 40 - see dpg_perform
    // 0.0.373: what the LAST heartbeat printed, so `pipeshimread` can say what it is - a snapshot taken at the instant
    // of the read - and print the boot's own last word beside it instead of in place of it.
    uint64_t beatUs, beatPerform, beatOk, beatRefused, beatReadback;
} gSh {};

// 0.0.373: the bucketing rule itself is display_pipe_guard.h's n48_dpg_present_bucket - pure, host-tested
// with a planted defect (tests/gfx_keystone_test.cpp), and the SAME header the kext compiles. This file only counts.

// =====================================================================================================================
// G. Route A (0.0.295) — the kext object slot 267 returns + the identity-gated resource slot-46 (prepare) guard.
//    DEFAULT OFF (gRa.mode 0 = exactly 0.0.294): allocates nothing and swaps nothing until `routea 1`. See
//    display_pipe_guard.h for the CONFIRMED field/slot spec and an internal review note for why.
//
//    Two moving parts, both gated behind gRa.mode and rooted on the safety core's already-guarded pipes:
//      1) slot 267 (dpg_initFb) returns gRa.obj[i] — a kext-owned, VidMemory-shaped object (never reserveFrameBuffer) —
//         and sets pipe+0x298 = 1 itself (with the route-B geometry fields), so readiness survives a WindowServer re-init.
//      2) slot 46 (AMDAccelResource::prepare) is neutralised ON THE FRAMEBUFFER RESOURCE ONLY, by a per-instance vptr
//         swap of pipe+0xe0 (Metal resources keep the native shared table) whose trampoline returns true without running
//         prepare()'s pruneOrphanedMappings + channel tree. The identity gate (this == our resource) is belt-and-suspenders.
// =====================================================================================================================
static_assert(N48_RA_OBJ_SIZE >= 0x48u, "route A object must be >= 0x48 bytes");
static_assert(N48_RA_VT_SLOTS >= 44u, "route A vtable must have >= 44 slots");
static_assert(N48_RA_SLOT_RELEASE * 8u == 0x28u, "release is slot 5 (*0x28)");
static_assert(N48_RA_SLOT_PHYSSEG * 8u == 0x158u, "getPhysicalSegment is slot 43 (*0x158)");
static_assert(N48_RA_PREPARE_SLOT * 8u == 0x170u, "prepare is slot 46 (*0x170)");
static_assert(N48_RA_OFF_FLAGC == 0x0cu && N48_RA_OFF_FLAGD == 0x0du, "flag bytes at +0xc / +0xd");
static_assert(N48_RA_OFF_F38 == 0x38u && N48_RA_OFF_LEN == 0x40u, "fields at +0x38 / +0x40");
static_assert(N48_RA_OFF_LEN + 8u <= N48_RA_OBJ_SIZE, "the length field fits in the object");

typedef bool     (*RaPrepareFn)(void *self);                          // AMDAccelResource::prepare() @0xbdd6198 (this-only)
typedef uint64_t (*RaPhysSegFn)(void *self, uint64_t idx, uint64_t *span);

static struct {
    uint32_t armed, mode, lastStatus;                                 // mode 0 off (0.0.294), 1 route A active
    uintptr_t slide;
    uint32_t pipes;
    uint64_t phys, len;                                               // scanout placement the object advertises (Console,*)
    uint32_t width, height, rowBytes, bpp, fmt;                       // captured framebuffer geometry (fixed console mode)
    void *res[kDpgMaxPipes];                                          // the framebuffer resources we guard (pipe+0xe0)
    void **resOrigVt[kDpgMaxPipes];
    void **resCopy;                                                   // one shared slot-46-guarded AMDGFX10Resource copy
    RaPrepareFn origPrepare;                                          // the real prepare(), for pass-through
    uint8_t *obj[kDpgMaxPipes];                                       // the kext object handed back per pipe
    void **objVt;                                                     // the object's shared kext-owned vtable (48 slots)
    uint64_t initFbHits, readyWrites, guardHits, passThru, releaseHits, physSegHits, shimBypass, identityMiss;
    uint32_t logged;
} gRa {};

// The object's own vtable slots. Any slot resolves to a harmless 0 return; only 5 (release) and 43 (getPhysicalSegment)
// are meaningful. Apple only ever calls *0x28 (destroy) and *0x158 (shim) on this object (CONFIRMED,).
static uint64_t ra_vt_stub(void *self) { (void)self; return 0; }
static void     ra_release(void *self) { gRa.releaseHits++; (void)self; }   // frees nothing AMD-owned (we own the object)
static uint64_t ra_getphysseg(void *self, uint64_t idx, uint64_t *span) {
    gRa.physSegHits++; (void)self; (void)idx; return n48_ra_physseg(gRa.phys, gRa.len, span);
}

static bool ra_is_our_obj(const void *p) {
    if (!p) return false;
    for (unsigned i = 0; i < kDpgMaxPipes; i++) if (gRa.obj[i] == p) return true;
    return false;
}
static bool ra_is_our_res(const void *p) {
    if (!p) return false;
    for (unsigned i = 0; i < kDpgMaxPipes; i++) if (gRa.res[i] == p) return true;
    return false;
}

// The resource slot-46 (prepare) trampoline. Reached ONLY through pipe+0xe0's swapped vptr; every other resource of the
// class keeps the native shared table and never gets here. With route A active and this==our framebuffer resource,
// return true WITHOUT prepare()'s body (this is what stops the unprovable pruneOrphanedMappings + channel tree). Anything
// else — a different this, or route A toggled off — is forwarded unchanged to the real prepare().
static bool ra_prepare(void *self) {
    if (gRa.mode && ra_is_our_res(self)) { gRa.guardHits++; return true; }
    gRa.passThru++;
    return gRa.origPrepare ? gRa.origPrepare(self) : true;
}

// Route A's readiness write, run from slot 267 (dpg_initFb) with self=pipe, res=pipe+0xe0. Writes the SAME fields
// init_framebuffer_resource writes at 0x145cc404-0x145cc4be (route B's set) from the captured console geometry — which
// equals what Apple just wrote, RDNA4FB doing no modesetting — then sets pipe+0x298 = 1 so readiness holds regardless of
// slot 46's return. No AMD binding code, no reserveFrameBuffer.
static void ra_route_a_ready(void *pipe, void *res) {
    char *pc = static_cast<char *>(pipe);
    char *rc = static_cast<char *>(res);
    *reinterpret_cast<uint32_t *>(pc + 0x28c) = gRa.fmt;
    *reinterpret_cast<uint32_t *>(pc + 0x290) = gRa.width;
    *reinterpret_cast<uint16_t *>(pc + 0x294) = (uint16_t)gRa.height;
    *reinterpret_cast<uint16_t *>(pc + 0x296) = (uint16_t)(gRa.bpp >> 3);
    *reinterpret_cast<uint16_t *>(rc + 0xb0)  = (uint16_t)gRa.width;
    *reinterpret_cast<uint16_t *>(rc + 0xb2)  = (uint16_t)gRa.height;
    *reinterpret_cast<uint16_t *>(rc + 0xd8)  = (uint16_t)(gRa.bpp >> 3);
    *reinterpret_cast<uint64_t *>(rc + 0xb8)  = gRa.rowBytes;
    *reinterpret_cast<uint64_t *>(rc + 0xc8)  = (uint64_t)(uint32_t)(gRa.height * gRa.rowBytes);
    *reinterpret_cast<uint32_t *>(rc + 0x28)  = 0;
    rc[0xf] = (char)(rc[0xf] & ~8);
    void *r30 = *reinterpret_cast<void **>(rc + 0x30);
    if (dpg_kptr(r30)) static_cast<char *>(r30)[0x1c] = 0;
    __asm__ __volatile__("sfence" ::: "memory");
    pc[0x298] = 1;
    gRa.readyWrites++;
    if (gRa.logged < 16u) {
        gRa.logged++;
        DPGLOG("routea: slot 267 on pipe %p resource %p -> object; geometry %ux%u rowBytes %u fmt %u re-affirmed, pipe+0x298 = 1",
               pipe, res, gRa.width, gRa.height, gRa.rowBytes, gRa.fmt);
    }
}

static constexpr uint32_t kShimCensusLogBudget = 24;
// IOAccelDisplayPipe hands the perform a transaction whose plane array is txn+0x38 (stride 0x70, entry +0x30 the plane
// IOAccelResource2, +0x38 a second plane; make_flip_transaction 0x145ccd9d/0x145ccdb7 and its `imulq $0x70,%rdx` at
// 0x145cce1d). txn+0x48 is NOT a plane count: it is getTransactionDirtyBits (0x145cf4c8 `movq 0x48(%rdi),%rax`), a mask
// that make_flip_transaction sets to 3 (0x145ccd88) and that executeTransaction tests bit by bit (0xbdcd1df
// `testb $0x1,%r12b`); txn+0x50 is getTransactionOptions (0x145cf4d2) and txn+0x54 the transaction ID (0x145cf4be).
// Because the array's length is not established from the bytes, the census reads ENTRY 0 ONLY - the entry
// make_flip_transaction fills and the one a single-plane desktop present uses - and logs the raw mask/options/ID so the
// first T-compose run can settle how many entries a multi-plane transaction really carries.
// Everything below is a READ, bounded, and every pointer is checked.
static bool shim_plane_placement(void *res, uint64_t *physOut, uint64_t *lenOut, uint32_t *wOut, uint32_t *hOut,
                                 uint32_t *strideOut, const char **whyOut) {
    *physOut = 0; *lenOut = 0; *wOut = 0; *hOut = 0; *strideOut = 0; *whyOut = "no plane resource";
    if (!dpg_kptr(res)) return false;
    const char *r = static_cast<const char *>(res);
    *wOut = *reinterpret_cast<const uint16_t *>(r + 0xb0);
    *hOut = *reinterpret_cast<const uint16_t *>(r + 0xb2);
    const uint64_t rowBytes = *reinterpret_cast<const uint64_t *>(r + 0xb8);
    *strideOut = (rowBytes > 0xffffffffull) ? 0u : (uint32_t)rowBytes;
    void *mem = *reinterpret_cast<void *const *>(r + 0x88);
    if (!dpg_kptr(mem)) { *whyOut = "resource+0x88 VidMemory is null"; return false; }
    // Route A: our own kext object (identity-gated) supplies its own slot 43 and is not an AMDAccelVidMemory, so BOTH the
    // class-name check and the slot-43 fn-slide check are relaxed FOR IT ONLY (the reviewer item 3). Calling dpg_class on it would
    // dereference our stub getMetaClass, so the class check must be skipped, not just its result ignored. Every other
    // object still gets the full strict checks below.
    const bool ours = ra_is_our_obj(mem);
    if (!ours && strcmp(dpg_class(mem), "AMDRadeonX6000_AMDAccelVidMemory")) { *whyOut = "resource+0x88 is not an AMDAccelVidMemory"; return false; }
    uint32_t sr = 0;
    const uintptr_t slide = navi48_x6000_slide(&sr);
    void **mvt = *reinterpret_cast<void ***>(mem);
    if (!dpg_kptr(mvt) || (!ours && !slide)) { *whyOut = "no X6000 slide / VidMemory vtable"; return false; }
    const uintptr_t fn = reinterpret_cast<uintptr_t>(mvt[kShimVidSegSlot]);
    if (!ours && fn - slide != kShimVidSegment) { *whyOut = "VidMemory slot 43 is not getPhysicalSegment"; return false; }
    if (ours) gRa.shimBypass++;
    const uint64_t mlen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(mem) + 0x40);
    uint64_t span = 0;
    const uint64_t phys = reinterpret_cast<ShimPhysSegFn>(fn)(mem, 0, &span);
    if (!phys || span < mlen) { *whyOut = "VidMemory has no contiguous VRAM placement (rule 91: a CPU-locked surface has none)"; return false; }
    *physOut = phys; *lenOut = mlen; *whyOut = "";
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.344  — THE SOURCE ARRANGEMENT PROBE. ONE SHOT, LOG ONLY, and it runs BEFORE the copy, so it
// cannot change one byte of what the copy moves. It answers one question: is WindowServer's composited surface LINEAR
// (what our row-by-row COPY_LINEAR assumes) or TILED (swizzled) — and if tiled, which mode.
//
// HALF 1, the descriptor, read off the plane-0 AMDRadeonX6000_AMDGFX10Resource. CONFIRMED from the shipped binary
// re/tahoe-26.6.2-x86_64/out/kexts/com.apple.kext.AMDRadeonX6000 (addresses + bytes):
//   * the object is 0x278 bytes — OSDefineMetaClassAndStructors' size argument at 0xbdf9838, `b9 78 02 00 00` — so
//     every offset below is inside the allocation.
//   * res+0x1dc is the surface record AMDAccelResourceAddr2::shapeSurfaceBuffer builds:
//       0xbdda673 movq 0x250(%r12),%rdi        res+0x250 is the AMDGFX10AlignManager
//       0xbdda684 callq *0x1b8(%rax)           vtable +0x1b8 = AMDGFX10AlignManager::getSwizzleMode  @0xbe446f0
//       0xbdda692 andl $0x1f,%eax  0xbdda695 andl $-0x20,%ecx  0xbdda69a movl %ecx,0x1dc(%r12)   -> bits [4:0]
//       0xbdda6b3 callq *0x1c8(%rax)           vtable +0x1c8 = AMDGFX10AlignManager::getResourceType @0xbe4470a
//       0xbdda6b9 shll $0x5,%eax  0xbdda6bc andl $0x60,%eax  -> bits [6:5];  0xbdda6d4 shll $0x7 -> bit 7
//   * AMDGFX10Resource::setupHwCBRegs reads exactly that record and puts [4:0] straight into the hardware:
//       0xbdf9133 leaq 0x40(%rax),%rcx / 0xbdf9137 leaq 0x1dc(%r14),%rsi / 0xbdf9141 cmoveq %rsi,%rcx
//       0xbdf9145 movl (%rcx),%ecx             rcx = res+0x180 ? *(res+0x180)+0x40 : res+0x1dc
//       0xbdf92e8 andl $0x1f,%ecx / 0xbdf92f3 shll $0xe,%ecx / 0xbdf9304 movl %ecx,0x4c(%rbx)
//     and rec+0x4c's keep-mask 0xc4f81fff (0xbdf92eb) clears exactly bits 13-18 and 24-25, which are
//     CB_COLOR_ATTRIB3's META_LINEAR, COLOR_SW_MODE[18:14] and RESOURCE_TYPE[25:24]. So [4:0] IS the swizzle mode
//     the hardware is told, not a derived guess.
//   * the numbering is addrlib's AddrSwizzleMode: getSwizzleMode's table (0xbed21a0) and getAddrSwizzleMode's
//     (0xbed2020) are the identity on every mode Apple accepts and 33 = ADDR_SW_MAX_TYPE on every one it does not.
//
// HALF 2, the bytes, through navi48_vram_read_mm — the EXISTING MM_INDEX/MM_DATA read path (vmib, vmstate, pagecopy
// and the scanout copy's own verify all use it), which is the only CPU view of VRAM above the 256 MiB BAR0 window;
// the surface sits at 0x10930000 = 265 MiB, above it. NO NEW HARDWARE-REGISTER WRITE.
// The test: a login screen is mostly flat background, so a LINEAR 1920-pixel BGRA row has few dword-to-dword
// transitions and they fall at arbitrary image positions. A TILED surface read as if linear breaks at every block
// boundary, so transitions are many AND land on a fixed period — 16 dwords (64 B), 64 dwords (256 B), 1024 (4 KiB).
static const char *dpg_sw_mode_name(uint32_t m) {
    static const char *const n[34] = {
        "ADDR_SW_LINEAR", "ADDR_SW_256B_S", "ADDR_SW_256B_D", "ADDR_SW_256B_R",
        "ADDR_SW_4KB_Z", "ADDR_SW_4KB_S", "ADDR_SW_4KB_D", "ADDR_SW_4KB_R",
        "ADDR_SW_64KB_Z", "ADDR_SW_64KB_S", "ADDR_SW_64KB_D", "ADDR_SW_64KB_R",
        "reserved12", "reserved13", "reserved14", "reserved15",
        "ADDR_SW_64KB_Z_T", "ADDR_SW_64KB_S_T", "ADDR_SW_64KB_D_T", "ADDR_SW_64KB_R_T",
        "ADDR_SW_4KB_Z_X", "ADDR_SW_4KB_S_X", "ADDR_SW_4KB_D_X", "ADDR_SW_4KB_R_X",
        "ADDR_SW_64KB_Z_X", "ADDR_SW_64KB_S_X", "ADDR_SW_64KB_D_X", "ADDR_SW_64KB_R_X",
        "ADDR_SW_VAR_Z_X", "reserved29", "reserved30", "ADDR_SW_VAR_R_X",
        "ADDR_SW_LINEAR_GENERAL", "ADDR_SW_MAX_TYPE"
    };
    return m < 34u ? n[m] : "OUT OF RANGE";
}

// One whole source row, streamed through the MM window 64 dwords at a time. Nothing is written, nothing is kept but
// the head and the counters. A refused read is reported as a NON-OBSERVATION, never as "no transitions".
static void dpg_probe_row(const char *what, uint64_t off, uint32_t dwords) {
    uint32_t buf[64] = { 0 }, head[8] = { 0 };
    uint32_t trans = 0, zeros = 0, prev = 0, tidx[12] = { 0 };
    uint32_t p4 = 0, p8 = 0, p16 = 0, p32 = 0, p64 = 0, p128 = 0, p1024 = 0;
    for (uint32_t i = 0; i < dwords; i += 64) {
        const uint32_t n = (dwords - i) < 64u ? (dwords - i) : 64u;
        if (!navi48_vram_read_mm(off + (uint64_t)i * 4u, buf, n)) {
            DPGLOG("srcbytes: %s at VRAM %#llx - MM read REFUSED at dword %u. NOTHING WAS READ: this line is a "
                   "NON-OBSERVATION, not a measurement of a flat surface", what, (unsigned long long)off, i);
            return;
        }
        for (uint32_t j = 0; j < n; j++) {
            const uint32_t k = i + j, v = buf[j];
            if (k < 8u) head[k] = v;
            if (v == 0u) zeros++;
            if (k && v != prev) {
                if (trans < 12u) tidx[trans] = k;
                trans++;
                if ((k & 3u) == 0u) p4++;
                if ((k & 7u) == 0u) p8++;
                if ((k & 15u) == 0u) p16++;
                if ((k & 31u) == 0u) p32++;
                if ((k & 63u) == 0u) p64++;
                if ((k & 127u) == 0u) p128++;
                if ((k & 1023u) == 0u) p1024++;
            }
            prev = v;
        }
    }
    DPGLOG("srcbytes: %s at VRAM %#llx, %u dword(s): head %08x %08x %08x %08x %08x %08x %08x %08x", what,
           (unsigned long long)off, dwords, head[0], head[1], head[2], head[3], head[4], head[5], head[6], head[7]);
    DPGLOG("srcbytes: %s: %u transition(s) of %u dword(s), %u all-zero; of those transitions %u/%u/%u/%u/%u/%u/%u sit on a "
           "4/8/16/32/64/128/1024-dword (16/32/64/128/256/512/4096-byte) boundary; first indices %u %u %u %u %u %u %u %u %u %u %u %u",
           what, trans, dwords, zeros, p4, p8, p16, p32, p64, p128, p1024, tidx[0], tidx[1], tidx[2], tidx[3], tidx[4],
           tidx[5], tidx[6], tidx[7], tidx[8], tidx[9], tidx[10], tidx[11]);
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.348 (notes sections 722-723) — THE LAYOUT DISCRIMINATOR. ONE 64 KiB block of the LIVE composited surface, read
// through the MM window and analysed for WHICH ADDRESS BIT IS WHICH IMAGE STEP. Read-only, before the copy, no new
// packet, no new register write, nothing about the copy changed.
//
// WHY A BIT SPECTRUM AND NOT A PICTURE COMPARISON. A login screen is mostly flat colour, so raw pixel VALUES are a weak
// discriminator. But every candidate layout is a map from address bit -> (dx, dy) in the image, and in ANY image with
// spatial structure two pixels one apart differ LESS than two pixels far apart. So for each address bit b measure
//     D(b) = mean over the block of |v(o) - v(o XOR (1 << b))|
// and D is monotone in the pixel distance that bit represents. That reads the address equation OUT of the data without
// assuming either candidate, and it is INVARIANT to any pipeBankXor, because a constant XOR of the address cancels
// inside the pair (o, o ^ bit).
//
// THE TWO CANDIDATES, CONFIRMED from vendored addrlib under re/graphics/src/mesa/src/amd/addrlib:
//   gfx12 ADDR3_64KB_2D, 4 BPE, 1xAA — gfx12/gfx12SwizzlePattern.h:95-102 GFX12_SW_64KB_2D_1xAA_PATINFO 4-BPE row
//     {2,3,3,0}; NIBBLE1[2] = {0,0,X0,Y0,X1,Y1,X2,Y2}, NIBBLE2[3] = {Y3,X3,Y4,X4}, NIBBLE3[3] = {Y5,X5,Y6,X6},
//     NIBBLE4[0] = {0,0}.   bits 2..15 = X0 Y0 X1 Y1 X2 Y2 Y3 X3 Y4 X4 Y5 X5 Y6 X6   (= n48_addr3_64kb_2d_off_4bpe).
//   gfx10 ADDR_SW_64KB_R_X, 4 bpe, 1xaa — gfx10/gfx10SwizzlePattern.h. Apple's driver is the Navi21 one, so the RbPlus
//     table applies: GFX10_SW_64K_R_X_1xaa_RBPLUS_PATINFO uses NIBBLE01[39] = {0,0,X0,X1,Y0,Y1,X2,Y2} in EVERY 4-bpe
//     row (all 15 pipe/PKR rows). The Navi1x table uses NIBBLE01[2] = {0,0,X0,X1,Y0,Y1,Y2,X2}. Both give
//     bits 2,3,4,5 = X0 X1 Y0 Y1.
//   NOTE, and it CORRECTS what section 721 quoted as "section 716 CONFIRMED": there is no section 716 in
//   notes/APPLE-DRIVER-VERDICT.md, and "the two AGREE in bits [15:8]" is true only of the gfx10 Navi1x 1-pipe row. The
//   RbPlus rows that apply to a Navi21 driver put XOR terms (Y4^X5^Y5, Z0^X4^Y4, X4^Y9 ...) in bits 8..15, so if the
//   bytes really were in gfx10 R_X the 8x8 units would ALSO be misplaced inside the block, not only shuffled inside.
//
// THE DISCRIMINATOR IS ADDRESS BITS 3 AND 4 AND NOTHING ELSE IS NEEDED:
//     gfx12: bit 3 = Y0 (ONE pixel down),   bit 4 = X1 (TWO pixels right)
//     gfx10: bit 3 = X1 (TWO pixels right), bit 4 = Y0 (ONE pixel down)
// They are the SAME PAIR of steps assigned the opposite way round.
//
// THE VERDICT IS NOT "D(3) < D(4)". That comparison pits a one-pixel VERTICAL step against a two-pixel HORIZONTAL one
// and inverts on an image whose vertical gradient is the stronger; measured on the host Mac over synthetic images
// (scratch harness, three anisotropies) the two-sided monotone form of it returned "BOTH" on an isotropic image, i.e.
// it does not discriminate. THE VERDICT IS TOTAL VARIATION: reconstruct the 128x128 block under each candidate and sum
// |neighbour difference| over the reconstruction. The layout the bytes are really in is the one that makes a picture;
// the other scrambles the 64 pixels inside each 8x8 unit. On the host Mac that separates the two by 1.54x-2.46x with the
// right sign at every anisotropy tried, ties to 1.002x on white noise and ties exactly on a flat block, so a margin
// under 110% of the runner-up is reported here as a NON-OBSERVATION rather than a verdict.
// The D(b) spectrum is still logged, because it is hypothesis-free: under gfx12 it must show two SEVEN-rung monotone
// ladders, x = bits 2,4,6,9,11,13,15 and y = bits 3,5,7,8,10,12,14. That is corroboration, not the verdict.
//
// THE METHOD CONTROLS, CPU only, no VRAM, run through the SAME buffer and the SAME code: a smooth ramp laid out by the
// gfx12 assignment and the SAME ramp laid out by the gfx10 assignment. The first must come back gfx12 and the second
// gfx10. If either control picks wrong the method is blind and the live verdict beneath it means nothing.
// AND IF NEITHER NAMED CANDIDATE WINS, the run still says what WOULD: a scan of all 91 single transpositions of two
// address bits applied to the gfx12 map, reporting the three lowest total variations.
// THE RAW EVIDENCE. The first 1024 dwords of the block go to the log verbatim, which is enough to recompute D(2)..D(11)
// independently of every line of code above.
static uint32_t gLayoutBlock[16384];        // 64 KiB of BSS: one 64 KiB tiled block of the live surface
static uint16_t gLayoutIdx[16384];          // 32 KiB: (x,y) -> dword index, rebuilt per candidate layout
static uint32_t gLayoutShots = 0;
static uint64_t gLayoutLastPhys = 0;
static uint64_t dpg_now_us();

// |a-b| summed over the four bytes. Format-agnostic enough for BGRA8 and for a packed 10-10-10-2, and monotone in "how
// different are these two pixels" either way.
static inline uint32_t dpg_pixdiff(uint32_t a, uint32_t b) {
    uint32_t s = 0;
    for (uint32_t i = 0; i < 4u; i++) {
        const uint32_t x = (a >> (i * 8)) & 0xffu, y = (b >> (i * 8)) & 0xffu;
        s += x > y ? x - y : y - x;
    }
    return s;
}

// gfx12 ADDR3_64KB_2D, 4 bpe: the DWORD index of pixel (x,y) inside one 64 KiB block. Identical to
// n48_addr3_64kb_2d_off_4bpe (scanout_copy.h) with the block term dropped.
static inline uint32_t dpg_off12_dw(uint32_t x, uint32_t y) {
    uint32_t o = 0;
    o |= (x & 1u) << 2;           o |= (y & 1u) << 3;
    o |= ((x >> 1) & 1u) << 4;    o |= ((y >> 1) & 1u) << 5;
    o |= ((x >> 2) & 1u) << 6;    o |= ((y >> 2) & 1u) << 7;
    o |= ((y >> 3) & 1u) << 8;    o |= ((x >> 3) & 1u) << 9;
    o |= ((y >> 4) & 1u) << 10;   o |= ((x >> 4) & 1u) << 11;
    o |= ((y >> 5) & 1u) << 12;   o |= ((x >> 5) & 1u) << 13;
    o |= ((y >> 6) & 1u) << 14;   o |= ((x >> 6) & 1u) << 15;
    return o >> 2;
}
// perm[b] = the address bit that gfx12's address bit b actually occupies. Identity = gfx12 itself.
static void dpg_perm_init(uint8_t *perm) { for (uint32_t b = 0; b < 16u; b++) perm[b] = (uint8_t)b; }
static void dpg_perm_swap(uint8_t *perm, uint32_t a, uint32_t b) { const uint8_t t = perm[a]; perm[a] = perm[b]; perm[b] = t; }

// TOTAL VARIATION of the block reconstructed under `perm`. The layout the bytes are REALLY in is the one that makes the
// reconstruction a picture; every other one scrambles pixels inside each 8x8 unit and raises the sum. This is the
// verdict; it needs no assumption about which axis has the stronger gradient, and it is invariant to any pipeBankXor
// (a constant XOR of the address permutes whole blocks, not pixels within a reconstruction of one block).
static uint64_t dpg_tv(const uint8_t *perm) {
    for (uint32_t y = 0; y < 128u; y++)
        for (uint32_t x = 0; x < 128u; x++) {
            const uint32_t o = dpg_off12_dw(x, y) << 2;
            uint32_t r = 0;
            for (uint32_t b = 2; b < 16u; b++) if (o & (1u << b)) r |= 1u << perm[b];
            gLayoutIdx[y * 128u + x] = (uint16_t)(r >> 2);
        }
    uint64_t s = 0;
    for (uint32_t y = 0; y < 128u; y++)
        for (uint32_t x = 0; x < 128u; x++) {
            const uint32_t v = gLayoutBlock[gLayoutIdx[y * 128u + x]];
            if (x + 1u < 128u) s += dpg_pixdiff(v, gLayoutBlock[gLayoutIdx[y * 128u + x + 1u]]);
            if (y + 1u < 128u) s += dpg_pixdiff(v, gLayoutBlock[gLayoutIdx[(y + 1u) * 128u + x]]);
        }
    return s;
}

// D(b) for address bits 2..15, x256 so the mean survives integer division. i is a DWORD index, so address bit b is
// dword-index bit (b - 2). Each pair is counted once (only offsets with the bit clear).
static void dpg_spectrum(uint32_t nDwords, uint32_t *d256 /* [16] */) {
    for (uint32_t b = 0; b < 16u; b++) d256[b] = 0;
    for (uint32_t b = 2; b < 16u; b++) {
        const uint32_t m = 1u << (b - 2);
        if (m >= nDwords) continue;
        uint64_t sum = 0; uint32_t n = 0;
        for (uint32_t i = 0; i < nDwords; i++) {
            if (i & m) continue;
            sum += dpg_pixdiff(gLayoutBlock[i], gLayoutBlock[i | m]);
            n++;
        }
        d256[b] = n ? (uint32_t)((sum * 256ull) / n) : 0u;
    }
}

// A smooth ramp with independent, NON-WRAPPING x and y gradients inside one 128x128 block — the method controls' image.
static inline uint32_t dpg_ramp(uint32_t x, uint32_t y) {
    const uint32_t lx = x & 127u, ly = y & 127u;
    return ((2u * lx) & 0xffu) | (((2u * ly) & 0xffu) << 8) | (((lx + ly) & 0xffu) << 16);
}
static void dpg_fill_synth(const uint8_t *perm) {       // lay the ramp out in the layout `perm` names
    for (uint32_t y = 0; y < 128u; y++)
        for (uint32_t x = 0; x < 128u; x++) {
            const uint32_t o = dpg_off12_dw(x, y) << 2;
            uint32_t r = 0;
            for (uint32_t b = 2; b < 16u; b++) if (o & (1u << b)) r |= 1u << perm[b];
            gLayoutBlock[r >> 2] = dpg_ramp(x, y);
        }
}

// Run the three named candidates over whatever is in gLayoutBlock and log the verdict. Returns 1 = gfx12, 2 = gfx10
// (either table), 0 = neither margin was met.
static uint32_t dpg_tv_verdict(const char *what) {
    uint8_t p12[16], pRb[16], pN1[16];
    dpg_perm_init(p12);
    dpg_perm_init(pRb); dpg_perm_swap(pRb, 3, 4);                       // gfx10 RbPlus NIBBLE01[39]
    dpg_perm_init(pN1); dpg_perm_swap(pN1, 3, 4); dpg_perm_swap(pN1, 6, 7);   // gfx10 Navi1x NIBBLE01[2]
    const uint64_t t12 = dpg_tv(p12), tRb = dpg_tv(pRb), tN1 = dpg_tv(pN1);
    uint64_t best = t12; uint32_t who = 1;
    if (tRb < best) { best = tRb; who = 2; }
    if (tN1 < best) { best = tN1; who = 3; }
    uint64_t second = 0xffffffffffffffffull;
    if (who != 1 && t12 < second) second = t12;
    if (who != 2 && tRb < second) second = tRb;
    if (who != 3 && tN1 < second) second = tN1;
    // The margin: second-best / best, x100. Below 110 the block has no opinion and this is a NON-OBSERVATION.
    const uint32_t margin = best ? (uint32_t)((second * 100ull) / best) : 0u;
    DPGLOG("layout: %s TOTAL VARIATION over the 128x128 block: gfx12 ADDR3_64KB_2D %llu, gfx10 R_X RbPlus (bits 3,4 "
           "swapped) %llu, gfx10 R_X Navi1x (bits 3,4 and 6,7 swapped) %llu -> LOWEST is %s, margin %u%% of the "
           "runner-up (a margin under 110%% is a NON-OBSERVATION, not a verdict)", what, (unsigned long long)t12,
           (unsigned long long)tRb, (unsigned long long)tN1,
           who == 1 ? "gfx12 ADDR3_64KB_2D" : who == 2 ? "gfx10 ADDR_SW_64KB_R_X (RbPlus)" : "gfx10 ADDR_SW_64KB_R_X (Navi1x)",
           margin);
    if (margin < 110u) return 0;
    return who == 1 ? 1u : 2u;
}

// If none of the named candidates wins, say what WOULD: the single transposition of two address bits, applied to the
// gfx12 map, that minimises total variation. 91 candidates, all in memory, nothing on the hardware.
static void dpg_tv_scan(void) {
    uint8_t p[16];
    dpg_perm_init(p);
    const uint64_t base = dpg_tv(p);
    uint64_t b1v = 0xffffffffffffffffull, b2v = 0xffffffffffffffffull, b3v = 0xffffffffffffffffull;
    uint32_t b1a = 0, b1b = 0, b2a = 0, b2b = 0, b3a = 0, b3b = 0;
    for (uint32_t i = 2; i < 16u; i++)
        for (uint32_t j = i + 1u; j < 16u; j++) {
            dpg_perm_init(p); dpg_perm_swap(p, i, j);
            const uint64_t t = dpg_tv(p);
            if (t < b1v) { b3v = b2v; b3a = b2a; b3b = b2b; b2v = b1v; b2a = b1a; b2b = b1b; b1v = t; b1a = i; b1b = j; }
            else if (t < b2v) { b3v = b2v; b3a = b2a; b3b = b2b; b2v = t; b2a = i; b2b = j; }
            else if (t < b3v) { b3v = t; b3a = i; b3b = j; }
        }
    DPGLOG("layout: SINGLE-TRANSPOSITION SCAN (all 91 pairs of address bits 2..15 swapped in the gfx12 map): gfx12 "
           "itself %llu; best swap (%u,%u) %llu; second (%u,%u) %llu; third (%u,%u) %llu. A swap that beats gfx12 by a "
           "wide margin names the bit that is wrong; none beating it corroborates gfx12", (unsigned long long)base,
           b1a, b1b, (unsigned long long)b1v, b2a, b2b, (unsigned long long)b2v, b3a, b3b, (unsigned long long)b3v);
}

// 0.0.349  — THE BLOCK CENSUS, and it exists because `lay1` measured something nobody predicted:
// the 4 KiB of the live surface it dumped is EXACTLY an affine function of its own address over GF(2) — 0 of 1024
// dwords mismatch the prediction v[i] = v[0] XOR (bit k of i) . (v[1<<k] XOR v[0]). No photograph, no desktop, no
// composited window is an affine function of its address. So block 67 of Apple's surface held a synthetic,
// address-derived pattern, not an image, and a layout verdict taken on it means nothing. This test is the gate:
//   return 0 = the window is CONSTANT (cleared, or never touched)
//   return 1 = the window is XOR-AFFINE in its own address and not constant: synthetic, NOT an image
//   return 2 = neither: it has content an address equation cannot generate, i.e. it may be a picture
static uint32_t dpg_window_kind(const uint32_t *w, uint32_t n, uint32_t *badOut, uint32_t *d1Out) {
    uint32_t bits = 0; while ((1u << bits) < n) bits++;
    bool varies = false;
    for (uint32_t i = 1; i < n; i++) if (w[i] != w[0]) { varies = true; break; }
    uint64_t s = 0; uint32_t c = 0;
    for (uint32_t i = 0; i < n; i += 2u) { s += dpg_pixdiff(w[i], w[i + 1u]); c++; }
    *d1Out = c ? (uint32_t)((s * 256ull) / c) : 0u;
    uint32_t basis[16] = { 0 };
    for (uint32_t k = 0; k < bits && k < 16u; k++) basis[k] = w[1u << k] ^ w[0];
    uint32_t bad = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t p = w[0];
        for (uint32_t k = 0; k < bits && k < 16u; k++) if ((i >> k) & 1u) p ^= basis[k];
        if (p != w[i]) bad++;
    }
    *badOut = bad;
    if (!varies) return 0u;
    return bad == 0u ? 1u : 2u;
}

// The whole surface, one 1 KiB window per 64 KiB block, as one map line: '.' constant, 'a' XOR-affine (synthetic),
// otherwise a hex digit of how far from affine it is. Returns the index of the LEAST affine block, which is the one
// most likely to hold a picture, and 0xffffffff if the reads were refused.
static uint32_t gLayoutCensusWin[256];
static uint32_t dpg_block_census(uint64_t phys, uint32_t nBlocks) {
    char map[160]; uint32_t nConst = 0, nAffine = 0, nOther = 0, bestBad = 0, best = 0xffffffffu;
    uint32_t firstOtherBad = 0, firstOther = 0xffffffffu;
    if (nBlocks > 150u) nBlocks = 150u;
    for (uint32_t b = 0; b < nBlocks; b++) {
        bool ok = true;
        for (uint32_t i = 0; i < 256u && ok; i += 64u)
            ok = navi48_vram_read_mm(phys + (uint64_t)b * 65536ull + (uint64_t)i * 4u, &gLayoutCensusWin[i], 64);
        if (!ok) {
            DPGLOG("layout: CENSUS - MM read REFUSED at block %u. NOTHING WAS MEASURED beyond it: NON-OBSERVATION", b);
            map[b] = '?'; nBlocks = b; break;
        }
        uint32_t bad = 0, d1 = 0;
        const uint32_t k = dpg_window_kind(gLayoutCensusWin, 256u, &bad, &d1);
        if (k == 0u) { nConst++; map[b] = '.'; }
        else if (k == 1u) { nAffine++; map[b] = 'a'; }
        else {
            nOther++; map[b] = (char)("0123456789abcdef"[(bad * 16u) / 256u]);
            if (firstOther == 0xffffffffu) { firstOther = b; firstOtherBad = bad; }
            if (bad > bestBad) { bestBad = bad; best = b; }
        }
    }
    map[nBlocks] = 0;
    DPGLOG("layout: CENSUS of the WHOLE surface, one 1 KiB window per 64 KiB block, %u block(s): %u CONSTANT, %u "
           "XOR-AFFINE in their own address (synthetic, NOT an image), %u with content an address equation cannot "
           "generate. Map ('.' constant, 'a' affine, hex digit = non-affine dwords x16/256): %s", nBlocks, nConst,
           nAffine, nOther, map);
    DPGLOG("layout: CENSUS verdict: %s; least-affine block %u with %u of 256 dword(s) off the affine prediction; first "
           "non-affine block %u (%u of 256)", nOther == 0u ? "NOT ONE BLOCK of Apple's composited surface holds data "
           "that an address equation cannot produce - there is no image in this surface to lay out" :
           "at least one block holds data an address equation cannot produce", best, bestBad, firstOther, firstOtherBad);
    return best;
}

// The surface-change monitor: has ANYTHING written into this surface since the first present? 1 KiB of the reference
// block, re-read on later presents against the snapshot taken on shot 1. Zero every time means the compositor is not
// drawing into the buffer it hands us, and no detile of it can ever be a picture.
static uint32_t gLayoutRef[1024];
static uint64_t gLayoutRefPhys = 0, gLayoutRefOff = 0;
static bool     gLayoutRefOk = false;
static uint32_t gLayoutWatch = 0;
static void dpg_probe_watch(uint64_t phys) {
    if (!gLayoutRefOk || phys != gLayoutRefPhys || gLayoutWatch >= 20u) return;
    uint32_t buf[64] = { 0 }, diff = 0; bool ok = true;
    for (uint32_t i = 0; i < 1024u && ok; i += 64u) {
        ok = navi48_vram_read_mm(phys + gLayoutRefOff + (uint64_t)i * 4u, buf, 64);
        if (ok) for (uint32_t j = 0; j < 64u; j++) if (buf[j] != gLayoutRef[i + j]) diff++;
    }
    gLayoutWatch++;
    DPGLOG("layout: WATCH %u at present %llu, uptime %llu us: %s, %u of 1024 dword(s) of the reference block changed "
           "since the first present. 0 every time = nothing composited into this surface between those presents",
           gLayoutWatch, (unsigned long long)gSh.perform, (unsigned long long)dpg_now_us(), ok ? "read ok" : "read "
           "REFUSED - NON-OBSERVATION", diff);
}

static void dpg_probe_layout(uint64_t phys, uint64_t len, uint32_t surfW, uint32_t surfH) {
    if (gLayoutShots >= 2u || !surfW || !surfH || !phys) return;
    if (gLayoutShots == 1u && phys == gLayoutLastPhys) return;     // shot 2 must be the OTHER double-buffer
    const uint32_t blocksPerRow = (surfW + 127u) >> 7;
    const uint32_t bx = 7u, by = 4u;                  // the block holding pixel (960,540): x 896..1023, y 512..639
    if (bx >= blocksPerRow) return;
    const uint64_t blockOff = ((uint64_t)by * blocksPerRow + bx) * 65536ull;
    if (blockOff + 65536ull > len) return;
    const uint32_t shot = ++gLayoutShots;
    gLayoutLastPhys = phys;

    // THE METHOD CONTROLS, first, on shot 1 only, and they use the SAME buffer and the SAME code the live answer will:
    // a smooth ramp laid out by each candidate. The gfx12-laid ramp must come back "gfx12" and the gfx10-laid ramp must
    // come back "gfx10". If either does not, the method is blind and the live verdict below it means nothing.
    // The affine gate is controlled here too: a ramp is NOT an affine function of its address, so it must read "2".
    if (shot == 1u) {
        uint8_t p[16];
        dpg_perm_init(p);                         dpg_fill_synth(p);
        (void)dpg_tv_verdict("METHOD CONTROL, ramp laid out gfx12:");
        uint32_t cb = 0, cd = 0;
        const uint32_t ck = dpg_window_kind(gLayoutBlock, 256u, &cb, &cd);
        dpg_perm_init(p); dpg_perm_swap(p, 3, 4); dpg_fill_synth(p);
        (void)dpg_tv_verdict("METHOD CONTROL, ramp laid out gfx10-RbPlus:");
        DPGLOG("layout: METHOD CONTROL of the AFFINE GATE: a smooth ramp reads kind %u (%s), %u of 256 dword(s) off the "
               "affine prediction, D(bit2)x256 %u. A picture MUST read kind 2; if this reads 0 or 1 the gate is broken",
               ck, ck == 0u ? "constant" : ck == 1u ? "XOR-AFFINE" : "neither - image-like", cb, cd);
    }

    // THE CENSUS, shot 1 only: is there ANY block in Apple's surface that holds data an address equation cannot make?
    uint32_t best = 0xffffffffu;
    if (shot == 1u) best = dpg_block_census(phys, (uint32_t)(len / 65536ull));

    for (uint32_t pass = 0; pass < 2u; pass++) {
        const uint64_t off = pass == 0u ? blockOff : (uint64_t)best * 65536ull;
        const char *why = pass == 0u ? "block 67, the one holding pixel (960,540)" : "the LEAST AFFINE block on the surface";
        if (pass == 1u && (best == 0xffffffffu || off + 65536ull > len || off == blockOff)) {
            if (shot == 1u) DPGLOG("layout: second pass skipped - %s", best == 0xffffffffu ? "the census found no "
                                   "non-affine block at all" : "it is the same block as the first pass");
            break;
        }
        const uint64_t t0 = dpg_now_us();
        uint32_t reads = 0; bool readOk = true;
        for (uint32_t i = 0; i < 16384u; i += 64u) {
            if (!navi48_vram_read_mm(phys + off + (uint64_t)i * 4u, &gLayoutBlock[i], 64)) {
                DPGLOG("layout: shot %u pass %u (%s) at VRAM %#llx+%#llx - MM read REFUSED at dword %u after %u call(s)."
                       " NOTHING WAS MEASURED: this is a NON-OBSERVATION", shot, pass, why,
                       (unsigned long long)phys, (unsigned long long)off, i, reads);
                readOk = false; break;
            }
            reads++;
        }
        if (!readOk) break;
        const uint64_t t1 = dpg_now_us();
        uint32_t re0[64] = { 0 }, re1[64] = { 0 }, moved = 0;
        const bool reOk = navi48_vram_read_mm(phys + off, re0, 64) &&
                          navi48_vram_read_mm(phys + off + 8192u * 4u, re1, 64);
        if (reOk) for (uint32_t i = 0; i < 64u; i++) {
            if (re0[i] != gLayoutBlock[i]) moved++;
            if (re1[i] != gLayoutBlock[8192u + i]) moved++;
        }
        uint32_t zeros = 0, changes = 0;
        for (uint32_t i = 0; i < 16384u; i++) {
            if (gLayoutBlock[i] == 0u) zeros++;
            if (i && gLayoutBlock[i] != gLayoutBlock[i - 1]) changes++;
        }
        uint32_t aBad = 0, aD1 = 0;
        const uint32_t kind = dpg_window_kind(gLayoutBlock, 1024u, &aBad, &aD1);
        uint32_t d[16];
        dpg_spectrum(16384u, d);
        uint32_t lo = 0xffffffffu, hi = 0;
        for (uint32_t b = 2; b < 16u; b++) { if (d[b] < lo) lo = d[b]; if (d[b] > hi) hi = d[b]; }
        DPGLOG("layout: shot %u pass %u (%s), block %u of the LIVE surface at VRAM %#llx+%#llx, 16384 dwords in %u MM "
               "reads, %llu us; CONTROL re-read %s, %u of 128 dword(s) moved; structure: %u zero dword(s), %u adjacent "
               "changes of 16383; D(b) spread hi/lo %u/%u; AFFINE GATE over the first 1024 dwords: kind %u (%s), %u of "
               "1024 off the prediction", shot, pass, why, (unsigned)(off / 65536ull), (unsigned long long)phys,
               (unsigned long long)off, reads, (unsigned long long)(t1 - t0), reOk ? "ok" : "REFUSED", moved, zeros,
               changes, hi, lo, kind, kind == 0u ? "CONSTANT - no image here" : kind == 1u ? "XOR-AFFINE in its own "
               "address - SYNTHETIC, no image here" : "neither - may be a picture", aBad);
        DPGLOG("layout: shot %u pass %u affine basis (v[1<<k] ^ v[0], k = 0..9) %08x %08x %08x %08x %08x %08x %08x %08x "
               "%08x %08x, v[0] %08x", shot, pass, gLayoutBlock[1] ^ gLayoutBlock[0], gLayoutBlock[2] ^ gLayoutBlock[0],
               gLayoutBlock[4] ^ gLayoutBlock[0], gLayoutBlock[8] ^ gLayoutBlock[0], gLayoutBlock[16] ^ gLayoutBlock[0],
               gLayoutBlock[32] ^ gLayoutBlock[0], gLayoutBlock[64] ^ gLayoutBlock[0], gLayoutBlock[128] ^ gLayoutBlock[0],
               gLayoutBlock[256] ^ gLayoutBlock[0], gLayoutBlock[512] ^ gLayoutBlock[0], gLayoutBlock[0]);
        DPGLOG("layout: shot %u pass %u D(b)x256, address bits 2..15: %u %u %u %u %u %u %u %u %u %u %u %u %u %u", shot,
               pass, d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15]);
        const uint32_t v = dpg_tv_verdict(pass == 0u ? "LIVE APPLE SURFACE, block 67:" : "LIVE APPLE SURFACE, least-affine block:");
        DPGLOG("layout: shot %u pass %u VERDICT: %s", shot, pass, kind != 2u ? "VOID - this block holds no image, so no "
               "layout verdict can be taken from it" : v == 1u ? "Apple's bytes here are in gfx12 ADDR3_64KB_2D - the "
               "mode we already tell SDMA" : v == 2u ? "Apple's bytes here are in gfx10 ADDR_SW_64KB_R_X - NOT what we "
               "tell SDMA" : "NEITHER named candidate reached the margin; see the scan");
        if (shot == 1u) {
            if (kind == 2u) dpg_tv_scan();
            if (pass == 0u) {
                for (uint32_t i = 0; i < 1024u; i++) gLayoutRef[i] = gLayoutBlock[i];
                gLayoutRefPhys = phys; gLayoutRefOff = off; gLayoutRefOk = true;
            }
            for (uint32_t i = 0; i < 512u; i += 8u)
                DPGLOG("layoutraw: pass %u +%#06x: %08x %08x %08x %08x %08x %08x %08x %08x", pass, i * 4u,
                       gLayoutBlock[i], gLayoutBlock[i + 1], gLayoutBlock[i + 2], gLayoutBlock[i + 3],
                       gLayoutBlock[i + 4], gLayoutBlock[i + 5], gLayoutBlock[i + 6], gLayoutBlock[i + 7]);
        }
    }
}

static bool gProbeDone = false;
// 0.0.344: the shim's present asked for the readback exactly once. 0.0.412: replaced by the per-content
// token gate below (`gSh.verifyRuns` / `gSh.lastVerifyToken`), bounded by N48_DPG_VERIFY_BUDGET; see display_pipe_guard.h.
static void dpg_probe_source(void *res, uint64_t phys, uint64_t len, uint32_t pw, uint32_t ph, uint32_t stride) {
    if (gProbeDone || !dpg_kptr(res) || !pw || !ph || !stride) return;
    gProbeDone = true;
    const char *r = static_cast<const char *>(res);
    const uint32_t surf = *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    const void *mask = *reinterpret_cast<const void *const *>(r + 0x180);
    const uint32_t maskSurf = dpg_kptr(mask) ? *reinterpret_cast<const uint32_t *>(static_cast<const char *>(mask) + 0x40) : 0u;
    const uint32_t eff = dpg_kptr(mask) ? maskSurf : surf;          // exactly the choice setupHwCBRegs makes (0xbdf9141)
    const uint32_t packed = *reinterpret_cast<const uint32_t *>(r + 0x1b4);
    DPGLOG("srcdesc: plane 0 resource %p (%s) %ux%u stride %u at VRAM %#llx +%#llx", res, dpg_class(res), pw, ph, stride,
           (unsigned long long)phys, (unsigned long long)len);
    DPGLOG("srcdesc: SURFACE RECORD (%s) = %#010x -> SW_MODE %u = %s, RESOURCE_TYPE %u, bit7 %u  [res+0x180 mask memory %p; "
           "res+0x1dc %#010x; *(mask+0x40) %#010x]", dpg_kptr(mask) ? "*(res+0x180)+0x40" : "res+0x1dc", eff, eff & 0x1fu,
           dpg_sw_mode_name(eff & 0x1fu), (eff >> 5) & 3u, (eff >> 7) & 1u, mask, surf, maskSurf);
    DPGLOG("srcdesc: type+0x14 %#x; geom+0xb0 %ux%u; depth+0xb4 %u; rowBytes+0xb8 %llu; VidMemory+0x88 %p; gpuOff+0xf8 %#llx "
           "+0xfc %#x; packed+0x1b4 %#010x (format %u, +0x1b8 %#x, +0x1b9 %u, +0x1ba %u); alignedPitch+0x1d8 %u; "
           "bytes+0x230 %#llx; alignMgr+0x250 %p; +0x268 %u; +0x26c %u",
           *reinterpret_cast<const uint8_t *>(r + 0x14), *reinterpret_cast<const uint16_t *>(r + 0xb0),
           *reinterpret_cast<const uint16_t *>(r + 0xb2), *reinterpret_cast<const uint32_t *>(r + 0xb4),
           (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0xb8), *reinterpret_cast<void *const *>(r + 0x88),
           (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0xf8), *reinterpret_cast<const uint32_t *>(r + 0xfc),
           packed, packed & 0xffu, *reinterpret_cast<const uint8_t *>(r + 0x1b8),
           *reinterpret_cast<const uint8_t *>(r + 0x1b9), *reinterpret_cast<const uint16_t *>(r + 0x1ba),
           *reinterpret_cast<const uint32_t *>(r + 0x1d8), (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0x230),
           *reinterpret_cast<void *const *>(r + 0x250), *reinterpret_cast<const uint32_t *>(r + 0x268),
           *reinterpret_cast<const uint32_t *>(r + 0x26c));
    const uint32_t *d = reinterpret_cast<const uint32_t *>(r);
    for (uint32_t i = 0; i + 8u <= 152u; i += 8u)
        DPGLOG("srcdesc: res+%#05x: %08x %08x %08x %08x %08x %08x %08x %08x", i * 4u, d[i], d[i + 1], d[i + 2], d[i + 3],
               d[i + 4], d[i + 5], d[i + 6], d[i + 7]);
    DPGLOG("srcdesc: res+0x260: %08x %08x %08x %08x %08x %08x  (object size 0x278, CONFIRMED 0xbdf9838)",
           d[152], d[153], d[154], d[155], d[156], d[157]);

    // The bytes. Three whole rows plus a column strip, all read-only, all through the MM window.
    dpg_probe_row("row 0", phys, pw);
    dpg_probe_row("row h/2", phys + (uint64_t)(ph / 2u) * stride, pw);
    dpg_probe_row("row h-1", phys + (uint64_t)(ph - 1u) * stride, pw);
    uint32_t col[16] = { 0 };
    bool colOk = true;
    for (uint32_t y = 0; y < 16u && colOk; y++)
        colOk = navi48_vram_read_mm(phys + (uint64_t)y * stride, &col[y], 1);
    if (colOk)
        DPGLOG("srcbytes: column x=0, y=0..15 (one dword per row, stride %u): %08x %08x %08x %08x %08x %08x %08x %08x "
               "%08x %08x %08x %08x %08x %08x %08x %08x", stride, col[0], col[1], col[2], col[3], col[4], col[5], col[6],
               col[7], col[8], col[9], col[10], col[11], col[12], col[13], col[14], col[15]);
    else
        DPGLOG("srcbytes: column x=0 - MM read REFUSED; NON-OBSERVATION");
    // Control: the same 8 dwords read twice. Equal = the MM path is live and the source held still across the probe;
    // unequal = the source is being rewritten under us and every statistic above is a blur, not a reading.
    uint32_t a[8] = { 0 }, b[8] = { 0 };
    const bool okA = navi48_vram_read_mm(phys, a, 8), okB = navi48_vram_read_mm(phys, b, 8);
    uint32_t diff = 0;
    for (uint32_t i = 0; i < 8u; i++) if (a[i] != b[i]) diff++;
    DPGLOG("srcbytes: CONTROL re-read of row 0 head: reads %s/%s, %u of 8 dword(s) changed between them (%08x vs %08x); "
           "0 = stable source, non-zero = the surface moved under the probe", okA ? "ok" : "REFUSED",
           okB ? "ok" : "REFUSED", diff, a[0], b[0]);
}

// 0.0.345 — THE ARRANGEMENT, read from the resource exactly the way Apple's own setupHwCBRegs reads it,
// so the numbers we hand SDMA are the numbers the CB was programmed with and nothing is guessed.
//
// CONFIRMED from com.apple.kext.AMDRadeonX6000 (addresses + bytes), AMDGFX10Resource::setupHwCBRegs @0xbdf90c6:
//   the swizzle mode, CB_COLOR0_ATTRIB3.COLOR_SW_MODE (rec+0x4c bits 18:14):
//     0xbdf9133  leaq 0x40(%rax),%rcx        ; rax = res+0x180 (mask memory)
//     0xbdf9137  leaq 0x1dc(%r14),%rsi
//     0xbdf9141  cmoveq %rsi,%rcx            ; rcx = res+0x180 ? *(res+0x180)+0x40 : res+0x1dc
//     0xbdf92e8  andl $0x1f,%ecx / shll $0xe,%ecx / movl %ecx,0x4c(%rbx)
//   CB_COLOR0_ATTRIB2 (rec+0x48): MIP0_HEIGHT [13:0], MIP0_WIDTH [27:14]  (xlat12_repack.h:2447-2453)
//     0xbdf933d  movq 0x180(%r14),%rcx
//     0xbdf9349  testb $0x40,0x6(%rcx) / jne 0xbdf9358
//     0xbdf934f  leaq 0xb2(%r14),%rcx        ; res+0xb2 = the u16 HEIGHT
//     0xbdf9358  addq $0x2,%rcx              ; or mask+0x2 when that bit is set
//     0xbdf935c  movzwl (%rcx),%edx / decl %edx / andl $0x3fff,%edx      -> MIP0_HEIGHT = h - 1
//     0xbdf9374  movl 0xb0(%r14),%edx        ; res+0xb0 = the u16 WIDTH (low half of the 32-bit load)
//     0xbdf937b  shll $0xe,%edx / addl $0xfffc000,%edx / andl $0xfffc000,%edx  -> MIP0_WIDTH = (w - 1) << 14
// THIS SETTLES's SUSPECTED ITEM: the extent the hardware is told is the LOGICAL surface size, res+0xb0/0xb2,
// NOT the padded height at res+0x1ba. It is also the same field radv passes (img_extent_el, radv_sdma.c:169-171).
// For a 1920 x 1080 surface the choice happens not to change one address - ceil(1080/128) and ceil(1152/128) are both
// 9 block rows - but "it does not matter here" is not a reason to send the wrong number.
//
// THE MODE WE SEND SDMA IS THE MODE THE CB GOT, BY CONSTRUCTION. src/xlat12/xlat12_repack.h:2548 writes
// CB_COLOR0_ATTRIB3.COLOR_SW_MODE as `((v >> 14) & 0x7u) << 15`, i.e. the LOW THREE BITS of Apple's five-bit
// AddrSwizzleMode. So the bytes gfx1201's CB laid down are in gfx12 enum `appleMode & 7` - for Apple's 27
// (ADDR_SW_64KB_R_X) that is 3, ADDR3_64KB_2D. Mirroring the truncation here is what makes the two ends agree; it is
// NOT an endorsement of the truncation, which is a real latent bug for modes 21-24 and is briefed separately.
static bool shim_surface_desc(void *res, uint32_t *swzOut, uint32_t *wOut, uint32_t *hOut, uint32_t *appleOut) {
    *swzOut = 0; *wOut = 0; *hOut = 0; *appleOut = 0xffffffffu;
    if (!dpg_kptr(res)) return false;
    const char *r = static_cast<const char *>(res);
    const void *mask = *reinterpret_cast<const void *const *>(r + 0x180);
    const bool maskOk = dpg_kptr(mask);
    const uint32_t rec = maskOk ? *reinterpret_cast<const uint32_t *>(static_cast<const char *>(mask) + 0x40)
                                : *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    const uint32_t appleMode = rec & 0x1fu;
    const uint32_t w = *reinterpret_cast<const uint16_t *>(r + 0xb0);
    const bool useMaskH = maskOk && (*reinterpret_cast<const uint8_t *>(static_cast<const char *>(mask) + 6) & 0x40u);
    const uint32_t h = useMaskH ? *reinterpret_cast<const uint16_t *>(static_cast<const char *>(mask) + 2)
                                : *reinterpret_cast<const uint16_t *>(r + 0xb2);
    if (w == 0 || h == 0) return false;
    *appleOut = appleMode;
    *swzOut = appleMode & 0x7u;
    *wOut = w; *hOut = h;
    return true;
}

static void dpg_note(const char *what, void *self, uint64_t n) {
    if (gPg.logged >= 32u) return;
    gPg.logged++;
    char pn[20] = { 0 };
    proc_selfname(pn, (int)sizeof(pn));
    DPGLOG("pipeguard: REFUSED %s #%llu on %p (%s) from pid %d (%s)", what, (unsigned long long)n, self, dpg_class(self),
           proc_selfpid(), pn);
}

static void *dpg_initFb(void *self, uint32_t idx, void *res) {
    gPg.initFb++;
    // Route A (active only when armed): return the kext-owned object and set readiness ourselves, but ONLY for a guarded
    // pipe whose +0xe0 is exactly the resource init just handed us. Anything unexpected falls through to the safety
    // core's NULL, which is the default (0.0.294) behaviour.
    if (gRa.mode) {
        for (unsigned i = 0; i < kDpgMaxPipes; i++) {
            if (gPg.pipe[i] == self && gRa.obj[i] && res == gRa.res[i] &&
                res == *reinterpret_cast<void *const *>(static_cast<char *>(self) + 0xe0)) {
                gRa.initFbHits++;
                ra_route_a_ready(self, res);
                (void)idx;
                return gRa.obj[i];
            }
        }
        // Route A is on but this call did not match a guarded pipe/resource - record it so a run can tell "mode off"
        // (gRa.mode 0) from "called but identity mismatch" (this counter > 0). Falls through to the safe NULL return.
        gRa.identityMiss++;
        if (gRa.logged < 16u) {
            gRa.logged++;
            DPGLOG("routea: slot 267 called with self %p res %p but no guarded match (pipe[0] %p, res[0] %p, self+0xe0 %p) - "
                   "returning NULL (safe)", self, res, gPg.pipe[0], gRa.res[0],
                   *reinterpret_cast<void *const *>(static_cast<char *>(self) + 0xe0));
        }
    }
    dpg_note("initFramebufferResource (no VidMemory over the scanout; the pipe stays inactive)", self, gPg.initFb);
    (void)idx; (void)res; return nullptr;
}
static void dpg_destroyFb(void *self, uint32_t idx, void *res) {
    gPg.destroyFb++;
    if (gPg.origDestroy) gPg.origDestroy(self, idx, res);
}
// 0.0.329 : these two were counting stubs that NEVER called Apple's original. Slot 273 is what arms
// the interrupt driving IOAccelDisplayPipe::event_interrupt_gated (0x145ce7ea) - the only path that reaches
// validateTransaction/performTransaction. Swallowing it would have hidden the first call we ever received. The chain is
// address-guarded exactly like every other Apple function we call; a failed guard counts and does NOT call.
static void dpg_enableIrq(void *self) {
    gPg.enableIrq++;
    if (!gSh.mode) dpg_note("enableTransactionInterrupt", self, gPg.enableIrq);
    if (n48_dpg_chain_ok(reinterpret_cast<uint64_t>(gPg.origEnableIrq), gPg.slide, N48_DPG_CHAIN_ENABLE_IRQ)) {
        gPg.chainedEnable++;
        gPg.origEnableIrq(self);
    } else {
        gPg.chainRefused++;
    }
}
static void dpg_disableIrq(void *self) {
    gPg.disableIrq++;
    if (n48_dpg_chain_ok(reinterpret_cast<uint64_t>(gPg.origDisableIrq), gPg.slide, N48_DPG_CHAIN_DISABLE_IRQ)) {
        gPg.chainedDisable++;
        gPg.origDisableIrq(self);
    } else {
        gPg.chainRefused++;
    }
}
static uint32_t dpg_validate(void *self, void *txn) {
    gPg.validate++;
    if (!gSh.mode) { dpg_note("validateTransaction", self, gPg.validate); (void)txn; return N48_DPG_REFUSE_IORETURN; }
    // Accept. AMD's own validateTransaction reaches executeTransaction (0xbdccfe6 -> 0xbdcd049); ours never does, so no
    // PM4 flip is built and nothing is pushed to a live channel.
    gSh.validate++;
    (void)txn;
    return 0;
}
// The shim's present. IOAcceleratorFamily2 has already set pipe+0x248 = txn (0x145ce903) and will retire the ring slot
// the moment this returns 0 (`incl 0x238` at 0x145ceb2b), then ask isTransactionComplete and complete the transaction
// inline. Everything here is synchronous, so the 4-deep ring can never fill.
static uint32_t dpg_perform(void *self, void *txn) {
    gPg.perform++;
    if (!gSh.mode) { dpg_note("performTransaction", self, gPg.perform); (void)txn; return N48_DPG_REFUSE_IORETURN; }
    gSh.perform++;
    if (!dpg_kptr(txn)) return 0;
    const char *t = static_cast<const char *>(txn);
    const uint64_t dirty = *reinterpret_cast<const uint64_t *>(t + 0x48);
    const uint32_t opts = *reinterpret_cast<const uint32_t *>(t + 0x50);
    const uint32_t tid = *reinterpret_cast<const uint32_t *>(t + 0x54);
    void *arr = *reinterpret_cast<void *const *>(t + 0x38);
    const bool log = gSh.logged < kShimCensusLogBudget;
    if (log) gSh.logged++;
    if (!dpg_kptr(arr)) {
        if (log) DPGLOG("pipeshim: perform #%llu on %p: txn+0x38 plane array %p is not a kernel pointer - nothing presented",
                        (unsigned long long)gSh.perform, self, arr);
        return 0;
    }
    const uint32_t n = 1u;                    // entry 0 only until the array's length is proven (see the note above)
    gSh.census++;
    if (log)
        DPGLOG("pipeshim: perform #%llu on %p: transaction %p id %u dirty bits %#llx options %#x, plane array %p (reading "
               "entry 0 only of stride %#x)", (unsigned long long)gSh.perform, self, txn, tid, (unsigned long long)dirty,
               opts, arr, kShimPlaneStride);
    uint64_t phys = 0, len = 0; uint32_t pw = 0, ph = 0, stride = 0; const char *why = "no plane";
    bool have = false;
    void *planeRes = nullptr;                 // 0.0.344: entry 0's IOAccelResource2, for the arrangement probe
    for (uint32_t i = 0; i < n; i++) {
        const char *e = static_cast<const char *>(arr) + (size_t)i * kShimPlaneStride;
        void *res = *reinterpret_cast<void *const *>(e + 0x30);
        void *res2 = *reinterpret_cast<void *const *>(e + 0x38);
        uint64_t p = 0, l = 0; uint32_t w = 0, h = 0, s = 0; const char *w2 = "";
        const bool ok = shim_plane_placement(res, &p, &l, &w, &h, &s, &w2);
        if (i == 0) { phys = p; len = l; pw = w; ph = h; stride = s; why = w2; have = ok; planeRes = res; }
        if (ok) gSh.planes++;
        if (log) {
            // The rects are IEEE-754 singles. The kernel must not touch the FPU, so they are decoded with integer
            // arithmetic only (dpg_f32_int) and never loaded into a float register.
            const uint32_t *f = reinterpret_cast<const uint32_t *>(e + 0x48);
            DPGLOG("pipeshim: perform #%llu plane %u/%u: resource %p (%s) second %p; %ux%u stride %u; VidMemory VRAM %#llx"
                   " +%#llx%s%s; src rect (%d,%d %dx%d) dst (%d,%d %dx%d)", (unsigned long long)gSh.perform, i, n, res,
                   dpg_class(res), res2, w, h, s, (unsigned long long)p, (unsigned long long)l, ok ? "" : " - UNUSABLE: ",
                   ok ? "" : w2, dpg_f32_int(f[0]), dpg_f32_int(f[1]), dpg_f32_int(f[2]), dpg_f32_int(f[3]),
                   dpg_f32_int(f[4]), dpg_f32_int(f[5]), dpg_f32_int(f[6]), dpg_f32_int(f[7]));
        }
    }
    gSh.lastPlaneW = pw; gSh.lastPlaneH = ph; gSh.lastPhys = phys; gSh.lastLen = len;
    // 0.0.344: the arrangement probe, ONE SHOT, before anything is copied and in BOTH modes. It only reads.
    // 0.0.348: and the layout discriminator, TWO shots (one per double-buffer), also read-only and also before the copy.
    if (have) { dpg_probe_source(planeRes, phys, len, pw, ph, stride); dpg_probe_layout(phys, len, pw, ph);
                dpg_probe_watch(phys); }
    if (gSh.mode < 2) return 0;           // census only: present nothing
    if (!have || !pw || !ph || !stride) {
        gSh.copyRefused++; gSh.lastCopySt = 0xffffffffu;
        if (log) DPGLOG("pipeshim: present REFUSED - plane 0 has no usable VRAM placement (%s)", why);
        return 0;
    }
    // 0.0.345: WHICH COPY. CONFIRMED this surface is tiled, so the row copy that ran here through
    // 0.0.344 could never produce a correct frame from it. The arrangement decides, and it is read from the resource,
    // not assumed: swizzle 3 (ADDR3_64KB_2D, what Apple's 27 becomes after our own register translation) takes the
    // COPY_TILED_SUB_WINDOW detile; swizzle 0 (ADDR3_LINEAR) keeps the proven row path; anything else is REFUSED
    // rather than guessed, because emitting a packet for a layout we have no address equation for would put unknown
    // bytes on the glass and tell us nothing.
    uint32_t swz = 0, surfW = 0, surfH = 0, appleMode = 0xffffffffu;
    const bool descOk = shim_surface_desc(planeRes, &swz, &surfW, &surfH, &appleMode);
    if (gSh.descLogged < 4u) {
        gSh.descLogged++;
        DPGLOG("pipeshim: plane 0 arrangement: Apple SW_MODE %u -> gfx12 ADDR3 %u (xlat12_repack.h:2548 keeps the low 3 "
               "bits, which is what the CB was programmed with); CB_COLOR0_ATTRIB2 MIP0_WIDTH/HEIGHT %u x %u (the "
               "LOGICAL extent at res+0xb0/0xb2, not the padded %u at res+0x1ba) -> %s", appleMode, swz, surfW, surfH,
               dpg_kptr(planeRes) ? *reinterpret_cast<const uint16_t *>(static_cast<const char *>(planeRes) + 0x1ba) : 0,
               !descOk ? "UNREADABLE - refusing"
                       : (swz == 3u ? (gSh.forceLinear ? "TILED source, FORCED LINEAR copy (S1)" : "TILED detile copy")
                                    : (swz == 0u ? "LINEAR row copy" : "REFUSED, no address equation for this mode")));
    }
    if (!descOk || (swz != 0u && swz != 3u)) {
        gSh.copyRefused++; gSh.lastCopySt = 0xfffffffeu;
        return 0;
    }
    // build 0.0.516 (switch 73, gfx_present73.h;  (a),): ON, the copy is made only when plane 0's last P
    // COMMITTED (keyed by `phys`, this plane's VRAM offset); anything else HOLDS it - no copy, no readback, the glass keeps its
    // last delivered picture - counted and reported by `gfxneuter 73`. OFF (the default) this is one call answering 0 and the
    // present below is 0.0.515's, byte for byte.
    // build 0.0.538 (switch 95, gfx_p95.h;  PLAN item 2): ON (with 73 ON), hw_p95_present asks switch 73's SAME
    // question for this plane and, when 73 holds it, may REPLAY a remembered held present whose P has COMMITTED since: it then
    // swaps the copy's source and geometry below to that plane's (the same copy path, the same locks). OFF: 0.0.537's question.
    if (n48::hw_p95_on()) {
        if (!n48::hw_p95_present(&phys, &len, &pw, &ph, &stride, &surfW, &surfH, &swz, gSh.perform)) return 0;
    } else
    if (n48::hw_p73_on() && !n48::hw_p73_present(phys, gSh.perform)) return 0;
    uint64_t cv[11] = { 0 };
    // 0.0.344 asked for the readback ONCE, on the first present only. 0.0.412 runs it on the FIRST
    // PRESENT AFTER EACH CHANGE of the presented plane's content token, and only for a bounded number of presents.
    // The copy itself is identical either way; what the readback adds is the destination-vs-source comparison
    // (rows 0 and h-1 in full, PLUS the 16x16 cell map), the one measurement that separates "the source is arranged
    // differently" from "our SDMA copy delivers the wrong bytes". A present whose verification disagrees
    // becomes status 11 (kScanStReadback) and counts as DELIVERED, not refused (0.0.373,) — expected, and exactly
    // the observation S2 exists to make.
    //
    // THE CONTENT TOKEN IS THE PLANE'S VRAM OFFSET, which the present path already reads as `phys`: WindowServer
    // presents a different buffer by naming a different offset in the plane array. No new state is invented. The
    // token gate and the bound are the pure n48_dpg_verify_due (display_pipe_guard.h), host-tested.
    const bool verifyNow = n48_dpg_verify_due(phys, gSh.lastVerifyToken, gSh.verifyRuns) != 0;
    if (verifyNow) { gSh.lastVerifyToken = phys; gSh.verifyRuns++; }
    // 0.0.412: `pipeshim 4` forces a swizzle-3 plane down the EXISTING linear row copy — the SAME
    // navi48_scanout_copy_vram call the swizzle-0 branch already makes, with the same arguments (pw/ph/stride) and the
    // same destination. No new copy mechanism, no new destination. DEFAULT OFF: gSh.forceLinear is 0 for every other
    // argument, so with S1 off this is 0.0.411's tiled call, byte for byte.
    // 0.0.416 (notes/design/SDMA-GCR.md, G3): ARG 5 is a TILED copy with the GCR_REQ in the same submission. It
    // is meaningful only for the tiled path (there is no COPY_TILED_SUB_WINDOW on the linear path, and the live
    // plane is swizzle 3); gSh.gcr is reported in the control line either way.
    const bool linearCopy = (swz != 3u) || (gSh.forceLinear != 0u);
    // build 0.0.518 (switch 74, flip mode; , apple/gfx_flipmode.h). Asked only AFTER switch 73's hold above, which
    // stays first. ON: the present is detiled into the buffer the display is NOT scanning (A = the console, B = flip mode's), then
    // HUBP0 is flipped to it at VUPDATE (navi48_fm_present: the bounded two-frame wait for the previous flip's latch, the front
    // from EARLIEST_INUSE, the copy, the flip); a held present returns here with no copy. OFF (the default): one load and the
    // copy below, 0.0.517's, unchanged.
    uint32_t cst = 0;
    if (navi48_fm_on()) {
        if (navi48_fm_present(phys, len, surfW, surfH, swz, pw, ph, linearCopy, verifyNow, gSh.gcr != 0u, gSh.perform, cv, 11,
                              &cst) != 0u)
            return 0;
    } else cst = !linearCopy
        ? navi48_scanout_copy_tiled(phys, len, surfW, surfH, swz, 0, 0, pw, ph, cv, 11, verifyNow, gSh.gcr != 0u)
        : navi48_scanout_copy_vram(phys, len, pw, ph, stride, 0, 0, pw, ph, cv, 11, verifyNow);
    gSh.lastCopySt = cst; gSh.lastUs = cv[4];
    // 0.0.373: three buckets, not two. Status 0 is a clean copy; kScanStReadback is a copy that HAPPENED
    // and whose post-hoc verification disagreed (the pixels are on the glass either way); anything else is a copy that
    // was not made. Nothing else about the present path changes.
    switch (n48_dpg_present_bucket(cst)) {
    case N48_DPG_PRESENT_OK:       gSh.copyOk++;       break;
    case N48_DPG_PRESENT_READBACK: gSh.copyReadback++; break;
    default:                       gSh.copyRefused++;  break;
    }
    // build 0.0.521 Part E (switch 73, gfx_present73.h): a copy that HAPPENED (clean or readback) put the P that 73 let through
    // on the glass: later copies must carry a NEWER P (monotonic). A copy not made changes nothing. OFF: one load.
    if (n48::hw_p73_on() && n48_dpg_present_bucket(cst) != N48_DPG_PRESENT_REFUSED) n48::hw_p73_delivered();
    // build 0.0.539: a replay chosen at THIS present is re-checked now that its copy returned (the
    // slot's seq, state and gate stamp against the pick's; a race counted and logged). Nothing pending: one load. Log-only.
    n48::hw_p95_after_copy(gSh.perform, n48_dpg_present_bucket(cst) != N48_DPG_PRESENT_REFUSED ? 1u : 0u);
    if (log || verifyNow)
        DPGLOG("pipeshim: present #%llu%s: SDMA0 QUEUE0 %s copy of plane 0 (VRAM %#llx, %ux%u stride %u, surface %ux%u "
               "swizzle %u) into the scanout at (0,0) -> status %u, plan %llu, %llu us", (unsigned long long)gSh.perform,
               verifyNow ? " (READBACK VERIFIED)" : "",
               !linearCopy ? (gSh.gcr ? "TILED (one packet + GCR_REQ)" : "TILED (one packet)")
                           : (swz == 3u ? "linear (FORCED - S1, one packet per row)" : "linear (one packet per row)"),
               (unsigned long long)phys, pw, ph, stride, surfW, surfH, swz, cst, (unsigned long long)cv[1],
               (unsigned long long)cv[4]);
    // 0.0.348 — THE HEARTBEAT, and it closes a named instrument gap, not a hunch. The user watched `tile4` do "3 frames,
    // then nothing for 10 s, then bursts of ~10 frames a second apart, then stuck, then dead", and EVERY counter the
    // shim had stopped at the 24-line census budget, so the burst phase was invisible. One line every 5th present, 60
    // of them (presents 5..300), each stamped with the uptime and the running totals: log only, nothing else changes.
    // 0.0.349: every 5th, not every 25th - lay1 reached present 17 in its whole armed window and fired none.
    if ((gSh.perform % 5ull) == 0ull && gSh.beats < 60u) {
        gSh.beats++;
        gSh.beatUs = dpg_now_us(); gSh.beatPerform = gSh.perform;
        gSh.beatOk = gSh.copyOk; gSh.beatRefused = gSh.copyRefused; gSh.beatReadback = gSh.copyReadback;
        DPGLOG("pipeshim: HEARTBEAT %u at uptime %llu us: perform %llu, census %llu, presents ok/readback-differed/refused "
               "%llu/%llu/%llu (DELIVERED %llu = ok + readback-differed; only `refused` means no copy was made), last "
               "status %llu, last copy %llu us, plane VRAM %#llx", gSh.beats, (unsigned long long)gSh.beatUs,
               (unsigned long long)gSh.perform, (unsigned long long)gSh.census, (unsigned long long)gSh.copyOk,
               (unsigned long long)gSh.copyReadback, (unsigned long long)gSh.copyRefused,
               (unsigned long long)(gSh.copyOk + gSh.copyReadback), (unsigned long long)gSh.lastCopySt,
               (unsigned long long)gSh.lastUs, (unsigned long long)phys);
    }
    return 0;
}
static bool dpg_isComplete(void *self, void *txn) { gPg.isComplete++; gSh.isComplete += gSh.mode ? 1u : 0u; (void)self; (void)txn; return true; }
// CONFIRMED (0x145ce90e-0x145ce91e): event_interrupt_gated enters the perform block only for txn+0x58 in
// {0xE0014042, 0xE00002D8}, and consults the AMD event machine's fence (accel+0x380 slot 48) only for 0xE0014042
// (0x145ce895). kIOReturnNotReady is therefore the ONLY submit value that both reaches our performTransaction and keeps
// the event machine's fence path out of the run.
#define N48_SHIM_SUBMIT_NOT_READY 0xe00002d8u
static uint32_t dpg_submit(void *self, void *txn) {
    gPg.submit++;
    if (!gSh.mode) { dpg_note("submitTransaction", self, gPg.submit); (void)txn; return N48_DPG_REFUSE_IORETURN; }
    gSh.submit++;
    (void)txn;
    return N48_SHIM_SUBMIT_NOT_READY;
}
static void dpg_begin(void *self, void *ev) {
    gPg.begin++;
    if (!gSh.mode) dpg_note("beginTransaction", self, gPg.begin);   // shim: a silent no-op (no AMD event begin)
    (void)ev;
}
static void dpg_signal(void *self, void *ev) { gPg.signal++; (void)self; (void)ev; }
static bool dpg_flip(void *self, uint32_t a, void *b, void *c, uint32_t d, void *e) {
    gPg.flip++; dpg_note("getDisplayPipeTransactionFlip (no PM4 flip is built)", self, gPg.flip);
    (void)a; (void)b; (void)c; (void)d; (void)e; return false;
}

static bool dpg_vt_matches(void **vt, uintptr_t slide, const n48_dpg_slot *s, unsigned n, unsigned *bad) {
    *bad = 0;
    for (unsigned i = 0; i < n; i++) {
        if (reinterpret_cast<uintptr_t>(vt[s[i].slot]) - slide != (uintptr_t)s[i].target) { *bad = i + 1; return false; }
    }
    return true;
}

static void **dpg_make_copy(void **vt, unsigned slots) {
    void **copy = static_cast<void **>(IOMalloc((slots + kVtHdr) * sizeof(void *)));
    if (copy) memcpy(copy, vt - kVtHdr, (slots + kVtHdr) * sizeof(void *));
    return copy;
}

// Status: 0 armed, 1 no accelerator, 2 slide, 3 display machine identity, 4 framebuffer count, 5 pipe identity, 6 pipe slot,
// 7 display identity, 8 display slot, 9 allocation, 10 WRITE_DATA guard refused, 11 no pipe yet (adoption not run),
// 12 an object already carries a foreign vtable; 14-17 the event-machine census (14 accel+0x380 not the AMD event machine,
// 15 a census slot's identity, 16 allocation, 17 vptr swap) - the safety core does not arm unless the census does too.
static uint32_t dpg_arm_locked() {
    if (gPg.armed) return 0;
    void *accel = navi48_accel_object();
    void **accelVt = navi48_accel_vtable_copy();
    if (!accel || !accelVt) return 1;
    uint32_t sr = 0;
    const uintptr_t slide = navi48_x6000_slide(&sr);
    if (!slide || (slide & 0xfff)) { DPGLOG("pipeguard: REFUSED - X6000 slide unavailable (reason %u)", sr); return 2; }
    void *dm = *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff);
    if (!dpg_is(dm, slide, kDpgDmGetMeta, "AMDRadeonX6000_AMDAccelDisplayMachine")) {
        DPGLOG("pipeguard: REFUSED - accel+0x378 %p is not the AMD display machine (vtable getMetaClass or class name)", dm);
        return 3;
    }
    void **dmVt = *reinterpret_cast<void ***>(dm);
    if (reinterpret_cast<uintptr_t>(dmVt[0]) - slide != kDpgDmD1 || reinterpret_cast<uintptr_t>(dmVt[7]) - slide != kDpgDmGetMeta) {
        DPGLOG("pipeguard: REFUSED - display machine vtable D1 %#lx getMetaClass %#lx minus slide (want %#lx %#lx)",
               (unsigned long)(reinterpret_cast<uintptr_t>(dmVt[0]) - slide), (unsigned long)(reinterpret_cast<uintptr_t>(dmVt[7]) - slide),
               (unsigned long)kDpgDmD1, (unsigned long)kDpgDmGetMeta);
        return 3;
    }
    const uint32_t fbCount = *reinterpret_cast<uint32_t *>(static_cast<char *>(dm) + kDpgDmFbCountOff);
    if (fbCount > kDpgMaxPipes) { DPGLOG("pipeguard: REFUSED - framebuffer count %u", fbCount); return 4; }
    if (fbCount == 0) { DPGLOG("pipeguard: REFUSED - no framebuffer adopted yet (run m4-adopt first)"); return 11; }
    void *pipes[kDpgMaxPipes] = { nullptr }, *disps[kDpgMaxPipes] = { nullptr };
    void **pvt[kDpgMaxPipes] = { nullptr }, **dvt[kDpgMaxPipes] = { nullptr };
    for (uint32_t i = 0; i < fbCount; i++) {
        void *p = *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff + 8u * i);
        if (!dpg_is(p, slide, kDpgPipeGetMeta, "AMDRadeonX6000_AMDAccelDisplayPipe")) {
            DPGLOG("pipeguard: REFUSED - pipe[%u] %p is not an AMD display pipe (vtable getMetaClass or class name)", i, p); return 5;
        }
        void **vt = *reinterpret_cast<void ***>(p);
        unsigned bad = 0;
        if (!dpg_vt_matches(vt, slide, kN48PipeGuardSlots, N48_DPG_N_PIPE_GUARD, &bad)) {
            const n48_dpg_slot &s = kN48PipeGuardSlots[bad - 1];
            DPGLOG("pipeguard: REFUSED - pipe[%u] %p slot %u (%s) = %#lx minus slide, want %#x%s", i, p, s.slot, s.name,
                   (unsigned long)(reinterpret_cast<uintptr_t>(vt[s.slot]) - slide), s.target,
                   (gPg.pipeCopy && vt == gPg.pipeCopy + kVtHdr) ? " (already our copy?)" : "");
            return (gPg.pipeCopy && vt == gPg.pipeCopy + kVtHdr) ? 12 : 6;
        }
        void *d = *reinterpret_cast<void **>(static_cast<char *>(p) + kDpgPipeDisplayOff);
        if (!dpg_is(d, slide, kDpgDispGetMeta, "AMDRadeonX6000_AMDNavi21Display")) {
            DPGLOG("pipeguard: REFUSED - pipe[%u]+0x330 %p is not the Navi21 display (vtable getMetaClass or class name)", i, d); return 7;
        }
        void **dv = *reinterpret_cast<void ***>(d);
        if (!dpg_vt_matches(dv, slide, kN48DispGuardSlots, N48_DPG_N_DISP_GUARD, &bad)) {
            const n48_dpg_slot &s = kN48DispGuardSlots[bad - 1];
            DPGLOG("pipeguard: REFUSED - display %p slot %u (%s) = %#lx minus slide, want %#x", d, s.slot, s.name,
                   (unsigned long)(reinterpret_cast<uintptr_t>(dv[s.slot]) - slide), s.target);
            return 8;
        }
        pipes[i] = p; disps[i] = d; pvt[i] = vt; dvt[i] = dv;
    }
    if (n48::hw_hook_dpg_writedata(1, nullptr, 0) != 0) {
        DPGLOG("pipeguard: REFUSED - the GFX-ring WRITE_DATA guard could not arm (no GFX writeTail hook this boot)");
        return 10;
    }
    // One shared copy per class: every pipe/display with the verified table gets the same replacement.
    void **pc = dpg_make_copy(pvt[0], N48_DPG_PIPE_SLOTS);
    void **dc = dpg_make_copy(dvt[0], N48_DPG_DISP_SLOTS);
    if (!pc || !dc) {
        if (pc) IOFree(pc, (N48_DPG_PIPE_SLOTS + kVtHdr) * sizeof(void *));
        if (dc) IOFree(dc, (N48_DPG_DISP_SLOTS + kVtHdr) * sizeof(void *));
        return 9;
    }
    gPg.origDestroy = reinterpret_cast<DpgDestroyFbFn>(pvt[0][268]);
    // 0.0.329: keep 273/274 so the stubs can chain. dpg_vt_matches has already proven both name their static target at
    // this slide; gPg.slide is set below, BEFORE the vptr swap, so a hook can never run with a stale slide.
    gPg.origEnableIrq  = reinterpret_cast<DpgIrqFn>(pvt[0][273]);
    gPg.origDisableIrq = reinterpret_cast<DpgIrqFn>(pvt[0][274]);
    pc[kVtHdr + 267] = reinterpret_cast<void *>(&dpg_initFb);
    pc[kVtHdr + 268] = reinterpret_cast<void *>(&dpg_destroyFb);
    pc[kVtHdr + 273] = reinterpret_cast<void *>(&dpg_enableIrq);
    pc[kVtHdr + 274] = reinterpret_cast<void *>(&dpg_disableIrq);
    pc[kVtHdr + 276] = reinterpret_cast<void *>(&dpg_validate);
    pc[kVtHdr + 277] = reinterpret_cast<void *>(&dpg_perform);
    pc[kVtHdr + 278] = reinterpret_cast<void *>(&dpg_isComplete);
    pc[kVtHdr + 279] = reinterpret_cast<void *>(&dpg_submit);
    pc[kVtHdr + 283] = reinterpret_cast<void *>(&dpg_begin);
    pc[kVtHdr + 284] = reinterpret_cast<void *>(&dpg_signal);
    dc[kVtHdr + 51] = reinterpret_cast<void *>(&dpg_flip);
    gPg.pipeCopy = pc; gPg.dispCopy = dc; gPg.slide = slide; gPg.fbCount = fbCount;
    uint32_t pipesOk = 0, dispOk = 0;
    for (uint32_t i = 0; i < fbCount; i++) {
        __asm__ __volatile__("sfence" ::: "memory");
        if (pvt[i] != pvt[0] || memcmp(pvt[i], pvt[0], N48_DPG_PIPE_SLOTS * sizeof(void *)) != 0) {
            DPGLOG("pipeguard: pipe[%u] %p has a different table than pipe[0] - NOT swapped", i, pipes[i]);
        } else {
            *reinterpret_cast<void ***>(pipes[i]) = pc + kVtHdr;
            if (*reinterpret_cast<void ***>(pipes[i]) == pc + kVtHdr) { gPg.pipe[i] = pipes[i]; gPg.pipeOrigVt[i] = pvt[i]; pipesOk++; }
        }
        if (dvt[i] != dvt[0] || memcmp(dvt[i], dvt[0], N48_DPG_DISP_SLOTS * sizeof(void *)) != 0) {
            DPGLOG("pipeguard: display[%u] %p has a different table than display[0] - NOT swapped", i, disps[i]);
        } else {
            *reinterpret_cast<void ***>(disps[i]) = dc + kVtHdr;
            if (*reinterpret_cast<void ***>(disps[i]) == dc + kVtHdr) { gPg.display[i] = disps[i]; gPg.dispOrigVt[i] = dvt[i]; dispOk++; }
        }
    }
    gPg.pipes = pipesOk; gPg.displays = dispOk;
    const bool coreOk = (pipesOk == fbCount && dispOk == fbCount);
    // Extend the safety core to the AMD event machine (accel+0x380) before any driven transaction (this session's brief).
    // A census failure fails the whole arm, with its own status, so a T-compose is never run against an unproven machine.
    // 0.0.291: `emcensus 2` (gEmDisarm) SKIPS the census wrap - accel+0x380 keeps its native IAF2 vtable - while the rest
    // of the safety core arms exactly as before. The arm then succeeds on coreOk alone; navi48_pipeguard_armed_all waives
    // the em-verified requirement for the same reason. This isolates cause 2 without weakening a single refusal.
    uint32_t emSt = 0;
    if (coreOk && !gEmDisarm) { emSt = dpg_arm_event_machine(accel, slide); gEm.lastStatus = emSt; }
    else if (coreOk) { gEm.lastStatus = 0; }
    gPg.armed = (coreOk && (gEmDisarm || emSt == 0)) ? 1u : 0u;
    DPGLOG("pipeguard: %s - slide %#lx, framebuffers %u, pipes swapped %u, displays swapped %u; pipe slots 267 268 273 274 276 277 278 "
           "279 283 284 and display slot 51 are ours (%s); GFX-ring WRITE_DATA guard ARMED; event-machine census %s",
           gPg.armed ? "ARMED" : "PARTIAL - NOT ARMED", (unsigned long)slide, fbCount, pipesOk, dispOk, N48_DPG_TOKEN,
           gEmDisarm ? "DISARMED (emcensus 2)" : (gEm.armed ? "ARMED" : "NOT armed"));
    return gPg.armed ? 0 : (coreOk ? emSt : 6u);
}

bool navi48_pipeguard_armed_all(void) {
    if (!gPg.armed) return false;
    void *accel = navi48_accel_object();
    if (!accel) return false;
    void *dm = *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff);
    if (!dpg_kptr(dm)) return false;
    const uint32_t fbCount = *reinterpret_cast<uint32_t *>(static_cast<char *>(dm) + kDpgDmFbCountOff);
    if (fbCount != gPg.fbCount || fbCount == 0 || fbCount > kDpgMaxPipes) return false;
    for (uint32_t i = 0; i < fbCount; i++) {
        void *p = *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff + 8u * i);
        if (p != gPg.pipe[i] || *reinterpret_cast<void ***>(p) != gPg.pipeCopy + kVtHdr) return false;
        void *d = *reinterpret_cast<void **>(static_cast<char *>(p) + kDpgPipeDisplayOff);
        if (d != gPg.display[i] || *reinterpret_cast<void ***>(d) != gPg.dispCopy + kVtHdr) return false;
    }
    // The event-machine census is part of the safety core since 0.0.289: it must still carry our copy too - UNLESS it was
    // intentionally disarmed for this boot (0.0.291 `emcensus 2`), in which case accel+0x380 rightly keeps its native table.
    if (!gEmDisarm && !dpg_em_verified()) return false;
    return true;
}

// action 66 `pipeguard [1]`: 1 arms, 0 reads. out[0] status, [1] armed | still-verified << 1, [2] pipes | displays << 16 |
// framebuffers << 32, [3] initFramebufferResource refused | destroy passed << 32, [4] enable | disable transaction interrupt << 32,
// [5] validate, [6] perform, [7] isComplete | submit << 32, [8] begin | signal << 32, [9] flips refused,
// [10] pipe[0] state: +0x298 active | +0x282 defer << 8 | +0x284 fb index << 16 | accel+0xc78 bit1 << 32 | user client << 33 |
//      framebuffer is RDNA4FB << 34 | (0.0.334) +0xd8 vbl cookie non-NULL << 35 | +0x2a0 vbl refcount << 36 (8 bits) |
//      +0x281 txn-over-vbl << 44 | +0x2a4 TransactIR << 45 (16 bits) | +0x258 head non-NULL << 61 |
//      (0.0.335) notify list NOT EMPTY (tail != &head) << 62 | +0x248 Pending or +0x250 Live non-NULL << 63,
//      [11] WRITE_DATA walked | DCN writes << 32, [12] DCN writes NOPed | mismatches << 32.
uint32_t navi48_pipeguard_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint32_t st = 0;
    if (arg == 1) {
        IOLockLock(gDpgLock);
        st = dpg_arm_locked();
        gPg.lastStatus = st;
        IOLockUnlock(gDpgLock);
    } else if (arg != 0) {
        st = 13;
    }
    uint64_t pstate = 0;
    void *accel = navi48_accel_object();
    uint32_t rsr = 0;
    const uintptr_t rslide = accel ? navi48_x6000_slide(&rsr) : 0;
    if (accel && rslide) {
        void *dm = *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff);
        if (dpg_is(dm, rslide, kDpgDmGetMeta, "AMDRadeonX6000_AMDAccelDisplayMachine")) {
            const uint32_t fbc = *reinterpret_cast<uint32_t *>(static_cast<char *>(dm) + kDpgDmFbCountOff);
            void *p = (fbc && fbc <= kDpgMaxPipes) ? *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff) : nullptr;
            const uint32_t c78 = *reinterpret_cast<uint32_t *>(static_cast<char *>(accel) + 0xc78);
            // A pipe already guarded carries OUR table, whose slot 7 is still Apple's getMetaClass, so dpg_is holds for it too.
            if (dpg_is(p, rslide, kDpgPipeGetMeta, "AMDRadeonX6000_AMDAccelDisplayPipe")) {
                const char *pc = static_cast<const char *>(p);
                const uint8_t active = (uint8_t)pc[0x298], defer = (uint8_t)pc[0x282];
                const uint32_t fbi = *reinterpret_cast<const uint32_t *>(pc + 0x284);
                void *uc = *reinterpret_cast<void *const *>(pc + 0xe8);
                void *fb = *reinterpret_cast<void *const *>(pc + 0x98);
                const bool isR = dpg_is_our_fb(fb);
                pstate = (uint64_t)active | ((uint64_t)defer << 8) | ((uint64_t)(fbi & 0xffff) << 16) | ((uint64_t)((c78 >> 1) & 1) << 32) |
                         ((uint64_t)(uc != nullptr) << 33) | ((uint64_t)isR << 34);
                // AMDGFX10Hardware::disableTransactions (0xbe317fc) calls slot 0x198 on hardware slot 93's object (hw+0x380, be53292),
                // while the pipe's display is hardware slot 95's (hw+0x398, be532b0; pipe init bdcc58f-bdcc595). Name both.
                void *hw = *reinterpret_cast<void *const *>(pc + 0x328);
                if (dpg_kptr(hw) && reinterpret_cast<uintptr_t>(*reinterpret_cast<void *const *>(hw)) - rslide == kDpgNavi21HwVtable + 16) {
                    void *h380 = *reinterpret_cast<void *const *>(static_cast<const char *>(hw) + 0x380);
                    void *h398 = *reinterpret_cast<void *const *>(static_cast<const char *>(hw) + 0x398);
                    void **v380 = dpg_kptr(h380) ? *reinterpret_cast<void ***>(h380) : nullptr;
                    DPGLOG("pipeguard: hardware %p (AMDNavi21Hardware by vtable): +0x380 (disableTransactions' slot-0x198 receiver) %p vtable "
                           "%#lx slot 51 %#lx (minus slide); +0x398 (the display) %p, %s pipe+0x330", hw, h380,
                           (unsigned long)(dpg_kptr(v380) ? reinterpret_cast<uintptr_t>(v380) - rslide : 0),
                           (unsigned long)(dpg_kptr(v380) ? reinterpret_cast<uintptr_t>(v380[51]) - rslide : 0), h398,
                           h398 == *reinterpret_cast<void *const *>(pc + 0x330) ? "SAME AS" : "differs from");
                }
                DPGLOG("pipeguard: pipe[0] %p: +0x298 active %u, +0x282 defer %u, +0x284 fb index %u, +0xe8 user client %p, +0x98 "
                       "framebuffer %p (%s); accel+0xc78 %#x (bit 1 enableAccelerator %u); display machine framebuffers %u",
                       p, active, defer, fbi, uc, fb, dpg_class(fb), c78, (c78 >> 1) & 1, fbc);
                // 0.0.334  — READ ONLY, no write anywhere in this block.
                // `request_notify` (user-client selector 3) is IOAccelDisplayPipeRequestVBLNotifyCallback: an
                // asynchronous ONE-SHOT vertical-blank registration. s_request_notify 0x145d1e3e -> requestNotify
                // 0x145d2c0a -> request_notify_gated 0x145cfd34 links the request onto pipe+0x258/+0x260 and, if
                // pipe+0x2a0 == 0, sets it to 1 and calls pipe vtable slot 271 enableVBLInterrupt (0x145cf514).
                // enableVBLInterrupt on the 0->1 edge loads rsi = pipe+0xd8 and `test rsi,rsi; je ret`: with a NULL
                // VBL cookie it silently does nothing and NO notification is ever delivered. pipe+0xd8 is written in
                // exactly two places in AMDRadeonX6000: IOAccelDisplayPipe::displayModeDidChange (0x145cc636), which
                // calls IOFramebuffer::registerForInterruptType (slot 346) with proc 0x145cc7da signalVBLInterrupt
                // and type 'vbl ', and framebuffer_terminated, which clears it. So pipe+0xd8 IS the answer to
                // "did our framebuffer ever accept a VBL registration"; RDNA4FB refuses that type unless boot-arg
                // rdna4-vbl=1 is set (src/RDNA4FB/src/framebuffer.cpp:1514, :2090). pipe+0x281 is the flag
                // enableTransactionInterrupt's slot-273 fallback sets when it routes completion over the SAME VBL.
                // ALL FIVE READS ARE LOADS. Nothing here writes the pipe. (token: vblread-pipe-0xd8 0.0.334)
                void *const vblCookie = *reinterpret_cast<void *const *>(pc + 0xd8);
                void *const pending   = *reinterpret_cast<void *const *>(pc + 0x248);   // Apple's name `Pending`
                void *const liveTxn   = *reinterpret_cast<void *const *>(pc + 0x250);   // Apple's name `Live`
                void *const reqHead   = *reinterpret_cast<void *const *>(pc + 0x258);
                void *const reqTail   = *reinterpret_cast<void *const *>(pc + 0x260);
                const uint32_t vblRc  = *reinterpret_cast<const uint32_t *>(pc + 0x2a0);
                const uint8_t  txnVbl = (uint8_t)pc[0x281];
                const uint32_t latch  = *reinterpret_cast<const uint32_t *>(pc + 0x2a4);
                // 0.0.335: the list at +0x258/+0x260 is a TAILQ whose EMPTY form is tail == &head. 0.0.334 called that
                // "a request IS queued" because it only tested the pointers for non-NULL; the pointers it printed were
                // right and the adjective was wrong. Decide it from the address, and carry the Pending/Live pair too -
                // Apple's own field names, from the `triage` format string at 0x145e2aeb, never read on this project.
                const bool listEmpty = (reqTail == nullptr) || (reqTail == static_cast<const void *>(pc + 0x258));
                pstate |= ((uint64_t)(vblCookie != nullptr) << 35) | ((uint64_t)(vblRc & 0xff) << 36) |
                          ((uint64_t)(txnVbl != 0) << 44) | ((uint64_t)(latch & 0xffff) << 45) |
                          ((uint64_t)(reqHead != nullptr) << 61) | ((uint64_t)(!listEmpty) << 62) |
                          ((uint64_t)(pending != nullptr || liveTxn != nullptr) << 63);
                DPGLOG("pipeguard: pipe[0] %p VBL REGISTRATION (vblread-pipe-0xd8 0.0.335, read-only): +0xd8 vbl cookie %p "
                       "-> registerForInterruptType('vbl ') %s; +0x2a0 vbl enable refcount %u; +0x281 txn-over-vbl %u; "
                       "+0x248 Pending %p; +0x250 Live %p; +0x258/+0x260 notify request list %p / %p (%s); "
                       "+0x2a4 TransactIR (transaction interrupt ARMED, not a completion) %u%s",
                       p, vblCookie, vblCookie ? "SUCCEEDED (a VBL source exists)" : "NEVER SUCCEEDED (enableVBLInterrupt is a no-op)",
                       vblRc, (unsigned)txnVbl, pending, liveTxn, reqHead, reqTail,
                       listEmpty ? "EMPTY - the tail points at the head itself" : "a request IS linked",
                       latch, latch ? "  *** NON-ZERO: THE TRANSACTION INTERRUPT IS ARMED ***" : "");
            }
        }
    }
    uint64_t wd[12] = { 0 };
    (void)n48::hw_hook_dpg_writedata(0, wd, 12);
    // The event-machine census: total hits across the wrapped slots, and the per-slot breakdown in the log. In T-ready
    // (route B, no entitled client) NO event-machine slot runs, so every count reads 0; a driven transaction (T-compose)
    // reaches only 40/54/55, each pure memory. Any hit paired with a non-zero WRITE_DATA->DCN count would be the alarm.
    uint64_t emTotal = 0;
    for (unsigned i = N48_DPG_EM_WRAP_FIRST; i < N48_DPG_N_EM_GUARD; i++) emTotal += gEm.hit[i];
    const uint64_t v[13] = {
        st, (uint64_t)gPg.armed | ((uint64_t)navi48_pipeguard_armed_all() << 1) | ((uint64_t)(gEm.armed ? 1u : 0u) << 2) |
            ((uint64_t)(dpg_em_verified() ? 1u : 0u) << 3),
        (uint64_t)gPg.pipes | ((uint64_t)gPg.displays << 16) | ((uint64_t)gPg.fbCount << 32) |
            ((uint64_t)(emTotal & 0xffffu) << 48),
        gPg.initFb | (gPg.destroyFb << 32), gPg.enableIrq | (gPg.disableIrq << 32),
        // 0.0.337: these two were printed by navi48test as "REFUSED validate/perform" and they are
        // nothing of the kind — `gPg.validate` / `gPg.perform` increment at the TOP of dpg_validate / dpg_perform,
        // BEFORE the `if (!gSh.mode)` refusal test, so they are ENTRY counts. With the shim armed every one of them
        // is an acceptance and the word REFUSED inverts the truth; says the same thing in prose and the printer
        // never caught up. Send the acceptances (`gSh.*`, incremented only past the gate) alongside, so the refusal
        // count is DERIVED — calls minus accepted — instead of asserted. Both fit in 32 bits by four orders.
        (gPg.validate & 0xffffffffull) | (gSh.validate << 32),
        (gPg.perform & 0xffffffffull) | (gSh.perform << 32),
        gPg.isComplete | (gPg.submit << 32), gPg.begin | (gPg.signal << 32), gPg.flip, pstate, wd[1] | (wd[5] << 32),
        wd[6] | (wd[7] << 32),
    };
    DPGLOG("pipeguard: event-machine census %s (verified now %u): accel+0x380 %p; hits initEvent %llu cleanEvent %llu "
           "testEventUnlocked %llu copyEvent %llu mergeEvent %llu deviceTerminated %llu enableEventStampInterrupts %llu "
           "(total %llu); each is pure IOAccelEvent memory - a hit with WRITE_DATA->DCN %llu > 0 would be the alarm",
           gEm.armed ? "ARMED" : "NOT armed", dpg_em_verified() ? 1u : 0u, gEm.machine, (unsigned long long)gEm.hit[2],
           (unsigned long long)gEm.hit[3], (unsigned long long)gEm.hit[4], (unsigned long long)gEm.hit[5],
           (unsigned long long)gEm.hit[6], (unsigned long long)gEm.hit[7], (unsigned long long)gEm.hit[8],
           (unsigned long long)emTotal, (unsigned long long)wd[6]);
    DPGLOG("pipeguard: control %llu -> status %u; armed %u (verified now %u); CALL COUNTS - entries, NOT refusals; each "
           "increments before the `if (!gSh.mode)` gate, so with the shim armed they count acceptances (0.0.337, notes "
           "section 697; the refusals are the `pipeguard: REFUSED ...` lines and `pipeshim`'s accepted counts): initFb %llu, destroy passed %llu, irq enable %llu "
           "disable %llu, validate %llu, perform %llu, isComplete %llu, submit %llu, begin %llu, signal %llu, FLIPS %llu; WRITE_DATA "
           "walked %llu, DCN %llu, NOPed %llu; 0.0.329 irq CHAINED enable %llu disable %llu, chain refused %llu "
           "(these count calls PASSED THROUGH to Apple, not refusals)",
           (unsigned long long)arg, st, gPg.armed, navi48_pipeguard_armed_all() ? 1u : 0u,
           (unsigned long long)gPg.initFb, (unsigned long long)gPg.destroyFb, (unsigned long long)gPg.enableIrq,
           (unsigned long long)gPg.disableIrq, (unsigned long long)gPg.validate, (unsigned long long)gPg.perform,
           (unsigned long long)gPg.isComplete, (unsigned long long)gPg.submit, (unsigned long long)gPg.begin,
           (unsigned long long)gPg.signal, (unsigned long long)gPg.flip, (unsigned long long)wd[1], (unsigned long long)wd[5],
           (unsigned long long)wd[6], (unsigned long long)gPg.chainedEnable, (unsigned long long)gPg.chainedDisable,
           (unsigned long long)gPg.chainRefused);
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}

// action 72 `emcensus [0|1|2]` (0.0.291): the event-machine census policy toggle, project convention
// (1 arm, 2 disarm, 0/none read). It ONLY sets gEmDisarm and ONLY when the safety core is not yet armed (the wrap
// decision is made once, at arm time); a change under a live arm is refused (status 1) so the reported policy never
// disagrees with what actually armed. Reads always work and also report the per-slot census hit breakdown, so the
// disarmed T-compose can prove the census was genuinely off (every per-slot count 0) and an armed one can show which
// slots a driven present touches. It writes NO hardware and touches NO Apple object; it is pure kext bookkeeping.
// out[0] status (0 ok, 1 refused - already armed, 2 arg out of range); [1] census armed (bit0) | gEm.armed (bit1) |
// verified (bit2) | safety-core armed (bit3); [2] total slot hits; [3] initEvent | cleanEvent << 32; [4] testEventUnlocked
// | copyEvent << 32; [5] mergeEvent | enableEventStampInterrupts << 32.
uint32_t navi48_emcensus_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint32_t st = 0;
    if (arg == 1 || arg == 2) {
        IOLockLock(gDpgLock);
        if (gPg.armed) st = 1;                       // policy is fixed at arm time; refuse to change under a live arm
        else gEmDisarm = (arg == 2) ? 1u : 0u;
        IOLockUnlock(gDpgLock);
    } else if (arg != 0) {
        st = 2;                                      // out of range (only 0/none read, 1 arm, 2 disarm)
    }
    uint64_t emTotal = 0;
    for (unsigned i = N48_DPG_EM_WRAP_FIRST; i < N48_DPG_N_EM_GUARD; i++) emTotal += gEm.hit[i];
    const uint64_t v[6] = {
        st,
        (uint64_t)(gEmDisarm ? 0u : 1u) | ((uint64_t)(gEm.armed ? 1u : 0u) << 1) |
            ((uint64_t)(dpg_em_verified() ? 1u : 0u) << 2) | ((uint64_t)(gPg.armed ? 1u : 0u) << 3),
        emTotal, gEm.hit[2] | (gEm.hit[3] << 32), gEm.hit[4] | (gEm.hit[5] << 32), gEm.hit[6] | (gEm.hit[8] << 32),
    };
    DPGLOG("pipeguard: emcensus control %llu -> status %u; policy %s (arg 0/none read, 1 arm census, 2 DISARM census); "
           "safety-core armed %u, census armed %u verified %u; per-slot hits init %llu clean %llu test %llu copy %llu "
           "merge %llu term %llu enStamp %llu (total %llu)", (unsigned long long)arg, st, gEmDisarm ? "DISARMED" : "armed",
           gPg.armed, gEm.armed ? 1u : 0u, dpg_em_verified() ? 1u : 0u, (unsigned long long)gEm.hit[2],
           (unsigned long long)gEm.hit[3], (unsigned long long)gEm.hit[4], (unsigned long long)gEm.hit[5],
           (unsigned long long)gEm.hit[6], (unsigned long long)gEm.hit[7], (unsigned long long)gEm.hit[8],
           (unsigned long long)emTotal);
    if (out) for (unsigned i = 0; i < count && i < 6; i++) out[i] = v[i];
    return st;
}

// =====================================================================================================================
// B. The AGDC nub
// =====================================================================================================================
// Static (unslid) addresses in com.apple.AppleGraphicsDeviceControl, System KC, CONFIRMED from the bytes:
static constexpr uintptr_t kAgdcGMetaClass   = 0x13d418e8;   // AppleGraphicsDeviceControl::gMetaClass
static constexpr uintptr_t kAgdcVtable       = 0x13d40478;   // __ZTV26AppleGraphicsDeviceControl (slot 0 at +0x10)
static constexpr unsigned  kAgdcVtSlots      = 276;          // slot 275 raw 0x0100000053d3e110 ends the chain
static constexpr uintptr_t kAgdcOpNew        = 0x13d3bea6;   // operator new(unsigned long): OSObject_typed_operator_new(ktv, n)
static constexpr uintptr_t kAgdcCtorMeta     = 0x13d3be34;   // AppleGraphicsDeviceControl(OSMetaClass const*): IOService ctor + vptr
static constexpr uintptr_t kAgdcD2           = 0x13d3be54;   // ~AppleGraphicsDeviceControl(): jmp IOService::~IOService
static constexpr uintptr_t kAgdcOpDelete     = 0x13d3beba;   // operator delete(void*, unsigned long)
static constexpr uintptr_t kAgdcStart        = 0x13d3c392;   // slot 184
static constexpr uintptr_t kAgdcNewUserClient= 0x13d3dcbe;   // slot 238
static constexpr uintptr_t kAgdcVendorBase   = 0x13d3dc34;   // slot 267 (calls slot 266 through *0x850)
static constexpr uint32_t  kAgdcClassSize    = 0x110;        // MetaClass ctor 13d3be0f: b9 10 01 00 00
// First bytes of each function we call, from the extracted binary.
static const uint8_t kBytesOpNew[]    = { 0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xfe, 0x48, 0x8d, 0x3d };
static const uint8_t kBytesCtorMeta[] = { 0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0xe8 };
static const uint8_t kBytesD2[]       = { 0x55, 0x48, 0x89, 0xe5, 0x5d, 0xe9 };
static const uint8_t kBytesOpDelete[] = { 0x55, 0x48, 0x89, 0xe5, 0x48, 0x89, 0xf2, 0x48, 0x89, 0xfe, 0x48, 0x8d, 0x3d };

typedef void *(*AgdcOpNewFn)(size_t);
typedef void  (*AgdcCtorFn)(void *, const OSMetaClass *);
typedef void  (*AgdcD2Fn)(void *);
typedef void  (*AgdcOpDeleteFn)(void *, size_t);

static struct {
    uint32_t published, lastStatus, startOk;
    uintptr_t slide;
    IOService *obj;
    void **vtCopy;
    uint64_t regId;
    IOService *fb;
    IOService *pci;
    uint64_t calls, vendorInfo, gpuCap, linkCfg, pipeCaps, other, badLen, logged;
    uint32_t cmdSeen[16]; uint64_t cmdCount[16];
    // Reply-dump bookkeeping (log only, notes/M4-CAPABILITIES-STRUCT.md). dumped711 is a bit per
    // 0x711 capability type (0x1 CSC .. 0x8000 Cursor, exactly the sixteen of that memo's); dumpedCmd bit 0
    // is 0x921, bit 1 is 0x980, bit 2 is a 0x711 whose type was not a single bit. Once set, never dumped again,
    // so this is once per boot per reply and costs nothing after the first acquire round.
    uint32_t dumped711, dumpedCmd;
    // The AGDC hold (0.0.322): holdMs is the armed duration (0 = disarmed), holdFired the one-shot
    // latch SET BEFORE THE SLEEP, holdEntered/holdSkipped the counters that make a hold that never entered
    // distinguishable from one that entered and did nothing.
    uint32_t holdMs, holdFired, holdEntered, holdSkipped;
    AgdcD2Fn d2; AgdcOpDeleteFn opDelete;
} gAg {};

static void agdc_count_cmd(uint32_t cmd) {
    for (unsigned i = 0; i < 16; i++) {
        if (gAg.cmdCount[i] && gAg.cmdSeen[i] == cmd) { gAg.cmdCount[i]++; return; }
        if (!gAg.cmdCount[i]) { gAg.cmdSeen[i] = cmd; gAg.cmdCount[i] = 1; return; }
    }
}

// ---- reply dump: LOG ONLY, once per boot per reply, notes/M4-CAPABILITIES-STRUCT.md) -----------------
// Prints the non-zero dwords of the reply we have just written, so the capability dictionary IOPresentment builds
// out of it can be replayed offline against Apple's producer and consumer rules (that memo's) instead
// of costing a deploy-and-run per guess. It READS the reply buffer and writes nothing to it; no caller-visible
// state, no return value, no reply byte depends on it. The replies are sparse (every 0x711 type except 0x10 is
// entirely zero today), so this is a handful of lines rather than 16 x 0x196c bytes of hex.
static void agdc_dump_reply(uint32_t cmd, uint32_t type, const uint8_t *buf, size_t len, uint32_t kr) {
    if (!buf || len < 4u) return;
    const size_t nd = len / 4u;
    size_t nz = 0;
    for (size_t i = 0; i < nd; i++) if (n48_agdc_rd32(buf, (uint32_t)(i * 4u))) nz++;
    DPGLOG("agdc: REPLYDUMP cmd %#x type %#x len %#lx kr %#x dwords %lu nonzero %lu",
           cmd, type, (unsigned long)len, kr, (unsigned long)nd, (unsigned long)nz);
    char line[192];
    size_t used = 0; unsigned onLine = 0;
    line[0] = '\0';
    for (size_t i = 0; i < nd; i++) {
        const uint32_t v = n48_agdc_rd32(buf, (uint32_t)(i * 4u));
        if (!v) continue;
        const int n = snprintf(line + used, sizeof(line) - used, " +%#06lx=%#010x", (unsigned long)(i * 4u), v);
        if (n > 0 && (size_t)n < sizeof(line) - used) { used += (size_t)n; onLine++; }
        if (onLine >= 6u || used >= sizeof(line) - 24u) {
            DPGLOG("agdc: REPLYDUMP cmd %#x type %#x dw%s", cmd, type, line);
            line[0] = '\0'; used = 0; onLine = 0;
        }
    }
    if (onLine) DPGLOG("agdc: REPLYDUMP cmd %#x type %#x dw%s", cmd, type, line);
}

// Slot 266: vendor_doDeviceAttribute(uint32 cmd, unsigned long *in, unsigned long inLen, unsigned long *out,
// unsigned long *outLen, IOExternalMethodArguments *args). Called by AGDC::start (13d3c40e / 13d3c44f) and by the user-client
// path (filtered_doDeviceAttribute 13d3cba5 for 0x980, with a 0xdc temp buffer it then converts and copies).
// build 0.0.514 B1: THE ENDPOINT SIZE the AGDC replies declare - route A's captured mode (gRa), else RDNA4FB's
// LIVE Console,Width/Height (navi48_scanout_live_dims: the same source scanout_copy.h's geometry reads), else 0.0.513's documented
// 1920x1080. 0.0.513 went from gRa straight to the constant. Read-only (registry properties). noinline: agdc_vendor's frame.
static __attribute__((noinline)) void agdc_endpoint(uint32_t *w, uint32_t *h) {
    uint32_t cw = 0u, ch = 0u;
    if (!gRa.width || !gRa.height) (void)navi48_scanout_live_dims(&cw, &ch);
    *w = n48_agdc_endpoint_dim(gRa.width, cw, 1920u);
    *h = n48_agdc_endpoint_dim(gRa.height, ch, 1080u);
}
// build 0.0.514 B2: getLinkConfig's reply. The timing is the lit OTG's raster (n48dcn::liveRaster:
// read-only; clock and porches from the sink's EDID row with the same totals) when that raster IS this endpoint - a native
// framebuffer - else 0.0.513's CEA 1080p blanking. At 1920x1080 no register is read and the reply is 0.0.513's byte for byte.
// build 0.0.515: the decision is display_pipe_guard.h's n48_agdc_link_timing (1080p never reads a
// register); the reader is n48dcn::liveRaster, which now reads through the read-only device built at start(), not bind's.
static uint32_t agdc_live_raster(void *ctx, n48_agdc_raster *lr) {
    (void)ctx;
    return n48dcn::liveRaster(&lr->h_active, &lr->v_active, &lr->h_total, &lr->v_total, &lr->h_front, &lr->h_sync,
                              &lr->v_front, &lr->v_sync, &lr->pixel_clock_hz);
}
static __attribute__((noinline)) uint32_t agdc_link_config(uint8_t *out, size_t len, uint32_t lw, uint32_t lh) {
    n48_agdc_timing lt {};
    n48_agdc_link_timing(lw, lh, &agdc_live_raster, nullptr, &lt);
    const uint32_t kr = n48_agdc_fill_link_config_t(out, len, &lt) ? 0xe00002c2u : 0u;
    if (kr) gAg.badLen++;
    if (gAg.linkCfg == 1u)
        DPGLOG("agdc: LINKCFG endpoint %ux%u pixelClock %llu blank %u/%u timing %s (reply +0x44/+0x54 now NON-ZERO; the zero "
               "there was 'Invalid end point size')", lt.w, lt.h, (unsigned long long)lt.pixel_clock, lt.h_blank, lt.v_blank,
               lt.live ? "LIVE raster (lit OTG + EDID row)" : "CEA 1080p blanking");
    return kr;
}
static uint32_t agdc_vendor(void *self, uint32_t cmd, unsigned long *in, unsigned long inLen, unsigned long *outp,
                            unsigned long *outLen, void *args) {
    (void)in; (void)inLen; (void)args;
    gAg.calls++;
    agdc_count_cmd(cmd);
    // ---- the one-shot hold (0.0.322). Reply bytes are untouched; only this thread is delayed. ----
    // FAILS CLOSED: n48_agdc_hold_ms returns 0 for anything that is not a positively identified WindowServer,
    // because this same handler is called from AGDC::start (0x13d3c40e / 0x13d3c44f) in IOKit matching context,
    // where a stall would hold device matching. An unknown caller behaves exactly like "not WindowServer".
    if (gAg.holdMs && !gAg.holdFired && cmd == N48_AGDC_CMD_LINK_CONFIG) {
        char hn[20] = { 0 };
        proc_selfname(hn, (int)sizeof(hn));
        const int hpid = proc_selfpid();
        const uint32_t ms = n48_agdc_hold_ms(gAg.holdMs, gAg.holdFired, cmd, hn, (uint32_t)sizeof(hn));
        if (ms) {
            gAg.holdFired = 1;              // latch BEFORE the sleep: a second thread must see it already set
            gAg.holdEntered++;
            DPGLOG("agdc: HOLD enter call #%llu cmd %#x pid %d (%s) sleeping %u ms - attach dtrace NOW",
                   (unsigned long long)gAg.calls, cmd, hpid, hn, ms);
            IOSleep(ms);
            DPGLOG("agdc: HOLD exit after %u ms (entered %u, skipped %u) - the gather and its parse now proceed",
                   ms, gAg.holdEntered, gAg.holdSkipped);
        } else {
            gAg.holdSkipped++;
            DPGLOG("agdc: HOLD SKIPPED (fail-closed) - caller pid %d name '%s' is not WindowServer, or armed %u ms is "
                   "out of range; skipped %u", hpid, hn, gAg.holdMs, gAg.holdSkipped);
        }
    }
    uint32_t kr = 0xe00002c7u;   // kIOReturnUnsupported
    const size_t len = outLen ? (size_t)*outLen : 0;
    // LOG ONLY: the 0x711 capability type arrives in the OUT buffer at +0x04 and the fill zeroes it (in == out),
    // so read it here to label the dump below. Same read n48_agdc_fill_pipeline_caps already does; nothing written.
    const uint32_t capType = (cmd == N48_AGDC_CMD_PIPELINE_CAPS && outp && len >= 8u)
                                 ? n48_agdc_rd32(reinterpret_cast<const uint8_t *>(outp), 4u) : 0u;
    if (cmd == N48_AGDC_CMD_VENDOR_INFO) {
        gAg.vendorInfo++;
        kr = n48_agdc_fill_vendor_info(reinterpret_cast<uint8_t *>(outp), len) ? 0xe00002c2u : 0u;
        if (kr) gAg.badLen++;
    } else if (cmd == N48_AGDC_CMD_GPU_CAPABILITY) {
        gAg.gpuCap++;
        const uint64_t fbs[1] = { reinterpret_cast<uint64_t>(gAg.fb) };
        kr = (gAg.fb && gAg.pci && !n48_agdc_fill_gpu_capability(reinterpret_cast<uint8_t *>(outp), len,
                                                                  reinterpret_cast<uint64_t>(gAg.pci), fbs, 1)) ? 0u : 0xe00002c2u;
        if (kr) gAg.badLen++;
        if (!(gAg.dumpedCmd & 2u)) { gAg.dumpedCmd |= 2u; agdc_dump_reply(cmd, 0u, reinterpret_cast<const uint8_t *>(outp), len, kr); }
    } else if (cmd == N48_AGDC_CMD_LINK_CONFIG) {
        gAg.linkCfg++;
        // The endpoint must describe the FRAMEBUFFER (1920x1080 by deliberate OpenCore GOP choice), not the DP link,
        // which runs the panel's native 2560x1440 through the DPP scaler. Same live-mode source and same fallback the
        // 0x711 branch below uses. A zero pair here is the "Invalid end point size" blocker itself.
        // build 0.0.514 B1/B2: the endpoint (gRa, else the LIVE Console size, else 1920x1080) and its timing,
        // in two noinline helpers so this frame keeps 0.0.513's size.
        uint32_t lw = 0u, lh = 0u;
        agdc_endpoint(&lw, &lh);
        kr = agdc_link_config(reinterpret_cast<uint8_t *>(outp), len, lw, lh);
        if (!(gAg.dumpedCmd & 1u)) { gAg.dumpedCmd |= 1u; agdc_dump_reply(cmd, 0u, reinterpret_cast<const uint8_t *>(outp), len, kr); }
    } else if (cmd == N48_AGDC_CMD_PIPELINE_CAPS) {
        gAg.pipeCaps++;
        // The scaler entry body comes from the live framebuffer mode (route A captured it into gRa at arm time); if route A
        // is not armed, fall back to this display's documented 1920x1080. count=1 clears 0x2006 either way.
        // build 0.0.514 B1: gRa, else the LIVE Console,Width/Height, else 1920x1080.
        uint32_t w = 0u, h = 0u;
        agdc_endpoint(&w, &h);
        kr = n48_agdc_fill_pipeline_caps(reinterpret_cast<uint8_t *>(outp), len, w, h) ? 0xe00002c2u : 0u;
        if (kr) gAg.badLen++;
        // One dump per capability type per boot. A type that is a single bit gets its own bit of dumped711
        // (0x1 CSC .. 0x8000 Cursor); anything else shares bit 2 of dumpedCmd so a malformed type cannot flood.
        const bool oneBit = capType && !(capType & (capType - 1u)) && capType <= 0x8000u;
        const uint32_t mask = oneBit ? capType : 0u;
        if (oneBit ? !(gAg.dumped711 & mask) : !(gAg.dumpedCmd & 4u)) {
            if (oneBit) gAg.dumped711 |= mask; else gAg.dumpedCmd |= 4u;
            agdc_dump_reply(cmd, capType, reinterpret_cast<const uint8_t *>(outp), len, kr);
        }
    } else {
        gAg.other++;
    }
    if (gAg.logged < 48u) {
        gAg.logged++;
        char pn[20] = { 0 };
        proc_selfname(pn, (int)sizeof(pn));
        DPGLOG("agdc: vendor_doDeviceAttribute #%llu on %p cmd %#x out %p len %#lx from pid %d (%s) -> %#x%s", (unsigned long long)gAg.calls,
               self, cmd, outp, (unsigned long)len, proc_selfpid(), pn, kr,
               cmd == 1 ? " (kAGDCVendorInfo: AMD 0x1002 type 2)" : cmd == 0x980 ? " (kAGDCGPUCapability: 1 framebuffer, RDNA4FB)"
               : cmd == 0x921 ? " (getLinkConfig: AGDCLinkConfig_t 0xb0 with EndPointSizeTiming at +0x44/+0x54)" : cmd == 0x711 ? " (pipeline/scaler caps 0x196c: type 0x10 -> count 1, one scaler entry of the endpoint size - see REPLYDUMP +0x30/+0x34)" : "");   // build 0.0.544 4c: the label only (the reply already follows the size)
    }
    return kr;
}

// The destructors: the abstract class's own slots 0/1 are ud2 (13d3e566, 13d3e56c).
static void agdc_d1(void *self) { if (gAg.d2) gAg.d2(self); }
static void agdc_d0(void *self) {
    DPGLOG("agdc: DELETING the nub object %p (its retain count reached zero)", self);
    if (gAg.d2) gAg.d2(self);
    if (gAg.opDelete) gAg.opDelete(self, kAgdcClassSize);
}

static bool agdc_bytes(uintptr_t fn, const uint8_t *want, size_t n) {
    return memcmp(reinterpret_cast<const uint8_t *>(fn), want, n) == 0;
}

// 0.0.332  — THE DECLARED-IDENTITY PROBE. LOG ONLY: nothing here writes a byte, a register or a
// reply field; it only reads and prints.
//
// Our `0x980` reply hands Apple raw kernel POINTERS (display_pipe_guard.h: +0x34 = pci, +0x3c+8*i = fbs[i]) and
// AMDRadeonX6000's AGDC vendor base converts each one with IORegistryEntry::getRegistryEntryID() before the memcpy to
// userspace. IOPresentment's ioPresentmentPopulateIOFramebufferEntries then calls ioPresentmentOpenServiceForID on
// every converted ID, and a ZERO result SKIPS that entry — silently, with every layer above still reporting success.
// So a perfectly valid registry ID for the WRONG object, or for an object that is not registered, is indistinguishable
// from a correct one anywhere upstream. This prints the three facts that separate them:
//   1. the object's pointer and class name — is it really our framebuffer, or something framebuffer-ish?
//   2. the registry ID we will declare — to be compared against RDNA4FB's own ID from `ioreg` on the same boot;
//   3. what that ID resolves BACK to, through IOService::registryEntryIDMatching + copyMatchingService, which is the
//      kernel side of the very lookup userspace's IOServiceGetMatchingService(IORegistryEntryIDMatching(id)) makes.
//      Only registered, matched services match, so an unregistered object fails here exactly as it would there.
// The matching dictionary is deliberately NOT released: copyMatchingService is not documented to consume it, but it is
// not documented to keep it either, and an over-release in log-only code would panic the box. The existing
// AppleGPUWrangler call below does the same. One small leak, once per boot, at publish time.
static void agdc_log_one_identity(const char *what, IOService *obj) {
    if (!obj) { DPGLOG("agdc-declared-id: %s = NULL - we would declare registry ID 0 for it", what); return; }
    if (!dpg_kptr(obj)) { DPGLOG("agdc-declared-id: %s = %p is NOT a kernel pointer - NOT dereferenced", what, obj); return; }
    const OSMetaClass *mc = obj->getMetaClass();
    const char *cls = (mc && mc->getClassName()) ? mc->getClassName() : "(no class)";
    const uint64_t id = obj->getRegistryEntryID();
    OSDictionary *idm = IOService::registryEntryIDMatching(id);
    IOService *back = idm ? IOService::copyMatchingService(idm) : nullptr;
    DPGLOG("agdc-declared-id: %s = %p class %s regID %#llx; that ID resolves to %p (%s) -> %s",
           what, obj, cls, (unsigned long long)id, back, back ? dpg_class(back) : "nothing registered under that ID",
           back == obj ? "SAME OBJECT - IOPresentment would keep this entry"
                       : (back ? "DIFFERENT OBJECT - IOPresentment would open the WRONG service"
                               : "UNRESOLVABLE - ioPresentmentOpenServiceForID returns 0 and the entry is SKIPPED"));
    if (back) back->release();
    // Independent cross-check: the object userspace would find by CLASS NAME. If this is a different pointer there is
    // more than one instance of that class registered and we may be declaring the one nobody else uses.
    OSDictionary *cm = IOService::serviceMatching(cls);
    IOService *bycls = cm ? IOService::copyMatchingService(cm) : nullptr;
    DPGLOG("agdc-declared-id: %s by class name '%s' -> %p regID %#llx (%s)", what, cls, bycls,
           bycls ? (unsigned long long)bycls->getRegistryEntryID() : 0ull,
           bycls == obj ? "SAME OBJECT" : (bycls ? "DIFFERENT INSTANCE" : "none registered"));
    if (bycls) bycls->release();
}

// Shared by the translation route (agdc_publish_locked) and the native route (AgdcNativeEnv): the class size, the first bytes of the four functions we call and the four vtable slots we rely on.
// READ-ONLY (0.0.614: moved out of agdc_publish_locked unchanged). 0 = all hold, else the publish status: 5 class size, 6 code bytes, 7 vtable slots.
static uint32_t agdc_class_checks(const OSMetaClass *mc, uintptr_t slide) {
    if (mc->getClassSize() != kAgdcClassSize) { DPGLOG("agdc: REFUSED - class size %u, want %#x", mc->getClassSize(), kAgdcClassSize); return 5; }
    if (!agdc_bytes(kAgdcOpNew + slide, kBytesOpNew, sizeof kBytesOpNew) ||
        !agdc_bytes(kAgdcCtorMeta + slide, kBytesCtorMeta, sizeof kBytesCtorMeta) ||
        !agdc_bytes(kAgdcD2 + slide, kBytesD2, sizeof kBytesD2) ||
        !agdc_bytes(kAgdcOpDelete + slide, kBytesOpDelete, sizeof kBytesOpDelete)) {
        DPGLOG("agdc: REFUSED - a function we call does not carry its expected first bytes"); return 6;
    }
    void **vt = reinterpret_cast<void **>(kAgdcVtable + slide + 16);
    if (reinterpret_cast<uintptr_t>(vt[184]) - slide != kAgdcStart || reinterpret_cast<uintptr_t>(vt[238]) - slide != kAgdcNewUserClient ||
        reinterpret_cast<uintptr_t>(vt[267]) - slide != kAgdcVendorBase || reinterpret_cast<uintptr_t>(vt[7]) - slide != 0x13d3be5e) {
        DPGLOG("agdc: REFUSED - AGDC vtable slots 7/184/238/267 do not name getMetaClass/start/newUserClient/vendor_doDeviceAttribute");
        return 7;
    }
    return 0;
}

// Shared by both routes: AppleGPUWrangler is registered (AGDC::start would wait for it forever, 13d3c55b). The matching dictionary is deliberately NOT released (see agdc_log_one_identity).
static bool agdc_have_wrangler() {
    IOService *w = IOService::copyMatchingService(IOService::serviceMatching("AppleGPUWrangler"));
    if (!w) { DPGLOG("agdc: REFUSED - no AppleGPUWrangler (AGDC::start would wait forever for it, 13d3c55b)"); return false; }
    w->release();
    return true;
}

// The tail both routes share (0.0.614: moved out of agdc_publish_locked unchanged): copy the AGDC vtable with slots 0 / 1 / 266 ours, allocate and construct the object with Apple's own
// operator new and constructor, init / attach / start it, register it. Every check has been made by the caller (class, slide, bytes, slots, framebuffer, PCI, provider, wrangler).
// Status: 0 published, 10 allocation, 11 start failed.
static uint32_t agdc_build_locked(const OSMetaClass *mc, uintptr_t slide, IOService *fb, IOService *pci, IOService *provider) {
    void **vt = reinterpret_cast<void **>(kAgdcVtable + slide + 16);
    void **copy = static_cast<void **>(IOMalloc((kAgdcVtSlots + kVtHdr) * sizeof(void *)));
    if (!copy) return 10;
    memcpy(copy, vt - kVtHdr, (kAgdcVtSlots + kVtHdr) * sizeof(void *));
    copy[kVtHdr + 0] = reinterpret_cast<void *>(&agdc_d1);
    copy[kVtHdr + 1] = reinterpret_cast<void *>(&agdc_d0);
    copy[kVtHdr + 266] = reinterpret_cast<void *>(&agdc_vendor);
    gAg.d2 = reinterpret_cast<AgdcD2Fn>(kAgdcD2 + slide);
    gAg.opDelete = reinterpret_cast<AgdcOpDeleteFn>(kAgdcOpDelete + slide);
    gAg.fb = fb; gAg.pci = pci; gAg.slide = slide; gAg.vtCopy = copy;
    void *mem = reinterpret_cast<AgdcOpNewFn>(kAgdcOpNew + slide)(kAgdcClassSize);
    if (!mem) { IOFree(copy, (kAgdcVtSlots + kVtHdr) * sizeof(void *)); gAg.vtCopy = nullptr; return 10; }
    reinterpret_cast<AgdcCtorFn>(kAgdcCtorMeta + slide)(mem, mc);
    if (*reinterpret_cast<void ***>(mem) != vt) {
        DPGLOG("agdc: constructor left vptr %p, want %p - freeing", *reinterpret_cast<void ***>(mem), vt);
    }
    *reinterpret_cast<void ***>(mem) = copy + kVtHdr;
    mc->instanceConstructed();
    IOService *obj = static_cast<IOService *>(static_cast<OSObject *>(mem));
    DPGLOG("agdc: constructed %p class %s (AGDC metaclass %p, slide %#lx), vtable copy %p with slots 0/1/266 ours", obj,
           obj->getMetaClass()->getClassName(), mc, (unsigned long)slide, copy);
    if (!obj->init(nullptr)) { obj->release(); return 11; }
    obj->setName("Navi48AGDC");
    obj->setProperty("Navi48,AGDC", N48_DPG_TOKEN);
    if (!obj->attach(provider)) { obj->release(); return 11; }
    gAg.obj = obj;
    const bool ok = obj->start(provider);
    gAg.startOk = ok ? 1u : 0u;
    if (!ok) {
        DPGLOG("agdc: start(provider %p) returned false - detaching", provider);
        obj->detach(provider);
        obj->release();
        gAg.obj = nullptr;
        return 11;
    }
    gAg.regId = obj->getRegistryEntryID();
    gAg.published = 1;
    // What our 0x980 reply will actually declare (log only, 0.0.332, an earlier analysis).
    agdc_log_one_identity("reply+0x3c framebuffer (pipe+0x98)", gAg.fb);
    agdc_log_one_identity("reply+0x34 PCI device", gAg.pci);
    DPGLOG("agdc: PUBLISHED %p regID %#llx under %s; start made %llu vendor call(s) (vendor info %llu, GPU capability %llu, link config %llu, pipeline caps %llu, other %llu)",
           obj, (unsigned long long)gAg.regId, provider->getMetaClass()->getClassName(), (unsigned long long)gAg.calls,
           (unsigned long long)gAg.vendorInfo, (unsigned long long)gAg.gpuCap, (unsigned long long)gAg.linkCfg, (unsigned long long)gAg.pipeCaps, (unsigned long long)gAg.other);
    return 0;
}


// Status: 0 published, 1 boot-arg absent, 2 safety core not armed on every pipe, 3 AGDC class not loaded, 4 slide, 5 class size,
// 6 code bytes, 7 vtable slots, 8 no RDNA4FB / PCI, 9 no AppleGPUWrangler, 10 allocation, 11 start failed, 12 already published.
static uint32_t agdc_publish_locked() {
    if (gAg.published) return 12;
    uint32_t ba = 0;
    if (!PE_parse_boot_argn("navi48-agdc", &ba, sizeof(ba)) || ba != 1) return 1;
    if (!navi48_pipeguard_armed_all()) return 2;
    const OSSymbol *nm = OSSymbol::withCString("AppleGraphicsDeviceControl");
    const OSMetaClass *mc = nm ? OSMetaClass::getMetaClassWithName(nm) : nullptr;
    if (nm) nm->release();
    if (!mc) return 3;
    const uintptr_t slide = reinterpret_cast<uintptr_t>(mc) - kAgdcGMetaClass;
    uint32_t sr = 0;
    const uintptr_t xs = navi48_x6000_slide(&sr);
    if ((slide & 0xfff) || slide != xs) {
        DPGLOG("agdc: REFUSED - metaclass %p gives slide %#lx, X6000's two-anchor slide is %#lx", mc, (unsigned long)slide, (unsigned long)xs);
        return 4;
    }
    {
        const uint32_t cc = agdc_class_checks(mc, slide);
        if (cc) return cc;
    }
    // The framebuffer the adopted pipe[0] names, and our PCI device.
    void *accel = navi48_accel_object();
    void *dm = accel ? *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff) : nullptr;
    void *p = dm ? *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff) : nullptr;
    void *fbv = dpg_kptr(p) ? *reinterpret_cast<void **>(static_cast<char *>(p) + 0x98) : nullptr;
    IOService *fb = dpg_kptr(fbv) ? OSDynamicCast(IOService, static_cast<OSObject *>(fbv)) : nullptr;
    IOService *pci = navi48_bringup_pci();
    IOService *provider = navi48_bringup_service();
    if (!fb || !dpg_is_our_fb_name(fb->getMetaClass()->getClassName()) || !pci || !provider) {
        DPGLOG("agdc: REFUSED - framebuffer %p (%s), PCI %p, provider %p", fbv, dpg_class(fbv), pci, provider); return 8;
    }
    if (!agdc_have_wrangler()) return 9;
    return agdc_build_locked(mc, slide, fb, pci, provider);
}

// The scalars both AGDC controls return (action 67 and action 88): out[0] status, [1] published | start ok << 1, [2] registry ID, [3] vendor calls,
// [4] vendor info | GPU capability << 32, [5] other | bad length << 32, [6..12] the first seven distinct commands as cmd << 32 | count.
static void agdc_fill_out(uint32_t st, uint64_t *out, unsigned count) {
    uint64_t v[13] = { st, (uint64_t)gAg.published | ((uint64_t)gAg.startOk << 1), gAg.regId, gAg.calls,
                       gAg.vendorInfo | (gAg.gpuCap << 32), gAg.other | (gAg.badLen << 32), 0, 0, 0, 0, 0, 0, 0 };
    for (unsigned i = 0; i < 7; i++) v[6 + i] = ((uint64_t)gAg.cmdSeen[i] << 32) | (gAg.cmdCount[i] & 0xffffffffull);
    for (unsigned i = 0; i < 16 && gAg.cmdCount[i]; i++)
        DPGLOG("agdc:   command %#x x%llu", gAg.cmdSeen[i], (unsigned long long)gAg.cmdCount[i]);
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
}

// action 67 `agdc [1]`: 1 publishes, 0 reads. The scalars are agdc_fill_out's.
uint32_t navi48_agdc_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint32_t st = 0;
    if (arg == 1) {
        IOLockLock(gDpgLock);
        st = agdc_publish_locked();
        gAg.lastStatus = st;
        IOLockUnlock(gDpgLock);
        DPGLOG("agdc: publish -> status %u (0 published, 1 boot-arg navi48-agdc=1 absent, 2 safety core not armed on every pipe, 3 AGDC "
               "class not loaded, 4 slide, 5 class size, 6 code bytes, 7 vtable, 8 framebuffer/PCI, 9 no wrangler, 10 alloc, 11 start "
               "failed, 12 already published)", st);
    } else if (arg != 0) {
        st = 13;
    }
    agdc_fill_out(st, out, count);
    return st;
}

// =====================================================================================================================
// B2. The NATIVE AGDC service (0.0.614, #11 step 11h.3; accel action 88 `pipeagdc [0|1]`). Design: amd/native_agdc_pure.h.
// =====================================================================================================================
// The environment the tested flow (amd/native_agdc_flow.h) runs against in the kernel. It supplies primitives only; every DECISION, and the ORDER of the decisions, is n48agdc::native_flow.
// What differs from the translation route: the slide comes from two live metaclasses (AGDC and IOAccelDisplayPipe), the framebuffer is the RDNA4FB instance itself, there is no pipe guard,
// and a row-120 mode hold refuses the publish. The class checks, the wrangler check and the build are the translation route's own functions (moved, unchanged).
struct AgdcNativeEnv {
    IOService *fb = nullptr;            // the lookup's reference: kept by gAg on success (the AGDC reply declares this pointer for the life of the object), dropped otherwise
    IOService *pci = nullptr, *provider = nullptr;
    static const OSMetaClass *meta(const char *name) {
        const OSSymbol *nm = OSSymbol::withCString(name);
        const OSMetaClass *mc = nm ? OSMetaClass::getMetaClassWithName(nm) : nullptr;
        if (nm) nm->release();
        return mc;
    }
    bool latch_on() { return n48disp_latched_on(); }
    bool published() { return gAg.published != 0u; }
    bool mode_held() { return n48dcn::modeHoldActive(); }
    uint64_t agdc_meta() { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(meta("AppleGraphicsDeviceControl"))); }
    uint64_t pipe_meta() { return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(meta("IOAccelDisplayPipe"))); }
    uint32_t class_checks(uint64_t am, uint64_t slide) {
        const uint32_t cc = agdc_class_checks(reinterpret_cast<const OSMetaClass *>(static_cast<uintptr_t>(am)), static_cast<uintptr_t>(slide));
        if (cc == 0u) DPGLOG("agdc: native: AGDC metaclass %#llx and IOAccelDisplayPipe metaclass agree on slide %#llx; class, code bytes and vtable slots verified", (unsigned long long)am, (unsigned long long)slide);
        return cc;
    }
    bool have_targets() {
        static const char *const kNames[2] = { "RDNA4FB", "AMDRDNA4FB" };     // RDNA4FB matches under either name (fbname swaps the OSMetaClass name pointer)
        for (unsigned i = 0; i < 2 && !fb; ++i) {
            OSDictionary *m = IOService::serviceMatching(kNames[i]);
            if (!m) continue;
            fb = IOService::waitForMatchingService(m, 0);                  // non-blocking lookup; returns a retained service
            m->release();
        }
        pci = navi48_bringup_pci();
        provider = navi48_bringup_service();
        const OSMetaClass *mc = fb ? fb->getMetaClass() : nullptr;
        const bool ok = fb && mc && n48agdc::fb_name_ok(mc->getClassName()) && pci && provider;
        if (!ok) DPGLOG("agdc: native: REFUSED - framebuffer %p (%s), PCI %p, provider %p", fb, fb ? dpg_class(fb) : "none", pci, provider);
        return ok;
    }
    bool wrangler() { return agdc_have_wrangler(); }
    uint32_t build(uint64_t am, uint64_t slide) {
        return agdc_build_locked(reinterpret_cast<const OSMetaClass *>(static_cast<uintptr_t>(am)), static_cast<uintptr_t>(slide), fb, pci, provider);
    }
    void finish(uint32_t st) {          // after the flow: a published object keeps the framebuffer reference; every other outcome gives it back
        if (fb && st != n48agdc::kPublished) fb->release();
        fb = nullptr;
    }
};

// action 88 `pipeagdc [0|1]`: 1 publishes, 0 reads. The scalars are agdc_fill_out's; out[0] is the n48agdc status.
uint32_t navi48_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    AgdcNativeEnv env;
    IOLockLock(gDpgLock);
    const uint32_t st = n48agdc::native_flow(env, arg);
    if (arg == 1ull) gAg.lastStatus = st;
    IOLockUnlock(gDpgLock);
    env.finish(st);
    if (arg == 1ull) DPGLOG("agdc: native publish -> status %u (%s)", st, n48agdc::status_name(st));
    agdc_fill_out(st, out, count);
    return st;
}

// The AGDC hold control (action 79, 0.0.322). arg 0 = status, arg = milliseconds to arm (1..5000),
// and re-arming clears the one-shot latch so a second boot-less attempt is possible. Status codes: 0 armed/ok,
// 13 out of range. Nothing here touches a reply byte or any hardware.
uint32_t navi48_agdc_hold_control(uint64_t arg, uint64_t *out, unsigned count) {
    uint32_t st = 0;
    if (arg != 0) {
        if (arg > (uint64_t)N48_AGDC_HOLD_MAX_MS) {
            st = 13;
            DPGLOG("agdc-hold: REFUSED - %llu ms is above the %u ms ceiling", (unsigned long long)arg, N48_AGDC_HOLD_MAX_MS);
        } else {
            gAg.holdMs = (uint32_t)arg;
            gAg.holdFired = 0;
            DPGLOG("agdc-hold: ARMED %u ms on cmd %#x, WindowServer only, one shot (fired latch cleared)",
                   gAg.holdMs, N48_AGDC_CMD_LINK_CONFIG);
        }
    }
    DPGLOG("agdc-hold: armed %u ms, fired %u, entered %u, skipped %u", gAg.holdMs, gAg.holdFired, gAg.holdEntered, gAg.holdSkipped);
    uint64_t v[13] = { st, gAg.holdMs, gAg.holdFired, gAg.holdEntered, gAg.holdSkipped, gAg.calls, gAg.linkCfg, 0, 0, 0, 0, 0, 0 };
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}

// The command-queue probe (action 80, 0.0.325, notes sections 617-618). READ-ONLY: it walks the accelerator's
// IOAccelCommandQueueList and reports, per queue, the two flags that say whether a thread is inside
// IOAccelCommandQueue2::submit_command_buffers and how far, plus the fields that routine's wait predicate reads.
// It WRITES NOTHING and CALLS NOTHING of Apple's - no virtuals, no locks, no IOLockTryLock (which would perturb
// the very lock under test). Every entry is identity-checked by its own back-pointer before a byte of it is used.
uint32_t navi48_cqprobe_control(uint64_t arg, uint64_t *out, unsigned count) {
    (void)arg;
    uint64_t v[13] = { 0 };
    void *accel = navi48_accel_object();
    if (!accel) { DPGLOG("cqprobe: REFUSED - no accelerator"); v[0] = 1; goto done; }
    {
        char *a8 = static_cast<char *>(accel);
        void *lock = *reinterpret_cast<void *const *>(a8 + 0x88);
        const uint32_t c78 = *reinterpret_cast<const uint32_t *>(a8 + 0xc78);
        char *list = a8 + N48_CQ_LIST_OFF;
        void *head = *reinterpret_cast<void *const *>(list + N48_CQ_LIST_HEAD);
        const uint32_t n = *reinterpret_cast<const uint32_t *>(list + N48_CQ_LIST_COUNT);
        const uint32_t bound = n48_cq_walk_bound(n);
        DPGLOG("cqprobe: accel %p  IOLock* (accel+0x88) %p  accel+0xc78 %#x  queue list head %p count %u (walking %u)",
               accel, lock, c78, head, n, bound);
        v[0] = 0; v[1] = reinterpret_cast<uint64_t>(lock); v[2] = c78; v[3] = n; v[4] = bound;
        // The lock object's first 16 bytes, logged raw. The lck_mtx_t layout is NOT decoded here on purpose:
        // reporting bytes keeps the reading falsifiable offline instead of asserting an owner from a guessed layout.
        if (lock) {
            const uint64_t *lw = reinterpret_cast<const uint64_t *>(lock);
            DPGLOG("cqprobe: lock words %#llx %#llx  (raw - lck_mtx_t layout deliberately NOT interpreted here)",
                   (unsigned long long)lw[0], (unsigned long long)lw[1]);
            v[5] = lw[0]; v[6] = lw[1];
        }
        uint32_t walked = 0, inside = 0, pastEnable = 0, inconsistent = 0, mismatched = 0;
        void *q = head;
        for (uint32_t i = 0; i < bound && q; i++) {
            char *qc = static_cast<char *>(q);
            void *back = *reinterpret_cast<void *const *>(qc + N48_CQ_ACCEL_OFF);
            if (back != accel) {
                mismatched++;
                DPGLOG("cqprobe: queue[%u] %p REFUSED - +0x5c0 back-pointer %p is not our accelerator %p", i, q, back, accel);
                break;   // a list that does not point back is not a list we understand; stop, do not guess
            }
            const uint8_t f615 = *reinterpret_cast<const uint8_t *>(qc + N48_CQ_F615_OFF);
            const uint8_t f616 = *reinterpret_cast<const uint8_t *>(qc + N48_CQ_F616_OFF);
            const uint32_t st = n48_cq_state(f615, f616);
            walked++;
            if (st == N48_CQ_INSIDE) inside++;
            else if (st == N48_CQ_INSIDE_PAST_ENABLE) { inside++; pastEnable++; }
            else if (st == N48_CQ_INCONSISTENT) inconsistent++;
            DPGLOG("cqprobe: queue[%u] %p state %u (%s); +0x615 %u +0x616 %u; predicate inputs "
                   "+0x578 %#x +0x594 %u +0x5a0 %#x +0x610 %#x +0x580 %p +0x5b8 %p +0x618 %p",
                   i, q, st,
                   st == N48_CQ_IDLE ? "IDLE - no thread inside submit_command_buffers"
                   : st == N48_CQ_INSIDE ? "INSIDE, not past acceleratorWaitEnabled"
                   : st == N48_CQ_INSIDE_PAST_ENABLE ? "INSIDE and past acceleratorWaitEnabled"
                   : "INCONSISTENT - (0,1) cannot happen, one 16-bit store clears both",
                   f615, f616,
                   *reinterpret_cast<const uint32_t *>(qc + 0x578),
                   *reinterpret_cast<const uint8_t  *>(qc + 0x594),
                   *reinterpret_cast<const uint32_t *>(qc + 0x5a0),
                   *reinterpret_cast<const uint32_t *>(qc + 0x610),
                   *reinterpret_cast<void *const *>(qc + 0x580),
                   *reinterpret_cast<void *const *>(qc + 0x5b8),
                   *reinterpret_cast<void *const *>(qc + 0x618));
            q = *reinterpret_cast<void *const *>(qc + N48_CQ_NEXT_OFF);
        }
        v[7] = walked; v[8] = inside; v[9] = pastEnable; v[10] = inconsistent; v[11] = mismatched;
        DPGLOG("cqprobe: walked %u of %u; inside %u (past-enable %u); inconsistent %u; identity-mismatched %u",
               walked, bound, inside, pastEnable, inconsistent, mismatched);
    }
done:
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return (uint32_t)v[0];
}

// =====================================================================================================================
// C. fbbench — write throughput into RDNA4FB's scanout per cache mode (Part A)
// =====================================================================================================================
static IOService *dpg_find_rdna4fb() {
    IOService *pci = navi48_bringup_pci();
    if (!pci) return nullptr;
    IOService *fb = nullptr;
    if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IOService *svc = OSDynamicCast(IOService, o);
            if (svc && dpg_is_our_fb_name(svc->getMetaClass()->getClassName())) { fb = svc; break; }
        }
        it->release();
    }
    return fb;
}
static bool dpg_prop_u64(IOService *s, const char *k, uint64_t *v) {
    OSNumber *n = OSDynamicCast(OSNumber, s->getProperty(k));
    if (!n) return false;
    *v = n->unsigned64BitValue();
    return true;
}
// Kernel mapping helpers that pass the cache WIMG straight to the page mapper (CONFIRMED: ml_io_map_wcomb 0xffffff80004cc8f0
// `ba 06 00 00 00 movl $0x6,%edx; b9 03 00 00 00; jmp io_map`, io_map 0xffffff80004c86b0 hands it to pmap_map_bd 0xffffff80004b5e20).
// IOMapPages, by contrast, only calls pmap_set_cache_attributes (a no-op above the managed range, 0xffffff80004b6b93) and then
// pmap_map_block with flags 0 (0xffffff8000482b10 xorl %r9d,%r9d), so an IOMemoryDescriptor kernel mapping of the BAR cannot differ
// by cache option - which is what agdc1's first fbbench measured (all five modes 44-47 MiB/s).
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
static vm_offset_t gBenchUc { 0 }, gBenchWc { 0 };
static uint64_t gBenchBase { 0 }, gBenchLen { 0 };

static uint64_t dpg_now_us() {
    uint64_t t = mach_absolute_time(), ns = 0;
    absolutetime_to_nanoseconds(t, &ns);
    return ns / 1000u;
}

// 0.0.287: the scanout range as a fresh device descriptor, for Navi48UserClient::clientMemoryForType - so a root
// process can map it with IOConnectMapMemory64 exactly the way CoreDisplay maps kIOFBVRAMMemory (the framebuffer's own user
// client refuses anything but a "local" task at the login window). viaFramebuffer=true returns RDNA4FB's OWN
// getVRAMRange descriptor through IOFramebuffer slot 310 (so an armed fbwc wraps it), after the behavioural identity check.
IOMemoryDescriptor *navi48_scanout_descriptor(bool viaFramebuffer) {
    IOService *fb = dpg_find_rdna4fb();
    uint64_t base = 0, len = 0;
    if (!fb || !dpg_prop_u64(fb, "Console,BaseAddress", &base) || !dpg_prop_u64(fb, "Console,Length", &len) || !base || !len) return nullptr;
    if (!viaFramebuffer) return IODeviceMemory::withRange((IOPhysicalAddress)base, (IOPhysicalLength)len);
    void **fvt = *reinterpret_cast<void ***>(fb);
    IODeviceMemory *md = reinterpret_cast<IODeviceMemory *(*)(void *)>(fvt[310])(fb);
    IOByteCount sl = 0;
    if (md && (md->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) != base || md->getLength() != len)) {
        DPGLOG("scanout-map: RDNA4FB slot 310 returned %p that is not the scanout range - refused", md);
        md->release();
        return nullptr;
    }
    return md;
}

// action 68 `fbbench [rows]`: READ-ONLY for the picture - each mode maps RDNA4FB's scanout range (the same physical range
// getVRAMRange returns) into the kernel with that cache option, reads `rows` rows (default 128 since 0.0.287, at most 1080) into RAM
// once, writes the same bytes back three times and keeps the best. out[0] status (0 ok, 1 no RDNA4FB, 2 geometry, 3 alloc),
// [1] bytes per copy, [2] RAM->RAM KiB/s, [3..7] write KiB/s for options default, 0x100 inhibit, 0x200 write-thru,
// 0x300 copyback, 0x400 write-combine, [8] the one read's KiB/s, [9..10] 0, [11] ml_io_map (uncached) write KiB/s,
// [12] ml_io_map_wcomb (write-combine) write KiB/s (0.0.287).
uint32_t navi48_fbbench(uint64_t arg, uint64_t *out, unsigned count) {
    uint64_t v[13] = { 0 };
    IOService *fb = dpg_find_rdna4fb();
    uint64_t base = 0, len = 0, rb = 0, h = 0;
    uint32_t st = 0;
    if (!fb) st = 1;
    else if (!dpg_prop_u64(fb, "Console,BaseAddress", &base) || !dpg_prop_u64(fb, "Console,Length", &len) ||
             !dpg_prop_u64(fb, "Console,RowBytes", &rb) || !dpg_prop_u64(fb, "Console,Height", &h) || !base || !rb || !h)
        st = 2;
    uint64_t rows = arg ? arg : 128u;
    if (rows > h) rows = h;
    const size_t n = (size_t)(rows * rb);
    if (!st && (n == 0 || n > len)) st = 2;
    uint8_t *snap = nullptr, *ram = nullptr;
    if (!st) {
        snap = static_cast<uint8_t *>(IOMalloc(n));
        ram = static_cast<uint8_t *>(IOMalloc(n));
        if (!snap || !ram) st = 3;
    }
    if (!st) {
        for (size_t i = 0; i < n; i++) snap[i] = (uint8_t)(i * 2654435761u >> 13);
        uint64_t best = ~0ull;
        for (int r = 0; r < 3; r++) {
            const uint64_t t0 = dpg_now_us(); memcpy(ram, snap, n); const uint64_t dt = dpg_now_us() - t0;
            if (dt < best) best = dt;
        }
        v[1] = n; v[2] = best ? (uint64_t)n * 1000000ull / 1024ull / best : 0;
        static const IOOptionBits modes[5] = { 0x000, 0x100, 0x200, 0x300, 0x400 };
        for (unsigned m = 0; m < 5; m++) {
            IODeviceMemory *md = IODeviceMemory::withRange((IOPhysicalAddress)base, (IOPhysicalLength)len);
            IOMemoryMap *map = md ? md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | modes[m], 0, len) : nullptr;
            if (!map) {
                DPGLOG("fbbench: options %#x - map FAILED", modes[m]);
                if (md) md->release();
                continue;
            }
            uint8_t *va = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
            // Reads through the BAR are ~0.57 MiB/s (agdc1): read the snapshot once, through the first mapping only.
            uint64_t t0 = dpg_now_us(), rd = 0;
            if (m == 0) { memcpy(snap, va, n); rd = dpg_now_us() - t0; }
            uint64_t wbest = ~0ull, wworst = 0;
            for (int r = 0; r < 3; r++) {
                t0 = dpg_now_us(); memcpy(va, snap, n); const uint64_t dt = dpg_now_us() - t0;
                if (dt < wbest) wbest = dt;
                if (dt > wworst) wworst = dt;
                if (dt > 3000000u) break;
            }
            v[3 + m] = wbest ? (uint64_t)n * 1000000ull / 1024ull / wbest : 0;
            if (m < 3) v[8 + m] = rd ? (uint64_t)n * 1000000ull / 1024ull / rd : 0;
            DPGLOG("fbbench: options %#x (%s): %zu bytes (%llu rows x %llu) READ %llu us = %llu KiB/s; WRITE best %llu us worst %llu us = %llu KiB/s "
                   "(RAM->RAM %llu KiB/s)", modes[m],
                   m == 0 ? "default" : m == 1 ? "inhibit" : m == 2 ? "write-thru" : m == 3 ? "copyback" : "WRITE-COMBINE",
                   n, (unsigned long long)rows, (unsigned long long)rb, (unsigned long long)rd, (unsigned long long)v[8 + m],
                   (unsigned long long)wbest, (unsigned long long)wworst, (unsigned long long)v[3 + m], (unsigned long long)v[2]);
            map->release();
            md->release();
        }
        // 0.0.287: the two kernel mappings whose cache type is not ignored - uncached (WIMG 7) and write-combined (WIMG 6) -
        // mapped once per boot over the whole scanout and kept. Written with the last snapshot read above (the same bytes).
        if (!gBenchUc && !gBenchWc) {
            gBenchBase = base; gBenchLen = len;
            gBenchUc = ml_io_map((vm_offset_t)base, (vm_size_t)len);
            gBenchWc = ml_io_map_wcomb((vm_offset_t)base, (vm_size_t)len);
            DPGLOG("fbbench: ml_io_map %#lx, ml_io_map_wcomb %#lx over %#llx+%#llx (kept for the boot)", (unsigned long)gBenchUc,
                   (unsigned long)gBenchWc, (unsigned long long)base, (unsigned long long)len);
        }
        if (gBenchBase == base && gBenchLen == len) {
            const vm_offset_t vas[2] = { gBenchUc, gBenchWc };
            for (unsigned m = 0; m < 2; m++) {
                if (!vas[m]) continue;
                uint8_t *va = reinterpret_cast<uint8_t *>(vas[m]);
                uint64_t wbest = ~0ull;
                for (int r = 0; r < 3; r++) {
                    const uint64_t t0 = dpg_now_us(); memcpy(va, snap, n); const uint64_t dt = dpg_now_us() - t0;
                    if (dt < wbest) wbest = dt;
                    if (dt > 3000000u) break;
                }
                const uint64_t kib = wbest ? (uint64_t)n * 1000000ull / 1024ull / wbest : 0;
                // out slots 11 and 12 (reads of copyback / write-combine) are reused for these two: see the layout comment.
                v[11 + m] = kib;
                DPGLOG("fbbench: %s: %zu bytes WRITE best %llu us = %llu KiB/s", m == 0 ? "ml_io_map (uncached, WIMG 7)" :
                       "ml_io_map_wcomb (WRITE-COMBINE, WIMG 6)", n, (unsigned long long)wbest, (unsigned long long)kib);
            }
        }
    }
    if (snap) IOFree(snap, n);
    if (ram) IOFree(ram, n);
    v[0] = st;
    DPGLOG("fbbench: status %u (0 ok, 1 no RDNA4FB, 2 geometry, 3 alloc); base %#llx len %#llx rowBytes %llu", st,
           (unsigned long long)base, (unsigned long long)len, (unsigned long long)rb);
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}

// =====================================================================================================================
// D. fbwc — write-combining for the framebuffer mapping CoreDisplay makes (Part A fix)
// =====================================================================================================================
// Kernel (Boot KC, unslid, CONFIRMED from the kernel symbol table): IOLog 0xffffff8000a859d0 and IOSleep 0xffffff8000a85960
// (the two slide anchors); __ZTV25IOGeneralMemoryDescriptor 0xffffff800026b5d8 (72 slots, slot 0 D1 0xffffff8000ada060,
// slot 7 getMetaClass 0xffffff8000ada870, slot 70 doMap 0xffffff8000adc940, slot 71 doUnmap 0xffffff8000adcf00).
// IOFramebuffer slot 310 (byte 0x9b0) is getVRAMRange: IOFramebufferSharedUserClient::clientMemoryForType 14725817 calls
// *0x9b0 for type 0x6e; RDNA4FB's table is IOFramebuffer's 351 slots (both 0xb08 bytes).
static constexpr uintptr_t kKIOLog = 0xffffff8000a859d0ull, kKIOSleep = 0xffffff8000a85960ull;
static constexpr uintptr_t kKGmdVtable = 0xffffff800026b5d8ull;
static constexpr unsigned  kKGmdSlots = 72, kKGmdDoMapSlot = 70;
static constexpr uintptr_t kKGmdD1 = 0xffffff8000ada060ull, kKGmdGetMeta = 0xffffff8000ada870ull, kKGmdDoMap = 0xffffff8000adc940ull;
static constexpr unsigned  kFbSlots = 351, kFbGetVramSlot = 310;

typedef IOReturn (*GmdDoMapFn)(void *, void *, uint64_t *, IOOptionBits, uint64_t, uint64_t);
typedef IODeviceMemory *(*FbGetVramFn)(void *);

static struct {
    uint32_t armed, viaBootArg, notifyInstalled, lastStatus;
    uintptr_t kslide;
    void **gmdCopy, **fbCopy, **fbOrigVt;
    IOService *fb;
    GmdDoMapFn origDoMap;
    FbGetVramFn origGetVram;
    uint64_t getVram, wrapped, refused, doMaps, forcedWc, keptMode, logged;
    uint64_t fbBase, fbLen;
} gWc {};

static IOReturn wc_doMap(void *self, void *map, uint64_t *addr, IOOptionBits options, uint64_t off, uint64_t len) {
    gWc.doMaps++;
    IOOptionBits o = options;
    if ((options & kIOMapCacheMask) == kIOMapDefaultCache) { o = (options & ~(IOOptionBits)kIOMapCacheMask) | (IOOptionBits)kIOMapWriteCombineCache; gWc.forcedWc++; }
    else gWc.keptMode++;
    if (gWc.logged < 24u) {
        gWc.logged++;
        char pn[20] = { 0 };
        proc_selfname(pn, (int)sizeof(pn));
        DPGLOG("fbwc: doMap #%llu on %p options %#x -> %#x (%s) from pid %d (%s)", (unsigned long long)gWc.doMaps, self, options, o,
               o != options ? "WRITE-COMBINE forced" : "caller's cache mode kept", proc_selfpid(), pn);
    }
    return gWc.origDoMap(self, map, addr, o, off, len);
}

static IODeviceMemory *wc_getVram(void *self) {
    IODeviceMemory *md = gWc.origGetVram ? gWc.origGetVram(self) : nullptr;
    gWc.getVram++;
    if (!md || !gWc.gmdCopy) return md;
    void ***slot = reinterpret_cast<void ***>(md);
    void **vt = *slot;
    IOByteCount seglen = 0;
    const uint64_t phys = md->getPhysicalSegment(0, &seglen, kIOMemoryMapperNone);
    if (vt != reinterpret_cast<void **>(kKGmdVtable + gWc.kslide + 16) || strcmp(dpg_class(md), "IOGeneralMemoryDescriptor") ||
        phys != gWc.fbBase || md->getLength() != gWc.fbLen) {
        gWc.refused++;
        if (gWc.refused <= 4)
            DPGLOG("fbwc: getVRAMRange returned %p (%s) phys %#llx len %#llx vtable %p - NOT the scanout descriptor we expect, left as is",
                   md, dpg_class(md), (unsigned long long)phys, (unsigned long long)md->getLength(), vt);
        return md;
    }
    __asm__ __volatile__("sfence" ::: "memory");
    *slot = gWc.gmdCopy + kVtHdr;
    gWc.wrapped++;
    return md;
}

static uint32_t wc_arm_locked(IOService *fb) {
    if (gWc.armed) return 0;
    const uintptr_t ks = reinterpret_cast<uintptr_t>(&IOLog) - kKIOLog;
    if ((ks & 0xfff) || reinterpret_cast<uintptr_t>(&IOSleep) - kKIOSleep != ks) {
        DPGLOG("fbwc: REFUSED - kernel slide anchors disagree (IOLog %#lx, IOSleep %#lx)", (unsigned long)ks,
               (unsigned long)(reinterpret_cast<uintptr_t>(&IOSleep) - kKIOSleep));
        return 2;
    }
    void **gvt = reinterpret_cast<void **>(kKGmdVtable + ks + 16);
    if (reinterpret_cast<uintptr_t>(gvt[0]) - ks != kKGmdD1 || reinterpret_cast<uintptr_t>(gvt[7]) - ks != kKGmdGetMeta ||
        reinterpret_cast<uintptr_t>(gvt[kKGmdDoMapSlot]) - ks != kKGmdDoMap) {
        DPGLOG("fbwc: REFUSED - IOGeneralMemoryDescriptor vtable slots 0/7/70 are not D1/getMetaClass/doMap"); return 3;
    }
    if (!fb || !dpg_is_our_fb_name(fb->getMetaClass()->getClassName())) return 4;
    uint64_t base = 0, len = 0;
    if (!dpg_prop_u64(fb, "Console,BaseAddress", &base) || !dpg_prop_u64(fb, "Console,Length", &len) || !base || !len) return 5;
    void **fvt = *reinterpret_cast<void ***>(fb);
    const FbGetVramFn orig = reinterpret_cast<FbGetVramFn>(fvt[kFbGetVramSlot]);
    // Identity of slot 310 by behaviour: it must hand back the scanout range RDNA4FB publishes.
    IODeviceMemory *probe = orig(fb);
    IOByteCount sl = 0;
    const bool same = probe && probe->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) == base && probe->getLength() == len;
    if (probe) probe->release();
    if (!same) { DPGLOG("fbwc: REFUSED - RDNA4FB slot 310 did not return the scanout range %#llx+%#llx", (unsigned long long)base, (unsigned long long)len); return 6; }
    void **gc = static_cast<void **>(IOMalloc((kKGmdSlots + kVtHdr) * sizeof(void *)));
    void **fc = static_cast<void **>(IOMalloc((kFbSlots + kVtHdr) * sizeof(void *)));
    if (!gc || !fc) {
        if (gc) IOFree(gc, (kKGmdSlots + kVtHdr) * sizeof(void *));
        if (fc) IOFree(fc, (kFbSlots + kVtHdr) * sizeof(void *));
        return 7;
    }
    memcpy(gc, gvt - kVtHdr, (kKGmdSlots + kVtHdr) * sizeof(void *));
    memcpy(fc, fvt - kVtHdr, (kFbSlots + kVtHdr) * sizeof(void *));
    gWc.origDoMap = reinterpret_cast<GmdDoMapFn>(gvt[kKGmdDoMapSlot]);
    gc[kVtHdr + kKGmdDoMapSlot] = reinterpret_cast<void *>(&wc_doMap);
    gWc.origGetVram = orig;
    fc[kVtHdr + kFbGetVramSlot] = reinterpret_cast<void *>(&wc_getVram);
    gWc.kslide = ks; gWc.gmdCopy = gc; gWc.fbCopy = fc; gWc.fbOrigVt = fvt; gWc.fb = fb; gWc.fbBase = base; gWc.fbLen = len;
    __asm__ __volatile__("sfence" ::: "memory");
    *reinterpret_cast<void ***>(fb) = fc + kVtHdr;
    gWc.armed = *reinterpret_cast<void ***>(fb) == fc + kVtHdr ? 1u : 0u;
    DPGLOG("fbwc: %s - kernel slide %#lx; RDNA4FB %p slot 310 getVRAMRange now wraps its descriptor so a default-cache mapping of "
           "%#llx+%#llx is WRITE-COMBINED", gWc.armed ? "ARMED" : "swap read-back FAILED", (unsigned long)ks, fb,
           (unsigned long long)base, (unsigned long long)len);
    return gWc.armed ? 0 : 8;
}

static bool wc_publish_handler(void *target, void *refCon, IOService *newService, IONotifier *notifier) {
    (void)target; (void)refCon; (void)notifier;
    dpg_lock_init();
    IOLockLock(gDpgLock);
    gWc.lastStatus = wc_arm_locked(newService);
    IOLockUnlock(gDpgLock);
    DPGLOG("fbwc: RDNA4FB published (%p) -> arm status %u", newService, gWc.lastStatus);
    return true;
}

// Called from Navi48Bringup::start: boot-arg navi48-fbwc=1 installs a first-publish notification for RDNA4FB.
void navi48_fbwc_boot(void) {
    uint32_t v = 0;
    if (!PE_parse_boot_argn("navi48-fbwc", &v, sizeof(v)) || v != 1) return;
    dpg_lock_init();
    gWc.viaBootArg = 1;
    // A matching dictionary holds ONE class name, and since 0.0.307 our framebuffer may publish under
    // either of two (see dpg_is_our_fb_name). Register the notification for BOTH: exactly one can ever
    // match in a given boot, so at most one fires, and neither name can be the one we forgot.
    static const char *const kOurFbClasses[2] = { "RDNA4FB", "AMDRDNA4FB" };
    unsigned installed = 0;
    for (unsigned i = 0; i < 2; i++) {
        OSDictionary *match = IOService::serviceMatching(kOurFbClasses[i]);
        IONotifier *n = match ? IOService::addMatchingNotification(gIOFirstPublishNotification, match, &wc_publish_handler,
                                                                   nullptr, nullptr, 0) : nullptr;
        if (match) match->release();
        if (n) installed++;
        DPGLOG("fbwc: first-publish notification for %s %s", kOurFbClasses[i], n ? "installed" : "FAILED");
    }
    gWc.notifyInstalled = installed ? 1u : 0u;
    DPGLOG("fbwc: boot-arg navi48-fbwc=1 - %u of 2 first-publish notifications installed", installed);
}

// action 69 `fbwc [1]`: 1 arms now (affects mappings made afterwards only), 0 reads. out[0] status (0 ok, 2 kernel slide, 3 descriptor
// vtable, 4 no RDNA4FB, 5 geometry, 6 slot 310 identity, 7 alloc, 8 swap), [1] armed | via boot-arg << 1 | notifier << 2,
// [2] getVRAMRange calls, [3] descriptors wrapped, [4] refused, [5] doMap calls, [6] write-combine forced, [7] cache mode kept.
uint32_t navi48_fbwc_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint32_t st = gWc.lastStatus;
    if (arg == 1) {
        IOLockLock(gDpgLock);
        st = wc_arm_locked(dpg_find_rdna4fb());
        gWc.lastStatus = st;
        IOLockUnlock(gDpgLock);
    }
    const uint64_t v[8] = { st, (uint64_t)gWc.armed | ((uint64_t)gWc.viaBootArg << 1) | ((uint64_t)gWc.notifyInstalled << 2), gWc.getVram,
                            gWc.wrapped, gWc.refused, gWc.doMaps, gWc.forcedWc, gWc.keptMode };
    DPGLOG("fbwc: control %llu -> status %u; armed %u (boot-arg %u); getVRAMRange %llu, wrapped %llu, refused %llu; doMap %llu, WC forced "
           "%llu, mode kept %llu", (unsigned long long)arg, st, gWc.armed, gWc.viaBootArg, (unsigned long long)gWc.getVram,
           (unsigned long long)gWc.wrapped, (unsigned long long)gWc.refused, (unsigned long long)gWc.doMaps,
           (unsigned long long)gWc.forcedWc, (unsigned long long)gWc.keptMode);
    if (out) for (unsigned i = 0; i < count && i < 8; i++) out[i] = v[i];
    return st;
}

// =====================================================================================================================
// E (control) — action 70 `pipeshim [0|1|2|3|4]`
// =====================================================================================================================
// 0 turns the shim OFF (the safety core refuses again) and reads; 1 participates and CENSUSES every transaction without
// presenting anything; 2 additionally copies plane 0 into RDNA4FB's scanout on SDMA0 QUEUE0; 3 (0.0.338) READS AND
// CHANGES NOTHING - the argument `accel pipeshim` with no scalar cannot express, because a missing scalar arrives as 0.
// 4 (0.0.412) is mode 2 with the swizzle-3 plane forced down the EXISTING linear row copy - the same
// navi48_scanout_copy_vram call the swizzle-0 path already makes, same arguments, same destination. DEFAULT OFF: 0/1/2
// clear it (so a swizzle-3 plane is tiled exactly as in 0.0.411), and 3 touches no state.
// Status: 0 ok, 1 the safety core is not armed on every pipe (the shim lives on ITS vtable copy, so there is nothing to
// turn on), 2 argument out of range, 3 mode 2 (or 4) refused because the scanout positive control has not passed this boot.
// out[0] status, [1] mode | armed << 8, [2] validate, [3] perform, [4] submit | isComplete << 32, [5] census | planes
// resolved << 32, [6] presents done | refused << 32, [7] last copy status | last fence us << 32, [8] last plane
// width << 32 | height, [9] last plane VRAM offset, [10] last plane VidMemory length, [11] pipe+0x23c submit index |
// pipe+0x238 retire index << 32, [12] pipe+0x248 live transaction != 0 | pipe+0x2a4 << 1 | pipe+0x298 ready << 2.
uint32_t navi48_pipeshim_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint32_t st = 0;
    // 0.0.338: ARGUMENT 3 IS A READ THAT CHANGES NOTHING, and it exists because the thing
    // everyone called "the read" was not one. `navi48test accel pipeshim` with no argument passes scalar 0,
    // and 0 is "turn the shim OFF" — so `accel-run.sh`'s `pipeshimread` step, whose own comment says
    // "read: mode, per-slot counts, ring indices", DISARMED THE SHIM every time it ran. Run `mmv1` caught it
    // live: `pipeshim-1` set mode 1, `pipeshim-2` correctly refused with status 3 and left mode 1, and the
    // very next step, the "read", put it back to 0 — so the whole armed window ran with the guard refusing.
    // An instrument that destroys what it measures. 3 takes no lock and touches no state; it deliberately
    // does NOT test `navi48_pipeguard_armed_all()` either, because a read must work from any state (the
    // armed bit is reported in out[1] regardless).
    if (arg > 5) st = 2;
    else if (arg == 3) { /* READ ONLY — fall through to the reporting below, change nothing */ }
    else if (!navi48_pipeguard_armed_all()) st = 1;
    else if ((arg == 2 || arg == 4 || arg == 5) && !navi48_scanout_pc_passed()) st = 3;
    else {
        // ARG 4 is NOT a fourth mode: it is mode 2 plus S1's forced-linear flag. ARG 5 is mode 2 with
        // the GCR_REQ (notes/design/SDMA-GCR.md G3). Every other accepted argument clears both, so the default and
        // all pre-0.0.416 callers keep the tiled swizzle-3 call exactly (18 dwords, no GCR).
        IOLockLock(gDpgLock);
        const uint32_t wasLinear = gSh.forceLinear;
        gSh.mode = (arg == 4 || arg == 5) ? 2u : (uint32_t)arg;
        gSh.forceLinear = (arg == 4) ? 1u : 0u;
        gSh.gcr = (arg == 5) ? 1u : 0u;
        // 0.0.412 (reviewer, + the review's binding 1): ARG 4 RE-OPENS the S2 read-back budget only on the
        // 0 -> 1 transition of forceLinear, so the wscompose tick re-asserting 4 every ~3 s cannot make the budget unbounded.
        if (arg == 4 && !wasLinear) { gSh.verifyRuns = 0u; gSh.lastVerifyToken = 0ull; }
        IOLockUnlock(gDpgLock);
    }
    gSh.lastStatus = st;
    // The ring indices, read straight off pipe[0] after the same identity checks `pipeguard` uses.
    uint64_t ring = 0, live = 0;
    void *accel = navi48_accel_object();
    uint32_t rsr = 0;
    const uintptr_t rslide = accel ? navi48_x6000_slide(&rsr) : 0;
    if (accel && rslide) {
        void *dm = *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff);
        if (dpg_is(dm, rslide, kDpgDmGetMeta, "AMDRadeonX6000_AMDAccelDisplayMachine")) {
            const uint32_t fbc = *reinterpret_cast<uint32_t *>(static_cast<char *>(dm) + kDpgDmFbCountOff);
            void *p = (fbc && fbc <= kDpgMaxPipes) ? *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff) : nullptr;
            if (dpg_is(p, rslide, kDpgPipeGetMeta, "AMDRadeonX6000_AMDAccelDisplayPipe")) {
                const char *pc = static_cast<const char *>(p);
                const uint32_t sub = *reinterpret_cast<const uint32_t *>(pc + 0x23c);
                const uint32_t ret = *reinterpret_cast<const uint32_t *>(pc + 0x238);
                ring = (uint64_t)sub | ((uint64_t)ret << 32);
                // 0.0.331: THE FIVE `transactionEnd` GATES, READ TOGETHER IN ONE PASS.
                // IOAccelDisplayPipeUserClient2::transactionEnd (external method SELECTOR 8, sDisplayMethodDescs
                // @ 0x14600950) tests five single bytes and returns kIOReturnNotReady (0xe00002d8) if ANY of them
                // fails - the code run `sacomp2` saw. They have never been read in one place, so it has never been
                // known WHICH one refuses. All five are plain byte loads off objects this block has already
                // identity-checked: `accel` is proven by its +0x378 typechecking as AMDAccelDisplayMachine, and
                // `p` by dpg_is against AMDRadeonX6000_AMDAccelDisplayPipe. Nothing is written.
                //   accel+0xc78 bit 1 - set by IOGraphicsAccelerator2::enableAccelerator (`orb $0x2,0xc78(%rbx)`
                //     at 0x145c40d2). SUSPECTED: offset and instruction are the static reading, not bytes
                //     I disassembled; the RAW byte is logged so a wrong offset shows up as noise, not as a verdict.
                //   pipe+0x280, +0x282, +0x298 (already read as "ready"), +0x299 - SUSPECTED, same provenance.
                // The RAW bytes are logged; the packed word carries only booleans, and the polarity each gate
                // needs is NOT assumed here - this verb reports, it does not judge.
                const uint8_t gAccel = *reinterpret_cast<const uint8_t *>(static_cast<const char *>(accel) + 0xc78);
                const uint8_t g280 = (uint8_t)pc[0x280], g282 = (uint8_t)pc[0x282];
                const uint8_t g298 = (uint8_t)pc[0x298], g299 = (uint8_t)pc[0x299];
                live = (uint64_t)(*reinterpret_cast<void *const *>(pc + 0x248) != nullptr) |
                       ((uint64_t)(*reinterpret_cast<const uint32_t *>(pc + 0x2a4) != 0) << 1) |
                       ((uint64_t)(g298 != 0) << 2) |
                       ((uint64_t)((gAccel & 2) != 0) << 3) |
                       ((uint64_t)(g280 != 0) << 4) |
                       ((uint64_t)(g282 != 0) << 5) |
                       ((uint64_t)(g299 != 0) << 6);
                DPGLOG("pipeshim: pipe[0] %p ring submit index %u, retire index %u (depth %d of 4; wait_for_queue_slot_gated "
                       "blocks at 4, 0x145cf0f4); live transaction %p, +0x2a4 %u, +0x298 ready %u", p, sub, ret, (int)(sub - ret),
                       *reinterpret_cast<void *const *>(pc + 0x248), *reinterpret_cast<const uint32_t *>(pc + 0x2a4),
                       (unsigned)g298);
                // LEGEND CORRECTED in 0.0.334 (an earlier analysis; the old one said "a zero in any of the five returns
                // kIOReturnNotReady 0xe00002d8" and that is WRONG in both directions - it seeded hours of wrong work).
                // transactionEnd 0x145d3182, from bytes: +0x299 != 0 -> 0xe00002e3 (145d3213); +0x280 != 0 -> 0xe00002d7
                // (145d3221); accel+0xc78 bit 1 CLEAR -> 0xe00002d8 (145d323c); +0x282 != 0 -> 0xe00002d8 (145d3249);
                // isActive, i.e. +0x298 == 0 -> 0xe00002d8 (145d325a). THREE of the five fail on NON-zero and want 0;
                // two want a SET bit. All five PASS at their measured values, and there is no early-success exit.
                // NEVER WRITE pipe+0x280, +0x282 or +0x299 - 0 is their passing value.
                DPGLOG("pipeshim: TXEND GATES (selector 8) accel %p +0xc78 = %#x (bit1 %u, must be SET) | pipe %p "
                       "+0x280 = %#x (must be 0), +0x282 = %#x (must be 0), +0x298 = %#x (must be NON-zero), "
                       "+0x299 = %#x (must be 0) || +0x2a4 = %u (0.0.335: this is Apple's `TransactIR`, the transaction "
                       "interrupt ARMED flag, not a completion; 0 in every reading ever taken). "
                       "At these values ALL FIVE PASS .",
                       accel, (unsigned)gAccel, (unsigned)((gAccel >> 1) & 1), p,
                       (unsigned)g280, (unsigned)g282, (unsigned)g298, (unsigned)g299,
                       *reinterpret_cast<const uint32_t *>(pc + 0x2a4));
            }
        }
    }
    // 0.0.373: the readback bucket rides in the HIGH 32 bits of `live`, whose low bits 0..6 are the only
    // ones the five TXEND booleans use. The ABI is full at 16 scalars (13 extras), so there is no new slot to take.
    live |= ((uint64_t)(gSh.copyReadback > 0xffffffffull ? 0xffffffffull : gSh.copyReadback)) << 32;
    const uint64_t v[13] = {
        st, (uint64_t)gSh.mode | ((uint64_t)(navi48_pipeguard_armed_all() ? 1u : 0u) << 8) |
                ((uint64_t)(gSh.gcr ? 1u : 0u) << 9), gSh.validate, gSh.perform,
        gSh.submit | (gSh.isComplete << 32), gSh.census | (gSh.planes << 32), gSh.copyOk | (gSh.copyRefused << 32),
        (gSh.lastCopySt & 0xffffffffull) | (gSh.lastUs << 32), (gSh.lastPlaneW << 32) | gSh.lastPlaneH, gSh.lastPhys,
        gSh.lastLen, ring, live,
    };
    // 0.0.412: SPLIT IN TWO. Adding forceLinear and the S2 run count to the single line pushed its worst case
    // past the logger's 512-byte buffer (the old one was already at 714 bytes worst case and relied on small live counts);
    // each line below is under 480 body bytes at its maximum argument widths.
    DPGLOG("pipeshim: control %llu -> status %u (0 ok, 1 safety core not armed, 2 ARG out of range, 3 mode 2/4/5 "
           "refused: scanout positive control not passed; 3 also = READ ONLY, 0.0.338; 4 = mode 2 + forced linear, "
           "; 5 = mode 2 + GCR_REQ before the tiled copy, notes/design/SDMA-GCR.md G3); "
           "mode %u forceLinear %u GCR %u (tiled copy now %u dwords); S2 verify runs %u of at most %u",
           (unsigned long long)arg, st, gSh.mode, (unsigned)gSh.forceLinear, (unsigned)gSh.gcr,
           (unsigned)n48_sdma_tiled_copy_dwords(gSh.gcr != 0u), (unsigned)gSh.verifyRuns,
           (unsigned)N48_DPG_VERIFY_BUDGET);
    DPGLOG("pipeshim: validate %llu, perform %llu, submit %llu (kIOReturnNotReady), isComplete %llu; "
           "census %llu with %llu usable plane(s); presents %llu ok / %llu readback-differed / %llu refused, "
           "last copy status %#llx, last plane %llux%llu at VRAM %#llx +%#llx",
           (unsigned long long)gSh.validate, (unsigned long long)gSh.perform,
           (unsigned long long)gSh.submit, (unsigned long long)gSh.isComplete, (unsigned long long)gSh.census,
           (unsigned long long)gSh.planes, (unsigned long long)gSh.copyOk, (unsigned long long)gSh.copyReadback,
           (unsigned long long)gSh.copyRefused,
           (unsigned long long)gSh.lastCopySt, (unsigned long long)gSh.lastPlaneW, (unsigned long long)gSh.lastPlaneH,
           (unsigned long long)gSh.lastPhys, (unsigned long long)gSh.lastLen);
    // 0.0.373 - SAY WHICH IT IS. Every number above is a SNAPSHOT taken at the instant of this read, not
    // a boot total: arm3's closing `pipeshimread` said "presents 6 ok / 1 refused" and the boot went on to perform
    // fifteen, of which fourteen were clean and one had its readback disagree. A counter read mid-stream
    // is a snapshot, and the run summary quoted the snapshot as the result. Both are printed now, and the delta
    // between them is exactly how much of the stream this read did not see.
    DPGLOG("pipeshim: the line above is a SNAPSHOT AT THIS INSTANT, not a boot total. Snapshot: perform %llu, "
           "presents %llu ok + %llu readback-differed = %llu DELIVERED, %llu refused. Last HEARTBEAT (%u of at most 60, "
           "the boot's own last word so far, uptime %llu us): perform %llu, %llu ok + %llu readback-differed = %llu "
           "delivered, %llu refused. %llu present(s) happened after that heartbeat and before this read.",
           (unsigned long long)gSh.perform, (unsigned long long)gSh.copyOk, (unsigned long long)gSh.copyReadback,
           (unsigned long long)(gSh.copyOk + gSh.copyReadback), (unsigned long long)gSh.copyRefused, gSh.beats,
           (unsigned long long)gSh.beatUs, (unsigned long long)gSh.beatPerform, (unsigned long long)gSh.beatOk,
           (unsigned long long)gSh.beatReadback, (unsigned long long)(gSh.beatOk + gSh.beatReadback),
           (unsigned long long)gSh.beatRefused,
           (unsigned long long)(gSh.perform >= gSh.beatPerform ? gSh.perform - gSh.beatPerform : 0ull));
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}

// =====================================================================================================================
// F. Route B readiness — action 71 `pipemode [0|1]`
// =====================================================================================================================
// IOAccelDisplayPipe::init_framebuffer_resource (0x145cc2ec) is what normally sets the readiness flag pipe+0x298, and it
// runs four things the shim does not control before it: framebuffer slot 318 getCurrentDisplayMode (*0x9f0, 0x145cc352),
// framebuffer slot 317 getPixelInformation (*0x9e8, 0x145cc380), vendor slot 267 initFramebufferResource (*0x858,
// 0x145cc4f2 — AMD's is reserveFrameBuffer binding a VidMemory over VRAM offset 0, the hazard the safety core exists to
// stop) and then the AMD event machine accel+0x380 slot 41 twice (0x145cc521, 0x145cc546) plus resource slot 46
// (*0x170, 0x145cc55f) before `movb $0x1,0x298(%r14)` at 0x145cc569.
//
// Route B writes the fields that function writes and sets the flag, running NONE of that AMD code. The field list is
// CONFIRMED from the bytes (pipe = %r14, res = %rbx, the IOPixelInformation is the 0xac-byte local at -0xd4):
//   0x145cc404/0x145cc411/0x145cc41e/0x145cc42b  pipe+0x28c pixel-format code: 0xf for 32 bpp with 10 bits per
//                                                component, else 4 for 32 bpp; 3 for 15/16 bpp; 0 for 8 bpp (a failure)
//   0x145cc476  pipe+0x290 = (u32) activeWidth          0x145cc480  pipe+0x294 = (u16) activeHeight
//   0x145cc491  pipe+0x296 = (u16) bitsPerPixel >> 3
//   0x145cc499  res+0xb0 = (u16) width                  0x145cc4a0  res+0xb2 = (u16) height
//   0x145cc4a7  res+0xd8 = (u16) bytes per pixel        0x145cc4b4  res+0xb8 = (u64) bytesPerRow
//   0x145cc4be  res+0xc8 = (u64)(u32)(height * bytesPerRow)
//   0x145cc4d8  *(res+0x30) + 0x1c byte = 0             0x145cc4e0  res+0x28 = 0
//   0x145cc504  res+0xf &= ~8                           0x145cc552  res+0x88 = the vendor VidMemory  [NOT written: see below]
// The resource is pipe+0xe0 — getFramebufferResource(0) is exactly `movq 0xe0(%rdi),%rax` (0x145cc08e) and
// displayModeDidChange passes it to init_framebuffer_resource (0x145cc696). It is CONFIRMED non-NULL on any pipe that
// exists: IOAccelDisplayPipe::init stores accel->*0x8d0(0, 0x40) there at 0x145cb414 and dereferences it with NO null
// check on the very next instruction (0x145cb41b `orb $0x10,0xc(%rax)`), so a null would have panicked in init.
//
// res+0x88 is deliberately LEFT ALONE. Writing a kext-owned object there hands a foreign vtable to whatever later calls
// a virtual on it, and the design memo's own "what I could not establish" lists the present-path readers of that field
// as unsettled. The readback reports its live value so the first T-compose census can settle it.
//
// Status: 0 done, 1 no accelerator / slide, 2 display machine identity, 3 no pipe, 4 pipe identity, 5 pipe+0xe0 is not a
// resource, 6 framebuffer is not RDNA4FB, 7 the framebuffer's mode call refused, 8 the pixel information refused, 9 not
// 32 bpp, 10 the framebuffer's mode disagrees with RDNA4FB's own Console,* geometry, 11 mode out of range,
// 12 the safety core is not armed (route B must not leave a ready pipe behind an unguarded AMD flip).
// The two framebuffer slots are RDNA4FB's own (our kext, so its table is IOFramebuffer-shaped by construction), and the
// identity check on them is BEHAVIOURAL and made before anything is written: the mode they describe must equal RDNA4FB's
// published Console,Width/Height/RowBytes/Depth, which is also the range our SDMA copy writes. Wrong slots cannot pass it.
typedef IOReturn (*FbCurrentModeFn)(void *, int32_t *, int32_t *);
typedef IOReturn (*FbPixelInfoFn)(void *, int32_t, int32_t, int32_t, void *);
static constexpr unsigned kFbCurrentModeSlot = 318;   // *0x9f0, init_framebuffer_resource 0x145cc352
static constexpr unsigned kFbPixelInfoSlot   = 317;   // *0x9e8, init_framebuffer_resource 0x145cc380

uint32_t navi48_pipemode_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint64_t v[13] = { 0 };
    uint32_t st = 0;
    if (arg > 1) st = 11;
    void *accel = navi48_accel_object();
    uint32_t sr = 0;
    const uintptr_t slide = accel ? navi48_x6000_slide(&sr) : 0;
    void *p = nullptr, *res = nullptr;
    IOService *fb = nullptr;
    if (!st && (!accel || !slide)) st = 1;
    if (!st) {
        void *dm = *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff);
        if (!dpg_is(dm, slide, kDpgDmGetMeta, "AMDRadeonX6000_AMDAccelDisplayMachine")) st = 2;
        else {
            const uint32_t fbc = *reinterpret_cast<uint32_t *>(static_cast<char *>(dm) + kDpgDmFbCountOff);
            if (!fbc || fbc > kDpgMaxPipes) st = 3;
            else p = *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff);
        }
    }
    if (!st && !dpg_is(p, slide, kDpgPipeGetMeta, "AMDRadeonX6000_AMDAccelDisplayPipe")) st = 4;
    if (!st) {
        res = *reinterpret_cast<void **>(static_cast<char *>(p) + 0xe0);
        if (!dpg_is_kind_of(res, "IOAccelResource2")) {
            DPGLOG("pipemode: REFUSED - pipe+0xe0 %p is %s, not a kind-of IOAccelResource2", res, dpg_class(res));
            st = 5;
        }
        void *fbv = *reinterpret_cast<void **>(static_cast<char *>(p) + 0x98);
        fb = dpg_kptr(fbv) ? OSDynamicCast(IOService, static_cast<OSObject *>(fbv)) : nullptr;
        if (!st && (!fb || !dpg_is_our_fb_name(fb->getMetaClass()->getClassName()))) {
            DPGLOG("pipemode: REFUSED - pipe+0x98 framebuffer %p is %s, not RDNA4FB", fbv, dpg_class(fbv));
            st = 6;
        }
    }
    // The geometry, taken exactly where init_framebuffer_resource takes it, then cross-checked against the Console,*
    // properties that govern OUR SDMA copy's destination. If the two disagree the pipe would advertise a frame the
    // scanout cannot hold, so this refuses rather than guesses.
    int32_t mode = 0, depth = 0;
    uint8_t pix[0xac];
    uint32_t width = 0, height = 0, rowBytes = 0, bpp = 0, bpc = 0, fmt = 0;
    uint64_t cw = 0, ch = 0, crb = 0, cd = 0;
    if (!st) {
        void **fvt = *reinterpret_cast<void ***>(fb);
        memset(pix, 0xaa, sizeof(pix));
        if (reinterpret_cast<FbCurrentModeFn>(fvt[kFbCurrentModeSlot])(fb, &mode, &depth) != kIOReturnSuccess) st = 7;
        else if (reinterpret_cast<FbPixelInfoFn>(fvt[kFbPixelInfoSlot])(fb, mode, depth, 0, pix) != kIOReturnSuccess) st = 8;
        else {
            rowBytes = *reinterpret_cast<const uint32_t *>(pix + 0x00);
            bpp      = *reinterpret_cast<const uint32_t *>(pix + 0x08);
            bpc      = *reinterpret_cast<const uint32_t *>(pix + 0x14);
            width    = *reinterpret_cast<const uint32_t *>(pix + 0x9c);
            height   = *reinterpret_cast<const uint32_t *>(pix + 0xa0);
            fmt = (bpp == 0x20) ? (bpc == 0xa ? 0xfu : 4u) : (bpp == 0xf || bpp == 0x10) ? 3u : (bpp == 8) ? 0u : 4u;
            if (bpp != 0x20) st = 9;
        }
    }
    if (!st) {
        if (!dpg_prop_u64(fb, "Console,Width", &cw) || !dpg_prop_u64(fb, "Console,Height", &ch) ||
            !dpg_prop_u64(fb, "Console,RowBytes", &crb) || !dpg_prop_u64(fb, "Console,Depth", &cd) ||
            cw != width || ch != height || crb != rowBytes || cd != bpp) {
            DPGLOG("pipemode: REFUSED - the framebuffer's current mode %d depth %d says %ux%u rowBytes %u bpp %u, but "
                   "RDNA4FB's Console,* properties say %llux%llu rowBytes %llu depth %llu; the SDMA copy writes the Console "
                   "range, so a ready pipe must describe the SAME frame", mode, depth, width, height, rowBytes, bpp,
                   (unsigned long long)cw, (unsigned long long)ch, (unsigned long long)crb, (unsigned long long)cd);
            st = 10;
        }
    }
    if (!st && arg == 1 && !navi48_pipeguard_armed_all()) st = 12;
    char *pc = static_cast<char *>(p);
    char *rc = static_cast<char *>(res);
    if (!st && arg == 1) {
        IOLockLock(gDpgLock);
        *reinterpret_cast<uint32_t *>(pc + 0x28c) = fmt;
        *reinterpret_cast<uint32_t *>(pc + 0x290) = width;
        *reinterpret_cast<uint16_t *>(pc + 0x294) = (uint16_t)height;
        *reinterpret_cast<uint16_t *>(pc + 0x296) = (uint16_t)(bpp >> 3);
        *reinterpret_cast<uint16_t *>(rc + 0xb0)  = (uint16_t)width;
        *reinterpret_cast<uint16_t *>(rc + 0xb2)  = (uint16_t)height;
        *reinterpret_cast<uint16_t *>(rc + 0xd8)  = (uint16_t)(bpp >> 3);
        *reinterpret_cast<uint64_t *>(rc + 0xb8)  = rowBytes;
        *reinterpret_cast<uint64_t *>(rc + 0xc8)  = (uint64_t)(uint32_t)(height * rowBytes);
        *reinterpret_cast<uint32_t *>(rc + 0x28)  = 0;
        rc[0xf] = (char)(rc[0xf] & ~8);
        void *r30 = *reinterpret_cast<void **>(rc + 0x30);
        if (dpg_kptr(r30)) static_cast<char *>(r30)[0x1c] = 0;
        __asm__ __volatile__("sfence" ::: "memory");
        pc[0x298] = 1;
        IOLockUnlock(gDpgLock);
        DPGLOG("pipemode: ROUTE B - pipe %p resource %p set from the framebuffer's own mode %d depth %d: %ux%u rowBytes %u "
               "bpp %u bpc %u -> format code %u; pipe+0x298 readiness %u (isActive 0x145cc03e reads this byte). AMD's "
               "initFramebufferResource, the event machine's slot 41 and resource slot 46 were NOT run; resource+0x88 "
               "VidMemory left as %p", p, res, mode, depth, width, height, rowBytes, bpp, bpc, fmt,
               (unsigned)(uint8_t)pc[0x298], *reinterpret_cast<void **>(rc + 0x88));
    }
    // The readback: every field route B is responsible for, live.
    if (p && res && st != 1 && st != 2 && st != 3 && st != 4 && st != 5) {
        v[3] = (uint64_t)*reinterpret_cast<const uint32_t *>(pc + 0x28c) |
               ((uint64_t)*reinterpret_cast<const uint32_t *>(pc + 0x290) << 32);
        v[4] = (uint64_t)*reinterpret_cast<const uint16_t *>(pc + 0x294) |
               ((uint64_t)*reinterpret_cast<const uint16_t *>(pc + 0x296) << 16) |
               ((uint64_t)(uint8_t)pc[0x298] << 32) | ((uint64_t)(uint8_t)pc[0x282] << 40);
        v[5] = (uint64_t)*reinterpret_cast<const uint16_t *>(rc + 0xb0) |
               ((uint64_t)*reinterpret_cast<const uint16_t *>(rc + 0xb2) << 16) |
               ((uint64_t)*reinterpret_cast<const uint16_t *>(rc + 0xd8) << 32);
        v[6] = *reinterpret_cast<const uint64_t *>(rc + 0xb8);
        v[7] = *reinterpret_cast<const uint64_t *>(rc + 0xc8);
        v[8] = reinterpret_cast<uint64_t>(*reinterpret_cast<void *const *>(rc + 0x88));
        v[9] = reinterpret_cast<uint64_t>(res);
        DPGLOG("pipemode: READBACK pipe %p +0x28c format %u +0x290 width %u +0x294 height %u +0x296 bpp/8 %u +0x298 ready %u; "
               "resource %p +0xb0 %u +0xb2 %u +0xd8 %u +0xb8 rowBytes %llu +0xc8 bytes %llu +0x88 VidMemory %p (%s)",
               p, *reinterpret_cast<const uint32_t *>(pc + 0x28c), *reinterpret_cast<const uint32_t *>(pc + 0x290),
               *reinterpret_cast<const uint16_t *>(pc + 0x294), *reinterpret_cast<const uint16_t *>(pc + 0x296),
               (unsigned)(uint8_t)pc[0x298], res, *reinterpret_cast<const uint16_t *>(rc + 0xb0),
               *reinterpret_cast<const uint16_t *>(rc + 0xb2), *reinterpret_cast<const uint16_t *>(rc + 0xd8),
               (unsigned long long)*reinterpret_cast<const uint64_t *>(rc + 0xb8),
               (unsigned long long)*reinterpret_cast<const uint64_t *>(rc + 0xc8),
               *reinterpret_cast<void *const *>(rc + 0x88), dpg_class(*reinterpret_cast<void *const *>(rc + 0x88)));
    }
    v[0] = st;
    v[1] = (uint64_t)(uint32_t)mode | ((uint64_t)(uint32_t)depth << 32);
    v[2] = (uint64_t)width | ((uint64_t)height << 16) | ((uint64_t)bpp << 32) | ((uint64_t)bpc << 40);
    v[10] = rowBytes;
    v[11] = cw | (ch << 16) | (crb << 32);
    v[12] = (uint64_t)(navi48_pipeguard_armed_all() ? 1u : 0u) | ((uint64_t)gSh.mode << 8);
    DPGLOG("pipemode: control %llu -> status %u (0 done, 1 accelerator/slide, 2 display machine, 3 no pipe, 4 pipe identity, "
           "5 pipe+0xe0, 6 not RDNA4FB, 7 getCurrentDisplayMode, 8 getPixelInformation, 9 not 32 bpp, 10 mode/Console "
           "disagree, 11 argument, 12 safety core not armed)", (unsigned long long)arg, st);
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}

// =====================================================================================================================
// G (control) — action 73 `routea [0|1]` (0.0.295, an internal review note)
// =====================================================================================================================
// Corrected route A, default OFF. Arms only on the safety core's guarded pipes and NEVER calls reserveFrameBuffer.
// Status: 0 ok, 1 safety core not armed, 2 accelerator/slide, 3 display machine, 4 no pipe / pipe identity,
// 5 pipe+0xe0 is not the AMDGFX10Resource vtable we expect (slot 46 != prepare), 6 framebuffer not RDNA4FB,
// 7 geometry read/format or Console,* disagree, 8 allocation, 9 resource vptr swap read-back, 10 arg out of range.
static uint32_t ra_arm_locked(uint64_t arg) {
    // Project verb convention (like emcensus/gfxneuter): 0 = READ ONLY (never changes state), 1 = arm / mode on,
    // 2 = mode off. Once armed, a read (0) must NOT disarm the runtime mode -: routeA3 stayed inert because the
    // `routearead` step and the watch's per-tick routea reads (all arg 0) were setting mode 0 under the old semantics.
    if (gRa.armed) {
        if (arg == 1) gRa.mode = 1u;
        else if (arg == 2) gRa.mode = 0u;
        return 0;                                                    // arg 0 = pure read, mode unchanged
    }
    if (arg != 1) return 0;                                          // read (0) or mode-off (2) with nothing armed is a no-op read
    if (!navi48_pipeguard_armed_all()) return 1;
    void *accel = navi48_accel_object();
    uint32_t sr = 0;
    const uintptr_t slide = accel ? navi48_x6000_slide(&sr) : 0;
    if (!accel || !slide) return 2;
    void *dm = *reinterpret_cast<void **>(static_cast<char *>(accel) + kDpgAccelDisplayMachineOff);
    if (!dpg_is(dm, slide, kDpgDmGetMeta, "AMDRadeonX6000_AMDAccelDisplayMachine")) return 3;
    const uint32_t fbCount = *reinterpret_cast<uint32_t *>(static_cast<char *>(dm) + kDpgDmFbCountOff);
    if (!fbCount || fbCount > kDpgMaxPipes) return 4;
    void *p0 = *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff);
    if (!dpg_is(p0, slide, kDpgPipeGetMeta, "AMDRadeonX6000_AMDAccelDisplayPipe")) return 4;
    void *fbv = *reinterpret_cast<void **>(static_cast<char *>(p0) + 0x98);
    IOService *fb = dpg_kptr(fbv) ? OSDynamicCast(IOService, static_cast<OSObject *>(fbv)) : nullptr;
    if (!fb || !dpg_is_our_fb_name(fb->getMetaClass()->getClassName())) return 6;

    // Geometry, exactly where init_framebuffer_resource takes it (slots 318/317), cross-checked against Console,* so the
    // object advertises the same frame the scanout holds. (Same computation route B uses; RDNA4FB's console mode is fixed.)
    void **fvt = *reinterpret_cast<void ***>(fb);
    int32_t mode = 0, depth = 0;
    uint8_t pix[0xac];
    memset(pix, 0xaa, sizeof(pix));
    if (reinterpret_cast<FbCurrentModeFn>(fvt[kFbCurrentModeSlot])(fb, &mode, &depth) != kIOReturnSuccess) return 7;
    if (reinterpret_cast<FbPixelInfoFn>(fvt[kFbPixelInfoSlot])(fb, mode, depth, 0, pix) != kIOReturnSuccess) return 7;
    const uint32_t rowBytes = *reinterpret_cast<const uint32_t *>(pix + 0x00);
    const uint32_t bpp      = *reinterpret_cast<const uint32_t *>(pix + 0x08);
    const uint32_t bpc      = *reinterpret_cast<const uint32_t *>(pix + 0x14);
    const uint32_t width    = *reinterpret_cast<const uint32_t *>(pix + 0x9c);
    const uint32_t height   = *reinterpret_cast<const uint32_t *>(pix + 0xa0);
    const uint32_t fmt = (bpp == 0x20) ? (bpc == 0xa ? 0xfu : 4u) : (bpp == 0xf || bpp == 0x10) ? 3u : (bpp == 8) ? 0u : 4u;
    if (bpp != 0x20) return 7;
    uint64_t cw = 0, ch = 0, crb = 0, cd = 0, cbase = 0, clen = 0;
    if (!dpg_prop_u64(fb, "Console,Width", &cw) || !dpg_prop_u64(fb, "Console,Height", &ch) ||
        !dpg_prop_u64(fb, "Console,RowBytes", &crb) || !dpg_prop_u64(fb, "Console,Depth", &cd) ||
        !dpg_prop_u64(fb, "Console,BaseAddress", &cbase) || !dpg_prop_u64(fb, "Console,Length", &clen) ||
        cw != width || ch != height || crb != rowBytes || cd != bpp || !cbase || clen < (uint64_t)height * rowBytes) {
        DPGLOG("routea: REFUSED - geometry/Console mismatch: mode %ux%u rowBytes %u bpp %u vs Console %llux%llu rb %llu depth %llu base %#llx len %#llx",
               width, height, rowBytes, bpp, (unsigned long long)cw, (unsigned long long)ch, (unsigned long long)crb,
               (unsigned long long)cd, (unsigned long long)cbase, (unsigned long long)clen);
        return 7;
    }

    // Per-pipe pre-check (no allocation yet): each pipe must be one the safety core already guards, its +0xe0 must carry
    // the exact AMDGFX10Resource vtable (which guarantees slot 46 = prepare and 111 slots), and all must share one table.
    void *resv[kDpgMaxPipes] = { nullptr };
    void **rvtv[kDpgMaxPipes] = { nullptr };
    for (uint32_t i = 0; i < fbCount; i++) {
        void *p = *reinterpret_cast<void **>(static_cast<char *>(dm) + kDpgDmPipeArrayOff + 8u * i);
        if (p != gPg.pipe[i]) { DPGLOG("routea: REFUSED - pipe[%u] %p is not the safety core's guarded pipe %p", i, p, gPg.pipe[i]); return 4; }
        void *res = *reinterpret_cast<void **>(static_cast<char *>(p) + 0xe0);
        if (!dpg_kptr(res)) { DPGLOG("routea: REFUSED - pipe[%u]+0xe0 is null", i); return 5; }
        void **rvt = *reinterpret_cast<void ***>(res);
        // Identity by CLASS + slot, not by the static vtable address: on this card (routeA1,) pipe+0xe0 carries a
        // PER-INSTANCE HEAP vtable (rvt - slide was a heap offset, not 0xbf26618), yet slot 46 still resolves to
        // AMDAccelResource::prepare 0xbdd6198. Require (a) kind-of IOAccelResource2 (as route B's pipemode does) and (b)
        // slot 46 == prepare. We copy this live vtable and swap ONLY this resource's vptr, so nothing else is touched.
        if (!dpg_is_kind_of(res, "IOAccelResource2") || !dpg_kptr(rvt) ||
            reinterpret_cast<uintptr_t>(rvt[N48_RA_PREPARE_SLOT]) - slide != (uintptr_t)N48_RA_PREPARE) {
            DPGLOG("routea: REFUSED - pipe[%u]+0xe0 %p (%s) vtable %p slot46 %#lx-slide is not a prepare-bearing IOAccelResource2",
                   i, res, dpg_class(res), rvt,
                   (unsigned long)(dpg_kptr(rvt) ? reinterpret_cast<uintptr_t>(rvt[N48_RA_PREPARE_SLOT]) - slide : 0));
            return 5;
        }
        if (i > 0 && rvt != rvtv[0]) { DPGLOG("routea: REFUSED - pipe[%u] resource vtable %p differs from pipe[0] %p (per-instance vtables: route A handles one framebuffer resource)", i, rvt, rvtv[0]); return 5; }
        resv[i] = res; rvtv[i] = rvt;
    }

    // Allocate: the object's kext-owned vtable (once), one slot-46-guarded copy of the resource vtable (once), and one
    // object per pipe. Nothing has been swapped yet, so a failure here just frees and returns.
    void **objVt = static_cast<void **>(IOMalloc(N48_RA_VT_SLOTS * sizeof(void *)));
    void **resCopy = dpg_make_copy(rvtv[0], N48_RA_RES_SLOTS);
    uint8_t *objs[kDpgMaxPipes] = { nullptr };
    bool allocOk = (objVt != nullptr && resCopy != nullptr);
    for (uint32_t i = 0; allocOk && i < fbCount; i++) { objs[i] = static_cast<uint8_t *>(IOMalloc(N48_RA_OBJ_SIZE)); if (!objs[i]) allocOk = false; }
    if (!allocOk) {
        if (objVt) IOFree(objVt, N48_RA_VT_SLOTS * sizeof(void *));
        if (resCopy) IOFree(resCopy, (N48_RA_RES_SLOTS + kVtHdr) * sizeof(void *));
        for (uint32_t i = 0; i < fbCount; i++) if (objs[i]) IOFree(objs[i], N48_RA_OBJ_SIZE);
        return 8;
    }
    for (unsigned s = 0; s < N48_RA_VT_SLOTS; s++) objVt[s] = reinterpret_cast<void *>(&ra_vt_stub);
    objVt[N48_RA_SLOT_RELEASE] = reinterpret_cast<void *>(&ra_release);
    objVt[N48_RA_SLOT_PHYSSEG] = reinterpret_cast<void *>(&ra_getphysseg);
    gRa.origPrepare = reinterpret_cast<RaPrepareFn>(rvtv[0][N48_RA_PREPARE_SLOT]);
    resCopy[kVtHdr + N48_RA_PREPARE_SLOT] = reinterpret_cast<void *>(&ra_prepare);
    for (uint32_t i = 0; i < fbCount; i++) {
        memset(objs[i], 0, N48_RA_OBJ_SIZE);
        *reinterpret_cast<void ***>(objs[i]) = objVt;        // +0x0 vtable (our own table; Apple indexes *0x28 / *0x158 forward)
        n48_ra_fill_object(objs[i], clen);                   // +0xc=0, +0xd=0x10, +0x38=0, +0x40=len
        gRa.obj[i] = objs[i]; gRa.res[i] = resv[i]; gRa.resOrigVt[i] = rvtv[i];
    }
    gRa.objVt = objVt; gRa.resCopy = resCopy; gRa.slide = slide;
    gRa.phys = cbase; gRa.len = clen;
    gRa.width = width; gRa.height = height; gRa.rowBytes = rowBytes; gRa.bpp = bpp; gRa.fmt = fmt;

    // Swap each framebuffer resource's vptr to the guarded copy. mode is still 0, so ra_prepare/dpg_initFb behave natively
    // until we flip it last.
    __asm__ __volatile__("sfence" ::: "memory");
    uint32_t swapped = 0;
    for (uint32_t i = 0; i < fbCount; i++) {
        *reinterpret_cast<void ***>(resv[i]) = resCopy + kVtHdr;
        if (*reinterpret_cast<void ***>(resv[i]) == resCopy + kVtHdr) swapped++;
    }
    if (swapped != fbCount) {
        DPGLOG("routea: REFUSED - resource vptr swap took on %u of %u pipes; leaving mode OFF (swapped copies forward to real prepare)", swapped, fbCount);
        gRa.mode = 0;
        return 9;
    }
    gRa.pipes = fbCount;
    gRa.armed = 1;
    gRa.mode = 1;
    DPGLOG("routea: ARMED - %u pipe(s); resource %p (%s) vtable %p [%u slots copied]; slot 267 returns a kext object "
           "(+0xd=0x10, +0x40=%#llx, slot43 phys %#llx) and sets pipe+0x298; slot 46 (prepare) neutralised on the "
           "framebuffer resource only (per-instance vptr swap, real prepare %p kept for pass-through); geometry %ux%u "
           "rowBytes %u fmt %u. reserveFrameBuffer is NEVER called (%s)",
           fbCount, resv[0], dpg_class(resv[0]), rvtv[0], N48_RA_RES_SLOTS, (unsigned long long)clen,
           (unsigned long long)cbase, reinterpret_cast<void *>(gRa.origPrepare), width, height, rowBytes, fmt, N48_RA_TOKEN);
    return 0;
}

// True iff route A is armed and every guarded resource still carries our slot-46 copy.
static bool ra_verified(void) {
    if (!gRa.armed || !gRa.resCopy) return false;
    for (uint32_t i = 0; i < gRa.pipes; i++)
        if (!gRa.res[i] || *reinterpret_cast<void *const *>(gRa.res[i]) != gRa.resCopy + kVtHdr) return false;
    return true;
}

// action 73 `routea [0|1]`: 1 arms (or, once armed, sets mode on); 0 sets mode off (inert; vptr stays swapped, forwarding
// to real prepare) and reads. out[0] status, [1] armed | mode << 1 | verified << 2 | pipeguard-armed << 3,
// [2] pipes | fmt << 16 | bpp << 24 | width << 32 | height << 48, [3] scanout phys, [4] object length,
// [5] initFb hits | ready writes << 32, [6] guard hits | pass-throughs << 32, [7] release | getPhysSeg << 32,
// [8] shim bypasses, [9] object vtable ptr, [10] resource copy ptr, [11] real prepare ptr, [12] origPrepare-minus-slide.
uint32_t navi48_routea_control(uint64_t arg, uint64_t *out, unsigned count) {
    dpg_lock_init();
    uint32_t st = 0;
    if (arg > 2) st = 10;                        // 0 read, 1 arm/mode on, 2 mode off
    else {
        IOLockLock(gDpgLock);
        st = ra_arm_locked(arg);
        gRa.lastStatus = st;
        IOLockUnlock(gDpgLock);
    }
    const uint64_t v[13] = {
        st,
        (uint64_t)(gRa.armed ? 1u : 0u) | ((uint64_t)(gRa.mode ? 1u : 0u) << 1) | ((uint64_t)(ra_verified() ? 1u : 0u) << 2) |
            ((uint64_t)(navi48_pipeguard_armed_all() ? 1u : 0u) << 3),
        (uint64_t)gRa.pipes | ((uint64_t)(gRa.fmt & 0xff) << 16) | ((uint64_t)(gRa.bpp & 0xff) << 24) |
            ((uint64_t)(gRa.width & 0xffff) << 32) | ((uint64_t)(gRa.height & 0xffff) << 48),
        gRa.phys, gRa.len,
        gRa.initFbHits | (gRa.readyWrites << 32), gRa.guardHits | (gRa.passThru << 32),
        gRa.releaseHits | (gRa.physSegHits << 32), gRa.shimBypass | (gRa.identityMiss << 32),
        reinterpret_cast<uint64_t>(gRa.objVt), reinterpret_cast<uint64_t>(gRa.resCopy),
        reinterpret_cast<uint64_t>(gRa.origPrepare),
    };
    DPGLOG("routea: control %llu -> status %u (0 ok, 1 safety core not armed, 2 accel/slide, 3 display machine, 4 pipe, "
           "5 resource vtable, 6 not RDNA4FB, 7 geometry, 8 alloc, 9 vptr swap, 10 arg); armed %u mode %u verified %u; "
           "pipes %u; slot267 initFb %llu ready %llu identityMiss %llu; slot46 guard %llu passthru %llu; release %llu physSeg %llu; shim bypass %llu",
           (unsigned long long)arg, st, gRa.armed, gRa.mode, ra_verified() ? 1u : 0u, gRa.pipes,
           (unsigned long long)gRa.initFbHits, (unsigned long long)gRa.readyWrites, (unsigned long long)gRa.identityMiss,
           (unsigned long long)gRa.guardHits, (unsigned long long)gRa.passThru, (unsigned long long)gRa.releaseHits,
           (unsigned long long)gRa.physSegHits, (unsigned long long)gRa.shimBypass);
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}
