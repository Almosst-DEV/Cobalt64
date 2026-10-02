//
//  native_s1c.h - the kernel half of native-stack step S1c (kext 0.0.601): the engine behind the native user client
//  (Navi48NativeClient, type 'N48N'). Contract: notes/design/NATIVE-S1C-ABI.md. Default OFF: nothing here runs unless a client opens,
//  and a client opens only on a native boot (navi48-native=1 accepted) whose S1b self-test reported POSITIVE PASS.
//
//  One client at a time, on VMID 8, with a fresh empty 4-level tree per client. All GPU work goes through the kernel GFX ring (the
//  KGQ) with the ring-safety rules of the contract section 6: a free-space check against the write-back rptr, one client lock, a
//  kext-global ring lock, and a HUNG latch after which memory is leaked instead of freed.
//
#pragma once
#include <stdint.h>
#include <IOKit/IOReturn.h>
#include <IOKit/IOTypes.h>
#include <mach/mach_types.h>   // task_t (0.0.612: n1c_bo_import_host)

#include "../Navi48NativeABI.h"
#include "amdgpu_init.h"
#include "native_s1b.h"
#include "native_s1c_pure.h"

class IOMemoryDescriptor;

namespace amdgpu {

constexpr uint32_t kN1cKextBuild = 620;   // the REAL build (0.0.620): Hello / QueryInfo report it; tests/native_s2a_test.cpp, native_s2_dal_test.cpp and native_s2d_test.cpp pin it to Info.plist so it cannot go stale again

// Legacy-client sites refused once the native VM self-test has touched the GPU (contract 6.2). The legacy ring writers and the pool
// users that share vram_alloc without a lock.
enum N1cLegacySite : uint32_t { kN1cSiteSelfTest = 2, kN1cSiteAllocVRAM = 3, kN1cSiteFreeVRAM = 4, kN1cSiteWriteVRAM = 5 };
// True (refuse, log once per site) when native_s1b_latched(); false otherwise, which is EVERY non-native boot.
bool n1c_refuse_legacy(uint32_t site);

// Open: gate checks, exclusivity, and the fresh tree. Returns kIOReturnSuccess, NotReady (gate / S1b / HUNG / device not ready),
// ExclusiveAccess (a client is open) or NoMemory. Logs one line per refusal.
IOReturn n1c_open(BringupContext &ctx);
// Close (clientClose / clientDied / stop): idempotent. `how` names the caller in the log line.
void n1c_close(const char *how);
bool n1c_is_open();
// 0.0.610 (ABI 1.8): read-only views for the Metal nub's gate. Hello done on the live session / the GPU declared HUNG this boot.
bool n1c_hello_done();
bool n1c_hung();

// Selector bodies. The caller (the client) has already checked scalar counts and struct sizes EXACTLY. Every one returns NotReady
// until Hello succeeded. Locking is described in the contract: all but QueryInfo / ReadRegs / WaitSeq take the client lock.
IOReturn n1c_hello(uint64_t clientAbi, uint64_t flags, uint64_t out[4]);
IOReturn n1c_query_info(n48n_info *out);
IOReturn n1c_read_regs(uint64_t dwordOff, uint64_t count, uint64_t instance, uint32_t *out);
IOReturn n1c_bo_create(const n48n_gem_create_in *in, uint64_t out[4]);
IOReturn n1c_bo_free(uint64_t handle);
// 0.0.612 (ABI 1.9), selector 21: import [hostVa, hostVa + size) of `task`'s address space (the caller's own: the client passes the task it was opened with) as a SYSTEM-memory BO. gpuVa == 0: handle
// only (map later with GemVa MAP); else map at import at gpuVa with the GemVa vm `flags`. out[0] handle [1] size [2] GPU VA mapped (0 if none) [3] N48N_PLACED_HOST_IMPORT. Limits and order: native_hostimport_pure.h.
IOReturn n1c_bo_import_host(task_t task, uint64_t hostVa, uint64_t size, uint64_t flags, uint64_t gpuVa, uint64_t out[4]);
// 0.0.612 (review item A): latch this GPU's PCI BARs (n entries, base + FULL size each, 0 <= n <= 7) once at start; the first call wins, later calls are ignored. With nothing latched every host import is refused.
void n1c_latch_pci_bars(const uint64_t *base, const uint64_t *size, uint32_t n);
IOReturn n1c_gem_va(const n48n_gem_va *in);
IOReturn n1c_ctx(const n48n_ctx *in, n48n_ctx *out);
IOReturn n1c_submit(const uint8_t *in, uint32_t size, uint64_t *seqOut);
IOReturn n1c_wait(uint64_t target, uint64_t timeoutNs, uint64_t out[3]);
// ---- ABI 1.1 (kext 0.0.603, native S2a): the scanout selectors 9..14. Same shape rules as the selectors above (the client checked counts and
// sizes exactly); all need Hello. Lock order: gCliLock, then the scanout (DCN) lock inside n48dcn::scan*. Contract: NATIVE-S1C-ABI.md, "ABI 1.1 addendum".
IOReturn n1c_scan_query(n48n_scan_query *out);
IOReturn n1c_scan_acquire(uint64_t flags, uint64_t out[2]);
IOReturn n1c_scan_register(const n48n_scan_reg *in, uint64_t out[2]);
IOReturn n1c_scan_present(uint64_t slot, uint64_t flags, uint64_t out[3]);
IOReturn n1c_scan_status(n48n_scan_status *out);
IOReturn n1c_scan_release(uint64_t out[2]);
// ---- ABI 1.2 (kext 0.0.604, native S2-DISPCLK): selector 15. Runs one named experiment step of notes/design/NATIVE-S2-DISPCLK.md (1 E1b, 2 E2, 3 E3, 4 E4)
// against the DAL mailbox (smu_dal.cpp) and fills *out. BadArgument for a step outside 1..4 or non-zero flags, NotReady until Hello; otherwise
// Success with the verdict inside *out (DENIED when the gate, the navi48-dalsmc level, the order, the latch or a concurrent step refuses it).
// The client lock is NOT held while the step runs (it sleeps for up to ~65 s): a busy flag refuses a second concurrent step.
IOReturn n1c_dal_step(uint64_t step, uint64_t flags, n48n_dal_result *out);
// ---- ABI 1.3 (kext 0.0.605, native S2d): selector 16. Runs one timed mode trial (row 50 or 120, dwell in ms) through n48dcn::modeTrial and fills *out (n48n_mode_result).
// BadArgument for non-zero flags or a null out, NotReady until Hello; otherwise Success with the verdict inside *out (DENIED when nothing was written). The client lock is NOT held
// while the trial runs (it sleeps for up to dwell + ~12 s); the trial's own busy flag refuses a second one.
IOReturn n1c_mode_hold(uint64_t maxMs, uint64_t flags, n48n_mode_result *out);   // 0.0.609 (ABI 1.7): the row-120 HELD mode; returns with the mode UP (or the trial's ordinary verdict)
IOReturn n1c_mode_release(uint64_t flags, n48n_mode_result *out);                // 0.0.609 (ABI 1.7): end the hold, wait, the final result
IOReturn n1c_mode_trial(uint64_t row, uint64_t dwellMs, uint64_t flags, n48n_mode_result *out);   // 0.0.606 (ABI 1.4): flags = N48N_MODE_TF_*, the result is 512 B
// clientMemoryForType(type = BO handle): the +1 descriptor to map, or NotFound / NotPermitted.
IOReturn n1c_memory_for_handle(uint32_t handle, IOOptionBits *options, IOMemoryDescriptor **memory);

} // namespace amdgpu
