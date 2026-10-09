// native_g6_test.cpp - build 0.0.656 (G6: sandboxed apps reach the N48N client through the IOAccelerator; an internal design note "G6 design" sections 2 and 4 step 1).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_g6_test.cpp -o /tmp/native_g6 && /tmp/native_g6 .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_g6_plant.sh plants breaks)
// Covers:
//   R1  the route x role x selector matrix (route_selector_ok / selector_permitted): the accelerator route reaches EXACTLY the APP set (0, 1, 3..8, 21) for every role (administrator, uid 88, APP), in
//       particular never 19 or 20; the Navi48Bringup route is the 0.0.612 / 0.0.640 rules bit for bit; route_valid; start_provider_ok over its truth table;
//   R2  native_open_verdict (the identity checks of the ops hook) over its truth table, and that its answer is an IOReturn, never a bool;
//   R3  the ops table: ABI 3, 152 bytes, native_open at offset 144, its own capability bit, kOpsCaps carries it, the two byte-identical header copies agree (when the aux tree is next to this one);
//   R4  source pins: the client attaches and starts on `attachTo` (never on the owner for the accelerator route), the route is stored before attach, start() asks start_provider_ok with the
//       owner / registered-accelerator identities, externalMethod's route check precedes the role checks, Navi48Bringup::newUserClient passes the Bringup route with itself, the hook creates the
//       client on the accelerator route, returns an IOReturn (never `rc == kIOReturnSuccess` as a bool), takes gLock only around the nub read; both ops tables end with op_native_open; the class
//       keeps its name and never derives from IOAccelerationUserClient (WebKit's GPU process denies that class by name).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <string>
#include <fstream>
#include <sstream>
#include "amd/native_open_policy_pure.h"
#include "amd/native_metal_pure.h"
#include "Navi48MetalOps.h"
#include "Navi48NativeABI.h"

using namespace n48native::policy;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}
static std::string slurp(const std::string &path) { std::ifstream f(path, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &needle) { size_t n = 0, p = 0; while ((p = s.find(needle, p)) != std::string::npos) { n++; p += needle.size(); } return n; }
static std::string fn_body(const std::string &src, const std::string &sig) {          // from the signature to its closing brace at column 0
    const size_t a = src.find(sig);
    if (a == std::string::npos) return "";
    const size_t e = src.find("\n}\n", a);
    return e == std::string::npos ? src.substr(a) : src.substr(a, e - a + 3);
}

// ---- R1 ------------------------------------------------------------------------------------------------------------------------------------------------
// The expected sets are written out here, NOT derived from the functions under test.
static bool in_app_set(uint32_t s) { return s == 0 || s == 1 || (s >= 3 && s <= 8) || s == 21; }
static bool in_ws_set(uint32_t s) { return s <= 14 || s == 21 || (s >= 22 && s <= 26); }   // 0.0.661: instance 2's scanout (22..26) is WindowServer's
static void r1_matrix() {
    expect_u("route Bringup is 0", kRouteBringup, 0); expect_u("route Accelerator is 1", kRouteAccelerator, 1);
    expect(route_valid(0) && route_valid(1) && !route_valid(2) && !route_valid(0xFFFFFFFFu), "only the two routes are valid");
    int accelAllowed = 0;
    for (uint32_t sel = 0; sel < 70; ++sel) {                                       // the ABI names 0..21; 22..69 are unknown numbers
        for (int admin = 0; admin < 2; ++admin) for (int app = 0; app < 2; ++app) {
            // roles: admin (root), uid-88 (non-admin, non-app), APP (non-admin, app); admin && app never happens (the open decision gives one reason), but the predicate must still be safe for it
            const bool acc = selector_permitted(kRouteAccelerator, admin != 0, app != 0, sel);
            expect(acc == in_app_set(sel), "ACCELERATOR route: exactly the APP selector set, for every role flag combination");
            if (sel == 19 || sel == 20) expect(!acc, "ACCELERATOR route never reaches the Metal nub selectors 19 / 20");
            if (sel == 2) expect(!acc, "ACCELERATOR route never reaches ReadRegs");
            if (sel >= 9 && sel <= 18) expect(!acc, "ACCELERATOR route never reaches the scanout / mode set");
            if (acc) accelAllowed++;
            // the Navi48Bringup route is the old rules
            const bool br = selector_permitted(kRouteBringup, admin != 0, app != 0, sel);
            bool want = admin ? true : in_ws_set(sel);
            if (app) want = want && in_app_set(sel);
            expect(br == want, "BRINGUP route: the 0.0.612 role rule and the 0.0.640 APP rule, unchanged");
            expect(selector_permitted(kRouteBringup, admin != 0, app != 0, sel) == (selector_allowed(admin != 0, sel) && (!app || selector_allowed_for_app(sel))), "BRINGUP route = the two old predicates and nothing else");
        }
    }
    expect_u("the accelerator route admits 9 selectors x 4 flag combinations over 0..69", accelAllowed, 9 * 4);
    // the named cases of the brief
    expect(selector_permitted(kRouteAccelerator, true, false, 4), "accelerator route, administrator, BoCreate: allowed");
    expect(!selector_permitted(kRouteAccelerator, true, false, 19), "accelerator route, administrator, selector 19: refused");
    expect(!selector_permitted(kRouteAccelerator, true, false, 20), "accelerator route, administrator, selector 20: refused");
    expect(selector_permitted(kRouteBringup, true, false, 20), "bringup route, administrator, selector 20: allowed (nub withdraw, as before)");
    expect(!selector_permitted(kRouteBringup, false, true, 20), "bringup route, APP, selector 20: refused");
    expect(!selector_permitted(kRouteAccelerator, false, false, 9), "accelerator route, uid 88, scanout: refused (WindowServer keeps the Bringup route for scanout)");
    expect(selector_permitted(kRouteBringup, false, false, 9), "bringup route, uid 88, scanout: allowed");
    expect(selector_permitted(kRouteAccelerator, false, true, 21), "accelerator route, APP, BoImportHost: allowed");
    // an out-of-range route value must not widen anything: it is not a route at all
    expect(!route_valid((uint32_t)7), "route 7 is not valid");
    // start_provider_ok truth table
    for (int own = 0; own < 2; ++own) for (int acc = 0; acc < 2; ++acc) {
        expect(start_provider_ok(kRouteBringup, own != 0, acc != 0) == (own != 0), "Bringup route: the provider must be the owner (the registered accelerator does not count)");
        expect(start_provider_ok(kRouteAccelerator, own != 0, acc != 0) == (acc != 0), "Accelerator route: the provider must be the registered accelerator (the owner does not count)");
        expect(!start_provider_ok((Route)9, own != 0, acc != 0), "an unknown route accepts no provider");
    }
}

// ---- R2 ------------------------------------------------------------------------------------------------------------------------------------------------
static void r2_verdict() {
    using namespace n48metal;
    for (int t = 0; t < 2; ++t) for (int d = 0; d < 2; ++d) for (int a = 0; a < 2; ++a) for (int n = 0; n < 2; ++n) {
        const uint32_t v = native_open_verdict(t != 0, d != 0, a != 0, n != 0);
        const bool all = t && d && a && n;
        expect((v == kOk) == all, "native_open succeeds only with the type, a live device, the registered accelerator AND a published nub");
        if (!t) expect_u("a wrong type is Unsupported", v, kUnsupported);
        if (t && !d) expect_u("no live device is NotReady", v, kNotReady);
        if (t && d && !a) expect_u("an accelerator that is not the registered one is BadArgument (identity)", v, kBadArg);
        if (t && d && a && !n) expect_u("no published nub is NotReady", v, kNotReady);
    }
    expect_u("kOk is kIOReturnSuccess = 0 (an IOReturn, not a bool)", kOk, 0u);
    expect(kUnsupported != kOk && kNotReady != kOk && kBadArg != kOk && kUnsupported != 1u && kNotReady != 1u && kBadArg != 1u, "no refusal collides with 0 or with a bool 1");
}

// ---- R3 ------------------------------------------------------------------------------------------------------------------------------------------------
static void r3_ops(const std::string &root) {
    expect_u("ops ABI is 3", N48_METAL_ABI, 3); expect_u("the oldest accepted ABI stays 1", N48_METAL_ABI_MIN, 1);
    expect_u("ABI 1 is 120 bytes", N48_METAL_OPS_MIN, 120); expect_u("ABI 2 is 144 bytes", N48_METAL_OPS_V2, 144); expect_u("ABI 3 is 152 bytes", N48_METAL_OPS_V3, 152);
    expect_u("sizeof(N48MetalOps)", sizeof(N48MetalOps), 152);
    expect_u("native_open sits right after the ABI-2 members", offsetof(N48MetalOps, native_open), 144);
    expect_u("disp_flags still at 120", offsetof(N48MetalOps, disp_flags), 120);
    expect_u("the capability bit is 1 << 1 (VHOOK stays 1 << 0)", N48_CAP_NATIVE_OPEN, 2); expect_u("VHOOK is bit 0", N48_CAP_VHOOK, 1);
    expect_u("kOpsCaps carries both", n48metal::kOpsCaps, N48_CAP_VHOOK | N48_CAP_NATIVE_OPEN);
    expect_u("the user-client type constant equals the ABI's 'N48N'", N48_METAL_UC_N48N, N48N_UC_TYPE);
    expect_u("the type is the four bytes N48N", N48_METAL_UC_N48N, 0x4E34384Eu);
    expect_u("trace event 15 is the native open", N48_TR_NATIVE_OPEN, 15);
    const std::string mine = slurp(root + "/src/navi48-bringup/src/Navi48MetalOps.h"), aux = slurp(root + "/tools/native/navi48accel/src/Navi48MetalOps.h");
    expect(!mine.empty(), "the bring-up copy of Navi48MetalOps.h is readable");
    if (!aux.empty()) expect(mine == aux, "the two Navi48MetalOps.h copies are byte-identical");
}

// ---- R4 ------------------------------------------------------------------------------------------------------------------------------------------------
static void r4_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/src/";
    const std::string cli = slurp(K + "Navi48NativeClient.cpp"), clih = slurp(K + "Navi48NativeClient.hpp"), nub = slurp(K + "Navi48MetalNub.cpp"), nubh = slurp(K + "Navi48MetalNub.hpp"), br = slurp(K + "Navi48Bringup.cpp");
    expect(!cli.empty() && !clih.empty() && !nub.empty() && !br.empty(), "the sources are readable");
    // the class name and base: WindowServer's sandbox wants an IOAccel* name, WebKit's GPU process denies IOAccelerationUserClient by name
    expect(clih.find("class IOAccelNavi48NativeClient : public IOUserClient {") != std::string::npos, "the class keeps its name and derives from IOUserClient");
    expect(clih.find("IOAccelerationUserClient") == std::string::npos && cli.find("IOAccelerationUserClient") == std::string::npos && nub.find("IOAccelerationUserClient") == std::string::npos, "nothing in the kext names IOAccelerationUserClient");
    // create
    expect(clih.find("static IOReturn create(Navi48Bringup *owner, IOService *attachTo, n48native::policy::Route route, task_t owningTask, void *securityID, UInt32 type, OSDictionary *properties,") != std::string::npos, "create takes the owner, the service to attach to and the route");
    const std::string cr = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::create(");
    expect(!cr.empty(), "create found");
    expect(cr.find("uc->attach(attachTo)") != std::string::npos && cr.find("uc->start(attachTo)") != std::string::npos && cr.find("uc->detach(attachTo)") != std::string::npos, "create attaches, starts and (on failure) detaches on attachTo");
    expect(cr.find("attach(owner)") == std::string::npos && cr.find("start(owner)") == std::string::npos && cr.find("detach(owner)") == std::string::npos, "create never attaches or starts on the owner");
    expect_u("the owner is stored once", count_of(cr, "uc->owner = owner;"), 1u);
    { const size_t pRoute = cr.find("uc->route = route;"), pAtt = cr.find("uc->attach(attachTo)"), pOpen = cr.find("amdgpu::n1c_open("), pInit = cr.find("uc->initWithTask(");
      expect(pInit != std::string::npos && pRoute != std::string::npos && pAtt != std::string::npos && pOpen != std::string::npos && pInit < pRoute && pRoute < pOpen && pOpen < pAtt, "order: initWithTask (admission), route stored, n1c_open, attach (the route is set before start() asks for it)"); }
    expect(cr.find("if (!n48native::policy::route_valid((uint32_t)route)) return kIOReturnBadArgument;") != std::string::npos, "create refuses an unknown route");
    expect(cr.find("route == n48native::policy::kRouteBringup && static_cast<IOService *>(owner) != attachTo") != std::string::npos &&
           cr.find("route == n48native::policy::kRouteAccelerator && static_cast<IOService *>(owner) == attachTo") != std::string::npos, "create cross-checks route and attachTo (Bringup = the owner, Accelerator = never the owner)");
    // start
    const std::string st = fn_body(cli, "bool IOAccelNavi48NativeClient::start(IOService *provider) {");
    expect(!st.empty() && st.find("n48native::policy::start_provider_ok(route, isOwner, isAccel)") != std::string::npos, "start asks start_provider_ok(route, ...)");
    expect(st.find("provider == static_cast<IOService *>(owner) && OSDynamicCast(Navi48Bringup, provider) != nullptr") != std::string::npos, "start: the owner identity is the Navi48Bringup that created the client");
    expect(st.find("provider == Navi48MetalNub::registeredAccelerator()") != std::string::npos, "start: the accelerator identity is Navi48MetalNub::registeredAccelerator()");
    expect(st.find("provider != nullptr") != std::string::npos && st.find("return false;") != std::string::npos && st.find("super::start(provider)") != std::string::npos && st.find("return false;") < st.find("super::start(provider)"), "start refuses before the base class starts");
    // externalMethod: the route check precedes the role checks and returns NotPrivileged
    const std::string ext = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::externalMethod(");
    { const size_t pR = ext.find("n48native::policy::route_selector_ok(route, selector)"), pA = ext.find("n48native::policy::selector_allowed(adminClient, selector)"), pP = ext.find("selector_allowed_for_app(selector)"), pSw = ext.find("switch (selector) {");
      expect(pR != std::string::npos && pA != std::string::npos && pP != std::string::npos && pSw != std::string::npos && pR < pA && pA < pP && pP < pSw, "externalMethod: route check, then the role rule, then the APP rule, then the switch");
      if (pR != std::string::npos && pA != std::string::npos) { const std::string blk = ext.substr(pR, pA - pR); expect(blk.find("return kIOReturnNotPrivileged;") != std::string::npos && blk.find("NCLOG(") != std::string::npos, "the route check logs (rate limited) and returns kIOReturnNotPrivileged"); } }
    expect_u("route_selector_ok is the only route test", count_of(ext, "route_selector_ok("), 1u);
    expect(clih.find("n48native::policy::Route route { n48native::policy::kRouteBringup };") != std::string::npos, "the route defaults to the Bringup route (the old behaviour)");
    // the Bringup entry point
    expect(br.find("IOAccelNavi48NativeClient::create(this, this, n48native::policy::kRouteBringup, owningTask, securityID, type, properties, handler)") != std::string::npos && count_of(br, "IOAccelNavi48NativeClient::create(") == 1u, "Navi48Bringup::newUserClient creates on the Bringup route with itself, and is the only caller besides the nub hook");
    expect_u("the nub hook is the only other caller of create", count_of(nub, "IOAccelNavi48NativeClient::create("), 1u);
    // the hook
    const std::string op = fn_body(nub, "int32_t op_native_open(");
    expect(!op.empty(), "op_native_open found");
    expect(op.find("IOAccelNavi48NativeClient::create(owner, static_cast<IOService *>(accel), n48native::policy::kRouteAccelerator, (task_t)task, securityID, type, nullptr, &uc)") != std::string::npos, "the hook creates the SAME client class on the accelerator route, attached to `accel`");
    expect(op.find("n48metal::native_open_verdict(type == N48_METAL_UC_N48N, live, isAccel, nub != nullptr)") != std::string::npos, "the hook decides with native_open_verdict");
    expect(op.find("accel == d->accel") != std::string::npos && op.find("d == __atomic_load_n(&gDev, __ATOMIC_ACQUIRE)") != std::string::npos && op.find("d->magic == kDevMagic") != std::string::npos, "identity: the ctx is the live device and `accel` is the one device_open saw");
    expect(op.find("return (int32_t)rc;") != std::string::npos && op.find("return (int32_t)v;") != std::string::npos && op.find("rc == kIOReturnSuccess)") != std::string::npos, "the hook returns IOReturn values (rc / v), compared with kIOReturnSuccess where it must branch");
    expect(op.find("return 1;") == std::string::npos && op.find("return true;") == std::string::npos && op.find("return rc ==") == std::string::npos && op.find("return !") == std::string::npos, "the hook never returns a bool-shaped value");
    { const size_t pLock = op.find("IOLockLock(gLock);"), pUnl = op.find("IOLockUnlock(gLock);"), pCreate = op.find("IOAccelNavi48NativeClient::create("), pRel = op.find("nub->release();", pCreate);
      expect(pLock != std::string::npos && pUnl != std::string::npos && pCreate != std::string::npos && pLock < pUnl && pUnl < pCreate && pCreate < pRel, "gLock is dropped BEFORE create (the client lock is taken inside it; hungLatched takes gLock under that lock), and the nub reference is released after");
      expect(op.find("nub->retain()") != std::string::npos && count_of(op, "IOLockLock(gLock)") == 1u, "the nub is retained under the lock, the lock is taken once"); }
    expect(op.find("*handler = uc;") != std::string::npos && op.find("*handler = uc;") > op.find("rc == kIOReturnSuccess"), "*handler is written only on success");
    // device_open binds the accelerator; registeredAccelerator reads it
    expect(nub.find("d->accel = accel;") != std::string::npos && nub.find("if (!nub || !accel || gDev) return nullptr;") != std::string::npos, "device_open stores the accelerator and refuses a NULL one");
    expect(nubh.find("static IOService *registeredAccelerator();") != std::string::npos, "the accessor is declared");
    expect(fn_body(nub, "IOService *Navi48MetalNub::registeredAccelerator() {").find("d->magic == kDevMagic") != std::string::npos, "registeredAccelerator checks the live device");
    // both tables
    expect_u("both tables end with op_native_open", count_of(nub, "\top_native_open,\n};"), 2u);
    expect(nub.find("const N48MetalOps gOps = {") != std::string::npos && nub.find("const N48MetalOps gOpsOff = {") != std::string::npos && nub.find("gOpsV1") == std::string::npos, "the tables are gOps (display on) and gOpsOff (display off): both ABI 3");
    expect_u("both tables are built at 664", count_of(nub, "(uint32_t)sizeof(N48MetalOps), 664u,"), 2u);
    expect(nub.find("n48disp::ops_shape(n48disp_latched_on()).dispFlags != 0u ? &gOps : &gOpsOff;") != std::string::npos, "callPlatformFunction hands out gOps only with the display latch ON");
    expect(nub.find("static_assert(N48_METAL_UC_N48N == N48N_UC_TYPE,") != std::string::npos, "the type constants are tied at compile time");
    // the Info.plist version
    const std::string plist = slurp(root + "/src/navi48-bringup/Info.plist");
    expect(plist.find("<string>0.0.664</string>") != std::string::npos && plist.find("0.0.654") == std::string::npos, "Info.plist says 0.0.664");
    expect(slurp(K + "amd/native_s1c.h").find("kN1cKextBuild = 664;") != std::string::npos, "Hello / QueryInfo report build 664");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    r1_matrix(); r2_verdict(); r3_ops(root); r4_pins(root);
    std::printf("native_g6_test: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
