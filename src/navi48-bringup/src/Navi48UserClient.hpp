//
//  Navi48UserClient.hpp — userspace access to the bring-up driver.
//
//  Phase 3 consolidation, Stage A6. Until now every experiment was a new kext
//  binary, an approval click and a reboot. This exposes the pieces the ladder
//  already proved — VRAM allocation, CPU access to VRAM through BAR0, indirect
//  buffer submission on the MES-mapped GFX ring, fences — so a userspace tool
//  can run thousands of variations against one loaded binary.
//
//  It is NOT an accelerator interface. Apple's graphics stack reaches a GPU
//  through IOAcceleratorFamily2's user clients with a private, versioned C++
//  ABI (see notes/re/PHASE2-MAP.md); this is a plain IOUserClient with our own
//  selectors, for our own test harness. Phase 4 is where the Apple-shaped
//  interface gets built.
//
//  Safety: every selector validates its arguments against the actual
//  allocation table and the ring geometry before touching the GPU. Submitted
//  indirect buffers are the caller's own PM4, so a malformed packet can hang
//  the ring — that is the point of having the harness — but it cannot reach
//  memory the GPU has no page-table entry for.
//
#ifndef Navi48UserClient_hpp
#define Navi48UserClient_hpp

#include <IOKit/IOUserClient.h>
#include "Navi48UserClientABI.h"
#include "amd/amdgpu_vram.h"

class Navi48Bringup;

class Navi48UserClient : public IOUserClient {
	OSDeclareDefaultStructors(Navi48UserClient)
	using super = IOUserClient;

public:
	bool     initWithTask(task_t owningTask, void *securityID, UInt32 type,
	                      OSDictionary *properties) override;
	bool     start(IOService *provider) override;
	void     stop(IOService *provider) override;
	IOReturn clientClose() override;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	                        IOExternalMethodDispatch *dispatch, OSObject *target,
	                        void *reference) override;
	// 0.0.287: memory type 0x46425343 ('FBSC') maps RDNA4FB's scanout range through a fresh device descriptor,
	// 0x46425652 ('FBVR') through RDNA4FB's own getVRAMRange; the caller's IOConnectMapMemory64 options apply unchanged,
	// exactly as for CoreDisplay's kIOFBVRAMMemory mapping. Root only (the user client is root-only already).
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
	// Allocation table. A handle is an index into it; 0 is never valid, so a
	// zeroed handle from userspace is rejected rather than freeing slot 0.
	static constexpr uint32_t kMaxAllocs = 64;
	struct Slot {
		bool                   inUse;
		bool                   hi;     // came from the device-only pool above BAR0
		amdgpu::VRAMAllocation a;
	};
	Slot slots[kMaxAllocs] {};

	IOReturn doGetInfo(IOExternalMethodArguments *args);
	IOReturn doAllocVRAM(IOExternalMethodArguments *args);
	IOReturn doFreeVRAM(IOExternalMethodArguments *args);
	IOReturn doWriteVRAM(IOExternalMethodArguments *args);
	IOReturn doReadVRAM(IOExternalMethodArguments *args);
	IOReturn doSubmitIB(IOExternalMethodArguments *args);
	IOReturn doWaitFence(IOExternalMethodArguments *args);
	IOReturn doRegRead(IOExternalMethodArguments *args);
	IOReturn doSelfTest(IOExternalMethodArguments *args);
	IOReturn doGetCounters(IOExternalMethodArguments *args);
	IOReturn doReadLog(IOExternalMethodArguments *args);
	IOReturn doLogReset(IOExternalMethodArguments *args);
	IOReturn doLogConsume(IOExternalMethodArguments *args);   // 0.0.276
	IOReturn doReadCap(IOExternalMethodArguments *args);       // 0.0.282
	IOReturn doCapConsume(IOExternalMethodArguments *args);    // 0.0.282
	IOReturn doReadScanFull(IOExternalMethodArguments *args);  // build 0.0.542 (`accel scanout full`'s capture)
	IOReturn doMetrics(IOExternalMethodArguments *args);
	IOReturn doPowerState(IOExternalMethodArguments *args);
	IOReturn doAccelExperiment(IOExternalMethodArguments *args);

	// True if `gpu_va .. gpu_va+len` lies inside a live allocation of ours.
	bool rangeIsOurs(uint64_t gpu_va, uint64_t len) const;

	Navi48Bringup *owner { nullptr };
	task_t         task  { nullptr };
	bool           opened { false };
};

#endif /* Navi48UserClient_hpp */
