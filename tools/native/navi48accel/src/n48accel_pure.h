//
//  n48accel_pure.h - the pure (no IOKit, no kernel) decisions of the Navi48Accel aux kext (route A, milestone #9). Compiled into the kext AND into
//  tests/host_test.cpp, which drives these very functions (with planted breaks: tests/plant.sh). Design: notes/design/NATIVE-S3.md.
//
//  THIN BY DESIGN (K3: every rebuild of this kext requires a security approval (Allow click) from the user): only the safety gates, the fixed class graph and the
//  fail-closed defaults live here. All values, stamp / task / config / factory decisions and logging live in the bring-up kext behind
//  Navi48MetalOps.h. What stays here and why: kill switch (must work with the bring-up kext absent or broken), the layout gate (it inspects THIS
//  binary's vtables), the event-machine init ORDER (a structural call sequence into the family), the class graph, and "no ops => refuse".
//
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "Navi48MetalOps.h"

namespace n48accel {

// ---- kill switch (MUST-FIX 5) --------------------------------------------------------------------------------------------------------------
// boot-arg navi48-aux=0 disables EVERYTHING this kext does. Absent or any other value: enabled. Every probe/start/factory begins with this test
// (the N48_AUX_ENTER guard in Navi48Accel.cpp, pinned by tests/host_test.cpp's source scan: it must be the first statement of each entry point).
inline bool aux_enabled(bool argPresent, uint32_t argValue) { return !(argPresent && argValue == 0u); }

// ---- the ops table -----------------------------------------------------------------------------------------------------------------------
enum OpsVerdict : uint32_t { kOpsOk = 0, kOpsNull = 1, kOpsMagic = 2, kOpsAbi = 3, kOpsSize = 4, kOpsMissing = 5 };
// Required hooks of ABI 1 present? Optional ones are never checked here. An ABI-1 table stays valid after ABI 2 (it means "display off", see disp_enabled).
inline OpsVerdict ops_check(const N48MetalOps *o, uint32_t mySize = N48_METAL_OPS_MIN) {
    if (!o) return kOpsNull;
    if (o->magic != N48_METAL_OPS_MAGIC) return kOpsMagic;
    if (o->abi < N48_METAL_ABI_MIN) return kOpsAbi;             // append-only table: ABI 1 (0.0.612) and every newer abi are accepted (unknown members ignored)
    if (o->size < mySize) return kOpsSize;
    if (!o->device_open || !o->device_close || !o->populate_config || !o->stamp_memory || !o->stamp_va || !o->task_window) return kOpsMissing;
    return kOpsOk;
}
// populateAccelConfig fail-closed default. Without a good hook result the aux kext does NOT blank the name (a NULL name is dereferenced by
// IOAccelDevice2::get_name, and the family's validateConfigStructure is never called in start): it leaves the config as the family defaults (non-zero IOSurface
// limits) with its own static non-NULL name, tears the device down (stamp VA and ctx cleared) so the event machine init fails and the family's start aborts.
inline bool config_keep(bool opsOk, int rc) { return opsOk && rc == 0; }
// A hook is usable only when its capability bit is set (and the table is big enough to contain it: ops_check guarantees size >= N48_METAL_OPS_MIN).
inline bool cap_has(const N48MetalOps *o, uint64_t bit) { return o && (o->caps & bit) != 0u; }
// Optional factories: only with a good ops table AND the bit set by the bring-up kext.
inline bool factory_allowed(bool opsOk, uint32_t mask, uint32_t bit) { return opsOk && (mask & bit) != 0u; }
// mm_hook fail-closed default: absent hook or non-zero result => the memory map operation returns false.
inline bool mm_result(bool opsOk, bool haveHook, int rc) { return opsOk && haveHook && rc == 0; }

// ---- the runtime vtable/layout gate (MUST-FIX 3) -------------------------------------------------------------------------------------------
// For each class we build: OUR linked vtable must equal the family's __ZTV<Parent> slot for slot, except the slots we implement (overrides:
// dtors, getMetaClass, the pure virtuals, our probe/start/free, the forwarders), which must point INTO our own kext's text and must not equal the
// family's slot. Both tables must be n slots long; ours must be zero-terminated. Fails closed: the first mismatch is remembered and every later
// probe refuses.
enum GateVerdict : uint32_t { kGateOk = 0, kGateNull = 1, kGateSlotDiffers = 2, kGateOverrideOutside = 3, kGateNoTerminator = 4, kGateFamilyNull = 5, kGateEmpty = 6, kGateOverrideIsFamily = 7, kGateFamilyLonger = 8 };
struct GateResult { GateVerdict v; uint32_t slot; uint64_t ours, fam; uint32_t compared; };
inline bool in_list(const uint16_t *l, uint32_t n, uint32_t i) { for (uint32_t k = 0; k < n; ++k) if (l[k] == i) return true; return false; }
inline GateResult gate_compare(const uintptr_t *ours, const uintptr_t *fam, uint32_t nslots, const uint16_t *ovr, uint32_t novr, uintptr_t textLo, uintptr_t textHi) {
    GateResult r = { kGateOk, 0, 0, 0, 0 };
    if (!ours || !fam) { r.v = kGateNull; return r; }
    if (nslots == 0) { r.v = kGateEmpty; return r; }
    for (uint32_t i = 0; i < nslots; ++i) {
        r.compared = i + 1;
        if (fam[i] == 0) { r.v = kGateFamilyNull; r.slot = i; return r; }
        if (in_list(ovr, novr, i)) {
            if (ours[i] < textLo || ours[i] >= textHi) { r.v = kGateOverrideOutside; r.slot = i; r.ours = ours[i]; r.fam = fam[i]; return r; }
            if (ours[i] == fam[i]) { r.v = kGateOverrideIsFamily; r.slot = i; r.ours = ours[i]; r.fam = fam[i]; return r; }
        } else if (ours[i] != fam[i]) { r.v = kGateSlotDiffers; r.slot = i; r.ours = ours[i]; r.fam = fam[i]; return r; }
    }
    if (ours[nslots] != 0) { r.v = kGateNoTerminator; r.slot = nslots; r.ours = ours[nslots]; return r; }
    if (fam[nslots] != 0) { r.v = kGateFamilyLonger; r.slot = nslots; r.fam = fam[nslots]; return r; }   // the family's vtable is longer than the header says
    return r;
}
// The text window our override slots must lie in: the kext's own [kmod_info.address, +size) when that range is filled in and contains one of our functions (the anchor);
// otherwise (a KC that leaves kmod_info unrelocated) +-4 MiB around the anchor. Returns true when the kmod_info range was used.
inline bool text_window(uintptr_t anchor, uintptr_t kaddr, uintptr_t ksize, uintptr_t *lo, uintptr_t *hi) {
    if (kaddr != 0 && ksize != 0 && anchor >= kaddr && anchor - kaddr < ksize) { *lo = kaddr; *hi = kaddr + ksize; return true; }
    *lo = anchor - 0x400000u; *hi = anchor + 0x400000u; return false;
}
inline bool size_ok(uint32_t have, uint32_t want) { return have == want; }

// ---- event machine init order (MUST-FIX 2) ------------------------------------------------------------------------------------------------
// Fast2::init FIRST; only if it returned true, setStampBaseAddress(the device's stamp VA) -- and only with a stamp VA. Returns whether the
// machine may be used. (AppleParavirtEventMachine::init at 0x142b6b66: Fast2::init, `testb %al`, getStampBaseAddress, setStampBaseAddress.)
enum EmStep : uint8_t { kEmInitSuper = 1, kEmSetStamp = 2 };
struct EmPlan { bool ok; uint8_t steps[2]; uint8_t nsteps; };
inline EmPlan em_init_plan(bool superInitOk, bool haveStampVA) {
    EmPlan p = { false, { 0, 0 }, 0 };
    p.steps[p.nsteps++] = kEmInitSuper;
    if (!superInitOk) return p;
    if (!haveStampVA) return p;
    p.steps[p.nsteps++] = kEmSetStamp;
    p.ok = true;
    return p;
}

// ---- the display pipe (ABI 2, aux 0.0.3; notes/design/NATIVE-S4-M11H.md section 3.4 and the "11h.1 RE facts" section) ---------------------------------
// Display is ON only with a table of abi >= 2 whose OWN size covers the ABI-2 members, the ON flag set by the bring-up kext (boot-arg navi48-metal-disp=1
// latched), and the hook present. The order of the tests matters: nothing past byte 120 is read unless the table says it has it. OFF (an ABI-1 table from the
// 0.0.612 bring-up kext, a short table, flag 0, no hook, no table) = every display entry point behaves exactly as aux 0.0.2 did: the family's own display pipe,
// the family's own display-machine start with the provider it was given, the family's own pipe slots.
inline bool disp_enabled(const N48MetalOps *o) {
    return o && o->abi >= 2u && o->size >= N48_METAL_OPS_V2 && (o->disp_flags & N48_DISP_F_ON) != 0u && o->disp_hook != nullptr;
}
// Navi48DisplayMachine::start: the provider the family's framebuffer walk starts from. The walk (IOAccelDisplayMachine::start, M11H section 3.2) looks only at
// the children and clients of its argument; our argument is the nub, which has no framebuffer below it (RDNA4FB hangs off the GPU's PCI device). So with display
// on, a nub argument and a PCI device from the bring-up kext, the walk starts at the PCI device; in every other case at what the family passed (0.0.2 behaviour).
inline void *dm_walk_provider(bool dispOn, bool providerIsNub, void *provider, void *pci) { return (dispOn && providerIsNub && pci) ? pci : provider; }
// Navi48Accelerator::newDisplayPipe: our subclass only with display on AND a successful allocation; otherwise the family's own allocation. Never NULL from us
// (11h.1 correction 4: found_framebuffer stores the pipe without a NULL check).
enum PipeChoice : uint32_t { kPipeOurs = 1, kPipeFamily = 2 };
inline PipeChoice pipe_choice(bool dispOn, bool allocated) { return (dispOn && allocated) ? kPipeOurs : kPipeFamily; }

// ---- task window (only the shape check; the values come from the bring-up kext) ------------------------------------------------------------
constexpr uint64_t kPage = 0x1000ull;
inline bool task_window_ok(uint64_t size, uint64_t reserve) { return size >= 0x100000ull && (size & (kPage - 1)) == 0 && reserve < size && (reserve & (kPage - 1)) == 0; }

} // namespace n48accel
