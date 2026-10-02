//
//  amdgpu_sdma.h — System DMA engine (SDMA 7.0.1 / sdma_v7_1) for Navi 48.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/amdgpu_sdma.h
//  which mirrors:
//      drivers/gpu/drm/amd/amdgpu/sdma_v7_1.c
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_0_0_offset.h / _sh_mask.h
//      drivers/gpu/drm/amd/include/asic_reg/gc/gc_12_1_0_offset.h / _sh_mask.h
//
//  RDNA4 (IP_VERSION SDMA(7,0,1)) dispatches to the sdma_v7_1 path —
//  see sdma_v7_1.c:33-34. Note the reference's file header says R9700
//  must use the gc_12_1_0 offsets while its own table selector routes
//  GFX 12.0.x (which is what gfx1201 / Navi 48 reports) to gc_12_0_0.
//  The selector is the code that shipped working (mac-amdgpu v0.1.48),
//  so it is kept verbatim; sdma_init_full() logs which table was picked
//  and the resulting RB_CNTL dword so a hardware run settles it.
//
//  RDNA4 has two SDMA instances, both living inside the GC IP block
//  (per amdgpu_discovery: SDMA0/SDMA1 ⇒ GC_HWIP for SOC15 base
//  resolution). Each instance has three queues (QUEUE0..2); we only
//  drive QUEUE0 for the initial bringup.
//
//  Register addressing (sdma_v7_0_get_reg_offset @ sdma_v7_0.c:125):
//      • Most registers: base = GC base[0] (every regSDMA0_QUEUE0_* /
//        STATUS_REG / UTCL1_* / WATCHDOG_CNTL carries BASE_IDX 0 in
//        gc_12_0_0_offset.h), add SDMA1_REG_OFFSET=0x600 for instance 1.
//      • Hyp-dec range [0x5880, 0x589a] (regSDMA0_VM_CTX_LO ..
//        regSDMA0_IC_CNTL, all BASE_IDX 1) uses GC base[1] with
//        SDMA1_HYP_DEC_REG_OFFSET=0x20 per instance.
//
//  NOTE: there is no sdma_7_0_0_offset.h in Linux. SDMA 7.0's registers
//  live in the GC header — upstream sdma_v7_0.c includes
//  gc/gc_12_0_0_offset.h and gc/gc_12_0_0_sh_mask.h and nothing else for
//  SDMA. That is why SDMA resolves through IPBlock::GC here.
//
//  ==================================================================
//  Deviations from reference (see also sdma_v7_0.cpp)
//  ==================================================================
//   D1. No DriverKit. `IOBufferMemoryDescriptor *wb_buf` + `IODMACommand
//       *wb_dma` become `amdgpu::SysMem wb_sysmem`. The reference could
//       hand the DART iova straight to the engine; on x86 with no IOMMU
//       the GPU reaches system memory only through GART, so the WB page
//       is bound with gmc_bind_existing() and `wb_bus` holds the
//       resulting GART **MC** address (same meaning as the reference:
//       "the address the engine writes to"). If the bind fails the WB
//       page falls back to VRAM (`wb_in_vram`, CPU access via BAR0),
//       which is what the reference already does for the ring.
//   D2. Added sdma_load_microcode(): the reference PSP-loads the SDMA
//       ucode from its userspace LoadFirmware selector
//       (dext/MacAMDGPU.cpp) and only flips SDMAContext::microcode_loaded;
//       we have no userspace, so the same LOAD_IP_FW submission lives
//       here and keeps the reference's placement (before gfx_resume).
//       Firmware bytes are a parameter — this module never depends on
//       src/fw/fw_table.h.
//   D3. Added sdma_vram_copy_test() — port of the reference's
//       kMacAMDGPUMethodSDMACopyVRAM selector (dext/MacAMDGPU.cpp), our
//       "the GPU wrote memory" milestone. No 16 KB stack pattern buffer
//       (kernel stacks are 16 KB): the pattern is generated on the fly.
//   D4. sdma_log_status() is declared here (the reference defines it in
//       the .cpp only) so diagnostics elsewhere can call it.
//   D5. SDMAContext gains `ucode_bin`/`ucode_size` (input for D2) and
//       `instance_present[]` bookkeeping for a possibly-unresolved SDMA1.
//   D6. Added sdma_wb_read32/sdma_wb_write32 so callers can inspect the
//       writeback page without knowing whether it is sysmem or VRAM.
//
#pragma once

#include <stdint.h>
#include <IOKit/IOReturn.h>

#include "amdgpu_ip.h"
#include "amdgpu_regs.h"
#include "amdgpu_sysmem.h"

namespace amdgpu {

struct GMCContext;  // forward — SDMA shares GART/VRAM with GFX
struct PSPContext;  // forward — SDMA microcode goes through the PSP ring

constexpr uint32_t kSDMAInstanceCount     = 2;
constexpr uint32_t kSDMARingDefaultBytes  = 16 * 1024;   // 4096 dwords
constexpr uint32_t kSDMAWBPageBytes       = 16 * 1024;   // AS page granular

// SDMA1 sits 0x600 dwords past SDMA0 in the GC[0] base. The hyp-dec
// range uses the GC[1] base and a 0x20 stride per instance.
//
// gc_12_0_0_offset.h proves both strides:
//   regSDMA0_DEC_START 0x0000 / regSDMA1_DEC_START 0x0600   → 0x600
//   regSDMA0_MCU_CNTL  0x588e / regSDMA1_MCU_CNTL  0x58ae   → 0x20
//   regSDMA0_VM_CTX_LO 0x5880 / regSDMA1_VM_CTX_LO 0x58a0   → 0x20
// and upstream sdma_v7_0.c:51-54 agrees (SDMA1_HYP_DEC_REG_OFFSET 0x20).
// The reference had 0x30 here, which put SDMA1's MCU_CNTL 0x10 dwords
// past the real register — i.e. SDMA1 halt/unhalt wrote into the
// hyp-dec hole and the second engine never came out of reset.
constexpr uint32_t kSDMA1_REG_OFFSET             = 0x600;
constexpr uint32_t kSDMA1_HYP_DEC_REG_OFFSET     = 0x20;
// Unused: the reference carried a second, looser hyp-dec boundary
// (internal_offset >= 0x450) taken from a 12_1_0 comment. Upstream
// sdma_v7_0.c uses only the [SDMA0_HYP_DEC_REG_START,
// SDMA0_HYP_DEC_REG_END] window declared below, and so do we — 0x450
// would have swept the whole regSDMA0_QUEUE*_ block (BASE_IDX 0) onto
// GC base[1]. Kept, unreferenced, as an audit-trail marker.
constexpr uint32_t kSDMA0_SDMA_IDX_0_END_unused  = 0x450;

// Byte offsets inside the 16 KB writeback page. Fixed by the reference:
//   +0x00  RPTR writeback (engine → host)
//   +0x40  WPTR shadow / poll slot (host → engine)
//   +0x80  fence slot used by sdma_ring_test / sdma_copy_linear_test
//   +0xC0  fence slot used by user submissions (reference CS path)
constexpr uint32_t kSDMAWBRptrOffset   = 0x00;
constexpr uint32_t kSDMAWBWptrOffset   = 0x40;
constexpr uint32_t kSDMAWBFenceOffset  = 0x80;
constexpr uint32_t kSDMAWBCSFenceOffset = 0xC0;

// Writeback slots for an EXTERNAL queue (one Apple owns, driven through
// sdma_program_external_queue). Carved from the SAME 16 KiB per-instance page:
// QUEUE0 uses 0x00/0x40/0x80/0xC0, so 0x100/0x140 are free, already GART-bound
// and already proven by the QUEUE0 path. No second allocation, and no chance of
// colliding with the writeback slots of the ring we are actively running.
constexpr uint32_t kSDMAWBExtRptrOffset = 0x100;
constexpr uint32_t kSDMAWBExtWptrOffset = 0x140;
// 0.0.185: a third EXTERNAL slot, for the QUEUE1 routing self-test's FENCE.
// QUEUE0 owns 0x00/0x40/0x80/0xC0 and the external rptr/wptr take 0x100/0x140,
// so 0x180 is the next free 64-byte slot in the same already-bound 16 KiB page.
constexpr uint32_t kSDMAWBExtFenceOffset = 0x180;

// QUEUE0 -> QUEUE1 register stride. Confirmed against the vendored header
// (ref/linux-asic-reg/gc_12_0_0_offset.h): regSDMA0_QUEUE0_MINOR_PTR_UPDATE
// 0x009b -> regSDMA0_QUEUE1_MINOR_PTR_UPDATE 0x00f3, and
// regSDMA1_QUEUE1_RB_CNTL 0x06d8 = 0x0080 + kSDMA1_REG_OFFSET + this. The stride
// holds on BOTH instances, which is why it is expressed as one constant instead
// of a second hand-transcribed offset table.
//
// 0.0.185 re-verified EVERY register the external-queue path writes, QUEUE0 line
// -> QUEUE1 line in ref/linux-asic-reg/gc_12_0_0_offset.h, because the SDMA
// takeover programs all of them and a single wrong offset is a hardware cycle:
//   RB_CNTL              0x0080 :182  -> 0x00d8 :276
//   RB_BASE              0x0081 :184  -> 0x00d9 :278
//   RB_BASE_HI           0x0082 :186  -> 0x00da :280
//   RB_RPTR              0x0083 :188  -> 0x00db :282
//   RB_RPTR_HI           0x0084 :190  -> 0x00dc :284
//   RB_WPTR              0x0085 :192  -> 0x00dd :286
//   RB_WPTR_HI           0x0086 :194  -> 0x00de :288
//   RB_RPTR_ADDR_LO      0x0087 :196  -> 0x00df :290
//   RB_RPTR_ADDR_HI      0x0088 :198  -> 0x00e0 :292
//   IB_CNTL              0x0089 :200  -> 0x00e1 :294
//   DOORBELL             0x008f :212  -> 0x00e7 :306
//   DOORBELL_OFFSET      0x0091 :216  -> 0x00e9 :310
//   RB_WPTR_POLL_ADDR_LO 0x0098 :230  -> 0x00f0 :324
//   RB_WPTR_POLL_ADDR_HI 0x0099 :232  -> 0x00f1 :326
//   MINOR_PTR_UPDATE     0x009b :236  -> 0x00f3 :330
// Every delta is 0x58, with no exception and no gap.
constexpr uint32_t kQueue0ToQueue1Stride = 0x58;

// 0.0.185 — the doorbell DWORD index the SDMA takeover gives Apple's ring on
// SDMA0 QUEUE1. Inside the routed S2A window 0x200..0x213 (: ENTRY_2 =
// 0x3029001d, RANGE_OFFSET=0x200 RANGE_SIZE=0x14), unassigned in our own map
// (sdma_engine[0]<<1 = 0x200 is our QUEUE0, sdma_engine[1]<<1 = 0x214 is SDMA1
// QUEUE0), so no NBIF change is needed. Whether ONE window routes TWO indices to
// two queues on one engine is the open question the boot-time self-test settles.
constexpr uint32_t kSDMAQ1AppleDoorbellIndex = 0x202;

// 0.0.196 — HOW MANY QUEUES EACH SDMA INSTANCE HAS.
//
// CONFIRMED from ref/linux-asic-reg/gc_12_0_0_offset.h: QUEUE0..QUEUE7 exist on
// both instances, at a flat 0x58 dword stride and nothing beyond QUEUE7.
//   regSDMA0_QUEUE0_RB_CNTL 0x0080 (:182)   regSDMA1_QUEUE0_RB_CNTL 0x0680 (:1164)
//   regSDMA0_QUEUE1_RB_CNTL 0x00d8 (:276)   regSDMA1_QUEUE1_RB_CNTL 0x06d8 (:1258)
//   regSDMA0_QUEUE2_RB_CNTL 0x0130 (:370)   regSDMA1_QUEUE2_RB_CNTL 0x0730 (:1352)
//   regSDMA0_QUEUE3_RB_CNTL 0x0188 (:464)   regSDMA1_QUEUE3_RB_CNTL 0x0788 (:1446)
//   regSDMA0_QUEUE4_RB_CNTL 0x01e0 (:558)   regSDMA1_QUEUE4_RB_CNTL 0x07e0 (:1540)
//   regSDMA0_QUEUE5_RB_CNTL 0x0238 (:652)   regSDMA1_QUEUE5_RB_CNTL 0x0838 (:1634)
//   regSDMA0_QUEUE6_RB_CNTL 0x0290 (:746)   regSDMA1_QUEUE6_RB_CNTL 0x0890 (:1728)
//   regSDMA0_QUEUE7_RB_CNTL 0x02e8 (:840)   regSDMA1_QUEUE7_RB_CNTL 0x08e8 (:1822)
// 0x00d8 - 0x0080 = 0x58 and 0x0680 - 0x0080 = 0x600 (the instance stride), so
// the queue register block for (instance i, queue q) is
//   GC base + i*0x600 + q*0x58 + <QUEUE0 offset>
// which is exactly what sdma_reg_offset(dev, i, r + q*kQueue0ToQueue1Stride) does.
// QUEUE0 on BOTH instances stays OURS; queues 1..7 are the takeover's to spend.
constexpr uint32_t kSDMAQueuesPerInstance = 8;

// 0.0.196 — the hardware queues `sdmamap` is allowed to hand out, in the order
// it hands them out.
//
// Doorbell dword indices all sit inside the ONE routed S2A window this card has
// for SDMA: ENTRY_2 = 0x3029001d, RANGE_OFFSET 0x200, RANGE_SIZE 0x14,
// i.e. dwords [0x200 .. 0x213]. 0x200/0x201 are our own SDMA0 QUEUE0's 64-bit
// doorbell; 0x214/0x215 are SDMA1 QUEUE0's and are OUTSIDE the window (which is
// why SDMA1 QUEUE0 is MMIO-kicked today). Every index below is even, unassigned
// in DoorbellIndex, and 8 bytes clear of its neighbour.
//
// SUSPECTED, and what `navi48-sdma-qn-test=1` settles per triple: that one S2A
// window routes to BOTH instances. Upstream's own comment says entry 2 covers
// every SDMA engine (nbif_v6_3_1.c:153, doorbell_size = range * num_instances),
// but this driver programs size 20 for one engine's worth, so the SDMA1 slots
// below are the ones most likely to come back "MMIO only".
struct SDMAExtQueueSlot {
    uint8_t  instance;
    uint8_t  queue;           // 1..7; QUEUE0 is never offered
    uint16_t doorbell_dword;
};
constexpr SDMAExtQueueSlot kSDMAExtSlots[] = {
    { 0, 1, 0x202 },   // proven by navi48-sdma-q1-test=1
    { 0, 2, 0x204 },
    { 0, 3, 0x206 },
    { 0, 4, 0x208 },
    { 0, 5, 0x20a },
    { 0, 6, 0x20c },
    { 0, 7, 0x20e },
    { 1, 1, 0x210 },   // SUSPECTED: does entry 2's window reach instance 1?
    { 1, 2, 0x212 },   // SUSPECTED, same question
};
constexpr uint32_t kSDMAExtSlotCount =
    sizeof(kSDMAExtSlots) / sizeof(kSDMAExtSlots[0]);

//------------------------------------------------------------------
// SDMA0 register offsets (relative to GC IP base for non-hyp-dec,
// or relative to GC[1] base for hyp-dec). All values are dword
// offsets, taken verbatim from the upstream asic_reg headers.
//------------------------------------------------------------------
// SDMA register offsets — per-chip-family tables selected at runtime
// from the discovered GC IP version (dev.ip.getVersion(IPBlock::GC)).
//
// [[feedback_mac_amdgpu_per_ip_version_offsets]] — same driver binary
// must work for ANY supported AMD card, so register offsets cannot be
// hardcoded for one chip family. Each supported (GC major, minor) gets
// its own offset table; sdma_reg_offset() picks the right one based on
// the chip discovery returned.
//
// Currently supported families:
//   • gc_12_0_X — Navi 48 / R9700 (gfx1201, GFX 12.0.1) and other
//     GFX 12.0 chips. Source: upstream gc_12_0_0_offset.h.
//   • gc_12_1_X — gfx1250 and the "SDMA0_SDMA_QUEUE0_*" naming family.
//     Same registers, offsets are 0x180 higher. Source: upstream
//     gc_12_1_0_offset.h.
//
// Adding a new family: define another SDMARegOffsets_gc_*_*_* struct,
// add it to sdma_pick_reg_table()'s dispatch, link the relevant chips
// to it. Don't fork sdma_v7_0.cpp — same code reads through the table.
struct SDMARegOffsets {
    uint32_t STATUS_REG;
    uint32_t WATCHDOG_CNTL;
    uint32_t UTCL1_CNTL;
    uint32_t UTCL1_PAGE;
    uint32_t QUEUE0_RB_CNTL;
    uint32_t QUEUE0_RB_BASE;
    uint32_t QUEUE0_RB_BASE_HI;
    uint32_t QUEUE0_RB_RPTR;
    uint32_t QUEUE0_RB_RPTR_HI;
    uint32_t QUEUE0_RB_WPTR;
    uint32_t QUEUE0_RB_WPTR_HI;
    uint32_t QUEUE0_RB_RPTR_ADDR_LO;
    uint32_t QUEUE0_RB_RPTR_ADDR_HI;
    uint32_t QUEUE0_IB_CNTL;
    uint32_t QUEUE0_DOORBELL;
    uint32_t QUEUE0_DOORBELL_OFFSET;
    uint32_t QUEUE0_RB_WPTR_POLL_ADDR_LO;
    uint32_t QUEUE0_RB_WPTR_POLL_ADDR_HI;
    uint32_t QUEUE0_MINOR_PTR_UPDATE;
    uint32_t MCU_CNTL;  // hyp-dec range; uses GC BASE_IDX 1 not 0
    const char *name;   // for the bring-up log (Navi48Bringup addition)
};

// gc_12_0_0 — for GFX 12.0.x (Navi 48 / R9700, etc.).
inline constexpr SDMARegOffsets kSDMARegOffsets_gc_12_0_0 = {
    /* STATUS_REG                  */ 0x0024,  // gc_12_0_0_offset.h:56
    /* WATCHDOG_CNTL               */ 0x002b,  // gc_12_0_0_offset.h:70
    /* UTCL1_CNTL                  */ 0x0035,  // gc_12_0_0_offset.h:90
    /* UTCL1_PAGE                  */ 0x0038,  // gc_12_0_0_offset.h:96
    /* QUEUE0_RB_CNTL              */ 0x0080,  // gc_12_0_0_offset.h:182
    /* QUEUE0_RB_BASE              */ 0x0081,  // gc_12_0_0_offset.h:184
    /* QUEUE0_RB_BASE_HI           */ 0x0082,  // gc_12_0_0_offset.h:186
    /* QUEUE0_RB_RPTR              */ 0x0083,  // gc_12_0_0_offset.h:188
    /* QUEUE0_RB_RPTR_HI           */ 0x0084,  // gc_12_0_0_offset.h:190
    /* QUEUE0_RB_WPTR              */ 0x0085,  // gc_12_0_0_offset.h:192
    /* QUEUE0_RB_WPTR_HI           */ 0x0086,  // gc_12_0_0_offset.h:194
    /* QUEUE0_RB_RPTR_ADDR_LO      */ 0x0087,  // gc_12_0_0_offset.h:196
    /* QUEUE0_RB_RPTR_ADDR_HI      */ 0x0088,  // gc_12_0_0_offset.h:198
    /* QUEUE0_IB_CNTL              */ 0x0089,  // gc_12_0_0_offset.h:200
    /* QUEUE0_DOORBELL             */ 0x008f,  // gc_12_0_0_offset.h:212
    /* QUEUE0_DOORBELL_OFFSET      */ 0x0091,  // gc_12_0_0_offset.h:216
    /* QUEUE0_RB_WPTR_POLL_ADDR_LO */ 0x0098,  // gc_12_0_0_offset.h:230
    /* QUEUE0_RB_WPTR_POLL_ADDR_HI */ 0x0099,  // gc_12_0_0_offset.h:232
    /* QUEUE0_MINOR_PTR_UPDATE     */ 0x009b,  // gc_12_0_0_offset.h:236
    /* MCU_CNTL                    */ 0x588e,  // gc_12_0_0_offset.h:948
    /* name                        */ "gc_12_0_0",
};

// gc_12_1_0 — for GFX 12.1.x (gfx1250 etc.). Offsets +0x180 vs 12.0.x.
inline constexpr SDMARegOffsets kSDMARegOffsets_gc_12_1_0 = {
    /* STATUS_REG                  */ 0x0024,  // gc_12_1_0_offset.h:56
    /* WATCHDOG_CNTL               */ 0x002b,  // gc_12_1_0_offset.h:70
    /* UTCL1_CNTL                  */ 0x0037,  // gc_12_1_0_offset.h:94
    /* UTCL1_PAGE                  */ 0x003a,  // gc_12_1_0_offset.h:100
    /* QUEUE0_RB_CNTL              */ 0x0200,  // gc_12_1_0_offset.h:194
    /* QUEUE0_RB_BASE              */ 0x0201,  // gc_12_1_0_offset.h:196
    /* QUEUE0_RB_BASE_HI           */ 0x0202,  // gc_12_1_0_offset.h:198
    /* QUEUE0_RB_RPTR              */ 0x0203,  // gc_12_1_0_offset.h:200
    /* QUEUE0_RB_RPTR_HI           */ 0x0204,  // gc_12_1_0_offset.h:202
    /* QUEUE0_RB_WPTR              */ 0x0205,  // gc_12_1_0_offset.h:204
    /* QUEUE0_RB_WPTR_HI           */ 0x0206,  // gc_12_1_0_offset.h:206
    /* QUEUE0_RB_RPTR_ADDR_LO      */ 0x0207,  // gc_12_1_0_offset.h:208
    /* QUEUE0_RB_RPTR_ADDR_HI      */ 0x0208,  // gc_12_1_0_offset.h:210
    /* QUEUE0_IB_CNTL              */ 0x0209,  // gc_12_1_0_offset.h:212
    /* QUEUE0_DOORBELL             */ 0x020f,  // gc_12_1_0_offset.h:224
    /* QUEUE0_DOORBELL_OFFSET      */ 0x0211,  // gc_12_1_0_offset.h:228
    /* QUEUE0_RB_WPTR_POLL_ADDR_LO */ 0x0218,  // gc_12_1_0_offset.h:242
    /* QUEUE0_RB_WPTR_POLL_ADDR_HI */ 0x0219,  // gc_12_1_0_offset.h:244
    /* QUEUE0_MINOR_PTR_UPDATE     */ 0x021b,  // gc_12_1_0_offset.h:248
    /* MCU_CNTL                    */ 0x588e,  // gc_12_1_0_offset.h:1224
    /* name                        */ "gc_12_1_0",
};

// Selector — pick the offset table by discovered GC IP version.
// Falls back to gc_12_0_0 if the version is unknown so partial-discovery
// runs (or older test traces) don't crash; sdma_init_full logs the pick.
inline const SDMARegOffsets &
sdma_pick_reg_table(const DeviceContext &dev)
{
    const IPVersion gc = dev.ip.getVersion(IPBlock::GC);
    // GFX 12.1.x family.
    if (gc.major == 12 && gc.minor == 1) {
        return kSDMARegOffsets_gc_12_1_0;
    }
    // GFX 12.0.x family (default — also catches {0,0,0} unresolved).
    return kSDMARegOffsets_gc_12_0_0;
}

// Convenience accessor: sdma_regs(dev).QUEUE0_RB_CNTL etc. Returns the
// selected per-chip table at runtime.
inline const SDMARegOffsets &sdma_regs(const DeviceContext &dev) {
    return sdma_pick_reg_table(dev);
}

//------------------------------------------------------------------
// Field shift / mask defs — minimal subset for ring resume.
// Mirrors upstream gc_12_1_0_sh_mask.h naming so REG_SET_FIELD can
// be used directly. Note the "SDMA0_SDMA_*" infix — the 12_1_0
// layout renamed every SDMA0_ macro to SDMA0_SDMA_ (sdma_v7_1.c
// uses these spellings throughout). The BIT POSITIONS are identical
// on gc_12_0_0 (where upstream spells them SDMA0_QUEUE0_* and calls
// MCU_WPTR_POLL_ENABLE "F32_WPTR_POLL_ENABLE"); only the register
// OFFSETS moved between the two families, which is what the tables
// above capture. One field-def block therefore serves both.
//------------------------------------------------------------------
// gc_12_1_0_sh_mask.h lines cited per shift below.
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_ENABLE__SHIFT                    0x0      // line 985
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_ENABLE_MASK                      0x00000001
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_SIZE__SHIFT                      0x1      // line 986
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_SIZE_MASK                        0x0000003E
#define SDMA0_SDMA_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE__SHIFT             0x8      // line 987
#define SDMA0_SDMA_QUEUE0_RB_CNTL__WPTR_POLL_ENABLE_MASK               0x00000100
#define SDMA0_SDMA_QUEUE0_RB_CNTL__MCU_WPTR_POLL_ENABLE__SHIFT         0xb      // line 990
#define SDMA0_SDMA_QUEUE0_RB_CNTL__MCU_WPTR_POLL_ENABLE_MASK           0x00000800
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE__SHIFT        0xc      // line 991
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK          0x00001000
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_PRIV__SHIFT                      0x17     // line 994
#define SDMA0_SDMA_QUEUE0_RB_CNTL__RB_PRIV_MASK                        0x00800000

#define SDMA0_SDMA_QUEUE0_IB_CNTL__IB_ENABLE__SHIFT                    0x0      // line 1032
#define SDMA0_SDMA_QUEUE0_IB_CNTL__IB_ENABLE_MASK                      0x00000001

#define SDMA0_SDMA_QUEUE0_DOORBELL__ENABLE__SHIFT                      0x1c     // line 1058
#define SDMA0_SDMA_QUEUE0_DOORBELL__ENABLE_MASK                        0x10000000

#define SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET__OFFSET__SHIFT               0x2      // line 1068
#define SDMA0_SDMA_QUEUE0_DOORBELL_OFFSET__OFFSET_MASK                 0x0FFFFFFC

#define SDMA0_SDMA_MCU_CNTL__HALT__SHIFT                               0x0      // line 3719
#define SDMA0_SDMA_MCU_CNTL__HALT_MASK                                 0x00000001
#define SDMA0_SDMA_MCU_CNTL__RESET__SHIFT                              0x1      // line 3720
#define SDMA0_SDMA_MCU_CNTL__RESET_MASK                                0x00000002

#define SDMA0_SDMA_UTCL1_CNTL__REDO_DELAY__SHIFT                       0x0      // line 393
#define SDMA0_SDMA_UTCL1_CNTL__REDO_DELAY_MASK                         0x0000001F
#define SDMA0_SDMA_UTCL1_CNTL__RESP_MODE__SHIFT                        0x9      // line 395
#define SDMA0_SDMA_UTCL1_CNTL__RESP_MODE_MASK                          0x00000600

#define SDMA0_SDMA_WATCHDOG_CNTL__QUEUE_HANG_COUNT__SHIFT              0x0      // line 289
#define SDMA0_SDMA_WATCHDOG_CNTL__QUEUE_HANG_COUNT_MASK                0x000000FF

//------------------------------------------------------------------
// SDMA opcode + helpers — sdma_pkt_open.h subset for NOP / FENCE /
// TRAP. Enough to emit a ring test that the host can wait on.
//------------------------------------------------------------------
constexpr uint32_t SDMA_OP_NOP    = 0;
constexpr uint32_t SDMA_OP_COPY   = 1;
constexpr uint32_t SDMA_OP_FENCE  = 5;
constexpr uint32_t SDMA_OP_TRAP   = 6;
constexpr uint32_t SDMA_OP_TIMESTAMP = 13;
//: SRBM_WRITE (register write from the command stream). Apple emits
// header 0xf000000e = op 0x0e with the BYTE-ENABLE nibble 0xf at bits 31:28.
// NOTE: SDMA_PKT_HEADER_CPV is NOT this field -- it sets a single bit at 28,
// so building the header with it silently emits 0x1000000e instead.
constexpr uint32_t SDMA_OP_SRBM_WRITE = 14;

// INDIRECT_BUFFER — ref/linux-amdgpu/sdma_v7_0.c:285-292
// (sdma_v7_0_ring_emit_ib). Six dwords: header, base_lo & 0xffffffe0 (32-byte
// aligned), base_hi, length_dw, csa_lo, csa_hi. Linux sets only VMID:
// SDMA_PKT_INDIRECT_HEADER_VMID(vmid & 0xf) — shift 16 per sdma_pkt_open.h.
// Apple's ring carries header 0x80000004 ( dumps: `[000a] 80000004
// 00800000 00000084 00000055`), i.e. op 4 with BIT 31 set and VMID 0. Bit 31 of
// the INDIRECT header is `priv` in sdma_pkt_open.h — SUSPECTED, no vendored copy
// of that header is in ref/. Both spellings are emitted by the 0.0.191 SRBM
// self-test so the hardware says which one an IB needs.
constexpr uint32_t SDMA_OP_INDIRECT = 4;
static inline uint32_t SDMA_PKT_INDIRECT_HEADER_VMID(uint32_t v) {
    return ((v & 0xfu) << 16);
}
static inline uint32_t SDMA_PKT_INDIRECT_HEADER_PRIV(uint32_t v) {
    return ((v & 0x1u) << 31);
}

constexpr uint32_t SDMA_SUBOP_COPY_LINEAR = 0;

// 0.0.345 — COPY_TILED_SUB_WINDOW, sub-opcode 5 of SDMA_OP_COPY.
// THE DETILE PACKET. The composited surface Apple hands the pipe shim is TILED
// (: res+0x1dc = 0x3b, SW_MODE 27, 2D, 32 bpp, no DCC), so a row-by-row
// COPY_LINEAR can never produce a correct frame from it.
//
// Layout source, in the tree, with EXPLICIT SDMA_7_0 branches:
//   re/graphics/src/mesa/src/amd/common/ac_cmdbuf_sdma.c:321-383
//       (ac_emit_sdma_copy_tiled_sub_window)
//   re/graphics/src/mesa/src/amd/common/ac_cmdbuf_sdma.c:270-286
//       (ac_sdma_get_tiled_info_dword; the >= SDMA_7_0 return is
//        element_size | swizzle<<3 | (mip_max-1)<<16 | mip_id<<24)
//   re/graphics/src/mesa/src/amd/common/ac_cmdbuf_sdma.c:210-223
//       (ac_sdma_get_tiled_header_dword returns 0 for >= SDMA_5_0)
//   re/graphics/src/mesa/src/amd/common/sid.h:325-336 (the opcode numbers)
// Corroborated field-for-field, with widths, by AMD's own
//   re/linux-dc/drivers/gpu/drm/amd/display/dmub/inc/dmub_cmd.h:2320-2366
//       (struct lsdma_tiled_copy_data).
//
// FOUR THINGS THAT WOULD HANG THE RING, and what this file does about them:
//  1. CPV MUST BE 0. found COPY_LINEAR grows a trailing dword only
//     when CPV is set; mesa never sets CPV on any copy. With CPV 0 and DCC 0
//     this packet is EXACTLY 14 dwords. Nothing below ever sets it.
//  2. The swizzle in the info dword is the GFX12 ADDR3 enum, not Apple's gfx10
//     AddrSwizzleMode. The bytes on the ground were laid down by gfx1201 CB
//     AFTER our register translation, i.e. in gfx12 numbering
//     (3 = ADDR3_64KB_2D); 27 is Apple's pre-translation value.
//  3. There is NO `dimension` field at bit 9 on SDMA 7.0 — that is the 5.x/6.x
//     encoding, and mesa's 7.0 branch omits it (ac_cmdbuf_sdma.c:243-246).
//  4. The ring pointer mask: 14 + 4 dwords instead of n*8 + 4. sdma_ring_write
//     masks only the ring INDEX and lets inst.wptr free-run, which is
//     packet-size independent; a packet may straddle the wrap exactly as the
//     1604-dword chunks already did.
constexpr uint32_t SDMA_SUBOP_COPY_TILED_SUB_WINDOW = 5;
constexpr uint32_t kSDMATiledSubWindowDwords = 14;     // dcc = 0, CPV = 0

// dw0 extras. tmz is bit 18 (mesa passes it as the packet's `n` field = 4),
// dcc is bit 19, detile is bit 31 (1 = tiled -> linear).
static inline uint32_t SDMA_PKT_TILED_SW_TMZ(uint32_t v)    { return ((v & 0x1u) << 18); }
static inline uint32_t SDMA_PKT_TILED_SW_DCC(uint32_t v)    { return ((v & 0x1u) << 19); }
static inline uint32_t SDMA_PKT_TILED_SW_DETILE(uint32_t v) { return ((v & 0x1u) << 31); }
// dw3 / dw9: x | y << 16.  dw4: z | (width-1) << 16.  dw5: (height-1) | (depth-1) << 16.
static inline uint32_t SDMA_PKT_TILED_SW_XY(uint32_t x, uint32_t y) {
    return (x & 0xffffu) | ((y & 0xffffu) << 16);
}
static inline uint32_t SDMA_PKT_TILED_SW_Z_W(uint32_t z, uint32_t widthMinus1) {
    return (z & 0x1fffu) | ((widthMinus1 & 0xffffu) << 16);
}
static inline uint32_t SDMA_PKT_TILED_SW_HD(uint32_t heightMinus1, uint32_t depthMinus1) {
    return (heightMinus1 & 0xffffu) | ((depthMinus1 & 0xffffu) << 16);
}
// dw6 info: element_size (log2 BYTES per element) | swizzle << 3 |
//           (mip_levels - 1) << 16 | mip_id << 24.  SDMA 7.0 branch only.
static inline uint32_t SDMA_PKT_TILED_SW_INFO(uint32_t elementSizeLog2, uint32_t swizzle,
                                              uint32_t mipMaxMinus1, uint32_t mipId) {
    return (elementSizeLog2 & 0x7u) | ((swizzle & 0x1fu) << 3) |
           ((mipMaxMinus1 & 0x1fu) << 16) | ((mipId & 0x1fu) << 24);
}
// dw10: z | (pitch - 1) << 16, pitch in ELEMENTS.  dw12: (w-1) | (h-1) << 16.
static inline uint32_t SDMA_PKT_TILED_SW_Z_PITCH(uint32_t z, uint32_t pitchMinus1) {
    return (z & 0x1fffu) | ((pitchMinus1 & 0xffffu) << 16);
}
static inline uint32_t SDMA_PKT_TILED_SW_RECT(uint32_t wMinus1, uint32_t hMinus1) {
    return (wMinus1 & 0xffffu) | ((hMinus1 & 0xffffu) << 16);
}
// gfx12 ADDR3 swizzle enums (mesa src/amd/common/ac_surface.h / amdgpu_drm.h
// ADDR3_*). Only 64KB_2D is emitted by this driver.
constexpr uint32_t kAddr3Linear   = 0;
constexpr uint32_t kAddr3_256B_2D = 1;
constexpr uint32_t kAddr3_4KB_2D  = 2;
constexpr uint32_t kAddr3_64KB_2D = 3;
// CONSTANT_FILL — ref/linux-amdgpu/sdma_v7_0.c:1796-1808
// (sdma_v7_0_emit_fill_buffer): header | COMPRESS(1), dst lo, dst hi, data,
// byte_count - 1. COMPRESS is bit 16 on SDMA 7 (sdma_v7_0.c:56-60). The
// Apple-IB walker in src/apple/AppleHardwareHook.cpp independently decodes
// this opcode as 5 dwords with the destination at [1][2].
constexpr uint32_t SDMA_OP_CONST_FILL = 11;
static inline uint32_t SDMA_PKT_CONST_FILL_COMPRESS(uint32_t v) {
    return ((v & 0x1u) << 16);
}

static inline uint32_t SDMA_PKT_HEADER_OP(uint32_t op)         { return (op & 0xff); }
static inline uint32_t SDMA_PKT_HEADER_SUB_OP(uint32_t sub_op) { return ((sub_op & 0xff) << 8); }
static inline uint32_t SDMA_PKT_HEADER_CPV(uint32_t v)         { return ((v & 0x1) << 28); }
static inline uint32_t SDMA_PKT_HEADER_BYTE_EN(uint32_t v)     { return ((v & 0xf) << 28); }

// COPY_LINEAR max byte count is 0x400000 - 1 on RDNA4
// (HW counter is a 22-bit field, byte_count - 1).
constexpr uint32_t kSDMACopyLinearMaxBytes = 0x00400000u;

//------------------------------------------------------------------
// Per-instance ring state. Two instances live side by side; we
// drive both with the same layout.
//------------------------------------------------------------------
struct SDMAInstance {
    uint32_t  instance;          // 0 or 1
    bool      inited;
    bool      enabled;

    // Ring buffer — VRAM-resident (the reference moved it there because
    // GPU-initiated reads of DART-mapped sysmem return zero on AS+TB5;
    // on x86 the FB aperture is still the simplest GMC-routable place
    // and needs no GART entry). Engine reads packets through the FB
    // aperture using ring_gpu_va; CPU writes go through BAR0 at
    // ring_vram_off.
    uint64_t  ring_gpu_va;       // MC address (= ring_bus equivalent)
    uint64_t  ring_vram_off;     // BAR0-relative byte offset for CPU writes
    uint32_t  ring_size_dwords;
    uint32_t  ring_ptr_mask;

    // Writeback page (rptr writeback + wptr shadow + fence slots).
    // Deviation D1: sysmem + GART bind, with a VRAM fallback.
    SysMem    wb_sysmem;         // valid when !wb_in_vram
    bool      wb_in_vram;        // true → CPU access through BAR0
    uint64_t  wb_vram_off;       // BAR0-relative byte offset (wb_in_vram)
    uint64_t  wb_phys;           // host physical address (!wb_in_vram)

    uint64_t  wb_bus;            // GPU-visible (MC) address of the WB page
    void     *wb_cpu;            // CPU pointer (nullptr when wb_in_vram)
    uint64_t  rptr_gpu_addr;     // wb_bus + 0
    uint64_t  wptr_poll_gpu_addr;// wb_bus + 0x40
    volatile uint32_t *rptr_cpu; // nullptr when wb_in_vram

    // 0.0.343 — FREE-RUNNING, NOT a ring index. Upstream keeps TWO masks per ring
    // (ref/linux-amdgpu/amdgpu_ring.c:347-349): buf_mask = ring_size/4 - 1 masks the INDEX into the
    // ring buffer, ptr_mask masks the POINTER handed to the hardware, and for SDMA v7
    // (.support_64bit_ptrs = true, ref/linux-amdgpu/sdma_v7_0.c:1676) ptr_mask is 0xffffffffffffffff,
    // so `ring->wptr &= ring->ptr_mask` is a no-op and the wptr never wraps. We had ONE mask and used
    // it for both, so this folded to 0 every 4096 dwords and the doorbell value went BACKWARDS at each
    // 16 KiB of ring traffic; the engine then executed nothing and the completion fence never landed.
    // Measured: run `wrap1`, 0.0.342 — `scanout 1` passed 7 times and failed on the 8th with
    // `status 10, fence 0000000000 after 1000000 us`, at exactly the submission that crossed 4096.
    // Mask with ring_ptr_mask at the two places that need a ring index, nowhere else.
    uint64_t  wptr;             // software wptr (free-running dword counter, NEVER wrapped)
    // DWORD offset into the doorbell BAR (BAR2). Programmed into
    // SDMA_QUEUE0_DOORBELL_OFFSET. SOC21 default: 0x200 for SDMA0,
    // 0x214 for SDMA1 (= sdma_engine[i] << 1).
    uint32_t  doorbell_index;
};

struct SDMAContext {
    SDMAInstance instance[kSDMAInstanceCount];
    bool         microcode_loaded;

    // ---- Navi48Bringup additions (deviation D5) ---------------------
    // Optional SDMA firmware image (contents of sdma_7_0_1.bin). When
    // microcode_loaded is false and these are set, sdma_init_full()
    // PSP-loads the image itself (sdma_load_microcode). Leave null to
    // get exactly the reference's behaviour (defer and log).
    const void  *ucode_bin;
    uint32_t     ucode_size;

    // Per-instance presence, decided at init time (SDMA1's discovery
    // entry can be missing on a partial IP walk).
    bool         instance_present[kSDMAInstanceCount];
};

//------------------------------------------------------------------
// API
//------------------------------------------------------------------

// Compute the absolute BAR5 dword offset for an SDMA register on a
// given instance. Direct port of sdma_v7_0_get_reg_offset (sdma_v7_0.c:125).
//
// Two register regimes:
//   Hyp-dec (HYPervisor DECoded) range [0x5880..0x589a] inclusive —
//     resolves through GC BASE_IDX 1; instance increment is
//     SDMA1_HYP_DEC_REG_OFFSET (0x30).
//   Everything else — GC BASE_IDX 0; instance increment is
//     SDMA1_REG_OFFSET (0x600) for instance==1.
//
// **v0.1.40 fix:** prior versions collapsed both regimes onto
// GC BASE_IDX 0, which meant MCU_CNTL (0x588e, in hyp-dec range) was
// being read/written at GC[0]+0x588e instead of GC[1]+0x588e. The
// resulting register hit a completely unrelated location, returning
// the apparently random 0x92929292 pattern and silently ignoring
// HALT/RESET writes — so the SDMA MCU never actually unhalted, and
// engine-side WPTR updates from the doorbell aperture never landed.
constexpr uint32_t kSDMA0_HYP_DEC_REG_START = 0x5880;
constexpr uint32_t kSDMA0_HYP_DEC_REG_END   = 0x589a;

static inline uint32_t
sdma_reg_offset(const DeviceContext &ctx, uint32_t instance, uint32_t reg)
{
    if (reg >= kSDMA0_HYP_DEC_REG_START && reg <= kSDMA0_HYP_DEC_REG_END) {
        const uint32_t base = ctx.ip.getBase(IPBlock::GC, /*baseIdx=*/1);
        const uint32_t inst_add = (instance != 0)
            ? (kSDMA1_HYP_DEC_REG_OFFSET * instance) : 0u;
        return base + reg + inst_add;
    }
    const uint32_t base = ctx.ip.get(IPBlock::GC);  // BASE_IDX 0
    return base + reg + (instance == 1 ? kSDMA1_REG_OFFSET : 0u);
}

// Allocate ring + WB page for one instance. Idempotent.
kern_return_t sdma_alloc_storage(DeviceContext &dev, SDMAInstance &inst,
                                 GMCContext &gmc);

// Release what sdma_alloc_storage allocated (Navi48Bringup addition —
// the reference leaks on unload; a kext must not).
void sdma_release_storage(GMCContext &gmc, SDMAInstance &inst);

// Halt/unhalt one engine via SDMA0_MCU_CNTL.HALT.
kern_return_t sdma_engine_halt(const DeviceContext &dev,
                               uint32_t instance, bool halt);

// Stop the GFX queue: clear RB_ENABLE + IB_ENABLE on QUEUE0.
kern_return_t sdma_gfx_stop_instance(const DeviceContext &dev,
                                     uint32_t instance);

// Port of sdma_v7_0_gfx_resume_instance(restore=false).
// Programs QUEUE0 HQD + unhalts the engine + enables the ring.
kern_return_t sdma_gfx_resume_instance(const DeviceContext &dev,
                                       SDMAInstance &inst);

// Kick QUEUE0's doorbell with the current software wptr (byte
// offset, so wptr_dword << 2). Writes to BAR2 at (doorbell_index * 8)
// — see amdgpu_mm_wdoorbell64 @ amdgpu_doorbell_mgr.c for the stride.
// Point a gfx12 SDMA QUEUE1 at a ring somebody else owns.
//
// This is the bridge for Apple's "14 (SDMA1.1)" ring: instance 1, QUEUE1, ring
// at a GART address Apple allocated and fills itself. Our own QUEUE0 ring on the
// same instance is untouched.
//
// Only QUEUE1-SCOPED registers are written. WATCHDOG_CNTL, UTCL1_CNTL,
// UTCL1_PAGE and MCU_CNTL carry no QUEUE prefix -- they are INSTANCE-wide and
// already programmed by the QUEUE0 resume, whose ring is live. Rewriting them
// (or re-unhalting the MCU) would perturb a running queue, so steps 10-13 of
// sdma_gfx_resume_instance are deliberately omitted here.
//
// enable_ring MUST be false for the first pass. With RB_ENABLE=0 the engine
// fetches nothing, so this call is inert with respect to silicon: it is's
// read-only form. Nothing here rings a doorbell -- that is sdma_kick_doorbell,
// and it stays unreachable until the ring has been dumped, decoded, and every
// embedded address proven RESIDENT in our page table.
kern_return_t sdma_program_external_queue(const DeviceContext &dev,
                                          SDMAInstance &inst,
                                          uint64_t ring_gpu_va,
                                          uint32_t ring_size_dwords,
                                          uint32_t doorbell_index,
                                          bool enable_ring);

// 0.0.185 — the same programming with the two write-back addresses supplied by
// the caller instead of carved from our own WB page.
//
// The SDMA takeover ( / notes/re/sdma-takeover-design.md) has to hand the
// engine APPLE'S addresses: its wptr write-back (ring->0xd0, the value slot 36
// received at IN+0x18) goes in RB_WPTR_POLL_ADDR, and its rptr report
// (ring->0xb8) in RB_RPTR_ADDR. had those two swapped; the GFX ring is the
// control (gfxstate prints +0xb8 rptr report and +0xd0 wptr wb, 8 bytes apart in
// the same 64-byte channel frame), and AMDRTRing::allocateMemoryResources
// @0xbe1b9c9/@0xbe1ba3a stores frame+0x10 into 0xd0 and frame+0x08 into 0xb8,
// which is the same relationship from the other side.
//
// Passing 0 for either address falls back to our own external slot, so a ring
// whose rptr report Apple never allocated is still programmable.
kern_return_t sdma_program_external_queue_ex(const DeviceContext &dev,
                                             SDMAInstance &inst,
                                             uint64_t ring_gpu_va,
                                             uint32_t ring_size_dwords,
                                             uint32_t doorbell_index,
                                             uint64_t rptr_addr,
                                             uint64_t wptr_poll_addr,
                                             bool enable_ring,
                                             uint32_t queue = 1);

// THE ESCAPE HATCH. Clear RB_ENABLE + IB_ENABLE on QUEUE1 and nothing else: one
// register pair, after which the engine fetches nothing from that queue. QUEUE0
// is not touched, so the live ring keeps running.
kern_return_t sdma_disable_external_queue(const DeviceContext &dev,
                                          const SDMAInstance &inst,
                                          uint32_t queue = 1);

// Everything the QUEUE1 block reads back, for `sdmastate` and for the gate the
// takeover applies before it sets RB_ENABLE=1.
struct SDMAQueue1Regs {
    uint32_t rb_cntl, rb_base, rb_base_hi, rb_rptr, rb_rptr_hi;
    uint32_t rb_wptr, rb_wptr_hi, rb_rptr_addr_lo, rb_rptr_addr_hi;
    uint32_t ib_cntl, doorbell, doorbell_offset;
    uint32_t wptr_poll_lo, wptr_poll_hi, minor_ptr_update;
};
void sdma_read_external_queue_regs(const DeviceContext &dev,
                                   const SDMAInstance &inst,
                                   SDMAQueue1Regs *out,
                                   uint32_t queue = 1);

// Ring QUEUE1's doorbell and NOTHING ELSE — no MMIO RB_WPTR fallback.
//
// sdma_kick_external_doorbell writes the doorbell AND the RB_WPTR register, so a
// queue that advanced after it cannot tell you whether the doorbell routed. The
// routing question ("does BAR2 dword 0x202 reach SDMA0 QUEUE1?") can only be
// answered by a kick that has no second path, which is what this is.
kern_return_t sdma_q1_kick_doorbell_only(const DeviceContext &dev,
                                         const SDMAInstance &inst,
                                         uint32_t wptrDwords,
                                         uint32_t doorbellIndex,
                                         uint32_t queue = 1);

// The MMIO half on its own — the control leg of the self-test. If the fence
// lands here but not after the doorbell-only kick, the queue is fine and the
// doorbell is the thing that does not route.
kern_return_t sdma_q1_set_wptr_mmio(const DeviceContext &dev,
                                    const SDMAInstance &inst,
                                    uint32_t wptrDwords,
                                    uint32_t queue = 1);

// Result of the boot-time QUEUE1 routing self-test (navi48-sdma-q1-test=1).
struct SDMAQ1TestResult {
    bool     ran            { false };
    bool     doorbell_ok    { false };  // the doorbell-only kick fenced
    bool     mmio_ok        { false };  // the MMIO RB_WPTR write fenced
    uint32_t doorbell_index { 0 };
    uint32_t instance       { 0 };      // 0.0.196: which (instance, queue) was tested
    uint32_t queue          { 1 };
    uint64_t ring_gpu_va    { 0 };
    uint32_t ring_dwords    { 0 };
    uint64_t fence_gpu      { 0 };
    uint32_t fence_last     { 0 };      // what the fence slot actually read
    uint64_t doorbell_us    { 0 };
    uint64_t mmio_us        { 0 };
    uint32_t rb_cntl        { 0 };
    uint32_t rb_rptr        { 0 };
    uint32_t rb_wptr        { 0 };
};

// SDMA0 QUEUE1 routing self-test — a 4 KiB ring of our own, ONE FENCE packet,
// a doorbell-only kick at `doorbell_index`, a bounded poll, and then (only if
// that failed) the MMIO control leg. QUEUE1 is left DISABLED and the scratch
// ring freed whichever way it goes, so the machine ends where it started.
//
// This exists so that the SDMA takeover never has to answer two questions at
// once. Rule 27: an instrument must be made of parts already proven on this
// silicon — the FENCE packet and the poll are stage 15's, and the only new thing
// under test is the QUEUE1 register block plus the doorbell index.
kern_return_t sdma_q1_selftest(DeviceContext &dev, GMCContext &gmc,
                               SDMAInstance &inst, uint32_t doorbell_index,
                               SDMAQ1TestResult *out, uint32_t queue = 1);

//------------------------------------------------------------------
// 0.0.191 — SRBM_WRITE self-test (navi48-srbm-test=1).
//
// ONE question: does an SDMA_OP_SRBM_WRITE packet from OUR SDMA0 QUEUE0 change
// a register on this card, (i) directly in the ring and (ii) from an
// INDIRECT_BUFFER, to (a) a free scratch register and (b) the register Apple
// programs and we measured as never written, GCVM_CONTEXT2_PAGE_TABLE_BASE_-
// ADDR_LO32 (r18: reads 0 after Apple's SDMA IB fenced).
//
// It also settles the operand encoding, which the project's own notes
// contradict themselves on:
//   • APPLE form  — header 0xf000000e (op 0x0e, byte-enable nibble 0xf) and the
//     register's absolute DWORD index as dword 1.'s decode of Apple's IB
//     (0x28f3 -> GC base[0] 0x1260 + 0x1693 = GCVM_CONTEXT2 PT base LO, with
//     five more offsets all resolving to the right GCVM registers) is what our
//     `xlatregs` hook writes today.
//   • LINUX form — header 0x0000000e and `reg << 2`, a BYTE offset, verbatim
//     from sdma_v7_0_ring_emit_wreg (ref/linux-amdgpu/sdma_v7_0.c:1203-1212,
//     whose own comment says "SRBM WRITE command will not support on sdma v7.
//     Use Register WRITE command instead, which OPCODE is same as SRBM WRITE").
// If the engine wants the Linux form, Apple's 0x28f3 is read as byte offset
// 0x28f3 -> a different register entirely, which alone explains.
//
// Every leg restores the register through MMIO afterwards, the scratch register
// is checked MMIO-writable FIRST (an unwritable register would make every leg
// read "FAILED" for the wrong reason), and the alternate-encoding leg for a
// target runs ONLY when the first one did not land — so no more stray writes
// than the answer needs. Default off; no register is touched when the boot-arg
// is absent.
//------------------------------------------------------------------
struct SDMASRBMTargetResult {
    uint32_t reg_dw   { 0 };     // absolute BAR5 dword index under test
    uint32_t value    { 0 };     // the value the packet writes
    uint32_t before   { 0 };     // MMIO read before anything was done
    bool     mmio_ok  { false }; // an MMIO write of `value` read back as `value`
    // Legs, in run order:
    //   0 ring / Apple form     1 ring / Linux form
    //   2 IB   / Apple form     3 IB   / Linux form
    //   4 IB   / Apple SRBM form but the PLAIN Linux IB header (0x00000004),
    //     run only to isolate IB-header bit 31 when the ring leg landed and
    //     both IB legs did not.
    bool     tried   [5] {};
    bool     fenced  [5] {};     // the FENCE after the packet materialised
    bool     landed  [5] {};     // fenced AND the MMIO read-back == value
    uint32_t readback[5] {};
};

struct SDMASRBMTestResult {
    bool     ran        { false };
    uint64_t ib_gpu_va  { 0 };   // MC address of the 4 KiB IB page
    SDMASRBMTargetResult scratch {};  // regSCRATCH_REG7 (GC base[1] + 0x2047)
    SDMASRBMTargetResult ctx2    {};  // GC base[0] + 0x1693
};

kern_return_t sdma_srbm_selftest(DeviceContext &dev, GMCContext &gmc,
                                 SDMAInstance &inst,
                                 SDMASRBMTestResult *out);

// 0.0.193 — COPY_LINEAR executed from an INDIRECT_BUFFER under `vmid`, so `src`
// and `dst` are VIRTUAL addresses walked through that VMID's page table. The
// IB's 8 dwords are staged into `ib_vram_off` through BAR0 and the IB is
// submitted from the ring at `ib_gpu_va` (32-byte aligned, MC). The FENCE stays
// in the ring (VMID 0) so a broken context reads as "the copy did not happen",
// not "the engine vanished". `tag` only distinguishes fence values in the log.
kern_return_t sdma_ib_copy_linear_test(const DeviceContext &dev,
                                       SDMAInstance &inst,
                                       uint64_t ib_gpu_va, uint64_t ib_vram_off,
                                       uint64_t src, uint64_t dst,
                                       uint32_t byte_count, uint32_t vmid,
                                       uint32_t tag, uint64_t timeout_us);

// Kick the EXTERNAL (QUEUE1) doorbell with a wptr somebody else owns.
//
// QUEUE1 needs its own: inst.wptr and inst.doorbell_index belong to our live
// QUEUE0 ring, so calling sdma_kick_doorbell here would ring the wrong doorbell
// with the wrong pointer. Mirrors it otherwise -- wptr shadow at
// kSDMAWBExtWptrOffset, HDP flush, WDOORBELL64 at doorbell_index * 4 (dword
// indexed; the reference's *8 was wrong), then the MMIO RB_WPTR fallback against
// the QUEUE1 register block.
//
// THIS IS THE CALL THAT MAKES THE GPU FETCH. It must not be reachable until
// RB_ENABLE=1 is deliberately set, and not before every address in the ring is
// proven resident.
kern_return_t sdma_kick_external_doorbell(const DeviceContext &dev,
                                          const SDMAInstance &inst,
                                          uint32_t wptrDwords,
                                          uint32_t doorbellIndex);

// Read back the QUEUE1 block for verification/logging. Reads only.
void sdma_log_external_queue_regs(const DeviceContext &dev,
                                  const SDMAInstance &inst, const char *tag,
                                  uint32_t queue = 1);

// QUEUE1's live RB_RPTR (BYTE domain), plus the rptr writeback dword when
// wb_rptr_out is non-null. Two reads and no writes; the queue is not disturbed.
//: Apple's AMDRTRing::getHead reads MMIO 0x193b, which our register shadow
// answers with the value Apple WROTE (always 0) rather than the value the engine
// has reached -- so Apple believes its ring has consumed nothing while the real
// QUEUE1 register reads RPTR == WPTR == 0xa00. This is the honest source.
uint32_t sdma_read_external_rptr(const DeviceContext &dev,
                                 const SDMAInstance &inst,
                                 uint32_t *wb_rptr_out);

kern_return_t sdma_kick_doorbell(const DeviceContext &dev,
                                 const SDMAInstance &inst);

// Append dwords to the ring at the current software wptr; wraps.
// Returns the number of dwords actually written (0 on overflow).
uint32_t sdma_ring_write(const DeviceContext &dev, SDMAInstance &inst,
                         const uint32_t *src, uint32_t dwords);

// Writeback-page accessors (deviation D6) — hide sysmem-vs-VRAM.
uint32_t sdma_wb_read32(const DeviceContext &dev, const SDMAInstance &inst,
                        uint32_t byte_offset);
void     sdma_wb_write32(const DeviceContext &dev, const SDMAInstance &inst,
                         uint32_t byte_offset, uint32_t value);

// Submit an SDMA COPY_LINEAR + FENCE pair, kick doorbell, poll fence.
// src/dst are GPU-visible addresses (VRAM MC addresses, or GART MC
// addresses for bound sysmem). byte_count must be ≤
// kSDMACopyLinearMaxBytes.
// 0.0.339: `cpv` selects the packet's HEADER_CPV bit. CPV=1 (the
// default, and what every pre-existing caller gets byte-for-byte) declares
// "compression parameters valid" and APPENDS an 8th dword; CPV=0 emits the
// 7-dword packet with no compression dword at all. It exists so the sweep can
// run the same copy both ways in one boot.
kern_return_t sdma_copy_linear_test(const DeviceContext &dev,
                                    SDMAInstance &inst,
                                    uint64_t src_bus, uint64_t dst_bus,
                                    uint32_t byte_count,
                                    uint64_t timeout_us,
                                    bool cpv = true);

// Submit an SDMA CONSTANT_FILL + FENCE pair, kick the doorbell, poll the
// fence. `dst` is whatever GPU-visible address the caller wants tested —
// that is the point: the PDB0 self-test (amdgpu_init.cpp) runs the SAME fill
// to a 0-based VRAM address, to the MC address of the same page, and to a
// GART sysmem page, and the only difference between the three is this
// argument. byte_count must be a multiple of 4 and <= 1 MiB.
kern_return_t sdma_const_fill_test(const DeviceContext &dev,
                                   SDMAInstance &inst,
                                   uint64_t dst, uint32_t pattern,
                                   uint32_t byte_count, uint64_t timeout_us);

// End-to-end ring test: emit FENCE, poll the fence word.
// Returns kIOReturnSuccess if the fence materialised, else the
// last-observed return code.
kern_return_t sdma_ring_test(const DeviceContext &dev,
                             SDMAInstance &inst,
                             uint64_t timeout_us);

// Read + log per-instance SDMA_STATUS_REG / RB_CNTL / rptr / wptr.
void sdma_log_status(const DeviceContext &dev, uint32_t inst);

// ---- Navi48Bringup additions -------------------------------------

// PSP-load the SDMA microcode (deviation D2). `bin`/`size` are the raw
// bytes of sdma_7_0_1.bin (sdma_firmware_header_v3_0 container); the
// payload is sliced by amdgpu_ucode_extract and submitted as
// GFX_FW_TYPE_SDMA_UCODE_TH0 (71). Sets sdma.microcode_loaded on
// success. Idempotent.
kern_return_t sdma_load_microcode(DeviceContext &dev, PSPContext &psp,
                                  SDMAContext &sdma,
                                  const void *bin, uint32_t size);

// Result of the VRAM→VRAM copy self-test (deviation D3).
struct SDMAVRAMCopyResult {
    kern_return_t kr;            // fence result from sdma_copy_linear_test
    uint32_t      bytes;         // bytes copied
    uint32_t      dwords;        // dwords compared
    uint32_t      mismatched;    // dwords that did NOT match the pattern
    uint32_t      first_bad_off; // byte offset of the first mismatch
    uint64_t      src_gpu_va;
    uint64_t      dst_gpu_va;
    uint64_t      elapsed_us;
};

// VRAM→VRAM SDMA copy smoke test — the "GPU wrote memory" milestone.
// Allocates src+dst from the VRAM allocator, stages an incrementing
// pattern through BAR0, poisons dst, runs COPY_LINEAR+FENCE, then
// verifies dst through MM_INDEX/MM_DATA (bus-aperture independent).
// Returns kIOReturnSuccess only when the fence landed AND every dword
// matched. `bytes` is clamped to [4, 64 KiB] and rounded down to a
// dword. Both VRAM slots are released before returning.
kern_return_t sdma_vram_copy_test(DeviceContext &dev, GMCContext &gmc,
                                  SDMAInstance &inst, uint32_t bytes,
                                  SDMAVRAMCopyResult *out);

// 0.0.339 — the SWEEP. sdma_vram_copy_test PASSES at 4 KiB in
// every boot in which the scanout pre-flight's 64 KiB copy fails, so the fault
// is in SIZE, in the ADDRESS PAIR, or in the packet header — not in
// COPY_LINEAR. This walks all three axes inside ONE 256 KiB VRAM region (so an
// address is never confounded with an allocator decision), with a pattern that
// is unique per dword, and logs the MAP — for every mismatch, the source dword
// the value it found actually belongs to. Allocates and frees exactly one
// region, writes nothing outside it, and touches no register this file does
// not already touch. Returns kIOReturnSuccess when every case matched.
kern_return_t sdma_vram_copy_sweep(DeviceContext &dev, GMCContext &gmc,
                                   SDMAInstance &inst);

// Top-level SDMAInit stage entry. Resolves IP base, asks PSP to
// load the SDMA0/SDMA1 microcode, allocates per-instance storage,
// runs gfx_resume on each. Idempotent.
kern_return_t sdma_init_full(DeviceContext &dev,
                             PSPContext &psp,
                             GMCContext &gmc,
                             SDMAContext &sdma);

} // namespace amdgpu
