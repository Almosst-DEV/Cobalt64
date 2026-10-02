//
//  navi48_fbname.hpp — the runtime class rename. DEFAULT OFF.
//
//  `accel fbname <0|1>`:
//     0  disarm and report — restores the original class name unconditionally, verifies the restore
//        by reading the name back, and is safe to call from any state including "never armed".
//     1  arm — replace the framebuffer's OSMetaClass className pointer with an interned OSSymbol for
//        a name containing "AMD", so CoreDisplay's GetGPUVendorForFramebufferService resolves our
//        vendor as AMD (bit 0x4) instead of unknown (bit 0x1).
//
//  This edits a live kernel structure. Every guard is in navi48_fbname.cpp and the load-bearing
//  search logic is host-tested in src/navi48-bringup/tests/fbname_scan_test.cpp. It is NOT to be run
//  without an adversarial review.
//
#ifndef Navi48Fbname_hpp
#define Navi48Fbname_hpp

#include <stdint.h>

namespace n48fbname {

// The name we swap in. Case-sensitive "AMD" is what CFStringFind looks for. Chosen to be
// obviously ours and NOT to collide with Apple's own AMDFramebuffer / AMDRadeonX6000* classes.
#define N48_FBNAME_NEW "AMDRDNA4FB"
// The names that may legitimately be the framebuffer's leaf class before we touch anything.
#define N48_FBNAME_OLD "RDNA4FB"

uint32_t control(uint64_t arg, uint64_t *out, unsigned outCount);

// Called from the kext's stop path: restores the original name if still armed. Leaving a patched
// className behind in a kext that is going away would leave a dangling OSSymbol pointer.
void restore_on_unload(void);

}  // namespace n48fbname

#endif /* Navi48Fbname_hpp */
