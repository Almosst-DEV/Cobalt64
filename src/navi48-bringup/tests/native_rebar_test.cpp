// native_rebar_test.cpp - build 0.0.663 (Resizable BAR support, an internal design note section 3 items 1-9; plus `vramstat`, Stage 2 review S3).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_rebar_test.cpp -o /tmp/native_rebar && /tmp/native_rebar .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_rebar_plant.sh plants breaks)
// Covers:
//   P1  plan_bar0 (rebar_pure.h): a 256 MiB BAR0 gives EXACTLY the 0.0.662 figures (map 256 MiB, vramLimit 256 MiB, visible pool 217 MiB, hi pool 15280 MiB); 1 GiB gives 985 / 14512 MiB; a 16 GiB BAR is capped at 1 GiB;
//       length 0, a 2 GiB BAR as IOPCIFamily truncates it (length 0) and as a mis-sized 1 GiB against the capability's 2 GiB, address mismatch, misalignment, wrap, a non power of two, address 0,
//       a missing capability above 256 MiB, a VRAM size too small; the TEST-ONLY 16 GiB cap clamps below VRAM minus the reserves (15472 MiB) and keeps a hi pool; the console above vramLimit;
//   P2  the census over a fake PCIe extended config space (header walk, loops, unreadable space, BAR0 / BAR2 entries, the size codes, supported masks) and that it only READS;
//   P3  cfg_bar_addr (a 64-bit BAR joins its high dword), bar0_latch (the union, the disagreement cases, a dropped BAR0), aperture_points, visible_size, pool_sizes;
//   P4  BAR0 at 0x7C00000000: consoleGeometry / inBar0 arithmetic, the aux aperture_ok_len, the doorbell self-ring halves;
//   P5  source pins of the REAL code: item 1 (kMaxBar0Map is the 1 GiB constant), item 3 (dev.vramLimit = bar0Limit, never bar0Size; gmc_mc_init and gmc_vram_alloc_init use the pure rules), item 4 (three
//       points, saved bytes restored, the top check runs BEFORE the allocator exists), item 5 (the latch union and the open refusal), item 6 (read only), item 7 (the log), the version;
//   P6  vramstat: the six sites (pure admission, the kext branch, the CLI), read-only;
//   P8  the identity at 256 MiB: the 0.0.662 arithmetic against the plan over a sweep of VRAM sizes;
//   P7  Mesa (no change): with the kernel's 985 / 14512 MiB figures RADV keeps a separate VRAM heap and the largest allocation is 738 MiB (the same formulas, pinned against the Mesa tree when present).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <sstream>
#include "amd/rebar_pure.h"
#include "amd/native_fb_pure.h"
#include "amd/native_disp_pure.h"

using namespace n48rebar;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }
static size_t at(const std::string &s, const std::string &needle) { return s.find(needle); }
static bool has(const std::string &s, const std::string &needle) { return s.find(needle) != std::string::npos; }
// The text of one function / region: from the first occurrence of `from` to the next occurrence of `to` after it.
static std::string between(const std::string &s, const std::string &from, const std::string &to) {
    const size_t a = s.find(from); if (a == std::string::npos) return "";
    const size_t b = s.find(to, a + from.size()); return b == std::string::npos ? s.substr(a) : s.substr(a, b - a);
}

static constexpr uint64_t MiB = 1ull << 20, GiB = 1ull << 30;
static constexpr uint64_t kVram = 16304 * MiB;                    // RCC_CONFIG_MEMSIZE on this card
static constexpr uint64_t kReserve = (512 + 256) * MiB;          // kVramHiTotalReserve (amdgpu_gmc.h)
static constexpr uint64_t kBase = 15 * MiB, kAllocOff = 0x1800000ull;   // vramBase at 1440p and kGMCVRAMAllocOffset

static PlanIn mk(uint64_t len, uint64_t addr) {
    PlanIn p {}; p.ioLen = len; p.ioAddr = addr; p.cfgAddr = addr; p.rebarPresent = true; p.rebarBytes = len;
    p.vramBytes = kVram; p.hiReserve = kReserve; p.consoleOff = kNoConsole; p.consoleLen = 0; p.mapCap = kMaxBar0Map;
    return p;
}

static void p1_plan() {
    expect_u("kMaxBar0Map is 1 GiB", kMaxBar0Map, GiB);
    {   // 256 MiB: today's figures
        const PlanOut o = plan_bar0(mk(256 * MiB, 0xD0000000ull));
        expect(o.ok && o.reason == kOk, "P1: 256 MiB is planned");
        expect_u("P1: 256 MiB maps 256 MiB", o.mapLen, 256 * MiB); expect_u("P1: 256 MiB vramLimit == 256 MiB (the 0.0.662 line dev.vramLimit = bar0Size)", o.vramLimit, 256 * MiB);
        const Pools pl = pool_sizes(kBase, kAllocOff, o.vramLimit, kVram, kReserve);
        expect_u("P1: 256 MiB visible pool is 217 MiB", pl.visSize, 217 * MiB); expect(pl.hi, "P1: 256 MiB has a hi pool"); expect_u("P1: 256 MiB hi pool is 15280 MiB", pl.hiSize, 15280 * MiB);
        expect_u("P1: 256 MiB hi pool starts at vramLimit", pl.hiBase, 256 * MiB);
        expect_u("P1: 256 MiB visible_size == bar0Size", visible_size(o.vramLimit, o.mapLen, kVram, 256 * MiB), 256 * MiB);
    }
    {   // 1 GiB high
        const PlanOut o = plan_bar0(mk(GiB, 0x7C00000000ull));
        expect(o.ok, "P1: 1 GiB at 0x7C00000000 is planned");
        expect_u("P1: 1 GiB maps 1 GiB", o.mapLen, GiB); expect_u("P1: 1 GiB vramLimit 1 GiB", o.vramLimit, GiB);
        const Pools pl = pool_sizes(kBase, kAllocOff, o.vramLimit, kVram, kReserve);
        expect_u("P1: 1 GiB visible pool is 985 MiB", pl.visSize, 985 * MiB); expect_u("P1: 1 GiB hi pool is 14512 MiB", pl.hiSize, 14512 * MiB);
        expect_u("P1: the hi pool ends exactly where the reserve begins (hiBase + hiSize + reserve == VRAM)", pl.hiBase + pl.hiSize + kReserve, kVram);
    }
    {   // 16 GiB: capped at 1 GiB
        const PlanOut o = plan_bar0(mk(16 * GiB, 0x400000000ull));
        expect(!o.ok && o.reason == kLenTooBig, "P1 (0.0.664 F3): a 16 GiB BAR is REFUSED (above the 1 GiB cap IOPCIFamily mis-sized it), no longer capped");
        for (uint64_t big : { 2 * GiB, 4 * GiB, 32 * GiB }) expect_u("P1 (F3): every length above 1 GiB refuses", plan_bar0(mk(big, big)).reason, kLenTooBig);
        expect(plan_bar0(mk(GiB, GiB)).ok && plan_bar0(mk(GiB, 0x80000000ull)).ok, "P1 (F3): exactly 1 GiB still plans");
    }
    {   // refusals
        PlanIn p = mk(256 * MiB, 0xD0000000ull); p.ioLen = 0; p.rebarBytes = 0;
        expect_u("P1: length 0 refuses", plan_bar0(p).reason, kLenZero);
        p = mk(2 * GiB, 0xC0000000ull); p.ioLen = 0;                                          // 2 GiB as IOPCIFamily truncates it: size 0
        expect(!plan_bar0(p).ok && plan_bar0(p).reason == kLenZero, "P1: a 2 GiB BAR truncated to length 0 by IOPCIFamily refuses");
        p = mk(2 * GiB, 0xC0000000ull); p.ioLen = GiB;                                        // mis-sized to 1 GiB: the capability says 2 GiB
        expect_u("P1: IOPCIFamily 1 GiB vs capability 2 GiB refuses (kRebarMismatch)", plan_bar0(p).reason, kRebarMismatch);
        p = mk(GiB, 0x80000000ull); p.cfgAddr = 0x40000000ull;
        expect_u("P1: address mismatch refuses", plan_bar0(p).reason, kAddrMismatch);
        p = mk(GiB, 0x90000000ull); p.cfgAddr = p.ioAddr;
        expect_u("P1: an address not aligned to the length refuses", plan_bar0(p).reason, kAddrMisaligned);
        p = mk(3 * 256 * MiB, 0xC0000000ull); p.rebarBytes = p.ioLen;
        expect_u("P1: a non power of two refuses", plan_bar0(p).reason, kLenNotPow2);
        p = mk(256 * MiB, 0); p.cfgAddr = 0;
        expect_u("P1: address 0 refuses", plan_bar0(p).reason, kAddrZero);
        p = mk(1ull << 63, 1ull << 63); p.mapCap = 1ull << 63;
        expect_u("P1: a 2^63 BAR at 2^63 wraps and refuses", plan_bar0(p).reason, kAddrWrap);
        p = mk(GiB, 0xFFFFFFFFC0000000ull);
        expect_u("P1: address + length wrapping refuses", plan_bar0(p).reason, kAddrWrap);
        p = mk(GiB, 0x80000000ull); p.rebarPresent = false;
        expect_u("P1: 1 GiB with no readable capability refuses (nothing to cross-check)", plan_bar0(p).reason, kNoRebarCap);
        p = mk(256 * MiB, 0xD0000000ull); p.rebarPresent = false;
        expect(plan_bar0(p).ok, "P1: 256 MiB with no readable capability is planned as in 0.0.662");
        p = mk(256 * MiB, 0xD0000000ull); p.rebarBytes = 512 * MiB;
        expect_u("P1: capability 512 MiB vs IOPCIFamily 256 MiB refuses even at 256 MiB", plan_bar0(p).reason, kRebarMismatch);
        p = mk(256 * MiB, 0xD0000000ull); p.vramBytes = kReserve + 64 * MiB;
        expect_u("P1: VRAM that leaves nothing after the reserves refuses", plan_bar0(p).reason, kVramTooSmall);
        p = mk(256 * MiB, 0xD0000000ull); p.mapCap = 0;
        expect_u("P1: a zero map cap refuses", plan_bar0(p).reason, kMapCapZero);
        p = mk(256 * MiB, 0xD0000000ull); p.consoleOff = 255 * MiB; p.consoleLen = 14 * MiB;
        expect_u("P1: a console running past vramLimit refuses", plan_bar0(p).reason, kConsoleBeyond);
        p = mk(256 * MiB, 0xD0000000ull); p.consoleOff = 0; p.consoleLen = 15 * MiB;
        expect(plan_bar0(p).ok, "P1: the usual console at the bottom is planned");
    }
    {   // the TEST-ONLY 16 GiB cap must clamp below VRAM minus the reserves
        PlanIn p = mk(16 * GiB, 0x400000000ull); p.mapCap = 16 * GiB;
        const PlanOut o = plan_bar0(p);
        expect(o.ok, "P1: 16 GiB with a 16 GiB test cap is planned"); expect_u("P1: the clamp is VRAM - reserve - 64 MiB = 15472 MiB", o.vramLimit, 15472 * MiB);
        expect_u("P1: the mapped length is still the whole 16 GiB (bar0Size = mapped bytes)", o.mapLen, 16 * GiB);
        const Pools pl = pool_sizes(kBase, kAllocOff, o.vramLimit, kVram, kReserve);
        expect(pl.hi && pl.hiSize == 64 * MiB, "P1: the hi pool exists (64 MiB) because the clamp leaves it");
        expect(o.vramLimit + kReserve <= kVram, "P1: vramLimit + reserve stays inside VRAM (the visible pool never reaches the PSP TMR / discovery table / Apple's arena)");
        expect_u("P1: gmc visible size is vramLimit, not the 256 MiB fallback", visible_size(o.vramLimit, o.mapLen, kVram, 256 * MiB), 15472 * MiB);
        expect((o.vramLimit & (kLimitAlign - 1)) == 0, "P1: vramLimit is 64 KiB aligned");
        PlanIn q = p; q.vramBytes = kVram + 12345;                                            // an odd VRAM size is rounded DOWN
        const PlanOut oq = plan_bar0(q);
        expect((oq.vramLimit & (kLimitAlign - 1)) == 0 && oq.vramLimit <= q.vramBytes - kReserve - kLimitSlack, "P1: vramLimit rounds down to 64 KiB for an odd VRAM size");
    }
    expect_u("F3: the reason is named", plan_bar0(mk(16 * GiB, 0x400000000ull)).reason, kLenTooBig);
    expect_u("visible_size keeps the old rule with no vramLimit (bar0Size)", visible_size(0, 256 * MiB, kVram, 123), 256 * MiB);
    expect_u("visible_size falls back with neither", visible_size(0, 0, kVram, 123), 123);
    expect_u("visible_size: a BAR bigger than VRAM with no vramLimit gives the fallback (the 0.0.662 behaviour the plan replaces)", visible_size(0, 32 * GiB, kVram, 123), 123);
}

// ---- P2: a fake extended config space ----------------------------------------------------------------------------------------------------------------
struct FakeCfg {
    std::map<uint32_t, uint32_t> d; mutable uint32_t reads = 0;
    uint32_t operator()(uint32_t off) const { const_cast<FakeCfg *>(this)->reads++; auto it = d.find(off); return it == d.end() ? 0u : it->second; }
};
static uint32_t ext_hdr(uint32_t id, uint32_t next) { return id | (1u << 16) | (next << 20); }
static uint32_t sizes_mask(std::initializer_list<int> codes) { uint32_t m = 0; for (int c : codes) m |= 1u << (4 + c); return m; }   // capability dword: bit (4 + code)
static void p2_census() {
    {   // AER at 0x100 -> ReBAR at 0x200 (BAR0: codes 8..14 supported, current 8 = 256 MiB; BAR2: 1 MiB..8 MiB, current 3 = 8 MiB)
        FakeCfg c; c.d[0x100] = ext_hdr(0x1, 0x200); c.d[0x200] = ext_hdr(kExtCapRebar, 0);
        c.d[0x204] = sizes_mask({8, 9, 10, 11, 12, 13, 14}); c.d[0x208] = (2u << 5) | (8u << 8) | 0u;
        c.d[0x20C] = sizes_mask({0, 1, 2, 3}); c.d[0x210] = (3u << 8) | 2u;
        const Census k = census_read(c);
        expect(k.found && k.extReadable && k.capOff == 0x200 && k.n == 2, "P2: the capability is found behind another one, two entries");
        const int e0 = census_entry(k, 0), e2 = census_entry(k, 2);
        expect(e0 == 0 && e2 == 1 && census_entry(k, 5) == -1, "P2: BAR0 and BAR2 entries are found by BAR index");
        expect_u("P2: BAR0 current size 256 MiB", entry_bytes(k.e[e0].ctrlReg), 256 * MiB);
        expect_u("P2: BAR2 current size 8 MiB", entry_bytes(k.e[e2].ctrlReg), 8 * MiB);
        expect_u("P2: BAR0 supported mask bit 10 = 1 GiB", (entry_supported_mask(k.e[e0].capReg) >> 10) & 1u, 1u);
        expect_u("P2: BAR0 mask does not offer 1 MiB", entry_supported_mask(k.e[e0].capReg) & 1u, 0u);
        expect_u("P2: BAR0 code 14 = 16 GiB", entry_bytes(14u << 8), 16 * GiB);
        expect_u("P2: BAR0 code 10 = 1 GiB", entry_bytes(10u << 8), GiB);
    }
    {   // 0.0.664 (F7): extended space that aliases the first 256 bytes (0x100 reads dword 0) is not readable, even when the aliased data looks like a capability header
        FakeCfg c; c.d[0] = ext_hdr(kExtCapRebar, 0); c.d[0x100] = c.d[0];
        const Census k = census_read(c); expect(!k.extReadable && !k.found, "P2 (F7): an aliased extended space is unreadable and nothing is decoded from it");
        FakeCfg d; d.d[0] = 0x73101002u; d.d[0x100] = 0x73101002u;
        expect(!census_read(d).extReadable, "P2 (F7): 0x100 == the vendor/device dword is aliasing");
        FakeCfg e; e.d[0] = 0x73101002u; e.d[0x100] = ext_hdr(0x1, 0);
        expect(census_read(e).extReadable, "P2 (F7): a real, different first extended dword is readable");
        expect_u("entry_min_bytes: lowest set bit", entry_min_bytes(sizes_mask({8, 9, 10}) >> 4), 256 * MiB);
        expect_u("entry_min_bytes: 1 MiB offered", entry_min_bytes(1u), MiB); expect_u("entry_min_bytes: none", entry_min_bytes(0u), 0);
    }
    {   // not present
        FakeCfg c; c.d[0x100] = ext_hdr(0x1, 0x140); c.d[0x140] = ext_hdr(0x19, 0);
        const Census k = census_read(c); expect(!k.found && k.extReadable, "P2: no capability: not found, extended space readable");
    }
    {   // unreadable (all ones)
        FakeCfg c; c.d[0x100] = 0xFFFFFFFFu;
        const Census k = census_read(c); expect(!k.found && !k.extReadable, "P2: all ones: not found and flagged unreadable");
    }
    {   // a loop and a bad pointer end the walk
        FakeCfg c; c.d[0x100] = ext_hdr(0x1, 0x140); c.d[0x140] = ext_hdr(0x2, 0x100);
        { auto rc = [&](uint32_t o) { return c(o); }; expect(!census_read(rc).found, "P2: a loop ends the walk"); expect(c.reads <= 100, "P2: the loop walk is bounded (96 headers at most)"); }
        FakeCfg d; d.d[0x100] = ext_hdr(0x1, 0x42);
        expect(!census_read(d).found, "P2: a next pointer below 0x100 / misaligned ends the walk");
        FakeCfg e; e.d[0x100] = ext_hdr(0x1, 0x100);
        { auto re = [&](uint32_t o) { return e(o); }; expect(!census_read(re).found && e.reads <= 3, "P2: a self pointer ends the walk at once (no 96-step spin)"); }
    }
    {   // entries clamp
        FakeCfg c; c.d[0x100] = ext_hdr(kExtCapRebar, 0); c.d[0x108] = (7u << 5) | (8u << 8);
        const Census k = census_read(c); expect(k.found && k.n == kMaxRebarEntries, "P2: the entry count is clamped to 6");
        FakeCfg d; d.d[0x100] = ext_hdr(kExtCapRebar, 0); d.d[0x108] = 8u << 8;
        expect(census_read(d).n == 1, "P2: a count of 0 is read as 1");
    }
    expect_u("P2: a size code of 31 is 2^51 bytes (the 5-bit field never wraps)", entry_bytes(31u << 8), 1ull << 51);
}

static void p3_pure() {
    expect_u("cfg_bar_addr: 64-bit BAR joins the high dword", cfg_bar_addr(0xC000000Cu, 0x7Cu), 0x7CC0000000ull);
    expect_u("cfg_bar_addr: 32-bit BAR ignores the next dword", cfg_bar_addr(0xD0000008u, 0x7Cu), 0xD0000000ull);
    expect_u("cfg_bar_addr: a 64-bit BAR below 4 GiB", cfg_bar_addr(0xD000000Cu, 0u), 0xD0000000ull);
    expect_u("cfg_bar_addr: flag bits are masked", cfg_bar_addr(0xD000000Fu, 0u), 0xD0000000ull);
    expect_u("cfg_bar_addr: the high half matters for 0x7C00000000", cfg_bar_addr(0x0000000Cu, 0x7Cu), 0x7C00000000ull);
    {   // the latch union
        LatchOut o = bar0_latch(0xD0000000ull, 256 * MiB, 0xD0000000ull, 256 * MiB);
        expect(o.have && !o.conflict && o.base == 0xD0000000ull && o.len == 256 * MiB, "P3: agreement latches the one range");
        o = bar0_latch(0xD0000000ull, 256 * MiB, 0xD0000000ull, 0);
        expect(o.have && !o.conflict && o.len == 256 * MiB, "P3: an unknown config size is judged by IOPCIFamily's");
        o = bar0_latch(0xD0000000ull, 256 * MiB, 0xC0000000ull, 256 * MiB);
        expect(o.have && o.conflict && o.base == 0xC0000000ull && o.len == 0x20000000ull, "P3: different addresses: conflict and the union covers both");
        o = bar0_latch(0xC0000000ull, GiB, 0xC0000000ull, 2 * GiB);
        expect(o.have && o.conflict && o.base == 0xC0000000ull && o.len == 2 * GiB, "P3: IOPCIFamily 1 GiB vs config space 2 GiB: conflict, the union is 2 GiB");
        o = bar0_latch(0, 0, 0x7C00000000ull, 16 * GiB);
        expect(o.have && o.conflict && o.base == 0x7C00000000ull && o.len == 16 * GiB, "P3: a BAR0 IOPCIFamily dropped (a 16 GiB one) is still latched, and it is a conflict");
        o = bar0_latch(0, 0, 0x7C00000000ull, 0);
        expect(!o.have && o.conflict, "P3: a dropped BAR0 of unknown size is a conflict with nothing to latch");
        o = bar0_latch(0, 0, 0, 0); expect(!o.have && !o.conflict, "P3: no BAR0 at all: nothing");
        o = bar0_latch(0xD0000000ull, 256 * MiB, 0, 0); expect(o.have && !o.conflict && o.len == 256 * MiB, "P3: no config-space address: IOPCIFamily's range stands");
        o = bar0_latch(0xFFFFFFFFF0000000ull, 0x10000000ull, 0xFFFFFFFFE0000000ull, 0x10000000ull);
        expect(o.have && o.conflict && o.base + o.len >= o.base, "P3: wrapping ranges never wrap the union");
    }
    {   // aperture points
        uint64_t pt[3] = {};
        expect(aperture_points(kBase, 256 * MiB, 256 * MiB, pt), "P3: aperture points at 256 MiB");
        expect_u("P3: bottom", pt[0], kBase); expect_u("P3: top is vramLimit - 64 KiB", pt[2], 256 * MiB - 64 * 1024);
        expect(pt[1] > pt[0] && pt[1] < pt[2] && (pt[1] & 0xFFFF) == 0, "P3: the middle is between, 64 KiB aligned");
        expect_u("P3: middle at 256 MiB", pt[1], (kBase + (256 * MiB - kBase) / 2) & ~0xFFFFull);
        expect(aperture_points(kBase, GiB, GiB, pt) && pt[2] == GiB - 64 * 1024 && pt[1] > 256 * MiB, "P3: at 1 GiB the middle is above the old 256 MiB window");
        expect(!aperture_points(kBase, GiB, 256 * MiB, pt), "P3: a limit beyond the mapped window is refused");
        expect(!aperture_points(GiB - 32, GiB, GiB, pt), "P3: a base too close to the top is refused");
        expect(!aperture_points(0, 0, 0, pt), "P3: zero limit is refused");
    }
}

static void p3b_layout() {
    const uint64_t cons = 14745600ull;
    uint64_t pt[3] = {}; const uint64_t lim = GiB;
    aperture_points(kBase, lim, lim, pt);
    expect(layout_ok(0, cons, kBase, pt, kBase + kVisAllocOffset, lim), "P3b (F5): the normal layout (console at the bottom, base 15 MiB) is fine");
    expect(layout_ok(kNoConsole, 0, 0, pt, kVisAllocOffset, lim), "P3b: a console not located is not judged");
    // a console at the top of BAR0 with chooseVramBase's fallback vramBase 0
    uint64_t pz[3] = {}; aperture_points(0, lim, lim, pz);
    expect(!layout_ok(lim - cons, cons, 0, pz, kVisAllocOffset, lim), "P3b (F5): a console at the top of BAR0 with vramBase 0 is REFUSED");
    expect(!layout_ok(0, cons, cons, pt, cons + kVisAllocOffset, lim), "P3b (F5): vramBase exactly at the console end (inside the 64 KiB cursor) is refused");
    expect(layout_ok(0, cons, cons + kCursorBytes, pt, cons + kCursorBytes + kVisAllocOffset, lim), "P3b (F5): vramBase at console end + cursor is fine");
    uint64_t pm[3] = { kBase, 512 * MiB, lim - 64 * 1024 };
    expect(!layout_ok(512 * MiB, 16 * MiB, kBase, pm, kBase + kVisAllocOffset, lim), "P3b (F5): a check point inside a console in the middle is refused");
    uint64_t pc[3] = { kBase, 700 * MiB, lim - 64 * 1024 };
    expect(!layout_ok(600 * MiB, 16 * MiB, kBase, pc, kBase + kVisAllocOffset, lim), "P3b (F5): the visible pool running over a console in the middle is refused (vramBase below it)");
    uint64_t pi[3] = { 605 * MiB, 800 * MiB, lim - 64 * 1024 };      // vramBase is fine (700 MiB) but a check point lies inside the console [600, 616) MiB
    expect(!layout_ok(600 * MiB, 16 * MiB, 700 * MiB, pi, 700 * MiB + kVisAllocOffset, lim), "P3b (F5): a check point inside the console is refused on its own");
    uint64_t pj[3] = { 700 * MiB, 800 * MiB, lim - 64 * 1024 };
    expect(!layout_ok(600 * MiB, 16 * MiB, 700 * MiB, pj, 605 * MiB, lim), "P3b (F5): a visible pool that starts inside the console is refused on its own");
    expect(layout_ok(600 * MiB, 16 * MiB, 700 * MiB, pj, 700 * MiB + kVisAllocOffset, lim), "P3b (F5): the same layout with consistent values is fine");
    expect(!layout_ok(lim - 10, 100, kBase, pt, kBase + kVisAllocOffset, lim), "P3b: a console running past the limit is refused");
    expect(!layout_ok(~0ull - 100, 1000, kBase, pt, kBase + kVisAllocOffset, lim), "P3b: a wrapping console end is refused");
    expect(bar_addr_agree(0x7C40000000ull, 0x7C40000000ull) && !bar_addr_agree(0x7C40000000ull, 0x7C00000000ull) && !bar_addr_agree(0, 0), "P3b (F2): BAR2 addresses must be equal and non-zero");
}

static void p4_high() {
    const uint64_t bar0Phys = 0x7C00000000ull, bar0Size = GiB;
    // the exact expression of Navi48Bringup::captureBootFramebuffer / consoleGeometry
    auto inBar0 = [&](uint64_t fbPhys, uint64_t fbLen) { return fbPhys >= bar0Phys && fbPhys + fbLen <= bar0Phys + bar0Size; };
    expect(inBar0(bar0Phys, 14745600) && inBar0(bar0Phys + 0x100000, 14745600), "P4: a console at the bottom of BAR0 at 0x7C00000000 is inside");
    expect(!inBar0(0xD0000000ull, 14745600) && !inBar0(bar0Phys + bar0Size - 10, 100), "P4: below / straddling the top is not inside");
    expect(inBar0(bar0Phys + bar0Size - 14745600, 14745600), "P4: ending exactly at the top is inside");
    expect(n48fb::aperture_ok_len(bar0Phys, bar0Size, 0x2000000ull, 14745600), "P4: the aux aperture check passes with a high BAR0 base");
    expect(!n48fb::aperture_ok_len(bar0Phys, bar0Size, bar0Size - 100, 14745600), "P4: the aux aperture check refuses a frame past the top");
    expect(!n48fb::aperture_ok_len(0x7C00001000ull, bar0Size, 0, 14745600), "P4: the aux aperture check refuses a base that is not 64 KiB aligned");
    // the doorbell self-ring: the halves written for a BAR2 above 4 GiB
    const uint64_t bar2 = 0x7C40000000ull;
    expect_u("P4: self-ring low half", (uint32_t)(bar2 & 0xFFFFFFFFull), 0x40000000u); expect_u("P4: self-ring high half", (uint32_t)(bar2 >> 32), 0x7Cu);
}

static void p5_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/src/";
    const std::string b = slurp(K + "Navi48Bringup.cpp"), hpp = slurp(K + "Navi48Bringup.hpp"), gmc = slurp(K + "amd/gmc_v12_0.cpp"), s1c = slurp(K + "amd/native_s1c.cpp"), s1ch = slurp(K + "amd/native_s1c.h");
    const std::string nbif = slurp(K + "amd/nbif_v6_3_1.cpp"), regs = slurp(K + "amd/amdgpu_regs.h");
    expect(!b.empty() && !gmc.empty() && !s1c.empty() && !nbif.empty(), "P5: sources read");
    // item 1
    expect(has(b, "constexpr uint64_t kMaxBar0Map    = n48rebar::kMaxBar0Map;") && !has(b, "kMaxBar0Map    = 256ULL << 20"), "P5 item 1: kMaxBar0Map is the 1 GiB constant (not 256 MiB)");
    // item 2/3: mapVramAperture plans before it maps
    const std::string mv = between(b, "bool Navi48Bringup::mapVramAperture() {", "// The plan again, now that the console is located");
    expect(!mv.empty() && has(mv, "n48rebar::plan_bar0(pin)") && has(mv, "readRebarCensus(pciDevice)") && has(mv, "pin.cfgAddr = cfgAddr"), "P5 item 2: mapVramAperture plans from IOPCIFamily, config space and the census");
    expect(at(mv, "n48rebar::plan_bar0(pin)") < at(mv, "IODeviceMemory::withSubRange(bar, 0, winLen)") && has(mv, "const uint64_t winLen  = pl.mapLen;"), "P5 item 2: the plan comes BEFORE the mapping and the mapped length is the plan's");
    expect(has(mv, "if (!pl.ok) {") && at(mv, "if (!pl.ok) {") < at(mv, "IODeviceMemory::withSubRange"), "P5 item 2: a refused plan maps nothing");
    expect(has(mv, "pin.mapCap = kMaxBar0Map;") && has(mv, "bar0Size = bar0Map->getLength();") && has(mv, "if (bar0Size != winLen) {"), "P5 item 3: bar0Size is the bytes actually mapped, and a different length refuses");
    expect(has(mv, "bar0Phys = cfgAddr;") && !has(mv, "configRead32(kIOPCIConfigBaseAddress0)"), "P5 item 2: bar0Phys is the one config-space address the plan cross-checked");
    expect(has(mv, "pin.hiReserve = amdgpu::kVramHiTotalReserve"), "P5 item 3: the plan uses the allocator's own reserve");
    const std::string fp = between(b, "bool Navi48Bringup::finishVramPlan() {", "bool Navi48Bringup::mapDoorbells()");
    expect(has(fp, "pin.consoleOff = bootFbOff;") && has(fp, "bar0Limit = pl.vramLimit;") && has(fp, "pl.mapLen != bar0Size"), "P5 item 3: finishVramPlan adds the console and fixes bar0Limit");
    const std::string bd = between(b, "bool Navi48Bringup::buildDeviceContext() {", "static amdgpu::BringupContext gBringup;");
    expect(has(bd, "dev.vramLimit = bar0Limit;") && !has(bd, "dev.vramLimit = bar0Size;"), "P5 item 3: dev.vramLimit is the planned limit, never the old bar0Size line");
    expect(has(bd, "dev.bar0Size = bar0Size;"), "P5 item 3: dev.bar0Size stays the mapped bytes");
    expect(has(gmc, "gmc.visible_vram_size = n48rebar::visible_size(dev.vramLimit, dev.bar0Size, gmc.real_vram_size, kFallbackVisibleVRAM);"), "P5 item 3: gmc_mc_init's visible size comes from the pure rule (vramLimit first)");
    expect(has(gmc, "n48rebar::pool_sizes(dev.vramBase, kGMCVRAMAllocOffset, dev.vramLimit, dev.vramSizeBytes, kVramHiTotalReserve)") && has(gmc, "const uint64_t alloc_size = pools.visSize;") && has(gmc, "if (pools.hi) {") && has(gmc, "gmc.vram_hi_size = pools.hiSize;"), "P5 item 3: both pools come from the pure rule");
    expect(has(regs, "n48rebar::plan_bar0's vramLimit"), "P5 item 3: the DeviceContext comment names the new rule");
    // item 4: three points, restore, ordering
    const std::string ac = between(b, "bool Navi48Bringup::apertureCheck(uint64_t off) {", "// 0.0.663 (ReBAR item 4): three points");
    expect(!ac.empty() && has(ac, "vramWrite(off, before, sizeof(before))") && !has(ac, "vramMemset(off, 0,"), "P5 item 4: the check RESTORES the saved bytes (it does not zero them)");
    expect(has(ac, "if (back != before[i]) {") && has(ac, "RESTORE FAILED") && has(ac, "vramRead32(off + 4 * i)"), "P5 item 4: the restore is read back and a failure refuses");
    const std::string aw = between(b, "bool Navi48Bringup::apertureCheckWindow() {", "// ---------------------------------------------------------------------------\n// Read-only survey stages");
    expect(has(aw, "n48rebar::aperture_points(vramBase, bar0Limit, bar0Size, pt)") && has(aw, "for (int i = 0; i < 3; i++) all = apertureCheck(pt[i]) && all;") && has(aw, "return all;"), "P5 item 4: three points, every one tried, any mismatch refuses");
    const std::string sp = between(b, "void Navi48Bringup::stagePSP() {", "// Compat layer for the ported mac-amdgpu modules");
    expect(!sp.empty() && at(sp, "mapVramAperture()") < at(sp, "captureBootFramebuffer()") && at(sp, "captureBootFramebuffer()") < at(sp, "chooseVramBase()") && at(sp, "chooseVramBase()") < at(sp, "finishVramPlan()") &&
           at(sp, "finishVramPlan()") < at(sp, "apertureCheckWindow()") && at(sp, "apertureCheckWindow()") < at(sp, "buildDeviceContext()"), "P5 item 4: map -> console -> base -> plan -> THREE-POINT CHECK -> buildDeviceContext (the allocator is initialised only after it)");
    expect(has(sp, "if (!apertureCheckWindow())") && has(sp, "\"aperture-mismatch\""), "P5 item 4: a failed check stops the ladder");
    expect(!has(sp, "apertureCheck(vramBase)"), "P5 item 4: the stage no longer runs the single-point check alone");
    expect(count_of(b, "gmc_vram_alloc_init(") == 0 && at(sp, "buildDeviceContext()") < at(sp, "runStages(targetStage)"), "P5 item 4: the allocator is initialised only by the ladder (runStages), which stagePSP starts after buildDeviceContext");
    // item 5: the latch
    const std::string lt = between(b, "static void latchPciBars(IOPCIDevice *pci) {", "bool Navi48Bringup::mapRegisters()");
    expect(has(lt, "n48rebar::bar0_latch(io0Base, io0Len, cfgBase, cfgLen)") && has(lt, "amdgpu::n1c_latch_bar0_conflict(u.conflict);") && has(lt, "readCfgBar0(pci)") && has(lt, "n48rebar::census_entry(cen, 0u)"), "P5 item 5: the latch takes the union with config space's BAR0 and reports the conflict");
    expect(at(lt, "amdgpu::n1c_latch_bar0_conflict(u.conflict);") < at(lt, "amdgpu::n1c_latch_pci_bars(base, size, n);"), "P5 item 5: the conflict is latched before the BARs");
    expect(has(b, "return n48rebar::cfg_bar_addr(lo, hi);") && has(b, "const uint32_t hi = ((lo & 0x6) == 0x4) ? pci->configRead32(kIOPCIConfigBaseAddress1) : 0;"), "P5 item 5: the config-space BAR0 address joins both dwords");
    expect(has(s1ch, "void n1c_latch_bar0_conflict(bool conflict);") && has(s1c, "void n1c_latch_bar0_conflict(bool conflict) {"), "P5 item 5: the latch is declared and defined");
    const std::string op = between(s1c, "IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {", "// (4) exclusivity.");
    expect(has(op, "__atomic_load_n(&gBar0Conflict, __ATOMIC_ACQUIRE) == 2u") && has(op, "return kIOReturnNotReady;") && at(op, "gBar0Conflict") < at(op, "(2) the S1b gate"), "P5 item 5: n1c_open refuses on a BAR0 disagreement, before the S1b gate");
    expect(at(s1c, "static volatile UInt32 gBar0Conflict;") != std::string::npos && has(s1c, "OSCompareAndSwap(0, conflict ? 2u : 1u, &gBar0Conflict)"), "P5 item 5: the conflict word is first-writer-wins");
    // item 6: read-only census
    const std::string cen = between(b, "static n48rebar::Census readRebarCensus(IOPCIDevice *pci) {", "static void latchPciBars(IOPCIDevice *pci) {");
    expect(has(cen, "configRead32") && !has(cen, "configWrite") && !has(mv, "configWrite") && !has(lt, "configWrite") && !has(fp, "configWrite"), "P5 item 6: the census / plan / latch code never writes config space");
    expect(has(mv, "setProperty(\"ReBAR,Found\", cen.found);") && has(mv, "ReBAR,%s,SupportedMask") && has(mv, "ReBAR,%s,CurrentMB") && has(mv, "ReBAR,CapOffset") && has(mv, "n48rebar::census_entry(cen, 2u)"), "P5 item 6: the census is published for BAR0 and BAR2 as registry properties");
    expect(has(hpp, "n48rebar::PlanIn   bar0Plan") && has(hpp, "uint64_t           bar0Limit"), "P5: the header carries the plan state");
    // item 7: the doorbell log
    expect(has(nbif, "writing BASE_LOW %#010x") && has(nbif, "writing BASE_HIGH %#010x") && at(nbif, "writing BASE_HIGH") < at(nbif, "kRegSelfringBaseHigh, aHigh, beforeHigh, high"), "P5 item 7: the self-ring low and high halves are logged before they are written");
    // version
    const std::string plist = slurp(root + "/src/navi48-bringup/Info.plist");
    expect(count_of(plist, "<string>0.0.664</string>") == 2, "P5: Info.plist is 0.0.664, carried twice");
    expect(has(s1ch, "constexpr uint32_t kN1cKextBuild = 664;"), "P5: the native client reports build 664");
}

static void p6_vramstat(const std::string &root) {
    using namespace n48disp;
    expect_u("P6: vramstat is action 108", kActVramStat, 108u);
    expect(action_admitted(true, 108u) && !action_admitted(false, 108u), "P6: admitted with the display latch, refused without");
    expect(is_new_action(108u) && !is_pipe_verb(108u), "P6: a new action, not a pipe verb (n48disp_verb never answers it)");
    expect(verb_args_ok(108u, 0u) && !verb_args_ok(108u, 1u) && !verb_args_ok(108u, ~0ull), "P6: takes no argument");
    expect(native_exempt(true, 108u, 0u) && !native_exempt(false, 108u, 0u) && !native_exempt(true, 108u, 5u), "P6: exempt from the native-boot refusal exactly with the latch and no argument");
    expect(!action_admitted(true, 109u) && !is_new_action(109u), "P6: 109 stays unadmitted");
    const std::string b = slurp(root + "/src/navi48-bringup/src/Navi48Bringup.cpp");
    const std::string vs = between(b, "	if (action == n48disp::kActVramStat) {", "	if (action == 83 ||");
    expect(!vs.empty() && has(vs, "n48disp::verb_args_ok(action, argScalar)") && has(vs, "gBringup.gmc.vram_alloc.is_inited()") && has(vs, "bytes_free()") && has(vs, "return st == n48disp::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;"), "P6: the kext branch reads the allocator and answers through outExtra");
    expect(!has(vs, "regWrite") && !has(vs, "vramWrite") && !has(vs, "vramMemset") && !has(vs, "configWrite") && !has(vs, ".alloc(") && !has(vs, ".free(") && !has(vs, "init("), "P6: read-only (no register, VRAM, config or allocator write)");
    expect(has(vs, "v[5] = bar0Limit; v[6] = bar0Size; v[7] = bar0Phys;") && has(vs, "v[1] = g.vram_alloc.size(); v[2] = g.vram_alloc.bytes_free();") && has(vs, "v[3] = g.vram_alloc_hi.size(); v[4] = g.vram_alloc_hi.bytes_free();"), "P6: reports vramLimit, the mapped BAR0 bytes, BAR0 phys and both pools' totals and free bytes");
    expect(at(b, "if (action == n48disp::kActVramStat) {") < at(b, "if (action == 83 || action == 84"), "P6: served before the pipe verbs");
    const std::string cli = slurp(root + "/tools/pc/navi48test.c");
    expect(has(cli, "else if (what && !strcmp(what, \"vramstat\")) in = 108;") && has(cli, "in == 108 ? \"vramstat\"") && has(cli, "if (in == 108) return cmd_vramstat(arg);") && has(cli, "static int cmd_vramstat(const char *a1) {") && has(cli, "dr_call(108, 0, o)"), "P6: the CLI sites (table, name, dispatch, command)");
    expect(has(cli, "scdcread appallow fbpublish m6stat m6xstat vramstat\\n") && has(cli, "|m6xstat [0|1|2]|vramstat|capstream"), "P6: the known list and the usage string name vramstat");
    const std::string uc = slurp(root + "/src/navi48-bringup/src/Navi48UserClient.cpp");
    expect(has(uc, "108 = vramstat"), "P6: the user client's table comment names the verb (its bound is the pure admission)");
    const std::string pure = slurp(root + "/src/navi48-bringup/src/amd/native_disp_pure.h");
    expect(has(pure, "constexpr uint32_t kActVramStat = 108u;") && has(pure, "action == kActM6XStat || action == kActVramStat)); }") && has(pure, "(action == kActVramStat && arg == 0ull) ||"), "P6: the pure admission lines");
}

static void p7_mesa(const std::string &root) {
    // The kernel's figures at 1 GiB; radv_get_vram_size = total - visible; the heap rule vram * 9 >= visible; max_allocation = vis * 3 / 4 (ac_darwin_drm.c acd_info_memory).
    for (int k = 0; k < 2; k++) {
        const uint64_t vis = (k ? 985 : 217) * MiB, hi = (k ? 14512 : 15280) * MiB;
        const uint64_t vramTotal = vis + hi, radvVram = vramTotal - vis, maxAlloc = vis * 3 / 4;
        expect(radvVram > 0 && radvVram * 9 >= vis, k ? "P7: at 985 / 14512 MiB RADV keeps a separate VRAM heap" : "P7: at 217 / 15280 MiB RADV keeps a separate VRAM heap");
        expect_u(k ? "P7: the VRAM heap is 14512 MiB at 1 GiB" : "P7: the VRAM heap is 15280 MiB at 256 MiB", radvVram, k ? 14512 * MiB : 15280 * MiB);
        expect_u(k ? "P7: the largest allocation is 738 MiB (738.75, truncated) at 1 GiB" : "P7: the largest allocation is 162.75 MiB (the contract rounds it to 163) at 256 MiB", maxAlloc, k ? 985 * MiB * 3 / 4 : 217 * MiB * 3 / 4);
        expect_u(k ? "P7: 738 whole MiB at 1 GiB" : "P7: 162 whole MiB at 256 MiB", maxAlloc >> 20, k ? 738 : 162);
        expect_u("P7: total VRAM is 15497 MiB either way", vramTotal >> 20, 15497);
    }
    // Pin the formulas against the Mesa tree when it is next to this one (no Mesa change is part of this build).
    const char *home = std::getenv("HOME");
    const std::string mesa = std::string(home ? home : "") + "/navi48-native/mesa-mac/src/amd/";
    const std::string drm = slurp(mesa + "common/darwin/ac_darwin_drm.c"), rpd = slurp(mesa + "vulkan/radv_physical_device.c");
    if (drm.empty() || rpd.empty()) { std::printf("note: the Mesa tree is not at %s: P7 formula pins skipped\n", mesa.c_str()); (void)root; return; }
    expect(has(drm, "m->vram.max_allocation = k.vram_vis_total * 3 / 4;") && has(drm, "const uint64_t vram_total = k.vram_vis_total + k.vram_hi_total;") && has(drm, "m->cpu_accessible_vram.total_heap_size = k.vram_vis_total;"), "P7: ac_darwin_drm.c computes the heaps from the kernel's figures (no 256 MiB constant)");
    expect(has(rpd, "if (vram_size > 0 && vram_size * 9 >= visible_vram_size) {") && has(rpd, "return total_size - MIN2(total_size, (uint64_t)pdev->info.vram_vis_size_kb * 1024);"), "P7: radv_physical_device.c keeps a VRAM heap while hidden VRAM >= visible / 9");
}

// P8: the identity at 256 MiB. The 0.0.662 arithmetic (dev.vramLimit = bar0Size; visible = bar0Size when it fits VRAM; alloc = vramLimit - off; hi when VRAM > limit + reserve) against the 0.0.663 plan, over a sweep of VRAM sizes.
static void p8_identity() {
    for (uint64_t vramMiB = 1088; vramMiB <= 32768; vramMiB += 16) {   // below 1088 MiB the clamp (VRAM - reserve - 64 MiB) is under 256 MiB: not a card this kext sees
        const uint64_t vram = vramMiB * MiB;
        PlanIn p = mk(256 * MiB, 0xD0000000ull); p.vramBytes = vram;
        const PlanOut o = plan_bar0(p);
        if (!o.ok) continue;                                   // a VRAM too small for a 256 MiB window was never a configuration this card has
        const uint64_t oldLimit = 256 * MiB, oldVisible = (oldLimit <= vram) ? oldLimit : 123;
        const uint64_t oldAlloc = oldLimit - (kBase + kAllocOff);
        const bool oldHi = vram > oldLimit + kReserve; const uint64_t oldHiSize = oldHi ? vram - kReserve - oldLimit : 0;
        const Pools pl = pool_sizes(kBase, kAllocOff, o.vramLimit, vram, kReserve);
        if (o.vramLimit != oldLimit || visible_size(o.vramLimit, o.mapLen, vram, 123) != oldVisible || pl.visSize != oldAlloc || pl.hi != oldHi || pl.hiSize != oldHiSize) { expect(false, "P8: the 256 MiB plan differs from the 0.0.662 arithmetic"); return; }
    }
    expect(true, "P8: for every VRAM size from 1088 MiB to 32 GiB (16 MiB steps) a 256 MiB BAR0 gives 0.0.662's vramLimit, visible size and both pools exactly");
    const PlanOut o = plan_bar0(mk(256 * MiB, 0xD0000000ull));
    expect_u("P8: this card: limit 256 MiB", o.vramLimit, 256 * MiB);
    // a VRAM size that is not a multiple of 64 KiB cannot move a 256 MiB limit (it is far below the clamp)
    PlanIn q = mk(256 * MiB, 0xD0000000ull); q.vramBytes = kVram + 4096; expect_u("P8: an odd VRAM size leaves the 256 MiB limit alone", plan_bar0(q).vramLimit, 256 * MiB);
}

// P9 (item 9): the four variants are the live variants with EXACTLY the contract's changes: -rebar0 drops npci=0x2000 from boot-args; -rebar1g also sets Booter Quirks ResizeAppleGpuBars to 10;
// ResizeGpuBars stays -1; nothing else differs.
static std::string replace_once(const std::string &s, const std::string &o, const std::string &n) { const size_t p = s.find(o); return p == std::string::npos || count_of(s, o) != 1 ? std::string("<<NOT ONCE>>") : s.substr(0, p) + n + s.substr(p + o.size()); }
static void p9_variants(const std::string &root) {
    const std::string D = root + "/variants/configs/";
    const char *bases[2] = { "stage17-native-1440-metal-disp-amfi-ms-apps", "stage17-native-1440-metal-disp-amfi-m6flip3" };
    for (const char *b : bases) {
        const std::string base = slurp(D + b + ".plist"), r0 = slurp(D + b + "-rebar0.plist"), r1 = slurp(D + b + "-rebar1g.plist");
        const std::string q0 = "<key>ResizeAppleGpuBars</key>\n\t\t\t<integer>0</integer>", q10 = "<key>ResizeAppleGpuBars</key>\n\t\t\t<integer>10</integer>";
        expect(!base.empty() && !r0.empty() && !r1.empty(), std::string(std::string("P9: ") + b + " and both rebar variants exist").c_str());
        expect(has(base, " npci=0x2000 ") && has(base, q0), "P9: the live variant has npci=0x2000 and ResizeAppleGpuBars 0 (the copy source is the current live one)");
        const std::string e0 = replace_once(base, " npci=0x2000", ""), e1 = replace_once(e0, q0, q10);
        expect(r0 == e0, "P9: -rebar0 is the live variant minus npci=0x2000 and nothing else");
        expect(r1 == e1, "P9: -rebar1g is -rebar0 plus ResizeAppleGpuBars 10 and nothing else");
        expect(!has(r0, "npci") && !has(r1, "npci"), "P9: no npci in either rebar variant");
        expect(has(r0, q0) && has(r1, q10) && !has(r1, q0), "P9: ResizeAppleGpuBars is 0 in -rebar0 and 10 in -rebar1g");
        expect(has(r0, "<key>ResizeGpuBars</key>\n\t\t\t<integer>-1</integer>") && has(r1, "<key>ResizeGpuBars</key>\n\t\t\t<integer>-1</integer>"), "P9: ResizeGpuBars stays -1");
    }
    const std::string rescue = slurp(root + "/variants/RESCUE/EFI/OC/config.plist");
    expect(!rescue.empty() && has(rescue, "npci=0x2000"), "P9: RESCUE is untouched (still carries npci=0x2000)");
}

// P10 (0.0.664): F2 nbif + mapDoorbells, F4 dropBar0, F5 layout call, vramstat census.
static std::string K_gmc(const std::string &root) { return slurp(root + "/src/navi48-bringup/src/amd/gmc_v12_0.cpp"); }
static void p10_pins664(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/src/";
    const std::string b = slurp(K + "Navi48Bringup.cpp"), nbif = slurp(K + "amd/nbif_v6_3_1.cpp"), cli = slurp(root + "/tools/pc/navi48test.c"), hpp = slurp(K + "Navi48Bringup.hpp");
    const std::string ns = between(nbif, "kern_return_t nbif_v6_3_1_enable_doorbell_selfring_aperture(", "// 3. GC doorbell routing");
    expect(!ns.empty() && has(ns, "const bool selfringOn = enable && kr == kIOReturnSuccess;") && has(ns, "    if (selfringOn) {\n        tmp = REG_SET_FIELD") && !has(ns, "    if (enable) {\n        tmp = REG_SET_FIELD"), "P10 F2: CNTL EN is set only when BASE_LOW and BASE_HIGH both read back");
    expect(at(ns, "writing BASE_HIGH") < at(ns, "const bool selfringOn") && at(ns, "const bool selfringOn") < at(ns, "nbif_commit(dev, kRegSelfringCntl"), "P10 F2: the decision comes after both base writes and before the CNTL write");
    expect(has(ns, "NOT enabling the self-ring aperture (CNTL = 0)") && has(ns, "    return kr;\n}"), "P10 F2: a mismatch is logged and the error returned");
    expect(has(ns, "if (k != kIOReturnSuccess) kr = k;") && count_of(ns, "if (k != kIOReturnSuccess) kr = k;") == 2, "P10 F2: both base read-backs feed kr");
    const std::string md = between(b, "bool Navi48Bringup::mapDoorbells() {", "void Navi48Bringup::dropBar0()");
    expect(has(md, "n48rebar::bar_addr_agree((uint64_t)bar->getPhysicalAddress(), n48rebar::cfg_bar_addr(clo, chi))") && at(md, "bar_addr_agree") < at(md, "bar2Map = bar->map();") && has(md, "BAR2 refused"), "P10 F2: mapDoorbells refuses when IOPCIFamily's BAR2 address != config space's, before mapping");
    const std::string db = between(b, "void Navi48Bringup::dropBar0() {", "void Navi48Bringup::unmapVramAperture() {");
    expect(has(db, "unmapVramAperture();") && has(db, "bar0Phys = 0; bar0Limit = 0; vramBase = 0;"), "P10 F4: dropBar0 unmaps and clears every BAR0 field");
    const std::string sp = between(b, "void Navi48Bringup::stagePSP() {", "// Compat layer for the ported mac-amdgpu modules");
    expect(has(sp, "if (!chooseVramBase())         { dropBar0();") && has(sp, "if (!finishVramPlan())         { dropBar0();") && has(sp, "if (!apertureCheckWindow())    { dropBar0();"), "P10 F4: each refusal after the map drops BAR0 (the no-BAR0 state)");
    const std::string aw = between(b, "bool Navi48Bringup::apertureCheckWindow() {", "// ---------------------------------------------------------------------------\n// Read-only survey stages");
    expect(has(aw, "if (!n48rebar::layout_ok(") && at(aw, "n48rebar::layout_ok(") < at(aw, "all = apertureCheck(pt[i])") && has(aw, "n48rebar::kVisAllocOffset"), "P10 F5: the console/cursor layout is judged before any check point is written");
    expect(has(K_gmc(root), "static_assert(kGMCVRAMAllocOffset == n48rebar::kVisAllocOffset"), "P10 F5: the layout check uses the allocator's own offset");
    expect(has(b, "rebarMask = rebarFound ?") && has(b, "((uint64_t)rebarMask << 32)") && has(b, "((rebarCurMB & 0xFFFFFFull) << 8)") && has(hpp, "uint32_t           rebarMask"), "P10: vramstat reports the ReBAR mask and current size");
    expect(has(cli, "supported-sizes mask %#x") && has(cli, "ResizeAppleGpuBars code %u") && has(cli, "(v[11] >> 32)"), "P10: the CLI prints the supported sizes, the smallest and its ResizeAppleGpuBars code");
    expect(has(b, "smallest %llu MiB, current %llu MiB"), "P10: the census logs the smallest supported size");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    p1_plan(); p2_census(); p3_pure(); p4_high(); p8_identity(); p9_variants(root); p3b_layout(); p10_pins664(root); p5_pins(root); p6_vramstat(root); p7_mesa(root);
    std::printf("native_rebar_test: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
