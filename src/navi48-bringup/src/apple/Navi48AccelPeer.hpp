//
//  Navi48AccelPeer.hpp — the framebuffer-side half of Apple's accelerator protocol.
//
//  AMDRadeonX6000 talks to its display driver through exactly ONE entry point:
//
//      peer->callPlatformFunction(OSSymbol("SpecialAMDKey"), /*waitForFunction=*/true,
//                                 &requestType, param2, param3, param4);
//
//  It finds that peer with GraphicsAccelerator::initLinkToPeer(), which walks the
//  PCI nub's clients and takes the first whose IOMatchCategory property equals
//  "ATIFramebuffer", falling back to "IOFramebuffer".
//
//  On this machine the only framebuffer is RDNA4FB, whose category is the generic
//  "IOFramebuffer". So the accelerator picked it, asked it an AMD-specific
//  question, and hung: callPlatformFunction with waitForFunction = true BLOCKS
//  when nothing in the provider chain handles the symbol. That is what left the
//  VGA node busy forever.
//
//  This class claims "ATIFramebuffer" — which the accelerator prefers — so it is
//  chosen first and RDNA4FB is never asked. No vtable surgery: callPlatformFunction
//  is ordinary public IOKit that we override in a class we own.
//
//  It deliberately does NOT drive any display. RDNA4FB still owns scanout. This
//  object exists only to answer questions.
//
#pragma once
#include <IOKit/IOService.h>

class Navi48Bringup;

// Defuse Apple's error-path panic, callable from anywhere in the kext.
//
// It cannot hang off SpecialAMDKey: measured on hardware, the accelerator never
// asks. `isDeviceValid()` returns early when `this->0x30d & 1` is clear, which
// is before the call we answer, so our peer is simply never consulted on this
// path. Our TTL *is* called, so the patch is triggered from there instead.
void Navi48AccelPeer_TryPatchAcceleratorStop();

class Navi48AccelPeer : public IOService {
    OSDeclareDefaultStructors(Navi48AccelPeer)
    using super = IOService;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool       start(IOService *provider) override;
    void       stop(IOService *provider) override;

    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                  void *param1, void *param2, void *param3,
                                  void *param4) override;

    uint32_t requestCount() const { return mRequests; }

    // See the note on Navi48AccelPeer_TryPatchAcceleratorStop().
    bool tryPatchAcceleratorStop();

    // MILESTONE 3 (0.0.241): stamp our display's framebuffer with the three
    // properties IOKit's IOAccelFindAccelerator actually reads, so the
    // framebuffer-to-accelerator lookup resolves instead of returning
    // 0xe00002bc (run r40). Called ONLY from kReqAccelStarted, which cannot
    // happen until the exposure gate has published LoadAccelerator, so the
    // pairing can never appear on an ungated boot. Writes nothing unless both
    // nodes are found under our own PCI function and both pass an identity
    // check. 0.0.267: OPT-IN — stamped only with the boot-arg
    // navi48-display-pairing=1 or `accel pairing 1` sent before `fire`
    // (pairing_policy.h). Takes the pairing lock.
    bool publishDisplayPairingKeys();

    // 0.0.267: under the pairing lock, for navi48_pairing_control only.
    bool     pairingStamped() const { return mPairingPublished && mPairedFb; }
    uint32_t withdrawDisplayPairingKeysLocked();

private:
    IOReturn handleSpecialAMDKey(uint32_t type, void *p2, void *p3, void *p4);
    bool     publishDisplayPairingKeysLocked();

    // Diagnostic: stop Apple's accelerator panicking on its own error path.
    //
    // When AMDGraphicsAccelerator::start() fails it calls its own stop(), which
    // NULLs the event logs at this+0x1ea0 — and then the epilogue that BOTH
    // outcomes share dereferences one, taking a page fault at
    // start+0x6d3 with CR2 = 0x18. Apple never hits it because start never
    // fails on hardware it supports.
    //
    // That panic destroys the log ring, which is the only record of WHY start
    // failed. So we no-op stop(): start then returns false cleanly, the machine
    // survives, and the trace can be read.
    //
    // This LEAKS whatever start() allocated. It is an instrument, not a fix, and
    // it only ever runs under the accel experiment.


    const OSSymbol *mKey { nullptr };
    uint32_t        mRequests { 0 };
    bool            mStopPatched { false };
    bool            mPairingPublished { false };
    IOService      *mPairedFb { nullptr };      // retained while our keys are on it
    char            mPairedPath[512] { 0 };     // the IOAccelTypes value we wrote
};

// 0.0.368: RULE E1's ring-generation clause. AccelChannel::resetHardwareAndReplay brackets a GPU reset with
// SpecialAMDKey type 0x17, so every bracket we answer is Apple telling us it has decided to reset the hardware
// and replay. On hp2 - the run whose ring E1 as first designed would have wrongly certified - FOUR of these arrived before
// the render drain armed; on hp1, hp3, hp4, hp5, hp7 and hp8, ZERO. Latched per boot, never cleared; read-only for E1.
uint64_t navi48_peer_reset_replay_keys(void);
