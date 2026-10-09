//
//  Navi48DisplayNub.hpp - the software nub the Aux-KC framebuffer (Navi48Framebuffer in com.navi48.accelprobe 0.0.4; 0.0.6 adds the monitor A's personality) matches, kext 0.0.652, milestone M5
//  (an internal design note "M5 build spec: Navi48Framebuffer for the monitor B"; 0.0.658: one nub PER DISPLAY INDEX, "monitor A (instance 1) plane + framebuffer spec at 1080p60" section 4).
//
//  Navi48DisplayNub : IOService, attached to the GPU's IOPCIDevice (NOT to Navi48Bringup: IOAccelDisplayMachine::start finds a framebuffer only two levels below its provider, so
//  PCI -> nub -> framebuffer is found and PCI -> Navi48Bringup -> nub -> framebuffer is not; M6 needs that), with Navi48DisplayIndex = 1 (the monitor B) or 2 (the monitor A, 0.0.658). It is published ON DEMAND by the accel verb
//  `fbpublish 2` / `fbpublish 1` (action 105), NEVER at boot, and there is NO withdraw in M5: a reboot is the only way back. Publishing writes no register: it reads the live plane gates and the monitor B's
//  EDID (the DC_I2C engine, exactly as `accel ddcread`) and creates an IORegistry object.
//
//  The nub answers callPlatformFunction("n48.disp.ops", false, &ops, ...) with the ops table of Navi48DisplayOps.h (ABI 2): the IMMUTABLE snapshot taken at publish, a power hook that only logs
//  and a trace hook. The pure decisions are amd/native_fb_pure.h (host-tested by tests/native_disp_test.cpp).
//
#ifndef Navi48DisplayNub_hpp
#define Navi48DisplayNub_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOReturn.h>

class Navi48Bringup;

class Navi48DisplayNub : public IOService {
	OSDeclareDefaultStructors(Navi48DisplayNub)
	using super = IOService;

public:
	// The verb `fbpublish 2` (the monitor B) / `fbpublish 1` (the monitor A, 0.0.658) (accel action 105): out[0..12] = the n48fb::Status and the published values; returns the status. All gating is n48fb::pre_verdict / live_verdict / data_verdict.
	static uint32_t publishVerb(Navi48Bringup *owner, uint64_t arg, uint64_t *out, unsigned count);
	// Navi48Bringup::stop: terminate the nub if it is published (no-op otherwise).
	static void shutdown();
	// The published nub (NOT retained: an identity), or NULL. 0.0.658: the first one (the monitor B's, else the monitor A's).
	static IOService *published();
	// 0.0.659 (M6 Stage 1a): the Navi48Framebuffer that started on the nub of display index `index` (1 = the monitor B, 2 = the monitor A), RETAINED (the caller releases), or NULL when the nub or its framebuffer does not exist.
	static IOService *framebufferOf(uint32_t index);

	IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3, void *param4) override;
	void free() override;
};

#endif /* Navi48DisplayNub_hpp */
