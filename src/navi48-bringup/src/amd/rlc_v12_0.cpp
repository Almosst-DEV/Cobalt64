//
//  rlc_v12_0.cpp — RLC clear-state alloc + autoload wait for GFX12.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT), commit 3bdeed2,
//  dext/amdgpu/rlc_v12_0.cpp (arm64 DriverKit dext) onto the
//  Navi48Bringup x86 kernel kext compatibility layer in src/amd/.
//
//  Copyright (c) the mac-amdgpu authors. MIT licence — see ../../NOTICE.
//
//  Upstream sources (via the reference):
//    drivers/gpu/drm/amd/amdgpu/amdgpu_rlc.c:amdgpu_gfx_rlc_init_csb
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_wait_for_rlc_autoload_complete
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_get_csb_size
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_get_csb_buffer
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_init_csb
//    drivers/gpu/drm/amd/amdgpu/gfx_v12_0.c:gfx_v12_0_rlc_enable_srm
//    drivers/gpu/drm/amd/amdgpu/clearstate_gfx12.h (vendored below).
//
//  RLC firmware is NOT loaded here: on this card the PSP secure OS pulls
//  the RLC sub-bins in (LOAD_IP_FW) and the IMU/PSP autoload state machine
//  starts RLC autonomously after GFX_CMD_ID_AUTOLOAD_RLC. This module only
//  waits for that to finish, then programs CSB + SRM — exactly as the
//  reference does.
//
//  ================= Deviations from reference =========================
//
//  1. Platform shims: os_log(OS_LOG_DEFAULT, "mac.amdgpu.rlc: " …) ->
//     IOLog via RLC_LOG (amdgpu_log.h); <DriverKit/IOLib.h> ->
//     <IOKit/IOLib.h>. No %{public}s existed in this file. uint64_t log
//     arguments carry explicit (unsigned long long) casts (PORTING.md).
//
//  2. CSB staging buffer. The reference builds the CSB in a 4 KB stack
//     array (`uint32_t cpu[1024]`) inside rlc_setup_csb_buffer. A dext
//     thread has a userspace-sized stack; an x86_64 kernel stack is
//     16 KB, so 4 KB of it for a scratch buffer is not acceptable here.
//     The staging buffer is a file-static fixed array instead (PORTING.md
//     rule 3 allows fixed arrays; no dynamic C++ allocation). Contents,
//     size math and the bar0_memcpy_to_vram write are unchanged. Bring-up
//     stages run one at a time from a single thread, so the shared static
//     has no concurrent user.
//
//  3. The reference's `#ifdef __APPLE__` around the VRAM streaming is
//     dropped — it is unconditionally true in a macOS kext.
//
//  4. Two silent-failure paths in the reference are turned into hard
//     errors, because on THIS machine they would corrupt the live display
//     instead of just failing:
//       (a) reference: if clear_state.gpu_va < gmc.vram_start it sets
//           csb_vram_byte_offset = 0. VRAM byte 0 here is the UEFI console
//           framebuffer the kernel is still drawing into (PORTING.md VRAM
//           layout), and RLC would then fetch its clear state from the
//           console. We refuse instead.
//       (b) bar0_memcpy_to_vram() drops the write silently when the range
//           leaves the mapped BAR0 aperture, which would leave RLC
//           pointed at uninitialised VRAM. We range-check first and fail.
//
//  5. Added logging only (no added writes, no registers the reference does
//     not already touch): before/after read-back for each of the four
//     registers written, the GC BASE_IDX 0/1 bases, a BAR0 read-back of
//     the CSB header through RBAR2_32, and the full 2x2 BOOTLOAD_STATUS
//     address probe (0x4E7C and 0x4C49 at BASE_IDX 0 and 1) so the first
//     hardware log settles the addressing question by itself.
//
//  6. NOT ported, because the reference does not have them: RLC safe-mode
//     enter/exit (regRLC_SAFE_MODE), the RLC stop/start/reset sequence
//     (regRLC_CNTL.RLC_ENABLE_F32) and GPM thread enable
//     (regRLC_GPM_THREAD_ENABLE). Those belong to upstream's *direct*
//     firmware-load path (gfx_v12_0_rlc_resume without PSP). On this card
//     RLC comes up through PSP+IMU autoload; the driver's only job is the
//     poll below. Writing RLC_CNTL / RLC_GPM_* behind a running,
//     PSP-owned RLC while the display is scanning out is exactly the kind
//     of un-referenced GC write this port is not allowed to make. The
//     only GC registers this file touches are CP_STAT and
//     RLC_RLCS_BOOTLOAD_STATUS (reads), RLC_CSIB_ADDR_HI/_LO/_LENGTH and
//     RLC_SRM_CNTL (writes) — the same four the reference writes.
//
//  7. RLCContext gained default member initializers in amdgpu_rlc.h (see
//     the banner there).
//
//  =====================================================================
//

#include <stdint.h>
#include <string.h>
#include <IOKit/IOLib.h>

#include "amdgpu_rlc.h"
#include "amdgpu_gmc.h"   // GMCContext / VRAMBumpAllocator
#include "amdgpu_log.h"   // RLC_LOG

namespace amdgpu {

//------------------------------------------------------------------
// Vendored gfx12_cs_data — direct port of upstream
// drivers/gpu/drm/amd/amdgpu/clearstate_gfx12.h:26-119.
//
// All initial values are 0 (matching upstream); we keep the named
// arrays separate so the size + register-index math matches
// upstream's get_csb_size / get_csb_buffer behaviour exactly.
//------------------------------------------------------------------
namespace {

// clearstate_gfx12.h:26-61  — 34 entries (mmPA_SC_VPORT_*).
const uint32_t kGfx12_SECT_CONTEXT_def_1[] = {
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0, 0,0,0,0,0,0,0,0,
    0,0
};
// clearstate_gfx12.h:63-66  — 2 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_2[] = { 0, 0 };
// clearstate_gfx12.h:68-70  — 1 entry.
const uint32_t kGfx12_SECT_CONTEXT_def_3[] = { 0 };
// clearstate_gfx12.h:72-79  — 6 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_4[] = { 0, 0, 0, 0, 0, 0 };
// clearstate_gfx12.h:81-93  — 11 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_5[] = { 0,0,0,0,0,0,0,0,0,0,0 };
// clearstate_gfx12.h:95-104 — 8 entries.
const uint32_t kGfx12_SECT_CONTEXT_def_6[] = { 0, 0, 0, 0, 0, 0, 0, 0 };

// clearstate_gfx12.h:106-114 — extent table. {extent, reg_index, reg_count}.
const CSExtentDef kGfx12_SECT_CONTEXT_defs[] = {
    { kGfx12_SECT_CONTEXT_def_1, 0x0000a03e, 34 },
    { kGfx12_SECT_CONTEXT_def_2, 0x0000a0cc,  2 },
    { kGfx12_SECT_CONTEXT_def_3, 0x0000a0d8,  1 },
    { kGfx12_SECT_CONTEXT_def_4, 0x0000a0db,  6 },
    { kGfx12_SECT_CONTEXT_def_5, 0x0000a2e5, 11 },
    { kGfx12_SECT_CONTEXT_def_6, 0x0000a3c0,  8 },
    { nullptr,                   0,           0 }   // terminator
};

// clearstate_gfx12.h:116-119 — section table.
const CSSectionDef kGfx12_cs_data[] = {
    { kGfx12_SECT_CONTEXT_defs, kCSSection_CONTEXT },
    { nullptr,                  kCSSection_NONE    }
};

// Deviation 2: CPU-side staging buffer for the CSB content. The
// reference keeps this on the stack; a 16 KB kernel stack cannot spare
// 4 KB. gfx12_cs_data needs 75 dwords (300 B) today.
constexpr uint32_t kMaxCsbDwords = 1024;   // 4 KB — plenty for gfx12
uint32_t g_csb_staging[kMaxCsbDwords];

} // anonymous namespace

//------------------------------------------------------------------
// rlc_get_csb_size — port of gfx_v12_0_get_csb_size (gfx_v12_0.c:669).
//
// Counts dwords needed to encode the full clear-state buffer:
//   1 (clustercount header)
// + per extent: 2 (reg_count + reg_index) + reg_count * dword
//------------------------------------------------------------------
static uint32_t
rlc_get_csb_size(const CSSectionDef *cs_data)
{
    uint32_t count = 1;
    for (const CSSectionDef *sect = cs_data; sect->section != nullptr; ++sect) {
        if (sect->id == kCSSection_CONTEXT) {
            for (const CSExtentDef *ext = sect->section;
                 ext->extent != nullptr; ++ext) {
                count += 2 + ext->reg_count;
            }
        } else {
            return 0;
        }
    }
    return count;
}

//------------------------------------------------------------------
// rlc_get_csb_buffer — port of gfx_v12_0_get_csb_buffer
// (gfx_v12_0.c:688). Fills `buffer` with the CSB layout RLC expects:
//
//     buffer[0]            = clustercount
//     for each extent:
//         buffer[count++]  = reg_count
//         buffer[count++]  = reg_index
//         buffer[count..]  = extent initial values
//
// `buffer` must point to at least rlc_get_csb_size dwords.
//------------------------------------------------------------------
static void
rlc_get_csb_buffer(const CSSectionDef *cs_data, uint32_t *buffer)
{
    if (cs_data == nullptr || buffer == nullptr) return;

    uint32_t count = 1;
    uint32_t clustercount = 0;
    for (const CSSectionDef *sect = cs_data; sect->section != nullptr; ++sect) {
        if (sect->id == kCSSection_CONTEXT) {
            for (const CSExtentDef *ext = sect->section;
                 ext->extent != nullptr; ++ext) {
                clustercount++;
                buffer[count++] = ext->reg_count;
                buffer[count++] = ext->reg_index;
                for (uint32_t i = 0; i < ext->reg_count; i++) {
                    buffer[count++] = ext->extent[i];
                }
            }
        } else {
            return;
        }
    }
    buffer[0] = clustercount;
}

kern_return_t
rlc_alloc_csb(GMCContext &gmc, RLCContext &rlc)
{
    if (rlc.inited && rlc.clear_state.gpu_va != 0) return kIOReturnSuccess;
    if (!gmc.vram_alloc.is_inited()) return kIOReturnNotReady;

    // Pre-compute the CSB size from the (now-vendored) cs_data table
    // so we don't over-allocate. Upstream amdgpu_gfx_rlc_init_csb
    // (amdgpu_rlc.c:128) does the same — it queries
    // get_csb_size, multiplies by 4, and allocates that many bytes.
    rlc.clear_state_dwords = rlc_get_csb_size(kGfx12_cs_data);
    const uint64_t csb_bytes_needed =
        static_cast<uint64_t>(rlc.clear_state_dwords) * 4u;
    const uint64_t alloc_bytes =
        (csb_bytes_needed < kRLCClearStateDefaultBytes)
            ? kRLCClearStateDefaultBytes  // pad up to page granularity
            : ((csb_bytes_needed + kASPageSize - 1) & ~(kASPageSize - 1));

    if (!gmc.vram_alloc.alloc(alloc_bytes, kASPageSize, &rlc.clear_state)) {
        RLC_LOG("CSB VRAM alloc failed (need %llu bytes, free=%llu)",
                static_cast<unsigned long long>(alloc_bytes),
                static_cast<unsigned long long>(gmc.vram_alloc.bytes_free()));
        return kIOReturnNoMemory;
    }

    // Store the VRAM byte offset (relative to vram_start) so
    // rlc_setup_csb_buffer can stream dwords via the BAR0 aperture.
    // Deviation 4(a): the reference falls back to offset 0 here; VRAM
    // byte 0 on this machine is the live UEFI console framebuffer, so a
    // gpu_va below vram_start is a hard error instead.
    if (rlc.clear_state.gpu_va >= gmc.vram_start) {
        rlc.csb_vram_byte_offset = rlc.clear_state.gpu_va - gmc.vram_start;
    } else {
        RLC_LOG("CSB gpu_va %#llx is below vram_start %#llx — refusing "
                "(the reference's offset-0 fallback would put the CSB in "
                "the live console framebuffer)",
                static_cast<unsigned long long>(rlc.clear_state.gpu_va),
                static_cast<unsigned long long>(gmc.vram_start));
        gmc.vram_alloc.free(rlc.clear_state);
        rlc.clear_state = VRAMAllocation{};
        return kIOReturnInternalError;
    }

    RLC_LOG("CSB alloc: gpu_va=%#llx size=%llu (csb_dwords=%u) "
            "vram_byte_offset=%#llx",
            static_cast<unsigned long long>(rlc.clear_state.gpu_va),
            static_cast<unsigned long long>(rlc.clear_state.size),
            rlc.clear_state_dwords,
            static_cast<unsigned long long>(rlc.csb_vram_byte_offset));
    rlc.inited = true;
    return kIOReturnSuccess;
}

kern_return_t
rlc_wait_for_autoload_complete(const DeviceContext &dev, RLCContext &rlc)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        RLC_LOG("GC IP base not resolved");
        return kIOReturnNotReady;
    }

    // CP_STAT lives at GC BASE_IDX 0 (gc_12_0_0_offset.h:2131).
    // RLC_RLCS_BOOTLOAD_STATUS lives at GC BASE_IDX 1 (line 6862).
    const uint32_t cp_stat_reg =
        RLC_GC_REG(dev, CP_STAT);
    const uint32_t bootload_reg =
        RLC_GC_REG(dev, RLC_RLCS_BOOTLOAD_STATUS);

    // Log initial register state for triage.
    {
        uint32_t init_cp = RREG32(dev, cp_stat_reg);
        uint32_t init_bs = RREG32(dev, bootload_reg);
        // Cross-checks to verify BAR5 GC base resolution:
        //   GRBM_STATUS at GC[0] + 0x0DA4
        //   RLC_CGCG_CGLS_CTRL at GC[1] + 0x4C49
        //   BOOTLOAD_STATUS read at GC[0] + 0x4E7C (the OLD wrong address)
        const uint32_t grbm_reg =
            RLC_GC_REG(dev, GRBM_STATUS);
        const uint32_t cgcg_reg =
            RLC_GC_REG(dev, RLC_CGCG_CGLS_CTRL);
        // Deliberate wrong-segment read (BASE_IDX 0 instead of the
        // header's 1) — read-only, kept as the regression witness.
        const uint32_t bootload_b0 =
            SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0,
                                  GCRegs::RLC_RLCS_BOOTLOAD_STATUS);
        uint32_t grbm = RREG32(dev, grbm_reg);
        uint32_t cgcg = RREG32(dev, cgcg_reg);
        uint32_t bs_b0 = RREG32(dev, bootload_b0);
        RLC_LOG("autoload poll start: CP_STAT[GC0]=%#010x(@%#x) "
                "BOOTLOAD_STATUS[GC1]=%#010x(@%#x) "
                "BOOTLOAD_STATUS[GC0(wrong)]=%#010x(@%#x) "
                "GRBM_STATUS[GC0]=%#010x(@%#x) "
                "CGCG_CGLS_CTRL[GC1]=%#010x(@%#x)",
                init_cp, cp_stat_reg,
                init_bs, bootload_reg,
                bs_b0, bootload_b0,
                grbm, grbm_reg,
                cgcg, cgcg_reg);

        // Deviation 5: complete the 2x2 address/segment matrix so the
        // first hardware log settles which address really is
        // BOOTLOAD_STATUS. 0x4C49 is regRLC_CGCG_CGLS_CTRL, NOT an
        // alternative BOOTLOAD_STATUS — it is here only because GC[1]
        // resolution is the thing under test. Reference values from the
        // reference's own proven R9700 run (mac-amdgpu README v0.1.22):
        //   BOOTLOAD_STATUS[GC1]=0x8000003f  GRBM_STATUS[GC0]=0x382c
        //   CGCG_CGLS_CTRL[GC1]=0x0001003c
        // Deliberate wrong-segment read, as above.
        const uint32_t cgcg_b0 =
            SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC, 0,
                                  GCRegs::RLC_CGCG_CGLS_CTRL);
        RLC_LOG("autoload addr probe: GC base[0]=%#x base[1]=%#x | "
                "0x4E7C@GC0=%#010x 0x4E7C@GC1=%#010x "
                "0x4C49@GC0=%#010x 0x4C49@GC1=%#010x | "
                "proven R9700: 0x4E7C@GC1=0x8000003f 0x4C49@GC1=0x0001003c",
                dev.ip.getBase(IPBlock::GC, 0), dev.ip.getBase(IPBlock::GC, 1),
                bs_b0, init_bs,
                RREG32(dev, cgcg_b0), cgcg);
    }

    // 10-second budget — Apple Silicon cold-boot autoload can be slow.
    const uint64_t kBudgetUs = 10 * 1000000;
    uint64_t elapsed = 0;
    uint32_t cp_stat = 0xFFFFFFFFu, bs = 0;
    while (elapsed < kBudgetUs) {
        cp_stat = RREG32(dev, cp_stat_reg);
        bs      = RREG32(dev, bootload_reg);
        if (cp_stat == 0 &&
            (bs & kRLC_RLCS_BOOTLOAD_STATUS__BOOTLOAD_COMPLETE_MASK)) {
            rlc.bootload_complete = true;
            RLC_LOG("RLC autoload complete (cp_stat=%#010x bootload=%#010x "
                    "after %llu us)", cp_stat, bs,
                    static_cast<unsigned long long>(elapsed));
            return kIOReturnSuccess;
        }
        IOSleep(1);
        elapsed += 1000;
    }
    RLC_LOG("RLC autoload timeout (cp_stat=%#010x bootload=%#010x "
            "after %llu ms) — SRM enabled=%u",
            cp_stat, bs,
            static_cast<unsigned long long>(elapsed / 1000),
            rlc.srm_enabled ? 1 : 0);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// rlc_setup_csb_buffer — fill the CSB content from gfx12_cs_data,
// then program RLC_CSIB_ADDR_HI/_LO + RLC_CSIB_LENGTH.
//
// Mirrors:
//   gfx_v12_0_get_csb_buffer    (gfx_v12_0.c:688) — fills the buffer
//   gfx_v12_0_init_csb          (gfx_v12_0.c:1905) — writes CSIB regs
//
// We can't kmap VRAM to memcpy into it — but the BAR0 aperture maps
// onto the visible VRAM window, so we stream dwords through it. The CSB
// lives inside the VRAM allocator's region, which is inside the
// BAR0-visible aperture.
//
// Audit-7 #7.
//------------------------------------------------------------------
kern_return_t
rlc_setup_csb_buffer(const DeviceContext &dev, GMCContext &gmc,
                     RLCContext &rlc)
{
    if (!rlc.inited || rlc.clear_state.gpu_va == 0) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    (void)gmc;

    if (!rlc.csb_populated) {
        // Build the CSB content into a small CPU-side buffer first,
        // then push it into VRAM via the BAR0 aperture.
        uint32_t *cpu = g_csb_staging;          // deviation 2
        memset(cpu, 0, sizeof(g_csb_staging));

        if (rlc.clear_state_dwords > kMaxCsbDwords) {
            RLC_LOG("CSB size %u exceeds staging buffer %u",
                    rlc.clear_state_dwords, kMaxCsbDwords);
            return kIOReturnNoMemory;
        }
        rlc_get_csb_buffer(kGfx12_cs_data, cpu);

        const uint64_t csb_bytes =
            static_cast<uint64_t>(rlc.clear_state_dwords) * 4u;

        // Deviation 4(b): bar0_memcpy_to_vram() silently does nothing if
        // the range leaves the mapped aperture. Programming CSIB after a
        // dropped write would point RLC at uninitialised VRAM, so check.
        if (rlc.csb_vram_byte_offset + csb_bytes > dev.bar0Size) {
            RLC_LOG("CSB at vram+%#llx (+%llu B) is outside the mapped "
                    "BAR0 aperture (%llu bytes) — refusing; the copy "
                    "would be dropped and RLC would fetch garbage",
                    static_cast<unsigned long long>(rlc.csb_vram_byte_offset),
                    static_cast<unsigned long long>(csb_bytes),
                    static_cast<unsigned long long>(dev.bar0Size));
            return kIOReturnNoMemory;
        }

        // Stream into VRAM via BAR0 aperture writes.
        bar0_memcpy_to_vram(dev, rlc.csb_vram_byte_offset, cpu, csb_bytes);
        // Drain the HDP write cache so RLC sees the CSB content.
        amdgpu_hdp_flush(dev);

        rlc.csb_populated = true;

        // Log first cluster for triage: dword[0] = cluster count.
        RLC_LOG("CSB filled: clustercount=%u, total_dwords=%u (%llu bytes) "
                "-> vram+%#llx (MC %#llx)",
                cpu[0], rlc.clear_state_dwords,
                static_cast<unsigned long long>(csb_bytes),
                static_cast<unsigned long long>(rlc.csb_vram_byte_offset),
                static_cast<unsigned long long>(rlc.clear_state.gpu_va));

        // Deviation 5: read the CSB header back through the aperture.
        // Expect 6 clusters / first extent reg_count 34 / reg_index
        // 0xa03e — anything else means the BAR0 write path is wrong and
        // RLC is about to consume garbage.
        RLC_LOG("CSB readback: [0]=%u(clusters, want 6) [1]=%u(reg_count, "
                "want 34) [2]=%#x(reg_index, want 0xa03e) [3]=%#x",
                RBAR2_32(dev, rlc.csb_vram_byte_offset),
                RBAR2_32(dev, rlc.csb_vram_byte_offset + 4),
                RBAR2_32(dev, rlc.csb_vram_byte_offset + 8),
                RBAR2_32(dev, rlc.csb_vram_byte_offset + 12));
    }

    // Program RLC_CSIB_ADDR_HI/_LO/_LENGTH per gfx_v12_0_init_csb
    // (gfx_v12_0.c:1909-1913). All three live at GC BASE_IDX 1
    // (gc_12_0_0_offset.h:7030/7032/7034).
    const uint64_t csb_gpu_va = rlc.clear_state.gpu_va;

    const uint32_t reg_csib_hi =
        RLC_GC_REG(dev, RLC_CSIB_ADDR_HI);
    const uint32_t val_csib_hi = static_cast<uint32_t>(csb_gpu_va >> 32);
    const uint32_t pre_csib_hi = RREG32(dev, reg_csib_hi);
    WREG32(dev, reg_csib_hi, val_csib_hi);
    RLC_LOG("RLC_CSIB_ADDR_HI @%#x: %#010x -> wrote %#010x -> reads %#010x",
            reg_csib_hi, pre_csib_hi, val_csib_hi, RREG32(dev, reg_csib_hi));

    const uint32_t reg_csib_lo =
        RLC_GC_REG(dev, RLC_CSIB_ADDR_LO);
    const uint32_t val_csib_lo =
        static_cast<uint32_t>(csb_gpu_va & 0xFFFFFFFCu);
    const uint32_t pre_csib_lo = RREG32(dev, reg_csib_lo);
    WREG32(dev, reg_csib_lo, val_csib_lo);
    RLC_LOG("RLC_CSIB_ADDR_LO @%#x: %#010x -> wrote %#010x -> reads %#010x",
            reg_csib_lo, pre_csib_lo, val_csib_lo, RREG32(dev, reg_csib_lo));

    const uint32_t reg_csib_len =
        RLC_GC_REG(dev, RLC_CSIB_LENGTH);
    const uint32_t val_csib_len = rlc.clear_state_dwords;
    const uint32_t pre_csib_len = RREG32(dev, reg_csib_len);
    WREG32(dev, reg_csib_len, val_csib_len);
    RLC_LOG("RLC_CSIB_LENGTH @%#x: %#010x -> wrote %#010x -> reads %#010x",
            reg_csib_len, pre_csib_len, val_csib_len,
            RREG32(dev, reg_csib_len));

    rlc.csib_programmed = true;
    RLC_LOG("CSIB programmed: ADDR=%#llx LENGTH=%u",
            static_cast<unsigned long long>(csb_gpu_va),
            rlc.clear_state_dwords);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// rlc_enable_srm — port of gfx_v12_0_rlc_enable_srm (gfx_v12_0.c:1967).
//
// Audit-7 #7.
//------------------------------------------------------------------
kern_return_t
rlc_enable_srm(const DeviceContext &dev, RLCContext &rlc)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    // regRLC_SRM_CNTL is at GC BASE_IDX 1 (gc_12_0_0_offset.h:6467).
    const uint32_t reg =
        RLC_GC_REG(dev, RLC_SRM_CNTL);

    // gfx_v12_0.c:1972-1975 — read, OR-in AUTO_INCR + SRM_ENABLE, write back.
    uint32_t tmp = RREG32(dev, reg);
    const uint32_t pre = tmp;
    tmp |= RLC_SRM_CNTL__AUTO_INCR_ADDR_MASK;
    tmp |= RLC_SRM_CNTL__SRM_ENABLE_MASK;
    WREG32(dev, reg, tmp);

    rlc.srm_enabled = true;
    RLC_LOG("RLC_SRM_CNTL @%#x: %#010x -> wrote %#010x -> reads %#010x "
            "(SRM enabled, auto-incr addr)",
            reg, pre, tmp, RREG32(dev, reg));
    return kIOReturnSuccess;
}

kern_return_t
rlc_init_full(const DeviceContext &dev, GMCContext &gmc, RLCContext &rlc)
{
    kern_return_t r;

    // Guard: if PSP rejected the RLC sub-bin LOAD_IP_FW frames we
    // CANNOT touch RLC registers — programming RLC_CSIB/RLC_SRM_CNTL
    // against a firmware-less RLC drops the GPU off the PCIe bus.
    if (!rlc.microcode_loaded) {
        RLC_LOG("init_full: RLC microcode not loaded into PSP "
                "(LOAD_IP_FW failed earlier) — refusing to touch "
                "RLC hardware; the link would drop");
        return kIOReturnNotReady;
    }

    RLC_LOG("init_full: GC base[0]=%#x base[1]=%#x vram_start=%#llx "
            "bar0=%llu MB",
            dev.ip.getBase(IPBlock::GC, 0), dev.ip.getBase(IPBlock::GC, 1),
            static_cast<unsigned long long>(gmc.vram_start),
            static_cast<unsigned long long>(dev.bar0Size >> 20));

    // Match upstream gfx_v12_0_hw_init + gfx_v12_0_rlc_resume(PSP):
    //   1. wait_for_rlc_autoload_complete    (poll CP_STAT==0 + BOOTLOAD_COMPLETE)
    //   2. gfx_v12_0_init_csb                (fill CSB + program RLC_CSIB_*)
    //   3. gfx_v12_0_rlc_enable_srm          (RLC_SRM_CNTL AUTO_INCR + SRM_ENABLE)
    //
    // Upstream does NOT touch RLC_CGCG_CGLS_CTRL on the PSP path —
    // clock-gating is programmed later in set_clockgating_state with
    // proper field masking. The PSP+IMU autoload runs autonomously
    // after psp_rlc_autoload_start (GFX_CMD_ID_AUTOLOAD_RLC); the
    // driver's job is just to wait for completion, then set up CSB+SRM.

    // 1. Wait for PSP+IMU-driven autoload to finish bringing RLC up.
    r = rlc_wait_for_autoload_complete(dev, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("autoload wait failed: %#x", r);
        return r;
    }

    // 2. Allocate CSB in VRAM.
    r = rlc_alloc_csb(gmc, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("alloc_csb failed: %#x", r);
        return r;
    }

    // 3. Fill CSB content + program RLC_CSIB_ADDR_HI/_LO/_LENGTH
    //    (== upstream gfx_v12_0_init_csb).
    r = rlc_setup_csb_buffer(dev, gmc, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("setup_csb_buffer failed: %#x", r);
        return r;
    }

    // 4. Enable SRM + AUTO_INCR_ADDR (== upstream gfx_v12_0_rlc_enable_srm).
    r = rlc_enable_srm(dev, rlc);
    if (r != kIOReturnSuccess) {
        RLC_LOG("enable_srm failed: %#x", r);
        return r;
    }

    RLC_LOG("init_full: done — bootload_complete=%u csb_populated=%u "
            "csib_programmed=%u srm_enabled=%u",
            rlc.bootload_complete ? 1 : 0, rlc.csb_populated ? 1 : 0,
            rlc.csib_programmed ? 1 : 0, rlc.srm_enabled ? 1 : 0);
    return kIOReturnSuccess;
}

} // namespace amdgpu
