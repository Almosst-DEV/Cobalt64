//
//  Navi48Ttl.cpp — see Navi48Ttl.hpp.
//
//  Every method logs. That is the point of the first iteration: the accelerator's
//  call order and arguments are the measurement, and the log ring survives a boot
//  that never reaches a login window (read it with `navi48test log`).
//
#include "Navi48Ttl.hpp"
#include "../Navi48Bringup.hpp"
#include "../amd/amdgpu_regs.h"
#include "../amd/amdgpu_init.h"
#include "../amd/amdgpu_gfx.h"
#include "../amd/amdgpu_gmc.h"
#include "../amd/n48log.h"
#include "../ipdiscovery.hpp"
#include "AppleHardwareHook.hpp"
#include "Navi48AccelPeer.hpp"

#define TTLLOG(fmt, ...) ::amdgpu::n48_logf("Navi48Ttl: " fmt "\n", ##__VA_ARGS__)

Navi48Ttl gNavi48Ttl;

Navi48Ttl::~Navi48Ttl() {}

void Navi48Ttl::bind(Navi48Bringup *owner) {
    mOwner = owner;
    mDev   = owner ? owner->deviceContext()  : nullptr;
    mCtx   = owner ? owner->bringupContext() : nullptr;
    TTLLOG("bound to Navi48Bringup %p (dev %p ctx %p)", owner, mDev, mCtx);
    dumpIpTable();
}

// Apple asks for register bases by an hwblock_type we have only partly decoded:
// 11 is confirmed to be GC (it resolved to v12.0.1 on hardware), but the other
// ids it uses (7, 66, 75 at the configureRegisterBases call sites) match nothing
// on this card. Printing what the card actually has is how that gets resolved,
// so dump the whole table once and let the ids speak for themselves.
void Navi48Ttl::dumpIpTable() const {
    if (!mOwner) return;
    const IpDiscovery &d = mOwner->ipDisc();
    if (!d.isValid()) { TTLLOG("IP discovery is not valid - cannot serve register bases"); return; }
    TTLLOG("--- IP discovery: %u blocks (hwId is what Apple calls hwblock_type) ---", d.ipCount());
    for (uint16_t i = 0; i < d.ipCount(); i++) {
        IpDiscovery::IpEntry e {};
        if (!d.ipAt(i, e)) continue;
        TTLLOG("  hwId %3u inst %u  v%u.%u.%u  %u segs: %08x %08x %08x %08x %08x %08x",
               e.hwId, e.instance, e.major, e.minor, e.revision, e.numBases,
               e.numBases > 0 ? e.bases[0] : 0, e.numBases > 1 ? e.bases[1] : 0,
               e.numBases > 2 ? e.bases[2] : 0, e.numBases > 3 ? e.bases[3] : 0,
               e.numBases > 4 ? e.bases[4] : 0, e.numBases > 5 ? e.bases[5] : 0);
    }
    TTLLOG("--- end IP discovery ---");
}

uint32_t Navi48Ttl::callCount(int slot) const {
    return (slot >= 0 && slot < 43) ? mCalls[slot] : 0;
}

uint32_t Navi48Ttl::note(int slot, const char *name, void *ret) const {
    // Retry the accelerator stop-patch on EVERY TTL entry.
    //
    // It cannot hang off initialize(): measured on hardware, that call comes
    // from HWServices before the accelerator object exists at all ("no
    // AMDGraphicsAccelerator in the service plane yet"). Calls such as
    // getLocalMemoryInfo come from X6000 during the accelerator's own start(),
    // which is exactly the window needed. Rather than guess which method that
    // is, try on all of them — once the patch lands it is a single branch.
    Navi48AccelPeer_TryPatchAcceleratorStop();
    if (slot >= 0 && slot < 43) mCalls[slot]++;
    mTotalCalls++;
    // Report the caller as an address in the X6000 binary we disassembled, so a
    // log line can be looked up directly in the static analysis.
    if (mX6000Slide && ret) {
        const uint64_t r = (uint64_t)ret - mX6000Slide;
        TTLLOG("[%02d] %s   <- X6000+0x%llx", slot, name, r);
    }
    return kTtlOk;
}

uint32_t Navi48Ttl::unsupported(int slot, const char *name, void *ret) const {
    note(slot, name, ret);
    if (mFirstUnsupported < 0) mFirstUnsupported = slot;
    TTLLOG("[%02d] %s -> UNSUPPORTED (not implemented yet)", slot, name);
    return kTtlUnsupported;
}

// ---------------------------------------------------------------- slot 6
// The failure the whole experiment is aimed at. Apple's TTL dies here because
// its device table stops at Navi 23; ours succeeds because the bring-up ladder
// has already done everything TTL::initialize() would have done — PSP, SMU,
// GMC/GART, IH, GFX, SDMA, MES, CP queues — before the accelerator ever loaded.
//
// So this does not bring hardware up. It records what the accelerator handed us
// and reports whether the ladder is actually in the state we are claiming.
uint32_t Navi48Ttl::initialize(TtlLibraryInitializationInput *in) {
    NOTE(6, "initialize");
    if (!in) { TTLLOG("[06] initialize: NULL input — refusing"); return kTtlError; }
    if (!mCtx || !mDev) { TTLLOG("[06] initialize: not bound to a bring-up instance"); return kTtlError; }

    // AMDRadeonX6000_AMDRTHardware::accelGmmCbAllocateVram is at 0xbe19e14 in the
    // shipping binary; the accelerator just handed us its runtime address. The
    // difference is X6000's load slide, which turns every later return address
    // into something we can look up in the disassembly.
    static constexpr uint64_t kStaticAccelGmmCbAllocateVram = 0xbe19e14ULL;
    if (in->pGmmCallbacks && in->pGmmCallbacks->allocateVram) {
        mX6000Slide = (uint64_t)in->pGmmCallbacks->allocateVram - kStaticAccelGmmCbAllocateVram;
        TTLLOG("[06] X6000 load slide = 0x%llx (alloc callback %p maps to static 0x%llx)",
               mX6000Slide, (void *)in->pGmmCallbacks->allocateVram,
               kStaticAccelGmmCbAllocateVram);
    }

    mGmmCb         = in->pGmmCallbacks;
    mLogCb         = in->pEventLogCallbacks;
    mDoorbellBase  = in->pDoorbellBase;
    mDoorbellBytes = in->doorbellApertureSizeInBytes;
    mNonlocalLimit = in->nonlocalMemSizeLimitBytes;

    TTLLOG("[06] initialize: gmmCb=%p eventLogCb=%p doorbellBase=%p doorbellBytes=%u (%u KiB) nonlocalLimit=0x%llx",
           mGmmCb, mLogCb, mDoorbellBase, mDoorbellBytes, mDoorbellBytes >> 10, mNonlocalLimit);
    if (mGmmCb) TTLLOG("[06]   gmm callbacks: ctx=%p alloc=%p release=%p",
                       mGmmCb->context, (void *)mGmmCb->allocateVram, (void *)mGmmCb->releaseVram);

    // pGmmCallbacks->context IS Apple's RTHardware instance — initializeTtl builds
    // that block out of `this`. This is the only place we get a handle on it, and
    // we need one: further down the start sequence Apple's GART code writes gfx12
    // memory-controller registers using GFX10 offsets and then polls an ACK that
    // can never set. That is an unbounded kernel spin. It hard-hung this machine
    // on 0.0.52 and needed a power cycle. Neutralise it before it can run.
    if (mGmmCb && mGmmCb->context) {
        n48::HwHookResult hw = n48::install_hardware_hooks(mGmmCb->context);
        if (!hw.installed)
            TTLLOG("[06] WARNING: hardware hooks NOT installed (%s). Apple's GART path can "
                   "hard-hang this machine; do not let it run unattended.", hw.why);
    } else {
        TTLLOG("[06] WARNING: no gmm context - cannot install the hardware hooks");
    }

    // Report, do not assume. If the ladder is not where we think it is, say so
    // in the log and still return success, because the next failure address is
    // what this boot is for.
    const int reached = (int)mCtx->reached;
    TTLLOG("[06] bring-up state: stage reached %d, CP=%d MES=%d SDMA=%d gfxhub=%d compute=%d",
           reached, mCtx->cpRingTestPassed, !mCtx->mesFailed, mCtx->sdmaCopyPassed,
           mCtx->gfxhubReady, mCtx->computePassed);
    if (reached < (int)amdgpu::BringupStage::ComputeDispatch)
        TTLLOG("[06] WARNING: ladder did not reach ComputeDispatch — the accelerator is about "
               "to drive hardware we have not finished bringing up");

    mInitialized = true;
    TTLLOG("[06] initialize -> OK");
    return kTtlOk;
}

uint32_t Navi48Ttl::uninitialize() {
    NOTE(7, "uninitialize");
    TTLLOG("[07] uninitialize");
    mInitialized = false;
    if (mAppleGartTable.valid()) {
        amdgpu::sysmem_free(mAppleGartTable);
        TTLLOG("[07] released the scratch GART table");
    }
    return kTtlOk;
}

// ---------------------------------------------------------------- slots 9,10,40
// The ladder leaves the GPU powered and clocked; there is nothing to raise.
// Returning success here is honest: the hardware IS up.
uint32_t Navi48Ttl::powerUp() {
    NOTE(9, "powerUp");
    TTLLOG("[09] powerUp -> OK (ladder already has the GPU running)");
    return kTtlOk;
}
uint32_t Navi48Ttl::powerDown() {
    NOTE(10, "powerDown");
    TTLLOG("[10] powerDown -> OK (no-op; the console is still scanning out of VRAM)");
    return kTtlOk;
}
uint32_t Navi48Ttl::notifyHardwareState(bool up) {
    NOTE(40, "notifyHardwareState");
    TTLLOG("[40] notifyHardwareState(%d)", up ? 1 : 0);
    return kTtlOk;
}

// ---------------------------------------------------------------- slot 5
uint32_t Navi48Ttl::getSwipErrorString(char *buf, unsigned long len) {
    NOTE(5, "getSwipErrorString");
    if (!buf || len == 0) return kTtlError;
    const char *msg = "Navi48Ttl: no SWIP error log (this is not Apple's TTL)";
    unsigned long i = 0;
    while (i + 1 < len && msg[i]) { buf[i] = msg[i]; i++; }
    buf[i] = '\0';
    TTLLOG("[05] getSwipErrorString(len=%lu)", len);
    return kTtlOk;
}

// ---------------------------------------------------------------- slots 11,12
// The heaviest-used pair: 19 and 11 call sites. This is IP discovery, which the
// bring-up kext already parses off the die, so we are simply re-publishing it.
//
// SIZE IS LOAD-BEARING. The accelerator stores the results into fixed arrays
// inside its own Hardware object, 6 dwords apart (0x206c4, 0x206dc, 0x206f4 …).
// Returning a count above 6 writes past the end of one block's array and into
// the next. Clamp, and never "improve" this number.
static constexpr unsigned kAppleHwipSegments = 6;

// Apple's hwblock_type is NOT AMD's discovery HW_ID. HWLibs translates between
// them in _get_hw_block_type() @ 0xc4aac10, a 170-entry jump table from HW_ID to
// hwblock_type. This is that table inverted, decoded from the shipping binary.
//
// The two numbering schemes agree at 11 (GC) and nowhere else that matters,
// which is why an identity mapping appeared to work for GC and then failed on
// the very next call. The four blocks the accelerator asks for during
// configureRegisterBases are 7, 11, 66 and 75 -> SMUIO, GC, NBIF and MP0/PSP,
// all present on this card.
struct HwBlockMap { uint16_t appleBlock; uint16_t discoveryHwId; };
static const HwBlockMap kHwBlockMap[] = {
    {  4,   1 }, {  5,   2 }, {  6,   3 }, {  7,   4 },   // MP1, -, SMUIO, FUSE
    {  8,   5 }, {  9,   6 }, { 10,  10 }, { 11,  11 },   // CLKA, CLKB, PWR, GC
    { 12,  12 }, { 13,  13 }, { 14,  14 }, { 15,  15 },
    { 17,  16 }, { 19,  17 }, { 20,  18 }, { 21, 274 },
    { 22,  19 }, { 23,  20 }, { 24,  24 }, { 25,  28 },
    { 26,  32 }, { 27,  34 }, { 28,  35 }, { 29,  36 },   // MMHUB, ATHUB, DBGU_NBIO
    { 30,  37 }, { 31,  38 }, { 32,  39 }, { 33,  40 },   // DFX, OSSSYS
    { 34,  41 }, { 35,  42 }, { 36,  43 }, { 37,  44 },   // HDP, SDMA0, SDMA1
    { 38,  45 }, { 39,  46 }, { 40,  47 }, { 41,  48 },   // DF
    { 42,  49 }, { 43,  50 }, { 44,  51 }, { 45,  52 },
    { 46,  53 }, { 47,  54 }, { 48,  55 }, { 49,  56 },
    { 50,  57 }, { 51,  58 }, { 52,  59 }, { 53,  60 },
    { 54,  61 }, { 55,  62 }, { 56,  63 }, { 57,  64 },
    { 58,  65 }, { 59,  66 }, { 60,  67 }, { 61,  70 },   // PCIE
    { 62,  80 }, { 63,  89 }, { 64,  90 }, { 65, 100 },
    { 66, 108 }, { 67, 124 }, { 68, 128 }, { 69, 144 },   // NBIF
    { 70, 150 }, { 71, 168 }, { 72, 170 }, { 75, 255 },   // UMC, MP0/PSP
    { 76,  68 }, { 77,  69 }, { 79,  91 },
};

static bool apple_block_to_hwid(uint32_t block, uint16_t &hwId) {
    for (unsigned i = 0; i < sizeof(kHwBlockMap) / sizeof(kHwBlockMap[0]); i++)
        if (kHwBlockMap[i].appleBlock == block) { hwId = kHwBlockMap[i].discoveryHwId; return true; }
    return false;
}


unsigned int Navi48Ttl::getHwipSegmentCount() const {
    NOTE(12, "getHwipSegmentCount");
    TTLLOG("[12] getHwipSegmentCount -> %u", kAppleHwipSegments);
    return kAppleHwipSegments;
}

uint32_t Navi48Ttl::queryHwBlockRegisterBase(hwblock_type block, unsigned char instance,
                                             unsigned int segment, unsigned int *out) {
    NOTE(11, "queryHwBlockRegisterBase");
    if (!out) return kTtlError;
    *out = 0;
    if (!mOwner) return kTtlError;

    // Apple's own implementation bounds instance at 9 and segment at 5 and
    // returns 0xe00002c2 outside those. Match it.
    if (instance > 9 || segment >= kAppleHwipSegments) {
        TTLLOG("[11] block=%u inst=%u seg=%u -> out of range", block, instance, segment);
        return kTtlError;
    }

    // ABSENT IS NOT AN ERROR. Apple's implementation zeroes a 0x260-byte buffer,
    // asks its own IP-instance service to fill it, and then returns
    // buffer[instance].bases[segment] unconditionally. A block the card does not
    // have, or a segment the block does not use, simply reads back as 0 and the
    // call still succeeds. Returning an error instead aborts the accelerator's
    // whole configureRegisterBases loop — which is what happened at UMC segment 3
    // on 0.0.45, since this card reports only 3 segments for UMC.
    uint16_t hwId = 0;
    if (!apple_block_to_hwid(block, hwId)) {
        TTLLOG("[11] block=%u inst=%u seg=%u -> no HW_ID mapping; reporting base 0 (absent)",
               block, instance, segment);
        return kTtlOk;
    }

    IpDiscovery::IpEntry e {};
    if (!mOwner->ipDisc().findIp(hwId, instance, e)) {
        TTLLOG("[11] block=%u (HW_ID %u) inst=%u seg=%u -> not on this card; reporting base 0",
               block, hwId, instance, segment);
        return kTtlOk;
    }
    if (segment >= e.numBases) {
        TTLLOG("[11] block=%u (HW_ID %u v%u.%u.%u) inst=%u seg=%u -> block uses only %u segments; "
               "reporting base 0", block, hwId, e.major, e.minor, e.revision,
               instance, segment, e.numBases);
        return kTtlOk;
    }
    *out = e.bases[segment];
    TTLLOG("[11] block=%u (HW_ID %u v%u.%u.%u) inst=%u seg=%u -> 0x%08x",
           block, hwId, e.major, e.minor, e.revision, instance, segment, *out);
    return kTtlOk;
}

// ---------------------------------------------------------------- slot 13
// Return value is IGNORED by the caller (AMDHardware::initHWInfo), which passes
// &this->0x2c4 as the u64 and &this->0x2c0 as the u32. Clear-state buffer:
// address and size. We do not publish an RLC CSB yet, so report zero rather than
// pointing Apple's driver at memory we do not own.
uint32_t Navi48Ttl::queryCSBInfo(unsigned long long *addr, unsigned int *size) {
    NOTE(13, "queryCSBInfo");
    uint64_t mc = 0; uint32_t bytes = 0;
    const bool ok = navi48_csb_publish(&mc, &bytes);   // 0.0.181: a real CP-executable IB
    if (addr) *addr = ok ? mc : 0;
    if (size) *size = ok ? bytes : 0;
    TTLLOG("[13] queryCSBInfo -> %#llx / %u bytes (%s); Apple stores these at hw+0x2c4/0x2c0 "
           "and performClearState submits them as an INDIRECT_BUFFER",
           (unsigned long long)(ok ? mc : 0), ok ? bytes : 0u,
           ok ? "16-dword NOP IB in GART" : "GART not ready, 0/0 as before");
    return kTtlOk;
}

// ---------------------------------------------------------------- slot 14
// Layout recovered from GFX10Hardware::setupAndInitializeHWCapabilities:
//   0x00 u32 numShaderEngines        (outer loop bound, max 4 — the struct is 0x40 bytes)
//   0x08 u32 numShaderArraysPerSE    (inner loop bound)
//   0x18 u32 a bitmap the caller popcounts
//   0x20 u32 wgpBitmap[SE][SA]       (row stride 8 bytes)
// Each set bit of wgpBitmap is expanded to a pair of CU bits and the count is
// doubled, so the caller is reading WGPs, not CUs.
//
// Failing this clears the accelerator's "capabilities valid" flag at +0x30 and
// aborts the rest of setupAndInitializeHWCapabilities, so answer it for real.
struct GcHardwareInfoLayout {
    uint32_t numShaderEngines;      uint32_t pad0;
    uint32_t numShaderArraysPerSE;  uint32_t pad1;
    uint32_t unknown0x10;           uint32_t pad2;
    uint32_t popcountedBitmap;      uint32_t pad3;
    uint32_t wgpBitmap[4][2];
};
static_assert(sizeof(GcHardwareInfoLayout) == 0x40, "GcHardwareInfo must be 0x40 bytes");

uint32_t Navi48Ttl::queryGcHardwareInfo(GcHardwareInfo *raw) {
    NOTE(14, "queryGcHardwareInfo");
    if (!raw || !mCtx) return kTtlError;
    auto *o = reinterpret_cast<GcHardwareInfoLayout *>(raw);
    const amdgpu::GFXConfig &g = mCtx->gfx;

    uint32_t se = g.max_shader_engines ? g.max_shader_engines : 4;
    uint32_t sa = g.max_sh_per_se      ? g.max_sh_per_se      : 2;
    if (se > 4) se = 4;                    // the struct holds exactly 4 rows
    if (sa > 2) sa = 2;                    // ...of 2 entries
    o->numShaderEngines     = se;
    o->numShaderArraysPerSE = sa;
    o->popcountedBitmap     = g.global_active_rb_bitmap;

    // max_cu_per_sh counts CUs; the caller wants WGPs, which are CU pairs.
    uint32_t wgpPerSa = (g.max_cu_per_sh ? g.max_cu_per_sh : 8) / 2;
    if (wgpPerSa > 32) wgpPerSa = 32;
    const uint32_t fullMask = (wgpPerSa >= 32) ? 0xFFFFFFFFu : ((1u << wgpPerSa) - 1u);
    for (uint32_t i = 0; i < se; i++)
        for (uint32_t j = 0; j < sa; j++)
            o->wgpBitmap[i][j] = fullMask;

    TTLLOG("[14] queryGcHardwareInfo -> SE=%u SA/SE=%u WGP/SA=%u mask=0x%08x rbBitmap=0x%08x "
           "(=%u CUs)", se, sa, wgpPerSa, fullMask, o->popcountedBitmap, se * sa * wgpPerSa * 2);
    if (!g.inited)
        TTLLOG("[14] WARNING: GFXConfig was never populated — these are fallback constants");
    return kTtlOk;
}

// ---------------------------------------------------------------- slot 15
// 8 bytes, two u32s, copied to Hardware+0x1b8 and +0x1bc. The meaning of the
// values is not yet known, so report failure: initHWInfo simply skips the copy
// and carries on, which is better than writing a guess into the accelerator.
uint32_t Navi48Ttl::queryGpuMemType(GpuMemType *out) {
    NOTE(15, "queryGpuMemType");
    TTLLOG("[15] queryGpuMemType -> declined (2-u32 layout known, values not yet identified)");
    return kTtlUnsupported;
}

// ---------------------------------------------------------------- slot 16
// Four u64s -> Hardware+0x190..0x1a8, which become 0xc0..0xd8 of the struct
// AMDAccelDevice::getHardwareInfo validates. It REJECTS the device if any of
// them is zero:
//
//   AMDAccelDevice::getHardwareInfo - Invalid values -
//       refClkFreq: 0x0 sysClkFreq: 0x0 memClkFreq: 0x0 cgRefClkFreq: 0x0
//
// which is what kept the two AMDAccelDevice children !registered after the
// accelerator itself started. Declining here is not free.
//
// The engine clocks come from the SMU, which the bring-up ladder already talks
// to. The reference clock does not: 100 MHz is AMD's reference on this family
// and there is no register that reports it, so it is a constant and is labelled
// as one.
//
// UNITS ARE A GUESS — kHz. See the note on GpuClkInfo. Nothing in AMDHardware
// reads these back, so there is nothing to infer the scale from; the only hard
// requirement established is non-zero. The values are logged so Apple's own
// "Core Clock(MHz)" counter can be compared against the truth and the scale
// corrected. If that counter reads 1000x wrong, this is why.
uint32_t Navi48Ttl::queryGpuClkInfo(GpuClkInfo *out) {
    NOTE(16, "queryGpuClkInfo");
    if (!out) {
        TTLLOG("[16] queryGpuClkInfo: NULL output");
        return kTtlError;
    }

    // AMD reference clock for this family. Constant, not measured.
    constexpr uint64_t kRefClkKHz = 100ull * 1000ull;

    uint64_t gfxKHz = 0, memKHz = 0;
    if (mDev) {
        amdgpu::SmuMetricsResult m{};
        if (amdgpu::smu_read_metrics(*mDev, &m) == KERN_SUCCESS && m.values_plausible) {
            // SMU reports these in MHz.
            gfxKHz = (uint64_t)m.metrics.CurrClock[amdgpu::SMUClk::GFXCLK] * 1000ull;
            memKHz = (uint64_t)m.metrics.CurrClock[amdgpu::SMUClk::UCLK]   * 1000ull;
            TTLLOG("[16] SMU: GFXCLK=%u MHz UCLK=%u MHz (if_version=%u, layout %s)",
                   m.metrics.CurrClock[amdgpu::SMUClk::GFXCLK],
                   m.metrics.CurrClock[amdgpu::SMUClk::UCLK],
                   m.if_version, m.layout_verified ? "verified" : "UNVERIFIED");
        } else {
            TTLLOG("[16] SMU metrics unavailable or implausible");
        }
    }

    // A zero here is rejected by getHardwareInfo, and the GPU is clamped to a
    // low idle clock by the ladder, so a plausible floor is better than a zero
    // that fails outright. Both substitutions are logged.
    if (!gfxKHz) { gfxKHz = 500ull * 1000ull; TTLLOG("[16] GFXCLK unknown — substituting 500 MHz"); }
    if (!memKHz) { memKHz = 1000ull * 1000ull; TTLLOG("[16] UCLK unknown — substituting 1000 MHz"); }

    out->refClkFreq   = kRefClkKHz;
    out->sysClkFreq   = gfxKHz;
    out->memClkFreq   = memKHz;
    out->cgRefClkFreq = kRefClkKHz;

    TTLLOG("[16] queryGpuClkInfo -> ref=%llu sys=%llu mem=%llu cgRef=%llu (kHz, UNITS UNCONFIRMED)",
           out->refClkFreq, out->sysClkFreq, out->memClkFreq, out->cgRefClkFreq);
    return kTtlOk;
}

// ---------------------------------------------------------------- slot 30
// Single GPU, no XGMI fabric. Apple's own code treats failure here as "no peer".
uint32_t Navi48Ttl::xgmi_services(uint32_t cmd, BgdSecurityXgmiInput *, BgdSecurityXgmiOutput *) {
    NOTE(30, "xgmi_services");
    TTLLOG("[30] xgmi_services(cmd=%u) -> unsupported (single GPU, no XGMI)", cmd);
    return kTtlUnsupported;
}


// ---------------------------------------------------------------- slot 3
// "Failed to get reserved memory info from TTL!" — the wall after initialize().
//
// Layout and validation recovered from AMDRadeonX6000_AMDRTHardware::
// getReservedVRAMForSwip() @ 0xbe1a0ec. The caller zeroes 0x48 bytes, calls us,
// and then REJECTS the answer unless all of these hold:
//
//     total   = [0x08] != 0
//     visible = [0x10] != 0
//     reservedLoOffset = [0x20] <= visible
//     reservedHiOffset = [0x30] == 0, or visible <= [0x30] <= total
//
// Fail any one and it returns false, which is the "[CRITICAL] Failed to get
// reserved memory info" path. On success it copies our two (offset, size) pairs
// into Hardware+0x328/0x330/0x340/0x348/0x350/0x358.
//
// WHAT THESE TWO PAIRS ACTUALLY ARE (corrected
// ---------------------------------------------------------
// They were read here as "VRAM Apple's TTL had set aside for SWIP", so we
// reported zeros meaning "we reserve nothing". The constraints accept zeros, so
// nothing complained — and the whole accelerator was dead because of it.
//
// The offsets are not exclusions. They are the TOPS of the two VRAM arenas that
// Apple carves its own reservations out of, downwards:
//
//   getReservedVRAMForSwip():            appendToReservedVRAMOffset(type,size,align):
//     Hardware->0x340 = [0x20]             type 0: if (0x340 <= size) return -1;
//     Hardware->0x350 = [0x30]                     0x340 -= roundup(size); return 0x340;
//                                          type 1: if (0x350 <= visible+size) return -1;
//                                                  ...same shape on 0x350
//
// Reporting 0 therefore says "there is no VRAM to carve from", and every request
// is refused by the first bounds test — including the 68 MiB page-table region
// that AMDHWVMM::setVirtualSpaceReady asks for. Without that region vmm->0x50
// stays 0, setMemoryAllocationsEnabled then tries to reserve VRAM at physical
// address 0 and is refused, the VM block allocators are never built, every
// mapVA() fails, every resource prepare fails, and IOAccelCommandQueue::
// coalesceSegment finally reports kIOReturnNoMemory. Measured with DTrace, the
// whole chain.
//
// The right values are confirmed independently by the DISABLE branch of
// RTHardware::setVirtualSpaceReady, which seeds the very same two fields when
// there is no SWIP data to seed them from:
//
//     Hardware->0x340 = memoryManager->0x48   // = visibleBytes
//     Hardware->0x350 = memoryManager->0x40   // = totalBytes
//
// So "no SWIP reservation" is reported as arenas that are entirely available —
// lo top at visibleBytes, hi top at totalBytes — not as zeros.
struct GmmLocalMemoryInfoLayout {   // 0x48 bytes
    uint64_t unknown0x00;
    uint64_t totalBytes;        // 0x08  must be non-zero
    uint64_t visibleBytes;      // 0x10  must be non-zero
    uint64_t unknown0x18;
    uint64_t reservedLoOffset;  // 0x20  must be <= visibleBytes
    uint64_t reservedLoSize;    // 0x28
    uint64_t reservedHiOffset;  // 0x30  must be 0, or within [visible, total]
    uint64_t reservedHiSize;    // 0x38
    uint64_t unknown0x40;
};
static_assert(sizeof(GmmLocalMemoryInfoLayout) == 0x48, "GmmLocalMemoryInfo must be 0x48 bytes");

uint32_t Navi48Ttl::getLocalMemoryInfo(GmmLocalMemoryInfo *raw) {
    NOTE(3, "getLocalMemoryInfo");
    if (!raw || !mDev) return kTtlError;
    auto *o = reinterpret_cast<GmmLocalMemoryInfoLayout *>(raw);

    const uint64_t total   = mDev->vramSizeBytes;
    const uint64_t visible = mDev->bar0Size ? (uint64_t)mDev->bar0Size : total;
    if (total == 0 || visible == 0) {
        TTLLOG("[03] getLocalMemoryInfo: VRAM geometry unknown (total=%llu visible=%llu) "
               "- the caller would reject this", total, visible);
        return kTtlError;
    }
    o->totalBytes        = total;
    o->visibleBytes      = visible;
    // No SWIP means both arenas are wholly available, which is expressed as their
    // tops — NOT as zeros. See the long note above: zeros mean "no VRAM at all".
    // HIGH ARENA TOP — lowered by amdgpu::kVramHiTailReserve.
    //
    // NOTE: this is NOT the same amount the GMC reserves. The GMC withholds
    // kVramHiTailReserve + kAppleArenaCarveReserve (768 MiB) so that our own
    // vram_alloc_hi stops BELOW the region Apple carves downward from the value
    // we report here (512 MiB). The two deliberately differ; amdgpu_gmc.h
    // defines both together and explains why. An earlier version of this comment
    // said "the same 512 MiB our own GMC reserves", which was true when written
    // and became false the moment the GMC side grew the extra carve reserve --
    // see.
    //
    // This field IS the page-table base, not merely advice. Hardware->0x350 is
    // seeded from it, and AMDHardware::appendToReservedVRAMOffset (Hardware
    // vtable slot 48, byte 0x180) carves downwards from it:
    //     type 1: if (0x350 <= visible+size) return -1
    //             0x350 -= roundup(size); return 0x350
    // AMDHWVMM::setVirtualSpaceReady calls it with type=1, size=0x4400000, and
    // stores the result in vmm->0x50. Reporting `total` therefore placed Apple's
    // 68 MiB page tables at total-68 MiB = 0x3f6c00000 — measured, and exactly
    // the valueb recorded.
    //
    // That span contains the SOS-managed PSP TMR (~0x3f860_0000, holding the live
    // SDMA/CP/MES/RLC firmware) and the IP discovery table (~0x3faff0000) — see
    //. Apple zeroes this region with 17 SDMA CONST_FILL packets before
    // use, so letting the engine run those would destroy loaded firmware.
    //
    // gmc_v12_0.cpp reserves the top of VRAM for precisely those objects.
    // Reporting a lowered top here makes Apple's view of VRAM agree with ours,
    // and its allocation lands at (total - kVramHiTailReserve) - 68 MiB, clear of
    // both. The constraint (0, or within [visible, total]) still holds.
    //
    // MEASURED on 0.0.106: TTL reports hi top 15792 MiB; Apple's CONST_FILLs
    // target 0x3d6c00000 (accel dumpib); our vram_alloc_hi tops out at
    // 0x3cb000000 -- 188 MiB of separation where had left exactly 0.
    // amdgpu::kVramHiTailReserve — SHARED with gmc_v12_0.cpp on purpose. Apple
    // carves downward from what we report here, so our own vram_alloc_hi has to
    // stop below that carve; amdgpu_gmc.h documents the coupling and defines the
    // extra kAppleArenaCarveReserve the GMC side subtracts. Do not fork this.
    const uint64_t hiTop = (total > amdgpu::kVramHiTailReserve + visible)
                         ? (total - amdgpu::kVramHiTailReserve) : total;

    o->reservedLoOffset  = visible;   // top of the CPU-visible (low) arena
    o->reservedLoSize    = 0;         // nothing carved out of it yet
    o->reservedHiOffset  = hiTop;     // high arena top, minus our reserved tail
    o->reservedHiSize    = 0;
    TTLLOG("[03] getLocalMemoryInfo -> total=%llu MiB visible=%llu MiB; lo top=%llu MiB, "
           "hi top=%llu MiB (top %llu MiB withheld: it holds the SOS TMR and the IP "
           "discovery table —). Apple's 68 MiB page tables will land at %#llx.",
           total >> 20, visible >> 20, visible >> 20, hiTop >> 20,
           (total - hiTop) >> 20, (unsigned long long)(hiTop - 0x4400000ull));
    return kTtlOk;
}


// ---------------------------------------------------------------- slot 4
// Layout and validation from AMDRadeonX6000_AMDHWGart::setParameters @ 0xbe0de5a.
// The caller zeroes 0x20 bytes and then rejects the answer unless:
//
//     [0x00] != 0  and  4 KiB aligned            // the non-local aperture base
//     [0x08] >  [0x18]                           // size must exceed the reserved tail
//     [0x08] and [0x18] both 4 KiB aligned
//
// On acceptance it keeps [0x10] at HWGart+0x78 and [0x18] at HWGart+0x80, and
// HWGart::init then calls HWGart::reserve(start, size, 0x20, 0x19) with that
// pair. They are a range to EXCLUDE from allocation, not padding.
//
// "Non-local" is system memory reachable through the GART, as opposed to VRAM.
//
// CORRECTED. This used to invent an aperture — base =
// vramMcBase + vramSizeBytes, size = the accelerator's own nonlocalMemSizeLimit —
// on the reasoning that "we do not manage a GART on Apple's behalf". We do manage
// one, and Apple believed the invented numbers. Measured: it placed its SDMA ring
// at 0x83fb220000 and its command buffers at 0x83fb8001xx, both inside the
// invented range and both OUTSIDE the GART this driver actually programmed
// (ALIGN(vram_end + 1, 4 GiB), 256 MiB). Nothing maps those addresses, so the
// ring could never be fetched, no completion timestamp was ever written, and
// Apple's AMDSWScheduler ran out of credit after ~7 submissions and hung in
// AMDAccelChannel::waitForRingBufferSpace. See notes/APPLE-DRIVER-VERDICT.md.
//
// So report the aperture that is really there. It is smaller than the
// accelerator's limit (256 MiB vs 1 GiB), which is fine — the limit is a ceiling,
// not a requirement — and it satisfies HWGart::setParameters' constraints:
// base != 0 and 4 KiB aligned (it is 4 GiB aligned), totalSize > reservedSize,
// both 4 KiB aligned.
struct GmmNonLocalMemoryInfoLayout {   // 0x20 bytes
    uint64_t base;           // 0x00  non-zero, 4 KiB aligned
    uint64_t totalSize;      // 0x08  must exceed reservedSize, 4 KiB aligned
    uint64_t reservedStart;  // 0x10  -> HWGart+0x78, then HWGart::reserve(start, ...)
    uint64_t reservedSize;   // 0x18  -> HWGart+0x80, 4 KiB aligned, < totalSize
};
static_assert(sizeof(GmmNonLocalMemoryInfoLayout) == 0x20, "GmmNonLocalMemoryInfo must be 0x20 bytes");

uint32_t Navi48Ttl::getNonLocalMemoryInfo(GmmNonLocalMemoryInfo *raw) {
    NOTE(4, "getNonLocalMemoryInfo");
    if (!raw || !mDev) return kTtlError;
    auto *o = reinterpret_cast<GmmNonLocalMemoryInfoLayout *>(raw);

    constexpr uint64_t k4K = 4096;
    uint64_t base = 0, size = 0;
    if (!navi48_gart_aperture(base, size)) {
        TTLLOG("[04] getNonLocalMemoryInfo: the GART is not enabled yet - "
               "declining rather than inventing an aperture Apple would then "
               "allocate into (that is what produced the unmapped SDMA ring)");
        return kTtlError;
    }
    base &= ~(k4K - 1);
    size &= ~(k4K - 1);
    if (mNonlocalLimit && size > mNonlocalLimit) size = mNonlocalLimit;

    if (base == 0 || size == 0) {
        TTLLOG("[04] getNonLocalMemoryInfo: cannot describe an aperture "
               "(base=0x%llx size=0x%llx) - the caller would reject it", base, size);
        return kTtlError;
    }
    // [0x10] and [0x18] are NOT spare. HWGart::setParameters copies them to
    // HWGart+0x78/+0x80 and HWGart::init then calls
    //     this->vtbl[0x120](usableStart, usableSize, 0x20, 0x19)
    // to register that range with the GART allocator. Reporting zeros there
    // registers an empty range, that call fails, and HWGart::init returns false
    // with no message anywhere — which is exactly what 0.0.51 did.
    //
    // So hand back the usable sub-range explicitly. One page short of the total
    // keeps the caller's `totalSize > usableSize` check satisfied.
    // CORRECTED: HWGart slot 36 is reserve(start, size, originator, type), so this
    // pair is a range to EXCLUDE from allocation, not a usable range. Reserving
    // nearly the whole aperture (what 0.0.52 did) leaves Apple almost nothing.
    // It cannot be empty either — reserve(0, 0) fails and HWGart::init then returns
    // false with no message, which was the 0.0.51 wall. So reserve one page at the
    // very top: non-empty, satisfies the caller's totalSize > size check, and
    // leaves the rest of the aperture available.
    constexpr uint64_t kReserve = 4096;
    o->base          = base;
    o->totalSize     = size;
    o->reservedStart = base + size - kReserve;
    o->reservedSize  = kReserve;
    TTLLOG("[04] getNonLocalMemoryInfo -> base=0x%llx total=%llu MiB, reserving one page at "
           "0x%llx (size from the accelerator's own nonlocalMemSizeLimitBytes)",
           base, size >> 20, o->reservedStart);
    return kTtlOk;
}


// ---------------------------------------------------------------- slot 17
// getGartTableAddress(void **cpuPtr, uint64_t *gpuAddr), from
// AMDRadeonX6000_AMDHWGart::setParameters @ 0xbe0dfc5. Returns 0 on success; the
// caller keeps *cpuPtr at HWGart+0x58 and aborts setParameters on failure.
//
// *cpuPtr is a KERNEL-WRITABLE pointer: Apple's GART code writes page-table
// entries straight through it. So this must NOT be our own GART table. CP, MES
// and SDMA are translating through that table right now, and letting Apple write
// its own PTE format into it would corrupt address translation under running
// engines. Give it a private table instead, allocated once, owned by us, read by
// nobody.
static constexpr uint64_t kAppleGartTableBytes = 2ull << 20;   // 1 GiB of 4 KiB pages, 8-byte PTEs

uint32_t Navi48Ttl::getGartTableAddress(void **cpuPtr, unsigned long long *gpuAddr) {
    NOTE(17, "getGartTableAddress");
    if (!cpuPtr) return kTtlError;

    if (!mAppleGartTable.valid()) {
        kern_return_t kr = amdgpu::sysmem_alloc(mAppleGartTable, kAppleGartTableBytes, 4096);
        if (kr != kIOReturnSuccess || !mAppleGartTable.valid()) {
            TTLLOG("[17] getGartTableAddress: could not allocate a %llu KiB scratch GART table (0x%x)",
                   kAppleGartTableBytes >> 10, kr);
            return kTtlError;
        }
        TTLLOG("[17] allocated a private %llu KiB GART table for Apple at cpu=%p bus=0x%llx "
               "(our own GART is untouched)", kAppleGartTableBytes >> 10,
               mAppleGartTable.cpu, mAppleGartTable.bus);
    }
    *cpuPtr = mAppleGartTable.cpu;
    if (gpuAddr) *gpuAddr = mAppleGartTable.bus;
    TTLLOG("[17] getGartTableAddress -> cpu=%p gpu=0x%llx", mAppleGartTable.cpu, mAppleGartTable.bus);
    return kTtlOk;
}

// ---------------------------------------------------------------- not yet done
uint32_t Navi48Ttl::getTtlRtsInfo(TtlRtsInfo *)                     { return UNSUPPORTED(2, "getTtlRtsInfo"); }
uint32_t Navi48Ttl::setFirmwareDirectory(AMDFirmwareDirectory *d)   {
    NOTE(8, "setFirmwareDirectory");
    TTLLOG("[08] setFirmwareDirectory(%p) -> accepted and ignored; our firmware is already loaded", d);
    return kTtlOk;
}
// Slots 18 and 19 — the wall that stopped createAccelChannels.
//
// EVERY caller measured on hardware is VIDEO, not graphics or compute:
//
//   queue 0 inst 0 cmd 0,1,2,3,6,7   AMDVCN2DecChannel::initialize{Start,End,
//                                    Fence,Trap,IndirectBufferCmd}
//   queue 0 inst 1 cmd 0             the second decode instance
//   queue 1 inst 0 cmd 18            AMDVCN2EncChannel::initQueueCommandFrame
//
// The accelerator is AMDRadeonX6000_AMDNavi21GraphicsAccelerator, and Navi21's
// engine set includes VCN 2.x, so it builds video channels whether or not this
// card's video block has been brought up. Ours has not been: the bring-up ladder
// does GC, SDMA, CP/MES and the memory hubs, and stops there.
//
// So these report a command of ZERO LENGTH. That is not a fake success — it is
// the honest answer for an engine with no command stream: the caller does
//
//     return info.sizeBytes >> 2;
//
// and advances its ring by that many dwords, so zero means "nothing to emit".
// buildCommand then writes nothing, which is consistent.
//
// If a zero-length command turns out not to satisfy the channel, the next thing
// to try is refusing the VIDEO queues specifically while supporting the graphics
// ones — but no graphics command has ever been requested, so there is nothing to
// support yet.
uint32_t Navi48Ttl::queryCommandInfo(AmdSwipQueueType q, unsigned int i, AmdTtlCommandType c,
                                     TtlCommandInfoOutput *out) {
    NOTE(18, "queryCommandInfo");
    if (!out) {
        TTLLOG("[18] queryCommandInfo(queue=%u inst=%u cmd=%u) -> NULL output", q, i, c);
        return kTtlError;
    }
    out->word0     = (uint32_t)c;   // handed straight back to buildCommand
    out->sizeBytes = 0;             // caller computes sizeBytes >> 2 = 0 dwords
    out->word2     = 0;
    TTLLOG("[18] queryCommandInfo(queue=%u inst=%u cmd=%u) -> 0 bytes "
           "(video engine, no command stream)", q, i, c);
    return kTtlOk;
}
uint32_t Navi48Ttl::buildCommand(AmdSwipQueueType q, unsigned int i,
                                 TtlBuildCommandInfo *in, void *dest) {
    NOTE(19, "buildCommand");
    // Nothing to write: queryCommandInfo said this command is zero dwords long.
    // `dest` is the caller's ring position and is deliberately left untouched.
    TTLLOG("[19] buildCommand(queue=%u inst=%u word0=%u dest=%p) -> wrote 0 dwords",
           q, i, in ? in->word0 : 0u, dest);
    return kTtlOk;
}
uint32_t Navi48Ttl::submitFrame(AmdSwipQueueType q, unsigned int i, void *) {
    NOTE(20, "submitFrame");
    TTLLOG("[20] submitFrame(queue=%u inst=%u) -> unsupported", q, i);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::queryFwLoadingStatus(CrossArchIriIsFwLoadedInput *, CrossArchIriIsFwLoadedOutput *) {
    return UNSUPPORTED(21, "queryFwLoadingStatus");
}
uint32_t Navi48Ttl::addGartSaveRestoreRange(unsigned long long a, unsigned long long b) {
    NOTE(22, "addGartSaveRestoreRange");
    TTLLOG("[22] addGartSaveRestoreRange(0x%llx, 0x%llx) -> accepted (no-op)", a, b);
    return kTtlOk;
}
uint32_t Navi48Ttl::removeGartSaveRestoreRange(unsigned long long a) {
    NOTE(23, "removeGartSaveRestoreRange");
    TTLLOG("[23] removeGartSaveRestoreRange(0x%llx) -> accepted (no-op)", a);
    return kTtlOk;
}
uint32_t Navi48Ttl::mapToGart(unsigned long long va, IOMemoryDescriptor *md) {
    NOTE(24, "mapToGart");
    TTLLOG("[24] mapToGart(va=0x%llx, md=%p) -> unsupported", va, md);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::unmapFromGart(unsigned long long va, IOMemoryDescriptor *md) {
    NOTE(25, "unmapFromGart");
    TTLLOG("[25] unmapFromGart(va=0x%llx, md=%p) -> unsupported", va, md);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::hdcp_services(uint32_t, BgdSecurityHdcpInput *, BgdSecurityHdcpOutput *)   { return UNSUPPORTED(26, "hdcp_services"); }
uint32_t Navi48Ttl::display_topology_services(uint32_t, BgdSecurityTopoInput *, BgdSecurityTopoOutput *) { return UNSUPPORTED(27, "display_topology_services"); }
uint32_t Navi48Ttl::auc_services(uint32_t, BgdSecurityAucInput *, BgdSecurityAucOutput *)      { return UNSUPPORTED(28, "auc_services"); }
uint32_t Navi48Ttl::fp_services(uint32_t, BgdSecurityFpInput *, BgdSecurityFpOutput *)         { return UNSUPPORTED(29, "fp_services"); }
uint32_t Navi48Ttl::rap_services(uint32_t, BgdSecurityRapOutput *)                             { return UNSUPPORTED(31, "rap_services"); }
uint32_t Navi48Ttl::collectEngineDiagInfo(AmdTtlCollectDiagInfoInput *, AmdTtlCollectDiagInfoOutput *) { return UNSUPPORTED(32, "collectEngineDiagInfo"); }
uint32_t Navi48Ttl::queryFwAttestationInfo(AmdFwAttestationInput *, AmdFwAttestationOutput *)  { return UNSUPPORTED(33, "queryFwAttestationInfo"); }
uint32_t Navi48Ttl::queryEngineQueueCount(AmdSwipQueueType q, unsigned int *n) {
    NOTE(34, "queryEngineQueueCount");
    if (n) *n = 0;
    TTLLOG("[34] queryEngineQueueCount(queue=%u) -> unsupported", q);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::notifyEngine(AmdSwipQueueType q, unsigned int i, AmdTtlEngineEvent e) {
    NOTE(35, "notifyEngine");
    TTLLOG("[35] notifyEngine(queue=%u inst=%u event=%u) -> accepted (no-op)", q, i, e);
    return kTtlOk;
}
// Slots 36-39 are the queue lifecycle Apple delegates to TTL — the seam that lets
// us answer with MES ADD_QUEUE where Apple's TTL used KIQ MAP_QUEUES. Not wired
// to MES yet: the AMD_SWIP_QUEUE_TYPE values and the params struct are unknown,
// and this boot is what tells us which queue types the accelerator actually asks
// for. Log the arguments; that IS the deliverable.
// Hand out one 64-bit doorbell word per (queue, inst), idempotently.
//
// Allocated from the TOP half of Apple's aperture: Apple places its own doorbells
// in there too (PM4 channel id 2 already had one at boot), and we have no map of
// which offsets it uses. Starting halfway in and stepping a cache line keeps us
// clear of the low offsets Apple hands out first, at a cost of nothing -- the
// aperture is 2 MiB and we need 8 words.
bool Navi48Ttl::doorbellSlot(uint32_t q, uint32_t i, uint32_t &offOut) {
    for (unsigned k = 0; k < kMaxDoorbells; k++)
        if (mDoorbells[k].used && mDoorbells[k].queue == q && mDoorbells[k].inst == i) {
            offOut = mDoorbells[k].off; return true;          // same queue -> same slot
        }
    if (mDoorbellNext >= kMaxDoorbells) return false;
    const uint32_t base = mDoorbellBytes / 2;
    const uint32_t off  = base + mDoorbellNext * 0x40;
    if (off + 8 > mDoorbellBytes) return false;
    mDoorbells[mDoorbellNext] = { q, i, off, true };
    mDoorbellNext++;
    offOut = off;
    return true;
}

// slot 36 — startEngineQueue. THE call that gates everything.
//
// AMDGFX10SDMAEngine::start() @0xbe278c0 calls this and, on success, does:
//     movq -0x40(%rbp),%rax ; movq %rax,0xc0(%r13)   ring->0xc0 = out[0]
//     callq *0x130(%rax)                             ring enable()
// and on ANY non-zero return jumps straight to its unwind path, which is what
// produced "Failed to start SDMA0" and left ring->0xc0 NULL -- the NULL that
// AMDRTRing::writeTail then dereferenced and panicked on.
//
// The structs are opaque in the ABI header, so the offsets below come from
// start()'s own stack frame:
//     rcx = &(-0x78)  IN  is 0x38 bytes:  +0x00 u32 size (0x40000)
//                                         +0x08 ring->0x48
//                                         +0x18 ring->0xd0
//     r8  = &(-0x40)  OUT is 0x18 bytes:  +0x00 -> becomes ring->0xc0
// Only OUT+0x00 is written. Writing past 0x18 would corrupt Apple's stack frame.
//
// What we return is a real, mapped, writable doorbell word that NOTHING is
// watching yet. That is deliberate and is's safe first form: writeTail will
// store the wptr into it on every submit, no engine fetches it, and no DMA can
// start from a stale address. Wiring it to a real gfx12 SDMA queue is the next
// step, and must not happen until the ring contents have been dumped and every
// address in them verified to lie inside the GART.
//: the queue type startKIQ passes (mov esi, 8 at 0xbe4a55b). SDMA uses 7/10.
static constexpr uint32_t kSwipQueueKIQ = 8;
// 0.0.178: the queue type initGraphicsMQD passes for APPLE'S GFX RING —
// `movl $0x9,%esi ; xorl %edx,%edx ; callq *0x120(%rax)` @0xbe25528. Its OUT
// layout is a THIRD one, distinct from both the SDMA and the KIQ layouts:
//
//   0xbe25557  movq -0x58(%rbp),%rax ; movq %rax,0xc0(%rcx)   OUT+0x00 -> ring->0xc0
//   0xbe25562  movl -0x50(%rbp),%eax ; movl %eax,0x248(%rbx)  OUT+0x08 -> engine+0x248
//
// (-0x58(%rbp) is the OUT struct's base, which initGraphicsMQD passed in %r8.)
// engine+0x248 is then read at 0xbe24293 (`movl 0x10(%r15,%r13),%edx`, r15 =
// engine+0x238, r13 = ringType*32) and handed to submitMapQueuesPacket as its
// `unsigned int` argument, where 0xbe4a312 does `shll $2 ; andl $0xffffffc` and
// stores it as MAP_QUEUES dw2. So OUT+0x08 is a DOORBELL DWORD INDEX, not a byte
// offset and not a pointer — 0.0.177 left it zero, which is why every MAP_QUEUES
// frame in the r1 dump carries dw2 = 0.
static constexpr uint32_t kSwipQueueAppleGfx = 9;
// 0.0.185: the SDMA queue types., reproduced address-for-address in r9 on a
// different boot: queue type 7 is the engine instance's QUEUE0 and type 10 its
// QUEUE1, each ring 0x40000 BYTES = 65536 dwords. Apple's chan id 14 is (10, 1),
// ring 0x8400220000, write-back 0x8400000190 — the one `sdmamap` takes over.
static constexpr uint32_t kSwipQueueSdmaQ0       = 7;    // Apple's SDMA QUEUE0s
static constexpr uint32_t kSwipQueueSdmaQ1       = 10;   // Apple's SDMA QUEUE1s

uint32_t Navi48Ttl::startEngineQueue(AmdSwipQueueType q, unsigned int i,
                                     SwipQueueInParams *in, SwipQueueOutParams *out) {
    NOTE(36, "startEngineQueue");

    // 0.0.367 — X9's C13 census, taken BEFORE anything can refuse or return early, so that a queue start we
    // then turn away is still counted as having been asked for. The four covered types are the only ones ever seen: a
    // census of every logged boot in notes/ gives 1971 x 7, 1057 x 8, 537 x 9, 1996 x 10 and nothing else (CONFIRMED).
    {
        const uint32_t qt = (uint32_t)q;
        mQStarts++;
        if (qt == kSwipQueueSdmaQ0 || qt == kSwipQueueKIQ || qt == kSwipQueueAppleGfx || qt == kSwipQueueSdmaQ1) mQKnown++;
        else mQUnknown++;
        if (mQStarts != mQKnown + mQUnknown) mQTallyOk = 0;   // cannot happen; if it ever does, X9 stops believing the count
    }

    uint32_t reqSize = 0; uint64_t inRingA = 0, inRingB = 0;
    uint64_t inMqdGpu = 0, inMqdCpu = 0;
    if (in) {
        const uint8_t *ib = reinterpret_cast<const uint8_t *>(in);
        memcpy(&reqSize, ib + 0x00, sizeof(reqSize));
        memcpy(&inRingA, ib + 0x08, sizeof(inRingA));
        memcpy(&inRingB, ib + 0x18, sizeof(inRingB));
        // 0.0.178: initGraphicsMQD also passes the MQD it built — @0xbe2551d
        // `movq %r12,0x20(%rcx)` (the GPU address, engine+0x250) and @0xbe25521
        // `movq %r13,0x28(%rcx)` (the CPU pointer). The gfxmap emulator needs
        // both: the CPU pointer is the only way to read back the rptr-report
        // address Apple patches into its own MQD dwords 139-140.
        memcpy(&inMqdGpu, ib + 0x20, sizeof(inMqdGpu));
        memcpy(&inMqdCpu, ib + 0x28, sizeof(inMqdCpu));
    }

    if (!out) {
        TTLLOG("[36] startEngineQueue(queue=%u inst=%u): NULL out params - refusing", q, i);
        return kTtlError;
    }
    if (!mDoorbellBase || mDoorbellBytes < 16) {
        TTLLOG("[36] startEngineQueue(queue=%u inst=%u): no doorbell aperture "
               "(base=%p bytes=%u) - cannot satisfy", q, i, mDoorbellBase, mDoorbellBytes);
        return kTtlUnsupported;
    }

    uint32_t off = 0;
    if (!doorbellSlot(q, i, off)) {
        TTLLOG("[36] startEngineQueue(queue=%u inst=%u): out of doorbell slots", q, i);
        return kTtlError;
    }
    void *db = reinterpret_cast<uint8_t *>(mDoorbellBase) + off;
    *reinterpret_cast<volatile uint64_t *>(db) = 0;       // start from a known wptr

    // ---- 0.0.178: queue 9 is APPLE'S GFX RING, and it gets the REAL doorbell.
    //
    // Everything above hands out a private word from the top half of Apple's
    // aperture —'s safe first form, deliberately watched by nothing. For
    // Apple's GFX ring that is now the wrong answer: the whole point of `gfxmap`
    // is that the MES maps that ring onto GFX pipe0/queue0 and the CP listens on
    // OUR kernel GFX queue's doorbell index, so writeTail has to store into the
    // BAR2 dword the CP is actually watching (bar2 + index * 4; kCPDoorbellStride
    // is 4, amdgpu_cp.h:554).
    //
    // GATED ON THE EMULATOR BEING ARMED. Without gfxmap nothing performs the
    // takeover, so pipe0/queue0 is still OUR kernel GFX queue with OUR ring
    // behind it — and handing Apple a real doorbell there would tell the CP to
    // fetch Apple's wptr out of our ring. So: emulator armed -> the real
    // doorbell; otherwise the private word, exactly as 0.0.177 behaved.
    if (q == kSwipQueueAppleGfx) {
        uint8_t *ob = reinterpret_cast<uint8_t *>(out);
        uint32_t dbIndex = 0;
        void    *realDb  = nullptr;
        uint64_t realOff = 0;
        const bool armed = n48::hw_hook_gfxmap_armed();
        const bool haveIdx = navi48_kgq_doorbell_index(dbIndex);
        if (haveIdx && armed)
            (void)navi48_doorbell_dword_ptr(dbIndex, &realDb, &realOff);

        void *answer = realDb ? realDb : db;
        memcpy(ob + 0x00, &answer, sizeof(answer));   // -> ring->0xc0  (0xbe2555b)
        if (haveIdx)
            memcpy(ob + 0x08, &dbIndex, sizeof(dbIndex)); // -> engine+0x248 (0xbe25565)

        TTLLOG("[36] startEngineQueue(queue=%u APPLE GFX inst=%u): OUT+0x08 = "
               "doorbell DWORD INDEX %u (%s; MAP_QUEUES dw2 will read 0x%08x after "
               "the packet's <<2), OUT+0x00 = %p (%s). Aperture slot %p (+0x%x) was "
               "allocated either way so the bookkeeping stays one-slot-per-queue.",
               q, i, dbIndex, haveIdx ? "our KGQ's, from cp.doorbell_index"
                                      : "*** UNAVAILABLE - left 0 ***",
               (dbIndex << 2) & 0x0FFFFFFCu, answer,
               realDb ? "the REAL BAR2 doorbell — the CP is listening on it"
                      : (armed ? "*** could not resolve the real doorbell; private "
                                 "word, nothing watches it ***"
                               : "private word, nothing watches it (gfxmap not "
                                 "armed, so nothing will remap pipe0/queue0)"),
               db, off);
        if (realDb)
            TTLLOG("[36]   real doorbell = BAR2 + 0x%llx (dword index %u * 4). "
                   "AMDRTRing::writeTail stores the wptr there on every submit.",
                   (unsigned long long)realOff, dbIndex);

        n48::AppleGfxQueueParams p {};
        p.valid         = true;
        p.ringGpu       = inRingA;
        p.sizeBytes     = reqSize;
        p.wptrWbGpu     = inRingB;
        p.mqdGpu        = inMqdGpu;
        p.mqdCpu        = reinterpret_cast<void *>(inMqdCpu);
        p.doorbellIndex = dbIndex;
        p.doorbellPtr   = answer;
        n48::hw_hook_note_apple_gfx_queue(p);

        TTLLOG("[36] startEngineQueue(queue=%u inst=%u) size=0x%x ring=0x%llx/0x%llx "
               "mqd gpu=0x%llx cpu=0x%llx -> OK", q, i, reqSize,
               (unsigned long long)inRingA, (unsigned long long)inRingB,
               (unsigned long long)inMqdGpu, (unsigned long long)inMqdCpu);
        return kTtlOk;
    }

    //: THE OUT LAYOUT IS QUEUE-TYPE DEPENDENT, and we only had the SDMA one.
    // GFX10KIQHWChannel::startKIQ @0xbe4a4d2 consumes its results differently:
    //
    //     0xbe4a56f  test eax,eax ; jne exit            our return; 0 = OK
    //     0xbe4a573  cmp [spec+0], OUT+0x00  (u32)
    //     0xbe4a57a  cmp [spec+4], OUT+0x04  (u32)
    //     0xbe4a582  cmp [spec+8], OUT+0x08  (u32)      any mismatch -> 0xe00002bc
    //     0xbe4a591  mov rcx, OUT+0x10
    //     0xbe4a595  mov [ring+0xc0], rcx               <== THE DOORBELL
    //
    // Writing the doorbell to OUT+0x00 (the SDMA layout, where SDMAEngine::start
    // takes out[0] -> ring->0xc0) left OUT+0x10 at zero, so KIQ stored a NULL
    // doorbell -- exactly the measured ring->0xc0 = 0 -- and then submitKIQFrame
    // logged "KIQ ring is disabled. Will not submit to ring!". The three
    // u32 compares also failed against our 64-bit pointer, so startKIQ returned
    // kIOReturnError and doStart bailed at 0xbe24125 without reaching its store.
    // That is why engine+0x340 stayed NULL and preempt() saw a NULL manager.
    //
    // The spec compared against is engine+0x2b8/0x2c0, which GFX10PM4Engine::init
    // fills with the constants movabs 0x100000002 -> +0x2b8 (so spec[0]=2,
    // spec[4]=1) and movabs 0xff00015700000000 -> +0x2c0 (so spec[8]=0).
    if (q == kSwipQueueKIQ) {
        uint8_t *ob = reinterpret_cast<uint8_t *>(out);
        const uint32_t s0 = 2, s1 = 1, s2 = 0;
        memcpy(ob + 0x00, &s0, sizeof(s0));
        memcpy(ob + 0x04, &s1, sizeof(s1));
        memcpy(ob + 0x08, &s2, sizeof(s2));
        memcpy(ob + 0x10, &db, sizeof(db));               // KIQ layout: doorbell here
        TTLLOG("[36] startEngineQueue(queue=%u KIQ inst=%u): KIQ layout - echoing spec "
               "(%u,%u,%u) at OUT+0x00/04/08 and the doorbell %p at OUT+0x10, which "
               "startKIQ stores as ring->0xc0", q, i, s0, s1, s2, db);
    } else {
        // ---- 0.0.185: the SDMA layout. OUT+0x00 -> ring->0xc0, exactly once,
        // in AMDGFX10SDMAEngine::start @0xbe27978 (48 8b 45 c0 movq -0x40(%rbp),
        // %rax ; 49 89 85 c0 00 00 00 movq %rax,0xc0(%r13)).
        //
        // For Apple's chan 14 — (queue 10, inst 1), ring 0x8400220000, —
        // `sdmamap` puts that ring on OUR SDMA0 QUEUE1 at doorbell dword 0x202,
        // so the word writeTail stores into has to be the BAR2 dword the engine
        // is listening on, not a private aperture word nothing watches.
        //
        // GATED ON THE TAKEOVER BEING ARMED, like queue 9's. Un-armed boots get
        // exactly the pre-0.0.185 answer. In practice Apple starts all eight SDMA
        // queues during `fire`, before userspace can arm anything, so this branch
        // only matters if the engine is ever restarted; the running queue gets
        // its doorbell from hw_hook_sdma_map rewriting ring->0xc0, which is safe
        // because writeTail re-reads that pointer on every call (0xbe1bbc1).
        // 0.0.196: this is the SDMA layout for EVERY SDMA-type queue, not just
        // (10,1). Apple starts eight of them at `fire` — queue type 7 inst 0-3
        // (its QUEUE0s) and queue type 10 inst 0-3 (its QUEUE1s), — and
        // until now seven had no record at all, so nothing downstream could say
        // which Apple channel id was which (type, inst). The record is also the
        // map: the ring GPU VA it carries is what a channel's ring->0x48 is
        // matched against.
        void    *answer  = db;
        uint32_t dbIndex = 0;
        bool     real    = false;
        const bool isSdma = (q == kSwipQueueSdmaQ0 || q == kSwipQueueSdmaQ1);
        if (isSdma && n48::hw_hook_sdmamap_doorbell_for(q, i, &dbIndex) && dbIndex) {
            void    *realDb  = nullptr;
            uint64_t realOff = 0;
            if (navi48_sdma_q1_doorbell_ptr(dbIndex, &realDb, &realOff) && realDb) {
                answer = realDb;
                real   = true;
                TTLLOG("[36]   sdmamap has a hardware queue for (queue=%u inst=%u): "
                       "OUT+0x00 = the REAL BAR2 doorbell %p (dword index %#x, byte "
                       "%#llx) instead of the private word %p.",
                       q, i, realDb, dbIndex, (unsigned long long)realOff, db);
            } else {
                TTLLOG("[36]   sdmamap armed for (queue=%u inst=%u) but the BAR2 "
                       "dword for index %#x could not be resolved — falling back to "
                       "the private word %p", q, i, dbIndex, db);
                dbIndex = 0;
            }
        }
        memcpy(reinterpret_cast<uint8_t *>(out) + 0x00, &answer, sizeof(answer));

        if (isSdma) {
            n48::AppleSdmaQueueParams p {};
            p.valid         = true;
            p.queue         = q;
            p.inst          = i;
            p.ringGpu       = inRingA;
            p.sizeBytes     = reqSize;
            p.wptrWbGpu     = inRingB;
            p.doorbellPtr   = answer;
            p.doorbellIndex = dbIndex;
            p.realDoorbell  = real;
            n48::hw_hook_note_apple_sdma_queue(p);
        }
    }

    TTLLOG("[36] startEngineQueue(queue=%u inst=%u) size=0x%x ring=0x%llx/0x%llx "
           "-> OK, doorbell %p (aperture +0x%x). Nothing watches it yet: writeTail "
           "will store the wptr here and no engine fetches it.",
           q, i, reqSize, (unsigned long long)inRingA, (unsigned long long)inRingB, db, off);
    return kTtlOk;
}
uint32_t Navi48Ttl::stopEngineQueue(AmdSwipQueueType q, unsigned int i) {
    NOTE(37, "stopEngineQueue");
    TTLLOG("[37] stopEngineQueue(queue=%u inst=%u) -> accepted (no-op)", q, i);
    return kTtlOk;
}
uint32_t Navi48Ttl::queryEngineQueueState(AmdSwipQueueType q, unsigned int i, SwipEngineState *s) {
    NOTE(38, "queryEngineQueueState");
    TTLLOG("[38] queryEngineQueueState(queue=%u inst=%u state=%p) -> unsupported", q, i, s);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::resetEngineQueue(AmdSwipQueueType q, unsigned int i) {
    NOTE(39, "resetEngineQueue");
    TTLLOG("[39] resetEngineQueue(queue=%u inst=%u) -> unsupported", q, i);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::sendRequestToMES(AmdMesRequestType t, void *p) {
    NOTE(41, "sendRequestToMES");
    TTLLOG("[41] sendRequestToMES(type=%u, %p) -> unsupported (we have MES; wire this up next)", t, p);
    return kTtlUnsupported;
}
uint32_t Navi48Ttl::queryDpmTableInfo(TtlDpmTableInfo *, AmdDpmClockType c) {
    NOTE(42, "queryDpmTableInfo");
    TTLLOG("[42] queryDpmTableInfo(clk=%u) -> unsupported", c);
    return kTtlUnsupported;
}
