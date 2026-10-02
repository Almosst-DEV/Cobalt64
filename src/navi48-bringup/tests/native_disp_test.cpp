// native_disp_test.cpp - build 0.0.613 (#11 step 11h.2, notes/design/NATIVE-S4-M11H.md + the 11h.1 RE facts section): the display pipe.
//   clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -I src/navi48-bringup/src -I src/navi48-bringup/src/amd \
//       src/navi48-bringup/tests/native_disp_test.cpp -o /tmp/native_disp && /tmp/native_disp .
//   (run from the tree root; the argument is that root, for the source pins. tests/native_disp_plant.sh plants breaks in the real headers and glue and demands a failure.)
// What it covers:
//   P  the pure decisions of amd/native_disp_pure.h: the latch, the action bound (OPEN past 82 only when latched), the native exemptions, the ops-table shape (ABI 1 / 120 bytes when OFF),
//      the fact mask, the accelerator's positive controls, the adopt / arm verdicts, submit (never 0, passes a nonzero status through), bounds (destination rows at the CONSOLE's stride),
//      source admission, the slot-62 plan and the shortcut guard, the stand-in object, the pipe table, the duration, the stamps;
//   O  ORDERING on the real flows (amd/native_disp_flow.h) over a fake kernel: the resource facts are on before requestProbe, a verified pipe exists before arm, disarm is always
//      allowed and never depends on a probe, nothing is recorded or published before verification, an old / OFF table means display off;
//   R  REACHABILITY: the four pipe slots through the dispatcher over fake kernel memory (a real txn / IOSurface / resource / sysmem layout and a console with guard pads):
//      the frame lands row by row at the console's stride, nothing is written past the console on ANY refusal, perform always returns success;
//   S  source pins on the kernel glue (amd/native_disp.cpp and its call sites): the glue runs the flows, the exemption and the bound go through the pure functions, no ungated path;
//   F  the project rule: the three forbidden pipe-offset spellings appear in no file of the tree.
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <fstream>
#include <functional>
#include <sstream>
#include "amd/native_disp_flow.h"

using namespace n48disp;

static int gFail = 0, gRun = 0;
static void expect(bool ok, const char *what) { gRun++; if (!ok) { gFail++; std::printf("FAIL: %s\n", what); } }
static void expect_u(const char *what, uint64_t got, uint64_t want) {
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL: %s: got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// the fake kernel
// ------------------------------------------------------------------------------------------------------------------------------------------------------
struct Mem {
    struct Reg { uint64_t base; std::vector<uint8_t> b; };
    std::vector<Reg> regs;
    void add(uint64_t base, size_t size) { regs.push_back({ base, std::vector<uint8_t>(size, 0) }); }
    Reg *find(uint64_t a, uint64_t n) {
        for (auto &r : regs) if (a >= r.base && n <= r.b.size() && a - r.base <= r.b.size() - n) return &r;
        return nullptr;
    }
    bool rd(uint64_t a, void *d, size_t n) { Reg *r = find(a, n); if (!r) return false; std::memcpy(d, &r->b[a - r->base], n); return true; }
    bool wr(uint64_t a, const void *s, size_t n) { Reg *r = find(a, n); if (!r) return false; std::memcpy(&r->b[a - r->base], s, n); return true; }
    template <class T> void put(uint64_t a, T v) { bool ok = wr(a, &v, sizeof v); if (!ok) { std::printf("test bug: put outside memory %#llx\n", (unsigned long long)a); std::exit(2); } }
    template <class T> T get(uint64_t a) { T v{}; rd(a, &v, sizeof v); return v; }
};

constexpr uint64_t K = 0xffffff8000000000ull;              // kernel-half base for the fake objects
constexpr uint64_t A_ACCEL = K + 0x100000, A_DM = K + 0x200000, A_EM = K + 0x300000, A_PIPE = K + 0x400000, A_FB = K + 0x500000, A_NUB = K + 0x600000;
constexpr uint64_t A_TXN = K + 0x700000, A_ARR = K + 0x710000, A_SURF = K + 0x720000, A_RES = K + 0x730000, A_SM = K + 0x740000, A_MD = K + 0x750000, A_DC = K + 0x760000;
constexpr uint64_t A_PIPE2 = K + 0x410000;

struct FakeEnv {
    Mem mem;
    std::vector<std::string> log;
    // state
    bool latch = true;
    uint32_t factMask = 0;
    bool canAddFacts = true;
    std::vector<uint64_t> pipes;
    bool armedFlag = false;
    uint8_t armByte = 0;
    // perform
    ConsoleInfo con { 2560, 1440, 10240, 10240ull * 1440 };
    bool haveConsole = true;
    size_t pad = 8192;
    std::vector<uint8_t> consoleBuf;                  // [pad][console bytes (allocated as the real size, con.bytes may claim less)][pad]
    size_t consoleReal = 0;
    std::map<uint64_t, std::vector<uint8_t>> srcs;
    std::vector<uint8_t> scratch = std::vector<uint8_t>(kScratchBytes);
    uint32_t scratchMode = 0;                         // 0 free, 1 busy, 2 none
    uint32_t acquired = 0, released = 0;
    int srcFailAfter = -1, dstFailAfter = -1;
    int srcReads = 0, dstWrites = 0, oobWrites = 0;
    uint64_t now = 0, clockStep = 1000;
    // 0.0.615
    CrashGuard cg {};
    uint32_t autoDisarms = 0, nullPipeNotes = 0, withdrawNotes = 0; bool withdrawWasArmed = false, pciAtProbe = false;
    uint32_t reasons[kPfCount] = {};
    uint64_t bytesNoted = 0, performNotes = 0, sourceNotes = 0;
    SourceLog lastSource {};
    uint64_t reads = 0, writes = 0;
    // slot 267
    std::map<uint64_t, std::vector<uint8_t>> standins;
    bool standinNull = false;
    uint32_t initFbNotes = 0;
    // adopt / arm
    bool busy = false, accelFound = true, layoutOk = true, probeRc = true, capsOk = true, writeOk = true;
    PipeProbe before {}, after {}, current {};
    bool requested = false;
    uint32_t adoptEnd = 0xffffffffu, adoptEndCalls = 0;
    uint64_t accelAddr = A_ACCEL;
    std::map<uint64_t, std::string> classes;
    bool shortcutSwitch = true;
    Res62Plan lastPlan {}; uint32_t res62Notes = 0;
    // 0.0.616: the prepared-descriptor cache
    MdCache mdcache {};
    std::map<uint64_t, int> prepCount, complCount;
    bool prepFail = false, hungFlag = false;
    int unheldReads = 0, armedTrueCalls = -1;
    uint32_t mdNotes[9] = {};
    std::function<void()> onClock;
    // 0.0.618 (V1/V2): the vblank timestamps
    bool vblOnFlag = true, sampleOk = true; int vblSamples = 0; VblSample vs { 16680000ull, 5000000ull, 1000000000ull, 1u, 1u };
    std::map<uint64_t, std::string> txnClass;            // address -> class name; class_derives answers true for exactly that name
    uint32_t vblNotes[kVblCount] = {}; VblPlan lastPlan618 {}; uint64_t lastVblPeriod = 0, lastVblDelay = 0;

    FakeEnv() { setConsole(2560, 1440, 10240); }
    void setConsole(uint64_t w, uint64_t h, uint64_t stride) {
        con.width = w; con.height = h; con.stride = stride; con.bytes = stride * h;
        consoleReal = (size_t)con.bytes; consoleBuf.assign(pad + consoleReal + pad, 0xCD);
        std::memset(&consoleBuf[pad], 0xAA, consoleReal);
    }
    uint8_t *consoleData() { return &consoleBuf[pad]; }
    bool padsClean() const {
        for (size_t i = 0; i < pad; ++i) if (consoleBuf[i] != 0xCD || consoleBuf[pad + consoleReal + i] != 0xCD) return false;
        return true;
    }
    bool consoleUntouched() const { for (size_t i = 0; i < consoleReal; ++i) if (consoleBuf[pad + i] != 0xAA) return false; return true; }

    // ---- the concept ----
    bool latch_on() { return latch; }
    uint32_t facts() { return factMask; }
    void facts_add(uint32_t b) { log.push_back("facts_add"); if (canAddFacts) factMask |= b; }
    template <class T> bool rdT(uint64_t a, T *o) { reads++; if (!kptr_ok(a)) return false; return mem.rd(a, o, sizeof(T)); }
    template <class T> bool wrT(uint64_t a, T v) { writes++; if (!kptr_ok(a)) return false; return mem.wr(a, &v, sizeof(T)); }
    bool rd8(uint64_t a, uint8_t *o) { return rdT(a, o); }
    bool rd16(uint64_t a, uint16_t *o) { return rdT(a, o); }
    bool rd32(uint64_t a, uint32_t *o) { return rdT(a, o); }
    bool rd64(uint64_t a, uint64_t *o) { return rdT(a, o); }
    bool wr8(uint64_t a, uint8_t v) { return wrT(a, v); }
    bool wr64(uint64_t a, uint64_t v) { if (a == A_TXN + kTxnVblTime) log.push_back("wr_t"); if (a == A_TXN + kTxnVblNext) log.push_back("wr_next"); return wrT(a, v); }
    bool known_pipe(uint64_t p) { for (uint64_t q : pipes) if (q == p) return true; return false; }
    bool armed() { if (armedTrueCalls >= 0) { if (armedTrueCalls-- > 0) return true; armedFlag = false; return false; } return armedFlag; }
    bool console(ConsoleInfo *c) { if (!haveConsole) return false; *c = con; return true; }
    uint32_t scratch_acquire(uint8_t **buf, uint32_t *cap) { if (scratchMode == 1) return 1; if (scratchMode == 2) return 2; *buf = scratch.data(); *cap = (uint32_t)scratch.size(); acquired++; return 0; }
    void scratch_release() { released++; }
    bool src_read(uint64_t md, uint64_t off, uint8_t *dst, uint64_t len) {
        if (!mdc_held(mdcache, md)) { unheldReads++; return false; }              // the kernel glue's own assertion: only a cache hold may read
        if (srcFailAfter >= 0 && srcReads >= srcFailAfter) return false;
        if (srcReads == 0) log.push_back("src_read");
        srcReads++;
        auto it = srcs.find(md); if (it == srcs.end()) return false;
        if (off > it->second.size() || len > it->second.size() - off) return false;
        std::memcpy(dst, &it->second[off], len);
        return true;
    }
    bool console_write(uint64_t off, const uint8_t *src, uint64_t len) {
        if (dstFailAfter >= 0 && dstWrites >= dstFailAfter) return false;
        dstWrites++;
        if (off + len > con.bytes) oobWrites++;                                 // a write past the console the kernel reported
        if (off + len > consoleReal + pad) return false;                         // the fake's own guard: never scribble outside the fake buffer
        std::memcpy(&consoleBuf[pad + off], src, len);
        return true;
    }
    uint64_t now_ns() { now += clockStep; if (onClock) onClock(); return now; }
    MdCache &mdc() { return mdcache; }
    bool md_prepare(uint64_t md) { log.push_back("md_prepare"); if (prepFail) return false; prepCount[md]++; return true; }
    void md_unprepare(uint64_t md) { log.push_back("md_complete"); complCount[md]++; }
    bool hung() { return hungFlag; }
    bool vbl_on() { return vblOnFlag; }
    bool class_derives(uint64_t obj, const char *name) { auto it = txnClass.find(obj); return it != txnClass.end() && it->second == name; }
    bool vbl_sample(VblSample *o) { log.push_back("vbl_sample"); vblSamples++; if (!sampleOk) return false; *o = vs; return true; }
    void note_vbl(uint32_t r, const VblPlan &p, uint64_t period, uint64_t delay) { if (r < kVblCount) vblNotes[r]++; if (r == kVblWrote) { lastPlan618 = p; lastVblPeriod = period; lastVblDelay = delay; } }
    void note_md(uint32_t ev, uint64_t, uint64_t) { if (ev < 9u) mdNotes[ev]++; }
    void note_perform(uint32_t r, uint64_t bytes, uint64_t ns) { (void)ns; reasons[r]++; bytesNoted += bytes; performNotes++; }
    void note_source(const SourceLog &s) { lastSource = s; sourceNotes++; }
    uint8_t *standin(uint64_t pipe) {
        if (standinNull) return nullptr;
        auto &v = standins[pipe];
        if (v.empty()) { v.assign(kObjSize, 0x77); v[0] = 0xEF; v[1] = 0xBE; v[2] = 0xAD; v[3] = 0xDE; v[4] = 1; v[5] = 2; v[6] = 3; v[7] = 4; }   // a vptr that must survive
        return v.data();
    }
    void note_init_fb(uint64_t, uint64_t) { initFbNotes++; }
    bool adopt_begin() { log.push_back("adopt_begin"); return !busy; }
    void adopt_end(uint32_t st) { log.push_back("adopt_end"); adoptEnd = st; adoptEndCalls++; }
    bool find_accel() { log.push_back("find_accel"); return accelFound; }
    bool accel_layout_ok() { log.push_back("accel_layout_ok"); return layoutOk; }
    PipeProbe probe_pipe() { log.push_back(requested ? "probe_after" : "probe_before"); return requested ? after : (current.accelOk || current.count ? current : before); }
    bool request_probe() { log.push_back("request_probe"); requested = true; pciAtProbe = pci_admit_flow(*this); return probeRc; }
    bool publish_caps() { log.push_back("publish_caps"); return capsOk; }
    void record_adopted(const PipeProbe &) { log.push_back("record_adopted"); }
    bool arm_write(uint8_t v) { log.push_back(v ? "arm_write1" : "arm_write0"); if (!writeOk) return false; armByte = v; armedFlag = v != 0; return true; }
    void disarm() { log.push_back("disarm"); armedFlag = false; armByte = 0; }
    uint64_t accel_addr() { return accelAddr; }
    bool class_is(uint64_t obj, const char *name) { auto it = classes.find(obj); return it != classes.end() && it->second == name; }
    CrashGuard &guard() { return cg; }
    uint32_t autoCause = 0;
    bool scanOwned = false; uint32_t scanSkipSubmits = 0; Ival iv {};          // 0.0.617 (K6)
    bool scan_active() { return scanOwned; }
    void note_scan_owned_submit() { scanSkipSubmits++; }
    Ival &ivl() { return iv; }
    void note_autodisarm(uint32_t cause) { log.push_back("note_autodisarm"); autoDisarms++; autoCause = cause; }
    void clear_autodisarm() { log.push_back("clear_autodisarm"); autoCause = 0; }
    void note_null_pipe() { log.push_back("note_null_pipe"); nullPipeNotes++; }
    void forget_pipes() { log.push_back("forget_pipes"); pipes.clear(); armedFlag = false; }
    void note_withdraw(bool was) { log.push_back("note_withdraw"); withdrawNotes++; withdrawWasArmed = was; }
    void note_res62(const Res62Plan &p, bool, bool, bool, bool) { lastPlan = p; res62Notes++; }
    // 0.0.619 (R1): the operator restart window
    ReloadWin rwin {}; uint32_t rwEv[kRwCount] = {}; std::vector<uint32_t> rwSeq;
    ReloadWin &rw() { return rwin; }
    void note_reload(uint32_t ev) { if (ev < kRwCount) rwEv[ev]++; rwSeq.push_back(ev); }

    int idx(const char *s) const { for (size_t i = 0; i < log.size(); ++i) if (log[i] == s) return (int)i; return -1; }
    bool has(const char *s) const { return idx(s) >= 0; }
    int count(const char *s) const { int n = 0; for (auto &l : log) if (l == s) n++; return n; }
};

static PipeProbe good_probe() {
    PipeProbe p {}; p.accelOk = true; p.dmReadable = true; p.count = 1; p.havePipe = true; p.classOurs = true; p.traced = true;
    p.backAccel = p.backDm = p.backFb = true; p.fbOurs = true; return p;
}

// A complete world: accelerator, display machine, event machine, pipe, and one transaction carrying a 2560x1440 BGRA plane 0.
static bool gAutoPrep = true;        // World runs the submit hook once (so perform has a prepared descriptor) unless a test turns this off
struct World {
    FakeEnv e;
    uint64_t W = 2560, H = 1440, sbpr = 10240;
    std::vector<uint8_t> src;
    World(uint64_t w = 2560, uint64_t h = 1440, uint64_t stride = 10240) : W(w), H(h), sbpr(stride) {
        Mem &m = e.mem;
        m.add(A_ACCEL, kAccelSize); m.add(A_DM, 0x178); m.add(A_EM, kEmSize); m.add(A_PIPE, kPipeSize); m.add(A_PIPE2, kPipeSize); m.add(A_FB, 0x100); m.add(A_NUB, 0x100);
        m.add(A_TXN, 0x1b8); m.add(A_ARR, 0x100); m.add(A_SURF, 0x400); m.add(A_RES, 0x200); m.add(A_SM, 0x100); m.add(A_MD, 0x100); m.add(A_DC, 0x100);
        m.put<uint64_t>(A_ACCEL + kAccelProvider, A_NUB); m.put<uint64_t>(A_ACCEL + kAccelDm, A_DM); m.put<uint64_t>(A_ACCEL + kAccelEm, A_EM);
        m.put<uint32_t>(A_ACCEL + kAccelCfgF0, 0x480000); m.put<uint32_t>(A_ACCEL + kAccelCfgF4, 8); m.put<uint8_t>(A_ACCEL + kAccelPipeGate, 0);
        m.put<uint32_t>(A_DM + kDmCount, 1); m.put<uint64_t>(A_DM + kDmPipes, A_PIPE);
        m.put<uint64_t>(A_PIPE + kPipeAccel, A_ACCEL); m.put<uint64_t>(A_PIPE + kPipeDm, A_DM); m.put<uint64_t>(A_PIPE + kPipeFb, A_FB);
        e.pipes.push_back(A_PIPE);
        e.classes[A_EM] = "Navi48EventMachine";
        m.put<uint32_t>(A_EM + kEmCount, 4); m.put<uint32_t>(A_EM + kEmDone, 10); m.put<uint32_t>(A_EM + kEmSub, 10);
        // the transaction
        m.put<uint64_t>(A_TXN + kTxnPlanes, A_ARR); m.put<uint64_t>(A_TXN + kTxnDirty, kDirtyPlane0); m.put<uint32_t>(A_TXN + kTxnStatus, 0);
        m.put<uint64_t>(A_ARR + kPlaneSurf, A_SURF); m.put<uint64_t>(A_ARR + kPlaneRes, A_RES);
        m.put<uint64_t>(A_SURF + kSurfW, W); m.put<uint64_t>(A_SURF + kSurfH, H); m.put<uint64_t>(A_SURF + kSurfBpr, sbpr); m.put<uint16_t>(A_SURF + kSurfBpe, 4);
        m.put<uint8_t>(A_SURF + kSurfElemW, 1); m.put<uint64_t>(A_SURF + kSurfBase, 0); m.put<uint32_t>(A_SURF + kSurfFmt, kFmtBGRA); m.put<uint32_t>(A_SURF + kSurfPlanes, 1);
        m.put<uint16_t>(A_RES + kResW, (uint16_t)W); m.put<uint16_t>(A_RES + kResH, (uint16_t)H); m.put<uint64_t>(A_RES + kResBpr, sbpr);
        m.put<uint64_t>(A_RES + kResSysMem, A_SM); m.put<uint64_t>(A_RES + kResDc, A_DC); m.put<uint64_t>(A_DC + kDcSurf, A_SURF);
        m.put<uint64_t>(A_SM + kSmLen, sbpr * H); m.put<uint64_t>(A_SM + kSmMd, A_MD); m.put<uint8_t>(A_SM + kSmFlags, 0);
        src.resize((size_t)(sbpr * H));
        for (size_t i = 0; i < src.size(); ++i) src[i] = (uint8_t)((i * 131u + (i >> 8) * 7u) & 0xff);
        e.srcs[A_MD] = src;
        e.setConsole(W, H, stride);
        e.armedFlag = true; e.factMask = kNeedFacts;
        if (gAutoPrep) { submit(); e.reads = e.writes = 0; e.log.clear(); }
    }
    int submit(uint64_t *ret = nullptr) { uint64_t a[1] = { A_TXN }, r = 0; const int h = hook_dispatch(e, N48_VC_DISPLAYPIPE, 279, A_PIPE, a, 1, ret ? ret : &r); return h; }
    // run the dispatcher for slot 277 and return (handled, ret)
    int perform(uint64_t *ret) { uint64_t a[1] = { A_TXN }; return hook_dispatch(e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 1, ret); }
    const uint8_t *srcRow(uint64_t y) const { return &src[(size_t)(y * sbpr)]; }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------
static std::string slurp(const std::string &p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }
static size_t count_of(const std::string &h, const std::string &n) { size_t c = 0, p = 0; while ((p = h.find(n, p)) != std::string::npos) { c++; p += n.size(); } return c; }

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string K = root + "/src/navi48-bringup/";

    // =========================================================================================================================================================
    // P. pure decisions
    // =========================================================================================================================================================
    // the latch: present and exactly 1 is ON; absent, 0, 2, anything else OFF
    expect_u("latch: absent -> off", latch_value(false, 1), kLatchOff);
    expect_u("latch: =1 -> on", latch_value(true, 1), kLatchOn);
    expect_u("latch: =0 -> off", latch_value(true, 0), kLatchOff);
    expect_u("latch: =2 -> off", latch_value(true, 2), kLatchOff);
    expect(latch_is_on(kLatchOn) && !latch_is_on(kLatchOff) && !latch_is_on(kLatchUnset), "latch_is_on: only the ON state");

    // the action bound: 0..82 always, 83..90 only when latched (88 = pipeagdc, 0.0.614; 89 = pipevbl, 0.0.618; 90 = pipereload, 0.0.619), 91+ never
    for (uint32_t a = 0; a <= 82; ++a) expect(action_admitted(false, a) && action_admitted(true, a), "actions 0..82 are admitted with the latch on or off");
    for (uint32_t a = 83; a <= 90; ++a) { expect(!action_admitted(false, a), "a new verb is NOT admitted with the boot-arg OFF"); expect(action_admitted(true, a), "a new verb is admitted with the boot-arg ON"); }
    expect(!action_admitted(true, 91) && !action_admitted(false, 91) && !action_admitted(true, 0xffffffffu) && kLastAction == 90 && kActAgdc == 88 && kActVbl == 89 && kActReload == 90, "no action past 90 is ever admitted");
    expect(is_pipe_verb(83) && is_pipe_verb(87) && is_pipe_verb(89) && is_pipe_verb(90) && !is_pipe_verb(82) && !is_pipe_verb(88) && !is_pipe_verb(91) && is_new_action(88) && is_new_action(89) && is_new_action(90) && !is_new_action(91), "n48disp_verb answers exactly 83..87, 89 and 90; 88 (pipeagdc) is a new action answered elsewhere");
    expect(kActAdopt == 83 && kActArm == 84 && kActStat == 85 && kActStamps == 86 && kActShortcut == 87 && kActFbname == 78, "the verb numbers are the published ones");

    // native exemptions: nothing when OFF; exactly the listed (action, arg) pairs when ON
    for (uint32_t a = 0; a <= 91; ++a) for (uint64_t g = 0; g <= 3; ++g) expect(!native_exempt(false, a, g), "no native exemption with the boot-arg OFF");
    expect(native_exempt(true, 78, 0) && native_exempt(true, 78, 1) && !native_exempt(true, 78, 2) && !native_exempt(true, 78, 1000), "fbname is exempt for 0 and 1 only");
    expect(native_exempt(true, 83, 0) && !native_exempt(true, 83, 1), "pipe adopt is exempt with argument 0 only");
    expect(native_exempt(true, 84, 0) && native_exempt(true, 84, 1) && !native_exempt(true, 84, 2), "pipe arm is exempt for 0 and 1");
    expect(native_exempt(true, 85, 0) && native_exempt(true, 85, 3) && native_exempt(true, 85, 4) && !native_exempt(true, 85, 5), "pipe stat is exempt for pages 0..4 (0.0.617: page 3, 0.0.618: page 4)");
    expect(native_exempt(true, 89, 0) && native_exempt(true, 89, 1) && !native_exempt(true, 89, 2) && !native_exempt(false, 89, 1), "pipevbl is exempt for 0 and 1, with the latch ON only (0.0.618)");
    expect(native_exempt(true, 90, 0) && native_exempt(true, 90, 1) && !native_exempt(true, 90, 2) && !native_exempt(false, 90, 0), "pipereload is exempt for 0 and 1, with the latch ON only (0.0.619)");
    expect(native_exempt(true, 86, 0) && !native_exempt(true, 86, 1), "pipe stamps is exempt with argument 0 only");
    expect(native_exempt(true, 87, 0) && native_exempt(true, 87, 1) && !native_exempt(true, 87, 2), "pipe shortcut is exempt for 0 and 1");
    expect(native_exempt(true, 88, 0) && native_exempt(true, 88, 1) && !native_exempt(true, 88, 2) && !native_exempt(false, 88, 1), "pipeagdc is exempt for 0 and 1, with the latch ON only");
    for (uint32_t a : { 1u, 2u, 31u, 50u, 66u, 67u, 76u, 77u, 79u, 80u, 81u, 82u, 91u }) expect(!native_exempt(true, a, 0) && !native_exempt(true, a, 1), "no other verb is exempt, even with the boot-arg ON");

    for (uint32_t a = 0; a <= 90; ++a) for (uint64_t g = 0; g <= 3; ++g) expect(native_exempt(true, a, g) == verb_args_ok(a, g), "native_exempt(ON) is exactly verb_args_ok");
    expect(!verb_args_ok(83, 1) && !verb_args_ok(86, 5) && !verb_args_ok(85, 5) && !verb_args_ok(87, 2) && !verb_args_ok(89, 2) && !verb_args_ok(84, 2) && !verb_args_ok(78, 2) && !verb_args_ok(82, 0), "verb_args_ok: the illegal arguments");

    // the ops table shape
    { const OpsShape off = ops_shape(false), on = ops_shape(true);
      expect(off.abi == 1 && off.size == N48_METAL_OPS_MIN && off.dispFlags == 0, "OFF: the ops table is the ABI-1, 120-byte table with no display flag (what 0.0.612 published)");
      expect(on.abi == 2 && on.size == N48_METAL_OPS_V2 && (on.dispFlags & N48_DISP_F_ON) != 0, "ON: ABI 2, 144 bytes, the display flag"); }
    expect(N48_METAL_ABI == 2 && N48_METAL_ABI_MIN == 1 && N48_METAL_OPS_V2 == 144 && N48_METAL_OPS_MIN == 120 && sizeof(N48MetalOps) == 144, "the ABI constants");
    expect(offsetof(N48MetalOps, disp_flags) == 120 && offsetof(N48MetalOps, disp_hook) == 128 && offsetof(N48MetalOps, pci_device) == 136, "the ABI-2 members sit after the 120 ABI-1 bytes");

    // the fact mask
    expect(kNeedFacts == (N48_FACT_RESOURCE | N48_FACT_SYSMEMORY | N48_FACT_VIDMEMORY) && kNeedFacts == 0xd, "the needed facts are RESOURCE | SYSMEMORY | VIDMEMORY");
    expect(!facts_ok(0) && !facts_ok(N48_FACT_RESOURCE) && !facts_ok(N48_FACT_SYSMEMORY | N48_FACT_VIDMEMORY) && !facts_ok(N48_FACT_RESOURCE | N48_FACT_VIDMEMORY) &&
           facts_ok(kNeedFacts) && facts_ok(0x7f), "facts_ok needs all three bits");
    expect_u("fact_mask OFF ignores the runtime bits", fact_mask(false, 0x40, kNeedFacts), 0x40);
    expect_u("fact_mask ON ORs the runtime bits in", fact_mask(true, 0x40, kNeedFacts), 0x40 | kNeedFacts);
    expect_u("fact_mask ON never adds a bit outside the needed three", fact_mask(true, 0, 0xffu), kNeedFacts);

    // the accelerator's positive controls
    expect(accel_layout_ok(0x480000, 8, 0) && accel_layout_ok(0x480000, 8, 1) && !accel_layout_ok(0x480000, 8, 2) && !accel_layout_ok(0, 8, 0) && !accel_layout_ok(0x480000, 7, 0) && !accel_layout_ok(0x480001, 8, 0),
           "accel_layout_ok: both stored values and a 0/1 gate byte");
    expect(kAccelPipeGate == 0xccf && kAccelCfgF0 == 0xc90 && kAccelCfgF4 == 0xcb0 && kAccelDm == 0x378 && kAccelEm == 0x380 && kAccelProvider == 0x368, "the accelerator offsets are the memo's");
    expect(kPipeAccel == 0x88 && kPipeDm == 0x90 && kPipeFb == 0x98 && kPipeFbRes == 0xe0 && kPipeActive == 0x298 && kPipeSize == 0x318, "the pipe offsets are the 11h.1 F1 ones");
    expect(kDmPipes == 0x88 && kDmCount == 0x108, "the display machine offsets are the memo's");

    // The layout constants pinned to the LITERAL numbers of the 11h.1 note / the memo / ioaccel-layout: a test that only reuses the constants cannot see one drift.
    expect(kTxnPlanes == 0x38 && kTxnDirty == 0x48 && kTxnStatus == 0x58 && kPlaneSurf == 0x20 && kPlaneRes == 0x30 && kDirtyPlane0 == 1, "11h.1 F2: the transaction offsets (+0x38 plane array, +0x48 dirty, +0x58 status; entry +0x20 IOSurface, +0x30 resource; dirty bit 0)");
    expect(kSurfW == 0x58 && kSurfH == 0x60 && kSurfBpr == 0x68 && kSurfBpe == 0x70 && kSurfElemW == 0x72 && kSurfBase == 0x78 && kSurfFmt == 0x80 && kSurfPlanes == 0x98 && kSurfUnk88 == 0x88 && kSurfFlags == 0x3da, "11h.1 F2: the IOSurface offsets");
    expect(kResShortcutFlag == 0xe && kResShortcutBit == 0x10 && kResPrepared == 0x70 && kResSysMem == 0x80 && kResW == 0xb0 && kResH == 0xb2 && kResBpr == 0xc0 && kResDc == 0xe0 && kDcSurf == 0x10, "11h.1 F2 / F3: the resource and device-cache offsets");
    expect(kSmFlags == 0xc && kSmBit4 == 0x04 && kSmLen == 0x40 && kSmMd == 0xd0, "11h.1 F2 / F3: the SysMemory offsets");
    expect(kEmCount == 0x30 && kEmDone == 0xf8 && kEmSub == 0xfc && kEmSize == 0xd30 && kAccelSize == 0xdd8 && kPipeSize == 0x318, "the event-machine words (SUSPECTED) and the layout sizes");
    expect(kObjSize == 0x80 && kObjFlagC == 0xc && kObjFlagD == 0xd && kObjFlagDValue == 0x10 && kObjZero38 == 0x38 && kObjVtSlots == 48 && kObjSlotRelease == 5 && kObjSlotMap == 39, "11h.1 F1: the stand-in object");
    expect(kMaxPipes == 8 && kScratchBytes == 16384 && kStampMaxSlots == 64 && kResOutBpr == 0xb8 && kResOutAlloc == 0xc8 && kKernelHalf == 0xffffff7000000000ull, "the table sizes and the kernel-half bound");

    // pipe_ok / verdicts: every single condition matters
    expect(pipe_ok(good_probe()) && pipe_verdict(good_probe()) == kOk, "a fully verified pipe is ok");
    { const char *names[] = { "accelOk", "dmReadable", "havePipe", "classOurs", "traced", "backAccel", "backDm", "backFb", "fbOurs" };
      for (int i = 0; i < 9; ++i) {
          PipeProbe p = good_probe();
          bool *f[] = { &p.accelOk, &p.dmReadable, &p.havePipe, &p.classOurs, &p.traced, &p.backAccel, &p.backDm, &p.backFb, &p.fbOurs };
          *f[i] = false;
          char m[96]; std::snprintf(m, sizeof m, "pipe_ok is false without %s", names[i]);
          expect(!pipe_ok(p) && pipe_verdict(p) != kOk, m);
      }
      PipeProbe two = good_probe(); two.count = 2; expect(!pipe_ok(two) && pipe_verdict(two) == kPipeMismatch, "two pipes: refused (the second would be an unhooked base pipe)");
      PipeProbe zero = good_probe(); zero.count = 0; expect(!pipe_ok(zero) && pipe_verdict(zero) == kNoPipe, "no pipe: kNoPipe"); }
    { PipeProbe p = good_probe(); p.classOurs = false; expect(pipe_verdict(p) == kPipeNotOurs, "a family pipe (class not ours): kPipeNotOurs");
      p = good_probe(); p.traced = false; expect(pipe_verdict(p) == kPipeNotOurs, "an untraced pipe: kPipeNotOurs");
      p = good_probe(); p.backFb = false; expect(pipe_verdict(p) == kPipeMismatch, "a pipe on another framebuffer: kPipeMismatch");
      p = good_probe(); p.accelOk = false; expect(pipe_verdict(p) == kNoAccel, "no accelerator: kNoAccel");
      p = good_probe(); p.dmReadable = false; expect(pipe_verdict(p) == kNoDisplayMachine, "no display machine: kNoDisplayMachine"); }
    { PipeProbe p {}; p.accelOk = true; p.dmReadable = true; p.count = 0; expect_u("precheck: an empty display machine is ok to probe", adopt_precheck(p), kOk);
      p = good_probe(); expect_u("precheck: the verified pipe is 'already'", adopt_precheck(p), kAlready);
      p = good_probe(); p.classOurs = false; expect_u("precheck: pipes that are not verified ours are refused", adopt_precheck(p), kHasPipes);
      p = good_probe(); p.accelOk = false; expect_u("precheck: no accelerator", adopt_precheck(p), kNoAccel);
      p = good_probe(); p.dmReadable = false; expect_u("precheck: no display machine", adopt_precheck(p), kNoDisplayMachine); }

    // arm_decide: disarm is unconditional; arm needs everything
    for (int bits = 0; bits < 16; ++bits) {
        ArmIn a { (bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, (bits & 8) != 0 };
        expect_u("arm 0 is allowed whatever the state (the recovery path)", arm_decide(0, a), kOk);
        expect_u("arm 2 is a bad argument", arm_decide(2, a), kBadArg);
        expect_u("arm 1 only with latch, facts, a verified pipe and the accelerator controls", arm_decide(1, a), bits == 15 ? kOk : (!a.latchOn ? kOff : !a.factsOk ? kFactsOff : !a.pipeOk ? kNoPipe : kAccelLayout));
    }
    expect_u("arm: a huge argument is a bad argument", arm_decide(0x100000000ull, ArmIn{true, true, true, true}), kBadArg);

    // submit / isComplete
    expect_u("submit: status 0 -> the will-perform code", submit_result(true, 0), kWillPerform);
    expect_u("submit: a nonzero status passes through (prepare failed: never reach perform)", submit_result(true, 0xE00002BEu), 0xE00002BEu);
    expect_u("submit: an unreadable status -> the will-perform code", submit_result(false, 0xE00002BEu), kWillPerform);
    expect(kWillPerform == 0xE00002D8u, "the will-perform code is 0xE00002D8");
    { bool never0 = true; for (uint32_t s : { 0u, 1u, 0xE00002BDu, 0xE0014042u, 0xffffffffu }) for (bool k : { true, false }) if (submit_result(k, s) == 0) never0 = false; expect(never0, "submit never returns 0 for any status"); }
    expect_u("isTransactionComplete is true", iscomplete_result(), 1);

    // bounds: the destination rows at the console's stride
    { BoundsIn b { 2560, 1440, 10240, 10240ull * 1440, 2560, 1440, 10240, 10240ull * 1440 };
      expect_u("bounds: the 1440p frame on the 1440p console", bounds_check(b), kBOk);
      BoundsIn c = b; c.width = 2561; expect_u("bounds: width != console width", bounds_check(c), kBGeom);
      c = b; c.height = 1441; c.srcBytes = 10240ull * 1441; expect_u("bounds: taller than the console", bounds_check(c), kBGeom);
      c = b; c.width = 0; expect_u("bounds: zero width", bounds_check(c), kBBadArg);
      c = b; c.srcBytesPerRow = 10239; expect_u("bounds: source stride narrower than a row", bounds_check(c), kBStride);
      c = b; c.consoleStride = 10236; c.consoleBytes = 10236ull * 1440; expect_u("bounds: console stride narrower than a row", bounds_check(c), kBStride);
      c = b; c.consoleBytes = 10240ull * 1440 - 1; expect_u("bounds: w*h*4 past the console", bounds_check(c), kBDest);
      // the correction: destination rows at the console's stride. w*h*4 fits (14.7 MB <= 14.8 MB) but the last row at stride 12288 does not.
      c = b; c.consoleStride = 12288; c.consoleBytes = 14800000ull; expect_u("bounds: destination rows at a wider console stride run past the console", bounds_check(c), kBDest);
      c = b; c.srcBytes = 10240ull * 1440 - 1; expect_u("bounds: source rows past the source", bounds_check(c), kBSrc);
      c = b; c.srcBytes = 10240ull * 1439 + 10240; expect_u("bounds: the source exactly fits", bounds_check(c), kBOk);
      c = b; c.srcBytesPerRow = 0x100000000ull; expect_u("bounds: a 2^32 stride is refused up front (no wrap)", bounds_check(c), kBBadArg);
      c = b; c.width = 0x100000000ull; expect_u("bounds: a 2^32 width is refused up front (no wrap)", bounds_check(c), kBBadArg);
      // a destination product past 2^32: truncated to 32 bits it would read as 4 bytes, which "fits" a 1 GiB console; in 64 bits it does not
      { BoundsIn w32 { 1, 65537, 4, 65536ull * 4 + 4, 1, 65537, 65536, 1ull << 30 };
        expect_u("bounds: a destination product of 2^32 + 4 is refused in 64 bits (a 32-bit product would wrap to 4 and be admitted)", bounds_check(w32), kBDest); }
      { BoundsIn w32 { 1, 65537, 65536, 1ull << 30, 1, 65537, 4, 1ull << 30 };
        expect_u("bounds: a source product of 2^32 + 4 is refused in 64 bits", bounds_check(w32), kBSrc); }
      c = b; c.width = 4097; c.consoleWidth = 4097; c.srcBytesPerRow = 4097 * 4; c.consoleStride = 4097 * 4; c.srcBytes = 4097ull * 4 * 1440; c.consoleBytes = 4097ull * 4 * 1440;
      expect_u("bounds: a row wider than the scratch buffer is refused", bounds_check(c), kBGeom);
      c = b; c.width = 4096; c.consoleWidth = 4096; c.srcBytesPerRow = 16384; c.consoleStride = 16384; c.srcBytes = 16384ull * 1440; c.consoleBytes = 16384ull * 1440;
      expect_u("bounds: a row exactly the scratch size is ok", bounds_check(c), kBOk); }
    expect(perf_reason_from_bounds(kBOk) == kPfCopied && perf_reason_from_bounds(kBGeom) == kPfGeom && perf_reason_from_bounds(kBStride) == kPfStride &&
           perf_reason_from_bounds(kBDest) == kPfDest && perf_reason_from_bounds(kBSrc) == kPfSrcRange && perf_reason_from_bounds(kBBadArg) == kPfGeom, "bounds verdicts map to perform reasons");

    // source admission
    expect(source_admitted(0, 1, kFmtBGRA, 4) && source_admitted(0, 0, kFmtBGRA, 4), "source admitted: base 0, one plane (count 0 or 1), BGRA, 4 bytes per element");
    expect(!source_admitted(4096, 1, kFmtBGRA, 4) && !source_admitted(0, 2, kFmtBGRA, 4) && !source_admitted(0, 1, 0x12345678u, 4) && !source_admitted(0, 1, kFmtBGRA, 2) && !source_admitted(0, 1, kFmtBGRA, 8),
           "source refused: base offset, plane count, format, element size");
    expect(kFmtBGRA == 0x42475241u, "the pixel format is the FourCC BGRA");
    expect(geometry_agrees(1, 2, 3, 1, 2, 3) && !geometry_agrees(9, 2, 3, 1, 2, 3) && !geometry_agrees(1, 9, 3, 1, 2, 3) && !geometry_agrees(1, 2, 9, 1, 2, 3), "surface and resource must agree on width, height and stride");

    // the shortcut guard and the slot-62 plan
    expect(shortcut_allowed(true, false) && !shortcut_allowed(true, true) && !shortcut_allowed(false, false) && !shortcut_allowed(false, true), "shortcut_allowed: known and bit 4 clear only; unknown fails closed");
    { Res62In in { true, 0x1000, true, 0x400, true, false, true };
      Res62Plan p = res62_plan(in); expect(p.handled && p.allocSize == 0x1000 && p.bytesPerRow == 0x400 && p.shortcut && !p.shortcutRefused, "res62: handled, outputs, shortcut when allowed and the switch is on");
      in.bit4 = true; p = res62_plan(in); expect(p.handled && !p.shortcut && p.shortcutRefused, "res62: bit 4 set -> no shortcut (refused)");
      in.bit4 = false; in.flagsKnown = false; p = res62_plan(in); expect(p.handled && !p.shortcut && p.shortcutRefused, "res62: flags unreadable -> no shortcut (fails closed)");
      in.flagsKnown = true; in.switchOn = false; p = res62_plan(in); expect(p.handled && !p.shortcut && !p.shortcutRefused, "res62: switch off -> no shortcut, not a refusal");
      in.switchOn = true; in.smKnown = false; in.flagsKnown = false; p = res62_plan(in); expect(!p.handled && !p.shortcut && p.shortcutRefused, "res62: SysMemory unreadable -> not handled, and no shortcut (its flags cannot be read)");
      in.smKnown = true; in.flagsKnown = true; in.smLen = 0; p = res62_plan(in); expect(!p.handled && p.shortcut, "res62: zero allocation size -> not handled, but the shortcut is independent (flags readable, bit clear)");
      in.smLen = 0x1000; in.surfKnown = false; p = res62_plan(in); expect(!p.handled && p.shortcut, "res62: IOSurface unreachable -> outputs not handled, the shortcut does not wait for it");
      in.surfKnown = true; in.surfBpr = 0; p = res62_plan(in); expect(!p.handled && p.shortcut, "res62: zero bytes per row -> not handled");
      in.surfBpr = 0x400; in.surfKnown = false; in.bit4 = true; p = res62_plan(in); expect(!p.handled && !p.shortcut && p.shortcutRefused, "res62: unreachable IOSurface AND bit 4 set -> nothing applied"); }

    // the stand-in object
    { uint8_t o[kObjSize]; std::memset(o, 0xEE, sizeof o); o[0] = 0x11; standin_fill(o);
      expect(o[0] == 0x11 && o[kObjFlagD] == 0x10 && o[kObjFlagC] == 0 && standin_check(o) == 0, "standin_fill: vptr kept, +0xd bit 0x10, +0xc clear, +0x38 zero");
      o[kObjFlagD] = 0; expect(standin_check(o) == 1, "standin_check: the prune-skip bit is required");
      standin_fill(o); o[kObjZero38 + 3] = 1; expect(standin_check(o) == 2, "standin_check: +0x38 must be zero"); }

    // the pipe table, the duration, the stamps
    { uint64_t t[kMaxPipes] = { 0x10, 0, 0x30 }; expect(pipe_table_find(t, 3, 0x30) == 2 && pipe_table_find(t, 3, 0x99) == -1 && pipe_table_find(t, 3, 0) == -1, "pipe table: found, absent, and 0 is never a pipe"); }
    { Dur d {}; expect_u("dur: avg of nothing", dur_avg(d), 0); dur_note(d, 500); dur_note(d, 100); dur_note(d, 900);
      expect(d.n == 3 && d.min == 100 && d.max == 900 && dur_avg(d) == 500, "dur: min / avg / max"); }
    expect(stamp_hazard(5, 6) && !stamp_hazard(6, 6) && !stamp_hazard(6, 5), "stamp hazard: submitted ahead of completed");
    expect_u("stamp walk bound clamps", stamp_walk_bound(1000), kStampMaxSlots);
    expect_u("stamp walk bound passes small counts", stamp_walk_bound(4), 4);
    expect(in_object(0, 4, 8) && in_object(4, 4, 8) && !in_object(5, 4, 8) && !in_object(9, 1, 8) && !in_object(0, 0, 8) && !in_object(~0ull, 2, 8), "in_object is overflow safe");
    for (uint32_t s = 0; s < kStatusCount; ++s) expect(std::strcmp(status_name(s), "unknown") != 0, "every status has a name");
    for (uint32_t r = 0; r < kPfCount; ++r) expect(std::strcmp(perf_name(r), "unknown") != 0, "every perform reason has a name");

    // =========================================================================================================================================================
    // O. ordering on the real flows
    // =========================================================================================================================================================
    { // adopt, the happy path: the exact call order
      FakeEnv e; e.before = PipeProbe{}; e.before.accelOk = true; e.before.dmReadable = true; e.before.count = 0; e.after = good_probe();
      const uint32_t st = adopt_flow(e);
      expect_u("adopt: OK", st, kOk);
      const char *want[] = { "adopt_begin", "find_accel", "accel_layout_ok", "probe_before", "facts_add", "request_probe", "probe_after", "publish_caps", "record_adopted", "adopt_end" };
      bool same = e.log.size() == 10; for (size_t i = 0; same && i < 10; ++i) same = e.log[i] == want[i];
      expect(same, "adopt: the call order is begin, find, controls, probe, FACTS, requestProbe, verify, capabilities, record, end");
      expect(e.idx("facts_add") < e.idx("request_probe"), "adopt: the resource facts are on BEFORE requestProbe");
      expect(e.idx("probe_after") < e.idx("publish_caps") && e.idx("publish_caps") < e.idx("record_adopted"), "adopt: the capabilities and the record come only after the verification");
      expect(facts_ok(e.factMask) && e.adoptEnd == kOk && e.adoptEndCalls == 1, "adopt: the facts are on and adopt_end ran once with the status"); }
    { // adopt that cannot turn the facts on never reaches requestProbe
      FakeEnv e; e.canAddFacts = false; e.before.accelOk = true; e.before.dmReadable = true; e.after = good_probe();
      expect_u("adopt: facts that stay off -> kFactsOff", adopt_flow(e), kFactsOff);
      expect(!e.has("request_probe") && !e.has("publish_caps") && !e.has("record_adopted") && e.adoptEndCalls == 1, "adopt without the resource facts: requestProbe is NEVER called, nothing published or recorded"); }
    { // partially on is not on
      FakeEnv e; e.factMask = N48_FACT_RESOURCE | N48_FACT_SYSMEMORY; e.canAddFacts = false; e.before.accelOk = e.before.dmReadable = true;
      expect_u("adopt: two of the three facts is refused", adopt_flow(e), kFactsOff); expect(!e.has("request_probe"), "adopt: requestProbe not called with two of three facts"); }
    { // foreign pipes already there: refused before the facts and before the probe
      FakeEnv e; e.before = good_probe(); e.before.classOurs = false;
      expect_u("adopt: foreign pipes -> kHasPipes", adopt_flow(e), kHasPipes);
      expect(!e.has("facts_add") && !e.has("request_probe") && !e.has("publish_caps"), "adopt: a display machine with foreign pipes is left alone"); }
    { // the verified pipe already there: no second requestProbe
      FakeEnv e; e.before = good_probe(); e.after = good_probe(); e.requested = false; e.current = good_probe(); e.factMask = 0;
      // after a verify the flow re-probes; with requested false the fake returns `current`
      const uint32_t st = adopt_flow(e);
      expect_u("adopt: the verified pipe is 'already'", st, kAlready);
      expect(!e.has("request_probe") && e.has("publish_caps") && e.has("record_adopted") && facts_ok(e.factMask), "adopt already: no second requestProbe; facts, capabilities and record done"); }
    { // requestProbe fails
      FakeEnv e; e.probeRc = false; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe();
      expect_u("adopt: a failed requestProbe", adopt_flow(e), kProbeFailed); expect(!e.has("publish_caps") && !e.has("record_adopted"), "adopt: nothing published or recorded after a failed probe"); }
    { // the pipe that appeared is not ours
      FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe(); e.after.classOurs = false;
      expect_u("adopt: a pipe that is not ours", adopt_flow(e), kPipeNotOurs); expect(!e.has("publish_caps") && !e.has("record_adopted"), "adopt: a foreign pipe is neither published for nor recorded"); }
    { FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe(); e.after.backFb = false;
      expect_u("adopt: a pipe on another framebuffer", adopt_flow(e), kPipeMismatch); expect(!e.has("record_adopted"), "adopt: a mismatched pipe is not recorded"); }
    { FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe(); e.capsOk = false;
      expect_u("adopt: the capabilities property fails", adopt_flow(e), kCapsFailed); expect(!e.has("record_adopted"), "adopt: no record without the capabilities"); }
    { FakeEnv e; e.latch = false; expect_u("adopt: OFF", adopt_flow(e), kOff); expect(e.log.empty(), "adopt with the boot-arg OFF touches nothing"); }
    { FakeEnv e; e.busy = true; expect_u("adopt: concurrent", adopt_flow(e), kBusy); expect(e.log.size() == 1 && e.adoptEndCalls == 0, "adopt: a concurrent adopt does nothing else"); }
    { FakeEnv e; e.accelFound = false; expect_u("adopt: no accelerator", adopt_flow(e), kNoAccel); expect(!e.has("facts_add") && !e.has("request_probe") && e.adoptEndCalls == 1, "adopt: no accelerator, nothing touched, adopt_end still runs"); }
    { FakeEnv e; e.layoutOk = false; expect_u("adopt: the accelerator fails its controls", adopt_flow(e), kAccelLayout); expect(!e.has("facts_add") && !e.has("request_probe"), "adopt: a failed layout control stops before any change"); }

    { // arm: a verified pipe exists before arm
      FakeEnv e; e.factMask = kNeedFacts; e.current = good_probe();
      expect_u("arm 1 with a verified pipe", arm_flow(e, 1), kOk);
      expect(e.idx("probe_before") >= 0 && e.idx("probe_before") < e.idx("arm_write1") && e.armByte == 1 && e.armedFlag, "arm 1: the pipe is verified BEFORE the byte is written, and the byte is 1"); }
    { FakeEnv e; e.factMask = kNeedFacts; e.current = PipeProbe{}; e.current.accelOk = true; e.current.dmReadable = true;
      expect_u("arm 1 with no pipe", arm_flow(e, 1), kNoPipe); expect(!e.has("arm_write1") && e.armByte == 0 && !e.armedFlag, "arm before the pipe exists: refused, nothing written"); }
    { FakeEnv e; e.factMask = kNeedFacts; e.current = good_probe(); e.current.classOurs = false;
      expect_u("arm 1 with a foreign pipe", arm_flow(e, 1), kNoPipe); expect(!e.has("arm_write1"), "arm: a pipe that is not ours is never armed"); }
    { FakeEnv e; e.factMask = 0; e.current = good_probe();
      expect_u("arm 1 without the facts", arm_flow(e, 1), kFactsOff); expect(!e.has("arm_write1") && !e.has("probe_before"), "arm: without the resource facts nothing is probed or written"); }
    { FakeEnv e; e.latch = false; e.factMask = kNeedFacts; e.current = good_probe();
      expect_u("arm 1 with the boot-arg OFF", arm_flow(e, 1), kOff); expect(!e.has("arm_write1"), "arm: OFF writes nothing"); }
    { FakeEnv e; e.factMask = kNeedFacts; e.current = good_probe(); e.layoutOk = false;
      expect_u("arm 1 with the accelerator failing its controls", arm_flow(e, 1), kAccelLayout); expect(!e.has("arm_write1"), "arm: a failed control writes nothing"); }
    { FakeEnv e; e.factMask = kNeedFacts; e.current = good_probe(); e.writeOk = false;
      expect_u("arm 1 whose write is refused", arm_flow(e, 1), kWriteFailed); expect(!e.armedFlag, "arm: the mirror flag follows the verified write only"); }
    for (int variant = 0; variant < 4; ++variant) { // disarm is always allowed and never depends on a probe
      FakeEnv e; e.armedFlag = true; e.armByte = 1;
      e.latch = (variant & 1) != 0; e.factMask = (variant & 2) ? kNeedFacts : 0; e.accelFound = false; e.layoutOk = false; e.current = PipeProbe{};
      expect_u("arm 0: always OK", arm_flow(e, 0), kOk);
      expect(e.has("disarm") && !e.armedFlag && e.armByte == 0 && !e.has("probe_before") && !e.has("probe_after") && !e.has("find_accel") && !e.has("accel_layout_ok"),
             "arm 0: the disarm runs with the latch / facts / accelerator / pipe in any state, and no probe runs"); }
    { FakeEnv e; expect_u("arm 2: bad argument", arm_flow(e, 2), kBadArg); expect(e.log.empty(), "arm 2 touches nothing"); }

    // =========================================================================================================================================================
    // R. reachability: the four slots over fake kernel memory
    // =========================================================================================================================================================
    { // the frame lands row by row
      World w; uint64_t ret = 99; const int h = w.perform(&ret);
      expect(h == 1 && ret == 0, "perform: handled, returns success");
      expect_u("perform: copied", w.e.reasons[kPfCopied], 1);
      bool same = true; for (uint64_t y = 0; y < w.H && same; ++y) same = std::memcmp(w.e.consoleData() + y * w.e.con.stride, w.srcRow(y), (size_t)(w.W * 4)) == 0;
      expect(same, "perform: every source row is in the console at the console's stride");
      expect(w.e.padsClean() && w.e.oobWrites == 0, "perform: nothing written outside the console");
      expect_u("perform: bytes", w.e.bytesNoted, 2560ull * 1440 * 4);
      expect(w.e.acquired == 1 && w.e.released == 1, "perform: the scratch buffer is released"); expect(w.e.srcReads == 1440 && w.e.dstWrites == 1440, "perform: one read and one write per row");
      expect(w.e.lastSource.sw == 2560 && w.e.lastSource.fmt == kFmtBGRA && w.e.lastSource.smLen == 10240ull * 1440, "perform: the source is logged"); }
    { // different strides: source wider than the console's
      World w(1024, 600, 4352); w.e.setConsole(1024, 600, 5120);     // the source stride is 4352, the console's 5120
      uint64_t ret; expect(w.perform(&ret) == 1, "perform (different strides): handled");
      expect_u("perform (different strides): copied", w.e.reasons[kPfCopied], 1);
      bool same = true; for (uint64_t y = 0; y < w.H && same; ++y) same = std::memcmp(w.e.consoleData() + y * 5120, w.srcRow(y), (size_t)(w.W * 4)) == 0;
      expect(same, "perform: rows are read at the source's stride and written at the console's");
      bool gap = true; for (uint64_t y = 0; y < w.H && gap; ++y) for (uint64_t x = w.W * 4; x < 5120; ++x) if (w.e.consoleData()[y * 5120 + x] != 0xAA) { gap = false; break; }
      expect(gap && w.e.padsClean(), "perform: the console's row padding is untouched"); }
    { World w; w.e.armedFlag = false; uint64_t ret = 9; const int h = w.perform(&ret);
      expect(h == 1 && ret == 0 && w.e.reasons[kPfDisarmed] == 1 && w.e.consoleUntouched() && w.e.reads == 0 && w.e.dstWrites == 0, "disarmed: the transaction completes (success), nothing is read and nothing is copied"); }
    { World w; w.e.mem.put<uint64_t>(A_TXN + kTxnDirty, 0); uint64_t ret; w.perform(&ret);
      expect(w.e.reasons[kPfNoPlane] == 1 && w.e.consoleUntouched() && ret == 0, "no plane 0 in the dirty mask: nothing copied, success"); }
    { World w; w.e.mem.put<uint64_t>(A_TXN + kTxnDirty, 2); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfNoPlane] == 1 && w.e.consoleUntouched(), "dirty mask without bit 0: no plane 0"); }
    { World w; w.e.mem.put<uint64_t>(A_TXN + kTxnPlanes, 0x1000); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfBadArr] == 1 && w.e.consoleUntouched() && ret == 0, "plane array not a kernel pointer"); }
    { World w; w.e.mem.put<uint64_t>(A_ARR + kPlaneSurf, 0x2000); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfBadSurf] == 1 && w.e.consoleUntouched(), "IOSurface not a kernel pointer"); }
    { World w; w.e.mem.put<uint64_t>(A_ARR + kPlaneRes, 0); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfBadSurf] == 1 && w.e.consoleUntouched(), "resource NULL"); }
    { World w; w.e.mem.put<uint64_t>(A_RES + kResSysMem, 0x3000); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfBadSrc] == 1 && w.e.consoleUntouched(), "SysMemory not a kernel pointer"); }
    { World w; w.e.mem.put<uint64_t>(A_SM + kSmMd, 0); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfBadSrc] == 1 && w.e.consoleUntouched(), "memory descriptor NULL"); }
    { World v; uint64_t a[1] = { 0x1234 }; uint64_t r2 = 7;
      expect(hook_dispatch(v.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 1, &r2) == 1 && r2 == 0 && v.e.reasons[kPfBadTxn] == 1 && v.e.consoleUntouched(), "txn not a kernel pointer: success, nothing copied"); }
    { World w; w.e.mem.put<uint64_t>(A_SURF + kSurfBase, 4096); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfFormat] == 1 && w.e.consoleUntouched(), "a nonzero baseOffset is refused"); }
    { World w; w.e.mem.put<uint32_t>(A_SURF + kSurfPlanes, 2); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfFormat] == 1 && w.e.consoleUntouched(), "two planes are refused"); }
    { World w; w.e.mem.put<uint32_t>(A_SURF + kSurfPlanes, 0); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfCopied] == 1, "plane count 0 (a non-planar surface) is admitted"); }
    { World w; w.e.mem.put<uint32_t>(A_SURF + kSurfFmt, 0x52474241u); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfFormat] == 1 && w.e.consoleUntouched(), "a pixel format that is not BGRA is refused"); }
    { World w; w.e.mem.put<uint16_t>(A_SURF + kSurfBpe, 8); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfFormat] == 1 && w.e.consoleUntouched(), "8 bytes per element is refused"); }
    { World w; w.e.mem.put<uint64_t>(A_SURF + kSurfUnk88, 0x1234); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfCopied] == 1 && w.e.lastSource.unk88 == 0x1234, "+0x88 is logged, never decided on"); }
    { World w; w.e.mem.put<uint16_t>(A_RES + kResW, 2559); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfMismatch] == 1 && w.e.consoleUntouched(), "resource width disagrees with the IOSurface: nothing copied"); }
    { World w; w.e.mem.put<uint16_t>(A_RES + kResH, 1439); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfMismatch] == 1 && w.e.consoleUntouched(), "resource height disagrees: nothing copied"); }
    { World w; w.e.mem.put<uint64_t>(A_RES + kResBpr, 10496); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfMismatch] == 1 && w.e.consoleUntouched(), "resource stride disagrees: nothing copied"); }
    { World w; w.e.haveConsole = false; uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfNoConsole] == 1 && w.e.consoleUntouched(), "console region unknown: nothing copied"); }
    { World w(2560, 1440, 10240); w.e.setConsole(2000, 1440, 8000); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfGeom] == 1 && w.e.consoleUntouched() && w.e.oobWrites == 0, "width != console width: nothing copied"); }
    { World w; w.e.setConsole(2560, 1000, 10240); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfGeom] == 1 && w.e.consoleUntouched() && w.e.padsClean(), "taller than the console: nothing copied, nothing past it"); }
    { // the correction: the console reports a stride whose LAST row does not fit its own byte count, w*h*4 alone would pass
      World w; w.e.setConsole(2560, 1440, 12288); w.e.con.bytes = 14800000ull;
      uint64_t ret; w.perform(&ret);
      expect(w.e.reasons[kPfDest] == 1 && w.e.consoleUntouched() && w.e.oobWrites == 0 && w.e.dstWrites == 0 && w.e.padsClean(), "destination rows past the console at its stride: refused before the FIRST write"); }
    { World w; w.e.setConsole(2560, 1440, 10240); w.e.con.bytes = 10240ull * 1440 - 1;
      uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfDest] == 1 && w.e.dstWrites == 0, "w*h*4 past the console: refused"); }
    { World w; w.e.mem.put<uint64_t>(A_SM + kSmLen, 10240ull * 1440 - 1); uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfSrcRange] == 1 && w.e.dstWrites == 0 && w.e.consoleUntouched(), "source shorter than its rows: refused before the first read"); }
    { World w; w.e.scratchMode = 1; uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfBusy] == 1 && w.e.consoleUntouched() && ret == 0, "a copy already running: this frame is completed, not copied"); }
    { World w; w.e.scratchMode = 2; uint64_t ret; w.perform(&ret); expect(w.e.reasons[kPfNoScratch] == 1 && w.e.consoleUntouched(), "no scratch buffer: nothing copied"); }
    { World w; w.e.srcFailAfter = 100; uint64_t ret = 5; w.perform(&ret);
      expect(w.e.reasons[kPfSrcRead] == 1 && ret == 0 && w.e.dstWrites == 100 && w.e.acquired == w.e.released && w.e.padsClean(), "a short source read stops the copy; success; the scratch buffer is released; nothing past the console"); }
    { World w; w.e.dstFailAfter = 50; uint64_t ret = 5; w.perform(&ret);
      expect(w.e.reasons[kPfDstWrite] == 1 && ret == 0 && w.e.acquired == w.e.released && w.e.padsClean(), "a refused console write stops the copy; success; the scratch buffer is released"); }
    { // whatever happens, perform returns success and the scratch buffer is balanced
      bool ok = true; uint32_t seen = 0;
      for (int v = 0; v < 26; ++v) {
          World w;
          switch (v) {
          case 1: w.e.armedFlag = false; break; case 2: w.e.mem.put<uint64_t>(A_TXN + kTxnDirty, 0); break; case 3: w.e.mem.put<uint64_t>(A_TXN + kTxnPlanes, 1); break;
          case 4: w.e.mem.put<uint64_t>(A_ARR + kPlaneSurf, 1); break; case 5: w.e.mem.put<uint64_t>(A_RES + kResSysMem, 1); break; case 6: w.e.mem.put<uint64_t>(A_SURF + kSurfBase, 1); break;
          case 7: w.e.mem.put<uint16_t>(A_RES + kResW, 1); break; case 8: w.e.haveConsole = false; break; case 9: w.e.setConsole(100, 100, 400); break; case 10: w.e.scratchMode = 1; break;
          case 11: w.e.scratchMode = 2; break; case 12: w.e.srcFailAfter = 3; break; case 13: w.e.dstFailAfter = 3; break; case 14: w.e.con.bytes = 5; break; case 15: w.e.latch = true; break;
          case 16: w.e.mem.put<uint64_t>(A_SM + kSmLen, 1); break; case 17: w.e.mem.put<uint32_t>(A_SURF + kSurfFmt, 0); break; case 18: w.e.mem.put<uint64_t>(A_SM + kSmMd, 0); break;
          case 19: w.e.mem.put<uint64_t>(A_ARR + kPlaneRes, 1); break; case 20: w.e.mem.put<uint64_t>(A_SURF + kSurfW, 0); break; case 21: w.e.mem.put<uint64_t>(A_SURF + kSurfH, 0); break;
          case 22: w.e.mem.put<uint64_t>(A_SURF + kSurfBpr, 0); break; case 23: w.e.mem.put<uint64_t>(A_SURF + kSurfW, 0xffffffffffffffffull); break;
          case 24: w.e.mem.put<uint64_t>(A_SURF + kSurfBpr, 0xffffffffffffffffull); break; default: break;
          }
          uint64_t ret = 77; const int h = w.perform(&ret);
          if (h != 1 || ret != 0 || w.e.acquired != w.e.released || !w.e.padsClean() || w.e.oobWrites != 0 || w.e.performNotes != 1) ok = false;
          for (uint32_t r = 0; r < kPfCount; ++r) if (w.e.reasons[r]) seen |= 1u << r;
      }
      expect(ok, "perform: in every state handled with success, the scratch buffer balanced, nothing written past the console, exactly one note");
      expect((seen & ((1u << kPfCopied) | (1u << kPfDisarmed) | (1u << kPfNoPlane) | (1u << kPfBadArr) | (1u << kPfBadSurf) | (1u << kPfBadSrc) | (1u << kPfFormat) | (1u << kPfMismatch) | (1u << kPfNoConsole) | (1u << kPfGeom) | (1u << kPfBusy) | (1u << kPfNoScratch) | (1u << kPfSrcRead) | (1u << kPfDstWrite) | (1u << kPfSrcRange))) != 0, "the sweep reached many distinct reasons"); }

    // the dispatcher
    { World w; uint64_t ret = 1; uint64_t a[1] = { A_TXN };
      expect(hook_dispatch(w.e, N48_VC_CTX2D, 277, A_PIPE, a, 1, &ret) == 0, "a class that is not the display pipe is not handled");
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE2, a, 1, &ret) == 0 && w.e.consoleUntouched(), "perform on a pipe we do not know is NOT handled (the family's own slot runs)");
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 278, A_PIPE2, a, 1, &ret) == 0 && hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE2, a, 1, &ret) == 0, "isComplete / submit on an unknown pipe are not handled");
      uint64_t two[2] = { 0, A_RES }; expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE2, two, 2, &ret) == 0, "slot 267 on an unknown pipe is not handled");
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 0, &ret) == 0 && hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 2, &ret) == 0, "a wrong argument count is not handled");
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 266, A_PIPE, a, 1, &ret) == 0 && hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 282, A_PIPE, a, 1, &ret) == 0, "slots we do not hook are not handled");
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, nullptr, 1, &ret) == 0 && hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 1, nullptr) == 0 && hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 7, &ret) == 0, "NULL args / NULL ret / too many args are not handled");
      w.e.latch = false; expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 1, &ret) == 0 && hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE, a, 1, &ret) == 0 && w.e.consoleUntouched(), "with the latch OFF nothing is handled"); }
    { World w; uint64_t ret = 0, a[1] = { A_TXN };
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 278, A_PIPE, a, 1, &ret) == 1 && ret == 1, "isTransactionComplete: handled, true");
      w.e.mem.put<uint32_t>(A_TXN + kTxnStatus, 0);
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE, a, 1, &ret) == 1 && ret == 0xE00002D8ull, "submitTransaction: status 0 -> 0xE00002D8, never 0");
      w.e.mem.put<uint32_t>(A_TXN + kTxnStatus, 0xE00002BEu);
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE, a, 1, &ret) == 1 && ret == 0xE00002BEull, "submitTransaction: a nonzero status passes through");
      uint64_t bad[1] = { 0x10 }; ret = 5;
      expect(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE, bad, 1, &ret) == 1 && ret == 0xE00002D8ull, "submitTransaction: an unreadable transaction -> 0xE00002D8, never 0"); }
    { // slot 267
      World w; uint64_t two[2] = { 0, A_RES }, ret = 0;
      const int h = hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, two, 2, &ret);
      expect(h == 1 && ret != 0, "slot 267: handled, returns an object pointer (not a status)");
      const uint8_t *o = (const uint8_t *)(uintptr_t)ret;
      expect(o && o[kObjFlagD] == 0x10 && standin_check(o) == 0 && o[0] == 0xEF && o[3] == 0xDE && o[7] == 4, "slot 267: the stand-in has +0xd bit 0x10, +0x38 zero and its vtable pointer intact");
      expect_u("slot 267: the pipe is marked active", w.e.mem.get<uint8_t>(A_PIPE + kPipeActive), 1);
      expect(w.e.initFbNotes == 1, "slot 267: noted");
      World n; n.e.standinNull = true; ret = 5; uint64_t t2[2] = { 0, A_RES };
      expect(hook_dispatch(n.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, t2, 2, &ret) == 0 && n.e.mem.get<uint8_t>(A_PIPE + kPipeActive) == 0, "slot 267 without a stand-in object: not handled and the pipe is NOT marked active");
      World u; u.e.pipes.clear(); ret = 5; uint64_t t3[2] = { 0, A_RES };
      expect(hook_dispatch(u.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, t3, 2, &ret) == 0 && u.e.mem.get<uint8_t>(A_PIPE + kPipeActive) == 0, "slot 267 on a pipe we do not know: untouched"); }

    // ---- 0.0.616: the prepared-descriptor cache (run m11h4-1: perform's readBytes on an unprepared IOGMD panicked). perform never wires; submit does, in thread context ----
    { gAutoPrep = false; World w; gAutoPrep = true;                          // R1: perform on a cache MISS refuses and never touches the descriptor
      uint64_t ret = 9; const int h = w.perform(&ret);
      expect(h == 1 && ret == 0 && w.e.reasons[kPfNotPrepared] == 1 && w.e.reasons[kPfSrcRead] == 0, "cache miss: perform completes with reason NotPrepared (never a short read)");
      expect(w.e.srcReads == 0 && w.e.unheldReads == 0 && w.e.dstWrites == 0 && w.e.consoleUntouched() && w.e.padsClean(), "cache miss: src_read is never called, nothing is copied");
      expect(w.e.prepCount.empty() && w.e.complCount.empty() && !w.e.has("md_prepare"), "cache miss: perform never prepares (and never completes) a descriptor");
      expect(w.e.acquired == w.e.released && w.e.mdcache.misses == 1 && w.e.mdNotes[kMdNoteMiss] == 1, "cache miss: counted and noted, scratch balanced"); }
    { gAutoPrep = false; World w; gAutoPrep = true;                          // R2: submit fills; ordering prepare (submit) before the first read (perform)
      uint64_t ret = 0;
      expect(w.submit(&ret) == 1 && ret == 0xE00002D8ull, "submit: still returns the will-perform code");
      expect(w.e.prepCount[A_MD] == 1 && w.e.mdcache.prepared == 1 && w.e.mdNotes[kMdNoteFill] == 1, "submit: the plane-0 descriptor (sysmem+0xd0) is prepared once");
      expect(w.submit(&ret) == 1 && w.e.prepCount[A_MD] == 1 && w.e.mdcache.hits == 1, "submit again: a hit, not prepared a second time");
      expect(!mdc_held(w.e.mdcache, A_MD), "a Ready entry that no perform holds is not 'held' (src_read's assertion needs the hold)");
      uint64_t pr = 5; expect(w.perform(&pr) == 1 && pr == 0 && w.e.reasons[kPfCopied] == 1, "perform after submit: the frame is copied");
      expect(w.e.idx("md_prepare") >= 0 && w.e.idx("src_read") > w.e.idx("md_prepare"), "ORDER: the prepare (submit) comes before the first read (perform)");
      bool idle = true; for (uint32_t i = 0; i < kMdCacheN; ++i) if (w.e.mdcache.e[i].busy != 0u) idle = false;
      expect(idle && w.e.unheldReads == 0, "perform released its hold on the entry; every read was under a hold");
      bool same = true; for (uint64_t y = 0; y < w.H && same; ++y) same = std::memcmp(w.e.consoleData() + y * w.e.con.stride, w.srcRow(y), (size_t)(w.W * 4)) == 0;
      expect(same, "perform after submit: every row landed"); }
    { gAutoPrep = false; World w; gAutoPrep = true; w.e.prepFail = true; uint64_t ret = 0;      // R3: a failed prepare inserts nothing; perform then refuses
      expect(w.submit(&ret) == 1 && ret == 0xE00002D8ull && w.e.mdcache.prepFail == 1 && w.e.mdcache.prepared == 0 && w.e.mdNotes[kMdNotePrepFail] == 1, "prepare failed: counted, submit's result unchanged");
      uint64_t pr = 5; w.perform(&pr);
      expect(w.e.reasons[kPfNotPrepared] == 1 && w.e.srcReads == 0 && w.e.consoleUntouched(), "prepare failed: perform refuses with NotPrepared, reads nothing");
      bool empty = true; for (uint32_t i = 0; i < kMdCacheN; ++i) if (w.e.mdcache.e[i].state != kMdEmpty || w.e.mdcache.e[i].md != 0) empty = false;
      expect(empty, "prepare failed: no entry was inserted"); }
    { gAutoPrep = false; World w; gAutoPrep = true; uint64_t ret = 0;      // R4: submit only prepares what will reach perform, only while armed
      w.e.mem.put<uint32_t>(A_TXN + kTxnStatus, 0xE00002BEu);
      expect(w.submit(&ret) == 1 && ret == 0xE00002BEull && w.e.prepCount.empty(), "submit with a nonzero txn status: passed through, nothing prepared");
      w.e.mem.put<uint32_t>(A_TXN + kTxnStatus, 0); w.e.armedFlag = false;
      expect(w.submit(&ret) == 1 && ret == 0xE00002D8ull && w.e.prepCount.empty() && w.e.mdcache.prepared == 0, "submit while disarmed: nothing is wired");
      w.e.armedFlag = true; w.e.mem.put<uint64_t>(A_TXN + kTxnDirty, 0);
      expect(submit_prepare(w.e, A_TXN) == kMdNoSource && w.e.prepCount.empty(), "submit without plane 0: nothing to prepare");
      w.e.mem.put<uint64_t>(A_TXN + kTxnDirty, 1); w.e.mem.put<uint64_t>(A_SM + kSmMd, 0x1000);
      expect(submit_prepare(w.e, A_TXN) == kMdNoSource && w.e.prepCount.empty(), "submit with a descriptor that is not a kernel pointer: nothing prepared");
      expect(submit_prepare(w.e, 0x10) == kMdNoSource, "submit with a bad txn pointer: nothing prepared"); }
    { gAutoPrep = false; World w; gAutoPrep = true; uint64_t md[16];     // R5: LRU eviction; never a busy entry; full = refuse
      for (int i = 0; i < 16; ++i) md[i] = ::K + 0x900000ull + 0x1000ull * i;
      for (int i = 0; i < 8; ++i) { w.e.mem.put<uint64_t>(A_SM + kSmMd, md[i]); expect(submit_prepare(w.e, A_TXN) == kMdFilled, "fill: entry prepared"); }
      expect(w.e.mdcache.prepared == 8 && w.e.complCount.empty(), "eight distinct descriptors fit, nothing completed");
      w.e.mem.put<uint64_t>(A_SM + kSmMd, md[0]); expect(submit_prepare(w.e, A_TXN) == kMdHit, "a hit refreshes the LRU stamp (md0 is now the newest, md1 the oldest)");
      const int hold = mdc_acquire(w.e.mdcache, md[1]);                  // a perform is inside md1, the LRU entry
      if (hold >= 0) w.e.mdcache.e[hold].last = 0;                       // (setup) a perform's hold refreshes the stamp; put md1 back as the oldest so the LRU victim IS the busy one
      expect(hold >= 0 && w.e.mdcache.e[hold].busy == 1u && w.e.mdcache.e[hold].last == 0, "(setup) md1 is held busy and is the LRU entry");
      w.e.mem.put<uint64_t>(A_SM + kSmMd, md[8]); expect(submit_prepare(w.e, A_TXN) == kMdFilled, "the 9th descriptor is prepared");
      expect(w.e.complCount.count(md[1]) == 0 && w.e.complCount[md[2]] == 1 && w.e.complCount.size() == 1 && w.e.mdcache.evicted == 1 && w.e.mdcache.evictBlocked == 1, "eviction tried and skipped the BUSY LRU entry (md1) and completed the next one (md2), exactly once");
      expect(mdc_held(w.e.mdcache, md[1]) && w.e.mdcache.e[hold].state == kMdReady && w.e.mdcache.e[hold].md == md[1], "the busy entry is still Ready with its descriptor");
      mdc_release(w.e.mdcache, hold);
      int holds[kMdCacheN], nh = 0; for (uint32_t i = 0; i < kMdCacheN; ++i) { const uint64_t m = w.e.mdcache.e[i].md; const int h2 = mdc_acquire(w.e.mdcache, m); if (h2 >= 0) holds[nh++] = h2; }
      expect(nh == (int)kMdCacheN, "(setup) every entry held busy");
      const size_t before = w.e.complCount.size(); const uint64_t pc = w.e.mdcache.prepared;
      w.e.mem.put<uint64_t>(A_SM + kSmMd, md[9]); expect(submit_prepare(w.e, A_TXN) == kMdFull && w.e.complCount.size() == before && w.e.mdcache.prepared == pc && w.e.prepCount.count(md[9]) == 0 && w.e.mdcache.full == 1, "every entry busy: nothing evicted, nothing prepared, counted");
      for (int i = 0; i < nh; ++i) mdc_release(w.e.mdcache, holds[i]);
      expect(submit_prepare(w.e, A_TXN) == kMdFilled && w.e.mdcache.evicted == 2, "after the holds are released the next descriptor evicts again"); }
    { gAutoPrep = false; World w; gAutoPrep = true; uint64_t md[8];       // R6: teardown completes every entry exactly once (disarm, withdraw); a second teardown completes nothing
      for (int i = 0; i < 8; ++i) md[i] = ::K + 0x900000ull + 0x1000ull * i;
      for (int i = 0; i < 5; ++i) { w.e.mem.put<uint64_t>(A_SM + kSmMd, md[i]); submit_prepare(w.e, A_TXN); }
      expect(arm_flow(w.e, 0) == kOk && !w.e.armedFlag, "arm 0: disarmed");
      bool once = true; for (int i = 0; i < 5; ++i) if (w.e.complCount[md[i]] != 1) once = false;
      expect(once && w.e.complCount.size() == 5 && w.e.mdcache.tornDown == 5, "arm 0: every cached descriptor completed exactly once");
      bool empty = true; for (uint32_t i = 0; i < kMdCacheN; ++i) if (w.e.mdcache.e[i].state != kMdEmpty) empty = false;
      expect(empty, "arm 0: the cache is empty");
      expect(arm_flow(w.e, 0) == kOk && w.e.mdcache.tornDown == 5 && w.e.complCount.size() == 5, "a second disarm completes nothing more (no double complete)");
      int tot = 0; for (auto &kv : w.e.complCount) tot += kv.second; expect(tot == 5, "five completes in total");
      uint64_t pr = 5; w.perform(&pr); expect(w.e.reasons[kPfDisarmed] == 1 && w.e.srcReads == 0 && w.e.unheldReads == 0, "disarm then perform: refused as disarmed, the descriptor is not read");
      w.e.armedFlag = true; w.e.mem.put<uint64_t>(A_SM + kSmMd, A_MD); pr = 5; w.perform(&pr);
      expect(w.e.reasons[kPfNotPrepared] == 1 && w.e.srcReads == 0 && w.e.unheldReads == 0 && w.e.consoleUntouched(), "re-armed without a submit: the torn-down descriptor is a MISS, not a stale hit"); }
    { gAutoPrep = false; World w; gAutoPrep = true; uint64_t md[3];        // R7: withdraw: disarm first, then teardown, then forget
      for (int i = 0; i < 3; ++i) { md[i] = ::K + 0x900000ull + 0x1000ull * i; w.e.mem.put<uint64_t>(A_SM + kSmMd, md[i]); submit_prepare(w.e, A_TXN); }
      w.e.log.clear(); withdraw_flow(w.e);
      bool once = true; for (int i = 0; i < 3; ++i) if (w.e.complCount[md[i]] != 1) once = false;
      expect(once && w.e.complCount.size() == 3, "withdraw: every cached descriptor completed exactly once");
      expect(w.e.idx("disarm") >= 0 && w.e.idx("md_complete") > w.e.idx("disarm") && w.e.idx("forget_pipes") > w.e.idx("md_complete"), "withdraw: disarm, then the completes, then the pipes are forgotten"); }
    { gAutoPrep = false; World w; gAutoPrep = true; uint64_t md = ::K + 0x900000ull;   // R8: teardown waits (bounded) for a perform inside the entry
      w.e.mem.put<uint64_t>(A_SM + kSmMd, md); submit_prepare(w.e, A_TXN);
      const int hold = mdc_acquire(w.e.mdcache, md); int calls = 0;
      w.e.onClock = [&]() { if (++calls == 20) mdc_release(w.e.mdcache, hold); };
      mdc_teardown(w.e);
      expect(calls >= 20 && w.e.complCount[md] == 1 && w.e.mdcache.drainFail == 0, "teardown waited for the in-flight perform, then completed once");
      gAutoPrep = false; World x; gAutoPrep = true; x.e.mem.put<uint64_t>(A_SM + kSmMd, md); submit_prepare(x.e, A_TXN);
      const int h2 = mdc_acquire(x.e.mdcache, md); (void)h2; mdc_teardown(x.e);
      expect(x.e.complCount.empty() && x.e.mdcache.drainFail == 1 && x.e.mdcache.leaked == 1 && x.e.mdcache.e[h2].state == kMdRetiring, "a perform that never drains: BOUNDED wait, the entry is leaked (not completed), the entry is dead");
      uint64_t pr = 5; x.e.armedFlag = true; x.perform(&pr); expect(x.e.reasons[kPfNotPrepared] == 1 && x.e.srcReads == 0, "a dead entry is never used again"); }
    { gAutoPrep = false; World w; gAutoPrep = true; uint64_t md[3];        // R9: the HUNG latch: leak, never complete; no new wiring
      for (int i = 0; i < 3; ++i) { md[i] = ::K + 0x900000ull + 0x1000ull * i; w.e.mem.put<uint64_t>(A_SM + kSmMd, md[i]); submit_prepare(w.e, A_TXN); }
      w.e.hungFlag = true;
      w.e.mem.put<uint64_t>(A_SM + kSmMd, ::K + 0x990000ull);
      expect(submit_prepare(w.e, A_TXN) == kMdHungRefused && w.e.prepCount.count(::K + 0x990000ull) == 0, "HUNG: no new descriptor is wired");
      w.e.mem.put<uint64_t>(A_SM + kSmMd, md[1]); expect(submit_prepare(w.e, A_TXN) == kMdHit, "HUNG: an existing entry is still a hit");
      mdc_teardown(w.e);
      expect(w.e.complCount.empty() && w.e.mdcache.leaked == 3 && w.e.mdcache.tornDown == 0, "HUNG: teardown leaks every descriptor (nothing completed or released)"); }
    { gAutoPrep = false; World w; gAutoPrep = true; w.e.armedTrueCalls = 1; uint64_t ret = 0;   // R10: a disarm that races the fill leaves nothing wired
      expect(w.submit(&ret) == 1 && w.e.prepCount[A_MD] == 1 && w.e.complCount[A_MD] == 1 && w.e.mdcache.tornDown == 1, "armed at the start, disarmed by the end of the fill: the new entry is torn down again"); }
    { gAutoPrep = false; World w; gAutoPrep = true; const uint64_t other = ::K + 0x900000ull;      // R12: a hit is for THAT descriptor only
      w.e.mem.put<uint64_t>(A_SM + kSmMd, other); submit_prepare(w.e, A_TXN);
      w.e.mem.put<uint64_t>(A_SM + kSmMd, A_MD); uint64_t pr = 5; w.perform(&pr);
      expect(w.e.reasons[kPfNotPrepared] == 1 && w.e.srcReads == 0 && w.e.unheldReads == 0 && w.e.consoleUntouched(), "a cached descriptor does not admit a different one: MISS, nothing read"); }
    { World w; uint64_t pr = 5;                                              // R11: whole-path liveness through the dispatcher with the autoprep world
      expect(w.perform(&pr) == 1 && w.e.reasons[kPfCopied] == 1 && w.e.mdcache.misses == 0, "a normal submit-then-perform is copied, no miss"); }
    { // the new reason has a name and its own number; the cache sits at the published size
      expect(kPfNotPrepared == 19 && kPfScanOwned == 20 && kPfCount == 21 && std::strcmp(perf_name(kPfScanOwned), "unknown") != 0 && std::strcmp(perf_name(kPfNotPrepared), "unknown") != 0 && kMdCacheN == 8, "the new reason code (19) is named; the cache has 8 entries"); }

    // slot 62
    constexpr uint64_t A_OUT = 0xffffff8000780000ull;       // the caller's two output words (locals): NOT inside the resource
    auto res62_world = [&](World &w) { w.e.mem.put<uint64_t>(A_RES + kResDc, A_DC); w.e.mem.add(A_OUT, 0x40); };
    { World w; res62_world(w);
      uint64_t args[2] = { A_OUT, A_OUT + 8 }, ret = 9;
      const int h = res62_flow(w.e, A_RES, args, 2, &ret, true);
      expect(h == 1 && ret == 0, "slot 62: handled, the slot's own return value is 0");
      expect_u("slot 62: args[0] receives the allocation size", w.e.mem.get<uint64_t>(A_OUT), 10240ull * 1440);
      expect_u("slot 62: args[1] receives the bytes per row", w.e.mem.get<uint64_t>(A_OUT + 8), 10240);
      expect(w.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0x10 && w.e.mem.get<uint64_t>(A_RES + kResPrepared) == A_SM, "slot 62: the shortcut is applied (res+0xe |= 0x10, res+0x70 = res+0x80)");
      expect(w.e.res62Notes == 1 && w.e.lastPlan.shortcut, "slot 62: a log note either way"); }
    { World w; res62_world(w); w.e.mem.put<uint8_t>(A_SM + kSmFlags, 0x04); uint64_t args[2] = { A_OUT, A_OUT + 8 }, ret = 9;
      expect(res62_flow(w.e, A_RES, args, 2, &ret, true) == 1 && w.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0 && w.e.mem.get<uint64_t>(A_RES + kResPrepared) == 0, "slot 62: the shortcut is NOT applied when SysMemory +0xc bit 4 is set");
      expect(w.e.mem.get<uint64_t>(A_OUT) == 10240ull * 1440 && w.e.res62Notes == 1 && w.e.lastPlan.shortcutRefused, "slot 62: the outputs are still written and the refusal is noted"); }
    { World w; res62_world(w); w.e.mem.put<uint8_t>(A_SM + kSmFlags, 0x0f); uint64_t args[2] = { A_OUT, A_OUT + 8 }, ret = 9;
      res62_flow(w.e, A_RES, args, 2, &ret, true); expect(w.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0, "slot 62: bit 4 set among other bits -> no shortcut");
      World v; res62_world(v); v.e.mem.put<uint8_t>(A_SM + kSmFlags, 0xfb); res62_flow(v.e, A_RES, args, 2, &ret, true); expect(v.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0x10, "slot 62: every bit but bit 4 set -> the shortcut is applied"); }
    { World w; res62_world(w); uint64_t args[2] = { A_OUT, A_OUT + 8 }, ret = 9;
      expect(res62_flow(w.e, A_RES, args, 2, &ret, false) == 1 && w.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0 && w.e.mem.get<uint64_t>(A_OUT + 8) == 10240, "slot 62: switch off -> outputs written, no shortcut"); }
    { World w; res62_world(w); w.e.mem.put<uint64_t>(A_RES + kResSysMem, 0); uint64_t args[2] = { A_OUT, A_OUT + 8 }, ret = 9;
      expect(res62_flow(w.e, A_RES, args, 2, &ret, true) == 0 && w.e.mem.get<uint64_t>(A_OUT) == 0 && w.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0, "slot 62: SysMemory not yet set -> not handled, nothing written, no shortcut"); }
    { World w; res62_world(w); w.e.mem.put<uint64_t>(A_RES + kResDc, 0); uint64_t args[2] = { A_OUT, A_OUT + 8 }, ret = 9;
      expect(res62_flow(w.e, A_RES, args, 2, &ret, true) == 0 && w.e.mem.get<uint64_t>(A_OUT + 8) == 0 && w.e.mem.get<uint64_t>(A_OUT) == 0, "slot 62: device cache not yet set -> outputs not handled, nothing written to them");
      expect(w.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0x10 && w.e.mem.get<uint64_t>(A_RES + kResPrepared) == A_SM, "slot 62: ... but the shortcut (guard clear, switch on) does not wait for the IOSurface"); 
      World v; res62_world(v); v.e.mem.put<uint64_t>(A_RES + kResDc, 0); v.e.mem.put<uint8_t>(A_SM + kSmFlags, 4); uint64_t r2 = 9;
      expect(res62_flow(v.e, A_RES, args, 2, &r2, true) == 0 && v.e.mem.get<uint8_t>(A_RES + kResShortcutFlag) == 0, "slot 62: ... and with bit 4 set there is no shortcut either"); }
    { World w; res62_world(w); uint64_t ret = 9;
      uint64_t same[2] = { A_OUT, A_OUT }; expect(res62_flow(w.e, A_RES, same, 2, &ret, true) == 0, "slot 62: the same pointer twice -> not handled");
      uint64_t unal[2] = { A_OUT + 4, A_OUT + 16 }; expect(res62_flow(w.e, A_RES, unal, 2, &ret, true) == 0 && w.e.mem.get<uint64_t>(A_OUT + 16) == 0, "slot 62: an unaligned output pointer -> not handled, nothing written");
      uint64_t user[2] = { 0x1000, A_OUT + 8 }; expect(res62_flow(w.e, A_RES, user, 2, &ret, true) == 0 && w.e.mem.get<uint64_t>(A_OUT + 8) == 0, "slot 62: a pointer outside the kernel half -> not handled, nothing written");
      uint64_t nul[2] = { A_OUT, 0 }; expect(res62_flow(w.e, A_RES, nul, 2, &ret, true) == 0 && w.e.mem.get<uint64_t>(A_OUT) == 0, "slot 62: a NULL output pointer -> not handled, nothing written");
      uint64_t ok[2] = { A_OUT, A_OUT + 8 };
      expect(res62_flow(w.e, A_RES, ok, 2, &ret, true) == 1 && w.e.mem.get<uint64_t>(A_OUT) == 10240ull * 1440 && w.e.mem.get<uint64_t>(A_RES + kResOutAlloc) == 0 && w.e.mem.get<uint64_t>(A_RES + kResOutBpr) == 0,
             "slot 62: outputs land where the caller points (its locals), never by assumption in the resource's own fields");
      World v; res62_world(v);
      expect(res62_flow(v.e, A_RES, ok, 1, &ret, true) == 0 && res62_flow(v.e, A_RES, nullptr, 2, &ret, true) == 0 && res62_flow(v.e, A_RES, ok, 2, nullptr, true) == 0, "slot 62: a wrong argument count / NULLs -> not handled");
      v.e.factMask = 0; expect(res62_flow(v.e, A_RES, ok, 2, &ret, true) == 0, "slot 62: without the RESOURCE fact -> not handled");
      v.e.factMask = kNeedFacts; v.e.latch = false; expect(res62_flow(v.e, A_RES, ok, 2, &ret, true) == 0 && v.e.mem.get<uint64_t>(A_OUT) == 0, "slot 62: latch OFF -> not handled, nothing written"); }

    // stamps
    { World w; StampsOut o {}; const uint32_t st = stamps_flow(w.e, &o);
      expect(st == kOk && o.count == 4 && o.completed == 10 && o.submitted == 10 && !o.hazard && o.clamped == 4, "stamps: the three named words, no hazard");
      w.e.mem.put<uint32_t>(A_EM + kEmSub, 11); StampsOut p {}; stamps_flow(w.e, &p); expect(p.hazard, "stamps: submitted ahead of completed is flagged");
      w.e.mem.put<uint32_t>(A_EM + kEmCount, 1000); StampsOut q {}; stamps_flow(w.e, &q); expect(q.count == 1000 && q.clamped == kStampMaxSlots, "stamps: a corrupt count is reported raw and clamped");
      w.e.classes[A_EM] = "IOAccelEventMachineFast2"; StampsOut r {}; expect_u("stamps: the wrong class is refused", stamps_flow(w.e, &r), kNoEventMachine);
      w.e.classes.clear(); expect_u("stamps: an unknown object is refused", stamps_flow(w.e, &r), kNoEventMachine);
      w.e.accelAddr = 0; expect_u("stamps: no accelerator", stamps_flow(w.e, &r), kNoAccel);
      World v; v.e.mem.put<uint64_t>(A_ACCEL + kAccelEm, 0x1000); StampsOut s {}; expect_u("stamps: event machine pointer not a kernel pointer", stamps_flow(v.e, &s), kNoEventMachine); }


    // =========================================================================================================================================================
    // N. 0.0.615 (the HIGH review of 0.0.614): P1 pci gate, P2 withdraw, P3 NULL pipe, G1 crash-loop guard, W1 flags word
    // =========================================================================================================================================================
    // ---- P1: the aux walk's PCI device only with the latch AND the resource facts on
    expect(!pci_admit(false, 0) && !pci_admit(false, kNeedFacts) && !pci_admit(true, 0) && !pci_admit(true, N48_FACT_RESOURCE) && !pci_admit(true, N48_FACT_RESOURCE | N48_FACT_SYSMEMORY) && pci_admit(true, kNeedFacts) &&
           pci_admit(true, kNeedFacts | 0x10u), "P1: pci_admit is latch AND all three resource facts, nothing less");
    { FakeEnv e; e.latch = true; e.factMask = 0;
      expect(!pci_admit_flow(e), "P1: facts off -> the ops hook answers NULL (a requestProbe before pipeadopt reaches the nub, finds nothing: 0.0.612 behaviour)");
      e.factMask = N48_FACT_RESOURCE | N48_FACT_VIDMEMORY; expect(!pci_admit_flow(e), "P1: two of the three facts -> NULL");
      e.factMask = kNeedFacts; expect(pci_admit_flow(e), "P1: all three facts -> the PCI device");
      e.latch = false; expect(!pci_admit_flow(e), "P1: latch OFF (boot-arg absent) -> NULL whatever the mask (0.0.614's OFF behaviour)"); }
    { // ORDERING on the real adopt flow: at the moment requestProbe runs the PCI device is admitted; before adopt it is not
      FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe(); e.factMask = 0;
      expect(!pci_admit_flow(e), "P1 order: before adopt the PCI device is not handed out");
      expect_u("P1 order: adopt OK", adopt_flow(e), kOk);
      expect(e.has("request_probe") && e.pciAtProbe, "P1 order: adopt's requestProbe ran with the PCI device admitted (facts on BEFORE it)"); }
    { FakeEnv e; e.canAddFacts = false; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe(); e.factMask = 0;
      expect_u("P1 order: facts that stay off", adopt_flow(e), kFactsOff); expect(!e.has("request_probe") && !pci_admit_flow(e), "P1 order: no requestProbe and no PCI device"); }
    { // somebody else's requestProbe with the facts off: the walk is handed NULL
      FakeEnv e; e.factMask = 0; (void)e.request_probe(); expect(!e.pciAtProbe, "P1: a requestProbe that is not adopt's (facts off) sees NULL, not the PCI device"); }

    // ---- P2: withdraw disarms FIRST
    { FakeEnv e; e.armedFlag = true; e.armByte = 1; e.pipes = { A_PIPE, A_PIPE2 };
      withdraw_flow(e);
      expect(e.armByte == 0 && !e.armedFlag, "P2: a withdraw with the gate ARMED writes the gate byte back to 0 and clears the mirror");
      expect(e.idx("disarm") >= 0 && e.idx("disarm") < e.idx("forget_pipes") && e.idx("forget_pipes") < e.idx("note_withdraw"), "P2 order: disarm, THEN forget the pipes, then the note");
      expect(e.pipes.empty() && e.withdrawNotes == 1 && e.withdrawWasArmed, "P2: the pipe table is cleared and the note records that it was armed"); }
    { FakeEnv e; e.armedFlag = false; e.armByte = 0; e.pipes = { A_PIPE }; withdraw_flow(e);
      expect(e.count("disarm") == 1 && e.armByte == 0 && e.pipes.empty() && !e.withdrawWasArmed, "P2: an unarmed withdraw still runs the disarm (harmless) and forgets"); }
    { // the real order at the decision level: nothing forgotten while the byte could still be 1
      FakeEnv e; e.armedFlag = true; e.armByte = 1; e.pipes = { A_PIPE };
      e.log.clear(); withdraw_flow(e); expect(e.log.size() == 3 && e.log[0] == "disarm", "P2: exactly three steps, the disarm first"); }

    // ---- P3: a NULL pipe stored by the family gets its own status, a log, and no further step
    { expect(kNullPipe == 17 && kStatusCount == 18 && std::strcmp(status_name(kNullPipe), "unknown") != 0 && kNullPipe != kNoPipe && kNullPipe != kHasPipes, "P3: kNullPipe is a distinct, named status");
      PipeProbe n {}; n.accelOk = n.dmReadable = true; n.count = 1; n.nullPipe = true;
      expect_u("P3: pipe_verdict of a counted NULL pipe", pipe_verdict(n), kNullPipe);
      expect_u("P3: adopt_precheck of a counted NULL pipe", adopt_precheck(n), kNullPipe);
      PipeProbe z = n; z.count = 0; z.nullPipe = false; expect_u("P3: count 0 is still kNoPipe", pipe_verdict(z), kNoPipe);
      PipeProbe h = n; h.nullPipe = false; h.havePipe = false; expect_u("P3: a count without a readable pipe word stays kNoPipe", pipe_verdict(h), kNoPipe);
      expect(!pipe_ok(n), "P3: a NULL pipe is never a verified pipe"); }
    { // before adopt: the family already stored a NULL pipe (an earlier failed init): refuse, log, touch nothing
      FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.before.count = 1; e.before.nullPipe = true; e.after = good_probe();
      expect_u("P3: adopt over an earlier NULL pipe", adopt_flow(e), kNullPipe);
      expect(e.nullPipeNotes == 1 && !e.has("facts_add") && !e.has("request_probe") && !e.has("publish_caps") && !e.has("record_adopted") && e.adoptEnd == kNullPipe, "P3: the advice is logged once; no facts, no probe request, no capabilities, no record"); }
    { // after requestProbe: the family's init failed this time
      FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.before.count = 0; e.after = PipeProbe{}; e.after.accelOk = e.after.dmReadable = true; e.after.count = 1; e.after.nullPipe = true;
      expect_u("P3: adopt whose requestProbe stored a NULL pipe", adopt_flow(e), kNullPipe);
      expect(e.nullPipeNotes == 1 && e.has("request_probe") && !e.has("publish_caps") && !e.has("record_adopted"), "P3: logged, and nothing published or recorded after the NULL pipe"); }
    { FakeEnv e; e.before.accelOk = e.before.dmReadable = true; e.after = good_probe(); (void)adopt_flow(e); expect(e.nullPipeNotes == 0, "P3: a good adopt logs no NULL-pipe advice"); }
    { // arm with a NULL pipe: refused
      FakeEnv e; e.armedFlag = false; e.factMask = kNeedFacts; e.current = PipeProbe{}; e.current.accelOk = e.current.dmReadable = true; e.current.count = 1; e.current.nullPipe = true;
      expect_u("P3: arm 1 over a NULL pipe is refused", arm_flow(e, 1), kNoPipe); expect(!e.has("arm_write1"), "P3: the gate byte is not written"); }

    // ---- G1: the crash-loop guard (slot 267 x3 within 120 s with no perform since the arm)
    expect(kGuardCalls == 3 && kGuardWindowNs == 120ull * 1000000000ull, "G1: three calls, 120 s");
    expect(!guard_trip(1, 5, 1) && guard_trip(2, 120000000001ull, 1) && !guard_trip(2, 120000000002ull, 1) && !guard_trip(2, 1, 5), "G1/K3: guard_trip (<= window, never before three calls, never a backwards clock; 0.0.617: no 'performed' exemption)");
    const uint64_t S = 1000000000ull;
    auto init267 = [&](World &w, uint64_t tNs, uint64_t *ret) { w.e.now = tNs; w.e.clockStep = 0; uint64_t a[2] = { 0, A_RES }; return hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, a, 2, ret); };
    { World w; w.e.mem.put<uint8_t>(A_ACCEL + kAccelPipeGate, 1); w.e.armByte = 1; uint64_t ret = 0;
      expect(init267(w, 1 * S, &ret) == 1 && w.e.armedFlag, "G1: call 1 handled, still armed");
      expect(init267(w, 50 * S, &ret) == 1 && w.e.armedFlag, "G1: call 2 still armed");
      expect(init267(w, 100 * S, &ret) == 1 && ret != 0, "G1: call 3 is still handled (the stand-in object is returned)");
      expect(!w.e.armedFlag && w.e.armByte == 0 && w.e.autoDisarms == 1 && w.e.count("disarm") == 1, "G1: the THIRD call within 120 s with no perform auto-disarms (gate byte 0, mirror cleared, noted once)");
      expect(init267(w, 101 * S, &ret) == 1 && w.e.count("disarm") == 1 && w.e.autoDisarms == 1, "G1: once disarmed a further call neither counts nor disarms again"); }
    { World w; w.e.armByte = 1; uint64_t ret = 0; init267(w, 1 * S, &ret); init267(w, 70 * S, &ret); init267(w, 140 * S, &ret);
      expect(w.e.armedFlag && w.e.autoDisarms == 0, "G1: calls at 1 s, 70 s, 140 s: the third is 139 s after the first -> no trip");
      init267(w, 150 * S, &ret); expect(!w.e.armedFlag && w.e.autoDisarms == 1, "G1: the window slides: 70 s, 140 s, 150 s are three within 80 s -> trip"); }
    { World w; w.e.armByte = 1; uint64_t ret = 0; init267(w, 1 * S, &ret); init267(w, 2 * S, &ret); init267(w, 121 * S, &ret); expect(!w.e.armedFlag, "G1: exactly 120 s from the oldest of the three -> trip (<= window)");
      World v; v.e.armByte = 1; init267(v, 1 * S, &ret); init267(v, 2 * S, &ret); init267(v, 122 * S, &ret); expect(v.e.armedFlag && v.e.autoDisarms == 0, "G1: 121 s -> no trip"); }
    { // K3 (0.0.617): a perform since the arm does NOT exempt: the m11h5-3 replay (performs, then three starts in 60 s)
      World w; w.e.armByte = 1; uint64_t ret = 0; init267(w, 1 * S, &ret);
      w.e.now = 3 * S; expect(w.perform(&ret) == 1 && w.e.cg.performed == 1, "K3: a performTransaction reaches the pipe (the crashed WindowServer had presented 40,016 frames)");
      w.e.now = 4 * S; (void)w.perform(&ret);
      init267(w, 10 * S, &ret); init267(w, 40 * S, &ret); init267(w, 70 * S, &ret);
      expect(!w.e.armedFlag && w.e.armByte == 0 && w.e.autoDisarms == 1 && w.e.autoCause == kAdGuard, "K3 replay: performs, then 3 x slot 267 within 60 s -> TRIP (cause: the restart loop) even though performs happened");
      uint64_t r2 = 9; w.e.now = 71 * S; (void)w.perform(&r2); expect(w.e.reasons[kPfDisarmed] >= 1, "K3: after the trip a perform is refused as disarmed"); }
    { // K3: a healthy WindowServer start (1-2 calls) with presents never trips
      World w; w.e.armByte = 1; uint64_t ret = 0; init267(w, 1 * S, &ret); w.e.now = 2 * S; (void)w.perform(&ret); init267(w, 3 * S, &ret); for (int i = 0; i < 50; ++i) { w.e.now = (4 + i) * S; (void)w.perform(&ret); }
      expect(w.e.armedFlag && w.e.autoDisarms == 0, "K3: two slot-267 calls and many performs do not trip (a healthy start produces 1-2)"); }
    { // the counter resets on arm: 2 calls, re-arm, 2 calls -> no trip; the third after the re-arm trips
      World w; w.e.armByte = 1; uint64_t ret = 0; init267(w, 1 * S, &ret); init267(w, 2 * S, &ret);
      w.e.armedFlag = false; w.e.armByte = 0; w.e.before = good_probe(); w.e.layoutOk = true;
      expect_u("G1: re-arm", arm_flow(w.e, 1), kOk); expect(w.e.armedFlag, "G1: armed again");
      init267(w, 3 * S, &ret); init267(w, 4 * S, &ret); expect(w.e.armedFlag, "G1: two calls after the re-arm do not trip (the counter was reset on arm)");
      init267(w, 5 * S, &ret); expect(!w.e.armedFlag && w.e.autoDisarms == 1, "G1: the third after the re-arm trips"); }
    { // a perform BEFORE the re-arm does not protect the new arm
      World w; w.e.armByte = 1; uint64_t ret = 0; w.e.now = 1 * S; w.perform(&ret);
      w.e.armedFlag = false; w.e.armByte = 0; w.e.before = good_probe(); arm_flow(w.e, 1);
      init267(w, 2 * S, &ret); init267(w, 3 * S, &ret); init267(w, 4 * S, &ret); expect(!w.e.armedFlag, "G1: 'performed since the arm' - a perform before the arm does not count"); }
    { // disarmed: 267 calls are not counted and never disarm
      World w; w.e.armedFlag = false; w.e.armByte = 0; uint64_t ret = 0; for (uint64_t t = 1; t <= 5; ++t) init267(w, t * S, &ret);
      expect(w.e.count("disarm") == 0 && w.e.autoDisarms == 0 && w.e.cg.n == 0, "G1: while disarmed nothing is counted and nothing is disarmed"); }
    { // a pipe we do not know: untouched, uncounted
      World w; w.e.armByte = 1; w.e.pipes.clear(); uint64_t ret = 0; for (uint64_t t = 1; t <= 4; ++t) init267(w, t * S, &ret);
      expect(w.e.armedFlag && w.e.cg.n == 0, "G1: slot 267 on an unknown pipe is neither handled nor counted"); }
    { // latch OFF: nothing is handled, the guard is never touched
      World w; w.e.latch = false; uint64_t ret = 0; expect(init267(w, 1 * S, &ret) == 0 && w.e.cg.n == 0 && w.e.armedFlag, "G1: latch OFF -> not handled, the guard state untouched"); }
    { // the real ordering: the reset happens BEFORE the gate byte is written
      World w; w.e.armedFlag = false; w.e.armByte = 0; w.e.before = good_probe(); w.e.cg.n = 2; w.e.cg.performed = 1; arm_flow(w.e, 1);
      expect(w.e.cg.n == 0 && w.e.cg.performed == 0, "G1: arm resets the counter and the performed flag"); }

    // ---- K1: the WindowServer GPU client closed while armed -> auto-disarm (0.0.617; m11h5-3: the crash loop ran 120 s into the userspace watchdog with the pipe still armed)
    expect(ws_close_disarms(true, false) && !ws_close_disarms(true, true) && !ws_close_disarms(false, false) && !ws_close_disarms(false, true), "K1: only a NON-admin (uid-88) client disarms, and only with the latch ON");
    { World w; uint64_t ret = 0; w.e.now = 1 * S; w.e.clockStep = 0; (void)w.perform(&ret);
      expect(w.e.armedFlag && w.e.mdcache.e[0].state != 0u, "K1: setup - armed, one descriptor prepared by submit");
      expect(ws_client_closed_flow(w.e, false), "K1: the uid-88 client's close while armed is acted on");
      expect(!w.e.armedFlag && w.e.armByte == 0 && w.e.autoDisarms == 1 && w.e.autoCause == kAdWsClose, "K1: gate byte 0, mirror cleared, noted once with the WsClose cause");
      expect(w.e.complCount[A_MD] == 1 && w.e.mdcache.tornDown == 1, "K1: the descriptor cache is torn down (complete + release exactly once)");
      expect(w.e.idx("disarm") >= 0 && w.e.idx("disarm") < w.e.idx("md_complete") && w.e.idx("md_complete") < w.e.idx("note_autodisarm"), "K1: ORDER: disarm (mirror + byte) before the teardown before the note");
      const int reads0 = w.e.srcReads; uint64_t pr = 5; const int h = w.perform(&pr); expect(h == 1 && pr == 0 && w.e.reasons[kPfDisarmed] >= 1 && w.e.srcReads == reads0, "K1: a perform after the close is refused as disarmed, nothing is read");
      expect(!ws_client_closed_flow(w.e, false) && w.e.autoDisarms == 1 && w.e.count("disarm") == 1, "K1: closing again (stop after clientClose) is idempotent: nothing more is done or logged"); }
    { World w; expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag && w.e.autoDisarms == 0 && w.e.count("disarm") == 0, "K1: an ADMIN client (operator tools) closing never disarms"); }
    { World w; w.e.armedFlag = false; w.e.armByte = 0; expect(!ws_client_closed_flow(w.e, false) && w.e.autoDisarms == 0 && w.e.log.empty(), "K1: not armed -> nothing at all (no write, no log)"); }
    { World w; w.e.latch = false; expect(!ws_client_closed_flow(w.e, false) && w.e.armedFlag && w.e.log.empty(), "K1: latch OFF (boot-arg absent) -> nothing: 0.0.616 / 0.0.612 behaviour"); }
    { // the real ordering: arm_flow -> 267 -> submit -> perform copies -> client dies -> next perform refused
      World w; w.e.armedFlag = false; w.e.armByte = 0; w.e.before = good_probe(); w.e.layoutOk = true; uint64_t ret = 0;
      expect_u("K1: arm", arm_flow(w.e, 1), kOk); init267(w, 1 * S, &ret); w.e.now = 2 * S; w.e.clockStep = 0; (void)w.submit(); (void)w.perform(&ret);
      expect(w.e.reasons[kPfCopied] == 1, "K1: a frame was copied while armed");
      (void)ws_client_closed_flow(w.e, false); (void)w.perform(&ret);
      expect(w.e.reasons[kPfCopied] == 1 && w.e.armByte == 0, "K1: no frame is copied after the WindowServer client died"); }

    // ---- K2: the HUNG latch while armed -> disarm (same flow, cause Hung)
    { World w; w.e.hungFlag = true; uint64_t ret = 0; (void)ret;
      expect(hung_latched_flow(w.e) && !w.e.armedFlag && w.e.armByte == 0 && w.e.autoCause == kAdHung && w.e.autoDisarms == 1, "K2: HUNG while armed disarms and records the Hung cause");
      expect(w.e.complCount[A_MD] == 0 && w.e.mdcache.leaked == 1, "K2: under HUNG the prepared descriptor is LEAKED (never completed), the teardown does not wait"); }
    { World w; w.e.armedFlag = false; w.e.armByte = 0; expect(!hung_latched_flow(w.e) && w.e.log.empty(), "K2: HUNG while not armed: nothing"); }
    { World w; w.e.latch = false; expect(!hung_latched_flow(w.e) && w.e.armedFlag && w.e.log.empty(), "K2: latch OFF -> nothing"); }

    // ---- R1 (0.0.619): the operator restart window (`pipereload`): a deliberate WindowServer restart must not trip K1 / K3
    expect(kReloadWindowNs == 15ull * 1000000000ull, "R1: the window is 15 s");
    expect(!reload_live(0u, 100 * S, 100 * S) && reload_live(1u, 100 * S, 100 * S) && reload_live(1u, 100 * S, 100 * S + kReloadWindowNs - 1ull) && !reload_live(1u, 100 * S, 100 * S + kReloadWindowNs) &&
           !reload_live(1u, 100 * S, 100 * S + kReloadWindowNs + 1ull) && !reload_live(1u, 100 * S, 99 * S), "R1: live inside [open, open+15 s) only; exactly 15 s is expired; a clock that went backwards is NOT live (fails towards containment)");
    expect(reload_honoured(true, false) && !reload_honoured(true, true) && !reload_honoured(false, false) && !reload_honoured(false, true), "R1: HUNG precedence: a hung GPU never honours the window");
    expect(reload_state(false, false) == 0u && reload_state(false, true) == 0u && reload_state(true, false) == 1u && reload_state(true, true) == 2u, "R1: the state: closed / open / open with the close tolerated");
    { // the real order: open -> client closes (tolerated, still armed) -> the new client's first slot 267 closes the window (not counted) -> then everything is as in 0.0.618
      World w; w.e.armByte = 1; uint64_t ret = 0; w.e.now = 100 * S; w.e.clockStep = 0; uint64_t rem = 99;
      expect(reload_state_flow(w.e, &rem) == 0u && rem == 0u, "R1: before the verb the window is closed");
      reload_open_flow(w.e); expect(reload_state_flow(w.e, &rem) == 1u && rem == kReloadWindowNs && w.e.rwEv[kRwOpened] == 1, "R1: the verb opens it: state 1, the full 15 s left, logged once");
      w.e.now = 103 * S; expect(reload_state_flow(w.e, &rem) == 1u && rem == 12 * S, "R1: 12 s left after 3 s");
      expect(init267(w, 104 * S, &ret) == 1 && w.e.cg.n == 0 && w.e.armedFlag && reload_state_flow(w.e, &rem) == 1u && w.e.rwEv[kRwClosed267] == 0, "R1: a slot 267 BEFORE the close (the old WindowServer) is not counted and does NOT close the window");
      w.e.now = 105 * S; const bool dis = ws_client_closed_flow(w.e, false);
      expect(!dis && w.e.armedFlag && w.e.armByte == 1 && w.e.autoDisarms == 0 && w.e.count("disarm") == 0 && w.e.complCount[A_MD] == 0 && w.e.rwEv[kRwTolerated] == 1, "R1: the uid-88 close inside the window is tolerated: still armed, gate byte kept, nothing torn down, logged");
      expect(reload_state_flow(w.e, &rem) == 2u, "R1: state 2 after the tolerated close (waiting for the new client's first slot 267)");
      expect(init267(w, 107 * S, &ret) == 1 && ret != 0 && w.e.armedFlag && w.e.cg.n == 0 && w.e.rwEv[kRwClosed267] == 1 && reload_state_flow(w.e, &rem) == 0u && !w.e.rwin.open, "R1: the new client's first slot 267 (still armed) closes the window, is handled normally and is NOT counted");
      expect(w.e.rwSeq.size() == 3 && w.e.rwSeq[0] == kRwOpened && w.e.rwSeq[1] == kRwTolerated && w.e.rwSeq[2] == kRwClosed267, "R1: log order: opened, tolerated, closed");
      // one-shot: the window is gone, so 0.0.618 behaviour is back: the next close disarms, and K3 counts again
      World v; v.e.armByte = 1; v.e.now = 100 * S; v.e.clockStep = 0; reload_open_flow(v.e); (void)ws_client_closed_flow(v.e, false); init267(v, 101 * S, &ret);
      init267(v, 102 * S, &ret); init267(v, 103 * S, &ret); init267(v, 104 * S, &ret);
      expect(v.e.cg.n == 3 && !v.e.armedFlag && v.e.autoCause == kAdGuard, "R1: ONE-SHOT: after the window closed, the next three slot-267 calls inside 120 s trip K3 exactly as in 0.0.618");
      World x; x.e.armByte = 1; x.e.now = 100 * S; x.e.clockStep = 0; reload_open_flow(x.e); (void)ws_client_closed_flow(x.e, false); init267(x, 101 * S, &ret);
      x.e.now = 102 * S; expect(ws_client_closed_flow(x.e, false) && !x.e.armedFlag && x.e.autoCause == kAdWsClose, "R1: ONE-SHOT: a SECOND close after the window closed disarms (K1 unchanged)"); }
    { // OFF identity: a window that was never opened changes nothing; K1 and K3 behave as in 0.0.618
      World w; w.e.armByte = 1; w.e.now = 100 * S; w.e.clockStep = 0; uint64_t ret = 0;
      expect(ws_client_closed_flow(w.e, false) && !w.e.armedFlag && w.e.autoCause == kAdWsClose && w.e.rwEv[kRwTolerated] == 0 && w.e.rwEv[kRwExpired] == 0, "R1: no window opened -> K1 disarms as before, nothing is logged for the window");
      World v; v.e.armByte = 1; init267(v, 1 * S, &ret); init267(v, 2 * S, &ret); init267(v, 3 * S, &ret); expect(!v.e.armedFlag && v.e.autoCause == kAdGuard && v.e.cg.n == 3 && v.e.rwSeq.empty(), "R1: no window opened -> K3 counts and trips as before"); }
    { // the time bounds: a close at 15 s - 1 ns is tolerated; at exactly 15 s the window has expired and K1 disarms (the expiry is logged once)
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); w.e.now = 100 * S + kReloadWindowNs - 1ull;
      expect(!ws_client_closed_flow(w.e, false) && w.e.armedFlag && w.e.rwEv[kRwTolerated] == 1, "R1: a close 1 ns before the 15 s is tolerated");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); v.e.now = 100 * S + kReloadWindowNs;
      expect(ws_client_closed_flow(v.e, false) && !v.e.armedFlag && v.e.autoCause == kAdWsClose && v.e.rwEv[kRwTolerated] == 0 && v.e.rwEv[kRwExpired] == 1 && !v.e.rwin.open, "R1: a close at exactly 15 s: the window has expired -> K1 disarms; the expiry is logged and the window is shut");
      uint64_t rem = 0; expect(reload_state_flow(v.e, &rem) == 0u && v.e.rwEv[kRwExpired] == 1, "R1: an expired window is logged once, not on every look"); }
    { // expiry between the tolerated close and the new client's slot 267: the 267 is counted (the window no longer protects it) and the pipe stays armed
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0; reload_open_flow(w.e); w.e.now = 101 * S; (void)ws_client_closed_flow(w.e, false);
      init267(w, 116 * S, &ret); expect(w.e.armedFlag && w.e.cg.n == 1 && w.e.rwEv[kRwExpired] == 1 && w.e.rwEv[kRwClosed267] == 0, "R1: the first 267 after the 15 s is counted by K3 and the window expires (not 'closed by 267')"); }
    { // an ADMIN close (operator tools) neither disarms nor consumes the window
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t rem = 0; reload_open_flow(w.e);
      expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag && w.e.rwEv[kRwTolerated] == 0 && reload_state_flow(w.e, &rem) == 1u, "R1: an admin client's close leaves the window untouched (state 1, nothing tolerated)"); }
    { // the pipe must still be armed at the new client's slot 267 for the window to close
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0, rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, false);
      w.e.armedFlag = false; w.e.armByte = 0; init267(w, 102 * S, &ret);
      expect(w.e.rwEv[kRwClosed267] == 0 && reload_state_flow(w.e, &rem) == 2u && w.e.cg.n == 0, "R1: a slot 267 while NOT armed does not close the window and is not counted");
      w.e.armedFlag = true; w.e.armByte = 1; init267(w, 103 * S, &ret); expect(w.e.rwEv[kRwClosed267] == 1 && reload_state_flow(w.e, &rem) == 0u, "R1: the first slot 267 with the pipe armed closes it"); }
    { // not armed: the close tolerates nothing (the verb on a disarmed pipe is harmless) and the window stays open until its 15 s
      World w; w.e.armedFlag = false; w.e.armByte = 0; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t rem = 0; reload_open_flow(w.e);
      expect(!ws_client_closed_flow(w.e, false) && w.e.rwEv[kRwTolerated] == 0 && reload_state_flow(w.e, &rem) == 1u && w.e.autoDisarms == 0, "R1: a close on a disarmed pipe tolerates nothing and consumes nothing"); }
    { // HUNG precedence: K2 is unchanged, and while HUNG neither the close nor the 267 is honoured
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); w.e.hungFlag = true;
      expect(hung_latched_flow(w.e) && !w.e.armedFlag && w.e.autoCause == kAdHung && w.e.autoDisarms == 1 && !w.e.rwin.open, "R1: HUNG with the window open disarms (K2 unchanged) and shuts the window");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); v.e.hungFlag = true;
      expect(ws_client_closed_flow(v.e, false) && !v.e.armedFlag && v.e.autoCause == kAdWsClose && v.e.rwEv[kRwTolerated] == 0, "R1: a close while HUNG is NOT tolerated even inside the window");
      World u; u.e.armByte = 1; u.e.clockStep = 0; u.e.now = 100 * S; uint64_t ret = 0; reload_open_flow(u.e); u.e.hungFlag = true; init267(u, 101 * S, &ret);
      expect(u.e.cg.n == 1 && u.e.rwEv[kRwClosed267] == 0, "R1: a slot 267 while HUNG is counted by K3 (the window is not honoured)"); }
    { // every disarm shuts the window: arm 0, K3 trip (outside any window), withdraw
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); expect_u("R1: arm 0", arm_flow(w.e, 0), kOk); expect(!w.e.rwin.open && !w.e.rwin.closeSeen, "R1: `pipearm 0` shuts the window");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); withdraw_flow(v.e); expect(!v.e.rwin.open, "R1: withdraw shuts the window");
      World u; u.e.armByte = 1; u.e.before = good_probe(); u.e.layoutOk = true; u.e.clockStep = 0; u.e.now = 100 * S; reload_open_flow(u.e); u.e.armedFlag = false; u.e.armByte = 0;
      expect_u("R1: explicit arm 1", arm_flow(u.e, 1), kOk); expect(u.e.armedFlag, "R1: arm 1 arms (the window itself never arms)"); }
    { // re-issuing the verb restarts the 15 s and clears a stale 'close seen'
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, false); w.e.now = 110 * S; reload_open_flow(w.e);
      expect(reload_state_flow(w.e, &rem) == 1u && rem == kReloadWindowNs && w.e.rwin.opens == 2, "R1: a second `pipereload` restarts the window (state 1, a full 15 s)"); }
    { // the hook table path: latch OFF hands nothing to the window; slot 267 through hook_dispatch inside the window is handled and not counted
      World w; w.e.latch = false; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); uint64_t ret = 0;
      expect(!ws_client_closed_flow(w.e, false) == true && w.e.armedFlag && init267(w, 101 * S, &ret) == 0 && w.e.cg.n == 0 && w.e.rwEv[kRwTolerated] == 0, "R1: latch OFF -> the close and the hooks do nothing, window or not (0.0.616 behaviour)"); }

    // ---- K4: after an auto-disarm the arm stays refused until an explicit `pipearm 1`; the cause is exposed
    expect(auto_cause_name(kAdWsClose) != nullptr && std::string(auto_cause_name(kAdWsClose)).find("WindowServer GPU client closed while armed") != std::string::npos && std::string(auto_cause_name(kAdNone)) == "none", "K4: cause names");
    expect(disp_flags(true, true, false, true, true, true, 0) == 59 && disp_flags(true, true, false, true, true, true, kAdWsClose) == (59u | 64u | (2u << 8)) && disp_flags(true, true, false, true, true, true, kAdHung) == (59u | 64u | (3u << 8)) &&
           disp_flags(true, false, false, false, false, false, kAdCount) == 1u && disp_flags(true, false, true, false, false, false) == 5u, "K4: the flags word: bit 64 and the cause in bits 8..11 only with a cause; 0.0.616 bits unchanged");
    { World w; w.e.before = good_probe(); w.e.layoutOk = true; (void)ws_client_closed_flow(w.e, false); uint64_t ret = 0;
      for (uint64_t t = 1; t <= 5; ++t) { init267(w, 100 * S + t * S, &ret); (void)w.submit(); (void)w.perform(&ret); }
      expect(!w.e.armedFlag && w.e.armByte == 0 && w.e.count("arm_write1") == 0 && w.e.autoCause == kAdWsClose, "K4: no hook (267 / 277 / 279) re-arms: the gate stays 0 and the cause stays");
      w.e.writeOk = false; expect_u("K4: a FAILED arm 1 leaves the cause", arm_flow(w.e, 1), kWriteFailed); expect(w.e.autoCause == kAdWsClose && w.e.count("clear_autodisarm") == 0, "K4: failed arm keeps the auto-disarmed mark");
      w.e.writeOk = true; expect_u("K4: explicit arm 1", arm_flow(w.e, 1), kOk); expect(w.e.armedFlag && w.e.autoCause == 0 && w.e.count("clear_autodisarm") == 1, "K4: the explicit arm re-arms and clears the mark");
      expect_u("K4: arm 0 is always allowed", arm_flow(w.e, 0), kOk); }

    // ---- K6 (0.0.617): while a native client owns the scanout plane the v1 copy is skipped (no source read, no descriptor wired); it resumes by itself when the acquisition ends
    { World w; w.e.scanOwned = true; const int reads0 = w.e.srcReads, dw0 = w.e.dstWrites; w.e.reads = 0; uint64_t pr = 5; const int h = w.perform(&pr);
      expect(h == 1 && pr == 0 && w.e.reasons[kPfScanOwned] == 1 && w.e.reasons[kPfCopied] == 0 && w.e.srcReads == reads0 && w.e.dstWrites == dw0 && w.e.unheldReads == 0 && w.e.consoleUntouched(),
             "K6: perform while the plane is acquired: completes (success), reason ScanOwned, no source read, no console write");
      expect(w.e.reads == 0, "K6: ScanOwned is decided BEFORE any memory read (the transaction is not even looked at)");
      w.e.scanOwned = false; (void)w.submit(); pr = 5; (void)w.perform(&pr);
      expect(w.e.reasons[kPfCopied] == 1 && w.e.consoleUntouched() == false, "K6: after the release the very next transaction copies again, by itself (no re-arm, no verb)"); }
    { // disarmed wins over scan-owned (the arm state is the first question)
      World w; w.e.scanOwned = true; w.e.armedFlag = false; uint64_t pr = 5; (void)w.perform(&pr); expect(w.e.reasons[kPfDisarmed] == 1 && w.e.reasons[kPfScanOwned] == 0, "K6: a disarmed pipe reports Disarmed, not ScanOwned"); }
    { // no wiring while acquired; wiring resumes after
      gAutoPrep = false; World w; gAutoPrep = true; w.e.scanOwned = true; uint64_t sr = 99;
      expect(w.submit(&sr) == 1 && sr == kWillPerform, "K6: submit while acquired still answers 'will perform' (the transaction retires normally)");
      expect(w.e.count("md_prepare") == 0 && w.e.mdcache.prepared == 0 && w.e.scanSkipSubmits == 1, "K6: submit while acquired wires NOTHING (no retain, no prepare) and counts the skip");
      for (int i = 0; i < 5; ++i) (void)w.submit(); expect(w.e.count("md_prepare") == 0 && w.e.scanSkipSubmits == 6, "K6: still nothing wired after more submits while acquired");
      uint64_t pr = 5; (void)w.perform(&pr); expect(w.e.reasons[kPfScanOwned] == 1 && w.e.reasons[kPfNotPrepared] == 0, "K6: perform while acquired says ScanOwned (not a cache miss)");
      w.e.scanOwned = false; (void)w.submit(); expect(w.e.count("md_prepare") == 1 && w.e.mdcache.prepared == 1, "K6: after the release the next submit wires the descriptor");
      (void)w.perform(&pr); expect(w.e.reasons[kPfCopied] == 1, "K6: and perform copies"); }
    { // an already-prepared cache entry (prepared before the acquisition) is not used, and not torn down by the skip
      World w; expect(w.e.mdcache.prepared == 1, "K6: setup - one descriptor prepared before the acquisition"); w.e.scanOwned = true; uint64_t pr = 5; (void)w.perform(&pr); (void)w.submit();
      expect(w.e.complCount[A_MD] == 0 && w.e.srcReads == 0 && w.e.reasons[kPfScanOwned] == 1, "K6: acquisition neither reads nor completes an existing prepared entry");
      w.e.scanOwned = false; (void)w.perform(&pr); expect(w.e.reasons[kPfCopied] == 1, "K6: ... and the same entry serves the copy again after the release"); }
    // the transaction interval counters
    { World w; w.e.iv = Ival{}; w.e.clockStep = 0; w.e.armedFlag = true;   // (the constructor's own submit stamped `last`)
      w.e.now = 1000; (void)w.submit(); expect(w.e.iv.d.n == 0 && w.e.iv.last == 1000, "K6: the first submit records no interval, only its stamp");
      w.e.now = 1300; (void)w.submit(); w.e.now = 2000; (void)w.submit();
      expect(w.e.iv.d.n == 2 && w.e.iv.d.min == 300 && w.e.iv.d.max == 700 && dur_avg(w.e.iv.d) == 500, "K6: intervals 300 and 700 -> n 2, min 300, avg 500, max 700");
      w.e.scanOwned = true; w.e.now = 2100; (void)w.submit(); expect(w.e.iv.d.n == 3 && w.e.iv.d.min == 100, "K6: the interval is counted while the plane is owned too (it measures the transaction rate)");
      w.e.now = 1; (void)w.submit(); expect(w.e.iv.d.n == 3, "K6: a clock that went backwards records nothing");
      Ival v {}; ival_note(v, 50); ival_reset(v); expect(v.last == 0 && v.d.n == 0, "K6: ival_reset clears"); }
    { World w; w.e.iv = Ival{}; w.e.clockStep = 0; w.e.now = 10; (void)w.submit(); w.e.now = 20; (void)w.submit(); w.e.armedFlag = false; w.e.armByte = 0; w.e.before = good_probe(); w.e.layoutOk = true; expect(w.e.iv.d.n == 1, "K6: setup - one interval");
      expect_u("K6: arm", arm_flow(w.e, 1), kOk); expect(w.e.iv.d.n == 0 && w.e.iv.last == 0, "K6: the arm restarts the interval statistics"); }
    { // unknown pipe / latch off: no interval
      World w; w.e.iv = Ival{}; w.e.clockStep = 0; w.e.now = 10; w.e.latch = false; (void)w.submit(); expect(w.e.iv.last == 0, "K6: latch OFF: the interval is not touched"); }
    { const std::string flw = slurp(K + "src/amd/native_disp_flow.h"), gl = slurp(K + "src/amd/native_disp.cpp"), dcn = slurp(K + "src/dcn/navi48_dcn.cpp"), dhp = slurp(K + "src/dcn/navi48_dcn.hpp"), tcli = slurp(root + "/tools/pc/navi48test.c");
      const size_t p0 = flw.find("uint32_t perform_inner("), s0 = flw.find("uint32_t submit_prepare(");
      const size_t pa = flw.find("e.scan_active()", p0), pb = flw.find("plane0_locate(e, txn, &surf, &res)", p0), sa = flw.find("e.scan_active()", s0), sb = flw.find("plane0_locate(e, txn, &surf, &res)", s0);
      expect(p0 != std::string::npos && pa != std::string::npos && pa < pb && s0 != std::string::npos && sa != std::string::npos && sa < sb && count_of(flw, "e.scan_active()") == 2, "K6: both perform_inner and submit_prepare ask scan_active() BEFORE locating / reading / wiring anything, once each");
      if (!dcn.empty()) expect(dcn.find("bool scanActive() { return __atomic_load_n(&gScan.active, __ATOMIC_ACQUIRE) != 0u; }") != std::string::npos && dhp.find("bool scanActive();") != std::string::npos, "K6: n48dcn::scanActive is ONE atomic load of the acquisition flag (no lock, no register)");
      expect(gl.find("bool scan_active() { return n48dcn::scanActive(); }") != std::string::npos && gl.find("void note_scan_owned_submit() { __atomic_add_fetch(&gD.submitScanSkipped, 1ull, __ATOMIC_RELAXED); }") != std::string::npos &&
             gl.find("out[9] = n48dcn::scanActive() ? 1u : 0u;") != std::string::npos && gl.find("out[5] = gD.ivl.d.n; out[6] = gD.ivl.d.min; out[7] = n48disp::dur_avg(gD.ivl.d); out[8] = gD.ivl.d.max;") != std::string::npos &&
             gl.find("out[3] = gD.reasons[n48disp::kPfScanOwned]; out[4] = gD.submitScanSkipped;") != std::string::npos && gl.find("(int)n48dcn::scanActive()") != std::string::npos, "K6: the glue reads the acquisition, counts skipped submits, and shows page 3 and the stat line");
      if (!tcli.empty()) expect(tcli.find("} else if (strtoull(arg, NULL, 0) == 3) {\n                /* 0.0.617 (K6): page 3") != std::string::npos && tcli.find("perform skipped, scanout plane owned by a native client") != std::string::npos && tcli.find("auto-disarmed           :") != std::string::npos, "K6/K4: navi48test pipestat shows page 3 and the auto-disarmed cause");
    }

    // ---- V1/V2 (0.0.618): the vblank timestamps in the perform hook
    expect(kTxnVblTime == 0x178 && kTxnVblNext == 0x188 && kTxnSize == 0x1b8 && kVblCount == 9 && kLastAction == 90, "V1: the literal offsets of the two timestamp words (AMDRadeonX6000 executeTransaction 0xbdcdbe2 / 0xbdcdbed) and the action bound");
    // pure: the period of a raster, the time to the next vblank start, the unit conversion, the plan
    expect_u("V1: period 2720x1481 at 241.5 MHz", vbl_period_ns(2720, 1481, 241500000ull), 16680414ull);
    expect(vbl_period_ns(0, 1481, 241500000ull) == 0 && vbl_period_ns(2720, 0, 241500000ull) == 0 && vbl_period_ns(2720, 1481, 0) == 0, "V1: a zero total or clock has no period");
    expect(vbl_period_ns(2720, 1481, 4000000ull) == 0 && vbl_period_ns(100, 100, 4000000000ull) == 0, "V1: a period outside 1 ms .. 100 ms (a wrong clock) has no period");
    { const uint64_t P = 16680000ull; uint64_t d = 99;
      expect(vbl_delay_ns(1440, 0, 1440, 2720, 1481, P, &d) && d == 0, "V1: a sample exactly at the blank start is 0 ns from it");
      expect(vbl_delay_ns(0, 0, 1440, 2720, 1481, P, &d) && d == P * 1440ull / 1481ull, "V1: from line 0 the blank start is 1440 lines away");
      expect(vbl_delay_ns(1441, 0, 1440, 2720, 1481, P, &d) && d == P * 1480ull / 1481ull, "V1: one line past the blank start the NEXT start is 1480 lines away (the counter wraps at vTotal)");
      expect(vbl_delay_ns(1480, 2719, 1440, 2720, 1481, P, &d) && d > 0 && d < P, "V1: the last pixel of the frame is a fraction of a line + 1440 lines from the next start");
      expect(vbl_delay_ns(1440, 1360, 1440, 2720, 1481, P, &d) && d == P * (uint64_t)(2720ull * 1481ull - 1360ull) / (2720ull * 1481ull), "V1: past the start by half a line the next start is a frame minus half a line away");
      expect(!vbl_delay_ns(1481, 0, 1440, 2720, 1481, P, &d) && !vbl_delay_ns(0, 2720, 1440, 2720, 1481, P, &d) && !vbl_delay_ns(0, 0, 1481, 2720, 1481, P, &d) && !vbl_delay_ns(0, 0, 1440, 0, 1481, P, &d) && !vbl_delay_ns(0, 0, 1440, 2720, 0, P, &d) &&
             !vbl_delay_ns(0, 0, 1440, 2720, 1481, 0, &d) && !vbl_delay_ns(0, 0, 1440, 2720, 1481, P, nullptr), "V1: incoherent samples (position outside the raster, blank start outside it, zero totals, zero period) are refused"); }
    { uint64_t o = 0;
      expect(ns_to_abs(123456789ull, 1, 1, &o) && o == 123456789ull, "V1: x86 timebase 1:1 keeps the nanoseconds");
      expect(ns_to_abs(1000ull, 125, 3, &o) && o == 24ull, "V1: a 125/3 timebase (ns = abs * 125 / 3, Apple silicon) turns 1000 ns into 24 ticks (abs = ns * denom / numer)");
      expect(ns_to_abs(1000ull, 3, 125, &o) && o == 41666ull, "V1: the inverse ratio (ns = abs * 3 / 125): 1000 ns = 41666 ticks");
      expect(ns_to_abs(16680000ull, 3, 125, &o) && o == 16680000ull / 3 * 125 + (16680000ull % 3) * 125 / 3, "V1: the split conversion equals the exact arithmetic");
      expect(!ns_to_abs(1000, 0, 1, &o) && !ns_to_abs(1000, 1, 0, &o) && !ns_to_abs(~0ull, 1, 2, &o) && !ns_to_abs(1, 1, 1, nullptr), "V1: zero ratio words, overflow and a null output are refused");
      const VblPlan p = vbl_plan(1000000000ull, 5000000ull, 16680000ull, 1, 1);
      expect(p.ok && p.t == 1005000000ull && p.next == 1005000000ull + 16680000ull && p.periodAbs == 16680000ull, "V1: t = now + delay, next = t + one period");
      expect(!vbl_plan(1000, 5, 0, 1, 1).ok && !vbl_plan(1000, 5, 1, 125, 3).ok, "V1: a period that is 0 ns or converts to 0 ticks is NEVER planned (P = 0 is not written)");
      expect(!vbl_plan(~0ull - 3, 5, 16680000ull, 1, 1).ok && !vbl_plan(~0ull - 10000000ull, 5, 16680000ull, 1, 1).ok, "V1: a plan that would wrap is refused"); }
    // the flow: both paths, the order, the identity, every refusal writes nothing
    auto vblWorld = [](World &w) { w.e.txnClass[A_TXN] = kTxnClass; w.e.mem.put<uint64_t>(A_TXN + 0x180, 0x1111222233334444ull); w.e.mem.put<uint64_t>(A_TXN + 0x190, 0x5555666677778888ull); w.e.mem.put<uint64_t>(A_TXN + 0x1a0, 0x99aabbccddeeff00ull); };
    auto word = [](World &w, uint32_t off) { uint64_t v = 0; w.e.mem.rd(A_TXN + off, &v, 8); return v; };
    { World w; vblWorld(w); uint64_t pr = 5; const int h = w.perform(&pr);
      expect(h == 1 && pr == 0 && w.e.reasons[kPfCopied] == 1, "V1: setup - the copy path ran");
      expect(word(w, kTxnVblTime) == 1000000000ull + 5000000ull && word(w, kTxnVblNext) == 1000000000ull + 5000000ull + 16680000ull, "V1: copy path: txn+0x178 = next vblank, txn+0x188 = that + one period, written before perform returns");
      expect(word(w, 0x180) == 0x1111222233334444ull && word(w, 0x190) == 0x5555666677778888ull && word(w, 0x1a0) == 0x99aabbccddeeff00ull, "V1: +0x180, +0x190 and +0x1a0 are untouched (+0x190's meaning is not established)");
      expect(w.e.vblNotes[kVblWrote] == 1 && w.e.lastPlan618.ok && w.e.lastVblPeriod == 16680000ull && w.e.lastVblDelay == 5000000ull, "V1: the write is counted with the plan");
      const int sr = w.e.idx("src_read"), vsm = w.e.idx("vbl_sample"), wt = w.e.idx("wr_t"), wn = w.e.idx("wr_next");
      expect(sr >= 0 && vsm > sr && wt > vsm && wn > wt && w.e.count("wr_t") == 1 && w.e.count("wr_next") == 1, "V1: ORDER: the copy, then the timing sample, then +0x178, then +0x188 (the sample is taken after the copy so it is the vblank the frame is shown at)");
      w.e.vs.delayNs = 7000000ull; w.e.vs.nowAbs = 2000000000ull; (void)w.submit(); (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 2007000000ull && word(w, kTxnVblNext) == 2007000000ull + 16680000ull && w.e.vblNotes[kVblWrote] == 2, "V1: every frame rewrites the words from a fresh sample"); }
    { World w; vblWorld(w); w.e.scanOwned = true; uint64_t pr = 5; const int h = w.perform(&pr);
      expect(h == 1 && pr == 0 && w.e.reasons[kPfScanOwned] == 1 && w.e.reasons[kPfCopied] == 0 && w.e.consoleUntouched(), "V1: setup - the scan-owned path ran, nothing was copied");
      expect(word(w, kTxnVblTime) == 1005000000ull && word(w, kTxnVblNext) == 1005000000ull + 16680000ull && w.e.vblNotes[kVblWrote] == 1, "V1: scan-owned path: the timestamps are written too, before perform returns");
      expect(w.e.idx("vbl_sample") >= 0 && w.e.idx("wr_t") > w.e.idx("vbl_sample") && w.e.idx("wr_next") > w.e.idx("wr_t") && w.e.count("src_read") == 0, "V1: ORDER on the scan-owned path: sample, +0x178, +0x188"); }
    { World w; vblWorld(w); w.e.vs.numer = 125; w.e.vs.denom = 3; uint64_t pr = 5; (void)w.perform(&pr); uint64_t d = 0, p = 0;
      expect(ns_to_abs(5000000ull, 125, 3, &d) && ns_to_abs(16680000ull, 125, 3, &p) && word(w, kTxnVblTime) == 1000000000ull + d && word(w, kTxnVblNext) == 1000000000ull + d + p && p != 16680000ull, "V1: a non-1:1 timebase converts the delay and the period"); }
    { World w; vblWorld(w); w.e.vblOnFlag = false; uint64_t pr = 5; (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 0 && word(w, kTxnVblNext) == 0 && w.e.vblSamples == 0 && w.e.count("wr_t") == 0 && w.e.vblNotes[kVblOff] == 1 && w.e.reasons[kPfCopied] == 1, "V2: switch OFF: nothing is sampled or written, the copy is unchanged (0.0.617 behaviour)");
      w.e.vblOnFlag = true; (void)w.submit(); (void)w.perform(&pr); expect(word(w, kTxnVblTime) != 0 && w.e.vblNotes[kVblWrote] == 1, "V2: switching ON takes effect on the next frame (no reboot)"); }
    { World w; vblWorld(w); w.e.latch = false; uint64_t pr = 5; const int h = w.perform(&pr); expect(h == 0 && word(w, kTxnVblTime) == 0 && w.e.vblSamples == 0, "V2: latch OFF: the hook does not handle the call and nothing is written (0.0.617)"); }
    { World w; vblWorld(w); w.e.armedFlag = false; uint64_t pr = 5; (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 0 && w.e.vblSamples == 0 && w.e.vblNotes[kVblDisarmed] == 1 && w.e.reasons[kPfDisarmed] == 1, "V1: a disarmed pipe writes nothing"); }
    { World w; vblWorld(w); w.e.txnClass[A_TXN] = "IOAccelResource2"; uint64_t pr = 5; (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 0 && word(w, kTxnVblNext) == 0 && w.e.vblSamples == 0 && w.e.vblNotes[kVblNotTxn] == 1, "V1: identity: an object that is not an IOAccelDisplayPipeTransaction2 is never written (and no register is sampled)");
      World x; uint64_t p2 = 5; (void)x.perform(&p2); expect(word(x, kTxnVblTime) == 0 && x.e.vblNotes[kVblNotTxn] == 1, "V1: identity: an unknown class (none recorded) is never written"); }
    { World w; vblWorld(w); w.e.hungFlag = true; uint64_t pr = 5; (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 0 && w.e.vblSamples == 0 && w.e.vblNotes[kVblHung] == 1, "V1: HUNG: no sample (no register read) and no write"); }
    { World w; vblWorld(w); w.e.sampleOk = false; uint64_t pr = 5; (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 0 && word(w, kTxnVblNext) == 0 && w.e.vblNotes[kVblNoTiming] == 1 && w.e.reasons[kPfCopied] == 1, "V1: no coherent timing sample: nothing is written, the frame is still copied"); }
    { World w; vblWorld(w); w.e.vs.periodNs = 0; uint64_t pr = 5; (void)w.perform(&pr);
      expect(word(w, kTxnVblTime) == 0 && word(w, kTxnVblNext) == 0 && w.e.vblNotes[kVblBadMath] == 1, "V1: a zero period is NEVER written");
      World x; vblWorld(x); x.e.vs.numer = 0; (void)x.perform(&pr); expect(word(x, kTxnVblTime) == 0 && x.e.vblNotes[kVblBadMath] == 1, "V1: a zero timebase word is never used"); }
    { FakeEnv e; e.armedFlag = true; e.vblOnFlag = true; uint64_t bad = 0x1000ull; expect(vbl_stamp_flow(e, bad, kPfCopied) == kVblBadTxn && e.vblSamples == 0, "V1: a transaction that is not a kernel pointer is refused before anything is read");
      expect(vbl_stamp_flow(e, A_TXN, kPfCopied) == kVblNotTxn, "V1: an unknown object is refused"); }
    { const std::string flw = slurp(K + "src/amd/native_disp_flow.h"), gl = slurp(K + "src/amd/native_disp.cpp"), dcn = slurp(K + "src/dcn/navi48_dcn.cpp"), dhp = slurp(K + "src/dcn/navi48_dcn.hpp"), tcli = slurp(root + "/tools/pc/navi48test.c");
      const size_t pf = flw.find("uint32_t perform_flow("), pi = flw.find("perform_inner(e, txn, &bytes)", pf), pv = flw.find("vbl_stamp_flow(e, txn, r)", pf), pn = flw.find("e.note_perform(r, bytes", pf);
      expect(pf != std::string::npos && pi != std::string::npos && pv != std::string::npos && pn != std::string::npos && pi < pv && pv < pn && count_of(flw, "vbl_stamp_flow(e, txn") == 1, "V1: perform_flow stamps once, after perform_inner (both of its paths) and before it returns");
      const size_t vf = flw.find("uint32_t vbl_stamp_flow("), vo = flw.find("e.vbl_on()", vf), vc = flw.find("e.class_derives(txn, kTxnClass)", vf), vh = flw.find("e.hung()", vf), vs2 = flw.find("e.vbl_sample(&s)", vf), vw = flw.find("e.wr64(txn + kTxnVblTime", vf);
      expect(vf != std::string::npos && vo > vf && vc > vo && vh > vc && vs2 > vh && vw > vs2 && count_of(flw, "e.wr64(txn + kTxn") == 2 && flw.find("kTxnVblNext, p.next") != std::string::npos, "V1: switch, identity, HUNG, sample, plan, then the two writes (and only those two)");
      expect(gl.find("bool vbl_on() { return __atomic_load_n(&gD.vblOff, __ATOMIC_ACQUIRE) == 0u; }") != std::string::npos && gl.find("__atomic_store_n(&gD.vblOff, arg ? 0u : 1u, __ATOMIC_RELEASE);") != std::string::npos &&
             gl.find("action == n48disp::kActVbl") != std::string::npos && gl.find("pipe stat page 4 (vblank timestamps)") != std::string::npos && gl.find("arg == 4ull") != std::string::npos, "V2: the switch defaults ON (zero-initialised vblOff), `pipevbl 0|1` toggles it live, stat page 4 shows it");
      expect(gl.find("nanoseconds_to_absolutetime(1000000000ull, &perSec);") != std::string::npos && gl.find("s->numer = 1000000000u; s->denom = (uint32_t)perSec;") != std::string::npos && gl.find("strcmp(c, name) == 0") != std::string::npos && gl.find("for (uint32_t depth = 0; mc && depth < 12u; ++depth, mc = mc->getSuperClass()) {") != std::string::npos, "V1: the glue reads the real timebase and walks the superclass chain for the identity");
      const size_t d0 = dcn.find("bool vblSample("), d1 = dcn.find("\n}\n", d0);
      if (!dcn.empty()) { const std::string body = d0 == std::string::npos ? std::string() : dcn.substr(d0, d1 - d0);
        expect(!body.empty() && body.find("wreg") == std::string::npos && body.find("WREG") == std::string::npos && body.find("dcn_wreg") == std::string::npos && body.find("gDcn") == std::string::npos && body.find("dcn41_otg_get_scanoutpos(&gLr.d") != std::string::npos && body.find("modeTrialBusy()") != std::string::npos,
               "V1: n48dcn::vblSample reads through the read-only device only (no wreg, no bound device), and skips while a mode trial runs");
        expect(dcn.find("dcn41_otg_get_totals(d, (uint32_t)otg, &ht1, &vt1)") != std::string::npos && dhp.find("bool vblSample(uint64_t *periodNs, uint64_t *delayNs, uint64_t *nowAbs);") != std::string::npos, "V1: the totals come from the OTG and the sample is declared"); }
      if (!tcli.empty()) expect(tcli.find("in = 89;") != std::string::npos && tcli.find("\"pipevbl\"") != std::string::npos && tcli.find("page 4 - kernel out[3..12] arrive as out[6..15] here") != std::string::npos && tcli.find("pipevbl [0|1]") != std::string::npos, "V2: navi48test has pipevbl (action 89) and pipestat page 4");
    }

    // ---- W1: the flags word carries the BAR0 write-combined bit
    expect(disp_flags(true, true, true, true, true, true) == 63 && disp_flags(false, false, false, false, false, false) == 0 && disp_flags(true, false, false, false, false, true) == 33 && disp_flags(false, false, false, false, false, true) == 32 &&
           disp_flags(true, true, false, false, false, false) == 3 && disp_flags(false, false, true, false, false, false) == 4 && disp_flags(false, false, false, true, false, false) == 8 && disp_flags(false, false, false, false, true, false) == 16,
           "W1: disp_flags: 1 on, 2 adopted, 4 armed, 8 capabilities, 16 shortcut, 32 BAR0 write-combined");
    expect(kFlagOn == 1 && kFlagAdopted == 2 && kFlagArmed == 4 && kFlagCaps == 8 && kFlagShortcut == 16 && kFlagBar0Wc == 32, "W1: the flag bits are the published ones (bits 1..16 unchanged from 0.0.614)");

    // =========================================================================================================================================================
    // S. source pins on the kernel glue
    // =========================================================================================================================================================
    const std::string glue = slurp(K + "src/amd/native_disp.cpp"), ghdr = slurp(K + "src/amd/native_disp.h"), nub = slurp(K + "src/Navi48MetalNub.cpp"), brg = slurp(K + "src/Navi48Bringup.cpp"),
                      ucl = slurp(K + "src/Navi48UserClient.cpp"), bhpp = slurp(K + "src/Navi48Bringup.hpp"), cli = slurp(root + "/tools/pc/navi48test.c"), plist = slurp(K + "Info.plist"),
                      pure = slurp(K + "src/amd/native_disp_pure.h"), flow = slurp(K + "src/amd/native_disp_flow.h"), ops = slurp(K + "src/Navi48MetalOps.h"), s1ch = slurp(K + "src/amd/native_s1c.h");
    expect(!glue.empty() && !ghdr.empty() && !nub.empty() && !brg.empty() && !ucl.empty() && !cli.empty(), "the glue sources are present");
    if (!glue.empty()) {
        // the kernel environment runs the real flows: every entry point goes through a flow from native_disp_flow.h
        expect(count_of(glue, "adopt_flow(env)") == 1 && count_of(glue, "arm_flow(env,") == 1 && count_of(glue, "hook_dispatch(env,") == 1 && count_of(glue, "res62_flow(env,") == 1 && count_of(glue, "stamps_flow(env,") == 1,
               "the glue runs adopt, arm, the hook dispatcher, slot 62 and stamps through the tested flows, once each");
        expect(glue.find("#include \"native_disp_flow.h\"") != std::string::npos, "the glue includes the flow header");
        expect(glue.find("PE_parse_boot_argn(\"navi48-metal-disp\"") != std::string::npos && count_of(glue, "PE_parse_boot_argn") == 1, "the boot-arg navi48-metal-disp is the only boot-arg the display glue reads, and it is read once");
        expect(glue.find("OSCompareAndSwap(n48disp::kLatchUnset") != std::string::npos, "the boot-arg is latched once (first writer wins)");
        expect(glue.find("n48disp::latch_value(") != std::string::npos && glue.find("n48disp::latch_is_on(") != std::string::npos, "the latch uses the pure latch functions");
        expect(glue.find("setProperty(\"IOAccelDisplayPipeCapabilities\"") != std::string::npos && glue.find("DisplayPipeSupported") != std::string::npos && glue.find("TransactionsSupported") != std::string::npos && glue.find("kOSBooleanTrue") != std::string::npos,
               "the capabilities dictionary carries both keys as OSBoolean true on the accelerator");
        expect(glue.find("->requestProbe(1)") != std::string::npos && count_of(glue, "->requestProbe(") == 1, "the only probe request is accel->requestProbe(1)");
        expect(glue.find("\"Navi48Accelerator\"") != std::string::npos && glue.find("\"Navi48DisplayMachine\"") != std::string::npos && glue.find("\"Navi48DisplayPipe\"") != std::string::npos && flow.find("\"Navi48EventMachine\"") != std::string::npos,
               "the positive controls name the four aux classes");
        expect(glue.find("kAccelPipeGate") != std::string::npos && glue.find("n48disp::accel_layout_ok(") != std::string::npos, "arm writes the config byte only through the accelerator's positive controls");
        expect(glue.find("n48disp::dur_note(") != std::string::npos && glue.find("clock_get_uptime") != std::string::npos, "the perform duration is measured");
        expect(glue.find("N48_TR_DISPPIPE") != std::string::npos && glue.find("N48_TR_DM_START") != std::string::npos, "the aux trace events of the pipe and the display-machine walk are recorded");
        expect(glue.find("standin_fill") == std::string::npos && flow.find("standin_fill(obj)") != std::string::npos, "the glue does not hand-roll the stand-in object (the flow calls standin_fill)");
        expect(glue.find("IOLock") == std::string::npos, "no lock is taken anywhere in the display glue (the hook paths are atomics only)");
        expect(glue.find("n48disp::verb_args_ok(action, arg)") != std::string::npos, "the verbs check their legal arguments with the exemption table's own function");
        expect(glue.find("if (!wr8(a, v) || !rd8(a, &back) || back != v) return false;") != std::string::npos, "arm's byte write is read back before the armed mirror follows it");
        { const size_t d0 = glue.find("void disarm() {"), m = glue.find("__atomic_store_n(&gD.armed, 0u, __ATOMIC_RELEASE);", d0), w = glue.find("(void)wr8(a, 0u);", d0);
          expect(d0 != std::string::npos && m != std::string::npos && w != std::string::npos && m < w, "disarm clears the armed mirror BEFORE it writes the gate byte"); }
        expect(glue.find("if (gD.accel) return gD.accel->getProvider() == nub;") != std::string::npos && glue.find("if (s->getProvider() != nub) { s->release(); return false; }") != std::string::npos, "the accelerator must be a child of OUR published nub");
        expect(glue.find("if (v && !gD.scratch) return false;") != std::string::npos, "the gate is never opened without the row buffer");
        expect(glue.find("shortcutOff") != std::string::npos && glue.find("rdy_shortcut() { return __atomic_load_n(&gD.shortcutOff, __ATOMIC_ACQUIRE) == 0u; }") != std::string::npos, "the shortcut switch defaults ON");
        // 0.0.615
        expect(glue.find("n48disp::pci_admit_flow(env) ? static_cast<void *>(navi48_bringup_pci()) : nullptr;") != std::string::npos && glue.find("n48disp_latched_on() ? static_cast<void *>(navi48_bringup_pci())") == std::string::npos, "P1: pci_device answers through pci_admit_flow (latch AND facts)");
        { const size_t w0 = glue.find("void n48disp_on_withdraw(void) {"), wf = glue.find("n48disp::withdraw_flow(env);", w0), rel = glue.find("a->release();", w0);
          expect(w0 != std::string::npos && wf != std::string::npos && rel != std::string::npos && wf < rel, "P2: on_withdraw runs withdraw_flow (disarm first) BEFORE the accelerator reference is released"); }
        { const size_t f0 = glue.find("void forget_pipes() {"), d0 = glue.find("void disarm() {"); expect(f0 != std::string::npos && d0 != std::string::npos && glue.find("if (rd8(a, &cur) && cur <= 1u) { (void)wr8(a, 0u);", d0) != std::string::npos, "P2: the glue's disarm still writes the gate byte back to 0"); }
        expect(glue.find("p.nullPipe = true;") != std::string::npos && glue.find("pipe0 == 0ull") != std::string::npos, "P3: the kernel probe marks a counted-but-NULL pipe");
        expect(glue.find("\"adopt: NULL pipe stored by the family; reboot before any WindowServer restart\"") != std::string::npos, "P3: the log line names the reboot advice");
        expect(glue.find("\"auto-disarm: %s; the gate byte is written back to 0") != std::string::npos && glue.find("n48disp::CrashGuard &guard() { return gD.guard; }") != std::string::npos, "G1: the auto-disarm log line; the guard state lives in the glue's atomics struct");
        expect(glue.find("uint64_t now_ns() { uint64_t t = 0, ns = 0; clock_get_uptime(&t); absolutetime_to_nanoseconds(t, &ns); return ns; }") != std::string::npos && glue.find("uint64_t now_ns() { return ::now_ns(); }") != std::string::npos, "G1: the guard's timestamps are the kernel's uptime clock");
        expect(glue.find("n48disp::disp_flags(n48disp_latched_on()") != std::string::npos && glue.find("navi48_bar0_write_combined(),") != std::string::npos && glue.find("bool navi48_bar0_write_combined(void);") != std::string::npos, "W1: the flags word carries the BAR0 write-combined bit");
        expect(glue.find("BAR0 %s") != std::string::npos, "W1: the stat log line names the mapping");
        expect(glue.find("native_s1b_refuse") == std::string::npos, "the display glue does not call the S1b refusal (the exemption table is in the pure header)");
        // 0.0.616: the prepared-descriptor cache
        expect(glue.find("if (!n48disp::mdc_held(gD.mdc, md)) return false;") != std::string::npos && glue.find("n48disp::MdCache mdc;") != std::string::npos, "0.0.616: src_read refuses a descriptor that is not a cache hold; the cache lives in the glue's atomics struct");
        expect(count_of(glue, "->prepare(") == 1 && count_of(glue, "->complete(") == 1 && glue.find("d->prepare(kIODirectionOut) != kIOReturnSuccess) { d->release(); return false; }") != std::string::npos, "0.0.616: the glue has exactly one prepare (md_prepare, which gives the reference back on failure) and one complete (md_unprepare)");
        expect(glue.find("OSDynamicCast(IOMemoryDescriptor, reinterpret_cast<OSObject *>(static_cast<uintptr_t>(a)))") != std::string::npos && glue.find("if (!d) return false;\n        d->retain();") != std::string::npos && glue.find("bool hung() { return amdgpu::n1c_hung(); }") != std::string::npos, "0.0.616: the descriptor is identity-checked by its metaclass before it is retained; hung() is the HUNG latch");
        expect(glue.find("static_assert(n48disp::kPfCount == 21u") != std::string::npos && glue.find("out[11] = n48disp::dur_avg(gD.dur); out[12] = gD.dur.max;") != std::string::npos && glue.find("out[10] = gD.dur") == std::string::npos, "0.0.616: stat page 2 carries reasons 12..19 (out[10] is reason 19, not the copy time minimum)");
    }
    if (!nub.empty()) {
        expect(nub.find("n48disp::ops_shape(") != std::string::npos && nub.find("n48disp_latched_on()") != std::string::npos, "the nub publishes the table shape chosen by the latch");
        expect(nub.find("op_disp_hook") != std::string::npos && nub.find("op_pci_device") != std::string::npos, "the ABI-2 members are wired");
        expect(nub.find("*(const N48MetalOps **)param1 = n48disp::ops_shape(n48disp_latched_on()).abi >= 2u ? &gOps : &gOpsV1;") != std::string::npos, "callPlatformFunction hands out the ABI-1 table unless the latch is ON (the table the aux kext sees decides display off)");
        expect(nub.find("OSNumber *abi = OSNumber::withNumber((uint64_t)shape.abi, 32);") != std::string::npos && nub.find("out[2] = shape.abi; out[3] = (uint64_t)shape.size;") != std::string::npos, "the published ABI / size properties follow the latch-chosen shape");
        expect(nub.find("620u") != std::string::npos && nub.find("613u") == std::string::npos, "the ops table carries the build 620");
        expect(nub.find("n48disp_pci_device(ctx)") != std::string::npos, "P1: op_pci_device is the gated hook");
        expect(nub.find("n48disp::fact_mask(") != std::string::npos, "the factory mask goes through n48disp::fact_mask (OFF: the boot-arg's mask unchanged)");
        expect(nub.find("n48disp_res62(") != std::string::npos, "slot 62 is handled by the display glue");
        expect(count_of(nub, "\n\tn48disp_on_withdraw();") == 2 && nub.find("n48disp_on_trace(event, a, b);") != std::string::npos, "both the withdraw selector and the kext stop reset the display state (live calls, not comments), and the aux trace is recorded");
    }
    if (!brg.empty()) {
        expect(brg.find("n48disp::native_exempt(n48disp_latched_on(), action, argScalar)") != std::string::npos, "accelExperiment adds the display exemptions through native_exempt with the latch");
        const size_t ex = brg.find("n48scan::accel_exempt(action, argScalar)"), de = brg.find("n48disp::native_exempt(");
        const size_t ref = brg.find("if (action != 0 && amdgpu::native_s1b_refuse(0)) return kIOReturnNotPermitted;");
        expect(ex != std::string::npos && de != std::string::npos && ref != std::string::npos && ex < de && de < ref, "the exemptions sit BEFORE the native refusal, the scanout table first");
        expect(brg.find("n48disp::action_admitted(n48disp_latched_on(), action)") != std::string::npos, "accelExperiment double-checks the action bound with the latch");
        for (const char *v : { "action == 83", "action == 84", "action == 85", "action == 86", "action == 87", "action == 88", "action == 89", "action == 90" }) expect(brg.find(v) != std::string::npos, "accelExperiment has a branch for each new verb");
        expect(brg.find("n48disp_verb(action, argScalar, v, 13)") != std::string::npos, "the new verbs all go through n48disp_verb");
        expect(brg.find("bool navi48_bar0_write_combined(void) {") != std::string::npos && brg.find("->bar0WriteCombined();") != std::string::npos, "W1: the bring-up class answers the BAR0 write-combined question");
        expect(bhpp.find("bool     bar0WriteCombined() const { return bar0Map && (bar0Map->getMapOptions() & kIOMapWriteCombineCache) != 0; }") != std::string::npos, "W1: the flag is the option the BAR0 map was made with (the test mapVramAperture logs)");
        expect(brg.find("navi48_console_region") != std::string::npos && brg.find("navi48_console_write") != std::string::npos, "the console accessors exist");
    }
    if (!ucl.empty()) {
        expect(ucl.find("if (action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;") != std::string::npos, "the user client's action bound opens past 82 only when latched (site 5 of the verb skill)");
        expect(ucl.find("if (action > 82) return kIOReturnBadArgument;") == std::string::npos, "the old unconditional bound is gone");
        expect(ucl.find("n1c_is_open") == std::string::npos && ucl.find("clientHasPrivilege(securityID, kIOClientPrivilegeAdministrator)") != std::string::npos, "the legacy user client is administrator-only and never consults the exclusive native-client open (it stays reachable while WindowServer holds N48N)");
    }
    if (!brg.empty()) {
        expect(brg.find("if (type == N48N_UC_TYPE) return IOAccelNavi48NativeClient::create(") != std::string::npos && brg.find("auto *uc = OSTypeAlloc(Navi48UserClient);") != std::string::npos,
               "newUserClient routes 'N48N' to the exclusive native client and every other type to the legacy Navi48UserClient");
    }
    if (!cli.empty()) {
        for (const char *v : { "\"pipeadopt\"", "\"pipearm\"", "\"pipestat\"", "\"pipestamps\"", "\"pipeshortcut\"" }) expect(cli.find(v) != std::string::npos, "navi48test has the pipe subcommands");
        expect(cli.find("NULL pipe stored by the family; REBOOT before any WindowServer restart") != std::string::npos && cli.find("st <= 17 ? sn[st]") != std::string::npos, "P3: navi48test prints the reboot advice for status 17");
        expect(cli.find("32 BAR0 kernel mapping write-combined") != std::string::npos && count_of(cli, "BAR0 kernel mapping     :") == 3 && cli.find("(out[12] & 32) ? \"WRITE-COMBINED (perform") != std::string::npos && count_of(cli, "(fl & 32) ? \"WRITE-COMBINED\"") == 2, "W1: navi48test prints the BAR0 mapping for pipeadopt, pipearm and pipestat");
        expect(cli.find("in = 83") != std::string::npos && cli.find("in = 84") != std::string::npos && cli.find("in = 85") != std::string::npos && cli.find("in = 86") != std::string::npos && cli.find("in = 87") != std::string::npos, "the subcommands send actions 83..87");
    }
    { // 0.0.617 (K1/K2): the kernel glue calls the tested flows, in the right place
      const std::string ncl = slurp(K + "src/Navi48NativeClient.cpp");
      if (!ncl.empty()) expect(count_of(ncl, "n48disp_on_ws_client_closed(") == 2 && count_of(ncl, "n48disp_on_ws_client_closed(adminClient); amdgpu::n1c_close(\"clientClose\");") == 1 && count_of(ncl, "n48disp_on_ws_client_closed(adminClient); amdgpu::n1c_close(\"stop\");") == 1 &&
                               ncl.find("amdgpu::n1c_close(\"attach failed\")") != std::string::npos, "K1: clientClose and stop disarm (with the client's admin flag) BEFORE n1c_close, on the same line; the create-failure paths are unchanged");
      { const size_t h0 = nub.find("void Navi48MetalNub::hungLatched() {"), a = nub.find("n48disp_on_hung();", h0), g = nub.find("if (!gLock)", h0);
        expect(h0 != std::string::npos && a != std::string::npos && g != std::string::npos && a < g, "K2: hungLatched() disarms (n48disp_on_hung) before its nub / lock handling"); }
      expect(glue.find("void n48disp_on_ws_client_closed(bool adminClient) {\n    if (!n48disp_latched_on()) return;\n    KernelEnv env;\n    (void)n48disp::ws_client_closed_flow(env, adminClient);") != std::string::npos &&
             glue.find("void n48disp_on_hung(void) {\n    if (!n48disp_latched_on()) return;\n    KernelEnv env;\n    (void)n48disp::hung_latched_flow(env);") != std::string::npos, "K1/K2: the entry points are latch-gated and call the tested flows");
      expect(ghdr.find("void     n48disp_on_ws_client_closed(bool adminClient);") != std::string::npos && ghdr.find("void     n48disp_on_hung(void);") != std::string::npos, "K1/K2: declared in native_disp.h");
      expect(glue.find("auto-disarm: %s; the gate byte is written back to 0") != std::string::npos && glue.find("__atomic_store_n(&gD.autoCause, cause <") != std::string::npos && glue.find("auto-disarmed: %s (count %llu)") != std::string::npos &&
             glue.find("__atomic_load_n(&gD.autoCause, __ATOMIC_ACQUIRE));") != std::string::npos && glue.find("void clear_autodisarm() { __atomic_store_n(&gD.autoCause, 0u, __ATOMIC_RELEASE); }") != std::string::npos, "K4: the cause is stored, cleared by an explicit arm, in the flags word and in the stat line");
      expect(std::string(auto_cause_name(kAdWsClose)).find("the WindowServer GPU client closed while armed") == 0, "K1: the log text names the cause");
      { // R1 (0.0.619): the wiring of the operator restart window (ordering and reachability are tested above on the real flows; these pin the glue, the verb plumbing and the CLI)
        const size_t k1 = flow.find("template <class E> bool ws_client_closed_flow("), k1e = flow.find("template <class E> bool hung_latched_flow(");
        const std::string k1s = k1 != std::string::npos && k1e != std::string::npos && k1e > k1 ? flow.substr(k1, k1e - k1) : std::string();
        const size_t c = k1s.find("ws_close_disarms(e.latch_on(), adminClient)"), t = k1s.find("reload_tolerate_close(e)"), d = k1s.find("return auto_disarm_flow(e, kAdWsClose);");
        expect(c != std::string::npos && t != std::string::npos && d != std::string::npos && c < t && t < d, "R1: K1 order: the uid-88 / latch check, then the window, then the disarm");
        const size_t h1 = flow.find("template <class E> bool hung_latched_flow("), h2 = flow.find("// ---- slot 267:");
        expect(h1 != std::string::npos && h2 > h1 && flow.substr(h1, h2 - h1).find("reload_") == std::string::npos, "R1: K2 (hung_latched_flow) never consults the window");
        expect(flow.find("if (e.armed() && !reload_on_init_fb(e) && guard_on_init_fb(e.guard(), e.now_ns()))") != std::string::npos, "R1: K3: the window is consulted before the guard counts, and only while armed");
        expect(glue.find("n48disp::ReloadWin &rw() { return gD.reload; }") != std::string::npos && glue.find("if (arg == 0ull) n48disp::reload_open_flow(env);") != std::string::npos && glue.find("action == n48disp::kActReload") != std::string::npos &&
               glue.find("operator restart window: client close tolerated") != std::string::npos && glue.find("operator restart window: closed") != std::string::npos && glue.find("operator restart window: expired") != std::string::npos &&
               glue.find("operator restart window: opened") != std::string::npos, "R1: the glue owns the state, the verb opens it through the tested flow, and the four log lines exist");
        expect(brg.find("action == 89 || action == 90) {") != std::string::npos, "R1: accelExperiment routes 90 to n48disp_verb");
        expect(cli.find("in = 90;") != std::string::npos && cli.find("\"pipereload\"") != std::string::npos && cli.find("//     killall -9 WindowServer            (within 15 s;") != std::string::npos && cli.find("pipereload [0|1]") != std::string::npos &&
               cli.find("in == 90 ? \"pipereload\"") != std::string::npos && cli.find("|| in == 89 || in == 90) {") != std::string::npos, "R1: navi48test has pipereload (action 90), prints the window and documents the operator sequence");
      }
    }
    if (!brg.empty()) expect(brg.find("pci->setProperty(\"VRAM,totalMB\", static_cast<uint64_t>(vramMB), 32);") != std::string::npos && brg.find("pci->setProperty(\"VRAM,totalsize\", d);") != std::string::npos && brg.find("setProperty(\"VRAM,TotalMB\", static_cast<uint64_t>(vramMB), 32);") != std::string::npos &&
                              count_of(brg, "publishNativeVramOn(") == 2 && brg.find("if (amdgpu::native_s1b_latched()) publishNativeVramOn(pciDevice, vramMB);") != std::string::npos && brg.find("const uint64_t bytes = static_cast<uint64_t>(vramMB) << 20;") != std::string::npos,
                              "K5: native boots only (inside the native latch) the IOPCIDevice also gets VRAM,totalMB (32-bit) and VRAM,totalsize (8-byte OSData, bytes); the old VRAM,TotalMB stays");
    expect(count_of(plist, "0.0.620") == 2 && plist.find("0.0.612") == std::string::npos, "Info.plist is 0.0.620, carried twice");
    expect(s1ch.find("kN1cKextBuild = 620;") != std::string::npos, "the native client reports build 620");
    expect(ops.find("#define N48_METAL_ABI         2u") != std::string::npos && ops.find("N48_VC_DISPLAYPIPE    7u") != std::string::npos, "the ops header is ABI 2 with the display class");
    expect(pure.find("kNeedFacts") != std::string::npos && flow.find("e.facts_add(kNeedFacts)") != std::string::npos, "adopt adds the needed facts");
    expect(count_of(flow, "e.md_prepare(") == 1 && count_of(flow, "mdc_teardown(e);") == 5 && flow.find("if (*ret == kWillPerform) (void)submit_prepare(e, args[0]);") != std::string::npos && count_of(flow, "mdc_acquire(e.mdc(), md)") == 1, "0.0.616: ONE prepare in the whole flow (mdc_ensure, from submit), teardown after every disarm path, submit prepares, perform acquires");
    expect(flow.find("e.note_null_pipe()") != std::string::npos && flow.find("guard_reset(e.guard());") != std::string::npos && flow.find("guard_on_perform(e.guard());") != std::string::npos && flow.find("guard_on_init_fb(e.guard(), e.now_ns())") != std::string::npos, "0.0.615: the flows call the NULL-pipe note and the three guard steps");
    expect(count_of(flow, "__atomic_") >= 8 && flow.find("IOLock") == std::string::npos && flow.find("lck_") == std::string::npos, "G1: the guard is atomics only, no lock");
    expect(bhpp.find("consoleWrite") != std::string::npos && bhpp.find("consoleGeometry") != std::string::npos, "the bring-up class exposes the console region and write");
    // the byte-identity of the ops header with the aux kext's copy (the aux tree root is the optional 2nd argument; default: this tree)
    { const std::string auxroot = argc > 2 ? argv[2] : root; const std::string aux = slurp(auxroot + "/tools/native/navi48accel/src/Navi48MetalOps.h");
      if (aux.empty()) std::printf("NOTE: the aux kext copy of Navi48MetalOps.h is not present under %s\n", auxroot.c_str()); else expect(aux == ops, "Navi48MetalOps.h is byte-identical in the bring-up kext and the aux kext"); }

    // =========================================================================================================================================================
    // F. the project rule: three spellings appear in no file
    // =========================================================================================================================================================
    // The repo's own check (tests/native_s2_dal_test.cpp) is scoped to the NEW native files; old files keep their historical comments. Here: the new files, this test's own neighbours, and the
    // files this task edited (navi48test.c carries exactly ONE legacy printf line whose text has the "pipe+" prefix before the first of them, at its 0.0.612 position: the counts prove the task added none).
    { const std::string a = std::string("pipe+") + "0x280", b = std::string("pipe+") + "0x282", c = std::string("pipe+") + "0x299";
      for (const std::string *f : { &pure, &flow, &glue, &ghdr, &nub, &ops }) expect(f->find(a) == std::string::npos && f->find(b) == std::string::npos && f->find(c) == std::string::npos,
             "no NEW display file spells the three forbidden pipe offsets");
      expect(count_of(cli, a) == 1 && count_of(cli, b) == 0 && count_of(cli, c) == 0, "navi48test.c: only its legacy TXEND line (0.0.612) carries the one prefixed spelling; this task added none");
      expect(brg.find(a) == std::string::npos && brg.find(b) == std::string::npos && brg.find(c) == std::string::npos && ucl.find(a) == std::string::npos && ucl.find(b) == std::string::npos && ucl.find(c) == std::string::npos, "Navi48Bringup.cpp and the user client carry none");
      const std::string dsp = slurp(K + "tests/native_disp_plant.sh"), me = slurp(K + "tests/native_disp_test.cpp");
      expect(dsp.find(a) == std::string::npos && dsp.find(b) == std::string::npos && dsp.find(c) == std::string::npos && me.find(a) == std::string::npos, "the plant script and this test never spell them"); }

    std::printf("native_disp_test: %d/%d passed\n", gRun - gFail, gRun);
    return gFail ? 1 : 0;
}
