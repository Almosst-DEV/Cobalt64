//
//  Navi48Bringup.cpp — bring-up service: read-only survey by default, PSP
//  bootloader chain when navi48-psp=1.
//
#include "Navi48Bringup.hpp"
#include "apple/Navi48Ttl.hpp"
#include "apple/AppleTtlHook.hpp"
#include "apple/AppleHardwareHook.hpp"
#include "apple/pairing_policy.h"
#include "regs.hpp"
#include "amd/amdgpu_discovery.h"
#include "amd/amdgpu_init.h"
#include "amd/amdgpu_regs.h"
#include "amd/amdgpu_sdma.h"
#include "Navi48MetalNub.hpp"   // 0.0.610: terminate a published Metal nub at stop
#include "Navi48DisplayNub.hpp"       // 0.0.652 (M5): fbpublish 2
#include <libkern/c++/OSData.h>
#include "amd/native_s1b.h"   // 0.0.600 (native S1b): navi48-native=1, default OFF
#include "amd/native_s1c.h"   // 0.0.612: n1c_latch_pci_bars
#include "Navi48NativeClient.hpp"   // 0.0.601 (native S1c): the N48N user client
#include "Navi48UserClient.hpp"
#include "dcn/navi48_dcn.hpp"
#include "dcn/navi48_scanout_pure.h"   // 0.0.603 (native S2a): n48scan::accel_exempt
#include "amd/native_disp.h"            // 0.0.613 (#11 11h.2): the display pipe glue (boot-arg navi48-metal-disp=1)
#include "amd/native_disp_pure.h"
#include "amd/native_agdc_pure.h"      // 0.0.614 (#11 11h.3): the native AGDC service (verb 88)
#include "amd/native_g1_pure.h"        // 0.0.623 (GPU-apps G1): hangtest / hangrecover / hangstat (verbs 100..102, boot-arg navi48-g1=1)
#include "amd/native_g5_pure.h"        // 0.0.650 (GPU-apps G5 Stage 1): sessstat page 5 (boot-arg navi48-g5=1)
#include "amd/native_g2_pure.h"        // 0.0.627 (GPU-apps G2): sessstat (verb 103, boot-arg navi48-multisession=1)
#include "amd/native_g4_pure.h"        // 0.0.640 (GPU-apps G4): appallow (verb 104, boot-args navi48-apps=1 + navi48-multisession=1)
#include "dcn/navi48_fbname.hpp"
#include <IOKit/IOLib.h>
#include <kern/thread.h>            // current_thread() - the MM-window priority owner identity (0.0.433)
#include <sys/proc.h>               // build 0.0.512 Part B: proc_selfpid/proc_name on the clock88w watch lines
#include "apple/gfx_clock88.h"      // build 0.0.512 Part B: the clock88 watch (pure)
#include "apple/gfx_ks81.h"         // build 0.0.526: the marker-holder gate in switch 37's yield (pure)
#include "apple/gfx_sk82.h"         // build 0.0.527 (an internal design note): switch 82, the byte-identical re-copy skip (pure)
#include <IOKit/IOMemoryDescriptor.h> // build 0.0.527: the source re-read under gSk82Lock (readBytes)
#include <kern/clock.h>             // clock_get_uptime/absolutetime_to_nanoseconds - the owner's wait and non-owner
                                     // yields are both timed (mirrors AppleHardwareHook.cpp's n48_pol_ns)

extern "C" {
extern const uint8_t fw_psp_14_0_3_sos[];
extern const size_t  fw_psp_14_0_3_sos_size;
}

OSDefineMetaClassAndStructors(Navi48Bringup, IOService)

namespace {
// Everything the kext puts in VRAM fits in this span above vramBase:
//   +0x000000 fw_pri (1 MiB)   +0x100000 PSP ring/cmd/fence   +0x200000 TMR (4 MiB)
//   +0x1000000 fw_buf (8 MiB)  — mirrors mac-amdgpu's layout, relocated.
constexpr uint64_t kBringupSpan   = 32ULL << 20;
constexpr uint64_t kOneMiB        = 1ULL << 20;
constexpr uint64_t kFallbackBase  = 64ULL << 20;   // if the console FB can't be located
constexpr uint64_t kMaxBar0Map    = n48rebar::kMaxBar0Map;   // 0.0.663 (ReBAR item 1): 1 GiB, what Tahoe's IOPCIFamily can size; never map more aperture than this (was 256 MiB)

// Drain write-combining buffers so BAR0 writes are visible to the GPU before
// we ring its doorbell registers.
inline void storeFence() { __asm__ __volatile__("sfence" ::: "memory"); }

// n48::HwAccess adapter: keeps the IOKit class single-inheritance.
struct HwBridge final : n48::HwAccess {
	Navi48Bringup *o;
	explicit HwBridge(Navi48Bringup *owner) : o(owner) {}
	uint32_t regReadIp (uint16_t h, uint8_t i, uint8_t s, uint32_t d) override { return o->regReadIp(h, i, s, d); }
	bool     regWriteIp(uint16_t h, uint8_t i, uint8_t s, uint32_t d, uint32_t v) override { return o->regWriteIp(h, i, s, d, v); }
	bool     vramWrite (uint64_t off, const void *src, size_t len) override { return o->vramWrite(off, src, len); }
	bool     vramMemset(uint64_t off, uint8_t v, size_t len) override { return o->vramMemset(off, v, len); }
};
} // namespace

// ---------------------------------------------------------------------------
// BAR5 register harness
// ---------------------------------------------------------------------------

// 0.0.612 (review item A): read every PCI BAR of this GPU ONCE, at start, from the IOPCIDevice: the six BAR registers and the expansion ROM, each through getDeviceMemoryWithRegister (the
// FULL assigned range: BAR0 is 16 GiB on a ReBAR boot, not the 256 MiB window bar0Size maps; a 64-bit BAR is one range, its upper-half register yields none). Hands the list to
// native_s1c.cpp, which keeps it (first writer wins) and refuses any BoImportHost page inside one. Read-only: config-space reads and registry data, no register write.
// 0.0.663 (ReBAR item 5): the BAR0 range is the UNION of IOPCIFamily's and our own config-space one (base from the BAR0/BAR1 dwords, size from the ReBAR capability, read-only). A BAR0 that IOPCIFamily dropped
// (it publishes length 0 for a BAR it cannot size) is therefore still guarded, and a disagreement refuses the native client open (n1c_latch_bar0_conflict).
static n48rebar::Census readRebarCensus(IOPCIDevice *pci) {
	return n48rebar::census_read([pci](uint32_t o) -> uint32_t { return pci->configRead32(o); });
}
static uint64_t readCfgBar0(IOPCIDevice *pci) {
	const uint32_t lo = pci->configRead32(kIOPCIConfigBaseAddress0);
	const uint32_t hi = ((lo & 0x6) == 0x4) ? pci->configRead32(kIOPCIConfigBaseAddress1) : 0;
	return n48rebar::cfg_bar_addr(lo, hi);
}
static void latchPciBars(IOPCIDevice *pci) {
	static const UInt8 kRegs[] = { kIOPCIConfigBaseAddress0, kIOPCIConfigBaseAddress1, kIOPCIConfigBaseAddress2, kIOPCIConfigBaseAddress3,
	                                  kIOPCIConfigBaseAddress4, kIOPCIConfigBaseAddress5, kIOPCIConfigExpansionROMBase };
	uint64_t base[7] = {}, size[7] = {};
	uint32_t n = 0;
	uint64_t io0Base = 0, io0Len = 0;
	for (uint32_t i = 0; i < 7u; i++) {
		IODeviceMemory *m = pci->getDeviceMemoryWithRegister(kRegs[i]);
		if (!m) continue;
		const uint64_t len = m->getLength();
		if (len == 0) continue;
		base[n] = (uint64_t)m->getPhysicalAddress(); size[n] = len; n++;
		if (i == 0) { io0Base = base[n - 1]; io0Len = len; }
		N48LOG("pci bars: register %#x -> [%#llx, +%#llx)", kRegs[i], (unsigned long long)base[n - 1], (unsigned long long)len);
	}
	// The config-space BAR0 and the ReBAR capability's current size, then the union.
	const n48rebar::Census cen = readRebarCensus(pci);
	const int e0 = n48rebar::census_entry(cen, 0u);
	const uint64_t cfgLen = (cen.found && e0 >= 0) ? n48rebar::entry_bytes(cen.e[e0].ctrlReg) : 0ull;
	const uint64_t cfgBase = readCfgBar0(pci);
	const n48rebar::LatchOut u = n48rebar::bar0_latch(io0Base, io0Len, cfgBase, cfgLen);
	if (u.have) {
		if (io0Len != 0 && n > 0 && base[0] == io0Base) { base[0] = u.base; size[0] = u.len; }              // BAR0 is entry 0 when IOPCIFamily published it
		else if (n < 7u) { for (uint32_t k = n; k > 0; k--) { base[k] = base[k - 1]; size[k] = size[k - 1]; } base[0] = u.base; size[0] = u.len; n++; }   // a dropped BAR0 is added
		N48LOG("pci bars: BAR0 union [%#llx, +%#llx) (IOPCIFamily [%#llx, +%#llx), config space %#llx, ReBAR size %#llx)%s", (unsigned long long)u.base, (unsigned long long)u.len,
		       (unsigned long long)io0Base, (unsigned long long)io0Len, (unsigned long long)cfgBase, (unsigned long long)cfgLen, u.conflict ? " - DISAGREE" : "");
	}
	amdgpu::n1c_latch_bar0_conflict(u.conflict);
	amdgpu::n1c_latch_pci_bars(base, size, n);
}

bool Navi48Bringup::mapRegisters() {
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress5);
	if (!bar) { N48LOG("mmio: BAR5 not present/assigned"); return false; }
	rmmioMap = bar->map();
	if (!rmmioMap) { N48LOG("mmio: failed to map BAR5"); return false; }
	rmmio     = reinterpret_cast<volatile uint32_t *>(rmmioMap->getVirtualAddress());
	rmmioSize = rmmioMap->getLength();
	N48LOG("mmio: BAR5 mapped, %zu KiB", rmmioSize / 1024);
	return rmmio != nullptr;
}

void Navi48Bringup::unmapRegisters() {
	rmmio = nullptr; rmmioSize = 0;
	if (rmmioMap) { rmmioMap->release(); rmmioMap = nullptr; }
}

uint32_t Navi48Bringup::regRead32(uint32_t byteOffset) const {
	if (!rmmio || byteOffset + 4 > rmmioSize) return 0xFFFFFFFF;
	return rmmio[byteOffset / 4];
}

bool Navi48Bringup::regWrite32(uint32_t byteOffset, uint32_t value) {
	if (!rmmio || byteOffset + 4 > rmmioSize) return false;
	rmmio[byteOffset / 4] = value;
	return true;
}

uint32_t Navi48Bringup::regReadIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword) {
	uint32_t off;
	if (!ipDiscovery.isValid() || !ipDiscovery.regByteOffset(hwId, inst, seg, dword, off)) return 0xFFFFFFFF;
	return regRead32(off);
}

bool Navi48Bringup::regWriteIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword, uint32_t value) {
	uint32_t off;
	if (!ipDiscovery.isValid() || !ipDiscovery.regByteOffset(hwId, inst, seg, dword, off)) return false;
	return regWrite32(off, value);
}

uint32_t Navi48Bringup::vramRead32(uint64_t pos) const {
	if (!rmmio) return 0xFFFFFFFF;
	rmmio[n48::kMMIndexHiDword] = static_cast<uint32_t>(pos >> 31);
	rmmio[n48::kMMIndexDword]   = (static_cast<uint32_t>(pos) & 0x7ffffffc) | 0x80000000u;
	return rmmio[n48::kMMDataDword];
}

// ---------------------------------------------------------------------------
// BAR0 VRAM aperture
// ---------------------------------------------------------------------------

// 0.0.663 (ReBAR items 2, 3, 6): plan BEFORE mapping. The inputs: IOPCIFamily's BAR0 range, config space's BAR0 address, the ReBAR capability's current BAR0 size (READ-ONLY; the capability is never written),
// the VRAM size and the hi-pool reserve. A refusal maps nothing: the ladder stops at "no-bar0" and the machine keeps its CPU desktop (RDNA4FB), exactly as with no BAR0. The census is published as registry
// properties and logged either way. The console is added by finishVramPlan once it has been located.
bool Navi48Bringup::mapVramAperture() {
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	if (!bar) { N48LOG("vram: BAR0 not present/assigned"); return false; }
	const uint64_t fullLen = bar->getLength();
	const n48rebar::Census cen = readRebarCensus(pciDevice);
	{
		const int e0 = n48rebar::census_entry(cen, 0u), e2 = n48rebar::census_entry(cen, 2u);
		N48LOG("rebar census: extended config %s; Resizable BAR capability %s at %#x, %u entr%s", cen.extReadable ? "readable" : "UNREADABLE (all ones)", cen.found ? "found" : "NOT found", cen.capOff, cen.n, cen.n == 1 ? "y" : "ies");
		setProperty("ReBAR,Found", cen.found);
		setProperty("ReBAR,ExtConfigReadable", cen.extReadable);
		if (cen.found) setProperty("ReBAR,CapOffset", (uint64_t)cen.capOff, 32);
		for (uint32_t k = 0; k < 2u; k++) {
			const int e = k == 0 ? e0 : e2;
			const char *nm = k == 0 ? "BAR0" : "BAR2";
			if (e < 0) { N48LOG("rebar census: %s has no resizable entry", nm); continue; }
			const uint32_t cap = cen.e[e].capReg, ctl = cen.e[e].ctrlReg;
			N48LOG("rebar census: %s capability dword %#010x (supported sizes mask %#x, bit k = 1 MiB << k), control dword %#010x -> current size code %u = %llu MiB", nm, cap, n48rebar::entry_supported_mask(cap), ctl,
			       n48rebar::entry_code(ctl), (unsigned long long)(n48rebar::entry_bytes(ctl) >> 20));
			char key[40];
			snprintf(key, sizeof(key), "ReBAR,%s,SupportedMask", nm); setProperty(key, (uint64_t)n48rebar::entry_supported_mask(cap), 32);
			snprintf(key, sizeof(key), "ReBAR,%s,CurrentMB", nm);     setProperty(key, (uint64_t)(n48rebar::entry_bytes(ctl) >> 20), 64);
		}
	}
	const uint64_t cfgAddr = readCfgBar0(pciDevice);
	const int e0 = n48rebar::census_entry(cen, 0u);
	rebarFound = cen.found && e0 >= 0;
	rebarMask = rebarFound ? n48rebar::entry_supported_mask(cen.e[e0].capReg) : 0u;
	rebarCurMB = rebarFound ? (n48rebar::entry_bytes(cen.e[e0].ctrlReg) >> 20) : 0ull;
	if (rebarFound) N48LOG("rebar census: BAR0 offers sizes mask %#x, smallest %llu MiB, current %llu MiB (ResizeAppleGpuBars code for 'smallest' = the lowest set bit)", rebarMask, (unsigned long long)(n48rebar::entry_min_bytes(rebarMask) >> 20), (unsigned long long)rebarCurMB);
	n48rebar::PlanIn pin {};
	pin.ioLen = fullLen; pin.ioAddr = (uint64_t)bar->getPhysicalAddress(); pin.cfgAddr = cfgAddr;
	pin.rebarPresent = cen.found && e0 >= 0; pin.rebarBytes = pin.rebarPresent ? n48rebar::entry_bytes(cen.e[e0].ctrlReg) : 0ull;
	pin.vramBytes = static_cast<uint64_t>(vramMB) << 20; pin.hiReserve = amdgpu::kVramHiTotalReserve;
	pin.consoleOff = n48rebar::kNoConsole; pin.consoleLen = 0; pin.mapCap = kMaxBar0Map;
	const n48rebar::PlanOut pl = n48rebar::plan_bar0(pin);
	N48LOG("vram: BAR0 plan: IOPCIFamily [%#llx, +%llu MiB), config space %#llx, ReBAR %s %llu MiB, VRAM %llu MiB -> %s%s (map %llu MiB, vramLimit %llu MiB)", (unsigned long long)pin.ioAddr, (unsigned long long)(pin.ioLen >> 20),
	       (unsigned long long)pin.cfgAddr, pin.rebarPresent ? "current" : "absent", (unsigned long long)(pin.rebarBytes >> 20), (unsigned long long)(pin.vramBytes >> 20),
	       pl.ok ? "OK" : "REFUSED: ", pl.ok ? "" : n48rebar::reason_name(pl.reason), (unsigned long long)(pl.mapLen >> 20), (unsigned long long)(pl.vramLimit >> 20));
	setProperty("BAR0,PlanReason", n48rebar::reason_name(pl.reason));
	if (!pl.ok) { N48LOG("vram: BAR0 refused (%s): nothing is mapped, the native ladder will not run", n48rebar::reason_name(pl.reason)); return false; }
	bar0Plan = pin;
	// Map the planned length: all of a BAR up to 1 GiB; anything larger is capped by kMaxBar0Map.
	const uint64_t winLen  = pl.mapLen;
	IODeviceMemory *win = IODeviceMemory::withSubRange(bar, 0, winLen);
	if (!win) { N48LOG("vram: BAR0 sub-range failed"); return false; }
	bar0Map = win->map(kIOMapWriteCombineCache);
	if (!bar0Map) bar0Map = win->map();
	win->release();                              // the map holds its own reference
	if (!bar0Map) { N48LOG("vram: failed to map BAR0"); return false; }
	bar0     = reinterpret_cast<volatile uint8_t *>(bar0Map->getVirtualAddress());
	bar0Size = bar0Map->getLength();             // item 3: bar0Size is the bytes ACTUALLY mapped
	if (bar0Size != winLen) {
		N48LOG("vram: BAR0 mapped %zu MiB but %llu MiB was planned; refusing", bar0Size >> 20, (unsigned long long)(winLen >> 20));
		unmapVramAperture();
		return false;
	}
	if (fullLen != bar0Size) N48LOG("vram: BAR0 is %llu MiB; mapped the first %zu MiB", fullLen >> 20, bar0Size >> 20);

	bar0Phys = cfgAddr;                          // planned equal to IOPCIFamily's address (plan_bar0 refuses a difference)
	N48LOG("vram: BAR0 aperture phys 0x%llx, %zu MiB, mapped %s", bar0Phys, bar0Size >> 20,
	       (bar0Map->getMapOptions() & kIOMapWriteCombineCache) ? "write-combining" : "uncached");
	setProperty("BAR0,Phys", bar0Phys, 64);
	setProperty("BAR0,SizeMB", static_cast<uint64_t>(bar0Size >> 20), 32);
	return bar0 != nullptr && bar0Size >= 2 * kOneMiB;
}

// The plan again, now that the console is located: judges the console against vramLimit and fixes bar0Limit (what buildDeviceContext hands the allocator as dev.vramLimit).
bool Navi48Bringup::finishVramPlan() {
	n48rebar::PlanIn pin = bar0Plan;
	if (bootFbOff != ~0ULL) { pin.consoleOff = bootFbOff; pin.consoleLen = bootFbLen; }
	const n48rebar::PlanOut pl = n48rebar::plan_bar0(pin);
	if (!pl.ok || pl.mapLen != bar0Size) {
		N48LOG("vram: BAR0 plan with the console: REFUSED (%s; planned map %llu MiB, mapped %zu MiB)", n48rebar::reason_name(pl.reason), (unsigned long long)(pl.mapLen >> 20), bar0Size >> 20);
		return false;
	}
	bar0Limit = pl.vramLimit;
	N48LOG("vram: vramLimit %llu MiB (mapped %zu MiB, VRAM %u MiB, hi reserve %llu MiB)", (unsigned long long)(bar0Limit >> 20), bar0Size >> 20, vramMB, (unsigned long long)(amdgpu::kVramHiTotalReserve >> 20));
	setProperty("BAR0,VramLimitMB", static_cast<uint64_t>(bar0Limit >> 20), 32);
	return true;
}

bool Navi48Bringup::mapDoorbells() {
	if (bar2Map) return true;
	IODeviceMemory *bar = pciDevice->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
	if (!bar) { N48LOG("doorbell: BAR2 not present/assigned"); return false; }
	{   // 0.0.664 (F2): IOPCIFamily's BAR2 address must be config space's (the self-ring aperture is pointed at the config-space one): same equality rule as BAR0
		const uint32_t clo = pciDevice->configRead32(kIOPCIConfigBaseAddress2);
		const uint32_t chi = ((clo & 0x6) == 0x4) ? pciDevice->configRead32(kIOPCIConfigBaseAddress3) : 0;
		if (!n48rebar::bar_addr_agree((uint64_t)bar->getPhysicalAddress(), n48rebar::cfg_bar_addr(clo, chi))) {
			N48LOG("doorbell: BAR2 refused: IOPCIFamily address %#llx != config space %#llx", (unsigned long long)bar->getPhysicalAddress(), (unsigned long long)n48rebar::cfg_bar_addr(clo, chi));
			return false;
		}
	}
	bar2Map = bar->map();                     // uncached: doorbells are MMIO
	if (!bar2Map) { N48LOG("doorbell: failed to map BAR2"); return false; }
	dev.bar2     = reinterpret_cast<volatile uint8_t *>(bar2Map->getVirtualAddress());
	dev.bar2Size = bar2Map->getLength();
	{
		uint32_t lo = pciDevice->configRead32(kIOPCIConfigBaseAddress2);
		uint32_t hi = ((lo & 0x6) == 0x4) ? pciDevice->configRead32(kIOPCIConfigBaseAddress3) : 0;
		dev.bar2Phys = (static_cast<uint64_t>(hi) << 32) | (lo & ~0xFULL);
	}
	N48LOG("doorbell: BAR2 mapped, %zu KiB at phys 0x%llx", dev.bar2Size >> 10, dev.bar2Phys);
	setProperty("BAR2,SizeKB", static_cast<uint64_t>(dev.bar2Size >> 10), 32);
	return dev.bar2 != nullptr;
}

// 0.0.664 (F4): after a refusal past the map the machine must be exactly the 'no BAR0' state: the map released, bar0 / bar0Size / bar0Phys / bar0Limit / vramBase all zero (nothing later may use a window the checks refused).
void Navi48Bringup::dropBar0() {
	unmapVramAperture();
	bar0Phys = 0; bar0Limit = 0; vramBase = 0;
}

void Navi48Bringup::unmapVramAperture() {
	bar0 = nullptr; bar0Size = 0;
	if (bar0Map) { bar0Map->release(); bar0Map = nullptr; }
}

uint32_t Navi48Bringup::bar0Read32(uint64_t vramOffset) const {
	if (!bar0 || vramOffset + 4 > bar0Size) return 0xFFFFFFFF;
	return *reinterpret_cast<const volatile uint32_t *>(bar0 + vramOffset);
}

bool Navi48Bringup::vramWrite(uint64_t vramOffset, const void *src, size_t len) {
	if (!bar0 || vramOffset + len > bar0Size) return false;
	const uint8_t *s = static_cast<const uint8_t *>(src);
	volatile uint8_t *d = bar0 + vramOffset;
	size_t i = 0;
	for (; i + 4 <= len; i += 4) {
		uint32_t w; __builtin_memcpy(&w, s + i, 4);
		*reinterpret_cast<volatile uint32_t *>(d + i) = w;
	}
	for (; i < len; i++) d[i] = s[i];
	storeFence();
	return true;
}

bool Navi48Bringup::vramMemset(uint64_t vramOffset, uint8_t value, size_t len) {
	if (!bar0 || vramOffset + len > bar0Size) return false;
	volatile uint8_t *d = bar0 + vramOffset;
	const uint32_t w = static_cast<uint32_t>(value) * 0x01010101u;
	size_t i = 0;
	for (; i + 4 <= len; i += 4) *reinterpret_cast<volatile uint32_t *>(d + i) = w;
	for (; i < len; i++) d[i] = value;
	storeFence();
	return true;
}

// The UEFI GOP framebuffer the kernel console (and IONDRVFramebuffer) draw
// into is VRAM behind BAR0 — Linux reserves it as "stolen VGA memory" at VRAM
// offset 0. Find it so nothing we stage lands on it.
// build 0.0.544 item 4b/4c: the console's size as getConsoleInfo reported it at start (0 until then), for the
// consumers that must follow the resolution (the fill set's members, gfx_fillset.h N48_FS_GEO_*). Written once, here; read-only after.
static uint32_t gN48ConsoleW { 0u }, gN48ConsoleH { 0u };
bool navi48_console_size(uint32_t &w, uint32_t &h) {
	if (!gN48ConsoleW || !gN48ConsoleH) return false;
	w = gN48ConsoleW; h = gN48ConsoleH;
	return true;
}

// 0.0.613: the console region for the display pipe's present. Known only when the boot capture located it inside BAR0 and rowBytes * height is its length.
bool Navi48Bringup::consoleGeometry(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes) const {
	if (bootFbOff == ~0ULL || bootFbLen == 0 || bootFbRow == 0 || !gN48ConsoleW || !gN48ConsoleH) return false;
	if (bootFbRow * gN48ConsoleH != bootFbLen || bootFbOff + bootFbLen > bar0Size) return false;
	if (off) *off = bootFbOff;
	if (len) *len = bootFbLen;
	if (w) *w = gN48ConsoleW;
	if (h) *h = gN48ConsoleH;
	if (rowBytes) *rowBytes = static_cast<uint32_t>(bootFbRow);
	return true;
}
bool Navi48Bringup::consoleWrite(uint64_t consoleOff, const void *src, size_t len) {
	uint64_t off = 0, clen = 0;
	if (!consoleGeometry(&off, &clen, nullptr, nullptr, nullptr)) return false;
	if (len == 0 || consoleOff > clen || len > clen - consoleOff) return false;              // the write stays inside the console, overflow-safe
	return vramWrite(off + consoleOff, src, len);
}


bool Navi48Bringup::captureBootFramebuffer() {
	PE_Video video {};
	IOPlatformExpert *platform = getPlatform();
	if (!platform || platform->getConsoleInfo(&video) != kIOReturnSuccess) {
		N48LOG("vram: getConsoleInfo failed — boot framebuffer location unknown");
		return false;
	}
	// v_baseAddr carries flag bits in its low bits on this platform (RDNA4FB
	// observed 0x840000001); the base itself is page aligned.
	bootFbPhys = static_cast<uint64_t>(video.v_baseAddr) & ~0xFFFULL;
	bootFbLen  = static_cast<uint64_t>(video.v_rowBytes) * video.v_height;
	bootFbRow  = static_cast<uint64_t>(video.v_rowBytes);                                           // 0.0.613
	gN48ConsoleW = static_cast<uint32_t>(video.v_width); gN48ConsoleH = static_cast<uint32_t>(video.v_height);   // build 0.0.544 4b
	if (bootFbPhys == 0 || bootFbLen == 0) {
		N48LOG("vram: console info empty (base 0x%llx len 0x%llx)", bootFbPhys, bootFbLen);
		return false;
	}
	const bool inBar0 = bootFbPhys >= bar0Phys && bootFbPhys + bootFbLen <= bar0Phys + bar0Size;
	bootFbOff = inBar0 ? bootFbPhys - bar0Phys : ~0ULL;
	N48LOG("vram: console framebuffer phys 0x%llx %lux%lu rowBytes %lu depth %lu (%llu KiB) -> %s",
	       bootFbPhys, video.v_width, video.v_height, video.v_rowBytes, video.v_depth, bootFbLen >> 10,
	       inBar0 ? "inside BAR0" : "NOT inside BAR0");
	if (inBar0) {
		N48LOG("vram: console framebuffer occupies vram+[0x%llx, 0x%llx)", bootFbOff, bootFbOff + bootFbLen);
		setProperty("BootFB,VramOffset", bootFbOff, 64);
		setProperty("BootFB,Length", bootFbLen, 64);
	}
	return true;
}

bool Navi48Bringup::chooseVramBase() {
	uint64_t base;
	if (bootFbOff != ~0ULL) {
		// First choice: just above the console FB (1 MiB aligned). If that does
		// not fit in the aperture (FB parked at the top), fall back to offset 0
		// provided the whole span ends below the FB.
		base = (bootFbOff + bootFbLen + kOneMiB - 1) & ~(kOneMiB - 1);
		if (base + kBringupSpan > bar0Size) {
			if (kBringupSpan <= bootFbOff) {
				N48LOG("vram: no room above the console FB; using vram+0 (FB starts at 0x%llx)", bootFbOff);
				base = 0;
			} else {
				N48LOG("vram: console FB at vram+0x%llx (%llu KiB) leaves no %llu MiB window in a %zu MiB BAR0; refusing",
				       bootFbOff, bootFbLen >> 10, kBringupSpan >> 20, bar0Size >> 20);
				return false;
			}
		}
	} else {
		base = kFallbackBase;                                             // unknown: assume it is low
		N48LOG("vram: console FB not located in BAR0; using fallback base %llu MiB", base >> 20);
		if (base + kBringupSpan > bar0Size) {
			N48LOG("vram: fallback base does not fit in a %zu MiB BAR0; refusing", bar0Size >> 20);
			return false;
		}
	}
	// Belt and braces: never overlap the console FB whatever the arithmetic said.
	if (bootFbOff != ~0ULL && base < bootFbOff + bootFbLen && base + kBringupSpan > bootFbOff) {
		N48LOG("vram: chosen base overlaps console FB; refusing");
		return false;
	}
	vramBase = base;
	N48LOG("vram: bring-up region vram+[0x%llx, 0x%llx) (%llu MiB in)", vramBase, vramBase + kBringupSpan, vramBase >> 20);
	setProperty("Navi48,VramBaseOffset", vramBase, 64);
	return true;
}

// Write a pattern through BAR0 and read it back through the MM_INDEX/MM_DATA
// indirect window (the path that already read the on-die discovery table).
// Both address VRAM by byte offset; if they agree, BAR0 offset == VRAM offset
// and our MC arithmetic (FB_LOCATION_BASE + offset) stands. Touches only the
// first 16 bytes of our own region.
bool Navi48Bringup::apertureCheck(uint64_t off) {
	const uint32_t pat[4] = { 0x4E343821u, 0xA5C3F00Du, static_cast<uint32_t>(off), static_cast<uint32_t>(~off) };
	uint32_t before[4], mm[4], bar[4];
	for (uint64_t i = 0; i < 4; i++) before[i] = vramRead32(off + 4 * i);
	if (!vramWrite(off, pat, sizeof(pat))) return false;
	for (uint64_t i = 0; i < 4; i++) { mm[i] = vramRead32(off + 4 * i); bar[i] = bar0Read32(off + 4 * i); }
	bool ok = true;
	for (int i = 0; i < 4; i++) ok = ok && mm[i] == pat[i] && bar[i] == pat[i];
	N48LOG("vram: aperture check at vram+0x%llx: wrote %08x %08x %08x %08x; MM window read %08x %08x %08x %08x; "
	       "BAR0 read %08x %08x %08x %08x -> %s (was %08x %08x %08x %08x)",
	       off, pat[0], pat[1], pat[2], pat[3], mm[0], mm[1], mm[2], mm[3], bar[0], bar[1], bar[2], bar[3],
	       ok ? "MATCH" : "MISMATCH", before[0], before[1], before[2], before[3]);
	// 0.0.663 (ReBAR item 4): put back the bytes that were there (0.0.662 zeroed them). The restore is read back through the MM window; a byte that does not come back refuses like a mismatch.
	if (!vramWrite(off, before, sizeof(before))) ok = false;
	for (uint64_t i = 0; i < 4; i++) {
		const uint32_t back = vramRead32(off + 4 * i);
		if (back != before[i]) { N48LOG("vram: aperture check at vram+0x%llx: RESTORE FAILED at dword %llu (wanted %08x, read %08x)", off, i, before[i], back); ok = false; }
	}
	return ok;
}

// 0.0.663 (ReBAR item 4): three points across the window (n48rebar::aperture_points): proves BAR0 offset == VRAM offset at the bottom, the middle and the top of the planned window, BEFORE the allocator exists
// (stagePSP calls this ahead of buildDeviceContext; the visible pool never starts until all three MATCH). Any mismatch, or a degenerate window, refuses.
bool Navi48Bringup::apertureCheckWindow() {
	uint64_t pt[3] = {};
	if (bar0Limit == 0 || !n48rebar::aperture_points(vramBase, bar0Limit, bar0Size, pt)) {
		N48LOG("vram: aperture check: no valid check points for vramBase %#llx vramLimit %#llx mapped %zu; refusing", (unsigned long long)vramBase, (unsigned long long)bar0Limit, bar0Size);
		setProperty("Navi48,ApertureCheck", "mismatch");
		return false;
	}
	if (!n48rebar::layout_ok(bootFbOff == ~0ULL ? n48rebar::kNoConsole : bootFbOff, bootFbLen, vramBase, pt, vramBase + n48rebar::kVisAllocOffset, bar0Limit)) {
		N48LOG("vram: aperture check: vramBase %#llx, a check point or the visible pool overlaps the console [%#llx, +%#llx) or its 64 KiB cursor; refusing before any write", (unsigned long long)vramBase, (unsigned long long)bootFbOff, (unsigned long long)bootFbLen);
		setProperty("Navi48,ApertureCheck", "mismatch");
		return false;
	}
	bool all = true;
	for (int i = 0; i < 3; i++) all = apertureCheck(pt[i]) && all;     // every point is tried and logged even after a mismatch
	N48LOG("vram: aperture checks at vram+[%#llx, %#llx, %#llx] -> %s", (unsigned long long)pt[0], (unsigned long long)pt[1], (unsigned long long)pt[2], all ? "ALL MATCH" : "MISMATCH");
	setProperty("Navi48,ApertureCheck", all ? "match" : "mismatch");
	return all;
}

// ---------------------------------------------------------------------------
// Read-only survey stages
// ---------------------------------------------------------------------------

bool Navi48Bringup::probeMemSize() {
	uint32_t off = n48::kNbifMemsizeByteFallback, discOff;
	if (ipDiscovery.isValid() &&
	    ipDiscovery.regByteOffset(IpDiscovery::HwNbif, 0, n48::kNbifMemsizeSeg, n48::kNbifMemsizeDword, discOff))
		off = discOff;
	vramMB = regRead32(off);
	if (vramMB == 0 || vramMB == 0xFFFFFFFF) {
		N48LOG("mmio: RCC_CONFIG_MEMSIZE read failed (0x%08x) — MMIO not usable", vramMB);
		return false;
	}
	N48LOG("mmio: VRAM size %u MiB (RCC_CONFIG_MEMSIZE @ 0x%x)", vramMB, off);
	setProperty("VRAM,TotalMB", static_cast<uint64_t>(vramMB), 32);
	setProperty("MMIO,Verified", vramMB >= 1024 && vramMB <= 65536);
	return true;
}

bool Navi48Bringup::loadOnDieDiscovery() {
	if (!rmmio || vramMB == 0) return false;
	onDieDisc = static_cast<uint8_t *>(IOMalloc(n48::kDiscTmrSize));
	if (!onDieDisc) return false;
	uint64_t pos = (static_cast<uint64_t>(vramMB) << 20) - n48::kDiscTmrOffset;
	uint32_t *dw = reinterpret_cast<uint32_t *>(onDieDisc);
	for (uint32_t i = 0; i < n48::kDiscTmrSize / 4; i++) dw[i] = vramRead32(pos + 4ULL * i);
	N48LOG("discovery: on-die TMR at vram+0x%llx, first dwords %08x %08x", pos, dw[0], dw[1]);
	if (!ipDiscovery.init(onDieDisc, n48::kDiscTmrSize)) {
		N48LOG("discovery: on-die TMR did not validate");
		IOFree(onDieDisc, n48::kDiscTmrSize); onDieDisc = nullptr;
		return false;
	}
	N48LOG("discovery: on-die binary valid, %u IPs", ipDiscovery.ipCount());
	setProperty("Discovery,Source", "on-die TMR");
	setProperty("Discovery,IPCount", static_cast<uint64_t>(ipDiscovery.ipCount()), 16);
	return true;
}

void Navi48Bringup::surveyIPs() {
	struct { uint16_t id; const char *name; } kWanted[] = {
		{ IpDiscovery::HwGc, "GC" }, { IpDiscovery::HwMmhub, "MMHUB" }, { IpDiscovery::HwMp0, "PSP" },
		{ IpDiscovery::HwMp1, "SMU" }, { IpDiscovery::HwSdma0, "SDMA" }, { IpDiscovery::HwOsssys, "IH" },
		{ IpDiscovery::HwNbif, "NBIF" }, { IpDiscovery::HwDmu, "DCN" }, { IpDiscovery::HwHdp, "HDP" },
		{ IpDiscovery::HwUmc, "UMC" },
	};
	for (auto &w : kWanted) {
		IpDiscovery::IpEntry ip;
		if (!ipDiscovery.findIp(w.id, 0, ip)) continue;
		N48LOG("ip: %-5s v%u.%u.%u  (%u segment base%s)", w.name, ip.major, ip.minor, ip.revision,
		       ip.numBases, ip.numBases == 1 ? "" : "s");
		char key[32], ver[16];
		snprintf(key, sizeof(key), "IP,%s", w.name);
		snprintf(ver, sizeof(ver), "%u.%u.%u", ip.major, ip.minor, ip.revision);
		setProperty(key, ver);
	}
}

void Navi48Bringup::surveyPSP() {
	uint32_t boot = regReadIp(IpDiscovery::HwMp0, 0, 0, n48::kPspC2PMsg35);
	uint32_t sol  = regReadIp(IpDiscovery::HwMp0, 0, 0, n48::kPspC2PMsg81);
	uint32_t ring = regReadIp(IpDiscovery::HwMp0, 0, 0, n48::kPspC2PMsg64);
	N48LOG("psp: boot=0x%08x sos=0x%08x ring64=0x%08x  [bootloader %s, sOS %s, C2PMSG_64 resp-flag %s]",
	       boot, sol, ring, (boot & n48::kPspBootReadyBit) ? "READY" : "not-ready",
	       sol ? "ALIVE" : "down", (ring & n48::kPspRespFlagBit) ? "set" : "clear");
	if (sol && sol != 0xFFFFFFFF) {
		uint32_t v58 = regReadIp(IpDiscovery::HwMp0, 0, 0, n48::kPspFwVer58);
		uint32_t v59 = regReadIp(IpDiscovery::HwMp0, 0, 0, n48::kPspFwVer59);
		N48LOG("psp: sOS alive: C2PMSG_81=0x%08x C2PMSG_58=0x%08x C2PMSG_59=0x%08x", sol, v58, v59);
		setProperty("PSP,SOSSignOfLife", static_cast<uint64_t>(sol), 32);
		setProperty("PSP,Alive", true);
	}
}

void Navi48Bringup::surveySMU() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("navi48-smu", &on, sizeof(on)) || on == 0) return;
	uint32_t regMsg, regParam, regResp;
	if (!ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, n48::kSmuSeg, n48::kSmuMsgDword,   regMsg)   ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, n48::kSmuSeg, n48::kSmuParamDword, regParam) ||
	    !ipDiscovery.regByteOffset(IpDiscovery::HwMp1, 0, n48::kSmuSeg, n48::kSmuRespDword,  regResp)) {
		N48LOG("smu: MP1 mailbox not in discovery table"); return;
	}
	if (regMsg + 4 > rmmioSize || regParam + 4 > rmmioSize || regResp + 4 > rmmioSize) return;
	auto send = [&](uint32_t msgId, uint32_t param, uint32_t *ret) -> uint32_t {
		rmmio[regResp / 4] = 0; rmmio[regParam / 4] = param; rmmio[regMsg / 4] = msgId;
		uint32_t resp = 0;
		for (int i = 0; i < 500; i++) { resp = rmmio[regResp / 4]; if (resp) break; IOSleep(1); }
		if (ret) *ret = rmmio[regParam / 4];
		return resp;
	};
	uint32_t ret = 0;
	if (send(n48::kSmuMsgTestMessage, 0xC0FFEE, &ret) != n48::kSmuRespOK) { N48LOG("smu: TestMessage not acked"); return; }
	N48LOG("smu: PING OK (ret=0x%08x)", ret);
	if (send(n48::kSmuMsgGetSmuVersion, 0, &ret) == n48::kSmuRespOK) {
		N48LOG("smu: PMFW version %u.%u.%u", (ret >> 16) & 0xff, (ret >> 8) & 0xff, ret & 0xff);
		setProperty("SMU,FirmwareVersion", static_cast<uint64_t>(ret), 32);
	}
	setProperty("SMU,Verified", true);
}

// ---------------------------------------------------------------------------
// navi48-psp=1 — FIRST WRITING STAGE: PSP bootloader chain -> SOS alive
// ---------------------------------------------------------------------------

void Navi48Bringup::stagePSP() {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("navi48-psp", &on, sizeof(on)) || on == 0) on = targetStage >= 5;
	if (!on) return;
	N48LOG("psp-stage: requested (navi48-psp=1) — this stage WRITES to VRAM and the PSP mailbox");
	setProperty("PSP,Stage", "requested");

	if (!mapVramAperture())        { N48LOG("psp-stage: no usable BAR0 aperture; aborting"); setProperty("PSP,Stage", "no-bar0"); return; }
	captureBootFramebuffer();      // best effort; chooseVramBase() copes if unknown
	if (!chooseVramBase())         { dropBar0(); setProperty("PSP,Stage", "no-vram-region"); return; }
	if (!finishVramPlan())         { dropBar0(); N48LOG("psp-stage: BAR0 plan refused with the console located; aborting (BAR0 unmapped: the no-BAR0 state)"); setProperty("PSP,Stage", "no-bar0"); return; }
	if (!apertureCheckWindow())    { dropBar0(); N48LOG("psp-stage: aperture check failed — BAR0 offsets are not VRAM offsets here; refusing (BAR0 unmapped: the no-BAR0 state)"); setProperty("PSP,Stage", "aperture-mismatch"); return; }

	HwBridge hw(this);
	n48::PspLoader psp(hw);
	if (!psp.parseContainer(fw_psp_14_0_3_sos, fw_psp_14_0_3_sos_size)) {
		N48LOG("psp-stage: embedded container failed to parse; aborting"); setProperty("PSP,Stage", "bad-container"); return;
	}
	setProperty("PSP,ContainerSOSVersion", static_cast<uint64_t>(psp.sosVersion()), 32);
	if (!psp.setFwPriOffset(vramBase)) { setProperty("PSP,Stage", "bad-fwpri"); return; }
	if (!psp.preflight()) { N48LOG("psp-stage: preflight refused; nothing sent to the PSP"); setProperty("PSP,Stage", "preflight-refused"); return; }
	setProperty("PSP,VramMCBase", psp.mcBase(), 64);
	setProperty("PSP,FwPriMC", psp.fwPriMC(), 64);

	const bool ok = psp.runChain();
	setProperty("PSP,Stage", ok ? "sos-alive" : "chain-failed");
	setProperty("PSP,LastStep", psp.lastStep());
	setProperty("PSP,LastStatus", static_cast<uint64_t>(psp.lastBootloaderStatus()), 32);
	if (ok) N48LOG("psp-stage: SUCCESS — SOS alive (sign-of-life 0x%08x)", psp.lastBootloaderStatus());
	else    N48LOG("psp-stage: FAILED at step %s, bootloader status 0x%08x", psp.lastStep(), psp.lastBootloaderStatus());
	surveyPSP();   // record the live state either way
	if (ok && targetStage > 5) {
		if (buildDeviceContext()) { dev.psoCAlive = true; runStages(targetStage); }
	}
}

// ---------------------------------------------------------------------------
// Compat layer for the ported mac-amdgpu modules (src/amd/*)
// ---------------------------------------------------------------------------

bool Navi48Bringup::buildDeviceContext() {
	// Stages >= 6 DMA into system memory (IH ring, GART-mapped buffers, MES
	// queues): the PCI nub must be a bus master. Memory space is already on.
	pciDevice->setBusMasterEnable(true);
	N48LOG("dev: PCI command 0x%04x (bus master %s)", pciDevice->configRead16(kIOPCIConfigCommand),
	       (pciDevice->configRead16(kIOPCIConfigCommand) & 0x4) ? "ON" : "off");
	dev.rmmio = rmmio; dev.rmmioSize = rmmioSize;
	dev.bar0 = bar0; dev.bar0Size = bar0Size; dev.bar0Phys = bar0Phys;
	dev.vramSizeBytes = static_cast<uint64_t>(vramMB) << 20;
	dev.vramBase  = vramBase;
	dev.vramLimit = bar0Limit;   // 0.0.663 (ReBAR item 3): n48rebar::plan_bar0's clamp (== bar0Size at a 256 MiB BAR0); finishVramPlan has run (stagePSP)
	uint32_t fbBase = regReadIp(IpDiscovery::HwMmhub, 0, 0, 0x0554);
	if (fbBase == 0xFFFFFFFF) { N48LOG("dev: cannot read FB_LOCATION_BASE"); return false; }
	dev.vramMcBase = static_cast<uint64_t>(fbBase & 0x00FFFFFF) << 24;
	if (!amdgpu::fill_ip_base_table(ipDiscovery, dev.ip)) { N48LOG("dev: IP base table incomplete"); return false; }
	N48LOG("dev: context ready — MC base 0x%llx, region vram+[0x%llx, 0x%llx), GC base 0x%x MP0 base 0x%x MMHUB base 0x%x",
	       dev.vramMcBase, dev.vramBase, dev.vramLimit, dev.ip.get(amdgpu::IPBlock::GC),
	       dev.ip.get(amdgpu::IPBlock::MP0), dev.ip.get(amdgpu::IPBlock::MMHUB));
	return true;
}

static amdgpu::BringupContext gBringup;   // one ladder per boot; constructed at kext load

// 0.0.201 — the MM_INDEX/MM_DATA window is three register accesses with no hardware
// atomicity: a second user between the index write and the data access redirects the
// data to (or from) whatever VRAM that user indexed. Linux holds mmio_idx_lock for
// exactly this. At run time our users are the read verbs (vmib, vmstate), the
// pagecopy observer and the residency copy that writes Apple's resource bytes into
// VRAM; the gfxmap poller and the IH handler never touch the window (RREG32/WREG32
// are BAR5-direct, Apple's register writes are blocked). navi48_vram_read_mm and
// navi48_vram_write_mm hold this mutex per <=64-dword batch. Thread context only.
static IOLock *gVramMmLock { nullptr };

// 0.0.433 (an internal design note) — MM-WINDOW PRIORITY FOR THE POLICY PASS, DEFAULT OFF.
// decide36b measured the single-IB policy's descriptor reads at 11.7 ms of its 19.25 ms/run, ~99% of it WAITING for
// gVramMmLock rather than reading (uncontended, the same reads cost ~0.1 ms) — most likely behind our OWN residency
// copier, which released and re-took this same lock every 64 dwords while pushing 492 MiB that boot
// (Navi48AccelPeer.cpp:1297-1312). gMmPrioOwner/gMmPrioNesting track which thread opened gfxsrc_policy's RAII scope
// (Navi48MmPrioScope, Navi48Ttl.hpp) so a NON-owner caller of navi48_vram_read_mm/write_mm can back off and let the
// owner through first — ONLY while gMmPrioOn is set (`accel gfxneuter 37 | M << 8`, the next free selector — 1..36
// were taken, re-checked against AppleHardwareHook.cpp's dispatch chain at HEAD — OFF at boot and OFF by default)
// AND a pass is active (gMmPrioNesting > 0). The owner's own IOLockLock wait is timed UNCONDITIONALLY (the
// instrument is always on; only the yielding is gated by the switch), so the `mmprio:` report line is an honest A/B
// whichever way the switch is thrown. Nesting/owner arithmetic and the yield/bound decisions are gfx_mmprio.h's
// pure, host-tested functions (tests/gfx_mmprio_test.cpp); nothing here writes a register, a page table or anything
// of Apple's, and the reader itself (gfxc_read, gfxc_page, gfxsrc_desc_read, n48_dp_read, gfxsrc_pgm_profile,
// xlat12) is untouched.
static volatile uint32_t gMmPrioOn { 0u };        // the switch: `gfxneuter 37 | 1 << 8` on, `| 0xFF << 8` off
static volatile uint32_t gMmPrioNesting { 0u };   // > 0 while gfxsrc_policy's RAII scope is open (possibly nested)
static uintptr_t gMmPrioOwner { 0u };             // valid only while gMmPrioNesting > 0: the thread that opened it
static n48_mmprio_stats gMmPrioStats {};          // owner-wait / yield counters, fed on every MM call, switch or no

// 0.0.433 — clock_get_uptime ticks to whole microseconds, mirroring AppleHardwareHook.cpp's n48_pol_ns (which
// converts to nanoseconds); microseconds are what IODelay and gfx_mmprio.h's 4 ms bound are already expressed in.
static inline uint64_t navi48_mmprio_us(uint64_t t0, uint64_t t1) {
	uint64_t ns = 0ull;
	if (t1 >= t0) absolutetime_to_nanoseconds(t1 - t0, &ns);
	return ns / 1000ull;
}

// 0.0.433 — opens/closes the MM-window priority scope. Called ONLY from Navi48MmPrioScope's ctor/dtor (gfxsrc_policy's
// whole body, AppleHardwareHook.cpp), so nesting/owner are exact across every return path. The owner is recorded
// only on the 0 -> 1 transition (the outermost scope), never overwritten by a re-entrant call.
void navi48_mm_prio_enter(void) {
	const uint32_t before = gMmPrioNesting;
	if (before == 0u) gMmPrioOwner = (uintptr_t)current_thread();
	gMmPrioNesting = n48_mmprio_enter_nesting(before);
}

void navi48_mm_prio_exit(void) {
	gMmPrioNesting = n48_mmprio_exit_nesting(gMmPrioNesting);   // T4: never underflows past 0
}

// 0.0.433 — `accel gfxneuter 37 | M << 8`: M 1 on, M 0xFF off, mode 0 reads without changing anything. Returns the
// switch's state AFTER this call (1 ON, 0 OFF).
uint32_t navi48_mm_prio_switch(uint32_t mode) {
	if (mode == 1u) gMmPrioOn = 1u;
	else if (mode == 0xFFu) gMmPrioOn = 0u;
	return gMmPrioOn;
}

// 0.0.433 — a plain copy of the live counters for AppleHardwareHook.cpp's always-on report line. No lock: every
// field is written only by navi48_vram_read_mm/write_mm's own caller at the moment it owns the fact it is updating,
// the same as every other read-only counter block in this kext (e.g. AppleHardwareHook.cpp's gMibPol).
void navi48_mm_prio_snapshot(n48_mmprio_stats *out) {
	if (out) *out = gMmPrioStats;
}

// build 0.0.514 A3 — WHO HOLDS gVramMmLock, AND FOR HOW LONG: READ-ONLY. Each MM call notes its hold
// (from its IOLockLock returning to just after its IOLockUnlock) under the calling thread's taker (gfx_mmhold.h: the innermost
// Navi48MmTakerScope open on it, else OTHER); the fast copy's verify readers name themselves. Printed by navi48_fc_report (the
// bare `gfxneuter 63`). Relaxed atomic counters, a CAS-claimed per-thread table: no lock, no log, nothing written but these.
static n48_mmt_table gMmTk {};
static n48_mmhold_stats gMmHold {};
uint32_t navi48_mm_taker_push(uint32_t tag) { return n48_mmt_push(&gMmTk, (uintptr_t)current_thread(), tag); }
void navi48_mm_taker_pop(uint32_t tok) { n48_mmt_pop(&gMmTk, (uintptr_t)current_thread(), tok); }
// build 0.0.515 D2: a DECIDE sub-tag (gfx_mmhold.h n48_mmt_push_sub: a no-op unless this thread's open tag is
// DECIDE or a DECIDE sub-tag). Called ONLY by Navi48MmSubScope (Navi48Ttl.hpp); the pop is navi48_mm_taker_pop.
uint32_t navi48_mm_taker_push_sub(uint32_t sub) { return n48_mmt_push_sub(&gMmTk, (uintptr_t)current_thread(), sub); }
// =============================================================================================================================
// build 0.0.540 ( and T9; apple/gfx_perf540.h) — SWITCH 96, perf540: the two accumulators that live here. OFF
// (the default, the boot value) navi48_vram_read_mm pays ONE load of the switch and n48_logf pays ONE load; nothing else runs.
// ON: navi48_vram_read_mm's WHOLE call (the mmprio spin, the lock wait, the reads) by the caller's MM tag, and n48_logf's
// vsnprintf / IOLog / ring time by thread class (a registered GFX-hook thread or any other). Relaxed atomics; no lock, no log.
volatile uint32_t gN48Pf540On { 0u };                    // the switch: written ONLY by `gfxneuter 96` (AppleHardwareHook.cpp)
alignas(64) n48_wc_gen gN48WcGen {};                       // build 0.0.541: switch 98's generation (gfx_wc98.h); 0.0.542: its own
                                                           // cache lines (the 0.0.541 review: it shared one with gN48Pf540On)
static n48_pf_ext gPfExt {};
static volatile uintptr_t gPfHookThr[4] {};               // GFX-hook threads while ON (a hook call's enter/exit)
uint32_t navi48_mm_taker_now(void) { return n48_mmt_lookup(&gMmTk, (uintptr_t)current_thread()); }
void navi48_pf540_ext_snapshot(n48_pf_ext *out) {
	if (!out) return;
	const uint64_t *a = reinterpret_cast<const uint64_t *>(&gPfExt);
	uint64_t *o = reinterpret_cast<uint64_t *>(out);
	for (uint32_t i = 0; i < sizeof(n48_pf_ext) / sizeof(uint64_t); i++) o[i] = __atomic_load_n(&a[i], __ATOMIC_RELAXED);
}
uint32_t navi48_pf540_hook_enter(void) {
	const uintptr_t me = (uintptr_t)current_thread();
	for (uint32_t i = 0; i < 4u; i++) {
		uintptr_t want = 0u;
		if (__atomic_compare_exchange_n(&gPfHookThr[i], &want, me, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return i + 1u;
	}
	return 0u;   // five hook calls at once: this one's lines count as another thread's
}
void navi48_pf540_hook_exit(uint32_t slot) {
	if (slot >= 1u && slot <= 4u) __atomic_store_n(&gPfHookThr[slot - 1u], (uintptr_t)0u, __ATOMIC_RELAXED);
}
static inline uint64_t pf540_ns(uint64_t t0, uint64_t t1) {
	uint64_t ns = 0ull;
	if (t1 >= t0) absolutetime_to_nanoseconds(t1 - t0, &ns);
	return ns;
}
// navi48_vram_read_mm, ON only: `t0` is the call's start (after the argument checks), the tag is the caller's innermost.
static __attribute__((noinline)) void pf540_mm_note(uint64_t t0, uint32_t dwords) {
	uint64_t t1 = 0ull;
	clock_get_uptime(&t1);
	uint32_t tag = n48_mmt_lookup(&gMmTk, (uintptr_t)current_thread());
	if (tag >= N48_PF_MM_TAGS) tag = N48_MMT_OTHER;
	__atomic_fetch_add(&gPfExt.mm_n[tag], 1ull, __ATOMIC_RELAXED);
	__atomic_fetch_add(&gPfExt.mm_ns[tag], pf540_ns(t0, t1), __ATOMIC_RELAXED);
	__atomic_fetch_add(&gPfExt.mm_dw[tag], (uint64_t)dwords, __ATOMIC_RELAXED);
}
// navi48_cg_seg_check, ON only.
static __attribute__((noinline)) void pf540_cg_note(uint64_t t0) {
	uint64_t t1 = 0ull;
	clock_get_uptime(&t1);
	__atomic_fetch_add(&gPfExt.cg_n, 1ull, __ATOMIC_RELAXED);
	__atomic_fetch_add(&gPfExt.cg_ns, pf540_ns(t0, t1), __ATOMIC_RELAXED);
}
// navi48_vram_read_mm's RAII: OFF one load of the switch and no clock; ON the call's start (after the argument checks) and, at its one
// `return true` (after the hold's own note, so the MM-hold pins' three-line tail is untouched), the note by the caller's MM tag.
struct Pf540MmCall {
	explicit Pf540MmCall(uint32_t dw) : t0_(0ull), dw_(dw) { if (__atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED)) clock_get_uptime(&t0_); }
	~Pf540MmCall() { if (t0_) pf540_mm_note(t0_, dw_); }
	Pf540MmCall(const Pf540MmCall &) = delete;
	Pf540MmCall &operator=(const Pf540MmCall &) = delete;
private:
	uint64_t t0_;
	uint32_t dw_;
};
// amd/n48log.cpp n48_logf, ON only: its four clock readings (entry, after vsnprintf, after IOLog, after the ring).
void navi48_pf540_log_note(uint64_t t0, uint64_t t1, uint64_t t2, uint64_t t3) {
	const uintptr_t me = (uintptr_t)current_thread();
	uint32_t k = 1u;
	for (uint32_t i = 0; i < 4u; i++) if (__atomic_load_n(&gPfHookThr[i], __ATOMIC_RELAXED) == me) { k = 0u; break; }
	__atomic_fetch_add(&gPfExt.lg_n[k], 1ull, __ATOMIC_RELAXED);
	__atomic_fetch_add(&gPfExt.lg_vsn[k], pf540_ns(t0, t1), __ATOMIC_RELAXED);
	__atomic_fetch_add(&gPfExt.lg_io[k], pf540_ns(t1, t2), __ATOMIC_RELAXED);
	__atomic_fetch_add(&gPfExt.lg_ring[k], pf540_ns(t2, t3), __ATOMIC_RELAXED);
}
// After IOLockUnlock: `h0` is the uptime at which the lock was acquired. noinline: the MM functions gain one local, not this body.
// build 0.0.515 D2: and the dwords the held call moved (0 from the fast copy's verify readers, which count their own).
static __attribute__((noinline)) void mm_hold_note(uint64_t h0, uint32_t tag, uint32_t dwords = 0u) {
	uint64_t h1 = 0ull, ns = 0ull;
	clock_get_uptime(&h1);
	if (h1 >= h0) absolutetime_to_nanoseconds(h1 - h0, &ns);
	if (tag == N48_MMT_LOOKUP) tag = n48_mmt_lookup(&gMmTk, (uintptr_t)current_thread());
	n48_mmhold_note_dw(&gMmHold, tag, ns, dwords);
}

// 0.0.435 (an internal design note Part 2) — THE COPY-OVERLAP REFUSAL'S STORAGE. All-zero at
// kext load is already each structure's own valid empty state (n48_cg_slot_init/ring_init/poison_init would write
// exactly these bytes), so no explicit init call is needed at boot. No lock: every field is either touched only
// under the __atomic ops gfx_copyguard.h's own functions use (the slot table, the ring, the poison table) or, like
// gMmPrioStats above, written only by the caller that owns the fact it is updating (gCgStats, gCgUntracked's own
// increments are the one place two residency copies on different threads could race a lost count on the same boot
// as gVramMmLock already serialises their MM access, not their OPEN/CLOSE bookkeeping - acceptable for a report-only
// counter, the same tradeoff this file already makes for gMmPrioStats).
static n48_cg_slot   gCgSlots[N48_CG_SLOTS] {};
static n48_cg_ring    gCgRing {};
static n48_cg_poison  gCgPoison {};
static n48_cg_stats   gCgStats {};
static volatile uint32_t gCgUntracked { 0u };   // copies currently running with no free slot: every check refuses
// The ACTIVE per-segment page recorder and its ring mark, set by navi48_cg_seg_begin (called once per segment,
// AppleHardwareHook.cpp, immediately before xlat12_ib_translate_draw_ex) and read by navi48_cg_seg_check right
// after it. Single-threaded by construction: gfxsrc_policy's whole pass already runs under gXdLock's try-lock.
static n48_cg_pagerec gCgSegRec {};
static uint64_t gCgSegSince { 0ull };

// =============================================================================================================================
// build 0.0.529 (an internal design note, ; apple/gfx_cg84.h) — SWITCH 84's STORAGE, LOCK AND GLUE. The pure
// halves are gfx_copyguard.h (granules, n48_cg_rec_hit, n48_cg_check_fx) and gfx_cg84.h (the keys, the plan, the update, the
// invalidations). Here: the mode (OFF at boot, written only by navi48_cg84_switch), the four keys' shadows and the scratch (5 x 32
// KiB, allocated on the FIRST transition to SHADOW or ON, on the verb thread, never freed), the LEAF lock gD84Lock (nothing of ours
// is taken while it is held; the backing's readBytes runs under it), and the reader-side counters. Before the first SHADOW/ON the
// copier and the event sites pay ONE load (gN48D84Live 0); navi48_cg_open pays the same one load.
// =============================================================================================================================
volatile uint32_t gN48D84Live { 0u };                  // 1 once the memory exists (sticky for the boot)
volatile uintptr_t gN48D84SrcThr { 0u };               // the thread whose ON copy writes from the scratch (ic_read / fc_copy_chunk)
static volatile uint32_t gCg84Mode { N48_CG84_OFF };   // switch 84; OFF at boot; written only by navi48_cg84_switch
static n48_d84_tab gD84T {};                           // under gD84Lock (the keys' vram/bytes/state/pend/pthr/inval: atomics)
static uint8_t *gD84Pool { nullptr };                  // (N48_D84_KEYS + 1) x N48_D84_MAX_BYTES
static IOLock *gD84Lock { nullptr };                   // LEAF
static n48_cg84_stats gCg84S {};                       // the reader's counters (gfxsrc_policy's pass: single-threaded under gXdLock)
uint32_t navi48_cg84_mode(void) { return gCg84Mode; }

// =============================================================================================================================
// build 0.0.527 (an internal design note, ; apple/gfx_sk82.h) — SWITCH 82's STORAGE, LOCK AND GLUE. The pure
// half (the decision, the establishment, the counters' arithmetic, the event ring) is gfx_sk82.h. Here: the memory (allocated on
// the first transition to MEASURE or SKIP, from the verb thread, never in the copy path: X4), the leaf lock gSk82Lock (nothing of
// ours is taken while it is held; the backing's readBytes runs under it, as SKIP82.md item 7 specifies), and the calls the copier
// (Navi48AccelPeer.cpp) and the event sites (AppleHardwareHook.cpp) make. Before the first ON the counter array does not exist:
// navi48_cg_open/close then pay ONE load of a null pointer (gSk82Cnt) and the event sites one load of gN48Sk82Live (0).
// =============================================================================================================================
volatile uint32_t gN48Sk82Live { 0u };                  // 1 once the memory exists (sticky for the boot): the event sites' gate
volatile uint32_t gN48Sk82On { 0u };                    // 1 while MEASURE or SKIP (the copier's gate; re-checked under the lock)
static volatile uint32_t gSk82Mode { N48_SK82_OFF };    // switch 82; OFF at boot; written only by navi48_sk82_switch, under the lock
static uint32_t *gSk82Cnt { nullptr };                  // one u32 per 64 KiB of VRAM; published (release) after it was zeroed
static uint64_t gSk82Nb { 0ull };                       // its length in buckets (set before the publish)
static uint64_t gSk82Every { 0ull };                    // the everything counter
static uint8_t *gSk82Pool { nullptr };                  // N48_SK82_ENTRIES x 64 KiB images (1 MiB)
static uint8_t *gSk82Scratch { nullptr };               // the 64 KiB compare scratch (the source, read under the lock)
static IOLock *gSk82Lock { nullptr };                   // LEAF
static n48_sk82_tab gSk82T {};                          // under gSk82Lock
static n48_sk82_evring gSk82Ev {};                      // lock-free producers, drained under gSk82Lock
static volatile uint64_t gSk82LastUnknownUs { 0ull };   // uptime of the last unknown-write event (| 1: never 0 once set)
static volatile uint64_t gSk82FullForced { 0ull };      // copies whose fast-copy latch sk82 asked for FULL

static uint64_t sk82_now_us() {
	uint64_t t = 0ull, ns = 0ull;
	clock_get_uptime(&t);
	absolutetime_to_nanoseconds(t, &ns);
	return ns / 1000ull;
}
// gfx_sk82.h's BUSY: a committed frame still in the flight ring, or an unknown-write event in the last N48_SK82_QUIET_US.
static uint32_t sk82_busy(void *) {
	if (n48::hw_sk82_flight_live()) return 1u;
	const uint64_t last = __atomic_load_n(&gSk82LastUnknownUs, __ATOMIC_SEQ_CST);
	const uint64_t now = sk82_now_us();
	return (last && (now < last || now - last < N48_SK82_QUIET_US)) ? 2u : 0u;
}
static void sk82_world(n48_sk82_world *w) {
	w->cnt = __atomic_load_n(&gSk82Cnt, __ATOMIC_ACQUIRE); w->nb = gSk82Nb; w->every = &gSk82Every;
	w->slots = gCgSlots; w->nslots = N48_CG_SLOTS; w->untracked = const_cast<const uint32_t *>(&gCgUntracked);
	w->poison = &gCgPoison; w->ev = &gSk82Ev; w->busy = &sk82_busy; w->busyCtx = nullptr;
}

// An invalidation event (AppleHardwareHook.cpp, only while gN48Sk82Live). Any thread, no lock. The unknown-write kinds also
// stamp the quiet window FIRST, so a G0 that follows the event's drain sees it as recent.
void navi48_sk82_ev(uint32_t kind, uint64_t va, uint64_t size) {
	if (kind == N48_SK82_EV_COMMIT || kind == N48_SK82_EV_RUN || kind == N48_SK82_EV_SDMA)
		__atomic_store_n(&gSk82LastUnknownUs, sk82_now_us() | 1ull, __ATOMIC_SEQ_CST);
	(void)n48_sk82_ev_push(&gSk82Ev, kind, va, size);
}

// THE DECISION (SKIP82.md item 7), called once per residency copy by Navi48AccelPeer.cpp's sk82_try_skip, BEFORE rp_lin_prepare
// and before the copy's scope opens (G0). Under the lock: the whole source is read into the scratch buffer and compared, never
// sampled. 1 = SKIP (the copier returns true at once: no scope, no slot, no provenance, no gCopy change, no COPIED line).
uint32_t navi48_sk82_try(const n48_sk82_key *k, const n48_sk82_snap *sn, void *md, uint64_t copyNo, uint32_t eligible,
                         n48_sk82_out *o) {
	*o = n48_sk82_out {};
	if (!gSk82Lock || !gSk82Scratch) return 0u;
	n48_sk82_world w {};
	sk82_world(&w);
	IOMemoryDescriptor *d = static_cast<IOMemoryDescriptor *>(md);
	IOLockLock(gSk82Lock);
	const uint32_t mode = gSk82Mode;
	const uint8_t *src = nullptr;
	if (mode != N48_SK82_OFF && eligible && d && k->bytes && k->bytes <= N48_SK82_MAX_BYTES) {
		if (d->readBytes(k->boff, gSk82Scratch, k->bytes) == k->bytes) src = gSk82Scratch;
		else gSk82T.st.readFail++;
	}
	if (mode != N48_SK82_OFF) n48_sk82_try(&gSk82T, &w, mode, k, sn, src, (uintptr_t)current_thread(), copyNo, md, o);
	IOLockUnlock(gSk82Lock);
	return o->act == N48_SK82_A_SKIP ? 1u : 0u;
}
// The copier's result (before its scope closes), a foreign write inside the copy's scope, and the per-thread FULL flag.
void navi48_sk82_result(uint32_t ok, uint64_t wBytes) {
	if (!gSk82Lock) return;
	IOLockLock(gSk82Lock);
	n48_sk82_result(&gSk82T, (uintptr_t)current_thread(), ok, wBytes);
	IOLockUnlock(gSk82Lock);
}
void navi48_sk82_taint_mine(void) {
	if (!gSk82Lock) return;
	IOLockLock(gSk82Lock);
	n48_sk82_taint(&gSk82T, (uintptr_t)current_thread());
	IOLockUnlock(gSk82Lock);
}
uint32_t navi48_sk82_cand_mine(void) {
	if (!gSk82Lock) return 0u;
	IOLockLock(gSk82Lock);
	const uint32_t c = n48_sk82_cand(&gSk82T, (uintptr_t)current_thread());
	IOLockUnlock(gSk82Lock);
	if (c) __atomic_fetch_add(&gSk82FullForced, 1ull, __ATOMIC_RELAXED);
	return c;
}
// THE ESTABLISHMENT (item 6), from navi48_cg_close_scope AFTER navi48_cg_close: the scope is closed (its END pushed, its poison
// decided, its slot released). The source is re-read and must equal the image stored at G0.
void navi48_sk82_closed(uint64_t lo, uint64_t hi, const n48_sk82_snap *sn) {
	if (!gSk82Lock || !gSk82Scratch) return;
	n48_sk82_world w {};
	sk82_world(&w);
	const uintptr_t me = (uintptr_t)current_thread();
	IOLockLock(gSk82Lock);
	const int32_t s = n48_sk82_thr_find(&gSk82T, me);
	if (s < 0 || gSk82T.t[s].lo != lo || gSk82T.t[s].hi != hi || gSk82Mode == N48_SK82_OFF) { IOLockUnlock(gSk82Lock); return; }
	const n48_sk82_thr h = gSk82T.t[s];
	IOMemoryDescriptor *d = static_cast<IOMemoryDescriptor *>(h.md);
	const uint64_t bytes = h.hi - h.lo;
	const uint8_t *post = nullptr;
	if (h.ok == 1u && !h.taint && d && bytes && bytes <= N48_SK82_MAX_BYTES && d->readBytes(h.boff, gSk82Scratch, bytes) == bytes)
		post = gSk82Scratch;
	uint32_t cause = N48_SK82_CAUSES;
	(void)n48_sk82_establish(&gSk82T, &w, me, lo, hi, sn, post, &cause);
	IOLockUnlock(gSk82Lock);
}

// X4: the 1 MiB of images, the 64 KiB scratch and the counters (one u32 per 64 KiB of VRAM), on the FIRST transition to MEASURE or
// SKIP, from the verb thread. Any failure frees what was taken and the switch refuses (stays OFF). Published once, never freed.
static bool sk82_alloc() {
	if (__atomic_load_n(&gSk82Cnt, __ATOMIC_ACQUIRE)) return true;
	const uint64_t vram = gBringup.dev ? gBringup.dev->vramSizeBytes : 0ull;
	const uint64_t nb = vram >> N48_SK82_BSHIFT;
	if (!nb || nb > (1ull << 20)) return false;
	const size_t poolB = (size_t)N48_SK82_ENTRIES * N48_SK82_MAX_BYTES, cntB = (size_t)nb * sizeof(uint32_t);
	IOLock *l = IOLockAlloc();
	uint8_t *pool = static_cast<uint8_t *>(IOMalloc(poolB));
	uint8_t *scr = static_cast<uint8_t *>(IOMalloc(N48_SK82_MAX_BYTES));
	uint32_t *cnt = static_cast<uint32_t *>(IOMalloc(cntB));
	if (!l || !pool || !scr || !cnt) {
		if (l) IOLockFree(l);
		if (pool) IOFree(pool, poolB);
		if (scr) IOFree(scr, N48_SK82_MAX_BYTES);
		if (cnt) IOFree(cnt, cntB);
		return false;
	}
	memset(pool, 0, poolB); memset(scr, 0, N48_SK82_MAX_BYTES); memset(cnt, 0, cntB);
	for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++) gSk82T.e[i].img = pool + (size_t)i * N48_SK82_MAX_BYTES;
	gSk82Pool = pool; gSk82Scratch = scr; gSk82Nb = nb; gSk82Lock = l;
	__atomic_store_n(&gSk82Cnt, cnt, __ATOMIC_RELEASE);
	__atomic_store_n(&gN48Sk82Live, 1u, __ATOMIC_RELEASE);
	return true;
}
static void sk82_report(const char *how) {
	n48_sk82_stats s {};
	uint32_t mode = gSk82Mode;
	if (gSk82Lock) { IOLockLock(gSk82Lock); s = gSk82T.st; mode = gSk82Mode; IOLockUnlock(gSk82Lock); }
	s.fullForced = __atomic_load_n(&gSk82FullForced, __ATOMIC_RELAXED);
	N48LOG(N48_SK82_REPORT1_FMT, n48_sk82_mode_name(mode), how, (unsigned long long)s.considered, (unsigned long long)s.eligible,
	       (unsigned long long)s.noEntry, (unsigned long long)s.equal, (unsigned long long)s.differ, (unsigned long long)s.skipped,
	       (unsigned long long)s.skippedBytes, (unsigned long long)s.wouldSkip, (unsigned long long)s.established,
	       (unsigned long long)s.candidate, (unsigned long long)s.fullForced, (unsigned long long)s.evicted,
	       (unsigned long long)s.notFull, __atomic_load_n(&gSk82Cnt, __ATOMIC_ACQUIRE) ? "allocated" : "not allocated");
	N48LOG(N48_SK82_REPORT2_FMT, N48_SK82_REPORT2_ARGS(&s));
}
// `gfxneuter 82 | M << 8` (AppleHardwareHook.cpp's selector, which computes the continuous-arm guard). Returns the mode after the
// call. Every change of mode resets the table (entries FREE, pendings gone, the event mark at the ring's head).
uint32_t navi48_sk82_switch(uint32_t m, uint32_t contRefused, uint32_t *st) {
	const uint32_t want = n48_sk82_mode_of_m(m);
	const char *how = " - read only";
	if (contRefused) { *st = 5u; how = " - REFUSED: a continuous arm stands"; }
	else if (want == N48_SK82_M_BAD) { *st = 11u; how = " - REFUSED (unknown M), unchanged"; }
	else if (want != N48_SK82_M_READ) {
		if (want != N48_SK82_OFF && !sk82_alloc()) {
			*st = 12u; how = " - REFUSED: no memory, stays OFF";
		} else if (gSk82Lock) {
			IOLockLock(gSk82Lock);
			if (gSk82Mode != want) {
				n48_sk82_reset(&gSk82T, &gSk82Ev);
				gSk82Mode = want;
				__atomic_store_n(&gN48Sk82On, want != N48_SK82_OFF ? 1u : 0u, __ATOMIC_RELEASE);
				// a quiet window after every change: SDMA / ring work submitted before it cannot be dated
				__atomic_store_n(&gSk82LastUnknownUs, sk82_now_us() | 1ull, __ATOMIC_SEQ_CST);
			}
			IOLockUnlock(gSk82Lock);
			how = " - CHANGED BY THIS VERB";
		} else {
			how = " - CHANGED BY THIS VERB";   // OFF, never allocated: nothing to reset
		}
	}
	sk82_report(how);
	return gSk82Mode;
}

// build 0.0.528 (; apple/gfx_tlb83.h) — `gfxneuter 83 | M << 8`. M 1 ON (339), M 2 OFF (595, the default and the
// boot value), bare 83 reads, any other M refused unchanged (st 11), a continuous arm refuses a change (st 5), ON without the leaf lock
// refuses (st 12, stays OFF). The ONLY writer of amdgpu::gTlb83On. It writes no register, no page table and nothing of Apple's.
uint32_t navi48_tlb83_switch(uint32_t m, uint32_t contRefused, uint32_t *st) {
	const uint32_t want = n48_tlb83_mode_of_m(m);
	const char *how = " - read only";
	if (contRefused) { *st = 5u; how = " - REFUSED: a continuous arm stands, unchanged"; }
	else if (want == N48_TLB83_M_BAD) { *st = 11u; how = " - REFUSED (unknown M), unchanged"; }
	else if (want != N48_TLB83_M_READ) {
		if (want == N48_TLB83_ON && !amdgpu::gTlb83Lock) { *st = 12u; how = " - ON REFUSED: no leaf lock, unchanged"; }
		else { __atomic_store_n(&amdgpu::gTlb83On, want, __ATOMIC_RELEASE); how = " - CHANGED BY THIS VERB"; }
	}
	const uint32_t now = __atomic_load_n(&amdgpu::gTlb83On, __ATOMIC_ACQUIRE);
	N48LOG(N48_TLB83_REPORT1_FMT, N48_TLB83_REPORT1_ARGS(&amdgpu::gTlb83S, n48_tlb83_mode_name(now), how));
	N48LOG(N48_TLB83_REPORT2_FMT, N48_TLB83_REPORT2_ARGS(&amdgpu::gTlb83S));
	N48LOG(N48_TLB83_REPORT3_FMT, N48_TLB83_REPORT3_ARGS(&amdgpu::gTlb83S, amdgpu::gTlb83Lock ? "present" : "MISSING"));
	return now;
}

// =============================================================================================================================
// build 0.0.529 (an internal design note; apple/gfx_cg84.h) — SWITCH 84's WRITER GLUE. Every function below takes gD84Lock (a
// LEAF: only the backing's readBytes runs under it) and returns at once while the memory does not exist.
// =============================================================================================================================
static void d84_world(n48_d84_world *w) {
	w->slots = gCgSlots; w->nslots = N48_CG_SLOTS; w->untracked = const_cast<const uint32_t *>(&gCgUntracked);
	w->poison = &gCgPoison; w->step = nullptr; w->stepCtx = nullptr;
}
struct D84Src { IOMemoryDescriptor *md; uint64_t boff; };
static int d84_read_src(void *c, uint8_t *dst, uint64_t n) {
	const D84Src *x = static_cast<const D84Src *>(c);
	return (x->md && n <= N48_D84_MAX_BYTES && x->md->readBytes(x->boff, dst, n) == n) ? 1 : 0;
}
// G0 (CG84.md item 6): the copier (Navi48AccelPeer.cpp d84_plan) after the pre-flight and before the copy's scope. Returns 0 (the copy
// is 0.0.528's: OFF, not eligible, no key, busy, SHADOW, or any cause MF-4 does not act on) or, ON with a delta, (1 << 63) | ds << 32 | de.
// Under ON an ESTABLISHING copy also returns 0 but sets gN48D84SrcThr: its source is the scratch and its fast copy is declined.
// S1: CLAIM under gD84Lock, the 32 KiB readBytes with the lock DROPPED (into the scratch the claim made this thread's), DECIDE under
// the lock again (c0, THEN the slots: D1).
static volatile uint64_t gD84LastUnknownUs { 0ull };   // MF-1: uptime (us, | 1) of the last unknown write (commit, SDMA, un-NOPed IB)
uint64_t navi48_d84_plan(const n48_d84_elig *e, const n48_d84_id *id, const n48_sk82_snap *sn, void *md, uint64_t cgLo, uint64_t cgHi,
                         uint64_t dirtyLo, uint64_t dirtyHi) {
	if (!gD84Lock || !e || !id || !sn) return 0ull;
	n48_d84_world w {};
	d84_world(&w);
	n48_d84_in in {};
	in.elig = n48_d84_eligible(e); in.wsPid = n48::hw_d84_ws_pid(); in.nowUs = sk82_now_us();
	in.lastUnknownUs = __atomic_load_n(&gD84LastUnknownUs, __ATOMIC_SEQ_CST);
	in.progs = &navi48_d84_programs_in; in.progsCtx = nullptr;
	in.cgLo = cgLo; in.cgHi = cgHi; in.dirtyLo = dirtyLo; in.dirtyHi = dirtyHi;
	const uintptr_t me = (uintptr_t)current_thread();
	n48_d84_plan_out o {};
	IOLockLock(gD84Lock);
	in.mode = gCg84Mode;
	const int32_t ki = n48_d84_claim(&gD84T, &in, id, me, &o);                                  // (1) CLAIM
	IOLockUnlock(gD84Lock);
	if (ki < 0) return 0ull;
	D84Src src { static_cast<IOMemoryDescriptor *>(md), id->boff };
	const int rdOk = d84_read_src(&src, gD84T.scratch, id->bytes);                            // (2) READ, the lock dropped
	uint32_t act = 0u;
	IOLockLock(gD84Lock);
	if (!rdOk) {
		n48_d84_unclaim(&gD84T, ki, me);
		gD84T.st.full++; gD84T.st.readFail++; gD84T.st.cause[N48_D84_C_BIG]++;
	} else {
		act = n48_d84_decide(&gD84T, &w, &in, id, sn, ki, me, &o);                              // (3) DECIDE: c0, THEN the slots
	}
	if (act && o.kind == N48_D84_K_EST) __atomic_store_n(&gN48D84SrcThr, me, __ATOMIC_SEQ_CST);
	IOLockUnlock(gD84Lock);
	if (act && o.kind == N48_D84_K_DELTA) return (1ull << 63) | (o.ds << 32) | o.de;
	return 0ull;
}
void navi48_d84_result(uint32_t ok, uint64_t compared, uint64_t mismatched, uint64_t wBytes) {
	if (!gD84Lock) return;
	IOLockLock(gD84Lock);
	n48_d84_result(&gD84T, (uintptr_t)current_thread(), ok, compared, mismatched, wBytes);
	IOLockUnlock(gD84Lock);
}
void navi48_d84_census(uint32_t programs) {
	if (!gD84Lock) return;
	IOLockLock(gD84Lock);
	n48_d84_census(&gD84T, (uintptr_t)current_thread(), programs);
	IOLockUnlock(gD84Lock);
}
// MF-6: the probe's read-back batch (the probing thread still owns the pending record, so one probe at a time: a file-scope static).
static uint32_t gD84ProbeBuf[64];
// Returns the first differing offset, or ~0 when VRAM equals the shadow over the whole key (the lock dropped; the key is pending).
static __attribute__((noinline)) uint64_t d84_probe(const n48_d84_key *k) {
	for (uint64_t off = 0; off < k->id.bytes; off += 256u) {
		const uint32_t n = (uint32_t)((k->id.bytes - off >= 256u ? 256u : k->id.bytes - off) / 4u);
		if (!navi48_vram_read_mm(k->id.vram + off, gD84ProbeBuf, n)) return off;   // unreadable: treated as a mismatch
		const uint64_t d = n48_d84_probe_cmp(k->shadow, off, gD84ProbeBuf, n);
		if (d != ~0ull) return d;
	}
	return ~0ull;
}
// AFTER the copy's scope closed (navi48_cg_close_scope, after navi48_cg_close): the update, and the scratch's release. S1: the 32 KiB
// shadow copy runs with gD84Lock DROPPED (the key stays pending on this thread: nothing else reads or takes it meanwhile).
void navi48_d84_closed(uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {
	if (!gD84Lock) return;
	n48_d84_world w {};
	d84_world(&w);
	const uintptr_t me = (uintptr_t)current_thread();
	IOLockLock(gD84Lock);
	uint32_t go = 0u;
	// A scope of THIS thread that is not the pending copy's (never expected: the copy's writes are all inside its own scope) ends the
	// pending copy fail-safe: the key is invalidated and the scratch and the source redirect are released.
	if (gD84T.p.thr == me && (gD84T.p.lo != lo || gD84T.p.hi != hi)) n48_d84_abandon(&gD84T, me);
	else go = n48_d84_closed_begin(&gD84T, &w, me, lo, hi, wrote ? 1u : 0u, failed ? 1u : 0u, mismatch ? 1u : 0u);
	const uint32_t probeNow = go && gD84T.p.act && gD84T.p.kind == N48_D84_K_DELTA && n48_d84_probe_due(gD84T.st.deltaUpd);
	IOLockUnlock(gD84Lock);
	if (go) {
		n48_d84_shadow_copy(&gD84T);                                                             // the lock dropped
		uint64_t bad = ~0ull;
		const n48_d84_key *k = &gD84T.k[gD84T.p.key % N48_D84_KEYS];
		if (probeNow) bad = d84_probe(k);                                                        // MF-6
		IOLockLock(gD84Lock);
		if (probeNow) gD84T.st.probes++;
		const uint64_t probes = gD84T.st.probes, dup = gD84T.st.deltaUpd;
		(void)n48_d84_closed_end(&gD84T, me, bad != ~0ull ? 1u : 0u);
		if (bad != ~0ull) { gD84T.st.probeBad++; (void)n48_d84_bump_all(gD84T.k, N48_D84_KEYS); n48_d84_inval_all(&gD84T); }
		IOLockUnlock(gD84Lock);
		if (bad != ~0ull)
			N48LOG(N48_D84_PROBE_FMT, gD84T.p.key % N48_D84_KEYS, (unsigned long long)k->id.res, (unsigned long long)k->id.vram,
			       (unsigned long long)(k->id.vram + k->id.bytes), (unsigned long long)bad, (unsigned long long)probes,
			       (unsigned long long)dup);
	}
	IOLockLock(gD84Lock);
	if (!gD84T.p.thr && __atomic_load_n(&gN48D84SrcThr, __ATOMIC_SEQ_CST) == me)
		__atomic_store_n(&gN48D84SrcThr, (uintptr_t)0, __ATOMIC_SEQ_CST);
	IOLockUnlock(gD84Lock);
}
// The plan-time image (the scratch): owned by the pending copier from its G0 to its post-close; never freed.
const uint8_t *navi48_d84_scratch(void) { return gD84T.scratch; }
// ic_read's source for THIS thread's ON copy: resource bytes [resOff, resOff + take) of the scratch.
uint64_t navi48_d84_src(uint64_t resOff, uint8_t *dst, uint64_t take) {
	const uint8_t *s = gD84T.scratch;
	if (!s || resOff > N48_D84_MAX_BYTES || take > N48_D84_MAX_BYTES - resOff) return 0ull;
	memcpy(dst, s + resOff, (size_t)take);
	return take;
}
// S2: THE INVALIDATION PRODUCERS ARE LOCK-FREE - every non-FREE key's counter moves (n48_d84_bump_all); gD84Lock is never taken here.
static inline void d84_bump_all_counted(uint64_t *ctr) {
	const uint32_t moved = n48_d84_bump_all(gD84T.k, N48_D84_KEYS);
	if (ctr) __atomic_fetch_add(ctr, 1ull, __ATOMIC_RELAXED);
	if (moved) __atomic_fetch_add(&gD84T.st.bumps, (uint64_t)moved, __ATOMIC_RELAXED);
}
void navi48_d84_pageout(const void *res) { (void)res; d84_bump_all_counted(&gD84T.st.invalRes); }
void navi48_d84_unmap(uint64_t va, uint64_t size) { (void)va; (void)size; d84_bump_all_counted(&gD84T.st.invalUnmap); }
void navi48_d84_inval_all(void) { d84_bump_all_counted(&gD84T.st.invalAll); }
// A frame the COMMIT gate answered yes for (MF-2): its shader / UAV stores are not enumerated, so EVERY key moves (switch 82's
// unknown-write rule) and the 100 ms window starts (MF-1: the frame runs after the gate). Both modes (S2).
void navi48_d84_commit(void) {
	__atomic_store_n(&gD84LastUnknownUs, sk82_now_us() | 1ull, __ATOMIC_SEQ_CST);
	d84_bump_all_counted(&gD84T.st.commits);
}
// MF-1 / S3: an Apple SDMA ring submission or a client IB that runs un-NOPed. ON: every key moves and the 100 ms window starts. SHADOW:
// counted and the window stamped (the plan prices it as `quiet-window would`), no key moves.
void navi48_d84_unknown(void) {
	__atomic_store_n(&gD84LastUnknownUs, sk82_now_us() | 1ull, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&gCg84Mode, __ATOMIC_RELAXED) == N48_CG84_ON) d84_bump_all_counted(&gD84T.st.unknowns);
	else __atomic_fetch_add(&gD84T.st.unknowns, 1ull, __ATOMIC_RELAXED);
}

static bool d84_alloc() {
	if (__atomic_load_n(&gN48D84Live, __ATOMIC_ACQUIRE)) return true;
	const size_t poolB = (size_t)(N48_D84_KEYS + 1u) * N48_D84_MAX_BYTES;
	IOLock *l = IOLockAlloc();
	uint8_t *pool = static_cast<uint8_t *>(IOMalloc(poolB));
	if (!l || !pool) {
		if (l) IOLockFree(l);
		if (pool) IOFree(pool, poolB);
		return false;
	}
	memset(pool, 0, poolB);
	for (uint32_t i = 0; i < N48_D84_KEYS; i++) gD84T.k[i].shadow = pool + (size_t)i * N48_D84_MAX_BYTES;
	gD84T.scratch = pool + (size_t)N48_D84_KEYS * N48_D84_MAX_BYTES;
	gD84Pool = pool; gD84Lock = l;
	__atomic_store_n(&gN48D84Live, 1u, __ATOMIC_RELEASE);
	return true;
}
static n48_d84_stats gD84Rep {};   // the report's snapshot (verb thread only): a file-scope static, off the stack
static void cg84_report(const char *how) {
	n48_d84_stats &s = gD84Rep;
	s = n48_d84_stats {};
	const n48_cg84_stats c = gCg84S;
	uint32_t mode = gCg84Mode;
	uint32_t v = 0u, inv = 0u;
	if (gD84Lock) {
		IOLockLock(gD84Lock);
		s = gD84T.st; mode = gCg84Mode;
		for (uint32_t i = 0; i < N48_D84_KEYS; i++) { if (gD84T.k[i].state == N48_D84_S_VALID) v++; else if (gD84T.k[i].state) inv++; }
		IOLockUnlock(gD84Lock);
	}
	N48LOG(N48_CG84_FMT, mode, (unsigned long long)c.fine, (unsigned long long)c.admitted, (unsigned long long)c.refused,
	       (unsigned long long)c.fullNotes);
	N48LOG(N48_D84_FMT, N48_D84_ARGS(&s));
	// CG84.md item 8: under SHADOW the keys are "sampled trust" (the copies are 0.0.528's, their verify may be sampled)
	N48LOG(N48_D84_FMT3, (unsigned long long)s.unknowns, (unsigned long long)s.quiet, (unsigned long long)s.quietWould,
	       (unsigned long long)s.commits, (unsigned long long)s.foreign, (unsigned long long)s.deltaPrograms,
	       (unsigned long long)s.deltaUpd, (unsigned long long)s.probes, (unsigned long long)s.probeBad);
	N48LOG(N48_D84_FMT2, mode == N48_CG84_SHADOW ? "SHADOW(340) sampled-trust keys" : n48_cg84_mode_name(mode), how, (unsigned long long)s.considered, (unsigned long long)s.eligible,
	       (unsigned long long)s.wouldDelta, (unsigned long long)s.cause[N48_D84_C_BUSY], (unsigned long long)s.cause[N48_D84_C_STATE],
	       (unsigned long long)s.updated, (unsigned long long)s.rejected, (unsigned long long)s.invalAll, (unsigned long long)s.invalRes,
	       (unsigned long long)s.invalUnmap, (unsigned long long)s.invalCommit, (unsigned long long)s.bumps, v, inv, N48_D84_KEYS,
	       gD84Lock ? "allocated" : "not allocated");
}
// `gfxneuter 84 | M << 8` (AppleHardwareHook.cpp's selector computes the continuous-arm guard). M 1 SHADOW (340), M 2 OFF (596, the
// default and the boot value), M 3 ON (852); bare 84 reads; any other M is refused unchanged (st 11); a continuous arm refuses a
// change (st 5); a failed allocation refuses (st 12, stays OFF). The ONLY writer of gCg84Mode. Every change of mode resets the keys.
// It writes no register, no page table and nothing of Apple's.
uint32_t navi48_cg84_switch(uint32_t m, uint32_t contRefused, uint32_t *st) {
	const uint32_t want = n48_cg84_mode_of_m(m);
	const char *how = " - read only";
	if (contRefused) { *st = 5u; how = " - REFUSED: a continuous arm stands, unchanged"; }
	else if (want == N48_CG84_M_BAD) { *st = 11u; how = " - REFUSED (unknown M), unchanged"; }
	else if (want != N48_CG84_M_READ) {
		if (want != N48_CG84_OFF && !d84_alloc()) {
			*st = 12u; how = " - REFUSED: no memory, stays OFF";
		} else if (gD84Lock) {
			IOLockLock(gD84Lock);
			if (gCg84Mode != want) { n48_d84_reset(&gD84T); gCg84Mode = want; }
			IOLockUnlock(gD84Lock);
			how = " - CHANGED BY THIS VERB";
		} else {
			how = " - CHANGED BY THIS VERB";   // OFF, never allocated: nothing to reset
		}
	}
	cg84_report(how);
	return gCg84Mode;
}

// 0.0.435 review fix — `opened` counts EVERY open, tracked or untracked (an untracked copy still opens; only the
// slot claim fails), so it stays in step with `closed`, which already counted both.
// 0.0.437 — every gCgStats counter is incremented with __atomic_fetch_add, RELAXED (this
// struct's fields are report-only counters, like gMmPrioStats; RELAXED is enough because nothing branches on the
// ORDER two increments become visible in, only their eventual value, and navi48_cg_snapshot below reads them back
// with __atomic_load_n rather than trusting a plain struct copy to see a consistent snapshot across threads).
// build 0.0.529 (CG84.md items 7-8): the switch-84 instrument range of THIS thread's pending residency copy, when [lo, hi) is
// the scope that copy planned (the would-be delta under SHADOW, the delta itself under ON); 0/0 otherwise. The pending record was
// written under gD84Lock by the same thread, before this open. noinline: navi48_cg_open's frame carries none of it.
static __attribute__((noinline)) void d84_open_range(uint64_t lo, uint64_t hi, uint64_t *dlo, uint64_t *dhi) {
	const n48_d84_pend *p = &gD84T.p;
	if (p->thr && p->thr == (uintptr_t)current_thread() && p->lo == lo && p->hi == hi) { *dlo = p->dlo; *dhi = p->dhi; }
}
// ... and the invalidation (item 7): every held key this open overlaps moves, AFTER the slot (or the untracked count) is published
//, except the key whose pending copier is this thread. Lock-free.
static __attribute__((noinline)) void d84_open_bump(uint64_t lo, uint64_t hi) {
	const uint32_t moved = n48_d84_bump(gD84T.k, N48_D84_KEYS, lo, hi, (uintptr_t)current_thread());
	if (moved) __atomic_fetch_add(&gD84T.st.bumps, (uint64_t)moved, __ATOMIC_RELAXED);
}
int32_t navi48_cg_open(uint64_t lo, uint64_t hi) {
	// build 0.0.541 (gfx_wc98.h): a copy scope opens = a remap may be in flight until its close (every residency copy and every
	// MM-window write, the keystone's root[511] writes included, is inside one). First, before the slot is published.
	n48_wc_enter(&gN48WcGen, N48_WC_B_CG_OPEN);
	// 0.0.438: the owner every open publishes is THIS thread - current_thread(), the kext's own
	// identity for "who may treat this slot as already scoping their own write" (n48_cg_slot_contains, below).
	// build 0.0.529: + the switch-84 instrument range (0/0 unless this thread's residency copy planned this scope).
	uint64_t dlo = 0ull, dhi = 0ull;
	if (gN48D84Live) d84_open_range(lo, hi, &dlo, &dhi);
	const int32_t slot = n48_cg_slot_open_d(gCgSlots, N48_CG_SLOTS, lo, hi, (uintptr_t)current_thread(), dlo, dhi);
	__atomic_fetch_add(&gCgStats.opened, 1ull, __ATOMIC_RELAXED);
	// build 0.0.527 (SKIP82.md item 3; gfx_sk82.h DEVIATION D1): the write counters, AFTER the slot (or, untracked, the
	// untracked count) is published open and BEFORE this scope's first write - so a write in progress is always visible to switch
	// 82 as an open scope or a moved counter. ONE load while switch 82 was never ON (the array does not exist).
	uint32_t *const sk82 = __atomic_load_n(&gSk82Cnt, __ATOMIC_ACQUIRE);
	if (slot < 0) {
		__atomic_fetch_add(&gCgUntracked, 1u, __ATOMIC_SEQ_CST);
		if (sk82) { n48_sk82_bump(sk82, gSk82Nb, &gSk82Every, lo, hi); __atomic_fetch_add(&gSk82Every, 1ull, __ATOMIC_SEQ_CST); }
		__atomic_fetch_add(&gCgStats.untracked, 1ull, __ATOMIC_RELAXED);
		// 0.0.437 — UNTRACKED COPIES LEAVE A MATCH-ALL TRAIL. No slot could be claimed to name
		// this copy's own range, so a reader has nothing to compare against unless this open still publishes
		// SOMETHING: push a BEGIN over the widest possible range, [0, ~0ull), so any reader window whose own check
		// runs while this copy is open refuses no matter what it reads - the same fail-closed answer the slot-scan
		// UNTRACKED short-circuit already gives n48_cg_check, now also true of the RING path.
		n48_cg_ring_push_o(&gCgRing, N48_CG_EV_BEGIN, ~0u, 0ull, ~0ull, (uintptr_t)current_thread());   // 0.0.523: + owner
		if (gN48D84Live) d84_open_bump(lo, hi);   // build 0.0.529: after the untracked count is published
		return -1;
	}
	if (sk82) n48_sk82_bump(sk82, gSk82Nb, &gSk82Every, lo, hi);
	if (gN48D84Live) d84_open_bump(lo, hi);   // build 0.0.529 (CG84.md item 7): after the slot is published, before any write
	// build 0.0.529 (CG84.md item 8): BEGIN carries the instrument range, written before the stamp.
	n48_cg_ring_push_od(&gCgRing, N48_CG_EV_BEGIN, (uint32_t)slot, lo, hi, (uintptr_t)current_thread(), dlo, dhi);
	return slot;
}

// 0.0.435 review fix — `wrote`: false only for the switch-39 phantom scope (navi48_cg_phantom_scope below), which
// writes nothing by construction. The poison decision itself is gfx_copyguard.h's n48_cg_close_poison (pure): a
// wrote=false close must neither mark nor clear a poison row, because "clear" would wrongly vouch for a range this
// scope never touched - a page a FAILED copy half-wrote earlier would be trusted again for free.
void navi48_cg_close(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {
	if (slot < 0) {
		// 0.0.438: END BEFORE THE DECREMENT, mirroring the tracked path's END-before-seq-even
		// rule just below (n48_cg_check's own comment has the race that order closes: a reader whose load of
		// gCgUntracked lands after this decrement, but before the END push, would see neither the untracked count
		// NOR this copy's own event in the ring - the exact gap "an untracked copy leaves no BEGIN/END"
		// warned about, now reopened at CLOSE instead of OPEN). Push the matching END for the match-all BEGIN this
		// copy's OPEN pushed FIRST, then decrement.
		n48_cg_ring_push_o(&gCgRing, N48_CG_EV_END, ~0u, 0ull, ~0ull, (uintptr_t)current_thread());   // 0.0.523: + owner
		// 0.0.438: an untracked copy must poison its own range on failure/mismatch exactly as
		// a tracked one does — no slot was ever free to tell the poison table what happened over [lo, hi), so
		// without this call a failed or mismatched UNTRACKED copy would leave nothing behind for a later reader's
		// poison check to see, silently trusting VRAM it never verified.
		n48_cg_close_poison(&gCgPoison, lo, hi, wrote ? 1 : 0, failed ? 1 : 0, mismatch ? 1 : 0);
		if (wrote && (failed || mismatch)) __atomic_fetch_add(&gCgStats.poisoned, 1ull, __ATOMIC_RELAXED);
		__atomic_fetch_sub(&gCgUntracked, 1u, __ATOMIC_SEQ_CST);
		__atomic_fetch_add(&gCgStats.closed, 1ull, __ATOMIC_RELAXED);
		n48_wc_leave(&gN48WcGen, N48_WC_B_CG_END);   // build 0.0.541: the copy-guard END, after the untracked count falls
		return;
	}
	// END before the slot is published closed (below): a reader whose ring-position load lands after this push,
	// but before n48_cg_slot_close runs, must still see this copy in the ring even though the slot scan can no
	// longer find it odd (gfx_copyguard.h's n48_cg_check comment has the race this order closes).
	// build 0.0.529 (CG84.md item 8): END carries the slot's instrument range (written before the stamp).
	n48_cg_ring_push_od(&gCgRing, N48_CG_EV_END, (uint32_t)slot, lo, hi, (uintptr_t)current_thread(),
	                    __atomic_load_n(&gCgSlots[slot].dlo, __ATOMIC_SEQ_CST), __atomic_load_n(&gCgSlots[slot].dhi, __ATOMIC_SEQ_CST));
	n48_cg_close_poison(&gCgPoison, lo, hi, wrote ? 1 : 0, failed ? 1 : 0, mismatch ? 1 : 0);
	if (wrote && (failed || mismatch)) __atomic_fetch_add(&gCgStats.poisoned, 1ull, __ATOMIC_RELAXED);
	n48_cg_slot_close(gCgSlots, slot);
	__atomic_fetch_add(&gCgStats.closed, 1ull, __ATOMIC_RELAXED);
	n48_wc_leave(&gN48WcGen, N48_WC_B_CG_END);   // build 0.0.541: the copy-guard END, after the slot is published closed
}

void navi48_cg_seg_begin(void) {
	gCg84S.fullNotes += gCgSegRec.nfull;   // build 0.0.529: the previous pass's whole-page notes (instrument)
	n48_cg_pagerec_reset(&gCgSegRec);
	gCgSegSince = n48_cg_ring_mark(&gCgRing);
	gCgSegRec.cur_since = gCgSegSince;   // 0.0.523: every page's since is the pass mark until a switch-78 redo re-bases
	// build 0.0.529 (CG84.md item 3): switch 84 LATCHED for the pass - `fine` (the counted check tests granules) only under ON.
	const uint32_t m84 = gCg84Mode;
	gCgSegRec.m84 = m84;
	gCgSegRec.fine = n48_cg84_fine_of(m84);
}

n48_cg_pagerec *navi48_cg_active_recorder(void) { return &gCgSegRec; }

// 0.0.436 (an internal design note Part 1, design "2. M") — the per-pass program-identity
// memo's own validity reads (AppleHardwareHook.cpp's gfx_pgmid.h n48_pm_lookup/n48_pm_store callers): the ring's
// live position, and a poison-overlap answer for one page, both thin wraps of this file's own gCgRing/gCgPoison.
uint64_t navi48_cg_ring_mark_now(void) { return n48_cg_ring_mark(&gCgRing); }
int navi48_cg_poison_overlaps_page(uint64_t page) { return n48_cg_poison_overlaps(&gCgPoison, page, page + 4096ull); }

// 0.0.435 review fix — no per-refusal `inFlight++` here any more: it duplicated `refused[N48_CG_IN_FLIGHT]` and
// could never fall back to 0 once every scope closed. The report's "in flight" field is the LIVE count, computed
// fresh at snapshot time below (n48_cg_live_open), never accumulated.
// build 0.0.523: the SAME counted check, through n48_cg_check_ex (identical while per_page is 0), keeping what refused
// in gCgLastWhy for the per-refusal line (navi48_cg_last_why). Still the ONLY admit of a segment.
static n48_cg_why gCgLastWhy {};
// build 0.0.529 (CG84.md item 8): THE UNCOUNTED OTHER ANSWER, after the counted check. SHADOW: the fine check over the
// would-be deltas (`used` 1) - what ON would have decided; ON: the page check (`fine` 0) - what 0.0.528 would have decided over
// the same (delta) scopes. Neither changes `r`, a counter of gCgStats, gCgLastWhy or the recorder. noinline (the pass's frame).
static n48_cg_why gCg84Why {};
static __attribute__((noinline)) void cg84_instrument(uint32_t r, uint32_t untracked) {
	const uint32_t m = gCgSegRec.m84;
	if (m == N48_CG84_SHADOW) {
		const uint32_t rf = n48_cg_check_fx(&gCgSegRec, 1u, 1u, &gCgPoison, gCgSlots, N48_CG_SLOTS, &gCgRing, gCgSegSince, untracked,
		                                    &gCg84Why);
		n48_cg84_note(&gCg84S, r, rf);
	} else if (m == N48_CG84_ON) {
		const uint32_t rp = n48_cg_check_fx(&gCgSegRec, 0u, 0u, &gCgPoison, gCgSlots, N48_CG_SLOTS, &gCgRing, gCgSegSince, untracked,
		                                    &gCg84Why);
		n48_cg84_note(&gCg84S, rp, r);
	}
}
uint32_t navi48_cg_seg_check(void) {
	uint64_t pfC = 0ull;   // build 0.0.540 (switch 96, T4): this check's time; OFF one load, no clock
	if (__atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED)) clock_get_uptime(&pfC);
	const uint32_t untracked = __atomic_load_n(&gCgUntracked, __ATOMIC_SEQ_CST);
	const uint32_t r = n48_cg_check_ex(&gCgSegRec, &gCgPoison, gCgSlots, N48_CG_SLOTS, &gCgRing, gCgSegSince, untracked,
	                                   &gCgLastWhy);
	__atomic_fetch_add(&gCgStats.checked, 1ull, __ATOMIC_RELAXED);
	if (r != N48_CG_OK && r < N48_CG_REASONS) __atomic_fetch_add(&gCgStats.refused[r], 1ull, __ATOMIC_RELAXED);
	if (gCgSegRec.m84 == N48_CG84_SHADOW || gCgSegRec.m84 == N48_CG84_ON) cg84_instrument(r, untracked);   // build 0.0.529
	if (pfC) pf540_cg_note(pfC);
	return r;
}
const n48_cg_why *navi48_cg_last_why(void) { return &gCgLastWhy; }
// build 0.0.523 (switch 78) — THE REDO's ACCESSORS. The PEEK is the same check, UNCOUNTED, into the caller's `why`: it
// decides only whether a redo is worth trying, never admits (the counted navi48_cg_seg_check above stays the only admit).
uint32_t navi48_cg_seg_peek(n48_cg_why *why) {
	const uint32_t untracked = __atomic_load_n(&gCgUntracked, __ATOMIC_SEQ_CST);
	return n48_cg_check_ex(&gCgSegRec, &gCgPoison, gCgSlots, N48_CG_SLOTS, &gCgRing, gCgSegSince, untracked, why);
}
uint32_t navi48_cg_rec_count(void) { return gCgSegRec.n; }
uint64_t navi48_cg_pass_since(void) { return gCgSegSince; }
const n48_cg_ring *navi48_cg_ring_ptr(void) { return &gCgRing; }
// Does any recorded page overlap an open (or torn) slot right now? The IN_FLIGHT wait's poll.
// build 0.0.529 (CG84.md item 3): stays PAGE-level - over the recorded pages and, under a fine pass, the census pages too.
int navi48_cg_rec_inflight(void) {
	for (uint32_t i = 0; i < gCgSegRec.n && i < N48_CG_REC_PAGES; i++)
		if (n48_cg_slot_scan(gCgSlots, N48_CG_SLOTS, gCgSegRec.page[i], gCgSegRec.page[i] + 4096ull) != N48_CG_SLOT_NONE)
			return 1;
	if (gCgSegRec.fine)
		for (uint32_t j = 0; j < gCgSegRec.xn && j < N48_CG_XPAGES; j++)
			if (n48_cg_slot_scan(gCgSlots, N48_CG_SLOTS, gCgSegRec.xpg[j], gCgSegRec.xpg[j] + 4096ull) != N48_CG_SLOT_NONE)
				return 1;
	return 0;
}

// 0.0.437 — every field read with __atomic_load_n, RELAXED (matching the RELAXED stores
// above): a plain `*out = gCgStats` struct copy is not what any of the increments above use to WRITE these fields
// any more, so reading it back the same informal way would no longer be the honest mirror of the real counters
// that every other snapshot in this file (e.g. navi48_mm_prio_snapshot) already is.
void navi48_cg_snapshot(n48_cg_stats *out) {
	if (!out) return;
	out->opened    = __atomic_load_n(&gCgStats.opened,    __ATOMIC_RELAXED);
	out->closed    = __atomic_load_n(&gCgStats.closed,    __ATOMIC_RELAXED);
	out->poisoned  = __atomic_load_n(&gCgStats.poisoned,  __ATOMIC_RELAXED);
	out->untracked = __atomic_load_n(&gCgStats.untracked, __ATOMIC_RELAXED);
	out->unscoped  = __atomic_load_n(&gCgStats.unscoped,  __ATOMIC_RELAXED);
	out->checked   = __atomic_load_n(&gCgStats.checked,   __ATOMIC_RELAXED);
	for (uint32_t i = 0; i < N48_CG_REASONS; i++)
		out->refused[i] = __atomic_load_n(&gCgStats.refused[i], __ATOMIC_RELAXED);
	out->inFlight = n48_cg_live_open(gCgSlots, N48_CG_SLOTS, __atomic_load_n(&gCgUntracked, __ATOMIC_SEQ_CST));
}

// 0.0.435 — `accel gfxneuter 39 | 1 << 8`: THE POSITIVE CONTROL. Opens one scope over the whole of measured VRAM,
// holds it 2 s writing nothing, then closes it. Blocking (an explicit verb call only, never a per-frame path):
// during the hold, every reader check whose recorded pages fall inside VRAM sees the slot odd and overlapping, so
// it refuses IN_FLIGHT; the moment this returns, the scope is closed and refusals go back to baseline. "Confirm
// free by census" - the whole point is that this needs no cooperating writer at all to prove the check is wired.
// 0.0.435 review fix — `wrote = false`: this scope writes nothing, so its close must never touch poison (see
// Navi48CopyScope's `wrote` parameter and n48_cg_close_poison above).
void navi48_cg_phantom_scope(void) {
	const uint64_t hi = gBringup.dev ? gBringup.dev->vramSizeBytes : 0ull;
	Navi48CopyScope scope(0ull, hi, /*wrote=*/false);
	uint64_t t0 = 0ull, now = 0ull, elapsedUs = 0ull;
	clock_get_uptime(&t0);
	while (elapsedUs < 2000000ull) {
		IODelay(1000);
		clock_get_uptime(&now);
		absolutetime_to_nanoseconds(now - t0, &elapsedUs);
		elapsedUs /= 1000ull;
	}
}

// The REAL GART aperture this driver programmed, for Navi48Ttl to report to
// Apple's accelerator.
//
// This exists because getNonLocalMemoryInfo used to invent an aperture — base =
// vramMcBase + vramSizeBytes, size = the accelerator's own 1 GiB limit — on the
// reasoning that "we do not manage a GART on Apple's behalf". We do manage one,
// and Apple believed the invented numbers: measured, it placed its
// SDMA ring at 0x83fb220000 and its command buffers at 0x83fb8001xx, both inside
// the invented range and both outside the GART we actually programmed
// (ALIGN(vram_end+1, 4 GiB), 256 MiB). Nothing maps those addresses, so the ring
// could never be fetched. Report what is really there instead.
// Mirror one of Apple's GART mappings into OUR page table, in GFX12 PTE format.
//
// Apple programs its own GART page table in GFX10 format and expects its own
// register writes to install it — writes this driver blocks, because those
// offsets are wrong on gfx12. So Apple's mappings never reach hardware. This
// writes the same virtual-to-physical mapping into the page table the GPU is
// actually walking.
//
// gartAddr is an absolute GART MC address (what Apple hands out); gmc_bind_at
// wants an offset within the aperture.
bool navi48_gart_bind_at(uint64_t gartAddr, uint64_t busAddr, uint64_t sizeBytes) {
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	const uint64_t start = gBringup.gmc.gart_start;
	if (gartAddr < start) return false;
	return amdgpu::gmc_bind_at(*gBringup.dev, gBringup.gmc,
	                           gartAddr - start, busAddr, sizeBytes) == kIOReturnSuccess;
}

// Read one GART PTE back out of our own GFX12 page table.
//
// step 2: "in range" and "resident" are different questions. The packets
// Apple wrote all point inside the GART aperture, but that says nothing about
// whether the page is actually mapped in OUR table -- the sync only binds
// entries Apple marked valid, and it binds them scattered across a 65536-entry
// array. An IB page that is in range but not resident means the engine walks a
// translation that does not exist.
//
// The page table lives in VRAM at gmc.gart_pt_vram_offset, 8 bytes per entry,
// indexed by (gartAddr - gart_start) / 4096. Reads go through the same BAR0
// aperture the write path uses. This reads only; it maps nothing.
bool navi48_gart_read_pte(uint64_t gartAddr, uint64_t &pteOut) {
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	const auto &gmc = gBringup.gmc;
	if (gmc.gart_pt_bus == 0 || gmc.gart_size == 0)  return false;
	const uint64_t start = gmc.gart_start;
	if (gartAddr < start || gartAddr >= start + gmc.gart_size) return false;

	const uint64_t idx = (gartAddr - start) / 4096ULL;
	const uint64_t off = gmc.gart_pt_vram_offset + idx * 8ULL;
	const auto &dev = *gBringup.dev;
	if (!dev.bar0 || off + 8 > dev.bar0Size) return false;

	const volatile uint32_t *p =
	    reinterpret_cast<const volatile uint32_t *>(dev.bar0 + off);
	const uint32_t lo = p[0], hi = p[1];
	pteOut = ((uint64_t)hi << 32) | lo;
	return true;
}

// Point SDMA1 QUEUE1 at a ring somebody else owns.
//
// enable_ring MUST be false until the ring has been dumped, decoded, and every
// embedded address proven RESIDENT. With RB_ENABLE=0 the engine
// fetches nothing, so this is inert with respect to silicon.
//
// doorbell_index is in QWORD units and ALREADY SHIFTED: sdma_v7_0.cpp:297 sets
// inst.doorbell_index = doorbell.index.sdma_engine[i] << 1, and that same value
// feeds both DOORBELL_OFFSET and the BAR2 write at doorbell_index * 8.'s
// free indices 0x10B..0x113 are in the UNSHIFTED sdma_engine[] space, so the
// caller must pass (0x10B << 1) = 0x216 -- clear of our SDMA1 QUEUE0 at 0x214
// and of SDMA2 at 0x228. Passing the unshifted value programs the wrong doorbell.
// Read a GART range that may SPAN PAGES.
//
// navi48_gart_read_page refuses cross-page reads on purpose: GART pages need not
// be physically contiguous, so each one needs its own PTE resolved. That is
// correct, but it made the IB walker abort — Apple's IB at 0x8400800f20 is 768
// dwords starting at page offset 0xf20, so it ends at 0x1b20, inside the NEXT
// page. The walker hit the refusal, took its `return 0` path, and never reached
// the IB after it.
//
// This loops page by page, so every page's PTE is resolved and residency-checked
// individually. Same guarantees as the single-page helper, just applied N times.
// 0.0.212: the cap was 8192 bytes, below Apple's 2240-dword page-map IB (8960 bytes). The loop
// resolves and residency-checks every page on its own, so the length bound only has to cover the SDMA IB cap.
bool navi48_gart_read_range(uint64_t gartAddr, void *dst, uint32_t len) {
	if (!dst || len == 0 || len > 65536) return false;
	uint8_t *out = static_cast<uint8_t *>(dst);
	uint32_t done = 0;
	while (done < len) {
		const uint64_t a   = gartAddr + done;
		const uint32_t off = (uint32_t)(a & 0xFFF);
		uint32_t chunk     = 4096u - off;
		if (chunk > len - done) chunk = len - done;
		if (!navi48_gart_read_page(a, out + done, chunk)) {
			N48LOG("gart-read-range: page %#llx of range %#llx+%u failed",
			       (unsigned long long)(a & ~0xFFFULL),
			       (unsigned long long)gartAddr, len);
			return false;
		}
		done += chunk;
	}
	return true;
}

// ---- 0.0.178 "gfxmap" ----------------------------------------------------
//
// Our kernel GFX queue's doorbell DWORD index. cp_v12_0.cpp:2348 sets it from
// dev.doorbell.index.gfx_ring0, which the upstream Navi10 map puts at 0x8B
// (139); `navi48test info` prints the same number ("GFX ring doorbell 139"), so
// this is a read of the live value, not a constant.
//
// NOTE FOR ANYONE WORKING FROM OLDER NOTES: the bring-up logs say
// "map_gfx_kgq_mes: ... doorbell=0". Those runs were the LEGACY map
// (doorbell_init printed max_idx=512 = 0x200, the legacy max_assignment), where
// gfx_ring0 IS 0. With the default upstream map it is 139, and the r1
// transcript confirms 139 on the machine as it boots today.
bool navi48_kgq_doorbell_index(uint32_t &indexOut) {
	if (!gBringup.dev || !gBringup.cp.inited) return false;
	indexOut = gBringup.cp.doorbell_index;
	return true;
}

// The REAL BAR2 doorbell dword for a DWORD index.
//
// cp_kick_doorbell writes at BAR2 + doorbell_index * kCPDoorbellStride, and
// kCPDoorbellStride is 4 (amdgpu_cp.h:554 — "dword-indexed aperture"), so the
// byte offset is index * 4 and the CP samples the 64-bit pair that starts there.
// That is the word Apple's writeTail has to store into for the doorbell to
// reach the queue the MES mapped.
//
// The refusals are the point: never hand out a doorbell one of OUR engines is
// listening on (amdgpu_doorbell.cpp's map: MES ring0/ring1, the aggregated
// block, SDMA's qword-indexed range, IH), because two owners on one doorbell is
// silent corruption rather than an error.
bool navi48_doorbell_dword_ptr(uint32_t index, void **ptrOut, uint64_t *byteOffOut) {
	if (ptrOut) *ptrOut = nullptr;
	if (byteOffOut) *byteOffOut = 0;
	if (!gBringup.dev || !gBringup.dev->bar2) return false;
	const auto &dev = *gBringup.dev;
	const auto &ix  = dev.doorbell.index;
	const uint64_t off = (uint64_t)index * 4ull;
	if (off + 8 > dev.bar2Size) {
		N48LOG("doorbell-ptr: dword index %u (byte %#llx) is outside the %llu KiB "
		       "BAR2 aperture - REFUSING", index, (unsigned long long)off,
		       (unsigned long long)(dev.bar2Size >> 10));
		return false;
	}
	const bool reserved =
		index == ix.mes_ring0 || index == ix.mes_ring1 || index == ix.ih ||
		index == ix.kiq || index == ix.hiq || index == ix.diq ||
		(index >= ix.mec_ring[0] && index <= ix.mec_ring[7]) ||
		(index >= ix.mes_aggregated[0] && index <= ix.mes_aggregated[4] + 1) ||
		(index >= (ix.sdma_engine[0] << 1));
	if (reserved) {
		N48LOG("doorbell-ptr: dword index %u belongs to one of OUR engines "
		       "(kiq %#x hiq %#x diq %#x mec %#x..%#x mes %#x/%#x ih %#x "
		       "aggregated %#x.. sdma %#x..) - REFUSING to hand it to Apple",
		       index, ix.kiq, ix.hiq, ix.diq, ix.mec_ring[0], ix.mec_ring[7],
		       ix.mes_ring0, ix.mes_ring1, ix.ih, ix.mes_aggregated[0],
		       ix.sdma_engine[0] << 1);
		return false;
	}
	if (ptrOut) *ptrOut = (void *)(dev.bar2 + off);
	if (byteOffOut) *byteOffOut = off;
	return true;
}

bool navi48_hdp_flush_now(void) {
	if (!gBringup.dev) return false;
	amdgpu::sysmem_wmb();
	amdgpu::amdgpu_hdp_flush(*gBringup.dev);
	return true;
}

// Clear-state buffer for TTL slot 13 (queryCSBInfo). Apple's AMDHardware::initHWInfo
// stores what we return at hw+0x2c4 (address) / hw+0x2c0 (size), and
// AMDPM4HWChannel::performClearState (0xbe16734) submits it as an INDIRECT_BUFFER
// with dw3 = (size_dw & 0xfffff) | 0x80000000 (review. Until 0.0.181
// slot 13 returned 0/0, so Apple submitted a 0-dword IB at GPU address 0 and the CP
// stalled at ring dword 6. This publishes ONE GART page holding
// 16 one-dword NOPs (0xffff1000, the padding encoding Apple's own ring uses) and
// reports 64 bytes. It is a CP-executable no-op, not the RLC clear-state blob:
// gfx12's rlc clear_state is a register list, not PM4, and must never be handed
// to slot 13 as-is. Bound once per boot; the binding is kept for the kext lifetime.
static amdgpu::SysMem gCsbMem {};
static uint64_t       gCsbMc        = 0;
static bool           gCsbPublished = false;
bool navi48_csb_publish(uint64_t *mcOut, uint32_t *sizeOut) {
	if (mcOut) *mcOut = 0;
	if (sizeOut) *sizeOut = 0;
	if (!gBringup.dev || !gBringup.gfxhubReady) {
		N48LOG("csb: GART not ready - publishing nothing");
		return false;
	}
	if (!gCsbPublished) {
		// 0.0.182: bind through the GMC bump allocator, NOT gart_bind_sysmem. The latter
		// hands out GART offset 0 = MC 0x8400000000, which is Apple's write-back page;
		// r6 measured Apple submitting exactly that address and the CP stalling on it.
		// gmc_bind_existing takes the next bump offset, which navi48-gart-high=1 puts
		// in the high region clear of Apple's pages (the MQD at 0x840fc04000 lives there).
		kern_return_t kr = amdgpu::sysmem_alloc(gCsbMem, 4096, 4096);
		if (kr != KERN_SUCCESS || !gCsbMem.valid()) {
			N48LOG("csb: sysmem_alloc failed (%d) - publishing nothing", (int)kr);
			return false;
		}
		kr = amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gCsbMem.bus, 4096, &gCsbMc);
		if (kr != KERN_SUCCESS || gCsbMc == 0) {
			N48LOG("csb: gmc_bind_existing failed (%d) - publishing nothing", (int)kr);
			return false;
		}
		if ((gCsbMc & 0xfffffULL) == 0 && gCsbMc == gBringup.gmc.gart_start) {
			N48LOG("csb: bump handed out GART page 0 (%#llx) - that is Apple's write-back page, REFUSING",
			       (unsigned long long)gCsbMc);
			return false;
		}
		volatile uint32_t *d = static_cast<volatile uint32_t *>(gCsbMem.cpu);
		for (uint32_t i = 0; i < 1024; i++) d[i] = 0xffff1000u;   // 1-dword NOP padding, whole page
		amdgpu::sysmem_wmb();
		amdgpu::amdgpu_hdp_flush(*gBringup.dev);
		gCsbPublished = true;
		N48LOG("csb: published a 16-dword NOP IB at GART MC %#llx (cpu %p, page of 0xffff1000) - "
		       "expect Apple's clear-state INDIRECT_BUFFER dw1/dw2 = that address and dw3 = 0x80000010",
		       (unsigned long long)gCsbMc, gCsbMem.cpu);
	}
	if (mcOut) *mcOut = gCsbMc;
	if (sizeOut) *sizeOut = 64;
	return true;
}

// 0.0.359: what the render drain's arm-time walk needs to identify performClearState's IB as OURS - the page's
// MC address, the size we reported, and our own mapping of it. Read-only; it never allocates, binds or publishes.
bool navi48_csb_peek(uint64_t *mcOut, uint32_t *sizeOut, const volatile uint32_t **cpuOut, uint32_t *pageDwordsOut) {
	if (mcOut) *mcOut = 0;
	if (sizeOut) *sizeOut = 0;
	if (cpuOut) *cpuOut = nullptr;
	if (pageDwordsOut) *pageDwordsOut = 0;
	if (!gCsbPublished || !gCsbMem.valid() || !gCsbMc) return false;
	if (mcOut) *mcOut = gCsbMc;
	if (sizeOut) *sizeOut = 64;
	if (cpuOut) *cpuOut = static_cast<const volatile uint32_t *>(gCsbMem.cpu);
	if (pageDwordsOut) *pageDwordsOut = 1024;   // the whole 4 KiB page was filled with 0xffff1000 at publish
	return true;
}

// 0.0.368: RULE E1's clause 2, half one — PROVE THE PAGE IS OURS IN HARDWARE.
//
// navi48_csb_peek says what the kext BELIEVES it published. It reads the kext's own variables, so it would keep saying
// the same thing if the GPU's page table had been repointed at somebody else's page underneath us. This reads the GART
// PTE the GPU will actually walk for that MC address, straight back out of the page table in VRAM, and reports it
// beside the value gmc_bind_existing wrote: our own DMA bus address, page-aligned, plus PTEFlags::SYSMEM_RW. If the two
// are equal, the address the CP fetches the clear-state IB from is the page this kext filled with NOPs.
//
// READ-ONLY: it writes no register, no PTE and no VRAM byte. The page table lives in VRAM at gart_pt_vram_offset; the
// BAR0 aperture covers the first 256 MiB of a 16 GiB card, so anything past it goes through the MM_INDEX window, which
// is the same path gmc_pdb0_build already reads with.
bool navi48_csb_pte_read(uint64_t *pteOut, uint64_t *wantOut) {
	if (pteOut) *pteOut = 0;
	if (wantOut) *wantOut = 0;
	if (!gBringup.dev || !gCsbPublished || !gCsbMem.valid() || !gCsbMc) return false;
	const amdgpu::GMCContext &gmc = gBringup.gmc;
	if (!gmc.inited || gmc.gart_pt_bus == 0 || gmc.gart_start == 0) return false;
	if (gCsbMc < gmc.gart_start || gCsbMc >= gmc.gart_start + gmc.gart_size) return false;
	const uint64_t gartOffset = gCsbMc - gmc.gart_start;
	if ((gartOffset & (amdgpu::kAMDGPUGPUPageSize - 1)) != 0) return false;
	const uint64_t pteIdx = gartOffset / amdgpu::kAMDGPUGPUPageSize;
	const uint64_t pteAt  = gmc.gart_pt_vram_offset + pteIdx * 8ULL;
	if (gmc.gart_pt_size && pteIdx * 8ULL + 8ULL > gmc.gart_pt_size) return false;
	uint64_t pte = 0;
	if (gBringup.dev->bar0 && pteAt + 8 <= gBringup.dev->bar0Size) {
		pte = amdgpu::RBAR0_64(*gBringup.dev, pteAt);
	} else {
		const uint32_t lo = amdgpu::RVRAM32_via_mm(*gBringup.dev, pteAt);
		const uint32_t hi = amdgpu::RVRAM32_via_mm(*gBringup.dev, pteAt + 4);
		pte = ((uint64_t)hi << 32) | lo;
	}
	if (pte == ~0ULL) return false;   // an unreadable aperture reads all-ones; that must never pass as a match
	const uint64_t want = (gCsbMem.bus & ~((uint64_t)0xFFFULL)) | amdgpu::PTEFlags::SYSMEM_RW;
	if (pteOut) *pteOut = pte;
	if (wantOut) *wantOut = want;
	return true;
}

bool navi48_gfx_remove_queue(uint32_t doorbellIndex, int32_t &krOut) {
	krOut = kIOReturnNotReady;
	if (!gBringup.dev) return false;
	if (!gBringup.mes.pipe[0].inited || !gBringup.mes.pipe[0].enabled) {
		N48LOG("gfx-remove: the MES SCHED pipe is not running - REFUSING");
		return false;
	}
	const kern_return_t r = amdgpu::mes_remove_hw_queue(
		*gBringup.dev, gBringup.mes, amdgpu::kMESQueueType_GFX,
		/*pipe_id=*/0, /*queue_id=*/0, doorbellIndex);
	krOut = (int32_t)r;
	gBringup.kgqMapped   = false;
	gBringup.cp.kgq_mapped = false;
	N48LOG("gfx-remove: MES REMOVE_QUEUE(GFX pipe0 queue0 doorbell=%u) %s (%#x)",
	       doorbellIndex, r == kIOReturnSuccess ? "ACKED" : "NOT acked", r);
	return r == kIOReturnSuccess;
}

// The GFX takeover, end to end. Every step is refusable and every refusal names
// itself in `failStep` as well as in the log, because rule 14 says the result
// must survive a log flood.
bool navi48_gfx_takeover(Navi48GfxTakeover &t) {
	t.failStep = 1;
	if (!gBringup.dev || !gBringup.gfxhubReady) {
		N48LOG("gfx-takeover: no device, or the GFXHUB GART is not up - REFUSING");
		return false;
	}
	if (!gBringup.cp.inited || !gBringup.cp.mqd_cpu || gBringup.cp.mqd_bus == 0) {
		N48LOG("gfx-takeover: the CP has no MQD buffer - REFUSING");
		return false;
	}
	if (!gBringup.mes.pipe[0].inited || !gBringup.mes.pipe[0].enabled) {
		N48LOG("gfx-takeover: the MES SCHED pipe is not running; only the MES can "
		       "map a GFX12 kernel GFX queue - REFUSING");
		return false;
	}

	// (1) PRECONDITION: every page of Apple's ring, its wptr write-back and its
	//     rptr report must already be resident in the table the GPU walks.
	//    : an unmapped page handed to an engine is exactly what must never
	//     happen, and "in the GART aperture" is a different question from
	//     "mapped in our table".
	t.failStep = 2;
	auto resident = [&](uint64_t page, const char *what) -> bool {
		uint64_t pte = 0;
		t.pagesChecked++;
		if (!navi48_gart_read_pte(page, pte)) {
			N48LOG("gfx-takeover: %s page %#llx - PTE UNREADABLE (outside our GART, "
			       "or the table is not up) - REFUSING", what,
			       (unsigned long long)page);
			return false;
		}
		if (!(pte & 1ull) || !(pte & (1ull << 63))) {
			N48LOG("gfx-takeover: %s page %#llx PTE %#llx is NOT RESIDENT (needs "
			       "VALID|IS_PTE) - REFUSING", what, (unsigned long long)page,
			       (unsigned long long)pte);
			return false;
		}
		t.pagesResident++;
		return true;
	};
	for (uint64_t p = t.ringGpu & ~0xFFFull; p < t.ringGpu + t.ringBytes; p += 4096ull)
		if (!resident(p, "ring")) return false;
	if (!resident(t.wptrPollGpu & ~0xFFFull, "wptr write-back")) return false;
	if (!resident(t.rptrReportGpu & ~0xFFFull, "rptr report")) return false;
	N48LOG("gfx-takeover: PTE check PASSED - %u of %u page(s) resident "
	       "(ring %#llx +%#x, wptr wb %#llx, rptr report %#llx)",
	       t.pagesResident, t.pagesChecked, (unsigned long long)t.ringGpu,
	       t.ringBytes, (unsigned long long)t.wptrPollGpu,
	       (unsigned long long)t.rptrReportGpu);

	// The wptr write-back is what the CP treats as "how far the host has
	// written". A bogus value there means the CP starts fetching packets Apple
	// has not written yet, so read it before committing to anything.
	t.failStep = 3;
	uint64_t wv = 0;
	if (navi48_gart_read_range(t.wptrPollGpu, &wv, sizeof(wv))) {
		t.wptrPollValue = wv;
		const uint64_t dwords = t.ringBytes / 4u;
		if (wv >= dwords) {
			N48LOG("gfx-takeover: the wptr write-back at %#llx holds %#llx, which is "
			       "past the end of a %llu-dword ring - the CP would fetch garbage - "
			       "REFUSING", (unsigned long long)t.wptrPollGpu,
			       (unsigned long long)wv, (unsigned long long)dwords);
			return false;
		}
		N48LOG("gfx-takeover: wptr write-back at %#llx = %#llx (ring holds %llu dwords)",
		       (unsigned long long)t.wptrPollGpu, (unsigned long long)wv,
		       (unsigned long long)dwords);
	} else {
		N48LOG("gfx-takeover: could not read the wptr write-back at %#llx through "
		       "our GART - REFUSING", (unsigned long long)t.wptrPollGpu);
		return false;
	}

	// (2) HDP flush. Apple's own flush after it writes an MQD or a ring through
	//     the BAR is the blocked 0xe17<-1 / 0xe08<-0 pair, so nothing Apple put
	//     in memory has been flushed. Do it for them before the GPU reads it.
	(void)navi48_hdp_flush_now();

	// (3) REMOVE our KGQ. It owns GFX pipe0/queue0 and doorbell index
	//     cp.doorbell_index, which is exactly what we are about to give away.
	t.failStep = 4;
	if (gBringup.kgqMapped) {
		int32_t rkr = 0;
		if (!navi48_gfx_remove_queue(gBringup.cp.doorbell_index, rkr)) {
			t.removeKr = rkr;
			N48LOG("gfx-takeover: our KGQ is still mapped on pipe0/queue0 and MES "
			       "would not unmap it (%#x) - REFUSING to map a second queue there",
			       rkr);
			return false;
		}
		t.removeKr = rkr;
	} else {
		t.removeKr = kIOReturnSuccess;
		N48LOG("gfx-takeover: our KGQ was already unmapped - nothing to REMOVE");
	}

	// (4) Build the MQD for Apple's ring, in the buffer our KGQ just gave up.
	t.failStep = 5;
	amdgpu::CPExternalGfxQueue q {};
	q.ring_gpu        = t.ringGpu;
	q.ring_bytes      = t.ringBytes;
	q.rptr_report_gpu = t.rptrReportGpu;
	q.wptr_poll_gpu   = t.wptrPollGpu;
	q.doorbell_index  = t.doorbellIndex;
	if (amdgpu::cp_gfx_mqd_init_external(*gBringup.dev, gBringup.cp, q) != kIOReturnSuccess) {
		N48LOG("gfx-takeover: the MQD build refused - nothing is mapped on "
		       "pipe0/queue0 now, which is the SAFE state");
		return false;
	}
	t.mqdGpu      = gBringup.cp.mqd_bus;
	t.rbBufsz     = q.rb_bufsz;
	t.hqdCntl     = q.hqd_cntl;
	t.doorbellCtl = q.doorbell_control;

	// (5) MES ADD_QUEUE, map_legacy_kq, exactly as cp_map_gfx_kgq_mes does it.
	t.failStep = 6;
	amdgpu::MESAddQueueInput in;
	memset(&in, 0, sizeof(in));
	in.queue_type      = amdgpu::kMESQueueType_GFX;
	in.pipe_id         = 0;
	in.queue_id        = 0;
	in.doorbell_offset = t.doorbellIndex;
	in.mqd_addr        = gBringup.cp.mqd_bus;
	in.wptr_addr       = t.wptrPollGpu;
	in.flags           = amdgpu::kAddQueueFlag_map_legacy_kq;
	N48LOG("gfx-takeover: MES ADD_QUEUE type=GFX pipe=0 queue=0 doorbell=%u "
	       "mqd_mc=%#llx wptr_mc=%#llx flags=map_legacy_kq - this is APPLE'S ring "
	       "%#llx taking over pipe0/queue0", in.doorbell_offset,
	       (unsigned long long)in.mqd_addr, (unsigned long long)in.wptr_addr,
	       (unsigned long long)t.ringGpu);
	const kern_return_t akr = amdgpu::mes_add_hw_queue(*gBringup.dev, gBringup.mes, in);
	t.addKr = (int32_t)akr;
	if (akr != kIOReturnSuccess) {
		N48LOG("gfx-takeover: MES did not ack ADD_QUEUE (%#x) - pipe0/queue0 is now "
		       "UNMAPPED (our KGQ is gone too). Nothing fetches.", akr);
		return false;
	}

	// (6) HDP flush again, then read what the firmware wrote back into the MQD.
	(void)navi48_hdp_flush_now();
	amdgpu::sysmem_rmb();
	const volatile uint32_t *m =
		static_cast<const volatile uint32_t *>(gBringup.cp.mqd_cpu);
	t.mqdActive = m[amdgpu::GfxMqd::cp_gfx_hqd_active];
	t.mqdMapped = m[amdgpu::GfxMqd::cp_gfx_hqd_mapped];
	t.mqdRptr   = m[amdgpu::GfxMqd::cp_gfx_hqd_rptr];
	t.mqdWptr   = m[amdgpu::GfxMqd::cp_gfx_hqd_wptr];
	t.failStep  = 0;
	N48LOG("gfx-takeover: ACKED. MQD now: active=%u mapped=%u rptr=%u wptr=%u "
	       "- APPLE'S GFX RING IS ON pipe0/queue0, doorbell dword %u",
	       t.mqdActive, t.mqdMapped, t.mqdRptr, t.mqdWptr, t.doorbellIndex);
	return true;
}

// GC IP base (BASE_IDX 0) from IP discovery. 0 if unresolved.
uint32_t navi48_gc_base(void) {
	if (!gBringup.dev) return 0;
	return gBringup.dev->ip.get(amdgpu::IPBlock::GC);
}

// 0.0.194 — GC register base for an arbitrary segment (gc_12_0_0_offset.h's
// *_BASE_IDX), 0 if that segment is absent. The CP IB registers `vmib` reads
// (CP_IB1_BASE_LO 0x20cc and friends) are BASE_IDX 1, so navi48_gc_base()
// would land them on a completely different register.
uint32_t navi48_gc_base_seg(uint8_t seg) {
	if (!gBringup.dev) return 0;
	if (!gBringup.dev->ip.isResolved(amdgpu::IPBlock::GC, (int)seg)) return 0;
	return gBringup.dev->ip.getBase(amdgpu::IPBlock::GC, (int)seg);
}

// Absolute-dword MMIO access for callers that hold no DeviceContext.
// RREG32/WREG32 bounds-check internally against rmmioSize, so no guard here.
uint32_t navi48_reg_read32(uint32_t reg) {
	if (!gBringup.dev) return 0xFFFFFFFFu;
	return amdgpu::RREG32(*gBringup.dev, reg);
}

void navi48_reg_write32(uint32_t reg, uint32_t value) {
	if (!gBringup.dev) return;
	amdgpu::WREG32(*gBringup.dev, reg, value);
}

// build 0.0.528 item 4: the marker holder's gVramMmLock wait, from before IOLockLock to after it (noinline: the MM paths gain
// one local). Measurement only: n48::hw_mkh_lockwait feeds the kswin2 L histogram; nothing decides on it.
static __attribute__((noinline)) void mm_lockwait_note(uint64_t l0) {
	uint64_t l1 = 0ull;
	clock_get_uptime(&l1);
	n48::hw_mkh_lockwait(navi48_mmprio_us(l0, l1));
}

// 0.0.193 — read VRAM through MM_INDEX/MM_DATA at a 0-BASED offset.
//
// The BAR0 aperture covers only the first 256 MiB of VRAM; Apple's page-table
// arena lives at ~0x3d6c00000, nearly 16 GiB up, so RBAR0_* cannot see it at
// all. The MM window can, and it is the GPU's own view of the memory — which is
// the point, because the entries under inspection were written by SDMA, not by
// any CPU. Read-only, bounded by the measured VRAM size, 64 dwords per call.
bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {
	if (!gBringup.dev || !dst || dwords == 0 || dwords > 64) return false;
	if ((vramOffset & 3) != 0) return false;
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (vramOffset > size || bytes > size - vramOffset) return false;
	Pf540MmCall pfCall(dwords);   // build 0.0.540 (switch 96, T5): the whole call's time by taker, noted at its return
	// 0.0.433 (an internal design note) — MM-WINDOW PRIORITY, BEFORE IOLockLock. See gVramMmLock's comment.
	if (gVramMmLock) {
		const uint32_t nesting = gMmPrioNesting;
		const uintptr_t caller = (uintptr_t)current_thread();
		const int isOwner = n48_mmprio_is_owner(nesting, gMmPrioOwner, caller);
		if (n48_mmprio_should_yield(gMmPrioOn, nesting, isOwner)) {
			// build 0.0.526 (items 1 and 5, gfx_ks81.h): does this caller HOLD a keystone withdrawal marker (hook_unmapVA)?
			// Its spin is accounted to that window; at switch 81 M4 it skips the spin and goes straight to IOLockLock.
			const uint32_t mkh = n48::hw_mkh_gate(caller);
			if (!n48_mkh_gate_skips(mkh)) {
				uint64_t y0 = 0ull, elapsedUs = 0ull;
				clock_get_uptime(&y0);
				while (n48_mmprio_keep_spinning(gMmPrioOn, gMmPrioNesting, elapsedUs)) {
					IODelay(50);
					uint64_t yn = 0ull;
					clock_get_uptime(&yn);
					elapsedUs = navi48_mmprio_us(y0, yn);
				}
				n48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));
				if (mkh) n48::hw_mkh_spin(mkh, elapsedUs);
			}
		}
		// build 0.0.528 item 4 (measurement only, decision-inert): a keystone withdrawal marker holder's wait for gVramMmLock.
		const uint32_t mkw = n48::hw_mkh_holder(caller);
		uint64_t l0 = 0ull;
		if (mkw) clock_get_uptime(&l0);
		if (isOwner) {
			uint64_t o0 = 0ull, o1 = 0ull;
			clock_get_uptime(&o0);
			IOLockLock(gVramMmLock);
			clock_get_uptime(&o1);
			n48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(o0, o1));
		} else {
			IOLockLock(gVramMmLock);
		}
		if (mkw) mm_lockwait_note(l0);
	}
	uint64_t h0 = 0ull; clock_get_uptime(&h0);   // 0.0.514 A3: the hold starts
	for (uint32_t i = 0; i < dwords; i++)
		dst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4);
	if (gVramMmLock) IOLockUnlock(gVramMmLock);
	if (gVramMmLock) mm_hold_note(h0, N48_MMT_LOOKUP, dwords);
	return true;
}

// 0.0.201 — WRITE up to 64 dwords of VRAM at a 0-based offset through the same
// window (amdgpu::WVRAM32_via_mm: MM_INDEX_HI, MM_INDEX, MM_DATA). The only caller is
// the residency copy in Navi48AccelPeer.cpp, which clears every destination with
// navi48_vram_apple_dest_check first and reads each batch back. Refuses rather than
// write unserialised when the window mutex is missing.
// =============================================================================================================================
// build 0.0.512 Part B2 (apple/gfx_clock88.h) — THE CLOCK88 WATCH: READ-ONLY. The policy pass (AppleHardwareHook.cpp, at the
// first S frame's log) adds up to N48_C88_WATCH surfaces; the residency copier (Navi48AccelPeer.cpp, after its COPIED line) and
// navi48_vram_write_mm (below, a write outside any residency copy's own scope) ask whether they touched one, and log at most
// N48_C88_WATCH_LINES lines per boot. Nothing here writes VRAM, a register, a page table or a byte of the caller's buffers: the
// hooks read their arguments and print. tests/gfx_clock88_test.cpp pins these bodies (no write primitive in them).
// =============================================================================================================================
static n48_c88_watch gC88W {};
static void c88_now(uint64_t *wallS, uint32_t *wallUs, uint64_t *upMs) {
	clock_sec_t cs = 0; clock_usec_t cu = 0;
	clock_get_calendar_microtime(&cs, &cu);
	uint64_t t = 0ull, ns = 0ull; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns);
	*wallS = (uint64_t)cs; *wallUs = (uint32_t)cu; *upMs = ns / 1000000ull;
}
uint32_t navi48_c88_watch_add(uint32_t ctx, uint64_t va, uint64_t len, uint64_t vram, uint32_t vramOk, uint32_t *idx) {
	return n48_c88_watch_add_ctx(&gC88W, ctx, va, len, vram, vramOk, idx);
}
// build 0.0.546 (: watches survived unmapVA, so #316 landing at S's old VA read as a HIT): hook_unmapVA, every
// recorded context. One load while no watch is set; a killed watch prints one DEAD line (capped). Nothing is written but gC88W.
void navi48_c88_unmap(uint32_t ctx, uint64_t va, uint64_t size) {
	if (!__atomic_load_n(&gC88W.n, __ATOMIC_ACQUIRE)) return;
	const uint32_t m = n48_c88_unmap(&gC88W, ctx, va, size);
	for (uint32_t i = 0; m && i < N48_C88_WATCH; i++) {
		if (!(m & (1u << i))) continue;
		const uint32_t ln = __atomic_add_fetch(&gC88W.deadLines, 1u, __ATOMIC_RELAXED);
		if (ln > N48_C88_WATCH_LINES) { __atomic_store_n(&gC88W.deadLines, N48_C88_WATCH_LINES, __ATOMIC_RELAXED); continue; }
		N48LOG(N48_C88_DEAD_FMT, i, (unsigned long long)gC88W.va[i], (unsigned long long)gC88W.len[i], ctx, (unsigned long long)va,
		       (unsigned long long)size, (unsigned long long)__atomic_load_n(&gC88W.kills, __ATOMIC_RELAXED), ln, N48_C88_WATCH_LINES);
	}
}
void navi48_c88_watch_state(uint32_t *n, uint64_t *va0, uint64_t *va1, uint32_t *lines, uint64_t *unlogged) {
	const uint32_t k = __atomic_load_n(&gC88W.n, __ATOMIC_ACQUIRE);
	if (n) *n = k;
	if (va0) *va0 = k > 0u ? gC88W.va[0] : 0ull;
	if (va1) *va1 = k > 1u ? gC88W.va[1] : 0ull;
	if (lines) *lines = __atomic_load_n(&gC88W.lines, __ATOMIC_RELAXED);
	if (unlogged) *unlogged = __atomic_load_n(&gC88W.unlogged, __ATOMIC_RELAXED);
}
// An MM-window write outside a residency copy's scope, onto a watched surface's VRAM run: one line per contiguous run of batches.
static __attribute__((noinline)) void c88_mm_note(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const int w = n48_c88_hit_vram(&gC88W, vramOffset, bytes);
	if (w < 0) return;
	const uint64_t runEnd = __atomic_exchange_n(&gC88W.mmRunEnd, vramOffset + bytes, __ATOMIC_RELAXED);
	if (runEnd == vramOffset) return;   // the next batch of a run already logged
	const uint32_t ln = n48_c88_take_line(&gC88W);
	if (!ln) return;
	uint64_t ws = 0ull, um = 0ull; uint32_t wu = 0u; c88_now(&ws, &wu, &um);
	char nm[32] = { 0 }; const int pid = proc_selfpid(); proc_name(pid, nm, (int)sizeof nm);
	N48LOG(N48_C88_MM_FMT, (uint32_t)w, (unsigned long long)gC88W.vram[w], (unsigned long long)gC88W.len[w],
	       (unsigned long long)gC88W.va[w], (unsigned long long)vramOffset, dwords, dwords ? src[0] : 0u,
	       (unsigned long long)ws, wu, (unsigned long long)um, pid, nm, ln, N48_C88_WATCH_LINES);
}

bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {
	if (!gBringup.dev || !src || dwords == 0 || dwords > 64 || !gVramMmLock) return false;
	if ((vramOffset & 3) != 0) return false;
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (vramOffset > size || bytes > size - vramOffset) return false;
	// 0.0.437 — UNSCOPED WRITES SCOPE THEMSELVES, replacing the 0.0.435 pre-write
	// N48_CG_EV_WRITE push and its `gMmPrioNesting > 0` gate. residency_copy_to_vram's own WA/WB writes are always
	// fully CONTAINED in the Navi48CopyScope its pre-flight loop already opened over the copy's bounding range -
	// n48_cg_slot_contains, CONTAINMENT not overlap (a write only half inside an open slot is not covered by it;
	// gfx_copyguard.h's own comment on this function has the reasoning). Every write that is NOT contained (
	// measured this at ~43,000 calls, all in the ringmap setup fill, before any pass is ever judged - so this costs
	// nothing while frames are being decided) opens its OWN scope over exactly [vramOffset, vramOffset+bytes)
	// before the write and closes it, wrote=true, after the write has landed - unconditionally, never gated on
	// whether a pass happens to be active, so it leaves the SAME BEGIN/END ring trail a real residency copy would.
	// 0.0.438: containment counts only a slot OWNED BY THIS CALLING THREAD - a slot another
	// thread opened (a concurrent residency copy, or the switch-39 phantom scope) must never let THIS write skip
	// self-scoping, or the write could end up covered by a scope this thread does not control and that can close
	// at any moment, independent of when the write itself lands.
	const bool cgContained = n48_cg_slot_contains(gCgSlots, N48_CG_SLOTS, vramOffset, vramOffset + bytes,
	                                              (uintptr_t)current_thread());
	int32_t cgSelfSlot = 0; bool cgSelfScoped = false;
	// build 0.0.512 Part B2: the clock88 watch, read-only (one load while no watch is set; a residency copy's own writes are
	// contained in its scope and reported by the copier instead)
	if (!cgContained && __atomic_load_n(&gC88W.n, __ATOMIC_RELAXED)) c88_mm_note(vramOffset, src, dwords);
	if (!cgContained) {
		__atomic_fetch_add(&gCgStats.unscoped, 1ull, __ATOMIC_RELAXED);
		cgSelfSlot = navi48_cg_open(vramOffset, vramOffset + bytes);
		cgSelfScoped = true;
	}
	// 0.0.433 — the same MM-window priority as navi48_vram_read_mm, before IOLockLock. gVramMmLock is non-null here
	// (checked in the guard above), so this always runs, unlike the read side's `if (gVramMmLock)` wrapper.
	{
		const uint32_t nesting = gMmPrioNesting;
		const uintptr_t caller = (uintptr_t)current_thread();
		const int isOwner = n48_mmprio_is_owner(nesting, gMmPrioOwner, caller);
		if (n48_mmprio_should_yield(gMmPrioOn, nesting, isOwner)) {
			// build 0.0.526 (items 1 and 5, gfx_ks81.h): does this caller HOLD a keystone withdrawal marker (hook_unmapVA)?
			// Its spin is accounted to that window; at switch 81 M4 it skips the spin and goes straight to IOLockLock.
			const uint32_t mkh = n48::hw_mkh_gate(caller);
			if (!n48_mkh_gate_skips(mkh)) {
				uint64_t y0 = 0ull, elapsedUs = 0ull;
				clock_get_uptime(&y0);
				while (n48_mmprio_keep_spinning(gMmPrioOn, gMmPrioNesting, elapsedUs)) {
					IODelay(50);
					uint64_t yn = 0ull;
					clock_get_uptime(&yn);
					elapsedUs = navi48_mmprio_us(y0, yn);
				}
				n48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));
				if (mkh) n48::hw_mkh_spin(mkh, elapsedUs);
			}
		}
		// build 0.0.528 item 4 (measurement only, decision-inert): a keystone withdrawal marker holder's wait for gVramMmLock.
		const uint32_t mkw = n48::hw_mkh_holder(caller);
		uint64_t l0 = 0ull;
		if (mkw) clock_get_uptime(&l0);
		if (isOwner) {
			uint64_t o0 = 0ull, o1 = 0ull;
			clock_get_uptime(&o0);
			IOLockLock(gVramMmLock);
			clock_get_uptime(&o1);
			n48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(o0, o1));
		} else {
			IOLockLock(gVramMmLock);
		}
		if (mkw) mm_lockwait_note(l0);
	}
	uint64_t h0 = 0ull; clock_get_uptime(&h0);   // 0.0.514 A3: the hold starts
	for (uint32_t i = 0; i < dwords; i++)
		amdgpu::WVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4, src[i]);
	// No failure path exists between IOLockLock above and here today - the loop above cannot return early - so `ok`
	// is always true. It is kept explicit, rather than folding `true` straight into the return below, so a future
	// failure path added to this loop has somewhere to set it false and this self-scope closes with failed=true
	// instead of silently vouching for a partial write (/item 1, "if that is possible").
	const bool ok = true;
	IOLockUnlock(gVramMmLock);
	mm_hold_note(h0, N48_MMT_LOOKUP, dwords);
	if (cgSelfScoped) navi48_cg_close(cgSelfSlot, vramOffset, vramOffset + bytes, /*wrote=*/true, /*failed=*/!ok, /*mismatch=*/false);
	return ok;
}

// 0.0.201 — may Apple's residency copy write VRAM [off, off + len) (0-based)? Only
// inside our device-only hi pool, and only while that pool has handed out NOTHING:
// the verbatim allocator cannot enumerate live ranges, so bytes_used() == 0 is the
// one exact proof of "no overlap with our allocations" it offers. By construction
// that also keeps the copy off the boot framebuffer [0, 8 MiB), the PSP / GART PT /
// PDB0 / fw_buf reservations below 0x2000000, the BAR0 pool [0x2000000, 0x10000000)
// our rings and self-tests use, and the 768 MiB tail (PSP TMR, IP discovery, Apple's
// page-table arena). Returns 0 = allowed, else 1 no context, 2 no hi pool, 3 outside
// the hi pool, 4 the hi pool has live allocations, 5 empty or misaligned, 6 overflow.
uint32_t navi48_vram_apple_dest_check(uint64_t off, uint64_t len) {
	if (!gBringup.dev || !gBringup.gmc.inited) return 1;
	const auto &g = gBringup.gmc;
	if (!g.vram_alloc_hi.is_inited() || g.vram_hi_size == 0) return 2;
	if (len == 0 || (off & 3) != 0) return 5;
	if (off + len < off) return 6;
	if (off < g.vram_hi_base || off + len > g.vram_hi_base + g.vram_hi_size) return 3;
	if (off < (8ull << 20) || off + len > gBringup.dev->vramSizeBytes) return 3;
	if (g.vram_alloc_hi.bytes_used() != 0) return 4;
	return 0;
}

// =====================================================================================================
// 0.0.272 — MILESTONE 4 ROUTE c' WALL 3: a GPU-side copy into RDNA4FB's scanout.
//
// The copy runs on OUR SDMA0 QUEUE0, the ring the ladder brought up and proved (stage 15's SDMACopyTest),
// never on queues 1-7 / SDMA1 1-2, which `sdmamap` and the drain hand to Apple's eight rings. Apple's
// queues keep their own IOLock inside writeTail; QUEUE0 has no other runtime user (its self-tests run at
// boot), so gScanoutLock serialises every submission of ours and nothing of Apple's is contended.
//
// Addressing, every term already proven on this card:
//   - destination = gmc.vram_start + scanout VRAM offset, the MC convention of every ladder allocation
//     (gpu_va - vram_start = BAR0 offset, gmc_vram_alloc_init); the scanout offset comes from RDNA4FB's own
//     Console,BaseAddress minus OUR BAR0 physical base - two different sources that must agree;
//   - the geometry, clip, overlap and per-row bounds are the pure scanout_copy.h, host-tested;
//   - every row's destination is re-checked with n48_scanout_row_dst_ok right before its packet is built.
// The positive control (mode 1) copies kext-written bars through a pre-flight copy between two scratch
// buffers FIRST (the scanout is not touched unless that lands and reads back), then into a 256x64
// rectangle, and reads every pixel back through BAR0. The surface copy refuses until that has passed.
// =====================================================================================================
#include "apple/scanout_copy.h"
#include "apple/gfx_flipmode.h"         // build 0.0.518: flip mode, switch 74 (pure)
#include "apple/scanout_full.h"         // build 0.0.542: `accel scanout full`, the full-res scanout readback (pure)
#include "apple/display_pipe_guard.h"   // 0.0.412: the pure 16x16 verify cell map / bound (S2)
#include "apple/sdma_gcr.h"             // 0.0.416 (an internal design note): the SDMA GCR_REQ cache rinse (G1-G3)
#include "apple/sdma_dcc.h"             // 0.0.417 (an internal design note): SDMA0_DCC_CNTL no-PTE compression (D1)

// =====================================================================================================
// 0.0.417 (an internal design note, D1) — action 82 `sdmadcc [0|1|2]`.
//
// SDMA0_DCC_CNTL (GC BASE_IDX 0, offset 0x0034) turns on no-PTE read DECOMPRESSION and write COMPRESSION for
// our SDMA0 QUEUE0 (VMID 0, FB aperture passed through —), which is why a uniform block reads back as a
// clear constant. `0` reads SDMA0 and SDMA1 raw and decodes every field per set; `1` CAPTURES SDMA0's current
// value on first use, writes `captured & ~0x00015554` (only the eight *_COMP_EN_n bits), reads back and
// reports MATCH/MISMATCH (idempotent); `2` writes the captured value back. Any other argument is REFUSED, and
// so is `2` before any capture. SDMA1 is READ ONLY and never written; SDMA0_DCC_CNTL is the ONLY register this
// verb writes. The argument rules, the mask and the field decode are the host-tested src/apple/sdma_dcc.h.
// =====================================================================================================
static uint32_t gSdmaDccRestore  { 0 };     // the captured SDMA0_DCC_CNTL value (RESTORE)
static bool     gSdmaDccCaptured { false }; // set once, by the first `sdmadcc 1` of the boot OR by E1's default write below (0.0.418)

uint32_t navi48_sdmadcc_control(uint64_t arg, uint64_t *out, unsigned count) {
	uint64_t v[9] = { 0 };
	const uint32_t op = n48_sdma_dcc_op(arg, gSdmaDccCaptured ? 1 : 0);
	v[0] = op;
	if (op == kN48DccOpRefused) {
		v[7] = kN48DccStBadArg;
		N48LOG("sdmadcc: argument %llu REFUSED (0 reads, 1 set, 2 restore; 2 before any capture is refused)",
		       (unsigned long long)arg);
		goto done;
	}
	if (!gBringup.dev) { v[7] = kN48DccStNoContext; goto done; }
	{
		const uint32_t gc = navi48_gc_base();
		if (!gc) { v[7] = kN48DccStNoGc; goto done; }
		const uint32_t reg0 = gc + N48_SDMA_DCC_CNTL_OFF;
		const uint32_t reg1 = gc + N48_SDMA1_DCC_CNTL_OFF;
		// Both registers are READ on every mode (including before a write) so the report always shows the raw
		// pair. SDMA1 is never written.
		const uint32_t s0 = navi48_reg_read32(reg0);
		const uint32_t s1 = navi48_reg_read32(reg1);
		v[1] = s0; v[2] = s1;
		if (op == kN48DccOpRead) {
			v[7] = kN48DccStOk;
			N48LOG("sdmadcc: SDMA0_DCC_CNTL %#010x (GC[0]+%#x) SDMA1_DCC_CNTL %#010x (GC[0]+%#x); "
			       "force-bypass %u/%u; set0 rd ovr %u comp %u wr ovr %u comp %u; set1 %u %u %u %u; "
			       "set2 %u %u %u %u; set3 %u %u %u %u",
			       s0, (unsigned)N48_SDMA_DCC_CNTL_OFF, s1, (unsigned)N48_SDMA1_DCC_CNTL_OFF,
			       n48_sdma_dcc_force_bypass(s0), n48_sdma_dcc_force_bypass(s1),
			       n48_sdma_dcc_rd_override(s0, 0u), n48_sdma_dcc_rd_comp(s0, 0u),
			       n48_sdma_dcc_wr_override(s0, 0u), n48_sdma_dcc_wr_comp(s0, 0u),
			       n48_sdma_dcc_rd_override(s0, 1u), n48_sdma_dcc_rd_comp(s0, 1u),
			       n48_sdma_dcc_wr_override(s0, 1u), n48_sdma_dcc_wr_comp(s0, 1u),
			       n48_sdma_dcc_rd_override(s0, 2u), n48_sdma_dcc_rd_comp(s0, 2u),
			       n48_sdma_dcc_wr_override(s0, 2u), n48_sdma_dcc_wr_comp(s0, 2u),
			       n48_sdma_dcc_rd_override(s0, 3u), n48_sdma_dcc_rd_comp(s0, 3u),
			       n48_sdma_dcc_wr_override(s0, 3u), n48_sdma_dcc_wr_comp(s0, 3u));
			goto done;
		}
		if (op == kN48DccOpSet) {
			if (!gSdmaDccCaptured) { gSdmaDccRestore = s0; gSdmaDccCaptured = true; }
			const uint32_t target = n48_sdma_dcc_cleared(gSdmaDccRestore);
			v[3] = gSdmaDccRestore; v[4] = target;
			navi48_reg_write32(reg0, target);
			const uint32_t rb = navi48_reg_read32(reg0);
			v[5] = rb; v[6] = (rb == target) ? 1u : 0u;
			v[7] = (rb == target) ? kN48DccStOk : kN48DccStMismatch;
			N48LOG("sdmadcc: SET SDMA0_DCC_CNTL %#010x -> %#010x (mask %#x), read back %#010x %s",
			       gSdmaDccRestore, target, (unsigned)N48_DCC_NOPTE_COMP_EN_MASK, rb,
			       (rb == target) ? "MATCH" : "MISMATCH");
			goto done;
		}
		// op == kN48DccOpRestore (READ and SET were handled above).
		navi48_reg_write32(reg0, gSdmaDccRestore);
		const uint32_t rb = navi48_reg_read32(reg0);
		v[3] = gSdmaDccRestore; v[4] = gSdmaDccRestore; v[5] = rb;
		v[6] = (rb == gSdmaDccRestore) ? 1u : 0u;
		v[7] = (rb == gSdmaDccRestore) ? kN48DccStOk : kN48DccStMismatch;
		N48LOG("sdmadcc: RESTORE SDMA0_DCC_CNTL <- %#010x, read back %#010x %s",
		       gSdmaDccRestore, rb, (rb == gSdmaDccRestore) ? "MATCH" : "MISMATCH");
	}
done:
	v[8] = gSdmaDccCaptured ? 1u : 0u;
	if (out) for (unsigned i = 0; i < count && i < 9; i++) out[i] = v[i];
	return (uint32_t)v[7];
}

// =====================================================================================================
// E1 (0.0.418, an internal design note) — THE SDMA0_DCC_CNTL CLEAR IS THE DEFAULT.
//
// THE SITE, cited: SDMA0's QUEUE0 ring is up when `sdma_init_full` returns in the ladder's SDMAInit stage
// (src/amd/amdgpu_init.cpp, the BringupStage::SDMAInit case). 0.0.657 (GitHub issue #1, fixed on the public repo in
// 1a04d8e): that case now calls this function IMMEDIATELY after `sdma_init_full` succeeds and BEFORE the ladder's own
// SDMA copy test and copy_sweep (through 0.0.656 those two ran first, on the boot default `0x0000aabe` with no-PTE
// compression ON, and their results disagreed from run to run). So from the first SDMA packet of the boot onward,
// `fire`, the copy test, the sweep, the boot chain, `scanout` and the pipe guard all run with no-PTE compression OFF.
//
// The write happens ONCE per boot. `Navi48Bringup::runStages` still calls this function at the first kext-layer
// point after the stage (`ctx.reached >= BringupStage::SDMAInit`); by then it has run, so that call logs that it
// already ran and writes nothing (gSdmaDccDefaultDone, set on the opt-out path and right after the write, NOT on the
// 'no DeviceContext' / 'GC base did not resolve' skips, so a skipped early call is retried by the later one).
//
// It captures the boot value into the SAME RESTORE pair the `sdmadcc` verb uses, writes
// `n48_sdma_dcc_cleared` of it, reads back, and logs ONE line. `navi48-sdmadcc=0` skips it; absent or nonzero
// runs it (n48_sdma_dcc_default_on). SDMA1 is never written and no other register is written at all.
// =====================================================================================================
static bool gSdmaDccDefaultDone { false };   // the default clear (or its opt-out) has been decided this boot
void navi48_sdmadcc_default(void) {
	if (gSdmaDccDefaultDone) {
		N48LOG("sdmadcc: DEFAULT already handled earlier this boot (right after sdma_init_full, before the copy test) - no second write");
		return;
	}
	uint32_t ba = 0;
	const uint32_t present = PE_parse_boot_argn("navi48-sdmadcc", &ba, sizeof(ba)) ? 1u : 0u;
	if (!n48_sdma_dcc_default_on(present, ba)) {
		gSdmaDccDefaultDone = true;
		N48LOG("sdmadcc: DEFAULT SKIPPED - navi48-sdmadcc=0 (SDMA0_DCC_CNTL left as it booted)");
		return;
	}
	if (!gBringup.dev) { N48LOG("sdmadcc: DEFAULT SKIPPED - no DeviceContext"); return; }
	const uint32_t gc = navi48_gc_base();
	if (!gc) { N48LOG("sdmadcc: DEFAULT SKIPPED - GC BASE_IDX 0 did not resolve"); return; }
	const uint32_t reg0 = gc + N48_SDMA_DCC_CNTL_OFF;
	if (!gSdmaDccCaptured) { gSdmaDccRestore = navi48_reg_read32(reg0); gSdmaDccCaptured = true; }
	const uint32_t target = n48_sdma_dcc_cleared(gSdmaDccRestore);
	navi48_reg_write32(reg0, target);
	gSdmaDccDefaultDone = true;
	const uint32_t rb = navi48_reg_read32(reg0);
	N48LOG("sdmadcc: DEFAULT SDMA0_DCC_CNTL %#010x -> %#010x (mask %#x), read back %#010x %s",
	       gSdmaDccRestore, target, (unsigned)N48_DCC_NOPTE_COMP_EN_MASK, rb,
	       (rb == target) ? "MATCH" : "MISMATCH");
}

static IOService *gBringupPci { nullptr };      // our IOPCIDevice, for finding RDNA4FB under it
static IOService *gBringupSelf { nullptr };     // 0.0.286: our Navi48Bringup service, the AGDC nub's provider
IOService *navi48_bringup_pci(void) { return gBringupPci; }
IOService *navi48_bringup_service(void) { return gBringupSelf; }
// 0.0.613 (amd/native_disp.h): the display pipe's window onto the console buffer. No-ops (false) before start() published the service.
bool navi48_console_region(uint64_t *off, uint64_t *len, uint32_t *w, uint32_t *h, uint32_t *rowBytes) {
	return gBringupSelf && static_cast<Navi48Bringup *>(gBringupSelf)->consoleGeometry(off, len, w, h, rowBytes);
}
bool navi48_bar0_write_combined(void) {   // 0.0.615 (W1): `pipe stat` flag bit 32; false before start() published the service
	return gBringupSelf && static_cast<Navi48Bringup *>(gBringupSelf)->bar0WriteCombined();
}
bool navi48_console_write(uint64_t consoleOff, const void *src, size_t len) {
	return gBringupSelf && static_cast<Navi48Bringup *>(gBringupSelf)->consoleWrite(consoleOff, src, len);
}
static IOLock    *gScanoutLock { nullptr };     // every QUEUE0 submission of this feature
static bool       gScanoutPcPassed { false };   // the interlock: positive control passed this boot
static uint32_t   gScanoutPcRuns { 0 }, gScanoutCopies { 0 }, gScanoutRefusals { 0 };
static uint32_t   gScanoutLastReason { 0 };
static uint32_t  *gScanoutSave { nullptr };     // the rectangle the positive control overwrote
static uint32_t   gScanoutSaveX { 0 }, gScanoutSaveY { 0 }, gScanoutSaveW { 0 }, gScanoutSaveH { 0 };
static constexpr uint32_t kScanPcX = 64, kScanPcY = 96, kScanPcW = 256, kScanPcH = 64;
static constexpr uint32_t kScanMaxRows = 256;     // 256*8 + 4 dwords, inside the 4096-dword ring
// 0.0.278: the flush copies are WHOLE FRAMES. The plan admits up to kScanCopyMaxRows rows and the packets go out
// in submissions of kScanChunkRows (200*8 + 4 = 1604 dwords, under scanout_sdma_copies' half-ring bound of 2048), each with
// its own fence, one after the other. The positive control keeps kScanMaxRows. Copy log lines have a budget: a WindowServer
// flushes on every update.
static constexpr uint32_t kScanCopyMaxRows = 2160, kScanChunkRows = 200, kScanCopyLogBudget = 12, kScanCopyFailLogBudget = 24;
static uint32_t   gScanoutCopyCalls { 0 }, gScanoutCopyFailLogs { 0 };
// build 0.0.543 item E3 (the 0.0.542 review's SHOULD-FIX): scanout WRITES into A or B, counted AT THE WRITE (under gScanoutLock,
// immediately before each writer's SDMA copy), not at the writer's entry: gScanoutCopyCalls++ runs before IOLockLock(gScanoutLock), so a
// writer that entered before `scanout full` sampled it and wrote after was invisible to N48_SF_F_WRITTEN_DURING.
static volatile uint32_t gScanoutWrites { 0 };

// Refusal / status codes shared by both entry points (out[0]). 0 = done and verified.
enum : uint32_t {
	kScanStOk = 0, kScanStNoContext = 1, kScanStNoFramebuffer = 2, kScanStGeom = 3, kScanStMcBase = 4,
	kScanStQueue = 5, kScanStQueueBusy = 6, kScanStAlloc = 7, kScanStSourceCtl = 8, kScanStPreflight = 9,
	kScanStFence = 10, kScanStReadback = 11, kScanStPlan = 12, kScanStInterlock = 13, kScanStRing = 14,
	kScanStNoSave = 15,
	// 0.0.416 (an internal design note, G2): the named refusals of `accel scanout 7`'s source window.
	kScanStGcrAlign = 18, kScanStGcrRange = 19, kScanStGcrOverlap = 20,
	// build 0.0.518: a writer into the console refused because flip mode is ON (the display may be
	// scanning B, and A is flip mode's to write: only the flip path and its restore write A or B while the switch is ON).
	kScanStFlipMode = 21,
	// build 0.0.542: `scanout 9` / `scanout 10` refused; out[1] names why (apple/scanout_full.h N48_SF_*).
	kScanStFull = 22,
};

// build 0.0.518: FLIP MODE's state (apple/gfx_flipmode.h; switch 74, DEFAULT OFF). Written only under
// gFmLock (lock order: gFmLock, then gScanoutLock inside the copies); `on` and `engaged` are also read lock-free by the other
// scanout writers and the readers (W4), and `restoreReq` is set lock-free by a withdrawal on the submit path.
static n48_fm  gFm {};
static IOLock *gFmLock { nullptr };

// build 0.0.518 (W4): the buffer the display is SCANNING, for our own readers (the `scanout 3` content read and FNV, the
// `scanout 4` thumbnail). Until flip mode has allocated B this boot it answers the console's offset without reading anything
// (0.0.517's readers exactly); afterwards it asks HUBP0's SURFACE_EARLIEST_INUSE (a read through the bound device) and answers
// B's offset when that names B, else the console's.
static uint64_t fm_displayed_off(uint64_t consoleOff, uint64_t consoleLen) {
	if (!__atomic_load_n(&gFm.haveB, __ATOMIC_ACQUIRE) || gFm.len < consoleLen) return consoleOff;
	uint32_t pending = 0; uint64_t e = 0;
	if (n48dcn::fmReadFront(&pending, &e) != 0) return consoleOff;
	return e == gFm.bMc ? gFm.bOff : consoleOff;
}

// G1: the pure header OWNS the packet, including the 14-dword tiled sub-window the kernel constant
// repeats. Pin them equal so the host-tested arithmetic cannot drift from what the kext emits.
static_assert(N48_SDMA_TILED_SUB_WINDOW_DWORDS == amdgpu::kSDMATiledSubWindowDwords,
              "sdma_gcr.h's tiled sub-window size must equal amdgpu_sdma.h's");

// 0.0.338 — THE PATTERN IS SELF-ADDRESSING, so a wrong value NAMES the source pixel it came from.
// n48_pc_pixel puts bars[x/32] in the low 24 bits and (y & 0xff) in the top byte, so any dword read out of the
// destination decodes back to (source bar, source row) unless it is not a pattern value at all. Returns 0..7, or
// -1 when the low 24 bits are none of the eight bars. (Bar 7 is black, so a zeroed dword decodes as bar 7 —
// recorded in the notes as the one ambiguity in this decode, and it is not load-bearing for the displacement.)
static int scanout_bar_index(uint32_t v) {
	static const uint32_t bars[8] = { 0x00ffffffu, 0x00ffff00u, 0x0000ffffu, 0x0000ff00u,
	                                  0x00ff00ffu, 0x00ff0000u, 0x000000ffu, 0x00000000u };
	for (int b = 0; b < 8; b++) if ((v & 0x00ffffffu) == bars[b]) return b;
	return -1;
}

static bool scanout_read_u64(IOService *svc, const char *key, uint64_t *v) {
	OSNumber *n = OSDynamicCast(OSNumber, svc->getProperty(key));
	if (!n) return false;
	*v = n->unsigned64BitValue();
	return true;
}

// Fill the geometry from RDNA4FB (under OUR PCI function only) and our own device context.
static uint32_t scanout_geometry(amdgpu::DeviceContext &dev, N48ScanoutGeom *g, IOService **fbOut) {
	*fbOut = nullptr;
	bzero(g, sizeof(*g));
	if (!gBringupPci) return kScanStNoContext;
	IOService *fb = nullptr;
	if (OSIterator *it = gBringupPci->getChildIterator(gIOServicePlane)) {
		while (OSObject *o = it->getNextObject()) {
			IOService *svc = OSDynamicCast(IOService, o);
			if (svc && svc->metaCast("IOFramebuffer")) { fb = svc; break; }
		}
		it->release();
	}
	if (!fb) return kScanStNoFramebuffer;
	uint64_t base = 0, len = 0, w = 0, h = 0, rb = 0, depth = 0;
	if (!scanout_read_u64(fb, "Console,BaseAddress", &base) || !scanout_read_u64(fb, "Console,Length", &len) ||
	    !scanout_read_u64(fb, "Console,Width", &w) || !scanout_read_u64(fb, "Console,Height", &h) ||
	    !scanout_read_u64(fb, "Console,RowBytes", &rb) || !scanout_read_u64(fb, "Console,Depth", &depth))
		return kScanStNoFramebuffer;
	g->fbPhys = base; g->fbLen = len; g->width = (uint32_t)w; g->height = (uint32_t)h;
	g->rowBytes = (uint32_t)rb; g->depth = (uint32_t)depth;
	g->bar0Phys = dev.bar0Phys; g->bar0Size = dev.bar0Size; g->vramBase = dev.vramBase;
	g->vramSize = dev.vramSizeBytes;
	*fbOut = fb;
	return kScanStOk;
}

// build 0.0.514 B1/B4: the LIVE framebuffer size - RDNA4FB's Console,Width/Height through scanout_geometry,
// the one source every scanout path already reads. Read-only (registry properties; no register, no VRAM). 1 = both read and
// non-zero; else 0 and *w/*h are left as the caller set them (its fallback).
uint32_t navi48_scanout_live_dims(uint32_t *w, uint32_t *h) {
	if (!w || !h || !gBringup.dev) return 0u;
	N48ScanoutGeom g {};
	IOService *fb = nullptr;
	if (scanout_geometry(*gBringup.dev, &g, &fb) != kScanStOk || !g.width || !g.height) return 0u;
	*w = g.width; *h = g.height;
	return 1u;
}

// QUEUE0 must be up and idle: the engine's read pointer (write-back page, bytes) at our software wptr.
static uint32_t scanout_queue_check(const amdgpu::DeviceContext &dev, const amdgpu::SDMAInstance &inst,
                                    uint32_t *rptrOut) {
	*rptrOut = 0xffffffffu;
	if (!inst.inited || !inst.enabled) return kScanStQueue;
	const uint32_t wb = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBRptrOffset);
	*rptrOut = wb;
	// 0.0.343 — sdma-wptr-freerunning: inst.wptr no longer wraps at the ring size,
	// so BOTH sides of this comparison must be reduced to a ring index before they can be compared.
	// Comparing a free-running wptr against a masked rptr would report BUSY forever.
	if (((wb >> 2) & inst.ring_ptr_mask) != (inst.wptr & inst.ring_ptr_mask)) return kScanStQueueBusy;
	return kScanStOk;
}

// Emit `n` COPY_LINEAR packets (src/dst are MC addresses, `bytes` each) and one FENCE on SDMA0 QUEUE0,
// kick, poll up to `timeoutMs`. Caller holds gScanoutLock and has range-checked every address.
static uint32_t scanout_sdma_copies(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                    const uint64_t *srcMc, const uint64_t *dstMc, const uint32_t *bytes,
                                    uint32_t n, uint32_t fenceValue, uint32_t timeoutMs,
                                    uint64_t *usOut, uint32_t *lastFenceOut) {
	*usOut = 0; *lastFenceOut = 0;
	const uint32_t dwords = n * 8u + 4u;
	if (n == 0 || dwords > inst.ring_size_dwords / 2) return kScanStRing;
	uint32_t *pkt = static_cast<uint32_t *>(IOMalloc(dwords * sizeof(uint32_t)));
	if (!pkt) return kScanStAlloc;
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |
		           amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_LINEAR) | amdgpu::SDMA_PKT_HEADER_CPV(1);
		pkt[k++] = bytes[i] - 1;
		pkt[k++] = 0;
		pkt[k++] = static_cast<uint32_t>(srcMc[i]);
		pkt[k++] = static_cast<uint32_t>(srcMc[i] >> 32);
		pkt[k++] = static_cast<uint32_t>(dstMc[i]);
		pkt[k++] = static_cast<uint32_t>(dstMc[i] >> 32);
		pkt[k++] = 0;
	}
	const uint64_t fenceMc = inst.wb_bus + amdgpu::kSDMAWBCSFenceOffset;
	amdgpu::sdma_wb_write32(dev, inst, amdgpu::kSDMAWBCSFenceOffset, 0);
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
	pkt[k++] = static_cast<uint32_t>(fenceMc);
	pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);
	pkt[k++] = fenceValue;
	amdgpu::amdgpu_hdp_flush(dev);   // rule 39: the engine reads VRAM the CPU just wrote
	const uint32_t wrote = amdgpu::sdma_ring_write(dev, inst, pkt, k);
	IOFree(pkt, dwords * sizeof(uint32_t));
	if (wrote != k) return kScanStRing;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return kScanStRing;
	uint64_t us = 0;
	uint32_t v = 0;
	for (;;) {
		v = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (v == fenceValue) break;
		if (us >= (uint64_t)timeoutMs * 1000u) break;
		if (us < 2000) { IODelay(50); us += 50; } else { IOSleep(1); us += 1000; }
	}
	*usOut = us; *lastFenceOut = v;
	return v == fenceValue ? kScanStOk : kScanStFence;
}

// =====================================================================================================
// build 0.0.496 (an internal design note) — SWITCH 63, THE RESIDENCY COPY THROUGH SDMA.
//
// apple/fastcopy.h holds the argument and every decision; this is its kernel half, beside the SDMA code it reuses:
//   - the staging buffer: ONE physically contiguous 1 MiB system-memory buffer (amdgpu::sysmem_alloc, snooped), bound into the
//     GART ONCE, the first time 63 is turned ON, through gmc_bind_existing - the csb page's own path (0.0.182: gart_bind_sysmem
//     hands out GART offset 0, Apple's write-back page) - and ONLY while the HIGH bump region is in force (navi48-gart-high=1,
//     clear of Apple's pages); the bump offset before and after is logged. Never unbound and never freed.
//   - the positive control, at the first ON: a self-addressing pattern staged, copied by the SAME submission code into a scratch
//     buffer from OUR low allocator, read back IN FULL through the MM window; any wrong dword (or a fence that does not land)
//     latches the path OFF for the boot.
//   - the submission: COPY_LINEAR + FENCE (n48_fc_build_packet: scanout_sdma_copies' packet), on OUR SDMA0 QUEUE0 under
//     gScanoutLock (every QUEUE0 submission of ours is serialised by it), a fence in its OWN write-back dword
//     (N48_FC_WB_FENCE_OFF 0x1C0, not pipeshim's kSDMAWBCSFenceOffset) with a value never used before, a bounded wait.
//   - LOCK ORDER: gFastCopyLock (staging fill through fence) THEN gScanoutLock (ring write + fence wait only). pipeshim never
//     takes gFastCopyLock. The MM verify runs after BOTH are released, against a snapshot taken under gFastCopyLock.
// Writes: the staging buffer's GART PTEs (once), the SDMA0 ring and doorbell, our scratch VRAM (control only) and the copy's own
// pre-checked VRAM range (by the engine). No register write is added; SDMA0_DCC_CNTL is READ per chunk (COMP_EN must be clear).
// =====================================================================================================
#include "apple/fastcopy.h"
static_assert(N48_FC_SDMA_OP_COPY == amdgpu::SDMA_OP_COPY && N48_FC_SDMA_OP_FENCE == amdgpu::SDMA_OP_FENCE &&
              N48_FC_SDMA_SUBOP_COPY_LINEAR == amdgpu::SDMA_SUBOP_COPY_LINEAR,
              "fastcopy.h's packet must be amdgpu_sdma.h's (scanout_sdma_copies' COPY_LINEAR and FENCE)");
static_assert(N48_FC_WB_FENCE_OFF >= amdgpu::kSDMAWBExtFenceOffset + 0x40u && N48_FC_WB_FENCE_OFF + 4u <= amdgpu::kSDMAWBPageBytes,
              "the fast copy's fence dword is past every slot QUEUE0 and the external queues use, inside the write-back page");

static IOLock          *gFastCopyLock { nullptr };   // staging fill through fence (then gScanoutLock inside it)
static volatile uint32_t gFcMode { 0u };             // 0 OFF (boot), N48_FC_M_SAMPLED or N48_FC_M_FULL
// build 0.0.527 (gfx_sk82.h): switch 63's setting, for switch 82's state snapshot. Read only.
uint32_t navi48_fc_mode_now(void) { return n48_fc_mode_of(__atomic_load_n(&gFcMode, __ATOMIC_RELAXED)); }
static n48_fc_state     gFc {};                      // under gFastCopyLock
static n48_fc_stats     gFcS {};                     // chunk totals under gFastCopyLock; fallbacks atomic
static amdgpu::SysMem   gFcStaging {};
static uint64_t         gFcStagingMc { 0 }, gFcBumpBefore { 0 }, gFcBumpAfter { 0 };
static uint32_t         gFcPcWrong { 0 }, gFcPcDone { 0 }, gFcLogN { 0 };
static uint64_t         gFcPcUs { 0 };
static uint64_t         gFcPcOff { 0 };           // build 0.0.534 (review M1): 63's control scratch (VRAM offset; 0 = none)
static volatile uint32_t gFcEverOn { 0u };           // 0.0.509: set by the first ON; until then the scope's close asks nothing

// Per-thread copy slots: the switch LATCHED at a copy's first chunk (pos 0) holds for its later chunks, and the copy's own
// totals feed its `via SDMA` line. Only the owning thread touches a slot's fields; claimed by compare-and-swap.
struct FcSlot {
    uintptr_t thread;
    uint32_t  mode, chunksSdma, chunksMm, lastWhy;
    uint64_t  stageUs, dmaUs, verifyUs, compared, bytes, mismatched;
    // build 0.0.509: the copy's first chunk's VRAM offset (its scope's close is matched by it), the gScanoutLock wait of its
    // submissions (item 1), and its verify's wait for gVramMmLock apart from its reads (item 4).
    uint64_t  at0, lockUs, vLockUs, vReadUs;
    // build 0.0.535 item 3: the COPIED # this copy will print if no other copy completes first (latched at its first chunk)
    uint64_t  copyNo;
};
static constexpr uint32_t kFcSlots = 16u;
static FcSlot gFcSlot[kFcSlots];

static IOLock *fc_lock_get() {
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	if (l) return l;
	IOLock *mine = IOLockAlloc();
	if (!mine) return nullptr;
	IOLock *want = nullptr;
	if (!__atomic_compare_exchange_n(&gFastCopyLock, &want, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { IOLockFree(mine); return want; }
	return mine;
}

static FcSlot *fc_slot_mine() {
	const uintptr_t me = reinterpret_cast<uintptr_t>(current_thread());
	for (uint32_t i = 0; i < kFcSlots; i++)
		if (__atomic_load_n(&gFcSlot[i].thread, __ATOMIC_ACQUIRE) == me) return &gFcSlot[i];
	return nullptr;
}

// build 0.0.510 A1: the copy guard's poison table asked for the copy's range [lo, hi) - any live row (VALID, STICKY or being
// written) overlapping it. gfx_copyguard.h's n48_cg_poison_overlaps over this file's own gCgPoison; read only.
static uint32_t fc_cg_poisoned(uint64_t lo, uint64_t hi) { return n48_cg_poison_overlaps(&gCgPoison, lo, hi) ? 1u : 0u; }

// The latch. pos 0 (a copy's first chunk): read the switch ONCE; ON claims (or reuses) this thread's slot and resets its totals;
// OFF releases any slot this thread still holds (0.0.509: the copy scope's close releases it on every path; this remains the
// backstop). pos > 0: this thread's slot, whatever the switch reads now. nullptr = OFF for this chunk.
// build 0.0.510 A1: the copy's range whose copy-guard poison also forces FULL verify, so the retry of a copy a non-latching
// failure poisoned can clear it. build 0.0.511 (LOW-3): that range is the copy's WHOLE VRAM range [cgLo, cgHi)
// (the pre-flight's bounding range, the copy guard's own scope), not the first chunk's [dAt, dAt + wBytes): a copy whose later
// segment lies elsewhere in VRAM is asked over every byte it writes.
// build 0.0.521 Part D: `rpCand` - the copy is a resprov candidate (the copier's `retile`): FULL verify.
static FcSlot *fc_slot_latch(uint64_t pos, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {
	FcSlot *s = fc_slot_mine();
	if (pos != 0u) return s;
	const uint32_t mode = n48_fc_mode_of(__atomic_load_n(&gFcMode, __ATOMIC_ACQUIRE));
	if (!mode) {
		if (s) __atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);
		return nullptr;
	}
	if (!s) {
		const uintptr_t me = reinterpret_cast<uintptr_t>(current_thread());
		for (uint32_t i = 0; i < kFcSlots && !s; i++) {
			uintptr_t want = 0;
			if (__atomic_compare_exchange_n(&gFcSlot[i].thread, &want, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) s = &gFcSlot[i];
		}
		if (!s) { __atomic_fetch_add(&gFcS.fallback[N48_FC_MM_NO_SLOT], 1ull, __ATOMIC_RELAXED); return nullptr; }
	}
	// build 0.0.509 F-3: sampled verify on a copy that overlaps a range switch 62 registered (ic_begin bumped the heap
	// generation, before this first chunk) runs FULL verify for the whole copy.
	// build 0.0.510 A1: ... and so does a copy whose range overlaps a live copy-guard poison row (only a FULL read-back
	// clears it: n48_fc_cg_close_wrote).
	const uint32_t b62 = navi48_ic_bumped_mine(), cgp = fc_cg_poisoned(cgLo, cgHi);
	const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);
	if (eff != mode) __atomic_fetch_add(b62 ? &gFcS.forced_full : cgp ? &gFcS.forced_full_cg : &gFcS.forced_full_rp, 1ull, __ATOMIC_RELAXED);
	__atomic_fetch_add(eff == N48_FC_M_FULL ? &gFcS.latched_full : &gFcS.latched_sampled, 1ull, __ATOMIC_RELAXED);   // 0.0.521 D
	s->mode = eff; s->chunksSdma = s->chunksMm = s->lastWhy = 0u;
	s->stageUs = s->dmaUs = s->verifyUs = s->compared = s->bytes = s->mismatched = 0ull;
	s->at0 = ~0ull; s->lockUs = s->vLockUs = s->vReadUs = 0ull;
	s->copyNo = navi48_peer_copy_seq() + 1u;   // build 0.0.535 item 3: for switch 89's per-chunk NOT TAKEN line
	return s;
}

static uint64_t fc_us(uint64_t a, uint64_t b) { uint64_t ns = 0; absolutetime_to_nanoseconds(b - a, &ns); return ns / 1000u; }

// SDMA0 QUEUE0 up and idle, and the MC base the scanout path proved. Takes gScanoutLock briefly (inside gFastCopyLock).
static uint32_t fc_sdma_up() {
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) return 0u;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	if (!inst.inited || !inst.enabled || !inst.wb_bus || gBringup.gmc.vram_start != dev.vramMcBase) return 0u;
	IOLockLock(gScanoutLock);
	uint32_t rptr = 0;
	const uint32_t st = scanout_queue_check(dev, inst, &rptr);
	IOLockUnlock(gScanoutLock);
	return st == kScanStOk ? 1u : 0u;
}

// SDMA0_DCC_CNTL as it reads now (a READ; 0xffffffff when it cannot be read, which has every COMP_EN bit set: refused).
static uint32_t fc_dcc_raw() {
	if (!gBringup.dev) return 0xffffffffu;
	const uint32_t gc = navi48_gc_base();
	return gc ? navi48_reg_read32(gc + N48_SDMA_DCC_CNTL_OFF) : 0xffffffffu;
}

// The submission context: which destination check applies (a residency copy: the VRAM guard; the control: our scratch only).
struct FcSub {
	uint64_t dAt;          // the chunk's 0-based VRAM offset
	uint64_t pcLo, pcHi;   // control only: the scratch buffer [pcLo, pcHi) (0 = a residency copy)
	uint64_t tSubmit;      // uptime at the submission's entry (the end of the staging fill)
	uint64_t dmaUs;        // 0.0.509: the fence wait counted FROM THE KICK (0.0.508: from the entry, lock wait included)
	uint64_t lockUs;       // 0.0.509: the wait for gScanoutLock (lock taken minus entry)
	uint32_t lastFence;
	N48FcFillFn fill; void *fillCtx;
};

// build 0.0.509 item 1: fastcopy.h's n48_fc_submit_wait holds the ORDER (entry, gScanoutLock, the lock wait, the kick, THEN
// the bound's clock); these are its kernel callbacks. fc_kick is 0.0.508's gate, packet, ring write and doorbell, unchanged.
static uint64_t fc_now_us(void *) { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000u; }
static void fc_lock(void *) { IOLockLock(gScanoutLock); }
static void fc_unlock(void *) { IOLockUnlock(gScanoutLock); }
static uint32_t fc_fence_read(void *) { return amdgpu::sdma_wb_read32(*gBringup.dev, gBringup.sdma.instance[0], N48_FC_WB_FENCE_OFF); }
static void fc_delay(void *, uint32_t d) { if (d >= 1000u) IOSleep(d / 1000u); else IODelay(d); }
static uint32_t fc_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	FcSub *c = static_cast<FcSub *>(vc);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	uint32_t rptr = 0;
	const uint32_t idle = (scanout_queue_check(dev, inst, &rptr) == kScanStOk && inst.wb_bus &&
	                       gBringup.gmc.vram_start == dev.vramMcBase && dstMc == gBringup.gmc.vram_start + c->dAt) ? 1u : 0u;
	// THE LAST GATE: the VRAM guard for exactly this chunk (a residency copy), or our own scratch (the control).
	const uint32_t destWhy = c->pcHi ? ((c->dAt >= c->pcLo && c->dAt + bytes <= c->pcHi) ? 0u : 99u)
	                                 : navi48_vram_apple_dest_check(c->dAt, bytes);
	if (!n48_fc_submit_ok(destWhy, idle)) return N48_FC_SUB_REFUSED;
	uint32_t pkt[N48_FC_PKT_DWORDS];
	const uint32_t k = n48_fc_build_packet(pkt, N48_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + N48_FC_WB_FENCE_OFF, fence);
	if (k != N48_FC_PKT_DWORDS) return N48_FC_SUB_REFUSED;
	amdgpu::sysmem_wmb();            // the staged bytes are in memory before the engine is told to read them (snooped GART)
	amdgpu::amdgpu_hdp_flush(dev);   // as scanout_sdma_copies
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return N48_FC_SUB_RING;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return N48_FC_SUB_RING;
	return 0u;
}
static const n48_fc_wait_ops kFcWaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &fc_kick, &fc_fence_read, &fc_delay };

static uint32_t fc_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	FcSub *c = static_cast<FcSub *>(vc);
	uint64_t t0 = 0; clock_get_uptime(&t0);
	c->tSubmit = t0; c->dmaUs = 0; c->lastFence = 0; c->lockUs = 0;
	if (!gBringup.dev || !gScanoutLock) return N48_FC_SUB_REFUSED;
	n48_fc_wait_out w {};
	const uint32_t r = n48_fc_submit_wait(&kFcWaitOps, c, srcMc, dstMc, bytes, fence, &w);
	c->lockUs = w.lock_us;
	if (w.kicked) { c->dmaUs = w.el_us; c->lastFence = w.seen; }
	return r;
}

static int fc_fill(void *vc, uint8_t *dst, uint64_t off, uint32_t take) {
	FcSub *c = static_cast<FcSub *>(vc);
	return c->fill ? c->fill(c->fillCtx, dst, off, take) : 0;
}
// build 0.0.509 item 4: the verify's read. navi48_vram_read_mm's body (bounds, the MM-window priority yield, gVramMmLock,
// the reads), with the wait for gVramMmLock - the MM-priority yield included - timed apart from the reads. Only the fast copy's
// verify calls it (switch 63 ON); navi48_vram_read_mm itself is unchanged.
struct FcVerifyT { uint64_t lockT, readT; };   // uptime ticks (whole-us rounding per 1-dword read would lose the answer)
static bool fc_vram_read_timed(uint64_t vramOffset, uint32_t *dst, uint32_t dwords, FcVerifyT *t) {
	if (!gBringup.dev || !dst || dwords == 0 || dwords > 64) return false;
	if ((vramOffset & 3) != 0) return false;
	const uint64_t bytes = (uint64_t)dwords * 4u;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (vramOffset > size || bytes > size - vramOffset) return false;
	uint64_t w0 = 0ull, w1 = 0ull, r1 = 0ull;
	clock_get_uptime(&w0);
	if (gVramMmLock) {
		const uint32_t nesting = gMmPrioNesting;
		const uintptr_t caller = (uintptr_t)current_thread();
		const int isOwner = n48_mmprio_is_owner(nesting, gMmPrioOwner, caller);
		if (n48_mmprio_should_yield(gMmPrioOn, nesting, isOwner)) {
			// build 0.0.526 (items 1 and 5, gfx_ks81.h): does this caller HOLD a keystone withdrawal marker (hook_unmapVA)?
			// Its spin is accounted to that window; at switch 81 M4 it skips the spin and goes straight to IOLockLock.
			const uint32_t mkh = n48::hw_mkh_gate(caller);
			if (!n48_mkh_gate_skips(mkh)) {
				uint64_t y0 = 0ull, elapsedUs = 0ull;
				clock_get_uptime(&y0);
				while (n48_mmprio_keep_spinning(gMmPrioOn, gMmPrioNesting, elapsedUs)) {
					IODelay(50);
					uint64_t yn = 0ull;
					clock_get_uptime(&yn);
					elapsedUs = navi48_mmprio_us(y0, yn);
				}
				n48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));
				if (mkh) n48::hw_mkh_spin(mkh, elapsedUs);
			}
		}
		// build 0.0.528 item 4 (measurement only, decision-inert): a keystone withdrawal marker holder's wait for gVramMmLock.
		const uint32_t mkw = n48::hw_mkh_holder(caller);
		uint64_t l0 = 0ull;
		if (mkw) clock_get_uptime(&l0);
		if (isOwner) {
			uint64_t o0 = 0ull, o1 = 0ull;
			clock_get_uptime(&o0);
			IOLockLock(gVramMmLock);
			clock_get_uptime(&o1);
			n48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(o0, o1));
		} else {
			IOLockLock(gVramMmLock);
		}
		if (mkw) mm_lockwait_note(l0);
	}
	clock_get_uptime(&w1);
	for (uint32_t i = 0; i < dwords; i++)
		dst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, vramOffset + (uint64_t)i * 4);
	clock_get_uptime(&r1);
	if (gVramMmLock) IOLockUnlock(gVramMmLock);
	if (gVramMmLock) mm_hold_note(w1, N48_MMT_FCVERIFY);   // 0.0.514 A3
	if (t) { t->lockT += w1 - w0; t->readT += r1 - w1; }
	return true;
}
static int fc_mm_read(void *vt, uint64_t vram, uint32_t *dst, uint32_t dwords) {
	return fc_vram_read_timed(vram, dst, dwords, static_cast<FcVerifyT *>(vt)) ? 1 : 0;
}
// build 0.0.514 A1: THE SAMPLED VERIFY'S READ, ONE gVramMmLock ACQUIRE PER CHUNK. fc3 spent 22.85 s of
// its 23.4 s of verify WAITING for this lock because 0.0.513 took it once per sampled dword (116,409 acquires, ~198 us each).
// Every check runs BEFORE the lock (the device, the lock, the chunk's shape, the plan's size, [dAt, dAt + n) inside VRAM); then
// the MM-priority yield (switch 37) exactly as navi48_vram_read_mm runs it, ONCE; then ONE IOLockLock, only RVRAM32_via_mm reads
// at the plan's offsets (n48_fc_sample_at, the same offsets the snapshot was taken at) between it and the IOLockUnlock (plus the
// two clock reads that time them), and the timing kept as fc_vram_read_timed keeps it (wait = w0..w1, reads = w1..r1). No log
// call, no allocation, no other lock under gVramMmLock. At most N48_FC_SAMPLE_MAX (258) dwords: ~0.43 ms held at fc3's
// 1.66 us per read. Unlike navi48_vram_read_mm it REFUSES when gVramMmLock is missing (never reads the window unserialised).
static bool fc_vram_read_sampled_timed(uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *dst, uint32_t ns, FcVerifyT *t) {
	if (!gBringup.dev || !dst || !gVramMmLock) return false;
	if ((dAt & 3) != 0 || n == 0u || (n & 3u) != 0u) return false;
	if (ns == 0u || ns > N48_FC_SAMPLE_MAX || ns > n48_fc_sample_count(n)) return false;
	const uint64_t size = gBringup.dev->vramSizeBytes;
	if (dAt > size || (uint64_t)n > size - dAt) return false;
	uint64_t w0 = 0ull, w1 = 0ull, r1 = 0ull;
	clock_get_uptime(&w0);
	const uint32_t nesting = gMmPrioNesting;
	const int isOwner = n48_mmprio_is_owner(nesting, gMmPrioOwner, (uintptr_t)current_thread());
	if (n48_mmprio_should_yield(gMmPrioOn, nesting, isOwner)) {
		uint64_t y0 = 0ull, elapsedUs = 0ull;
		clock_get_uptime(&y0);
		while (n48_mmprio_keep_spinning(gMmPrioOn, gMmPrioNesting, elapsedUs)) {
			IODelay(50);
			uint64_t yn = 0ull;
			clock_get_uptime(&yn);
			elapsedUs = navi48_mmprio_us(y0, yn);
		}
		n48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));
	}
	IOLockLock(gVramMmLock);
	clock_get_uptime(&w1);
	for (uint32_t i = 0; i < ns; i++)
		dst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, dAt + n48_fc_sample_at(n, rot, i));
	clock_get_uptime(&r1);
	IOLockUnlock(gVramMmLock);
	if (isOwner) n48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(w0, w1));
	mm_hold_note(w1, N48_MMT_FCVERIFY);
	if (t) { t->lockT += w1 - w0; t->readT += r1 - w1; }
	return true;
}
static int fc_mm_read_plan(void *vt, uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
	return fc_vram_read_sampled_timed(dAt, n, rot, got, ns, static_cast<FcVerifyT *>(vt)) ? 1 : 0;
}

// The control's pattern: self-addressing (the low 24 bits name the dword), top byte 0xA5 - never the 0xDEADBEEF poison.
static inline uint32_t fc_pc_word(uint32_t i) { return 0xA5000000u | (i & 0x00FFFFFFu); }
static int fc_pc_fill(void *, uint8_t *dst, uint64_t off, uint32_t take) {
	for (uint32_t i = 0; i < take; i += 4u) { const uint32_t w = fc_pc_word((uint32_t)((off + i) / 4u)); memcpy(dst + i, &w, 4); }
	return 1;
}

// Staging, once (under gFastCopyLock). 0 = bound; else why not (logged).
static uint32_t fc_staging_bind() {
	if (gFc.staging_ok) return 0u;
	if (!gBringup.dev || !gBringup.gfxhubReady || !gBringup.gmc.inited) { N48LOG("fastcopy63: STAGING REFUSED - no GART yet"); return 1u; }
	if (!gBringup.gmc.gart_high_bump) {
		N48LOG("fastcopy63: STAGING REFUSED - the GART bump region is LOW (navi48-gart-high is not 1): the staging buffer would be "
		       "bound among Apple's pages. The path stays OFF (every copy keeps the MM window)");
		return 2u;
	}
	if (amdgpu::sysmem_alloc(gFcStaging, N48_FC_STAGING_BYTES, 4096) != KERN_SUCCESS || !gFcStaging.valid()) {
		N48LOG("fastcopy63: STAGING REFUSED - no physically contiguous %u-byte system-memory buffer", N48_FC_STAGING_BYTES);
		return 3u;
	}
	gFcBumpBefore = gBringup.gmc.gart_bump_offset;
	uint64_t mc = 0;
	const kern_return_t kr = amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gFcStaging.bus, N48_FC_STAGING_BYTES, &mc);
	gFcBumpAfter = gBringup.gmc.gart_bump_offset;
	if (kr != KERN_SUCCESS || !mc || mc == gBringup.gmc.gart_start) {
		N48LOG("fastcopy63: STAGING REFUSED - gmc_bind_existing %#x, MC %#llx (GART page 0 is Apple's write-back page); the buffer "
		       "is kept, unbound", (unsigned)kr, (unsigned long long)mc);
		return 4u;
	}
	gFcStagingMc = mc;
	gFc.staging_ok = 1u;
	N48LOG("fastcopy63: STAGING bound - %u bytes, sysmem bus %#llx cpu %p -> GART MC %#llx (%u PTEs, SYSMEM_RW, snooped); GART bump "
	       "offset (nextFreeOffset) %#llx -> %#llx of %#llx, bump floor %#llx (HIGH)", N48_FC_STAGING_BYTES,
	       (unsigned long long)gFcStaging.bus, gFcStaging.cpu, (unsigned long long)mc, N48_FC_STAGING_BYTES / 4096u,
	       (unsigned long long)gFcBumpBefore, (unsigned long long)gFcBumpAfter, (unsigned long long)gBringup.gmc.gart_size,
	       (unsigned long long)gBringup.gmc.gart_bump_start);
	return 0u;
}

// THE POSITIVE CONTROL (under gFastCopyLock). 0 = passed.
static uint32_t fc_positive_control() {
	gFcPcDone = 1u;
	amdgpu::VRAMAllocation a {};
	bool have = false;
	uint64_t t0 = 0; clock_get_uptime(&t0);
	if (!gBringup.dev || !gScanoutLock) return 1u;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	IOLockLock(gScanoutLock);
	have = gBringup.gmc.vram_alloc.alloc(N48_FC_STAGING_BYTES, 65536, &a);
	IOLockUnlock(gScanoutLock);
	if (!have) { N48LOG("fastcopy63: CONTROL not run - no %u-byte scratch in our allocator", N48_FC_STAGING_BYTES); return 2u; }
	const uint64_t off = a.gpu_va - gBringup.gmc.vram_start;
	gFcPcOff = off;                                         // build 0.0.534 (review M1): switch 89's control line names it
	if (off + N48_FC_STAGING_BYTES > dev.bar0Size) {
		IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock);
		N48LOG("fastcopy63: CONTROL not run - scratch VRAM %#llx is outside BAR0", (unsigned long long)off);
		return 3u;
	}
	for (uint32_t i = 0; i < N48_FC_STAGING_BYTES; i += 4u) amdgpu::WBAR0_32(dev, off + i, 0xdeadbeefu);   // our scratch only
	amdgpu::amdgpu_hdp_flush(dev);
	FcSub sc {}; sc.dAt = off; sc.pcLo = off; sc.pcHi = off + N48_FC_STAGING_BYTES; sc.fill = &fc_pc_fill; sc.fillCtx = nullptr;
	const n48_fc_ops ops { &sc, &fc_fill, &fc_submit };
	uint32_t *exp = static_cast<uint32_t *>(IOMalloc(N48_FC_STAGING_BYTES));
	if (!exp) {
		IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock);
		return 4u;
	}
	const uint32_t r = n48_fc_chunk_run(&ops, &gFc, static_cast<uint8_t *>(gFcStaging.cpu), gFcStagingMc, a.gpu_va,
	                                    N48_FC_STAGING_BYTES, exp, N48_FC_M_FULL, 0u, 0xffffffffull, nullptr);
	uint64_t cmp = 0; uint32_t rf = 0, first[4] = { 0 }, firstIdx[4] = { 0 }, nf = 0;
	gFcPcWrong = 0;
	if (r == N48_FC_RUN_OK) {
		for (uint32_t i = 0; i < N48_FC_STAGING_BYTES / 4u; i += 64u) {
			uint32_t buf[64];
			if (!navi48_vram_read_mm(off + (uint64_t)i * 4u, buf, 64u)) { rf = 1u; break; }
			for (uint32_t j = 0; j < 64u; j++) {
				if (buf[j] == fc_pc_word(i + j) && buf[j] == exp[i + j]) continue;
				if (nf < 4u) { first[nf] = buf[j]; firstIdx[nf] = i + j; nf++; }
				gFcPcWrong++;
			}
			cmp += 64u;
		}
	}
	IOFree(exp, N48_FC_STAGING_BYTES);
	uint64_t t1 = 0; clock_get_uptime(&t1);
	gFcPcUs = fc_us(t0, t1);
	// the scratch goes back only when the engine is known to be done with it (a landed fence)
	if (r == N48_FC_RUN_OK) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock); }
	const bool pass = (r == N48_FC_RUN_OK) && !rf && cmp == N48_FC_STAGING_BYTES / 4u && gFcPcWrong == 0u;
	N48LOG("fastcopy63: POSITIVE CONTROL %s - %u-byte pattern staged at GART MC %#llx, SDMA0 QUEUE0 COPY_LINEAR -> our scratch "
	       "VRAM %#llx, fence %#010x (run %u, dma %llu us); MM-window read-back %llu of %u dwords, %u WRONG%s; first wrong "
	       "[%u]=%08x [%u]=%08x; %llu us in all", pass ? "PASSED" : "FAILED - the SDMA path is LATCHED OFF for this boot",
	       N48_FC_STAGING_BYTES, (unsigned long long)gFcStagingMc, (unsigned long long)off, sc.lastFence, r,
	       (unsigned long long)sc.dmaUs, (unsigned long long)cmp, N48_FC_STAGING_BYTES / 4u, gFcPcWrong,
	       rf ? " (the MM window refused a read)" : "", firstIdx[0], first[0], firstIdx[1], first[1], (unsigned long long)gFcPcUs);
	if (pass) { gFc.pc_passed = 1u; return 0u; }
	n48_fc_latch(&gFc, N48_FC_LATCH_PC);
	return 5u;
}

// `gfxneuter 63 | M << 8` (AppleHardwareHook.cpp calls this for M 1, 2, 3 after its mid-arm guard). 0 = done; 9 = the lock
// could not be allocated (unchanged); 12 = the switch is set but the path cannot copy (no staging, or the control did not pass).
uint32_t navi48_fc_set(uint32_t m) {
	if (m == N48_FC_M_OFF) { __atomic_store_n(&gFcMode, 0u, __ATOMIC_RELEASE); return 0u; }
	if (!n48_fc_mode_of(m)) return 11u;
	IOLock *l = fc_lock_get();
	if (!l) return 9u;
	IOLockLock(l);
	__atomic_store_n(&gFcEverOn, 1u, __ATOMIC_RELEASE);     // 0.0.509: from now on the copy scope's close asks the slot
	uint32_t st = 0u;
	if (!gFc.latched_off) {
		if (fc_staging_bind() != 0u) st = 12u;
		else if (!gFc.pc_passed && fc_positive_control() != 0u) st = 12u;
	} else st = 12u;
	__atomic_store_n(&gFcMode, m, __ATOMIC_RELEASE);
	IOLockUnlock(l);
	return st;
}

// =====================================================================================================
// build 0.0.534 ( ranked item 1; apple/fastcopy89.h) — SWITCH 89, A FULL CHUNK VERIFIED BY AN SDMA READ-BACK.
// fastcopy89.h holds the argument and the order (n48_fc89_chunk); this is its kernel half:
//   - the read-back buffer: ONE more physically contiguous 1 MiB system-memory buffer (amdgpu::sysmem_alloc: cached, snooped),
//     bound into the GART ONCE by gmc_bind_existing at the first `gfxneuter 89 | 1 << 8` - on the VERB thread, never in the copy
//     path - only while the HIGH bump region is in force, exactly as 63's staging buffer (fc_staging_bind). Never unbound, never freed.
//   - its positive control at that first ON: a self-addressing pattern written into scratch VRAM from OUR low allocator through BAR0
//     (63's control's own pattern and writes), the buffer poisoned with the pattern's complement, the scratch copied back by the SAME
//     read-back submission, every dword compared; a wrong dword or a fence that did not land retires the buffer for the boot.
//   - the read-back submission (fc89_kick): fc_kick's gate with the directions swapped - the SOURCE must be vram_start + the chunk's
//     own offset, the DESTINATION exactly the read-back buffer's MC and no longer than it - then fastcopy.h's packet, the ring write
//     and the doorbell on OUR SDMA0 QUEUE0 under gScanoutLock, the fence in 63's own write-back dword with a value never used before.
//   - LOCK ORDER: gFastCopyLock (poison through compare) THEN gScanoutLock (ring write + fence wait), as 63. The MM cross-check runs
//     after gFastCopyLock is released (fastcopy89.h step 5), through 63's one-acquire sampled read (fc_vram_read_sampled_timed).
// Writes: the read-back buffer's GART PTEs (once), our scratch VRAM (control only), the SDMA0 ring and doorbell. The engine writes only
// the read-back buffer (and the fence dword). No register write is added; SDMA0_DCC_CNTL is READ before each read-back.
// =====================================================================================================
#include "apple/fastcopy89.h"
static volatile uint32_t gFc89Mode { 0u };           // switch 89: 0 OFF (boot, default), N48_FC89_M_ON (1) or N48_FC89_M_SHADOW (3)
static uint64_t          gFc89PcOff { 0 };           // the previous switch-89 control's scratch (VRAM offset; 0 = none)
// 0.0.534 review item 2: navi48_fc_chunk reads the mode ONCE per chunk, BEFORE its allocation and gFastCopyLock, and hands it on.
static uint32_t fc89_mode_now() { return n48_fc89_mode_of(__atomic_load_n(&gFc89Mode, __ATOMIC_ACQUIRE)); }
static n48_fc89_state    gFc89 {};                   // under gFastCopyLock
static n48_fc89_stats    gFc89S {};                  // under gFastCopyLock
static amdgpu::SysMem    gFc89Rb {};
static uint64_t          gFc89RbMc { 0 };
static uint32_t          gFc89LogN { 0 };

// build 0.0.535 item 3 (; fastcopy89.h n48_fc89_qwait): the bounded queue-idle wait's callbacks, and what the last
// kick answered (under gFastCopyLock, like gFc89S): the NOT TAKEN reason and the chunk's wait.
static uint32_t gFc89NtWhy { N48_FC89_NT_OTHER };
static uint64_t gFc89WaitUs { 0 };
static uint32_t gFc89NtLines { 0 };
static uint32_t fc89_q_idle(void *) {
	uint32_t r = 0;
	return scanout_queue_check(*gBringup.dev, gBringup.sdma.instance[0], &r) == kScanStOk ? 1u : 0u;
}
static const n48_fc89_qwait_ops kFc89QWait = { nullptr, &fc89_q_idle, &fc_now_us, &fc_delay };
static uint32_t fc89_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	FcSub *c = static_cast<FcSub *>(vc);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	uint32_t rptr = 0;
	uint32_t qs = scanout_queue_check(dev, inst, &rptr);
	gFc89WaitUs = 0u;
	// build 0.0.535 item 3: QUEUE0 not idle right after the forward write's fence - wait for it, bounded (<= 2 ms, counted).
	if (qs == kScanStQueueBusy) {
		const uint32_t ok = n48_fc89_qwait(&kFc89QWait, N48_FC89_QWAIT_US, &gFc89WaitUs);
		n48_fc89_qwait_note(&gFc89S, ok, gFc89WaitUs);
		if (ok) qs = kScanStOk;
	}
	gFc89NtWhy = qs == kScanStQueueBusy ? N48_FC89_NT_QBUSY : N48_FC89_NT_OTHER;
	const uint32_t idle = (qs == kScanStOk && inst.wb_bus &&
	                       gBringup.gmc.vram_start == dev.vramMcBase && srcMc == gBringup.gmc.vram_start + c->dAt) ? 1u : 0u;
	// THE LAST GATE: the engine may write ONLY our read-back buffer, and no more of it than it holds.
	const uint32_t destWhy = (gFc89Rb.valid() && gFc89RbMc && dstMc == gFc89RbMc && bytes != 0u && bytes <= N48_FC_STAGING_BYTES) ? 0u : 98u;
	if (n48_fc_submit_ok(destWhy, idle) != 1u) return N48_FC_SUB_REFUSED;
	uint32_t pkt[N48_FC_PKT_DWORDS];
	const uint32_t k = n48_fc_build_packet(pkt, N48_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + N48_FC_WB_FENCE_OFF, fence);
	if (k != N48_FC_PKT_DWORDS) return N48_FC_SUB_REFUSED;
	amdgpu::sysmem_wmb();            // the poison is in memory before the engine is told to overwrite it (snooped GART)
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return N48_FC_SUB_RING;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return N48_FC_SUB_RING;
	return 0u;
}
static const n48_fc_wait_ops kFc89WaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &fc89_kick, &fc_fence_read, &fc_delay };
// The chunk's callbacks (fastcopy89.h n48_fc89_ops): `sub` the read-back's submission record, `l` gFastCopyLock, `vt` the cross-check's
// MM-lock / read timing.
struct Fc89Ctx { FcSub sub; IOLock *l; FcVerifyT vt; };
static uint32_t fc89_wait(FcSub *c, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	uint64_t t0 = 0; clock_get_uptime(&t0);
	c->tSubmit = t0; c->dmaUs = 0; c->lastFence = 0; c->lockUs = 0;
	if (!gBringup.dev || !gScanoutLock) return N48_FC_SUB_REFUSED;
	n48_fc_wait_out w {};
	const uint32_t r = n48_fc_submit_wait(&kFc89WaitOps, c, srcMc, dstMc, bytes, fence, &w);
	c->lockUs = w.lock_us;
	if (w.kicked) { c->dmaUs = w.el_us; c->lastFence = w.seen; }
	amdgpu::sysmem_rmb();            // the fence was read landed: the buffer's reads come after it
	return r;
}
static uint32_t fc89_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	return fc89_wait(&static_cast<Fc89Ctx *>(vc)->sub, srcMc, dstMc, bytes, fence);
}
static void fc89_unlock(void *vc) { IOLockUnlock(static_cast<Fc89Ctx *>(vc)->l); }
static int fc89_plan(void *vc, uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
	return fc_vram_read_sampled_timed(dAt, n, rot, got, ns, &static_cast<Fc89Ctx *>(vc)->vt) ? 1 : 0;
}

// The read-back buffer, once (under gFastCopyLock, on the verb thread). 0 = bound; else why not (logged).
static uint32_t fc89_rb_bind() {
	if (gFc89Rb.valid() && gFc89RbMc) return 0u;
	if (!gBringup.dev || !gBringup.gfxhubReady || !gBringup.gmc.inited) { N48LOG("fastcopy89: READ-BACK BUFFER REFUSED - no GART yet"); return 1u; }
	if (!(gBringup.gmc.gart_high_bump)) {
		N48LOG("fastcopy89: READ-BACK BUFFER REFUSED - the GART bump region is LOW (navi48-gart-high is not 1): switch 89 stays inert");
		return 2u;
	}
	if (!gFc89Rb.valid() && (amdgpu::sysmem_alloc(gFc89Rb, N48_FC_STAGING_BYTES, 4096) != KERN_SUCCESS || !gFc89Rb.valid())) {
		N48LOG("fastcopy89: READ-BACK BUFFER REFUSED - no physically contiguous %u-byte system-memory buffer", N48_FC_STAGING_BYTES);
		return 3u;
	}
	const uint64_t b0 = gBringup.gmc.gart_bump_offset;
	uint64_t mc = 0;
	const kern_return_t kr = amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gFc89Rb.bus, N48_FC_STAGING_BYTES, &mc);
	const uint64_t b1 = gBringup.gmc.gart_bump_offset;
	if (kr != KERN_SUCCESS || !mc || mc == gBringup.gmc.gart_start) {
		N48LOG("fastcopy89: READ-BACK BUFFER REFUSED - gmc_bind_existing %#x, MC %#llx; the buffer is kept, unbound", (unsigned)kr,
		       (unsigned long long)mc);
		return 4u;
	}
	gFc89RbMc = mc;
	N48LOG("fastcopy89: READ-BACK BUFFER bound - %u bytes, sysmem bus %#llx cpu %p -> GART MC %#llx (%u PTEs, SYSMEM_RW, snooped); GART "
	       "bump offset %#llx -> %#llx of %#llx", N48_FC_STAGING_BYTES, (unsigned long long)gFc89Rb.bus, gFc89Rb.cpu,
	       (unsigned long long)mc, N48_FC_STAGING_BYTES / 4096u, (unsigned long long)b0, (unsigned long long)b1,
	       (unsigned long long)gBringup.gmc.gart_size);
	return 0u;
}

// THE READ DIRECTION's POSITIVE CONTROL (under gFastCopyLock). 0 = passed (gFc89.rb_ok = 1). A wrong dword, or a fence that did not
// land (fastcopy.h n48_fc_fence_outcome: 63's path latched OFF too), retires the buffer for the boot; "not run" leaves it for a retry.
static uint32_t fc89_control() {
	if (!gBringup.dev || !gScanoutLock || !gFc89Rb.valid() || !gFc89RbMc) return 1u;
	if (!fc_sdma_up()) { N48LOG("fastcopy89: CONTROL not run - SDMA0 QUEUE0 not up or busy"); return 2u; }
	if (fc_dcc_raw() & N48_DCC_NOPTE_COMP_EN_MASK) { N48LOG("fastcopy89: CONTROL not run - SDMA0_DCC_CNTL has a COMP_EN bit set"); return 3u; }
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::VRAMAllocation a {};
	IOLockLock(gScanoutLock);
	const bool have = gBringup.gmc.vram_alloc.alloc(N48_FC_STAGING_BYTES, 65536, &a);
	IOLockUnlock(gScanoutLock);
	if (!have) { N48LOG("fastcopy89: CONTROL not run - no %u-byte scratch in our allocator", N48_FC_STAGING_BYTES); return 4u; }
	const uint64_t off = a.gpu_va - gBringup.gmc.vram_start;
	if (off + N48_FC_STAGING_BYTES > dev.bar0Size) {
		IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock);
		N48LOG("fastcopy89: CONTROL not run - scratch VRAM %#llx is outside BAR0", (unsigned long long)off);
		return 5u;
	}
	const uint32_t fence = n48_fc_fence_next(&gFc);
	if (!fence) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock); return 6u; }
	gFc89.pc_done = 1u; gFc89.pc_wrong = 0u;
	// 0.0.534 review M1: a FRESH pattern - never 63's (fc_pc_word) and never the previous switch-89 control's - so a read-back that
	// returned stale VRAM from either can never pass.
	const uint32_t nonce = n48_fc89_pc_nonce(fence, gFc89.pc_nonce);
	const uint64_t prevOff = gFc89PcOff;
	gFc89.pc_nonce = nonce; gFc89PcOff = off;
	uint64_t t0 = 0; clock_get_uptime(&t0);
	for (uint32_t i = 0; i < N48_FC_STAGING_BYTES; i += 4u) amdgpu::WBAR0_32(dev, off + i, n48_fc89_pc_word(i / 4u, nonce));   // our scratch only
	amdgpu::amdgpu_hdp_flush(dev);   // as 63's control: the BAR0 pattern is in VRAM before the engine reads it
	uint32_t *rb = static_cast<uint32_t *>(gFc89Rb.cpu);
	for (uint32_t i = 0; i < N48_FC_STAGING_BYTES / 4u; i++) rb[i] = ~n48_fc89_pc_word(i, nonce);
	FcSub sc {}; sc.dAt = off;
	const uint32_t sub = fc89_wait(&sc, a.gpu_va, gFc89RbMc, N48_FC_STAGING_BYTES, fence);
	const uint32_t run = n48_fc_fence_outcome(&gFc, sub);
	uint32_t first[2] = { 0u, 0u }, firstIdx[2] = { 0u, 0u };
	if (run == N48_FC_RUN_OK) gFc89.pc_wrong = n48_fc89_pc_check(rb, N48_FC_STAGING_BYTES / 4u, nonce, firstIdx, first);
	uint64_t t1 = 0; clock_get_uptime(&t1);
	// the scratch goes back only when the engine is known to be done with it (a landed fence) or never told of it (refused)
	if (run == N48_FC_RUN_OK || run == N48_FC_RUN_REFUSED) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a); IOLockUnlock(gScanoutLock); }
	const bool pass = run == N48_FC_RUN_OK && gFc89.pc_wrong == 0u;
	if (pass) gFc89.rb_ok = 1u;
	else if (run != N48_FC_RUN_REFUSED) gFc89.rb_retired = 1u;
	N48LOG("fastcopy89: POSITIVE CONTROL %s - pattern 0x3C|i^%#08x BAR0-written to scratch VRAM %#llx (63's control %#llx, previous "
	       "89 control %#llx), SDMA -> GART MC %#llx (poisoned), fence %#010x (run %u, dma %llu us); %u of %u WRONG; first [%u]=%08x "
	       "[%u]=%08x; %llu us", pass ? "PASSED" : run == N48_FC_RUN_REFUSED ? "NOT RUN - refused before the ring (retried at the next ON)"
	       : "FAILED - switch 89 stays inert this boot", nonce, (unsigned long long)off, (unsigned long long)gFcPcOff,
	       (unsigned long long)prevOff, (unsigned long long)gFc89RbMc, sc.lastFence, run, (unsigned long long)sc.dmaUs, gFc89.pc_wrong,
	       N48_FC_STAGING_BYTES / 4u, firstIdx[0], first[0], firstIdx[1], first[1], (unsigned long long)fc_us(t0, t1));
	return pass ? 0u : 7u;
}

// `gfxneuter 89 | M << 8` (AppleHardwareHook.cpp calls this for M 1 and 2 after its mid-arm guard). 0 = done; 9 = no lock; 11 = an
// unknown M; 12 = the switch is set but the read-back cannot run (every FULL chunk keeps the MM verify).
uint32_t navi48_fc89_set(uint32_t m) {
	if (m == N48_FC89_M_OFF) { __atomic_store_n(&gFc89Mode, 0u, __ATOMIC_RELEASE); return 0u; }
	if (!n48_fc89_mode_of(m)) return 11u;
	IOLock *l = fc_lock_get();
	if (!l) return 9u;
	IOLockLock(l);
	uint32_t st = 0u;
	if (gFc89.rb_retired) st = 12u;
	else if (!gFc89.rb_ok) {
		if (fc89_rb_bind() != 0u) st = 12u;
		else if (fc89_control() != 0u) st = 12u;
	}
	__atomic_store_n(&gFc89Mode, m, __ATOMIC_RELEASE);   // N48_FC89_M_ON or N48_FC89_M_SHADOW
	IOLockUnlock(l);
	return st;
}

void navi48_fc89_report(const char *why) {
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	if (l) IOLockLock(l);
	N48LOG(N48_FC89_FMT, N48_FC89_ARGS(fc89_mode_now(), why, &gFc89, &gFc89S));
	N48LOG(N48_FC89_SH_FMT, (unsigned long long)gFc89S.sh_chunks, (unsigned long long)gFc89S.sh_agree,
	       (unsigned long long)gFc89S.sh_disagree, (unsigned long long)gFc89S.sh_dis_dwords, (unsigned long long)gFc89S.sh_noview,
	       (unsigned long long)gFc89S.sh_rb_bad, (unsigned long long)gFc89S.sh_mm_bad);
	N48LOG(N48_FC89_QW_FMT, N48_FC89_QWAIT_US, (unsigned long long)gFc89S.qwaits, (unsigned long long)gFc89S.qwait_idle,   // 0.0.535
	       (unsigned long long)gFc89S.qwait_busy, (unsigned long long)gFc89S.qwait_max_us, (unsigned long long)gFc89S.nt_qbusy,
	       (unsigned long long)gFc89S.nt_other, gFc89NtLines, N48_FC89_NT_LINES);
	if (l) IOLockUnlock(l);
}

// ONE FULL CHUNK WHOSE WRITE LANDED (navi48_fc_chunk, under gFastCopyLock `l`). 0 = NOT TAKEN, `l` STILL HELD, nothing freed:
// navi48_fc_chunk's own verify runs exactly as 0.0.533's. Else the chunk's result word with `l` RELEASED and `exp` freed.
// `m89`: the mode navi48_fc_chunk read BEFORE its allocation; `exp` then carries n48_fc89_extra(m89, n) bytes past the n / 4
// snapshot dwords (0.0.534 review item 2: nothing is allocated here, under gFastCopyLock).
static __attribute__((noinline)) uint64_t fc89_shadow(FcSlot *s, IOLock *l, uint64_t dAt, uint64_t n, uint32_t *exp, size_t expBytes,
                                                      uint32_t rot, uint64_t stageUs, const FcSub *wsc);
// build 0.0.535 item 3: ONE CHUNK NOT TAKEN (under gFastCopyLock): counted by reason, and named - copy #, chunk
// index (the copy loop's chunks are N48_FC_STAGING_BYTES apart), VRAM range, mode, reason, the chunk's queue wait - for the first
// N48_FC89_NT_LINES of the boot.
static __attribute__((noinline)) void fc89_nt(const FcSlot *s, uint64_t pos, uint64_t dAt, uint64_t n, uint32_t m89, uint32_t why,
                                              uint64_t waitUs) {
	if (why == N48_FC89_NT_QBUSY) gFc89S.nt_qbusy++; else gFc89S.nt_other++;
	if (!n48_fc89_nt_line(&gFc89NtLines)) return;
	N48LOG(N48_FC89_NT_FMT, (unsigned long long)s->copyNo, (unsigned long long)(pos / N48_FC_STAGING_BYTES), (unsigned long long)dAt,
	       (unsigned long long)n, m89 == N48_FC89_M_SHADOW ? "SHADOW" : "ON", n48_fc89_nt_name(why), (unsigned long long)waitUs,
	       (unsigned long long)gFc89S.qwait_max_us);
}
static __attribute__((noinline)) uint64_t fc89_chunk(FcSlot *s, IOLock *l, uint64_t pos, uint64_t dAt, uint64_t n, uint32_t *exp,
                                                     size_t expBytes, uint32_t rot, uint64_t stageUs, const FcSub *wsc, uint32_t m89) {
	if (!m89) return 0ull;                                                         // OFF: nothing read, nothing written
	if (!n48_fc89_use(1u, s->mode, &gFc89, gFc.latched_off) || (fc_dcc_raw() & N48_DCC_NOPTE_COMP_EN_MASK) ||
	    expBytes < (size_t)(n + n48_fc89_extra(m89, n))) {
		gFc89S.not_taken++;                                                        // the read would decompress, or not usable
		fc89_nt(s, pos, dAt, n, m89, N48_FC89_NT_OTHER, 0u);
		return 0ull;
	}
	gFc89NtWhy = N48_FC89_NT_OTHER; gFc89WaitUs = 0u;                              // a refusal before the kick is "other"
	if (m89 == N48_FC89_M_SHADOW) {
		const uint64_t rs = fc89_shadow(s, l, dAt, n, exp, expBytes, rot, stageUs, wsc);
		if (!rs) fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs);           // NOT TAKEN: `l` still held
		return rs;
	}
	uint32_t *got = exp + n / 4u;                                                  // allocated with exp, before the lock
	Fc89Ctx c {}; c.sub.dAt = dAt; c.l = l;
	const n48_fc89_ops ops { &c, &fc89_submit, &fc89_unlock, &fc89_plan };
	n48_fc89_out o {};
	uint64_t t0 = 0, t1 = 0; clock_get_uptime(&t0);
	const uint64_t r = n48_fc89_chunk(&ops, &gFc, &gFc89, static_cast<uint32_t *>(gFc89Rb.cpu), gFc89RbMc,
	                                  gBringup.gmc.vram_start + dAt, dAt, exp, got, (uint32_t)n, rot, &o);
	clock_get_uptime(&t1);
	if (!r) { gFc89S.not_taken++; fc89_nt(s, pos, dAt, n, m89, gFc89NtWhy, gFc89WaitUs); return 0ull; }   // refused before its ring write: `l` held
	// `l` RELEASED by n48_fc89_chunk (step 5, or its failure exit).
	s->stageUs += stageUs; s->dmaUs += wsc->dmaUs; s->lockUs += wsc->lockUs;      // the WRITE's, as navi48_fc_chunk adds them
	IOFree(exp, expBytes);
	const uint64_t vUs = fc_us(t0, t1), vLockUs = fc_us(0ull, c.vt.lockT), vReadUs = fc_us(0ull, c.vt.readT);
	if (o.run != N48_FC89_OK) {
		// THE READ-BACK's fence did not land (or its ring write failed): the chunk FAILS exactly as a write timeout (the path is
		// latched OFF and the staging retired by n48_fc_fence_outcome; the read-back buffer retired too), and the range is poisoned
		// for the boot as navi48_fc_chunk poisons a dead range. No compare is credited.
		n48_cg_poison_mark_sticky(&gCgPoison, dAt, dAt + n);
		__atomic_fetch_add(&gFcS.dead_ranges, 1ull, __ATOMIC_RELAXED);
		navi48_ic_chunk_dead(pos, dAt, n);
		IOLockLock(l);
		if (o.run == N48_FC89_TIMEOUT) gFc89S.timeouts++; else gFc89S.ring_fail++;
		IOLockUnlock(l);
		N48LOG("fastcopy63: CHUNK FAILED at resource offset %#llx (VRAM %#llx, %#llx bytes): switch 89's READ-BACK run %u - %s; fence "
		       "want %#010x seen %#010x after %llu us from the kick; path LATCHED OFF for the boot; the range is a DEAD RANGE, poisoned "
		       "for the boot", (unsigned long long)pos, (unsigned long long)dAt, (unsigned long long)n, o.sub, n48_fc_fail_text(r),
		       o.fence, c.sub.lastFence, (unsigned long long)c.sub.dmaUs);
		return r;
	}
	const uint32_t failed = (r & N48_FC_R_FAIL) ? 1u : 0u;                          // only the cross-check's refused MM read
	const uint64_t cmp = failed ? 0ull : ((r >> 32) & 0x3FFFFFFFull), bad = failed ? 0ull : (uint64_t)(uint32_t)r;
	s->chunksSdma++; s->verifyUs += vUs; s->compared += cmp; s->bytes += n; s->mismatched += bad;
	s->vLockUs += vLockUs; s->vReadUs += vReadUs;
	IOLockLock(l);
	gFcS.verify_us += vUs; gFcS.mismatched += bad;
	gFcS.vlock_us += vLockUs; gFcS.vread_us += vReadUs;
	gFcS.full_dwords += cmp;
	if (o.xread_fail) { gFcS.verify_read_fail++; gFc89S.xread_fail++; }
	gFc89S.chunks++; gFc89S.bytes += n; gFc89S.mismatches += o.rb_bad; gFc89S.xmismatches += o.x_bad;
	gFc89S.us += vUs; gFc89S.dma_us += c.sub.dmaUs; gFc89S.xcheck_us += vLockUs + vReadUs;
	if (bad || o.rb_bad || o.x_bad) n48_fc_latch(&gFc, N48_FC_LATCH_MISMATCH);   // a wrong dword the engine wrote: no further copy uses it
	IOLockUnlock(l);
	if (bad || o.rb_bad || o.x_bad)
		N48LOG("fastcopy63: VERIFY MISMATCH - %llu of %llu dword(s) compared at VRAM %#llx (%#llx bytes) differ from the staged "
		       "stream (switch 89: SDMA read-back %llu, MM cross-check %llu of %u sampled): the copy is poisoned (read-back mismatch) and "
		       "the SDMA path is LATCHED OFF for the boot", (unsigned long long)(o.rb_bad + o.x_bad), (unsigned long long)(n / 4u),
		       (unsigned long long)dAt, (unsigned long long)n, (unsigned long long)o.rb_bad, (unsigned long long)o.x_bad, o.ns);
	else if (gFc89LogN < 8u) {
		gFc89LogN++;
		N48LOG("fastcopy89: chunk VRAM %#llx (%#llx bytes) READ BACK by SDMA in full (%llu dwords, dma %llu us) and cross-checked "
		       "through the MM window (%u sampled, %s): verify %llu us", (unsigned long long)dAt, (unsigned long long)n,
		       (unsigned long long)(n / 4u), (unsigned long long)c.sub.dmaUs, o.ns, o.xread_fail ? "READ REFUSED" : "match",
		       (unsigned long long)vUs);
	}
	return r;
}

// SWITCH 89 SHADOW (857; 0.0.534 review item 3). Under gFastCopyLock `l`: phase A (fastcopy89.h n48_fc89_shadow_a) - the read-back
// into the buffer, copied out to `sd` (= exp + n / 4, allocated before the lock); 0 = NOT TAKEN, `l` STILL HELD (today's path runs).
// Else `l` is released and TODAY'S full MM verify runs - n48_fc_verify over the same fc_mm_read, only wrapped by n48_fc89_shadow_read,
// which records where the MM and SDMA views differ - and navi48_fc_chunk's own tail follows, deciding by the MM verify ALONE
// (n48_fc89_shadow_decide): the same credit, the same mismatch latch and line, the same verify-read failure.
static __attribute__((noinline)) uint64_t fc89_shadow(FcSlot *s, IOLock *l, uint64_t dAt, uint64_t n, uint32_t *exp, size_t expBytes,
                                                      uint32_t rot, uint64_t stageUs, const FcSub *wsc) {
	uint32_t *sd = exp + n / 4u;
	Fc89Ctx c {}; c.sub.dAt = dAt; c.l = l;
	const n48_fc89_ops ops { &c, &fc89_submit, &fc89_unlock, &fc89_plan };
	n48_fc89_out o {};
	uint32_t sdOk = 0u;
	if (!n48_fc89_shadow_a(&ops, &gFc, &gFc89, static_cast<uint32_t *>(gFc89Rb.cpu), gFc89RbMc, gBringup.gmc.vram_start + dAt, exp, sd,
	                       (uint32_t)n, &o, &sdOk)) { gFc89S.not_taken++; return 0ull; }
	// `l` RELEASED. From here: navi48_fc_chunk's tail, with the verify's read wrapped.
	s->stageUs += stageUs; s->dmaUs += wsc->dmaUs; s->lockUs += wsc->lockUs;
	uint64_t v0 = 0, v1 = 0, cmp = 0; uint32_t rf = 0;
	FcVerifyT vt { 0ull, 0ull };
	n48_fc89_shadow_rec rec {};
	rec.read = &fc_mm_read; rec.ctx = &vt; rec.sd = sd; rec.sd_ok = sdOk; rec.nd = (uint32_t)(n / 4u); rec.d_at = dAt;
	clock_get_uptime(&v0);
	const uint64_t bad = n48_fc_verify(&n48_fc89_shadow_read, nullptr, &rec, dAt, exp, nullptr, (uint32_t)n, N48_FC_M_FULL, rot,
	                                   &cmp, &rf);
	clock_get_uptime(&v1);
	IOFree(exp, expBytes);
	const uint64_t r = n48_fc89_shadow_decide(bad, cmp, rf, o.run, o.rb_bad);
	const uint64_t vUs = fc_us(v0, v1), vLockUs = fc_us(0ull, vt.lockT), vReadUs = fc_us(0ull, vt.readT);
	s->chunksSdma++; s->verifyUs += vUs; s->compared += cmp; s->bytes += n; s->mismatched += bad;
	s->vLockUs += vLockUs; s->vReadUs += vReadUs;
	IOLockLock(l);
	gFcS.verify_us += vUs; gFcS.mismatched += bad;
	gFcS.vlock_us += vLockUs; gFcS.vread_us += vReadUs;
	gFcS.full_dwords += cmp;
	if (rf) gFcS.verify_read_fail++;
	if (bad) n48_fc_latch(&gFc, N48_FC_LATCH_MISMATCH);   // a wrong dword the engine wrote: no further copy uses it this boot
	gFc89S.sh_chunks++;
	if (!sdOk) gFc89S.sh_noview++;
	else if (rec.dis) { gFc89S.sh_disagree++; gFc89S.sh_dis_dwords += rec.dis; }
	else gFc89S.sh_agree++;
	gFc89S.sh_rb_bad += o.rb_bad; gFc89S.sh_mm_bad += bad;
	if (o.run == N48_FC89_TIMEOUT) gFc89S.timeouts++; else if (o.run == N48_FC89_RING) gFc89S.ring_fail++;
	gFc89S.dma_us += c.sub.dmaUs;
	const uint32_t logDis = rec.dis && gFc89LogN < 64u;
	if (logDis) gFc89LogN++;
	IOLockUnlock(l);
	if (bad)
		N48LOG("fastcopy63: VERIFY MISMATCH - %llu of %llu dword(s) compared at VRAM %#llx (%#llx bytes) differ from the staged "
		       "stream: the copy is poisoned (read-back mismatch) and the SDMA path is LATCHED OFF for the boot",
		       (unsigned long long)bad, (unsigned long long)cmp, (unsigned long long)dAt, (unsigned long long)n);
	if (logDis)
		N48LOG(N48_FC89_SH_DIS_FMT, (unsigned long long)dAt, (unsigned long long)n, (unsigned long long)rec.dis,
		       (unsigned long long)rec.compared, rec.off[0], rec.mm[0], rec.sdv[0], rec.off[1], rec.mm[1], rec.sdv[1], rec.off[2],
		       rec.mm[2], rec.sdv[2], rec.off[3], rec.mm[3], rec.sdv[3]);
	if (o.run != N48_FC89_OK)
		N48LOG("fastcopy89: SHADOW read-back of VRAM %#llx (%#llx bytes) did not land (run %u, fence want %#010x seen %#010x): the SDMA "
		       "path is LATCHED OFF and the read-back buffer retired; this chunk was decided by the MM verify", (unsigned long long)dAt,
		       (unsigned long long)n, o.sub, o.fence, c.sub.lastFence);
	return r;
}

// ONE CHUNK (Navi48AccelPeer.cpp fc_copy_chunk, per chunk of the residency copy, AFTER the copy loop's own VRAM-guard check of
// exactly [dAt, dAt + n)). Returns 0 (the MM loop runs this chunk: nothing staged, nothing written) or N48_FC_R_TAKEN with the
// result (fastcopy.h). A FAILED result poisons through the copy loop's own FAILED path.
uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,
                         uint8_t *lead, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {
	FcSlot *s = fc_slot_latch(pos, cgLo, cgHi, rpCand);
	if (!s || !s->mode) return 0ull;                       // OFF (latched): the MM loop, exactly 0.0.495's
	if (pos == 0u) s->at0 = dAt;                           // 0.0.509: the scope's close finds this copy's slot by it
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	const uint32_t full = (s->mode == N48_FC_M_FULL) ? 1u : 0u;
	const bool shapeOk = n != 0u && n <= N48_FC_STAGING_BYTES && (n & 3u) == 0u;
	// 0.0.514 A1: sampled - the snapshot (N48_FC_SAMPLE_MAX dwords) and, after it, the plan read's destination (as many again)
	const uint32_t m89 = full ? fc89_mode_now() : 0u;      // build 0.0.534 (switch 89): read once, before the allocation
	const size_t expBytes = full ? (size_t)(n + n48_fc89_extra(m89, n)) : (size_t)N48_FC_SAMPLE_MAX * 8u;
	uint32_t *exp = (l && shapeOk) ? static_cast<uint32_t *>(IOMalloc(expBytes)) : nullptr;
	uint32_t why = N48_FC_MM_NO_STAGING;
	if (l) {
		IOLockLock(l);
		why = n48_fc_decide(1u, fc_sdma_up(), fc_dcc_raw(), gFc.pc_passed, gFc.latched_off, n48_fc_staging_usable(&gFc), wBytes, dAt, n);
		if (why == N48_FC_SDMA && !exp) why = N48_FC_MM_NO_MEM;
		if (why != N48_FC_SDMA) IOLockUnlock(l);
	}
	if (why != N48_FC_SDMA) {
		if (exp) IOFree(exp, expBytes);
		s->chunksMm++; s->lastWhy = why;
		if (why < N48_FC_MM_REASONS) __atomic_fetch_add(&gFcS.fallback[why], 1ull, __ATOMIC_RELAXED);
		return 0ull;
	}
	// gFastCopyLock held: the staging buffer is this chunk's alone until the fence has landed (or the path is latched off).
	uint64_t t0 = 0; clock_get_uptime(&t0);
	FcSub sc {}; sc.dAt = dAt; sc.fill = fill; sc.fillCtx = fillCtx;
	const n48_fc_ops ops { &sc, &fc_fill, &fc_submit };
	const uint32_t rot = (uint32_t)gFcS.chunks_sdma;
	const uint32_t r = n48_fc_chunk_run(&ops, &gFc, static_cast<uint8_t *>(gFcStaging.cpu), gFcStagingMc,
	                                    gBringup.gmc.vram_start + dAt, (uint32_t)n, exp, s->mode, rot, pos, lead);
	const uint64_t stageUs = sc.tSubmit ? fc_us(t0, sc.tSubmit) : 0ull;
	if (r == N48_FC_RUN_OK) { gFcS.chunks_sdma++; gFcS.bytes_sdma += n; gFcS.stage_us += stageUs; gFcS.dma_us += sc.dmaUs; }
	else if (r == N48_FC_RUN_FILL) gFcS.fill_fail++;
	else if (r == N48_FC_RUN_REFUSED) gFcS.refused++;
	else if (r == N48_FC_RUN_RING) gFcS.ring_fail++;
	if (sc.tSubmit) {                                      // 0.0.509 item 1: the submission's wait for gScanoutLock
		gFcS.lock_waits++; gFcS.lock_wait_us += sc.lockUs;
		if (sc.lockUs > gFcS.lock_wait_max_us) gFcS.lock_wait_max_us = sc.lockUs;
	}
	const uint32_t latched = gFc.latched_off, timeouts = gFc.timeouts, lastFence = sc.lastFence, fenceWant = gFc.fence_last;
	// build 0.0.534 (switch 89, fastcopy89.h): a FULL chunk whose write landed is verified by an SDMA read-back + a sampled MM
	// cross-check. fc89_chunk answers 0 with gFastCopyLock STILL HELD (89 OFF, not usable, refused): everything below is 0.0.533's.
	if (r == N48_FC_RUN_OK && full) { const uint64_t r89 = fc89_chunk(s, l, pos, dAt, n, exp, expBytes, rot, stageUs, &sc, m89); if (r89) return r89; }
	IOLockUnlock(l);
	s->stageUs += stageUs; s->dmaUs += sc.dmaUs; s->lockUs += sc.lockUs;
	if (r != N48_FC_RUN_OK) {
		IOFree(exp, expBytes);
		// build 0.0.510 A1: refused before the ring was written at the copy's FIRST chunk - nothing of this copy is written -
		// so the MM loop runs this chunk (a declined chunk) instead of failing the copy: no poison is left for a retry to clear.
		if (n48_fc_refused_to_mm(r, pos)) {
			__atomic_fetch_add(&gFcS.refused_mm, 1ull, __ATOMIC_RELAXED);
			s->chunksMm++; s->lastWhy = N48_FC_MM_NO_SDMA;
			if (gFcLogN < 64u) {
				gFcLogN++;
				N48LOG("fastcopy63: FIRST CHUNK REFUSED before the ring was written (VRAM %#llx, %#llx bytes; gScanoutLock wait %llu us) - "
				       "nothing of this copy is written, so the MM window runs it instead of failing the copy (refused to MM %llu)",
				       (unsigned long long)dAt, (unsigned long long)n, (unsigned long long)sc.lockUs,
				       (unsigned long long)__atomic_load_n(&gFcS.refused_mm, __ATOMIC_RELAXED));
			}
			return 0ull;
		}
		// build 0.0.509 item 2 (F-2): a fence that did not land (or a ring write/doorbell that failed part-way) leaves a
		// chunk a LATE DMA may still write: its VRAM range is poisoned for the rest of the boot - the copy guard's sticky row
		// and, for a copy that bumped 62, 62's sticky entry - BEFORE the copy loop's FAILED path closes the copy's scope.
		const uint32_t dead = n48_fc_run_dead(r);
		if (dead) {
			n48_cg_poison_mark_sticky(&gCgPoison, dAt, dAt + n);
			__atomic_fetch_add(&gFcS.dead_ranges, 1ull, __ATOMIC_RELAXED);
			navi48_ic_chunk_dead(pos, dAt, n);
		}
		if (gFcLogN < 64u || r == N48_FC_RUN_TIMEOUT || r == N48_FC_RUN_RING) {
			gFcLogN++;
			N48LOG("fastcopy63: CHUNK FAILED at resource offset %#llx (VRAM %#llx, %#llx bytes): run %u - %s; fence want %#010x seen "
			       "%#010x after %llu us from the kick (gScanoutLock wait %llu us before it); timeouts %u, path %s%s",
			       (unsigned long long)pos, (unsigned long long)dAt,
			       (unsigned long long)n, r, n48_fc_fail_text(n48_fc_result_fail(r)), fenceWant, lastFence,
			       (unsigned long long)sc.dmaUs, (unsigned long long)sc.lockUs, timeouts, latched ? "LATCHED OFF for the boot" : "still usable",
			       dead ? "; the range is a DEAD RANGE, poisoned for the boot" : "");
		}
		return n48_fc_result_fail(r);
	}
	// THE VERIFY, after both locks are released, against the snapshot taken under gFastCopyLock.
	uint64_t v0 = 0, v1 = 0, cmp = 0; uint32_t rf = 0;
	FcVerifyT vt { 0ull, 0ull };                           // 0.0.509 item 4: gVramMmLock wait apart from the reads
	clock_get_uptime(&v0);
	const uint64_t bad = n48_fc_verify(&fc_mm_read, &fc_mm_read_plan, &vt, dAt, exp, full ? nullptr : exp + N48_FC_SAMPLE_MAX,
	                                   (uint32_t)n, s->mode, rot, &cmp, &rf);
	clock_get_uptime(&v1);
	IOFree(exp, expBytes);
	const uint64_t vUs = fc_us(v0, v1), vLockUs = fc_us(0ull, vt.lockT), vReadUs = fc_us(0ull, vt.readT);
	s->chunksSdma++; s->verifyUs += vUs; s->compared += cmp; s->bytes += n; s->mismatched += bad;
	s->vLockUs += vLockUs; s->vReadUs += vReadUs;
	IOLockLock(l);
	gFcS.verify_us += vUs; gFcS.mismatched += bad;
	gFcS.vlock_us += vLockUs; gFcS.vread_us += vReadUs;
	if (full) gFcS.full_dwords += cmp; else gFcS.sampled += cmp;
	if (rf) gFcS.verify_read_fail++;
	if (bad) n48_fc_latch(&gFc, N48_FC_LATCH_MISMATCH);   // a wrong dword the engine wrote: no further copy uses it this boot
	IOLockUnlock(l);
	if (bad)
		N48LOG("fastcopy63: VERIFY MISMATCH - %llu of %llu dword(s) compared at VRAM %#llx (%#llx bytes) differ from the staged "
		       "stream: the copy is poisoned (read-back mismatch) and the SDMA path is LATCHED OFF for the boot",
		       (unsigned long long)bad, (unsigned long long)cmp, (unsigned long long)dAt, (unsigned long long)n);
	if (rf) return n48_fc_result_fail(N48_FC_RUN_VERIFY_READ);
	return n48_fc_result_ok(cmp, bad);
}

// After the COPIED line (Navi48AccelPeer.cpp): this copy's transport, when switch 63 was ON for it. 0.0.509: the slot is NOT
// released here any more - the copy scope's close (navi48_fc_scope_closed) reads it first, on every path, then releases it.
void navi48_fc_copy_report(uint64_t copyNo) {
	FcSlot *s = fc_slot_mine();
	if (!s) return;
	if (s->mode && s->chunksSdma) {
		__atomic_fetch_add(&gFcS.copies_sdma, 1ull, __ATOMIC_RELAXED);
		const uint64_t us = s->stageUs + s->dmaUs + s->verifyUs + s->lockUs;   // 0.0.509: dma no longer holds the lock wait
		N48LOG("residency-copy: #%llu was copied VIA SDMA (not the MM window): %u chunk(s) via SDMA, %u via the MM window; stage %llu "
		       "us, dma %llu us, verify %llu us (%s %llu dword(s), %llu MISMATCHED); %llu bytes, %llu MB/s; waits: scanout-lock %llu us, "
		       "verify MM-lock %llu us, reads %llu us", (unsigned long long)copyNo,
		       s->chunksSdma, s->chunksMm, (unsigned long long)s->stageUs, (unsigned long long)s->dmaUs,
		       (unsigned long long)s->verifyUs, s->mode == N48_FC_M_FULL ? "full" : "sampled", (unsigned long long)s->compared,
		       (unsigned long long)s->mismatched, (unsigned long long)s->bytes, (unsigned long long)(us ? s->bytes / us : 0ull),
		       (unsigned long long)s->lockUs, (unsigned long long)s->vLockUs, (unsigned long long)s->vReadUs);
		IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);   // non-null: a chunk went via SDMA under it
		if (l) {                                           // 0.0.509 item 4: the boot's slowest verify per dword
			IOLockLock(l);
			(void)n48_fc_vmax_note(&gFcS, s->verifyUs, s->vLockUs, s->vReadUs, s->compared, s->bytes);
			IOLockUnlock(l);
		}
	} else if (s->mode && gFcLogN < 64u) {
		gFcLogN++;
		N48LOG("residency-copy: #%llu stayed on the MM window with switch 63 ON - %u chunk(s), SDMA declined: %s", (unsigned long long)copyNo,
		       s->chunksMm, n48_fc_reason_name(s->lastWhy));
	}
}

// build 0.0.512 Part B2 (the clock88 watch, above navi48_vram_write_mm): here, below fc_slot_mine.
// A residency copy's destination [dVa, dVa + bytes) (GPU VA, from the copier's own map_gpu_va): one line when it overlaps a watch.
// The copy's SDMA / MM-window chunk counts come from THIS thread's fast-copy slot (still open: the copier calls this before its
// scope closes); with switch 63 off there is no slot and both read 0 (the whole copy went through the MM window).
void navi48_c88_copy_note(uint64_t dVa, uint32_t dVaOk, uint64_t bytes, uint64_t vram, uint64_t copyNo) {
	if (!dVaOk || !__atomic_load_n(&gC88W.n, __ATOMIC_ACQUIRE)) return;
	const int w = n48_c88_hit_va(&gC88W, dVa, bytes);
	if (w < 0) return;
	const uint32_t ln = n48_c88_take_line(&gC88W);
	if (!ln) return;
	const FcSlot *s = fc_slot_mine();
	uint64_t ws = 0ull, um = 0ull; uint32_t wu = 0u; c88_now(&ws, &wu, &um);
	char nm[32] = { 0 }; const int pid = proc_selfpid(); proc_name(pid, nm, (int)sizeof nm);
	N48LOG(N48_C88_COPY_FMT, (uint32_t)w, (unsigned long long)gC88W.va[w], (unsigned long long)gC88W.len[w], (unsigned long long)copyNo,
	       (unsigned long long)dVa, (unsigned long long)bytes, (unsigned long long)vram, s ? s->chunksSdma : 0u, s ? s->chunksMm : 0u,
	       (unsigned long long)ws, wu, (unsigned long long)um, pid, nm, ln, N48_C88_WATCH_LINES);
}

// build 0.0.509 F-3: the copy scope's close (Navi48AccelPeer.cpp navi48_cg_close_scope), on every path - COPIED or FAILED.
// Answers 1 when THIS thread's copy over [lo, hi) ran a chunk through SDMA under sampled verify (n48_fc_copy_sampled), and
// releases its slot. Before 63 has ever been ON this boot it answers 0 at once, touching nothing.
uint32_t navi48_fc_scope_closed(uint64_t lo, uint64_t hi) {
	if (!__atomic_load_n(&gFcEverOn, __ATOMIC_ACQUIRE)) return 0u;
	FcSlot *s = fc_slot_mine();
	if (!s || s->at0 < lo || s->at0 >= hi) return 0u;   // not this scope's copy (no chunk ran inside it)
	const uint32_t sampled = n48_fc_copy_sampled(s->mode, s->chunksSdma);
	__atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);
	return sampled;
}

// The bare `gfxneuter 63` report (and after every change).
void navi48_fc_report(const char *why) {
	IOLock *l = __atomic_load_n(&gFastCopyLock, __ATOMIC_ACQUIRE);
	if (l) IOLockLock(l);
	const uint32_t m = gFcMode;
	const uint64_t tUs = gFcS.stage_us + gFcS.dma_us + gFcS.lock_wait_us;   // 0.0.509: dma counts from the kick; the lock wait is added back
	N48LOG("fastcopy63: switch 63 is %s%s; path %s%s (latch %u), control %s (%u wrong, %llu us), staging %s GART MC %#llx (bump %#llx "
	       "-> %#llx); SDMA copies %llu (%llu chunk(s), %llu bytes), stage %llu us, dma %llu us, verify %llu us, transport %llu MB/s",
	       m == N48_FC_M_FULL ? "ON, full MM verify (831)" : m == N48_FC_M_SAMPLED ? "ON, sampled verify (319)" : "OFF (575, default)",
	       why, gFc.latched_off ? "LATCHED OFF" : "usable", gFc.staging_retired ? ", staging RETIRED" : "", gFc.latch_why,
	       gFc.pc_passed ? "PASSED" : (gFcPcDone ? "FAILED" : "not run"), gFcPcWrong, (unsigned long long)gFcPcUs,
	       gFc.staging_ok ? "bound at" : "none,", (unsigned long long)gFcStagingMc, (unsigned long long)gFcBumpBefore,
	       (unsigned long long)gFcBumpAfter, (unsigned long long)gFcS.copies_sdma, (unsigned long long)gFcS.chunks_sdma,
	       (unsigned long long)gFcS.bytes_sdma, (unsigned long long)gFcS.stage_us, (unsigned long long)gFcS.dma_us,
	       (unsigned long long)gFcS.verify_us, (unsigned long long)(tUs ? gFcS.bytes_sdma / tUs : 0ull));
	N48LOG("fastcopy63: fence timeouts %u, verify mismatches %llu (sampled %llu, full %llu dwords), fill failures %llu, refused %llu, "
	       "ring failures %llu, verify-read failures %llu, last fence %#010x; fallbacks to MM: off %llu latched %llu no-control %llu "
	       "no-staging %llu no-sdma %llu dcc %llu shape %llu no-mem %llu no-slot %llu",
	       gFc.timeouts, (unsigned long long)gFcS.mismatched, (unsigned long long)gFcS.sampled, (unsigned long long)gFcS.full_dwords,
	       (unsigned long long)gFcS.fill_fail, (unsigned long long)gFcS.refused, (unsigned long long)gFcS.ring_fail,
	       (unsigned long long)gFcS.verify_read_fail, gFc.fence_last,
	       (unsigned long long)gFcS.fallback[N48_FC_MM_OFF], (unsigned long long)gFcS.fallback[N48_FC_MM_LATCHED],
	       (unsigned long long)gFcS.fallback[N48_FC_MM_NO_PC], (unsigned long long)gFcS.fallback[N48_FC_MM_NO_STAGING],
	       (unsigned long long)gFcS.fallback[N48_FC_MM_NO_SDMA], (unsigned long long)gFcS.fallback[N48_FC_MM_DCC],
	       (unsigned long long)gFcS.fallback[N48_FC_MM_SHAPE], (unsigned long long)gFcS.fallback[N48_FC_MM_NO_MEM],
	       (unsigned long long)gFcS.fallback[N48_FC_MM_NO_SLOT]);
	// build 0.0.509 items 1-4 (a third line: the two above are near the logger's 512 bytes).
	N48LOG("fastcopy63: fence bound from the kick; scanout-lock wait max %llu us, total %llu us over %llu submission(s); verify "
	       "MM-lock (gVramMmLock) wait %llu us, reads %llu us; slowest verify %llu ns/dword (a %llu-byte copy, %llu dword(s), %llu us: "
	       "lock %llu us, reads %llu us); forced full (62-overlapping) %llu; dead ranges %llu",
	       (unsigned long long)gFcS.lock_wait_max_us, (unsigned long long)gFcS.lock_wait_us, (unsigned long long)gFcS.lock_waits,
	       (unsigned long long)gFcS.vlock_us, (unsigned long long)gFcS.vread_us, (unsigned long long)gFcS.vmax_ns_dw,
	       (unsigned long long)gFcS.vmax_bytes, (unsigned long long)gFcS.vmax_dwords, (unsigned long long)gFcS.vmax_us,
	       (unsigned long long)gFcS.vmax_lock_us, (unsigned long long)gFcS.vmax_read_us,
	       (unsigned long long)gFcS.forced_full, (unsigned long long)gFcS.dead_ranges);
	// build 0.0.510 A1 (a fourth line: the third is near the logger's cap).
	N48LOG("fastcopy63: forced full for copy-guard poison %llu (a sampled retry of a poisoned range verifies every dword, so it can "
	       "clear the poison); first chunks refused to the MM window %llu (nothing written: the copy does not fail)",
	       (unsigned long long)gFcS.forced_full_cg, (unsigned long long)gFcS.refused_mm);
	// build 0.0.521 Part D: resprov candidates latched FULL (so resprov can record them), every copy's mode.
	N48LOG("fastcopy63: forced full for resprov candidates (re-tiled / backing-sourced / known-asset images) %llu; copies latched "
	       "sampled %llu, full %llu",
	       (unsigned long long)gFcS.forced_full_rp, (unsigned long long)gFcS.latched_sampled, (unsigned long long)gFcS.latched_full);
	// build 0.0.514 A3: who held gVramMmLock (every taker, not only the fast copy), two lines
	N48LOG(N48_MMHOLD_FMT1, N48_MMHOLD_ARGS1(&gMmHold));
	N48LOG(N48_MMHOLD_FMT2, N48_MMHOLD_ARGS2(&gMmHold, __atomic_load_n(&gMmTk.overflow, __ATOMIC_RELAXED)));
	N48LOG(N48_MMHOLD_FMT3, N48_MMHOLD_ARGS3(&gMmHold));   // build 0.0.515 D2: DECIDE by site
	if (l) IOLockUnlock(l);
}

// build 0.0.531 item 4 ( item 4; log-only): the mmhold lines, once, at a continuous arm's START and at its STOP (the
// counters are cumulative since boot: the STOP minus the START is the arm's MM-window cost by taker). Takes no lock of its own.
void navi48_mmhold_snapshot(const char *where) {
	N48LOG("mmhold531: snapshot at the continuous arm's %s - the mmhold514/mmhold515 lines below are cumulative since boot; "
	       "STOP minus START is this arm's gVramMmLock holding by taker", where);
	N48LOG(N48_MMHOLD_FMT1, N48_MMHOLD_ARGS1(&gMmHold));
	N48LOG(N48_MMHOLD_FMT2, N48_MMHOLD_ARGS2(&gMmHold, __atomic_load_n(&gMmTk.overflow, __ATOMIC_RELAXED)));
	N48LOG(N48_MMHOLD_FMT3, N48_MMHOLD_ARGS3(&gMmHold));
}

// 0.0.345 — THE DETILE. ONE SDMA COPY_TILED_SUB_WINDOW packet (14 dwords, dcc 0, CPV 0) plus one FENCE
// (4 dwords) for the WHOLE frame, where the linear path emitted 1080 packets in six chunked submissions. detile = 1:
// the tiled surface is the SOURCE and the linear scanout is the DESTINATION.
//
// Everything in this function is a field copy out of the plan; the plan did the checking (n48_scanout_plan_tiled, and
// n48_scanout_tiled_dst_ok over EVERY byte the packet may write). tiledMc/linearMc are MC addresses the caller has
// already range-checked. CPV IS NEVER SET: found COPY_LINEAR grows a trailing dword only when CPV is set,
// and a 15-dword packet in a 14-dword slot desynchronises the ring for ever.
// 0.0.416 (an internal design note, G3): `gcr` prepends the five-dword GCR_REQ to the SAME submission as the
// copy — one ring write, one doorbell, one fence — so the cache rinse is ordered before the copy the engine then
// reads the tiled source with. Default false: every pre-0.0.416 caller is byte-identical (18 dwords, no GCR).
static uint32_t scanout_sdma_tiled_copy(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                        const N48ScanoutTiledPlan *p, uint64_t tiledMc, uint64_t linearMc,
                                        bool detile, uint32_t fenceValue, uint32_t timeoutMs,
                                        uint64_t *usOut, uint32_t *lastFenceOut, bool gcr = false) {
	*usOut = 0; *lastFenceOut = 0;
	const uint32_t dwords = n48_sdma_tiled_copy_dwords(gcr);           // 18, or 23 with the GCR_REQ
	if (!p || dwords > inst.ring_size_dwords / 2) return kScanStRing;
	// The tiled surface base must be 64 KiB aligned in MC space too, not only as a VRAM offset: the block index
	// arithmetic the engine does is relative to this address, and a misaligned base would shift every block.
	if (tiledMc & 0xffffull) return kScanStRing;
	uint32_t pkt[N48_SDMA_TILED_SUB_WINDOW_DWORDS + N48_SDMA_FENCE_DWORDS + N48_SDMA_GCR_REQ_DWORDS];
	uint32_t k = 0;
	if (gcr) k += n48_sdma_gcr_req(pkt + k);                           // the ONE emitter (G1)
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |
	           amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_TILED_SUB_WINDOW) |
	           amdgpu::SDMA_PKT_TILED_SW_TMZ(0) | amdgpu::SDMA_PKT_TILED_SW_DCC(0) |
	           amdgpu::SDMA_PKT_TILED_SW_DETILE(detile ? 1u : 0u);
	pkt[k++] = static_cast<uint32_t>(tiledMc);
	pkt[k++] = static_cast<uint32_t>(tiledMc >> 32);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_XY(p->srcX, p->srcY);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_Z_W(0, p->surfW - 1u);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_HD(p->surfH - 1u, 0);            // depth 1 -> depth-1 = 0
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_INFO(p->elementLog2, p->swizzle, 0, 0);
	pkt[k++] = static_cast<uint32_t>(linearMc);
	pkt[k++] = static_cast<uint32_t>(linearMc >> 32);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_XY(p->dstX, p->dstY);
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_Z_PITCH(0, p->linPitch - 1u);
	pkt[k++] = p->linSlice - 1u;
	pkt[k++] = amdgpu::SDMA_PKT_TILED_SW_RECT(p->w - 1u, p->h - 1u);
	pkt[k++] = 0;                                                          // rect depth - 1
	const uint64_t fenceMc = inst.wb_bus + amdgpu::kSDMAWBCSFenceOffset;
	amdgpu::sdma_wb_write32(dev, inst, amdgpu::kSDMAWBCSFenceOffset, 0);
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
	pkt[k++] = static_cast<uint32_t>(fenceMc);
	pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);
	pkt[k++] = fenceValue;
	if (k != dwords) return kScanStRing;
	amdgpu::amdgpu_hdp_flush(dev);
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return kScanStRing;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return kScanStRing;
	uint64_t us = 0;
	uint32_t v = 0;
	for (;;) {
		v = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (v == fenceValue) break;
		if (us >= (uint64_t)timeoutMs * 1000u) break;
		if (us < 2000) { IODelay(50); us += 50; } else { IOSleep(1); us += 1000; }
	}
	*usOut = us; *lastFenceOut = v;
	return v == fenceValue ? kScanStOk : kScanStFence;
}

// 0.0.416 (an internal design note, G2) — ONE COPY_LINEAR window plus, if asked, the GCR_REQ immediately before
// it in the SAME submission (one ring write, one doorbell, one fence). The COPY_LINEAR shape is scanout_sdma_copies'
// (header with CPV 1, bytes-1, src, dst), so mode 7's copy is the same packet the scanout path already proved.
static uint32_t scanout_sdma_copy_linear_gcr(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                             uint64_t srcMc, uint64_t dstMc, uint32_t bytes, bool gcr,
                                             uint32_t fenceValue, uint32_t timeoutMs,
                                             uint64_t *usOut, uint32_t *lastFenceOut) {
	*usOut = 0; *lastFenceOut = 0;
	if (bytes == 0 || bytes > amdgpu::kSDMACopyLinearMaxBytes) return kScanStPlan;
	const uint32_t dwords = n48_sdma_linear_copy_dwords(gcr);
	if (dwords > inst.ring_size_dwords / 2) return kScanStRing;
	uint32_t pkt[N48_SDMA_COPY_LINEAR_DWORDS + N48_SDMA_FENCE_DWORDS + N48_SDMA_GCR_REQ_DWORDS];
	uint32_t k = 0;
	if (gcr) k += n48_sdma_gcr_req(pkt + k);                           // the ONE emitter (G1)
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |
	           amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_LINEAR) | amdgpu::SDMA_PKT_HEADER_CPV(1);
	pkt[k++] = bytes - 1;                                              // byte_count - 1
	pkt[k++] = 0;
	pkt[k++] = static_cast<uint32_t>(srcMc);
	pkt[k++] = static_cast<uint32_t>(srcMc >> 32);
	pkt[k++] = static_cast<uint32_t>(dstMc);
	pkt[k++] = static_cast<uint32_t>(dstMc >> 32);
	pkt[k++] = 0;
	const uint64_t fenceMc = inst.wb_bus + amdgpu::kSDMAWBCSFenceOffset;
	amdgpu::sdma_wb_write32(dev, inst, amdgpu::kSDMAWBCSFenceOffset, 0);
	pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
	pkt[k++] = static_cast<uint32_t>(fenceMc);
	pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);
	pkt[k++] = fenceValue;
	if (k != dwords) return kScanStRing;
	amdgpu::amdgpu_hdp_flush(dev);
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return kScanStRing;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return kScanStRing;
	uint64_t us = 0;
	uint32_t v = 0;
	for (;;) {
		v = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (v == fenceValue) break;
		if (us >= (uint64_t)timeoutMs * 1000u) break;
		if (us < 2000) { IODelay(50); us += 50; } else { IOSleep(1); us += 1000; }
	}
	*usOut = us; *lastFenceOut = v;
	return v == fenceValue ? kScanStOk : kScanStFence;
}

// 0.0.278: `n` row packets in submissions of at most kScanChunkRows, each fenced (fenceBase + chunk index) and
// waited for before the next. usOut sums the waits; the first failing chunk's status is returned.
static uint32_t scanout_sdma_copies_chunked(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                            const uint64_t *srcMc, const uint64_t *dstMc, const uint32_t *bytes,
                                            uint32_t n, uint32_t fenceBase, uint32_t timeoutMs,
                                            uint64_t *usOut, uint32_t *lastFenceOut, uint32_t *chunksOut) {
	*usOut = 0; *lastFenceOut = 0; *chunksOut = 0;
	uint32_t st = kScanStOk;
	for (uint32_t at = 0, c = 0; st == kScanStOk && at < n; at += kScanChunkRows, c++) {
		const uint32_t k = n - at < kScanChunkRows ? n - at : kScanChunkRows;
		uint32_t rptr = 0;
		if (c > 0) st = scanout_queue_check(dev, inst, &rptr);
		uint64_t us = 0;
		if (st == kScanStOk)
			st = scanout_sdma_copies(dev, inst, &srcMc[at], &dstMc[at], &bytes[at], k, fenceBase + c, timeoutMs, &us, lastFenceOut);
		*usOut += us;
		*chunksOut = c + 1;
	}
	return st;
}

static void scanout_free_vram(amdgpu::VRAMAllocation &a) {
	if (a.size) gBringup.gmc.vram_alloc.free(a);
	a.size = 0;
}

// =====================================================================================================
// 0.0.414 (an internal design note,) — THE FULL-GEOMETRY SDMA SELF-TEST.
//
// Mode 5 proves the 14-dword COPY_TILED_SUB_WINDOW packet on a 256x256 surface.'s live
// plane is 1920x1080, ADDR3 64KB_2D, pitch 1920, 15 blocks across and a partial 9th block row, and the
// one-packet copy of it reads black at most sampled points on the glass. This function measures the SAME
// packet, built by the SAME scanout_sdma_tiled_copy, on the LIVE geometry with our own buffers.
//
// S3/S4's method, parameterised by (w, h): the CPU writes a LINEAR probe in ASCENDING address order (the
// order proved SDMA reads correctly), SDMA TILES it (detile 0) into a tiled scratch, then SDMA DETILES
// that SDMA-written scratch (detile 1) into a poisoned linear destination. proved the SDMA-written tiled
// source detiles correctly on 256x256; this asks the same question on the live geometry.
//
// S5's bounded sample set (n48_tile_ss_*) is read through the MM window — the GPU's own view — for the TILE
// and DETILE verdicts, and the destination is read through BAR0 at the same set for a second opinion. S2's
// 16x16 cell map (display_pipe_guard.h) records where the detile is wrong. Buffers are our own pool's and are
// freed on every exit; nothing outside those three buffers is touched (S2/S8).
struct N48FullCase {
	uint32_t w, h;
	uint32_t tileWrong, tileSampled;
	uint32_t detWrong, detSampled, poison, barMm, barSampled;
	uint64_t map[4];
	uint32_t nfirst;
	uint32_t fdx[8], fdy[8], fgot[8], fpx[8], fpy[8];
	uint64_t probeOff, tiledOff, destOff, probeMc, tiledMc, destMc, linBytes, tilBytes;
	uint64_t tileUs, detUs;
	uint32_t tileStatus, detStatus, tileFence, detFence;
	uint32_t st;
};

// S6's decoded batch: up to 8 wrong destination pixels as (dx,dy) got (sx,sy)|POISON, with the tiled address
// our equation gives for (dx,dy) and for (sx,sy). Kept well under the logger's 512-byte line.
static void scanout_full_decode(const N48FullCase *r, char *out, unsigned cap) {
	unsigned o = 0;
	if (!cap) return;
	out[0] = 0;
	for (uint32_t i = 0; i < r->nfirst && o + 1u < cap; i++) {
		char one[96];
		int l;
		const unsigned long long aD = (unsigned long long)n48_addr3_64kb_2d_off_4bpe(r->fdx[i], r->fdy[i], r->w);
		if (r->fpx[i] == 0xffffffffu) {
			l = snprintf(one, sizeof(one), "(%u,%u)%s@%#llx; ", r->fdx[i], r->fdy[i], "POISON", aD);
		} else {
			const unsigned long long aS = (unsigned long long)n48_addr3_64kb_2d_off_4bpe(r->fpx[i], r->fpy[i], r->w);
			l = snprintf(one, sizeof(one), "(%u,%u)->(%u,%u)@%#llx/%#llx; ",
			             r->fdx[i], r->fdy[i], r->fpx[i], r->fpy[i], aD, aS);
		}
		if (l < 0 || o + (unsigned)l >= cap) break;
		memcpy(out + o, one, (size_t)l);
		o += (unsigned)l;
		out[o] = 0;
	}
}

// D7 (0.0.417, an internal design note): `uniform` selects the probe pattern. Mode 6 passes false and is
// byte-identical; mode 8 passes true for its 1920x1080 case only, so the CPU writes N48_TILE_UNIFORM_PIXEL at
// every position and every expected value is that same value. Everything else - buffers, packet fields, sample
// set, report lines, POISON detection - is the same code the two modes share.
static inline uint32_t scanout_full_probe(uint32_t x, uint32_t y, bool uniform) {
	return uniform ? n48_tile_uniform_pixel(x, y) : n48_tile_probe_pixel(x, y);
}

static uint32_t scanout_full_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                  uint32_t w, uint32_t h, uint32_t fenceBase, N48FullCase *r, bool uniform) {
	amdgpu::VRAMAllocation pa {}, ta {}, da {};
	uint32_t st = kScanStOk;
	bzero(r, sizeof(*r));
	r->w = w; r->h = h;
	r->linBytes = (uint64_t)w * h * 4u;
	r->tilBytes = n48_addr3_64kb_2d_bytes_4bpe(w, h);
	{
		uint32_t rp = 0;
		st = scanout_queue_check(dev, inst, &rp);
		if (st != kScanStOk) { r->st = st; return st; }
	}
	if (!gBringup.gmc.vram_alloc.alloc(r->linBytes, 65536, &pa) ||
	    !gBringup.gmc.vram_alloc.alloc(r->tilBytes, 65536, &ta) ||
	    !gBringup.gmc.vram_alloc.alloc(r->linBytes, 65536, &da)) {
		// S2: refuse with a named status and log the pool's free size; do NOT grow the pool in this build.
		N48LOG("scanout-full: %ux%u needs %llu (probe) + %llu (tiled) + %llu (dest) = %llu byte(s) from "
		       "gmc.vram_alloc; pool free now %llu byte(s) - REFUSING (VRAM allocation failed)",
		       w, h, (unsigned long long)r->linBytes, (unsigned long long)r->tilBytes,
		       (unsigned long long)r->linBytes,
		       (unsigned long long)(2ull * r->linBytes + r->tilBytes),
		       (unsigned long long)gBringup.gmc.vram_alloc.bytes_free());
		scanout_free_vram(pa); scanout_free_vram(ta); scanout_free_vram(da);
		r->st = kScanStAlloc;
		return kScanStAlloc;
	}
	r->probeOff = pa.gpu_va - gBringup.gmc.vram_start;
	r->tiledOff = ta.gpu_va - gBringup.gmc.vram_start;
	r->destOff  = da.gpu_va - gBringup.gmc.vram_start;
	r->probeMc = pa.gpu_va; r->tiledMc = ta.gpu_va; r->destMc = da.gpu_va;
	if (r->probeOff + r->linBytes > dev.bar0Size || r->destOff + r->linBytes > dev.bar0Size ||
	    r->tiledOff + r->tilBytes > dev.bar0Size || (r->tiledOff & 0xffffu) || (r->probeOff & 0xffffu) ||
	    (r->destOff & 0xffffu)) { st = kScanStAlloc; goto done; }
	// S3: the CPU writes the LINEAR probe in ASCENDING address order; pattern n48_tile_probe_pixel.
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			amdgpu::WBAR0_32(dev, r->probeOff + ((uint64_t)y * w + x) * 4u, scanout_full_probe(x, y, uniform));
	amdgpu::amdgpu_hdp_flush(dev);
	// The plan is mode 5's, with the LIVE field values: swizzle 3, element size 2, pitch = width.
	{
		N48ScanoutTiledPlan p {};
		uint64_t us = 0; uint32_t last = 0;
		p.fbOff = r->destOff; p.srcOff = r->tiledOff; p.surfW = w; p.surfH = h;
		p.swizzle = amdgpu::kAddr3_64KB_2D; p.elementLog2 = 2u;
		p.srcX = 0; p.srcY = 0; p.dstX = 0; p.dstY = 0;
		p.linPitch = w; p.linSlice = w * h; p.w = w; p.h = h;
		// S3: SDMA TILES the verified linear probe into the tiled scratch (detile 0).
		st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + r->tiledOff,
		                             gBringup.gmc.vram_start + r->probeOff, false, fenceBase, 1000, &us, &last);
		r->tileUs = us; r->tileFence = last; r->tileStatus = st;
		if (st != kScanStOk) goto done;
		// TILE verdict (S3): the tiled scratch through the GPU's own MM window, against the equation.
		{
			const uint64_t n = n48_tile_ss_count(w, h);
			uint32_t bad = 0;
			for (uint64_t i = 0; i < n; i++) {
				uint32_t x = 0, y = 0, got = 0;
				if (!n48_tile_ss_at(w, h, i, &x, &y)) break;
				const uint64_t o = r->tiledOff + n48_addr3_64kb_2d_off_4bpe(x, y, w);
				if (!navi48_vram_read_mm(o, &got, 1) || got != scanout_full_probe(x, y, uniform)) bad++;
			}
			r->tileWrong = bad; r->tileSampled = (uint32_t)n;
		}
		// S4: poison the whole linear destination, then ONE detile of the whole surface into it, pitch w.
		amdgpu::bar0_memset_vram(dev, r->destOff, N48_TILE_PROBE_POISON, r->linBytes);
		amdgpu::amdgpu_hdp_flush(dev);
		st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + r->tiledOff,
		                             gBringup.gmc.vram_start + r->destOff, true, fenceBase + 1u, 1000, &us, &last);
		r->detUs = us; r->detFence = last; r->detStatus = st;
		if (st != kScanStOk) goto done;
		// DETILE verdict (S4/S5): the bounded sample set through MM, BAR0 at the same set counted separately.
		{
			const uint64_t n = n48_tile_ss_count(w, h);
			uint32_t bad = 0, poison = 0, barMm = 0, barSampled = 0;
			for (uint64_t i = 0; i < n; i++) {
				uint32_t dx = 0, dy = 0, got = 0;
				if (!n48_tile_ss_at(w, h, i, &dx, &dy)) break;
				const uint64_t o = r->destOff + ((uint64_t)dy * w + dx) * 4u;
				const bool mmOk = navi48_vram_read_mm(o, &got, 1);
				barSampled++;
				if (mmOk && amdgpu::RBAR0_32(dev, o) != got) barMm++;
				if (mmOk && got == scanout_full_probe(dx, dy, uniform)) continue;
				if (mmOk && got == N48_TILE_PROBE_POISON) poison++;
				n48_dpg_verify_map_set(r->map, n48_dpg_cell16(dx, dy, w, h));
				if (r->nfirst < 8u) {
					uint32_t sx = 0, sy = 0;
					r->fdx[r->nfirst] = dx; r->fdy[r->nfirst] = dy; r->fgot[r->nfirst] = got;
					if (n48_tile_decode(got, &sx, &sy)) { r->fpx[r->nfirst] = sx; r->fpy[r->nfirst] = sy; }
					else { r->fpx[r->nfirst] = 0xffffffffu; r->fpy[r->nfirst] = 0xffffffffu; }
					r->nfirst++;
				}
				bad++;
			}
			r->detWrong = bad; r->detSampled = (uint32_t)n; r->poison = poison;
			r->barMm = barMm; r->barSampled = barSampled;
		}
		// S6: one summary line, one map line, one decoded batch, each under 512 bytes.
		N48LOG("scanout-full: %ux%u swizzle 3 (ADDR3_64KB_2D, 32 bpp, pitch %u) probe %#llx (MC %#llx) tiled %#llx "
		       "(MC %#llx, %llu B) dest %#llx (MC %#llx); TILE status %u fence %#010x %llu us; DETILE status %u "
		       "fence %#010x %llu us", w, h, w, (unsigned long long)r->probeOff, (unsigned long long)r->probeMc,
		       (unsigned long long)r->tiledOff, (unsigned long long)r->tiledMc, (unsigned long long)r->tilBytes,
		       (unsigned long long)r->destOff, (unsigned long long)r->destMc, r->tileStatus, r->tileFence,
		       (unsigned long long)r->tileUs, r->detStatus, r->detFence, (unsigned long long)r->detUs);
		N48LOG("scanout-full: %ux%u TILE wrong %u of %u; DETILE wrong %u of %u; POISON (never written) %u of %u; "
		       "BAR0-vs-MM disagreements %u of %u%s", w, h, r->tileWrong, r->tileSampled, r->detWrong, r->detSampled,
		       r->poison, r->detSampled, r->barMm, r->barSampled,
		       (r->tileWrong || r->detWrong) ? " - MEASURED (not a refusal)" : " - CLEAN");
		N48LOG("scanout-full: %ux%u detile wrong 16x16 map %016llx%016llx%016llx%016llx", w, h,
		       (unsigned long long)r->map[0], (unsigned long long)r->map[1],
		       (unsigned long long)r->map[2], (unsigned long long)r->map[3]);
		if (r->nfirst) {
			char dec[400];
			scanout_full_decode(r, dec, sizeof(dec));
			N48LOG("scanout-full: %ux%u DECODE %u: %s", w, h, r->nfirst, dec);
		}
	}
done:
	scanout_free_vram(pa); scanout_free_vram(ta); scanout_free_vram(da);
	r->st = st;
	return st;
}

// =====================================================================================================
// 0.0.416 (an internal design note, G2;) — MODE 7: THE SDMA CACHE-RINSE INSTRUMENT.
//
// READ-ONLY on the source. ONE SDMA COPY_LINEAR moves a 256 KiB window from VRAM offset `vramOff` into a low
// scratch buffer of ours; if `gcr` is asked for, the SDMA GCR_REQ (GL2 write-back + invalidate) goes
// immediately BEFORE the copy in the SAME submission, so it is ordered before the engine reads the source.
// Fenced as every copy here is. Then the scratch (what the engine's copy delivered) and the source (what the
// GPU's own MM window sees) are BOTH read through the MM window and compared per 256-byte line: lines fully
// matching / partly / fully differing, a 64-bit map of which of the 64 4-KiB pages hold a difference, and the
// first 8 differing dwords (offset, MM source, SDMA scratch). `vramOff == 0` is the CONTROL: a low buffer of
// our own is written with a known pattern through the MM window and the SAME comparison runs on it.
//
// The question it asks: does the engine read the same bytes at these addresses as the GPU's own view? If a
// plane window differs plain but matches with the GCR_REQ, cache coherence is the mechanism ('s (A)).
// Every log line is under 512 bytes; every buffer is our own and freed on every exit path.
// =====================================================================================================
#define N48_GCR_WINDOW_BYTES 0x40000u          // 256 KiB
#define N48_GCR_LINE_DWORDS  64u               // one 256-byte line
#define N48_GCR_LINES        (N48_GCR_WINDOW_BYTES / (N48_GCR_LINE_DWORDS * 4u))   // 1024
#define N48_GCR_PAGE_LINES   16u               // 4 KiB / 256 B

static uint32_t scanout_gcr_case(amdgpu::DeviceContext &dev, amdgpu::SDMAInstance &inst,
                                 uint64_t arg, uint64_t *v) {
	amdgpu::VRAMAllocation sc {}, pat {};
	uint32_t st = kScanStOk;
	const bool control = (n48_scanout7_off(arg) == 0);
	const bool gcr = n48_scanout7_gcr(arg) != 0;
	uint64_t srcOff = n48_scanout7_off(arg);
	uint32_t rp = 0;
	st = scanout_queue_check(dev, inst, &rp);
	if (st != kScanStOk) { v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff; return st; }
	if (!control) {
		const uint32_t rr = n48_gcr_src_reason(srcOff, N48_GCR_WINDOW_BYTES, dev.vramSizeBytes,
		                                       gBringup.gmc.vram_alloc.base(), gBringup.gmc.vram_alloc.size(),
		                                       gBringup.gmc.vram_alloc_hi.base(), gBringup.gmc.vram_alloc_hi.size());
		if (rr != kN48GcrSrcOk) {
			N48LOG("scanout-gcr: source window %#llx + %u byte(s) REFUSED (%s; allocation pools %#llx+%llu and "
			       "%#llx+%llu, VRAM %llu byte(s))", (unsigned long long)srcOff, (unsigned)N48_GCR_WINDOW_BYTES,
			       rr == kN48GcrSrcAlign ? "not 4 KiB aligned" : rr == kN48GcrSrcRange ? "outside the card's VRAM"
			       : "overlaps one of our own allocations",
			       (unsigned long long)gBringup.gmc.vram_alloc.base(), (unsigned long long)gBringup.gmc.vram_alloc.size(),
			       (unsigned long long)gBringup.gmc.vram_alloc_hi.base(), (unsigned long long)gBringup.gmc.vram_alloc_hi.size(),
			       (unsigned long long)dev.vramSizeBytes);
			v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
			return rr == kN48GcrSrcAlign ? kScanStGcrAlign : rr == kN48GcrSrcRange ? kScanStGcrRange : kScanStGcrOverlap;
		}
	}
	if (!gBringup.gmc.vram_alloc.alloc(N48_GCR_WINDOW_BYTES, 65536, &sc) ||
	    (control && !gBringup.gmc.vram_alloc.alloc(N48_GCR_WINDOW_BYTES, 65536, &pat))) {
		N48LOG("scanout-gcr: cannot allocate %u byte(s) scratch (+ control) from gmc.vram_alloc; pool free now %llu "
		       "byte(s) - REFUSING", (unsigned)N48_GCR_WINDOW_BYTES, (unsigned long long)gBringup.gmc.vram_alloc.bytes_free());
		scanout_free_vram(sc); scanout_free_vram(pat);
		v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
		return kScanStAlloc;
	}
	const uint64_t scOff = sc.gpu_va - gBringup.gmc.vram_start;
	if (scOff + N48_GCR_WINDOW_BYTES > dev.bar0Size ||
	    (control && (pat.gpu_va - gBringup.gmc.vram_start) + N48_GCR_WINDOW_BYTES > dev.bar0Size)) {
		scanout_free_vram(sc); scanout_free_vram(pat);
		v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
		return kScanStAlloc;
	}
	if (control) {
		// The control source is OUR low buffer, written through the MM window (the GPU's own view) so the same
		// comparison runs on memory we own and know. The pattern is self-naming: index | 0x5a5a0000.
		srcOff = pat.gpu_va - gBringup.gmc.vram_start;
		for (uint64_t i = 0; i < N48_GCR_WINDOW_BYTES / 4u; i += 64u) {
			uint32_t p[64];
			for (uint32_t x = 0; x < 64u; x++) p[x] = 0x5a5a0000u | (uint32_t)((i + x) & 0xffffu);
			if (!navi48_vram_write_mm(srcOff + i * 4u, p, 64)) {
				N48LOG("scanout-gcr: CONTROL pattern write through the MM window failed at +%#llx - REFUSING",
				       (unsigned long long)(i * 4u));
				scanout_free_vram(sc); scanout_free_vram(pat);
				v[1] = (uint64_t)gcr | ((uint64_t)control << 1); v[2] = srcOff;
				return kScanStSourceCtl;
			}
		}
		amdgpu::amdgpu_hdp_flush(dev);
	}
	uint64_t us = 0; uint32_t last = 0;
	st = scanout_sdma_copy_linear_gcr(dev, inst, gBringup.gmc.vram_start + srcOff,
	                                  gBringup.gmc.vram_start + scOff, N48_GCR_WINDOW_BYTES, gcr,
	                                  0x5CA12000u, 1000, &us, &last);
	uint32_t full = 0, part = 0, diff = 0, ndiff = 0, nfirst = 0;
	uint32_t nd[8] = { 0 }, nmm[8] = { 0 }, nsd[8] = { 0 };
	uint64_t pageMap = 0;
	if (st == kScanStOk) {
		for (uint32_t l = 0; l < N48_GCR_LINES; l++) {
			const uint64_t off = (uint64_t)l * (N48_GCR_LINE_DWORDS * 4u);
			uint32_t mm[64] = { 0 }, sd[64] = { 0 };
			const bool mok = navi48_vram_read_mm(srcOff + off, mm, N48_GCR_LINE_DWORDS);
			const bool sok = navi48_vram_read_mm(scOff + off, sd, N48_GCR_LINE_DWORDS);
			uint32_t bad = 0;
			for (uint32_t k = 0; k < N48_GCR_LINE_DWORDS; k++) {
				if (mok && sok && mm[k] == sd[k]) continue;
				bad++; ndiff++;
				pageMap |= 1ull << (l / N48_GCR_PAGE_LINES);
				if (nfirst < 8u) { nd[nfirst] = l * N48_GCR_LINE_DWORDS + k; nmm[nfirst] = mm[k]; nsd[nfirst] = sd[k]; nfirst++; }
			}
			if (bad == 0) full++;
			else if (bad == N48_GCR_LINE_DWORDS) diff++;
			else part++;
		}
	}
	// out: [1] gcr | control<<1, [2] source VRAM offset, [3] scratch VRAM offset, [4] fence us | copy dwords<<32,
	// [5] lines full | part<<16 | fully-differing<<32, [6] differing dwords, [7] the 64-page difference map.
	v[1] = (uint64_t)gcr | ((uint64_t)control << 1);
	v[2] = srcOff; v[3] = scOff;
	v[4] = us | ((uint64_t)n48_sdma_linear_copy_dwords(gcr) << 32);
	v[5] = (uint64_t)full | ((uint64_t)part << 16) | ((uint64_t)diff << 32);
	v[6] = ndiff; v[7] = pageMap;
	N48LOG("scanout-gcr: mode 7 %s (%u-byte window), GCR_REQ %s; source %#llx (MC %#llx) -> scratch %#llx (MC %#llx); "
	       "status %u, fence %#010x after %llu us, COPY_LINEAR+GCR %u dwords; lines full %u, partly %u, fully differing %u "
	       "of %u; differing dwords %u; pages with a difference %#018llx", control ? "CONTROL" : "PLANE",
	       (unsigned)N48_GCR_WINDOW_BYTES, gcr ? "ON" : "off", (unsigned long long)srcOff,
	       (unsigned long long)(gBringup.gmc.vram_start + srcOff), (unsigned long long)scOff,
	       (unsigned long long)(gBringup.gmc.vram_start + scOff), st, last, (unsigned long long)us,
	       (unsigned)n48_sdma_linear_copy_dwords(gcr), full, part, diff, (unsigned)N48_GCR_LINES, ndiff,
	       (unsigned long long)pageMap);
	if (nfirst)
		N48LOG("scanout-gcr: first %u differing dword(s) (offset, MM source, SDMA scratch): %u:%#010x/%#010x "
		       "%u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x %u:%#010x/%#010x "
		       "%u:%#010x/%#010x", nfirst, nd[0], nmm[0], nsd[0], nd[1], nmm[1], nsd[1], nd[2], nmm[2], nsd[2],
		       nd[3], nmm[3], nsd[3], nd[4], nmm[4], nsd[4], nd[5], nmm[5], nsd[5], nd[6], nmm[6], nsd[6],
		       nd[7], nmm[7], nsd[7]);
	scanout_free_vram(sc); scanout_free_vram(pat);
	return st;
}

// Mode 0 reads, 1 runs the positive control, 2 restores the rectangle the positive control overwrote.
// out[0] status, [1] geometry reason | plan reason<<8 | pre-flight BAR0 pass 2<<16 | BAR0 pass 3 (after the HDP
// flush)<<32 | poison (0xdeadbeef) mismatches<<48, [2] scanout VRAM offset, [3] rect x<<48|y<<32|w<<16|h,
// [4] pre-flight BAR0 pass 1 mismatches | MM-WINDOW mismatches<<32 (0.0.337: the MM window is the verdict),
// [5] pre-flight fence us<<32 | scanout fence us, [6] rect BAR0 mismatches | rect MM-window mismatches<<32,
// [7] first mismatch index<<32 | value read (the PRE-FLIGHT's when status is 9, the rectangle's otherwise),
// [8..11] samples expected<<32|read (row 0 x=16, row 0 x=112, row 0 x=240, row 63 x=16),
// [12] passed | runs<<8 | copies<<24 | refusals<<40.
static uint32_t navi48_scanout_full(uint32_t mode, uint64_t *v);   // build 0.0.542: modes 9 and 10 (below, beside flip mode)
uint32_t navi48_scanout_control(uint64_t arg, uint64_t *out, unsigned count) {
	uint64_t v[13] = { 0 };
	// 0.0.416 (an internal design note, G2): mode 7 packs the mode, the GCR flag and the source VRAM offset
	// into the one ABI scalar (n48_scanout7_scalar). Modes 0..6 keep scalar == mode, byte-identical; any other
	// scalar with high bits set is refused exactly as it was before (kScanStPlan).
	const uint32_t mode = (uint32_t)(arg & N48_SCANOUT_MODE_MASK);
	uint32_t st = kScanStOk;
	amdgpu::VRAMAllocation a {}, b {}, c {};
	N48ScanoutGeom g {};
	IOService *fb = nullptr;
	uint64_t fbOff = 0;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { st = kScanStNoContext; goto done_nolock; }
	if (arg != (uint64_t)mode && mode != 7u) { st = kScanStPlan; goto done_nolock; }
	// build 0.0.542: `scanout 9` (full) / `scanout 10` (release) take flip mode's lock BEFORE gScanoutLock (the lock order), so
	// they branch here, before this function's own gScanoutLock. v[1] carries scanout_full.h's reason; 0 captured / released.
	if (mode == N48_SF_MODE || mode == N48_SF_MODE_RELEASE) {
		st = navi48_scanout_full(mode, v) ? kScanStFull : kScanStOk;
		goto done_nolock;
	}
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		st = scanout_geometry(dev, &g, &fb);
		if (st != kScanStOk) goto done;
		{
			const uint32_t gr = n48_scanout_geom_check(&g, &fbOff);
			v[1] = gr; v[2] = fbOff;
			N48LOG("scanout: RDNA4FB %s base %#llx len %#llx %ux%u rowBytes %u depth %u; our BAR0 %#llx size %#llx, "
			       "vramBase %#llx, vram_start(MC) %#llx vramMcBase %#llx -> geometry %s (reason %u), scanout VRAM "
			       "offset %#llx", fb->getName(), (unsigned long long)g.fbPhys, (unsigned long long)g.fbLen, g.width,
			       g.height, g.rowBytes, g.depth, (unsigned long long)g.bar0Phys, (unsigned long long)g.bar0Size,
			       (unsigned long long)g.vramBase, (unsigned long long)gBringup.gmc.vram_start,
			       (unsigned long long)dev.vramMcBase, gr ? "REFUSED" : "OK", gr, (unsigned long long)fbOff);
			if (gr) { st = kScanStGeom; goto done; }
			if (gBringup.gmc.vram_start != dev.vramMcBase || dev.vramMcBase == 0) { st = kScanStMcBase; goto done; }
		}
		if (mode == 0) goto done;
		// build 0.0.518 (W4): the content read / FNV (3) and the thumbnail (4) read the buffer HUBP0 is scanning (A or flip
		// mode's B), not A. Both only read; every write mode below keeps the console's offset.
		if (mode == 3 || mode == 4) {
			const uint64_t rd = fm_displayed_off(fbOff, g.fbLen);
			if (rd != fbOff) N48LOG("scanout: mode %u reads flip mode's B (vram+%#llx), the buffer SURFACE_EARLIEST_INUSE names",
			                        mode, (unsigned long long)rd);
			fbOff = rd;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		if (mode == 2) {
			if (!gScanoutSave) { st = kScanStNoSave; goto done; }
			for (uint32_t y = 0; y < gScanoutSaveH; y++)
				for (uint32_t x = 0; x < gScanoutSaveW; x++) {
					const uint64_t o = fbOff + (uint64_t)(gScanoutSaveY + y) * g.rowBytes + (uint64_t)(gScanoutSaveX + x) * 4u;
					if (!n48_scanout_row_dst_ok(fbOff, g.fbLen, o, 4)) continue;
					amdgpu::WBAR0_32(dev, o, gScanoutSave[y * gScanoutSaveW + x]);
				}
			N48LOG("scanout: RESTORED the %ux%u rectangle at (%u,%u) from the saved copy (CPU writes through BAR0, "
			       "inside the scanout only)", gScanoutSaveW, gScanoutSaveH, gScanoutSaveX, gScanoutSaveY);
			goto done;
		}
		// 0.0.279: mode 3 READS WHAT THE MONITOR SHOWS, writes nothing. CoreDisplay's legacy present copies
		// WindowServer's shadow into the kIOFBVRAMMemory mapping - RDNA4FB's getVRAMRange, this scanout - so the scanout itself
		// is the evidence. out[1] positive-control rectangle pixels still holding the kext bars (of out[2]), [3] FNV-1a over
		// every 4th pixel of every 4th row, [4] non-black sampled pixels, [5] value changes along the sampled rows, [6] samples,
		// [7] centre, [8] (16,16), [9] (w-17,16), [10] (16,h-17), [11] (w-17,h-17), [12] pixels differing from the bars in the rect.
		if (mode == 3) {
			uint32_t bars = 0, diff = 0;
			for (uint32_t y = 0; y < kScanPcH; y++)
				for (uint32_t x = 0; x < kScanPcW; x++) {
					const uint64_t o = fbOff + (uint64_t)(kScanPcY + y) * g.rowBytes + (uint64_t)(kScanPcX + x) * 4u;
					if (amdgpu::RBAR0_32(dev, o) == n48_pc_pixel(x, y, kScanPcW)) bars++; else diff++;
				}
			uint64_t h = 1469598103934665603ull;
			uint32_t nz = 0, changes = 0, samples = 0;
			for (uint32_t y = 0; y < g.height; y += 4) {
				uint32_t prev = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes);
				for (uint32_t x = 0; x < g.width; x += 4) {
					const uint32_t px = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes + (uint64_t)x * 4u);
					h = (h ^ px) * 1099511628211ull;
					if (px & 0x00ffffffu) nz++;
					if (px != prev) { changes++; prev = px; }
					samples++;
				}
			}
			const uint32_t sx[5] = { g.width / 2, 16, g.width - 17, 16, g.width - 17 }, sy[5] = { g.height / 2, 16, 16, g.height - 17, g.height - 17 };
			for (unsigned i = 0; i < 5; i++) v[7 + i] = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)sy[i] * g.rowBytes + (uint64_t)sx[i] * 4u);
			v[1] = bars; v[2] = kScanPcW * kScanPcH; v[3] = h; v[4] = nz; v[5] = changes; v[6] = samples; v[12] = diff;
			N48LOG("scanout: CONTENT READ (writes nothing): control rectangle %ux%u at (%u,%u) still holds the kext bars at %u pixel(s), %u differ; "
			       "sampled %u pixel(s) (every 4th of every 4th row): non-black %u, value changes %u, FNV-1a %#018llx; centre %#010x, "
			       "inset corners TL %#010x TR %#010x BL %#010x BR %#010x", kScanPcW, kScanPcH, kScanPcX, kScanPcY, bars, diff, samples, nz,
			       changes, (unsigned long long)h, (uint32_t)v[7], (uint32_t)v[8], (uint32_t)v[9], (uint32_t)v[10], (uint32_t)v[11]);
			goto done;
		}
		// 0.0.281: mode 4 is a READ-ONLY THUMBNAIL - the scanout averaged into 64x36 cells (30x30 px at 1920x1080,
		// every 3rd pixel of every 3rd row per cell), one log line per cell row as 64 RRGGBB hex triplets, bracketed by BEGIN/END
		// lines; tools/scanout-thumb-png.py turns a log or stream into a PNG. out[1] cols, [2] rows, [3] FNV-1a of the cells,
		// [4] cells with any non-black average, [5] brightest cell luma, [6] mean luma * 1000.
		if (mode == 4) {
			static constexpr uint32_t cols = 64, rows = 36;
			const uint32_t cw = g.width / cols, chh = g.height / rows;
			uint64_t h = 1469598103934665603ull, lumaSum = 0;
			uint32_t lit = 0, maxLuma = 0;
			static uint64_t thumbSeq = 0;
			thumbSeq++;
			N48LOG("scanout-thumb: BEGIN #%llu %ux%u cells of %ux%u px from the %ux%u scanout (writes nothing)", (unsigned long long)thumbSeq,
			       cols, rows, cw, chh, g.width, g.height);
			for (uint32_t r = 0; r < rows; r++) {
				char hex[cols * 6 + 1];
				for (uint32_t c = 0; c < cols; c++) {
					uint64_t sr = 0, sg = 0, sb = 0, n = 0;
					for (uint32_t y = r * chh; y < (r + 1) * chh; y += 3)
						for (uint32_t x = c * cw; x < (c + 1) * cw; x += 3) {
							const uint32_t px = amdgpu::RBAR0_32(dev, fbOff + (uint64_t)y * g.rowBytes + (uint64_t)x * 4u);
							sr += (px >> 16) & 0xffu; sg += (px >> 8) & 0xffu; sb += px & 0xffu; n++;
						}
					const uint32_t ar = n ? (uint32_t)(sr / n) : 0u, ag = n ? (uint32_t)(sg / n) : 0u, ab = n ? (uint32_t)(sb / n) : 0u;
					const uint32_t cell = (ar << 16) | (ag << 8) | ab;
					h = (h ^ cell) * 1099511628211ull;
					const uint32_t luma = (299u * ar + 587u * ag + 114u * ab) / 1000u;
					lumaSum += luma;
					if (cell) lit++;
					if (luma > maxLuma) maxLuma = luma;
					static const char hx[] = "0123456789abcdef";
					for (unsigned k = 0; k < 6; k++) hex[c * 6 + k] = hx[(cell >> (20 - 4 * k)) & 0xfu];
				}
				hex[cols * 6] = 0;
				N48LOG("scanout-thumb: #%llu row %02u %s", (unsigned long long)thumbSeq, r, hex);
			}
			N48LOG("scanout-thumb: END #%llu FNV-1a %#018llx, lit cells %u of %u, brightest luma %u, mean luma %llu.%03llu",
			       (unsigned long long)thumbSeq, (unsigned long long)h, lit, cols * rows, maxLuma,
			       (unsigned long long)(lumaSum / (cols * rows)), (unsigned long long)((lumaSum * 1000u / (cols * rows)) % 1000u));
			v[1] = cols; v[2] = rows; v[3] = h; v[4] = lit; v[5] = maxLuma; v[6] = lumaSum * 1000u / (cols * rows);
			goto done;
		}
		// 0.0.345 — MODE 5: THE UNARMED DETILE PROOF. Touches NOTHING outside three scratch VRAM buffers
		// from our own pool: it never reads or writes the scanout, never needs the positive-control interlock, needs no
		// WindowServer, no Apple accelerator and no display pipe, and costs neither a reboot nor a panic.
		// proved the fence fix this way before any armed run; this is the same discipline for the packet.
		//
		// Four measurements, in order, on a 256 x 256 (2 x 2 blocks of 128 x 128) surface at 32 bpp:
		//   1. THE VERDICT. The CPU lays down a KNOWN TILED image using n48_addr3_64kb_2d_off_4bpe (the address
		//      equation read out of vendored addrlib, scanout_copy.h). SDMA detiles it with the new 14-dword packet
		//      into a linear buffer. Every one of the 65536 destination pixels is compared, MM window (the GPU's own
		//      view,'s verdict) and BAR0. The pattern is n48_tile_probe_pixel = (y<<16)|x, so a wrong dword
		//      NAMES the source pixel it actually came from, and the first six are logged decoded.
		//   2. THE CROSS-CHECK, and it is the one that makes 1 mean anything about the HARDWARE. SDMA tiles the
		//      verified linear result back (detile = 0) into a third buffer, and every position is compared against
		//      the same equation. Agreement = the hardware's ADDR3_64KB_2D layout IS our equation, measured, not
		//      assumed. 1 alone could have passed with a wrong-but-self-consistent equation only if the hardware
		//      shared the same wrong equation; 2 is what closes that.
		//   3. THE NEGATIVE CONTROL. `differs from linear` counts the positions where the tiled address is NOT the
		//      linear one. If SDMA had ignored the swizzle field and done a straight copy, 1 would still pass and 2
		//      would fail at exactly these positions - so a non-zero count here with 2 passing is what rules out
		//      "the swizzle field did nothing".
		//   4. THE ASYMMETRIC FIELD CHECK. A second detile of the SUB-RECTANGLE at tiled (128,128), 128 x 128, into
		//      linear (0,0). A round trip cannot catch a field the hardware reads differently from us if the error
		//      is symmetric between tile and detile; a sub-window at a non-zero tiled origin can, because it moves
		//      tiled_x/tiled_y, the rect and the linear pitch independently of the surface extent.
		if (mode == 5) {
			static constexpr uint32_t kTW = 256u, kTH = 256u, kTN = kTW * kTH;
			const uint64_t linBytes = (uint64_t)kTN * 4u;
			const uint64_t tilBytes = n48_addr3_64kb_2d_bytes_4bpe(kTW, kTH);
			uint32_t rptr = 0;
			st = scanout_queue_check(dev, inst, &rptr);
			N48LOG("scanout-tiled: SDMA0 QUEUE0 inited %d enabled %d wptr %llu (ring index %u), write-back rptr %#x -> %s",
			       inst.inited, inst.enabled, (unsigned long long)inst.wptr,
			       (uint32_t)(inst.wptr & inst.ring_ptr_mask), rptr,
			       st == kScanStOk ? "IDLE" : (st == kScanStQueueBusy ? "BUSY - REFUSING" : "NOT UP - REFUSING"));
			if (st != kScanStOk) goto done;
			if (!gBringup.gmc.vram_alloc.alloc(tilBytes, 65536, &a) ||
			    !gBringup.gmc.vram_alloc.alloc(linBytes, 65536, &b) ||
			    !gBringup.gmc.vram_alloc.alloc(tilBytes, 65536, &c)) { st = kScanStAlloc; goto done; }
			const uint64_t tOff = a.gpu_va - gBringup.gmc.vram_start;     // tiled, CPU-written from the equation
			const uint64_t dOff = b.gpu_va - gBringup.gmc.vram_start;     // linear destination
			const uint64_t t2Off = c.gpu_va - gBringup.gmc.vram_start;    // tiled, SDMA-written (the cross-check)
			if (tOff + tilBytes > dev.bar0Size || dOff + linBytes > dev.bar0Size ||
			    t2Off + tilBytes > dev.bar0Size || (tOff & 0xffffu) || (t2Off & 0xffffu)) { st = kScanStAlloc; goto done; }
			// The known tiled image, plus poison everywhere the copies must write.
			uint32_t nonLinear = 0;
			for (uint32_t y = 0; y < kTH; y++)
				for (uint32_t x = 0; x < kTW; x++) {
					const uint64_t o = n48_addr3_64kb_2d_off_4bpe(x, y, kTW);
					if (o != ((uint64_t)y * kTW + x) * 4u) nonLinear++;
					amdgpu::WBAR0_32(dev, tOff + o, n48_tile_probe_pixel(x, y));
				}
			for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
			for (uint64_t i = 0; i < tilBytes / 4u; i++) amdgpu::WBAR0_32(dev, t2Off + i * 4u, 0xdeadbeefu);
			amdgpu::amdgpu_hdp_flush(dev);
			v[2] = tOff; v[3] = ((uint64_t)kTW << 16) | kTH; v[7] = nonLinear;
			// 0.0.346 — THE TWO CONTROLS 0.0.345 LEFT OUT, and they are not optional: had to add
			// exactly these to the row path after 24 runs could not tell "the copy is wrong" from "the buffer was
			// never what we thought". EVERY source position and EVERY destination dword, both ways, BEFORE the copy.
			{
				uint32_t srcMm = 0, srcBar = 0, poisonBad = 0, firstSrc = 0xffffffffu, firstSrcGot = 0;
				bool mmOk = true;
				for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
					uint32_t mm[64] = { 0 };
					if (!navi48_vram_read_mm(tOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k2 = 0; k2 < 64u; k2++) {
						// Which (x,y) owns tiled dword (i+k2)? Decode the value instead of inverting the equation:
						// the pattern is self-addressing, so re-deriving the address from it is the check.
						const uint32_t got = mm[k2];
						const uint32_t sx = got & 0xffffu, sy = got >> 16;
						const bool ok = sx < kTW && sy < kTH &&
						                n48_addr3_64kb_2d_off_4bpe(sx, sy, kTW) == (uint64_t)(i + k2) * 4u;
						if (!ok) {
							if (firstSrc == 0xffffffffu) { firstSrc = i + k2; firstSrcGot = got; }
							srcMm++;
						}
					}
				}
				for (uint32_t i = 0; i < kTN; i++) {
					const uint32_t got = amdgpu::RBAR0_32(dev, tOff + (uint64_t)i * 4u);
					const uint32_t sx = got & 0xffffu, sy = got >> 16;
					if (!(sx < kTW && sy < kTH && n48_addr3_64kb_2d_off_4bpe(sx, sy, kTW) == (uint64_t)i * 4u)) srcBar++;
					if (amdgpu::RBAR0_32(dev, dOff + (uint64_t)i * 4u) != 0xdeadbeefu) poisonBad++;
				}
				N48LOG("scanout-tiled: buffers tiled %#llx (MC %#llx, %llu bytes) linear %#llx (MC %#llx) tiled2 %#llx; "
				       "surface %ux%u at 32 bpp, swizzle 3 (ADDR3_64KB_2D); %u of %u position(s) are NOT at their linear "
				       "address (the negative control)", (unsigned long long)tOff, (unsigned long long)a.gpu_va,
				       (unsigned long long)tilBytes, (unsigned long long)dOff, (unsigned long long)b.gpu_va,
				       (unsigned long long)t2Off, kTW, kTH, nonLinear, kTN);
				N48LOG("scanout-tiled: CONTROLS BEFORE THE COPY, every dword both ways: the WHOLE tiled source holds the "
				       "self-addressing pattern - MM WINDOW %s %u of %u wrong, BAR0 %u wrong (first bad tiled dword %u reads "
				       "%#010x); the destination poison landed everywhere - %u of %u dword(s) were NOT 0xdeadbeef. A "
				       "non-zero here voids everything below", mmOk ? "read" : "UNREADABLE", srcMm, kTN, srcBar,
				       firstSrc == 0xffffffffu ? 0u : firstSrc, firstSrcGot, poisonBad, kTN);
				v[9] = (uint64_t)srcMm | ((uint64_t)srcBar << 16) | ((uint64_t)poisonBad << 32);
				if (!mmOk || srcMm || srcBar || poisonBad) { st = kScanStSourceCtl; goto done; }
			}
			N48ScanoutTiledPlan p {};
			p.fbOff = dOff; p.srcOff = tOff; p.surfW = kTW; p.surfH = kTH;
			p.swizzle = amdgpu::kAddr3_64KB_2D; p.elementLog2 = 2u;
			p.srcX = 0; p.srcY = 0; p.dstX = 0; p.dstY = 0;
			p.linPitch = kTW; p.linSlice = kTN; p.w = kTW; p.h = kTH;
			uint32_t detileBad = 0xffffffffu, tileBad = 0xffffffffu, subBad = 0xffffffffu, discBad = 0xffffffffu;
			// 1. THE VERDICT — detile, and when it disagrees, WHERE the bytes came from.
			{
				uint64_t us = 0; uint32_t last = 0;
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + tOff,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D00u, 1000, &us, &last);
				v[4] = us;
				uint32_t mmBad = 0, barBad = 0, poison = 0, firstIdx = 0, firstGot = 0, ns = 0;
				uint32_t sIdx[6] = { 0 }, sGot[6] = { 0 };
				uint32_t bitSet[18] = { 0 }, bucket[32] = { 0 };
				uint64_t loAddr = ~0ull, hiAddr = 0;
				bool mmOk = true;
				if (st == kScanStOk) {
					for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
						uint32_t mm[64] = { 0 };
						if (!navi48_vram_read_mm(dOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
						for (uint32_t k2 = 0; k2 < 64u; k2++) {
							const uint32_t idx = i + k2, dx = idx % kTW, dy = idx / kTW;
							if (mm[k2] == n48_tile_probe_pixel(dx, dy)) continue;
							if (!mmBad) { firstIdx = idx; firstGot = mm[k2]; }
							mmBad++;
							if (mm[k2] == 0xdeadbeefu) poison++;
							if (ns < 6) { sIdx[ns] = idx; sGot[ns] = mm[k2]; ns++; }
							// THE MAP: every wrong destination pixel has ONE tiled source address it should have
							// come from. Which bits of that address are always set among the wrong ones NAMES the
							// fault - a single always-set bit is a bit we and the engine disagree about; a
							// contiguous range is a truncation.
							const uint64_t sa = n48_addr3_64kb_2d_off_4bpe(dx, dy, kTW);
							if (sa < loAddr) loAddr = sa;
							if (sa > hiAddr) hiAddr = sa;
							for (unsigned bit = 0; bit < 18u; bit++) if (sa & (1ull << bit)) bitSet[bit]++;
							bucket[(sa >> 13) & 31u]++;
						}
					}
					for (uint32_t i = 0; i < kTN; i++)
						if (amdgpu::RBAR0_32(dev, dOff + (uint64_t)i * 4u) != n48_tile_probe_pixel(i % kTW, i / kTW)) barBad++;
				}
				detileBad = (st == kScanStOk && mmOk) ? mmBad : 0xffffffffu;
				v[1] = (uint64_t)mmBad | ((uint64_t)barBad << 32);
				v[6] = ((uint64_t)firstIdx << 32) | firstGot;
				N48LOG("scanout-tiled: DETILE (one 14-dword COPY_TILED_SUB_WINDOW, detile 1, dcc 0, CPV 0, + FENCE): status %u, "
				       "fence %#010x after %llu us; destination %ux%u compared EVERY pixel - MM WINDOW %s %u wrong of %u "
				       "(still poison %u), BAR0 %u wrong%s", st, last, (unsigned long long)us, kTW, kTH,
				       mmOk ? "read" : "UNREADABLE", mmBad, kTN, poison, barBad,
				       (st == kScanStOk && mmOk && !mmBad) ? " - THE DETILE IS CORRECT AT EVERY POSITION" : " - FAILED");
				if (ns) {
					// What does the SOURCE actually hold at the address each wrong pixel should have come from?
					// Source right + destination wrong = the engine read somewhere else. Both wrong = our write.
					uint32_t sv[6] = { 0 };
					uint64_t sa[6] = { 0 };
					for (unsigned i2 = 0; i2 < ns; i2++) {
						sa[i2] = n48_addr3_64kb_2d_off_4bpe(sIdx[i2] % kTW, sIdx[i2] / kTW, kTW);
						(void)navi48_vram_read_mm(tOff + sa[i2], &sv[i2], 1);
					}
					N48LOG("scanout-tiled: DETILE first %u mismatch(es) — destination (x,y): value READ / value WANTED / the "
					       "tiled source address it should have come from / what the SOURCE HOLDS there right now -- "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x; "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x; "
					       "(%u,%u): %#010x/%#010x/%#07llx/%#010x; (%u,%u): %#010x/%#010x/%#07llx/%#010x", ns,
					       sIdx[0] % kTW, sIdx[0] / kTW, sGot[0], n48_tile_probe_pixel(sIdx[0] % kTW, sIdx[0] / kTW), (unsigned long long)sa[0], sv[0],
					       sIdx[1] % kTW, sIdx[1] / kTW, sGot[1], n48_tile_probe_pixel(sIdx[1] % kTW, sIdx[1] / kTW), (unsigned long long)sa[1], sv[1],
					       sIdx[2] % kTW, sIdx[2] / kTW, sGot[2], n48_tile_probe_pixel(sIdx[2] % kTW, sIdx[2] / kTW), (unsigned long long)sa[2], sv[2],
					       sIdx[3] % kTW, sIdx[3] / kTW, sGot[3], n48_tile_probe_pixel(sIdx[3] % kTW, sIdx[3] / kTW), (unsigned long long)sa[3], sv[3],
					       sIdx[4] % kTW, sIdx[4] / kTW, sGot[4], n48_tile_probe_pixel(sIdx[4] % kTW, sIdx[4] / kTW), (unsigned long long)sa[4], sv[4],
					       sIdx[5] % kTW, sIdx[5] / kTW, sGot[5], n48_tile_probe_pixel(sIdx[5] % kTW, sIdx[5] / kTW), (unsigned long long)sa[5], sv[5]);
					N48LOG("scanout-tiled: DETILE MAP — of %u wrong pixel(s), the tiled source address spans %#07llx..%#07llx; "
					       "wrong pixels whose source address has bit N set, N=2..17: %u %u %u %u %u %u %u %u %u %u %u %u %u %u "
					       "%u %u (a count equal to %u means EVERY wrong pixel has that bit set, 0 means none do)",
					       mmBad, (unsigned long long)(loAddr == ~0ull ? 0 : loAddr), (unsigned long long)hiAddr,
					       bitSet[2], bitSet[3], bitSet[4], bitSet[5], bitSet[6], bitSet[7], bitSet[8], bitSet[9],
					       bitSet[10], bitSet[11], bitSet[12], bitSet[13], bitSet[14], bitSet[15], bitSet[16], bitSet[17], mmBad);
					N48LOG("scanout-tiled: DETILE MAP — wrong pixels per 8 KiB of tiled source address (32 buckets, %#x total): "
					       "%u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u | %u %u %u %u %u %u %u %u",
					       (unsigned)tilBytes, bucket[0], bucket[1], bucket[2], bucket[3], bucket[4], bucket[5], bucket[6], bucket[7],
					       bucket[8], bucket[9], bucket[10], bucket[11], bucket[12], bucket[13], bucket[14], bucket[15],
					       bucket[16], bucket[17], bucket[18], bucket[19], bucket[20], bucket[21], bucket[22], bucket[23],
					       bucket[24], bucket[25], bucket[26], bucket[27], bucket[28], bucket[29], bucket[30], bucket[31]);
				}
				if (st != kScanStOk) goto done;
			}
			// 2. THE CROSS-CHECK — tile the verified linear result back and compare against the same equation.
			// 0.0.346: this RUNS EVEN WHEN THE DETILE FAILED. A detile that is wrong while the tile direction is right
			// is a different fault from both being wrong, and 0.0.345 threw that distinction away by aborting first.
			// The linear buffer is re-seeded by the CPU so the tile direction starts from a KNOWN image either way.
			{
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r2 = 0;
				st = scanout_queue_check(dev, inst, &r2);
				if (st != kScanStOk) { N48LOG("scanout-tiled: SDMA0 QUEUE0 is not idle after the detile (status %u, rptr %#x) - "
				                              "REFUSING the cross-check", st, r2); goto done; }
				for (uint32_t y = 0; y < kTH; y++)
					for (uint32_t x = 0; x < kTW; x++)
						amdgpu::WBAR0_32(dev, dOff + ((uint64_t)y * kTW + x) * 4u, n48_tile_probe_pixel(x, y));
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + t2Off,
				                             gBringup.gmc.vram_start + dOff, false, 0x5CA11D01u, 1000, &us, &last);
				v[4] |= us << 32;
				uint32_t bad = 0, stillPoison = 0, fx = 0, fy = 0, fgot = 0;
				if (st == kScanStOk)
					for (uint32_t y = 0; y < kTH; y++)
						for (uint32_t x = 0; x < kTW; x++) {
							const uint32_t got = amdgpu::RBAR0_32(dev, t2Off + n48_addr3_64kb_2d_off_4bpe(x, y, kTW));
							if (got == n48_tile_probe_pixel(x, y)) continue;
							if (!bad) { fx = x; fy = y; fgot = got; }
							if (got == 0xdeadbeefu) stillPoison++;
							bad++;
						}
				tileBad = st == kScanStOk ? bad : 0xffffffffu;
				v[5] = bad;
				N48LOG("scanout-tiled: TILE BACK (detile 0, same packet, same swizzle, from a CPU-written linear image): "
				       "status %u, fence %#010x after %llu us; %u of %u position(s) are NOT where the addrlib equation says "
				       "(%u still hold the 0xdeadbeef poison, i.e. never written) - %s. First disagreement (%u,%u) reads "
				       "%#010x. With %u position(s) differing from linear, a pass here also rules out SDMA ignoring the "
				       "swizzle field", st, last, (unsigned long long)us, bad, kTN, stillPoison,
				       (st == kScanStOk && !bad) ? "THE WRITE SIDE AGREES WITH THE EQUATION" : "FAILED", fx, fy, fgot, nonLinear);
				if (st != kScanStOk) goto done;
			}
			// 2b. 0.0.347 — THE DISCRIMINATOR. Detile the tiled image SDMA ITSELF just wrote, which
			// TILE BACK has verified byte-for-byte against the equation, into the same linear buffer. The ONLY thing
			// that changes from step 1 is WHO WROTE THE TILED SOURCE: the CPU through BAR0, or the engine through
			// its own path. Same packet, same fields, same swizzle, same destination, same addresses.
			//   clean here + dirty in step 1  -> the packet and the equation are correct and the fault is that the
			//                                    engine does not see the CPU's writes to that buffer
			//   dirty here too               -> the tiled READ path itself is wrong and the packet is not usable
			{
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r4 = 0;
				st = scanout_queue_check(dev, inst, &r4);
				if (st != kScanStOk) { N48LOG("scanout-tiled: SDMA0 QUEUE0 is not idle before the discriminator (status %u, "
				                              "rptr %#x) - REFUSING", st, r4); goto done; }
				for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &p, gBringup.gmc.vram_start + t2Off,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D03u, 1000, &us, &last);
				uint32_t bad = 0, poison2 = 0, firstIdx = 0, firstGot = 0;
				bool mmOk = true;
				if (st == kScanStOk)
					for (uint32_t i = 0; i < kTN && mmOk; i += 64) {
						uint32_t mm[64] = { 0 };
						if (!navi48_vram_read_mm(dOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
						for (uint32_t k2 = 0; k2 < 64u; k2++) {
							const uint32_t idx = i + k2;
							if (mm[k2] == n48_tile_probe_pixel(idx % kTW, idx / kTW)) continue;
							if (!bad) { firstIdx = idx; firstGot = mm[k2]; }
							if (mm[k2] == 0xdeadbeefu) poison2++;
							bad++;
						}
					}
				discBad = (st == kScanStOk && mmOk) ? bad : 0xffffffffu;
				v[8] = bad;
				N48LOG("scanout-tiled: DISCRIMINATOR — the SAME detile packet, the SAME destination, but the tiled source is "
				       "the one SDMA WROTE (%#llx, verified byte-for-byte by TILE BACK) instead of the one the CPU wrote "
				       "(%#llx): status %u, fence %#010x after %llu us; MM WINDOW %s %u of %u wrong (%u still poison), first "
				       "(%u,%u) reads %#010x. %s", (unsigned long long)t2Off, (unsigned long long)tOff, st, last,
				       (unsigned long long)us, mmOk ? "read" : "UNREADABLE", bad, kTN, poison2,
				       firstIdx % kTW, firstIdx / kTW, firstGot,
				       (st == kScanStOk && mmOk && !bad)
				           ? "CLEAN: the packet and the equation are CORRECT; step 1's failure is the engine not seeing the "
				             "CPU's writes to that buffer, not the tiling"
				           : "ALSO DIRTY: the tiled READ path is wrong independently of who wrote the source");
				if (st != kScanStOk) goto done;
			}
			// 3. THE ASYMMETRIC FIELD CHECK — a sub-window at a non-zero tiled origin, out of the CPU-written source.
			{
				N48ScanoutTiledPlan q = p;
				q.srcX = 128; q.srcY = 128; q.dstX = 0; q.dstY = 0; q.w = 128; q.h = 128;
				uint64_t us = 0; uint32_t last = 0;
				uint32_t r3 = 0;
				st = scanout_queue_check(dev, inst, &r3);
				if (st != kScanStOk) { N48LOG("scanout-tiled: SDMA0 QUEUE0 is not idle after the tile back (status %u, rptr %#x) - "
				                              "REFUSING the sub-window check", st, r3); goto done; }
				for (uint64_t i = 0; i < linBytes / 4u; i++) amdgpu::WBAR0_32(dev, dOff + i * 4u, 0xdeadbeefu);
				amdgpu::amdgpu_hdp_flush(dev);
				st = scanout_sdma_tiled_copy(dev, inst, &q, gBringup.gmc.vram_start + tOff,
				                             gBringup.gmc.vram_start + dOff, true, 0x5CA11D02u, 1000, &us, &last);
				uint32_t bad = 0, outside = 0, fx = 0, fy = 0, fgot = 0;
				bool mmOk = true;
				if (st == kScanStOk) {
					for (uint32_t y = 0; y < q.h && mmOk; y++)
						for (uint32_t x = 0; x < q.w; x += 64) {
							uint32_t mm[64] = { 0 };
							if (!navi48_vram_read_mm(dOff + ((uint64_t)y * q.linPitch + x) * 4u, mm, 64)) { mmOk = false; break; }
							for (uint32_t k2 = 0; k2 < 64u; k2++) {
								if (mm[k2] == n48_tile_probe_pixel(x + k2 + q.srcX, y + q.srcY)) continue;
								if (!bad) { fx = x + k2; fy = y; fgot = mm[k2]; }
								bad++;
							}
						}
					for (uint32_t y = 0; y < 4u; y++) {
						uint32_t one = 0;
						if (navi48_vram_read_mm(dOff + ((uint64_t)y * q.linPitch + q.w) * 4u, &one, 1) && one != 0xdeadbeefu) outside++;
					}
				}
				subBad = (st == kScanStOk && mmOk) ? bad : 0xffffffffu;
				v[5] |= (uint64_t)bad << 32;
				N48LOG("scanout-tiled: SUB-WINDOW detile of tiled (%u,%u) %ux%u into linear (0,0) pitch %u: status %u, fence "
				       "%#010x after %llu us; MM WINDOW %s %u of %u wrong%s (first (%u,%u) reads %#010x); bytes written past "
				       "the rectangle in the first 4 rows: %u of 4 (must be 0)", q.srcX, q.srcY, q.w, q.h, q.linPitch, st, last,
				       (unsigned long long)us, mmOk ? "read" : "UNREADABLE", bad, q.w * q.h,
				       (st == kScanStOk && mmOk && !bad && !outside) ? " - EVERY PIXEL OF THE SUB-WINDOW IS CORRECT" : " - FAILED",
				       fx, fy, fgot, outside);
				v[10] = outside;
				if (st != kScanStOk) goto done;
			}
			N48LOG("scanout-tiled: SUMMARY — detile (CPU-written source) %u wrong, tile back %u wrong, DISCRIMINATOR "
			       "(SDMA-written source) %u wrong, sub-window %u wrong, of %u each (0xffffffff = the step did not run). "
			       "None of them says whether Apple's composited surface is in THIS mode - that is what an armed run asks",
			       detileBad, tileBad, discBad, subBad, kTN);
			if (detileBad || tileBad || subBad || discBad || v[10]) st = kScanStReadback;
			goto done;
		}
		// 0.0.414 (an internal design note) — MODE 6: THE FULL-GEOMETRY SDMA SELF-TEST.
		// ONE call, two cases IN ORDER (S1): (a) 256x256, the control, and (b) 1920x1080, the live plane's
		// exact packet fields (swizzle 3, pitch 1920, 15 blocks wide, a partial 9th block row). Both run S3/S4's
		// method on OUR OWN three scratch buffers and never touch the scanout or Apple's surfaces. Mode 5's
		// 256x256 proof is byte-for-byte unchanged; this mode is purely additive. The maps and the decoded
		// pixels are in the `scanout-full:` driver-log lines (S6, every line < 512 bytes); out[] carries the
		// per-case counts: 1|2 case0 tile|detile wrong, poison|bar-mm, tile|detile sampled; 4|5|6 the same for
		// case1; 7|8 case tile-status|detile-status.
		if (mode == 6) {
			N48FullCase c0 {}, c1 {};
			// build 0.0.514 B3: case (b) is the LIVE geometry (Console,Width/Height, read above into `g`),
			// 1920x1080 only when it is absent - 0.0.513's constant, so a 1080p boot runs exactly 0.0.513's case.
			const uint32_t lw = n48_live_dim(g.width, 1920u), lh = n48_live_dim(g.height, 1080u);
			const uint32_t s0 = scanout_full_case(dev, inst, 256u, 256u, 0x5CA11F00u, &c0, false);
			const uint32_t s1 = scanout_full_case(dev, inst, lw, lh, 0x5CA11F10u, &c1, false);
			v[1] = (uint64_t)c0.tileWrong | ((uint64_t)c0.detWrong << 32);
			v[2] = (uint64_t)c0.poison | ((uint64_t)c0.barMm << 32);
			v[3] = (uint64_t)c0.tileSampled | ((uint64_t)c0.detSampled << 32);
			v[4] = (uint64_t)c1.tileWrong | ((uint64_t)c1.detWrong << 32);
			v[5] = (uint64_t)c1.poison | ((uint64_t)c1.barMm << 32);
			v[6] = (uint64_t)c1.tileSampled | ((uint64_t)c1.detSampled << 32);
			v[7] = ((uint64_t)c0.tileStatus << 32) | c0.detStatus;
			v[8] = ((uint64_t)c1.tileStatus << 32) | c1.detStatus;
			st = s0 != kScanStOk ? s0 : (s1 != kScanStOk ? s1 : kScanStOk);
			// The 256x256 control must still pass. A dirty live case is a MEASUREMENT, not a refusal.
			if (st == kScanStOk && (c0.tileWrong || c0.detWrong || c0.poison)) st = kScanStReadback;
			N48LOG("scanout-full: SUMMARY mode 6 — control 256x256 tile %u/%u, detile %u/%u, poison %u; live "
			       "%ux%u tile %u/%u, detile %u/%u, poison %u; statuses %u/%u and %u/%u -> %u",
			       c0.tileWrong, c0.tileSampled, c0.detWrong, c0.detSampled, c0.poison, lw, lh,
			       c1.tileWrong, c1.tileSampled, c1.detWrong, c1.detSampled, c1.poison,
			       c0.tileStatus, c0.detStatus, c1.tileStatus, c1.detStatus, st);
			goto done;
		}
		// D7 (0.0.417, an internal design note) — MODE 8: THE UNIFORM PROBE. Mode 6's live 1920x1080 case
		// EXACTLY - same buffers, same packet fields, same bounded sample set, same `scanout-full:` report lines and
		// the same POISON detection - except the CPU writes N48_TILE_UNIFORM_PIXEL (0xff00ff00) everywhere and every
		// expected value is that value. It is the write-compression half of the DCC fault: with our no-PTE write
		// compression ON a constant 256-byte block is stored as a code the raw MM window reads instead of the pixels;
		// after `sdmadcc 1` the same probe must read 0 wrong. out[4..6] are the unused control slots (0); the live
		// counts are out[7..9] and the statuses out[11], exactly where mode 6 leaves its case-1 counts.
		if (mode == 8) {
			N48FullCase c1 {};
			const uint32_t s1 = scanout_full_case(dev, inst, 1920u, 1080u, 0x5CA11F10u, &c1, true);
			v[4] = (uint64_t)c1.tileWrong | ((uint64_t)c1.detWrong << 32);
			v[5] = (uint64_t)c1.poison | ((uint64_t)c1.barMm << 32);
			v[6] = (uint64_t)c1.tileSampled | ((uint64_t)c1.detSampled << 32);
			v[8] = ((uint64_t)c1.tileStatus << 32) | c1.detStatus;
			st = s1;
			N48LOG("scanout-full: SUMMARY mode 8 (UNIFORM probe %#010x) 1920x1080 tile %u/%u, detile %u/%u, poison %u; "
			       "statuses %u/%u -> %u; the same test with `sdmadcc 1` must read 0 wrong",
			       (unsigned)N48_TILE_UNIFORM_PIXEL, c1.tileWrong, c1.tileSampled, c1.detWrong, c1.detSampled,
			       c1.poison, c1.tileStatus, c1.detStatus, st);
			goto done;
		}
		// 0.0.416 (an internal design note, G2) — MODE 7: the SDMA cache-rinse instrument. READ-ONLY on the
		// source; our own low scratch only; the same interlock-free proof discipline as modes 5/6 (three/four
		// scratch buffers, no WindowServer, no Apple accelerator, no reboot). out: 1 gcr|control<<1, 2 source
		// VRAM offset, 3 scratch VRAM offset, 4 fence us | copy dwords<<32, 5 lines full|part<<16|diff<<32,
		// 6 differing dwords, 7 the 64-page difference map. The maps and first-8 decode are in the log.
		if (mode == 7) {
			st = scanout_gcr_case(dev, inst, arg, v);
			goto done;
		}
		if (mode != 1) { st = kScanStPlan; goto done; }
		gScanoutPcRuns++;
		uint32_t rptr = 0;
		st = scanout_queue_check(dev, inst, &rptr);
		N48LOG("scanout: SDMA0 QUEUE0 inited %d enabled %d wptr %llu (ring index %u), write-back rptr %#x (bytes) -> %s", inst.inited,
		       inst.enabled, (unsigned long long)inst.wptr, (uint32_t)(inst.wptr & inst.ring_ptr_mask), rptr, st == kScanStOk ? "IDLE" : (st == kScanStQueueBusy ? "BUSY - REFUSING" : "NOT UP - REFUSING"));
		if (st != kScanStOk) goto done;
		const uint32_t bytes = kScanPcW * kScanPcH * 4u;
		if (!gBringup.gmc.vram_alloc.alloc(bytes, 16384, &a) || !gBringup.gmc.vram_alloc.alloc(bytes, 16384, &b)) {
			st = kScanStAlloc; goto done;
		}
		const uint64_t aOff = a.gpu_va - gBringup.gmc.vram_start, bOff = b.gpu_va - gBringup.gmc.vram_start;
		if (aOff + bytes > dev.bar0Size || bOff + bytes > dev.bar0Size) { st = kScanStAlloc; goto done; }
		// The pattern, and a poisoned scratch buffer.
		for (uint32_t y = 0; y < kScanPcH; y++)
			for (uint32_t x = 0; x < kScanPcW; x++) {
				amdgpu::WBAR0_32(dev, aOff + ((uint64_t)y * kScanPcW + x) * 4u, n48_pc_pixel(x, y, kScanPcW));
				amdgpu::WBAR0_32(dev, bOff + ((uint64_t)y * kScanPcW + x) * 4u, 0xdeadbeefu);
			}
		amdgpu::amdgpu_hdp_flush(dev);
		// Source positive control, CPU view (every pixel) and the GPU's own view (MM window, row 0).
		{
			uint32_t bad = 0, mm[64] = { 0 }, mmBad = 0;
			for (uint32_t i = 0; i < kScanPcW * kScanPcH; i++)
				if (amdgpu::RBAR0_32(dev, aOff + (uint64_t)i * 4u) != n48_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad++;
			const bool mmOk = navi48_vram_read_mm(aOff, mm, 64);
			for (uint32_t x = 0; x < 64; x++) if (mm[x] != n48_pc_pixel(x, 0, kScanPcW)) mmBad++;
			N48LOG("scanout: pattern buffer VRAM %#llx (MC %#llx), scratch VRAM %#llx (MC %#llx), %u bytes each; source "
			       "control: BAR0 mismatches %u of %u, MM-window row 0 %s mismatches %u of 64 (mm[16] %#010x)",
			       (unsigned long long)aOff, (unsigned long long)a.gpu_va, (unsigned long long)bOff,
			       (unsigned long long)b.gpu_va, bytes, bad, kScanPcW * kScanPcH, mmOk ? "read" : "UNREADABLE", mmBad, mm[16]);
			if (bad || !mmOk || mmBad) { st = kScanStSourceCtl; goto done; }
		}
		// Pre-flight: pattern -> scratch, nothing near the scanout.
		//
		// 0.0.337 — THE INSTRUMENT. left this as the unresolved limitation: the compare was a bare
		// `bad++` with no index, no value and no second opinion, so for 24 runs "our SDMA copy corrupted 7904 of 16384
		// pixels" and "the CPU's BAR0 view of a perfectly copied buffer is stale" were indistinguishable — two faults
		// with nothing in common and no shared fix. The same bytes are now read FOUR ways:
		//   1. BAR0, first pass, with every mismatch CLASSIFIED — 0xdeadbeef is the poison this function wrote before
		//      the copy (so: never written, or a stale read of the pre-copy value); the right colour with a wrong row
		//      byte is an addressing fault; anything else is real corruption — and the first six logged with index,
		//      value read and value expected.
		//   2. BAR0 again immediately, nothing in between: a difference means the first pass was early or stale.
		//   3. BAR0 once more after amdgpu_hdp_flush — the SAME call this function already makes at the top of this
		//      block and again inside scanout_sdma_copies. No new register and no new register offset is written.
		//   4. THE MM WINDOW over the whole buffer — MM_INDEX/MM_DATA, the GPU's own view of VRAM, which this file
		//      already designates as the right instrument for memory written by SDMA rather than by a CPU (the comment
		//      on navi48_vram_read_mm) and which the source control 20 lines above already uses on this same buffer.
		// THE MM WINDOW IS THE VERDICT. That is not a relaxation of the interlock. What the interlock protects is that
		// OUR SDMA0 QUEUE0 copy puts the right bytes in VRAM, and the MM window reads VRAM; a BAR0 aperture read of
		// memory a GPU engine has just written, in a driver that issues no HDP read-cache invalidate anywhere, is the
		// weaker instrument, not the stronger one. Every BAR0 number is still measured, logged and returned, so the two
		// paths can never disagree silently, and if the copy is genuinely wrong BOTH fail and the interlock still holds.
		{
			uint64_t us = 0; uint32_t last = 0;
			uint32_t bad = 0, bad2 = 0, bad3 = 0, mmBad = 0, poison = 0, colour = 0, other = 0;
			uint32_t firstIdx = 0, firstGot = 0, ns = 0;
			uint32_t sIdx[6] = { 0 }, sGot[6] = { 0 }, sWant[6] = { 0 };
			bool mmOk = true;
			const uint32_t total = kScanPcW * kScanPcH;
			const uint64_t s1 = a.gpu_va, d1 = b.gpu_va; const uint32_t n1 = bytes;
			// 0.0.338: two gaps 0.0.337 left open, closed BEFORE the copy runs.
			//  - `poisonBad` — nothing ever verified that the 0xdeadbeef poison actually LANDED. "poison 0
			//    after the copy" only means "the copy wrote everywhere" if the poison was everywhere first.
			//  - `srcAfter` (below, after the copy) — nothing ever re-read the SOURCE afterwards. If anything
			//    else on this machine writes VRAM under us, the source is where it would show.
			uint32_t poisonBad = 0, srcAfter = 0;
			for (uint32_t i = 0; i < total; i++)
				if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != 0xdeadbeefu) poisonBad++;
			uint32_t alien = 0, dRowOther = 0, dRowHist[7] = { 0 }, dBarHist[16] = { 0 }, rowBad[kScanPcH] = { 0 };
			char map[kScanPcH + 1];
			st = scanout_sdma_copies(dev, inst, &s1, &d1, &n1, 1, 0x5CA11ED0u, 500, &us, &last);
			if (st == kScanStOk) {
				for (uint32_t i = 0; i < total; i++) {
					const uint32_t want = n48_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW);
					const uint32_t got = amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u);
					if (got == want) continue;
					if (!bad) { firstIdx = i; firstGot = got; }
					bad++;
					if (got == 0xdeadbeefu) poison++;
					else if ((got & 0x00ffffffu) == (want & 0x00ffffffu)) colour++;
					else other++;
					if (ns < 6) { sIdx[ns] = i; sGot[ns] = got; sWant[ns] = want; ns++; }
					// Where did this dword come from? Decode the value back into the source pixel that owns it.
					rowBad[i / kScanPcW]++;
					const int sb = scanout_bar_index(got);
					const uint32_t sy = got >> 24;
					if (sb < 0 || sy >= kScanPcH) { alien++; continue; }
					const int dRow = (int)sy - (int)(i / kScanPcW);
					const int dBar = sb - (int)((i % kScanPcW) / 32u);
					if (dRow >= -3 && dRow <= 3) dRowHist[dRow + 3]++; else dRowOther++;
					dBarHist[(dBar + 8) & 15]++;
				}
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != n48_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad2++;
				amdgpu::amdgpu_hdp_flush(dev);
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, bOff + (uint64_t)i * 4u) != n48_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) bad3++;
				for (uint32_t i = 0; i < total && mmOk; i += 64) {
					uint32_t mm[64] = { 0 };
					if (!navi48_vram_read_mm(bOff + (uint64_t)i * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k = 0; k < 64; k++)
						if (mm[k] != n48_pc_pixel((i + k) % kScanPcW, (i + k) / kScanPcW, kScanPcW)) mmBad++;
				}
				for (uint32_t i = 0; i < total; i++)
					if (amdgpu::RBAR0_32(dev, aOff + (uint64_t)i * 4u) != n48_pc_pixel(i % kScanPcW, i / kScanPcW, kScanPcW)) srcAfter++;
			}
			for (uint32_t y = 0; y < kScanPcH; y++)
				map[y] = rowBad[y] == 0 ? '.' : (rowBad[y] >= kScanPcW ? '#' : "0123456789abcdef"[(rowBad[y] * 16u) / kScanPcW]);
			map[kScanPcH] = 0;
			v[4] = (uint64_t)bad | ((uint64_t)mmBad << 32);
			v[5] = us << 32;
			v[1] |= ((uint64_t)(bad2 & 0xffffu) << 16) | ((uint64_t)(bad3 & 0xffffu) << 32) |
			        ((uint64_t)(poison & 0xffffu) << 48);
			v[7] = ((uint64_t)firstIdx << 32) | firstGot;    // pre-flight only; the rectangle readback overwrites it if we get there
			N48LOG("scanout: PRE-FLIGHT copy pattern -> scratch: status %u, fence %#010x after %llu us; BAR0 pass 1 %u of "
			       "%u wrong (poison 0xdeadbeef %u, right colour wrong row byte %u, other %u), BAR0 pass 2 (nothing in "
			       "between) %u, BAR0 pass 3 (after amdgpu_hdp_flush) %u; MM WINDOW %s %u of %u wrong. THE MM WINDOW IS "
			       "THE VERDICT : it is the GPU's own view of the VRAM the copy wrote, and no HDP "
			       "read-cache invalidate exists on the BAR0 path", st, last, (unsigned long long)us, bad, total,
			       poison, colour, other, bad2, bad3, mmOk ? "read" : "UNREADABLE", mmBad, total);
			N48LOG("scanout: PRE-FLIGHT MAP, one char per 256-pixel row of the scratch buffer ('.' = that row is entirely "
			       "correct, '#' = all 256 wrong, otherwise the hex of wrong*16/256): %s", map);
			N48LOG("scanout: PRE-FLIGHT DISPLACEMENT — every pattern value names its own source (colour = source x/32, top "
			       "byte = source row), so a wrong dword says where it came from: alien (not any bar value) %u; source-row "
			       "delta -3..+3 = %u %u %u %u %u %u %u and |delta|>3 %u; source-bar delta -8..+7 = %u %u %u %u %u %u %u %u "
			       "%u %u %u %u %u %u %u %u. CONTROLS THIS RUN: poison landed everywhere? %u of %u dwords were NOT 0xdeadbeef "
			       "before the copy; source still intact after the copy? %u of %u wrong",
			       alien, dRowHist[0], dRowHist[1], dRowHist[2], dRowHist[3], dRowHist[4], dRowHist[5], dRowHist[6], dRowOther,
			       dBarHist[0], dBarHist[1], dBarHist[2], dBarHist[3], dBarHist[4], dBarHist[5], dBarHist[6], dBarHist[7],
			       dBarHist[8], dBarHist[9], dBarHist[10], dBarHist[11], dBarHist[12], dBarHist[13], dBarHist[14], dBarHist[15],
			       poisonBad, total, srcAfter, total);
			N48LOG("scanout: PRE-FLIGHT first %u mismatch(es) as index: read / expected -- %u: %#010x/%#010x, %u: %#010x/%#010x, "
			       "%u: %#010x/%#010x, %u: %#010x/%#010x, %u: %#010x/%#010x, %u: %#010x/%#010x", ns,
			       sIdx[0], sGot[0], sWant[0], sIdx[1], sGot[1], sWant[1], sIdx[2], sGot[2], sWant[2],
			       sIdx[3], sGot[3], sWant[3], sIdx[4], sGot[4], sWant[4], sIdx[5], sGot[5], sWant[5]);
			if (st != kScanStOk) goto done;
			if (!mmOk || mmBad) { st = kScanStPreflight; goto done; }
		}
		// Plan the rectangle, save what is there, copy, read every pixel back.
		N48ScanoutPlan plan {};
		{
			const uint32_t pr = n48_scanout_plan_rows(&g, aOff, bytes, kScanPcW, kScanPcH, kScanPcW * 4u, 0, 0,
			                                          kScanPcX, kScanPcY, kScanPcW, kScanPcH, kScanMaxRows, &plan);
			v[1] |= (uint64_t)pr << 8;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr || plan.w != kScanPcW || plan.h != kScanPcH) { st = kScanStPlan; goto done; }
		}
		if (!gScanoutSave) gScanoutSave = static_cast<uint32_t *>(IOMalloc(kScanPcW * kScanPcH * sizeof(uint32_t)));
		if (gScanoutSave) {
			for (uint32_t y = 0; y < plan.h; y++)
				for (uint32_t x = 0; x < plan.w; x++)
					gScanoutSave[y * plan.w + x] = amdgpu::RBAR0_32(dev, n48_scanout_row_dst(&plan, g.rowBytes, y) + (uint64_t)x * 4u);
			gScanoutSaveX = plan.dstX; gScanoutSaveY = plan.dstY; gScanoutSaveW = plan.w; gScanoutSaveH = plan.h;
		}
		{
			uint64_t srcMc[kScanPcH], dstMc[kScanPcH]; uint32_t nb[kScanPcH];
			for (uint32_t i = 0; i < plan.h; i++) {
				const uint64_t d = n48_scanout_row_dst(&plan, g.rowBytes, i);
				if (!n48_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; goto done; }
				srcMc[i] = gBringup.gmc.vram_start + n48_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0;
			st = scanout_sdma_copies(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11ED1u, 1000, &us, &last);
			v[5] |= us;
			N48LOG("scanout: POSITIVE CONTROL copy %u rows x %u bytes, MC %#llx.. -> MC %#llx.. (rect %u,%u %ux%u): "
			       "status %u, fence %#010x after %llu us", plan.h, plan.rowBytes, (unsigned long long)srcMc[0],
			       (unsigned long long)dstMc[0], plan.dstX, plan.dstY, plan.w, plan.h, st, last, (unsigned long long)us);
			if (st != kScanStOk) goto done;
		}
		{
			uint32_t bad = 0, firstIdx = 0, firstVal = 0;
			for (uint32_t y = 0; y < plan.h; y++)
				for (uint32_t x = 0; x < plan.w; x++) {
					const uint32_t got = amdgpu::RBAR0_32(dev, n48_scanout_row_dst(&plan, g.rowBytes, y) + (uint64_t)x * 4u);
					if (got != n48_pc_pixel(x, y, kScanPcW)) { if (!bad) { firstIdx = y * plan.w + x; firstVal = got; } bad++; }
				}
			// 0.0.337: the same second opinion the pre-flight now takes. The rectangle is inside
			// RDNA4FB's scanout, written by the same SDMA0 QUEUE0 copy and read back through the same BAR0
			// aperture — so if the pre-flight's BAR0 view was stale this one is too. The MM window decides here
			// as well; the BAR0 count and the four named samples are still measured and still reported, so a
			// disagreement between the two paths is visible, never silent.
			uint32_t mmBad = 0; bool mmOk = true;
			for (uint32_t y = 0; y < plan.h && mmOk; y++) {
				const uint64_t rowOff = n48_scanout_row_dst(&plan, g.rowBytes, y);
				for (uint32_t x = 0; x < plan.w; x += 64) {
					uint32_t mm[64] = { 0 };
					if (!navi48_vram_read_mm(rowOff + (uint64_t)x * 4u, mm, 64)) { mmOk = false; break; }
					for (uint32_t k = 0; k < 64; k++)
						if (mm[k] != n48_pc_pixel(x + k, y, kScanPcW)) mmBad++;
				}
			}
			v[6] = (uint64_t)bad | ((uint64_t)mmBad << 32); v[7] = ((uint64_t)firstIdx << 32) | firstVal;
			static const uint32_t sx[4] = { 16, 112, 240, 16 }, sy[4] = { 0, 0, 0, 63 };
			for (unsigned i = 0; i < 4; i++) {
				const uint32_t got = amdgpu::RBAR0_32(dev, n48_scanout_row_dst(&plan, g.rowBytes, sy[i]) + (uint64_t)sx[i] * 4u);
				v[8 + i] = ((uint64_t)n48_pc_pixel(sx[i], sy[i], kScanPcW) << 32) | got;
			}
			N48LOG("scanout: READBACK of the scanout rectangle: BAR0 %u of %u pixels differ from the bars; MM WINDOW %s "
			       "%u of %u differ%s; samples through BAR0 (expected/read) row0 x16 %#010x/%#010x, row0 x112 "
			       "%#010x/%#010x, row0 x240 %#010x/%#010x, row63 x16 %#010x/%#010x", bad, plan.w * plan.h,
			       mmOk ? "read" : "UNREADABLE", mmBad, plan.w * plan.h,
			       (!mmOk || mmBad) ? " - FAILED" : " - EVERY PIXEL MATCHES IN THE GPU'S OWN VIEW",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], (uint32_t)(v[9] >> 32), (uint32_t)v[9],
			       (uint32_t)(v[10] >> 32), (uint32_t)v[10], (uint32_t)(v[11] >> 32), (uint32_t)v[11]);
			if (!mmOk || mmBad) { st = kScanStReadback; goto done; }
			gScanoutPcPassed = true;
			gScanoutCopies++;
		}
	}
done:
	scanout_free_vram(a);
	scanout_free_vram(b);
	scanout_free_vram(c);
	if (st != kScanStOk && mode != 0) { gScanoutRefusals++; gScanoutLastReason = st; }
	IOLockUnlock(gScanoutLock);
done_nolock:
	v[0] = st;
	if (mode != 3 && mode != 4 && mode != N48_SF_MODE && mode != N48_SF_MODE_RELEASE)   // 0.0.280: modes 3/4 (0.0.542: 9/10) keep their own out[12]
		v[12] = (gScanoutPcPassed ? 1u : 0u) | ((uint64_t)(gScanoutPcRuns & 0xffff) << 8) |
		        ((uint64_t)(gScanoutCopies & 0xffff) << 24) | ((uint64_t)(gScanoutRefusals & 0xffff) << 40);
	N48LOG("scanout: mode %u -> status %u; interlock %s (positive-control runs %u, verified copies %u, refusals %u)",
	       mode, st, gScanoutPcPassed ? "PASSED" : "not passed", gScanoutPcRuns, gScanoutCopies, gScanoutRefusals);
	if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
	return st;
}

// 0.0.412 — S2's RICHER READBACK. One sample per 16x16 destination cell (256 samples), compared with the
// source at the same geometric position: the tiled path detiles with the SAME equation the copy used, the linear path
// reads (stride, x). Read-only. The caller has already bounded how often this runs (n48_dpg_verify_due). Prints ONE
// line, under 480 body bytes: how many samples differ, the 256-bit cell map as four 64-bit words, and the cell and
// dst/src values of the first 8 differing samples. The cells, centres and bit packing are display_pipe_guard.h's pure
// helpers; this function only does the reads.
struct N48VerifySource {
	uint64_t off; uint32_t stride; uint32_t surfW; uint32_t srcX, srcY; int tiled;
};
static void n48_scanout_verify_cells(amdgpu::DeviceContext &dev, uint64_t fbOff, uint32_t dstX, uint32_t dstY,
                                     uint32_t w, uint32_t h, uint32_t linPitch, const N48VerifySource &s)
{
	if (w == 0 || h == 0 || linPitch == 0) return;
	uint64_t map[N48_DPG_VERIFY_MAP_WORDS] = { 0, 0, 0, 0 };
	uint32_t differ = 0, fx[N48_DPG_VERIFY_FIRST] = { 0 }, fd[N48_DPG_VERIFY_FIRST] = { 0 },
	         fs[N48_DPG_VERIFY_FIRST] = { 0 };
	for (uint32_t cy = 0; cy < N48_DPG_VERIFY_CELLS; cy++) {
		for (uint32_t cx = 0; cx < N48_DPG_VERIFY_CELLS; cx++) {
			uint32_t x = 0, y = 0;
			n48_dpg_cell16_centre(cx, cy, w, h, &x, &y);
			uint32_t sv = 0;
			const uint64_t so = s.tiled
			    ? s.off + n48_addr3_64kb_2d_off_4bpe(s.srcX + x, s.srcY + y, s.surfW)
			    : s.off + (uint64_t)(s.srcY + y) * s.stride + (uint64_t)(s.srcX + x) * 4u;
			const bool sok = navi48_vram_read_mm(so, &sv, 1);
			const uint64_t d = fbOff + ((uint64_t)(dstY + y) * linPitch + (dstX + x)) * 4u;
			const uint32_t dv = amdgpu::RBAR0_32(dev, d);
			if (!sok || dv != sv) {
				n48_dpg_verify_map_set(map, n48_dpg_cell16(x, y, w, h));
				if (differ < N48_DPG_VERIFY_FIRST) { fx[differ] = cy * N48_DPG_VERIFY_CELLS + cx; fd[differ] = dv; fs[differ] = sv; }
				differ++;
			}
		}
	}
	N48LOG("scanout-copy: S2 16x16 %s %ux%u: %u of %u samples differ; map %016llx%016llx%016llx%016llx; first %u: "
	       "c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x c%u=%08x/%08x "
	       "(cell=dst/src, src=%s)", s.tiled ? "tiled" : "linear", w, h, differ, N48_DPG_VERIFY_SAMPLES,
	       (unsigned long long)map[0], (unsigned long long)map[1], (unsigned long long)map[2], (unsigned long long)map[3],
	       differ, fx[0], fd[0], fs[0], fx[1], fd[1], fs[1], fx[2], fd[2], fs[2], fx[3], fd[3], fs[3],
	       fx[4], fd[4], fs[4], fx[5], fd[5], fs[5], fx[6], fd[6], fs[6], fx[7], fd[7], fs[7],
	       s.tiled ? "tiled equation" : "linear rows");
}

// The surface copy, called from the flush hook (Navi48AccelPeer.cpp) AFTER Apple's own selector 10 returned.
// Source = a VRAM buffer [srcOff, srcOff+srcLen) holding srcW x srcH pixels at srcStride. Copies the rectangle
// (0,0,w,h) of it to (dstX,dstY), verifies rows 0 and h-1 in full (destination through BAR0, source through
// BAR0 when it is inside the aperture, else the MM window) and nine samples against the test client's pattern.
// out[0] status, [1] plan reason, [2] srcOff, [3] rect, [4] fence us, [5] row mismatches, [6] rows compared (px),
// [7] client-pattern matches<<32 | samples, [8..10] samples dst<<32|src (0,0), (w/2,h/2), (w-1,h-1).
uint32_t navi48_scanout_copy_vram(uint64_t srcOff, uint64_t srcLen, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                                  uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                  bool verify) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	const bool logit = gScanoutCopyCalls++ < kScanCopyLogBudget;   // 0.0.278
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		N48ScanoutGeom g {}; IOService *fb = nullptr; uint64_t fbOff = 0;
		N48ScanoutPlan plan {};
		st = scanout_geometry(dev, &g, &fb);
		if (st == kScanStOk && n48_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = n48_scanout_plan_rows(&g, srcOff, srcLen, srcW, srcH, srcStride, 0, 0, dstX, dstY, w, h,
			                                          kScanCopyMaxRows, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) {
			uint64_t *srcMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint64_t *dstMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint32_t *nb = static_cast<uint32_t *>(IOMalloc(plan.h * sizeof(uint32_t)));
			if (!srcMc || !dstMc || !nb) st = kScanStAlloc;
			for (uint32_t i = 0; st == kScanStOk && i < plan.h; i++) {
				const uint64_t d = n48_scanout_row_dst(&plan, g.rowBytes, i);
				if (!n48_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; break; }
				srcMc[i] = gBringup.gmc.vram_start + n48_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0, chunks = 0;
			if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
			if (st == kScanStOk)
				st = scanout_sdma_copies_chunked(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11E00u, 1000, &us, &last, &chunks);
			v[4] = us;
			if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
				N48LOG("scanout-copy: surface VRAM %#llx (+%#llx) %ux%u stride %u -> rect (%u,%u) %ux%u: %u packet(s) in %u "
				       "submission(s), first MC %#llx -> %#llx; status %u, last fence %#010x, %llu us waited (copy call #%u)",
				       (unsigned long long)srcOff, (unsigned long long)srcLen, srcW, srcH, srcStride, plan.dstX, plan.dstY,
				       plan.w, plan.h, plan.h, chunks, (unsigned long long)(srcMc ? srcMc[0] : 0),
				       (unsigned long long)(dstMc ? dstMc[0] : 0), st, last, (unsigned long long)us, gScanoutCopyCalls);
			if (srcMc) IOFree(srcMc, plan.h * sizeof(uint64_t));
			if (dstMc) IOFree(dstMc, plan.h * sizeof(uint64_t));
			if (nb) IOFree(nb, plan.h * sizeof(uint32_t));
		}
		// 0.0.288: the pipe shim's present asks for the copy WITHOUT this readback (a BAR0 read is ~0.57 MiB/s,).
		// The copy above - plan, per-row n48_scanout_row_dst_ok, fence - is identical either way; only the comparison
		// after it is skipped, and an unverified copy does NOT count towards gScanoutCopies.
		if (st == kScanStOk && !verify) {
			v[5] = 0; v[6] = 0; v[7] = 0;
		} else if (st == kScanStOk) {
			// Rows 0 and h-1 in full, 64 dwords per source read.
			const bool srcInBar = srcOff + srcLen <= dev.bar0Size;
			uint32_t bad = 0, compared = 0, clientOk = 0, samples = 0;
			const uint32_t rows[2] = { 0, plan.h - 1 };
			for (unsigned r = 0; r < 2; r++) {
				uint64_t so = n48_scanout_row_src(&plan, rows[r]);
				const uint64_t dor = n48_scanout_row_dst(&plan, g.rowBytes, rows[r]);
				for (uint32_t x = 0; x < plan.w; x += 64) {
					uint32_t chunk[64] = { 0 };
					const uint32_t nw = plan.w - x < 64 ? plan.w - x : 64;
					bool okr = true;
					if (srcInBar) { for (uint32_t i = 0; i < nw; i++) chunk[i] = amdgpu::RBAR0_32(dev, so + (uint64_t)(x + i) * 4u); }
					else okr = navi48_vram_read_mm(so + (uint64_t)x * 4u, chunk, nw);
					for (uint32_t i = 0; i < nw; i++) {
						compared++;
						if (!okr || amdgpu::RBAR0_32(dev, dor + (uint64_t)(x + i) * 4u) != chunk[i]) bad++;
					}
				}
				(void)so;
			}
			const uint32_t px[3] = { 0, plan.w / 2, plan.w - 1 }, py[3] = { 0, plan.h / 2, plan.h - 1 };
			for (unsigned i = 0; i < 3; i++) {
				const uint32_t d = amdgpu::RBAR0_32(dev, n48_scanout_row_dst(&plan, g.rowBytes, py[i]) + (uint64_t)px[i] * 4u);
				uint32_t sv = 0;
				const uint64_t so = n48_scanout_row_src(&plan, py[i]) + (uint64_t)px[i] * 4u;
				if (srcInBar) sv = amdgpu::RBAR0_32(dev, so); else navi48_vram_read_mm(so, &sv, 1);
				v[8 + i] = ((uint64_t)d << 32) | sv;
				samples++;
				if (sv == n48_client_pixel(px[i], py[i])) clientOk++;
			}
			v[5] = bad; v[6] = compared; v[7] = ((uint64_t)clientOk << 32) | samples;
			if (logit || bad)
			N48LOG("scanout-copy: READBACK rows 0 and %u, %u pixel(s) compared destination (BAR0) against source (%s): "
			       "%u differ%s; samples dst/src (0,0) %#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x; "
			       "the source holds the test client's pattern at %u of %u samples", plan.h - 1, compared,
			       srcInBar ? "BAR0" : "MM window", bad, bad ? " - FAILED" : " - ROWS MATCH",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9],
			       px[2], py[2], (uint32_t)(v[10] >> 32), (uint32_t)v[10], clientOk, samples);
			if (bad) st = kScanStReadback; else gScanoutCopies++;
			// 0.0.412: the 16x16 cell map over the destination, source read linearly (this path's
			// own addressing). The linear copy is byte-faithful, so a uniform source reads uniform here whatever the
			// source's true layout - which is exactly what makes S1's forced-linear result decidable.
			{
				N48VerifySource vs;
				vs.off = plan.srcOff; vs.stride = plan.srcStride; vs.surfW = 0; vs.srcX = plan.srcX; vs.srcY = plan.srcY;
				vs.tiled = 0;
				n48_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, g.rowBytes / 4u, vs);
			}
			// 0.0.278: WHAT the frame holds (read from the scanout, which the readback just matched to the source):
			// non-black pixels and value changes in rows 0, h/2 and h-1.
			if (logit && !bad) {
				const uint32_t rowsC[3] = { 0, plan.h / 2, plan.h - 1 };
				uint32_t nz[3] = { 0 }, ch3[3] = { 0 };
				for (unsigned r = 0; r < 3; r++) {
					const uint64_t dor = n48_scanout_row_dst(&plan, g.rowBytes, rowsC[r]);
					uint32_t prev = amdgpu::RBAR0_32(dev, dor);
					for (uint32_t x = 0; x < plan.w; x++) {
						const uint32_t pxv = amdgpu::RBAR0_32(dev, dor + (uint64_t)x * 4u);
						if (pxv & 0x00ffffffu) nz[r]++;
						if (pxv != prev) { ch3[r]++; prev = pxv; }
					}
				}
				N48LOG("scanout-copy: FRAME CONTENT (scanout after the copy): rows 0/%u/%u non-black pixels %u/%u/%u of %u, value "
				       "changes along the row %u/%u/%u", plan.h / 2, plan.h - 1, nz[0], nz[1], nz[2], plan.w, ch3[0], ch3[1], ch3[2]);
			}
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// 0.0.345 — THE TILED SURFACE COPY, the shim's present path. Same contract as navi48_scanout_copy_vram
// above (same interlock, same lock, same geometry, same fence), but ONE COPY_TILED_SUB_WINDOW packet for the whole
// frame instead of one COPY_LINEAR per row, because CONFIRMED the source is tiled and no row address into it is
// meaningful. `swizzle` is the GFX12 ADDR3 enum, `surfW`/`surfH` are Apple's CB_COLOR0_ATTRIB2 MIP0_WIDTH/HEIGHT + 1 as
// the shim read them out of the resource - both come from the caller, neither is guessed here.
// out[0] status, [1] plan reason, [2] srcOff, [3] rect, [4] fence us, [5] readback mismatches, [6] pixels compared,
// [7] swizzle | surfW<<16 | surfH<<32, [8..10] samples dst<<32|src at (0,0), (w/2,h/2), (w-1,h-1).
// 0.0.416 (an internal design note, G3): `gcr` prepends the five-dword GCR_REQ to the SAME submission as the
// tiled copy (and so grows the ring traffic from 18 to 23 dwords). false is the 0.0.415 behaviour, byte for byte.
uint32_t navi48_scanout_copy_tiled(uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                   uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                   bool verify, bool gcr) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	v[7] = (uint64_t)swizzle | ((uint64_t)surfW << 16) | ((uint64_t)surfH << 32);
	const bool logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		N48ScanoutGeom g {}; IOService *fb = nullptr; uint64_t fbOff = 0;
		N48ScanoutTiledPlan plan {};
		st = scanout_geometry(dev, &g, &fb);
		if (st == kScanStOk && n48_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = n48_scanout_plan_tiled(&g, srcOff, srcLen, surfW, surfH, swizzle, 0, 0,
			                                           dstX, dstY, w, h, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		// The emission-time check, repeated here exactly as the row path repeats n48_scanout_row_dst_ok per packet:
		// every byte this packet may write must lie inside [fbOff, fbOff + fbLen).
		if (st == kScanStOk && !n48_scanout_tiled_dst_ok(&plan, g.fbLen)) st = kScanStPlan;
		uint64_t us = 0; uint32_t last = 0;
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk)
			st = scanout_sdma_tiled_copy(dev, inst, &plan, gBringup.gmc.vram_start + plan.srcOff,
			                             gBringup.gmc.vram_start + plan.fbOff, true, 0x5CA11E80u, 1000, &us, &last, gcr);
		v[4] = us;
		if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
			N48LOG("scanout-copy: TILED surface VRAM %#llx (+%#llx) %ux%u swizzle %u -> rect (%u,%u) %ux%u: ONE 14-dword "
			       "COPY_TILED_SUB_WINDOW (detile 1, dcc 0, CPV 0) + FENCE%s, %u dwords in all, tiled MC %#llx -> linear MC "
			       "%#llx pitch %u; status %u, last fence %#010x, %llu us waited (copy call #%u)", (unsigned long long)srcOff,
			       (unsigned long long)srcLen, surfW, surfH, swizzle, plan.dstX, plan.dstY, plan.w, plan.h,
			       gcr ? " + GCR_REQ (GL2 WB+INV) before it in the same submission" : "",
			       (unsigned)n48_sdma_tiled_copy_dwords(gcr),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.srcOff),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.fbOff), plan.linPitch, st, last,
			       (unsigned long long)us, gScanoutCopyCalls);
		if (st == kScanStOk && verify) {
			// Rows 0 and h-1 of the DESTINATION against the TILED source read through the same address equation.
			// Note what this can and cannot settle: it reads a LIVE source AFTER the copy, so a difference is
			// "the copy is wrong" OR "the source was re-rendered in between" ( left exactly that unresolved).
			uint32_t bad = 0, compared = 0;
			const uint32_t rows[2] = { 0, plan.h - 1u };
			for (unsigned r = 0; r < 2; r++)
				for (uint32_t x = 0; x < plan.w; x++) {
					uint32_t sv = 0;
					const uint64_t so = plan.srcOff + n48_addr3_64kb_2d_off_4bpe(x + plan.srcX, rows[r] + plan.srcY, plan.surfW);
					const uint64_t d = plan.fbOff + ((uint64_t)(plan.dstY + rows[r]) * plan.linPitch + plan.dstX + x) * 4u;
					if (!navi48_vram_read_mm(so, &sv, 1)) { bad++; compared++; continue; }
					compared++;
					if (amdgpu::RBAR0_32(dev, d) != sv) bad++;
				}
			const uint32_t px[3] = { 0, plan.w / 2u, plan.w - 1u }, py[3] = { 0, plan.h / 2u, plan.h - 1u };
			for (unsigned i = 0; i < 3; i++) {
				uint32_t sv = 0;
				const uint64_t so = plan.srcOff + n48_addr3_64kb_2d_off_4bpe(px[i] + plan.srcX, py[i] + plan.srcY, plan.surfW);
				const uint64_t d = plan.fbOff + ((uint64_t)(plan.dstY + py[i]) * plan.linPitch + plan.dstX + px[i]) * 4u;
				(void)navi48_vram_read_mm(so, &sv, 1);
				v[8 + i] = ((uint64_t)amdgpu::RBAR0_32(dev, d) << 32) | sv;
			}
			v[5] = bad; v[6] = compared;
			N48LOG("scanout-copy: TILED READBACK rows 0 and %u, %u pixel(s) compared destination (BAR0) against the tiled "
			       "source detiled by our own addrlib equation (MM window): %u differ%s; samples dst/src (0,0) "
			       "%#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x", plan.h - 1u, compared, bad,
			       bad ? " - FAILED" : " - EVERY COMPARED PIXEL MATCHES", (uint32_t)(v[8] >> 32), (uint32_t)v[8],
			       px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9], px[2], py[2], (uint32_t)(v[10] >> 32), (uint32_t)v[10]);
			// Same counter semantics as the row path (0.0.288): an UNVERIFIED copy does not count towards
			// gScanoutCopies, so `verified copies` keeps meaning what its name says.
			if (bad) st = kScanStReadback; else gScanoutCopies++;
			// 0.0.412: the 16x16 cell map over the destination, source detiled by the SAME addrlib
			// equation the copy was built from. This is the map that must reproduce the 1/16 grid on the glass when
			// S1 is off, and read uniform when S1 forces the linear row copy.
			{
				N48VerifySource vs;
				vs.off = plan.srcOff; vs.stride = 0; vs.surfW = plan.surfW; vs.srcX = plan.srcX; vs.srcY = plan.srcY;
				vs.tiled = 1;
				n48_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, plan.linPitch, vs);
			}
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// =====================================================================================================================
// build 0.0.518 — FLIP MODE, switch 74, DEFAULT OFF. The decisions and their ORDER are
// apple/gfx_flipmode.h's, host-tested against a display model (tests/gfx_flipmode_test.cpp). Below is only what the pure code
// is given: the I/O (n48_fm_ops over the bound n48dcn device and the SDMA copies), the engage, the verb, the unarmed A/B test.
// Lock order: gFmLock, then gScanoutLock (taken inside the copies). Nothing here holds gScanoutLock while it waits on the display.

static uint32_t gFmLogs { 0 };                    // present-side event lines (flips, holds, OFF), budgeted
static char     gFmTestLine[4][256];              // the A/B test's four per-flip lines (under gFmLock; kept off the stack)
// build 0.0.519: the A/B test's CRC witness and fill tallies (under gFmLock; kept off the stack).
static struct { uint32_t on, a, b0, b1, aOk, bOk, diff, prev, n, aVar, fillMin, fillMax, fills, ll[4]; } gFmTestW;
static constexpr uint32_t kFmLogBudget = 24;
static uint64_t fm_now_us() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000u; }

// One present's detile INTO A OR B: navi48_scanout_copy_tiled's contract (interlock, gScanoutLock, geometry, the
// COPY_TILED_SUB_WINDOW packet, fence) with the destination a parameter. n48_scanout_plan_tiled_to admits exactly the console
// (A) or flip mode's registered B, and the emission check repeats it over every byte the packet may write. `verify` runs S2's
// 16x16 cell map on the copy's OWN destination (the buffer about to become the front).
static uint32_t fm_copy_tiled_to(uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                 uint32_t w, uint32_t h, uint64_t dstOff, uint64_t *out, unsigned count, bool verify, bool gcr) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	v[2] = srcOff;
	v[7] = (uint64_t)swizzle | ((uint64_t)surfW << 16) | ((uint64_t)surfH << 32);
	const bool logit = gScanoutCopyCalls++ < kScanCopyLogBudget;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (!n48_fm_dst_ok(&gFm, dstOff)) { v[0] = kScanStPlan; goto fin; }
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		N48ScanoutGeom g {}; IOService *fb = nullptr; uint64_t dstLen = 0;
		N48ScanoutTiledPlan plan {};
		st = scanout_geometry(dev, &g, &fb);
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk) {
			const uint32_t pr = n48_scanout_plan_tiled_to(&g, gFm.bOff, gFm.len, dstOff, srcOff, srcLen, surfW, surfH, swizzle,
			                                              0, 0, 0, 0, w, h, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr) st = kScanStPlan;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		// The emission-time check: the destination is still exactly A or B, and every byte the packet may write lies in it.
		if (st == kScanStOk && (plan.fbOff != dstOff || n48_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, plan.fbOff, &dstLen) ||
		                        !n48_scanout_tiled_dst_ok(&plan, dstLen)))
			st = kScanStPlan;
		uint64_t us = 0; uint32_t last = 0;
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk)
			st = scanout_sdma_tiled_copy(dev, inst, &plan, gBringup.gmc.vram_start + plan.srcOff,
			                             gBringup.gmc.vram_start + plan.fbOff, true, 0x5CA11E90u, 1000, &us, &last, gcr);
		v[4] = us;
		if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
			N48LOG("flipmode74: TILED surface VRAM %#llx (+%#llx) %ux%u swizzle %u -> %s (vram+%#llx) rect %ux%u: ONE "
			       "COPY_TILED_SUB_WINDOW + FENCE%s, tiled MC %#llx -> linear MC %#llx pitch %u; status %u, fence %#010x, %llu us",
			       (unsigned long long)srcOff, (unsigned long long)srcLen, surfW, surfH, swizzle,
			       dstOff == gFm.bOff ? "B" : "A", (unsigned long long)dstOff, plan.w, plan.h, gcr ? " + GCR_REQ" : "",
			       (unsigned long long)(gBringup.gmc.vram_start + plan.srcOff),
			       (unsigned long long)(gBringup.gmc.vram_start + plan.fbOff), plan.linPitch, st, last, (unsigned long long)us);
		if (st == kScanStOk && verify) {
			N48VerifySource vs;
			vs.off = plan.srcOff; vs.stride = 0; vs.surfW = plan.surfW; vs.srcX = plan.srcX; vs.srcY = plan.srcY; vs.tiled = 1;
			n48_scanout_verify_cells(dev, plan.fbOff, plan.dstX, plan.dstY, plan.w, plan.h, plan.linPitch, vs);
		}
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

// A whole-buffer COPY_LINEAR between A and B (the restore's B -> A, the engage's A -> B seed): up to 8 packets of 4 MiB and one
// fence on SDMA0 QUEUE0. Source and destination must each be exactly A or B (n48_fm_dst_ok and n48_scanout_flip_dst_check),
// distinct, and every packet's destination range lies inside the destination buffer.
static uint32_t fm_copy_linear(uint64_t srcOff, uint64_t dstOff, uint64_t *usOut) {
	*usOut = 0;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) return kScanStNoContext;
	if (!gScanoutPcPassed) return kScanStInterlock;
	if (!n48_fm_dst_ok(&gFm, dstOff) || !n48_fm_dst_ok(&gFm, srcOff) || srcOff == dstOff) return kScanStPlan;
	uint32_t st = kScanStOk, last = 0;
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		N48ScanoutGeom g {}; IOService *fb = nullptr; uint64_t dstLen = 0, srcLen = 0;
		st = scanout_geometry(dev, &g, &fb);
		if (st == kScanStOk && (n48_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, dstOff, &dstLen) ||
		                        n48_scanout_flip_dst_check(&g, gFm.bOff, gFm.len, srcOff, &srcLen)))
			st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		const uint64_t bytes = gFm.len;
		const uint32_t chunk = amdgpu::kSDMACopyLinearMaxBytes;
		const uint32_t n = (uint32_t)((bytes + chunk - 1u) / chunk);
		if (st == kScanStOk && (bytes == 0 || bytes > dstLen || bytes > srcLen || n == 0 || n > 8u)) st = kScanStPlan;
		uint64_t srcMc[8] = { 0 }, dstMc[8] = { 0 };
		uint32_t nb[8] = { 0 };
		for (uint32_t i = 0; st == kScanStOk && i < n; i++) {
			const uint64_t o = (uint64_t)i * chunk;
			const uint32_t b = (uint32_t)(bytes - o < chunk ? bytes - o : chunk);
			if (!n48_scanout_row_dst_ok(dstOff, dstLen, dstOff + o, b)) { st = kScanStPlan; break; }
			srcMc[i] = gBringup.gmc.vram_start + srcOff + o;
			dstMc[i] = gBringup.gmc.vram_start + dstOff + o;
			nb[i] = b;
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
		if (st == kScanStOk) st = scanout_sdma_copies(dev, inst, srcMc, dstMc, nb, n, 0x5CA11EA0u, 1000, usOut, &last);
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
	}
	IOLockUnlock(gScanoutLock);
	N48LOG("flipmode74: linear copy %s -> %s (vram+%#llx -> vram+%#llx, %llu bytes) status %u, fence %#010x, %llu us",
	       srcOff == gFm.bOff ? "B" : "A", dstOff == gFm.bOff ? "B" : "A", (unsigned long long)srcOff,
	       (unsigned long long)dstOff, (unsigned long long)gFm.len, st, last, (unsigned long long)*usOut);
	return st;
}

// ---- the I/O flip mode's pure code is given --------------------------------------------------------------------------
struct FmPresentArgs {
	uint64_t phys, len; uint32_t surfW, surfH, swz, pw, ph; bool verify, gcr; uint64_t *cv; unsigned cvCount;
};
static int fm_io_read_front(void *, uint32_t *pending, uint64_t *earliest) { return n48dcn::fmReadFront(pending, earliest); }
static int fm_io_read_primary(void *, uint64_t *mc) { return n48dcn::fmReadPrimary(mc); }
static void fm_io_delay(void *, uint32_t us) { IODelay(us); }
static uint32_t fm_io_copy_to(void *c, uint64_t dstOff) {
	const FmPresentArgs *a = static_cast<const FmPresentArgs *>(c);
	if (!a) return kScanStNoContext;
	return fm_copy_tiled_to(a->phys, a->len, a->surfW, a->surfH, a->swz, a->pw, a->ph, dstOff, a->cv, a->cvCount, a->verify,
	                        a->gcr);
}
static uint32_t fm_io_copy_b_to_a(void *) { uint64_t us = 0; return fm_copy_linear(gFm.bOff, gFm.aOff, &us); }
static int fm_io_flip(void *, uint64_t mc) { return n48dcn::fmFlip(mc, "fmflip"); }
static void fm_io_release(void *) { (void)n48dcn::fmSetExact(0u, 0u, 0u); }
// build 0.0.519: may the restore flip to A after an earlier B -> A copy failed or timed out? Only when no
// write into A can still be running: SDMA0 QUEUE0 is idle (its read pointer is at our write pointer, scanout_queue_check) AND
// its fence dword is non-zero (every QUEUE0 submission of ours zeroes it first and its own FENCE packet writes it last, after
// every copy packet before it on the in-order queue). Polled under gScanoutLock for at most 100 ms. 1 = settled. Read-only.
static uint32_t fm_io_a_settled(void *) {
	if (!gBringup.dev || !gScanoutLock) return 0u;
	uint32_t ok = 0u, fence = 0u, rptr = 0u;
	IOLockLock(gScanoutLock);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	for (uint32_t us = 0; us <= 100000u; us += 1000u) {
		fence = amdgpu::sdma_wb_read32(dev, inst, amdgpu::kSDMAWBCSFenceOffset);
		if (scanout_queue_check(dev, inst, &rptr) == kScanStOk && fence != 0u) { ok = 1u; break; }
		IOSleep(1);
	}
	IOLockUnlock(gScanoutLock);
	N48LOG("flipmode74: A-SETTLED check after a failed B->A copy: %s (QUEUE0 rptr %#x, fence dword %#010x)",
	       ok ? "SETTLED - no write into A can still be running" : "*** NOT SETTLED *** - the restore does not flip to A", rptr, fence);
	return ok;
}
static n48_fm_ops fm_ops(void *ctx) {
	n48_fm_ops o { ctx, fm_io_read_front, fm_io_read_primary, fm_io_delay, fm_io_copy_to, fm_io_copy_b_to_a, fm_io_flip,
	               fm_io_release, fm_io_a_settled };
	return o;
}

// ENGAGE: A = the console (RDNA4FB's Console,* geometry, n48_scanout_geom_check), B = one console-shaped allocation
// (gmc.vram_alloc, 64 KiB aligned, allocated ONCE per boot and kept), HUBP0 must be scanning A and be console-shaped, then the
// device's exact flip set becomes {A, B}. `seedB` (the switch; not the A/B test) first copies A into B by SDMA so a partial
// present can never show stale VRAM. 0 = engaged; else the refusal (logged, counted).
static uint32_t fm_engage_locked(bool seedB) {
	if (gFm.engaged) return 0u;
	uint32_t why = 0u, hw = 0u, hh = 0u, hp = 0u, hf = 0u, hs = 0u, pend = 1u;
	uint64_t aOff = 0, prim = 0, early = 0;
	const char *what = "";
	N48ScanoutGeom g {};
	IOService *fb = nullptr;
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock || !gBringupSelf) { why = 1u; what = "no device context"; }
	else if (seedB && !gScanoutPcPassed) { why = 2u; what = "the scanout positive control has not passed this boot"; }
	else if ((void)n48dcn::bind(static_cast<Navi48Bringup *>(gBringupSelf)), !n48dcn::fmArmed()) {   // gBringupSelf = this (start)
		why = 3u; what = "the DCN display layer is not bound";
	}
	else if (n48dcn::fmTestFlipHeld()) { why = 4u; what = "dcnflip's test pattern is on screen"; }
	if (!why) {
		IOLockLock(gScanoutLock);
		if (scanout_geometry(*gBringup.dev, &g, &fb) != kScanStOk || n48_scanout_geom_check(&g, &aOff) != kScanGeomOk ||
		    gBringup.gmc.vram_start != gBringup.dev->vramMcBase) {
			why = 5u; what = "no usable console geometry";
		} else if (n48dcn::fmHubpGeom(&hw, &hh, &hp, &hf, &hs) != 0 || !n48_fm_hubp_ok(hw, hh, hp, hf, hs, g.width, g.height, g.rowBytes)) {
			why = 6u; what = "HUBP0 is not scanning a console-shaped linear 32-bit plane (or OTG0 is not lit)";
		} else {
			const uint64_t len = (uint64_t)g.rowBytes * g.height;
			if (!gFm.haveB) {
				amdgpu::VRAMAllocation b {};
				if (!gBringup.gmc.vram_alloc.alloc(len, 65536, &b) || b.gpu_va < gBringup.gmc.vram_start) {
					if (b.size) gBringup.gmc.vram_alloc.free(b);
					why = 7u; what = "the back buffer could not be allocated";
				} else {
					gFm.bOff = b.gpu_va - gBringup.gmc.vram_start;
					gFm.len = len;
					__atomic_store_n(&gFm.haveB, 1u, __ATOMIC_RELEASE);
					N48LOG("flipmode74: BACK BUFFER B allocated ONCE: MC %#llx = vram+%#llx, %llu bytes (%ux%u, RowBytes %u, the "
					       "console's shape); A (the console) MC %#llx = vram+%#llx", (unsigned long long)b.gpu_va,
					       (unsigned long long)gFm.bOff, (unsigned long long)len, g.width, g.height, g.rowBytes,
					       (unsigned long long)(gBringup.gmc.vram_start + aOff), (unsigned long long)aOff);
				}
			} else if (gFm.len != len) { why = 8u; what = "the console's shape changed since B was allocated"; }
			if (!why) {
				const uint32_t br = n48_fm_buffers_ok(aOff, gFm.bOff, len, gBringup.dev->bar0Size, gBringup.dev->vramBase,
				                                      gBringup.gmc.vram_start);
				if (br) { why = 9u; what = br == 5u ? "a buffer ends above 256 MiB (ADDRESS_HIGH could change)"
				                              : "A/B placement (overlap, alignment, BAR0 or the MC high dword)"; }
			}
			if (!why) {
				gFm.aOff = aOff;
				gFm.aMc = gBringup.gmc.vram_start + aOff;
				gFm.bMc = gBringup.gmc.vram_start + gFm.bOff;
			}
		}
		IOLockUnlock(gScanoutLock);
	}
	if (!why && (n48dcn::fmReadPrimary(&prim) != 0 || prim != gFm.aMc)) { why = 10u; what = "HUBP0's programmed address is not A"; }
	if (!why && (n48dcn::fmSetExact(gFm.aMc, gFm.bMc, 2u) != 0)) { why = 13u; what = "the device refused the exact flip set {A, B}"; }
	if (!why && (n48dcn::fmReadFront(&pend, &early) != 0 || pend || early != gFm.aMc)) {
		why = 11u; what = "the display is not scanning A (a flip is pending or EARLIEST_INUSE is not A)";
	}
	if (!why && seedB) {
		uint64_t us = 0;
		if (fm_copy_linear(gFm.aOff, gFm.bOff, &us) != kScanStOk) { why = 12u; what = "the A -> B seed copy failed"; }
	}
	if (why) {
		if (why >= 11u) (void)n48dcn::fmSetExact(0u, 0u, 0u);
		gFm.engageRefused++;
		N48LOG("flipmode74: ENGAGE REFUSED (%u: %s); HUBP0 %ux%u pitch %u fmt %u SW_MODE %u, programmed %#llx, EARLIEST_INUSE %#llx "
		       "pending %u; A MC %#llx B MC %#llx - flip mode stays OFF", why, what, hw, hh, hp, hf, hs,
		       (unsigned long long)prim, (unsigned long long)early, pend, (unsigned long long)gFm.aMc, (unsigned long long)gFm.bMc);
		return why;
	}
	gFm.bHoldsPresent = seedB ? 1u : 0u;   // 0.0.519 F2: B holds a real picture only when seeded from A
	gFm.consecTimeouts = 0u;               // 0.0.519 F1: a fresh engage starts a fresh timeout run
	__atomic_store_n(&gFm.engaged, 1u, __ATOMIC_RELEASE);
	gFm.engages++;
	gFm.front = early;
	N48LOG("flipmode74: ENGAGED - A MC %#llx, B MC %#llx (vram+%#llx, %llu bytes), HUBP0 %ux%u pitch %u fmt %u linear; the "
	       "device flips ONLY to A or B now (dcn41 exact set)%s", (unsigned long long)gFm.aMc, (unsigned long long)gFm.bMc,
	       (unsigned long long)gFm.bOff, (unsigned long long)gFm.len, hw, hh, hp, hf, seedB ? "; B seeded from A" : "");
	return 0u;
}

static void fm_report(const char *how) {
	N48LOG(N48_FM_REPORT1_FMT, gFm.on ? "ON" : "OFF (default)", how, gFm.engaged, (unsigned long long)gFm.aMc,
	       (unsigned long long)gFm.bMc, (unsigned long long)gFm.bOff, (unsigned long long)gFm.len, (unsigned long long)gFm.front,
	       n48_fm_off_name(gFm.offWhy), __atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE));
	N48LOG(N48_FM_REPORT2_FMT, (unsigned long long)gFm.flips, (unsigned long long)gFm.flipsToB, (unsigned long long)gFm.flipsToA,
	       (unsigned long long)gFm.presents, (unsigned long long)gFm.waits, (unsigned long long)gFm.waitHist[0],
	       (unsigned long long)gFm.waitHist[1], (unsigned long long)gFm.waitHist[2], (unsigned long long)gFm.waitHist[3],
	       (unsigned long long)gFm.waitHist[4], (unsigned long long)gFm.waitHist[5], (unsigned long long)gFm.waitHist[6],
	       gFm.waitMaxUs, (unsigned long long)gFm.timeouts, (unsigned long long)gFm.copyFails);
	N48LOG(N48_FM_REPORT3_FMT, (unsigned long long)gFm.foreign, (unsigned long long)gFm.highChanged,
	       (unsigned long long)gFm.flipFails, (unsigned long long)gFm.readFails, (unsigned long long)gFm.restores,
	       (unsigned long long)gFm.restoreFails, (unsigned long long)gFm.restoreCopies,
	       (unsigned long long)__atomic_load_n(&gFm.restoreRequests, __ATOMIC_RELAXED), (unsigned long long)gFm.engages,
	       (unsigned long long)gFm.engageRefused, (unsigned long long)__atomic_load_n(&gFm.writersHeld, __ATOMIC_RELAXED),
	       (unsigned long long)gFm.testRuns);
	N48LOG(N48_FM_REPORT4_FMT, gFm.consecTimeouts, (unsigned long long)gFm.stuckRestores,   // build 0.0.519
	       (unsigned long long)gFm.timeoutForeign, (unsigned long long)gFm.restoreCopyFails,
	       (unsigned long long)gFm.restoreCopySkipped, (unsigned long long)gFm.restoreUnsettled, gFm.bHoldsPresent,
	       gFm.aCopyPending);
}

static void fm_restore_locked(uint32_t why, uint32_t force) {
	if (force && !gFm.engaged && gFm.haveB && gFm.aMc && gFm.bMc && n48dcn::fmArmed() &&
	    n48dcn::fmSetExact(gFm.aMc, gFm.bMc, 2u) == 0)
		__atomic_store_n(&gFm.engaged, 1u, __ATOMIC_RELEASE);   // the force verb restores even what it did not see engaged
	const uint32_t wasEngaged = gFm.engaged;
	n48_fm_ops o = fm_ops(nullptr);
	const uint32_t ok = n48_fm_restore(&gFm, &o, why, force);
	N48LOG("flipmode74: RESTORE (%s)%s -> %s; front %#llx (A %#llx); restores verified %llu failed %llu; flip mode OFF: %s "
	       "(B->A copy failures %llu, A copy pending %u)",
	       n48_fm_off_name(why), wasEngaged ? "" : " - nothing of ours was engaged", ok ? "A VERIFIED (EARLIEST_INUSE and the "
	       "programmed address)" : "*** NOT VERIFIED ***", (unsigned long long)gFm.front, (unsigned long long)gFm.aMc,
	       (unsigned long long)gFm.restores, (unsigned long long)gFm.restoreFails, n48_fm_off_name(gFm.offWhy),
	       (unsigned long long)gFm.restoreCopyFails, gFm.aCopyPending);
}

uint32_t navi48_fm_on(void) { return __atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE); }

// dpg_perform's question, asked ONLY while navi48_fm_on() and only AFTER switch 73's hold check. 0 = a copy was made (its scanout
// status in *cstOut: flipped, or a failed copy that held the flip); 1 = held, no copy and no flip.
uint32_t navi48_fm_present(uint64_t phys, uint64_t len, uint32_t surfW, uint32_t surfH, uint32_t swz, uint32_t pw, uint32_t ph,
                           bool linear, bool verify, bool gcr, uint64_t presentNo, uint64_t *cv, unsigned cvCount,
                           uint32_t *cstOut) {
	*cstOut = 0u;
	if (!gFmLock) return 1u;
	uint32_t held = 1u;
	IOLockLock(gFmLock);
	if (gFm.on) {
		if (linear || swz != 3u) {
			fm_restore_locked(N48_FM_OFF_NOTTILED, 0u);
		} else {
			FmPresentArgs a { phys, len, surfW, surfH, swz, pw, ph, verify, gcr, cv, cvCount };
			n48_fm_ops o = fm_ops(&a);
			const uint64_t to0 = gFm.timeouts;
			const uint32_t r = n48_fm_present(&gFm, &o, cstOut);
			held = (r == N48_FM_P_FLIPPED || r == N48_FM_P_COPYFAIL) ? 0u : 1u;
			if (gFmLogs < kFmLogBudget || r == N48_FM_P_OFF) {
				gFmLogs++;
				N48LOG("flipmode74: present #%llu (plane VRAM %#llx) -> %s; front was %#llx (%s); copy status %u; flips %llu, "
				       "timeouts %llu%s", (unsigned long long)presentNo, (unsigned long long)phys,
				       r == N48_FM_P_FLIPPED ? "FLIPPED" : r == N48_FM_P_COPYFAIL ? "HELD (copy failed, no flip)"
				       : r == N48_FM_P_HELD ? (gFm.timeouts != to0 ? "HELD (the previous flip did not latch in 2 frames)" : "HELD")
				       : "FLIP MODE OFF", (unsigned long long)gFm.front,
				       gFm.front == gFm.aMc ? "A" : gFm.front == gFm.bMc ? "B" : "FOREIGN", *cstOut,
				       (unsigned long long)gFm.flips, (unsigned long long)gFm.timeouts,
				       r == N48_FM_P_OFF ? " - flip mode is OFF, last reason below" : "");
				if (r == N48_FM_P_OFF) fm_report(" - turned OFF by the present path");
			}
		}
	}
	IOLockUnlock(gFmLock);
	return held;
}

// A token / guard / keystone withdrawal on the submit path: only a flag (no lock, no I/O); the next present or verb restores.
void navi48_fm_note_withdrawal(void) { (void)n48_fm_request_restore(&gFm, N48_FM_OFF_WITHDRAWAL); }

// build 0.0.525 (switch 80): the buffer the last present's copy went into (n48_fm_present copies into the one that is NOT
// `front`, the last EARLIEST_INUSE it read). Read-only and lock-free: presents are one at a time and this is asked right after one.
uint32_t navi48_fm_copy_buf(void) {
	if (!__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) return 0u;
	const uint64_t f = __atomic_load_n(&gFm.front, __ATOMIC_RELAXED);
	return f == gFm.aMc ? 2u : f == gFm.bMc ? 1u : 3u;
}

// build 0.0.538 (switch 95, gfx_p95.h): may a replayed present go through flip mode now? 1 = no: a restore is requested
// (served by the next present, which then holds), a failed restore's B -> A copy is pending (A may still be written), or A/B are
// still engaged with flip mode OFF (the failed restore 842 retries). A flip whose latch is pending is n48_fm_present's own
// bounded wait (it holds without a copy). Read-only and lock-free, like navi48_fm_copy_buf.
uint32_t navi48_fm_replay_blocked(void) {
	if (__atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE)) return 1u;
	if (__atomic_load_n(&gFm.aCopyPending, __ATOMIC_ACQUIRE)) return 1u;
	if (__atomic_load_n(&gFm.engaged, __ATOMIC_ACQUIRE) && !__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) return 1u;
	return 0u;
}

// The commit arm's disarm verb (`gfxneuter 4 | 2 << 8`): back to A, flip mode OFF. Verb context.
void navi48_fm_disarm(void) {
	if (!gFmLock) return;
	IOLockLock(gFmLock);
	if (gFm.on || gFm.engaged) fm_restore_locked(N48_FM_OFF_DISARM, 0u);
	IOLockUnlock(gFmLock);
}

// `gfxneuter 74 | M << 8`: M 1 ON (= 330: engage, then every tiled present flips), M 2 OFF (= 586, the default and the boot
// value: restore to A), M 3 FORCE RESTORE (= 842: copy B into A if B is the front, flip to A and verify, flip mode OFF; allowed
// at ANY time, even mid-arm, like every other way back), bare 74 reads. `contRefused` = the continuous mid-arm guard's answer
// (the caller's; M 1 and M 2 only). Returns the verb status: 0, 5 mid-arm refused, 11 unknown M, 12 the engage refused.
uint32_t navi48_fm_control(uint32_t m, uint32_t contRefused) {
	if (!gFmLock) return 1u;
	uint32_t st = 0u;
	const char *how = " (read only, unchanged)";
	IOLockLock(gFmLock);
	if (m == 0u) {
		if (__atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE)) { fm_restore_locked(gFm.restoreReq, 0u); how = " - a pending restore was served"; }
	} else if (m == 3u) {
		fm_restore_locked(N48_FM_OFF_FORCE, 1u);
		how = " - FORCE RESTORE BY THIS VERB";
	} else if (m != 1u && m != 2u) {
		st = 11u; how = " - REFUSED (unknown M), unchanged";
	} else if (contRefused) {
		st = 5u; how = " - REFUSED: a continuous arm stands, unchanged";
	} else if (m == 1u) {
		if (gFm.on) how = " - already ON";
		else if (gFm.engaged) { st = 12u; how = " - ON REFUSED: a failed restore left A/B engaged (842 retries it), unchanged"; }   // 0.0.520
		else if (fm_engage_locked(true) != 0u) { st = 12u; how = " - ON REFUSED (the engage failed, line above), unchanged"; }
		else { gFm.offWhy = N48_FM_OFF_NONE; __atomic_store_n(&gFm.on, 1u, __ATOMIC_RELEASE); how = " - CHANGED BY THIS VERB"; }
	} else {
		if (gFm.on || gFm.engaged) { fm_restore_locked(N48_FM_OFF_VERB, 0u); how = " - CHANGED BY THIS VERB (restored to A)"; }
		else how = " - already OFF";
	}
	fm_report(how);
	IOLockUnlock(gFmLock);
	return st;
}

// `dcnflip 1000 + N` (N 2..240): flip mode's UNARMED A/B test (PC T-F1). Refused while an arm stands or flip mode is ON.
// Engages without the SDMA seed (B is filled by the CPU through BAR0, as dcnflip's pattern is), then N flips alternating
// B, A, B, ...: before each flip to B it is filled with one of two distinct dark greys (alternating, so every visit to B shows
// NEW content); each flip is VUPDATE-latched and waited for (<= 3 frames). Reported: SURFACE_FLIP_PENDING set on the first read
// after the write, the latch time, EARLIEST_INUSE == the target per flip, OTG0's frame count before/after. Ends on A, verified
// (EARLIEST_INUSE and the programmed address; one more flip to A if not). A is never written.
// out[0] status, [1] N, [2] latched, [3] EARLIEST_INUSE matches, [4] pending-set on the first read, [5] latch us min | max << 32,
// [6] latch us sum, [7] frame count before | after << 32, [8] elapsed us, [9] ended on A verified, [10] A MC, [11] B MC,
// [12] fill us total.
uint32_t navi48_fm_test(uint32_t n, uint64_t *out, unsigned count) {
	uint64_t v[13] = { 0 };
	uint32_t st = 0u;
	v[1] = n;
	if (n < N48_FM_TEST_MIN || n > N48_FM_TEST_MAX) st = 1u;
	else if (n48::hw_cm_armed()) st = 2u;
	else if (!gFmLock) st = 3u;
	if (st) {
		N48LOG("fmtest: A/B test of %u flip(s) REFUSED (%u: %s)", n, st,
		       st == 1u ? "N outside 2..240" : st == 2u ? "a commit arm stands - the test is unarmed only" : "no lock");
	} else {
		IOLockLock(gFmLock);
		uint32_t r = 0u;
		if (gFm.on) { st = 4u; N48LOG("fmtest: REFUSED - flip mode (switch 74) is ON; turn it OFF first"); }
		else if (gFm.engaged) { st = 7u; N48LOG("fmtest: REFUSED - a failed restore left A/B engaged; `gfxneuter 842` first"); }   // 0.0.520
		else if ((r = fm_engage_locked(false)) != 0u) { st = 0x10u | r; }
		else {
			gFm.testRuns++;
			n48_fm_ops o = fm_ops(nullptr);
			amdgpu::DeviceContext &dev = *gBringup.dev;
			uint32_t fc0 = 0, fc1 = 0, latched = 0, match = 0, pset = 0, lmin = 0xffffffffu, lmax = 0;
			uint64_t lsum = 0, fillUs = 0;
			// build 0.0.519: the witness nobody has to watch - OTG0's CRC read after each flip has
			// latched and a frame of the new buffer has been scanned (2 frames), and the fill time per visit to B.
			auto &W = gFmTestW;
			memset(&W, 0, sizeof(W));
			W.fillMin = 0xffffffffu;
			char (&line)[4][256] = gFmTestLine;
			line[0][0] = 0; line[1][0] = 0; line[2][0] = 0; line[3][0] = 0;
			gFm.bHoldsPresent = 0u;   // 0.0.519 F2: B is about to hold test fills, never a present
			(void)n48dcn::fmFrameCount(&fc0);
			W.on = n48dcn::fmCrcBegin() == 0 ? 1u : 0u;
			const uint64_t t0 = fm_now_us();
			for (uint32_t i = 0; i < n && st == 0u; i++) {
				const bool toB = (i & 1u) == 0u;
				const uint64_t target = toB ? gFm.bMc : gFm.aMc;
				uint32_t fillThis = 0u;
				if (toB) {
					const uint32_t fill = ((i >> 1) & 1u) ? 0xFF101018u : 0xFF101010u;
					const uint64_t f0 = fm_now_us();
					volatile uint32_t *px = reinterpret_cast<volatile uint32_t *>(dev.bar0 + gFm.bOff);
					for (uint64_t k = 0; k < gFm.len / 4u; k++) px[k] = fill;
					amdgpu::storeFence();
					amdgpu::amdgpu_hdp_flush(dev);   // 0.0.519 F4: as fc_positive_control, after the CPU fill
					fillThis = (uint32_t)(fm_now_us() - f0);
					fillUs += fillThis; W.fills++;
					if (fillThis < W.fillMin) W.fillMin = fillThis;
					if (fillThis > W.fillMax) W.fillMax = fillThis;
				}
				if (!n48_fm_flip_ok(&gFm, target) || n48dcn::fmFlip(target, "fmtest") != 0) { st = 5u; break; }
				uint32_t pend = 0, us = 0, polls = 0; uint64_t e = 0;
				if (o.read_front(o.ctx, &pend, &e) == 0 && pend) pset++;
				const uint32_t w = n48_fm_wait_latch(&o, N48_FM_RESTORE_WAIT_US, &e, &us, &polls);
				if (w == N48_FM_W_LATCHED) { latched++; lsum += us; if (us < lmin) lmin = us; if (us > lmax) lmax = us; }
				if (e == target) match++;
				if (toB) gFm.flipsToB++; else gFm.flipsToA++;
				uint32_t crg = 0u, cb = 0u, crc = 0u;
				if (W.on && n48dcn::fmCrcRead(2u, &crg, &cb) == 0) {
					crc = crg ^ (cb * 0x9E3779B1u);   // one word per frame (R/G and B both enter it: the greys differ in B only)
					W.n++;
					if (W.n > 1u && crc != W.prev) W.diff++;
					W.prev = crc;
					if (!toB) { if (!W.aOk) { W.a = crc; W.aOk = 1u; } else if (crc != W.a) W.aVar++; }
					else if (((i >> 1) & 1u) == 0u) { if (!(W.bOk & 1u)) W.b0 = crc; W.bOk |= 1u; }
					else { if (!(W.bOk & 2u)) W.b1 = crc; W.bOk |= 2u; }
				}
				if (i < 16u) {
					char *L = line[i / 4u];
					uint32_t &at = W.ll[i / 4u];
					const int k = snprintf(L + at, sizeof(line[0]) - at, " %c%s/%uus/fill %uus/crc %08x", toB ? 'B' : 'A',
					                       w == N48_FM_W_LATCHED ? (e == target ? "" : "!inuse") : "!late", us, fillThis, crc);
					if (k > 0 && at + (uint32_t)k < sizeof(line[0])) at += (uint32_t)k;
				}
			}
			if (W.on) n48dcn::fmCrcEnd();
			// End on A, verified - unconditionally, like dcnflip's restore.
			uint32_t endOk = 0u;
			for (uint32_t attempt = 0; attempt < 2u && !endOk; attempt++) {
				uint64_t e = 0, prim = 0; uint32_t us = 0, polls = 0;
				if (n48dcn::fmFlip(gFm.aMc, "fmtestend") != 0) continue;
				(void)n48_fm_wait_latch(&o, N48_FM_RESTORE_WAIT_US, &e, &us, &polls);
				endOk = (e == gFm.aMc && n48dcn::fmReadPrimary(&prim) == 0 && prim == gFm.aMc) ? 1u : 0u;
				gFm.front = e;
			}
			const uint64_t el = fm_now_us() - t0;
			(void)n48dcn::fmFrameCount(&fc1);
			(void)n48dcn::fmSetExact(0u, 0u, 0u);
			__atomic_store_n(&gFm.engaged, 0u, __ATOMIC_RELEASE);
			if (!endOk && st == 0u) st = 6u;
			uint64_t refused = 0;
			const uint64_t allowed = n48dcn::fmAllowCounts(&refused);
			N48LOG("fmtest: A/B test %u flip(s): latched %u, EARLIEST_INUSE == target %u, SURFACE_FLIP_PENDING set on the first "
			       "read %u; latch us min %u avg %llu max %u; OTG0 frame count %u -> %u in %llu us; B fills %llu us; ended on A %s "
			       "(front %#llx); status %u; DCN allowlist allowed %llu refused %llu", n, latched, match, pset,
			       lmin == 0xffffffffu ? 0u : lmin, latched ? (unsigned long long)(lsum / latched) : 0ull, lmax, fc0, fc1,
			       (unsigned long long)el, (unsigned long long)fillUs, endOk ? "VERIFIED" : "*** NOT VERIFIED ***",
			       (unsigned long long)gFm.front, st, (unsigned long long)allowed, (unsigned long long)refused);
			// build 0.0.519: the CRC witness and the fill time per visit.
			N48LOG("fmtest: CRC WITNESS %s: %u read(s); A %08x (A visits that differ from it %u); B grey-1 %08x, B grey-2 %08x; the CRC "
			       "changed on %u of %u consecutive reads (ALTERNATION %s); B fill per visit min %u us avg %llu us max %u us over %u "
			       "visit(s)", W.on ? "ON (OTG0 CRC0 over the active raster, 2 frames after each latch)" : "NOT ENABLED (OTG0 not lit)",
			       W.n, W.a, W.aVar, W.b0, W.b1, W.diff, W.n ? W.n - 1u : 0u,
			       (W.n > 1u && W.diff == W.n - 1u && W.aVar == 0u && W.aOk && (W.bOk & 1u) && W.b0 != W.a) ? "SEEN" : "NOT SEEN",
			       W.fillMin == 0xffffffffu ? 0u : W.fillMin, W.fills ? (unsigned long long)(fillUs / W.fills) : 0ull, W.fillMax, W.fills);
			for (uint32_t q = 0; q < 4u && q * 4u < n; q++) N48LOG("fmtest: flips %u-%u:%s", q * 4u + 1u, q * 4u + 4u, line[q]);
			v[2] = latched; v[3] = match; v[4] = pset; v[5] = (uint64_t)(lmin == 0xffffffffu ? 0u : lmin) | ((uint64_t)lmax << 32);
			v[6] = lsum; v[7] = (uint64_t)fc0 | ((uint64_t)fc1 << 32); v[8] = el; v[9] = endOk; v[10] = gFm.aMc;
			v[11] = gFm.bMc; v[12] = fillUs;
		}
		IOLockUnlock(gFmLock);
	}
	v[0] = st;
	if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
	return st;
}

// 0.0.273: the STAGED surface copy. flush2 measured the test client's flushed buffer as a VidMemory with NO
// VRAM placement (getPhysicalSegment 0/0) while the client's pixels sat in the resource's system-memory backing
// (IOAccelResource2 +0x80, the IOAccelSysMemory lockForCPUAccess maps, 0x145a81ef/0x145a81fd). The GPU cannot reach that
// memory from VMID 0, so the rows are staged by the CPU into a BAR0-pool VRAM buffer (never the scanout) and copied into the
// scanout by SDMA0 QUEUE0 through the same checked plan, fence and readback as the VRAM path. Every scanout write is still
// an SDMA packet whose destination passed n48_scanout_row_dst_ok.
// out as navi48_scanout_copy_vram, plus [2] the staging VRAM offset and status 17 (backing not prepared / too short).
uint32_t navi48_scanout_copy_staged(IOMemoryDescriptor *md, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                                    uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count) {
	uint64_t v[11] = { 0 };
	uint32_t st = kScanStOk;
	amdgpu::VRAMAllocation stg {};
	uint32_t *row = nullptr;
	bool prepared = false;
	const bool logit = gScanoutCopyCalls++ < kScanCopyLogBudget;   // 0.0.278
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock) { v[0] = kScanStNoContext; goto fin; }
	if (!gScanoutPcPassed) { v[0] = kScanStInterlock; gScanoutRefusals++; gScanoutLastReason = kScanStInterlock; goto fin; }
	if (__atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE)) { v[0] = kScanStFlipMode; __atomic_fetch_add(&gFm.writersHeld, 1ull, __ATOMIC_RELAXED); goto fin; }   // 0.0.518: flip mode owns A/B
	IOLockLock(gScanoutLock);
	{
		amdgpu::DeviceContext &dev = *gBringup.dev;
		N48ScanoutGeom g {}; IOService *fb = nullptr; uint64_t fbOff = 0;
		N48ScanoutPlan plan {};
		const uint32_t cw = w < srcW ? w : srcW, ch = h < srcH ? h : srcH;
		const uint64_t need = (uint64_t)(srcH - 1) * srcStride + (uint64_t)srcW * 4u;
		st = scanout_geometry(dev, &g, &fb);
		if (st == kScanStOk && n48_scanout_geom_check(&g, &fbOff)) st = kScanStGeom;
		if (st == kScanStOk && gBringup.gmc.vram_start != dev.vramMcBase) st = kScanStMcBase;
		if (st == kScanStOk && (!md || md->getLength() < need || cw == 0 || ch == 0 || srcStride < srcW * 4u))
			st = 17;
		// 0.0.274: flush3 measured the backing as an IOBufferMemoryDescriptor with preparationID 0 (not wired).
		// The IOKit contract for reading such memory is prepare() first and complete() after, balanced, from thread
		// context; readBytes walks physical segments that exist only while it is prepared.
		if (st == kScanStOk) {
			const IOReturn pr = md->prepare();
			prepared = (pr == kIOReturnSuccess);
			if (logit || !prepared)
				N48LOG("scanout-staged: backing %p prepare() -> %#x, preparationID now %#llx", md, pr,
				       (unsigned long long)md->getPreparationID());
			if (!prepared) st = 17;
		}
		const uint32_t bytes = cw * ch * 4u;
		if (st == kScanStOk && (!gBringup.gmc.vram_alloc.alloc(bytes, 16384, &stg) ||
		                        stg.gpu_va - gBringup.gmc.vram_start + bytes > dev.bar0Size))
			st = kScanStAlloc;
		const uint64_t stgOff = stg.size ? stg.gpu_va - gBringup.gmc.vram_start : 0;
		v[2] = stgOff;
		if (st == kScanStOk) {
			const uint32_t pr = n48_scanout_plan_rows(&g, stgOff, bytes, cw, ch, cw * 4u, 0, 0, dstX, dstY, cw, ch,
			                                          kScanCopyMaxRows, &plan);
			v[1] = pr;
			v[3] = ((uint64_t)plan.dstX << 48) | ((uint64_t)plan.dstY << 32) | ((uint64_t)plan.w << 16) | plan.h;
			if (pr || plan.w != cw || plan.h != ch) st = kScanStPlan;
		}
		// Stage: read each source row from the backing and write it to the staging buffer through BAR0.
		if (st == kScanStOk) {
			row = static_cast<uint32_t *>(IOMalloc(cw * sizeof(uint32_t)));
			if (!row) st = kScanStAlloc;
			for (uint32_t y = 0; st == kScanStOk && y < ch; y++) {
				if (md->readBytes((IOByteCount)y * srcStride, row, cw * 4u) != cw * 4u) { st = 17; break; }
				amdgpu::bar0_memcpy_to_vram(dev, stgOff + (uint64_t)y * cw * 4u, row, cw * 4u);
			}
			amdgpu::amdgpu_hdp_flush(dev);
			// Source control: the staging buffer reads back as the backing's first and last rows.
			if (st == kScanStOk) {
				uint32_t bad = 0;
				const uint32_t rows2[2] = { 0, ch - 1 };
				for (unsigned r = 0; r < 2; r++) {
					md->readBytes((IOByteCount)rows2[r] * srcStride, row, cw * 4u);
					for (uint32_t x = 0; x < cw; x++)
						if (amdgpu::RBAR0_32(dev, stgOff + ((uint64_t)rows2[r] * cw + x) * 4u) != row[x]) bad++;
				}
				if (logit || bad)
					N48LOG("scanout-staged: backing %p (%s, length %#llx) %ux%u stride %u -> staging VRAM %#llx (MC %#llx) %ux%u: "
					       "staged rows 0 and %u read back with %u mismatch(es)", md, md->getMetaClass()->getClassName(),
					       (unsigned long long)md->getLength(), srcW, srcH, srcStride, (unsigned long long)stgOff,
					       (unsigned long long)stg.gpu_va, cw, ch, ch - 1, bad);
				if (bad) st = kScanStSourceCtl;
				// 0.0.278: WHAT the frame holds, so the report can say what the screen shows - non-zero pixels and
				// distinct values in five rows, and the value at the centre and at four inset corners.
				if (logit && st == kScanStOk) {
					const uint32_t rowsC[5] = { 0, ch / 4, ch / 2, (3 * ch) / 4, ch - 1 };
					uint32_t nz[5] = { 0 }, first[5] = { 0 }, distinctish[5] = { 0 };
					for (unsigned r = 0; r < 5; r++) {
						md->readBytes((IOByteCount)rowsC[r] * srcStride, row, cw * 4u);
						first[r] = row[0];
						uint32_t prev = row[0];
						for (uint32_t x = 0; x < cw; x++) {
							if (row[x] & 0x00ffffffu) nz[r]++;
							if (row[x] != prev) { distinctish[r]++; prev = row[x]; }
						}
					}
					const uint32_t sx[5] = { cw / 2, 16, cw - 17, 16, cw - 17 }, sy[5] = { ch / 2, 16, 16, ch - 17, ch - 17 };
					uint32_t sv[5] = { 0 };
					for (unsigned i = 0; i < 5; i++) md->readBytes((IOByteCount)sy[i] * srcStride + (IOByteCount)sx[i] * 4u, &sv[i], 4);
					N48LOG("scanout-staged: FRAME CONTENT (backing, BGRA little-endian dwords): rows 0/%u/%u/%u/%u non-black pixels "
					       "%u/%u/%u/%u/%u of %u, value changes along the row %u/%u/%u/%u/%u, first pixel %#010x/%#010x/%#010x/%#010x/%#010x; "
					       "centre %#010x, inset corners TL %#010x TR %#010x BL %#010x BR %#010x", ch / 4, ch / 2, (3 * ch) / 4, ch - 1,
					       nz[0], nz[1], nz[2], nz[3], nz[4], cw, distinctish[0], distinctish[1], distinctish[2], distinctish[3],
					       distinctish[4], first[0], first[1], first[2], first[3], first[4], sv[0], sv[1], sv[2], sv[3], sv[4]);
				}
			}
		}
		amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
		uint32_t rptr = 0;
		if (st == kScanStOk) st = scanout_queue_check(dev, inst, &rptr);
		if (st == kScanStOk) {
			uint64_t *srcMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint64_t *dstMc = static_cast<uint64_t *>(IOMalloc(plan.h * sizeof(uint64_t)));
			uint32_t *nb = static_cast<uint32_t *>(IOMalloc(plan.h * sizeof(uint32_t)));
			if (!srcMc || !dstMc || !nb) st = kScanStAlloc;
			for (uint32_t i = 0; st == kScanStOk && i < plan.h; i++) {
				const uint64_t d = n48_scanout_row_dst(&plan, g.rowBytes, i);
				if (!n48_scanout_row_dst_ok(fbOff, g.fbLen, d, plan.rowBytes)) { st = kScanStPlan; break; }
				srcMc[i] = gBringup.gmc.vram_start + n48_scanout_row_src(&plan, i);
				dstMc[i] = gBringup.gmc.vram_start + d;
				nb[i] = plan.rowBytes;
			}
			uint64_t us = 0; uint32_t last = 0, chunks = 0;
			if (st == kScanStOk) __atomic_fetch_add(&gScanoutWrites, 1u, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: at the write
			if (st == kScanStOk)
				st = scanout_sdma_copies_chunked(dev, inst, srcMc, dstMc, nb, plan.h, 0x5CA11F00u, 1000, &us, &last, &chunks);
			v[4] = us;
			if (logit || (st != kScanStOk && gScanoutCopyFailLogs++ < kScanCopyFailLogBudget))
				N48LOG("scanout-staged: SDMA copy %u rows x %u bytes in %u submission(s), staging MC %#llx.. -> scanout MC %#llx.. "
				       "(rect %u,%u %ux%u): status %u, last fence %#010x, %llu us waited (copy call #%u)", plan.h, plan.rowBytes,
				       chunks, (unsigned long long)(srcMc ? srcMc[0] : 0), (unsigned long long)(dstMc ? dstMc[0] : 0), plan.dstX,
				       plan.dstY, plan.w, plan.h, st, last, (unsigned long long)us, gScanoutCopyCalls);
			if (srcMc) IOFree(srcMc, plan.h * sizeof(uint64_t));
			if (dstMc) IOFree(dstMc, plan.h * sizeof(uint64_t));
			if (nb) IOFree(nb, plan.h * sizeof(uint32_t));
		}
		// Readback: the scanout rows 0 and h-1 against the BACKING (the client's own bytes), and nine samples against the
		// client's pattern.
		if (st == kScanStOk) {
			uint32_t bad = 0, compared = 0, clientOk = 0;
			const uint32_t rows2[2] = { 0, plan.h - 1 };
			for (unsigned r = 0; r < 2; r++) {
				md->readBytes((IOByteCount)rows2[r] * srcStride, row, cw * 4u);
				const uint64_t dor = n48_scanout_row_dst(&plan, g.rowBytes, rows2[r]);
				for (uint32_t x = 0; x < plan.w; x++) { compared++; if (amdgpu::RBAR0_32(dev, dor + (uint64_t)x * 4u) != row[x]) bad++; }
			}
			const uint32_t px[3] = { 0, plan.w / 2, plan.w - 1 }, py[3] = { 0, plan.h / 2, plan.h - 1 };
			for (unsigned i = 0; i < 3; i++) {
				const uint32_t d = amdgpu::RBAR0_32(dev, n48_scanout_row_dst(&plan, g.rowBytes, py[i]) + (uint64_t)px[i] * 4u);
				uint32_t sv = 0;
				md->readBytes((IOByteCount)py[i] * srcStride + (IOByteCount)px[i] * 4u, &sv, 4);
				v[8 + i] = ((uint64_t)d << 32) | sv;
				if (sv == n48_client_pixel(px[i], py[i])) clientOk++;
			}
			v[5] = bad; v[6] = compared; v[7] = ((uint64_t)clientOk << 32) | 3u;
			if (logit || bad)
			N48LOG("scanout-staged: READBACK rows 0 and %u of the scanout (BAR0) against the surface BACKING: %u of %u pixel(s) "
			       "differ%s; samples dst/backing (0,0) %#010x/%#010x (%u,%u) %#010x/%#010x (%u,%u) %#010x/%#010x; the backing holds "
			       "the test client's pattern at %u of 3 samples", plan.h - 1, bad, compared, bad ? " - FAILED" : " - ROWS MATCH",
			       (uint32_t)(v[8] >> 32), (uint32_t)v[8], px[1], py[1], (uint32_t)(v[9] >> 32), (uint32_t)v[9], px[2], py[2],
			       (uint32_t)(v[10] >> 32), (uint32_t)v[10], clientOk);
			if (bad) st = kScanStReadback; else gScanoutCopies++;
		}
		if (row) IOFree(row, cw * sizeof(uint32_t));
		if (prepared) md->complete();
		scanout_free_vram(stg);
		if (st != kScanStOk) { gScanoutRefusals++; gScanoutLastReason = st; }
		v[0] = st;
	}
	IOLockUnlock(gScanoutLock);
fin:
	if (out) for (unsigned i = 0; i < count && i < 11; i++) out[i] = v[i];
	return (uint32_t)v[0];
}

bool navi48_scanout_pc_passed(void) { return gScanoutPcPassed; }

// =====================================================================================================================
// build 0.0.542 (apple/scanout_full.h; the push condition's full-res readback) — `accel scanout 9` / `scanout full`: THE
// SURFACE THE DISPLAY ENGINE IS SCANNING OUT, WHOLE, into a kernel capture buffer the CLI pulls through kNavi48SelReadScanFull.
// scanout_full.h holds the decision, the gate, the copy's order and the header; this is only the I/O it is given:
//   - the display: n48dcn::roScanSurface (the READ-ONLY device; nothing here writes a DCN register);
//   - the console: RDNA4FB's Console,* geometry (scanout_geometry, n48_scanout_geom_check), as every scanout path reads it;
//   - flip mode's record, read under gFmLock, which this verb TRY-locks (a present or a restore running = FLIP_BUSY) and holds
//     for the whole copy, so flip mode can neither flip nor write A or B under it;
//   - the staging: switch 89's GART read-back buffer (gFc89Rb, 1 MiB, its read-direction positive control passed), under
//     the fast-copy lock (fc_lock_get), TRY-locked (busy = STAGING_BUSY). Not yet bound this boot: REFUSED as STAGING (0.0.543 E1:
//     never bound here, under gFmLock; `gfxneuter 345` binds and proves it);
//   - each chunk: fastcopy.h's n48_fc_submit_wait (gScanoutLock inside it; the fence wait bounded from the doorbell) with sf_kick
//     below (fc89_kick's packet with this verb's gate), the fence in 63's write-back dword with a value never used before; a
//     timeout RETIRES the read-back buffer (89 goes inert) and latches 63 off (n48_fc_fence_outcome), as 89's control does.
// Lock order: gFmLock (try) -> the fast-copy lock (try) -> gScanoutLock (per chunk, inside n48_fc_submit_wait); gSfLock only around
// the published capture's pointer. OFF / inert: nothing below runs unless `scanout 9`, `scanout 10` or the read selector is called.
// =====================================================================================================================
static_assert(N48_SF_CHUNK_BYTES == N48_FC_STAGING_BYTES, "a scanout-full chunk is exactly switch 89's read-back buffer");
static_assert(N48_SF_SUB_LANDED == N48_FC_SUB_LANDED && N48_SF_SUB_TIMEOUT == N48_FC_SUB_TIMEOUT &&
              N48_SF_SUB_REFUSED == N48_FC_SUB_REFUSED && N48_SF_SUB_RING == N48_FC_SUB_RING,
              "scanout_full.h's submission answers are fastcopy.h's");
static IOLock   *gSfLock { nullptr };     // the published capture (allocated at start())
static uint8_t  *gSfBuf { nullptr };
static uint64_t  gSfLen { 0 }, gSfSeq { 0 }, gSfRuns { 0 }, gSfRefused { 0 };
static uint32_t  gSfDead { 0 };           // a fence that did not land: no further capture this boot
struct SfCtx { const n48_sf_plan *p; uint8_t *dst; uint64_t dmaUs; uint32_t lastFence; };
static uint32_t sf_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	const SfCtx *c = static_cast<const SfCtx *>(vc);
	amdgpu::DeviceContext &dev = *gBringup.dev;
	amdgpu::SDMAInstance &inst = gBringup.sdma.instance[0];
	uint32_t rptr = 0;
	uint32_t qs = scanout_queue_check(dev, inst, &rptr);
	if (qs == kScanStQueueBusy) {   // the bounded queue-idle wait 89 uses (<= N48_FC89_QWAIT_US), without 89's counters
		uint64_t w = 0;
		if (n48_fc89_qwait(&kFc89QWait, N48_FC89_QWAIT_US, &w)) qs = kScanStOk;
	}
	const uint32_t idle = (qs == kScanStOk && inst.wb_bus && gBringup.gmc.vram_start == dev.vramMcBase) ? 1u : 0u;
	// THE LAST GATE, at the ring: the source inside the planned surface, the destination exactly the read-back buffer.
	const uint32_t destWhy = (gFc89Rb.valid() && gFc89RbMc && n48_sf_kick_ok(srcMc, dstMc, bytes, c->p, gFc89RbMc, N48_FC_STAGING_BYTES))
	                         ? 0u : 97u;
	if (n48_fc_submit_ok(destWhy, idle) != 1u) return N48_FC_SUB_REFUSED;
	uint32_t pkt[N48_FC_PKT_DWORDS];
	const uint32_t k = n48_fc_build_packet(pkt, N48_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + N48_FC_WB_FENCE_OFF, fence);
	if (k != N48_FC_PKT_DWORDS) return N48_FC_SUB_REFUSED;
	if (amdgpu::sdma_ring_write(dev, inst, pkt, k) != k) return N48_FC_SUB_RING;
	if (amdgpu::sdma_kick_doorbell(dev, inst) != kIOReturnSuccess) return N48_FC_SUB_RING;
	return 0u;
}
static const n48_fc_wait_ops kSfWaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &sf_kick, &fc_fence_read, &fc_delay };
static uint32_t sf_fence_next(void *) { return n48_fc_fence_next(&gFc); }   // under the fast-copy lock (held)
static uint32_t sf_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
	SfCtx *c = static_cast<SfCtx *>(vc);
	n48_fc_wait_out w {};
	const uint32_t r = n48_fc_submit_wait(&kSfWaitOps, c, srcMc, dstMc, bytes, fence, &w);
	if (w.kicked) { c->dmaUs += w.el_us; c->lastFence = w.seen; }
	amdgpu::sysmem_rmb();            // the fence was read landed: the buffer's reads come after it
	return r;
}
static uint64_t  gSfRetires { 0 };        // build 0.0.543 item E2: captures that retired the read-back buffer (under the fast-copy lock)
static void sf_retire(void *, uint32_t sub) {
	(void)n48_fc_fence_outcome(&gFc, sub);   // 63's path latched off, as 89's own control does on a fence that did not land
	gFc89.rb_retired = 1u;                   // the engine may still write the read-back buffer: never handed out again
	gSfDead = 1u;
	// build 0.0.543 item E2 (the 0.0.542 review's SHOULD-FIX): counted in 89's own stats (as its chunk path counts a fence that did
	// not land) and in this verb's, and named on a line: until 0.0.542 the latch and the retirement happened with no line at all.
	if (sub == N48_FC_SUB_RING) gFc89S.ring_fail++; else gFc89S.timeouts++;
	gSfRetires++;
	N48LOG("scanfull542: a chunk's fence did NOT land (%s): switch 63's fast-copy path LATCHED OFF, switch 89's read-back buffer RETIRED "
	       "for the boot (retires %llu; 63 latched %u why %u; 89 timeouts %llu ring %llu)",
	       sub == N48_FC_SUB_RING ? "ring write / doorbell failed" : "fence timeout", (unsigned long long)gSfRetires,
	       gFc.latched_off, gFc.latch_why, (unsigned long long)gFc89S.timeouts, (unsigned long long)gFc89S.ring_fail);
}
static void sf_copy_out(void *vc, uint64_t off, uint32_t bytes) {
	SfCtx *c = static_cast<SfCtx *>(vc);
	memcpy(c->dst + N48_SF_HDR_BYTES + off, gFc89Rb.cpu, bytes);
}
// Flip mode's record as the capture sees it (lock-free for the pre-size; under gFmLock for the decision that counts).
static void sf_ctx_fill(n48_sf_ctx *c) {
	c->fm_have_b = __atomic_load_n(&gFm.haveB, __ATOMIC_ACQUIRE);
	c->fm_on = __atomic_load_n(&gFm.on, __ATOMIC_ACQUIRE);
	c->fm_engaged = __atomic_load_n(&gFm.engaged, __ATOMIC_ACQUIRE);
	c->fm_restore_req = __atomic_load_n(&gFm.restoreReq, __ATOMIC_ACQUIRE);
	c->fm_a_copy_pending = __atomic_load_n(&gFm.aCopyPending, __ATOMIC_ACQUIRE);
	c->fm_test_held = n48dcn::fmTestFlipHeld();
	c->b_off = gFm.bOff; c->b_len = gFm.len; c->b_mc = c->fm_have_b ? gBringup.gmc.vram_start + gFm.bOff : 0u;
	c->fm_front = __atomic_load_n(&gFm.front, __ATOMIC_RELAXED);
	c->armed = n48::hw_cm_armed() ? 1u : 0u;
}
static uint64_t sf_now_us() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns / 1000u; }

// out: [0] status (the caller's), [1] reason (N48_SF_*), [2] surface MC, [3] payload bytes, [4] width << 32 | height,
// [5] pitch bytes << 32 | format << 8 | SW_MODE, [6] which | flags << 8, [7] copy us, [8] chunks, [9] FNV-1a, [10] seq,
// [11] frame count before << 32 | after, [12] total bytes published (header + payload).
static uint32_t navi48_scanout_full(uint32_t mode, uint64_t *v) {
	if (!gSfLock) return N48_SF_R_NO_CONTEXT;
	if (mode == N48_SF_MODE_RELEASE) {
		IOLockLock(gSfLock);
		uint8_t *b = gSfBuf; const uint64_t n = gSfLen;
		gSfBuf = nullptr; gSfLen = 0;
		IOLockUnlock(gSfLock);
		if (b) IOFree(b, n);
		N48LOG("scanfull542: RELEASED capture #%llu (%llu bytes freed)", (unsigned long long)gSfSeq, (unsigned long long)(b ? n : 0));
		return N48_SF_OK;
	}
	if (!gBringup.dev || !gBringup.gmc.inited || !gScanoutLock || !gFmLock) return N48_SF_R_NO_CONTEXT;
	gSfRuns++;
	amdgpu::DeviceContext &dev = *gBringup.dev;
	n48_sf_ctx c {}; n48_sf_dcn d0 {}, d1 {}; n48_sf_plan p {};
	N48ScanoutGeom g {}; IOService *fb = nullptr; uint64_t aOff = 0;
	uint8_t *buf = nullptr; uint64_t need = 0;
	uint32_t r = N48_SF_OK, chunks = 0, w0 = 0, w1 = 0;
	uint64_t t0 = 0, t1 = 0;
	clock_sec_t cs = 0; clock_usec_t cu = 0;
	IOLock *fl = nullptr;
	bool fmHeld = false, fcHeld = false;
	SfCtx sc {};
	if (gSfDead) { r = N48_SF_R_RETIRED; goto out; }
	// 1. the console and the display (registry and read-only reads), to size the capture buffer before any lock.
	c.con_ok = (scanout_geometry(dev, &g, &fb) == kScanStOk && n48_scanout_geom_check(&g, &aOff) == kScanGeomOk &&
	            gBringup.gmc.vram_start == dev.vramMcBase) ? 1u : 0u;
	c.con_w = g.width; c.con_h = g.height; c.con_row_bytes = g.rowBytes;
	c.a_off = aOff; c.a_mc = gBringup.gmc.vram_start + aOff; c.a_len = g.fbLen;
	sf_ctx_fill(&c);
	(void)n48dcn::roScanSurface(&d0);
	r = n48_sf_decide(&d0, &c, &p);
	if (r) goto out;
	need = N48_SF_HDR_BYTES + p.bytes;
	buf = static_cast<uint8_t *>(IOMalloc(need));
	if (!buf) { r = N48_SF_R_ALLOC; goto out; }
	// 2. flip mode's lock, then the fast-copy lock: both TRY-locked, so this verb never waits behind a present or a copy.
	if (!IOLockTryLock(gFmLock)) { r = N48_SF_R_FLIP_BUSY; goto out; }
	fmHeld = true;
	fl = fc_lock_get();
	if (!fl || !IOLockTryLock(fl)) { r = fl ? N48_SF_R_STAGING_BUSY : N48_SF_R_NO_CONTEXT; goto out; }
	fcHeld = true;
	if (gFc89.rb_retired) { r = N48_SF_R_RETIRED; goto out; }
	// build 0.0.543 item E1 (the 0.0.542 review's SHOULD-FIX; option "refuse", chosen over binding before gFmLock): the read-back
	// buffer is NEVER bound here. fc89_rb_bind + fc89_control take ~58 ms, and under gFmLock that stalled every present; 89 ON
	// (`gfxneuter 345`, navi48_fc89_set) binds and proves it under the fast-copy lock alone. Unbound = STAGING, nothing waited on.
	if (!(gFc89Rb.valid() && gFc89RbMc && gFc89.rb_ok)) { r = N48_SF_R_STAGING; goto out; }
	if (!fc_sdma_up()) { r = N48_SF_R_QUEUE; goto out; }
	if (fc_dcc_raw() & N48_DCC_NOPTE_COMP_EN_MASK) { r = N48_SF_R_DCC; goto out; }
	// 3. THE DECISION THAT COUNTS, under gFmLock: flip mode is still, the display is read again.
	sf_ctx_fill(&c);
	(void)n48dcn::roScanSurface(&d0);
	{
		n48_sf_plan p2 {};
		r = n48_sf_decide(&d0, &c, &p2);
		if (r) goto out;
		if (p2.bytes != p.bytes) { r = N48_SF_R_CHANGED; goto out; }
		p = p2;
	}
	// 4. the copy (n48_sf_run: per chunk the gate, a fence value, ONE bounded submission, the copy-out).
	sc.p = &p; sc.dst = buf;
	w0 = __atomic_load_n(&gScanoutWrites, __ATOMIC_SEQ_CST);   // build 0.0.543 E3: the writes counted AT the write
	clock_get_calendar_microtime(&cs, &cu);
	t0 = sf_now_us();
	{
		const n48_sf_ops ops { &sc, &sf_fence_next, &sf_submit, &sf_retire, &sf_copy_out };
		r = n48_sf_run(&ops, &p, gFc89RbMc, &chunks);
	}
	t1 = sf_now_us();
	w1 = __atomic_load_n(&gScanoutWrites, __ATOMIC_SEQ_CST);
	// build 0.0.543 item E5: the fast copy's state as this copy left it (the fast-copy lock is still held), for the header.
	c.fc63_latched = gFc.latched_off; c.fc63_why = gFc.latched_off ? gFc.latch_why : 0u;
	c.fc89_state = (gFc89.rb_ok ? 1u : 0u) | (gFc89.rb_retired ? 2u : 0u) | (gFc89.pc_done ? 4u : 0u);
	c.fc89_mode = __atomic_load_n(&gFc89Mode, __ATOMIC_ACQUIRE);
	// 5. the display again: the same surface, no flip pending (else the capture is not one picture).
	if (!r) {
		(void)n48dcn::roScanSurface(&d1);
		if (!d1.dcn_ok || d1.pending || d1.earliest != d0.earliest || d1.primary != d0.primary) r = N48_SF_R_CHANGED;
	}
out:
	if (fcHeld) IOLockUnlock(fl);
	if (fmHeld) IOLockUnlock(gFmLock);
	uint32_t fnv = 0;
	if (!r && buf) {
		n48_sf_hdr h {};
		n48_sf_hdr_fill(&h, &d0, &c, &p);
		if (w1 != w0) h.flags |= N48_SF_F_WRITTEN_DURING;
		h.writers_during = w1 - w0;
		h.uptime_us = t0; h.cal_sec = (uint64_t)cs; h.cal_usec = (uint32_t)cu;
		h.copy_us = (uint32_t)(t1 - t0); h.chunks = chunks;
		h.fc_before = d0.frame_count; h.fc_after = d1.frame_count;
		fnv = n48_sf_fnv32(buf + N48_SF_HDR_BYTES, p.bytes);
		h.fnv32 = fnv;
		IOLockLock(gSfLock);
		h.seq = ++gSfSeq;
		memcpy(buf, &h, sizeof(h));
		uint8_t *old = gSfBuf; const uint64_t oldLen = gSfLen;
		gSfBuf = buf; gSfLen = need;
		IOLockUnlock(gSfLock);
		buf = nullptr;
		if (old) IOFree(old, oldLen);
		v[10] = h.seq; v[12] = need;
		v[7] = h.copy_us; v[8] = chunks; v[9] = fnv; v[6] = (uint64_t)p.which | ((uint64_t)h.flags << 8);
		v[11] = ((uint64_t)d0.frame_count << 32) | d1.frame_count;
	} else {
		gSfRefused++;
		v[6] = (uint64_t)p.which | ((uint64_t)p.flags << 8);
	}
	if (buf) IOFree(buf, need);
	v[1] = r; v[2] = d0.earliest; v[3] = p.bytes; v[4] = ((uint64_t)d0.vp_w << 32) | d0.vp_h;
	v[5] = ((uint64_t)d0.pitch_px * 4u << 32) | ((uint64_t)d0.fmt << 8) | d0.sw_mode;
	N48LOG("scanfull542: %s (%u: %s); runs %llu refused %llu", r ? "REFUSED" : "CAPTURED", r, n48_sf_reason_name(r),
	       (unsigned long long)gSfRuns, (unsigned long long)gSfRefused);
	N48LOG("scanfull542: surface %s MC %#llx (programmed %#llx, pending %u, OTG %d) %ux%u pitch %u fmt %u SW_MODE %u; %llu bytes in %u "
	       "chunk(s), %llu us (dma %llu us, fence %#010x), FNV %08x; flip mode %s engaged %u front %#llx; frames %u -> %u; writers %u",
	       p.which == 1u ? "A" : p.which == 2u ? "B" : "-", (unsigned long long)d0.earliest, (unsigned long long)d0.primary,
	       d0.pending, (int)d0.lit_otg, d0.vp_w, d0.vp_h, d0.pitch_px * 4u, d0.fmt, d0.sw_mode, (unsigned long long)p.bytes, chunks,
	       (unsigned long long)(t1 - t0), (unsigned long long)sc.dmaUs, sc.lastFence, fnv, c.fm_on ? "ON" : "OFF", c.fm_engaged,
	       (unsigned long long)c.fm_front, d0.frame_count, d1.frame_count, w1 - w0);
	return r;
}

// kNavi48SelReadScanFull (Navi48UserClient.cpp): up to `max` bytes of the published capture from `off`. Read-only. Returns the bytes
// copied; *total the capture's size (0 = none published), *seq its number.
uint32_t navi48_scanfull_read(uint64_t off, void *dst, uint32_t max, uint64_t *total, uint64_t *seq) {
	*total = 0; *seq = 0;
	if (!gSfLock || !dst) return 0u;
	uint32_t n = 0;
	IOLockLock(gSfLock);
	if (gSfBuf && off < gSfLen) {
		const uint64_t left = gSfLen - off;
		n = left < max ? (uint32_t)left : max;
		memcpy(dst, gSfBuf + off, n);
	}
	*total = gSfBuf ? gSfLen : 0u;
	*seq = gSfBuf ? gSfSeq : 0u;
	IOLockUnlock(gSfLock);
	return n;
}

// 0.0.193 — pulse CLEAR_PROTECTION_FAULT_STATUS_ADDR on both hubs. The status
// register is first-fault latched, so without this every measurement reads
// whichever fault happened first since boot.
bool navi48_vm_fault_clear(void) {
	if (!gBringup.dev || !gBringup.gmc.inited) return false;
	amdgpu::gmc_clear_vm_faults(*gBringup.dev, gBringup.gmc);
	return true;
}

// -- THE DISCRIMINATOR.
//
// Apple's submission 5 issues nine SRBM_WRITEs to GCVM registers and then polls
// an ACK. The engine demonstrably executes them (IB_OFFSET sits at the poll, past
// all nine) yet the target registers still read reset defaults. Two
// explanations remained: the GCVM block rejects those writes specifically, or
// SRBM_WRITE does nothing at all on this part.
//
// Apple's stream offers no positive control -- the opcode appears only in the IB
// that wedges. So drive it from a ring we own:
//
//   1. MMIO-write sentinelA. Proves the target register is writable and sticks.
//      If this fails the target is bad and nothing else is measured.
//   2. Emit SRBM_WRITE(reg, sentinelB) + FENCE on SDMA0 QUEUE0, kick, poll the fence.
//      The FENCE matters: without it a null result cannot distinguish "opcode
//      inert" from "engine never ran the packet".
//   3. Read back.
//
//   reads sentinelB -> the opcode WORKS; GCVM specifically rejects Apple's writes
//   reads sentinelA -> register writable, opcode INERT: Apple's VM programming can
//                      never work from the command stream at ANY numbering
//
// Operand form is the DWORD INDEX, which established on hardware:
// the reg<<2 targets all read 0xffffffff, i.e. unmapped. A mis-decode therefore
// lands in that same inert region rather than on a live register.
bool navi48_sdma_srbm_probe(uint32_t reg, uint32_t sentinelA, uint32_t sentinelB,
                            uint32_t sentinelC) {
	if (!gBringup.dev) { N48LOG("srbm-probe: no device context"); return false; }
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[0]) { N48LOG("srbm-probe: SDMA0 not present"); return false; }
	auto &inst = sdma.instance[0];
	if (!inst.inited) { N48LOG("srbm-probe: SDMA0 not inited"); return false; }

	const uint32_t before = amdgpu::RREG32(*gBringup.dev, reg);
	amdgpu::WREG32(*gBringup.dev, reg, sentinelA);
	const uint32_t afterMmio = amdgpu::RREG32(*gBringup.dev, reg);
	N48LOG("srbm-probe: reg %#06x was %#010x; MMIO wrote %#010x, reads %#010x -- %s",
	       reg, before, sentinelA, afterMmio,
	       afterMmio == sentinelA ? "WRITABLE" : "*** MMIO WRITE DID NOT STICK - bad target ***");
	if (afterMmio != sentinelA) return false;

	//: run BOTH operand encodings. Only the dword-index form was tested in
	// 0.0.152, so "did not land" could equally have meant the hardware expects
	// reg<<2 and our operand addressed something else. Both mis-decode targets are
	// measured UNMAPPED (0x810-0x812 and 0x8116-0x811a all read 0xffffffff), so
	// whichever form is wrong writes into dead space rather than a live register.
	const uint64_t fenceGpu = inst.wb_bus + amdgpu::kSDMAWBFenceOffset;
	uint32_t landed = 0;

	for (int form = 0; form < 2; form++) {
		const bool shifted   = (form == 1);
		const uint32_t operand = shifted ? (reg << 2) : reg;
		const uint32_t val     = shifted ? sentinelC : sentinelB;
		const uint32_t fenceVal = shifted ? 0x5A5AC0DFu : 0x5A5AC0DEu;
		const char *name = shifted ? "reg<<2 (upstream emit_wreg,)"
		                           : "dword index";

		amdgpu::sdma_wb_write32(*gBringup.dev, inst, amdgpu::kSDMAWBFenceOffset, 0);

		uint32_t pkt[7];
		pkt[0] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_SRBM_WRITE) |
		         amdgpu::SDMA_PKT_HEADER_BYTE_EN(0xf);
		pkt[1] = operand;
		pkt[2] = val;
		pkt[3] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);
		pkt[4] = static_cast<uint32_t>(fenceGpu);
		pkt[5] = static_cast<uint32_t>(fenceGpu >> 32);
		pkt[6] = fenceVal;

		N48LOG("srbm-probe: form %d = %s: header %#010x operand %#06x val %#010x "
		       "(targets reg %#06x) wptr %llu", form, name, pkt[0], operand, val,
		       shifted ? reg : operand, (unsigned long long)inst.wptr);

		if (amdgpu::sdma_ring_write(*gBringup.dev, inst, pkt, 7) != 7) {
			N48LOG("srbm-probe: form %d ring_write failed - nothing kicked", form);
			return false;
		}
		if (amdgpu::sdma_kick_doorbell(*gBringup.dev, inst) != kIOReturnSuccess) {
			N48LOG("srbm-probe: form %d doorbell kick failed", form); return false;
		}

		uint32_t seen = 0, waited = 0;
		for (; waited < 200000; waited += 10) {
			seen = amdgpu::sdma_wb_read32(*gBringup.dev, inst, amdgpu::kSDMAWBFenceOffset);
			if (seen == fenceVal) break;
			IODelay(10);
		}
		const uint32_t after = amdgpu::RREG32(*gBringup.dev, reg);
		const bool hit = (after == val);
		if (hit) landed++;
		N48LOG("srbm-probe: form %d FENCE %s after ~%u us; reg %#06x now %#010x -- %s",
		       form, seen == fenceVal ? "FIRED" : "TIMEOUT", waited, reg, after,
		       hit ? "*** LANDED - this is the hardware's operand encoding ***"
		           : "did not land");
	}

	N48LOG("srbm-probe: VERDICT - %s",
	       landed ? "SRBM_WRITE WORKS; GCVM specifically rejects Apple's writes"
	              : "*** NEITHER encoding wrote the register: SRBM_WRITE cannot write "
	                "registers from the command stream on this part ***");
	return true;
}

// Write a GART range that may span pages. Mirror of the read, same per-page
// residency refusal.

bool navi48_gart_write_range(uint64_t gartAddr, const void *src, uint32_t len) {
	if (!src || len == 0 || len > 65536) return false;
	const uint8_t *in = static_cast<const uint8_t *>(src);
	uint32_t done = 0;
	while (done < len) {
		const uint64_t a   = gartAddr + done;
		const uint32_t off = (uint32_t)(a & 0xFFF);
		uint32_t chunk     = 4096u - off;
		if (chunk > len - done) chunk = len - done;
		if (!navi48_gart_write_page(a, in + done, chunk)) {
			N48LOG("gart-write-range: page %#llx of range %#llx+%u FAILED — the "
			       "range may now be PARTIALLY written",
			       (unsigned long long)(a & ~0xFFFULL),
			       (unsigned long long)gartAddr, len);
			return false;
		}
		done += chunk;
	}
	return true;
}

// Kick the EXTERNAL (QUEUE1) doorbell with a wptr Apple owns.
//
// WHY THIS IS A SEPARATE STEP FROM PROGRAMMING. sdma_program_external_queue
// deliberately resets RPTR/WPTR to 0 (step 2, and again in the MINOR_PTR_UPDATE
// handshake). So after `enablequeue` the QUEUE1 block reads RB_ENABLE=1 with
// RPTR=0 WPTR=0 — armed, and correctly seeing an EMPTY ring, because Apple's
// 128 dwords are described by a wptr the engine has never been told about.
// Measured, not assumed:
//     SDMA1 QUEUE1 [after]: RB_CNTL=0x00841821 RB_BASE=0:0x84002200
//                           RPTR=0 WPTR=0 DOORBELL=0x10000000 OFF=0x00000858
// This is the call that actually makes the engine fetch.
//
// No residency re-check here on purpose: programming already validated the ring
// page AND the writeback page, and nothing has been remapped since. Re-testing
// on a different premise would risk the failure mode the programming bridge
// warns about — a guard that fails closed on a false premise is still a bug.
// Read-only dump of the QUEUE1 register block.
//
// WHY THIS EXISTS. Until now the only way to see QUEUE1's RPTR/WPTR was to run
// `enablequeue`, because sdma_program_external_queue logs them before and after.
// But that call RESETS RPTR/WPTR to 0 — so the only available instrument
// destroyed the state it was measuring. sdma_log_external_queue_regs is eight
// RREG32s and no writes; exposing it directly makes the queue observable without
// disturbing it.
bool navi48_sdma_log_external_queue(const char *tag) {
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[1]) return false;
	auto &inst = sdma.instance[1];
	if (!inst.inited) return false;
	amdgpu::sdma_log_external_queue_regs(*gBringup.dev, inst, tag ? tag : "peek");
	return true;
}

// QUEUE1's live read pointer, for's getHead instrumentation. Reads only, and
// guarded exactly like the dump above so a torn-down device fails closed.
bool navi48_sdma_read_external_rptr(uint32_t *rptrRegOut, uint32_t *wbRptrOut) {
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[1]) return false;
	auto &inst = sdma.instance[1];
	if (!inst.inited) return false;
	uint32_t wb = 0;
	const uint32_t reg = amdgpu::sdma_read_external_rptr(*gBringup.dev, inst, &wb);
	if (rptrRegOut) *rptrRegOut = reg;
	if (wbRptrOut)  *wbRptrOut  = wb;
	return true;
}

bool navi48_sdma_kick_external_doorbell(uint32_t wptrDwords, uint32_t doorbellIndex) {
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[1]) {
		N48LOG("ext-kick: SDMA instance 1 is not present — refusing");
		return false;
	}
	auto &inst = sdma.instance[1];
	if (!inst.inited || inst.wb_bus == 0) {
		N48LOG("ext-kick: SDMA1 not inited (inited=%d wb_bus=%#llx) — refusing",
		       (int)inst.inited, (unsigned long long)inst.wb_bus);
		return false;
	}
	if (wptrDwords == 0) {
		N48LOG("ext-kick: wptr is 0 — nothing to fetch, refusing");
		return false;
	}
	return amdgpu::sdma_kick_external_doorbell(*gBringup.dev, inst, wptrDwords,
	                                           doorbellIndex) == kIOReturnSuccess;
}

bool navi48_sdma_program_external_queue(uint64_t ringGpuVa, uint32_t ringSizeDwords,
                                        uint32_t doorbellIndex, bool enableRing) {
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[1]) {
		N48LOG("ext-queue: SDMA instance 1 is not present — refusing");
		return false;
	}
	auto &inst = sdma.instance[1];
	if (!inst.inited || inst.wb_bus == 0) {
		N48LOG("ext-queue: SDMA1 not inited (inited=%d wb_bus=%#llx) — refusing",
		       (int)inst.inited, (unsigned long long)inst.wb_bus);
		return false;
	}
	// The engine WRITES the rptr into the writeback page once RB_ENABLE=1, so
	// that page has to be reachable by the GPU before we ever enable. dumpring
	// only inspects addresses embedded in the ring, so this one has never been
	// checked.
	//
	// Branch on wb_in_vram, NOT on the address range. The writeback page has two
	// allocation paths (sdma_v7_0.cpp): sysmem + gmc_bind_existing, where wb_bus
	// is a GART MC address and a PTE must exist; or a VRAM fallback, where wb_bus
	// is an FB-aperture address with no GART PTE at all. Demanding a PTE in the
	// second case would refuse a perfectly valid setup — a guard that fails
	// closed on a false premise is still a bug.
	const uint64_t extRptr = inst.wb_bus + amdgpu::kSDMAWBExtRptrOffset;
	const uint64_t extWptr = inst.wb_bus + amdgpu::kSDMAWBExtWptrOffset;
	if (inst.wb_in_vram) {
		N48LOG("ext-queue: WB page is in VRAM (mc=%#llx, FB aperture) — no GART "
		       "PTE expected; rptr_wb=%#llx wptr_poll=%#llx",
		       (unsigned long long)inst.wb_bus,
		       (unsigned long long)extRptr, (unsigned long long)extWptr);
	} else {
		// Both offsets (0x100/0x140) lie in the same 4 KiB page as wb_bus, so one
		// PTE covers both.
		const uint64_t page = extRptr & ~0xFFFULL;
		uint64_t pte = 0;
		if (!navi48_gart_read_pte(page, pte)) {
			N48LOG("ext-queue: cannot read the PTE for the WB page %#llx — REFUSING",
			       (unsigned long long)page);
			return false;
		}
		if (!(pte & 1ULL) || !(pte & (1ULL << 63))) {
			N48LOG("ext-queue: WB page %#llx PTE %#llx is NOT RESIDENT "
			       "(needs VALID|IS_PTE) — the engine would write the rptr into an "
			       "unmapped page. Run `accel synctables` first — REFUSING",
			       (unsigned long long)page, (unsigned long long)pte);
			return false;
		}
		N48LOG("ext-queue: WB page %#llx PTE %#llx phys %#llx RESIDENT "
		       "(rptr_wb=%#llx wptr_poll=%#llx, GART-backed)",
		       (unsigned long long)page, (unsigned long long)pte,
		       (unsigned long long)(pte & 0xFFFFFFF000ULL),
		       (unsigned long long)extRptr, (unsigned long long)extWptr);
	}

	return amdgpu::sdma_program_external_queue(*gBringup.dev, inst, ringGpuVa,
	                                           ringSizeDwords, doorbellIndex,
	                                           enableRing) == kIOReturnSuccess;
}

// ---- 0.0.185 "sdmamap" — SDMA **instance 0** QUEUE1 ----------------------
//
// Everything below is instance 0, deliberately:'s decision is that our own
// SDMA0 QUEUE0 keeps running as the control while QUEUE1 on the same engine
// takes Apple's ring. The bridge above stays on instance 1 so the two
// experiments cannot collide.

uint32_t navi48_sdma_q1_doorbell_index(void) {
	return amdgpu::kSDMAQ1AppleDoorbellIndex;
}

bool navi48_sdma_q1_doorbell_ptr(uint32_t index, void **ptrOut, uint64_t *byteOffOut) {
	if (ptrOut) *ptrOut = nullptr;
	if (byteOffOut) *byteOffOut = 0;
	if (!gBringup.dev || !gBringup.dev->bar2) return false;
	const auto &dev = *gBringup.dev;
	const auto &ix  = dev.doorbell.index;
	const uint64_t off = (uint64_t)index * 4ull;
	if (off + 8 > dev.bar2Size) {
		N48LOG("sdma-q1-doorbell: dword index %#x (byte %#llx) is outside the "
		       "%llu KiB BAR2 aperture - REFUSING", index,
		       (unsigned long long)off, (unsigned long long)(dev.bar2Size >> 10));
		return false;
	}
	// Refuse anything our own map assigns. The generic navi48_doorbell_dword_ptr
	// refuses the whole range at and above sdma_engine[0]<<1; here the four SDMA
	// engines' own QUEUE0 doorbells are named individually instead, because the
	// takeover's whole point is to use a FREE index inside that range.
	for (unsigned e = 0; e < 4; e++) {
		const uint32_t owned = ix.sdma_engine[e] << 1;
		if (index == owned || index == owned + 1) {
			N48LOG("sdma-q1-doorbell: dword index %#x is SDMA%u QUEUE0's own "
			       "doorbell (%#x) - REFUSING", index, e, owned);
			return false;
		}
	}
	if (index == ix.mes_ring0 || index == ix.mes_ring1 || index == ix.ih ||
	    index == ix.kiq || index == ix.hiq || index == ix.diq ||
	    (index >= ix.mec_ring[0] && index <= ix.mec_ring[7]) ||
	    (index >= ix.mes_aggregated[0] && index <= ix.mes_aggregated[4] + 1)) {
		N48LOG("sdma-q1-doorbell: dword index %#x belongs to one of OUR engines "
		       "- REFUSING", index);
		return false;
	}
	if (ptrOut) *ptrOut = (void *)(dev.bar2 + off);
	if (byteOffOut) *byteOffOut = off;
	N48LOG("sdma-q1-doorbell: dword index %#x -> BAR2 + %#llx (%p). This is the "
	       "word AMDRTRing::writeTail will store the wptr into.",
	       index, (unsigned long long)off, (void *)(dev.bar2 + off));
	return true;
}

bool navi48_sdma_q1_read_regs(Navi48SdmaQ1Regs *out) {
	if (!out) return false;
	*out = Navi48SdmaQ1Regs {};
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[0]) return false;
	auto &inst = sdma.instance[0];
	if (!inst.inited) return false;
	amdgpu::SDMAQueue1Regs r {};
	amdgpu::sdma_read_external_queue_regs(*gBringup.dev, inst, &r);
	out->rb_cntl = r.rb_cntl;               out->rb_base = r.rb_base;
	out->rb_base_hi = r.rb_base_hi;         out->rb_rptr = r.rb_rptr;
	out->rb_rptr_hi = r.rb_rptr_hi;         out->rb_wptr = r.rb_wptr;
	out->rb_wptr_hi = r.rb_wptr_hi;
	out->rb_rptr_addr_lo = r.rb_rptr_addr_lo;
	out->rb_rptr_addr_hi = r.rb_rptr_addr_hi;
	out->ib_cntl = r.ib_cntl;               out->doorbell = r.doorbell;
	out->doorbell_offset = r.doorbell_offset;
	out->wptr_poll_lo = r.wptr_poll_lo;     out->wptr_poll_hi = r.wptr_poll_hi;
	out->minor_ptr_update = r.minor_ptr_update;
	return true;
}

bool navi48_sdma_q1_fallback_rptr_addr(uint64_t *addrOut) {
	if (addrOut) *addrOut = 0;
	if (!gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[0]) return false;
	auto &inst = sdma.instance[0];
	if (!inst.inited || inst.wb_bus == 0) return false;
	if (addrOut) *addrOut = inst.wb_bus + amdgpu::kSDMAWBExtRptrOffset;
	return true;
}

bool navi48_sdma_q1_disable(void) {
	if (!gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[0]) return false;
	auto &inst = sdma.instance[0];
	if (!inst.inited) return false;
	return amdgpu::sdma_disable_external_queue(*gBringup.dev, inst) == kIOReturnSuccess;
}

uint32_t navi48_sdma_q1_test_result(void) {
	const auto &t = gBringup.sdmaQ1Test;
	if (!t.ran)         return 0;
	if (t.doorbell_ok)  return 1;
	if (t.mmio_ok)      return 2;
	return 3;
}

// ---- 0.0.196: the N-ring generalisation ----------------------------------
//
// One table (amdgpu::kSDMAExtSlots) names every hardware queue the takeover may
// spend and the doorbell dword it comes with; everything below is a thin,
// bounds-checked view of it, so the Apple side never computes a register offset
// or a doorbell index of its own.

uint32_t navi48_sdma_slot_count(void) { return amdgpu::kSDMAExtSlotCount; }

// The SDMA instance behind a slot, or nullptr when the slot cannot be used on
// this boot. Never returns an instance whose QUEUE0 resume did not run: QUEUE1+
// borrow that resume's MCU unhalt and UTCL1/WATCHDOG setup.
static amdgpu::SDMAInstance *sdma_slot_instance(uint32_t slot) {
	if (slot >= amdgpu::kSDMAExtSlotCount) return nullptr;
	if (!gBringup.gfxhubReady || !gBringup.dev) return nullptr;
	const amdgpu::SDMAExtQueueSlot &sl = amdgpu::kSDMAExtSlots[slot];
	if (sl.instance >= amdgpu::kSDMAInstanceCount) return nullptr;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[sl.instance]) return nullptr;
	auto &inst = sdma.instance[sl.instance];
	if (!inst.inited || inst.wb_bus == 0) return nullptr;
	return &inst;
}

bool navi48_sdma_slot_info(uint32_t slot, uint32_t *instOut, uint32_t *queueOut,
                           uint32_t *doorbellOut, bool *usableOut) {
	if (instOut) *instOut = 0;
	if (queueOut) *queueOut = 0;
	if (doorbellOut) *doorbellOut = 0;
	if (usableOut) *usableOut = false;
	if (slot >= amdgpu::kSDMAExtSlotCount) return false;
	const amdgpu::SDMAExtQueueSlot &sl = amdgpu::kSDMAExtSlots[slot];
	if (instOut) *instOut = sl.instance;
	if (queueOut) *queueOut = sl.queue;
	if (doorbellOut) *doorbellOut = sl.doorbell_dword;
	if (usableOut) *usableOut = (sdma_slot_instance(slot) != nullptr);
	return true;
}

uint32_t navi48_sdma_slot_test_result(uint32_t slot) {
	if (slot >= amdgpu::kSDMAExtSlotCount) return 0;
	const auto &t = gBringup.sdmaQnTest[slot];
	if (t.ran) {
		if (t.doorbell_ok) return 1;
		if (t.mmio_ok)     return 2;
		return 3;
	}
	// Slot 0 is (SDMA0, QUEUE1, 0x202) — exactly what the older
	// navi48-sdma-q1-test=1 proves, so its verdict stands in when only the
	// original boot-arg was given.
	if (slot == 0) return navi48_sdma_q1_test_result();
	return 0;
}

bool navi48_sdma_slot_read_regs(uint32_t slot, Navi48SdmaQ1Regs *out) {
	if (!out) return false;
	*out = Navi48SdmaQ1Regs {};
	amdgpu::SDMAInstance *inst = sdma_slot_instance(slot);
	if (!inst) return false;
	amdgpu::SDMAQueue1Regs r {};
	amdgpu::sdma_read_external_queue_regs(*gBringup.dev, *inst, &r,
	                                      amdgpu::kSDMAExtSlots[slot].queue);
	out->rb_cntl = r.rb_cntl;               out->rb_base = r.rb_base;
	out->rb_base_hi = r.rb_base_hi;         out->rb_rptr = r.rb_rptr;
	out->rb_rptr_hi = r.rb_rptr_hi;         out->rb_wptr = r.rb_wptr;
	out->rb_wptr_hi = r.rb_wptr_hi;
	out->rb_rptr_addr_lo = r.rb_rptr_addr_lo;
	out->rb_rptr_addr_hi = r.rb_rptr_addr_hi;
	out->ib_cntl = r.ib_cntl;               out->doorbell = r.doorbell;
	out->doorbell_offset = r.doorbell_offset;
	out->wptr_poll_lo = r.wptr_poll_lo;     out->wptr_poll_hi = r.wptr_poll_hi;
	out->minor_ptr_update = r.minor_ptr_update;
	return true;
}

bool navi48_sdma_slot_disable(uint32_t slot) {
	amdgpu::SDMAInstance *inst = sdma_slot_instance(slot);
	if (!inst) return false;
	return amdgpu::sdma_disable_external_queue(*gBringup.dev, *inst,
	           amdgpu::kSDMAExtSlots[slot].queue) == kIOReturnSuccess;
}

bool navi48_sdma_slot_fallback_rptr_addr(uint32_t slot, uint64_t *addrOut) {
	if (addrOut) *addrOut = 0;
	amdgpu::SDMAInstance *inst = sdma_slot_instance(slot);
	if (!inst) return false;
	// One write-back page per INSTANCE, so two slots on the same instance must
	// not share the external rptr slot. Step the per-slot address by 8 inside
	// the instance's WB page, keyed on the queue number.
	const uint32_t q = amdgpu::kSDMAExtSlots[slot].queue;
	if (addrOut)
		*addrOut = inst->wb_bus + amdgpu::kSDMAWBExtRptrOffset + (uint64_t)(q - 1) * 8ull;
	return true;
}

bool navi48_sdma_map_external_ring_slot(uint32_t slot, uint64_t ringGpuVa,
                                        uint32_t ringSizeDwords, uint64_t rptrAddr,
                                        uint64_t wptrPollAddr, uint32_t *failStepOut) {
	if (failStepOut) *failStepOut = 1;
	amdgpu::SDMAInstance *instp = sdma_slot_instance(slot);
	if (!instp) {
		N48LOG("sdma-map: slot %u is not usable on this boot (instance absent or "
		       "not inited) — REFUSING", slot);
		return false;
	}
	auto &inst = *instp;
	const amdgpu::SDMAExtQueueSlot &sl = amdgpu::kSDMAExtSlots[slot];
	if (!inst.enabled)
		N48LOG("sdma-map: NOTE — SDMA%u QUEUE0 reads not-enabled. QUEUE%u borrows "
		       "this instance's MCU unhalt and UTCL1/WATCHDOG setup from QUEUE0's "
		       "resume, so this is worth reading in the log above.",
		       sl.instance, sl.queue);

	// Step 1: program with the ring DISABLED. Nothing is fetched yet.
	if (failStepOut) *failStepOut = 2;
	kern_return_t r = amdgpu::sdma_program_external_queue_ex(
	    *gBringup.dev, inst, ringGpuVa, ringSizeDwords, sl.doorbell_dword,
	    rptrAddr, wptrPollAddr, /*enable_ring=*/false, sl.queue);
	if (r != kIOReturnSuccess) {
		N48LOG("sdma-map: SDMA%u QUEUE%u programming refused %#x (ring %#llx, %u "
		       "dwords) — nothing enabled", sl.instance, sl.queue, r,
		       (unsigned long long)ringGpuVa, ringSizeDwords);
		return false;
	}

	// Step 2: read back and compare against what we asked for. Same gate as the
	// single-queue path, RB_WPTR still logged and never judged (0.0.187).
	if (failStepOut) *failStepOut = 3;
	amdgpu::SDMAQueue1Regs g {};
	amdgpu::sdma_read_external_queue_regs(*gBringup.dev, inst, &g, sl.queue);
	uint32_t want_size = 0;
	while ((1u << want_size) < ringSizeDwords) want_size++;
	const uint32_t got_size   = (g.rb_cntl >> 1) & 0x1Fu;
	const uint32_t want_base  = (uint32_t)(ringGpuVa >> 8);
	const uint32_t want_bhi   = (uint32_t)(ringGpuVa >> 40);
	const uint32_t want_doff  = ((uint32_t)sl.doorbell_dword << 2) & 0x0FFFFFFCu;
	const bool ok = (g.rb_base == want_base) && (g.rb_base_hi == want_bhi) &&
	                (got_size == want_size) &&
	                ((g.doorbell & 0x10000000u) != 0) &&
	                ((g.doorbell_offset & 0x0FFFFFFCu) == want_doff) &&
	                (g.rb_rptr == 0);
	N48LOG("sdma-map: SDMA%u QUEUE%u read-back RB_CNTL=%#010x (RB_SIZE=%u, want %u) "
	       "RB_BASE=%#010x:%#010x (want %#010x:%#010x) RPTR=%#010x WPTR=%#010x "
	       "DOORBELL=%#010x OFF=%#010x (want %#010x) RPTR_ADDR=%#010x:%#010x "
	       "WPTR_POLL=%#010x:%#010x -> %s",
	       sl.instance, sl.queue, g.rb_cntl, got_size, want_size, g.rb_base_hi,
	       g.rb_base, want_bhi, want_base, g.rb_rptr, g.rb_wptr, g.doorbell,
	       g.doorbell_offset, want_doff, g.rb_rptr_addr_hi, g.rb_rptr_addr_lo,
	       g.wptr_poll_hi, g.wptr_poll_lo, ok ? "MATCHES" : "*** MISMATCH ***");
	if (!ok) {
		N48LOG("sdma-map: SDMA%u QUEUE%u did not read back what we wrote — "
		       "REFUSING to enable. RB_ENABLE stays 0.", sl.instance, sl.queue);
		(void)amdgpu::sdma_disable_external_queue(*gBringup.dev, inst, sl.queue);
		return false;
	}

	// Step 3: HDP flush, then enable.
	if (failStepOut) *failStepOut = 4;
	(void)navi48_hdp_flush_now();
	r = amdgpu::sdma_program_external_queue_ex(
	    *gBringup.dev, inst, ringGpuVa, ringSizeDwords, sl.doorbell_dword,
	    rptrAddr, wptrPollAddr, /*enable_ring=*/true, sl.queue);
	if (r != kIOReturnSuccess) {
		N48LOG("sdma-map: enabling SDMA%u QUEUE%u failed %#x", sl.instance, sl.queue, r);
		(void)amdgpu::sdma_disable_external_queue(*gBringup.dev, inst, sl.queue);
		return false;
	}
	if (failStepOut) *failStepOut = 0;
	N48LOG("sdma-map: SDMA%u QUEUE%u is ENABLED on ring %#llx (%u dwords), doorbell "
	       "dword index %#x. Nothing is rung here — the next writeTail does that.",
	       sl.instance, sl.queue, (unsigned long long)ringGpuVa, ringSizeDwords,
	       sl.doorbell_dword);
	return true;
}

// Program SDMA0 QUEUE1 for somebody else's ring, VERIFY the read-back, and only
// then enable it. The verify is the gate the design memo asks for at its step 4:
// RB_BASE, RB_SIZE, the doorbell enable bit and DOORBELL_OFFSET all have to read
// back as written before one dword can be fetched.
bool navi48_sdma_map_external_ring(uint64_t ringGpuVa, uint32_t ringSizeDwords,
                                   uint64_t rptrAddr, uint64_t wptrPollAddr,
                                   uint32_t doorbellIndex, uint32_t *failStepOut) {
	if (failStepOut) *failStepOut = 1;
	if (!gBringup.gfxhubReady || !gBringup.dev) return false;
	auto &sdma = gBringup.sdma;
	if (!sdma.instance_present[0]) {
		N48LOG("sdma-map: SDMA instance 0 is not present — REFUSING");
		return false;
	}
	auto &inst = sdma.instance[0];
	if (!inst.inited || inst.wb_bus == 0) {
		N48LOG("sdma-map: SDMA0 not inited (inited=%d wb_bus=%#llx) — REFUSING",
		       (int)inst.inited, (unsigned long long)inst.wb_bus);
		return false;
	}
	if (!inst.enabled)
		N48LOG("sdma-map: NOTE — SDMA0 QUEUE0 reads not-enabled. QUEUE1 borrows "
		       "this instance's MCU unhalt and UTCL1/WATCHDOG setup from QUEUE0's "
		       "resume, so this is worth reading in the log above.");

	// Step 1: program with the ring DISABLED. Nothing is fetched yet.
	if (failStepOut) *failStepOut = 2;
	kern_return_t r = amdgpu::sdma_program_external_queue_ex(
	    *gBringup.dev, inst, ringGpuVa, ringSizeDwords, doorbellIndex,
	    rptrAddr, wptrPollAddr, /*enable_ring=*/false);
	if (r != kIOReturnSuccess) {
		N48LOG("sdma-map: sdma_program_external_queue_ex refused %#x "
		       "(ring %#llx, %u dwords) — nothing enabled",
		       r, (unsigned long long)ringGpuVa, ringSizeDwords);
		return false;
	}

	// Step 2: read back and compare against what we asked for.
	if (failStepOut) *failStepOut = 3;
	amdgpu::SDMAQueue1Regs g {};
	amdgpu::sdma_read_external_queue_regs(*gBringup.dev, inst, &g);
	uint32_t want_size = 0;
	while ((1u << want_size) < ringSizeDwords) want_size++;
	const uint32_t got_size   = (g.rb_cntl >> 1) & 0x1Fu;
	const uint32_t want_base  = (uint32_t)(ringGpuVa >> 8);
	const uint32_t want_bhi   = (uint32_t)(ringGpuVa >> 40);
	const uint32_t want_doff  = (doorbellIndex << 2) & 0x0FFFFFFCu;
	const bool ok = (g.rb_base == want_base) && (g.rb_base_hi == want_bhi) &&
	                (got_size == want_size) &&
	                ((g.doorbell & 0x10000000u) != 0) &&
	                ((g.doorbell_offset & 0x0FFFFFFCu) == want_doff) &&
	                (g.rb_rptr == 0);
	// 0.0.187: RB_WPTR is deliberately NOT compared. r13 read 0x10 back after writing 0 -
	// with WPTR polling on, the register tracks the poll word / the last doorbell, not our
	// write - and refusing on it left the queue disabled. It is logged, not judged.
	N48LOG("sdma-map: QUEUE1 read-back RB_CNTL=%#010x (RB_SIZE=%u, want %u) "
	       "RB_BASE=%#010x:%#010x (want %#010x:%#010x) RPTR=%#010x WPTR=%#010x "
	       "DOORBELL=%#010x OFF=%#010x (want %#010x) RPTR_ADDR=%#010x:%#010x "
	       "WPTR_POLL=%#010x:%#010x -> %s",
	       g.rb_cntl, got_size, want_size, g.rb_base_hi, g.rb_base,
	       want_bhi, want_base, g.rb_rptr, g.rb_wptr, g.doorbell,
	       g.doorbell_offset, want_doff, g.rb_rptr_addr_hi, g.rb_rptr_addr_lo,
	       g.wptr_poll_hi, g.wptr_poll_lo, ok ? "MATCHES" : "*** MISMATCH ***");
	if (!ok) {
		N48LOG("sdma-map: the QUEUE1 block did not read back what we wrote — "
		       "REFUSING to enable. RB_ENABLE stays 0 and the engine fetches "
		       "nothing.");
		(void)amdgpu::sdma_disable_external_queue(*gBringup.dev, inst);
		return false;
	}

	// Step 3: HDP flush, then enable. From here the engine will fetch on the
	// next wptr update that reaches it.
	if (failStepOut) *failStepOut = 4;
	(void)navi48_hdp_flush_now();
	r = amdgpu::sdma_program_external_queue_ex(
	    *gBringup.dev, inst, ringGpuVa, ringSizeDwords, doorbellIndex,
	    rptrAddr, wptrPollAddr, /*enable_ring=*/true);
	if (r != kIOReturnSuccess) {
		N48LOG("sdma-map: enabling QUEUE1 failed %#x", r);
		(void)amdgpu::sdma_disable_external_queue(*gBringup.dev, inst);
		return false;
	}
	if (failStepOut) *failStepOut = 0;
	N48LOG("sdma-map: SDMA0 QUEUE1 is ENABLED on ring %#llx (%u dwords), doorbell "
	       "dword index %#x. Nothing is rung here — the next writeTail does that. "
	       "Escape hatch: `accel sdmastate` reports it, and RB_ENABLE=0 stops it.",
	       (unsigned long long)ringGpuVa, ringSizeDwords, doorbellIndex);
	return true;
}

// Read bytes out of a GART-mapped page that somebody else owns.
//
//: the ring's INDIRECT packet points at an IB whose 85 dwords have never
// been decoded, and the engine will FOLLOW it. Unlike the ring there is no
// ring->0x40-style CPU pointer for it (ring->0xd0 is a control block in GART
// page 0, not the IB), so the only way to look is through the physical page the
// PTE names.
//
// Tightly bounded on purpose: one page at a time, read-only (kIODirectionIn),
// and ONLY for an address whose PTE already reads VALID|IS_PTE — so this can
// never be pointed at an arbitrary physical address, only at a page our own
// page table already maps for the GPU.
bool navi48_gart_read_page(uint64_t gartAddr, void *dst, uint32_t len) {
	if (!dst || len == 0 || len > 4096) return false;
	const uint64_t page = gartAddr & ~0xFFFULL;
	const uint32_t off  = (uint32_t)(gartAddr & 0xFFF);
	if (off + len > 4096) return false;            // no cross-page reads

	uint64_t pte = 0;
	if (!navi48_gart_read_pte(page, pte)) return false;
	if (!(pte & 1ULL) || !(pte & (1ULL << 63))) {
		N48LOG("gart-read: page %#llx is not resident (PTE %#llx) — refusing",
		       (unsigned long long)page, (unsigned long long)pte);
		return false;
	}
	const uint64_t phys = (pte & 0xFFFFFFF000ULL) + off;

	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
	    (IOPhysicalAddress)phys, (IOByteCount)len, kIODirectionIn);
	if (!md) { N48LOG("gart-read: withPhysicalAddress(%#llx) failed",
	                  (unsigned long long)phys); return false; }
	bool ok = false;
	if (md->prepare(kIODirectionIn) == kIOReturnSuccess) {
		IOMemoryMap *map = md->map();
		if (map) {
			const void *src = (const void *)map->getVirtualAddress();
			if (src) { memcpy(dst, src, len); ok = true; }
			map->release();
		}
		md->complete(kIODirectionIn);
	}
	md->release();
	if (!ok) N48LOG("gart-read: map/copy failed for %#llx", (unsigned long long)phys);
	return ok;
}

// The VRAM (local memory) aperture as the bring-up driver measured it, NOT as
// anything guessed: vram_start comes from the live MMHUB FB_LOCATION_BASE and is
// never reprogrammed. Needed by the IB rebase, which has to turn an
// address in Apple's VM context into one inside the FB aperture.
//: the BAR0 HOST-PHYSICAL aperture, for code that needs an IOMemoryDescriptor
// over VRAM. navi48_vram_aperture below reports the GPU/MC base (0x8000000000),
// which IOMemoryDescriptor::withPhysicalAddress cannot use. Defined adjacent so the
// two are never confused again. BAR0 is genuinely all of VRAM the CPU can reach on
// this card, so a 256 MiB length is the truth, not a limitation.
bool navi48_bar0_aperture(uint64_t &phys, uint64_t &size) {
	if (!gBringup.dev) return false;
	if (gBringup.dev->bar0Phys == 0 || gBringup.dev->bar0Size == 0) return false;
	phys = gBringup.dev->bar0Phys;
	size = (uint64_t)gBringup.dev->bar0Size;
	return true;
}

bool navi48_vram_aperture(uint64_t &start, uint64_t &size) {
	if (!gBringup.dev) return false;
	if (gBringup.gmc.vram_start == 0 || gBringup.dev->vramSizeBytes == 0) return false;
	start = gBringup.gmc.vram_start;
	size  = gBringup.dev->vramSizeBytes;
	return true;
}

// 0.0.242 — the device-only hi pool, for `vmroots` (52). Reports the same three
// numbers navi48_vram_apple_dest_check decides on, so a reservation proposed in the
// tail ABOVE this pool can be shown to leave bytes_used() at zero and the residency
// copy (and with it milestone 1) untouched. Read-only.
bool navi48_vram_hi_pool(uint64_t &base, uint64_t &size, uint64_t &used) {
	if (!gBringup.dev || !gBringup.gmc.inited) return false;
	auto &g = gBringup.gmc;
	if (!g.vram_alloc_hi.is_inited() || g.vram_hi_size == 0) return false;
	base = g.vram_hi_base;
	size = g.vram_hi_size;
	used = g.vram_alloc_hi.bytes_used();
	return true;
}

// 0.0.244 — the per-VMID TLB / walker-cache invalidate, reachable from src/apple/.
//
// CORRECTION to  and to an internal review note.3, which both say the
// apple/ layer needs a NEW accessor for the HDP flush. It does not, and this file is
// where that is checkable: navi48_hdp_flush_now() is defined above, is declared in
// apple/Navi48Ttl.hpp, and AppleHardwareHook.cpp already calls it. It also does the
// sysmem_wmb() that a freshly written wrapper would have left out. So exactly ONE
// accessor was missing rather than two, and this is it - gmc_flush_gpu_tlb is declared
// in amd/amdgpu_gmc.h, which apple/ does not include.
//
// Engine 17 across the whole address space (hub_program_invalidation arms all 18
// engines with range 0xFFFFFFFF / 0x1F), invalidating L2_PTES + L2_PDE0/1/2 + L1_PTES
// for one VMID, with a bounded 100 ms ACK poll and a clean timeout return. This is
// deliberately NOT Apple's AMDHWVMM::invalidateVM, which funnels into the vtable slot
// patch_vmm replaces with a hook that returns 0 and writes nothing - calling Apple's
// would invalidate nothing at all, silently (review.1).
//
// Nothing calls this yet. It exists because it cannot be discovered at run time and
// must be in place before any mapping of ours is ever pointed at.
bool navi48_gmc_flush_tlb_vmid(uint32_t vmid, uint32_t flush_type) {
	if (!gBringup.dev || !gBringup.gmc.inited) return false;
	if (!gBringup.gmc.gfxhub.inited) return false;
	if (vmid > 15) return false;
	const kern_return_t r = amdgpu::gmc_flush_gpu_tlb(*gBringup.dev, gBringup.gmc,
	                                                  gBringup.gmc.gfxhub, vmid,
	                                                  flush_type);
	if (r != kIOReturnSuccess)
		N48LOG("gmc-flush-tlb: GFXHUB vmid %u type %u returned %#x - the TLB was NOT "
		       "invalidated", vmid, flush_type, r);
	return r == kIOReturnSuccess;
}

// Write up to one page INTO a GART-mapped buffer. Exact mirror of
// navi48_gart_read_page, including its residency refusal — the guard is the point:
// this writes into memory another driver owns, so an unmapped or non-resident page
// must be refused, never faulted on.
bool navi48_gart_write_page(uint64_t gartAddr, const void *src, uint32_t len) {
	if (!src || len == 0 || len > 4096) return false;
	const uint64_t page = gartAddr & ~0xFFFULL;
	const uint32_t off  = (uint32_t)(gartAddr & 0xFFF);
	if (off + len > 4096) return false;            // no cross-page writes

	uint64_t pte = 0;
	if (!navi48_gart_read_pte(page, pte)) return false;
	if (!(pte & 1ULL) || !(pte & (1ULL << 63))) {
		N48LOG("gart-write: page %#llx is not resident (PTE %#llx) — refusing",
		       (unsigned long long)page, (unsigned long long)pte);
		return false;
	}
	const uint64_t phys = (pte & 0xFFFFFFF000ULL) + off;

	IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(
	    (IOPhysicalAddress)phys, (IOByteCount)len, kIODirectionOut);
	if (!md) { N48LOG("gart-write: withPhysicalAddress(%#llx) failed",
	                  (unsigned long long)phys); return false; }
	bool ok = false;
	if (md->prepare(kIODirectionOut) == kIOReturnSuccess) {
		IOMemoryMap *map = md->map();
		if (map) {
			void *dst = (void *)map->getVirtualAddress();
			if (dst) { memcpy(dst, src, len); ok = true; }
			map->release();
		}
		md->complete(kIODirectionOut);
	}
	md->release();
	if (!ok) N48LOG("gart-write: map/copy failed for %#llx", (unsigned long long)phys);
	return ok;
}

bool navi48_gart_aperture(uint64_t &start, uint64_t &size) {
	if (!gBringup.gfxhubReady) return false;          // GART not enabled yet
	if (gBringup.gmc.gart_size == 0) return false;
	start = gBringup.gmc.gart_start;
	size  = gBringup.gmc.gart_size;
	return true;
}

// Publish the option-B' (navi48-pdb0=1) result. `Navi48,PDB0` is a single
// sentence so `ioreg` alone answers "did the alias work"; `Navi48,PDB0Regs`
// carries the seven GFXHUB CONTEXT0 registers the design turns on. Both are
// absent entirely when the boot-arg is off, which is itself the answer.
static void publishPDB0On(IOService *svc, const amdgpu::BringupContext &ctx) {
	const amdgpu::PDB0SelfTest &t = ctx.pdb0Test;
	if (!ctx.gmc.pdb0_want) return;
	char s[192];
	if (!t.ran) {
		snprintf(s, sizeof(s), "off: navi48-pdb0=1 but the PDB0 was not enabled "
		         "(build refused or SDMA not up)");
	} else {
		snprintf(s, sizeof(s),
		         "%s: alias %s, identity %s, gart %s%s",
		         ctx.gmc.pdb0_active ? "on" : "off",
		         t.alias_ok    ? "ok" : "FAILED",
		         t.identity_ok ? "ok" : "FAILED",
		         t.gart_ok     ? "ok" : "FAILED",
		         t.reverted ? " (CONTEXT0 REVERTED to the flat table)" : "");
	}
	svc->setProperty("Navi48,PDB0", s);
	char r[256];
	snprintf(r, sizeof(r),
	         "CNTL(0x1624)=%#010x BASE(0x168f/0x1690)=%#010x:%#010x "
	         "START(0x16af/0x16b0)=%#010x:%#010x END(0x16cf/0x16d0)=%#010x:%#010x "
	         "depth=%u block_size=%u",
	         t.regs[0], t.regs[2], t.regs[1], t.regs[4], t.regs[3],
	         t.regs[6], t.regs[5],
	         (unsigned)((t.regs[0] >> 1) & 0x3u),
	         (unsigned)((t.regs[0] >> 4) & 0xFu));
	svc->setProperty("Navi48,PDB0Regs", r);
	if (t.ran) {
		svc->setProperty("Navi48,PDB0Addr", ctx.gmc.pdb0_mc, 64);
		svc->setProperty("Navi48,PDB0AliasAddr", t.alias_addr, 64);
		svc->setProperty("Navi48,PDB0BadDwords",
		                 static_cast<uint64_t>(t.alias_bad + t.identity_bad + t.gart_bad), 32);
	}
}

// 0.0.185 — `Navi48,SDMAQ1Test` answers ONE question in one sentence, so `ioreg`
// alone settles whether `sdmamap` may be run on this boot: does BAR2 doorbell
// dword 0x202 reach SDMA0 QUEUE1? "MMIO only" means the queue works and the
// doorbell does not route — then the S2A window has to be widened before
// Apple's ring is handed over, and `sdmamap` will refuse.
static void publishSDMAQ1On(IOService *svc, const amdgpu::BringupContext &ctx) {
	if (!ctx.sdmaQ1TestWant) return;
	const amdgpu::SDMAQ1TestResult &t = ctx.sdmaQ1Test;
	char s[224];
	if (!t.ran) {
		snprintf(s, sizeof(s), "skipped: navi48-sdma-q1-test=1 but SDMA0 QUEUE0 was "
		         "not running (or no VRAM for the scratch ring)");
	} else if (t.doorbell_ok) {
		snprintf(s, sizeof(s), "doorbell dword %#x ROUTES to SDMA0 QUEUE1 — fence in "
		         "%llu us (ring %#llx, %u dwords)", t.doorbell_index,
		         (unsigned long long)t.doorbell_us,
		         (unsigned long long)t.ring_gpu_va, t.ring_dwords);
	} else if (t.mmio_ok) {
		snprintf(s, sizeof(s), "MMIO ONLY: QUEUE1 executes (fence in %llu us via "
		         "RB_WPTR) but doorbell dword %#x does NOT route — widen the S2A "
		         "window before sdmamap", (unsigned long long)t.mmio_us,
		         t.doorbell_index);
	} else {
		snprintf(s, sizeof(s), "FAILED: neither the doorbell nor MMIO fenced "
		         "(fence slot read %#010x) — QUEUE1 is not executing; do NOT run "
		         "sdmamap", t.fence_last);
	}
	svc->setProperty("Navi48,SDMAQ1Test", s);
	if (t.ran) {
		char r[192];
		snprintf(r, sizeof(r), "RB_CNTL=%#010x RPTR=%#010x WPTR=%#010x "
		         "doorbell=%#x fence_gpu=%#llx last=%#010x",
		         t.rb_cntl, t.rb_rptr, t.rb_wptr, t.doorbell_index,
		         (unsigned long long)t.fence_gpu, t.fence_last);
		svc->setProperty("Navi48,SDMAQ1TestRegs", r);
	}
}

// 0.0.191 — `Navi48,SRBMTest` answers's open question in one sentence:
// does an SDMA SRBM_WRITE packet change a register on this card, from the ring
// and from an IB, for a harmless scratch register and for the VM-context
// register Apple programs? "ok(apple)" means the form Apple emits (header
// 0xf000000e + the register's DWORD index) worked; "ok(linux)" means only the
// form sdma_v7_0_ring_emit_wreg emits (header 0x0000000e + reg<<2) worked —
// which would mean our `xlatregs` hook has been writing a well-formed packet
// with the wrong operand all along.
static const char *srbmLegWord(const amdgpu::SDMASRBMTargetResult &r,
                               bool ib)
{
	const uint32_t a = ib ? 2u : 0u;
	const uint32_t l = ib ? 3u : 1u;
	if (r.landed[a]) return "ok(apple)";
	if (r.landed[l]) return "ok(linux)";
	if (ib && r.landed[4]) return "ok(plain-hdr)";
	if (!r.mmio_ok)  return "VOID(reg not MMIO-writable)";
	// Distinguish "the engine never ran the packet" from "it ran and the
	// register did not move" — they have completely different causes.
	const bool fenced = r.fenced[a] || r.fenced[l] || (ib && r.fenced[4]);
	return fenced ? "FAILED(fenced, reg unchanged)" : "FAILED(no fence)";
}

static void publishSRBMOn(IOService *svc, const amdgpu::BringupContext &ctx) {
	if (!ctx.srbmTestWant) return;
	const amdgpu::SDMASRBMTestResult &t = ctx.srbmTest;
	char s[320];
	if (!t.ran) {
		snprintf(s, sizeof(s), "skipped: navi48-srbm-test=1 but SDMA0 QUEUE0 was "
		         "not running (or no VRAM for the IB page)");
	} else {
		snprintf(s, sizeof(s),
		         "ring-scratch %s, ib-scratch %s, ring-ctx2 %s, ib-ctx2 %s",
		         srbmLegWord(t.scratch, false), srbmLegWord(t.scratch, true),
		         srbmLegWord(t.ctx2, false),    srbmLegWord(t.ctx2, true));
	}
	svc->setProperty("Navi48,SRBMTest", s);
	if (t.ran) {
		char r[352];
		snprintf(r, sizeof(r),
		         "scratch dw %#x before %#010x want %#010x read ring %#010x/%#010x "
		         "ib %#010x/%#010x/%#010x | ctx2 dw %#x before %#010x want %#010x "
		         "read ring %#010x/%#010x ib %#010x/%#010x/%#010x | IB mc %#llx",
		         t.scratch.reg_dw, t.scratch.before, t.scratch.value,
		         t.scratch.readback[0], t.scratch.readback[1],
		         t.scratch.readback[2], t.scratch.readback[3], t.scratch.readback[4],
		         t.ctx2.reg_dw, t.ctx2.before, t.ctx2.value,
		         t.ctx2.readback[0], t.ctx2.readback[1],
		         t.ctx2.readback[2], t.ctx2.readback[3], t.ctx2.readback[4],
		         (unsigned long long)t.ib_gpu_va);
		svc->setProperty("Navi48,SRBMTestRegs", r);
	}
}

// 0.0.193 — Navi48,VMFragTest. One line that answers INCREMENT 1's question:
// does gfx12's walker accept an Apple-SHAPED tree (DEPTH 1 / BLOCK_SIZE 7,
// 64 KiB L1 entries, 16-entry sub-tables) written in gfx12 ENCODING?
static void publishVMFragOn(IOService *svc, const amdgpu::BringupContext &ctx) {
	if (!ctx.vmfragTestWant) return;
	const amdgpu::VMFragTestResult &t = ctx.vmfragTest;
	char s[320];
	if (!t.ran) {
		snprintf(s, sizeof(s), "skipped: navi48-vmfrag-test=1 but GFXHUB or SDMA0 "
		         "QUEUE0 was not ready (or no VRAM for the table)");
	} else {
		snprintf(s, sizeof(s),
		         "sub-table %s%s (%u dw wrong), 64K leaf %s (%u dw wrong); "
		         "CONTEXT1_CNTL %#010x; last STATUS_LO32 %#010x walker %u",
		         t.sub_ok ? "ok" : "FAILED",
		         t.inverted ? "(inverse-P)" : "", t.sub_bad,
		         t.leaf_ok ? "ok" : "FAILED", t.leaf_bad,
		         t.ctx1_cntl, t.fault_lo, t.fault_walker);
	}
	svc->setProperty("Navi48,VMFragTest", s);
	if (ctx.vmfragNoValidWant) {   // M4-WS-VMID-VALID: leg (c), only with navi48-vmfrag-novalid=1
		char nv[256];
		if (!t.nv_ran)
			snprintf(nv, sizeof(nv), "not run: leg (b) did not pass with VALID, so a bare-base result would mean nothing");
		else
			snprintf(nv, sizeof(nv), "base LO %#010x (bit 0 %s): copy %s (%u dw wrong); STATUS_LO32 %#010x walker %u vmid %u "
			         "ADDR_LO %#010x -> %s", t.nv_base_lo, (t.nv_base_lo & 1u) ? "SET, leg void" : "clear",
			         t.nv_ok ? "ok" : "FAILED", t.nv_bad, t.nv_fault_lo, (t.nv_fault_lo >> 1) & 7u,
			         (t.nv_fault_lo >> 20) & 0xfu, t.nv_fault_addr_lo,
			         t.nv_ok ? "gfx12 IGNORED bit 0" : "gfx12 CHECKS bit 0 (or the leg failed otherwise)");
		svc->setProperty("Navi48,VMFragNoValid", nv);
	}
	if (t.ran) {
		char r[256];
		snprintf(r, sizeof(r), "table mc %#llx, data mc %#llx, VA start %#llx, "
		         "sub-table VA %#llx, 64K leaf VA %#llx",
		         (unsigned long long)t.table_mc, (unsigned long long)t.data_mc,
		         (unsigned long long)t.va_start, (unsigned long long)t.sub_va,
		         (unsigned long long)t.leaf_va);
		svc->setProperty("Navi48,VMFragTestGeom", r);
	}
}

// NATIVE S1b (0.0.600, an internal design note step S1b + review MUST-FIX 4): navi48-native=1 asks for the reserved-VMID VM
// self-test at the END of the ladder. It is REFUSED (logged, nothing native runs) when the Apple accelerator experiment, the boot
// chain, any of the four self-tests (they rewrite CONTEXT1 with RETRY = 1) or the two Apple-only hooks are also armed. Absent (every
// current config) it reads one boot-arg, finds it 0 and does nothing else: no log line, no property, no register. noinline and
// file-static so runStages' frame does not grow.
static __attribute__((noinline)) uint32_t nativeS1bGate(IOService *svc) {
	auto bootArgU32 = [](const char *name) -> uint32_t {
		uint32_t v = 0;
		return PE_parse_boot_argn(name, &v, sizeof(v)) ? v : 0u;
	};
	n48native::GateArgs ga {};
	ga.native = bootArgU32("navi48-native");
	if (ga.native == 0u) return (uint32_t)n48native::kGateOff;
	ga.accelExperiment = bootArgU32("navi48-accel-experiment");
	ga.bootChain       = bootArgU32("navi48-boot-chain");
	ga.sdmaQ1Test      = bootArgU32("navi48-sdma-q1-test");
	ga.sdmaQnTest      = bootArgU32("navi48-sdma-qn-test");
	ga.srbmTest        = bootArgU32("navi48-srbm-test");
	ga.vmfragTest      = bootArgU32("navi48-vmfrag-test");
	ga.eopBridge       = bootArgU32("navi48-eop-bridge");
	ga.shaderCache     = bootArgU32("navi48-shader-cache");
	const uint32_t gate = (uint32_t)n48native::gate_decide(ga);
	amdgpu::native_s1b_set_gate(gate, n48native::gate_conflicts(ga));
	if (gate == n48native::kGateRefused) {
		N48LOG("native-s1b: REFUSED navi48-native=%u: conflicting boot-args armed (mask %#x: 1 accel-experiment, 2 boot-chain, 4 sdma-q1-test, "
		       "8 sdma-qn-test, 0x10 srbm-test, 0x20 vmfrag-test, 0x40 eop-bridge, 0x80 shader-cache). Nothing native runs on this boot.",
		       ga.native, n48native::gate_conflicts(ga));
	} else {
		N48LOG("native-s1b: navi48-native=%u accepted (no Apple-path or self-test boot-arg armed); the VM self-test runs after stage 17", ga.native);
	}
	char nb[384];
	amdgpu::native_s1b_format(nb, sizeof(nb));
	svc->setProperty("Navi48,NativeS1b", nb);
	return gate;
}
static __attribute__((noinline)) void publishNativeS1bOn(IOService *svc) {
	char nb[384];
	amdgpu::native_s1b_format(nb, sizeof(nb));
	svc->setProperty("Navi48,NativeS1b", nb);
}

// 0.0.617 (K5, cosmetic): About This Mac / system_profiler fall back to RDNA4FB's IOFBMemorySize ("VRAM (Total): 14 MB") because Apple's drivers publish "VRAM,totalMB" (32-bit) and "VRAM,totalsize" (8-byte
// OSData, bytes) on the GPU's IOPCIDevice and ours only had "VRAM,TotalMB" (capital T, on the service). Native boots only (called inside the native latch); the old key stays. No register is touched.
static __attribute__((noinline)) void publishNativeVramOn(IOService *pci, uint32_t vramMB) {
	if (!pci || vramMB == 0u || vramMB == 0xFFFFFFFFu) return;
	pci->setProperty("VRAM,totalMB", static_cast<uint64_t>(vramMB), 32);
	const uint64_t bytes = static_cast<uint64_t>(vramMB) << 20;
	OSData *d = OSData::withBytes(&bytes, sizeof(bytes));
	if (d) { pci->setProperty("VRAM,totalsize", d); d->release(); }
	N48LOG("native: published VRAM,totalMB = %u and VRAM,totalsize = %llu on the IOPCIDevice (About This Mac)", vramMB, (unsigned long long)bytes);
}

void Navi48Bringup::runStages(uint32_t target) {
	if (target > (uint32_t)amdgpu::BringupStage::Max) target = (uint32_t)amdgpu::BringupStage::Max;
	amdgpu::BringupContext &ctx = gBringup;
	ctx.dev = &dev;
	ctx.reached = amdgpu::BringupStage::IPDiscovery;     // discovery done by the survey; SOS by stagePSP
	uint32_t full = 0;
	// SMU feature enable is REQUIRED for the GFX core to power up (stage 11+ hardware finding,
	//: default it on for targets >= RLCInit; navi48-smu-basic=1 forces it off.
	uint32_t basic = 0;
	ctx.smuFullSetup = (PE_parse_boot_argn("navi48-smu-full", &full, sizeof(full)) && full != 0) ||
	                   (target >= (uint32_t)amdgpu::BringupStage::RLCInit &&
	                    !(PE_parse_boot_argn("navi48-smu-basic", &basic, sizeof(basic)) && basic != 0));
	uint32_t mnf = 0;
	ctx.mesNonFatal = PE_parse_boot_argn("navi48-mes-nonfatal", &mnf, sizeof(mnf)) && mnf != 0;
	uint32_t legacyRb = 0;
	ctx.cpLegacyRb = PE_parse_boot_argn("navi48-cp-legacy-rb", &legacyRb, sizeof(legacyRb)) && legacyRb != 0;
	// The kernel compute queue is proven as of 0.0.29 (MES maps it, the MEC
	// executes indirect buffers on it, fences land in ~78 us), so it is on by
	// default; navi48-kcq=0 turns it off.
	uint32_t clamp = 1;
	PE_parse_boot_argn("navi48-idle-clamp", &clamp, sizeof(clamp));
	ctx.idleClamp = (clamp != 0);
	uint32_t kcq = 1;
	PE_parse_boot_argn("navi48-kcq", &kcq, sizeof(kcq));
	ctx.wantKcq = (kcq != 0);
	uint32_t hsa = 0;
	ctx.hsaAbi = PE_parse_boot_argn("navi48-hsa-abi", &hsa, sizeof(hsa)) && hsa != 0;
	uint32_t legacyDb = 0;
	ctx.doorbellLegacy = PE_parse_boot_argn("navi48-doorbell-legacy", &legacyDb, sizeof(legacyDb)) && legacyDb != 0;
	dev.doorbell.index.legacy_map = ctx.doorbellLegacy;
	//: move our GART bump region to the top of the aperture so it stops
	// contending with Apple's page 0 (its completion frame). MUST be set before
	// the ladder reaches GMCInit, which is where gmc_mc_init reads it. Off by
	// default — this relocates the CP ring, MQD, WB and MES buffers.
	uint32_t gartHigh = 0;
	ctx.gmc.gart_high_bump =
	    PE_parse_boot_argn("navi48-gart-high", &gartHigh, sizeof(gartHigh)) && gartHigh != 0;
	// navi48-pdb0=1 (0.0.183, option B'): give GFXHUB VMID-0 a two-level page
	// table whose PDB0 aliases MC [0, 16 GiB) onto VRAM physical [0, 16 GiB),
	// so Apple's 0-based VRAM addresses (MQDs at 0x0fffd000, CONST_FILLs at
	// 0x3d6c00000, IB bases) become valid GPU addresses without rewriting
	// them. MMHUB is never touched — the VBIOS scanout goes through it.
	// MUST be set before the ladder reaches CPInit, which is where
	// ensure_gfxhub_gart -> gmc_gfxhub_gart_enable programs CONTEXT0.
	// Default OFF: with this clear, not one byte of behaviour changes.
	uint32_t pdb0 = 0;
	ctx.gmc.pdb0_want =
	    PE_parse_boot_argn("navi48-pdb0", &pdb0, sizeof(pdb0)) && pdb0 != 0;
	if (ctx.gmc.pdb0_want)
		N48LOG("stages: navi48-pdb0=1 — GFXHUB VMID-0 will use the two-level PDB0 "
		       "(option B'); MMHUB stays on the flat table");
	// navi48-sdma-q1-test=1 (0.0.185): at the END of stage 15, prove whether BAR2
	// doorbell dword 0x202 reaches SDMA0 QUEUE1, using OUR OWN 4 KiB ring and one
	// FENCE packet. Open question 1 of notes/re/sdma-takeover-design.md; it must
	// be answered before `sdmamap` points the engine at Apple's ring, because a
	// silent takeover failure and a doorbell that does not route look identical.
	// QUEUE1 is left DISABLED and the scratch ring freed, so this changes nothing
	// a later boot depends on. Default OFF.
	uint32_t q1t = 0;
	ctx.sdmaQ1TestWant =
	    PE_parse_boot_argn("navi48-sdma-q1-test", &q1t, sizeof(q1t)) && q1t != 0;
	if (ctx.sdmaQ1TestWant)
		N48LOG("stages: navi48-sdma-q1-test=1 — the SDMA0 QUEUE1 doorbell-routing "
		       "self-test will run at the end of SDMAInit (doorbell dword 0x%x)",
		       navi48_sdma_q1_doorbell_index());
	// navi48-sdma-qn-test=1 (0.0.196): the same FENCE self-test, once per entry
	// of amdgpu::kSDMAExtSlots — every (instance, queue, doorbell) triple the
	// generalised `sdmamap` may hand Apple. Each leg leaves its queue DISABLED
	// and frees its scratch ring, so a test boot still ends inert. It answers the
	// one thing no amount of reading settles: whether the single routed S2A
	// window (RANGE_OFFSET 0x200, RANGE_SIZE 0x14) reaches SDMA1 as well as
	// SDMA0. Default OFF.
	uint32_t qnt = 0;
	ctx.sdmaQnTestWant =
	    PE_parse_boot_argn("navi48-sdma-qn-test", &qnt, sizeof(qnt)) && qnt != 0;
	if (ctx.sdmaQnTestWant)
		N48LOG("stages: navi48-sdma-qn-test=1 — %u (instance, queue, doorbell) "
		       "triples will be fence-tested at the end of SDMAInit",
		       navi48_sdma_slot_count());
	// navi48-srbm-test=1 (0.0.191): at the very END of stage 15, ask whether an
	// SDMA_OP_SRBM_WRITE packet from OUR SDMA0 QUEUE0 changes a register at all —
	// in the ring and from an INDIRECT_BUFFER, to a free scratch register and to
	// GCVM_CONTEXT2_PAGE_TABLE_BASE_ADDR_LO32. measured Apple's SDMA IB
	// fencing while those very registers stayed zero; until this is settled every
	// VMID-2 theory rests on a packet nobody has seen work on this silicon.
	// Both registers are restored through MMIO afterwards. Default OFF.
	uint32_t srbmt = 0;
	ctx.srbmTestWant =
	    PE_parse_boot_argn("navi48-srbm-test", &srbmt, sizeof(srbmt)) && srbmt != 0;
	if (ctx.srbmTestWant)
		N48LOG("stages: navi48-srbm-test=1 — the SDMA SRBM_WRITE self-test will run "
		       "at the end of SDMAInit (SCRATCH_REG7 and GCVM_CONTEXT2 PT base LO, "
		       "ring and IB, both operand encodings; every leg restores the register)");
	// navi48-vmfrag-test=1 (0.0.193): INCREMENT 1 of the Apple page-table
	// encoding fix. At the very end of stage 15, build an APPLE-SHAPED
	// tree (DEPTH 1 / BLOCK_SIZE 7, a 256 MiB root entry, 64 KiB L1 entries,
	// 16-entry sub-tables) in OUR VRAM using GFX12 ENTRY ENCODING, put it on
	// GFXHUB CONTEXT1 (VMID 1 — nothing else on this machine uses it), and copy
	// through it with SDMA IBs submitted under VMID 1. CONTEXT1's six registers
	// are saved and restored; Apple's arena is never read or written. Default
	// OFF: with this clear the self-test does not run at all.
	uint32_t vmfragt = 0;
	ctx.vmfragTestWant =
	    PE_parse_boot_argn("navi48-vmfrag-test", &vmfragt, sizeof(vmfragt)) && vmfragt != 0;
	if (ctx.vmfragTestWant)
		N48LOG("stages: navi48-vmfrag-test=1 — the Apple-shaped-page-table self-test "
		       "will run at the end of SDMAInit on GFXHUB CONTEXT1/VMID 1 "
		       "(saved and restored); Apple's arena is not touched");
	// M4-WS-VMID-VALID: leg (c) of that self-test - the leaf copy again on a base WITHOUT the VALID bit. A SEPARATE boot-arg, honoured
	// only together with navi48-vmfrag-test; absent (every variant) it is false and the self-test is exactly 0.0.193's.
	uint32_t vmfragnv = 0;
	ctx.vmfragNoValidWant = ctx.vmfragTestWant &&
	    PE_parse_boot_argn("navi48-vmfrag-novalid", &vmfragnv, sizeof(vmfragnv)) && vmfragnv == 1;
	if (ctx.vmfragNoValidWant)
		N48LOG("stages: navi48-vmfrag-novalid=1 — vmfrag leg (c) will copy through CONTEXT1 with its base WITHOUT the "
		       "VALID bit (retry off for that leg; CONTEXT1 restored after)");
	// NATIVE S1b (0.0.600): the gate for navi48-native=1 (default OFF; see nativeS1bGate above). Absent, it reads one boot-arg and returns.
	const uint32_t nativeGate = nativeS1bGate(this);
	IOAccelNavi48NativeClient::latchBootArgs();   // 0.0.612: boot-arg navi48-metal-ws is read ONCE here (default absent = OFF); the N48N open policy uses the latched value
	mapDoorbells();   // NBIO doorbell path init (stage 2) needs the BAR2 physical base
	N48LOG("stages: running ladder to %u (%s); smu-full=%d", target,
	       amdgpu::stage_name((amdgpu::BringupStage)target), ctx.smuFullSetup);
	setProperty("Navi48,StageTarget", static_cast<uint64_t>(target), 32);
	// A previous load of this kext this boot already took the GPU through
	// PSPFwLoad? The marker lives on the IOPCIDevice, which outlives our
	// unload, so a kmutil reload adopts the provisioned GPU instead of asking
	// the PSP to load firmware it already holds (which never fences).
	// Has an earlier load of this kext already run the ladder this boot? The
	// marker lives on the IOPCIDevice, which outlives our unload.
	//
	// REFUSE to run again. Measured: unloading is clean (MES
	// REMOVE_QUEUE, engines halted, buffers freed — the machine carries on),
	// but `kmutil load` afterwards restarts the box every time. Not a panic:
	// the SMC reports shutdown cause 5, a clean OS-initiated restart, with no
	// panic report and nothing in our log past stage 2.
	//
	// What it looks like on the monitor says where it happens: the desktop
	// turns green and purple while staying recognisably the same image, then
	// goes black, then the link drops. That is the display engine scanning the
	// same framebuffer bytes through a changed translation — MMHUB — so the
	// culprit is stage 3 (GMCInit) re-programming the page-table base, system
	// aperture and L2/TLB config of the hub the console is scanning through.
	// Skipping the PSP stages (0.0.20's adopt path) could never have helped.
	// Reproduced at the login window and with a user logged in.
	//
	// The honest fix is a GPU reset before re-initialising, which we cannot do
	// while the display lives on this card. So: one ladder per boot. Iteration
	// happens through the user client, which needs no reload.
	ctx.adoptProvisioned = pciDevice && pciDevice->getProperty("Navi48,ProvisionedThisBoot") != nullptr;
	if (ctx.adoptProvisioned) {
		N48LOG("stages: REFUSING to re-run — this boot already took the GPU through the ladder. "
		       "Re-initialising a live GPU (SMU/IMU/RLC) restarts the machine. Reboot to test a "
		       "new build; use the user client to iterate without one.");
		setProperty("Navi48,LadderSkipped", "already-run-this-boot");
		setProperty("Navi48,StageReached", static_cast<uint64_t>(ctx.reached), 32);
		return;
	}

	kern_return_t r = amdgpu::bringup_to(ctx, (amdgpu::BringupStage)target);
	if (pciDevice && ctx.reached >= amdgpu::BringupStage::PSPFwLoad)
		pciDevice->setProperty("Navi48,ProvisionedThisBoot", true);
	setProperty("Navi48,StageReached", static_cast<uint64_t>(ctx.reached), 32);
	setProperty("Navi48,StageReachedName", amdgpu::stage_name(ctx.reached));
	setProperty("Navi48,StageResult", static_cast<uint64_t>(r), 32);
	if (r != kIOReturnSuccess) setProperty("Navi48,StageFailed", amdgpu::stage_name(ctx.failedAt));
	if (ctx.smuOnline) setProperty("SMU,FirmwareVersion", static_cast<uint64_t>(ctx.smuVersion), 32);
	if (ctx.mesFailed) setProperty("Navi48,MESFailed", true);
	if (ctx.reached >= amdgpu::BringupStage::ComputeDispatch || ctx.failedAt == amdgpu::BringupStage::ComputeDispatch) {
		setProperty("Navi48,ComputeDispatch", ctx.computePassed ? "passed" : "failed");
		setProperty("Navi48,IBTest", ctx.computeResult.ib_test_passed ? "passed" : "failed");
		setProperty("Navi48,ComputeLanesCorrect", static_cast<uint64_t>(ctx.computeResult.lanes_checked - ctx.computeResult.lanes_mismatched), 32);
		setProperty("Navi48,ComputeObserved0", static_cast<uint64_t>(ctx.computeResult.observed[0]), 32);
	}
	if (ctx.reached >= amdgpu::BringupStage::PM4Test || ctx.failedAt == amdgpu::BringupStage::PM4Test) {
		setProperty("Navi48,GFXQueueMappedByMES", ctx.cpLegacyRb ? "legacy-rb" : (ctx.kgqMapped ? "yes" : "no"));
		setProperty("Navi48,CPRingTest", ctx.cpRingTestPassed ? "passed" : "failed");
		setProperty("Navi48,ComputeQueueMapped", ctx.kcqMapped ? "yes" : "no");
		setProperty("Navi48,ComputeQueueTest", ctx.kcqTestPassed ? "passed" : "failed");
		setProperty("Navi48,CPEopTest", ctx.cpEopPassed ? "passed" : "failed");
		setProperty("Navi48,CPWriteDataTest", ctx.cpWriteDataPassed ? "passed" : "failed");
	}
	if (ctx.reached >= amdgpu::BringupStage::SDMAInit) {
		setProperty("Navi48,SDMACopyTest", ctx.sdmaCopyPassed ? "passed" : "failed");
		setProperty("Navi48,SDMACopyMismatched", static_cast<uint64_t>(ctx.sdmaCopy.mismatched), 32);
		// E1 (0.0.418, an internal design note): the SDMA0_DCC_CNTL no-PTE compression clear is the DEFAULT.
		// 0.0.657: the SDMAInit stage already applied it right after `sdma_init_full` (before its own copy test and
		// sweep), so this call is then a logged no-op (gSdmaDccDefaultDone). `navi48-sdmadcc=0` skips it.
		navi48_sdmadcc_default();
	}
	publishPDB0On(this, ctx);
	publishSDMAQ1On(this, ctx);
	publishSRBMOn(this, ctx);
	publishVMFragOn(this, ctx);
	// NATIVE S1b (0.0.600): once, after the ladder, only when the gate accepted navi48-native=1. Every wait inside is bounded at 2 s.
	if (nativeGate == n48native::kGateOn) {
		(void)amdgpu::native_s1b_run(ctx);
		publishNativeS1bOn(this);
		if (amdgpu::native_s1b_latched()) publishNativeVramOn(pciDevice, vramMB);   // 0.0.617 (K5): only on a native boot whose self-test latched
	}
	N48LOG("stages: ladder %s — reached %u (%s), result 0x%x", r == kIOReturnSuccess ? "complete" : "stopped",
	       (unsigned)ctx.reached, amdgpu::stage_name(ctx.reached), r);
}

// ---------------------------------------------------------------------------
// Userspace access
// ---------------------------------------------------------------------------

amdgpu::BringupContext *Navi48Bringup::bringupContext() { return &gBringup; }

void Navi48Bringup::fillCounters(Navi48Counters *out) {
	if (!out) return;
	out->irq_count        = irqCount;
	out->irq_entries      = irqEntries;
	out->irq_eop          = irqEop;
	out->irq_faults       = irqFaults;
	out->irq_cp_errors    = irqCpError;
	out->irq_other        = irqOther;
	out->submits          = submits;
	out->fences_completed = fencesCompleted;
	out->fence_timeouts   = fenceTimeouts;
}

IOReturn Navi48Bringup::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                      OSDictionary *properties, IOUserClient **handler) {
	if (!handler) return kIOReturnBadArgument;
	// NATIVE S1c (0.0.601): type 'N48N' is the native user client; every other type is the legacy Navi48UserClient, exactly as before.
	if (type == N48N_UC_TYPE) return IOAccelNavi48NativeClient::create(this, this, n48native::policy::kRouteBringup, owningTask, securityID, type, properties, handler);   // 0.0.656 (G6): the Navi48Bringup route (WindowServer, root tools); the accelerator route is Navi48MetalNub's native_open
	auto *uc = OSTypeAlloc(Navi48UserClient);
	if (!uc) return kIOReturnNoMemory;
	if (!uc->initWithTask(owningTask, securityID, type, properties)) {
		uc->release(); return kIOReturnNotPermitted;
	}
	if (!uc->attach(this)) { uc->release(); return kIOReturnInternalError; }
	if (!uc->start(this))  { uc->detach(this); uc->release(); return kIOReturnInternalError; }
	*handler = uc;
	return kIOReturnSuccess;
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
//
// The card is a PCIe device with MSI capability; macOS exposes each interrupt
// the device can raise as a numbered source on the IOPCIDevice, and
// getInterruptType() says which of them is message-signalled. We take the
// first messaged source — INTx on a modern Radeon is shared and level
// triggered, and acking it needs register knowledge we would rather not
// depend on during bring-up.
//
// The handler runs on our own work loop (IOInterruptEventSource dispatches
// there, not in primary interrupt context), so draining the ring, reading
// registers and logging are all legal.
//
// Acking: IH v7 rearms through IH_RB_RPTR, which ih_process() already writes
// at the end of every drain (RPTR_REARM is set when enable_cpu_intr is on).
// So "handle" == "drain the ring", and an empty drain is harmless.

void Navi48Bringup::interruptTrampoline(OSObject *owner, IOInterruptEventSource *src, int count) {
	auto *self = OSDynamicCast(Navi48Bringup, owner);
	if (self) self->handleInterrupt(count);
	(void)src;
}

void Navi48Bringup::handleInterrupt(int count) {
	irqCount++;
	amdgpu::BringupContext &ctx = gBringup;
	if (!ctx.ih.inited) return;

	amdgpu::IHEntry e[32];
	uint32_t total = 0;
	// MILESTONE 3 step 1: did this drain see an end-of-pipe entry? If so, and if
	// the bridge is armed, Apple's own retire runs once after the drain (not once
	// per entry — checkTimestamps walks every channel, so one call covers them
	// all). r38 measured EOP as client 0x14 / src 0xb5 (181); the client id is
	// recorded rather than used as a gate, so a different client still fires the
	// bridge and the log says which one it was.
	bool sawEop = false;
	uint32_t eopClient = 0;
	// 0.0.270: the SDMA trap. live1 saw one `client 10 src 49 ring 1` entry per drained SDMA frame.
	bool sawTrap = false;
	uint32_t trapClient = 0;
	// Drain until the ring reports empty; a single MSI can cover many entries.
	for (int pass = 0; pass < 16; pass++) {
		uint32_t n = amdgpu::ih_poll(dev, ctx.ih, e, 32, 0);
		if (n == 0) break;
		total += n;
		for (uint32_t i = 0; i < n; i++) {
			// DCE (display) entries first, by CLIENT id: the DCN source ids overlap nothing in the
			// switch below, but a DCE entry must be recognised by client AND source together
			// (dcn41_ih_to_irq returns NONE for any other client). n48dcn classifies it, acknowledges
			// it the way amdgpu_dm_irq_handler does — ack before anything else — counts it by kind and
			// instance, and keeps its own small log budget, so these never land in irqOther.
			if (n48dcn::ihEntry(e[i].client_id, e[i].src_id, e[i].src_data[0])) continue;
			switch (e[i].src_id) {
			case 181: irqEop++;
				sawEop = true; eopClient = e[i].client_id;
				break;   // CP end-of-pipe
			case 49:  irqOther++;
				if (e[i].client_id == 0x0a) { sawTrap = true; trapClient = e[i].client_id; }
				break;   // SDMA trap (SUSPECTED id, ): counted as before, and bridged below
			case 0:   irqFaults++;  break;   // UTCL2 / VM fault
			case 183: case 184: case 185: irqCpError++; break;
			default:  irqOther++;   break;
			}
			// 0.0.385 — src 0 (UTCL2 / VM fault) gets its OWN budget and its OWN line.
			// Class A's walk is 43 entries on arm5/7/8/9 and 102 on arm6; the shared 32 above could
			// not carry one whole walk, and the two-dword line dropped src_data[2..3] (the element
			// count) and the timestamp, which is how the stride and the interleave have to be read.
			// dw1-3 are RECONSTRUCTED from the decode (amdgpu_ih.h: DW1 = timestamp_lo, DW2 =
			// timestamp_hi in [15:0], DW3 = pasid | node_id << 16). ih_v7_0.cpp's decoder does not
			// keep DW2 bit 31 (timestamp_src) or DW2[30:16], so dw2 here is the timestamp's high
			// half only - stated rather than implied, because a reconstructed dword that silently
			// differs from the wire is exactly the class of instrument this project has been burned by.
			if (e[i].src_id == 0) {
				if (irqSrc0LogBudget > 0) {
					irqSrc0LogBudget--;
					const uint32_t dw1 = (uint32_t)(e[i].timestamp & 0xFFFFFFFFull);
					const uint32_t dw2 = (uint32_t)((e[i].timestamp >> 32) & 0xFFFFull);
					const uint32_t dw3 = (uint32_t)e[i].pasid | ((uint32_t)e[i].node_id << 16);
					N48LOG("irq: src0 fault client %u ring %u vmid %u vmidsrc %u data %08x %08x %08x %08x; "
					       "dw1 %08x dw2 %08x dw3 %08x (ts %#llx, pasid %#x, node %u)",
					       e[i].client_id, e[i].ring_id, e[i].vmid, e[i].vmid_src,
					       e[i].src_data[0], e[i].src_data[1], e[i].src_data[2], e[i].src_data[3],
					       dw1, dw2, dw3, (unsigned long long)e[i].timestamp, e[i].pasid, e[i].node_id);
				}
			} else if (irqLogBudget > 0) {
				irqLogBudget--;
				N48LOG("irq: client %u src %u ring %u vmid %u data %08x %08x",
				       e[i].client_id, e[i].src_id, e[i].ring_id, e[i].vmid,
				       e[i].src_data[0], e[i].src_data[1]);
			}
		}
	}
	irqEntries += total;
	// The completion bridge (0.0.235). Inert unless armed; it refuses on its own
	// guards if Apple's objects are not there, and it is capped per boot. This
	// runs on our work loop, not in primary interrupt context, so calling into
	// Apple's scheduler here is legal.
	if (sawEop) n48::hw_hook_eop_bridge_fire(eopClient);
	else if (sawTrap) n48::hw_hook_sdma_trap_bridge_fire(trapClient);   // one checkTimestamps covers every channel
	(void)count;
}

bool Navi48Bringup::armInterrupts() {
	if (intArmed) return true;
	if (!pciDevice) return false;

	int msiIndex = -1;
	for (int i = 0; i < 8; i++) {
		int type = 0;
		if (pciDevice->getInterruptType(i, &type) != kIOReturnSuccess) break;
		N48LOG("irq: source %d type %#x%s", i, type,
		       (type & kIOInterruptTypePCIMessaged) ? " (message-signalled)" : " (level/INTx)");
		if (type & kIOInterruptTypePCIMessaged) { msiIndex = i; break; }
	}
	if (msiIndex < 0) {
		N48LOG("irq: no message-signalled interrupt source — staying on the polled path");
		return false;
	}

	workLoop = IOWorkLoop::workLoop();
	if (!workLoop) { N48LOG("irq: no work loop"); return false; }
	intSource = IOInterruptEventSource::interruptEventSource(
		this, &Navi48Bringup::interruptTrampoline, pciDevice, msiIndex);
	if (!intSource) {
		N48LOG("irq: interruptEventSource(index %d) failed", msiIndex);
		workLoop->release(); workLoop = nullptr; return false;
	}
	if (workLoop->addEventSource(intSource) != kIOReturnSuccess) {
		N48LOG("irq: addEventSource failed");
		intSource->release(); intSource = nullptr;
		workLoop->release(); workLoop = nullptr; return false;
	}
	intSource->enable();
	intArmed = true;
	irqLogBudget = 32;
	irqSrc0LogBudget = kIrqSrc0LogLines;   // 0.0.385: src-0 entries get their own, wider budget
	N48LOG("irq: armed on MSI source %d — IH ring will run with ENABLE_INTR=1", msiIndex);
	// A SEPARATE line, deliberately: the line above is 0.0.384's verbatim, so nothing that reads it moves.
	N48LOG("irq: src-0 (UTCL2 / VM fault) entries have their OWN log budget of %u line(s) this boot and print "
	       "dw1-3 and src_data[2..3] (: class A's walk is 43-102 entries and the shared 32 could not "
	       "carry one). Worst case %u extra line(s) for the whole boot; a fault-free boot prints none of them.",
	       irqSrc0LogBudget, kIrqSrc0LogLines);
	return true;
}

void Navi48Bringup::disarmInterrupts() {
	if (intSource) {
		intSource->disable();
		if (workLoop) workLoop->removeEventSource(intSource);
		intSource->release(); intSource = nullptr;
	}
	if (workLoop) { workLoop->release(); workLoop = nullptr; }
	intArmed = false;
}

void Navi48Bringup::publishInterruptCounters() {
	if (!intArmed) return;
	setProperty("Navi48,IRQArmed", true);
	setProperty("Navi48,IRQCount", irqCount, 64);
	setProperty("Navi48,IRQEntries", irqEntries, 64);
	setProperty("Navi48,IRQEop", irqEop, 64);
	setProperty("Navi48,IRQFaults", irqFaults, 64);
	setProperty("Navi48,IRQCPErrors", irqCpError, 64);
	setProperty("Navi48,IRQOther", irqOther, 64);
	N48LOG("irq: %llu interrupts, %llu entries (eop %llu, faults %llu, cp-errors %llu, other %llu)",
	       irqCount, irqEntries, irqEop, irqFaults, irqCpError, irqOther);
}

// ---------------------------------------------------------------------------
// IOService lifecycle
// ---------------------------------------------------------------------------

IOService *Navi48Bringup::probe(IOService *provider, SInt32 *score) {
	uint32_t on = 0;
	if (!PE_parse_boot_argn("navi48bringup", &on, sizeof(on)) || on == 0) return nullptr;
	if (!super::probe(provider, score)) return nullptr;
	auto *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) return nullptr;
	uint32_t vendorDevice = pci->configRead32(kIOPCIConfigVendorID);
	uint16_t device = static_cast<uint16_t>(vendorDevice >> 16);
	if ((vendorDevice & 0xffff) != 0x1002 || (device != 0x7550 && device != 0x7551)) {
		N48LOG("probe: 0x%08x not Navi 48, ignoring", vendorDevice); return nullptr;
	}
	N48LOG("probe: Navi 48 %s (0x%04x) — bring-up enabled", device == 0x7550 ? "RX 9070/XT" : "R9700", device);
	return this;
}

bool Navi48Bringup::start(IOService *provider) {
	if (!super::start(provider)) return false;
	if (!gVramMmLock) gVramMmLock = IOLockAlloc();   // never freed: a verb may hold it at stop
	if (!gScanoutLock) gScanoutLock = IOLockAlloc(); // 0.0.272: never freed, same reason
	if (!gFmLock) gFmLock = IOLockAlloc();           // build 0.0.518 (switch 74, flip mode): never freed, same reason
	if (!gSfLock) gSfLock = IOLockAlloc();           // build 0.0.542 (`scanout full`'s published capture): never freed, same reason
	if (!amdgpu::gTlb83Lock) amdgpu::gTlb83Lock = IOLockAlloc();   // build 0.0.528 (switch 83's leaf lock): never freed, same reason
	pciDevice = OSDynamicCast(IOPCIDevice, provider);
	gBringupPci = pciDevice;
	gBringupSelf = this;
	navi48_fbwc_boot();   // 0.0.286: boot-arg navi48-fbwc=1 only; otherwise returns at once
	// 0.0.386: the host-page range guard's RAM top, derived once from the EFI memory map. HERE because it
	// must run exactly once, single-threaded, before any hook exists and therefore long before gfxc_read can be called; it
	// reads boot_args only, touches no register, and on every failure path leaves the guard at 0.0.385's constant.
	n48::hw_hook_ramtop_init();
	if (!pciDevice) { N48LOG("start: no PCI provider"); return false; }
	pciDevice->setMemoryEnable(true);
	latchPciBars(pciDevice);   // 0.0.612 (review item A): every BAR's FULL base and size, once, for BoImportHost's device-page refusal

	if (!mapRegisters()) { N48LOG("start: BAR5 unavailable — aborting"); return false; }
	if (!probeMemSize())  { N48LOG("start: MMIO not usable — aborting"); unmapRegisters(); return false; }
	if (!loadOnDieDiscovery()) {
		N48LOG("start: IP discovery unavailable — MMIO works, but no register map");
	} else {
		surveyIPs();
		surveyPSP();
		surveySMU();
		if (!PE_parse_boot_argn("navi48-stage", &targetStage, sizeof(targetStage))) targetStage = 0;
		if (targetStage) N48LOG("stages: navi48-stage=%u (mac-amdgpu BringupStage numbering; 5 = PSP SOS)", targetStage);
		// Arm the host side before the ladder so stage 2 (IHInit) can bring the
		// ring up with ENABLE_INTR=1. The GPU cannot assert until then.
		// On by default as of 0.0.35: proven on hardware — the MSI
		// source arms, the IH ring runs with ENABLE_INTR=1, and the handler
		// drains and classifies (6 interrupts / 7 entries / 4 EOP / 3 faults on
		// a stage-17 run) with no storm. navi48-interrupts=0 falls back to the
		// polled path, which is unchanged.
		uint32_t wantIrq = 1;
		PE_parse_boot_argn("navi48-interrupts", &wantIrq, sizeof(wantIrq));
		if (wantIrq != 0) gBringup.wantInterrupts = armInterrupts();
		stagePSP();        // no-op unless navi48-psp=1 or navi48-stage>=5
		publishInterruptCounters();
	}
	// build 0.0.515: the read-only raster device the AGDC LINKCFG reply reads through
	// (n48dcn::liveRaster), built here - after stagePSP built the device context, before the Phase 4 block below can install
	// any hook that calls it. No register is read or written by attach; it arms nothing (bind() still does that, on verbs 74-77).
	n48dcn::attach(this);
	// ---- Phase 4 route experiment (navi48-accel-experiment=1) ----
	// Apple's accelerator personalities all carry IOPropertyMatch
	// { LoadAccelerator }, so the accelerator only attaches once something
	// declares the card ready — normally Apple's own framebuffer/wrangler,
	// which cannot drive DCN 4.0.1. Publishing the property ourselves is the
	// entire entry condition, and our Info.plist carries a second personality
	// offering AMDRadeonX6000_AMDNavi21GraphicsAccelerator our device ID.
	//
	// What this is for: measuring how far Apple's generation-agnostic
	// accelerator code (92 of its 131 classes) gets on GFX12 hardware before it
	// touches something GFX10-specific. See notes/PHASE4-ROUTES.md. It is
	// expected to fail — the question is WHERE.
	//
	// Off by default: Apple's driver will program this card believing it is a
	// Navi 21, and the display is on the same card.
	{
		uint32_t accelExp = 0;
		accelExperimentArmed = PE_parse_boot_argn("navi48-accel-experiment", &accelExp,
		                                          sizeof(accelExp)) && accelExp != 0;
		if (accelExperimentArmed) {
			// ARMED, NOT FIRED. Everything this experiment does now happens on
			// demand through the user client (`navi48test accel fire`).
			//
			// Firing at boot was the original design and it is unsafe unattended:
			// Apple's accelerator drives real hardware through our TTL, and if that
			// panics, a boot-time trigger panics again on every subsequent boot.
			// Recovery needs someone at the OpenCore picker. Fired from userspace
			// instead, the worst case is one reboot back to a clean machine.
			N48LOG("accel-experiment: ARMED (not fired). Run `navi48test accel fire` "
			       "to install the TTL hook and let Apple's accelerator match.");
			setProperty("Navi48,AccelExperiment", "armed — fire with navi48test accel fire");
		}
		// MILESTONE 3 step 1 (0.0.235): the IH -> Apple completion bridge can be
		// armed at boot instead of by a verb, which is what makes it part of a
		// self-driving driver rather than an instrument. Arming is only a flag —
		// the bridge resolves Apple's scheduler at fire time and refuses on its
		// own guards until the accelerator exists — so this is safe at start().
		// Default OFF: a boot without the arg behaves exactly as 0.0.234.
		// MILESTONE 3 step 2 (0.0.236): announce the exposure gate at start(), so a
		// boot log says what `fire` will decide before anything fires. The decision
		// itself is made in the fire path, where the self-test result is final.
		{
			uint32_t gate = 1;
			PE_parse_boot_argn("navi48-accel-gate", &gate, sizeof(gate));
			static const char *gateWhy[] = {
				"closed: LoadAccelerator will NEVER be published",
				"self-test: LoadAccelerator only after the stage-17 compute dispatch passes",
				"forced open: LoadAccelerator published without requiring the self-test",
			};
			N48LOG("accel-gate: navi48-accel-gate=%u — %s", gate,
			       gate <= 2 ? gateWhy[gate] : "unknown mode, treated as self-test");
			setProperty("Navi48,AccelGateMode", static_cast<uint64_t>(gate), 32);
		}
		// MILESTONE 3 step 3 (0.0.237): the boot chain. A bitmask, default 0 = OFF,
		// so a boot that does not ask for it behaves exactly as 0.0.236 did and
		// every verb keeps working for diagnosis.
		{
			uint32_t chain = 0;
			PE_parse_boot_argn("navi48-boot-chain", &chain, sizeof(chain));
			n48::hw_hook_boot_chain_configure(chain);
			if (chain) {
				const uint64_t upUs = n48::hw_hook_uptime_us();
				N48LOG("boot-chain: [up %llu.%06llu s] navi48-boot-chain=%#x — %s%s%s%s%s. The accelerator-start "
				       "sequence will run in-kext with no ssh verb.", (unsigned long long)(upUs / 1000000ull),
				       (unsigned long long)(upUs % 1000000ull), chain,
				       (chain & 1) ? "phase A (accelerator-start chain)" : "phase A OFF",
				       (chain & 2) ? " + phase B (submission takeover)" : "",
				       (chain & 8) ? " + ringmap at arming (0.0.269)" : "",
				       (chain & 4) ? " + THE DRAIN (0.0.269: every SDMA submission translated before its doorbell)" : "",
				       (chain & 0x20) ? " + THE CONTEXT LATCH (no client VM context until every protection is live)" : "");
				setProperty("Navi48,BootChainMode", static_cast<uint64_t>(chain), 32);
			}
		}
		// MILESTONE 3 step 2 (0.0.239): the shader cache. Arming is only a flag — the
		// blob is opened and validated on the first arm, and the substitution itself
		// cannot run until a residency copy happens — so this is safe at start().
		// Default OFF: a boot without the arg behaves exactly as 0.0.238.
		uint32_t shaderCache = 0;
		if (PE_parse_boot_argn("navi48-shader-cache", &shaderCache, sizeof(shaderCache)) && shaderCache) {
			navi48_shadercache_control(1, nullptr, 0);
			N48LOG("accel-experiment: navi48-shader-cache=1 — the hash-keyed shader cache is ARMED "
			       "from start(); every residency copy will be scanned and substituted.");
			setProperty("Navi48,ShaderCache", "armed-at-boot");
		}
		uint32_t eopBridge = 0;
		if (PE_parse_boot_argn("navi48-eop-bridge", &eopBridge, sizeof(eopBridge)) && eopBridge) {
			n48::hw_hook_eop_bridge_control(1, nullptr, 0);
			N48LOG("accel-experiment: navi48-eop-bridge=1 — the completion bridge is ARMED "
			       "from start(); end-of-pipe interrupts will drive Apple's own retire.");
			setProperty("Navi48,EopBridge", "armed-at-boot");
		}
	}

	N48LOG("start: done (VRAM %u MiB, %u IPs)", vramMB, ipDiscovery.isValid() ? ipDiscovery.ipCount() : 0);
	if (!getProperty("PSP,Stage")) setProperty("Navi48,Stage", "survey-readonly");
	registerService();
	return true;
}


// Phase 4 accelerator experiment, fired on demand from the user client.
//
// Two steps, in this order and only in this order:
//   1. Redirect HWServices::getTtl() to our Navi48Ttl.
//   2. Publish LoadAccelerator so Apple's accelerator personality can match.
// If step 1 fails we must not do step 2. Apple's accelerator running against
// Apple's own TTL cannot initialise on gfx1201; it hangs inside start(), the
// graphics node never goes quiet, and the machine sits at the verbose console
// instead of reaching a login window. Measured.
IOReturn Navi48Bringup::accelExperiment(uint32_t action, uint64_t *outInstalled,
                                        uint64_t *outTtlCalls, uint64_t *outFirstUnsupported,
                                        uint64_t *outExtra, uint64_t argScalar) {
	if (outExtra)
		for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = 0;
	if (outInstalled)       *outInstalled = accelExperimentFired ? 1 : 0;
	if (outTtlCalls)        *outTtlCalls = gNavi48Ttl.totalCalls();
	if (outFirstUnsupported) *outFirstUnsupported = (uint64_t)(int64_t)gNavi48Ttl.firstUnsupported();

	// NATIVE S1b (0.0.600): after the native VM self-test ran on this boot every accel verb but the read-only action 0 refuses
	// (the self-test left a user VMID and live page tables behind; Apple's driver must not run on top of them).
	// NATIVE S2a (0.0.603, decision D-S2-2): EXACTLY three (action, arg) pairs are exempt, placed BEFORE that refusal - dcnstate 0 (read-only),
	// dcnflip 0 (the console restore), dcnmode 0 (read-only, restores the boot capture). They touch DCN only, never GFX/VM, which is what
	// the refusal protects. The table is n48scan::accel_exempt (host-tested, incl. that dcnflip 1002 and dcnmode 1 stay refused).
	// 0.0.613 (#11 11h.2, B5): with boot-arg navi48-metal-disp=1 latched, fbname 0|1 and the five display verbs (83..87, each with its legal argument) are ALSO exempt: they touch no GFX/VM
	// state and no register (n48disp::native_exempt, host-tested: with the latch OFF it is false for everything, so this line is 0.0.612's).
	// action 100..102 (0.0.623, GPU-apps G1, an internal design note section 4; boot-arg navi48-g1=1 only): `hangtest 1` / `hangrecover <0..4>` / `hangstat [0..8]`.
	// Dispatched BEFORE the native-boot refusal below, as its own exemption: with the latch ON exactly n48native::g1::native_exempt's (action, argument) pairs run,
	// and they act only on the native kernel GFX queue and a kernel-owned native test context (amdgpu::n1c_g1_verb, amd/native_s1c.cpp), never on Apple's driver
	// state. With the latch OFF (the default) 100..102 are BadArgument here and in the user client, as before 0.0.623. o[0] is the verdict code.
	if (n48native::g1::is_g1_action(action)) {
		if (!n48native::g1::native_exempt(amdgpu::n1c_g1_latched_on(), action, argScalar)) return kIOReturnBadArgument;
		uint64_t v[n48native::g1::kOutN] = { 0 };
		const IOReturn kr = amdgpu::n1c_g1_verb(action, argScalar, gBringup, v);
		N48LOG("accel-experiment: %s %llu -> kr %#x code %llu", action == n48native::g1::kActHangTest ? "hangtest" : action == n48native::g1::kActHangRecover ? "hangrecover" : "hangstat",
		       (unsigned long long)argScalar, kr, (unsigned long long)v[0]);
		if (outExtra)
			for (unsigned i = 0; i < n48native::g1::kOutN && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	// action 103 (0.0.627, GPU-apps G2; boot-arg navi48-multisession=1 only): `sessstat <0..4>`, READ-ONLY: page 0..3 one native session slot (VMID 8..11), page 4 the
	// global page (open sessions, DEAD slots, seqnos, the shared VRAM budget, automatic recoveries). Like G1, dispatched before the native-boot refusal as its own
	// exemption (n48native::g2::native_exempt); with the latch OFF (the default) 103 is BadArgument here and in the user client, as before 0.0.627.
	// 0.0.650 (G5 Stage 1): page 5 is the application-credit page; it exists only with boot-arg navi48-g5=1 (and multisession) latched, else BadArgument as before.
	if (n48native::g2::is_g2_action(action)) {
		const bool g5page = n48native::g5::args_ok(amdgpu::n1c_g5_latched_on(), action, argScalar);
		if (!g5page && !n48native::g2::native_exempt(amdgpu::n1c_multi_latched_on(), action, argScalar)) return kIOReturnBadArgument;
		uint64_t v[n48native::g2::kOutN] = { 0 };
		const IOReturn kr = g5page ? amdgpu::n1c_g5_stat(v) : amdgpu::n1c_g2_stat(argScalar, v);
		N48LOG("accel-experiment: sessstat %llu -> kr %#x code %llu", (unsigned long long)argScalar, kr, (unsigned long long)v[0]);
		if (outExtra)
			for (unsigned i = 0; i < n48native::g2::kOutN && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	// action 104 (0.0.640, GPU-apps G4; boot-args navi48-apps=1 AND navi48-multisession=1 only): `appallow add|remove <name>` / `appallow list [page]`, the kernel's APP allow-list
	// (max 32 entries, empty at every boot). Reachable only through the admin-only legacy user client. It touches no hardware and no register: a 32-slot table in the kext's
	// data. Like G1 / G2, dispatched before the native-boot refusal as its own exemption (n48native::g4::native_exempt); with the role disabled (the default) 104 is BadArgument here
	// and in the user client, as before 0.0.640. o[0] is the code (n48native::g4::AllowCode).
	if (n48native::g4::is_g4_action(action)) {
		if (!n48native::g4::native_exempt(amdgpu::n1c_apps_enabled(), action, argScalar)) return kIOReturnBadArgument;
		uint64_t v[n48native::g4::kOutN] = { 0 };
		const IOReturn kr = amdgpu::n1c_app_verb(argScalar, v);
		N48LOG("accel-experiment: appallow op %llu -> kr %#x code %llu entries %llu", (unsigned long long)v[2], kr, (unsigned long long)v[0], (unsigned long long)v[1]);
		if (outExtra)
			for (unsigned i = 0; i < n48native::g4::kOutN && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kr;
	}
	if (!n48scan::accel_exempt(action, argScalar) && !n48disp::native_exempt(n48disp_latched_on(), action, argScalar)) {
	if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;
	}
	// 0.0.613: the action bound again, here (the user client checks it first): 83..87 exist only with the latch ON.
	if (action > n48disp::kLastOldAction && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;
	if (action == 0) {
		// - THE COMPLETION BLIND SPOT. `Navi48,IRQEop` / `Navi48,IRQFaults`
		// (publishInterruptCounters, :3624-3625) were published exactly ONCE, from
		// start() at :3684, so every ioreg read of them - including accel-run.sh's
		// post-run health.txt - returned the BOOT-TIME snapshot. That is why arm5
		// could not answer "did the committed IB complete": the irq: log budget was
		// spent by the fault burst and the counters never moved in ioreg.
		// Re-publishing here makes `accel status` a sampling point, so a pre/post
		// pair of reads gives a real delta. The call is idempotent, returns at once
		// when interrupts were never armed, and touches no hardware - it only calls
		// setProperty on values the IH handler already maintains.
		publishInterruptCounters();
		return kIOReturnSuccess;              // status only
	}

	// action 2 — re-drive AMDHardware::setMemoryAllocationsEnabled(true).
	//
	// Measured with DTrace on an earlier run, not inferred: that cascade is what
	// builds AMDHWVMM's page-table block allocators at vmm->0x78 and ->0x80.
	// On this card it returns early and leaves both NULL, because the 68 MiB
	// (0x4400000) VRAM allocation it makes first — stored at vmm->0x58 — comes
	// back NULL. Everything downstream follows mechanically:
	//
	//   allocVMBlock()            false   (its first test is `if (!this->0x78)`)
	//   mapVA()                   false
	//   commitIntoGPUPageTable()  false
	//   BatchPrepare / SegmentResourceList::prepare()   false, three times
	//   IOAccelCommandQueue::coalesceSegment   movl $0xe00002bd, 0x610(queue)
	//
	// and submitCommands() then finds no channel and returns having called
	// nothing. That is why every "Commands Submitted" counter reads zero: not a
	// submit bug at all, but a virtual-memory system that was never armed.
	//
	// The cascade normally runs exactly once, deep inside Apple's accelerator
	// start(), which is far too early to have a tracer attached — the kext is
	// not even loaded when the trace would have to begin. This makes it callable
	// on demand so the failing allocation can be watched directly.
	// action 4 — enable Apple's SDMA rings.
	//
	// traced the complete reason nothing the accelerator builds ever
	// executes, and it is one flag. AMDGFX10SDMARing::enable() is never called,
	// so ring->0xa8 stays 0, so the gate at the top of every
	// AMDHWChannel::submitCommandBuffer (`ring->vtbl[0x140]()`, which is just
	// `return ring->0xa8 & 1`) is false and all 17 submits bail before writing a
	// single dword. The ring stays 64 KiB of zeros and the GPU has nothing to
	// fetch. enable()'s only caller is AMDGFX10SDMAEngine::start(), which is
	// reached solely through a vtable and demonstrably never runs.
	//
	// enable() is idempotent and, with the register shadow armed, reaches no
	// hardware — see hw_hook_enable_sdma_rings for the safety gate it applies
	// before calling anything.
	// action 5 — run Apple's own startHWEngines.
	//
	// 0.0.91 showed that enabling the ring makes Apple's entire submit path
	// execute: submitCommandBuffer stopped bailing and ran through
	// commitIndirectCommandBuffer -> RTRing::submit -> RTRing::writeTail, the
	// three functions measured at 0 calls. It panicked in writeTail on a NULL
	// ring->0xc0 (the doorbell), which SDMAEngine::start() is what fills.
	// startHWEngines is the caller of that, so run it rather than reimplement it.
	// action 19 —: probe the completion slot, then supply the fence value.
	// action 28 —: translate Apple's GCVM register operands by +0x28.
	// action 29 —: does SDMA's SRBM_WRITE write a register at all?
	// action 30 —: un-pause the SW scheduler (Apple's own resume()).
	// action 32 —: install the virtual-space descriptor (sets mem+0x98 bit 0).
	// action 50 — `bootchain` (0.0.237, MILESTONE 3 step 3).
	//   Read-only: what the in-kext boot chain did this boot. State 0 idle (not
	//   armed), 1 running, 2 done, 3 failed; plus the per-step results and the
	//   step it failed at, so a refusal is diagnosable without the driver log.
	//   The boot-arg navi48-boot-chain is the PRODUCTION mechanism and sets the
	//   mode at start(). argScalar is the TEST lever, and it exists for the same
	//   reason `fire 1|2` does: the mode must be set before `fire`, because the
	//   chain arms from Apple's accelerator-started callback, which `fire` is what
	//   triggers. Setting it here is only writing our own mode word — it starts
	//   nothing by itself. Sent with no argument (or 0) this verb is read-only.
	// action 57 — `pairing [1|2]` (0.0.267). The display-pairing stamp
	//   (IOAccelTypes/IOAccelIndex/IOAccelRevision on RDNA4FB, 0.0.241) is now OPT-IN:
	//   found WindowServer uses that lookup only when a WindowServer process
	//   initialises its displays, and P1 measured that login and logout never restart it,
	//   so the stamp is inert normally and hazardous exactly when WindowServer crashes on an
	//   armed boot. 0 reads; 1 enables for this boot and must precede `fire` (refused after
	//   the accelerator-started decision); 2 disables or withdraws our stamped keys. Every
	//   verdict except a bad argument returns SUCCESS with the scalars, so a refusal is
	//   readable from the return path (rule 27); navi48test exits non-zero on a refusal.
	//   Registry edits only: no hardware register, no Apple object.
	// action 58 — `drain` (0.0.269). READ-ONLY: the drain's state and counters. The drain itself is
	//   armed by boot-chain bit 2 at the end of phase A (navi48-boot-chain); this verb never arms, maps or writes.
	// action 59 — `flushhook [1|2|3]` (0.0.272): the per-surface externalMethod hook of route c' wall 3.
	//   0 reads, 1 log-only, 3 log + copy into the scanout, 2 pass-through. Defined in Navi48AccelPeer.cpp.
	// action 60 — `scanout [0|1|2]` (0.0.272): 0 geometry read, 1 the positive-control copy with BAR0 readback,
	//   2 restore the rectangle it overwrote. Defined above.
	// action 82 — `sdmadcc [0|1|2]` (0.0.417, an internal design note): SDMA0_DCC_CNTL's no-PTE read
	//   decompression / write compression. 0 reads SDMA0+SDMA1 raw and decoded, 1 captures-then-clears only the
	//   eight *_COMP_EN_n bits on SDMA0, 2 restores the captured value; other arguments (and 2 before a capture)
	//   are REFUSED. SDMA1 is never written. Defined above.
	// action 61 — `gfxcensus [1|2]` (0.0.276): READ-ONLY census of Apple's GFX frames and their IBs at the GFX-ring
	//   writeTail, plus the 2D-context observe hook (new2DContext, blitCopy, blitFill). 1 arms both, 2 disarms the logs, 0 reads.
	//   out[0..6] the census (armed, frames, detailed, IBs, short reads, IB dwords, program heads), out[7..12] the 2D hook
	//   (installed, mode, minted, patched, blitCopy calls, blitFill calls).
	if (action == 61) {
		uint64_t c[7] = { 0 }, d[7] = { 0 };
		(void)n48::hw_hook_gfx_census(argScalar, c, 7);
		(void)navi48_ctx2d_control(argScalar, d, 7);
		N48LOG("accel-experiment: gfxcensus %llu -> census armed %llu frames %llu; 2D hook installed %llu minted %llu patched %llu copies %llu fills %llu",
		       (unsigned long long)argScalar, (unsigned long long)c[0], (unsigned long long)c[1], (unsigned long long)d[0],
		       (unsigned long long)d[2], (unsigned long long)d[3], (unsigned long long)d[4], (unsigned long long)d[5]);
		if (outExtra) {
			for (unsigned i = 0; i < 7 && i < kAccelExtraScalars; i++) outExtra[i] = c[i];
			for (unsigned i = 0; i < 6 && 7 + i < kAccelExtraScalars; i++) outExtra[7 + i] = d[i];
		}
		return kIOReturnSuccess;
	}
	// action 62 — `gfxneuter [1|2]` (0.0.278): while armed, every VMID-2 INDIRECT_BUFFER packet in a new frame on
	//   Apple's GFX ring is rewritten in the ring into a same-length NOP before Apple's doorbell, so the frame's wrapper and its
	//   own RELEASE_MEM stamp run and the client's stream is dropped. 1 arms (refused unless the render drain holds the ring),
	//   2 disarms, 0 reads. out[0..12] as hw_hook_gfx_neuter.
	// action 63 — `finishread` (0.0.279): READ-ONLY counters of the 2D context's user-client methods (selector 0x100
	//   set_surface, 0x101 finish = CoreDisplay's MPHWSync through IOAccelerator2D.plugin's WaitComplete, 0x102 blit), per process.
	// 0.0.286, DisplayPipeGuard.cpp. out[i] = v[i] of each control function (13 at most).
	// action 66 — `pipeguard [1]`: the display-pipe safety core (1 arms on the adopted pipes, 0 reads).
	// action 67 — `agdc [1]`: the AppleGraphicsDeviceControl nub (1 publishes; needs boot-arg navi48-agdc=1 and pipeguard), 0 reads.
	// action 68 — `fbbench [rows]`: in-kernel write throughput into RDNA4FB's scanout per cache mode (writes back what it read).
	// action 69 — `fbwc [1]`: write-combining for RDNA4FB's VRAM mappings (1 arms now; boot-arg navi48-fbwc=1 arms at publish), 0 reads.
	// action 70 — `pipeshim [0|1|2]` (0.0.288): the vendor-slot shim on the safety core's own vtable copy. 0 refuses as
	//   before, 1 participates in the transaction lifecycle and censuses every plane, 2 additionally presents plane 0 into
	//   RDNA4FB's scanout on SDMA0 QUEUE0. The flip (display slot 51) stays false in every mode.
	// action 71 — `pipemode [0|1]` (0.0.288): route B readiness. 0 reads back every field, 1 writes the fields
	//   init_framebuffer_resource would and sets pipe+0x298, without running AMD's initFramebufferResource, the event
	//   machine's slot 41 or resource slot 46.
	// action 72 — `emcensus [0|1|2]` (0.0.291): the event-machine census policy toggle (1 arm, 2 disarm,
	//   0/none read). Disarming leaves the whole safety core armed and drops only the accel+0x380 pass-through counters,
	//   to isolate cause 2 of (whether the census interfered with the Metal present fence).
	// action 73 — `routea [0|1]` (0.0.295, an internal review note): corrected route A, DEFAULT OFF.
	//   1 arms on the safety core's guarded pipes - slot 267 returns a kext-owned object and sets pipe+0x298, and slot 46
	//   (AMDAccelResource::prepare) is neutralised on the framebuffer resource only; 0 sets the runtime mode off and reads.
	// action 74 — `dcnstate [1]` (0.0.300, Track D stage 1): READ-ONLY DCN 4.1 display state.
	//   Reports which OTG is master-enabled (read, never assumed), its frame count, position and
	//   OTG_GLOBAL_SYNC_STATUS, the classified DCN interrupt counters, and the write allowlist's counters
	//   and its kept samples. Argument 1 additionally runs the allowlist's self-test inside the kernel
	//   against this card's own addresses (a legal display register, the SMU mailbox, an SMN indirect
	//   window and a stray address) on a SEPARATE allowlist state, so the live counters stay clean.
	// action 75 — `dcnvbl [0|1|2]` (0.0.300, T2-VBL): 0 disables whatever we enabled, 1 enables
	//   VUPDATE_NO_LOCK on the lit OTG, 2 enables VSTARTUP. One source at a time by construction: an
	//   enable disables the previous one first, and 0 is the unconditional escape hatch. The ONLY writes
	//   on this path are that source's own interrupt enable and acknowledge bits in one register, and
	//   every one of them passes the allowlist before it reaches the card.
	// action 76 — `dcnflip [0|1|2..30]` (0.0.301, T3-FLIP): 0 restores the console plane
	//   unconditionally (the emergency command, idempotent, allocates nothing), 1 is the
	//   machine-verified single-frame flip with an OTG CRC witness, 2..30 hold the test pattern for
	//   that many seconds so a person can see it. The hold runs inside the kernel call, so a dropped
	//   caller cannot strand the display, and an interrupt watchdog restores the plane from the work
	//   loop if it ever overruns. Geometry is read from HUBP0's own viewport/pitch/format registers.
	// action 77 — `dcnmode [0|1|2..30]` (0.0.302): the first mode-set, a SAME-MODE RE-TIMING of the
	//   lit output. 0 is read-only plus the emergency restore of the boot-time capture; 1 writes the
	//   identical timing set back and verifies every dword; 2..30 add that many seconds of dwell with
	//   a frame-counter stall watchdog. Nothing is computed, so nothing can be computed wrongly.
	// action 78 — `fbname [0|1]` (0.0.308): the RUNTIME class rename. 1 replaces the
	//   framebuffer's OSMetaClass className pointer with an interned symbol containing "AMD" so
	//   CoreDisplay resolves vendor bit 0x4 instead of the uncoverable 0x1; 0 restores it and verifies
	//   the restore. DEFAULT OFF. The offset is SEARCHED and must match exactly once, the superclass is
	//   checked to be IOFramebuffer, and a failed read-back restores immediately — a wrong offset is a
	//   refusal, never a write. NOT TO BE RUN without an adversarial review.
	// actions 83..87 (0.0.613, #11 11h.2; boot-arg navi48-metal-disp=1 only): the display pipe. All served by the legacy accel user client (Navi48UserClient), which is administrator-only and NOT
	// exclusive, so `pipe arm 0` and `fbname 0` stay reachable while WindowServer holds the exclusive N48N connection.
	//   83 pipeadopt            : turn the resource facts on, ask the accelerator for its probe (requestProbe(1)), verify the pipe is a Navi48DisplayPipe of ours on RDNA4FB, publish the capabilities.
	//   84 pipearm [0|1]        : 1 writes accel+0xccf (the type-4 user-client gate) once a verified pipe exists; 0 is ALWAYS allowed.
	//   85 pipestat [0|1|2|3|4] : the counters, in five pages (3: scan-owned + submit interval, 4: the vblank timestamps, 0.0.618).
	//   86 pipestamps           : READ-ONLY dump of the event machine's stamp words (SUSPECTED layout).
	//   87 pipeshortcut [0|1]   : the slot-62 'already prepared' shortcut switch (default ON; guarded by the SysMemory flag bit 4 either way).
	//   89 pipevbl [0|1]        : (0.0.618) the vblank-timestamp switch: 1 (the default with the latch ON) writes the transaction's +0x178 / +0x188 (next vblank time, next + one period; mach_absolute_time units) in the perform hook, 0 = 0.0.617 behaviour. `pipestat 4` shows the counts.
	//   90 pipereload [0|1]     : (0.0.619) the operator restart window: 0 (the default) opens a one-shot 15 s window in which the next uid-88 WindowServer client close while armed does not auto-disarm and slot-267 calls are not counted by the restart guard; 1 only reads it. Then `killall -9 WindowServer`.
	// action 106 (0.0.659, M6 Stage 1a; the same latch as 83..90): `m6stat [0|1|2|3]` - READ-ONLY report of the multi-display routing: per pipe instance the submits, the surface IDs (3 expected per display), the ambiguous ones, the refused presents and
	// their reasons, the performs completed without a copy, the vblank stamps per OTG and the AGDC answers. Writes nothing, reads no register. Served by n48disp_verb.
	// action 108 (0.0.663, ReBAR + Stage 2 review S3; the same latch as 83..90): `vramstat` - READ-ONLY report of the kernel allocator's visible-VRAM state: the pools' totals and free bytes, vramLimit, the mapped BAR0 bytes and BAR0's physical
	// address. Writes nothing and reads no register; the pool figures are plain loads of the allocator's counters (a diagnostic, like QueryInfo's). v[0] status (0 ok, 1 the ladder / allocator is not up: the BAR0 fields are still filled),
	// v[1] visible total, v[2] visible free, v[3] hi total, v[4] hi free, v[5] vramLimit, v[6] mapped BAR0 bytes, v[7] BAR0 phys, v[8] VRAM bytes, v[9] vramBase, v[10] gmc visible size, v[11] bit0 = hi pool exists, v[12] hi pool base.
	if (action == n48disp::kActVramStat) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48disp::verb_args_ok(action, argScalar) ? 0u : n48disp::kBadArg;
		if (st == 0u) {
			v[5] = bar0Limit; v[6] = bar0Size; v[7] = bar0Phys; v[8] = static_cast<uint64_t>(vramMB) << 20; v[9] = vramBase;
			if (gBringup.dev && gBringup.gmc.vram_alloc.is_inited()) {
				const amdgpu::GMCContext &g = gBringup.gmc;
				v[1] = g.vram_alloc.size(); v[2] = g.vram_alloc.bytes_free();
				if (g.vram_alloc_hi.is_inited()) { v[3] = g.vram_alloc_hi.size(); v[4] = g.vram_alloc_hi.bytes_free(); v[11] = 1ull; v[12] = g.vram_hi_base; }
				v[10] = g.visible_vram_size;
			} else v[0] = 1ull;
			// 0.0.664: the census for the step-B3 choice of ResizeAppleGpuBars: bit1 = the capability was found for BAR0, bits 8..31 = its current size in MiB, bits 32..63 = its supported-sizes mask (bit k = 1 MiB << k)
			v[11] |= (rebarFound ? 2ull : 0ull) | ((rebarCurMB & 0xFFFFFFull) << 8) | ((uint64_t)rebarMask << 32);
		}
		N48LOG("accel-experiment: vramstat -> status %u: visible %llu / %llu KiB free, hi %llu / %llu KiB free, vramLimit %#llx, BAR0 mapped %#llx at %#llx", st == n48disp::kBadArg ? 2u : (uint32_t)v[0],
		       (unsigned long long)(v[2] >> 10), (unsigned long long)(v[1] >> 10), (unsigned long long)(v[4] >> 10), (unsigned long long)(v[3] >> 10), (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)v[7]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == n48disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	if (action == 83 || action == 84 || action == 85 || action == 86 || action == 87 || action == 89 || action == 90 || action == n48disp::kActM6Stat) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48disp_verb(action, argScalar, v, 13);
		N48LOG("accel-experiment: pipe verb %u arg %llu -> status %u", action, (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == n48disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	// action 88 (0.0.614, #11 11h.3; boot-arg navi48-metal-disp=1 only): `pipeagdc [0|1]` - the NATIVE AGDC service (amd/native_agdc_pure.h). 1 builds Apple's AppleGraphicsDeviceControl object on a vtable copy
	// whose slots 0 / 1 / 266 are ours (IOPresentment's "Unable to get AGDC information" goes away); 0 only reads the state. Refused with the boot-arg OFF, while a row-120 hold is up, and on any failed check.
	// Served by DisplayPipeGuard.cpp (the AGDC state and the reply fillers live there); the admission and the exemption above are the generic n48disp ones.
	// actions 91..93 (0.0.622, multi-monitor stage M1; boot-arg navi48-metal-disp=1 only, like 83..90): the display instruments `ddcread <line> <block>`, `dmubring [page]`, `dispcensus [page]` (dcn/navi48_dispread.h).
	// dmubring and dispcensus only READ registers (through the read-only device attach() built); ddcread writes ONLY the DC_I2C engine's registers, through the DCN write allowlist, after the arbitration read says the engine is
	// free (dcn/navi48_dispread_flow.h). No command is ever sent to the DMUB and 91..93 read no VRAM through MM_INDEX (94, below, does: READ-ONLY). Served by n48dcn (dcn/navi48_dcn.cpp).
	// action 94 (0.0.624, stage M1.5; same latch): `region4read <off> [n]` - READ-ONLY dump of the DMUB REGION4 window in VRAM (the first 64 KiB, 64 dwords a call), the window base computed live from the DMCUB registers, the
	// memory read through navi48_vram_read_mm (the existing MM_INDEX reader). No register and no VRAM byte is written. Served by n48dcn::region4Read (dcn/navi48_dcn.cpp).
	// action 107 (0.0.661, M6 Stage 1b; the same latch as 83..90): `m6xstat [0|1|2]` - READ-ONLY report of instance 2's scanout (the monitor B: acquired, slots, EARLIEST2, FLIP_PENDING, the counters, the watchdog restores, the live HUBP2 health registers).
	// Writes nothing; with navi48-m6 / navi48-m6flip OFF it reads nothing either (v[0] bits 0..1 name the latches). Served by n48dcn::scanXReport (dcn/navi48_dcn.cpp).
	if (action == n48disp::kActM6XStat) {
		uint64_t v[13] = { 0 };
		if (n48m6_latched_on() && n48m6flip_latched_on()) (void)n48dcn::bind(this);
		const uint32_t st = n48disp::verb_args_ok(action, argScalar) ? n48dcn::scanXReport(argScalar, v, 13) : n48disp::kBadArg;
		N48LOG("accel-experiment: m6xstat page %llu -> status %u (latch words %#llx)", (unsigned long long)argScalar, st, (unsigned long long)v[0]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == n48disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	if (action == N48DR_ACT_DDCREAD || action == N48DR_ACT_DMUBRING || action == N48DR_ACT_DISPCENSUS || action == N48DR_ACT_REGION4READ) {
		uint64_t v[13] = { 0 };
		uint32_t st;
		if (!n48disp::verb_args_ok(action, argScalar)) {
			st = N48DR_BAD_ARG;
			v[0] = st;
		} else if (action == N48DR_ACT_DDCREAD) {
			(void)n48dcn::bind(this);
			st = n48dcn::ddcRead(argScalar, v, 13);
		} else if (action == N48DR_ACT_DMUBRING) {
			st = n48dcn::dmubRing(argScalar, v, 13);
		} else if (action == N48DR_ACT_REGION4READ) {
			st = n48dcn::region4Read(argScalar, v, 13);
		} else {
			st = n48dcn::dispCensus(argScalar, v, 13);
		}
		N48LOG("accel-experiment: %s %llu -> status %u (%s)", action == N48DR_ACT_DDCREAD ? "ddcread" : action == N48DR_ACT_DMUBRING ? "dmubring" : action == N48DR_ACT_REGION4READ ? "region4read" : "dispcensus",
		       (unsigned long long)argScalar, st, n48dr_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// actions 95..97 (0.0.625, stages M2 / M3; boot-arg navi48-metal-disp=1 for the admission AND boot-arg navi48-dmubcmd=1 inside the verb): `dmubsend <slotspec>`, `dmubmode <op>`, `dmubctx <ctx> status <v>` (dcn/navi48_dmubcmd.h) - the FIRST
	// verbs that send to the display firmware. With navi48-dmubcmd absent each returns N48DR_CMD_OFF and touches nothing. dmubsend needs bind() (only with the latch ON; its one register write, DMCUB_INBOX1_WPTR, goes through the DCN write allowlist).
	if (action == N48DM_ACT_SEND || action == N48DM_ACT_MODE || action == N48DM_ACT_CTX) {
		uint64_t v[13] = { 0 };
		if (action == N48DM_ACT_SEND && n48dcn::dmubCmdLatchedOn()) (void)n48dcn::bind(this);   // 0.0.626 (F4): the latch FIRST - OFF must touch nothing (bind() allocates a lock, reads registers, sets gDcn.armed)
		const uint32_t st = action == N48DM_ACT_SEND ? n48dcn::dmubSend(argScalar, v, 13) : action == N48DM_ACT_MODE ? n48dcn::dmubMode(argScalar, v, 13) : n48dcn::dmubCtx(argScalar, v, 13);
		N48LOG("accel-experiment: %s %#llx -> status %u (%s)", action == N48DM_ACT_SEND ? "dmubsend" : action == N48DM_ACT_MODE ? "dmubmode" : "dmubctx", (unsigned long long)argScalar, st, n48dr_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 88) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48disp::verb_args_ok(action, argScalar) ? navi48_agdc_native_control(argScalar, v, 13) : (uint32_t)n48agdc::kBadArg;
		N48LOG("accel-experiment: pipeagdc %llu -> status %u (%s)", (unsigned long long)argScalar, st, n48agdc::status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st == n48agdc::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	// action 99 (0.0.633; boot-arg navi48-metal-disp=1 only, like 91): `scdcread <line> <off> [len]` - READ-ONLY SCDC status read from the HDMI sink over the DC_I2C engine, the same engine sequence and containment as ddcread
	// (dcn/navi48_dispread_flow.h i2c_xfer; the only bus write is the 1-byte register offset). Served by n48dcn::scdcRead (dcn/navi48_dcn.cpp).
	if (action == N48DR_ACT_SCDCREAD) {
		uint64_t v[13] = { 0 };
		uint32_t st;
		if (!n48disp::verb_args_ok(action, argScalar)) {
			st = N48DR_BAD_ARG;
			v[0] = st;
		} else {
			(void)n48dcn::bind(this);
			st = n48dcn::scdcRead(argScalar, v, 13);
		}
		N48LOG("accel-experiment: scdcread %#llx -> status %u (%s)", (unsigned long long)argScalar, st, n48dr_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 98 (0.0.631, stage M4d; boot-arg navi48-metal-disp=1 for the admission AND boot-arg navi48-disp2=1 inside the verb): `disp2 timing|connect|off|status` (dcn/navi48_disp2.h) - OTG1 / ODM1 / OPP1 / DPG1 and DIG2's
	// front end for a test pattern on the HDMI display PHY C drives (the DMUB's `pclk otg1-on` / `phyc enable` first). With navi48-disp2 absent it returns N48D2_OFF and touches nothing (no bind, no read).
	if (action == N48D2_ACT) {
		uint64_t v[13] = { 0 };
		if (n48dcn::disp2LatchedOn()) (void)n48dcn::bind(this);   // the latch FIRST - OFF must touch nothing (bind() allocates a lock, reads registers, sets gDcn.armed)
		const uint32_t st = n48dcn::disp2(argScalar, v, 13, &gBringup);   // 0.0.635: &gBringup = the VRAM allocator the monitor B plane's two buffers come from (amdgpu::n1c_d2_alloc)
		N48LOG("accel-experiment: disp2 %#llx -> status %u (%s)", (unsigned long long)argScalar, st, n48d2_status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 105 (0.0.652, multi-monitor stage M5; boot-arg navi48-metal-disp=1 for the admission AND boot-arg navi48-fb2=1 inside the verb): `fbpublish 2` (the monitor B) / `fbpublish 1` (the monitor A, 0.0.658) - after `disp2 plane N` / `show N` / `fbhold N` (the plane HELD, the pair pinned), build the
	// IMMUTABLE snapshot (the live gates re-read, the monitor B's EDID over DDC line 3, the aperture) and publish Navi48DisplayNub (Navi48DisplayIndex 1) under the GPU's PCI device: the aux kext's Navi48Framebuffer matches it. Refuses with a named status
	// (n48fb::Status) and no register read when the plane is not held, a pipe is adopted, a WindowServer native session is open, or a nub already exists; WRITES NO REGISTER but the DC_I2C engine's (the EDID read, as `accel ddcread`).
	// There is NO withdraw in M5: a reboot is the only way back. Served by Navi48DisplayNub.cpp (decisions: amd/native_fb_pure.h).
	if (action == n48disp::kActFbPublish) {
		uint64_t v[13] = { 0 };
		const uint32_t st = Navi48DisplayNub::publishVerb(this, argScalar, v, 13);
		N48LOG("accel-experiment: fbpublish %llu -> status %u (%s)", (unsigned long long)argScalar, st, n48fb::status_name(st));
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 78) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48fbname::control(argScalar, v, 13);
		N48LOG("accel-experiment: fbname %llu -> status %u", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// build 0.0.518: `dcnflip 1002..1240` is flip mode's UNARMED A/B test of N = arg - 1000 flips
	// (navi48_fm_test; PC T-F1), and dcnflip's own pattern flip (1..30) is REFUSED while flip mode (switch 74) is ON: both would
	// program HUBP0. `dcnflip 0` (the restore of dcnflip's own pattern) is unchanged.
	if (action == 76 && n48_fm_test_n(argScalar) != 0u) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_fm_test(n48_fm_test_n(argScalar), v, 13);
		N48LOG("accel-experiment: dcnflip %llu (flip mode's A/B test) -> status %u", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 76 && argScalar != 0u && navi48_fm_on()) {
		N48LOG("accel-experiment: dcnflip %llu REFUSED - flip mode (switch 74) is ON and owns HUBP0's address; `gfxneuter 586` "
		       "turns it OFF (restoring A)", (unsigned long long)argScalar);
		if (outExtra) { outExtra[0] = 0; outExtra[1] = 0x70u; }
		return kIOReturnSuccess;
	}
	if (action == 74 || action == 75 || action == 76 || action == 77) {
		(void)n48dcn::bind(this);
		uint64_t v[13] = { 0 };
		const uint32_t st = action == 74 ? n48dcn::state(argScalar, v, 13)
		                  : action == 75 ? n48dcn::vbl(argScalar, v, 13)
		                  : action == 76 ? n48dcn::flip(argScalar, v, 13)
		                                 : n48dcn::mode(argScalar, v, 13);
		N48LOG("accel-experiment: %s %llu -> status %u",
		       action == 74 ? "dcnstate" : action == 75 ? "dcnvbl"
		       : action == 76 ? "dcnflip" : "dcnmode",
		       (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 81) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_ucprobe_control(argScalar, v, 13);
		N48LOG("accel-experiment: ucprobe %llu -> status %u (0 ok, 1 no accelerator vtable, 2 slide, "
		       "3 slot 239 is not newUserClient, 5 bad argument); clients minted %llu, display-pipe clients "
		       "seen %llu patched %llu, externalMethod calls %llu, sel4 %llu sel8 %llu, selector bitmask %#llx",
		       (unsigned long long)argScalar, st, (unsigned long long)(v[3] & 0xffffffffull),
		       (unsigned long long)(v[5] & 0xffffffffull), (unsigned long long)(v[5] >> 32),
		       (unsigned long long)v[7], (unsigned long long)(v[8] & 0xffffffffull),
		       (unsigned long long)(v[8] >> 32), (unsigned long long)v[9]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 80) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_cqprobe_control(argScalar, v, 13);
		N48LOG("accel-experiment: cqprobe -> status %u (0 ok, 1 no accelerator)", st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 79) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_agdc_hold_control(argScalar, v, 13);
		N48LOG("accel-experiment: agdchold %llu -> status %u (0 ok, 13 above the ceiling)",
		       (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action >= 66 && action <= 73) {
		uint64_t v[13] = { 0 };
		const uint32_t st = action == 66 ? navi48_pipeguard_control(argScalar, v, 13)
		                  : action == 67 ? navi48_agdc_control(argScalar, v, 13)
		                  : action == 68 ? navi48_fbbench(argScalar, v, 13)
		                  : action == 69 ? navi48_fbwc_control(argScalar, v, 13)
		                  : action == 70 ? navi48_pipeshim_control(argScalar, v, 13)
		                  : action == 71 ? navi48_pipemode_control(argScalar, v, 13)
		                  : action == 72 ? navi48_emcensus_control(argScalar, v, 13)
		                                 : navi48_routea_control(argScalar, v, 13);
		N48LOG("accel-experiment: %s %llu -> status %u", action == 66 ? "pipeguard" : action == 67 ? "agdc" : action == 68 ? "fbbench"
		       : action == 69 ? "fbwc" : action == 70 ? "pipeshim" : action == 71 ? "pipemode" : action == 72 ? "emcensus" : "routea",
		       (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 63) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_ctx2d_calls(v, 13);
		N48LOG("accel-experiment: finishread -> hook %u; finish %llu returned %llu; WindowServer finish %llu returned %llu",
		       st, (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[6], (unsigned long long)v[7]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 65 — `gfxprobe [1|2]` (0.0.283): the copy-back probe - fills SecurityAgent's last VRAM drawable of each
	//   captured submission with magenta (needs `gfxcapture 1`). 1 arms, 2 disarms, 0 reads. out[0..12] as hw_hook_gfx_probe.
	if (action == 65) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48::hw_hook_gfx_probe(argScalar, v, 13);
		N48LOG("accel-experiment: gfxprobe %llu -> status %u armed %llu considered %llu filled %llu refused plan %llu page %llu window %llu",
		       (unsigned long long)argScalar, st, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
		       (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st ? kIOReturnNotReady : kIOReturnSuccess;
	}
	// action 64 — `gfxcapture [1|2]` (0.0.282): READ-ONLY capture of every submission at the source hook (needs the
	//   neuter's hook installed: `gfxneuter 1` first) into the binary capture ring, streamed by `navi48test capstream`. 1 arms,
	//   2 disarms, 0 reads. out[0..12] as hw_hook_gfx_capture.
	if (action == 64) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48::hw_hook_gfx_capture(argScalar, v, 13);
		N48LOG("accel-experiment: gfxcapture %llu -> status %u armed %llu seen %llu full %llu IBs %llu regions %llu programs %llu dropped %llu",
		       (unsigned long long)argScalar, st, (unsigned long long)v[0], (unsigned long long)v[1], (unsigned long long)v[2],
		       (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[6], (unsigned long long)v[11]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return st ? kIOReturnNoMemory : kIOReturnSuccess;
	}
	if (action == 62) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48::hw_hook_gfx_neuter(argScalar, v, 13);
		N48LOG("accel-experiment: gfxneuter %llu -> status %u (0 ok, 1 arming refused); armed %llu, frames %llu, IBs NOPed %llu, "
		       "mismatches %llu", (unsigned long long)argScalar, st, (unsigned long long)v[0], (unsigned long long)v[1],
		       (unsigned long long)v[2], (unsigned long long)v[3]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 59) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_flushhook_control(argScalar, v, 13);
		N48LOG("accel-experiment: flushhook %llu -> status %u, mode %llu", (unsigned long long)argScalar, st,
		       (unsigned long long)v[1]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 60) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_scanout_control(argScalar, v, 13);
		N48LOG("accel-experiment: scanout %llu -> status %u", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 82 — `sdmadcc [0|1|2]` (0.0.417, an internal design note, D1): the SDMA0_DCC_CNTL
	// no-PTE read-decompression / write-compression set and restore. 0 reads SDMA0 and SDMA1 raw and decoded,
	// 1 captures-then-clears the eight *_COMP_EN_n bits on SDMA0 only, 2 restores the captured value; any other
	// argument, and 2 before a capture, is REFUSED. SDMA1 is never written. Defined above.
	if (action == 82) {
		uint64_t v[13] = { 0 };
		const uint32_t st = navi48_sdmadcc_control(argScalar, v, 13);
		N48LOG("accel-experiment: sdmadcc %llu -> status %u (0 ok, 1 bad argument, 2 no context, 3 no GC base, "
		       "4 read-back mismatch)", (unsigned long long)argScalar, st);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 58) {
		uint64_t v[13] = { 0 };
		const uint32_t st = n48::hw_hook_drain_state(v, 13);
		N48LOG("accel-experiment: drain — state %u, submissions %llu, IBs %llu (translated %llu, refused %llu), "
		       "polls neutered %llu, max latency %llu us", st, (unsigned long long)v[1], (unsigned long long)v[2],
		       (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[7],
		       (unsigned long long)v[11]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 57) {
		uint64_t v[10] = { 0 };
		const uint32_t verdict = navi48_pairing_control(argScalar, v, 10);
		N48LOG("accel-experiment: pairing %llu — verdict %u (%s); decided %llu, source %llu (%s), "
		       "stamped %llu, withdrawn %llu, withdraw reason %llu",
		       (unsigned long long)argScalar, verdict, n48_pairing_verdict_name(verdict),
		       (unsigned long long)v[4], (unsigned long long)v[5],
		       v[4] ? n48_pairing_source_name((uint32_t)v[5]) : "not decided yet",
		       (unsigned long long)v[6], (unsigned long long)v[7], (unsigned long long)v[8]);
		if (outExtra)
			for (unsigned i = 0; i < 10 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return verdict == kPairingVerdictBadArg ? kIOReturnBadArgument : kIOReturnSuccess;
	}
	if (action == 50) {
		if (argScalar) {
			if (accelExperimentFired) {
				N48LOG("accel-experiment: bootchain mode %#llx REFUSED — the accelerator has "
				       "already fired this boot, so the chain's arming point is past. Set the "
				       "mode BEFORE `fire`, or use the boot-arg navi48-boot-chain.",
				       (unsigned long long)argScalar);
				return kIOReturnNotReady;
			}
			n48::hw_hook_boot_chain_configure((uint32_t)argScalar);
			N48LOG("accel-experiment: bootchain mode set to %#llx by verb (bit0 accelerator-start "
			       "chain, bit1 submission takeover); it arms at the accelerator-started callback",
			       (unsigned long long)argScalar);
			setProperty("Navi48,BootChainMode", static_cast<uint64_t>(argScalar), 32);
		}
		uint64_t v[13] = { 0 };
		const uint32_t st = n48::hw_hook_boot_chain_state(v, 13);
		static const char *sn[] = { "idle (not armed)", "RUNNING", "DONE", "FAILED" };
		N48LOG("accel-experiment: bootchain — state %u (%s), mode %#llx, failedAt %llu, %llu ms",
		       st, sn[st <= 3 ? st : 0], (unsigned long long)v[1],
		       (unsigned long long)v[2], (unsigned long long)v[8]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 49 — `eopbridge [1|2]` (0.0.235, MILESTONE 3 step 1).
	//   Arms the IH -> Apple completion bridge for this boot: an end-of-pipe IH
	//   entry (src 181) then calls Apple's OWN AMDSWScheduler::checkTimestamps
	//   for every channel with work outstanding, so a command buffer retires
	//   with no client blocked in waitUntilCompleted and no forced `runcheckts`.
	//   This is the gap addendum 2 measured: Apple's traffic raises no
	//   interrupts on our stack because we own the MSI and the IH ring, so
	//   Apple's own timeStampInterruptCallback is never reached.
	//   Nothing is fabricated — it is the identical call action 26 makes.
	//   1 arms, 2 disarms, anything else reads the counters. Needs `fire`,
	//   because the scheduler is resolved through Apple's live objects.
	if (action == 49) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: eopbridge refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		uint64_t v[13] = { 0 };
		const uint32_t n = n48::hw_hook_eop_bridge_control((uint32_t)argScalar, v, 13);
		N48LOG("accel-experiment: eopbridge — %s (entries %llu, calls %llu, retired %llu, "
		       "skipped %llu, refused %llu)", n ? "ARMED" : "not armed",
		       (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[4],
		       (unsigned long long)v[3], (unsigned long long)v[5]);
		if (outExtra)
			for (unsigned i = 0; i < 13 && i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 32) {
		const uint32_t n = n48::hw_hook_set_virtual_space();
		N48LOG("accel-experiment: setvspace — %s",
		       n ? "descriptor INSTALLED, mem+0x98 set" : "refused, or still clear");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	// action 31 —: run GFX10PM4Engine::powerUp so doStart fills engine+0x340.
	// action 33 —: enable the KIQ ring (GFX10ComputeRing::enable).
	// action 34 — route 1: arm the KIQ stamp poller. submitKIQFrame passes
	//   the POST-INCREMENT value of chan+0x80 to waitForHwStamp (0xbe18970 incl,
	//   0xbe18991 reload), which returns true once stamp - chan+0x84 <= 0
	//   (0xbe08852 / 0xbe088e0). 0.0.176 does NOT write chan+0x84 to get there:
	//   Apple's checkForTimestampUpdate -> timestampUpdated PATH B (0xbe08551)
	//   overwrites +0x84 from the write-back dword, which is what erased 0.0.175's
	//   store. It writes *[chan+0xc0] instead — the dword the GPU would have
	//   written — against a BASELINE taken at arm time, and only falls back to a
	//   direct +0x84 write, logged as such, after 300 ms without propagation.
	//   It runs on its own kernel thread because action 31 blocks for 5 s.
	// action 35 — read-only: the same channel state, the poller's full record
	//   (baseline, fires, fallbacks, propagation time) and a dump of the KIQ ring,
	//   which is how the result is read AFTER pm4powerup returns.
	if (action == 34 || action == 35) {
		n48::KiqStampInfo info {};
		const uint32_t n = (action == 34) ? n48::hw_hook_kiq_stamp(&info)
		                                  : n48::hw_hook_kiq_chan_state(&info);
		N48LOG("accel-experiment: %s — %s", action == 34 ? "kiqstamp" : "kiqchan",
		       n ? (action == 34 ? "poller ARMED" : "channel dumped")
		         : "refused (identity check failed, or already armed)");
		// Deliberately NOT overwriting outInstalled: navi48test prints it as
		// "hook installed", and a refusal here says nothing about the TTL hook.
		// Rule 5 — never print a label the run's own output could contradict.
		// The armed/refused answer rides in outExtra[0].
		// 0.0.176: 13 extras, which is every scalar slot the ABI has
		// (scalarOutput[3..15]). Keep this list and navi48test's out[] indices in
		// lockstep — they are numbered by position, with nothing checking them.
		if (outExtra) {
			outExtra[0]  = n;
			outExtra[1]  = info.chan;
			outExtra[2]  = info.id;
			outExtra[3]  = info.submitted;
			outExtra[4]  = info.completed;
			outExtra[5]  = info.pollState;    // low byte state, bit 8 = PATH B
			outExtra[6]  = info.baseline;
			outExtra[7]  = info.fires;
			outExtra[8]  = info.fallbacks;
			outExtra[9]  = info.lastWritten;
			outExtra[10] = info.lastPropMs;   // 0xFFFFFFFF = never propagated
			outExtra[11] = info.wbPtr;
			outExtra[12] = info.wbValue;
		}
		return kIOReturnSuccess;
	}
	// action 36 — `gfxmap`, the first increment of route 2. It SUPERSEDES
	//   kiqstamp for a boot (they share the arm-once CAS, so the second one
	//   refuses): the same 32-dword KIQ frames, but decoded rather than merely
	//   counted. On MAP_QUEUES with engine_sel 4 it PTE-checks Apple's ring,
	//   HDP-flushes, MES REMOVE_QUEUEs our kernel GFX queue off GFX pipe0/queue0,
	//   builds a v12_gfx_mqd for APPLE'S ring in our MQD buffer, MES ADD_QUEUEs
	//   it with map_legacy_kq, and only then reads CP_GFX_HQD_ACTIVE / CP_RB0_RPTR
	//   / CP_RB0_WPTR back to decide whether doStart's 0xbe24357 gate may see the
	//   truth. Until it does, hook_regRead answers a live-zero CP_RB0_RPTR with 1
	//   so the gate FAILS and doStart unwinds through the bounded doStop path.
	// action 37 — `gfxstate`, read-only: the emulator's record, the live gate
	//   registers read through our own MMIO path, Apple's GFX ring header and
	//   content, and the wptr/rptr write-backs whose ADVANCE is the proof the CP
	//   consumed Apple's ring.
	if (action == 36 || action == 37) {
		n48::GfxMapInfo g {};
		const uint32_t n = (action == 36) ? n48::hw_hook_gfx_map(&g)
		                                  : n48::hw_hook_gfx_state(&g);
		N48LOG("accel-experiment: %s — %s", action == 36 ? "gfxmap" : "gfxstate",
		       n ? (action == 36 ? "emulator ARMED (gate shadow ON)" : "state read")
		         : "refused (identity check failed, or an emulator is already live)");
		// Same discipline as 34/35: outInstalled means "the TTL hook is
		// installed" and a refusal here says nothing about that, so it is left
		// alone. 13 extras is the whole ABI budget (scalarOutput[3..15]); keep
		// this list and navi48test's out[] indices in lockstep.
		if (outExtra) {
			outExtra[0]  = n;
			outExtra[1]  = (uint64_t)g.armed | ((uint64_t)g.proceed << 1) |
			               ((uint64_t)g.gateShadows << 8);
			outExtra[2]  = g.frames;
			outExtra[3]  = g.stamps;
			outExtra[4]  = g.refusals;
			outExtra[5]  = g.mapResult;
			outExtra[6]  = g.failStep;
			outExtra[7]  = ((uint64_t)g.addKr << 32) | (uint32_t)g.removeKr;
			outExtra[8]  = g.mqdGpu;
			outExtra[9]  = g.doorbell;
			outExtra[10] = ((uint64_t)g.regWptrHi << 32) | g.regWptr;
			outExtra[11] = ((uint64_t)g.regActive << 32) | g.regRptr;
			outExtra[12] = ((uint64_t)g.appleRingWptr << 32) | g.regCntl;
		}
		return kIOReturnSuccess;
	}
	// action 38 — `sdmamap`, the SDMA counterpart of `gfxmap` (design memo
	//   notes/re/sdma-takeover-design.md). It reads Apple's chan-14 SDMA ring,
	//   PTE-checks every page of it and of its write-back frame, HDP-flushes,
	//   programs SDMA0 QUEUE1 (RB_ENABLE=0 -> verify the read-back -> RB_ENABLE=1)
	//   with APPLE'S write-back addresses and doorbell dword 0x202, and rewrites
	//   ring->0xc0 to the real BAR2 doorbell — only if that pointer is still
	//   exactly the private word our own TTL slot 36 handed out for (10,1).
	//   Our SDMA0 QUEUE0 is NOT torn down: it stays live as the control.
	// action 39 — `sdmastate`, read-only: the QUEUE1 register block, Apple's ring
	//   header, the wptr write-back / rptr report / FENCE dwords, and a decode of
	//   the ring's first min(wptr, 128) dwords.
	if (action == 38 || action == 39) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: %s refused — fire the experiment first",
			       action == 38 ? "sdmamap" : "sdmastate");
			return kIOReturnNotReady;
		}
		n48::SdmaMapInfo s {};
		const uint32_t n = (action == 38) ? n48::hw_hook_sdma_map(&s)
		                                  : n48::hw_hook_sdma_state(&s);
		N48LOG("accel-experiment: %s — %s", action == 38 ? "sdmamap" : "sdmastate",
		       n ? (action == 38 ? "SDMA0 QUEUE1 ARMED on Apple's ring" : "state read")
		         : "refused (see the REFUSING line above for which step)");
		// Same discipline as 34/35 and 36/37: outInstalled means "the TTL hook is
		// installed" and is left alone. 13 extras is the whole ABI budget
		// (scalarOutput[3..15]); keep this list and navi48test's out[] in lockstep.
		if (outExtra) {
			outExtra[0]  = n;
			outExtra[1]  = (uint64_t)s.armed | ((uint64_t)s.q1TestResult << 8) |
			               ((uint64_t)s.mappedCount << 16) |
			               ((uint64_t)s.unmappedCount << 24);
			outExtra[2]  = s.mapResult;
			outExtra[3]  = s.failStep;
			outExtra[4]  = s.doorbell;
			outExtra[5]  = s.ringGpu;
			outExtra[6]  = ((uint64_t)s.ringWptr << 32) | s.ringDwords;
			outExtra[7]  = s.rptrReport;
			outExtra[8]  = s.wptrWb;
			outExtra[9]  = ((uint64_t)s.regWptr << 32) | s.regRptr;
			outExtra[10] = ((uint64_t)s.regDoorbell << 32) | s.regCntl;
			outExtra[11] = ((uint64_t)s.regDoorbellOff << 32) | s.fenceValue;
			outExtra[12] = s.newDoorbellPtr ? s.newDoorbellPtr : s.oldDoorbellPtr;
		}
		return kIOReturnSuccess;
	}
	// action 40 — `faultclear` (0.0.193): pulse GCVM_L2_PROTECTION_FAULT_CNTL
	//   bit 0 so the first-fault-latched status belongs to the NEXT dispatch,
	//   printing the word it discards. Run it immediately before the measurement.
	// action 41 — `vmstate`, read-only: GCVM_CONTEXT2 (Apple's VMID 2)
	//   CNTL/BASE/START/END, the latched fault status decoded field by field, and
	//   four entries of Apple's page-table arena read through MM_INDEX.
	// Neither needs `fire`: they read hardware registers and VRAM, not Apple's
	// objects, so they are useful on a boot where the experiment never ran.
	if (action == 40 || action == 41) {
		const uint32_t n = (action == 40) ? n48::hw_hook_vm_fault_clear()
		                                  : n48::hw_hook_vm_arena_state();
		N48LOG("accel-experiment: %s — %s", action == 40 ? "faultclear" : "vmstate",
		       n ? (action == 40 ? (n == 1 ? "fault status CLEARED (reads 0)"
		                                   : "cleared, but a fault re-latched at once")
		                         : "state read")
		         : "refused (see the REFUSING line above)");
		if (outExtra) outExtra[0] = n;
		return kIOReturnSuccess;
	}
	// action 42 — `vmib` (0.0.194), read-only and writes nothing anywhere.
	//   Software-walks Apple's VMID-2 page table for argScalar (0 = the blit
	//   IB at VA 0x4000a0000), prints every entry decoded, then reads 0x80 dwords
	//   of the page it lands on — through an IOMemoryDescriptor when the leaf has
	//   SYSTEM set, through MM_INDEX when it is VRAM — and decodes them as TYPE3
	//   PM4. Also logs CP_STAT/GRBM_STATUS/CP_RB0_RPTR and the CP IB registers.
	//   Like 40/41 it needs no `fire`: it touches registers and memory, not
	//   Apple's objects.
	if (action == 42) {
		uint64_t phys = 0;
		uint32_t first = 0, cpStat = 0;
		const uint32_t n = n48::hw_hook_vm_ib_dump(argScalar, &phys, &first, &cpStat);
		static const char *why[] = { "refused (see the REFUSING line above)",
		                             "root PDE not valid", "L1 entry not valid",
		                             "leaf entry not valid", "the page could not be read",
		                             "dumped from VRAM", "dumped from a HOST page" };
		N48LOG("accel-experiment: vmib — %s", why[n <= 6 ? n : 0]);
		if (outExtra) {
			outExtra[0] = n;
			outExtra[1] = phys;
			outExtra[2] = first;
			outExtra[3] = cpStat;
		}
		return kIOReturnSuccess;
	}
	// action 43 — `ringib [chan]` (0.0.196), read-only and writes nothing
	//   anywhere. Decodes ONE Apple channel's SDMA ring packet by packet
	//   (COND_EXE / INDIRECT_BUFFER / FENCE / TRAP / COPY / WRITE / CONST_FILL /
	//   PTEPDE / POLL_REGMEM / REG_WRITE) and then dumps and decodes every
	//   INDIRECT_BUFFER it references, read through our GART.'s open
	//   question: what is in chan 13's three frames, and is the blit's shader
	//   upload a COPY_LINEAR in one of them? argScalar 0 means chan 14.
	if (action == 43) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ringib refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_ring_ib_dump(argScalar);
		N48LOG("accel-experiment: ringib — %s (%u indirect buffer(s) decoded)",
		       n ? "ring decoded" : "refused (see the REFUSING line above)",
		       n ? n - 1u : 0u);
		if (outExtra) {
			outExtra[0] = n;
			outExtra[1] = argScalar;
		}
		return kIOReturnSuccess;
	}
	// action 44 — `pagecopy [1]` (0.0.201). argScalar 1 ARMS the residency copy for
	//   this boot: Navi48AccelPeer's pageTexture hook then copies a sysmem ->
	//   AMDAccelVidMemory page-on into VRAM through the MM window and reads every
	//   dword back, instead of reporting success without copying (the skip, which
	//   stays in force for every other shape and for page-out). Any other argument
	//   only reads the counters. Needs navi48-skip-pagecopy=1 and `fire` (the hook is
	//   installed when Apple's accelerator starts). Results ride the out-scalars.
	if (action == 44) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = navi48_pagecopy_control2((argScalar == 1 || argScalar == 2) ? (uint32_t)argScalar : 0u, v, kAccelExtraScalars);   // 0.0.284: 2 also arms page-out
		N48LOG("accel-experiment: pagecopy — %s; %llu copied, %llu read-back mismatch(es), "
		       "%llu unhandled, %llu page-out(s)",
		       (st & 1) ? "residency copy ARMED"
		                : ((st & 2) ? "not armed" : "no skip-pagecopy hook (boot-arg off or refused)"),
		       (unsigned long long)v[1], (unsigned long long)v[4],
		       (unsigned long long)v[5], (unsigned long long)v[7]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 45 — `flushdrop` (0.0.202). AN INSTRUMENT, never a fix: after
	//   xlatregs and before sdmamap it rewrites the EVENT_WRITE CS_PARTIAL_FLUSH that
	//   follows the compute DISPATCH_DIRECT in Apple's VMID-2 blit IB into two one-dword
	//   NOPs, so the ME can finish the IB while the waves stay stuck. The IB's host
	//   page comes from the PTEPDE entries still pending in Apple's SDMA IBs; the IB's
	//   identity is checked before the write. Results ride the out-scalars.
	if (action == 45) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: flushdrop refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_blit_flush_drop(v, kAccelExtraScalars);
		static const char *why[] = { "refused (see the REFUSING line above), nothing written",
		                             "partial flush after the dispatch REWRITTEN to two NOPs, read back",
		                             "already rewritten, nothing to do",
		                             "written but the read-back MISMATCHED" };
		N48LOG("accel-experiment: flushdrop — %s", why[st <= 3 ? st : 0]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 46 — `kernsub` (0.0.204). Substitute a gfx1201 blit kernel for
	//   Apple's Navi21 one at the residency copy's shader region (VRAM lastDst +
	//   0xfb00), guarded by an exact match on Apple's original bytes. argScalar 1
	//   arms it (and substitutes now if a copy has already run this boot); any
	//   other argument only reads the counters. Results ride the out-scalars.
	//   0.0.206: argScalar selects the kernel — 1 blit_copy_gfx1201 (the copy),
	//   2 blit_diag_gfx1201 and 3 blit_diagmin_gfx1201 (INSTRUMENTS, section 328);
	//   0.0.207: 4 blit_copy_offen_gfx1201 (the copy by byte offset, section 329);
	//   one mode per boot; 0 or anything else only reads the counters.
	if (action == 46) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t mode = (argScalar >= 1 && argScalar <= 4) ? (uint32_t)argScalar : 0u;
		const uint32_t st = navi48_kernelsub_control(mode, v, kAccelExtraScalars);
		static const char *why[] = { "refused or read-only (see the log), nothing written",
		                             "gfx1201 kernel SUBSTITUTED, read back",
		                             "already substituted, nothing to do",
		                             "written but the read-back MISMATCHED" };
		N48LOG("accel-experiment: kernsub mode %u (armed mode %llu) — %s", mode,
		       (unsigned long long)v[8], why[st <= 3 ? st : 0]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 51 — `shadercache [1|2]` (0.0.239, MILESTONE 3 step 2).
	//   Arms the hash-keyed substitution at the residency copy: every 0x100-grid slot of
	//   every copied resource is looked up in the embedded cache blob and, on a verified
	//   full-byte match, Apple's GFX10 code is replaced by our gfx1201 code. This is
	//   kernsub's seam with kernsub's exact-byte guard generalised to a keyed lookup, so
	//   it needs no hard-coded offset and no hard-coded shader. ORDERING IS NOT kernsub's:
	//   kernsub may be armed after the client has paged, because it re-substitutes at the
	//   copy's recorded destination; this path retains no Apple pointer and can only act
	//   while a copy runs, so it must be armed BEFORE the client pages its shaders —
	//   right after copyarm. Arming late is logged as such rather than silently doing
	//   nothing. 1 arms, 2 disarms, no argument reads the counters.
	// action 52 — `vmroots` (0.0.242, MILESTONE 3 step 4). READ-ONLY: it
	//   writes nothing into Apple's page tables or anywhere else, and reserves no VRAM.
	//   It is the boot asked for — which root indices are valid, whether the set
	//   moves across a client's life, whether a SECOND client gets a SECOND root table,
	//   and where a VRAM-backed ring region could be carved. argScalar non-zero also
	//   dumps the root page at that address, so a table seen in an earlier read can be
	//   re-read later in the same boot.
	if (action == 52) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_vm_root_dump(argScalar, v, kAccelExtraScalars);
		N48LOG("accel-experiment: vmroots — %s; live CONTEXT2 root %#llx; %llu valid root "
		       "entr(y/ies) in the first table (highest all-zero index %llu); %llu distinct "
		       "(context, root) pair(s); Apple arena [%#llx..%#llx); free tail %#llx + %#llx",
		       st ? "read" : "REFUSED (see the REFUSING line above)",
		       (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[4],
		       (unsigned long long)v[5], (unsigned long long)v[8], (unsigned long long)v[9],
		       (unsigned long long)v[10], (unsigned long long)v[11]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 53 — `ringmap [0|1]` (0.0.244, milestone 3 step 4, an earlier analysis
	//   increments (i) and (ii)). Reserve the VRAM tail region r80 measured free, build
	//   the kext-owned page-directory block and its 64 KiB leaves over the GE ring region,
	//   and verify the mapping with OUR OWN walker. It writes ONLY our own block, in VRAM
	//   outside vram_alloc_hi and below Apple's arena; Apple's root page is READ-ONLY here.
	//   The single root PDE write that would make the mapping live is increment (iii), held
	//   behind the adversarial review section 397 asks for, because r80 found Apple recycles
	//   a dead client's root page as an ordinary PTE page. 0 reserves and reports; 1 builds.
	if (action == 53) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_ring_map(argScalar, v, kAccelExtraScalars);
		N48LOG("accel-experiment: ringmap — %s; ring region vram+%#llx + %#llx; L1 block "
		       "vram+%#llx; %llu leaf entr(y/ies); %llu L1 entr(y/ies) written, %llu read-back "
		       "mismatch(es); %llu walk probe(s), %llu mismatch(es); ring VA %#llx; Apple's root "
		       "slot %llu reads %#llx and was NOT written",
		       st == 0 ? "REFUSED (see the REFUSING line above)"
		               : st == 1 ? "reserved, read-only (the L1 block was not written)"
		               : st == 2 ? "BUILT and VERIFIED" : "built, but a check MISMATCHED",
		       (unsigned long long)v[2], (unsigned long long)v[3], (unsigned long long)v[4],
		       (unsigned long long)v[5], (unsigned long long)v[6], (unsigned long long)v[7],
		       (unsigned long long)(v[8] >> 32), (unsigned long long)(v[8] & 0xFFFFFFFFull),
		       (unsigned long long)v[9], (unsigned long long)v[10], (unsigned long long)v[11]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 54 — `vmctx` (0.0.247, milestone 3 step 4, the OBSERVE BOOT that
	//   notes/an internal review note section 7.1 requires before increment (iii)).
	//   READ-ONLY: neither this verb nor the VMM slot-40/41 hooks it reports on write
	//   anything - not Apple's page tables, not Apple's objects, not a register, not
	//   VRAM. Run it while a Metal client is alive: it reports the root page-table
	//   address read out of each context object that owns it, cross-checks the live
	//   value against the root Apple's CONTEXT2 register names, reads root[0] and
	//   root[511] of each live root, and reads GFXHUB engine 17's invalidation range.
	if (action == 54) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_vm_context_observe(argScalar, v, kAccelExtraScalars);
		N48LOG("accel-experiment: vmctx — %s; observe pair %s; %llu create(s), %llu release(s), "
		       "%llu live; root now %#llx / %#llx; Apple's CONTEXT2 root %#llx; %llu agree; "
		       "ENG17 range %08llx/%08llx; root[511] %#llx; %llu context(s) had a NON-ZERO root at "
		       "create. NOTHING was written.",
		       st ? "read" : "REFUSED (see the REFUSING line above)",
		       (v[0] & 1) ? "installed" : ((v[0] & 2) ? "REFUSED by the geometry check"
		                                             : "not installed"),
		       (unsigned long long)v[1], (unsigned long long)v[2], (unsigned long long)v[3],
		       (unsigned long long)v[4], (unsigned long long)v[5], (unsigned long long)v[6],
		       (unsigned long long)v[7],
		       (unsigned long long)(v[8] >> 32), (unsigned long long)(v[8] & 0xFFFFFFFFull),
		       (unsigned long long)v[9], (unsigned long long)(v[10] >> 32));
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 55 — `rootwrite [0|1]` (0.0.250, milestone 3 step 4 increment (iii)).
	//   THE SINGLE 8-BYTE ROOT PDE WRITE: the first write this project has ever made
	//   into a live Apple VM page table. It points Apple's VMID-2 root slot 511
	//   (VA 0x23F0000000) at the kext-owned L1 block `ringmap` built, which is what
	//   finally makes our GE ring mapping live after six boots of it being inert.
	//   NO ARGUMENT REPORTS ONLY - it evaluates guards G1..G6 against the live state
	//   and says what would happen, writing nothing. 1 performs the write, and only
	//   if all six guards pass. Each guard refuses rather than proceeds, and the
	//   guard predicate is the same code the host test drives through every refusal
	//   path (src/navi48-bringup/tests/rootwrite_guards_test.cpp), because G1, G2 and
	//   G3 fail SILENTLY and OFF-TARGET and a passing hardware run cannot prove them.
	//   The withdrawal is not here: it runs on Apple's own teardown path, in the
	//   AMDHWVMContext::pageOffPD hook, at the last instant the page is still ours.
	// 56 = rearmdrain (0.0.261,). The trace in
	//   notes/re/review-vmid2-teardown.md found that NOTHING tears VMID 2 down:
	//   prepareVmBlockForUpdate queues a clearWithDMA of the freshly allocated root block
	//   inside the very mapVA call our arm writes in, and OUR sdmamap drain is what
	//   executes it. So the entry is wiped by a packet Apple queued before we armed, and
	//   the fix is to re-assert it AFTER the drain. It reuses rootwrite_arm_context - the
	//   one write path, all six guards - and adds no write machinery. It also runs
	//   instrument D: an UNFILTERED dump of every pending packet touching the root page,
	//   with a positive control. Argument 0 REPORTS ONLY; 1 performs the re-arm.
	if (action == 56) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_rearm_after_drain(argScalar, v, kAccelExtraScalars);
		N48LOG("accel-experiment: rearmdrain — %s; refusal reason %llu; root[511] after the "
		       "drain %#llx; armed root %#llx; CONTEXT2 root %#llx; packets touching the root "
		       "page %llu (positive control: %llu PTEPDE at root[0]); %llu packet(s) walked, "
		       "%llu IB(s) stopped on an unknown stride; %llu with no memory destination, %llu "
		       "addressed elsewhere; read back %#llx; fault status %#llx",
		       st == 0 ? "REFUSED (nothing was written)"
		               : st == 1 ? "REPORT ONLY (nothing was written)"
		               : st == 2 ? "RE-ARMED and verified"
		                         : "RE-ARMED but a read-back MISMATCHED",
		       (unsigned long long)v[1], (unsigned long long)v[2],
		       (unsigned long long)v[3], (unsigned long long)v[4],
		       (unsigned long long)(v[5] >> 32), (unsigned long long)(v[5] & 0xFFFFFFFFull),
		       (unsigned long long)(v[6] >> 32), (unsigned long long)(v[6] & 0xFFFFFFFFull),
		       (unsigned long long)(v[7] >> 32), (unsigned long long)(v[7] & 0xFFFFFFFFull),
		       (unsigned long long)v[8], (unsigned long long)v[9]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 55) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_root_write(argScalar, v, kAccelExtraScalars);
		N48LOG("accel-experiment: rootwrite — %s; guard refused %llu; root %#llx; wrote "
		       "%#llx, read back %#llx; slot 511 before %#llx; L1 block vram+%#llx; ring VA "
		       "%#llx; hdp/tlb %llu; fault status after %#llx; withdrawals %llu; pageOffPD "
		       "hooks %llu; fires %llu over %llu patched context(s)",
		       st == 0 ? "REFUSED (nothing was written)"
		               : st == 1 ? "REPORT ONLY (nothing was written)"
		               : st == 2 ? "WRITTEN and verified"
		                         : "WRITTEN but a read-back MISMATCHED",
		       (unsigned long long)v[1], (unsigned long long)v[2],
		       (unsigned long long)v[3], (unsigned long long)v[4],
		       (unsigned long long)v[5], (unsigned long long)v[6],
		       (unsigned long long)v[7], (unsigned long long)v[8],
		       (unsigned long long)v[9], (unsigned long long)v[10],
		       (unsigned long long)v[11], (unsigned long long)(v[12] >> 32),
		       (unsigned long long)(v[12] & 0xFFFFFFFFull));
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 51) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		// 0.0.266: mode 3 (arm + accept register adjustments) was added in 0.0.265 at
		// five of rule 10's six sites and MISSED HERE, so `accel shadercache 3` was clamped to 0 —
		// the read-only query — and r100 ran with the cache never armed, blob never opened, and
		// reported "0 resource(s) scanned" as though that were a measurement. This clamp is exactly
		// the "appears to run but does nothing" failure rule 10 names.
		const uint32_t mode = (argScalar >= 1 && argScalar <= 3) ? (uint32_t)argScalar : 0u;
		const uint32_t st = navi48_shadercache_control(mode, v, kAccelExtraScalars);
		N48LOG("accel-experiment: shadercache mode %u — state %#x (1 armed, 2 blob open, 4 residency "
		       "hook live, 8 adjustments accepted); %llu entr(y/ies), %llu resource(s) scanned, %llu "
		       "substituted, %llu read-back mismatch(es), %llu refused", mode, st,
		       (unsigned long long)v[1], (unsigned long long)v[2],
		       (unsigned long long)v[6], (unsigned long long)v[7],
		       (unsigned long long)(v[8] & 0xFFFFFFFFull));
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 47 — `vmpage [va]` (0.0.206), read-only and writes nothing anywhere.
	//   vmib's walk of Apple's VMID-2 table for argScalar (0 = blit2's destination V#
	//   base 0x400004000), then the WHOLE 4 KiB page from offset 0: its nonzero lines
	//   logged, the blit_diag_gfx1201 records decoded (sentinels 0x600d600d /
	//   0x600d0b0b), with a planted-buffer self-test of the decoder as the positive
	//   control. Every result rides the out-scalars (rule 27: never only the log).
	if (action == 47) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t n = n48::hw_hook_vm_page_scan(argScalar, v, kAccelExtraScalars);
		static const char *why[] = { "refused (see the REFUSING line above)",
		                             "root PDE not valid", "L1 entry not valid",
		                             "leaf entry not valid", "the page could not be read",
		                             "scanned a VRAM page", "scanned a HOST page" };
		N48LOG("accel-experiment: vmpage — %s; self-test %s; %llu nonzero dwords; "
		       "sentinel hits A %llu B %llu", why[n <= 6 ? n : 0], v[2] ? "ok" : "FAILED",
		       (unsigned long long)v[3], (unsigned long long)v[4], (unsigned long long)v[5]);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	// action 48 — `renderxlat`: after xlatregs and BEFORE sdmamap, find Apple's render IB by content
	//   among the page-table leaves pending in its SDMA IBs (rule 65). argScalar low byte 0 = census + dump,
	//   3 = BLANK (instrument), 1 = translate in place; 0x100 seeds NGG; 2 is refused. Default: nothing runs.
	if (action == 48) {
		uint64_t v[kAccelExtraScalars] = { 0 };
		const uint32_t st = n48::hw_hook_render_xlat(argScalar, v, kAccelExtraScalars);
		N48LOG("accel-experiment: renderxlat arg %#llx — %s (self-test %llu, candidate VA %#llx, write status %llu, "
		       "out %llu dw, candidates %llu)", (unsigned long long)argScalar,
		       st == 5 ? "census" : st == 1 ? "WRITTEN" : st == 2 ? "already written" : st == 3 ? "read-back MISMATCH" : "REFUSED",
		       (unsigned long long)((v[1] >> 8) & 1), (unsigned long long)v[2], (unsigned long long)v[11] & 0xFFFF,
		       (unsigned long long)(v[11] >> 16) & 0xFFFF, (unsigned long long)v[5] & 0xFF);
		if (outExtra)
			for (unsigned i = 0; i < kAccelExtraScalars; i++) outExtra[i] = v[i];
		return kIOReturnSuccess;
	}
	if (action == 33) {
		const uint32_t n = n48::hw_hook_kiq_enable();
		N48LOG("accel-experiment: kiq-enable — %s",
		       n ? "KIQ ring ENABLED" : "refused, or still disabled");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 31) {
		const uint32_t n = n48::hw_hook_pm4_power_up();
		N48LOG("accel-experiment: pm4-powerup — %s",
		       n ? "engine+0x340 POPULATED" : "refused, or still NULL");
		if (outInstalled) *outInstalled = n;
		//: the driver log hits capacity during powerUp (Apple's isDeviceValid
		// storm) and drops the closing lines, so carry the result back through an
		// out-scalar where no flood can erase it. navi48test prints out[2] as
		// "first unsupported slot" whenever it is >= 0.
		if (outFirstUnsupported) *outFirstUnsupported = (uint64_t)n;
		return kIOReturnSuccess;
	}
	if (action == 30) {
		const uint32_t n = n48::hw_hook_resume_scheduler();
		N48LOG("accel-experiment: resume — %s", n ? "scheduler RUNNING" : "refused or still paused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 29) {
		const bool ranP = navi48_sdma_srbm_probe(0x2046, 0xA5A50001u, 0x5A5A0002u, 0x5A5A0003u);
		N48LOG("accel-experiment: srbm-probe — %s", ranP ? "ran" : "refused");
		return ranP ? kIOReturnSuccess : kIOReturnNotReady;
	}
	if (action == 28) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: xlat-regs refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_translate_gcvm_regs();
		N48LOG("accel-experiment: xlat-regs — %u operand(s) translated", n);
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 27) {
		// — run Apple's own retry pass (what timerCallback calls).
		const uint32_t n = n48::hw_hook_run_advance();
		N48LOG("accel-experiment: run-advance — %s", n ? "called" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 26) {
		// — run Apple's own retire pass once (nothing fabricated).
		const uint32_t n = n48::hw_hook_run_checktimestamps();
		N48LOG("accel-experiment: run-checkts — %s", n ? "called" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 25) {
		// — read-only: Apple's stamp TARGET vs the HARDWARE stamp.
		const uint32_t n = n48::hw_hook_stamp_gap();
		N48LOG("accel-experiment: stamp-gap — %s", n ? "measured" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 24) {
		// — call the sole writer of the stamp array (identity-checked).
		const uint32_t n = n48::hw_hook_signal_stamp();
		N48LOG("accel-experiment: signal-stamp — %s", n ? "called" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 23) {
		// — read-only dump of the stamp array waitForStamp resolves against.
		const uint32_t n = n48::hw_hook_stamp_state();
		N48LOG("accel-experiment: stamp-state — %s", n ? "dumped" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 22) {
		// — read-only dump of the scheduler's HW-channel slot for chan 14.
		const uint32_t n = n48::hw_hook_sched_state();
		N48LOG("accel-experiment: sched-state — %s", n ? "dumped" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 21) {
		// — register chan 14 so timestampUpdated reaches the retire loop.
		const uint32_t n = n48::hw_hook_bind_channel();
		N48LOG("accel-experiment: bind-channel — %s", n ? "bound" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 20) {
		// — call the reader Apple never calls. See hw_hook_signal_completion.
		const uint32_t n = n48::hw_hook_signal_completion();
		N48LOG("accel-experiment: signal-completion — %s",
		       n ? "timestampUpdated invoked" : "refused");
		if (outInstalled) *outInstalled = n;
		return kIOReturnSuccess;
	}
	if (action == 19) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: poke-completion refused — fire first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_poke_completion();
		N48LOG("accel-experiment: poke-completion — %s", n ? "done" : "refused");
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 18 —: dump the completion dword Apple's scheduler polls.
	if (action == 18) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: chan-state refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_dump_channel_completion();
		N48LOG("accel-experiment: chan-state — %u channel(s)", n);
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 17 —: neuter the VM-flush ack poll that can never satisfy.
	if (action == 17) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: poll-neuter refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_neuter_register_polls();
		N48LOG("accel-experiment: poll-neuter — %u poll(s) made always-true", n);
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 16 —: open the COND_EXE gate Apple left clear.
	if (action == 16) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: open-gate refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_open_cond_exec_gate();
		N48LOG("accel-experiment: open-gate — %s", n ? "gate open" : "refused");
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 15 — read-only QUEUE1 register dump (does NOT reset the pointers).
	if (action == 15) {
		const bool ok = navi48_sdma_log_external_queue("peek");
		N48LOG("accel-experiment: queue-state — %s", ok ? "dumped" : "unavailable");
		return ok ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 14 — read the COND_EXE gate and FENCE target. Read-only.
	if (action == 14) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ring-refs refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_dump_ring_refs();
		N48LOG("accel-experiment: ring-refs — %u reference(s) read", n);
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 13 — THE LAST STEP: ring the doorbell. The engine fetches.
	if (action == 13) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ext-kick refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_kick_apple_queue();
		N48LOG("accel-experiment: ext-kick — %s", n ? "doorbell rung" : "refused");
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 12 — step 3 FINAL: program the queue with RB_ENABLE=1.
	// This is the step where the GPU actually fetches and executes Apple's work.
	// Deliberately separate from action 8 so enabling is always an explicit act.
	if (action == 12) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: program-queue(enabled) refused — fire first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_program_apple_queue(/*enableRing=*/true);
		N48LOG("accel-experiment: program-queue ENABLED — %s",
		       n ? "RB_ENABLE=1" : "refused");
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 11 —: rebase Apple's IB CONST_FILL destinations by fb_start.
	if (action == 11) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ib-rebase refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_rebase_ib_fills();
		N48LOG("accel-experiment: ib-rebase — %u packet(s) rebased", n);
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 10 —: dump the indirect buffers the ring points at.
	if (action == 10) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ib-dump refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_dump_indirect_buffers();
		N48LOG("accel-experiment: ib-dump — %u indirect buffer(s)", n);
		return kIOReturnSuccess;
	}
	// action 9 — install observation-only ring hooks.
	if (action == 9) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ring-hooks refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_install_ring_hooks();
		N48LOG("accel-experiment: ring-hooks — %s", n ? "installed (observation only)" : "failed");
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 8 — step 3 (SAFE): program SDMA1 QUEUE1 at Apple's ring,
	// RB_ENABLE=0, no doorbell. Refuses unless the ring page is RESIDENT.
	if (action == 8) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: program-queue refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_program_apple_queue();
		N48LOG("accel-experiment: program-queue — %s", n ? "programmed (inert)" : "refused");
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	// action 7 — read-only: dump what Apple wrote into the rings.
	if (action == 7) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ring-dump refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_dump_rings();
		N48LOG("accel-experiment: ring-dump — %u ring(s) had content", n);
		return kIOReturnSuccess;
	}
	// action 6 — read-only ring state. Is Apple filling the rings?
	if (action == 6) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: ring-state refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_report_ring_state();
		N48LOG("accel-experiment: ring-state — %u ring(s) show activity", n);
		return kIOReturnSuccess;
	}
	if (action == 5) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: start-engines refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t n = n48::hw_hook_start_hw_engines();
		N48LOG("accel-experiment: start-engines left %u ring(s) with a doorbell", n);
		return n ? kIOReturnSuccess : kIOReturnNotReady;
	}
	if (action == 4) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: enable-rings refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t enabled = n48::hw_hook_enable_sdma_rings();
		N48LOG("accel-experiment: enable-rings enabled %u SDMA ring(s)", enabled);
		return enabled ? kIOReturnSuccess : kIOReturnNotReady;
	}
	if (action == 3) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: gart-sync refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		const uint32_t synced = n48::hw_hook_sync_apple_gart();
		N48LOG("accel-experiment: gart-sync reformatted %u Apple GART pages into "
		       "our GFX12 table", synced);
		return synced ? kIOReturnSuccess : kIOReturnNotReady;
	}
	if (action == 2) {
		if (!accelExperimentFired) {
			N48LOG("accel-experiment: re-drive refused — fire the experiment first");
			return kIOReturnNotReady;
		}
		N48LOG("accel-experiment: re-driving setVirtualSpaceReady(true) then "
		       "setMemoryAllocationsEnabled(true)");
		n48::hw_hook_enable_vram_allocations();
		n48::hw_hook_set_virtual_space_ready(false);   // seeds the VRAM watermarks
		const bool a = n48::hw_hook_set_virtual_space_ready(true);
		const bool b = n48::hw_hook_enable_memory_allocations(true);
		return (a && b) ? kIOReturnSuccess : kIOReturnNotReady;
	}

	if (!accelExperimentArmed) {
		N48LOG("accel-experiment: refused — boot without navi48-accel-experiment=1 is not armed");
		return kIOReturnNotPermitted;
	}
	if (accelExperimentFired) return kIOReturnSuccess;     // idempotent

	gNavi48Ttl.bind(this);
	n48::TtlHookResult hook = n48::install_ttl_hook(pciDevice, &gNavi48Ttl);
	setProperty("Navi48,TtlHook", hook.why);
	if (!hook.installed) {
		N48LOG("accel-experiment: TTL hook FAILED (%s) — NOT publishing LoadAccelerator.", hook.why);
		setProperty("Navi48,AccelExperiment", "fire aborted: TTL hook failed");
		return kIOReturnNotReady;
	}

	// ---- MILESTONE 3 step 2 (0.0.236): THE EXPOSURE GATE ----
	//
	// Publishing LoadAccelerator is the single switch that hands this card to
	// every waiting Metal client. Apple's accelerator personalities all carry
	// IOPropertyMatch { LoadAccelerator }, so nothing matches until we publish;
	// the moment we do, WindowServer, loginwindow and SecurityAgent each open an
	// AMDAccelDevice and an AMDAccelSharedUserClient (run disp1). That makes this
	// property, and not any hardware register, the thing that decides whether a
	// half-working driver freezes the machine.
	//
	// CONFIRMED what happens when it is published too early: on an armed
	// boot with no takeover, Apple's own hang detection fires during the cycle
	// itself and every Metal client then blocks in uninterruptible wait — Safari,
	// WebKit's GPU process, NotificationCenter, bluetoothd, system_profiler. The
	// control run on the same boot image with the accelerator NOT armed works
	// fine on software rendering. So the safe posture is not "publish and hope",
	// it is "publish only after something has actually completed end to end".
	//
	// The self-test is the stage-17 compute dispatch the ladder already runs on
	// every boot, long before this point: a real dispatch on this silicon whose
	// lanes are checked (Navi48,ComputeDispatch / Navi48,ComputeLanesCorrect). It
	// costs nothing extra, it is already proven on this hardware, and if it did
	// not pass then nothing else on this card is going to work either.
	//
	// navi48-accel-gate:
	//   0  FORCED CLOSED — never publish. The control: no accelerator nub,
	//      no Metal device, the desktop stays on software rendering. This is the
	//      escape hatch that does not need a config swap or the rescue stick.
	//   1  DEFAULT — publish only if the self-test passed. Since the dispatch
	//      passes on every boot, this is behaviourally identical to 0.0.235.
	//   2  FORCED OPEN — publish unconditionally, exactly as 0.0.234 and earlier
	//      did. Kept for diagnosis of a boot where the ladder itself failed.
	// The boot-arg is the production mechanism. `fire <mode>` is the TEST lever, so
	// the gate can be proven BOTH ways without swapping the boot config: argScalar
	// 1 forces it closed for this fire, 2 forces it open, 0 (an older navi48test
	// sends nothing, which lands here as 0) defers to the boot-arg.
	uint32_t gate = 1;
	PE_parse_boot_argn("navi48-accel-gate", &gate, sizeof(gate));
	if (argScalar == 1) { gate = 0; N48LOG("accel-gate: forced CLOSED by `fire 1`"); }
	else if (argScalar == 2) { gate = 2; N48LOG("accel-gate: forced OPEN by `fire 2`"); }
	const bool selfTestPassed = gBringup.computePassed &&
	                            gBringup.reached >= amdgpu::BringupStage::ComputeDispatch;
	N48LOG("accel-gate: mode %u (0 closed, 1 self-test, 2 forced open); in-kext self-test "
	       "(stage-17 compute dispatch) %s, stage reached %u",
	       gate, selfTestPassed ? "PASSED" : "did NOT pass", (unsigned)gBringup.reached);

	if (gate == 0) {
		N48LOG("accel-gate: CLOSED by navi48-accel-gate=0 — NOT publishing LoadAccelerator. "
		       "Apple's accelerator cannot match, no Metal device is exposed, and the desktop "
		       "keeps software rendering. The TTL hook stays installed.");
		setProperty("Navi48,AccelGate", "closed: navi48-accel-gate=0, LoadAccelerator withheld");
		setProperty("Navi48,AccelExperiment", "fire refused: exposure gate closed");
		return kIOReturnNotPermitted;
	}
	if (gate != 2 && !selfTestPassed) {
		N48LOG("accel-gate: CLOSED — the in-kext self-test did not pass, so LoadAccelerator is "
		       "WITHHELD. Exposing the accelerator now would hand the GPU to every Metal client "
		       "on a card that cannot complete work. Boot navi48-accel-gate=2 to override "
		       "for diagnosis.");
		setProperty("Navi48,AccelGate", "closed: self-test did not pass, LoadAccelerator withheld");
		setProperty("Navi48,AccelExperiment", "fire refused: self-test did not pass");
		return kIOReturnNotReady;
	}

	N48LOG("accel-experiment: TTL hooked and the exposure gate is OPEN (%s); publishing "
	       "LoadAccelerator. Apple's AMDRadeonX6000 may now match this card and will reach "
	       "hardware through Navi48Ttl.",
	       gate == 2 ? "FORCED OPEN by navi48-accel-gate=2, self-test NOT required"
	                 : "in-kext self-test passed");
	pciDevice->setProperty("LoadAccelerator", kOSBooleanTrue);
	setProperty("Navi48,AccelGate", gate == 2
	            ? "open: forced by navi48-accel-gate=2 (self-test not required)"
	            : "open: in-kext self-test passed, LoadAccelerator published");
	setProperty("Navi48,AccelExperiment", "fired: LoadAccelerator published (TTL hooked)");
	pciDevice->registerService();
	accelExperimentFired = true;
	if (outInstalled) *outInstalled = 1;
	return kIOReturnSuccess;
}

void Navi48Bringup::stop(IOService *provider) {
	// : never leave a patched className behind in a kext that is going away.
	n48fbname::restore_on_unload();
	// 0.0.603 (native S2a): the console plane back and every scanout watchdog thread finished BEFORE anything is unmapped (both need the registers).
	n48dcn::scanShutdown();
	// 0.0.610 (milestone #9): a published Metal nub (and the aux accelerator under it) goes before the GPU state does. No-op unless the nub was published.
	Navi48DisplayNub::shutdown();      // 0.0.652 (M5): the framebuffer's nub first (no-op when never published)
	Navi48MetalNub::shutdown();
	// Quiesce the GPU before any of its memory goes away: a mapped MES queue
	// or a running CP would keep fetching from buffers IOFree is about to
	// reclaim. No-op if the ladder never ran.
	amdgpu::bringup_teardown(gBringup);
	disarmInterrupts();
	if (bar2Map) { bar2Map->release(); bar2Map = nullptr; dev.bar2 = nullptr; dev.bar2Size = 0; }
	unmapVramAperture();
	unmapRegisters();
	super::stop(provider);
}

void Navi48Bringup::free() {
	if (onDieDisc) { IOFree(onDieDisc, n48::kDiscTmrSize); onDieDisc = nullptr; }
	super::free();
}
