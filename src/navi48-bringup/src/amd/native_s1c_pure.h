//
//  native_s1c_pure.h - the PURE half of native-stack step S1c (kext 0.0.601): the arithmetic and the decisions behind the native
//  user client (an internal design note). No kernel header is included, so tests/native_s1c_test.cpp compiles this on the
//  host Mac and drives the exact functions the kext calls. The hardware/IOKit half is native_s1c.cpp and Navi48NativeClient.cpp.
//
//  Contents: return codes, BoCreate placement decisions, GemVa validation and the flag -> PTE table, the VA map (overlap, exact
//  match, EXEC coverage of an IB), the page-table walker/mapper over an abstract memory (host tests walk the result), the
//  Submit chunk parser (exact-size, refuse-everything-else), the ring free-space arithmetic including wrap, the HUNG latch state
//  machine, WaitSeq target resolution, the Submit packet builder (user fence BEFORE the kernel seqno) and the fence-window
//  slot picker.
//
#pragma once
#include <stdint.h>

#include "Navi48NativeABI.h"
#include "native_s1b_pure.h"
#include "cp_pm4_gfx12.h"     // cp_p3, kP3_* (stdint only)
#include "amdgpu_pm4.h"       // pm4_release_mem_dw1/2 (stdint only)

namespace n48native {
namespace s1c {

// ---- return codes: the kIOReturn values the contract names (the kext static_asserts them against IOReturn.h) ------------------
constexpr uint32_t kOk = 0u;
constexpr uint32_t kBadArg = 0xe00002c2u, kNotFound = 0xe00002f0u, kUnsupported = 0xe00002c7u, kNoMemory = 0xe00002bdu;
constexpr uint32_t kNoResources = 0xe00002beu, kNotReady = 0xe00002d8u, kExclusive = 0xe00002c5u, kNotPrivileged = 0xe00002c1u;
constexpr uint32_t kNotPermitted = 0xe00002e2u, kTimeout = 0xe00002d6u, kAborted = 0xe00002ebu;

// ---- limits ---------------------------------------------------------------------------------------------------------------------
constexpr uint64_t kPage       = 4096ull;
constexpr uint64_t kMaxAlign   = 2ull << 20;
constexpr uint64_t kMaxBoBytes = 1ull << 40;        // overflow guard only; real limits are the pools
constexpr uint64_t kGttCap     = 512ull << 20;      // per client
constexpr uint64_t kGttMaxBo   = 64ull << 20;
constexpr uint64_t kPtCap      = 32ull << 20;       // page-table bytes per client
constexpr uint32_t kMaxMaps    = 4096u;
constexpr uint64_t kVaFirst    = 0x10000ull;
constexpr uint64_t kWaitCapNs  = N48N_WAIT_CAP_NS;
constexpr uint32_t kRingSlack  = 16u;
constexpr uint32_t kWbOffsetNativeSeq = 0x0C0u;     // in the CP write-back page; Rptr 0x000, Wptr 0x040, Fence 0x080 are taken
static_assert(kWbOffsetNativeSeq % 8u == 0u && kWbOffsetNativeSeq != 0x000u && kWbOffsetNativeSeq != 0x040u &&
              kWbOffsetNativeSeq != 0x080u && (kWbOffsetNativeSeq >= 0x088u), "the native seqno slot: aligned, clear of Rptr/Wptr/Fence");

// ---- generic table helpers ----------------------------------------------------------------------------------------------
// Lowest free id in [first, last] of `used` (indexed by id), or 0 when full. 0 is never a valid id.
inline uint32_t lowest_free(const uint8_t *used, uint32_t first, uint32_t last) {
    for (uint32_t i = first; i <= last; i++) if (used[i] == 0) return i;
    return 0u;
}
inline bool id_live(const uint8_t *used, uint32_t last, uint32_t id) { return id != 0u && id <= last && used[id] != 0; }
// used + add > cap without overflow.
constexpr bool would_exceed(uint64_t used, uint64_t add, uint64_t cap) { return add > cap || used > cap - add; }

// ---- BoCreate placement -----------------------------------------------------------------------------------------------------
enum PlaceKind : uint32_t { kPlaceNone = 0, kPlaceVis = 1, kPlaceGtt = 2 };
struct Place { uint32_t rc; uint32_t kind; uint64_t size; uint64_t align; bool uc; bool hiOk; };
constexpr uint64_t kGemAccepted = N48N_GEM_CPU_ACCESS_REQUIRED | N48N_GEM_NO_CPU_ACCESS | N48N_GEM_CPU_GTT_USWC | N48N_GEM_VRAM_CLEARED |
    N48N_GEM_VRAM_CONTIGUOUS | N48N_GEM_VM_ALWAYS_VALID | N48N_GEM_EXPLICIT_SYNC | N48N_GEM_DISCARDABLE | N48N_GEM_UNCACHED |
    N48N_GEM_GFX12_DCC | N48N_GEM_VIRTIO_SHARED;
constexpr bool pow2(uint64_t v) { return v != 0ull && (v & (v - 1ull)) == 0ull; }
// The contract's S1c policy. Malformed sizes/alignments are BadArg; a well-formed request S1c does not implement (a CPU/GDS/GWS/OA/
// DOORBELL domain, no domain, an unlisted/refused GEM flag) is Unsupported. Anything containing VRAM goes to the visible pool first.
constexpr Place place_decide(uint64_t boSize, uint64_t alignment, uint64_t domains, uint64_t flags) {
    if (boSize == 0ull || boSize > kMaxBoBytes) return Place{ kBadArg, kPlaceNone, 0, 0, false, false };
    if (alignment != 0ull && (!pow2(alignment) || alignment > kMaxAlign)) return Place{ kBadArg, kPlaceNone, 0, 0, false, false };
    const uint64_t size = (boSize + kPage - 1ull) & ~(kPage - 1ull);
    const uint64_t align = alignment < kPage ? kPage : alignment;
    if (domains == 0ull || (domains & ~(uint64_t)(N48N_GEM_DOMAIN_GTT | N48N_GEM_DOMAIN_VRAM)) != 0ull)
        return Place{ kUnsupported, kPlaceNone, size, align, false, false };
    if ((flags & ~kGemAccepted) != 0ull) return Place{ kUnsupported, kPlaceNone, size, align, false, false };
    const bool hiOk = (flags & N48N_GEM_NO_CPU_ACCESS) != 0ull && (flags & N48N_GEM_CPU_ACCESS_REQUIRED) == 0ull;
    return Place{ kOk, (domains & N48N_GEM_DOMAIN_VRAM) ? (uint32_t)kPlaceVis : (uint32_t)kPlaceGtt, size, align,
                  (flags & N48N_GEM_UNCACHED) != 0ull, hiOk };
}

// ---- GemVa ------------------------------------------------------------------------------------------------------------------
struct VaReq { uint32_t rc; uint64_t stripped; };   // rc + the 48-bit stripped start
constexpr uint32_t kVmFlagsMapOk = N48N_VM_DELAY_UPDATE | N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE |
                                   N48N_VM_PAGE_PRT | N48N_VM_MTYPE_MASK | N48N_VM_PAGE_NOALLOC;
constexpr bool vm_mtype_known(uint32_t flags) { return ((flags & N48N_VM_MTYPE_MASK) >> 5) <= 5u; }
// Everything that can be judged from the struct alone (the BO is looked up afterwards). The range must sit inside one half of the
// canonical space, the stripped start must be >= 0x10000, everything 4 KiB aligned, and the timeline fields zero.
constexpr VaReq gemva_check(uint32_t operation, uint32_t handle, uint32_t pad, uint32_t flags, uint64_t va, uint64_t off, uint64_t size,
                            uint64_t tlPoint, uint32_t tlSyncobj, uint32_t nSyncobj, uint64_t inFence) {
    if (operation != N48N_VA_OP_MAP && operation != N48N_VA_OP_UNMAP && operation != N48N_VA_OP_CLEAR && operation != N48N_VA_OP_REPLACE)
        return VaReq{ kBadArg, 0 };
    if (pad != 0u || tlPoint != 0ull || tlSyncobj != 0u || nSyncobj != 0u || inFence != 0ull) return VaReq{ kBadArg, 0 };
    if (operation == N48N_VA_OP_CLEAR || operation == N48N_VA_OP_REPLACE) return VaReq{ kUnsupported, 0 };
    if (handle == 0u || (flags & N48N_VM_PAGE_PRT) != 0u) return VaReq{ kUnsupported, 0 };
    if (operation == N48N_VA_OP_MAP && ((flags & ~kVmFlagsMapOk) != 0u || !vm_mtype_known(flags))) return VaReq{ kBadArg, 0 };
    if (((va | off | size) & (kPage - 1ull)) != 0ull || size == 0ull) return VaReq{ kBadArg, 0 };
    if (!va_canonical(va)) return VaReq{ kBadArg, 0 };
    const uint64_t s = va_strip(va);
    if (s < kVaFirst) return VaReq{ kBadArg, 0 };
    if (size > kVaMask48 + 1ull || s + size > kVaMask48 + 1ull) return VaReq{ kBadArg, 0 };
    if ((s >> 47) != ((s + size - 1ull) >> 47)) return VaReq{ kBadArg, 0 };      // never straddle the sign-extension seam
    return VaReq{ kOk, s };
}
constexpr bool range_in_bo(uint64_t off, uint64_t size, uint64_t boSize) { return off <= boSize && size <= boSize - off; }

// The leaf flags of a mapping (pte_encode adds IS_PTE): READ/WRITE/EXEC from the vm flags, MTYPE UC when the vm flags say UC or the
// BO was created UNCACHED (upstream gmc_v12_0_get_vm_pte), every other MTYPE = NC = 0; a GTT page adds SYSTEM|SNOOPED.
constexpr uint64_t leaf_flags(uint32_t vmflags, bool boUncached, bool system) {
    return kPteValid | ((vmflags & N48N_VM_PAGE_READABLE) ? kPteRead : 0ull) | ((vmflags & N48N_VM_PAGE_WRITEABLE) ? kPteWrite : 0ull) |
           ((vmflags & N48N_VM_PAGE_EXECUTABLE) ? kPteExec : 0ull) |
           (((vmflags & N48N_VM_MTYPE_MASK) == N48N_VM_MTYPE_UC || boUncached) ? kPteMtypeUC : 0ull) |
           (system ? (kPteSystem | kPteSnooped) : 0ull);
}
// The flags the S1d recording carries: 0xe (R|W|X, MTYPE 0 -> NC), 0xa (R|X, NC), 0x8a (R|X, MTYPE 4 = UC).
static_assert(leaf_flags(0xe, false, false) == (kPteValid | kPteRead | kPteWrite | kPteExec), "0xe = R|W|X, NC");
static_assert(leaf_flags(0xa, false, false) == (kPteValid | kPteRead | kPteExec), "0xa = R|X, NC");
static_assert(leaf_flags(0x8a, false, false) == (kPteValid | kPteRead | kPteExec | kPteMtypeUC), "0x8a = R|X|UC");
static_assert(leaf_flags(0xe, false, true) == (kPteValid | kPteRead | kPteWrite | kPteExec | kPteSystem | kPteSnooped), "GTT adds SYSTEM|SNOOPED");
static_assert(leaf_flags(0xe, true, false) == (kPteValid | kPteRead | kPteWrite | kPteExec | kPteMtypeUC), "BO UNCACHED forces UC");

// ---- the VA map ---------------------------------------------------------------------------------------------------------------
struct VaEnt { uint64_t start; uint64_t size; uint64_t off; uint32_t handle; uint32_t flags; };   // stripped start; size 0 = free slot
// ReadRegs allowlist: the GB_ADDR_CONFIG neighbourhood only (absolute dwords 0x263e..0x2641). Anything else is BadArgument.
constexpr uint32_t kRegsAllowFirst = 0x263Eu, kRegsAllowLast = 0x2641u;
constexpr bool regs_allowed(uint64_t off, uint64_t count) {
    return count >= 1ull && count <= 16ull && off >= kRegsAllowFirst && off <= kRegsAllowLast && off + count - 1ull <= kRegsAllowLast;
}
constexpr bool va_ent_used(const VaEnt &e) { return e.size != 0ull; }
inline int va_overlap(const VaEnt *t, uint32_t n, uint64_t start, uint64_t size) {
    for (uint32_t i = 0; i < n; i++)
        if (va_ent_used(t[i]) && start < t[i].start + t[i].size && t[i].start < start + size) return (int)i;
    return -1;
}
inline int va_find_exact(const VaEnt *t, uint32_t n, uint32_t handle, uint64_t start, uint64_t size) {
    for (uint32_t i = 0; i < n; i++)
        if (va_ent_used(t[i]) && t[i].handle == handle && t[i].start == start && t[i].size == size) return (int)i;
    return -1;
}
inline int va_insert(VaEnt *t, uint32_t n, const VaEnt &e) {
    for (uint32_t i = 0; i < n; i++) if (!va_ent_used(t[i])) { t[i] = e; return (int)i; }
    return -1;
}
inline uint32_t va_remove_handle(VaEnt *t, uint32_t n, uint32_t handle) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) if (va_ent_used(t[i]) && t[i].handle == handle) { t[i] = VaEnt{ 0, 0, 0, 0, 0 }; r++; }
    return r;
}
inline uint32_t va_count_handle(const VaEnt *t, uint32_t n, uint32_t handle) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < n; i++) if (va_ent_used(t[i]) && t[i].handle == handle) r++;
    return r;
}
constexpr uint32_t kIbNeedFlags = N48N_VM_PAGE_READABLE | N48N_VM_PAGE_EXECUTABLE;
// [start, start + bytes) (stripped) is entirely inside READ+EXEC-mapped ranges of this client. Mappings never overlap, so walking from the
// start and jumping to each covering mapping's end is exact.
inline bool va_exec_covered(const VaEnt *t, uint32_t n, uint64_t start, uint64_t bytes) {
    if (bytes == 0ull) return false;
    const uint64_t end = start + bytes;
    if (end < start) return false;
    uint64_t cur = start;
    for (uint32_t guard = 0; cur < end && guard <= n; guard++) {
        int hit = -1;
        for (uint32_t i = 0; i < n; i++)
            if (va_ent_used(t[i]) && t[i].start <= cur && cur < t[i].start + t[i].size) { hit = (int)i; break; }
        if (hit < 0 || (t[hit].flags & kIbNeedFlags) != kIbNeedFlags) return false;   // an IB fetch needs READ and EXEC
        cur = t[hit].start + t[hit].size;
    }
    return cur >= end;
}

// ---- the page-table tree over an abstract memory ------------------------------------------------------------------------------
// `M` supplies: uint64_t rd(uint64_t tablePa, uint32_t idx); void wr(uint64_t tablePa, uint32_t idx, uint64_t v);
//               uint64_t alloc_page()  -> the pa of a fresh ZEROED 4 KiB table page, or 0 when the page-table cap is reached.
// Ensures the PDB1 / PDB0 / PTB chain for `va` exists under `rootPa` and returns the PTB's pa (0 on exhaustion or an unencodable pa).
template <class M>
inline uint64_t pt_ensure_ptb(uint64_t rootPa, uint64_t va, M &m) {
    uint64_t tbl = rootPa;
    const uint32_t idx[3] = { idx_pdb2(va), idx_pdb1(va), idx_pdb0(va) };
    for (uint32_t lvl = 0; lvl < 3; lvl++) {
        uint64_t e = m.rd(tbl, idx[lvl]);
        if (!entry_valid(e)) {
            const uint64_t pa = m.alloc_page();
            const uint64_t enc = pa ? pde_encode(pa) : 0ull;
            if (enc == 0ull) return 0ull;
            m.wr(tbl, idx[lvl], enc);
            e = enc;
        }
        tbl = entry_pa(e);
    }
    return tbl;
}
// Map `pages` 4 KiB pages at `va` to consecutive physical pages starting at `firstPa`. Two passes: every table page first (the only
// step that can fail), then the leaves, so a failed call leaves no partial mapping. Returns kOk / kNoMemory / kBadArg.
template <class M>
inline uint32_t pt_map(uint64_t rootPa, uint64_t va, uint64_t pages, uint64_t firstPa, uint64_t leafFlags, M &m) {
    if (pages == 0ull) return kBadArg;
    for (uint64_t p = 0; p < pages;) {
        const uint64_t v = va + p * kPage;
        if (pt_ensure_ptb(rootPa, v, m) == 0ull) return kNoMemory;
        p += 512ull - idx_ptb(v);                                 // to the next PTB
    }
    for (uint64_t p = 0; p < pages;) {
        const uint64_t v = va + p * kPage;
        const uint64_t ptb = pt_ensure_ptb(rootPa, v, m);         // all present now: no allocation
        if (ptb == 0ull) return kNoMemory;
        const uint64_t run = 512ull - idx_ptb(v) < pages - p ? 512ull - idx_ptb(v) : pages - p;
        for (uint64_t k = 0; k < run; k++) {
            const uint64_t pte = pte_encode(firstPa + (p + k) * kPage, leafFlags);
            if (pte == 0ull) return kBadArg;
            m.wr(ptb, idx_ptb(v) + (uint32_t)k, pte);
        }
        p += run;
    }
    return kOk;
}
// Clear `pages` leaves at `va` (a missing table is skipped: nothing was ever mapped there).
template <class M>
inline void pt_unmap(uint64_t rootPa, uint64_t va, uint64_t pages, M &m) {
    for (uint64_t p = 0; p < pages;) {
        const uint64_t v = va + p * kPage;
        uint64_t tbl = rootPa; bool ok = true;
        const uint32_t idx[3] = { idx_pdb2(v), idx_pdb1(v), idx_pdb0(v) };
        for (uint32_t lvl = 0; lvl < 3 && ok; lvl++) {
            const uint64_t e = m.rd(tbl, idx[lvl]);
            if (!entry_valid(e)) ok = false; else tbl = entry_pa(e);
        }
        const uint64_t run = 512ull - idx_ptb(v) < pages - p ? 512ull - idx_ptb(v) : pages - p;
        if (ok) for (uint64_t k = 0; k < run; k++) m.wr(tbl, idx_ptb(v) + (uint32_t)k, 0ull);
        p += run;
    }
}
// Host-test walker: the physical address and leaf flags `va` resolves to under `rootPa` (0 when a level is invalid).
template <class M>
inline uint64_t pt_walk(uint64_t rootPa, uint64_t va, M &m, uint64_t *leafOut) {
    uint64_t tbl = rootPa;
    const uint32_t idx[3] = { idx_pdb2(va), idx_pdb1(va), idx_pdb0(va) };
    for (uint32_t lvl = 0; lvl < 3; lvl++) {
        const uint64_t e = m.rd(tbl, idx[lvl]);
        if (!entry_valid(e) || entry_is_leaf(e)) return 0ull;
        tbl = entry_pa(e);
    }
    const uint64_t leaf = m.rd(tbl, idx_ptb(va));
    if (leafOut) *leafOut = leaf;
    return entry_valid(leaf) ? (entry_pa(leaf) | (va & (kPage - 1ull))) : 0ull;
}

// ---- Submit: parse and validate the chunk (everything that needs no table lookups) ---------------------------------------------
constexpr uint32_t kCsHdr = 32u, kCsIb = 32u;
constexpr uint32_t kIbUnsupportedFlags = N48N_IB_FLAG_CE | N48N_IB_FLAG_RESET_GDS_MAX_WAVE_ID | N48N_IB_FLAG_SECURE | N48N_IB_FLAG_EMIT_MEM_SYNC;
constexpr uint32_t kIbKnownFlags = kIbUnsupportedFlags | N48N_IB_FLAG_PREAMBLE | N48N_IB_FLAG_PREEMPT | N48N_IB_FLAG_TC_WB_NOT_INVALIDATE;
constexpr uint32_t kIbMaxDw = 0xFFFFFu;
inline uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
inline uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32); }
struct CsView { uint32_t ctx, nIbs, flags, fenceHandle, fenceOffset; uint32_t badIb; };
// Validates the header and every IB record: `size` must be exactly 32 + 32*num_ibs (checked against num_ibs read from the bytes, so a
// short buffer can never be over-read). Returns kOk / kBadArg / kUnsupported; nothing here touches the ring.
inline uint32_t cs_parse(const uint8_t *p, uint32_t size, CsView *v) {
    v->badIb = 0xFFFFFFFFu;
    if (p == nullptr || size < kCsHdr) return kBadArg;
    if (rd32(p + 0) != N48N_ABI_VERSION) return kBadArg;
    v->ctx = rd32(p + 4); v->nIbs = rd32(p + 8); v->flags = rd32(p + 12);
    v->fenceHandle = rd32(p + 16); v->fenceOffset = rd32(p + 20);
    if (v->nIbs < 1u || v->nIbs > N48N_MAX_IBS) return kBadArg;
    if (size != kCsHdr + kCsIb * v->nIbs) return kBadArg;
    if (rd64(p + 24) != 0ull) return kBadArg;
    if ((v->flags & ~(uint32_t)N48N_CS_HAS_FENCE) != 0u) return kBadArg;
    if ((v->flags & N48N_CS_HAS_FENCE) != 0u && (v->fenceOffset & 7u) != 0u) return kBadArg;
    for (uint32_t i = 0; i < v->nIbs; i++) {
        const uint8_t *r = p + kCsHdr + kCsIb * i;
        const uint32_t pad = rd32(r + 0), fl = rd32(r + 4), bytes = rd32(r + 16), ipType = rd32(r + 20), ipInst = rd32(r + 24), ring = rd32(r + 28);
        const uint64_t va = rd64(r + 8);
        v->badIb = i;
        if (pad != 0u || ipType != N48N_HW_IP_GFX || ipInst != 0u || ring != 0u) return kBadArg;
        if ((fl & ~kIbKnownFlags) != 0u) return kBadArg;
        if ((fl & kIbUnsupportedFlags) != 0u) return kUnsupported;
        if (bytes == 0u || (bytes & 3u) != 0u || bytes > kIbMaxDw * 4u) return kBadArg;
        if (!va_canonical(va) || (va & 3ull) != 0ull) return kBadArg;
    }
    v->badIb = 0xFFFFFFFFu;
    return kOk;
}
inline void cs_ib(const uint8_t *p, uint32_t i, uint64_t *va, uint32_t *bytes) {
    const uint8_t *r = p + kCsHdr + kCsIb * i;
    *va = rd64(r + 8); *bytes = rd32(r + 16);
}
constexpr bool cs_fence_in_bo(uint32_t off, uint64_t boSize) { return (off & 7u) == 0u && boSize >= 8ull && off <= boSize - 8ull; }

// ---- ring arithmetic ----------------------------------------------------------------------------------------------------------
constexpr uint32_t ring_free(uint32_t rptr, uint32_t wptr, uint32_t mask) { return (rptr - wptr - 1u) & mask; }
// free >= need + 16 (the contract's slack), overflow-safe.
// The native 64-bit write counter: monotonically increasing; `& mask` is only the ring index and the space check.
constexpr uint32_t ring_idx(uint64_t wc, uint32_t mask) { return (uint32_t)(wc & mask); }
constexpr uint32_t ring_free64(uint32_t rptr, uint64_t wc, uint32_t mask) { return ring_free(rptr & mask, ring_idx(wc, mask), mask); }
constexpr bool ring_has_space(uint32_t freeDw, uint32_t need) { return need <= 0xFFFFu && freeDw >= need + kRingSlack; }
constexpr uint32_t ring_advance(uint32_t wptr, uint32_t n, uint32_t mask) { return (wptr + n) & mask; }
// 3 (CONTEXT_CONTROL) + 4 per IB + 8 for the kernel seqno RELEASE_MEM + 8 for the user fence when asked.
constexpr uint32_t submit_dwords(uint32_t nIbs, bool hasFence) { return 3u + 4u * nIbs + 8u + (hasFence ? 8u : 0u); }
static_assert(submit_dwords(N48N_MAX_IBS, true) == 275u, "the contract's 275 dwords for n = 64");

// ---- the HUNG latch -------------------------------------------------------------------------------------------------------------
constexpr uint64_t kHangNs = 2000000000ull;
struct Hang { uint64_t lastRetired; uint64_t tProgress; bool hung; };
// Record what a call saw: any advance of `retired` restarts the clock; an idle ring keeps it restarting (the clock starts at the KICK that
// makes the ring busy, hang_kick).
inline void hang_note(Hang &h, uint64_t retired, uint64_t emitted, uint64_t now) {
    if (retired != h.lastRetired) { h.lastRetired = retired; h.tProgress = now; }
    else if (retired >= emitted) h.tProgress = now;
}
// Called when a submit is about to make the ring busy: if it was idle, the clock starts now.
inline void hang_kick(Hang &h, uint64_t retired, uint64_t emittedBefore, uint64_t now) { if (retired >= emittedBefore) h.tProgress = now; }
constexpr bool hang_expired(const Hang &h, uint64_t retired, uint64_t emitted, uint64_t now) {
    return retired < emitted && now >= h.tProgress && now - h.tProgress >= kHangNs;
}
// One call's evaluation (rule 6.3b): true when THIS call detects the hang and latches it. An already-latched state is not a detection.
inline bool hang_detect(Hang &h, uint64_t retired, uint64_t emitted, uint64_t now) {
    if (h.hung) return false;
    hang_note(h, retired, emitted, now);
    if (!hang_expired(h, retired, emitted, now)) return false;
    h.hung = true;
    return true;
}
// A bounded internal wait that reached its 2 s bound (rule 6.3a) latches the same state.
inline bool hang_latch_wait(Hang &h) { if (h.hung) return false; h.hung = true; return true; }
// What a GPU-dependent call returns: the detecting call Timeout, every later call Aborted.
constexpr uint32_t hang_rc(bool hungBefore, bool detectedNow) { return hungBefore ? kAborted : (detectedNow ? kTimeout : kOk); }

// ---- WaitSeq ----------------------------------------------------------------------------------------------------------------------
// Resolve the input seqno. 0 -> idle at once (*resolved = 0); LAST -> the seq emitted at entry; a future seqno -> BadArg.
inline uint32_t wait_resolve(uint64_t target, uint64_t emitted, uint64_t *resolved) {
    if (target == N48N_SEQ_LAST) { *resolved = emitted; return kOk; }
    if (target > emitted) return kBadArg;
    *resolved = target;
    return kOk;
}
constexpr uint64_t wait_timeout_ns(uint64_t t) { return t > kWaitCapNs ? kWaitCapNs : t; }
// retired as read from the slot, never above what was emitted (a corrupt slot must not retire work that was never queued).
constexpr uint64_t retired_clamped(uint64_t slot, uint64_t emitted) { return slot > emitted ? emitted : slot; }

// ---- the Submit packet builder ----------------------------------------------------------------------------------------------------
// One RELEASE_MEM in upstream emit_fence form: GCR SEQ | GL2_WB, CACHE_FLUSH_AND_INV_TS, DATA_SEL 2 (64-bit), INT_SEL 0. 8 dwords.
inline uint32_t emit_release_mem(uint32_t *o, uint64_t addr, uint64_t data) {
    o[0] = amdgpu::cp_p3(amdgpu::kP3_RELEASE_MEM, 6);
    o[1] = amdgpu::pm4_release_mem_dw1();
    o[2] = amdgpu::pm4_release_mem_dw2(amdgpu::kPM4RMDataSel64, amdgpu::kPM4RMIntSelNone);
    o[3] = (uint32_t)(addr & 0xFFFFFFF8u);
    o[4] = (uint32_t)(addr >> 32);
    o[5] = (uint32_t)(data & 0xFFFFFFFFu);
    o[6] = (uint32_t)(data >> 32);
    o[7] = 0u;
    return 8u;
}
// The whole Submit, in the contract's order: CONTEXT_CONTROL, each IB (VA canonical, control = dwords | vmid << 24), the user fence,
// THEN the kernel seqno (so a retired seqno implies the user fence landed), then nothing. Returns the dword count, or 0 (and writes
// nothing beyond `cap`) when the arguments are unusable: n out of range, a misaligned fence/seq address, or a too-small buffer.
inline uint32_t build_submit(uint32_t *o, uint32_t cap, const uint64_t *ibVa, const uint32_t *ibBytes, uint32_t n, uint32_t vmid,
                             bool hasFence, uint64_t fenceAddr, uint64_t seqAddr, uint64_t seq) {
    if (n < 1u || n > N48N_MAX_IBS || vmid > 15u || (seqAddr & 7ull) != 0ull || (hasFence && (fenceAddr & 7ull) != 0ull)) return 0u;
    const uint32_t need = submit_dwords(n, hasFence);
    if (cap < need) return 0u;
    uint32_t k = 0;
    o[k++] = amdgpu::cp_p3(amdgpu::kP3_CONTEXT_CONTROL, 1);
    o[k++] = 0x80000000u;                                          // load_enable only, as cp_submit_ib / emit_cntxcntl
    o[k++] = 0u;
    for (uint32_t i = 0; i < n; i++) {
        o[k++] = amdgpu::cp_p3(amdgpu::kP3_INDIRECT_BUFFER, 2);    // no VALID bit: gfx_v12_0_ring_emit_ib_gfx
        o[k++] = (uint32_t)(ibVa[i] & 0xFFFFFFFFu);
        o[k++] = (uint32_t)(ibVa[i] >> 32);
        o[k++] = (ibBytes[i] / 4u) | (vmid << 24);
    }
    if (hasFence) k += emit_release_mem(o + k, fenceAddr, seq);
    k += emit_release_mem(o + k, seqAddr, seq);
    return k == need ? k : 0u;
}

// ---- the fence window (16 GART pages for GTT fence targets) ---------------------------------------------------------------------
struct FenceSlot { uint32_t handle; uint32_t page; uint8_t used; };
// The slot already bound to (handle, page) or the first free one; -1 when the window is full. *isNew tells the caller to bind.
inline int fence_slot_pick(const FenceSlot *s, uint32_t n, uint32_t handle, uint32_t page, bool *isNew) {
    int freeIdx = -1;
    for (uint32_t i = 0; i < n; i++) {
        if (s[i].used && s[i].handle == handle && s[i].page == page) { *isNew = false; return (int)i; }
        if (!s[i].used && freeIdx < 0) freeIdx = (int)i;
    }
    *isNew = true;
    return freeIdx;
}
// The VMID-0 address of a fence: a VRAM BO is addressed by its MC address; a GTT BO through the window slot.
constexpr uint64_t fence_addr_vram(uint64_t mc, uint32_t off) { return mc + off; }
constexpr uint64_t fence_addr_gtt(uint64_t windowMc, uint32_t slot, uint32_t off) { return windowMc + (uint64_t)slot * kPage + (off & (kPage - 1u)); }

} // namespace s1c
} // namespace n48native
