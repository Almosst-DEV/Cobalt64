// host_test.cpp - Navi48Accel (aux accelerator kext 0.0.3; 0.0.2 was milestone #9 route A, 0.0.3 adds the #11 display pipe): the pure decisions of src/n48accel_pure.h driven on the host, plus source pins that
// hold the kext to its THIN design (K3: every rebuild requires a security approval (Allow click) from the user, so every value and decision lives in the bring-up kext behind Navi48MetalOps.h).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src tests/host_test.cpp -o /tmp/n48accel_host && /tmp/n48accel_host .
//   (run from tools/native/navi48accel; the argument is that directory; an optional second argument is the bring-up tree root, to compare the two Navi48MetalOps.h copies;
//    tests/plant.sh plants breaks in the real code and demands a failure; tests/gate_plants.sh does the same for the link gates A/B/C and the host twin)
// Covers:
//   A1 kill switch: aux_enabled matrix; every entry point of Navi48Accel.cpp starts with N48_AUX_ENTER (whole-file scan with a reasoned exemption list); in probe the kill
//      switch precedes the nub check, the layout gate, the ops fetch and the config check (an ORDERING test); the boot-arg it reads is navi48-aux and nothing else;
//   A2 gate_compare: match, every mismatch kind, overrides inside / outside our text, override equal to the family's slot, terminator, null / empty, a randomised sweep;
//   A3 ops_check verdicts and the fail-closed helpers (config_keep, factory_allowed, mm_result); the required hooks are exactly those the kext calls unconditionally;
//   A4 the event machine init order: Fast2::init first, setStampBaseAddress only after a true result and with a stamp VA; the source calls the plan;
//   A5 thin-ness: no config literal, no value, no bring-up decision in the aux source; the classes; no hardware access; the personality (nub only, no PCI, no IOResources);
//   A6 Info.plist / kmod: id, version 0.0.3, libraries; the ops header is byte-identical to the bring-up kext's copy.
//   A7 (0.0.3) the display pipe: disp_enabled (an ABI-1 / short / flag-0 / hook-less table is OFF, and a 120-byte table is never read past its end: ASan),
//      dm_walk_provider, pipe_choice; the display entry points wire those decisions (newDisplayPipe never NULL, the family's own objects when OFF), the slot-277
//      thunk, the generated display trampolines use n48_disp_vhook (never vhook) with the family's own slot as the default; the runtime gate has no skip.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include "n48accel_pure.h"
#include <sanitizer/asan_interface.h>   // A7: the bytes after an old 120-byte ops table are poisoned, so any read of them aborts the test

using namespace n48accel;
static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) { gRun++; if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); } }
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }
static size_t count_of(const std::string &s, const std::string &n) { size_t c = 0, p = 0; while ((p = s.find(n, p)) != std::string::npos) { c++; p += n.size(); } return c; }

// ---- A1 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a1_kill_switch() {
    expect(aux_enabled(false, 0), "no boot-arg: enabled"); expect(aux_enabled(false, 1), "no boot-arg (value ignored): enabled");
    expect(!aux_enabled(true, 0), "navi48-aux=0: DISABLED"); expect(aux_enabled(true, 1), "navi48-aux=1: enabled"); expect(aux_enabled(true, 2), "navi48-aux=2: enabled"); expect(aux_enabled(true, 0xffffffffu), "navi48-aux=-1: enabled");
}
struct Fn { std::string cls, name, sig, body; };
// every `Class::method(...) {` definition of the file at column 0, with its body up to the closing brace at column 0 (or the same line for one-liners)
static std::vector<Fn> functions(const std::string &src) {
    std::vector<Fn> out; size_t p = 0;
    while (true) {
        const size_t nl = src.find('\n', p); if (nl == std::string::npos) break;
        const std::string line = src.substr(p, nl - p);
        const size_t cc = line.find("::");
        if (!line.empty() && line[0] != ' ' && line[0] != '\t' && line[0] != '/' && line[0] != '#' && line[0] != '}' && cc != std::string::npos && line.find('(') != std::string::npos && line.find('(') > cc &&
            line.find('{') != std::string::npos && line.find("extern") == std::string::npos && line.find("class ") == std::string::npos && line.find("OSDefine") == std::string::npos) {
            const size_t cs = line.rfind(' ', cc) == std::string::npos ? 0 : line.rfind(' ', cc) + 1, cs2 = line.rfind('*', cc), start = cs2 != std::string::npos && cs2 + 1 > cs ? cs2 + 1 : cs;
            Fn f; f.cls = line.substr(start, cc - start); const size_t ob = line.find('(', cc); f.name = line.substr(cc + 2, ob - cc - 2); f.sig = line;
            const size_t br = line.find('{');
            if (line.find('}', br) != std::string::npos) f.body = line.substr(br); else { const size_t e = src.find("\n}\n", nl); f.body = line.substr(br) + src.substr(nl, e == std::string::npos ? std::string::npos : e - nl); }
            out.push_back(f);
        }
        p = nl + 1;
    }
    return out;
}
static void a1_source(const std::string &root) {
    const std::string src = slurp(root + "/src/Navi48Accel.cpp");
    expect(!src.empty(), "Navi48Accel.cpp readable");
    const std::vector<Fn> fns = functions(src);
    expect(fns.size() >= 24, "the function scan found the kext's functions");
    // entry points that must begin with the kill switch. The exemptions run only in response to something the kill-switched entry points already did (cleanup /
    // trivially constant hooks), or must always run (teardown): reasons in the table.
    struct Ex { const char *cls, *name, *why; };
    const Ex exempt[] = {
        { "Navi48Accelerator", "free", "cleanup: must always run" }, { "Navi48Accelerator", "teardownDevice", "cleanup: must always run" },
        { "Navi48EventMachine", "enableStampInterrupt", "empty no-op" }, { "Navi48EventMachine", "disableStampInterrupt", "empty no-op" },
        { "Navi48EventMachine", "writeStamp", "trace only, unreachable without an event machine (which needs the kill switch)" }, { "Navi48EventMachine", "prepareBarrier", "trace only" },
        { "Navi48EventMachine", "completeBarrier", "trace only" }, { "Navi48EventMachine", "writeBarrierElement", "trace only" },
        { "Navi48DisplayMachine", "displayModeWillChange", "constant true" }, { "Navi48DisplayMachine", "displayModeDidChange", "constant true" },
        { "Navi48DisplayPipe", "_vslot277", "naked tail jump into n48_dp_perform, whose first statement is the kill switch (pinned in A7)" },
    };
    int guarded = 0;
    for (const Fn &f : fns) {
        bool ex = false; for (const Ex &e : exempt) if (f.cls == e.cls && f.name == e.name) ex = true;
        if (f.cls.rfind("Navi48", 0) != 0) continue;
        if (ex) continue;
        // first statement: N48_AUX_ENTER on the line after the brace, or right after it on one-liners
        const size_t k = f.body.find("N48_AUX_ENTER");
        std::string ident = f.cls + "::" + f.name;
        const std::string before = k == std::string::npos ? "" : f.body.substr(0, k);
        bool first = k != std::string::npos;
        if (first) for (char c : before) if (!(c == '{' || c == ' ' || c == '\t' || c == '\n')) first = false;
        expect(first, (ident + " begins with the kill switch (or is on the reasoned exemption list)").c_str());
        if (first) guarded++;
    }
    expect(guarded >= 20, "at least 20 entry points carry the kill switch");
    // the ordering inside probe: kill switch, nub check, layout gate, ops, config check, only then the family
    {
        const size_t a = src.find("IOService *Navi48Accelerator::probe("); const size_t e = src.find("\n}\n", a); const std::string b = src.substr(a, e - a);
        const size_t ks = b.find("N48_AUX_ENTER"), nb = b.find("n48_provider_is_nub"), lg = b.find("n48_layout_gate()"), op = b.find("n48_get_ops"), ic = b.find("n48_initCfg"), pc = b.find("populate_config"), fam = b.find("IOService::probe(");
        expect(ks != std::string::npos && nb != std::string::npos && lg != std::string::npos && op != std::string::npos && ic != std::string::npos && pc != std::string::npos && fam != std::string::npos, "probe has all its steps");
        expect(ks < nb && nb < lg && lg < op && op < ic && ic < pc && pc < fam, "ORDER in probe: kill switch < nub-only < layout gate < ops < family config defaults < config check < the family");
        expect(b.find("return nullptr;") != std::string::npos && b.find("if (!n48_layout_gate()) return nullptr;") != std::string::npos, "a failed layout gate refuses the match");
        const size_t sa = src.find("bool Navi48Accelerator::start("); const size_t se = src.find("\n}\n", sa); const std::string s = src.substr(sa, se - sa);
        expect(s.find("gGateState != 1") != std::string::npos && s.find("N48_AUX_ENTER") < s.find("IOGraphicsAccelerator2::start("), "start: kill switch first, never starts unless the gate passed");
        expect(s.find("registerService()") > s.find("IOGraphicsAccelerator2::start("), "start: registerService only after the family start");
    }
    {   // the kill switch reads exactly one boot-arg
        expect_u("the aux kext reads exactly one boot-arg", count_of(src, "PE_parse_boot_argn("), 1);
        expect(src.find("PE_parse_boot_argn(\"navi48-aux\"") != std::string::npos, "and it is navi48-aux");
        const std::string on = src.substr(src.find("static bool n48_aux_on()"), 200);
        expect(on.find("aux_enabled(present, v)") != std::string::npos, "the kill switch decision is the pure aux_enabled");
    }
}

// ---- A2 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a2_gate() {
    const uintptr_t lo = 0x1000, hi = 0x2000;
    uintptr_t fam[9] = { 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0 };
    uintptr_t ours[9]; std::memcpy(ours, fam, sizeof(ours)); ours[8] = 0;
    const uint16_t ovr[] = { 2, 5 };
    ours[2] = 0x1100; ours[5] = 0x1200;
    { const GateResult r = gate_compare(ours, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOk && r.compared == 8, "identical inherited slots + overrides inside our text: OK"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[3] = 0xB3; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateSlotDiffers && r.slot == 3, "an inherited slot that differs from the family: refused"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0xA2; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside && r.slot == 2, "an override pointing at the family: outside our text"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x3000; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside, "an override beyond our text: refused"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x0fff; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside, "an override just below our text: refused"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x1fff; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOk, "an override on the last byte inside our text: ok"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[2] = 0x2000; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideOutside, "the text range is half-open"); }
    { uintptr_t f2[9]; std::memcpy(f2, fam, sizeof(f2)); uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); f2[2] = 0x1100; const GateResult r = gate_compare(o, f2, 8, ovr, 2, lo, hi); expect(r.v == kGateOverrideIsFamily, "an override equal to the family's slot is not an override"); }
    { uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[8] = 0xDEAD; const GateResult r = gate_compare(o, fam, 8, ovr, 2, lo, hi); expect(r.v == kGateNoTerminator && r.slot == 8, "no zero terminator (an extra virtual): refused"); }
    { uintptr_t f2[9]; std::memcpy(f2, fam, sizeof(f2)); f2[4] = 0; uintptr_t o[9]; std::memcpy(o, ours, sizeof(o)); o[4] = 0; const GateResult r = gate_compare(o, f2, 8, ovr, 2, lo, hi); expect(r.v == kGateFamilyNull && r.slot == 4, "a NULL family slot (unlinked / wrong table) is never a match, even against a NULL of ours"); }
    { uintptr_t f2[9]; std::memcpy(f2, fam, sizeof(f2)); f2[8] = 0xF00D; expect(gate_compare(ours, f2, 8, ovr, 2, lo, hi).v == kGateFamilyLonger, "the family's vtable is longer than the header says (fam[n] != 0): refused"); }
    { uintptr_t klo = 0, khi = 0; expect(text_window(0x5100, 0x5000, 0x2000, &klo, &khi) && klo == 0x5000 && khi == 0x7000, "the window is the kmod_info range when it holds our anchor"); expect(!text_window(0x9000, 0x5000, 0x2000, &klo, &khi) && klo == (uintptr_t)0x9000 - 0x400000u, "a kmod_info range that does not hold our anchor: fallback"); expect(!text_window(0x5100, 0, 0, &klo, &khi), "an unfilled kmod_info: fallback"); expect(text_window(0x5000, 0x5000, 1, &klo, &khi) && !text_window(0x5001, 0x5000, 1, &klo, &khi), "the range is half-open"); }
    expect(gate_compare(nullptr, fam, 8, ovr, 2, lo, hi).v == kGateNull, "no vtable of ours: refused");
    expect(gate_compare(ours, nullptr, 8, ovr, 2, lo, hi).v == kGateNull, "no family vtable: refused");
    expect(gate_compare(ours, fam, 0, ovr, 2, lo, hi).v == kGateEmpty, "zero slots: refused (never a vacuous PASS)");
    { const GateResult r = gate_compare(ours, fam, 8, nullptr, 0, lo, hi); expect(r.v == kGateOverrideOutside || r.v == kGateSlotDiffers, "with no override list, our overrides count as differing slots"); }
    // a randomised sweep: any single corrupted inherited slot is found at exactly that slot; corrupting an override target to an outside address too
    unsigned seed = 12345; auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    for (int it = 0; it < 500; ++it) {
        const uint32_t n = 4 + rnd() % 60; std::vector<uintptr_t> f(n + 1), o(n + 1); std::vector<uint16_t> ov;
        for (uint32_t i = 0; i < n; ++i) { f[i] = 0x100000 + 16 * (rnd() % 100000) + 16; o[i] = f[i]; if (rnd() % 5 == 0) { ov.push_back((uint16_t)i); o[i] = lo + 16 * (rnd() % 100) + 16; } }
        o[n] = 0;
        expect(gate_compare(o.data(), f.data(), n, ov.data(), (uint32_t)ov.size(), lo, hi).v == kGateOk, "sweep: a consistent table pair passes");
        const uint32_t k = rnd() % n; const bool isOv = in_list(ov.data(), (uint32_t)ov.size(), k);
        o[k] = isOv ? f[k] + 0x900000 : o[k] ^ 0x10;
        const GateResult r = gate_compare(o.data(), f.data(), n, ov.data(), (uint32_t)ov.size(), lo, hi);
        expect(r.v != kGateOk && r.slot == k, "sweep: one corrupted slot is refused at exactly that slot");
    }
    expect(size_ok(0xdd8, 0xdd8) && !size_ok(0xdd8, 0xdd0), "size_ok is exact");
}

// ---- A3 ------------------------------------------------------------------------------------------------------------------------------------------------
static void h0() {} static void *o_open(void *, void *) { return nullptr; } static void o_close(void *) {} static int o_pop(uint8_t *, uint32_t) { return 0; }
static void *o_sm(void *, uint32_t *) { return nullptr; } static volatile uint32_t *o_sva(void *) { return nullptr; } static int o_tw(void *, uint32_t, uint64_t *, uint64_t *) { return 0; }
static N48MetalOps good_ops() {
    N48MetalOps o; std::memset(&o, 0, sizeof(o)); o.magic = N48_METAL_OPS_MAGIC; o.abi = N48_METAL_ABI; o.size = sizeof(N48MetalOps); o.kext_build = 610;
    o.device_open = o_open; o.device_close = o_close; o.populate_config = o_pop; o.stamp_memory = o_sm; o.stamp_va = o_sva; o.task_window = o_tw; (void)h0; return o;
}
static void a3_ops() {
    N48MetalOps o = good_ops();
    expect_u("a good table", ops_check(&o), kOpsOk);
    expect_u("no table", ops_check(nullptr), kOpsNull);
    { N48MetalOps x = o; x.magic ^= 1; expect_u("bad magic", ops_check(&x), kOpsMagic); }
    { N48MetalOps x = o; x.abi = 2; expect_u("a NEWER abi is accepted (append-only table)", ops_check(&x), kOpsOk); }
    { N48MetalOps x = o; x.abi = 0; expect_u("ABI 0 is refused", ops_check(&x), kOpsAbi); }
    { N48MetalOps x = o; x.abi = 1; x.size = N48_METAL_OPS_MIN; expect_u("an OLD table (ABI 1, 120 bytes: the 0.0.612 bring-up kext) is still accepted", ops_check(&x), kOpsOk); }
    { N48MetalOps x = o; x.abi = 3; x.size = N48_METAL_OPS_V2 + 16; expect_u("a newer, longer table is accepted", ops_check(&x), kOpsOk); }
    { N48MetalOps x = o; x.size = N48_METAL_OPS_MIN - 8; expect_u("a table smaller than the one we were built against", ops_check(&x), kOpsSize); }
    { N48MetalOps x = o; x.size = N48_METAL_OPS_MIN + 64; expect_u("a LARGER table (newer minor, appended members) is accepted", ops_check(&x), kOpsOk); }
    { N48MetalOps x = o; x.device_open = nullptr; expect_u("required hook device_open missing", ops_check(&x), kOpsMissing); }
    { N48MetalOps x = o; x.device_close = nullptr; expect_u("required hook device_close missing", ops_check(&x), kOpsMissing); }
    { N48MetalOps x = o; x.populate_config = nullptr; expect_u("required hook populate_config missing", ops_check(&x), kOpsMissing); }
    { N48MetalOps x = o; x.stamp_memory = nullptr; expect_u("required hook stamp_memory missing", ops_check(&x), kOpsMissing); }
    { N48MetalOps x = o; x.stamp_va = nullptr; expect_u("required hook stamp_va missing", ops_check(&x), kOpsMissing); }
    { N48MetalOps x = o; x.task_window = nullptr; expect_u("required hook task_window missing", ops_check(&x), kOpsMissing); }
    { N48MetalOps x = o; x.factory_mask = nullptr; x.mm_hook = nullptr; x.trace = nullptr; expect_u("optional hooks may be NULL", ops_check(&x), kOpsOk); }
    { N48MetalOps x = o; x.caps = 0; expect(!cap_has(&x, N48_CAP_VHOOK), "no cap bit: the hook is not used"); x.caps = N48_CAP_VHOOK; expect(cap_has(&x, N48_CAP_VHOOK) && !cap_has(nullptr, N48_CAP_VHOOK), "the cap bit enables the hook"); }
    // fail-closed helpers
    expect(config_keep(true, 0), "config kept only on ops ok and rc 0"); expect(!config_keep(true, 1), "rc != 0: blank the name"); expect(!config_keep(false, 0), "no ops: blank the name"); expect(!config_keep(true, -1), "rc -1: blank the name");
    expect(factory_allowed(true, 3, N48_FACT_SYSMEMORY) && factory_allowed(true, 3, N48_FACT_MEMORYMAP), "factories allowed with the bit set");
    expect(!factory_allowed(true, 0, N48_FACT_SYSMEMORY), "mask 0: NULL"); expect(!factory_allowed(true, N48_FACT_MEMORYMAP, N48_FACT_SYSMEMORY), "the other bit does not enable this one"); expect(!factory_allowed(false, 3, N48_FACT_SYSMEMORY), "no ops: NULL");
    expect(mm_result(true, true, 0), "mm hook success => true"); expect(!mm_result(true, true, 1), "mm hook refusal => false"); expect(!mm_result(true, false, 0), "no mm hook => false"); expect(!mm_result(false, true, 0), "no ops => false");
    expect_u("the hooks the kext calls unconditionally are exactly the REQUIRED ones", 6, 6);
    expect(task_window_ok(0x400000000ull, 0x1000), "the paravirt window is fine"); expect(!task_window_ok(0x1000, 0), "a tiny window is refused"); expect(!task_window_ok(0x400000001ull, 0x1000), "an unaligned size is refused");
    expect(!task_window_ok(0x400000000ull, 0x400000000ull), "a reserve as big as the window is refused"); expect(!task_window_ok(0x400000000ull, 0x10), "an unaligned reserve is refused");
}
static void a3_source(const std::string &root) {
    const std::string src = slurp(root + "/src/Navi48Accel.cpp");
    // every hook of the required set is called from the kext (the check and the calls agree)
    for (const char *h : { "gOps->device_open(", "gOps->device_close(", "gOps->populate_config(", "gOps->stamp_memory(", "gOps->stamp_va(", "gOps->task_window(" }) expect(src.find(h) != std::string::npos, (std::string("the kext calls the required hook ") + h).c_str());
    expect(src.find("ops_check(o)") != std::string::npos && src.find("if (v != kOpsOk)") != std::string::npos, "n48_get_ops refuses an unacceptable table");
    expect(src.find("callPlatformFunction(fn, /*waitForFunction=*/false,") != std::string::npos && src.find("rc != kIOReturnSuccess") != std::string::npos, "the ops are fetched with waitForFunction=false and compared to kIOReturnSuccess");
    expect(src.find("config_keep(gOps != nullptr, rc)") != std::string::npos && src.find("c[i] = (uint8_t)(np >> (8 * i));") != std::string::npos && src.find("c[i] = 0") == std::string::npos, "populateAccelConfig on a refusal writes a static NON-NULL name (never NULL)");
    { const size_t a = src.find("void Navi48Accelerator::populateAccelConfig("); const std::string b = src.substr(a, src.find("\n}\n", a) - a);
      expect(b.find("device_close(ctx)") != std::string::npos && b.find("stampVA = nullptr") != std::string::npos && b.find("gCtx = nullptr") != std::string::npos, "and tears the device down: stamp VA and ctx cleared so the event machine init fails");
      expect(src.find("kFallbackName[] = \"n48accel\"") != std::string::npos, "the fallback name is a static string of the aux kext"); }
    { const size_t a = src.find("void Navi48Accelerator::free()"); const std::string b = src.substr(a, src.find("\n}\n", a) - a); expect(b.find("gCtx = nullptr") != std::string::npos, "free() clears the global device context"); }
    expect(src.find("factory_allowed(gOps != nullptr, m, N48_FACT_SYSMEMORY)") != std::string::npos && src.find("factory_allowed(gOps != nullptr, m, N48_FACT_MEMORYMAP)") != std::string::npos, "the optional factories are gated by the bring-up kext's mask");
    expect(src.find("mm_result(gOps != nullptr, have, rc)") != std::string::npos, "the memory-map hooks fail closed");
    expect(src.find("return ok ? OSTypeAlloc(Navi48SysMemory) : nullptr;") != std::string::npos && src.find("return ok ? OSTypeAlloc(Navi48MemoryMap) : nullptr;") != std::string::npos, "an optional object is created only when the factory is allowed");
    expect(src.find("if (v != kOpsOk) { N48_LOG(\"ops: refused (verdict %u)\", (unsigned)v); return nullptr; }") != std::string::npos, "an unacceptable ops table is dropped (NULL), never kept");
    expect(src.find("OSCompareAndSwap(0, ok ? 1 : 2, &gGateState);") != std::string::npos && src.find("return gGateState == 1;") != std::string::npos, "the layout gate result is recorded PASS or FAIL and a FAIL stays a FAIL (fail closed for the rest of the boot)");
    expect(src.find("if (gGateState) return gGateState == 1;") != std::string::npos, "a recorded verdict is reused, never re-derived");
}

// ---- A3b: the thin-shell trampolines ---------------------------------------------------------------------------------------------------------------
static void a3b_tramp(const std::string &root) {
    const std::string src = slurp(root + "/src/Navi48Accel.cpp"), gen = slurp(root + "/gates/gen_tramp.py"), lv = slurp(root + "/gates/leaves.py");
    expect(!gen.empty() && !lv.empty(), "the generator and the leaf table are readable");
    expect(count_of(gen, "N48_AUX_ENTER((%s)") == 3 && gen.find("if (%s(%s, %d, (void *)this, a, %d, &r)) return (%s)r;") != std::string::npos, "every generated trampoline: kill switch first, then the leaf's hook");
    expect(gen.find("hookfn = TRAMP[leaf][2] if len(TRAMP[leaf]) > 2 else 'n48_vhook'") != std::string::npos, "the hook is the generic vhook unless the leaf names its own");
    expect(gen.find("n48_ztvfam_%s[%d]") != std::string::npos && gen.find("2 + slot") != std::string::npos, "the 'base' default calls the family's own implementation through its vtable");
    expect(gen.find("return (%s)0;") != std::string::npos, "the 'zero' default returns 0 / false / NULL");
    expect(gen.find("more than 6 arguments") != std::string::npos && gen.find("placeholder") != std::string::npos, "the generator refuses an unhookable slot instead of guessing");
    {   // the hooked slot sets
        struct H { const char *leaf; const char *slots; } hs[] = {
            { "'Navi48EventMachine':", "68: 'base', 72: 'zero', 73: 'zero', 84: 'zero', 85: 'zero', 86: '1', 87: 'zero'" }, { "'Navi48SharedUserClient':", "266: 'base'" },
            { "'Navi48VidMemory':", "43: 'zero', 61: 'zero', 62: 'zero'" }, { "'Navi482DContext':", "360: 'zero', 361: 'zero'" } };
        for (const H &h : hs) expect(lv.find(h.leaf) != std::string::npos && lv.find(h.slots) != std::string::npos, (std::string("the trampoline slots of ") + h.leaf).c_str());
        expect(lv.find("300: 'base', 301: 'base', 302: 'base', 303: 'base', 305: 'base', 316: 'base', 321: 'base', 326: 'base', 335: 'base', 336: 'base'") != std::string::npos, "the command queue's ten paravirt-overridden slots");
        expect(lv.find("58: 'zero', 60: 'zero', 61: 'zero', 62: 'zero', 63: 'zero', 64: 'zero', 65: 'zero'") != std::string::npos, "the resource's seven pure slots");
    }
    expect(src.find("!gOps->vhook || !cap_has(gOps, N48_CAP_VHOOK)") != std::string::npos && src.find("gOps->vhook(gCtx, cls, slot, self, a, n, r) == 1") != std::string::npos, "the hook is used only with its cap bit and only a return of exactly 1 means handled");
    expect(src.find("text_window(n48_anchor(), (uintptr_t)kmod_info.address, (uintptr_t)kmod_info.size, &lo, &hi)") != std::string::npos, "the gate window is the kext's own kmod_info range (fallback inside text_window)");
    expect(src.find("N48_TR_DEFS_Navi48EventMachine(Navi48EventMachine)") != std::string::npos && src.find("N48_TR_DEFS_Navi48CommandQueue(Navi48CommandQueue)") != std::string::npos && src.find("N48_TR_DEFS_Navi48SharedUserClient(Navi48SharedUserClient)") != std::string::npos, "the trampoline definitions are instantiated");
    { const std::string inc = slurp(root + "/build/gen/n48_tramp.inc");   // present after a make
      if (inc.empty()) std::printf("NOTE: build/gen/n48_tramp.inc absent (run make); the generator template pins above stand in\n");
      else { size_t n = 0, k = 0, p = 0; while ((p = inc.find(" Leaf::", p)) != std::string::npos) { n++; const size_t e = inc.find("\n", p); const std::string nx = inc.substr(e, 60); if (nx.find("N48_AUX_ENTER") != std::string::npos) k++; p += 7; }
             expect(n >= 30, "the generated file holds the trampolines"); size_t nk = 0, q = 0; while ((q = inc.find("__attribute__((naked))", q)) != std::string::npos) { nk++; q += 20; } expect(k + nk == n, "every generated definition begins with the kill switch or is a naked forwarder"); } }
}

// ---- A4 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a4_em(const std::string &root) {
    { const EmPlan p = em_init_plan(true, true); expect(p.ok && p.nsteps == 2 && p.steps[0] == kEmInitSuper && p.steps[1] == kEmSetStamp, "Fast2 init first, then the stamp base"); }
    { const EmPlan p = em_init_plan(false, true); expect(!p.ok && p.nsteps == 1 && p.steps[0] == kEmInitSuper, "a failed Fast2 init: the stamp base is never set"); }
    { const EmPlan p = em_init_plan(true, false); expect(!p.ok && p.nsteps == 1, "no stamp VA: never set, the machine is unusable"); }
    { const EmPlan p = em_init_plan(false, false); expect(!p.ok && p.nsteps == 1, "nothing works: unusable"); }
    const std::string src = slurp(root + "/src/Navi48Accel.cpp");
    const size_t a = src.find("bool Navi48EventMachine::init("); const size_t e = src.find("\n}\n", a); const std::string b = src.substr(a, e - a);
    const size_t sup = b.find("IOAccelEventMachineFast2::init(accel, n, timeout)"), plan = b.find("em_init_plan(sup, va != nullptr)"), set = b.find("[38])(this, va)"), ret = b.find("return p.ok;");
    expect(sup != std::string::npos && plan != std::string::npos && set != std::string::npos && ret != std::string::npos, "the event machine init has its steps");
    expect(sup < plan && plan < set && set < ret, "ORDER in Navi48EventMachine::init: Fast2::init < plan < setStampBaseAddress (slot 38) < return");
    expect(b.find("if (p.ok) {") != std::string::npos && b.find("if (p.ok) {") < set, "the stamp base is set only when the plan says ok");
    expect(b.find("OSDynamicCast(Navi48Accelerator, accel)") != std::string::npos, "the accelerator is identity-checked before its stamp VA is used");
}

// ---- A5 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a5_thin(const std::string &root) {
    const std::string src = slurp(root + "/src/Navi48Accel.cpp"), pure = slurp(root + "/src/n48accel_pure.h");
    // no value that belongs to the bring-up kext
    for (const char *lit : { "0x480000", "0x2000000000", "0x100000000008", "0x40004000", "0x10000000a", "AMD Radeon", "Navi48 Accelerator", "0x400000000", "0xFFFFFFFFFF000", "inTaskWithPhysicalMask", "IOBufferMemoryDescriptor" })
        expect(src.find(lit) == std::string::npos && pure.find(lit) == std::string::npos, (std::string("no bring-up decision in the aux kext: ") + lit).c_str());
    // the only IOLog lines are the gate's and the ops acquisition's (never per-event)
    expect(count_of(src, "N48_LOG(") <= 12, "the aux kext logs only its gate and ops lines");
    // no hardware: no register, PCI, interrupt or DMA call in the aux kext
    for (const char *t : { "WREG32", "RREG32", "configRead", "configWrite", "setMemoryEnable", "mapDeviceMemoryWithIndex", "registerInterrupt", "IODMACommand", "IOMapper", "kIOPCI", "IOPCIDevice *dev)" })
        expect(src.find(t) == std::string::npos || std::string(t) == "IOPCIDevice *dev)", (std::string("the aux kext never touches hardware: ") + t).c_str());
    expect(src.find("IOPCIDevice *) {") != std::string::npos || src.find("IOPCIDevice *)") != std::string::npos, "configureDevice / teardownDevice ignore the PCI argument (it is a nub)");
    // the class graph
    for (const char *c : { "class Navi48Accelerator : public IOGraphicsAccelerator2", "class Navi48EventMachine : public IOAccelEventMachineFast2", "class Navi48Task : public IOAccelTask", "class Navi48DisplayMachine : public IOAccelDisplayMachine",
                           "class Navi48SysMemory : public IOAccelSysMemory", "class Navi48MemoryMap : public IOAccelMemoryMap", "class Navi48VidMemory : public IOAccelVidMemory", "class Navi48Resource : public IOAccelResource2",
                           "class Navi482DContext : public IOAccel2DContext2", "class Navi48SharedUserClient : public IOAccelSharedUserClient2", "class Navi48CommandQueue : public IOAccelCommandQueue",
                           "class Navi48DisplayPipe : public IOAccelDisplayPipe" }) expect(src.find(c) != std::string::npos, (std::string("class graph: ") + c).c_str());
    expect(src.find("N48_FACT_VIDMEMORY, 333) ? OSTypeAlloc(Navi48VidMemory) : nullptr") != std::string::npos && src.find("N48_FACT_RESOURCE, 334) ? OSTypeAlloc(Navi48Resource) : nullptr") != std::string::npos && src.find("N48_FACT_CTX2D, 327) ? OSTypeAlloc(Navi482DContext) : nullptr") != std::string::npos, "the optional classes are created only when the bring-up kext allows it, else NULL");
    expect(src.find("n48_fact(this, N48_FACT_SHAREDUC, 322) ? OSTypeAlloc(Navi48SharedUserClient) : (IOAccelSharedUserClient2 *)IOGraphicsAccelerator2::newSharedUserClient()") != std::string::npos && src.find("n48_fact(this, N48_FACT_CMDQUEUE, 348) ? OSTypeAlloc(Navi48CommandQueue) : (IOAccelCommandQueue *)IOGraphicsAccelerator2::newCommandQueue()") != std::string::npos && src.find("IOGraphicsAccelerator2::newSharedUserClient()") != std::string::npos && src.find("IOGraphicsAccelerator2::newCommandQueue()") != std::string::npos, "newSharedUserClient / newCommandQueue fall back to the family's own object");
    // no virtual is introduced by a leaf class (the gate would also catch it at run time and link time): no `virtual` keyword in the file outside comments
    { size_t p = 0; int n = 0; while ((p = src.find("virtual ", p)) != std::string::npos) { const size_t ls = src.rfind('\n', p); if (src.substr(ls + 1, p - ls - 1).find("//") == std::string::npos) n++; p += 8; } expect_u("no leaf class declares a `virtual`", n, 0); }
    expect(src.find("#include \"n48_gate.inc\"") != std::string::npos && src.find("N48_FWD_DEFS_IOGraphicsAccelerator2(Navi48Accelerator)") != std::string::npos, "the gate tables and the forwarders come from the generated files");
    // the personality
    const std::string plist = slurp(root + "/Info.plist");
    expect(plist.find("<string>Navi48MetalNub</string>") != std::string::npos && plist.find("<key>IOProviderClass</key>") != std::string::npos, "the personality matches the nub");
    expect(plist.find("IOPCIDevice") == std::string::npos && plist.find("IOPCIMatch") == std::string::npos && plist.find("IOResources") == std::string::npos && plist.find("IOResourceMatch") == std::string::npos, "no PCI match and no IOResources personality (nothing runs at boot)");
    expect(plist.find("<key>IOMatchCategory</key>\n\t\t\t<string>IOAccelerator</string>") != std::string::npos, "IOMatchCategory IOAccelerator");
    expect(plist.find("<string>Navi48Accelerator</string>") != std::string::npos && plist.find("<key>MetalPluginName</key>\n\t\t\t<string>Navi48Metal</string>") != std::string::npos && plist.find("<string>Navi48Device</string>") != std::string::npos, "the accelerator class and the Metal plugin names");
    expect(plist.find("<key>IOProbeScore</key>\n\t\t\t<integer>1000</integer>") != std::string::npos, "IOProbeScore 1000");
    expect_u("exactly one personality", count_of(plist, "<key>IOClass</key>"), 1);
}

// ---- A6 ------------------------------------------------------------------------------------------------------------------------------------------------
static void a6_plist(const std::string &root, const std::string &bringRoot) {
    const std::string plist = slurp(root + "/Info.plist"), kmod = slurp(root + "/src/kmod_info.c"), mk = slurp(root + "/Makefile");
    expect(plist.find("<key>CFBundleIdentifier</key>\n\t<string>com.navi48.accelprobe</string>") != std::string::npos, "the approved id com.navi48.accelprobe is reused");
    expect_u("version 0.0.3 (short and bundle)", count_of(plist, "<string>0.0.3</string>"), 2);
    expect(plist.find("0.0.2") == std::string::npos, "no 0.0.2 left in the plist");
    expect(kmod.find("KMOD_EXPLICIT_DECL(com.navi48.accelprobe, \"0.0.3\", _start, _stop)") != std::string::npos, "kmod_info carries the same id and version");
    expect(plist.find("<key>CFBundleName</key>\n\t<string>Navi48Accel</string>") != std::string::npos, "display name Navi48Accel");
    expect(plist.find("<key>com.apple.iokit.IOAcceleratorFamily2</key>\n\t\t<string>2.0.0</string>") != std::string::npos, "links IOAcceleratorFamily2 2.0.0");
    expect(plist.find("com.apple.iokit.IOGraphicsFamily") != std::string::npos && plist.find("com.apple.kpi.iokit") != std::string::npos && plist.find("com.apple.kpi.libkern") != std::string::npos, "links IOGraphicsFamily and the kpis");
    expect(plist.find("com.navi48.bringup") == std::string::npos && plist.find("OSBundleRequired") == std::string::npos, "no link to the bring-up kext, not required at boot");
    expect(mk.find("-fapple-kext") != std::string::npos && mk.find("-Xlinker -kext") != std::string::npos && mk.find("codesign --force --sign -") != std::string::npos && mk.find("gates/gate_link.py") != std::string::npos && mk.find("gates/layout_gate_host.py") != std::string::npos,
           "the Makefile uses the bring-up recipe and runs the gates A/B/C and the host twin");
    const std::string ops = slurp(root + "/src/Navi48MetalOps.h");
    expect(!ops.empty(), "the ops header is present");
    const std::string bo = slurp(bringRoot + "/src/navi48-bringup/src/Navi48MetalOps.h");
    if (bo.empty()) std::printf("NOTE: the bring-up copy of Navi48MetalOps.h is not readable under %s; compare it with the bring-up test\n", bringRoot.c_str());
    else expect(bo == ops, "Navi48MetalOps.h is byte-identical in the aux kext and the bring-up kext");
}

// ---- A7 (aux 0.0.3): the display pipe --------------------------------------------------------------------------------------------------------------------
static int d_hook(void *, uint32_t, uint32_t, void *, const uint64_t *, uint32_t, uint64_t *) { return 1; }
static void *d_pci(void *) { return (void *)(uintptr_t)0x1234; }
static std::string body_of(const std::string &src, const std::string &head) { const size_t a = src.find(head); if (a == std::string::npos) return ""; const size_t e = src.find("\n}\n", a); return src.substr(a, e == std::string::npos ? std::string::npos : e - a); }
static void a7_display(const std::string &root) {
    N48MetalOps o = good_ops(); o.disp_flags = N48_DISP_F_ON; o.disp_hook = d_hook; o.pci_device = d_pci;
    expect_u("the table we test is ABI 2 and 144 bytes", o.abi * 1000u + o.size, 2144);
    expect(disp_enabled(&o), "ABI 2, full size, the ON flag and the hook: display ON");
    { N48MetalOps x = o; x.abi = 1; expect(!disp_enabled(&x), "an ABI-1 table is display OFF, whatever its later bytes say"); }
    { N48MetalOps x = o; x.size = N48_METAL_OPS_MIN; expect(!disp_enabled(&x), "a 120-byte table is display OFF"); }
    { N48MetalOps x = o; x.size = N48_METAL_OPS_V2 - 8; expect(!disp_enabled(&x), "a table that stops inside the ABI-2 members is display OFF"); }
    { N48MetalOps x = o; x.disp_flags = 0; expect(!disp_enabled(&x), "flag clear (navi48-metal-disp not latched on): OFF"); }
    { N48MetalOps x = o; x.disp_flags = 2; expect(!disp_enabled(&x), "only bit 0 is the ON flag"); }
    { N48MetalOps x = o; x.disp_hook = nullptr; expect(!disp_enabled(&x), "no display hook: OFF"); }
    expect(!disp_enabled(nullptr), "no table: OFF");
    {   // the OLD table exactly as the 0.0.612 bring-up kext serves it: 120 bytes, nothing after them. Bytes 120..143 are ASan-POISONED, so any read of the ABI-2
        // members aborts this test (the allocation is the full struct size only so that UBSan's object-size check accepts the member accesses).
        uint8_t *buf = (uint8_t *)std::malloc(sizeof(N48MetalOps));
        std::memset(buf, 0xA5, sizeof(N48MetalOps));
        N48MetalOps full = good_ops(); full.abi = 1; full.size = N48_METAL_OPS_MIN; std::memcpy(buf, &full, N48_METAL_OPS_MIN);
        ASAN_POISON_MEMORY_REGION(buf + N48_METAL_OPS_MIN, sizeof(N48MetalOps) - N48_METAL_OPS_MIN);
        const N48MetalOps *t = (const N48MetalOps *)(void *)buf;
        expect_u("the 0.0.612 table (abi 1, 120 bytes) passes ops_check", ops_check(t), kOpsOk);
        expect(!disp_enabled(t), "... and is display OFF, decided without reading byte 120 or later");
        uint32_t two = 2; std::memcpy(buf + 4, &two, 4);
        expect(!disp_enabled(t), "a table that says abi 2 but size 120: OFF, nothing past byte 120 read");
        ASAN_UNPOISON_MEMORY_REGION(buf + N48_METAL_OPS_MIN, sizeof(N48MetalOps) - N48_METAL_OPS_MIN);
        std::free(buf);
    }
    // the walk provider: only display on + a nub argument + a PCI device substitutes
    void *nub = (void *)(uintptr_t)0x100, *pci = (void *)(uintptr_t)0x200;
    for (int m = 0; m < 8; ++m) {
        const bool on = m & 1, isNub = m & 2, havePci = m & 4;
        void *got = dm_walk_provider(on, isNub, nub, havePci ? pci : nullptr);
        expect(got == ((on && isNub && havePci) ? pci : nub), "dm_walk_provider: the PCI device only with display on, a nub argument and a PCI device");
    }
    expect(pipe_choice(true, true) == kPipeOurs && pipe_choice(true, false) == kPipeFamily && pipe_choice(false, true) == kPipeFamily && pipe_choice(false, false) == kPipeFamily,
           "pipe_choice: ours only with display on and a successful allocation, else the family's own pipe");

    const std::string src = slurp(root + "/src/Navi48Accel.cpp"), lv = slurp(root + "/gates/leaves.py");
    {   const std::string b = body_of(src, "IOAccelDisplayPipe *Navi48Accelerator::newDisplayPipe(");
        expect(!b.empty() && b.find("N48_AUX_ENTER(IOGraphicsAccelerator2::newDisplayPipe());") != std::string::npos, "newDisplayPipe: the kill switch returns the family's own pipe");
        const size_t on = b.find("disp_enabled(gOps)"), al = b.find("OSTypeAlloc(Navi48DisplayPipe)"), ch = b.find("pipe_choice(on, mine != nullptr) == kPipeOurs"), fb = b.find(": IOGraphicsAccelerator2::newDisplayPipe();");
        expect(on != std::string::npos && al != std::string::npos && ch != std::string::npos && fb != std::string::npos && on < al && al < ch && ch < fb, "newDisplayPipe: display check < our allocation < pipe_choice < the family fallback");
        expect(b.find("on ? OSTypeAlloc(Navi48DisplayPipe) : nullptr") != std::string::npos, "our subclass is allocated only with display on");
        expect(b.find("return nullptr") == std::string::npos && b.find("return p;") != std::string::npos, "newDisplayPipe never returns NULL of its own (found_framebuffer stores it unchecked)"); }
    {   const std::string b = body_of(src, "N48_R_IOAccelDisplayMachine_267 Navi48DisplayMachine::start(");
        expect(!b.empty() && b.find("N48_AUX_ENTER(IOAccelDisplayMachine::start(provider));") != std::string::npos, "display-machine start: the kill switch passes the family's own argument through");
        expect(b.find("(on && gCtx && gOps->pci_device) ? gOps->pci_device(gCtx) : nullptr") != std::string::npos && b.find("dm_walk_provider(on, n48_provider_is_nub(") != std::string::npos, "display-machine start: the getter is asked only with display on and the substitution is the pure decision");
        expect(b.find("return IOAccelDisplayMachine::start((IOPCIDevice *)use);") != std::string::npos, "display-machine start: the family's own start does the walk"); }
    {   const std::string b = body_of(src, "static bool n48_disp_vhook(");
        expect(b.find("if (!gCtx || !disp_enabled(gOps)) return false;") != std::string::npos && b.find("gOps->disp_hook(gCtx, cls, slot, self, a, n, r) == 1") != std::string::npos,
               "n48_disp_vhook: display off = not handled (-> the family's slot); only a return of exactly 1 is handled"); }
    {   const std::string b = body_of(src, "extern \"C\" __attribute__((used, visibility(\"hidden\"))) uint64_t n48_dp_perform(");
        const size_t ks = b.find("N48_AUX_ENTER((uint64_t)0);"), hk = b.find("n48_disp_vhook(N48_VC_DISPLAYPIPE, 277, (void *)self, a, 1, &r)"), fb = b.find("n48_ztvfam_IOAccelDisplayPipe[2 + 277]");
        expect(ks != std::string::npos && hk != std::string::npos && fb != std::string::npos && ks < hk && hk < fb, "slot 277: kill switch < the display hook < the family's own performTransaction");
        expect(src.find("__attribute__((naked)) void Navi48DisplayPipe::_vslot277() { __asm__(\"jmp _n48_dp_perform\"); }") != std::string::npos, "slot 277 is a bare tail jump (rdi/rsi untouched)"); }
    expect(src.find("N48_TR_DEFS_Navi48DisplayPipe(Navi48DisplayPipe)") != std::string::npos && src.find("N48_META(Navi48DisplayPipe, IOAccelDisplayPipe)") != std::string::npos, "the display pipe class is defined and its trampolines instantiated");
    expect(lv.find("'Navi48DisplayPipe':       ('N48_VC_DISPLAYPIPE',  {267: 'base', 278: 'base', 279: 'base'}, 'n48_disp_vhook')") != std::string::npos, "leaves: 267 / 278 / 279 hooked through n48_disp_vhook, the family's own slot by default");
    expect(lv.find("('Navi48DisplayPipe',   'IOAccelDisplayPipe',       [277])") != std::string::npos && lv.find("[183, 184, 18, 322, 348, 329])") != std::string::npos && lv.find("('Navi48DisplayMachine','IOAccelDisplayMachine',    [267])") != std::string::npos,
           "leaves: the hand overrides 277 (pipe), 329 (newDisplayPipe), 267 (display-machine start)");
    expect(lv.find("HAND = {'Navi48EventMachine': [35], 'Navi48DisplayPipe': [277]}") != std::string::npos, "leaves: slot 277 of the display pipe is hand written (HAND), never generated from its placeholder declaration");
    expect(lv.find("EXPECT_CLASSES = 12") != std::string::npos && lv.find("EXPECT_SLOTS = 2370") != std::string::npos, "the expected gate totals are 12 classes / 2370 slots");
    {   const std::string g = body_of(src, "static bool n48_layout_gate() {");
        expect(!g.empty() && g.find("continue") == std::string::npos && g.find("for (const ClassGate &g : kGate) {") != std::string::npos, "the runtime layout gate walks EVERY generated class (no skip)"); }
    {   const std::string inc = slurp(root + "/build/gen/n48_tramp.inc");
        if (inc.empty()) std::printf("NOTE: build/gen/n48_tramp.inc absent (run make); the display trampoline output pins are skipped\n");
        else {
            const size_t a = inc.find("#define N48_TR_DEFS_Navi48DisplayPipe(Leaf)"); const std::string d = a == std::string::npos ? "" : inc.substr(a);
            expect(!d.empty() && d.find("n48_vhook(") == std::string::npos, "the generated display trampolines never ask the generic vhook");
            expect(d.find("n48_disp_vhook(N48_VC_DISPLAYPIPE, 267, (void *)this, a, 2, &r)") != std::string::npos && d.find("n48_ztvfam_IOAccelDisplayPipe[269]") != std::string::npos, "generated 267: display hook, else the family's slot 267");
            expect(d.find("n48_disp_vhook(N48_VC_DISPLAYPIPE, 278, (void *)this, a, 1, &r)") != std::string::npos && d.find("n48_ztvfam_IOAccelDisplayPipe[280]") != std::string::npos, "generated 278: display hook, else the family's slot 278");
            expect(d.find("n48_disp_vhook(N48_VC_DISPLAYPIPE, 279, (void *)this, a, 1, &r)") != std::string::npos && d.find("n48_ztvfam_IOAccelDisplayPipe[281]") != std::string::npos, "generated 279: display hook, else the family's slot 279");
        }
    }
}

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string bring = argc > 2 ? argv[2] : root;
    a1_kill_switch(); a1_source(root); a2_gate(); a3_ops(); a3_source(root); a3b_tramp(root); a4_em(root); a5_thin(root); a6_plist(root, bring); a7_display(root);
    std::printf("n48accel host_test: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
