//
//  native_agdc_pure.h - the pure half of the NATIVE AGDC service (kext 0.0.614, #11 step 11h.3): every DECISION of `pipeagdc` (accel action 88), judged from arguments alone.
//  No kernel header: tests/native_agdc_test.cpp compiles this very file (and amd/native_agdc_flow.h, the sequencing built on it) and drives it, with planted breaks
//  (tests/native_agdc_plant.sh).
//
//  WHAT THIS IS. IOPresentment needs an AppleGraphicsDeviceControl object (AGDC) to answer four vendor commands (1 / 0x980 / 0x921 / 0x711; DisplayPipeGuard.cpp's `agdc_vendor`).
//  On a native boot no AGDC instance exists ("agdc 0x0" in the wrangler log, 11h.0), and the class has a pure virtual, so `Navi48AGDC` is ours to build: a genuine AGDC object (Apple's own
//  allocator and constructor) on a vtable copy whose slots 0 / 1 / 266 are ours. The translation route (verb 67, `agdc`) did exactly that, but it depends on three things a native boot does
//  not have: the X6000 accelerator's slide, its display machine's pipe[0]+0x98 and its armed pipe guard. This header is the native replacement for those three:
//      slide        two anchors that must AGREE: the live AGDC metaclass minus its System-KC static address, and the live IOAccelDisplayPipe metaclass minus ITS static address
//                   (both are in the one System KC, one slide: M11H memo section 2 row 2; the statics are read from the extracted kexts' symbols, see kAgdcMeta / kPipeMeta);
//      framebuffer  the RDNA4FB instance itself (either name), not a pointer read out of an accelerator's pipe;
//      guard        none (no X6000 pipe exists). Instead: the display boot-arg latch, and a refusal while row-120's mode HOLD is up (the 0x921 reply's timing must be the framebuffer's 60 Hz).
//
//  Everything stays behind boot-arg navi48-metal-disp=1 (the latch of native_disp.cpp): with it OFF the verb is not admitted (native_disp_pure.h kLastAction / action_admitted) and this file's
//  flow answers "off" before it reads anything.
//
#pragma once
#include <stdint.h>

namespace n48agdc {

// ---- the verb ----------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint32_t kActAgdc = 88u;    // accel action 88: `pipeagdc [0|1]` - 0 reads the state, 1 publishes (n48disp::verb_args_ok admits exactly these two arguments)
constexpr bool arg_ok(uint64_t arg) { return arg <= 1ull; }

// ---- status codes ------------------------------------------------------------------------------------------------------------------------------------------
// Numbers 0 and 3..13 keep the TRANSLATION route's meaning (DisplayPipeGuard.cpp's agdc_publish_locked), so the two decode tables only differ at 1, 2, 14 and 15.
enum Status : uint32_t {
    kPublished = 0, kOff = 1, kUnused2 = 2, kNoAgdc = 3, kSlide = 4, kClassSize = 5, kCodeBytes = 6, kVtable = 7, kNoFbPci = 8, kNoWrangler = 9, kAlloc = 10, kStartFailed = 11,
    kAlready = 12, kBadArg = 13, kHeld = 14, kNoFamily = 15, kStatusCount = 16
};
constexpr const char *status_name(uint32_t s) {
    return s == kPublished ? "PUBLISHED" : s == kUnused2 ? "(unused here: the translation route's \"safety core not armed\")" : s == kOff ? "display is OFF (boot-arg navi48-metal-disp is not 1)" : s == kNoAgdc ? "AppleGraphicsDeviceControl class is not loaded" :
           s == kSlide ? "the two slide anchors disagree (or the slide is not page aligned)" : s == kClassSize ? "AGDC class size is not 0x110" :
           s == kCodeBytes ? "a function we call does not carry its expected first bytes" : s == kVtable ? "AGDC vtable slots 7/184/238/267 are not the expected functions" :
           s == kNoFbPci ? "no RDNA4FB / PCI device / provider" : s == kNoWrangler ? "no AppleGPUWrangler (AGDC::start would wait for it)" : s == kAlloc ? "allocation failed" :
           s == kStartFailed ? "AGDC::start returned false" : s == kAlready ? "already published" : s == kBadArg ? "bad argument" :
           s == kHeld ? "row-120 mode hold is up (the link timing would not be the framebuffer's)" : s == kNoFamily ? "IOAccelDisplayPipe class (the second slide anchor) is not loaded" : "unknown";
}

// ---- the slide ---------------------------------------------------------------------------------------------------------------------------------------------
// Static (unslid) addresses in the System KC, read from the EXTRACTED kexts' symbol tables (~/navi48-native/re-s3/kexts, `nm`): com.apple.AppleGraphicsDeviceControl
// __ZN26AppleGraphicsDeviceControl10gMetaClassE = 0x13d418e8 and com.apple.iokit.IOAcceleratorFamily2 __ZN18IOAccelDisplayPipe10gMetaClassE = 0x146036d8.
constexpr uint64_t kAgdcMeta = 0x13d418e8ull;
constexpr uint64_t kPipeMeta = 0x146036d8ull;
constexpr uint64_t kKernelHalf = 0xffffff7000000000ull;     // the bound DisplayPipeGuard.cpp's dpg_kptr and native_disp_pure.h use
constexpr bool kptr_ok(uint64_t p) { return p >= kKernelHalf; }
struct SlideOut { uint32_t status; uint64_t slide; };
// Both metaclass pointers must be kernel pointers, the two slides must be equal, and the slide must be page aligned. Anything else is kSlide (a zero metaclass is the caller's kNoAgdc /
// kNoFamily, decided before this). A kernel pointer is always ABOVE both statics (the static_assert below), so the subtractions cannot wrap once kptr_ok holds for both.
static_assert(kKernelHalf > kAgdcMeta && kKernelHalf > kPipeMeta, "a kernel pointer is above both static addresses: the slide subtractions cannot wrap");
constexpr SlideOut slide_decide(uint64_t agdcMeta, uint64_t pipeMeta) {
    return (!kptr_ok(agdcMeta) || !kptr_ok(pipeMeta)) ? SlideOut{ kSlide, 0ull } :
           ((agdcMeta - kAgdcMeta) != (pipeMeta - kPipeMeta) || ((agdcMeta - kAgdcMeta) & 0xfffull) != 0ull) ? SlideOut{ kSlide, 0ull } :
           SlideOut{ kPublished, agdcMeta - kAgdcMeta };
}

// ---- the framebuffer --------------------------------------------------------------------------------------------------------------------------------------
// RDNA4FB keeps matching under either name (the class rename of fbname swaps the OSMetaClass name pointer).
constexpr bool str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return *a == *b;
}
constexpr bool fb_name_ok(const char *name) { return name != nullptr && (str_eq(name, "RDNA4FB") || str_eq(name, "AMDRDNA4FB")); }

} // namespace n48agdc
