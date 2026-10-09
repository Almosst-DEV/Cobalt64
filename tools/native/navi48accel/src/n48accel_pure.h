//
//  n48accel_pure.h - the pure (no IOKit, no kernel) decisions of the Navi48Accel aux kext (route A, milestone #9). Compiled into the kext AND into
//  tests/host_test.cpp, which drives these very functions (with planted breaks: tests/plant.sh). Design: an internal design note.
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
#include "Navi48DisplayOps.h"   // aux 0.0.4 (M5): the framebuffer's snapshot / ops table (byte-identical copy of the bring-up kext's)

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

// ---- the display pipe (ABI 2, aux 0.0.3; an internal design note section 3.4 and the "11h.1 RE facts" section) ---------------------------------
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

// ---- the 'N48N' user client on the accelerator (aux 0.0.5, G6; an internal design note "G6 design", section 2) ---------------------------------------------------------------------------------------------
// Navi48Accelerator overrides IOGraphicsAccelerator2::newUserClient (slot 239, the 4-argument form). A sandboxed application may open any user client on a service that conforms to IOAccelerator; this is how it reaches the
// bring-up kext's native client. The decision for one open is nuc_plan:
//   * type != 'N48N'                         -> kNucFamily: the family's own newUserClient, untouched (its per-PID limits and bookkeeping are the family's);
//   * type == 'N48N', everything usable      -> kNucNative: the ops table's native_open (ABI 3, capability N48_CAP_NATIVE_OPEN), whose IOReturn is returned UNCHANGED;
//   * type == 'N48N', anything else          -> kNucUnsupported (kIOReturnUnsupported): kill switch off, layout gate not passed, no ops table, an ABI < 3 / short table, the capability bit clear, no hook, no device.
//     It NEVER falls through to the family: the family hands every unknown type to newContext (slot 331) and would give the caller a generic IOAccelContext2 (design section 2, CONFIRMED in the disassembly).
enum NucPlan : uint32_t { kNucFamily = 1, kNucNative = 2, kNucUnsupported = 3 };
inline bool native_open_usable(const N48MetalOps *o) {
    return o && o->abi >= 3u && o->size >= N48_METAL_OPS_V3 && cap_has(o, N48_CAP_NATIVE_OPEN) && o->native_open != nullptr;
}
inline NucPlan nuc_plan(uint32_t type, bool auxOn, bool gatePass, const N48MetalOps *o, bool haveDevice) {
    if (type != N48_METAL_UC_N48N) return kNucFamily;
    if (!auxOn || !gatePass || !haveDevice || !native_open_usable(o)) return kNucUnsupported;
    return kNucNative;
}
// The hook's answer is an IOReturn, passed through as is (kIOReturnSuccess = 0). A non-zero value is a refusal, a zero with no client object is an internal error: a "1 = handled" reading is wrong here.
constexpr int32_t kIoSuccess = 0, kIoInternalError = (int32_t)0xE00002C9, kIoUnsupported = (int32_t)0xE00002C7, kIoBadArgument = (int32_t)0xE00002C2;
inline int32_t nuc_result(int32_t rc, bool haveClient) { return rc != kIoSuccess ? rc : (haveClient ? kIoSuccess : kIoInternalError); }

// ---- the monitor B framebuffer (aux 0.0.4, milestone M5; an internal design note "M5 build spec: Navi48Framebuffer for the monitor B", section 2) ----------------------------------------------------------------
// Navi48Framebuffer answers IOFramebuffer's questions from the IMMUTABLE N48DispSnap the bring-up kext took at `fbpublish 2` (Navi48DisplayOps.h). Every answer below is a pure function of the snapshot, host-tested; the class in
// Navi48Accel.cpp only copies the results into IOKit structures. No register is read or written, nothing is looked up at run time. The shared validators (n48disp_ops_check, n48disp_snap_valid) are in Navi48DisplayOps.h.

// start: refuse unless EVERYTHING holds, in this order: the kill switch (navi48-aux=0), boot-arg navi48-metal-ws present (any value: that boot gives the DP to the GPU desktop, the monitor B would race it), the provider is a
// Navi48DisplayNub, the RUNTIME LAYOUT GATE passed (all 13 classes, IOFramebuffer's 350 slots included: on an fb2 boot no accelerator is probed, so the framebuffer must run the gate itself), the ops table is good (magic / ABI / size /
// hooks), the snapshot hook answered (rc 0) and the snapshot is valid. Only then does the class call IOFramebuffer::start.
enum FbStart : uint32_t { kFbStartOk = 0, kFbStartAuxOff = 1, kFbStartMetalWs = 2, kFbStartNotNub = 3, kFbStartOps = 4, kFbStartSnapshot = 5, kFbStartLayout = 6 };
// 0.0.7 (M6 Stage 1a, R1): boot-arg navi48-m6 == 1 (present AND exactly 1) lifts the framebuffers' refusal to start beside navi48-metal-ws: under that latch the GPU desktop composites EVERY display through its own IOPresentment pipe (the
// bring-up kext's routing guard keeps a monitor B frame off the DP). Without it (the default, and any other value) the refusal is exactly aux 0.0.6's. The verdict function below is unchanged: start() passes it
// fb_metal_ws_blocks(), which is "navi48-metal-ws present" with the latch OFF and false with it ON.
constexpr bool m6_on(bool present, uint32_t value) { return present && value == 1u; }
constexpr bool fb_metal_ws_blocks(bool metalWsPresent, bool m6On) { return metalWsPresent && !m6On; }
inline FbStart fb_start_verdict(bool auxOn, bool metalWsPresent, bool providerIsNub, bool layoutOk, uint32_t opsVerdict, int snapRc, uint32_t snapVerdict) {
    if (!auxOn) return kFbStartAuxOff;
    if (metalWsPresent) return kFbStartMetalWs;
    if (!providerIsNub) return kFbStartNotNub;
    if (!layoutOk) return kFbStartLayout;
    if (opsVerdict != N48_DOV_OK) return kFbStartOps;
    if (snapRc != 0 || snapVerdict != N48_DSV_OK) return kFbStartSnapshot;
    return kFbStartOk;
}

// 0.0.6 (ABI 2, the monitor A): ONE class, TWO personalities (Navi48DisplayIndex 1 = the monitor B, 2 = the monitor A). The provider's index property decides which geometry the snapshot MUST carry: the ops table is checked FOR THAT INDEX
// (n48disp_ops_check_for: index 2 needs an ABI-2 table, so an ABI-1 bring-up kext is refused cleanly) and the snapshot must be valid for its own index (n48disp_snap_valid: the per-index geometry table of Navi48DisplayOps.h)
// AND carry exactly the index of the nub it was fetched from. Nothing is hard-wired to the monitor B's resolution here: the mode, the pixel format, the aperture and the EDID all come from the snapshot.
inline uint32_t fb_provider_index(bool haveNumber, uint64_t value) { return (haveNumber && (value == N48_DISP_INDEX || value == N48_DISPA_INDEX)) ? (uint32_t)value : 0u; }
inline uint32_t fb_snap_for_index(const N48DispSnap *s, uint32_t providerIndex) {
    const uint32_t v = n48disp_snap_valid(s);
    if (v != N48_DSV_OK) return v;
    return s->index == providerIndex ? (uint32_t)N48_DSV_OK : (uint32_t)N48_DSV_INDEX;
}

// The one display mode: id 1, depth 0. The display's one mode (the monitor B: its CTA DTD at 59.95 Hz; the monitor A: 1920x1080 @ 60 Hz) comes from the snapshot; maxDepthIndex 0; Valid | Safe | Default.
constexpr uint32_t kFbModeId = 1u;
constexpr uint32_t fb_mode_count() { return 1u; }
constexpr bool fb_mode_ok(uint32_t mode, uint32_t depth) { return mode == kFbModeId && depth == 0u; }
struct FbModeInfo { uint32_t w, h, refresh1616, maxDepthIndex; bool valid, safe, isDefault; };
inline bool fb_mode_info(const N48DispSnap *s, uint32_t mode, FbModeInfo *o) {
    if (!s || !o || mode != kFbModeId) return false;
    o->w = s->w; o->h = s->h; o->refresh1616 = s->refresh1616; o->maxDepthIndex = 0u; o->valid = true; o->safe = true; o->isDefault = true;
    return true;
}
// setDisplayMode accepts only (1, 0); getCurrentDisplayMode / getStartupDisplayMode answer (1, 0) (the base startup getter would return 0xE00002C7).

// The pixel format, matching SURFACE_CONFIG 8 and the kext's 0xFFRRGGBB fill: 32 bpp direct RGB, 3 x 8 bits, masks R 0xFF0000 / G 0xFF00 / B 0xFF, "--------RRRRRRRRGGGGGGGGBBBBBBBB" (the same bytes RDNA4FB's channel map 0 answers).
struct FbPixel { uint32_t bytesPerRow, bitsPerPixel, componentCount, bitsPerComponent, masks[3], w, h; char format[33]; };
inline bool fb_pixel_info(const N48DispSnap *s, uint32_t mode, uint32_t depth, bool systemAperture, FbPixel *o) {
    if (!s || !o || !fb_mode_ok(mode, depth) || !systemAperture) return false;
    o->bytesPerRow = s->pitchBytes; o->bitsPerPixel = 32u; o->componentCount = 3u; o->bitsPerComponent = 8u;
    o->masks[0] = 0x00FF0000u; o->masks[1] = 0x0000FF00u; o->masks[2] = 0x000000FFu;
    o->w = s->w; o->h = s->h;
    static const char fmt[] = "--------RRRRRRRRGGGGGGGGBBBBBBBB";
    for (unsigned i = 0; i < sizeof(fmt); i++) o->format[i] = fmt[i];
    return true;
}
// The aperture: the system aperture (and getVRAMRange) is buffer A as the CPU sees it, 14,745,600 bytes at BAR0 + off[0]; any other aperture has no range.
inline bool fb_aperture(const N48DispSnap *s, bool systemAperture, uint64_t *phys, uint64_t *len) {
    if (!s || !phys || !len || !systemAperture || s->aperPhys == 0u || s->aperLen == 0u) return false;
    *phys = s->aperPhys; *len = s->aperLen;
    return true;
}
// Connection attributes (RDNA4FB framebuffer.cpp:2385-2417): Enable 1, CheckEnable 1, Flags 0, SupportsHLDDCSense = success (no value) when an EDID is cached; anything else goes to super (kFbRcSuper).
constexpr uint32_t fb_fourcc(char a, char b, char c, char d) { return ((uint32_t)(uint8_t)a << 24) | ((uint32_t)(uint8_t)b << 16) | ((uint32_t)(uint8_t)c << 8) | (uint32_t)(uint8_t)d; }
constexpr uint32_t kFbAttrEnable = fb_fourcc('e', 'n', 'a', 'b'), kFbAttrCheckEnable = fb_fourcc('c', 'e', 'n', 'a'), kFbAttrFlags = fb_fourcc('f', 'l', 'g', 's'), kFbAttrHlDdc = fb_fourcc('h', 'd', 'd', 'c');
constexpr uint32_t kFbAttrPower = fb_fourcc('p', 'o', 'w', 'r'), kFbAttrCursor = fb_fourcc('c', 'r', 's', 'r');     // kConnectionPower / kIOPowerAttribute are 'powr'; kIOHardwareCursorAttribute is 'crsr'
enum FbRc : uint32_t { kFbRcSuccess = 0, kFbRcUnsupported = 1, kFbRcSuper = 2 };
struct FbAttr { FbRc rc; bool hasValue; uintptr_t value; };
inline FbAttr fb_connection_attr(const N48DispSnap *s, uint32_t attribute) {
    FbAttr r = { kFbRcSuper, false, 0u };
    if (attribute == kFbAttrEnable || attribute == kFbAttrCheckEnable) { r.rc = kFbRcSuccess; r.hasValue = true; r.value = 1u; }
    else if (attribute == kFbAttrFlags) { r.rc = kFbRcSuccess; r.hasValue = true; r.value = 0u; }
    else if (attribute == kFbAttrHlDdc) r.rc = (s && s->edidLen != 0u) ? kFbRcSuccess : kFbRcUnsupported;
    return r;
}
// getAttribute: the hardware cursor attribute answers 0 (software cursor, no 'vbl ' timer, no hardware plane); everything else is super's.
inline FbAttr fb_attr(uint32_t attribute) {
    FbAttr r = { kFbRcSuper, false, 0u };
    if (attribute == kFbAttrCursor) { r.rc = kFbRcSuccess; r.hasValue = true; r.value = 0u; }
    return r;
}
// setAttribute(kIOPowerAttribute) / setAttributeForConnection(kConnectionPower): record the value and succeed; NO hardware action in M5. Others are super's.
constexpr bool fb_power_attr(uint32_t attribute) { return attribute == kFbAttrPower; }

// DDC: one connection (index 0); blocks are 1-based (1 = base EDID, 2 = the CTA extension) over the cached 256 bytes; only the EDID block type. Same answers as RDNA4FB's.
constexpr uint32_t kFbDdcBlock = 128u;
enum FbDdc : uint32_t { kFbDdcOk = 0, kFbDdcUnsupported = 1, kFbDdcNotFound = 2 };
inline bool fb_has_ddc(const N48DispSnap *s, int32_t connect) { return s && connect == 0 && s->edidLen != 0u; }
inline FbDdc fb_ddc_block(const N48DispSnap *s, int32_t connect, uint32_t blockNumber, bool typeIsEdid, bool haveOut, uint64_t outCap, const uint8_t **src, uint64_t *n) {
    if (!s || connect != 0 || !typeIsEdid || !haveOut || !src || !n) return kFbDdcUnsupported;
    if (blockNumber < 1u || (uint64_t)blockNumber * kFbDdcBlock > s->edidLen) return kFbDdcNotFound;
    *src = s->edid + (uint64_t)(blockNumber - 1u) * kFbDdcBlock;
    *n = outCap < kFbDdcBlock ? outCap : kFbDdcBlock;
    return kFbDdcOk;
}

// ---- task window (only the shape check; the values come from the bring-up kext) ------------------------------------------------------------
constexpr uint64_t kPage = 0x1000ull;
inline bool task_window_ok(uint64_t size, uint64_t reserve) { return size >= 0x100000ull && (size & (kPage - 1)) == 0 && reserve < size && (reserve & (kPage - 1)) == 0; }

} // namespace n48accel
