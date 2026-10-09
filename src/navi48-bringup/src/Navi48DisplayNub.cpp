//
//  Navi48DisplayNub.cpp - see the header. Pure logic: amd/native_fb_pure.h (tests/native_disp_test.cpp), the shared interface: Navi48DisplayOps.h.
//
//  0.0.658 (ABI 2): ONE nub PER DISPLAY INDEX. `fbpublish 2` publishes index 1 (the monitor B: instance 2, OTG2, DDC line 3, 2560x1440), `fbpublish 1` index 2 (the monitor A: instance 1, OTG1, DDC line 2, 1920x1080); the verb's
//  argument is the disp2 INSTANCE and ONE table (Navi48DisplayOps.h n48disp_geom_for_index) maps it. Publishing one index reads and writes only that instance's state: the other nub, snapshot and flags are untouched.
//
//  What this file WRITES: nothing on the hardware. `fbpublish <inst>` reads the live plane gates (n48dcn::fbLive: reads only) and the display's EDID blocks 0 and 1 over its DDC line (n48dcn::fbEdid: the same
//  engine sequence as `accel ddcread`, the DC_I2C engine's registers only, through the DCN allowlist), then allocates one nub object and its property dictionary and registers the nub in the
//  IORegistry under the GPU's IOPCIDevice. The order is the contract: the software-only refusals (n48fb::pre_verdict) BEFORE any hardware read, the live gates BEFORE the DDC engine is touched,
//  the snapshot judged (n48disp_snap_valid) BEFORE the nub exists.
//
#include "Navi48DisplayNub.hpp"
#include "Navi48Bringup.hpp"
#include "Navi48DisplayOps.h"
#include "amd/native_fb_pure.h"
#include "amd/native_disp.h"          // n48disp_pipe_adopted
#include "amd/native_s1c.h"           // n1c_ws_is_open
#include "amd/amdgpu_log.h"
#include "dcn/navi48_dcn.hpp"         // fbView / fbLive / fbEdid
#include "dcn/navi48_disp2.h"         // N48D2_GATES, N48D2_GT_*
#include "dcn/navi48_dispread.h"      // N48DR_ statuses

#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

#define FBLOG(fmt, ...) AMDGPU_LOG("fb", fmt, ##__VA_ARGS__)

IOService *navi48_bringup_pci(void);   // Navi48Bringup.cpp: the GPU's IOPCIDevice (not retained)

OSDefineMetaClassAndStructors(Navi48DisplayNub, IOService)

// ---- state (one nub per boot; never withdrawn) ------------------------------------------------------------------------------------------------------------------------------------------
namespace {
constexpr unsigned   kNubs = 2;        // 0.0.658: slot 0 = display index 1 (the monitor B), slot 1 = display index 2 (the monitor A): slot = index - 1
IOLock              *gLock;            // created on first use; held across a whole fbpublish (serialises two callers, of either instance)
Navi48DisplayNub    *gNub[kNubs];      // our reference while the nub lives
volatile uint32_t    gExists[kNubs];   // 1 from the moment the nub is attached; NEVER cleared (no re-publish of that index this boot, even if the object is freed)
volatile uint32_t    gHaveSnap[kNubs]; // 1 once gSnap[slot] holds the published snapshot (set BEFORE the nub is registered, never cleared)
N48DispSnap          gSnap[kNubs];     // the IMMUTABLE snapshots: each written once, under gLock, before its registerService
uint8_t              gPropScratch[128]; // RDNA4FB's EDID,DDC<line> bytes: scratch of the verb, used only under gLock (file scope keeps the verb's stack frame at its 0.0.657 size)
volatile UInt32      gFb2Latch = 0;    // 0 unread, 1 off, 2 on (boot-arg navi48-fb2)

void ensure_lock() {
	if (gLock) return;
	IOLock *l = IOLockAlloc();
	if (l && !OSCompareAndSwapPtr(nullptr, l, &gLock)) IOLockFree(l);
}

// boot-arg navi48-fb2 == 1, read and latched ONCE (first writer wins; never re-read)
bool fb2_latched_on() {
	UInt32 l = gFb2Latch;
	if (l == 0u) {
		uint32_t v = 0;
		const bool present = PE_parse_boot_argn("navi48-fb2", &v, sizeof(v));
		OSCompareAndSwap(0u, n48fb::fb2_on(present, v) ? 2u : 1u, &gFb2Latch);
		l = gFb2Latch;
		FBLOG("boot-arg navi48-fb2 %s: fbpublish is %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent", l == 2u ? "ENABLED" : "OFF");
	}
	return l == 2u;
}

// ---- the ops table's hooks (Navi48DisplayOps.h) -------------------------------------------------------------------------------------------------------------------------------------------
int op_snapshot(void *nub, N48DispSnap *out) {
	int slot = -1;                                          // the slot whose nub this is (identity); a nub of ours answers only with ITS OWN index's snapshot
	for (unsigned i = 0; i < kNubs; i++) if (nub != nullptr && nub == (void *)gNub[i]) slot = (int)i;
	const bool ok = n48fb::snapshot_hook_verdict(slot >= 0, slot >= 0 && __atomic_load_n(&gHaveSnap[slot], __ATOMIC_ACQUIRE) != 0u, out != nullptr) == 0;
	if (!ok) return -1;
	*out = gSnap[slot];                                     // gSnap[slot] is immutable once gHaveSnap[slot] is 1
	return 0;
}
int op_power(void *nub, uint32_t state) {
	(void)nub;
	FBLOG("framebuffer power state %u recorded (M5: no hardware action; the monitor B keeps its last image)", state);
	return 0;
}
void op_trace(uint32_t ev, uint64_t a, uint64_t b) {
	FBLOG("framebuffer trace: event %u a %#llx b %#llx", ev, (unsigned long long)a, (unsigned long long)b);
}
const N48DispOps gDispOps = {
	N48_DISP_OPS_MAGIC, N48_DISP_ABI, (uint32_t)sizeof(N48DispOps), 664u, 0u, 0u,
	op_snapshot, op_power, op_trace,
	nullptr, nullptr, nullptr,
};

// RDNA4FB's "EDID,DDC<line>" property (the monitor B: EDID,DDC3, the monitor A: EDID,DDC2): the framebuffer child of OUR PCI function that carries Console,* (the node scanout_geometry in Navi48Bringup.cpp finds) publishes it as 128 bytes of OSData.
bool rdna4fb_edid_prop(const char *name, uint8_t out[128]) {
	IOService *pci = navi48_bringup_pci();
	if (!pci || !name) return false;
	bool found = false;
	if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
		while (OSObject *o = it->getNextObject()) {
			IOService *svc = OSDynamicCast(IOService, o);
			if (!svc || !svc->metaCast("IOFramebuffer")) continue;
			OSData *d = OSDynamicCast(OSData, svc->getProperty(name));
			if (d && d->getLength() == 128u && d->getBytesNoCopy()) { memcpy(out, d->getBytesNoCopy(), 128u); found = true; break; }
		}
		it->release();
	}
	return found;
}
}  // namespace

bool n48fb_nub_exists_idx(uint32_t index) { return index >= 1u && index <= kNubs && __atomic_load_n(&gExists[index - 1u], __ATOMIC_ACQUIRE) != 0u; }      // 0.0.658: the nub of ONE display index
bool n48fb_nub_exists(void) { return n48fb_nub_exists_idx(N48_DISP_INDEX) || n48fb_nub_exists_idx(N48_DISPA_INDEX); }                                         // ANY display nub (the interlocks: pipeadopt, pipearm 1, the Metal publish)

// ---- the verb ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
uint32_t Navi48DisplayNub::publishVerb(Navi48Bringup *owner, uint64_t arg, uint64_t *out, unsigned count) {
	(void)owner;
	if (!out || count < 13u) return n48fb::kBadArg;
	for (unsigned i = 0; i < count; i++) out[i] = 0u;
	ensure_lock();
	if (!gLock) { out[0] = n48fb::kPublishFailed; return n48fb::kPublishFailed; }
	IOLockLock(gLock);
	uint32_t st;
	Navi48DisplayNub *nub = nullptr;
	// 0.0.658: the argument is the disp2 INSTANCE; ONE table (n48disp_geom_for_index) gives its display index, OTG, DDC line and mode. A bad argument leaves index 0 / no geometry and is refused by pre_verdict before anything is used.
	const uint32_t inst = n48fb::inst_of_arg(arg), index = n48fb::index_of_arg(arg);
	N48DispGeom geom;
	bzero(&geom, sizeof geom);
	const bool haveGeom = n48disp_geom_for_index(index, &geom) != 0;
	do {
		// ---- phase 1: software facts only (no register is read)
		n48dcn::FbView v;
		bzero(&v, sizeof v);
		n48dcn::fbView(&v, inst);
		n48fb::Pre pre;
		bzero(&pre, sizeof pre);
		pre.argOk = n48fb::arg_ok(arg) && haveGeom;
		pre.latched = fb2_latched_on();
		pre.nubExists = n48fb_nub_exists_idx(index);                      // THIS index's nub: publishing the monitor B never blocks the monitor A and the reverse
		pre.held = v.held; pre.pinned = v.pinned; pre.devOk = v.devOk;
		pre.pipeAdopted = n48disp_pipe_adopted();
		pre.wsOpen = amdgpu::n1c_ws_is_open();
		pre.metalNub = n48metal_nub_published();                         // 0.0.653: the Metal nub (and the accelerator behind it) was published earlier this boot
		pre.m6 = n48m6_latched_on();                                     // 0.0.659 (M6 Stage 1a, R2): under navi48-m6 the three interlocks above are lifted (the routing guard replaces them)
		st = n48fb::pre_verdict(pre);
		FBLOG("fbpublish %llu (instance %u -> display index %u, OTG%u, DDC line %u): arg %d latched %d nub %d held %d pinned %d dev %d pipe-adopted %d ws-open %d metal-nub %d -> %u (%s)", (unsigned long long)arg, inst, index, geom.otg, geom.ddcLine,
		      (int)pre.argOk, (int)pre.latched, (int)pre.nubExists, (int)pre.held, (int)pre.pinned, (int)pre.devOk, (int)pre.pipeAdopted, (int)pre.wsOpen, (int)pre.metalNub, st, n48fb::status_name(st));
		if (st != n48fb::kOk) break;
		// ---- phase 2a: the plane's live gates (READS ONLY), before the DDC engine is touched
		uint32_t holdBad = 0xFFFFFFFFu, gates[N48D2_GATES];
		for (uint32_t i = 0; i < N48D2_GATES; i++) gates[i] = 0u;
		const uint32_t ls = n48dcn::fbLive(inst, &holdBad, gates);
		if (ls == N48D2_BUSY) { st = n48fb::kBusy; break; }
		if (ls != N48D2_OK) { st = n48fb::kNoDevice; break; }
		out[10] = ((uint64_t)(gates[N48D2_GT_EARLY_HI] & 0xFFFFu) << 32) | gates[N48D2_GT_EARLY_LO];
		out[11] = (uint64_t)gates[N48D2_GT_HUBP_CNTL] | ((uint64_t)gates[N48D2_GT_FLIP_CTL] << 32);
		out[12] = (uint64_t)gates[N48D2_GT_VMFAULT] | ((uint64_t)holdBad << 32);
		FBLOG("fbpublish: live gates (instance %u): EARLIEST_INUSE %#llx (A %#llx) HUBP<inst>_DCHUBP_CNTL %#010x FLIP_CONTROL %#010x DCN_VM_FAULT_STATUS %#010x -> hold verdict %#x", inst, (unsigned long long)out[10], (unsigned long long)v.mc[0],
		      gates[N48D2_GT_HUBP_CNTL], gates[N48D2_GT_FLIP_CTL], gates[N48D2_GT_VMFAULT], holdBad);
		st = n48fb::live_verdict(holdBad);
		if (st != n48fb::kOk) break;
		// ---- phase 2b: the display's EDID (blocks 0 and 1, its DDC line) and RDNA4FB's EDID,DDC<line> property
		uint8_t edid[256];
		uint8_t *const prop = gPropScratch;
		bzero(prop, 128u);
		const uint32_t es = n48dcn::fbEdid(geom.ddcLine, edid);
		if (es == N48DR_BUSY) { st = n48fb::kBusy; break; }
		n48fb::Data d;
		bzero(&d, sizeof d);
		d.edidStatus = es;
		d.block0Sum = n48disp_edid_sum_ok(edid); d.block0Hdr = n48disp_edid_header_ok(edid); d.block1Sum = n48disp_edid_sum_ok(edid + 128);
		d.propPresent = rdna4fb_edid_prop(n48fb::edid_prop_name(geom.ddcLine), prop);
		d.block0EqProp = d.propPresent && memcmp(edid, prop, 128u) == 0;
		d.apertureOk = n48fb::aperture_ok_len(v.bar0Phys, v.bar0Size, v.offA, geom.aperLen) && n48fb::len_in_alloc(geom.aperLen, v.bufBytes);
		out[9] = (uint64_t)d.block0Sum | ((uint64_t)d.block0Hdr << 1) | ((uint64_t)d.block1Sum << 2) | ((uint64_t)d.propPresent << 3) | ((uint64_t)d.block0EqProp << 4) | ((uint64_t)d.apertureOk << 5) | ((uint64_t)es << 8);
		FBLOG("fbpublish: EDID read status %u (%s); block 0 checksum %d header %d, block 1 checksum %d; RDNA4FB %s %s, block 0 %s it; aperture BAR0 phys %#llx + offset %#llx (BAR0 size %#llx, %llu bytes, plane buffer %llu bytes) %s",
		      es, n48dr_status_name(es), (int)d.block0Sum, (int)d.block0Hdr, (int)d.block1Sum, n48fb::edid_prop_name(geom.ddcLine), d.propPresent ? "present" : "MISSING", d.block0EqProp ? "EQUALS" : "DIFFERS from",
		      (unsigned long long)v.bar0Phys, (unsigned long long)v.offA, (unsigned long long)v.bar0Size, (unsigned long long)geom.aperLen, (unsigned long long)v.bufBytes, d.apertureOk ? "ok" : "REFUSED");
		st = n48fb::data_verdict(d);
		if (st != n48fb::kOk) break;
		// ---- the snapshot, judged before the nub exists
		N48DispSnap snap;
		n48fb::snap_build_idx(&snap, index, v.bar0Phys, v.offA, v.mc[0], v.mc[1], edid);
		const uint32_t sv = n48disp_snap_valid(&snap);
		if (sv != N48_DSV_OK) { FBLOG("fbpublish: the snapshot failed validation (verdict %u)", sv); st = n48fb::kSnapshot; break; }
		// ---- publish: the nub is created, the snapshot stored BEFORE registration (the aux kext reads it in start), the nub attached to the GPU's PCI device
		const unsigned slot = index - 1u;
		IOService *pci = navi48_bringup_pci();
		OSDictionary *props = OSDictionary::withCapacity(3);
		nub = OSTypeAlloc(Navi48DisplayNub);
		st = n48fb::kPublishFailed;
		if (pci && props && nub && nub->init(props)) {
			OSNumber *idx = OSNumber::withNumber((uint64_t)index, 32);
			OSNumber *abi = OSNumber::withNumber((uint64_t)N48_DISP_ABI, 32);
			if (idx && abi) {
				nub->setProperty(N48_DISP_INDEX_KEY, idx);
				nub->setProperty("Navi48,DisplayABI", abi);
				gSnap[slot] = snap;
				__atomic_store_n(&gHaveSnap[slot], 1u, __ATOMIC_RELEASE);
				if (nub->attach(pci)) {
					gNub[slot] = nub;                                                 // our reference (from alloc) is kept for the rest of the boot
					__atomic_store_n(&gExists[slot], 1u, __ATOMIC_RELEASE);
					st = n48fb::kOk;
				} else __atomic_store_n(&gHaveSnap[slot], 0u, __ATOMIC_RELEASE);
			}
			if (idx) idx->release();
			if (abi) abi->release();
		}
		if (props) props->release();
		if (st != n48fb::kOk) break;
		out[1] = nub->getRegistryEntryID();
		out[2] = snap.aperPhys; out[3] = snap.aperLen; out[4] = snap.mcA; out[5] = snap.mcB;
		out[6] = (uint64_t)snap.w | ((uint64_t)snap.h << 16) | ((uint64_t)snap.index << 32);
		out[7] = (uint64_t)snap.refresh1616 | ((uint64_t)snap.edidLen << 32);
		out[8] = snap.pixHz;
		nub->retain();                                                                // held across registerService(): see below
	} while (false);
	if (st != n48fb::kOk) {
		IOLockUnlock(gLock);
		if (nub && !n48fb_nub_exists_idx(index)) nub->release();                     // a failed publish: AFTER the unlock
		out[0] = st;
		FBLOG("fbpublish %llu: REFUSED / FAILED: %s", (unsigned long long)arg, n48fb::status_name(st));
		return st;
	}
	IOLockUnlock(gLock);
	FBLOG("fbpublish: nub published (id %llu), Navi48DisplayIndex %u (instance %u), aperture %#llx + %#llx, A %#llx B %#llx, %ux%u refresh %#x: the aux framebuffer may now match it",
	      (unsigned long long)out[1], index, inst, (unsigned long long)out[2], (unsigned long long)out[3], (unsigned long long)out[4], (unsigned long long)out[5], geom.w, geom.h, (unsigned)geom.refresh1616);
	nub->registerService();                                                           // outside the lock: matching runs the aux kext's start, which calls the ops
	nub->release();                                                                   // the extra reference taken above
	out[0] = n48fb::kOk;
	return n48fb::kOk;
}

IOService *Navi48DisplayNub::framebufferOf(uint32_t index) {
	if (index < 1u || index > kNubs || !gLock) return nullptr;
	IOLockLock(gLock);
	Navi48DisplayNub *nub = gNub[index - 1u];
	if (nub) nub->retain();
	IOLockUnlock(gLock);
	if (!nub) return nullptr;
	IOService *found = nullptr;
	if (OSIterator *it = nub->getChildIterator(gIOServicePlane)) {
		while (OSObject *o = it->getNextObject()) {
			IOService *svc = OSDynamicCast(IOService, o);
			const OSMetaClass *mc = svc ? svc->getMetaClass() : nullptr;
			const char *cn = mc ? mc->getClassName() : nullptr;
			if (cn && strcmp(cn, "Navi48Framebuffer") == 0 && svc->metaCast("IOFramebuffer")) { found = svc; found->retain(); break; }      // the aux class of BOTH personalities
		}
		it->release();
	}
	nub->release();
	return found;
}

IOService *Navi48DisplayNub::published() { return gNub[0] ? gNub[0] : gNub[1]; }      // 0.0.658: the first published nub (the monitor B's, else the monitor A's); NOT retained

void Navi48DisplayNub::shutdown() {
	if (!gLock) return;
	for (unsigned i = 0; i < kNubs; i++) {
		IOLockLock(gLock);
		Navi48DisplayNub *nub = gNub[i];
		gNub[i] = nullptr;
		IOLockUnlock(gLock);
		if (nub) { nub->terminate(); nub->release(); FBLOG("display nub (index %u) terminated at kext stop", i + 1u); }
	}
}

IOReturn Navi48DisplayNub::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2, void *param3, void *param4) {
	if (functionName && functionName->isEqualTo(N48_DISP_FN_SYMBOL)) {
		if (!param1) return kIOReturnBadArgument;
		*(const N48DispOps **)param1 = &gDispOps;
		return kIOReturnSuccess;
	}
	return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

void Navi48DisplayNub::free() {
	for (unsigned i = 0; i < kNubs; i++) if (gNub[i] == this) gNub[i] = nullptr;      // gExists[i] stays 1: no re-publish of that index this boot, and the interlocks stay on
	super::free();
}
