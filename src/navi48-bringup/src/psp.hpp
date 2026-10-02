//
//  psp.hpp — PSP (Platform Security Processor) bootloader chain for Navi 48.
//
//  Ports mac-amdgpu's psp_v14_0 pre-SOS sequence (MIT) — itself a port of
//  Linux psp_v14_0.c — into a freestanding loader driven through a small
//  hardware-access interface: the bring-up kext supplies register and VRAM
//  access, this module owns the protocol.
//
//  What it does (the FIRST stage that writes to the GPU):
//    1. Parse psp_14_0_3_sos.bin (a v2.0 container of 11 sub-images).
//    2. For each pre-SOS component, in upstream order — KDB, SPL, SYS_DRV,
//       SOC_DRV, INTF_DRV, DBG/HAD_DRV, RAS_DRV, IPKEYMGR — stage it in a
//       1 MiB, 1 MiB-aligned fw_pri window in VRAM (written through the BAR0
//       aperture), hand the bootloader its MC address (>>20) in C2PMSG_36 and
//       the load command in C2PMSG_35, then wait for "bootloader ready".
//    3. Load SOS the same way; completion is C2PMSG_81 changing to nonzero.
//
//  MC addressing (hardware-proven on this card by RDNA4FB's cursor readback):
//  BAR0 byte offset x is VRAM offset x is MC address FB_LOCATION_BASE<<24 + x.
//
//  Safety: the bootloader validates every image's signature; a bad address or
//  image yields an error status / timeout, not damage. This is the sequence
//  every Linux boot performs. Still: gated, preflighted, tested on hardware.
//
#ifndef Navi48_psp_hpp
#define Navi48_psp_hpp

#include <stdint.h>
#include <stddef.h>

namespace n48 {

// Minimal hardware access the loader needs; implemented by the kext.
struct HwAccess {
	// MMIO register access resolved through IP discovery:
	// (hwId, instance, segment, dwordOffset) -> value. 0xFFFFFFFF on failure.
	virtual uint32_t regReadIp (uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword) = 0;
	virtual bool     regWriteIp(uint16_t hwId, uint8_t inst, uint8_t seg, uint32_t dword, uint32_t value) = 0;
	// CPU writes into VRAM through the BAR0 aperture, at a VRAM byte offset.
	// Must be globally visible (fenced) on return.
	virtual bool vramWrite (uint64_t vramOffset, const void *src, size_t len) = 0;
	virtual bool vramMemset(uint64_t vramOffset, uint8_t value, size_t len) = 0;
protected:
	~HwAccess() = default;   // never destroyed through the interface (kext ABI: no deleting dtors)
};

struct SubBin {
	const uint8_t *data { nullptr };
	uint32_t size { 0 };
	uint32_t version { 0 };
	bool present() const { return data != nullptr && size != 0; }
};

class PspLoader {
public:
	static constexpr uint32_t kFwPriSize = 1024u * 1024u;   // PSP_1_MEG

	explicit PspLoader(HwAccess &hw) : hw(hw) {}

	// Parse the sos.bin container (header v2.0/v2.1). Returns false on a
	// malformed container. Keeps pointers into `fw`; caller owns it.
	bool parseContainer(const uint8_t *fw, size_t fwSize);

	// Where in VRAM (byte offset, 1 MiB aligned) the 1 MiB fw_pri staging
	// window lives. The kext picks a spot clear of the boot framebuffer.
	bool setFwPriOffset(uint64_t vramOffset);
	uint64_t fwPriOffset() const { return fwPriOff; }

	// Read-only preflight: MP0 bootloader ready? SOS already alive? MMHUB FB
	// location posted and fw_pri inside it? Returns true if a load may proceed.
	bool preflight();

	// Run the full pre-SOS chain then load SOS. Returns true when SOS reports
	// alive (C2PMSG_81 nonzero). Only call after preflight() returned true.
	bool runChain();

	// State / report
	bool     sosAlive() const;                 // live register check
	uint64_t mcBase() const { return vramMcBase; }         // FB_LOCATION_BASE << 24
	uint64_t fwPriMC() const { return vramMcBase + fwPriOff; }
	uint32_t sosVersion() const { return sos.version; }
	uint32_t lastBootloaderStatus() const { return lastStatus; }
	const char *lastStep() const { return lastStepName; }

	// Parsed sub-images (public for logging/inspection)
	SubBin kdb, spl, sys, sos, soc, intf, dbg, ras, ipkey, toc, rl, spdm;

private:
	bool waitBootloader(uint32_t budgetMs, uint32_t *lastValue);
	bool loadComponent(const char *name, const SubBin &bin, uint32_t blCmd);
	bool loadSOS();

	HwAccess &hw;
	uint64_t vramMcBase { 0 };      // MC address of VRAM offset 0
	uint64_t vramMcTop  { 0 };      // MC address one past the last VRAM byte
	uint64_t fwPriOff   { 0 };      // VRAM byte offset of fw_pri
	uint32_t lastStatus { 0 };
	const char *lastStepName { "none" };
};

} // namespace n48

#endif /* Navi48_psp_hpp */
