//
//  amdgpu_init.h — the bring-up ladder (port of mac-amdgpu's amdgpu_init.cpp
//  bringup_to/run_stage, MIT, commit 3bdeed2). Stage numbers are the reference's
//  BringupStage values; boot-arg navi48-stage=N runs the ladder up to N.
//
//  Deviations: stage 5 (PSPLoadSOS) adopts the SOS our src/psp.cpp chain already
//  booted; stage 8 (PSPFwLoad) loads the embedded blobs in-kernel via fw_loader
//  (the reference's host app fed them); stage 9's clock/power part of SMU setup
//  is opt-in (navi48-smu-full=1); stages whose modules are not linked yet return
//  kIOReturnUnsupported and stop the ladder there.
//
#pragma once
#include "amdgpu_regs.h"
#include "amdgpu_psp.h"
#include "amdgpu_gmc.h"
#include "amdgpu_gart.h"
#include "amdgpu_ih.h"
#include "amdgpu_imu.h"
#include "amdgpu_gfx.h"
#include "amdgpu_smu.h"
#include "amdgpu_rlc.h"
#include "amdgpu_cp.h"
#include "amdgpu_sdma.h"
#include "amdgpu_mes.h"
#include "amdgpu_pm4.h"
#include "compute_test.h"
#include "amdgpu_sysmem.h"
#include "amdgpu_doorbell.h"
#include "fw_loader.h"

namespace amdgpu {

enum class BringupStage : uint32_t {
    None          = 0,
    IPDiscovery   = 1,
    IHInit        = 2,
    GMCInit       = 3,
    PSPInit       = 4,
    PSPLoadSOS    = 5,
    PSPRingCreate = 6,
    TMRSetup      = 7,
    PSPFwLoad     = 8,
    SMUInit       = 9,
    IMUInit       = 10,
    RLCInit       = 11,
    CPInit        = 12,
    MESInit       = 13,
    GFXInit       = 14,
    SDMAInit      = 15,
    PM4Test       = 16,   // ours: PM4 WRITE_DATA + EOP fence through the GFX ring (CP proof)
    ComputeDispatch = 17, // ours: gfx1201 shader stores a value; readback (Phase 3 finish line)
    Max           = 17,
};
const char *stage_name(BringupStage s);

struct BringupContext {
    DeviceContext *dev { nullptr };
    PSPContext     psp {};
    GMCContext     gmc;
    GARTContext    gart {};
    IHContext      ih {};
    IMUContext     imu {};
    GFXConfig      gfx {};
    RLCContext     rlc {};
    CPContext      cp {};
    ComputeQueue   kcq {};        // kernel compute queue (MEC), mapped by the MES
    SDMAContext    sdma {};
    MESContext     mes {};
    SDMAVRAMCopyResult sdmaCopy {};
    bool           sdmaCopyPassed { false };
    // Option B' self-test. Runs at the END of SDMAInit (stage 15) — the first
    // point in the ladder where SDMA can issue a VMID-0 write, and early
    // enough that a revert still leaves stages 16/17 running on the flat
    // table. Inert unless navi48-pdb0=1 put the PDB0 in place at stage 12.
    PDB0SelfTest   pdb0Test {};
    // 0.0.185 — the SDMA0 QUEUE1 doorbell-routing self-test (navi48-sdma-q1-test=1).
    // Runs at the END of SDMAInit for the same reason the PDB0 one does: QUEUE0
    // has just resumed (its MCU unhalt and instance-wide registers are what
    // QUEUE1 borrows), and nothing downstream depends on QUEUE1. Settles open
    // question 1 of notes/re/sdma-takeover-design.md — does one S2A window route
    // two indices to two queues on one engine — with OUR ring, before `sdmamap`
    // ever points the engine at Apple's. Inert unless the boot-arg is set.
    bool             sdmaQ1TestWant { false };
    SDMAQ1TestResult sdmaQ1Test {};
    // 0.0.196 (navi48-sdma-qn-test=1): the SAME self-test run once per entry of
    // kSDMAExtSlots, so every (instance, queue, doorbell) triple `sdmamap` may
    // hand Apple has been proven on this boot before Apple's ring touches it.
    bool             sdmaQnTestWant { false };
    SDMAQ1TestResult sdmaQnTest[kSDMAExtSlotCount] {};
    // 0.0.191 — the SRBM_WRITE self-test (navi48-srbm-test=1). Runs LAST in
    // SDMAInit, after the copy test and the two tests above, for the same
    // reason: QUEUE0 has just been proven this boot, and no later stage reads
    // SCRATCH_REG7 or GCVM_CONTEXT2's page-table base. Answers's open
    // question — Apple's SDMA SRBM_WRITEs of the VM-context registers fenced
    // but the registers read zero — with OUR queue and OUR packets. Inert
    // unless the boot-arg is set.
    bool               srbmTestWant { false };
    SDMASRBMTestResult srbmTest {};
    // 0.0.193 — the Apple-shaped-page-table self-test (navi48-vmfrag-test=1).
    // Runs after the SRBM one, i.e. last of all, so a leg that faults cannot
    // cost an earlier test its queue. Builds a DEPTH 1 / BLOCK 7 tree of our own
    // on GFXHUB CONTEXT1 (VMID 1) and restores CONTEXT1 afterwards; Apple's
    // arena is never read or written. Inert unless the boot-arg is set.
    bool             vmfragTestWant { false };
    bool             vmfragNoValidWant { false };   // M4-WS-VMID-VALID: navi48-vmfrag-novalid=1 (needs navi48-vmfrag-test=1 too)
    VMFragTestResult vmfragTest {};
    bool           cpEopPassed { false };
    bool           cpWriteDataPassed { false };
    uint32_t       cpEopFence { 0 };
    ComputeTestResult computeResult {};
    bool           computePassed { false };
    FwLoaderState  fw;
    BringupStage   reached { BringupStage::None };
    BringupStage   failedAt { BringupStage::None };
    kern_return_t  lastResult { kIOReturnSuccess };
    bool smuFullSetup { false };    // navi48-smu-full=1: run smc_hw_setup (clocks/features)
    bool smuOnline { false };
    bool gfxhubReady { false };     // GFXHUB GART enabled + gart_init done (needed before CP/MES/SDMA touch GART sysmem)
    bool doorbellPathReady { false };
    bool mesNonFatal { false };      // navi48-mes-nonfatal=1: keep going past a failed MESInit (diagnostics)
    bool mesFailed { false };
    bool cpLegacyRb { false };       // navi48-cp-legacy-rb=1: program CP_RB0_* directly at CPInit (stage 16 runs 1-3 path)
    bool doorbellLegacy { false };   // navi48-doorbell-legacy=1: the pre-0.0.18 ad-hoc doorbell map (A/B only)
    bool wantInterrupts { false };   // navi48-interrupts=1 AND the kext armed an MSI source
    // Set when an earlier load of this kext already took the GPU through
    // PSPFwLoad THIS BOOT (the marker is a property on the IOPCIDevice, which
    // outlives our unload). The PSP will not re-load firmware it already holds
    // in the TMR — the second LOAD_IP_FW simply never fences — so stages 4..8
    // are skipped and the engines adopt what is already there.
    bool adoptProvisioned { false };
    bool tornDown { false };         // bringup_teardown() has run
    bool kgqMapped { false };        // PM4Test: MES acked ADD_QUEUE(map_legacy_kq) for the kernel GFX ring
    bool cpRingTestPassed { false }; // PM4Test: gfx_v12_0_ring_test_ring (SET_UCONFIG_REG SCRATCH_REG0) landed
    bool idleClamp { true };         // navi48-idle-clamp=0 to disable: clamp GFXCLK after the ladder
    bool wantKcq { false };
    // navi48-hsa-abi=1: dispatch the LLVM-compiled kernel (shaders/store_magic_hsa.s)
    // with the standard HSA kernarg convention instead of the hand-written shader.
    // Off by default — the hand-written path is the proven stage-17 result and stays
    // the reference. See notes/B1-COMPILER-BRIDGE.md.
    bool hsaAbi { false };          // navi48-kcq=1: create the kernel compute queue (opt-in, see below)
    bool kcqMapped { false };        // PM4Test: MES acked ADD_QUEUE(COMPUTE) for the kernel compute queue
    bool kcqTestPassed { false };    // PM4Test: a WRITE_DATA IB executed on the compute ring
    uint32_t smuVersion { 0 };
};

// Runs stages reached+1 .. target in order; stops at the first failure.
kern_return_t bringup_to(BringupContext &ctx, BringupStage target);
kern_return_t run_stage(BringupContext &ctx, BringupStage s);

// Unwind whatever bringup_to() brought up, in reverse order, and release
// every buffer the ladder allocated. Safe to call at any reached stage and
// safe to call twice. Called from Navi48Bringup::stop(), so `kmutil unload`
// leaves the GPU quiescent instead of leaving MES scheduling a queue whose
// memory the kernel has reclaimed.
//
// It deliberately does NOT reset the GPU or tear the PSP down: the console
// framebuffer is still scanning out of VRAM the whole time.
void bringup_teardown(BringupContext &ctx);

} // namespace amdgpu
