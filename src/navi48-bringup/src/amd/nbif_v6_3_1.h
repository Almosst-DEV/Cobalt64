//
//  nbif_v6_3_1.h — NBIO/NBIF 6.3.1 doorbell-path bring-up for Navi 48.
//
//  Origin: Linux drivers/gpu/drm/amd/amdgpu/nbif_v6_3_1.c (and its call sites
//  in soc24.c / gfx_v12_0.c / sdma_v7_0.c). Those files and the AMD register
//  headers they include carry AMD's MIT notice:
//
//      Copyright 2023 Advanced Micro Devices, Inc.
//      Permission is hereby granted, free of charge, to any person obtaining a
//      copy of this software and associated documentation files (the
//      "Software"), to deal in the Software without restriction ... THE
//      SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.
//
//  Register offsets / BASE_IDX / field SHIFT+MASK below are transcribed from
//  the authoritative headers, which are the truth for this port:
//      ref/linux-asic-reg/nbif_6_3_1_offset.h
//      ref/linux-asic-reg/nbif_6_3_1_sh_mask.h
//  and, for the three `_nbif_4_10` aliases, from the local #defines at the top
//  of nbif_v6_3_1.c itself (they are not in the offset header).
//
//  ---------------------------------------------------------------------------
//  Deviations from the reference (Linux nbif_v6_3_1.c)
//  ---------------------------------------------------------------------------
//   1. Register access is routed per-register: an absolute dword index that
//      still fits inside the mapped BAR5 window uses RREG32/WREG32, anything
//      past it uses SMN_WREG32/SMN_RREG32 (BIF_BX1_PCIE_INDEX2/DATA2). Linux
//      does the same thing inside amdgpu_device_rreg()/wreg(); here it is
//      explicit and logged, because on this card BAR5 is only 512 KiB
//      (dword index >= 0x20000 is outside it) and regRCC_DEV0_EPF2_STRAP2
//      lives at NBIO BASE_IDX 5, far outside.
//   2. adev->doorbell.base is not available, so the BAR2 physical address is
//      passed in as `bar2Phys` by the caller.
//   3. Every register is logged before the write, after the write, and
//      readback-checked under a field mask; each function returns
//      kIOReturnIOError on a readback mismatch. Linux never checks.
//   4. Read-modify-write functions refuse to write when the pre-read is
//      0xFFFFFFFF (no response through the chosen path), instead of
//      scribbling all-ones back into a strap/control register.
//   5. nbif_v6_3_1_doorbell_path_init() bundles the pieces soc24.c spreads
//      across early_init / hw_init / late_init and gfx_v12_0_hw_init, in
//      Linux's relative order. init_registers and remap_hdp_registers are
//      treated as non-fatal there (nothing in the doorbell path depends on
//      them); the three doorbell steps are fatal.
//   6. sdma_doorbell_range is exported but NOT called by doorbell_path_init —
//      it belongs to the SDMA stage, exactly as in Linux (sdma_v7_0.c).
//   7. Not ported (out of scope for the doorbell path): ih_doorbell_range,
//      vcn/vpe_doorbell_range, ih_control, mc_access_enable, get_rev_id,
//      get_rom_offset, program_aspm/ltr, the RAS irq plumbing.
//
#pragma once

#include "amdgpu_regs.h"
#include "amdgpu_log.h"

#define NBIF_LOG(fmt, ...) AMDGPU_LOG("nbif", fmt, ##__VA_ARGS__)

// ===========================================================================
// Register offsets + BASE_IDX — nbif_6_3_1_offset.h (verbatim)
// ===========================================================================

// Doorbell aperture enable. nbif_6_3_1_offset.h:1807.
#define regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN                     0x00c0
#define regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN_BASE_IDX            2

// Self-ring (GPU-originated) doorbell aperture. nbif_6_3_1_offset.h:1759-1764.
#define regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH        0x00f3
#define regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH_BASE_IDX 2
#define regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW         0x00f4
#define regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW_BASE_IDX  2
#define regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL             0x00f5
#define regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL_BASE_IDX      2

// HDP flush-register remap window. nbif_6_3_1_offset.h:1501-1504.
// NOTE: nbif_v6_3_1.c's IP_VERSION(7,11,5) alias
// regBIF_BX1_REMAP_HDP_MEM_FLUSH_CNTL_nbio_7_11_5 resolves to the *same*
// 0x012d / BASE_IDX 2, so there is no alternative address to try.
#define regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL                       0x012d
#define regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL_BASE_IDX              2
#define regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL                       0x012e
#define regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL_BASE_IDX              2

// GDC "slave-to-AXI" doorbell routing entries. nbif_6_3_1_offset.h:1839-1846.
// Entry 0 and 3 are GC (gc_doorbell_init); entry 2 is SDMA
// (sdma_doorbell_range); entry 1 is IH, 4/5 are VCN/VPE (not ported).
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL                     0x01cb
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_BASE_IDX            2
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL                     0x01cd
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_BASE_IDX            2
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL                     0x01ce
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_BASE_IDX            2

// Strap register cleared by nbif_v6_3_1_init_registers.
// nbif_6_3_1_offset.h:6985-6986.
//
// !! This contradicts the comment in amdgpu_ip.h (NBIORegs::RCC_DEV0_EPF2_
// !! STRAP2 = 0x009A, BASE_IDX 2, mask 0x2). The header wins: 0xd102 at
// !! BASE_IDX 5, mask 0x80 (bit 7). BASE_IDX 5 is far outside BAR5 -> SMN.
#define regRCC_DEV0_EPF2_STRAP2                                   0xd102
#define regRCC_DEV0_EPF2_STRAP2_BASE_IDX                          5

// `_nbif_4_10` variants, used by nbif_v6_3_1.c only when
// amdgpu_ip_version(NBIO_HWIP) >= IP_VERSION(7, 11, 4). Transcribed from the
// local #defines at nbif_v6_3_1.c:33-52 (not present in the offset header).
// Navi 48 reports NBIF 6.3.1, so these are never selected here — they exist so
// the runtime branch is real rather than hardcoded for one chip.
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10           0x4f0aeb
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10_BASE_IDX  3
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10           0x4f0aef
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10_BASE_IDX  3
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10           0x4f0af1
#define regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10_BASE_IDX  3

// ===========================================================================
// Field SHIFT / MASK — nbif_6_3_1_sh_mask.h (verbatim, `L` suffix dropped per
// the amdgpu_field_defs.h convention)
// ===========================================================================

// nbif_6_3_1_sh_mask.h:11205-11206
#define RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN__SHIFT   0x0
#define RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN_MASK     0x00000001

// nbif_6_3_1_sh_mask.h:10982-10993
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH__DOORBELL_SELFRING_GPA_APER_BASE_HIGH__SHIFT 0x0
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH__DOORBELL_SELFRING_GPA_APER_BASE_HIGH_MASK   0xFFFFFFFF
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW__DOORBELL_SELFRING_GPA_APER_BASE_LOW__SHIFT   0x0
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW__DOORBELL_SELFRING_GPA_APER_BASE_LOW_MASK     0xFFFFFFFF
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_EN__SHIFT             0x0
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_MODE__SHIFT           0x1
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_SIZE__SHIFT           0x8
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_EN_MASK               0x00000001
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_MODE_MASK             0x00000002
#define BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_SIZE_MASK             0x000FFF00

// nbif_6_3_1_sh_mask.h:9783-9787. Only ADDRESS is defined: bits 18:2 hold the
// BAR5 *byte* offset of the remapped flush window (bits 1:0 implicitly 0), so
// Linux writes the byte offset straight into the register without REG_SET_FIELD.
#define BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL__ADDRESS__SHIFT                  0x2
#define BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL__ADDRESS_MASK                    0x0007FFFC
#define BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL__ADDRESS__SHIFT                  0x2
#define BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL__ADDRESS_MASK                    0x0007FFFC

// nbif_6_3_1_sh_mask.h:18255/18268 — bit 7, NOT bit 1 as amdgpu_ip.h claims.
#define RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2__SHIFT          0x7
#define RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK            0x00000080

// S2A doorbell entry field layout. Identical bit positions for every entry,
// but the field names carry the port number, so all three used entries are
// spelled out. nbif_6_3_1_sh_mask.h:11313-11330 / 11351-11368 / 11370-11387.
//
//   ENABLE                    [0]      route this port at all
//   AWID                      [5:1]    AXI write ID handed to the consumer
//   FENCE_ENABLE              [6]
//   RANGE_OFFSET              [16:7]   first doorbell index claimed  (10 bits)
//   RANGE_SIZE                [24:17]  number of doorbell indices    (8 bits)
//   64BIT_SUPPORT_DIS         [25]
//   NEED_DEDUCT_RANGE_OFFSET  [26]     subtract RANGE_OFFSET from the AXI addr
//   DROP_EN                   [27]
//   AWADDR_31_28_VALUE        [31:28]  bits 31:28 of the generated AXI address
//
// The nine masks tile all 32 bits with no gap and no overlap, so a readback of
// an entry can be compared against the written word exactly.
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_ENABLE__SHIFT                   0x0
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_AWID__SHIFT                     0x1
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_FENCE_ENABLE__SHIFT             0x6
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_RANGE_OFFSET__SHIFT             0x7
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_RANGE_SIZE__SHIFT               0x11
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_64BIT_SUPPORT_DIS__SHIFT        0x19
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_NEED_DEDUCT_RANGE_OFFSET__SHIFT 0x1a
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_DROP_EN__SHIFT                  0x1b
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_AWADDR_31_28_VALUE__SHIFT       0x1c
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_ENABLE_MASK                     0x00000001
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_AWID_MASK                       0x0000003E
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_FENCE_ENABLE_MASK               0x00000040
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_RANGE_OFFSET_MASK               0x0001FF80
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_RANGE_SIZE_MASK                 0x01FE0000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_64BIT_SUPPORT_DIS_MASK          0x02000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_NEED_DEDUCT_RANGE_OFFSET_MASK   0x04000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_DROP_EN_MASK                    0x08000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL__S2A_DOORBELL_PORT0_AWADDR_31_28_VALUE_MASK         0xF0000000

#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_ENABLE__SHIFT                   0x0
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWID__SHIFT                     0x1
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_FENCE_ENABLE__SHIFT             0x6
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET__SHIFT             0x7
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE__SHIFT               0x11
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_64BIT_SUPPORT_DIS__SHIFT        0x19
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_NEED_DEDUCT_RANGE_OFFSET__SHIFT 0x1a
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_DROP_EN__SHIFT                  0x1b
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE__SHIFT       0x1c
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_ENABLE_MASK                     0x00000001
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWID_MASK                       0x0000003E
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_FENCE_ENABLE_MASK               0x00000040
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_OFFSET_MASK               0x0001FF80
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_RANGE_SIZE_MASK                 0x01FE0000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_64BIT_SUPPORT_DIS_MASK          0x02000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_NEED_DEDUCT_RANGE_OFFSET_MASK   0x04000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_DROP_EN_MASK                    0x08000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL__S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE_MASK         0xF0000000

#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_ENABLE__SHIFT                   0x0
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_AWID__SHIFT                     0x1
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_FENCE_ENABLE__SHIFT             0x6
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_RANGE_OFFSET__SHIFT             0x7
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_RANGE_SIZE__SHIFT               0x11
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_64BIT_SUPPORT_DIS__SHIFT        0x19
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_NEED_DEDUCT_RANGE_OFFSET__SHIFT 0x1a
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_DROP_EN__SHIFT                  0x1b
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_AWADDR_31_28_VALUE__SHIFT       0x1c
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_ENABLE_MASK                     0x00000001
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_AWID_MASK                       0x0000003E
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_FENCE_ENABLE_MASK               0x00000040
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_RANGE_OFFSET_MASK               0x0001FF80
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_RANGE_SIZE_MASK                 0x01FE0000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_64BIT_SUPPORT_DIS_MASK          0x02000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_NEED_DEDUCT_RANGE_OFFSET_MASK   0x04000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_DROP_EN_MASK                    0x08000000
#define GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL__S2A_DOORBELL_PORT3_AWADDR_31_28_VALUE_MASK         0xF0000000

// ===========================================================================
// HDP remap window placement
// ===========================================================================
//
// nbif_v6_3_1.c:613  #define MMIO_REG_HOLE_OFFSET (0x80000 - PAGE_SIZE)
// nbif_v6_3_1_set_reg_remap() (nbif_v6_3_1.c:615), called from
// soc24_common_early_init (soc24.c:335):
//
//     if (!amdgpu_sriov_vf(adev) && (PAGE_SIZE <= 4096)) {
//         adev->rmmio_remap.reg_offset = MMIO_REG_HOLE_OFFSET;
//         adev->rmmio_remap.bus_addr   = adev->rmmio_base + MMIO_REG_HOLE_OFFSET;
//     } else { ... regBIF_BX_PF0_HDP_MEM_COHERENCY_FLUSH_CNTL << 2 ... }
//
// We are x86_64 macOS: the kernel page size is 4096, so the first branch
// applies and the window is the LAST 4 KiB page of the 512 KiB register BAR —
// bytes 0x7F000..0x7FFFF. That page is the architectural "MMIO register hole":
// no IP block's discovered base + offset lands in it, which is exactly why
// Linux picks it and then exposes it to user space (KFD maps that page into a
// process so it can ring the HDP flush without a syscall).
//
// (mac-amdgpu's arm64 dext has PAGE_SIZE 16384 and therefore takes the *other*
// branch upstream; on this port we must take the 4 KiB one.)
#define NBIF_V6_3_1_PAGE_SIZE                 0x1000u
#define NBIF_V6_3_1_MMIO_REG_HOLE_OFFSET      (0x80000u - NBIF_V6_3_1_PAGE_SIZE)

// include/uapi/linux/kfd_ioctl.h:745-746 — byte offsets inside that page.
#define KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL     0u
#define KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL     4u

// ===========================================================================
// gc_doorbell_init literals (nbif_v6_3_1.c:299-307)
// ===========================================================================
//
// Linux writes bare literals here (no REG_SET_FIELD), so the values are
// reproduced verbatim and decoded in the log at run time:
//
//   ENTRY_0 = 0x30000007  ENABLE=1  AWID=0x3  FENCE=0  RANGE_OFFSET=0
//                         RANGE_SIZE=0  64BIT_DIS=0  DEDUCT=0  DROP=0
//                         AWADDR_31_28=0x3        <- GFX / HQD doorbells
//   ENTRY_3 = 0x3000000d  ENABLE=1  AWID=0x6  (rest as above)
//                         AWADDR_31_28=0x3        <- MES / compute doorbells
//
// (0x07 = bit0 ENABLE plus bits5:1 = 3; 0x0d = bit0 ENABLE plus bits5:1 = 6.)
// RANGE_SIZE=0 with ENABLE=1 is upstream's value: these two ports are routed
// by AWID, and the per-ring window is gated engine-side by
// CP_RB_DOORBELL_RANGE_LOWER/UPPER instead.
#define NBIF_V6_3_1_GC_DOORBELL_ENTRY_0_VALUE  0x30000007u
#define NBIF_V6_3_1_GC_DOORBELL_ENTRY_3_VALUE  0x3000000du

// SDMA0 routing constants (nbif_v6_3_1.c:158-199).
#define NBIF_V6_3_1_SDMA0_AWID                 0xeu
#define NBIF_V6_3_1_SDMA0_AWADDR_31_28         0x3u

namespace amdgpu {

// --- 1 ---------------------------------------------------------------------
// nbif_v6_3_1_enable_doorbell_aperture (nbif_v6_3_1.c:309).
// Read-modify-write of RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN.BIF_DOORBELL_APER_EN.
// Without this the BAR2 doorbell aperture is not decoded at all.
kern_return_t nbif_v6_3_1_enable_doorbell_aperture(DeviceContext &dev, bool enable);

// --- 2 ---------------------------------------------------------------------
// nbif_v6_3_1_enable_doorbell_selfring_aperture (nbif_v6_3_1.c:316).
// CNTL = EN 1 | MODE 1 | SIZE 0 and BASE_LOW/HIGH = the BAR2 *physical* address
// (Linux: adev->doorbell.base). Only written when enabling, exactly as upstream.
kern_return_t nbif_v6_3_1_enable_doorbell_selfring_aperture(DeviceContext &dev,
                                                            bool enable,
                                                            uint64_t bar2Phys);

// --- 3 ---------------------------------------------------------------------
// nbif_v6_3_1_gc_doorbell_init (nbif_v6_3_1.c:297). S2A entries 0 and 3.
kern_return_t nbif_v6_3_1_gc_doorbell_init(DeviceContext &dev);

// --- 4 ---------------------------------------------------------------------
// nbif_v6_3_1_sdma_doorbell_range (nbif_v6_3_1.c:153). S2A entry 2, instance 0
// only (upstream's whole body is inside `if (instance == 0)`; sdma_v7_0.c:559
// likewise calls it only for i == 0). Exported for the SDMA stage; NOT called
// by nbif_v6_3_1_doorbell_path_init.
kern_return_t nbif_v6_3_1_sdma_doorbell_range(DeviceContext &dev, int instance,
                                              bool use_doorbell,
                                              int doorbell_index,
                                              int doorbell_size);

// --- 5 ---------------------------------------------------------------------
// nbif_v6_3_1_remap_hdp_registers (nbif_v6_3_1.c:135) + set_reg_remap.
kern_return_t nbif_v6_3_1_remap_hdp_registers(DeviceContext &dev);

// Current raw value of regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL (0xFFFFFFFF if the
// NBIO base segment is unresolved or the register does not respond). Pure
// accessor: logs once per boot, so it is safe to call from a flush path.
uint32_t nbif_v6_3_1_read_hdp_remap(DeviceContext &dev);

// --- 6 ---------------------------------------------------------------------
// nbif_v6_3_1_init_registers (nbif_v6_3_1.c:480) — clear
// RCC_DEV0_EPF2_STRAP2.STRAP_NO_SOFT_RESET_DEV0_F2. BASE_IDX 5 => SMN indirect.
kern_return_t nbif_v6_3_1_init_registers(DeviceContext &dev);

// --- 7 ---------------------------------------------------------------------
// Everything Linux does before any engine doorbell is rung, in its order:
//   soc24_common_early_init  -> set_reg_remap  (folded into remap_hdp_registers)
//   soc24_common_hw_init     -> init_registers
//                            -> remap_hdp_registers
//                            -> enable_doorbell_aperture(true)
//   soc24_common_late_init   -> enable_doorbell_selfring_aperture(true)
//   gfx_v12_0_hw_init        -> gc_doorbell_init   (before rlc/cp resume)
// `bar2Phys` is the physical (bus) base of the 2 MiB BAR2 doorbell aperture.
kern_return_t nbif_v6_3_1_doorbell_path_init(DeviceContext &dev, uint64_t bar2Phys);

} // namespace amdgpu
