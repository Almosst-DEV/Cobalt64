//
//  Navi48NativeClient.cpp - see the header. The contract is notes/design/NATIVE-S1C-ABI.md (ABI 1.1 addendum: selectors 9..14, 0.0.603; ABI 1.9: selector 21, the class rename and the uid-88 policy, 0.0.612).
//
#include "Navi48NativeClient.hpp"
#include "Navi48Bringup.hpp"
#include "amd/amdgpu_init.h"
#include "amd/native_s1c.h"
#include "amd/native_open_policy_pure.h"   // 0.0.612: the open decision (uid 88 behind navi48-metal-ws=1)
#include "amd/amdgpu_log.h"
#include "dcn/navi48_dcn.hpp"
#include "Navi48MetalNub.hpp"   // 0.0.610 (ABI 1.8): selectors 19 / 20
#include "amd/native_disp.h"     // 0.0.617 (K1): n48disp_on_ws_client_closed

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>   // PE_parse_boot_argn (navi48-metal-ws)
#include <sys/kauth.h>        // kauth_cred_get / kauth_cred_getuid: the opening thread's credential
#include <kern/task.h>         // current_task (selector 21: only a thread of the owning task may import)

#define NCLOG(fmt, ...) AMDGPU_LOG("native-s1c", fmt, ##__VA_ARGS__)

OSDefineMetaClassAndStructors(IOAccelNavi48NativeClient, IOUserClient)

IOReturn IOAccelNavi48NativeClient::create(Navi48Bringup *owner, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) {
	if (!owner || !handler) return kIOReturnBadArgument;
	auto *uc = OSTypeAlloc(IOAccelNavi48NativeClient);
	if (!uc) return kIOReturnNoMemory;
	if (!uc->initWithTask(owningTask, securityID, type, properties)) {
		uc->release();
		return kIOReturnNotPrivileged;   // non-root (the contract maps it to -EACCES)
	}
	uc->owner = owner;
	// The gate, exclusivity and the fresh tree. Nothing has been attached yet, so a refusal is a plain release.
	const IOReturn rc = amdgpu::n1c_open(*owner->bringupContext());
	if (rc != kIOReturnSuccess) { uc->release(); return rc; }
	uc->opened = true;
	if (!uc->attach(owner)) { amdgpu::n1c_close("attach failed"); uc->opened = false; uc->release(); return kIOReturnInternalError; }
	if (!uc->start(owner))  { uc->detach(owner); amdgpu::n1c_close("start failed"); uc->opened = false; uc->release(); return kIOReturnInternalError; }
	*handler = uc;
	return kIOReturnSuccess;
}

// 0.0.612: the boot-arg navi48-metal-ws, latched once (0 unset, 1 off, 2 on; n48native::policy::kLatch*). Default OFF: with it absent the open policy below is exactly the old one.
static volatile UInt32 gMetalWsLatch = n48native::policy::kLatchUnset;
void IOAccelNavi48NativeClient::latchBootArgs() {
	uint32_t v = 0;
	const bool present = PE_parse_boot_argn("navi48-metal-ws", &v, sizeof(v));
	(void)OSCompareAndSwap(n48native::policy::kLatchUnset, n48native::policy::latch_value(present, v), &gMetalWsLatch);   // the first writer wins; never re-read
}

bool IOAccelNavi48NativeClient::initWithTask(task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties) {
	if (!super::initWithTask(owningTask, securityID, type, properties)) return false;
	// 0.0.612: root (administrator) as always; ALSO uid 88 (_windowserver) only when boot-arg navi48-metal-ws=1 was latched and no client is open. The decision is
	// n48native::policy::open_decision (pure, host-tested): with the boot-arg absent it is "admit iff administrator", the 0.0.611 policy. IOServiceOpen runs on the caller's
	// thread, so kauth_cred_get() is the opener's credential (its effective uid).
	privileged = false;
	if (gMetalWsLatch == n48native::policy::kLatchUnset) latchBootArgs();
	const bool admin = clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess;
	const uint32_t uid = (uint32_t)kauth_cred_getuid(kauth_cred_get());
	const n48native::policy::OpenDecision d = n48native::policy::open_decision(admin, uid, n48native::policy::latch_is_on(gMetalWsLatch), amdgpu::n1c_is_open());
	if (!d.admit) {
		NCLOG("open refused: uid %u, reason: %s", uid, n48native::policy::open_reason_text(d.reason));
		return false;
	}
	NCLOG("open admitted: uid %u, reason: %s", uid, n48native::policy::open_reason_text(d.reason));
	privileged = true;
	adminClient = d.reason == n48native::policy::kReasonAdmin;   // 0.0.612: the uid-88 path is NOT an administrator (selector_allowed limits it)
	task = owningTask;
	return true;
}

bool IOAccelNavi48NativeClient::start(IOService *provider) {
	if (!OSDynamicCast(Navi48Bringup, provider)) return false;
	return super::start(provider);
}

// Cleanup is idempotent (n1c_close), and runs from every path the contract names: clientClose, clientDied (which calls clientClose) and stop.
void IOAccelNavi48NativeClient::stop(IOService *provider) {
	if (opened) { n48disp_on_ws_client_closed(adminClient); amdgpu::n1c_close("stop"); opened = false; }   // 0.0.617 (K1)
	super::stop(provider);
}

IOReturn IOAccelNavi48NativeClient::clientClose() {
	if (opened) { n48disp_on_ws_client_closed(adminClient); amdgpu::n1c_close("clientClose"); opened = false; terminate(); }   // 0.0.617 (K1): the uid-88 client's close disarms an armed display pipe FIRST, with no lock held
	return kIOReturnSuccess;
}

IOReturn IOAccelNavi48NativeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
	if (!memory) return kIOReturnBadArgument;
	*memory = nullptr;
	if (!opened) return kIOReturnNotReady;
	return amdgpu::n1c_memory_for_handle((uint32_t)type, options, memory);
}

// Every selector's shape is checked here, exactly: scalar counts, struct input/output sizes, and that nothing arrives out of line.
IOReturn IOAccelNavi48NativeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                                            OSObject *target, void *reference) {
	(void)dispatch; (void)target; (void)reference;
	if (!args || !opened) return kIOReturnNotReady;
	if (args->structureInputDescriptor != nullptr || args->structureOutputDescriptor != nullptr) return kIOReturnBadArgument;
	const uint32_t sic = args->scalarInputCount, soc = args->scalarOutputCount, sis = args->structureInputSize, sos = args->structureOutputSize;
	const uint64_t *si = args->scalarInput;
	uint64_t *so = args->scalarOutput;
	auto shape = [&](uint32_t wantSic, uint32_t wantSoc, uint32_t wantSis, uint32_t wantSos) {
		return sic == wantSic && soc == wantSoc && sis == wantSis && sos == wantSos &&
		       (wantSis == 0 || args->structureInput != nullptr) && (wantSos == 0 || args->structureOutput != nullptr);
	};
	// 0.0.612 (review item E): a client admitted by the uid-88 rule reaches only selectors 0..14 and 21. An administrator client reaches every selector, as before.
	if (!n48native::policy::selector_allowed(adminClient, selector)) {
		static volatile UInt32 gSelRefusedLogged = 0;
		if (gSelRefusedLogged < 16u && OSIncrementAtomic((volatile SInt32 *)&gSelRefusedLogged) < 16) NCLOG("selector %u refused: not permitted for a uid-88 (non-administrator) client", selector);
		return kIOReturnNotPrivileged;
	}
	switch (selector) {
	case N48N_SEL_HELLO:
		if (!shape(2, 4, 0, 0)) return kIOReturnBadArgument;
		return amdgpu::n1c_hello(si[0], si[1], so);
	case N48N_SEL_QUERYINFO:
		if (!shape(0, 0, 0, sizeof(n48n_info))) return kIOReturnBadArgument;
		return amdgpu::n1c_query_info(static_cast<n48n_info *>(args->structureOutput));
	case N48N_SEL_READREGS: {
		if (sic != 3 || soc != 0 || sis != 0) return kIOReturnBadArgument;
		if (si[1] < 1 || si[1] > 16 || sos != (uint32_t)si[1] * 4u || args->structureOutput == nullptr) return kIOReturnBadArgument;
		return amdgpu::n1c_read_regs(si[0], si[1], si[2], static_cast<uint32_t *>(args->structureOutput));
	}
	case N48N_SEL_BOCREATE:
		if (!shape(0, 4, sizeof(n48n_gem_create_in), 0)) return kIOReturnBadArgument;
		return amdgpu::n1c_bo_create(static_cast<const n48n_gem_create_in *>(args->structureInput), so);
	case N48N_SEL_BOFREE:
		if (!shape(1, 0, 0, 0)) return kIOReturnBadArgument;
		return amdgpu::n1c_bo_free(si[0]);
	case N48N_SEL_GEMVA:
		if (!shape(0, 0, sizeof(n48n_gem_va), 0)) return kIOReturnBadArgument;
		return amdgpu::n1c_gem_va(static_cast<const n48n_gem_va *>(args->structureInput));
	case N48N_SEL_CTX:
		if (!shape(0, 0, sizeof(n48n_ctx), sizeof(n48n_ctx))) return kIOReturnBadArgument;
		return amdgpu::n1c_ctx(static_cast<const n48n_ctx *>(args->structureInput), static_cast<n48n_ctx *>(args->structureOutput));
	case N48N_SEL_SUBMIT: {
		// 32 + 32*n bytes: the size is checked against num_ibs inside the parser; here only the shape and the ceiling.
		if (sic != 0 || soc != 1 || sos != 0 || sis < 32u + 32u || sis > 32u + 32u * N48N_MAX_IBS || args->structureInput == nullptr)
			return kIOReturnBadArgument;
		return amdgpu::n1c_submit(static_cast<const uint8_t *>(args->structureInput), sis, so);
	}
	case N48N_SEL_WAITSEQ:
		if (!shape(3, 3, 0, 0)) return kIOReturnBadArgument;
		return amdgpu::n1c_wait(si[0], si[1], so);
	// ---- ABI 1.1 (0.0.603): the scanout selectors. bind() arms the display layer (idempotent; it writes no register) - on a native boot no
	// accel verb ever does, so the first scanout call is what binds it.
	case N48N_SEL_SCAN_QUERY:
		if (!shape(0, 0, 0, sizeof(n48n_scan_query))) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_scan_query(static_cast<n48n_scan_query *>(args->structureOutput));
	case N48N_SEL_SCAN_ACQUIRE:
		if (!shape(1, 2, 0, 0)) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_scan_acquire(si[0], so);
	case N48N_SEL_SCAN_REGISTER:
		if (!shape(0, 2, sizeof(n48n_scan_reg), 0)) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_scan_register(static_cast<const n48n_scan_reg *>(args->structureInput), so);
	case N48N_SEL_SCAN_PRESENT:
		if (!shape(2, 3, 0, 0)) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_scan_present(si[0], si[1], so);
	case N48N_SEL_SCAN_STATUS:
		if (!shape(0, 0, 0, sizeof(n48n_scan_status))) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_scan_status(static_cast<n48n_scan_status *>(args->structureOutput));
	case N48N_SEL_SCAN_RELEASE:
		if (!shape(0, 2, 0, 0)) return kIOReturnBadArgument;
		return amdgpu::n1c_scan_release(so);
	// ---- ABI 1.2 (0.0.604): the DISPCLK/DPPCLK experiment. bind() arms the display layer (idempotent; no register written): the step reads the OTG frame
	// counter through the scanout status code.
	case N48N_SEL_DAL_STEP:
		if (!shape(2, 0, 0, sizeof(n48n_dal_result))) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_dal_step(si[0], si[1], static_cast<n48n_dal_result *>(args->structureOutput));
	// ---- ABI 1.3 (0.0.605): the timed mode trial. bind() arms the display layer (idempotent; on a native boot it also takes the golden register copy, read-only).
	case N48N_SEL_MODE_TRIAL:
		if (!shape(3, 0, 0, sizeof(n48n_mode_result))) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_mode_trial(si[0], si[1], si[2], static_cast<n48n_mode_result *>(args->structureOutput));
	// ---- ABI 1.7 (0.0.609): the HELD mode of row 120. bind() as for the trial.
	case N48N_SEL_MODE_HOLD:
		if (!shape(2, 0, 0, sizeof(n48n_mode_result))) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_mode_hold(si[0], si[1], static_cast<n48n_mode_result *>(args->structureOutput));
	case N48N_SEL_MODE_RELEASE:
		if (!shape(1, 0, 0, sizeof(n48n_mode_result))) return kIOReturnBadArgument;
		(void)n48dcn::bind(owner);
		return amdgpu::n1c_mode_release(si[0], static_cast<n48n_mode_result *>(args->structureOutput));
	// ---- ABI 1.8 (0.0.610): the Metal nub (milestone #9, route A). Publish / withdraw a software IOService under Navi48Bringup for the aux accelerator kext to match.
	// No register is written; every gate is n48metal::publish_verdict (amd/native_metal_pure.h).
	case N48N_SEL_METAL_NUB_PUBLISH:
		if (!shape(1, 4, 0, 0)) return kIOReturnBadArgument;
		return Navi48MetalNub::selectorPublish(owner, si[0], so);
	case N48N_SEL_METAL_NUB_WITHDRAW:
		if (!shape(1, 1, 0, 0)) return kIOReturnBadArgument;
		return Navi48MetalNub::selectorWithdraw(si[0], so);
	// ---- ABI 1.9 (0.0.612): host-memory import (milestone #11 step 11c). The task is the one this client was opened with (the caller's own address space).
	case N48N_SEL_BO_IMPORT_HOST:
		if (!shape(4, 4, 0, 0)) return kIOReturnBadArgument;
		// 0.0.612 (review item B): only a thread of the task this client was opened with may import: the range is read from THAT task's address space.
		if (!n48native::policy::import_caller_ok(current_task(), task)) {
			static volatile UInt32 gImportCallerLogged = 0;
			if (OSCompareAndSwap(0, 1, &gImportCallerLogged)) NCLOG("import refused: the caller is not the task that opened this client (logged once)");
			return kIOReturnNotPermitted;
		}
		return amdgpu::n1c_bo_import_host(task, si[0], si[1], si[2], si[3], so);
	default:
		break;
	}
	return kIOReturnBadArgument;   // an unknown selector on the native client: nothing falls through to IOUserClient's dispatch
}
