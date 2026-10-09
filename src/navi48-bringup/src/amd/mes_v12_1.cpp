//
//  mes_v12_1.cpp — MES v12_1 storage allocation, microcode start, MQD /
//  HQD programming, the scheduler ring, and the MES API packets
//  (SET_HW_RESOURCES, SET_HW_RESOURCES_1, ADD_QUEUE,
//  QUERY_SCHEDULER_STATUS) needed for the first PM4 dispatch.
//
//  Ported from lemonade-sdk/mac-amdgpu (MIT) @ commit 3bdeed2:
//      dext/amdgpu/mes_v12_1.cpp
//      dext/amdgpu/amdgpu_mes.h
//      dext/MacAMDGPU.cpp   (the MES firmware-header parse in LoadFirmware)
//
//  which in turn ports:
//      drivers/gpu/drm/amd/amdgpu/mes_v12_0.c
//          (mes_v12_0_allocate_eop_buf, mes_v12_0_enable,
//           mes_v12_0_set_ucode_start_addr, mes_v12_0_mqd_init,
//           mes_v12_0_queue_init_register, mes_v12_0_set_hw_resources,
//           mes_v12_0_set_hw_resources_1, mes_v12_0_add_hw_queue,
//           mes_v12_0_kiq_setting,
//           mes_v12_0_enable_unmapped_doorbell_handling,
//           mes_v12_0_init_aggregated_doorbell,
//           mes_v12_0_submit_pkt_and_poll_completion, mes_v12_0_hw_init)
//      drivers/gpu/drm/amd/include/mes_v12_api_def.h
//
//  ==================================================================
//  Deviations from the reference
//  ==================================================================
//   1. DriverKit IOBufferMemoryDescriptor + IODMACommand → amdgpu::SysMem
//      (PORTING.md). There is no IOMMU on this platform, so SysMem.bus
//      is the HOST PHYSICAL address, which is not a GPU address: each
//      MES buffer is additionally bound into the GART with
//      gmc_bind_existing() and `MESInstance::*_bus` holds the returned
//      MC address — i.e. exactly what the reference's `*_bus` meant to
//      the GPU (its IODMACommand produced a DART IOVA the GPU could
//      use directly). The reference's per-buffer `*_dma`/`*_buf` pairs
//      collapse into one SysMem each.
//   2. Because of (1), the three entry points that allocate GPU-visible
//      memory take an extra `GMCContext &gmc`: mes_alloc_storage,
//      mes_set_hw_resources, mes_set_hw_resources_1. Every other
//      function keeps the reference's exact signature.
//   3. The reference loads MES microcode through the dext's LoadFirmware
//      external method before MESInit, and parses mes_uc_start_addr
//      there (MacAMDGPU.cpp:2011-2024). This kext has no such selector,
//      so that parse lives here as mes_parse_ucode_header() and
//      mes_init_full() runs it when uc_start_addr is still 0. The actual
//      LOAD_IP_FW for CP_MES / CP_MES_DATA happens one stage earlier in
//      psp_load_non_psp_fw (BringupStage::PSPFwLoad) — same place in the
//      order as the reference. mes_load_firmware() is provided for a
//      caller that skipped that stage and wants this module to push the
//      microcode itself; mes_init_full does not call it.
//      mes_init_full's signature keeps the reference's four arguments
//      and adds two DEFAULTED firmware-blob parameters.
//   4. `grbm_select` is named `mes_grbm_select` here — amdgpu_gfx.h /
//      amdgpu_cp.h in this tree own the GFX/CP GRBM select paths and a
//      plain `grbm_select` in a header risks colliding with them. Same
//      body, same register writes.
//   5. Register-field macros shared with the CP/GFX MQD paths come from
//      amdgpu_gfx.h (which already carries byte-identical copies of the
//      reference's amdgpu_mes.h definitions). See amdgpu_mes.h.
//   6. Waits: the reference's `IOSleep(1)` after pipe activation (it had
//      no sub-millisecond sleep in DriverKit) becomes IODelay(500) —
//      exactly upstream's `udelay(500)` at mes_v12_0.c:1140. In
//      mes_submit_pkt the reference's 2000-iteration dummy read loop
//      ("~100 us") becomes a real IODelay(100) for the first 10 ms and
//      IOSleep(1) beyond that, so a 2 s timeout does not busy-spin a
//      core. The timeout values themselves (2 s) are unchanged.
//   7. mes_set_hw_resources: the reference fills mmhub_base[0] from
//      dev.ip.get(IPBlock::GMC). Our on-die discovery never populates
//      IPBlock::GMC (amdgpu_discovery.cpp maps hwid 34 to IPBlock::MMHUB
//      and leaves GMC at the 0xFFFFFFFF sentinel), so we read MMHUB and
//      fall back to GMC. Unresolved bases are written as 0 instead of
//      the sentinel, and every value is logged.
//   8. Explicit sysmem_wmb()/sysmem_rmb() around ring writes, doorbell
//      kicks and fence polls. The reference relied on `volatile` alone.
//   9. mes_release_storage() added — the dext never freed these buffers.
//  10. Logging: every programmed address and every touched register is
//      logged with its before/after value (PORTING.md rule 6). No
//      %{public}s; uint64_t values are cast to unsigned long long.
//
//  11. ADDRESSING. The reference addressed every GC register in this file
//      at SOC15 BASE_IDX 0 (plain SOC15_REG_OFFSET). Per
//      gc_12_0_0_offset.h the CP_MES_* block, RLC_CP_SCHEDULERS,
//      CP_UNMAPPED_DOORBELL and GRBM_GFX_CNTL are BASE_IDX 1, while the
//      CP_HQD_*/CP_MQD_* window and CP_HQD_GFX_CONTROL are BASE_IDX 0.
//      Every access now goes through MES_REG() (amdgpu_mes.h), which
//      bakes in the header's per-register BASE_IDX from MESRegBaseIdx.
//      No register order or value changed.
//
//  Not ported (absent from the reference as well): the non-uni_mes KIQ
//  pipe program (mes_v12_0_kiq_hw_init's second pipe), REMOVE_QUEUE,
//  SET_SCHEDULING_CONFIG, MES event logging / MSCRATCH ring, and the
//  MES reset/suspend/resume paths.
//

#include <stddef.h>
#include <string.h>
#include <IOKit/IOLib.h>

#include "amdgpu_mes.h"
#include "amdgpu_psp.h"
#include "amdgpu_gmc.h"
#include "amdgpu_ucode_psp.h"
#include "amdgpu_log.h"
#include "../fw/fw_table.h"

namespace amdgpu {

//------------------------------------------------------------------
// Local allocation helper — replaces the reference's
// mes_alloc_dma_block (IOBufferMemoryDescriptor::Create +
// IODMACommand::PrepareForDMA). Deviation 1: physically contiguous,
// zeroed sysmem, then bound into GART so the GPU has an MC address.
//------------------------------------------------------------------
static kern_return_t
mes_alloc_gtt_block(DeviceContext &dev, GMCContext &gmc, uint64_t size,
                    SysMem &mem, uint64_t *outMc, void **outCpu,
                    const char *what)
{
    *outMc = 0; *outCpu = nullptr;
    if (mem.valid()) {           // already allocated — idempotent like the reference
        *outCpu = mem.cpu;
        return kIOReturnSuccess;
    }
    // gmc_bind_existing requires a 4 KB-aligned bus address.
    kern_return_t r = sysmem_alloc(mem, size, kAMDGPUGPUPageSize);
    if (r != kIOReturnSuccess) {
        MES_LOG("%s: sysmem_alloc(%llu) failed: %#x",
                what, (unsigned long long)size, r);
        return r;
    }
    uint64_t mc = 0;
    r = gmc_bind_existing(dev, gmc, mem.bus, mem.size, &mc);
    if (r != kIOReturnSuccess) {
        MES_LOG("%s: gart bind of bus=%#llx (%llu B) failed: %#x",
                what, (unsigned long long)mem.bus,
                (unsigned long long)mem.size, r);
        sysmem_free(mem);
        return r;
    }
    memset(mem.cpu, 0, (size_t)mem.size);
    sysmem_wmb();
    *outMc  = mc;
    *outCpu = mem.cpu;
    MES_LOG("%s: %llu B  cpu=%p  phys=%#llx  gart_mc=%#llx",
            what, (unsigned long long)mem.size, mem.cpu,
            (unsigned long long)mem.bus, (unsigned long long)mc);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_alloc_storage — EOP + MQD + ring + cmd buffer.
//------------------------------------------------------------------
kern_return_t
mes_alloc_storage(DeviceContext &dev, GMCContext &gmc, MESInstance &inst)
{
    if (inst.inited) return kIOReturnSuccess;
    if (!gmc.inited || gmc.gart_pt_bus == 0) {
        MES_LOG("alloc_storage: GART not ready (gmc.inited=%d pt_bus=%#llx) — "
                "run GMCInit first",
                gmc.inited, (unsigned long long)gmc.gart_pt_bus);
        return kIOReturnNotReady;
    }
    void *cpu = nullptr;

    kern_return_t r = mes_alloc_gtt_block(dev, gmc, kMES_EOP_SIZE,
                                          inst.eop_mem, &inst.eop_bus,
                                          &cpu, "EOP");
    if (r != kIOReturnSuccess) {
        MES_LOG("EOP alloc failed: %#x", r);
        return r;
    }
    inst.eop_cpu = cpu;

    r = mes_alloc_gtt_block(dev, gmc, kMES_MQD_SIZE,
                            inst.mqd_mem, &inst.mqd_bus, &cpu, "MQD");
    if (r != kIOReturnSuccess) {
        MES_LOG("MQD alloc failed: %#x", r);
        return r;
    }
    inst.mqd_cpu = cpu;

    r = mes_alloc_gtt_block(dev, gmc, kMES_RING_SIZE,
                            inst.ring_mem, &inst.ring_bus, &cpu, "ring");
    if (r != kIOReturnSuccess) {
        MES_LOG("ring alloc failed: %#x", r);
        return r;
    }
    inst.ring_cpu = cpu;

    r = mes_alloc_gtt_block(dev, gmc, kMES_CMD_BUF_SIZE,
                            inst.cmd_mem, &inst.cmd_bus, &cpu, "cmd");
    if (r != kIOReturnSuccess) {
        MES_LOG("cmd buf alloc failed: %#x", r);
        return r;
    }
    inst.cmd_cpu = cpu;

    // Write-back page — rptr/wptr shadows for the SCHED ring.
    r = mes_alloc_gtt_block(dev, gmc, kASPageSize,
                            inst.wb_mem, &inst.wb_bus, &cpu, "wb");
    if (r != kIOReturnSuccess) {
        MES_LOG("wb alloc failed: %#x", r);
        return r;
    }
    inst.wb_cpu             = cpu;
    inst.ring_rptr_gpu_addr = inst.wb_bus + 0x00;
    inst.ring_wptr_gpu_addr = inst.wb_bus + 0x40;
    inst.ring_size_dwords   = kMES_RING_SIZE / 4;
    // Doorbell index: Sched pipe gets slot from doorbell_index map.
    // MES ring0 doorbell offset (BAR2 DWORD offset) — matches
    // doorbell.index.mes_ring0 = 0x20 in the ASIC-specific map.
    inst.doorbell_index = dev.doorbell.index.mes_ring0;

    inst.inited = true;
    MES_LOG("storage: EOP %#llx, MQD %#llx, ring %#llx, cmd %#llx",
            (unsigned long long)inst.eop_bus,
            (unsigned long long)inst.mqd_bus,
            (unsigned long long)inst.ring_bus,
            (unsigned long long)inst.cmd_bus);
    MES_LOG("storage: wb %#llx (rptr %#llx, wptr %#llx), ring %u dw, "
            "doorbell idx %#x (BAR2 byte %#llx)",
            (unsigned long long)inst.wb_bus,
            (unsigned long long)inst.ring_rptr_gpu_addr,
            (unsigned long long)inst.ring_wptr_gpu_addr,
            inst.ring_size_dwords, inst.doorbell_index,
            (unsigned long long)inst.doorbell_index * 4ull);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_release_storage — deviation 9. GART slots are bump-allocated
// by gmc_bind_existing and are not reclaimed; only the host pages go
// back. Safe to call on a partially-initialised context.
//------------------------------------------------------------------
void
mes_release_storage(MESContext &mes)
{
    for (uint32_t p = 0; p < kMaxMESPipes; p++) {
        MESInstance &inst = mes.pipe[p];
        sysmem_free(inst.eop_mem);
        sysmem_free(inst.mqd_mem);
        sysmem_free(inst.ring_mem);
        sysmem_free(inst.cmd_mem);
        sysmem_free(inst.wb_mem);
        inst.eop_bus = inst.mqd_bus = inst.ring_bus = 0;
        inst.cmd_bus = inst.wb_bus = 0;
        inst.eop_cpu = inst.mqd_cpu = inst.ring_cpu = nullptr;
        inst.cmd_cpu = inst.wb_cpu = nullptr;
        inst.inited  = false;
        inst.enabled = false;
    }
    sysmem_free(mes.sch_ctx_mem);
    sysmem_free(mes.status_fence_mem);
    sysmem_free(mes.resource_1_mem);
    mes.sch_ctx_bus = mes.status_fence_bus = mes.resource_1_bus = 0;
    MES_LOG("release_storage: all MES sysmem released");
}

//------------------------------------------------------------------
// mes_set_uc_start_addr — call from LoadFirmware after fw bytes
// have been handed to PSP. We parse the firmware header here.
//------------------------------------------------------------------
kern_return_t
mes_set_uc_start_addr(MESContext &mes, MESPipe pipe, uint64_t addr)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    mes.pipe[p].uc_start_addr = addr;
    if (pipe == MESPipe::Sched) mes.sched_ucode_loaded = true;
    if (pipe == MESPipe::KIQ)   mes.kiq_ucode_loaded   = true;
    MES_LOG("pipe %u uc_start_addr = %#llx",
            p, (unsigned long long)addr);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_parse_ucode_header — deviation 3. Port of the MES branch of
// MacAMDGPU.cpp's LoadFirmware (MacAMDGPU.cpp:2011-2024), which
// mirrors upstream amdgpu_mes.c:708-713 (the driver stashes
// adev->mes.uc_start_addr[pipe] from the file header before the PSP
// load).
//------------------------------------------------------------------
kern_return_t
mes_parse_ucode_header(MESContext &mes, MESPipe pipe,
                       const uint8_t *bin, uint64_t size)
{
    if (bin == nullptr || size < sizeof(mes_firmware_header_v1_0)) {
        MES_LOG("parse_ucode_header: blob too small (%llu B, need %llu)",
                (unsigned long long)size,
                (unsigned long long)sizeof(mes_firmware_header_v1_0));
        return kIOReturnBadArgument;
    }
    auto *mhdr = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin);
    if (mhdr->header.header_version_major != 1) {
        MES_LOG("parse_ucode_header: unexpected header version %u.%u",
                mhdr->header.header_version_major,
                mhdr->header.header_version_minor);
        return kIOReturnUnsupported;
    }
    const uint64_t uc_addr =
        static_cast<uint64_t>(mhdr->mes_uc_start_addr_lo) |
        (static_cast<uint64_t>(mhdr->mes_uc_start_addr_hi) << 32);
    const uint64_t data_addr =
        static_cast<uint64_t>(mhdr->mes_data_start_addr_lo) |
        (static_cast<uint64_t>(mhdr->mes_data_start_addr_hi) << 32);
    MES_LOG("parse_ucode_header: ucode ver %#x @+%u (%u B), data ver %#x "
            "@+%u (%u B), uc_start=%#llx data_start=%#llx",
            mhdr->mes_ucode_version, mhdr->mes_ucode_offset_bytes,
            mhdr->mes_ucode_size_bytes, mhdr->mes_ucode_data_version,
            mhdr->mes_ucode_data_offset_bytes,
            mhdr->mes_ucode_data_size_bytes,
            (unsigned long long)uc_addr, (unsigned long long)data_addr);
    return mes_set_uc_start_addr(mes, pipe, uc_addr);
}

//------------------------------------------------------------------
// mes_load_firmware — deviation 3. Pushes the uni_mes ucode + data
// through the PSP, mirroring amdgpu_ucode_extract.cpp's extract_mes
// payload split and step 6 of psp_load_non_psp_fw:
//
//   CP_MES        (33) = ucode, SCHED pipe
//   CP_MES_DATA   (34) = data,  SCHED pipe
//   CP_MES_KIQ    (81) = same ucode bytes, KIQ pipe tag
//   MES_KIQ_STACK (82) = same data bytes,  KIQ pipe tag
//
// gfx_v12_0 runs with enable_uni_mes = true and mes_v12_0_early_init
// registers BOTH pipes from the SAME uni_mes.bin, so all four frames
// are submitted (PSP's autoload manifest references the KIQ slots).
//
// No-op success when the microcode has already been loaded (the
// normal ladder path — BringupStage::PSPFwLoad did it).
//------------------------------------------------------------------
kern_return_t
mes_load_firmware(DeviceContext &dev, PSPContext &psp, MESContext &mes,
                  const uint8_t *bin, uint64_t size)
{
    if (mes.sched_ucode_loaded) {
        MES_LOG("load_firmware: MES microcode already loaded — skipping");
        return kIOReturnSuccess;
    }
    if (bin == nullptr || size < sizeof(mes_firmware_header_v1_0)) {
        return kIOReturnBadArgument;
    }
    auto *mhdr = reinterpret_cast<const mes_firmware_header_v1_0 *>(bin);
    if (mhdr->header.header_version_major != 1) return kIOReturnUnsupported;

    struct Payload {
        uint32_t    fw_type;
        uint32_t    offset;
        uint32_t    size;
        const char *name;
    };
    const Payload payloads[] = {
        { PSPGfxFwType::CP_MES,        mhdr->mes_ucode_offset_bytes,
          mhdr->mes_ucode_size_bytes,      "CP_MES"        },
        { PSPGfxFwType::CP_MES_DATA,   mhdr->mes_ucode_data_offset_bytes,
          mhdr->mes_ucode_data_size_bytes, "CP_MES_DATA"   },
        { PSPGfxFwType::CP_MES_KIQ,    mhdr->mes_ucode_offset_bytes,
          mhdr->mes_ucode_size_bytes,      "CP_MES_KIQ"    },
        { PSPGfxFwType::MES_KIQ_STACK, mhdr->mes_ucode_data_offset_bytes,
          mhdr->mes_ucode_data_size_bytes, "MES_KIQ_STACK" },
    };

    for (const Payload &pl : payloads) {
        if (pl.size == 0 ||
            static_cast<uint64_t>(pl.offset) + pl.size > size) {
            MES_LOG("load_firmware: %s payload out of bounds "
                    "(+%u, %u B, file %llu B)",
                    pl.name, pl.offset, pl.size, (unsigned long long)size);
            return kIOReturnBadArgument;
        }
        uint64_t mc = 0;
        kern_return_t r = psp_fw_buf_stage(dev, psp, bin + pl.offset,
                                           pl.size, &mc);
        if (r != kIOReturnSuccess) {
            MES_LOG("load_firmware: %s staging failed %#x", pl.name, r);
            return r;
        }
        r = psp_load_ip_fw(dev, psp, mc, pl.size, pl.fw_type);
        if (r != kIOReturnSuccess) {
            MES_LOG("load_firmware: %s LOAD_IP_FW(type=%u, mc=%#llx, %u B) "
                    "failed %#x",
                    pl.name, pl.fw_type, (unsigned long long)mc, pl.size, r);
            return r;
        }
        MES_LOG("load_firmware: %s loaded (type=%u, mc=%#llx, %u B)",
                pl.name, pl.fw_type, (unsigned long long)mc, pl.size);
    }

    return mes_parse_ucode_header(mes, MESPipe::Sched, bin, size);
}

//------------------------------------------------------------------
// mes_enable — port of mes_v12_0_enable. For our uni_mes path we
// only program pipe 0; the non-uni path is deferred until we need
// the KIQ pipe.
//------------------------------------------------------------------
kern_return_t
mes_enable(const DeviceContext &dev, MESContext &mes, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) {
        MES_LOG("enable: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    const uint32_t cnt_reg =
        MES_REG(dev, CP_MES_CNTL);

    if (!enable) {
        // Halt + reset + invalidate. Same write sequence as the
        // !enable branch of mes_v12_0_enable.
        uint32_t v = RREG32(dev, cnt_reg);
        MES_LOG("enable(false): CP_MES_CNTL before = %#x", v);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_ACTIVE, 0);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_ACTIVE, 0);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_INVALIDATE_ICACHE, 1);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE1_RESET, 1);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_HALT, 1);
        WREG32(dev, cnt_reg, v);
        MES_LOG("enable(false): CP_MES_CNTL wrote %#x, readback %#x",
                v, RREG32(dev, cnt_reg));
        mes.pipe[0].enabled = false;
        mes.pipe[1].enabled = false;
        return kIOReturnSuccess;
    }

    if (mes.pipe[0].uc_start_addr == 0) {
        MES_LOG("enable: pipe 0 uc_start_addr not set "
                "(load MES microcode via LoadFirmware first)");
        return kIOReturnNotReady;
    }

    MES_LOG("enable: CP_MES_CNTL before = %#x, CP_MES_INSTR_PNTR = %#x",
            RREG32(dev, cnt_reg),
            RREG32(dev, MES_REG(dev, CP_MES_INSTR_PNTR)));

    // GRBM select MES pipe 0 (me=3, pipe=0, queue=0, vmid=0).
    mes_grbm_select(dev, /*me=*/3, /*pipe=*/0, /*queue=*/0, /*vmid=*/0);

    // CP_MES_MSCRATCH_{HI,LO} — direct port of mes_v12_0.c:1100-1108.
    // Upstream writes these only when amdgpu_mes_log_enable is true
    // and event_log_size is large enough. We don't enable event log,
    // so we zero the registers (they latch reset garbage otherwise on
    // some SOC variants; explicit zeroing matches upstream's
    // "register value is undefined unless written" warning in the
    // gc_12_0_0 reg-doc).
    //
    // Audit-7 #5.
    const uint32_t mscratch_lo_reg =
        MES_REG(dev, CP_MES_MSCRATCH_LO_OFFSET);
    const uint32_t mscratch_hi_reg =
        MES_REG(dev, CP_MES_MSCRATCH_HI_OFFSET);
    MES_LOG("enable: CP_MES_MSCRATCH_LO before %#x, HI before %#x",
            RREG32(dev, mscratch_lo_reg), RREG32(dev, mscratch_hi_reg));
    WREG32(dev, mscratch_lo_reg, 0);
    WREG32(dev, mscratch_hi_reg, 0);

    // Pre-reset: pulse PIPE0_RESET. mes_v12_0.c:1112-1117 reads the
    // CNTL register first and OR'd PIPE0_RESET — keep that RMW.
    {
        uint32_t v = RREG32(dev, cnt_reg);
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_RESET, 1);
        WREG32(dev, cnt_reg, v);
        MES_LOG("enable: PIPE0_RESET pulse, CP_MES_CNTL = %#x (readback %#x)",
                v, RREG32(dev, cnt_reg));
    }

    // Program ucode start address (shift right 2 per upstream).
    // mes_v12_0.c:1119-1123.
    const uint64_t ucode_addr = mes.pipe[0].uc_start_addr >> 2;
    const uint32_t pc_lo_reg =
        MES_REG(dev, CP_MES_PRGRM_CNTR_START);
    const uint32_t pc_hi_reg =
        MES_REG(dev, CP_MES_PRGRM_CNTR_START_HI);
    WREG32(dev, pc_lo_reg, static_cast<uint32_t>(ucode_addr));
    WREG32(dev, pc_hi_reg, static_cast<uint32_t>(ucode_addr >> 32));
    MES_LOG("enable: PRGRM_CNTR_START = %#x / _HI = %#x "
            "(uc_start %#llx >> 2 = %#llx)",
            RREG32(dev, pc_lo_reg), RREG32(dev, pc_hi_reg),
            (unsigned long long)mes.pipe[0].uc_start_addr,
            (unsigned long long)ucode_addr);

    // Activate pipe 0 (start from cleared CP_MES_CNTL — matches
    // upstream which builds the activate value from 0 at
    // mes_v12_0.c:1126).
    {
        uint32_t v = 0;
        v = REG_SET_FIELD(v, CP_MES_CNTL, MES_PIPE0_ACTIVE, 1);
        WREG32(dev, cnt_reg, v);
        MES_LOG("enable: activate write CP_MES_CNTL = %#x", v);
    }

    // GRBM deselect.
    mes_grbm_select(dev, 0, 0, 0, 0);

    // mes_v12_0.c:1140 — udelay(500) for uni_mes. Deviation 6: the
    // reference used IOSleep(1) because DriverKit has no sub-ms sleep;
    // the kernel has IODelay, so we use upstream's exact 500 us.
    IODelay(500);

    mes.pipe[0].enabled = true;
    MES_LOG("enable: pipe 0 active, uc_start=%#llx; CP_MES_CNTL after = %#x, "
            "CP_MES_INSTR_PNTR = %#x",
            (unsigned long long)mes.pipe[0].uc_start_addr,
            RREG32(dev, cnt_reg),
            RREG32(dev, MES_REG(dev, CP_MES_INSTR_PNTR)));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_set_hw_resources_1 — port of mes_v12_0_set_hw_resources_1
// (mes_v12_0.c:711).
//
// Audit-7 #6. Sent after SET_HW_RESOURCES, gated on
// sched_version >= 0x4b. Lazy-allocates the cleaner-shader fence
// buffer.
//------------------------------------------------------------------
kern_return_t
mes_set_hw_resources_1(DeviceContext &dev, GMCContext &gmc, MESContext &mes)
{
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // Lazy-alloc the cleaner-shader fence (4 KB sysmem).
    if (mes.resource_1_bus == 0) {
        void *cpu = nullptr;
        kern_return_t r = mes_alloc_gtt_block(dev, gmc, kMES_Resource1Bytes,
                                              mes.resource_1_mem,
                                              &mes.resource_1_bus, &cpu,
                                              "cleaner_shader_fence");
        if (r != kIOReturnSuccess) return r;
    }

    // mes_v12_0.c:713-726 — header, mes_kiq_unmap_timeout=0xa,
    // cleaner_shader_fence_mc_addr, then submit. We mirror the
    // upstream layout in mes_v12_api_def.h:312-335 verbatim. Natural
    // C struct alignment on x86_64 / aarch64 introduces 4-byte
    // padding before each uint64_t that follows a uint32_t — match
    // that with explicit `_pad*` members so the offsets stay bit-
    // compatible.
    //
    // Upstream layout (after natural alignment):
    //   header                       u32  @  0 dw
    //   api_status                   16B  @  1 dw  (4 dw)
    //   timestamp                    u64  @  6 dw  (after 4 B pad)
    //   flags (u32 union)            u32  @  8 dw
    //   mes_debug_ctx_mc_addr        u64  @ 10 dw  (after 4 B pad)
    //   mes_debug_ctx_size           u32  @ 12 dw
    //   mes_kiq_unmap_timeout        u32  @ 13 dw
    //   coop_sch_shared_mc_addr      u64  @ 14 dw
    //   cleaner_shader_fence_mc_addr u64  @ 16 dw
    //   ... padding to 64 dw total.
    // Natural alignment matches upstream union layout: compiler
    // inserts 4-byte pads after `header` (to align api_status's u64
    // fence_addr) and after `flags` (to align mes_debug_ctx_mc_addr).
    // We sum: header 4 + auto-pad 4 + api_status 16 + _pad_after_status 4
    // + auto-pad 4 (timestamp align) + timestamp 8 + flags 4
    // + _pad_after_flags 4 + mes_debug_ctx_mc_addr 8 + size 4 + timeout 4
    // + coop 8 + cleaner 8 = 80 bytes (= 20 dwords). Round to 64 dw:
    // pad[44] adds 176 bytes → 256 total.
    struct MES_SetHwResources1 {
        MES_Header_Wire header;                       // dw  0
        MES_API_Status  api_status;                   // dw  2..5 (auto-pad at dw 1)
        uint32_t        _pad_after_status;            // dw  6
        uint64_t        timestamp;                    // dw  8..9 (auto-pad at dw 7)
        uint32_t        flags;                        // dw 10
        uint32_t        _pad_after_flags;             // dw 11
        uint64_t        mes_debug_ctx_mc_addr;        // dw 12..13
        uint32_t        mes_debug_ctx_size;           // dw 14
        uint32_t        mes_kiq_unmap_timeout;        // dw 15
        uint64_t        coop_sch_shared_mc_addr;      // dw 16..17
        uint64_t        cleaner_shader_fence_mc_addr; // dw 18..19
        uint32_t        pad[44];                      // round to 64 dw
    };
    static_assert(sizeof(MES_SetHwResources1) == 64 * 4,
                  "SetHwResources1 must be 64 dw");

    MES_SetHwResources1 pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::SET_HW_RSRC_1,
                                       kMES_API_FRAME_DWORDS);
    pkt.mes_kiq_unmap_timeout = 0xa;  // mes_v12_0.c:720
    pkt.cleaner_shader_fence_mc_addr = mes.resource_1_bus;

    const uint32_t api_status_dw =
        offsetof(MES_SetHwResources1, api_status) / 4;
    MES_LOG("set_hw_resources_1: header=%#x kiq_unmap_timeout=%#x "
            "cleaner_shader_fence_mc=%#llx (api_status @ dw %u)",
            pkt.header.u32All, pkt.mes_kiq_unmap_timeout,
            (unsigned long long)pkt.cleaner_shader_fence_mc_addr,
            api_status_dw);
    return mes_submit_pkt(dev, mes, MESPipe::Sched,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          api_status_dw,
                          /*timeout_us=*/2000000);
}

//------------------------------------------------------------------
// v12_compute_mqd field offsets (in dwords) — from upstream
// include/v12_structs.h. We only mirror the fields written by
// mes_v12_0_mqd_init, plus a couple of constants. Anything we
// don't touch stays 0 (the MQD page was memset to 0 at alloc).
//------------------------------------------------------------------
namespace {
namespace MQDOff {
    constexpr uint32_t cp_mqd_base_addr_lo             = 128;
    constexpr uint32_t cp_mqd_base_addr_hi             = 129;
    constexpr uint32_t cp_hqd_active                   = 130;
    constexpr uint32_t cp_hqd_vmid                     = 131;
    constexpr uint32_t cp_hqd_persistent_state         = 132;
    constexpr uint32_t cp_hqd_pq_base_lo               = 136;
    constexpr uint32_t cp_hqd_pq_base_hi               = 137;
    constexpr uint32_t cp_hqd_pq_rptr_report_addr_lo   = 139;
    constexpr uint32_t cp_hqd_pq_rptr_report_addr_hi   = 140;
    constexpr uint32_t cp_hqd_pq_wptr_poll_addr_lo     = 141;
    constexpr uint32_t cp_hqd_pq_wptr_poll_addr_hi     = 142;
    constexpr uint32_t cp_hqd_pq_doorbell_control      = 143;
    constexpr uint32_t cp_hqd_pq_control               = 145;
    constexpr uint32_t cp_mqd_control                  = 162;
    constexpr uint32_t cp_hqd_eop_base_addr_lo         = 165;
    constexpr uint32_t cp_hqd_eop_base_addr_hi         = 166;
    constexpr uint32_t cp_hqd_eop_control              = 167;
    constexpr uint32_t cp_hqd_pq_wptr_lo               = 182;
    constexpr uint32_t cp_hqd_pq_wptr_hi               = 183;
    constexpr uint32_t reserved_184                    = 184;  // unmapped doorbell
}

inline uint32_t order_base_2_u32(uint32_t x)
{
    uint32_t r = 0;
    while ((1u << r) < x) r++;
    return r;
}
}  // anonymous namespace

//------------------------------------------------------------------
// mes_queue_init — port of mes_v12_0_mqd_init + queue_init_register
// fused into one pass. Computes the HQD field values from the
// MESInstance addresses, writes them to the MQD struct in memory
// at the upstream byte offsets, then GRBM-selects MES pipe 0 and
// writes the same values to the live CP_HQD_* / CP_MQD_* registers.
//------------------------------------------------------------------
kern_return_t
mes_queue_init(const DeviceContext &dev, MESContext &mes, MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    MESInstance &inst = mes.pipe[p];
    if (!inst.inited) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // ---- 1) Compute the MQD field values upstream writes ----
    const uint64_t mqd_addr = inst.mqd_bus;
    const uint32_t cp_mqd_base_lo = static_cast<uint32_t>(mqd_addr) & 0xfffffffcu;
    const uint32_t cp_mqd_base_hi = static_cast<uint32_t>(mqd_addr >> 32);

    const uint64_t hqd_addr  = inst.ring_bus >> 8;
    const uint32_t cp_pq_lo  = static_cast<uint32_t>(hqd_addr);
    const uint32_t cp_pq_hi  = static_cast<uint32_t>(hqd_addr >> 32);

    const uint64_t rptr_addr = inst.ring_rptr_gpu_addr;
    const uint32_t cp_rptr_addr_lo = static_cast<uint32_t>(rptr_addr) & 0xfffffffcu;
    const uint32_t cp_rptr_addr_hi = static_cast<uint32_t>(rptr_addr >> 32) & 0xffffu;

    const uint64_t wptr_addr = inst.ring_wptr_gpu_addr;
    const uint32_t cp_wptr_addr_lo = static_cast<uint32_t>(wptr_addr) & 0xfffffff8u;
    const uint32_t cp_wptr_addr_hi = static_cast<uint32_t>(wptr_addr >> 32) & 0xffffu;

    // cp_hqd_pq_control — set QUEUE_SIZE, RPTR_BLOCK_SIZE, the
    // dispatch + queue flags. AMDGPU_GPU_PAGE_SIZE = 4096.
    // Mirrors mes_v12_0_mqd_init (mes_v12_0.c:1327-1336). The
    // raw value passed to REG_SET_FIELD is the FIELD VALUE (not
    // pre-shifted); REG_SET_FIELD applies the shift internally.
    uint32_t pq_ctrl = kCP_HQD_PQ_CONTROL_DEFAULT;
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
                            order_base_2_u32(inst.ring_size_dwords) - 1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE,
                            order_base_2_u32(4096u / 4u) - 1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, UNORD_DISPATCH,  1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, 0);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, PRIV_STATE,      1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, KMD_QUEUE,       1);
    pq_ctrl = REG_SET_FIELD(pq_ctrl, CP_HQD_PQ_CONTROL, NO_UPDATE_RPTR,  1);

    uint32_t db_ctrl = 0;
    db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                            DOORBELL_OFFSET, inst.doorbell_index);
    db_ctrl = REG_SET_FIELD(db_ctrl, CP_HQD_PQ_DOORBELL_CONTROL,
                            DOORBELL_EN, 1);

    uint32_t persist = kCP_HQD_PERSISTENT_STATE_DEFAULT;
    persist = REG_SET_FIELD(persist, CP_HQD_PERSISTENT_STATE,
                            PRELOAD_SIZE, 0x55);

    uint32_t mqd_ctrl = 0;
    mqd_ctrl = REG_SET_FIELD(mqd_ctrl, CP_MQD_CONTROL, VMID, 0);

    // EOP fields (upstream sets in mqd_init):
    const uint64_t eop_base_addr = inst.eop_bus >> 8;
    const uint32_t cp_eop_lo = static_cast<uint32_t>(eop_base_addr);
    const uint32_t cp_eop_hi = static_cast<uint32_t>(eop_base_addr >> 32);
    uint32_t eop_ctrl = kCP_HQD_EOP_CONTROL_DEFAULT;
    // EOP size: log2(MES_EOP_SIZE/4) - 1 = log2(512) - 1 = 8.
    // Field at bits [5:0]; default already 0x06, override to 0x08.
    eop_ctrl = (eop_ctrl & ~0x3fu)
             | ((order_base_2_u32(kMES_EOP_SIZE / 4u) - 1u) & 0x3fu);

    // ---- 2) Write the MQD struct in memory ----
    auto *mqd = static_cast<uint32_t *>(inst.mqd_cpu);
    if (mqd != nullptr) {
        // Header magic from upstream mqd_init.
        mqd[80]                                  = 0xC0310800u;  // header
        // compute_pipelinestat_enable (idx 81) + compute_static_thread_mgmt
        // (82..85) + compute_misc_reserved (86): not strictly needed
        // for a kernel-mode queue but set to upstream defaults to
        // keep MES happy on context save.
        mqd[81] = 0x00000001u;
        mqd[82] = 0xffffffffu;
        mqd[83] = 0xffffffffu;
        mqd[84] = 0xffffffffu;
        mqd[85] = 0xffffffffu;
        mqd[86] = 0x00000007u;

        mqd[MQDOff::cp_mqd_base_addr_lo]         = cp_mqd_base_lo;
        mqd[MQDOff::cp_mqd_base_addr_hi]         = cp_mqd_base_hi;
        mqd[MQDOff::cp_hqd_active]               = 1;
        mqd[MQDOff::cp_hqd_vmid]                 = 0;
        mqd[MQDOff::cp_hqd_persistent_state]     = persist;
        mqd[MQDOff::cp_hqd_pq_base_lo]           = cp_pq_lo;
        mqd[MQDOff::cp_hqd_pq_base_hi]           = cp_pq_hi;
        mqd[MQDOff::cp_hqd_pq_rptr_report_addr_lo] = cp_rptr_addr_lo;
        mqd[MQDOff::cp_hqd_pq_rptr_report_addr_hi] = cp_rptr_addr_hi;
        mqd[MQDOff::cp_hqd_pq_wptr_poll_addr_lo] = cp_wptr_addr_lo;
        mqd[MQDOff::cp_hqd_pq_wptr_poll_addr_hi] = cp_wptr_addr_hi;
        mqd[MQDOff::cp_hqd_pq_doorbell_control]  = db_ctrl;
        mqd[MQDOff::cp_hqd_pq_control]           = pq_ctrl;
        mqd[MQDOff::cp_mqd_control]              = mqd_ctrl;
        mqd[MQDOff::cp_hqd_eop_base_addr_lo]     = cp_eop_lo;
        mqd[MQDOff::cp_hqd_eop_base_addr_hi]     = cp_eop_hi;
        mqd[MQDOff::cp_hqd_eop_control]          = eop_ctrl;
        mqd[MQDOff::cp_hqd_pq_wptr_lo]           = 0;
        mqd[MQDOff::cp_hqd_pq_wptr_hi]           = 0;
        // Unmapped-doorbell handling — bit 15 of reserved_184.
        mqd[MQDOff::reserved_184]                = (1u << 15);
        sysmem_wmb();   // deviation 8 — MQD visible before HQD activation
    }

    MES_LOG("queue_init: MQD mc=%#llx cpu=%p | pq_base=%#x/%#x (ring %#llx>>8) "
            "| rptr=%#x/%#x | wptr_poll=%#x/%#x | eop=%#x/%#x (%#llx>>8) "
            "eop_ctrl=%#x | persist=%#x mqd_ctrl=%#x",
            (unsigned long long)inst.mqd_bus, inst.mqd_cpu,
            cp_pq_lo, cp_pq_hi, (unsigned long long)inst.ring_bus,
            cp_rptr_addr_lo, cp_rptr_addr_hi,
            cp_wptr_addr_lo, cp_wptr_addr_hi,
            cp_eop_lo, cp_eop_hi, (unsigned long long)inst.eop_bus,
            eop_ctrl, persist, mqd_ctrl);

    // ---- 3) GRBM-select MES pipe, write the same values live ----
    mes_grbm_select(dev, /*me=*/3, /*pipe=*/p, /*queue=*/0, /*vmid=*/0);

    // Disable doorbell first while we reprogram.
    {
        uint32_t v = RREG32(dev, MES_REG(dev, CP_HQD_PQ_DOORBELL_CONTROL));
        MES_LOG("queue_init: CP_HQD_PQ_DOORBELL_CONTROL before = %#x", v);
        v = REG_SET_FIELD(v, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 0);
        WREG32(dev, MES_REG(dev, CP_HQD_PQ_DOORBELL_CONTROL), v);
    }
    // VMID = 0.
    {
        uint32_t v = RREG32(dev, MES_REG(dev, CP_HQD_VMID));
        v = REG_SET_FIELD(v, CP_HQD_VMID, VMID, 0);
        WREG32(dev, MES_REG(dev, CP_HQD_VMID), v);
    }

    WREG32(dev, MES_REG(dev, CP_MQD_BASE_ADDR),         cp_mqd_base_lo);
    WREG32(dev, MES_REG(dev, CP_MQD_BASE_ADDR_HI),      cp_mqd_base_hi);
    // Upstream writes 0 to CP_MQD_CONTROL (not mqd_ctrl) — keep that.
    WREG32(dev, MES_REG(dev, CP_MQD_CONTROL),           0);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_BASE),           cp_pq_lo);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_BASE_HI),        cp_pq_hi);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_RPTR_REPORT_ADDR),    cp_rptr_addr_lo);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_RPTR_REPORT_ADDR_HI), cp_rptr_addr_hi);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_CONTROL),        pq_ctrl);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_WPTR_POLL_ADDR),     cp_wptr_addr_lo);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_WPTR_POLL_ADDR_HI),  cp_wptr_addr_hi);
    WREG32(dev, MES_REG(dev, CP_HQD_PQ_DOORBELL_CONTROL),   db_ctrl);
    WREG32(dev, MES_REG(dev, CP_HQD_PERSISTENT_STATE),      persist);
    WREG32(dev, MES_REG(dev, CP_HQD_ACTIVE),            1);

    // Read the programmed HQD back while the GRBM select is still on
    // the MES pipe — outside the select these offsets alias ME0 state.
    const uint32_t rb_pq_base   = RREG32(dev, MES_REG(dev, CP_HQD_PQ_BASE));
    const uint32_t rb_pq_ctrl   = RREG32(dev, MES_REG(dev, CP_HQD_PQ_CONTROL));
    const uint32_t rb_db_ctrl   = RREG32(dev, MES_REG(dev, CP_HQD_PQ_DOORBELL_CONTROL));
    const uint32_t rb_active    = RREG32(dev, MES_REG(dev, CP_HQD_ACTIVE));
    const uint32_t rb_mqd_base  = RREG32(dev, MES_REG(dev, CP_MQD_BASE_ADDR));

    mes_grbm_select(dev, 0, 0, 0, 0);

    MES_LOG("queue_init: pipe %u, ring %#llx (%u dw), doorbell %#x, "
            "pq_ctrl=%#x db_ctrl=%#x",
            p, (unsigned long long)inst.ring_bus,
            inst.ring_size_dwords, inst.doorbell_index,
            pq_ctrl, db_ctrl);
    MES_LOG("queue_init: readback CP_MQD_BASE_ADDR=%#x CP_HQD_PQ_BASE=%#x "
            "CP_HQD_PQ_CONTROL=%#x CP_HQD_PQ_DOORBELL_CONTROL=%#x "
            "CP_HQD_ACTIVE=%#x",
            rb_mqd_base, rb_pq_base, rb_pq_ctrl, rb_db_ctrl, rb_active);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_ring_write — write `n_dw` dwords to the SCHED ring at the
// current software wptr (wraps modulo ring size).
//------------------------------------------------------------------
static uint32_t
mes_ring_write(MESInstance &inst, const uint32_t *src, uint32_t n_dw)
{
    if (!inst.inited || n_dw == 0) return 0;
    if (n_dw > inst.ring_size_dwords) return 0;
    auto *ring = static_cast<uint32_t *>(inst.ring_cpu);
    if (ring == nullptr || inst.wb_cpu == nullptr) return 0;
    // Track wptr inside the cmd_buf slot we never use — reuse the
    // upper part of the wb page after the rptr/wptr shadow.
    auto *wb_bytes = static_cast<volatile uint8_t *>(inst.wb_cpu);
    volatile uint32_t *sw_wptr = reinterpret_cast<volatile uint32_t *>(
        wb_bytes + 0x80);
    uint32_t wptr = *sw_wptr;
    const uint32_t mask = inst.ring_size_dwords - 1u;
    for (uint32_t i = 0; i < n_dw; i++) {
        ring[(wptr + i) & mask] = src[i];
    }
    wptr = (wptr + n_dw) & mask;
    *sw_wptr = wptr;
    sysmem_wmb();   // deviation 8 — ring bytes land before the doorbell
    return n_dw;
}

//------------------------------------------------------------------
// mes_kick_doorbell — BAR2 write at (doorbell_index * 4, dword-indexed aperture). The
// MES sees a new wptr value and dispatches packets up to it.
//------------------------------------------------------------------
static kern_return_t
mes_kick_doorbell(const DeviceContext &dev, const MESInstance &inst)
{
    if (!inst.inited || inst.wb_cpu == nullptr) return kIOReturnNotReady;
    if (dev.bar2 == nullptr) {
        MES_LOG("kick_doorbell: BAR2 not mapped");
        return kIOReturnNotReady;
    }
    auto *wb_bytes = static_cast<volatile uint8_t *>(inst.wb_cpu);
    volatile uint32_t *sw_wptr = reinterpret_cast<volatile uint32_t *>(
        wb_bytes + 0x80);
    // Linux amdgpu_mm_wdoorbell64(index, ring->wptr): the doorbell aperture is
    // indexed in DWORDS (byte offset = index * 4) and the MES/compute HQD takes
    // its wptr in DWORDS. The reference wrote index*8 with a byte wptr — the
    // HQD's DOORBELL_OFFSET field (dword index) never matched that address, so
    // the scheduler never saw a kick, stage 13 runs 1-3).
    const uint64_t offs = static_cast<uint64_t>(inst.doorbell_index) * 4ull;
    const uint32_t v = *sw_wptr;
    WDOORBELL32(dev, offs, v);
    MES_LOG("kick_doorbell: BAR2+%#llx <= %#x (sw_wptr %u dw)",
            (unsigned long long)offs, v, *sw_wptr);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_submit_pkt — write a 64-dword API frame to the ring, chain a
// QUERY_SCHEDULER_STATUS frame for fence acknowledgement, kick the
// doorbell, poll the status slot.
//------------------------------------------------------------------
// 0.0.626 (G1 review F1 / F3): the per-call fence sequence and the sticky timeout flag. The sequence is the value MES must write into THIS call's status slot
// (upstream mes_v12_0_submit_pkt_and_poll_completion uses an incrementing seq): a late completion of an earlier frame writes an earlier value and is never read as
// this frame's ack. Kept in 1..0x7fffffff (never 0 = the cleared slot, never bit 31 = the error flag the poll tests).
static volatile uint32_t gMesFenceSeq = 0;
static volatile uint32_t gMesTimeoutSeen = 0;
bool mes_timeout_seen() { return __atomic_load_n(&gMesTimeoutSeen, __ATOMIC_SEQ_CST) != 0u; }

kern_return_t
mes_submit_pkt(const DeviceContext &dev, MESContext &mes, MESPipe pipe,
               const uint32_t *pkt, uint32_t api_status_off_dw,
               uint64_t timeout_us)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    MESInstance &inst = mes.pipe[p];
    if (!inst.inited || !inst.enabled) return kIOReturnNotReady;
    if (pkt == nullptr || api_status_off_dw + 4 > kMES_API_FRAME_DWORDS) {
        return kIOReturnBadArgument;
    }

    // Status slot in the WB page at +0xC0 (reserved area beyond
    // rptr/wptr/wptr-poll). MES writes our 64-bit fence_value here.
    auto *wb_bytes = static_cast<volatile uint8_t *>(inst.wb_cpu);
    volatile uint64_t *status_slot = reinterpret_cast<volatile uint64_t *>(
        wb_bytes + 0xC0);
    *status_slot = 0;
    const uint64_t status_gpu = inst.wb_bus + 0xC0;
    uint32_t fence_seq;
    do { fence_seq = __atomic_add_fetch(&gMesFenceSeq, 1u, __ATOMIC_SEQ_CST) & 0x7FFFFFFFu; } while (fence_seq == 0u);
    const uint64_t fence_value = fence_seq;   // 0.0.626 (F1): this frame's own value, compared below (it was the constant 1)

    // Patch the embedded MES_API_Status fence_addr / fence_value.
    uint32_t frame[kMES_API_FRAME_DWORDS];
    memcpy(frame, pkt, sizeof(frame));
    auto *st = reinterpret_cast<MES_API_Status *>(
        reinterpret_cast<uint8_t *>(frame) + api_status_off_dw * 4u);
    st->fence_addr  = status_gpu;
    st->fence_value = fence_value;

    MES_LOG("submit_pkt: pipe %u header=%#x api_status@dw%u fence_addr=%#llx "
            "fence_value=%llu",
            p, frame[0], api_status_off_dw,
            (unsigned long long)status_gpu,
            (unsigned long long)fence_value);

    if (mes_ring_write(inst, frame, kMES_API_FRAME_DWORDS) !=
        kMES_API_FRAME_DWORDS) {
        return kIOReturnNoSpace;
    }

    // Chain a QUERY_SCHEDULER_STATUS — its own status slot at +0xD0.
    volatile uint64_t *q_slot = reinterpret_cast<volatile uint64_t *>(
        wb_bytes + 0xD0);
    *q_slot = 0;
    uint32_t q[kMES_API_FRAME_DWORDS];
    memset(q, 0, sizeof(q));
    q[0] = mes_api_header(kMES_API_TYPE_SCHEDULER,
                          MESSchOp::QUERY_SCHEDULER_STATUS,
                          kMES_API_FRAME_DWORDS);
    // status footprint also at the end of QUERY frame — upstream
    // places it at the same offset as SET_HW_RSRC. We'll put it
    // at dword 60 (16-byte aligned, 4 dwords) for simplicity.
    auto *qst = reinterpret_cast<MES_API_Status *>(
        reinterpret_cast<uint8_t *>(q) + 60u * 4u);
    qst->fence_addr  = inst.wb_bus + 0xD0;
    qst->fence_value = fence_value;

    if (mes_ring_write(inst, q, kMES_API_FRAME_DWORDS) !=
        kMES_API_FRAME_DWORDS) {
        return kIOReturnNoSpace;
    }

    kern_return_t r = mes_kick_doorbell(dev, inst);
    if (r != kIOReturnSuccess) return r;

    // Poll status_slot. Success = lower 32 bits == this frame's fence_value (0.0.626: it used to be the constant 1).
    //
    // Deviation 6: the reference "waited" 100 us per iteration with a
    // 2000-iteration dummy-read loop. We use a real IODelay(100) for
    // the first 10 ms (MES answers in microseconds when it is alive)
    // and IOSleep(1) after that so a wedged scheduler does not spin a
    // core for the whole 2 s timeout. Timeout value unchanged.
    constexpr uint64_t kSpinPhaseUs = 10000;
    uint64_t elapsed = 0;
    while (elapsed < timeout_us) {
        sysmem_rmb();
        uint64_t v = *status_slot;
        if ((v & 0xFFFFFFFFull) == fence_value) {
            MES_LOG("submit_pkt: pipe %u ok in ~%llu us (status=%#llx, "
                    "chained query slot=%#llx)",
                    p, (unsigned long long)elapsed,
                    (unsigned long long)v, (unsigned long long)*q_slot);
            return kIOReturnSuccess;
        }
        if ((v >> 31) & 0x1) {
            MES_LOG("submit_pkt: pipe %u error status=%#llx",
                    p, (unsigned long long)v);
            return kIOReturnInternalError;
        }
        if (elapsed < kSpinPhaseUs) {
            IODelay(100);
            elapsed += 100;
        } else {
            IOSleep(1);
            elapsed += 1000;
        }
    }
    __atomic_store_n(&gMesTimeoutSeen, 1u, __ATOMIC_SEQ_CST);   // 0.0.626 (F3): sticky for the boot
    MES_LOG("submit_pkt: pipe %u timeout (last status=%#llx, query=%#llx, "
            "CP_MES_INSTR_PNTR=%#x)",
            p, (unsigned long long)*status_slot,
            (unsigned long long)*q_slot,
            dev.ip.isResolved(IPBlock::GC)
                ? RREG32(dev, MES_REG(dev, CP_MES_INSTR_PNTR))
                : 0u);
    return kIOReturnTimeout;
}

//------------------------------------------------------------------
// mes_query_sched_status — convenience wrapper. Sends a no-payload
// QUERY frame and checks MES echoes the fence.
//------------------------------------------------------------------
kern_return_t
mes_query_sched_status(const DeviceContext &dev, MESContext &mes,
                       MESPipe pipe)
{
    uint32_t pkt[kMES_API_FRAME_DWORDS];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = mes_api_header(kMES_API_TYPE_SCHEDULER,
                            MESSchOp::QUERY_SCHEDULER_STATUS,
                            kMES_API_FRAME_DWORDS);
    // MESAPI__QUERY_MES_STATUS = { header (dw0), bool mes_healthy (padded), MES_API_STATUS } →
    // api_status lives at byte 8 = dword 2 (mes_v12_api_def.h). dw60 (the reference's guess) never
    // received the completion fence on hardware.
    return mes_submit_pkt(dev, mes, pipe, pkt, /*status_off=*/2, 2000000);
}

//------------------------------------------------------------------
// mes_set_hw_resources — port of mes_v12_0_set_hw_resources for
// the SCHED pipe. Lazy-allocates the scheduler context + status-
// fence buffers (4 KB sysmem each) on first call.
//------------------------------------------------------------------
kern_return_t
mes_set_hw_resources(DeviceContext &dev, GMCContext &gmc, MESContext &mes,
                     const MESSetHwResourcesInput &in)
{
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // 1) Lazy-alloc scheduler context + status fence.
    if (mes.sch_ctx_bus == 0) {
        void *cpu = nullptr;
        kern_return_t r = mes_alloc_gtt_block(dev, gmc, kMES_SchCtxBytes,
                                              mes.sch_ctx_mem,
                                              &mes.sch_ctx_bus, &cpu,
                                              "sch_ctx");
        if (r != kIOReturnSuccess) return r;
    }
    if (mes.status_fence_bus == 0) {
        void *cpu = nullptr;
        kern_return_t r = mes_alloc_gtt_block(dev, gmc, kMES_StatusFenceBytes,
                                              mes.status_fence_mem,
                                              &mes.status_fence_bus, &cpu,
                                              "status_fence");
        if (r != kIOReturnSuccess) return r;
    }

    // 2) Build the 64-dword frame.
    MES_SetHwResources pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::SET_HW_RSRC,
                                       kMES_API_FRAME_DWORDS);
    pkt.vmid_mask_mmhub  = in.vmid_mask_mmhub;
    pkt.vmid_mask_gfxhub = in.vmid_mask_gfxhub;
    pkt.gds_size         = 0;
    pkt.paging_vmid      = 0;
    for (int i = 0; i < 8; i++) pkt.compute_hqd_mask[i] = in.compute_hqd_mask[i];
    for (int i = 0; i < 2; i++) pkt.gfx_hqd_mask[i]     = in.gfx_hqd_mask[i];
    for (int i = 0; i < 2; i++) pkt.sdma_hqd_mask[i]    = in.sdma_hqd_mask[i];
    for (int i = 0; i < 5; i++) pkt.aggregated_doorbells[i] = in.aggregated_doorbells[i];

    pkt.g_sch_ctx_gpu_mc_ptr              = mes.sch_ctx_bus;
    pkt.query_status_fence_gpu_mc_ptr     = mes.status_fence_bus;

    // gc_base / mmhub_base / osssys_base — upstream
    // mes_v12_0_set_hw_resources (mes_v12_0.c:979-985) copies FIVE
    // segments per IP:
    //     for (i = 0; i < 5; i++)
    //         pkt.gc_base[i] = adev->reg_offset[GC_HWIP][0][i];
    // MES firmware then resolves its own registers as
    // base[BASE_IDX] + offset, exactly like the driver. The reference
    // filled only slot 0 and zeroed the rest, so every register MES
    // reaches at BASE_IDX 1 (the whole CP_MES_* block, GRBM_GFX_CNTL,
    // RLC_CP_SCHEDULERS) resolved against base 0 — the same class of
    // bug this audit fixes on the driver side. Our IPBaseTable does
    // track all segments (getBase(block, idx)), so fill all five.
    //
    // Deviation 7: the reference reads IPBlock::GMC for mmhub_base;
    // our discovery fills IPBlock::MMHUB (hwid 34) and leaves GMC at
    // the unresolved sentinel, so prefer MMHUB and fall back to GMC.
    // Unresolved segments go out as 0 rather than 0xFFFFFFFF.
    auto seg_or_zero = [&](IPBlock b, int i) -> uint32_t {
        return dev.ip.isResolved(b, i) ? dev.ip.getBase(b, i) : 0u;
    };
    const IPBlock mmhub_blk = dev.ip.isResolved(IPBlock::MMHUB)
                            ? IPBlock::MMHUB : IPBlock::GMC;
    constexpr int kMESBaseSegments = 5;   // mes_v12_0.c:979
    for (int i = 0; i < kMESBaseSegments; i++) {
        pkt.gc_base[i]     = seg_or_zero(IPBlock::GC, i);
        pkt.mmhub_base[i]  = seg_or_zero(mmhub_blk, i);
        pkt.osssys_base[i] = seg_or_zero(IPBlock::OSSSYS, i);
    }

    // Flags match mes_v12_0_set_hw_resources (mes_v12_0.c:780-792):
    //   disable_reset = 1, disable_mes_log = 1,
    //   use_different_vmid_compute = 1, enable_reg_active_poll = 1,
    //   enable_level_process_quantum_check = 1,
    //   unmapped_doorbell_handling = 1 (basic version)
    pkt.flags = kSetHwRsrcFlag_disable_reset
              | kSetHwRsrcFlag_disable_mes_log
              | kSetHwRsrcFlag_use_different_vmid_compute
              | kSetHwRsrcFlag_enable_reg_active_poll
              | kSetHwRsrcFlag_enable_level_process_quantum_check
              | kSetHwRsrcFlag_unmapped_doorbell_handling_BASIC;

    // mes_v12_0.c:791 — oversubscription_timer is 0 for sched_version
    // < 0x8b and 50 otherwise. We pick 50 for safety since RDNA4
    // ships ≥ 0x4b firmware. (No SDMA-only sched_version split is
    // exposed in our struct.)
    pkt.oversubscription_timer = 50;

    MES_LOG("set_hw_resources: header=%#x flags=%#x oversub=%u "
            "vmid_mmhub=%#x vmid_gfxhub=%#x",
            pkt.header.u32All, pkt.flags, pkt.oversubscription_timer,
            pkt.vmid_mask_mmhub, pkt.vmid_mask_gfxhub);
    MES_LOG("set_hw_resources: sch_ctx_mc=%#llx status_fence_mc=%#llx",
            (unsigned long long)pkt.g_sch_ctx_gpu_mc_ptr,
            (unsigned long long)pkt.query_status_fence_gpu_mc_ptr);
    MES_LOG("set_hw_resources: gc_base[0..4]=%#x/%#x/%#x/%#x/%#x",
            pkt.gc_base[0], pkt.gc_base[1], pkt.gc_base[2],
            pkt.gc_base[3], pkt.gc_base[4]);
    MES_LOG("set_hw_resources: mmhub_base[0..4]=%#x/%#x/%#x/%#x/%#x",
            pkt.mmhub_base[0], pkt.mmhub_base[1], pkt.mmhub_base[2],
            pkt.mmhub_base[3], pkt.mmhub_base[4]);
    MES_LOG("set_hw_resources: osssys_base[0..4]=%#x/%#x/%#x/%#x/%#x",
            pkt.osssys_base[0], pkt.osssys_base[1], pkt.osssys_base[2],
            pkt.osssys_base[3], pkt.osssys_base[4]);
    MES_LOG("set_hw_resources: gfx_hqd=%#x/%#x sdma_hqd=%#x/%#x "
            "compute_hqd[0..3]=%#x/%#x/%#x/%#x",
            pkt.gfx_hqd_mask[0], pkt.gfx_hqd_mask[1],
            pkt.sdma_hqd_mask[0], pkt.sdma_hqd_mask[1],
            pkt.compute_hqd_mask[0], pkt.compute_hqd_mask[1],
            pkt.compute_hqd_mask[2], pkt.compute_hqd_mask[3]);

    // 3) Submit. api_status sits at byte offsetof(MES_SetHwResources,
    //    api_status); convert to dword offset for mes_submit_pkt.
    const uint32_t api_status_dw =
        offsetof(MES_SetHwResources, api_status) / 4;
    return mes_submit_pkt(dev, mes, MESPipe::Sched,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          api_status_dw,
                          /*timeout_us=*/2000000);
}

//------------------------------------------------------------------
// mes_add_hw_queue — port of mes_v12_0_add_hw_queue.
//------------------------------------------------------------------
kern_return_t
mes_add_hw_queue(const DeviceContext &dev, MESContext &mes,
                 const MESAddQueueInput &in)
{
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    MES_AddQueue pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::ADD_QUEUE,
                                       kMES_API_FRAME_DWORDS);
    pkt.process_id              = in.process_id;
    pkt.page_table_base_addr    = in.page_table_base_addr;
    pkt.process_va_start        = 0;
    pkt.process_va_end          = 0;
    pkt.process_quantum         = 0;
    pkt.process_context_addr    = in.process_context_addr;
    pkt.gang_quantum            = 0;
    pkt.gang_context_addr       = in.gang_context_addr;
    pkt.inprocess_gang_priority = in.inprocess_gang_priority;
    pkt.gang_global_priority_level = in.gang_global_priority_level;
    pkt.doorbell_offset         = in.doorbell_offset;
    pkt.mqd_addr                = in.mqd_addr;
    pkt.wptr_addr               = in.wptr_addr;
    pkt.queue_type              = in.queue_type;
    pkt.pipe_id                 = in.pipe_id;
    pkt.queue_id                = in.queue_id;
    pkt.flags                   = in.flags;

    MES_LOG("add_hw_queue: type=%u pipe=%u queue=%u doorbell=%#x "
            "mqd_mc=%#llx wptr_mc=%#llx pt_base=%#llx flags=%#x prio=%u/%u",
            pkt.queue_type, pkt.pipe_id, pkt.queue_id, pkt.doorbell_offset,
            (unsigned long long)pkt.mqd_addr,
            (unsigned long long)pkt.wptr_addr,
            (unsigned long long)pkt.page_table_base_addr,
            pkt.flags, pkt.inprocess_gang_priority,
            pkt.gang_global_priority_level);

    const uint32_t api_status_dw =
        offsetof(MES_AddQueue, api_status) / 4;
    return mes_submit_pkt(dev, mes, MESPipe::Sched,
                          reinterpret_cast<const uint32_t *>(&pkt),
                          api_status_dw,
                          /*timeout_us=*/2000000);
}

// ----- mes_remove_hw_queue — port of mes_v12_0_unmap_legacy_queue -----
//
// The teardown counterpart of mes_add_hw_queue. Upstream sends this from
// amdgpu_mes_unmap_legacy_queue() before a queue's memory goes away; we send
// it from bringup_teardown() for the same reason. `unmap_legacy_queue = 1` is
// the plain unmap (action UNMAP_LATENCY / RESET_QUEUES); the preempt variant
// with a trailing fence is not needed for teardown.
kern_return_t
mes_remove_hw_queue(const DeviceContext &dev, MESContext &mes,
                    uint32_t queue_type, uint32_t pipe_id,
                    uint32_t queue_id, uint32_t doorbell_offset)
{
    return mes_remove_hw_queue_flags(dev, mes, queue_type, pipe_id, queue_id, doorbell_offset,
                                     kRemoveQueueFlag_unmap_legacy_queue);
}

// 0.0.623 (G1): the same frame with the flags word given. mes_remove_hw_queue above is exactly this with unmap_legacy_queue (the frame it sent
// before 0.0.623, byte for byte).
kern_return_t
mes_remove_hw_queue_flags(const DeviceContext &dev, MESContext &mes,
                          uint32_t queue_type, uint32_t pipe_id,
                          uint32_t queue_id, uint32_t doorbell_offset,
                          uint32_t flags)
{
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    MES_RemoveQueue pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                       MESSchOp::REMOVE_QUEUE,
                                       kMES_API_FRAME_DWORDS);
    pkt.doorbell_offset   = doorbell_offset;
    pkt.gang_context_addr = 0;
    pkt.pipe_id           = pipe_id;
    pkt.queue_id          = queue_id;
    pkt.queue_type        = queue_type;
    pkt.flags             = flags;

    MES_LOG("remove_hw_queue: type=%u pipe=%u queue=%u doorbell=%#x "
            "flags=%#x (unmap_legacy_queue%s)", queue_type, pipe_id, queue_id, doorbell_offset,
            flags, (flags & kRemoveQueueFlag_remove_after_reset) ? " + remove_queue_after_reset" : "");

    const uint32_t api_status_dw = offsetof(MES_RemoveQueue, api_status) / 4;
    kern_return_t r = mes_submit_pkt(dev, mes, MESPipe::Sched,
                                     reinterpret_cast<const uint32_t *>(&pkt),
                                     api_status_dw,
                                     /*timeout_us=*/1000000);
    MES_LOG("remove_hw_queue: %s (%#x)",
            r == kIOReturnSuccess ? "acked" : "not acked", r);
    return r;
}

// ----- mes_reset_legacy_gfx_queue — mes_v12_0_reset_hw_queue, legacy_gfx -----
//
// 0.0.623 (G1, hang recovery). mes_v12_0.c:1096-1140 with input->use_mmio false and input->legacy_gfx true (the path gfx_v12_0_reset_kgq ->
// amdgpu_gfx_mes_reset_queue -> amdgpu_mes_reset_legacy_queue takes for the kernel GFX ring: gfx_v12_0.c:1608 me.use_mmio_for_reset = false):
//
//     header RESET; queue_type = GFX; reset_legacy_gfx = 1; pipe_id_lp = ring->pipe; queue_id_lp = ring->queue;
//     mqd_mc_addr_lp = MQD gpu addr; doorbell_offset_lp = ring->doorbell_index; wptr_addr_lp = ring->wptr_gpu_addr; vmid_id_lp = vmid
//
// on the SCHED pipe through mes_v12_0_submit_pkt_and_poll_completion (our mes_submit_pkt, 2 s bound). Nothing else in the frame is set.
// No register is written by this function itself: the frame goes into the MES ring and mes_submit_pkt kicks the MES doorbell (one doorbell write), exactly as every other MES API call.
kern_return_t
mes_reset_legacy_gfx_queue(const DeviceContext &dev, MESContext &mes,
                           uint32_t pipe_id, uint32_t queue_id, uint32_t vmid,
                           uint64_t mqd_addr, uint32_t doorbell_offset,
                           uint64_t wptr_addr)
{
    if (!mes.pipe[0].inited || !mes.pipe[0].enabled) return kIOReturnNotReady;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    MES_ResetQueue pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.u32All      = mes_api_header(kMES_API_TYPE_SCHEDULER,
                                            MESSchOp::RESET,
                                            kMES_API_FRAME_DWORDS);
    pkt.queue_type         = kMESQueueType_GFX;
    pkt.flags              = kResetFlag_reset_legacy_gfx;
    pkt.pipe_id_lp         = pipe_id;
    pkt.queue_id_lp        = queue_id;
    pkt.vmid_id_lp         = vmid;
    pkt.mqd_mc_addr_lp     = mqd_addr;
    pkt.doorbell_offset_lp = doorbell_offset;
    pkt.wptr_addr_lp       = wptr_addr;

    MES_LOG("reset_legacy_gfx_queue: pipe=%u queue=%u vmid=%u doorbell=%#x "
            "mqd_mc=%#llx wptr_mc=%#llx (RESET reset_legacy_gfx)", pipe_id, queue_id, vmid,
            doorbell_offset, (unsigned long long)mqd_addr, (unsigned long long)wptr_addr);

    const uint32_t api_status_dw = offsetof(MES_ResetQueue, api_status) / 4;
    kern_return_t r = mes_submit_pkt(dev, mes, MESPipe::Sched,
                                     reinterpret_cast<const uint32_t *>(&pkt),
                                     api_status_dw,
                                     /*timeout_us=*/2000000);
    MES_LOG("reset_legacy_gfx_queue: %s (%#x)",
            r == kIOReturnSuccess ? "acked" : "not acked", r);
    return r;
}

//------------------------------------------------------------------
// mes_kiq_setting — port of mes_v12_0_kiq_setting (mes_v12_0.c:1728).
//
// Writes RLC_CP_SCHEDULERS to identify the KIQ ring for RLC's
// IRQ-routing logic. Upstream packs:
//     value = (existing & 0xffffff00)
//           | (me << 5) | (pipe << 3) | (queue)
//           | 0x80   /* enable scheduler */
//
// For uni-MES on RDNA4 the KIQ queue is mes.ring[KIQ_PIPE] with
// me=3, pipe=1, queue=0 — but we accept any (me, pipe, queue) tuple
// so the bringup orchestrator can also call this for the legacy
// gfx.kiq[0] ring if uni_mes is ever disabled.
//
// Audit-7 #5/#6.
//------------------------------------------------------------------
kern_return_t
mes_kiq_setting(const DeviceContext &dev, uint32_t me, uint32_t pipe,
                uint32_t queue)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t reg =
        MES_REG(dev, RLC_CP_SCHEDULERS);

    // mes_v12_0.c:1734-1737 — RMW preserving the high bytes that
    // RLC owns for its own state machine. The low byte encodes
    // (me, pipe, queue) and the 0x80 bit flips on the scheduler.
    uint32_t tmp = RREG32(dev, reg);
    MES_LOG("kiq_setting: RLC_CP_SCHEDULERS before = %#x", tmp);
    tmp &= 0xffffff00u;
    tmp |= ((me & 0x7u) << 5) | ((pipe & 0x3u) << 3) | (queue & 0x7u);
    WREG32(dev, reg, tmp | 0x80u);

    MES_LOG("kiq_setting: me=%u pipe=%u queue=%u RLC_CP_SCHEDULERS=%#x "
            "(readback %#x)",
            me, pipe, queue, tmp | 0x80u, RREG32(dev, reg));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_enable_unmapped_doorbell_handling — port of
// mes_v12_0_enable_unmapped_doorbell_handling (mes_v12_0.c:863).
//
// Audit-7 #6.
//------------------------------------------------------------------
kern_return_t
mes_enable_unmapped_doorbell_handling(const DeviceContext &dev, bool enable)
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;
    const uint32_t reg =
        MES_REG(dev, CP_UNMAPPED_DOORBELL);

    // mes_v12_0.c:867-880 — read-modify-write. PROC_LSB encodes the
    // bit position that selects the doorbell page; 0xd matches KFD's
    // 2-page-per-process convention.
    uint32_t data = RREG32(dev, reg);
    MES_LOG("unmapped_doorbell_handling: CP_UNMAPPED_DOORBELL before = %#x",
            data);
    data &= ~static_cast<uint32_t>(CP_UNMAPPED_DOORBELL__PROC_LSB_MASK);
    data |= 0xdu << CP_UNMAPPED_DOORBELL__PROC_LSB__SHIFT;
    if (enable) data |= (1u << CP_UNMAPPED_DOORBELL__ENABLE__SHIFT);
    else        data &= ~(1u << CP_UNMAPPED_DOORBELL__ENABLE__SHIFT);
    WREG32(dev, reg, data);

    MES_LOG("unmapped_doorbell_handling %s: CP_UNMAPPED_DOORBELL=%#x "
            "(readback %#x)",
            enable ? "enabled" : "disabled", data, RREG32(dev, reg));
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_read_sched_version — port of the inline RREG32 in
// mes_v12_0_queue_init (mes_v12_0.c:1499-1512).
//
// Sequence (must run AFTER mes_enable(true)):
//   GRBM-select MES pipe → read CP_MES_GP3_LO → store on mes
//   → GRBM-deselect.
//
// Audit-7 #6.
//------------------------------------------------------------------
kern_return_t
mes_read_sched_version(const DeviceContext &dev, MESContext &mes,
                       MESPipe pipe)
{
    const uint32_t p = static_cast<uint32_t>(pipe);
    if (p >= kMaxMESPipes) return kIOReturnBadArgument;
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    mes_grbm_select(dev, /*me=*/3, /*pipe=*/p, /*queue=*/0, /*vmid=*/0);
    const uint32_t v = RREG32(dev,
        MES_REG(dev, CP_MES_GP3_LO));
    mes_grbm_select(dev, 0, 0, 0, 0);

    if (pipe == MESPipe::Sched) mes.sched_version = v;
    else                        mes.kiq_version   = v;

    MES_LOG("sched_version (pipe %u) = %#x (masked = %#x)",
            p, v, v & kMES_VERSION_MASK);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_init_aggregated_doorbell — port of mes_v12_0_init_aggregated_doorbell.
// Programs CP_MES_DOORBELL_CONTROL1..5 with the 5 priority doorbells
// and sets CP_HQD_GFX_CONTROL.DB_UPDATED_MSG_EN.
//------------------------------------------------------------------
kern_return_t
mes_init_aggregated_doorbell(const DeviceContext &dev,
                             const uint32_t doorbells[5])
{
    if (!dev.ip.isResolved(IPBlock::GC)) return kIOReturnNotReady;

    // All five CP_MES_DOORBELL_CONTROLn are BASE_IDX 1 per
    // gc_12_0_0_offset.h (the reference used 0).
    auto reg = [&](uint32_t r) {
        return SOC15_REG_OFFSET_BIDX(dev, IPBlock::GC,
                                     MESRegBaseIdx::kMESDoorbellControlBaseIdx, r);
    };

    const uint32_t ctrl_regs[5] = {
        MESRegs::CP_MES_DOORBELL_CONTROL1,
        MESRegs::CP_MES_DOORBELL_CONTROL2,
        MESRegs::CP_MES_DOORBELL_CONTROL3,
        MESRegs::CP_MES_DOORBELL_CONTROL4,
        MESRegs::CP_MES_DOORBELL_CONTROL5,
    };
    const uint32_t clear_mask =
        CP_MES_DOORBELL_CONTROL1__DOORBELL_OFFSET_MASK
      | CP_MES_DOORBELL_CONTROL1__DOORBELL_EN_MASK
      | CP_MES_DOORBELL_CONTROL1__DOORBELL_HIT_MASK;

    for (int i = 0; i < 5; i++) {
        uint32_t v = RREG32(dev, reg(ctrl_regs[i]));
        const uint32_t before = v;
        v &= ~clear_mask;
        v = REG_SET_FIELD(v, CP_MES_DOORBELL_CONTROL1,
                          DOORBELL_OFFSET, doorbells[i]);
        v = REG_SET_FIELD(v, CP_MES_DOORBELL_CONTROL1, DOORBELL_EN, 1);
        WREG32(dev, reg(ctrl_regs[i]), v);
        MES_LOG("aggregated_doorbell: CTRL%d (reg %#x) %#x -> %#x "
                "(readback %#x, offset %#x)",
                i + 1, ctrl_regs[i], before, v,
                RREG32(dev, reg(ctrl_regs[i])), doorbells[i]);
    }

    // Final touch: gate the GFX queue update msg through to MES.
    uint32_t v = (1u << CP_HQD_GFX_CONTROL__DB_UPDATED_MSG_EN__SHIFT);
    const uint32_t gfx_ctrl_before = RREG32(dev, MES_REG(dev, CP_HQD_GFX_CONTROL));
    WREG32(dev, MES_REG(dev, CP_HQD_GFX_CONTROL), v);
    MES_LOG("aggregated_doorbell: CP_HQD_GFX_CONTROL %#x -> %#x "
            "(readback %#x)",
            gfx_ctrl_before, v, RREG32(dev, MES_REG(dev, CP_HQD_GFX_CONTROL)));

    MES_LOG("aggregated_doorbell: LOW=%#x NORMAL=%#x MED=%#x HIGH=%#x RT=%#x",
            doorbells[0], doorbells[1], doorbells[2], doorbells[3], doorbells[4]);
    return kIOReturnSuccess;
}

//------------------------------------------------------------------
// mes_init_full — MESInit bringup stage.
//------------------------------------------------------------------
kern_return_t
mes_init_full(DeviceContext &dev, PSPContext &psp,
              GMCContext &gmc, MESContext &mes,
              const uint8_t *mes_fw, uint64_t mes_fw_size)
{
    (void)psp;

    if (!dev.ip.isResolved(IPBlock::GC)) {
        MES_LOG("init_full: GC IP base not resolved");
        return kIOReturnNotReady;
    }

    // Always allocate storage. mes_enable runs only when microcode
    // has been loaded for the SCHED pipe.
    kern_return_t r = mes_alloc_storage(dev, gmc, mes.pipe[0]);
    if (r != kIOReturnSuccess) return r;
    if (!kEnableUniMES) {
        r = mes_alloc_storage(dev, gmc, mes.pipe[1]);
        if (r != kIOReturnSuccess) return r;
    }
    mes.uni_mes_active = kEnableUniMES;

    // Deviation 3: the reference's host-side LoadFirmware pushed the
    // MES microcode through the PSP *and* stashed uc_start_addr from
    // the file header. In this kext the LOAD_IP_FW half already ran in
    // BringupStage::PSPFwLoad (psp_load_non_psp_fw step 6, which sends
    // CP_MES + CP_MES_DATA); only the header parse is missing, so do
    // it here from the embedded uni_mes blob.
    if (mes.pipe[0].uc_start_addr == 0) {
        const uint8_t *bin  = mes_fw;
        uint64_t       size = mes_fw_size;
        if (bin == nullptr || size == 0) {
            const FwBlob *blob = fw_get(FwId::GC_UNI_MES);
            if (blob != nullptr && blob->data != nullptr && blob->size != 0) {
                bin  = blob->data;
                size = blob->size;
                MES_LOG("init_full: using embedded blob %s (%llu B)",
                        blob->name, (unsigned long long)blob->size);
            }
        }
        if (bin != nullptr && size != 0) {
            kern_return_t hr = mes_parse_ucode_header(mes, MESPipe::Sched,
                                                      bin, size);
            if (hr != kIOReturnSuccess) {
                MES_LOG("init_full: MES firmware header parse failed (%#x)",
                        hr);
            }
        } else {
            MES_LOG("init_full: no MES firmware blob available for the "
                    "uc_start_addr parse");
        }
    }

    if (!mes.sched_ucode_loaded) {
        MES_LOG("init_full: storage allocated; awaiting MES microcode "
                "via LoadFirmware before enabling pipe 0");
        return kIOReturnSuccess;
    }

    // Audit-7 #5: order MUST be enable → queue_init.
    //
    // Upstream mes_v12_0_hw_init (mes_v12_0.c:1820) sequence:
    //   1) mes_v12_0_enable(true)           — MES microcode running
    //   2) mes_v12_0_enable_unmapped_doorbell_handling(true)
    //   3) mes_v12_0_queue_init(SCHED_PIPE) — programs HQD live
    //   4) mes_v12_0_set_hw_resources(SCHED_PIPE)
    //   5) [if sched_version >= 0x4b] mes_v12_0_set_hw_resources_1
    //   6) mes_v12_0_init_aggregated_doorbell
    //   7) mes_v12_0_query_sched_status
    //
    // Writing CP_HQD_* while MES pipe 0 is still in reset (the bug
    // we had) makes the HQD registers latch the new values on the
    // wrong side of MES's internal state machine — some writes are
    // dropped because the GRBM_GFX_CNTL-selected pipe context isn't
    // backed by a running MES yet.

    // (0) Tell RLC about the KIQ queue BEFORE enabling MES — upstream
    // does this in mes_v12_0_kiq_hw_init (mes_v12_0.c:1748), which
    // runs before mes_v12_0_enable in the kiq_hw_init flow. For uni-MES
    // KIQ pipe (1) shares microcode with sched pipe (0) but RLC
    // routing must be set first.  Audit-7 #5/#6.
    mes_kiq_setting(dev, /*me=*/3, /*pipe=*/1, /*queue=*/0);

    // (1) Enable MES microcode.
    r = mes_enable(dev, mes, true);
    if (r != kIOReturnSuccess) return r;

    // (2) Enable unmapped doorbell handling.  Audit-7 #6. Runs AFTER
    // mes_enable per mes_v12_0_hw_init (mes_v12_0.c:1849).
    mes_enable_unmapped_doorbell_handling(dev, true);

    // (3) Program the SCHED HQD AFTER MES is running.
    r = mes_queue_init(dev, mes, MESPipe::Sched);
    if (r != kIOReturnSuccess) return r;

    // Read sched_version from CP_MES_GP3_LO — gates set_hw_resources_1.
    mes_read_sched_version(dev, mes, MESPipe::Sched);

    // (6) Program aggregated doorbells (5 priority levels). We pick a
    // 5-slot window starting at kMES_AggregatedDoorbellsBase.
    uint32_t doorbells[kMES_PriorityLevels];
    // 0.0.18: these come from the one owner of the BAR2 layout,
    // DeviceContext::doorbell.index.mes_aggregated[] (amdgpu_ip.h), spaced
    // two DWORDs apart as amdgpu_mes.c:55-56 does, and clear of SDMA's
    // DWORD range. Fall back to the constant if doorbell_init never ran.
    for (uint32_t i = 0; i < kMES_PriorityLevels; i++) {
        doorbells[i] = dev.doorbell.index.max_assignment != 0
                           ? dev.doorbell.index.mes_aggregated[i]
                           : kMES_AggregatedDoorbellsBase + i * 2;
    }
    mes_init_aggregated_doorbell(dev, doorbells);

    // (4) Tell MES which hw resources it owns. VMID 0 stays kernel-only;
    // VMIDs 1..7 are MES-scheduled compute VMIDs. We keep GFX HQD 0
    // for the direct CP_RB0 path (used by SubmitIB/SubmitTestPM4)
    // so gfx_hqd_mask[0] = 0xFE — MES owns 1..7. Compute HQDs are
    // all owned by MES; SDMA HQDs likewise.
    MESSetHwResourcesInput in{};
    in.vmid_mask_mmhub  = 0xFE;
    in.vmid_mask_gfxhub = 0xFE;
    // compute_hqd_mask tells MES which compute HQDs it may schedule into. It
    // must EXCLUDE the ones we map kernel queues onto by hand, or MES owns the
    // slot we then ask it to place a legacy queue in — which is what made
    // ADD_QUEUE(COMPUTE) ack while the queue never ran (0.0.23.
    //
    // Upstream computes it (amdgpu_mes.c amdgpu_mes_get_hqd_mask):
    //     total    = (1 << num_queue_per_pipe) - 1
    //     reserved = (1 << DIV_ROUND_UP(num_compute_rings, num_pipe_per_mec)) - 1
    //     mask     = total & ~reserved
    // GFX12 has 8 queues per pipe and 4 pipes per MEC, and upstream asks for 8
    // compute rings: total 0xFF, reserved 0x3, mask 0xFC — queues 0 and 1 of
    // every pipe left for kernel queues. Only the first MEC's pipes get a mask
    // (amdgpu_mes.c:156); the rest stay zero.
    constexpr uint32_t kComputeQueuesPerPipe = 8;
    constexpr uint32_t kComputePipesPerMec   = 4;
    constexpr uint32_t kKernelComputeRings   = 8;
    const uint32_t total    = (1u << kComputeQueuesPerPipe) - 1u;
    const uint32_t per_pipe = (kKernelComputeRings + kComputePipesPerMec - 1) / kComputePipesPerMec;
    const uint32_t reserved = (1u << per_pipe) - 1u;
    for (int i = 0; i < 8; i++)
        in.compute_hqd_mask[i] = (i < (int)kComputePipesPerMec) ? (total & ~reserved) : 0u;
    MES_LOG("set_hw_resources: compute_hqd_mask = %#x on pipes 0..%u "
            "(queues 0..%u reserved for kernel compute queues)",
            in.compute_hqd_mask[0], kComputePipesPerMec - 1, per_pipe - 1);
    in.gfx_hqd_mask[0]  = 0xFE;
    in.gfx_hqd_mask[1]  = 0x00;
    in.sdma_hqd_mask[0] = 0x0F;
    in.sdma_hqd_mask[1] = 0x0F;
    for (uint32_t i = 0; i < kMES_PriorityLevels; i++) {
        in.aggregated_doorbells[i] = doorbells[i];
    }
    // Failure here is non-fatal: storage + enable succeeded, but the
    // scheduler may not have echoed our SET_HW_RSRC (e.g. microcode
    // version mismatch). Log and continue — userspace can re-attempt
    // via a future selector once we add one.
    kern_return_t sr = mes_set_hw_resources(dev, gmc, mes, in);
    if (sr != kIOReturnSuccess) {
        // Navi48Bringup: made FATAL — a scheduler that never acknowledged
        // SET_HW_RESOURCES cannot schedule anything; the ladder must stop
        // here instead of reporting MESInit as done run 1).
        MES_LOG("init_full: set_hw_resources failed (%#x) — MES enabled "
                "but scheduler not configured; FAILING the stage", sr);
        return sr;
    }

    // (5) Conditional SET_HW_RESOURCES_1. Upstream mes_v12_0.c:1859
    // gates on (sched_version & MASK) >= 0x4b.  Audit-7 #6.
    if ((mes.sched_version & kMES_VERSION_MASK) >=
            kMES_HwResources1MinSchedVersion) {
        kern_return_t r1 = mes_set_hw_resources_1(dev, gmc, mes);
        if (r1 != kIOReturnSuccess) {
            MES_LOG("init_full: set_hw_resources_1 failed (%#x) — "
                    "non-fatal but cleaner-shader fence not registered",
                    r1);
        }
    } else {
        MES_LOG("init_full: sched_version %#x < 0x4b, skipping "
                "SET_HW_RESOURCES_1", mes.sched_version & kMES_VERSION_MASK);
    }

    // (7) Query echoes the running scheduler — confirms MES is alive
    // before we hand it user queues.  Non-fatal failure.
    kern_return_t qr = mes_query_sched_status(dev, mes, MESPipe::Sched);
    if (qr != kIOReturnSuccess) {
        MES_LOG("init_full: query_sched_status failed (%#x) — MES may "
                "be busy or wedged", qr);
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
