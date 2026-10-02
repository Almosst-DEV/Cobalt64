//
//  native_open_policy_pure.h - the pure half of the N48N open policy (kext 0.0.612, milestone #11 step 11c; notes/design/NATIVE-S4-M11.md sections 3 and 8). No kernel header:
//  tests/native_ws_open_test.cpp compiles this very file and drives it; IOAccelNavi48NativeClient::initWithTask (Navi48NativeClient.cpp) is the only caller.
//
//  The policy. Until 0.0.611 the native client admitted exactly one kind of caller: a task with kIOClientPrivilegeAdministrator (root). From 0.0.612 it ALSO admits a task whose
//  uid is 88 (_windowserver, measured on the PC) - but ONLY when the boot-arg navi48-metal-ws=1 was set (latched once, at start) and no native client is open. With the boot-arg
//  absent the decision is the old one, bit for bit: admit iff the caller is an administrator. (Exclusivity itself - one client, VMID 8 - is still n1c_open's compare-and-swap; the
//  `alreadyOpen` input only makes the uid-88 path refuse EARLY, with its own reason. A root caller never looks at it: its second open still ends in n1c_open's ExclusiveAccess.)
//
#pragma once
#include <stdint.h>

#include "Navi48NativeABI.h"

namespace n48native {
namespace policy {

constexpr uint32_t kWindowServerUid = 88u;   // _windowserver (id _windowserver -> uid=88, measured on the PC's 25G83 profile)

enum OpenReason : uint32_t {
    kReasonAdmin         = 0,   // admitted: an administrator (root), exactly as before 0.0.612
    kReasonWindowServer  = 1,   // admitted: uid 88, navi48-metal-ws=1, no client open
    kReasonNotPrivileged = 2,   // refused: not an administrator, and not the uid-88 path (today's refusal)
    kReasonWsArgOff      = 3,   // refused: uid 88 but navi48-metal-ws is not 1 on this boot (the feature does not exist)
    kReasonWsAlreadyOpen = 4,   // refused: uid 88 with the boot-arg, but a native client is already open
    kReasonCount         = 5
};
struct OpenDecision { bool admit; OpenReason reason; };

// admin: clientHasPrivilege(kIOClientPrivilegeAdministrator) succeeded. uid: the opening credential's effective uid. wsArg: the latched boot-arg. alreadyOpen: a native client is open now.
constexpr OpenDecision open_decision(bool admin, uint32_t uid, bool wsArg, bool alreadyOpen) {
    if (admin) return OpenDecision{ true, kReasonAdmin };
    if (uid != kWindowServerUid) return OpenDecision{ false, kReasonNotPrivileged };
    if (!wsArg) return OpenDecision{ false, kReasonWsArgOff };
    if (alreadyOpen) return OpenDecision{ false, kReasonWsAlreadyOpen };
    return OpenDecision{ true, kReasonWindowServer };
}
inline const char *open_reason_text(OpenReason r) {
    switch (r) {
    case kReasonAdmin:         return "administrator";
    case kReasonWindowServer:  return "uid 88 with navi48-metal-ws=1";
    case kReasonNotPrivileged: return "not an administrator";
    case kReasonWsArgOff:      return "uid 88 but boot-arg navi48-metal-ws is not 1";
    case kReasonWsAlreadyOpen: return "uid 88 but a native client is already open";
    default:                   return "?";
    }
}

// ---- who may call what (0.0.612 review items B and E) -------------------------------------------------------------------------------------
// B: selector 21 imports a range of the address space the client was OPENED with, so only a thread of that task may call it (a connection handed to another process by a Mach port
// must not import the owner's memory, or map its own under the owner's name). `cur` = current_task(), `owner` = the task stored at open; both null-checked.
constexpr bool import_caller_ok(const void *cur, const void *owner) { return owner != nullptr && cur == owner; }
// E: a client admitted by the uid-88 rule (NOT an administrator) may call only the selectors WindowServer's bundle needs: 0..8 (Hello, QueryInfo, ReadRegs, BoCreate, BoFree, GemVa,
// Ctx, Submit, WaitSeq), 9..14 (the scanout set) and 21 (BoImportHost). Everything else (15 DAL step, 16..18 mode trial / hold / release, 19..20 the Metal nub publish / withdraw, and any
// number the ABI does not name) is NotPrivileged for it. An administrator client reaches every selector, exactly as before 0.0.612.
constexpr bool selector_allowed_for_windowserver(uint32_t sel) {
    return sel <= (uint32_t)N48N_SEL_SCAN_RELEASE || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;
}
constexpr bool selector_allowed(bool admin, uint32_t sel) { return admin || selector_allowed_for_windowserver(sel); }

// The boot-arg latch: 0 = not latched yet, 1 = latched OFF, 2 = latched ON. First writer wins, never re-read.
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }

} // namespace policy
} // namespace n48native
