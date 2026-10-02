//
//  psp.cpp — PSP bootloader chain (see psp.hpp). Protocol per Linux
//  psp_v14_0.c via mac-amdgpu's macOS port; register offsets from the
//  mp_14_0_2 / mmhub_4_1_0 headers, resolved at runtime through IP discovery.
//
#include "psp.hpp"
#include "regs.hpp"
#include "ipdiscovery.hpp"
#include <IOKit/IOLib.h>

#define PSPLOG(fmt, ...) IOLog("Navi48Bringup: psp: " fmt "\n", ##__VA_ARGS__)

namespace n48 {

// ---- firmware container layout (1:1 with Linux amdgpu_ucode.h) --------------
struct __attribute__((packed)) CommonFwHeader {
	uint32_t size_bytes;
	uint32_t header_size_bytes;
	uint16_t header_version_major;
	uint16_t header_version_minor;
	uint16_t ip_version_major;
	uint16_t ip_version_minor;
	uint32_t ucode_version;
	uint32_t ucode_size_bytes;
	uint32_t ucode_array_offset_bytes;
	uint32_t crc32;
};
static_assert(sizeof(CommonFwHeader) == 32, "CommonFwHeader must be 32 bytes");

struct __attribute__((packed)) PspFwBinDesc {
	uint32_t fw_type;
	uint32_t fw_version;
	uint32_t offset_bytes;    // relative to ucode_array_offset_bytes
	uint32_t size_bytes;
};
static_assert(sizeof(PspFwBinDesc) == 16, "PspFwBinDesc must be 16 bytes");

enum PspFwType : uint32_t {
	kFwSOS = 1, kFwSYS_DRV = 2, kFwKDB = 3, kFwTOC = 4, kFwSPL = 5, kFwRL = 6,
	kFwSOC_DRV = 7, kFwINTF_DRV = 8, kFwDBG_DRV = 9, kFwRAS_DRV = 10,
	kFwIPKEYMGR_DRV = 11, kFwSPDM_DRV = 12,
};

// ---- PSP bootloader commands (psp_v14_0.c PSP_BL__*) --------------------------
namespace BlCmd {
	static constexpr uint32_t LoadKeyDatabase = 0x00080000;
	static constexpr uint32_t LoadTosSPLTable = 0x10000000;
	static constexpr uint32_t LoadSysDrv      = 0x00010000;
	static constexpr uint32_t LoadSocDrv      = 0x000B0000;
	static constexpr uint32_t LoadIntfDrv     = 0x000D0000;
	static constexpr uint32_t LoadHADDrv      = 0x000C0000;   // "dbg_drv was renamed to had_drv in psp v14"
	static constexpr uint32_t LoadRASDrv      = 0x000E0000;
	static constexpr uint32_t LoadIPKeyMgrDrv = 0x000F0000;
	static constexpr uint32_t LoadSOSDrv      = 0x00020000;
}

// MP0 (PSP) scratch regs, segment 0.
static constexpr uint8_t  kMp0Seg   = 0;
static constexpr uint32_t kC2PMsg35 = n48::kPspC2PMsg35;   // cmd / status (bit31 = ready)
static constexpr uint32_t kC2PMsg36 = 0x0064;              // fw_pri MC addr >> 20
static constexpr uint32_t kC2PMsg81 = n48::kPspC2PMsg81;   // SOS sign-of-life
// MMHUB (mmhub_4_1_0), segment 0.
static constexpr uint8_t  kMmhubSeg = 0;
static constexpr uint32_t kMmhubFbLocationBase = 0x0554;   // regMMMC_VM_FB_LOCATION_BASE
static constexpr uint32_t kMmhubFbLocationTop  = 0x0555;   // regMMMC_VM_FB_LOCATION_TOP
static constexpr uint32_t kMmhubFbOffset       = 0x04c7;   // regMMMC_VM_FB_OFFSET (per mac-amdgpu)
// DCN's own copy of the FB location (DMU segment 2) — what RDNA4FB uses.
static constexpr uint8_t  kDmuSeg = 2;
static constexpr uint32_t kDcnVmFbLocationBase = 0x0475;
static constexpr uint32_t kFbMask  = 0x00FFFFFF;
static constexpr uint32_t kFbShift = 24;                    // 16 MiB units

// ---- container parse ----------------------------------------------------------
bool PspLoader::parseContainer(const uint8_t *fw, size_t fwSize) {
	kdb = spl = sys = sos = soc = intf = dbg = ras = ipkey = toc = rl = spdm = SubBin{};
	if (!fw || fwSize < sizeof(CommonFwHeader)) {
		PSPLOG("parse: container missing or too small (%zu)", fwSize);
		return false;
	}
	const auto *h = reinterpret_cast<const CommonFwHeader *>(fw);
	if (h->header_version_major != 2) {
		PSPLOG("parse: unsupported header v%u.%u (need v2.x for psp_v14)",
		       h->header_version_major, h->header_version_minor);
		return false;
	}
	const uint32_t ucodeOff = h->ucode_array_offset_bytes;
	if (ucodeOff > fwSize) {
		PSPLOG("parse: ucode_array_offset %u > size %zu", ucodeOff, fwSize);
		return false;
	}
	// v2.0: count at +32, descriptors at +36. v2.1 adds an aux index at +36,
	// descriptors at +40.
	size_t countOff = sizeof(CommonFwHeader);
	size_t descOff  = countOff + 4 + (h->header_version_minor >= 1 ? 4 : 0);
	if (descOff > fwSize) return false;
	uint32_t count;
	__builtin_memcpy(&count, fw + countOff, 4);
	if (count == 0 || count > 32 || descOff + (size_t)count * sizeof(PspFwBinDesc) > fwSize) {
		PSPLOG("parse: implausible bin count %u", count);
		return false;
	}
	PSPLOG("parse: header v%u.%u ip v%u.%u ucode 0x%08x, %u sub-images",
	       h->header_version_major, h->header_version_minor,
	       h->ip_version_major, h->ip_version_minor, h->ucode_version, count);

	const uint8_t *base = fw + ucodeOff;
	for (uint32_t i = 0; i < count; i++) {
		PspFwBinDesc d;
		__builtin_memcpy(&d, fw + descOff + (size_t)i * sizeof(d), sizeof(d));
		if ((uint64_t)ucodeOff + d.offset_bytes + d.size_bytes > fwSize) {
			PSPLOG("parse: sub-image %u (type %u) out of bounds", i, d.fw_type);
			return false;
		}
		SubBin b; b.data = base + d.offset_bytes; b.size = d.size_bytes; b.version = d.fw_version;
		switch (d.fw_type) {
		case kFwSOS:          sos   = b; break;
		case kFwSYS_DRV:      sys   = b; break;
		case kFwKDB:          kdb   = b; break;
		case kFwTOC:          toc   = b; break;
		case kFwSPL:          spl   = b; break;
		case kFwRL:           rl    = b; break;
		case kFwSOC_DRV:      soc   = b; break;
		case kFwINTF_DRV:     intf  = b; break;
		case kFwDBG_DRV:      dbg   = b; break;
		case kFwRAS_DRV:      ras   = b; break;
		case kFwIPKEYMGR_DRV: ipkey = b; break;
		case kFwSPDM_DRV:     spdm  = b; break;
		default: PSPLOG("parse: ignoring unknown fw_type %u", d.fw_type); break;
		}
	}
	if (!sos.present()) { PSPLOG("parse: no SOS image in container"); return false; }
	PSPLOG("parse: KDB %u SPL %u SYS %u SOC %u INTF %u DBG %u RAS %u IPKEY %u SOS %u bytes (SOS ver 0x%08x)",
	       kdb.size, spl.size, sys.size, soc.size, intf.size, dbg.size, ras.size, ipkey.size,
	       sos.size, sos.version);
	return true;
}

bool PspLoader::setFwPriOffset(uint64_t vramOffset) {
	if (vramOffset & (kFwPriSize - 1)) {
		PSPLOG("fw_pri offset 0x%llx is not 1 MiB aligned (C2PMSG_36 carries addr>>20)", vramOffset);
		return false;
	}
	fwPriOff = vramOffset;
	return true;
}

// ---- hardware protocol ----------------------------------------------------------
bool PspLoader::sosAlive() const {
	uint32_t v = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg81);
	return v != 0 && v != 0xFFFFFFFF;
}

bool PspLoader::waitBootloader(uint32_t budgetMs, uint32_t *lastValue) {
	uint32_t v = 0;
	for (uint32_t ms = 0; ms < budgetMs; ms++) {
		v = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg35);
		if (v != 0xFFFFFFFF && (v & n48::kPspBootReadyBit)) {
			if (lastValue) *lastValue = v;
			lastStatus = v;
			return true;
		}
		IOSleep(1);
	}
	if (lastValue) *lastValue = v;
	lastStatus = v;
	return false;
}

bool PspLoader::preflight() {
	lastStepName = "preflight";
	const uint32_t bl  = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg35);
	const uint32_t sol = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg81);
	const uint32_t fbBase = hw.regReadIp(IpDiscovery::HwMmhub, 0, kMmhubSeg, kMmhubFbLocationBase);
	const uint32_t fbTop  = hw.regReadIp(IpDiscovery::HwMmhub, 0, kMmhubSeg, kMmhubFbLocationTop);
	const uint32_t fbOff  = hw.regReadIp(IpDiscovery::HwMmhub, 0, kMmhubSeg, kMmhubFbOffset);
	const uint32_t dcnBase = hw.regReadIp(IpDiscovery::HwDmu, 0, kDmuSeg, kDcnVmFbLocationBase);

	PSPLOG("preflight: C2PMSG_35=0x%08x (%s) C2PMSG_81=0x%08x (%s)",
	       bl, (bl & n48::kPspBootReadyBit) ? "bootloader READY" : "bootloader NOT ready",
	       sol, sol ? "SOS alive" : "SOS down");
	PSPLOG("preflight: MMHUB FB_LOCATION base=0x%08x top=0x%08x FB_OFFSET=0x%08x  DCN FB_LOCATION base=0x%08x",
	       fbBase, fbTop, fbOff, dcnBase);

	if (bl == 0xFFFFFFFF || sol == 0xFFFFFFFF || fbBase == 0xFFFFFFFF || fbTop == 0xFFFFFFFF) {
		PSPLOG("preflight: MP0/MMHUB registers unreadable; refusing"); return false;
	}
	vramMcBase = (uint64_t)(fbBase & kFbMask) << kFbShift;
	vramMcTop  = ((uint64_t)(fbTop & kFbMask) << kFbShift) + (1ULL << kFbShift);
	if (vramMcBase == 0 || vramMcTop <= vramMcBase) {
		PSPLOG("preflight: FB_LOCATION not posted (base 0x%llx top 0x%llx); refusing", vramMcBase, vramMcTop);
		return false;
	}
	if (dcnBase != 0xFFFFFFFF && (dcnBase & kFbMask) != (fbBase & kFbMask))
		PSPLOG("preflight: WARNING DCN and MMHUB disagree on FB base (0x%06x vs 0x%06x)",
		       dcnBase & kFbMask, fbBase & kFbMask);
	if (fbOff != 0xFFFFFFFF && (fbOff & kFbMask) != (fbBase & kFbMask))
		PSPLOG("preflight: NOTE FB_OFFSET 0x%06x != FB_LOCATION_BASE 0x%06x — using FB_LOCATION_BASE "
		       "(BAR0 offset == MC offset is hardware-proven on this card by RDNA4FB)",
		       fbOff & kFbMask, fbBase & kFbMask);

	const uint64_t mc = vramMcBase + fwPriOff;
	if (mc + kFwPriSize > vramMcTop) {
		PSPLOG("preflight: fw_pri MC 0x%llx outside VRAM [0x%llx, 0x%llx); refusing", mc, vramMcBase, vramMcTop);
		return false;
	}
	PSPLOG("preflight: VRAM MC [0x%llx, 0x%llx) = %llu MiB; fw_pri at vram+0x%llx = MC 0x%llx (C2PMSG_36 will be 0x%08x)",
	       vramMcBase, vramMcTop, (vramMcTop - vramMcBase) >> 20, fwPriOff, mc, (uint32_t)(mc >> 20));

	if (sol != 0)        { PSPLOG("preflight: SOS already alive — nothing to load"); return true; }
	if (!(bl & n48::kPspBootReadyBit)) { PSPLOG("preflight: bootloader not ready; refusing"); return false; }
	if (!sos.present())  { PSPLOG("preflight: container not parsed; refusing"); return false; }
	return true;
}

bool PspLoader::loadComponent(const char *name, const SubBin &bin, uint32_t blCmd) {
	lastStepName = name;
	if (!bin.present()) { PSPLOG("%s: not in container, skipping", name); return true; }
	if (bin.size > kFwPriSize) { PSPLOG("%s: %u bytes exceeds fw_pri", name, bin.size); return false; }
	if (sosAlive()) { PSPLOG("%s: SOS already alive, bootloader window closed — skip", name); return true; }

	uint32_t st;
	if (!waitBootloader(10000, &st)) { PSPLOG("%s: bootloader not ready (0x%08x)", name, st); return false; }

	// Zero the whole 1 MiB window first, then copy the image (psp_copy_fw).
	if (!hw.vramMemset(fwPriOff, 0, kFwPriSize))   { PSPLOG("%s: fw_pri memset failed", name); return false; }
	if (!hw.vramWrite(fwPriOff, bin.data, bin.size)) { PSPLOG("%s: fw_pri write failed", name); return false; }

	const uint64_t mc = vramMcBase + fwPriOff;
	if (!hw.regWriteIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg36, (uint32_t)(mc >> 20)) ||
	    !hw.regWriteIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg35, blCmd)) {
		PSPLOG("%s: register write failed", name); return false;
	}
	if (!waitBootloader(10000, &st)) {
		PSPLOG("%s: FAILED — bootloader status 0x%08x after cmd 0x%x", name, st, blCmd);
		return false;
	}
	PSPLOG("%s: ok (%u bytes, cmd 0x%x, status 0x%08x)", name, bin.size, blCmd, st);
	return true;
}

bool PspLoader::loadSOS() {
	lastStepName = "SOS";
	if (sosAlive()) { PSPLOG("SOS: already alive"); return true; }
	if (sos.size > kFwPriSize) { PSPLOG("SOS: %u bytes exceeds fw_pri", sos.size); return false; }
	uint32_t st;
	if (!waitBootloader(10000, &st)) { PSPLOG("SOS: bootloader not ready (0x%08x)", st); return false; }

	if (!hw.vramMemset(fwPriOff, 0, kFwPriSize)) return false;
	if (!hw.vramWrite(fwPriOff, sos.data, sos.size)) return false;

	const uint32_t solBefore = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg81);
	const uint64_t mc = vramMcBase + fwPriOff;
	if (!hw.regWriteIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg36, (uint32_t)(mc >> 20)) ||
	    !hw.regWriteIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg35, BlCmd::LoadSOSDrv)) {
		PSPLOG("SOS: register write failed"); return false;
	}
	IOSleep(20);   // upstream: "there might be handshake issue with hardware which needs delay"

	// Done when the sign-of-life register changes from its pre-load value to
	// nonzero. Normally well under a second; allow 5 s.
	for (uint32_t ms = 0; ms < 5000; ms++) {
		uint32_t v = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg81);
		if (v != solBefore && v != 0 && v != 0xFFFFFFFF) {
			PSPLOG("SOS ALIVE — C2PMSG_81 0x%08x -> 0x%08x after ~%u ms", solBefore, v, ms + 20);
			lastStatus = v;
			return true;
		}
		IOSleep(1);
	}
	lastStatus = hw.regReadIp(IpDiscovery::HwMp0, 0, kMp0Seg, kC2PMsg35);
	PSPLOG("SOS: timeout — C2PMSG_81 unchanged (0x%08x), bootloader status 0x%08x", solBefore, lastStatus);
	return false;
}

bool PspLoader::runChain() {
	if (sosAlive()) { PSPLOG("chain: SOS already alive, skipping"); return true; }
	struct Step { const char *name; const SubBin *bin; uint32_t cmd; } steps[] = {
		// Order from Linux psp_hw_start; identical to mac-amdgpu's proven
		// psp_load_sos_package (no SPDM_DRV: psp_14_0_3 ships none).
		{ "KDB",      &kdb,   BlCmd::LoadKeyDatabase },
		{ "SPL",      &spl,   BlCmd::LoadTosSPLTable },
		{ "SYS_DRV",  &sys,   BlCmd::LoadSysDrv      },
		{ "SOC_DRV",  &soc,   BlCmd::LoadSocDrv      },
		{ "INTF_DRV", &intf,  BlCmd::LoadIntfDrv     },
		{ "HAD_DRV",  &dbg,   BlCmd::LoadHADDrv      },
		{ "RAS_DRV",  &ras,   BlCmd::LoadRASDrv      },
		{ "IPKEYMGR", &ipkey, BlCmd::LoadIPKeyMgrDrv },
	};
	for (auto &s : steps)
		if (!loadComponent(s.name, *s.bin, s.cmd)) return false;
	return loadSOS();
}

} // namespace n48
