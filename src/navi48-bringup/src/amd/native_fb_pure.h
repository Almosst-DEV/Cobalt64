//
//  native_fb_pure.h - the pure half of M5 (kext 0.0.652, an internal design note "M5 build spec: Navi48Framebuffer for the monitor B"; 0.0.658: and the monitor A, "monitor A (instance 1) plane + framebuffer
//  spec at 1080p60" section 4): every DECISION of the verb `fbpublish 2` (the monitor B) / `fbpublish 1` (the monitor A) and of the Navi48DisplayNub, judged from arguments alone. No kernel header: tests/native_disp_test.cpp compiles this very file and drives it, with planted breaks
//  (tests/native_disp_plant.sh). The kernel glue is Navi48DisplayNub.cpp; the shared interface (the snapshot, the ops table, the snapshot validator) is Navi48DisplayOps.h.
//
//  The verb publishes ONE software nub, on demand, never at boot. It writes NO register: it reads (the live plane gates, the monitor B's EDID over DDC line 3) and creates an IORegistry
//  object. Its refusals are in two PHASES so that nothing touches the hardware before the cheap, software-only refusals have passed:
//    phase 1 (pre_verdict)  arguments and software state: the argument, the boot-arg latch, an existing nub, the HELD plane, the bound device, an adopted pipe, a WindowServer session, a Metal nub published earlier this boot (0.0.653);
//    phase 2 (post_verdict) the facts read from the hardware and the registry: the live gates, the EDID read / checksums / equality with RDNA4FB's EDID,DDC3, the aperture.
//  Then the snapshot is built (snap_build) and judged by n48disp_snap_valid (Navi48DisplayOps.h) before the nub exists.
//
#pragma once
#include <stdint.h>
#include "../Navi48DisplayOps.h"

namespace n48fb {

// ---- the verb's status (out[0]) ---------------------------------------------------------------------------------------------------------------------------------------------
enum Status : uint32_t {
    kOk = N48_FBP_OK, kBadArg = N48_FBP_BAD_ARG, kOff = N48_FBP_OFF, kNoDevice = N48_FBP_NO_DEVICE, kBusy = N48_FBP_BUSY, kNotHeld = N48_FBP_NOT_HELD, kExists = N48_FBP_EXISTS, kPipeAdopted = N48_FBP_PIPE_ADOPTED,
    kWsOpen = N48_FBP_WS_OPEN, kMetalNub = N48_FBP_METAL_NUB, kGate = N48_FBP_GATE, kEdidRead = N48_FBP_EDID_READ, kEdidBad = N48_FBP_EDID_BAD, kEdidMismatch = N48_FBP_EDID_MISMATCH, kAperture = N48_FBP_APERTURE, kSnapshot = N48_FBP_SNAPSHOT,
    kPublishFailed = N48_FBP_PUBLISH_FAILED, kStatusCount = N48_FBP_COUNT
};
inline const char *status_name(uint32_t s) { return n48disp_fbp_name(s); }     // one text table, in the shared header (the CLI prints the same texts)

// ---- the boot-arg latch navi48-fb2 (same shape as the other latches: present AND == 1) ----------------------------------------------------------------------------------------------
constexpr bool fb2_on(bool present, uint32_t value) { return present && value == 1u; }

// ---- the argument: `fbpublish 2` (the monitor B's instance) or (0.0.658) `fbpublish 1` (the monitor A's instance). Anything else is refused before the latch. ---------------------------------------
// ONE table maps the instance to its display index, OTG and DDC line (Navi48DisplayOps.h n48disp_geom_for_index; spec correction 6): {inst 2 -> index 1, OTG2, DDC line 3}, {inst 1 -> index 2, OTG1, DDC line 2}.
constexpr bool arg_ok(uint64_t a) { return a == 2ull || a == 1ull; }
constexpr uint32_t index_of_arg(uint64_t a) { return a == 2ull ? N48_DISP_INDEX : a == 1ull ? N48_DISPA_INDEX : 0u; }
constexpr uint32_t inst_of_arg(uint64_t a) { return arg_ok(a) ? (uint32_t)a : 0u; }
// RDNA4FB's EDID property of a DDC line ("EDID,DDC3" for the monitor B, "EDID,DDC2" for the monitor A); NULL for any other line
constexpr const char *edid_prop_name(uint32_t ddcLine) { return ddcLine == N48_DISP_DDC_LINE ? "EDID,DDC3" : ddcLine == N48_DISPA_DDC_LINE ? "EDID,DDC2" : nullptr; }

// ---- phase 1: software facts only -------------------------------------------------------------------------------------------------------------------------------------------------
struct Pre {
    bool argOk;          // arg_ok(argument)
    bool latched;        // boot-arg navi48-fb2 == 1, latched
    bool nubExists;      // THE NUB OF THIS INSTANCE'S INDEX is published (0.0.658: one nub per index; the other index's nub is not this verb's business)
    bool held;           // the plane stage OF THIS INSTANCE is HELD (disp2 fbhold <inst> succeeded)
    bool pinned;         // ... and the allocator pinned the pair (n1c_d2_pinned)
    bool devOk;          // the display layer is bound and armed
    bool pipeAdopted;    // pipeadopt succeeded this boot
    bool wsOpen;         // a WindowServer native session is open (n1c_ws_is_open)
    bool metalNub;       // 0.0.653: the Metal nub was published earlier this boot (n48metal_nub_published; sticky: withdrawn or not)
    bool m6 = false;     // 0.0.659 (M6 Stage 1a, R2): boot-arg navi48-m6 == 1 (latched). With it the three interlocks below (adopted pipe, WindowServer session, Metal nub) are LIFTED: the routing guard (native_m6_pure.h) replaces them
};
constexpr uint32_t pre_verdict(const Pre &p) {
    if (!p.argOk) return kBadArg;
    if (!p.latched) return kOff;
    if (p.nubExists) return kExists;
    if (!p.held || !p.pinned) return kNotHeld;
    if (!p.devOk) return kNoDevice;
    if (p.m6) return kOk;                            // 0.0.659 (R2): under navi48-m6 the order is the operator's (fbpublish before the nub publish and pipeadopt is the recipe; a later publish is caught by adopt's pipe-count check, which fails closed)
    if (p.pipeAdopted) return kPipeAdopted;
    if (p.wsOpen) return kWsOpen;
    if (p.metalNub) return kMetalNub;                // 0.0.653 (M5 review): the mirror of the Metal publish's dispNub interlock
    return kOk;
}

// ---- the aperture: buffer A as the CPU sees it -------------------------------------------------------------------------------------------------------------------------------------
// bar0Phys + offA, N48_DISP_BYTES long: the offset 64 KiB aligned (the allocator's alignment, and the register granularity), the whole buffer inside BAR0, no wrap, the base non-zero.
constexpr bool aperture_ok_len(uint64_t bar0Phys, uint64_t bar0Size, uint64_t offA, uint64_t len) {
    return bar0Phys != 0u && (bar0Phys & 0xFFFFull) == 0u && (offA & 0xFFFFull) == 0u && offA <= bar0Size && len <= bar0Size - offA && bar0Phys + offA >= bar0Phys;
}
constexpr bool aperture_ok(uint64_t bar0Phys, uint64_t bar0Size, uint64_t offA) {      // the monitor B's 14,745,600 bytes
    return aperture_ok_len(bar0Phys, bar0Size, offA, N48_DISP_BYTES);
}
// 0.0.658: the frame (the aperture's length) must lie inside the plane's own allocation (the monitor B: 225 x 64 KiB = 14,745,600 = the frame; the monitor A: 127 x 64 KiB = 8,323,072 >= the 8,294,400-byte frame), whose size is a whole number of 64 KiB blocks.
constexpr bool len_in_alloc(uint64_t len, uint64_t allocBytes) { return len != 0u && allocBytes != 0u && (allocBytes & 0xFFFFull) == 0u && len <= allocBytes; }

// ---- phase 2: the facts read from the hardware and the registry --------------------------------------------------------------------------------------------------------------------
// Two steps so that the DDC engine is not touched when the live gates already fail: live_verdict (the plane's live gates), then data_verdict (the EDID read and the registry / aperture facts).
constexpr uint32_t live_verdict(uint32_t holdBad) { return holdBad != 0u ? (uint32_t)kGate : (uint32_t)kOk; }
struct Data {
    uint32_t edidStatus;     // the N48DR_* status of the two DDC reads (0 = both OK)
    bool     block0Sum, block0Hdr, block1Sum;     // the checksums / header of the bytes read
    bool     propPresent;    // RDNA4FB's "EDID,DDC<line>" property exists and is 128 bytes
    bool     block0EqProp;   // block 0 equals it byte for byte
    bool     apertureOk;     // aperture_ok(...)
};
constexpr uint32_t data_verdict(const Data &p) {
    if (p.edidStatus != 0u) return kEdidRead;
    if (!p.block0Sum || !p.block0Hdr || !p.block1Sum) return kEdidBad;
    if (!p.propPresent || !p.block0EqProp) return kEdidMismatch;
    if (!p.apertureOk) return kAperture;
    return kOk;
}
struct Post { uint32_t holdBad; Data d; };
constexpr uint32_t post_verdict(const Post &p) { return live_verdict(p.holdBad) != kOk ? live_verdict(p.holdBad) : data_verdict(p.d); }

// ---- the snapshot ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
// bar0Phys + offA is the aperture; mcA / mcB the MC addresses of the pair; the mode constants are the monitor B's (Navi48DisplayOps.h). The result is judged by n48disp_snap_valid before use.
inline void snap_build_idx(N48DispSnap *s, uint32_t index, uint64_t bar0Phys, uint64_t offA, uint64_t mcA, uint64_t mcB, const uint8_t edid[256]) {
    uint8_t *z = (uint8_t *)s;
    for (unsigned i = 0; i < sizeof(*s); i++) z[i] = 0;
    N48DispGeom g;
    if (!n48disp_geom_for_index(index, &g)) return;        // an unknown index leaves the all-zero snapshot, which n48disp_snap_valid refuses (N48_DSV_INDEX)
    s->index = g.index; s->otg = g.otg; s->ddcLine = g.ddcLine;
    s->w = g.w; s->h = g.h; s->pitchBytes = g.pitchBytes; s->fmt = g.fmt; s->refresh1616 = g.refresh1616;
    s->pixHz = g.pixHz;
    s->aperPhys = bar0Phys + offA; s->aperLen = g.aperLen;
    s->mcA = mcA; s->mcB = mcB;
    s->edidLen = N48_DISP_EDID_BYTES;
    for (unsigned i = 0; i < N48_DISP_EDID_BYTES; i++) s->edid[i] = edid[i];
}
inline void snap_build(N48DispSnap *s, uint64_t bar0Phys, uint64_t offA, uint64_t mcA, uint64_t mcB, const uint8_t edid[256]) {      // the monitor B's (index 1), exactly as 0.0.652..657
    snap_build_idx(s, N48_DISP_INDEX, bar0Phys, offA, mcA, mcB, edid);
}

// ---- the ops table the nub serves -------------------------------------------------------------------------------------------------------------------------------------------------------
// The snapshot hook copies only for A nub of ours (identity: one nub per display index) and only while a valid snapshot is published FOR THAT NUB. 0 = copied.
constexpr int snapshot_hook_verdict(bool isOurNub, bool havePublished, bool outNonNull) { return (isOurNub && havePublished && outNonNull) ? 0 : -1; }

// ---- publication state: once PER INDEX, never withdrawn in M5 ----------------------------------------------------------------------------------------------------------------------------------------
constexpr bool may_publish(bool nubExists) { return !nubExists; }

}  // namespace n48fb
