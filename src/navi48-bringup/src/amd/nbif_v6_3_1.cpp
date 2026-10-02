//
//  nbif_v6_3_1.cpp — port of Linux drivers/gpu/drm/amd/amdgpu/nbif_v6_3_1.c
//  (AMD, MIT; see the licence excerpt and the full "Deviations from the
//  reference" list at the top of nbif_v6_3_1.h).
//
//  Register offsets, BASE_IDX values and field layouts come from
//  ref/linux-asic-reg/nbif_6_3_1_offset.h and nbif_6_3_1_sh_mask.h; those
//  headers are the truth, not the comments in amdgpu_ip.h (two of which are
//  wrong — see regRCC_DEV0_EPF2_STRAP2 in nbif_v6_3_1.h).
//
#include "nbif_v6_3_1.h"

namespace amdgpu {
namespace {

// ---------------------------------------------------------------------------
// Register plumbing
// ---------------------------------------------------------------------------
//
// An NBIF register is (BASE_IDX, offset). Discovery gives us NBIO's six base
// segments; the absolute dword index is base[BASE_IDX] + offset. BAR5 on this
// card is 512 KiB (0x20000 dwords), so anything at or past that index must go
// through the SMN index/data window instead of a direct MMIO access.

struct NbifReg {
    const char *name;
    int         baseIdx;
    uint32_t    offset;
};

struct NbifAddr {
    uint32_t abs   { 0 };       // absolute dword index into the register space
    bool     valid { false };   // discovery resolved this NBIO base segment
    bool     bar5  { false };   // reachable by direct MMIO through BAR5
};

NbifAddr nbif_addr(const DeviceContext &dev, const NbifReg &r) {
    NbifAddr a;
    if (!dev.ip.isResolved(IPBlock::NBIO, r.baseIdx))
        return a;
    a.abs   = SOC15_REG_OFFSET_BIDX(dev, IPBlock::NBIO, r.baseIdx, r.offset);
    a.valid = true;
    a.bar5  = dev.rmmio != nullptr &&
              ((uint64_t)a.abs * 4ull + 4ull) <= (uint64_t)dev.rmmioSize;
    return a;
}

uint32_t nbif_rd(const DeviceContext &dev, const NbifAddr &a) {
    if (!a.valid) return 0xFFFFFFFFu;
    return a.bar5 ? RREG32(dev, a.abs) : SMN_RREG32(dev, a.abs);
}

void nbif_wr(const DeviceContext &dev, const NbifAddr &a, uint32_t v) {
    if (!a.valid) return;
    if (a.bar5) WREG32(dev, a.abs, v);
    else        SMN_WREG32(dev, a.abs, v);
}

// Resolve + log the address + read the pre-write value. false => the NBIO base
// segment this register needs was never filled in by discovery.
bool nbif_begin(const DeviceContext &dev, const NbifReg &r,
                NbifAddr &a, uint32_t &before) {
    a = nbif_addr(dev, r);
    if (!a.valid) {
        before = 0xFFFFFFFFu;
        NBIF_LOG("%s: NBIO BASE_IDX %d unresolved in discovery -- skipped",
                 r.name, r.baseIdx);
        return false;
    }
    before = nbif_rd(dev, a);
    NBIF_LOG("%s bidx=%d off=0x%04x abs=0x%08x byte=0x%llx via=%s: before=0x%08x",
             r.name, r.baseIdx, r.offset, a.abs,
             (unsigned long long)a.abs * 4ull, a.bar5 ? "BAR5" : "SMN", before);
    return true;
}

// Write, read back, log, compare under checkMask.
kern_return_t nbif_commit(const DeviceContext &dev, const NbifReg &r,
                          const NbifAddr &a, uint32_t before,
                          uint32_t value, uint32_t checkMask) {
    nbif_wr(dev, a, value);
    const uint32_t after = nbif_rd(dev, a);
    const bool ok = (((after ^ value) & checkMask) == 0u);
    NBIF_LOG("%s: 0x%08x -> wrote 0x%08x -> after=0x%08x (check 0x%08x) %s",
             r.name, before, value, after, checkMask, ok ? "OK" : "MISMATCH");
    return ok ? kIOReturnSuccess : kIOReturnIOError;
}

// nbif_v6_3_1.c switches to the `_nbif_4_10` register table (BASE_IDX 3) when
// amdgpu_ip_version(adev, NBIO_HWIP, 0) >= IP_VERSION(7, 11, 4). Navi 48
// reports NBIF 6.3.1, which is below that, so the direct BASE_IDX 2 table is
// selected — but the branch is real, not hardcoded.
bool nbif_use_4_10(const DeviceContext &dev) {
    const IPVersion v = dev.ip.getVersion(IPBlock::NBIO);
    const uint32_t pack = ((uint32_t)v.major << 16) |
                          ((uint32_t)v.minor << 8) | (uint32_t)v.rev;
    const uint32_t v7_11_4 = (7u << 16) | (11u << 8) | 4u;
    return pack >= v7_11_4;
}

// Decode one S2A_DOORBELL_ENTRY_x_CTRL word. The bit layout is identical for
// every port, so entry 0's field names stand in for all of them.
void nbif_log_s2a(const char *what, uint32_t v) {
    NBIF_LOG("  %s = 0x%08x: ENABLE=%u AWID=0x%x FENCE=%u RANGE_OFFSET=0x%x "
             "RANGE_SIZE=0x%x 64BIT_DIS=%u DEDUCT=%u DROP=%u AWADDR_31_28=0x%x",
             what, v,
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_ENABLE),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_AWID),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_FENCE_ENABLE),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_RANGE_OFFSET),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_RANGE_SIZE),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_64BIT_SUPPORT_DIS),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_NEED_DEDUCT_RANGE_OFFSET),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_DROP_EN),
             REG_GET_FIELD(v, GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, S2A_DOORBELL_PORT0_AWADDR_31_28_VALUE));
}

// The three GDC entries this file touches, per register table.
NbifReg nbif_s2a_entry0(const DeviceContext &dev) {
    if (nbif_use_4_10(dev))
        return NbifReg{ "GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10",
                        regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10_BASE_IDX,
                        regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_nbif_4_10 };
    return NbifReg{ "GDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL",
                    regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL_BASE_IDX,
                    regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL };
}
NbifReg nbif_s2a_entry2(const DeviceContext &dev) {
    if (nbif_use_4_10(dev))
        return NbifReg{ "GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10",
                        regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10_BASE_IDX,
                        regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_nbif_4_10 };
    return NbifReg{ "GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL",
                    regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL_BASE_IDX,
                    regGDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL };
}
NbifReg nbif_s2a_entry3(const DeviceContext &dev) {
    if (nbif_use_4_10(dev))
        return NbifReg{ "GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10",
                        regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10_BASE_IDX,
                        regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_nbif_4_10 };
    return NbifReg{ "GDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL",
                    regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL_BASE_IDX,
                    regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL };
}

constexpr NbifReg kRegDoorbellAperEn = {
    "RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN",
    regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN_BASE_IDX,
    regRCC_DEV0_EPF0_RCC_DOORBELL_APER_EN
};
constexpr NbifReg kRegSelfringCntl = {
    "BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL",
    regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL_BASE_IDX,
    regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL
};
constexpr NbifReg kRegSelfringBaseLow = {
    "BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW",
    regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW_BASE_IDX,
    regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW
};
constexpr NbifReg kRegSelfringBaseHigh = {
    "BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH",
    regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH_BASE_IDX,
    regBIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH
};
constexpr NbifReg kRegRemapHdpMem = {
    "BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL",
    regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL_BASE_IDX,
    regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL
};
constexpr NbifReg kRegRemapHdpReg = {
    "BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL",
    regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL_BASE_IDX,
    regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL
};
constexpr NbifReg kRegStrap2 = {
    "RCC_DEV0_EPF2_STRAP2",
    regRCC_DEV0_EPF2_STRAP2_BASE_IDX,
    regRCC_DEV0_EPF2_STRAP2
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// 1. Doorbell aperture enable
// ---------------------------------------------------------------------------
//
// Linux (nbif_v6_3_1.c:309):
//     WREG32_FIELD15_PREREG(NBIO, 0, RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN,
//                           BIF_DOORBELL_APER_EN, enable ? 1 : 0);
// WREG32_FIELD15_PREREG is a read-modify-write of that one bit.
kern_return_t nbif_v6_3_1_enable_doorbell_aperture(DeviceContext &dev, bool enable) {
    NbifAddr a;
    uint32_t before = 0;
    if (!nbif_begin(dev, kRegDoorbellAperEn, a, before))
        return kIOReturnNoDevice;
    if (before == 0xFFFFFFFFu) {
        NBIF_LOG("enable_doorbell_aperture: pre-read is all-ones (no response) "
                 "-- refusing the read-modify-write");
        return kIOReturnIOError;
    }

    const uint32_t value = REG_SET_FIELD(before,
                                         RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN,
                                         BIF_DOORBELL_APER_EN, enable ? 1u : 0u);
    const kern_return_t kr =
        nbif_commit(dev, kRegDoorbellAperEn, a, before, value,
                    RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN__BIF_DOORBELL_APER_EN_MASK);
    NBIF_LOG("enable_doorbell_aperture(%s): BIF_DOORBELL_APER_EN now %u",
             enable ? "true" : "false",
             REG_GET_FIELD(nbif_rd(dev, a), RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN,
                           BIF_DOORBELL_APER_EN));
    return kr;
}

// ---------------------------------------------------------------------------
// 2. Self-ring doorbell aperture
// ---------------------------------------------------------------------------
//
// Linux (nbif_v6_3_1.c:316):
//     u32 tmp = 0;
//     if (enable) {
//         tmp = REG_SET_FIELD(tmp, ..., DOORBELL_SELFRING_GPA_APER_EN,   1) |
//               REG_SET_FIELD(tmp, ..., DOORBELL_SELFRING_GPA_APER_MODE, 1) |
//               REG_SET_FIELD(tmp, ..., DOORBELL_SELFRING_GPA_APER_SIZE, 0);
//         WREG32 ..._BASE_LOW,  lower_32_bits(adev->doorbell.base);
//         WREG32 ..._BASE_HIGH, upper_32_bits(adev->doorbell.base);
//     }
//     WREG32 ..._CNTL, tmp;              // written in both directions
//
// Each REG_SET_FIELD folds into tmp == 0, so the enabled CNTL word is
// 0x1 | 0x2 | 0x0 == 0x3: EN=1, MODE=1 (GPA mode), SIZE=0.
// adev->doorbell.base is the BAR2 *physical* address; we take it as bar2Phys.
kern_return_t nbif_v6_3_1_enable_doorbell_selfring_aperture(DeviceContext &dev,
                                                            bool enable,
                                                            uint64_t bar2Phys) {
    NBIF_LOG("enable_doorbell_selfring_aperture(%s): bar2Phys=0x%llx "
             "bar2Size=%llu KiB bar2Mapped=%s",
             enable ? "true" : "false", (unsigned long long)bar2Phys,
             (unsigned long long)(dev.bar2Size >> 10),
             dev.bar2 ? "yes" : "no");

    if (enable && bar2Phys == 0) {
        NBIF_LOG("enable_doorbell_selfring_aperture: bar2Phys is 0 -- refusing "
                 "to point the self-ring aperture at physical address 0");
        return kIOReturnBadArgument;
    }

    kern_return_t kr = kIOReturnSuccess;

    if (enable) {
        // BASE_LOW / BASE_HIGH first, as upstream does, so the aperture is
        // fully described before CNTL turns it on.
        NbifAddr aLow;
        uint32_t beforeLow = 0;
        if (!nbif_begin(dev, kRegSelfringBaseLow, aLow, beforeLow))
            return kIOReturnNoDevice;
        const uint32_t low = (uint32_t)(bar2Phys & 0xFFFFFFFFull);
        kern_return_t k = nbif_commit(dev, kRegSelfringBaseLow, aLow, beforeLow, low,
                                      0xFFFFFFFFu);
        if (k != kIOReturnSuccess) kr = k;

        NbifAddr aHigh;
        uint32_t beforeHigh = 0;
        if (!nbif_begin(dev, kRegSelfringBaseHigh, aHigh, beforeHigh))
            return kIOReturnNoDevice;
        const uint32_t high = (uint32_t)(bar2Phys >> 32);
        k = nbif_commit(dev, kRegSelfringBaseHigh, aHigh, beforeHigh, high,
                        0xFFFFFFFFu);
        if (k != kIOReturnSuccess) kr = k;
    }

    uint32_t tmp = 0;
    if (enable) {
        tmp = REG_SET_FIELD(tmp, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL,
                            DOORBELL_SELFRING_GPA_APER_EN, 1u) |
              REG_SET_FIELD(tmp, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL,
                            DOORBELL_SELFRING_GPA_APER_MODE, 1u) |
              REG_SET_FIELD(tmp, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL,
                            DOORBELL_SELFRING_GPA_APER_SIZE, 0u);
    }

    NbifAddr aCntl;
    uint32_t beforeCntl = 0;
    if (!nbif_begin(dev, kRegSelfringCntl, aCntl, beforeCntl))
        return kIOReturnNoDevice;

    constexpr uint32_t kCntlDefined =
        BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_EN_MASK |
        BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_MODE_MASK |
        BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL__DOORBELL_SELFRING_GPA_APER_SIZE_MASK;

    const kern_return_t kCntl =
        nbif_commit(dev, kRegSelfringCntl, aCntl, beforeCntl, tmp, kCntlDefined);
    if (kCntl != kIOReturnSuccess) kr = kCntl;

    const uint32_t rb = nbif_rd(dev, aCntl);
    NBIF_LOG("  SELFRING_GPA_APER_CNTL = 0x%08x: EN=%u MODE=%u SIZE=0x%x", rb,
             REG_GET_FIELD(rb, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL,
                           DOORBELL_SELFRING_GPA_APER_EN),
             REG_GET_FIELD(rb, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL,
                           DOORBELL_SELFRING_GPA_APER_MODE),
             REG_GET_FIELD(rb, BIF_BX_PF0_DOORBELL_SELFRING_GPA_APER_CNTL,
                           DOORBELL_SELFRING_GPA_APER_SIZE));
    return kr;
}

// ---------------------------------------------------------------------------
// 3. GC doorbell routing
// ---------------------------------------------------------------------------
//
// Linux (nbif_v6_3_1.c:297) writes two bare literals:
//     WREG32_SOC15(NBIO, 0, regGDC_S2A0_S2A_DOORBELL_ENTRY_0_CTRL, 0x30000007);
//     WREG32_SOC15(NBIO, 0, regGDC_S2A0_S2A_DOORBELL_ENTRY_3_CTRL, 0x3000000d);
// Decoded (see nbif_v6_3_1.h): entry 0 = ENABLE 1 / AWID 0x3 / AWADDR_31_28 0x3
// (GFX + HQD doorbells), entry 3 = ENABLE 1 / AWID 0x6 / AWADDR_31_28 0x3
// (MES + compute doorbells). Every other field is 0 in both, including
// RANGE_OFFSET and RANGE_SIZE — these two ports route by AWID and the per-ring
// window is enforced engine-side by CP_RB_DOORBELL_RANGE_LOWER/UPPER.
kern_return_t nbif_v6_3_1_gc_doorbell_init(DeviceContext &dev) {
    const IPVersion v = dev.ip.getVersion(IPBlock::NBIO);
    NBIF_LOG("gc_doorbell_init: NBIO/NBIF version %u.%u.%u -> %s register table",
             (unsigned)v.major, (unsigned)v.minor, (unsigned)v.rev,
             nbif_use_4_10(dev) ? "_nbif_4_10 (BASE_IDX 3)" : "direct (BASE_IDX 2)");

    const NbifReg e0 = nbif_s2a_entry0(dev);
    const NbifReg e3 = nbif_s2a_entry3(dev);

    kern_return_t kr = kIOReturnSuccess;

    NbifAddr a0;
    uint32_t before0 = 0;
    if (!nbif_begin(dev, e0, a0, before0)) return kIOReturnNoDevice;
    nbif_log_s2a("ENTRY_0 before", before0);
    kern_return_t k = nbif_commit(dev, e0, a0, before0,
                                  NBIF_V6_3_1_GC_DOORBELL_ENTRY_0_VALUE, 0xFFFFFFFFu);
    nbif_log_s2a("ENTRY_0 after ", nbif_rd(dev, a0));
    if (k != kIOReturnSuccess) kr = k;

    NbifAddr a3;
    uint32_t before3 = 0;
    if (!nbif_begin(dev, e3, a3, before3)) return kIOReturnNoDevice;
    nbif_log_s2a("ENTRY_3 before", before3);
    k = nbif_commit(dev, e3, a3, before3,
                    NBIF_V6_3_1_GC_DOORBELL_ENTRY_3_VALUE, 0xFFFFFFFFu);
    nbif_log_s2a("ENTRY_3 after ", nbif_rd(dev, a3));
    if (k != kIOReturnSuccess) kr = k;

    return kr;
}

// ---------------------------------------------------------------------------
// 4. SDMA doorbell range
// ---------------------------------------------------------------------------
//
// Linux (nbif_v6_3_1.c:153). The entire body is inside `if (instance == 0)`,
// and sdma_v7_0.c:559 only calls it for i == 0, so there is no SDMA1 entry on
// NBIF 6.3.1: both SDMA engines are covered by S2A entry 2's
// [RANGE_OFFSET, RANGE_OFFSET + RANGE_SIZE) window (sdma_v7_0.c passes
// doorbell_size = doorbell_index.sdma_doorbell_range * sdma.num_instances).
//
//     ENABLE            = 1
//     AWID              = 0xe
//     RANGE_OFFSET      = doorbell_index
//     RANGE_SIZE        = doorbell_size
//     AWADDR_31_28_VALUE= 0x3
// and when !use_doorbell only RANGE_SIZE is cleared (ENABLE is left alone).
kern_return_t nbif_v6_3_1_sdma_doorbell_range(DeviceContext &dev, int instance,
                                              bool use_doorbell,
                                              int doorbell_index,
                                              int doorbell_size) {
    if (instance != 0) {
        NBIF_LOG("sdma_doorbell_range: instance %d ignored -- nbif_v6_3_1 "
                 "programs S2A entry 2 for instance 0 only (upstream); entry 2's "
                 "RANGE covers every SDMA engine", instance);
        return kIOReturnSuccess;
    }

    const NbifReg e2 = nbif_s2a_entry2(dev);
    NbifAddr a2;
    uint32_t before = 0;
    if (!nbif_begin(dev, e2, a2, before)) return kIOReturnNoDevice;
    nbif_log_s2a("ENTRY_2 before", before);

    if (before == 0xFFFFFFFFu) {
        NBIF_LOG("sdma_doorbell_range: pre-read is all-ones (no response) -- "
                 "refusing the read-modify-write");
        return kIOReturnIOError;
    }

    uint32_t doorbell_range = before;
    if (use_doorbell) {
        doorbell_range = REG_SET_FIELD(doorbell_range,
                                       GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
                                       S2A_DOORBELL_PORT2_ENABLE, 0x1u);
        doorbell_range = REG_SET_FIELD(doorbell_range,
                                       GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
                                       S2A_DOORBELL_PORT2_AWID,
                                       NBIF_V6_3_1_SDMA0_AWID);
        doorbell_range = REG_SET_FIELD(doorbell_range,
                                       GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
                                       S2A_DOORBELL_PORT2_RANGE_OFFSET,
                                       (uint32_t)doorbell_index);
        doorbell_range = REG_SET_FIELD(doorbell_range,
                                       GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
                                       S2A_DOORBELL_PORT2_RANGE_SIZE,
                                       (uint32_t)doorbell_size);
        doorbell_range = REG_SET_FIELD(doorbell_range,
                                       GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
                                       S2A_DOORBELL_PORT2_AWADDR_31_28_VALUE,
                                       NBIF_V6_3_1_SDMA0_AWADDR_31_28);
    } else {
        doorbell_range = REG_SET_FIELD(doorbell_range,
                                       GDC_S2A0_S2A_DOORBELL_ENTRY_2_CTRL,
                                       S2A_DOORBELL_PORT2_RANGE_SIZE, 0u);
    }

    NBIF_LOG("sdma_doorbell_range: instance 0 use_doorbell=%s index=0x%x size=0x%x",
             use_doorbell ? "true" : "false", (unsigned)doorbell_index,
             (unsigned)doorbell_size);
    const kern_return_t kr =
        nbif_commit(dev, e2, a2, before, doorbell_range, 0xFFFFFFFFu);
    nbif_log_s2a("ENTRY_2 after ", nbif_rd(dev, a2));
    return kr;
}

// ---------------------------------------------------------------------------
// 5. HDP flush-register remap
// ---------------------------------------------------------------------------
//
// Linux (nbif_v6_3_1.c:135):
//     WREG32_SOC15(NBIO, 0, regBIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL,
//                  adev->rmmio_remap.reg_offset + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL);
//     WREG32_SOC15(NBIO, 0, regBIF_BX0_REMAP_HDP_REG_FLUSH_CNTL,
//                  adev->rmmio_remap.reg_offset + KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL);
// with rmmio_remap.reg_offset = MMIO_REG_HOLE_OFFSET = 0x80000 - PAGE_SIZE
// (nbif_v6_3_1_set_reg_remap, called from soc24_common_early_init). PAGE_SIZE
// is 4096 on x86_64 macOS, so the values are 0x0007F000 and 0x0007F004: the
// last 4 KiB page of the 512 KiB register BAR, which is the architectural MMIO
// register hole and therefore collides with no real register block.
//
// The register's only field is ADDRESS at bits 18:2, i.e. it stores the BAR5
// byte offset with bits 1:0 implied zero, which is why Linux writes the byte
// offset directly instead of going through REG_SET_FIELD.
kern_return_t nbif_v6_3_1_remap_hdp_registers(DeviceContext &dev) {
    const uint32_t hole    = NBIF_V6_3_1_MMIO_REG_HOLE_OFFSET;
    const uint32_t memVal  = hole + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL;
    const uint32_t regVal  = hole + KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL;

    NBIF_LOG("remap_hdp_registers: MMIO_REG_HOLE_OFFSET=0x%08x (0x80000 - 4096), "
             "MEM_FLUSH<-0x%08x REG_FLUSH<-0x%08x; rmmio=%s rmmioSize=%llu bytes",
             hole, memVal, regVal, dev.rmmio ? "mapped" : "null",
             (unsigned long long)dev.rmmioSize);

    if ((uint64_t)hole + 8ull > (uint64_t)dev.rmmioSize) {
        // Not fatal: the hardware BAR really is 512 KiB, we just did not map all
        // of it, so amdgpu_hdp_flush()'s own bounds check would drop the write.
        NBIF_LOG("remap_hdp_registers: WARNING remap window 0x%08x is outside the "
                 "mapped register window (%llu bytes) -- later HDP flushes will "
                 "be dropped by the bounds check in amdgpu_hdp_flush",
                 hole, (unsigned long long)dev.rmmioSize);
    } else {
        // Probe: whatever is in the hole today. A real register block here
        // would be a reason not to use Linux's offset.
        NBIF_LOG("remap_hdp_registers: BAR5+0x%08x currently reads 0x%08x, "
                 "BAR5+0x%08x reads 0x%08x (expect 0/~0 for an unused hole)",
                 memVal, RREG32(dev, memVal / 4u),
                 regVal, RREG32(dev, regVal / 4u));
    }

    kern_return_t kr = kIOReturnSuccess;

    NbifAddr aMem;
    uint32_t beforeMem = 0;
    if (!nbif_begin(dev, kRegRemapHdpMem, aMem, beforeMem))
        return kIOReturnNoDevice;
    kern_return_t k = nbif_commit(dev, kRegRemapHdpMem, aMem, beforeMem, memVal,
                                  BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL__ADDRESS_MASK);
    if (k != kIOReturnSuccess) kr = k;

    NbifAddr aReg;
    uint32_t beforeReg = 0;
    if (!nbif_begin(dev, kRegRemapHdpReg, aReg, beforeReg))
        return kIOReturnNoDevice;
    k = nbif_commit(dev, kRegRemapHdpReg, aReg, beforeReg, regVal,
                    BIF_BX0_REMAP_HDP_REG_FLUSH_CNTL__ADDRESS_MASK);
    if (k != kIOReturnSuccess) kr = k;

    if (kr != kIOReturnSuccess) {
        NBIF_LOG("remap_hdp_registers: readback mismatch. 0x012d/BASE_IDX 2 is "
                 "confirmed against nbif_6_3_1_offset.h:1501 and nbif_v6_3_1.c's "
                 "IP_VERSION(7,11,5) alias resolves to the same address, so there "
                 "is no other offset to try -- the register is not responding on "
                 "this path");
    }
    return kr;
}

uint32_t nbif_v6_3_1_read_hdp_remap(DeviceContext &dev) {
    const NbifAddr a = nbif_addr(dev, kRegRemapHdpMem);
    const uint32_t v = nbif_rd(dev, a);
    static bool logged = false;
    if (!logged) {
        logged = true;
        NBIF_LOG("read_hdp_remap: %s bidx=%d off=0x%04x abs=0x%08x via=%s = 0x%08x",
                 kRegRemapHdpMem.name, kRegRemapHdpMem.baseIdx,
                 kRegRemapHdpMem.offset, a.abs,
                 a.valid ? (a.bar5 ? "BAR5" : "SMN") : "unresolved", v);
    }
    return v;
}

// ---------------------------------------------------------------------------
// 6. init_registers
// ---------------------------------------------------------------------------
//
// Linux (nbif_v6_3_1.c:480):
//     data  = RREG32_SOC15(NBIO, 0, regRCC_DEV0_EPF2_STRAP2);
//     data &= ~RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK;
//     WREG32_SOC15(NBIO, 0, regRCC_DEV0_EPF2_STRAP2, data);
//
// The register DOES exist in nbif_6_3_1_offset.h (line 6985) but at 0xd102 /
// BASE_IDX 5 with the mask at bit 7 — not 0x009A / BASE_IDX 2 / bit 1 as the
// comment in amdgpu_ip.h says. BASE_IDX 5 puts the absolute dword index far
// past BAR5's 0x20000 dwords, so the access goes through SMN_RREG32 /
// SMN_WREG32; nbif_addr() picks that automatically and the log says which.
kern_return_t nbif_v6_3_1_init_registers(DeviceContext &dev) {
    NbifAddr a;
    uint32_t before = 0;
    if (!nbif_begin(dev, kRegStrap2, a, before)) {
        NBIF_LOG("init_registers: skipped (NBIO BASE_IDX %d unresolved)",
                 kRegStrap2.baseIdx);
        return kIOReturnNoDevice;
    }
    if (before == 0xFFFFFFFFu) {
        NBIF_LOG("init_registers: RCC_DEV0_EPF2_STRAP2 pre-read is all-ones "
                 "(no response via %s) -- skipping the strap clear rather than "
                 "writing garbage into a strap register", a.bar5 ? "BAR5" : "SMN");
        return kIOReturnIOError;
    }

    const uint32_t value =
        before & ~(uint32_t)RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK;
    NBIF_LOG("init_registers: clearing STRAP_NO_SOFT_RESET_DEV0_F2 (bit 7, "
             "mask 0x%08x); was %u",
             (uint32_t)RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK,
             REG_GET_FIELD(before, RCC_DEV0_EPF2_STRAP2, STRAP_NO_SOFT_RESET_DEV0_F2));
    return nbif_commit(dev, kRegStrap2, a, before, value,
                       RCC_DEV0_EPF2_STRAP2__STRAP_NO_SOFT_RESET_DEV0_F2_MASK);
}

// ---------------------------------------------------------------------------
// 7. Full doorbell-path bring-up
// ---------------------------------------------------------------------------
kern_return_t nbif_v6_3_1_doorbell_path_init(DeviceContext &dev, uint64_t bar2Phys) {
    const IPVersion v = dev.ip.getVersion(IPBlock::NBIO);
    NBIF_LOG("doorbell_path_init: NBIF %u.%u.%u, bar2Phys=0x%llx bar2Size=%llu KiB, "
             "rmmioSize=%llu bytes (dword index >= 0x%x needs SMN)",
             (unsigned)v.major, (unsigned)v.minor, (unsigned)v.rev,
             (unsigned long long)bar2Phys,
             (unsigned long long)(dev.bar2Size >> 10),
             (unsigned long long)dev.rmmioSize,
             (unsigned)(dev.rmmioSize / 4));
    for (int i = 0; i < 6; i++) {
        NBIF_LOG("  NBIO base[%d] = 0x%08x%s", i, dev.ip.getBase(IPBlock::NBIO, i),
                 dev.ip.isResolved(IPBlock::NBIO, i) ? "" : "  (unresolved)");
    }

    // soc24_common_hw_init (soc24.c:436): nbio.funcs->init_registers(adev).
    // Non-fatal here: nothing in the doorbell path depends on the strap.
    kern_return_t k = nbif_v6_3_1_init_registers(dev);
    if (k != kIOReturnSuccess)
        NBIF_LOG("doorbell_path_init: init_registers failed (0x%x) -- continuing", k);

    // soc24_common_hw_init (soc24.c:441): nbio.funcs->remap_hdp_registers(adev).
    // Non-fatal: this only arms the HDP flush window, not doorbell routing.
    k = nbif_v6_3_1_remap_hdp_registers(dev);
    if (k != kIOReturnSuccess)
        NBIF_LOG("doorbell_path_init: remap_hdp_registers failed (0x%x) -- "
                 "continuing; HDP flushes stay disabled", k);

    // soc24_common_hw_init (soc24.c:447): enable_doorbell_aperture(adev, true).
    k = nbif_v6_3_1_enable_doorbell_aperture(dev, true);
    if (k != kIOReturnSuccess) {
        NBIF_LOG("doorbell_path_init: enable_doorbell_aperture failed (0x%x) -- "
                 "BAR2 doorbell writes will not be decoded", k);
        return k;
    }

    // soc24_common_late_init (soc24.c:422): enable_doorbell_selfring_aperture.
    // Upstream comment: done late because the doorbell BAR aperture changes if
    // the BAR resize in gmc sw_init succeeds.
    k = nbif_v6_3_1_enable_doorbell_selfring_aperture(dev, true, bar2Phys);
    if (k != kIOReturnSuccess) {
        NBIF_LOG("doorbell_path_init: enable_doorbell_selfring_aperture failed "
                 "(0x%x) -- doorbell writes will be decoded but not routed", k);
        return k;
    }

    // gfx_v12_0_hw_init (gfx_v12_0.c:3835), between constants_init and
    // rlc_resume/cp_resume.
    k = nbif_v6_3_1_gc_doorbell_init(dev);
    if (k != kIOReturnSuccess) {
        NBIF_LOG("doorbell_path_init: gc_doorbell_init failed (0x%x) -- GFX/MES "
                 "doorbells are not routed to the engines", k);
        return k;
    }

    NBIF_LOG("doorbell_path_init: complete. SDMA S2A entry 2 is left to the SDMA "
             "stage (call nbif_v6_3_1_sdma_doorbell_range).");
    return kIOReturnSuccess;
}

} // namespace amdgpu
