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
//  0.0.623 (GPU-apps G1, boot-arg navi48-g1=1, default OFF): the hang-recovery verbs (n1c_g1_verb, the block at the end of this file). With the boot-arg ON they ALSO write:
//  kernel GFX ring dwords outside the live range (NOPs) and, after an ACKED MES unmap, the whole ring (NOPs), the CP write-back rptr / wptr words and the MQD memory
//  (cp_gfx_mqd_init); they send MES RESET / REMOVE_QUEUE / ADD_QUEUE frames for GFX pipe 0 queue 0 (the doorbell writes are mes_submit_pkt's and ring_emit's own); and they clear the HUNG latch after a verified probe. REGISTER WRITES (0.0.626 correction of the 0.0.623 text "No MMIO register write", which was wrong): none of their own, but the paths they call write registers, all in the graphics block, the VM/TLB block or HDP: the doorbells (MES and ring), GRBM_GFX_CNTL twice (cp_map_gfx_kgq_mes -> cp_dump_gfx_hqd), the VMID-8 TLB-invalidate registers (setup_bo -> flush_vmid), the CONTEXT8 registers (program_root at open and close) and the HDP flush (amdgpu_hdp_flush). Nothing in DCN, PSP or SMU.
//
//  0.0.627 (GPU-apps G2, boot-arg navi48-multisession=1, default OFF): up to four sessions, slot i on VMID 8 + i, each with its own tree, BO / map tables, fence window, caps,
//  lock and last seqno; one shared in-order ring. With the switch ON this file ALSO writes: the CONTEXT9..11 registers (CNTL once to the S1b geometry with cntl_native,
//  then BASE / START / END at open and the park at close, exactly as CONTEXT8), those VMIDs' TLB invalidates, their slots' fence-window GART PTEs, and - on a stall whose
//  oldest unretired submission belongs to a non-WindowServer session - the G1 recovery on its own initiative: ring dwords outside the live range (NOPs), one MES RESET
//  (reset_legacy_gfx) frame and one kernel probe RELEASE_MEM on the ring. Nothing in DCN, PSP or SMU. With the switch OFF the behaviour is 0.0.626's (one session, VMID 8).
//
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOUserClient.h>   // 0.0.641: IOUserClient::removeMappingForDescriptor (n1c_unmap_for_close)
#include <kern/clock.h>
#include <libkern/OSAtomic.h>

#include "native_s1c.h"
#include "native_hostimport_pure.h"   // 0.0.612 (ABI 1.9): BoImportHost
#include "Navi48MetalNub.hpp"        // 0.0.612: hungLatched() writes Navi48,Ready = 0
#include <IOKit/IOMemoryDescriptor.h>
#include "dcn/navi48_dcn.hpp"     // build 0.0.603: n48dcn::scan* (the scanout path behind selectors 9..14)
#include "dcn/navi48_scanout_pure.h"
#include "dcn/navi48_scanx_flow.h"   // 0.0.661 (M6 Stage 1b): teardown (the instances restored before any BO is touched); 0.0.662: teardown_all, three instances
#include "native_disp.h"          // 0.0.661: n48m6_latched_on / n48m6flip_latched_on (the selectors 22..26 latch)
#include "smu_dal.h"          // build 0.0.604: dal_run_step (selector 15, the DISPCLK/DPPCLK experiment)
#include "amdgpu_log.h"
#include "amdgpu_field_defs.h"
#include "amdgpu_gfx.h"
#include "amdgpu_pm4.h"
#include "cp_pm4_gfx12.h"
#include "native_d2pin_pure.h" // 0.0.652 (M5): the monitor B pair's pin / free / alloc decisions (host-tested)
#include "native_g1_pure.h"   // 0.0.623 (G1): hang recovery (boot-arg navi48-g1=1, default OFF)
#include "native_g2_pure.h"   // 0.0.627 (G2): several sessions (boot-arg navi48-multisession=1, default OFF)
#include "native_g5_pure.h"   // 0.0.650 (G5 Stage 1): the application credit (boot-arg navi48-g5=1, default OFF)
#include "native_g4_pure.h"   // 0.0.640 (G4): the APP role, the allow-list, the budget report, the close-path mapping rule (boot-arg navi48-apps=1, default OFF)
#include <pexpert/pexpert.h>  // 0.0.623: PE_parse_boot_argn (navi48-g1)

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
static_assert(n48native::g2::kBusy == (uint32_t)kIOReturnBusy, "R6's refusal is kIOReturnBusy");
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
    uint8_t   pinMask1;    // 0.0.662 (M6 Stage 2): bit k = this BO hosts INSTANCE 1's (the monitor A's) slot 0x10|k (SCANX_REGISTER); under gCliLock; stale when pinGen1 != n48dcn::scanXGeneration(1)
    uint32_t  pinGen1;
    uint8_t   pinMask2;    // 0.0.661 (M6 Stage 1b): bit k = this BO hosts INSTANCE 2's slot 0x20|k (SCANX_REGISTER); under gCliLock; stale when pinGen2 != n48dcn::scanXGeneration(2)
    uint32_t  pinGen2;
    uint8_t   pinLeak;     // the console restore did NOT verify while this BO was pinned: it may still be scanned, so it is LEAKED (like HUNG), never freed
    uint8_t   budgeted;    // 0.0.627 (G2): this VRAM BO is counted in gVramOthers (a non-WindowServer session, switch ON); taken off when its memory is freed
    uint8_t   sysCounted;  // 0.0.629 (R4): this GTT / host-import BO is counted in gSysHeld (switch ON, any session); taken off only when its memory is really freed
    uint8_t   visCounted;  // 0.0.641 (G4 review): this VISIBLE-pool BO is counted in gVisOthers (a non-WindowServer session, apps ON); taken off when its memory is freed
    SysMem    sm;          // gtt
    IODeviceMemory *vmd;   // vis: the ONE CPU-map descriptor of this BO (0.0.602), created on first clientMemoryForType, released at BoFree / close, leaked with the BO when HUNG
    IOMemoryDescriptor *hmd;   // host (0.0.612): the prepare()d descriptor of the caller's range; complete() + release() at BoFree / close, LEAKED when HUNG
    uint64_t *hpages;          // host: the physical page list (heap, hpageCount entries), freed with the descriptor
    uint32_t  hpageCount;
};
using n48native::g2::kMaxSessions;
constexpr uint32_t kArenaBlocks = (uint32_t)(kPtCap / 16384ull);   // 2048 blocks of 16 KiB (the pool's minimum allocation): one session's page-table cap
constexpr uint32_t kArenaBlockBytes = 16384u;
constexpr uint32_t kArenaReserve = kArenaBlocks * kMaxSessions;    // 0.0.627 (G2): the per-boot reserve can hold every session's cap (with the switch OFF only kArenaBlocks are ever used)
static_assert(kArenaReserve <= 0xFFFFu, "page-table block indices fit the uint16_t lists");
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
    uint64_t  emitted;         // seqno of THIS session's last Submit (0.0.627: the global count is gEmitted). Written under gRingLock; atomics
    // 0.0.627 (G2): identity. lock / slot / vmid never change once the slot exists; id / open / ws are written under gCliLock at open and close.
    IOLock   *lock;            // THIS session's lock: its tables and counters (the old client lock's job for one session). Allocated with the slot, never freed
    uint32_t  slot, vmid, id;  // id: unique per open (1, 2, 3 ...), the client's N1cRef carries it; 0 = never opened
    bool      open, ws;        // ws: admitted by the uid-88 (WindowServer) rule
    bool      app;             // 0.0.640 (G4): admitted by the allow-list rule (an ordinary user application); set once by n1c_mark_app right after the open, never ReadRegs / scanout
    uint64_t  appKey;          // 0.0.641: the executable-name key (Navi48AppKey.h) of an APP session, for the strike count; written once with `app`
    volatile UInt32 poisoned;  // 1 once an automatic recovery named this session guilty (switch ON): Submit refused, waits and GPU calls Aborted
    uint64_t  faultAtOpen;     // 0.0.629 (R5): n48native::g2::fault_word read (never cleared) at this session's open, for sessstat
};
static Session gSess;          // slot 0 (VMID 8): STATIC and never freed: a selector racing a close can never touch freed memory
// 0.0.627 (G2): the session slots. Slot 0 is gSess; slots 1..3 (switch ON only) are allocated on first use and NEVER freed, for the same reason.
static Session *gSlot[kMaxSessions] = { &gSess, nullptr, nullptr, nullptr };
static uint8_t gSlotState[kMaxSessions];   // n48native::g2::SlotState; written under gCliLock. A DEAD slot's VMID is never handed out again this boot
// The per-boot page-table reserve: blocks taken from vram_alloc for page tables are NEVER returned to it (a user CPU mapping kept
// after BoFree can therefore never alias a page table); a closing client returns its blocks to this list and the next client reuses them.
static VRAMAllocation gPt[kArenaReserve];
static uint16_t gPtFree[kArenaReserve];
static uint32_t gPtN, gPtFreeN;
// The native write counter: 64 bits, monotonically increasing, initialised from cp.wptr at the first native open. `& mask` is only the ring
// index and the space check; the FULL value goes to the wptr shadow and the doorbell (upstream gfx12 support_64bit_ptrs).
static uint64_t gWc;
static bool gWcInit;
static uint64_t gEmitted;      // 0.0.627 (G2): the global seqno of the last submission on the ring (any session, or a kernel probe); written under gRingLock, atomics. Restarts at 0 with the first session
static BringupContext *gCtx;
static volatile UInt32 gOpenFlag;   // the number of open sessions: 0 or 1 with the switch OFF (OSCompareAndSwap, as 0.0.626), 0..4 with it ON (under gCliLock)
static Hang gHang;             // boot-wide: hung survives the client
// THE LOCK MAP (0.0.627, G2). Order: a session's own lock -> gCliLock -> gRingLock -> gHangLock; gTlbLock is a leaf (nothing is taken under it).
//   Session::lock  one per session: that session's BO / map / ctx / fence-slot tables, its counters and hello. Held for a whole selector body, INCLUDING
//                  its own-seqno waits (own_wait), and NEVER while another session's lock is wanted.
//   gCliLock       the SHARED state, never held across a GPU wait: the slot table (open / close, VMID hand-out, gOpenFlag with the switch ON, gSessWs),
//                  the page-table reserve (gPt / gPtFree, PtMem::alloc_page), the VRAM pools (vram_alloc / vram_alloc_hi alloc and free) and the VRAM
//                  budget, the GART fence windows (reservation and PTE writes), CONTEXTn programming and the park root, the scanout pins and owner.
//                  The G1 block (a kernel-owned test context, exclusive by construction) still uses it as before.
//   gRingLock      the ring: seqno assignment (gEmitted), gSubmitBuf, the ring dwords, gWc, the owner log, a session's `emitted`, the doorbell. The space
//                  wait (2 s) runs under it, as before. An automatic recovery holds it for its whole bounded run (emission frozen).
//   gHangLock      gHang and gRecovering.
//   gTlbLock       gmc_flush_gpu_tlb (one invalidation engine for every VMID; 0.0.626 only ever flushed under the one client lock).
// A GPU-dependent check that may RUN a recovery (gpu_gate, hang_poll, own_wait, stall_from_wait) is called with at most the caller's own session lock:
// never with gCliLock or gRingLock held.
static IOLock *gCliLock, *gRingLock, *gHangLock, *gTlbLock;
static uint32_t gSessSeqCounter = 0;   // 0.0.609: the session identity (1, 2, 3 ... per open); 0.0.627: carried per session (Session::id)
// 0.0.623 (G1): 1 while an open native session was admitted by the uid-88 (WindowServer) rule. 0.0.627: set by n1c_open (under gCliLock) for that
// session and cleared by its close. Read by `hangtest`, which refuses on it by name.
static volatile UInt32 gSessWs;
// 0.0.612 (review item A): this GPU's PCI BARs (the FULL sizes), latched once at start by n1c_latch_pci_bars. gBarsState: 0 = not latched, 1 = latched. Read without a lock after the acquire load.
static PciBar gBars[kMaxPciBars];
static uint32_t gNBars;
static volatile UInt32 gBarsState;
static volatile UInt32 gBar0Conflict;   // 0.0.663 (ReBAR item 5): 0 = not latched, 1 = agree, 2 = IOPCIFamily and config space DISAGREE on BAR0 (n1c_open refuses)
static bool gLegacyLogged[8];
static uint32_t gSubmitBuf[280];   // one Submit's dwords; 0.0.627: used only under gRingLock (ring_submit)

struct FenceWindow { bool reserved; uint64_t gartOff; uint64_t mc; };
static FenceWindow gWin[kMaxSessions];   // 0.0.627 (G2): one 16-page window per slot (slot 0's is the 0.0.626 window)
struct ParkRoot { bool have; VRAMAllocation a; uint64_t pa; };
static ParkRoot gPark;
// 0.0.627 (G2) state.
static n48native::g2::OwnerLog gOwners;   // the owner of every seqno (written under gRingLock before the seqno is published)
static volatile UInt32 gRecovering;       // 1 while an automatic recovery runs (set and cleared under gHangLock)
static uint64_t gVramOthers;              // VRAM bytes held by non-WindowServer sessions (switch ON), under gCliLock
static volatile UInt32 gScanOwner;        // switch ON: the session id that holds the scanout plane (0 = none), under gCliLock
struct G2Stat { uint64_t recoveries, failed, lastGuiltySeq, collateralTotal; uint32_t lastGuiltySlot; };
static G2Stat gG2;
static uint64_t gSysHeld;                 // 0.0.629 (R4): GTT + host-import bytes held by ALL sessions (switch ON), under gCliLock; cap n48native::g2::kSysGlobalCap
static uint64_t gFaultPrev;               // 0.0.629 (R5): the fault word at the previous `sessstat 4` (0 before the first); written only by n1c_g2_stat, under gCliLock
// 0.0.640 (G4) state.
static uint64_t gAppAllow[n48native::g4::kMaxEntries];   // the APP allow-list (Navi48AppKey.h keys, 0 = empty slot); static = zero at every boot, never persisted; lock-free (CAS)
static uint64_t gMappedLeakBytes;         // visible-VRAM bytes leaked at a session close because a CPU mapping outlived it (switch ON), under gCliLock; they stay counted in gVramOthers
static uint32_t gMappedLeakBos;           // ... and how many BOs
static uint64_t gVisOthers;               // 0.0.641 (G4 review): VISIBLE-pool bytes held by non-WindowServer sessions (apps ON), under gCliLock; leaked bytes stay counted. Cap: appvis_cap()
static n48native::g4::StrikeTab gStrikes; // 0.0.641: automatic recoveries blamed on an APP session, per executable-name key (atomics; zero at every boot)
// 0.0.650 (G5 Stage 1) state. All of it is written only with navi48-g5=1 (OFF: never touched).
static volatile UInt32 gWsId;             // the WindowServer session id (0 = none); set in n1c_open / cleared in n1c_close beside gSessWs, under gCliLock; read with atomics
static uint64_t gWsLastNs;                // the time of WindowServer's last submit (atomics; 0 = never)
static uint64_t gG5Since[n48native::g2::kMaxSessions];   // per slot: when that slot's app Submit began waiting for its turn (0 = not waiting); guarded by gRingLock
static n48native::g5::Stat gG5;           // the counters of sessstat page 5; guarded by gRingLock
static n48native::g5::AdmitIn gG5In;     // ring_submit_app_g5's verdict inputs, rebuilt on every try; guarded by gRingLock (a file-scope static keeps the Submit path's stack frame small)
static uint64_t gLogGate;                 // 0.0.641: the "open refused" log rate limiter (n48native::g4::log_gate_step); a log-only word

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
static inline uint64_t emitted_now() { return __atomic_load_n(&gEmitted, __ATOMIC_ACQUIRE); }
static inline uint64_t retired_now(uint64_t emitted) {
    sysmem_rmb();
    return retired_clamped(*seq_slot(), emitted);
}
static inline bool hung_now() { return __atomic_load_n(&gHang.hung, __ATOMIC_ACQUIRE); }

// ---- the sessions (0.0.627, G2) ------------------------------------------------------------------------------------------------------
static bool multi_on();   // boot-arg navi48-multisession=1 (latched once; below)
static bool g5_on();      // 0.0.650 (G5 Stage 1): navi48-g5=1 AND navi48-multisession=1 (latched once; below)
static uint32_t g5_credit();   // 0.0.650: K, navi48-g5credit (1..4, default 1; latched once; below)
static bool apps_on();    // 0.0.640 (G4): navi48-apps=1 AND navi48-multisession=1 (latched once; below)
static uint64_t appvis_cap();   // 0.0.641: the non-WindowServer visible-pool cap in bytes (boot-arg navi48-appvis, latched once; below)
static bool g1_holds();   // a G1 test context holds the GPU (the G1 block)
static inline Session *slot_ptr(uint32_t slot) { return slot < kMaxSessions ? __atomic_load_n(&gSlot[slot], __ATOMIC_ACQUIRE) : nullptr; }
// The caller's session without a lock: open and still the SAME session (the id of its open). A Session is never freed, so a stale reference reads a
// closed or re-opened record and is refused.
static inline Session *sess_peek(const N1cRef &r) {
    Session *s = slot_ptr(r.slot);
    if (s == nullptr || r.id == 0u || !__atomic_load_n(&s->open, __ATOMIC_ACQUIRE) || __atomic_load_n(&s->id, __ATOMIC_ACQUIRE) != r.id) return nullptr;
    return s;
}
static inline bool sess_hello(const N1cRef &r) { Session *g = sess_peek(r); return g != nullptr && __atomic_load_n(&g->hello, __ATOMIC_ACQUIRE); }
// Take the caller's session lock and re-check under it. Returns the session LOCKED, or null with nothing held.
static Session *sess_lock(const N1cRef &r, bool needHello) {
    Session *s = slot_ptr(r.slot);
    IOLock *l = s != nullptr ? __atomic_load_n(&s->lock, __ATOMIC_ACQUIRE) : nullptr;
    if (l == nullptr) return nullptr;
    IOLockLock(l);
    if (!s->open || r.id == 0u || s->id != r.id || (needHello && !s->hello)) { IOLockUnlock(l); return nullptr; }
    return s;
}
static inline void sess_unlock(Session *s) { IOLockUnlock(s->lock); }
static inline bool sess_poisoned(const Session *s) { return __atomic_load_n(&s->poisoned, __ATOMIC_SEQ_CST) != 0u; }
// 0.0.640 (G4): defence in depth. The client class already keeps an APP session off ReadRegs, the scanout set and selectors 15..20 (native_open_policy_pure.h
// selector_allowed_for_app); each engine function behind them asks again, so a future mistake in the dispatch cannot open them. True = refuse (NotPrivileged).
static inline bool app_refused(const N1cRef &r, uint32_t call) {
    const Session *g = sess_peek(r);
    return g != nullptr && !n48native::g4::app_call_allowed(__atomic_load_n(&g->app, __ATOMIC_ACQUIRE), call);
}
static inline uint64_t vram_total() { return gCtx->gmc.vram_alloc.size() + (gCtx->gmc.vram_alloc_hi.is_inited() ? gCtx->gmc.vram_alloc_hi.size() : 0ull); }

bool n1c_is_open() { return __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) != 0u; }

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
// 0.0.612 (W3): the ONE place a latch is announced. Every latch site calls it only after hang_detect / hang_latch_wait returned true (gHang.hung is set),
// and it writes Navi48,Ready = 0 on the Metal nub (sticky: never back to 1 this boot). Called with no hang lock held.
static void hang_announce(uint64_t emitted, uint64_t retired) {
    hang_log(emitted, retired);
    Navi48MetalNub::hungLatched();
}
static bool stall_handle(uint64_t em, uint64_t re);   // 0.0.627 (G2): below the ring
// Rule 6.3(b), on any GPU-dependent call: true when THIS call latched the hang. 0.0.627 (G2): with the switch ON a detected stall goes to stall_handle
// first (which may recover a non-WindowServer session instead of latching); with it OFF this is the 0.0.626 function.
static bool hang_poll() {
    const uint64_t em = emitted_now(), re = retired_now(em), now = now_ns();
    if (multi_on()) {
        bool expired = false;
        IOLockLock(gHangLock);
        if (!gHang.hung && __atomic_load_n(&gRecovering, __ATOMIC_SEQ_CST) == 0u) {
            hang_note(gHang, re, em, now);
            expired = hang_expired(gHang, re, em, now);
            if (expired) __atomic_store_n(&gRecovering, 1u, __ATOMIC_SEQ_CST);   // this call owns the stall
        }
        IOLockUnlock(gHangLock);
        return expired ? stall_handle(em, re) : false;
    }
    IOLockLock(gHangLock);
    const bool det = hang_detect(gHang, re, em, now);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
    return det;
}
// Rule 6.3(a): a bounded internal wait reached 2 s. Used as is for a TLB flush that did not acknowledge (never attributable to a session).
static void hang_from_wait() {
    const uint64_t em = emitted_now(), re = retired_now(em);
    IOLockLock(gHangLock);
    const bool det = hang_latch_wait(gHang);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
}
// 0.0.627 (G2): a wait for the RING (an own-seqno wait or the ring-space wait) reached its bound. Switch OFF: hang_from_wait, as 0.0.626 (returns kBoundStalled).
// Switch ON (0.0.629, G2 review fix R2): the 2 s of THAT wait blame nobody by themselves. The hang clock is noted and the stall goes to stall_handle ONLY when
// hang_expired (no change of the retired seqno for kHangNs, measured from the last observed change or idle kick, not from the start of the wait); otherwise
// kBoundNoBlame: the caller returns Timeout and nobody is reset or poisoned. kBoundBusy: HUNG is latched or a recovery already runs (the caller re-checks).
static uint32_t stall_from_wait() {
    if (!multi_on()) { hang_from_wait(); return n48native::g2::kBoundStalled; }
    const uint64_t em = emitted_now(), re = retired_now(em), now = now_ns();
    bool mine = false;
    IOLockLock(gHangLock);
    const bool busy = gHang.hung || __atomic_load_n(&gRecovering, __ATOMIC_SEQ_CST) != 0u;
    if (!busy) {
        hang_note(gHang, re, em, now);
        mine = hang_expired(gHang, re, em, now);   // R2: the hang threshold since the last progress, never the waiter's own 2 s
        if (mine) __atomic_store_n(&gRecovering, 1u, __ATOMIC_SEQ_CST);   // this call owns the stall
    }
    IOLockUnlock(gHangLock);
    if (mine) (void)stall_handle(em, re);
    return n48native::g2::bound_verdict(busy, mine);
}
// The prologue of every GPU-dependent call: Aborted after a latch (or for a poisoned session), Timeout for the call that detects it, else kOk.
static uint32_t gpu_gate(const Session *s) {
    if (hung_now()) return hang_rc(true, false);
    if (s != nullptr && sess_poisoned(s)) return n48native::g2::poison_rc(true);
    const uint32_t rc = hang_rc(false, hang_poll());
    if (rc == kOk && s != nullptr && sess_poisoned(s)) return n48native::g2::poison_rc(true);   // this very call's stall named the caller guilty
    return rc;
}
// 0.0.627 (G2): wait for retired >= THIS session's own last seqno (the ring runs in order, so that is sufficient), bounded at 2 s, plus up to one recovery's
// bound when the 2 s ran out (a stall that another session caused may be recovered meanwhile). Called with the session's own lock only. true = idle.
// false = not idle (already hung, this wait latched the hang, or the work did not retire): the caller leaks. 0.0.629 (R2): *noBlame (optional) is set when the
// 2 s ran out while the GPU was still making progress (switch ON only): nobody was blamed, the work is merely not retired yet (BoFree then keeps the BO).
constexpr uint64_t kRecoverWaitNs = 6000000000ull;   // RESET 2 s + ring space 2 s + verify 1 s + slack
static bool own_wait(Session *s, bool *noBlame = nullptr) {
    const uint64_t own = __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE);
    const uint64_t start = now_ns();
    bool stalled = false;
    if (noBlame != nullptr) *noBlame = false;
    for (;;) {
        if (n48native::g2::own_idle(retired_now(emitted_now()), own)) return true;
        if (hung_now()) return false;
        const uint64_t el = now_ns() - start;
        if (!stalled && el >= kWaitCapNs) {
            if (!multi_on()) { hang_from_wait(); return false; }   // switch OFF: exactly the 0.0.626 idle wait (latch, not idle)
            if (stall_from_wait() == n48native::g2::kBoundNoBlame) { if (noBlame != nullptr) *noBlame = true; return false; }   // R2: progress within kHangNs: Timeout, no blame
            stalled = true; continue;
        }
        if (stalled && el >= kWaitCapNs + kRecoverWaitNs) return false;
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
}

// ---- the ring -------------------------------------------------------------------------------------------------------------------
// Everything that goes on the ring goes through here: the space check, the write, the owner record, the emitted-seq update and the kick. Caller holds
// gRingLock. Returns kOk, or kTimeout when the space wait reached 2 s (the CALLER decides what that means, after dropping the lock).
static uint32_t ring_put_locked(const uint32_t *dw, uint32_t n, uint64_t seq, uint8_t ownerSlot, uint32_t ownerId, Session *owner) {
    DeviceContext &dev = *gCtx->dev;
    CPContext &cp = gCtx->cp;
    const uint32_t mask = cp.ring_ptr_mask;
    if (n > cp.ring_size_dwords / 2u) return kBadArg;
    const uint64_t t0 = now_ns();
    for (;;) {
        sysmem_rmb();
        const uint32_t r = *cp.rptr_cpu & mask;
        if (ring_has_space(ring_free64(r, gWc, mask), n)) break;
        const uint64_t el = now_ns() - t0;
        if (el >= kWaitCapNs) {
            N1C_LOG("ring space: need %u dwords (+%u slack), rptr %u wptr %u, no room in 2 s", n, kRingSlack, r, (uint32_t)(gWc & mask));
            return kTimeout;
        }
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    auto *ring = static_cast<uint32_t *>(cp.ring_cpu);
    const uint64_t startWc = gWc;
    uint64_t wc = gWc;
    for (uint32_t i = 0; i < n; i++) { ring[ring_idx(wc, mask)] = dw[i]; wc++; }
    gWc = wc;
    cp.wptr = ring_idx(wc, mask);
    // The clock for rule 6.3(b) starts at the kick that makes an idle ring busy; emitted goes up BEFORE the doorbell so retired can never
    // be observed above emitted. 0.0.627: the owner is recorded before the seqno is published.
    const uint64_t emBefore = emitted_now();
    const uint64_t reNow = retired_now(emBefore);
    IOLockLock(gHangLock);
    hang_kick(gHang, reNow, emBefore, now_ns());
    IOLockUnlock(gHangLock);
    n48native::g2::owner_note(gOwners, seq, ownerSlot, ownerId, startWc);
    if (owner != nullptr) __atomic_store_n(&owner->emitted, seq, __ATOMIC_RELEASE);
    __atomic_store_n(&gEmitted, seq, __ATOMIC_RELEASE);
    amdgpu_hdp_flush(dev);
    const uint64_t wptr64 = wc;   // the FULL 64-bit counter, never masked
    *reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(cp.wb_cpu) + kCPWBOffsetWptr) = wptr64;
    sysmem_wmb();
    const uint64_t off = (uint64_t)cp.doorbell_index * kCPDoorbellStride;
    WDOORBELL64(dev, off, wptr64);
    (void)RDOORBELL32(dev, off);
    return kOk;
}
// One submission of session `s` (its VMID): the seqno is assigned, the packet built and put on the ring under ONE hold of gRingLock, so two sessions can
// never take the same seqno. Returns kOk (*seqOut set), BadArg / InternalError for an unusable packet, or Timeout when the space wait reached 2 s (that
// latches HUNG, or with the switch ON goes to stall_handle, AFTER the ring lock is dropped and before anything was written).
static uint32_t ring_submit(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    IOLockLock(gRingLock);
    const uint64_t seq = emitted_now() + 1ull;
    const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, s->vmid, hasFence,
                                    fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);
    if (n == 0u) { IOLockUnlock(gRingLock); return (uint32_t)kIOReturnInternalError; }
    const uint32_t er = ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);
    IOLockUnlock(gRingLock);
    if (er == kTimeout) { (void)stall_from_wait(); return kTimeout; }   // 0.0.629 (R2): blames only when the hang clock expired; Timeout either way
    if (er != kOk) return er;
    *seqOut = seq;
    return kOk;
}

// 0.0.650: the verdict's inputs, read under ONE hold of gRingLock (the caller holds it): the ring space, the retired / emitted seqnos, WindowServer's id and last submit, the
// waiters' clocks. A separate function and a file-scope static keep ring_submit_app_g5's stack frame small.
__attribute__((noinline)) static void g5_fill_in_locked(const Session *s, uint32_t K, uint32_t need) {
    const uint64_t now = now_ns();
    sysmem_rmb();
    const uint32_t mask = gCtx->cp.ring_ptr_mask;
    const uint32_t rp = *gCtx->cp.rptr_cpu & mask;
    const uint64_t em = emitted_now(), re = retired_now(em);
    const uint32_t wsId = __atomic_load_n(&gWsId, __ATOMIC_SEQ_CST);
    gG5In = n48native::g5::AdmitIn{ true, false, wsId != 0u, __atomic_load_n(&gWsLastNs, __ATOMIC_ACQUIRE), now, K, re, em, wsId, ring_free64(rp, gWc, mask), need, gG5Since, s->slot };
}
// 0.0.650 (G5 Stage 1; navi48-g5=1 only): the Submit of a session that is NOT WindowServer. Called with ONLY the session's own lock held (the lock order of own_wait).
// The admission verdict (n48native::g5::admission: ring reserve, the credit while WindowServer is active, first come first served) and the seqno are taken in ONE hold of
// gRingLock; the wait for a Wait verdict runs OUTSIDE it (no sleep under the lock), bounded by kG5WaitBoundNs. Every exit clears this slot's gG5Since entry.
static uint32_t ring_submit_app_g5(Session *s, const uint64_t *va, const uint32_t *by, uint32_t nIbs, bool hasFence, uint64_t fenceAddr, uint64_t *seqOut) {
    using namespace n48native::g5;
    const uint32_t need = submit_dwords(nIbs, hasFence);
    const uint32_t K = g5_credit();
    uint32_t rc = kOk;
    bool waiting = false, stallDone = false, timedOut = false;
    uint64_t t0 = 0;
    for (;;) {
        IOLockLock(gRingLock);
        g5_fill_in_locked(s, K, need);
        const uint64_t now = gG5In.now;
        if (admission(gOwners, gG5In) == kAdmit) {
            const uint64_t waitedNs = gG5Since[s->slot] != 0ull && now > gG5Since[s->slot] ? now - gG5Since[s->slot] : 0ull;
            since_clear(gG5Since, s->slot); waiting = false;
            const uint64_t seq = emitted_now() + 1ull;
            const uint32_t n = build_submit(gSubmitBuf, (uint32_t)(sizeof(gSubmitBuf) / sizeof(gSubmitBuf[0])), va, by, nIbs, s->vmid, hasFence,
                                            fenceAddr, gCtx->cp.wb_bus + kWbOffsetNativeSeq, seq);
            if (n == 0u) { IOLockUnlock(gRingLock); rc = (uint32_t)kIOReturnInternalError; break; }
            const uint32_t er = ring_put_locked(gSubmitBuf, n, seq, (uint8_t)s->slot, s->id, s);
            if (er == kOk) stat_admit(gG5, waitedNs);
            IOLockUnlock(gRingLock);
            if (er == kTimeout) { (void)stall_from_wait(); rc = kTimeout; break; }   // as ring_submit (R2): blames only when the hang clock expired
            if (er != kOk) { rc = er; break; }
            *seqOut = seq;
            break;
        }
        since_mark(gG5Since, s->slot, now); waiting = true; t0 = gG5Since[s->slot];
        IOLockUnlock(gRingLock);
        if (hung_now()) { rc = hang_rc(true, false); break; }
        if (sess_poisoned(s)) { rc = n48native::g2::poison_rc(true); break; }
        const uint64_t el = now_ns() - t0;
        const uint32_t step = wait_step(el, stallDone);
        if (step == kGiveUp) { timedOut = true; rc = kTimeout; break; }   // the app gets DEVICE_LOST, but only after a stall that recovery did not clear
        if (step == kStallNow) { stallDone = true; (void)stall_from_wait(); continue; }   // kBoundNoBlame / Busy: keep waiting
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    if (waiting) {   // the single exit of every path that left while still waiting
        IOLockLock(gRingLock);
        since_clear(gG5Since, s->slot);
        if (timedOut) stat_timeout(gG5);
        IOLockUnlock(gRingLock);
    }
    return rc;
}

// ---- the automatic recovery (0.0.627, G2; switch ON only) ----------------------------------------------------------------------------
// NOP every ring dword outside the live range [liveStart, gWc) - the G1 scrub rule (the older dwords have run; the free space must not hold stale
// packets if the RESET moves the read pointer). Caller holds gRingLock.
static void g2_scrub_locked(uint64_t liveStart) {
    CPContext &cp = gCtx->cp;
    const uint32_t mask = cp.ring_ptr_mask;
    auto *ring = static_cast<volatile uint32_t *>(cp.ring_cpu);
    uint32_t n = 0;
    if (gWc - liveStart <= (uint64_t)cp.ring_size_dwords) {
        for (uint32_t i = 0; i < cp.ring_size_dwords; i++)
            if (!n48native::g1::in_live_range(i, liveStart, gWc, mask)) { ring[i] = cp_p3_nop1(); n++; }
    }
    sysmem_wmb();
    N1C_LOG("g2 recovery: ring scrub: %u of %u dwords outside the live range [%llu, %llu) set to NOP", n, cp.ring_size_dwords, (unsigned long long)liveStart, (unsigned long long)gWc);
}
// The G1 recovery (MES RESET reset_legacy_gfx, proven by G1 on this firmware: notes), run on the kernel's own initiative for a
// guilty NON-WindowServer session, then proven by a kernel-only probe fence (one RELEASE_MEM of the next seqno: no IB, no VMID). gRingLock is held for the
// whole run, so no session emits meanwhile. The guilty session is poisoned BEFORE the RESET (its seqno can retire only after it, so its waiter sees the
// poison first). Returns recovery_ok.
static bool g2_recover(const n48native::g2::Guilty &g, Session *gs) {
    using namespace n48native::g2;
    DeviceContext &dev = *gCtx->dev;
    CPContext &cp = gCtx->cp;
    IOLockLock(gRingLock);
    const uint64_t emAt = emitted_now();
    g2_scrub_locked(g.startWc);
    __atomic_store_n(&gs->poisoned, 1u, __ATOMIC_SEQ_CST);
    const uint64_t t0 = now_ns();
    const kern_return_t r = mes_reset_legacy_gfx_queue(dev, gCtx->mes, 0u, 0u, gs->vmid, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);
    const uint64_t actNs = now_ns() - t0;
    bool put = false, ret = false;
    uint64_t probe = 0, verifyNs = 0;
    if (r == kIOReturnSuccess) {
        uint32_t dw[8];
        probe = emAt + 1ull;
        (void)emit_release_mem(dw, cp.wb_bus + kWbOffsetNativeSeq, probe);
        put = ring_put_locked(dw, 8u, probe, kOwnerKernel, 0u, nullptr) == kOk;
        const uint64_t t1 = now_ns();
        while (put && !ret) {
            ret = retired_now(emitted_now()) >= probe;
            verifyNs = now_ns() - t1;
            if (!ret && verifyNs >= kVerifyNs) break;
            if (!ret) { if (verifyNs < 5000000ull) IODelay(1); else IOSleep(1); }
        }
    }
    IOLockUnlock(gRingLock);
    const bool ok = recovery_ok(r == kIOReturnSuccess, put, ret);
    if (ok) { gG2.recoveries++; gG2.lastGuiltySlot = g.slot; gG2.lastGuiltySeq = g.seq; gG2.collateralTotal += collateral(g.seq, emAt); }
    else gG2.failed++;
    N1C_LOG("g2 recovery: MES RESET(reset_legacy_gfx, vmid %u) %s (%#x, %llu us); probe seq %llu %s (%llu us); guilty slot %u session %u seq %llu POISONED; "
            "%llu later submission(s) of other sessions were queued behind it (executed or dropped: not known): %s", gs->vmid, r == kIOReturnSuccess ? "ACKED" : "NOT acked", r,
            (unsigned long long)(actNs / 1000ull), (unsigned long long)probe, ret ? "RETIRED" : put ? "did NOT retire" : "not emitted", (unsigned long long)(verifyNs / 1000ull),
            g.slot, g.sessId, (unsigned long long)g.seq, (unsigned long long)collateral(g.seq, emAt), ok ? "RECOVERED, the other sessions continue" : "FAILED: latching HUNG");
    return ok;
}
// A stall was detected and THIS call owns it (it set gRecovering). The owner of the oldest unretired seqno is the guilty session; a non-WindowServer user
// session is recovered (switch ON), everything else latches HUNG exactly as before. Clears gRecovering. Returns true when THIS call latched HUNG.
// Called with at most the caller's own session lock (never gCliLock / gRingLock).
static bool stall_handle(uint64_t em, uint64_t re) {
    using namespace n48native::g2;
    const Guilty g = guilty_of(gOwners, re, em);
    Session *gs = (g.known && g.slot < kMaxSessions) ? slot_ptr(g.slot) : nullptr;
    const bool user = gs != nullptr && __atomic_load_n(&gs->open, __ATOMIC_ACQUIRE) && __atomic_load_n(&gs->id, __ATOMIC_ACQUIRE) == g.sessId &&
                      __atomic_load_n(&gs->hello, __ATOMIC_ACQUIRE);
    const bool ws = user && __atomic_load_n(&gs->ws, __ATOMIC_ACQUIRE);
    const uint64_t tried = gG2.recoveries + gG2.failed;   // 0.0.629 (R3): attempts this boot (gRecovering, held by this call, serialises the counters)
    const uint32_t act = stall_action_budgeted(multi_on(), g.known, g.slot, user, ws, mes_timeout_seen(), g1_holds(), tried);
    N1C_LOG("stall: seq emitted %llu retired %llu; the oldest unretired seq %llu belongs to %s (slot %u, session %u%s): %s", (unsigned long long)em, (unsigned long long)re,
            (unsigned long long)g.seq, !g.known ? "an unknown owner" : g.slot == kOwnerKernel ? "the kernel" : user ? "a user session" : "a non-user context", g.slot, g.sessId,
            ws ? ", WindowServer" : "", act == kStallRecover ? "recovering that session alone" : !recovery_budget_left(tried) ? "latching HUNG (the automatic-recovery budget of this boot is spent)" : "latching HUNG (today's behaviour)");
    if (act == kStallRecover && __atomic_load_n(&gs->app, __ATOMIC_ACQUIRE)) {   // 0.0.641: one strike against the executable name of an APP session an automatic recovery is blamed on (gs->app is set only with apps ON)
        const uint32_t strikes = n48native::g4::strike_add(gStrikes, gs->appKey);
        N1C_LOG("stall: strike %u of %u for the application key %#llx (session %u)%s", strikes, n48native::g4::kStrikeLimit, (unsigned long long)gs->appKey, g.sessId,
                n48native::g4::strike_refused(strikes) ? ": that name is now REFUSED at open for the rest of this boot" : "");
    }
    if (act == kStallRecover && g2_recover(g, gs)) {
        const uint64_t em2 = emitted_now(), re2 = retired_now(em2);
        IOLockLock(gHangLock);
        gHang.lastRetired = re2; gHang.tProgress = now_ns();
        __atomic_store_n(&gRecovering, 0u, __ATOMIC_SEQ_CST);
        IOLockUnlock(gHangLock);
        return false;
    }
    IOLockLock(gHangLock);
    const bool det = hang_latch_wait(gHang);
    __atomic_store_n(&gRecovering, 0u, __ATOMIC_SEQ_CST);
    IOLockUnlock(gHangLock);
    if (det) hang_announce(em, re);
    return det;
}

// ---- the page tables ------------------------------------------------------------------------------------------------------------
struct PtMem {
    Session *s;   // 0.0.627: the session whose tree (and page-table cap) this is
    uint64_t rd(uint64_t pa, uint32_t idx) { return RBAR0_64(*gCtx->dev, pa - gCtx->gmc.vram_base_offset + (uint64_t)idx * 8u); }
    void wr(uint64_t pa, uint32_t idx, uint64_t v) { WBAR0_64(*gCtx->dev, pa - gCtx->gmc.vram_base_offset + (uint64_t)idx * 8u, v); }
    // A zeroed 4 KiB table page from the session's arena (16 KiB blocks of the visible pool: the pool's minimum allocation), or 0 at the cap.
    // 0.0.627: the reserve and vram_alloc are shared: the caller holds gCliLock.
    uint64_t alloc_page() {
        if (s->pagesInBlk >= 4u) {
            if (s->nblk >= kArenaBlocks || would_exceed((uint64_t)s->nblk * kArenaBlockBytes, kArenaBlockBytes, kPtCap)) return 0ull;
            uint32_t idx;
            if (gPtFreeN > 0) idx = gPtFree[--gPtFreeN];
            else {
                if (gPtN >= kArenaReserve) return 0ull;
                VRAMAllocation a {};
                if (!gCtx->gmc.vram_alloc.alloc(kArenaBlockBytes, kArenaBlockBytes, &a)) return 0ull;
                const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
                if (a.size < kArenaBlockBytes || off + a.size > gCtx->dev->bar0Size) { gCtx->gmc.vram_alloc.free(a); return 0ull; }
                gPt[gPtN] = a;
                idx = gPtN++;
            }
            bar0_memset_vram(*gCtx->dev, gPt[idx].gpu_va - gCtx->gmc.vram_start, 0, kArenaBlockBytes);   // a reused block is zeroed again
            s->blk[s->nblk++] = (uint16_t)idx;
            s->pagesInBlk = 0;
        }
        const uint64_t pa = pa_of_mc(gPt[s->blk[s->nblk - 1]].gpu_va) + (uint64_t)s->pagesInBlk * kPage;
        s->pagesInBlk++;
        s->ptPages++;
        return pa;
    }
};

// Return a session's page-table blocks to the reserve (never to vram_alloc). Caller holds gCliLock.
static void pt_release_blocks(Session *s) {
    for (uint32_t i = 0; i < s->nblk; i++) gPtFree[gPtFreeN++] = s->blk[i];
    s->nblk = 0;
}
static uint32_t flush_vmid(uint32_t vmid) {
    if (gTlbLock != nullptr) IOLockLock(gTlbLock);
    const kern_return_t r = gmc_flush_gpu_tlb(*gCtx->dev, gCtx->gmc, gCtx->gmc.gfxhub, vmid, 0);
    if (gTlbLock != nullptr) IOLockUnlock(gTlbLock);
    if (r != kIOReturnSuccess) {
        N1C_LOG("TLB flush for VMID %u did not acknowledge (kr=%#x): treating as a hang", vmid, r);
        hang_from_wait();
        return kTimeout;
    }
    return kOk;
}
// The CONTEXTn register addresses of a session VMID (the same arithmetic as native_s1b.cpp, which programs CONTEXT8).
struct CtxRegs { uint32_t cntl, baseLo, baseHi, stLo, stHi, enLo, enHi; };
static CtxRegs ctx_regs(uint32_t vmid) {
    const HubContext &h = gCtx->gmc.gfxhub;
    DeviceContext &dev = *gCtx->dev;
    CtxRegs r;
    r.cntl   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_cntl) + (vmid - 1u) * h.ctx_distance;
    r.baseLo = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_lo) + vmid * h.ctx_addr_distance;
    r.baseHi = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx0_pt_base_hi) + vmid * h.ctx_addr_distance;
    r.stLo   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_lo) + (vmid - 1u) * h.ctx_addr_distance;
    r.stHi   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_start_hi) + (vmid - 1u) * h.ctx_addr_distance;
    r.enLo   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_lo) + (vmid - 1u) * h.ctx_addr_distance;
    r.enHi   = SOC15_REG_OFFSET_BIDX(dev, h.ip, h.base_idx, h.ctx1_pt_end_hi) + (vmid - 1u) * h.ctx_addr_distance;
    return r;
}
// DEPTH 3 / BLOCK 0 / RETRY 0 with the context enabled (the S1b geometry).
static inline bool ctx_geometry_ok(uint32_t cntl) {
    return (cntl & kCntlEnableMask) != 0u && ((cntl & kCntlDepthMask) >> kCntlDepthShift) == kCtxDepth && (cntl & kCntlRetryMask) == 0u &&
           ((cntl & kCntlBlockMask) >> kCntlBlockShift) == kCtxBlockSize;
}
// Point CONTEXT<vmid> at `rootPa` (VALID set, START 0, END max_pfn - 1) and flush that VMID's TLB. Caller holds gCliLock.
static uint32_t program_root(uint32_t vmid, uint64_t rootPa) {
    DeviceContext &dev = *gCtx->dev;
    const CtxRegs r = ctx_regs(vmid);
    const uint64_t root = rootPa | kPteValid;
    WREG32(dev, r.baseLo, (uint32_t)(root & 0xFFFFFFFFu));
    WREG32(dev, r.baseHi, (uint32_t)(root >> 32));
    WREG32(dev, r.stLo, 0u);
    WREG32(dev, r.stHi, 0u);
    WREG32(dev, r.enLo, (uint32_t)((kMaxPfn - 1u) & 0xFFFFFFFFu));
    WREG32(dev, r.enHi, (uint32_t)((kMaxPfn - 1u) >> 32));
    return flush_vmid(vmid);
}

// ---- open / close -------------------------------------------------------------------------------------------------------------------
// 0.0.627 (G2): n1c_open's helpers, out of line so its frame stays within the stack rule (+0x40).
// CONTEXT<vmid> must carry the S1b geometry (DEPTH 3 / BLOCK 0 / RETRY 0): a context that lost it must not be pointed at a tree.
// VMIDs 9..11 (slots 1..3, switch ON only) still carry the boot geometry with RETRY 1 (hub_setup_vmid_config): their CNTL gets cntl_native exactly as
// native_s1b.cpp set CONTEXT8, then is read back. CONTEXT8 itself is never written here (the S1b self-test owns it). Returns the CNTL read; *ok = usable.
static __attribute__((noinline)) uint32_t ctx_prepare(uint32_t slot, uint32_t vmid, bool *ok) {
    DeviceContext &dev = *gCtx->dev;
    const CtxRegs r = ctx_regs(vmid);
    uint32_t cntl = RREG32(dev, r.cntl);
    if (slot != 0u && !ctx_geometry_ok(cntl)) {
        const uint32_t before = cntl;
        WREG32(dev, r.cntl, cntl_native(before));
        cntl = RREG32(dev, r.cntl);
        N1C_LOG("CONTEXT%u CNTL %#010x -> %#010x (cntl_native: DEPTH 3 / BLOCK 0 / RETRY 0, as S1b set CONTEXT8)", vmid, before, cntl);
    }
    *ok = ctx_geometry_ok(cntl);
    if (!*ok) N1C_LOG("open refused (NotReady): CONTEXT%u CNTL %#010x is not the native geometry (DEPTH 3 / BLOCK 0 / RETRY 0)", vmid, cntl);
    return cntl;
}
// 0.0.629 (G2 review fix R5): READ the GCVM L2 protection-fault registers of the GFXHUB - STATUS_LO32 0x15d0, STATUS_HI32 0x15d1, ADDR_LO32 0x15d2, ADDR_HI32
// 0x15d3, the four native_s1b.cpp's fault leg reads - log them, and return n48native::g2::fault_word(STATUS_LO32, ADDR_LO32). NEVER written or cleared here (the
// S1b self-test's VMID-8 fault stays latched: a new fault is told only by a CHANGE of this word). Register reads only.
static __attribute__((noinline)) uint64_t fault_snap(const char *when, uint32_t slot) {
    const uint32_t sLo = hub_rd(0x15d0), sHi = hub_rd(0x15d1), aLo = hub_rd(0x15d2), aHi = hub_rd(0x15d3);
    N1C_LOG("gcvm fault (%s, slot %u): STATUS_LO32 %#010x (vmid %u) HI32 %#010x ADDR_LO32 %#010x HI32 %#010x (read only, never cleared)", when, slot, sLo,
            n48native::g2::fault_word_vmid(n48native::g2::fault_word(sLo, aLo)), sHi, aLo, aHi);
    return n48native::g2::fault_word(sLo, aLo);
}
static __attribute__((noinline)) void open_refused_log(bool multi, bool ws, uint32_t openNow, bool apps, bool wsOpen) {
    N1C_LOG("open refused (ExclusiveAccess): %s (%u session(s) open, switch %s)", !multi ? "the session slot is not free" : g1_holds() ? "a G1 test context holds the GPU" :
            apps ? (ws && wsOpen) ? "a WindowServer session is already open" : (!ws && !wsOpen) ? "the last free slot is reserved for WindowServer" : "every session slot is in use or DEAD" :
            (ws && openNow != 0u) ? "a WindowServer client opens only when no session is open" : "every session slot is in use or DEAD", openNow, multi ? "ON" : "OFF");
}
static __attribute__((noinline)) void opened_log(const Session *s, uint32_t cntl, uint64_t winMc) {
    const CPContext &cp = gCtx->cp;
    N1C_LOG("client opened: slot %u VMID %u (session %u%s), fresh root pa %#llx (visible pool), CONTEXT%u CNTL %#010x, ring %u dwords rptr %u wptr %u, fence window mc %#llx",
            s->slot, s->vmid, s->id, s->ws ? ", WindowServer" : "", (unsigned long long)s->rootPa, s->vmid, cntl, cp.ring_size_dwords, *cp.rptr_cpu, cp.wptr, (unsigned long long)winMc);
}
IOReturn n1c_open(BringupContext &ctx, bool ws, N1cRef *out) {
    using namespace n48native::g2;
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
    // (1b) 0.0.663 (ReBAR item 5): BAR0 as IOPCIFamily sees it and as config space / the ReBAR capability describe it must agree, or the host-import guard's BAR list cannot be trusted.
    if (__atomic_load_n(&gBar0Conflict, __ATOMIC_ACQUIRE) == 2u) {
        N1C_LOG("open refused (NotReady): IOPCIFamily and config space disagree on BAR0 (see the 'BAR0 latch' and 'pci bars: BAR0 union' lines)");
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
    // (4) exclusivity. Switch OFF: exactly one client, by this compare-and-swap (0.0.626). Switch ON (0.0.627, G2): open_pick under the client lock below.
    const bool multi = multi_on();
    if (!multi && !OSCompareAndSwap(0, 1, &gOpenFlag)) {
        N1C_LOG("open refused (ExclusiveAccess): a native client is already open");
        return kIOReturnExclusiveAccess;
    }
    lazy_lock(&gCliLock); lazy_lock(&gRingLock); lazy_lock(&gHangLock); lazy_lock(&gTlbLock);
    if (!gCliLock || !gRingLock || !gHangLock || !gTlbLock) { if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag); return kIOReturnNoMemory; }
    gCtx = &ctx;

    // The whole open runs under the client lock so a racing close cannot see a half-built session.
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    Session *s = nullptr;
    do {
        CPContext &cp = ctx.cp;
        const uint32_t openNow = multi ? __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) : 0u;
        const bool appsNow = apps_on(), wsOpen = __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;   // 0.0.641: apps ON = a WindowServer slot is always available and the last free slot is its
        const OpenPick pk = open_pick(multi, gSlotState, openNow, ws, g1_holds(), appsNow, wsOpen);
        if (pk.rc != kOk) { open_refused_log(multi, ws, openNow, appsNow, wsOpen); rc = kIOReturnExclusiveAccess; break; }
        const uint32_t slot = pk.slot, vmid = vmid_of_slot(slot);
        // The ring must be idle for the FIRST session: S1b's (or the last session's) work has retired and the seqno restarts. 0.0.627: a later session
        // (switch ON) opens next to the running ones.
        if (openNow == 0u) {
            const uint64_t t0 = now_ns();
            for (;;) {
                sysmem_rmb();
                if ((*cp.rptr_cpu & cp.ring_ptr_mask) == cp.wptr) break;
                if (now_ns() - t0 >= kWaitCapNs) { N1C_LOG("open refused (NotReady): the kernel ring is not idle (rptr %u wptr %u)", *cp.rptr_cpu, cp.wptr); rc = kIOReturnNotReady; break; }
                IOSleep(1);
            }
        }
        if (rc != kIOReturnSuccess) break;
        bool ctxOk = false;
        const uint32_t cntl = ctx_prepare(slot, vmid, &ctxOk);   // CONTEXT<vmid>: the S1b geometry, set once for VMIDs 9..11, read back
        if (!ctxOk) { rc = kIOReturnNotReady; break; }
        // The slot's fence window: 16 GART pages reserved once per boot from the same bump allocator the ring, MQD and write-back page came from.
        FenceWindow &win = gWin[slot];
        if (!win.reserved) {
            GMCContext &gmc = ctx.gmc;
            const uint64_t bytes = (uint64_t)N48N_FENCE_SLOTS * kPage;
            if (gmc.gart_size == 0 || gmc.gart_pt_bus == 0 || gmc.gart_bump_offset + bytes > gmc.gart_size) {
                N1C_LOG("open refused (NoMemory): no GART room for the fence window"); rc = kIOReturnNoMemory; break;
            }
            win.gartOff = gmc.gart_bump_offset;
            gmc.gart_bump_offset += bytes;
            win.mc = gmc.gart_start + win.gartOff;
            win.reserved = true;
            const uint64_t zero = 0;
            for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++)
                bar0_memcpy_to_vram(*dev, gmc.gart_pt_vram_offset + (win.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
            amdgpu_hdp_flush(*dev);
            N1C_LOG("fence window reserved: %u GART pages at mc %#llx (offset %#llx), every PTE invalid until a GTT fence target binds it",
                    N48N_FENCE_SLOTS, (unsigned long long)win.mc, (unsigned long long)win.gartOff);
        }
        // The park root: a kernel-owned zeroed page a context is pointed at while no client owns a tree.
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
        // The session record. Slot 0 is the static gSess; slots 1..3 are allocated once (switch ON) and never freed. Its lock is kept across opens.
        if (gSlot[slot] == nullptr) {
            Session *ns = static_cast<Session *>(IOMalloc(sizeof(Session)));
            if (ns == nullptr) { rc = kIOReturnNoMemory; break; }
            bzero(ns, sizeof(Session));
            __atomic_store_n(&gSlot[slot], ns, __ATOMIC_RELEASE);
        }
        s = gSlot[slot];
        // 0.0.629 (G2 review fix R1): the record is wiped WITHOUT its lock field. A closing thread may still hold (and is about to release) that lock: it
        // was never NULL once created, and it is created exactly once per slot, here, before the slot's first open publishes the session.
        if (s->lock == nullptr) {
            IOLock *nl = IOLockAlloc();
            if (nl == nullptr) { s = nullptr; rc = kIOReturnNoMemory; break; }
            __atomic_store_n(&s->lock, nl, __ATOMIC_RELEASE);
        }
        n48native::g2::sess_wipe_keep_lock(s);
        s->slot = slot; s->vmid = vmid; s->ws = ws;
        s->faultAtOpen = fault_snap("open", slot);   // R5: read only
        s->pagesInBlk = 4u;   // forces the first block
        PtMem mem{ s };
        s->rootPa = mem.alloc_page();
        if (s->rootPa == 0ull) { N1C_LOG("open refused (NoMemory): no VRAM for the root table"); rc = kIOReturnNoMemory; break; }
        amdgpu_hdp_flush(*dev);
        if (openNow == 0u) {
            // The first session: zero the native seqno slot while the ring is idle, restart the count, and start the clock.
            *seq_slot() = 0ull;
            sysmem_wmb();
            __atomic_store_n(&gEmitted, 0ull, __ATOMIC_RELEASE);
            gHang.lastRetired = 0; gHang.tProgress = now_ns();
        }
        if (program_root(vmid, s->rootPa) != kOk) { rc = kIOReturnNotReady; break; }
        __atomic_store_n(&s->id, ++gSessSeqCounter, __ATOMIC_SEQ_CST);   // 0.0.609
        __atomic_store_n(&s->open, true, __ATOMIC_RELEASE);
        gSlotState[slot] = kSlotOpen;
        if (multi) __atomic_add_fetch(&gOpenFlag, 1u, __ATOMIC_SEQ_CST);
        if (ws) __atomic_store_n(&gSessWs, 1u, __ATOMIC_SEQ_CST);   // 0.0.623 (G1) note, 0.0.627: set with the open itself
        if (ws) __atomic_store_n(&gWsId, s->id, __ATOMIC_SEQ_CST);   // 0.0.650 (G5): WindowServer is found by its session id, never by its slot
        if (out != nullptr) { out->slot = slot; out->id = s->id; }
        opened_log(s, cntl, win.mc);
    } while (false);
    if (rc != kIOReturnSuccess) {
        if (s != nullptr) pt_release_blocks(s);
        if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag);
    }
    IOLockUnlock(gCliLock);
    return rc;
}

enum RelMode : uint32_t { kRelNormal = 0, kRelLeak = 1, kRelClosing = 2 };
static_assert(kRelNormal == kHostRelNormal && kRelLeak == kHostRelLeak && kRelClosing == kHostRelClosing, "the pure header's release modes are RelMode");
static void bo_release(Session *s, uint32_t h, RelMode mode);

// ---- the scanout pins (0.0.603) -------------------------------------------------------------------------------------------------
// A BO that hosts scanout slots (ScanoutRegister) carries pinMask / pinGen, written ONLY under gCliLock. The console plane goes back BEFORE
// such a BO is freed or leaked, on every path: BoFree, the HUNG leak, and the close of the whole session (lock order gCliLock -> the DCN lock).
// 0.0.661 (M6 Stage 1b): BOTH instances are restored (instance 0, then instance 2's A) before any BO is touched: n48scanx::teardown_both (host-tested with a call recorder) owns the order.
// 0.0.662 (M6 Stage 2): ALL THREE (instance 0, the monitor A's A, the monitor B's A): n48scanx::teardown_all.
struct TeardownEnv {
    Session *s;
    void restore0(const char *how, uint64_t r[2]) { (void)n48dcn::scanRelease(how, r); }
    void restore1(const char *how, uint64_t r[2]) { (void)n48dcn::scanXRelease(1u, how, r); }   // 0.0.662: inert (no register, no lock) with navi48-m6flip1 (or a Stage-1b latch) OFF
    void restore2(const char *how, uint64_t r[2]) { (void)n48dcn::scanXRelease(2u, how, r); }   // inert (no register, no lock) with a Stage-1b latch OFF
    uint32_t nbo() { return N48N_MAX_BOS; }
    bool used(uint32_t h) { return s->boUsed[h]; }
    uint32_t pin0(uint32_t h) { return s->bo[h].pinMask; }
    uint32_t pin1(uint32_t h) { return s->bo[h].pinMask1; }
    uint32_t pin2(uint32_t h) { return s->bo[h].pinMask2; }
    void mark_leak(uint32_t h) { s->bo[h].pinLeak = 1u; }
    void clear_pins(uint32_t h) { s->bo[h].pinMask = 0; s->bo[h].pinMask1 = 0; s->bo[h].pinMask2 = 0; }
};
static void scan_teardown(Session *s, const char *how, uint64_t r[2]) {
    r[0] = 1; r[1] = 0;
    uint64_t r1[2] = { 1, 0 }, r2[2] = { 1, 0 };
    TeardownEnv te { s };
    n48scanx::teardown_all(te, how, r, r1, r2);
    if (r[0] == 0ull) N1C_LOG("scanout: the console restore at %s was NOT verified (plane %#llx): run `navi48test accel dcnflip 0`, reboot if it persists", how, (unsigned long long)r[1]);
    if (r1[0] == 0ull) N1C_LOG("scanx: the restore of instance 1's (the monitor A's) A at %s was NOT verified (plane %#llx): its BOs are LEAKED, reboot if it persists", how, (unsigned long long)r1[1]);
    if (r2[0] == 0ull) N1C_LOG("scanx: the restore of instance 2's A at %s was NOT verified (plane %#llx): its BOs are LEAKED, reboot if it persists", how, (unsigned long long)r2[1]);
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
// 0.0.661 / 0.0.662: the same for an HDMI instance's slots (pinMask1 / pinMask2): n48scanx::bo_gone, judged with THAT instance's EARLIEST and generation. true = A is not verified back: LEAK the BO.
static bool scan_unpinx(uint32_t inst, uint32_t h, Bo &b, RelMode mode) {
    uint8_t &mask = inst == 1u ? b.pinMask1 : b.pinMask2;
    const uint32_t pgen = inst == 1u ? b.pinGen1 : b.pinGen2;
    uint64_t r[2] = { 1, 0 };
    const uint32_t res = n48dcn::scanXBoGone(inst, mask, pgen, mode != kRelNormal, r);
    if (res != 0u) N1C_LOG("BO %u hosted instance-%u slot(s) %#x and is going away (%s): the restore of A %s", h, (unsigned)inst, (unsigned)mask,
                           mode == kRelNormal ? "BoFree while shown or pending" : "leak / close", res == 1u ? "ran FIRST and VERIFIED" : "did NOT verify: the BO is LEAKED, not freed");
    mask = 0;
    return res == 2u;
}
static bool scan_unpin1(uint32_t h, Bo &b, RelMode mode) { return scan_unpinx(1u, h, b, mode); }
static bool scan_unpin2(uint32_t h, Bo &b, RelMode mode) { return scan_unpinx(2u, h, b, mode); }

void n1c_close(const N1cRef &ref, const char *how) {
    if (gCliLock == nullptr) return;
    Session *s = sess_lock(ref, false);
    if (s == nullptr) return;
    IOLock *const sl = s->lock;   // 0.0.629 (G2 review fix R1): the lock this thread holds, kept LOCAL: after the last gCliLock below the record may be re-opened
    using namespace n48native::g2;
    const bool multi = multi_on();
    DeviceContext &dev = *gCtx->dev;
    IOLockLock(gCliLock);
    // 0.0.603: the console plane FIRST - before the own wait, before the HUNG decision, before anything is freed or leaked. 0.0.627 (G2): with the switch ON only the
    // session that holds the plane restores it (another client's close never takes WindowServer's plane away).
    if (!multi || __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id) { uint64_t tr[2]; scan_teardown(s, how, tr); if (multi) __atomic_store_n(&gScanOwner, 0u, __ATOMIC_RELEASE); }
    n48dcn::modeHoldSessionClosed(s->id);   // 0.0.609: a row-120 hold owned by this session ends (or, HANDOFF, passes on) - AFTER the plane is back; atomic words only
    IOLockUnlock(gCliLock);
    bool leak = hung_now();
    if (!leak && !own_wait(s)) leak = true;            // a timeout here latched the hang (switch ON: or left this slot's work unproven)
    const bool ownIdle = !leak;
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
    IOLockLock(gCliLock);
    bool parked = false;
    if (!leak) {
        // No client owns this tree from here: park the context on the kernel's zeroed page, then free everything.
        if (program_root(s->vmid, gPark.pa) != kOk) leak = true; else parked = true;
    }
    if (!leak) {
        for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) bo_release(s, h, kRelClosing);
        bool slots = false;
        const FenceWindow &win = gWin[s->slot];
        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) {
            if (!s->fslot[i].used) continue;
            const uint64_t zero = 0;
            bar0_memcpy_to_vram(dev, gCtx->gmc.gart_pt_vram_offset + (win.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
            s->fslot[i].used = 0; slots = true;
        }
        if (slots) { amdgpu_hdp_flush(dev); (void)flush_vmid(0); }
        pt_release_blocks(s);   // into the reserve, not vram_alloc
        N1C_LOG("client closed (%s): slot %u VMID %u: %u BOs (%llu MiB VRAM, %llu MiB GTT), %u PT pages, %llu seqs, freed", how, s->slot, s->vmid, nBos,
                (unsigned long long)(vramB >> 20), (unsigned long long)(gttB >> 20), ptPages, (unsigned long long)seqs);
        if (nHost != 0u) N1C_LOG("client closed (%s): %u host imports (%llu KiB) unmapped, then completed and released", how, nHost, (unsigned long long)(hostB >> 10));   // 0.0.612
    } else {
        // HUNG (or this slot's own work not proven retired): nothing the GPU might still read is freed; the context stays as it is, and the slot is DEAD.
        N1C_LOG("client closed (%s): slot %u VMID %u: %u BOs (%llu MiB VRAM, %llu MiB GTT), %u PT pages, %llu seqs, LEAKED (HUNG)", how, s->slot, s->vmid, nBos,
                (unsigned long long)(vramB >> 20), (unsigned long long)(gttB >> 20), ptPages, (unsigned long long)seqs);
        if (nHost != 0u) N1C_LOG("client closed (%s): %u host imports (%llu KiB) LEAKED (HUNG): pages stay wired, never completed or released", how, nHost, (unsigned long long)(hostB >> 10));   // 0.0.612
    }
    __atomic_store_n(&s->hello, false, __ATOMIC_RELEASE);
    __atomic_store_n(&s->open, false, __ATOMIC_RELEASE);          // the Session itself is static (slot 0) or never freed (slots 1..3)
    gSlotState[s->slot] = slot_after_close(parked, ownIdle);       // 0.0.627 (G2): a VMID that may still be in use is never handed out again
    if (s->ws) __atomic_store_n(&gSessWs, 0u, __ATOMIC_SEQ_CST);   // 0.0.623 (G1): no WindowServer session any more
    if (s->ws) __atomic_store_n(&gWsId, 0u, __ATOMIC_SEQ_CST);     // 0.0.650 (G5)
    if (!multi) OSCompareAndSwap(1, 0, &gOpenFlag);
    else __atomic_sub_fetch(&gOpenFlag, 1u, __ATOMIC_SEQ_CST);
    IOLockUnlock(gCliLock);
    IOLockUnlock(sl);   // R1: through the local, never the record: a racing n1c_open may already be re-initialising it (it never writes the lock field)
}

// ---- BOs ------------------------------------------------------------------------------------------------------------------------
// 0.0.612: the memory of a host-import BO. Called ONLY where the PTEs are already gone and the TLB flushed (kRelNormal: after pt_unmap + flush_vmid; kRelClosing: after program_root(park)),
// and NEVER in kRelLeak (bo_release's leak branch does not reach it: host_may_release). complete() before release(); the page list goes last.
static void host_release(Bo &b) {
    if (b.hmd != nullptr) { b.hmd->complete(); b.hmd->release(); b.hmd = nullptr; }
    if (b.hpages != nullptr) { IOFree(b.hpages, (vm_size_t)b.hpageCount * sizeof(uint64_t)); b.hpages = nullptr; }
}
// The VRAM a BO held goes back to its pool: take it off the budget too (0.0.627). Caller holds gCliLock.
static inline void budget_release(Bo &b) {
    if (b.budgeted != 0u) { gVramOthers = gVramOthers >= b.size ? gVramOthers - b.size : 0ull; b.budgeted = 0u; }
    if (b.visCounted != 0u) { gVisOthers = gVisOthers >= b.size ? gVisOthers - b.size : 0ull; b.visCounted = 0u; }   // 0.0.641
}
// 0.0.629 (R4): the GTT pages / host import of a BO were really freed (released, never leaked): off the global total. Caller holds gCliLock.
static inline void sys_release(Bo &b) { if (b.sysCounted != 0u) { gSysHeld = gSysHeld >= b.size ? gSysHeld - b.size : 0ull; b.sysCounted = 0u; } }
// Drop every mapping, fence slot and the memory of handle h of session s. leak = HUNG: forget the handle and the software state, touch nothing else.
// Caller holds the session's lock AND gCliLock (the pools, the budget and the fence window are shared).
static void bo_release(Session *s, uint32_t h, RelMode mode) {
    Bo &b = s->bo[h];
    if (b.pinMask != 0u && scan_unpin(h, b, mode)) b.pinLeak = 1u;   // 0.0.603: the console before the memory, in every mode
    if (b.pinMask1 != 0u && scan_unpin1(h, b, mode)) b.pinLeak = 1u; // 0.0.662: and the monitor A's (instance 1) A before ITS slots' memory
    if (b.pinMask2 != 0u && scan_unpin2(h, b, mode)) b.pinLeak = 1u; // 0.0.661: and instance 2's A before ITS slots' memory
    if (b.pinLeak != 0u) mode = kRelLeak;             // a restore that did not verify: keep the memory and the PTEs, like HUNG
    const FenceWindow &win = gWin[s->slot];
    if (mode == kRelClosing) {
        // The whole tree is being discarded (the context is parked, the arena is freed by the caller) and the fence-window PTEs are cleared
        // by the caller in one pass: only the memory goes.
        // 0.0.640 (G4), R6 on the CLOSE path: read the descriptor's references BEFORE this record drops its own. A visible-VRAM BO that a process still has CPU-mapped (a reference
        // beyond the record's) must not go back to the shared pool: the mapping could read and write whoever is handed that range next. 0.0.641: keyed on apps_on() (BOTH latches); without
        // navi48-apps: as 0.0.632 (always freed). By now the client's close has already removed the mappings it could (n1c_unmap_for_close): what is still counted here is the fallback.
        const bool vramReturnOk = n48native::g4::close_vram_return_ok(apps_on(), b.kind == kBoVis, b.vmd != nullptr, b.vmd != nullptr ? (uint32_t)b.vmd->getRetainCount() : 0u);
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) { sysmem_free(b.sm); sys_release(b); }
        else if (b.kind == kBoHi) { gCtx->gmc.vram_alloc_hi.free(b.a); budget_release(b); }
        else if (b.kind == kBoVis && !vramReturnOk) {
            // LEAKED until reboot, and still COUNTED: gVramOthers keeps these bytes (they are never taken off the budget), so the 25 % cap reflects what the pool really lost.
            gMappedLeakBytes += b.size; gMappedLeakBos++;
            N1C_LOG("close: slot %u BO %u (visible VRAM, %llu KiB at mc %#llx) is still CPU-mapped: its range is LEAKED until reboot (%u BO(s), %llu KiB leaked so far), not returned to the pool", s->slot, h,
                    (unsigned long long)(b.size >> 10), (unsigned long long)b.mc, gMappedLeakBos, (unsigned long long)(gMappedLeakBytes >> 10));
        }
        else if (b.kind == kBoVis) { gCtx->gmc.vram_alloc.free(b.a); budget_release(b); }
        else if (b.kind == kBoHost && host_may_release(kHostRelClosing)) { host_release(b); sys_release(b); }   // 0.0.612: the context is parked and the TLB flushed (the caller's program_root)
    } else if (mode == kRelNormal) {
        bool touched = false;
        PtMem mem{ s };
        for (uint32_t i = 0; i < kMaxMaps; i++) {
            const VaEnt &e = s->maps[i];
            if (va_ent_used(e) && e.handle == h) { pt_unmap(s->rootPa, e.start, e.size / kPage, mem); touched = true; }
        }
        bool memOk = true;   // review item D: false once a TLB flush did not acknowledge (HUNG): the memory of this BO is then leaked, like the HUNG leak
        if (touched) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(s->vmid)) && memOk; }
        bool slotCleared = false;
        for (uint32_t i = 0; i < N48N_FENCE_SLOTS; i++) {
            if (s->fslot[i].used && s->fslot[i].handle == h) {
                const uint64_t zero = 0;
                bar0_memcpy_to_vram(*gCtx->dev, gCtx->gmc.gart_pt_vram_offset + (win.gartOff / kPage + i) * 8ull, &zero, sizeof(zero));
                s->fslot[i] = FenceSlot{ 0, 0, 0 };
                slotCleared = true;
            }
        }
        if (slotCleared) { amdgpu_hdp_flush(*gCtx->dev); memOk = memory_may_free_after_flush(flush_vmid(0)) && memOk; }
        if (b.vmd != nullptr) { b.vmd->release(); b.vmd = nullptr; }   // the descriptor is released once, before the range it names is freed
        if (b.kind == kBoGtt) { if (memOk) { sysmem_free(b.sm); sys_release(b); } }   // review item D: a flush that did not acknowledge leaks the GTT pages (they stay counted, R4)
        else if (b.kind == kBoHi) { gCtx->gmc.vram_alloc_hi.free(b.a); budget_release(b); }
        else if (b.kind == kBoVis) { gCtx->gmc.vram_alloc.free(b.a); budget_release(b); }
        else if (b.kind == kBoHost && host_may_release(kHostRelNormal) && memOk) { host_release(b); sys_release(b); }   // 0.0.612: AFTER pt_unmap and flush_vmid above (unmap before complete)
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

// 0.0.610 (ABI 1.8): read-only views for the Metal nub gate (that source file is the only user). 0.0.627: Hello done on ANY open session (one with the switch OFF).
bool n1c_hello_done() {
    for (uint32_t i = 0; i < kMaxSessions; i++) { Session *g = slot_ptr(i); if (g != nullptr && __atomic_load_n(&g->open, __ATOMIC_ACQUIRE) && __atomic_load_n(&g->hello, __ATOMIC_ACQUIRE)) return true; }
    return false;
}
bool n1c_hung() { return hung_now(); }

IOReturn n1c_hello(const N1cRef &ref, uint64_t clientAbi, uint64_t flags, uint64_t out[4]) {
    Session *s = sess_lock(ref, false);
    if (s == nullptr) return kIOReturnNotReady;
    IOReturn rc = kIOReturnSuccess;
    if ((flags & ~(uint64_t)N48N_HELLO_F_MINOR) != 0ull) rc = kIOReturnBadArgument;
    else if (clientAbi != N48N_ABI_VERSION) { N1C_LOG("hello: client ABI %llu, kernel ABI %u: refused", (unsigned long long)clientAbi, N48N_ABI_VERSION); rc = kIOReturnUnsupported; }
    else {
        __atomic_store_n(&s->hello, true, __ATOMIC_RELEASE);
        // ABI 1.1: the minor is reported only when the client asks for it; without the flag out[0] is exactly the major, as in 1.0.
        out[0] = ((flags & N48N_HELLO_F_MINOR) != 0ull) ? (uint64_t)N48N_ABI_VERSION | ((uint64_t)N48N_ABI_MINOR << 16) : (uint64_t)N48N_ABI_VERSION;
        out[1] = kN1cKextBuild; out[2] = N48N_MAX_IBS; out[3] = s->vmid;   // 0.0.627: the session's own VMID (8 with the switch OFF)
    }
    sess_unlock(s);
    return rc;
}

IOReturn n1c_query_info(const N1cRef &ref, n48n_info *o) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    BringupContext &c = *gCtx;
    memset(o, 0, sizeof(*o));
    const IPVersion v = c.dev->ip.getVersion(IPBlock::GC);
    const uint64_t em = emitted_now();
    o->abi_version = N48N_ABI_VERSION; o->kext_build = kN1cKextBuild;
    o->flags = (hung_now() ? N48N_INFO_HUNG : 0u) | (native_s1b_state().positivePass ? N48N_INFO_S1B_POSITIVE : 0u) |
               (native_s1b_state().gate == kGateOn ? N48N_INFO_NATIVE_BOOT : 0u);
    o->vmid = s->vmid;
    o->gb_addr_config = RREG32(*c.dev, SOC15_REG_OFFSET_BIDX(*c.dev, IPBlock::GC, GFXRegBaseIdx::GB_ADDR_CONFIG, GFXRegs::GB_ADDR_CONFIG));
    o->gc_version = ((uint32_t)v.major << 16) | ((uint32_t)v.minor << 8) | v.rev;
    o->ring_size_dw = c.cp.ring_size_dwords; o->max_ibs = N48N_MAX_IBS;
    o->vram_vis_total = c.gmc.vram_alloc.size(); o->vram_vis_free = c.gmc.vram_alloc.bytes_free();
    o->vram_hi_total = c.gmc.vram_alloc_hi.is_inited() ? c.gmc.vram_alloc_hi.size() : 0;
    o->vram_hi_free = c.gmc.vram_alloc_hi.is_inited() ? c.gmc.vram_alloc_hi.bytes_free() : 0;
    o->gtt_cap = kGttCap; o->gtt_used = s->gttUsed; o->gtt_max_bo = kGttMaxBo;
    o->pt_cap = kPtCap; o->pt_used = (uint64_t)s->nblk * kArenaBlockBytes;
    o->reserved[0] = (uint32_t)(gWc & 0xFFFFFFFFu); o->reserved[1] = (uint32_t)(gWc >> 32);   // the native write counter (a diagnostic; the contract leaves reserved[] unused)
    o->reserved[2] = N48N_ABI_MINOR;                                                          // ABI 1.1 (0.0.603): the minor
    o->seq_emitted = em; o->seq_retired = retired_now(em);
    {   // 0.0.640 (G4), ABI 1.10: the session's VRAM budget (reserved[3..7]); all zero for a WindowServer session and with the switch OFF. The two loads are a diagnostic, not under gCliLock.
        const uint64_t poolFree = o->vram_vis_free + o->vram_hi_free;
        n48native::g4::budget_pack(n48native::g4::budget_report(multi_on(), s->ws, __atomic_load_n(&s->app, __ATOMIC_ACQUIRE), __atomic_load_n(&gVramOthers, __ATOMIC_RELAXED), vram_total(), poolFree), o->reserved);
    }
    o->va_low_first = kVaFirst; o->va_low_last = 0x00007FFFFFFFFFFFull;
    o->va_high_first = 0xFFFF800000000000ull; o->va_high_last = 0xFFFFFFFFFFFFFFFFull;
    o->max_bos = N48N_MAX_BOS; o->fence_slots = N48N_FENCE_SLOTS;
    return kIOReturnSuccess;
}

IOReturn n1c_read_regs(const N1cRef &ref, uint64_t off, uint64_t count, uint64_t instance, uint32_t *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallReadRegs)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): an APP session never reads registers
    if (count < 1 || count > 16 || instance != 0xFFFFFFFFull) return kIOReturnBadArgument;
    if (!regs_allowed(off, count) || (off + count) * 4ull > gCtx->dev->rmmioSize) return kIOReturnBadArgument;   // allowlist 0x263e..0x2641 only
    for (uint64_t i = 0; i < count; i++) out[i] = RREG32(*gCtx->dev, (uint32_t)(off + i));
    return kIOReturnSuccess;
}

IOReturn n1c_bo_create(const N1cRef &ref, const n48n_gem_create_in *in, uint64_t out[4]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    const Place p = place_decide(in->bo_size, in->alignment, in->domains, in->domain_flags);
    if (p.rc != kOk) return (IOReturn)p.rc;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    DeviceContext &dev = *gCtx->dev;
    IOReturn rc = (IOReturn)gpu_gate(s);
    const bool multi = multi_on();
    IOLockLock(gCliLock);   // the VRAM pools and the budget are shared
    do {
        if (rc != kIOReturnSuccess) break;
        const uint32_t h = lowest_free(s->boUsed, 1, N48N_MAX_BOS - 1);
        if (h == 0) { rc = kIOReturnNoResources; break; }
        Bo b {};
        b.uc = p.uc; b.size = p.size;
        uint32_t placed = N48N_GEM_DOMAIN_GTT, bits = 0;
        if (p.kind == kPlaceVis) {
            // 0.0.627 (G2): non-WindowServer sessions together stay within the global VRAM budget (switch ON; always true with it OFF).
            if (!n48native::g2::vram_budget_ok(multi, s->ws, gVramOthers, p.size, vram_total())) {
                N1C_LOG("BoCreate refused (NoMemory): slot %u is over the shared VRAM budget for non-WindowServer sessions (%llu MiB held of %llu MiB, %llu KiB asked)", s->slot,
                        (unsigned long long)(gVramOthers >> 20), (unsigned long long)(n48native::g2::others_vram_cap(vram_total()) >> 20), (unsigned long long)(p.size >> 10));
                rc = kIOReturnNoMemory; break;
            }
            VRAMAllocation a {};
            // 0.0.641 (G4 review), apps ON: a non-WindowServer session's NO_CPU_ACCESS BO goes to the hi pool FIRST (the visible pool is WindowServer's), and a CPU-visible placement of such a
            // session must fit under the shared cap (appvis_cap(), default 64 MiB) or the request fails like any other out-of-memory. Everything else is as 0.0.640.
            const bool appsNow = apps_on();
            bool got = false;
            bool hiDone = n48native::g4::hi_first(appsNow, s->ws, p.hiOk) && gCtx->gmc.vram_alloc_hi.is_inited() && gCtx->gmc.vram_alloc_hi.alloc(p.size, p.align, &a);
            if (!hiDone) {
                if (!n48native::g4::appvis_ok(appsNow, s->ws, gVisOthers, p.size, appsNow ? appvis_cap() : 0ull)) {
                    N1C_LOG("BoCreate refused (NoMemory): slot %u is over the visible-VRAM cap for non-WindowServer sessions (%llu KiB held of %llu MiB, %llu KiB asked)", s->slot,
                            (unsigned long long)(gVisOthers >> 10), (unsigned long long)(appvis_cap() >> 20), (unsigned long long)(p.size >> 10));
                    rc = kIOReturnNoMemory; break;
                }
                got = gCtx->gmc.vram_alloc.alloc(p.size, p.align, &a);
                if (got) {
                    const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
                    if (off + a.size > dev.bar0Size) { gCtx->gmc.vram_alloc.free(a); got = false; }
                }
            }
            if (got) {
                b.kind = kBoVis; b.a = a; b.mc = a.gpu_va;
                bar0_memset_vram(dev, a.gpu_va - gCtx->gmc.vram_start, 0, a.size);
                amdgpu_hdp_flush(dev);
                placed = N48N_GEM_DOMAIN_VRAM; bits = N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED;
                if (n48native::g4::appvis_counts(appsNow, s->ws, true)) { b.visCounted = 1u; gVisOthers += p.size; }
            } else if (hiDone || (p.hiOk && gCtx->gmc.vram_alloc_hi.is_inited() && gCtx->gmc.vram_alloc_hi.alloc(p.size, p.align, &a))) {
                b.kind = kBoHi; b.a = a; b.mc = a.gpu_va;
                placed = N48N_GEM_DOMAIN_VRAM; bits = N48N_PLACED_HI_POOL;
            } else { rc = kIOReturnNoMemory; break; }
            if (multi && !s->ws) { b.budgeted = 1u; gVramOthers += p.size; }
        } else {
            if (p.size > kGttMaxBo || would_exceed(s->gttUsed, p.size, kGttCap)) { rc = kIOReturnNoMemory; break; }
            if (!n48native::g2::sys_budget_ok(multi, s->ws, gSysHeld, p.size)) {   // 0.0.629 (R4): GTT + imports of all sessions <= 6 GiB (WindowServer: per-session caps only)
                N1C_LOG("BoCreate refused (NoMemory): slot %u: GTT + host imports of all sessions would pass the global %llu MiB (%llu MiB held, %llu KiB asked)", s->slot,
                        (unsigned long long)(n48native::g2::kSysGlobalCap >> 20), (unsigned long long)(gSysHeld >> 20), (unsigned long long)(p.size >> 10));
                rc = kIOReturnNoMemory; break;
            }
            SysMem sm {};
            if (sysmem_alloc(sm, p.size, p.align) != kIOReturnSuccess || !sm.valid()) { sysmem_free(sm); rc = kIOReturnNoMemory; break; }
            b.kind = kBoGtt; b.sm = sm;
            s->gttUsed += p.size;
            if (multi) { b.sysCounted = 1u; gSysHeld += p.size; }
            bits = N48N_PLACED_CPU_MAPPABLE | N48N_PLACED_ZEROED;
        }
        if (b.kind != kBoGtt) s->vramBytes += p.size;
        s->bo[h] = b; s->boUsed[h] = 1; s->nBos++;
        out[0] = h; out[1] = p.size; out[2] = placed; out[3] = bits;
    } while (false);
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}

// ---- 0.0.635 (M4c): the monitor B plane's two scanout buffers (see native_s1c.h). 0.0.655: ONE PAIR PER INSTANCE (slot 0 = the monitor A, instance 1; slot 1 = the monitor B, instance 2): each instance holds, frees and pins its own pair; the monitor B's held
// (or pinned) pair never blocks the monitor A's allocation and the reverse; a second pair for the SAME instance is refused. Everything here runs under gCliLock, the lock every user of the shared VRAM pools holds; no register, no memory is touched. ----
static VRAMAllocation gD2Buf[2][2];
static bool gD2Held[2];            // under gCliLock
static bool gD2Pinned[2];          // 0.0.652 (M5), under gCliLock: `disp2 fbhold` pinned the pair for the rest of the boot (a reboot is the release); never cleared
static BringupContext *gD2Ctx[2];  // the context the pair came from (under gCliLock)
static inline uint32_t d2slot(uint32_t inst) { return inst == 1u ? 0u : 1u; }
IOReturn n1c_d2_alloc(BringupContext &ctx, uint32_t inst, uint64_t bytes, uint64_t mc[2], uint64_t off[2]) {
    if ((inst != 1u && inst != 2u) || bytes == 0 || (bytes & 0xFFFFull) != 0 || lazy_lock(&gCliLock) == nullptr) return kIOReturnBadArgument;
    const uint32_t sl = d2slot(inst);
    IOReturn rc = kIOReturnSuccess;
    IOLockLock(gCliLock);
    do {
        if (n48d2pin::alloc_refused(gD2Held[sl])) { rc = kIOReturnExclusiveAccess; break; }
        if (!ctx.dev || !ctx.gmc.vram_alloc.is_inited()) { rc = kIOReturnNotReady; break; }
        VRAMAllocation a[2] {};
        uint32_t got = 0;
        bool ok = true;
        for (uint32_t k = 0; k < 2 && ok; k++) {
            if (!ctx.gmc.vram_alloc.alloc(bytes, 65536, &a[k])) { ok = false; break; }
            got = k + 1;
            ok = (a[k].gpu_va & 0xFFFFull) == 0 && a[k].gpu_va >= ctx.gmc.vram_start && (a[k].gpu_va - ctx.gmc.vram_start) + a[k].size <= ctx.dev->bar0Size;     // 64 KiB aligned and INSIDE the BAR0 aperture, else give it back
        }
        if (!ok) {
            for (uint32_t k = 0; k < got; k++) ctx.gmc.vram_alloc.free(a[k]);
            rc = kIOReturnNoMemory;
            break;
        }
        gD2Buf[sl][0] = a[0]; gD2Buf[sl][1] = a[1]; gD2Held[sl] = true; gD2Ctx[sl] = &ctx;
        for (uint32_t k = 0; k < 2; k++) { mc[k] = a[k].gpu_va; off[k] = a[k].gpu_va - ctx.gmc.vram_start; }
    } while (false);
    IOLockUnlock(gCliLock);
    return rc;
}
void n1c_d2_free(uint32_t inst) {
    if ((inst != 1u && inst != 2u) || gCliLock == nullptr) return;
    const uint32_t sl = d2slot(inst);
    IOLockLock(gCliLock);
    if (n48d2pin::free_action(gD2Held[sl] && gD2Ctx[sl] != nullptr, gD2Pinned[sl]) == n48d2pin::kFreePinned) {      // 0.0.652 (M5): the framebuffer scans buffer A and WindowServer maps it: a freed buffer that is still mapped would let its CPU writes corrupt whatever reuses that VRAM. A logged NO-OP while pinned.
        N1C_LOG("d2 free REFUSED: instance %u's pair is pinned (disp2 fbhold) for the rest of this boot; nothing was returned to the allocator", inst);
        IOLockUnlock(gCliLock);
        return;
    }
    if (n48d2pin::free_action(gD2Held[sl] && gD2Ctx[sl] != nullptr, gD2Pinned[sl]) == n48d2pin::kFreeDo) {
        gD2Ctx[sl]->gmc.vram_alloc.free(gD2Buf[sl][0]);
        gD2Ctx[sl]->gmc.vram_alloc.free(gD2Buf[sl][1]);
        gD2Buf[sl][0] = VRAMAllocation {}; gD2Buf[sl][1] = VRAMAllocation {};
        gD2Held[sl] = false; gD2Ctx[sl] = nullptr;
    }
    IOLockUnlock(gCliLock);
}

// 0.0.652 (M5): `disp2 fbhold 2` pins the pair (0.0.655: of the instance). Under gCliLock: the allocator must hold exactly the pair the plane reports (A = mc[0], B = mc[1]) and it must not be pinned already. false = nothing changed.
bool n1c_d2_pin(uint32_t inst, const uint64_t mc[2]) {
    if ((inst != 1u && inst != 2u) || mc == nullptr || gCliLock == nullptr) return false;
    const uint32_t sl = d2slot(inst);
    bool ok = false;
    IOLockLock(gCliLock);
    if (n48d2pin::pin_ok(gD2Held[sl], gD2Ctx[sl] != nullptr, gD2Pinned[sl], mc[0] != 0u && gD2Buf[sl][0].gpu_va == mc[0], mc[1] != 0u && gD2Buf[sl][1].gpu_va == mc[1])) { gD2Pinned[sl] = true; ok = true; }
    IOLockUnlock(gCliLock);
    if (ok) N1C_LOG("d2 pair of instance %u PINNED (disp2 fbhold): A %#llx B %#llx stay allocated for the rest of this boot", inst, (unsigned long long)mc[0], (unsigned long long)mc[1]);
    return ok;
}
bool n1c_d2_pinned(uint32_t inst) { return (inst == 1u || inst == 2u) && __atomic_load_n(&gD2Pinned[d2slot(inst)], __ATOMIC_ACQUIRE); }

IOReturn n1c_bo_free(const N1cRef &ref, uint64_t handle) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    if (handle > 0xFFFFFFFFull || !id_live(s->boUsed, N48N_MAX_BOS - 1, (uint32_t)handle)) rc = kIOReturnNotFound;
    else {
        const uint32_t h = (uint32_t)handle;
        RelMode m = kRelNormal;
        bool noBlame = false;
        const Bo &b0 = s->bo[h];
        // 0.0.629 (G2 review fix R6), switch ON: a visible-VRAM BO whose descriptor still has a user mapping (a reference beyond the record's own) is NOT freed:
        // Busy, nothing touched, the handle stays valid (unmap, then free again). Under HUNG the memory is leaked anyway, so the check is not needed there.
        const uint32_t mrc = hung_now() ? kOk : n48native::g2::bofree_mapping_rc(multi_on(), b0.kind == kBoVis, b0.vmd != nullptr, b0.vmd != nullptr ? (uint32_t)b0.vmd->getRetainCount() : 0u);
        if (mrc != kOk) {
            N1C_LOG("BoFree refused (Busy): slot %u BO %u (visible VRAM, %llu KiB) is still CPU-mapped (%d descriptor references): unmap it first", s->slot, h,
                    (unsigned long long)(b0.size >> 10), b0.vmd->getRetainCount());
            rc = (IOReturn)mrc;
        } else {
            if (hung_now()) m = kRelLeak;                                    // HUNG: drop the handle, leak the memory and the PTEs, succeed
            else if (!own_wait(s, &noBlame)) { m = kRelLeak; rc = kIOReturnTimeout; }  // 0.0.627: THIS session's work only. Not retired: the handle is dropped and leaked
            if (noBlame) {
                // 0.0.629 (R2), switch ON: the 2 s ran out while the GPU was still making progress. Nobody was blamed; the BO is KEPT (handle valid, memory and PTEs
                // as they were) and the caller gets Timeout. Its close frees it after its own work retired.
                N1C_LOG("BoFree: slot %u BO %u: own work not retired within 2 s while the GPU is progressing: Timeout, the BO is kept (no blame)", s->slot, h);
            } else {
                IOLockLock(gCliLock);
                bo_release(s, h, m);
                IOLockUnlock(gCliLock);
            }
        }
    }
    sess_unlock(s);
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

void n1c_latch_bar0_conflict(bool conflict) {
    if (OSCompareAndSwap(0, conflict ? 2u : 1u, &gBar0Conflict)) N1C_LOG("BAR0 latch: IOPCIFamily and config space %s on BAR0", conflict ? "DISAGREE (native client opens are refused)" : "agree");
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
// sleep: a racing close must not wait for a page-in); (3) under the session lock and gCliLock: the HUNG gate, the AUTHORITATIVE cap check, the handle, the optional map, the record. A failure before (3)
// completes leaves nothing mapped, so the descriptor is completed and released even under the HUNG latch (no PTE ever named those pages).
IOReturn n1c_bo_import_host(const N1cRef &ref, task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]) {
    const uint32_t seq0 = ref.id;   // review item C: the session this import belongs to, fixed BEFORE anything is wired (0.0.627: the client's own reference)
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (task == nullptr || out == nullptr) return kIOReturnBadArgument;
    Session *s0 = sess_peek(ref);
    if (s0 == nullptr) return kIOReturnNotReady;
    const ImportChk pre = import_check(hostVa, size, flags, gpuVa, __atomic_load_n(&s0->importedBytes, __ATOMIC_RELAXED));
    if (pre.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached (%llu MiB held, %llu KiB asked)", (unsigned long long)(kImportCap >> 20), (unsigned long long)(__atomic_load_n(&s0->importedBytes, __ATOMIC_RELAXED) >> 20), (unsigned long long)(size >> 10));   // 0.0.620
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
    Session *s = sess_lock(ref, false);
    const uint32_t gate = (s != nullptr && s->hello) ? gpu_gate(s) : kOk;   // 0.0.627: the HUNG / poison gate with only the session lock held (a stall poll may run a recovery: never under gCliLock)
    if (s != nullptr) IOLockLock(gCliLock);   // the page-table reserve is shared
    do {
        if (s == nullptr || !s->hello) { rc = kIOReturnNotReady; break; }   // closed while we waited for the lock
        if (!session_unchanged(seq0, s->id)) { rc = kIOReturnNotReady; break; }   // review item C: a different (or no) session now: these pages are the old client's
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        const ImportChk c = import_check(hostVa, size, flags, gpuVa, s->importedBytes);   // authoritative: the total as of now
        if (c.rc == kNoMemory) N1C_LOG("import refused: per-client cap of %llu MiB reached under the lock (%llu MiB held, %llu KiB asked)", (unsigned long long)(kImportCap >> 20), (unsigned long long)(s->importedBytes >> 20), (unsigned long long)(size >> 10));   // 0.0.620
        if (c.rc != kOk) { rc = (IOReturn)c.rc; break; }
        if (!n48native::g2::sys_budget_ok(multi_on(), s->ws, gSysHeld, size)) {   // 0.0.629 (R4): under gCliLock, before anything is mapped
            N1C_LOG("import refused (NoMemory): slot %u: GTT + host imports of all sessions would pass the global %llu MiB (%llu MiB held, %llu KiB asked)", s->slot,
                    (unsigned long long)(n48native::g2::kSysGlobalCap >> 20), (unsigned long long)(gSysHeld >> 20), (unsigned long long)(size >> 10));
            rc = kIOReturnNoMemory; break;
        }
        const uint32_t h = lowest_free(s->boUsed, 1, N48N_MAX_BOS - 1);
        if (h == 0) { rc = kIOReturnNoResources; break; }
        uint32_t slot = 0xFFFFFFFFu;
        bool mapped = false;
        if (c.gpuStripped != 0ull) {
            if (va_overlap(s->maps, kMaxMaps, c.gpuStripped, size) >= 0) { rc = kIOReturnBadArgument; break; }
            for (uint32_t i = 0; i < kMaxMaps; i++) if (!va_ent_used(s->maps[i])) { slot = i; break; }
            if (slot == 0xFFFFFFFFu) { rc = kIOReturnNoResources; break; }
            PtMem mem{ s };
            const uint32_t r = pt_map_pages(s->rootPa, c.gpuStripped, c.pages, pages, leaf_flags((uint32_t)flags, false, true), mem);   // SYSTEM | SNOOPED (+ R/W/X, MTYPE as asked)
            if (r != kOk) { rc = (IOReturn)r; break; }
            mapped = true;
        }
        Bo b {};
        b.kind = kBoHost; b.size = size; b.hmd = md; b.hpages = pages; b.hpageCount = (uint32_t)c.pages;
        if (multi_on()) { b.sysCounted = 1u; gSysHeld += size; }   // R4
        s->bo[h] = b; s->boUsed[h] = 1; s->nBos++;
        s->importedBytes += size;
        taken = true;                                             // md and pages now belong to the BO record
        if (mapped) {
            s->maps[slot] = VaEnt{ c.gpuStripped, size, 0, h, (uint32_t)flags };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = flush_vmid(s->vmid);               // synchronous: the mapping is complete on return
            if (f != kOk) rc = (IOReturn)f;                        // a hang was latched: the BO stays recorded (leaked at close)
        }
        out[0] = h; out[1] = size; out[2] = mapped ? va_canonicalize(c.gpuStripped) : 0ull; out[3] = N48N_PLACED_HOST_IMPORT;
    } while (false);
    if (s != nullptr) { IOLockUnlock(gCliLock); sess_unlock(s); }
    if (!taken) {
        IOFree(pages, (vm_size_t)(pre.pages * sizeof(uint64_t)));
        md->complete(); md->release();                            // never mapped: safe even when HUNG
    }
    return rc;
}

IOReturn n1c_gem_va(const N1cRef &ref, const n48n_gem_va *in) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    const VaReq q = gemva_check(in->operation, in->handle, in->_pad, in->flags, in->va_address, in->offset_in_bo, in->map_size,
                                in->vm_timeline_point, in->vm_timeline_syncobj_out, in->num_syncobj_handles, in->input_fence_syncobj_handles);
    if (q.rc != kOk) return (IOReturn)q.rc;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    do {
        if (in->operation == N48N_VA_OP_UNMAP && hung_now()) {
            // HUNG: skip the wait and the tables; forget the software mapping (its PTEs are leaked with the BO).
            const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);
            if (hit >= 0) s->maps[hit] = VaEnt{ 0, 0, 0, 0, 0 };
            break;
        }
        const uint32_t gate = gpu_gate(s);
        if (gate != kOk) { rc = (IOReturn)gate; break; }
        if (!id_live(s->boUsed, N48N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
        Bo &b = s->bo[in->handle];
        if (!range_in_bo(in->offset_in_bo, in->map_size, b.size)) { rc = kIOReturnBadArgument; break; }
        PtMem mem{ s };
        if (in->operation == N48N_VA_OP_MAP) {
            if (va_overlap(s->maps, kMaxMaps, q.stripped, in->map_size) >= 0) { rc = kIOReturnBadArgument; break; }
            const bool host = b.kind == kBoHost;                      // 0.0.612: scattered pages (b.hpages), mapped SYSTEM|SNOOPED like a GTT BO
            const bool sys = b.kind == kBoGtt || host;
            const uint64_t basePa = host ? 0ull : (sys ? b.sm.bus : pa_of_mc(b.mc)) + in->offset_in_bo;
            uint32_t slot = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < kMaxMaps; i++) if (!va_ent_used(s->maps[i])) { slot = i; break; }
            if (slot == 0xFFFFFFFFu) { rc = kIOReturnNoResources; break; }
            IOLockLock(gCliLock);   // 0.0.627: the page-table reserve is shared
            const uint32_t r = host ? pt_map_pages(s->rootPa, q.stripped, in->map_size / kPage, b.hpages + in->offset_in_bo / kPage, leaf_flags(in->flags, b.uc, sys), mem)
                                    : pt_map(s->rootPa, q.stripped, in->map_size / kPage, basePa, leaf_flags(in->flags, b.uc, sys), mem);
            IOLockUnlock(gCliLock);
            if (r != kOk) { rc = (IOReturn)r; break; }
            s->maps[slot] = VaEnt{ q.stripped, in->map_size, in->offset_in_bo, in->handle, in->flags };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = flush_vmid(s->vmid);      // synchronous: the mapping is complete on return
            if (f != kOk) rc = (IOReturn)f;
        } else {
            const int hit = va_find_exact(s->maps, kMaxMaps, in->handle, q.stripped, in->map_size);
            if (hit < 0) { rc = kIOReturnBadArgument; break; }
            if (!own_wait(s)) {
                // Not idle: leave the tables alone. This call's wait latched the hang, so it returns Timeout.
                rc = kIOReturnTimeout;
                break;
            }
            pt_unmap(s->rootPa, q.stripped, in->map_size / kPage, mem);
            s->maps[hit] = VaEnt{ 0, 0, 0, 0, 0 };
            amdgpu_hdp_flush(*gCtx->dev);
            const uint32_t f = flush_vmid(s->vmid);
            if (f != kOk) rc = (IOReturn)f;
        }
    } while (false);
    sess_unlock(s);
    return rc;
}

IOReturn n1c_ctx(const N1cRef &ref, const n48n_ctx *in, n48n_ctx *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
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
        const uint64_t fl = n48native::g2::ctx_state2(hung_now(), sess_poisoned(s));   // 0.0.627: also RESET | GUILTY for a poisoned session
        out->op = (uint32_t)(fl & 0xFFFFFFFFu); out->flags = (uint32_t)(fl >> 32);   // qword0
        break;
    }
    default:
        rc = kIOReturnBadArgument;
    }
    sess_unlock(s);
    return rc;
}

// The VMID-0 address of a user-fence target (contract 4.3): a VRAM BO by its MC address, a GTT BO through the slot's fence window. Caller holds the
// session's lock and gCliLock (the GART table and the VMID-0 TLB are shared).
static uint32_t resolve_fence(Session *s, uint32_t handle, uint32_t off, uint64_t *addr) {
    const Bo &b = s->bo[handle];
    const FenceWindow &win = gWin[s->slot];
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
        bar0_memcpy_to_vram(*gCtx->dev, gCtx->gmc.gart_pt_vram_offset + (win.gartOff / kPage + (uint64_t)slot) * 8ull, &pte, sizeof(pte));
        amdgpu_hdp_flush(*gCtx->dev);
        const uint32_t f = flush_vmid(0);
        if (f != kOk) return f;
        s->fslot[slot] = FenceSlot{ handle, page, 1 };
    }
    *addr = fence_addr_gtt(win.mc, (uint32_t)slot, off);
    return kOk;
}

IOReturn n1c_submit(const N1cRef &ref, const uint8_t *in, uint32_t size, uint64_t *seqOut) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    CsView v;
    const uint32_t prc = cs_parse(in, size, &v);
    if (prc != kOk) return (IOReturn)prc;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    do {
        const uint32_t gate = gpu_gate(s);   // 0.0.627: also Aborted for a poisoned session
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
            IOLockLock(gCliLock);
            const uint32_t fr = resolve_fence(s, v.fenceHandle, v.fenceOffset, &fenceAddr);
            IOLockUnlock(gCliLock);
            if (fr != kOk) { rc = (IOReturn)fr; break; }
        }
        uint32_t er;
        if (!g5_on()) er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);   // G5 OFF: exactly 0.0.642
        else if (s->ws) { __atomic_store_n(&gWsLastNs, now_ns(), __ATOMIC_RELEASE); er = ring_submit(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut); }   // 0.0.650 (G5): WindowServer is stamped, never gated
        else er = ring_submit_app_g5(s, va, by, v.nIbs, hasFence, fenceAddr, seqOut);   // 0.0.650 (G5): every other session passes the admission
        if (er != kOk) { rc = (IOReturn)er; break; }
    } while (false);
    sess_unlock(s);
    return rc;
}

IOReturn n1c_wait(const N1cRef &ref, uint64_t target, uint64_t timeoutNs, uint64_t out[3]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (sess_poisoned(s)) return kIOReturnAborted;   // 0.0.627 (G2): a poisoned session's fences are failed
    uint64_t res = 0;
    uint64_t em = emitted_now();
    const uint32_t rrc = n48native::g2::own_wait_resolve(target, em, __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE), &res);   // 0.0.627: LAST = this session's own last seqno
    if (rrc != kOk) return (IOReturn)rrc;
    const uint64_t to = wait_timeout_ns(timeoutNs);
    const uint64_t start = now_ns();
    uint64_t re = 0;
    for (;;) {
        em = emitted_now();
        re = retired_now(em);
        if (sess_poisoned(s)) return kIOReturnAborted;
        if (res == 0ull || re >= res) { out[0] = 0; out[1] = re; out[2] = em; return kIOReturnSuccess; }
        if (hung_now()) return kIOReturnAborted;
        const uint64_t el = now_ns() - start;
        if (el >= to) break;
        if (el < 5000000ull) IODelay(1); else IOSleep(1);
    }
    if (hang_poll()) return kIOReturnTimeout;                // this call's wait detected the hang
    if (sess_poisoned(s)) return kIOReturnAborted;           // 0.0.627: this call's stall named the caller guilty
    out[0] = 1; out[1] = re; out[2] = em;
    return kIOReturnSuccess;
}

// ---- ABI 1.1: native scanout (0.0.603) --------------------------------------------------------------------------------------------
// Lock order: the session lock, gCliLock, then the scanout lock inside n48dcn::scan*. Acquire / Register / Release take gCliLock (they touch the session's
// pins, and a racing close must not leave the plane taken); Query / Present / Status take only the scanout lock (a Present in flight either
// happened before the close's restore, which covers it, or is refused after it). 0.0.627 (G2), switch ON: one session holds the plane (gScanOwner);
// another session's Acquire is ExclusiveAccess and its Register / Present / Release are NotPermitted. Query and Status stay readable by all.
static inline bool scan_foreign(const Session *s) {
    if (!multi_on()) return false;
    const uint32_t o = __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE);
    return o != 0u && o != s->id;
}
IOReturn n1c_scan_query(const N1cRef &ref, n48n_scan_query *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    return (IOReturn)n48dcn::scanQuery(out);
}
IOReturn n1c_scan_acquire(const N1cRef &ref, uint64_t flags, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnExclusiveAccess;
    if (!scan_foreign(s)) {
        rc = (IOReturn)n48dcn::scanAcquire(out, s->id);   // 0.0.609: the session id lets a row-120 hold be Acquired
        if (rc == kIOReturnSuccess && multi_on()) __atomic_store_n(&gScanOwner, s->id, __ATOMIC_RELEASE);
    }
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scan_register(const N1cRef &ref, const n48n_scan_reg *in, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (in->reserved0 != 0u) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    do {
        if (scan_foreign(s)) { rc = kIOReturnNotPermitted; break; }
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
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scan_present(const N1cRef &ref, uint64_t slot, uint64_t flags, uint64_t out[3]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (scan_foreign(s)) return kIOReturnNotPermitted;
    return (IOReturn)n48dcn::scanPresent(slot, flags, out);
}
IOReturn n1c_scan_status(const N1cRef &ref, n48n_scan_status *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    n48disp_m6_flush();   // 0.0.661 (review M1): the DP's 1 s keep-alive also flushes a deferred table publish (instance 2 may never be acquired); inert unless both latches are ON
    return (IOReturn)n48dcn::scanStatus(out);
}
IOReturn n1c_scan_release(const N1cRef &ref, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    if (scan_foreign(s)) rc = kIOReturnNotPermitted;
    else {
        uint64_t r[2] = { 1, 0 };
        scan_teardown(s, "ScanoutRelease", r);                                     // idempotent; the verdict is the restore's own
        if (multi_on()) __atomic_store_n(&gScanOwner, 0u, __ATOMIC_RELEASE);
        out[0] = r[0]; out[1] = r[1];
    }
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}

// ---- ABI 1.11 (0.0.661, M6 Stage 1b) + 1.12 (0.0.662, M6 Stage 2): the HDMI instances (2 = the monitor B; 1 = the monitor A, additionally behind navi48-m6flip1). Every call: Hello, not an APP, the latches (Unsupported with navi48-m6 / navi48-m6flip OFF,
// before anything else), the instance (2, or 1 with navi48-m6flip1; anything else - and 1 with that latch OFF - is BadArgument, exactly 0.0.661's answer) and the flag words, the session must hold instance 0 (the plane Acquire of selector 10), and a foreign
// session is refused - ALL before any register read. Lock order as selectors 9..14: the session lock, gCliLock, then the instance's lock inside n48dcn::scanX*.
static inline bool scanx_on() { return n48m6_latched_on() && n48m6flip_latched_on(); }
static inline bool scanx_inst_ok(uint64_t inst) { return n48scanx::inst_admitted(inst, n48m6flip1_latched_on()); }
static inline bool scanx_owner(const Session *s) { return multi_on() ? __atomic_load_n(&gScanOwner, __ATOMIC_ACQUIRE) == s->id : n48dcn::scanActive(); }
IOReturn n1c_scanx_acquire(const N1cRef &ref, uint64_t inst, uint64_t flags, uint64_t out[5]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst) || flags != 0ull) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnNotPermitted;
    if (!scan_foreign(s) && scanx_owner(s)) { rc = (IOReturn)n48dcn::scanXAcquire((uint32_t)inst, out); if (rc == kIOReturnSuccess) out[3] = gCtx->gmc.vram_alloc.bytes_free(); }   // review S4: the free visible-VRAM figure (under gCliLock, the lock every user of the pool holds)
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scanx_register(const N1cRef &ref, uint64_t inst, const n48n_scan_reg *in, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst) || in->reserved0 != 0u) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    do {
        if (scan_foreign(s) || !scanx_owner(s)) { rc = kIOReturnNotPermitted; break; }
        if (!id_live(s->boUsed, N48N_MAX_BOS - 1, in->handle)) { rc = kIOReturnNotFound; break; }
        Bo &b = s->bo[in->handle];
        if (b.kind == kBoHost) { rc = kIOReturnNotPermitted; break; }
        const uint32_t gen = n48dcn::scanXGeneration((uint32_t)inst);       // THAT instance's generation: pinGen1 is judged against instance 1's, pinGen2 against instance 2's
        uint8_t &mask = inst == 1ull ? b.pinMask1 : b.pinMask2;
        uint32_t &pgen = inst == 1ull ? b.pinGen1 : b.pinGen2;
        mask = n48scanx::pins_live(mask, pgen, gen);                        // a pin of an acquisition that has ended is stale
        uint64_t o2[2] = { 0, 0 };
        const uint32_t r = n48dcn::scanXRegister((uint32_t)inst, b.kind == kBoVis, b.mc, b.size, in->offset, in->pitch_bytes, in->width, in->height, in->format, o2);
        if (r != kOk) { rc = (IOReturn)r; break; }
        const n48scanx::XDesc *xd = n48scanx::desc_of((uint32_t)inst);
        if (xd == nullptr || !n48scanx::id_ok(*xd, o2[0])) {               // the kernel returned an id that is not this instance's tag (cannot happen unless a descriptor table is wrong): the slot would be unpinned, so the instance is RELEASED (A back) and, if that is not verified, the BO leaked
            uint64_t rr[2] = { 1, 0 };
            (void)n48dcn::scanXRelease((uint32_t)inst, "a registered slot id of another instance", rr);
            if (rr[0] == 0ull) b.pinLeak = 1u;
            rc = kIOReturnInternalError; break;
        }
        mask = (uint8_t)(mask | (1u << (n48scanx::slot_of(*xd, o2[0]) & 7u))); pgen = gen;
        out[0] = o2[0]; out[1] = o2[1];
    } while (false);
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}
IOReturn n1c_scanx_present(const N1cRef &ref, uint64_t inst, uint64_t slot, uint64_t flags, uint64_t out[3]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst)) return kIOReturnBadArgument;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (scan_foreign(s) || !scanx_owner(s)) return kIOReturnNotPermitted;
    return (IOReturn)n48dcn::scanXPresent((uint32_t)inst, slot, flags, out);
}
IOReturn n1c_scanx_status(const N1cRef &ref, uint64_t inst, n48n_scan_status *out, uint64_t sout[4]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst)) return kIOReturnBadArgument;
    Session *s = sess_peek(ref);
    if (s == nullptr) return kIOReturnNotReady;
    if (scan_foreign(s)) return kIOReturnNotPermitted;
    return (IOReturn)n48dcn::scanXStatus((uint32_t)inst, out, sout);
}
IOReturn n1c_scanx_release(const N1cRef &ref, uint64_t inst, uint64_t out[2]) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;
    if (!scanx_on()) return kIOReturnUnsupported;
    if (!scanx_inst_ok(inst)) return kIOReturnBadArgument;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;
    IOLockLock(gCliLock);
    IOReturn rc = kIOReturnSuccess;
    if (scan_foreign(s)) rc = kIOReturnNotPermitted;
    else {
        uint64_t r[2] = { 1, 0 };
        (void)n48dcn::scanXRelease((uint32_t)inst, "SCANX_RELEASE", r);
        for (uint32_t h = 1; h < N48N_MAX_BOS; h++) if (s->boUsed[h]) {
            uint8_t &mask = inst == 1ull ? s->bo[h].pinMask1 : s->bo[h].pinMask2;      // only THIS instance's pins are judged and cleared
            if (r[0] == 0ull && mask != 0u) s->bo[h].pinLeak = 1u;
            mask = 0;
        }
        out[0] = r[0]; out[1] = r[1];
    }
    IOLockUnlock(gCliLock);
    sess_unlock(s);
    return rc;
}

// ---- ABI 1.2: the DAL experiment (0.0.604). One named step of an internal design note per call. The client lock is NOT held: a step
// sleeps for up to ~65 s (E4's dwell) and a racing close must not wait for it; smu_dal.cpp's own busy flag refuses a second concurrent step, and
// its latch, gate and allowlist do all the refusing. Nothing here touches a session field.
IOReturn n1c_dal_step(const N1cRef &ref, uint64_t step, uint64_t flags, n48n_dal_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if (flags != 0ull || step < N48N_DAL_STEP_E1B || step > N48N_DAL_STEP_E4 || out == nullptr) return kIOReturnBadArgument;
    return dal_run_step(*gCtx->dev, (uint32_t)step, out);
}

// ---- ABI 1.3: the timed mode trial (0.0.605). The client lock is NOT held (the trial sleeps for up to dwell + ~12 s and a racing close must not wait for it); dcn/navi48_dcn.cpp's
// busy flag, latch, gate and deny checks do all the refusing. Nothing here touches a session field.
IOReturn n1c_mode_trial(const N1cRef &ref, uint64_t row, uint64_t dwellMs, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if ((flags & ~(uint64_t)N48N_MODE_TF_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;   // 0.0.606: the flags word carries N48N_MODE_TF_* (0.0.605 required 0)
    return (IOReturn)n48dcn::modeTrial(row, dwellMs, flags, out);
}

// ---- ABI 1.7: the HELD mode of row 120 (0.0.609). Like the trial: no client lock is held (the launch waits up to ~20 s, the release up to ~30 s); the session id names the owner.
IOReturn n1c_mode_hold(const N1cRef &ref, uint64_t maxMs, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if ((flags & ~(uint64_t)N48N_HOLD_F_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)n48dcn::modeHold(ref.id, maxMs, flags, out);   // 0.0.627: the caller's own session id
}
IOReturn n1c_mode_release(const N1cRef &ref, uint64_t flags, n48n_mode_result *out) {
    if (!sess_hello(ref)) return kIOReturnNotReady;
    if (app_refused(ref, n48native::g4::kCallScanout)) return kIOReturnNotPrivileged;   // 0.0.640 (G4): the display plane / mode / DAL belong to WindowServer and the administrator
    if ((flags & ~(uint64_t)N48N_REL_F_MASK) != 0ull || out == nullptr) return kIOReturnBadArgument;
    return (IOReturn)n48dcn::modeRelease(flags, out);
}

IOReturn n1c_memory_for_handle(const N1cRef &ref, uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (memory == nullptr) return kIOReturnBadArgument;
    *memory = nullptr;
    if (!sess_hello(ref)) return kIOReturnNotReady;
    Session *s = sess_lock(ref, true);
    if (s == nullptr) return kIOReturnNotReady;   // closed while we waited for the lock
    IOReturn rc = kIOReturnSuccess;
    if (!id_live(s->boUsed, N48N_MAX_BOS - 1, handle)) rc = kIOReturnNotFound;
    else {
        Bo &b = s->bo[handle];
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
    sess_unlock(s);
    return rc;
}

// ==== 0.0.627: GPU-apps stage G2, several sessions (boot-arg navi48-multisession=1; default OFF) =================================================
// The pure half is native_g2_pure.h (host-tested by tests/native_g2_test.cpp). The latch and the stat verb (accel action 103 `sessstat <0..4>`).
static volatile UInt32 gMultiLatch = n48native::g2::kLatchUnset;
static bool multi_on() {
    UInt32 l = gMultiLatch;
    if (l == n48native::g2::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-multisession", &v, sizeof(v));
        if (OSCompareAndSwap(n48native::g2::kLatchUnset, n48native::g2::latch_value(present, v), &gMultiLatch))
            N1C_LOG("boot-arg navi48-multisession %s: native sessions are %s for this boot", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                    n48native::g2::latch_is_on(gMultiLatch) ? "MULTIPLE (up to 4: VMID 8 for the first, 9..11 for the others)" : "SINGLE (VMID 8, one open at a time)");
        l = gMultiLatch;
    }
    return n48native::g2::latch_is_on(l);
}
bool n1c_multi_latched_on() { return multi_on(); }

// ==== 0.0.640: GPU-apps stage G4, ordinary user applications (boot-arg navi48-apps=1 AND navi48-multisession=1; default OFF) ====================================
// The pure half is native_g4_pure.h (host-tested by tests/native_g4_test.cpp). The latch, the allow-list, the role mark and the verb (accel action 104 `appallow`).
static volatile UInt32 gAppsLatch = n48native::g4::kLatchUnset;
static bool apps_on() {
    UInt32 l = gAppsLatch;
    if (l == n48native::g4::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-apps", &v, sizeof(v));
        if (OSCompareAndSwap(n48native::g4::kLatchUnset, n48native::g4::latch_value(present, v), &gAppsLatch))
            N1C_LOG("boot-arg navi48-apps %s: user applications are %s for this boot (they also need navi48-multisession=1, which is %s)", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                    n48native::g4::latch_is_on(gAppsLatch) ? "ADMITTED when allow-listed" : "NOT admitted", multi_on() ? "ON" : "OFF");
        l = gAppsLatch;
    }
    return n48native::g4::apps_enabled(n48native::g4::latch_is_on(l), multi_on());
}
bool n1c_apps_enabled() { return apps_on(); }
// ==== 0.0.650: GPU-apps stage G5 Stage 1, the application credit (boot-arg navi48-g5=1 AND navi48-multisession=1; default OFF) ====================================
// The pure half is native_g5_pure.h (host-tested by tests/native_g5_test.cpp). The latches and the stat page (accel 103 `sessstat 5`).
static volatile UInt32 gG5Latch = n48native::g2::kLatchUnset;
static bool g5_on() {
    UInt32 l = gG5Latch;
    if (l == n48native::g2::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-g5", &v, sizeof(v));
        if (OSCompareAndSwap(n48native::g2::kLatchUnset, n48native::g2::latch_value(present, v), &gG5Latch))
            N1C_LOG("boot-arg navi48-g5 %s: the application credit is %s for this boot (it also needs navi48-multisession=1, which is %s)", present ? (v == 1u ? "=1" : "present but not 1") : "absent",
                    n48native::g2::latch_is_on(gG5Latch) ? "ARMED" : "OFF", multi_on() ? "ON" : "OFF");
        l = gG5Latch;
    }
    return n48native::g5::on_effective(n48native::g2::latch_is_on(l), multi_on());
}
bool n1c_g5_latched_on() { return g5_on(); }
static volatile UInt32 gG5CreditLatch;   // 0 = unread, 1 = latched
static uint32_t gG5Credit = n48native::g5::kCreditDefault;
static uint32_t g5_credit() {
    if (__atomic_load_n(&gG5CreditLatch, __ATOMIC_ACQUIRE) == 0u) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-g5credit", &v, sizeof(v));
        gG5Credit = n48native::g5::credit_value(present, v);   // idempotent: every racing first reader computes the same value
        __atomic_store_n(&gG5CreditLatch, 1u, __ATOMIC_RELEASE);
        N1C_LOG("boot-arg navi48-g5credit %s: at most %u application job(s) in flight ahead of WindowServer while it is active (G5 ON only)", !present ? "absent" : (v >= 1u && v <= n48native::g5::kCreditMax) ? "read" : "out of range 1..4 (default used)", gG5Credit);
    }
    return gG5Credit;
}
// accel 103 `sessstat 5`. Read-only. The caller (accelExperiment) checked n48native::g5::args_ok. o[4] and o[5] are read under ONE hold of gRingLock (never nested in gCliLock).
IOReturn n1c_g5_stat(uint64_t *o) {
    using namespace n48native::g5;
    for (uint32_t i = 0; i < n48native::g2::kOutN; i++) o[i] = 0;
    if (!g5_on()) { o[0] = n48native::g2::kStatBadArg; return kIOReturnBadArgument; }
    StatView v {};
    v.K = g5_credit();
    const uint64_t now = now_ns();
    const uint64_t last = __atomic_load_n(&gWsLastNs, __ATOMIC_ACQUIRE);
    const uint32_t wsId = __atomic_load_n(&gWsId, __ATOMIC_SEQ_CST);
    v.wsOpen = wsId != 0u;
    v.wsActive = ws_active(v.wsOpen, last, now, kWsWindowNs);
    v.sinceWsMs = (last != 0ull && now >= last) ? (now - last) / 1000000ull : kNever;
    if (gRingLock != nullptr && gCtx != nullptr) {
        IOLockLock(gRingLock);
        const uint64_t em = emitted_now(), re = retired_now(em);
        v.appInFlight = app_in_flight(gOwners, re, em, wsId, n48native::g2::kOwnerN);
        v.wsOutstanding = ws_outstanding(gOwners, re, em, wsId);
        v.st = gG5;
        IOLockUnlock(gRingLock);
    }
    stat_out(v, o);
    return kIOReturnSuccess;
}
bool n1c_ws_is_open() { return __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u; }   // 0.0.641: a WindowServer session is open (the open policy's alreadyOpen input with apps ON)
// 0.0.641: the non-WindowServer visible-pool cap, boot-arg navi48-appvis=<MiB> (16..128; default 64), read once.
static volatile UInt32 gAppVisLatch;   // 0 = unread, 1 = latched
static uint64_t gAppVisCap;
static uint64_t appvis_cap() {
    if (__atomic_load_n(&gAppVisLatch, __ATOMIC_ACQUIRE) == 0u) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-appvis", &v, sizeof(v));
        gAppVisCap = n48native::g4::appvis_cap_bytes(present, v);   // idempotent: every racing first reader computes the same value
        __atomic_store_n(&gAppVisLatch, 1u, __ATOMIC_RELEASE);
        N1C_LOG("boot-arg navi48-appvis %s: non-WindowServer sessions may hold %llu MiB of the visible VRAM pool together (apps ON only)", !present ? "absent" : (v >= n48native::g4::kAppVisMinMiB && v <= n48native::g4::kAppVisMaxMiB) ? "read" : "out of range 16..128 (default used)", (unsigned long long)(gAppVisCap >> 20));
    }
    return gAppVisCap;
}
// Is this process name on the allow-list AND not struck out? (false at once with the role disabled.) The caller reads the name with proc_selfname. *struckOut: it IS on the list but has
// n48native::g4::kStrikeLimit strikes (automatic recoveries blamed on an APP session of that name this boot): refused at open, with kReasonAppNotAllowed, like an unlisted name.
bool n1c_app_allowed(const char *comm, bool *struckOut) {
    *struckOut = false;
    if (!apps_on()) return false;
    const uint64_t key = n48a_comm_key(comm);
    const bool listed = n48native::g4::al_contains(gAppAllow, key);
    *struckOut = listed && n48native::g4::strike_refused(n48native::g4::strike_count(gStrikes, key));
    return n48native::g4::app_admit(listed, n48native::g4::strike_count(gStrikes, key));
}
// Mark the session the client was just opened with as an APP. Called by create() right after n1c_open, before the client is returned (so no selector can have run). A stale reference
// or a disabled role is refused; the client then closes the session again. 0.0.641: `key` = the opener's executable-name key (for the strike count).
IOReturn n1c_mark_app(const N1cRef &ref, uint64_t key) {
    if (!apps_on()) return kIOReturnNotPermitted;
    Session *s = sess_lock(ref, false);
    if (s == nullptr) return kIOReturnNotReady;
    s->appKey = key;
    __atomic_store_n(&s->app, true, __ATOMIC_RELEASE);
    sess_unlock(s);
    return kIOReturnSuccess;
}
// 0.0.641 (G4 review HIGH 1b): called by the client class BEFORE n1c_close, with apps ON only. For each visible-VRAM BO of the session that still has CPU mappings (descriptor references beyond the
// record's own) the mappings that IOUserClient keeps are removed (removeMappingForDescriptor returns each map retained; releasing it drops the map's reference on the descriptor and unmaps it from
// the task if that is still alive), so that n1c_close sees the record's reference alone and frees the range. No lock is held across the removal. A mapping that cannot be removed (it was handed to
// another task as a send right) keeps the count above 1 and n1c_close leaks the range, as before.
void n1c_unmap_for_close(const N1cRef &ref, IOUserClient *uc) {
    if (uc == nullptr || !apps_on()) return;
    uint32_t removed = 0;
    for (uint32_t h = 1; h < N48N_MAX_BOS; h++) {
        Session *s = sess_lock(ref, false);
        if (s == nullptr) break;
        IODeviceMemory *vmd = s->boUsed[h] ? s->bo[h].vmd : nullptr;
        const uint32_t n = n48native::g4::unmap_attempts(true, vmd != nullptr && s->bo[h].kind == kBoVis, vmd != nullptr, vmd != nullptr ? (uint32_t)vmd->getRetainCount() : 0u);
        if (n != 0u) vmd->retain();   // our own reference while the lock is dropped (a racing BoFree may release the record's)
        sess_unlock(s);
        if (n == 0u) continue;
        for (uint32_t i = 0; i < n; i++) {
            IOMemoryMap *m = uc->removeMappingForDescriptor(vmd);
            if (m == nullptr) break;
            m->release(); removed++;
        }
        vmd->release();
    }
    if (removed != 0u) N1C_LOG("close: slot %u: %u CPU mapping(s) of visible-VRAM BOs removed before the session's BOs are released", ref.slot, removed);
}
// 0.0.641: the "open refused" log gate (n48native::g4::log_gate_step). true = log this line; *suppressed = lines dropped in the previous window (report them once). Log-only.
bool n1c_log_gate(uint32_t *suppressed) {
    bool admit = false;
    const uint64_t sec = now_ns() / 1000000000ull;
    uint64_t w = __atomic_load_n(&gLogGate, __ATOMIC_RELAXED);
    for (;;) {
        const uint64_t nw = n48native::g4::log_gate_step(w, sec, &admit, suppressed);
        if (__atomic_compare_exchange_n(&gLogGate, &w, nw, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) break;
    }
    return admit;
}
// accel 104 `appallow`. The caller (accelExperiment) checked the role and the argument; this runs the pure verb on the kernel's list and logs the change.
IOReturn n1c_app_verb(uint64_t arg, uint64_t *o) {
    using namespace n48native::g4;
    const uint32_t rc = verb_run(apps_on(), gAppAllow, arg, o, &gStrikes);
    if (rc == kAllowOk && o[2] != N48A_OP_LIST) N1C_LOG("appallow: %s key %#llx -> %u entr%s on the allow-list", o[2] == N48A_OP_ADD ? "ADDED" : "REMOVED", (unsigned long long)o[4], (unsigned)o[1], o[1] == 1ull ? "y" : "ies");
    return rc == kAllowOk || rc == kAllowExists || rc == kAllowNotFound ? kIOReturnSuccess : rc == kAllowFull ? kIOReturnNoSpace : rc == kAllowOff ? kIOReturnNotPermitted : kIOReturnBadArgument;
}
// accel 103 `sessstat <page>`: page 0..3 one session slot, page 4 the global page. Read-only. The caller (accelExperiment) checked the latch and the argument.
IOReturn n1c_g2_stat(uint64_t page, uint64_t *o) {
    using namespace n48native::g2;
    for (uint32_t i = 0; i < kOutN; i++) o[i] = 0;
    if (!multi_on()) { o[0] = kStatOff; return kIOReturnSuccess; }
    if (page > (uint64_t)kStatGlobal) { o[0] = kStatBadArg; return kIOReturnBadArgument; }
    if (gCliLock == nullptr || gCtx == nullptr) { o[0] = kStatOk; o[1] = page; return kIOReturnSuccess; }   // nothing ever opened this boot
    if (page == (uint64_t)kStatGlobal) {
        GlobalView g {};
        IOLockLock(gCliLock);
        g.open = __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST);
        for (uint32_t i = 0; i < kMaxSessions; i++) if (gSlotState[i] == kSlotDead) g.dead++;
        g.othersVram = gVramOthers; g.othersCap = others_vram_cap(vram_total());
        if (apps_on()) { g.appVisBytes = gVisOthers; g.appVisCapMiB = (uint32_t)(appvis_cap() >> 20); }   // 0.0.641: the visible-pool use of non-WindowServer sessions and its cap
        IOLockUnlock(gCliLock);
        g.hung = hung_now(); g.emitted = emitted_now(); g.retired = retired_now(g.emitted);
        g.mappedLeakBos = gMappedLeakBos;   // 0.0.640 (G4): BOs leaked at close because a CPU mapping outlived the session (read without gCliLock: a diagnostic)
        g.recoveries = gG2.recoveries; g.lastGuiltySlot = gG2.lastGuiltySlot; g.lastGuiltySeq = gG2.lastGuiltySeq; g.collateralTotal = gG2.collateralTotal;
        g.faultNow = fault_snap("sessstat", kStatGlobal);   // R5: read only
        IOLockLock(gCliLock);
        g.faultPrev = gFaultPrev; gFaultPrev = g.faultNow;  // the previous call's word, then this call's is kept for the next
        IOLockUnlock(gCliLock);
        if (fault_changed(g.faultPrev, g.faultNow)) N1C_LOG("sessstat: the gcvm fault word CHANGED since the previous sessstat 4: %#llx -> %#llx", (unsigned long long)g.faultPrev, (unsigned long long)g.faultNow);
        global_out(g, o);
        return kIOReturnSuccess;
    }
    const uint32_t slot = (uint32_t)page;
    SessView v {};
    v.vmid = vmid_of_slot(slot);
    IOLockLock(gCliLock);
    v.state = gSlotState[slot];
    Session *s = slot_ptr(slot);
    if (s != nullptr && __atomic_load_n(&s->open, __ATOMIC_ACQUIRE)) {   // the counters are read under gCliLock (a racing selector may be mid-update: a diagnostic)
        v.ws = s->ws; v.app = s->app; v.hello = s->hello; v.poisoned = sess_poisoned(s); v.id = s->id;   // 0.0.640 (G4): app
        v.vram = s->vramBytes; v.gtt = s->gttUsed; v.imported = s->importedBytes; v.ptBytes = (uint64_t)s->nblk * kArenaBlockBytes;
        v.lastSeq = __atomic_load_n(&s->emitted, __ATOMIC_ACQUIRE); v.nBos = s->nBos;
        v.faultAtOpen = s->faultAtOpen;   // 0.0.629 (R5)
    }
    IOLockUnlock(gCliLock);
    sess_out(slot, v, o);
    return kIOReturnSuccess;
}

// ==== 0.0.623: GPU-apps stage G1, hang recovery (boot-arg navi48-g1=1; default OFF) ================================================================
// The pure half is native_g1_pure.h (decisions, plan, bounds, the run templates; host-tested by tests/native_g1_test.cpp). This is the kernel
// environment those templates run against. What it may touch, and nothing else:
//   - a kernel-owned TEST CONTEXT: the ordinary native session (n1c_open: every gate, the exclusivity CAS, a fresh VMID-8 tree) that no user client
//     owns (hello stays false, so no selector reaches it) with ONE 16 KiB visible-VRAM BO mapped R|W|X|UC at g1::kTestVa;
//   - the kernel GFX ring through ring_submit (the one ring writer; 0.0.623-0.0.626: ring_emit), and, for the recovery methods only: ring memory outside the live range (NOPs), the
//     MES RESET / REMOVE_QUEUE / ADD_QUEUE frames for GFX pipe 0 queue 0 (the queue amdgpu_init's PM4Test mapped with map_legacy_kq), the MQD memory
//     (cp_gfx_mqd_init: memory only), the CP write-back rptr / wptr words and the software ring counters;
//   - the HUNG latch: set through hang_from_wait (the client path), cleared ONLY by G1Env::clear_latch, which only recover_run calls after a VERIFIED probe.
// It writes no register ITSELF (no register-write macro appears in this block), but the calls it makes do (0.0.626 correction; see the header comment): the MES and ring doorbells, GRBM_GFX_CNTL (cp_dump_gfx_hqd), the TLB invalidate (flush_vmid), CONTEXT8 (program_root) and the HDP flush. It never touches PSP, SMU, DCN or a GRBM soft reset.
static volatile UInt32 gG1Latch = n48native::g1::kLatchUnset;
bool n1c_g1_latched_on() {
    UInt32 l = gG1Latch;
    if (l == n48native::g1::kLatchUnset) {
        uint32_t v = 0;
        const bool present = PE_parse_boot_argn("navi48-g1", &v, sizeof(v));
        if (OSCompareAndSwap(n48native::g1::kLatchUnset, n48native::g1::latch_value(present, v), &gG1Latch))
            N1C_LOG("boot-arg navi48-g1 %s: the G1 hang-recovery verbs (hangtest / hangrecover / hangstat) are %s for this boot",
                    present ? (v == 1u ? "=1" : "present but not 1") : "absent", n48native::g1::latch_is_on(gG1Latch) ? "ENABLED" : "OFF");
        l = gG1Latch;
    }
    return n48native::g1::latch_is_on(l);
}

namespace {
struct G1State {
    bool holds;                    // the test context holds the native session (written only under gG1Lock)
    bool resetAcked;               // the MOST RECENT MES RESET on THIS held hang was acked (remap then adds remove_queue_after_reset, sched >= 0x5a; a standalone remap needs it)
    uint64_t hungSeq;              // the seqno of the hang IB
    uint64_t liveStart;            // the write counter at the start of the oldest unretired submission (the hang IB's, or 0 after a remap)
    uint64_t boOff;                // the test BO's BAR0 offset
    uint32_t attempts;             // attempts made on this held hang
    n48native::g1::Attempt att[n48native::g1::kMaxAttempts];
    uint32_t attRptr[n48native::g1::kMaxAttempts], attCpStat[n48native::g1::kMaxAttempts];
    uint32_t lastCode, lastMethod, tests, recovered, failedRuns;
    N1cRef   ref;                  // 0.0.627: the test context's session (slot 0, VMID 8)
};
G1State gG1;
IOLock *gG1Lock;
}

static bool g1_holds() { return gG1.holds; }   // 0.0.627 (G2): n1c_open refuses every open while a test context holds the GPU (switch ON)
static uint32_t g1_cp_stat() { return RREG32(*gCtx->dev, SOC15_REG_OFFSET_BIDX(*gCtx->dev, IPBlock::GC, 0, 0x0F40u)); }   // CP_STAT (read only), as hang_log
static uint32_t g1_rptr() { sysmem_rmb(); return gCtx->cp.rptr_cpu ? *gCtx->cp.rptr_cpu : 0u; }

// NOP every ring dword outside the live range [gG1.liveStart, gWc). Caller holds gCliLock; the ring lock is taken here.
static void g1_scrub_ring() {
    CPContext &cp = gCtx->cp;
    IOLockLock(gRingLock);
    const uint32_t mask = cp.ring_ptr_mask;
    auto *ring = static_cast<volatile uint32_t *>(cp.ring_cpu);
    uint32_t n = 0;
    if (gWc - gG1.liveStart <= (uint64_t)cp.ring_size_dwords) {
        for (uint32_t i = 0; i < cp.ring_size_dwords; i++)
            if (!n48native::g1::in_live_range(i, gG1.liveStart, gWc, mask)) { ring[i] = cp_p3_nop1(); n++; }
    }
    sysmem_wmb();
    IOLockUnlock(gRingLock);
    N1C_LOG("g1: ring scrub: %u of %u dwords outside the live range [%llu, %llu) set to NOP", n, cp.ring_size_dwords,
            (unsigned long long)gG1.liveStart, (unsigned long long)gWc);
}

struct G1Env {
    BringupContext *ctx;
    uint64_t now_ns() { return amdgpu::now_ns(); }
    void relax(uint64_t el) { if (el < 5000000ull) IODelay(1); else IOSleep(1); }
    uint64_t retired() { return retired_now(emitted_now()); }

    // ---- hangtest ----
    uint32_t open_test() {
        const IOReturn rc = n1c_open(*ctx, false, &gG1.ref);
        if (rc == kIOReturnSuccess) { gG1.holds = true; gG1.resetAcked = false; gG1.attempts = 0; }
        return (uint32_t)rc;
    }
    uint32_t setup_bo() {
        using namespace n48native::g1;
        DeviceContext &dev = *gCtx->dev;
        IOLockLock(gCliLock);
        Session *s = &gSess;
        uint32_t rc = kOk;
        do {
            if (!s->open || s->id != gG1.ref.id || gG1.ref.slot != 0u || s->hello || s->nBos != 0u) { rc = kNotReady; break; }   // the context is ours: fresh, no client, no BO
            VRAMAllocation a {};
            if (!gCtx->gmc.vram_alloc.alloc(kTestBytes, kArenaBlockBytes, &a)) { rc = kNoMemory; break; }
            const uint64_t off = a.gpu_va - gCtx->gmc.vram_start;
            if (a.size < kTestBytes || off + a.size > dev.bar0Size) { gCtx->gmc.vram_alloc.free(a); rc = kNoMemory; break; }
            bar0_memset_vram(dev, off, 0, kTestBytes);
            Bo b {};
            b.kind = kBoVis; b.uc = true; b.size = kTestBytes; b.mc = a.gpu_va; b.a = a;
            s->bo[1] = b; s->boUsed[1] = 1; s->nBos = 1; s->vramBytes += kTestBytes;   // an ordinary BO of the session: close frees it, HUNG leaks it
            gG1.boOff = off;
            const uint32_t vmf = N48N_VM_PAGE_READABLE | N48N_VM_PAGE_WRITEABLE | N48N_VM_PAGE_EXECUTABLE | N48N_VM_MTYPE_UC;
            PtMem mem{ s };
            rc = pt_map(s->rootPa, kTestVa, kTestBytes / kPage, pa_of_mc(a.gpu_va), leaf_flags(vmf, true, false), mem);
            if (rc != kOk) break;
            s->maps[0] = VaEnt{ kTestVa, kTestBytes, 0, 1u, vmf };
            amdgpu_hdp_flush(dev);
            rc = flush_vmid(kNativeVmid);
        } while (false);
        IOLockUnlock(gCliLock);
        N1C_LOG("g1 hangtest: test BO %s (rc %#x): 16 KiB visible VRAM at BAR0 %#llx mapped R|W|X|UC at VMID-8 VA %#llx",
                rc == kOk ? "ready" : "NOT ready", rc, (unsigned long long)gG1.boOff, (unsigned long long)kTestVa);
        return rc;
    }
    n48native::g1::ProbeSub submit_hang() {
        using namespace n48native::g1;
        DeviceContext &dev = *gCtx->dev;
        ProbeSub ps{ kOk, 0ull, 0u };
        IOLockLock(gCliLock);
        uint32_t ib[kWaitDw];
        const uint32_t ndw = emit_wait_never(ib, kTestVa + kPollOff);
        bar0_memcpy_to_vram(dev, gG1.boOff + kHangIbOff, ib, ndw * 4u);
        amdgpu_hdp_flush(dev);
        const uint64_t va = kTestVa + kHangIbOff;
        const uint32_t by = ndw * 4u;
        uint64_t seq = 0;
        gG1.liveStart = gWc;
        ps.rc = ring_submit(&gSess, &va, &by, 1u, false, 0ull, &seq);   // 0.0.627: the one submit path (seqno, packet and ring under gRingLock), VMID 8 (slot 0)
        ps.seq = seq;
        gG1.hungSeq = seq;
        IOLockUnlock(gCliLock);
        N1C_LOG("g1 hangtest: WAIT_REG_MEM(equal %#x, poll VA %#llx) IB at VA %#llx submitted as seq %llu (rc %#x), ring [%llu, %llu)",
                kWaitRefNever, (unsigned long long)(kTestVa + kPollOff), (unsigned long long)va, (unsigned long long)seq, ps.rc,
                (unsigned long long)gG1.liveStart, (unsigned long long)gWc);
        return ps;
    }
    void latch_hang() {
        N1C_LOG("g1 hangtest: seq %llu did not retire in 2 s: latching HUNG through the client path (rptr %u, CP_STAT %#x)",
                (unsigned long long)gG1.hungSeq, g1_rptr(), g1_cp_stat());
        hang_from_wait();
    }
    void close_test(const char *how) {
        n1c_close(gG1.ref, how);
        gG1.holds = false;
    }

    // ---- hangrecover ----
    uint32_t act(uint32_t method, bool resetAcked) {
        using namespace n48native::g1;
        DeviceContext &dev = *gCtx->dev;
        CPContext &cp = gCtx->cp;
        MESContext &mes = gCtx->mes;
        if (method == kMethRelease) {
            IOLockLock(gCliLock);
            const uint32_t v = kWaitRefNever;
            bar0_memcpy_to_vram(dev, gG1.boOff + kPollOff, &v, sizeof(v));
            amdgpu_hdp_flush(dev);
            IOLockUnlock(gCliLock);
            N1C_LOG("g1 hangrecover release: the CPU wrote %#x to the poll dword (a harness control: the WAIT_REG_MEM may now end)", v);
            return kOk;
        }
        if (method == kMethMesReset) {
            IOLockLock(gCliLock);
            g1_scrub_ring();
            IOLockUnlock(gCliLock);
            const kern_return_t r = mes_reset_legacy_gfx_queue(dev, mes, 0u, 0u, kNativeVmid, cp.mqd_bus, cp.doorbell_index, cp.wptr_gpu_addr);
            gG1.resetAcked = (r == kIOReturnSuccess);   // 0.0.626 (F2): the MOST RECENT RESET's verdict (a later failed RESET clears it), never sticky-true
            N1C_LOG("g1 hangrecover mesreset: MES RESET(reset_legacy_gfx, pipe 0 queue 0 vmid %u) %s (%#x); rptr %u CP_STAT %#x", kNativeVmid,
                    r == kIOReturnSuccess ? "ACKED" : "NOT acked", r, g1_rptr(), g1_cp_stat());
            return (uint32_t)r;
        }
        if (method == kMethRemap) {
            const bool after = unmap_after_reset(resetAcked || gG1.resetAcked, mes.sched_version & kMES_VERSION_MASK);
            const uint32_t fl = kRemoveQueueFlag_unmap_legacy_queue | (after ? kRemoveQueueFlag_remove_after_reset : 0u);
            kern_return_t r = mes_remove_hw_queue_flags(dev, mes, kMESQueueType_GFX, 0u, 0u, cp.doorbell_index, fl);
            N1C_LOG("g1 hangrecover remap: MES REMOVE_QUEUE(GFX pipe 0 queue 0, flags %#x) %s (%#x)", fl, r == kIOReturnSuccess ? "ACKED" : "NOT acked", r);
            if (r != kIOReturnSuccess) return (uint32_t)r;    // the queue's state is unknown: no re-init, no ADD_QUEUE
            cp.kgq_mapped = false; gCtx->kgqMapped = false;
            // amdgpu_gfx_mqd_reset_restore: ring->wptr = 0, *wptr_cpu_addr = 0, amdgpu_ring_clear_ring (the whole ring to NOP).
            IOLockLock(gCliLock);
            IOLockLock(gRingLock);
            auto *ring = static_cast<volatile uint32_t *>(cp.ring_cpu);
            for (uint32_t i = 0; i < cp.ring_size_dwords; i++) ring[i] = cp_p3_nop1();
            *cp.rptr_cpu = 0u;
            *reinterpret_cast<volatile uint64_t *>(static_cast<uint8_t *>(cp.wb_cpu) + kCPWBOffsetWptr) = 0ull;
            gWc = 0ull; cp.wptr = 0u; gG1.liveStart = 0ull;
            sysmem_wmb();
            IOLockUnlock(gRingLock);
            IOLockUnlock(gCliLock);
            r = cp_gfx_mqd_init(dev, cp);                       // memory only: the MQD as the boot built it (rptr 0, wptr 0, active 1)
            if (r == kIOReturnSuccess) r = cp_map_gfx_kgq_mes(dev, cp, mes);   // MES ADD_QUEUE(map_legacy_kq), as PM4Test at boot
            if (r == kIOReturnSuccess) gCtx->kgqMapped = true;
            N1C_LOG("g1 hangrecover remap: ring cleared, counters 0, MQD re-initialised, ADD_QUEUE(map_legacy_kq) %s (%#x); rptr %u CP_STAT %#x",
                    r == kIOReturnSuccess ? "ACKED" : "NOT acked", r, g1_rptr(), g1_cp_stat());
            return (uint32_t)r;
        }
        return kBadArg;
    }
    n48native::g1::ProbeSub submit_probe(uint32_t idx) {
        using namespace n48native::g1;
        DeviceContext &dev = *gCtx->dev;
        ProbeSub ps{ kOk, 0ull, probe_magic(idx) };
        IOLockLock(gCliLock);
        const uint32_t zero = 0u;
        bar0_memcpy_to_vram(dev, gG1.boOff + kProbeOff, &zero, sizeof(zero));
        uint32_t ib[5];
        ib[0] = pm4_header(kPM4OpWriteData, 3u);
        ib[1] = pm4_write_data_control(kPM4WriteDataEngineME, kPM4WriteDataDstSelMemory, true);
        ib[2] = (uint32_t)((kTestVa + kProbeOff) & 0xFFFFFFFFull);
        ib[3] = (uint32_t)((kTestVa + kProbeOff) >> 32);
        ib[4] = ps.magic;
        bar0_memcpy_to_vram(dev, gG1.boOff + kProbeIbOff, ib, sizeof(ib));
        amdgpu_hdp_flush(dev);
        const uint64_t va = kTestVa + kProbeIbOff;
        const uint32_t by = (uint32_t)sizeof(ib);
        uint64_t seq = 0;
        ps.rc = ring_submit(&gSess, &va, &by, 1u, false, 0ull, &seq);   // 0.0.627: the one submit path
        ps.seq = seq;
        IOLockUnlock(gCliLock);
        return ps;
    }
    uint32_t probe_data() { return RBAR0_32(*gCtx->dev, gG1.boOff + n48native::g1::kProbeOff); }
    // The ONLY place HUNG is cleared. recover_run calls it only when may_clear (this attempt's probe retired AND read back).
    void clear_latch() {
        const uint64_t em = emitted_now(), re = retired_now(em);
        IOLockLock(gHangLock);
        gHang.hung = false;
        gHang.lastRetired = re;
        gHang.tProgress = amdgpu::now_ns();
        IOLockUnlock(gHangLock);
        N1C_LOG("g1: HUNG CLEARED after a verified probe (seq emitted %llu retired %llu); Navi48,Ready on the Metal nub stays 0 until reboot (sticky)",
                (unsigned long long)em, (unsigned long long)re);
    }
    void note(const n48native::g1::Attempt &a) {
        const uint32_t k = gG1.attempts;
        if (k < n48native::g1::kMaxAttempts) { gG1.att[k] = a; gG1.attRptr[k] = g1_rptr(); gG1.attCpStat[k] = g1_cp_stat(); gG1.attempts = k + 1u; }
        N1C_LOG("g1 attempt %u: method %u status %u act rc %#x (%llu us), probe rc %#x seq %llu retired %d data %d (%llu us); rptr %u CP_STAT %#x",
                k, a.method, a.status, a.actRc, (unsigned long long)(a.actNs / 1000ull), a.probeRc, (unsigned long long)a.probeSeq, (int)a.retired,
                (int)a.dataOk, (unsigned long long)(a.verifyNs / 1000ull), g1_rptr(), g1_cp_stat());
    }
};

static void g1_summary(uint64_t *o) {
    n48native::g1::Summary s {};
    const uint64_t em = emitted_now();
    s.lastCode = gG1.lastCode; s.phase = gG1.holds ? 1u : 0u; s.hung = hung_now(); s.testHolds = gG1.holds;
    s.hungSeq = gG1.hungSeq; s.emitted = em; s.retired = gCtx ? retired_now(em) : 0ull; s.wc = gWc;
    s.attempts = gG1.attempts; s.lastMethod = gG1.lastMethod; s.tests = gG1.tests; s.recovered = gG1.recovered; s.failedRuns = gG1.failedRuns;
    n48native::g1::summary_out(s, o);
}

// The three verbs (accel actions 100..102). The caller (accelExperiment) has checked the latch and the action bound; `o` has kOutN scalars.
// Returns the IOReturn for the user client; the verdict rides in o[0].
IOReturn n1c_g1_verb(uint32_t action, uint64_t arg, BringupContext &ctx, uint64_t *o) {
    using namespace n48native::g1;
    for (uint32_t i = 0; i < kOutN; i++) o[i] = 0;
    if (!args_ok(action, arg)) return kIOReturnBadArgument;
    if (lazy_lock(&gG1Lock) == nullptr) return kIOReturnNoMemory;
    IOLockLock(gG1Lock);
    IOReturn ret = kIOReturnSuccess;
    if (action == kActHangStat) {
        if (gCtx == nullptr) { o[0] = 0; }
        else if (arg == 0ull) g1_summary(o);
        else {
            const uint32_t k = (uint32_t)arg - 1u;
            if (k < gG1.attempts) attempt_out(gG1.att[k], gG1.attRptr[k], gG1.attCpStat[k], o);
        }
    } else if (action == kActHangTest) {
        const bool open = __atomic_load_n(&gOpenFlag, __ATOMIC_SEQ_CST) != 0u;
        const bool ws = open && __atomic_load_n(&gSessWs, __ATOMIC_SEQ_CST) != 0u;
        const uint32_t d = test_decide(n1c_g1_latched_on(), (uint32_t)arg, gG1.holds, open, ws, hung_now());
        if (d != kTestOk) {
            N1C_LOG("g1 hangtest REFUSED (%u): %s", d, d == kTestWsSession ? "a WindowServer native session is open" : d == kTestActive ? "a test context already holds the GPU"
                    : d == kTestSessionOpen ? "a native session is open" : d == kTestHung ? "the GPU is HUNG" : d == kTestBadMode ? "unknown mode" : "boot-arg navi48-g1 is not 1");
            o[0] = d; gG1.lastCode = d;
        } else {
            gG1.tests++;
            G1Env e{ &ctx };
            const TestRep t = hangtest_run(e);
            o[0] = t.outcome; o[1] = t.rc; o[2] = t.seq;
            gG1.lastCode = t.outcome;
            if (gCtx != nullptr) {
                const uint64_t em = emitted_now();
                o[3] = em; o[4] = retired_now(em); o[5] = g1_cp_stat(); o[6] = g1_rptr(); o[7] = gWc; o[8] = hub_rd(0x15d0);
            }
            N1C_LOG("g1 hangtest: outcome %u (rc %#x, seq %llu)", t.outcome, t.rc, (unsigned long long)t.seq);
        }
    } else {   // kActHangRecover
        const Plan p = plan_for((uint32_t)arg);
        const uint32_t d = recover_decide(n1c_g1_latched_on(), (uint32_t)arg, gG1.holds, hung_now(), gG1.attempts, p.n, mes_timeout_seen(), gG1.resetAcked);   // 0.0.626 F2/F3
        if (d != kRecOk) {
            N1C_LOG("g1 hangrecover %llu REFUSED (%u): %s", (unsigned long long)arg, d, d == kRecNoTest ? "no test context holds a hang (run hangtest 1 first)"
                    : d == kRecNotHung ? "HUNG is not latched" : d == kRecTooMany ? "the attempt budget for this hang is spent (abandon, or reboot)"
                    : d == kRecMesTimedOut ? "a MES frame timed out earlier in this boot: no further MES frame (only release and abandon remain)"
                    : d == kRecRemapNeedsReset ? "remap needs the most recent MES RESET on this hang to have been acknowledged (run mesreset first)"
                    : d == kRecBadMethod ? "unknown method" : "boot-arg navi48-g1 is not 1");
            o[0] = d; gG1.lastCode = d;
        } else if ((uint32_t)arg == kMethAbandon) {
            G1Env e{ &ctx };
            e.close_test("g1 hangrecover: abandoned (HUNG stays latched)");
            o[0] = kRecAbandoned; gG1.lastCode = kRecAbandoned; gG1.lastMethod = kMethAbandon; gG1.failedRuns++;
        } else {
            G1Env e{ &ctx };
            const RecoverRep r = recover_run(e, (uint32_t)arg, gG1.attempts);
            gG1.lastMethod = (uint32_t)arg; gG1.lastCode = r.outcome;
            if (r.cleared) gG1.recovered++; else gG1.failedRuns++;
            o[0] = r.outcome; o[1] = r.n;
            for (uint32_t i = 0; i < r.n && i < kMaxPlan; i++) { o[2 + 3 * i] = r.a[i].status; o[3 + 3 * i] = r.a[i].actRc; o[4 + 3 * i] = r.a[i].verifyNs / 1000ull; }
            const uint64_t em = emitted_now();
            o[8] = r.cleared ? 1u : 0u; o[9] = hung_now() ? 1u : 0u; o[10] = em; o[11] = retired_now(em); o[12] = gG1.attempts;
            N1C_LOG("g1 hangrecover %llu: outcome %u after %u attempt(s); HUNG %s", (unsigned long long)arg, r.outcome, r.n,
                    hung_now() ? "STILL LATCHED (the test context keeps the GPU; try another method, hangrecover 4, or reboot)" : "cleared");
        }
    }
    IOLockUnlock(gG1Lock);
    return ret;
}

} // namespace amdgpu
