//
//  native_s1b.cpp - native-stack step S1b (kext 0.0.600): the reserved-VMID VM self-test. See native_s1b.h for the shape and
//  native_s1b_pure.h for the arithmetic (host-tested by tests/native_s1b_test.cpp, which drives the same pure functions).
//
//  Runs ONCE, from Navi48Bringup::runStages after the ladder reached stage 17 with the compute test passed, and only when
//  navi48-native=1 was accepted by the gate. It writes exactly: one VRAM block for a 4-level page table, one VRAM block of test
//  pages, and the registers of ONE GFXHUB context (CONTEXT<vmid>: CNTL, PAGE_TABLE_BASE, START, END). Nothing else is written:
//  not MMHUB, not CONTEXT0..7, not CP_DEBUG, and the fault-control register only by the same read-modify-write pulse of its bit 0
//  (CLEAR_PROTECTION_FAULT_STATUS_ADDR) that gmc_clear_vm_faults uses: clear_gfx_faults writes v|1 then v&~1 to
//  GCVM_L2_PROTECTION_FAULT_CNTL, every other bit as found. Every wait is bounded at 2 s and a timeout latches a
//  stop that skips every later leg.
//
#include <string.h>
#include <IOKit/IOLib.h>

#include "native_s1b.h"
#include "amdgpu_log.h"
#include "amdgpu_field_defs.h"
#include "amdgpu_pm4.h"
#include "compute_test.h"

#define NAT_LOG(fmt, ...) AMDGPU_LOG("native-s1b", fmt, ##__VA_ARGS__)

namespace amdgpu {

using namespace n48native;

// The pure header's copies of the register field layouts and PTE bits must be the kext's own.
static_assert(MMVM_CONTEXT1_CNTL__ENABLE_CONTEXT__SHIFT == kCntlEnableShift &&
              MMVM_CONTEXT1_CNTL__ENABLE_CONTEXT_MASK == kCntlEnableMask, "CNTL.ENABLE_CONTEXT");
static_assert(MMVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT == kCntlDepthShift &&
              MMVM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH_MASK == kCntlDepthMask, "CNTL.PAGE_TABLE_DEPTH");
static_assert(MMVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT == kCntlBlockShift &&
              MMVM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE_MASK == kCntlBlockMask, "CNTL.PAGE_TABLE_BLOCK_SIZE");
static_assert(MMVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT__SHIFT == kCntlRetryShift &&
              MMVM_CONTEXT1_CNTL__RETRY_PERMISSION_OR_INVALID_PAGE_FAULT_MASK == kCntlRetryMask, "CNTL.RETRY");
static_assert(MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT_MASK == 0x40000000u &&
              MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT_MASK == 0x80000000u &&
              kFaultCntlCrashMask == (MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_NO_RETRY_FAULT_MASK |
                                      MMVM_L2_PROTECTION_FAULT_CNTL__CRASH_ON_RETRY_FAULT_MASK), "CRASH_ON_* bits");
static_assert(PTEFlags::VALID == kPteValid && PTEFlags::SYSTEM == kPteSystem && PTEFlags::SNOOPED == kPteSnooped &&
              PTEFlags::EXECUTABLE == kPteExec && PTEFlags::READABLE == kPteRead && PTEFlags::WRITEABLE == kPteWrite &&
              PTEFlags::IS_PTE == kPteIsPte && PTEFlags::MTYPE_GFX12_UC == kPteMtypeUC, "PTE bits");

// ---- state (file scope: keeps every frame small) --------------------------------------------------------------------------
static NativeS1bState gN;
static bool           gLatched;        // set when the VM self-test begins to touch the GPU: the Apple verbs and SubmitIB refuse from then on
static bool           gRefusedLogged[2];
static uint32_t       sIb[64];        // staging for one IB's dwords
static uint32_t       sPkt[64];       // the compute builder's output
constexpr uint32_t    kPatWords = 16; // dwords each WRITE_DATA writes and the CPU checks
constexpr uint32_t    kPoison   = 0xDEADBEEEu;   // bit 0 CLEAR: no poison word can ever decode as a VALID page-table entry

struct Env {
    BringupContext *ctx;
    DeviceContext  *dev;
    GMCContext     *gmc;
    const HubContext *h;
    CPContext      *cp;
    VRAMAllocation  tbl, data;
    uint64_t        tblOff, dataOff, tblPa, dataPa;
    uint32_t        vmid;
    Seq             seq;
    uint32_t        cntlReg, baseLoReg, baseHiReg, stLoReg, stHiReg, enLoReg, enHiReg;
};
static Env E;

void native_s1b_set_gate(uint32_t gate, uint32_t conflicts) { gN.gate = gate; gN.conflicts = conflicts; }
const NativeS1bState &native_s1b_state() { return gN; }
bool native_s1b_latched() { return gLatched; }
bool native_s1b_refuse(uint32_t site) {
    if (!gLatched) return false;
    if (site < 2 && !gRefusedLogged[site]) {
        gRefusedLogged[site] = true;
        NAT_LOG("REFUSING %s: the native VM self-test ran on this boot (it left a user VMID programmed and page tables live); "
                "reboot without navi48-native=1 for that path", site == 0 ? "the accel verbs" : "SubmitIB");
    }
    return true;
}

static const char *const kLegName[kLegCount] = { "lowwrite", "highwrite", "mixed", "compute", "fault" };
static const char *result_word(uint32_t r) {
    switch (r) {
    case kLegPass: return "PASS";
    case kLegFail: return "FAIL";
    case kLegSkip: return "SKIP";
    case kLegRefused: return "REFUSED";
    default: return "PENDING";
    }
}

// ---- hardware helpers -------------------------------------------------------------------------------------------------------
static inline uint32_t hub_rd(uint32_t off) { return RREG32(*E.dev, SOC15_REG_OFFSET_BIDX(*E.dev, E.h->ip, E.h->base_idx, off)); }
static inline uint64_t pa_of(uint64_t mc) { return E.gmc->vram_base_offset + mc - E.gmc->vram_start; }

// Pulse CLEAR_PROTECTION_FAULT_STATUS_ADDR (bit 0 of GCVM_L2_PROTECTION_FAULT_CNTL) on the GFXHUB only, exactly the pulse
// gmc_clear_vm_faults uses, so this leg's fault is not an earlier one. (Not MMHUB: the console scans out through it.)
static void clear_gfx_faults() {
    const uint32_t reg = SOC15_REG_OFFSET_BIDX(*E.dev, E.h->ip, E.h->base_idx, E.h->vm_l2_protection_fault_cntl);
    const uint32_t v = RREG32(*E.dev, reg);
    WREG32(*E.dev, reg, v | 1u);
    WREG32(*E.dev, reg, v & ~1u);
}

// One page-table tree in VRAM for the native VMID, (n48native::tree_build makes every entry; the host test walks the
// same tree). Returns false if any entry could not be encoded.
static bool build_tables() {
    DeviceContext &dev = *E.dev;
    bar0_memset_vram(dev, E.tblOff, 0, kTblBytes);
    const bool ok = tree_build(E.tblPa, E.dataPa, [&](uint32_t page, uint32_t idx, uint64_t val) {
        WBAR0_64(dev, E.tblOff + page + (uint64_t)idx * 8u, val);
    });
    amdgpu_hdp_flush(dev);
    NAT_LOG("tables at mc %#llx (pa %#llx): root[0]=%#018llx root[256]=%#018llx | PDB1lo[%u]=%#018llx "
            "PDB0lo[%u]=%#018llx PTBlo[%#x]=%#018llx | PDB1hi[%u]=%#018llx PDB0hi[%u]=%#018llx PTBhi[%#x]=%#018llx",
            (unsigned long long)E.tbl.gpu_va, (unsigned long long)E.tblPa,
            (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPdb2 + 0u * 8u),
            (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPdb2 + 256u * 8u),
            idx_pdb1(kVaLowBase), (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPdb1Lo + (uint64_t)idx_pdb1(kVaLowBase) * 8u),
            idx_pdb0(kVaLowBase), (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPdb0Lo + (uint64_t)idx_pdb0(kVaLowBase) * 8u),
            idx_ptb(kVaLowBase), (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPtbLo + (uint64_t)idx_ptb(kVaLowBase) * 8u),
            idx_pdb1(kVaHighBase), (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPdb1Hi + (uint64_t)idx_pdb1(kVaHighBase) * 8u),
            idx_pdb0(kVaHighBase), (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPdb0Hi + (uint64_t)idx_pdb0(kVaHighBase) * 8u),
            idx_ptb(kVaHighBase), (unsigned long long)RBAR0_64(dev, E.tblOff + kTblPtbHi + (uint64_t)idx_ptb(kVaHighBase) * 8u));
    return ok;
}

// Stage `n` dwords into a data page through BAR0 and read them back (the CPU's view), so a wrong IB is never a mystery.
static bool stage_page(uint32_t pg, const uint32_t *dw, uint32_t n) {
    DeviceContext &dev = *E.dev;
    const uint64_t off = E.dataOff + (uint64_t)pg * 0x1000u;
    bar0_memcpy_to_vram(dev, off, dw, n * 4u);
    amdgpu_hdp_flush(dev);
    for (uint32_t i = 0; i < n; i++)
        if (RBAR0_32(dev, off + (uint64_t)i * 4u) != dw[i]) return false;
    return true;
}
static void poison_page_range(uint32_t pg, uint32_t byteOff, uint32_t bytes) {
    bar0_memset_vram(*E.dev, E.dataOff + (uint64_t)pg * 0x1000u + byteOff, kPoison, bytes);
    amdgpu_hdp_flush(*E.dev);
}

// One WRITE_DATA (ME, memory destination, WR_CONFIRM) of `n` dwords of data[i] = seed ^ i to `va`, appended at sIb[at]. The
// count field is the payload dword count minus one: control + address lo/hi + n data = 3 + n, so 2 + n.
static uint32_t emit_write_data(uint32_t at, uint64_t va, uint32_t n, uint32_t seed) {
    sIb[at++] = pm4_header(kPM4OpWriteData, 2u + n);
    sIb[at++] = pm4_write_data_control(kPM4WriteDataEngineME, kPM4WriteDataDstSelMemory, true);
    sIb[at++] = (uint32_t)(va & 0xFFFFFFFFu);
    sIb[at++] = (uint32_t)(va >> 32);
    for (uint32_t i = 0; i < n; i++) sIb[at++] = seed ^ i;
    return at;
}

// Submit the IB at `ibVa` under the native VMID and wait (<= 2 s) for its fence. *timedOut is the latched-stop trigger.
static bool submit_wait(uint64_t ibVa, uint32_t ndw, const char *what, bool *timedOut, uint64_t *elapsedUs) {
    uint32_t fence = 0;
    kern_return_t kr = cp_submit_ib(*E.dev, *E.cp, ibVa, ndw, E.vmid, &fence);
    if (kr != kIOReturnSuccess) { NAT_LOG("%s: cp_submit_ib refused (kr=%#x)", what, kr); return false; }
    uint64_t obs = 0, el = 0;
    kr = cp_wait_fence(*E.cp, fence, kWaitBoundUs, &obs, &el);
    if (elapsedUs) *elapsedUs = el;
    if (kr != kIOReturnSuccess) {
        if (timedOut) *timedOut = true;
        NAT_LOG("%s: fence %u did NOT land in %llu us (slot holds %#llx)", what, fence, (unsigned long long)el,
                (unsigned long long)obs);
        return false;
    }
    return true;
}

static uint32_t count_wrong(uint32_t pg, uint32_t byteOff, uint32_t n, uint32_t seed) {
    uint32_t bad = 0;
    const uint64_t off = E.dataOff + (uint64_t)pg * 0x1000u + byteOff;
    for (uint32_t i = 0; i < n; i++)
        if (RBAR0_32(*E.dev, off + (uint64_t)i * 4u) != (seed ^ i)) bad++;
    return bad;
}

// ---- the legs ---------------------------------------------------------------------------------------------------------------
struct Dst { uint32_t pg; uint32_t off; uint32_t seed; };

// A WRITE_DATA leg: the IB lives in page `ibPg` (its VA is that page's mapping, high or low), and writes 16 dwords to each
// destination (VA = the destination page's mapping + off). The CPU checks every dword.
static LegResult leg_write(uint32_t leg, uint32_t ibPg, const Dst *d, uint32_t nd, bool *timedOut, uint32_t *bad) {
    *bad = 0;
    for (uint32_t k = 0; k < nd; k++) poison_page_range(d[k].pg, d[k].off, kPatWords * 4u);
    uint32_t at = 0;
    for (uint32_t k = 0; k < nd; k++) at = emit_write_data(at, va_of_page(d[k].pg) + d[k].off, kPatWords, d[k].seed);
    const uint64_t ibVa = va_of_page(ibPg);
    if (!stage_page(ibPg, sIb, at)) { NAT_LOG("leg %s FAIL the IB did not stage into VRAM", kLegName[leg]); return kLegFail; }
    clear_gfx_faults();
    uint64_t us = 0;
    const bool landed = submit_wait(ibVa, at, kLegName[leg], timedOut, &us);
    for (uint32_t k = 0; k < nd; k++) *bad += count_wrong(d[k].pg, d[k].off, kPatWords, d[k].seed);
    const uint32_t sLo = hub_rd(0x15d0), aLo = hub_rd(0x15d2);
    const bool pass = landed && *bad == 0u;
    NAT_LOG("leg %s %s IB VA %#llx (%s half, fetched under VMID %u) -> %u destination(s), %u of %u dwords wrong, fence %s in %llu us",
            kLegName[leg], pass ? "PASS" : "FAIL", (unsigned long long)ibVa, va_high_half(ibVa) ? "high" : "low", E.vmid,
            nd, *bad, nd * kPatWords, landed ? "landed" : "MISSING", (unsigned long long)us);
    NAT_LOG("leg %s fault registers after the leg: STATUS_LO32 %#010x (vmid %u walker %u) ADDR_LO %#010x",
            kLegName[leg], sLo, fault_status_vmid(sLo), (sLo >> 1) & 7u, aLo);
    return pass ? kLegPass : kLegFail;
}

// The compute leg: shader, result page and the dispatch IB all behind the native VMID's tables (shader fetch + store).
static LegResult leg_compute(bool *timedOut, uint32_t *bad) {
    DeviceContext &dev = *E.dev;
    *bad = kComputeThreadsX;
    uint32_t codeBytes = 0;
    const uint8_t *code = compute_store_magic_code(&codeBytes);
    if (code == nullptr || codeBytes == 0 || codeBytes > 0x800u) {
        NAT_LOG("leg compute FAIL the embedded shader blob is %u bytes", codeBytes);
        return kLegFail;
    }
    // The shader page: s_endpgm filled (a fetch past the blob ends the wave), then the blob.
    const uint64_t shOff = E.dataOff + (uint64_t)kPgShader * 0x1000u;
    bar0_memset_vram(dev, shOff, 0xBFB00000u, 0x1000u);
    bar0_memcpy_to_vram(dev, shOff, code, codeBytes);
    poison_page_range(kPgResult, 0, 0x1000u);
    amdgpu_hdp_flush(dev);
    const uint64_t codeVa = va_of_page(kPgShader), resVa = va_of_page(kPgResult), ibVa = va_of_page(kPgDispatchIb);
    const uint32_t n = compute_build_va_dispatch_ib(dev, sPkt, 64, codeVa, resVa);
    if (n == 0) { NAT_LOG("leg compute FAIL the dispatch builder refused"); return kLegFail; }
    if (!stage_page(kPgDispatchIb, sPkt, n)) { NAT_LOG("leg compute FAIL the dispatch IB did not stage"); return kLegFail; }
    clear_gfx_faults();
    uint64_t us = 0;
    const bool landed = submit_wait(ibVa, n, "compute", timedOut, &us);
    uint32_t wrong = 0;
    for (uint32_t i = 0; i < kComputeThreadsX; i++)
        if (RBAR0_32(dev, E.dataOff + (uint64_t)kPgResult * 0x1000u + (uint64_t)i * 4u) != kComputeMagic + i) wrong++;
    *bad = wrong;
    const uint32_t sLo = hub_rd(0x15d0), aLo = hub_rd(0x15d2);
    const bool pass = landed && wrong == 0u;
    NAT_LOG("leg compute %s dispatch IB VA %#llx, shader VA %#llx (%u bytes), result VA %#llx: %u of %u lanes wrong, fence %s in %llu us",
            pass ? "PASS" : "FAIL", (unsigned long long)ibVa, (unsigned long long)codeVa, codeBytes, (unsigned long long)resVa,
            wrong, kComputeThreadsX, landed ? "landed" : "MISSING", (unsigned long long)us);
    NAT_LOG("leg compute result[0] %#010x (want %#010x); fault registers: STATUS_LO32 %#010x (vmid %u) ADDR_LO %#010x",
            RBAR0_32(dev, E.dataOff + (uint64_t)kPgResult * 0x1000u), kComputeMagic, sLo, fault_status_vmid(sLo), aLo);
    return pass ? kLegPass : kLegFail;
}

// The fault leg: one WRITE_DATA to an unmapped VA (root present, PDB1[8] absent). It must latch a fault naming OUR VMID and the
// unmapped page, and the NEXT fence (an EOP-only submission on the ring) must still land: the ring survived the fault.
static LegResult leg_fault(bool *timedOut) {
    DeviceContext &dev = *E.dev;
    uint32_t at = emit_write_data(0, kVaUnmapped, 1, 0xFA017000u);
    const uint64_t ibVa = va_of_page(kPgFaultIb);
    if (!stage_page(kPgFaultIb, sIb, at)) { NAT_LOG("leg fault FAIL the IB did not stage"); return kLegFail; }
    clear_gfx_faults();
    uint64_t us1 = 0;
    const bool f1 = submit_wait(ibVa, at, "fault", timedOut, &us1);
    const uint32_t sLo = hub_rd(0x15d0), sHi = hub_rd(0x15d1), aLo = hub_rd(0x15d2), aHi = hub_rd(0x15d3);
    const bool seen = fault_matches(sLo, aLo, aHi, E.vmid, kVaUnmapped);
    gN.faultStatusLo = sLo;
    gN.faultVa = fault_addr_va(aLo, aHi);
    uint32_t f2fence = 0;
    const kern_return_t k2 = cp_submit_eop_test(dev, *E.cp, kWaitBoundUs, &f2fence);
    const bool f2 = (k2 == kIOReturnSuccess);
    if (!f2) *timedOut = true;
    gN.nextFenceLanded = f2;
    const bool pass = seen && f2;
    NAT_LOG("leg fault %s WRITE_DATA to unmapped VA %#llx under VMID %u: %s; NEXT fence %s (kr=%#x)",
            pass ? "PASS" : "FAIL", (unsigned long long)kVaUnmapped, E.vmid,
            seen ? "the latched fault is the one we asked for" : "NO matching fault latched",
            f2 ? "LANDED (the ring survived)" : "did NOT land", k2);
    NAT_LOG("leg fault own fence %s (%llu us); latched STATUS_LO32 %#010x HI32 %#010x (vmid %u walker %u perm %#x mapping %u) fault VA %#llx",
            f1 ? "landed" : "MISSING", (unsigned long long)us1, sLo, sHi, fault_status_vmid(sLo), (sLo >> 1) & 7u, (sLo >> 4) & 0xFu,
            (sLo >> 8) & 1u, (unsigned long long)fault_addr_va(aLo, aHi));
    return pass ? kLegPass : kLegFail;
}

// ---- the run ----------------------------------------------------------------------------------------------------------------
static void skip_rest(Seq &s, const char *why) {
    for (uint32_t i = 0; i < kLegCount; i++) {
        if (s.res[i] == kLegPending) {
            s.res[i] = kLegSkip;
            NAT_LOG("leg %s SKIP (%s)", kLegName[i], why);
        }
    }
}

// What seq_drive calls. Seeds differ per leg so a stale line can never pass one.
struct LegRunner {
    BringupContext *ctx;
    const HubContext &h;
    uint32_t attempt { 0 };
    LegResult run(Leg leg, bool &timedOut) {
        const uint32_t seed = 0x53314200u ^ (attempt++ << 20);   // 'S1B'
        uint32_t bad = 0;
        LegResult r = kLegFail;
        switch (leg) {
        case kLegLowWrite: {
            const Dst d[1] = { { kPgLowDst, 0x000u, seed ^ 0x1000u } };
            r = leg_write(kLegLowWrite, kPgLowIb, d, 1, &timedOut, &bad);
            break;
        }
        case kLegHighWrite: {
            const Dst d[1] = { { kPgHighDst, 0x000u, seed ^ 0x2000u } };
            r = leg_write(kLegHighWrite, kPgHighIb, d, 1, &timedOut, &bad);     // the IB is fetched from a high-half VA
            break;
        }
        case kLegMixed: {
            const Dst d[2] = { { kPgHighDst, 0x100u, seed ^ 0x3000u }, { kPgLowDst, 0x100u, seed ^ 0x3100u } };
            r = leg_write(kLegMixed, kPgMixedIb, d, 2, &timedOut, &bad);        // a low-half IB writing to a high and a low VA
            break;
        }
        case kLegCompute:
            r = leg_compute(&timedOut, &bad);
            break;
        case kLegFault:
            r = leg_fault(&timedOut);
            break;
        default:
            break;
        }
        gN.legBad[leg] = bad;
        if (timedOut) NAT_LOG("a bounded wait timed out on leg %s - the stop is LATCHED, no later leg will run", kLegName[leg]);
        if (r == kLegFail && leg == kLegLowWrite && !timedOut)
            NAT_LOG("leg lowwrite failed: the mapping is unproven, so every later leg is skipped");
        return r;
    }
    void refusedFault() {
        NAT_LOG("leg fault REFUSED (%s)", !gN.faultAllowed ? ((gN.l2FaultCntl & kFaultCntlCrashMask) ?
                "GCVM_L2_PROTECTION_FAULT_CNTL has a CRASH_ON_* bit set" :
                "CP_DEBUG.CPG_UTCL1_ERROR_HALT_DISABLE is clear: the CP may halt on a page fault") :
                "a positive leg did not run or the run is stopped");
    }
};

kern_return_t native_s1b_run(BringupContext &ctx)
{
    gN.ran = false; gN.resultPass = false;
    DeviceContext *dev = ctx.dev;
    if (gN.gate != kGateOn) return kIOReturnNotPermitted;    // the caller checks too; this is the belt
    if (dev == nullptr || ctx.reached != BringupStage::ComputeDispatch || !ctx.computePassed || !ctx.cp.inited) {
        NAT_LOG("NOT RUN: the ladder must reach stage 17 with the compute test passed (reached %u, computePassed %d, cp %d)",
                (unsigned)ctx.reached, (int)ctx.computePassed, (int)ctx.cp.inited);
        NAT_LOG("RESULT FAIL (not run)");
        return kIOReturnNotReady;
    }
    E = Env {};
    E.ctx = &ctx; E.dev = dev; E.gmc = &ctx.gmc; E.h = &ctx.gmc.gfxhub; E.cp = &ctx.cp; E.vmid = kNativeVmid;
    const HubContext &h = *E.h;
    if (!h.inited || !dev->ip.isResolved(h.ip) || !ctx.gmc.vram_alloc.is_inited() || dev->bar0 == nullptr || !vmid_ok(E.vmid)) {
        NAT_LOG("NOT RUN: GFXHUB (inited %d), the VRAM allocator or BAR0 is not ready, or vmid %u is not allowed", (int)h.inited,
                E.vmid);
        NAT_LOG("RESULT FAIL (not run)");
        return kIOReturnNotReady;
    }
    gN.vmid = E.vmid;

    // (3) The fault control, read and logged BEFORE any fault leg.
    gN.l2FaultCntl = RREG32(*dev, SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.vm_l2_protection_fault_cntl));
    gN.cpDebug = RREG32(*dev, SOC15_REG_OFFSET_BIDX(*dev, IPBlock::GC, 0, 0x1e1f));
    gN.faultAllowed = fault_leg_allowed2(gN.l2FaultCntl, gN.cpDebug);
    NAT_LOG("GCVM_L2_PROTECTION_FAULT_CNTL = %#010x (CRASH_ON_NO_RETRY_FAULT %u, CRASH_ON_RETRY_FAULT %u) -> fault leg %s; "
            "CP_DEBUG = %#010x (CPG_UTCL1_ERROR_HALT_DISABLE %u: %s; this kext does not write it)",
            gN.l2FaultCntl, (gN.l2FaultCntl >> 30) & 1u, (gN.l2FaultCntl >> 31) & 1u,
            gN.faultAllowed ? "ALLOWED" : "REFUSED (a CRASH_ON_* bit is set, or CP_DEBUG halt-disable is clear)", gN.cpDebug, (gN.cpDebug >> 15) & 1u,
            (gN.cpDebug & kCpDebugHaltDisableMask) ? "the CP will not halt on a page fault"
                                                   : "the CP MAY halt on a page fault: a fence that never lands would mean that");

    // VRAM for the tables and the test pages, inside the BAR0 window (every entry and every check is a BAR0 access).
    if (!ctx.gmc.vram_alloc.alloc(kTblBytes, 0x1000u, &E.tbl) || !ctx.gmc.vram_alloc.alloc(kDataBytes, 0x1000u, &E.data)) {
        NAT_LOG("NOT RUN: no VRAM for the page tables / test pages");
        if (E.tbl.size) ctx.gmc.vram_alloc.free(E.tbl);
        NAT_LOG("RESULT FAIL (not run)");
        return kIOReturnNoMemory;
    }
    E.tblOff = E.tbl.gpu_va - ctx.gmc.vram_start;
    E.dataOff = E.data.gpu_va - ctx.gmc.vram_start;
    E.tblPa = pa_of(E.tbl.gpu_va);
    E.dataPa = pa_of(E.data.gpu_va);
    if (E.tblOff + kTblBytes > dev->bar0Size || E.dataOff + kDataBytes > dev->bar0Size ||
        (E.tblPa & 0xFFFull) != 0 || (E.dataPa & 0xFFFull) != 0) {
        NAT_LOG("NOT RUN: table vram+%#llx / data vram+%#llx outside BAR0 (%llu) or not 4 KiB aligned",
                (unsigned long long)E.tblOff, (unsigned long long)E.dataOff, (unsigned long long)dev->bar0Size);
        ctx.gmc.vram_alloc.free(E.data); ctx.gmc.vram_alloc.free(E.tbl);
        NAT_LOG("RESULT FAIL (not run)");
        return kIOReturnNoMemory;
    }
    gN.tableMc = E.tbl.gpu_va; gN.dataMc = E.data.gpu_va;
    gN.ran = true;
    E.seq = seq_open(gN.faultAllowed);
    Seq &s = E.seq;

    // (2) The native VMID's context: upstream geometry, RETRY = 0, this VMID only.
    E.cntlReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx1_cntl) + (E.vmid - 1u) * h.ctx_distance;
    E.baseLoReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx0_pt_base_lo) + E.vmid * h.ctx_addr_distance;
    E.baseHiReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx0_pt_base_hi) + E.vmid * h.ctx_addr_distance;
    E.stLoReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx1_pt_start_lo) + (E.vmid - 1u) * h.ctx_addr_distance;
    E.stHiReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx1_pt_start_hi) + (E.vmid - 1u) * h.ctx_addr_distance;
    E.enLoReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx1_pt_end_lo) + (E.vmid - 1u) * h.ctx_addr_distance;
    E.enHiReg = SOC15_REG_OFFSET_BIDX(*dev, h.ip, h.base_idx, h.ctx1_pt_end_hi) + (E.vmid - 1u) * h.ctx_addr_distance;

    gLatched = true;   // from here the GPU is being touched under a user VMID: the Apple verbs and SubmitIB refuse
    // MUST-FIX 1: zero the WHOLE data block through BAR0 before any table is built, so no stale VRAM word (a previous boot's
    // content survives a warm reboot) is ever read as a page-table entry or a shader.
    bar0_memset_vram(*dev, E.dataOff, 0, kDataBytes);
    amdgpu_hdp_flush(*dev);
    if (!build_tables()) {
        NAT_LOG("tables: an entry could not be encoded (address outside the field) - stopping before touching the context");
        s.stopped = true;
    } else {
        gN.cntlBefore = RREG32(*dev, E.cntlReg);
        WREG32(*dev, E.cntlReg, cntl_native(gN.cntlBefore));
        const uint64_t root = (E.tblPa + kTblPdb2) | kPteValid;   // base | VALID, as vmfrag and gfxhub_v12_0_setup_vm_pt_regs
        WREG32(*dev, E.baseLoReg, (uint32_t)(root & 0xFFFFFFFFu));
        WREG32(*dev, E.baseHiReg, (uint32_t)(root >> 32));
        WREG32(*dev, E.stLoReg, 0u);
        WREG32(*dev, E.stHiReg, 0u);
        WREG32(*dev, E.enLoReg, (uint32_t)((kMaxPfn - 1u) & 0xFFFFFFFFu));
        WREG32(*dev, E.enHiReg, (uint32_t)((kMaxPfn - 1u) >> 32));
        gN.cntlAfter = RREG32(*dev, E.cntlReg);
        (void)gmc_flush_gpu_tlb(*dev, ctx.gmc, h, E.vmid, /*type*/ 0);
        NAT_LOG("GFXHUB CONTEXT%u (VMID %u, outside MES mask %#x): CNTL@%#x %#010x -> %#010x (want %#010x = DEPTH 3 / BLOCK 0 / RETRY 0 on "
                "the hub_setup_vmid_config bits; RETRY reads %u) BASE %#010x:%#010x START %#010x END %#010x:%#010x; every other context untouched",
                E.vmid, E.vmid, kMesVmidMaskGfxhub, E.cntlReg, gN.cntlBefore, gN.cntlAfter, cntl_native(gN.cntlBefore),
                (gN.cntlAfter & kCntlRetryMask) ? 1u : 0u, RREG32(*dev, E.baseHiReg), RREG32(*dev, E.baseLoReg),
                RREG32(*dev, E.stLoReg), RREG32(*dev, E.enHiReg), RREG32(*dev, E.enLoReg));
        if ((gN.cntlAfter & kCntlRetryMask) != 0u || ((gN.cntlAfter & kCntlDepthMask) >> kCntlDepthShift) != kCtxDepth) {
            NAT_LOG("the context did not take the native geometry (CNTL readback %#010x) - stopping: a fault leg under RETRY=1 could storm",
                    gN.cntlAfter);
            s.stopped = true;
        }
    }

    // (4) The legs, in the order n48native::seq_drive enforces (the same driver the host test runs against a fake).
    LegRunner runner { &ctx, h };
    s = seq_drive(s, runner);
    if (s.stopped) skip_rest(s, "latched stop");
    skip_rest(s, "not reached");

    for (uint32_t i = 0; i < kLegCount; i++) gN.leg[i] = (uint32_t)s.res[i];
    gN.stopped = s.stopped;
    gN.resultPass = seq_result_pass(s);
    NAT_LOG("the tables and test pages (table mc %#llx, data mc %#llx) stay allocated for S1c",
            (unsigned long long)E.tbl.gpu_va, (unsigned long long)E.data.gpu_va);
    gN.positivePass = seq_positive_pass(s);
    gN.faultWord = seq_fault_word(s);
    NAT_LOG("POSITIVE %s (fault leg %s)", gN.positivePass ? "PASS" : "FAIL", gN.faultWord);
    NAT_LOG("RESULT %s", gN.resultPass ? "PASS" : "FAIL");
    return gN.resultPass ? kIOReturnSuccess : kIOReturnError;
}

uint32_t native_s1b_format(char *buf, uint32_t n)
{
    if (buf == nullptr || n == 0) return 0;
    if (gN.gate == kGateRefused) {
        return (uint32_t)snprintf(buf, n, "REFUSED: navi48-native=1 with conflicting boot-args (mask %#x); nothing native ran", gN.conflicts);
    }
    if (!gN.ran) {
        return (uint32_t)snprintf(buf, n, "%s", gN.gate == kGateOn ? "NOT RUN (see the native-s1b lines)" : "OFF");
    }
    return (uint32_t)snprintf(buf, n,
        "POSITIVE %s (fault leg %s); RESULT %s; vmid %u; CNTL %#010x; legs: %s %s (%u bad), %s %s (%u bad), %s %s (%u bad), %s %s (%u bad), %s %s; "
        "fault STATUS_LO32 %#010x va %#llx next-fence %s; L2_FAULT_CNTL %#010x CP_DEBUG %#010x%s",
        gN.positivePass ? "PASS" : "FAIL", gN.faultWord, gN.resultPass ? "PASS" : "FAIL", gN.vmid, gN.cntlAfter,
        kLegName[0], result_word(gN.leg[0]), gN.legBad[0], kLegName[1], result_word(gN.leg[1]), gN.legBad[1],
        kLegName[2], result_word(gN.leg[2]), gN.legBad[2], kLegName[3], result_word(gN.leg[3]), gN.legBad[3],
        kLegName[4], result_word(gN.leg[4]), gN.faultStatusLo, (unsigned long long)gN.faultVa,
        gN.nextFenceLanded ? "landed" : "no", gN.l2FaultCntl, gN.cpDebug, gN.stopped ? "; STOP LATCHED" : "");
}

} // namespace amdgpu
