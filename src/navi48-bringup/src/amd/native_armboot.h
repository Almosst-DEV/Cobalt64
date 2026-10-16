//
//  native_armboot.h - 0.0.670: THE KEXT PUBLISHES THE NUB AND TAKES THE DISPLAY PIPE OVER AT BOOT, BY ITSELF, INSIDE A BOOT HOLD.
//
//  WHAT THIS REPLACES. The route this file replaces took the display pipe over from USERSPACE, by a deployment script run by a daemon: `pipeadopt -> pipeagdc 1 -> fbname 1 -> (the plugin's headless flag) -> pipearm 1 -> pipereload -> kill WindowServer`. The
//  kill is what makes that route fragile, and it is what this file removes:
//    * the new WindowServer's device request ran into `CoreDisplay_CreateDisplayForCGXDisplayDevice`'s "Failed to create MetalDevice (non-null)" assert,
//    * every WindowServer start is counted by the L2 crash latch (a switch file in a private temporary directory, >= 3 starts in 300 s = the device is refused for the rest of
//      the boot), so a failed restart feeds a self-sustaining crash loop,
//    * `pipereload`'s restart window is 15 s wide, and a WindowServer that takes longer than that loses the race,
//    * and the arm itself is a one-shot action of that script, so whether a boot works depends on the script running at the right moment.
//
//  WHAT THIS FILE DOES (the four display verbs plus the two things that make them reach WindowServer's FIRST start). With boot-arg
//  navi48-armboot=1 (DEFAULT OFF, latched once like every other switch of this kext) `Navi48Bringup::start()` does TWO things, in this order:
//
//    (A) THE BOOT HOLD, taken BEFORE `registerService()` - the busy count is raised first and the service is registered after, so the
//        hold is already in place when IOKit publishes the service: `adjustBusy(1)` on the bring-up IOService. WindowServer's IOKitWaitQuiet blocks until the registry is quiet,
//        and a service with a non-zero busyState is not quiet (IOKit/IOService.h: the busy state propagates to the provider, so the IOPCIDevice is
//        marked busy too). The busy count is released only when the arm is DONE, so WindowServer's first start already finds the gate byte open.
//        That is what makes `pipereload`, `killall -9 WindowServer`, the 15 s window, the L2 latch and the userspace script all unnecessary.
//        THE GUARDS COME WITH IT, because a hold without a bound is a boot without a bound:
//          * a CAP timer (thread_call, navi48-holdcap ms, default 40 s - there is no BAR settle to add) that releases the busy count
//            AND refuses the arm from then on (n48metal::hold_cap_ok): a hold that outlived its cap must never arm behind a live WindowServer;
//          * the claim CAS: the worker must take the hold from `held` to `claimed` BEFORE it arms. If the cap got there first the claim fails and the
//            bring-up does NOT arm with WindowServer up;
//          * every wait is bounded, and EVERY failure path releases the hold and returns: no path here can fail the kext load or hold up the boot;
//          * if the cap timer cannot be allocated the hold is NOT taken at all (a hold that could never be released is worse than no hold).
//
//    (B) ONE kernel thread that first PUBLISHES THE METAL NUB (0.0.670: `Navi48MetalNub::bootPublish`, in-kernel, no userspace verb) and then runs
//        THE SAME FOUR STEPS, IN THE SAME ORDER, that the userspace recipe runs:
//            1. `pipeadopt`  (accel action 83, n48disp::kActAdopt)   the facts, requestProbe(1), the verified pipe, the capabilities property
//            2. `pipeagdc 1` (accel action 88, navi48_agdc_native_control) the native AGDC service on a vtable copy
//            3. `fbname 1`   (accel action 78, n48fbname::control)   RDNA4FB's metaclass name -> AMDRDNA4FB, so CoreDisplay resolves the vendor
//            4. `pipearm 1`  (accel action 84, n48disp::kActArm)     ONE byte, accel+0xccf, read back - the type-4 user-client gate
//        It NEVER opens the restart window (`pipereload`, action 90) and NEVER kills, restarts or touches WindowServer: the whole point is
//        that the gate byte is already 1 when WindowServer's FIRST device creation reads it.
//
//  WHY THE NUB HAD TO MOVE INTO THE KEXT (0.0.670). The nub's only publisher was the native client's selector, i.e. a USERSPACE daemon
//  (that daemon polls `n48nub publish` up to 420 s). The accelerator's personality matches that nub (IOProviderClass Navi48MetalNub), so
//  with no nub at boot there is no accelerator at boot - and the accelerator is what the Metal plugin's device is built on. The kext now publishes
//  it itself, gated by the SAME verdict minus the session term (there is no session at boot): n48metal::publish_at_boot_verdict. `navi48-nubboot=0`
//  puts the userspace-only behaviour back, one boot-arg away.
//
//  WHY A THREAD AND NOT A CALL IN start(). The order is fixed: the Metal nub is published first, and the accelerator (`aux trace: event 10`)
//  and its display machine (`event 14`) exist only AFTER that publish; WindowServer's first start follows the accelerator by seconds. A
//  blocking wait inside `start()` was
//  rejected: `start()` runs on the IOKit matching thread, and a bounded-but-long sleep there would hold up every other match of this device. The
//  worker owes nothing to that thread - and with the hold taken, WindowServer owes the worker its first start.
//
//  FAILURE IS ALWAYS SAFE, and every wait is BOUNDED:
//    * the ONLY waits are (i) the bounded wait for our own service to be in the registry before the boot publish, (ii) the adopt retry loop, and
//      (iii) the IOSleep between attempts: a total budget (kBudgetNs, 35 s - INSIDE the 40 s cap on purpose) and no lock of ours held across any of
//      them (`n48disp_verb` takes and releases its own per-call busy CAS; the loop holds nothing);
//    * EVERY step's return code is checked; the first failure STOPS the sequence (the userspace recipe's own rule) - nothing is disarmed, torn down or
//      rolled back, because none of the four steps is destructive and a later boot can simply try again;
//    * the arm is SKIPPED (with a named reason) when it would come too late or be harmful: a WindowServer session is already open (this path never
//      restarts WindowServer, so arming behind a live WindowServer would leave the half-state the restart exists to fix) or the GPU is HUNG
//      (an armed pipe plus a declined device is fatal to WindowServer) - and whenever the hold was released by the cap
//      before the claim (the CAS, i.e. "WindowServer is up or on its way");
//    * nothing here can fail the kext load: `n48armboot_boot()` returns void, and every failure is a log line plus an IORegistry property. The
//      `Navi48,ArmBoot` and `Navi48,BootHold` properties carry the verdict to a boot that can be read back without a log daemon.
//
//  WHAT IT DOES NOT DO (deliberately): set the Metal plugin's non-headless flag (a switch file read by the bundle in userspace, NOT
//  by this kext - the plugin's `isHeadless` decides whether CoreDisplay builds a display device at all, and WindowServer reads it once per process),
//  or replace anything else of the deployment. The userspace script and its daemon are left in place: this path makes them OPTIONAL, it does not
//  remove them. NOTE: a userspace daemon of the deployment runs `pipereload` + `kickstart -k WindowServer`, so it must be disabled
//  (or reduced to creating that switch file) once this path is on, or the restart comes back.
//
//  Default OFF: with navi48-armboot absent this file's only effect is ONE log line (the latch's own, the same shape every other latch of this kext
//  prints), and no hold, no thread, no property and no verb.
//
#pragma once

// Called at the end of Navi48Bringup::start(), BEFORE registerService() (the boot hold is taken here - see the file comment). No-op unless
// boot-arg navi48-armboot=1 (and the display latch navi48-metal-disp=1 is on).
void n48armboot_boot(void);

// Called from Navi48Bringup::stop(): tells the worker to give up at its next check and gives the boot hold back (an unloaded kext must never leave a
// busy count behind). The worker also holds a reference to the service, so the kext cannot be unloaded under it; this is the belt to that braces.
void n48armboot_stop(void);
