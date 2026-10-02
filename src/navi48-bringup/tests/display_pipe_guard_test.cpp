// display_pipe_guard_test.cpp — the 0.0.286 display-pipe safety core's pure parts.
//     clang++ -std=c++17 -Wall -Wextra -O1 -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/display_pipe_guard_test.cpp -o /tmp/dpgtest && /tmp/dpgtest
// Covers: the WRITE_DATA classifier and its same-length NOP over Apple's own packet shape (writeWriteData1RegCmdPacket
// @0xbe442b8), the DCN windows against every GC 12 register span, the slot-table check with a slide, and the AGDC reply
// layouts against AMD's getVendorInfo / getGpuCapability offsets.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "display_pipe_guard.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-72s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-72s %#llx\n", what, (unsigned long long)got);
}

static uint32_t rd32(const uint8_t *p, unsigned off) { uint32_t v; std::memcpy(&v, p + off, 4); return v; }
static uint64_t rd64(const uint8_t *p, unsigned off) { uint64_t v; std::memcpy(&v, p + off, 8); return v; }

int main()
{
    // Apple's own packet (bytes at be442bc-be442d9): 0xC0033700, 0x00010000, reg, 0, value.
    const uint32_t hdr = 0xC0033700u;
    uint32_t body[4] = { 0x00010000u, 0x34c0u + 0x1234u, 0u, 0xdeadbeefu };
    uint32_t reg = 0;
    expect_u("WRITE_DATA to DCN base 2 + 0x1234 -> REG_DISPLAY", (uint64_t)n48_dpg_classify(hdr, body, 4, &reg), N48_DPG_WD_REG_DISPLAY);
    expect_u("  its register", reg, 0x34c0u + 0x1234u);
    expect_u("  NOP header keeps type and count, op 0x10", n48_dpg_nop_for(hdr), 0xC0031000u);
    expect_u("  NOP spans the same dwords (count field)", (uint64_t)((n48_dpg_nop_for(hdr) >> 16) & 0x3FFFu), 3u);
    body[1] = 0x1260u + 0x1de2u;   // a GC register (CP_RB0_* neighbourhood)
    expect_u("WRITE_DATA to GC base 0 -> REG_OK", (uint64_t)n48_dpg_classify(hdr, body, 4, &reg), N48_DPG_WD_REG_OK);
    body[1] = 0xa000u + 0x20c0u;
    expect_u("WRITE_DATA to GC base 1 -> REG_OK", (uint64_t)n48_dpg_classify(hdr, body, 4, &reg), N48_DPG_WD_REG_OK);
    body[0] = 0x00010005u; body[1] = 0x34d0u;   // DST_SEL 5 = memory: never a register
    expect_u("WRITE_DATA to memory (DST_SEL 5) at a DCN-looking address -> OTHER", (uint64_t)n48_dpg_classify(hdr, body, 4, &reg), N48_DPG_WD_OTHER);
    body[0] = 0x00010000u;
    expect_u("short body -> SHORT", (uint64_t)n48_dpg_classify(hdr, body, 1, &reg), N48_DPG_WD_SHORT);
    expect_u("an INDIRECT_BUFFER header -> NOT_WD", (uint64_t)n48_dpg_classify(0xC0023F00u, body, 4, &reg), N48_DPG_NOT_WD);
    expect_u("a NOP header -> no NOP for it (refuse 0)", n48_dpg_nop_for(0xC0031000u), 0u);
    expect_u("a type-2 dword -> no NOP for it", n48_dpg_nop_for(0x80000000u), 0u);

    // Windows: edges.
    expect_u("0x11 not display", (uint64_t)n48_dpg_is_display_reg(0x11u), 0u);
    expect_u("0x12 display (DMU base 0)", (uint64_t)n48_dpg_is_display_reg(0x12u), 1u);
    expect_u("0x5f display", (uint64_t)n48_dpg_is_display_reg(0x5fu), 1u);
    expect_u("0x100 display (DMU base 1 + 0x40)", (uint64_t)n48_dpg_is_display_reg(0x100u), 1u);
    expect_u("0x34c0 + 0x3966 display (highest base-2 register)", (uint64_t)n48_dpg_is_display_reg(0x34c0u + 0x3966u), 1u);
    expect_u("0x9000 display (DMU base 3)", (uint64_t)n48_dpg_is_display_reg(0x9000u), 1u);
    expect_u("0x9fff display", (uint64_t)n48_dpg_is_display_reg(0x9fffu), 1u);

    // No GC 12 register (gc_12_0_0_offset.h spans) is ever in a window: sweep both GC spans completely.
    uint64_t gcHits = 0, gcCount = 0;
    for (uint32_t d = 0x1260u; d <= 0x1260u + 0x214du; d++, gcCount++) gcHits += (uint64_t)n48_dpg_is_display_reg(d);
    for (uint32_t d = 0xa000u; d <= 0xa000u + 0x5f91u; d++, gcCount++) gcHits += (uint64_t)n48_dpg_is_display_reg(d);
    expect_u("GC 12 base-0 and base-1 spans swept (dwords)", gcCount, (0x214du + 1u) + (0x5f91u + 1u));
    expect_u("  of which inside a DCN window", gcHits, 0u);
    // Every DCN 4.1 BASE_IDX 0-2 span is fully inside a window (dcn_4_1_0_offset.h min/max per base).
    uint64_t dcnMiss = 0;
    for (uint32_t d = 0x12u + 0x0u; d <= 0x12u + 0x4du; d++) dcnMiss += (uint64_t)!n48_dpg_is_display_reg(d);
    for (uint32_t d = 0xc0u + 0x40u; d <= 0xc0u + 0x799u; d++) dcnMiss += (uint64_t)!n48_dpg_is_display_reg(d);
    for (uint32_t d = 0x34c0u + 0x4au; d <= 0x34c0u + 0x3966u; d++) dcnMiss += (uint64_t)!n48_dpg_is_display_reg(d);
    expect_u("DCN 4.1 base 0-2 dwords outside every window", dcnMiss, 0u);

    // Slot check with a slide; a positive control and an inverted one.
    uint64_t vt[N48_DPG_PIPE_SLOTS] = { 0 };
    const uint64_t slide = 0x12345000ull;
    for (unsigned i = 0; i < N48_DPG_N_PIPE_GUARD; i++) vt[kN48PipeGuardSlots[i].slot] = kN48PipeGuardSlots[i].target + slide;
    expect_u("pipe slots all match (positive control)", n48_dpg_check_slots(vt, slide, kN48PipeGuardSlots, N48_DPG_N_PIPE_GUARD), 0u);
    vt[276] += 0x14;   // the reviewer's 0xbdccffa is mid-function; the vtable says 0xbdccfe6
    expect_u("validateTransaction off by 0x14 -> index 8 (1-based) refused", n48_dpg_check_slots(vt, slide, kN48PipeGuardSlots, N48_DPG_N_PIPE_GUARD), 8u);
    expect_u("  wrong slide refuses at the first slot", n48_dpg_check_slots(vt, slide + 0x1000, kN48PipeGuardSlots, N48_DPG_N_PIPE_GUARD), 1u);
    uint64_t dvt[N48_DPG_DISP_SLOTS] = { 0 };
    for (unsigned i = 0; i < N48_DPG_N_DISP_GUARD; i++) dvt[kN48DispGuardSlots[i].slot] = kN48DispGuardSlots[i].target + slide;
    expect_u("display slots match (positive control)", n48_dpg_check_slots(dvt, slide, kN48DispGuardSlots, N48_DPG_N_DISP_GUARD), 0u);
    expect_u("slot 0x198 / 8 is the flip slot index", (uint64_t)kN48DispGuardSlots[2].slot, 0x198u / 8u);
    expect_u("pipe slot 267 is byte 0x858", (uint64_t)kN48PipeGuardSlots[3].slot * 8u, 0x858u);

    // The AMD event-machine census slot table (accel+0x380, 0.0.289). The called slots resolve into IAF2 addresses under
    // the SAME shared slide the safety core proves; the identity anchors 0/7 are X6000 addresses under that same slide.
    expect_u("event-machine census has 9 slots", (uint64_t)N48_DPG_N_EM_GUARD, 9u);
    expect_u("event-machine vtable is 95 slots", (uint64_t)N48_DPG_EM_SLOTS, 95u);
    expect_u("census wraps from index 2 (0/7 are identity anchors)", (uint64_t)N48_DPG_EM_WRAP_FIRST, 2u);
    uint64_t evt[N48_DPG_EM_SLOTS] = { 0 };
    for (unsigned i = 0; i < N48_DPG_N_EM_GUARD; i++) evt[kN48EmGuardSlots[i].slot] = kN48EmGuardSlots[i].target + slide;
    expect_u("event-machine slots all match (positive control)", n48_dpg_check_slots(evt, slide, kN48EmGuardSlots, N48_DPG_N_EM_GUARD), 0u);
    expect_u("  copyEvent is byte 0x1b0 (slot 54)", (uint64_t)kN48EmGuardSlots[5].slot * 8u, 0x1b0u);
    expect_u("  testEventUnlocked is byte 0x180 (slot 48)", (uint64_t)kN48EmGuardSlots[4].slot * 8u, 0x180u);
    evt[54] += 0x8;   // copyEvent tampered
    expect_u("copyEvent off by 8 -> index 6 (1-based) refused", n48_dpg_check_slots(evt, slide, kN48EmGuardSlots, N48_DPG_N_EM_GUARD), 6u);
    expect_u("  wrong slide refuses at the first event-machine slot", n48_dpg_check_slots(evt, slide + 0x1000, kN48EmGuardSlots, N48_DPG_N_EM_GUARD), 1u);

    // AGDC vendor info, against getVendorInfo @0xbf7f79e.
    uint8_t vi[N48_AGDC_VENDOR_INFO_LEN];
    expect_u("vendor info fills at 0x2c", (uint64_t)n48_agdc_fill_vendor_info(vi, sizeof vi), 0u);
    expect_u("  +0x00 = 0x30000", rd32(vi, 0x00), 0x30000u);
    expect_u("  +0x04 = \"AMD\"", rd64(vi, 0x04), 0x444d41u);
    expect_u("  +0x24 vendor 0x1002", rd32(vi, 0x24), 0x1002u);
    expect_u("  +0x28 type 2 (IOPresentment wants 1 or 2, 0x7ff81598fe57)", rd32(vi, 0x28), 2u);
    expect_u("  wrong length refused", (uint64_t)n48_agdc_fill_vendor_info(vi, 0x2b), 1u);

    // AGDC GPU capability, against getGpuCapability's cache at this+0x140.
    uint8_t cap[N48_AGDC_GPU_CAP_LEN];
    const uint64_t fbs[1] = { 0xffffff8012345678ull };
    expect_u("capability fills at 0xdc", (uint64_t)n48_agdc_fill_gpu_capability(cap, sizeof cap, 0xffffff80abcdef00ull, fbs, 1), 0u);
    expect_u("  +0x00 mask 2 << 0", rd64(cap, 0x00), 2u);
    expect_u("  +0x20 count", rd32(cap, 0x20), 1u);
    expect_u("  +0x30 count (IOPresentment requires non-zero, 0x7ff815990853)", rd32(cap, 0x30), 1u);
    expect_u("  +0x34 PCI device pointer", rd64(cap, 0x34), 0xffffff80abcdef00ull);
    expect_u("  +0x3c framebuffer[0] pointer", rd64(cap, 0x3c), 0xffffff8012345678ull);
    expect_u("  +0x44 framebuffer[1] zero", rd64(cap, 0x44), 0u);
    expect_u("  zero framebuffers refused", (uint64_t)n48_agdc_fill_gpu_capability(cap, sizeof cap, 1, fbs, 0), 1u);
    expect_u("  eight framebuffers refused (AMD's cap is 7, bf7fb8f)", (uint64_t)n48_agdc_fill_gpu_capability(cap, sizeof cap, 1, fbs, 8), 1u);
    expect_u("  wrong length refused", (uint64_t)n48_agdc_fill_gpu_capability(cap, 0xd8, 1, fbs, 1), 1u);

    // AGDC getLinkConfig (selector 0x921), AGDCLinkConfig_t 0xb0 — clears routeA4's 0x80e (M4-AGDC-CAPABILITIES).
    expect_u("link config length is 0xb0", (uint64_t)N48_AGDC_LINK_CONFIG_LEN, 0xb0u);
    expect_u("link config selector is 0x921", (uint64_t)N48_AGDC_CMD_LINK_CONFIG, 0x921u);
    uint8_t lc[N48_AGDC_LINK_CONFIG_LEN];
    std::memset(lc, 0xa5, sizeof lc);
    /* 0.0.337: these five assertions were STALE — they still demanded the all-zero reply that 0.0.336 was written
       to abolish, and the file did not compile at all because the helper grew its w/h arguments. The contract now
       under test is's: the active size is filled (that is the whole fix) and the pixel clock is DERIVED, never
       zero, because RefreshTime/Min/Max are one field computed from it. */
    expect_u("link config fills at 0xb0", (uint64_t)n48_agdc_fill_link_config(lc, sizeof lc, 1920u, 1080u), 0u);
    expect_u("  +0x08 bit0 = 0 (no extra link-caps gather, 0x7ff81599cc6f)", (uint64_t)(lc[0x08] & 1u), 0u);
    expect_u("  +0x44 HorizontalActive = 1920 (GetEndpointSize's width)", (uint64_t)rd32(lc, 0x44), 1920u);
    expect_u("  +0x54 VerticalActive   = 1080 (GetEndpointSize's height)", (uint64_t)rd32(lc, 0x54), 1080u);
    expect_u("  +0x2c PixelClock = 2200*1125*60 (CEA-861 1080p60, derived)", rd64(lc, 0x2c), 148500000u);
    expect_u("  +0xa4 stream address still zeroed", rd64(lc, 0xa4), 0u);
    expect_u("  a zero mode is CLAMPED to 1920x1080, never left zero",
             (uint64_t)(n48_agdc_fill_link_config(lc, sizeof lc, 0u, 0u) == 0 && rd32(lc, 0x44) == 1920u &&
                        rd32(lc, 0x54) == 1080u), 1u);
    expect_u("  wrong length refused", (uint64_t)n48_agdc_fill_link_config(lc, 0xaf, 1920u, 1080u), 1u);
    expect_u("  null buffer refused", (uint64_t)n48_agdc_fill_link_config(nullptr, 0xb0, 1920u, 1080u), 1u);

    // AGDC display-pipeline/scaler caps (selector 0x711), 0x196c — the 0x2006 fix (M4-AGDC-CAPABILITIES follow-up).
    expect_u("pipeline caps length is 0x196c", (uint64_t)N48_AGDC_PIPELINE_CAPS_LEN, 0x196cu);
    expect_u("pipeline caps selector is 0x711", (uint64_t)N48_AGDC_CMD_PIPELINE_CAPS, 0x711u);
    expect_u("scaler type is 0x10", (uint64_t)N48_AGDC_SCALER_TYPE, 0x10u);
    static uint8_t pc[N48_AGDC_PIPELINE_CAPS_LEN];
    // A non-scaler type (buffer full of 0x5a, so reply+0x04 low32 != 0x10): zero-filled, entry count stays 0.
    std::memset(pc, 0x5a, sizeof pc);
    expect_u("pipeline caps: non-scaler type fills at 0x196c", (uint64_t)n48_agdc_fill_pipeline_caps(pc, sizeof pc, 1920u, 1080u), 0u);
    expect_u("  first dword zeroed", rd32(pc, 0x00), 0u);
    expect_u("  non-scaler: entry count +0x08 stays 0 (would yield 0x2006)", rd32(pc, 0x08), 0u);
    expect_u("  last dword zeroed", rd32(pc, N48_AGDC_PIPELINE_CAPS_LEN - 4u), 0u);
    // The gating type-0x10 (Plane Scaler) query: IOPresentment prewrites reply+0x04 = u64 0x800000010 (bytes 10 00 00 00
    // 08 00 00 00), reads reply+0x08 after our fill. Model that prewrite and confirm we overwrite +0x08 with 1.
    std::memset(pc, 0x5a, sizeof pc);
    pc[0x04] = 0x10; pc[0x05] = 0x00; pc[0x06] = 0x00; pc[0x07] = 0x00;
    pc[0x08] = 0x08; pc[0x09] = 0x00; pc[0x0a] = 0x00; pc[0x0b] = 0x00;   // the caller's prewrite at reply+0x08 = 8
    expect_u("pipeline caps: type 0x10 fills at 0x196c", (uint64_t)n48_agdc_fill_pipeline_caps(pc, sizeof pc, 1920u, 1080u), 0u);
    expect_u("  entry count +0x08 = 1 (clears 0x2006, 0x7ff81599cde7)", rd32(pc, 0x08), 1u);
    expect_u("  +0x0c scaler type/format 0x00080004", rd32(pc, 0x0c), 0x00080004u);
    expect_u("  +0x28 count-ish = 1", rd32(pc, 0x28), 1u);
    expect_u("  +0x30 width 1920", rd32(pc, 0x30), 1920u);
    expect_u("  +0x34 height 1080", rd32(pc, 0x34), 1080u);
    expect_u("  +0x38 width 1920", rd32(pc, 0x38), 1920u);
    expect_u("  +0x3c height 1080", rd32(pc, 0x3c), 1080u);
    expect_u("  +0x40 marker 0x00050005", rd32(pc, 0x40), 0x00050005u);
    expect_u("  +0x44 marker 4", rd32(pc, 0x44), 4u);
    expect_u("  +0x1b0 count-ish = 1", rd32(pc, 0x1b0), 1u);
    expect_u("  +0x1b8 width 1920", rd32(pc, 0x1b8), 1920u);
    expect_u("  +0x1bc height 1080", rd32(pc, 0x1bc), 1080u);
    expect_u("  +0x1c0 width 1920", rd32(pc, 0x1c0), 1920u);
    expect_u("  +0x1c4 height 1080", rd32(pc, 0x1c4), 1080u);
    expect_u("  +0x1c8 marker 0x00050005", rd32(pc, 0x1c8), 0x00050005u);
    expect_u("  +0x1cc marker 4", rd32(pc, 0x1cc), 4u);
    expect_u("  a reserved dword past the entry stays 0 (+0x400)", rd32(pc, 0x400), 0u);
    expect_u("  the input type flag reply+0x04 was overwritten to 0 (buffer zeroed)", rd32(pc, 0x04), 0u);
    // width/height come from the args (the live mode), not a constant: type 0x10 at a different geometry.
    std::memset(pc, 0x00, sizeof pc);
    pc[0x04] = 0x10;
    expect_u("pipeline caps: type 0x10 at 1280x720 fills", (uint64_t)n48_agdc_fill_pipeline_caps(pc, sizeof pc, 1280u, 720u), 0u);
    expect_u("  +0x30 width follows args = 1280", rd32(pc, 0x30), 1280u);
    expect_u("  +0x34 height follows args = 720", rd32(pc, 0x34), 720u);
    expect_u("  +0x08 count = 1 regardless of geometry", rd32(pc, 0x08), 1u);
    expect_u("  wrong length refused", (uint64_t)n48_agdc_fill_pipeline_caps(pc, 0x196b, 1920u, 1080u), 1u);
    expect_u("  null buffer refused", (uint64_t)n48_agdc_fill_pipeline_caps(nullptr, 0x196c, 1920u, 1080u), 1u);

    // Route A (0.0.295): the kext object slot 267 returns, its vtable slot indices, and the resource slot-46 guard.
    // Compile-time asserts on the layout the reviewer's spec requires (offsets 0xc/0xd/0x38/0x40, slots 5/43, prepare slot 46).
    static_assert(N48_RA_OBJ_SIZE >= 0x48u, "object >= 0x48");
    static_assert(N48_RA_VT_SLOTS >= 44u, "vtable >= 44 slots");
    static_assert(N48_RA_SLOT_RELEASE * 8u == 0x28u, "release slot 5 = *0x28");
    static_assert(N48_RA_SLOT_PHYSSEG * 8u == 0x158u, "getPhysicalSegment slot 43 = *0x158");
    static_assert(N48_RA_PREPARE_SLOT * 8u == 0x170u, "prepare slot 46 = *0x170");
    static_assert(N48_RA_OFF_FLAGC == 0x0cu, "flag C at +0xc");
    static_assert(N48_RA_OFF_FLAGD == 0x0du, "flag D at +0xd");
    static_assert(N48_RA_OFF_F38 == 0x38u, "field at +0x38");
    static_assert(N48_RA_OFF_LEN == 0x40u, "length at +0x40");
    expect_u("route A: release is slot 5 (byte 0x28)", (uint64_t)N48_RA_SLOT_RELEASE * 8u, 0x28u);
    expect_u("route A: getPhysicalSegment is slot 43 (byte 0x158)", (uint64_t)N48_RA_SLOT_PHYSSEG * 8u, 0x158u);
    expect_u("route A: prepare is slot 46 (byte 0x170)", (uint64_t)N48_RA_PREPARE_SLOT * 8u, 0x170u);
    expect_u("route A: object >= 0x48 bytes", (uint64_t)(N48_RA_OBJ_SIZE >= 0x48u), 1u);
    expect_u("route A: vtable >= 44 slots", (uint64_t)(N48_RA_VT_SLOTS >= 44u), 1u);
    expect_u("route A: resource vtable is 111 slots", (uint64_t)N48_RA_RES_SLOTS, 111u);
    expect_u("route A: AMDGFX10Resource vptr is 0xbf26608 + 0x10", (uint64_t)N48_RA_RES_VPTR, 0x0bf26618u);
    expect_u("route A: prepare target 0xbdd6198", (uint64_t)N48_RA_PREPARE, 0x0bdd6198u);

    // Build the object into a zeroed buffer; the fields must be exactly what init/destroy/shim require.
    uint8_t obj[N48_RA_OBJ_SIZE];
    std::memset(obj, 0xee, sizeof obj);                    // poison, then build
    std::memset(obj, 0, sizeof obj);
    n48_ra_fill_object(obj, N48_RA_LEN_DEFAULT);
    expect_u("route A: +0xc is 0 (init will orb $0x20)", obj[N48_RA_OFF_FLAGC], 0u);
    expect_u("route A: +0xd is 0x10 (prune-skip bit set)", obj[N48_RA_OFF_FLAGD], 0x10u);
    expect_u("route A: +0x38 is 0 (destroy writes 0, writable)", rd64(obj, N48_RA_OFF_F38), 0u);
    expect_u("route A: +0x40 length = 0x800000", rd64(obj, N48_RA_OFF_LEN), N48_RA_LEN_DEFAULT);
    expect_u("route A: object_check passes (positive control)", (uint64_t)n48_ra_object_check(obj, N48_RA_LEN_DEFAULT), 0u);

    // Mutants: each corrupted field must be caught.
    obj[N48_RA_OFF_FLAGD] = 0;                             // clear the prune-skip bit -> destroy would prune our object
    expect_u("route A MUTANT: +0xd cleared -> check code 1", (uint64_t)n48_ra_object_check(obj, N48_RA_LEN_DEFAULT), 1u);
    obj[N48_RA_OFF_FLAGD] = 0x10;
    obj[N48_RA_OFF_LEN] ^= 0x01;                           // wrong length
    expect_u("route A MUTANT: +0x40 length tampered -> check code 2", (uint64_t)n48_ra_object_check(obj, N48_RA_LEN_DEFAULT), 2u);
    obj[N48_RA_OFF_LEN] ^= 0x01;
    expect_u("route A: object_check passes again after repair", (uint64_t)n48_ra_object_check(obj, N48_RA_LEN_DEFAULT), 0u);
    n48_ra_fill_object(obj, 0x1000u);                      // a different length must round-trip
    expect_u("route A: object_check at len 0x1000", (uint64_t)n48_ra_object_check(obj, 0x1000u), 0u);
    expect_u("route A MUTANT: object built for 0x1000 fails check for 0x800000", (uint64_t)n48_ra_object_check(obj, N48_RA_LEN_DEFAULT), 2u);

    // getPhysicalSegment math: returns phys, sets span = len.
    uint64_t span = 0;
    expect_u("route A: physseg returns phys 0xC0000000", n48_ra_physseg(N48_RA_PHYS_DEFAULT, N48_RA_LEN_DEFAULT, &span), N48_RA_PHYS_DEFAULT);
    expect_u("route A: physseg span = len (>= object length, shim accepts)", span, N48_RA_LEN_DEFAULT);
    expect_u("route A: physseg tolerates a null span", n48_ra_physseg(N48_RA_PHYS_DEFAULT, 0, nullptr), N48_RA_PHYS_DEFAULT);

    // ---- the command-queue probe classifier (0.0.325, an earlier analysis) ----
    expect_u("cq: (0,0) -> IDLE",                      (uint64_t)n48_cq_state(0,0), (uint64_t)N48_CQ_IDLE);
    expect_u("cq: (1,0) -> INSIDE (not past the enable check)", (uint64_t)n48_cq_state(1,0), (uint64_t)N48_CQ_INSIDE);
    expect_u("cq: (1,1) -> INSIDE_PAST_ENABLE",         (uint64_t)n48_cq_state(1,1), (uint64_t)N48_CQ_INSIDE_PAST_ENABLE);
    // MUTANT: the exit clears both with ONE 16-bit store, so (0,1) cannot occur. It must be REPORTED,
    // never silently read as IDLE - an instrument that rounds this to IDLE would hide a torn read.
    expect_u("cq MUTANT: (0,1) is impossible -> INCONSISTENT, not IDLE", (uint64_t)n48_cq_state(0,1), (uint64_t)N48_CQ_INCONSISTENT);
    // MUTANT: any non-zero byte counts as set (the kernel writes 1, but a torn/stale read may differ).
    expect_u("cq: a non-1 truthy flag still reads as INSIDE", (uint64_t)n48_cq_state(0x80,0), (uint64_t)N48_CQ_INSIDE);
    // The walk bound must refuse an empty or implausible count rather than spin the kernel on a bad list.
    expect_u("cq: walk bound 0 for an empty list",      (uint64_t)n48_cq_walk_bound(0), 0u);
    expect_u("cq: walk bound 3 for a plausible count",  (uint64_t)n48_cq_walk_bound(3), 3u);
    expect_u("cq: walk bound at the cap",               (uint64_t)n48_cq_walk_bound(N48_CQ_MAX_WALK), (uint64_t)N48_CQ_MAX_WALK);
    expect_u("cq MUTANT: an implausible count walks NOTHING", (uint64_t)n48_cq_walk_bound(N48_CQ_MAX_WALK + 1u), 0u);
    expect_u("cq MUTANT: a garbage count walks NOTHING", (uint64_t)n48_cq_walk_bound(0xfffffffful), 0u);

    // ---- the inline-buffer 0x711 reply (0.0.324, an earlier analysis): the count and the two tested fields ----
    {
        static uint8_t ib[N48_AGDC_PIPELINE_CAPS_LEN];
        for (size_t i = 0; i < sizeof ib; i++) ib[i] = 0;
        n48_le32(ib, 0x04u, N48_AGDC_IB_TYPE);                   // the type flag the gather writes in
        expect_u("IB: fill accepts the 0x40 type", (uint64_t)n48_agdc_fill_pipeline_caps(ib, sizeof ib, 1920u, 1080u), 0u);
        expect_u("IB: entry count non-zero (clears the builder's bail)", rd32(ib, 0x08u), 1u);
        expect_u("IB: MaxHeight at +0x18 == 1080 (tested non-zero)", rd32(ib, 0x18u), 1080u);
        expect_u("IB: MaxWidth  at +0x1c == 1920 (tested non-zero)", rd32(ib, 0x1cu), 1920u);
        expect_u("IB: PixelFormats at +0x20 non-zero", rd64(ib, 0x20u), 1u);
        expect_u("IB: latency at +0x10 is 1.0f as bits", rd32(ib, 0x10u), (uint64_t)N48_AGDC_IB_LATENCY);
        // MUTANT: a zero mode would write zero width/height and the builder would skip the entry.
        for (size_t i = 0; i < sizeof ib; i++) ib[i] = 0;
        n48_le32(ib, 0x04u, N48_AGDC_IB_TYPE);
        (void)n48_agdc_fill_pipeline_caps(ib, sizeof ib, 0u, 0u);
        expect_u("IB MUTANT: a zero mode leaves MaxHeight 0 - the builder would SKIP this entry", rd32(ib, 0x18u), 0u);
        // The scaler type must be untouched by the new branch.
        for (size_t i = 0; i < sizeof ib; i++) ib[i] = 0;
        n48_le32(ib, 0x04u, N48_AGDC_SCALER_TYPE);
        (void)n48_agdc_fill_pipeline_caps(ib, sizeof ib, 1920u, 1080u);
        expect_u("IB: the 0x10 scaler branch still writes its own count at +0x08", rd32(ib, 0x08u), 1u);
        expect_u("IB: the 0x10 branch does NOT write the IB PixelFormats slot", rd64(ib, 0x20u), 0u);
        // An UNHANDLED type must still be a pure zero fill.
        for (size_t i = 0; i < sizeof ib; i++) ib[i] = 0;
        n48_le32(ib, 0x04u, 0x8000u);
        (void)n48_agdc_fill_pipeline_caps(ib, sizeof ib, 1920u, 1080u);
        expect_u("IB: an unhandled type (0x8000 Cursor) stays a zero fill", rd32(ib, 0x08u), 0u);
    }

    // ---- the AGDC hold decision (0.0.322): it must FAIL CLOSED on anything but a real WindowServer ----
    const char ws[20]   = "WindowServer";
    const char kern[20] = "kernel_task";
    const char longer[20] = "WindowServerX";
    const char empty[20] = "";
    const uint32_t MS = 1500u;
    expect_u("hold: positive control - armed, unfired, 0x921, WindowServer -> 1500",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, ws, sizeof ws), MS);
    // Every fail-closed case. agdc_vendor also runs in IOKit matching context, where a stall holds device matching.
    expect_u("hold MUTANT: disarmed (0 ms) never holds",
             n48_agdc_hold_ms(0u, 0u, N48_AGDC_CMD_LINK_CONFIG, ws, sizeof ws), 0u);
    expect_u("hold MUTANT: absurd duration is refused, not clamped",
             n48_agdc_hold_ms(N48_AGDC_HOLD_MAX_MS + 1u, 0u, N48_AGDC_CMD_LINK_CONFIG, ws, sizeof ws), 0u);
    expect_u("hold MUTANT: wrong selector (0x711) never holds",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_PIPELINE_CAPS, ws, sizeof ws), 0u);
    expect_u("hold MUTANT: kernel_task (AGDC::start matching context) never holds",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, kern, sizeof kern), 0u);
    expect_u("hold MUTANT: empty name (unidentified caller) never holds",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, empty, sizeof empty), 0u);
    expect_u("hold MUTANT: NULL name never holds",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, nullptr, 20u), 0u);
    expect_u("hold MUTANT: zero-capacity name buffer never holds",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, ws, 0u), 0u);
    expect_u("hold MUTANT: a LONGER name must not match on its prefix",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, longer, sizeof longer), 0u);
    expect_u("hold MUTANT: a name buffer too short to hold 'WindowServer' never holds",
             n48_agdc_hold_ms(MS, 0u, N48_AGDC_CMD_LINK_CONFIG, ws, 5u), 0u);

    // THE LATCH MUTANT, planted exactly as asked: a latch that never sets fires on EVERY 0x921.
    // The correct caller sets `fired` BEFORE sleeping, so the second gather's 0x921 does not hold.
    uint32_t firedBroken = 0, firedGood = 0, hitsBroken = 0, hitsGood = 0;
    for (int call = 0; call < 2; call++) {                       // the two 0x921 calls of a real boot
        if (n48_agdc_hold_ms(MS, firedBroken, N48_AGDC_CMD_LINK_CONFIG, ws, sizeof ws)) hitsBroken++;  // latch never set
        if (n48_agdc_hold_ms(MS, firedGood, N48_AGDC_CMD_LINK_CONFIG, ws, sizeof ws)) { hitsGood++; firedGood = 1; }
    }
    expect_u("hold MUTANT: a latch that never sets holds on BOTH 0x921 calls", (uint64_t)hitsBroken, 2u);
    expect_u("hold: the one-shot latch set before the sleep holds exactly ONCE", (uint64_t)hitsGood, 1u);

    // -----------------------------------------------------------------------------------------------------------------
    // 0.0.329 : n48_dpg_chain_ok - may a hooked slot call back into Apple's original?
    // The bug it exists to prevent: dpg_enableIrq was a counting stub that never chained, so slot 273 -
    // enableTransactionInterrupt, the call that arms the interrupt driving event_interrupt_gated - would have been
    // swallowed the first time Apple ever made it.
    // -----------------------------------------------------------------------------------------------------------------
    {
        const uint64_t slide = 0xffffff7f9d2e5000ull;            // a real slide from an earlier analysis (page aligned)
        const uint64_t enFn  = slide + N48_DPG_CHAIN_ENABLE_IRQ;
        const uint64_t disFn = slide + N48_DPG_CHAIN_DISABLE_IRQ;

        expect_u("chain: the exact enableTransactionInterrupt target is callable",
                 (uint64_t)n48_dpg_chain_ok(enFn, slide, N48_DPG_CHAIN_ENABLE_IRQ), 1u);
        expect_u("chain: the exact disableTransactionInterrupt target is callable",
                 (uint64_t)n48_dpg_chain_ok(disFn, slide, N48_DPG_CHAIN_DISABLE_IRQ), 1u);

        expect_u("chain MUTANT: a null stored pointer is never called",
                 (uint64_t)n48_dpg_chain_ok(0u, slide, N48_DPG_CHAIN_ENABLE_IRQ), 0u);
        expect_u("chain MUTANT: a zero slide is never called",
                 (uint64_t)n48_dpg_chain_ok(enFn, 0u, N48_DPG_CHAIN_ENABLE_IRQ), 0u);
        expect_u("chain MUTANT: an unaligned slide is a bad read, never called",
                 (uint64_t)n48_dpg_chain_ok(enFn, slide | 8u, N48_DPG_CHAIN_ENABLE_IRQ), 0u);
        expect_u("chain MUTANT: a pointer below the image base is never called",
                 (uint64_t)n48_dpg_chain_ok(slide - 0x10u, slide, N48_DPG_CHAIN_ENABLE_IRQ), 0u);
        expect_u("chain MUTANT: one byte off the target is never called",
                 (uint64_t)n48_dpg_chain_ok(enFn + 1u, slide, N48_DPG_CHAIN_ENABLE_IRQ), 0u);

        // THE CROSS-WIRING MUTANT: 273 and 274 are adjacent slots with adjacent targets. Storing one and checking it
        // against the other's offset must fail, or a swap of the two stores would chain silently to the wrong function.
        expect_u("chain MUTANT: the ENABLE pointer must not satisfy the DISABLE guard",
                 (uint64_t)n48_dpg_chain_ok(enFn, slide, N48_DPG_CHAIN_DISABLE_IRQ), 0u);
        expect_u("chain MUTANT: the DISABLE pointer must not satisfy the ENABLE guard",
                 (uint64_t)n48_dpg_chain_ok(disFn, slide, N48_DPG_CHAIN_ENABLE_IRQ), 0u);

        // THE STALE-SLIDE MUTANT: the same stored pointer read against a different boot's slide must refuse.
        expect_u("chain MUTANT: a pointer from another boot's slide is never called",
                 (uint64_t)n48_dpg_chain_ok(enFn, slide + 0x1000u, N48_DPG_CHAIN_ENABLE_IRQ), 0u);
    }

    // =========================================================================================================
    // build 0.0.514 B1/B2/B5: native 1440p preparation. At 1920x1080 every answer is 0.0.513's byte for
    // byte (fill513 below is 0.0.513's n48_agdc_fill_link_config body, copied verbatim); at 2560x1440 the endpoint is the
    // live Console size and the timing is the live raster's when it IS that endpoint.
    // =========================================================================================================
    {
        struct F513 {
            static int fill(uint8_t *out, size_t len, uint32_t w, uint32_t h) {
                if (!out || len != N48_AGDC_LINK_CONFIG_LEN) return 1;
                for (size_t i = 0; i < len; i++) out[i] = 0;
                if (!w) w = 1920u;
                if (!h) h = 1080u;
                const uint64_t hTotal = (uint64_t)w + (uint64_t)N48_AGDC_HBLANK;
                const uint64_t vTotal = (uint64_t)h + (uint64_t)N48_AGDC_VBLANK;
                const uint64_t pixelClock = hTotal * vTotal * (uint64_t)N48_AGDC_REFRESH_HZ;
                n48_le64(out, 0x2cu, pixelClock); n48_le64(out, 0x34u, pixelClock); n48_le64(out, 0x3cu, pixelClock);
                n48_le32(out, 0x44u, w); n48_le32(out, 0x48u, N48_AGDC_HBLANK); n48_le32(out, 0x4cu, N48_AGDC_HSYNC_OFF);
                n48_le32(out, 0x50u, N48_AGDC_HSYNC_W); n48_le32(out, 0x54u, h); n48_le32(out, 0x58u, N48_AGDC_VBLANK);
                n48_le32(out, 0x5cu, N48_AGDC_VSYNC_OFF); n48_le32(out, 0x60u, N48_AGDC_VSYNC_W);
                return 0;
            }
        };
        // B1: the endpoint size's source (gRa, else the live Console size, else the constant)
        struct Dim { uint32_t ra, live, fb, want; const char *what; } dims[] = {
            { 0u,    1920u, 1920u, 1920u, "B1 1080p boot, route A unarmed: Console,Width 1920 -> 1920 (0.0.513's answer)" },
            { 0u,    1080u, 1080u, 1080u, "B1 1080p boot, route A unarmed: Console,Height 1080 -> 1080 (0.0.513's answer)" },
            { 0u,    0u,    1920u, 1920u, "B1 no Console property: the old constant 1920" },
            { 1920u, 2560u, 1920u, 1920u, "B1 route A armed: its captured width wins over Console" },
            { 0u,    2560u, 1920u, 2560u, "B1 1440p boot, route A unarmed: Console,Width 2560 (0.0.513 said 1920)" },
            { 0u,    1440u, 1080u, 1440u, "B1 1440p boot, route A unarmed: Console,Height 1440 (0.0.513 said 1080)" },
            { 0u,    99999u, 1920u, 1920u, "B1 an absurd Console value (> 16384) is treated as absent" },
        };
        for (const Dim &d : dims) expect_u(d.what, n48_agdc_endpoint_dim(d.ra, d.live, d.fb), d.want);
        // B2 + B5: 1920x1080 byte identity, with no raster and with the 1440p link raster (a scaled 1080p framebuffer)
        const n48_agdc_raster panel = { 2560u, 1440u, 2720u, 1481u, 48u, 32u, 3u, 5u, 241500000ull };
        uint8_t a[N48_AGDC_LINK_CONFIG_LEN], b[N48_AGDC_LINK_CONFIG_LEN];
        const uint32_t sizes[][2] = { { 1920u, 1080u }, { 0u, 0u }, { 1280u, 720u }, { 2560u, 1440u } };
        for (const auto &sz : sizes) {
            char what[160];
            std::memset(a, 0x5a, sizeof a); std::memset(b, 0xa5, sizeof b);
            const int ra = F513::fill(a, sizeof a, sz[0], sz[1]);
            const int rb = n48_agdc_fill_link_config(b, sizeof b, sz[0], sz[1]);
            std::snprintf(what, sizeof what, "B5 %ux%u: n48_agdc_fill_link_config's reply == 0.0.513's, byte for byte", sz[0], sz[1]);
            expect_u(what, (uint64_t)(ra == rb && std::memcmp(a, b, sizeof a) == 0), 1u);
            if (sz[0] == 2560u) continue;   // 2560x1440 with the raster is the live case below
            n48_agdc_timing t {};
            n48_agdc_timing_for(sz[0], sz[1], &panel, &t);
            std::memset(b, 0xa5, sizeof b);
            const int rc = n48_agdc_fill_link_config_t(b, sizeof b, &t);
            std::snprintf(what, sizeof what, "B5 %ux%u with the 2560x1440 link raster (scaled): CEA, byte for byte 0.0.513's", sz[0], sz[1]);
            expect_u(what, (uint64_t)(rc == ra && t.live == 0u && std::memcmp(a, b, sizeof a) == 0), 1u);
        }
        // B2: native 2560x1440 with the live raster -> the panel's own timing (: 2720x1481 totals, 241.5 MHz)
        {
            n48_agdc_timing t {};
            n48_agdc_timing_for(2560u, 1440u, &panel, &t);
            std::memset(b, 0xa5, sizeof b);
            expect_u("B2 2560x1440 + live raster: filled", (uint64_t)n48_agdc_fill_link_config_t(b, sizeof b, &t), 0u);
            expect_u("B2   timing is LIVE", t.live, 1u);
            expect_u("B2   +0x44 HorizontalActive 2560", rd32(b, 0x44), 2560u);
            expect_u("B2   +0x54 VerticalActive 1440", rd32(b, 0x54), 1440u);
            expect_u("B2   +0x48 HorizontalBlanking 160 (2720 - 2560)", rd32(b, 0x48), 160u);
            expect_u("B2   +0x58 VerticalBlanking 41 (1481 - 1440)", rd32(b, 0x58), 41u);
            expect_u("B2   +0x4c/+0x50 H sync offset 48, width 32", (uint64_t)(rd32(b, 0x4c) == 48u && rd32(b, 0x50) == 32u), 1u);
            expect_u("B2   +0x5c/+0x60 V sync offset 3, width 5", (uint64_t)(rd32(b, 0x5c) == 3u && rd32(b, 0x60) == 5u), 1u);
            expect_u("B2   +0x2c/+0x34/+0x3c PixelClock 241500000 (the EDID DTD's)",
                     (uint64_t)(rd64(b, 0x2c) == 241500000ull && rd64(b, 0x34) == 241500000ull && rd64(b, 0x3c) == 241500000ull), 1u);
            // refresh = 241.5 MHz / (2720 * 1481) = 59.95 Hz: coherent (the derived CEA clock at 2560x1440 would say 60.0 at
            // 2840x1485 totals, which is not what the panel runs)
            expect_u("B2   refresh from the reply = 59950 mHz, floor of 59.9507 Hz (the panel's measured 59.951 Hz,)",
                     rd64(b, 0x2c) * 1000ull / ((uint64_t)(rd32(b, 0x44) + rd32(b, 0x48)) * (rd32(b, 0x54) + rd32(b, 0x58))), 59950u);
            n48_agdc_timing c {};
            n48_agdc_timing_for(2560u, 1440u, nullptr, &c);
            expect_u("B2 2560x1440 with the raster UNKNOWN: the old CEA blanking, clock derived for 60 Hz",
                     (uint64_t)(c.live == 0u && c.h_blank == N48_AGDC_HBLANK && c.v_blank == N48_AGDC_VBLANK &&
                                c.pixel_clock == 2840ull * 1485ull * 60ull), 1u);
            n48_agdc_raster bad = panel; bad.h_total = 2560u;
            n48_agdc_timing_for(2560u, 1440u, &bad, &c);
            expect_u("B2 an incoherent raster (total == active) is not used", c.live, 0u);
            bad = panel; bad.pixel_clock_hz = 0u;
            n48_agdc_timing_for(2560u, 1440u, &bad, &c);
            expect_u("B2 a raster with no clock is not used", c.live, 0u);
            bad = panel; bad.v_front = 40u;
            n48_agdc_timing_for(2560u, 1440u, &bad, &c);
            expect_u("B2 porches outside the blanking are not used", c.live, 0u);
        }
        // B5's end-to-end 1440p case: the reply a 1440p boot (route A unarmed) now gives, from Console + the raster
        {
            const uint32_t w = n48_agdc_endpoint_dim(0u, 2560u, 1920u), h = n48_agdc_endpoint_dim(0u, 1440u, 1080u);
            n48_agdc_timing t {};
            n48_agdc_timing_for(w, h, &panel, &t);
            std::memset(b, 0, sizeof b);
            (void)n48_agdc_fill_link_config_t(b, sizeof b, &t);
            expect_u("B5 1440p boot end to end: LINKCFG endpoint 2560x1440, live timing",
                     (uint64_t)(rd32(b, 0x44) == 2560u && rd32(b, 0x54) == 1440u && t.live == 1u), 1u);
            uint8_t pc[N48_AGDC_PIPELINE_CAPS_LEN];
            std::memset(pc, 0, sizeof pc);
            n48_le32(pc, 4u, N48_AGDC_SCALER_TYPE);
            expect_u("B5 1440p boot end to end: the 0x711 scaler reply fills at 2560x1440",
                     (uint64_t)n48_agdc_fill_pipeline_caps(pc, sizeof pc, w, h), 0u);
        }
    }

    std::printf("\n%s + %s: %d/%d passed\n", N48_DPG_TOKEN, N48_RA_TOKEN, gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
