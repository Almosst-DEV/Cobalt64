//
//  amdgpu_regs.h — Navi48Bringup's kernel-side implementation of mac-amdgpu's
//  register/VRAM access API (same names, same semantics), so IP-block modules
//  ported from lemonade-sdk/mac-amdgpu (MIT) compile with minimal edits.
//
//  Differences from the DriverKit original:
//    * DeviceContext holds direct MMIO pointers (BAR5 registers, BAR0 VRAM
//      aperture, BAR2 doorbells) instead of IOPCIDevice memory indices.
//    * bar0_* helpers write through a write-combining mapping and fence.
//    * All bring-up VRAM lives at ctx.vramBase (above the UEFI console
//      framebuffer); MC address of VRAM byte offset X is ctx.vramMcBase + X.
//
#pragma once
#include <stdint.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOReturn.h>
#include "amdgpu_ip.h"

// Cast-clean version of the reference macro (identical arithmetic; avoids
// -Wsign-conversion noise from int-typed mask/shift constants).
#define REG_SET_FIELD(value, reg, field, val) \
    ((((uint32_t)(value)) & ~((uint32_t)(reg##__##field##_MASK))) | \
     ((((uint32_t)(val)) << ((uint32_t)(reg##__##field##__SHIFT))) & \
      ((uint32_t)(reg##__##field##_MASK))))
#define REG_GET_FIELD(value, reg, field) \
    ((((uint32_t)(value)) & ((uint32_t)(reg##__##field##_MASK))) >> ((uint32_t)(reg##__##field##__SHIFT)))

extern "C" void *memcpy(void *, const void *, size_t);

namespace amdgpu {

struct DeviceContext {
    // --- MMIO windows (mapped by the kext) ---
    volatile uint32_t *rmmio     { nullptr };   // BAR5 registers, dword-indexed
    size_t             rmmioSize { 0 };         // bytes
    volatile uint8_t  *bar0      { nullptr };   // VRAM aperture (write-combining)
    size_t             bar0Size  { 0 };         // bytes actually mapped
    uint64_t           bar0Phys  { 0 };
    volatile uint8_t  *bar2      { nullptr };   // doorbells (mapped on demand)
    size_t             bar2Size  { 0 };
    uint64_t           bar2Phys  { 0 };         // physical base of the doorbell BAR (NBIO selfring aperture)

    // --- VRAM geometry ---
    uint64_t vramSizeBytes { 0 };   // RCC_CONFIG_MEMSIZE
    uint64_t vramMcBase    { 0 };   // MMHUB FB_LOCATION_BASE << 24 (0x8000000000 here)
    uint64_t vramBase      { 0 };   // byte offset of the bring-up region (8 MiB here)
    uint64_t vramLimit     { 0 };   // byte offset one past usable aperture (= bar0Size)

    // --- IP register bases from on-die discovery ---
    IPBaseTable ip;

    // --- state flags kept for API compatibility with the reference ---
    bool psoCAlive { false };   // (sic) SOS alive
    bool smuOnline { false };
    bool gmcReady  { false };
    DoorbellState doorbell {};   // BAR2 doorbell map (amdgpu_ip.h), filled by the doorbell module

    uint64_t vramMC(uint64_t vramByteOffset) const { return vramMcBase + vramByteOffset; }
};

// ---- register addressing (absolute dword index into BAR5) ----------------------
static inline uint32_t SOC15_REG_OFFSET(const DeviceContext &ctx, IPBlock block, uint32_t reg) {
    return ctx.ip.get(block) + reg;
}
static inline uint32_t SOC15_REG_OFFSET_BIDX(const DeviceContext &ctx, IPBlock block, int baseIdx, uint32_t reg) {
    return ctx.ip.getBase(block, baseIdx) + reg;
}

static inline uint32_t RREG32(const DeviceContext &ctx, uint32_t reg) {
    if (!ctx.rmmio || (size_t)reg * 4 + 4 > ctx.rmmioSize) return 0xFFFFFFFFu;
    return ctx.rmmio[reg];
}
static inline void WREG32(const DeviceContext &ctx, uint32_t reg, uint32_t value) {
    if (!ctx.rmmio || (size_t)reg * 4 + 4 > ctx.rmmioSize) return;
    ctx.rmmio[reg] = value;
}
static inline uint32_t RREG32_abs(const DeviceContext &ctx, uint32_t reg) { return RREG32(ctx, reg); }

// ---- VRAM aperture (BAR0) ------------------------------------------------------
static inline void storeFence() { __asm__ __volatile__("sfence" ::: "memory"); }

static inline uint32_t RBAR0_32(const DeviceContext &ctx, uint64_t byte_offset) {
    if (!ctx.bar0 || byte_offset + 4 > ctx.bar0Size) return 0xFFFFFFFFu;
    return *reinterpret_cast<const volatile uint32_t *>(ctx.bar0 + byte_offset);
}
// The reference names its VRAM-aperture read RBAR2_32 (on Apple Silicon the
// aperture is BAR2); keep the name so ported code compiles.
static inline uint32_t RBAR2_32(const DeviceContext &ctx, uint64_t byte_offset) { return RBAR0_32(ctx, byte_offset); }
static inline uint64_t RBAR0_64(const DeviceContext &ctx, uint64_t byte_offset) {
    if (!ctx.bar0 || byte_offset + 8 > ctx.bar0Size) return ~0ULL;
    return *reinterpret_cast<const volatile uint64_t *>(ctx.bar0 + byte_offset);
}
static inline void WBAR0_32(const DeviceContext &ctx, uint64_t byte_offset, uint32_t value) {
    if (!ctx.bar0 || byte_offset + 4 > ctx.bar0Size) return;
    *reinterpret_cast<volatile uint32_t *>(ctx.bar0 + byte_offset) = value;
    storeFence();
}
static inline void WBAR0_64(const DeviceContext &ctx, uint64_t byte_offset, uint64_t value) {
    if (!ctx.bar0 || byte_offset + 8 > ctx.bar0Size) return;
    *reinterpret_cast<volatile uint64_t *>(ctx.bar0 + byte_offset) = value;
    storeFence();
}

static inline void bar0_memcpy_to_vram(const DeviceContext &ctx, uint64_t vram_byte_offset,
                                       const void *src, uint64_t size_bytes) {
    if (!ctx.bar0 || vram_byte_offset + size_bytes > ctx.bar0Size) return;
    const uint8_t *bytes = static_cast<const uint8_t *>(src);
    volatile uint8_t *dst = ctx.bar0 + vram_byte_offset;
    uint64_t off = 0;
    while (((vram_byte_offset + off) & 7) && off + 4 <= size_bytes) {
        uint32_t v; memcpy(&v, bytes + off, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + off) = v; off += 4;
    }
    while (off + 8 <= size_bytes) {
        uint64_t v; memcpy(&v, bytes + off, 8);
        *reinterpret_cast<volatile uint64_t *>(dst + off) = v; off += 8;
    }
    while (off + 4 <= size_bytes) {
        uint32_t v; memcpy(&v, bytes + off, 4);
        *reinterpret_cast<volatile uint32_t *>(dst + off) = v; off += 4;
    }
    if (off < size_bytes) {
        uint32_t v = 0; memcpy(&v, bytes + off, size_bytes - off);
        *reinterpret_cast<volatile uint32_t *>(dst + off) = v;
    }
    storeFence();
}

// NOTE: `pattern` is a 32-bit pattern, as in the reference (not a byte).
static inline void bar0_memset_vram(const DeviceContext &ctx, uint64_t vram_byte_offset,
                                    uint32_t pattern, uint64_t size_bytes) {
    if (!ctx.bar0 || vram_byte_offset + size_bytes > ctx.bar0Size) return;
    volatile uint8_t *dst = ctx.bar0 + vram_byte_offset;
    const uint64_t pattern64 = ((uint64_t)pattern << 32) | pattern;
    uint64_t off = 0;
    while (((vram_byte_offset + off) & 7) && off + 4 <= size_bytes) {
        *reinterpret_cast<volatile uint32_t *>(dst + off) = pattern; off += 4;
    }
    while (off + 8 <= size_bytes) {
        *reinterpret_cast<volatile uint64_t *>(dst + off) = pattern64; off += 8;
    }
    while (off + 4 <= size_bytes) {
        *reinterpret_cast<volatile uint32_t *>(dst + off) = pattern; off += 4;
    }
    storeFence();
}

// Read VRAM at any offset through the MM_INDEX/MM_DATA window (slow, whole card).
static inline uint32_t RVRAM32_via_mm(const DeviceContext &ctx, uint64_t pos) {
    constexpr uint32_t mmMM_INDEX = 0x0, mmMM_DATA = 0x1, mmMM_INDEX_HI = 0x6;
    WREG32(ctx, mmMM_INDEX_HI, (uint32_t)(pos >> 31));
    WREG32(ctx, mmMM_INDEX, ((uint32_t)pos & 0x7ffffffcu) | 0x80000000u);
    return RREG32(ctx, mmMM_DATA);
}

// Write VRAM at any offset through the same window. The BAR0 aperture only
// covers the first bar0Size bytes (256 MiB here) of a 16 GiB card, so this is
// the only CPU path to the rest of it. Three MMIO accesses per dword, so it is
// for staging, not for bulk traffic — use SDMA for that once a queue exists.
static inline void WVRAM32_via_mm(const DeviceContext &ctx, uint64_t pos, uint32_t value) {
    constexpr uint32_t mmMM_INDEX = 0x0, mmMM_DATA = 0x1, mmMM_INDEX_HI = 0x6;
    WREG32(ctx, mmMM_INDEX_HI, (uint32_t)(pos >> 31));
    WREG32(ctx, mmMM_INDEX, ((uint32_t)pos & 0x7ffffffcu) | 0x80000000u);
    WREG32(ctx, mmMM_DATA, value);
}

// Copy into VRAM at any offset, choosing the fast path when it is available:
// the BAR0 aperture for anything inside it, the MM_INDEX window beyond. `len`
// is rounded down to a dword; callers stage dword-aligned data.
static inline void vram_memcpy(const DeviceContext &ctx, uint64_t vram_byte_offset,
                               const void *src, size_t len) {
    if (vram_byte_offset + len <= ctx.bar0Size && ctx.bar0 != nullptr) {
        bar0_memcpy_to_vram(ctx, vram_byte_offset, src, len);
        return;
    }
    const auto *p = static_cast<const uint8_t *>(src);
    for (size_t i = 0; i + 4 <= len; i += 4) {
        uint32_t v = (uint32_t)p[i] | ((uint32_t)p[i+1] << 8) |
                     ((uint32_t)p[i+2] << 16) | ((uint32_t)p[i+3] << 24);
        WVRAM32_via_mm(ctx, vram_byte_offset + i, v);
    }
}

// ---- HDP flush (host data path coherency) — same registers the reference uses ----
// Linux flushes through the NBIO-remapped HDP_MEM_COHERENCY_FLUSH_CNTL window
// (rmmio_remap.reg_offset + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL). The reference
// blind-writes BAR5+0x44000; we read the remap register and only write where
// the hardware actually points (nothing if the remap is unprogrammed).
static inline void amdgpu_hdp_flush(const DeviceContext &ctx) {
    static bool logged = false;
    // The remap register's BASE_IDX differs between NBIO generations (2 on most
    // nbio_v7_x, 0 in the reference's table): read both, use the first plausible.
    uint32_t remap = 0xFFFFFFFFu, r2 = 0xFFFFFFFFu, r0 = 0xFFFFFFFFu;
    // NBIO base[0] is legitimately 0 on this card (isResolved() treats 0 as unresolved), so key on idx 2.
    if (ctx.ip.isResolved(IPBlock::NBIO, 2))
        r2 = RREG32(ctx, SOC15_REG_OFFSET_BIDX(ctx, IPBlock::NBIO, 2, NBIORegs::BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL));
    if (ctx.ip.isResolved(IPBlock::NBIO, 0))
        r0 = RREG32(ctx, SOC15_REG_OFFSET_BIDX(ctx, IPBlock::NBIO, 0, NBIORegs::BIF_BX0_REMAP_HDP_MEM_FLUSH_CNTL));
    auto plausible = [&](uint32_t v) { return v != 0 && v != 0xFFFFFFFFu && (v & 3) == 0 && (size_t)v + 4 <= ctx.rmmioSize; };
    remap = plausible(r2) ? r2 : (plausible(r0) ? r0 : 0);
    const bool usable = remap != 0;
    if (!logged) { logged = true; IOLog("Navi48Bringup: hdp: REMAP_HDP_MEM_FLUSH_CNTL idx2=0x%08x idx0=0x%08x -> %s (0x%x)\n", r2, r0, usable ? "flush via remap window" : "no flush window; HDP flush writes skipped", remap); }
    if (usable) ctx.rmmio[remap / 4] = 0;
    (void)RREG32(ctx, 0x0DE3);   // read-back barrier the reference relies on
}

// ---- SMN indirect access (BIF_BX1_PCIE_INDEX2 / DATA2, absolute dwords from amdgpu_ip.h) ----
static inline uint32_t SMN_RREG32(const DeviceContext &ctx, uint32_t smn_reg_dword) {
    WREG32(ctx, NBIORegs::BIF_BX1_PCIE_INDEX2, smn_reg_dword << 2);
    (void)RREG32(ctx, NBIORegs::BIF_BX1_PCIE_INDEX2);
    return RREG32(ctx, NBIORegs::BIF_BX1_PCIE_DATA2);
}
static inline void SMN_WREG32(const DeviceContext &ctx, uint32_t smn_reg_dword, uint32_t value) {
    WREG32(ctx, NBIORegs::BIF_BX1_PCIE_INDEX2, smn_reg_dword << 2);
    (void)RREG32(ctx, NBIORegs::BIF_BX1_PCIE_INDEX2);
    WREG32(ctx, NBIORegs::BIF_BX1_PCIE_DATA2, value);
    (void)RREG32(ctx, NBIORegs::BIF_BX1_PCIE_DATA2);
}

// ---- PCIe port registers via NBIO base-idx-1 RSMU index/data ----
static inline uint32_t PCIE_PORT_RREG32(const DeviceContext &ctx, uint32_t pcie_port_reg_dword) {
    const uint32_t idx = SOC15_REG_OFFSET_BIDX(ctx, IPBlock::NBIO, 1, NBIORegs::BIF_BX_PF1_RSMU_INDEX);
    const uint32_t dat = SOC15_REG_OFFSET_BIDX(ctx, IPBlock::NBIO, 1, NBIORegs::BIF_BX_PF1_RSMU_DATA);
    WREG32(ctx, idx, pcie_port_reg_dword << 2); (void)RREG32(ctx, idx);
    return RREG32(ctx, dat);
}
static inline void PCIE_PORT_WREG32(const DeviceContext &ctx, uint32_t pcie_port_reg_dword, uint32_t value) {
    const uint32_t idx = SOC15_REG_OFFSET_BIDX(ctx, IPBlock::NBIO, 1, NBIORegs::BIF_BX_PF1_RSMU_INDEX);
    const uint32_t dat = SOC15_REG_OFFSET_BIDX(ctx, IPBlock::NBIO, 1, NBIORegs::BIF_BX_PF1_RSMU_DATA);
    WREG32(ctx, idx, pcie_port_reg_dword << 2); (void)RREG32(ctx, idx);
    WREG32(ctx, dat, value); (void)RREG32(ctx, dat);
}

// ---- BAR2 doorbells (reference: dev.pci->MemoryWrite32/64(bar2MemIndex, byteOff, v)) ----
static inline void WDOORBELL32(const DeviceContext &ctx, uint64_t byte_offset, uint32_t value) {
    if (!ctx.bar2 || byte_offset + 4 > ctx.bar2Size) return;
    *reinterpret_cast<volatile uint32_t *>(ctx.bar2 + byte_offset) = value;
    storeFence();
}
static inline void WDOORBELL64(const DeviceContext &ctx, uint64_t byte_offset, uint64_t value) {
    if (!ctx.bar2 || byte_offset + 8 > ctx.bar2Size) return;
    *reinterpret_cast<volatile uint64_t *>(ctx.bar2 + byte_offset) = value;
    storeFence();
}
static inline uint32_t RDOORBELL32(const DeviceContext &ctx, uint64_t byte_offset) {
    if (!ctx.bar2 || byte_offset + 4 > ctx.bar2Size) return 0xFFFFFFFFu;
    return *reinterpret_cast<const volatile uint32_t *>(ctx.bar2 + byte_offset);
}

// ---- polling helpers (identical semantics to the reference) ----
static inline kern_return_t poll_psp_response(const DeviceContext &ctx, uint32_t reg, uint32_t mask,
                                              uint32_t expected, uint64_t timeout_us, uint32_t *outValue) {
    constexpr uint32_t kPSPStatusMask = 0x0000FFFFu;
    uint64_t elapsed_us = 0; uint32_t v = 0;
    while (true) {
        v = RREG32(ctx, reg);
        if ((v & mask) == expected) { if (outValue) *outValue = v; return kIOReturnSuccess; }
        if ((v & 0x80000000u) && (v & kPSPStatusMask) != 0) { if (outValue) *outValue = v; return kIOReturnIOError; }
        if (elapsed_us >= timeout_us) { if (outValue) *outValue = v; return kIOReturnTimeout; }
        IOSleep(1); elapsed_us += 1000;
    }
}
static inline bool poll_reg(const DeviceContext &ctx, uint32_t reg, uint32_t mask, uint32_t expected,
                            uint64_t timeout_us, uint32_t *outValue = nullptr) {
    uint64_t elapsed_us = 0; uint32_t v = 0;
    while (true) {
        v = RREG32(ctx, reg);
        if ((v & mask) == expected) { if (outValue) *outValue = v; return true; }
        if (elapsed_us >= timeout_us) { if (outValue) *outValue = v; return false; }
        IOSleep(1); elapsed_us += 1000;
    }
}

} // namespace amdgpu
