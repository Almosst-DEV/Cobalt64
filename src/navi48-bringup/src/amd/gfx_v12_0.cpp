//
//  gfx_v12_0.cpp — GFX12 top-level configuration.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT), commit 3bdeed2,
//  dext/amdgpu/gfx_v12_0.cpp (arm64 DriverKit dext) onto the
//  Navi48Bringup x86 kernel kext compatibility layer in src/amd/.
//
//  Copyright (c) the mac-amdgpu authors. MIT licence — see ../../NOTICE.
//
//  Implements the gfx_v12_0_constants_init bringup helpers:
//      - gpu_early_init            (gfx_v12_0.c:1640)
//      - get_gb_addr_config        (gfx_v12_0.c:3575)
//      - setup_rb                  (gfx_v12_0.c:1731)
//      - get_sa_active_bitmap      (gfx_v12_0.c:1694)
//      - get_rb_active_bitmap      (gfx_v12_0.c:1712)
//      - select_se_sh              (gfx_v12_0.c:1667)
//      - get_cu_info               (gfx_v12_0.c:5737)
//      - get_wgp_active_bitmap_per_sh (gfx_v12_0.c:5704)
//      - get_cu_active_bitmap_per_sh  (gfx_v12_0.c:5719)
//      - init_compute_vmid         (gfx_v12_0.c:1766)
//      - constants_init            (gfx_v12_0.c:1806)
//  plus the MQD builders (gfx_v12_0.c:2969 / :3143).
//
//  Audit-7 #8 / #9.
//
//  ================= Deviations from reference =========================
//
//  1. Platform shims: os_log -> IOLog via GFX_LOG (amdgpu_log.h); no
//     %{public}s (none were present); no DriverKit includes. The
//     reference's `#include "amdgpu_mes.h"` is dropped — the four
//     kCP_*_DEFAULT constants it supplied now live in amdgpu_gfx.h.
//
//  2. Navi 48 caps: max_sh_per_se is 2, not the reference's 1.
//     The reference comment claims "4 SEs x 1 SA x 4 RBs/SE x 8 CUs/SA"
//     for R9700; that is 32 CUs, but R9700 and the RX 9070 XT are the
//     same Navi 48 die with 64 CUs (32 WGPs) = 4 SE x 2 SA x 4 WGP.
//     With max_sh_per_se = 1, get_cu_info only walks shader arrays
//     0..3 of 8 and under-reports CUs by 2x, and setup_rb builds an
//     RB bitmap from a 4-entry SA mask instead of 8. Everything else
//     about the harvest decode is unchanged — the *enabled* counts
//     still come only from the harvest registers, never assumed.
//
//  3. gpu_early_init split out of constants_init into its own function
//     (the reference inlines it) and given upstream's IP_VERSION(12,0,1)
//     sc_* FIFO constants, which the reference omits. No MMIO either way.
//
//  4. ADDED gfx_probe_gc_bases(): a read-only diagnostic + safety gate
//     that runs before the first register WRITE. The reference addressed
//     every GC register in this file at SOC15 BASE_IDX 0. We now have
//     gc_12_0_0_offset.h (ref/linux-asic-reg/, documentation only — never
//     compiled) and every register's BASE_IDX comes from its
//     reg<NAME>_BASE_IDX line, centralised in GFXRegBaseIdx
//     (amdgpu_gfx.h). GRBM_GFX_INDEX / GRBM_GFX_CNTL / SH_MEM_* and the
//     three GC_USER_* harvest registers are BASE_IDX 1, not 0.
//     The probe now reads GRBM_GFX_INDEX at its own (header) segment and
//     aborts constants_init — no writes at all — if THAT read returns
//     0xFFFFFFFF, because on this card the display is scanning out and a
//     GRBM_GFX_INDEX write landing on an unrelated GC register is not
//     something to do blind. The other segment is still read and logged
//     as a cross-check.
//
//  5. Logging: every harvest/config register value is logged with its
//     absolute dword address (PORTING.md rule 6 — on hardware the log
//     is all we get). The reference logs only summaries.
//
//  6. gfx_constants_init(dev) convenience overload uses a file-scope
//     `static GFXConfig` instead of a function-local static: kexts do
//     not reliably run C++ static constructors or guard variables, and
//     a file-scope POD lands in .bss zero-initialised.
//
//  7. No GMC/PSP state is referenced. Upstream's constants_init reads
//     gmc.private_aperture_start / shared_aperture_start for the
//     VMID >= 1 SH_MEM_BASES write; the reference already substitutes
//     the fixed RDNA4 default apertures (SHARED=0x1, PRIVATE=0x2) and
//     we keep that. If GMC code later exposes the real apertures,
//     they belong in GFXConfig as two new fields, not as an include.
//  =====================================================================
//

#include <string.h>
#include "amdgpu_gfx.h"
#include "amdgpu_log.h"

namespace amdgpu {

//------------------------------------------------------------------
// GC register addressing. Unlike the reference (which hardcodes
// SOC15 BASE_IDX 0 at every call site) each register carries its
// segment index from GFXRegBaseIdx — see amdgpu_gfx.h.
//------------------------------------------------------------------
#define GC_REG(dev, name) \
    SOC15_REG_OFFSET_BIDX((dev), IPBlock::GC, \
                          GFXRegBaseIdx::name, GFXRegs::name)

//------------------------------------------------------------------
// Helpers — bit math.
//------------------------------------------------------------------
static inline uint32_t
gfx_create_bitmask(uint32_t bit_width)
{
    // amdgpu_gfx.c:amdgpu_gfx_create_bitmask. Returns (1 << bw) - 1.
    if (bit_width >= 32) return 0xFFFFFFFFu;
    return (1u << bit_width) - 1u;
}

static inline uint32_t
gfx_hweight32(uint32_t v)
{
    // Standard popcount.
    v = v - ((v >> 1) & 0x55555555u);
    v = (v & 0x33333333u) + ((v >> 2) & 0x33333333u);
    v = (v + (v >> 4)) & 0x0F0F0F0Fu;
    return (v * 0x01010101u) >> 24;
}

//------------------------------------------------------------------
// gfx_probe_gc_bases — NOT in the reference. Deviation #4.
//
// Read-only. Logs GRBM_GFX_INDEX at both GC segments (and GB_ADDR_CONFIG
// / GRBM_STATUS at their own header BASE_IDX) with absolute dword
// addresses, then gates the first write on GRBM_GFX_INDEX reading the
// reset pattern at ITS OWN header-declared BASE_IDX — which
// gc_12_0_0_offset.h says is 1, not 0.
//
// Expected on a VBIOS-posted, idle GPU:
//   GRBM_GFX_INDEX @GC[1] == 0xE0000000 (all three broadcast bits set)
//   GRBM_GFX_INDEX @GC[0] == 0xFFFFFFFF (nothing there — that segment's
//                            0x2200 is outside the decoded window)
//   GB_ADDR_CONFIG @GC[0] == 0x08200545, GRBM_STATUS @GC[0] == 0x382c
//
// The gate is now the positive test (does GRBM_GFX_INDEX decode where the
// header says it lives?) rather than the old negative one, which passed
// vacuously as long as *something* answered at GC[0]+0x2200.
//------------------------------------------------------------------
static kern_return_t
gfx_probe_gc_bases(const DeviceContext &dev, GFXConfig &cfg)
{
    // GRBM_GFX_INDEX at its own BASE_IDX (header: 1).
    const uint32_t idx_own = GC_REG(dev, GRBM_GFX_INDEX);
    const uint32_t v_idx_own = RREG32(dev, idx_own);
    // GB_ADDR_CONFIG and GRBM_STATUS at their own BASE_IDX (header: 0).
    const uint32_t gb_reg  = GC_REG(dev, GB_ADDR_CONFIG);
    const uint32_t v_gb    = RREG32(dev, gb_reg);
    const uint32_t st_reg  = SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
                                                   kGFXRegBaseIdx_GRBM_STATUS,
                                                   GFXRegsRO::GRBM_STATUS);
    const uint32_t v_st    = RREG32(dev, st_reg);

    GFX_LOG("probe: GC bases base[0]=%#x base[1]=%#x",
            dev.ip.getBase(IPBlock::GC, 0), dev.ip.getBase(IPBlock::GC, 1));
    GFX_LOG("probe: GRBM_GFX_INDEX (BASE_IDX %d) @%#x=%#010x  "
            "GB_ADDR_CONFIG (BASE_IDX %d) @%#x=%#010x  "
            "GRBM_STATUS (BASE_IDX %d) @%#x=%#010x",
            GFXRegBaseIdx::GRBM_GFX_INDEX, idx_own, v_idx_own,
            GFXRegBaseIdx::GB_ADDR_CONFIG, gb_reg, v_gb,
            kGFXRegBaseIdx_GRBM_STATUS, st_reg, v_st);

    // Cross-check read of GRBM_GFX_INDEX at the OTHER segment — pure
    // diagnostics, kept so the log still shows both (PORTING.md rule 6).
    if (dev.ip.isResolved(IPBlock::GC, GFXRegBaseIdx::kProbeAlternate)) {
        const uint32_t idx_alt = SOC15_REG_OFFSET_BIDX(
            dev, IPBlock::GC, GFXRegBaseIdx::kProbeAlternate,
            GFXRegs::GRBM_GFX_INDEX);
        GFX_LOG("probe: GRBM_GFX_INDEX cross-check at GC[%d] @%#x=%#010x "
                "(expected garbage — not this register's BASE_IDX)",
                GFXRegBaseIdx::kProbeAlternate, idx_alt,
                RREG32(dev, idx_alt));
    } else {
        GFX_LOG("probe: GC[%d] segment not resolved by discovery — "
                "no cross-check available", GFXRegBaseIdx::kProbeAlternate);
    }

    cfg.gc_bases_probed = true;

    // Safety gate (deviation #4): do not start writing GRBM state through
    // an address that does not decode. RREG32 returns 0xFFFFFFFF both for
    // an offset outside the mapped BAR5 window and for an unclaimed PCIe
    // read; neither is a legal GRBM_GFX_INDEX.
    if (v_idx_own == 0xFFFFFFFFu) {
        GFX_LOG("probe: GRBM_GFX_INDEX@%#x (GC[%d], the header's BASE_IDX) "
                "reads 0xFFFFFFFF — GC register window does not decode; "
                "refusing to write GRBM state",
                idx_own, GFXRegBaseIdx::GRBM_GFX_INDEX);
        return kIOReturnNoDevice;
    }
    if (v_idx_own != kGRBM_GFX_INDEX_BROADCAST_ALL) {
        // Not fatal — the GPU may legitimately have a non-reset selection
        // latched — but it is the single most useful line in the log if
        // the segment ever moves again, so say so loudly.
        GFX_LOG("probe: WARNING GRBM_GFX_INDEX@%#x=%#010x is not the "
                "broadcast reset pattern %#010x; continuing",
                idx_own, v_idx_own, kGRBM_GFX_INDEX_BROADCAST_ALL);
    }
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// gfx_select_se_sh — port of gfx_v12_0_select_se_sh (gfx_v12_0.c:1667).
//
// Writes GRBM_GFX_INDEX to scope subsequent reads to a particular
// (SE, SH, instance) tuple. 0xffffffff means broadcast.
//------------------------------------------------------------------
static void
gfx_select_se_sh(const DeviceContext &dev, uint32_t se_num,
                 uint32_t sh_num, uint32_t instance)
{
    uint32_t data = 0;

    if (instance == 0xFFFFFFFFu) {
        data = REG_SET_FIELD(0,    GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
    } else {
        data = REG_SET_FIELD(0,    GRBM_GFX_INDEX, INSTANCE_INDEX, instance);
    }
    if (se_num == 0xFFFFFFFFu) {
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1);
    } else {
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se_num);
    }
    if (sh_num == 0xFFFFFFFFu) {
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_BROADCAST_WRITES, 1);
    } else {
        data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_INDEX, sh_num);
    }

    WREG32(dev, GC_REG(dev, GRBM_GFX_INDEX), data);
}

//------------------------------------------------------------------
// gfx_get_sa_active_bitmap — port of gfx_v12_0_get_sa_active_bitmap
// (gfx_v12_0.c:1694). Combines CC + USER SA-disable masks.
//------------------------------------------------------------------
static uint32_t
gfx_get_sa_active_bitmap(const DeviceContext &dev, const GFXConfig &cfg)
{
    const uint32_t cc_raw =
        RREG32(dev, GC_REG(dev, GRBM_CC_GC_SA_UNIT_DISABLE));
    const uint32_t cc_dis =
        (cc_raw & GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE_MASK)
        >> GRBM_CC_GC_SA_UNIT_DISABLE__SA_DISABLE__SHIFT;

    const uint32_t user_raw =
        RREG32(dev, GC_REG(dev, GRBM_GC_USER_SA_UNIT_DISABLE));
    const uint32_t user_dis =
        (user_raw & GRBM_GC_USER_SA_UNIT_DISABLE__SA_DISABLE_MASK)
        >> GRBM_GC_USER_SA_UNIT_DISABLE__SA_DISABLE__SHIFT;

    const uint32_t sa_mask =
        gfx_create_bitmask(cfg.max_sh_per_se * cfg.max_shader_engines);
    const uint32_t active = sa_mask & ~(cc_dis | user_dis);

    GFX_LOG("harvest SA: GRBM_CC_GC_SA_UNIT_DISABLE@%#x=%#010x (dis=%#x) "
            "GRBM_GC_USER_SA_UNIT_DISABLE@%#x=%#010x (dis=%#x) "
            "sa_mask=%#x active_sa=%#x",
            GC_REG(dev, GRBM_CC_GC_SA_UNIT_DISABLE), cc_raw, cc_dis,
            GC_REG(dev, GRBM_GC_USER_SA_UNIT_DISABLE), user_raw, user_dis,
            sa_mask, active);
    return active;
}

//------------------------------------------------------------------
// gfx_get_rb_active_bitmap — port of gfx_v12_0_get_rb_active_bitmap
// (gfx_v12_0.c:1712).
//------------------------------------------------------------------
static uint32_t
gfx_get_rb_active_bitmap(const DeviceContext &dev, const GFXConfig &cfg)
{
    const uint32_t cc_raw =
        RREG32(dev, GC_REG(dev, CC_RB_BACKEND_DISABLE));
    const uint32_t cc_dis =
        (cc_raw & CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK)
        >> CC_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;

    const uint32_t user_raw =
        RREG32(dev, GC_REG(dev, GC_USER_RB_BACKEND_DISABLE));
    const uint32_t user_dis =
        (user_raw & GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK)
        >> GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;

    const uint32_t rb_mask =
        gfx_create_bitmask(cfg.max_backends_per_se * cfg.max_shader_engines);
    const uint32_t active = rb_mask & ~(cc_dis | user_dis);

    GFX_LOG("harvest RB: CC_RB_BACKEND_DISABLE@%#x=%#010x (dis=%#x) "
            "GC_USER_RB_BACKEND_DISABLE@%#x=%#010x (dis=%#x) "
            "rb_mask=%#x active_rb=%#x",
            GC_REG(dev, CC_RB_BACKEND_DISABLE), cc_raw, cc_dis,
            GC_REG(dev, GC_USER_RB_BACKEND_DISABLE), user_raw, user_dis,
            rb_mask, active);
    return active;
}

//------------------------------------------------------------------
// gfx_get_wgp_active_bitmap_per_sh — port of
// gfx_v12_0_get_wgp_active_bitmap_per_sh (gfx_v12_0.c:5704).
//
// Combines CC + USER INACTIVE_WGPS, inverts to active, masks by
// max_cu_per_sh >> 1 (1 WGP = 2 CUs).
//
// NOTE: reads are scoped by whatever GRBM_GFX_INDEX currently selects.
//------------------------------------------------------------------
static uint32_t
gfx_get_wgp_active_bitmap_per_sh(const DeviceContext &dev,
                                 const GFXConfig &cfg)
{
    const uint32_t cc_raw =
        RREG32(dev, GC_REG(dev, CC_GC_SHADER_ARRAY_CONFIG));
    const uint32_t user_raw =
        RREG32(dev, GC_REG(dev, GC_USER_SHADER_ARRAY_CONFIG));

    uint32_t data = cc_raw;
    data |= user_raw;

    data &= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS_MASK;
    data >>= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_WGPS__SHIFT;

    const uint32_t wgp_bitmask = gfx_create_bitmask(cfg.max_cu_per_sh >> 1);
    const uint32_t active = (~data) & wgp_bitmask;

    GFX_LOG("harvest WGP: CC_GC_SHADER_ARRAY_CONFIG@%#x=%#010x "
            "GC_USER_SHADER_ARRAY_CONFIG@%#x=%#010x "
            "inactive_wgps=%#x wgp_mask=%#x active_wgp=%#x",
            GC_REG(dev, CC_GC_SHADER_ARRAY_CONFIG), cc_raw,
            GC_REG(dev, GC_USER_SHADER_ARRAY_CONFIG), user_raw,
            data, wgp_bitmask, active);
    return active;
}

//------------------------------------------------------------------
// gfx_get_cu_active_bitmap_per_sh — port of
// gfx_v12_0_get_cu_active_bitmap_per_sh (gfx_v12_0.c:5719).
//
// Expand each WGP bit into 2 adjacent CU bits.
//------------------------------------------------------------------
static uint32_t
gfx_get_cu_active_bitmap_per_sh(const DeviceContext &dev,
                                const GFXConfig &cfg)
{
    const uint32_t wgp_bitmap = gfx_get_wgp_active_bitmap_per_sh(dev, cfg);
    uint32_t cu_bitmap = 0;
    for (uint32_t wgp_idx = 0; wgp_idx < 16; wgp_idx++) {
        const uint32_t cu_bitmap_per_wgp = 3u << (2u * wgp_idx);
        if (wgp_bitmap & (1u << wgp_idx)) {
            cu_bitmap |= cu_bitmap_per_wgp;
        }
    }
    return cu_bitmap;
}

//==================================================================
// Public API
//==================================================================

//------------------------------------------------------------------
// gfx_gpu_early_init — port of gfx_v12_0_gpu_early_init
// (gfx_v12_0.c:1640), IP_VERSION(12, 0, 1) branch. No MMIO.
//
// Upstream reads max_shader_engines / max_sh_per_se /
// max_backends_per_se / max_cu_per_sh from the on-die GC discovery
// table; only max_hw_contexts and the sc_* FIFO sizes come from the
// per-IP-version switch. We hardcode the Navi 48 die geometry —
// see deviation #2 at the top of this file. Nothing here assumes how
// many of those units are *enabled*; that is decoded from the
// harvest registers by setup_rb / get_cu_info.
//------------------------------------------------------------------
kern_return_t
gfx_gpu_early_init(const DeviceContext &dev, GFXConfig &cfg)
{
    const IPVersion gc = dev.ip.getVersion(IPBlock::GC);

    // Navi 48 die geometry: 4 SE x 2 SA/SE x 4 WGP/SA x 2 CU/WGP = 64 CU,
    // 4 RB/SE = 16 RBs, 8 hardware contexts.
    cfg.max_shader_engines  = 4;
    cfg.max_sh_per_se       = 2;
    cfg.max_backends_per_se = 4;
    cfg.max_cu_per_sh       = 8;
    cfg.max_hw_contexts     = 8;

    // gfx_v12_0.c:1643-1647 — IP_VERSION(12, 0, 0) / (12, 0, 1).
    cfg.sc_prim_fifo_size_frontend = 0x20;
    cfg.sc_prim_fifo_size_backend  = 0x100;
    cfg.sc_hiz_tile_fifo_size      = 0;
    cfg.sc_earlyz_tile_fifo_size   = 0x4C0;

    GFX_LOG("gpu_early_init: GC %u.%u.%u caps max_se=%u max_sh_per_se=%u "
            "max_be_per_se=%u max_cu_per_sh=%u max_hw_contexts=%u "
            "(physical CUs=%u)",
            gc.major, gc.minor, gc.rev,
            cfg.max_shader_engines, cfg.max_sh_per_se,
            cfg.max_backends_per_se, cfg.max_cu_per_sh,
            cfg.max_hw_contexts,
            cfg.max_shader_engines * cfg.max_sh_per_se * cfg.max_cu_per_sh);
    GFX_LOG("gpu_early_init: sc_prim_fifo fe=%#x be=%#x hiz_tile=%#x "
            "earlyz_tile=%#x",
            cfg.sc_prim_fifo_size_frontend, cfg.sc_prim_fifo_size_backend,
            cfg.sc_hiz_tile_fifo_size, cfg.sc_earlyz_tile_fifo_size);

    if (!dev.ip.isVersion(IPBlock::GC, 12, 0, 1)) {
        GFX_LOG("gpu_early_init: WARNING discovery reports GC %u.%u.%u, "
                "not 12.0.1 — the hardcoded Navi 48 caps may not apply",
                gc.major, gc.minor, gc.rev);
    }
    return kIOReturnSuccess;
}

kern_return_t
gfx_get_gb_addr_config(const DeviceContext &dev, GFXConfig &cfg)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // gfx_v12_0.c:3579 — read regGB_ADDR_CONFIG.
    const uint32_t reg = GC_REG(dev, GB_ADDR_CONFIG);
    const uint32_t gb  = RREG32(dev, reg);
    if (gb == 0 || gb == 0xFFFFFFFFu) {
        GFX_LOG("get_gb_addr_config: GB_ADDR_CONFIG@%#x=%#010x (driver may "
                "be racing with PSP/RLC autoload)", reg, gb);
        return kIOReturnNotReady;
    }

    cfg.gb_addr_config_raw   = gb;
    cfg.num_pipes            = 1u << ((gb & GB_ADDR_CONFIG__NUM_PIPES_MASK)
                                     >> GB_ADDR_CONFIG__NUM_PIPES__SHIFT);
    cfg.num_shader_engines   =
        1u << ((gb & GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK)
              >> GB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT);
    cfg.num_rb_per_se        =
        1u << ((gb & GB_ADDR_CONFIG__NUM_RB_PER_SE_MASK)
              >> GB_ADDR_CONFIG__NUM_RB_PER_SE__SHIFT);
    cfg.num_pkrs             =
        1u << ((gb & GB_ADDR_CONFIG__NUM_PKRS_MASK)
              >> GB_ADDR_CONFIG__NUM_PKRS__SHIFT);
    cfg.pipe_interleave_size =
        1u << (8u + ((gb & GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE_MASK)
                    >> GB_ADDR_CONFIG__PIPE_INTERLEAVE_SIZE__SHIFT));
    cfg.max_compressed_frags =
        1u << ((gb & GB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS_MASK)
              >> GB_ADDR_CONFIG__MAX_COMPRESSED_FRAGS__SHIFT);

    GFX_LOG("gb_addr_config@%#x=%#010x num_pipes=%u num_se=%u "
            "num_rb_per_se=%u num_pkrs=%u pipe_interleave=%u "
            "max_compressed_frags=%u",
            reg, gb, cfg.num_pipes, cfg.num_shader_engines,
            cfg.num_rb_per_se, cfg.num_pkrs, cfg.pipe_interleave_size,
            cfg.max_compressed_frags);

    // Cross-check the live geometry against the hardcoded caps. Not in
    // the reference; log-only (deviation #5) — upstream never compares
    // these two sources either, but if they disagree the caps are wrong
    // and every harvest mask below is the wrong width.
    if (cfg.max_shader_engines != 0 &&
        cfg.num_shader_engines != cfg.max_shader_engines) {
        GFX_LOG("get_gb_addr_config: WARNING GB_ADDR_CONFIG says %u SEs but "
                "caps say max_shader_engines=%u",
                cfg.num_shader_engines, cfg.max_shader_engines);
    }
    if (cfg.max_backends_per_se != 0 &&
        cfg.num_rb_per_se != cfg.max_backends_per_se) {
        GFX_LOG("get_gb_addr_config: WARNING GB_ADDR_CONFIG says %u RBs/SE "
                "but caps say max_backends_per_se=%u",
                cfg.num_rb_per_se, cfg.max_backends_per_se);
    }
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// gfx_setup_rb — port of gfx_v12_0_setup_rb (gfx_v12_0.c:1731).
//------------------------------------------------------------------
kern_return_t
gfx_setup_rb(const DeviceContext &dev, GFXConfig &cfg)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    if (cfg.max_shader_engines == 0 || cfg.max_sh_per_se == 0 ||
        cfg.max_backends_per_se == 0) {
        return kIOReturnNotReady;
    }

    // gfx_v12_0.c:1741-1744 — read SA + RB harvest masks under broadcast.
    gfx_select_se_sh(dev, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);
    const uint32_t active_sa = gfx_get_sa_active_bitmap(dev, cfg);
    const uint32_t global_active_rb = gfx_get_rb_active_bitmap(dev, cfg);

    // gfx_v12_0.c:1747-1751 — derive active_rb_bitmap from active_sa.
    const uint32_t max_sa = cfg.max_shader_engines * cfg.max_sh_per_se;
    const uint32_t rb_bitmap_width_per_sa =
        cfg.max_backends_per_se / cfg.max_sh_per_se;
    const uint32_t rb_bitmap_per_sa =
        gfx_create_bitmask(rb_bitmap_width_per_sa);

    uint32_t active_rb_bitmap = 0;
    for (uint32_t i = 0; i < max_sa; i++) {
        if (active_sa & (1u << i)) {
            active_rb_bitmap |= rb_bitmap_per_sa << (i * rb_bitmap_width_per_sa);
        }
    }
    active_rb_bitmap &= global_active_rb;

    cfg.active_sa_bitmap        = active_sa;
    cfg.global_active_rb_bitmap = global_active_rb;
    cfg.active_rb_bitmap        = active_rb_bitmap;
    cfg.num_rbs                 = gfx_hweight32(active_rb_bitmap);

    GFX_LOG("setup_rb: max_sa=%u rb_width_per_sa=%u rb_per_sa=%#x "
            "active_sa=%#x (%u SAs) global_active_rb=%#x "
            "active_rb_bitmap=%#x num_rbs=%u",
            max_sa, rb_bitmap_width_per_sa, rb_bitmap_per_sa,
            active_sa, gfx_hweight32(active_sa), global_active_rb,
            active_rb_bitmap, cfg.num_rbs);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// gfx_get_cu_info — port of gfx_v12_0_get_cu_info (gfx_v12_0.c:5737).
//------------------------------------------------------------------
kern_return_t
gfx_get_cu_info(const DeviceContext &dev, GFXConfig &cfg)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    cfg.num_active_cus  = 0;
    cfg.num_active_wgps = 0;
    memset(cfg.active_cu_bitmap, 0, sizeof(cfg.active_cu_bitmap));

    // gfx_v12_0.c:5750-5790 — for each (SE, SH) tuple that's active
    // per the SA harvest mask, GRBM-select that (SE, SH), read the
    // CU active bitmap, popcount it, and stash into cu_info.
    for (uint32_t i = 0; i < cfg.max_shader_engines; i++) {
        for (uint32_t j = 0; j < cfg.max_sh_per_se; j++) {
            const uint32_t sa_idx = i * cfg.max_sh_per_se + j;
            if (!((gfx_get_sa_active_bitmap(dev, cfg) >> sa_idx) & 1u)) {
                GFX_LOG("get_cu_info: SE%u SA%u (sa_idx=%u) harvested off",
                        i, j, sa_idx);
                continue;
            }

            gfx_select_se_sh(dev, i, j, 0xFFFFFFFFu);
            const uint32_t bitmap = gfx_get_cu_active_bitmap_per_sh(dev, cfg);

            // Mirror upstream's "GFX12 can have > 4 SEs but ioctl
            // table is 4x4" layout (gfx_v12_0.c:5778). Our config
            // limits the array to 4x2 (Navi 48 has 4 SEs x 2 SAs), so
            // for i in [0..3] and j in [0..1] this is straightforward.
            if (i < 4 && j < 2) {
                cfg.active_cu_bitmap[i % 4][j + (i / 4) * 2] = bitmap;
            }

            // popcount up to max_cu_per_sh.
            uint32_t counter = 0;
            uint32_t mask = 1u;
            for (uint32_t k = 0; k < cfg.max_cu_per_sh; k++) {
                if (bitmap & mask) counter++;
                mask <<= 1;
            }
            cfg.num_active_cus  += counter;
            // Each WGP contributes exactly 2 adjacent CU bits.
            cfg.num_active_wgps += counter / 2u;

            GFX_LOG("get_cu_info: SE%u SA%u cu_bitmap=%#010x active_cus=%u",
                    i, j, bitmap, counter);
        }
    }

    gfx_select_se_sh(dev, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu);

    GFX_LOG("get_cu_info: num_active_cus=%u num_active_wgps=%u "
            "(physical max %u CUs)",
            cfg.num_active_cus, cfg.num_active_wgps,
            cfg.max_shader_engines * cfg.max_sh_per_se * cfg.max_cu_per_sh);
    GFX_LOG("get_cu_info: cu_bitmap SE0=[%#x,%#x] SE1=[%#x,%#x] "
            "SE2=[%#x,%#x] SE3=[%#x,%#x]",
            cfg.active_cu_bitmap[0][0], cfg.active_cu_bitmap[0][1],
            cfg.active_cu_bitmap[1][0], cfg.active_cu_bitmap[1][1],
            cfg.active_cu_bitmap[2][0], cfg.active_cu_bitmap[2][1],
            cfg.active_cu_bitmap[3][0], cfg.active_cu_bitmap[3][1]);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// gfx_init_compute_vmid — port of gfx_v12_0_init_compute_vmid
// (gfx_v12_0.c:1766).
//
// VMIDs first_kfd_vmid..AMDGPU_NUM_VMID-1 get LDS/Scratch aperture
// bases programmed and SPI_GDBG_PER_VMID_CNTL.TRAP_EN = 1.
//
// AMDGPU_NUM_VMID = 16. KFD VMIDs are 8..15 (first_kfd_vmid = 8 on
// gfx12 with default num_kfd_vmids).
//------------------------------------------------------------------
kern_return_t
gfx_init_compute_vmid(const DeviceContext &dev)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // gfx_v12_0.c:1763-1764 + 1778-1779.
    //   LDS_APP_BASE     = 0x1
    //   SCRATCH_APP_BASE = 0x2
    //   sh_mem_bases     = (LDS_APP_BASE << SHARED_BASE__SHIFT) | SCRATCH_APP_BASE
    constexpr uint32_t kLDS_APP_BASE     = 0x1;
    constexpr uint32_t kSCRATCH_APP_BASE = 0x2;
    const uint32_t sh_mem_bases =
        (kLDS_APP_BASE << SH_MEM_BASES__SHARED_BASE__SHIFT)
        | kSCRATCH_APP_BASE;

    constexpr uint32_t kFirstKFDVmid = 8;
    constexpr uint32_t kNumVmid      = 16;

    const uint32_t reg_mem_config = GC_REG(dev, SH_MEM_CONFIG);
    const uint32_t reg_mem_bases  = GC_REG(dev, SH_MEM_BASES);
    const uint32_t reg_gdbg       = GC_REG(dev, SPI_GDBG_PER_VMID_CNTL);
    const uint32_t reg_grbm_cntl  = GC_REG(dev, GRBM_GFX_CNTL);

    for (uint32_t i = kFirstKFDVmid; i < kNumVmid; i++) {
        // Equivalent of soc24_grbm_select(adev, me=0, pipe=0, queue=0, vmid=i).
        uint32_t sel = 0;
        sel = REG_SET_FIELD(sel, GRBM_GFX_CNTL, VMID, i);
        WREG32(dev, reg_grbm_cntl, sel);

        // gfx_v12_0.c:1785-1786 — config + bases.
        WREG32(dev, reg_mem_config, kDefaultSHMemConfig);
        WREG32(dev, reg_mem_bases,  sh_mem_bases);

        // gfx_v12_0.c:1789-1791 — TRAP_EN = 1 for each KFD vmid.
        uint32_t data = RREG32(dev, reg_gdbg);
        data = REG_SET_FIELD(data, SPI_GDBG_PER_VMID_CNTL, TRAP_EN, 1);
        WREG32(dev, reg_gdbg, data);
    }
    // Deselect.
    WREG32(dev, reg_grbm_cntl, 0);

    GFX_LOG("init_compute_vmid: programmed VMIDs %u..%u "
            "(SH_MEM_CONFIG@%#x=%#010x SH_MEM_BASES@%#x=%#010x "
            "SPI_GDBG_PER_VMID_CNTL@%#x TRAP_EN=1)",
            kFirstKFDVmid, kNumVmid - 1,
            reg_mem_config, kDefaultSHMemConfig,
            reg_mem_bases, sh_mem_bases, reg_gdbg);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// gfx_constants_init — direct port of gfx_v12_0_constants_init
// (gfx_v12_0.c:1806).
//
// Audit-7 #8.
//------------------------------------------------------------------
kern_return_t
gfx_constants_init(const DeviceContext &dev, GFXConfig &cfg)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        GFX_LOG("constants_init: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    // Navi 48 (gfx1201) early-init caps — upstream's
    // gfx_v12_0_gpu_early_init IP_VERSION(12, 0, 1) branch plus the
    // discovery-sourced die geometry. No MMIO.
    gfx_gpu_early_init(dev, cfg);

    // Deviation #4 — read-only GC BASE_IDX probe + a gate on the first
    // write. Must run BEFORE the GRBM_CNTL write below.
    {
        kern_return_t pr = gfx_probe_gc_bases(dev, cfg);
        if (pr != kIOReturnSuccess) {
            GFX_LOG("constants_init: GC register window probe failed: %#x "
                    "— aborting before any write", pr);
            return pr;
        }
    }

    // gfx_v12_0.c:1812 — GRBM_CNTL.READ_TIMEOUT = 0xFF.
    {
        const uint32_t reg = GC_REG(dev, GRBM_CNTL);
        uint32_t old = RREG32(dev, reg);
        uint32_t v = (old & ~kGRBM_CNTL_READ_TIMEOUT_MASK)
                     | kGRBM_CNTL_READ_TIMEOUT_VALUE;
        WREG32(dev, reg, v);
        GFX_LOG("constants_init: GRBM_CNTL@%#x %#010x -> %#010x "
                "(READ_TIMEOUT=%#x)",
                reg, old, v, kGRBM_CNTL_READ_TIMEOUT_VALUE);
    }

    // gfx_v12_0.c:1814-1816 — setup_rb + get_cu_info + get_tcc_info.
    // get_tcc_info is empty upstream on gfx12 (gfx_v12_0.c:1802).
    //
    // Both setup_rb and get_cu_info NEED gb_addr_config populated for
    // some fields, but on RDNA4 the caps we use are hardcoded — the
    // gb_addr_config read is best-effort and non-fatal.
    (void)gfx_get_gb_addr_config(dev, cfg);
    kern_return_t r;
    r = gfx_setup_rb(dev, cfg);
    if (r != kIOReturnSuccess) {
        GFX_LOG("setup_rb failed: %#x (continuing)", r);
    }
    r = gfx_get_cu_info(dev, cfg);
    if (r != kIOReturnSuccess) {
        GFX_LOG("get_cu_info failed: %#x (continuing)", r);
    }

    // gfx_v12_0.c:1820-1836 — per-VMID SH_MEM_CONFIG + SH_MEM_BASES.
    // Iterate VMIDs 0..num_ids-1 (typically 8 — the system + gfx VMIDs).
    {
        constexpr uint32_t kNumGfxVmids = 8;
        const uint32_t reg_mem_config = GC_REG(dev, SH_MEM_CONFIG);
        const uint32_t reg_mem_bases  = GC_REG(dev, SH_MEM_BASES);
        const uint32_t reg_grbm_cntl  = GC_REG(dev, GRBM_GFX_CNTL);

        uint32_t bases_written = 0;
        for (uint32_t i = 0; i < kNumGfxVmids; i++) {
            // grbm-select with vmid = i, others = 0.
            uint32_t sel = 0;
            sel = REG_SET_FIELD(sel, GRBM_GFX_CNTL, VMID, i);
            WREG32(dev, reg_grbm_cntl, sel);

            // gfx_v12_0.c:1825 — SH_MEM_CONFIG = DEFAULT for every VMID.
            WREG32(dev, reg_mem_config, kDefaultSHMemConfig);

            if (i != 0) {
                // gfx_v12_0.c:1827-1831 — VMID i >= 1 gets the gmc
                // private/shared aperture bases. Both apertures are
                // 48-bit GPU-VA tops; we don't yet track the
                // private_aperture_start / shared_aperture_start
                // values in GMCContext, so use the same hardcoded
                // KFD apertures that gfx_init_compute_vmid uses
                // (LDS=0x1, SCRATCH=0x2) — RDNA4 default per
                // gmc_v12_0_init_default_aperture in upstream.
                uint32_t bases = 0;
                bases = REG_SET_FIELD(bases, SH_MEM_BASES, PRIVATE_BASE, 0x2);
                bases = REG_SET_FIELD(bases, SH_MEM_BASES, SHARED_BASE,  0x1);
                WREG32(dev, reg_mem_bases, bases);
                bases_written = bases;
            }
        }
        // grbm-deselect.
        WREG32(dev, reg_grbm_cntl, 0);

        GFX_LOG("constants_init: VMIDs 0..%u SH_MEM_CONFIG@%#x=%#010x, "
                "VMIDs 1..%u SH_MEM_BASES@%#x=%#010x "
                "(GRBM_GFX_CNTL@%#x deselected)",
                kNumGfxVmids - 1, reg_mem_config, kDefaultSHMemConfig,
                kNumGfxVmids - 1, reg_mem_bases, bases_written,
                reg_grbm_cntl);
    }

    // gfx_v12_0.c:1838 — init_compute_vmid.
    gfx_init_compute_vmid(dev);

    cfg.inited = true;
    GFX_LOG("constants_init complete: max_se=%u max_sh=%u max_be=%u "
            "max_cu_per_sh=%u (active_rbs=%u active_cus=%u)",
            cfg.max_shader_engines, cfg.max_sh_per_se,
            cfg.max_backends_per_se, cfg.max_cu_per_sh,
            cfg.num_rbs, cfg.num_active_cus);
    return kIOReturnSuccess;
}

// Convenience overload — uses a shared GFXConfig for callers that
// don't yet have a real GFXContext. Should be migrated in a follow-up.
//
// Deviation #6: file-scope rather than function-local static — kexts do
// not run C++ static constructors or __cxa_guard_* reliably; a POD at
// file scope is zero-initialised in .bss with no guard variable.
static GFXConfig s_cfg;

kern_return_t
gfx_constants_init(const DeviceContext &dev)
{
    return gfx_constants_init(dev, s_cfg);
}

//==================================================================
// MQD builders — port the field math from gfx_v12_0_compute_mqd_init
// and gfx_v12_0_gfx_mqd_init.  Audit-7 #9.
//==================================================================

static inline uint32_t
order_base_2_local(uint32_t x)
{
    uint32_t r = 0;
    while ((1u << r) < x) r++;
    return r;
}

kern_return_t
gfx_build_compute_mqd(uint32_t *mqd_cpu, const GFXMqdProps &p)
{
    if (mqd_cpu == nullptr) return kIOReturnBadArgument;

    // Zero everything first — gfx_v12_0_kiq_init_queue / kcq_init_queue
    // do memset((void*)mqd, 0, sizeof(*mqd)) before this. (Caller did
    // the same; defensive re-zero is harmless.)
    memset(mqd_cpu, 0, kGFX_MQDPageBytes);

    // gfx_v12_0.c:3150-3156 — magic header + per-SE thread-mgmt masks.
    mqd_cpu[ComputeMqdOff::header]                         = 0xC0310800u;
    mqd_cpu[ComputeMqdOff::compute_pipelinestat_enable]    = 0x00000001u;
    mqd_cpu[ComputeMqdOff::compute_static_thread_mgmt_se0] = 0xFFFFFFFFu;
    mqd_cpu[ComputeMqdOff::compute_static_thread_mgmt_se1] = 0xFFFFFFFFu;
    mqd_cpu[ComputeMqdOff::compute_static_thread_mgmt_se2] = 0xFFFFFFFFu;
    mqd_cpu[ComputeMqdOff::compute_static_thread_mgmt_se3] = 0xFFFFFFFFu;
    mqd_cpu[ComputeMqdOff::compute_misc_reserved]          = 0x00000007u;

    // gfx_v12_0.c:3158-3167 — EOP base + size.
    const uint64_t eop_base_addr = p.eop_gpu_addr >> 8;
    mqd_cpu[ComputeMqdOff::cp_hqd_eop_base_addr_lo] =
        static_cast<uint32_t>(eop_base_addr);
    mqd_cpu[ComputeMqdOff::cp_hqd_eop_base_addr_hi] =
        static_cast<uint32_t>(eop_base_addr >> 32);
    uint32_t eop_ctrl = kCP_HQD_EOP_CONTROL_DEFAULT;
    if (p.eop_size_bytes > 0) {
        const uint32_t eop_log =
            order_base_2_local(p.eop_size_bytes / 4u) - 1u;
        eop_ctrl = REG_SET_FIELD(eop_ctrl, CP_HQD_EOP_CONTROL,
                                 EOP_SIZE, eop_log);
    }
    mqd_cpu[ComputeMqdOff::cp_hqd_eop_control] = eop_ctrl;

    // gfx_v12_0.c:3170-3186 — doorbell control.
    uint32_t db_ctrl = kRegCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
    if (p.use_doorbell) {
        db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                                DOORBELL_OFFSET, p.doorbell_index);
        db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                                DOORBELL_EN, 1);
        db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                                DOORBELL_SOURCE, 0);
        db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                                DOORBELL_HIT, 0);
    } else {
        db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                                DOORBELL_EN, 0);
    }
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_doorbell_control] = db_ctrl;

    // gfx_v12_0.c:3189-3196 — disable + MQD base pointer.
    mqd_cpu[ComputeMqdOff::cp_hqd_dequeue_request]     = 0;
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_rptr]             = 0;
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_wptr_lo]          = 0;
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_wptr_hi]          = 0;
    mqd_cpu[ComputeMqdOff::cp_mqd_base_addr_lo] =
        static_cast<uint32_t>(p.mqd_gpu_addr & 0xFFFFFFFCu);
    mqd_cpu[ComputeMqdOff::cp_mqd_base_addr_hi] =
        static_cast<uint32_t>(p.mqd_gpu_addr >> 32);

    // gfx_v12_0.c:3199-3201 — MQD VMID = 0.
    {
        uint32_t v = kCP_MQD_CONTROL_DEFAULT;
        v = REG_SET_FIELD(v, CP_MQD_CONTROL, VMID, 0);
        mqd_cpu[ComputeMqdOff::cp_mqd_control] = v;
    }

    // gfx_v12_0.c:3203-3206 — HQD PQ base.
    const uint64_t hqd_gpu_addr = p.hqd_base_gpu_addr >> 8;
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_base_lo] =
        static_cast<uint32_t>(hqd_gpu_addr);
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_base_hi] =
        static_cast<uint32_t>(hqd_gpu_addr >> 32);

    // gfx_v12_0.c:3209-3222 — HQD PQ control.
    {
        uint32_t v = kCP_HQD_PQ_CONTROL_DEFAULT;
        v = REG_SET_FIELD(v, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
                          order_base_2_local(p.queue_size_bytes / 4u) - 1u);
        v = REG_SET_FIELD(v, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE,
                          order_base_2_local(4096u / 4u) - 1u);
        v = REG_SET_FIELD(v, CP_HQD_PQ_CONTROL, UNORD_DISPATCH,  1);
        v = REG_SET_FIELD(v, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
        if (p.kernel_queue) {
            v = REG_SET_FIELD(v, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
            v = REG_SET_FIELD(v, CP_HQD_PQ_CONTROL, KMD_QUEUE,  1);
        }
        mqd_cpu[ComputeMqdOff::cp_hqd_pq_control] = v;
    }

    // gfx_v12_0.c:3224-3233 — rptr/wptr write-back/poll addresses.
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_rptr_report_addr_lo] =
        static_cast<uint32_t>(p.rptr_gpu_addr & 0xFFFFFFFCu);
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_rptr_report_addr_hi] =
        static_cast<uint32_t>(p.rptr_gpu_addr >> 32) & 0xFFFFu;
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_wptr_poll_addr_lo] =
        static_cast<uint32_t>(p.wptr_gpu_addr & 0xFFFFFFFCu);
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_wptr_poll_addr_hi] =
        static_cast<uint32_t>(p.wptr_gpu_addr >> 32) & 0xFFFFu;

    // gfx_v12_0.c:3253 — initial rptr value (DEFAULT).
    mqd_cpu[ComputeMqdOff::cp_hqd_pq_rptr] = kRegCP_HQD_PQ_RPTR_DEFAULT;

    // gfx_v12_0.c:3256 — VMID.
    mqd_cpu[ComputeMqdOff::cp_hqd_vmid] = 0;

    // gfx_v12_0.c:3258-3260 — persistent state with PRELOAD_SIZE = 0x55.
    {
        uint32_t v = kCP_HQD_PERSISTENT_STATE_DEFAULT;
        v = REG_SET_FIELD(v, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x55);
        mqd_cpu[ComputeMqdOff::cp_hqd_persistent_state] = v;
    }

    // gfx_v12_0.c:3263-3265 — IB control.
    {
        uint32_t v = kRegCP_HQD_IB_CONTROL_DEFAULT;
        v = REG_SET_FIELD(v, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
        mqd_cpu[ComputeMqdOff::cp_hqd_ib_control] = v;
    }

    // gfx_v12_0.c:3268-3271 — pipe/queue priorities + active flag.
    mqd_cpu[ComputeMqdOff::cp_hqd_pipe_priority]  = 0;
    mqd_cpu[ComputeMqdOff::cp_hqd_queue_priority] = 0;
    mqd_cpu[ComputeMqdOff::cp_hqd_active]         = 1;

    return kIOReturnSuccess;
}

kern_return_t
gfx_build_gfx_mqd(uint32_t *mqd_cpu, const GFXMqdProps &p)
{
    if (mqd_cpu == nullptr) return kIOReturnBadArgument;

    memset(mqd_cpu, 0, kGFX_MQDPageBytes);

    // gfx_v12_0.c:2978-2979 — wptr starts at 0.
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_wptr]    = 0;
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_wptr_hi] = 0;

    // gfx_v12_0.c:2982-2983 — MQD base.
    mqd_cpu[GfxMqdOff::cp_mqd_base_addr] =
        static_cast<uint32_t>(p.mqd_gpu_addr & 0xFFFFFFFCu);
    mqd_cpu[GfxMqdOff::cp_mqd_base_addr_hi] =
        static_cast<uint32_t>(p.mqd_gpu_addr >> 32);

    // gfx_v12_0.c:2986-2990 — MQD control: VMID 0, PRIV_STATE = 1.
    {
        uint32_t v = kRegCP_GFX_MQD_CONTROL_DEFAULT;
        v = REG_SET_FIELD(v, CP_GFX_MQD_CONTROL, VMID, 0);
        v = REG_SET_FIELD(v, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
        v = REG_SET_FIELD(v, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
        mqd_cpu[GfxMqdOff::cp_gfx_mqd_control] = v;
    }

    // gfx_v12_0.c:2993-2995 — HQD VMID. Note upstream writes 0
    // directly into mqd->cp_gfx_hqd_vmid (line 2995) despite the
    // REG_SET_FIELD above; both lines set the same value, just keep
    // it consistent with upstream's last write wins.
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_vmid] = 0;

    // gfx_v12_0.c:2999-3001 — priority.
    {
        uint32_t v = kRegCP_GFX_HQD_QUEUE_PRIORITY_DEFAULT;
        v = REG_SET_FIELD(v, CP_GFX_HQD_QUEUE_PRIORITY,
                          PRIORITY_LEVEL, 0);
        mqd_cpu[GfxMqdOff::cp_gfx_hqd_queue_priority] = v;
    }

    // gfx_v12_0.c:3004-3006 — quantum.
    {
        uint32_t v = kRegCP_GFX_HQD_QUANTUM_DEFAULT;
        v = REG_SET_FIELD(v, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
        mqd_cpu[GfxMqdOff::cp_gfx_hqd_quantum] = v;
    }

    // gfx_v12_0.c:3009-3011 — HQD base.
    const uint64_t hqd_gpu_addr = p.hqd_base_gpu_addr >> 8;
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_base]    =
        static_cast<uint32_t>(hqd_gpu_addr);
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_base_hi] =
        static_cast<uint32_t>(hqd_gpu_addr >> 32);

    // gfx_v12_0.c:3013-3017 — rptr_addr.
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_rptr_addr] =
        static_cast<uint32_t>(p.rptr_gpu_addr & 0xFFFFFFFCu);
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_rptr_addr_hi] =
        static_cast<uint32_t>(p.rptr_gpu_addr >> 32) & 0xFFFFu;

    // gfx_v12_0.c:3020-3022 — wptr poll addr.
    mqd_cpu[GfxMqdOff::cp_rb_wptr_poll_addr_lo] =
        static_cast<uint32_t>(p.wptr_gpu_addr & 0xFFFFFFFCu);
    mqd_cpu[GfxMqdOff::cp_rb_wptr_poll_addr_hi] =
        static_cast<uint32_t>(p.wptr_gpu_addr >> 32) & 0xFFFFu;

    // gfx_v12_0.c:3025-3036 — HQD control.
    {
        const uint32_t rb_bufsz =
            order_base_2_local(p.queue_size_bytes / 4u) - 1u;
        uint32_t v = kRegCP_GFX_HQD_CNTL_DEFAULT;
        v = REG_SET_FIELD(v, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
        v = REG_SET_FIELD(v, CP_GFX_HQD_CNTL, RB_BLKSZ,
                          (rb_bufsz >= 2) ? (rb_bufsz - 2) : 0);
        if (!p.kernel_queue) {
            v = REG_SET_FIELD(v, CP_GFX_HQD_CNTL, RB_NON_PRIV, 1);
        }
        mqd_cpu[GfxMqdOff::cp_gfx_hqd_cntl] = v;
    }

    // gfx_v12_0.c:3039-3048 — doorbell.
    {
        uint32_t v = kRegCP_HQD_PQ_DOORBELL_CONTROL_DEFAULT;
        if (p.use_doorbell) {
            v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL,
                              DOORBELL_OFFSET, p.doorbell_index);
            v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL,
                              DOORBELL_EN, 1);
        } else {
            v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL,
                              DOORBELL_EN, 0);
        }
        mqd_cpu[GfxMqdOff::cp_rb_doorbell_control] = v;
    }

    // gfx_v12_0.c:3051 — initial rptr.
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_rptr] = kRegCP_GFX_HQD_RPTR_DEFAULT;

    // gfx_v12_0.c:3054 — activate.
    mqd_cpu[GfxMqdOff::cp_gfx_hqd_active] = 1;

    return kIOReturnSuccess;
}

// (gfx_gfxhub_enable_after_rlc is declared in amdgpu_gfx.h for the
// stage handler to depend on, but its body lives in amdgpu_init.cpp
// where BringupContext.gmc is available. Audit-7 #10.)

} // namespace amdgpu
