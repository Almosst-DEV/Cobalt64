// display_pipe_guard.h — pure C for the 0.0.286 display-pipe safety core and the AGDC nub's replies display brief,
// ). Host-tested by tests/display_pipe_guard_test.cpp; the kext compiles the SAME header. No IOKit, no allocation.
//
// 1. The GFX-ring WRITE_DATA guard. Apple's display flip path builds its register writes with
//    AMDRadeonX6000_AMDGFX10Display::writeWriteData1RegCmdPacket @0xbe442b8 (CONFIRMED, bytes):
//      be442bc 48 b8 00 37 03 c0 00 00 01 00  movabs $0x10000c0033700,%rax   ; dw0 0xC0033700 (type 3, count 3, op 0x37),
//      be442c6 48 89 06                       movq %rax,(%rsi)                ; dw1 0x00010000 (WR_ONE_ADDR, DST_SEL 0 = register)
//      be442c9 81 e2 ff ff 03 00              andl $0x3ffff,%edx
//      be442cf 89 56 08                       movl %edx,0x8(%rsi)             ; dw2 register dword offset
//      be442d2 c7 46 0c 00 00 00 00           movl $0,0xc(%rsi)               ; dw3 0
//      be442d9 89 4e 10                       movl %ecx,0x10(%rsi)            ; dw4 value
//    A register WRITE_DATA (op 0x37, DST_SEL 0) whose dword address lies in a DCN window is rewritten in place into a type-3 NOP
//    of the SAME length (only the opcode byte changes, as gfx_neuter.h does for INDIRECT_BUFFER).
//    The windows are the absolute dword spans of every DCN 4.1 register (Linux dcn_4_1_0_offset.h, BASE_IDX 0-2, plus the dense
//    part of BASE_IDX 3) on this card's DMU segment bases 0x12 / 0xc0 / 0x34c0 / 0x9000 (runbook I, on-die discovery):
//      base 0: 0x12 + [0x0, 0x4d]      -> [0x12, 0x60)
//      base 1: 0xc0 + [0x40, 0x799]    -> [0x100, 0x860)
//      base 2: 0x34c0 + [0x4a, 0x3966] -> [0x350a, 0x6e27)    widened to [0x34c0, 0x7000)
//      base 3: 0x9000 + ...            -> [0x9000, 0xa000)
//    GC 12 (gc_12_0_0_offset.h) spans 0x1260 + [0, 0x214d] = [0x1260, 0x33ae) and 0xa000 + [0, 0x5f91] = [0xa000, 0xff92): no
//    overlap, so no GC register write Apple's rings legitimately make can be caught.
//
// 2. The AGDC replies (IOPresentment's association map). kAGDCVendorInfo (1) is AMD's own static struct,
//    AMDRadeonX6000_AmdAgdcServices::getVendorInfo @0xbf7f79e (CONFIRMED): len 0x2c, +0x00 u32 0x30000, +0x04 u64 0x444d41
//    ("AMD"), +0x24 u32 0x1002, +0x28 u32 2. kAGDCGPUCapability (0x980) is getGpuCapability @0xbf7f916's cached struct at
//    this+0x140 (CONFIRMED layout, bf7fbbc-bf7fbf7): len 0xdc, +0x00 u64 framebuffer-present mask (2 << i), +0x20 u32 count,
//    +0x24/+0x28/+0x2c u32 a per-link byte, +0x30 u32 count (IOPresentment requires it non-zero, 0x7ff815990853), +0x34 u64 the
//    controller's PCI device, +0x3c u64[] one IOFramebuffer* per framebuffer (count <= 7, bf7fb8f). The AGDC base class turns +0x34
//    and each +0x3c entry into registry IDs before a user client sees them (0x13d3d9fc / 0x13d3da01 call
//    IORegistryEntry::getRegistryEntryID through GOT 0x20230, CONFIRMED); IOPresentment opens each +0x3c ID (0x7ff81599087a).
#ifndef N48_DISPLAY_PIPE_GUARD_H
#define N48_DISPLAY_PIPE_GUARD_H

#include <stdint.h>
#include <stddef.h>

#define N48_DPG_TOKEN "display-pipe-guard 0.0.286"

/* ---- 1. WRITE_DATA guard ------------------------------------------------------------------------------------------------ */
#define N48_PM4_OP_WRITE_DATA 0x37u
#define N48_PM4_OP_NOP        0x10u

typedef struct { uint32_t lo, hi; } n48_dpg_window;
static const n48_dpg_window kN48DcnWindows[4] = {
    { 0x12u, 0x60u }, { 0x100u, 0x860u }, { 0x34c0u, 0x7000u }, { 0x9000u, 0xa000u },
};

static inline int n48_dpg_is_display_reg(uint32_t dw)
{
    for (unsigned i = 0; i < 4u; i++)
        if (dw >= kN48DcnWindows[i].lo && dw < kN48DcnWindows[i].hi) return 1;
    return 0;
}

enum { N48_DPG_NOT_WD = 0, N48_DPG_WD_OTHER = 1, N48_DPG_WD_REG_OK = 2, N48_DPG_WD_REG_DISPLAY = 3, N48_DPG_WD_SHORT = 4 };

/* Classify the type-3 packet whose header is h and whose next dwords (up to `avail` of them after the header) are body[].
 * Returns N48_DPG_NOT_WD for anything but opcode 0x37; for a WRITE_DATA: SHORT when the body cannot hold control + address,
 * OTHER for a non-register destination, REG_DISPLAY when DST_SEL 0 names a DCN window, else REG_OK. *regOut gets the address. */
static inline int n48_dpg_classify(uint32_t h, const uint32_t *body, uint32_t avail, uint32_t *regOut)
{
    if (regOut) *regOut = 0;
    if ((h >> 30) != 3u || ((h >> 8) & 0xFFu) != N48_PM4_OP_WRITE_DATA) return N48_DPG_NOT_WD;
    const uint32_t cnt = (h >> 16) & 0x3FFFu;      /* body dwords = cnt + 1 */
    if (cnt < 1u || avail < 2u || !body) return N48_DPG_WD_SHORT;
    if ((body[0] & 0xFu) != 0u) return N48_DPG_WD_OTHER;
    if (regOut) *regOut = body[1];
    return n48_dpg_is_display_reg(body[1]) ? N48_DPG_WD_REG_DISPLAY : N48_DPG_WD_REG_OK;
}

/* The same-length NOP for a WRITE_DATA header; 0 (refuse) for anything else. */
static inline uint32_t n48_dpg_nop_for(uint32_t h)
{
    if ((h >> 30) != 3u || ((h >> 8) & 0xFFu) != N48_PM4_OP_WRITE_DATA) return 0u;
    return (h & ~0x0000FF00u) | (N48_PM4_OP_NOP << 8);
}

/* ---- 2. The AMD pipe / display slots the safety core replaces, and their static (unslid) targets --------------------------
 * __ZTV34AMDRadeonX6000_AMDAccelDisplayPipe @0xbf18220: 306 slots (slot 305 raw 0x01000000545cc0fc ends the chain).
 * __ZTV31AMDRadeonX6000_AMDNavi21Display @0xbf40fa0: 78 slots (slot 77 raw 0x010000004be536de ends the chain). */
#define N48_DPG_PIPE_SLOTS    306u
#define N48_DPG_DISP_SLOTS    78u
typedef struct { uint16_t slot; uint32_t target; const char *name; } n48_dpg_slot;
static const n48_dpg_slot kN48PipeGuardSlots[] = {
    {   0u, 0x0bdcc36eu, "D1" },
    {   7u, 0x0bdcc3b8u, "getMetaClass" },
    { 266u, 0x0bdcc4e4u, "init" },
    { 267u, 0x0bdcc88au, "initFramebufferResource" },
    { 268u, 0x0bdcca94u, "destroyFramebufferResource" },
    { 273u, 0x0bdcceceu, "enableTransactionInterrupt" },
    { 274u, 0x0bdccf58u, "disableTransactionInterrupt" },
    { 276u, 0x0bdccfe6u, "validateTransaction" },
    { 277u, 0x0bdcddbeu, "performTransaction" },
    { 278u, 0x0bdcdcdcu, "isTransactionComplete" },
    { 279u, 0x0bdcdfaeu, "submitTransaction" },
    { 283u, 0x0bdcdf54u, "beginTransaction" },
    { 284u, 0x0bdce030u, "signalTransactionComplete" },
};
static const n48_dpg_slot kN48DispGuardSlots[] = {
    {  0u, 0x0be53598u, "D1" },
    {  7u, 0x0be535e2u, "getMetaClass" },
    { 51u, 0x0be0b69eu, "getDisplayPipeTransactionFlip" },
};

/* The AMD event machine accel+0x380 = AMDRadeonX6000_AMDAccelEventMachine (created by the accelerator's slot-272 factory,
 * newEventMachine 0xbdbf9e8 -> operator new(0xd58); IOGraphicsAccelerator2::start stores it at accel+0x380). Its concrete
 * base is IAF2's IOAccelEventMachineFast, so the slots IAF2 calls on it are IAF2 addresses, resolved by the SAME shared
 * X6000/IAF2 slide the safety core already proves. __ZTV35AMDRadeonX6000_AMDAccelEventMachine @0xbf18cc0: 95 slots (slot 94
 * raw 0x01000000545b3d0e ends the chain). Slots 0/7 are the X6000 identity anchors (D1 0xbdce540, getMetaClass 0xbdce58a);
 * 40/41/48/54/55/74/75 are the IAF2 IOAccelEventMachineFast methods IOAccelDisplayPipe reaches during readiness and every
 * transaction (the reviewer's list). Decoded and disassembled (this session): each is pure IOAccelEvent memory
 * bookkeeping and touches NO gfx1201 register and NO ring — the census below hit-counts them on live hardware to confirm.
 *   40 initEvent 0x145b1fe6                  (memzero: 8 qwords 0xffffffff into the event)
 *   41 cleanEvent 0x145b274a                 (the same memzero)
 *   48 testEventUnlocked 0x145b2368          (read stamp values in memory; only reached for txn+0x58==0xE0014042)
 *   54 copyEvent 0x145b3284                  (0x40-byte memcpy event->event)
 *   55 mergeEvent 0x145b32c8                 (read stamps, update cached stamps at machine+0xf8)
 *   74 deviceTerminatedUnlocked 0x145b3948
 *   75 enableEventStampInterrupts 0x145b23d4 (refcount + slot *0x240; only on the armed 0xE0014042 path we skip) */
#define N48_DPG_EM_SLOTS 95u
static const n48_dpg_slot kN48EmGuardSlots[] = {
    {  0u, 0x0bdce540u, "D1" },
    {  7u, 0x0bdce58au, "getMetaClass" },
    { 40u, 0x145b1fe6u, "initEvent" },
    { 41u, 0x145b274au, "cleanEvent" },
    { 48u, 0x145b2368u, "testEventUnlocked" },
    { 54u, 0x145b3284u, "copyEvent" },
    { 55u, 0x145b32c8u, "mergeEvent" },
    { 74u, 0x145b3948u, "deviceTerminatedUnlocked" },
    { 75u, 0x145b23d4u, "enableEventStampInterrupts" },
};
/* The subset the census wraps with counting trampolines (0/7 are identity anchors, not wrapped). */
#define N48_DPG_EM_WRAP_FIRST 2u
#define N48_DPG_N_PIPE_GUARD (sizeof(kN48PipeGuardSlots) / sizeof(kN48PipeGuardSlots[0]))
#define N48_DPG_N_DISP_GUARD (sizeof(kN48DispGuardSlots) / sizeof(kN48DispGuardSlots[0]))
#define N48_DPG_N_EM_GUARD   (sizeof(kN48EmGuardSlots) / sizeof(kN48EmGuardSlots[0]))

/* -------------------------------------------------------------------------------------------------------------------
 * 0.0.329 : chaining a hooked slot back to Apple's original.
 *
 * dpg_enableIrq replaced slot 273 (enableTransactionInterrupt) with a counting stub that NEVER called the original,
 * while dpg_destroyFb correctly keeps gPg.origDestroy and chains. Apple has never called slot 273 (enable/disable read
 * 0/0 in every run, and the driver log carries no `pipeguard: REFUSED enableTransactionInterrupt` note), so the stub
 * has never cost us anything - but slot 273 is what arms the interrupt that drives IOAccelDisplayPipe::
 * event_interrupt_gated (0x145ce7ea), which is the only path that reaches validateTransaction/performTransaction.
 * A stub there would swallow the exact call we are waiting for, the first time anything upstream started working.
 *
 * The chain is address-guarded like every other Apple call we make: the stored pointer must be the slot's own static
 * target at the live slide, or we do not call it. N48_DPG_CHAIN_* name the two slots this applies to.
 */
#define N48_DPG_CHAIN_ENABLE_IRQ  0x0bdcceceu   /* slot 273, enableTransactionInterrupt  */
#define N48_DPG_CHAIN_DISABLE_IRQ 0x0bdccf58u   /* slot 274, disableTransactionInterrupt */

/* 1 when fn is exactly wantOff past a sane slide and may be called, else 0. Fails closed on every degenerate input. */
static inline int n48_dpg_chain_ok(uint64_t fn, uint64_t slide, uint32_t wantOff)
{
    if (!fn || !slide) return 0;          /* nothing stored, or no slide */
    if (slide & 0xfffu) return 0;         /* a slide is page aligned; anything else is a bad read */
    if (fn <= slide) return 0;            /* below the image base - not a kext function */
    if (fn - slide > 0xffffffffull) return 0;
    return (uint32_t)(fn - slide) == wantOff;
}

/* 0 when every guarded slot of vt (slide already known) names its static target, else 1 + the index of the first mismatch. */
static inline unsigned n48_dpg_check_slots(const uint64_t *vt, uint64_t slide, const n48_dpg_slot *s, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        if (vt[s[i].slot] - slide != (uint64_t)s[i].target) return i + 1u;
    return 0u;
}

/* The refusal values the hooks return, from the callers' own tests (CONFIRMED):
 *  - initFramebufferResource returns the VidMemory; IOAccelDisplayPipe::init_framebuffer_resource tests it
 *    (145cc4f8 testq %rax,%rax; je -> false), so NULL leaves the pipe inactive (+0x298 stays 0).
 *  - getDisplayPipeTransactionFlip returns bool; executeTransaction bdcd2fb `testb %al,%al; je` -> 0xe00002bc, no flip.
 *  - validate/perform/submit return IOReturn to IOAccelDisplayPipeTransaction2::set_transaction_args (145ce08e) and
 *    event_interrupt_gated (145ce984); a non-zero value fails the transaction. kIOReturnNotPermitted marks OUR refusal
 *    apart from the family's own kIOReturnNotReady (0xe00002d8).
 *  - isTransactionComplete returns bool; true lets a waiter or the timeout path finish instead of waiting. */
#define N48_DPG_REFUSE_IORETURN 0xe00002e2u

/* ---- 3. AGDC replies -------------------------------------------------------------------------------------------------- */
#define N48_AGDC_CMD_VENDOR_INFO    0x1u
#define N48_AGDC_CMD_GPU_CAPABILITY 0x980u
#define N48_AGDC_CMD_LINK_CONFIG    0x921u      /* getLinkConfig(AGDCLinkConfig_t*) — IOPresentment __GatherAGDCLogicalDeviceCapabilities */
#define N48_AGDC_CMD_PIPELINE_CAPS  0x711u      /* display-pipeline/scaler caps — IOPresentment __GatherAGDCDisplayPipelineCapabilities */
#define N48_AGDC_VENDOR_INFO_LEN    0x2cu
#define N48_AGDC_GPU_CAP_LEN        0xdcu
#define N48_AGDC_LINK_CONFIG_LEN    0xb0u       /* CONFIRMED: IOPresentment inStructCnt/outStructCnt = 0xb0 (M4-AGDC-CAPABILITIES) */
#define N48_AGDC_PIPELINE_CAPS_LEN  0x196cu     /* SUSPECTED-to-fire: __GatherAGDCDisplayPipelineCapabilities struct size (same memo) */
#define N48_AGDC_MAX_FB             7u

static inline void n48_le32(uint8_t *p, uint32_t off, uint32_t v)
{
    p[off] = (uint8_t)v; p[off + 1u] = (uint8_t)(v >> 8); p[off + 2u] = (uint8_t)(v >> 16); p[off + 3u] = (uint8_t)(v >> 24);
}
static inline void n48_le64(uint8_t *p, uint32_t off, uint64_t v)
{
    n48_le32(p, off, (uint32_t)v); n48_le32(p, off + 4u, (uint32_t)(v >> 32));
}

/* 0 on success; 1 on a wrong length or null buffer (AMD refuses a mismatched length with kIOReturnBadArgument too). */
static inline int n48_agdc_fill_vendor_info(uint8_t *out, size_t len)
{
    if (!out || len != N48_AGDC_VENDOR_INFO_LEN) return 1;
    for (size_t i = 0; i < len; i++) out[i] = 0;
    n48_le32(out, 0x00u, 0x30000u);
    n48_le64(out, 0x04u, 0x444d41u);
    n48_le32(out, 0x24u, 0x1002u);
    n48_le32(out, 0x28u, 2u);
    return 0;
}

/* fbs[0..nfb) are the IOFramebuffer pointers (the AGDC base class converts them to registry IDs), pci the PCI device. */
static inline int n48_agdc_fill_gpu_capability(uint8_t *out, size_t len, uint64_t pci, const uint64_t *fbs, uint32_t nfb)
{
    if (!out || len != N48_AGDC_GPU_CAP_LEN || nfb == 0u || nfb > N48_AGDC_MAX_FB || !fbs) return 1;
    for (size_t i = 0; i < len; i++) out[i] = 0;
    uint64_t mask = 0;
    for (uint32_t i = 0; i < nfb; i++) { mask |= (uint64_t)2u << i; n48_le64(out, 0x3cu + 8u * i, fbs[i]); }
    n48_le64(out, 0x00u, mask);
    n48_le32(out, 0x20u, nfb);
    n48_le32(out, 0x30u, nfb);
    n48_le64(out, 0x34u, pci);
    return 0;
}

/* CEA-861 1080p60 blanking. SUSPECTED, NOT read from this panel's EDID: the DP link runs the monitor's native
 * 2560x1440 and the framebuffer is a deliberate 1920x1080 (OpenCore GOP resolution, set to cut CPU load), so the
 * OTG registers describe the LINK, not the endpoint we are declaring. Only HorizontalActive / VerticalActive are
 * load-bearing — DisplayPipe::Commit's gate is a plain `ucomisd` against 0.0, so any non-zero pair passes. The
 * blanking exists to keep the derived refresh coherent, not because Commit reads it. */
#define N48_AGDC_HBLANK    280u
#define N48_AGDC_HSYNC_OFF 88u
#define N48_AGDC_HSYNC_W   44u
#define N48_AGDC_VBLANK    45u
#define N48_AGDC_VSYNC_OFF 4u
#define N48_AGDC_VSYNC_W   5u
#define N48_AGDC_REFRESH_HZ 60u

/* getLinkConfig (selector 0x921), AGDCLinkConfig_t 0xb0 bytes. Reply-relative offsets, ALL CONFIRMED from
 * IOPresentment bytes (0.0.336,). The reply carries an IODetailedTimingInformationV2 at +0x04:
 * __GatherAGDCLogicalDeviceCapabilities (0x7ff81599cad2) sets the buffer base to obj+0x2c, then at
 * 0x7ff81599cbbe-0x7ff81599cc65 copies reply+0x10..+0x9c into a struct at obj+0x19b00 with dst = reply - 4.
 * IOPresentmentCreateEndPointDictionary (0x7ff81599a7c6) then names every field of that struct:
 *   dst+0x28 PixelClock   dst+0x30 MinPixelClock  dst+0x38 MaxPixelClock       (SInt64, CFNumberType 4)
 *   dst+0x40 HorizontalActive  +0x44 HorizontalBlanking  +0x48 HorizontalSyncOffset  +0x4c HorizontalSyncPulseWidth
 *   dst+0x50 VerticalActive    +0x54 VerticalBlanking    +0x58 VerticalSyncOffset    +0x5c VerticalSyncPulseWidth
 * so reply+0x44 = HorizontalActive and reply+0x54 = VerticalActive. Those two become the EndPointDictionary's
 * HorizontalActive/VerticalActive, which is what CoreDisplay::IOPCapabilities::GetEndpointSize returns; zero there
 * is the "Invalid end point size" that has destroyed every IOPTransaction before Submit.
 * PixelClock is NOT optional either: __gatherCapabilities computes ONE float at obj+0x19c54 as
 * pixelClock / ((hAct+hBlank)*(vAct+vBlank)) (0x7ff81599a676-0x7ff81599a6fb) and hands the SAME field to
 * kIOPresentmentRefreshTime, MinRefreshTime AND MaxRefreshTime (0x7ff81599abb8-0x7ff81599ac5e). Non-zero
 * active with a zero clock would turn 60.0 into 0.0 in all three — trading one zero for another.
 * +0x08 bit0 is left CLEAR: 0x7ff81599cc6f `testb $0x1,0x34(%rbx)` gates __GatherAGDCLinkCapabilities, which we
 * do not want on the path. Returns 1 on a wrong length or null buffer, else 0. */
/* build 0.0.514 B1: THE ENDPOINT SIZE'S SOURCE, in order: route A's captured mode (gRa, when armed), else
 * RDNA4FB's live Console,Width/Height (`live`, 0 = absent: the same property scanout_copy.h's geometry reads), else the old
 * documented constant (`fallback`: 1920 / 1080,). 0.0.513 went straight from gRa to the constant, so a native 1440p
 * framebuffer without route A would have been declared 1920x1080. A live value above 16384 is treated as absent. */
static inline uint32_t n48_agdc_endpoint_dim(uint32_t ra, uint32_t live, uint32_t fallback)
{
    return ra ? ra : ((live && live <= 16384u) ? live : fallback);
}

/* build 0.0.514 B2: THE REPORTED TIMING. The live raster is the lit OTG's (read-only: its active
 * size from OTG_H/V_BLANK_START_END and its totals from OTG_H/V_TOTAL, src/dcn41 dcn41_otg_get_active_size/_totals); its pixel
 * clock and porches come from the sink's own EDID row with the SAME active size and totals (src/dcn41/dcn41_modes.h: the
 * SINK-A's 2560x1440@60 DTD, 2720x1481 totals, 241.5 MHz, front 48/3, sync 32/5 -'s measured raster). The live raster
 * is used ONLY when its active size IS the endpoint being declared (no DPP scaling: a native framebuffer) and it is coherent
 * (totals above the actives, porches inside the blanking, a non-zero clock); otherwise - and always at 1920x1080 - the timing
 * is 0.0.513's CEA-861 1080p60 blanking with the clock derived for 60 Hz, byte for byte. */
typedef struct {
    uint32_t h_active, v_active, h_total, v_total;
    uint32_t h_front, h_sync, v_front, v_sync;
    uint64_t pixel_clock_hz;
} n48_agdc_raster;
typedef struct {
    uint32_t w, h, h_blank, h_sync_off, h_sync_w, v_blank, v_sync_off, v_sync_w;
    uint64_t pixel_clock;
    uint32_t live;   /* 1 = from the live raster, 0 = the CEA fallback */
} n48_agdc_timing;
static inline void n48_agdc_timing_for(uint32_t w, uint32_t h, const n48_agdc_raster *r, n48_agdc_timing *t)
{
    if (!t) return;
    /* A zero here is the exact bug the link-config reply fixed, so it can never leave: clamp rather than trust. */
    if (!w) w = 1920u;
    if (!h) h = 1080u;
    t->w = w; t->h = h;
    if (r && r->h_active == w && r->v_active == h && r->h_total > w && r->v_total > h && r->pixel_clock_hz &&
        r->h_sync && r->v_sync &&
        (uint64_t)r->h_front + r->h_sync <= (uint64_t)(r->h_total - w) &&
        (uint64_t)r->v_front + r->v_sync <= (uint64_t)(r->v_total - h)) {
        t->h_blank = r->h_total - w; t->h_sync_off = r->h_front; t->h_sync_w = r->h_sync;
        t->v_blank = r->v_total - h; t->v_sync_off = r->v_front; t->v_sync_w = r->v_sync;
        t->pixel_clock = r->pixel_clock_hz;
        t->live = 1u;
        return;
    }
    t->h_blank = N48_AGDC_HBLANK; t->h_sync_off = N48_AGDC_HSYNC_OFF; t->h_sync_w = N48_AGDC_HSYNC_W;
    t->v_blank = N48_AGDC_VBLANK; t->v_sync_off = N48_AGDC_VSYNC_OFF; t->v_sync_w = N48_AGDC_VSYNC_W;
    /* Derived, not copied, so the refresh stays 60.0 for whatever mode route A captured. At 1920x1080 this is
     * 2200 * 1125 * 60 = 148500000 — bit-identical to the CEA-861 1080p60 pixel clock. */
    t->pixel_clock = ((uint64_t)w + (uint64_t)N48_AGDC_HBLANK) * ((uint64_t)h + (uint64_t)N48_AGDC_VBLANK)
                     * (uint64_t)N48_AGDC_REFRESH_HZ;
    t->live = 0u;
}

/* build 0.0.515: getLinkConfig's timing decision in ONE pure place, so the tests can hold the kext
 * to it. At 1920x1080 the raster reader is NEVER called (no register read) and the answer is 0.0.513's CEA timing byte for
 * byte; otherwise the reader (the kext passes n48dcn::liveRaster, which no longer depends on bind) is asked once and its
 * raster is used when n48_agdc_timing_for accepts it. */
typedef uint32_t (*n48_agdc_raster_fn)(void *ctx, n48_agdc_raster *r);
static inline void n48_agdc_link_timing(uint32_t lw, uint32_t lh, n48_agdc_raster_fn fn, void *ctx, n48_agdc_timing *lt)
{
    n48_agdc_raster lr = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0ull };
    const uint32_t ok = (!(lw == 1920u && lh == 1080u) && fn && fn(ctx, &lr)) ? 1u : 0u;
    n48_agdc_timing_for(lw, lh, ok ? &lr : 0, lt);
}

static inline int n48_agdc_fill_link_config_t(uint8_t *out, size_t len, const n48_agdc_timing *t)
{
    if (!out || !t || len != N48_AGDC_LINK_CONFIG_LEN) return 1;
    for (size_t i = 0; i < len; i++) out[i] = 0;   /* +0x08 bit0 stays 0 -> no __GatherAGDCLinkCapabilities gather */
    n48_le64(out, 0x2cu, t->pixel_clock);          /* PixelClock     (timing+0x28) */
    n48_le64(out, 0x34u, t->pixel_clock);          /* MinPixelClock  (timing+0x30) */
    n48_le64(out, 0x3cu, t->pixel_clock);          /* MaxPixelClock  (timing+0x38) */
    n48_le32(out, 0x44u, t->w);                    /* HorizontalActive — THE FIX (timing+0x40) */
    n48_le32(out, 0x48u, t->h_blank);              /* HorizontalBlanking        (timing+0x44) */
    n48_le32(out, 0x4cu, t->h_sync_off);           /* HorizontalSyncOffset      (timing+0x48) */
    n48_le32(out, 0x50u, t->h_sync_w);             /* HorizontalSyncPulseWidth  (timing+0x4c) */
    n48_le32(out, 0x54u, t->h);                    /* VerticalActive   — THE FIX (timing+0x50) */
    n48_le32(out, 0x58u, t->v_blank);              /* VerticalBlanking          (timing+0x54) */
    n48_le32(out, 0x5cu, t->v_sync_off);           /* VerticalSyncOffset        (timing+0x58) */
    n48_le32(out, 0x60u, t->v_sync_w);             /* VerticalSyncPulseWidth    (timing+0x5c) */
    return 0;
}

/* 0.0.513's entry, unchanged in its answers: the CEA timing for (w, h). */
static inline int n48_agdc_fill_link_config(uint8_t *out, size_t len, uint32_t w, uint32_t h)
{
    if (!out || len != N48_AGDC_LINK_CONFIG_LEN) return 1;
    n48_agdc_timing t;
    n48_agdc_timing_for(w, h, 0, &t);
    return n48_agdc_fill_link_config_t(out, len, &t);
}

/* The gating capability-type flag. __GatherAGDCDisplayPipelineCapabilities issues selector 0x711 once per scaler type,
 * writing a u64 0x8000000TT at reply+0x04; it switches on the low 32 bits (M4-AGDC-CAPABILITIES follow-up). Type 0x10
 * (Plane Scaler) is the FIRST-issued and the ONLY one whose reply gates the gather: it reads reply+0x08 (entry count) and
 * returns 0x2006 if it is 0 (0x7ff81599cde7 test eax,eax; 0x7ff81599cdef je 0xdd8b). */
#define N48_AGDC_SCALER_TYPE 0x10u

/* The INLINE BUFFER capability type (0.0.324, an earlier analysis). __GatherAGDCDisplayPipelineCapabilities
 * queries it at 0x7ff81599d24b with `movabsq $0x800000040` -> reply+0x04 = 0x40 (type), reply+0x08 = 8
 * (a capacity hint it then READS BACK as our entry count). Logged by Apple as "Info: AGDC IB successful".
 * With our old zero fill the count came back 0, so IOPresentmentCreateInlineBufferDictionary bailed at
 * 0x7ff81599add1 without writing a key, and __inlineBufferFromDictionary returned 0x2004 - the code
 * measured on hardware in run iop2 . Record layout decoded from the builder's own
 * loop (0x7ff81599addf-0x7ff81599afb3), all offsets relative to the reply buffer:
 *   +0x08 entry count                            +0x18 MaxHeight  (SInt32, MUST be non-zero)
 *   +0x0c Flags (& 0x803fffff)                   +0x1c MaxWidth   (SInt32, MUST be non-zero)
 *   +0x10 latency (CFNumberType 0xc = Float32)   +0x20 PixelFormats (SInt64)
 *   +0x14 latency (Float32)                      stride 0x32c
 * Both +0x18 and +0x1c are tested: `cmpl $0,-0x8; je skip; cmpl $0,-0x4; jne process`. */
#define N48_AGDC_IB_TYPE     0x40u
#define N48_AGDC_IB_STRIDE   0x32cu
/* 1.0f as an IEEE-754 bit pattern: the kernel has no FPU, so the float is written as an integer. */
#define N48_AGDC_IB_LATENCY  0x3f800000u

/* little-endian u32 read (matches n48_le32's byte order); named apart from gfx_neuter.h's n48_rd32 (same TU includes both) */
static inline uint32_t n48_agdc_rd32(const uint8_t *p, uint32_t off)
{
    return (uint32_t)p[off] | ((uint32_t)p[off + 1u] << 8) | ((uint32_t)p[off + 2u] << 16) | ((uint32_t)p[off + 3u] << 24);
}

/* display-pipeline/scaler caps (selector 0x711), 0x196c bytes via IOConnectCallStructMethod. IOPresentment prewrites the
 * capability-type flag at reply+0x04 and calls with in==out (the same buffer), so `type` is read from the OUT buffer
 * BEFORE zeroing. For the type-0x10 (Plane Scaler) query, mirror AMD's getFbCapabilityForScaler (0xbf711df): write
 * reply+0x08 = 1 (the count that clears 0x2006) and the one scaler entry (base reply+0x0c) from the live mode w x h,
 * exactly as AMD's producer fills it ( follow-up, every field CONFIRMED from AMD's bytes). count=1 is the only
 * load-bearing field; the entry body de-risks the device-dictionary builder. Other types are zero-filled and return 0
 * (the gather issues them best-effort after type 0x10 succeeds, and their returns do not gate). Returns 1 on a wrong
 * length or null buffer, else 0. */
static inline int n48_agdc_fill_pipeline_caps(uint8_t *out, size_t len, uint32_t w, uint32_t h)
{
    if (!out || len != N48_AGDC_PIPELINE_CAPS_LEN) return 1;
    const uint32_t type = n48_agdc_rd32(out, 0x04u);   /* read the input type flag BEFORE zeroing (in==out) */
    for (size_t i = 0; i < len; i++) out[i] = 0;
    if (type == N48_AGDC_IB_TYPE) {
        /* One well-formed inline-buffer entry. count != 0 clears the builder's bail at 0x7ff81599add1;
         * MaxHeight and MaxWidth non-zero keep the entry from being skipped; the three keys the parser
         * requires (ActivationLatency, MaxWidth, PixelFormats) then all get written. */
        n48_le32(out, 0x08u, 1u);                      /* entry count */
        n48_le32(out, 0x0cu, 1u);                      /* Flags, masked 0x803fffff by the builder */
        n48_le32(out, 0x10u, N48_AGDC_IB_LATENCY);     /* Float32 latency */
        n48_le32(out, 0x14u, N48_AGDC_IB_LATENCY);     /* Float32 latency */
        n48_le32(out, 0x18u, h);                       /* MaxHeight - tested non-zero */
        n48_le32(out, 0x1cu, w);                       /* MaxWidth  - tested non-zero */
        n48_le64(out, 0x20u, 1u);                      /* PixelFormats (SInt64) */
        return 0;
    }
    if (type == N48_AGDC_SCALER_TYPE) {
        n48_le32(out, 0x08u, 1u);            /* entry count — the ONE field that clears 0x2006 (0xbf711df *count=1) */
        n48_le32(out, 0x0cu, 0x00080004u);   /* entry+0x00 scaler type/format (0xbf711f5) */
        n48_le32(out, 0x28u, 1u);            /* entry+0x1c  count-ish = 1 */
        n48_le32(out, 0x30u, w);             /* entry+0x24  width  */
        n48_le32(out, 0x34u, h);             /* entry+0x28  height */
        n48_le32(out, 0x38u, w);             /* entry+0x2c  width  */
        n48_le32(out, 0x3cu, h);             /* entry+0x30  height */
        n48_le32(out, 0x40u, 0x00050005u);   /* entry+0x34  marker */
        n48_le32(out, 0x44u, 4u);            /* entry+0x38  marker */
        n48_le32(out, 0x1b0u, 1u);           /* entry+0x1a4 count-ish = 1 */
        n48_le32(out, 0x1b8u, w);            /* entry+0x1ac width  */
        n48_le32(out, 0x1bcu, h);            /* entry+0x1b0 height */
        n48_le32(out, 0x1c0u, w);            /* entry+0x1b4 width  */
        n48_le32(out, 0x1c4u, h);            /* entry+0x1b8 height */
        n48_le32(out, 0x1c8u, 0x00050005u);  /* entry+0x1bc marker */
        n48_le32(out, 0x1ccu, 4u);           /* entry+0x1c0 marker */
    }
    return 0;
}

/* ---- 3b. The AGDC hold (0.0.322) ---------------------------------------------------------------------------
 * A deliberate one-shot delay inside our own reply handler, to open an attach window that does not otherwise exist:
 * IOPresentment's gather runs in under a millisecond and the parse follows its last reply by 30-76 us, so
 * `dtrace -p` can never be scheduled into it. The reply BYTES are untouched; only the calling thread is delayed.
 *
 * Safety, CONFIRMED from bytes: AppleGraphicsDeviceControl contains no IOCommandGate, no workloop and no lock
 * of any kind across all 3227 of its instructions, and the call reaches us synchronously on the caller's own thread, so
 * the delay holds one thread and nothing of Apple's kernel display machinery.
 *
 * THE ONE WAY THIS GOES WRONG: agdc_vendor is ALSO called from AGDC::start (0x13d3c40e / 0x13d3c44f) in IOKit matching
 * context, where a stall would hold device matching. This decision therefore FAILS CLOSED - anything that is not a
 * positively identified WindowServer, including an empty or truncated name, returns 0 and does not hold. */
#define N48_AGDC_HOLD_TOKEN  "agdc-hold 0.0.322"
#define N48_AGDC_HOLD_PROC   "WindowServer"
#define N48_AGDC_HOLD_MAX_MS 5000u

/* Returns the number of milliseconds to hold, or 0 for "do not hold".
 * `fired` is the one-shot latch; the CALLER MUST SET IT BEFORE SLEEPING, so that a second thread arriving during the
 * sleep sees it already set. A latch set after the sleep is the mutant the host test plants. */
static inline uint32_t n48_agdc_hold_ms(uint32_t armedMs, uint32_t fired, uint32_t cmd,
                                        const char *procName, uint32_t nameCap)
{
    static const char want[] = N48_AGDC_HOLD_PROC;
    unsigned i = 0;
    if (armedMs == 0u || armedMs > N48_AGDC_HOLD_MAX_MS) return 0u;  /* disarmed, or an unreasonable value */
    if (fired) return 0u;                                            /* one-shot: already held this boot */
    if (cmd != N48_AGDC_CMD_LINK_CONFIG) return 0u;                  /* only the selector that opens a gather */
    if (!procName || nameCap == 0u) return 0u;                       /* no identification -> DO NOT HOLD */
    for (; i < (unsigned)(sizeof(want) - 1u); i++) {
        if (i >= nameCap) return 0u;                                 /* name buffer shorter than the wanted name */
        if (procName[i] != want[i]) return 0u;                       /* any mismatch, including an empty name */
    }
    if (i >= nameCap || procName[i] != '\0') return 0u;              /* a LONGER name must not match on its prefix */
    return armedMs;
}

/* ---- 3c. The command-queue probe (0.0.325, notes sections 617-618) ------------------------------------------------------
 * READ-ONLY. Answers which of the six blocking points of IOAccelCommandQueue2::submit_command_buffers
 * (0x145c6e00, decoded in section 617) a thread is at, so "parked at (4) with the lock RELEASED" can be told from
 * "stuck at (6) holding it" - two states implying opposite fixes.
 *
 * The discriminator is a pure read, and that is why no hook is needed (CONFIRMED from bytes):
 *   0x145c6e9a  movb $0x1, 0x615(%rbx)   set on ENTRY, after the re-entrancy test
 *   0x145c6eee  movb $0x1, 0x616(%rbx)   set once acceleratorWaitEnabled() has been passed
 *   0x145c7140  movw $0x0, 0x615(%rbx)   ONE 16-bit store clears BOTH on every exit path
 * So (615,616) == (0,0) idle; (1,0) inside but not past the enable check; (1,1) inside and past it.
 * (0,1) is IMPOSSIBLE - the exit clears them together - and is reported as INCONSISTENT rather than guessed.
 *
 * The queue list (IOAccelCommandQueueList::getCountWithPID 0x1459de42, insert at 0x1459de8e):
 *   accel+0xa68 = the list;  list+0x00 = head;  list+0x08 = count (u32);  queue+0x568 = next
 * and every queue carries queue+0x5c0 = its accelerator, which submit_command_buffers itself uses
 * (0x145c6e17 movq 0x5c0(%rdi),%r14) - so it is the identity check for a walked entry. */
#define N48_CQ_LIST_OFF     0xa68u   /* accel -> IOAccelCommandQueueList */
#define N48_CQ_LIST_HEAD    0x00u
#define N48_CQ_LIST_COUNT   0x08u
#define N48_CQ_NEXT_OFF     0x568u   /* queue -> next queue */
#define N48_CQ_ACCEL_OFF    0x5c0u   /* queue -> accelerator (the identity check) */
#define N48_CQ_F615_OFF     0x615u   /* "inside submit_command_buffers" */
#define N48_CQ_F616_OFF     0x616u   /* "past acceleratorWaitEnabled" */
#define N48_CQ_MAX_WALK     32u      /* a hard bound: a corrupt list must never spin the kernel */

enum { N48_CQ_IDLE = 0, N48_CQ_INSIDE = 1, N48_CQ_INSIDE_PAST_ENABLE = 2, N48_CQ_INCONSISTENT = 3 };

/* Pure classifier. (0,1) cannot happen because one 16-bit store clears both, so it is reported, not guessed. */
static inline uint32_t n48_cq_state(uint8_t f615, uint8_t f616)
{
    if (!f615) return f616 ? (uint32_t)N48_CQ_INCONSISTENT : (uint32_t)N48_CQ_IDLE;
    return f616 ? (uint32_t)N48_CQ_INSIDE_PAST_ENABLE : (uint32_t)N48_CQ_INSIDE;
}

/* How many list entries it is safe to walk: 0 when the list is empty or the count is implausible. */
static inline uint32_t n48_cq_walk_bound(uint32_t count)
{
    if (count == 0u || count > N48_CQ_MAX_WALK) return 0u;
    return count;
}

/* ---- 4. Route A (0.0.295) — the kext object slot 267 returns, and the resource-class slot-46 guard ----------------------
 * Corrected route A (an internal review note). The safety core's slot-267 NULL drops pipe+0x298 to 0
 * when WindowServer re-runs display init; route A instead returns a kext-owned VidMemory-shaped object AND neutralises
 * AMDRadeonX6000_AMDAccelResource::prepare() on the framebuffer resource, so readiness persists without running AMD's
 * reserveFrameBuffer or prepare()'s pruneOrphanedMappings/channel tree.
 *
 * Every field/slot below is CONFIRMED from the bytes (this session, disassembled from the tahoe-26.6.2 extraction):
 *   init_framebuffer_resource (IOAcceleratorFamily2):
 *     0x145cc54c  orb $0x20, 0xc(%r12)          -> object +0xc must be writable
 *     0x145cc552  movq %r12, 0x88(%rbx)         -> object stored at resource+0x88
 *     0x145cc559  movq (%rbx),%rax; movq %rbx,%rdi; callq *0x170(%rax)   -> slot 46 (prepare) on the RESOURCE (this=rbx)
 *     0x145cc569  movb $0x1, 0x298(%r14)        -> readiness set iff slot 46 returned true
 *   destroy_framebuffer_resource (IOAcceleratorFamily2), rdi = resource+0x88 = our object:
 *     0x145cc211  testb $0x10, 0xd(%rdi)        -> object +0xd bit 0x10 MUST be set, else...
 *     0x145cc217  callq IOAccelMemory::pruneOrphanedMappings   (the hazard, run on our object if +0xd clear)
 *     0x145cc226  movq %r15(=0), 0x38(%rdi)     -> object +0x38 must be writable
 *     0x145cc22d  movq (%rdi),%rax; callq *0x28(%rax)          -> slot 5 (release) on our object
 *   shim_plane_placement (DisplayPipeGuard.cpp): reads object +0x40 as length, calls slot 43 (*0x158)
 *     getPhysicalSegment(0,&span) and requires phys != 0 && span >= len.
 *   AMDGFX10Resource vtable @0xbf26608 (vptr 0xbf26618), 111 slots; slot 46 (*0x170) = AMDAccelResource::prepare 0xbdd6198.
 */
#define N48_RA_TOKEN         "route-a 0.0.295"
/* The kext object slot 267 returns. */
#define N48_RA_OBJ_SIZE      0x80u    /* >= 0x48 (the reviewer minimum); zeroed at allocation */
#define N48_RA_OFF_VTABLE    0x00u
#define N48_RA_OFF_FLAGC     0x0cu    /* init does orb $0x20 here */
#define N48_RA_OFF_FLAGD     0x0du    /* destroy tests bit 0x10 here */
#define N48_RA_OFF_F38       0x38u    /* destroy writes 0 here */
#define N48_RA_OFF_LEN       0x40u    /* shim reads the VidMemory length here */
#define N48_RA_FLAGD_VALUE   0x10u
/* The object's kext-owned vtable: >= 44 slots so *0x28 (5) and *0x158 (43) are valid; other slots are a benign stub. */
#define N48_RA_VT_SLOTS      48u
#define N48_RA_SLOT_RELEASE  5u       /* *0x28 — destroy calls it as release (must free nothing AMD-owned) */
#define N48_RA_SLOT_PHYSSEG  43u      /* *0x158 — shim calls getPhysicalSegment(0,&span) */
/* The scanout placement the object advertises. Captured live from RDNA4FB's Console,* at arm time; these are this card's
 * values (getVRAMRange returns IODeviceMemory::withRange(0xC0000000, 0x800000)) and match the reviewer's spec. */
#define N48_RA_PHYS_DEFAULT  0xC0000000ull
#define N48_RA_LEN_DEFAULT   0x800000ull
/* The resource-class slot-46 guard target. NOTE (routeA1,): pipe+0xe0 carries a PER-INSTANCE HEAP vtable on this
 * card, not this static table, so N48_RA_RES_VPTR is informational only - the arm-time identity is "kind-of
 * IOAccelResource2 AND slot 46 == prepare", never the static vtable address. N48_RA_RES_SLOTS is the copy length; the
 * X6000 resource classes (AMDGFX10Resource, AMDAccelResourceAddr2) are both 111 slots. */
#define N48_RA_RES_VPTR      0x0bf26618u   /* AMDGFX10Resource static vtable symbol 0xbf26608 + 0x10 (reference only) */
#define N48_RA_RES_SLOTS     111u          /* 0xbf26990 (MetaClass) - 0xbf26618 = 0x378 = 111 * 8 */
#define N48_RA_PREPARE_SLOT  46u           /* *0x170 */
#define N48_RA_PREPARE       0x0bdd6198u   /* AMDRadeonX6000_AMDAccelResource::prepare() */

/* Lay out the object's non-vtable fields into a zeroed buffer of >= N48_RA_OBJ_SIZE bytes (pure; the vtable pointer at
 * +0x0 is set by the kext with the real kernel table). Little-endian, integer-only (kernel: no SSE/FPU). */
static inline void n48_ra_fill_object(uint8_t *o, uint64_t len)
{
    o[N48_RA_OFF_FLAGC] = 0;                       /* init will orb $0x20 onto it */
    o[N48_RA_OFF_FLAGD] = (uint8_t)N48_RA_FLAGD_VALUE;
    for (unsigned i = 0; i < 8u; i++) o[N48_RA_OFF_F38 + i] = 0;
    for (unsigned i = 0; i < 8u; i++) o[N48_RA_OFF_LEN + i] = (uint8_t)(len >> (8u * i));
}
/* 0 iff the object's fields are well-formed for `len`, else a 1-based failure code (host test / mutants). */
static inline unsigned n48_ra_object_check(const uint8_t *o, uint64_t len)
{
    if (o[N48_RA_OFF_FLAGD] != (uint8_t)N48_RA_FLAGD_VALUE) return 1;   /* prune-skip bit */
    uint64_t l = 0;
    for (unsigned i = 0; i < 8u; i++) l |= (uint64_t)o[N48_RA_OFF_LEN + i] << (8u * i);
    if (l != len) return 2;                                            /* length mismatch */
    return 0;
}
/* getPhysicalSegment math (pure): returns phys, sets *span = len. */
static inline uint64_t n48_ra_physseg(uint64_t phys, uint64_t len, uint64_t *span)
{
    if (span) *span = len;
    return phys;
}

/* ---------------------------------------------------------------------------------------------------------------------
 * 0.0.373 — WHICH BUCKET A PRESENT GOES IN. The defect, closed.
 *
 * Through 0.0.372 dpg_perform counted `if (cst == 0) copyOk++; else copyRefused++;`, and kScanStReadback (11) went in
 * with the refusals. It is not a refusal. That status means the SDMA tiled copy was built, submitted and FENCED - the
 * pixels are on the scanout - and then OUR OWN detile-equation verification of rows 0 and h-1 disagreed with the
 * source. arm3 is what it cost: the run reported "presents ok 6 / refused 1" for a boot that performed
 * fifteen real copies, fourteen clean and one whose verification differed, and a report of many frames
 * was treated as the thing needing explanation.
 *
 * DELIVERED is ok + readback-differed. REFUSED means no copy was made. */
enum { N48_DPG_PRESENT_OK = 0, N48_DPG_PRESENT_READBACK = 1, N48_DPG_PRESENT_REFUSED = 2 };
/* navi48_scanout_copy_*'s kScanStReadback, src/Navi48Bringup.cpp:1040 (a file-local enum there). */
#define N48_DPG_SCAN_ST_READBACK 11u

static inline uint32_t n48_dpg_present_bucket(uint32_t cst)
{
    if (cst == 0u) return N48_DPG_PRESENT_OK;
    if (cst == N48_DPG_SCAN_ST_READBACK) return N48_DPG_PRESENT_READBACK;
    return N48_DPG_PRESENT_REFUSED;
}

/* The count of presents whose pixels reached the scanout. */
static inline uint64_t n48_dpg_delivered(uint64_t ok, uint64_t readback) { return ok + readback; }

static inline const char *n48_dpg_bucket_name(uint32_t b)
{
    return b == N48_DPG_PRESENT_OK ? "ok"
         : b == N48_DPG_PRESENT_READBACK ? "readback-differed (the copy landed; our own verification disagreed)"
         : "REFUSED (no copy was made)";
}

/* ---------------------------------------------------------------------------------------------------------------------
 * 0.0.412 — S2: THE 16x16 VERIFY CELL MAP AND ITS PER-BOOT BOUND.
 *
 *'s design: the present path's readback ran ONCE (present #1). S2 runs it on the FIRST PRESENT AFTER EACH CHANGE of
 * the presented plane's content token, and reports WHERE the destination disagrees with the source, not just how many.
 * "Content token" here is the plane's VRAM byte offset — WindowServer presents a different buffer by naming a different
 * offset in the plane array, and `dpg_perform` already reads that offset for the copy and the arrangement probe. No new
 * source of truth is introduced; the token is `phys`, a value the present path already holds.
 *
 * The map is 16x16 cells over the DESTINATION rectangle; ONE sample is read at each cell's centre (256 samples), the
 * source at the same geometric position and the destination through BAR0 (both existing read paths — Navi48Bringup.cpp's
 * two copy routines). The cell index, the centre coordinates, the 256-bit bit packing and the "is it due?" predicate are
 * pure and host-tested (tests/pipeshim_verify_test.cpp); the reads are not.
 *
 * THE BOUND IS A CEILING, NOT A RATE: at most N48_DPG_VERIFY_BUDGET S2 map lines can ever be printed in one boot, so a
 * run that presents thousands of frames still produces a bounded log. (The independent copy-census budget is
 * kShimCensusLogBudget; the heartbeat is 60.)
 */
#define N48_DPG_VERIFY_CELLS     16u                                   /* cells per side                       */
#define N48_DPG_VERIFY_SAMPLES   (N48_DPG_VERIFY_CELLS * N48_DPG_VERIFY_CELLS)   /* 256 sampled pixels      */
#define N48_DPG_VERIFY_MAP_WORDS 4u                                    /* 256 bits                             */
#define N48_DPG_VERIFY_FIRST     8u                                    /* differing samples given both values  */
#define N48_DPG_VERIFY_BUDGET    48u                                   /* map lines per boot, hard ceiling     */

/* Row-major cell index of destination pixel (x,y) in a w x h rectangle: 0..255. Out-of-rect clamps to the last cell. */
static inline uint32_t n48_dpg_cell16(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (w == 0u || h == 0u) return 0u;
    uint64_t cx = ((uint64_t)x * N48_DPG_VERIFY_CELLS) / w;
    uint64_t cy = ((uint64_t)y * N48_DPG_VERIFY_CELLS) / h;
    if (cx >= N48_DPG_VERIFY_CELLS) cx = N48_DPG_VERIFY_CELLS - 1u;
    if (cy >= N48_DPG_VERIFY_CELLS) cy = N48_DPG_VERIFY_CELLS - 1u;
    return (uint32_t)(cy * N48_DPG_VERIFY_CELLS + cx);
}

/* The centre pixel of cell (cx,cy) in a w x h rectangle, clamped inside it. cell == 0..15. */
static inline void n48_dpg_cell16_centre(uint32_t cx, uint32_t cy, uint32_t w, uint32_t h, uint32_t *x, uint32_t *y)
{
    uint64_t px = (((uint64_t)(2u * cx + 1u) * w) / (2u * N48_DPG_VERIFY_CELLS));
    uint64_t py = (((uint64_t)(2u * cy + 1u) * h) / (2u * N48_DPG_VERIFY_CELLS));
    if (w && px >= w) px = w - 1u;
    if (h && py >= h) py = h - 1u;
    if (x) *x = (uint32_t)px;
    if (y) *y = (uint32_t)py;
}

static inline void n48_dpg_verify_map_set(uint64_t *map, uint32_t cell)
{
    if (map && cell < N48_DPG_VERIFY_SAMPLES) map[cell >> 6] |= (uint64_t)1u << (cell & 63u);
}
static inline int n48_dpg_verify_map_get(const uint64_t *map, uint32_t cell)
{
    if (!map || cell >= N48_DPG_VERIFY_SAMPLES) return 0;
    return (int)((map[cell >> 6] >> (cell & 63u)) & 1u);
}

/* S2's trigger: the content token changed, and the boot's map-line budget is not spent. runs == 0 fires on the FIRST
 * present whatever the token, so a plane at offset 0 still verifies. A re-assert of the same mode does not reset it. */
static inline int n48_dpg_verify_due(uint64_t token, uint64_t lastToken, uint32_t runs)
{
    return runs < N48_DPG_VERIFY_BUDGET && (runs == 0u || token != lastToken);
}

#endif
