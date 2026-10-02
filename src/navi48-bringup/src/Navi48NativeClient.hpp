//
//  Navi48NativeClient.hpp - the native user client (type 'N48N', kext 0.0.601, native step S1c).
//
//  0.0.612: the C++ class is now IOAccelNavi48NativeClient (this file and Navi48NativeClient.cpp keep their names). WindowServer's sandbox admits iokit-open only for user-client classes
//  whose name starts with "IOAccel" (measured on the PC's 25G83 profile); the service (Navi48Bringup) and the type ('N48N') are unchanged, so RADV opens it exactly as before.
//
//  A separate IOUserClient class from the legacy Navi48UserClient (type 0, unchanged): its own selector numbering 0..8 (ABI 1.0) plus 9..14, the native scanout (ABI 1.1, 0.0.603), its own ABI
//  header (Navi48NativeABI.h, shared byte for byte with the Mesa Darwin backend), one client per boot at a time, and a gate that
//  refuses to open unless this boot's S1b self-test reported POSITIVE PASS. All state and every rule live in amd/native_s1c.{h,cpp};
//  this class only checks the shape of each call (scalar counts and struct sizes EXACTLY, every struct inline) and forwards it.
//
#ifndef Navi48NativeClient_hpp
#define Navi48NativeClient_hpp

#include <IOKit/IOUserClient.h>
#include "Navi48NativeABI.h"

class Navi48Bringup;

class IOAccelNavi48NativeClient : public IOUserClient {
	OSDeclareDefaultStructors(IOAccelNavi48NativeClient)
	using super = IOUserClient;

public:
	// Create, privilege-check, gate and open in one step. Returns kIOReturnSuccess with *handler set, or the error IOServiceOpen must
	// see: NotPrivileged (non-root), NotReady (gate / S1b / HUNG), ExclusiveAccess (a client is open), NoMemory.
	// 0.0.612: latch the boot-arg navi48-metal-ws once (Navi48Bringup::runStages calls it at start; initWithTask latches it itself if that never ran). First writer wins; never re-read.
	static void latchBootArgs();

	static IOReturn create(Navi48Bringup *owner, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
	                       IOUserClient **handler);

	bool     initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) override;
	bool     start(IOService *provider) override;
	void     stop(IOService *provider) override;
	IOReturn clientClose() override;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
	                        void *reference) override;
	// memoryType = the BO handle (contract 3.4).
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;

private:
	Navi48Bringup *owner  { nullptr };
	task_t         task   { nullptr };
	bool           opened { false };   // this object owns the (single) native session
	bool           privileged { false };
	bool           adminClient { false };   // 0.0.612: admitted as an administrator (root). false = admitted by the uid-88 rule: it reaches only the selectors of n48native::policy::selector_allowed
};

#endif /* Navi48NativeClient_hpp */
