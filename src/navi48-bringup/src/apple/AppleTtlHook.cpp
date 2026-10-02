//
//  AppleTtlHook.cpp — see AppleTtlHook.hpp.
//
#include "AppleTtlHook.hpp"
#include "Navi48Ttl.hpp"
#include "../amd/n48log.h"
#include <IOKit/IOLib.h>
#include <libkern/c++/OSIterator.h>

#define HOOKLOG(fmt, ...) ::amdgpu::n48_logf("AppleTtlHook: " fmt "\n", ##__VA_ARGS__)

namespace n48 {

// Measured from com.apple.kext.AMDRadeonX6000HWServices, Tahoe 26.6.2:
//   __ZTV43AMDRadeonX6000_AMDRadeonHWServicesNavi = 270 slots
//   slot 266 (byte offset 0x850) = AMDRadeonX6000_AMDRadeonHWServicesSWIP::getTtl()
// If a future build changes either number this hook must be re-measured, not
// nudged — see tools/verify-ttl-vtable.sh and notes/APPLE-DRIVER-VERDICT.md.
static constexpr unsigned kHwServicesVtableSlots = 270;
static constexpr unsigned kGetTtlSlot            = 266;
static constexpr const char *kHwServicesClass    = "AMDRadeonX6000_AMDRadeonHWServicesNavi";

// HOW WE KNOW SLOT 266 IS getTtl AT RUNTIME
//
// An absolute address range is useless here: kext text in a kernel collection
// lives around 0xffffff7f________, far below the kernel's 0xffffff80________,
// and nowhere near the base `kmutil showloaded` prints (that is the kext's data
// region, not its text). Version 0.0.40 refused to patch for exactly that
// reason — the guard was wrong, not the slot.
//
// So verify the vtable's own internal geometry, which no slide can change:
//
//   * slots 266, 268 and 269 all point into HWServices' own __text, so the
//     distances between them survive relocation exactly.
//   * a slide is page-granular, so the low 12 bits of getTtl are invariant.
//
// Static values from com.apple.kext.AMDRadeonX6000HWServices, Tahoe 26.6.2:
//   [266] 0x0c269150  AMDRadeonX6000_AMDRadeonHWServicesSWIP::getTtl()
//   [268] 0x0c269dc0  AMDRadeonX6000_AMDRadeonHWServicesAbstract::createPowerPlayInterface()
//   [269] 0x0c268be0  AMDRadeonX6000_AMDRadeonHWServicesNavi::getMatchProperty()
// If any of these three checks fails, the binary changed and the slot number
// must be re-measured — never nudged.
static constexpr unsigned kAnchorSlotA        = 269;
static constexpr long     kDeltaGetTtlToA     = 0x570;      // vt[266] - vt[269]
static constexpr unsigned kAnchorSlotB        = 268;
static constexpr long     kDeltaGetTtlToB     = -0xc70;     // vt[266] - vt[268]
static constexpr uintptr_t kGetTtlPageOffset  = 0x150;      // 0x0c269150 & 0xfff

// Canonical kernel-half addresses: everything at or above this, kexts included.
static constexpr uintptr_t kKernelHalfBase = 0xFFFFFF7F00000000ULL;

// A C++ vtable pointer points PAST two header words (offset-to-top, RTTI), so
// the allocation has to include them or anything that walks backwards reads our
// heap instead.
static constexpr unsigned kVtableHeaderWords = 2;
static constexpr unsigned kCopyWords         = kHwServicesVtableSlots + kVtableHeaderWords;

static Navi48Ttl *gHookedTtl   { nullptr };
static void     **gPatchedVt   { nullptr };   // the copy we installed (points at slot 0)

// Replacement for AMDRadeonX6000_AMDRadeonHWServicesSWIP::getTtl().
// Called with rdi = the HWServices instance, which we ignore.
static void *navi48_get_ttl(void * /*self*/) {
    return static_cast<AmdTtlServicesABI *>(gHookedTtl);
}

static IOService *find_hw_services(IOService *nub) {
    if (!nub) return nullptr;
    OSIterator *it = nub->getChildIterator(gIOServicePlane);
    if (!it) return nullptr;
    IOService *found = nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOService *svc = OSDynamicCast(IOService, o);
        if (!svc) continue;
        // metaCast by NAME: we have no link-time access to Apple's class, but
        // IOKit will do the cast for us from the string.
        if (svc->metaCast(kHwServicesClass)) { found = svc; break; }
    }
    it->release();
    return found;
}

TtlHookResult install_ttl_hook(IOService *pciNub, Navi48Ttl *ttl) {
    TtlHookResult r;
    if (!pciNub || !ttl) { r.why = "null nub or ttl"; return r; }

    IOService *svc = nullptr;
    for (int attempt = 0; attempt < 40 && !svc; attempt++) {   // ~2 s
        svc = find_hw_services(pciNub);
        if (!svc) IOSleep(50);
    }
    if (!svc) {
        r.why = "HWServices never appeared under the PCI nub";
        HOOKLOG("%s (looked for %s for 2 s)", r.why, kHwServicesClass);
        return r;
    }
    r.hwServices = svc;
    HOOKLOG("found %s at %p", kHwServicesClass, svc);

    void ***instVtSlot = reinterpret_cast<void ***>(svc);
    void  **vt         = *instVtSlot;
    if (!vt) { r.why = "instance has a null vtable pointer"; HOOKLOG("%s", r.why); return r; }

    if (gPatchedVt && vt == gPatchedVt + kVtableHeaderWords) {
        r.installed = true; r.why = "already hooked";
        HOOKLOG("already hooked — nothing to do");
        return r;
    }

    void *orig = vt[kGetTtlSlot];
    r.originalGetTtl = orig;
    const uintptr_t a  = reinterpret_cast<uintptr_t>(orig);
    const uintptr_t an = reinterpret_cast<uintptr_t>(vt[kAnchorSlotA]);
    const uintptr_t bn = reinterpret_cast<uintptr_t>(vt[kAnchorSlotB]);

    if (a == 0 || a < kKernelHalfBase || an < kKernelHalfBase || bn < kKernelHalfBase) {
        r.why = "vtable slots are not canonical kernel addresses";
        HOOKLOG("%s (266=%p 268=%p 269=%p). REFUSING to patch.", r.why, orig, (void *)bn, (void *)an);
        return r;
    }
    const long dA = (long)(a - an), dB = (long)(a - bn);
    if (dA != kDeltaGetTtlToA || dB != kDeltaGetTtlToB) {
        r.why = "vtable geometry does not match the measured binary — slot 266 may not be getTtl";
        HOOKLOG("%s: delta(266,269)=%#lx expected %#lx; delta(266,268)=%#lx expected %#lx. "
                "REFUSING to patch.", r.why, dA, (long)kDeltaGetTtlToA, dB, (long)kDeltaGetTtlToB);
        return r;
    }
    if ((a & 0xFFFULL) != kGetTtlPageOffset) {
        r.why = "slot 266 has the wrong page offset for getTtl";
        HOOKLOG("%s (%p & 0xfff = %#lx, expected %#lx). REFUSING to patch.",
                r.why, orig, (unsigned long)(a & 0xFFF), (unsigned long)kGetTtlPageOffset);
        return r;
    }
    HOOKLOG("vtable verified: getTtl=%p, delta to slot 269 = %#lx, to slot 268 = %#lx, "
            "page offset %#lx — all three match the shipping binary",
            orig, dA, dB, (unsigned long)(a & 0xFFF));

    const size_t bytes = kCopyWords * sizeof(void *);
    void **copy = static_cast<void **>(IOMalloc(bytes));
    if (!copy) { r.why = "IOMalloc for the vtable copy failed"; HOOKLOG("%s", r.why); return r; }
    memcpy(copy, vt - kVtableHeaderWords, bytes);

    gHookedTtl = ttl;
    copy[kVtableHeaderWords + kGetTtlSlot] = reinterpret_cast<void *>(&navi48_get_ttl);
    gPatchedVt = copy;

    // Publish the new table before the instance points at it.
    __asm__ __volatile__("sfence" ::: "memory");
    *instVtSlot = copy + kVtableHeaderWords;

    r.installed = true;
    r.why       = "ok";
    HOOKLOG("hooked getTtl (slot %u): Apple's %p -> ours %p; instance %p now uses our vtable copy %p",
            kGetTtlSlot, orig, (void *)&navi48_get_ttl, svc, copy + kVtableHeaderWords);
    HOOKLOG("Apple's accelerator will now drive the GPU through Navi48Ttl, not AmdTtlServices");
    return r;
}

} // namespace n48
