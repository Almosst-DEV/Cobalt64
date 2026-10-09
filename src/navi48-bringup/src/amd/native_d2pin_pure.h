//
//  native_d2pin_pure.h - the pure decisions of the monitor B plane's buffer pair in the allocator helper (native_s1c.cpp n1c_d2_alloc / n1c_d2_free / n1c_d2_pin), kext 0.0.652 (milestone M5).
//  No kernel header: tests/native_disp2_test.cpp compiles this very file and drives sequences of it; the kernel helper makes its decisions through these functions.
//
//  The pair (buffer A and B of `disp2 plane 2`) lives in the shared visible-VRAM allocator. `disp2 fbhold 2` PINS it for the rest of the boot: Navi48Framebuffer scans A and WindowServer maps it, so a freed pair that is
//  still mapped would let CPU writes corrupt whatever reuses that VRAM. Pinned = held forever: a free is a logged no-op, a second allocation is refused (the pair is still held), there is no unpin (a reboot is the release).
//
#pragma once

namespace n48d2pin {

enum FreeAction : unsigned { kFreeNone = 0u, kFreeDo = 1u, kFreePinned = 2u };
// n1c_d2_free: nothing held -> nothing to do; held and not pinned -> give both buffers back; PINNED -> a logged no-op (checked FIRST: a pinned pair is by definition held).
constexpr FreeAction free_action(bool held, bool pinned) { return pinned ? kFreePinned : (held ? kFreeDo : kFreeNone); }

// n1c_d2_alloc: a second pair while one is held (a pinned pair is always held) is refused (kIOReturnExclusiveAccess).
constexpr bool alloc_refused(bool held) { return held; }

// n1c_d2_pin: the allocator must hold a pair with a context, it must not be pinned already, and it must be EXACTLY the pair the plane reports (A = mc[0], B = mc[1], both non-zero).
constexpr bool pin_ok(bool held, bool haveCtx, bool alreadyPinned, bool isA, bool isB) { return held && haveCtx && !alreadyPinned && isA && isB; }

// The state after each call (held, pinned): free keeps both while pinned; pin sets pinned (never cleared); alloc never changes a held state.
struct State { bool held, pinned; };
constexpr State after_free(State s) { return free_action(s.held, s.pinned) == kFreeDo ? State{ false, false } : s; }
constexpr State after_pin(State s, bool ok) { return ok ? State{ s.held, true } : s; }
constexpr State after_alloc(State s, bool ok) { return (!alloc_refused(s.held) && ok) ? State{ true, false } : s; }

}  // namespace n48d2pin
