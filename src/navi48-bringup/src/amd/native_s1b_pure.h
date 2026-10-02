//
//  native_s1b_pure.h - the PURE half of native-stack step S1b (kext 0.0.600, boot-arg navi48-native=1).
//
//  No kernel header is included, so tests/native_s1b_test.cpp compiles this on the host Mac and drives the exact functions the kext
//  calls. Everything here is arithmetic: the boot-arg gate, the gfx12 page-table entry encoding (4 levels, 9 bits each, 48-bit
//  VA, the sign extension stripped), the VMID rules, the context CNTL value, the fault-control check and the LEG SEQUENCE
//  (which leg runs next, and when the latched stop ends the run). The hardware half is native_s1b.cpp.
//
//  Sources (upstream reference copies in ref/linux-amdgpu, which is not tracked in the worktree):
//    * gmc_v12_0.c   amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48): num_level 3 (PDB2 -> PDB1 -> PDB0 -> PTB), block 9 bits.
//    * gfxhub_v12_0.c setup_vmid_config: PAGE_TABLE_DEPTH = num_level, PAGE_TABLE_BLOCK_SIZE = block_size - 9,
//      RETRY_PERMISSION_OR_INVALID_PAGE_FAULT = !noretry (noretry = 1 for GC 12.0.1, so RETRY = 0).
//    * gmc_v12_0.c   get_vm_pde: a VRAM directory entry is base_offset + addr - vram_start, VALID only, P (bit 63) clear;
//      translate_further is never set for v12 in the reference, so there is no BFS on any directory entry (SUSPECTED: the
//      assignment lives in amdgpu_vm.c, which the reference tree does not carry; our L2_CNTL3 programming assumes the same).
//    * amdgpu_vm.h   AMDGPU_PTE_* / AMDGPU_PDE_PTE_GFX12 (bit 63) / MTYPE_GFX12 at 54.
//    * The LEAF word: upstream's GC 12.0.1 4 KiB leaves carry NO IS_PTE (P) bit (the review's reading; get_vm_pte sets it only for
//      PRT). We set P on the leaf anyway because that is the word gmc_vmfrag_selftest (0.0.193) proved on hardware for the
//      Apple-shaped tree: pa | IS_PTE | MTYPE_UC | EXEC | READ | WRITE | VALID; a directory pointer has P = 0. Whether a depth-3
//      tree also accepts P = 1 leaves is SUSPECTED and is what the first leg tests. There is no inverse-polarity fallback: with
//      upstream leaves carrying no P bit, a P=1-pointer / P=0-leaf tree can only fault.
//
#pragma once
#include <stdint.h>

namespace n48native {

// ---- VMID rules ---------------------------------------------------------------------------------------------------------
// mes_v12_1.cpp sets vmid_mask_gfxhub = 0xFE (MES schedules VMIDs 1..7). The native VMID must stay clear of it: 8..15.
constexpr uint32_t kMesVmidMaskGfxhub = 0xFEu;
constexpr uint32_t kNativeVmid        = 8u;
constexpr bool vmid_ok(uint32_t vmid) {
    return vmid >= 8u && vmid <= 15u && ((kMesVmidMaskGfxhub >> vmid) & 1u) == 0u;
}
static_assert(vmid_ok(kNativeVmid), "the native VMID must be in 8..15 and outside the MES mask");

// ---- the geometry the context is programmed with ----------------------------------------------------------------------------
constexpr uint32_t kCtxDepth     = 3u;   // num_level: PDB2 -> PDB1 -> PDB0 -> PTB
constexpr uint32_t kCtxBlockSize = 0u;   // block_size (9) - 9
constexpr uint64_t kMaxPfn       = (1ull << 48) >> 12;   // vm_manager.max_pfn: the END register is max_pfn - 1

// GCVM_CONTEXT1_CNTL fields (amdgpu_field_defs.h MMVM_CONTEXT0_CNTL__*; the kext asserts these equal the field-def shifts).
constexpr uint32_t kCntlEnableShift = 0u,  kCntlEnableMask = 0x1u;
constexpr uint32_t kCntlDepthShift  = 1u,  kCntlDepthMask  = 0x6u;
constexpr uint32_t kCntlBlockShift  = 4u,  kCntlBlockMask  = 0xF0u;
constexpr uint32_t kCntlRetryShift  = 8u,  kCntlRetryMask  = 0x100u;

// The CNTL value for the native VMID: the current value with ENABLE = 1, DEPTH = 3, BLOCK_SIZE = 0, RETRY = 0. Every other bit
// (the *_PROTECTION_FAULT_ENABLE_DEFAULT set hub_setup_vmid_config wrote) is left exactly as found.
constexpr uint32_t cntl_native(uint32_t cur) {
    return (cur & ~(kCntlEnableMask | kCntlDepthMask | kCntlBlockMask | kCntlRetryMask)) |
           (1u << kCntlEnableShift) | (kCtxDepth << kCntlDepthShift) | (kCtxBlockSize << kCntlBlockShift);   // RETRY stays 0
}
// The value we expect on a hub_setup_vmid_config'd context (0x03fffd07 with retry, per its comment) with retry off.
static_assert(cntl_native(0x03fffd07u) == 0x03fffc07u, "DEPTH 3 / BLOCK 0 / RETRY 0 on the upstream default bits");

// ---- virtual addresses ------------------------------------------------------------------------------------------------------
constexpr uint64_t kVaMask48 = (1ull << 48) - 1;
// The kext strips the sign extension before it indexes a table: the walker sees 48 bits.
constexpr uint64_t va_strip(uint64_t va) { return va & kVaMask48; }
// A canonical address has bits 63:47 all equal (all zero: the low half; all one: the high half).
constexpr bool va_canonical(uint64_t va) {
    const uint64_t top = va >> 47;                 // 17 bits
    return top == 0ull || top == 0x1FFFFull;
}
// Sign-extend a 48-bit value from bit 47 (what amdgpu_gmc_sign_extend does).
constexpr uint64_t va_canonicalize(uint64_t va48) {
    va48 &= kVaMask48;
    return (va48 & (1ull << 47)) ? (va48 | ~kVaMask48) : va48;
}
constexpr bool va_high_half(uint64_t va) { return (va_strip(va) >> 47) & 1ull; }

constexpr uint32_t idx_pdb2(uint64_t va) { return (uint32_t)((va_strip(va) >> 39) & 0x1FFull); }   // root: 256..511 = the high half
constexpr uint32_t idx_pdb1(uint64_t va) { return (uint32_t)((va_strip(va) >> 30) & 0x1FFull); }
constexpr uint32_t idx_pdb0(uint64_t va) { return (uint32_t)((va_strip(va) >> 21) & 0x1FFull); }
constexpr uint32_t idx_ptb (uint64_t va) { return (uint32_t)((va_strip(va) >> 12) & 0x1FFull); }
// The page frame the context's START/END registers compare against, and the address a fault register reports (>> 12 again).
constexpr uint64_t va_pfn(uint64_t va) { return va_strip(va) >> 12; }
constexpr bool va_in_context(uint64_t va) { return va_pfn(va) < kMaxPfn; }

// ---- entries ----------------------------------------------------------------------------------------------------------------
constexpr uint64_t kPteValid     = 1ull << 0;
constexpr uint64_t kPteSystem    = 1ull << 1;
constexpr uint64_t kPteSnooped   = 1ull << 2;
constexpr uint64_t kPteExec      = 1ull << 4;
constexpr uint64_t kPteRead      = 1ull << 5;
constexpr uint64_t kPteWrite     = 1ull << 6;
constexpr uint64_t kPteMtypeUC   = 2ull << 54;
constexpr uint64_t kPteIsPte     = 1ull << 63;       // AMDGPU_PDE_PTE_GFX12 / AMDGPU_PTE_IS_PTE
constexpr uint64_t kPtePaMask    = 0x0000FFFFFFFFF000ull;   // pte_addr_mask for GC 12.0.1
constexpr uint64_t kPdeAddrMask  = 0x0000FFFFFFFFF000ull;   // we only place tables on 4 KiB boundaries (the field is 47:6)
// The flags RADV gives every BO (AMDGPU_VM_PAGE_READABLE | WRITEABLE | EXECUTABLE) on local VRAM, as vmfrag_leaf: UC memory type.
constexpr uint64_t kPteFlagsRwx  = kPteExec | kPteRead | kPteWrite | kPteValid | kPteMtypeUC;

// A directory entry pointing at the next level's table: address | VALID and nothing else. `pa` must be 4 KiB aligned and fit the
// address field. Returns 0 (an invalid entry) for a bad address: an unusable pointer must never look like a valid one.
constexpr uint64_t pde_encode(uint64_t pa) {
    return ((pa & ~kPdeAddrMask) != 0ull) ? 0ull : (pa | kPteValid);
}
// A 4 KiB leaf: address | flags | P (bit 63, the vmfrag-proven word).
constexpr uint64_t pte_encode(uint64_t pa, uint64_t flags) {
    return ((pa & ~kPtePaMask) != 0ull) ? 0ull : (pa | (flags & ~kPteIsPte) | kPteIsPte);
}
// A directory entry never grants access: no READ / WRITE / EXEC bit (those belong to leaves).
static_assert((pde_encode(0x1000ull) & (kPteRead | kPteWrite | kPteExec)) == 0ull, "pde_encode never sets READ/WRITE/EXEC");
static_assert((pde_encode(0x0000FFFFFFFFF000ull) & (kPteRead | kPteWrite | kPteExec)) == 0ull, "... at the widest address either");
constexpr bool entry_valid(uint64_t e) { return (e & kPteValid) != 0ull; }
constexpr uint64_t entry_pa(uint64_t e) { return e & kPtePaMask; }
constexpr bool entry_is_leaf(uint64_t e) { return ((e >> 63) & 1ull) != 0ull; }

// ---- the test layout -----------------------------------------------------------------------------------------------------
// Page-table pages inside one VRAM block (byte offsets), and the data pages inside a second block. Two trees share the root:
// the low half hangs off root[0], the high half off root[256].
enum TblPage : uint32_t {
    kTblPdb2 = 0x0000, kTblPdb1Lo = 0x1000, kTblPdb0Lo = 0x2000, kTblPtbLo = 0x3000,
    kTblPdb1Hi = 0x4000, kTblPdb0Hi = 0x5000, kTblPtbHi = 0x6000, kTblBytes = 0x8000,
};
enum DataPage : uint32_t {           // page index inside the data block
    kPgLowDst = 0, kPgLowIb = 1, kPgHighDst = 2, kPgHighIb = 3,
    kPgShader = 4, kPgResult = 5, kPgDispatchIb = 6, kPgFaultIb = 7, kPgMixedIb = 8, kPgCount = 9, kDataBytes = 0x10000,
};
constexpr uint64_t kVaLowBase   = 0x0000000000100000ull;   // pages at +0x1000 * index (every page but the two high ones)
constexpr uint64_t kVaHighBase  = 0xFFFF800000100000ull;   // canonical; stripped = 0x800000100000, root index 256
constexpr uint64_t kVaUnmapped  = 0x0000000200000000ull;   // root[0] is present, PDB1[8] is not: the fault leg's target
constexpr uint64_t page_va(bool high, uint32_t pageIdx) { return (high ? kVaHighBase : kVaLowBase) + 0x1000ull * pageIdx; }
// Page k of the low pages sits at page_va(false, k); of the high pages at page_va(true, k). Which pages are mapped where:
constexpr bool page_is_high(uint32_t pg) { return pg == kPgHighDst || pg == kPgHighIb; }
constexpr uint64_t va_of_page(uint32_t pg) {
    return page_is_high(pg) ? page_va(true, pg - kPgHighDst) : page_va(false, pg);
}
// Every mapped page must land in a table the layout built: the low pages share PTB-low, the high pages PTB-high.
static_assert(idx_pdb2(kVaLowBase) == 0 && idx_pdb2(kVaHighBase) == 256, "low half = root[0], high half = root[256]");
static_assert(idx_pdb1(kVaLowBase) == 0 && idx_pdb0(kVaLowBase) == 0 && idx_ptb(kVaLowBase) == 0x100, "low path");
static_assert(idx_pdb1(kVaHighBase) == 0 && idx_pdb0(kVaHighBase) == 0 && idx_ptb(kVaHighBase) == 0x100, "high path");
static_assert(va_canonical(kVaHighBase) && va_canonical(kVaLowBase) && !va_canonical(0x0000800000000000ull), "canonical forms");
static_assert(va_strip(kVaHighBase) == 0x800000100000ull, "the sign extension is stripped");
static_assert(va_canonicalize(va_strip(kVaHighBase)) == kVaHighBase, "strip and canonicalize are inverses on the high half");
static_assert(idx_pdb2(kVaUnmapped) == 0 && idx_pdb1(kVaUnmapped) == 8 && va_in_context(kVaUnmapped), "the unmapped VA is inside the context");

// ---- the gate ---------------------------------------------------------------------------------------------------------------
// The boot-args that must NOT be armed together with navi48-native=1 (NATIVE-S1.md review MUST-FIX 4): the Apple accelerator
// experiment, the boot chain, the four self-tests (vmfrag rewrites CONTEXT1 with RETRY = 1) and the two hooks that only exist for
// Apple's driver.
struct GateArgs {
    uint32_t native;          // navi48-native
    uint32_t accelExperiment; // navi48-accel-experiment
    uint32_t bootChain;       // navi48-boot-chain
    uint32_t sdmaQ1Test;      // navi48-sdma-q1-test
    uint32_t sdmaQnTest;      // navi48-sdma-qn-test
    uint32_t srbmTest;        // navi48-srbm-test
    uint32_t vmfragTest;      // navi48-vmfrag-test
    uint32_t eopBridge;       // navi48-eop-bridge
    uint32_t shaderCache;     // navi48-shader-cache
};
enum GateBit : uint32_t {
    kConflictAccel = 1u << 0, kConflictBootChain = 1u << 1, kConflictSdmaQ1 = 1u << 2, kConflictSdmaQn = 1u << 3,
    kConflictSrbm = 1u << 4, kConflictVmfrag = 1u << 5, kConflictEopBridge = 1u << 6, kConflictShaderCache = 1u << 7,
};
enum GateState : uint32_t { kGateOff = 0, kGateRefused = 1, kGateOn = 2 };
constexpr uint32_t gate_conflicts(const GateArgs &a) {
    return (a.accelExperiment ? kConflictAccel : 0u) | (a.bootChain ? kConflictBootChain : 0u) |
           (a.sdmaQ1Test ? kConflictSdmaQ1 : 0u) | (a.sdmaQnTest ? kConflictSdmaQn : 0u) |
           (a.srbmTest ? kConflictSrbm : 0u) | (a.vmfragTest ? kConflictVmfrag : 0u) |
           (a.eopBridge ? kConflictEopBridge : 0u) | (a.shaderCache ? kConflictShaderCache : 0u);
}
// OFF when navi48-native is absent or 0, whatever else is set (so a PC boot without the arg is exactly today's boot); REFUSED when
// it is set and any conflicting arg is armed; ON otherwise.
constexpr GateState gate_decide(const GateArgs &a) {
    return a.native == 0u ? kGateOff : (gate_conflicts(a) != 0u ? kGateRefused : kGateOn);
}

// ---- the fault-control check (review MUST-FIX 2) ------------------------------------------------------------------------------
// GCVM_L2_PROTECTION_FAULT_CNTL: CRASH_ON_NO_RETRY_FAULT bit 30, CRASH_ON_RETRY_FAULT bit 31 (amdgpu_field_defs.h). Either set:
// a fault would crash the engine instead of being answered, so the fault leg is refused.
constexpr uint32_t kFaultCntlCrashMask = 0xC0000000u;
constexpr bool fault_leg_allowed(uint32_t l2FaultCntl) { return (l2FaultCntl & kFaultCntlCrashMask) == 0u; }
// CP_DEBUG.CPG_UTCL1_ERROR_HALT_DISABLE (bit 15; gc_12_0_0_sh_mask.h). Upstream's gfxhub set_fault_enable_default sets it so the
// CP does not halt on a page fault; nothing in this kext writes it. LOGGED, and (kRefuseFaultLegIfCpHalts = true, reviewer
// decision) the fault leg is REFUSED while the bit is clear. Nothing here writes CP_DEBUG.
constexpr uint32_t kCpDebugHaltDisableMask = 0x00008000u;
constexpr bool kRefuseFaultLegIfCpHalts = true;
constexpr bool fault_leg_allowed2(uint32_t l2FaultCntl, uint32_t cpDebug) {
    return fault_leg_allowed(l2FaultCntl) &&
           (!kRefuseFaultLegIfCpHalts || (cpDebug & kCpDebugHaltDisableMask) != 0u);
}

// What a fault leg must have produced: a latched status naming OUR vmid, some error bits, and the unmapped page.
// GCVM_L2_PROTECTION_FAULT_STATUS_LO32: WALKER_ERROR [3:1], PERMISSION_FAULTS [7:4], MAPPING_ERROR [8], VMID [23:20].
constexpr uint32_t fault_status_vmid(uint32_t lo)  { return (lo >> 20) & 0xFu; }
constexpr bool fault_status_has_error(uint32_t lo) { return (lo & 0x1FEu) != 0u; }
// ADDR_LO32 / ADDR_HI32 hold the faulting page frame (VA >> 12); the second is 4 bits.
constexpr uint64_t fault_addr_va(uint32_t lo32, uint32_t hi32) { return (((uint64_t)(hi32 & 0xFu) << 32) | lo32) << 12; }
constexpr bool fault_matches(uint32_t statusLo, uint32_t addrLo, uint32_t addrHi, uint32_t vmid, uint64_t va) {
    return fault_status_vmid(statusLo) == vmid && fault_status_has_error(statusLo) &&
           (fault_addr_va(addrLo, addrHi) >> 12) == (va_strip(va) >> 12);
}

// ---- leg sequencing ---------------------------------------------------------------------------------------------------------
// Order: the positive legs first (low WRITE_DATA, high WRITE_DATA incl. an IB fetched from a high-half VA, a mixed IB, the compute
// dispatch), and the fault leg LAST. A latched stop (any bounded wait that times out, or the first leg failing) ends the run:
// every later leg is recorded as SKIP, and the fault leg never starts after a timeout.
enum Leg : uint32_t { kLegLowWrite = 0, kLegHighWrite = 1, kLegMixed = 2, kLegCompute = 3, kLegFault = 4, kLegCount = 5, kLegEnd = 99 };
enum LegResult : uint32_t { kLegPending = 0, kLegPass = 1, kLegFail = 2, kLegSkip = 3, kLegRefused = 4 };
constexpr uint32_t kWaitBoundUs = 2000000u;    // every wait <= 2 s

struct Seq {
    uint32_t  next;                 // index of the next leg to run
    bool      stopped;              // the latched stop
    bool      faultAllowed;         // fault_leg_allowed2 at the time the sequence was opened
    LegResult res[kLegCount];
};
constexpr Seq seq_open(bool faultAllowed) {
    return Seq{ 0u, false, faultAllowed,
                { kLegPending, kLegPending, kLegPending, kLegPending, kLegPending } };
}
// The next leg to run, or kLegEnd. A stopped sequence yields nothing; a refused fault leg is passed over.
constexpr uint32_t seq_next(const Seq &s) {
    return (s.stopped || s.next >= kLegCount) ? (uint32_t)kLegEnd : s.next;
}
// Record a leg's result and advance. A timeout latches the stop. Legs already run stay recorded.
constexpr Seq seq_record(Seq s, Leg leg, LegResult r, bool timedOut) {
    if ((uint32_t)leg < kLegCount) s.res[leg] = r;
    if (timedOut) s.stopped = true;
    if ((uint32_t)leg == s.next) s.next = s.next + 1u;
    return s;
}
// Fault-leg admission: after every positive leg has been RUN (recorded), not stopped, and the fault control allows it.
constexpr bool seq_may_run_fault(const Seq &s) {
    return !s.stopped && s.faultAllowed && s.next == (uint32_t)kLegFault &&
           s.res[kLegLowWrite] != kLegPending && s.res[kLegHighWrite] != kLegPending &&
           s.res[kLegMixed] != kLegPending && s.res[kLegCompute] != kLegPending;
}
// The positive-legs verdict alone (the four legs that need no fault), and the whole verdict (positives AND the fault leg). A refused
// or skipped leg is not a pass.
constexpr bool seq_positive_pass(const Seq &s) {
    return s.res[kLegLowWrite] == kLegPass && s.res[kLegHighWrite] == kLegPass && s.res[kLegMixed] == kLegPass &&
           s.res[kLegCompute] == kLegPass;
}
constexpr const char *seq_fault_word(const Seq &s) {
    return s.res[kLegFault] == kLegPass ? "PASS" : (s.res[kLegFault] == kLegRefused ? "REFUSED" : "FAIL");
}
constexpr bool seq_result_pass(const Seq &s) { return seq_positive_pass(s) && s.res[kLegFault] == kLegPass; }

// ---- the tree, and the driver of the legs -----------------------------------------------------------------------------------
// The whole test tree, entry by entry, through `put(pageOffsetInTableBlock, index, value)`. The kext passes a BAR0 writer; the host
// test passes an array and then WALKS it. Returns false if any entry could not be encoded (an address outside its field).
template <class Put>
inline bool tree_build(uint64_t tblPa, uint64_t dataPa, Put &&put) {
    bool ok = true;
    auto p = [&](uint32_t page, uint32_t idx, uint64_t val) { if (val == 0ull) ok = false; put(page, idx, val); };
    p(kTblPdb2,   idx_pdb2(kVaLowBase),  pde_encode(tblPa + kTblPdb1Lo));
    p(kTblPdb2,   idx_pdb2(kVaHighBase), pde_encode(tblPa + kTblPdb1Hi));
    p(kTblPdb1Lo, idx_pdb1(kVaLowBase),  pde_encode(tblPa + kTblPdb0Lo));
    p(kTblPdb0Lo, idx_pdb0(kVaLowBase),  pde_encode(tblPa + kTblPtbLo));
    p(kTblPdb1Hi, idx_pdb1(kVaHighBase), pde_encode(tblPa + kTblPdb0Hi));
    p(kTblPdb0Hi, idx_pdb0(kVaHighBase), pde_encode(tblPa + kTblPtbHi));
    for (uint32_t pg = 0; pg < kPgCount; pg++)
        p(page_is_high(pg) ? (uint32_t)kTblPtbHi : (uint32_t)kTblPtbLo, idx_ptb(va_of_page(pg)),
          pte_encode(dataPa + (uint64_t)pg * 0x1000u, kPteFlagsRwx));
    return ok;
}

// Runs the legs in order through `r`, which supplies:
//   LegResult run(Leg, bool &timedOut)   execute one leg
//   void refusedFault()                   the fault leg was passed over
// Ordering guarantees (host-tested against a fake runner): the legs run in enum order, each exactly once; the fault leg runs last
// and only when seq_may_run_fault admits it; a timeout latches the stop and nothing runs after it; if the FIRST leg fails without a
// timeout the mapping is unproven, so the stop latches too (there is no polarity fallback).
template <class R>
inline Seq seq_drive(Seq s, R &r) {
    for (uint32_t guard = 0; guard < 16u; guard++) {
        const uint32_t next = seq_next(s);
        if (next == (uint32_t)kLegEnd) break;
        bool timedOut = false;
        LegResult res;
        if (next == (uint32_t)kLegFault && !seq_may_run_fault(s)) {
            res = kLegRefused;
            r.refusedFault();
        } else {
            res = r.run((Leg)next, timedOut);
            if (res == kLegFail && next == (uint32_t)kLegLowWrite && !timedOut) s.stopped = true;
        }
        s = seq_record(s, (Leg)next, res, timedOut);
    }
    return s;
}

} // namespace n48native
