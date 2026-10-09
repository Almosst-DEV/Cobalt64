// rebar_pure.h - 0.0.663: Resizable BAR support, the PURE logic (no kernel types, no hardware access; host-tested by tests/native_rebar_test.cpp).
// Contract: an internal design note section 3. Rule: with a 256 MiB BAR0 everything behaves exactly as 0.0.662; a larger BAR maps at most 1 GiB
// (what Tahoe's IOPCIFamily can size), and ANY inconsistency refuses the native ladder (the CPU desktop through RDNA4FB, as with no BAR0).
//
// What lives here:
//   plan_bar0       the mapping length and vramLimit from the IOPCIFamily BAR0 range, the config-space BAR0 address, the ReBAR capability's current
//                   BAR0 size, the VRAM size and the console; or a named reason to refuse.
//   cfg_bar_addr    the 64-bit address of a memory BAR from its two config dwords.
//   find_ext_cap / census_read   the READ-ONLY walk of the PCIe extended capability list to the Resizable BAR capability (id 0x15).
//   bar0_latch      the union of IOPCIFamily's BAR0 range and the config-space one for the host-import guard, and whether they disagree.
//   aperture_points the three BAR0-vs-MM-window check offsets (vramBase, the middle, vramLimit - 64 KiB).
//   visible_size    gmc_mc_init's CPU-visible size.   pool_sizes   the two allocator pools.
#pragma once
#include <stdint.h>

namespace n48rebar {

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;
constexpr uint64_t kMaxBar0Map  = kGiB;            // 0.0.663 (item 1): the most BAR0 the kext ever maps. Tahoe's IOPCIFamily cannot size a BAR above 1 GiB (REBAR.md F1), so a full BAR is mapped and anything larger is capped.
constexpr uint64_t kLimitAlign  = 64ull << 10;     // vramLimit is rounded DOWN to this
constexpr uint64_t kLimitSlack  = 64ull << 20;     // vramLimit <= VRAM size - hi reserve - this
constexpr uint64_t kLegacyBar   = 256ull << 20;    // today's BAR0; above it a ReBAR capability read is REQUIRED (no capability read = no cross-check = refuse)
constexpr uint64_t kApertureTail = 64ull << 10;    // the top check sits this far below vramLimit
constexpr uint64_t kNoConsole   = ~0ull;           // PlanIn::consoleOff when the console was not located inside BAR0

enum Reason : uint32_t {
    kOk = 0,
    kLenZero = 1,            // IOPCIFamily published no BAR0 length (a BAR it could not size, e.g. above 1 GiB, reads as 0)
    kLenNotPow2 = 2,
    kAddrZero = 3,           // BAR0 unassigned
    kAddrMisaligned = 4,     // the address is not aligned to the length
    kAddrWrap = 5,           // address + length wraps the address space
    kAddrMismatch = 6,       // IOPCIFamily's BAR0 address differs from config space's
    kRebarMismatch = 7,      // the ReBAR capability's current BAR0 size differs from IOPCIFamily's length
    kNoRebarCap = 8,         // the BAR is above 256 MiB but the capability could not be read: nothing to cross-check against
    kVramTooSmall = 9,       // the VRAM size leaves no room for a visible window after the reserves
    kConsoleBeyond = 10,     // the located console lies (partly) above vramLimit
    kMapCapZero = 11,
    kRebarSizeBad = 12,      // the capability's size code is not a power of two we can hold (code >= 40)
    kConsoleOverlap = 13,    // 0.0.664 (F5): vramBase, a check point or the visible pool overlaps the console or its 64 KiB cursor
    kLenTooBig = 14          // 0.0.664 (F3): the length exceeds the map cap (1 GiB): IOPCIFamily mis-sized the BAR
};

inline const char *reason_name(uint32_t r) {
    switch (r) {
    case kOk: return "ok";                         case kLenZero: return "BAR0 length is 0";
    case kLenNotPow2: return "BAR0 length is not a power of two";
    case kAddrZero: return "BAR0 address is 0";   case kAddrMisaligned: return "BAR0 address is not aligned to its length";
    case kAddrWrap: return "BAR0 address + length wraps"; case kAddrMismatch: return "IOPCIFamily and config space disagree on the BAR0 address";
    case kRebarMismatch: return "the ReBAR capability's BAR0 size differs from IOPCIFamily's length";
    case kNoRebarCap: return "BAR0 is above 256 MiB and the ReBAR capability could not be read";
    case kVramTooSmall: return "the VRAM size leaves no visible window after the reserves";
    case kConsoleBeyond: return "the console lies above vramLimit"; case kMapCapZero: return "map cap is 0";
    case kRebarSizeBad: return "the ReBAR size code is out of range";
    case kConsoleOverlap: return "the bring-up region, a check point or the visible pool overlaps the console or its cursor";
    case kLenTooBig: return "BAR0 length is above the map cap (IOPCIFamily mis-sized the BAR)";
    default: return "?";
    }
}

constexpr bool is_pow2(uint64_t v) { return v != 0ull && (v & (v - 1ull)) == 0ull; }

struct PlanIn {
    uint64_t ioLen;          // IOPCIFamily: getDeviceMemoryWithRegister(BAR0)->getLength() (0 = absent)
    uint64_t ioAddr;         // IOPCIFamily: its physical address
    uint64_t cfgAddr;        // config space BAR0 (cfg_bar_addr)
    bool     rebarPresent;   // the Resizable BAR capability was found and BAR0's entry decoded
    uint64_t rebarBytes;     // its current BAR0 size in bytes (only with rebarPresent)
    uint64_t vramBytes;      // RCC_CONFIG_MEMSIZE << 20
    uint64_t hiReserve;      // kVramHiTotalReserve (amdgpu_gmc.h)
    uint64_t consoleOff;     // the console's offset inside BAR0 (kNoConsole = not located)
    uint64_t consoleLen;
    uint64_t mapCap;         // kMaxBar0Map in production; a test may pass more
};
struct PlanOut { bool ok; uint32_t reason; uint64_t mapLen; uint64_t vramLimit; };

// Item 2 + 3. mapLen = min(ioLen, mapCap); vramLimit = min(mapLen, vramBytes - hiReserve - 64 MiB) rounded down to 64 KiB.
// At a 256 MiB BAR0 on this card (16304 MiB VRAM) that is mapLen = vramLimit = 256 MiB: exactly 0.0.662.
constexpr PlanOut plan_bar0(const PlanIn &in) {
    PlanOut r { false, kOk, 0ull, 0ull };
    if (in.mapCap == 0ull) { r.reason = kMapCapZero; return r; }
    if (in.ioLen == 0ull) { r.reason = kLenZero; return r; }
    if (!is_pow2(in.ioLen)) { r.reason = kLenNotPow2; return r; }
    if (in.ioLen > in.mapCap) { r.reason = kLenTooBig; return r; }          // 0.0.664 (F3): above the cap (1 GiB) IOPCIFamily's figure cannot be right (IOPCIConfigurator MAX_BAR_SIZE)
    if (in.ioAddr == 0ull) { r.reason = kAddrZero; return r; }
    if ((in.ioAddr & (in.ioLen - 1ull)) != 0ull) { r.reason = kAddrMisaligned; return r; }
    if (in.ioAddr + in.ioLen < in.ioAddr) { r.reason = kAddrWrap; return r; }
    if (in.cfgAddr != in.ioAddr) { r.reason = kAddrMismatch; return r; }
    if (in.rebarPresent) { if (in.rebarBytes != in.ioLen) { r.reason = kRebarMismatch; return r; } }
    else if (in.ioLen > kLegacyBar) { r.reason = kNoRebarCap; return r; }
    const uint64_t mapLen = in.ioLen < in.mapCap ? in.ioLen : in.mapCap;
    if (in.vramBytes <= in.hiReserve + kLimitSlack) { r.reason = kVramTooSmall; return r; }
    const uint64_t cap = in.vramBytes - in.hiReserve - kLimitSlack;
    uint64_t lim = mapLen < cap ? mapLen : cap;
    lim &= ~(kLimitAlign - 1ull);
    if (lim == 0ull) { r.reason = kVramTooSmall; return r; }
    if (in.consoleOff != kNoConsole && in.consoleLen != 0ull && (in.consoleOff > lim || in.consoleLen > lim - in.consoleOff)) { r.reason = kConsoleBeyond; return r; }
    r.ok = true; r.mapLen = mapLen; r.vramLimit = lim;
    return r;
}

// The address of a memory BAR from its config dword(s): a 64-bit BAR (type bits 2:1 == 10b) takes the next dword as the high half.
constexpr uint64_t cfg_bar_addr(uint32_t lo, uint32_t hi) {
    return (((lo & 0x6u) == 0x4u) ? ((uint64_t)hi << 32) : 0ull) | (uint64_t)(lo & ~0xFu);
}

// ---- the Resizable BAR capability (PCIe extended capability id 0x15), READ ONLY ------------------------------------------------------------
// Header dword: id [15:0], version [19:16], next offset [31:20]. Then per BAR an 8-byte entry: capability dword (supported sizes: bit n of [31:4] = 2^(n+20) bytes) and control dword
// (BAR index [2:0], number of resizable BARs [7:5] (first entry only), current size code [12:8] = 2^(code+20) bytes).
constexpr uint32_t kExtCapRebar = 0x15u;
constexpr uint32_t kExtCapStart = 0x100u;
constexpr uint32_t kMaxRebarEntries = 6u;
template <class Rd>
inline bool find_ext_cap(Rd rd, uint32_t id, uint32_t &off) {
    uint32_t o = kExtCapStart;
    for (uint32_t n = 0; n < 96u; n++) {                            // a loop in the list ends the walk
        const uint32_t h = rd(o);
        if (h == 0u || h == 0xFFFFFFFFu) return false;
        if ((h & 0xFFFFu) == id) { off = o; return true; }
        const uint32_t nx = (h >> 20) & 0xFFFu;
        if (nx < kExtCapStart || (nx & 3u) != 0u || nx == o) return false;
        o = nx;
    }
    return false;
}
struct RebarEntry { uint32_t bar; uint32_t capReg; uint32_t ctrlReg; };
struct Census {
    bool     found;
    bool     extReadable;                                           // the first extended dword (0x100) did not read as all-ones
    uint32_t capOff;
    uint32_t n;                                                     // entries decoded (0..kMaxRebarEntries)
    RebarEntry e[kMaxRebarEntries];
};
constexpr uint32_t entry_code(uint32_t ctrl) { return (ctrl >> 8) & 0x1Fu; }
constexpr uint64_t entry_bytes(uint32_t ctrl) { return entry_code(ctrl) < 40u ? (1ull << (20u + entry_code(ctrl))) : 0ull; }
constexpr uint32_t entry_supported_mask(uint32_t cap) { return cap >> 4; }                // bit k = 2^(k+20) bytes
template <class Rd>
inline Census census_read(Rd rd) {
    Census c {};
    const uint32_t x100 = rd(kExtCapStart);
    c.extReadable = x100 != 0xFFFFFFFFu && x100 != rd(0u);          // 0.0.664 (F7): extended space that merely aliases the first 256 bytes (0x100 reads back dword 0) is not readable
    if (!c.extReadable) return c;
    if (!find_ext_cap(rd, kExtCapRebar, c.capOff)) return c;
    c.found = true;
    const uint32_t ctrl0 = rd(c.capOff + 8u);
    uint32_t n = (ctrl0 >> 5) & 7u;
    if (n == 0u) n = 1u;
    if (n > kMaxRebarEntries) n = kMaxRebarEntries;
    for (uint32_t i = 0; i < n; i++) {
        c.e[i].capReg = rd(c.capOff + 4u + 8u * i);
        c.e[i].ctrlReg = rd(c.capOff + 8u + 8u * i);
        c.e[i].bar = c.e[i].ctrlReg & 7u;
    }
    c.n = n;
    return c;
}
// The entry for BAR `bar`, or -1.
constexpr int census_entry(const Census &c, uint32_t bar) {
    for (uint32_t i = 0; i < c.n && i < kMaxRebarEntries; i++) if (c.e[i].bar == bar) return (int)i;
    return -1;
}

constexpr uint64_t entry_min_bytes(uint32_t supportedMask) {          // the smallest size the capability offers (0 = none)
    for (uint32_t k = 0; k < 32u; k++) if ((supportedMask >> k) & 1u) return 1ull << (20u + k);
    return 0ull;
}
// 0.0.664 (F2): a BAR's IOPCIFamily address and its config-space address must be the same (BAR0 had this since 0.0.663; BAR2, the doorbells, now too).
constexpr bool bar_addr_agree(uint64_t ioAddr, uint64_t cfgAddr) { return ioAddr != 0ull && ioAddr == cfgAddr; }

// ---- the console layout (0.0.664, F5) -------------------------------------------------------------------------------------------------------
// With the console located inside BAR0: the bring-up region must start at or above console end + the 64 KiB cursor, and no check point (16 bytes each) nor the visible pool [visBase, vramLimit) may
// overlap the console or its cursor. chooseVramBase can pick vramBase 0 for a console at the top of BAR0; that layout is refused here, before anything is written. Console unknown: nothing to judge.
constexpr uint64_t kCursorBytes = 64ull << 10;
constexpr uint64_t kVisAllocOffset = 0x01800000ull;     // gmc_v12_0.cpp kGMCVRAMAllocOffset (static_asserted there): the visible pool starts this far above vramBase
constexpr bool layout_ok(uint64_t consoleOff, uint64_t consoleLen, uint64_t vramBase, const uint64_t pt[3], uint64_t visBase, uint64_t vramLimit) {
    if (consoleOff == kNoConsole || consoleLen == 0ull) return true;
    const uint64_t cEnd = consoleOff + consoleLen + kCursorBytes;
    if (cEnd < consoleOff) return false;
    if (vramBase < cEnd) return false;
    for (int i = 0; i < 3; i++) if (pt[i] < cEnd && pt[i] + 16ull > consoleOff) return false;
    if (vramLimit > visBase && visBase < cEnd && vramLimit > consoleOff) return false;
    return true;
}

// ---- the BAR latch (item 5) ----------------------------------------------------------------------------------------------------------------
// ioLen 0 = IOPCIFamily published no BAR0. cfgLen 0 = the config-space size is unknown (no readable ReBAR capability). cfgBase 0 = config space has no BAR0 address.
struct LatchOut { bool have; uint64_t base; uint64_t len; bool conflict; };
constexpr LatchOut bar0_latch(uint64_t ioBase, uint64_t ioLen, uint64_t cfgBase, uint64_t cfgLen) {
    LatchOut o { false, 0ull, 0ull, false };
    const bool io = ioLen != 0ull;
    const bool cf = cfgBase != 0ull;
    if (!io && !cf) return o;
    if (io && !cf) { o.have = true; o.base = ioBase; o.len = ioLen; return o; }                       // no config-space address to compare (unassigned): IOPCIFamily's range stands
    const uint64_t cl = cfgLen != 0ull ? cfgLen : ioLen;                                              // an unknown config size is judged by IOPCIFamily's
    if (!io) {                                                                                        // config space has a BAR0 that IOPCIFamily dropped (a BAR it could not size)
        o.conflict = true;
        if (cfgLen != 0ull) { o.have = true; o.base = cfgBase; o.len = cfgLen; }                      // a known size is still guarded
        return o;
    }
    if (ioBase != cfgBase || (cfgLen != 0ull && cfgLen != ioLen)) o.conflict = true;
    const uint64_t b = ioBase < cfgBase ? ioBase : cfgBase;
    const uint64_t ie = ioBase + ioLen, ce = cfgBase + cl;
    const uint64_t e = (ie < ioBase || ce < cfgBase) ? ~0ull : (ie > ce ? ie : ce);                   // a wrapping range is read as reaching the top of the address space (guards more, never less)
    o.have = true; o.base = b; o.len = e - b;
    return o;
}

// ---- the aperture check points (item 4) ----------------------------------------------------------------------------------------------------
// vramBase, the middle (64 KiB aligned) and vramLimit - 64 KiB; each has 16 bytes tested, so each must lie inside the mapped window. false = degenerate (refuse).
constexpr bool aperture_points(uint64_t vramBase, uint64_t vramLimit, uint64_t bar0Size, uint64_t out[3]) {
    if (vramLimit > bar0Size || vramLimit < kApertureTail + 16ull || vramBase + 16ull > vramLimit) return false;
    const uint64_t top = vramLimit - kApertureTail;
    const uint64_t mid = (vramBase + ((vramLimit - vramBase) >> 1)) & ~(kLimitAlign - 1ull);
    if (!(vramBase < mid && mid < top)) return false;
    out[0] = vramBase; out[1] = mid; out[2] = top;
    return true;
}

// ---- the allocator sizes (item 3) ----------------------------------------------------------------------------------------------------------
// gmc_mc_init: the CPU-visible size is vramLimit when set (never the 256 MiB fallback because a BAR exceeds VRAM); a context without vramLimit keeps the 0.0.662 rule.
constexpr uint64_t visible_size(uint64_t vramLimit, uint64_t bar0Size, uint64_t realVram, uint64_t fallback) {
    if (vramLimit > 0ull && (realVram == 0ull || vramLimit <= realVram)) return vramLimit;
    if (bar0Size > 0ull && (realVram == 0ull || bar0Size <= realVram)) return bar0Size;
    return fallback;
}
struct Pools { uint64_t visBase; uint64_t visSize; bool hi; uint64_t hiBase; uint64_t hiSize; };
// gmc_vram_alloc_init: the visible pool is [vramBase + allocOff, vramLimit); the hi pool is [vramLimit, vramBytes - hiReserve) when there is room.
constexpr Pools pool_sizes(uint64_t vramBase, uint64_t allocOff, uint64_t vramLimit, uint64_t vramBytes, uint64_t hiReserve) {
    Pools p { vramBase + allocOff, 0ull, false, 0ull, 0ull };
    if (vramLimit > p.visBase) p.visSize = vramLimit - p.visBase;
    if (vramBytes > vramLimit + hiReserve) { p.hi = true; p.hiBase = vramLimit; p.hiSize = vramBytes - hiReserve - vramLimit; }
    return p;
}

} // namespace n48rebar
