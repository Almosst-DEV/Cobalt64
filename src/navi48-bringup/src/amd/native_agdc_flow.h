//
//  native_agdc_flow.h - the SEQUENCE of the native AGDC publish (kext 0.0.614), templated over an environment E so tests/native_agdc_test.cpp runs the very code the kernel runs over a fake
//  kernel. The kernel's E is DisplayPipeGuard.cpp's AgdcNativeEnv. Decisions: amd/native_agdc_pure.h.
//
//  ORDER IS THE CONTRACT. Every refusal that needs no kernel object comes before any lookup; every lookup before the class checks (which READ memory at slide + a static address, so they
//  run only once two independent anchors agree on the slide); the framebuffer, PCI device and provider before the wrangler; and the build (allocate, construct, start, register) is reached
//  ONLY after all of them. `0` reads the state and never builds.
//
#pragma once
#include "native_agdc_pure.h"

namespace n48agdc {

// What the environment must provide (all read-only except build):
//   bool latch_on();                                   the display boot-arg latch (n48disp_latched_on)
//   bool published();                                  an AGDC object of ours already exists (either route)
//   bool mode_held();                                  row-120's mode hold is launching or up (n48dcn::modeHoldActive)
//   uint64_t agdc_meta();                              the live AppleGraphicsDeviceControl metaclass (0 = class not loaded)
//   uint64_t pipe_meta();                              the live IOAccelDisplayPipe metaclass (0 = class not loaded)
//   uint32_t class_checks(uint64_t agdcMeta, uint64_t slide);   0, or 5 / 6 / 7 (class size, code bytes, vtable slots), READ-ONLY
//   bool have_targets();                               the RDNA4FB instance (fb_name_ok), our PCI device and our provider service all exist
//   bool wrangler();                                   AppleGPUWrangler is registered
//   uint32_t build(uint64_t agdcMeta, uint64_t slide); allocate + construct + start + register; 0, 10 or 11
template <class E> uint32_t native_flow(E &e, uint64_t arg) {
    if (!arg_ok(arg)) return kBadArg;
    if (arg == 0ull) return kPublished;                          // the state read: never publishes, never reads the class
    if (!e.latch_on()) return kOff;
    if (e.published()) return kAlready;
    if (e.mode_held()) return kHeld;
    const uint64_t am = e.agdc_meta();
    if (am == 0ull) return kNoAgdc;
    const uint64_t pm = e.pipe_meta();
    if (pm == 0ull) return kNoFamily;
    const SlideOut s = slide_decide(am, pm);
    if (s.status != kPublished) return s.status;
    const uint32_t cc = e.class_checks(am, s.slide);
    if (cc != kPublished) return cc;
    if (!e.have_targets()) return kNoFbPci;
    if (!e.wrangler()) return kNoWrangler;
    return e.build(am, s.slide);
}

} // namespace n48agdc
