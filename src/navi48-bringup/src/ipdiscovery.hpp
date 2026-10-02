//
//  ipdiscovery.hpp
//  RDNA4FB
//
//  Parser for AMD's IP discovery binary — the authoritative per-card list of
//  IP blocks, their versions, and their register base addresses. Modern
//  amdgpu derives its whole register map from this table at runtime; we do
//  the same instead of hardcoding segment bases.
//
//  Freestanding like AtomBios: no IOKit, no allocation, reads from a caller
//  buffer. Verified against firmware/Sapphire.RX9070XT.16384.241213.rom
//  (binary v1.3 at 0x34900, IPDS version 3, 1 die, 46 IPs — GC v12.0.1,
//  DMU/DCN v4.1.0, NBIF v6.3.1) and cross-checked with Linux
//  drivers/gpu/drm/amd/include/discovery.h and amdgpu_discovery.c.
//
//  The discovery binary lives in the PSP region of the 2 MiB unified flash
//  (before the legacy VBIOS image), so at runtime it is only available when
//  the full ROM dump is injected (ATY,bin_image), not via the expansion ROM.
//  At boot the PSP also copies it to the top of VRAM, which is how amdgpu
//  reads it; that path needs working MMIO first.
//

#ifndef IpDiscovery_hpp
#define IpDiscovery_hpp

#include <stdint.h>
#include <stddef.h>

class IpDiscovery {
public:
	// Hardware IP ids (Linux soc15_hw_ip.h) we care about.
	enum HwId : uint16_t {
		HwMp1    = 1,
		HwGc     = 11,
		HwMmhub  = 34,
		HwOsssys = 40,
		HwHdp    = 41,
		HwSdma0  = 42,
		HwNbif   = 108,
		HwUmc    = 150,
		HwMp0    = 255,
		HwDmu    = 271,   // display micro-unit: the DCN register block
	};

	static constexpr size_t MaxBases = 16;   // MP0/MP1 carry 15 segments

	struct IpEntry {
		uint16_t hwId;
		uint8_t  instance;
		uint8_t  major, minor, revision;
		uint8_t  numBases;
		uint32_t bases[MaxBases];   // dword register segment base addresses
	};

	// Scans `buffer` for the discovery binary signature (0x28211407),
	// validates the binary checksum and the IPDS table, and locates die 0.
	bool init(const uint8_t *buffer, size_t bufferSize);

	bool isValid() const { return valid; }
	size_t binaryOffset() const { return binBase; }
	uint16_t ipCount() const { return numIps; }

	// Iterate all IPs: index 0..ipCount()-1.
	bool ipAt(uint16_t index, IpEntry &out) const;

	// Find a specific IP block instance.
	bool findIp(uint16_t hwId, uint8_t instance, IpEntry &out) const;

	// Convenience: absolute MMIO *byte* offset of a register, given its
	// segment index and dword offset within the segment (the BASE_IDX /
	// reg offset pair from Linux asic_reg headers). Returns false if the
	// IP or segment is absent.
	bool regByteOffset(uint16_t hwId, uint8_t instance, uint8_t segment,
	                   uint32_t regDwordOffset, uint32_t &byteOffset) const;

private:
	const uint8_t *data { nullptr };
	size_t size { 0 };
	size_t binBase { 0 };    // offset of binary_header in data
	size_t dieBase { 0 };    // offset of die_header in data
	uint16_t numIps { 0 };
	bool valid { false };

	bool validateBinary(size_t offset);

	bool readU8 (size_t off, uint8_t  &v) const;
	bool readU16(size_t off, uint16_t &v) const;
	bool readU32(size_t off, uint32_t &v) const;
};

#endif /* IpDiscovery_hpp */
