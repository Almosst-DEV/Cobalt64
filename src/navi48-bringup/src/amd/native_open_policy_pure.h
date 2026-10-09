//
//  native_open_policy_pure.h - the pure half of the N48N open policy (kext 0.0.612, milestone #11 step 11c; an internal design note sections 3 and 8). No kernel header:
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
    kReasonNotPrivileged = 2,   // refused: not root (kIOClientPrivilegeAdministrator is "root"), and not the uid-88 path (today's refusal)
    kReasonWsArgOff      = 3,   // refused: uid 88 but navi48-metal-ws is not 1 on this boot (the feature does not exist)
    kReasonWsAlreadyOpen = 4,   // refused: uid 88 with the boot-arg, but a native client is already open
    kReasonApp           = 5,   // admitted (0.0.640, GPU-apps G4): a non-root uid >= 501 process on the kernel allow-list, navi48-apps=1 AND navi48-multisession=1
    kReasonAppNotAllowed = 6,   // refused (0.0.640): uid >= 501, both boot-args set, but the process is not on the allow-list
    kReasonCount         = 7
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
    case kReasonNotPrivileged: return "not root (euid != 0)";
    case kReasonWsArgOff:      return "uid 88 but boot-arg navi48-metal-ws is not 1";
    case kReasonWsAlreadyOpen: return "uid 88 but a native client is already open";
    case kReasonApp:           return "allow-listed user application (navi48-apps=1)";
    case kReasonAppNotAllowed: return "user application not on the navi48-apps allow-list";
    default:                   return "?";
    }
}

// ---- 0.0.640 (GPU-apps G4): the third role, APP ---------------------------------------------------------------------------------------------------------------
// open_decision (above) is UNCHANGED: it still knows two kinds of caller. app_refine takes ITS verdict and, only for a caller it refused as kReasonNotPrivileged whose uid is an ordinary
// user's (>= kAppMinUid) while BOTH boot-args are set (appsOn = navi48-apps=1 AND navi48-multisession=1), turns the refusal into kReasonApp (the process is on the kernel allow-list) or
// kReasonAppNotAllowed (it is not). Everything else - an administrator, uid 88, a system uid below 501, and EVERY caller when appsOn is false - passes through untouched, so with the
// switches off the decision is bit for bit today's.
constexpr uint32_t kAppMinUid = 501u;   // the first ordinary macOS user; below it are root, daemons and _windowserver
constexpr OpenDecision app_refine(OpenDecision d, uint32_t uid, bool appsOn, bool onAllowList) {
    if (d.admit || d.reason != kReasonNotPrivileged || !appsOn || uid < kAppMinUid) return d;
    return onAllowList ? OpenDecision{ true, kReasonApp } : OpenDecision{ false, kReasonAppNotAllowed };
}
// An APP client reaches the application set only: Hello, QueryInfo, BoCreate, BoFree, GemVa, Ctx, Submit, WaitSeq (0, 1, 3..8) and BoImportHost (21). Never ReadRegs (2: raw register reads),
// never the scanout set (9..14: the display plane belongs to WindowServer), never 15..20 (DAL step, mode trial / hold / release, Metal nub publish / withdraw), nor an unknown number.
constexpr bool selector_allowed_for_app(uint32_t sel) {
    return sel == (uint32_t)N48N_SEL_HELLO || sel == (uint32_t)N48N_SEL_QUERYINFO ||
           (sel >= (uint32_t)N48N_SEL_BOCREATE && sel <= (uint32_t)N48N_SEL_WAITSEQ) || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST;
}

// ---- who may call what (0.0.612 review items B and E) -------------------------------------------------------------------------------------
// B: selector 21 imports a range of the address space the client was OPENED with, so only a thread of that task may call it (a connection handed to another process by a Mach port
// must not import the owner's memory, or map its own under the owner's name). `cur` = current_task(), `owner` = the task stored at open; both null-checked.
constexpr bool import_caller_ok(const void *cur, const void *owner) { return owner != nullptr && cur == owner; }
// E: a client admitted by the uid-88 rule (NOT an administrator) may call only the selectors WindowServer's bundle needs: 0..8 (Hello, QueryInfo, ReadRegs, BoCreate, BoFree, GemVa,
// Ctx, Submit, WaitSeq), 9..14 (the scanout set) and 21 (BoImportHost). Everything else (15 DAL step, 16..18 mode trial / hold / release, 19..20 the Metal nub publish / withdraw, and any
// number the ABI does not name) is NotPrivileged for it. An administrator client reaches every selector, exactly as before 0.0.612.
constexpr bool selector_allowed_for_windowserver(uint32_t sel) {
    return sel <= (uint32_t)N48N_SEL_SCAN_RELEASE || sel == (uint32_t)N48N_SEL_BO_IMPORT_HOST ||
           (sel >= (uint32_t)N48N_SEL_SCANX_ACQUIRE && sel <= (uint32_t)N48N_SEL_SCANX_RELEASE);   // 0.0.661 (ABI 1.11): instance 2's scanout - WindowServer's bundle presents the monitor B with them (APPs and the accelerator route still never reach them)
}
constexpr bool selector_allowed(bool admin, uint32_t sel) { return admin || selector_allowed_for_windowserver(sel); }

// ---- 0.0.656 (G6): the ROUTE a native client is attached through -------------------------------------------------------------------------------------------------
// A client is attached either to Navi48Bringup (kRouteBringup: the PCI driver, what WindowServer and root tools open, exactly as before) or to the aux kext's Navi48Accelerator (kRouteAccelerator: what a SANDBOXED application
// opens, because its profile admits user clients on an IOAccelerator service; an internal design note "G6 design"). Admission (initWithTask: allow-list / root / WindowServer identity) is the same on both routes.
// The accelerator route gets the APP selector set (0, 1, 3..8, 21) ONLY, whatever the client's role: selector 20 (Metal nub withdraw) on a client whose provider is the accelerator would terminate the client's own provider from
// inside externalMethod, 19 publishes the nub the accelerator hangs under, and ReadRegs / the scanout set belong to the Navi48Bringup route.
enum Route : uint32_t { kRouteBringup = 0, kRouteAccelerator = 1 };
constexpr bool route_valid(uint32_t r) { return r == (uint32_t)kRouteBringup || r == (uint32_t)kRouteAccelerator; }
constexpr bool route_selector_ok(Route r, uint32_t sel) { return r == kRouteBringup || selector_allowed_for_app(sel); }
// The whole predicate in the order externalMethod applies it: the route first, then the 0.0.612 role rule, then the 0.0.640 APP rule. (The client calls the three functions one after another to keep its log lines apart; this
// composition is what tests/native_g6_test.cpp drives over route x role x selector and what the source pins hold the client to.)
constexpr bool selector_permitted(Route r, bool admin, bool app, uint32_t sel) {
    return route_selector_ok(r, sel) && selector_allowed(admin, sel) && (!app || selector_allowed_for_app(sel));
}
// start(provider): the provider must be what the route says and nothing else. Bringup route: the Navi48Bringup that is the client's owner. Accelerator route: the accelerator the aux kext registered (the pointer device_open received;
// identity only), never an arbitrary IOService and never the owner.
constexpr bool start_provider_ok(Route r, bool providerIsOwner, bool providerIsRegisteredAccelerator) {
    return r == kRouteBringup ? providerIsOwner : r == kRouteAccelerator ? providerIsRegisteredAccelerator : false;
}

// The boot-arg latch: 0 = not latched yet, 1 = latched OFF, 2 = latched ON. First writer wins, never re-read.
constexpr uint32_t kLatchUnset = 0u, kLatchOff = 1u, kLatchOn = 2u;
constexpr uint32_t latch_value(bool present, uint32_t value) { return (present && value == 1u) ? kLatchOn : kLatchOff; }
constexpr bool latch_is_on(uint32_t latch) { return latch == kLatchOn; }

} // namespace policy
} // namespace n48native
