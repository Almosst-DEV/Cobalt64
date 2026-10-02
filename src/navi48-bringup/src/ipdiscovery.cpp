//
//  ipdiscovery.cpp
//  RDNA4FB
//
//  See ipdiscovery.hpp for scope and the ROM this was validated against.
//

#include "ipdiscovery.hpp"

// binary_header (discovery.h): u32 signature, u16 version_major,
// u16 version_minor, u16 binary_checksum, u16 binary_size, then the
// table_info list (v1: at +12; v2: u16 num_tables u16 pad, list at +16).
static constexpr uint32_t kBinarySignature = 0x28211407;
// table_info: u16 offset (from binary start), u16 checksum, u16 size, u16 pad.
// Table index 0 is IP_DISCOVERY.
// ip_discovery_header: u32 "IPDS", u16 version, u16 size, u32 id,
// u16 num_dies, then die_info[16] {u16 die_id; u16 die_offset} — offsets
// relative to the *binary* start, not the table.
static constexpr uint32_t kIpdsSignature = 0x53445049; // "IPDS" LE

bool IpDiscovery::readU8(size_t off, uint8_t &v) const {
	if (off >= size) return false;
	v = data[off];
	return true;
}

bool IpDiscovery::readU16(size_t off, uint16_t &v) const {
	if (off + 2 > size) return false;
	v = static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
	return true;
}

bool IpDiscovery::readU32(size_t off, uint32_t &v) const {
	if (off + 4 > size) return false;
	v = static_cast<uint32_t>(data[off]) |
	    (static_cast<uint32_t>(data[off + 1]) << 8) |
	    (static_cast<uint32_t>(data[off + 2]) << 16) |
	    (static_cast<uint32_t>(data[off + 3]) << 24);
	return true;
}

bool IpDiscovery::validateBinary(size_t offset) {
	uint16_t vmajor, checksum, binSize;
	if (!readU16(offset + 4, vmajor) || !readU16(offset + 8, checksum) ||
	    !readU16(offset + 10, binSize))
		return false;
	if (binSize < 16 || offset + binSize > size)
		return false;

	// Checksum: 16-bit byte sum of everything after the checksum field.
	uint16_t sum = 0;
	for (size_t i = offset + 10; i < offset + binSize; i++)
		sum = static_cast<uint16_t>(sum + data[i]);
	if (sum != checksum)
		return false;

	// IP_DISCOVERY is table_list[0]; list position depends on header version.
	size_t tableList = offset + (vmajor >= 2 ? 16 : 12);
	uint16_t ipdOffset, ipdSize;
	if (!readU16(tableList, ipdOffset) || !readU16(tableList + 4, ipdSize) ||
	    ipdOffset == 0 || ipdSize < 80)
		return false;

	size_t ipd = offset + ipdOffset;
	uint32_t sig;
	uint16_t numDies;
	if (!readU32(ipd, sig) || sig != kIpdsSignature || !readU16(ipd + 12, numDies) || numDies == 0)
		return false;

	// Single-die parts only (true for all dGPUs); die_info[0] at +14.
	uint16_t dieOffset;
	if (!readU16(ipd + 16, dieOffset) || dieOffset == 0)
		return false;

	size_t die = offset + dieOffset;
	uint16_t nips;
	if (!readU16(die + 2, nips) || nips == 0 || nips > 256)
		return false;

	binBase = offset;
	dieBase = die;
	numIps = nips;
	return true;
}

bool IpDiscovery::init(const uint8_t *buffer, size_t bufferSize) {
	data = buffer;
	size = bufferSize;
	valid = false;
	if (!data || size < 512)
		return false;

	// The binary is not alignment-guaranteed within a flash dump (this ROM:
	// 0x34900); scan for the signature on 4-byte steps.
	for (size_t off = 0; off + 512 <= size; off += 4) {
		uint32_t sig;
		if (readU32(off, sig) && sig == kBinarySignature && validateBinary(off)) {
			valid = true;
			return true;
		}
	}
	return false;
}

bool IpDiscovery::ipAt(uint16_t index, IpEntry &out) const {
	if (!valid || index >= numIps)
		return false;

	// ip_v3/v4 with 32-bit bases: u16 hw_id, u8 instance, u8 num_base,
	// u8 major, u8 minor, u8 revision, u8 sub/variant, u32 bases[].
	// Walk the variable-size records to the requested index.
	size_t p = dieBase + 4;
	for (uint16_t i = 0; ; i++) {
		uint16_t hwId;
		uint8_t inst, nbase, major, minor, rev;
		if (!readU16(p, hwId) || !readU8(p + 2, inst) || !readU8(p + 3, nbase) ||
		    !readU8(p + 4, major) || !readU8(p + 5, minor) || !readU8(p + 6, rev))
			return false;

		if (i == index) {
			out.hwId = hwId;
			out.instance = inst;
			out.major = major;
			out.minor = minor;
			out.revision = rev;
			out.numBases = nbase < MaxBases ? nbase : MaxBases;
			for (uint8_t b = 0; b < out.numBases; b++)
				if (!readU32(p + 8 + 4u * b, out.bases[b]))
					return false;
			return true;
		}
		p += 8 + 4u * nbase;
	}
}

bool IpDiscovery::findIp(uint16_t hwId, uint8_t instance, IpEntry &out) const {
	for (uint16_t i = 0; i < numIps; i++) {
		if (!ipAt(i, out))
			return false;
		if (out.hwId == hwId && out.instance == instance)
			return true;
	}
	return false;
}

bool IpDiscovery::regByteOffset(uint16_t hwId, uint8_t instance, uint8_t segment,
                                uint32_t regDwordOffset, uint32_t &byteOffset) const {
	IpEntry ip;
	if (!findIp(hwId, instance, ip) || segment >= ip.numBases)
		return false;
	byteOffset = (ip.bases[segment] + regDwordOffset) * 4;
	return true;
}
