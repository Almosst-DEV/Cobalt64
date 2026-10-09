//
//  Navi48AccelPeer.cpp — see the header.
//
#include "Navi48AccelPeer.hpp"
#include "../amd/n48log.h"
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "AppleHardwareHook.hpp"
#include "Navi48Ttl.hpp"
#include "shadercache.h"
#include "gfx_subst_caps.h"   // build 0.0.484 (GLASS.md Q2 K1): kScMaxSubstBytes, with the recognition side's K2
#include "gfx_rv92.h"         // build 0.0.536 (switch 92): which RectPosTexFast_VS image the residency copy writes
#include "pairing_policy.h"
#include "sc_census.h"
#include <sys/proc.h>
#include <IOKit/IOKitKeys.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread.h>          // build 0.0.495: current_thread() keys the in-copy substitution slot
#include <IOKit/IOUserClient.h>
#include "scanout_copy.h"
#include "fastcopy.h"         // build 0.0.496: switch 63, the residency copy through SDMA (chunk plan, result word)
#include "hybrid_policy.h"    // build 0.0.503 (HYBRID.md H1): switch 68, who may open the accelerator (slot 239)
#include <sys/kauth.h>        // build 0.0.503: kauth_getuid, the caller's uid for the hybrid policy
#include <kern/task.h>        // build 0.0.503: current_task, compared with newUserClient's owner (counted only)

#define PEERLOG(fmt, ...) ::amdgpu::n48_logf("Navi48AccelPeer: " fmt "\n", ##__VA_ARGS__)

OSDefineMetaClassAndStructors(Navi48AccelPeer, IOService)

// The live peer, so the patch can be driven from the TTL rather than from a
// SpecialAMDKey request the accelerator never makes.
static Navi48AccelPeer *gPeer { nullptr };

// 0.0.267: the display-pairing stamp is OPT-IN. The decision is taken
// once per boot at kReqAccelStarted from the boot-arg and the `pairing` verb's request
// (pairing_policy.h); these record it for the verb to report and to refuse late enables.
static IOLock   *gPairingLock      { nullptr };
static uint32_t  gPairingRequest   { kPairingReqNone };
static bool      gPairingDecided   { false };
static uint32_t  gPairingSource    { kPairingSrcDefaultOff };
static bool      gPairingWithdrawn { false };

void Navi48AccelPeer_TryPatchAcceleratorStop() {
    if (gPeer) gPeer->tryPatchAcceleratorStop();
}

// _eAMDAccelIOFBRequestType, from the accelerator's 23 call sites.
enum : uint32_t {
    // Sent from GraphicsAccelerator::start() itself, at 0xbdbe363, as the LAST
    // thing before it decides whether it started:
    //
    //   requestType = 0; param2 = the accelerator itself; waitForFunction = 0
    //   ret = peer->callPlatformFunction(SpecialAMDKey, 0, &requestType, this, 0, 0)
    //   r13 = (ret == kIOReturnSuccess) & r15
    //
    // So a non-success return here fails start() outright, whatever else went
    // right. It reads as the accelerator announcing itself to the framebuffer —
    // param2 is its own `this` and nothing is asked of us — so success is the
    // answer. We hold no display state that needs updating; RDNA4FB owns scanout.
    kReqAccelStarted     = 0x00,
    kReqIOSurface        = 0x01,  // AccelResource::initIOSurfaceRef / configureAllocationOptions
    kReqVcnStartQueue    = 0x08,  // VCNHWEngine::startQueue
    kReqGfxOff           = 0x12,  // RTHardware enable/disable/isEnabled/querySupported
    kReqVcnSuspendResume = 0x13,  // VCNHWEngine::setSuspendResumeState
    kReqIsDeviceValid    = 0x14,  // Hardware::isDeviceValid
    kReqResetAndReplay   = 0x17,  // AccelChannel::resetHardwareAndReplay
    kReqScanoutDcc       = 0x1a,  // AccelResourceAddr2::shouldAllocScanoutDcc
    kReqDfCstate         = 0x1d,  // GFX10Hardware enable/disable/isEnabled/querySupported
};

// 0.0.368: RULE E1's ring-generation evidence - every reset-and-replay bracket Apple opened this boot.
// See Navi48AccelPeer.hpp for why this is the sound reading of "the ring has been through a reset since boot".
static volatile uint64_t gPeerResetReplayKeys = 0;
uint64_t navi48_peer_reset_replay_keys(void) { return gPeerResetReplayKeys; }

static const char *req_name(uint32_t t) {
    switch (t) {
    case kReqAccelStarted:     return "accelerator-started";
    case kReqIOSurface:        return "IOSurface/allocation-options";
    case kReqVcnStartQueue:    return "VCN start-queue";
    case kReqGfxOff:           return "GFXOFF";
    case kReqVcnSuspendResume: return "VCN suspend/resume";
    case kReqIsDeviceValid:    return "isDeviceValid";
    case kReqResetAndReplay:   return "reset-hardware-and-replay";
    case kReqScanoutDcc:       return "should-alloc-scanout-DCC";
    case kReqDfCstate:         return "DF C-state";
    default:                   return "unknown";
    }
}

IOService *Navi48AccelPeer::probe(IOService *provider, SInt32 *score) {
    // Same gate as the rest of the Phase 4 work: without the boot-arg this
    // object must not exist, or it would claim the ATIFramebuffer category on
    // every boot for no reason.
    uint32_t on = 0;
    if (!PE_parse_boot_argn("navi48-accel-experiment", &on, sizeof(on)) || on == 0) return nullptr;
    return super::probe(provider, score);
}

bool Navi48AccelPeer::start(IOService *provider) {
    gPeer = this;
    if (!super::start(provider)) return false;
    // 0.0.267: serialises the pairing decision/stamp (Apple's accelerator-start
    // thread) against the `pairing` verb (a user-client thread). Allocated once and never
    // freed: the verb may be running when stop() is, and a freed lock would be worse than
    // a few bytes kept for the life of the kernel.
    if (!gPairingLock) gPairingLock = IOLockAlloc();
    mKey = OSSymbol::withCString("SpecialAMDKey");
    // initLinkToPeer matches on this property, so it must be present and exact.
    setProperty("IOMatchCategory", "ATIFramebuffer");
    setProperty("Navi48,AccelPeer", "claiming ATIFramebuffer for SpecialAMDKey");
    registerService();
    PEERLOG("started: claiming IOMatchCategory=ATIFramebuffer so Apple's accelerator asks US "
            "for SpecialAMDKey instead of hanging on RDNA4FB");
    return true;
}

void Navi48AccelPeer::stop(IOService *provider) {
    if (gPeer == this) gPeer = nullptr;
    if (mKey) { mKey->release(); mKey = nullptr; }
    super::stop(provider);
}

IOReturn Navi48AccelPeer::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                               void *param1, void *param2, void *param3,
                                               void *param4) {
    if (functionName && mKey && functionName->isEqualTo(mKey)) {
        mRequests++;
        const uint32_t type = param1 ? *reinterpret_cast<uint32_t *>(param1) : 0xFFFFFFFFu;
        // PEER LOG THROTTLE: Apple's recovery path polls isDeviceValid
        // (0x14) tens of thousands of times - one interval went from request
        // #3461 to #49411 - and each poll wrote two lines. That refills the
        // 512 KiB driver log faster than any measurement can be read back, which
        // is why the pm4-powerup outcome kept vanishing even after a logreset
        // immediately before it. Log every request that is NOT the poll, the
        // first few polls, and then only every 4096th.
        {
            const bool poll = (type == kReqIsDeviceValid);
            if (!poll || mRequests <= 4 || (mRequests & 0xFFF) == 0)
                PEERLOG("SpecialAMDKey #%u: type=0x%02x (%s) wait=%d p2=%p p3=%p p4=%p",
                        mRequests, type, req_name(type), waitForFunction ? 1 : 0,
                        param2, param3, param4);
        }
        tryPatchAcceleratorStop();
        return handleSpecialAMDKey(type, param2, param3, param4);
    }
    return super::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

// ---------------------------------------------------------------------------
// Neutralising Apple's error-path panic
// ---------------------------------------------------------------------------
//
// Geometry recovered from the shipping AMDRadeonX6000 (Tahoe 26.6.2 x86_64),
// __ZTV37AMDRadeonX6000_AMDGraphicsAccelerator @ 0xbf14be0, which runs to the
// next vtable symbol at 0xbf15760 — 0xb80 bytes, 368 pointers, 366 slots after
// the two header words.
//
//   slot 184  AMDGraphicsAccelerator::start(IOService*)   0x0bdbddc2   <- anchor
//   slot 185  AMDGraphicsAccelerator::stop(IOService*)    0x0bdbf024   <- patched
//
// The call site in start() is `callq *0x5c8(%rax)`, and 0x5c8 / 8 = 185, which
// is how the slot was identified rather than guessed.
static constexpr unsigned  kVtHeader          = 2;
static constexpr unsigned  kAccelVtableSlots  = 366;
static constexpr unsigned  kAccelStopSlot     = 185;
static constexpr unsigned  kAccelAnchorSlot   = 184;   // start()
static constexpr long      kAccelStopDelta    = 0x1262; // stop - start
static constexpr uintptr_t kAccelStopPageOff  = 0x024;
static constexpr uintptr_t kKernelHalfBase    = 0xffffff7000000000ULL;

// Instrument: skip the resource paging copy.
//
// AMDRadeonX6000_AMDAccelResource::pageTexture(bool, IOAccelMemoryMap*,
// IOAccelMemoryMap*) is slot 103 (byte 0x338) of every AMDAccelResource
// subclass — the same implementation address in all of them, so the check is
// simply that the slot's target has pageTexture's page offset.
//
// WHY THIS EXISTS
// ---------------
// The copy fails, and it fails for a reason we cannot reach yet: Apple's blit
// manager never builds its engine, because the UBM factory returns NULL even
// though the ASIC identity gate it guards (family 0xd, chip 0x8f) passes on this
// card — measured. That library is statically linked into AMDRadeonX6000 with no
// symbols, so there is nothing left to trace by name.
//
// pageon() turns that failure into `this->0x210 |= 4`, prepare() sees the bit and
// returns false, BatchPrepare returns false, and coalesceSegment reports
// kIOReturnNoMemory. So one failing memcpy masks the entire rest of the path.
//
// This makes pageTexture claim success WITHOUT copying anything, purely to find
// out whether everything downstream of residency works. The blit's DATA will be
// wrong — that is expected and is not the point. What it answers is whether the
// command actually reaches the hardware once residency stops failing.
static constexpr unsigned  kResPageTextureSlot = 103;      // byte 0x338
static constexpr uintptr_t kResPageTexturePageOff = 0xdf6;
static constexpr unsigned  kResVtableSlots    = 111;
static constexpr unsigned  kAccelNewResourceSlot = 334;    // byte 0xa70

static void   **gAccelVt { nullptr };
static void    *gAccelObj { nullptr };   // 0.0.272: the accelerator gAccelVt belongs to
static uint32_t gStopCalls { 0 };

// The replacement. stop() returns void.
static void hook_accel_stop(void * /*self*/, void * /*provider*/) {
    gStopCalls++;
    PEERLOG("AMDGraphicsAccelerator::stop() #%u SUPPRESSED — start() failed and "
            "Apple's cleanup would NULL the event logs its own epilogue then "
            "dereferences. Leaking instead so the machine survives and the log "
            "can be read.", gStopCalls);
}

static bool     gSkipPageCopy { false };   // set by boot-arg navi48-skip-pagecopy=1
static uint32_t gSkippedCopies { 0 };
static uint32_t gInspectedCopies { 0 };    // 0.0.201: the observer's own budget
static void   **gResOrigVt { nullptr };
static void   **gResCopyVt { nullptr };
typedef void *(*NewResourceFn)(void *);
static NewResourceFn gOrigNewResource { nullptr };

// Residency observation only (0.0.199-0.0.201). pageon supplies two live
// IOAccelMemoryMaps; IOAccelMemoryMap::getLength / getGPUVirtualAddress prove +0x18
// is their memory object, and IOAccelMemory::getLength reads memory+0x40. Inspect
// only the measured linear blit-library resource (0x1a000 bytes) while Apple still
// owns the maps. Never retain their pointers or repair residency here.
//
// Destination: memory->vtable[0x158] is AMDRadeonX6000_AMDAccelVidMemory::
// getPhysicalSegment (0xbdf6c3e, exact-address guard), which returns a 0-based VRAM
// address (: offset 0xfb00 -> 0x1000fb00, span 0xa500). Source (0.0.201): not
// IOAccelSysMemory::getPhysicalSegment, whose DMA walk failed in upload-03 and
// handed back its 0xAAAA... sentinel, but the backing's own
// IOMemoryDescriptor (backing_descriptor below).
static constexpr uintptr_t kStaticPageTexture = 0xbdd5df6ULL;   // AMDRadeonX6000_AMDAccelResource::pageTexture
static constexpr uintptr_t kStaticVidSegment  = 0xbdf6c3eULL;   // AMDRadeonX6000_AMDAccelVidMemory::getPhysicalSegment
typedef uint64_t (*PhysSegmentFn)(void *, uint64_t, uint64_t *);

// X6000's slide from pageTexture's live address (the vtable newResource captured);
// 0 if not captured yet or not page-aligned.
static uintptr_t x6000_slide_from_page_texture() {
    if (!gResOrigVt) return 0;
    const uintptr_t slide = reinterpret_cast<uintptr_t>(gResOrigVt[kResPageTextureSlot]) - kStaticPageTexture;
    return (slide & 0xfff) ? 0 : slide;
}

// The VidMemory accessor, only when it is exactly AMDAccelVidMemory::getPhysicalSegment.
static PhysSegmentFn vid_segment_fn(void *mem, uintptr_t *seen) {
    *seen = 0;
    const uintptr_t slide = x6000_slide_from_page_texture();
    void **vt = *reinterpret_cast<void ***>(mem);
    if (!slide || reinterpret_cast<uintptr_t>(vt) < kKernelHalfBase) return nullptr;
    const uintptr_t fn = reinterpret_cast<uintptr_t>(vt[0x158 / 8]);
    *seen = fn;
    return fn == slide + kStaticVidSegment ? reinterpret_cast<PhysSegmentFn>(fn) : nullptr;
}

// The backing's IOMemoryDescriptor. IOAccelSysMemory keeps it at +0xd0:
// withMemoryDescriptor stores its argument there (0x145d6a3b), wire() hands it to the
// DMA command it takes from IOGraphicsAccelerator2::getDMACommand (0x145d703a..
// 0x145d705e), and getPhysicalSegment falls back to its vtable 0x138 when there is no
// DMA command (0x145d6dc2..0x145d6dd9). OSDynamicCast proves the type; callers then
// require getPreparationID() != kIOPreparationIDUnprepared (the memory is wired, so
// readBytes can walk its physical segments) before reading. Nothing is retained.
static IOMemoryDescriptor *backing_descriptor(void *srcMem, uint64_t *prepOut) {
    *prepOut = kIOPreparationIDUnprepared;
    OSObject *o = *reinterpret_cast<OSObject *const *>(static_cast<const char *>(srcMem) + 0xd0);
    if (reinterpret_cast<uintptr_t>(o) < kKernelHalfBase) return nullptr;
    IOMemoryDescriptor *md = OSDynamicCast(IOMemoryDescriptor, o);
    if (md) *prepOut = md->getPreparationID();
    return md;
}

static void inspect_pagecopy(void *self, void *dst, void *src) {
    if (!self || !dst || !src) return;
    const char *r = static_cast<const char *>(self);
    const uint64_t bytes = *reinterpret_cast<const uint64_t *>(r + 0x230);
    const uint64_t backingOffset = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    PEERLOG("pagecopy-observe: resource=%p type=%#x bytes=%#llx backingOffset=%#llx",
            self, (unsigned)*reinterpret_cast<const uint8_t *>(r + 0x14),
            (unsigned long long)bytes, (unsigned long long)backingOffset);
    void *maps[2] = {dst, src};
    void *mems[2] = {nullptr, nullptr};
    const char *names[2] = {nullptr, nullptr};
    for (unsigned j = 0; j < 2; j++) {
        if (reinterpret_cast<uintptr_t>(maps[j]) < kKernelHalfBase) return;
        const char *m = static_cast<const char *>(maps[j]);
        mems[j] = *reinterpret_cast<void *const *>(m + 0x18);
        if (reinterpret_cast<uintptr_t>(mems[j]) < kKernelHalfBase) return;
        const OSMetaClass *meta = static_cast<OSObject *>(mems[j])->getMetaClass();
        if (!meta) return;
        names[j] = meta->getClassName();
        PEERLOG("pagecopy-observe: %s map=%p flags=%#x cachedVA=%#llx memory=%p class=%s length=%#llx",
                j ? "source" : "destination", maps[j],
                *reinterpret_cast<const uint32_t *>(m + 0x10),
                (unsigned long long)*reinterpret_cast<const uint64_t *>(m + 0x98),
                mems[j], names[j],
                (unsigned long long)*reinterpret_cast<const uint64_t *>(static_cast<const char *>(mems[j]) + 0x40));
    }
    if (bytes != 0x1a000 || backingOffset != 0 || !names[0] || !names[1] ||
        strcmp(names[0], "AMDRadeonX6000_AMDAccelVidMemory") ||
        (strcmp(names[1], "IOAccelSysMemory") &&
         strcmp(names[1], "AMDRadeonX6000_AMDAccelSysMemory"))) {
        PEERLOG("pagecopy-observe: sample refused (resource shape or memory class differs)");
        return;
    }
    const uint64_t sampleOffset = 0xfb00; // shader VA minus destination VA in upload-01
    uintptr_t seen = 0;
    const PhysSegmentFn dSeg = vid_segment_fn(mems[0], &seen);
    const uint64_t dLen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(mems[0]) + 0x40);
    if (!dSeg || dLen < sampleOffset + 64) {
        PEERLOG("pagecopy-observe: destination physical-segment entry guard refused (%p)", (void *)seen);
        return;
    }
    uint64_t dSpan = 0;
    const uint64_t dPhys = dSeg(mems[0], sampleOffset, &dSpan);
    PEERLOG("pagecopy-observe: destination offset=%#llx physical=%#llx span=%#llx",
            (unsigned long long)sampleOffset, (unsigned long long)dPhys, (unsigned long long)dSpan);
    if (!dPhys || (dPhys & 3) || dSpan < 64) {
        PEERLOG("pagecopy-observe: destination segment unusable - sample refused");
        return;
    }
    uint64_t prep = 0;
    IOMemoryDescriptor *md = backing_descriptor(mems[1], &prep);
    if (!md) {
        PEERLOG("pagecopy-observe: source+0xd0 is not an IOMemoryDescriptor - sample refused");
        return;
    }
    const uint64_t mdLen = md->getLength();
    PEERLOG("pagecopy-observe: source descriptor=%p class=%s length=%#llx preparationID=%#llx%s",
            md, md->getMetaClass()->getClassName(), (unsigned long long)mdLen,
            (unsigned long long)prep, prep == kIOPreparationIDUnprepared ? " (UNPREPARED - not read)" : "");
    if (prep == kIOPreparationIDUnprepared || mdLen < bytes) return;
    uint32_t head[16] = {}, source[16] = {}, destination[16] = {};
    const IOByteCount gotHead = md->readBytes(0, head, sizeof(head));
    const IOByteCount gotSource = md->readBytes(sampleOffset, source, sizeof(source));
    uint64_t readDwords = 0, nonzero = 0;
    uint32_t chunk[64];
    for (uint64_t o = 0; o + sizeof(chunk) <= bytes; o += sizeof(chunk)) {
        if (md->readBytes(o, chunk, sizeof(chunk)) != sizeof(chunk)) break;
        readDwords += 64;
        for (unsigned i = 0; i < 64; i++) if (chunk[i]) nonzero++;
    }
    const bool gotVram = navi48_vram_read_mm(dPhys, destination, 16);
    PEERLOG("pagecopy-observe: source readBytes head %llu/64 sample %llu/64; VRAM sample %s",
            (unsigned long long)gotHead, (unsigned long long)gotSource, gotVram ? "read" : "NOT read");
    if (gotHead == sizeof(head))
        for (unsigned j = 0; j < 16; j += 8)
            PEERLOG("pagecopy-observe: +%#x SOURCE %08x %08x %08x %08x %08x %08x %08x %08x",
                    j * 4, head[j], head[j+1], head[j+2], head[j+3], head[j+4], head[j+5], head[j+6], head[j+7]);
    if (gotSource == sizeof(source) && gotVram)
        for (unsigned j = 0; j < 16; j += 4)
            PEERLOG("pagecopy-observe: +%#x SOURCE %08x %08x %08x %08x ; VRAM %08x %08x %08x %08x",
                    (unsigned)sampleOffset + j * 4, source[j], source[j+1], source[j+2], source[j+3],
                    destination[j], destination[j+1], destination[j+2], destination[j+3]);
    PEERLOG("pagecopy-observe: source census: %llu of %llu dword(s) nonzero (resource %#llx bytes)",
            (unsigned long long)nonzero, (unsigned long long)readDwords, (unsigned long long)bytes);
}

// ---------------------------------------------------------------------------
// 0.0.201 — the residency copy for pageTexture(toVram), armed per boot by
// `accel pagecopy 1`. Unarmed, the hook below behaves exactly as 0.0.200.
// ---------------------------------------------------------------------------
// Apple's pageTexture (0xbdd5df6) returns early when the resource's residency masks
// (res->0x30, +0 / +0xc) say nothing is dirty, then picks a copy flavour by the type
// nibble (res+0x1b4 >> 26) & 0xf through the jump table at 0xbdd6174 - vtable 0x310
// WithStretch (0,1), 0x318 DepthStencil (2), 0x320 WithMemcpy (3,4), 0x328
// WithSurfaceCopy (7,8), 5/6 fail - and on success ORs one mask into the other. The
// memcpy flavour adds res+0xf8 (the backing offset) to one map's address and copies
// res+0x230 bytes (0xbdd6a85..0xbdd6a99).
//
// This reproduces only the data transfer:
//   source      [backingOffset, backingOffset + bytes) of the backing descriptor
//               (sysmem+0xd0, readBytes; see backing_descriptor)
//   destination [0, bytes) of the AMDAccelVidMemory, walked segment by segment with
//               its exact getPhysicalSegment (0-based VRAM addresses)
// Transport: the MM_INDEX/MM_DATA window, the only CPU path to VRAM above the 256 MiB
// BAR0 aperture (the measured destination starts at VRAM 0x10000000), 64 dwords per
// batch under the window mutex, every dword read back and compared. Every destination
// segment is cleared by navi48_vram_apple_dest_check before the first write. The
// residency masks are left alone, as the skip always left them. No pointer Apple
// hands us outlives the call.
static bool gCopyArmed { false };
static struct {
    uint64_t copies, bytes, compared, mismatched, unhandled, pageOuts;
    uint64_t lastDst, lastBytes, lastMicros, failed;
    uint32_t unhandledMask;
} gCopy {};
static constexpr uint64_t kCopyMaxBytes   = 64ull << 20;
static constexpr uint64_t kCopyChunkBytes = 1ull << 20;
static_assert(N48_FC_STAGING_BYTES == kCopyChunkBytes, "build 0.0.496: one chunk is one SDMA submission from one staging buffer");

enum : uint32_t {
    kCopyShapeIdentity = 1u << 0, kCopyShapeClass   = 1u << 1, kCopyShapeLength  = 1u << 2,
    kCopyShapeAccessor = 1u << 3, kCopyShapeSegment = 1u << 4, kCopyShapeDest    = 1u << 5,
    kCopyShapeBacking  = 1u << 6,
};

#define COPY_UNHANDLED(bit, fmt, ...) do {                                              \
        gCopy.unhandled++;                                                             \
        if (!(gCopy.unhandledMask & (bit))) {                                          \
            gCopy.unhandledMask |= (bit);                                              \
            PEERLOG("residency-copy: UNHANDLED shape %#x (logged once; this pageTexture " \
                    "keeps the skip): " fmt, (unsigned)(bit), ##__VA_ARGS__);          \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
//  — THE RESIDENCY COPY AS A PROVENANCE SOURCE (ws_resprov.h). Acts only while n48::hw_resprov_on() (the descriptor
// path AND `accel gfxneuter 11 | 1 << 8`); off, every line below is skipped and the copy is byte-for-byte 0.0.363's.
// ---------------------------------------------------------------------------
// Apple's gfx10 GB_ADDR_CONFIG, as ITS OWN kernel addrlib was created with it. AMDHWAlignManager2::init (0xbe1c4e6) stores its
// IAMDHWInterface argument at +0x10 (0xbe1c587 movq %r15,0x10(%rbx)), calls that interface's vtable 0x1c0 (0xbe1c591) and copies
// result+0xa0 into ADDR_CREATE_INPUT+0x30 = regValue.gbAddrConfig (0xbe1c600 movl 0xa0(%r15),%eax; 0xbe1c607 movl %eax,0x30(%r14)).
// Slot 0x1c0 of AMDNavi21Hardware / AMDGFX10Hardware / AMDHardware is `leaq 0xd0(%rdi),%rax; ret` (0xbe531e6 / 0xbe33018 /
// 0xbe2f728), so the value is the dword at interface+0x170. The resource names its align manager at res+0x250.
// Every hop is identity-checked; nothing is called.
static constexpr uintptr_t kStaticAlignMgr2Vt  = 0x0bf32d00ULL + 16;   // __ZTV33AMDRadeonX6000_AMDHWAlignManager2
static constexpr uintptr_t kStaticGfx10AlignVt = 0x0bf3c438ULL + 16;   // __ZTV35AMDRadeonX6000_AMDGFX10AlignManager
static constexpr uintptr_t kStaticHwCfgFn[3]   = { 0x0be531e6ULL, 0x0be33018ULL, 0x0be2f728ULL };
// build 0.0.450 item 2 (reviewer fix): PER GFX10 MODE, not one shared flag - see ws_resprov.h n48_rp_pageout_refuse's
// own banner. gRpEverRetiled4kbdx covers mode 22 (set by the always-on 32bpp path and, under switch 46, the 8/64bpp
// kinds); gRpEverRetiled256bd covers mode 2 (set ONLY by the 256B_D kind, itself only reachable under switch 46) - so
// with switch 46 OFF, gRpEverRetiled256bd can never become 1 and the page-out check reduces to exactly 0.0.449's.
static uint32_t gRpEverRetiled4kbdx { 0 }, gRpEverRetiled256bd { 0 };   // sticky for the boot: page-out symmetry
// build 0.0.492: mode 27 (ADDR_SW_64KB_R_X), set ONLY by a switch-59 backing-sourced copy of such a resource (the wallpaper).
static uint32_t gRpEverRetiled64krx { 0 };
// build 0.0.493 (ws_resprov.h section 7f): every resource a switch-59 KNOWN-ASSET conversion was prepared for, remembered
// before its first byte is written (a full table refuses the conversion). Its page-outs are REFUSED (the per-mode flags above cannot
// see it: its swizzle dword says mode 0). gRpKnownAny = at least one was added (0 unless switch 59 converted a known asset).
static constexpr uint32_t kRpKnownRes = 16u;
static uint64_t gRpKnownRes[kRpKnownRes] {};
static uint32_t gRpKnownAny { 0 };
//: THE PER-COPY LOG CAP. hp7 held it at 64 lines, which covered copies #459-#522 only - F3's own copy #594 fell outside it,
// so had to INFER its shape verdict from the code instead of reading it. 1024 covers the whole early window in copy order
// (hp7 made 1806 copies in the boot, and F3's is #594).
static constexpr uint64_t kRpLogLines = 1024ull;
static struct {
    uint64_t considered, retiled, shape[N48_RP_SHAPE_REASONS], allocFail, readFail, pageoutRefused, logged;
    //: res+0x1dc disagreeing with the record the hardware reads is a counted WARNING, not a refusal (Apple's cmoveq at
    // 0xbdf9141 means the hardware never reads res+0x1dc while res+0x180 is set). hp7: that disagreement refused all 23 textures.
    uint64_t warnSwz, warnType;
    uint32_t lastGb, gbRead;
    uint64_t bytesSmall;   // build 0.0.451 item 1 (S1): N48_RP_SHAPE_BYTES_SMALL, counted separately from shape[]'s own tally
} gRt {};
uint64_t navi48_resprov_warn_swz(void) { return gRt.warnSwz; }
uint64_t navi48_resprov_warn_type(void) { return gRt.warnType; }
// build 0.0.451 item 1 (S1): logged ONCE PER RESOURCE (by its own pointer - a boot-lived table, not per-copy),
// with its w, h, rowBytes, bytes and swizzle, the moment a re-tile is refused because Apple's own allocation is
// SMALLER than the shape's exact layout - a resource this small would be unsafe to re-tile (an out-of-bounds walk
// over the copy's own read/write buffers), so this is a distinct, once-only diagnostic line, not the per-copy
// resprov summary every copy of every resource already gets.
static constexpr uint32_t kRpBytesSmallSeen = 64u;
static const void *gRpBytesSmallSeenPtr[kRpBytesSmallSeen] {};
static uint32_t gRpBytesSmallSeenN { 0u };
static void rp_log_bytes_small_once(const void *res, uint32_t w, uint32_t h, uint32_t rowBytes, uint64_t bytes, uint32_t swz) {
    for (uint32_t i = 0; i < gRpBytesSmallSeenN && i < kRpBytesSmallSeen; i++) if (gRpBytesSmallSeenPtr[i] == res) return;
    if (gRpBytesSmallSeenN < kRpBytesSmallSeen) gRpBytesSmallSeenPtr[gRpBytesSmallSeenN++] = res;
    gRt.bytesSmall++;
    PEERLOG("resprov: resource=%p REFUSED (BYTES_SMALL, once per resource): %ux%u rowBytes %u bytes %#llx swizzle %#x - "
            "Apple's own allocation is SMALLER than this shape's exact layout; re-tiling it would read/write past the "
            "buffer the copy actually allocated, so the plain byte-for-byte copy is kept instead",
            res, w, h, rowBytes, (unsigned long long)bytes, swz);
}
static int apple_gb_addr_config(const char *r, uint32_t *gb, const char **why) {
    const uintptr_t slide = x6000_slide_from_page_texture();
    if (!slide) { *why = "no X6000 slide"; return 0; }
    void *am = *reinterpret_cast<void *const *>(r + 0x250);
    if (reinterpret_cast<uintptr_t>(am) < kKernelHalfBase) { *why = "res+0x250 is not a kernel pointer"; return 0; }
    const uintptr_t amVt = reinterpret_cast<uintptr_t>(*reinterpret_cast<void *const *>(am));
    if (amVt != slide + kStaticAlignMgr2Vt && amVt != slide + kStaticGfx10AlignVt) {
        *why = "res+0x250 is not an AMDHWAlignManager2 / AMDGFX10AlignManager"; return 0;
    }
    void *hw = *reinterpret_cast<void *const *>(static_cast<const char *>(am) + 0x10);
    if (reinterpret_cast<uintptr_t>(hw) < kKernelHalfBase) { *why = "align manager +0x10 is not a kernel pointer"; return 0; }
    void *const *hwVt = *reinterpret_cast<void *const *const *>(hw);
    if (reinterpret_cast<uintptr_t>(hwVt) < kKernelHalfBase) { *why = "the hardware interface has no kernel vtable"; return 0; }
    const uintptr_t fn = reinterpret_cast<uintptr_t>(hwVt[0x1c0 / 8]) - slide;
    if (fn != kStaticHwCfgFn[0] && fn != kStaticHwCfgFn[1] && fn != kStaticHwCfgFn[2]) {
        *why = "the hardware interface's slot 0x1c0 is not the known `leaq 0xd0(%rdi),%rax`"; return 0;
    }
    *gb = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(hw) + 0xd0 + 0xa0);
    return 1;
}
// IOAccelMemoryMap::getGPUVirtualAddress (IOAcceleratorFamily2 0x145d4640): `testb $0x40,0x10(%rdi); jne <physical>;
// movq 0x98(%rdi),%rax` - the non-physical branch is a field read, done here without the call. 0 = not available.
static uint64_t map_gpu_va(void *map, uint32_t *ok) {
    *ok = 0;
    if (reinterpret_cast<uintptr_t>(map) < kKernelHalfBase) return 0;
    const OSMetaClass *m = static_cast<OSObject *>(map)->getMetaClass();
    const char *n = m ? m->getClassName() : nullptr;
    static const char kSuffix[] = "MemoryMap";           // IOAccelMemoryMap or a subclass named ...MemoryMap
    const size_t ln = n ? strlen(n) : 0, ls = sizeof(kSuffix) - 1;
    if (!n || ln < ls || strcmp(n + ln - ls, kSuffix) != 0) return 0;
    if (*reinterpret_cast<const uint8_t *>(static_cast<const char *>(map) + 0x10) & 0x40u) return 0;
    *ok = 1;
    return *reinterpret_cast<const uint64_t *>(static_cast<const char *>(map) + 0x98);
}
// build 0.0.451 item 3 (S3, review of 0.0.450): the SAME hardware swizzle dword n48_rp_shape_check(_kind) uses
// (n48_rp_swz_dword: *(res+0x180)+0x40 when res+0x180 is set, else res+0x1dc -'s setupHwCBRegs cmoveq), read
// standalone for the page-out check. The ORIGINAL page-out call read res+0x1dc DIRECTLY and unconditionally - which
// decide42 logged as 0x20 (mode 0) for every re-tiled resource, since every one of them has res+0x180 set,
// so the page-out symmetry check compared the WRONG dword and could never match a re-tiled mode. This mirrors
// resource_shape's own mask read exactly (same fields, same fail path when the mask pointer is not a kernel
// pointer at all - `mask` false keeps `surf`, the res+0x1dc fallback, unchanged).
static uint32_t rp_pageout_swz_dword(const char *r) {
    n48_rp_shape s {};
    s.surf = *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    void *mask = *reinterpret_cast<void *const *>(r + 0x180);
    s.hasMask = mask ? 1u : 0u;
    if (reinterpret_cast<uintptr_t>(mask) >= kKernelHalfBase) {
        s.maskSurf = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(mask) + 0x40);
        s.maskOk = 1u;
    } else if (mask) s.maskSurf = 0xffffffffu;   // hasMask but not a kernel pointer: n48_rp_swz_dword still returns
                                                  // this (mode [4:0] = 0x1f), matching neither tracked mode - the
                                                  // narrow corner case where the SAME resource is unreadable at
                                                  // page-out time; not the case S3 measured (every re-tiled input's
                                                  // mask WAS readable - the bug was reading the wrong field, not an
                                                  // unreadable one).
    return n48_rp_swz_dword(&s);
}
// build 0.0.492: the page-out symmetry check over all three flags, noinline (hook_page_texture's frame is at its cap): the
// SAME dword read once, then 0.0.451's n48_rp_pageout_refuse exactly, OR the mode-27 flag (0 unless switch 59 wrote such a resource).
static int __attribute__((noinline)) rp_pageout_refused(const char *r) {
    const uint32_t d = rp_pageout_swz_dword(r);
    return n48_rp_pageout_refuse(gRpEverRetiled4kbdx, gRpEverRetiled256bd, d) || n48_rp_pageout_refuse_64krx(gRpEverRetiled64krx, d) ||
           n48_rp_known_pageout(gRpKnownRes, kRpKnownRes, reinterpret_cast<uint64_t>(r));   // build 0.0.493
}
// The resource's shape, read from its own fields ( offsets; ws_resprov.h n48_rp_shape).
static n48_rp_shape resource_shape(const char *r, const char **cfgWhy) {
    n48_rp_shape s {};
    s.surf = *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    void *mask = *reinterpret_cast<void *const *>(r + 0x180);
    s.hasMask = mask ? 1u : 0u;
    //: res+0x180 is the per-resource surface record AMDAccelResource::initialize allocates as 0x3c8 x count bytes (0xbdd154d
    // imull $0x3c8; 0xbdd155b alloc; 0xbdd1560 movq %rax,0x180(%rbx)) and fills from the create args +0x1f8 (0xbdd15a5), so +0x40
    // is inside it. setupHwCBRegs tells the hardware its +0x40 dword when it is set (0xbdf9141 cmoveq).
    //: maskOk says the record was READABLE. hasMask && !maskOk refuses (N48_RP_SHAPE_MASK): the hardware would read a
    // dword we never saw. A readable record decides on its own; res+0x1dc is then only a warning (n48_rp_mask_warn).
    if (reinterpret_cast<uintptr_t>(mask) >= kKernelHalfBase) {
        s.maskSurf = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(mask) + 0x40);
        s.maskOk = 1u;
    } else if (mask) s.maskSurf = 0xffffffffu;       // not a kernel pointer: maskOk stays 0 and the shape refuses MASK
    s.w = *reinterpret_cast<const uint16_t *>(r + 0xb0);
    s.h = *reinterpret_cast<const uint16_t *>(r + 0xb2);
    s.depth = *reinterpret_cast<const uint32_t *>(r + 0xb4);
    s.rowBytes = *reinterpret_cast<const uint32_t *>(r + 0xb8);
    s.bytes = *reinterpret_cast<const uint64_t *>(r + 0x230);
    *cfgWhy = "not read (shape refused first)";
    if ((n48_rp_swz_dword(&s) & 0x1fu) == N48_RP_G10_4KB_D_X) s.gbRead = (uint32_t)apple_gb_addr_config(r, &s.gb, cfgWhy);
    return s;
}
// Two buffers of the resource's size, freed on every exit (the gfx10 re-tiles), or the backing-sourced copy's STREAM.
// build 0.0.492: the 0.0.486 fields `nd` / `kind` (a whole gfx12 image in `dst`, at most 1 MiB) became `ls`, the heap-held
// stream of the backing-sourced copy (RpLinStream below: the plan, its two chunk buffers of at most N48_RP_MAX_BYTES each, and
// the chunk now held); `lin` bit 0 = switch 59 is ON and this copy's record says the backing is LINEAR, so the gfx10 re-tiles must
// refuse it (ws_resprov.h n48_rp_lin_blocks_old). Same 32 bytes as 0.0.491's struct (`n` <= N48_RP_MAX_BYTES fits 32 bits: the
// shape checks that set it demand bytes <= N48_RP_MAX_BYTES), so hook_page_texture's frame does not grow for it.
struct RpLinStream;
static void rp_lin_stream_free(RpLinStream *ls);
struct RtBufs {
    uint32_t *src { nullptr }, *dst { nullptr };
    RpLinStream *ls { nullptr };   // build 0.0.492: non-null = the backing-sourced copy was prepared (switch 59 ON, check OK)
    uint32_t n { 0 };              // src/dst size (the gfx10 re-tiles only)
    // build 0.0.492: bit 0 (N48_RT_LINBLK) = the gfx10 re-tiles must refuse this copy (linear backing, 59 ON); bits [31:1] = the
    // prepared gfx12 length (a whole number of 256-byte blocks, <= N48_RP_LIN_MAX_TOTAL = 64 MiB, so bit 0 is always free; 0 = no
    // stream). One word, read like 0.0.486's rt.nd, so the inlined copy keeps no new 64-bit value live across its calls.
    uint32_t lin { 0 };
    ~RtBufs() { if (src) IOFree(src, n); if (dst) IOFree(dst, n); if (ls) rp_lin_stream_free(ls); }
};
static constexpr uint32_t N48_RT_LINBLK = 1u;
static_assert(N48_RP_LIN_MAX_TOTAL < (1ull << 32), "the prepared gfx12 length fits RtBufs::lin");
static_assert(sizeof(RtBufs) == 32, "build 0.0.492: RtBufs lives in hook_page_texture's frame (inlined): it must not grow");
static_assert(N48_RP_LIN_MAX_TOTAL == kCopyMaxBytes, "ws_resprov.h's total bound is the copy's own (kCopyMaxBytes)");

// ---------------------------------------------------------------------------
// build 0.0.486 (an internal design note Q6) - THE BACKING-SOURCED COPY (ws_resprov.h section 6), GENERALISED AND
// STREAMED BY build 0.0.492.
// ---------------------------------------------------------------------------
// Called by residency_copy_to_vram ONLY while n48::hw_resprov_on() (switch 11 with the descriptor path), BEFORE its pre-flight,
// for every copy; returns at once for anything but a Stretch / SurfaceCopy texture (nibble 0, 1, 7, 8), reading nothing.
//   STEP 0 (no behaviour change, independent of switch 59): four capped lines (kRpBkLines copies per boot) with the resource's
//     fields, the record's level-0 backing fields (the ones fillUBMSurfaceInfoBacking reads, ws_resprov.h section 6), the
//     lengths, what n48_rp_lin_check says, 16 dwords at the backing offset and (0.0.492,) 16 dwords of the MIDDLE row and the
//     non-zero bytes over the whole source extent. Every read is a field read or a read of the same backing descriptor the copy
//     reads, bounded by its length; the extent count reads through one 64 KiB heap buffer, freed before returning.
//   STEP 1 (switch 59 ON and the check OK): the STREAM is prepared - the plan (the VRAM side's gfx12 mode, the element size, the
//     block rows, the chunk bound), the in-block tables and two chunk buffers, each at most N48_RP_MAX_BYTES (1 MiB). Nothing is
//     read from the backing and nothing is written to VRAM here: the caller walks the VRAM guard over the whole gfx12 length
//     (rt->ls->p.g12Bytes) first, then its own verified write loop pulls the gfx12 bytes through rp_lin_fetch, which reads and
//     converts one chunk of block rows at a time (n48_rp_lin_chunk / n48_rp_lin_band). A failure here (allocation) is counted and
//     leaves today's copy, untouched; a failure inside the loop is the loop's own FAILED path (poisoned, the skip kept).
// WHY STREAMED, AND WHY IT IS SAFE (item 4): the wallpaper's gfx12 image is 0x870000 bytes, over the 1 MiB any one kext buffer may
//     hold. Streaming keeps every buffer at <= 1 MiB (two per copy), every chunk's source span inside the extent n48_rp_lin_check
//     proved inside srcLen / the descriptor / res+0x230, and every chunk's destination inside [0, g12Bytes) - which the pre-flight
//     already walked through navi48_vram_apple_dest_check segment by segment, and which the write loop re-checks per MM chunk
//     exactly as it does for today's plain copy of the same resource (0x7e9000 bytes today). The loop still writes in the same
//     256-byte batches with a read-back of each, inside the same copy-guard scope, in Apple's pageTexture call: the only new work
//     per chunk is one readBytes of at most 1 MiB and a CPU conversion of it. Refusing instead would leave the wallpaper unproven.
// noinline: its locals (the check's input, 16 dwords) stay out of residency_copy_to_vram's frame.
struct RpLinStream {
    n48_rp_lin_plan p;
    n48_rp_lin_lut lut;
    IOMemoryDescriptor *md;
    uint64_t backingOffset;
    uint8_t *src, *dst;
    uint32_t srcCap, dstCap;
    uint64_t lo, hi;               // the gfx12 bytes [lo, hi) now in dst; hi == 0: nothing converted yet
    uint32_t w, h;
    // build 0.0.493: known = row + 1 of a KNOWN-ASSET conversion (ws_resprov.h section 7), 0 otherwise. Such a stream holds the
    // WHOLE gfx12 image in dst from the start ([lo, hi) = [0, g12Bytes)), so rp_lin_fetch never reads or converts a chunk for it
    // (its plan has no chunk geometry: a fetch outside [lo, hi) fails closed). res = the resource, for the one log line.
    uint32_t known, pad;
    const void *res;
};
static void rp_lin_stream_free(RpLinStream *ls) {
    if (!ls) return;
    if (ls->src) IOFree(ls->src, ls->srcCap);
    if (ls->dst) IOFree(ls->dst, ls->dstCap);
    IOFree(ls, sizeof(*ls));
}
// The gfx12 bytes [off, off + take) of the image, into `out`: the chunk holding each byte is read from the backing and converted
// first when it is not the one held. 1, or 0 on any refusal (the caller's FAILED path). take <= 256 (the write loop's batch).
static int __attribute__((noinline)) rp_lin_fetch(RpLinStream *ls, uint64_t off, uint8_t *out, uint64_t take) {
    if (!ls || !out || take > 256u || off > ls->p.g12Bytes || take > ls->p.g12Bytes - off) return 0;
    while (take) {
        if (!(ls->hi && off >= ls->lo && off < ls->hi)) {
            uint32_t j0 = 0, nr = 0; uint64_t sf = 0, sl = 0, df = 0, dl = 0;
            if (!n48_rp_lin_chunk(&ls->p, ls->w, ls->h, off, &j0, &nr, &sf, &sl, &df, &dl)) return 0;
            if (sl > ls->srcCap || dl > ls->dstCap || ls->backingOffset > ~0ull - sf) return 0;
            if (ls->md->readBytes(ls->backingOffset + sf, ls->src, sl) != sl) return 0;
            if (!n48_rp_lin_band(&ls->p, &ls->lut, ls->w, ls->h, j0, nr, ls->src, sl, ls->dst, dl)) return 0;
            ls->lo = df; ls->hi = df + dl;
            n48::hw_resprov_lin_note(N48_RP_LINEV_CHUNK);
        }
        const uint64_t c = (take < ls->hi - off) ? take : ls->hi - off;
        memcpy(out, ls->dst + (off - ls->lo), (size_t)c);
        out += c; off += c; take -= c;
    }
    return 1;
}
// The write loop's source for a re-tiled copy (hook_page_texture's frame is at its cap: one call, no new live value there).
// build 0.0.493 - A KNOWN-ASSET ROW FOR NIBBLE-4 TILED IMAGES (ws_resprov.h section 7; switch 59, DEFAULT OFF).
// Called by rp_lin_prepare for a nibble-4 copy (pageTextureWithMemcpy), and nothing else. Switch 59 OFF: returns at once, reading
// nothing (0.0.492 returned 0 for nibble 4 at the same point). ON, in this order - each step a refusal that keeps the plain copy:
//   FACTS  the resource's own fields against a row (n48_rp_known_row_for); no row = silent return, nothing read from the backing;
//   CONFIG Apple's gfx10 GB_ADDR_CONFIG through the same guarded read the gfx10 re-tile uses (apple_gb_addr_config);
//   READ   the whole allocation (res+0x230 bytes at the backing offset, inside the descriptor: the caller checked mdLen) into a heap
//          buffer of exactly that size;
//   KEY    its first 32 dwords against the row's (n48_rp_known_key_ok) - a mismatch is logged once per row;
//   CONVERT the whole allocation to gfx12 ADDR3_4KB_2D (n48_rp_known_convert: n48_rp_retile, the golden-tested equations);
//   REMEMBER the resource for the page-out refusal (a full table refuses here, before anything is written).
// The stream then carries the whole gfx12 image, and the caller's OWN pre-flight (VRAM guard), copy-guard scope, verified write
// loop (rp_retile_bytes -> rp_lin_fetch) and record run exactly as for 0.0.492's backing-sourced copy: no new write path. The gfx12
// length equals res+0x230 (one 4 KiB block), so wBytes == bytes.
static uint64_t __attribute__((noinline)) rp_known_prepare(const char *r, IOMemoryDescriptor *md, RtBufs *rt) {
    if (!n48::hw_resprov_lin_on()) return 0ull;   // switch 59 OFF: nothing read (0.0.492's return for nibble 4)
    n48_rp_known_in in {};
    in.nibble = (*reinterpret_cast<const uint32_t *>(r + 0x1b4) >> 26) & 0xfu;
    in.resType = *reinterpret_cast<const uint8_t *>(r + 0x14);
    in.w = *reinterpret_cast<const uint16_t *>(r + 0xb0);
    in.h = *reinterpret_cast<const uint16_t *>(r + 0xb2);
    in.rowBytes = *reinterpret_cast<const uint32_t *>(r + 0xb8);
    in.bytes = *reinterpret_cast<const uint64_t *>(r + 0x230);
    in.backingOffset = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    const uint32_t row = n48_rp_known_row_for(&in);
    if (row == N48_RP_KNOWN_NONE) return 0ull;   // FACTS: not a known asset (every other nibble-4 copy): silent, nothing read
    static uint32_t sLogged[N48_RP_KNOWN_N][N48_RP_KN_REASONS] {};
    uint32_t why = N48_RP_KN_OK;
    uint32_t gb = 0u;
    const char *cfgWhy = "unread";
    const uint32_t gbRead = (uint32_t)apple_gb_addr_config(r, &gb, &cfgWhy);
    RpLinStream *ls = nullptr;
    const uint64_t mdLen = (uint64_t)md->getLength();
    if (gbRead != 1u || !n48_rp_g10_cfg_ok(gb)) why = N48_RP_KN_CFG;                                       // CONFIG
    else if (in.backingOffset > mdLen || in.bytes > mdLen - in.backingOffset || in.bytes > N48_RP_MAX_BYTES) why = N48_RP_KN_READ;
    else if (!(ls = static_cast<RpLinStream *>(IOMalloc(sizeof(RpLinStream))))) why = N48_RP_KN_ALLOC;
    else {
        memset(ls, 0, sizeof(*ls));
        ls->srcCap = ls->dstCap = (uint32_t)in.bytes;
        ls->src = static_cast<uint8_t *>(IOMalloc(ls->srcCap));
        ls->dst = static_cast<uint8_t *>(IOMalloc(ls->dstCap));
        if (!ls->src || !ls->dst) why = N48_RP_KN_ALLOC;
        else if (md->readBytes(in.backingOffset, ls->src, in.bytes) != in.bytes) why = N48_RP_KN_READ;           // READ
        else if (!n48_rp_known_key_ok(row, reinterpret_cast<const uint32_t *>(ls->src), in.bytes)) why = N48_RP_KN_KEY;   // KEY
        else why = n48_rp_known_convert(row, gbRead, gb, reinterpret_cast<const uint32_t *>(ls->src),                 // CONVERT
                                        reinterpret_cast<uint32_t *>(ls->dst), in.bytes);
        if (why == N48_RP_KN_OK && !n48_rp_known_res_add(gRpKnownRes, kRpKnownRes, reinterpret_cast<uint64_t>(r)))    // REMEMBER
            why = N48_RP_KN_TABLE;
    }
    if (why != N48_RP_KN_OK) {
        rp_lin_stream_free(ls);
        n48::hw_resprov_lin_note(why == N48_RP_KN_KEY ? N48_RP_LINEV_KNOWNKEY : N48_RP_LINEV_KNOWNFAIL);
        if (why < N48_RP_KN_REASONS && __atomic_exchange_n(&sLogged[row][why], 1u, __ATOMIC_RELAXED) == 0u)
            PEERLOG(N48_RP_KNOWN_REFUSED_FMT, row, kN48RpKnownNib4[row].name, r,
                    why == N48_RP_KN_CFG ? cfgWhy : n48_rp_known_why(why), why);
        return 0ull;
    }
    __atomic_store_n(&gRpKnownAny, 1u, __ATOMIC_RELEASE);
    ls->p.g10Mode = N48_RP_G10_4KB_D_X; ls->p.g12Mode = N48_RP_G12_4KB_2D; ls->p.blkLog2 = 12u; ls->p.bpeLog2 = 2u; ls->p.bpe = 4u;
    ls->p.g12Bytes = in.bytes;                 // chunk geometry stays 0: n48_rp_lin_chunk refuses any fetch outside [lo, hi)
    ls->lo = 0ull; ls->hi = in.bytes;          // the whole gfx12 image is held
    ls->md = md; ls->backingOffset = in.backingOffset; ls->w = in.w; ls->h = in.h;
    ls->known = row + 1u; ls->res = r;
    rt->ls = ls;
    rt->lin |= (uint32_t)in.bytes;             // 0x1000: bit 0 (the gfx10 re-tile refusal flag) stays 0
    return in.bytes;
}
static int __attribute__((noinline)) rp_retile_bytes(RtBufs *rt, uint64_t off, uint8_t *out, uint64_t take) {
    if (rt->ls) return rp_lin_fetch(rt->ls, off, out, take);
    memcpy(out, reinterpret_cast<const uint8_t *>(rt->dst) + off, (size_t)take);   // 0.0.491's line, unchanged
    return 1;
}
static constexpr uint64_t kRpBkLines = 128ull;
static uint64_t gRpBkN { 0ull };
// build 0.0.492: step 0's non-zero-byte count over the source extent [from, to) of the backing, through one 64 KiB heap
// buffer. 0 = not counted (allocation, a short read, or an extent past the descriptor).
static int __attribute__((noinline)) rp_step0_count(IOMemoryDescriptor *md, uint64_t mdLen, uint64_t from, uint64_t to, uint64_t *nz) {
    static constexpr uint32_t kBuf = 64u * 1024u;
    *nz = 0ull;
    if (to < from || to > mdLen) return 0;
    uint8_t *b = static_cast<uint8_t *>(IOMalloc(kBuf));
    if (!b) return 0;
    int ok = 1;
    for (uint64_t at = from; at < to; ) {
        const uint64_t n = (to - at < kBuf) ? to - at : kBuf;
        if (md->readBytes(at, b, n) != n) { ok = 0; break; }
        for (uint64_t i = 0; i < n; i++) *nz += b[i] ? 1u : 0u;
        at += n;
    }
    IOFree(b, kBuf);
    return ok;
}
// ws_resprov.h's pure check, out of line (its many scalars would otherwise be spilled into rp_lin_prepare's frame).
static uint32_t __attribute__((noinline)) rp_lin_check_ool(const n48_rp_lin_in *in, n48_rp_lin_plan *p) { return n48_rp_lin_check(in, p); }
// STEP 1's allocation (noinline: its locals stay out of rp_lin_prepare's frame): the stream holding the plan, the in-block tables
// and the two chunk buffers. Returns the gfx12 length, or 0 (counted and logged; the plain copy is kept).
static uint64_t __attribute__((noinline)) rp_lin_stream_make(const n48_rp_lin_plan *p, IOMemoryDescriptor *md, uint64_t backingOffset,
                                                             uint32_t w, uint32_t h, uint64_t serial, const char *r, RtBufs *rt) {
    const char *fail = nullptr;
    RpLinStream *ls = static_cast<RpLinStream *>(IOMalloc(sizeof(RpLinStream)));
    if (ls) {
        memset(ls, 0, sizeof(*ls));
        ls->p = *p; ls->md = md; ls->backingOffset = backingOffset; ls->w = w; ls->h = h;
        // the largest chunk: chunkRows bands of gfx12 bytes; its source span is at most chunkRows x bh rows of pitch
        ls->dstCap = (uint32_t)(p->bandBytes * p->chunkRows);
        ls->srcCap = (uint32_t)((p->pitchBytes << p->bhLog2) * p->chunkRows);
        if (!n48_rp_lin_lut_fill(&ls->p, &ls->lut)) fail = "the in-block tables";
        // build 0.0.544 4a: a one-band chunk may reach N48_RP_LIN_BAND_MAX (2 MiB: the 2560-wide wallpaper's 1.25 MiB row)
        else if (ls->dstCap == 0u || ls->srcCap == 0u || ls->dstCap > N48_RP_LIN_BAND_MAX || ls->srcCap > N48_RP_LIN_BAND_MAX)
            fail = "a chunk bound";
        else {
            ls->src = static_cast<uint8_t *>(IOMalloc(ls->srcCap));
            ls->dst = static_cast<uint8_t *>(IOMalloc(ls->dstCap));
            if (!ls->src || !ls->dst) fail = "allocation";
        }
    } else fail = "allocation";
    if (fail) {
        rp_lin_stream_free(ls);
        n48::hw_resprov_lin_note(N48_RP_LINEV_FAILED);
        PEERLOG("resprov: backing #%llu resource=%p switch-59 copy NOT prepared (%s) - the plain byte-for-byte copy is kept",
                (unsigned long long)serial, r, fail);
        return 0ull;
    }
    rt->ls = ls;
    rt->lin |= (uint32_t)p->g12Bytes;   // a multiple of 256 (whole blocks) <= 64 MiB: bit 0 stays the re-tile flag
    return p->g12Bytes;
}
// Step 0's fourth line: 16 dwords of the image's middle row and the non-zero bytes over the source extent (the element size from the
// resource's format row: n48_rp_lin_step0_geom; nothing is read when it is unknown, or past the descriptor).
static void __attribute__((noinline)) rp_step0_mid(const n48_rp_lin_in *in, IOMemoryDescriptor *md, uint64_t mdLen, uint64_t backingOffset,
                                                   uint64_t serial) {
    uint32_t dw[16] = { 0 };
    uint32_t row = 0u; uint64_t mid = 0ull, e0 = 0ull, e1 = 0ull, nz = 0ull;
    const bool geo = in->recOk && n48_rp_lin_step0_geom(in, &row, &mid, &e0, &e1) && backingOffset <= ~0ull - e1;
    const bool rdm = geo && backingOffset + mid <= mdLen && sizeof(dw) <= mdLen - (backingOffset + mid) &&
                     md->readBytes(backingOffset + mid, dw, sizeof(dw)) == sizeof(dw);
    const bool cnt = geo && rp_step0_count(md, mdLen, backingOffset + e0, backingOffset + e1, &nz);
    PEERLOG(N48_RP_BK4_FMT, N48_RP_BK4_ARGS(serial, row, backingOffset + mid, rdm, dw, nz, e1 - e0, backingOffset + e0,
            backingOffset + e1, cnt));
}
// STEP 0's four lines (0.0.486's three, verbatim, and 0.0.492's middle row / non-zero count), out of rp_lin_prepare's frame.
static void __attribute__((noinline)) rp_step0_log(const char *r, const char *rec, const n48_rp_lin_in *inp, IOMemoryDescriptor *md,
                                                   uint64_t mdLen, uint64_t backingOffset, uint64_t dstLen, uint64_t srcLen,
                                                   uint64_t bytes, uint32_t why, bool sw, uint64_t serial) {
    const n48_rp_lin_in &in = *inp;
    const unsigned nibble = in.nibble;
    n48::hw_resprov_lin_note(N48_RP_LINEV_STEP0);
    PEERLOG(N48_RP_BK1_FMT, N48_RP_BK1_ARGS(serial, r, nibble, in.fmtRes, in.w, in.h, in.depth, in.rowBytes,
            *reinterpret_cast<const uint8_t *>(r + 0xac), *reinterpret_cast<const uint8_t *>(r + 0xad), in.firstMip,
            *reinterpret_cast<const uint64_t *>(r + 0xc8), *reinterpret_cast<const uint64_t *>(r + 0xd0), in.hasE0,
            backingOffset, dstLen, srcLen, mdLen, bytes, why, sw));
    PEERLOG(N48_RP_BK2_FMT, N48_RP_BK2_ARGS(serial, rec, in.recOk, in.bw, in.bh, in.rec04,
            in.recOk ? *reinterpret_cast<const uint8_t *>(rec + 0x06) : 0u, in.fmtBk, in.off,
            in.recOk ? *reinterpret_cast<const uint32_t *>(rec + 0x2c) : 0u,
            in.recOk ? *reinterpret_cast<const uint16_t *>(rec + 0x34) : 0u,
            in.recOk ? *reinterpret_cast<const uint16_t *>(rec + 0x36) : 0u, in.pitch,
            in.recOk ? *reinterpret_cast<const uint16_t *>(rec + 0x3c) : 0u, in.swzVram, in.swzBk));
    uint32_t dw[16] = { 0 };
    const uint64_t at = backingOffset + in.off;
    const bool rd = in.recOk && at >= backingOffset && at <= mdLen && sizeof(dw) <= mdLen - at &&
                    md->readBytes(at, dw, sizeof(dw)) == sizeof(dw);
    PEERLOG(N48_RP_BK3_FMT, N48_RP_BK3_ARGS(serial, at, rd, dw));
}
// Five arguments (all in registers): the resource's own nibble, bytes and backing offset are re-read here from the same fields the
// caller read, and every bound below is checked against what THIS function read - the plan is self-consistent by construction.
static uint64_t __attribute__((noinline)) rp_lin_prepare(const char *r, IOMemoryDescriptor *md, uint64_t srcLen, uint64_t dstLen,
                                                         RtBufs *rt) {
    const unsigned nibble = (*reinterpret_cast<const uint32_t *>(r + 0x1b4) >> 26) & 0xfu;
    if (nibble == 4u) return rp_known_prepare(r, md, rt);   // build 0.0.493: the known-asset rows (switch 59 OFF: returns 0)
    if (nibble != 0u && nibble != 1u && nibble != 7u && nibble != 8u) return 0ull;   // not a texture flavour: nothing read
    n48_rp_lin_in in {};
    in.nibble = nibble;
    in.fmtRes = *reinterpret_cast<const uint32_t *>(r + 0x1b4) & 0xffu;
    in.w = *reinterpret_cast<const uint16_t *>(r + 0xb0);
    in.h = *reinterpret_cast<const uint16_t *>(r + 0xb2);
    in.depth = *reinterpret_cast<const uint32_t *>(r + 0xb4);
    in.rowBytes = *reinterpret_cast<const uint32_t *>(r + 0xb8);
    in.firstMip = *reinterpret_cast<const uint8_t *>(r + 0xae);
    in.hasE0 = *reinterpret_cast<void *const *>(r + 0xe0) ? 1u : 0u;
    const uint64_t bytes = *reinterpret_cast<const uint64_t *>(r + 0x230);
    const uint64_t backingOffset = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    const uint64_t mdLen = (uint64_t)md->getLength();
    in.bytes = bytes;
    in.backingOffset = backingOffset;
    const char *rec = *reinterpret_cast<const char *const *>(r + 0x180);
    in.recOk = reinterpret_cast<uintptr_t>(rec) >= kKernelHalfBase ? 1u : 0u;   // the 0x3c8-byte record: +0x47 is inside
    if (in.recOk) {
        in.bw = *reinterpret_cast<const uint16_t *>(rec + 0x00);
        in.bh = *reinterpret_cast<const uint16_t *>(rec + 0x02);
        in.rec04 = *reinterpret_cast<const uint16_t *>(rec + 0x04);
        in.fmtBk = *reinterpret_cast<const uint8_t *>(rec + 0x07);
        in.off = *reinterpret_cast<const uint64_t *>(rec + 0x08);
        in.pitch = *reinterpret_cast<const uint16_t *>(rec + 0x3a);
        in.swzVram = *reinterpret_cast<const uint32_t *>(rec + 0x40);
        in.swzBk = *reinterpret_cast<const uint32_t *>(rec + 0x44);
    }
    in.srcLen = srcLen; in.mdLen = mdLen; in.dstLen = dstLen;
    n48_rp_lin_plan p {};
    const uint32_t why = rp_lin_check_ool(&in, &p);   // build 0.0.492: out of line (this frame stays 0.0.491's size)
    const bool sw = n48::hw_resprov_lin_on();
    // build 0.0.492 item 3: switch 59 ON and a LINEAR backing - the gfx10 re-tiles refuse this copy, whatever the check says.
    rt->lin = n48_rp_lin_blocks_old(sw ? 1u : 0u, nibble, in.recOk, in.swzBk) ? N48_RT_LINBLK : 0u;
    const uint64_t serial = __atomic_add_fetch(&gRpBkN, 1ull, __ATOMIC_RELAXED);
    // STEP 0: four lines, each under the 491-byte body cap (tested). build 0.0.492: out of line (rp_step0_log), so this
    // frame holds only the check's input and plan.
    // The two helpers are called one after the other, never nested: the deepest stack is this frame plus the larger of the two.
    if (serial <= kRpBkLines) {
        rp_step0_log(r, rec, &in, md, mdLen, backingOffset, dstLen, srcLen, bytes, why, sw, serial);
        rp_step0_mid(&in, md, mdLen, backingOffset, serial);   // build 0.0.492: middle row, non-zero count
    }
    if (!n48_rp_lin_take(why, sw ? 1u : 0u)) return 0ull;   // switch 59 off, or any refusal: today's copy, nothing more read
    // STEP 1: every bound was proven by n48_rp_lin_check above; n48_rp_lin_chunk / n48_rp_lin_band re-check each chunk's.
    n48::hw_resprov_lin_note(N48_RP_LINEV_PLANNED);
    return rp_lin_stream_make(&p, md, backingOffset, in.w, in.h, serial, r, rt);
}
// build 0.0.492: the copy-side pieces of the backing-sourced copy that residency_copy_to_vram (inlined into hook_page_texture,
// whose frame is at its cap) would otherwise inline - each noinline, each touching only its arguments and the boot-lived statics.
static void __attribute__((noinline)) rp_lin_old_blocked(const void *self, const RpLinStream *ls) {
    n48::hw_resprov_lin_note(N48_RP_LINEV_OLDBLK);
    if (gRt.logged < kRpLogLines)
        PEERLOG("resprov: resource=%p gfx10 re-tile REFUSED (switch 59, \xc2\xa7" "1098): the record says the backing is LINEAR - "
                "the backing-sourced copy %s", self, ls ? "takes it" : "refused it too: the plain copy stands, unrecorded");
}
// The page-out flag of the VRAM side's own mode (n48_rp_lin_check admits only 2, 22, 27), set before the first write; returns the
// gfx12 mode the copy writes (the ledger's rc.mode).
static uint32_t __attribute__((noinline)) rp_lin_mark_pageout(const RpLinStream *ls) {
    if (ls->known) return ls->p.g12Mode;   // build 0.0.493: remembered per resource in rp_known_prepare, before this point
    if (ls->p.g10Mode == N48_RP_G10_256B_D) gRpEverRetiled256bd = 1;
    else if (ls->p.g10Mode == 27u) gRpEverRetiled64krx = 1;
    else gRpEverRetiled4kbdx = 1;
    return ls->p.g12Mode;
}
static const char *__attribute__((noinline)) rp_retile_suffix(const RpLinStream *ls, uint32_t kind) {
    if (ls && ls->known) return N48_RP_KNOWN_SUFFIX;   // build 0.0.493
    return ls ? n48_rp_lin_suffix(ls->p.g12Mode, ls->p.bpeLog2) : n48_rp_retile_suffix(0u, kind);
}
// A backing-sourced entry: its image's width and height (the ask matches the T# against them) and the format pairing's element size.
static void __attribute__((noinline)) rp_lin_fill_copy(n48_rp_copy *rc, const RpLinStream *ls) {
    rc->lin = 1u; rc->w = ls->w; rc->h = ls->h; rc->elemBytes = ls->p.bpe;
    if (ls->known) {   // build 0.0.493: a known-asset conversion names its row (n48_rp_record checks it against the row)
        rc->lin = N48_RP_LIN_KNOWN0 + ls->known - 1u;
        n48::hw_resprov_lin_note(N48_RP_LINEV_KNOWN);
        PEERLOG(N48_RP_KNOWN_FMT, N48_RP_KNOWN_ARGS(ls->known - 1u, ls->res, rc->compared, rc->mismatched));
        return;
    }
    n48::hw_resprov_lin_note(N48_RP_LINEV_WRITTEN);
}

// ---------------------------------------------------------------------------
// 0.0.204 — `kernsub` (action 46): substitute a gfx1201 blit kernel for Apple's
// Navi21 one . Apple's blit IB programs COMPUTE_PGM_LO/HI,
// RSRC1/2/3, the two V#s in USER_DATA_0-7 and DISPATCH_DIRECT itself and never
// changes (section 309/317); only the CODE BYTES at VA 0x400017b00 (VRAM
// lastDst + 0xfb00, our residency copy's destination) are Apple's GFX10 ISA that
// gfx1201 cannot execute. This overwrites those bytes with a byte-exact gfx1201
// re-encoding of the same copy (shaders/blit_copy_gfx1201.s), guarded by an
// exact match on Apple's original kernel so nothing else is ever touched.
// ---------------------------------------------------------------------------
extern "C" {
extern const uint8_t fw_shader_blit_copy_gfx1201[];
extern const size_t  fw_shader_blit_copy_gfx1201_size;
extern const uint8_t fw_shader_blit_diag_gfx1201[];
extern const size_t  fw_shader_blit_diag_gfx1201_size;
extern const uint8_t fw_shader_blit_diagmin_gfx1201[];
extern const size_t  fw_shader_blit_diagmin_gfx1201_size;
extern const uint8_t fw_shader_blit_copy_offen_gfx1201[];
extern const size_t  fw_shader_blit_copy_offen_gfx1201_size;
}

// Apple's original blit kernel: the 8 dwords at resource +0xfb00, measured in
// r28/r30 (sections 314, 317). The exact-match guard for the substitution. The public
// tree carries only a fingerprint of those 32 bytes (64-bit FNV-1a over the dwords in
// little-endian byte order), not the bytes themselves; the guard is unchanged in effect.
static constexpr uint64_t kAppleBlitKernelFnv = 0x5fe70be325ab2aabull;
static uint64_t apple_blit_fnv(const uint32_t *d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint32_t i = 0; i < 8; i++)
        for (uint32_t b = 0; b < 4; b++) { h ^= (d[i] >> (8u * b)) & 0xFFu; h *= 0x100000001b3ull; }
    return h;
}
static constexpr uint64_t kBlitShaderOffset = 0xfb00; // shader VA minus resource VA (section 311)

// 0.0.206 — the substitutable kernels, indexed by `kernsub <mode>`. Modes 2 and 3
// are INSTRUMENTS (rule 43): they write id registers into blit2's destination page
// and copy nothing . Apple's slot at +0xfb00 is 64 dwords: its
// 8-dword kernel, then 56 zero dwords, then the next kernel at +0xfc00 (r30's
// shader-page dump), so no kernel here may exceed kSubMaxDwords.
struct SubKernel { const uint8_t *bytes; const size_t *size; const char *name; const char *what; };
static const SubKernel kSubKernels[5] = {
    { nullptr, nullptr, "none", "none" },
    { fw_shader_blit_copy_gfx1201, &fw_shader_blit_copy_gfx1201_size,
      "blit_copy_gfx1201", "copy kernel" },
    { fw_shader_blit_diag_gfx1201, &fw_shader_blit_diag_gfx1201_size,
      "blit_diag_gfx1201", "DIAGNOSTIC kernel (INSTRUMENT: records A+B into the destination page, copies nothing)" },
    { fw_shader_blit_diagmin_gfx1201, &fw_shader_blit_diagmin_gfx1201_size,
      "blit_diagmin_gfx1201", "MINIMAL DIAGNOSTIC kernel (INSTRUMENT: record A only, copies nothing)" },
    // 0.0.207 : the copy with raw 128-bit load/store at a BYTE
    // offset (offen, index x 16), so no V# record size and no format apply.
    { fw_shader_blit_copy_offen_gfx1201, &fw_shader_blit_copy_offen_gfx1201_size,
      "blit_copy_offen_gfx1201", "copy kernel by byte offset (raw b128, offen)" },
};
static constexpr uint32_t kSubModes     = 4;
static constexpr uint32_t kSubMaxDwords = 32;

#if defined(N48_KERNSUB_DRAFTS)
// NOT BUILT — the follow-ups of an earlier analysis, drafted but deliberately not
// deployable: shaders/drafts/blit_copy_clamp_gfx1201.s (branch (i): the copy with
// the group id from the register r37 names, index clamped to 0..255) and
// shaders/drafts/blit_copy_global_gfx1201.s (branch (ii): the copy by address
// arithmetic, bypassing the V#'s record size). Enabling one is three deliberate
// steps, after its branch is MEASURED: move the .s into shaders/ and run
// tools/build-shader.sh (which embeds it), add it here as mode 4 or 5 with
// kSubModes raised, and add the mode to navi48test/accel-run. Until then this
// block only stops a half-enabled build.
#error "N48_KERNSUB_DRAFTS: embed the draft kernel with tools/build-shader.sh and extend kSubKernels/kSubModes first "
#endif

static uint32_t gSubMode { 0 };      // 0 = not armed; else the armed kSubKernels index
static struct { uint64_t subs, mismatched, lastAt; uint32_t lastReason; } gSub {};

static uint32_t sub_kernel_bytes(uint32_t mode) {
    return (mode >= 1 && mode <= kSubModes) ? (uint32_t)*kSubKernels[mode].size : 0u;
}

// Overwrite [vramAt, +size) with kernel `mode` IFF it currently holds Apple's
// exact Navi21 kernel and every dword past Apple's 8 up to our length is zero.
// Returns 1 substituted (read back clean), 2 already ours (idempotent), 3 written
// but the read-back mismatched, 0 refused (reason in *reason: 1 guard/aperture/bad
// kernel, 2 read failed, 3 not Apple's kernel / already changed, 4 write/read-back
// failed). Nothing is written unless the exact-match guard passes.
static uint32_t substitute_blit_kernel_at(uint64_t vramAt, uint32_t mode, uint32_t *reason) {
    *reason = 0;
    if (mode < 1 || mode > kSubModes) { *reason = 1; return 0; }
    const SubKernel &sk = kSubKernels[mode];
    const uint32_t nb = sub_kernel_bytes(mode);                       // 40 / 80 / 44
    const uint32_t nd = nb / 4u;                                      // 10 / 20 / 11
    if (nb == 0 || (nb & 3u) || nd < 8 || nd > kSubMaxDwords || (vramAt & 3)) { *reason = 1; return 0; }
    if (navi48_vram_apple_dest_check(vramAt, nb)) { *reason = 1; return 0; }
    const uint32_t *k = reinterpret_cast<const uint32_t *>(sk.bytes);
    uint32_t cur[kSubMaxDwords] = { 0 };
    if (!navi48_vram_read_mm(vramAt, cur, nd)) { *reason = 2; return 0; }
    bool alreadyOurs = true;
    for (uint32_t i = 0; i < nd; i++) if (cur[i] != k[i]) { alreadyOurs = false; break; }
    if (alreadyOurs) return 2;
    if (apple_blit_fnv(cur) != kAppleBlitKernelFnv)                     { *reason = 3; return 0; }
    for (uint32_t i = 8; i < nd; i++) if (cur[i] != 0)                   { *reason = 3; return 0; }
    uint32_t buf[kSubMaxDwords];
    for (uint32_t i = 0; i < nd; i++) buf[i] = k[i];
    if (gN48Sk82On) navi48_sk82_taint_mine();   // build 0.0.527 (SKIP82.md item 5): kernsub writes inside the copy's scope
    if (!navi48_vram_write_mm(vramAt, buf, nd)) { *reason = 4; return 0; }
    uint32_t back[kSubMaxDwords] = { 0 };
    if (!navi48_vram_read_mm(vramAt, back, nd)) { *reason = 4; return 0; }
    uint32_t bad = 0;
    for (uint32_t i = 0; i < nd; i++) if (back[i] != k[i]) bad++;
    gSub.subs++; gSub.lastAt = vramAt; gSub.mismatched += bad;
    PEERLOG("kernsub: SUBSTITUTED gfx1201 blit kernel at VRAM %#llx (%u dwords, first %08x %08x, "
            "last %08x); read-back %u mismatch(es); Apple's Navi21 kernel replaced by mode %u %s, "
            "a %s", (unsigned long long)vramAt, nd, back[0], back[1], back[nd - 1], bad,
            mode, sk.name, sk.what);
    return bad ? 3u : 1u;
}

// ---------------------------------------------------------------------------
// 0.0.239 — MILESTONE 3 step 2: the hash-keyed shader cache at the residency copy
// ---------------------------------------------------------------------------
//
// `kernsub` above substitutes ONE kernel, recognised by an exact byte compare against
// the eight dwords Apple's blit kernel happens to have, at ONE hard-coded resource
// offset (+0xfb00). That is what milestone 1 rests on, and it cannot generalise: a
// second shader, or the same shader at another offset, is invisible to it.
//
// This replaces the RECOGNITION, not the placement. The seam is the same residency
// copy, which already fires for every resource of every process. What changes is the
// key: instead of one hard-coded byte string, every 0x100-grid slot of the copied
// resource is looked up in a cache blob built offline (src/shadercache, format and
// evidence in its README), keyed by FNV-1a-64 over Apple's code through its first
// s_endpgm, and a hit is confirmed by a FULL byte compare of every unmasked dword
// before anything is written. The library refuses on its own for ambiguity
// (SC_E_AMBIGUOUS, two entries matching), for entries needing register adjustments
// this path cannot apply (accept_adjust = 0), and for a value that would not fit
// Apple's own allocation (SC_E_CAPACITY).
//
// The discipline of substitute_blit_kernel_at is kept verbatim on the write side:
// navi48_vram_apple_dest_check before any write, the MM window in 64-dword batches
// (rule 81: the helper takes at most 64 dwords and returns false, not a short write),
// and every dword read back and compared. `kernsub` is left in place and untouched
// for diagnosis; with both armed the cache runs first and kernsub then correctly
// refuses with reason 3, because the bytes are no longer Apple's.
extern "C" {
extern const uint8_t fw_shadercache[];
extern const size_t  fw_shadercache_size;
}

static bool     gScArmed { false };
static bool     gScOpened { false };
// 0.0.265, set ONLY by `shadercache 3`. See navi48_shadercache_control for why this
// is a separate mode and not a relaxed default: it does not apply any register anywhere.
static uint32_t gScAcceptAdjust { 0 };
static sc_cache gSc {};
static struct {
    uint64_t resources, windows, candidates, matches, substituted, mismatched, knownOnly;
    uint64_t refused, ambiguous, missNoKey, missCompare, missShort;
    uint64_t missEmpty, missNoEnd, gridSlots, gridEmpty, gridLongestRun, gridRunAt;
    uint64_t lastAt, lastOff, lastBytes, dupSkipped, readFails;
    int32_t  openStatus, lastRefusal;
} gScSt {};
// 0.0.437 — THE FLAG residency_copy_to_vram READS. shadercache_scan_resource/shadercache_hit
// run INSIDE residency_copy_to_vram's own Navi48CopyScope (the call site is below, after that scope is opened);
// "did this scan leave a partial write or an unstuck mismatch behind" is carried back to the copier through
// ScCtx (per-call state, below) and shadercache_scan_resource's own out-parameters, never a file-scope flag.
// 0.0.438 (ScCtx FLAGS FIX): these used to be file-scope globals, set inside shadercache_hit and read
// immediately after shadercache_scan_resource's call returned. Two residency copies running on different pageon
// threads at once (residency_copy_to_vram takes no lock of its own; only the MM window does) could interleave
// their scans and CLEAR each other's flag before its own owning call ever read it - one copy's real partial write
// silently vanishing because the OTHER copy's later, clean scan reset gScPartialWrite back to false first. Moved
// into ScCtx so each call's own flags live only as long as that call's own stack frame.

// A window big enough that a whole small resource is one pass, with the overlap the
// header itself dictates so a shader straddling a window boundary is still compared
// in full. Both buffers are file-scope: this runs on Apple's pageon thread.
static constexpr uint32_t kScWindowBytes    = 64u * 1024u;
// build 0.0.484 (an internal design note Q2 K1): 2048 -> 5376 (gfx_subst_caps.h's N48_SC_MAX_SUBST_BYTES). sc_subst_render
// below writes max(our image, Apple's program) bytes and refuses SC_E_CAPACITY when `sizeof gScOut` is smaller, so at 2048 a
// glass_background_lph value (5084/5076 B of code in Apple's 5376-byte allocation, rendered to 5376 B) could never be
// substituted at all, whatever the blob held. gScOut stays a file-scope static (.bss 2 KiB -> 5.25 KiB; nothing on the
// pageon thread's stack grows). The recognition side must render the same images: gfx_subst_caps.h's K2 is equal.
static constexpr uint32_t kScMaxSubstBytes  = N48_SC_MAX_SUBST_BYTES;
static_assert(kScMaxSubstBytes == N48_XD_SUBST_BYTES,
              "the residency writer (K1) and the recognition pool (K2) must render the same widest image, or a value "
              "that is substituted can never be recognised as ours (or one that is recognised is never written)");
static constexpr uint32_t kScMaxPerResource = 32u;   // rule 72: the cap is part of the query
static uint8_t gScWin[kScWindowBytes];
static uint8_t gScOut[kScMaxSubstBytes];

// 0.0.268 INSTRUMENT: the shader CENSUS. Every terminated program in a
// scanned resource is fingerprinted (sc_census.h) and each NEW fingerprint is logged ONCE with
// its size, first dword, the cache's verdict and the process the copy ran in. Read-only: it
// looks at gScWin, which the scan already filled with Apple's own bytes. Bounded three ways
// (rule 72): 256 unique fingerprints, a 0x1000-dword extent, and resources above 4 MiB are
// skipped and COUNTED, because a texture of junk would otherwise cost seconds in pageon.
// 0.0.269: test A REACHED the 256-key cap with 241 programs beyond it and two
// resources never fingerprinted. The key table is now 4096; the LOG keeps the first
// kCensusMaxLines new fingerprints verbatim and then every kCensusSampleEvery-th one as a
// SAMPLE, so the log cannot be flooded and what is counted past the line cap is still witnessed.
static constexpr unsigned kCensusMaxKeys      = 4096u;
static constexpr unsigned kCensusMaxLines     = 768u;
static constexpr unsigned kCensusSampleEvery  = 16u;
static constexpr uint32_t kCensusMaxDwords    = 0x1000u;
static constexpr uint64_t kCensusMaxResBytes  = 4ull * 1024u * 1024u;
struct CensusRec { uint64_t key; uint32_t dwords; uint32_t seen; uint8_t ws; };
// 0.0.275 (test A with liveness): every fingerprint WindowServer makes resident is logged verbatim, never
// sampled - a new key, or a key another process made resident first ("ALSO from") - so its lines are the work list.
static uint32_t gCensusWsLines = 0, gCensusWsKeys = 0;
static CensusRec gCensus[kCensusMaxKeys];
static unsigned  gCensusN = 0;
static uint64_t  gCensusPrograms = 0, gCensusOverflow = 0, gCensusSkippedRes = 0, gCensusResources = 0;
static uint64_t  gCensusUnlogged = 0;   // 0.0.269: new fingerprints past the line cap that were NOT sampled
struct CensusCtx { uint64_t resBytes; int pid; char name[32]; uint32_t programs, fresh; };

static const char *census_miss_name(uint32_t r) {
    switch (r) {
    case SC_MISS_EMPTY:   return "MISS empty";
    case SC_MISS_NOEND:   return "MISS no-terminator-within-cache-cap (longer than any cached key)";
    case SC_MISS_NOKEY:   return "MISS no-key (NOT IN CACHE)";
    case SC_MISS_COMPARE: return "MISS key-present-bytes-differ";
    case SC_MISS_SHORT:   return "MISS short";
    default:              return "MISS";
    }
}

static int census_note(void *vctx, const sc_census_item *it) {
    CensusCtx *cx = static_cast<CensusCtx *>(vctx);
    cx->programs++;
    gCensusPrograms++;
    const bool ws = !strncmp(cx->name, "WindowServer", sizeof(cx->name));
    for (unsigned i = 0; i < gCensusN; i++)
        if (gCensus[i].key == it->key && gCensus[i].dwords == it->dwords) {
            gCensus[i].seen++;
            if (ws && !gCensus[i].ws) {
                gCensus[i].ws = 1; gCensusWsKeys++; gCensusWsLines++;
                PEERLOG("shadercache: CENSUS #%u ALSO from pid %d (%s) [WS] key %#018llx %u dword(s) = %u B, first dword %#010x, "
                        "cache %s (status %d), at resource +%#llx of %#llx B", i + 1, cx->pid, cx->name,
                        (unsigned long long)it->key, it->dwords, it->dwords * 4u, it->first,
                        it->status == SC_OK ? "HIT" : (it->status == SC_MISS ? census_miss_name(it->miss_reason)
                                                                             : sc_status_name(it->status)),
                        it->status, (unsigned long long)it->offset, (unsigned long long)cx->resBytes);
            }
            return 0;
        }
    if (gCensusN >= kCensusMaxKeys) { gCensusOverflow++; return 0; }
    gCensus[gCensusN].key = it->key; gCensus[gCensusN].dwords = it->dwords; gCensus[gCensusN].seen = 1;
    gCensus[gCensusN].ws = ws ? 1 : 0;
    gCensusN++;
    cx->fresh++;
    if (ws) { gCensusWsKeys++; gCensusWsLines++; }
    if (!ws && gCensusN > kCensusMaxLines && ((gCensusN - kCensusMaxLines) % kCensusSampleEvery) != 0) {
        gCensusUnlogged++;
        return 0;
    }
    PEERLOG("shadercache: CENSUS #%u%s key %#018llx %u dword(s) = %u B, first dword %#010x, cache %s "
            "(status %d, cache key %#llx), at resource +%#llx of %#llx B, pid %d (%s)",
            gCensusN, ws ? " [WS]" : (gCensusN > kCensusMaxLines ? " (SAMPLE past the line cap)" : ""),
            (unsigned long long)it->key, it->dwords, it->dwords * 4u, it->first,
            it->status == SC_OK ? "HIT" : (it->status == SC_MISS ? census_miss_name(it->miss_reason)
                                                                 : sc_status_name(it->status)),
            it->status, (unsigned long long)it->cache_key, (unsigned long long)it->offset,
            (unsigned long long)cx->resBytes, cx->pid, cx->name);
    return 0;
}

static bool shadercache_open_once() {
    if (gScOpened) return true;
    const int st = sc_open(&gSc, fw_shadercache, fw_shadercache_size);
    gScSt.openStatus = st;
    if (st != SC_OK) {
        PEERLOG("shadercache: sc_open of the embedded %lu-byte blob REFUSED (%d %s) - substitution "
                "stays OFF for this boot", (unsigned long)fw_shadercache_size, st, sc_status_name(st));
        return false;
    }
    gScOpened = true;
    PEERLOG("shadercache: blob opened - %u entries, grid %#x, key cap %u dword(s), compare cap %u "
            "dword(s), %lu bytes. Every stored key was recomputed from its own stored bytes at open.",
            sc_entry_count(&gSc), gSc.grid, gSc.max_key_dwords, gSc.max_apple_dwords,
            (unsigned long)fw_shadercache_size);
    return true;
}

struct ScCtx {
    void         *dstMem;
    PhysSegmentFn dSeg;
    uint64_t      winBase;
    uint64_t      done[kScMaxPerResource];
    uint32_t      nDone;
    // 0.0.438 (ScCtx FLAGS FIX): per-call state, not file-scope - see the comment on the old
    // gScPartialWrite/gScMismatch globals above. Zero-initialised by `ScCtx ctx {};` at shadercache_scan_resource's
    // own entry; set only inside shadercache_hit's PARTIAL WRITE / mismatch paths, never cleared mid-scan, so a
    // later successful substitution in the SAME resource can never paper over an earlier one's damage.
    bool          partialWrite;
    bool          mismatch;
};

// One verified candidate. Returns 0 so the scan continues whatever happens here:
// a refusal is per-shader, never a reason to stop looking at the rest.
static int shadercache_hit(void *vctx, size_t off, const sc_match *m) {
    ScCtx *c = static_cast<ScCtx *>(vctx);
    const uint64_t resOff = c->winBase + (uint64_t)off;
    uint32_t nlen = 0;
    const char *nm = sc_entry_name(&gSc, m, &nlen);
    gScSt.matches++;

    if (!m->verified) {                       // a hash-only entry: we know OF it, we cannot place it
        gScSt.knownOnly++;
        PEERLOG("shadercache: resource +%#llx is entry '%s' by key %#llx but KNOWN-ONLY (no stored "
                "Apple bytes to compare) - nothing written", (unsigned long long)resOff,
                nm ? nm : "?", (unsigned long long)m->key);
        return 0;
    }
    // 0.0.244: this is now a CONTROL, not the mechanism. sc_scan_window gives each
    // window exclusive ownership of its starts and carries a resource-wide cursor over
    // the boundary, so the library no longer offers the same slot twice - and it skips a
    // matched span whether or not this callback substituted it, which is exactly the
    // refused-hit-in-an-overlap case that used to be counted twice. A nonzero
    // dupSkipped now means that ownership is broken, so it is reported rather than
    // silently doing its old job.
    for (uint32_t i = 0; i < c->nDone; i++)
        if (c->done[i] == resOff) { gScSt.dupSkipped++; return 0; }

    //: a GRAPHICS substitution needs the same explicit opt-in (`accel shadercache 3`) as an entry
    // with register adjustments. Writing our gfx1201 vertex or fragment program over Apple's does
    // nothing useful until the source hook can translate the IB around it (: it cannot yet), so
    // an unasked-for one is a change with no upside on a boot that wanted only the compute path.
    // This is a NO-OP for every entry the blob held before 0.0.304: all four SkyLight graphics
    // entries and g2_tri_fs carry adjustments and were already behind mode 3, and the one
    // substitutable entry with no adjustments, gShaderCode_gfx10_BufferCopy_CS, is COMPUTE.
    if ((m->stage == SC_STAGE_VERTEX || m->stage == SC_STAGE_FRAGMENT) && !gScAcceptAdjust) {
        gScSt.refused++;
        PEERLOG("shadercache: resource +%#llx '%s' key %#llx is a %s program - graphics substitution is "
                "OPT-IN (accel shadercache 3); nothing written", (unsigned long long)resOff, nm ? nm : "?",
                (unsigned long long)m->key, m->stage == SC_STAGE_VERTEX ? "vertex" : "fragment");
        return 0;
    }
    uint32_t nb = 0;
    // build 0.0.536 (switch 92, gfx_rv92.h): the image written - today's (OFF, and every other key), or ON the blob's v3
    // alternative of RectPosTexFast_VS. `m` stays the verified match for everything else below (name, key, capacity).
    sc_match alt92; uint32_t why92 = N48_RV92_OLD;
    const sc_match *use92 = n48_rv92_pick(&gSc, m, n48::hw_rv92_on() ? 1u : 0u, &alt92, &why92);
    const int rst = sc_subst_render(&gSc, use92, (int)gScAcceptAdjust, gScOut, sizeof gScOut, &nb);
    if (rst != SC_OK || nb == 0 || (nb & 3u)) {
        gScSt.refused++; gScSt.lastRefusal = rst;
        PEERLOG("shadercache: resource +%#llx '%s' key %#llx MATCHED but not substituted: %d %s "
                "(adjustments are refused until the IB hook can apply them, and a value may never "
                "exceed Apple's own %u-byte allocation)", (unsigned long long)resOff, nm ? nm : "?",
                (unsigned long long)m->key, rst, sc_status_name(rst), m->capacity_bytes);
        return 0;
    }
    uint64_t span = 0;
    const uint64_t at = c->dSeg(c->dstMem, resOff, &span);
    if (!at || (at & 3) || span < nb) {
        gScSt.refused++;
        PEERLOG("shadercache: resource +%#llx '%s': destination segment unusable (VRAM %#llx span "
                "%#llx, need %u) - nothing written", (unsigned long long)resOff, nm ? nm : "?",
                (unsigned long long)at, (unsigned long long)span, nb);
        return 0;
    }
    const uint32_t why = navi48_vram_apple_dest_check(at, nb);
    if (why) {
        gScSt.refused++;
        PEERLOG("shadercache: resource +%#llx '%s': VRAM [%#llx,%#llx) refused by the VRAM guard, "
                "reason %u - nothing written", (unsigned long long)resOff, nm ? nm : "?",
                (unsigned long long)at, (unsigned long long)(at + nb), why);
        return 0;
    }
    uint32_t bad = 0;
    bool ok = true;
    // build 0.0.527 (SKIP82.md item 5): VRAM will hold more than the source - this thread's copy cannot establish a switch-82 entry.
    if (gN48Sk82On) navi48_sk82_taint_mine();
    for (uint32_t o = 0; o < nb && ok; o += 256u) {          // rule 81: 64 dwords per call, no more
        const uint32_t take = (nb - o) > 256u ? 256u : (nb - o);
        const uint32_t nd = take / 4u;
        uint32_t w[64] = { 0 }, b[64] = { 0 };
        memcpy(w, gScOut + o, take);
        ok = navi48_vram_write_mm(at + o, w, nd) && navi48_vram_read_mm(at + o, b, nd);
        if (ok) for (uint32_t i = 0; i < nd; i++) if (b[i] != w[i]) bad++;
    }
    if (!ok) {
        gScSt.refused++;
        // 0.0.437: a batch that landed before the refusing one is already in VRAM - the copy's
        // range must be poisoned exactly as residency_copy_to_vram's own PARTIAL WRITE paths poison it.
        c->partialWrite = true;
        PEERLOG("shadercache: resource +%#llx '%s': the MM window refused a batch at VRAM %#llx - "
                "PARTIAL WRITE", (unsigned long long)resOff, nm ? nm : "?", (unsigned long long)at);
        return 0;
    }
    // 0.0.437: written, but the read-back did not stick - the same VERIFIED hazard
    // residency_copy_to_vram's own `if (mismatched) cgScope.markMismatch();` guards against, for this write too.
    if (bad) c->mismatch = true;
    n48::hw_hg_note_substituted(at, nb);   // build 0.0.495: the switch-62 registry (a no-op until 62 is first turned ON)
    if (why92 != N48_RV92_OLD) n48::hw_rv92_note(why92);   // build 0.0.536: switch 92's substitution counters
    gScSt.substituted++; gScSt.mismatched += bad;
    gScSt.lastAt = at; gScSt.lastOff = resOff; gScSt.lastBytes = nb;
    if (c->nDone < kScMaxPerResource) c->done[c->nDone++] = resOff;
    const uint32_t *o32 = reinterpret_cast<const uint32_t *>(gScOut);
    PEERLOG("shadercache: SUBSTITUTED '%s'%s (key %#llx, %u Apple dword(s), capacity %#x) at resource "
            "+%#llx = VRAM %#llx, %u byte(s) written as %08x %08x ... ; read-back %u mismatch(es)",
            nm ? nm : "?", why92 == N48_RV92_NEW ? " [switch 92: the v3 image]" : why92 == N48_RV92_NOALT ?
            " [switch 92 ON: no alternative, today's image]" : "", (unsigned long long)m->key, m->apple_dwords, m->capacity_bytes,
            (unsigned long long)resOff, (unsigned long long)at, nb, o32[0], nb > 4 ? o32[1] : 0u, bad);
    return 0;
}

// Scan the resource that was just copied and substitute every shader the cache holds.
// Reads Apple's bytes from the BACKING descriptor, not back out of VRAM: that is the
// same source the copy itself used, so a hit is keyed on exactly what was uploaded.
// 0.0.438 (ScCtx FLAGS FIX): `outPartialWrite`/`outMismatch` replace the retired gScPartialWrite/
// gScMismatch globals - each call writes only ITS OWN ScCtx's flags out through these, so two concurrent calls on
// different threads can never clear one another's answer. Either pointer may be null; both default false so an
// early return (armed off, bad arguments) reports clean, exactly as the reset-then-early-return order used to.
static void shadercache_scan_resource(void *dstMem, PhysSegmentFn dSeg, IOMemoryDescriptor *md,
                                      uint64_t backingOffset, uint64_t bytes,
                                      bool *outPartialWrite, bool *outMismatch) {
    if (outPartialWrite) *outPartialWrite = false;
    if (outMismatch) *outMismatch = false;
    if (!gScArmed || !gScOpened || !dstMem || !dSeg || !md || bytes < 4) return;
    // 0.0.244 (src/shadercache/README.md, "Coordinator integration"). The LIBRARY now
    // owns the window accounting. sc_scan_window gives each window EXCLUSIVE ownership of
    // the grid starts in [base, base + owned) and carries a resource-wide cursor across
    // the boundary, so a match that spans two windows - and, the case that mattered, a
    // match this callback REFUSES - is counted exactly once. The old arrangement re-scanned
    // the whole overlap with sc_scan and suppressed the duplicate after the fact inside the
    // callback, which counted a refused overlap hit twice and left the resource total
    // depending on callback behaviour.
    //
    // The LOOKAHEAD is the library's requirement, not ours: sc_scan_window refuses with
    // SC_E_ARG unless a non-final window carries 4 x max(max_key_dwords, max_apple_dwords)
    // bytes beyond its owned range. The pre-0.0.244 overlap covered max_apple_dwords ONLY,
    // so it cannot simply be reused here - a cache whose longest KEY exceeds its longest
    // compare would have refused every window and scanned nothing.
    uint32_t overlap = (gSc.max_apple_dwords > gSc.max_key_dwords
                        ? gSc.max_apple_dwords : gSc.max_key_dwords) * 4u;
    if (gSc.grid) overlap = (overlap + gSc.grid - 1u) & ~(gSc.grid - 1u);
    if (overlap > kScWindowBytes / 2u) overlap = kScWindowBytes / 2u;
    const uint32_t stride = kScWindowBytes - overlap;

    ScCtx ctx {};
    ctx.dstMem = dstMem; ctx.dSeg = dSeg;
    uint64_t pos = 0, cand = 0, amb = 0, nokey = 0, cmp = 0, shrt = 0;
    uint64_t empty = 0, noend = 0, mat = 0, errs = 0, winBad = 0;
    size_t next = 0;             // the resource-wide cursor, initialised ONCE per resource
    // 0.0.242 (an earlier analysis defect 3, rules 25/72). r78's closing line asserted its
    // counts reconciled and they did not: 414 candidates - 13 matches - 327 no-key left 74
    // unaccounted, because the line printed neither miss_empty nor miss_noend. Both terms
    // are already in sc_scan_stats, so this is a printing fix, not a library change.
    //
    // Beside them, the OCCUPANCY of Apple's own 0x100 grid, which is what decides whether a
    // relocated vertex program (580/596/804 bytes, so 3 or 4 consecutive slots) could live in
    // space Apple has already mapped, with no new mapping at all (the review's option 6c).
    // Each window is counted from `startAt` so the overlap is never counted twice, and the
    // run length carries across windows.
    uint64_t slots = 0, emptySlots = 0, longestRun = 0, longestAt = 0, run = 0, runAt = 0;
    uint32_t windows = 0;
    // 0.0.268 census context: the process this copy runs in (proc_selfname reads the current
    // proc without a lookup, so no proc-list lock is taken on Apple's pageon thread).
    CensusCtx census {};
    census.resBytes = bytes;
    census.pid = proc_selfpid();
    proc_selfname(census.name, (int)sizeof(census.name));
    const bool censusOn = bytes <= kCensusMaxResBytes;
    sc_census_counts censusCounts {};
    while (pos < bytes) {
        uint64_t take = bytes - pos;
        if (take > kScWindowBytes) take = kScWindowBytes;
        if (md->readBytes(backingOffset + pos, gScWin, take) != take) {
            gScSt.readFails++;
            PEERLOG("shadercache: readBytes of the backing at +%#llx (%llu byte(s)) came up short - "
                    "stopping the scan of this resource after %u window(s)",
                    (unsigned long long)pos, (unsigned long long)take, windows);
            break;
        }
        ctx.winBase = pos;
        // The final window owns everything it holds; every other window owns `stride` and
        // carries `overlap` bytes of lookahead past it. Owned ranges are contiguous and
        // grid aligned, which is what the library checks before it counts a candidate.
        const bool finalWin = (pos + take >= bytes);
        const uint64_t owned = finalWin ? take : (uint64_t)stride;
        sc_scan_stats st {};
        const int r = sc_scan_window(&gSc, gScWin, (size_t)take, (size_t)pos, (size_t)owned,
                                     gSc.grid, &next, &shadercache_hit, &ctx, &st);
        windows++;
        if (censusOn)
            sc_census_window(&gSc, gScWin, (size_t)take, (size_t)owned, gSc.grid, kCensusMaxDwords,
                             pos, &census_note, &census, &censusCounts);
        cand += st.candidates; amb += st.ambiguous; nokey += st.miss_nokey;
        cmp += st.miss_compare; shrt += st.miss_short;
        empty += st.miss_empty; noend += st.miss_noend; mat += st.matches;
        errs += st.errors;
        // Check each window against the LIBRARY'S OWN total rather than a sum written out
        // here, so this code cannot drift away from the buckets shadercache.c counts (that
        // drift is exactly what 0.0.243 fixed after the library gained ambiguous/errors).
        if ((uint64_t)st.candidates != sc_scan_total(&st)) {
            winBad++;
            PEERLOG("shadercache: the window at +%#llx counted %u candidate(s) but its terminal "
                    "buckets total %llu - sc_scan_total disagrees with the candidate count, so "
                    "a start landed in no bucket or in two",
                    (unsigned long long)pos, st.candidates,
                    (unsigned long long)sc_scan_total(&st));
        }
        if (gSc.grid >= 4u) {
            // Count only the starts this window OWNS, so no byte is counted twice and the
            // empty-run length carries across the boundary.
            for (uint64_t o = 0; o + 4u <= take && o < owned; o += gSc.grid) {
                slots++;
                uint32_t d0 = 0;
                for (unsigned b = 0; b < 4; b++) d0 |= (uint32_t)gScWin[o + b] << (8u * b);
                if (d0 == 0) {
                    if (run == 0) runAt = pos + o;
                    run++;
                    if (run > longestRun) { longestRun = run; longestAt = runAt; }
                    emptySlots++;
                } else run = 0;
            }
        }
        if (r != SC_OK) {
            PEERLOG("shadercache: sc_scan_window REFUSED the window at +%#llx (base %#llx, owned "
                    "%#llx of %#llx held, lookahead %#llx): %d %s - stopping this resource",
                    (unsigned long long)pos, (unsigned long long)pos, (unsigned long long)owned,
                    (unsigned long long)take, (unsigned long long)(take - owned), r,
                    sc_status_name(r));
            break;
        }
        if (finalWin) break;
        pos += stride;
    }
    gCensusResources++;
    if (!censusOn) gCensusSkippedRes++;
    PEERLOG("shadercache: census of this resource (%#llx B, pid %d %s): %s - %u grid start(s), %u "
            "empty, %u unterminated within %#x dword(s), %u program(s), %u NEW fingerprint(s); "
            "boot totals %u unique (%llu past the %u-line log cap not sampled, 1 in %u sampled), %llu "
            "program(s), %llu over the %u-key cap, %llu of %llu "
            "resource(s) skipped as larger than %#llx B",
            (unsigned long long)bytes, census.pid, census.name,
            censusOn ? "scanned" : "SKIPPED (too large)", censusCounts.starts, censusCounts.empty,
            censusCounts.unterminated, kCensusMaxDwords, census.programs, census.fresh, gCensusN,
            (unsigned long long)gCensusUnlogged, kCensusMaxLines, kCensusSampleEvery,
            (unsigned long long)gCensusPrograms, (unsigned long long)gCensusOverflow, kCensusMaxKeys,
            (unsigned long long)gCensusSkippedRes, (unsigned long long)gCensusResources,
            (unsigned long long)kCensusMaxResBytes);
    gScSt.resources++; gScSt.windows += windows;
    gScSt.candidates += cand; gScSt.ambiguous += amb;
    // build 0.0.529 (CG84.md item 6): the establishing copy's CENSUS for switch 84 - programs found, candidate starts and
    // substitutions made. Any of them keeps the key from ever writing a delta. A copy that is not this thread's pending one: no-op.
    // 0.0.529 fix pass: `cand` counts EVERY grid start (empty slots too), so it cannot be the test; a program candidate is any start
    // sc_lookup answered other than an empty slot or no terminator (matches, ambiguous, no key, compare, short) - MF-3's predicate.
    if (gN48D84Live) {
        const uint64_t pc = mat + amb + nokey + cmp + shrt;
        navi48_d84_census(census.programs + (uint32_t)(pc > 0xffffffull ? 0xffffffull : pc) + ctx.nDone);
    }
    gScSt.missNoKey += nokey; gScSt.missCompare += cmp; gScSt.missShort += shrt;
    gScSt.missEmpty += empty; gScSt.missNoEnd += noend;
    gScSt.gridSlots += slots; gScSt.gridEmpty += emptySlots;
    if (longestRun > gScSt.gridLongestRun) { gScSt.gridLongestRun = longestRun; gScSt.gridRunAt = longestAt; }
    // The reconciliation r78's line CLAIMED and did not perform. Every candidate start
    // must land in exactly one terminal bucket: a match, or one of the five miss reasons.
    // Printing `candidates - terms` on both sides (as the first draft of this line did)
    // is a tautology that can never fail, which is the same defect again; print the real
    // sum, the real residual, and let the label be contradicted by the numbers beside it.
    // 0.0.243: the library's terminal buckets are matches + ambiguous + errors + the five
    // miss reasons (shadercache.c's own sc_scan_total). r80 had ambiguous 0 and errors 0 so
    // omitting them still reconciled, but on any run where one is non-zero this line would
    // have reported DOES NOT ADD UP against perfectly correct library behaviour - sending the
    // next session after a phantom. Sum exactly what the library counts.
    const uint64_t terms = mat + amb + errs + empty + noend + nokey + cmp + shrt;
    const uint64_t resid = (cand > terms) ? (cand - terms) : 0;
    PEERLOG("shadercache: scanned %#llx byte(s) in %u window(s) (overlap %#x): %llu candidate(s) on "
            "the %#x grid -> %u substituted here; matches %llu; misses: EMPTY (first dword zero) "
            "%llu, no terminator %llu, no key %llu, bytes differ %llu, too few bytes %llu; "
            "ambiguous %llu, errors %llu. Reconciliation %llu + %llu + %llu + %llu + %llu + %llu + "
            "%llu + %llu = %llu vs %llu candidate(s), residual %llu -> %s",
            (unsigned long long)bytes, windows, overlap, (unsigned long long)cand, gSc.grid,
            ctx.nDone, (unsigned long long)mat, (unsigned long long)empty,
            (unsigned long long)noend, (unsigned long long)nokey, (unsigned long long)cmp,
            (unsigned long long)shrt, (unsigned long long)amb, (unsigned long long)errs,
            (unsigned long long)mat, (unsigned long long)amb, (unsigned long long)errs,
            (unsigned long long)empty, (unsigned long long)noend,
            (unsigned long long)nokey, (unsigned long long)cmp, (unsigned long long)shrt,
            (unsigned long long)terms, (unsigned long long)cand, (unsigned long long)resid,
            (terms == cand) ? "ADDS UP"
                            : "DOES NOT ADD UP - a terminal bucket is missing from this line");
    // 0.0.244: the sum above is sc_scan_total's own formula, and every window was checked
    // against it individually as it was scanned. These two controls must BOTH read 0 now
    // that the library owns the overlap; they are printed whatever they say.
    PEERLOG("shadercache: window accounting - %u window(s) of %#x byte(s), stride %#x, lookahead "
            "%#x (the library requires 4 x max(key cap %u, compare cap %u) dword(s)); every grid "
            "start is owned by exactly ONE window, and the resource-wide cursor ended at %#llx of "
            "%#llx byte(s). Controls: per-window total disagreements %llu, duplicate callback hits "
            "suppressed %llu -> %s",
            windows, kScWindowBytes, stride, overlap, gSc.max_key_dwords, gSc.max_apple_dwords,
            (unsigned long long)next, (unsigned long long)bytes,
            (unsigned long long)winBad, (unsigned long long)gScSt.dupSkipped,
            (winBad == 0 && gScSt.dupSkipped == 0)
                ? "both zero, as window ownership requires"
                : "NONZERO - window ownership is not holding and a count may be doubled");
    // The occupancy answer for a relocated vertex program: 580/596/804 bytes need 3 or 4
    // consecutive 0x100 slots, so the LONGEST RUN is the number that decides it.
    PEERLOG("shadercache: 0x%x-grid occupancy of this resource: %llu slot(s) counted (each byte "
            "counted once; the window overlap is skipped), %llu EMPTY (first dword zero), longest "
            "run of consecutive empty slots %llu at resource +%#llx = %llu byte(s) - a relocated "
            "vertex program of 804 bytes needs 4 consecutive slots",
            gSc.grid, (unsigned long long)slots, (unsigned long long)emptySlots,
            (unsigned long long)longestRun, (unsigned long long)longestAt,
            (unsigned long long)(longestRun * gSc.grid));
    if (ctx.nDone >= kScMaxPerResource)
        PEERLOG("shadercache: the per-resource substitution table (%u) is FULL - further slots in "
                "this resource were re-scanned rather than de-duplicated", kScMaxPerResource);
    // 0.0.438 (ScCtx FLAGS FIX): hand THIS call's own flags back out - ctx is a local, so nothing here
    // can be seen or clobbered by any other concurrent call to this function.
    if (outPartialWrite) *outPartialWrite = ctx.partialWrite;
    if (outMismatch) *outMismatch = ctx.mismatch;
}

// The number of substitutions the shader cache has WRITTEN. The source hook's program memo (gfx_src_decide.h)
// uses it as an EPOCH. 0.0.304 used gScSt.resources, the count of resources SCANNED, and run m4c2 measured what
// that costs: 54 scans in one run dropped the memo 27 times and left it with 4 hits against 284 misses.
// Scanning a resource cannot change a shader's bytes; only a WRITE can, and the only writes we make are these.
// RESIDUAL RISK, stated: Apple re-uploading a shader at the same address with different bytes would not move this
// counter. It has never been observed - gfxcap1 recorded 24 distinct (program VA, content) pairs over 24 distinct
// VAs and 561 reads, i.e. every address held the same bytes every time - and the memo's class rule means a stale
// row can only make us neuter a frame we could have translated, never translate one we should not.
// Read-only; it changes nothing here.
uint64_t navi48_shadercache_epoch(void) { return gScSt.substituted; }

// - THE TWELFTH LYING STRING, fixed. gSkipPageCopy is parsed from the boot-arg only when the accelerator is stop-patched,
// which on a normal boot happens AFTER shadercache is armed (hp5: the note at line 1422, the parse at 1488, then 1786 residency
// copies). So "gSkipPageCopy is false" meant "not parsed yet", and the old note said the boot-arg was NOT set on boots where it
// was. Read the boot-arg itself and say which of the two is true.
static void shadercache_skip_pagecopy_note() {
    uint32_t skip = 0;
    const bool set = PE_parse_boot_argn("navi48-skip-pagecopy", &skip, sizeof(skip)) && skip;
    if (!set)
        PEERLOG("shadercache: note - the boot-arg navi48-skip-pagecopy=1 is NOT set (read now), so no residency-copy hook "
                "will be installed and nothing will be substituted");
    else
        PEERLOG("shadercache: note - the boot-arg navi48-skip-pagecopy=1 IS set; its hooks are not installed YET (newResource "
                "when the accelerator is stop-patched, the residency copy when Apple creates its first resource). Arming "
                "before that is the right order: the scan runs from inside each copy");
}

uint32_t navi48_shadercache_control(uint32_t mode, uint64_t *out, unsigned count) {
    if (mode == 1) {
        // 0.0.240. r77 refused here and substituted nothing, and the guard was
        // the reason. It was copied from kernsub, where requiring the resource vtable hook is
        // right because kernsub WRITES immediately at the copy's recorded destination. This
        // path never writes from here: it only sets a flag that residency_copy_to_vram reads,
        // and that function cannot run unless the hook exists. So the hook is a precondition
        // of the WORK, not of the ARM - and demanding it at arm time is unsatisfiable, because
        // hook_new_resource installs it when APPLE creates its first resource, which happens
        // inside the Metal client's run: after the last point a step list can reach, and after
        // start(). Arm regardless, and say plainly what is not yet in place.
        if (shadercache_open_once() && !gScArmed) {
            gScArmed = true;
            PEERLOG("shadercache: ARMED for this boot - every residency copy of a handled resource is "
                    "now scanned on the %#x grid and every shader the cache holds is substituted. "
                    "kernsub is untouched and still works for diagnosis.", gSc.grid);
            // ORDERING, and it is the OPPOSITE of kernsub's. kernsub can be armed after
            // the client has already paged, because it re-substitutes directly at the
            // copy's recorded destination (gCopy.lastDst + 0xfb00). This path deliberately
            // retains none of Apple's pointers, so it can only act while a copy is running:
            // arming it after the copies have happened substitutes NOTHING and would look
            // exactly like a cache that found no match. Say so loudly instead.
            if (!gSkipPageCopy)
                shadercache_skip_pagecopy_note();
            else if (!gResCopyVt)
                PEERLOG("shadercache: note - the residency-copy vtable hook is not installed yet "
                        "because Apple has not created a resource. That is EXPECTED and CORRECT "
                        "when arming early, which is the order this path needs: the scan runs from "
                        "inside the copy, so arming must happen before the client pages.");
            if (gCopy.copies)
                PEERLOG("shadercache: *** ARMED LATE - %llu residency copy/copies ALREADY RAN this "
                        "boot and cannot be re-scanned (no Apple pointer is retained). Nothing of "
                        "what is already resident will be substituted. Arm BEFORE the client pages "
                        "its shaders - right after copyarm - or use navi48-shader-cache=1. ***",
                        (unsigned long long)gCopy.copies);
        }
    } else if (mode == 2 && gScArmed) {
        gScArmed = false;
        PEERLOG("shadercache: DISARMED - copies are no longer scanned (already-substituted bytes stay)");
    } else if (mode == 3) {
        // 0.0.265 — arm, AND permit entries that carry register adjustments.
        //
        // READ THE WARRANT BEFORE USING THIS. `accept_adjust` does NOT mean the adjustments are
        // applied here: nothing on this path writes a register. It means "substitute the code even
        // though the entry records that its registers differ from Apple's". That is sound only where
        // something ELSE programs those registers, and on the RENDER path something does —
        // xlat12_ib_profile_for supplies RSRC1/RSRC2_GS, RSRC1/RSRC2_PS and SPI_PS_INPUT_ENA/ADDR per
        // shader identity, which is the same information the entry's adjustments carry,
        // by a different route. On any path where our draw policy does NOT translate the stream this
        // would leave APPLE's register values in front of OUR code, which is why mode 1 refuses these
        // entries and why this is a separate, separately-named mode rather than a relaxed default.
        gScAcceptAdjust = 1;
        if (shadercache_open_once() && !gScArmed) {
            gScArmed = true;
            PEERLOG("shadercache: ARMED for this boot (mode 3) - as mode 1, and entries carrying "
                    "register adjustments are NO LONGER REFUSED. Nothing here applies a register: the "
                    "render path's per-identity profile does that, and this mode is only valid for "
                    "shaders our draw policy translates. %#x grid.", gSc.grid);
            if (!gSkipPageCopy)
                shadercache_skip_pagecopy_note();
            else if (!gResCopyVt)
                PEERLOG("shadercache: note - the residency-copy vtable hook is not installed yet "
                        "because Apple has not created a resource. That is EXPECTED when arming early.");
            if (gCopy.copies)
                PEERLOG("shadercache: *** ARMED LATE - %llu residency copy/copies ALREADY RAN this "
                        "boot and cannot be re-scanned. Arm BEFORE the client pages its shaders. ***",
                        (unsigned long long)gCopy.copies);
        } else if (gScArmed) {
            PEERLOG("shadercache: already armed; adjustments are now ACCEPTED for subsequent copies");
        }
    }
    const uint64_t state = (gScArmed ? 1u : 0u) | (gScOpened ? 2u : 0u) | (gResCopyVt ? 4u : 0u) |
                           (gScAcceptAdjust ? 8u : 0u);
    const uint64_t v[13] = {
        state, gScOpened ? sc_entry_count(&gSc) : 0u, gScSt.resources, gScSt.windows,
        gScSt.candidates, gScSt.matches, gScSt.substituted, gScSt.mismatched,
        gScSt.refused | (gScSt.ambiguous << 32), gScSt.missNoKey,
        gScSt.missCompare | (gScSt.missShort << 32), gScSt.lastAt,
        gScSt.lastOff | (gScSt.lastBytes << 32),
    };
    if (out)
        for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return (uint32_t)state;
}

// =============================================================================================================================
// build 0.0.495 (: RUN D's hang) — SWITCH 62's COPIER SIDE: SUBSTITUTE IN THE COPY. gfx_heapgen.h has the
// whole argument. Here: ic_begin (called by residency_copy_to_vram BEFORE its first VRAM write, only while n48::hw_hg_on())
// scans the SAME backing bytes the post-copy substitution (shadercache_scan_resource) will scan, with the SAME library calls and
// the SAME refusal ladder as shadercache_hit (verified match, graphics opt-in, sc_subst_render into a kScMaxSubstBytes window,
// the destination segment's span, the VRAM guard), and records what it would write as PATCHES; ic_overlay (called from the
// write loop on each batch after it is read from the backing and before it is written) puts those bytes into the batch;
// navi48_ic_scope_closed (Navi48CopyScope's destructor, i.e. after the post-copy substitution and kernsub) completes the copy.
// Nothing here writes VRAM: the only VRAM write stays the copy's own batch write, over the copy's own pre-flighted range.
// State is per copy (heap, IOMalloc'd once per copy while 62 is ON) and found by the copying thread, so two concurrent copies on
// two pageon threads never share a buffer, and residency_copy_to_vram (inlined into hook_page_texture) gains no local.
// =============================================================================================================================
struct IcPlan {
    uint8_t       win[kScWindowBytes];                 // the pre-scan's own window (gScWin belongs to the post-copy scan)
    uint8_t       arena[N48_HG_ARENA_BYTES];           // the rendered bytes of every patch
    uint8_t       apple[N48_HG_ARENA_BYTES];           // 0.0.496 F4: the Apple bytes each patch's pre-scan matched (the re-check's)
    n48_hg_patch  patch[N48_HG_PATCH_MAX];
    n48_hg_poison poison[N48_HG_PATCH_MAX];
    uint64_t      done[N48_HG_PATCH_MAX * 2u];         // resource offsets already decided (the library's ownership control)
    uint32_t      npatch, npoison, ndone, arenaUsed, patchBytes, overlaid;
    uint32_t      appleUsed, recheckBad;               // 0.0.496 F4: apple arena use; patches the re-check found changed
};
struct IcSlot {
    uintptr_t thread;                                  // the copying thread; 0 = free (claimed by compare-and-swap)
    IcPlan   *plan;                                    // null: no plan could be allocated (then the whole copy is poisoned)
    uint32_t  bumped, overlaying, scanFailed, noPlan, vaOk;
    uint64_t  cgLo, cgHi, va, bytes;
    uint64_t  regSeq;                                  // 0.0.496 F1: the registry sequence when this copy began (its prune bound)
    n48_hg_copy_id hgId;                               // build 0.0.533 (switch 88): this copy's identity; its record's token (tok)
};
static constexpr uint32_t kIcSlots = 16u;
static IcSlot gIcSlot[kIcSlots];
static volatile uint32_t gIcOverlaying { 0u };         // copies with patches in flight: the write loop's one-load test
static uint64_t gIcNoSlot { 0u }, gIcNoPlan { 0u };

struct IcScan { IcPlan *p; IcSlot *s; void *dstMem; PhysSegmentFn dSeg; uint64_t winBase; uint32_t noOverlay; };

static IcSlot *ic_slot_mine() {
    const uintptr_t me = reinterpret_cast<uintptr_t>(current_thread());
    for (uint32_t i = 0; i < kIcSlots; i++)
        if (__atomic_load_n(&gIcSlot[i].thread, __ATOMIC_ACQUIRE) == me) return &gIcSlot[i];
    return nullptr;
}

static void ic_poison(IcScan *c, uint64_t resOff, uint64_t len) {
    IcPlan *p = c->p;
    if (p->npoison >= N48_HG_PATCH_MAX) { c->s->scanFailed = 1u; return; }   // no row left: the whole copy is poisoned instead
    n48_hg_poison &e = p->poison[p->npoison++];
    e.va_ok = c->s->vaOk; e.va_lo = c->s->va + resOff; e.va_hi = c->s->va + resOff + len; e.used = 1u;
    uint64_t span = 0; const uint64_t at = c->dSeg(c->dstMem, resOff, &span);
    e.vram_lo = at; e.vram_hi = at ? at + len : 0ull;
}

// One verified candidate of the pre-scan: shadercache_hit's ladder, with "record a patch" where it writes VRAM.
static int ic_hit(void *vctx, size_t off, const sc_match *m) {
    IcScan *c = static_cast<IcScan *>(vctx);
    IcPlan *p = c->p;
    const uint64_t resOff = c->winBase + (uint64_t)off;
    if (!m->verified) return 0;                                                  // known-only: the post-copy scan writes nothing
    for (uint32_t i = 0; i < p->ndone; i++) if (p->done[i] == resOff) return 0;
    if (p->ndone < N48_HG_PATCH_MAX * 2u) p->done[p->ndone++] = resOff;
    if ((m->stage == SC_STAGE_VERTEX || m->stage == SC_STAGE_FRAGMENT) && !gScAcceptAdjust) return 0;   // opt-in, as today
    if (p->npatch >= N48_HG_PATCH_MAX || N48_HG_ARENA_BYTES - p->arenaUsed < kScMaxSubstBytes) {
        ic_poison(c, resOff, m->capacity_bytes);                                 // cannot be patched in the copy: POISONED
        return 0;
    }
    uint32_t nb = 0;
    // build 0.0.536 (switch 92, gfx_rv92.h): the same choice as shadercache_hit, so the patch is the image the post-copy
    // substitution would write. The Apple bytes kept below are still `m`'s (the ones this match verified).
    sc_match alt92; uint32_t why92 = N48_RV92_OLD;
    const sc_match *use92 = n48_rv92_pick(&gSc, m, n48::hw_rv92_on() ? 1u : 0u, &alt92, &why92);
    const int rst = sc_subst_render(&gSc, use92, (int)gScAcceptAdjust, p->arena + p->arenaUsed, kScMaxSubstBytes, &nb);
    if (rst != SC_OK || nb == 0 || (nb & 3u)) return 0;                          // refused today too: never ours
    uint64_t span = 0;
    const uint64_t at = c->dSeg(c->dstMem, resOff, &span);
    if (!at || (at & 3) || span < nb) return 0;                                  // shadercache_hit's segment rule
    if (navi48_vram_apple_dest_check(at, nb)) return 0;                          // shadercache_hit's VRAM guard
    if (c->noOverlay || resOff + nb > c->s->bytes) { ic_poison(c, resOff, nb); return 0; }   // re-tiled / beyond the copy
    // 0.0.496 F4: keep the Apple bytes this match was made on (the window's own), so the write loop can re-check them before it
    // overlays; a program whose matched bytes cannot be kept (none, past the window, the apple arena full) is POISONED instead.
    const uint32_t anb = m->apple_dwords * 4u;
    if (!anb || off + anb > kScWindowBytes || N48_HG_ARENA_BYTES - p->appleUsed < anb) { ic_poison(c, resOff, nb > anb ? nb : anb); return 0; }
    memcpy(p->apple + p->appleUsed, p->win + off, anb);
    n48_hg_patch &e = p->patch[p->npatch++];
    e.off = (uint32_t)resOff; e.nb = nb; e.arena_off = p->arenaUsed; e.pad = 0u; e.vram = at;
    e.apple_nb = anb; e.apple_off = p->appleUsed; e.bad = 0u; e.pad2 = 0u;
    p->arenaUsed += nb; p->patchBytes += nb; p->appleUsed += anb;
    return 0;
}

// The pre-scan: shadercache_scan_resource's window loop (the library's ownership, lookahead and cursor), over this plan's window.
static bool ic_scan(IcScan *c, IOMemoryDescriptor *md, uint64_t backingOffset, uint64_t bytes) {
    uint32_t overlap = (gSc.max_apple_dwords > gSc.max_key_dwords ? gSc.max_apple_dwords : gSc.max_key_dwords) * 4u;
    if (gSc.grid) overlap = (overlap + gSc.grid - 1u) & ~(gSc.grid - 1u);
    if (overlap > kScWindowBytes / 2u) overlap = kScWindowBytes / 2u;
    const uint32_t stride = kScWindowBytes - overlap;
    uint64_t pos = 0;
    size_t next = 0;
    while (pos < bytes) {
        uint64_t take = bytes - pos;
        if (take > kScWindowBytes) take = kScWindowBytes;
        if (md->readBytes(backingOffset + pos, c->p->win, take) != take) return false;
        c->winBase = pos;
        const bool finalWin = (pos + take >= bytes);
        const uint64_t owned = finalWin ? take : (uint64_t)stride;
        sc_scan_stats st {};
        if (sc_scan_window(&gSc, c->p->win, (size_t)take, (size_t)pos, (size_t)owned, gSc.grid, &next, &ic_hit, c, &st) != SC_OK)
            return false;
        if (finalWin) break;
        pos += stride;
    }
    return true;
}

// Called by residency_copy_to_vram after its pre-flight and copy-guard scope, BEFORE its first VRAM write, only while switch 62
// is ON. Claims this thread's slot, pre-scans, and bumps the heap generation when the copy overlaps a substituted program.
// build 0.0.533 (switch 88): `res` is the AMDAccelResource being copied (residency_copy_to_vram's `self`): the identity switch
// 88's in_place answer is keyed on (the same resource's previous copy over the identical VRAM range).
static __attribute__((noinline)) void ic_begin(void *dstMem, PhysSegmentFn dSeg, IOMemoryDescriptor *md, uint64_t backingOffset,
                                               uint64_t bytes, uint32_t noOverlay, uint64_t cgLo, uint64_t cgHi, void *dstMap,
                                               void *res) {
    const uintptr_t me = reinterpret_cast<uintptr_t>(current_thread());
    IcSlot *s = nullptr;
    for (uint32_t i = 0; i < kIcSlots && !s; i++) {
        uintptr_t want = 0;
        if (__atomic_compare_exchange_n(&gIcSlot[i].thread, &want, me, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) s = &gIcSlot[i];
    }
    if (!s) {
        // No slot: this copy cannot be completed later, so it cannot be patched or tracked. Bump with nothing to close it:
        // `active` never returns to 0 and EVERY commit is refused for the rest of the boot (fail closed, loudly).
        gIcNoSlot++;
        (void)n48::hw_hg_copy_begin(cgLo, cgHi, nullptr, 0u, 1u, nullptr, 0u, nullptr);   // untracked: never waits on PRE (mayWait 0); under 72 M3 it waits on LIVE like any bumping copy (0.0.532); 0.0.533: no identity = an UNKNOWN-range record, never closed
        PEERLOG("shaderheap62: NO COPY SLOT (%u in use) - the copy over VRAM [%#llx,%#llx) is untracked: the heap generation stays "
                "in progress and every later commit is refused this boot (untracked %llu)", kIcSlots, (unsigned long long)cgLo,
                (unsigned long long)cgHi, (unsigned long long)gIcNoSlot);
        return;
    }
    s->plan = nullptr; s->bumped = 0u; s->overlaying = 0u; s->scanFailed = 0u; s->noPlan = 0u;
    s->cgLo = cgLo; s->cgHi = cgHi; s->bytes = bytes;
    s->va = map_gpu_va(dstMap, &s->vaOk);
    // build 0.0.533 (switch 88): the identity, kept in the slot (static: no new local in this frame).
    s->hgId.res = reinterpret_cast<uintptr_t>(res); s->hgId.va = s->va; s->hgId.bytes = bytes; s->hgId.tok = 0ull;
    s->hgId.pid = proc_selfpid(); s->hgId.va_ok = s->vaOk; s->hgId.range_ok = 1u; s->hgId.pad = 0u;
    if (gScArmed && gScOpened) {
        IcPlan *p = static_cast<IcPlan *>(IOMalloc(sizeof(IcPlan)));
        if (!p) { s->noPlan = 1u; gIcNoPlan++; }
        else {
            p->npatch = p->npoison = p->ndone = p->arenaUsed = p->patchBytes = p->overlaid = 0u;
            p->appleUsed = p->recheckBad = 0u;
            s->plan = p;
            IcScan c { p, s, dstMem, dSeg, 0ull, noOverlay };
            if (!ic_scan(&c, md, backingOffset, bytes)) s->scanFailed = 1u;
        }
    }
    // A pre-scan that could not finish (or no plan at all) does NOT by itself make the copy overlapping: the post-copy scan runs
    // the same library over the same bytes, so a program it never reached was never ours. When the copy DOES overlap (a patch, an
    // unpatchable program, or the registry), scanFailed/noPlan make its completion unclean, so its whole range is poisoned.
    const uint32_t np = s->plan ? s->plan->npatch : 0u;
    const uint32_t npo = s->plan ? s->plan->npoison : 0u;
    // build 0.0.511 (MEDIUM-1): switch 72 may hold back only a FULLY PATCHED copy - every program it overlaps is
    // patched in the copy (np, no poison), the overlay is possible (!noOverlay) and the pre-scan finished with a plan (!scanFailed,
    // !noPlan). What is NOT knowable here: an F4-BAD patch (n48_hg_overlay_checked's re-check in ic_overlay) is found only while
    // the copy writes, AFTER this call; such a copy completes unclean and poisons its range, but it may have waited.
    const uint32_t mayWait = n48_hg_copy_may_wait(np, npo, noOverlay, s->scanFailed, s->noPlan);
    s->bumped = n48::hw_hg_copy_begin(cgLo, cgHi, s->plan ? s->plan->patch : nullptr, np, npo, &s->regSeq, mayWait, &s->hgId);
    if (s->bumped && np) { s->overlaying = 1u; __atomic_fetch_add(&gIcOverlaying, 1u, __ATOMIC_SEQ_CST); }
    if (!s->bumped) {                                    // overlaps nothing we substitute: this copy is exactly today's
        if (s->plan) IOFree(s->plan, sizeof(IcPlan));
        s->plan = nullptr;
        __atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);
    }
}

// The write loop's hook: the batch holding resource bytes [resOff, resOff + take) was just read from the backing. Put every
// patch byte that falls inside it in place, before the batch is written.
// 0.0.496 F4: n48_hg_overlay_checked re-checks each patch's matched Apple bytes in this batch BEFORE overlaying; a patch whose
// backing changed since the pre-scan is not overlaid (now or later), its program range is POISONED here, and the copy completes
// UNCLEAN (recheckBad: navi48_ic_scope_closed).
static __attribute__((noinline)) void ic_overlay(uint64_t resOff, uint8_t *raw, uint32_t take) {
    IcSlot *s = ic_slot_mine();
    if (!s || !s->overlaying || !s->plan) return;
    IcPlan *p = s->plan;
    uint32_t nbad = 0u;
    p->overlaid += n48_hg_overlay_checked(p->patch, p->npatch, p->arena, p->apple, resOff, raw, take, &nbad);
    if (!nbad) return;
    p->recheckBad += nbad;
    for (uint32_t i = 0; i < p->npatch; i++) {
        n48_hg_patch &e = p->patch[i];
        if (!e.bad || e.pad2) continue;
        e.pad2 = 1u;                                  // poisoned once
        if (p->npoison >= N48_HG_PATCH_MAX) { s->scanFailed = 1u; continue; }   // no row: the whole copy is poisoned (unclean)
        const uint64_t len = e.nb > e.apple_nb ? e.nb : e.apple_nb;
        n48_hg_poison &q = p->poison[p->npoison++];
        q.va_ok = s->vaOk; q.va_lo = s->va + e.off; q.va_hi = s->va + e.off + len; q.used = 1u;
        q.vram_lo = e.vram; q.vram_hi = e.vram + len;
    }
    PEERLOG("shaderheap62: OVERLAY RE-CHECK FAILED - %u program(s) in the batch at resource +%#llx no longer hold the Apple bytes "
            "the pre-scan matched: NOT overlaid, POISONED, and the copy completes unclean", nbad, (unsigned long long)resOff);
}

// The write loop's read: exactly `md->readBytes(backingOffset + resOff, raw, take)`, then, only while some copy carries patches
// (gIcOverlaying) and only if it is THIS thread's, the overlay. One call in place of the read, so hook_page_texture (which
// inlines residency_copy_to_vram) keeps its frame: a separate overlay call beside the read grew it by 0x10.
// build 0.0.529 (CG84.md item 6): THIS thread's switch-84 ON establishing copy reads the plan-time image (the scratch) instead of
// the backing, so the key's shadow is exactly what was written. One load while switch 84 is not acting (gN48D84SrcThr 0).
static __attribute__((noinline)) uint64_t ic_read(IOMemoryDescriptor *md, uint64_t backingOffset, uint64_t resOff, uint8_t *raw,
                                                  uint64_t take) {
    const uintptr_t d84 = __atomic_load_n(&gN48D84SrcThr, __ATOMIC_RELAXED);
    const uint64_t got = (d84 && d84 == (uintptr_t)current_thread()) ? navi48_d84_src(resOff, raw, take)
                                                                     : md->readBytes(backingOffset + resOff, raw, take);
    if (got == take && gIcOverlaying) ic_overlay(resOff, raw, (uint32_t)take);
    return got;
}

// Navi48CopyScope's destructor: the copy and its post-copy substitution are complete. Only the thread that began a tracked copy
// has a slot, so every other scope (the standalone kernsub scope, the phantom control) returns at once.
__attribute__((noinline)) void navi48_ic_scope_closed(uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch, bool sampled) {
    IcSlot *s = ic_slot_mine();
    if (!s || s->cgLo != lo || s->cgHi != hi) return;
    IcPlan *p = s->plan;
    const uint32_t np = p ? p->npatch : 0u;
    // CLEAN: every byte written and read back equal, the pre-scan complete, and every patch byte overlaid (the in-copy
    // substitution's own verification: a patch that never reached a batch is not in VRAM).
    // build 0.0.509 F-3: and every byte READ BACK - a chunk the SDMA path verified by sampling (`sampled`) was not, so the
    // copy is unclean (it neither clears 62's poison nor prunes). With switch 63 OFF `sampled` is always false.
    const bool clean = n48_fc_hg_clean(wrote && !failed && !mismatch && !s->scanFailed && !s->noPlan && (!p || p->overlaid == p->patchBytes) &&
                       (!p || !p->recheckBad), sampled ? 1u : 0u) != 0u;   // 0.0.496 F4: a patch whose backing changed makes the copy unclean

    if (s->bumped)
        n48::hw_hg_copy_end(clean ? 1u : 0u, s->vaOk, s->va, s->va + s->bytes, s->cgLo, s->cgHi, p ? p->poison : nullptr,
                            p ? p->npoison : 0u, clean ? np : 0u, clean && p ? p->patchBytes : 0u, np, s->regSeq, s->hgId.tok);
    if (s->overlaying) __atomic_fetch_sub(&gIcOverlaying, 1u, __ATOMIC_SEQ_CST);
    if (p) IOFree(p, sizeof(IcPlan));
    s->plan = nullptr; s->overlaying = 0u; s->bumped = 0u;
    __atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);
}

// build 0.0.509 F-3: the fast copy's slot is asked FIRST (and released): a copy with a chunk the SDMA path verified by
// sampling may neither clear the copy guard's poison (n48_fc_cg_close_wrote: it still poisons on a failure or a mismatch) nor
// complete clean for switch 62. With switch 63 never turned ON this boot navi48_fc_scope_closed answers 0 at once and both calls
// are 0.0.508's.
static __attribute__((noinline)) void sk82_scope_closed(uint64_t lo, uint64_t hi);   // build 0.0.527, defined below
__attribute__((noinline)) void navi48_cg_close_scope(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {
    const uint32_t sampled = navi48_fc_scope_closed(lo, hi);
    navi48_cg_close(slot, lo, hi, n48_fc_cg_close_wrote(wrote ? 1u : 0u, failed ? 1u : 0u, mismatch ? 1u : 0u, sampled) != 0u, failed, mismatch);
    navi48_ic_scope_closed(lo, hi, wrote, failed, mismatch, sampled != 0u);
    // build 0.0.527 (SKIP82.md item 6): switch 82's establishment, AFTER the scope closed (its END, poison and slot release
    // are done). A scope that is not this thread's pending copy's returns at once. One load while 82 is OFF.
    if (gN48Sk82On) sk82_scope_closed(lo, hi);
    // build 0.0.529 (CG84.md item 6): switch 84's update, AFTER the scope closed (END pushed, poison decided, slot released): a
    // scope that is not this thread's pending copy's returns at once. One load while switch 84 never left OFF.
    if (gN48D84Live) navi48_d84_closed(lo, hi, wrote, failed, mismatch);
}

// build 0.0.509 F-3: this thread's copy bumped 62's heap generation (ic_begin ran before the write loop, so this is known at
// the copy's first chunk). Asked by the fast copy's per-copy latch only while switch 63 is ON.
__attribute__((noinline)) uint32_t navi48_ic_bumped_mine(void) {
    IcSlot *s = ic_slot_mine();
    return (s && s->bumped) ? 1u : 0u;
}

// build 0.0.509 F-2: a fast-copy chunk of this thread's copy - resource bytes [pos, pos + n), VRAM [dAt, dAt + n) - rang the
// doorbell and did not see its fence land. When the copy bumped 62, its range is poisoned in 62's table for the rest of the boot
// (by the copy's GPU VA when known, else every frame is refused - gfx_heapgen.h's fail-closed rule for a VA-less entry).
__attribute__((noinline)) void navi48_ic_chunk_dead(uint64_t pos, uint64_t dAt, uint64_t n) {
    IcSlot *s = ic_slot_mine();
    if (!s || !s->bumped || !n) return;
    n48_hg_poison e {};
    e.va_ok = s->vaOk; e.va_lo = s->va + pos; e.va_hi = s->va + pos + n; e.vram_lo = dAt; e.vram_hi = dAt + n; e.used = 1u;
    n48::hw_hg_poison_sticky(&e);
}

// =============================================================================================================================
// build 0.0.496 (an internal design note,) — SWITCH 63's COPIER SIDE. fastcopy.h has the argument; the SDMA
// machinery (staging, positive control, submission, fence, verify, the per-copy latch) is Navi48Bringup.cpp's navi48_fc_chunk.
// Here: the batch producer it calls - THE SAME producer the MM loop below calls, with the same (offset, take) pairs: a re-tiled
// copy's bytes through rp_retile_bytes, every other copy's through ic_read (readBytes, then 0.0.495's in-copy overlay) - so the
// byte stream written to staging is exactly the stream the MM loop would write, overlay included, and it is complete before
// anything is submitted. fc_copy_chunk is noinline so hook_page_texture (which inlines residency_copy_to_vram) gains no local.
// =============================================================================================================================
// =============================================================================================================================
// build 0.0.527 (an internal design note, ; gfx_sk82.h) — SWITCH 82's COPIER SIDE. Three noinline helpers, so
// hook_page_texture (which inlines residency_copy_to_vram) keeps its frame: the decision before rp_lin_prepare (sk82_try_skip), the
// result before the scope closes (sk82_copy_result) and the establishment after it closed (sk82_scope_closed, from
// navi48_cg_close_scope). The storage, the lock and the pure decision are Navi48Bringup.cpp's and gfx_sk82.h's.
// =============================================================================================================================
// The state a skip must find unchanged (item 5): switches 11, 46, 59, 62, 63, kernsub, the shadercache arm, the arm level, the
// ring neuter, WindowServer's binding.
static __attribute__((noinline)) void sk82_snap_now(n48_sk82_snap *s) {
    s->v[N48_SK82_SN_S11] = n48::hw_resprov_on() ? 1u : 0u;
    s->v[N48_SK82_SN_S46] = n48::hw_resprov_kinds_on() ? 1u : 0u;
    s->v[N48_SK82_SN_S59] = n48::hw_resprov_lin_on() ? 1u : 0u;
    s->v[N48_SK82_SN_S62] = n48::hw_hg_on() ? 1u : 0u;
    s->v[N48_SK82_SN_S63] = navi48_fc_mode_now();
    s->v[N48_SK82_SN_KSUB] = gSubMode;
    s->v[N48_SK82_SN_SC] = gScArmed ? 1u : 0u;
    s->v[N48_SK82_SN_ARM] = n48::hw_sk82_arm_level();
    s->v[N48_SK82_SN_NEUTER] = n48::hw_sk82_neuter();
    s->v[N48_SK82_SN_WS] = n48::hw_sk82_ws_binding();
}
// ONCE per residency copy, before rp_lin_prepare and before the copy's scope opens (G0). Its own read-only checks: type 0x40,
// res+0x1dc SW_MODE 0, bytes <= 64 KiB and a multiple of 4, ONE contiguous destination segment the VRAM guard admits, a GPU VA.
// true = SKIP (the whole source equals a TRUSTED entry's image and every check passed; SKIP mode only). The SKIP / DIFFER lines
// are logged here, outside the lock: the first 32, then every 256th.
static __attribute__((noinline)) bool sk82_try_skip(void *self, void *dstMap, void *dstMem, PhysSegmentFn dSeg, IOMemoryDescriptor *md) {
    const char *r = static_cast<const char *>(self);
    n48_sk82_key k {};
    k.res = reinterpret_cast<uintptr_t>(self); k.dstMem = reinterpret_cast<uintptr_t>(dstMem);
    k.bytes = *reinterpret_cast<const uint64_t *>(r + 0x230);
    k.boff = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    k.pid = proc_selfpid();
    const uint32_t surfRec = *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    uint32_t eligible = (*reinterpret_cast<const uint8_t *>(r + 0x14) == 0x40u && (surfRec & 0x1fu) == 0u && k.bytes &&
                         k.bytes <= N48_SK82_MAX_BYTES && !(k.bytes & 3ull)) ? 1u : 0u;
    uint64_t span = 0;
    if (eligible) {
        k.vram = dSeg(dstMem, 0, &span);
        if (span < k.bytes || (k.vram & 3ull) || navi48_vram_apple_dest_check(k.vram, k.bytes)) eligible = 0u;
    }
    uint32_t vaOk = 0;
    if (eligible) { k.va = map_gpu_va(dstMap, &vaOk); if (!vaOk) eligible = 0u; }
    n48_sk82_snap sn {};
    sk82_snap_now(&sn);
    n48_sk82_out o {};
    const uint32_t skip = navi48_sk82_try(&k, &sn, md, gCopy.copies + 1u, eligible, &o);   // this copy's COPIED number (best effort)
    if (o.logIt) {
        // build 0.0.528 item 5b (log-only): Apple's dirty START (res+0x138) and LENGTH (res+0x140) as two fields, and their sum.
        const uint64_t dirtyStart = *reinterpret_cast<const uint64_t *>(r + 0x138);
        const uint64_t dirtyLen = *reinterpret_cast<const uint64_t *>(r + 0x140);
        const uint64_t dirtyEnd = dirtyStart + dirtyLen;
        if (skip || o.would)
            PEERLOG(N48_SK82_SKIP_FMT, skip ? "SKIP" : "WOULD-SKIP (MEASURE)", (unsigned long long)o.n, self,
                    (unsigned long long)k.vram, (unsigned long long)(k.vram + k.bytes), (unsigned long long)k.va,
                    (unsigned long long)k.bytes, (unsigned long long)o.fromCopy, (unsigned long long)dirtyEnd,
                    (unsigned long long)dirtyStart, (unsigned long long)dirtyLen);
        else if (o.cmp == N48_SK82_CMP_DIFFER)
            PEERLOG(N48_SK82_DIFFER_FMT, self, (unsigned long long)o.first, (unsigned long long)o.last,
                    (unsigned long long)o.ndiff, (unsigned long long)dirtyEnd, (unsigned long long)dirtyStart,
                    (unsigned long long)dirtyLen, (unsigned long long)o.n);
    }
    return skip != 0u;
}
// The copier's result, BEFORE its scope closes (item 6): every byte written and read back (compared x 4 >= wBytes), 0 mismatches,
// not re-tiled, not the backing-sourced image (wBytes == the key's bytes, checked in gfx_sk82.h), no switch-62 bump.
// build 0.0.529: and switch 84's (CG84.md item 6): its `ok` leaves the read-back count to gfx_cg84.h's update (full under ON,
// "sampled trust" under SHADOW): 0 mismatches, not re-tiled, no switch-62 bump.
static __attribute__((noinline)) void sk82_copy_result(uint64_t compared, uint64_t mismatched, uint64_t wBytes, uint32_t retiled) {
    const uint32_t bumped = navi48_ic_bumped_mine();
    if (gN48Sk82On) {
        const uint32_t ok = (compared * 4ull >= wBytes && !mismatched && !retiled && !bumped) ? 1u : 0u;
        navi48_sk82_result(ok, wBytes);
    }
    if (gN48D84Live) navi48_d84_result((!mismatched && !retiled && !bumped) ? 1u : 0u, compared, mismatched, wBytes);
}
// The establishment, AFTER the copy's scope closed (navi48_cg_close_scope, after navi48_cg_close).
static __attribute__((noinline)) void sk82_scope_closed(uint64_t lo, uint64_t hi) {
    n48_sk82_snap sn {};
    sk82_snap_now(&sn);
    navi48_sk82_closed(lo, hi, &sn);
}

struct FcFill { RtBufs *rt; IOMemoryDescriptor *md; uint64_t backingOffset, pos; uint32_t retile; };
static int fc_fill_batch(void *vc, uint8_t *dst, uint64_t off, uint32_t take) {
    FcFill *c = static_cast<FcFill *>(vc);
    if (c->retile) return rp_retile_bytes(c->rt, c->pos + off, dst, take) ? 1 : 0;
    return ic_read(c->md, c->backingOffset, c->pos + off, dst, take) == take ? 1 : 0;
}
static __attribute__((noinline)) uint64_t fc_copy_chunk(RtBufs *rt, bool retile, IOMemoryDescriptor *md, uint64_t backingOffset,
                                                        uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, uint8_t *lead,
                                                        uint64_t cgLo, uint64_t cgHi) {
    // build 0.0.529 (CG84.md item 6): a switch-84 ON establishing copy goes through the MM path (every dword read back). One
    // load while switch 84 is not acting. Declined before navi48_fc_chunk: no staging, no slot, no latch for this copy.
    const uintptr_t d84 = __atomic_load_n(&gN48D84SrcThr, __ATOMIC_RELAXED);
    if (d84 && d84 == (uintptr_t)current_thread()) return 0ull;
    FcFill c { rt, md, backingOffset, pos, retile ? 1u : 0u };
    // build 0.0.510 A1: a first chunk SDMA staged and was then REFUSED before the ring comes back 0 and the MM loop below runs
    // it, calling the SAME producer over the same batches again. The producer's only count is the in-copy overlay's (switch 62):
    // put it back, so the MM loop's own pass counts each patch byte once (else overlaid != patchBytes and the copy completes
    // UNCLEAN). A chunk declined before staging ran no producer: the value put back is the value it has.
    IcSlot *is = gIcOverlaying ? ic_slot_mine() : nullptr;
    const uint32_t ov0 = (is && is->plan) ? is->plan->overlaid : 0u;
    // 0.0.511 LOW-3: the copy's range. build 0.0.521 Part D: `retile` (= switch 59's resprov ON and this image
    // re-tiled, backing-sourced or a known asset: rpOn && rtShape OK, fixed before the first chunk) makes the copy a resprov
    // candidate, latched FULL so resprov's full-read-back rung can record it.
    // build 0.0.527 (SKIP82.md item 6): a switch-82 CANDIDATE (SKIP mode, the source equals the stored image) is latched FULL
    // too, so its copy is read back in full and can establish the entry. Asked at the first chunk only (the latch reads it there).
    const uint32_t cand = (retile ? 1u : 0u) | ((pos == 0u && gN48Sk82On) ? navi48_sk82_cand_mine() : 0u);
    const uint64_t r = navi48_fc_chunk(pos, wBytes, dAt, n, &fc_fill_batch, &c, lead, cgLo, cgHi, cand);
    if (!(r & N48_FC_R_TAKEN) && is && is->plan) is->plan->overlaid = ov0;
    return r;
}

// =============================================================================================================================
// build 0.0.529 (an internal design note item 6, ; gfx_cg84.h) — SWITCH 84's COPIER SIDE. Two noinline helpers so
// hook_page_texture (which inlines residency_copy_to_vram) keeps its frame: G0 (d84_plan, after the pre-flight and before the copy's
// Navi48CopyScope) and the ON delta write (d84_delta_copy, which replaces the rest of the copy). NOTHING OF APPLE'S IS WRITTEN: the
// delta is our own shadow's diff; Apple's dirty range (res+0x138 / +0x140) is READ, only to count a delta outside it.
// =============================================================================================================================
// MF-3: shadercache's own candidate predicate (sc_lookup, the call sc_scan_window makes at every grid start) over img[ds, de): 1 when
// any grid-aligned start there is a PROGRAM CANDIDATE - anything but SC_MISS_EMPTY (first dword zero) or SC_MISS_NOEND (no terminator
// within the key length). With the shader cache not armed nothing would ever be substituted in any copy: 0. Called under gD84Lock
// (pure CPU over the thread-owned scratch).
uint32_t navi48_d84_programs_in(void *, const uint8_t *img, uint64_t bytes, uint64_t ds, uint64_t de) {
    if (!gScArmed || !gScOpened) return 0u;
    if (!img || de > bytes || ds >= de) return 1u;
    uint32_t grid = gSc.grid;
    if (grid < 4u || (grid & (grid - 1u))) return 1u;   // fail-closed
    for (uint64_t o = ds & ~(uint64_t)(grid - 1u); o < de && o + 4u <= bytes; o += grid) {
        sc_match m {};
        const int r = sc_lookup(&gSc, img + o, (size_t)(bytes - o), &m);
        if (r == SC_MISS && (m.miss_reason == SC_MISS_EMPTY || m.miss_reason == SC_MISS_NOEND)) continue;
        return 1u;
    }
    return 0u;
}
static int d84_wr(void *, uint64_t vram, const uint32_t *src, uint32_t dwords) { return navi48_vram_write_mm(vram, src, dwords) ? 1 : 0; }
static int d84_rd(void *, uint64_t vram, uint32_t *dst, uint32_t dwords) { return navi48_vram_read_mm(vram, dst, dwords) ? 1 : 0; }
static uint64_t gD84Lines { 0ull };
static uint32_t gD84Buf[64], gD84Back[64];   // the delta loop's batches: ONE delta at a time (the scratch's owner), off the stack
// THE ON DELTA (CG84.md item 6): [ds, de) of the resource at VRAM `vram`, from the plan-time image, through the MM loop only (no
// fc_copy_chunk: its latch keys on pos == 0), every dword read back, under a scope of EXACTLY [vram + ds, vram + de). Skips
// shadercache_scan_resource, ic_begin and the resprov record (the key's establishing copy ran them and found nothing). The update
// runs from the scope's close (navi48_cg_close_scope -> navi48_d84_closed). Returns as the rest of residency_copy_to_vram would.
static __attribute__((noinline)) bool d84_delta_copy(void *self, uint64_t vram, uint64_t bytes, uint64_t packed) {
    const uint64_t ds = (packed >> 32) & 0x7fffffffull, de = packed & 0xffffffffull;
    uint64_t lo = 0ull, hi = 0ull;
    n48_d84_scope(vram, ds, de, &lo, &hi);
    uint64_t t0 = 0ull, t1 = 0ull, ns = 0ull;
    clock_get_uptime(&t0);
    uint64_t compared = 0ull, mismatched = 0ull;
    bool ok = false;
    {
        Navi48CopyScope cgScope(lo, hi);
        const uint8_t *img = navi48_d84_scratch();
        ok = img && de > ds && de <= bytes &&
             n48_d84_copy_loop(nullptr, &d84_wr, &d84_rd, img, vram, ds, de, &compared, &mismatched, gD84Buf, gD84Back) != 0;
        if (!ok) {
            gCopy.failed++;
            cgScope.markFailed();   // a partial write: poisoned over exactly the delta
        } else {
            if (mismatched) cgScope.markMismatch();
            navi48_d84_result(1u, compared, mismatched, de - ds);   // before the scope closes
            gCopy.copies++;
            gCopy.bytes += de - ds; gCopy.compared += compared; gCopy.mismatched += mismatched;
            gCopy.lastDst = vram; gCopy.lastBytes = de - ds;   // fix pass SHOULD: the resource's VRAM, not the delta's
        }
    }
    clock_get_uptime(&t1);
    if (t1 >= t0) absolutetime_to_nanoseconds(t1 - t0, &ns);
    const uint64_t n = ++gD84Lines;
    if (!ok)
        PEERLOG("residency-copy: DELTA FAILED resource=%p VRAM [%#llx,%#llx) - a write or read-back batch was refused; the delta is "
                "poisoned, this pageTexture keeps the skip", self, (unsigned long long)lo, (unsigned long long)hi);
    else if (n <= N48_D84_LOG_FIRST || (n % N48_D84_LOG_EVERY) == 0u)
        PEERLOG(N48_D84_DELTA_FMT, (unsigned long long)gCopy.copies, self, (unsigned long long)lo, (unsigned long long)hi,
                (unsigned long long)vram, (unsigned long long)(vram + bytes), (unsigned long long)(de - ds), (unsigned long long)compared,
                (unsigned long long)mismatched, (unsigned long long)(ns / 1000ull), (unsigned long long)n);
    return ok;
}

// Six arguments (all in registers): hook_page_texture, which inlines the call site, must not grow an outgoing-argument area. `segs`
// is the pre-flight's segment count, with bit 32 set when switch 59 prepared an image (linBytes != 0). bytes, backingOffset and
// dstMem are re-read from the objects residency_copy_to_vram already validated.
// Returns 0 (the copy below is 0.0.528's - OFF, not eligible, no key, busy, SHADOW, or an ON establishing copy whose source and path
// navi48_d84_plan redirected by thread), or, ON with a valid key, the DELTA'S OWN ANSWER: 1 (true) or 2 (false), after
// d84_delta_copy ran in place of the rest of the copy. One small integer back: the caller keeps no packed value alive.
// The state snapshot and Apple's dirty union (read-only), in their own frame (each helper <= 0x100 B of stack).
static __attribute__((noinline)) uint64_t d84_plan_snap(const n48_d84_elig *e, const n48_d84_id *id, IOMemoryDescriptor *md, uint64_t cgLo,
                                                        uint64_t cgHi, const char *r) {
    n48_sk82_snap sn {};
    sk82_snap_now(&sn);
    const uint64_t dLo = *reinterpret_cast<const uint64_t *>(r + 0x138), dLen = *reinterpret_cast<const uint64_t *>(r + 0x140);
    return navi48_d84_plan(e, id, &sn, md, cgLo, cgHi, dLo, dLo + dLen);
}
static __attribute__((noinline)) uint32_t d84_plan(void *self, void *dstMap, IOMemoryDescriptor *md, uint64_t cgLo, uint64_t cgHi,
                                                   uint64_t segs) {
    const char *r = static_cast<const char *>(self);
    const uint64_t bytes = *reinterpret_cast<const uint64_t *>(r + 0x230);
    const uint64_t backingOffset = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    void *dstMem = *reinterpret_cast<void *const *>(static_cast<const char *>(dstMap) + 0x18);
    const unsigned dSegs = (unsigned)(segs & 0xffffffffull);
    n48_d84_elig e {};
    e.type = *reinterpret_cast<const uint8_t *>(r + 0x14);
    e.swz = rp_pageout_swz_dword(r);                                   // the record the hardware reads (res+0x1dc without one)
    e.swz1dc = *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    const void *mask = *reinterpret_cast<void *const *>(r + 0x180);
    e.maskBad = (mask && reinterpret_cast<uintptr_t>(mask) < kKernelHalfBase) ? 1u : 0u;
    e.bytes = bytes; e.dsegs = (cgHi - cgLo == bytes) ? dSegs : 0u; e.linBytes = (segs >> 32) ? 1u : 0u;
    e.keyOk = 1u;   // self, the maps and md: checked by the caller
    n48_d84_id id {};
    id.res = reinterpret_cast<uintptr_t>(self); id.dstMem = reinterpret_cast<uintptr_t>(dstMem); id.vram = cgLo; id.bytes = bytes;
    id.boff = backingOffset; id.md = reinterpret_cast<uintptr_t>(md); id.pid = proc_selfpid();
    uint32_t vaOk = 0u;
    id.va = map_gpu_va(dstMap, &vaOk);
    e.vaOk = vaOk;
    const uint64_t packed = d84_plan_snap(&e, &id, md, cgLo, cgHi, r);
    if (!packed) return 0u;
    return d84_delta_copy(self, cgLo, bytes, packed) ? 1u : 2u;
}
// true: every byte of the resource was written and read back (the mismatch count
// says whether it stuck). false: this call keeps the skip - before any write for
// every refusal, or on a mid-copy failure (counted in gCopy.failed, logged).
static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {
    Navi48MmTakerScope mmTaker(N48_MMT_RESIDENCY);   // build 0.0.514 A3: this copy's gVramMmLock holds (read-only instrument)
    if (!gResOrigVt || !gResCopyVt ||
        reinterpret_cast<uintptr_t>(self) < kKernelHalfBase ||
        reinterpret_cast<uintptr_t>(dstMap) < kKernelHalfBase ||
        reinterpret_cast<uintptr_t>(srcMap) < kKernelHalfBase ||
        *reinterpret_cast<void ***>(self) != gResCopyVt) {
        COPY_UNHANDLED(kCopyShapeIdentity, "resource %p is not a patched AMDAccelResource or a "
                       "map is not a kernel pointer (dst %p src %p)", self, dstMap, srcMap);
        return false;
    }
    const char *r = static_cast<const char *>(self);
    const uint64_t bytes         = *reinterpret_cast<const uint64_t *>(r + 0x230);
    const uint64_t backingOffset = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    const unsigned type          = *reinterpret_cast<const uint8_t *>(r + 0x14);
    const unsigned nibble        = (*reinterpret_cast<const uint32_t *>(r + 0x1b4) >> 26) & 0xfu;
    void *dstMem = *reinterpret_cast<void *const *>(static_cast<const char *>(dstMap) + 0x18);
    void *srcMem = *reinterpret_cast<void *const *>(static_cast<const char *>(srcMap) + 0x18);
    if (reinterpret_cast<uintptr_t>(dstMem) < kKernelHalfBase ||
        reinterpret_cast<uintptr_t>(srcMem) < kKernelHalfBase) {
        COPY_UNHANDLED(kCopyShapeIdentity, "map memory objects are not kernel pointers "
                       "(dst %p src %p)", dstMem, srcMem);
        return false;
    }
    const OSMetaClass *dMeta = static_cast<OSObject *>(dstMem)->getMetaClass();
    const OSMetaClass *sMeta = static_cast<OSObject *>(srcMem)->getMetaClass();
    const char *dName = dMeta ? dMeta->getClassName() : nullptr;
    const char *sName = sMeta ? sMeta->getClassName() : nullptr;
    if (!dName || !sName || strcmp(dName, "AMDRadeonX6000_AMDAccelVidMemory") ||
        (strcmp(sName, "AMDRadeonX6000_AMDAccelSysMemory") && strcmp(sName, "IOAccelSysMemory"))) {
        COPY_UNHANDLED(kCopyShapeClass, "destination %s, source %s (handled: "
                       "IOAccelSysMemory / AMDRadeonX6000_AMDAccelSysMemory -> "
                       "AMDRadeonX6000_AMDAccelVidMemory)", dName ? dName : "?", sName ? sName : "?");
        return false;
    }
    const uint64_t dstLen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(dstMem) + 0x40);
    const uint64_t srcLen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(srcMem) + 0x40);
    if (bytes == 0 || bytes > kCopyMaxBytes || bytes > dstLen ||
        backingOffset > srcLen || bytes > srcLen - backingOffset) {
        COPY_UNHANDLED(kCopyShapeLength, "bytes %#llx backingOffset %#llx destination length "
                       "%#llx source length %#llx (limit %#llx)", (unsigned long long)bytes,
                       (unsigned long long)backingOffset, (unsigned long long)dstLen,
                       (unsigned long long)srcLen, (unsigned long long)kCopyMaxBytes);
        return false;
    }
    uintptr_t dSeen = 0;
    const PhysSegmentFn dSeg = vid_segment_fn(dstMem, &dSeen);
    if (!dSeg) {
        const uintptr_t slide = x6000_slide_from_page_texture();
        COPY_UNHANDLED(kCopyShapeAccessor, "destination physical-segment accessor %p (static %#lx, "
                       "want %#lx, slide %#lx)", (void *)dSeen, (unsigned long)(dSeen - slide),
                       (unsigned long)kStaticVidSegment, (unsigned long)slide);
        return false;
    }
    uint64_t prep = 0;
    IOMemoryDescriptor *md = backing_descriptor(srcMem, &prep);
    const uint64_t mdLen = md ? (uint64_t)md->getLength() : 0;
    if (!md || prep == kIOPreparationIDUnprepared || mdLen < backingOffset + bytes) {
        COPY_UNHANDLED(kCopyShapeBacking, "backing descriptor at source+0xd0 = %p (%s), "
                       "preparationID %#llx, length %#llx, need %#llx", md,
                       md ? md->getMetaClass()->getClassName() : "not an IOMemoryDescriptor",
                       (unsigned long long)prep, (unsigned long long)mdLen,
                       (unsigned long long)(backingOffset + bytes));
        return false;
    }

    // build 0.0.486 (switch 59; ws_resprov.h section 6): the backing-sourced copy is decided and PREPARED here, before the
    // pre-flight, because its gfx12 image is LONGER than `bytes`: `wBytes` is the length the pre-flight walks through the VRAM
    // guard, the copy-guard scope covers, and the write loop writes. It equals `bytes` for every copy switch 59 does not take
    // (always, while it is off), so everything below is today's code on today's length. rp_lin_prepare also logs STEP 0.
    // Stack (0.0.486 measured hook_page_texture, which inlines this function): no new address-taken local - the prepared
    // image's length and kind lived in `rt` (0.0.486: rt.nd, rt.kind; 0.0.492: the heap stream rt.ls), which this function held.
    // build 0.0.492: the prepared stream (rt.ls) carries the gfx12 length; the image itself is converted chunk by chunk
    // inside the write loop below (rp_lin_fetch), never held whole.
    // build 0.0.527 (an internal design note item 7; gfx_sk82.h): switch 82, ONCE, before rp_lin_prepare and before the copy's
    // scope opens. true = a TRUSTED entry's image equals the whole source and every check passed (SKIP mode): report success with
    // nothing else done - no scope, no ring event, no fast-copy slot, no provenance, no gCopy change, no COPIED line (item 8).
    // Otherwise this is the copy's G0. ONE load while switch 82 is OFF.
    if (gN48Sk82On && sk82_try_skip(self, dstMap, dstMem, dSeg, md)) return true;
    const bool rpOn = n48::hw_resprov_on();
    RtBufs rt;
    if (rpOn) (void)rp_lin_prepare(r, md, srcLen, dstLen, &rt);
    const uint64_t linBytes = rt.lin & ~N48_RT_LINBLK;   // 0 = not prepared: always, while switch 59 is off
    const uint64_t wBytes = linBytes ? linBytes : bytes;

    // Pre-flight: every destination segment must pass the VRAM guard BEFORE any write. 2 (notes
    // an internal design note): also track this copy's own BOUNDING range [cgLo, cgHi) across every
    // segment here, so the guard's scope (opened right after this loop, below) covers exactly what the design
    // asks for - "the bounding range that the pre-flight loop already computes" - even for a non-contiguous copy.
    // build 0.0.486: over wBytes (== bytes unless switch 59 prepared a longer gfx12 image).
    unsigned dSegs = 0;
    uint64_t cgLo = 0, cgHi = 0; bool cgHave = false;
    for (uint64_t pos = 0; pos < wBytes; dSegs++) {
        uint64_t span = 0;
        const uint64_t at = dSeg(dstMem, pos, &span);
        if (span == 0 || (at & 3) || dSegs >= 4096) {
            COPY_UNHANDLED(kCopyShapeSegment, "destination segment %u at resource offset %#llx: "
                           "VRAM %#llx span %#llx", dSegs, (unsigned long long)pos,
                           (unsigned long long)at, (unsigned long long)span);
            return false;
        }
        const uint64_t n = (span < wBytes - pos) ? span : wBytes - pos;
        const uint32_t why = navi48_vram_apple_dest_check(at, (n + 3) & ~3ULL);
        if (why) {
            COPY_UNHANDLED(kCopyShapeDest, "destination VRAM [%#llx,%#llx) refused by the VRAM "
                           "guard, reason %u (1 no context, 2 no hi pool, 3 outside our hi pool, "
                           "4 hi pool has live allocations, 5 misaligned, 6 overflow)",
                           (unsigned long long)at, (unsigned long long)(at + n), why);
            return false;
        }
        if (!cgHave) { cgLo = at; cgHi = at + n; cgHave = true; }
        else { if (at < cgLo) cgLo = at; if (at + n > cgHi) cgHi = at + n; }
        pos += n;
    }
    // build 0.0.529 (an internal design note item 6): switch 84's G0, after the pre-flight and before this copy's scope. 0 (OFF, not
    // eligible, no key, busy, SHADOW, or an ON establishing copy - whose source and path d84_plan redirected by thread): the copy
    // below is 0.0.528's. Non-zero: ON with a valid key - the delta (d84_delta_copy, called by d84_plan) replaced the rest of this
    // copy, and this is its answer (1 true, 2 false). ONE load while 84 never left OFF.
    // __builtin_expect: the OFF path's layout is kept (measured: without the hint hook_page_texture's frame grew by 0x10).
    if (__builtin_expect(gN48D84Live != 0u, 0)) {
        const uint32_t d84 = d84_plan(self, dstMap, md, cgLo, cgHi, (uint64_t)dSegs | (linBytes ? (1ull << 32) : 0ull));
        if (d84) return d84 == 1u;
    }
    // 2: open the copy-guard scope now - before this copy's first MM access - so it encloses the write
    // loop below, shadercache_scan_resource and substitute_blit_kernel_at. RAII: every return path from here on
    // (including the FAILED returns inside the write loop) closes it, poisoning [cgLo, cgHi) exactly when
    // markFailed()/markMismatch() was called first.
    Navi48CopyScope cgScope(cgLo, cgHi);

    // (switch on only): a proven-shape ADDR_SW_4KB_D_X resource is RE-TILED to ADDR3_4KB_2D before it is written, so
    // what lands in VRAM is what gfx1201's texture unit reads. Any refusal keeps the byte-for-byte copy (and it is not ledgered).
    uint32_t rtShape = N48_RP_SHAPE_REASONS, rpWarn = 0u;
    // build 0.0.450 item 1, switch 46: which kind actually matched, and ITS gfx12 mode - so n48_rp_record below
    // ledgers the SAME mode a 256B_D re-tile actually produced (N48_RP_G12_256B_2D), not always N48_RP_G12_4KB_2D.
    // Defaulted to the always-on 32bpp kind, unchanged, so a switch-46-off run's rc.mode is 0.0.449's line for line.
    uint32_t rtKind = N48_RP_KIND_4KB_D_X_32, rtMode = N48_RP_G12_4KB_2D;
    const char *rtCfgWhy = "switch off";
    // build 0.0.486 ('s instrument fix): the reason the resprov line prints - the check that got FURTHEST, the 32bpp
    // check's or a switch-46 kind's (n48_rp_shape_further). Log text only: rtShape and every counter are unchanged.
    uint32_t rtShapeLog = N48_RP_SHAPE_REASONS;
    n48_rp_shape shp {};
    if (rpOn) {
        gRt.considered++;
        shp = resource_shape(r, &rtCfgWhy);
        if (shp.gbRead) { gRt.gbRead = 1; gRt.lastGb = shp.gb; }
        rpWarn = n48_rp_mask_warn(&shp);            //: counted, never a refusal - the hardware ignores res+0x1dc here
        if (rpWarn & N48_RP_WARN_SWZ) gRt.warnSwz++;
        if (rpWarn & N48_RP_WARN_TYPE) gRt.warnType++;
        rtShape = n48_rp_shape_check(&shp);
        rtShapeLog = rtShape;
        if (rtShape < N48_RP_SHAPE_REASONS) gRt.shape[rtShape]++;
        if (rtShape == N48_RP_SHAPE_BYTES_SMALL)
            rp_log_bytes_small_once(r, shp.w, shp.h, shp.rowBytes, shp.bytes, n48_rp_swz_dword(&shp) & 0x1fu);
        // build 0.0.450 item 1, switch 46 (DEFAULT OFF): the three T450 golden-tested kinds n48_rp_shape_check
        // does not admit (4KB_D_X@8/64bpp, 256B_D@8bpp). Tried ONLY when the 32bpp check above refused AND the
        // switch is on (n48::hw_resprov_kinds_on(), itself gated on hw_resprov_on()), narrowed by the resource's OWN
        // swizzle mode (a 256B_D resource is never tried against a 4KB_D_X hypothesis or vice versa). resource_shape
        // reads Apple's GB_ADDR_CONFIG ONLY for a mode-22 resource (`if ((...) == N48_RP_G10_4KB_D_X) s.gbRead = ...`,
        // above) - a mode-2 (256B_D) resource's `shp.gbRead` is therefore always 0 from that call, so this re-reads
        // it into a LOCAL copy here, but ONLY under this switch and ONLY for a candidate whose mode already matches:
        // with the switch off, or on a resource of neither mode, resource_shape's own call above stays 0.0.449's,
        // byte for byte, and no extra register read happens.
        if (rtShape != N48_RP_SHAPE_OK && n48::hw_resprov_kinds_on()) {
            const uint32_t mode = n48_rp_swz_dword(&shp) & 0x1fu;
            static const uint32_t k4kbdx[2] = { N48_RP_KIND_4KB_D_X_8, N48_RP_KIND_4KB_D_X_64 };
            static const uint32_t k256d[1]  = { N48_RP_KIND_256B_D_8 };
            const uint32_t *cand = (mode == N48_RP_G10_4KB_D_X) ? k4kbdx : (mode == N48_RP_G10_256B_D) ? k256d : nullptr;
            const uint32_t ncand = (mode == N48_RP_G10_4KB_D_X) ? 2u : (mode == N48_RP_G10_256B_D) ? 1u : 0u;
            for (uint32_t ci = 0; ci < ncand; ci++) {
                n48_rp_shape s2 = shp;
                if (!s2.gbRead) s2.gbRead = (uint32_t)apple_gb_addr_config(r, &s2.gb, &rtCfgWhy);
                const uint32_t rs = n48_rp_shape_check_kind(&s2, cand[ci]);
                rtShapeLog = n48_rp_shape_further(rtShapeLog, rs);
                if (rs == N48_RP_SHAPE_OK) {
                    shp = s2; rtShape = rs; rtKind = cand[ci];
                    rtMode = (cand[ci] == N48_RP_KIND_256B_D_8) ? N48_RP_G12_256B_2D : N48_RP_G12_4KB_2D;
                    break;
                }
                // build 0.0.451 item 1 (S1): this specific kind hypothesis matched everything but the byte
                // count, and Apple's allocation is SMALLER than the layout it would need - name it, once per
                // resource, even though `rtShape` itself is not updated on a failed candidate (unchanged from the
                // 32bpp check above, exactly as 0.0.450 left it - only a SUCCESSFUL candidate ever changes rtShape).
                if (rs == N48_RP_SHAPE_BYTES_SMALL)
                    rp_log_bytes_small_once(r, s2.w, s2.h, s2.rowBytes, s2.bytes, n48_rp_swz_dword(&s2) & 0x1fu);
            }
        }
        // build 0.0.492 item 3 (PRIORITY): switch 59 ON and a LINEAR backing (rt.lin bit 0, set by rp_lin_prepare from the
        // record's +0x44): the gfx10 re-tiles above read the system-memory bytes as gfx10-tiled, which showed they
        // never are - they refuse here, BEFORE any buffer is allocated or byte re-ordered; the backing-sourced copy (if its
        // check passed) takes the copy below, else the plain byte-for-byte copy stands, unrecorded. Switch 59 OFF: the bit is 0.
        if ((rt.lin & N48_RT_LINBLK) && rtShape == N48_RP_SHAPE_OK) {
            rp_lin_old_blocked(self, rt.ls);   // noinline: counts and logs (hook_page_texture's frame must not grow)
            rtShape = N48_RP_SHAPE_REASONS; rtShapeLog = N48_RP_SHAPE_REASONS;
            rtKind = N48_RP_KIND_4KB_D_X_32; rtMode = N48_RP_G12_4KB_2D;
        }
        // build 0.0.486: should both ever claim one copy, nothing is written: this pageTexture keeps the skip. 0.0.492: the
        // block just above makes this unreachable (a prepared stream implies a linear backing, so the bit is 1); kept, fail-closed.
        if (linBytes && rtShape == N48_RP_SHAPE_OK) {
            n48::hw_resprov_lin_note(N48_RP_LINEV_FAILED);
            cgScope.markNothingWritten();
            PEERLOG("residency-copy: resource=%p claimed by BOTH the backing-sourced copy and a gfx10 re-tile - refused, nothing "
                    "written, this pageTexture keeps the skip", self);
            return false;
        }
        if (rtShape == N48_RP_SHAPE_OK) {
            // item 1's write-range proof: `rt.dst` is IOMalloc'd to EXACTLY `bytes` (rt.n = bytes, just below), the
            // SAME `bytes` n48_rp_shape_check_kind/n48_rp_shape_check just proved equals n48_rp_kind_bytes(kind, w, h)
            // (or n48_rp_bytes(w, h) for the 32bpp kind); n48_rp_retile/n48_rp_retile_kind re-verify that equality
            // themselves before writing a single byte and write only offsets their own golden-tested equations
            // produce, which are bijections over [0, n48_rp_kind_bytes(kind, w, h)) by construction (ws_resprov.h)
            // - so every byte landing in `rt.dst` stays inside this buffer. The VRAM DESTINATION range this buffer
            // is later copied into is a SEPARATE bound this item does not touch: the pre-flight loop above (already
            // run, before any resprov code) walked every destination segment through navi48_vram_apple_dest_check
            // and recorded [cgLo, cgHi) BEFORE this switch is ever consulted, exactly as the 32bpp path already
            // does - the switch only changes WHICH BYTES are written (gfx10 or one more re-tile's gfx12 order),
            // never HOW MANY or WHERE.
            // build 0.0.451 item 1 (S1): `bytes` (== shp.bytes) may be the kind's EXACT layout size or that size
            // rounded up to Apple's own page - the shape check above just proved one of the two. Retile ONLY the
            // exact layout portion (`exactBytes`), which is what n48_rp_retile/n48_rp_retile_kind's own internal
            // check requires anyway; the trailing padding, if any, is a PLAIN COPY - it is outside every re-tile
            // equation's domain (the block layout does not cover it), so it is not gfx10-or-gfx12 ordered data at
            // all, just Apple's own page-rounding filler, carried through unchanged exactly like a byte-for-byte
            // copy would.
            const uint64_t exactBytes = (rtKind == N48_RP_KIND_4KB_D_X_32) ? n48_rp_bytes(shp.w, shp.h)
                                                                            : n48_rp_kind_bytes(rtKind, shp.w, shp.h);
            rt.n = (uint32_t)bytes;   // <= N48_RP_MAX_BYTES: both shape checks demand it (build 0.0.492: RtBufs::n is 32-bit)
            rt.src = static_cast<uint32_t *>(IOMalloc(rt.n));
            rt.dst = static_cast<uint32_t *>(IOMalloc(rt.n));
            if (!rt.src || !rt.dst) { gRt.allocFail++; rtShape = N48_RP_SHAPE_REASONS; }
            else if (md->readBytes(backingOffset, rt.src, rt.n) != rt.n) { gRt.readFail++; rtShape = N48_RP_SHAPE_REASONS; }
            else {
                const int okRetile = (rtKind == N48_RP_KIND_4KB_D_X_32)
                    ? n48_rp_retile(rt.src, rt.dst, shp.w, shp.h, exactBytes, 1)
                    : n48_rp_retile_kind(rtKind, reinterpret_cast<const uint8_t *>(rt.src), reinterpret_cast<uint8_t *>(rt.dst),
                                         shp.w, shp.h, exactBytes, 1);
                if (!okRetile) rtShape = N48_RP_SHAPE_REASONS;
                else if (bytes > exactBytes)   // the page-rounded tail: plain copy, untouched by any re-tile equation
                    memcpy(reinterpret_cast<uint8_t *>(rt.dst) + exactBytes, reinterpret_cast<const uint8_t *>(rt.src) + exactBytes,
                           (size_t)(bytes - exactBytes));
                // set BEFORE the first write: page-out symmetry holds even if the copy fails. PER MODE (item 2's
                // reviewer fix): the 256B_D flag is reachable ONLY through rtKind == N48_RP_KIND_256B_D_8, which
                // is reachable ONLY under switch 46 (hw_resprov_kinds_on(), above) - never by the always-on path.
                else if (rtKind == N48_RP_KIND_256B_D_8) gRpEverRetiled256bd = 1;
                else gRpEverRetiled4kbdx = 1;
            }
        }
        // build 0.0.486 (switch 59): the backing-sourced image is what the write loop below writes (0.0.492: streamed through
        // rp_lin_fetch, wBytes long). The page-out flag OF THE VRAM SIDE'S OWN MODE is set HERE, before the first write: a later
        // page-out of such a resource is refused and gfx12 bytes never reach system memory (sticky for the boot). 0.0.492: mode 22,
        // 2 or 27 (n48_rp_lin_check admits only kN48RpLinModes), each with its own flag.
        if (linBytes) {
            rtMode = rp_lin_mark_pageout(rt.ls);   // noinline: sets the VRAM mode's flag, returns the gfx12 mode written
            rtShape = N48_RP_SHAPE_OK; rtShapeLog = N48_RP_SHAPE_OK;
        }
    }
    const bool retile = rpOn && rtShape == N48_RP_SHAPE_OK;
    // build 0.0.495 (switch 62): substitute IN the copy - pre-scan, patches and the heap generation's first bump, all BEFORE
    // the first VRAM write below. A re-tiled or longer image cannot carry a patch (its programs are poisoned instead).
    if (n48::hw_hg_on()) ic_begin(dstMem, dSeg, md, backingOffset, bytes, (retile || wBytes != bytes) ? 1u : 0u, cgLo, cgHi, dstMap, self);
    uint64_t t0 = 0; clock_get_uptime(&t0);
    uint8_t  raw[256];
    uint32_t buf[64], back[64];
    uint64_t compared = 0, mismatched = 0, firstDst = 0, pos = 0;
    uint32_t firstSrc[2] = { 0, 0 };
    uint32_t head[32] = { 0 }; uint32_t headN = 0;     //: every copied resource's first dwords, logged
    unsigned chunks = 0;
    bool contiguous = true;                        //: one VRAM range [firstDst, firstDst + bytes)
    // build 0.0.486: wBytes == bytes unless switch 59 prepared a longer gfx12 image (the pre-flight walked exactly this).
    while (pos < wBytes) {
        uint64_t dSpan = 0;
        const uint64_t dAt = dSeg(dstMem, pos, &dSpan);
        if (pos == 0) firstDst = dAt;
        else if (dAt != firstDst + pos) contiguous = false;
        // build 0.0.496: the chunk plan is fastcopy.h's n48_fc_chunk_len - THIS loop's own arithmetic through 0.0.495 (at most
        // the segment's span, at most kCopyChunkBytes, dword boundaries until the tail), shared with the SDMA path and host-tested.
        const uint64_t n = n48_fc_chunk_len(wBytes, pos, dSpan, kCopyChunkBytes);
        if (n == 0 || (dAt & 3) || navi48_vram_apple_dest_check(dAt, (n + 3) & ~3ULL)) {
            gCopy.failed++;
            // 0.0.437: POISON ONLY WHAT WAS WRITTEN. `pos` IS "byte(s) already written" - the
            // log line right below says so - so a refusal at pos == 0 (the very first chunk, before any MM write
            // this call has ever issued) must not poison at all; a refusal after earlier chunks landed (pos > 0)
            // still does, exactly as before this fix.
            // 0.0.438: the 0.0.437 fix skipped markFailed() here but left the scope's own
            // `wrote_` at its constructor default (true), so CLOSE still ran n48_cg_close_poison with wrote=1,
            // failed=0 - a CLEAN close over [cgLo, cgHi), which CLEARS any poison a previous failed copy left over
            // that range although this call never wrote a byte of it ('s phantom defect, again). A pos == 0
            // refusal must close as "nothing written" instead, so CLOSE takes neither branch.
            if (pos > 0) cgScope.markFailed();
            else cgScope.markNothingWritten();
            PEERLOG("residency-copy: FAILED at resource offset %#llx of %#llx (destination %#llx "
                    "span %#llx); %#llx byte(s) already written, this pageTexture keeps the skip",
                    (unsigned long long)pos, (unsigned long long)wBytes, (unsigned long long)dAt,
                    (unsigned long long)dSpan, (unsigned long long)pos);
            return false;
        }
        const char *fail = nullptr;
        // build 0.0.496 (switch 63, latched per copy): SDMA takes this chunk - the SAME bytes (this loop's producer, overlay
        // included) staged, copied by SDMA0 QUEUE0 into [dAt, dAt + n), which the check just above passed, and verified through
        // the MM window - or it declines and this loop runs it exactly as 0.0.495 did. A failed SDMA chunk takes this loop's
        // FAILED path below (poisoned, the skip kept).
        const uint64_t fc = fc_copy_chunk(&rt, retile, md, backingOffset, pos, wBytes, dAt, n, raw, cgLo, cgHi);
        if (fc & N48_FC_R_TAKEN) {
            if (fc & N48_FC_R_FAIL) fail = n48_fc_fail_text(fc);
            else {
                compared += (fc >> 32) & 0x3FFFFFFFull;
                mismatched += (uint32_t)fc;
                // the log's head (the copy's first 32 dwords) and first two source dwords: the SDMA path left this chunk's bytes
                // below resource offset 128 at the start of `raw` - exactly the dwords the batch loop would have appended
                for (uint32_t i = 0; headN < 32u && i < 32u && i * 4u < n; i++) { uint32_t w; memcpy(&w, raw + 4u * i, 4); head[headN++] = w; }
                if (pos == 0) { firstSrc[0] = head[0]; firstSrc[1] = n >= 8u ? head[1] : 0u; }
            }
        } else
        for (uint64_t k = 0; k < n; ) {
            const uint64_t left = n - k;
            const uint64_t take = left >= 256 ? 256 : left;   // source bytes in this batch
            const uint32_t cnt  = (uint32_t)((take + 3) / 4);
            //: gfx12 order. build 0.0.492: rp_retile_bytes (noinline) - the gfx10 re-tile's whole image (rt.dst, as
            // before), or the backing-sourced copy's gfx12 bytes converted a chunk at a time (rp_lin_fetch).
            if (retile) { if (!rp_retile_bytes(&rt, pos + k, raw, take)) { fail = "the backing-sourced chunk could not be read or converted"; break; } }
            // build 0.0.495: ic_read = this readBytes, then (switch 62, a patched copy on this thread only) our bytes over it.
            else if (ic_read(md, backingOffset, pos + k, raw, take) != take) {
                fail = "readBytes of the backing returned short"; break;
            }
            if (take & 3) {                                    // resource ends inside this dword
                uint32_t cur = 0;
                if (!navi48_vram_read_mm(dAt + k + (take & ~3ULL), &cur, 1)) {
                    fail = "the MM window refused the tail read"; break;
                }
                memcpy(raw + take, reinterpret_cast<const uint8_t *>(&cur) + (take & 3), 4 - (take & 3));
            }
            memcpy(buf, raw, (size_t)cnt * 4);
            if (!navi48_vram_write_mm(dAt + k, buf, cnt)) { fail = "the MM window refused a write batch"; break; }
            if (!navi48_vram_read_mm(dAt + k, back, cnt)) { fail = "the MM window refused a read-back batch"; break; }
            for (uint32_t i = 0; i < cnt; i++) if (back[i] != buf[i]) mismatched++;
            compared += cnt;
            if (pos == 0 && k == 0) { firstSrc[0] = buf[0]; firstSrc[1] = cnt > 1 ? buf[1] : 0; }
            for (uint32_t i = 0; i < cnt && headN < 32; i++) head[headN++] = buf[i];
            k += (uint64_t)cnt * 4;
        }
        if (fail) {
            gCopy.failed++;
            cgScope.markFailed();   // 2: PARTIAL WRITE - some dwords of this chunk may already be written
            PEERLOG("residency-copy: FAILED - %s in the chunk at resource offset %#llx (VRAM %#llx); "
                    "this pageTexture keeps the skip", fail, (unsigned long long)pos,
                    (unsigned long long)dAt);
            return false;
        }
        pos += n;
        chunks++;
    }
    uint64_t t1 = 0; clock_get_uptime(&t1);
    uint64_t ns = 0; absolutetime_to_nanoseconds(t1 - t0, &ns);
    // 2 (VERIFIED): this function returns true below even when mismatched > 0 - the read-back
    // found the write did not stick - so the guard must poison here too, not only on the FAILED early returns above.
    if (mismatched) cgScope.markMismatch();
    gCopy.copies++;
    gCopy.bytes      += wBytes;
    gCopy.compared   += compared;
    gCopy.mismatched += mismatched;
    gCopy.lastDst     = firstDst;
    gCopy.lastBytes   = wBytes;
    gCopy.lastMicros  = ns / 1000;
    if (gN48Sk82On || gN48D84Live) sk82_copy_result(compared, mismatched, wBytes, retile ? 1u : 0u);   // 0.0.527/0.0.529: before the scope closes
    //: the resource's GPU VA, from the destination (VidMemory) map and the source map, and its surface record - so a run ties
    // each copy to the VA a T# names (#585 <-> 0x400006000 was only SUSPECTED in hp5) and censuses the tiled modes copied.
    uint32_t dVaOk = 0, sVaOk = 0;
    const uint64_t dVa = map_gpu_va(dstMap, &dVaOk), sVa = map_gpu_va(srcMap, &sVaOk);
    const uint32_t surfRec = *reinterpret_cast<const uint32_t *>(r + 0x1dc);
    //: BOTH swizzle records on every copy - res+0x1dc and the record the hardware is told when res+0x180 is set - so a run
    // settles which one carries this resource's mode. Read only; +0x40 lies inside the 0x3c8-byte record (resource_shape).
    const void *rec180 = *reinterpret_cast<void *const *>(r + 0x180);
    const bool rec180Ok = reinterpret_cast<uintptr_t>(rec180) >= kKernelHalfBase;
    const uint32_t rec180Swz = rec180Ok ? *reinterpret_cast<const uint32_t *>(static_cast<const char *>(rec180) + 0x40) : 0u;
    PEERLOG("residency-copy: COPIED #%llu resource=%p type=%#x nibble=%u bytes=%#llx "
            "backingOffset=%#llx -> VRAM [%#llx,%#llx) via the MM window in %u chunk(s), %llu us; "
            "read-back %llu dword(s), %llu MISMATCHED; first source dwords %08x %08x; GPU VA %s%#llx (source map %s%#llx); "
            "surface SW_MODE %u type %u %ux%u rowBytes %u; swz res+0x1dc %#x res+0x180 %p *(res+0x180)+0x40 %s%#x (SW_MODE %u type "
            "%u)%s",
            (unsigned long long)gCopy.copies, self, type, nibble, (unsigned long long)bytes,
            (unsigned long long)backingOffset, (unsigned long long)firstDst,
            (unsigned long long)(firstDst + wBytes), chunks, (unsigned long long)(ns / 1000),
            (unsigned long long)compared, (unsigned long long)mismatched, firstSrc[0], firstSrc[1],
            dVaOk ? "" : "UNAVAILABLE ", (unsigned long long)dVa, sVaOk ? "" : "UNAVAILABLE ", (unsigned long long)sVa,
            surfRec & 0x1fu, (surfRec >> 5) & 3u, (unsigned)*reinterpret_cast<const uint16_t *>(r + 0xb0),
            (unsigned)*reinterpret_cast<const uint16_t *>(r + 0xb2), *reinterpret_cast<const uint32_t *>(r + 0xb8),
            surfRec, rec180, rec180Ok ? "" : (rec180 ? "NOT A KERNEL POINTER " : "n/a "), rec180Swz, rec180Swz & 0x1fu,
            (rec180Swz >> 5) & 3u,
            retile ? rp_retile_suffix(rt.ls, rtKind) : "");   // build 0.0.486/0.0.492: names the kind / mode + bpp that ran
    navi48_fc_copy_report(gCopy.copies);   // build 0.0.496: this copy's `via SDMA` line (switch 63 ON only); 0.0.509: the slot is freed at the scope's close
    navi48_c88_copy_note(dVa, dVaOk, wBytes, firstDst, gCopy.copies);   // build 0.0.512 Part B2: the clock88 watch (read-only)
    if (rpOn) {
        n48_rp_copy rc {};
        rc.copied = 1u; rc.retiled = retile ? 1u : 0u;
        rc.bytes = wBytes; rc.compared = compared; rc.mismatched = mismatched;   // build 0.0.486: the length written
        rc.vaOk = dVaOk; rc.va = dVa;
        rc.contiguous = contiguous ? 1u : 0u; rc.vram = firstDst;
        // build 0.0.450 item 1: rtMode is the KIND that actually matched (N48_RP_G12_4KB_2D by default, the
        // ONLY value 0.0.449 ever produced; N48_RP_G12_256B_2D only when switch 46 admitted a 256B_D resource), so
        // n48_rp_ok's mode match at the ask stays exact for every kind. With switch 46 off, rtMode never changes
        // from its default and this line is 0.0.449's, byte for byte.
        rc.mode = retile ? rtMode : 0u;      // only a re-tiled image is claimed tiled; anything else refuses MODE
        // build 0.0.451 item 2 (S4): the MATCHED kind's own bytes-per-element (n48_rp_kind_geom's `eb`,
        // 4/1/8/1 for 32/8/64bpp 4KB_D_X and 256B_D@8bpp respectively - reduces to 4 for the always-on default
        // rtKind exactly as 0.0.450 left it), so the ask can catch a pitch-collision (a 128bpp resource matching
        // the 64bpp kind's pitch, or a 16bpp one matching the 32bpp kind's) that the shape check alone cannot.
        { uint32_t bw = 0, bh = 0, eb = 0; n48_rp_kind_geom(rtKind, &bw, &bh, &eb); rc.elemBytes = retile ? eb : 0u; }
        // build 0.0.486 (switch 59): a backing-sourced entry, with the image's own width and height for the T# match.
        // build 0.0.492: its element size is the format pairing's (n48_rp_lin_fmt_pair), its mode the VRAM side's gfx12 mode.
        if (linBytes && retile) rp_lin_fill_copy(&rc, rt.ls);   // noinline: lin, w, h, elemBytes; counts WRITTEN
        uint64_t key = 0;
        const uint32_t why = n48::hw_resprov_note_copy(&rc, proc_selfpid(), &key, gCopy.copies);
        if (gRt.logged < kRpLogLines || why == N48_RP_REC_OK) {
            gRt.logged++;
            // build 0.0.453 item 7 (inv-f84/REPORT.txt): this used to be ONE PEERLOG call whose format string
            // alone (before ANY value was substituted - the shape/reason legend text) is ~518 bytes with the
            // "Navi48AccelPeer: " prefix, already past n48_logf's 512-byte line buffer (src/amd/n48log.cpp:
            // `char line[512]`) - so the tail (context, VA, VRAM, the two warnings) was cut on EVERY line, which is
            // exactly what  measured ("QUEUED and UNREAD-config lines lose the VA"). Split in two: the
            // per-copy DATA line first (worst case ~430 bytes with every %s at its longest and every %llu at
            // UINT64_MAX), the static shape/reason LEGEND second (worst case ~370 bytes) - so a run that never
            // needs the legend still has the VA on the first line, and neither line is ever cut.
            PEERLOG("resprov: copy #%llu shape %u, Apple gfx10 GB_ADDR_CONFIG %s%#010x (%s); ledger: %s (reason %u), "
                    "context %llu, VA %#llx -> VRAM %#llx%s%s", (unsigned long long)gCopy.copies,
                    rtShapeLog, shp.gbRead ? "" : "UNREAD ", shp.gb, shp.gbRead ? "read" : rtCfgWhy,
                    why == N48_RP_REC_OK ? "RECORDED" : (why == N48_RP_REC_REASONS + 3u ? "QUEUED (lock busy; drained under the lock)"
                    : "not recorded"), why, (unsigned long long)key,
                    (unsigned long long)dVa, (unsigned long long)firstDst,
                    (rpWarn & N48_RP_WARN_SWZ) ? "; WARNING (, not a refusal) res+0x1dc swizzle disagrees with the record "
                    "the hardware reads - 0xbdf9141 cmoveq means the hardware never reads res+0x1dc here" : "",
                    (rpWarn & N48_RP_WARN_TYPE) ? "; WARNING (, not a refusal) res+0x1dc type disagrees" : "");
            PEERLOG("resprov: copy #%llu legend - shape: 0 re-tile 1 mode 2 mask 3 type 4 bpe 5 depth 6 dims 7 bytes "
                    "8 config unread 9 config class 10 bytes too small (Apple's allocation is smaller than the "
                    "layout - unsafe to re-tile) 11 not considered/failed; ledger reason: 0 recorded 1 not copied "
                    "2 unverified 3 not WindowServer's 4 no context 5 no VA 6 split VRAM 7 mode 8 not re-tiled 9 "
                    "full 11 off 12 busy/dropped 13 queued.", (unsigned long long)gCopy.copies);
        }
    }
    for (uint32_t i = 0; i < headN; i += 8)            //: which resource holds what (read-only)
        PEERLOG("residency-copy: #%llu source+%#x %08x %08x %08x %08x %08x %08x %08x %08x",
                (unsigned long long)gCopy.copies, i * 4, head[i], head[i + 1], head[i + 2], head[i + 3],
                head[i + 4], head[i + 5], head[i + 6], head[i + 7]);
    if (bytes >= 0xfb00 + 16) {                       // the measured shader offset
        uint64_t span = 0;
        const uint64_t at = dSeg(dstMem, 0xfb00, &span);
        uint32_t v[4] = { 0, 0, 0, 0 };
        if (span >= 16 && navi48_vram_read_mm(at, v, 4))
            PEERLOG("residency-copy: VRAM %#llx (resource +0xfb00) now reads %08x %08x %08x %08x",
                    (unsigned long long)at, v[0], v[1], v[2], v[3]);
    }
    // 0.0.239 (MILESTONE 3 step 2): with `shadercache` armed, scan everything that was
    // just copied and substitute every shader the cache holds. This runs BEFORE kernsub
    // so that, when both are armed, the log shows the general path doing the work and
    // kernsub refusing with reason 3 (the bytes are no longer Apple's) rather than the
    // other way round. Unarmed it does nothing at all.
    bool scPartialWrite = false, scMismatch = false;
    shadercache_scan_resource(dstMem, dSeg, md, backingOffset, bytes, &scPartialWrite, &scMismatch);
    // 0.0.437: shadercache_hit runs INSIDE this function's own Navi48CopyScope (cgScope, opened
    // above) but has no return value of its own - THIS CALL'S own out-parameters (0.0.438, : replacing the
    // gScPartialWrite/gScMismatch globals) are the least-invasive way to carry "did this scan leave a partial write
    // or an unstuck mismatch behind" back to the copier without threading a new parameter through sc_scan_window's
    // whole callback chain. Read immediately after the call that filled them, exactly once, so a LATER unrelated
    // write into this same cgScope (kernsub, below) is never blamed on an earlier resource's shader-cache damage or
    // vice versa - and a CONCURRENT residency copy's own scan can never clobber this one's answer (0.0.438).
    if (scPartialWrite) cgScope.markFailed();
    if (scMismatch) cgScope.markMismatch();
    // 0.0.204: with `kernsub` armed, replace Apple's Navi21 kernel with the
    // gfx1201 twin at every copy of the shader resource, so the substitution is
    // in place before the CP dispatches and re-applies if Apple ever re-pages.
    // 0.0.206: the armed mode's kernel (copy or an instrument), same guard.
    const uint32_t subBytes = sub_kernel_bytes(gSubMode);
    if (gSubMode && subBytes && bytes >= kBlitShaderOffset + subBytes) {
        uint64_t span = 0;
        const uint64_t at = dSeg(dstMem, kBlitShaderOffset, &span);
        uint32_t reason = 0;
        if (at && span >= subBytes) {
            const uint32_t st = substitute_blit_kernel_at(at, gSubMode, &reason);
            gSub.lastReason = reason;
            // 0.0.437: this call runs INSIDE residency_copy_to_vram's own cgScope (unlike the
            // standalone verb call below, which opens its OWN separate scope) - mirror that call's own two lines
            // exactly: reason 4 is the write/read-back itself failing (a partial write), status 3 is written-but-
            // the-read-back-did-not-stick (a mismatch, VERIFIED to return non-zero anyway).
            if (st == 0 && reason == 4) cgScope.markFailed();
            else if (st == 3) cgScope.markMismatch();
            if (st == 0)
                PEERLOG("residency-copy: kernel substitution at VRAM %#llx REFUSED, reason %u "
                        "(1 guard, 2 read, 3 not Apple's kernel, 4 write/read-back)",
                        (unsigned long long)at, reason);
        } else {
            gSub.lastReason = 1;
            PEERLOG("residency-copy: kernel substitution refused - shader segment at +0xfb00 "
                    "unusable (VRAM %#llx span %#llx)", (unsigned long long)at,
                    (unsigned long long)span);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// 0.0.284 — THE PAGE-OUT COPY: pageTexture(toVram = 0), VRAM -> system memory.
// ---------------------------------------------------------------------------
// probe1: 66 of SecurityAgent's clock drawables were filled with magenta in VRAM and read back clean, and the screen did not
// change by one pixel, while this hook logged `residency-copy: page-out #1 (VRAM -> sysmem) ... SKIPPED`. Since 0.0.200 every page-out
// has reported success WITHOUT copying, so whatever the GPU (or the probe) puts into a VRAM resource never reaches the resource's
// system-memory backing - the copy WindowServer's CPU compositor reads. This is the reverse of residency_copy_to_vram: the
// AMDAccelVidMemory's getPhysicalSegment walk gives the VRAM source, the IOAccelSysMemory's backing descriptor (+0xd0, prepared)
// the destination at res+0xf8 for res+0x230 bytes. Which map is which is taken from the memory objects' CLASSES, not from the
// argument order. Transport: 64-dword MM-window reads, descriptor writeBytes, the first and last 64 dwords read back through
// readBytes and compared. Reads of VRAM change nothing on the GPU; the only write is into Apple's own prepared system-memory
// backing of the very resource Apple asked to page out. Armed separately (`accel pagecopy 2`), at most 4 MiB per resource.
static bool gCopyOutArmed { false };
static struct { uint64_t copies, bytes, refused, failed, mismatched, tooBig, logged, nonzero; uint64_t lastBytes, lastMicros; } gOut {};
static constexpr uint64_t kCopyOutMaxBytes = 4ull << 20;
static constexpr uint64_t kCopyOutLog = 96;

static bool residency_copy_from_vram(void *self, void *mapA, void *mapB) {
    if (!gResOrigVt || !gResCopyVt || reinterpret_cast<uintptr_t>(self) < kKernelHalfBase ||
        reinterpret_cast<uintptr_t>(mapA) < kKernelHalfBase || reinterpret_cast<uintptr_t>(mapB) < kKernelHalfBase ||
        *reinterpret_cast<void ***>(self) != gResCopyVt) { gOut.refused++; return false; }
    const char *r = static_cast<const char *>(self);
    const uint64_t bytes         = *reinterpret_cast<const uint64_t *>(r + 0x230);
    const uint64_t backingOffset = *reinterpret_cast<const uint64_t *>(r + 0xf8);
    const unsigned type          = *reinterpret_cast<const uint8_t *>(r + 0x14);
    const unsigned nibble        = (*reinterpret_cast<const uint32_t *>(r + 0x1b4) >> 26) & 0xfu;
    void *memA = *reinterpret_cast<void *const *>(static_cast<const char *>(mapA) + 0x18);
    void *memB = *reinterpret_cast<void *const *>(static_cast<const char *>(mapB) + 0x18);
    if (reinterpret_cast<uintptr_t>(memA) < kKernelHalfBase || reinterpret_cast<uintptr_t>(memB) < kKernelHalfBase) {
        gOut.refused++; return false;
    }
    const char *nA = static_cast<OSObject *>(memA)->getMetaClass() ? static_cast<OSObject *>(memA)->getMetaClass()->getClassName() : nullptr;
    const char *nB = static_cast<OSObject *>(memB)->getMetaClass() ? static_cast<OSObject *>(memB)->getMetaClass()->getClassName() : nullptr;
    auto isVid = [](const char *n) { return n && !strcmp(n, "AMDRadeonX6000_AMDAccelVidMemory"); };
    auto isSys = [](const char *n) { return n && (!strcmp(n, "AMDRadeonX6000_AMDAccelSysMemory") || !strcmp(n, "IOAccelSysMemory")); };
    void *vidMem = nullptr, *sysMem = nullptr;
    if (isVid(nA) && isSys(nB)) { vidMem = memA; sysMem = memB; }
    else if (isVid(nB) && isSys(nA)) { vidMem = memB; sysMem = memA; }
    if (!vidMem) {
        gOut.refused++;
        if (gOut.logged < 32) { gOut.logged++;
            PEERLOG("page-out: REFUSED resource=%p type=%#x nibble=%u bytes=%#llx: map classes %s / %s (handled: one "
                    "AMDAccelVidMemory and one IOAccelSysMemory) - keeps the skip", self, type, nibble,
                    (unsigned long long)bytes, nA ? nA : "?", nB ? nB : "?"); }
        return false;
    }
    const uint64_t vidLen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(vidMem) + 0x40);
    const uint64_t sysLen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(sysMem) + 0x40);
    if (bytes == 0 || bytes > vidLen || backingOffset > sysLen || bytes > sysLen - backingOffset) {
        gOut.refused++;
        if (gOut.logged < 32) { gOut.logged++;
            PEERLOG("page-out: REFUSED resource=%p bytes %#llx backingOffset %#llx vid length %#llx sys length %#llx", self,
                    (unsigned long long)bytes, (unsigned long long)backingOffset, (unsigned long long)vidLen,
                    (unsigned long long)sysLen); }
        return false;
    }
    if (bytes > kCopyOutMaxBytes) {
        gOut.tooBig++;
        if (gOut.logged < 32) { gOut.logged++;
            PEERLOG("page-out: resource=%p type=%#x bytes %#llx is over the %#llx page-out cap - keeps the skip", self, type,
                    (unsigned long long)bytes, (unsigned long long)kCopyOutMaxBytes); }
        return false;
    }
    uintptr_t seen = 0;
    const PhysSegmentFn vSeg = vid_segment_fn(vidMem, &seen);
    uint64_t prep = 0;
    IOMemoryDescriptor *md = backing_descriptor(sysMem, &prep);
    if (!vSeg || !md || prep == kIOPreparationIDUnprepared || (uint64_t)md->getLength() < backingOffset + bytes) {
        gOut.refused++;
        if (gOut.logged < 32) { gOut.logged++;
            PEERLOG("page-out: REFUSED resource=%p: segment accessor %p, backing %p (%s), preparationID %#llx, length %#llx need %#llx",
                    self, (void *)seen, md, md ? md->getMetaClass()->getClassName() : "not an IOMemoryDescriptor",
                    (unsigned long long)prep, (unsigned long long)(md ? md->getLength() : 0), (unsigned long long)(backingOffset + bytes)); }
        return false;
    }
    // Pre-flight: every source segment inside VRAM.
    for (uint64_t pos = 0; pos < bytes; ) {
        uint64_t span = 0;
        const uint64_t at = vSeg(vidMem, pos, &span);
        if (span == 0 || (at & 3)) { gOut.refused++; return false; }
        pos += span < bytes - pos ? span : bytes - pos;
    }
    uint64_t t0 = 0; clock_get_uptime(&t0);
    uint32_t buf[64], back[64], firstDw[8] = { 0 };
    uint64_t nonzero = 0, mismatched = 0, firstVram = 0;
    for (uint64_t pos = 0; pos < bytes; ) {
        uint64_t span = 0;
        const uint64_t at = vSeg(vidMem, pos, &span);
        if (pos == 0) firstVram = at;
        uint64_t n = bytes - pos;
        if (span < n) n = span;
        for (uint64_t k = 0; k < n; ) {
            const uint64_t left = n - k;
            const uint32_t cnt = (uint32_t)((left >= 256u ? 256u : left) + 3u) / 4u;
            const uint64_t take = (left >= 256u) ? 256u : left;
            if (!navi48_vram_read_mm(at + k, buf, cnt)) {
                gOut.failed++;
                PEERLOG("page-out: FAILED - MM-window read at VRAM %#llx (resource offset %#llx of %#llx); %#llx byte(s) of the "
                        "backing already written", (unsigned long long)(at + k), (unsigned long long)(pos + k),
                        (unsigned long long)bytes, (unsigned long long)(pos + k));
                return false;
            }
            if (md->writeBytes(backingOffset + pos + k, buf, take) != take) {
                gOut.failed++;
                PEERLOG("page-out: FAILED - writeBytes at backing %#llx returned short", (unsigned long long)(backingOffset + pos + k));
                return false;
            }
            for (uint32_t i = 0; i < cnt; i++) if (buf[i]) nonzero++;
            if (pos == 0 && k == 0) for (unsigned i = 0; i < 8 && i < cnt; i++) firstDw[i] = buf[i];
            if ((pos == 0 && k == 0) || pos + k + take >= bytes) {
                if (md->readBytes(backingOffset + pos + k, back, take) == take)
                    for (uint32_t i = 0; i < (uint32_t)(take / 4u); i++) if (back[i] != buf[i]) mismatched++;
            }
            k += take;
        }
        pos += n;
    }
    uint64_t t1 = 0; clock_get_uptime(&t1);
    uint64_t ns = 0; absolutetime_to_nanoseconds(t1 - t0, &ns);
    gOut.copies++; gOut.bytes += bytes; gOut.mismatched += mismatched; gOut.nonzero += nonzero ? 1u : 0u;
    // 0.0.285: WHO asked (the calling thread's process) and WHERE the backing lives (its first physical segment).
    char who[20] = { 0 };
    proc_selfname(who, (int)sizeof(who));
    const int whoPid = proc_selfpid();
    IOByteCount seg0Len = 0;
    const addr64_t seg0 = md->getPhysicalSegment(backingOffset, &seg0Len, kIOMemoryMapperNone);
    gOut.lastBytes = bytes; gOut.lastMicros = ns / 1000u;
    if (gOut.logged < kCopyOutLog) {
        gOut.logged++;
        PEERLOG("page-out: COPIED #%llu for pid %d (%s) resource=%p type=%#x nibble=%u bytes=%#llx VRAM %#llx -> backing %p +%#llx (%s, "
                "physical %#llx len %#llx) in %llu us; "
                "%llu nonzero dword(s), read-back mismatches %llu; first %08x %08x %08x %08x %08x %08x %08x %08x",
                (unsigned long long)gOut.copies, whoPid, who, self, type, nibble, (unsigned long long)bytes, (unsigned long long)firstVram,
                md, (unsigned long long)backingOffset, md->getMetaClass()->getClassName(), (unsigned long long)seg0,
                (unsigned long long)seg0Len, (unsigned long long)(ns / 1000u),
                (unsigned long long)nonzero, (unsigned long long)mismatched, firstDw[0], firstDw[1], firstDw[2], firstDw[3],
                firstDw[4], firstDw[5], firstDw[6], firstDw[7]);
    }
    return true;
}

uint32_t navi48_pagecopy_control(bool arm, uint64_t *out, unsigned count) {
    return navi48_pagecopy_control2(arm ? 1u : 0u, out, count);
}

// 0.0.284: mode 1 arms the page-in copy (as before); mode 2 arms it AND the page-out copy. out[] as before, with
// out[12] = skipped copies | page-out copies << 16 | page-out refusals << 32 | page-out failures << 48 (each field 16 bits).
uint32_t navi48_pagecopy_control2(uint32_t mode, uint64_t *out, unsigned count) {
    const bool arm = mode == 1u || mode == 2u;
    if (mode == 2u && gSkipPageCopy && !gCopyOutArmed) {
        gCopyOutArmed = true;
        PEERLOG("page-out: page-out copy ARMED for this boot - pageTexture(toVram = 0) of AMDAccelVidMemory -> IOAccelSysMemory "
                "now reads VRAM through the MM window into the backing descriptor (at most %#llx bytes per resource)",
                (unsigned long long)kCopyOutMaxBytes);
    }
    if (arm) {
        if (!gSkipPageCopy)
            PEERLOG("pagecopy: arm REFUSED - navi48-skip-pagecopy=1 is not active (or its hook was "
                    "refused), so there is no pageTexture hook to arm");
        else if (!gCopyArmed) {
            gCopyArmed = true;
            PEERLOG("pagecopy: residency copy ARMED for this boot - pageTexture(toVram) of sysmem -> "
                    "AMDAccelVidMemory now copies through the MM window and reads every dword back; "
                    "every other shape, and page-out, keeps the skip");
        }
    }
    const uint64_t state = (gCopyArmed ? 1u : 0u) | (gSkipPageCopy ? 2u : 0u) | (gResCopyVt ? 4u : 0u) | (gCopyOutArmed ? 8u : 0u);
    if (gCopyOutArmed)
        PEERLOG("page-out: counters - copies %llu (%llu bytes, %llu with nonzero content), refused %llu, over the cap %llu, failed %llu, "
                "read-back mismatches %llu, last %#llx bytes in %llu us", (unsigned long long)gOut.copies, (unsigned long long)gOut.bytes,
                (unsigned long long)gOut.nonzero, (unsigned long long)gOut.refused, (unsigned long long)gOut.tooBig,
                (unsigned long long)gOut.failed, (unsigned long long)gOut.mismatched, (unsigned long long)gOut.lastBytes,
                (unsigned long long)gOut.lastMicros);
    const uint64_t v[13] = { state, gCopy.copies, gCopy.bytes, gCopy.compared, gCopy.mismatched,
                             gCopy.unhandled, gCopy.unhandledMask, gCopy.pageOuts, gCopy.lastDst,
                             gCopy.lastBytes, gCopy.lastMicros, gCopy.failed,
                             ((uint64_t)gSkippedCopies & 0xffffu) | ((gOut.copies & 0xffffu) << 16) |
                                 (((gOut.refused + gOut.tooBig) & 0xffffu) << 32) | ((gOut.failed & 0xffffu) << 48) };
    if (out)
        for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return (uint32_t)state;
}

// 0.0.204 — `kernsub` (action 46). With arm, sets the substitution flag so every
// residency copy of the shader resource replaces Apple's Navi21 kernel with the
// gfx1201 twin, AND, if a copy has already run this boot (the usual order:
// copyarm, blit2start, then kernsub), performs the substitution immediately at
// the copy's recorded destination. Returns the substitution status.
uint32_t navi48_kernelsub_control(uint32_t mode, uint64_t *out, unsigned count) {
    uint32_t status = 0, reason = 0;
    uint64_t at = 0;
    const bool arm = (mode >= 1 && mode <= kSubModes);
    if (arm && gSubMode && gSubMode != mode) {
        // One kernel per boot: a second mode would be refused by the guard anyway
        // (our first kernel is no longer Apple's), but say so by name.
        gSub.lastReason = 5;
        PEERLOG("kernsub: mode %u REFUSED - mode %u (%s) is already armed this boot; one "
                "kernel mode per boot, nothing written", mode, gSubMode, kSubKernels[gSubMode].name);
    } else if (arm) {
        gSubMode = mode;
        if (!gResCopyVt) {
            PEERLOG("kernsub: arm REFUSED - no residency-copy hook is active "
                    "(navi48-skip-pagecopy=1 and fire needed); cannot substitute");
        } else if (gCopyArmed && gCopy.copies > 0 && gCopy.lastDst) {
            at = gCopy.lastDst + kBlitShaderOffset;
            // 2 (an internal design note): this is the STANDALONE substitute_blit_kernel_at call
            // (residency_copy_to_vram's own call, above, is already covered by that function's Navi48CopyScope) -
            // the design asks for it to have its own scope so its navi48_vram_write_mm is never counted unscoped.
            {
                Navi48CopyScope cgScope(at, at + (uint64_t)sub_kernel_bytes(mode));
                status = substitute_blit_kernel_at(at, mode, &reason);
                if (status == 0 && reason == 4) cgScope.markFailed();   // write/read-back itself failed: a partial write
                else if (status == 3) cgScope.markMismatch();           // written, but the read-back did not stick
            }
            gSub.lastReason = reason;
            if (status == 0)
                PEERLOG("kernsub: direct substitution of mode %u %s at VRAM %#llx REFUSED, reason %u "
                        "(1 guard/aperture, 2 read, 3 not Apple's kernel/already changed, 4 write/"
                        "read-back) - still armed for the next residency copy", mode,
                        kSubKernels[mode].name, (unsigned long long)at, reason);
        } else {
            PEERLOG("kernsub: ARMED mode %u %s for this boot - the next residency copy of the shader "
                    "resource will replace Apple's Navi21 kernel with it (no copy has run yet)",
                    mode, kSubKernels[mode].name);
        }
    }
    const uint32_t nb = sub_kernel_bytes(gSubMode ? gSubMode : mode);
    const uint64_t v[10] = { (uint64_t)status, at, (uint64_t)nb,
                             gSub.mismatched, gSub.subs, (uint64_t)gSub.lastReason,
                             (gSubMode ? 1u : 0u), gCopy.lastDst, (uint64_t)gSubMode,
                             (uint64_t)(nb / 4u) };
    if (out)
        for (unsigned i = 0; i < count && i < 10; i++) out[i] = v[i];
    return status;
}

// The replacement pageTexture. Unarmed: claim the resource is resident, copy
// nothing. Armed (0.0.201): copy sysmem -> VRAM for the handled shape first.
static uint32_t gPageDrawableLog { 0 };
// build 0.0.531 item 4: noinline, so the timed entry below (the one installed) adds its own small frame on top of this one.
static __attribute__((noinline)) uint8_t hook_page_texture(void *self, uint8_t toVram, void *dst, void *src);
static uint8_t hook_page_texture(void *self, uint8_t toVram,
                                 void *dst, void *src) {
    if (gInspectedCopies < 8 && toVram) { gInspectedCopies++; inspect_pagecopy(self, dst, src); }
    // 0.0.285: every pageTexture on a resource of the clock drawables' shape (type 0xc0, nibble 8: probe2's page-outs),
    // in both directions, with the calling process - the sequence says who renders into and who reads those surfaces.
    if (gCopyOutArmed && gPageDrawableLog < 160u && reinterpret_cast<uintptr_t>(self) >= kKernelHalfBase) {
        const char *r = static_cast<const char *>(self);
        if (*reinterpret_cast<const uint8_t *>(r + 0x14) == 0xc0u && ((*reinterpret_cast<const uint32_t *>(r + 0x1b4) >> 26) & 0xfu) == 8u) {
            gPageDrawableLog++;
            char who[20] = { 0 };
            proc_selfname(who, (int)sizeof(who));
            uint64_t now = 0; clock_get_uptime(&now);
            PEERLOG("page-drawable: #%u %s resource=%p bytes=%#llx from pid %d (%s) at uptime %llu", gPageDrawableLog,
                    toVram ? "PAGE-IN (sysmem -> VRAM)" : "PAGE-OUT (VRAM -> sysmem)", self,
                    (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0x230), proc_selfpid(), who, (unsigned long long)now);
        }
    }
    if (!toVram) {
        ++gCopy.pageOuts;
        if (gN48D84Live) navi48_d84_pageout(self);   // build 0.0.529 (CG84.md item 7): a page-out invalidates the resource's key
        // PAGE-OUT SYMMETRY: once a resource's OWN gfx10 mode was re-tiled this boot, its page-out keeps the skip, so
        // gfx12-ordered bytes never reach system memory for a later page-in to re-tile a second time. build 0.0.450
        // item 2 (reviewer fix): PER MODE, so a 256B_D resource is never refused unless a 256B_D re-tile actually
        // happened (switch 46 only) - gRpEverRetiled4kbdx/256bd are each 0 unless their own kind was ever retiled.
        // build 0.0.451 item 3 (S3): keyed on rp_pageout_swz_dword (mask-aware, the SAME dword the shape check
        // uses), not the raw res+0x1dc read 0.0.450 had - see rp_pageout_swz_dword's own banner.
        // build 0.0.492: + mode 27 (gRpEverRetiled64krx, 0 unless switch 59 wrote a mode-27 resource).
        // build 0.0.493: + the known-asset resources (gRpKnownAny, 0 unless switch 59 converted one).
        if (gCopyOutArmed && (gRpEverRetiled4kbdx || gRpEverRetiled256bd || gRpEverRetiled64krx || gRpKnownAny) &&
            reinterpret_cast<uintptr_t>(self) >= kKernelHalfBase &&
            rp_pageout_refused(static_cast<const char *>(self))) {
            if ((gRt.pageoutRefused++ % 64u) == 0u)
                PEERLOG("resprov: page-out #%llu of a re-tiled-mode resource=%p REFUSED (keeps the skip): this boot re-tiled "
                        "that mode on page-in, and a page-out would put gfx12-ordered bytes into system memory (refused "
                        "%llu so far)", (unsigned long long)gCopy.pageOuts, self, (unsigned long long)gRt.pageoutRefused);
        } else
        // 0.0.284: armed, copy it; any refusal keeps the 0.0.200 skip below.
        if (gCopyOutArmed && residency_copy_from_vram(self, dst, src)) return 1;
        // Page-out (VRAM -> sysmem) otherwise stays skipped; logged, never copied.
        if ((gCopy.pageOuts % 64) == 1)
            PEERLOG("residency-copy: page-out #%llu (VRAM -> sysmem) resource=%p SKIPPED - only "
                    "sysmem -> VRAM is handled", (unsigned long long)gCopy.pageOuts, self);
    } else if (gCopyArmed && residency_copy_to_vram(self, dst, src)) {
        return 1;
    }
    if ((++gSkippedCopies % 64) == 1)
        PEERLOG("pageTexture SKIPPED (#%u): reporting success without copying. The "
                "blit's data will be wrong; this only tests whether anything "
                "downstream of residency works.", gSkippedCopies);
    return 1;
}

// build 0.0.524 item 7 (an internal design note): hook_new_resource's "second, different resource class" path was
// SILENT, and a resource taking it keeps Apple's own pageTexture (no residency copy of ours ever runs for it). Now it is COUNTED, and
// the first one per boot is NAMED (getMetaClass()->getClassName(), behind kernel-pointer checks on the object, its vtable, the
// metaclass and the name). READ-ONLY and unswitched: it patches nothing, changes no return value and writes nothing of Apple's.
// build 0.0.531 item 4 ( item 4; log-only): the installed pageTexture entry - times hook_page_texture (entry to return,
// every call, both directions) into AppleHardwareHook.cpp's page-in histogram and changes nothing else.
static uint8_t hook_page_texture_timed(void *self, uint8_t toVram, void *dst, void *src) {
    uint64_t t0 = 0ull;
    clock_get_uptime(&t0);
    const uint8_t rv = hook_page_texture(self, toVram, dst, src);
    n48::hw_wt_pagein(t0);
    return rv;
}
static uint64_t gRes2Count { 0ull };
static uint32_t gRes2Logged { 0u };
static __attribute__((noinline)) void res2_note(void *res, void **vt) {
    const uint64_t n = __atomic_add_fetch(&gRes2Count, 1ull, __ATOMIC_RELAXED);
    uint32_t zero = 0u;
    if (!__atomic_compare_exchange_n(&gRes2Logged, &zero, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    const char *cls = "(not a kernel object)";
    if (reinterpret_cast<uintptr_t>(res) >= kKernelHalfBase && reinterpret_cast<uintptr_t>(vt) >= kKernelHalfBase) {
        const OSMetaClass *mc = static_cast<const OSObject *>(res)->getMetaClass();
        const char *nm = (mc && reinterpret_cast<uintptr_t>(mc) >= kKernelHalfBase) ? mc->getClassName() : nullptr;
        cls = (nm && reinterpret_cast<uintptr_t>(nm) >= kKernelHalfBase) ? nm : "(no class)";
    }
    PEERLOG("newResource: a SECOND resource class '%.64s' (vtable %p; the patched class's %p) keeps Apple's own pageTexture - "
            "counted, never patched (named once per boot; #%llu so far)", cls, (void *)vt, (void *)gResOrigVt, (unsigned long long)n);
}
uint64_t navi48_peer_res2_count(void) { return __atomic_load_n(&gRes2Count, __ATOMIC_RELAXED); }
// build 0.0.535 item 3: the `COPIED #` counter, read-only (switch 89's per-chunk NOT TAKEN line names a copy by it).
uint64_t navi48_peer_copy_seq(void) { return __atomic_load_n(&gCopy.copies, __ATOMIC_RELAXED); }

static void *hook_new_resource(void *accel) {
    void *res = gOrigNewResource ? gOrigNewResource(accel) : nullptr;
    if (!res || !gSkipPageCopy) return res;

    void ***slot = reinterpret_cast<void ***>(res);
    void  **vt   = *slot;
    if (!vt) return res;

    if (vt == gResCopyVt) return res;            // already ours
    if (gResOrigVt && vt == gResOrigVt) { *slot = gResCopyVt; return res; }
    if (gResOrigVt) {
        // A second, different resource class. Leave it alone rather than guess.
        res2_note(res, vt);   // build 0.0.524 item 7: counted, and its class named once per boot (read-only)
        return res;
    }

    const uintptr_t pt = reinterpret_cast<uintptr_t>(vt[kResPageTextureSlot]);
    if (pt < kKernelHalfBase || (pt & 0xFFF) != kResPageTexturePageOff) {
        PEERLOG("newResource: slot %u is not pageTexture (%p, page %#lx, expected %#lx) "
                "— NOT patching", kResPageTextureSlot, (void *)pt,
                (unsigned long)(pt & 0xFFF), (unsigned long)kResPageTexturePageOff);
        gSkipPageCopy = false;                   // disarm rather than risk it
        return res;
    }

    const size_t bytes = (kResVtableSlots + kVtHeader) * sizeof(void *);
    void **copy = static_cast<void **>(IOMalloc(bytes));
    if (!copy) { PEERLOG("newResource: IOMalloc(%lu) failed", (unsigned long)bytes); return res; }
    memcpy(copy, vt - kVtHeader, bytes);
    copy[kVtHeader + kResPageTextureSlot] = reinterpret_cast<void *>(&hook_page_texture_timed);   // 0.0.531: the timed entry
    gResOrigVt = vt;
    gResCopyVt = copy + kVtHeader;
    __asm__ __volatile__("sfence" ::: "memory");
    *slot = gResCopyVt;
    PEERLOG("newResource: resource vtable patched — pageTexture (slot %u) was %p, now ours",
            kResPageTextureSlot, (void *)pt);
    return res;
}

// build 0.0.503 (HYBRID.md H1): puts the slot-239 hook into the accelerator vtable COPY before that copy is published
// (defined with the hook, below). Guards: the two slide anchors and slot 239 == IOGraphicsAccelerator2::newUserClient.
static void ucp_install_at_start(IOService *accel, void **vt, void **copy);

bool Navi48AccelPeer::tryPatchAcceleratorStop() {
    if (mStopPatched) return true;

    IOService *pci = getProvider();
    if (!pci) { PEERLOG("stop-patch: no provider"); return false; }

    // Finding the accelerator.
    //
    // It is NOT a client of the PCI nub — measured, not assumed: the first
    // version of this looked there and logged "not among the nub's clients yet"
    // on every run. It attaches to AMDRadeonHWServicesNavi, which is what
    // publishes LoadAccelerator and therefore what it matches against.
    //
    // So walk the service plane by class instead. serviceMatching() is not used
    // because it only finds REGISTERED services, and at this moment the
    // accelerator is still inside its own start() and has not registered.
    // metaCast, not a class-name comparison. Apple instantiates a per-ASIC
    // SUBCLASS of AMDGraphicsAccelerator, so an exact name match finds nothing —
    // which is precisely what the first attempt logged, repeatedly. metaCast
    // accepts the subclass, and the vtable geometry check below still proves the
    // layout is the one that was decoded before anything is written.
    IOService *accel = nullptr;
    char seen[160]; seen[0] = '\0';
    if (IORegistryIterator *ri = IORegistryIterator::iterateOver(
            gIOServicePlane, kIORegistryIterateRecursively)) {
        while (IORegistryEntry *e = ri->getNextObject()) {
            if (e->metaCast("AMDRadeonX6000_AMDGraphicsAccelerator")) {
                accel = OSDynamicCast(IOService, e);
                if (accel) break;
            }
            // Keep a few nearby names so a miss is diagnosable from one run.
            const char *cls = e->getMetaClass()->getClassName();
            if (cls && strnstr((char *)cls, "Accel", 64) && strlen(seen) + strlen(cls) + 2 < sizeof(seen)) {
                strlcat(seen, cls, sizeof(seen));
                strlcat(seen, " ", sizeof(seen));
            }
        }
        ri->release();
    }
    if (!accel) {
        PEERLOG("stop-patch: no AMDGraphicsAccelerator subclass in the service plane yet "
                "(Accel-ish classes present: %s)", seen[0] ? seen : "none");
        return false;
    }
    PEERLOG("stop-patch: found accelerator %p, class %s", accel,
            accel->getMetaClass()->getClassName());
    (void)pci;

    void ***slot = reinterpret_cast<void ***>(accel);
    void  **vt   = *slot;
    if (!vt) { PEERLOG("stop-patch: accelerator has no vtable?"); return false; }

    // Same verification discipline as every other hook here: prove the object's
    // vtable is the layout we decoded before writing to anything.
    const uintptr_t a = reinterpret_cast<uintptr_t>(vt[kAccelStopSlot]);
    const uintptr_t b = reinterpret_cast<uintptr_t>(vt[kAccelAnchorSlot]);
    if (a < kKernelHalfBase || b < kKernelHalfBase) {
        PEERLOG("stop-patch: slots are not kernel addresses (%p / %p) — REFUSING",
                (void *)a, (void *)b);
        return false;
    }
    if ((long)(a - b) != kAccelStopDelta || (a & 0xFFF) != kAccelStopPageOff) {
        PEERLOG("stop-patch: geometry mismatch (delta %#lx expected %#lx, page %#lx "
                "expected %#lx) — REFUSING", (long)(a - b), kAccelStopDelta,
                (unsigned long)(a & 0xFFF), (unsigned long)kAccelStopPageOff);
        return false;
    }

    const size_t bytes = (kAccelVtableSlots + kVtHeader) * sizeof(void *);
    void **copy = static_cast<void **>(IOMalloc(bytes));
    if (!copy) { PEERLOG("stop-patch: IOMalloc(%lu) failed", (unsigned long)bytes); return false; }
    memcpy(copy, vt - kVtHeader, bytes);
    copy[kVtHeader + kAccelStopSlot] = reinterpret_cast<void *>(&hook_accel_stop);

    // Opt-in only: the copy-skip instrument is armed by boot-arg, so a normal
    // accel-experiment boot behaves exactly as before.
    uint32_t skip = 0;
    gSkipPageCopy = PE_parse_boot_argn("navi48-skip-pagecopy", &skip, sizeof(skip)) && skip;
    if (gSkipPageCopy) {
        const uintptr_t nr = reinterpret_cast<uintptr_t>(vt[kAccelNewResourceSlot]);
        if (nr >= kKernelHalfBase) {
            gOrigNewResource = reinterpret_cast<NewResourceFn>(vt[kAccelNewResourceSlot]);
            copy[kVtHeader + kAccelNewResourceSlot] = reinterpret_cast<void *>(&hook_new_resource);
            PEERLOG("newResource (slot %u) now ours — navi48-skip-pagecopy=1 is set, so "
                    "resource paging copies will be SKIPPED to test the rest of the path",
                    kAccelNewResourceSlot);
        } else {
            PEERLOG("newResource slot %u is not a kernel address (%p) — NOT patching",
                    kAccelNewResourceSlot, (void *)nr);
            gSkipPageCopy = false;
        }
    }
    // build 0.0.503 (an internal design note.2,): the newUserClient hook (slot 239) goes into THIS copy, before
    // the copy is published below and while the accelerator is still inside its own start() (it has not registered, so no
    // client can have matched it yet). Switch 68 OFF (the default) it only reads the caller and calls Apple.
    ucp_install_at_start(accel, vt, copy);
    gAccelVt = copy;
    gAccelObj = accel;
    __asm__ __volatile__("sfence" ::: "memory");
    *slot = copy + kVtHeader;

    mStopPatched = true;
    PEERLOG("stop-patch: AMDGraphicsAccelerator at %p — stop() (slot %u) now ours "
            "(was %p); a failing start() will return cleanly instead of panicking",
            accel, kAccelStopSlot, (void *)a);
    return true;
}

// ---------------------------------------------------------------------------
// Marking the accelerator "enabled"
// ---------------------------------------------------------------------------
//
// Every resource allocation Metal makes goes through
//
//   IOAccelSharedUserClient2::new_resource
//     IOGraphicsAccelerator2::acceleratorWaitEnabled()
//       loop { if (this->0xc78 & 2) break; waiting_for_fEnabled(lock, this); }
//
// and `waiting_for_fEnabled` is IOLockSleep(accel->0x88, accel, THREAD_UNINT) —
// uninterruptible, no timeout. Which is why the probe process could not even be
// killed.
//
// Bit 1 of 0xc78 is set by exactly one function, IOGraphicsAccelerator2::
// enableAccelerator(), and that has exactly two callers in the entire kext:
//
//   IOAccelDisplayMachine::display_mode_did_change(unsigned int)
//   IOAccelLegacyDisplayMachine::display_mode_did_change(unsigned int)
//
// **Apple's accelerator enables itself when a DISPLAY reports a mode change.**
// Nothing reports one here: RDNA4FB owns scanout and is not an Apple AMD
// framebuffer, and this peer deliberately drives no display. So fEnabled is
// never set and Metal blocks forever on its first allocation.
//
// The real path does, from display_mode_did_change:
//
//   accel->enableAccelerator();                     ; orb $0x2, 0xc78
//   IOLockWakeup(accel->0x88, /*event=*/accel, 0);
//
// which is reproduced here. enableAccelerator() also starts a hardware-progress
// timer when !(0xc92 & 8); that is skipped, and skipping it is if anything the
// safer half — it is the GPU-hang watchdog and we are not submitting work yet.
//
// THIS IS AN INSTRUMENT, like the stop() suppression. It tells the accelerator a
// display came up when none did, to find out what the next wall is. It must not
// survive into anything resembling a real driver; the honest fix is to give the
// accelerator a display machine that genuinely reports a mode.
static constexpr uintptr_t kAccelFlagsOff  = 0xc78;   // u32; bit 1 = fEnabled
static constexpr uintptr_t kAccelLockOff   = 0x88;    // IOLock*
static constexpr uint32_t  kAccelFEnabled  = 0x2;

static uint32_t gForcedEnables { 0 };

static void force_accelerator_enabled(void *accel) {
    if (!accel) return;
    auto *flags = reinterpret_cast<volatile uint32_t *>(
        reinterpret_cast<uint8_t *>(accel) + kAccelFlagsOff);
    IOLock *lock = *reinterpret_cast<IOLock **>(
        reinterpret_cast<uint8_t *>(accel) + kAccelLockOff);

    const uint32_t before = *flags;
    if (before & kAccelFEnabled) {
        PEERLOG("force-enable: accelerator already reports fEnabled (0xc78=%#x)", before);
        return;
    }
    *flags = before | kAccelFEnabled;
    __asm__ __volatile__("sfence" ::: "memory");
    gForcedEnables++;
    if (lock) {
        IOLockWakeup(lock, accel, /*oneThread=*/false);
        PEERLOG("force-enable #%u: 0xc78 %#x -> %#x, woke sleepers on lock %p "
                "(NO display reported a mode — this is an instrument)",
                gForcedEnables, before, *flags, lock);
    } else {
        PEERLOG("force-enable #%u: 0xc78 %#x -> %#x, but accel->0x88 is NULL so "
                "nothing could be woken", gForcedEnables, before, *flags);
    }
}

// Replicating the safe tail of AMDGraphicsAccelerator::powerUpHW()
// -----------------------------------------------------------------
// powerUpHW() is the one routine that arms Apple's memory system, and it is
// never called on this path. It cannot simply be called either: its third step
// is `hw->vtbl[0x208]()` — Navi21Hardware::powerUp — which descends to
// GFX10Hardware::initializeVmHardware and page-faults at CR2 = 0 on the register
// block our allocRegisters hook withholds, because those GFX10 memory-controller
// offsets are wrong on gfx12. Build 0.0.74 panicked exactly there, at
// powerUpHW+0x252, which is the instruction after that call.
//
// Everything AFTER that call is software, and that is what we replicate, in
// powerUpHW's own order:
//
//     hw->vtbl[0x208]()   powerUp                      <- SKIPPED (panics)
//     hw->vtbl[0x250]()   gate
//     memmgr->enableAllocations()                      hw_hook_enable_vram_allocations
//     reserveNDRVSpace()                               here
//     hw->vtbl[0x270]()   setMemoryAllocationsEnabled  hw_hook_enable_memory_allocations
//     createBltMgr()                                   here
//     ... 0x568 / 0x570 / 0x578, reduceVRAM()
//
// These two are NOT virtual, so there is no vtable slot to verify. The safety is
// the slide, which is derived from a function pointer Apple handed us in TTL
// initialize() rather than guessed, plus a kernel-range check.
static constexpr uint64_t kStaticReserveNDRVSpace = 0xbdc1810ULL;
static constexpr uint64_t kStaticCreateBltMgr     = 0xbdc19bcULL;

static uint8_t call_accel_fn(const char *what, uint64_t staticAddr, void *accel) {
    if (!accel) { PEERLOG("%s: no accelerator", what); return 0; }
    const uint64_t slide = gNavi48Ttl.x6000Slide();
    if (!slide) {
        PEERLOG("%s: AMDRadeonX6000 slide not known yet — SKIPPING", what);
        return 0;
    }
    const uint64_t addr = staticAddr + slide;
    if (addr < kKernelHalfBase) {
        PEERLOG("%s: computed address %#llx is not in the kernel half — REFUSING",
                what, (unsigned long long)addr);
        return 0;
    }
    typedef uint8_t (*Fn)(void *);
    const uint8_t r = reinterpret_cast<Fn>(addr)(accel);
    PEERLOG("%s() at %p (0x%llx + slide %#llx) -> %u", what, (void *)addr,
            (unsigned long long)staticAddr, (unsigned long long)slide, (unsigned)r);
    return r;
}

// ---------------------------------------------------------------------------
// MILESTONE 3 (0.0.241) — the display-pairing keys
// ---------------------------------------------------------------------------
//
// IOKit's IOAccelFindAccelerator(framebuffer, &accel, &index) is what pairs a
// display's framebuffer with an accelerator, and run r40 measured it FAILING
// here with 0xe00002bc while both nodes sat on the same PCI function.
//
// That is not a policy refusal. CONFIRMED by disassembling
// _IOAccelFindAccelerator @ IOKit 0x7ff806c8c4ee (re/m3b/IOKit, extracted from
// the x86_64 dyld cache; nm -n names the symbol at exactly that address):
//
//   7ff806c8c53a  callq _IORegistryEntryCreateCFProperties  ; the FRAMEBUFFER's props
//   7ff806c8c571  leaq 0x39a31ae8(%rip),%rsi  ## 0x7ff8406be060 -> CFSTR("IOAccelTypes")
//   7ff806c8c57d  movl $0xe00002bc,%r15d      ; the value returned when that key is ABSENT
//   7ff806c8c586  je   0x7ff806c8c546         ; -> bail out with 0xe00002bc
//   7ff806c8c593  callq <CFStringGetCStringPtr>  ; the value is a C STRING
//   7ff806c8c5a6  callq _IORegistryEntryFromPath ; ... used as a REGISTRY PATH
//   7ff806c8c5b6  leaq 0x8f33d(%rip),%rsi     ## 0x7ff806d1b8fa = "IOAccelerator"
//   7ff806c8c5bf  callq _IOObjectConformsTo   ; the entry must BE an IOAccelerator
//   7ff806c8c5d0  leaq 0x39a31aa9(%rip),%rsi  ## 0x7ff8406be080 -> CFSTR("IOAccelIndex")
//   7ff806c8c5e4  movl $0x3,%esi              ; kCFNumberSInt32Type
//   7ff806c8c5f0  callq <CFNumberGetValue>    ; -> *pFramebufferIndex
//
// The two CFString constants were READ, not guessed: each __cfstring record's
// +0x10 word is its backing cstring pointer (chained fixup, target = raw &
// 0x3FFFFFFF, rebased onto the image), and both are 12 bytes long —
//   0x7ff8406be060 +0x10 -> 0x7ff806d1b8ed = "IOAccelTypes"
//   0x7ff8406be080 +0x10 -> 0x7ff806d1b908 = "IOAccelIndex"
// So r40's failure is a MISSING DICTIONARY KEY on the framebuffer, nothing more,
// and IOFBDependentID (which notes/MILESTONE3-DESIGN.md named) is not read here
// at all.
//
// Who normally writes those keys is NOT the framebuffer driver. CONFIRMED in
// IOAcceleratorFamily2: IOAccelDisplayPipe::init(IOGraphicsAccelerator2*,
// IOAccelDisplayMachine*, IOFramebuffer*, unsigned) @ 0x145cb2ac — the mangled
// symbol itself names the IOFramebuffer* argument, so the target is read rather
// than inferred — stamps all three onto that argument:
//
//   145cb2c7  movq %rcx,%r15       ; arg4 = the IOFramebuffer*
//   145cb328  movq %r15,%r12       ; ...which is what every setProperty targets
//   145cb4d5  callq *0x3d8(%rax)   ; a getPath-shaped call into a 0x200 buffer
//   145cb4e3  leaq ## 0x145e28c3 = "IOAccelTypes" ; %rdx = -0x230(%rbp), that buffer
//   145cb4f4  callq *0x270(%rax)   ; setProperty(key, const char *path)
//   145cb4fe  leaq ## 0x145e28d0 = "IOAccelIndex" ; %rdx = the pipe's index argument
//   145cb510  callq *0x280(%rax)   ; setProperty(key, value, 32 bits)
//   145cb51a  leaq ## 0x145e13bd = "IOAccelRevision" ; %edx = 2
//   145cb52e  callq *0x280(%rax)   ; setProperty(key, 2, 32 bits)
//
// Each of those three string addresses was read out of __TEXT,__cstring. The
// negative control is clean: Apple's own AMDRadeonX6000Framebuffer binary
// contains NONE of the three strings, while it does contain IOFBDependentID and
// IOFBDependentIndex — so these keys are never baked into a framebuffer driver;
// the accelerator's display pipe stamps them at pairing time. Our framebuffer
// has none because Apple's IOAccelDisplayMachine never claimed it.
//
// So this publishes what Apple's display pipe would have published.
// IOAccelRevision is included because Apple's code does set it, to 2, and the
// live accelerator nub already carries IOAccelRevision = 2 itself (r40).
//
// ORDERING, and it is the whole safety argument. Section 333 measured that on an
// armed boot every Metal client blocks in uninterruptible wait; a SUCCESSFUL
// pairing on such a boot is strictly WORSE than a failed one, because it would
// route the compositor onto an accelerator that cannot run its shaders. Doing
// this from kReqAccelStarted gives the right ordering by construction: Apple's
// accelerator cannot start, and so cannot announce itself here, until the
// exposure gate has published LoadAccelerator.
//
// 0.0.267: OPT-IN. found WindowServer consults this lookup only when
// a WindowServer process initialises its displays, and P1 measured that login and logout do
// not restart it. So the stamp is inert on a normal boot and dangerous exactly when
// WindowServer crashes or is killed on an armed boot: the new process would put its own
// compositing, login window included, on a GPU that cannot run it. Default OFF; stamped only
// with navi48-display-pairing=1 or `accel pairing 1` sent before `fire`.
bool Navi48AccelPeer::publishDisplayPairingKeys() {
    if (!gPairingLock) {
        PEERLOG("pairing: no pairing lock (IOLockAlloc failed at start) — REFUSING (nothing written)");
        setProperty("Navi48,DisplayPairing", "refused: no pairing lock");
        return false;
    }
    IOLockLock(gPairingLock);
    const bool ok = publishDisplayPairingKeysLocked();
    IOLockUnlock(gPairingLock);
    return ok;
}

bool Navi48AccelPeer::publishDisplayPairingKeysLocked() {
    if (mPairingPublished) return true;

    if (!gPairingDecided) {
        uint32_t bootArg = 0;
        const bool present = PE_parse_boot_argn("navi48-display-pairing", &bootArg, sizeof(bootArg));
        gPairingSource  = n48_pairing_source(present ? 1 : 0, bootArg, gPairingRequest);
        gPairingDecided = true;
        PEERLOG("pairing: decision taken at accelerator start — %s (boot-arg %s%u, verb request %u)",
                n48_pairing_source_name(gPairingSource), present ? "present, value " : "absent ",
                present ? bootArg : 0, gPairingRequest);
    }
    if (!n48_pairing_source_stamps(gPairingSource)) {
        PEERLOG("pairing: NOT stamping — %s. The framebuffer keeps no IOAccelTypes, so "
                "IOAccelFindAccelerator returns 0xe00002bc (the r40 control) and a WindowServer "
                "that restarts on this armed boot cannot pair the display with this accelerator.",
                n48_pairing_source_name(gPairingSource));
        setProperty("Navi48,DisplayPairing", gPairingSource == kPairingSrcDefaultOff
                    ? "off: default (0.0.267 opt-in; navi48-display-pairing=1 or `accel pairing 1` before fire)"
                    : "off: disabled by boot-arg or verb");
        return false;
    }

    IOService *pci = getProvider();
    if (!pci) {
        PEERLOG("pairing: no provider — REFUSING (nothing written)");
        setProperty("Navi48,DisplayPairing", "refused: no provider");
        return false;
    }

    // The framebuffer is a DIRECT child of our own PCI function in the IOService
    // plane: r40 measured RDNA4FB's parent[0] as the IOPCIDevice regID
    // 0x1000002d5 that the accelerator also hangs off. Bounding the search to
    // our own provider is what keeps this from ever stamping some other GPU's
    // framebuffer. metaCast, never a class-name compare (the lesson the
    // accelerator search below already paid for).
    IOService *fb = nullptr;
    if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IOService *svc = OSDynamicCast(IOService, o);
            if (svc && svc->metaCast("IOFramebuffer")) { fb = svc; break; }
        }
        it->release();
    }
    if (!fb) {
        PEERLOG("pairing: no IOFramebuffer under our PCI function — REFUSING (nothing written)");
        setProperty("Navi48,DisplayPairing", "refused: no framebuffer under our PCI device");
        return false;
    }

    // The accelerator nub. serviceMatching() would only find REGISTERED
    // services and Apple's accelerator is still inside its own start() at this
    // moment, so walk the plane by class exactly as tryPatchAcceleratorStop
    // does. The class asked for is the one the CONSUMER checks — IOAccelerator,
    // per IOObjectConformsTo at 0x7ff806c8c5bf — not the AMD subclass, because
    // conforming to IOAccelerator is the actual precondition of the lookup.
    // It must also be OUR accelerator: the parent walk bounds it to this PCI
    // function, so a second GPU's nub could never be published here.
    IOService *accel = nullptr;
    if (IORegistryIterator *ri = IORegistryIterator::iterateOver(
            gIOServicePlane, kIORegistryIterateRecursively)) {
        while (IORegistryEntry *e = ri->getNextObject()) {
            if (!e->metaCast("IOAccelerator")) continue;
            IOService *svc = OSDynamicCast(IOService, e);
            if (!svc) continue;
            IORegistryEntry *p = svc->getParentEntry(gIOServicePlane);
            for (int hop = 0; p && hop < 8; hop++) {
                if (p == pci) { accel = svc; break; }
                p = p->getParentEntry(gIOServicePlane);
            }
            if (accel) break;
        }
        ri->release();
    }
    if (!accel) {
        PEERLOG("pairing: no IOAccelerator under our PCI function — nothing written. That is "
                "EXPECTED and correct on a boot whose exposure gate stayed shut.");
        setProperty("Navi48,DisplayPairing", "deferred: no accelerator nub under our PCI device");
        return false;
    }

    // The VALUE is a path computed now, never a registry ID: the accelerator
    // nub's ID changes every boot (0x1000006b3 on disp1, 0x1000006a3 on r40),
    // which is exactly why Apple's own code calls getPath here too.
    char path[512];
    int  len = (int)sizeof(path);
    if (!accel->getPath(path, &len, gIOServicePlane)) {
        PEERLOG("pairing: getPath failed for accelerator %p — REFUSING (nothing written)", accel);
        setProperty("Navi48,DisplayPairing", "refused: getPath failed");
        return false;
    }

    fb->setProperty("IOAccelTypes",    path);
    fb->setProperty("IOAccelIndex",    (unsigned long long)0, 32);
    fb->setProperty("IOAccelRevision", (unsigned long long)2, 32);

    // Read the string back off the node we just wrote, so the log records what
    // the registry HOLDS and not merely what we passed (rule 41).
    const char *back = nullptr;
    if (OSString *s = OSDynamicCast(OSString, fb->getProperty("IOAccelTypes")))
        back = s->getCStringNoCopy();
    const bool ok = back && strncmp(back, path, sizeof(path)) == 0;

    mPairingPublished = true;
    // Remember exactly what we wrote, so `pairing 2` withdraws only OUR keys (identity check
    // on the path read back) and never a value some other writer put there since.
    if (mPairedFb) mPairedFb->release();
    fb->retain();
    mPairedFb = fb;
    strlcpy(mPairedPath, path, sizeof(mPairedPath));
    PEERLOG("pairing: STAMPED framebuffer %s (class %s) with IOAccelTypes=\"%s\", "
            "IOAccelIndex=0, IOAccelRevision=2 — the three properties "
            "IOAccelFindAccelerator reads (IOKit 0x7ff806c8c4ee). Accelerator %p class %s. "
            "Read-back %s. r40 measured this same lookup returning 0xe00002bc with "
            "IOAccelTypes absent.",
            fb->getName(), fb->getMetaClass()->getClassName(), path,
            accel, accel->getMetaClass()->getClassName(),
            ok ? "MATCHES" : (back ? "DIFFERS - the registry holds something else" : "MISSING"));
    setProperty("Navi48,DisplayPairing", ok
                ? "stamped: IOAccelTypes/IOAccelIndex/IOAccelRevision on the framebuffer, read back"
                : "stamped: read-back did NOT match");
    return ok;
}

// 0.0.267: remove the three keys we stamped, under the pairing lock. Returns 0
// when they are gone and read back absent, else a reason: 1 nothing of ours is stamped,
// 2 IOAccelTypes on the node no longer equals the path we wrote (not ours to remove —
// REFUSED, nothing removed), 3 a key is still present after removal. Registry edits only:
// no hardware, no Apple object. It cannot un-pair a WindowServer that already paired (:
// the choice is stored per display at WindowServer's display initialisation); it stops the
// NEXT WindowServer process from pairing.
uint32_t Navi48AccelPeer::withdrawDisplayPairingKeysLocked() {
    if (!mPairingPublished || !mPairedFb) return 1;
    OSString *cur = OSDynamicCast(OSString, mPairedFb->getProperty("IOAccelTypes"));
    if (!cur || strncmp(cur->getCStringNoCopy(), mPairedPath, sizeof(mPairedPath)) != 0) {
        PEERLOG("pairing: withdraw REFUSED — %s's IOAccelTypes is %s, not the path we stamped "
                "(\"%s\"); removing nothing", mPairedFb->getName(),
                cur ? "a DIFFERENT value" : "ABSENT", mPairedPath);
        return 2;
    }
    mPairedFb->removeProperty("IOAccelTypes");
    mPairedFb->removeProperty("IOAccelIndex");
    mPairedFb->removeProperty("IOAccelRevision");
    const bool gone = !mPairedFb->getProperty("IOAccelTypes") &&
                      !mPairedFb->getProperty("IOAccelIndex") &&
                      !mPairedFb->getProperty("IOAccelRevision");
    PEERLOG("pairing: WITHDRAWN from %s — IOAccelTypes/IOAccelIndex/IOAccelRevision removed; "
            "read-back %s. A WindowServer that already paired keeps its choice; the next "
            "one to initialise its displays will not find this accelerator.",
            mPairedFb->getName(), gone ? "ALL ABSENT" : "a key is STILL PRESENT");
    if (!gone) return 3;
    gPairingWithdrawn = true;
    mPairedFb->release();
    mPairedFb = nullptr;
    setProperty("Navi48,DisplayPairing", "withdrawn: keys removed by `accel pairing 2`, read back absent");
    return 0;
}

// 0.0.267, action 57 `pairing [1|2]`. See the declaration in Navi48Ttl.hpp.
uint32_t navi48_pairing_control(uint64_t arg, uint64_t *out, unsigned count) {
    if (gPairingLock) IOLockLock(gPairingLock);
    Navi48AccelPeer *peer = gPeer;
    const bool stamped = peer && peer->pairingStamped();
    const uint32_t verdict = n48_pairing_verb_verdict(arg, gPairingDecided ? 1 : 0,
                                                      stamped ? 1 : 0, gPairingWithdrawn ? 1 : 0);
    uint32_t withdrawReason = 0;
    switch (verdict) {
    case kPairingVerdictEnabled:  gPairingRequest = kPairingReqEnable;  break;
    case kPairingVerdictDisabled: gPairingRequest = kPairingReqDisable; break;
    case kPairingVerdictWithdraw:
        withdrawReason = peer->withdrawDisplayPairingKeysLocked();
        if (withdrawReason == 0) gPairingRequest = kPairingReqDisable;
        break;
    default: break;
    }
    uint32_t bootArg = 0;
    const bool present = PE_parse_boot_argn("navi48-display-pairing", &bootArg, sizeof(bootArg));
    uint64_t v[10] = {
        verdict, gPairingRequest, present ? 1u : 0u, present ? bootArg : 0u,
        gPairingDecided ? 1u : 0u, gPairingSource,
        (peer && peer->pairingStamped()) ? 1u : 0u, gPairingWithdrawn ? 1u : 0u,
        withdrawReason, peer ? 1u : 0u,
    };
    if (gPairingLock) IOLockUnlock(gPairingLock);
    if (out)
        for (unsigned i = 0; i < count && i < 10; i++) out[i] = v[i];
    return verdict;
}

IOReturn Navi48AccelPeer::handleSpecialAMDKey(uint32_t type, void *p2, void *p3, void *p4) {
    switch (type) {

    // GraphicsAccelerator::start()'s final gate. See kReqAccelStarted.
    case kReqAccelStarted:
        PEERLOG("  -> accelerator announcing itself (accel=%p); returning SUCCESS "
                "— this is the gate start() gives up on", p2);
        // p2 IS the accelerator (it passes its own `this`), and start() is
        // finished by the time it asks. Exactly the moment to mark it enabled.
        force_accelerator_enabled(p2);
        // Forcing the bit is not the whole of what the real enable path does.
        // It also has to turn memory allocations on across the hardware layer,
        // which is what populates AMDHWVMM::0x28 — without it the first command
        // buffer faults in endVMPTUpdate.
        // Order matters, and it is not arbitrary: setVirtualSpaceReady() carves
        // the 68 MiB page-table region out of reserved VRAM and writes where it
        // landed into AMDHWVMM::0x50; setMemoryAllocationsEnabled() then reserves
        // that address and builds the page-table block allocators on top of it.
        // Calling only the second one asks to reserve VRAM at address 0, which is
        // refused — measured with DTrace, see AppleHardwareHook.cpp.
        // Apple does all of this in ONE function, and measurement showed that
        // function never runs here:
        //
        //   AMDGraphicsAccelerator::powerUpHW()      (non-virtual, 0xbdc132a)
        //       ... "Virtual space is ready" ...
        //       if (!hw->vtbl[0x208]()) fail
        //       if (!hw->vtbl[0x250]()) fail
        //       if (!memoryManager->enableAllocations()) fail    <-- accel->0x1a48
        //       if (!reserveNDRVSpace()) fail
        //       hw->setVirtualSpaceReady(1) / setMemoryAllocationsEnabled(1) ...
        //
        // enableAllocations() is what hands the two VRAM arenas to the memory
        // manager's allocators. Traced across an entire accelerator start, it is
        // never called once — so every AMDHWMemory::reserve() fails no matter how
        // correct its arguments are, which is exactly what we measured after
        // fixing the arena tops and the page-table base.
        //
        // So stop reimplementing the tail of powerUpHW() one vtable poke at a
        // time and call it. It is not virtual, so it is reached through the
        // AMDRadeonX6000 load slide we already recover from a callback pointer
        // Apple hands us in TTL initialize().
        // The order is Apple's own, taken from powerUpHW(). See the note above
        // call_accel_fn for why each piece is done separately.
        n48::hw_hook_enable_vram_allocations();         // arenas -> allocators
        call_accel_fn("reserveNDRVSpace", kStaticReserveNDRVSpace, p2);
        n48::hw_hook_set_virtual_space_ready(false);    // seeds the VRAM watermarks
        n48::hw_hook_set_virtual_space_ready(true);     // carves the 68 MiB page tables
        n48::hw_hook_enable_memory_allocations(true);   // builds the VM block allocators
        // The AMDHWGart (Hardware+0x378) exists only after the cascade above —
        // set_virtual_space_ready dispatches through it, so by here it is live.
        // Installing the GART mirror at install_hardware_hooks was too early
        // (0x378 was NULL and the hook refused). Opt-in, navi48-mirror-gart=1.
        n48::hw_hook_mirror_apple_gart();               // mirror Apple's GART -> our PTEs
        // createBltMgr() sets accel->0x1a60. Without it AMDAccelResource::pageon
        // dereferences NULL at +0x133 the moment anything is made resident —
        // measured, 0.0.75 panicked exactly there.
        call_accel_fn("createBltMgr", kStaticCreateBltMgr, p2);
        // MILESTONE 3 step 3 (0.0.237): this callback is Apple's own "I have
        // started" announcement, sent from GraphicsAccelerator::start() as the
        // last thing before it decides whether it started - which is exactly the
        // TTL plateau rule 15 says to wait for, and therefore the right moment to
        // run the accelerator-start sequence ourselves. It starts a kernel thread
        // and returns immediately: pm4powerup blocks for 5 s and must never stall
        // a callback Apple is waiting on. Inert unless navi48-boot-chain bit 0.
        // MILESTONE 3 (0.0.241): the display-pairing keys. This callback is the
        // first moment the accelerator nub exists, and it can only be reached
        // behind the exposure gate — see publishDisplayPairingKeys() for why
        // that ordering is the whole safety argument. Done BEFORE arming the
        // boot chain so it cannot be affected by the chain's own thread.
        publishDisplayPairingKeys();
        n48::hw_hook_boot_chain_arm();
        return kIOReturnSuccess;

    // Hardware::isDeviceValid(). param2 points at an int the caller zeroed.
    //
    // ANSWERING 0 HERE IS FATAL, and not in an obvious way: on return the
    // accelerator does
    //     if (handled || *param2 == 0) this->invalidateDevice();
    // so leaving the int at 0 tears the device down. The caller's own fallback
    // path (used when the platform function FAILS) sets it to 1 after confirming
    // the PCI vendor ID reads back, which is what 1 means: the device is there.
    case kReqIsDeviceValid:
        if (p2) *reinterpret_cast<uint32_t *>(p2) = 1;
        // PEER REPLY THROTTLE: this reply line was 10,839 of the log's
        // 10,880 lines. 0.0.173 throttled the REQUEST line and missed this one,
        // so the 512 KiB buffer still hit capacity mid-powerUp and dropped the
        // pm4-powerup outcome - four runs in a row. Gate it on the same counter
        // so request and reply stay in step.
        if (mRequests <= 4 || (mRequests & 0xFFF) == 0)
            PEERLOG("  -> device is valid (*p2 = 1)");
        return kIOReturnSuccess;

    // Power features we do control, but through our own bring-up kext rather
    // than through Apple's framebuffer. Report "not supported" for now so the
    // accelerator does not try to drive them; wiring them to the real SMU path
    // is a later step once the start sequence completes.
    //: AccelChannel::resetHardwareAndReplay() brackets a GPU reset with
    // this request - *p2 = 1 on entry, 0 in the shared tail. Apple never TESTS our
    // return value (: no test follows either sendRequestToController call), so
    // answering success is not by itself the fix; the reset outcome is decided by
    // AMDHardware::resetHardware() reached through iface vtbl[0x3a8]. But leaving a
    // request Apple explicitly brackets falling through to "unhandled" is wrong on
    // its face, and it muddies every log we read. Acknowledge it.
    case kReqResetAndReplay:
        // 0.0.368: RULE E1's reset evidence. Counted BEFORE the log line so a truncated or dropped log
        // line cannot lose the count - the twelfth of our own strings to mislead was a truncated verdict.
        gPeerResetReplayKeys++;
        PEERLOG("  -> reset-hardware-and-replay bracket (*p2=%u); acknowledging. "
                "NOTE: the reset outcome is decided by AMDHardware::resetHardware(), "
                "not by this reply",
                p2 ? *reinterpret_cast<uint32_t *>(p2) : 0u);
        return kIOReturnSuccess;

    case kReqGfxOff:
    case kReqDfCstate:
        PEERLOG("  -> reporting unsupported (we manage this in the bring-up kext)");
        return kIOReturnUnsupported;

    default:
        PEERLOG("  -> unhandled request type; returning unsupported");
        return kIOReturnUnsupported;
    }
}

// =====================================================================================================
// 0.0.272 — MILESTONE 4 ROUTE c' WALL 3: the per-surface externalMethod hook.
//
// Apple's AMD surface does not override externalMethod: AMD surface vtable slot 0x850 holds raw
// 0x400000545b8822 = IOAccelSurface::externalMethod 0x145b8822, whose selector-10 arm returns success having
// done nothing for any surface that is not an IOAccelLegacySurface (145b8964: 0f 84 c7 01 00 00 -> 145b8b31).
// So the hook goes on the SURFACE, per instance, installed where the accelerator mints one:
// AMDRadeonX6000_AMDGraphicsAccelerator::newSurface (accelerator slot 0xa30 = 326, 0xbdc22b6: `new` 0x1358 bytes,
// C1 constructor, no argument). The accelerator already runs on our per-instance vtable copy (gAccelVt, the stop()
// patch), so installing is one slot store into our own table.
//
// Guards, all from live memory minus the slide (rule 54), the slide itself taken from TWO anchors of the
// accelerator's own table: slot 184 = start 0xbdbddc2 must give a page-aligned slide, and slot 326 must then read
// exactly newSurface 0xbdc22b6. The first surface minted must carry, minus that SAME slide, AMD surface D1 0xbde0b02
// at slot 0, IOAccelSurface::externalMethod 0x145b8822 at slot 266 and IOAccelSurface::set_id_mode 0x145b7816 at
// slot 321 - the last two are IOAcceleratorFamily2 addresses, so this also proves the two kexts share one slide
// before anything of ours runs on a surface. Every surface after it is patched only if its vtable pointer is that
// same class table; anything else is counted and left alone. ONE shared copy of the 330-slot table (no per-surface
// allocation, nothing to free).
//
// The hook calls Apple's externalMethod unchanged for every selector and returns its result. Mode 1 (log-only)
// reads fields; mode 3 additionally, after a successful selector 10, copies the flushed surface's buffer into the
// scanout through navi48_scanout_copy_vram, which refuses until the positive control passed.
// =====================================================================================================
static constexpr uintptr_t kStaticAccelStart     = 0x0bdbddc2ULL;  // accelerator slot 184 (tryPatchAcceleratorStop)
static constexpr uintptr_t kStaticNewSurface     = 0x0bdc22b6ULL;  // accelerator slot 326 (byte 0xa30)
static constexpr unsigned  kAccelNewSurfaceSlot  = 326;
static constexpr unsigned  kAccelStartSlot       = 184;
static constexpr uintptr_t kStaticSurfD1         = 0x0bde0b02ULL;  // AMD surface slot 0
static constexpr uintptr_t kStaticSurfExtMethod  = 0x145b8822ULL;  // IOAccelSurface::externalMethod, slot 266
static constexpr uintptr_t kStaticSurfSetIdMode  = 0x145b7816ULL;  // IOAccelSurface::set_id_mode, slot 321
static constexpr unsigned  kSurfVtableSlots      = 330;            // __ZTV30...AMDAccelSurface 0xbf1b260..0xbf1bcc0
static constexpr unsigned  kSurfExtMethodSlot    = 266;
static constexpr unsigned  kSurfSetIdModeSlot    = 321;
// 0.0.278: WHOLE FRAMES at (0,0), clipped by the plan to the scanout; flush4's band was rows 300-427. Surfaces
// smaller than 640x480 are not copied (a cursor-sized surface over the top-left corner would say nothing about the frame).
static constexpr uint32_t  kFlushCopyDstY        = 0;
static constexpr uint32_t  kFlushCopyRows        = 1080;   // 0.0.514 B4: the FALLBACK; the live Console,Height clamps first
static constexpr uint32_t  kFlushCopyMinW        = 640, kFlushCopyMinH = 480;
static constexpr uint32_t  kFlushCopyLogBudget   = 12, kFlushCopyFailLogBudget = 24, kFlushSel10Budget = 8;

typedef void    *(*NewSurfaceFn)(void *);
typedef IOReturn (*SurfExtFn)(void *, uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *);

static IOLock      *gFlushLock { nullptr };
static uint32_t     gFlushMode { 0 };          // bit0 log, bit1 copy
static uint32_t     gFlushInstalled { 0 };     // 1 once newSurface is ours
static uintptr_t    gFlushSlide { 0 };
static NewSurfaceFn gOrigNewSurface { nullptr };
static void       **gSurfOrigVt { nullptr };
static void       **gSurfCopyVt { nullptr };
static SurfExtFn    gOrigSurfExt { nullptr };
static uint32_t     gSurfMinted { 0 }, gSurfPatched { 0 }, gSurfForeign { 0 }, gSurfGuardRefused { 0 };
static uint32_t     gSel7 { 0 }, gSel7Bad { 0 }, gSelShape { 0 }, gSelLock { 0 }, gSelUnlock { 0 };
static uint32_t     gFlushCalls { 0 }, gFlushBad { 0 }, gFlushCopyOk { 0 }, gFlushCopyRefused { 0 };
static uint32_t     gFlushLastCopySt { 0 }, gFlushLastPlan { 0 }, gFlushLastId { 0 }, gFlushLastFbCount { 0 };
static uint32_t     gFlushLogLines { 0 }, gFlushDetailLines { 0 }, gFlushLogDropped { 0 };
static uint32_t     gFlushCopyLogs { 0 }, gFlushCopyFailLogs { 0 }, gFlushCopySmall { 0 }, gFlushSel10Lines { 0 };   // 0.0.278
// 0.0.279: set by the 2D-context method stubs below; once WindowServer's sync has returned, its next surface calls
// are logged whatever the shared budget, so the report can say what the present does after the sync.
static uint64_t     g2dWsFinishReturns { 0 };
static uint32_t     g2dPostFinishSurfaceLines { 0 };
static constexpr uint32_t k2dPostFinishSurfaceBudget = 48;
static constexpr uint32_t kFlushLogBudget = 64, kFlushDetailBudget = 8;

static bool flush_kptr(const void *p) { return reinterpret_cast<uintptr_t>(p) >= kKernelHalfBase; }

static const char *flush_class(const void *obj) {
    if (!flush_kptr(obj)) return "(not a kernel pointer)";
    const OSMetaClass *m = static_cast<const OSObject *>(obj)->getMetaClass();
    const char *n = m ? m->getClassName() : nullptr;
    return n ? n : "(no class)";
}

static const char *flush_selector_name(uint32_t sel) {
    switch (sel) {
    case 0: return "ReadLockWithOptions"; case 1: return "ReadUnlockWithOptions";
    case 3: return "WriteLockWithOptions"; case 4: return "WriteUnlockWithOptions";
    case 5: return "ReadSurface"; case 7: return "create/set_id_mode"; case 8: return "SetSurfaceScale";
    case 9: return "SetSurfaceFramebufferShape"; case 10: return "FlushSurfaceOnFramebuffers";
    case 11: return "QueryLock"; case 12: return "ReadLock"; case 13: return "ReadUnlock";
    case 14: return "WriteLock"; case 15: return "WriteUnlock"; case 16: return "SurfaceControl";
    case 17: return "SetSurfaceFramebufferShapeWithBacking"; default: return "?";
    }
}

// Everything the flush can tell us about the surface, its buffers and the adopted framebuffers. Reads only.
// Returns the VidMemory physical offset and length of buffer 0 through the out-params when it is resolvable.
static bool flush_describe(void *surf, bool log, uint64_t *physOut, uint64_t *lenOut, uint32_t *wOut, uint32_t *hOut,
                           IOMemoryDescriptor **backingOut) {
    *physOut = 0; *lenOut = 0; *wOut = 0; *hOut = 0; *backingOut = nullptr;
    const char *s = static_cast<const char *>(surf);
    void *accel = *reinterpret_cast<void *const *>(s + 0x12c8);
    const uint32_t id = *reinterpret_cast<const uint32_t *>(s + 0x1168);
    const uint64_t mode = *reinterpret_cast<const uint64_t *>(s + 0x11b0);
    gFlushLastId = id;
    if (log)
        PEERLOG("flush-hook: surface %p class %s id %#x mode %#llx accelerator %p (%s)", surf, flush_class(surf), id,
                (unsigned long long)mode, accel, accel == gAccelObj ? "OURS" : "NOT the patched accelerator");
    if (accel != gAccelObj) return false;
    // The adopted framebuffers: accel+0x378 display machine, dm+0x108 count, dm+0x88[i] pipe, pipe+0x98 fb
    // (IOAccelDisplayMachine::getFramebufferCount 0x1458fe92, getIOFramebuffer 0x1458ffc6, pipe getFramebuffer
    // 0x145cc078), accel+0x180/+0x188/+0x190 the VRAM descriptor, map and kernel VA (find_vram_descriptor).
    {
        const char *a = static_cast<const char *>(accel);
        void *dm = *reinterpret_cast<void *const *>(a + 0x378);
        uint32_t n = flush_kptr(dm) ? *reinterpret_cast<const uint32_t *>(static_cast<const char *>(dm) + 0x108) : 0;
        gFlushLastFbCount = n;
        if (log) {
            PEERLOG("flush-hook: accelerator display machine %p (%s) holds %u framebuffer(s); VRAM descriptor %p (%s) map %p "
                    "kernel VA %#llx", dm, flush_class(dm), n, *reinterpret_cast<void *const *>(a + 0x180),
                    flush_class(*reinterpret_cast<void *const *>(a + 0x180)), *reinterpret_cast<void *const *>(a + 0x188),
                    (unsigned long long)*reinterpret_cast<const uint64_t *>(a + 0x190));
            for (uint32_t i = 0; flush_kptr(dm) && i < n && i < 4; i++) {
                void *pipe = *reinterpret_cast<void *const *>(static_cast<const char *>(dm) + 0x88 + 8 * i);
                void *fb = flush_kptr(pipe) ? *reinterpret_cast<void *const *>(static_cast<const char *>(pipe) + 0x98) : nullptr;
                PEERLOG("flush-hook:   framebuffer[%u] pipe %p (%s) framebuffer %p (%s)", i, pipe, flush_class(pipe), fb,
                        flush_class(fb));
            }
        }
    }
    bool resolved = false;
    for (unsigned idx = 0; idx < 13; idx++) {
        void *res = *reinterpret_cast<void *const *>(s + 0x11c0 + 8 * idx);
        if (!res) continue;
        if (!flush_kptr(res)) { if (log) PEERLOG("flush-hook:   buffer[%u] %p is not a kernel pointer", idx, res); continue; }
        const char *r = static_cast<const char *>(res);
        const uint32_t w = *reinterpret_cast<const uint16_t *>(r + 0xb0), h = *reinterpret_cast<const uint16_t *>(r + 0xb2);
        void *mem = *reinterpret_cast<void *const *>(r + 0x88);
        uint64_t phys = 0, span = 0, mlen = 0;
        const char *mcls = flush_class(mem);
        bool physOk = false;
        if (flush_kptr(mem) && !strcmp(mcls, "AMDRadeonX6000_AMDAccelVidMemory")) {
            mlen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(mem) + 0x40);
            void **mvt = *reinterpret_cast<void ***>(mem);
            const uintptr_t fn = flush_kptr(mvt) ? reinterpret_cast<uintptr_t>(mvt[0x158 / 8]) : 0;
            if (gFlushSlide && fn == gFlushSlide + kStaticVidSegment) {
                phys = reinterpret_cast<PhysSegmentFn>(fn)(mem, 0, &span);
                physOk = phys != 0 && span >= mlen;
            } else if (log) {
                PEERLOG("flush-hook:   buffer[%u] VidMemory getPhysicalSegment slot %p is not 0xbdf6c3e + slide %#lx - "
                        "NOT called", idx, (void *)fn, (unsigned long)gFlushSlide);
            }
        }
        if (log)
            PEERLOG("flush-hook:   buffer[%u] %p class %s flags+0xe %#x type+0x14 %#x %ux%u gpuOffset+0xf8 %#llx bytes+0x230 "
                    "%#llx memory %p class %s length %#llx VRAM offset %#llx span %#llx%s", idx, res, flush_class(res),
                    *reinterpret_cast<const uint8_t *>(r + 0xe), *reinterpret_cast<const uint8_t *>(r + 0x14), w, h,
                    (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0xf8),
                    (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0x230), mem, mcls,
                    (unsigned long long)mlen, (unsigned long long)phys, (unsigned long long)span,
                    physOk ? "" : " (no contiguous VRAM segment resolved)");
        if (idx == 0) { *wOut = w; *hOut = h; }
        if (idx == 0 && physOk) { *physOut = phys; *lenOut = mlen; resolved = true; }
        // 0.0.273: the system-memory backing, +0x80 (IOAccelResource2::lockForCPUAccess maps it,
        // 0x145a81ef: movq 0x80(%rbx),%rdi -> IOAccelSysMemory::lockForCPUAccess), and its descriptor at +0xd0.
        if (idx == 0) {
            void *sm = *reinterpret_cast<void *const *>(r + 0x80);
            const char *scls = flush_class(sm);
            uint64_t prep = kIOPreparationIDUnprepared;
            IOMemoryDescriptor *md = nullptr;
            if (flush_kptr(sm) && (!strcmp(scls, "IOAccelSysMemory") || !strcmp(scls, "AMDRadeonX6000_AMDAccelSysMemory")))
                md = backing_descriptor(sm, &prep);
            // 0.0.274: an unprepared backing is used too; the staged copy prepares and completes it.
            if (md) *backingOut = md;
            if (log)
                PEERLOG("flush-hook:   buffer[0] backing +0x80 %p class %s descriptor %p (%s) length %#llx preparationID %#llx%s",
                        sm, scls, md, md ? md->getMetaClass()->getClassName() : "none",
                        (unsigned long long)(md ? md->getLength() : 0), (unsigned long long)prep,
                        md && prep == kIOPreparationIDUnprepared ? " (unprepared: the staged copy will prepare it)" : "");
        }
    }
    return resolved;
}

// 0.0.275: every refused surface create (selector 7) and shape (9, 17) is counted by its FIRST failing check,
// replicated from the bytes, and logged with the process and the three properties CoreDisplay's mapped-display surfaces
// carry (rule 90) - id < 256, mode bits 0x400/0x800, shape option 0x4000 - so the prediction is settled live.
// set_id_mode 0x145b7816 order: bad bits 0xff8073c0 (145b7824), both 0x400 and 0x800 (145b7939), windowed 0x20 (145b79a4),
// id > 0xff (145b79ef); anything else is lock/already-has-id/incompatible. set_shape_backing_length_ext 0x145b8b6c: frame
// buffer index < count (145b8bad), windowed bit (145b8e46), option 0x4000 or surface mode 0xc00 (145b8f4b-145b8f71).
static uint32_t gRej7 { 0 }, gRej7BadBits { 0 }, gRej7FrontBack { 0 }, gRej7NoWin { 0 }, gRej7IdLow { 0 }, gRej7Other { 0 };
static uint32_t gRejShape { 0 }, gRejShapeFb { 0 }, gRejShapeNoWin { 0 }, gRejShape4000 { 0 }, gRejShapeForce { 0 },
                gRejShapeOther { 0 };
static uint32_t gSel7OkWs { 0 }, gRejWs { 0 }, gRejLines { 0 };
static constexpr uint32_t kRejLogBudget = 32, kRejSampleEvery = 64;

static void flush_note_rejection(void *self, uint32_t selector, IOExternalMethodArguments *args, IOReturn kr) {
    char nm[32] = { 0 };
    proc_selfname(nm, (int)sizeof(nm));
    const bool ws = !strncmp(nm, "WindowServer", sizeof(nm));
    uint64_t s0 = 0, s1 = 0;
    const uint32_t nsc = args ? args->scalarInputCount : 0;
    if (nsc > 0) s0 = args->scalarInput[0];
    if (nsc > 1) s1 = args->scalarInput[1];
    const char *s = static_cast<const char *>(self);
    const char *why = "other";
    uint64_t id = 0, mode = 0, opts = 0, fbi = 0;
    uint32_t fbCount = 0;
    uint32_t n = 0;
    if (selector == 7) {
        id = s0; mode = s1;
        n = ++gRej7;
        if (mode & 0xff8073c0ull) { why = "mode has bad bits (mask 0xff8073c0)"; gRej7BadBits++; }
        else if ((mode & 0xc00ull) == 0xc00ull) { why = "mode forces front AND back (0xc00)"; gRej7FrontBack++; }
        else if (!(mode & 0x20ull)) { why = "mode lacks the windowed bit 0x20"; gRej7NoWin++; }
        else if (id <= 0xffull) { why = "id < 256 (assembly surfaces for display updates not supported)"; gRej7IdLow++; }
        else { gRej7Other++; }
    } else {
        opts = s0; fbi = s1;
        id = *reinterpret_cast<const uint32_t *>(s + 0x1168);
        mode = *reinterpret_cast<const uint32_t *>(s + 0x11b0);
        void *dm = *reinterpret_cast<void *const *>(s + 0x12d0);
        fbCount = flush_kptr(dm) ? *reinterpret_cast<const uint32_t *>(static_cast<const char *>(dm) + 0x108) : 0;
        n = ++gRejShape;
        if (fbi >= fbCount) { why = "framebuffer index >= display machine count"; gRejShapeFb++; }
        else if (!(mode & 0x20ull)) { why = "surface mode lacks the windowed bit"; gRejShapeNoWin++; }
        else if (opts & 0x4000ull) { why = "shape option 0x4000"; gRejShape4000++; }
        else if (mode & 0xc00ull) { why = "surface mode forces front/back (0x400/0x800)"; gRejShapeForce++; }
        else { gRejShapeOther++; }
    }
    if (ws) gRejWs++;
    const uint32_t total = gRej7 + gRejShape;
    if (gRejLines < kRejLogBudget || (total % kRejSampleEvery) == 0) {
        gRejLines++;
        PEERLOG("flush-hook: REFUSED%s selector %u (%s) #%u from pid %d (%s)%s: kr %#x, first failing check: %s | id %#llx (%s 256), "
                "mode %#llx (0x400 %s, 0x800 %s, 0x8000 %s), shape options %#llx (0x4000 %s), fb index %llu of %u",
                gRejLines > kRejLogBudget ? " (SAMPLE)" : "", selector, flush_selector_name(selector), n, proc_selfpid(), nm,
                ws ? " [WS]" : "", kr, why, (unsigned long long)id, id <= 0xff ? "BELOW" : "at or above",
                (unsigned long long)mode, (mode & 0x400) ? "SET" : "clear", (mode & 0x800) ? "SET" : "clear",
                (mode & 0x8000) ? "SET" : "clear", (unsigned long long)opts, (opts & 0x4000) ? "SET" : "clear",
                (unsigned long long)fbi, fbCount);
    }
}

static IOReturn hook_surface_ext(void *self, uint32_t selector, IOExternalMethodArguments *args,
                                 IOExternalMethodDispatch *disp, OSObject *target, void *ref) {
    const uint32_t mode = gFlushMode;
    const bool log = (mode & 1u) != 0;
    uint64_t s0 = 0, s1 = 0;
    uint32_t nsc = 0;
    if (args) { nsc = args->scalarInputCount; if (nsc > 0) s0 = args->scalarInput[0]; if (nsc > 1) s1 = args->scalarInput[1]; }
    const bool interesting = selector == 7 || selector == 9 || selector == 17 || selector == 10 ||
                             selector == 0 || selector == 3 || selector == 12 || selector == 14 ||
                             selector == 1 || selector == 4 || selector == 13 || selector == 15;
    IOReturn kr = gOrigSurfExt ? gOrigSurfExt(self, selector, args, disp, target, ref) : kIOReturnUnsupported;
    switch (selector) {
    case 7: {
        gSel7++;
        if (kr) gSel7Bad++;
        if (!kr) { char pn[32] = { 0 }; proc_selfname(pn, (int)sizeof(pn)); if (!strncmp(pn, "WindowServer", sizeof(pn))) gSel7OkWs++; }
        break;
    }
    case 9: case 17: gSelShape++; break;
    case 0: case 3: case 12: case 14: gSelLock++; break;
    case 1: case 4: case 13: case 15: gSelUnlock++; break;
    case 10: gFlushCalls++; if (kr) gFlushBad++; break;
    default: break;
    }
    if (kr != kIOReturnSuccess && (selector == 7 || selector == 9 || selector == 17)) flush_note_rejection(self, selector, args, kr);
    // 0.0.278: the first flushes are logged whatever the shared budget, which creates and locks can spend first.
    if (g2dWsFinishReturns && g2dPostFinishSurfaceLines < k2dPostFinishSurfaceBudget) {
        char pn[32] = { 0 };
        proc_selfname(pn, (int)sizeof(pn));
        if (!strncmp(pn, "WindowServer", sizeof(pn))) {
            g2dPostFinishSurfaceLines++;
            PEERLOG("flush-hook: AFTER A SYNC RETURNED (%u of %u): surface %p selector %u (%s) from WindowServer pid %d: scalars %u [%#llx, %#llx] -> kr %#x",
                    g2dPostFinishSurfaceLines, k2dPostFinishSurfaceBudget, self, selector, flush_selector_name(selector), proc_selfpid(), nsc,
                    (unsigned long long)s0, (unsigned long long)s1, kr);
        }
    }
    const bool sel10Line = log && selector == 10 && gFlushSel10Lines < kFlushSel10Budget && gFlushLogLines >= kFlushLogBudget;
    if (sel10Line) {
        gFlushSel10Lines++;
        char nm[32] = { 0 };
        proc_selfname(nm, (int)sizeof(nm));
        PEERLOG("flush-hook: FLUSH (selector 10, own budget %u of %u) on surface %p from pid %d (%s): scalars %u [%#llx, %#llx] -> kr %#x",
                gFlushSel10Lines, kFlushSel10Budget, self, proc_selfpid(), nm, nsc, (unsigned long long)s0, (unsigned long long)s1, kr);
    }
    if (log && interesting) {
        if (gFlushLogLines < kFlushLogBudget) {
            gFlushLogLines++;
            char nm[32] = { 0 };
            proc_selfname(nm, (int)sizeof(nm));
            const uint32_t sin = args ? args->structureInputSize : 0, sout = args ? args->structureOutputSize : 0;
            PEERLOG("flush-hook: #%u surface %p selector %u (%s) from pid %d (%s): scalars %u [%#llx, %#llx] struct in %u "
                    "out %u -> kr %#x", gFlushLogLines, self, selector, flush_selector_name(selector), proc_selfpid(), nm, nsc,
                    (unsigned long long)s0, (unsigned long long)s1, sin, sout, kr);
            if (!kr && args && (selector == 0 || selector == 3 || selector == 12 || selector == 14) &&
                args->structureOutput && args->structureOutputSize >= 0x58) {
                const char *o = static_cast<const char *>(args->structureOutput);
                PEERLOG("flush-hook:   lock info: address %#llx +0x20 %#x width %u height %u format %#x +0x30 %#x +0x44 %#x "
                        "+0x48 %#x +0x4c %#x +0x50 %#x", (unsigned long long)*reinterpret_cast<const uint64_t *>(o),
                        *reinterpret_cast<const uint32_t *>(o + 0x20), *reinterpret_cast<const uint32_t *>(o + 0x24),
                        *reinterpret_cast<const uint32_t *>(o + 0x28), *reinterpret_cast<const uint32_t *>(o + 0x2c),
                        *reinterpret_cast<const uint32_t *>(o + 0x30), *reinterpret_cast<const uint32_t *>(o + 0x44),
                        *reinterpret_cast<const uint32_t *>(o + 0x48), *reinterpret_cast<const uint32_t *>(o + 0x4c),
                        *reinterpret_cast<const uint32_t *>(o + 0x50));
            }
        } else {
            gFlushLogDropped++;
        }
    }
    if (selector == 10 && kr == kIOReturnSuccess && mode) {
        const bool detail = log && gFlushDetailLines < kFlushDetailBudget;
        if (detail) gFlushDetailLines++;
        uint64_t phys = 0, len = 0; uint32_t w = 0, h = 0;
        IOMemoryDescriptor *backing = nullptr;
        const bool resolved = flush_describe(self, detail, &phys, &len, &w, &h, &backing);
        // build 0.0.516 (switch 73): THE OTHER SCANOUT WRITER IS IDLE WHILE 73 IS ON. This copy puts a
        // flushed surface into the scanout with no P behind it, which is exactly what switch 73 exists to withhold; so while the
        // switch is ON it is skipped (counted in `gfxneuter 73`'s report) whatever the flush-hook mode. OFF, one call answering 0.
        if ((mode & 2u) && n48::hw_p73_on()) n48::hw_p73_flush_held();
        else if (mode & 2u) {
            uint64_t v[11] = { 0 };
            uint32_t st = 16;   // 16 = neither a contiguous VRAM segment nor a prepared backing for buffer 0
            // build 0.0.514 B4: the clamp is the LIVE framebuffer (Console,Width/Height), 1920 x kFlushCopyRows
            // only when that is absent - 0.0.513's constants, so a 1080p boot clamps exactly as before.
            uint32_t liveW = 0u, liveH = 0u;
            (void)navi48_scanout_live_dims(&liveW, &liveH);
            const uint32_t capW = n48_live_dim(liveW, 1920u), capH = n48_live_dim(liveH, kFlushCopyRows);
            const uint32_t cw = w < capW ? w : capW, ch = h < capH ? h : capH;
            const char *path = "none";
            if (w < kFlushCopyMinW || h < kFlushCopyMinH) {
                path = "NOT COPIED (smaller than 640x480)";
                st = 18;
                gFlushCopySmall++;
            } else if (resolved) {
                path = "VRAM";
                st = navi48_scanout_copy_vram(phys, len, w, h, w * 4u, 0, kFlushCopyDstY, cw, ch, v, 11);
            } else if (backing && w && h) {
                path = "STAGED from the system-memory backing";
                st = navi48_scanout_copy_staged(backing, w * 4u, w, h, 0, kFlushCopyDstY, cw, ch, v, 11);
            }
            gFlushLastCopySt = st; gFlushLastPlan = (uint32_t)v[1];
            if (st == 0) gFlushCopyOk++; else if (st != 18) gFlushCopyRefused++;
            const bool logCopy = gFlushCopyLogs < kFlushCopyLogBudget || (st != 0 && st != 18 && gFlushCopyFailLogs < kFlushCopyFailLogBudget);
            if (logCopy) {
                if (gFlushCopyLogs < kFlushCopyLogBudget) gFlushCopyLogs++; else gFlushCopyFailLogs++;
                char nm[32] = { 0 };
                proc_selfname(nm, (int)sizeof(nm));
                PEERLOG("flush-hook: COPY #%u of surface %p id %#x from pid %d (%s) buffer 0 (%ux%u, stride w*4; path %s; VRAM %#llx "
                        "len %#llx) -> rect (0,%u) %ux%u status %u (0 copied and read back; 13 interlock: positive control not passed; "
                        "16 buffer unresolved; 17 backing unprepared or short; 18 too small), plan reason %llu, row mismatches %llu of "
                        "%llu, samples dst/src centre %#010x/%#010x; copies ok %u refused %u small %u",
                        gFlushCopyOk + gFlushCopyRefused + gFlushCopySmall, self, gFlushLastId, proc_selfpid(), nm, w, h, path,
                        (unsigned long long)phys, (unsigned long long)len, kFlushCopyDstY, cw, ch, st, (unsigned long long)v[1],
                        (unsigned long long)v[5], (unsigned long long)v[6], (uint32_t)(v[9] >> 32), (uint32_t)v[9], gFlushCopyOk,
                        gFlushCopyRefused, gFlushCopySmall);
            }
        }
    }
    return kr;
}

static bool gSurfGuardFailed { false };

// First surface since arming: verify its class table against the guards and build the ONE shared copy.
static void flush_build_surface_copy(void *s) {
    if (!gFlushLock) return;
    IOLockLock(gFlushLock);
    void **vt = *reinterpret_cast<void ***>(s);
    if (!gSurfCopyVt && !gSurfGuardFailed && flush_kptr(vt)) {
        const uintptr_t d1 = reinterpret_cast<uintptr_t>(vt[0]);
        const uintptr_t em = reinterpret_cast<uintptr_t>(vt[kSurfExtMethodSlot]);
        const uintptr_t sm = reinterpret_cast<uintptr_t>(vt[kSurfSetIdModeSlot]);
        if (d1 - gFlushSlide != kStaticSurfD1 || em - gFlushSlide != kStaticSurfExtMethod ||
            sm - gFlushSlide != kStaticSurfSetIdMode) {
            gSurfGuardRefused++;
            gSurfGuardFailed = true;   // never retried: one refusal line, then every surface stays Apple's
            PEERLOG("flush-hook: surface %p class %s vtable GUARD REFUSED - slot 0 %#lx (want %#lx), slot 266 %#lx (want %#lx), "
                    "slot 321 %#lx (want %#lx), all minus slide %#lx. Nothing patched, now or later this boot.", s,
                    flush_class(s), (unsigned long)(d1 - gFlushSlide), (unsigned long)kStaticSurfD1,
                    (unsigned long)(em - gFlushSlide), (unsigned long)kStaticSurfExtMethod, (unsigned long)(sm - gFlushSlide),
                    (unsigned long)kStaticSurfSetIdMode, (unsigned long)gFlushSlide);
        } else {
            const size_t bytes = (kSurfVtableSlots + kVtHeader) * sizeof(void *);
            void **copy = static_cast<void **>(IOMalloc(bytes));
            if (copy) {
                memcpy(copy, vt - kVtHeader, bytes);
                gOrigSurfExt = reinterpret_cast<SurfExtFn>(vt[kSurfExtMethodSlot]);
                copy[kVtHeader + kSurfExtMethodSlot] = reinterpret_cast<void *>(&hook_surface_ext);
                gSurfOrigVt = vt;
                gSurfCopyVt = copy + kVtHeader;   // published last: a reader sees either nullptr or a complete table
                PEERLOG("flush-hook: first surface %p class %s: vtable GUARDS PASSED (D1 %#lx, externalMethod %#lx, set_id_mode "
                        "%#lx, one slide %#lx for AMDRadeonX6000 AND IOAcceleratorFamily2); shared %u-slot copy built, slot 266 "
                        "now ours", s, flush_class(s), (unsigned long)kStaticSurfD1, (unsigned long)kStaticSurfExtMethod,
                        (unsigned long)kStaticSurfSetIdMode, (unsigned long)gFlushSlide, kSurfVtableSlots);
            }
        }
    }
    IOLockUnlock(gFlushLock);
}

static void *hook_new_surface(void *accel) {
    void *s = gOrigNewSurface ? gOrigNewSurface(accel) : nullptr;
    gSurfMinted++;
    if (!s || gFlushMode == 0) return s;
    if (accel != gAccelObj) { gSurfForeign++; return s; }
    if (!gSurfCopyVt) flush_build_surface_copy(s);
    if (!gSurfCopyVt) return s;
    void ***slot = reinterpret_cast<void ***>(s);
    if (*slot == gSurfCopyVt) return s;
    if (*slot != gSurfOrigVt) { gSurfForeign++; return s; }
    __asm__ __volatile__("sfence" ::: "memory");
    *slot = gSurfCopyVt;
    gSurfPatched++;
    if (gSurfPatched <= 8)
        PEERLOG("flush-hook: surface %p (#%u minted) patched - externalMethod now ours, pass-through", s, gSurfMinted);
    return s;
}

// out[0] status (0 ok, 1 no accelerator table, 2 slide, 3 newSurface guard, 4 copy refused by the interlock, 5 bad
// argument), [1] mode, [2] installed, [3] slide, [4] minted | patched<<32, [5] foreign | guard refused<<32,
// [6] selector 7 calls | non-zero<<32, [7] shape calls, [8] lock | unlock<<32, [9] flushes | non-zero<<32,
// [10] copies ok | refused<<32, [11] last copy status | last plan reason<<32, [12] last flushed id | framebuffers<<32.
uint32_t navi48_flushhook_control(uint64_t arg, uint64_t *out, unsigned count) {
    uint32_t st = 0;
    if (!gFlushLock) gFlushLock = IOLockAlloc();
    if (arg == 4) {
        // 0.0.275: the rejection counters, a different layout; the mode is not changed.
        uint64_t r[13] = { 0, gRej7, gRej7IdLow, gRej7BadBits, gRej7FrontBack, gRej7NoWin, gRej7Other, gRejShape, gRejShape4000,
                           gRejShapeForce, gRejShapeFb, ((uint64_t)gRejShapeOther << 32) | gRejShapeNoWin,
                           ((uint64_t)gRejWs << 32) | gSel7OkWs };
        PEERLOG("flush-hook: REJECTIONS - selector 7: %u (id<256 %u, bad bits %u, front+back %u, no windowed %u, other %u); shape: %u "
                "(option 0x4000 %u, mode 0x400/0x800 %u, fb index %u, no windowed %u, other %u); from WindowServer: %u refused, %u "
                "creates OK; census WindowServer keys %u (lines %u)", gRej7, gRej7IdLow, gRej7BadBits, gRej7FrontBack, gRej7NoWin,
                gRej7Other, gRejShape, gRejShape4000, gRejShapeForce, gRejShapeFb, gRejShapeNoWin, gRejShapeOther, gRejWs,
                gSel7OkWs, gCensusWsKeys, gCensusWsLines);
        if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = r[i];
        return 0;
    }
    if (arg != 0 && arg != 1 && arg != 2 && arg != 3) st = 5;
    if (!st && (arg == 1 || arg == 3) && !gFlushInstalled) {
        if (!gAccelVt || !gAccelObj || !gFlushLock) {
            st = 1;
        } else {
            const uintptr_t start = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelStartSlot]);
            const uintptr_t slide = start - kStaticAccelStart;
            const uintptr_t ns = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelNewSurfaceSlot]);
            const uintptr_t pt = x6000_slide_from_page_texture();
            if (!flush_kptr(reinterpret_cast<void *>(start)) || (slide & 0xfff)) {
                st = 2;
            } else if (ns - slide != kStaticNewSurface) {
                st = 3;
            } else {
                gFlushSlide = slide;
                gOrigNewSurface = reinterpret_cast<NewSurfaceFn>(ns);
                __asm__ __volatile__("sfence" ::: "memory");
                gAccelVt[kVtHeader + kAccelNewSurfaceSlot] = reinterpret_cast<void *>(&hook_new_surface);
                gFlushInstalled = 1;
            }
            PEERLOG("flush-hook: install - accelerator %p slot 184 start %#lx -> slide %#lx (pageTexture slide %#lx: %s); slot 326 "
                    "%#lx minus slide = %#lx (want newSurface %#lx) -> %s", gAccelObj, (unsigned long)start,
                    (unsigned long)slide, (unsigned long)pt, pt ? (pt == slide ? "AGREES" : "DISAGREES") : "not captured",
                    (unsigned long)ns, (unsigned long)(ns - slide), (unsigned long)kStaticNewSurface,
                    st == 0 ? "INSTALLED (newSurface now ours)" : (st == 2 ? "REFUSED: slide" : "REFUSED: slot is not newSurface"));
            if (!st && pt && pt != slide) { /* recorded above; the accelerator's own two anchors decide */ }
        }
    }
    if (!st) {
        if (arg == 1) gFlushMode = 1;
        else if (arg == 2) gFlushMode = 0;
        else if (arg == 3) {
            if (navi48_scanout_pc_passed()) gFlushMode = 3;
            else { gFlushMode = 1; st = 4; }
        }
        if (arg) PEERLOG("flush-hook: mode now %u (%s)%s", gFlushMode,
                         gFlushMode == 3 ? "log + COPY into the scanout" : gFlushMode == 1 ? "LOG-ONLY" : "pass-through",
                         st == 4 ? " - copy REFUSED: the scanout positive control has not passed this boot" : "");
    }
    uint64_t v[13] = {
        st, gFlushMode, gFlushInstalled, gFlushSlide,
        ((uint64_t)gSurfPatched << 32) | gSurfMinted, ((uint64_t)gSurfGuardRefused << 32) | gSurfForeign,
        ((uint64_t)gSel7Bad << 32) | gSel7, gSelShape, ((uint64_t)gSelUnlock << 32) | gSelLock,
        ((uint64_t)gFlushBad << 32) | gFlushCalls, ((uint64_t)gFlushCopyRefused << 32) | gFlushCopyOk,
        ((uint64_t)gFlushLastPlan << 32) | gFlushLastCopySt, ((uint64_t)gFlushLastFbCount << 32) | gFlushLastId,
    };
    PEERLOG("flush-hook: control %llu -> status %u; minted %u patched %u foreign %u guard-refused %u; sel7 %u (non-zero %u) "
            "shape %u lock %u unlock %u flush %u (non-zero %u) copies %u refused %u; log lines %u dropped %u",
            (unsigned long long)arg, st, gSurfMinted, gSurfPatched, gSurfForeign, gSurfGuardRefused, gSel7, gSel7Bad,
            gSelShape, gSelLock, gSelUnlock, gFlushCalls, gFlushBad, gFlushCopyOk, gFlushCopyRefused, gFlushLogLines,
            gFlushLogDropped);
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}

// =====================================================================================================
// 0.0.276 — OBSERVE WindowServer's 2D context. READ-ONLY: every call goes to Apple unchanged.
//
// testa2 blocked in MPHWSync -> IOAccel2DContext::finish. AMD's 2D context has exactly two operations,
// AMDRadeonX6000_AMDAccel2DContext::blitCopy (slot 360, 0xbde1fe6) and ::blitFill (slot 361, 0xbde2658); blitFill takes
// (IOAccelEvent*, colour, IOAccelResource2 *dst, IOAccel2DBlitRectStruc *rects, n), reads each 12-byte rect's int16 x, y, w, h
// at +4/+6/+8/+10 (bde27e9-bde2806), and hands the batch to the blit manager (accel+0x1a60, vtable 0x128, bde2880) - where
// the GPU work and its stamp come from. The context is minted by AMDGraphicsAccelerator::new2DContext (accelerator slot 327,
// 0xbdc22e8). Guards as for surfaces: two-anchor slide, then slot 0 D1 0xbde1e2c, 316 contextStart 0xbde1fa2, 360 and 361 on
// the first 2D context minted; one shared copy of the 369-slot table (__ZTV32...AMDAccel2DContext 0xbf1bdb8..0xbf1c950).
// =====================================================================================================
static constexpr unsigned  kAccelNew2DSlot       = 327;
static constexpr uintptr_t kStaticNew2DContext   = 0x0bdc22e8ULL;
static constexpr unsigned  k2dVtableSlots        = 369;
static constexpr uintptr_t kStatic2dD1           = 0x0bde1e2cULL;
static constexpr unsigned  k2dStartSlot          = 316;
static constexpr uintptr_t kStatic2dStart        = 0x0bde1fa2ULL;
static constexpr unsigned  k2dCopySlot           = 360;
static constexpr uintptr_t kStatic2dCopy         = 0x0bde1fe6ULL;
static constexpr unsigned  k2dFillSlot           = 361;
static constexpr uintptr_t kStatic2dFill         = 0x0bde2658ULL;

typedef void    *(*New2DFn)(void *);
typedef uint64_t (*Blit2DCopyFn)(void *, void *, void *, void *, void *, uint32_t);
typedef uint64_t (*Blit2DFillFn)(void *, void *, uint32_t, void *, void *, uint32_t);
static uint32_t     g2dMode { 0 }, g2dInstalled { 0 };
static uintptr_t    g2dSlide { 0 };
static New2DFn      gOrigNew2D { nullptr };
static void       **g2dOrigVt { nullptr }, **g2dCopyVt { nullptr };
static bool         g2dGuardFailed { false };
static Blit2DCopyFn gOrig2dCopy { nullptr };
static Blit2DFillFn gOrig2dFill { nullptr };
static uint32_t     g2dMinted { 0 }, g2dPatched { 0 }, g2dCopies { 0 }, g2dFills { 0 }, g2dLines { 0 }, g2dRects { 0 };
static constexpr uint32_t k2dLogBudget = 48;

static void res_describe(const char *tag, void *res) {
    if (!flush_kptr(res)) { PEERLOG("ctx2d:   %s %p (not a kernel pointer)", tag, res); return; }
    const char *r = static_cast<const char *>(res);
    const uint32_t w = *reinterpret_cast<const uint16_t *>(r + 0xb0), h = *reinterpret_cast<const uint16_t *>(r + 0xb2);
    void *mem = *reinterpret_cast<void *const *>(r + 0x88);
    const char *mcls = flush_class(mem);
    uint64_t phys = 0, span = 0, mlen = 0;
    if (flush_kptr(mem) && !strcmp(mcls, "AMDRadeonX6000_AMDAccelVidMemory")) {
        mlen = *reinterpret_cast<const uint64_t *>(static_cast<const char *>(mem) + 0x40);
        void **mvt = *reinterpret_cast<void ***>(mem);
        const uintptr_t fn = flush_kptr(mvt) ? reinterpret_cast<uintptr_t>(mvt[0x158 / 8]) : 0;
        if (g2dSlide && fn == g2dSlide + kStaticVidSegment) phys = reinterpret_cast<PhysSegmentFn>(fn)(mem, 0, &span);
    }
    void *sm = *reinterpret_cast<void *const *>(r + 0x80);
    PEERLOG("ctx2d:   %s %p class %s %ux%u flags+0xe %#x bytes+0x230 %#llx | vidmem %p (%s) len %#llx VRAM %#llx span %#llx | backing %p (%s)",
            tag, res, flush_class(res), w, h, *reinterpret_cast<const uint8_t *>(r + 0xe),
            (unsigned long long)*reinterpret_cast<const uint64_t *>(r + 0x230), mem, mcls, (unsigned long long)mlen,
            (unsigned long long)phys, (unsigned long long)span, sm, flush_class(sm));
}

static void ctx2d_rects(void *rects, uint32_t n) {
    if (!flush_kptr(rects) || n == 0) return;
    const char *q = static_cast<const char *>(rects);
    for (uint32_t i = 0; i < n && i < 3; i++) {
        const char *e = q + 12 * i;
        PEERLOG("ctx2d:   rect[%u] +0 %#010x | int16 +4 %d +6 %d +8 %d +10 %d", i, *reinterpret_cast<const uint32_t *>(e),
                *reinterpret_cast<const int16_t *>(e + 4), *reinterpret_cast<const int16_t *>(e + 6),
                *reinterpret_cast<const int16_t *>(e + 8), *reinterpret_cast<const int16_t *>(e + 10));
    }
}

static void ctx2d_event(const char *when, void *ev) {
    if (!flush_kptr(ev)) { PEERLOG("ctx2d:   event %s %p (not a kernel pointer)", when, ev); return; }
    const uint64_t *q = reinterpret_cast<const uint64_t *>(ev);
    PEERLOG("ctx2d:   event %s %p = %#018llx %#018llx %#018llx %#018llx", when, ev, (unsigned long long)q[0],
            (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3]);
}

// 0.0.277: which of the accelerator's channels (accel+0x1a78[0..62], stamp index at chan+0x20 - the table
// `stampgap` reads) this LIVE 2D context holds a pointer to. It scans only the context's own 0x1770-byte object
// (new2DContext: `movl $0x1770,%edi`, bdc22fb) and the accelerator's own array; nothing read is dereferenced except the channel
// objects the accelerator itself lists, and only their +0x20 index.
static void ctx2d_find_channels(void *self) {
    if (!gAccelObj || !flush_kptr(self)) return;
    const char *a = static_cast<const char *>(gAccelObj);
    const uint64_t *ctx = reinterpret_cast<const uint64_t *>(self);
    uint32_t hits = 0;
    for (uint32_t slot = 0; slot < 63; slot++) {
        const uint64_t ch = *reinterpret_cast<const uint64_t *>(a + 0x1a78 + (uint64_t)slot * 8);
        if (ch < kKernelHalfBase) continue;
        for (uint32_t o = 0; o < 0x1770 / 8; o++) {
            if (ctx[o] != ch) continue;
            hits++;
            PEERLOG("ctx2d:   context %p holds accel channel slot %u (%#llx, stamp index %u) at +%#x", self, slot,
                    (unsigned long long)ch, *reinterpret_cast<const uint32_t *>(reinterpret_cast<const char *>(ch) + 0x20), o * 8);
        }
    }
    if (!hits) PEERLOG("ctx2d:   context %p holds NONE of the accelerator's channel pointers in its 0x1770 bytes", self);
}

static uint64_t hook_2d_fill(void *self, void *ev, uint32_t color, void *dst, void *rects, uint32_t n) {
    g2dFills++; g2dRects += n;
    const bool log = g2dMode && g2dLines < k2dLogBudget;
    if (log) {
        g2dLines++;
        char nm[24] = { 0 }; proc_selfname(nm, (int)sizeof(nm));
        PEERLOG("ctx2d: blitFill #%u ctx %p from pid %d (%s): colour %#010x, %u rect(s)", g2dFills, self, proc_selfpid(), nm, color, n);
        if (g2dLines <= 2) ctx2d_find_channels(self);
        res_describe("dst", dst); ctx2d_rects(rects, n); ctx2d_event("before", ev);
    }
    const uint64_t rv = gOrig2dFill ? gOrig2dFill(self, ev, color, dst, rects, n) : 0;
    if (log) ctx2d_event("after", ev);
    return rv;
}

static uint64_t hook_2d_copy(void *self, void *ev, void *a, void *b, void *rects, uint32_t n) {
    g2dCopies++; g2dRects += n;
    const bool log = g2dMode && g2dLines < k2dLogBudget;
    if (log) {
        g2dLines++;
        char nm[24] = { 0 }; proc_selfname(nm, (int)sizeof(nm));
        PEERLOG("ctx2d: blitCopy #%u ctx %p from pid %d (%s): %u rect(s)", g2dCopies, self, proc_selfpid(), nm, n);
        if (g2dLines <= 2) ctx2d_find_channels(self);
        res_describe("arg2", a); res_describe("arg3", b); ctx2d_rects(rects, n); ctx2d_event("before", ev);
    }
    const uint64_t rv = gOrig2dCopy ? gOrig2dCopy(self, ev, a, b, rects, n) : 0;
    if (log) ctx2d_event("after", ev);
    return rv;
}

// =====================================================================================================
// 0.0.279 — WHERE WINDOWSERVER'S PRESENT GOES, counted: the 2D context's three user-client methods.
// IOAccelerator2D.plugin (UUID E0F9FDCE, the image could not name) is a CFPlugIn implementing
// IOGraphicsAcceleratorInterface; CoreDisplay's MPHWSync calls its WaitComplete (+0xb0 = 0xa79), which is
// IOConnectCallMethod(selector 0x101, 1 scalar), and IOAccel2DContext2 serves selectors 0x100-0x102 from the legacy table
// s2DContextMethods @0x145eddc0 through getTargetAndMethodForIndex (vtable slot 296, 0x145858a8: `leal -0x100(%rdx)`,
// `cmpl $0x2`, 48-byte entries): [0] set_surface(j, modebits) 0x14584b2c, [1] finish(j) 0x14584e64, [2] blit(cmd, size)
// 0x14584fbc. Hooking slot 296 in the shared 2D-context vtable copy hands out wrapper entries whose function pointers are the
// stubs below; each stub counts per process, times the call and calls Apple's function with the same arguments. Guards: the
// returned entry must be exactly slide + table + 48*k, its function exactly slide + the static address, its adjustment 0.
static constexpr unsigned  k2dTargetMethodSlot  = 296;
static constexpr uintptr_t kStatic2dTargetMethod = 0x145858a8ULL;
static constexpr uintptr_t kStatic2dMethods      = 0x145eddc0ULL;
static constexpr uintptr_t kStatic2dMethodFn[3]  = { 0x14584b2cULL, 0x14584e64ULL, 0x14584fbcULL };
struct N48ExtMethod { void *object; uintptr_t fnPtr; intptr_t fnAdj; uint64_t flags; uint64_t count0; uint64_t count1; };
typedef N48ExtMethod *(*TargetMethodFn)(void *, void **, uint32_t);
typedef IOReturn (*Method6Fn)(void *, void *, void *, void *, void *, void *, void *);
static TargetMethodFn gOrig2dTargetMethod { nullptr };
static N48ExtMethod   g2dWrap[3] {};
static bool           g2dWrapReady[3] {};
static uint32_t       g2dTargetGuardRefused { 0 }, g2dEntryRefused { 0 };
static struct { int pid; char name[20]; uint64_t sel[3], finishReturns, finishMaxUs; } g2dPid[8] {};
static uint64_t       g2dCalls[3] {}, g2dFinishReturns { 0 }, g2dFinishMaxUs { 0 }, g2dWsFinishCalls { 0 };
static uint64_t       g2dLastFinishArg { 0 };
// 0.0.281: the sync latency. Duration buckets (us): <100, <1000, <4000, <16000, <33000, <100000, <1000000, >=1 s;
// WindowServer's interval between successive syncs (ms): <8, <17, <34, <100, <500, <1000, <5000, >=5 s.
static uint64_t       g2dFinishHist[8] {}, g2dWsIntervalHist[8] {};
static uint64_t       g2dWsLastFinishAbs { 0 };
static uint32_t       g2dLastFinishKr { 0 }, g2dCallLines { 0 }, g2dSlowLines { 0 };
static constexpr uint32_t k2dCallLineBudget = 64, k2dSlowLineBudget = 16;

static unsigned ctx2d_pid_slot() {
    const int pid = proc_selfpid();
    for (unsigned i = 0; i < 8; i++) if (g2dPid[i].pid == pid && g2dPid[i].name[0]) return i;
    for (unsigned i = 0; i < 8; i++) if (!g2dPid[i].name[0]) { g2dPid[i].pid = pid; proc_selfname(g2dPid[i].name, (int)sizeof(g2dPid[i].name)); return i; }
    return 7;
}

static IOReturn ctx2d_method_common(unsigned k, void *self, void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) {
    const Method6Fn fn = reinterpret_cast<Method6Fn>(g2dSlide + kStatic2dMethodFn[k]);
    const unsigned ps = ctx2d_pid_slot();
    g2dCalls[k]++;
    g2dPid[ps].sel[k]++;
    const bool ws = !strncmp(g2dPid[ps].name, "WindowServer", sizeof(g2dPid[ps].name));
    if (k == 1) { if (ws) g2dWsFinishCalls++; g2dLastFinishArg = reinterpret_cast<uint64_t>(a0); }
    const bool line = g2dCallLines < k2dCallLineBudget;
    if (line) {
        g2dCallLines++;
        PEERLOG("ctx2d-call: #%u ENTER selector %#x (%s) ctx %p from pid %d (%s): args %#llx %#llx", g2dCallLines, 0x100u + k,
                k == 0 ? "set_surface" : k == 1 ? "finish" : "blit", self, g2dPid[ps].pid, g2dPid[ps].name,
                (unsigned long long)reinterpret_cast<uint64_t>(a0), (unsigned long long)reinterpret_cast<uint64_t>(a1));
    }
    uint64_t t0 = 0; clock_get_uptime(&t0);
    const IOReturn kr = fn(self, a0, a1, a2, a3, a4, a5);
    uint64_t t1 = 0; clock_get_uptime(&t1);
    uint64_t ns = 0; absolutetime_to_nanoseconds(t1 - t0, &ns);
    const uint64_t us = ns / 1000u;
    if (k == 1) {
        static constexpr uint64_t durEdge[7] = { 100, 1000, 4000, 16000, 33000, 100000, 1000000 };
        unsigned db = 7;
        for (unsigned i = 0; i < 7; i++) if (us < durEdge[i]) { db = i; break; }
        g2dFinishHist[db]++;
        if (ws) {
            if (g2dWsLastFinishAbs) {
                uint64_t ins = 0; absolutetime_to_nanoseconds(t0 - g2dWsLastFinishAbs, &ins);
                const uint64_t ims = ins / 1000000u;
                static constexpr uint64_t ivEdge[7] = { 8, 17, 34, 100, 500, 1000, 5000 };
                unsigned ib = 7;
                for (unsigned i = 0; i < 7; i++) if (ims < ivEdge[i]) { ib = i; break; }
                g2dWsIntervalHist[ib]++;
            }
            g2dWsLastFinishAbs = t0;
        }
        g2dFinishReturns++; g2dPid[ps].finishReturns++; g2dLastFinishKr = (uint32_t)kr;
        if (ws) g2dWsFinishReturns++;
        if (us > g2dFinishMaxUs) g2dFinishMaxUs = us;
        if (us > g2dPid[ps].finishMaxUs) g2dPid[ps].finishMaxUs = us;
    }
    if (line || (k == 1 && us > 1000000u && g2dSlowLines++ < k2dSlowLineBudget))
        PEERLOG("ctx2d-call: RETURN selector %#x from pid %d (%s) -> kr %#x after %llu us%s", 0x100u + k, g2dPid[ps].pid, g2dPid[ps].name, kr,
                (unsigned long long)us, (k == 1 && ws) ? " - WindowServer's sync RETURNED" : "");
    return kr;
}
// 0.0.280: wsneuter2 PANICKED here. An IOExternalMethod's `func` is an Itanium member-function pointer: an ODD
// pointer means "virtual, vtable offset + 1". This kext is built -Os, the linker put ctx2d_stub1 at 0x58f95, and the kernel's
// shim_io_connect_method_scalarI_scalarO (+0x2ab) indexed WindowServer's 2D-context vtable with it (R11 = 0xffffff801d442f95 =
// load base + 0x58f95; CR2 = vtable + R11 - 1). 0.0.279's `if (stubAddr & 1) return m;` was folded away by the compiler, which
// assumes a function address is even. Now: the stubs are aligned to 16 bytes, and the parity test reads the addresses from
// volatile storage filled at run time, which the compiler cannot fold.
__attribute__((noinline, aligned(16)))
static IOReturn ctx2d_stub0(void *s, void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) { return ctx2d_method_common(0, s, a0, a1, a2, a3, a4, a5); }
__attribute__((noinline, aligned(16)))
static IOReturn ctx2d_stub1(void *s, void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) { return ctx2d_method_common(1, s, a0, a1, a2, a3, a4, a5); }
__attribute__((noinline, aligned(16)))
static IOReturn ctx2d_stub2(void *s, void *a0, void *a1, void *a2, void *a3, void *a4, void *a5) { return ctx2d_method_common(2, s, a0, a1, a2, a3, a4, a5); }
static volatile uintptr_t g2dStubAddr[3] {};
static uint32_t g2dStubOdd { 0 };

static N48ExtMethod *hook_2d_target_method(void *self, void **target, uint32_t index) {
    N48ExtMethod *m = gOrig2dTargetMethod ? gOrig2dTargetMethod(self, target, index) : nullptr;
    if (!m) return m;
    const uintptr_t table = g2dSlide + kStatic2dMethods;
    for (unsigned k = 0; k < 3; k++) {
        if (reinterpret_cast<uintptr_t>(m) != table + 48u * k) continue;
        if (m->fnPtr != g2dSlide + kStatic2dMethodFn[k] || m->fnAdj != 0) {
            if (g2dEntryRefused++ < 4u)
                PEERLOG("ctx2d-call: method entry %u at %p has function %#lx adj %ld (want %#lx, 0) - NOT wrapped", k, m,
                        (unsigned long)m->fnPtr, (long)m->fnAdj, (unsigned long)(g2dSlide + kStatic2dMethodFn[k]));
            return m;
        }
        const uintptr_t stubAddr = g2dStubAddr[k];   // volatile: this parity test must survive the optimiser
        if (stubAddr == 0 || (stubAddr & 1u)) { g2dStubOdd++; return m; }
        if (!g2dWrapReady[k]) {
            g2dWrap[k] = *m;
            g2dWrap[k].fnPtr = stubAddr;
            g2dWrapReady[k] = true;
        }
        return &g2dWrap[k];
    }
    return m;
}

static void *hook_new_2d(void *accel) {
    void *c = gOrigNew2D ? gOrigNew2D(accel) : nullptr;
    g2dMinted++;
    if (!c || !g2dMode || accel != gAccelObj || !gFlushLock) return c;
    void ***slot = reinterpret_cast<void ***>(c);
    if (!g2dCopyVt && !g2dGuardFailed) {
        IOLockLock(gFlushLock);
        void **vt = *slot;
        if (!g2dCopyVt && !g2dGuardFailed && flush_kptr(vt)) {
            const uintptr_t d1 = reinterpret_cast<uintptr_t>(vt[0]) - g2dSlide, st = reinterpret_cast<uintptr_t>(vt[k2dStartSlot]) - g2dSlide,
                            cp = reinterpret_cast<uintptr_t>(vt[k2dCopySlot]) - g2dSlide, fl = reinterpret_cast<uintptr_t>(vt[k2dFillSlot]) - g2dSlide;
            if (d1 != kStatic2dD1 || st != kStatic2dStart || cp != kStatic2dCopy || fl != kStatic2dFill) {
                g2dGuardFailed = true;
                PEERLOG("ctx2d: 2D context %p class %s GUARD REFUSED - D1 %#lx start %#lx copy %#lx fill %#lx (want %#lx %#lx %#lx %#lx) - "
                        "nothing patched this boot", c, flush_class(c), (unsigned long)d1, (unsigned long)st, (unsigned long)cp,
                        (unsigned long)fl, (unsigned long)kStatic2dD1, (unsigned long)kStatic2dStart, (unsigned long)kStatic2dCopy,
                        (unsigned long)kStatic2dFill);
            } else if (void **copy = static_cast<void **>(IOMalloc((k2dVtableSlots + kVtHeader) * sizeof(void *)))) {
                memcpy(copy, vt - kVtHeader, (k2dVtableSlots + kVtHeader) * sizeof(void *));
                gOrig2dCopy = reinterpret_cast<Blit2DCopyFn>(vt[k2dCopySlot]);
                gOrig2dFill = reinterpret_cast<Blit2DFillFn>(vt[k2dFillSlot]);
                copy[kVtHeader + k2dCopySlot] = reinterpret_cast<void *>(&hook_2d_copy);
                copy[kVtHeader + k2dFillSlot] = reinterpret_cast<void *>(&hook_2d_fill);
                // 0.0.279: the method-table hook, under its own exact guard; a refusal leaves the blit hooks in place.
                const uintptr_t tm = reinterpret_cast<uintptr_t>(vt[k2dTargetMethodSlot]) - g2dSlide;
                g2dStubAddr[0] = reinterpret_cast<uintptr_t>(&ctx2d_stub0);
                g2dStubAddr[1] = reinterpret_cast<uintptr_t>(&ctx2d_stub1);
                g2dStubAddr[2] = reinterpret_cast<uintptr_t>(&ctx2d_stub2);
                const bool anyOdd = ((g2dStubAddr[0] | g2dStubAddr[1] | g2dStubAddr[2]) & 1u) != 0;
                PEERLOG("ctx2d-call: stub addresses %#lx %#lx %#lx (%s - an odd one would be read as a virtual call)",
                        (unsigned long)g2dStubAddr[0], (unsigned long)g2dStubAddr[1], (unsigned long)g2dStubAddr[2],
                        anyOdd ? "ONE IS ODD: the method hook is NOT installed" : "all even");
                if (tm == kStatic2dTargetMethod && !anyOdd) {
                    gOrig2dTargetMethod = reinterpret_cast<TargetMethodFn>(vt[k2dTargetMethodSlot]);
                    copy[kVtHeader + k2dTargetMethodSlot] = reinterpret_cast<void *>(&hook_2d_target_method);
                    PEERLOG("ctx2d-call: slot 296 getTargetAndMethodForIndex %#lx = IOAccel2DContext2's (0x145858a8): selectors 0x100-0x102 "
                            "now counted per process (set_surface, finish, blit)", (unsigned long)tm);
                } else {
                    g2dTargetGuardRefused++;
                    PEERLOG("ctx2d-call: slot 296 is %#lx minus slide, not getTargetAndMethodForIndex 0x145858a8 - NOT hooked", (unsigned long)tm);
                }
                g2dOrigVt = vt;
                g2dCopyVt = copy + kVtHeader;
                PEERLOG("ctx2d: first 2D context %p class %s: GUARDS PASSED (D1, contextStart, blitCopy, blitFill); shared %u-slot copy, "
                        "slots 360/361 now ours (pass-through)", c, flush_class(c), k2dVtableSlots);
            }
        }
        IOLockUnlock(gFlushLock);
    }
    if (g2dCopyVt && *slot == g2dOrigVt) {
        __asm__ __volatile__("sfence" ::: "memory");
        *slot = g2dCopyVt;
        g2dPatched++;
        char nm[24] = { 0 }; proc_selfname(nm, (int)sizeof(nm));
        PEERLOG("ctx2d: 2D context %p (#%u minted) patched, created from pid %d (%s)", c, g2dMinted, proc_selfpid(), nm);
    }
    return c;
}

void *navi48_accel_object(void) { return gAccelObj; }
void **navi48_accel_vtable_copy(void) { return gAccelVt; }
// 0.0.286: the X6000 slide from the accelerator's own two anchors, as navi48_flushhook_control derives it.
uintptr_t navi48_x6000_slide(uint32_t *reason) {
    if (reason) *reason = 0;
    if (!gAccelVt || !gAccelObj) { if (reason) *reason = 1; return 0; }
    const uintptr_t start = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelStartSlot]);
    const uintptr_t slide = start - kStaticAccelStart;
    if (!flush_kptr(reinterpret_cast<void *>(start)) || (slide & 0xfff)) { if (reason) *reason = 2; return 0; }
    const uintptr_t ns = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelNewSurfaceSlot]);
    // The newSurface slot may already hold our flush hook; then the recorded original is the anchor.
    const uintptr_t nsOrig = (ns == reinterpret_cast<uintptr_t>(&hook_new_surface)) ? reinterpret_cast<uintptr_t>(gOrigNewSurface) : ns;
    if (nsOrig - slide != kStaticNewSurface) { if (reason) *reason = 3; return 0; }
    return slide;
}

// Folded into action 61 (`gfxcensus`): 1 installs new2DContext and arms the observe log, 2 stops logging (hooks stay,
// pass-through), 0 reads. out[0] installed, [1] mode, [2] minted, [3] patched, [4] blitCopy calls, [5] blitFill calls, [6] rects.
// 0.0.279, action 63 `finishread`: the 2D-context method counters. out[0] method hook installed | guard refusals << 8,
// [1] set_surface calls, [2] finish calls, [3] finish returns, [4] blit calls, [5] finish in flight, [6] WindowServer finish calls,
// [7] WindowServer finish returns, [8] longest finish us, [9] last finish arg | last kr << 32, [10] surface lines logged after a
// WindowServer finish returned, [11] contexts minted | patched << 32, [12] entry refusals.
uint32_t navi48_ctx2d_calls(uint64_t *out, unsigned count) {
    PEERLOG("ctx2d-call: hook %s; set_surface %llu, finish %llu (returned %llu, in flight %llu, longest %llu us, last arg %#llx kr %#x), "
            "blit %llu; WindowServer finish %llu returned %llu; post-return surface lines %u",
            gOrig2dTargetMethod ? "INSTALLED" : "not installed", (unsigned long long)g2dCalls[0], (unsigned long long)g2dCalls[1],
            (unsigned long long)g2dFinishReturns, (unsigned long long)(g2dCalls[1] - g2dFinishReturns), (unsigned long long)g2dFinishMaxUs,
            (unsigned long long)g2dLastFinishArg, g2dLastFinishKr, (unsigned long long)g2dCalls[2], (unsigned long long)g2dWsFinishCalls,
            (unsigned long long)g2dWsFinishReturns, g2dPostFinishSurfaceLines);
    PEERLOG("ctx2d-call: sync duration histogram (us) <100 %llu | <1000 %llu | <4000 %llu | <16000 %llu | <33000 %llu | <100000 %llu | "
            "<1000000 %llu | >=1s %llu; WindowServer sync interval (ms) <8 %llu | <17 %llu | <34 %llu | <100 %llu | <500 %llu | <1000 %llu | "
            "<5000 %llu | >=5s %llu", (unsigned long long)g2dFinishHist[0], (unsigned long long)g2dFinishHist[1],
            (unsigned long long)g2dFinishHist[2], (unsigned long long)g2dFinishHist[3], (unsigned long long)g2dFinishHist[4],
            (unsigned long long)g2dFinishHist[5], (unsigned long long)g2dFinishHist[6], (unsigned long long)g2dFinishHist[7],
            (unsigned long long)g2dWsIntervalHist[0], (unsigned long long)g2dWsIntervalHist[1], (unsigned long long)g2dWsIntervalHist[2],
            (unsigned long long)g2dWsIntervalHist[3], (unsigned long long)g2dWsIntervalHist[4], (unsigned long long)g2dWsIntervalHist[5],
            (unsigned long long)g2dWsIntervalHist[6], (unsigned long long)g2dWsIntervalHist[7]);
    for (unsigned i = 0; i < 8; i++)
        if (g2dPid[i].name[0])
            PEERLOG("ctx2d-call:   pid %d (%s): set_surface %llu, finish %llu returned %llu (longest %llu us), blit %llu", g2dPid[i].pid,
                    g2dPid[i].name, (unsigned long long)g2dPid[i].sel[0], (unsigned long long)g2dPid[i].sel[1],
                    (unsigned long long)g2dPid[i].finishReturns, (unsigned long long)g2dPid[i].finishMaxUs, (unsigned long long)g2dPid[i].sel[2]);
    const uint64_t v[13] = { (uint64_t)(gOrig2dTargetMethod ? 1u : 0u) | ((uint64_t)g2dTargetGuardRefused << 8), g2dCalls[0], g2dCalls[1],
                             g2dFinishReturns, g2dCalls[2], g2dCalls[1] - g2dFinishReturns, g2dWsFinishCalls, g2dWsFinishReturns,
                             g2dFinishMaxUs, g2dLastFinishArg | ((uint64_t)g2dLastFinishKr << 32), g2dPostFinishSurfaceLines,
                             (uint64_t)g2dMinted | ((uint64_t)g2dPatched << 32), g2dEntryRefused };
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return gOrig2dTargetMethod ? 1u : 0u;
}

uint32_t navi48_ctx2d_control(uint64_t arg, uint64_t *out, unsigned count) {
    if (!gFlushLock) gFlushLock = IOLockAlloc();
    if (arg == 1 && !g2dInstalled && gAccelVt && gAccelObj) {
        const uintptr_t start = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelStartSlot]);
        const uintptr_t slide = start - kStaticAccelStart;
        const uintptr_t nc = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelNew2DSlot]);
        if (flush_kptr(reinterpret_cast<void *>(start)) && !(slide & 0xfff) && nc - slide == kStaticNew2DContext) {
            g2dSlide = slide;
            gOrigNew2D = reinterpret_cast<New2DFn>(nc);
            __asm__ __volatile__("sfence" ::: "memory");
            gAccelVt[kVtHeader + kAccelNew2DSlot] = reinterpret_cast<void *>(&hook_new_2d);
            g2dInstalled = 1;
        }
        PEERLOG("ctx2d: install - slot 184 start %#lx -> slide %#lx; slot 327 %#lx minus slide %#lx (want new2DContext %#lx) -> %s",
                (unsigned long)start, (unsigned long)slide, (unsigned long)nc, (unsigned long)(nc - slide),
                (unsigned long)kStaticNew2DContext, g2dInstalled ? "INSTALLED" : "REFUSED");
    }
    if (arg == 1 && g2dInstalled) g2dMode = 1; else if (arg == 2) g2dMode = 0;
    const uint64_t v[7] = { g2dInstalled, g2dMode, g2dMinted, g2dPatched, g2dCopies, g2dFills, g2dRects };
    if (out) for (unsigned i = 0; i < count && i < 7; i++) out[i] = v[i];
    return g2dInstalled;
}

// =====================================================================================================
// 0.0.333 — `ucprobe`: THE DISPLAY-PIPE USER CLIENT'S OWN externalMethod, IN-KERNEL.
//
// WHY. saw userspace issue external method **selector 8** on a connection of our
// accelerator and get `kr=0`, while `pipe+0x2a4` never latched. That was read as "transactionEnd takes
// an early-success exit". The disassembly says it cannot be:
//
//   IOAccelDisplayPipeUserClient2::transactionEnd  0x145d3182  (CONFIRMED, symbol + bytes)
//     145d31e3  f6 83 91 01 00 00 01   test byte [rbx+0x191],1 ; jne -> continue
//                                      ... else  r13d = 0xe00002e2   RETURN (begin flag clear)
//     145d3203  mov r15,[rbx+0xe8]     ; je   -> r13d = 0xe00002d9   RETURN (no pipe)
//     145d3213  cmp byte [r15+0x299],0 ; jne  -> r13d = 0xe00002e3   RETURN   => 0 is the PASSING value
//     145d3221  cmp byte [r15+0x280],0 ; jne  -> r13d = 0xe00002d7   RETURN   => 0 is the PASSING value
//     145d323c  test byte [rax+0xc78],2; je   -> r13d = 0xe00002d8   RETURN   => bit 1 SET passes
//     145d3249  cmp byte [r15+0x282],0 ; jne  -> r13d = 0xe00002d8   RETURN   => 0 is the PASSING value
//     145d325a  call isActive 0x145cc03e = (pipe+0x298 != 0) ; je -> 0xe00002d8 => 1 passes
//     145d326e  eax = [r15+0x23c] - [r15+0x238] ; cmp 4 ; JNE -> 0x145d3399 = QUEUE
//     145d3399  call IOAccelDisplayPipe::transaction_end 0x145cd4b4 ; return ITS value
//
// So POLARITY IS SETTLED and the five bytes measured in
// (`accel+0xc78 = 2, pipe+0x280 = 0, +0x282 = 0, +0x298 = 1, +0x299 = 0`) **ALL PASS**, and
// **transactionEnd has NO early-success exit**: every exit that is not the queue returns a distinct
// non-zero code. The only way it returns 0 is through IOAccelDisplayPipe::transaction_end, which
// always calls transaction_queue -> transaction_queue_gated (0x145cf034), which latches +0x2a4.
// Therefore the observed kr=0 was NOT this function. The connection carrying it was connect type 5,
// and type 5 is IOAccelDevice2 (newUserClient jump table 0x145c0610), whose selector 8 is
// IOAccelDevice2::get_next_gid_group (method table 0x145f1bb0) - a different class, a different table.
//
// WHAT THIS INSTRUMENT DOES. Log only. It hooks the accelerator's own newUserClient (slot 239) on the
// per-instance vtable copy we already own, names EVERY user client's class IN-KERNEL from the object's
// own metaclass (never inferred from a connect type), and, for an IOAccelDisplayPipeUserClient2 only,
// swaps that instance's vptr for a copy whose slot 266 (externalMethod) is ours. The hook records the
// selector, the argument counts, the five gate bytes, the 4-slot ring indices, the transaction list and
// `+0x2a4` before and after, and - for selector 8, whose structIn is 0x118 - the transaction args
// themselves. It chains to Apple unconditionally, exactly as dpg_enableIrq does. No register is
// written, no behaviour changes.
//
// GUARDS, all against the ONE slide the accelerator's own two anchors give (slot 184 start 0xbdbddc2,
// slot 326 newSurface 0xbdc22b6):
//   accelerator slot 239 (byte 0x778) must read IOGraphicsAccelerator2::newUserClient 0x145bfe02;
//   the client's vtable must read D1 0x145d23a2 (slot 0), getMetaClass 0x145d23ec (slot 7),
//   externalMethod 0x145d2842 (slot 266) and clientClose 0x145d275c (slot 286), all minus that slide,
//   AND the object's own class name must be IOAccelDisplayPipeUserClient2. Anything else is counted
//   and left alone.
// =====================================================================================================
#define N48_UCPROBE_TOKEN "ucprobe-dpuc-externalmethod 0.0.333"

static constexpr unsigned  kAccelNewUserClientSlot = 239;          // byte 0x778
static constexpr uintptr_t kStaticNewUserClient    = 0x145bfe02ULL; // IOGraphicsAccelerator2::newUserClient
static constexpr unsigned  kDpUcVtableSlots        = 300;           // __ZTV29IOAccelDisplayPipeUserClient2 0x14600ab0, slot 300 NULL
static constexpr unsigned  kDpUcExtMethodSlot      = 266;           // byte 0x850
static constexpr uintptr_t kStaticDpUcD1           = 0x145d23a2ULL; // slot 0
static constexpr uintptr_t kStaticDpUcGetMeta      = 0x145d23ecULL; // slot 7
static constexpr uintptr_t kStaticDpUcExtMethod    = 0x145d2842ULL; // slot 266
static constexpr uintptr_t kStaticDpUcClientClose  = 0x145d275cULL; // slot 286
static const char * const  kDpUcClassName          = "IOAccelDisplayPipeUserClient2";

typedef IOReturn (*NewUserClientFn)(void *, task_t, void *, UInt32, IOUserClient **);
typedef IOReturn (*DpUcExtFn)(void *, uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *, void *);

static uint32_t         gUcMode { 0 }, gUcInstalled { 0 };
static uintptr_t        gUcSlide { 0 };
static NewUserClientFn  gOrigNewUserClient { nullptr };
static void           **gDpUcOrigVt { nullptr }, **gDpUcCopyVt { nullptr };
static DpUcExtFn        gOrigDpUcExt { nullptr };
static uint32_t         gUcMinted { 0 }, gUcForeign { 0 }, gUcFailed { 0 };
static uint32_t         gDpUcSeen { 0 }, gDpUcPatched { 0 }, gDpUcGuardRefused { 0 };
static uint32_t         gDpUcCalls { 0 }, gDpUcSel4 { 0 }, gDpUcSel8 { 0 }, gDpUcLatched { 0 };
static uint32_t         gDpUcSelMask { 0 };          // bit n set = selector n seen (n < 32)
static uint32_t         gDpUcLastSel { 0 }, gDpUcLastKr { 0 }, gDpUcLast2a4 { 0 };
static uint32_t         gUcLines { 0 }, gUcArgLines { 0 }, gUcCensusLines { 0 };
static constexpr uint32_t kUcLineBudget = 192, kUcArgBudget = 6, kUcCensusBudget = 48;

// =====================================================================================================
// 0.0.335 — NAME THE SLOT. Selector 3 `request_notify` carries a 24-byte structIn and the
// dword at structIn+0x10 IS the slot selector. CONFIRMED from the bytes, twice:
//
//   IOAccelDisplayPipeUserClient2::s_request_notify  0x145d1e3e
//     145d1e42  mov  rsi,[rdx+0x10]      ; args->asyncReference
//     145d1e46  test rsi,rsi ; je  -> 0xe00002c2                     (no async reference -> refuse)
//     145d1e4b  mov  rdx,[rdx+0x30]      ; args->structureInput
//     145d1e4f  cmp  dword [rdx+0x10],1
//     145d1e53  ja   -> 0xe00002c2       ; so structIn+0x10 is 0 or 1 AND NOTHING ELSE
//     145d1e56  jmp  requestNotify(asyncReference, structureInput)
//
//   IOAccelDisplayPipeUserClient2::requestNotify    0x145d2c0a
//     145d2c1f  lea  rax,[rdi+0x140]     ; record 1
//     145d2c26  lea  rcx,[rdi+0xf8]      ; record 0
//     145d2c2d  cmp  dword [rdx+0x10],0
//     145d2c31  cmove rax,rcx            ; structIn+0x10 == 0 -> record 0, else record 1
//     ... -> IOAccelDisplayPipe::request_notify(asyncRef, {record, structIn}) -> request_notify_gated
//
//   IOAccelDisplayPipeUserClient2::init            (uc+0x138 / uc+0x180 = each record's +0x40)
//     145d2517  mov  dword [r14+0x138],0 ; record0+0x40 = 0   <- THE ARMING KIND
//     145d252b  mov  dword [r14+0x180],1 ; record1+0x40 = 1   <- NEVER ARMS
//
//   IOAccelDisplayPipe::request_notify_gated       0x145cfd34
//     145cfd4a  cmp  qword [r12+0x10],0 ; jne -> 0xe00002d5  (a request is already outstanding)
//     145cfd92  cmp  dword [r12+0x40],0
//     145cfd98  je   -> 0x145cfdc8       ; ARMING path: link onto pipe+0x258/+0x260, then
//               145cfdf8  cmp dword [rbx+0x2a0],0 ; jne -> skip
//               145cfe01  mov dword [rbx+0x2a0],1
//               145cfe0b  call [pipe vtable +0x878] = slot 271 enableVBLInterrupt
//               ^^ NOTE: the enable call happens ONLY on the 0 -> 1 edge of +0x2a0. A slot-0 registration
//                  taken while pipe+0xd8 is still NULL therefore burns the edge for the whole boot.
//     (record+0x40 != 0) fall-through 145cfd9a: zero record+0x28/+0x30 and RETURN 0 - no link, NO ARM.
//
// So `structIn+0x10` decides everything, and this block LOGS it. No write, no behaviour change.
// =====================================================================================================
#define N48_REQNOTIFY_TOKEN "reqnotify-slot-structin-0x10 0.0.335"

static constexpr unsigned kUcRecord0Off = 0xf8;   // 145d2c26
static constexpr unsigned kUcRecord1Off = 0x140;  // 145d2c1f
static constexpr unsigned kUcRecordKindOff = 0x40; // record+0x40, tested at 145cfd92

static uint32_t gRnLines { 0 }, gVblLines { 0 };
static constexpr uint32_t kRnBudget = 10, kVblBudget = 40;
static uint32_t gRnCalls { 0 }, gRnSlot0 { 0 }, gRnSlot1 { 0 }, gRnSlotOther { 0 };
static uint32_t gRnCookieLiveBefore { 0 };          // sel-3 calls whose pipe+0xd8 was ALREADY non-NULL
static uint32_t gRnLastSlot { 0xffffffff }, gRnLastKind { 0xffffffff };
static uint32_t gRnLastCookieB { 0 }, gRnLastCookieA { 0 }, gRnLastArmB { 0 }, gRnLastArmA { 0 };
static uint32_t gUcMaxArm { 0 };                    // the largest pipe+0x2a0 ever seen at this hook
static uint32_t gUcCookieEverLive { 0 };

// Read the seven pipe fields the VBL path turns on. Every one is a LOAD. Nothing here writes.
struct ucp_vbl_state {
    void *cookie, *reqHead, *reqTail, *pending, *live;
    uint32_t arm, transactIR; uint8_t txnOverVbl;
};
static void ucp_read_vbl(const void *pipe, ucp_vbl_state *s) {
    *s = ucp_vbl_state { nullptr, nullptr, nullptr, nullptr, nullptr, 0, 0, 0 };
    if (!pipe || !flush_kptr(pipe)) return;
    const char *p = static_cast<const char *>(pipe);
    s->cookie     = *reinterpret_cast<void *const *>(p + 0xd8);
    s->pending    = *reinterpret_cast<void *const *>(p + 0x248);   // Apple's name `Pending` (triage fmt 0x145e2aeb)
    s->live       = *reinterpret_cast<void *const *>(p + 0x250);   // Apple's name `Live`
    s->reqHead    = *reinterpret_cast<void *const *>(p + 0x258);
    s->reqTail    = *reinterpret_cast<void *const *>(p + 0x260);
    s->txnOverVbl = (uint8_t)p[0x281];
    s->arm        = *reinterpret_cast<const uint32_t *>(p + 0x2a0);
    s->transactIR = *reinterpret_cast<const uint32_t *>(p + 0x2a4);  // Apple's name `TransactIR`
}

static const char *ucp_dp_sel_name(uint32_t s) {
    switch (s) {
    case 0:  return "set_pipe_index";      case 1:  return "get_display_mode_scaler";
    case 2:  return "get_capabilities_data"; case 3: return "request_notify";
    case 4:  return "TRANSACTION_BEGIN";   case 5:  return "set_plane_gamma_table";
    case 6:  return "set_pipe_pregamma";   case 7:  return "set_pipe_postgamma";
    case 8:  return "TRANSACTION_END";     case 9:  return "transaction_wait";
    case 10: return "set_pipe_precsclin";  case 11: return "set_pipe_postcscgamma";
    case 12: return "copy_surface";        case 13: return "triage";
    default: return "(out of table)";
    }
}

// The exit transactionEnd WILL take, decided from the SAME conditions the code tests, in the code's own
// order. Pure: reads nothing, decides nothing, changes nothing - its only job is to make the prediction
// falsifiable against the kern_return_t the very next call produces.
static const char *ucp_predict_exit(uint32_t begin, const void *pipe, uint8_t b299, uint8_t b280,
                                    uint8_t c78, uint8_t b282, uint8_t b298, uint32_t idx3c, uint32_t idx38,
                                    const void *txnHead, uint32_t *krOut) {
    if (!(begin & 1))          { *krOut = 0xe00002e2; return "0xe00002e2 begin flag clear (145d31e3)"; }
    if (!pipe)                 { *krOut = 0xe00002d9; return "0xe00002d9 uc+0xe8 NULL (145d320d)"; }
    if (b299)                  { *krOut = 0xe00002e3; return "0xe00002e3 pipe+0x299 non-zero (145d321b)"; }
    if (b280)                  { *krOut = 0xe00002d7; return "0xe00002d7 pipe+0x280 non-zero (145d3229)"; }
    if (!(c78 & 2))            { *krOut = 0xe00002d8; return "0xe00002d8 accel+0xc78 bit1 clear (145d3243)"; }
    if (b282)                  { *krOut = 0xe00002d8; return "0xe00002d8 pipe+0x282 non-zero (145d3251)"; }
    if (!b298)                 { *krOut = 0xe00002d8; return "0xe00002d8 isActive() false (145d3268)"; }
    if (idx3c - idx38 == 4)    { *krOut = 0;          return "RING FULL - waits at 145d3285, then retries"; }
    if (!txnHead)              { *krOut = 0xe00002f0; return "QUEUE PATH, but pipe+0xf8 is NULL -> transaction_end returns 0xe00002f0 (EMPTY)"; }
    *krOut = 0;
    return "QUEUE PATH (145d3399) -> transaction_end -> transaction_queue -> +0x2a4 MUST latch";
}

static void ucp_dump_txn_args(const void *p, size_t len) {
    if (!flush_kptr(p) || gUcArgLines >= kUcArgBudget) return;
    gUcArgLines++;
    const uint64_t *q = static_cast<const uint64_t *>(p);
    const uint32_t id = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(p) + 0x10);
    PEERLOG("ucprobe:   TXNARGS %p len %llu  id(+0x10) %#x  [the field IOAccelDisplayPipe::transaction_end "
            "matches against transaction+0x54 at 145cd4e5]", p, (unsigned long long)len, id);
    const size_t words = (len > 0x118 ? 0x118 : len) / 8;
    for (size_t i = 0; i < words; i += 4)
        PEERLOG("ucprobe:   TXNARGS +%#04llx  %#018llx %#018llx %#018llx %#018llx", (unsigned long long)(i * 8),
                (unsigned long long)q[i], (unsigned long long)(i + 1 < words ? q[i + 1] : 0),
                (unsigned long long)(i + 2 < words ? q[i + 2] : 0), (unsigned long long)(i + 3 < words ? q[i + 3] : 0));
}

static IOReturn ucp_hook_ext(void *self, uint32_t selector, IOExternalMethodArguments *args,
                             IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    const bool ours = gDpUcCopyVt && *reinterpret_cast<void ***>(self) == gDpUcCopyVt + kVtHeader;
    const char *cls = flush_class(self);
    const bool ident = ours && cls && !strcmp(cls, kDpUcClassName);
    gDpUcCalls++;
    if (selector == 4) gDpUcSel4++;
    if (selector == 8) gDpUcSel8++;
    if (selector < 32) gDpUcSelMask |= (1u << selector);
    gDpUcLastSel = selector;

    const char *c = static_cast<const char *>(self);
    void *accel = *reinterpret_cast<void *const *>(c + 0xd8);
    void *pipe  = *reinterpret_cast<void *const *>(c + 0xe8);
    const uint8_t begin = *reinterpret_cast<const uint8_t *>(c + 0x191);
    const uint8_t c78 = (accel && flush_kptr(accel)) ? *reinterpret_cast<const uint8_t *>(static_cast<const char *>(accel) + 0xc78) : 0xff;
    uint8_t b280 = 0xff, b282 = 0xff, b298 = 0xff, b299 = 0xff;
    uint32_t i38 = 0, i3c = 0, a4pre = 0xffffffff;
    void *txnHead = nullptr; uint32_t txnId0 = 0; uint32_t txnCount = 0;
    if (pipe && flush_kptr(pipe)) {
        const char *p = static_cast<const char *>(pipe);
        b280 = *reinterpret_cast<const uint8_t *>(p + 0x280); b282 = *reinterpret_cast<const uint8_t *>(p + 0x282);
        b298 = *reinterpret_cast<const uint8_t *>(p + 0x298); b299 = *reinterpret_cast<const uint8_t *>(p + 0x299);
        i38 = *reinterpret_cast<const uint32_t *>(p + 0x238); i3c = *reinterpret_cast<const uint32_t *>(p + 0x23c);
        a4pre = *reinterpret_cast<const uint32_t *>(p + 0x2a4);
        txnHead = *reinterpret_cast<void *const *>(p + 0xf8);
        for (void *t = txnHead; t && flush_kptr(t) && txnCount < 8; txnCount++) {
            if (txnCount == 0) txnId0 = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(t) + 0x54);
            t = *reinterpret_cast<void *const *>(static_cast<const char *>(t) + 0x10);
        }
    }
    uint32_t predKr = 0;
    const char *pred = (selector == 8)
        ? ucp_predict_exit(begin, pipe, b299, b280, c78, b282, b298, i3c, i38, txnHead, &predKr) : nullptr;

    const bool logIt = gUcMode && gUcLines < kUcLineBudget;
    if (logIt) {
        gUcLines++;
        PEERLOG("ucprobe: CALL uc %p class %s%s sel %u (%s) | scalarIn %u structIn %u scalarOut %u structOut %u | "
                "begin(uc+0x191) %#x pipe(uc+0xe8) %p accel(uc+0xd8) %p", self, cls, ident ? "" : " [IDENTITY MISMATCH]",
                selector, ucp_dp_sel_name(selector), args ? args->scalarInputCount : 0,
                args ? args->structureInputSize : 0, args ? args->scalarOutputCount : 0,
                args ? args->structureOutputSize : 0, begin, pipe, accel);
        PEERLOG("ucprobe:   GATES accel+0xc78 %#x (bit1 %u) | pipe +0x280 %u +0x282 %u +0x298 %u +0x299 %u | "
                "ring +0x238 %u +0x23c %u (diff %u) | +0x2a4 BEFORE %u | txn list head %p count %u first id %#x",
                c78, (unsigned)((c78 >> 1) & 1), b280, b282, b298, b299, i38, i3c, i3c - i38, a4pre,
                txnHead, txnCount, txnId0);
        if (pred) PEERLOG("ucprobe:   PREDICTED EXIT: %s  (expected kr %#x)", pred, predKr);
    }
    if (selector == 8 && args && args->structureInput && args->structureInputSize)
        ucp_dump_txn_args(args->structureInput, args->structureInputSize);

    // 0.0.335 — the VBL state at the moment of the call, and for selector 3 the SLOT.
    // Counters are kept unconditionally so a truncated log cannot lose the answer; only the PEERLOGs
    // are budgeted. (token: N48_REQNOTIFY_TOKEN)
    ucp_vbl_state vb; ucp_read_vbl(pipe, &vb);
    if (vb.cookie) gUcCookieEverLive = 1;
    if (vb.arm > gUcMaxArm) gUcMaxArm = vb.arm;
    const bool vblSel = (selector == 3 || selector == 4 || selector == 8);
    if (gUcMode && vblSel && gVblLines < kVblBudget) {
        gVblLines++;
        PEERLOG("ucprobe:   VBL BEFORE sel %u | pipe+0xd8 cookie %p (%s) | +0x2a0 arm %u | +0x2a4 TransactIR %u | "
                "+0x248 Pending %p | +0x250 Live %p | +0x258 head %p +0x260 tail %p (%s) | +0x281 txn-over-vbl %u",
                selector, vb.cookie, vb.cookie ? "a VBL source EXISTS" : "NULL - enableVBLInterrupt is a no-op",
                vb.arm, vb.transactIR, vb.pending, vb.live, vb.reqHead, vb.reqTail,
                (vb.reqTail == static_cast<const void *>(static_cast<const char *>(pipe) + 0x258) || !vb.reqTail)
                    ? "EMPTY (the tail points at the head itself)" : "a request IS linked",
                (unsigned)vb.txnOverVbl);
    }
    if (selector == 3) {
        gRnCalls++;
        const void *si = args ? args->structureInput : nullptr;
        const uint32_t siLen = args ? args->structureInputSize : 0;
        uint32_t slot = 0xffffffff, kind = 0xffffffff;
        const char *recName = "(unreadable structIn)";
        const void *rec = nullptr;
        if (si && siLen >= 0x18 && flush_kptr(si)) {
            slot = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(si) + 0x10);
            rec  = static_cast<const char *>(self) + (slot == 0 ? kUcRecord0Off : kUcRecord1Off);
            kind = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(rec) + kUcRecordKindOff);
            recName = (slot == 0) ? "record 0 at uc+0xf8  -> +0x40 is 0 at init (145d2517): THE ARMING KIND"
                                  : "record 1 at uc+0x140 -> +0x40 is 1 at init (145d252b): NEVER ARMS, NEVER LINKS";
            if (slot == 0) gRnSlot0++; else if (slot == 1) gRnSlot1++; else gRnSlotOther++;
        }
        if (vb.cookie) gRnCookieLiveBefore++;
        gRnLastSlot = slot; gRnLastKind = kind;
        gRnLastCookieB = vb.cookie != nullptr; gRnLastArmB = vb.arm;
        if (gUcMode && gRnLines < kRnBudget && si && siLen && flush_kptr(si)) {
            gRnLines++;
            const uint64_t *q = static_cast<const uint64_t *>(si);
            const uint32_t words = (siLen > 0x18 ? 0x18 : siLen) / 8;
            PEERLOG("ucprobe:   REQNOTIFY structIn %p len %u raw %#018llx %#018llx %#018llx | asyncRef %p "
                    "[" N48_REQNOTIFY_TOKEN "]", si, siLen,
                    (unsigned long long)(words > 0 ? q[0] : 0), (unsigned long long)(words > 1 ? q[1] : 0),
                    (unsigned long long)(words > 2 ? q[2] : 0), args ? (void *)args->asyncReference : nullptr);
            PEERLOG("ucprobe:   REQNOTIFY *** SLOT SELECTOR structIn+0x10 = %u *** -> %s; that record is at %p and its "
                    "+0x40 reads %u (145cfd92 takes the NON-ARMING early-out on non-zero); record+0x10 (already-pending "
                    "gate at 145cfd4a) %p; pipe+0xd8 cookie %p, pipe+0x2a0 arm %u BEFORE this call",
                    slot, recName, rec, kind,
                    rec && flush_kptr(rec) ? *reinterpret_cast<void *const *>(static_cast<const char *>(rec) + 0x10) : nullptr,
                    vb.cookie, vb.arm);
        }
    }

    const IOReturn kr = gOrigDpUcExt ? gOrigDpUcExt(self, selector, args, dispatch, target, reference)
                                     : kIOReturnUnsupported;

    uint32_t a4post = 0xffffffff, beginPost = 0xff;
    if (pipe && flush_kptr(pipe)) a4post = *reinterpret_cast<const uint32_t *>(static_cast<const char *>(pipe) + 0x2a4);
    beginPost = *reinterpret_cast<const uint8_t *>(c + 0x191);
    if (a4post && a4post != 0xffffffff) gDpUcLatched++;
    gDpUcLastKr = (uint32_t)kr; gDpUcLast2a4 = a4post;
    // 0.0.335 — the same seven fields AFTER the call. `+0x2a0 arm 0 -> 1` is the arm actually happening;
    // `+0x2a4 TransactIR` going non-zero is the STOP condition of this brief.
    ucp_vbl_state va; ucp_read_vbl(pipe, &va);
    if (va.cookie) gUcCookieEverLive = 1;
    if (va.arm > gUcMaxArm) gUcMaxArm = va.arm;
    if (selector == 3) { gRnLastCookieA = va.cookie != nullptr; gRnLastArmA = va.arm; }
    if (gUcMode && vblSel && gVblLines < kVblBudget) {
        gVblLines++;
        PEERLOG("ucprobe:   VBL AFTER  sel %u kr %#x | pipe+0xd8 cookie %p | +0x2a0 arm %u (was %u)%s | +0x2a4 "
                "TransactIR %u (was %u)%s | +0x248 Pending %p | +0x250 Live %p | +0x258/+0x260 %p / %p",
                selector, (unsigned)kr, va.cookie, va.arm, vb.arm,
                (va.arm != vb.arm) ? "  *** THE VBL ARM HAPPENED ***" : "  (unchanged - request_notify_gated never reached 145cfe01)",
                va.transactIR, vb.transactIR,
                (va.transactIR != vb.transactIR) ? "  *** TransactIR ARMED - FIRST TRANSACTION ***" : "",
                va.pending, va.live, va.reqHead, va.reqTail);
    }
    if (logIt)
        PEERLOG("ucprobe:   RETURN sel %u kr %#x | +0x2a4 AFTER %u (was %u)%s | begin AFTER %#x | ring +0x23c %u", selector,
                (unsigned)kr, a4post, a4pre, (a4post != a4pre) ? "  *** LATCHED ***" : "", beginPost,
                (pipe && flush_kptr(pipe)) ? *reinterpret_cast<const uint32_t *>(static_cast<const char *>(pipe) + 0x23c) : 0);
    if (selector == 8 && pred && (uint32_t)kr != predKr && gUcLines < kUcLineBudget) {
        gUcLines++;
        PEERLOG("ucprobe:   *** PREDICTION MISSED: predicted %#x, got %#x - the decode of 145d3182 is wrong "
                "or a condition moved between the read and the call ***", predKr, (unsigned)kr);
    }
    return kr;
}

// Build the one shared vtable copy for IOAccelDisplayPipeUserClient2 and put slot 266 in it. Every guard is
// against gUcSlide, which came from the accelerator's own two anchors before any of this ran.
static bool ucp_build_dpuc_copy(void *uc) {
    if (gDpUcCopyVt) return true;
    void **vt = *reinterpret_cast<void ***>(uc);
    if (!flush_kptr(vt)) { gDpUcGuardRefused++; PEERLOG("ucprobe: REFUSED - client %p has no kernel vtable", uc); return false; }
    struct { unsigned slot; uintptr_t want; const char *name; } g[] = {
        { 0,                   kStaticDpUcD1,          "~IOAccelDisplayPipeUserClient2 D1" },
        { 7,                   kStaticDpUcGetMeta,     "getMetaClass" },
        { kDpUcExtMethodSlot,  kStaticDpUcExtMethod,   "externalMethod" },
        { 286,                 kStaticDpUcClientClose, "clientClose" },
    };
    for (unsigned i = 0; i < sizeof(g) / sizeof(g[0]); i++) {
        const uintptr_t got = reinterpret_cast<uintptr_t>(vt[g[i].slot]) - gUcSlide;
        if (got != g[i].want) {
            gDpUcGuardRefused++;
            PEERLOG("ucprobe: REFUSED - client %p vtable slot %u (%s) = %#lx minus slide %#lx, want %#lx", uc,
                    g[i].slot, g[i].name, (unsigned long)got, (unsigned long)gUcSlide, (unsigned long)g[i].want);
            return false;
        }
    }
    void **copy = static_cast<void **>(IOMalloc((kDpUcVtableSlots + kVtHeader) * sizeof(void *)));
    if (!copy) { gDpUcGuardRefused++; PEERLOG("ucprobe: REFUSED - IOMalloc of the %u-slot copy failed", kDpUcVtableSlots); return false; }
    memcpy(copy, vt - kVtHeader, (kDpUcVtableSlots + kVtHeader) * sizeof(void *));
    gOrigDpUcExt = reinterpret_cast<DpUcExtFn>(vt[kDpUcExtMethodSlot]);
    copy[kVtHeader + kDpUcExtMethodSlot] = reinterpret_cast<void *>(&ucp_hook_ext);
    gDpUcOrigVt = vt;
    __asm__ __volatile__("sfence" ::: "memory");
    gDpUcCopyVt = copy;   // published last: a reader sees either nullptr or a complete table
    PEERLOG("ucprobe: client %p vtable GUARDS PASSED (D1 %#lx, getMetaClass %#lx, externalMethod %#lx, clientClose %#lx, "
            "one slide %#lx); %u-slot copy built, slot %u now ours (%s)", uc, (unsigned long)kStaticDpUcD1,
            (unsigned long)kStaticDpUcGetMeta, (unsigned long)kStaticDpUcExtMethod, (unsigned long)kStaticDpUcClientClose,
            (unsigned long)gUcSlide, kDpUcVtableSlots, kDpUcExtMethodSlot, N48_UCPROBE_TOKEN);
    return true;
}

// =====================================================================================================================
// build 0.0.503 (an internal design note and) — SWITCH 68, THE HYBRID: WindowServer on the GPU,
// every other process kept off it. The slot-239 hook is installed at tryPatchAcceleratorStop time (ucp_install_at_start,
// inside the accelerator's own start(), before it registers), so it sees every client from the first. The policy is
// hybrid_policy.h's n48_hy_decide (host-tested): switch 68 OFF admits everyone and the hook calls Apple exactly as before;
// ON, a caller that is not named "WindowServer" with uid 88 is REFUSED with kIOReturnNotPermitted and *handler NULL BEFORE
// Apple's newUserClient is called. The switch is read ONCE per call (`on`), and that value feeds the decision and the
// counters. It writes no register, no page table and nothing of Apple's: the refusal writes only the caller's out-pointer.
// Until the `ucprobe 1` verb asks, the 0.0.333 census below stays off (gUcInstalled 0), exactly as when the verb installed it.
// =====================================================================================================================
static volatile uint32_t gHyOn { 0u };              // switch 68; OFF at boot
static uint32_t gUcHooked { 0u };                   // 1 once slot 239 is ours (at accelerator start, or by the verb)
static uint32_t gHyRegisteredAtInstall { 0xffu };   // the accelerator's registered bit when the hook went in (0xff: never)
static n48_hy_counts gHyC {};
static uint64_t gHyOwnerMismatch { 0u };            // newUserClient's owner task is not the calling thread's task (counted)
static uint32_t gHyRefuseLines { 0u }, gHyAdmitLines { 0u };
static constexpr uint32_t kHyRefuseLineBudget = 48u, kHyAdmitLineBudget = 8u;

static IOReturn ucp_hook_new_user_client(void *accel, task_t owner, void *sid, UInt32 type, IOUserClient **handler) {
    if (accel == gAccelObj) {
        const uint32_t on = gHyOn;                   // LATCHED: one reading of switch 68 for this whole call
        char nm[33]; nm[0] = '\0';
        proc_selfname(nm, (int)sizeof(nm));
        const uint32_t uid = (uint32_t)kauth_getuid();
        if (owner != current_task()) gHyOwnerMismatch++;
        const uint32_t dec = n48_hy_decide(nm, uid, on);
        n48_hy_count(&gHyC, dec, nm, uid, on);
        if (dec == N48_HY_REFUSE) {
            if (handler) *handler = nullptr;
            if (gHyRefuseLines < kHyRefuseLineBudget) {
                gHyRefuseLines++;
                PEERLOG(N48_HY_REFUSE_FMT, proc_selfpid(), nm, uid, (unsigned)type, (unsigned long long)gHyC.refused);
            }
            return kIOReturnNotPermitted;            // BEFORE Apple: nothing of the accelerator's is touched
        }
        if (on && gHyAdmitLines < kHyAdmitLineBudget) {
            gHyAdmitLines++;
            PEERLOG(N48_HY_ADMIT_FMT, proc_selfpid(), uid, (unsigned)type, (unsigned long long)gHyC.ws);
        }
    }
    const IOReturn kr = gOrigNewUserClient ? gOrigNewUserClient(accel, owner, sid, type, handler)
                                           : kIOReturnUnsupported;
    if (!gUcInstalled) return kr;                    // build 0.0.503: the 0.0.333 census starts at `ucprobe 1`, as before
    gUcMinted++;
    if (accel != gAccelObj) { gUcForeign++; return kr; }
    IOUserClient *uc = handler ? *handler : nullptr;
    if (kr != kIOReturnSuccess || !uc || !flush_kptr(uc)) { gUcFailed++; return kr; }
    const char *cls = flush_class(uc);
    const bool isDp = cls && !strcmp(cls, kDpUcClassName);
    if (isDp) gDpUcSeen++;
    if (gUcCensusLines < kUcCensusBudget) {
        gUcCensusLines++;
        PEERLOG("ucprobe: NEWUSERCLIENT #%u type %u -> %p class %s (kr %#x)%s", gUcMinted, (unsigned)type, uc, cls,
                (unsigned)kr, isDp ? "   <<< THE DISPLAY-PIPE USER CLIENT" : "");
    }
    if (!isDp || !gUcMode) return kr;
    if (!ucp_build_dpuc_copy(uc)) return kr;
    void ***slot = reinterpret_cast<void ***>(uc);
    if (*slot == gDpUcCopyVt + kVtHeader) return kr;
    if (*slot != gDpUcOrigVt) { gDpUcGuardRefused++;
        PEERLOG("ucprobe: client %p carries a foreign vtable %p (not %p) - NOT patched", uc, *slot, gDpUcOrigVt); return kr; }
    __asm__ __volatile__("sfence" ::: "memory");
    *slot = gDpUcCopyVt + kVtHeader;
    if (*slot == gDpUcCopyVt + kVtHeader) {
        gDpUcPatched++;
        PEERLOG("ucprobe: client %p PATCHED - externalMethod (slot %u) is now ours, pass-through", uc, kDpUcExtMethodSlot);
    }
    return kr;
}

// build 0.0.503 — called from tryPatchAcceleratorStop with Apple's vtable `vt` and our unpublished `copy` of it. The
// slide comes from the same two anchors as navi48_x6000_slide (start, slot 184; newSurface, slot 326); slot 239 must then be
// IOGraphicsAccelerator2::newUserClient (0x145bfe02, the ucprobe verb's own guard, CONFIRMED on hardware: "slot 239
// 0xffffff7fb18a4e02 minus slide = 0x145bfe02"). Any mismatch leaves slot 239 Apple's (the hybrid is then INERT and says so).
static void ucp_install_at_start(IOService *accel, void **vt, void **copy) {
    gHyRegisteredAtInstall = (accel && (accel->getState() & kIOServiceRegisteredState)) ? 1u : 0u;
    const uintptr_t start = reinterpret_cast<uintptr_t>(vt[kAccelStartSlot]);
    const uintptr_t slide = start - kStaticAccelStart;
    const uintptr_t ns    = reinterpret_cast<uintptr_t>(vt[kAccelNewSurfaceSlot]);
    const uintptr_t nuc   = reinterpret_cast<uintptr_t>(vt[kAccelNewUserClientSlot]);
    const char *why = nullptr;
    if (gUcHooked) why = "already installed";
    else if (!flush_kptr(reinterpret_cast<void *>(start)) || (slide & 0xfff)) why = "slide (start anchor)";
    else if (ns - slide != kStaticNewSurface) why = "slide (newSurface anchor)";
    else if (nuc - slide != kStaticNewUserClient) why = "slot 239 is not newUserClient";
    if (!why) {
        gUcSlide = slide;
        gOrigNewUserClient = reinterpret_cast<NewUserClientFn>(nuc);
        copy[kVtHeader + kAccelNewUserClientSlot] = reinterpret_cast<void *>(&ucp_hook_new_user_client);
        gUcHooked = 1u;
    }
    PEERLOG("hybrid68: slot-239 hook at accelerator start - accelerator %p %s; slide %#lx; slot 239 %#lx minus slide = %#lx "
            "(want IOGraphicsAccelerator2::newUserClient %#lx) -> %s%s; switch 68 %s [" N48_HY_TOKEN "]", accel,
            gHyRegisteredAtInstall ? "ALREADY REGISTERED" : "not yet registered", (unsigned long)slide, (unsigned long)nuc,
            (unsigned long)(nuc - slide), (unsigned long)kStaticNewUserClient, why ? "NOT INSTALLED: " : "INSTALLED in the copy",
            why ? why : "", gHyOn ? "ON" : "OFF");
}

// build 0.0.503 — switch 68's three entry points for hw_hook_gfx_neuter (AppleHardwareHook.cpp), which owns the
// verb and its mid-arm guard. Declared in Navi48Ttl.hpp.
uint32_t navi48_hybrid_get(void) { return gHyOn; }
void navi48_hybrid_set(uint32_t on) { gHyOn = on ? 1u : 0u; }
void navi48_hybrid_report(const char *how) {
    PEERLOG(N48_HY_REPORT_FMT, gHyOn ? "ON" : "OFF (default)", how ? how : "", (gHyOn && !gUcHooked) ? N48_HY_INERT_TXT : "",
            gUcHooked ? "INSTALLED" : "NOT installed",
            gHyRegisteredAtInstall == 0xffu ? "never seen" : gHyRegisteredAtInstall ? "ALREADY REGISTERED" : "not yet registered",
            (unsigned long long)gHyC.ws, (unsigned long long)gHyC.others_off, (unsigned long long)gHyC.others_on,
            (unsigned long long)gHyC.refused, (unsigned long long)gHyOwnerMismatch);
}

// action 81 `ucprobe [0|1|2]`: 0 reads, 1 installs + logs, 2 stops logging (the hook stays, pass-through).
// out[0] status (0 ok, 1 no accelerator vtable, 2 slide, 3 slot 239 is not newUserClient, 5 bad argument),
// [1] installed | mode<<32, [2] slide, [3] clients minted | foreign<<32, [4] failed | census lines<<32,
// [5] display-pipe clients SEEN | PATCHED<<32, [6] guard refusals | sel-3 calls<<16 | slot-0<<32 | slot-1<<48,
// [7] externalMethod calls,
// [8] selector 4 count | selector 8 count<<32, [9] selector bitmask (bit n = selector n seen),
// [10] calls that saw +0x2a4 non-zero afterwards | (0.0.335) last sel-3 cookie-before<<32, cookie-after<<33,
//      arm-before-nonzero<<34, arm-after-nonzero<<35, (last slot selector + 1)<<36 (4 b), last record+0x40<<40 (8 b),
//      max pipe+0x2a0 ever seen<<48 (8 b), sel-3 calls with the cookie ALREADY live<<56 (8 b),
// [11] last selector | last kr<<32, [12] last +0x2a4 | (0.0.335) cookie ever non-NULL<<32.
uint32_t navi48_ucprobe_control(uint64_t arg, uint64_t *out, unsigned count) {
    uint32_t st = 0;
    if (!gFlushLock) gFlushLock = IOLockAlloc();
    if (arg != 0 && arg != 1 && arg != 2) st = 5;
    if (!st && arg == 1 && !gUcInstalled && gUcHooked) {
        // build 0.0.503: slot 239 has been ours since the accelerator's start() (ucp_install_at_start); the verb now
        // only starts the census, which is what its install did before.
        gUcInstalled = 1;
        PEERLOG("ucprobe: install - slot 239 already ours since accelerator start (slide %#lx) -> INSTALLED (census on)",
                (unsigned long)gUcSlide);
    }
    if (!st && arg == 1 && !gUcInstalled) {
        if (!gAccelVt || !gAccelObj) {
            st = 1;
            PEERLOG("ucprobe: REFUSED - no accelerator vtable copy (accelerator %p, table %p)", gAccelObj, gAccelVt);
        } else {
            uint32_t sr = 0;
            const uintptr_t slide = navi48_x6000_slide(&sr);
            const uintptr_t nuc = reinterpret_cast<uintptr_t>(gAccelVt[kVtHeader + kAccelNewUserClientSlot]);
            if (!slide) {
                st = 2;
            } else if (nuc - slide != kStaticNewUserClient) {
                st = 3;
            } else {
                gUcSlide = slide;
                gOrigNewUserClient = reinterpret_cast<NewUserClientFn>(nuc);
                __asm__ __volatile__("sfence" ::: "memory");
                gAccelVt[kVtHeader + kAccelNewUserClientSlot] = reinterpret_cast<void *>(&ucp_hook_new_user_client);
                gUcInstalled = 1;
                gUcHooked = 1u;   // build 0.0.503: the start-time install was refused; the verb's late one carries switch 68 too
            }
            PEERLOG("ucprobe: install - accelerator %p slide %#lx (reason %u); slot %u %#lx minus slide = %#lx "
                    "(want IOGraphicsAccelerator2::newUserClient %#lx) -> %s", gAccelObj, (unsigned long)slide, sr,
                    kAccelNewUserClientSlot, (unsigned long)nuc, (unsigned long)(slide ? nuc - slide : 0),
                    (unsigned long)kStaticNewUserClient,
                    st == 0 ? "INSTALLED (newUserClient now ours)" : st == 2 ? "REFUSED: slide" : "REFUSED: slot is not newUserClient");
        }
    }
    if (!st) {
        if (arg == 1 && gUcInstalled) gUcMode = 1;
        else if (arg == 2) gUcMode = 0;
    }
    PEERLOG("ucprobe: control %llu -> status %u; installed %u mode %u; clients minted %u (foreign %u, failed %u); "
            "display-pipe clients seen %u patched %u (guard refusals %u); externalMethod calls %u "
            "[sel 4 = transaction_begin: %u | sel 8 = transaction_end: %u | selector bitmask %#x]; "
            "calls after which pipe+0x2a4 was non-zero: %u; last sel %u kr %#x +0x2a4 %u",
            (unsigned long long)arg, st, gUcInstalled, gUcMode, gUcMinted, gUcForeign, gUcFailed, gDpUcSeen,
            gDpUcPatched, gDpUcGuardRefused, gDpUcCalls, gDpUcSel4, gDpUcSel8, gDpUcSelMask, gDpUcLatched,
            gDpUcLastSel, gDpUcLastKr, gDpUcLast2a4);
    PEERLOG("ucprobe: REQNOTIFY CENSUS (" N48_REQNOTIFY_TOKEN "): sel-3 calls %u = slot 0 (ARMING, uc+0xf8) %u + "
            "slot 1 (NEVER ARMS, uc+0x140) %u + out of range %u; of those, %u arrived with pipe+0xd8 ALREADY NON-NULL. "
            "Last sel-3: slot %d, that record's +0x40 %d, cookie before/after %u/%u, pipe+0x2a0 before/after %u/%u. "
            "Max pipe+0x2a0 ever seen here %u; cookie ever non-NULL %u",
            gRnCalls, gRnSlot0, gRnSlot1, gRnSlotOther, gRnCookieLiveBefore,
            gRnLastSlot == 0xffffffff ? -1 : (int)gRnLastSlot, gRnLastKind == 0xffffffff ? -1 : (int)gRnLastKind,
            gRnLastCookieB, gRnLastCookieA, gRnLastArmB, gRnLastArmA, gUcMaxArm, gUcCookieEverLive);
    const uint64_t v[13] = {
        st, (uint64_t)gUcInstalled | ((uint64_t)gUcMode << 32), (uint64_t)gUcSlide,
        (uint64_t)gUcMinted | ((uint64_t)gUcForeign << 32), (uint64_t)gUcFailed | ((uint64_t)gUcCensusLines << 32),
        (uint64_t)gDpUcSeen | ((uint64_t)gDpUcPatched << 32),
        (uint64_t)(gDpUcGuardRefused & 0xffff) | ((uint64_t)(gRnCalls & 0xffff) << 16) |
            ((uint64_t)(gRnSlot0 & 0xffff) << 32) | ((uint64_t)(gRnSlot1 & 0xffff) << 48),
        gDpUcCalls,
        (uint64_t)gDpUcSel4 | ((uint64_t)gDpUcSel8 << 32), gDpUcSelMask,
        (uint64_t)gDpUcLatched | ((uint64_t)(gRnLastCookieB != 0) << 32) | ((uint64_t)(gRnLastCookieA != 0) << 33) |
            ((uint64_t)(gRnLastArmB != 0) << 34) | ((uint64_t)(gRnLastArmA != 0) << 35) |
            ((uint64_t)((gRnLastSlot == 0xffffffff ? 0u : gRnLastSlot + 1u) & 0xf) << 36) |
            ((uint64_t)((gRnLastKind == 0xffffffff ? 0xffu : gRnLastKind) & 0xff) << 40) |
            ((uint64_t)(gUcMaxArm & 0xff) << 48) | ((uint64_t)(gRnCookieLiveBefore & 0xff) << 56),
        (uint64_t)gDpUcLastSel | ((uint64_t)gDpUcLastKr << 32),
        (uint64_t)gDpUcLast2a4 | ((uint64_t)gUcCookieEverLive << 32),
    };
    if (out) for (unsigned i = 0; i < count && i < 13; i++) out[i] = v[i];
    return st;
}
