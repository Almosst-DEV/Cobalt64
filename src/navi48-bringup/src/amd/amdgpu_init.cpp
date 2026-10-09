// Port of bringup_to / run_stage from lemonade-sdk/mac-amdgpu dext/amdgpu/amdgpu_init.cpp (MIT, 3bdeed2).
// See amdgpu_init.h for the deviations list.
#include "amdgpu_init.h"
#include "amdgpu_log.h"
#include "../fw/fw_table.h"
#include "nbif_v6_3_1.h"

// Navi48Bringup.cpp: the SDMA0_DCC_CNTL no-PTE compression clear (one register write + readback + log line; once per
// boot, `navi48-sdmadcc=0` skips it). Called right after sdma_init_full, before our own SDMA copy test and sweep.
void navi48_sdmadcc_default(void);

namespace amdgpu {

const char *stage_name(BringupStage s) {
    switch (s) {
    case BringupStage::None: return "None";               case BringupStage::IPDiscovery: return "IPDiscovery";
    case BringupStage::IHInit: return "IHInit";           case BringupStage::GMCInit: return "GMCInit";
    case BringupStage::PSPInit: return "PSPInit";         case BringupStage::PSPLoadSOS: return "PSPLoadSOS";
    case BringupStage::PSPRingCreate: return "PSPRingCreate"; case BringupStage::TMRSetup: return "TMRSetup";
    case BringupStage::PSPFwLoad: return "PSPFwLoad";     case BringupStage::SMUInit: return "SMUInit";
    case BringupStage::IMUInit: return "IMUInit";         case BringupStage::RLCInit: return "RLCInit";
    case BringupStage::CPInit: return "CPInit";           case BringupStage::MESInit: return "MESInit";
    case BringupStage::GFXInit: return "GFXInit";         case BringupStage::SDMAInit: return "SDMAInit";
    case BringupStage::PM4Test: return "PM4Test";         case BringupStage::ComputeDispatch: return "ComputeDispatch";
    }
    return "?";
}


// GC-side engines (CP, MES, SDMA) translate through the GFXHUB, so its GART must be
// live before any of them is handed a GART-mapped ring. Linux does this in GMC hw_init;
// here the GC block is only accessible once the SMU has powered it (stage 9), so we run
// it lazily before the first GC-engine stage instead of at GMCInit.

// NBIO doorbell path (Linux soc24 common hw_init + gfx hw_init gc_doorbell_init):
// doorbell aperture enable, selfring aperture at the BAR2 physical base, GDC S2A
// entries for GC, HDP flush-register remap. Without it BAR2 doorbell writes never
// reach the engines (stage 13 run 1: MES booted but never saw its ring kick).

// On a failure in the engine stages, capture what the hardware knows before the
// user has to reboot: both hubs' VM fault registers and any pending IH entries.
// Decode the IH entries that matter for bring-up. soc21 IH client 0x0a = GFX
// (the GFXHUB's UTCL2 faults arrive under it with src 0, gmc_v12_0_process_interrupt);
// src ids per irqsrcs_gfx_11_0_0.h; fault src_data[1] flags per amdgpu_gmc.h
// (RETRY 0x80, READ 0x40, WRITE 0x20, EXE 0x10) with the VA in src_data[0] << 12.
static void log_iv_decoded(const IHEntry &e) {
    if (e.src_id == 0) {
        const uint64_t va = ((uint64_t)e.src_data[0] << 12) | (((uint64_t)e.src_data[1] & 0xFu) << 44);
        INIT_LOG("      -> VM FAULT (client %#x%s) va=%#llx vmid=%u%s%s%s%s", e.client_id,
                 e.client_id == 0x0a ? " GFX/gfxhub" : "", (unsigned long long)va, e.vmid,
                 (e.src_data[1] & 0x80u) ? " RETRY" : "", (e.src_data[1] & 0x40u) ? " READ" : "",
                 (e.src_data[1] & 0x20u) ? " WRITE" : "", (e.src_data[1] & 0x10u) ? " EXE" : "");
    } else if (e.src_id == 239) {
        INIT_LOG("      -> SQ interrupt (wave error / trap / trace) vmid=%u data0=%#010x", e.vmid, e.src_data[0]);
    } else if (e.src_id == 181) {
        INIT_LOG("      -> CP end-of-pipe interrupt (ring_id %u: me %u pipe %u queue %u)", e.ring_id,
                 (e.ring_id & 0x0c) >> 2, e.ring_id & 0x03, (e.ring_id & 0x70) >> 4);
    } else if (e.src_id == 183) {
        INIT_LOG("      -> CP BAD OPCODE in the command stream");
    } else if (e.src_id == 184) {
        INIT_LOG("      -> CP PRIVILEGED REGISTER fault in the command stream");
    } else if (e.src_id == 185) {
        INIT_LOG("      -> CP PRIVILEGED INSTRUCTION fault in the command stream");
    } else if (e.src_id == 200) {
        INIT_LOG("      -> CP unattached VM doorbell");
    }
}

static void drain_ih(BringupContext &ctx, const char *label) {
    DeviceContext &dev = *ctx.dev;
    if (!ctx.ih.inited) return;
    IHEntry e[16];
    uint32_t n = ih_poll(dev, ctx.ih, e, 16, 0);
    INIT_LOG("IH ring (%s): %u pending entries", label, n);
    for (uint32_t i = 0; i < n; i++) {
        INIT_LOG("  iv[%u]: client %u src %u ring %u vmid %u pasid %u data %08x %08x %08x %08x", i,
                 e[i].client_id, e[i].src_id, e[i].ring_id, e[i].vmid, e[i].pasid,
                 e[i].src_data[0], e[i].src_data[1], e[i].src_data[2], e[i].src_data[3]);
        log_iv_decoded(e[i]);
    }
}

static void dump_failure_state(BringupContext &ctx, const char *where) {
    DeviceContext &dev = *ctx.dev;
    INIT_LOG("--- diagnostics after %s ---", where);
    gmc_log_vm_faults(dev, ctx.gmc);
    drain_ih(ctx, where);
    INIT_LOG("--- end diagnostics ---");
}

// Before a test: show and clear whatever faults/interrupts the ladder left
// behind, so the test's own faults are unambiguous in the log.
static void pre_test_snapshot(BringupContext &ctx, const char *test) {
    DeviceContext &dev = *ctx.dev;
    INIT_LOG("--- pre-%s snapshot (faults/IVs left by earlier stages) ---", test);
    gmc_log_vm_faults(dev, ctx.gmc);
    drain_ih(ctx, test);
    gmc_clear_vm_faults(dev, ctx.gmc);
    INIT_LOG("--- end pre-%s snapshot ---", test);
}

// ===========================================================================
//  PDB0 (option B') self-test — an internal review note Q3,
//  "First testable increments" item 2.
// ===========================================================================
//
// Three SDMA CONSTANT_FILLs on VMID 0, differing ONLY in the destination
// address, plus a register readback:
//   (a) ALIAS     — a 0-based VRAM address (MC address minus 0x8000000000).
//                   This is the address shape Apple uses and the whole point
//                   of B'. It resolves through PDB0 entries [0]/[1].
//   (b) IDENTITY  — the same page by its normal MC address. The CONTROL: it
//                   proves the packet, the engine and the fence are fine, so
//                   a failure in (a) alone is unambiguous.
//   (c) GART      — a sysmem page bound through gmc_bind_existing, i.e. PDB0
//                   entry [66] descending into the flat PTB.
//
// Failure policy, per the brief: (a) failing leaves the PDB0 in place (the
// identity entries keep everything else working, and the measurement is the
// point). (b) or (c) failing means VMID-0 is worse off than before, so
// CONTEXT0 goes straight back to the flat table.
//
// Each leg uses its OWN VRAM page, so one leg cannot mask another.
static void run_pdb0_selftest(BringupContext &ctx) {
    DeviceContext &dev = *ctx.dev;
    GMCContext    &gmc = ctx.gmc;
    PDB0SelfTest  &t   = ctx.pdb0Test;

    if (!gmc.pdb0_want) return;
    if (!gmc.pdb0_active) {
        INIT_LOG("pdb0 selftest: skipped — navi48-pdb0=1 but the PDB0 was not enabled "
                 "(gmc_pdb0_build refused; see its REFUSING line)");
        return;
    }
    SDMAInstance &inst = ctx.sdma.instance[0];
    if (!inst.inited || !inst.enabled) {
        INIT_LOG("pdb0 selftest: skipped — SDMA0 is not running");
        return;
    }
    t.ran = true;
    constexpr uint32_t kBytes = 4096;
    constexpr uint32_t kDwords = kBytes / 4;

    // ---- (b) IDENTITY control, first: if the packet itself is broken we want
    //          to know before attributing anything to the alias.
    // 0.0.184: the three legs use COPY_LINEAR, the packet stage 15 has proven, instead of
    // CONSTANT_FILL. r10 (0.0.183) showed CONST_FILL "ok" from SDMA's side but 1024/1024 dwords
    // wrong on ALL legs while the copy test right before it passed under the same PDB0 - the fill
    // packet (COMPRESS bit / fill size) is unproven, so it cannot be the instrument. The source
    // page is filled through BAR0 with the pattern; only the DESTINATION address differs per leg.
    VRAMAllocation srcpg {};
    if (!gmc.vram_alloc.alloc(kBytes, kASPageSize, &srcpg)) {
        INIT_LOG("pdb0 selftest: no VRAM for the copy source page - skipping");
        return;
    }
    const uint64_t srcoff = srcpg.gpu_va - gmc.vram_start;
    auto fill_via_copy = [&](uint64_t dst, uint32_t pat) -> kern_return_t {
        bar0_memset_vram(dev, srcoff, pat, kBytes);
        amdgpu_hdp_flush(dev);
        return sdma_copy_linear_test(dev, inst, srcpg.gpu_va, dst, kBytes, 200000ull);
    };
    VRAMAllocation ident {};
    if (gmc.vram_alloc.alloc(kBytes, kASPageSize, &ident)) {
        const uint64_t off = ident.gpu_va - gmc.vram_start;
        const uint32_t pat = 0x1DE71717u;   // 'IDEnTITY'-ish
        bar0_memset_vram(dev, off, 0xDEADBEEFu, kBytes);
        amdgpu_hdp_flush(dev);
        t.identity_addr = ident.gpu_va;
        kern_return_t r = fill_via_copy(ident.gpu_va, pat);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < kDwords; i++)
            if (RBAR0_32(dev, off + (uint64_t)i * 4) != pat) bad++;
        t.identity_bad = bad;
        t.identity_ok  = (r == kIOReturnSuccess) && (bad == 0);
        INIT_LOG("pdb0 selftest (b) IDENTITY: COPY_LINEAR %#010x -> MC %#llx "
                 "(vram+%#llx) %s — kr=%#x, %u/%u dwords wrong",
                 pat, (unsigned long long)ident.gpu_va, (unsigned long long)off,
                 t.identity_ok ? "PASSED" : "FAILED", r, bad, kDwords);
        gmc.vram_alloc.free(ident);
    } else {
        INIT_LOG("pdb0 selftest (b) IDENTITY: VRAM alloc failed");
    }

    // ---- (a) ALIAS: the same fill, addressed 0-based.
    VRAMAllocation alias {};
    if (gmc.vram_alloc.alloc(kBytes, kASPageSize, &alias)) {
        const uint64_t off       = alias.gpu_va - gmc.vram_start;  // 0-based address
        const uint32_t pat       = 0x0BA5ED00u;                    // '0-BASED'
        bar0_memset_vram(dev, off, 0xDEADBEEFu, kBytes);
        amdgpu_hdp_flush(dev);
        t.alias_addr = off;
        kern_return_t r = fill_via_copy(off, pat);
        uint32_t bad = 0;
        for (uint32_t i = 0; i < kDwords; i++)
            if (RBAR0_32(dev, off + (uint64_t)i * 4) != pat) bad++;
        t.alias_bad = bad;
        t.alias_ok  = (r == kIOReturnSuccess) && (bad == 0);
        INIT_LOG("pdb0 selftest (a) ALIAS: COPY_LINEAR %#010x -> 0-BASED %#llx "
                 "(= MC %#llx - %#llx) %s — kr=%#x, %u/%u dwords wrong",
                 pat, (unsigned long long)off, (unsigned long long)alias.gpu_va,
                 (unsigned long long)gmc.vram_start,
                 t.alias_ok ? "PASSED — Apple's 0-based VRAM addresses now land"
                            : "FAILED", r, bad, kDwords);
        gmc.vram_alloc.free(alias);
    } else {
        INIT_LOG("pdb0 selftest (a) ALIAS: VRAM alloc failed");
    }

    // ---- (c) GART round-trip through PDE[66].
    {
        SysMem page {};
        kern_return_t ar = sysmem_alloc(page, kAMDGPUGPUPageSize, kAMDGPUGPUPageSize);
        uint64_t mc = 0;
        if (ar == kIOReturnSuccess)
            ar = gmc_bind_existing(dev, gmc, page.bus, page.size, &mc);
        if (ar == kIOReturnSuccess && mc != 0) {
            const uint32_t pat = 0x6A27BABEu;   // 'GART'-ish
            volatile uint32_t *cpu = reinterpret_cast<volatile uint32_t *>(page.cpu);
            for (uint32_t i = 0; i < kDwords; i++) cpu[i] = 0xDEADBEEFu;
            sysmem_wmb();
            t.gart_addr = mc;
            kern_return_t r = fill_via_copy(mc, pat);
            sysmem_rmb();
            uint32_t bad = 0;
            for (uint32_t i = 0; i < kDwords; i++) if (cpu[i] != pat) bad++;
            t.gart_bad = bad;
            t.gart_ok  = (r == kIOReturnSuccess) && (bad == 0);
            INIT_LOG("pdb0 selftest (c) GART: COPY_LINEAR %#010x -> GART MC %#llx "
                     "(sysmem phys %#llx) %s — kr=%#x, %u/%u dwords wrong",
                     pat, (unsigned long long)mc, (unsigned long long)page.bus,
                     t.gart_ok ? "PASSED — PDE[66] descends into the flat PTB"
                               : "FAILED", r, bad, kDwords);
        } else {
            INIT_LOG("pdb0 selftest (c) GART: sysmem alloc / bind failed %#x", ar);
        }
        // The page stays bound: the GART bump allocator has no free, and
        // unbinding would leave a live PTE pointing at reclaimed host memory.
    }

    // ---- (d) register readback.
    gmc_pdb0_read_regs(dev, gmc, t.regs);

    // ---- failure policy.
    if (!t.identity_ok || !t.gart_ok) {
        INIT_LOG("pdb0 selftest: the %s leg FAILED — VMID-0 is worse off than the "
                 "flat table, reverting CONTEXT0 now so the rest of the ladder runs",
                 !t.identity_ok ? "identity" : "gart");
        (void)gmc_pdb0_revert(dev, gmc);
        t.reverted = true;
        gmc_pdb0_read_regs(dev, gmc, t.regs);
    } else if (!t.alias_ok) {
        INIT_LOG("pdb0 selftest: the alias leg FAILED but identity and gart are "
                 "good — leaving the PDB0 ENABLED (it is no worse than the flat "
                 "table) so the fault registers can be read");
    } else {
        INIT_LOG("pdb0 selftest: *** ALL THREE LEGS PASSED — GFXHUB VMID-0 now "
                 "resolves 0-based VRAM addresses, our MC window and the GART ***");
    }
    gmc_log_vm_faults(dev, gmc);
}

static kern_return_t ensure_doorbell_path(BringupContext &ctx) {
    if (ctx.doorbellPathReady) return kIOReturnSuccess;
    DeviceContext &dev = *ctx.dev;
    if (!dev.bar2 || dev.bar2Phys == 0) { INIT_LOG("doorbell path: BAR2 not mapped — skipping NBIO doorbell init"); return kIOReturnNotReady; }
    kern_return_t r = nbif_v6_3_1_doorbell_path_init(dev, dev.bar2Phys);
    if (r != kIOReturnSuccess) { INIT_LOG("doorbell path: NBIO init failed %#x", r); return r; }
    ctx.doorbellPathReady = true;
    return kIOReturnSuccess;
}

static kern_return_t ensure_gfxhub_gart(BringupContext &ctx) {
    if (ctx.gfxhubReady) return kIOReturnSuccess;
    DeviceContext &dev = *ctx.dev;
    if (!dev.ip.isResolved(IPBlock::GC)) { INIT_LOG("gfxhub: GC base unresolved"); return kIOReturnNotReady; }
    kern_return_t r = gmc_gfxhub_gart_enable(dev, ctx.gmc);
    if (r != kIOReturnSuccess) { INIT_LOG("gfxhub: gart_enable failed %#x", r); return r; }
    gmc_hdp_flush(dev);
    gmc_flush_gpu_tlb(dev, ctx.gmc, ctx.gmc.gfxhub, /*vmid*/ 0, /*type*/ 0);
    r = gart_init(dev, ctx.gmc, ctx.gart);
    if (r != kIOReturnSuccess) INIT_LOG("gfxhub: gart_init failed %#x — GTT buffers unavailable", r);
    ctx.gfxhubReady = true;
    INIT_LOG("gfxhub: GART live on the GC hub (pt=0x%llx)", (unsigned long long)ctx.gmc.gart_pt_bus);
    return kIOReturnSuccess;
}

kern_return_t run_stage(BringupContext &ctx, BringupStage s) {
    DeviceContext &dev = *ctx.dev;
    INIT_LOG("running stage %u (%s)", (unsigned)s, stage_name(s));
    switch (s) {
    case BringupStage::None:
    case BringupStage::IPDiscovery:
        return dev.ip.isResolved(IPBlock::MP0) && dev.ip.isResolved(IPBlock::GC) ? kIOReturnSuccess : kIOReturnNotReady;

    case BringupStage::IHInit: {
        // With an MSI handler armed, bring the ring up the way upstream does:
        // IH_RB_CNTL.ENABLE_INTR = 1 and RPTR_REARM = 1. Without one this stays
        // false and the ring is drained by polling (ih_v7_0.cpp deviation 2).
        ctx.ih.enable_cpu_intr = ctx.wantInterrupts;
        kern_return_t r = ensure_doorbell_path(ctx);     // Linux: common hw_init, before every IP
        if (r != kIOReturnSuccess) return r;
        return ih_init_full(dev, ctx.ih);
    }

    case BringupStage::GMCInit:
        return gmc_init(dev, ctx.gmc);

    // ---- adopt path: this boot already provisioned the GPU ----
    // NOTE (0.0.21): Navi48Bringup::runStages now refuses to re-run the ladder
    // at all when the marker is present, because skipping only the PSP stages
    // was not enough — see the comment there. This block is kept because it is
    // correct and cheap, and a future design that can reset the GPU (once the
    // display no longer lives on it) will want it. It is currently unreachable.
    // Stages 4..8 are the PSP's: allocate its buffers, adopt the SOS, create
    // the GPCOM ring, set up the TMR, push every firmware image into it. All
    // of that survives our unload, and re-doing the last step wedges: the PSP
    // accepted LOAD_IP_FW for the SMU and never fenced it, first
    // reload test). The RS64 instruction/data cache bases are still programmed
    // from the first load too — cp_config_rs64_caches sees them nonzero and
    // leaves them alone — so nothing downstream needs the TMR addresses.
    case BringupStage::PSPInit:
    case BringupStage::PSPLoadSOS:
    case BringupStage::PSPRingCreate:
    case BringupStage::TMRSetup:
    case BringupStage::PSPFwLoad:
        if (ctx.adoptProvisioned) {
            // Tell the engines their microcode is already in place, or they
            // will each try to PSP-load it again at stages 13 and 15.
            ctx.mes.sched_ucode_loaded = true;
            ctx.sdma.microcode_loaded  = true;
            INIT_LOG("%s: skipped — the GPU was provisioned by an earlier load this boot "
                     "(firmware is in the PSP TMR, RS64 cache bases are programmed)",
                     stage_name(s));
            return kIOReturnSuccess;
        }
        break;
    default:
        break;
    }

    // Stages None..GMCInit returned from the switch above; this one owns
    // PSPInit..ComputeDispatch.
    switch (s) {
    case BringupStage::None:
    case BringupStage::IPDiscovery:
    case BringupStage::IHInit:
    case BringupStage::GMCInit:
        return kIOReturnSuccess;   // unreachable: handled above

    case BringupStage::PSPInit: {
        kern_return_t r = psp_init(dev, ctx.psp);
        if (r != kIOReturnSuccess) return r;
        (void)psp_read_runtime_db(dev, ctx.psp, dev.vramSizeBytes);
        return kIOReturnSuccess;
    }

    case BringupStage::PSPLoadSOS: {
        // Our bootloader chain (src/psp.cpp) already booted SOS before the ladder
        // runs; still parse the container so the TOC/RL sub-bins are known
        // (psp_setup_tmr's LOAD_TOC and psp_rl_load need them).
        const FwBlob *sos = fw_get(FwId::PSP_SOS);
        if (!sos || !sos->data) { INIT_LOG("PSPLoadSOS: embedded SOS container missing"); return kIOReturnNotFound; }
        kern_return_t r = psp_parse_sos_microcode(ctx.psp, sos->data, sos->size);
        if (r != kIOReturnSuccess) { INIT_LOG("PSPLoadSOS: SOS container parse failed %#x", r); return r; }
        return psp_adopt_sos_alive(dev, ctx.psp);
    }

    case BringupStage::PSPRingCreate: {
        kern_return_t r = psp_ring_create(dev, ctx.psp);
        if (r != kIOReturnSuccess) return r;
        return psp_query_fw_reservation(dev, ctx.psp);
    }

    case BringupStage::TMRSetup: {
        kern_return_t r = psp_setup_tmr(dev, ctx.psp);
        if (r != kIOReturnSuccess) return r;
        // Linux psp_hw_start order: tmr_load -> rl_load -> asd_initialize.
        r = psp_rl_load(dev, ctx.psp);
        if (r != kIOReturnSuccess) { INIT_LOG("TMRSetup: psp_rl_load (REG_LIST) failed %#x", r); return r; }
        kern_return_t ar = psp_asd_initialize(dev, ctx.psp);
        if (ar != kIOReturnSuccess) INIT_LOG("TMRSetup: psp_asd_initialize %#x (non-fatal; TA not ported)", ar);
        return kIOReturnSuccess;
    }

    case BringupStage::PSPFwLoad: {
        if (!ctx.psp.sosAlive) { INIT_LOG("PSPFwLoad: SOS not alive"); return kIOReturnNotReady; }
        kern_return_t r = fw_loader_prepare(dev, ctx.psp, ctx.fw);
        if (r != kIOReturnSuccess) { INIT_LOG("PSPFwLoad: no embedded firmware payloads (%#x)", r); return r; }
        FirmwareLoader loader = fw_loader_make(ctx.fw);
        r = psp_load_non_psp_fw(dev, ctx.psp, loader);
        if (r != kIOReturnSuccess) return r;
        ctx.imu.microcode_loaded = ctx.fw.imuPresent;    // the reference's host set these after LoadFirmware(...)
        ctx.rlc.microcode_loaded = ctx.fw.rlcPresent;
        ctx.sdma.microcode_loaded = ctx.fw.sdmaPresent;   // TH0 already loaded by psp_load_non_psp_fw; ucode_bin stays null
        INIT_LOG("PSPFwLoad: all embedded firmware loaded through the PSP");
        return kIOReturnSuccess;
    }

    case BringupStage::SMUInit: {
        uint32_t echo = 0;
        kern_return_t r = smu_test_message(dev, &echo);
        if (r != kIOReturnSuccess) { INIT_LOG("SMUInit: TestMessage failed %#x", r); return r; }
        (void)smu_get_version(dev, &ctx.smuVersion);
        dev.smuOnline = true; ctx.smuOnline = true;
        // smu_hw_init(dev, true) runs the reference's smc_hw_setup (driver table,
        // DcBtc, power source, feature enable) — it changes clocks/fans while the
        // firmware console is still scanning out, so it is opt-in via navi48-smu-full=1.
        kern_return_t hwr = smu_hw_init(dev, ctx.smuFullSetup);
        if (hwr != kIOReturnSuccess)
            INIT_LOG("SMUInit: smu_hw_init(full=%d) non-fatal failure %#x — proceeding", ctx.smuFullSetup, hwr);
        return kIOReturnSuccess;
    }

    case BringupStage::IMUInit:
        return imu_init_full(dev, ctx.imu);

    case BringupStage::RLCInit:
        return rlc_init_full(dev, ctx.gmc, ctx.rlc);

    case BringupStage::CPInit: {
        kern_return_t r = doorbell_init(dev, dev.doorbell);
        if (r != kIOReturnSuccess) return r;
        r = ensure_gfxhub_gart(ctx);
        if (r != kIOReturnSuccess) return r;
        // RS64 instruction/data cache bases: the PSP told us where it placed each image in the
        // TMR (LOAD_IP_FW responses); in the PSP-load flow nothing else programs these.
        {
            const uint64_t *t = ctx.psp.tmr_fw_addr_by_type;
            ctx.cp.tmr_pfp_ic  = t[PSPGfxFwType::RS64_PFP];
            ctx.cp.tmr_pfp_dc  = t[PSPGfxFwType::RS64_PFP_P0];
            ctx.cp.tmr_me_ic   = t[PSPGfxFwType::RS64_ME];
            ctx.cp.tmr_me_dc   = t[PSPGfxFwType::RS64_ME_P0];
            ctx.cp.tmr_mec_ic  = t[PSPGfxFwType::RS64_MEC];
            ctx.cp.tmr_mec_dc0 = t[PSPGfxFwType::RS64_MEC_P0];
            ctx.cp.tmr_mec_dc1 = t[PSPGfxFwType::RS64_MEC_P1];
            INIT_LOG("CPInit: RS64 TMR addresses from PSP — PFP ic=%#llx dc=%#llx  ME ic=%#llx dc=%#llx  MEC ic=%#llx dc0=%#llx dc1=%#llx",
                     (unsigned long long)ctx.cp.tmr_pfp_ic, (unsigned long long)ctx.cp.tmr_pfp_dc,
                     (unsigned long long)ctx.cp.tmr_me_ic, (unsigned long long)ctx.cp.tmr_me_dc,
                     (unsigned long long)ctx.cp.tmr_mec_ic, (unsigned long long)ctx.cp.tmr_mec_dc0, (unsigned long long)ctx.cp.tmr_mec_dc1);
        }
        if (ctx.wantInterrupts) cp_set_eop_interrupt(dev, true);
        return cp_init_full(dev, ctx.gmc, ctx.cp, ctx.cpLegacyRb);
    }

    case BringupStage::MESInit: {
        kern_return_t r = ensure_gfxhub_gart(ctx);
        if (r != kIOReturnSuccess) return r;
        // uni_mes firmware bytes default to fw_get(FwId::GC_UNI_MES); LOAD_IP_FW of
        // CP_MES/CP_MES_DATA already happened in PSPFwLoad (reference order).
        r = mes_init_full(dev, ctx.psp, ctx.gmc, ctx.mes);
        if (r != kIOReturnSuccess) {
            dump_failure_state(ctx, "MESInit");
            if (ctx.mesNonFatal) {
                ctx.mesFailed = true;
                INIT_LOG("MESInit: failed %#x but navi48-mes-nonfatal=1 — continuing to GFX/SDMA for diagnostics", r);
                return kIOReturnSuccess;
            }
        }
        return r;
    }

    case BringupStage::GFXInit: {
        kern_return_t r = ensure_gfxhub_gart(ctx);
        if (r != kIOReturnSuccess) return r;
        return gfx_constants_init(dev, ctx.gfx);
    }

    case BringupStage::SDMAInit: {
        kern_return_t r = ensure_gfxhub_gart(ctx);
        if (r != kIOReturnSuccess) return r;
        // NBIO S2A doorbell range for SDMA0 (Linux sdma_v7_0_gfx_resume via nbio->sdma_doorbell_range):
        // index = doorbell_index.sdma_engine[0] << 1 (64-bit doorbells), range size 20.
        r = nbif_v6_3_1_sdma_doorbell_range(dev, 0, true, (int)(dev.doorbell.index.sdma_engine[0] << 1), 20);
        if (r != kIOReturnSuccess) INIT_LOG("SDMAInit: sdma_doorbell_range failed %#x (SDMA also kicks RB_WPTR via MMIO)", r);
        r = sdma_init_full(dev, ctx.psp, ctx.gmc, ctx.sdma);
        if (r != kIOReturnSuccess) return r;
        // 0.0.657 (GitHub issue #1): the SDMA0_DCC_CNTL no-PTE compression clear (0xaabe -> 0xaaaa) lands BEFORE any SDMA copy,
        // including the copy test and the sweep below: with compression still ON they read back inconsistently from run to run.
        // The same single write + readback + log line the default has always made; only its position moved. `navi48-sdmadcc=0`
        // skips it; the later runStages call is then a logged no-op.
        navi48_sdmadcc_default();
        // Phase 3 sub-milestone: the GPU copies a known pattern between two VRAM
        // buffers and we read the result back through the MM window.
        r = sdma_vram_copy_test(dev, ctx.gmc, ctx.sdma.instance[0], 4096, &ctx.sdmaCopy);
        ctx.sdmaCopyPassed = (r == kIOReturnSuccess && ctx.sdmaCopy.mismatched == 0);
        INIT_LOG("SDMAInit: VRAM copy test %s (kr=%#x, %u bytes, %u mismatched, %llu us)",
                 ctx.sdmaCopyPassed ? "PASSED — the GPU wrote VRAM on command" : "FAILED", r,
                 ctx.sdmaCopy.bytes, ctx.sdmaCopy.mismatched, (unsigned long long)ctx.sdmaCopy.elapsed_us);
        // 0.0.339: the size / address / CPV sweep. The 4 KiB test
        // just above passes in every boot in which the scanout pre-flight's
        // 64 KiB copy mis-delivers 7904 of 16384 dwords, so the discriminator
        // is one of those three axes. Runs AFTER the control so the control's
        // log line and its VRAM addresses are unchanged, and frees everything
        // it takes, so no later stage's VRAM address moves.
        // 0.0.341 — THE CONTROL, and the only reason this build
        // exists. The scanout pre-flight failed in twenty-five consecutive runs
        // on 0.0.335-0.0.338 and PASSED in both runs of 0.0.339 and 0.0.340,
        // 0 of 16384 twice, with the sweep as the only new thing in the boot.
        // Either the sweep's own VRAM traffic is what clears the fault, or the
        // pass was a coin toss on a machine whose clean/broken history already
        // flipped once with no known cause. One boot without the sweep answers
        // it. Nothing else differs from 0.0.340: the function is still compiled
        // in, still exported, and simply not called.
        // 0.0.342: RE-ENABLED, and now it is a FIX rather than an
        // instrument. The 0.0.341 control boot answered it: with the sweep the
        // scanout pre-flight reads 0 of 16384 (runs sweep1, sweep2); with the
        // identical binary and only this flag flipped it reads 7904 of 16384,
        // the historical number, and `scanout-1` returns status 9 (run sweep3).
        // The sweep's own VRAM traffic is what leaves the cache lines around
        // the pre-flight's buffers in a state where its copy is read back
        // correctly. It is a workaround, not the right fix — the right fix is a
        // GPU-side cache operation around the copy — so it stays until that
        // lands, and the eight cases it still fails are the evidence for it.
        constexpr bool kSweepEnabled = true;
        if (kSweepEnabled) {
            const kern_return_t sw = sdma_vram_copy_sweep(dev, ctx.gmc, ctx.sdma.instance[0]);
            INIT_LOG("SDMAInit: VRAM copy SWEEP %s (kr=%#x) — see the `copy_sweep` lines",
                     sw == kIOReturnSuccess ? "every case matched" : "AT LEAST ONE CASE WRONG", sw);
        } else {
            INIT_LOG("SDMAInit: VRAM copy SWEEP DELIBERATELY NOT RUN — 0.0.341 is the "
                     "CONTROL BOOT for an earlier analysis: does the scanout pre-flight "
                     "still pass when no sweep has touched VRAM first?");
        }
        // Option B' self-test (navi48-pdb0=1 only; a complete no-op otherwise).
        // Here rather than in stage 17 because the PDB0 went live back at
        // CPInit: measuring now is the last chance to revert before PM4Test
        // and ComputeDispatch run on top of it.
        run_pdb0_selftest(ctx);
        // 0.0.185: the SDMA0 QUEUE1 doorbell-routing self-test. Opt-in
        // (navi48-sdma-q1-test=1) and a complete no-op otherwise. It programs,
        // fences and then DISABLES QUEUE1, so a plain boot is unchanged and a
        // test boot ends with QUEUE1 exactly as inert as it started.
        if (ctx.sdmaQ1TestWant) {
            (void)sdma_q1_selftest(dev, ctx.gmc, ctx.sdma.instance[0],
                                   kSDMAQ1AppleDoorbellIndex, &ctx.sdmaQ1Test);
        }
        // 0.0.196: the same test, once per hardware queue `sdmamap` may hand
        // Apple. Each leg programs, fences, reads back and then DISABLES its
        // queue and frees its scratch ring, so a test boot still ends with every
        // queue but QUEUE0 inert. A slot whose instance is absent or not inited
        // is skipped and left with ran=false, which sdmamap reads as UNPROVEN.
        if (ctx.sdmaQnTestWant) {
            for (uint32_t s = 0; s < kSDMAExtSlotCount; s++) {
                const SDMAExtQueueSlot &sl = kSDMAExtSlots[s];
                if (sl.instance >= kSDMAInstanceCount ||
                    !ctx.sdma.instance_present[sl.instance]) {
                    INIT_LOG("SDMAInit: qn-test slot %u (SDMA%u QUEUE%u db %#x) "
                             "skipped — instance not present", s, sl.instance,
                             sl.queue, sl.doorbell_dword);
                    continue;
                }
                (void)sdma_q1_selftest(dev, ctx.gmc, ctx.sdma.instance[sl.instance],
                                       sl.doorbell_dword, &ctx.sdmaQnTest[s],
                                       sl.queue);
            }
        }
        // 0.0.191: the SRBM_WRITE self-test (navi48-srbm-test=1). LAST, so a
        // leg that times out cannot cost the tests above their queue. Both
        // registers it touches are restored by MMIO whichever way it goes, and
        // with the boot-arg absent not one register is read or written.
        if (ctx.srbmTestWant) {
            (void)sdma_srbm_selftest(dev, ctx.gmc, ctx.sdma.instance[0],
                                     &ctx.srbmTest);
        }
        // 0.0.193: the vmfrag self-test (navi48-vmfrag-test=1) — INCREMENT 1 of
        // the Apple page-table encoding fix. After the SRBM one, i.e. last of
        // all, for the same reason. It borrows GFXHUB CONTEXT1 (VMID 1, which
        // no queue of ours and no later stage uses), saves and restores its six
        // registers, and never reads or writes Apple's arena. A total no-op
        // without the boot-arg.
        if (ctx.vmfragTestWant) {
            (void)gmc_vmfrag_selftest(dev, ctx.gmc, ctx.sdma.instance[0],
                                      &ctx.vmfragTest, ctx.vmfragNoValidWant);
        }
        return kIOReturnSuccess;   // engine init succeeded; the copy result is reported separately
    }

    case BringupStage::PM4Test: {
        kern_return_t r = kIOReturnSuccess;
        pre_test_snapshot(ctx, "PM4Test");
        // (0) Bring the kernel GFX queue up the way the GFX12 driver does on real hardware
        //     (gfx_v12_0_cp_async_gfx_ring_resume): fill a v12_gfx_mqd, have the MES map it
        //     (ADD_QUEUE map_legacy_kq=1 on the uni_mes SCHED pipe), then CP_MAX_CONTEXT /
        //     CP_DEVICE_ID. The CP_RB0_* register path (navi48-cp-legacy-rb=1) made the RS64
        //     firmware halt itself on the first doorbell in stage 16 runs 1-3.
        if (!ctx.cpLegacyRb) {
            if (ctx.mesFailed || !ctx.mes.pipe[0].inited || !ctx.mes.pipe[0].enabled) {
                INIT_LOG("PM4Test: the MES scheduler pipe is not running — on GFX12 only the MES can map the kernel GFX queue");
                return kIOReturnNotReady;
            }
            r = cp_gfx_mqd_init(dev, ctx.cp);
            if (r != kIOReturnSuccess) { INIT_LOG("PM4Test: cp_gfx_mqd_init failed %#x", r); return r; }
            r = cp_map_gfx_kgq_mes(dev, ctx.cp, ctx.mes);
            ctx.kgqMapped = (r == kIOReturnSuccess);
            INIT_LOG("PM4Test: MES ADD_QUEUE(map_legacy_kq) for the GFX ring %s (kr=%#x)",
                     ctx.kgqMapped ? "ACKED — the scheduler mapped our queue" : "FAILED", r);
            if (r != kIOReturnSuccess) { dump_failure_state(ctx, "PM4Test/map_kgq"); return r; }
            r = cp_gfx_start(dev, ctx.cp, /*legacy_unhalt=*/false);
            if (r != kIOReturnSuccess) return r;
            // Diagnostic (non-fatal by itself): 16 NOPs + doorbell, does rptr move?
            const kern_return_t pr = cp_ring_fetch_probe(dev, ctx.cp, 16, 500000);
            INIT_LOG("PM4Test: ring fetch probe %s (kr=%#x)",
                     pr == kIOReturnSuccess ? "PASSED — the CP consumes the GFX ring" : "did not see the CP fetch", pr);
        }

        // (a0) gfx_v12_0_ring_test_ring: SET_UCONFIG_REG SCRATCH_REG0 <- 0xDEADBEEF, poll the
        //      register. No memory write by the CP involved — pure "does it execute PM4".
        r = cp_ring_test_scratch(dev, ctx.cp, 1000000);
        ctx.cpRingTestPassed = (r == kIOReturnSuccess);
        INIT_LOG("PM4Test: ring test (SET_UCONFIG_REG SCRATCH_REG0) %s (kr=%#x SCRATCH_REG0=%#010x)",
                 ctx.cpRingTestPassed ? "PASSED — the command processor executes our PM4" : "FAILED", r,
                 ctx.cp.ring_test_value);
        if (r != kIOReturnSuccess) { dump_failure_state(ctx, "PM4Test/ring_test"); return r; }

        // The command processor itself, through the GFX ring (GART sysmem):
        // (a) NOP + RELEASE_MEM end-of-pipe fence written back to sysmem.
        uint32_t fence = 0;
        r = cp_submit_eop_test(dev, ctx.cp, 2000000, &fence);
        ctx.cpEopFence = fence;
        ctx.cpEopPassed = (r == kIOReturnSuccess);
        INIT_LOG("PM4Test: GFX ring EOP fence %s (kr=%#x fence=%u)", ctx.cpEopPassed ? "LANDED" : "did not land", r, fence);
        if (r != kIOReturnSuccess) { dump_failure_state(ctx, "PM4Test/eop"); return r; }

        // (b) WRITE_DATA: the CP writes a magic dword into a fresh VRAM page; we
        //     verify through the MM_INDEX window (the GPU's own view of VRAM).
        VRAMAllocation scratch {};
        if (!ctx.gmc.vram_alloc.alloc(4096, 4096, &scratch)) { INIT_LOG("PM4Test: VRAM alloc failed"); return kIOReturnNoMemory; }
        const uint64_t off = scratch.gpu_va - ctx.gmc.vram_start;
        bar0_memset_vram(dev, off, 0xDEADBEEFu, 64);
        const uint32_t magic = 0x4E34384Fu;   // 'N48O'
        uint32_t pkt[5];
        pkt[0] = pm4_header(kPM4OpWriteData, 3);   // 4 payload dwords - 1
        pkt[1] = pm4_write_data_control(kPM4WriteDataEngineME, kPM4WriteDataDstSelMemory, true);
        pkt[2] = (uint32_t)(scratch.gpu_va & 0xFFFFFFFFu);
        pkt[3] = (uint32_t)(scratch.gpu_va >> 32);
        pkt[4] = magic;
        if (cp_ring_write(ctx.cp, pkt, 5) != 5) { INIT_LOG("PM4Test: ring full"); return kIOReturnNoResources; }
        const uint32_t fence2 = cp_emit_eop_fence(ctx.cp);
        r = cp_kick_doorbell(dev, ctx.cp);
        if (r != kIOReturnSuccess) return r;
        uint64_t elapsed = 0; uint64_t observed = 0;
        while (elapsed < 2000000) {
            sysmem_rmb();
            observed = *ctx.cp.fence_cpu;
            if (observed == fence2) break;
            IOSleep(1); elapsed += 1000;
        }
        const uint32_t before = 0xDEADBEEFu;
        const uint32_t got = RVRAM32_via_mm(dev, off);
        ctx.cpWriteDataPassed = (observed == fence2) && (got == magic);
        INIT_LOG("PM4Test: WRITE_DATA -> vram+%#llx (mc %#llx): fence %llu/%u after %llu us, MM readback %#010x (poison %#010x, want %#010x) => %s",
                 (unsigned long long)off, (unsigned long long)scratch.gpu_va, (unsigned long long)observed, fence2,
                 (unsigned long long)elapsed, got, before, magic,
                 ctx.cpWriteDataPassed ? "PASSED — the command processor executed our PM4 packet" : "FAILED");
        if (!ctx.cpWriteDataPassed) { dump_failure_state(ctx, "PM4Test/write_data"); return kIOReturnIOError; }

        // (c) The kernel COMPUTE queue. Same creation story as the GFX queue —
        //     v12_compute_mqd + MES ADD_QUEUE(map_legacy_kq) — but on a MEC
        //     pipe, which is where compute work belongs and the shape every
        //     later user queue takes (gfx_v12_0_kcq_resume).
        // On by default as of 0.0.29 (navi48-kcq=0 disables). Getting here took
        // three distinct bugs, all of which presented as "the MEC ignores us":
        //   1. compute_hqd_mask handed MES all 8 queues per pipe, including the
        //      one we then asked it to map a legacy queue into (fixed to 0xFC,
        //      matching amdgpu_mes_get_hqd_mask);
        //   2. the compute INDIRECT_BUFFER control dword lacked
        //      INDIRECT_BUFFER_VALID, which the GFX ring does not use;
        //   3. RELEASE_MEM was one dword shorter than its own header declared,
        //      so the engine stalled waiting for the rest of the packet.
        // Only the last one actually froze the queue; the first two are real
        // bugs that would have bitten later.
        if (ctx.wantKcq && !ctx.cpLegacyRb && ctx.mes.pipe[0].enabled) {
            r = cp_compute_queue_alloc(dev, ctx.gmc, ctx.kcq, /*pipe=*/0, /*queue=*/0);
            if (r != kIOReturnSuccess) { INIT_LOG("PM4Test: compute queue alloc failed %#x", r); return kIOReturnSuccess; }
            r = cp_compute_mqd_init(dev, ctx.kcq);
            if (r != kIOReturnSuccess) { INIT_LOG("PM4Test: compute MQD init failed %#x", r); return kIOReturnSuccess; }
            cp_dump_compute_hqd(dev, ctx.kcq, "before MES ADD_QUEUE(COMPUTE)");
            r = cp_compute_queue_map_mes(dev, ctx.kcq, ctx.mes);
            ctx.kcqMapped = (r == kIOReturnSuccess);
            INIT_LOG("PM4Test: MES ADD_QUEUE(COMPUTE) %s (kr=%#x)",
                     ctx.kcqMapped ? "ACKED" : "FAILED", r);
            if (ctx.kcqMapped) {
                cp_dump_compute_hqd(dev, ctx.kcq, "after MES ADD_QUEUE(COMPUTE)");
                // One WRITE_DATA through an IB on the compute ring — the same
                // proof stage 16 ran for the GFX ring, on the other engine.
                VRAMAllocation ib {}, tgt {};
                if (ctx.gmc.vram_alloc.alloc(4096, 4096, &ib) &&
                    ctx.gmc.vram_alloc.alloc(4096, 4096, &tgt)) {
                    const uint64_t ib_off  = ib.gpu_va  - ctx.gmc.vram_start;
                    const uint64_t tgt_off = tgt.gpu_va - ctx.gmc.vram_start;
                    const uint32_t magic = 0x4B435121u;   // 'KCQ!'
                    bar0_memset_vram(dev, tgt_off, 0xDEADBEEFu, 64);
                    uint32_t dw[5];
                    dw[0] = pm4_header(kPM4OpWriteData, 3);
                    dw[1] = pm4_write_data_control(kPM4WriteDataEngineME, kPM4WriteDataDstSelMemory, true);
                    dw[2] = (uint32_t)(tgt.gpu_va & 0xFFFFFFFFu);
                    dw[3] = (uint32_t)(tgt.gpu_va >> 32);
                    dw[4] = magic;
                    bar0_memcpy_to_vram(dev, ib_off, dw, sizeof(dw));
                    (void)gmc_hdp_flush(dev);
                    uint32_t f = 0;
                    kern_return_t sr = cp_compute_submit_ib(dev, ctx.kcq, ib.gpu_va, 5, 0, &f);
                    uint64_t obs = 0, el = 0;
                    if (sr == kIOReturnSuccess)
                        sr = cp_compute_wait_fence(ctx.kcq, f, 2000000, &obs, &el);
                    const uint32_t got = RVRAM32_via_mm(dev, tgt_off);
                    ctx.kcqTestPassed = (sr == kIOReturnSuccess) && (got == magic);
                    INIT_LOG("PM4Test: compute-ring WRITE_DATA %s — kr=%#x fence %llu/%u after %llu us, "
                             "readback %#010x (want %#010x)",
                             ctx.kcqTestPassed ? "PASSED — the MEC executed our packet" : "FAILED",
                             sr, (unsigned long long)obs, f, (unsigned long long)el, got, magic);
                    if (!ctx.kcqTestPassed) {
                        cp_dump_compute_hqd(dev, ctx.kcq, "compute-ring test failure");
                        // Do not leave a queue mapped that the MEC is not
                        // draining: unmap it before anything else runs.
                        kern_return_t ur = mes_remove_hw_queue(dev, ctx.mes, kMESQueueType_COMPUTE,
                                                               ctx.kcq.pipe, ctx.kcq.queue,
                                                               ctx.kcq.doorbell_index);
                        ctx.kcqMapped = false;
                        INIT_LOG("PM4Test: compute queue unmapped after the failure (%#x)", ur);
                    }
                    ctx.gmc.vram_alloc.free(tgt);
                    ctx.gmc.vram_alloc.free(ib);
                }
            } else {
                cp_dump_compute_hqd(dev, ctx.kcq, "after FAILED MES ADD_QUEUE(COMPUTE)");
            }
        }
        return kIOReturnSuccess;
    }

    case BringupStage::ComputeDispatch: {
        pre_test_snapshot(ctx, "ComputeDispatch");
        kern_return_t r = compute_dispatch_test(dev, ctx.gmc, ctx.cp, &ctx.computeResult, ctx.hsaAbi);
        const ComputeTestResult &cr = ctx.computeResult;
        ctx.computePassed = (r == kIOReturnSuccess) && cr.lanes_mismatched == 0;
        INIT_LOG("ComputeDispatch: %s — kr=%#x IB test %s, fence %s (%llu us), lanes %u/%u correct, observed %08x %08x %08x %08x expected %08x %08x %08x %08x",
                 ctx.computePassed ? "PASSED — a shader ran on the RX 9070 XT and wrote VRAM" : "FAILED", r,
                 cr.ib_test_passed ? "passed" : "FAILED",
                 cr.fence_landed ? "landed" : "did not land", (unsigned long long)cr.elapsed_us,
                 cr.lanes_checked - cr.lanes_mismatched, cr.lanes_checked,
                 cr.observed[0], cr.observed[1], cr.observed[2], cr.observed[3],
                 cr.expected[0], cr.expected[1], cr.expected[2], cr.expected[3]);
        if (!ctx.computePassed) { dump_failure_state(ctx, "ComputeDispatch"); return r != kIOReturnSuccess ? r : kIOReturnIOError; }
        return kIOReturnSuccess;
    }
    }
    return kIOReturnUnsupported;
}

// ===========================================================================
//  bringup_teardown — unwind the ladder
// ===========================================================================
//
// Order is the reverse of bring-up, and the first three steps matter most:
// a mapped MES queue and a running CP both hold pointers into GART sysmem
// that IOFree is about to reclaim, so they are stopped before anything is
// released. Each step is independently guarded, so tearing down from a
// half-finished ladder (a stage failed) is fine.
//
// Not done here, on purpose:
//   * no GPU reset — the UEFI console is still scanning out of VRAM and a
//     mode-1 reset would take the display with it;
//   * the PSP is left alive (its TMR holds the firmware the next start()
//     would otherwise have to re-load);
//   * SMU features are left enabled — smu_disable_all_features() would drop
//     the GFX core's power, and the display side shares that rail.
void bringup_teardown(BringupContext &ctx) {
    if (ctx.dev == nullptr || ctx.tornDown) return;
    DeviceContext &dev = *ctx.dev;
    ctx.tornDown = true;
    INIT_LOG("teardown: unwinding from stage %u (%s)",
             (unsigned)ctx.reached, stage_name(ctx.reached));

    // (1) Unmap the kernel GFX queue from the scheduler while both are still
    //     alive (amdgpu_mes_unmap_legacy_queue before the ring goes away).
    if (ctx.kgqMapped && ctx.mes.pipe[0].inited && ctx.mes.pipe[0].enabled) {
        kern_return_t r = mes_remove_hw_queue(dev, ctx.mes, kMESQueueType_GFX,
                                              /*pipe_id=*/0, /*queue_id=*/0,
                                              ctx.cp.doorbell_index);
        INIT_LOG("teardown: MES REMOVE_QUEUE(GFX) %s (%#x)",
                 r == kIOReturnSuccess ? "acked" : "not acked", r);
        ctx.kgqMapped = false;
        ctx.cp.kgq_mapped = false;
    }

    if (ctx.kcqMapped && ctx.mes.pipe[0].inited && ctx.mes.pipe[0].enabled) {
        kern_return_t r = mes_remove_hw_queue(dev, ctx.mes, kMESQueueType_COMPUTE,
                                              ctx.kcq.pipe, ctx.kcq.queue,
                                              ctx.kcq.doorbell_index);
        INIT_LOG("teardown: MES REMOVE_QUEUE(COMPUTE) %s (%#x)",
                 r == kIOReturnSuccess ? "acked" : "not acked", r);
        ctx.kcqMapped = false;
    }

    // (2) Stop the engines that fetch from memory: SDMA queues first (they
    //     have no dependency on the CP), then the CP front-ends and the MEC.
    if (dev.ip.isResolved(IPBlock::GC)) {
        for (uint32_t i = 0; i < kSDMAInstanceCount; i++) {
            if (!ctx.sdma.instance[i].inited) continue;
            sdma_gfx_stop_instance(dev, i);
            sdma_engine_halt(dev, i, true);
        }
        if (ctx.cp.inited || ctx.reached >= BringupStage::CPInit) {
            cp_enable(dev, false);
            cp_compute_enable(dev, false);
        }
        // (3) Park the scheduler last: REMOVE_QUEUE above needed it running.
        if (ctx.mes.pipe[0].enabled) {
            kern_return_t r = mes_enable(dev, ctx.mes, false);
            INIT_LOG("teardown: MES disable %s (%#x)",
                     r == kIOReturnSuccess ? "ok" : "failed", r);
        }
    }

    // (4) Interrupt source off before its ring is freed.
    if (ctx.ih.inited) ih_v7_0_hw_fini(dev, ctx.ih);

    // (4b) Destroy the PSP GPCOM ring. The SOS stays alive across a kext
    //      reload (we never reset it), and it remembers the ring — so
    //      without this the next load's psp_ring_create is refused with
    //      status 0x115 and the ladder stops at stage 6. Found by the first
    //      kmutil unload/load test.
    if (ctx.psp.ringCreated) psp_ring_destroy(dev, ctx.psp);

    // (5) Release every allocation, innermost first. Each of these is a
    //     no-op on a context that never got that far.
    for (uint32_t i = 0; i < kSDMAInstanceCount; i++)
        sdma_release_storage(ctx.gmc, ctx.sdma.instance[i]);
    mes_release_storage(ctx.mes);
    cp_compute_queue_release(ctx.gmc, ctx.kcq);
    cp_release_storage(ctx.cp);
    ih_release(ctx.ih);
    psp_release(ctx.psp);
    gmc_release_resources(ctx.gmc);

    ctx.gfxhubReady      = false;
    ctx.doorbellPathReady = false;
    ctx.reached          = BringupStage::None;
    INIT_LOG("teardown: done — engines halted, scheduler parked, all buffers released");
}

kern_return_t bringup_to(BringupContext &ctx, BringupStage target) {
    if (!ctx.dev) return kIOReturnNotReady;
    if ((uint32_t)target > (uint32_t)BringupStage::Max) target = BringupStage::Max;
    for (uint32_t s = (uint32_t)ctx.reached + 1; s <= (uint32_t)target; s++) {
        kern_return_t r = run_stage(ctx, (BringupStage)s);
        ctx.lastResult = r;
        if (r != kIOReturnSuccess) {
            ctx.failedAt = (BringupStage)s;
            if (s >= (uint32_t)BringupStage::RLCInit && s != (uint32_t)BringupStage::MESInit) dump_failure_state(ctx, stage_name((BringupStage)s));
            INIT_LOG("stage %u (%s) FAILED: %#x — ladder stopped; reached %s", s, stage_name((BringupStage)s), r, stage_name(ctx.reached));
            return r;
        }
        ctx.reached = (BringupStage)s;
        INIT_LOG("stage %u (%s) done", s, stage_name((BringupStage)s));
    }
    INIT_LOG("reached stage %u (%s)", (unsigned)ctx.reached, stage_name(ctx.reached));

    // Leave the card idling cool. Enabling all SMU features powers the GFX
    // core, but nothing here gates it again, so with no work queued the board
    // sat at its full 304 W limit with the hotspot at 86 C and the fan ramping
    // — measured, the first thing telemetry showed. Clamping
    // PPCLK_GFXCLK to the low state drops that to ~32 W and 50 C with every
    // test still passing (the suite runs perhaps 2x slower). Only GFXCLK is
    // touched; display scanout runs off DISPCLK/DCFCLK and is unaffected.
    // `navi48test power 0` restores the automatic state for benchmarking.
    if (ctx.idleClamp && ctx.smuOnline && ctx.dev) {
        kern_return_t pr = smu_set_power_state(*ctx.dev, SMUPowerState::Low);
        INIT_LOG("idle clamp: GFXCLK -> low %s (%#x) — the GFX core is powered but "
                 "ungated, so an unclamped idle burns full board power. "
                 "`navi48test power 0` to unclamp.",
                 pr == kIOReturnSuccess ? "applied" : "FAILED", pr);
    }
    return kIOReturnSuccess;
}

} // namespace amdgpu
