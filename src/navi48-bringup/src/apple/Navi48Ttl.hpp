//
//  Navi48Ttl.hpp — our implementation of the hardware-library interface that
//  Apple's AMDRadeonX6000 accelerator consumes.
//
//  Apple's own implementation (AmdTtlServices, in AMDRadeonX6000HWLibs) stops
//  at Navi 23: its device table has no entry past 0x73FF and its firmware
//  vocabulary predates MES entirely, so TTL::initialize() cannot succeed on
//  gfx1201. But the accelerator above it is generation-agnostic — it links only
//  against IOKit and reaches silicon through this one interface, 16 methods of
//  it. Those 16 jobs are exactly what the bring-up ladder already does.
//
//  So we answer them from BringupContext instead, and hand Apple's accelerator
//  our object in place of Apple's. See notes/APPLE-DRIVER-VERDICT.md.
//
//  STATUS: first iteration. The methods whose output layout we recovered from
//  the accelerator's own disassembly are implemented for real; the rest log
//  their arguments and return kTtlUnsupported. That is deliberate — the point
//  of the first boot is to learn the call order and arguments, not to succeed.
//
#pragma once
#include "AmdTtlServicesABI.h"
#include "../amd/amdgpu_sysmem.h"
#include "gfx_mmprio.h"   // 0.0.433 (notes/design/MM-PRIORITY.md): n48_mmprio_stats, the pure decision logic
#include "gfx_mmhold.h"    // build 0.0.514 A3: the gVramMmLock hold-time taker tags (N48_MMT_*)
#include "gfx_perf540.h"   // build 0.0.540: switch 96, perf540 (the ext accumulators' type, n48_pf_ext)
#include "gfx_copyguard.h" // 0.0.435 (notes/design/PGMID-COPYGUARD.md Part 2): the copy-overlap refusal's pure logic
#include "gfx_sk82.h"      // build 0.0.527 (notes/design/SKIP82.md): switch 82, the byte-identical re-copy skip (pure)
#include "gfx_cg84.h"      // build 0.0.529 (notes/design/CG84.md): switch 84, the granule copy guard + delta write (pure)
#include "gfx_wc98.h"      // build 0.0.541: switch 98, the provenance-ask walk cache (pure): the generation

namespace amdgpu { struct DeviceContext; struct BringupContext; }
class Navi48Bringup;

// The GART aperture the bring-up driver actually programmed. False before the
// GFXHUB GART is enabled. Defined in Navi48Bringup.cpp.
bool navi48_gart_aperture(uint64_t &start, uint64_t &size);

// Mirror one of Apple's GART mappings into our page table in GFX12 PTE format.
// Defined in Navi48Bringup.cpp.
bool navi48_gart_bind_at(uint64_t gartAddr, uint64_t busAddr, uint64_t sizeBytes);
// Read one PTE back out of our GFX12 GART table. Residency check for
// step 2: on GFX12 a live entry needs BOTH PTEFlags::VALID and IS_PTE (bit 63).
bool navi48_gart_read_pte(uint64_t gartAddr, uint64_t &pteOut);
// Read up to one page out of a GART-mapped buffer (for dumping the IB an
// INDIRECT packet points at). Read-only, single page, and refuses unless the
// PTE already reads VALID|IS_PTE.
bool navi48_gart_read_page(uint64_t gartAddr, void *dst, uint32_t len);
// Write up to one page into a GART-mapped buffer. Same single-page
// limit and same residency refusal as the read. Defined in Navi48Bringup.cpp.
bool navi48_gart_write_page(uint64_t gartAddr, const void *src, uint32_t len);
// Read/write a GART range that may span pages (each page's PTE resolved and
// residency-checked separately). Defined in Navi48Bringup.cpp.
bool navi48_gart_read_range(uint64_t gartAddr, void *dst, uint32_t len);
bool navi48_gart_write_range(uint64_t gartAddr, const void *src, uint32_t len);
// ---- 0.0.178 "gfxmap": handing GFX pipe0/queue0 to Apple's ring -----------
//
// Our kernel GFX queue's doorbell DWORD index — cp.doorbell_index, which is the
// value the ladder handed MES as ADD_QUEUE.doorbell_offset and the value
// cp_kick_doorbell turns into a BAR2 byte offset as index * 4. False if the CP
// never initialised. MEASURED on this machine: 139 (0x8B = gfx_ring0 in the
// upstream Navi10 map), NOT 0 — `navi48test info` prints "GFX ring doorbell 139".
bool navi48_kgq_doorbell_index(uint32_t &indexOut);
// The REAL BAR2 doorbell dword for a DWORD index: dev.bar2 + index * 4. Refuses
// an index outside the mapped aperture, and refuses the indices the ladder's own
// engines own (MES ring 0x20-scale, the aggregated block, SDMA) so a mistake
// cannot hand Apple a doorbell another engine is listening on.
bool navi48_doorbell_dword_ptr(uint32_t index, void **ptrOut, uint64_t *byteOffOut);
// One HDP flush through the NBIO remap window — the flush Apple would have done
// with the regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL pair our hook blocks
// (dwords 0xe17 <- 1 / 0xe08 <- 0), so the GPU sees Apple's CPU writes.
bool navi48_hdp_flush_now(void);
// Clear-state buffer for TTL slot 13: one GART page of 1-dword NOPs, 64 bytes reported
// (0.0.181). Returns false and 0/0 if the GART is not ready.
bool navi48_csb_publish(uint64_t *mcOut, uint32_t *sizeOut);
// 0.0.359: a READ-ONLY view of that page for the render drain's arm-time walk - its GART MC address, the 64 bytes
// reported to Apple, and our own CPU mapping of it. Never publishes, never writes; false when nothing was published.
bool navi48_csb_peek(uint64_t *mcOut, uint32_t *sizeOut, const volatile uint32_t **cpuOut, uint32_t *pageDwordsOut);
// 0.0.368: RULE E1's hardware proof - the GART PTE the GPU walks for the clear-state page, read back out of
// the page table, and the value gmc_bind_existing wrote (our bus address | PTEFlags::SYSMEM_RW). Read-only; false when the
// page was never published or the entry could not be read, which refuses.
bool navi48_csb_pte_read(uint64_t *pteOut, uint64_t *wantOut);


// The whole GFX takeover, in the one place that owns the bring-up context.
// Bounded: the MES calls are the existing ones (REMOVE 1 s, ADD 2 s).
struct Navi48GfxTakeover {
    // ---- in
    uint64_t ringGpu        { 0 };   // Apple's GFX ring, GART MC
    uint32_t ringBytes      { 0 };   // 0x80000
    uint64_t wptrPollGpu    { 0 };   // MAP_QUEUES dw5/dw6
    uint64_t rptrReportGpu  { 0 };   // Apple's ring+0xb8 / its MQD dwords 139-140
    uint32_t doorbellIndex  { 0 };   // DWORD index, ours
    // ---- out
    uint32_t pagesChecked   { 0 };
    uint32_t pagesResident  { 0 };
    uint64_t mqdGpu         { 0 };
    uint32_t rbBufsz        { 0 };
    uint32_t hqdCntl        { 0 };
    uint32_t doorbellCtl    { 0 };
    uint64_t wptrPollValue  { 0 };   // what the wptr write-back held before the map
    int32_t  removeKr       { 0 };
    int32_t  addKr          { 0 };
    uint32_t mqdActive      { 0 };
    uint32_t mqdMapped      { 0 };
    uint32_t mqdRptr        { 0 };
    uint32_t mqdWptr        { 0 };
    // 0 = mapped. 1 not ready, 2 a page is not resident, 3 the wptr write-back
    // is bogus, 4 REMOVE_QUEUE not acked, 5 the MQD build refused, 6 ADD_QUEUE
    // not acked.
    uint32_t failStep       { 0 };
};
bool navi48_gfx_takeover(Navi48GfxTakeover &t);
// MES REMOVE_QUEUE(GFX, pipe 0, queue 0, doorbellIndex). Used for the unwind
// when Apple sends UNMAP_QUEUES for the queue we mapped.
bool navi48_gfx_remove_queue(uint32_t doorbellIndex, int32_t &krOut);

// GC IP base (BASE_IDX 0) from IP discovery; 0 if unresolved.
uint32_t navi48_gc_base(void);
// 0.0.194: GC IP base for an arbitrary register segment (BASE_IDX); 0 if the
// segment is absent. BASE_IDX 1 holds the CP IB registers.
uint32_t navi48_gc_base_seg(uint8_t seg);
//: absolute-dword MMIO register access, for callers with no DeviceContext.
uint32_t navi48_reg_read32(uint32_t reg);
void     navi48_reg_write32(uint32_t reg, uint32_t value);
// 0.0.193: read `dwords` dwords of VRAM at `vramOffset` (0-based, the same
// address space Apple's arena lives in) through the MM_INDEX/MM_DATA window —
// the GPU's own view, not a CPU copy, and the only way to see a page-table
// entry Apple wrote with SDMA. READ-ONLY, bounds-checked against the measured
// VRAM size, and capped at 64 dwords. Defined in Navi48Bringup.cpp.
bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords);
// 0.0.201: WRITE up to 64 dwords at a 0-based VRAM offset through the same window,
// serialised with the read above. Only the residency copy calls it, after
// navi48_vram_apple_dest_check. Defined in Navi48Bringup.cpp.
bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords);
// build 0.0.512 Part B2 (gfx_clock88.h): THE CLOCK88 WATCH, read-only. navi48_c88_watch_add: the policy pass sets a watched
// surface (GPU VA, extent, its first page's VRAM when resolved); 1 = added. navi48_c88_copy_note: the residency copier, after its
// COPIED line, names its destination (GPU VA, bytes, VRAM, copy number) - one line when it overlaps a watch. navi48_c88_watch_state:
// the report's numbers. The MM-window writes are watched inside navi48_vram_write_mm itself. Defined in Navi48Bringup.cpp.
// build 0.0.546: navi48_c88_watch_add takes the surface's context sequence (0 = unknown); navi48_c88_unmap, called by
// hook_unmapVA for every recorded context, turns a watch whose VA range that context unmapped DEAD (one line), so a later resource at
// the same VA is never reported as a HIT on it. Log only.
uint32_t navi48_c88_watch_add(uint32_t ctx, uint64_t va, uint64_t len, uint64_t vram, uint32_t vramOk, uint32_t *idx);
void navi48_c88_unmap(uint32_t ctx, uint64_t va, uint64_t size);
void navi48_c88_copy_note(uint64_t dVa, uint32_t dVaOk, uint64_t bytes, uint64_t vram, uint64_t copyNo);
void navi48_c88_watch_state(uint32_t *n, uint64_t *va0, uint64_t *va1, uint32_t *lines, uint64_t *unlogged);
// build 0.0.524 item 7 (notes/design/T0SRC.md): resources of a SECOND, different resource class that hook_new_resource left
// alone (they keep Apple's own pageTexture). A count, read-only; the class is named once per boot by Navi48AccelPeer.cpp.
uint64_t navi48_peer_res2_count(void);
// build 0.0.535 item 3: the residency copies COMPLETED so far (the `COPIED #` counter, read-only; Navi48AccelPeer.cpp).
uint64_t navi48_peer_copy_seq(void);
// 0.0.433 (notes/design/MM-PRIORITY.md) — MM-WINDOW PRIORITY FOR THE POLICY PASS. Opens/closes the
// scope that marks which thread owns gVramMmLock priority; nesting/owner arithmetic is gfx_mmprio.h's pure,
// host-tested functions. Called ONLY by Navi48MmPrioScope's ctor/dtor below, and (build 0.0.550, switch 108) by
// Navi48MmPrioRelease's ctor/dtor below, in the reverse order. Defined in Navi48Bringup.cpp.
void navi48_mm_prio_enter(void);
void navi48_mm_prio_exit(void);
// `accel gfxneuter 37 | M << 8`: M 1 turns MM-window priority ON, M 0xFF turns it OFF, mode 0 reads without
// changing anything. Returns the switch's state AFTER this call (1 ON, 0 OFF). Defined in Navi48Bringup.cpp.
uint32_t navi48_mm_prio_switch(uint32_t mode);
// A plain copy of the live owner-wait/yield counters, for AppleHardwareHook.cpp's always-on `mmprio:` report line.
// Defined in Navi48Bringup.cpp.
void navi48_mm_prio_snapshot(n48_mmprio_stats *out);

// 0.0.433 — gfxsrc_policy's ENTIRE scope (AppleHardwareHook.cpp), constructed as that function's FIRST statement so
// nesting/owner are exact across every return path, including the early `if (!gXdOut) return;`, as well as the
// normal fall-through. RAII rather than manual enter/exit calls so a future early return can never leave the scope
// open. Not copyable: two scopes must never alias one nesting count.
struct Navi48MmPrioScope {
    Navi48MmPrioScope()  { navi48_mm_prio_enter(); }
    ~Navi48MmPrioScope() { navi48_mm_prio_exit(); }
    Navi48MmPrioScope(const Navi48MmPrioScope &) = delete;
    Navi48MmPrioScope &operator=(const Navi48MmPrioScope &) = delete;
};
// build 0.0.550 (switch 108, gfx_cgw108.h) - THE INVERSE, for ONE bounded wait inside gfxsrc_policy's scope: the ctor closes
// the pass's MM-window priority (nesting -> 0: no caller yields while it stands) and the dtor re-opens it (0 -> 1: the owner is
// this thread again, exactly as at the pass's top). Used ONLY around switch 108's IN_FLIGHT wait (gfxsrc_cg_redo), so a copier the
// pass is waiting on is not made to yield to the pass. It takes no lock and touches only the nesting/owner pair. Not copyable.
struct Navi48MmPrioRelease {
    Navi48MmPrioRelease()  { navi48_mm_prio_exit(); }
    ~Navi48MmPrioRelease() { navi48_mm_prio_enter(); }
    Navi48MmPrioRelease(const Navi48MmPrioRelease &) = delete;
    Navi48MmPrioRelease &operator=(const Navi48MmPrioRelease &) = delete;
};

// build 0.0.514 A3 — WHO HOLDS gVramMmLock: READ-ONLY. navi48_mm_taker_push names the calling thread's MM
// taker (N48_MMT_*, gfx_mmhold.h) until the matching pop; navi48_vram_read_mm / write_mm file each hold under the innermost
// open tag. Called ONLY by Navi48MmTakerScope below. Defined in Navi48Bringup.cpp.
// build 0.0.514 B1/B4: RDNA4FB's live Console,Width/Height (1 = read; else *w/*h untouched). Read-only.
// Defined in Navi48Bringup.cpp.
uint32_t navi48_scanout_live_dims(uint32_t *w, uint32_t *h);
uint32_t navi48_mm_taker_push(uint32_t tag);
void navi48_mm_taker_pop(uint32_t tok);
struct Navi48MmTakerScope {
    explicit Navi48MmTakerScope(uint32_t tag) : tok_(navi48_mm_taker_push(tag)) {}
    ~Navi48MmTakerScope() { navi48_mm_taker_pop(tok_); }
    Navi48MmTakerScope(const Navi48MmTakerScope &) = delete;
    Navi48MmTakerScope &operator=(const Navi48MmTakerScope &) = delete;
private:
    uint32_t tok_;
};
// build 0.0.515 D2: a DECIDE SUB-TAG over one site (gfx_mmhold.h N48_MMT_D_*): only where this thread's open
// tag is DECIDE or a DECIDE sub-tag, else nothing changes. RAII, so every return path restores the outer tag. Read-only.
uint32_t navi48_mm_taker_push_sub(uint32_t sub);
struct Navi48MmSubScope {
    explicit Navi48MmSubScope(uint32_t sub) : tok_(navi48_mm_taker_push_sub(sub)) {}
    ~Navi48MmSubScope() { navi48_mm_taker_pop(tok_); }
    Navi48MmSubScope(const Navi48MmSubScope &) = delete;
    Navi48MmSubScope &operator=(const Navi48MmSubScope &) = delete;
private:
    uint32_t tok_;
};
// build 0.0.540 (T9; gfx_perf540.h) — SWITCH 96, perf540. gN48Pf540On: the switch (written ONLY by the
// `gfxneuter 96` verb in AppleHardwareHook.cpp; OFF at boot). navi48_mm_taker_now: the calling thread's open MM tag (read-only).
// navi48_pf540_ext_snapshot: a copy of navi48_vram_read_mm's per-tag call time (T5) and n48_logf's split (T9).
// navi48_pf540_hook_enter/exit: while ON, the GFX hook's thread is registered so n48_logf can tell a hook thread's lines from
// others'. navi48_pf540_log_note: n48_logf's three phases (amd/n48log.cpp; ON only). All defined in Navi48Bringup.cpp.
extern volatile uint32_t gN48Pf540On;
// build 0.0.541 (gfx_wc98.h) — SWITCH 98's GENERATION: moved by every bump site (hook_unmapVA / hook_mapVA brackets,
// navi48_cg_open / navi48_cg_close, the keystone's root[511] writes, rebinds, context releases, the ws-valid base write, the SDMA
// submit). Defined in Navi48Bringup.cpp; read by AppleHardwareHook.cpp's gfxc_page_wc. Moving it only ever invalidates.
extern n48_wc_gen gN48WcGen;
uint32_t navi48_mm_taker_now(void);
void navi48_pf540_ext_snapshot(n48_pf_ext *out);
uint32_t navi48_pf540_hook_enter(void);
void navi48_pf540_hook_exit(uint32_t slot);
// A JUDGE sub-tag over one site (gfx_mmhold.h N48_MMT_J_*), ONLY while switch 96 is ON: OFF, one load of the switch and nothing
// else (no call, no table). ON, Navi48MmSubScope's rule: pushed only where this thread's open tag is JUDGE or a JUDGE sub-tag.
struct Navi48PfSubScope {
    explicit Navi48PfSubScope(uint32_t sub) : tok_(__atomic_load_n(&gN48Pf540On, __ATOMIC_RELAXED) ? navi48_mm_taker_push_sub(sub) : 0u) {}
    ~Navi48PfSubScope() { if (tok_) navi48_mm_taker_pop(tok_); }
    Navi48PfSubScope(const Navi48PfSubScope &) = delete;
    Navi48PfSubScope &operator=(const Navi48PfSubScope &) = delete;
private:
    uint32_t tok_;
};

// 0.0.435 (notes/design/PGMID-COPYGUARD.md Part 2) — THE COPY-OVERLAP REFUSAL. Open/close one residency
// copy's scope over [lo, hi): claims a slot, publishes BEGIN, and on close publishes END, poisons on failure/
// mismatch (a later clean covering copy clears it) and releases the slot — see gfx_copyguard.h for the pure
// arithmetic and the binding order this depends on. Returns the slot index (>= 0), or -1 (UNTRACKED: no slot was
// free, the copy still ran, and every reader check refuses until this scope closes). Defined in Navi48Bringup.cpp.
int32_t navi48_cg_open(uint64_t lo, uint64_t hi);
// 0.0.435 review fix — `wrote`: false ONLY for a scope that never writes a byte (the switch-39 phantom-scope
// positive control); such a close must neither mark nor clear poison (gfx_copyguard.h's n48_cg_close_poison is the
// pure decision; this wraps it with the ring END push and the slot release). Every real copy passes wrote = true.
void navi48_cg_close(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch);
// The RAII wrapper. Open the scope BEFORE the copy's first MM access (Navi48AccelPeer.cpp's residency_copy_to_vram
// and, separately, the standalone substitute_blit_kernel_at verb call); it must enclose shadercache_scan_resource
// and substitute_blit_kernel_at. Call markFailed()/markMismatch() before returning on either condition so CLOSE
// poisons the range; not copyable, so two scopes can never alias one slot. `wrote` defaults true for every real
// caller; only navi48_cg_phantom_scope passes false.
// build 0.0.495 (switch 62, gfx_heapgen.h): the copy scope's close is also the in-copy substitution's completion - the
// post-copy substitution and kernsub have run inside the scope by then. navi48_cg_close_scope = navi48_cg_close, then
// navi48_ic_scope_closed: one call from the destructor (two inlined calls grew hook_page_texture's frame by 0x20). The latter is
// a no-op for every scope but the one a residency copy opened on this thread while 62 was ON (the standalone kernsub scope, the
// phantom control and every copy with 62 OFF return at once). navi48_vram_write_mm's self-scope still calls navi48_cg_close
// directly, so it can never complete a copy. Both defined in Navi48AccelPeer.cpp.
void navi48_ic_scope_closed(uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch, bool sampled);
void navi48_cg_close_scope(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch);
struct Navi48CopyScope {
    Navi48CopyScope(uint64_t lo, uint64_t hi, bool wrote = true) : lo_(lo), hi_(hi), wrote_(wrote), slot_(navi48_cg_open(lo, hi)) {}
    ~Navi48CopyScope() { navi48_cg_close_scope(slot_, lo_, hi_, wrote_, failed_, mismatch_); }
    void markFailed()   { failed_ = true; }
    void markMismatch() { mismatch_ = true; }
    // 0.0.438: a refusal at pos == 0 - nothing this copy has written yet - must close with
    // wrote = false, exactly like the switch-39 phantom scope, so CLOSE neither marks nor clears poison over a
    // range this scope never touched. Callable any time before destruction; only a refusal that landed before the
    // first byte ever hit VRAM should use it.
    void markNothingWritten() { wrote_ = false; }
    Navi48CopyScope(const Navi48CopyScope &) = delete;
    Navi48CopyScope &operator=(const Navi48CopyScope &) = delete;
private:
    uint64_t lo_, hi_;
    bool wrote_;
    int32_t slot_;
    bool failed_ { false };
    bool mismatch_ { false };
};
// The reader side, called once per segment around gfxsrc_policy's `xlat12_ib_translate_draw_ex` (AppleHardwareHook.cpp).
// navi48_cg_seg_begin resets the active page recorder and marks the ring position (before the translate call);
// navi48_cg_active_recorder is what gfxc_read_rs feeds during it (descriptor-read and program-identity callbacks
// only); navi48_cg_seg_check runs the ordered binding check afterward and returns N48_CG_OK or a refusal reason.
// Defined in Navi48Bringup.cpp.
void navi48_cg_seg_begin(void);
n48_cg_pagerec *navi48_cg_active_recorder(void);
uint32_t navi48_cg_seg_check(void);
// build 0.0.523 (switch 78, gfx_copyguard.h n48_cg_redo_*): what the last counted check refused on; the uncounted peek;
// the recorder's count and the pass mark; the ring (read-only, for n48_cg_redo_seq's mark); the IN_FLIGHT wait's poll.
const n48_cg_why *navi48_cg_last_why(void);
uint32_t navi48_cg_seg_peek(n48_cg_why *why);
uint32_t navi48_cg_rec_count(void);
uint64_t navi48_cg_pass_since(void);
const n48_cg_ring *navi48_cg_ring_ptr(void);
int navi48_cg_rec_inflight(void);
// A plain copy of the live counters, for the always-on `cguard:` report line (AppleHardwareHook.cpp). Defined in
// Navi48Bringup.cpp.
void navi48_cg_snapshot(n48_cg_stats *out);
// `accel gfxneuter 39 | 1 << 8` (AppleHardwareHook.cpp dispatch): the positive control. Opens a phantom scope over
// the whole of VRAM for 2 s, writing nothing, so a run can confirm the reader check refuses IN_FLIGHT for its
// whole duration and returns to baseline the moment it closes ("confirm free by census"). Blocking; an explicit
// verb call, never a per-frame path. Defined in Navi48Bringup.cpp.
void navi48_cg_phantom_scope(void);
// 0.0.436 (notes/design/PGMID-COPYGUARD.md Part 1, design "2. M") — THE PER-PASS PROGRAM-IDENTITY
// MEMO'S OWN VALIDITY READS. gfx_pgmid.h's n48_pm needs the copy-guard ring's CURRENT position and a poison-overlap
// answer for a single 4 KiB page, at arbitrary points within a pass (not only at navi48_cg_seg_begin's per-segment
// mark) — these two thin reads are all it needs of this file's otherwise-private gCgRing/gCgPoison state. Defined
// in Navi48Bringup.cpp.
uint64_t navi48_cg_ring_mark_now(void);
int navi48_cg_poison_overlaps_page(uint64_t page);

// 0.0.201: 0 when VRAM [off, off+len) lies inside our unused device-only hi pool,
// else a reason code (see the definition in Navi48Bringup.cpp).
uint32_t navi48_vram_apple_dest_check(uint64_t off, uint64_t len);
// 0.0.201, action 44 `pagecopy [1]`: arm (arm=true) the residency copy for this boot
// and/or read its counters into out[0..count). Returns the state bits (1 armed,
// 2 skip-pagecopy hook active, 4 a resource vtable is patched). Defined in
// Navi48AccelPeer.cpp.
uint32_t navi48_pagecopy_control(bool arm, uint64_t *out, unsigned count);
// 0.0.284: mode 0 read, 1 arm the page-in copy, 2 arm page-in AND page-out copies.
uint32_t navi48_pagecopy_control2(uint32_t mode, uint64_t *out, unsigned count);
// 0.0.239, action 51 `shadercache [1|2]`: MILESTONE 3 step 2. Arm (mode 1) the
// hash-keyed substitution of gfx1201 code for Apple's GFX10 shaders at the residency
// copy, generalising kernsub's exact-byte guard to every shader the embedded cache
// blob holds; 2 disarms; no argument reads the counters. Returns the state bits
// (1 armed, 2 the blob opened). Defined in Navi48AccelPeer.cpp.
uint32_t navi48_shadercache_control(uint32_t mode, uint64_t *out, unsigned count);
//: the count of resources the shader cache has scanned at a residency copy. The source hook's
// program memo (gfx_src_decide.h) uses it as an EPOCH - a residency copy is the only thing that
// rewrites the bytes at a shader's GPU address - and drops everything it learned when it moves.
// Read-only. Defined in Navi48AccelPeer.cpp.
uint64_t navi48_shadercache_epoch(void);
//: how often the residency copy found res+0x1dc disagreeing with the record the hardware
// actually reads (*(res+0x180)+0x40). A WARNING, never a refusal: setupHwCBRegs' `cmoveq %rsi,%rcx`
// (0xbdf9141) keeps the record while res+0x180 is set, so the hardware never reads res+0x1dc for
// such a resource. Read-only. Defined in Navi48AccelPeer.cpp.
uint64_t navi48_resprov_warn_swz(void);
uint64_t navi48_resprov_warn_type(void);
// 0.0.267, action 57 `pairing [1|2]`: the display-pairing stamp is OPT-IN.
// 0 reads; 1 enables it for this boot and must be sent BEFORE `fire` (the decision is taken
// once, at the accelerator-started callback, and a later enable is REFUSED); 2 disables it,
// or, if our keys are already stamped, withdraws them (registry only). Returns the verdict
// (pairing_policy.h) and fills out[0..9]: verdict, request, boot-arg present, boot-arg value,
// decided, source, stamped, withdrawn, withdraw reason, peer present. Defined in
// Navi48AccelPeer.cpp.
uint32_t navi48_pairing_control(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.272, milestone 4 route c' wall 3. action 60 `scanout [0|1|2]`: 0 reads RDNA4FB's scanout
// geometry and checks it (scanout_copy.h); 1 runs the POSITIVE CONTROL (kext bars -> pre-flight copy between two
// scratch VRAM buffers -> SDMA0 QUEUE0 copy into a 256x64 scanout rectangle -> every pixel read back through BAR0);
// 2 restores the rectangle the positive control overwrote. out[0..12] as the definition documents. Defined in
// Navi48Bringup.cpp.
// 0.0.416 (notes/design/SDMA-GCR.md): the argument is the ONE ABI scalar, not just the mode. Modes 0..6 pass
// scalar == mode (unchanged); mode 7 packs the mode, the GCR flag and the source VRAM offset into it
// (sdma_gcr.h's n48_scanout7_scalar). Any other scalar with high bits set is refused as before.
uint32_t navi48_scanout_control(uint64_t arg, uint64_t *out, unsigned count);
// action 82 `sdmadcc [0|1|2]` (0.0.417, notes/design/SDMA-DCC-NOPTE.md, D1): SDMA0_DCC_CNTL's no-PTE read
// decompression / write compression. 0 reads SDMA0 (offset 0x0034) and SDMA1 (0x0634) raw and decoded per set;
// 1 CAPTURES SDMA0's value on first use, writes `captured & ~0x00015554`, reads back and reports (idempotent);
// 2 writes the captured value back. Any other argument, and 2 before a capture, is REFUSED. SDMA1 is never
// written and SDMA0_DCC_CNTL is the only register this verb writes. out[0..8]; the argument rules and the mask
// are src/apple/sdma_dcc.h, host-tested by tests/sdma_dcc_test.cpp. Defined in Navi48Bringup.cpp.
uint32_t navi48_sdmadcc_control(uint64_t arg, uint64_t *out, unsigned count);
// build 0.0.496 (notes/design/FAST-PAGEIN.md,), switch 63 (DEFAULT OFF): the residency copy through SDMA. The pure
// half is src/apple/fastcopy.h. navi48_fc_set: `gfxneuter 63 | M << 8` for M 1 (sampled verify), 2 (OFF), 3 (full verify); the
// first ON binds the staging buffer and runs the positive control (0 done, 9 no lock, 12 set but the path cannot copy).
// navi48_fc_chunk: one chunk of residency_copy_to_vram, AFTER that loop's own VRAM-guard check of [dAt, dAt + n); `fill`
// produces the chunk's bytes [off, off + take) exactly as the MM loop's batch does (rp_retile_bytes, or ic_read = readBytes +
// 0.0.495's overlay). Returns 0 (the MM loop runs the chunk) or N48_FC_R_TAKEN | result; a taken chunk starting below resource
// offset 128 leaves its bytes below 128 at `lead` (the copy loop's own 256-byte batch buffer) for the log's head. navi48_fc_copy_report: the copy's
// `via SDMA` line after its COPIED line. navi48_fc_report: the bare `gfxneuter 63` lines. Defined in Navi48Bringup.cpp.
typedef int (*N48FcFillFn)(void *ctx, uint8_t *dst, uint64_t off, uint32_t take);
uint32_t navi48_fc_set(uint32_t m);
// build 0.0.511 (LOW-3 of the 0.0.510 review): `cgLo`/`cgHi` = the copy's whole VRAM range (the pre-flight's
// bounding [cgLo, cgHi), the copy guard's scope), asked at the first chunk for live copy-guard poison.
// build 0.0.521 Part D: `rpCand` 1 = the copy is a resprov candidate (the copier's `retile`): latched FULL.
uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,
                         uint8_t *lead, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand);
void navi48_fc_copy_report(uint64_t copyNo);
void navi48_fc_report(const char *why);
// build 0.0.534, switch 89 (DEFAULT OFF): a fast-copy chunk latched FULL is verified by an SDMA read-back
// of its VRAM range into a second GART-bound buffer plus a sampled MM cross-check (the pure half is src/apple/fastcopy89.h).
// navi48_fc89_set: `gfxneuter 89 | M << 8` for M 1 (ON), 2 (OFF) and 3 (SHADOW: read-back compared with the MM verify, which
// decides everything); the first ON/SHADOW binds the read-back buffer and runs its control
// (0 done, 9 no lock, 11 unknown M, 12 set but the read-back cannot run). navi48_fc89_report: the bare `gfxneuter 89` line.
// Defined in Navi48Bringup.cpp.
uint32_t navi48_fc89_set(uint32_t m);
void navi48_fc89_report(const char *why);
// build 0.0.531 item 4 (log-only): the three mmhold lines, once, at a continuous arm's START and STOP. Defined in Navi48Bringup.cpp.
void navi48_mmhold_snapshot(const char *where);
// build 0.0.509: navi48_fc_scope_closed - the copy scope's close (navi48_cg_close_scope) asks whether THIS thread's copy
// ran any chunk through SDMA under sampled verify (1; F-3: it then neither clears poison nor completes clean) and releases the
// copy's slot (0.0.496 released it at navi48_fc_copy_report). Defined in Navi48Bringup.cpp. navi48_ic_bumped_mine - this
// thread's copy bumped switch 62's heap generation (it overlaps a range 62 registered): its sampled verify is upgraded to full.
// navi48_ic_chunk_dead - a chunk of this thread's copy that did not see its fence land: 62's sticky poison for its range (F-2).
// Both defined in Navi48AccelPeer.cpp.
uint32_t navi48_fc_scope_closed(uint64_t lo, uint64_t hi);
uint32_t navi48_ic_bumped_mine(void);
void navi48_ic_chunk_dead(uint64_t pos, uint64_t dAt, uint64_t n);
// build 0.0.527 (notes/design/SKIP82.md; gfx_sk82.h) — SWITCH 82. gN48Sk82Live: 1 once its memory exists (sticky; the event
// sites' one load). navi48_sk82_ev: one invalidation event (AppleHardwareHook.cpp; any thread, no lock). navi48_sk82_try: the
// copier's decision before rp_lin_prepare and the scope (1 = skip). navi48_sk82_result: the copy's result before its scope closes;
// navi48_sk82_taint_mine: a write inside the copy's scope that is not the source (shadercache, kernsub); navi48_sk82_cand_mine:
// the per-thread FULL-verify flag fc_copy_chunk ORs into rpCand; navi48_sk82_closed: the establishment, from navi48_cg_close_scope
// after the scope closed; navi48_sk82_switch: `gfxneuter 82 | M << 8`. navi48_fc_mode_now: switch 63's latched-mode setting (read).
// All defined in Navi48Bringup.cpp.
extern volatile uint32_t gN48Sk82Live;
extern volatile uint32_t gN48Sk82On;     // 1 while MEASURE or SKIP: the copier's gate (one load while OFF)
void navi48_sk82_ev(uint32_t kind, uint64_t va, uint64_t size);
uint32_t navi48_sk82_try(const n48_sk82_key *k, const n48_sk82_snap *sn, void *md, uint64_t copyNo, uint32_t eligible,
                         n48_sk82_out *o);
void navi48_sk82_result(uint32_t ok, uint64_t wBytes);
void navi48_sk82_taint_mine(void);
uint32_t navi48_sk82_cand_mine(void);
void navi48_sk82_closed(uint64_t lo, uint64_t hi, const n48_sk82_snap *sn);
uint32_t navi48_sk82_switch(uint32_t m, uint32_t contRefused, uint32_t *st);
// build 0.0.528 (; gfx_tlb83.h) — SWITCH 83: `gfxneuter 83 | M << 8` (AppleHardwareHook.cpp's selector computes the
// continuous-arm guard). Writes amdgpu::gTlb83On (the ONLY writer) and prints the two bare-83 lines. Defined in Navi48Bringup.cpp.
uint32_t navi48_tlb83_switch(uint32_t m, uint32_t contRefused, uint32_t *st);
uint32_t navi48_fc_mode_now(void);
// build 0.0.529 (notes/design/CG84.md; gfx_cg84.h) — SWITCH 84. gN48D84Live: 1 once the keys' memory exists (sticky; the
// copier's and the event sites' one load). gN48D84SrcThr: the thread whose ON copy writes from the scratch (0 = none; ic_read's and
// fc_copy_chunk's one load). navi48_d84_plan: G0, before the copy's scope (returns the packed ON delta, 0 = the copy is 0.0.528's);
// navi48_d84_result: the copy's result before its scope closes; navi48_d84_census: shadercache's programs for the copy;
// navi48_d84_closed: the update, from navi48_cg_close_scope after navi48_cg_close; navi48_d84_scratch/src: the plan-time image;
// navi48_d84_pageout / unmap / ctx / ws / commit / db_note: the invalidations (AppleHardwareHook.cpp, Navi48AccelPeer.cpp);
// navi48_cg84_switch: `gfxneuter 84 | M << 8`; navi48_cg84_mode: the live mode (read). All defined in Navi48Bringup.cpp.
extern volatile uint32_t gN48D84Live;
extern volatile uintptr_t gN48D84SrcThr;
uint64_t navi48_d84_plan(const n48_d84_elig *e, const n48_d84_id *id, const n48_sk82_snap *sn, void *md, uint64_t cgLo, uint64_t cgHi,
                         uint64_t dirtyLo, uint64_t dirtyHi);
void navi48_d84_result(uint32_t ok, uint64_t compared, uint64_t mismatched, uint64_t wBytes);
void navi48_d84_census(uint32_t programs);
void navi48_d84_closed(uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch);
const uint8_t *navi48_d84_scratch(void);
uint64_t navi48_d84_src(uint64_t resOff, uint8_t *dst, uint64_t take);
void navi48_d84_pageout(const void *res);
void navi48_d84_unmap(uint64_t va, uint64_t size);
void navi48_d84_inval_all(void);
void navi48_d84_commit(void);    // 0.0.529 fix pass (S2/MF-2): lock-free, every key moves; the 100 ms window starts
void navi48_d84_unknown(void);   // MF-1 / S3: Apple SDMA or an un-NOPed client IB (ON: every key moves; SHADOW: counted)
// MF-3 (Navi48AccelPeer.cpp): shadercache's own candidate predicate over img[ds, de) - 1 when any grid start there is a program
// candidate (sc_lookup answers anything but an empty slot or no terminator), or on any doubt.
uint32_t navi48_d84_programs_in(void *ctx, const uint8_t *img, uint64_t bytes, uint64_t ds, uint64_t de);
uint32_t navi48_cg84_switch(uint32_t m, uint32_t contRefused, uint32_t *st);
uint32_t navi48_cg84_mode(void);
// 0.0.272: copy the rectangle (0,0,w,h) of a VRAM buffer into the scanout at (dstX,dstY) on SDMA0 QUEUE0 and read
// it back. REFUSES (status 13) until the positive control passed this boot, and on any plan refusal (overlap with
// the scanout, stride, range). Called by the flush hook. out[0..10]. Defined in Navi48Bringup.cpp.
// 0.0.288: `verify` false skips the BAR0 readback ( measured a BAR0 read at ~0.57 MiB/s, so verifying two 1920-pixel
// rows costs tens of milliseconds - affordable for a verb, not for a per-frame present). The copy, the plan, the per-row
// destination check and the fence are UNCHANGED; only the comparison after them is skipped. Everything but the flush hook
// and the pipe shim's present passes true.
uint32_t navi48_scanout_copy_vram(uint64_t srcOff, uint64_t srcLen, uint32_t srcW, uint32_t srcH, uint32_t srcStride,
                                  uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                  bool verify = true);
// 0.0.345: the TILED source. Same interlock, lock, geometry, plan-then-check and fence as the row copy
// above, but ONE COPY_TILED_SUB_WINDOW packet for the whole frame - CONFIRMED the composited surface is tiled, so
// srcStride is not a row pitch into it and no row address can be right. `swizzle` is the GFX12 ADDR3 enum (only 3,
// ADDR3_64KB_2D, is accepted) and surfW/surfH are Apple's CB_COLOR0_ATTRIB2 MIP0_WIDTH/HEIGHT + 1, both read from the
// resource by the caller. Defined in Navi48Bringup.cpp. out[0..10].
// 0.0.416 (notes/design/SDMA-GCR.md G3): `gcr` prepends the SDMA GCR_REQ (GL2 write-back + invalidate) to the
// same submission as the tiled copy. false is the 0.0.415 behaviour, byte for byte; pipeshim ARG 5 passes true.
uint32_t navi48_scanout_copy_tiled(uint64_t srcOff, uint64_t srcLen, uint32_t surfW, uint32_t surfH, uint32_t swizzle,
                                   uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count,
                                   bool verify = true, bool gcr = false);
bool navi48_scanout_pc_passed(void);
// build 0.0.518: FLIP MODE, switch 74 (apple/gfx_flipmode.h; defined in Navi48Bringup.cpp).
// navi48_fm_on: the switch (lock-free). navi48_fm_present: dpg_perform's per-present question, asked only while ON and only after
// switch 73's hold check - 0 = a copy was made into the back buffer (*cstOut its scanout status) and flipped (or the copy failed
// and the flip was held), 1 = held (no copy). navi48_fm_note_withdrawal: a withdrawal asks for a restore (a flag only).
// navi48_fm_disarm: the commit arm's disarm verb restores to A. navi48_fm_control: `gfxneuter 74 | M << 8`. navi48_fm_test: the
// unarmed A/B test (`dcnflip 1000 + N`).
uint32_t navi48_fm_on(void);
uint32_t navi48_fm_present(uint64_t phys, uint64_t len, uint32_t surfW, uint32_t surfH, uint32_t swz, uint32_t pw, uint32_t ph,
                           bool linear, bool verify, bool gcr, uint64_t presentNo, uint64_t *cv, unsigned cvCount,
                           uint32_t *cstOut);
void navi48_fm_note_withdrawal(void);
// build 0.0.525 (switch 80): which buffer the last present's copy went into, for the cyc80 copy ring (read-only, no lock):
// 0 = flip mode OFF (the console copy), 1 = A, 2 = B (the buffer the display was NOT scanning at the last latch), 3 = unknown.
uint32_t navi48_fm_copy_buf(void);
// build 0.0.538 (switch 95, gfx_p95.h): 1 while flip mode must not take a replayed present - a restore is requested, a failed
// restore's A copy is pending, or A/B are engaged with flip mode OFF (a failed restore awaiting 842). Read-only and lock-free.
uint32_t navi48_fm_replay_blocked(void);
void navi48_fm_disarm(void);
uint32_t navi48_fm_control(uint32_t m, uint32_t contRefused);
uint32_t navi48_fm_test(uint32_t n, uint64_t *out, unsigned count);
// 0.0.276: the AMDGraphicsAccelerator whose vtable the stop() patch copied, or null. Defined in
// Navi48AccelPeer.cpp; lets read-only verbs avoid a registry walk on a boot where WindowServer may hold registry work.
void *navi48_accel_object(void);
// 0.0.286, DisplayPipeGuard.cpp: the display-pipe safety core (action 66), the AGDC nub (67), the in-kernel
// framebuffer write benchmark (68) and the write-combining framebuffer mapping (69, boot-arg navi48-fbwc=1 at start).
uint32_t navi48_pipeguard_control(uint64_t arg, uint64_t *out, unsigned count);
bool navi48_pipeguard_armed_all(void);
uint32_t navi48_agdc_control(uint64_t arg, uint64_t *out, unsigned count);
uint32_t navi48_agdc_hold_control(uint64_t arg, uint64_t *out, unsigned count);   // action 79 (0.0.322, )
uint32_t navi48_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count);   // action 88 `pipeagdc` (0.0.614, amd/native_agdc_pure.h): the native AGDC service
uint32_t navi48_cqprobe_control(uint64_t arg, uint64_t *out, unsigned count);     // action 80 (0.0.325, )
uint32_t navi48_fbbench(uint64_t arg, uint64_t *out, unsigned count);
uint32_t navi48_fbwc_control(uint64_t arg, uint64_t *out, unsigned count);
void navi48_fbwc_boot(void);
// 0.0.288 (an internal review note), DisplayPipeGuard.cpp:
// action 70 `pipeshim [0|1|2]` - the vendor-slot shim that PARTICIPATES in the transaction lifecycle instead of refusing it;
// action 71 `pipemode [0|1]` - route B readiness (write the fields init_framebuffer_resource would and set pipe+0x298).
uint32_t navi48_pipeshim_control(uint64_t arg, uint64_t *out, unsigned count);
uint32_t navi48_pipemode_control(uint64_t arg, uint64_t *out, unsigned count);
// action 72 `emcensus [0|1|2]` (0.0.291): the event-machine census policy toggle (1 arm, 2 disarm, 0/none
// read) - disarms only the accel+0x380 pass-through counters, leaving every safety-core refusal and the WRITE_DATA guard
// armed, to isolate cause 2 of. Must precede pipeguard arming. Reads also report the per-slot census hit breakdown.
uint32_t navi48_emcensus_control(uint64_t arg, uint64_t *out, unsigned count);
// action 73 `routea [0|1]` (0.0.295, an internal review note): corrected route A, DEFAULT OFF. 1 arms
// on the safety core's guarded pipes (slot 267 returns a kext-owned VidMemory-shaped object and sets pipe+0x298; slot 46
// AMDAccelResource::prepare is neutralised on the framebuffer resource only, via a per-instance vptr swap of pipe+0xe0);
// 0 sets the runtime mode off (inert) and reads. Never calls reserveFrameBuffer. Rides on pipeguard being armed.
uint32_t navi48_routea_control(uint64_t arg, uint64_t *out, unsigned count);
// Accessors those need: Apple's accelerator vtable copy (header words included) and the X6000 slide by the two accelerator anchors
// (slot 184 start, slot 326 newSurface), 0 with a reason when unavailable; our PCI device and our Navi48Bringup service.
void **navi48_accel_vtable_copy(void);
uintptr_t navi48_x6000_slide(uint32_t *reason);
class IOService;
IOService *navi48_bringup_pci(void);
IOService *navi48_bringup_service(void);
// 0.0.276: the read-only 2D-context observe hook (new2DContext slot 327, blitCopy/blitFill 360/361). Folded into
// action 61 with the GFX census. Defined in Navi48AccelPeer.cpp.
uint32_t navi48_ctx2d_control(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.279, action 63 `finishread`: the 2D context's user-client method counters (set_surface, finish, blit per pid).
uint32_t navi48_ctx2d_calls(uint64_t *out, unsigned count);
// 0.0.273: the same copy when the surface's pixels are in its system-memory BACKING (an IOMemoryDescriptor,
// prepared): rows staged by the CPU into a BAR0-pool VRAM buffer, then SDMA0 QUEUE0 into the scanout, read back against the
// backing. Status 17 = backing unprepared or too short. Defined in Navi48Bringup.cpp.
class IOMemoryDescriptor;
uint32_t navi48_scanout_copy_staged(IOMemoryDescriptor *md, uint32_t srcStride, uint32_t srcW, uint32_t srcH,
                                    uint32_t dstX, uint32_t dstY, uint32_t w, uint32_t h, uint64_t *out, unsigned count);
// 0.0.272, action 59 `flushhook [1|2|3]`: 0 reads; 1 arms the LOG-ONLY per-surface externalMethod hook (installed on
// every AMDAccelSurface the accelerator mints from then on); 3 additionally arms the copy of a flushed surface into
// the scanout (refused unless `scanout 1` passed); 2 returns to pass-through. out[0..12]. Defined in
// Navi48AccelPeer.cpp.
uint32_t navi48_flushhook_control(uint64_t arg, uint64_t *out, unsigned count);
// 0.0.333, action 81 `ucprobe [0|1|2]`: LOG ONLY. Hooks the accelerator's newUserClient (slot 239)
// on our per-instance vtable copy, names every user client's class IN-KERNEL from its own metaclass, and for an
// IOAccelDisplayPipeUserClient2 swaps that instance's vptr for a copy whose slot 266 (externalMethod) is ours - so
// selector, argument counts, the five transactionEnd gate bytes, the 4-slot ring indices, the transaction list and
// pipe+0x2a4 before/after are recorded per call, with the 0x118-byte transaction args dumped for selector 8.
// Chains to Apple unconditionally; writes no register. 1 install+log, 2 stop logging, 0 read. Defined in
// Navi48AccelPeer.cpp.
uint32_t navi48_ucprobe_control(uint64_t arg, uint64_t *out, unsigned count);
// build 0.0.503 (notes/design/HYBRID.md H1): switch 68, the hybrid refusal inside the slot-239 hook (installed at the
// accelerator's start). get/set the switch (hw_hook_gfx_neuter's `68 | M << 8` owns the verb and its mid-arm guard) and print
// the `hybrid68:` report line with `how` appended. Defined in Navi48AccelPeer.cpp.
uint32_t navi48_hybrid_get(void);
void navi48_hybrid_set(uint32_t on);
void navi48_hybrid_report(const char *how);
// 0.0.204, action 46 `kernsub [mode]`: arm the substitution of Apple's Navi21 blit
// kernel with a gfx1201 kernel at the residency copy's shader region (+0xfb00),
// guarded by an exact match on Apple's original bytes, and substitute immediately
// if a copy has already run this boot. 0.0.206: mode 1 = blit_copy_gfx1201 (the
// copy), 2 = blit_diag_gfx1201 (INSTRUMENT, section 328), 3 = blit_diagmin_gfx1201
// (INSTRUMENT), 4 (0.0.207) = blit_copy_offen_gfx1201 (the copy by byte offset,
// section 329); any other mode only reads the counters; one mode per boot.
// out[0]=status (0 refused, 1 substituted, 2 already ours, 3 read-back mismatch),
// out[1]=VRAM address, out[2]=kernel bytes, out[3]=read-back mismatches,
// out[4]=substitutions, out[5]=last reason, out[6]=armed, out[7]=copy lastDst,
// out[8]=armed mode, out[9]=kernel dwords. Defined in Navi48AccelPeer.cpp.
uint32_t navi48_kernelsub_control(uint32_t mode, uint64_t *out, unsigned count);
// 0.0.193: pulse GCVM/MMVM_L2_PROTECTION_FAULT_CNTL bit 0 on both hubs so the
// first-fault-latched status registers start fresh (gmc_clear_vm_faults).
// False if the bring-up context is not ready. Defined in Navi48Bringup.cpp.
bool navi48_vm_fault_clear(void);
//: does SDMA's SRBM_WRITE actually write a register on this part?
// Writes sentinelA by MMIO (proving the target is writable), then sentinelB
// via an SRBM_WRITE packet on OUR OWN SDMA0 QUEUE0 ring, fenced and kicked.
bool navi48_sdma_srbm_probe(uint32_t reg, uint32_t sentinelA, uint32_t sentinelB,
                            uint32_t sentinelC);
// The VRAM aperture the bring-up driver measured (fb_start, size). Defined in
// Navi48Bringup.cpp.
bool navi48_vram_aperture(uint64_t &start, uint64_t &size);
// build 0.0.544 item 4b: the console size getConsoleInfo reported at start (false before it ran). Navi48Bringup.cpp.
bool navi48_console_size(uint32_t &w, uint32_t &h);
// 0.0.242, for `vmroots` (52): our device-only hi pool as the GMC built it - its
// 0-based VRAM offset, its size, and the bytes it has handed out. `used` is the
// same bytes_used() navi48_vram_apple_dest_check refuses on, so a caller can show
// that a tail reservation outside this pool leaves the residency copy untouched.
// False when the GMC is not initialised or the pool was never created. Defined in
// Navi48Bringup.cpp.
bool navi48_vram_hi_pool(uint64_t &base, uint64_t &size, uint64_t &used);
// 0.0.244: invalidate the GFXHUB TLB and walker cache for ONE VMID (engine 17,
// L2_PTES + L2_PDE0/1/2 + L1_PTES, bounded 100 ms ACK poll). gmc_flush_gpu_tlb is
// declared in amd/amdgpu_gmc.h, which this layer does not include, so it is the one
// accessor the mapping work was actually missing - the HDP flush it also needs
// already exists above as navi48_hdp_flush_now(), contrary to  and to
// M3-ROOT-WRITE-REVIEW.md.3. NOT Apple's invalidateVM, which our own vtable patch
// has already made inert. False when the GMC or GFXHUB is not ready, or on ACK
// timeout. Nothing calls it yet. Defined in Navi48Bringup.cpp.
bool navi48_gmc_flush_tlb_vmid(uint32_t vmid, uint32_t flush_type);
//: the BAR0 host-physical aperture (phys, size). NOT the same as above:
// that one is the GPU/MC base. Defined in Navi48Bringup.cpp.
bool navi48_bar0_aperture(uint64_t &phys, uint64_t &size);
// Point SDMA1 QUEUE1 at a ring Apple owns. enableRing MUST stay false until
// every address in that ring is proven resident. doorbellIndex is in
// QWORD units, already shifted (sdma_engine[] value << 1).
// Kick the QUEUE1 doorbell with a wptr Apple owns. Programming resets WPTR to 0,
// so this is what actually makes the engine fetch. Defined in Navi48Bringup.cpp.
bool navi48_sdma_kick_external_doorbell(uint32_t wptrDwords, uint32_t doorbellIndex);
// Read-only QUEUE1 register dump. Does NOT reset the pointers, unlike programming.
bool navi48_sdma_log_external_queue(const char *tag);
// QUEUE1's live RB_RPTR (byte domain) and its writeback dword. Reads only.:
// the value Apple's getHead is SHADOWED away from. Defined in Navi48Bringup.cpp.
bool navi48_sdma_read_external_rptr(uint32_t *rptrRegOut, uint32_t *wbRptrOut);
bool navi48_sdma_program_external_queue(uint64_t ringGpuVa, uint32_t ringSizeDwords,
                                       uint32_t doorbellIndex, bool enableRing);

// ---- 0.0.185 "sdmamap": Apple's SDMA ring onto SDMA0 QUEUE1 ---------------
//
// Everything below works on SDMA **instance 0** QUEUE1, not instance 1 like the
// bridge above. The decision is's: our own SDMA0 QUEUE0 stays
// programmed and running as the liveness control, and QUEUE1 on the same engine
// is an independent register block with no MQD and no MES map, so taking it
// costs nothing that is currently working.

// The doorbell DWORD index the takeover uses (0x202) — one constant, read by
// the hook, the TTL and the self-test so they cannot disagree.
uint32_t navi48_sdma_q1_doorbell_index(void);

// The REAL BAR2 doorbell dword for the SDMA0 QUEUE1 index.
//
// A separate entry point from navi48_doorbell_dword_ptr on purpose: that one
// refuses every index >= sdma_engine[0]<<1 (= 0x200) precisely so a mistake
// cannot hand Apple a doorbell one of OUR engines owns, and 0x202 is inside that
// blanket refusal. This narrows the exception to the one index the takeover
// programs, and still refuses anything our map actually assigns.
bool navi48_sdma_q1_doorbell_ptr(uint32_t index, void **ptrOut, uint64_t *byteOffOut);

// The QUEUE1 register block, read back. Plain scalars so the Apple-side code
// never has to include the amdgpu headers.
struct Navi48SdmaQ1Regs {
    uint32_t rb_cntl, rb_base, rb_base_hi, rb_rptr, rb_rptr_hi;
    uint32_t rb_wptr, rb_wptr_hi, rb_rptr_addr_lo, rb_rptr_addr_hi;
    uint32_t ib_cntl, doorbell, doorbell_offset;
    uint32_t wptr_poll_lo, wptr_poll_hi, minor_ptr_update;
};
bool navi48_sdma_q1_read_regs(Navi48SdmaQ1Regs *out);

// Program SDMA0 QUEUE1 for a ring somebody else owns, with THEIR write-back
// addresses, then verify the read-back and only then set RB_ENABLE/IB_ENABLE.
//
// failStepOut, when non-null, receives 0 on success or the step that refused:
//   1 SDMA0 not ready, 2 arguments rejected by the programmer,
//   3 the read-back did not match what we wrote (nothing is enabled),
//   4 enabling failed.
bool navi48_sdma_map_external_ring(uint64_t ringGpuVa, uint32_t ringSizeDwords,
                                   uint64_t rptrAddr, uint64_t wptrPollAddr,
                                   uint32_t doorbellIndex, uint32_t *failStepOut);

// The escape hatch, callable from the Apple side: RB_ENABLE=0 + IB_ENABLE=0 on
// SDMA0 QUEUE1 and nothing else.
bool navi48_sdma_q1_disable(void);

// Our own SDMA0 QUEUE1 external rptr-report address —'s known-safe value in
// our already-GART-bound write-back page. The fallback for RB_RPTR_ADDR when
// Apple's ring carries no rptr report of its own.
bool navi48_sdma_q1_fallback_rptr_addr(uint64_t *addrOut);

// The boot-time QUEUE1 routing self-test's verdict, for `sdmastate` and for the
// refusal `sdmamap` applies when the test ran and FAILED.
//   0 = never ran, 1 = the doorbell routed, 2 = MMIO only (the doorbell does
//   not route), 3 = neither leg fenced.
uint32_t navi48_sdma_q1_test_result(void);

// ---- 0.0.196: the SAME takeover, generalised to N Apple rings --------------
//
// kSDMAExtSlots (amdgpu_sdma.h) is the table of hardware queues the takeover may
// spend: SDMA0/SDMA1 QUEUE1..QUEUE7 (QUEUE0 on both instances stays ours), each
// with a distinct doorbell dword inside the ONE routed S2A window. These four
// accessors are the Apple side's whole view of it, so AppleHardwareHook.cpp
// still needs none of the amdgpu headers.

// How many hardware queues the takeover may hand out (kSDMAExtSlotCount).
uint32_t navi48_sdma_slot_count(void);

// Slot -> (instance, queue, doorbell dword). False if the slot index is out of
// range or its SDMA instance is not present/inited on this boot.
bool navi48_sdma_slot_info(uint32_t slot, uint32_t *instOut, uint32_t *queueOut,
                           uint32_t *doorbellOut, bool *usableOut);

// The boot self-test verdict for ONE slot, same encoding as
// navi48_sdma_q1_test_result: 0 never ran, 1 the doorbell routed, 2 MMIO only,
// 3 neither leg fenced.
uint32_t navi48_sdma_slot_test_result(uint32_t slot);

// Program slot `slot` for a ring somebody else owns, verify the read-back and
// only then enable. failStepOut as navi48_sdma_map_external_ring.
bool navi48_sdma_map_external_ring_slot(uint32_t slot, uint64_t ringGpuVa,
                                        uint32_t ringSizeDwords, uint64_t rptrAddr,
                                        uint64_t wptrPollAddr, uint32_t *failStepOut);

// Read one slot's register block; disable one slot (the per-slot escape hatch);
// that slot's fallback rptr-report address in OUR write-back page.
bool navi48_sdma_slot_read_regs(uint32_t slot, Navi48SdmaQ1Regs *out);
bool navi48_sdma_slot_disable(uint32_t slot);
bool navi48_sdma_slot_fallback_rptr_addr(uint32_t slot, uint64_t *addrOut);

class Navi48Ttl final : public AmdTtlServicesABI {
public:
    // Bind to the running bring-up instance. Must be called before the
    // accelerator matches, i.e. before LoadAccelerator is published.
    void bind(Navi48Bringup *owner);
    bool bound() const { return mOwner != nullptr; }

    // Per-slot call counters, published to the IORegistry and readable through
    // the user client. The whole value of the first boot is knowing which of
    // the 43 slots Apple actually reaches and in what order, so count them all.
    uint32_t callCount(int slot) const;
    uint32_t totalCalls() const { return mTotalCalls; }
    int      firstUnsupported() const { return mFirstUnsupported; }

    // 0.0.367 — X9's C13 OBSERVER. Slot 36 is the only route by which Apple can put a ring on a hardware
    // queue, so it is the one place that can say "a queue nobody of ours watches was started". Classified HERE, at the
    // call, into exactly two buckets: a type one of our observers covers (7/10 the SDMA drain, 8 the KIQ emulator,
    // 9 the source hook and the ring walk) and anything else. `starts == known + unknown` is X9's N48_DEP_ID_QUEUE
    // identity, and `tallyOk` goes false if a start could not be classified at all, which refuses.
    struct QueueCensus { uint64_t starts, known, unknown; uint32_t tallyOk; };
    QueueCensus queueCensus() const { return { mQStarts, mQKnown, mQUnknown, mQTallyOk }; }

    ~Navi48Ttl() override;

    // ---- implemented for real -------------------------------------------
    uint32_t     initialize(TtlLibraryInitializationInput *) override;
    uint32_t     uninitialize() override;
    uint32_t     powerUp() override;
    uint32_t     powerDown() override;
    uint32_t     notifyHardwareState(bool) override;
    uint32_t     getSwipErrorString(char *, unsigned long) override;
    uint32_t     queryHwBlockRegisterBase(hwblock_type, unsigned char,
                                          unsigned int, unsigned int *) override;
    unsigned int getHwipSegmentCount() const override;
    uint32_t     queryCSBInfo(unsigned long long *, unsigned int *) override;
    uint32_t     queryGcHardwareInfo(GcHardwareInfo *) override;
    uint32_t     queryGpuMemType(GpuMemType *) override;
    uint32_t     queryGpuClkInfo(GpuClkInfo *) override;
    uint32_t     xgmi_services(uint32_t, BgdSecurityXgmiInput *,
                               BgdSecurityXgmiOutput *) override;

    // ---- logged, not yet implemented ------------------------------------
    uint32_t getTtlRtsInfo(TtlRtsInfo *) override;
    uint32_t getLocalMemoryInfo(GmmLocalMemoryInfo *) override;
    uint32_t getNonLocalMemoryInfo(GmmNonLocalMemoryInfo *) override;
    uint32_t setFirmwareDirectory(AMDFirmwareDirectory *) override;
    uint32_t getGartTableAddress(void **, unsigned long long *) override;
    uint32_t queryCommandInfo(AmdSwipQueueType, unsigned int,
                              AmdTtlCommandType, TtlCommandInfoOutput *) override;
    uint32_t buildCommand(AmdSwipQueueType, unsigned int,
                          TtlBuildCommandInfo *, void *) override;
    uint32_t submitFrame(AmdSwipQueueType, unsigned int, void *) override;
    uint32_t queryFwLoadingStatus(CrossArchIriIsFwLoadedInput *,
                                  CrossArchIriIsFwLoadedOutput *) override;
    uint32_t addGartSaveRestoreRange(unsigned long long, unsigned long long) override;
    uint32_t removeGartSaveRestoreRange(unsigned long long) override;
    uint32_t mapToGart(unsigned long long, IOMemoryDescriptor *) override;
    uint32_t unmapFromGart(unsigned long long, IOMemoryDescriptor *) override;
    uint32_t hdcp_services(uint32_t, BgdSecurityHdcpInput *, BgdSecurityHdcpOutput *) override;
    uint32_t display_topology_services(uint32_t, BgdSecurityTopoInput *,
                                       BgdSecurityTopoOutput *) override;
    uint32_t auc_services(uint32_t, BgdSecurityAucInput *, BgdSecurityAucOutput *) override;
    uint32_t fp_services(uint32_t, BgdSecurityFpInput *, BgdSecurityFpOutput *) override;
    uint32_t rap_services(uint32_t, BgdSecurityRapOutput *) override;
    uint32_t collectEngineDiagInfo(AmdTtlCollectDiagInfoInput *,
                                   AmdTtlCollectDiagInfoOutput *) override;
    uint32_t queryFwAttestationInfo(AmdFwAttestationInput *, AmdFwAttestationOutput *) override;
    uint32_t queryEngineQueueCount(AmdSwipQueueType, unsigned int *) override;
    uint32_t notifyEngine(AmdSwipQueueType, unsigned int, AmdTtlEngineEvent) override;
    uint32_t startEngineQueue(AmdSwipQueueType, unsigned int,
                              SwipQueueInParams *, SwipQueueOutParams *) override;
    uint32_t stopEngineQueue(AmdSwipQueueType, unsigned int) override;
    uint32_t queryEngineQueueState(AmdSwipQueueType, unsigned int, SwipEngineState *) override;
    uint32_t resetEngineQueue(AmdSwipQueueType, unsigned int) override;
    uint32_t sendRequestToMES(AmdMesRequestType, void *) override;
    uint32_t queryDpmTableInfo(TtlDpmTableInfo *, AmdDpmClockType) override;

private:
    void     dumpIpTable() const;
    uint32_t note(int slot, const char *name, void *ret) const;   // count + log; returns kTtlOk
    uint32_t unsupported(int slot, const char *name, void *ret) const;
    // Apple's X6000 load address, derived in initialize() from a callback whose
    // static address we know. Lets every log line name the Apple function that
    // called us, in terms of the binary we disassembled.
    uint64_t mX6000Slide { 0 };
public:
    // Load slide of AMDRadeonX6000, recovered from a callback pointer Apple
    // hands us in initialize(). Lets us reach its NON-virtual functions.
    uint64_t x6000Slide() const { return mX6000Slide; }
private:

    Navi48Bringup            *mOwner   { nullptr };
    amdgpu::DeviceContext    *mDev     { nullptr };
    amdgpu::BringupContext   *mCtx     { nullptr };

    // Saved from initialize(); the accelerator hands us its VRAM allocator and
    // its doorbell aperture here, so TTL never needs its own.
    AccelGmmCallbacks        *mGmmCb   { nullptr };
    AccelEventLogCallbacks   *mLogCb   { nullptr };
    void                     *mDoorbellBase { nullptr };
    uint32_t                  mDoorbellBytes { 0 };

    // Doorbell slots handed out by startEngineQueue (slot 36). Apple gives us its
    // whole doorbell aperture in initialize(); each engine queue needs one 64-bit
    // word of it, because AMDRTRing::writeTail does `movq %rbx,(%rax)` on the
    // pointer we return. Keyed by (queue,inst) so a repeated start() returns the
    // SAME slot -- 8 rings aliasing one doorbell word would be a real bug.
    struct DoorbellSlot { uint32_t queue; uint32_t inst; uint32_t off; bool used; };
    static constexpr unsigned kMaxDoorbells = 16;
    DoorbellSlot              mDoorbells[kMaxDoorbells] {};
    uint32_t                  mDoorbellNext { 0 };
    bool                      doorbellSlot(uint32_t q, uint32_t i, uint32_t &offOut);
    uint64_t                  mNonlocalLimit { 0 };
    bool                      mInitialized { false };

    // A GART page table handed to Apple's accelerator, allocated lazily and used
    // by NOTHING of ours. Apple writes its own PTE format into it; pointing it at
    // the bring-up kext's live GART instead would corrupt the table CP, MES and
    // SDMA are actively translating through.
    amdgpu::SysMem            mAppleGartTable {};

    mutable uint32_t mCalls[43] {};
    mutable uint32_t mTotalCalls { 0 };
    mutable int      mFirstUnsupported { -1 };

    // 0.0.367 — the C13 census, written only by startEngineQueue (see queueCensus above).
    uint64_t mQStarts { 0 }, mQKnown { 0 }, mQUnknown { 0 };
    uint32_t mQTallyOk { 1 };
};

// Each method reports its own return address so the log can say WHICH Apple
// function called it. That is how the accelerator's call order gets mapped.
#define NOTE(slot, name)        note((slot), (name), __builtin_return_address(0))
#define UNSUPPORTED(slot, name) unsupported((slot), (name), __builtin_return_address(0))

extern Navi48Ttl gNavi48Ttl;
