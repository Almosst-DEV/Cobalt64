//
//  native_metal_pure.h - the pure half of the Metal nub and its ops table (kext 0.0.610, milestone #9 route A; an internal design note sections 1 and 3 + the
//  review MUST-FIX 1, 2, 5, 6). No IOKit, no kernel: tests/native_metal_test.cpp compiles this very file and drives it, with planted breaks.
//
//  What lives here (and NOT in the aux kext, which every rebuild of requires a security approval (Allow click) from the user): the publish / withdraw verdicts, the nub state machine,
//  the IOAccelConfig contents and its static check, the stamp-page and task-window rules, the factory mask. Default OFF: nothing here runs unless a native
//  client calls the new selectors, and the nub is published only with boot-arg navi48-metal=1 on a native boot whose S1b reported POSITIVE PASS.
//
#pragma once
#include <stdint.h>
#include "../Navi48MetalOps.h"

namespace n48metal {

// ---- return codes (the kernel's IOReturn values; static_assert'ed in Navi48MetalNub.cpp) --------------------------------------------------------------
constexpr uint32_t kOk = 0u, kBadArg = 0xe00002c2u, kNotFound = 0xe00002f0u, kUnsupported = 0xe00002c7u, kNotReady = 0xe00002d8u,
                   kExclusive = 0xe00002c5u, kBusy = 0xe00002d5u, kNoMemory = 0xe00002bdu;

// ---- the nub state machine -----------------------------------------------------------------------------------------------------------------------------
// Off -> (publish) Published -> (withdraw) Terminating -> (the nub's free()) Off. A second nub is refused until the first is really gone.
enum State : uint32_t { kOff = 0, kPublished = 1, kTerminating = 2 };
inline State sm_after_publish(State s) { return s == kOff ? kPublished : s; }
inline State sm_after_withdraw(State s) { return s == kPublished ? kTerminating : s; }
inline State sm_after_free(State) { return kOff; }

// Everything the gate looks at, gathered by the kext at call time.
struct GateIn {
    uint64_t flags;        // the selector's flags word (must be 0)
    bool     hello;        // the native session said Hello
    bool     bootarg;      // boot-arg navi48-metal=1
    bool     s1bGateOn;    // the navi48-native gate accepted this boot
    bool     s1bRan;       // the S1b self-test ran
    bool     s1bPositive;  // ... and reported POSITIVE PASS
    bool     s1bStopped;   // a latched stop fired
    bool     hung;         // the GPU was declared HUNG this boot
    bool     dispNub;      // 0.0.652 (M5): a Navi48DisplayNub is published (fbpublish 2): the Metal nub (and with it the display pipe's accelerator) must not appear on this boot
    bool     m6 = false;   // 0.0.659 (M6 Stage 1a, R3): boot-arg navi48-m6 == 1 (latched): the display-nub interlock is lifted (the routing guard replaces it)
};
// The verdict order is part of the contract: cheapest / most specific first, the state last.
inline uint32_t publish_verdict(const GateIn &g, State st) {
    if (!g.hello) return kNotReady;
    if (g.flags != 0) return kBadArg;
    if (!g.bootarg) return kUnsupported;                    // navi48-metal absent / 0: the feature does not exist on this boot
    if (!g.s1bGateOn || !g.s1bRan || !g.s1bPositive || g.s1bStopped) return kNotReady;
    if (g.hung) return kNotReady;
    if (g.dispNub && !g.m6) return kExclusive;                       // 0.0.652 (M5) INTERLOCK: the monitor B framebuffer and the accelerator's display machine never coexist (see native_disp_pure.h fb_interlock_refuses)
    if (st == kPublished) return kExclusive;
    if (st == kTerminating) return kBusy;
    return kOk;
}
// Withdraw must always be able to clean up: it needs the session (Hello) and a clean flags word, nothing about the boot-arg or S1b.
inline uint32_t withdraw_verdict(const GateIn &g, State st) {
    if (!g.hello) return kNotReady;
    if (g.flags != 0) return kBadArg;
    if (st == kPublished) return kOk;
    if (st == kTerminating) return kBusy;
    return kNotFound;
}
// Boot: the nub is NEVER published by the kext itself. (Pinned by a source scan: the only caller of the publish code is the native client's selector.)
inline bool publish_at_boot() { return false; }

// ---- IOAccelConfig (MUST-FIX 1) --------------------------------------------------------------------------------------------------------------------------
// AppleParavirtAccelerator::populateAccelConfig (0x142b78f4 in the paravirt kext), 12 stores (CONFIRMED by disassembly; the design review said 13):
//   +0x00 q name ptr | +0x08 d 0x480000 | +0x0c q 7 | +0x14 q 0x2000000000 | +0x20 q 0x40000000 | +0x28 q 0x100000000008 | +0x30 d 0x40004000 |
//   +0x47 b 1 (type-4 display-pipe UC enable: OURS 0, headless) | +0x5c d 4 | +0x64 d 2 | +0x68 q 0x10000000a | +0x88 q 0
// Everything else (+0x4c, +0x50, +0x54 ...) keeps the FAMILY DEFAULTS that IOGraphicsAccelerator2::initializeConfigStructure() wrote just before
// (start calls that, then populateAccelConfig): never memset/memcpy the structure. IOSurfaceRoot::updateLimits takes +0x4c/+0x50 (ORed) and
// +0x54/+0x30/+0x32 as MINIMUMS for every IOSurface in the system for the whole boot: a zero there is a boot-lasting hazard, hence the static check.
constexpr uint32_t kCfgBytes = 0x90u;
enum : uint32_t { kOffName = 0x00, kOffF0 = 0x08, kOffF1 = 0x0c, kOffF2 = 0x14, kOffF3 = 0x20, kOffF4 = 0x28, kOffLim30 = 0x30, kOffLim32 = 0x32,
                  kOffPipeUC = 0x47, kOffLim4c = 0x4c, kOffLim50 = 0x50, kOffLim54 = 0x54, kOffF5c = 0x5c, kOffF64 = 0x64, kOffF68 = 0x68, kOffF88 = 0x88 };
struct CfgStore { uint32_t off; uint8_t width; uint64_t value; };   // width 0 = the name pointer
constexpr CfgStore kCfgStores[12] = {
    { kOffName,   0, 0 },
    { kOffF0,     4, 0x480000ull },
    { kOffF64,    4, 2ull },
    { kOffF1,     8, 7ull },
    { kOffF2,     8, 0x2000000000ull },
    { kOffF3,     8, 0x40000000ull },
    { kOffF4,     8, 0x100000000008ull },
    { kOffLim30,  4, 0x40004000ull },         // +0x30 = 0x4000 (u16), +0x32 = 0x4000 (u16)
    { kOffPipeUC, 1, 0ull },                  // paravirt stores 1; headless: 0
    { kOffF68,    8, 0x10000000aull },
    { kOffF88,    8, 0ull },
    { kOffF5c,    4, 4ull },
};
inline void st_le(uint8_t *cfg, uint32_t off, uint8_t width, uint64_t v) { for (uint8_t i = 0; i < width; ++i) cfg[off + i] = (uint8_t)(v >> (8u * i)); }
inline uint64_t ld_le(const uint8_t *p, uint8_t width) { uint64_t v = 0; for (uint8_t i = 0; i < width; ++i) v |= (uint64_t)p[i] << (8u * i); return v; }
inline void config_fill(uint8_t *cfg, uint64_t namePtr) {
    for (const CfgStore &s : kCfgStores) {
        if (s.width == 0) st_le(cfg, s.off, 8, namePtr);
        else st_le(cfg, s.off, s.width, s.value);
    }
}
enum ConfigVerdict : uint32_t { kCfgOk = 0, kCfgNoName = 1, kCfgLimit30 = 2, kCfgLimit32 = 3, kCfgLimit4c = 4, kCfgLimit50 = 5, kCfgLimit54 = 6, kCfgPipeUC = 7 };
// The static check: all five surface limits non-zero, the name set, the type-4 enable clear.
inline ConfigVerdict config_check(const uint8_t *cfg) {
    if (ld_le(cfg + kOffName, 8) == 0)  return kCfgNoName;
    if (ld_le(cfg + kOffLim30, 2) == 0) return kCfgLimit30;
    if (ld_le(cfg + kOffLim32, 2) == 0) return kCfgLimit32;
    if (ld_le(cfg + kOffLim4c, 4) == 0) return kCfgLimit4c;
    if (ld_le(cfg + kOffLim50, 4) == 0) return kCfgLimit50;
    if (ld_le(cfg + kOffLim54, 4) == 0) return kCfgLimit54;
    if (cfg[kOffPipeUC] != 0)           return kCfgPipeUC;
    return kCfgOk;
}
// The hook body: 0 = filled, checked and copied into the caller's structure; non-zero = the caller's structure is left BYTE-FOR-BYTE as it was (the family
// defaults: 0.0.611 fills a LOCAL copy, checks that, and copies it back only when the check passed, so a failure never leaves our values in the struct that
// IOSurfaceRoot::updateLimits reads). The aux kext then installs its own static non-NULL name and tears the device down; the family's start fails closed.
// (The matching comment in Navi48MetalOps.h, byte-identical in the aux kext, already says "leaves the config as the family defaults": it is true from 0.0.611.)
inline int config_populate(uint8_t *cfg, uint32_t bytes, uint64_t namePtr) {
    if (!cfg || bytes != kCfgBytes || namePtr == 0) return 1;
    uint8_t work[kCfgBytes];
    for (uint32_t i = 0; i < kCfgBytes; ++i) work[i] = cfg[i];      // the family defaults, copied (never memset: the untouched bytes are the defaults)
    config_fill(work, namePtr);
    const ConfigVerdict v = config_check(work);
    if (v != kCfgOk) return (int)(10u + (uint32_t)v);                // nothing copied back
    for (uint32_t i = 0; i < kCfgBytes; ++i) cfg[i] = work[i];
    return 0;
}

// ---- the stamp page ---------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kStampBytes = 0x1000u;
constexpr uint64_t kStampPhysMask = 0xFFFFFFFFFF000ull;      // 4 KiB aligned, below 2^52
inline bool stamp_page_ok(uint64_t phys, uint64_t seg, uint64_t len) { return phys != 0 && seg >= kStampBytes && len >= kStampBytes && (phys & 0xfffull) == 0 && (phys & ~kStampPhysMask) == 0; }

// ---- the task VA window ------------------------------------------------------------------------------------------------------------------------------------
// paravirt: IORangeAllocator::init(size-1, 0x1000, 0x400, 0), the first page taken. Same for the user and the kernel task.
constexpr uint64_t kTaskVaSize = 0x400000000ull, kTaskVaReserve = 0x1000ull;
inline int task_window(uint32_t kind, uint64_t *size, uint64_t *reserve) {
    if (kind > 1 || !size || !reserve) return 1;
    *size = kTaskVaSize; *reserve = kTaskVaReserve;
    return 0;
}

// ---- factories ---------------------------------------------------------------------------------------------------------------------------------------------
// Default 0 (every optional factory of the aux kext returns NULL: NATIVE-S3 "before 9e: NULL"). boot-arg navi48-metal-fact=<mask> enables the optional aux classes (bit 0 SysMemory, 1 MemoryMap, 2 VidMemory, 3 Resource, 4 2DContext, 5 SharedUserClient, 6 CommandQueue).
inline uint32_t factory_mask(bool argPresent, uint32_t argValue) { return argPresent ? (argValue & N48_FACT_ALL) : 0u; }

// ---- the generic virtual hook (0.0.611) ------------------------------------------------------------------------------------------------------------------
// The aux kext's trampolines call ops->vhook(cls, slot, ...) on every hooked slot; return 1 = handled (*ret is the slot's return value), else the aux default.
// (1) Logging is rate limited per (cls, slot): the first kVhookLogFirst calls log a full line, then one count line every kVhookLogEvery calls (a hooked slot on a hot
// path, e.g. eventTimeout from gart_collector on the workloop, must not flood the log).
// (2) Handling exists only for the optional VidMemory / Resource classes, only when their factory bit is enabled (default mask 0: nothing is handled), and only writes
// what AppleParavirtGPU's own trivial implementations write (CONFIRMED by disassembly of com.apple.driver.AppleParavirtGPU):
//   VidMemory slot 43 getPhysicalSegment(off, u64 *len):  0x142b690c  movq $0,(%rdx); xor eax,eax     -> *len = 0, return 0
//   VidMemory slot 61 allocPhysical():                    0x142b691c  movb $1,%al                      -> return true
//   Resource  slot 60 getLevelOffset(u8,u8,int *o):       0x142c2dce  movl $0,(%rcx); xor eax,eax      -> *o = 0, return 0
//   Resource  slot 61 getBackingLevelOffset(u8,u8,int *o):0x142c2ddc  movl $0,(%rcx); xor eax,eax      -> *o = 0, return 0
//   Resource  slot 62 calculateIOSurfaceDeviceCacheVRAMBytes(u64 *a, u64 *b): 0x142c2dea  writes both outputs from the host device object (SUSPECTED: two accessor
//             calls whose targets the extracted kext does not resolve); we have no host VRAM cache: both outputs = 0, return 0.
// Every output pointer is checked before ANY write (non-NULL, naturally aligned, in the kernel half of the address space); a bad pointer = not handled (no write).
constexpr uint64_t kKernelMin = 0xFFFF800000000000ull;
constexpr uint32_t kVhookLogFirst = 4u, kVhookLogEvery = 4096u, kVhookSlots = 32u;
struct VhookCounts { uint32_t key[kVhookSlots]; uint32_t n[kVhookSlots]; };   // key 0 = free; the last entry is the shared overflow bucket
// The 1-based call number of (cls, slot). Lock-free (the hook may run on several threads).
inline uint32_t vhook_count(VhookCounts &t, uint32_t cls, uint32_t slot) {
    const uint32_t key = 0x80000000u | ((cls & 0x7fffu) << 16) | (slot & 0xffffu);
    for (uint32_t i = 0; i + 1u < kVhookSlots; ++i) {
        uint32_t k = __atomic_load_n(&t.key[i], __ATOMIC_ACQUIRE);
        if (k == 0u) {
            uint32_t exp = 0u;
            if (__atomic_compare_exchange_n(&t.key[i], &exp, key, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) k = key; else k = exp;
        }
        if (k == key) return __atomic_add_fetch(&t.n[i], 1u, __ATOMIC_RELAXED);
    }
    return __atomic_add_fetch(&t.n[kVhookSlots - 1u], 1u, __ATOMIC_RELAXED);
}
enum VhookLog : uint32_t { kVhLogNone = 0, kVhLogFull = 1, kVhLogCount = 2 };
inline uint32_t vhook_log_kind(uint32_t n) { return n <= kVhookLogFirst ? kVhLogFull : (n % kVhookLogEvery == 0u ? kVhLogCount : kVhLogNone); }

inline bool vhook_ptr_ok(uint64_t p, uint32_t align, uint64_t kmin) { return p != 0 && (p % align) == 0 && p >= kmin; }
inline int vhook_handle(uint32_t mask, uint32_t cls, uint32_t slot, const uint64_t *args, uint32_t nargs, uint64_t *ret, uint64_t kmin) {
    if (!args || !ret || nargs > 6u) return 0;
    if (cls == N48_VC_VIDMEMORY && (mask & N48_FACT_VIDMEMORY) != 0u) {
        if (slot == 43u && nargs == 2u && vhook_ptr_ok(args[1], 8u, kmin)) { *(volatile uint64_t *)(uintptr_t)args[1] = 0; *ret = 0; return 1; }
        if (slot == 61u && nargs == 0u) { *ret = 1; return 1; }
        return 0;
    }
    if (cls == N48_VC_RESOURCE && (mask & N48_FACT_RESOURCE) != 0u) {
        if ((slot == 60u || slot == 61u) && nargs == 3u && vhook_ptr_ok(args[2], 4u, kmin)) { *(volatile uint32_t *)(uintptr_t)args[2] = 0; *ret = 0; return 1; }
        if (slot == 62u && nargs == 2u && vhook_ptr_ok(args[0], 8u, kmin) && vhook_ptr_ok(args[1], 8u, kmin) && args[0] != args[1]) {
            *(volatile uint64_t *)(uintptr_t)args[0] = 0; *(volatile uint64_t *)(uintptr_t)args[1] = 0; *ret = 0; return 1;
        }
        return 0;
    }
    return 0;
}

// ---- the Ready property (0.0.612, milestone #11 step 11c; NATIVE-S4-M11 section 7 L3) ---------------------------------------------------------------------------
// The nub carries "Navi48,Ready": 1 while the native path is usable, 0 once the GPU was declared HUNG. Sticky: it is written 1 at publish only if no hang was latched, is set to 0 by the
// HUNG latch itself (native_s1c.cpp hang_announce -> Navi48MetalNub::hungLatched), and NEVER goes back to 1 on that boot (the bundle reads it to decline without opening N48N).
constexpr uint32_t kReadyYes = 1u, kReadyNo = 0u;
// The value to publish given the boot-wide sticky state (`stickyNo` = a hang was ever latched this boot) and what the gate saw right now.
constexpr uint32_t ready_at_publish(bool stickyNo, bool hungNow) { return (stickyNo || hungNow) ? kReadyNo : kReadyYes; }
// The next sticky state after an event: once No, always No.
constexpr bool ready_sticky_after(bool stickyNo, bool hangEvent) { return stickyNo || hangEvent; }

// ---- the ops table's identity ----------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kOpsFlags = N48_METAL_F_SOFTWARE_ONLY;
constexpr uint64_t kOpsCaps = N48_CAP_VHOOK | N48_CAP_NATIVE_OPEN;   // 0.0.656 (G6): + native_open (ABI 3)

// ---- native_open (ops ABI 3, 0.0.656, G6): the aux accelerator's newUserClient hands an 'N48N' open here -----------------------------------------------------------------
// The answer is an IOReturn (kOk = 0 = "a client is being returned"); it is NEVER a bool. The checks, in order: the type is 'N48N' (anything else is Unsupported: the aux kext only calls us for it); a live device whose context
// is the ctx the aux kext passed (identity, magic); `accel` is the accelerator that device was opened for (the pointer device_open received: nobody else is a valid provider); a nub is published (the owner is its provider).
// Only then does the caller build the client. A refusal never touches *handler.
constexpr uint32_t native_open_verdict(bool typeIsN48N, bool deviceLive, bool accelIsRegistered, bool nubPublished) {
    return !typeIsN48N ? kUnsupported : !deviceLive ? kNotReady : !accelIsRegistered ? kBadArg : !nubPublished ? kNotReady : kOk;
}

} // namespace n48metal
