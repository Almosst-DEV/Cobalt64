//
//  Navi48MetalNub.cpp - see the header. Pure logic: amd/native_metal_pure.h (tests/native_metal_test.cpp).
//
//  What this file WRITES: nothing on the hardware. It allocates one 4 KiB physically-contiguous system-memory page (the stamp page, on the aux kext's
//  device_open hook), one nub object and its property dictionary. It registers the nub in the IORegistry under Navi48Bringup.
//
#include "Navi48MetalNub.hpp"
#include "Navi48Bringup.hpp"
#include "Navi48MetalOps.h"
#include "amd/native_metal_pure.h"
#include "amd/native_s1b.h"
#include "amd/native_s1c.h"
#include "amd/native_disp.h"         // 0.0.613: the display pipe glue (behind boot-arg navi48-metal-disp=1)
#include "amd/native_disp_pure.h"
#include "amd/amdgpu_log.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

#define MNLOG(fmt, ...) AMDGPU_LOG("metal", fmt, ##__VA_ARGS__)

static_assert(n48metal::kNotReady == (uint32_t)kIOReturnNotReady && n48metal::kBadArg == (uint32_t)kIOReturnBadArgument && n48metal::kNotFound == (uint32_t)kIOReturnNotFound &&
              n48metal::kUnsupported == (uint32_t)kIOReturnUnsupported && n48metal::kExclusive == (uint32_t)kIOReturnExclusiveAccess &&
              n48metal::kBusy == (uint32_t)kIOReturnBusy && n48metal::kNoMemory == (uint32_t)kIOReturnNoMemory && n48metal::kOk == (uint32_t)kIOReturnSuccess,
              "the pure header's codes are the IOReturn.h values");

OSDefineMetaClassAndStructors(Navi48MetalNub, IOService)

// 0.0.613: the mask the aux kext's factories see (op_factory_mask below, and the display glue's fact check): the boot-arg navi48-metal-fact mask, read on every call as before, plus the
// fact bits `pipe adopt` turned on at run time. With boot-arg navi48-metal-disp absent the OR is a no-op (n48disp::fact_mask returns the base mask): the 0.0.612 value, bit for bit.
uint32_t n48metal_factory_mask_now(void) {
	uint32_t v = 0;
	const bool present = PE_parse_boot_argn("navi48-metal-fact", &v, sizeof(v));
	return n48disp::fact_mask(n48disp_latched_on(), n48metal::factory_mask(present, v), n48disp_fact_bits());
}

// ---- state (one nub per boot at a time) -----------------------------------------------------------------------------------------------------------------
namespace {
IOLock                *gLock;         // created on first publish
Navi48MetalNub        *gNub;          // our reference while Published
volatile uint32_t      gState = 0;    // n48metal::State
volatile uint32_t      gReadyNo = 0;  // 0.0.612: sticky, 1 once the HUNG latch tripped this boot (Navi48,Ready is 0 from then on, never 1 again)
struct Dev {                          // the per-device state the aux kext's device_open hook gets
	uint32_t                    magic;
	IOBufferMemoryDescriptor   *md;
	volatile uint32_t          *va;
};
constexpr uint32_t kDevMagic = 0x4D44564Eu;   // 'NVDM'
Dev *gDev;                            // one accelerator per boot

const char kAccelName[] = "Navi48 Accelerator";   // IOAccelConfig +0: a pointer into THIS kext, which outlives the aux kext's accelerator

// ---- the ops table's hooks (Navi48MetalOps.h). The aux kext calls these; each decision is n48metal:: pure code. ---------------------------------------------
void *op_device_open(void *accel, void *nub) {
	(void)accel;
	if (!nub || gDev) return nullptr;                                            // one device
	const mach_vm_address_t mask = n48metal::kStampPhysMask;
	IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
		kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, n48metal::kStampBytes, mask);
	if (!md) { MNLOG("device_open: stamp page allocation failed"); return nullptr; }
	if (md->prepare() != kIOReturnSuccess) { md->release(); return nullptr; }
	IOByteCount seg = 0;
	const addr64_t phys = md->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
	void *cpu = md->getBytesNoCopy();
	if (!cpu || !n48metal::stamp_page_ok(phys, seg, md->getLength())) {
		MNLOG("device_open: stamp page unusable (phys %#llx seg %llu len %llu)", (unsigned long long)phys, (unsigned long long)seg, (unsigned long long)md->getLength());
		md->complete(); md->release(); return nullptr;
	}
	bzero(cpu, n48metal::kStampBytes);                                             // stamps stay 0: nothing is ever submitted at #9
	Dev *d = (Dev *)IOMalloc(sizeof(Dev));
	if (!d) { md->complete(); md->release(); return nullptr; }
	d->magic = kDevMagic; d->md = md; d->va = (volatile uint32_t *)cpu;
	gDev = d;
	MNLOG("device_open: stamp page phys %#llx kva %p", (unsigned long long)phys, cpu);
	return d;
}
void op_device_close(void *ctx) {
	Dev *d = (Dev *)ctx;
	if (!d || d != gDev || d->magic != kDevMagic) return;                          // idempotent: only the live device, once
	gDev = nullptr;
	d->magic = 0;
	if (d->md) { d->md->complete(); d->md->release(); d->md = nullptr; }
	IOFree(d, sizeof(Dev));
	MNLOG("device_close");
}
int op_populate_config(uint8_t *cfg, uint32_t bytes) {
	const int rc = n48metal::config_populate(cfg, bytes, (uint64_t)(uintptr_t)kAccelName);
	if (rc != 0) { MNLOG("populate_config: REFUSED rc %d (a limit the family default left at zero would cap every IOSurface for the boot)", rc); return rc; }
	// (0.0.611: config_populate fills a local copy and copies it into cfg only when the static check passed, so a refusal leaves the family defaults in cfg.)
	// The whole 0x90 bytes and the values IOSurfaceRoot::updateLimits will take as system-wide minimums (NATIVE-S3 review SHOULD-FIX).
	MNLOG("populate_config: ok; IOSurface limits +0x30 %llu +0x32 %llu +0x4c %llu +0x50 %llu +0x54 %llu, +0x47 (type-4 pipe) %u",
	      (unsigned long long)n48metal::ld_le(cfg + n48metal::kOffLim30, 2), (unsigned long long)n48metal::ld_le(cfg + n48metal::kOffLim32, 2),
	      (unsigned long long)n48metal::ld_le(cfg + n48metal::kOffLim4c, 4), (unsigned long long)n48metal::ld_le(cfg + n48metal::kOffLim50, 4),
	      (unsigned long long)n48metal::ld_le(cfg + n48metal::kOffLim54, 4), (unsigned)cfg[n48metal::kOffPipeUC]);
	for (uint32_t o = 0; o < n48metal::kCfgBytes; o += 0x30)
		MNLOG("populate_config: cfg[%#x..] %016llx %016llx %016llx %016llx %016llx %016llx", o,
		      (unsigned long long)n48metal::ld_le(cfg + o, 8), (unsigned long long)n48metal::ld_le(cfg + o + 8, 8), (unsigned long long)n48metal::ld_le(cfg + o + 16, 8),
		      (unsigned long long)n48metal::ld_le(cfg + o + 24, 8), (unsigned long long)n48metal::ld_le(cfg + o + 32, 8), (unsigned long long)n48metal::ld_le(cfg + o + 40, 8));
	return rc;
}
void *op_stamp_memory(void *ctx, uint32_t *n) {
	Dev *d = (Dev *)ctx;
	if (n) *n = 0;
	return (d && d == gDev && d->magic == kDevMagic) ? (void *)d->md : nullptr;
}
volatile uint32_t *op_stamp_va(void *ctx) {
	Dev *d = (Dev *)ctx;
	return (d && d == gDev && d->magic == kDevMagic) ? d->va : nullptr;
}
int op_task_window(void *ctx, uint32_t kind, uint64_t *size, uint64_t *reserve) {
	(void)ctx;
	return n48metal::task_window(kind, size, reserve);
}
uint32_t op_factory_mask(void *ctx) {
	(void)ctx;
	return n48metal_factory_mask_now();
}
int op_mm_hook(void *ctx, uint32_t op, void *obj) {
	(void)ctx; (void)op; (void)obj;
	return (int)kIOReturnUnsupported;                                              // no map / unmap entry point exists before #10: the aux memory map returns false
}
void op_trace(uint32_t event, uint64_t a, uint64_t b) {
	MNLOG("aux trace: event %u a %#llx b %#llx", event, (unsigned long long)a, (unsigned long long)b);
	n48disp_on_trace(event, a, b);                                                 // 0.0.613: the display pipe's trace events (a no-op unless boot-arg navi48-metal-disp=1)
}

// The generic virtual hook (0.0.611): the optional VidMemory / Resource output-parameter slots are handled only when their factory bit is on (default mask 0 = nothing
// is handled, every aux trampoline takes its default); the rest is the trace. Logging is rate limited per (cls, slot) (pure code: n48metal::vhook_count / _log_kind).
static n48metal::VhookCounts gVhCounts;
int op_vhook(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret) {
	(void)ctx;
	const uint32_t n = n48metal::vhook_count(gVhCounts, cls, slot);
	uint32_t mask = 0;
	if (cls == N48_VC_VIDMEMORY || cls == N48_VC_RESOURCE) mask = op_factory_mask(nullptr);
	int h = 0;
	if (cls == N48_VC_RESOURCE && slot == 62u) h = n48disp_res62(self, args, nargs, ret);   // 0.0.613: type 0xC0 surfaces (11h.1 F3); 0 at once unless boot-arg navi48-metal-disp=1
	if (!h) h = n48metal::vhook_handle(mask, cls, slot, args, nargs, ret, n48metal::kKernelMin);
	const uint32_t k = n48metal::vhook_log_kind(n);
	if (k == n48metal::kVhLogFull) MNLOG("vhook: cls %u slot %u nargs %u call %u: %s", cls, slot, nargs, n, h ? "handled" : "not handled (default)");
	else if (k == n48metal::kVhLogCount) MNLOG("vhook: cls %u slot %u nargs %u: %u calls so far (last %s)", cls, slot, nargs, n, h ? "handled" : "default");
	return h;
}

// 0.0.613 (ABI 2): the display-pipe hook and the PCI getter (notes/design/NATIVE-S4-M11H.md section 3.4). The aux kext calls them only from a table of abi >= 2 that carries N48_DISP_F_ON.
int op_disp_hook(void *ctx, uint32_t cls, uint32_t slot, void *self, const uint64_t *args, uint32_t nargs, uint64_t *ret) { return n48disp_hook(ctx, cls, slot, self, args, nargs, ret); }
void *op_pci_device(void *ctx) { return n48disp_pci_device(ctx); }

// TWO tables, the one published chosen by the latch (n48disp::ops_shape): ABI 2 / 144 bytes with the display flag ONLY with boot-arg navi48-metal-disp=1; otherwise the ABI-1 / 120-byte table
// 0.0.612 published (its members are the same, the trailing ABI-2 members are outside its size and zero): the aux kext then takes every 0.0.2 default.
const N48MetalOps gOps = {
	N48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 620u, n48metal::kOpsFlags, 0u,
	op_device_open, op_device_close, op_populate_config, op_stamp_memory, op_stamp_va, op_task_window,
	op_factory_mask, op_mm_hook, op_trace,
	op_vhook, n48metal::kOpsCaps, 0,
	N48_DISP_F_ON, 0u, op_disp_hook, op_pci_device,
};
const N48MetalOps gOpsV1 = {
	N48_METAL_OPS_MAGIC, N48_METAL_ABI_MIN, N48_METAL_OPS_MIN, 620u, n48metal::kOpsFlags, 0u,
	op_device_open, op_device_close, op_populate_config, op_stamp_memory, op_stamp_va, op_task_window,
	op_factory_mask, op_mm_hook, op_trace,
	op_vhook, n48metal::kOpsCaps, 0,
	0u, 0u, nullptr, nullptr,
};

void ensure_lock() {
	if (gLock) return;
	IOLock *l = IOLockAlloc();
	if (l && !OSCompareAndSwapPtr(nullptr, l, &gLock)) IOLockFree(l);
}

n48metal::GateIn gate_now(uint64_t flags, bool wantBootArg) {
	n48metal::GateIn g {};
	const amdgpu::NativeS1bState &s = amdgpu::native_s1b_state();
	g.flags = flags;
	g.hello = amdgpu::n1c_hello_done();
	g.s1bGateOn = (s.gate == n48native::kGateOn);
	g.s1bRan = s.ran; g.s1bPositive = s.positivePass; g.s1bStopped = s.stopped;
	g.hung = amdgpu::n1c_hung();
	uint32_t v = 0;
	g.bootarg = wantBootArg && PE_parse_boot_argn("navi48-metal", &v, sizeof(v)) && v == 1u;
	return g;
}
} // namespace

// ---- the selectors --------------------------------------------------------------------------------------------------------------------------------------
IOReturn Navi48MetalNub::selectorPublish(Navi48Bringup *owner, uint64_t flags, uint64_t out[4]) {
	if (!owner || !out) return kIOReturnBadArgument;
	ensure_lock();
	if (!gLock) return kIOReturnNoMemory;
	IOLockLock(gLock);
	const n48metal::GateIn g = gate_now(flags, true);
	const uint32_t verdict = n48metal::publish_verdict(g, (n48metal::State)gState);
	if (verdict != n48metal::kOk) {
		IOLockUnlock(gLock);
		MNLOG("nub publish refused: %#x (hello %d flags %llu bootarg %d gate %d ran %d positive %d stopped %d hung %d state %u)", verdict, (int)g.hello, (unsigned long long)flags,
		      (int)g.bootarg, (int)g.s1bGateOn, (int)g.s1bRan, (int)g.s1bPositive, (int)g.s1bStopped, (int)g.hung, (unsigned)gState);
		return (IOReturn)verdict;
	}
	Navi48MetalNub *nub = OSTypeAlloc(Navi48MetalNub);
	OSDictionary *props = OSDictionary::withCapacity(4);
	IOReturn rc = kIOReturnNoMemory;
	if (nub && props && nub->init(props)) {
		OSString *model = OSString::withCString("AMD Radeon RX 9070 XT");
		const n48disp::OpsShape shape = n48disp::ops_shape(n48disp_latched_on());   // 0.0.613: OFF reports what 0.0.612 did (ABI 1, 120 bytes)
		OSNumber *abi = OSNumber::withNumber((uint64_t)shape.abi, 32);
		OSNumber *va = OSNumber::withNumber((uint64_t)n48metal::kTaskVaSize, 64);
		amdgpu::BringupContext *ctx = owner->bringupContext();
		OSNumber *vram = OSNumber::withNumber(ctx ? ctx->gmc.real_vram_size : 0ull, 64);
		OSNumber *ready = OSNumber::withNumber((uint64_t)n48metal::ready_at_publish(__atomic_load_n(&gReadyNo, __ATOMIC_ACQUIRE) != 0u, g.hung), 32);   // 0.0.612: 1 while usable
		if (model && abi && va && vram && ready) {
			nub->setProperty("model", model);
			nub->setProperty("Navi48,MetalABI", abi);
			nub->setProperty("Navi48,TaskVASize", va);
			nub->setProperty("Navi48,VRAMBytes", vram);
			nub->setProperty("Navi48,Ready", ready);
			if (nub->attach(owner)) {
				gNub = nub;                                                       // our reference (from alloc) is kept until withdraw
				gState = n48metal::sm_after_publish((n48metal::State)gState);
				out[0] = 1; out[1] = nub->getRegistryEntryID(); out[2] = shape.abi; out[3] = (uint64_t)shape.size;
				rc = kIOReturnSuccess;
			}
		}
		if (model) model->release();
		if (abi) abi->release();
		if (va) va->release();
		if (vram) vram->release();
		if (ready) ready->release();
	}
	if (props) props->release();
	if (rc != kIOReturnSuccess) {
		IOLockUnlock(gLock);
		if (nub) nub->release();                                                  // AFTER the unlock: the last release runs free(), which takes gLock
		MNLOG("nub publish failed: %#x", (unsigned)rc);
		return rc;
	}
	nub->retain();                                                                // held across registerService(): a racing withdraw must not free the nub under it
	IOLockUnlock(gLock);
	MNLOG("nub published (id %llu, ops ABI %u, %u bytes): the aux kext may now match it", (unsigned long long)out[1], (unsigned)out[2], (unsigned)out[3]);
	nub->registerService();                                                       // outside the lock: matching may run the aux kext's probe, which calls back into the ops
	nub->release();                                                               // the extra reference taken under the lock
	return kIOReturnSuccess;
}

IOReturn Navi48MetalNub::selectorWithdraw(uint64_t flags, uint64_t out[1]) {
	if (!out) return kIOReturnBadArgument;
	ensure_lock();
	if (!gLock) return kIOReturnNoMemory;
	IOLockLock(gLock);
	const n48metal::GateIn g = gate_now(flags, false);
	const uint32_t verdict = n48metal::withdraw_verdict(g, (n48metal::State)gState);
	if (verdict != n48metal::kOk) { IOLockUnlock(gLock); return (IOReturn)verdict; }
	Navi48MetalNub *nub = gNub;
	out[0] = gState;
	gNub = nullptr;
	gState = n48metal::sm_after_withdraw((n48metal::State)gState);
	IOLockUnlock(gLock);
	if (nub) { nub->terminate(); nub->release(); }
	n48disp_on_withdraw();                                                        // 0.0.613: forget the pipes AFTER the accelerator was told to stop (a no-op unless boot-arg navi48-metal-disp=1)
	MNLOG("nub withdrawn");
	return kIOReturnSuccess;
}

// 0.0.612 (W3): called by native_s1c.cpp's hang_announce right after the HUNG latch is set. The sticky word is stored FIRST (a publish that races us reads it under gLock or is
// ordered before our write of the property, which waits for gLock), then the published nub's property goes to 0. No nub yet: gLock may not even exist; the sticky word covers it.
void Navi48MetalNub::hungLatched() {
	__atomic_store_n(&gReadyNo, n48metal::ready_sticky_after(__atomic_load_n(&gReadyNo, __ATOMIC_ACQUIRE) != 0u, true) ? 1u : 0u, __ATOMIC_RELEASE);
	n48disp_on_hung();                                                            // 0.0.617 (K2): a declined device with an ARMED display pipe is fatal to WindowServer (m11h5-3): disarm first (no lock taken; a no-op unless boot-arg navi48-metal-disp=1 and armed)
	if (!gLock) { MNLOG("HUNG latched: Navi48,Ready = 0 (no nub is published)"); return; }
	IOLockLock(gLock);
	Navi48MetalNub *nub = gNub;
	if (nub) nub->setProperty("Navi48,Ready", (unsigned long long)n48metal::kReadyNo, 32);
	IOLockUnlock(gLock);
	MNLOG("HUNG latched: Navi48,Ready = 0 (%s)", nub ? "written on the published nub" : "no nub is published");
}

IOService *Navi48MetalNub::published() { return gNub; }   // 0.0.613: identity only (NOT retained)

void Navi48MetalNub::shutdown() {
	if (!gLock) return;
	IOLockLock(gLock);
	Navi48MetalNub *nub = gNub;
	if (nub) { gNub = nullptr; gState = n48metal::sm_after_withdraw((n48metal::State)gState); }
	IOLockUnlock(gLock);
	if (nub) { nub->terminate(); nub->release(); MNLOG("nub terminated at kext stop"); }
	n48disp_on_withdraw();
}

IOReturn Navi48MetalNub::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3, void *param4) {
	if (functionName && functionName->isEqualTo(N48_METAL_FN_SYMBOL)) {
		if (!param1) return kIOReturnBadArgument;
		*(const N48MetalOps **)param1 = n48disp::ops_shape(n48disp_latched_on()).abi >= 2u ? &gOps : &gOpsV1;
		return kIOReturnSuccess;
	}
	return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

void Navi48MetalNub::free() {
	if (gLock) { IOLockLock(gLock); gState = n48metal::sm_after_free((n48metal::State)gState); IOLockUnlock(gLock); }
	super::free();
}
