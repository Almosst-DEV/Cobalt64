// native_agdc_test.cpp - build 0.0.614 (#11 step 11h.3, notes/design/NATIVE-S4-M11H.md section 2 row 2 and section 3.5): the NATIVE AGDC service (`pipeagdc`, accel action 88).
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src -I src/navi48-bringup/src/amd \
//       src/navi48-bringup/tests/native_agdc_test.cpp -o /tmp/native_agdc && /tmp/native_agdc .
//   (run from the tree root; the argument is that root, for the source pins. tests/native_agdc_plant.sh plants breaks in the real headers and glue and demands a failure.)
// What it covers:
//   P  the pure decisions of amd/native_agdc_pure.h: the two static anchors pinned to the LITERAL numbers of the extracted kexts' symbols, the slide rule (two live metaclasses must agree,
//      above their statics, page aligned, both kernel pointers), the framebuffer name allowlist, the statuses, the verb admission (action 88 opens only with the display latch ON);
//   O  ORDERING on the real flow (amd/native_agdc_flow.h) over a fake environment: OFF / already published / row-120 hold are refused before ANY lookup, the two metaclass lookups come
//      before the slide, the class checks (which read memory at slide + a static address) only after two independent anchors agree, the framebuffer / PCI / provider before the wrangler,
//      the wrangler before the build, and the build is reached by exactly one path;
//   R  REACHABILITY: a fully healthy environment does reach the build, exactly once, with the metaclass and the slide the decisions produced; every single fault (one at a time) refuses with
//      its own status and never reaches the build; the state read (argument 0) never builds and never looks anything up;
//   S  source pins on the kernel glue (DisplayPipeGuard.cpp's AgdcNativeEnv and the shared class checks / build, Navi48Bringup.cpp's action-88 branch, the dcn accessor, the CLI);
//   F  the project rule: the three forbidden pipe-offset spellings appear in no NEW file of this task.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "amd/native_agdc_flow.h"
#include "amd/native_disp_pure.h"

using namespace n48agdc;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// the fake environment: every primitive logs its name; every answer is a field the test sets
// ------------------------------------------------------------------------------------------------------------------------------------------------------
constexpr uint64_t SLIDE = 0xffffff8010e00000ull;         // a plausible System KC slide (page aligned)
constexpr uint64_t AMC = SLIDE + kAgdcMeta, PMC = SLIDE + kPipeMeta;

struct FakeEnv {
    std::vector<std::string> calls;
    bool latch = true, pub = false, held = false, targets = true, wrangl = true;
    uint64_t amc = AMC, pmc = PMC;
    uint32_t checks = 0, buildRc = 0;
    uint64_t checkedMc = 0, checkedSlide = 0, builtMc = 0, builtSlide = 0;
    unsigned builds = 0;
    bool latch_on() { calls.push_back("latch"); return latch; }
    bool published() { calls.push_back("published"); return pub; }
    bool mode_held() { calls.push_back("held"); return held; }
    uint64_t agdc_meta() { calls.push_back("agdc_meta"); return amc; }
    uint64_t pipe_meta() { calls.push_back("pipe_meta"); return pmc; }
    uint32_t class_checks(uint64_t m, uint64_t s) { calls.push_back("class_checks"); checkedMc = m; checkedSlide = s; return checks; }
    bool have_targets() { calls.push_back("targets"); return targets; }
    bool wrangler() { calls.push_back("wrangler"); return wrangl; }
    uint32_t build(uint64_t m, uint64_t s) { calls.push_back("build"); builds++; builtMc = m; builtSlide = s; return buildRc; }
    int index(const char *n) const { for (size_t i = 0; i < calls.size(); ++i) if (calls[i] == n) return (int)i; return -1; }
    bool called(const char *n) const { return index(n) >= 0; }
    std::string joined() const { std::string r; for (auto &c : calls) { if (!r.empty()) r += ","; r += c; } return r; }
};

static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static size_t count_of(const std::string &h, const std::string &n) { size_t c = 0, p = 0; while ((p = h.find(n, p)) != std::string::npos) { c++; p += n.size(); } return c; }

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string K = root + "/src/navi48-bringup/";

    // =========================================================================================================================================================
    // P. the pure decisions
    // =========================================================================================================================================================
    // The two anchors, pinned to the LITERAL numbers of the extracted kexts' symbol tables (nm: AppleGraphicsDeviceControl gMetaClass, IOAcceleratorFamily2 IOAccelDisplayPipe::gMetaClass).
    expect_u("the AGDC metaclass static", kAgdcMeta, 0x13d418e8ull);
    expect_u("the IOAccelDisplayPipe metaclass static", kPipeMeta, 0x146036d8ull);
    expect(kActAgdc == 88u && n48disp::kActAgdc == 88u && kActAgdc == n48disp::kActAgdc && n48disp::kLastAction == 90u && n48disp::kActVbl == 89u && n48disp::kActReload == 90u, "the verb is action 88 in both headers; 90 (pipereload, 0.0.619) is the last admitted action");

    { const SlideOut s = slide_decide(AMC, PMC);
      expect(s.status == kPublished && s.slide == SLIDE, "two agreeing anchors give the slide"); }
    { const SlideOut s = slide_decide(AMC + 0x1000, PMC);            expect(s.status == kSlide && s.slide == 0ull, "anchors one page apart disagree"); }
    { const SlideOut s = slide_decide(AMC, PMC + 0x1000);            expect(s.status == kSlide, "anchors one page apart disagree (other way)"); }
    { const SlideOut s = slide_decide(AMC + 8, PMC + 8);             expect(s.status == kSlide, "agreeing but not page aligned: refused"); }
    { const SlideOut s = slide_decide(AMC + 0xfff, PMC + 0xfff);     expect(s.status == kSlide, "agreeing, 0xfff off: refused"); }
    { const SlideOut s = slide_decide(0x1000ull, PMC);               expect(s.status == kSlide, "a non-kernel AGDC metaclass is refused"); }
    { const SlideOut s = slide_decide(AMC, 0x2000ull);               expect(s.status == kSlide, "a non-kernel IOAccelDisplayPipe metaclass is refused"); }
    { const SlideOut s = slide_decide(0xffffff7000000000ull + 0x10, 0xffffff7000000000ull + 0x10);
      expect(s.status == kSlide, "the same address for both metaclasses gives two different slides: refused"); }
    // The two kernel-pointer checks are not redundant: with the slides EQUAL (mod 2^64) and page aligned, exactly one metaclass can still sit outside the kernel half, at a boundary.
    { const uint64_t sl = (kKernelHalf - kPipeMeta + 0xfffull) & ~0xfffull;         // the AGDC metaclass lands just BELOW the kernel half, the family's just above
      const SlideOut o = slide_decide(sl + kAgdcMeta, sl + kPipeMeta);
      expect(!kptr_ok(sl + kAgdcMeta) && kptr_ok(sl + kPipeMeta) && ((sl + kAgdcMeta) - kAgdcMeta) == ((sl + kPipeMeta) - kPipeMeta) && (sl & 0xfffull) == 0ull, "test setup: equal page-aligned slides, only the AGDC metaclass outside the kernel half");
      expect(o.status == kSlide && o.slide == 0ull, "agreeing slides with the AGDC metaclass outside the kernel half: refused"); }
    { const uint64_t sl = (0ull - kPipeMeta + 0xfffull) & ~0xfffull;                // the family metaclass wraps past 2^64 to 0x6d8, the AGDC one is a kernel pointer
      const SlideOut o = slide_decide(sl + kAgdcMeta, sl + kPipeMeta);
      expect(kptr_ok(sl + kAgdcMeta) && !kptr_ok(sl + kPipeMeta) && ((sl + kAgdcMeta) - kAgdcMeta) == ((sl + kPipeMeta) - kPipeMeta) && (sl & 0xfffull) == 0ull, "test setup: equal page-aligned slides, only the family metaclass outside the kernel half");
      expect(o.status == kSlide && o.slide == 0ull, "agreeing slides with the family metaclass wrapped past 2^64: refused"); }
    expect_u("the kernel-half bound is the project's (DisplayPipeGuard.cpp dpg_kptr, native_disp_pure.h)", kKernelHalf, 0xffffff7000000000ull);
    expect(kptr_ok(0xffffff7000000000ull) && !kptr_ok(0xffffff6fffffffffull) && !kptr_ok(0ull) && !kptr_ok(0x13d418e8ull), "kptr_ok: the kernel half only");
    { const SlideOut s = slide_decide(kKernelHalf + kAgdcMeta, kKernelHalf + kPipeMeta);
      expect(s.status == kPublished && s.slide == kKernelHalf, "the smallest admissible slide is the kernel-half base"); }
    { const SlideOut s = slide_decide(AMC, kKernelHalf + 0x100);
      expect(s.status == kSlide, "a second anchor whose slide differs is refused"); }
    // the slide is the AGDC anchor's (they agree, so either): never the pipe's alone
    { const SlideOut s = slide_decide(AMC + 0x200000, PMC + 0x200000); expect(s.status == kPublished && s.slide == SLIDE + 0x200000, "the slide follows both anchors"); }

    expect(fb_name_ok("RDNA4FB") && fb_name_ok("AMDRDNA4FB"), "both framebuffer names are allowed");
    expect(!fb_name_ok("") && !fb_name_ok("RDNA4FB2") && !fb_name_ok("rdna4fb") && !fb_name_ok("IOFramebuffer") && !fb_name_ok("AMDRDNA4F") && !fb_name_ok(nullptr) && !fb_name_ok("AMDRDNA4FBX"),
           "nothing else is a framebuffer");
    expect(arg_ok(0) && arg_ok(1) && !arg_ok(2) && !arg_ok(~0ull), "only 0 and 1 are arguments");
    // the status decode covers every code, and only the unused one is nameless-ish
    for (uint32_t st = 0; st < kStatusCount; ++st) expect(std::strcmp(status_name(st), "unknown") != 0, "every status has a name");
    expect(std::strcmp(status_name(kStatusCount), "unknown") == 0 && std::strcmp(status_name(0xffffffffu), "unknown") == 0, "an out-of-range status is unknown");
    expect(kPublished == 0 && kOff == 1 && kNoAgdc == 3 && kSlide == 4 && kClassSize == 5 && kCodeBytes == 6 && kVtable == 7 && kNoFbPci == 8 && kNoWrangler == 9 && kAlloc == 10 && kStartFailed == 11 &&
           kAlready == 12 && kBadArg == 13 && kHeld == 14 && kNoFamily == 15 && kStatusCount == 16, "the status numbers match the translation route's (3..13) and the CLI's decode table");
    // admission of the verb
    expect(!n48disp::action_admitted(false, 88) && n48disp::action_admitted(true, 88), "action 88 opens only with the display latch ON");
    expect(n48disp::native_exempt(true, 88, 0) && n48disp::native_exempt(true, 88, 1) && !n48disp::native_exempt(true, 88, 2) && !n48disp::native_exempt(false, 88, 0) && !n48disp::native_exempt(false, 88, 1),
           "the native exemption for action 88 is the latch plus a 0|1 argument");
    expect(!n48disp::is_pipe_verb(88), "n48disp_verb does not answer 88");

    // =========================================================================================================================================================
    // R. reachability: a healthy environment reaches the build once, with the decisions' own values
    // =========================================================================================================================================================
    { FakeEnv e; const uint32_t st = native_flow(e, 1);
      expect_u("healthy: status", st, kPublished);
      expect(e.builds == 1, "healthy: the build runs exactly once");
      expect_u("healthy: the build gets the AGDC metaclass", e.builtMc, AMC);
      expect_u("healthy: the build gets the slide the two anchors agree on", e.builtSlide, SLIDE);
      expect_u("healthy: the class checks get the same metaclass", e.checkedMc, AMC);
      expect_u("healthy: the class checks get the same slide", e.checkedSlide, SLIDE);
      expect(e.joined() == "latch,published,held,agdc_meta,pipe_meta,class_checks,targets,wrangler,build", "healthy: the exact sequence of primitives"); }
    // the state read: no primitive at all
    { FakeEnv e; const uint32_t st = native_flow(e, 0);
      expect_u("state read: status 0", st, kPublished); expect(e.calls.empty() && e.builds == 0, "state read: nothing is looked up and nothing is built"); }
    { FakeEnv e; e.latch = false; e.pub = true; e.held = true; e.amc = 0; const uint32_t st = native_flow(e, 0);
      expect_u("state read with everything wrong still answers 0 and touches nothing", st, kPublished); expect(e.calls.empty(), "state read: no primitive with a hostile environment"); }
    // a bad argument: before everything
    for (uint64_t a : { 2ull, 3ull, 0x100000000ull, ~0ull }) { FakeEnv e; const uint32_t st = native_flow(e, a);
      expect_u("bad argument: status", st, kBadArg); expect(e.calls.empty() && e.builds == 0, "bad argument: no primitive runs"); }
    // the build's own failures are passed through unchanged
    { FakeEnv e; e.buildRc = kAlloc; expect_u("build failure 10 passes through", native_flow(e, 1), kAlloc); }
    { FakeEnv e; e.buildRc = kStartFailed; expect_u("build failure 11 passes through", native_flow(e, 1), kStartFailed); }
    // a class-check failure code is passed through unchanged (5 / 6 / 7)
    for (uint32_t cc : { 5u, 6u, 7u }) { FakeEnv e; e.checks = cc;
      expect_u("class check failure passes through", native_flow(e, 1), cc);
      expect(e.builds == 0 && !e.called("targets") && !e.called("wrangler"), "after a failed class check nothing else runs"); }

    // =========================================================================================================================================================
    // O. ordering: each refusal happens BEFORE the primitives that come after it
    // =========================================================================================================================================================
    { FakeEnv e; e.latch = false; const uint32_t st = native_flow(e, 1);
      expect_u("OFF: status", st, kOff);
      expect(e.joined() == "latch", "OFF: the latch is the ONLY primitive called (nothing is read with the display boot-arg off)"); }
    { FakeEnv e; e.pub = true; const uint32_t st = native_flow(e, 1);
      expect_u("already published: status", st, kAlready);
      expect(e.joined() == "latch,published", "already published: refused before the hold check and before any lookup"); }
    { FakeEnv e; e.held = true; const uint32_t st = native_flow(e, 1);
      expect_u("held: status", st, kHeld);
      expect(e.joined() == "latch,published,held", "a row-120 hold is refused BEFORE any class lookup (the lookups take the registry)"); }
    { FakeEnv e; e.latch = false; e.pub = true; e.held = true; expect_u("OFF wins over published and held", native_flow(e, 1), kOff); }
    { FakeEnv e; e.pub = true; e.held = true; expect_u("published wins over held", native_flow(e, 1), kAlready); }
    { FakeEnv e; e.amc = 0; const uint32_t st = native_flow(e, 1);
      expect_u("no AGDC class: status", st, kNoAgdc);
      expect(!e.called("pipe_meta") && !e.called("class_checks") && e.builds == 0, "no AGDC class: nothing after the AGDC lookup runs"); }
    { FakeEnv e; e.pmc = 0; const uint32_t st = native_flow(e, 1);
      expect_u("no family class: status", st, kNoFamily);
      expect(e.index("agdc_meta") < e.index("pipe_meta") && !e.called("class_checks") && e.builds == 0, "no family class: the AGDC lookup came first, the class checks never run"); }
    { FakeEnv e; e.pmc = PMC + 0x1000; const uint32_t st = native_flow(e, 1);
      expect_u("anchors disagree: status", st, kSlide);
      expect(!e.called("class_checks") && !e.called("targets") && e.builds == 0, "DISAGREEING anchors: the class checks (which read memory at slide + a static) never run"); }
    { FakeEnv e; e.amc = AMC + 4; e.pmc = PMC + 4; const uint32_t st = native_flow(e, 1);
      expect_u("unaligned slide: status", st, kSlide); expect(!e.called("class_checks") && e.builds == 0, "an unaligned slide never reaches the class checks"); }
    { FakeEnv e; e.targets = false; const uint32_t st = native_flow(e, 1);
      expect_u("no framebuffer / PCI: status", st, kNoFbPci);
      expect(e.index("class_checks") < e.index("targets") && !e.called("wrangler") && e.builds == 0, "no framebuffer: the class checks came first, the wrangler and the build never run"); }
    { FakeEnv e; e.wrangl = false; const uint32_t st = native_flow(e, 1);
      expect_u("no wrangler: status", st, kNoWrangler);
      expect(e.index("targets") < e.index("wrangler") && e.builds == 0, "no wrangler: the targets came first and the build never runs"); }
    // the build comes last and is the only path to a publish
    { FakeEnv e; native_flow(e, 1); expect(e.index("build") == (int)e.calls.size() - 1 && e.index("wrangler") == e.index("build") - 1, "the build is the last primitive, straight after the wrangler"); }

    // every single fault (one at a time) refuses, with its own status, and never builds
    struct Fault { const char *what; void (*apply)(FakeEnv &); uint32_t want; };
    const Fault faults[] = {
        { "latch off",          [](FakeEnv &e) { e.latch = false; },             kOff },
        { "already published",  [](FakeEnv &e) { e.pub = true; },                kAlready },
        { "row-120 held",       [](FakeEnv &e) { e.held = true; },               kHeld },
        { "no AGDC class",      [](FakeEnv &e) { e.amc = 0; },                   kNoAgdc },
        { "no family class",    [](FakeEnv &e) { e.pmc = 0; },                   kNoFamily },
        { "slide mismatch",     [](FakeEnv &e) { e.pmc += 0x4000; },             kSlide },
        { "unaligned slide",    [](FakeEnv &e) { e.amc += 16; e.pmc += 16; },    kSlide },
        { "AGDC not kernel",    [](FakeEnv &e) { e.amc = 0x7000; },              kSlide },
        { "class size",         [](FakeEnv &e) { e.checks = kClassSize; },       kClassSize },
        { "code bytes",         [](FakeEnv &e) { e.checks = kCodeBytes; },       kCodeBytes },
        { "vtable slots",       [](FakeEnv &e) { e.checks = kVtable; },          kVtable },
        { "no targets",         [](FakeEnv &e) { e.targets = false; },           kNoFbPci },
        { "no wrangler",        [](FakeEnv &e) { e.wrangl = false; },            kNoWrangler },
    };
    for (const Fault &f : faults) { FakeEnv e; f.apply(e); const uint32_t st = native_flow(e, 1);
        std::string w = std::string("fault '") + f.what + "': status"; expect_u(w.c_str(), st, f.want);
        w = std::string("fault '") + f.what + "': the build is never reached"; expect(e.builds == 0 && !e.called("build"), w.c_str()); }
    // and every PAIR of faults still refuses (no pair opens the build)
    for (size_t i = 0; i < sizeof faults / sizeof faults[0]; ++i) for (size_t j = i + 1; j < sizeof faults / sizeof faults[0]; ++j) {
        FakeEnv e; faults[i].apply(e); faults[j].apply(e); const uint32_t st = native_flow(e, 1);
        expect(st != kPublished && e.builds == 0, "no pair of faults reaches the build"); }

    // =========================================================================================================================================================
    // S. source pins on the kernel glue
    // =========================================================================================================================================================
    const std::string dpg = slurp(K + "src/apple/DisplayPipeGuard.cpp"), brg = slurp(K + "src/Navi48Bringup.cpp"), dcn = slurp(K + "src/dcn/navi48_dcn.cpp"), dcnh = slurp(K + "src/dcn/navi48_dcn.hpp"),
                      ttl = slurp(K + "src/apple/Navi48Ttl.hpp"), cli = slurp(root + "/tools/pc/navi48test.c"), pure = slurp(K + "src/amd/native_agdc_pure.h"), flow = slurp(K + "src/amd/native_agdc_flow.h"),
                      dglue = slurp(K + "src/amd/native_disp.cpp"), ucl = slurp(K + "src/Navi48UserClient.cpp");
    expect(!dpg.empty() && !brg.empty() && !dcn.empty() && !cli.empty() && !pure.empty() && !flow.empty() && !dglue.empty(), "the source files are present (run from the tree root)");
    if (!dpg.empty()) {
        const size_t envA = dpg.find("struct AgdcNativeEnv {"), envB = dpg.find("uint32_t navi48_agdc_native_control(");
        expect(envA != std::string::npos && envB != std::string::npos && envA < envB, "the native env precedes the native control");
        const std::string env = (envA != std::string::npos && envB != std::string::npos && envA < envB) ? dpg.substr(envA, envB - envA) : std::string();
        // the env supplies exactly the primitives the flow asks for, each from its real source
        expect(env.find("bool latch_on() { return n48disp_latched_on(); }") != std::string::npos, "latch_on is the display boot-arg latch");
        expect(env.find("bool published() { return gAg.published != 0u; }") != std::string::npos, "published reads the AGDC state shared with the translation route");
        expect(env.find("bool mode_held() { return n48dcn::modeHoldActive(); }") != std::string::npos, "mode_held is the dcn hold accessor");
        expect(env.find("meta(\"AppleGraphicsDeviceControl\")") != std::string::npos && env.find("meta(\"IOAccelDisplayPipe\")") != std::string::npos, "the two anchors are the AGDC and IOAccelDisplayPipe metaclasses");
        expect(env.find("agdc_class_checks(") != std::string::npos && env.find("agdc_have_wrangler()") != std::string::npos && env.find("agdc_build_locked(") != std::string::npos,
               "the env uses the translation route's own class checks, wrangler check and build");
        expect(env.find("n48agdc::fb_name_ok(") != std::string::npos, "the framebuffer is judged by the tested allowlist");
        expect(env.find("\"RDNA4FB\"") != std::string::npos && env.find("\"AMDRDNA4FB\"") != std::string::npos, "the framebuffer is looked up under both names");
        // what the native route must NOT depend on
        expect(env.find("navi48_pipeguard_armed_all") == std::string::npos && env.find("navi48_x6000_slide") == std::string::npos && env.find("navi48_accel_object") == std::string::npos &&
               env.find("PE_parse_boot_argn") == std::string::npos && env.find("kDpgAccelDisplayMachineOff") == std::string::npos, "the native route has no X6000 / pipe-guard / legacy boot-arg dependency");
        expect(env.find("IOLock") == std::string::npos, "the env takes no lock of its own (the control holds the AGDC lock)");
        // finish: a published object keeps the framebuffer reference, every other outcome gives it back
        expect(env.find("if (fb && st != n48agdc::kPublished) fb->release();") != std::string::npos, "a failed publish gives the framebuffer reference back; a published one keeps it");
        // the control
        const std::string ctl = envB != std::string::npos ? dpg.substr(envB, 900) : std::string();
        expect(ctl.find("n48agdc::native_flow(env, arg)") != std::string::npos && count_of(dpg, "n48agdc::native_flow(") == 1, "the control runs the tested flow, once");
        { const size_t lk = ctl.find("IOLockLock(gDpgLock);"), fl = ctl.find("native_flow(env, arg)"), ul = ctl.find("IOLockUnlock(gDpgLock);"), fin = ctl.find("env.finish(st);");
          expect(lk != std::string::npos && fl != std::string::npos && ul != std::string::npos && fin != std::string::npos && lk < fl && fl < ul && ul < fin, "the flow runs under the AGDC lock; finish runs after it is dropped"); }
        // the shared pieces: the translation route keeps its checks and uses the same class checks / wrangler / build
        const size_t tA = dpg.find("static uint32_t agdc_publish_locked() {"), tB = dpg.find("static void agdc_fill_out(");
        expect(tA != std::string::npos && tB != std::string::npos && tA < tB, "the translation route's publish is present");
        const std::string tr = (tA != std::string::npos && tB != std::string::npos && tA < tB) ? dpg.substr(tA, tB - tA) : std::string();
        expect(tr.find("PE_parse_boot_argn(\"navi48-agdc\"") != std::string::npos && tr.find("navi48_pipeguard_armed_all()") != std::string::npos && tr.find("navi48_x6000_slide(&sr)") != std::string::npos,
               "the translation route still demands its boot-arg, its armed pipe guard and X6000's slide");
        expect(tr.find("agdc_class_checks(mc, slide)") != std::string::npos && tr.find("agdc_have_wrangler()") != std::string::npos && tr.find("agdc_build_locked(mc, slide, fb, pci, provider)") != std::string::npos,
               "the translation route uses the same shared class checks, wrangler check and build");
        expect(count_of(dpg, "agdc_build_locked(") == 3 && count_of(dpg, "agdc_class_checks(") == 3 && count_of(dpg, "agdc_have_wrangler()") == 3, "each shared piece is defined once and called from exactly the two routes");
        // the static anchors: the glue's and the pure header's are the same numbers
        expect(dpg.find("kAgdcGMetaClass   = 0x13d418e8;") != std::string::npos && pure.find("kAgdcMeta = 0x13d418e8ull;") != std::string::npos, "the AGDC anchor is the same number in the glue and in the pure header");
        expect(pure.find("kPipeMeta = 0x146036d8ull;") != std::string::npos, "the family anchor literal");
        // the class checks keep all three groups of checks
        { const size_t cA = dpg.find("static uint32_t agdc_class_checks("), cB = dpg.find("static bool agdc_have_wrangler()");
          const std::string cc = (cA != std::string::npos && cB != std::string::npos && cA < cB) ? dpg.substr(cA, cB - cA) : std::string();
          expect(cc.find("getClassSize() != kAgdcClassSize") != std::string::npos && cc.find("agdc_bytes(kAgdcOpNew + slide") != std::string::npos && cc.find("agdc_bytes(kAgdcOpDelete + slide") != std::string::npos &&
                 cc.find("vt[184]") != std::string::npos && cc.find("vt[238]") != std::string::npos && cc.find("vt[267]") != std::string::npos && cc.find("vt[7]") != std::string::npos &&
                 cc.find("return 5;") != std::string::npos && cc.find("return 6;") != std::string::npos && cc.find("return 7;") != std::string::npos, "the shared class checks still check the size, the four functions' bytes and four vtable slots"); }
        // the shared build still carries the whole construction
        { const size_t bA = dpg.find("static uint32_t agdc_build_locked("), bB = dpg.find("static uint32_t agdc_publish_locked() {");
          const std::string b = (bA != std::string::npos && bB != std::string::npos && bA < bB) ? dpg.substr(bA, bB - bA) : std::string();
          expect(b.find("copy[kVtHdr + 266] = reinterpret_cast<void *>(&agdc_vendor);") != std::string::npos && b.find("kAgdcOpNew + slide") != std::string::npos &&
                 b.find("obj->start(provider)") != std::string::npos && b.find("gAg.published = 1;") != std::string::npos, "the shared build installs slot 266, allocates with Apple's operator new, starts and registers");
          expect(b.find("navi48_pipeguard_armed_all") == std::string::npos && b.find("PE_parse_boot_argn") == std::string::npos && b.find("navi48_x6000_slide") == std::string::npos, "the shared build has no route-specific dependency"); }
        expect(dpg.find("static void agdc_fill_out(uint32_t st, uint64_t *out, unsigned count) {") != std::string::npos && count_of(dpg, "agdc_fill_out(st, out, count);") == 2, "both controls return the same scalars through one filler");
    }
    // The decisions of the reply (agdc_vendor and the fillers) are unchanged by this task and shared byte for byte.
    if (!dpg.empty()) expect(dpg.find("static uint32_t agdc_vendor(void *self, uint32_t cmd,") != std::string::npos && dpg.find("copy[kVtHdr + 266] = reinterpret_cast<void *>(&agdc_vendor);") != std::string::npos, "the vendor handler is the same for both routes");
    if (!dcn.empty() && !dcnh.empty()) {
        expect(dcn.find("bool modeHoldActive() { return n48mt::ld(gMt.launching) != 0u || n48mt::ld(gMt.held) != 0u; }") != std::string::npos, "the hold accessor reads both the launch claim and the held word, by atomic loads only");
        expect(dcnh.find("bool modeHoldActive();") != std::string::npos, "the accessor is declared");
    }
    if (!ttl.empty()) expect(ttl.find("uint32_t navi48_agdc_native_control(uint64_t arg, uint64_t *out, unsigned count);") != std::string::npos, "the native control is declared");
    if (!brg.empty()) {
        expect(brg.find("if (action == 88) {") != std::string::npos && brg.find("navi48_agdc_native_control(argScalar, v, 13)") != std::string::npos, "accelExperiment has the action-88 branch");
        expect(brg.find("n48disp::verb_args_ok(action, argScalar) ? navi48_agdc_native_control(argScalar, v, 13)") != std::string::npos, "the branch checks the legal arguments with the exemption table's own function");
        expect(brg.find("n48agdc::kBadArg") != std::string::npos && brg.find("return st == n48agdc::kBadArg ? kIOReturnBadArgument : kIOReturnSuccess;") != std::string::npos, "a bad argument is a bad-argument IOReturn, every other status a scalar");
        expect(brg.find("#include \"amd/native_agdc_pure.h\"") != std::string::npos, "Bringup.cpp includes the pure header");
        // the action-88 branch sits AFTER the generic exemption / bound (it inherits both) and before fbname's
        const size_t ex = brg.find("n48disp::native_exempt(n48disp_latched_on(), action, argScalar)"), ab = brg.find("if (action == 88) {"), fb = brg.find("if (action == 78) {");
        expect(ex != std::string::npos && ab != std::string::npos && fb != std::string::npos && ex < ab && ab < fb, "the exemption comes first; the action-88 branch sits before fbname's");
        expect(brg.find("if (action == 83 || action == 84 || action == 85 || action == 86 || action == 87 || action == 89 || action == 90) {") != std::string::npos, "actions 83..87, 89 and 90 (0.0.619) go to n48disp_verb, and 88 is not among them");
    }
    if (!dglue.empty()) expect(dglue.find("!n48disp::is_pipe_verb(action)") != std::string::npos && dglue.find("!n48disp::is_new_action(action)") == std::string::npos, "n48disp_verb answers only the five pipe verbs");
    if (!ucl.empty()) expect(ucl.find("if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;") != std::string::npos, "the user client bound is the latched one (88 is admitted through it)");
    if (!cli.empty()) {
        expect(cli.find("\"pipeagdc\"") != std::string::npos && cli.find("!strcmp(what, \"pipeagdc\")) in = 88;") != std::string::npos, "navi48test has pipeagdc, action 88");
        expect(cli.find("in == 88 ? \"pipeagdc\"") != std::string::npos && cli.find("pipeagdc [0|1]") != std::string::npos && cli.find("if (in == 88) {") != std::string::npos, "navi48test names, documents and decodes it");
        expect(cli.find("\"IOAccelDisplayPipe class (the second slide anchor) is not loaded\"") != std::string::npos && cli.find("\"row-120 mode hold is up") != std::string::npos, "the CLI decode knows the two native-only statuses");
    }
    expect(flow.find("if (!e.latch_on()) return kOff;") != std::string::npos && flow.find("e.class_checks(am, s.slide)") != std::string::npos && flow.find("return e.build(am, s.slide);") != std::string::npos, "the flow's three key lines");

    // =========================================================================================================================================================
    // F. the project rule: three spellings appear in no NEW file of this task
    // =========================================================================================================================================================
    { const std::string a = std::string("pipe+") + "0x280", b = std::string("pipe+") + "0x282", c = std::string("pipe+") + "0x299";
      for (const std::string *f : { &pure, &flow }) expect(f->find(a) == std::string::npos && f->find(b) == std::string::npos && f->find(c) == std::string::npos, "no NEW AGDC header spells the three forbidden pipe offsets");
      const std::string me = slurp(K + "tests/native_agdc_test.cpp"), pl = slurp(K + "tests/native_agdc_plant.sh");
      expect(me.find(a) == std::string::npos && me.find(b) == std::string::npos && me.find(c) == std::string::npos && pl.find(a) == std::string::npos && pl.find(b) == std::string::npos && pl.find(c) == std::string::npos,
             "this test and its plant script never spell them");
      // DisplayPipeGuard.cpp is an OLD file whose historical comments carry them; this task added none: the count is its 0.0.613 count
      const size_t envA = dpg.find("struct AgdcNativeEnv {"), envB = dpg.find("uint32_t navi48_agdc_native_control(");
      if (envA != std::string::npos && envB != std::string::npos) { const std::string mine = dpg.substr(envA, envB - envA + 900);
          expect(mine.find(a) == std::string::npos && mine.find(b) == std::string::npos && mine.find(c) == std::string::npos, "the native AGDC block of DisplayPipeGuard.cpp spells none"); } }

    std::printf("native_agdc_test: %d/%d passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
