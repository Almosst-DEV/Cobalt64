//
//  native_s1c.cpp - native-stack step S1c (kext 0.0.601): the engine behind Navi48NativeClient. See native_s1c.h for the shape and
//  native_s1c_pure.h for the arithmetic (host-tested by tests/native_s1c_test.cpp, which drives the same pure functions).
//
//  What this file WRITES (and nothing else): VRAM pages it allocated from the two VRAM pools (through BAR0 for the visible pool); the
//  native VMID's own registers CONTEXT8 BASE_LO/HI + START/END (open, and the park at close); GART PTEs inside its own 16-page fence
//  window; system memory it allocated; the kernel GFX ring (the KGQ) and its wptr shadow / doorbell; the native seqno slot in the CP
//  write-back page (kCPWBOffsetNativeSeq, zeroed at open). It writes no other hardware register, and the TLB flushes are the existing
//  gmc_flush_gpu_tlb. It never calls native_s1b_refuse.
//
//  0.0.612 (ABI 1.9): BoImportHost (n1c_bo_import_host) wires a page-aligned range of the CALLER's address space and maps its scattered physical pages into VMID 8 as SYSTEM|SNOOPED
//  entries (the GTT leaf flags). The only new writes: the VMID-8 PTEs of such a BO (in the client's own tree) and, from the HUNG latch, the "Navi48,Ready" property on the Metal nub.
//
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>

#include "native_s1c.h"
#include "native_hostimport_pure.h"   // 0.0.612 (ABI 1.9): BoImportHost
#include "Navi48MetalNub.hpp"        // 0.0.612: hungLatched() writes Navi48,Ready = 0
#include <IOKit/IOMemoryDescriptor.h>
#include "dcn/navi48_dcn.hpp"     // build 0.0.603: n48dcn::scan* (the scanout path behind selectors 9..14)
#include "dcn/navi48_scanout_pure.h"
#include "smu_dal.h"          // build 0.0.604: dal_run_step (selector 15, the DISPCLK/DPPCLK experiment)
#include "amdgpu_log.h"
#include "amdgpu_field_defs.h"
#include "amdgpu_gfx.h"
#include "amdgpu_pm4.h"
#include "cp_pm4_gfx12.h"

#define N1C_LOG(fmt, ...) AMDGPU_LOG("native-s1c", fmt, ##__VA_ARGS__)

namespace n48 { uint64_t hw_hook_ramtop_derived_or_zero(); }   // apple/AppleHardwareHook.cpp (0.0.612 review item A): the EFI-map RAM top, 0 = not read cleanly

namespace amdgpu {

using namespace n48native;
using namespace n48native::s1c;

// The pure header's codes are the kernel's.
static_assert(kOk == (uint32_t)kIOReturnSuccess && kBadArg == (uint32_t)kIOReturnBadArgument && kNotFound == (uint32_t)kIOReturnNotFound &&
              kUnsupported == (uint32_t)kIOReturnUnsupported && kNoMemory == (uint32_t)kIOReturnNoMemory &&
              kNoResources == (uint32_t)kIOReturnNoResources && kNotReady == (uint32_t)kIOReturnNotReady &&
              kExclusive == (uint32_t)kIOReturnExclusiveAccess && kNotPrivileged == (uint32_t)kIOReturnNotPrivileged &&
              kNotPermitted == (uint32_t)kIOReturnNotPermitted && kTimeout == (uint32_t)kIOReturnTimeout &&
              kAborted == (uint32_t)kIOReturnAborted, "the contract's return codes are the IOReturn.h values");
static_assert(kNativeVmid == 8u, "S1c runs on VMID 8");
static_assert(n48scan::kOk == (uint32_t)kIOReturnSuccess && n48scan::kBadArg == (uint32_t)kIOReturnBadArgument && n48scan::kNotFound == (uint32_t)kIOReturnNotFound &&
              n48scan::kUnsupported == (uint32_t)kIOReturnUnsupported && n48scan::kNoResources == (uint32_t)kIOReturnNoResources &&
              n48scan::kNotReady == (uint32_t)kIOReturnNotReady && n48scan::kBusy == (uint32_t)kIOReturnBusy &&
              n48scan::kNotPermitted == (uint32_t)kIOReturnNotPermitted && n48scan::kNoDevice == (uint32_t)kIOReturnNoDevice,
              "the scanout pure header's codes are the IOReturn.h values");

// ---- state --------------------------------------------------------------------------------------------------------------------
enum : uint8_t { kBoNone = 0, kBoVis = 1, kBoHi = 2, kBoGtt = 3, kBoHost = 4 };   // kBoHost: 0.0.612, an import of the caller's memory (hmd / hpages)
struct Bo {
    uint8_t   kind;
    bool      uc;          // created UNCACHED
    uint64_t  size;        // 4 KiB rounded (what the caller asked for)
    uint64_t  mc;          // VRAM MC address (vis / hi)
    VRAMAllocation a;      // vis / hi
    uint8_t   pinMask;     // 0.0.603: bit i = this BO hosts scanout slot i (ScanoutRegister). Set and cleared ONLY under gCliLock; a set bit whose pinGen is older than n48dcn::scanGeneration() is STALE (its acquisition ended)
    uint32_t  pinGen;      // the scan generation the pins were recorded under
    uint8_t   pinLeak;     // the console restore did NOT verify while this BO was pinned: it may still be scanned, so it is LEAKED (like HUNG), never freed
    SysMem    sm;          // gtt
    IODeviceMemory *vmd;   // vis: the ONE CPU-map descriptor of this BO (0.0.602), created on first clientMemoryForType, released at BoFree / close, leaked with the BO when HUNG
    IOMemoryDescriptor *hmd;   // host (0.0.612): the prepare()d descriptor of the caller's range; complete() + release() at BoFree / close, LEAKED when HUNG
    uint64_t *hpages;          // host: the physical page list (heap, hpageCount entries), freed with the descriptor
    uint32_t  hpageCount;
};
constexpr uint32_t kArenaBlocks = (uint32_t)(kPtCap / 16384ull);   // 2048 blocks of 16 KiB (the pool's minimum allocation)
constexpr uint32_t kArenaBlockBytes = 16384u;
struct Session {
    bool      hello;
    uint8_t   boUsed[N48N_MAX_BOS];
    Bo        bo[N48N_MAX_BOS];
    VaEnt     maps[kMaxMaps];
    uint8_t   ctxUsed[N48N_MAX_CTX + 1];
    FenceSlot fslot[N48N_FENCE_SLOTS];
    uint16_t  blk[kArenaBlocks];   // indices into gPt (the per-boot page-table reserve)
    uint32_t  nblk;
    uint32_t  pagesInBlk;      // 4 KiB pages handed out from the newest block (4 = full)
    uint32_t  ptPages;         // table pages handed out (the root included)
    uint64_t  rootPa;
    uint64_t  gttUsed;
    uint64_t  vramBytes;       // vis + hi bytes held
    uint64_t  importedBytes;   // 0.0.612: host-import bytes held (cap kImportCap, separate from the GTT cap)
    uint32_t  nBos;
    uint64_t  emitted;         // seqno of the last Submit; atomics
};
static Session gSess;          // STATIC and never freed: a selector racing a close can never touch freed memory
static Session *gS;            // &gSess while a client is open (set/cleared under the client lock), else null
// The per-boot page-table reserve: blocks taken from vram_alloc for page tables are NEVER returned to it (a user CPU mapping kept
// after BoFree can therefore never alias a page table); a closing client returns its blocks to this list and the next client reuses them.
static VRAMAllocation gPt[kArenaBlocks];
static uint16_t gPtFree[kArenaBlocks];
static uint32_t gPtN, gPtFreeN;
// The native write counter: 64 bits, monotonically increasing, initialised from cp.wptr at the first native open. `& mask` is only the ring
// index and the space check; the FULL value goes to the wptr shadow and the doorbell (upstream gfx12 support_64bit_ptrs).
static uint64_t gWc;
static bool gWcInit;
static BringupContext *gCtx;
static volatile UInt32 gOpenFlag;   // 1 while a client is open (OSCompareAndSwap)
static Hang gHang;             // boot-wide: hung survives the client
static IOLock *gCliLock, *gRingLock, *gHangLock;
// 0.0.609: the identity of the open native session (1, 2, 3 ... per open; 0 = none). Written under gCliLock at open and cleared at close; the HELD mode (row 120) names its owner by it.
static uint32_t gSessSeqCounter = 0, gSessSeq = 0;
// 0.0.612 (review item A): this GPU's PCI BARs (the FULL sizes), latched once at start by n1c_latch_pci_bars. gBarsState: 0 = not latched, 1 = latched. Read without a lock after the acquire load.
static PciBar gBars[kMaxPciBars];
static uint32_t gNBars;
static volatile UInt32 gBarsState;
static bool gLegacyLogged[8];
static uint32_t gSubmitBuf[280];   // one Submit's dwords; used only under the client lock

struct FenceWindow { bool reserved; uint64_t gartOff; uint64_t mc; };
static FenceWindow gWin;
struct ParkRoot { bool have; VRAMAllocation a; uint64_t pa; };
static ParkRoot gPark;

// ---- helpers ------------------------------------------------------------------------------------------------------------------
static IOLock *lazy_lock(IOLock **slot) {
    if (*slot == nullptr) {
        IOLock *l = IOLockAlloc();
        if (l != nullptr && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)slot)) IOLockFree(l);
    }
    return *slot;
}
static uint64_t now_ns() {
    uint64_t t = 0, ns = 0;
    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &ns);
    return ns;
}
static inline uint64_t pa_of_mc(uint64_t mc) { return gCtx->gmc.vram_base_offset + mc - gCtx->gmc.vram_start; }
static inline volatile uint64_t *seq_slot() { return reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(gCtx->cp.wb_cpu) + kWbOffsetNativeSeq); }
static inline uint32_t hub_rd(uint32_t off) {
    const HubContext &h = gCtx->gmc.gfxhub;
    return RREG32(*gCtx->dev, SOC15_REG_OFFSET_BIDX(*gCtx->dev, h.ip, h.base_idx, off));
}
static inline uint64_t emitted_now() { return __atomic_load_n(&gSess.emitted, __ATOMIC_ACQUIRE); }
static inline bool sess_hello() { Session *g = __atomic_load_n(&gS, __ATOMIC_ACQUIRE); return g != nullptr && g->hello; }
static inline uint64_t retired_now(uint64_t emitted) {
    sysmem_rmb();
    return retired_clamped(*seq_slot(), emitted);
}
static inline bool hung_now() { return __atomic_load_n(&gHang.hung, __ATOMIC_ACQUIRE); }

bool n1c_is_open() { return gS != nullptr; }

bool n1c_refuse_legacy(uint32_t site) {
    if (!native_s1b_latched()) return false;
    if (site < 8 && !gLegacyLogged[site]) {
        gLegacyLogged[site] = true;
        N1C_LOG("REFUSING legacy client site %u (%s): the native VM self-test ran on this boot; the legacy GFX-ring writers and the "
                "unlocked VRAM pool users are off (reboot without navi48-native=1 for that path)", site,
                site == kN1cSiteSelfTest ? "SelfTest" : site == kN1cSiteAllocVRAM ? "AllocVRAM" : site == kN1cSiteFreeVRAM ? "FreeVRAM" :
                site == kN1cSiteWriteVRAM ? "WriteVRAM" : "?");
    }
    return true;
}

// ---- the HUNG latch -----------------------------------------------------------------------------------------------------------
static void hang_log(uint64_t emitted, uint64_t retired) {
    CPContext &cp = gCtx->cp;
    N1C_LOG("HUNG: seq emitted %llu retired %llu rptr %u wptr %u CP_STAT %#x GCVM_L2_PROTECTION_FAULT_STATUS_LO32 %#x",
            (unsigned long long)emitted, (unsigned long long)retired, cp.rptr_cpu ? *cp.rptr_cpu : 0u, cp.wptr,
            RREG32(*gCtx->dev, SOC15_REG_OFFSET_BIDX(*gCtx->dev, IPBlock::GC, 0, 0x0F40u)), hub_rd(0x15d0));   // CP_STAT, GC base idx 0
}
// 0.0.612 (W3): the ONE place a latch is announced. Both latch sites (hang_poll, hang_from_wait) call it only after hang_detect / hang_latch_wait returned true (gHang.hung is set),
// and it writes Navi48,Ready = 0 on the Metal nub (sticky: never back to 1 this boot). Called with no hang lock held.
static void hang_announce(uint64_t emitted, uint64_t retired) {
    hang_log(emitted, retired);
    Navi48MetalNub::hungLatched();
}
// Rule 6.3(b), on any GPU-dependent call: true when THIS call latched the hang.
static bool hang_poll() {
    const uint64_t em = emitted_now(), re = retired_now(em), now = now_ns();
    IOLockLock(gHangLock);
    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
    return det;
}
// Rule 6.3(a): a bounded internal wait reached 2 s.
static void hang_from_wait() {
    const uint64_t em = emitted_now(), re = retired_now(em);
    IOLockLock(gHangLock);
    const bool det = hang_latch_wait(gHang);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
}
// The prologue of every GPU-dependent call: Aborted after a latch, Timeout for the call that detects it, else kOk.
static uint32_t gpu_gate() {
    if (hung_now()) return hang_rc(true, false);
    return hang_rc(false, hang_poll());
}
// Wait for retired >= emitted, bounded at 2 s. true = idle. false = not idle (already hung, or this wait latched the hang).
static bool idle_wait() {
    const uint64_t start = now_ns();
    for (;;) {
        const uint64_t em = emitted_now();
        if (retired_now(em) >= em) return true;
        if (hung_now()) return false;
        const uint64_t el = now_ns() - start;
        if (el >= kWaitCapNs) { hang_from_wait(); return false; }
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
}

// ---- the ring -------------------------------------------------------------------------------------------------------------------
// Everything a Submit writes goes through here: the space check, the write, the emitted-seq update and the kick. Caller holds the client
// lock. Returns kOk, or Timeout when the space wait reached 2 s (which latches the hang).
static uint32_t ring_emit(const uint32_t *dw, uint32_t n, uint64_t seq) {
    DeviceContext &dev = *gCtx->dev;
    CPContext &cp = gCtx->cp;
    IOLockLock(gRingLock);
    const uint32_t mask = cp.ring_ptr_mask;
    if (n > cp.ring_size_dwords / 2u) { IOLockUnlock(gRingLock); return kBadArg; }
    const uint64_t t0 = now_ns();
    for (;;) {
        sysmem_rmb();
        const uint32_t r = *cp.rptr_cpu & mask;
        if (ring_has_space(ring_free64(r, gWc, mask), n)) break;
        const uint64_t el = now_ns() - t0;
        if (el >= kWaitCapNs) {
            IOLockUnlock(gRingLock);
            N1C_LOG("ring space: need %u dwords (+%u slack), rptr %u wptr %u, no room in 2 s", n, kRingSlack, r, (uint32_t)(gWc & mask));
            hang_from_wait();
            return kTimeout;
        }
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    uint64_t wc = gWc;
    for (uint32_t i = 0; i < n; i++) { ring[ring_idx(wc, mask)] = dw[i]; wc++; }
    gWc = wc;
    cp.wptr = ring_idx(wc, mask);
    // The clock for rule 6.3(b) starts at the kick that makes an idle ring busy; emitted goes up BEFORE the doorbell so retired can never
    // be observed above emitted.
    const uint64_t emBefore = emitted_now();
    const uint64_t reNow = retired_now(emBefore);
    IOLockLock(gHangLock);
    hang_kick(gHang, reNow, emBefore, now_ns());
    IOLockUnlock(gHangLock);
    __atomic_store_n(&gSess.emitted, seq, __ATOMIC_RELEASE);
    amdgpu_hdp_flush(dev);
    const uint64_t wptr64 = wc;   // the FULL 64-bit counter, never masked
    *reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(cp.wb_cpu) + kCPWBOffsetWptr) = wptr64;
    sysmem_wmb();
    const uint64_t off = (uint64_t)cp.doorbell_index * kCPDoorbellStride;
    WDOORBELL64(dev, off, wptr64);
    (void)RDOORBELL32(dev, off);
    IOLockUnlock(gRingLock);
    return kOk;
}

// ---- the page tables ------------------------------------------------------------------------------------------------------------
struct PtMem {
    uint64_t rd(uint64_t pa, uint32_t idx) { return RBAR0_64(*gCtx->dev, pa - gCtx->gmc.vram_base_offset + (uint64_t)idx * 8u); }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { WBAR0_64(*gCtx->dev, pa - gCtx->gmc.vram_base_offset + (uint64_t)idx * 8u, v); }
    // A zeroed 4 KiB table page from the client's arena (16 KiB blocks of the visible pool: the pool's minimum allocation), or 0 at the cap.
    uint64_t alloc_page() {
        if (gS->pagesInBlk >= 4u) {
            if (gS->nblk >= kArenaBlocks || would_exceed((uint64_t)gS->nblk * kArenaBlockBytes, kArenaBlockBytes, kPtCap)) return 0ull;
            uint32_t idx;
            if (gPtFreeN > 0) idx = gPtFree[--gPtFreeN];
            else {
                if (gPtN >= kArenaBlocks) return 0ull;
                VRAMAllocation a {};
                if (!gCtx->gmc.vram_alloc.alloc(kArenaBlockBytes, kArenaBlockBytes, &a)) return 0ull;
                const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
                if (a.size < kArenaBlockBytes || off + a.size > gCtx->dev->bar0Size) { gCtx->gmc.vram_alloc.free(a); return 0ull; }
                gPt[gPtN] = a;
                idx = gPtN++;
            }
            bar0_memset_vram(*gCtx->dev, gPt[idx].gpu_va - gCtx->gmc.vram_start, 0, kArenaBlockBytes);   // a reused block is zeroed again
            gS->blk[gS->nblk++] = (uint16_t)idx;
            gS->pagesInBlk = 0;
        }
        const uint64_t pa = pa_of_mc(gPt[gS->blk[gS->nblk - 1]].gpu_va) + (uint64_t)gS->pagesInBlk * kPage;
        gS->pagesInBlk++;
        gS->ptPages++;
        return pa;
    }
};

// Return a session's page-table blocks to the reserve (never to vram_alloc).
static void pt_release_blocks(Session *s) {
    for (uint32_t i = 0; i < s->nblk; i++) gPtFree[gPtFreeN++] = s->blk[i];
    s->nblk = 0;
}
static uint32_t flush_vmid(uint32_t vmid) {
    const kern_return_t r = gmc_flush_gpu_tlb(*gCtx->dev, gCtx->gmc, gCtx->gmc.gfxhub, vmid, 0);
    if (r != kIOReturnSuccess) {
        N1C_LOG("TLB flush for VMID %u did not acknowledge (kr=%#x): treating as a hang", vmid, r);
        hang_from_wait();
        return kTimeout;
    }
    return kOk;
}
// The CONTEXT8 register addresses (the same arithmetic as native_s1b.cpp).
struct CtxRegs { uint32_t cntl, baseLo, baseHi, stLo, stHi, enLo, enHi; };
static CtxRegs ctx_regs() {
    const HubContext &h = gCtx->gmc.gfxhub;
    DeviceContext &dev = *gCtx->dev;
    CtxRegs r;
    r.cntl   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_cntl) + (kNativeVmid - 1u) * h.ctx_distance;
    r.baseLo = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_lo) + kNativeVmid * h.ctx_addr_distance;
    r.baseHi = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_hi) + kNativeVmid * h.ctx_addr_distance;
    r.stLo   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_lo) + (kNativeVmid - 1u) * h.ctx_addr_distance;
    r.stHi   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_hi) + (kNativeVmid - 1u) * h.ctx_addr_distance;
    r.enLo   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_lo) + (kNativeVmid - 1u) * h.ctx_addr_distance;
    r.enHi   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_hi) + (kNativeVmid - 1u) * h.ctx_addr_distance;
    return r;
}
// Point CONTEXT8 at `rootPa` (VALID set, START 0, END max_pfn - 1) and flush the VMID-8 TLB.
static uint32_t program_root(uint64_t rootPa) {
    DeviceContext &dev = *gCtx->dev;
    const CtxRegs r = ctx_regs();
    const uint64_t root = rootPa | kPteValid;
    WREG32(dev, r.baseLo, (uint32_t)(root & 0xFFFFFFFFu));
    WREG32(dev, r.baseHi, (uint32_t)(root >> 32));
    WREG32(dev, r.stLo, 0u);
    WREG32(dev, r.stHi, 0u);
    WREG32(dev, r.enLo, (uint32_t)((kMaxPfn - 1u) & 0xFFFFFFFFu));
    WREG32(dev, r.enHi, (uint32_t)((kMaxPfn - 1u) >> 32));
    return flush_vmid(kNativeVmid);
}

// ---- open / close -------------------------------------------------------------------------------------------------------------------
IOReturn n1c_open(BringupContext &ctx) {
    DeviceContext *dev = ctx.dev;
    const NativeS1bState &s1b = native_s1b_state();
    // (1) the ladder: stage 17 reached, CP up, the compute test passed.
    if (dev == nullptr || ctx.reached != BringupStage::ComputeDispatch || !ctx.cp.inited || !ctx.computePassed || ctx.cp.wb_cpu == nullptr ||
        ctx.cp.ring_cpu == nullptr || ctx.cp.rptr_cpu == nullptr || dev->bar0 == nullptr || dev->bar2 == nullptr ||
        !ctx.gmc.gfxhub.inited || !ctx.gmc.vram_alloc.is_inited()) {
        N1C_LOG("open refused (NotReady): the ladder did not reach stage 17 with CP, the compute test, BAR0/BAR2 and the GFXHUB up "
                "(reached %u, cp %d, compute %d)", (unsigned)ctx.reached, (int)ctx.cp.inited, (int)ctx.computePassed);
        return kIOReturnNotReady;
    }
    // (2) the S1b gate: accepted, ran, POSITIVE PASS, no latched stop.
    if (s1b.gate != kGateOn || !s1b.ran || !s1b.positivePass || s1b.stopped) {
        N1C_LOG("open refused (NotReady): native-s1b gate %u ran %d positive %d stopped %d (need gate ON, ran, POSITIVE PASS, not stopped)",
                s1b.gate, (int)s1b.ran, (int)s1b.positivePass, (int)s1b.stopped);
        return kIOReturnNotReady;
    }
    // (3) not HUNG this boot.
    if (gHang.hung) {
        N1C_LOG("open refused (NotReady): the GPU was declared HUNG earlier this boot; reboot");
        return kIOReturnNotReady;
    }
    // (4) exclusivity.
    if (!OSCompareAndSwap(0, 1, &gOpenFlag)) {
        N1C_LOG("open refused (ExclusiveAccess): a native client is already open");
        return kIOReturnExclusiveAccess;
    }
    lazy_lock(&gCliLock); lazy_lock(&gRingLock); lazy_lock(&gHangLock);
    if (!gCliLock || !gRingLock || !gHangLock) { OSCompareAndSwap(1, 0, &gOpenFlag); return kIOReturnNoMemory; }
    gCtx = &ctx;

    // The whole open runs under the client lock so a racing close cannot see a half-built session.
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    Session *s = nullptr;
    do {
        CPContext &cp = ctx.cp;
        // The ring must be idle: S1b's last submission has retired.
        const uint64_t t0 = now_ns();
        for (;;) {
            sysmem_rmb();
            if ((*cp.rptr_cpu & cp.ring_ptr_mask) == cp.wptr) break;
            if (now_ns() - t0 >= kWaitCapNs) { N1C_LOG("open refused (NotReady): the kernel ring is not idle (rptr %u wptr %u)", *cp.rptr_cpu, cp.wptr); rc = kIOReturnNotReady; break; }
            IOSleep(1);
        }
        if (rc != kIOReturnSuccess) break;
        // CONTEXT8 must carry the S1b geometry (DEPTH 3 / BLOCK 0 / RETRY 0): a context that lost it must not be pointed at a tree.
        const CtxRegs r = ctx_regs();
        const uint32_t cntl = RREG32(*dev, r.cntl);
        if ((cntl & kCntlEnableMask) == 0u || ((cntl & kCntlDepthMask) >> kCntlDepthShift) != kCtxDepth || (cntl & kCntlRetryMask) != 0u ||
            ((cntl & kCntlBlockMask) >> kCntlBlockShift) != kCtxBlockSize) {
            N1C_LOG("open refused (NotReady): CONTEXT%u CNTL %#010x is not the native geometry (DEPTH 3 / BLOCK 0 / RETRY 0)", kNativeVmid, cntl);
            rc = kIOReturnNotReady; break;
        }
        // The fence window: 16 GART pages reserved once per boot from the same bump allocator the ring, MQD and write-back page came from.
        if (!gWin.reserved) {
            GMCContext &gmc = ctx.gmc;
            const uint64_t bytes = (uint64_t)N48N_FENCE_SLOTS * kPage;
            if (gmc.gart_size == 0 || gmc.gart_pt_bus == 0 || gmc.gart_bump_offset + bytes > gmc.gart_size) {
                N1C_LOG("open refused (NoMemory): no GART room for the fence window"); rc = kIOReturnNoMemory; break;
            }
            gWin.gartOff = gmc.gart_bump_offset;
            gmc.gart_bump_offset += bytes;
            gWin.mc = gmc.gart_start + gWin.gartOff;
            gWin.reserved = true;
            const uint64_t zero = 0;
            for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++)
                bar0_memcpy_to_vram(*dev, gmc.gart_pt_vram_offset + (gWin.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
            amdgpu_hdp_flush(*dev);
            N1C_LOG("fence window reserved: %u GART pages at mc %#llx (offset %#llx), every PTE invalid until a GTT fence target binds it",
                    N48N_FENCE_SLOTS, (unsigned long long)gWin.mc, (unsigned long long)gWin.gartOff);
        }
        // The park root: a kernel-owned zeroed page CONTEXT8 is pointed at while no client owns a tree.
        if (!gPark.have) {
            if (!ctx.gmc.vram_alloc.alloc(kArenaBlockBytes, kArenaBlockBytes, &gPark.a)) { rc = kIOReturnNoMemory; break; }
            const uint64_t off = gPark.a.gpu_va - ctx.gmc.vram_start;
            if (off + gPark.a.size > dev->bar0Size) { ctx.gmc.vram_alloc.free(gPark.a); rc = kIOReturnNoMemory; break; }
            bar0_memset_vram(*dev, off, 0, gPark.a.size);
            amdgpu_hdp_flush(*dev);
            gPark.pa = pa_of_mc(gPark.a.gpu_va);
            gPark.have = true;
        }
        if (!gWcInit) { gWc = cp.wptr; gWcInit = true; }   // the ring is idle: the counter starts at the hardware wptr and never restarts
        s = &gSess;
        bzero(s, sizeof(Session));
        s->pagesInBlk = 4u;   // forces the first block
        __atomic_store_n(&gS, s, __ATOMIC_RELEASE);
        PtMem mem;
        s->rootPa = mem.alloc_page();
        if (s->rootPa == 0ull) { N1C_LOG("open refused (NoMemory): no VRAM for the root table"); rc = kIOReturnNoMemory; break; }
        amdgpu_hdp_flush(*dev);
        // Zero the native seqno slot while the ring is idle, and start the clock.
        *seq_slot() = 0ull;
        sysmem_wmb();
        gHang.lastRetired = 0; gHang.tProgress = now_ns();
        if (program_root(s->rootPa) != kOk) { rc = kIOReturnNotReady; break; }
        __atomic_store_n(&gSessSeq, ++gSessSeqCounter, __ATOMIC_SEQ_CST);   // 0.0.609
        N1C_LOG("client opened: VMID %u, fresh root pa %#llx (visible pool), CONTEXT8 CNTL %#010x, ring %u dwords rptr %u wptr %u, fence window mc %#llx",
                kNativeVmid, (unsigned long long)s->rootPa, cntl, cp.ring_size_dwords, *cp.rptr_cpu, cp.wptr, (unsigned long long)gWin.mc);
    } while (false);
    if (rc != kIOReturnSuccess) {
        if (s != nullptr) pt_release_blocks(s);
        __atomic_store_n(&gS, (Session *)nullptr, __ATOMIC_RELEASE);
        OSCompareAndSwap(1, 0, &gOpenFlag);
    }
    IOLockUnlock(gCliLock);
    return rc;
}

enum RelMode : uint32_t { kRelNormal = 0, kRelLeak = 1, kRelClosing = 2 };
static_assert(kRelNormal == kHostRelNormal && kRelLeak == kHostRelLeak && kRelClosing == kHostRelClosing, "the pure header's release modes are RelMode");
static void bo_release(uint32_t h, RelMode mode);

// ---- the scanout pins (0.0.603) -------------------------------------------------------------------------------------------------
// A BO that hosts scanout slots (ScanoutRegister) carries pinMask / pinGen, written ONLY under gCliLock. The console plane goes back BEFORE
// such a BO is freed or leaked, on every path: BoFree, the HUNG leak, and the close of the whole session (lock order gCliLock -> the DCN lock).
static void scan_teardown(Session *s, const char *how, uint64_t r[2]) {
    r[0] = 1; r[1] = 0;
    (void)n48dcn::scanRelease(how, r);
    for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) { if (r[0] == 0ull && s->bo[h].pinMask != 0u) s->bo[h].pinLeak = 1u; s->bo[h].pinMask = 0; }
    if (r[0] == 0ull) N1C_LOG("scanout: the console restore at %s was NOT verified (plane %#llx): run `navi48test accel dcnflip 0`, reboot if it persists", how, (unsigned long long)r[1]);
}
// BoFree / release of a BO with pins. n48scan::unpin_plan (via n48dcn::scanBoGone) decides: Normal - a slot that is neither shown nor pending is
// simply dropped, one the hardware is fetching (or has pending) needs the full restore first. Leak (HUNG) and Closing: ALWAYS the full restore
// first, then the caller leaks or frees. Either way the console is back before the BO's memory is freed or leaked.
static bool scan_unpin(uint32_t h, Bo &b, RelMode mode) {
    uint64_t r[2] = { 1, 0 };
    const uint32_t res = n48dcn::scanBoGone(b.pinMask, b.pinGen, mode != kRelNormal, r);
    if (res != 0u) {
        N1C_LOG("BO %u hosted scanout slot(s) %#x and is going away (%s): the console restore %s", h, (unsigned)b.pinMask,
                mode == kRelNormal ? "BoFree while shown or pending" : "leak / close", res == 1u ? "ran FIRST and VERIFIED" : "did NOT verify: the BO is LEAKED, not freed");
    }
    b.pinMask = 0;
    return res == 2u;   // true = the plane may still scan this BO: leak it
}

void n1c_close(const char *how) {
    if (gS == nullptr) return;
    IOLockLock(gCliLock);
    Session *s = gS;
    if (s == nullptr) { IOLockUnlock(gCliLock); return; }
    DeviceContext &dev = *gCtx->dev;
    { uint64_t tr[2]; scan_teardown(s, how, tr); }     // 0.0.603: the console plane FIRST - before the idle wait, before the HUNG decision, before anything is freed or leaked
    { const uint32_t seq = __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST); __atomic_store_n(&gSessSeq, 0u, __ATOMIC_SEQ_CST); n48dcn::modeHoldSessionClosed(seq); }   // 0.0.609: a row-120 hold owned by this session ends (or, HANDOFF, passes on) - AFTER the plane is back; atomic words only
    bool leak = hung_now();
    if (!leak && !idle_wait()) leak = true;            // a timeout here latched the hang
    uint32_t nBos = 0, nHost = 0; uint64_t vramB = 0, gttB = 0, hostB = 0;
    for (uint32_t h = 1; h < N48N_MAX_BOS; h++) {
        if (!s->boUsed[h]) continue;
        nBos++;
        if (s->bo[h].kind == kBoGtt) gttB += s->bo[h].size;
        else if (s->bo[h].kind == kBoHost) { hostB += s->bo[h].size; nHost++; }   // 0.0.612
        else vramB += s->bo[h].size;
    }
    const uint64_t seqs = s->emitted;
    const uint32_t ptPages = s->ptPages;
    if (!leak) {
        // No client owns a tree from here: park CONTEXT8 on the kernel's zeroed page, then free everything.
        if (program_root(gPark.pa) != kOk) leak = true;
    }
    if (!leak) {
        for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) bo_release(h, kRelClosing);
        bool slots = false;
        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) {
            if (!s->fslot[i].used) continue;
            const uint64_t zero = 0;
            bar0_memcpy_to_vram(dev, gCtx->gmc.gart_pt_vram_offset + (gWin.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
            s->fslot[i].used = 0; slots = true;
        }
        if (slots) { amdgpu_hdp_flush(dev); (void)flush_vmid(0); }
        pt_release_blocks(s);   // into the reserve, not vram_alloc
        N1C_LOG("client closed (%s): %u BOs (%llu MiB VRAM, %llu MiB GTT), %u PT pages, %llu seqs, freed", how, nBos,
                (unsigned long long)(vramB >> 20), (unsigned long long)(gttB >> 20), ptPages, (unsigned long long)seqs);
        if (nHost != 0u) N1C_LOG("client closed (%s): %u host imports (%llu KiB) unmapped, then completed and released", how, nHost, (unsigned long long)(hostB >> 10));   // 0.0.612
    } else {
        // HUNG: nothing a hung GPU might still read is freed; CONTEXT8 stays as it is.
        N1C_LOG("client closed (%s): %u BOs (%llu MiB VRAM, %llu MiB GTT), %u PT pages, %llu seqs, LEAKED (HUNG)", how, nBos,
                (unsigned long long)(vramB >> 20), (unsigned long long)(gttB >> 20), ptPages, (unsigned long long)seqs);
        if (nHost != 0u) N1C_LOG("client closed (%s): %u host imports (%llu KiB) LEAKED (HUNG): pages stay wired, never completed or released", how, nHost, (unsigned long long)(hostB >> 10));   // 0.0.612
    }
    __atomic_store_n(&gS, (Session *)nullptr, __ATOMIC_RELEASE);   // gSess itself is static and never freed
    OSCompareAndSwap(1, 0, &gOpenFlag);
    IOLockUnlock(gCliLock);
}

// ---- BOs ------------------------------------------------------------------------------------------------------------------------
// 0.0.612: the memory of a host-import BO. Called ONLY where the PTEs are already gone and the TLB flushed (kRelNormal: after pt_unmap + flush_vmid; kRelClosing: after program_root(park)),
// and NEVER in kRelLeak (bo_release's leak branch does not reach it: host_may_release). complete() before release(); the page list goes last.
static void host_release(Bo &b) {
    if (b.hmd != nullptr) { b.hmd->complete(); b.hmd->release(); b.hmd = nullptr; }
    if (b.hpages != nullptr) { IOFree(b.hpages, (vm_size_t)b.hpageCount * sizeof(uint64_t)); b.hpages = nullptr; }
}
// Drop every mapping, fence slot and the memory of handle h. leak = HUNG: forget the handle and the software state, touch nothing else.
static void bo_release(uint32_t h, RelMode mode) {
    Session *s = gS;
    Bo &b = s->bo[h];
    if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;   // 0.0.603: the console before the memory, in every mode
    if (b.pinLeak != 0u) mode = kRelLeak;             // a restore that did not verify: keep the memory and the PTEs, like HUNG
    if (mode == kRelClosing) {
        // The whole tree is being discarded (CONTEXT8 is parked, the arena is freed by the caller) and the fence-window PTEs are cleared
        // by the caller in one pass: only the memory goes.
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) sysmem_free(b.sm);
        else if (b.kind == kBoHi) gCtx->gmc.vram_alloc_hi.free(b.a);
        else if (b.kind == kBoVis) gCtx->gmc.vram_alloc.free(b.a);
        else if (b.kind == kBoHost && host_may_release(kHostRelClosing)) host_release(b);   // 0.0.612: CONTEXT8 is parked and the TLB flushed (the caller's program_root)
    } else if (mode == kRelNormal) {
        bool touched = false;
        PtMem mem;
        for (uint32_t i = 0; i < kMaxMaps; i++) {
            const VaEnt &e = s->maps[i];
            if (va_ent_used(e) && e.handle == h) { pt_unmap(s->rootPa, e.start, e.size / kPage, mem); touched = true; }
        }
        bool memOk = true;   // review item D: false once a TLB flush did not acknowledge (HUNG): the memory of this BO is then leaked, like the HUNG leak
        if (touched) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(kNativeVmid)) && memOk; }
        bool slotCleared = false;
        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) {
            if (s->fslot[i].used && s->fslot[i].handle == h) {
                const uint64_t zero = 0;
                bar0_memcpy_to_vram(*gCtx->dev, gCtx->gmc.gart_pt_vram_offset + (gWin.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
                s->fslot[i] = FenceSlot{ 0, 0, 0 };
                slotCleared = true;
            }
        }
        if (slotCleared) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk; }
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) { if (memOk) sysmem_free(b.sm); }         // review item D: a flush that did not acknowledge leaks the GTT pages
        else if (b.kind == kBoHi) gCtx->gmc.vram_alloc_hi.free(b.a);
        else if (b.kind == kBoVis) gCtx->gmc.vram_alloc.free(b.a);
        else if (b.kind == kBoHost && host_may_release(kHostRelNormal) && memOk) host_release(b);   // 0.0.612: AFTER pt_unmap and flush_vmid above (unmap before complete)
    } else {
        // Leak: the memory, the PTEs and any fence-slot PTE stay; only the software records go.
        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) if (s->fslot[i].used && s->fslot[i].handle == h) s->fslot[i].used = 0;
    }
    va_remove_handle(s->maps, kMaxMaps, h);
    if (b.kind == kBoGtt) s->gttUsed -= b.size;
    else if (b.kind == kBoHost) s->importedBytes = import_after_free(s->importedBytes, b.size);   // 0.0.612 (a leaked host BO: its pages stay wired, only the software record goes)
    else s->vramBytes -= b.size;
    s->nBos--;
    s->boUsed[h] = 0;
    memset(&b, 0, sizeof(b));
}

// 0.0.610 (ABI 1.8): read-only views for the Metal nub gate (that source file is the only user).
bool n1c_hello_done() { return sess_hello(); }
bool n1c_hung() { return hung_now(); }

IOReturn n1c_hello(uint64_t clientAbi, uint64_t flags, uint64_t out[4]) {
    if (gS == nullptr || gCliLock == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    if (gS == nullptr) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }
    IOReturn rc = kIOReturnSuccess;
    if ((flags & ~(uint64_t)N48N_HELLO_F_MINOR) != 0ull) rc = kIOReturnBadArgument;
    else if (clientAbi != N48N_ABI_VERSION) { N1C_LOG("hello: client ABI %llu, kernel ABI %u: refused", (unsigned long long)clientAbi, N48N_ABI_VERSION); rc = kIOReturnUnsupported; }
    else {
        gS->hello = true;
        // ABI 1.1: the minor is reported only when the client asks for it; without the flag out[0] is exactly the major, as in 1.0.
        out[0] = ((flags & N48N_HELLO_F_MINOR) != 0ull) ? (uint64_t)N48N_ABI_VERSION | ((uint64_t)N48N_ABI_MINOR << 16) : (uint64_t)N48N_ABI_VERSION;
        out[1] = kN1cKextBuild; out[2] = N48N_MAX_IBS; out[3] = kNativeVmid;
    }
    IOLockUnlock(gCliLock);
    return rc;
}

IOReturn n1c_query_info(n48n_info *o) {
    if (!sess_hello()) return kIOReturnNotReady;
    BringupContext &c = *gCtx;
    memset(o, 0, sizeof(*o));
    const IPVersion v = c.dev->ip.getVersion(IPBlock::GC);
    const uint64_t em = emitted_now();
    o->abi_version = N48N_ABI_VERSION; o->kext_build = kN1cKextBuild;
    o->flags = (hung_now() ? N48N_INFO_HUNG : 0u) | (native_s1b_state().positivePass ? N48N_INFO_S1B_POSITIVE : 0u) |
               (native_s1b_state().gate == kGateOn ? N48N_INFO_NATIVE_BOOT : 0u);
    o->vmid = kNativeVmid;
    o->gb_addr_config = RREG32(*c.dev, SOC15_REG_OFFSET_BIDX(*c.dev, IPBlock::GC, GFXRegBaseIdx::GB_ADDR_CONFIG, GFXRegs::GB_ADDR_CONFIG));
    o->gc_version = ((uint32_t)v.major << 16) | ((uint32_t)v.minor << 8) | v.rev;
    o->ring_size_dw = c.cp.ring_size_dwords; o->max_ibs = N48N_MAX_IBS;
    o->vram_vis_total = c.gmc.vram_alloc.size(); o->vram_vis_free = c.gmc.vram_alloc.bytes_free();
    o->vram_hi_total = c.gmc.vram_alloc_hi.is_inited() ? c.gmc.vram_alloc_hi.size() : 0;
    o->vram_hi_free = c.gmc.vram_alloc_hi.is_inited() ? c.gmc.vram_alloc_hi.bytes_free() : 0;
    o->gtt_cap = kGttCap; o->gtt_used = gSess.gttUsed; o->gtt_max_bo = kGttMaxBo;
    o->pt_cap = kPtCap; o->pt_used = (uint64_t)gSess.nblk * kArenaBlockBytes;
    o->reserved[0] = (uint32_t)(gWc & 0xFFFFFFFFu); o->reserved[1] = (uint32_t)(gWc >> 32);   // the native write counter (a diagnostic; the contract leaves reserved[] unused)
    o->reserved[2] = N48N_ABI_MINOR;                                                          // ABI 1.1 (0.0.603): the minor
    o->seq_emitted = em; o->seq_retired = retired_now(em);
    o->va_low_first = kVaFirst; o->va_low_last = 0x00007FFFFFFFFFFFull;
    o->va_high_first = 0xFFFF800000000000ull; o->va_high_last = 0xFFFFFFFFFFFFFFFFull;
    o->max_bos = N48N_MAX_BOS; o->fence_slots = N48N_FENCE_SLOTS;
    return kIOReturnSuccess;
}

IOReturn n1c_read_regs(uint64_t off, uint64_t count, uint64_t instance, uint32_t *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    if (count < 1 || count > 16 || instance != 0xFFFFFFFFull) return kIOReturnBadArgument;
    if (!regs_allowed(off, count) || (off + count) * 4ull > gCtx->dev->rmmioSize) return kIOReturnBadArgument;   // allowlist 0x263e..0x2641 only
    for (uint64_t i = 0; i < count; i++) out[i] = RREG32(*gCtx->dev, (uint32_t)(off + i));
    return kIOReturnSuccess;
}

IOReturn n1c_bo_create(const n48n_gem_create_in *in, uint64_t out[4]) {
    if (!sess_hello()) return kIOReturnNotReady;
    const Place p = place_decide(in->bo_size, in->alignment, in->domains, in->domain_flags);
    if (p.rc != kOk) return (IOReturn)p.rc;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    Session *s = gS;
    DeviceContext &dev = *gCtx->dev;
    IOReturn rc = (IOReturn)gpu_gate();
    do {
        if (rc != kIOReturnSuccess) break;
        const uint32_t h = lowest_free(s->boUsed, 1, N48N_MAX_BOS - 1);
        if (h == 0) { rc = kIOReturnNoResources; break; }
        Bo b {};
        b.uc = p.uc; b.size = p.size;
        uint32_t placed = N48N_GEM_DOMAIN_GTT, bits = 0;
        if (p.kind == kPlaceVis) {
            VRAMAllocation a {};
            bool got = gCtx->gmc.vram_alloc.alloc(p.size, p.align, &a);
            if (got) {
                const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
                if (off + a.size > dev.bar0Size) { gCtx->gmc.vram_alloc.free(a); got = false; }
            }
            if (got) {
                b.kind = kBoVis; b.a = a; b.mc = a.gpu_va;
                bar0_memset_vram(dev, a.gpu_va - gCtx->gmc.vram_start, 0, a.size);
                amdgpu_hdp_flush(dev);
                placed = N48N_GEM_DOMAIN_VRAM; bits = N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED;
            } else if (p.hiOk && gCtx->gmc.vram_alloc_hi.is_inited() && gCtx->gmc.vram_alloc_hi.alloc(p.size, p.align, &a)) {
                b.kind = kBoHi; b.a = a; b.mc = a.gpu_va;
                placed = N48N_GEM_DOMAIN_VRAM; bits = N48N_PLACED_HI_POOL;
            } else { rc = kIOReturnNoMemory; break; }
        } else {
            if (p.size > kGttMaxBo || would_exceed(s->gttUsed, p.size, kGttCap)) { rc = kIOReturnNoMemory; break; }
            SysMem sm {};
            if (sysmem_alloc(sm, p.size, p.align) != kIOReturnSuccess || !sm.valid()) { sysmem_free(sm); rc = kIOReturnNoMemory; break; }
            b.kind = kBoGtt; b.sm = sm;
            s->gttUsed += p.size;
            bits = N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED;
        }
        if (b.kind != kBoGtt) s->vramBytes += p.size;
        s->bo[h] = b; s->boUsed[h] = 1; s->nBos++;
        out[0] = h; out[1] = p.size; out[2] = placed; out[3] = bits;
    } while (false);
    IOLockUnlock(gCliLock);
    return rc;
}

IOReturn n1c_bo_free(uint64_t handle) {
    if (!sess_hello()) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    if (handle > 0xFFFFFFFFull || !id_live(gS->boUsed, N48N_MAX_BOS - 1, (uint32_t)handle)) rc = kIOReturnNotFound;
    else {
        const uint32_t h = (uint32_t)handle;
        if (hung_now()) bo_release(h, kRelLeak);                // HUNG: drop the handle, leak the memory and the PTEs, succeed
        else if (!idle_wait()) { bo_release(h, kRelLeak); rc = kIOReturnTimeout; }   // this call detected it: it returns Timeout, the handle is dropped and leaked
        else bo_release(h, kRelNormal);
    }
    IOLockUnlock(gCliLock);
    return rc;
}

// 0.0.612 (review item A): the top of DRAM is NOT read from a kernel symbol: max_mem / mem_actual / sane_size exist in the kernel's own symbol table but are not in any KPI header or export list
// the SDK carries, so a reference could leave the kext unloadable. The kext's own derivation (the EFI memory map, gfx_ramtop.h) is used instead; 0 = it was not read cleanly = unknown.
void n1c_latch_pci_bars(const uint64_t *base, const uint64_t *size, uint32_t n) {
    if (base == nullptr || size == nullptr || n > kMaxPciBars) return;
    if (!OSCompareAndSwap(0, 2, &gBarsState)) return;          // first writer wins; 2 = being written
    for (uint32_t i = 0; i < n; i++) { gBars[i].base = base[i]; gBars[i].size = size[i]; }
    gNBars = n;
    __atomic_store_n(&gBarsState, 1u, __ATOMIC_RELEASE);
    N1C_LOG("pci bars latched: %u BAR range(s) will never be imported as host pages", n);
}

// ---- BoImportHost (0.0.612, ABI 1.9) ----------------------------------------------------------------------------------------------------
// The pure collect_pages' segment source over a prepare()d descriptor: the physical address of byte `off` and the length of the contiguous run there. CPU physical == bus (no IOMMU on this platform).
struct DescSeg {
    IOMemoryDescriptor *md;
    uint64_t phys(uint64_t off, uint64_t *len) {
        IOByteCount seg = 0;
        const addr64_t pa = md->getPhysicalSegment((IOByteCount)off, &seg, kIOMemoryMapperNone);
        *len = pa != 0 ? (uint64_t)seg : 0ull;
        return (uint64_t)pa;
    }
};
// Order: (1) argument and advisory cap check, no lock, nothing touched; (2) wire the caller's range and read its physical pages OUTSIDE the client lock (prepare() may fault pages in and
// sleep: a racing close must not wait for a page-in); (3) under the client lock: the HUNG gate, the AUTHORITATIVE cap check, the handle, the optional map, the record. A failure before (3)
// completes leaves nothing mapped, so the descriptor is completed and released even under the HUNG latch (no PTE ever named those pages).
IOReturn n1c_bo_import_host(task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]) {
    const uint32_t seq0 = __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST);   // review item C: the session this import belongs to, read BEFORE anything is wired
    if (!sess_hello()) return kIOReturnNotReady;
    if (task == nullptr || out == nullptr) return kIOReturnBadArgument;
    const ImportChk pre = import_check(hostVa, size, flags, gpuVa, __atomic_load_n(&gSess.importedBytes, __ATOMIC_RELAXED));
    if (pre.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached (%llu MiB held, %llu KiB asked)", (unsigned long long)(kImportCap >> 20), (unsigned long long)(__atomic_load_n(&gSess.importedBytes, __ATOMIC_RELAXED) >> 20), (unsigned long long)(size >> 10));   // 0.0.620
    if (pre.rc != kOk) return (IOReturn)pre.rc;
    IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)hostVa, (mach_vm_size_t)size, kIODirectionInOut, task);
    if (md == nullptr) return kIOReturnNoMemory;
    if (md->prepare() != kIOReturnSuccess) { md->release(); return kIOReturnBadArgument; }   // the range is not (fully) mapped in the caller
    uint64_t *pages = static_cast<uint64_t *>(IOMalloc((vm_size_t)(pre.pages * sizeof(uint64_t))));
    if (pages == nullptr) { md->complete(); md->release(); return kIOReturnNoMemory; }
    DescSeg seg { md };
    const uint32_t crc = collect_pages(seg, size, pages, kImportMaxPages);
    if (crc != kOk) {
        IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
        md->complete(); md->release();
        N1C_LOG("import refused (%#x): the caller's range %#llx + %llu KiB has no usable physical page list", crc, (unsigned long long)hostVa, (unsigned long long)(size >> 10));
        return (IOReturn)crc;
    }
    {   // review item A: a page of device memory (any of this GPU's BARs, or above the top of DRAM when known) is never imported. Nothing is mapped yet, so the descriptor is completed and released.
        const bool barsOk = __atomic_load_n(&gBarsState, __ATOMIC_ACQUIRE) == 1u;
        const uint64_t dramTop = ::n48::hw_hook_ramtop_derived_or_zero();
        const uint64_t bad = import_first_refused(pages, pre.pages, barsOk ? gBars : nullptr, barsOk ? gNBars : 0u, dramTop);
        if (bad < pre.pages) {
            const uint64_t badPa = pages[bad];
            const PageVerdict v = import_page_verdict(badPa, barsOk ? gBars : nullptr, barsOk ? gNBars : 0u, dramTop);
            IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
            md->complete(); md->release();
            N1C_LOG("import refused: device page (page %llu of %llu at pa %#llx: %s)", (unsigned long long)bad, (unsigned long long)pre.pages, (unsigned long long)badPa,
                    v == kPageInBar ? "inside a PCI BAR of this GPU" : v == kPageAboveDram ? "above the top of DRAM" : "no BAR list latched");
            return kIOReturnBadArgument;
        }
    }
    IOReturn rc = kIOReturnSuccess;
    bool taken = false;
    IOLockLock(gCliLock);
    do {
        if (!sess_hello()) { rc = kIOReturnNotReady; break; }   // closed while we waited for the lock
        if (!session_unchanged(seq0, __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST))) { rc = kIOReturnNotReady; break; }   // review item C: a different (or no) session now: these pages are the old client's
        Session *s = gS;
        const uint32_t gate = gpu_gate();
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        const ImportChk c = import_check(hostVa, size, flags, gpuVa, s->importedBytes);   // authoritative: the total as of now
        if (c.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached under the lock (%llu MiB held, %llu KiB asked)", (unsigned long long)(kImportCap >> 20), (unsigned long long)(s->importedBytes >> 20), (unsigned long long)(size >> 10));   // 0.0.620
        if (c.rc != kOk) { rc = (IOReturn)c.rc; break; }
        const uint32_t h = lowest_free(s->boUsed, 1, N48N_MAX_BOS - 1);
        if (h == 0) { rc = kIOReturnNoResources; break; }
        uint32_t slot = 0xFFFFFFFFu;
        bool mapped = false;
        if (c.gpuStripped != 0ull) {
            if (va_overlap(s->maps, kMaxMaps, c.gpuStripped, size) >= 0) { rc = kIOReturnBadArgument; break; }
            for (uint32_t i = 0; i < kMaxMaps; i++) if (!va_ent_used(s->maps[i])) { slot = i; break; }
            if (slot == 0xFFFFFFFFu) { rc = kIOReturnNoResources; break; }
            PtMem mem;
            const uint32_t r = pt_map_pages(s->rootPa, c.gpuStripped, c.pages, pages, leaf_flags((uint32_t)flags, false, true), mem);   // SYSTEM | SNOOPED (+ R/W/X, MTYPE as asked)
            if (r != kOk) { rc = (IOReturn)r; break; }
            mapped = true;
        }
        Bo b {};
        b.kind = kBoHost; b.size = size; b.hmd = md; b.hpages = pages; b.hpageCount = (uint32_t)c.pages;
        s->bo[h] = b; s->boUsed[h] = 1; s->nBos++;
        s->importedBytes += size;
        taken = true;                                             // md and pages now belong to the BO record
        if (mapped) {
            s->maps[slot] = VaEnt{ c.gpuStripped, size, 0, h, (uint32_t)flags };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = flush_vmid(kNativeVmid);           // synchronous: the mapping is complete on return
            if (f != kOk) rc = (IOReturn)f;                        // a hang was latched: the BO stays recorded (leaked at close)
        }
        out[0] = h; out[1] = size; out[2] = mapped ? va_canonicalize(c.gpuStripped) : 0ull; out[3] = N48N_PLACED_HOST_IMPORT;
    } while (false);
    IOLockUnlock(gCliLock);
    if (!taken) {
        IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
        md->complete(); md->release();                            // never mapped: safe even when HUNG
    }
    return rc;
}

IOReturn n1c_gem_va(const n48n_gem_va *in) {
    if (!sess_hello()) return kIOReturnNotReady;
    const VaReq q = gemva_check(in->operation, in->handle, in->_pad, in->flags, in->va_address, in->offset_in_bo, in->map_size,
                                in->vm_timeline_point, in->vm_timeline_syncobj_out, in->num_syncobj_handles, in->input_fence_syncobj_handles);
    if (q.rc != kOk) return (IOReturn)q.rc;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    Session *s = gS;
    IOReturn rc = kIOReturnSuccess;
    do {
        if (in->operation == N48N_VA_OP_UNMAP && hung_now()) {
            // HUNG: skip the wait and the tables; forget the software mapping (its PTEs are leaked with the BO).
            const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);
            if (hit >= 0) s->maps[hit] = VaEnt{ 0, 0, 0, 0, 0 };
            break;
        }
        const uint32_t gate = gpu_gate();
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        if (!id_live(s->boUsed, N48N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
        Bo &b = s->bo[in->handle];
        if (!range_in_bo(in->offset_in_bo, in->map_size, b.size)) { rc = kIOReturnBadArgument; break; }
        PtMem mem;
        if (in->operation == N48N_VA_OP_MAP) {
            if (va_overlap(s->maps, kMaxMaps, q.stripped, in->map_size) >= 0) { rc = kIOReturnBadArgument; break; }
            const bool host = b.kind == kBoHost;                      // 0.0.612: scattered pages (b.hpages), mapped SYSTEM|SNOOPED like a GTT BO
            const bool sys = b.kind == kBoGtt || host;
            const uint64_t basePa = host ? 0ull : (sys ? b.sm.bus : pa_of_mc(b.mc)) + in->offset_in_bo;
            uint32_t slot = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < kMaxMaps; i++) if (!va_ent_used(s->maps[i])) { slot = i; break; }
            if (slot == 0xFFFFFFFFu) { rc = kIOReturnNoResources; break; }
            const uint32_t r = host ? pt_map_pages(s->rootPa, q.stripped, in->map_size / kPage, b.hpages + in->offset_in_bo / kPage, leaf_flags(in->flags, b.uc, sys), mem)
                                    : pt_map(s->rootPa, q.stripped, in->map_size / kPage, basePa, leaf_flags(in->flags, b.uc, sys), mem);
            if (r != kOk) { rc = (IOReturn)r; break; }
            s->maps[slot] = VaEnt{ q.stripped, in->map_size, in->offset_in_bo, in->handle, in->flags };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = flush_vmid(kNativeVmid);      // synchronous: the mapping is complete on return
            if (f != kOk) rc = (IOReturn)f;
        } else {
            const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);
            if (hit < 0) { rc = kIOReturnBadArgument; break; }
            if (!idle_wait()) {
                // Not idle: leave the tables alone. This call's wait latched the hang, so it returns Timeout.
                rc = kIOReturnTimeout;
                break;
            }
            pt_unmap(s->rootPa, q.stripped, in->map_size / kPage, mem);
            s->maps[hit] = VaEnt{ 0, 0, 0, 0, 0 };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = flush_vmid(kNativeVmid);
            if (f != kOk) rc = (IOReturn)f;
        }
    } while (false);
    IOLockUnlock(gCliLock);
    return rc;
}

IOReturn n1c_ctx(const n48n_ctx *in, n48n_ctx *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    Session *s = gS;
    memset(out, 0, sizeof(*out));
    IOReturn rc = kIOReturnSuccess;
    switch (in->op) {
    case N48N_CTX_OP_ALLOC: {
        const uint32_t id = lowest_free(s->ctxUsed, 1, N48N_MAX_CTX);
        if (id == 0) { rc = kIOReturnNoResources; break; }
        s->ctxUsed[id] = 1;
        out->op = id; out->flags = 0;          // dword0 = ctx_id, dword1 = 0
        break;
    }
    case N48N_CTX_OP_FREE:
        if (!id_live(s->ctxUsed, N48N_MAX_CTX, in->ctx_id)) rc = kIOReturnNotFound;
        else s->ctxUsed[in->ctx_id] = 0;
        break;
    case N48N_CTX_OP_QUERY_STATE2: {
        if (!id_live(s->ctxUsed, N48N_MAX_CTX, in->ctx_id)) { rc = kIOReturnNotFound; break; }
        const uint64_t fl = hung_now() ? (N48N_CTX_QUERY2_RESET | N48N_CTX_QUERY2_GUILTY) : 0ull;
        out->op = (uint32_t)(fl & 0xFFFFFFFFu); out->flags = (uint32_t)(fl >> 32);   // qword0
        break;
    }
    default:
        rc = kIOReturnBadArgument;
    }
    IOLockUnlock(gCliLock);
    return rc;
}

// The VMID-0 address of a user-fence target (contract 4.3): a VRAM BO by its MC address, a GTT BO through the fence window.
static uint32_t resolve_fence(uint32_t handle, uint32_t off, uint64_t *addr) {
    Session *s = gS;
    const Bo &b = s->bo[handle];
    if (b.kind == kBoHost) return kNotPermitted;   // 0.0.612: an imported range of the caller's memory is never a kernel-written fence target
    if (b.kind != kBoGtt) { *addr = fence_addr_vram(b.mc, off); return kOk; }
    const uint32_t page = off / (uint32_t)kPage;
    bool isNew = false;
    const int slot = fence_slot_pick(s->fslot, N48N_FENCE_SLOTS, handle, page, &isNew);
    if (slot < 0) return kNoResources;
    if (isNew) {
        // U3: gmc_bind_existing only bumps, gart_bind_existing runs a different allocator; so the slot's PTE is written directly with the
        // encoding gmc_bind_existing uses (PTEFlags::SYSMEM_RW on the page's bus address), inside the window reserved at first open.
        const uint64_t pte = ((b.sm.bus + (uint64_t)page * kPage) & ~0xFFFull) | PTEFlags::SYSMEM_RW;
        bar0_memcpy_to_vram(*gCtx->dev, gCtx->gmc.gart_pt_vram_offset + (gWin.gartOff / kPage + (uint64_t)slot) * 8ull, &pte, sizeof(pte));
        amdgpu_hdp_flush(*gCtx->dev);
        const uint32_t f = flush_vmid(0);
        if (f != kOk) return f;
        s->fslot[slot] = FenceSlot{ handle, page, 1 };
    }
    *addr = fence_addr_gtt(gWin.mc, (uint32_t)slot, off);
    return kOk;
}

IOReturn n1c_submit(const uint8_t *in, uint32_t size, uint64_t *seqOut) {
    if (!sess_hello()) return kIOReturnNotReady;
    CsView v;
    const uint32_t prc = cs_parse(in, size, &v);
    if (prc != kOk) return (IOReturn)prc;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    Session *s = gS;
    IOReturn rc = kIOReturnSuccess;
    do {
        const uint32_t gate = gpu_gate();
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        if (!id_live(s->ctxUsed, N48N_MAX_CTX, v.ctx)) { rc = kIOReturnNotFound; break; }
        const bool hasFence = (v.flags & N48N_CS_HAS_FENCE) != 0u;
        if (hasFence) {
            if (!id_live(s->boUsed, N48N_MAX_BOS - 1, v.fenceHandle)) { rc = kIOReturnNotFound; break; }
            if (!cs_fence_in_bo(v.fenceOffset, s->bo[v.fenceHandle].size)) { rc = kIOReturnBadArgument; break; }
        }
        // Every IB must sit entirely in EXEC-mapped pages of this client: an unmapped fetch could halt the CP.
        uint64_t va[N48N_MAX_IBS]; uint32_t by[N48N_MAX_IBS];
        bool bad = false;
        for (uint32_t i = 0; i < v.nIbs; i++) {
            cs_ib(in, i, &va[i], &by[i]);
            const uint64_t st = va_strip(va[i]);
            if (st < kVaFirst || (st >> 47) != ((st + by[i] - 1ull) >> 47) || !va_exec_covered(s->maps, kMaxMaps, st, by[i])) { bad = true; break; }
        }
        if (bad) { rc = kIOReturnBadArgument; break; }
        uint64_t fenceAddr = 0;
        if (hasFence) {
            const uint32_t fr = resolve_fence(v.fenceHandle, v.fenceOffset, &fenceAddr);
            if (fr != kOk) { rc = (IOReturn)fr; break; }
        }
        const uint64_t seq = emitted_now() + 1ull;
        const uint64_t seqAddr = gCtx->cp.wb_bus + kWbOffsetNativeSeq;
        const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, v.nIbs, kNativeVmid, hasFence,
                                        fenceAddr, seqAddr, seq);
        if (n == 0u) { rc = kIOReturnInternalError; break; }
        const uint32_t er = ring_emit(gSubmitBuf, n, seq);
        if (er != kOk) { rc = (IOReturn)er; break; }
        *seqOut = seq;
    } while (false);
    IOLockUnlock(gCliLock);
    return rc;
}

IOReturn n1c_wait(uint64_t target, uint64_t timeoutNs, uint64_t out[3]) {
    if (!sess_hello()) return kIOReturnNotReady;
    uint64_t res = 0;
    uint64_t em = emitted_now();
    const uint32_t rrc = wait_resolve(target, em, &res);
    if (rrc != kOk) return (IOReturn)rrc;
    const uint64_t to = wait_timeout_ns(timeoutNs);
    const uint64_t start = now_ns();
    uint64_t re = 0;
    for (;;) {
        em = emitted_now();
        re = retired_now(em);
        if (res == 0ull || re >= res) { out[0] = 0; out[1] = re; out[2] = em; return kIOReturnSuccess; }
        if (hung_now()) return kIOReturnAborted;
        const uint64_t el = now_ns() - start;
        if (el >= to) break;
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    if (hang_poll()) return kIOReturnTimeout;                // this call's wait detected the hang
    out[0] = 1; out[1] = re; out[2] = em;
    return kIOReturnSuccess;
}

// ---- ABI 1.1: native scanout (0.0.603) --------------------------------------------------------------------------------------------
// Lock order: gCliLock, then the scanout lock inside n48dcn::scan*. Acquire / Register / Release take gCliLock (they touch the session's
// pins, and a racing close must not leave the plane taken); Query / Present / Status take only the scanout lock (a Present in flight either
// happened before the close's restore, which covers it, or is refused after it).
IOReturn n1c_scan_query(n48n_scan_query *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    return (IOReturn)n48dcn::scanQuery(out);
}
IOReturn n1c_scan_acquire(uint64_t flags, uint64_t out[2]) {
    if (!sess_hello()) return kIOReturnNotReady;
    if (flags != 0ull) return kIOReturnBadArgument;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    const IOReturn rc = (IOReturn)n48dcn::scanAcquire(out, __atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST));   // 0.0.609: the session id lets a row-120 hold be Acquired
    IOLockUnlock(gCliLock);
    return rc;
}
IOReturn n1c_scan_register(const n48n_scan_reg *in, uint64_t out[2]) {
    if (!sess_hello()) return kIOReturnNotReady;
    if (in->reserved0 != 0u) return kIOReturnBadArgument;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    Session *s = gS;
    IOReturn rc = kIOReturnSuccess;
    do {
        if (!id_live(s->boUsed, N48N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
        Bo &b = s->bo[in->handle];
        if (b.kind == kBoHost) { rc = kIOReturnNotPermitted; break; }   // 0.0.612: system pages of the caller are never a scanout slot (HUBP flips VRAM only)
        const uint32_t gen = n48dcn::scanGeneration();
        if (b.pinMask != 0u && b.pinGen != gen) b.pinMask = 0u;                // a pin of an acquisition that has ended is stale
        uint64_t o2[2] = { 0, 0 };
        const uint32_t r = n48dcn::scanRegister(b.kind == kBoVis, b.mc, b.size, in->offset, in->pitch_bytes, in->width, in->height, in->format, o2);
        if (r != kOk) { rc = (IOReturn)r; break; }
        b.pinMask = (uint8_t)(b.pinMask | (1u << (o2[0] & 7u))); b.pinGen = gen;   // recorded under gCliLock, released in bo_release / close / Release
        out[0] = o2[0]; out[1] = o2[1];
    } while (false);
    IOLockUnlock(gCliLock);
    return rc;
}
IOReturn n1c_scan_present(uint64_t slot, uint64_t flags, uint64_t out[3]) {
    if (!sess_hello()) return kIOReturnNotReady;
    return (IOReturn)n48dcn::scanPresent(slot, flags, out);
}
IOReturn n1c_scan_status(n48n_scan_status *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    return (IOReturn)n48dcn::scanStatus(out);
}
IOReturn n1c_scan_release(uint64_t out[2]) {
    if (!sess_hello()) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    uint64_t r[2] = { 1, 0 };
    scan_teardown(gS, "ScanoutRelease", r);                                    // idempotent; the verdict is the restore's own
    out[0] = r[0]; out[1] = r[1];
    IOLockUnlock(gCliLock);
    return kIOReturnSuccess;
}

// ---- ABI 1.2: the DAL experiment (0.0.604). One named step of notes/design/NATIVE-S2-DISPCLK.md per call. The client lock is NOT held: a step
// sleeps for up to ~65 s (E4's dwell) and a racing close must not wait for it; smu_dal.cpp's own busy flag refuses a second concurrent step, and
// its latch, gate and allowlist do all the refusing. Nothing here touches a session field.
IOReturn n1c_dal_step(uint64_t step, uint64_t flags, n48n_dal_result *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    if (flags != 0ull || step < N48N_DAL_STEP_E1B || step > N48N_DAL_STEP_E4 || out == nullptr) return kIOReturnBadArgument;
    return dal_run_step(*gCtx->dev, (uint32_t)step, out);
}

// ---- ABI 1.3: the timed mode trial (0.0.605). The client lock is NOT held (the trial sleeps for up to dwell + ~12 s and a racing close must not wait for it); dcn/navi48_dcn.cpp's
// busy flag, latch, gate and deny checks do all the refusing. Nothing here touches a session field.
IOReturn n1c_mode_trial(uint64_t row, uint64_t dwellMs, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    if ((flags & ~(uint64_t)N48N_MODE_TF_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;   // 0.0.606: the flags word carries N48N_MODE_TF_* (0.0.605 required 0)
    return (IOReturn)n48dcn::modeTrial(row, dwellMs, flags, out);
}

// ---- ABI 1.7: the HELD mode of row 120 (0.0.609). Like the trial: no client lock is held (the launch waits up to ~20 s, the release up to ~30 s); the session id names the owner.
IOReturn n1c_mode_hold(uint64_t maxMs, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    if ((flags & ~(uint64_t)N48N_HOLD_F_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)n48dcn::modeHold(__atomic_load_n(&gSessSeq, __ATOMIC_SEQ_CST), maxMs, flags, out);
}
IOReturn n1c_mode_release(uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello()) return kIOReturnNotReady;
    if ((flags & ~(uint64_t)N48N_REL_F_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)n48dcn::modeRelease(flags, out);
}

IOReturn n1c_memory_for_handle(uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (memory == nullptr) return kIOReturnBadArgument;
    *memory = nullptr;
    if (!sess_hello()) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    if (!sess_hello()) { IOLockUnlock(gCliLock); return kIOReturnNotReady; }   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    if (!id_live(gS->boUsed, N48N_MAX_BOS - 1, handle)) rc = kIOReturnNotFound;
    else {
        Bo &b = gS->bo[handle];
        if (b.kind == kBoHi || b.kind == kBoHost) rc = kIOReturnNotPermitted;   // 0.0.612: a host import is already in the caller's own address space
        else if (b.kind == kBoGtt) {
            b.sm.md->retain();
            *memory = b.sm.md;
        } else {
            // ONE descriptor per BO (0.0.602): IOConnectUnmapMemory64 finds the existing mapping by descriptor identity, so a fresh object per call
            // could never be unmapped. Created on first use, kept in the BO record, retained for each caller, released once at BoFree / close.
            if (b.vmd == nullptr) {
                const uint64_t phys = gCtx->dev->bar0Phys + (b.mc - gCtx->gmc.vram_start);
                b.vmd = IODeviceMemory::withRange((IOPhysicalAddress)phys, (IOPhysicalLength)b.size);
            }
            IODeviceMemory *md = b.vmd;
            if (md == nullptr) rc = kIOReturnNoMemory;
            else { md->retain(); *memory = md; if (options) *options |= kIOMapWriteCombineCache; }   // NOTE: XNU's mapClientMemory64 replaces the cache bits with the caller's own mapFlags, so a caller that wants write-combining passes kIOMapWriteCombineCache itself
        }
    }
    IOLockUnlock(gCliLock);
    return rc;
}

} // namespace amdgpu
