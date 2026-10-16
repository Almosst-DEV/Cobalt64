//
//  native_armboot.cpp - see the header. 0.0.670: the kext's boot-time takeover, the N-card BOOT HOLD, and the kext's own boot publish.
//
//  This file holds NO decision that is not a return code check: the four display steps are the very functions the accel verbs expose
//  the accel verbs (`n48disp_verb` 83/84, `navi48_agdc_native_control` 88, `n48fbname::control` 78), called in the order the userspace recipe uses. Everything it can
//  write, it writes through them. What 0.0.670 adds is (a) the boot publish of the Metal nub (Navi48MetalNub::bootPublish), so the accelerator can
//  exist before WindowServer without a userspace publisher, and (b) the boot hold, so WindowServer's FIRST start waits for the arm instead of the arm
//  chasing a WindowServer restart. Both are decided by the pure code of amd/native_metal_pure.h.
//
#include "native_armboot.h"

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>

#include "Navi48Bringup.hpp"      // 0.0.670: OSDynamicCast for the boot publish's owner (Navi48MetalNub::bootPublish takes a Navi48Bringup *)
#include "Navi48MetalNub.hpp"     // 0.0.670: Navi48MetalNub::bootPublish
#include "amdgpu_log.h"
#include "native_disp.h"          // n48disp_verb / n48disp_latched_on / n48disp_pipe_adopted / n48metal_nub_published
#include "native_disp_pure.h"     // n48disp::kActAdopt / kActArm / kOk / kAlready / the refusal statuses / status_name
#include "native_agdc_pure.h"     // n48agdc::kPublished / kAlready / status_name
#include "native_s1c.h"           // amdgpu::n1c_ws_is_open / n1c_hung
#include "native_metal_pure.h"    // 0.0.670: publish_at_boot / the boot-hold transition table / hold_cap_ms
#include "../dcn/navi48_fbname.hpp"   // n48fbname::control

#define ABLOG(fmt, ...) AMDGPU_LOG("armboot", fmt, ##__VA_ARGS__)

// Navi48Bringup.cpp (the same forward declaration DisplayPipeGuard.cpp uses): our own IOService, NOT retained by the getter.
IOService *navi48_bringup_service(void);
// DisplayPipeGuard.cpp (declared by src/apple/Navi48Ttl.hpp): accel action 88, `pipeagdc [0|1]`.
uint32_t   navi48_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count);

namespace {

// ---- the boot-arg latches: read ONCE (first writer wins, never re-read). The master is default OFF, like every other switch of this kext. ----
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
volatile UInt32 gLatch     = kLatchUnset;   // navi48-armboot == 1: the kext takes the display pipe over at boot (and publishes the nub, and holds the boot)
volatile UInt32 gHoldLatch = kLatchUnset;   // navi48-nohold  == 1: do NOT hold the boot (the single-variable lever that isolates the hold's effect)
volatile UInt32 gNubLatch  = kLatchUnset;   // navi48-nubboot == 0: do NOT publish the nub from the kext (the on-demand-only path: the nub is published only when userspace asks)
volatile uint32_t gCapMs   = n48metal::kHoldCapMsDefault;   // the effective cap, clamped by n48metal::hold_cap_ms

// ---- the run: at most ONE worker per boot, and a stop request from the kext's stop path ----
volatile uint32_t gStarted = 0u;
volatile uint32_t gStop = 0u;
volatile uint32_t gState = 0u;      // RunState, for the log and the Navi48,ArmBoot property

// ---- 0.0.670: the boot hold. ONE word, four transitions (n48metal::HoldState); every release path is a CAS on it, so adjustBusy(-1) runs exactly
//      once per taken hold, whoever gets there first: the cap, the worker after the arm, or the kext's stop path. ----
volatile UInt32 gHold = (UInt32)n48metal::kHoldNone;
volatile uint32_t gHoldUsed = 0u;   // 1 while a busy count raised BY THIS FILE is outstanding (with navi48-nohold=1 it stays 0: nothing here owns one)
thread_call_t gCapCall = nullptr;

// The one deadline of the worker's wait, stored here so the retry loop can see it without passing a frame-local around (written once, before the thread).
uint64_t deadline_ns = 0;

enum RunState : uint32_t { kIdle = 0u, kWaiting = 1u, kArmed = 2u, kNotArmed = 3u, kStopped = 4u };

// THE ONLY WAITS IN THIS FILE, and they are bounded twice: a total budget and an IOSleep between attempts (no lock of ours is held across any of
// them: n48disp_verb takes and releases its own per-call busy CAS). 200 ms keeps the latency between "the accelerator exists" and "the gate byte is
// 1" small. The budget covers the whole boot chain of this route (the aux kext loads a couple of seconds after launchd starts
// it, the nub is published about ten seconds later and the four verbs finish a few seconds after that) plus the time between this kext's start() and launchd; the 35 s sits INSIDE the 40 s cap
// ON PURPOSE, so in every normal case the WORKER decides first and the cap is only the last-resort bound (which also refuses the arm).
constexpr uint64_t kBudgetNs = 35ull * 1000000000ull;
constexpr uint32_t kPollMs   = 200u;
constexpr uint32_t kProgressMs = 5000u;    // a "still waiting" line every 5 s: a verbose boot must never look frozen

// fbname's statuses: FBN_OK / FBN_ALREADY of navi48_fbname.cpp's file-local enum (navi48_fbname.cpp:64-76). Only control() is exported, so the two
// values this file needs are named here rather than duplicated logic: 0 = renamed (or restored), 9 = the class name was ALREADY AMDRDNA4FB (a second
// boot-time call, or an earlier `accel fbname 1` - the verb reports it as "already in that state" and nothing is written).
constexpr uint32_t kFbnameOk = 0u, kFbnameAlready = 9u;

uint64_t now_ns() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns; }
uint64_t uptime_ms() { return now_ns() / 1000000ull; }

bool stop_requested() { return __atomic_load_n(&gStop, __ATOMIC_ACQUIRE) != 0u; }

// Every transition is logged AND published, so a boot can be read back without a log daemon: `ioreg -l | grep ArmBoot`.
void set_state(uint32_t s, const char *what) {
    __atomic_store_n(&gState, s, __ATOMIC_RELEASE);
    if (IOService *svc = navi48_bringup_service()) svc->setProperty("Navi48,ArmBoot", what);
}

// ============================================================================================================================================================
// THE BOOT HOLD (0.0.670)
// ============================================================================================================================================================
// The four transitions are the pure table of amd/native_metal_pure.h (hold_take_ok / hold_claim_ok / hold_cap_ok / hold_release_ok and the four
// "after" states). Each of these functions performs exactly the transition its predicate allows and NOTHING else; the busy count moves WITH the
// transition, never on its own, so "the hold exists" and "the busy count is raised" cannot drift apart.
//
// Why adjustBusy(1) on the BRING-UP service and not on the PCI device: it is our own root service, the parent of its
// display nubs), and IOKit propagates a busy-state change up to the provider (IOKit/IOService.h, adjustBusy's discussion), so the IOPCIDevice is
// marked busy as well. WindowServer's IOKitWaitQuiet cannot return until the count is back to zero.

void hold_take(IOService *svc) {
    if (!OSCompareAndSwap((UInt32)n48metal::kHoldNone, (UInt32)n48metal::hold_after_take(), &gHold)) return;
    gHoldUsed = 1u;
    svc->adjustBusy(1);
    svc->setProperty("Navi48,BootHold", "holding");
    ABLOG("boot hold TAKEN (adjustBusy(1) on the bring-up service): WindowServer's IOKitWaitQuiet cannot return until the arm is done, so its "
          "FIRST start will find the display pipe armed - no pipereload, no WindowServer restart. Cap %u ms", gCapMs);
}

// The worker, right before the arm byte: held -> claimed, so the cap can no longer release the hold mid-arm. A false return means the cap already
// released it, i.e. the bring-up came too late and WindowServer is up or on its way - and then it must NOT arm.
bool hold_claim() { return OSCompareAndSwap((UInt32)n48metal::kHoldHeld, (UInt32)n48metal::hold_after_claim(), &gHold); }

// The worker, once the claim is taken: claimed -> none. THE ONE adjustBusy(-1) of the normal path.
bool hold_release(IOService *svc, const char *why) {
    if (!OSCompareAndSwap((UInt32)n48metal::kHoldClaimed, (UInt32)n48metal::hold_after_release(), &gHold)) return false;
    gHoldUsed = 0u;
    svc->adjustBusy(-1);
    svc->setProperty("Navi48,BootHold", why);
    ABLOG("boot hold RELEASED (%s) - WindowServer's IOKitWaitQuiet may return", why);
    return true;
}

// The worker gives up BEFORE it could arm: held -> none, the busy count goes back and WindowServer may start. This is the FAIL-OPEN path, and it is
// what keeps a bring-up that cannot finish from turning into a boot that does not finish. A no-op when the hold was never taken (navi48-nohold=1) or
// the cap got there first (the cap's own line was already printed).
bool hold_abandon(IOService *svc, const char *why) {
    if (!OSCompareAndSwap((UInt32)n48metal::kHoldHeld, (UInt32)n48metal::hold_after_cap(), &gHold)) return false;
    gHoldUsed = 0u;
    svc->adjustBusy(-1);
    svc->setProperty("Navi48,BootHold", why);
    ABLOG("boot hold RELEASED (%s) - WindowServer's IOKitWaitQuiet may return", why);
    return true;
}

// The worker's ONE exit, whatever the sequence decided: after the claim only a release can be live, before it only an abandon. Trying the release
// first and falling back to the abandon keeps that in one place - and makes "exactly one adjustBusy(-1)" true by construction rather than by care.
void hold_finish(IOService *svc, bool armed, const char *why) {
    if (gHoldUsed == 0u) return;
    if (hold_release(svc, armed ? "display armed" : "display PARTIAL")) return;   // the two outcomes the arm reports
    hold_abandon(svc, why);
}

// The CAP: held -> none, and the arm is refused from here on (hold_claim_ok(kHoldNone) is false). thread_call context is a kernel thread, so
// setProperty and the log are as safe here as they are in the worker.
void cap_call(thread_call_param_t p0, thread_call_param_t) {
    IOService *svc = static_cast<IOService *>(p0);
    if (svc) {
        if (OSCompareAndSwap((UInt32)n48metal::kHoldHeld, (UInt32)n48metal::hold_after_cap(), &gHold)) {
            gHoldUsed = 0u;
            svc->adjustBusy(-1);
            svc->setProperty("Navi48,BootHold", "CAP: released, the arm is refused from here on");
            ABLOG("boot hold RELEASED by the %u ms CAP - the bring-up did not finish in time, so it will NOT arm with WindowServer on its way. "
                  "This is NOT a freeze: WindowServer may start now", gCapMs);
        }
        svc->release();                     // the reference n48armboot_boot took for this timer
    }
    // Freeing the call from its own callback is documented safe (kern/thread_call.h: "Calling thread_call_free from a thread call's own callback is
    // safe; the work loop is held"), so this file does not leak one per boot.
    if (gCapCall) { thread_call_free(gCapCall); gCapCall = nullptr; }
}

// ============================================================================================================================================================
// the probes the worker polls with
// ============================================================================================================================================================
// Is OUR service in the registry yet? n48armboot_boot() runs at the very end of Navi48Bringup::start(), BEFORE registerService() (the hold is
// taken before the service is registered), and the boot publish ATTACHES AND REGISTERS a child of it, so the worker waits for the parent to be
// findable first. CHEAP and SILENT: the same non-blocking lookup accel_present() uses (serviceMatching + waitForMatchingService with a 0 timeout).
bool service_registered() {
    OSDictionary *m = IOService::serviceMatching("Navi48Bringup");
    if (!m) return false;
    IOService *s = IOService::waitForMatchingService(m, 0);
    m->release();
    if (!s) return false;
    s->release();
    return true;
}

// Is the aux accelerator in the registry? A CHEAP, SILENT probe with exactly the lookup native_disp.cpp's find_accel uses (serviceMatching +
// waitForMatchingService with a 0 timeout = non-blocking), so the retry loop can run for its whole budget without a single log line: the expensive
// verb (`pipeadopt`, which logs every verdict) is attempted only once this says the accelerator is there. Whether the accelerator has FINISHED
// starting is not this probe's business - adopt's own verdicts (kNoDisplayMachine / kNoPipe / kBusy) are what the retry set covers.
bool accel_present() {
    OSDictionary *m = IOService::serviceMatching("Navi48Accelerator");
    if (!m) return false;
    IOService *s = IOService::waitForMatchingService(m, 0);
    m->release();
    if (!s) return false;
    s->release();
    return true;
}

// The statuses that mean "not yet" rather than "never": the accelerator has not matched / the display machine is not up / the walk has not produced the
// pipe / requestProbe failed this time / another display verb is running. Everything else (a NULL pipe, an accelerator that fails its positive
// controls, pipes that are not ours, the facts, the capabilities property) is a refusal this worker must not retry: it stops the sequence, exactly as
// the userspace recipe stops at its first failed step.
bool adopt_retryable(uint32_t st) {
    return st == n48disp::kNoAccel || st == n48disp::kNoDisplayMachine || st == n48disp::kNoPipe ||
           st == n48disp::kProbeFailed || st == n48disp::kBusy;
}

// ============================================================================================================================================================
// the arm: the four steps above, in that order, each return code checked, the first failure ending the sequence
// ============================================================================================================================================================
struct ArmOut { bool ok; uint32_t state; const char *why; };

ArmOut arm_sequence() {
    uint64_t v[13] = { 0 };
    uint32_t st = (uint32_t)n48disp::kBadArg;

    // ---- 1. pipeadopt (83): the facts, requestProbe(1), the verified pipe, the capabilities property ----
    // It must be FIRST: adopt turns the resource fact bits on (they are what lets the display machine's walk be handed the PCI device at all,
    // native_disp_pure.h's pci_admit) and runs the probe that creates the pipe; `pipearm 1` refuses with kFactsOff without it, and without the probe
    // the display machine holds no pipe at all (kNoPipe).
    uint32_t lastLogged = 0xFFFFFFFFu;
    uint32_t lastProgress = 0u;
    for (;;) {
        if (stop_requested()) {
            ABLOG("the kext is stopping: giving up before adopt (uptime %llu ms)", (unsigned long long)uptime_ms());
            return ArmOut { false, kStopped, "stopped: the kext was unloading" };
        }
        if (n48metal_nub_published() && accel_present()) {
            st = n48disp_verb((uint32_t)n48disp::kActAdopt, 0ull, v, 13);
            if (st == (uint32_t)n48disp::kOk || st == (uint32_t)n48disp::kAlready) break;
            if (!adopt_retryable(st)) {
                ABLOG("pipeadopt REFUSED: status %u (%s) - NOT adopting; the sequence runs no further step and nothing is rolled back "
                      "(none of the four steps is destructive); the boot continues unchanged", st, n48disp::status_name(st));
                return ArmOut { false, kNotArmed, "not armed: pipeadopt refused" };
            }
            if (st != lastLogged) {                      // one line per DISTINCT status: a long wait must not flood the ring
                ABLOG("pipeadopt is not possible yet: status %u (%s) - retrying every %u ms", st, n48disp::status_name(st), kPollMs);
                lastLogged = st;
            }
        }
        const uint64_t now = now_ns();
        if (now >= deadline_ns) {
            ABLOG("no Navi48Accelerator under our nub within %llu ms (uptime %llu ms): NOT adopting. The nub itself is published by the kext at boot "
                  "(0.0.670) unless navi48-nubboot=0, so this is what a boot without the aux kext or with a failed "
                  "self-test looks like - the boot continues unchanged",
                  (unsigned long long)(kBudgetNs / 1000000ull), (unsigned long long)uptime_ms());
            return ArmOut { false, kNotArmed, "not armed: no accelerator in the budget" };
        }
        if ((uint32_t)(now / 1000000ull) - lastProgress >= kProgressMs) {   // the "not a freeze" line: a verbose boot screen must say so
            lastProgress = (uint32_t)(now / 1000000ull);
            ABLOG("still waiting for the Navi48Accelerator (%llu ms of %llu ms used; nub %s) - this is not a freeze",
                  (unsigned long long)((now - (deadline_ns - kBudgetNs)) / 1000000ull), (unsigned long long)(kBudgetNs / 1000000ull),
                  n48metal_nub_published() ? "published" : "not published yet");
        }
        IOSleep(kPollMs);
    }
    if (!n48disp_pipe_adopted()) {                       // the verb said OK but the state flag is not set: refuse to build on that
        ABLOG("pipeadopt returned %u (%s) but n48disp_pipe_adopted() is false: NOT arming", st, n48disp::status_name(st));
        return ArmOut { false, kNotArmed, "not armed: adopt state disagrees" };
    }
    ABLOG("pipeadopt -> %s (%u); pipe %#llx, flags %#llx, uptime %llu ms", n48disp::status_name(st), st,
          (unsigned long long)v[2], (unsigned long long)v[9], (unsigned long long)uptime_ms());

    // ---- 2. pipeagdc 1 (88): the native AGDC service, on a vtable copy, so IOPresentment's AGDC queries have an answer ----
    uint32_t ag = navi48_agdc_native_control(1ull, v, 13);
    if (ag != n48agdc::kPublished && ag != n48agdc::kAlready) {
        ABLOG("pipeagdc 1 -> status %u (%s): NOT arming (the sequence stops at its first failure, as the userspace recipe does)",
              ag, n48agdc::status_name(ag));
        return ArmOut { false, kNotArmed, "not armed: pipeagdc failed" };
    }
    ABLOG("pipeagdc 1 -> %s (%u); published %llu, registry ID %#llx", n48agdc::status_name(ag), ag,
          (unsigned long long)(v[1] & 1ull), (unsigned long long)v[2]);

    // ---- 3. fbname 1 (78): RDNA4FB's metaclass name -> AMDRDNA4FB, so CoreDisplay resolves the vendor (bit 0x4) ----
    // Before WindowServer's first device creation, which is where CoreDisplay reads it; this kext's stop path restores the original name.
    const uint32_t f = n48fbname::control(1ull, v, 13);
    if (f != kFbnameOk && f != kFbnameAlready) {
        ABLOG("fbname 1 -> status %u (0 = renamed, 9 = already AMDRDNA4FB): NOT arming", f);
        return ArmOut { false, kNotArmed, "not armed: fbname failed" };
    }
    ABLOG("fbname 1 -> status %u (%s)", f, f == kFbnameOk ? "renamed" : "already in that state");

    // ---- 4. pipearm 1 (84): the ONE byte, accel+0xccf, read back - and only while the arm still has a purpose ----
    // (a) a HUNG GPU: an armed pipe plus a device WindowServer cannot get is fatal to it.
    if (amdgpu::n1c_hung()) {
        ABLOG("the GPU is HUNG (n1c_hung): NOT arming - an armed pipe behind a declined device is fatal to WindowServer");
        return ArmOut { false, kNotArmed, "not armed: the GPU is HUNG" };
    }
    // (b) a WindowServer session is already open: the gate byte is read when WindowServer CREATES its display-pipe client, so an arm behind a live
    // WindowServer cannot reach the client that already exists - and this path NEVER restarts WindowServer (that is the whole point of it). With the
    // boot hold on this is the second, independent reading of the same fact the claim CAS below answers ("NOT arming with WindowServer up"); with navi48-nohold=1 it
    // is the only one.
    if (amdgpu::n1c_ws_is_open()) {
        ABLOG("a WindowServer session is already open (n1c_ws_is_open): the arm would come too late for its device creation, and this path never "
              "restarts WindowServer - NOT arming. The pipe stays adopted with accel+0xccf = 0, which is exactly today's unarmed behaviour for "
              "WindowServer; a reboot arms before WindowServer starts");
        return ArmOut { false, kNotArmed, "not armed: WindowServer was already up" };
    }
    // (c) THE CLAIM: held -> claimed, right here, before the byte. While the hold is claimed the cap can no longer release it, so WindowServer cannot
    // start in the middle of the arm - and if the cap got there first this FAILS and nothing is armed ("NOT arming with WindowServer
    // up"). This is also the only place the hold is claimed, so hold_finish() has exactly one of its two transitions to run on the way out.
    if (gHoldUsed != 0u && !hold_claim()) {
        ABLOG("the boot hold was already RELEASED (the cap fired first): NOT arming with WindowServer up or on its way. The pipe stays adopted with "
              "accel+0xccf = 0; the next boot with navi48-armboot=1 arms in time");
        return ArmOut { false, kNotArmed, "not armed: the hold was released before the arm (the cap)" };
    }
    st = n48disp_verb((uint32_t)n48disp::kActArm, 1ull, v, 13);
    if (st != (uint32_t)n48disp::kOk) {
        ABLOG("pipearm 1 REFUSED: status %u (%s) - NOT armed; the pipe stays adopted with the gate byte unchanged", st, n48disp::status_name(st));
        return ArmOut { false, kNotArmed, "not armed: pipearm refused" };
    }
    ABLOG("pipearm 1 -> OK; accel+0xccf reads %llu, flags %#llx. THE DISPLAY PIPE IS ARMED BY THE KEXT AT BOOT, uptime %llu ms, with NO WindowServer "
          "restart: WindowServer's first device creation will find the type-4 gate already open (adopted by pipeadopt, AGDC published, fbname armed)",
          (unsigned long long)v[2], (unsigned long long)v[1], (unsigned long long)uptime_ms());
    return ArmOut { true, kArmed, "armed" };
}

// ============================================================================================================================================================
// the worker
// ============================================================================================================================================================
void armboot_worker(void *arg, wait_result_t wr) {
    (void)wr;
    IOService *svc = static_cast<IOService *>(arg);      // the reference n48armboot_boot took; released on every exit
    const uint64_t startNs = now_ns();

    // ---- 0. THE BOOT PUBLISH (0.0.670) ------------------------------------------------------------------------------------------------------------------
    // Before anything else, and before any wait for the accelerator: the accelerator's personality matches the Metal nub, so the nub has to exist first.
    // On the userspace-only path this was a userspace client's `n48nub publish` (a client with a Hello'd session, up to 420 s of polling). The kext now
    // does it itself: the same verdict (n48metal::publish_at_boot_verdict), the same body, the same lock (Navi48MetalNub::bootPublish -> the ONE
    // publish_impl), no session, no userspace verb. navi48-nubboot=0 puts the old, on-demand-only behaviour back.
    if (gNubLatch == kLatchOn) {
        ABLOG("navi48-nubboot=0: the kext does NOT publish the nub on this boot - the nub must be published on demand (`n48nub publish`, "
              "from userspace)");
    } else {
        while (!service_registered()) {
            if (stop_requested()) {
                ABLOG("the kext is stopping: giving up before the boot publish");
                set_state(kStopped, "stopped: the kext was unloading");
                hold_finish(svc, false, "the kext was unloading");
                if (svc) svc->release();
                return;
            }
            if (now_ns() >= deadline_ns) {
                ABLOG("our own service is still not in the registry after %llu ms: NOT publishing the nub from the kext. This is not expected "
                      "(n48armboot_boot runs at the end of start(), and registerService() follows it immediately); the boot continues unchanged",
                      (unsigned long long)(kBudgetNs / 1000000ull));
                set_state(kNotArmed, "not armed: our service never registered");
                hold_finish(svc, false, "our service never registered");
                if (svc) svc->release();
                return;
            }
            IOSleep(20);                                  // 20 ms: registerService() is the very next statement of start()
        }
        Navi48Bringup *owner = OSDynamicCast(Navi48Bringup, svc);
        const IOReturn rc = owner ? Navi48MetalNub::bootPublish(owner) : kIOReturnNotReady;
        if (rc != kIOReturnSuccess) {
            ABLOG("the KEXT's own nub publish was REFUSED (%#x): nothing can arm today (the accelerator needs the nub), so the hold is given back at "
                  "once and WindowServer is not delayed. The gate is the same one the selector uses, minus the session term: navi48-metal=1, a native "
                  "boot whose S1b reported POSITIVE PASS, the GPU not HUNG, no display nub (0.0.652's interlock) and no nub already published", (unsigned)rc);
            set_state(kNotArmed, "not armed: the kext's nub publish was refused");
            hold_finish(svc, false, "the boot publish was refused");
            if (svc) svc->release();
            return;
        }
        ABLOG("THE METAL NUB WAS PUBLISHED BY THE KEXT ITSELF at boot (uptime %llu ms): no userspace publisher, no 420 s daemon poll. The aux "
              "accelerator's personality matches it (IOProviderClass Navi48MetalNub + the Navi48LoadAccelerator gate)",
              (unsigned long long)uptime_ms());
    }

    // ---- 1..4. the arm -----------------------------------------------------------------------------------------------------------------------------------
    const ArmOut r = arm_sequence();
    set_state(r.state, r.why);
    if (r.ok)
        ABLOG("ARMBOOT: the display pipe is armed by the kext %llu ms after the worker started, inside the boot hold",
              (unsigned long long)((now_ns() - startNs) / 1000000ull));
    // THE ONE PLACE the hold can end after the arm sequence: claimed -> none (armed, or PARTIAL after a refusal that followed the claim), or held -> none
    // if the refusal came before the claim. Either way WindowServer may now start, and the busy count is given back exactly once.
    hold_finish(svc, r.ok, r.why);
    if (svc) svc->release();
}

}  // namespace

void n48armboot_boot(void) {
    // ---- the master latch ----
    UInt32 l = gLatch;
    if (l == kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-armboot", &v, sizeof(v));
        if (OSCompareAndSwap(kLatchUnset, (present && v == 1u) ? kLatchOn : kLatchOff, &gLatch)) {
            ABLOG("boot-arg navi48-armboot %s: the boot-time takeover (the kext publishes the nub, holds the boot, then runs pipeadopt -> pipeagdc -> "
                  "fbname -> pipearm, with NO WindowServer restart) is %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                  gLatch == kLatchOn ? "ENABLED" : "OFF");
        }
        l = gLatch;
    }
    if (l != kLatchOn) return;

    // ---- the three sub-switches (0.0.670), latched once like every other switch of this kext ----
    if (gHoldLatch == kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-nohold", &v, sizeof(v));
        if (OSCompareAndSwap(kLatchUnset, (present && v == 1u) ? kLatchOn : kLatchOff, &gHoldLatch))
            ABLOG("boot-arg navi48-nohold %s: the boot hold (adjustBusy(1) until the arm is done) is %s for this boot",
                  present ? (v == 1u ? "=1" : "present but not 1") : "absent", gHoldLatch == kLatchOn ? "OFF" : "ON");
    }
    if (gNubLatch == kLatchUnset) {
        uint32_t v = 1u;
        const bool present = PE_parse_boot_argn("navi48-nubboot", &v, sizeof(v));
        if (OSCompareAndSwap(kLatchUnset, (present && v == 0u) ? kLatchOn : kLatchOff, &gNubLatch))
            ABLOG("boot-arg navi48-nubboot %s: the kext's own boot publish of the Metal nub is %s for this boot",
                  present ? (v == 0u ? "=0" : "present but not 0") : "absent", gNubLatch == kLatchOn ? "OFF (on demand only)" : "ON");
    }
    {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-holdcap", &v, sizeof(v));
        gCapMs = n48metal::hold_cap_ms(present, v);
        if (present) ABLOG("boot-arg navi48-holdcap=%u -> the cap is %u ms (clamped to %u..%u)", v, gCapMs,
                           n48metal::kHoldCapMsMin, n48metal::kHoldCapMsMax);
    }

    // The four steps are the display pipe's own verbs: with the display latch OFF they answer kOff before reading anything, so the whole point of this
    // path (an arm WindowServer can use) is gone. One line and nothing else - this is also the only place the two switches are compared.
    if (!n48disp_latched_on()) {
        ABLOG("navi48-armboot=1 but boot-arg navi48-metal-disp is not 1: the display pipe is OFF for this boot - doing nothing");
        return;
    }

    uint32_t zero = 0u;
    if (!__atomic_compare_exchange_n(&gStarted, &zero, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;   // one worker per boot

    IOService *svc = navi48_bringup_service();

    // ---- THE BOOT HOLD, taken HERE: before registerService() (which start() calls immediately after this function returns). Every way out of
    //      this block leaves either a hold WITH a cap or no hold at all. ----
    if (svc && gHoldLatch != kLatchOn) {
        hold_take(svc);
        if (gHoldUsed != 0u) {
            if (thread_call_t cap = thread_call_allocate(&cap_call, svc)) {
                gCapCall = cap;
                svc->retain();                            // the cap's own reference, released in cap_call (one hold per boot)
                uint64_t dl = 0;
                clock_interval_to_deadline(gCapMs, kMillisecondScale, &dl);
                thread_call_enter_delayed(cap, dl);
            } else {
                // A hold whose cap could not be armed is a boot with no bound: give it straight back.
                ABLOG("thread_call_allocate FAILED: the boot hold is given back immediately (a hold with no cap is a boot with no bound)");
                hold_abandon(svc, "no cap timer");
            }
        }
    } else if (svc) {
        ABLOG("navi48-nohold=1: NO boot hold - WindowServer is NOT delayed, so the arm may come too late for its first device creation (this is the "
              "single-variable lever that isolates the hold's effect, and the emergency lever if the hold ever misbehaves)");
    }

    deadline_ns = now_ns() + kBudgetNs;
    // The state FIRST: the worker may reach its own set_state() before kernel_thread_start returns, and the last writer wins - the "waiting" line must
    // not be able to overwrite the worker's verdict.
    set_state(kWaiting, "waiting for the aux accelerator");
    if (svc) svc->retain();                              // the worker's reference: the kext cannot be unloaded under it (n48armboot_stop is the belt)
    thread_t th = nullptr;
    const kern_return_t kr = kernel_thread_start(&armboot_worker, svc, &th);
    if (kr != KERN_SUCCESS) {
        ABLOG("kernel_thread_start failed (%#x): NOT arming", (unsigned)kr);
        set_state(kNotArmed, "not armed: the worker thread could not be started");
        if (svc) { hold_abandon(svc, "no worker thread"); svc->release(); }
        return;
    }
    thread_deallocate(th);
    ABLOG("ARMBOOT ARMED: the kext will publish the Metal nub and then wait for the Navi48Accelerator under it (up to %llu ms, poll %u ms), inside a "
          "%s boot hold; it arms BEFORE WindowServer's first start", (unsigned long long)(kBudgetNs / 1000000ull), kPollMs,
          gHoldUsed != 0u ? "taken" : "NOT taken");
}

void n48armboot_stop(void) {
    __atomic_store_n(&gStop, 1u, __ATOMIC_RELEASE);
    // 0.0.670: an unloaded kext must never leave a busy count behind, so if the hold is still ours it goes back here too (the worker's own paths do it
    // as well; these CASes are what makes "exactly one adjustBusy(-1)" true whatever order the two run in).
    if (IOService *svc = navi48_bringup_service()) {
        if (OSCompareAndSwap((UInt32)n48metal::kHoldHeld, (UInt32)n48metal::hold_after_cap(), &gHold) ||
            OSCompareAndSwap((UInt32)n48metal::kHoldClaimed, (UInt32)n48metal::hold_after_release(), &gHold)) {
            gHoldUsed = 0u;
            svc->adjustBusy(-1);
            svc->setProperty("Navi48,BootHold", "released: the kext is stopping");
            ABLOG("stop: the boot hold is released (the kext is going away) - WindowServer's IOKitWaitQuiet may return");
        }
    }
    if (__atomic_load_n(&gState, __ATOMIC_ACQUIRE) == (uint32_t)kWaiting)
        ABLOG("stop: the armboot worker was still waiting and has been told to give up");
}
