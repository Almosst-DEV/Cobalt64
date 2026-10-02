// Doorbell aperture bookkeeping.
//
// Origin: doorbell_init from lemonade-sdk/mac-amdgpu dext/amdgpu/amdgpu_init.cpp
// (MIT, 3bdeed2). Deviations: BAR2 presence is dev.bar2 != nullptr instead of a
// DriverKit memory index, and the index map itself is now upstream amdgpu's
// AMDGPU_NAVI10_DOORBELL_ASSIGNMENT rather than the reference's ad-hoc values
// (see DoorbellIndex in amdgpu_ip.h for the unit conventions).
//
// Why the map changed in 0.0.18: the reference put the GFX ring at DWORD 0 and
// "compute" at 0x200. DWORD 0 is KIQ's slot in every upstream map, and 0x200 is
// exactly where SDMA0's doorbell lands once sdma_engine[0] = 0x100 is shifted
// left by one — so the moment compute queues arrive, two engines share a slot.
// Nothing collided in the stages 16/17 runs (GFX at 0, MES ring at 0x20, MES
// aggregated at 0x100, SDMA at 0x200 — all distinct), so this is a correctness
// fix ahead of compute queues, not a fix for an observed failure.
#include "amdgpu_doorbell.h"
#include "amdgpu_log.h"

namespace amdgpu {

kern_return_t doorbell_init(DeviceContext &dev, DoorbellState &db) {
    if (!dev.bar2 || dev.bar2Size == 0) {
        INIT_LOG("doorbell_init: BAR2 not mapped");
        return kIOReturnNotReady;
    }
    constexpr uint64_t kRDNA4DoorbellSize = 2 * 1024 * 1024;   // 2 MB
    db.size = dev.bar2Size < kRDNA4DoorbellSize ? dev.bar2Size : kRDNA4DoorbellSize;

    if (db.index.legacy_map) {
        // The pre-0.0.18 assignment, verbatim, for A/B only.
        db.index.gfx_ring0      = 0;
        db.index.gfx_ring1      = 1;
        db.index.sdma_engine[0] = 0x100;
        db.index.sdma_engine[1] = 0x10A;
        db.index.sdma_engine[2] = 0x114;
        db.index.sdma_engine[3] = 0x11E;
        db.index.ih             = 6;
        db.index.mes_ring0      = 0x20;
        for (uint32_t i = 0; i < 5; i++) db.index.mes_aggregated[i] = 0x100 + i;
        db.index.max_assignment = 0x200;
    }
    // else: the DoorbellIndex defaults are already the Navi10 map.

    const uint32_t max_by_size = static_cast<uint32_t>(db.size / sizeof(uint32_t));
    const uint32_t max_by_idx  = db.index.max_assignment + 1;
    db.num_kernel_doorbells = (max_by_size < max_by_idx) ? max_by_size : max_by_idx;
    db.num_kernel_doorbells += 0x400;

    INIT_LOG("doorbell_init: BAR2 %llu KiB, size=%llu num_kernel=%u max_idx=%#x (%s map)",
             (unsigned long long)(dev.bar2Size >> 10), (unsigned long long)db.size,
             db.num_kernel_doorbells, db.index.max_assignment,
             db.index.legacy_map ? "legacy 0.0.17" : "upstream Navi10");
    INIT_LOG("doorbell_init: kiq=%#x mec0=%#x mes0=%#x uq=[%#x..%#x] | gfx0=%#x gfx_uq_end=%#x | "
             "sdma0=%#x(dw %#x) ih=%#x | mes_aggregated=%#x,%#x,%#x,%#x,%#x",
             db.index.kiq, db.index.mec_ring[0], db.index.mes_ring0,
             db.index.userqueue_start, db.index.userqueue_end,
             db.index.gfx_ring0, db.index.gfx_userqueue_end,
             db.index.sdma_engine[0], db.index.sdma_engine[0] << 1, db.index.ih,
             db.index.mes_aggregated[0], db.index.mes_aggregated[1],
             db.index.mes_aggregated[2], db.index.mes_aggregated[3],
             db.index.mes_aggregated[4]);
    return kIOReturnSuccess;
}

} // namespace amdgpu
