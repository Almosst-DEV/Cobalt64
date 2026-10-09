//
//  Navi48MetalNub.hpp - the software nub the Aux-KC accelerator kext (Navi48Accel) matches, kext 0.0.610, milestone #9 route A (an internal design note sec. 1).
//
//  Navi48MetalNub : IOService, attached to Navi48Bringup, published ONLY on demand by the native client's selector N48N_SEL_METAL_NUB_PUBLISH (ABI 1.8), and
//  only with boot-arg navi48-metal=1 on a native boot whose S1b reported POSITIVE PASS, the GPU not HUNG and a Hello'd session. NEVER at boot. It matches
//  nothing by itself; the aux kext's personality (IOProviderClass Navi48MetalNub, IOMatchCategory IOAccelerator) is what makes an accelerator appear under it.
//  0.0.612: it carries "Navi48,Ready" (1 while the native path is usable, 0 once the HUNG latch tripped; sticky). It touches no hardware: publishing it writes no register. Withdraw terminates it (the accelerator under it stops and is released).
//
//  The nub also answers callPlatformFunction("n48.metal.ops", waitForFunction=false, &ops, ...) with the versioned ops table of Navi48MetalOps.h: every decision
//  the aux kext makes (IOAccelConfig contents, stamp page, task window, factory mask, memory-map hooks, logging) lives in that table, in this kext, so the aux kext
//  (whose every rebuild requires a security approval (Allow click) from the user) can stay untouched. The pure parts are amd/native_metal_pure.h (host-tested).
//
#ifndef Navi48MetalNub_hpp
#define Navi48MetalNub_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOReturn.h>

class Navi48Bringup;

class Navi48MetalNub : public IOService {
	OSDeclareDefaultStructors(Navi48MetalNub)
	using super = IOService;

public:
	// Selector bodies (the client checked scalar counts / struct sizes EXACTLY). All gating is n48metal::publish_verdict / withdraw_verdict.
	//   publish : in [0] flags (0)  out [0] state (1) [1] nub registry entry ID [2] N48_METAL_ABI [3] sizeof(N48MetalOps)
	//   withdraw: in [0] flags (0)  out [0] previous state
	static IOReturn selectorPublish(Navi48Bringup *owner, uint64_t flags, uint64_t out[4]);
	static IOReturn selectorWithdraw(uint64_t flags, uint64_t out[1]);
	// Navi48Bringup::stop: terminate the nub if it is published (no-op otherwise).
	static void shutdown();
	// 0.0.612: the HUNG latch tripped (native_s1c.cpp hang_announce, the only caller). Sets the sticky boot-wide "not ready" state and writes "Navi48,Ready" = 0 on the published nub
	// (if any); a nub published later carries 0 too. Never writes 1 again on this boot. Safe with any native lock held (lock order: the client lock, then this file's lock).
	static void hungLatched();
	// 0.0.613: the published nub (NOT retained: an identity for the display glue's provider check), or NULL when none is published.
	static IOService *published();
	// 0.0.659 (M6 Stage 1a): write a property on the published nub (the bundle reads "Navi48,M6" and "Navi48,M6Surf" there; native_m6_pure.h). false = no nub is published.
	static bool setPublishedData(const char *key, const void *bytes, uint32_t len);
	static bool setPublishedNumber(const char *key, uint64_t v);
	// 0.0.656 (G6): the accelerator the aux kext registered through device_open (identity only, NOT retained), or NULL while there is no device. The native client's start() accepts it as the accelerator route's provider.
	static IOService *registeredAccelerator();

	IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3, void *param4) override;
	void free() override;
};

#endif /* Navi48MetalNub_hpp */
