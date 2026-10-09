// native_ws_open_test.cpp - build 0.0.612 (milestone #11 step 11c, an internal design note sections 3, 7, 8): W1 the class rename, W2 the open policy (uid 88 behind
// navi48-metal-ws=1), W3 the "Navi48,Ready" property and where the HUNG latch writes it.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_ws_open_test.cpp -o /tmp/native_ws_open && /tmp/native_ws_open .
//   (run from the repo root; the argument is the repo root the source pins read from; tests/native_ws_open_plant.sh plants breaks)
// Covers:
//   O1  open_decision over the WHOLE input space (admin x uid x boot-arg x already-open): root admitted; uid 88 with / without the arg; uid 501 refused; uid 88 + arg + already open refused;
//       OFF identity: with the boot-arg absent the decision is exactly "admit iff administrator" for every uid and every already-open value;
//   O2  the boot-arg latch values (absent / 0 / 1 / 2 -> only 1 is ON), first writer wins;
//   O3  source pins for W1 / W2: the class is IOAccelNavi48NativeClient (the sandbox prefix), the old class name is gone from the kext, the service name and the type are unchanged, the
//       policy call sits in initWithTask with the uid of the opener, one log line on each admit / refuse with the uid and the reason, the boot-arg is read in ONE place and latched
//       (never re-read), no other user of the old name in tools/native;
//   O5  who may call what (review items B and E): selector_allowed over every selector for admin / uid-88 clients, import_caller_ok, source pins of the dispatch;
//   O4  Ready: ready_at_publish / ready_sticky_after truth tables; a REACHABILITY model that drives the real HUNG state machine (hang_detect / hang_latch_wait) into the announce and
//       shows Ready goes 1 -> 0 and never back; source pins of the real wiring (both latch sites announce after the latch, hang_log has one caller, hungLatched writes the property).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <filesystem>
#include "amd/native_open_policy_pure.h"
#include "amd/native_metal_pure.h"
#include "amd/native_s1c_pure.h"
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

// ---- O1 ------------------------------------------------------------------------------------------------------------------------------------------------
static void o1_policy() {
    // the named cases of the brief
    { const OpenDecision d = open_decision(true, 0, false, false);   expect(d.admit && d.reason == kReasonAdmin, "root (administrator), no boot-arg: admitted"); }
    { const OpenDecision d = open_decision(true, 0, true, false);    expect(d.admit && d.reason == kReasonAdmin, "root with the boot-arg: admitted as an administrator"); }
    { const OpenDecision d = open_decision(false, 88, true, false);  expect(d.admit && d.reason == kReasonWindowServer, "uid 88 with navi48-metal-ws=1, nothing open: admitted"); }
    { const OpenDecision d = open_decision(false, 88, false, false); expect(!d.admit && d.reason == kReasonWsArgOff, "uid 88 without the boot-arg: refused"); }
    { const OpenDecision d = open_decision(false, 501, true, false); expect(!d.admit && d.reason == kReasonNotPrivileged, "uid 501 (a user) with the boot-arg: refused"); }
    { const OpenDecision d = open_decision(false, 501, false, false); expect(!d.admit && d.reason == kReasonNotPrivileged, "uid 501 without the boot-arg: refused"); }
    { const OpenDecision d = open_decision(false, 88, true, true);   expect(!d.admit && d.reason == kReasonWsAlreadyOpen, "uid 88 + the boot-arg + a client already open: refused"); }
    { const OpenDecision d = open_decision(true, 0, false, true);    expect(d.admit && d.reason == kReasonAdmin, "root with a client open: the policy admits; n1c_open's ExclusiveAccess refuses it, exactly as before 0.0.612"); }
    expect_u("the uid is 88", kWindowServerUid, 88);
    // uid neighbours are not _windowserver
    const uint32_t uids[] = { 0u, 1u, 87u, 88u, 89u, 501u, 502u, 0x80000058u, 0xFFFFFFFFu, 88u + 0x100u };
    for (uint32_t uid : uids) for (int admin = 0; admin < 2; admin++) for (int arg = 0; arg < 2; arg++) for (int open = 0; open < 2; open++) {
        const OpenDecision d = open_decision(admin != 0, uid, arg != 0, open != 0);
        // OFF identity: the boot-arg absent is the old policy bit for bit (admit iff administrator)
        if (!arg) expect(d.admit == (admin != 0), "OFF identity: with navi48-metal-ws absent the decision is exactly 'admit iff administrator'");
        // an administrator is always admitted, for its own reason
        if (admin) expect(d.admit && d.reason == kReasonAdmin, "an administrator is admitted whatever the uid, boot-arg or open state");
        // only uid 88 can be admitted without being an administrator, and only with the arg and nothing open
        if (!admin) expect(d.admit == (uid == kWindowServerUid && arg && !open), "a non-administrator is admitted only as uid 88 with the boot-arg and no client open");
        // admit and refuse reasons are consistent
        expect(d.admit == (d.reason == kReasonAdmin || d.reason == kReasonWindowServer), "admit iff the reason is an admit reason");
        expect(d.reason < kReasonCount && open_reason_text(d.reason) != nullptr && std::strcmp(open_reason_text(d.reason), "?") != 0, "every reason has a text");
    }
    // the five reason texts are distinct (the log line says which)
    for (uint32_t a = 0; a < kReasonCount; a++) for (uint32_t b = a + 1; b < kReasonCount; b++)
        expect(std::strcmp(open_reason_text((OpenReason)a), open_reason_text((OpenReason)b)) != 0, "reason texts are distinct");
    expect(std::strcmp(open_reason_text((OpenReason)99), "?") == 0, "an out-of-range reason has no text");
}

// ---- O2 ------------------------------------------------------------------------------------------------------------------------------------------------
static void o2_latch() {
    expect_u("boot-arg absent latches OFF", latch_value(false, 0), kLatchOff);
    expect_u("boot-arg absent (garbage value) latches OFF", latch_value(false, 1), kLatchOff);
    expect_u("navi48-metal-ws=0 latches OFF", latch_value(true, 0), kLatchOff);
    expect_u("navi48-metal-ws=1 latches ON", latch_value(true, 1), kLatchOn);
    expect_u("navi48-metal-ws=2 latches OFF (only 1 enables)", latch_value(true, 2), kLatchOff);
    expect_u("navi48-metal-ws=0xffffffff latches OFF", latch_value(true, 0xFFFFFFFFu), kLatchOff);
    expect(!latch_is_on(kLatchUnset) && !latch_is_on(kLatchOff) && latch_is_on(kLatchOn), "only the ON latch value enables");
    // first writer wins (the kext's compare-and-swap from Unset): model
    uint32_t w = kLatchUnset;
    auto cas = [&](uint32_t v) { if (w == kLatchUnset) w = v; };
    cas(latch_value(true, 1)); cas(latch_value(false, 0));
    expect_u("a second latch attempt cannot flip the first", w, kLatchOn);
}

// ---- O3 ------------------------------------------------------------------------------------------------------------------------------------------------
static void o3_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string cli = slurp(K + "src/Navi48NativeClient.cpp"), clih = slurp(K + "src/Navi48NativeClient.hpp"), br = slurp(K + "src/Navi48Bringup.cpp"), plist = slurp(K + "Info.plist");
    expect(!cli.empty() && !clih.empty() && !br.empty() && !plist.empty(), "the sources are readable from the root given");
    // W1: the class. WindowServer's sandbox admits iokit-open for a class name starting with IOAccel.
    expect_u("the metaclass is defined once, under the IOAccel-prefixed name", count_of(cli, "OSDefineMetaClassAndStructors(IOAccelNavi48NativeClient, IOUserClient)"), 1);
    expect(clih.find("class IOAccelNavi48NativeClient : public IOUserClient {") != std::string::npos && clih.find("OSDeclareDefaultStructors(IOAccelNavi48NativeClient)") != std::string::npos, "the header declares the renamed class");
    expect(std::string("IOAccelNavi48NativeClient").compare(0, 7, "IOAccel") == 0, "the class name starts with the sandbox's IOAccel prefix");
    {   // the old class name is gone as a C++ token in every kext source (file names and header guards keep it)
        const char *files[] = { "src/Navi48NativeClient.cpp", "src/Navi48NativeClient.hpp", "src/Navi48Bringup.cpp", "src/Navi48MetalNub.cpp", "src/amd/native_s1c.cpp" };
        for (const char *f : files) {
            std::string s = slurp(K + f);
            size_t bad = 0;
            for (size_t p = 0; (p = s.find("Navi48NativeClient", p)) != std::string::npos; p += 18) {
                const bool prefixed = p >= 7 && s.compare(p - 7, 7, "IOAccel") == 0;
                const bool inFile = (p >= 1 && s[p - 1] == '"') || (p + 18 < s.size() && (s[p + 18] == '.' || s[p + 18] == '_'));   // "Navi48NativeClient.cpp", the header guard
                const bool inComment = [&] { const size_t ls = s.rfind('\n', p); const std::string line = s.substr(ls == std::string::npos ? 0 : ls + 1, p - (ls == std::string::npos ? 0 : ls + 1)); return line.find("//") != std::string::npos || line.find("/*") != std::string::npos; }();
                if (!prefixed && !inFile && !inComment) bad++;
            }
            expect_u("no kext source uses the old class name as code", bad, 0);
        }
    }
    expect(br.find("return IOAccelNavi48NativeClient::create(this, this, n48native::policy::kRouteBringup, owningTask, securityID, type, properties, handler);") != std::string::npos, "newUserClient creates the renamed class for type N48N");
    expect_u("the user-client type is still 'N48N'", N48N_UC_TYPE, 0x4E34384Eu);
    expect(br.find("if (type == N48N_UC_TYPE)") != std::string::npos, "the type switch is unchanged");
    expect(plist.find("Navi48NativeClient") == std::string::npos, "no personality names the user-client class (it is created by newUserClient)");
    {   // no user of the old class name outside the kext: tools/native
        std::error_code ec;
        int hits = 0, scanned = 0;
        for (auto it = std::filesystem::recursive_directory_iterator(root + "/tools/native", ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const std::string ps = it->path().string();
            if (ps.find("/mtlprobe/") != std::string::npos || ps.find("/shader-compile-census/") != std::string::npos) continue;   // owned elsewhere; they never look the class up by name
            if (!it->is_regular_file()) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".c" && ext != ".m" && ext != ".mm" && ext != ".h" && ext != ".cpp" && ext != ".sh" && ext != ".py" && ext != ".plist") continue;
            if (it->file_size() > (1u << 20)) continue;
            scanned++;
            if (slurp(it->path().string()).find("Navi48NativeClient") != std::string::npos) { hits++; std::printf("  old class name in %s\n", it->path().string().c_str()); }
        }
        expect(scanned > 0, "tools/native was scanned");
        expect_u("no tools/native file names the old class", (uint64_t)hits, 0);
    }
    // W2: the policy wiring
    const std::string init = fn_body(cli, "bool IOAccelNavi48NativeClient::initWithTask(");
    expect(!init.empty(), "initWithTask is found");
    expect(init.find("n48native::policy::open_decision(admin, uid, n48native::policy::latch_is_on(gMetalWsLatch),") != std::string::npos && init.find("n48native::g2::ws_already_open(appsOn, amdgpu::n1c_is_open(), amdgpu::n1c_ws_is_open())") != std::string::npos, "initWithTask asks the pure decision, with the latched boot-arg and the open state (0.0.641: apps ON = a WindowServer session, else any native client)");
    expect(init.find("clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator) == kIOReturnSuccess") != std::string::npos, "the administrator test is the old clientHasPrivilege call, unchanged");
    expect(init.find("kauth_cred_getuid(kauth_cred_get())") != std::string::npos, "the uid is the opener's effective uid");
    expect(init.find("if (!d.admit)") != std::string::npos && init.find("return false;") != std::string::npos, "a refused decision fails initWithTask (NotPrivileged)");
    expect(init.find("privileged = true;") > init.find("if (!d.admit)") && init.find("task = owningTask;") > init.find("privileged = true;"), "privileged / task are set only after the decision admitted");
    expect_u("one log line on refuse, with the uid and the reason", count_of(init, "NCLOG(\"open refused: uid %u, reason: %s\", uid, n48native::policy::open_reason_text(d.reason));"), 1);
    expect_u("one log line on admit, with the uid and the reason", count_of(init, "NCLOG(\"open admitted: uid %u, reason: %s\", uid, n48native::policy::open_reason_text(d.reason));"), 1);
    expect_u("the refuse line is the only refusal exit", count_of(init, "return false;"), 2);   // super::initWithTask failing + the refusal
    // the boot-arg: ONE reader, latched once (compare-and-swap from Unset), read nowhere else
    expect_u("boot-arg navi48-metal-ws is read in exactly one place in the whole kext", count_of(cli, "PE_parse_boot_argn(\"navi48-metal-ws\""), 1);
    for (const char *f : { "src/Navi48Bringup.cpp", "src/Navi48MetalNub.cpp", "src/amd/native_s1c.cpp", "src/dcn/navi48_dcn.cpp", "src/Navi48UserClient.cpp" })
        expect(slurp(K + f).find("navi48-metal-ws") == std::string::npos || std::string(f) == "src/Navi48Bringup.cpp", "no other source reads navi48-metal-ws");
    expect(cli.find("(void)OSCompareAndSwap(n48native::policy::kLatchUnset, n48native::policy::latch_value(present, v), &gMetalWsLatch);") != std::string::npos, "the latch is a compare-and-swap from Unset (first writer wins, never re-read)");
    expect_u("the latch is written only in latchBootArgs", count_of(cli, "&gMetalWsLatch"), 1);
    expect(br.find("IOAccelNavi48NativeClient::latchBootArgs();") != std::string::npos, "runStages latches the boot-arg at start");
    expect(init.find("if (gMetalWsLatch == n48native::policy::kLatchUnset) latchBootArgs();") != std::string::npos && init.find("latchBootArgs();") < init.find("open_decision("), "initWithTask latches (if start never did) BEFORE it decides");
    expect(init.find("clientHasPrivilege") < init.find("open_decision("), "the privilege query precedes the decision");
    // the service and the open path are unchanged
    expect(cli.find("const IOReturn rc = amdgpu::n1c_open(*owner->bringupContext(), uc->wsSession, &ref);") != std::string::npos && cli.find("return kIOReturnNotPrivileged;   // non-root") != std::string::npos, "create() still refuses with NotPrivileged and opens through n1c_open (exclusivity with the switch OFF, VMID 8; 0.0.627: the WindowServer flag and the session reference)");
    expect_u("still exactly one native VMID", n48native::kNativeVmid, 8);
}

// ---- O4 ------------------------------------------------------------------------------------------------------------------------------------------------
namespace {
// A model of hang_poll / hang_from_wait + hang_announce + the nub: it runs the REAL state machine (hang_detect / hang_latch_wait from native_s1c_pure.h) and the real ready rules.
struct NubModel {
    bool published = false;
    uint32_t readyProp = 99;   // the property as the registry shows it (99 = none)
    bool stickyNo = false;
    void publish(bool hungNow) { if (published) return; published = true; readyProp = n48metal::ready_at_publish(stickyNo, hungNow); }
    void hungLatched() { stickyNo = n48metal::ready_sticky_after(stickyNo, true); if (published) readyProp = n48metal::kReadyNo; }
};
struct Engine {
    n48native::s1c::Hang hang { 0, 0, false };
    NubModel *nub;
    int announces = 0;
    void announce() { announces++; nub->hungLatched(); }
    bool hang_poll(uint64_t retired, uint64_t emitted, uint64_t now) { const bool det = n48native::s1c::hang_detect(hang, retired, emitted, now); if (det) announce(); return det; }
    bool hang_from_wait() { const bool det = n48native::s1c::hang_latch_wait(hang); if (det) announce(); return det; }
};
}
static void o4_ready() {
    expect_u("ready constants", n48metal::kReadyYes, 1); expect_u("not ready constant", n48metal::kReadyNo, 0);
    expect_u("publish: no hang ever -> 1", n48metal::ready_at_publish(false, false), 1);
    expect_u("publish: a hang was latched earlier this boot -> 0", n48metal::ready_at_publish(true, false), 0);
    expect_u("publish: the gate sees HUNG now -> 0", n48metal::ready_at_publish(false, true), 0);
    expect_u("publish: both -> 0", n48metal::ready_at_publish(true, true), 0);
    expect(!n48metal::ready_sticky_after(false, false), "no event: still usable");
    expect(n48metal::ready_sticky_after(false, true), "a hang event makes it sticky");
    expect(n48metal::ready_sticky_after(true, false) && n48metal::ready_sticky_after(true, true), "sticky never clears");
    // REACHABILITY: publish, then the real latch trips through hang_poll: Ready goes 1 -> 0, exactly one announce, and a later publish never shows 1
    {
        NubModel nub; Engine e; e.nub = &nub;
        nub.publish(false);
        expect_u("Ready is 1 after a healthy publish", nub.readyProp, 1);
        expect(!e.hang_poll(0, 0, 10), "an idle ring does not trip the latch");
        expect(!e.hang_poll(3, 5, 1000), "busy for under 2 s: no latch");
        expect_u("Ready still 1 while healthy", nub.readyProp, 1);
        expect(e.hang_poll(3, 5, 1000ull + n48native::s1c::kHangNs + 1), "no progress for 2 s while busy: THIS call latches");
        expect_u("the latch announced exactly once", (uint64_t)e.announces, 1);
        expect_u("Ready is 0 after the latch", nub.readyProp, 0);
        expect(!e.hang_poll(3, 5, 9000000000ull) && !e.hang_from_wait(), "an already-latched state is not a new detection");
        expect_u("no second announce", (uint64_t)e.announces, 1);
        expect_u("Ready never goes back to 1", nub.readyProp, 0);
    }
    // the wait-cap latch (hang_from_wait) announces too
    {
        NubModel nub; Engine e; e.nub = &nub;
        nub.publish(false);
        expect(e.hang_from_wait(), "a bounded wait that reached 2 s latches");
        expect_u("Ready is 0 after the wait latch", nub.readyProp, 0);
    }
    // a latch BEFORE any publish: the nub is published later carrying 0 (the gate refuses it anyway), and a hang with no nub is remembered
    {
        NubModel nub; Engine e; e.nub = &nub;
        expect(e.hang_from_wait(), "latch with no nub published");
        expect(nub.stickyNo && nub.readyProp == 99, "remembered, no property yet");
        nub.publish(false);
        expect_u("a nub published after a latch carries Ready = 0", nub.readyProp, 0);
    }
}
static void o4_pins(const std::string &root) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string eng = slurp(K + "src/amd/native_s1c.cpp"), nub = slurp(K + "src/Navi48MetalNub.cpp"), nubh = slurp(K + "src/Navi48MetalNub.hpp");
    expect(!eng.empty() && !nub.empty() && !nubh.empty(), "the sources are readable from the root given");
    const std::string ann = fn_body(eng, "static void hang_announce(");
    const std::string poll = fn_body(eng, "static bool hang_poll()"), wait = fn_body(eng, "static void hang_from_wait()");
    expect(!ann.empty() && !poll.empty() && !wait.empty(), "hang_announce, hang_poll and hang_from_wait are found");
    // where the latch is set: hang_detect / hang_latch_wait; the property write is reached right after, and ONLY after
    expect(poll.find("hang_detect(gHang") != std::string::npos && poll.find("if (det) hang_announce(em, re);") != std::string::npos && poll.find("hang_detect(gHang") < poll.find("hang_announce("), "hang_poll: the latch is set, then announced");
    expect(wait.find("hang_latch_wait(gHang)") != std::string::npos && wait.find("if (det) hang_announce(em, re);") != std::string::npos && wait.find("hang_latch_wait(gHang)") < wait.find("hang_announce("), "hang_from_wait: the latch is set, then announced");
    expect(poll.find("IOLockUnlock(gHangLock);") < poll.find("hang_announce(") && wait.find("IOLockUnlock(gHangLock);") < wait.find("hang_announce("), "the announce runs with the hang lock released");
    expect_u("hang_announce has exactly three callers (0.0.627: + stall_handle, after its own hang_latch_wait)", count_of(eng, "hang_announce(em, re)"), 3);
    { const std::string sh = fn_body(eng, "static bool stall_handle(uint64_t em, uint64_t re) {");
      expect(!sh.empty() && sh.find("const bool det = hang_latch_wait(gHang);") < sh.find("if (det) hang_announce(em, re);") && sh.find("IOLockUnlock(gHangLock);\n    if (det) hang_announce(em, re);") != std::string::npos, "stall_handle: the latch is set, then announced, with the hang lock released"); }
    expect_u("hang_log has exactly one caller: hang_announce", count_of(eng, "hang_log(emitted, retired);"), 1);
    expect(count_of(eng, "hang_log(") == 2, "hang_log is defined once and called once");
    expect(ann.find("hang_log(emitted, retired);") != std::string::npos && ann.find("Navi48MetalNub::hungLatched();") != std::string::npos && ann.find("hang_log(") < ann.find("Navi48MetalNub::hungLatched();"), "hang_announce logs, then writes the property");
    expect_u("the engine itself never writes gHang.hung (only the pure latch functions do)", count_of(eng, ".hung = true") + count_of(eng, "hung = true;"), 0);
    {   // the pure latch functions are the only writers of `hung`
        const std::string pure = slurp(K + "src/amd/native_s1c_pure.h");
        expect_u("the pure header sets hung in exactly two places (hang_detect, hang_latch_wait)", count_of(pure, "h.hung = true;"), 2);
    }
    // the property itself
    const std::string lat = fn_body(nub, "void Navi48MetalNub::hungLatched()");
    expect(!lat.empty(), "hungLatched is found");
    expect(lat.find("nub->setProperty(\"Navi48,Ready\", (unsigned long long)n48metal::kReadyNo, 32);") != std::string::npos, "hungLatched writes Navi48,Ready = 0 (kReadyNo) on the published nub");
    expect(lat.find("__atomic_store_n(&gReadyNo") != std::string::npos && lat.find("__atomic_store_n(&gReadyNo") < lat.find("IOLockLock(gLock);"), "the sticky word is stored BEFORE the nub lock is taken");
    expect(lat.find("(unsigned long long)1") == std::string::npos && lat.find("kReadyYes") == std::string::npos, "hungLatched never writes 1");
    expect_u("Navi48,Ready is written in exactly two places: publish (from ready_at_publish) and hungLatched", count_of(nub, "\"Navi48,Ready\""), 2);
    expect(nub.find("nub->setProperty(\"Navi48,Ready\", ready);") != std::string::npos && nub.find("n48metal::ready_at_publish(__atomic_load_n(&gReadyNo, __ATOMIC_ACQUIRE) != 0u, g.hung)") != std::string::npos, "publish sets Ready from ready_at_publish (sticky state and the gate's HUNG)");
    expect(nubh.find("static void hungLatched();") != std::string::npos, "the header declares hungLatched");
    expect(nub.find("volatile uint32_t      gReadyNo = 0;") != std::string::npos, "the sticky word is initialised usable");
    {   // nothing else in the kext can set the property back to 1
        for (const char *f : { "src/Navi48NativeClient.cpp", "src/Navi48Bringup.cpp", "src/amd/native_s1c.cpp", "src/dcn/navi48_dcn.cpp" })
            expect(slurp(K + f).find("setProperty(\"Navi48,Ready\"") == std::string::npos, "no other source writes Navi48,Ready");
    }
}

// ---- O5 (review items B and E): who may call what -----------------------------------------------------------------------------------------------------
static void o5_reach(const std::string &root) {
    const uint32_t allowedWs[] = { N48N_SEL_HELLO, N48N_SEL_QUERYINFO, N48N_SEL_READREGS, N48N_SEL_BOCREATE, N48N_SEL_BOFREE, N48N_SEL_GEMVA, N48N_SEL_CTX, N48N_SEL_SUBMIT, N48N_SEL_WAITSEQ,
                                  N48N_SEL_SCAN_QUERY, N48N_SEL_SCAN_ACQUIRE, N48N_SEL_SCAN_REGISTER, N48N_SEL_SCAN_PRESENT, N48N_SEL_SCAN_STATUS, N48N_SEL_SCAN_RELEASE, N48N_SEL_BO_IMPORT_HOST,
                                  N48N_SEL_SCANX_ACQUIRE, N48N_SEL_SCANX_REGISTER, N48N_SEL_SCANX_PRESENT, N48N_SEL_SCANX_STATUS, N48N_SEL_SCANX_RELEASE };   // 0.0.661: instance 2's scanout (22..26) is WindowServer's too
    const uint32_t refusedWs[] = { N48N_SEL_DAL_STEP, N48N_SEL_MODE_TRIAL, N48N_SEL_MODE_HOLD, N48N_SEL_MODE_RELEASE, N48N_SEL_METAL_NUB_PUBLISH, N48N_SEL_METAL_NUB_WITHDRAW,
                                   27u, 28u, 64u, 255u, 0x80000000u, 0xFFFFFFFFu };
    for (uint32_t sel : allowedWs) {
        expect(selector_allowed(false, sel) && selector_allowed(true, sel), "a uid-88 client may call the WindowServer set (Hello .. WaitSeq, scanout, BoImportHost)");
        expect(selector_allowed_for_windowserver(sel), "the WindowServer set holds it");
    }
    for (uint32_t sel : refusedWs) {
        expect(!selector_allowed(false, sel), "a uid-88 client may NOT call DAL / mode trial / hold / release / nub publish / nub withdraw / an unknown selector");
        expect(selector_allowed(true, sel), "an administrator client reaches every selector, unchanged");
    }
    // the exact set over every number the ABI names, and beyond: 16 allowed, 6 refused below the ABI count
    uint32_t nAllowed = 0, nRefusedBelowCount = 0;
    for (uint32_t sel = 0; sel < 300u; sel++) {
        expect(selector_allowed(true, sel), "administrator: every selector is allowed");
        if (selector_allowed(false, sel)) nAllowed++; else if (sel < N48N_SEL_COUNT_1_9) nRefusedBelowCount++;
        expect(selector_allowed(false, sel) == (sel <= 14u || sel == 21u || (sel >= 22u && sel <= 26u)), "uid 88: exactly 0..14, 21 and (0.0.661) 22..26");
    }
    expect_u("the WindowServer set has 21 selectors (0.0.661: + 22..26)", nAllowed, 21u);
    expect_u("six ABI selectors are refused to it (15..20)", nRefusedBelowCount, 6u);
    expect_u("selector 21 is BoImportHost", N48N_SEL_BO_IMPORT_HOST, 21u); expect_u("selector 14 is the last scanout selector", N48N_SEL_SCAN_RELEASE, 14u);
    expect_u("selector 8 is WaitSeq", N48N_SEL_WAITSEQ, 8u); expect_u("selector 15 is the DAL step", N48N_SEL_DAL_STEP, 15u); expect_u("selector 19 is the nub publish", N48N_SEL_METAL_NUB_PUBLISH, 19u);
    // B: only the owning task
    int a = 0, b = 0;
    expect(import_caller_ok(&a, &a), "B: the owning task may import");
    expect(!import_caller_ok(&b, &a), "B: another task may not");
    expect(!import_caller_ok(nullptr, nullptr), "B: a null owner never matches a null caller");
    expect(!import_caller_ok(&a, nullptr), "B: no stored owner: refused");
    expect(!import_caller_ok(nullptr, &a), "B: no current task: refused");
    // source pins of the real dispatch
    const std::string K = root + "/src/navi48-bringup/";
    const std::string cli = slurp(K + "src/Navi48NativeClient.cpp"), clih = slurp(K + "src/Navi48NativeClient.hpp");
    const std::string ext = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::externalMethod(");
    expect(!ext.empty(), "externalMethod is found");
    const size_t pOpen = ext.find("if (!args || !opened) return kIOReturnNotReady;"), pGate = ext.find("if (!n48native::policy::selector_allowed(adminClient, selector)) {"), pSw = ext.find("switch (selector) {");
    expect(pOpen != std::string::npos && pGate != std::string::npos && pSw != std::string::npos && pOpen < pGate && pGate < pSw, "E: the selector gate sits after the opened check and BEFORE the switch (reachable for every selector)");
    if (pGate != std::string::npos && pSw != std::string::npos) {
        const size_t pAppGate = ext.find("// 0.0.640 (G4): an APP client reaches only");   // 0.0.640: the APP gate follows the uid-88 gate; this block is the uid-88 gate alone
        const std::string gate = ext.substr(pGate, (pAppGate != std::string::npos && pAppGate > pGate ? pAppGate : pSw) - pGate);
        expect(gate.find("return kIOReturnNotPrivileged;") != std::string::npos && gate.find("NCLOG(\"selector %u refused: not permitted for a uid-88 (non-administrator) client\", selector);") != std::string::npos, "E: the refusal is NotPrivileged with a log line");
        expect(gate.find("return kIOReturnNotPrivileged;") > gate.find("NCLOG("), "E: the log line comes before the return");
    }
    expect_u("the switch has no second gate: adminClient is read in exactly one place", count_of(cli, "selector_allowed(adminClient"), 1);
    expect(cli.find("adminClient = d.reason == n48native::policy::kReasonAdmin;") != std::string::npos && count_of(cli, "adminClient = ") == 1, "E: adminClient is true only for the administrator reason, set in one place");
    { const size_t pDec = cli.find("open_decision(admin, uid"), pAdm = cli.find("adminClient = "), pPriv = cli.find("privileged = true;"); expect(pDec < pPriv && pPriv < pAdm, "E: adminClient is set after the open decision admitted the caller"); }
    expect(clih.find("bool           adminClient { false };") != std::string::npos, "E: adminClient defaults to false (least privilege)");
    const size_t c21 = cli.find("case N48N_SEL_BO_IMPORT_HOST:");
    expect(c21 != std::string::npos, "the selector 21 case is found");
    if (c21 != std::string::npos) {
        const std::string c = cli.substr(c21, cli.find("default:", c21) - c21);
        const size_t pShape = c.find("if (!shape(4, 4, 0, 0)) return kIOReturnBadArgument;"), pOwn = c.find("if (!n48native::policy::import_caller_ok(current_task(), task)) {"), pCall = c.find("return amdgpu::n1c_bo_import_host(r, task,");
        expect(pShape != std::string::npos && pOwn != std::string::npos && pCall != std::string::npos && pShape < pOwn && pOwn < pCall, "B: shape, then the owning-task check, THEN the import");
        if (pOwn != std::string::npos && pCall != std::string::npos) {
            const std::string blk = c.substr(pOwn, pCall - pOwn);
            expect(blk.find("return kIOReturnNotPermitted;") != std::string::npos && blk.find("OSCompareAndSwap(0, 1, &gImportCallerLogged)") != std::string::npos && blk.find("NCLOG(\"import refused: the caller is not the task that opened this client (logged once)\")") != std::string::npos, "B: NotPermitted, logged once");
        }
    }
    expect(cli.find("#include <kern/task.h>") != std::string::npos, "current_task comes from <kern/task.h>");
    expect_u("the stored task is used in exactly one selector: 21 (the lifetime report)", count_of(cli, "(r, task,") + count_of(cli, ", task)"), 2u);
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    o1_policy(); o2_latch(); o3_pins(root); o4_ready(); o4_pins(root); o5_reach(root);
    std::printf("native_ws_open_test: %d checks, %d failed\n", gRun, gFail);
    return gFail == 0 ? 0 : 1;
}
