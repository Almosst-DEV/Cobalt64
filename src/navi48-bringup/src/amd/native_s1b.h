//
//  native_s1b.h - the kernel half of native-stack step S1b (kext 0.0.600). Default OFF: nothing here runs unless the boot-arg
//  navi48-native=1 is present AND no Apple-path / self-test boot-arg is armed (n48native::gate_decide, native_s1b_pure.h).
//
//  What it does, once, at the end of the ladder (stage 17 passed): programs GFXHUB CONTEXT<vmid> (vmid 8, outside MES's 0xFE mask)
//  with upstream's geometry (DEPTH 3 / BLOCK_SIZE 0 / RETRY 0) and a 4-level table of our own in VRAM, then submits IBs on the
//  kernel GFX ring under that VMID: WRITE_DATA at low and high-half VAs, an IB fetched FROM a high-half VA, a compute dispatch
//  (shader fetch), and last one deliberately unmapped VA that must raise a VM fault while the NEXT fence still lands.
//
#pragma once
#include <stdint.h>
#include <IOKit/IOReturn.h>

#include "amdgpu_init.h"
#include "native_s1b_pure.h"

namespace amdgpu {

struct NativeS1bState {
    uint32_t gate { 0 };              // n48native::GateState
    uint32_t conflicts { 0 };         // n48native::GateBit mask when refused
    bool     ran { false };           // the self-test body executed
    bool     resultPass { false };
    bool     positivePass { false };  // the four positive legs (low, high, mixed, compute) all passed
    const char *faultWord { "FAIL" }; // PASS | REFUSED | FAIL for the fault leg
    bool     stopped { false };       // the latched stop fired
    bool     faultAllowed { false };
    uint32_t vmid { 0 };
    uint32_t l2FaultCntl { 0 };       // GCVM_L2_PROTECTION_FAULT_CNTL as found
    uint32_t cpDebug { 0 };
    uint32_t cntlBefore { 0 };
    uint32_t cntlAfter { 0 };         // CONTEXT<vmid>_CNTL read back
    uint32_t leg[n48native::kLegCount] { 0, 0, 0, 0, 0 };   // n48native::LegResult
    uint32_t legBad[n48native::kLegCount] { 0, 0, 0, 0, 0 };
    uint64_t tableMc { 0 };
    uint64_t dataMc { 0 };
    uint32_t faultStatusLo { 0 };
    uint64_t faultVa { 0 };
    bool     nextFenceLanded { false };
};

// Record the gate's decision (called from Navi48Bringup::runStages before the ladder).
void native_s1b_set_gate(uint32_t gate, uint32_t conflicts);
const NativeS1bState &native_s1b_state();

// Run the self-test. Requires ctx.reached == ComputeDispatch with the compute test passed (the ring, the fences and the IH are
// then proven this boot). Every wait is bounded at n48native::kWaitBoundUs; a timeout latches the stop.
kern_return_t native_s1b_run(BringupContext &ctx);

// True once the self-test has begun to touch the GPU on this boot. native_s1b_refuse(site) is the guard the Apple verbs (site 0)
// and the user client's SubmitIB (site 1) call: it returns true (refuse, log once per site) when latched, false otherwise.
bool native_s1b_latched();
bool native_s1b_refuse(uint32_t site);

// One line for the IORegistry ("Navi48,NativeS1b"): the verdict, then each leg. Returns the length written.
uint32_t native_s1b_format(char *buf, uint32_t n);

} // namespace amdgpu
