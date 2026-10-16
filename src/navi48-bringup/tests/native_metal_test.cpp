// native_metal_test.cpp - build 0.0.610, extended 0.0.611 (milestone #9, route A, an internal design note): the pure half of the Metal nub and its ops table, plus source pins.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//       -I src/navi48-bringup/src -I src/navi48-bringup/src/amd src/navi48-bringup/tests/native_metal_test.cpp -o /tmp/native_metal && /tmp/native_metal .
//   (run from the repo root; the argument is the repo root the source pins read from; an optional second argument is the tree that holds the aux kext
//    tools/native/navi48accel (default: the same root) - the two Navi48MetalOps.h copies must be byte-identical; tests/native_metal_plant.sh plants breaks)
// Covers:
//   M1  publish_verdict: every gate alone (Hello, flags, boot-arg, S1b gate / ran / positive / stopped, HUNG, state) refuses with its code, the order, the one success;
//       withdraw_verdict: cleanup never needs the boot-arg or S1b; 0.0.670: publish_at_boot() (the master latch AND the kill switch) and publish_at_boot_verdict
//       (publish_verdict with the session term removed, and NOTHING else: the two agree on every session-bearing gate combination);
//   M2  the nub state machine over whole sequences: publish, again (Exclusive), withdraw, publish while Terminating (Busy), free -> Off, publish again, withdraw with no nub;
//   M3  IOAccelConfig: the 12 stores equal the DISASSEMBLED bytes of AppleParavirtAccelerator::populateAccelConfig (a mini x86 decoder over the real 114 bytes) except the
//       two intended differences (name pointer, +0x47 = 0); the family defaults (+0x4c/+0x50/+0x54 ...) survive untouched (never memset); the static check refuses each zero limit
//       and a zero family default; +0x47 clear;
//   M4  stamp page, task window, factory mask (default 0), the ops table's layout (size 120, offsets) and its byte-identity with the aux kext's copy;
//   M6  (0.0.611) config_populate never leaves our values in the caller's struct on a refusal; vhook logging is rate limited per (cls, slot); vhook output-parameter
//       writes (VidMemory 43/61, Resource 60/61/62) only with their factory bit, pointer-checked before any write;
//   M5  source pins: the TWO publishers (the native client's selector and, since 0.0.670, the kext's own boot path) share ONE implementation, the verdict precedes the
//       allocation and the registration, the selectors' shapes, the hooks are wired to the pure code, no register access in the nub, version 0.0.670, the ABI text,
//       banned strings;
//   M7  (0.0.670) source pins: the boot worker publishes the nub through Navi48MetalNub::bootPublish and nothing else does; the boot hold is adjustBusy on
//       the BRING-UP service, taken once, released by exactly one of four CAS-guarded paths, capped by a thread_call, and the arm is refused when the cap got there
//       first (never arm behind WindowServer); every one of those decisions is the pure code above.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "amd/native_metal_pure.h"
#include "Navi48NativeABI.h"

using namespace n48metal;

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

// ---- M1 ------------------------------------------------------------------------------------------------------------------------------------------------
static GateIn good() { GateIn g {}; g.flags = 0; g.hello = true; g.bootarg = true; g.s1bGateOn = true; g.s1bRan = true; g.s1bPositive = true; g.s1bStopped = false; g.hung = false; return g; }
static void m1_verdicts() {
    expect_u("all gates open, no nub: OK", publish_verdict(good(), kOff), kOk);
    { GateIn g = good(); g.hello = false;      expect_u("no Hello: NotReady", publish_verdict(g, kOff), kNotReady); }
    { GateIn g = good(); g.flags = 1;          expect_u("flags != 0: BadArgument", publish_verdict(g, kOff), kBadArg); }
    { GateIn g = good(); g.bootarg = false;    expect_u("boot-arg navi48-metal absent: Unsupported (the feature does not exist)", publish_verdict(g, kOff), kUnsupported); }
    { GateIn g = good(); g.s1bGateOn = false;  expect_u("S1b gate not on: NotReady", publish_verdict(g, kOff), kNotReady); }
    { GateIn g = good(); g.s1bRan = false;     expect_u("S1b did not run: NotReady", publish_verdict(g, kOff), kNotReady); }
    { GateIn g = good(); g.s1bPositive = false; expect_u("S1b not POSITIVE PASS: NotReady", publish_verdict(g, kOff), kNotReady); }
    { GateIn g = good(); g.s1bStopped = true;  expect_u("S1b latched a stop: NotReady", publish_verdict(g, kOff), kNotReady); }
    { GateIn g = good(); g.hung = true;        expect_u("GPU declared HUNG: NotReady", publish_verdict(g, kOff), kNotReady); }
    expect_u("already published: ExclusiveAccess", publish_verdict(good(), kPublished), kExclusive);
    expect_u("still terminating: Busy", publish_verdict(good(), kTerminating), kBusy);
    // order: the session and the flags word before the boot-arg before S1b before HUNG before the state
    { GateIn g = good(); g.hello = false; g.flags = 5; g.bootarg = false; expect_u("order: Hello first", publish_verdict(g, kPublished), kNotReady); }
    { GateIn g = good(); g.flags = 5; g.bootarg = false; expect_u("order: flags before boot-arg", publish_verdict(g, kOff), kBadArg); }
    { GateIn g = good(); g.bootarg = false; g.s1bPositive = false; expect_u("order: boot-arg before S1b", publish_verdict(g, kOff), kUnsupported); }
    { GateIn g = good(); g.hung = true; expect_u("order: HUNG before the state", publish_verdict(g, kPublished), kNotReady); }
    // no single satisfied gate is enough: every combination of the 7 booleans except all-open refuses
    { int opened = 0; for (unsigned m = 0; m < 128; ++m) { GateIn g = good(); g.hello = m & 1; g.bootarg = m & 2; g.s1bGateOn = m & 4; g.s1bRan = m & 8; g.s1bPositive = m & 16; g.s1bStopped = !(m & 32); g.hung = !(m & 64);
        if (publish_verdict(g, kOff) == kOk) { opened++; expect(m == 127, "only the all-open combination publishes"); } }
      expect_u("exactly one of 128 gate combinations publishes", opened, 1); }
    // withdraw: cleanup never depends on the boot-arg or S1b
    { GateIn g = good(); g.bootarg = false; g.s1bGateOn = false; g.s1bRan = false; g.s1bPositive = false; g.s1bStopped = true; g.hung = true;
      expect_u("withdraw works with the boot-arg gone, S1b failed and the GPU HUNG (it only cleans up)", withdraw_verdict(g, kPublished), kOk); }
    { GateIn g = good(); g.hello = false; expect_u("withdraw needs the session: NotReady", withdraw_verdict(g, kPublished), kNotReady); }
    { GateIn g = good(); g.flags = 3;     expect_u("withdraw flags != 0: BadArgument", withdraw_verdict(g, kPublished), kBadArg); }
    expect_u("withdraw with no nub: NotFound", withdraw_verdict(good(), kOff), kNotFound);
    expect_u("withdraw while terminating: Busy", withdraw_verdict(good(), kTerminating), kBusy);
    // ---- 0.0.670: the BOOT publish path. The latch is pure, and every direction of it is checked here. ----
    expect(publish_at_boot(true, false), "0.0.670: navi48-armboot=1 and no kill switch: the KEXT publishes the nub at boot");
    expect(!publish_at_boot(false, false), "0.0.670: without navi48-armboot the kext publishes nothing (the default is OFF)");
    expect(!publish_at_boot(true, true), "0.0.670: navi48-nubboot=0 is the kill switch: publish on demand only");
    expect(!publish_at_boot(false, true), "0.0.670: master off and kill switch on: nothing at boot");
    // The boot verdict is publish_verdict with the SESSION term removed and nothing else. Three facts, all over the 128 gate combinations:
    //   (1) the ONLY difference it makes is the session: the selector refuses every session-less combination, the boot verdict refuses all of them but
    //       the all-open one - and that one is exactly what the boot path needs (at boot no session can exist);
    //   (2) it publishes for exactly TWO combinations: the all-open one with a session (where the two agree) and the all-open one without it;
    //   (3) wherever a session exists the two verdicts are identical, so the boot path can never be a WEAKER gate than the selector.
    {
        unsigned opened = 0, withSession = 0, selectorOpened = 0, bootOnlyOpened = 0;
        for (unsigned m = 0; m < 128; ++m) {
            GateIn g = good();
            g.hello = m & 1; g.bootarg = m & 2; g.s1bGateOn = m & 4; g.s1bRan = m & 8; g.s1bPositive = m & 16;
            g.s1bStopped = !(m & 32); g.hung = !(m & 64);
            const uint32_t sel = publish_verdict(g, kOff), boot = publish_at_boot_verdict(g, kOff);
            if (boot == kOk) { opened++; if (g.hello) selectorOpened++; else bootOnlyOpened++; }
            if (g.hello) { withSession++; expect_u("with a session the boot verdict equals the selector's", boot, sel); }
        }
        expect_u("the boot verdict publishes for exactly two combinations", opened, 2);
        expect_u("... one of them is the all-open combination the selector also publishes", selectorOpened, 1);
        expect_u("... the other is that SAME combination with no session: the one the boot path exists for", bootOnlyOpened, 1);
        expect_u("... and it agrees with the selector on all 64 session-bearing combinations", withSession, 64);
        { GateIn s = good(); s.hello = false;                      // the all-open combination as it looks at boot: no session
          expect_u("the all-open, session-less combination: the selector REFUSES it", publish_verdict(s, kOff), kNotReady);
          expect_u("... and the boot path publishes it (this is the whole point of the boot verdict)", publish_at_boot_verdict(s, kOff), kOk); }
    }
    {   // the gate is the SAME check in the SAME order: every single term still refuses the boot path with its own code
        GateIn g = good(); g.hello = false;
        { GateIn b = g; b.flags = 1;          expect_u("boot: flags != 0 -> BadArgument", publish_at_boot_verdict(b, kOff), kBadArg); }
        { GateIn b = g; b.bootarg = false;    expect_u("boot: no navi48-metal -> Unsupported", publish_at_boot_verdict(b, kOff), kUnsupported); }
        { GateIn b = g; b.s1bGateOn = false;  expect_u("boot: S1b gate off -> NotReady", publish_at_boot_verdict(b, kOff), kNotReady); }
        { GateIn b = g; b.s1bRan = false;     expect_u("boot: S1b did not run -> NotReady", publish_at_boot_verdict(b, kOff), kNotReady); }
        { GateIn b = g; b.s1bPositive = false; expect_u("boot: S1b not POSITIVE -> NotReady", publish_at_boot_verdict(b, kOff), kNotReady); }
        { GateIn b = g; b.s1bStopped = true;  expect_u("boot: S1b latched a stop -> NotReady", publish_at_boot_verdict(b, kOff), kNotReady); }
        { GateIn b = g; b.hung = true;        expect_u("boot: GPU HUNG -> NotReady", publish_at_boot_verdict(b, kOff), kNotReady); }
        { GateIn b = g; b.dispNub = true;     expect_u("boot: a display nub is published -> ExclusiveAccess (the 0.0.652 interlock)", publish_at_boot_verdict(b, kOff), kExclusive); }
        { GateIn b = g; b.dispNub = true; b.m6 = true; expect_u("boot: ... unless navi48-m6 lifts it", publish_at_boot_verdict(b, kOff), kOk); }
        expect_u("boot: already published -> ExclusiveAccess", publish_at_boot_verdict(g, kPublished), kExclusive);
        expect_u("boot: still terminating -> Busy", publish_at_boot_verdict(g, kTerminating), kBusy);
    }
    // ---- 0.0.670: the BOOT HOLD's transition table. Each transition is allowed from exactly ONE state, and that is what makes "exactly one adjustBusy(-1)" and
    //      "never arm with WindowServer up" true rather than careful. ----
    expect(hold_take_ok(kHoldNone) && !hold_take_ok(kHoldHeld) && !hold_take_ok(kHoldClaimed), "the hold can only be TAKEN from none (one per boot)");
    expect(hold_claim_ok(kHoldHeld) && !hold_claim_ok(kHoldNone) && !hold_claim_ok(kHoldClaimed), "the arm can only be claimed from held: a hold the cap released refuses the arm");
    expect(hold_cap_ok(kHoldHeld) && !hold_cap_ok(kHoldNone) && !hold_cap_ok(kHoldClaimed), "the cap can only release a HELD hold (never one the worker already claimed)");
    expect(hold_release_ok(kHoldClaimed) && !hold_release_ok(kHoldHeld) && !hold_release_ok(kHoldNone), "the worker's release is valid only after its own claim");
    expect_u("take: none -> held", (uint64_t)hold_after_take(), (uint64_t)kHoldHeld);
    expect_u("claim: held -> claimed", (uint64_t)hold_after_claim(), (uint64_t)kHoldClaimed);
    expect_u("cap: held -> none", (uint64_t)hold_after_cap(), (uint64_t)kHoldNone);
    expect_u("release: claimed -> none", (uint64_t)hold_after_release(), (uint64_t)kHoldNone);
    {
        const HoldState all[3] = { kHoldNone, kHoldHeld, kHoldClaimed };
        for (int i = 0; i < 3; ++i) {
            // `held` is the ONE state where both the cap and the worker's claim are possible - that race is what the CAS arbitrates, and only one of the
            // two can win it (the state then refuses the other). Every OTHER state must allow neither.
            if (all[i] != kHoldHeld) expect(!hold_cap_ok(all[i]) && !hold_claim_ok(all[i]), "only a HELD hold can be ended by the cap or by the claim");
            expect(!(hold_take_ok(all[i]) && hold_cap_ok(all[i])), "no state can be taken and capped at once");
            expect(!(hold_take_ok(all[i]) && hold_claim_ok(all[i])), "no state can be taken and claimed at once");
        }
        expect(hold_cap_ok(kHoldHeld) && hold_claim_ok(kHoldHeld), "from held the cap and the claim are BOTH possible: the CAS decides, and the loser is refused");
        // the two ways out of `held` are mutually exclusive, and the one way out of `claimed` is the worker's: 2 releases max, 1 owner
        expect(hold_cap_ok(kHoldHeld) && hold_claim_ok(kHoldHeld) && !hold_release_ok(kHoldHeld), "from held: cap OR claim, never a release");
        expect(hold_release_ok(kHoldClaimed) && !hold_cap_ok(kHoldClaimed) && !hold_claim_ok(kHoldClaimed), "from claimed: the worker's release only");
    }
    // the cap's clamp: a typo must neither disable the guard (a cap that never expires is a boot that never finishes) nor fire before the bring-up could have finished
    expect_u("the cap defaults to 40 s (we add no settle of our own)", hold_cap_ms(false, 0), kHoldCapMsDefault);
    expect_u("a present but zero arg means the default too", hold_cap_ms(true, 0), kHoldCapMsDefault);
    expect_u("below the minimum is clamped UP", hold_cap_ms(true, 1), kHoldCapMsMin);
    expect_u("above the maximum is clamped DOWN", hold_cap_ms(true, 999999u), kHoldCapMsMax);
    expect_u("inside the range the value is taken as given", hold_cap_ms(true, 25000u), 25000u);
    expect_u("the minimum is below the maximum (a clamp that inverts is a clamp that does nothing)", (uint64_t)(kHoldCapMsMin < kHoldCapMsMax), 1);
}

// ---- M2 ------------------------------------------------------------------------------------------------------------------------------------------------
static void m2_sm() {
    State s = kOff;
    expect_u("publish 1", publish_verdict(good(), s), kOk);                     s = sm_after_publish(s);   expect_u("state Published", s, kPublished);
    expect_u("publish 2 refused", publish_verdict(good(), s), kExclusive);      s = sm_after_publish(s);   expect_u("a refused publish leaves Published", s, kPublished);
    expect_u("withdraw ok", withdraw_verdict(good(), s), kOk);                  s = sm_after_withdraw(s);  expect_u("state Terminating", s, kTerminating);
    expect_u("publish while terminating refused", publish_verdict(good(), s), kBusy);
    expect_u("withdraw while terminating refused", withdraw_verdict(good(), s), kBusy);
    s = sm_after_withdraw(s);                                                   expect_u("withdraw does not move Terminating", s, kTerminating);
    s = sm_after_free(s);                                                       expect_u("the nub's free() returns to Off", s, kOff);
    expect_u("publish after free ok", publish_verdict(good(), s), kOk);         s = sm_after_publish(s);   expect_u("Published again", s, kPublished);
    expect_u("withdraw with nothing to withdraw", withdraw_verdict(good(), sm_after_free(kOff)), kNotFound);
    expect_u("publish never moves Terminating", sm_after_publish(kTerminating), kTerminating);
    expect_u("withdraw never moves Off", sm_after_withdraw(kOff), kOff);
}

// ---- M3: the config, against the DISASSEMBLED paravirt function ---------------------------------------------------------------------------------------------
// AppleParavirtAccelerator::populateAccelConfig, 0x142b78f4 .. 0x142b7967 of the AppleParavirtGPU kext extracted from the System KC (macOS 26.6.2 25G83), 117 bytes.
static const uint8_t kParavirtBytes[117] = {
    0x55,0x48,0x89,0xe5,0x48,0x8d,0x05,0xd2,0x33,0x01,0x00,0x48,0x89,0x06,0xc7,0x46,0x08,0x00,0x00,0x48,0x00,0xc7,0x46,0x64,0x02,0x00,0x00,0x00,0x48,0xc7,0x46,0x0c,0x07,0x00,0x00,0x00,
    0x48,0xb8,0x00,0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x48,0x89,0x46,0x14,0x48,0xc7,0x46,0x20,0x00,0x00,0x00,0x40,0x48,0xb8,0x08,0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x48,0x89,0x46,0x28,
    0xc7,0x46,0x30,0x00,0x40,0x00,0x40,0xc6,0x46,0x47,0x01,0x48,0xb8,0x0a,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x48,0x89,0x46,0x68,0x48,0xc7,0x86,0x88,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0xc7,0x46,0x5c,0x04,0x00,0x00,0x00,0x5d,0xc3 };
struct Dec { uint32_t off; uint8_t width; uint64_t value; bool isName; };
static const uint64_t kNameMark = 0x1122334455667788ull;
static std::vector<Dec> decode_paravirt(bool &ok) {
    std::vector<Dec> out; ok = true;
    const uint8_t *p = kParavirtBytes; size_t i = 0; const size_t n = sizeof(kParavirtBytes);
    uint64_t rax = 0; bool raxIsName = false;
    auto u32 = [&](size_t k) { uint32_t v; std::memcpy(&v, p + k, 4); return v; };
    while (i < n) {
        if (p[i] == 0x55 || p[i] == 0x5d || p[i] == 0xc3) { i += 1; continue; }                                         // push/pop rbp, ret
        if (i + 3 < n && p[i] == 0x48 && p[i+1] == 0x89 && p[i+2] == 0xe5) { i += 3; continue; }                        // mov rbp, rsp
        if (p[i] == 0x48 && p[i+1] == 0x8d && p[i+2] == 0x05) { rax = kNameMark; raxIsName = true; i += 7; continue; }  // lea rax, [rip+x]
        if (p[i] == 0x48 && p[i+1] == 0xb8) { std::memcpy(&rax, p + i + 2, 8); raxIsName = false; i += 10; continue; }   // movabs rax, imm64
        if (p[i] == 0x48 && p[i+1] == 0x89 && p[i+2] == 0x06) { out.push_back({ 0, 8, rax, raxIsName }); i += 3; continue; }              // mov [rsi], rax
        if (p[i] == 0x48 && p[i+1] == 0x89 && p[i+2] == 0x46) { out.push_back({ p[i+3], 8, rax, raxIsName }); i += 4; continue; }        // mov [rsi+d8], rax
        if (p[i] == 0xc7 && p[i+1] == 0x46) { out.push_back({ p[i+2], 4, u32(i + 3), false }); i += 7; continue; }                          // mov dword [rsi+d8], imm32
        if (p[i] == 0x48 && p[i+1] == 0xc7 && p[i+2] == 0x46) { out.push_back({ p[i+3], 8, (uint64_t)(int64_t)(int32_t)u32(i + 4), false }); i += 8; continue; }   // mov qword [rsi+d8], simm32
        if (p[i] == 0xc6 && p[i+1] == 0x46) { out.push_back({ p[i+2], 1, p[i+3], false }); i += 4; continue; }                              // mov byte [rsi+d8], imm8
        if (p[i] == 0x48 && p[i+1] == 0xc7 && p[i+2] == 0x86) { out.push_back({ u32(i + 3), 8, (uint64_t)(int64_t)(int32_t)u32(i + 7), false }); i += 11; continue; }  // mov qword [rsi+d32], simm32
        ok = false; break;
    }
    return out;
}
static void m3_config() {
    bool ok = false;
    const std::vector<Dec> d = decode_paravirt(ok);
    expect(ok, "the paravirt bytes decode completely with the mini decoder");
    expect_u("paravirt populateAccelConfig has 12 stores (the design review said 13)", d.size(), 12);
    expect_u("our table has 12 stores", sizeof(kCfgStores) / sizeof(kCfgStores[0]), 12);
    // every paravirt store is in our table with the same offset, width and value; the two intended differences are the name pointer and +0x47
    for (const Dec &x : d) {
        const CfgStore *m = nullptr;
        for (const CfgStore &s : kCfgStores) if (s.off == x.off) m = &s;
        char w[96]; std::snprintf(w, sizeof(w), "paravirt store +%#x has a match in kCfgStores", x.off);
        expect(m != nullptr, w);
        if (!m) continue;
        if (x.isName) { std::snprintf(w, sizeof(w), "+%#x is the name pointer in both", x.off); expect(m->width == 0 && x.width == 8, w); continue; }
        std::snprintf(w, sizeof(w), "+%#x width", x.off); expect_u(w, m->width, x.width);
        if (x.off == kOffPipeUC) { expect_u("paravirt stores 1 at +0x47 (type-4 display-pipe UC enable)", x.value, 1); expect_u("ours stores 0 at +0x47 (headless)", m->value, 0); continue; }
        std::snprintf(w, sizeof(w), "+%#x value", x.off); expect_u(w, m->value, x.value);
    }
    // the family defaults IOGraphicsAccelerator2::initializeConfigStructure writes (0x145bc2be..): what the fill must leave alone
    uint8_t cfg[kCfgBytes]; std::memset(cfg, 0xA5, sizeof(cfg));                    // poison: a byte the stores do not touch must come out as 0xA5
    st_le(cfg, kOffLim4c, 4, 3); st_le(cfg, kOffLim50, 4, 3); st_le(cfg, kOffLim54, 4, 0x7fff);   // +0x4c, +0x50, +0x54 as the family sets them
    uint8_t before[kCfgBytes]; std::memcpy(before, cfg, sizeof(cfg));
    expect_u("populate on the family defaults succeeds", (uint64_t)config_populate(cfg, kCfgBytes, 0xFFFFFF8012345678ull), 0);
    uint8_t want[kCfgBytes]; std::memcpy(want, before, sizeof(want));
    for (const CfgStore &s : kCfgStores) { if (s.width == 0) st_le(want, s.off, 8, 0xFFFFFF8012345678ull); else st_le(want, s.off, s.width, s.value); }
    expect(std::memcmp(cfg, want, sizeof(cfg)) == 0, "the config is exactly the family defaults with the 12 stores applied (nothing else touched)");
    expect_u("the family default +0x4c survives", ld_le(cfg + kOffLim4c, 4), 3);
    expect_u("the family default +0x54 survives", ld_le(cfg + kOffLim54, 4), 0x7fff);
    expect_u("an untouched byte keeps its value (no memset)", cfg[0x1f], 0xA5);
    expect_u("+0x30 = 0x4000", ld_le(cfg + kOffLim30, 2), 0x4000); expect_u("+0x32 = 0x4000", ld_le(cfg + kOffLim32, 2), 0x4000);
    expect_u("+0x47 = 0 (headless)", cfg[kOffPipeUC], 0);
    expect_u("the name pointer is stored", ld_le(cfg, 8), 0xFFFFFF8012345678ull);
    expect_u("the check passes", config_check(cfg), kCfgOk);
    // the static check: each limit
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); st_le(c, kOffLim4c, 4, 0); expect_u("limit +0x4c zero refused", config_check(c), kCfgLimit4c); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); st_le(c, kOffLim50, 4, 0); expect_u("limit +0x50 zero refused", config_check(c), kCfgLimit50); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); st_le(c, kOffLim54, 4, 0); expect_u("limit +0x54 zero refused", config_check(c), kCfgLimit54); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); st_le(c, kOffLim30, 2, 0); expect_u("limit +0x30 zero refused", config_check(c), kCfgLimit30); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); st_le(c, kOffLim32, 2, 0); expect_u("limit +0x32 zero refused", config_check(c), kCfgLimit32); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); st_le(c, kOffName, 8, 0); expect_u("no name refused", config_check(c), kCfgNoName); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, cfg, sizeof(c)); c[kOffPipeUC] = 1; expect_u("+0x47 set refused", config_check(c), kCfgPipeUC); }
    // a zero family default (initializeConfigStructure did not run / changed) makes populate REFUSE, so the aux kext blanks the name and the family's start fails closed
    { uint8_t c[kCfgBytes]; std::memset(c, 0, sizeof(c)); expect(config_populate(c, kCfgBytes, 1) != 0, "populate on an all-zero structure (no family defaults) is refused"); }
    { uint8_t c[kCfgBytes]; std::memcpy(c, before, sizeof(c)); st_le(c, kOffLim54, 4, 0); expect(config_populate(c, kCfgBytes, 1) != 0, "populate with a zero +0x54 family default is refused"); }
    expect(config_populate(cfg, kCfgBytes - 8, 1) != 0, "populate with the wrong byte count is refused");
    expect(config_populate(nullptr, kCfgBytes, 1) != 0, "populate with no buffer is refused");
    expect(config_populate(cfg, kCfgBytes, 0) != 0, "populate with no name is refused");
    // a poisoned family default at +0x18 (the upper half of the 0x2000000000 store) must be overwritten by the store, not preserved
    expect_u("+0x14 qword store covers +0x18", ld_le(cfg + kOffF2, 8), 0x2000000000ull);
}

// ---- M4 ------------------------------------------------------------------------------------------------------------------------------------------------
static void m4_misc() {
    expect(stamp_page_ok(0x12345000, 0x1000, 0x1000), "an aligned 4 KiB page below 2^52 is a stamp page");
    expect(!stamp_page_ok(0, 0x1000, 0x1000), "phys 0 refused");
    expect(!stamp_page_ok(0x12345800, 0x1000, 0x1000), "unaligned refused");
    expect(!stamp_page_ok(0x12345000, 0x800, 0x1000), "a short segment refused");
    expect(!stamp_page_ok(0x12345000, 0x1000, 0x800), "a short descriptor refused");
    expect(!stamp_page_ok(0x0010000000000000ull, 0x1000, 0x1000), "above the 2^52 mask refused");
    uint64_t sz = 0, rs = 0;
    expect_u("task window kind 0 ok", (uint64_t)task_window(0, &sz, &rs), 0); expect_u("size 16 GiB", sz, 0x400000000ull); expect_u("page 0 reserved", rs, 0x1000);
    expect_u("task window kind 1 ok", (uint64_t)task_window(1, &sz, &rs), 0);
    expect(task_window(2, &sz, &rs) != 0, "an unknown task kind refused"); expect(task_window(0, nullptr, &rs) != 0, "no size pointer refused");
    expect_u("factory mask default (arg absent) is 0", factory_mask(false, 3), 0);
    expect_u("factory mask 0 with the arg 0", factory_mask(true, 0), 0);
    expect_u("factory mask from the arg", factory_mask(true, 3), 3);
    expect_u("factory mask ignores unknown bits", factory_mask(true, 0xff), 0x7f);
    expect_u("ops flags: software only", kOpsFlags, N48_METAL_F_SOFTWARE_ONLY); expect_u("caps: the generic hook and (0.0.656) native_open", kOpsCaps, N48_CAP_VHOOK | N48_CAP_NATIVE_OPEN);
    // the ops struct layout (both kexts are built against it)
    expect_u("sizeof(N48MetalOps): ABI 3", sizeof(N48MetalOps), 152);
    expect_u("offsetof size", offsetof(N48MetalOps, size), 8);
    expect_u("offsetof device_open", offsetof(N48MetalOps, device_open), 24);
    expect_u("offsetof populate_config", offsetof(N48MetalOps, populate_config), 40);
    expect_u("offsetof task_window", offsetof(N48MetalOps, task_window), 64);
    expect_u("offsetof factory_mask", offsetof(N48MetalOps, factory_mask), 72);
    expect_u("offsetof trace", offsetof(N48MetalOps, trace), 88);
    expect_u("offsetof vhook", offsetof(N48MetalOps, vhook), 96); expect_u("offsetof caps", offsetof(N48MetalOps, caps), 104); expect_u("offsetof reserved1", offsetof(N48MetalOps, reserved1), 112);
    expect_u("offsetof disp_flags", offsetof(N48MetalOps, disp_flags), 120); expect_u("offsetof disp_hook", offsetof(N48MetalOps, disp_hook), 128); expect_u("offsetof pci_device", offsetof(N48MetalOps, pci_device), 136);
    expect_u("magic", N48_METAL_OPS_MAGIC, 0x4F38344Eu); expect_u("abi", N48_METAL_ABI, 3); expect_u("the oldest accepted abi", N48_METAL_ABI_MIN, 1); expect_u("ABI 1 was 120 bytes", N48_METAL_OPS_MIN, 120); expect_u("ABI 2 is 144", N48_METAL_OPS_V2, 144); expect_u("ABI 3 is 152", N48_METAL_OPS_V3, 152);
}

// ---- M5: source pins -------------------------------------------------------------------------------------------------------------------------------------
// ---- M6 (0.0.611) --------------------------------------------------------------------------------------------------------------------------------------
static void m6_config_atomic_and_vhook() {
    // (1) a refusal leaves the caller's structure BYTE-FOR-BYTE as it was; a success copies the filled copy
    {
        uint8_t c[kCfgBytes]; std::memset(c, 0xA5, sizeof(c));
        uint8_t was[kCfgBytes];
        st_le(c, kOffLim4c, 4, 3); st_le(c, kOffLim50, 4, 3); st_le(c, kOffLim54, 4, 0);      // a zero +0x54 family default: the check refuses
        std::memcpy(was, c, sizeof(c));
        expect(config_populate(c, kCfgBytes, 0xFFFFFF8012345678ull) != 0, "populate refuses a zero +0x54 family default");
        expect(std::memcmp(c, was, sizeof(c)) == 0, "a refused populate leaves the struct byte-for-byte as it was (none of our 12 stores landed)");
        expect_u("... the name pointer was not stored", ld_le(c, 8), 0xA5A5A5A5A5A5A5A5ull);
        expect_u("... +0x47 was not stored", c[kOffPipeUC], 0xA5);
        uint8_t z[kCfgBytes]; std::memset(z, 0, sizeof(z)); std::memset(was, 0, sizeof(was));
        expect(config_populate(z, kCfgBytes, 1) != 0 && std::memcmp(z, was, sizeof(z)) == 0, "an all-zero (no family defaults) struct is refused and stays all zero");
        uint8_t ok[kCfgBytes]; std::memset(ok, 0xA5, sizeof(ok)); st_le(ok, kOffLim4c, 4, 3); st_le(ok, kOffLim50, 4, 3); st_le(ok, kOffLim54, 4, 0x7fff);
        expect_u("a good populate succeeds", (uint64_t)config_populate(ok, kCfgBytes, 0xFFFFFF8012345678ull), 0);
        expect_u("... and the stores are in the caller's struct", ld_le(ok, 8), 0xFFFFFF8012345678ull);
        uint8_t bad[kCfgBytes]; std::memset(bad, 0xA5, sizeof(bad)); st_le(bad, kOffLim4c, 4, 3); st_le(bad, kOffLim50, 4, 3); st_le(bad, kOffLim54, 4, 0x7fff);
        bad[kOffPipeUC] = 0; uint8_t bcopy[kCfgBytes]; std::memcpy(bcopy, bad, sizeof(bad));
        expect(config_populate(bad, 0x80u, 1) != 0 && std::memcmp(bad, bcopy, sizeof(bad)) == 0, "a wrong byte count is refused without touching the struct");
    }
    // (2) the log rate limit: full lines for the first kVhookLogFirst calls of a (cls, slot), then one count line every kVhookLogEvery
    {
        static VhookCounts t; std::memset(&t, 0, sizeof(t));
        uint32_t full = 0, cnt = 0, none = 0;
        for (uint32_t i = 0; i < 20000; ++i) { const uint32_t n = vhook_count(t, N48_VC_EVENTMACHINE, 68); expect_u("call numbers are 1-based and consecutive", n, i + 1);
            const uint32_t k = vhook_log_kind(n); if (k == kVhLogFull) full++; else if (k == kVhLogCount) cnt++; else none++; if (i > 30 && gFail) break; }
        expect_u("20000 calls of one slot log kVhookLogFirst full lines", full, kVhookLogFirst);
        expect_u("... and 4 count lines (every 4096)", cnt, 4);
        expect_u("... and are silent otherwise", none, 20000 - kVhookLogFirst - 4);
        expect_u("a different slot has its own counter", vhook_count(t, N48_VC_EVENTMACHINE, 72), 1);
        expect_u("a different class has its own counter", vhook_count(t, N48_VC_RESOURCE, 68), 1);
        // the table never fails: more distinct keys than slots share the overflow bucket, which is still rate limited
        static VhookCounts o; std::memset(&o, 0, sizeof(o));
        uint32_t last = 0; for (uint32_t s = 0; s < 200; ++s) last = vhook_count(o, 5, 1000 + s);
        expect(last >= 1, "the table overflows into a shared bucket without failing");
        uint32_t loud = 0; for (uint32_t i = 0; i < 5000; ++i) if (vhook_log_kind(vhook_count(o, 5, 5000 + (i % 100))) != kVhLogNone) loud++;
        expect(loud < 20, "an overflowing table is still rate limited (few log lines for 5000 calls)");
        expect_u("log kind: call 4 is full", vhook_log_kind(4), kVhLogFull); expect_u("log kind: call 5 is silent", vhook_log_kind(5), kVhLogNone);
        expect_u("log kind: call 4096 is a count line", vhook_log_kind(4096), kVhLogCount);
    }
    // (3) the output-parameter writes
    {
        alignas(8) uint64_t q0 = 0x1111, q1 = 0x2222; alignas(8) uint32_t i0 = 0x3333; uint64_t ret = 0xdead;
        const uint64_t P = 0;                                    // kmin 0: host pointers are user-space; the kernel-half check is tested separately
        const uint32_t vm = N48_FACT_VIDMEMORY, rs = N48_FACT_RESOURCE;
        { uint64_t a[6] = { 0x1000, (uint64_t)(uintptr_t)&q0, 0, 0, 0, 0 };
          expect_u("VidMemory 43 getPhysicalSegment: handled", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 43, a, 2, &ret, P), 1);
          expect_u("... *len = 0", q0, 0); expect_u("... returns 0", ret, 0); q0 = 0x1111; ret = 0xdead;
          expect_u("VidMemory 43 with the factory bit OFF: not handled", (uint64_t)vhook_handle(0, N48_VC_VIDMEMORY, 43, a, 2, &ret, P), 0);
          expect_u("... nothing written", q0, 0x1111); expect_u("... ret untouched", ret, 0xdead);
          expect_u("VidMemory 43 with only the Resource bit: not handled", (uint64_t)vhook_handle(rs, N48_VC_VIDMEMORY, 43, a, 2, &ret, P), 0); expect_u("... nothing written", q0, 0x1111);
          expect_u("VidMemory 43 with the wrong nargs: not handled", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 43, a, 3, &ret, P), 0); expect_u("... nothing written", q0, 0x1111);
          a[1] = 0; expect_u("VidMemory 43 with a NULL len pointer: not handled", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 43, a, 2, &ret, P), 0);
          a[1] = (uint64_t)(uintptr_t)&q0 + 4; expect_u("VidMemory 43 with a misaligned len pointer: not handled", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 43, a, 2, &ret, P), 0);
          expect_u("VidMemory 43 with a user-space pointer and the real kernel base: not handled", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 43, a, 2, &ret, kKernelMin), 0);
          a[1] = (uint64_t)(uintptr_t)&q0; expect_u("... a real host pointer is also refused by the kernel-half check", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 43, a, 2, &ret, kKernelMin), 0); expect_u("... nothing written", q0, 0x1111); }
        { uint64_t a[6] = { 0 }; ret = 0xdead;
          expect_u("VidMemory 61 allocPhysical: handled", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 61, a, 0, &ret, P), 1); expect_u("... returns true", ret, 1);
          ret = 0xdead; expect_u("VidMemory 61 with the bit OFF: not handled", (uint64_t)vhook_handle(0, N48_VC_VIDMEMORY, 61, a, 0, &ret, P), 0); expect_u("... ret untouched", ret, 0xdead);
          expect_u("VidMemory 62 deallocPhysical is not handled (the default is paravirt's empty body)", (uint64_t)vhook_handle(vm, N48_VC_VIDMEMORY, 62, a, 0, &ret, P), 0); }
        for (uint32_t slot : { 60u, 61u }) {
            i0 = 0x3333; ret = 0xdead; uint64_t a[6] = { 1, 2, (uint64_t)(uintptr_t)&i0, 0, 0, 0 };
            char w[96]; std::snprintf(w, sizeof(w), "Resource %u (level offset): handled", slot);
            expect_u(w, (uint64_t)vhook_handle(rs, N48_VC_RESOURCE, slot, a, 3, &ret, P), 1); expect_u("... *int = 0", i0, 0); expect_u("... returns 0", ret, 0);
            i0 = 0x3333; ret = 0xdead;
            expect_u("Resource level offset with the bit OFF: not handled", (uint64_t)vhook_handle(0, N48_VC_RESOURCE, slot, a, 3, &ret, P), 0); expect_u("... nothing written", i0, 0x3333);
            expect_u("Resource level offset with only the VidMemory bit: not handled", (uint64_t)vhook_handle(vm, N48_VC_RESOURCE, slot, a, 3, &ret, P), 0); expect_u("... nothing written", i0, 0x3333);
            a[2] = (uint64_t)(uintptr_t)&i0 + 2; expect_u("Resource level offset with a misaligned pointer: not handled", (uint64_t)vhook_handle(rs, N48_VC_RESOURCE, slot, a, 3, &ret, P), 0); expect_u("... nothing written", i0, 0x3333);
            a[2] = 0; expect_u("Resource level offset with a NULL pointer: not handled", (uint64_t)vhook_handle(rs, N48_VC_RESOURCE, slot, a, 3, &ret, P), 0);
        }
        { q0 = 0x1111; q1 = 0x2222; ret = 0xdead; uint64_t a[6] = { (uint64_t)(uintptr_t)&q0, (uint64_t)(uintptr_t)&q1, 0, 0, 0, 0 };
          expect_u("Resource 62 calculateIOSurfaceDeviceCacheVRAMBytes: handled", (uint64_t)vhook_handle(rs, N48_VC_RESOURCE, 62, a, 2, &ret, P), 1);
          expect_u("... first output = 0", q0, 0); expect_u("... second output = 0", q1, 0); expect_u("... returns 0", ret, 0);
          q0 = 0x1111; q1 = 0x2222; ret = 0xdead; a[1] = 0;
          expect_u("Resource 62 with one NULL output: not handled", (uint64_t)vhook_handle(rs, N48_VC_RESOURCE, 62, a, 2, &ret, P), 0);
          expect_u("... and the OTHER output was not written either (all pointers are checked before any write)", q0, 0x1111);
          a[1] = a[0]; expect_u("Resource 62 with both outputs the same pointer: not handled", (uint64_t)vhook_handle(rs, N48_VC_RESOURCE, 62, a, 2, &ret, P), 0);
          a[1] = (uint64_t)(uintptr_t)&q1; expect_u("Resource 62 with the bit OFF: not handled", (uint64_t)vhook_handle(0, N48_VC_RESOURCE, 62, a, 2, &ret, P), 0); expect_u("... nothing written", q1, 0x2222); }
        { uint64_t a[6] = { (uint64_t)(uintptr_t)&q0, (uint64_t)(uintptr_t)&q1, (uint64_t)(uintptr_t)&i0, 0, 0, 0 }; q0 = 0x1111; q1 = 0x2222; i0 = 0x3333;
          const uint32_t all = N48_FACT_ALL; uint32_t hits = 0;
          const uint32_t classes[] = { N48_VC_EVENTMACHINE, N48_VC_SHAREDUC, N48_VC_CMDQUEUE, N48_VC_CTX2D };
          for (uint32_t c : classes) for (uint32_t sl = 0; sl < 400; ++sl) for (uint32_t n = 0; n <= 6; ++n) hits += (uint32_t)vhook_handle(all, c, sl, a, n, &ret, P);
          expect_u("no other class is ever handled, whatever the mask (default = the aux kext's own defaults)", hits, 0);
          expect_u("nothing written by the unhandled classes", q0 + q1 + i0, 0x1111 + 0x2222 + 0x3333);
          hits = 0; for (uint32_t sl = 0; sl < 400; ++sl) for (uint32_t n = 0; n <= 6; ++n) { if (sl == 43 || sl == 61) continue; hits += (uint32_t)vhook_handle(all, N48_VC_VIDMEMORY, sl, a, n, &ret, P); }
          expect_u("VidMemory: only slots 43 and 61 are handled", hits, 0);
          hits = 0; for (uint32_t sl = 0; sl < 400; ++sl) for (uint32_t n = 0; n <= 6; ++n) { if (sl == 60 || sl == 61 || sl == 62) continue; hits += (uint32_t)vhook_handle(all, N48_VC_RESOURCE, sl, a, n, &ret, P); }
          expect_u("Resource: only slots 60, 61 and 62 are handled", hits, 0);
          expect_u("a NULL ret is refused", (uint64_t)vhook_handle(all, N48_VC_VIDMEMORY, 61, a, 0, nullptr, P), 0);
          expect_u("a NULL args is refused", (uint64_t)vhook_handle(all, N48_VC_VIDMEMORY, 61, nullptr, 0, &ret, P), 0);
          expect_u("the default mask 0 handles nothing in any class", (uint64_t)vhook_handle(0, N48_VC_RESOURCE, 60, a, 3, &ret, P) + (uint64_t)vhook_handle(0, N48_VC_VIDMEMORY, 61, a, 0, &ret, P), 0); }
    }
}

static void m5_pins(const std::string &root, const std::string &auxroot) {
    const std::string K = root + "/src/navi48-bringup/";
    const std::string nub = slurp(K + "src/Navi48MetalNub.cpp"), nubh = slurp(K + "src/Navi48MetalNub.hpp"), cli = slurp(K + "src/Navi48NativeClient.cpp"), br = slurp(K + "src/Navi48Bringup.cpp");
    const std::string abi = slurp(K + "src/Navi48NativeABI.h"), plist = slurp(K + "Info.plist"), mk = slurp(K + "Makefile"), engh = slurp(K + "src/amd/native_s1c.h"), pure = slurp(K + "src/amd/native_metal_pure.h");
    const std::string opsh = slurp(K + "src/Navi48MetalOps.h");
    expect(!nub.empty() && !nubh.empty() && !cli.empty() && !br.empty() && !abi.empty() && !pure.empty() && !opsh.empty(), "the sources are readable from the root given");
    // -- never at boot: the publish code has ONE caller, the native client's selector; nothing else in the kext names the nub except the stop hook --
    expect_u("the client calls selectorPublish exactly once", count_of(cli, "Navi48MetalNub::selectorPublish("), 1);
    expect_u("Navi48Bringup.cpp never publishes the nub", count_of(br, "selectorPublish"), 0);
    expect(br.find("Navi48MetalNub::shutdown();") != std::string::npos && count_of(br, "Navi48MetalNub::") == 1, "Navi48Bringup.cpp's only use of the nub is the shutdown hook in stop()");
    {   // 0.0.612: native_s1c.cpp names the nub exactly once, for the HUNG latch's Ready write, and never publishes it
        const std::string e = slurp(K + "src/amd/native_s1c.cpp");
        expect_u("native_s1c.cpp calls Navi48MetalNub::hungLatched() exactly once (hang_announce)", count_of(e, "Navi48MetalNub::hungLatched()"), 1);
        expect_u("native_s1c.cpp never publishes the nub", count_of(e, "selectorPublish"), 0);
        expect_u("native_s1c.cpp names Navi48MetalNub exactly TWICE in total (the include of its header and the one hungLatched call): a third reference is a new use of the nub", count_of(e, "Navi48MetalNub"), 2);
        expect(e.find("#include \"Navi48MetalNub.hpp\"") != std::string::npos, "native_s1c.cpp includes the nub header (one of the two references)");
    }
    {   // every other source file: no reference to the nub at all
        const char *files[] = { "src/amd/native_s1b.cpp", "src/dcn/navi48_dcn.cpp", "src/Navi48UserClient.cpp", "src/psp.cpp", "src/ipdiscovery.cpp" };
        for (const char *f : files) { const std::string s = slurp(K + f); expect(!s.empty() && s.find("Navi48MetalNub") == std::string::npos && s.find("selectorPublish") == std::string::npos, "no other source references the nub or publishes it"); }
    }
    expect(nub.find("registerService()") != std::string::npos && count_of(nub, "->registerService()") == 1, "the nub is registered in exactly one place (selectorPublish)");
    // -- ordering in selectorPublish: gate verdict, then allocation, then attach, then state, then registration --
    {
        // 0.0.670: the body is publish_impl, shared by the selector and the boot path (the two entry points are one-liners below it)
        const std::string b = fn_body(nub, "static IOReturn publish_impl(");
        const size_t v = b.find("n48metal::publish_verdict("), a = b.find("OSTypeAlloc(Navi48MetalNub)"), at = b.find("nub->attach(owner)"), st = b.find("gState = n48metal::sm_after_publish"), rg = b.find("nub->registerService()");
        expect(!b.empty() && v != std::string::npos && a != std::string::npos && at != std::string::npos && st != std::string::npos && rg != std::string::npos, "selectorPublish has all its steps");
        expect(v < a && a < at && at < st && st < rg, "order in selectorPublish: gate verdict < allocation < attach < state < registerService");
        expect(b.find("verdict != n48metal::kOk") != std::string::npos && b.find("return (IOReturn)verdict;") != std::string::npos, "a refusing verdict returns before anything is created");
        expect(b.find("nub->retain();") != std::string::npos && b.find("nub->retain();") < b.find("IOLockUnlock(gLock);\n\tMNLOG(\"nub published by the %s") && b.find("nub->registerService()") < b.find("nub->release();  ", b.find("nub->registerService()")), "the nub is retained under the lock and released only after registerService()");
        {   // free() takes gLock, so no path may release the nub while holding it
            const size_t f = b.find("if (rc != kIOReturnSuccess) {"); const size_t u = b.find("IOLockUnlock(gLock);", f), r = b.find("nub->release();", f);
            expect(f != std::string::npos && u != std::string::npos && r != std::string::npos && u < r, "the failed-publish path unlocks BEFORE it releases the nub (free() takes the same lock)");
            expect(fn_body(nub, "void Navi48MetalNub::free()").find("IOLockLock(gLock)") != std::string::npos, "free() takes gLock (why the release must come after the unlock)");
        }
        expect(b.find("gate_now(flags, true)") != std::string::npos, "publish asks for the boot-arg");
        const std::string w = fn_body(nub, "IOReturn Navi48MetalNub::selectorWithdraw(");
        expect(w.find("gate_now(flags, false)") != std::string::npos && w.find("n48metal::withdraw_verdict(") != std::string::npos, "withdraw never asks for the boot-arg (cleanup)");
        const std::string g = fn_body(nub, "n48metal::GateIn gate_now(");
        expect(g.find("PE_parse_boot_argn(\"navi48-metal\"") != std::string::npos && g.find("v == 1u") != std::string::npos, "the boot-arg is navi48-metal and only =1 counts");
        expect(g.find("amdgpu::n1c_hello_done()") != std::string::npos && g.find("amdgpu::n1c_hung()") != std::string::npos && g.find("s.positivePass") != std::string::npos && g.find("s.ran") != std::string::npos &&
               g.find("n48native::kGateOn") != std::string::npos && g.find("s.stopped") != std::string::npos, "the gate reads Hello, HUNG, S1b gate / ran / POSITIVE / stopped from the live state");
    }
    // -- the selectors --
    expect_u("N48N_SEL_METAL_NUB_PUBLISH", N48N_SEL_METAL_NUB_PUBLISH, 19);
    expect_u("N48N_SEL_METAL_NUB_WITHDRAW", N48N_SEL_METAL_NUB_WITHDRAW, 20);
    expect_u("selector count 1.8", N48N_SEL_COUNT_1_8, 21); expect_u("selector count 1.9", N48N_SEL_COUNT_1_9, 22); expect_u("BoImportHost is selector 21", N48N_SEL_BO_IMPORT_HOST, 21);
    expect_u("selector count 1.7 unchanged", N48N_SEL_COUNT_1_7, 19);
    expect_u("ABI major unchanged", N48N_ABI_VERSION, 1);
    expect_u("ABI minor 12 (1.12 since 0.0.662: instance 1 behind navi48-m6flip1; 1.11 since 0.0.661: the instance-2 scanout selectors 22..26)", N48N_ABI_MINOR, 12);
    expect(cli.find("case N48N_SEL_METAL_NUB_PUBLISH:\n\t\tif (!shape(1, 4, 0, 0)) return kIOReturnBadArgument;") != std::string::npos, "publish: shape 1 scalar in, 4 out, no structs");
    expect(cli.find("case N48N_SEL_METAL_NUB_WITHDRAW:\n\t\tif (!shape(1, 1, 0, 0)) return kIOReturnBadArgument;") != std::string::npos, "withdraw: shape 1 in, 1 out, no structs");
    {   // the selectors sit behind the same guard as every other native selector
        const std::string e = fn_body(cli, "IOReturn IOAccelNavi48NativeClient::externalMethod(");
        const size_t g = e.find("if (!args || !opened) return kIOReturnNotReady;"), sw = e.find("switch (selector)"), p = e.find("case N48N_SEL_METAL_NUB_PUBLISH:");
        expect(g != std::string::npos && sw != std::string::npos && p != std::string::npos && g < sw && sw < p, "the nub selectors are inside the opened-session switch");
        expect(count_of(e, "case N48N_SEL_") == 27, "the native client dispatches exactly the 27 selectors of ABI 1.11 (0.0.661: + the five instance-2 scanout selectors 22..26)");
    }
    // -- the hooks are the pure code --
    expect(nub.find("n48metal::config_populate(cfg, bytes, (uint64_t)(uintptr_t)kAccelName)") != std::string::npos, "populate_config is the pure config_populate on the kext's own name string");
    expect(nub.find("n48metal::stamp_page_ok(phys, seg, md->getLength())") != std::string::npos, "the stamp page is judged by stamp_page_ok");
    expect(nub.find("n48metal::task_window(kind, size, reserve)") != std::string::npos, "task_window is the pure one");
    expect(nub.find("n48metal::factory_mask(present, v)") != std::string::npos && nub.find("\"navi48-metal-fact\"") != std::string::npos, "factory_mask is the pure one behind navi48-metal-fact");
    expect(nub.find("return (int)kIOReturnUnsupported;") != std::string::npos, "mm_hook refuses (no map entry point before #10)");
    expect(count_of(nub, "N48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 666u, n48metal::kOpsFlags, 0u,") == 2 && nub.find("op_vhook, n48metal::kOpsCaps, 0,") != std::string::npos && nub.find("N48_DISP_F_ON, 0u, op_disp_hook, op_pci_device,\n\top_native_open,\n};") != std::string::npos, "the ABI-3 ops table identity: magic, abi 3, 152 bytes, build 657, flags, the generic hook and the caps, the display flag and the two display members, native_open");
    expect(nub.find("const N48MetalOps gOpsOff = {\n\tN48_METAL_OPS_MAGIC, N48_METAL_ABI, (uint32_t)sizeof(N48MetalOps), 666u, n48metal::kOpsFlags, 0u,") != std::string::npos && nub.find("0u, 0u, nullptr, nullptr,\n\top_native_open,\n};") != std::string::npos, "the OFF table is ABI 3 / 152 bytes with no display members (flags 0, no hook, no PCI getter) and native_open");
    expect(nub.find("functionName->isEqualTo(N48_METAL_FN_SYMBOL)") != std::string::npos && nub.find("*(const N48MetalOps **)param1 = n48disp::ops_shape(n48disp_latched_on()).dispFlags != 0u ? &gOps : &gOpsOff;") != std::string::npos, "callPlatformFunction n48.metal.ops hands out the static table the latch chooses (the display-OFF ABI-3 table unless boot-arg navi48-metal-disp=1)");
    expect(nub.find("if (!d || d != gDev || d->magic != kDevMagic) return;") != std::string::npos, "device_close is idempotent and identity-checked");
    // -- 0.0.611: the vhook and the config are the pure code --
    expect(nub.find("n48metal::vhook_handle(mask, cls, slot, args, nargs, ret, n48metal::kKernelMin)") != std::string::npos && nub.find("n48metal::vhook_count(gVhCounts, cls, slot)") != std::string::npos &&
           nub.find("n48metal::vhook_log_kind(n)") != std::string::npos, "op_vhook counts, handles and rate-limits through the pure code");
    { const std::string b = fn_body(nub, "int op_vhook(");
      const size_t c = b.find("vhook_count("), l = b.find("MNLOG("), h = b.find("vhook_handle(");
      expect(!b.empty() && c != std::string::npos && h != std::string::npos && l != std::string::npos && count_of(b, "MNLOG(") == 2 && b.find("if (k == n48metal::kVhLogFull) MNLOG(") != std::string::npos &&
             b.find("else if (k == n48metal::kVhLogCount) MNLOG(") != std::string::npos, "op_vhook logs only behind the rate-limit decision (two guarded MNLOG lines, no unconditional one)");
      expect(b.find("op_factory_mask(nullptr)") != std::string::npos && b.find("N48_VC_VIDMEMORY || cls == N48_VC_RESOURCE") != std::string::npos, "op_vhook reads the factory mask only for the two optional classes"); }
    { const std::string b = fn_body(pure, "inline int config_populate(");
      const size_t w = b.find("work[i] = cfg[i]"), f = b.find("config_fill(work"), k = b.find("config_check(work)"), r = b.find("return (int)(10u"), cp = b.find("cfg[i] = work[i]");
      expect(w != std::string::npos && f != std::string::npos && k != std::string::npos && r != std::string::npos && cp != std::string::npos && w < f && f < k && k < r && r < cp && b.find("config_fill(cfg") == std::string::npos,
             "config_populate: local copy < fill the copy < check the copy < refusal return < copy back (the caller's struct is written last, only after the check)"); }
    // -- the nub writes no hardware register --
    for (const char *t : { "WREG32", "RREG32", "bar0", "bar2", "->dev", "MMIO", "WREG64" }) expect(nub.find(t) == std::string::npos, "the nub source names no register access");
    // -- the version --
    {
        const size_t p = engh.find("kN1cKextBuild = "); const int build = p == std::string::npos ? -1 : std::atoi(engh.c_str() + p + 16);
        const size_t v = plist.find("<string>0.0."); const int ver = v == std::string::npos ? -2 : std::atoi(plist.c_str() + v + 12);
        expect(build > 0 && build == ver, "kN1cKextBuild equals the Info.plist patch version");
        expect_u("Info.plist is 0.0.670", (uint64_t)ver, 670); expect_u("the plist carries the version twice", count_of(plist, "0.0.670"), 2);
        expect(nub.find("666u") != std::string::npos && nub.find("658u") == std::string::npos && nub.find("613u") == std::string::npos, "both ops tables carry the build 666");
    }
    expect(mk.find("src/Navi48MetalNub.cpp") != std::string::npos, "the Makefile builds Navi48MetalNub.cpp");
    // ---- M7 (0.0.670): the boot publish and the boot hold ----
    {
        const std::string arm = slurp(K + "src/amd/native_armboot.cpp"), armh = slurp(K + "src/amd/native_armboot.h");
        expect(!arm.empty() && !armh.empty(), "the boot worker's source is readable");
        // (1) the boot publish exists, goes through the ONE publish implementation, and has exactly one caller
        expect(nub.find("IOReturn Navi48MetalNub::bootPublish(Navi48Bringup *owner) {") != std::string::npos &&
               nub.find("return publish_impl(owner, 0, out, /*boot=*/true);") != std::string::npos &&
               nub.find("return publish_impl(owner, flags, out, /*boot=*/false);") != std::string::npos,
               "0.0.670: the selector and the boot path are one-line wrappers around ONE publish_impl (the ordering and the failure path cannot drift apart)");
        expect(count_of(arm, "Navi48MetalNub::bootPublish(") == 1 && arm.find("if (gNubLatch == kLatchOn) {") != std::string::npos &&
               count_of(arm, "if (gNubLatch == kLatchOn) {") == 1,
               "0.0.670: the boot worker is the ONLY caller of the boot publish, and navi48-nubboot=0 turns it off (the skip is the branch around the publish)");
        expect(count_of(nub, "n48metal::publish_at_boot_verdict(") == 1 && nub.find("boot ? n48metal::publish_at_boot_verdict(") != std::string::npos,
               "0.0.670: the boot path's verdict is the pure publish_at_boot_verdict, chosen by the boot flag");
        // (2) the accelerator's property gate: written at publish, withheld by one boot-arg
        expect(nub.find("if (accel_bind_allowed()) nub->setProperty(N48_METAL_ACCEL_GATE_KEY, kOSBooleanTrue);") != std::string::npos &&
               nub.find("PE_parse_boot_argn(\"navi48-accelbind\"") != std::string::npos &&
               nub.find("if (!PE_parse_boot_argn(\"navi48-accelbind\"") != std::string::npos && nub.find("return v != 0u;") != std::string::npos,
               "0.0.670: the nub carries the accelerator's IOPropertyMatch gate, and navi48-accelbind=0 really withholds it (present-and-zero is the refusal)");
        expect(nub.find("nub->setProperty(N48_METAL_ACCEL_GATE_KEY, kOSBooleanTrue);") < nub.find("if (nub->attach(owner))"),
               "0.0.670: the gate is written BEFORE attach/registerService, so the aux kext's probe sees it");
        // (3) the boot hold: adjustBusy on the BRING-UP service, taken once, released by four CAS-guarded paths, and never twice
        // the CALLS, not the prose: the log lines and the comments name adjustBusy too, so the pins count `svc->adjustBusy(...)` exactly
        expect_u("0.0.670: the hold raises the busy count exactly once (the call in hold_take)", count_of(arm, "svc->adjustBusy(1)"), 1);
        expect_u("0.0.670: four release paths, each behind its own CAS (release / abandon / cap / stop)", count_of(arm, "svc->adjustBusy(-1);"), 4);
        expect(count_of(arm, "OSCompareAndSwap") >= 6 && arm.find("&gHold") != std::string::npos, "0.0.670: every transition of the hold is a CAS on the one word");
        expect(arm.find("n48metal::hold_after_take()") != std::string::npos && arm.find("n48metal::hold_after_claim()") != std::string::npos &&
               arm.find("n48metal::hold_after_cap()") != std::string::npos && arm.find("n48metal::hold_after_release()") != std::string::npos,
               "0.0.670: the four transitions come from the pure table, not from literals");
        expect(arm.find("thread_call_allocate(&cap_call, svc)") != std::string::npos && arm.find("thread_call_enter_delayed(cap, dl)") != std::string::npos &&
               arm.find("clock_interval_to_deadline(gCapMs, kMillisecondScale, &dl)") != std::string::npos,
               "0.0.670: the cap is a real timer, armed from the clamped navi48-holdcap value");
        expect(arm.find("hold_abandon(svc, \"no cap timer\")") != std::string::npos,
               "0.0.670: a hold whose cap could not be armed is given straight back (a hold with no cap is a boot with no bound)");
        // (4) the CAS guard, in the arm sequence and BEFORE the arm byte
        { const size_t c = arm.find("if (gHoldUsed != 0u && !hold_claim())"), a = arm.find("n48disp::kActArm, 1ull");
          expect(c != std::string::npos && a != std::string::npos && c < a, "0.0.670: the claim happens BEFORE pipearm, and a failed claim refuses the arm (never arm behind WindowServer)");
          expect(arm.find("NOT arming with WindowServer up or on its way") != std::string::npos, "0.0.670: ... and says so"); }
        // (5) fail-open: every early exit gives the hold back, and nothing here can fail the kext load
        expect(arm.find("hold_finish(svc, false, \"the boot publish was refused\")") != std::string::npos &&
               arm.find("hold_finish(svc, r.ok, r.why)") != std::string::npos &&
               count_of(arm, "hold_finish(svc,") == 4,
               "0.0.670: the worker's exits (stop / no service / refused publish / the arm verdict) all go through hold_finish");
        expect(armh.find("void n48armboot_boot(void);") != std::string::npos && arm.find("void n48armboot_boot(void)") != std::string::npos,
               "0.0.670: the entry point is still the void one start() calls: no return code, so no way to fail the load");
        expect(count_of(arm, "PE_parse_boot_argn(") == 4 && arm.find("\"navi48-nohold\"") != std::string::npos && arm.find("\"navi48-nubboot\"") != std::string::npos &&
               arm.find("\"navi48-holdcap\"") != std::string::npos && arm.find("\"navi48-armboot\"") != std::string::npos,
               "0.0.670: four latched switches: the master, the no-hold lever, the no-boot-publish kill switch and the cap");
        // (6) the hold is on the SAME IOService the kext is (the provider's busy state follows it), and never on the PCI device
        expect(arm.find("const IOReturn rc = owner ? Navi48MetalNub::bootPublish(owner) : kIOReturnNotReady;") != std::string::npos,
               "0.0.670: the owner cast is nil-checked before the boot publish (a NULL owner must never be dereferenced)");
        expect(arm.find("IOService *svc = navi48_bringup_service();") != std::string::npos && arm.find("hold_take(svc);") != std::string::npos &&
               arm.find("IOService *navi48_bringup_service(void);") != std::string::npos,
               "0.0.670: the hold is taken on the BRING-UP IOService (navi48_bringup_service), the same object the bring-up kext registers, never on a PCI device");
    }
    expect(abi.find("N48N_SEL_METAL_NUB_PUBLISH") != std::string::npos && abi.find("Never done at boot") != std::string::npos, "the ABI header documents the publish selector");
    { const std::string doc = slurp(root + "/an internal design note");
      expect(doc.find("ABI 1.8 addendum") != std::string::npos && doc.find("N48N_SEL_METAL_NUB_PUBLISH") != std::string::npos && doc.find("N48N_SEL_METAL_NUB_WITHDRAW") != std::string::npos && doc.find("navi48-metal") != std::string::npos,
             "NATIVE-S1C-ABI.md carries the ABI 1.8 addendum"); }
    // -- the ops header: identical to the aux kext's copy --
    {
        const std::string aux = slurp(auxroot + "/tools/native/navi48accel/src/Navi48MetalOps.h");
        if (aux.empty()) std::printf("NOTE: the aux kext copy of Navi48MetalOps.h is not present under %s; compare it with the aux test\n", auxroot.c_str());
        else expect(aux == opsh, "Navi48MetalOps.h is byte-identical in the bring-up kext and the aux kext");
    }
    // -- the tool --
    { const std::string tool = slurp(root + "/tools/native/n48nub.c"), bsh = slurp(root + "/tools/native/build.sh");
      expect(tool.find("N48N_SEL_METAL_NUB_PUBLISH") != std::string::npos && tool.find("N48N_SEL_METAL_NUB_WITHDRAW") != std::string::npos && tool.find("Navi48MetalNub") != std::string::npos &&
             tool.find("n48n_hello(") != std::string::npos, "the n48nub tool says Hello, then publishes / withdraws, and inspects the nub");
      expect_u("both publish and withdraw say Hello first", count_of(tool, "if (hello(&c)) return 1;"), 2);
      expect(bsh.find("n48nub:n48nub.c") != std::string::npos, "build.sh builds n48nub"); }
    // -- banned strings --
    const std::string bad1 = std::string("pipe+0x2") + "80", bad2 = std::string("+0x2") + "82", bad3 = std::string("+0x2") + "99";
    for (const std::string *f : { &nub, &nubh, &pure, &opsh }) expect(f->find(bad1) == std::string::npos && f->find(bad2) == std::string::npos && f->find(bad3) == std::string::npos, "no banned pipe offsets in the new files");
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string auxroot = argc > 2 ? argv[2] : root;
    m1_verdicts(); m2_sm(); m3_config(); m4_misc(); m6_config_atomic_and_vhook(); m5_pins(root, auxroot);
    std::printf("native_metal_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
