// native_disp_test.cpp - build 0.0.613 (#11 step 11h.2, an internal design note + the 11h.1 RE facts section): the display pipe.
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
//   M  (0.0.652, M5) the display nub and `fbpublish 2`: the pure verdicts of amd/native_fb_pure.h (the two phases, the snapshot, the aperture), the shared Navi48DisplayOps.h (shapes, ops check, snapshot validator),
//      the interlocks (pipeadopt / pipearm 1 / the Metal-nub publish refuse while the nub exists; fbpublish refuses an adopted pipe and a WindowServer session), admission of action 105, the ORDER inside Navi48DisplayNub.cpp
//      (software refusals < live gates < DDC < snapshot < attach < register), the byte-identity of the ops header with the aux kext's copy, the variant;
//   F  the project rule: the three forbidden pipe-offset spellings appear in no file of the tree.
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>
#include <atomic>
#include "amd/native_disp_flow.h"
#include "native_fb_monb_battery.h"   // 0.0.658: the monitor B's publish path, hashed (the golden is the hash of the 0.0.657 sources)
#include "native_m6_off_battery.h"     // 0.0.659: the latch-OFF paths, hashed (the golden is the hash of the 0.0.658 sources)

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

    // 0.0.659 (M6 Stage 1a): the latch, the framebuffer -> instance answers, the surface table (the REAL n48m6:: table code), and the counters the kernel glue keeps
    bool m6 = false; std::map<uint64_t, uint32_t> pinst; n48m6::SurfTable surfTbl {};
    uint32_t m6Learns = 0, m6Published = 0, m6Submits[4] = {}, m6NoCopy[3] = {}, m6Routes[n48m6::kRouteCount] = {}, m6Skips[n48m6::kSkCount] = {}, m6StampOk[3] = {}, m6StampBad[3] = {}, m6SampleInst = 0;
    bool m6_on() { return m6; }
    bool m6flip = false; bool m6flip_on() { return m6flip; }   // 0.0.661 (M6 Stage 1b): navi48-m6flip (the reset moves to the new start's first slot-267, R1)
    uint32_t pipe_inst(uint64_t pipe) { auto it = pinst.find(pipe); return it == pinst.end() ? n48m6::kInstNone : it->second; }
    n48m6::Look m6_lookup(uint32_t id) { return n48m6::lookup_surface(surfTbl, id); }
    uint32_t m6_learn(uint32_t id, uint32_t inst) { m6Learns++; log.push_back("m6_learn"); const uint32_t r = n48m6::learn_surface(surfTbl, id, inst, m6flip); if (n48m6::learn_changed(r)) m6Published++; return r; }
    uint32_t m6Resets = 0, m6Touches = 0; bool resetSawOpen = false;
    void m6_reset() { log.push_back("m6_reset"); m6Resets++; resetSawOpen = __atomic_load_n(&rwin.open, __ATOMIC_ACQUIRE) != 0u; (void)n48m6::reset_table(surfTbl); m6Published++; }       // 0.0.660 (S1): the glue empties the table and republishes
    void m6_touch(uint32_t id) { m6Touches++; (void)n48m6::touch_surface(surfTbl, id); }
    void m6_note_submit(uint32_t inst) { m6Submits[inst < 3u ? inst : 3u]++; }
    void m6_note_learn_skip(uint32_t why) { m6Skips[why < n48m6::kSkCount ? why : 0u]++; }
    void m6_note_route(uint32_t v, uint32_t) { m6Routes[v < n48m6::kRouteCount ? v : 0u]++; }
    void m6_note_nocopy(uint32_t inst) { m6NoCopy[inst < 3u ? inst : 0u]++; }
    void m6_note_stamp(uint32_t inst, uint32_t verdict, uint64_t) { if (inst < 3u) { if (verdict == kVblWrote) m6StampOk[inst]++; else m6StampBad[inst]++; } }
    VblSample vsInst[3] = { { 16680000ull, 5000000ull, 1000000000ull, 1u, 1u }, { 16666667ull, 4000000ull, 1000000000ull, 1u, 1u }, { 16683333ull, 3000000ull, 1000000000ull, 1u, 1u } };
    bool vbl_sample_inst(uint32_t inst, VblSample *o) { log.push_back("vbl_sample_inst"); m6SampleInst = inst; if (!sampleOk || inst >= 3u) return false; *o = vsInst[inst]; return true; }

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

    // the action bound: 0..82 always, 83..94 only when latched (88 = pipeagdc, 0.0.614; 89 = pipevbl, 0.0.618; 90 = pipereload, 0.0.619; 91..93 = ddcread / dmubring / dispcensus, 0.0.622; 94 = region4read, 0.0.624), 95+ never
    for (uint32_t a = 0; a <= 82; ++a) expect(action_admitted(false, a) && action_admitted(true, a), "actions 0..82 are admitted with the latch on or off");
    for (uint32_t a = 83; a <= 98; ++a) { expect(!action_admitted(false, a), "a new verb is NOT admitted with the boot-arg OFF"); expect(action_admitted(true, a), "a new verb is admitted with the boot-arg ON"); }
    expect(action_admitted(true, 99) && !action_admitted(false, 99) && !action_admitted(true, 100) && !action_admitted(false, 100) && !action_admitted(true, 0xffffffffu) && kLastAction == 99 && kActScdcRead == 99 && kActDisp2 == 98 && kActDmubSend == 95 && kActDmubMode == 96 && kActDmubCtx == 97 && kActRegion4Read == 94 && kActAgdc == 88 && kActVbl == 89 && kActReload == 90 && kActDdcRead == 91 && kActDmubRing == 92 && kActDispCensus == 93, "no action past 99 is ever admitted (0.0.625: 95..97 = dmubsend / dmubmode / dmubctx; 0.0.631: 98 = disp2; 0.0.633: 99 = scdcread, only with the latch ON)");
    expect(is_pipe_verb(83) && is_pipe_verb(87) && is_pipe_verb(89) && is_pipe_verb(90) && !is_pipe_verb(82) && !is_pipe_verb(88) && !is_pipe_verb(91) && !is_pipe_verb(92) && !is_pipe_verb(93) && !is_pipe_verb(94) && !is_pipe_verb(95) && !is_pipe_verb(96) && !is_pipe_verb(97) && is_new_action(88) && is_new_action(89) && is_new_action(90) && is_new_action(91) && is_new_action(93) && is_new_action(94) && is_new_action(95) && is_new_action(97) && is_new_action(98) && !is_pipe_verb(98) && is_new_action(99) && !is_pipe_verb(99) && !is_new_action(100), "n48disp_verb answers exactly 83..87, 89 and 90; 88 (pipeagdc) is a new action answered elsewhere; 99 (scdcread, 0.0.633) is the last new action");
    expect(kActAdopt == 83 && kActArm == 84 && kActStat == 85 && kActStamps == 86 && kActShortcut == 87 && kActFbname == 78, "the verb numbers are the published ones");

    // native exemptions: nothing when OFF; exactly the listed (action, arg) pairs when ON
    for (uint32_t a = 0; a <= 99; ++a) for (uint64_t g = 0; g <= 3; ++g) expect(!native_exempt(false, a, g), "no native exemption with the boot-arg OFF");
    expect(native_exempt(true, 78, 0) && native_exempt(true, 78, 1) && !native_exempt(true, 78, 2) && !native_exempt(true, 78, 1000), "fbname is exempt for 0 and 1 only");
    expect(native_exempt(true, 83, 0) && !native_exempt(true, 83, 1), "pipe adopt is exempt with argument 0 only");
    expect(native_exempt(true, 84, 0) && native_exempt(true, 84, 1) && !native_exempt(true, 84, 2), "pipe arm is exempt for 0 and 1");
    expect(native_exempt(true, 85, 0) && native_exempt(true, 85, 3) && native_exempt(true, 85, 4) && !native_exempt(true, 85, 5), "pipe stat is exempt for pages 0..4 (0.0.617: page 3, 0.0.618: page 4)");
    expect(native_exempt(true, 89, 0) && native_exempt(true, 89, 1) && !native_exempt(true, 89, 2) && !native_exempt(false, 89, 1), "pipevbl is exempt for 0 and 1, with the latch ON only (0.0.618)");
    expect(native_exempt(true, 90, 0) && native_exempt(true, 90, 1) && !native_exempt(true, 90, 2) && !native_exempt(false, 90, 0), "pipereload is exempt for 0 and 1, with the latch ON only (0.0.619)");
    expect(native_exempt(true, 86, 0) && !native_exempt(true, 86, 1), "pipe stamps is exempt with argument 0 only");
    expect(native_exempt(true, 87, 0) && native_exempt(true, 87, 1) && !native_exempt(true, 87, 2), "pipe shortcut is exempt for 0 and 1");
    expect(native_exempt(true, 88, 0) && native_exempt(true, 88, 1) && !native_exempt(true, 88, 2) && !native_exempt(false, 88, 1), "pipeagdc is exempt for 0 and 1, with the latch ON only");
    // 0.0.622 (M1): the three display instruments are exempt with their LEGAL arguments only, with the latch ON only (they touch DCN registers at most - never GFX/VM state)
    expect(native_exempt(true, 91, n48dr_ddc_arg(2, 0, 0)) && native_exempt(true, 91, n48dr_ddc_arg(3, 3, 1)) && !native_exempt(true, 91, n48dr_ddc_arg(1, 0, 0)) && !native_exempt(true, 91, n48dr_ddc_arg(0, 0, 0)) &&
           !native_exempt(true, 91, n48dr_ddc_arg(4, 0, 0)) && !native_exempt(true, 91, n48dr_ddc_arg(2, 4, 0)) && !native_exempt(true, 91, n48dr_ddc_arg(2, 0, 2)) && !native_exempt(true, 91, 0) && !native_exempt(true, 91, 1ull << 24) &&
           !native_exempt(false, 91, n48dr_ddc_arg(2, 0, 0)), "ddcread is exempt for lines 2/3, blocks 0..3, pages 0/1, with the latch ON only");
    expect(native_exempt(true, 92, 0) && native_exempt(true, 92, 25) && native_exempt(true, 92, 0x80) && !native_exempt(true, 92, 26) && !native_exempt(true, 92, 0x81) && !native_exempt(false, 92, 0), "dmubring is exempt for page 0, 1..25 and 0x80, with the latch ON only");
    expect(native_exempt(true, 93, 0) && native_exempt(true, 93, 1) && native_exempt(true, 93, 2) && native_exempt(true, 93, 7) && native_exempt(true, 93, 8) && native_exempt(true, 93, 9) && native_exempt(true, 93, 10) && native_exempt(true, 93, 11) && native_exempt(true, 93, 42) && native_exempt(true, 93, 43) && native_exempt(true, 93, 44) && native_exempt(true, 93, 59) && !native_exempt(true, 93, 60) && !native_exempt(true, 93, ~0ull) && !native_exempt(false, 93, 0) && !native_exempt(false, 93, 5) && !native_exempt(false, 93, 43) && !native_exempt(false, 93, 44), "dispcensus is exempt for pages 0..59 (0.0.654: 44..59 the monitor A census; 0.0.628: 2..7 are the M4a pages; 0.0.630: 8..9 the M4c pages; 0.0.634: 10 the DP watch, 11..42 the plane census; 0.0.635: 43 the rest of the DP watch), with the latch ON only");
    // 0.0.624 (M1.5): region4read is exempt with a LEGAL (offset, dwords, page) only: 4-byte aligned, 1..64 dwords, inside the 64 KiB window, page < ceil(dwords / 22), with the latch ON only
    expect(native_exempt(true, 94, n48dr_r4_arg(0x6000, 64, 2)) && native_exempt(true, 94, n48dr_r4_arg(0xFFFC, 1, 0)) && !native_exempt(true, 94, n48dr_r4_arg(0xFFFC, 2, 0)) && !native_exempt(true, 94, n48dr_r4_arg(0x6002, 4, 0)) && !native_exempt(true, 94, n48dr_r4_arg(0x6000, 0, 0)) &&
           !native_exempt(true, 94, n48dr_r4_arg(0x6000, 65, 0)) && !native_exempt(true, 94, n48dr_r4_arg(0x6000, 22, 1)) && !native_exempt(true, 94, n48dr_r4_arg(0x6000, 23, 2)) && !native_exempt(true, 94, 1ull << 48) && !native_exempt(false, 94, n48dr_r4_arg(0x6000, 16, 0)), "region4read is exempt for a legal window read only, with the latch ON only");
        // 0.0.625 (M2/M3): dmubsend / dmubmode / dmubctx are exempt with the metal-disp latch ON for ANY argument, so a refusal keeps its own status; each verb judges its argument and boot-arg navi48-dmubcmd itself (tests/native_dmubcmd_test.cpp)
    for (uint32_t a = 95; a <= 98; ++a) { expect(native_exempt(true, a, 0) && native_exempt(true, a, 0xFFFFFFFFFFFFFFFFull) && !native_exempt(false, a, 0), "95..98 are exempt with the latch ON only (any argument; 0.0.631: 98 = disp2)"); }
    for (uint32_t a : { 1u, 2u, 31u, 50u, 66u, 67u, 76u, 77u, 79u, 80u, 81u, 82u, 99u }) expect(!native_exempt(true, a, 0) && !native_exempt(true, a, 1), "no other verb is exempt, even with the boot-arg ON");

    for (uint32_t a = 0; a <= 99; ++a) for (uint64_t g = 0; g <= 3; ++g) expect(native_exempt(true, a, g) == verb_args_ok(a, g), "native_exempt(ON) is exactly verb_args_ok");
    expect(!verb_args_ok(83, 1) && !verb_args_ok(86, 5) && !verb_args_ok(85, 5) && !verb_args_ok(87, 2) && !verb_args_ok(89, 2) && !verb_args_ok(84, 2) && !verb_args_ok(78, 2) && !verb_args_ok(82, 0), "verb_args_ok: the illegal arguments");

    // the ops table shape
    { const OpsShape off = ops_shape(false), on = ops_shape(true);
      expect(off.abi == 3 && off.size == N48_METAL_OPS_V3 && off.dispFlags == 0, "OFF: the ops table is the ABI-3, 152-byte table with NO display flag (0.0.656: native_open must exist with the display off; the display members are zero, which is display off to every aux kext)");
      expect(on.abi == 3 && on.size == N48_METAL_OPS_V3 && (on.dispFlags & N48_DISP_F_ON) != 0, "ON: ABI 3, 152 bytes, the display flag"); }
    expect(N48_METAL_ABI == 3 && N48_METAL_ABI_MIN == 1 && N48_METAL_OPS_V2 == 144 && N48_METAL_OPS_V3 == 152 && N48_METAL_OPS_MIN == 120 && sizeof(N48MetalOps) == 152, "the ABI constants");
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
      expect(kPfNotPrepared == 19 && kPfScanOwned == 20 && kPfOtherInst == 21 && kPfRouteRefused == 22 && kPfCount == 23 && std::strcmp(perf_name(kPfOtherInst), "unknown") != 0 && std::strcmp(perf_name(kPfRouteRefused), "unknown") != 0 && std::strcmp(perf_name(kPfScanOwned), "unknown") != 0 && std::strcmp(perf_name(kPfNotPrepared), "unknown") != 0 && kMdCacheN == 8, "the new reason code (19) is named; the cache has 8 entries"); }

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
    { expect(kNullPipe == 17 && kFbNub == 18 && kStatusCount == 19 && std::strcmp(status_name(kNullPipe), "unknown") != 0 && kNullPipe != kNoPipe && kNullPipe != kHasPipes, "P3: kNullPipe is a distinct, named status");
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
    expect(ws_close_disarms(true, true) && !ws_close_disarms(true, false) && !ws_close_disarms(false, false) && !ws_close_disarms(false, true), "K1 (0.0.627: keyed on the closing session being WindowServer's): only WindowServer's session disarms, and only with the latch ON");
    { World w; uint64_t ret = 0; w.e.now = 1 * S; w.e.clockStep = 0; (void)w.perform(&ret);
      expect(w.e.armedFlag && w.e.mdcache.e[0].state != 0u, "K1: setup - armed, one descriptor prepared by submit");
      expect(ws_client_closed_flow(w.e, true), "K1: the uid-88 client's close while armed is acted on");
      expect(!w.e.armedFlag && w.e.armByte == 0 && w.e.autoDisarms == 1 && w.e.autoCause == kAdWsClose, "K1: gate byte 0, mirror cleared, noted once with the WsClose cause");
      expect(w.e.complCount[A_MD] == 1 && w.e.mdcache.tornDown == 1, "K1: the descriptor cache is torn down (complete + release exactly once)");
      expect(w.e.idx("disarm") >= 0 && w.e.idx("disarm") < w.e.idx("md_complete") && w.e.idx("md_complete") < w.e.idx("note_autodisarm"), "K1: ORDER: disarm (mirror + byte) before the teardown before the note");
      const int reads0 = w.e.srcReads; uint64_t pr = 5; const int h = w.perform(&pr); expect(h == 1 && pr == 0 && w.e.reasons[kPfDisarmed] >= 1 && w.e.srcReads == reads0, "K1: a perform after the close is refused as disarmed, nothing is read");
      expect(!ws_client_closed_flow(w.e, true) && w.e.autoDisarms == 1 && w.e.count("disarm") == 1, "K1: closing again (stop after clientClose) is idempotent: nothing more is done or logged"); }
    { World w; expect(!ws_client_closed_flow(w.e, false) && w.e.armedFlag && w.e.autoDisarms == 0 && w.e.count("disarm") == 0, "K1: an ADMIN client (operator tools) closing never disarms"); }
    { World w; w.e.armedFlag = false; w.e.armByte = 0; expect(!ws_client_closed_flow(w.e, true) && w.e.autoDisarms == 0 && w.e.log.empty(), "K1: not armed -> nothing at all (no write, no log)"); }
    { World w; w.e.latch = false; expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag && w.e.log.empty(), "K1: latch OFF (boot-arg absent) -> nothing: 0.0.616 / 0.0.612 behaviour"); }
    { // the real ordering: arm_flow -> 267 -> submit -> perform copies -> client dies -> next perform refused
      World w; w.e.armedFlag = false; w.e.armByte = 0; w.e.before = good_probe(); w.e.layoutOk = true; uint64_t ret = 0;
      expect_u("K1: arm", arm_flow(w.e, 1), kOk); init267(w, 1 * S, &ret); w.e.now = 2 * S; w.e.clockStep = 0; (void)w.submit(); (void)w.perform(&ret);
      expect(w.e.reasons[kPfCopied] == 1, "K1: a frame was copied while armed");
      (void)ws_client_closed_flow(w.e, true); (void)w.perform(&ret);
      expect(w.e.reasons[kPfCopied] == 1 && w.e.armByte == 0, "K1: no frame is copied after the WindowServer client died"); }

    // ---- K2: the HUNG latch while armed -> disarm (same flow, cause Hung)
    { World w; w.e.hungFlag = true; uint64_t ret = 0; (void)ret;
      expect(hung_latched_flow(w.e) && !w.e.armedFlag && w.e.armByte == 0 && w.e.autoCause == kAdHung && w.e.autoDisarms == 1, "K2: HUNG while armed disarms and records the Hung cause");
      expect(w.e.complCount[A_MD] == 0 && w.e.mdcache.leaked == 1, "K2: under HUNG the prepared descriptor is LEAKED (never completed), the teardown does not wait"); }
    { World w; w.e.armedFlag = false; w.e.armByte = 0; expect(!hung_latched_flow(w.e) && w.e.log.empty(), "K2: HUNG while not armed: nothing"); }
    { World w; w.e.latch = false; expect(!hung_latched_flow(w.e) && w.e.armedFlag && w.e.log.empty(), "K2: latch OFF -> nothing"); }

    // ---- R1 (0.0.619): the operator restart window (`pipereload`): a deliberate WindowServer restart must not trip K1 / K3
    expect(kReloadWindowNs == 15ull * 1000000000ull, "R1: the window is 15 s");
    expect(kReloadSettleNs == 4ull * 1000000000ull && kReloadSettleNs < kReloadWindowNs && kReloadSettleNs > 1610000000ull, "R1 (0.0.621): the settle time is 4 s: inside the 15 s and longer than the measured 1.61 s gap between the old client's two closes");
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
      w.e.now = 105 * S; const bool dis = ws_client_closed_flow(w.e, true);
      expect(!dis && w.e.armedFlag && w.e.armByte == 1 && w.e.autoDisarms == 0 && w.e.count("disarm") == 0 && w.e.complCount[A_MD] == 0 && w.e.rwEv[kRwTolerated] == 1, "R1: the uid-88 close inside the window is tolerated: still armed, gate byte kept, nothing torn down, logged");
      expect(reload_state_flow(w.e, &rem) == 2u, "R1: state 2 after the tolerated close (waiting for the new client's first slot 267)");
      expect(init267(w, 109 * S - 1ull, &ret) == 1 && w.e.armedFlag && w.e.cg.n == 0 && w.e.rwEv[kRwClosed267] == 0 && w.e.rwin.open && w.e.rwin.seen267 == 1u, "R1 (0.0.621): the new client's first slot 267 1 ns before the settle time: handled, NOT counted, remembered, the window stays OPEN");
      expect(init267(w, 109 * S, &ret) == 1 && ret != 0 && w.e.armedFlag && w.e.cg.n == 1 && w.e.rwEv[kRwClosed267] == 1 && reload_state_flow(w.e, &rem) == 0u && !w.e.rwin.open, "R1: the next slot 267 at EXACTLY the settle time (4 s after the last tolerated close) finds the window settled: closed (logged once), handled normally and counted by K3 (an event at or after the settle time is no longer protected)");
      expect(w.e.rwSeq.size() == 3 && w.e.rwSeq[0] == kRwOpened && w.e.rwSeq[1] == kRwTolerated && w.e.rwSeq[2] == kRwClosed267, "R1: log order: opened, tolerated, closed");
      // one-shot: the window is gone, so 0.0.618 behaviour is back: the next close disarms, and K3 counts again
      World v; v.e.armByte = 1; v.e.now = 100 * S; v.e.clockStep = 0; reload_open_flow(v.e); (void)ws_client_closed_flow(v.e, true); init267(v, 104 * S, &ret);
      init267(v, 105 * S, &ret); init267(v, 106 * S, &ret); init267(v, 107 * S, &ret);
      expect(v.e.cg.n == 3 && !v.e.armedFlag && v.e.autoCause == kAdGuard, "R1: ONE-SHOT: after the window closed, the next three slot-267 calls inside 120 s trip K3 exactly as in 0.0.618");
      World x; x.e.armByte = 1; x.e.now = 100 * S; x.e.clockStep = 0; reload_open_flow(x.e); (void)ws_client_closed_flow(x.e, true); init267(x, 104 * S, &ret);
      x.e.now = 105 * S; expect(ws_client_closed_flow(x.e, true) && !x.e.armedFlag && x.e.autoCause == kAdWsClose, "R1: ONE-SHOT: a SECOND close after the window closed disarms (K1 unchanged)"); }
    { // OFF identity: a window that was never opened changes nothing; K1 and K3 behave as in 0.0.618
      World w; w.e.armByte = 1; w.e.now = 100 * S; w.e.clockStep = 0; uint64_t ret = 0;
      expect(ws_client_closed_flow(w.e, true) && !w.e.armedFlag && w.e.autoCause == kAdWsClose && w.e.rwEv[kRwTolerated] == 0 && w.e.rwEv[kRwExpired] == 0, "R1: no window opened -> K1 disarms as before, nothing is logged for the window");
      World v; v.e.armByte = 1; init267(v, 1 * S, &ret); init267(v, 2 * S, &ret); init267(v, 3 * S, &ret); expect(!v.e.armedFlag && v.e.autoCause == kAdGuard && v.e.cg.n == 3 && v.e.rwSeq.empty(), "R1: no window opened -> K3 counts and trips as before"); }
    { // the time bounds: a close at 15 s - 1 ns is tolerated; at exactly 15 s the window has expired and K1 disarms (the expiry is logged once)
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); w.e.now = 100 * S + kReloadWindowNs - 1ull;
      expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag && w.e.rwEv[kRwTolerated] == 1, "R1: a close 1 ns before the 15 s is tolerated");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); v.e.now = 100 * S + kReloadWindowNs;
      expect(ws_client_closed_flow(v.e, true) && !v.e.armedFlag && v.e.autoCause == kAdWsClose && v.e.rwEv[kRwTolerated] == 0 && v.e.rwEv[kRwExpired] == 1 && !v.e.rwin.open, "R1: a close at exactly 15 s: the window has expired -> K1 disarms; the expiry is logged and the window is shut");
      uint64_t rem = 0; expect(reload_state_flow(v.e, &rem) == 0u && v.e.rwEv[kRwExpired] == 1, "R1: an expired window is logged once, not on every look"); }
    { // expiry between the tolerated close and the new client's slot 267: the 267 is counted (the window no longer protects it) and the pipe stays armed
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0; reload_open_flow(w.e); w.e.now = 101 * S; (void)ws_client_closed_flow(w.e, true);
      init267(w, 116 * S, &ret); expect(w.e.armedFlag && w.e.cg.n == 1 && w.e.rwEv[kRwExpired] == 1 && w.e.rwEv[kRwClosed267] == 0, "R1: the first 267 after the 15 s is counted by K3 and the window expires (not 'closed by 267')"); }
    // ---- 0.0.621: EVERY uid-88 close inside the window is tolerated; the window closes on (the new client's slot 267 after a tolerated close) AND (kReloadSettleNs since the LAST tolerated close), or at 15 s
    { // the MEASURED sequence: old client close (t), the new client's first slot 267 (t+1.24 s), the old WindowServer's SECOND close (t+1.61 s): all tolerated, the pipe stays armed (0.0.619 disarmed here)
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0, rem = 0; reload_open_flow(w.e);
      w.e.now = 101 * S; expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag, "R1b: close 1 tolerated");
      expect(init267(w, 101 * S + 240000000ull, &ret) == 1 && w.e.cg.n == 0 && w.e.armedFlag && w.e.rwEv[kRwClosed267] == 0 && w.e.rwin.open, "R1b: the new client's first slot 267 (1.24 s later) is not counted and does NOT close the window yet");
      w.e.now = 101 * S + 610000000ull; const bool dis2 = ws_client_closed_flow(w.e, true);
      expect(!dis2 && w.e.armedFlag && w.e.armByte == 1 && w.e.autoDisarms == 0 && w.e.count("disarm") == 0 && w.e.rwEv[kRwTolerated] == 2 && w.e.rwin.open, "R1b: the SECOND close (0.37 s after the 267, 1.61 s after the first) is tolerated: still armed, nothing torn down, two tolerated");
      expect(reload_state_flow(w.e, &rem) == 2u, "R1b: the verb still reads OPEN (close tolerated) while settling");
      expect(init267(w, 101 * S + 700000000ull, &ret) == 1 && w.e.cg.n == 0 && w.e.rwin.open, "R1b: another slot 267 while settling is still not counted");
      // the settle time restarts at the SECOND close (t+1.61 s): it ends at t+5.61 s
      w.e.now = 101 * S + 610000000ull + kReloadSettleNs - 1ull; expect(reload_state_flow(w.e, &rem) == 2u && w.e.rwin.open, "R1b: 1 ns before the settle time (from the LAST tolerated close): still open");
      init267(w, 101 * S + 610000000ull + kReloadSettleNs, &ret); expect(w.e.cg.n == 1 && !w.e.rwin.open && w.e.rwEv[kRwClosed267] == 1 && w.e.armedFlag, "R1b: at exactly the settle time the next slot 267 finds the window settled: closed (logged once), counted by K3 (one count does not trip it)");
      w.e.now = 101 * S + 610000000ull + kReloadSettleNs + 1ull * S; expect(ws_client_closed_flow(w.e, true) && !w.e.armedFlag && w.e.autoCause == kAdWsClose && w.e.rwEv[kRwTolerated] == 2, "R1b: a close AFTER the window closed disarms (K1 unchanged)"); }
    { // a close that arrives AFTER the settle time (no event closed the window in between) finds it closed: lazily evaluated, K1 disarms; and the same close 1 ns earlier is tolerated
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true); init267(w, 101 * S, &ret);
      w.e.now = 104 * S - 1ull; expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag && w.e.rwEv[kRwTolerated] == 2, "R1b: a second close 1 ns before close 1 + 4 s is tolerated (and restarts the settle time)");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); (void)ws_client_closed_flow(v.e, true); init267(v, 101 * S, &ret);
      v.e.now = 104 * S + 1ull; expect(ws_client_closed_flow(v.e, true) && !v.e.armedFlag && v.e.autoCause == kAdWsClose && v.e.rwEv[kRwTolerated] == 1 && v.e.rwEv[kRwClosed267] == 1 && !v.e.rwin.open,
                                      "R1b: close 1 at 100 s, the 267 at 101 s, a second close at 104 s + 1 ns: the window had settled -> closed by the settle rule, THAT close disarms (K1)");
      World u; u.e.armByte = 1; u.e.clockStep = 0; u.e.now = 100 * S; reload_open_flow(u.e); (void)ws_client_closed_flow(u.e, true); (void)ws_client_closed_flow(u.e, true); (void)ws_client_closed_flow(u.e, true);
      expect(u.e.rwEv[kRwTolerated] == 3 && u.e.armedFlag && u.e.rwin.open, "R1b: THREE closes in the window are all tolerated (every close, not just the first)"); }
    { // the 267 alone never closes the window (a window that closes on the 267 AND nothing else is the 0.0.619 gap); neither does the settle time alone
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0, rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true);
      init267(w, 101 * S, &ret); expect(w.e.rwin.open && w.e.rwEv[kRwClosed267] == 0 && w.e.cg.n == 0, "R1b: the first slot 267 alone (1 s after the close) does not close the window");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); (void)ws_client_closed_flow(v.e, true); v.e.now = 110 * S;
      expect(reload_state_flow(v.e, &rem) == 2u && v.e.rwin.open && v.e.rwEv[kRwClosed267] == 0 && rem == 5 * S, "R1b: the settle time alone (10 s after the close, no slot 267 yet) does not close the window: state 2, 5 s left");
      init267(v, 111 * S, &ret); expect(!v.e.rwin.open && v.e.rwEv[kRwClosed267] == 1 && v.e.cg.n == 0, "R1b: the first slot 267 after the settle time closes it at once and is not counted"); }
    { // a look by the verb closes a settled window too (state 0 afterwards, logged once)
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0, rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true); init267(w, 101 * S, &ret);
      w.e.now = 104 * S; expect(reload_state_flow(w.e, &rem) == 0u && rem == 0u && w.e.rwEv[kRwClosed267] == 1 && !w.e.rwin.open, "R1b: `pipereload 1` after the settle time reads closed, logged once as closed");
      expect(reload_state_flow(w.e, &rem) == 0u && w.e.rwEv[kRwClosed267] == 1, "R1b: and not logged again"); }
    { // the 15 s expiry still caps everything: a close at 15 s - 1 ns is tolerated even when it comes after other tolerated closes; at 15 s the window is expired and the close disarms
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true); w.e.now = 115 * S - 1ull;
      expect(!ws_client_closed_flow(w.e, true) && w.e.armedFlag && w.e.rwEv[kRwTolerated] == 2, "R1b: a second close 1 ns before the 15 s is tolerated (no 267 seen: only the expiry bounds the window)");
      w.e.now = 115 * S; expect(ws_client_closed_flow(w.e, true) && !w.e.armedFlag && w.e.rwEv[kRwExpired] == 1 && w.e.rwEv[kRwClosed267] == 0, "R1b: a third close at exactly 15 s: expired -> K1 disarms"); }
    { // K2 is never overridden by the extended tolerance: HUNG after two tolerated closes disarms; a close while HUNG is not tolerated; a settled-window look under HUNG still shuts it
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true); (void)ws_client_closed_flow(w.e, true); w.e.hungFlag = true;
      expect(hung_latched_flow(w.e) && !w.e.armedFlag && w.e.autoCause == kAdHung && !w.e.rwin.open, "R1b: HUNG after two tolerated closes: K2 disarms and shuts the window");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); (void)ws_client_closed_flow(v.e, true); v.e.hungFlag = true;
      expect(ws_client_closed_flow(v.e, true) && !v.e.armedFlag && v.e.autoCause == kAdWsClose && v.e.rwEv[kRwTolerated] == 1, "R1b: the second close while HUNG is NOT tolerated"); }
    { // a re-issued verb forgets the 267 and the last-close stamp of the previous window
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0, rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true); init267(w, 101 * S, &ret); w.e.now = 102 * S; reload_open_flow(w.e);
      expect(w.e.rwin.seen267 == 0 && w.e.rwin.closeSeen == 0 && reload_state_flow(w.e, &rem) == 1u, "R1b: a second `pipereload` clears the remembered slot 267 and the tolerated close");
      init267(w, 110 * S, &ret); expect(w.e.rwin.open && w.e.rwEv[kRwClosed267] == 0, "R1b: after the re-issue a slot 267 before any close does not close the window"); }
    { // pure edges of reload_settled
      expect(!reload_settled(false, true, 100 * S, 200 * S) && !reload_settled(true, false, 100 * S, 200 * S) && !reload_settled(true, true, 100 * S, 100 * S + kReloadSettleNs - 1ull) && reload_settled(true, true, 100 * S, 100 * S + kReloadSettleNs) &&
             reload_settled(true, true, 100 * S, 100 * S + kReloadSettleNs + 1ull) && reload_settled(true, true, 100 * S, 99 * S), "R1b: reload_settled needs both the close and the 267; exactly the settle time is settled; a clock that went backwards is settled (fails towards containment)"); }
    { // an ADMIN close (operator tools) neither disarms nor consumes the window
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t rem = 0; reload_open_flow(w.e);
      expect(!ws_client_closed_flow(w.e, false) && w.e.armedFlag && w.e.rwEv[kRwTolerated] == 0 && reload_state_flow(w.e, &rem) == 1u, "R1: an admin client's close leaves the window untouched (state 1, nothing tolerated)"); }
    { // the pipe must still be armed at the new client's slot 267 for the window to close
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t ret = 0, rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true);
      w.e.armedFlag = false; w.e.armByte = 0; init267(w, 102 * S, &ret);
      expect(w.e.rwEv[kRwClosed267] == 0 && reload_state_flow(w.e, &rem) == 2u && w.e.cg.n == 0, "R1: a slot 267 while NOT armed does not close the window and is not counted");
      w.e.armedFlag = true; w.e.armByte = 1; init267(w, 104 * S, &ret); expect(w.e.rwEv[kRwClosed267] == 1 && reload_state_flow(w.e, &rem) == 0u, "R1: the first slot 267 with the pipe armed closes it"); }
    { // not armed: the close tolerates nothing (the verb on a disarmed pipe is harmless) and the window stays open until its 15 s
      World w; w.e.armedFlag = false; w.e.armByte = 0; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t rem = 0; reload_open_flow(w.e);
      expect(!ws_client_closed_flow(w.e, true) && w.e.rwEv[kRwTolerated] == 0 && reload_state_flow(w.e, &rem) == 1u && w.e.autoDisarms == 0, "R1: a close on a disarmed pipe tolerates nothing and consumes nothing"); }
    { // HUNG precedence: K2 is unchanged, and while HUNG neither the close nor the 267 is honoured
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); w.e.hungFlag = true;
      expect(hung_latched_flow(w.e) && !w.e.armedFlag && w.e.autoCause == kAdHung && w.e.autoDisarms == 1 && !w.e.rwin.open, "R1: HUNG with the window open disarms (K2 unchanged) and shuts the window");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); v.e.hungFlag = true;
      expect(ws_client_closed_flow(v.e, true) && !v.e.armedFlag && v.e.autoCause == kAdWsClose && v.e.rwEv[kRwTolerated] == 0, "R1: a close while HUNG is NOT tolerated even inside the window");
      World u; u.e.armByte = 1; u.e.clockStep = 0; u.e.now = 100 * S; uint64_t ret = 0; reload_open_flow(u.e); u.e.hungFlag = true; init267(u, 101 * S, &ret);
      expect(u.e.cg.n == 1 && u.e.rwEv[kRwClosed267] == 0, "R1: a slot 267 while HUNG is counted by K3 (the window is not honoured)"); }
    { // every disarm shuts the window: arm 0, K3 trip (outside any window), withdraw
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); expect_u("R1: arm 0", arm_flow(w.e, 0), kOk); expect(!w.e.rwin.open && !w.e.rwin.closeSeen, "R1: `pipearm 0` shuts the window");
      World v; v.e.armByte = 1; v.e.clockStep = 0; v.e.now = 100 * S; reload_open_flow(v.e); withdraw_flow(v.e); expect(!v.e.rwin.open, "R1: withdraw shuts the window");
      World u; u.e.armByte = 1; u.e.before = good_probe(); u.e.layoutOk = true; u.e.clockStep = 0; u.e.now = 100 * S; reload_open_flow(u.e); u.e.armedFlag = false; u.e.armByte = 0;
      expect_u("R1: explicit arm 1", arm_flow(u.e, 1), kOk); expect(u.e.armedFlag, "R1: arm 1 arms (the window itself never arms)"); }
    { // re-issuing the verb restarts the 15 s and clears a stale 'close seen'
      World w; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; uint64_t rem = 0; reload_open_flow(w.e); (void)ws_client_closed_flow(w.e, true); w.e.now = 110 * S; reload_open_flow(w.e);
      expect(reload_state_flow(w.e, &rem) == 1u && rem == kReloadWindowNs && w.e.rwin.opens == 2, "R1: a second `pipereload` restarts the window (state 1, a full 15 s)"); }
    { // the hook table path: latch OFF hands nothing to the window; slot 267 through hook_dispatch inside the window is handled and not counted
      World w; w.e.latch = false; w.e.armByte = 1; w.e.clockStep = 0; w.e.now = 100 * S; reload_open_flow(w.e); uint64_t ret = 0;
      expect(!ws_client_closed_flow(w.e, true) == true && w.e.armedFlag && init267(w, 101 * S, &ret) == 0 && w.e.cg.n == 0 && w.e.rwEv[kRwTolerated] == 0, "R1: latch OFF -> the close and the hooks do nothing, window or not (0.0.616 behaviour)"); }

    // ---- K4: after an auto-disarm the arm stays refused until an explicit `pipearm 1`; the cause is exposed
    expect(auto_cause_name(kAdWsClose) != nullptr && std::string(auto_cause_name(kAdWsClose)).find("WindowServer GPU client closed while armed") != std::string::npos && std::string(auto_cause_name(kAdNone)) == "none", "K4: cause names");
    expect(disp_flags(true, true, false, true, true, true, 0) == 59 && disp_flags(true, true, false, true, true, true, kAdWsClose) == (59u | 64u | (2u << 8)) && disp_flags(true, true, false, true, true, true, kAdHung) == (59u | 64u | (3u << 8)) &&
           disp_flags(true, false, false, false, false, false, kAdCount) == 1u && disp_flags(true, false, true, false, false, false) == 5u, "K4: the flags word: bit 64 and the cause in bits 8..11 only with a cause; 0.0.616 bits unchanged");
    { World w; w.e.before = good_probe(); w.e.layoutOk = true; (void)ws_client_closed_flow(w.e, true); uint64_t ret = 0;
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
    expect(kTxnVblTime == 0x178 && kTxnVblNext == 0x188 && kTxnSize == 0x1b8 && kVblCount == 9 && kLastAction == 99, "V1: the literal offsets of the two timestamp words (AMDRadeonX6000 executeTransaction 0xbdcdbe2 / 0xbdcdbed) and the action bound");
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
      const size_t pf = flw.find("uint32_t perform_flow("), pi = flw.find("perform_inner(e, txn, &bytes, pipe)", pf), pv = flw.find("vbl_stamp_flow(e, txn, r, pipe)", pf), pn = flw.find("e.note_perform(r, bytes", pf);
      expect(pf != std::string::npos && pi != std::string::npos && pv != std::string::npos && pn != std::string::npos && pi < pv && pv < pn && count_of(flw, "vbl_stamp_flow(e, txn") == 1, "V1: perform_flow stamps once, after perform_inner (both of its paths) and before it returns");
      const size_t vf = flw.find("uint32_t vbl_stamp_flow("), vo = flw.find("e.vbl_on()", vf), vc = flw.find("e.class_derives(txn, kTxnClass)", vf), vh = flw.find("e.hung()", vf), vs2 = flw.find("e.vbl_sample(&s)", vf), vw = flw.find("e.wr64(txn + kTxnVblTime", vf);
      expect(vf != std::string::npos && vo > vf && vc > vo && vh > vc && vs2 > vh && vw > vs2 && count_of(flw, "e.wr64(txn + kTxn") == 2 && flw.find("kTxnVblNext, p.next") != std::string::npos, "V1: switch, identity, HUNG, sample, plan, then the two writes (and only those two)");
      expect(gl.find("bool vbl_on() { return __atomic_load_n(&gD.vblOff, __ATOMIC_ACQUIRE) == 0u; }") != std::string::npos && gl.find("__atomic_store_n(&gD.vblOff, arg ? 0u : 1u, __ATOMIC_RELEASE);") != std::string::npos &&
             gl.find("action == n48disp::kActVbl") != std::string::npos && gl.find("pipe stat page 4 (vblank timestamps)") != std::string::npos && gl.find(std::string("} else if (arg == 4ull) {") + std::string(78, ' ') + "// 0.0.618 (V2), page 4") != std::string::npos, "V2: the switch defaults ON (zero-initialised vblOff), `pipevbl 0|1` toggles it live, stat page 4 shows it");
      expect(gl.find("nanoseconds_to_absolutetime(1000000000ull, &perSec);") != std::string::npos && count_of(gl, "s->numer = 1000000000u; s->denom = (uint32_t)perSec;") == 2 && gl.find("n48dcn::vblSample(&s->periodNs, &s->delayNs, &s->nowAbs)) return false;\n        s->numer = 1000000000u; s->denom = (uint32_t)perSec;") != std::string::npos && gl.find("n48dcn::vblSampleInst(inst, g.w, g.h, g.pixHz, &s->periodNs, &s->delayNs, &s->nowAbs)) return false;\n        s->numer = 1000000000u; s->denom = (uint32_t)perSec;") != std::string::npos && gl.find("strcmp(c, name) == 0") != std::string::npos && gl.find("for (uint32_t depth = 0; mc && depth < 12u; ++depth, mc = mc->getSuperClass()) {") != std::string::npos, "V1: the glue reads the real timebase and walks the superclass chain for the identity");
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
        expect(glue.find("PE_parse_boot_argn(\"navi48-metal-disp\"") != std::string::npos && glue.find("PE_parse_boot_argn(\"navi48-m6\"") != std::string::npos && count_of(glue, "PE_parse_boot_argn") == 4 && count_of(glue, "PE_parse_boot_argn(\"navi48-m6\"") == 1 && count_of(glue, "PE_parse_boot_argn(\"navi48-m6flip\"") == 1 && count_of(glue, "PE_parse_boot_argn(\"navi48-m6flip1\"") == 1, "the boot-args navi48-metal-disp, (0.0.659) navi48-m6, (0.0.661) navi48-m6flip and (0.0.662) navi48-m6flip1 are the only boot-args the display glue reads, each read at ONE site");
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
        expect(glue.find("static_assert(n48disp::kPfCount == 23u") != std::string::npos && glue.find("out[11] = n48disp::dur_avg(gD.dur); out[12] = gD.dur.max;") != std::string::npos && glue.find("out[10] = gD.dur") == std::string::npos, "0.0.616: stat page 2 carries reasons 12..19 (out[10] is reason 19, not the copy time minimum)");
    }
    if (!nub.empty()) {
        expect(nub.find("n48disp::ops_shape(") != std::string::npos && nub.find("n48disp_latched_on()") != std::string::npos, "the nub publishes the table shape chosen by the latch");
        expect(nub.find("op_disp_hook") != std::string::npos && nub.find("op_pci_device") != std::string::npos, "the ABI-2 members are wired");
        expect(nub.find("*(const N48MetalOps **)param1 = n48disp::ops_shape(n48disp_latched_on()).dispFlags != 0u ? &gOps : &gOpsOff;") != std::string::npos, "callPlatformFunction hands out the display-OFF table (display members zero) unless the latch is ON (the table the aux kext sees decides display off)");
        expect(nub.find("OSNumber *abi = OSNumber::withNumber((uint64_t)shape.abi, 32);") != std::string::npos && nub.find("out[2] = shape.abi; out[3] = (uint64_t)shape.size;") != std::string::npos, "the published ABI / size properties follow the latch-chosen shape");
        expect(nub.find("664u") != std::string::npos && nub.find("658u") == std::string::npos && nub.find("613u") == std::string::npos, "the ops table carries the build 664");
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
      if (!ncl.empty()) expect(count_of(ncl, "n48disp_on_ws_client_closed(") == 2 && count_of(ncl, "n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, \"clientClose\");") == 1 && count_of(ncl, "n48disp_on_ws_client_closed(wsSession); amdgpu::n1c_unmap_for_close(amdgpu::N1cRef{ sessSlot, sessId }, this); amdgpu::n1c_close(amdgpu::N1cRef{ sessSlot, sessId }, \"stop\");") == 1 &&
                               ncl.find("amdgpu::n1c_close(ref, \"attach failed\")") != std::string::npos && ncl.find("wsSession = d.reason == n48native::policy::kReasonWindowServer;") != std::string::npos && count_of(ncl, "n48disp_on_ws_client_closed(adminClient)") == 0,
                               "K1: clientClose and stop disarm (0.0.627: with the client's WindowServer-SESSION flag, set from the policy reason kReasonWindowServer, never the admin flag) BEFORE the 0.0.641 unmap sweep and n1c_close, on the same line; the create-failure paths are unchanged");
      { const size_t h0 = nub.find("void Navi48MetalNub::hungLatched() {"), a = nub.find("n48disp_on_hung();", h0), g = nub.find("if (!gLock)", h0);
        expect(h0 != std::string::npos && a != std::string::npos && g != std::string::npos && a < g, "K2: hungLatched() disarms (n48disp_on_hung) before its nub / lock handling"); }
      expect(glue.find("void n48disp_on_ws_client_closed(bool closingIsWsSession) {\n    if (!n48disp_latched_on()) return;\n    KernelEnv env;\n    (void)n48disp::ws_client_closed_flow(env, closingIsWsSession);") != std::string::npos &&
             glue.find("void n48disp_on_hung(void) {\n    if (!n48disp_latched_on()) return;\n    KernelEnv env;\n    (void)n48disp::hung_latched_flow(env);") != std::string::npos, "K1/K2: the entry points are latch-gated and call the tested flows");
      expect(ghdr.find("void     n48disp_on_ws_client_closed(bool closingIsWsSession);") != std::string::npos && ghdr.find("void     n48disp_on_hung(void);") != std::string::npos, "K1/K2: declared in native_disp.h");
      expect(glue.find("auto-disarm: %s; the gate byte is written back to 0") != std::string::npos && glue.find("__atomic_store_n(&gD.autoCause, cause <") != std::string::npos && glue.find("auto-disarmed: %s (count %llu)") != std::string::npos &&
             glue.find("__atomic_load_n(&gD.autoCause, __ATOMIC_ACQUIRE));") != std::string::npos && glue.find("void clear_autodisarm() { __atomic_store_n(&gD.autoCause, 0u, __ATOMIC_RELEASE); }") != std::string::npos, "K4: the cause is stored, cleared by an explicit arm, in the flags word and in the stat line");
      expect(std::string(auto_cause_name(kAdWsClose)).find("the WindowServer GPU client closed while armed") == 0, "K1: the log text names the cause");
      { // R1 (0.0.619): the wiring of the operator restart window (ordering and reachability are tested above on the real flows; these pin the glue, the verb plumbing and the CLI)
        const size_t k1 = flow.find("template <class E> bool ws_client_closed_flow("), k1e = flow.find("template <class E> bool hung_latched_flow(");
        const std::string k1s = k1 != std::string::npos && k1e != std::string::npos && k1e > k1 ? flow.substr(k1, k1e - k1) : std::string();
        const size_t c = k1s.find("ws_close_disarms(e.latch_on(), closingIsWsSession)"), t = k1s.find("reload_tolerate_close(e)"), d = k1s.find("return auto_disarm_flow(e, kAdWsClose);");
        expect(c != std::string::npos && t != std::string::npos && d != std::string::npos && c < t && t < d, "R1: K1 order: the uid-88 / latch check, then the window, then the disarm");
        const size_t h1 = flow.find("template <class E> bool hung_latched_flow("), h2 = flow.find("// ---- slot 267:");
        expect(h1 != std::string::npos && h2 > h1 && flow.substr(h1, h2 - h1).find("reload_") == std::string::npos, "R1: K2 (hung_latched_flow) never consults the window");
        expect(flow.find("if (counted && e.armed() && !reload_on_init_fb(e) && guard_on_init_fb(e.guard(), e.now_ns()))") != std::string::npos && flow.find("const bool counted = n48m6::restart_guard_counts(m6, m6 ? e.pipe_inst(pipe) : n48m6::kInstDp);") != std::string::npos, "R1: K3: the window is consulted before the guard counts, and only while armed (0.0.660: and the exemption is the pure restart_guard_counts: only a KNOWN monitor A / monitor B pipe)");
        {   // 0.0.621: every close is tolerated and stamps the settle time BEFORE closeSeen; the 267 closes only through reload_settled; the lazy settle runs in the common honoured check
            const size_t tc = flow.find("template <class E> bool reload_tolerate_close("), tce = flow.find("// K3 side:");
            const std::string tcs = tc != std::string::npos && tce > tc ? flow.substr(tc, tce - tc) : std::string();
            const size_t st = tcs.find("__atomic_store_n(&w.lastTolNs, e.now_ns(), __ATOMIC_RELEASE);"), sc = tcs.find("__atomic_store_n(&w.closeSeen, 1u, __ATOMIC_RELEASE);");
            expect(st != std::string::npos && sc != std::string::npos && st < sc && tcs.find("closeSeen, __ATOMIC_ACQUIRE") == std::string::npos && tcs.find("tolerated, 1ull") != std::string::npos, "R1b: reload_tolerate_close stamps the LAST tolerated close before it sets closeSeen, and has no 'first close only' test");
            const size_t hn = flow.find("template <class E> bool reload_honoured_now("), hne = flow.find("// The verb: open a fresh one-shot window");
            const std::string hns = hn != std::string::npos && hne > hn ? flow.substr(hn, hne - hn) : std::string();
            expect(hns.find("if (reload_settled(") != std::string::npos && hns.find("reload_close_settled(e)") != std::string::npos && hns.find("return reload_honoured(true, e.hung());") != std::string::npos && hns.find("reload_settled(") < hns.find("return reload_honoured(true, e.hung());"), "R1b: every hook's honoured check evaluates the settle rule before it answers");
            const size_t on = flow.find("template <class E> bool reload_on_init_fb("), one = flow.find("// ---- 0.0.616: the plane-0 accessors");
            const std::string ons = on != std::string::npos && one > on ? flow.substr(on, one - on) : std::string();
            expect(ons.find("reload_settled(true, true, ") != std::string::npos && ons.find("w.seen267, 1u") != std::string::npos && ons.find("compare_exchange") == std::string::npos, "R1b: the slot-267 hook closes the window only through the settle rule (reload_close_settled), never by itself");
        }
        expect(glue.find("n48disp::ReloadWin &rw() { return gD.reload; }") != std::string::npos && glue.find("if (arg == 0ull) n48disp::reload_open_flow(env);") != std::string::npos && glue.find("action == n48disp::kActReload") != std::string::npos &&
               glue.find("operator restart window: client close tolerated") != std::string::npos && glue.find("operator restart window: closed") != std::string::npos && glue.find("operator restart window: expired") != std::string::npos &&
               glue.find("operator restart window: opened") != std::string::npos, "R1: the glue owns the state, the verb opens it through the tested flow, and the four log lines exist");
        expect(brg.find("action == 89 || action == 90 || action == n48disp::kActM6Stat) {") != std::string::npos, "R1: accelExperiment routes 90 to n48disp_verb");
        expect(cli.find("in = 90;") != std::string::npos && cli.find("\"pipereload\"") != std::string::npos && cli.find("//     killall -9 WindowServer            (within 15 s;") != std::string::npos && cli.find("pipereload [0|1]") != std::string::npos &&
               cli.find("in == 90 ? \"pipereload\"") != std::string::npos && cli.find("|| in == 89 || in == 90) {") != std::string::npos, "R1: navi48test has pipereload (action 90), prints the window and documents the operator sequence");
      }
    }
    if (!brg.empty()) expect(brg.find("pci->setProperty(\"VRAM,totalMB\", static_cast<uint64_t>(vramMB), 32);") != std::string::npos && brg.find("pci->setProperty(\"VRAM,totalsize\", d);") != std::string::npos && brg.find("setProperty(\"VRAM,TotalMB\", static_cast<uint64_t>(vramMB), 32);") != std::string::npos &&
                              count_of(brg, "publishNativeVramOn(") == 2 && brg.find("if (amdgpu::native_s1b_latched()) publishNativeVramOn(pciDevice, vramMB);") != std::string::npos && brg.find("const uint64_t bytes = static_cast<uint64_t>(vramMB) << 20;") != std::string::npos,
                              "K5: native boots only (inside the native latch) the IOPCIDevice also gets VRAM,totalMB (32-bit) and VRAM,totalsize (8-byte OSData, bytes); the old VRAM,TotalMB stays");
    expect(count_of(plist, "0.0.664") == 2 && plist.find("0.0.612") == std::string::npos, "Info.plist is 0.0.664, carried twice");
    expect(s1ch.find("kN1cKextBuild = 664;") != std::string::npos, "the native client reports build 660");
    expect(ops.find("#define N48_METAL_ABI         3u") != std::string::npos && ops.find("N48_VC_DISPLAYPIPE    7u") != std::string::npos, "the ops header is ABI 3 (since 0.0.656) with the display class");
    expect(pure.find("kNeedFacts") != std::string::npos && flow.find("e.facts_add(kNeedFacts)") != std::string::npos, "adopt adds the needed facts");
    expect(count_of(flow, "e.md_prepare(") == 1 && count_of(flow, "mdc_teardown(e);") == 5 && flow.find("if (inst == n48m6::kInstDp) (void)submit_prepare(e, args[0]);") != std::string::npos && count_of(flow, "mdc_acquire(e.mdc(), md)") == 1, "0.0.616: ONE prepare in the whole flow (mdc_ensure, from submit), teardown after every disarm path, submit prepares, perform acquires");
    expect(flow.find("e.note_null_pipe()") != std::string::npos && flow.find("guard_reset(e.guard());") != std::string::npos && flow.find("guard_on_perform(e.guard());") != std::string::npos && flow.find("guard_on_init_fb(e.guard(), e.now_ns())") != std::string::npos, "0.0.615: the flows call the NULL-pipe note and the three guard steps");
    expect(count_of(flow, "__atomic_") >= 8 && flow.find("IOLock") == std::string::npos && flow.find("lck_") == std::string::npos, "G1: the guard is atomics only, no lock");
    expect(bhpp.find("consoleWrite") != std::string::npos && bhpp.find("consoleGeometry") != std::string::npos, "the bring-up class exposes the console region and write");
    // the byte-identity of the ops header with the aux kext's copy (the aux tree root is the optional 2nd argument; default: this tree)
    { const std::string auxroot = argc > 2 ? argv[2] : root; const std::string aux = slurp(auxroot + "/tools/native/navi48accel/src/Navi48MetalOps.h");
      if (aux.empty()) std::printf("NOTE: the aux kext copy of Navi48MetalOps.h is not present under %s\n", auxroot.c_str()); else expect(aux == ops, "Navi48MetalOps.h is byte-identical in the bring-up kext and the aux kext"); }

    // =========================================================================================================================================================
    // M. 0.0.652 (M5): the display nub and `fbpublish 2` (an internal design note "M5 build spec", sections 1 and 3)
    // =========================================================================================================================================================
    {
        const std::string fbp = slurp(K + "src/amd/native_fb_pure.h"), dop = slurp(K + "src/Navi48DisplayOps.h"), dnub = slurp(K + "src/Navi48DisplayNub.cpp"), dnubh = slurp(K + "src/Navi48DisplayNub.hpp"), mk = slurp(K + "Makefile"),
                          s1cc = slurp(K + "src/amd/native_s1c.cpp"), mpure = slurp(K + "src/amd/native_metal_pure.h");
        expect(!fbp.empty() && !dop.empty() && !dnub.empty() && !dnubh.empty() && !mk.empty(), "M: the new files are readable");
        // ---- statuses
        { std::set<std::string> names; for (uint32_t st = 0; st < n48fb::kStatusCount; st++) { const std::string n = n48fb::status_name(st); expect(n != "unknown", "M: every fbpublish status has a name"); names.insert(n); expect(n == n48disp_fbp_name(st), "M: the pure and the shared text table agree"); }
          expect_u("M: all 17 fbpublish statuses have DISTINCT texts", names.size(), n48fb::kStatusCount); expect(std::string(n48fb::status_name(n48fb::kStatusCount)) == "unknown", "M: past the last status the name is unknown"); }
        expect(n48fb::kOk == 0u && n48fb::kBadArg == 1u && n48fb::kOff == 2u && n48fb::kNoDevice == 3u && n48fb::kBusy == 4u && n48fb::kNotHeld == 5u && n48fb::kExists == 6u && n48fb::kPipeAdopted == 7u && n48fb::kWsOpen == 8u && n48fb::kGate == 9u &&
               n48fb::kEdidRead == 10u && n48fb::kEdidBad == 11u && n48fb::kEdidMismatch == 12u && n48fb::kAperture == 13u && n48fb::kSnapshot == 14u && n48fb::kPublishFailed == 15u && n48fb::kMetalNub == 16u && n48fb::kStatusCount == 17u, "M: the status numbers (0.0.653: kMetalNub = 16)");
        // ---- the argument and the latch
        { bool ok = true; for (uint64_t a = 0; a < 0x2000; a++) if (n48fb::arg_ok(a) != (a == 2ull || a == 1ull)) ok = false; expect(ok && !n48fb::arg_ok(0x10002ull) && !n48fb::arg_ok(0x100000002ull) && !n48fb::arg_ok(0x101ull), "M: arg_ok accepts exactly 2 (the monitor B) and 1 (the monitor A, 0.0.658)"); }
        expect(n48fb::fb2_on(true, 1u) && !n48fb::fb2_on(true, 0u) && !n48fb::fb2_on(true, 2u) && !n48fb::fb2_on(false, 1u) && !n48fb::fb2_on(false, 0u) && !n48fb::fb2_on(true, 0xFFFFFFFFu), "M: navi48-fb2 is on only when present AND == 1");
        // ---- phase 1: one fault at a time, then the priority
        { n48fb::Pre good = { true, true, false, true, true, true, false, false, false }; expect_u("M: pre: all good", n48fb::pre_verdict(good), n48fb::kOk);
          { n48fb::Pre p = good; p.argOk = false; expect_u("M: pre: bad argument", n48fb::pre_verdict(p), n48fb::kBadArg); }
          { n48fb::Pre p = good; p.latched = false; expect_u("M: pre: no boot-arg (navi48-fb2)", n48fb::pre_verdict(p), n48fb::kOff); }
          { n48fb::Pre p = good; p.nubExists = true; expect_u("M: pre: a nub exists", n48fb::pre_verdict(p), n48fb::kExists); }
          { n48fb::Pre p = good; p.held = false; expect_u("M: pre: NOT HELD (no hold)", n48fb::pre_verdict(p), n48fb::kNotHeld); }
          { n48fb::Pre p = good; p.pinned = false; expect_u("M: pre: held but not pinned", n48fb::pre_verdict(p), n48fb::kNotHeld); }
          { n48fb::Pre p = good; p.devOk = false; expect_u("M: pre: no bound device", n48fb::pre_verdict(p), n48fb::kNoDevice); }
          { n48fb::Pre p = good; p.pipeAdopted = true; expect_u("M: pre: the pipe is ADOPTED", n48fb::pre_verdict(p), n48fb::kPipeAdopted); }
          { n48fb::Pre p = good; p.wsOpen = true; expect_u("M: pre: a WindowServer native session is OPEN", n48fb::pre_verdict(p), n48fb::kWsOpen); }
          { n48fb::Pre p = good; p.metalNub = true; expect_u("M: pre (0.0.653): the Metal nub was published earlier this boot", n48fb::pre_verdict(p), n48fb::kMetalNub); }
          expect(std::string(n48fb::status_name(n48fb::kMetalNub)).find("Metal nub") != std::string::npos && std::string(n48fb::status_name(n48fb::kMetalNub)).find("REFUSED") != std::string::npos, "M: the Metal-nub refusal has its own CLI text");
          // priority: arg < latch < exists < held/pinned < device < adopted < ws < metal nub
          { n48fb::Pre p = { false, false, true, false, false, false, true, true, true }; expect_u("M: pre: priority 1 (arg beats all)", n48fb::pre_verdict(p), n48fb::kBadArg); p.argOk = true; expect_u("M: pre: priority 2 (latch)", n48fb::pre_verdict(p), n48fb::kOff);
            p.latched = true; expect_u("M: pre: priority 3 (exists)", n48fb::pre_verdict(p), n48fb::kExists); p.nubExists = false; expect_u("M: pre: priority 4 (held)", n48fb::pre_verdict(p), n48fb::kNotHeld); p.held = true; p.pinned = true;
            expect_u("M: pre: priority 5 (device)", n48fb::pre_verdict(p), n48fb::kNoDevice); p.devOk = true; expect_u("M: pre: priority 6 (adopted)", n48fb::pre_verdict(p), n48fb::kPipeAdopted); p.pipeAdopted = false; expect_u("M: pre: priority 7 (ws)", n48fb::pre_verdict(p), n48fb::kWsOpen); p.wsOpen = false; expect_u("M: pre: priority 8 (Metal nub, 0.0.653)", n48fb::pre_verdict(p), n48fb::kMetalNub); p.metalNub = false; expect_u("M: pre: priority 9 (ok)", n48fb::pre_verdict(p), n48fb::kOk); } }
        // ---- phase 2
        expect_u("M: live: clean", n48fb::live_verdict(0u), n48fb::kOk); expect_u("M: live: any hold-verdict bit refuses", n48fb::live_verdict(1u), n48fb::kGate); expect_u("M: live: a failed read (0xFFFFFFFF) refuses", n48fb::live_verdict(0xFFFFFFFFu), n48fb::kGate);
        { n48fb::Data good = { 0u, true, true, true, true, true, true }; expect_u("M: data: all good", n48fb::data_verdict(good), n48fb::kOk);
          { n48fb::Data d = good; d.edidStatus = 7u; expect_u("M: data: DDC failed", n48fb::data_verdict(d), n48fb::kEdidRead); }
          { n48fb::Data d = good; d.block0Sum = false; expect_u("M: data: block 0 checksum", n48fb::data_verdict(d), n48fb::kEdidBad); } { n48fb::Data d = good; d.block0Hdr = false; expect_u("M: data: block 0 header", n48fb::data_verdict(d), n48fb::kEdidBad); }
          { n48fb::Data d = good; d.block1Sum = false; expect_u("M: data: block 1 checksum", n48fb::data_verdict(d), n48fb::kEdidBad); }
          { n48fb::Data d = good; d.propPresent = false; d.block0EqProp = false; expect_u("M: data: RDNA4FB's EDID,DDC3 missing", n48fb::data_verdict(d), n48fb::kEdidMismatch); }
          { n48fb::Data d = good; d.block0EqProp = false; expect_u("M: data: block 0 differs from RDNA4FB's", n48fb::data_verdict(d), n48fb::kEdidMismatch); }
          { n48fb::Data d = good; d.apertureOk = false; expect_u("M: data: aperture", n48fb::data_verdict(d), n48fb::kAperture); }
          { n48fb::Data d = { 9u, false, false, false, false, false, false }; expect_u("M: data: priority (DDC status beats the rest)", n48fb::data_verdict(d), n48fb::kEdidRead); d.edidStatus = 0u; expect_u("M: data: priority (EDID bad beats mismatch)", n48fb::data_verdict(d), n48fb::kEdidBad); } }
        { n48fb::Post p = { 0u, { 0u, true, true, true, true, true, true } }; expect_u("M: post: clean", n48fb::post_verdict(p), n48fb::kOk); p.holdBad = 4u; p.d.edidStatus = 3u; expect_u("M: post: the live gate beats the EDID", n48fb::post_verdict(p), n48fb::kGate); }
        // ---- the aperture: bar0Phys + off[0], 14,745,600 bytes, 64 KiB aligned, inside BAR0
        { const uint64_t B = 0x80000000ull, SZ = 0x10000000ull;
          expect(n48fb::aperture_ok(B, SZ, 0x01800000ull) && n48fb::aperture_ok(B, SZ, 0u), "M: aperture: inside BAR0 (offset 0x01800000 and 0)");
          expect(!n48fb::aperture_ok(0u, SZ, 0x01800000ull), "M: aperture: BAR0 base 0"); expect(!n48fb::aperture_ok(B + 0x1000u, SZ, 0x01800000ull), "M: aperture: BAR0 base unaligned");
          expect(!n48fb::aperture_ok(B, SZ, 0x01800000ull + 0x1000ull), "M: aperture: offset not 64 KiB aligned"); expect(!n48fb::aperture_ok(B, SZ, SZ), "M: aperture: offset at the end");
          expect(!n48fb::aperture_ok(B, SZ, SZ - N48_DISP_BYTES + 0x10000ull), "M: aperture: the buffer runs past BAR0 by 64 KiB"); expect(n48fb::aperture_ok(B, SZ, SZ - N48_DISP_BYTES), "M: aperture: the last placement that fits");
          expect(!n48fb::aperture_ok(B, N48_DISP_BYTES - 1u, 0u), "M: aperture: BAR0 smaller than the buffer"); expect(!n48fb::aperture_ok(0xFFFFFFFFFFFF0000ull, SZ, 0x01800000ull), "M: aperture: the sum wraps"); }
        // ---- the snapshot builder
        { uint8_t e[256]; std::memset(e, 0, sizeof e); e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xFF; { uint32_t sm = 0; for (int i = 0; i < 127; i++) sm += e[i]; e[127] = (uint8_t)(0u - sm); } e[128] = 2; { uint32_t sm = 0; for (int i = 128; i < 255; i++) sm += e[i]; e[255] = (uint8_t)(0u - sm); }
          N48DispSnap sn; n48fb::snap_build(&sn, 0x80000000ull, 0x01800000ull, 0x8001800000ull, 0x8002600000ull, e);
          expect_u("M: snap_build: a valid snapshot from valid facts", n48disp_snap_valid(&sn), N48_DSV_OK);
          expect(sn.aperPhys == 0x81800000ull && sn.aperLen == 14745600ull && sn.mcA == 0x8001800000ull && sn.mcB == 0x8002600000ull && sn.index == 1u && sn.otg == 2u && sn.ddcLine == 3u && sn.w == 2560u && sn.h == 1440u && sn.pitchBytes == 10240u &&
                 sn.refresh1616 == N48_DISP_REFRESH1616 && sn.pixHz == 241500000ull && sn.edidLen == 256u && std::memcmp(sn.edid, e, 256) == 0, "M: snap_build: aperture = bar0Phys + off[0], the monitor B's mode, the EDID");
          n48fb::snap_build(&sn, 0x80000000ull, 0x01801000ull, 0x8001800000ull, 0x8002600000ull, e); expect_u("M: snap_build: an unaligned offset makes the snapshot INVALID (the validator is the second line of defence)", n48disp_snap_valid(&sn), N48_DSV_APERTURE);
          n48fb::snap_build(&sn, 0x80000000ull, 0x01800000ull, 0u, 0x8002600000ull, e); expect_u("M: snap_build: mcA 0 is invalid", n48disp_snap_valid(&sn), N48_DSV_MC);
          e[20] ^= 1; n48fb::snap_build(&sn, 0x80000000ull, 0x01800000ull, 0x8001800000ull, 0x8002600000ull, e); expect_u("M: snap_build: a corrupted EDID is invalid", n48disp_snap_valid(&sn), N48_DSV_EDID_SUM); }
        // the shared snapshot validator, driven from snap_build's output (the bring-up side of what the aux kext checks again before it starts)
        { uint8_t e[256]; std::memset(e, 0, sizeof e); e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xFF; { uint32_t sm = 0; for (int i = 0; i < 127; i++) sm += e[i]; e[127] = (uint8_t)(0u - sm); } e[128] = 2; { uint32_t sm = 0; for (int i = 128; i < 255; i++) sm += e[i]; e[255] = (uint8_t)(0u - sm); }
          N48DispSnap g; n48fb::snap_build(&g, 0x80000000ull, 0x01800000ull, 0x8001800000ull, 0x8002600000ull, e); expect_u("M: validator: good", n48disp_snap_valid(&g), N48_DSV_OK);
          { N48DispSnap x = g; x.mcB = x.mcA; expect_u("M: validator: the two buffers must differ", n48disp_snap_valid(&x), N48_DSV_MC); } { N48DispSnap x = g; x.mcB = 0; expect_u("M: validator: mcB 0", n48disp_snap_valid(&x), N48_DSV_MC); }
          { N48DispSnap x = g; x.mcA += 0x1000; expect_u("M: validator: mcA unaligned", n48disp_snap_valid(&x), N48_DSV_MC); } { N48DispSnap x = g; x.mcB += 0x1000; expect_u("M: validator: mcB unaligned", n48disp_snap_valid(&x), N48_DSV_MC); }
          { N48DispSnap x = g; x.edid[200] ^= 1; expect_u("M: validator: block 1 checksum", n48disp_snap_valid(&x), N48_DSV_EDID_SUM); } { N48DispSnap x = g; x.edid[40] ^= 1; expect_u("M: validator: block 0 checksum", n48disp_snap_valid(&x), N48_DSV_EDID_SUM); }
          { N48DispSnap x = g; x.edid[0] = 1; x.edid[127] = (uint8_t)(x.edid[127] - 1); expect_u("M: validator: block 0 header", n48disp_snap_valid(&x), N48_DSV_EDID_HDR); } { N48DispSnap x = g; x.edidLen = 128; expect_u("M: validator: one block only", n48disp_snap_valid(&x), N48_DSV_EDID_LEN); }
          { N48DispSnap x = g; x.index = 3; expect_u("M: validator: index", n48disp_snap_valid(&x), N48_DSV_INDEX); } { N48DispSnap x = g; x.w = 1920; expect_u("M: validator: width", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); } { N48DispSnap x = g; x.aperLen -= 0x10000; expect_u("M: validator: aperture length", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = g; x.aperPhys = 0; expect_u("M: validator: no aperture", n48disp_snap_valid(&x), N48_DSV_APERTURE); } expect_u("M: validator: NULL", n48disp_snap_valid(nullptr), N48_DSV_NULL); }
        expect(n48fb::snapshot_hook_verdict(true, true, true) == 0 && n48fb::snapshot_hook_verdict(false, true, true) != 0 && n48fb::snapshot_hook_verdict(true, false, true) != 0 && n48fb::snapshot_hook_verdict(true, true, false) != 0, "M: the snapshot hook copies only for the nub, only when published, only into a buffer");
        expect(n48fb::may_publish(false) && !n48fb::may_publish(true), "M: a second publish is refused");
        // ---- the shared header: shapes, constants, the ops check, byte identity with the aux copy
        expect_u("M: N48DispSnap is 336 bytes", sizeof(N48DispSnap), 336); expect_u("M: N48DispOps is 72 bytes (ABI 1 and 2)", sizeof(N48DispOps), 72); expect_u("M: the ops magic is 'N48D'", N48_DISP_OPS_MAGIC, 0x4438344Eu); expect_u("M: ABI 2 (0.0.658)", N48_DISP_ABI, 2); expect_u("M: ... ABI 1 is still the minimum for the monitor B", N48_DISP_ABI_MIN, 1);
        expect_u("M: refresh = (241500000 << 16) / (2720 * 1481)", N48_DISP_REFRESH1616, (uint32_t)((241500000ull << 16) / (2720ull * 1481ull))); expect_u("M: 14,745,600 = 225 x 64 KiB", N48_DISP_BYTES, 225ull * 65536ull);
        { N48DispOps o; std::memset(&o, 0, sizeof o); o.magic = N48_DISP_OPS_MAGIC; o.abi = 1; o.size = 72; o.snapshot = [](void *, N48DispSnap *) { return 0; }; o.power = [](void *, uint32_t) { return 0; };
          expect_u("M: ops check: good", n48disp_ops_check(&o), N48_DOV_OK); expect_u("M: ops check: NULL", n48disp_ops_check(nullptr), N48_DOV_NULL); { N48DispOps x = o; x.magic = 0; expect_u("M: ops check: magic", n48disp_ops_check(&x), N48_DOV_MAGIC); }
          { N48DispOps x = o; x.abi = 0; expect_u("M: ops check: ABI", n48disp_ops_check(&x), N48_DOV_ABI); } { N48DispOps x = o; x.size = 48; expect_u("M: ops check: size", n48disp_ops_check(&x), N48_DOV_SIZE); }
          { N48DispOps x = o; x.snapshot = nullptr; expect_u("M: ops check: no snapshot hook", n48disp_ops_check(&x), N48_DOV_MISSING); } }
        { const std::string auxroot = argc > 2 ? argv[2] : root; const std::string aux = slurp(auxroot + "/tools/native/navi48accel/src/Navi48DisplayOps.h");
          if (aux.empty()) std::printf("NOTE: the aux kext copy of Navi48DisplayOps.h is not present under %s\n", auxroot.c_str()); else expect(aux == dop, "M: Navi48DisplayOps.h is byte-identical in the bring-up kext and the aux kext"); }
        // ---- the interlocks and the admission of action 105 (pure)
        expect(fb_interlock_refuses(kActAdopt, 0ull, true) && fb_interlock_refuses(kActArm, 1ull, true), "M: interlock: pipeadopt and pipearm 1 are refused while the display nub exists");
        expect(!fb_interlock_refuses(kActArm, 0ull, true), "M: interlock: pipearm 0 (disarm) is ALWAYS allowed");
        expect(!fb_interlock_refuses(kActAdopt, 0ull, false) && !fb_interlock_refuses(kActArm, 1ull, false) && !fb_interlock_refuses(kActArm, 0ull, false), "M: interlock: without the nub nothing is refused");
        { bool other = false; for (uint32_t a = 0; a < 200; a++) if (a != kActAdopt && a != kActArm && fb_interlock_refuses(a, 1ull, true)) other = true; expect(!other, "M: interlock: only actions 83 and 84 are touched"); }
        expect_u("M: kFbNub is status 18 and named", kFbNub, 18); expect(std::string(status_name(kFbNub)).find("Navi48DisplayNub") != std::string::npos, "M: kFbNub says why");
        expect(kActFbPublish == 105u && action_admitted(true, 105u) && !action_admitted(false, 105u) && action_admitted(true, 106u) && !action_admitted(false, 106u) && action_admitted(true, 107u) && !action_admitted(false, 107u) && kActM6XStat == 107u && !action_admitted(true, 109u) && kActM6Stat == 106u && !action_admitted(true, 100u) && !action_admitted(true, 104u) && action_admitted(true, 99u) && !action_admitted(true, 103u), "M: action 105 (and, 0.0.659, 106 m6stat) is admitted only with the display latch, nothing else past 99 changed");
        expect(is_new_action(105u) && !is_pipe_verb(105u), "M: 105 is a new action but not a pipe verb (n48disp_verb never answers it)");
        expect(native_exempt(true, 105u, 2ull) && native_exempt(true, 105u, 1ull) && !native_exempt(true, 105u, 0ull) && !native_exempt(true, 105u, 3ull) && !native_exempt(false, 105u, 2ull) && !native_exempt(false, 105u, 1ull), "M: on a native boot fbpublish is exempt only as `fbpublish 2` (the monitor B) or `fbpublish 1` (the monitor A) with the latch");
        // ---- the Metal-nub publish refuses while a display nub exists
        { n48metal::GateIn g; std::memset(&g, 0, sizeof g); g.hello = true; g.bootarg = true; g.s1bGateOn = true; g.s1bRan = true; g.s1bPositive = true;
          expect_u("M: the Metal nub publishes without a display nub", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kOk);
          g.dispNub = true; expect_u("M: the Metal nub publish is REFUSED while a display nub exists", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kExclusive);
          g.hello = false; expect_u("M: ... the session check still comes first", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kNotReady); g.hello = true; g.bootarg = false; expect_u("M: ... and the boot-arg check", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kUnsupported);
          g.bootarg = true; g.dispNub = false; expect_u("M: withdraw is unaffected by the display nub", n48metal::withdraw_verdict(g, n48metal::kPublished), n48metal::kOk); g.dispNub = true; expect_u("M: ... even with the display nub", n48metal::withdraw_verdict(g, n48metal::kPublished), n48metal::kOk); }
        // ---- source pins: the order inside the nub's verb
        { const size_t v0 = dnub.find("uint32_t Navi48DisplayNub::publishVerb("), v1 = dnub.find("\nIOService *Navi48DisplayNub::published()", v0); const std::string v = (v0 == std::string::npos || v1 == std::string::npos) ? "" : dnub.substr(v0, v1 - v0);
          const size_t lk = v.find("IOLockLock(gLock);"), view = v.find("n48dcn::fbView(&v, inst);"), pre = v.find("n48fb::pre_verdict(pre)"), brk = v.find("if (st != n48fb::kOk) break;"), live = v.find("n48dcn::fbLive(inst, &holdBad, gates)"), lv = v.find("n48fb::live_verdict(holdBad)"),
                       edid = v.find("n48dcn::fbEdid(geom.ddcLine, edid)"), prop = v.find("rdna4fb_edid_prop(n48fb::edid_prop_name(geom.ddcLine), prop)"), dv = v.find("n48fb::data_verdict(d)"), sb = v.find("n48fb::snap_build_idx(&snap, index,"), sv = v.find("n48disp_snap_valid(&snap)"), svchk = v.find("if (sv != N48_DSV_OK) {"), alloc = v.find("OSTypeAlloc(Navi48DisplayNub)"),
                       at = v.find("nub->attach(pci)"), ex = v.find("__atomic_store_n(&gExists[slot], 1u, __ATOMIC_RELEASE);"), hv = v.find("__atomic_store_n(&gHaveSnap[slot], 1u, __ATOMIC_RELEASE);"), rs = v.find("nub->registerService();"), ul = v.find("IOLockUnlock(gLock);");
          expect(!v.empty() && lk != std::string::npos && view != std::string::npos && pre != std::string::npos && brk != std::string::npos && live != std::string::npos && lv != std::string::npos && edid != std::string::npos && prop != std::string::npos && dv != std::string::npos &&
                 sb != std::string::npos && sv != std::string::npos && svchk != std::string::npos && alloc != std::string::npos && at != std::string::npos && ex != std::string::npos && hv != std::string::npos && rs != std::string::npos && ul != std::string::npos, "M: the verb has all its steps");
          expect(lk < view && view < pre && pre < brk && brk < live && live < lv && lv < edid && edid < prop && prop < dv && dv < sb && sb < sv && sv < svchk && svchk < alloc && alloc < hv && hv < at && at < ex, "M: ORDER in fbpublish: lock < software facts < pre-verdict < refuse < LIVE GATES < live verdict < DDC < registry < data verdict < snapshot < validation < refuse on a bad snapshot < alloc < snapshot stored < attach < exists");
          expect(v.find("if (st != n48fb::kOk) break;", pre) < live && v.find("if (st != n48fb::kOk) break;", lv) < edid, "M: REACHABILITY: a software refusal ends the verb before ANY hardware read; a failed live gate before the DDC engine is touched");
          expect(at < rs && rs > ul && count_of(v, "nub->registerService();") == 1, "M: the nub is registered once, after the unlock (matching runs the aux kext's start, which calls back into the ops)");
          expect(v.find("n48dcn::fbEdid(geom.ddcLine, edid)") > v.find("n48dcn::fbLive(inst, &holdBad, gates)"), "M: the DDC engine is read after the gates");
          expect(v.find("WREG32") == std::string::npos && dnub.find("WREG32") == std::string::npos && dnub.find("RREG32") == std::string::npos && dnub.find("amdgpu::W") == std::string::npos, "M: the nub file writes no register itself"); }
        expect(count_of(dnub, "OSTypeAlloc(Navi48DisplayNub)") == 1 && count_of(dnub, "->attach(") == 1 && dnub.find("selectorPublish") == std::string::npos && dnub.find("selectorWithdraw") == std::string::npos && dnubh.find("static IOReturn selector") == std::string::npos && dnubh.find("withdraw(") == std::string::npos && dnubh.find("Withdraw(") == std::string::npos && dnub.find("::withdraw") == std::string::npos && dnub.find("->terminate()") == dnub.rfind("->terminate()"),
               "M: ONE creation site (the verb), no selector, NO withdraw in M5");
        expect(dnub.find("nub->attach(pci)") != std::string::npos && dnub.find("IOService *pci = navi48_bringup_pci();") != std::string::npos && dnub.find("setProperty(N48_DISP_INDEX_KEY, idx)") != std::string::npos && dnub.find("OSNumber::withNumber((uint64_t)index, 32)") != std::string::npos,
               "M: the nub attaches to the GPU's IOPCIDevice (NOT to Navi48Bringup) with Navi48DisplayIndex = the instance's index (0.0.658: 1 = the monitor B, 2 = the monitor A)");
        expect(dnub.find("N48_METAL_OPS_MAGIC") == std::string::npos && dnub.find("N48_DISP_OPS_MAGIC, N48_DISP_ABI, (uint32_t)sizeof(N48DispOps), 664u, 0u, 0u,\n\top_snapshot, op_power, op_trace,\n\tnullptr, nullptr, nullptr,\n};") != std::string::npos, "M: the ops table: magic, ABI 1, its size, build 660, the three hooks, the M6 members NULL");
        expect(dnub.find("functionName->isEqualTo(N48_DISP_FN_SYMBOL)") != std::string::npos && dnub.find("*(const N48DispOps **)param1 = &gDispOps;") != std::string::npos, "M: callPlatformFunction(\"n48.disp.ops\") answers the table");
        expect(dnub.find("n48fb::snapshot_hook_verdict(slot >= 0, slot >= 0 && __atomic_load_n(&gHaveSnap[slot], __ATOMIC_ACQUIRE) != 0u, out != nullptr) == 0") != std::string::npos && dnub.find("*out = gSnap[slot];") != std::string::npos && dnub.find("if (nub != nullptr && nub == (void *)gNub[i]) slot = (int)i;") != std::string::npos, "M: the snapshot hook answers only for A nub of ours, with ITS OWN index's snapshot, once published");
        { const std::string mnub = slurp(K + "src/Navi48MetalNub.cpp"), dh2 = slurp(K + "src/amd/native_disp.h");
          expect(dnub.find("pre.metalNub = n48metal_nub_published();") != std::string::npos && dh2.find("bool     n48metal_nub_published(void);") != std::string::npos && fbp.find("if (p.metalNub) return kMetalNub;") != std::string::npos, "M (0.0.653): fbpublish asks whether the Metal nub was ever published this boot, and refuses");
          expect(mnub.find("bool n48metal_nub_published(void) { return __atomic_load_n(&gEverPublished, __ATOMIC_ACQUIRE) != 0u; }") != std::string::npos && count_of(mnub, "__atomic_store_n(&gEverPublished, 1u, __ATOMIC_RELEASE);") == 1u && mnub.find("gEverPublished = 0") != std::string::npos && count_of(mnub, "gEverPublished = 0") == 1u, "M (0.0.653): the Metal-nub flag is sticky: set once at a successful publish, never cleared");
          expect(cli.find("st != N48_FBP_WS_OPEN && st != N48_FBP_METAL_NUB) {") != std::string::npos && dop.find("#define N48_FBP_METAL_NUB       16u") != std::string::npos && dop.find("case N48_FBP_METAL_NUB:") != std::string::npos, "M (0.0.653): the CLI skips the hardware printout for the Metal-nub refusal, and the shared header names it"); }
        expect(dnub.find("pre.latched = fb2_latched_on();") != std::string::npos && dnub.find("PE_parse_boot_argn(\"navi48-fb2\"") != std::string::npos && dnub.find("pre.pipeAdopted = n48disp_pipe_adopted();") != std::string::npos && dnub.find("pre.wsOpen = amdgpu::n1c_ws_is_open();") != std::string::npos &&
               dnub.find("pre.held = v.held; pre.pinned = v.pinned; pre.devOk = v.devOk;") != std::string::npos && dnub.find("pre.nubExists = n48fb_nub_exists_idx(index);") != std::string::npos, "M: the pre-verdict is fed by the latch, the existing nub, the held / pinned plane, the device, the adopted pipe and the WindowServer session");
        expect(dnub.find("d.block0EqProp = d.propPresent && memcmp(edid, prop, 128u) == 0;") != std::string::npos && dnub.find("OSDynamicCast(OSData, svc->getProperty(name))") != std::string::npos && dnub.find("d->getLength() == 128u") != std::string::npos &&
               dnub.find("d.block0Sum = n48disp_edid_sum_ok(edid); d.block0Hdr = n48disp_edid_header_ok(edid); d.block1Sum = n48disp_edid_sum_ok(edid + 128);") != std::string::npos, "M: EDID blocks 0 / 1 are checksum-checked and block 0 is compared byte for byte with RDNA4FB's EDID,DDC3 (128 bytes)");
        expect(dnub.find("d.apertureOk = n48fb::aperture_ok_len(v.bar0Phys, v.bar0Size, v.offA, geom.aperLen) && n48fb::len_in_alloc(geom.aperLen, v.bufBytes);") != std::string::npos && dnub.find("n48fb::snap_build_idx(&snap, index, v.bar0Phys, v.offA, v.mc[0], v.mc[1], edid);") != std::string::npos, "M: the aperture is bar0Phys + off[0], judged against BAR0, from the plane's own allocation");
        expect(mk.find("src/Navi48DisplayNub.cpp") != std::string::npos, "M: the Makefile builds Navi48DisplayNub.cpp");
        // ---- the glue: interlocks, the adopted accessor
        { const size_t a = glue.find("uint32_t n48disp_verb(uint32_t action, uint64_t arg, uint64_t *out, unsigned count) {"), e = glue.find("\n}\n", a); const std::string f = a == std::string::npos ? "" : glue.substr(a, e - a);
          const size_t va = f.find("if (!n48disp::verb_args_ok(action, arg))"), il = f.find("n48disp::fb_interlock_refuses(action, arg, n48fb_nub_exists(), n48m6_latched_on())"), env = f.find("KernelEnv env;"), adopt = f.find("n48disp::adopt_flow(env)"), arm = f.find("n48disp::arm_flow(env, arg)");
          expect(va != std::string::npos && il != std::string::npos && env != std::string::npos && adopt != std::string::npos && arm != std::string::npos && va < il && il < env && env < adopt && env < arm, "M: REACHABILITY: the interlock sits after the argument check and BEFORE any flow (pipeadopt / pipearm 1 never reach a flow while the nub exists)");
          expect(f.find("out[0] = n48disp::kFbNub;") != std::string::npos && f.find("return n48disp::kFbNub;") != std::string::npos, "M: the interlock answers kFbNub"); }
        expect(glue.find("bool n48disp_pipe_adopted(void) { return __atomic_load_n(&gD.adopted, __ATOMIC_ACQUIRE) != 0u; }") != std::string::npos && ghdr.find("bool     n48disp_pipe_adopted(void);") != std::string::npos && ghdr.find("bool     n48fb_nub_exists(void);") != std::string::npos, "M: the adopted-pipe accessor and the nub accessor are exported");
        expect(nub.find("g.dispNub = n48fb_nub_exists();") != std::string::npos && nub.find("n48metal::publish_verdict(g, (n48metal::State)gState)") != std::string::npos && mpure.find("if (g.dispNub && !g.m6) return kExclusive;") != std::string::npos, "M: the Metal nub's publish reads the display nub's existence into its verdict");
        { const size_t h = mpure.find("if (g.hung) return kNotReady;"), d = mpure.find("if (g.dispNub && !g.m6) return kExclusive;"), st = mpure.find("if (st == kPublished) return kExclusive;"); expect(h != std::string::npos && d != std::string::npos && st != std::string::npos && h < d && d < st, "M: the display-nub test sits after the HUNG test and before the state test"); }
        // ---- the dispatcher, the shutdown and the exemption
        { const size_t a = brg.find("if (action == n48disp::kActFbPublish) {"), e = brg.find("\n\t}\n", a); const std::string f = a == std::string::npos ? "" : brg.substr(a, e - a);
          expect(!f.empty() && f.find("Navi48DisplayNub::publishVerb(this, argScalar, v, 13)") != std::string::npos && f.find("outExtra[i] = v[i]") != std::string::npos && f.find("return kIOReturnSuccess;") != std::string::npos, "M: accelExperiment dispatches action 105 to the nub's verb and returns its scalars");
          const size_t refuse = brg.find("if (action > n48disp::kLastOldAction && !n48disp::action_admitted(n48disp_latched_on(), action)) return kIOReturnBadArgument;"), exempt = brg.find("!n48disp::native_exempt(n48disp_latched_on(), action, argScalar)"), d98 = brg.find("if (action == N48D2_ACT) {");
          expect(exempt != std::string::npos && refuse != std::string::npos && d98 != std::string::npos && a != std::string::npos && exempt < refuse && refuse < a && d98 < a, "M: REACHABILITY: action 105 passes the native-boot exemption and the admission bound before its branch"); }
        expect(brg.find("Navi48DisplayNub::shutdown();") != std::string::npos && brg.find("#include \"Navi48DisplayNub.hpp\"") != std::string::npos, "M: the kext's stop terminates the display nub; the header is included");
        expect(ucl.find("action > 82 && !n48disp::action_admitted(n48disp_latched_on(), action)") != std::string::npos, "M: the user client's bound goes through the same admission (105 needs the display latch)");
        // ---- the CLI
        expect(cli.find("else if (what && !strcmp(what, \"fbpublish\")) in = 105;") != std::string::npos && cli.find("in == 105 ? \"fbpublish\"") != std::string::npos && cli.find("hangstat sessstat scdcread appallow fbpublish m6stat m6xstat vramstat\\n") != std::string::npos && cli.find("|fbpublish 2|1|m6stat [0|1|2|3]|m6xstat [0|1|2]|vramstat|capstream") != std::string::npos &&
               cli.find("if (in == 105) return cmd_fbpublish(arg);") != std::string::npos && cli.find("dr_call(105, inst, o)") != std::string::npos && cli.find("n48disp_fbp_name(st)") != std::string::npos, "M: navi48test has fbpublish (action 105): the table line, the name, the known list, the usage, the command");
        expect(cli.find("!strcmp(a1, \"fbhold\") ? N48D2_OP_FBHOLD") != std::string::npos && cli.find("accel disp2 plane|show|flipA|flipB|crc|planeoff|fbhold 1|2") != std::string::npos && cli.find("p5    (0.0.652, M5;") != std::string::npos && cli.find("accel disp2 fbhold 2    HOLDS the plane") != std::string::npos, "M: navi48test has `disp2 fbhold 2` (table, usage, the printed run script)");
        expect(cli.find("N48D2_STAT_HELD_BIT") != std::string::npos && cli.find("N48D2_RV_HELD") != std::string::npos && cli.find("N48D2_PL_HELD ? \"HELD for this boot") != std::string::npos, "M: the CLI reports the HELD plane in `disp2 status`, planerec and the gates");
        expect(cli.find("if (watchBad && op == N48D2_OP_FBHOLD)") != std::string::npos && cli.find("if (watchBad && op == N48D2_OP_FBHOLD)") < cli.find("if (watchBad && op != N48D2_OP_PLANEOFF) {"), "M: a DP disturbance around fbhold never runs the (refused) planeoff");
        { const size_t c0 = cli.find("static int cmd_fbpublish("), c1 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {", c0); const std::string f = c0 == std::string::npos ? "" : cli.substr(c0, c1 - c0);
          expect(!f.empty() && f.find("strcmp(a1, \"2\")") != std::string::npos && f.find("NO withdraw") != std::string::npos, "M: `accel fbpublish` needs the argument 2 and says there is no withdraw"); }
        // ---- the variant
        { const std::string var = slurp(root + "/variants/configs/stage17-native-1440-metal-disp-amfi-fb2.plist"), base = slurp(root + "/variants/configs/stage17-native-1440-metal-disp-amfi-disp2.plist");
          expect(!var.empty() && !base.empty(), "M: the fb2 variant and its disp2 base are readable");
          expect(var.find("-v keepsyms=1 npci=0x2000 alcid=7 msgbuf=1048576 navi48bringup=1 navi48-stage=17 navi48-smu-full=1 navi48-native=1 navi48-metal=1 navi48-metal-disp=1 amfi_get_out_of_my_way=1 navi48-dmubcmd=1 navi48-disp2=1 navi48-fb2=1</string>") != std::string::npos, "M: the variant's boot-args: the stage17 base, native, metal, metal-disp, dmubcmd, disp2, fb2");
          expect(var.find("navi48-metal-ws") == std::string::npos, "M: the variant has NO navi48-metal-ws");
          { std::string a = base, b = var; const std::string o1 = "navi48-metal-disp=1 navi48-metal-ws=1 amfi_get_out_of_my_way=1 navi48-dmubcmd=1 navi48-disp2=1</string>", n1 = "navi48-metal-disp=1 amfi_get_out_of_my_way=1 navi48-dmubcmd=1 navi48-disp2=1 navi48-fb2=1</string>";
            const size_t p = a.find(o1); if (p != std::string::npos) a.replace(p, o1.size(), n1); expect(p != std::string::npos && a == b, "M: the variant is the disp2 variant with exactly metal-ws removed and fb2 added (nothing else differs)"); } }
        // ---- the banned spellings in every new file
        { const std::string p1 = std::string("pipe+") + "0x280", p2 = std::string("pipe+") + "0x282", p3 = std::string("pipe+") + "0x299", q2 = std::string("+") + "0x282", q3 = std::string("+") + "0x299";
          for (const std::string *f : { &fbp, &dop, &dnub, &dnubh }) expect(f->find(p1) == std::string::npos && f->find(p2) == std::string::npos && f->find(p3) == std::string::npos && f->find(q2) == std::string::npos && f->find(q3) == std::string::npos, "M: no banned pipe-offset spelling in a new file"); }
    }

    // =========================================================================================================================================================
    // M2. 0.0.658 (ABI 2): the MONA as display index 2 (an internal design note "monitor A (instance 1) plane + framebuffer spec at 1080p60", corrections 5 and 6, section 4)
    // =========================================================================================================================================================
    {
        const std::string fbp = slurp(K + "src/amd/native_fb_pure.h"), dop = slurp(K + "src/Navi48DisplayOps.h"), dnub = slurp(K + "src/Navi48DisplayNub.cpp"), dcnc = slurp(K + "src/dcn/navi48_dcn.cpp"), dcnh = slurp(K + "src/dcn/navi48_dcn.hpp");
        // ---- the monitor B's publish path is byte for byte the 0.0.657 one (the golden is the hash of the SAME battery compiled against `git show f38ba015:` native_fb_pure.h + Navi48DisplayOps.h)
        { const uint64_t h = fbbat::monb_publish_hash(); std::printf("monitor B fbpublish battery hash %016llx (golden from the 0.0.657 sources: fb2cf4fd54ed61ed)\n", (unsigned long long)h);
          expect(h == 0xfb2cf4fd54ed61edull, "M2: the monitor B's fbpublish decisions, snapshot bytes, validator and ops check are BYTE-FOR-BYTE the 0.0.657 ones (the battery hashes to the 0.0.657 golden)"); }
        // ---- THE ONE TABLE (correction 6): instance -> display index / OTG / DDC line, pinned for both rows
        { N48DispGeom g1, g2, gx; std::memset(&g1, 0, sizeof g1); std::memset(&g2, 0, sizeof g2); std::memset(&gx, 0xAB, sizeof gx);
          expect(n48disp_geom_for_index(1u, &g1) == 1 && n48disp_geom_for_index(2u, &g2) == 1 && n48disp_geom_for_index(0u, &gx) == 0 && n48disp_geom_for_index(3u, &gx) == 0 && n48disp_geom_for_index(1u, nullptr) == 0, "M2: the table knows display indexes 1 and 2 and nothing else");
          expect(g1.index == 1u && g1.inst == 2u && g1.otg == 2u && g1.ddcLine == 3u && g1.w == 2560u && g1.h == 1440u && g1.pitchBytes == 10240u && g1.refresh1616 == (uint32_t)((241500000ull << 16) / (2720ull * 1481ull)) && g1.pixHz == 241500000ull && g1.aperLen == 14745600ull,
                 "M2: row {instance 2 (the monitor B) -> display index 1, OTG2, DDC line 3, 2560x1440, pitch 10240, 241.5 MHz, 14,745,600 bytes}");
          expect(g2.index == 2u && g2.inst == 1u && g2.otg == 1u && g2.ddcLine == 2u && g2.w == 1920u && g2.h == 1080u && g2.pitchBytes == 7680u && g2.refresh1616 == 0x3C0000u && g2.pixHz == 148500000ull && g2.aperLen == 8294400ull && g2.fmt == N48_DISP_FMT_ARGB8888,
                 "M2: row {instance 1 (the monitor A) -> display index 2, OTG1, DDC line 2, 1920x1080, pitch 7680, 148.5 MHz, refresh 0x3C0000 (exactly 60 Hz), 8,294,400 bytes}");
          expect(g2.refresh1616 == (uint32_t)((148500000ull << 16) / (2200ull * 1125ull)) && N48_DISPA_HTOTAL * N48_DISPA_VTOTAL * 60ull == 148500000ull && g2.aperLen == 1920ull * 1080ull * 4ull && g2.aperLen == 2025ull * 4096ull && g2.aperLen <= 127ull * 65536ull && g2.aperLen > 126ull * 65536ull,
                 "M2: the monitor A's refresh is exactly 60, the aperture is the exact frame (2025 pages) inside the 127 x 64 KiB allocation (8,323,072 bytes)");
          expect(n48disp_index_for_inst(2u) == 1u && n48disp_index_for_inst(1u) == 2u && n48disp_index_for_inst(0u) == 0u && n48disp_index_for_inst(3u) == 0u && n48disp_index_for_inst(0x102u) == 0u, "M2: instance 2 -> index 1, instance 1 -> index 2, nothing else maps");
          bool coherent = true;
          for (uint64_t a = 0; a < 0x400; a++) {
              const uint32_t idx = n48fb::index_of_arg(a), inst = n48fb::inst_of_arg(a); N48DispGeom g;
              if (n48fb::arg_ok(a)) { if (idx != n48disp_index_for_inst(inst) || !n48disp_geom_for_index(idx, &g) || g.inst != inst || g.inst != (uint32_t)a) coherent = false; }
              else if (idx != 0u || inst != 0u) coherent = false;
          }
          expect(coherent, "M2: fbpublish's argument is the instance, mapped to its index through the ONE table; a refused argument maps to nothing");
          expect(n48fb::index_of_arg(1ull) == 2u && n48fb::index_of_arg(2ull) == 1u && n48fb::inst_of_arg(1ull) == 1u && n48fb::inst_of_arg(2ull) == 2u, "M2: `fbpublish 1` -> display index 2 (the monitor A), `fbpublish 2` -> display index 1 (the monitor B)");
          expect(std::string(n48fb::edid_prop_name(3u)) == "EDID,DDC3" && std::string(n48fb::edid_prop_name(2u)) == "EDID,DDC2" && n48fb::edid_prop_name(1u) == nullptr && n48fb::edid_prop_name(4u) == nullptr && n48fb::edid_prop_name(g1.ddcLine) != n48fb::edid_prop_name(g2.ddcLine), "M2: the monitor B's EDID is compared with EDID,DDC3 and the monitor A's with EDID,DDC2"); }
        // ---- snapshots of both indexes, built from the table and judged by the shared validator
        { uint8_t e[256]; std::memset(e, 0, sizeof e); e[1] = e[2] = e[3] = e[4] = e[5] = e[6] = 0xFF; { uint32_t sm = 0; for (int i = 0; i < 127; i++) sm += e[i]; e[127] = (uint8_t)(0u - sm); } e[128] = 2; { uint32_t sm = 0; for (int i = 128; i < 255; i++) sm += e[i]; e[255] = (uint8_t)(0u - sm); }
          N48DispSnap a, d; n48fb::snap_build_idx(&a, 2u, 0x80000000ull, 0x02000000ull, 0x8002000000ull, 0x80027F0000ull, e); n48fb::snap_build_idx(&d, 1u, 0x80000000ull, 0x01800000ull, 0x8001800000ull, 0x8002600000ull, e);
          expect_u("M2: the monitor A's snapshot is valid", n48disp_snap_valid(&a), N48_DSV_OK);
          expect(a.index == 2u && a.otg == 1u && a.ddcLine == 2u && a.w == 1920u && a.h == 1080u && a.pitchBytes == 7680u && a.refresh1616 == 0x3C0000u && a.pixHz == 148500000ull && a.aperLen == 8294400ull && a.aperPhys == 0x82000000ull && a.edidLen == 256u && std::memcmp(a.edid, e, 256) == 0, "M2: snap_build_idx(2): the monitor A's mode, aperture = bar0Phys + off[0], the EDID");
          { N48DispSnap d0; n48fb::snap_build(&d0, 0x80000000ull, 0x01800000ull, 0x8001800000ull, 0x8002600000ull, e); expect(std::memcmp(&d0, &d, sizeof d) == 0, "M2: snap_build (the 0.0.657 signature) == snap_build_idx(1) byte for byte"); }
          // "anything else refused": every geometry field of each index wrong, and the two geometries swapped
          { N48DispSnap x = a; x.index = 1u; expect_u("M2: the monitor A's geometry under index 1 is refused", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = d; x.index = 2u; expect_u("M2: the monitor B's geometry under index 2 is refused", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.index = 0u; expect_u("M2: index 0", n48disp_snap_valid(&x), N48_DSV_INDEX); } { N48DispSnap x = a; x.index = 3u; expect_u("M2: index 3", n48disp_snap_valid(&x), N48_DSV_INDEX); }
          { N48DispSnap x = a; x.otg = 2u; expect_u("M2: monitor A otg", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); } { N48DispSnap x = a; x.ddcLine = 3u; expect_u("M2: monitor A ddc line", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.w = 2560u; expect_u("M2: monitor A width", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); } { N48DispSnap x = a; x.h = 1440u; expect_u("M2: monitor A height", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.pitchBytes = 10240u; expect_u("M2: monitor A pitch", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); } { N48DispSnap x = a; x.fmt = 0u; expect_u("M2: monitor A format", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.refresh1616 = (uint32_t)((148500000ull << 16) / (2200ull * 1126ull)); expect_u("M2: monitor A refresh not exactly 60", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.pixHz = 241500000ull; expect_u("M2: monitor A pixel clock", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.aperLen = 127ull * 65536ull; expect_u("M2: monitor A aperture length = the allocation instead of the frame", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.aperLen = 14745600ull; expect_u("M2: monitor A aperture length = the monitor B's", n48disp_snap_valid(&x), N48_DSV_GEOMETRY); }
          { N48DispSnap x = a; x.aperPhys += 0x1000; expect_u("M2: monitor A aperture unaligned", n48disp_snap_valid(&x), N48_DSV_APERTURE); } { N48DispSnap x = a; x.mcB = x.mcA; expect_u("M2: monitor A mcA == mcB", n48disp_snap_valid(&x), N48_DSV_MC); }
          { N48DispSnap x = a; x.edid[200] ^= 1; expect_u("M2: monitor A EDID block 1 checksum", n48disp_snap_valid(&x), N48_DSV_EDID_SUM); }
          n48fb::snap_build_idx(&a, 3u, 0x80000000ull, 0x02000000ull, 0x8002000000ull, 0x80027F0000ull, e); expect_u("M2: snap_build_idx of an unknown index is the all-zero snapshot, refused", n48disp_snap_valid(&a), N48_DSV_INDEX); }
        // ---- the ops check FOR AN INDEX: the monitor A needs ABI 2, an ABI-1 table is refused cleanly
        { N48DispOps o; std::memset(&o, 0, sizeof o); o.magic = N48_DISP_OPS_MAGIC; o.size = 72; o.snapshot = [](void *, N48DispSnap *) { return 0; }; o.power = [](void *, uint32_t) { return 0; };
          o.abi = 1; expect_u("M2: ABI-1 table, index 1 (the monitor B): accepted", n48disp_ops_check_for(&o, 1u), N48_DOV_OK); expect_u("M2: ABI-1 table, index 2 (the monitor A): REFUSED (N48_DOV_ABI)", n48disp_ops_check_for(&o, 2u), N48_DOV_ABI);
          o.abi = 2; expect_u("M2: ABI-2 table, index 1", n48disp_ops_check_for(&o, 1u), N48_DOV_OK); expect_u("M2: ABI-2 table, index 2", n48disp_ops_check_for(&o, 2u), N48_DOV_OK);
          expect_u("M2: an unknown index is refused", n48disp_ops_check_for(&o, 0u), N48_DOV_ABI); expect_u("M2: ... index 3", n48disp_ops_check_for(&o, 3u), N48_DOV_ABI);
          o.abi = 3; expect_u("M2: a NEWER table serves both", n48disp_ops_check_for(&o, 2u), N48_DOV_OK);
          { N48DispOps x = o; x.abi = 2; x.magic = 0; expect_u("M2: the base check still comes first (magic)", n48disp_ops_check_for(&x, 2u), N48_DOV_MAGIC); } expect_u("M2: NULL", n48disp_ops_check_for(nullptr, 2u), N48_DOV_NULL); }
        expect(n48fb::len_in_alloc(8294400ull, 127ull * 65536ull) && n48fb::len_in_alloc(14745600ull, 225ull * 65536ull) && !n48fb::len_in_alloc(14745600ull, 127ull * 65536ull) && !n48fb::len_in_alloc(8294400ull, 8294400ull) && !n48fb::len_in_alloc(0ull, 65536ull) && !n48fb::len_in_alloc(65536ull, 0ull),
               "M2: the frame must lie inside the plane's own allocation (a whole number of 64 KiB blocks)");
        expect(n48fb::aperture_ok_len(0x80000000ull, 0x10000000ull, 0x02000000ull, N48_DISPA_BYTES) && !n48fb::aperture_ok_len(0x80000000ull, 0x10000000ull, 0x10000000ull - 4096u, N48_DISPA_BYTES) && n48fb::aperture_ok_len(0x80000000ull, 0x10000000ull, 0x10000000ull - N48_DISPA_BYTES - 0x8000ull + 0x8000ull - 0x0ull - (0x10000000ull - N48_DISPA_BYTES) % 0x10000ull, N48_DISPA_BYTES),
               "M2: the monitor A's aperture is judged against BAR0 with ITS length");
        // ---- the verb: per-index state, nothing shared between the two nubs
        { const size_t v0 = dnub.find("uint32_t Navi48DisplayNub::publishVerb("), v1 = dnub.find("\nIOService *Navi48DisplayNub::published()", v0); const std::string v = (v0 == std::string::npos || v1 == std::string::npos) ? "" : dnub.substr(v0, v1 - v0);
          expect(!v.empty() && v.find("const uint32_t inst = n48fb::inst_of_arg(arg), index = n48fb::index_of_arg(arg);") != std::string::npos && v.find("n48disp_geom_for_index(index, &geom)") != std::string::npos && v.find("const unsigned slot = index - 1u;") != std::string::npos, "M2: the verb maps the argument (instance) to its index and geometry through the ONE table and picks the nub slot from the index");
          expect(count_of(v, "gNub[slot] = nub;") == 1 && count_of(v, "gSnap[slot] = snap;") == 1 && count_of(v, "gExists[slot]") == 1 && count_of(v, "gHaveSnap[slot]") == 2 && v.find("gNub[0]") == std::string::npos && v.find("gNub[1]") == std::string::npos && v.find("gSnap[0]") == std::string::npos && v.find("gSnap[1]") == std::string::npos && v.find("gExists[0]") == std::string::npos && v.find("gExists[1]") == std::string::npos,
                 "M2: publishing one index writes ONLY that index's nub, snapshot and flags (never the other slot)");
          expect(v.find("pre.nubExists = n48fb_nub_exists_idx(index);") != std::string::npos && v.find("n48fb_nub_exists()") == std::string::npos && v.find("if (nub && !n48fb_nub_exists_idx(index)) nub->release();") != std::string::npos, "M2: the 'nub exists' refusal is per index (publishing the monitor B never blocks the monitor A and the reverse)");
          expect(v.find("n48dcn::fbLive(inst, &holdBad, gates)") != std::string::npos && v.find("n48dcn::fbEdid(geom.ddcLine, edid)") != std::string::npos && v.find("N48_DISP_DDC_LINE") == std::string::npos && v.find("N48D2_INST_MONB") == std::string::npos && v.find("N48_DISP_INDEX)") == std::string::npos && v.find("N48_DISP_INDEX,") == std::string::npos, "M2: no monitor B constant is left in the verb: the live gates, the DDC line and the EDID property all come from the instance's row"); }
        expect(dnub.find("bool n48fb_nub_exists(void) { return n48fb_nub_exists_idx(N48_DISP_INDEX) || n48fb_nub_exists_idx(N48_DISPA_INDEX); }") != std::string::npos && dnub.find("constexpr unsigned   kNubs = 2;") != std::string::npos, "M2: the interlocks (pipeadopt, pipearm 1, the Metal publish) see ANY display nub");
        expect(dnub.find("for (unsigned i = 0; i < kNubs; i++) if (gNub[i] == this) gNub[i] = nullptr;") != std::string::npos && count_of(dnub, "->terminate()") == 1 && dnub.find("for (unsigned i = 0; i < kNubs; i++) {\n\t\tIOLockLock(gLock);") != std::string::npos, "M2: free() and the kext's stop handle both nubs");
        { const size_t a = dcnc.find("void fbView(n48dcn::FbView *v, uint32_t inst) {"), b = dcnc.find("// The dispatcher asks this BEFORE bind()", a); const std::string f = (a == std::string::npos || b == std::string::npos) ? "" : dcnc.substr(a, b - a);
          const size_t fv1 = f.find("uint32_t fbLive("); const std::string fv = fv1 == std::string::npos ? "" : f.substr(0, fv1);
          expect(!f.empty() && !fv.empty() && fv.find("const uint32_t pi = d2i(inst);") != std::string::npos && f.find("gD2Pl[1]") == std::string::npos && f.find("gD2Off[1]") == std::string::npos && f.find("d2i(inst)") != std::string::npos && count_of(f, "gD2Pl[pi]") >= 4 && f.find("n48d2_surf_get(inst)->buf_bytes") != std::string::npos,
                 "M2: fbView / fbLive / fbEdid read the plane state of the INSTANCE asked for (no hard-wired monitor B slot)");
          expect(f.find("n48dr::ddc_read(env, ddcLine, block)") != std::string::npos && f.find("ddcLine != N48_DISP_DDC_LINE && ddcLine != N48_DISPA_DDC_LINE") != std::string::npos, "M2: fbEdid reads the line the table names (the monitor B's 3 or the monitor A's 2) and nothing else"); }
        expect(dcnh.find("void fbView(FbView *v, uint32_t inst);") != std::string::npos && dcnh.find("uint32_t fbLive(uint32_t inst, uint32_t *holdBad, uint32_t *gates);") != std::string::npos && dcnh.find("uint32_t fbEdid(uint32_t ddcLine, uint8_t out[256]);") != std::string::npos, "M2: the display-layer entry points take the instance / the DDC line");
        // ---- the CLI: fbpublish 1, fbhold 1, the Run B block
        { const size_t c0 = cli.find("static int cmd_fbpublish("), c1 = cli.find("static int cmd_disp2(const char *a1, const char *a2) {", c0); const std::string f = c0 == std::string::npos ? "" : cli.substr(c0, c1 - c0);
          expect(!f.empty() && f.find("(strcmp(a1, \"2\") && strcmp(a1, \"1\"))") != std::string::npos && f.find("const unsigned inst = (unsigned)(a1[0] - '0');") != std::string::npos && f.find("dr_call(105, inst, o)") != std::string::npos, "M2: `accel fbpublish 1|2` sends the instance"); }
        { const size_t d0 = cli.find("static void d2_script(void) {"), d1 = cli.find("static int d2_status_call(", d0); const std::string f = d0 == std::string::npos ? "" : cli.substr(d0, d1 - d0); const size_t rb = f.find("Run B (0.0.658;");
          expect(rb != std::string::npos, "M2: the printed script has a Run B block");
          const std::string r = rb == std::string::npos ? "" : f.substr(rb);
          size_t at = 0; bool ordered = true; for (const char *t : { "accel dmubsend pclk otg2-1440-on", "accel disp2 plane 2", "accel disp2 show 2", "accel dmubsend pclk otg1-on", "accel disp2 plane 1", "accel disp2 show 1", "accel disp2 fbhold 2", "accel disp2 fbhold 1", "accel fbpublish 2", "accel fbpublish 1", "killall -9 WindowServer" }) { const size_t p = r.find(t, at); if (p == std::string::npos) { ordered = false; break; } at = p; }
          expect(ordered, "M2: Run B order: monitor B chain, monitor B plane / show, monitor A chain, monitor A plane / show, fbhold 2, fbhold 1, fbpublish 2, fbpublish 1, then the (single) WindowServer restart");
          expect(count_of(r, "killall -9 WindowServer") == 1 && r.find("ONE `killall -9 WindowServer` at 0 users") != std::string::npos && r.find("stage17-native-1440-metal-disp-amfi-fb2.plist") != std::string::npos && r.find("NO navi48-metal-ws") != std::string::npos && r.find("Navi48Accel-0.0.6.kext") != std::string::npos && r.find("0 users") != std::string::npos, "M2: Run B names the -fb2 variant, no metal-ws, the aux 0.0.6 path and exactly one WindowServer restart at 0 users"); }
        expect(cli.find("(strcmp(a2, \"1\") && strcmp(a2, \"2\"))) { fprintf(stderr, \"accel disp2 %s: the plane ops take the instance") != std::string::npos && cli.find("(pop == N48D2_OP_FBHOLD && strcmp(a2, \"2\"))") == std::string::npos, "M2: `accel disp2 fbhold 1` is accepted by the CLI");
        expect(cli.find("Next: `accel fbpublish %u` (needs boot-arg navi48-fb2=1)") != std::string::npos, "M2: fbhold's follow-up names the instance's fbpublish");
        // ---- the aux copy of the ops header is byte-identical (checked above); both carry the table
        expect(dop.find("static inline uint32_t n48disp_ops_check_for(") != std::string::npos && dop.find("n48disp_geom_for_index(s->index, &g)") != std::string::npos && fbp.find("constexpr uint32_t index_of_arg(uint64_t a)") != std::string::npos, "M2: the shared header carries the per-index table, the index-aware ops check and the validator through the table");
    }

    // =========================================================================================================================================================
    // N. 0.0.659 (M6 Stage 1a): the routing guard, the per-pipe adopt, the interlock lifts, the per-endpoint AGDC answers, the per-OTG stamps. All behind the navi48-m6 latch.
    //    The flows run the REAL code of amd/native_disp_flow.h over the fake kernel (the surface table is the real n48m6:: table); the OFF path is the 0.0.658 one (the battery below hashes it).
    // =========================================================================================================================================================
    {
        using namespace n48m6;
        // ---- N1 pure: the latch, the instance tables, the table, the guard ------------------------------------------------------------------------------------------------------------------
        expect_u("N1 latch: absent -> off", n48m6::latch_value(false, 1), n48m6::kLatchOff); expect_u("N1 latch: =1 -> on", n48m6::latch_value(true, 1), n48m6::kLatchOn);
        expect_u("N1 latch: =0 -> off", n48m6::latch_value(true, 0), n48m6::kLatchOff); expect_u("N1 latch: =2 -> off (only 1 enables)", n48m6::latch_value(true, 2), n48m6::kLatchOff);
        expect(n48m6::latch_is_on(n48m6::kLatchOn) && !n48m6::latch_is_on(n48m6::kLatchOff) && !n48m6::latch_is_on(n48m6::kLatchUnset), "N1 latch_is_on: only the ON state");
        expect(inst_of_index(N48_DISP_INDEX) == kInstMonB && inst_of_index(N48_DISPA_INDEX) == kInstMonA && inst_of_index(0) == kInstNone && inst_of_index(3) == kInstNone && index_of_inst(kInstMonB) == N48_DISP_INDEX && index_of_inst(kInstMonA) == N48_DISPA_INDEX && index_of_inst(kInstDp) == 0u, "N1 the instance <-> display index tables (monitor B = instance 2 = index 1, monitor A = instance 1 = index 2)");
        expect(otg_of_inst(kInstDp) == 0u && otg_of_inst(kInstMonA) == 1u && otg_of_inst(kInstMonB) == 2u, "N1 the OTG of each instance (DP 0, monitor A 1, monitor B 2)");
        { FbMap m {}; m.fb[0] = 0x10; m.fb[1] = 0x20; m.fb[2] = 0x30;
          expect(inst_of_fb(m, 0x10) == 0u && inst_of_fb(m, 0x20) == 1u && inst_of_fb(m, 0x30) == 2u && inst_of_fb(m, 0x40) == kInstNone && inst_of_fb(m, 0) == kInstNone, "N1 inst_of_fb: a known pointer maps to its instance, an unknown one and NULL to none");
          FbMap z {}; expect(inst_of_fb(z, 0) == kInstNone, "N1 inst_of_fb: an empty map never matches NULL"); }
        { SurfTable t {};
          expect_u("N1 learn: a new ID", learn_surface(t, 101, kInstDp), kLNew); expect_u("N1 learn: the same ID, same instance", learn_surface(t, 101, kInstDp), kLSame);
          expect_u("N1 learn: ID 0 refused", learn_surface(t, 0, kInstDp), kLBadId); expect_u("N1 learn: an unknown instance refused", learn_surface(t, 5, kInstNone), kLBadInst); expect_u("N1 learn: instance 3 refused", learn_surface(t, 5, 3), kLBadInst);
          expect_u("N1 learn: the same ID on ANOTHER pipe becomes AMBIGUOUS", learn_surface(t, 101, kInstMonB), kLBecameAmbiguous); expect_u("N1 learn: and stays so", learn_surface(t, 101, kInstDp), kLWasAmbiguous);
          { Look l = lookup_surface(t, 101); expect(l.found && l.ambiguous, "N1 lookup: the ambiguous ID is found and flagged"); }
          expect(!lookup_surface(t, 999).found && !lookup_surface(t, 0).found, "N1 lookup: an unseen ID and ID 0 are not found");
          for (uint32_t id = 1000; id < 1000 + kMaxSurf; ++id) (void)learn_surface(t, id, kInstMonA);
          expect(!lookup_surface(t, 101).found && lookup_surface(t, 1000).found, "N1 learn: filling the table past 16 evicted the entry seen the longest ago (101), not the newest ones");
          expect_u("N1 learn: a full table EVICTS the entry not seen for the longest time (0.0.660: it never refuses)", learn_surface(t, 5000, kInstMonA), kLEvicted);
          expect(!lookup_surface(t, 1000).found && lookup_surface(t, 5000).found && lookup_surface(t, 5000).inst == kInstMonA && lookup_surface(t, 1001).found && lookup_surface(t, 1001).inst == kInstMonA, "N1 learn: the oldest (1000) made room; the others and the new ID are there");
          expect_u("N1 learn: a full table still answers an existing ID", learn_surface(t, 1001, kInstMonA), kLSame);
          SurfTable b {}; b.busy = 1; expect_u("N1 learn: a busy table declines (counted), nothing is written", learn_surface(b, 7, kInstDp), kLBusy); expect(b.n == 0, "N1 learn: nothing written while busy"); }
        { SurfTable t {}; bool allNew = true;      // EXACT capacity: kMaxSurf distinct IDs fit, the next one is refused and nothing past the array is written (the entry array ends where `n` begins)
          for (uint32_t i = 0; i < kMaxSurf; ++i) allNew = allNew && learn_surface(t, 7000 + i, kInstMonB) == kLNew;
          expect(allNew && t.n == kMaxSurf && t.busy == 0u, "N1 learn: exactly kMaxSurf IDs fit, one entry each");
          expect_u("N1 learn: ID kMaxSurf+1 evicts (kLEvicted)", learn_surface(t, 7999, kInstMonB), kLEvicted);
          expect(t.n == kMaxSurf && t.busy == 0u && t.learn[kLEvicted] == 1 && t.learn[kLFull] == 0 && lookup_surface(t, 7999).found && !lookup_surface(t, 7000).found && lookup_surface(t, 7001).found, "N1 learn: a full table stays at kMaxSurf (nothing written past the array, the writer lock is released), the entry seen longest ago is gone, kLFull is never returned"); }
        expect(kSurfId == 0x10u && kTxnPipe == 0x28u, "N1 the layout facts the learn reads: IOSurface+0x10 (CONFIRMED, getSurfaceID) and txn+0x28 (SUSPECTED until a Stage 1a log; the learn refuses a transaction whose +0x28 is not the hooked pipe)");
        { SurfTable t {}; (void)learn_surface(t, 5, kInstDp);
          const Look u = lookup_surface(t, 999);
          expect(!u.found && !u.ambiguous && u.inst == kInstNone, "N1 lookup: an unseen ID reports NO instance (kInstNone), never the DP's"); }
        { SurfTable t {}; (void)learn_surface(t, 11, kInstDp); (void)learn_surface(t, 12, kInstDp); (void)learn_surface(t, 13, kInstDp); (void)learn_surface(t, 21, kInstMonB); (void)learn_surface(t, 22, kInstMonB); (void)learn_surface(t, 23, kInstMonB); (void)learn_surface(t, 22, kInstDp);
          expect(count_for_inst(t, kInstDp) == 3 && count_for_inst(t, kInstMonB) == 3 && count_for_inst(t, kInstMonA) == 0 && count_ambiguous(t) == 1, "N1 per-instance counts: 3 IDs per pipe, one ambiguous (the first instance keeps the count)"); }
        { const Look none { false, false, kInstNone }, dp { true, false, kInstDp }, monb { true, false, kInstMonB }, amb { true, true, kInstMonB };
          expect_u("N1 guard: the DP's surface on the DP's pipe", route_verdict(dp, true, kInstDp), kRouteOk);
          expect_u("N1 guard: the monitor B's surface on the DP's pipe is REFUSED", route_verdict(monb, true, kInstDp), kRouteWrongInst);
          expect_u("N1 guard: the DP's surface on the monitor B's pipe is REFUSED", route_verdict(dp, true, kInstMonB), kRouteWrongInst);
          expect_u("N1 guard: an unseen surface", route_verdict(none, true, kInstDp), kRouteUnknown); expect_u("N1 guard: an ambiguous surface", route_verdict(amb, true, kInstMonB), kRouteAmbiguous);
          expect_u("N1 guard: no readable ID", route_verdict(dp, false, kInstDp), kRouteNoId); expect_u("N1 guard: a pipe on no known framebuffer", route_verdict(dp, true, kInstNone), kRouteBadPipe);
          expect(pipe_may_copy(kInstDp) && !pipe_may_copy(kInstMonA) && !pipe_may_copy(kInstMonB) && !pipe_may_copy(kInstNone), "N1 only the DP's pipe may copy into the DP console");
          for (uint32_t r = 0; r < kRouteCount; ++r) expect(std::strcmp(route_name(r), "unknown") != 0, "N1 every guard verdict is named"); }
        // the property blob the bundle reads
        { SurfTable t {}; (void)learn_surface(t, 0x11223344u, kInstMonB); (void)learn_surface(t, 0x55u, kInstDp); (void)learn_surface(t, 0x55u, kInstMonA);
          uint8_t blob[kBlobMax]; const uint32_t n = blob_build(t, blob, sizeof blob);
          auto r32 = [&](uint32_t o) { return (uint32_t)blob[o] | ((uint32_t)blob[o + 1] << 8) | ((uint32_t)blob[o + 2] << 16) | ((uint32_t)blob[o + 3] << 24); };
          expect(n == kBlobHdr + 2 * kBlobEnt && r32(0) == kBlobVersion && r32(4) == 2 && r32(8) == 0x11223344u && r32(12) == kInstMonB && r32(16) == 0u && r32(20) == 0x55u && r32(24) == kInstDp && r32(28) == kEntAmbiguous, "N1 the property blob: version, count, then {id, instance, flags} per entry");
          expect(blob_build(t, nullptr, 100) == 0 && blob_build(t, blob, 10) == 0, "N1 the blob builder refuses a short or NULL buffer"); }
        { // the latch is READ ONCE: the reader runs once, whatever happens to the boot-arg afterwards, and a loser of the race reads the stored word
          volatile uint32_t slot = n48m6::kLatchUnset; int reads = 0; uint32_t arg = 1;
          auto reader = [&]() -> uint32_t { reads++; return n48m6::latch_value(true, arg); };
          expect(n48m6::latch_is_on(n48m6::latch_get(&slot, reader)) && reads == 1, "N1 latch_get: the first call reads the boot-arg and latches ON");
          arg = 0; expect(n48m6::latch_is_on(n48m6::latch_get(&slot, reader)) && reads == 1 && slot == n48m6::kLatchOn, "N1 latch_get: a later call does NOT read the boot-arg again (the latched ON survives the change)");
          for (int i = 0; i < 5; ++i) (void)n48m6::latch_get(&slot, reader); expect(reads == 1, "N1 latch_get: five more calls, still one read");
          volatile uint32_t off = n48m6::kLatchUnset; int r2 = 0; uint32_t a2 = 0; auto rd2 = [&]() -> uint32_t { r2++; return n48m6::latch_value(true, a2); };
          expect(!n48m6::latch_is_on(n48m6::latch_get(&off, rd2)) && r2 == 1, "N1 latch_get: OFF is latched too (n48m6::kLatchOff is not 'unset')"); a2 = 1; expect(!n48m6::latch_is_on(n48m6::latch_get(&off, rd2)) && r2 == 1, "N1 latch_get: and OFF stays OFF for the boot");
          volatile uint32_t raced = n48m6::kLatchUnset; int r3 = 0; auto rd3 = [&]() -> uint32_t { r3++; __atomic_store_n(&raced, n48m6::kLatchOff, __ATOMIC_RELEASE); return n48m6::kLatchOn; };      // another thread latched OFF while this one was reading
          expect(!n48m6::latch_is_on(n48m6::latch_get(&raced, rd3)) && r3 == 1, "N1 latch_get: first writer wins - the loser of a race returns the stored word"); }
        // ---- N2 the interlock lifts --------------------------------------------------------------------------------------------------------------------------------------------------------
        { n48fb::Pre p {}; p.argOk = true; p.latched = true; p.held = true; p.pinned = true; p.devOk = true;
          p.pipeAdopted = true; expect_u("N2 R2: a pipe adopted refuses fbpublish with the latch OFF", n48fb::pre_verdict(p), n48fb::kPipeAdopted); p.m6 = true; expect_u("N2 R2: ... and does not with navi48-m6", n48fb::pre_verdict(p), n48fb::kOk);
          p.pipeAdopted = false; p.wsOpen = true; p.m6 = false; expect_u("N2 R2: a WindowServer session refuses it with the latch OFF", n48fb::pre_verdict(p), n48fb::kWsOpen); p.m6 = true; expect_u("N2 R2: ... and does not with navi48-m6", n48fb::pre_verdict(p), n48fb::kOk);
          p.wsOpen = false; p.metalNub = true; p.m6 = false; expect_u("N2 R2: the Metal nub refuses it with the latch OFF", n48fb::pre_verdict(p), n48fb::kMetalNub); p.m6 = true; expect_u("N2 R2: ... and does not with navi48-m6", n48fb::pre_verdict(p), n48fb::kOk);
          p.nubExists = true; expect_u("N2 R2: an existing nub of the SAME index is still refused under navi48-m6", n48fb::pre_verdict(p), n48fb::kExists);
          p.nubExists = false; p.held = false; expect_u("N2 R2: an unheld plane is still refused under navi48-m6", n48fb::pre_verdict(p), n48fb::kNotHeld);
          p.held = true; p.latched = false; expect_u("N2 R2: the navi48-fb2 latch is still required under navi48-m6", n48fb::pre_verdict(p), n48fb::kOff);
          p.latched = true; p.argOk = false; expect_u("N2 R2: a bad argument is still refused under navi48-m6", n48fb::pre_verdict(p), n48fb::kBadArg);
          p.argOk = true; p.devOk = false; expect_u("N2 R2: an unbound device is still refused under navi48-m6", n48fb::pre_verdict(p), n48fb::kNoDevice); }
        { n48metal::GateIn g {}; g.hello = true; g.bootarg = true; g.s1bGateOn = g.s1bRan = g.s1bPositive = true; g.dispNub = true;
          expect_u("N2 R3: the Metal publish is refused while a display nub exists (latch OFF)", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kExclusive);
          g.m6 = true; expect_u("N2 R3: ... and allowed under navi48-m6", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kOk);
          g.hung = true; expect_u("N2 R3: HUNG still refuses it under navi48-m6", n48metal::publish_verdict(g, n48metal::kOff), n48metal::kNotReady); g.hung = false;
          expect_u("N2 R3: a published nub still refuses a second one under navi48-m6", n48metal::publish_verdict(g, n48metal::kPublished), n48metal::kExclusive); }
        expect(fb_interlock_refuses(kActAdopt, 0, true, false) && fb_interlock_refuses(kActArm, 1, true, false) && !fb_interlock_refuses(kActAdopt, 0, true, true) && !fb_interlock_refuses(kActArm, 1, true, true) && !fb_interlock_refuses(kActAdopt, 0, false, false) && !fb_interlock_refuses(kActArm, 0, true, false) && fb_interlock_refuses(kActAdopt, 0, true), "N2 R4: pipeadopt / pipearm 1 are refused while a display nub exists unless navi48-m6; the 3-argument form is the old one");
        expect(kActM6Stat == 106u && action_admitted(true, kActM6Stat) && !action_admitted(false, kActM6Stat) && is_pipe_verb(kActM6Stat) && is_new_action(kActM6Stat) && verb_args_ok(kActM6Stat, 0) && verb_args_ok(kActM6Stat, 2) && verb_args_ok(kActM6Stat, 3) && verb_args_ok(kActM6Stat, 4) && !verb_args_ok(kActM6Stat, 5) && kStatPages == 5u && native_exempt(true, kActM6Stat, 1) && !native_exempt(false, kActM6Stat, 1), "N2 verb 106 m6stat: admitted with the display latch only, five pages (0.0.660: page 3 = the pipes' clocks and the self-healing counters; 0.0.662: page 4 = the AGDC replies per instance)");
        expect(std::strcmp(perf_name(kPfOtherInst), "unknown") != 0 && std::strcmp(perf_name(kPfRouteRefused), "unknown") != 0 && kPfCount == 23, "N2 the two new perform reasons are named");
        // ---- N3 adopt: EVERY pipe is judged -----------------------------------------------------------------------------------------------------------------------------------------------
        auto ent = [](uint32_t inst, uint64_t fb) { PipeEnt e {}; e.have = true; e.classOurs = true; e.traced = true; e.backAccel = true; e.backDm = true; e.fbOk = true; e.inst = inst; e.fb = fb; return e; };
        auto mprobe = [&](bool monb, bool mona, bool swapOrder) {
            PipeProbe p {}; p.accelOk = true; p.dmReadable = true; p.havePipe = true; p.classOurs = true; p.traced = true; p.backAccel = p.backDm = p.backFb = p.fbOurs = true;
            p.ents.expectMask = expect_mask(monb, mona); uint32_t n = 0;
            PipeEnt l[3]; uint32_t k = 0; l[k++] = ent(kInstDp, 0xD0); if (monb) l[k++] = ent(kInstMonB, 0xD2); if (mona) l[k++] = ent(kInstMonA, 0xD1);
            if (swapOrder && k >= 2) { PipeEnt t = l[0]; l[0] = l[k - 1]; l[k - 1] = t; }
            for (uint32_t i = 0; i < k; ++i) p.ents.e[n++] = l[i];
            p.ents.n = n; p.count = n; return p; };
        { const PipeProbe g1 = mprobe(true, false, false), g2 = mprobe(true, true, false), g3 = mprobe(false, false, false), sw = mprobe(true, true, true);
          expect(pipe_ok(g1) && pipe_ok(g2) && pipe_ok(g3) && pipe_ok(sw), "N3 the DP + monitor B, DP + monitor B + monitor A, DP alone and a REORDERED walk are all verified");
          expect_u("N3 the verdict of a good m6 probe", pipe_verdict(g2), kOk);
          // every field of every entry, one at a time: a damaged pipe 0, 1 or 2 is refused (the walk's order is not established)
          for (uint32_t k = 0; k < 3; ++k) for (int f = 0; f < 7; ++f) {
              PipeProbe p = mprobe(true, true, false); PipeEnt &x = p.ents.e[k];
              switch (f) { case 0: x.have = false; break; case 1: x.classOurs = false; break; case 2: x.traced = false; break; case 3: x.backAccel = false; break; case 4: x.backDm = false; break; case 5: x.fbOk = false; break; default: x.nullp = true; x.have = false; break; }
              expect(!pipe_ok(p), "N3 a damaged pipe in ANY slot refuses the probe (every pipe is judged, not dm+0x88[0] only)");
              expect(pipe_verdict(p) != kOk, "N3 ... with a named status"); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].inst = kInstDp; expect(!pipe_ok(p), "N3 two pipes on one instance are refused"); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[2] = ent(kInstMonB, 0xD3); p.ents.n = 3; p.count = 3;   // the expected set {DP, monitor B} is complete: only the DUPLICATE check can refuse the third pipe
            expect(!pipe_ok(p) && n48m6::ents_verdict(p.ents) == n48m6::kEntsInst, "N3 a THIRD pipe on an instance already holding one is refused although the expected set is complete (the duplicate check on its own)"); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].nullp = true; p.ents.e[1].have = false; expect_u("N3 ents_verdict names a NULL slot as such (kEntsNull), not as an unreadable one", n48m6::ents_verdict(p.ents), n48m6::kEntsNull); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].inst = kInstNone; p.ents.e[1].fbOk = true; expect_u("N3 ents_verdict: an unknown instance with a (wrongly) set fbOk is still refused (kEntsInst)", n48m6::ents_verdict(p.ents), n48m6::kEntsInst); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].inst = kInstNone; expect(!pipe_ok(p), "N3 a pipe on an unknown instance is refused"); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].inst = kInstMonA; expect(!pipe_ok(p), "N3 a pipe on an instance that has no published nub is refused"); }
          { PipeProbe p = mprobe(true, false, false); p.ents.n = 1; p.count = 1; expect(!pipe_ok(p), "N3 the monitor B is published but has no pipe: refused"); }
          { PipeProbe p = mprobe(false, false, false); p.ents.n = 2; p.count = 2; p.ents.e[1] = ent(kInstMonB, 0xD2); expect(!pipe_ok(p), "N3 a monitor B pipe with no published monitor B nub: refused"); }
          { PipeProbe p = mprobe(true, false, false); p.count = 3; expect(!pipe_ok(p), "N3 the display machine counts more pipes than were verified: refused"); }
          { PipeProbe p = mprobe(true, true, false); p.ents.n = 5; p.count = 5; expect(!pipe_ok(p), "N3 more pipes than the table holds: refused"); }
          { PipeProbe p = mprobe(true, false, false); p.accelOk = false; expect(!pipe_ok(p), "N3 no accelerator: refused"); p = mprobe(true, false, false); p.dmReadable = false; expect(!pipe_ok(p), "N3 no display machine: refused"); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].nullp = true; p.ents.e[1].have = false; expect_u("N3 a NULL pipe in slot 1 is the NULL-pipe verdict", pipe_verdict(p), kNullPipe); expect_u("N3 ... and adopt's precheck says so before anything is requested", adopt_precheck(p), kNullPipe); }
          expect_u("N3 precheck: a verified m6 set is 'already'", adopt_precheck(g2), kAlready);
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].classOurs = false; expect_u("N3 precheck: foreign pipes are refused", adopt_precheck(p), kHasPipes); }
          { PipeProbe p {}; p.accelOk = p.dmReadable = true; p.count = 0; expect_u("N3 precheck: no pipes yet is OK to request", adopt_precheck(p), kOk); }
          { PipeProbe p = mprobe(true, false, false); p.ents.e[1].classOurs = false; expect_u("N3 verdict: a foreign second pipe is PipeNotOurs", pipe_verdict(p), kPipeNotOurs);
            p = mprobe(true, false, false); p.ents.e[1].fbOk = false; expect_u("N3 verdict: an unknown framebuffer is a mismatch", pipe_verdict(p), kPipeMismatch);
            p = mprobe(true, false, false); p.ents.e[1].have = false; expect_u("N3 verdict: an unreadable slot is NoPipe", pipe_verdict(p), kNoPipe); } }
        // adopt_flow / arm_flow over the fake: a bad SECOND pipe stops adopt before anything is published or recorded, a good set is recorded
        auto none = [&]() { PipeProbe p {}; p.accelOk = true; p.dmReadable = true; return p; };      // the display machine holds no pipe yet: adopt asks for the probe
        { FakeEnv e; e.before = none(); e.after = mprobe(true, false, false); expect_u("N3 adopt: DP + monitor B verified", adopt_flow(e), kOk); expect(e.has("publish_caps") && e.has("record_adopted"), "N3 adopt: published and recorded"); }
        { FakeEnv e; PipeProbe p = mprobe(true, false, false); p.ents.e[1].classOurs = false; e.before = none(); e.after = p; expect_u("N3 adopt: a foreign SECOND pipe -> kPipeNotOurs", adopt_flow(e), kPipeNotOurs);
          expect(!e.has("publish_caps") && !e.has("record_adopted"), "N3 adopt: a foreign second pipe is neither published nor recorded"); }
        { FakeEnv e; PipeProbe p = mprobe(true, false, false); p.ents.e[1].fbOk = false; e.before = none(); e.after = p; expect_u("N3 adopt: a second pipe on an unknown framebuffer -> kPipeMismatch", adopt_flow(e), kPipeMismatch); expect(!e.has("record_adopted"), "N3 adopt: not recorded"); }
        { FakeEnv e; e.before = mprobe(true, true, true); e.after = mprobe(true, true, true); expect_u("N3 adopt: an already verified set is 'already', no new probe request", adopt_flow(e), kAlready); expect(!e.has("request_probe") && e.has("record_adopted"), "N3 adopt: already -> verified again, recorded"); }
        { FakeEnv e; e.factMask = kNeedFacts; e.current = mprobe(true, false, false); e.armedFlag = false; expect_u("N3 arm 1: a verified two-pipe set arms", arm_flow(e, 1), kOk); expect(e.armedFlag, "N3 arm 1: armed"); }
        { FakeEnv e; PipeProbe p = mprobe(true, false, false); p.ents.e[1].backAccel = false; e.factMask = kNeedFacts; e.current = p; e.armedFlag = false; expect_u("N3 arm 1: a damaged SECOND pipe refuses to arm", arm_flow(e, 1), kNoPipe); expect(!e.armedFlag && !e.has("arm_write1"), "N3 arm 1: nothing was written"); }
        // ---- N4 reachability: the routing guard through the dispatcher over the fake kernel -----------------------------------------------------------------------------------------------
        const uint32_t DPS = 101, DLS = 201, DLS2 = 202;
        auto m6world = [&](World &w) { FakeEnv &e = w.e; e.m6 = true; e.pipes = { A_PIPE, A_PIPE2 }; e.pinst[A_PIPE] = kInstDp; e.pinst[A_PIPE2] = kInstMonB; e.txnClass[A_SURF] = kSurfClass; e.txnClass[A_TXN] = kTxnClass; e.armedFlag = true; };
        auto settxn = [&](World &w, uint64_t pipe, uint32_t sid) { w.e.mem.put<uint64_t>(A_TXN + kTxnPipe, pipe); w.e.mem.put<uint32_t>(A_SURF + kSurfId, sid); };
        auto sub = [&](World &w, uint64_t pipe, uint32_t sid) { settxn(w, pipe, sid); uint64_t a[1] = { A_TXN }, r = 0; const int h = hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, pipe, a, 1, &r); expect(h == 1 && r == kWillPerform, "N4 submit is handled and will perform"); };
        auto perf = [&](World &w, uint64_t pipe, uint32_t sid) { settxn(w, pipe, sid); uint64_t a[1] = { A_TXN }, r = 5; const int h = hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 277, pipe, a, 1, &r); expect(h == 1 && r == 0, "N4 perform is handled and always succeeds"); };
        const bool savedPrep = gAutoPrep; gAutoPrep = false;
        { World w; m6world(w);       // the DP's own surface: learned, routed, copied
          sub(w, A_PIPE, DPS); perf(w, A_PIPE, DPS);
          expect(lookup_surface(w.e.surfTbl, DPS).found && lookup_surface(w.e.surfTbl, DPS).inst == kInstDp, "N4 the DP's surface is learned against instance 0");
          expect(w.e.reasons[kPfCopied] == 1 && w.e.m6Routes[kRouteOk] == 1 && !w.e.consoleUntouched() && std::memcmp(w.e.consoleData(), w.srcRow(0), 2560 * 4) == 0, "N4 the DP's pipe copies its own surface row by row");
          expect(w.e.vblSamples == 1 && w.e.m6SampleInst == 0 && w.e.m6StampOk[0] == 1, "N4 the DP's stamps come from the DP's OTG (the 0.0.658 sample)"); }
        { World w; m6world(w);       // the monitor B's pipe: learned, NO copy, no descriptor wired, stamps from OTG2
          sub(w, A_PIPE2, DLS);
          expect(lookup_surface(w.e.surfTbl, DLS).found && lookup_surface(w.e.surfTbl, DLS).inst == kInstMonB && w.e.m6Submits[kInstMonB] == 1, "N4 the monitor B's surface is learned against instance 2, one submit counted for it");
          expect(!w.e.has("md_prepare") && w.e.mdcache.prepared == 0, "N4 nothing is wired for a pipe on another instance");
          perf(w, A_PIPE2, DLS);
          expect(w.e.reasons[kPfOtherInst] == 1 && w.e.reasons[kPfCopied] == 0 && w.e.consoleUntouched() && w.e.padsClean() && w.e.srcReads == 0 && w.e.dstWrites == 0 && w.e.m6NoCopy[kInstMonB] == 1, "N4 the monitor B's pipe completes WITHOUT any copy: no source read, no console write");
          expect(w.e.m6SampleInst == kInstMonB && w.e.vblSamples == 0 && w.e.m6StampOk[kInstMonB] == 1 && w.e.m6StampOk[0] == 0, "N4 the monitor B's stamps come from OTG2 (instance 2's sample), never the DP's");
          { uint64_t t = w.e.mem.get<uint64_t>(A_TXN + kTxnVblTime), nx = w.e.mem.get<uint64_t>(A_TXN + kTxnVblNext); expect(t != 0 && nx == t + 16683333ull, "N4 the stamp words carry the MONB's period"); } }
        { World w; m6world(w);       // THE GUARD: a monitor B surface presented on the DP's pipe is refused
          sub(w, A_PIPE2, DLS); sub(w, A_PIPE, DPS);
          w.e.reasons[kPfRouteRefused] = 0; perf(w, A_PIPE, DLS);
          expect(w.e.reasons[kPfRouteRefused] == 1 && w.e.reasons[kPfCopied] == 0 && w.e.m6Routes[kRouteWrongInst] == 1 && w.e.consoleUntouched() && w.e.srcReads == 0, "N4 GUARD: a surface mapped to the monitor B, presented on the DP's pipe, is REFUSED: nothing read, nothing written");
          perf(w, A_PIPE, DPS);
          expect(w.e.reasons[kPfCopied] == 1, "N4 GUARD: the DP's own surface still goes through afterwards"); }
        { World w; m6world(w);       // an ID seen on two pipes is AMBIGUOUS and refused everywhere
          sub(w, A_PIPE, 301); perf(w, A_PIPE, 301); expect(w.e.reasons[kPfCopied] == 1, "N4 AMBIGUOUS: before the second sighting the DP's surface is presented");
          uint64_t a[1] = { A_TXN }, r = 0; settxn(w, A_PIPE2, 301); (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE2, a, 1, &r);
          expect(lookup_surface(w.e.surfTbl, 301).ambiguous && count_ambiguous(w.e.surfTbl) == 1, "N4 AMBIGUOUS: the ID seen on a second pipe is flagged");
          w.e.reasons[kPfCopied] = 0; perf(w, A_PIPE, 301);
          expect(w.e.reasons[kPfRouteRefused] == 1 && w.e.m6Routes[kRouteAmbiguous] == 1 && w.e.reasons[kPfCopied] == 0, "N4 AMBIGUOUS: the DP's present of an ambiguous ID is refused"); }
        { World w; m6world(w);       // an ID never seen on a pipe
          settxn(w, A_PIPE, 777); perf(w, A_PIPE, 777);
          expect(w.e.reasons[kPfRouteRefused] == 1 && w.e.m6Routes[kRouteUnknown] == 1 && w.e.consoleUntouched(), "N4 an ID never learned is refused (unknown)"); }
        { World w; m6world(w); w.e.pinst.erase(A_PIPE2); w.e.pipes.push_back(A_PIPE2);   // a pipe on a framebuffer we do not know
          sub(w, A_PIPE2, DLS2);
          expect(!lookup_surface(w.e.surfTbl, DLS2).found && w.e.surfTbl.learn[kLBadInst] == 1 && !w.e.has("md_prepare"), "N4 a pipe on no known framebuffer learns nothing and wires nothing");
          perf(w, A_PIPE2, DLS2);
          expect(w.e.reasons[kPfRouteRefused] == 1 && w.e.m6Routes[kRouteBadPipe] == 1 && w.e.consoleUntouched() && w.e.srcReads == 0, "N4 ... and its perform is refused without a copy");
          expect(!w.e.has("vbl_sample") && !w.e.has("vbl_sample_inst") && w.e.mem.get<uint64_t>(A_TXN + kTxnVblTime) == 0 && w.e.mem.get<uint64_t>(A_TXN + kTxnVblNext) == 0, "N4 ... and it gets NO vblank sample and no stamp (no OTG belongs to it)"); }
        { World w; m6world(w);       // the transaction names another pipe than the hooked one
          settxn(w, A_PIPE2, DPS); uint64_t a[1] = { A_TXN }, r = 0; (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 279, A_PIPE, a, 1, &r);
          expect(!lookup_surface(w.e.surfTbl, DPS).found && w.e.m6Skips[kSkPipeMismatch] == 1, "N4 txn+0x28 != the hooked pipe: nothing is learned"); }
        { World w; m6world(w); w.e.txnClass.erase(A_SURF);   // the object is not an IOSurface
          sub(w, A_PIPE, DPS); expect(!lookup_surface(w.e.surfTbl, DPS).found && w.e.m6Skips[kSkClass] == 1, "N4 a plane object that fails the IOSurface class check is not read for an ID");
          perf(w, A_PIPE, DPS); expect(w.e.reasons[kPfRouteRefused] == 1 && w.e.m6Routes[kRouteNoId] == 1 && w.e.consoleUntouched(), "N4 ... and the DP's present of it is refused (no ID)"); }
        { World w; m6world(w); sub(w, A_PIPE, 0); expect(w.e.surfTbl.n == 0 && w.e.surfTbl.learn[kLBadId] == 1, "N4 surface ID 0 is never learned"); }
        { World w; m6world(w); w.e.mem.put<uint64_t>(A_TXN + kTxnPlanes, 0); sub(w, A_PIPE, DPS); expect(w.e.m6Skips[kSkPlane] == 1 && w.e.surfTbl.n == 0, "N4 a transaction with no readable plane learns nothing"); }
        { World w; m6world(w);       // scan-owned: the monitor B's pipe still never copies, the DP's pipe is skipped before any source read
          w.e.scanOwned = true; sub(w, A_PIPE2, DLS); perf(w, A_PIPE2, DLS); perf(w, A_PIPE, DPS);
          expect(w.e.reasons[kPfOtherInst] == 1 && w.e.reasons[kPfScanOwned] == 1 && w.e.consoleUntouched(), "N4 scan-owned: the monitor B completes without a copy, the DP is skipped"); }
        { World w; m6world(w);       // disarmed: nothing at all
          w.e.armedFlag = false; sub(w, A_PIPE2, DLS); perf(w, A_PIPE2, DLS);
          expect(w.e.reasons[kPfDisarmed] == 1 && w.e.m6NoCopy[kInstMonB] == 0 && w.e.consoleUntouched(), "N4 disarmed: completed, nothing counted as routed"); }
        { World w; m6world(w); w.e.sampleOk = false; sub(w, A_PIPE2, DLS); perf(w, A_PIPE2, DLS);
          expect(w.e.m6SampleInst == kInstMonB && w.e.m6StampBad[kInstMonB] == 1 && w.e.reasons[kPfOtherInst] == 1, "N4 no coherent sample on the monitor B's OTG: counted against the monitor B, the transaction still completes"); }
        { World w; m6world(w); w.e.pinst[A_PIPE2] = kInstMonA; sub(w, A_PIPE2, 401); perf(w, A_PIPE2, 401);
          expect(w.e.m6SampleInst == kInstMonA && w.e.m6NoCopy[kInstMonA] == 1 && lookup_surface(w.e.surfTbl, 401).inst == kInstMonA, "N4 the monitor A's pipe: instance 1, OTG1's sample, no copy"); }
        { World w; m6world(w);       // the restart-loop guard counts WindowServer STARTS: slot 267 runs once per pipe, only the DP's pipe is counted
          uint64_t a[2] = { 0, A_RES }, r = 0;
          for (int i = 0; i < 6; ++i) (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE2, a, 2, &r);
          expect(w.e.autoDisarms == 0 && w.e.armedFlag && w.e.initFbNotes == 6, "N4 the guard: six slot-267 calls on the MONB's pipe do not trip the restart-loop guard (and the stand-in still answers)");
          for (int i = 0; i < 2; ++i) (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, a, 2, &r);
          expect(w.e.autoDisarms == 0 && w.e.armedFlag, "N4 the guard: two DP calls are still a healthy start");
          (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, a, 2, &r);
          expect(w.e.autoDisarms == 1 && !w.e.armedFlag && w.e.autoCause == kAdGuard, "N4 the guard: three DP calls inside the window still trip it (K3 unchanged for the DP)"); }
        { World w; w.e.pipes = { A_PIPE, A_PIPE2 }; w.e.m6 = false; w.e.armedFlag = true; uint64_t a[2] = { 0, A_RES }, r = 0;
          for (int i = 0; i < 3; ++i) (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE2, a, 2, &r);
          expect(w.e.autoDisarms == 1, "N4 OFF: the guard counts every known pipe's slot 267 exactly as 0.0.658 did"); }
        // OFF: the latch is the only switch - a second REGISTERED pipe is copied exactly as in 0.0.658
        { World w; w.e.pipes = { A_PIPE, A_PIPE2 }; w.e.m6 = false; sub(w, A_PIPE2, DLS); perf(w, A_PIPE2, DLS);
          expect(w.e.reasons[kPfCopied] == 1 && w.e.m6Learns == 0 && w.e.m6Submits[0] + w.e.m6Submits[1] + w.e.m6Submits[2] + w.e.m6Submits[3] == 0 && w.e.m6SampleInst == 0, "N4 OFF: latch OFF is 0.0.658 - a registered second pipe is copied and no M6 hook is touched"); }
        gAutoPrep = savedPrep;
        // ---- N5 AGDC: the 0x980 list and the per-endpoint 0x921 / 0x711 answers (the SAME code DisplayPipeGuard.cpp runs) -------------------------------------------------------------------------
        { expect(agdc_position(0, 2) == 0 && agdc_position(1, 2) == 1 && agdc_position(2, 2) == 0 && agdc_position(3, 2) == 0 && agdc_position(1, 1) == 0 && agdc_position(0, 1) == 0 && agdc_position(7, 3) == 0 && agdc_position(2, 3) == 2 && agdc_position(1, 3) == 1 && agdc_position(3, 3) == 0 && agdc_position(0xFFFFFFFFu, 3) == 0,
                 "N5 E1 endpoint -> list position: the INDEX reading (measured: dwords 0 and 1 with a 2-entry list); an endpoint at or past the list's end answers the DP");
          expect(agdc_ep_in_range(0, 2) && agdc_ep_in_range(1, 2) && !agdc_ep_in_range(2, 2) && !agdc_ep_in_range(0, 0) && !agdc_ep_in_range(0xFFFFFFFFu, 3), "N5 E1 in-range: ep < nfb");
          const uint32_t li[3] = { kInstDp, kInstMonB, kInstMonA };
          expect(agdc_instance(li, 3, 0) == kInstDp && agdc_instance(li, 3, 1) == kInstMonB && agdc_instance(li, 3, 2) == kInstMonA && agdc_instance(li, 3, 3) == kInstDp && agdc_instance(li, 3, 9) == kInstDp && agdc_instance(nullptr, 3, 1) == kInstDp && agdc_instance(li, 1, 1) == kInstDp, "N5 E1 endpoint -> instance (index reading; out of range = the DP)");
          const uint32_t l2[2] = { kInstDp, kInstMonB };
          expect(agdc_instance(l2, 2, 0) == kInstDp && agdc_instance(l2, 2, 1) == kInstMonB && agdc_instance(l2, 2, 2) == kInstDp, "N5 E1 the MEASURED run: a [DP, monitor B] list, endpoints 0 and 1 are the DP and the monitor B (0.0.659 answered the DP for both)"); }
        {
            AgdcList g {}; g.nfb = 2; g.inst[0] = kInstDp; g.inst[1] = kInstMonB; g.fb[0] = 0xFFFFFF8000AA0000ull; g.fb[1] = 0xFFFFFF8000BB0000ull;
            auto rd = [](const uint8_t *b, uint32_t o) { return (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24); };
            uint8_t buf[N48_AGDC_PIPELINE_CAPS_LEN]; uint32_t kr = 99;
            { std::memset(buf, 0xEE, sizeof buf); expect(agdc_answer(g, N48_AGDC_CMD_GPU_CAPABILITY, buf, N48_AGDC_GPU_CAP_LEN, 0xFFFFFF8000CC0000ull, &kr) && kr == 0 && rd(buf, 0x20) == 2 && rd(buf, 0x30) == 2 && rd(buf, 0x3c) == 0x00AA0000u && rd(buf, 0x44) == 0x00BB0000u && rd(buf, 0) == 6u && g.lastNfb == 2, "N5 0x980 lists BOTH framebuffers (nfb 2, mask bits 1 and 2, the DP then the monitor B)"); }
            { std::memset(buf, 0, sizeof buf); expect(agdc_answer(g, N48_AGDC_CMD_GPU_CAPABILITY, buf, N48_AGDC_GPU_CAP_LEN, 0, &kr) && kr != 0, "N5 0x980 without a PCI device is refused (kIOReturnBadArgument), not answered"); }
            { std::memset(buf, 0, sizeof buf); expect(agdc_answer(g, N48_AGDC_CMD_GPU_CAPABILITY, buf, N48_AGDC_GPU_CAP_LEN - 4, 0xFFFFFF8000CC0000ull, &kr) && kr != 0, "N5 0x980 with a wrong length is refused"); }
            { std::memset(buf, 0, sizeof buf); buf[0] = 1; expect(agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr) && kr == 0 && rd(buf, 0x44) == 2560u && rd(buf, 0x54) == 1440u && rd(buf, 0x2c) == (uint32_t)N48_DISP_PIXHZ && rd(buf, 0x48) == 160u && rd(buf, 0x58) == 41u && g.n921 == 1 && g.lastEp == 1 && g.nOor == 0,
                   "N5 0x921 for endpoint 1 (the monitor B) carries the monitor B's raster: 2560x1440, 241.5 MHz, 2720x1481 totals"); }
            { std::memset(buf, 0, sizeof buf); buf[0] = 0; expect(!agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr) && g.n921 == 1 && buf[0] == 0 && g.nOor == 0, "N5 0x921 for endpoint 0 (the DP) is NOT answered here: the 0.0.658 live-raster path runs, the buffer is untouched, nothing counted as out of range"); }
            { AgdcList o = g; uint8_t b2[N48_AGDC_PIPELINE_CAPS_LEN];                       // E1: an endpoint the list does not have is answered as the DP AND counted (0.0.659 had no such counter)
              std::memset(b2, 0, sizeof b2); b2[0] = 2; expect(!agdc_answer(o, N48_AGDC_CMD_LINK_CONFIG, b2, N48_AGDC_LINK_CONFIG_LEN, 0, &kr) && o.nOor == 1 && o.lastOor == 2 && b2[0] == 2 && o.n921 == g.n921, "N5 E1 0x921 for endpoint 2 of a 2-entry list: the DP answers (not answered here, buffer untouched), counted out of range (last 2)");
              std::memset(b2, 0, sizeof b2); b2[0] = 77; b2[4] = (uint8_t)N48_AGDC_SCALER_TYPE; expect(!agdc_answer(o, N48_AGDC_CMD_PIPELINE_CAPS, b2, N48_AGDC_PIPELINE_CAPS_LEN, 0, &kr) && o.nOor == 2 && o.lastOor == 77 && o.n711 == g.n711, "N5 E1 0x711 for endpoint 77: the DP answers, counted (2 so far, last 77)");
              std::memset(b2, 0, sizeof b2); b2[0] = 1; expect(agdc_answer(o, N48_AGDC_CMD_LINK_CONFIG, b2, N48_AGDC_LINK_CONFIG_LEN, 0, &kr) && o.nOor == 2, "N5 E1 an in-range endpoint is not counted");
              std::memset(b2, 0, sizeof b2); expect(agdc_answer(o, N48_AGDC_CMD_GPU_CAPABILITY, b2, N48_AGDC_GPU_CAP_LEN, 0xFFFFFF8000CC0000ull, &kr) && o.nOor == 2, "N5 E1 0x980 carries no endpoint and is never counted"); }
            { std::memset(buf, 0, sizeof buf); buf[0] = 0; expect(!agdc_answer(g, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr), "N5 0x921 for endpoint 0 falls back to the DP"); }
            { std::memset(buf, 0, sizeof buf); buf[0] = 1; buf[4] = (uint8_t)N48_AGDC_SCALER_TYPE; expect(agdc_answer(g, N48_AGDC_CMD_PIPELINE_CAPS, buf, N48_AGDC_PIPELINE_CAPS_LEN, 0, &kr) && kr == 0 && rd(buf, 0x08) == 1u && rd(buf, 0x30) == 2560u && rd(buf, 0x34) == 1440u && g.n711 == 1,
                   "N5 0x711 type 0x10 for endpoint 1 (the monitor B): one scaler entry of 2560x1440"); }
            { std::memset(buf, 0, sizeof buf); buf[0] = 0; buf[4] = (uint8_t)N48_AGDC_SCALER_TYPE; expect(!agdc_answer(g, N48_AGDC_CMD_PIPELINE_CAPS, buf, N48_AGDC_PIPELINE_CAPS_LEN, 0, &kr), "N5 0x711 for the DP's endpoint (0) is not answered here"); }
            { AgdcList one {}; one.nfb = 1; one.inst[0] = kInstDp; std::memset(buf, 0, sizeof buf); buf[0] = 1; expect(!agdc_answer(one, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr) && !agdc_answer(one, N48_AGDC_CMD_GPU_CAPABILITY, buf, N48_AGDC_GPU_CAP_LEN, 1, &kr), "N5 a one-framebuffer list (no latch) answers nothing here: 0.0.658's replies"); }
            { AgdcList zero {}; std::memset(buf, 0, sizeof buf); expect(!agdc_answer(zero, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr), "N5 an empty list answers nothing"); }
            { std::memset(buf, 0, sizeof buf); expect(!agdc_answer(g, N48_AGDC_CMD_VENDOR_INFO, buf, N48_AGDC_VENDOR_INFO_LEN, 0, &kr), "N5 vendor info is never this list's"); }
            { AgdcList three = g; three.nfb = 3; three.inst[2] = kInstMonA; three.fb[2] = 0xFFFFFF8000DD0000ull; std::memset(buf, 0, sizeof buf); buf[0] = 2;
              expect(agdc_answer(three, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr) && rd(buf, 0x44) == 1920u && rd(buf, 0x54) == 1080u && rd(buf, 0x2c) == (uint32_t)N48_DISPA_PIXHZ && rd(buf, 0x48) == 280u && rd(buf, 0x58) == 45u, "N5 endpoint 2 of three is the monitor A's 1920x1080 at 148.5 MHz (2200 x 1125)"); }
            { AgdcList big = g; big.nfb = 4; std::memset(buf, 0, sizeof buf); buf[0] = 1; expect(!agdc_answer(big, N48_AGDC_CMD_LINK_CONFIG, buf, N48_AGDC_LINK_CONFIG_LEN, 0, &kr), "N5 more than three framebuffers is not a list we answer"); }
            { n48_agdc_timing t {}; expect(agdc_timing_for_inst(kInstMonB, &t) && t.w == 2560 && t.h == 1440 && t.pixel_clock == N48_DISP_PIXHZ && agdc_timing_for_inst(kInstMonA, &t) && t.w == 1920 && t.h == 1080 && t.pixel_clock == N48_DISPA_PIXHZ && !agdc_timing_for_inst(kInstDp, &t) && !agdc_timing_for_inst(kInstNone, &t) && !agdc_timing_for_inst(kInstMonB, nullptr),
                   "N5 the per-index timing: the monitor B's and the monitor A's rasters from the geometry table, never the DP's"); }
        }
        // ---- N6 the kernel glue and the call sites (source pins; the behaviour above is the reachability) -----------------------------------------------------------------------------------------
        {
            const std::string m6p = slurp(K + "src/amd/native_m6_pure.h"), dnub = slurp(K + "src/Navi48DisplayNub.cpp");
            expect(glue.find("PE_parse_boot_argn(\"navi48-m6\", &v, sizeof(v))") != std::string::npos && glue.find("volatile uint32_t gM6Latch = n48m6::kLatchUnset;") != std::string::npos && glue.find("n48m6::latch_is_on(n48m6::latch_get(&gM6Latch, []() -> uint32_t {") != std::string::npos && glue.find("return n48m6::latch_value(present, v);") != std::string::npos, "N6 the navi48-m6 latch goes through the read-once latch_get, default unset = OFF");
            expect(glue.find("bool m6_on() { return n48m6_latched_on(); }") != std::string::npos && count_of(glue, "n48m6_latched_on()") >= 5, "N6 the flows' m6_on() is the latch accessor");
            expect(glue.find("if (n48m6_latched_on()) m6_record_map();") != std::string::npos, "N6 adopt records the framebuffer map under the latch");
            expect(glue.find("if (!n48m6flip_latched_on()) { m6_publish_now(); return; }") != std::string::npos && glue.find("const bool v2 = n48m6flip_latched_on();") != std::string::npos && glue.find("bool m6flip_on() { return n48m6flip_latched_on(); }") != std::string::npos, "0.0.661: with navi48-m6flip OFF every change publishes at once as a version-1 blob (the rate gate and the version-2 blob are behind the latch)");
            expect(flow.find("if (m6 && m6_route_flow(e, surf, pinst) != n48m6::kRouteOk) return kPfRouteRefused;") != std::string::npos && flow.find("if (!n48m6::pipe_may_copy(pinst)) {") != std::string::npos, "N6 perform_inner carries the non-DP completion and the guard");
            { const size_t a = flow.find("uint32_t perform_inner("), b = flow.find("if (!n48m6::pipe_may_copy(pinst))", a), c = flow.find("if (e.scan_active()) return kPfScanOwned;", a), d = flow.find("plane0_locate(e, txn, &surf, &res);", a), r = flow.find("m6_route_flow(e, surf, pinst)", a), s = flow.find("e.src_read(", a), t = flow.find("e.console_write(", a);
              expect(a != std::string::npos && b < c && c < d && d < r && r < s && s < t, "N6 ORDER in perform_inner: the non-DP completion < scan-owned < locate < the guard < the first source read < the first console write"); }
            { const size_t a = flow.find("case 279u: {"), b = flow.find("m6_learn_flow(e, self, args[0], inst)", a), c = flow.find("submit_prepare(e, args[0])", a); expect(a != std::string::npos && b != std::string::npos && c != std::string::npos && b < c, "N6 the submit hook learns BEFORE it wires, and wires only for the DP's pipe"); }
            expect(pure.find("action == kActM6Stat") != std::string::npos && pure.find("n48m6::ents_verdict(p.ents) == n48m6::kEntsOk") != std::string::npos, "N6 the pure verdicts carry verb 106 and the per-pipe check");
            expect(glue.find("m6_probe_ents(&p, acc, dm)") != std::string::npos && glue.find("for (uint32_t i = 0; i < p->count && i < n48m6::kMaxEnt; ++i)") != std::string::npos && glue.find("rd64(dm + n48disp::kDmPipes + 8u * i, &pp)") != std::string::npos, "N6 the kernel probe reads EVERY pipe slot of the display machine");
            expect(glue.find("pipe_inst(uint64_t pipe)") != std::string::npos && glue.find("rd64(pipe + n48disp::kPipeFb, &fb)") != std::string::npos && glue.find("n48m6::inst_of_fb(gD.m6Fb, fb)") != std::string::npos, "N6 a pipe's instance is its +0x98 framebuffer looked up in the adopt-recorded map");
            expect(glue.find("n48m6::learn_surface(gD.m6Surf, id, inst, n48m6flip_latched_on())") != std::string::npos && glue.find("Navi48MetalNub::setPublishedData(N48M6_PROP_SURF") != std::string::npos && glue.find("Navi48MetalNub::setPublishedNumber(N48M6_PROP_LATCH") != std::string::npos, "N6 the table is learned in the glue and published as the nub property the bundle reads");
            expect(glue.find("(void)n48m6::publish_serialised(gD.m6Pub,") != std::string::npos && glue.find(": n48m6::blob_build(gD.m6Surf, gM6Blob, sizeof gM6Blob); },") != std::string::npos && glue.find("Navi48MetalNub::setPublishedData(N48M6_PROP_SURF, gM6Blob, n)") != std::string::npos, "N6 (0.0.660 S2) m6_publish runs the SERIALISED publisher over the file-scope blob");
            expect(glue.find("uint8_t gM6Blob[n48m6::kBlobBufBytes];") != std::string::npos && glue.find("uint8_t blob[n48m6::kBlobMax]") == std::string::npos && count_of(glue, "setPublishedData(N48M6_PROP_SURF") == 1, "N6 (0.0.660 S2) the blob is a file-scope static, no ~200-byte local on the submit path, and the property has ONE writer");
            expect(glue.find("            m6_publish();\n        }\n        return r;\n    }") != std::string::npos && glue.find("if (n48m6::learn_changed(r)) {") != std::string::npos && glue.find("if (n48m6::learn_counted(r)) __atomic_add_fetch(&gD.m6Learned") != std::string::npos, "N6 the learn publishes the table on every CHANGE (a new ID, an ambiguous one, a re-learned one, an eviction)");
            expect(glue.find("__atomic_store_n(&gD.m6Fb.fb[i], 0ull, __ATOMIC_RELEASE);\n            IOService *o = gD.m6FbRef[i]; gD.m6FbRef[i] = nullptr;\n            if (o) o->release();") != std::string::npos, "N6 forgetting the pipes clears the framebuffer map FIRST, then drops the references");
            expect(brg.find("n48disp::kActM6Stat") != std::string::npos && glue.find("action == n48disp::kActM6Stat") != std::string::npos, "N6 verb 106 is routed to n48disp_verb and answered there");
            expect(cli.find("else if (what && !strcmp(what, \"m6stat\")) in = 106;") != std::string::npos && cli.find("in == 106 ? \"m6stat\"") != std::string::npos && cli.find("if (in == 106) return cmd_m6stat(arg);") != std::string::npos && cli.find("dr_call(106, page, o)") != std::string::npos, "N6 the CLI has `accel m6stat`: the table line, the name, the command, the page call");
            expect(dnub.find("pre.m6 = n48m6_latched_on();") != std::string::npos && nub.find("g.m6 = n48m6_latched_on();") != std::string::npos, "N6 the two interlock sites read the latch");
            expect(dnub.find("static IOService *framebufferOf") == std::string::npos && dnub.find("IOService *Navi48DisplayNub::framebufferOf(uint32_t index)") != std::string::npos && dnub.find("strcmp(cn, \"Navi48Framebuffer\") == 0 && svc->metaCast(\"IOFramebuffer\")") != std::string::npos, "N6 the nub finds its framebuffer child by class name and by IOFramebuffer ancestry");
            { const std::string dpg = slurp(K + "src/apple/DisplayPipeGuard.cpp"); const size_t a = dpg.find("static uint32_t agdc_vendor("), b = dpg.find("const bool m6done = gAgm.nfb > 1u && n48m6::agdc_answer(gAgm, cmd,", a), c = dpg.find("kr = n48_agdc_fill_vendor_info(", a), d = dpg.find("kr = agdc_link_config(", a);
              expect(a != std::string::npos && b != std::string::npos && b < c && c < d, "N6 agdc_vendor asks the per-endpoint answerer BEFORE any 0.0.658 fill");
              expect(dpg.find("static n48m6::AgdcList gAgm {};") != std::string::npos && dpg.find("gAgm.nfb = k;") != std::string::npos && dpg.find("if (st != 0u) gAgm.nfb = 0u;") != std::string::npos && dpg.find("if (ok && n48m6_latched_on()) ok = collect_m6();") != std::string::npos, "N6 the native route builds the list only under the latch and drops it when the build fails");
              expect(dpg.find("REFUSED - the display nub of index %u is published but its framebuffer has not started") != std::string::npos, "N6 a published nub without a started framebuffer refuses pipeagdc"); }
            { const std::string dc = slurp(K + "src/dcn/navi48_dcn.cpp");   // the latch line must NOT split `if (pixHz != 0) flags |= DTO_VALID; else {...}` (the else would then bind to the latch: a 0.0.658 behaviour change with the latch OFF); it sits after the else block
              const size_t dto = dc.find("if (pixHz != 0ull) flags |= N48N_SCANQ_DTO_VALID;\n\telse {"), ln = dc.find("\tif (n48m6_latched_on()) flags |= N48N_SCANQ_M6;"), pc = dc.find("pixHz = pc;\n\t}\n", dto == std::string::npos ? 0 : dto);
              expect(dto != std::string::npos && ln != std::string::npos && pc != std::string::npos && dto < pc && pc < ln && ln - pc < 20, "N6 scan_query: the DTO_VALID if/else is intact (0.0.658's) and the latch flag line comes after the else block"); }
            expect(slurp(K + "src/dcn/navi48_dcn.cpp").find("if (n48m6_latched_on()) flags |= N48N_SCANQ_M6;") != std::string::npos && slurp(K + "src/Navi48NativeABI.h").find("#define N48N_SCANQ_M6         (1u << 5)") != std::string::npos, "N6 scan_query reports the latch in its flags");
            expect(slurp(K + "src/dcn/navi48_dcn.cpp").find("bool vblSampleInst(uint32_t inst, uint32_t w, uint32_t h, uint64_t pixHz,") != std::string::npos && slurp(K + "src/dcn/navi48_dcn.cpp").find("VblCache gVblX[2] {};") != std::string::npos && slurp(K + "src/dcn/navi48_dcn.cpp").find("n48m6::otg_of_inst(inst)") != std::string::npos, "N6 the per-instance vblank sample reads that instance's own OTG with its own cache");
            // the forbidden spellings, in the new header
            { const std::string a = std::string("pipe+") + "0x280", b = std::string("pipe+") + "0x282", c = std::string("pipe+") + "0x299"; expect(m6p.find(a) == std::string::npos && m6p.find(b) == std::string::npos && m6p.find(c) == std::string::npos, "N6 the new header never spells the forbidden pipe offsets"); }
        }
        // ---- N8 the OpenCore variant: the ms-apps boot-args + navi48-fb2=1 navi48-m6=1, nothing else
        {
            const std::string base = slurp(root + "/variants/configs/stage17-native-1440-metal-disp-amfi-ms-apps.plist"), var = slurp(root + "/variants/configs/stage17-native-1440-metal-disp-amfi-m6.plist");
            const std::string tail = "navi48-dmubcmd=1 navi48-disp2=1</string>", add = "navi48-dmubcmd=1 navi48-disp2=1 navi48-fb2=1 navi48-m6=1</string>";
            expect(!base.empty() && !var.empty() && count_of(base, tail) == 1 && count_of(var, add) == 1, "N8 the variant ends its boot-args with navi48-fb2=1 navi48-m6=1 (once)");
            { std::string a = var; const size_t p = a.find(add); if (p != std::string::npos) a.replace(p, add.size(), tail); expect(a == base, "N8 the variant is the ms-apps plist with ONLY the boot-args string changed"); }
            expect(var.find("navi48-metal-ws=1") != std::string::npos && var.find("navi48-fb2=1") != std::string::npos && var.find("navi48-m6=1") != std::string::npos && var.find("navi48-disp2=1") != std::string::npos && var.find("navi48-metal-disp=1") != std::string::npos && var.find("navi48-dmubcmd=1") != std::string::npos, "N8 the variant carries metal-ws (the GPU desktop), disp2, dmubcmd, fb2 and m6");
            expect(count_of(var, "navi48-m6=1") == 1 && count_of(var, "navi48-fb2=1") == 1, "N8 each new boot-arg appears once");
            expect(base.find("navi48-m6") == std::string::npos && base.find("navi48-fb2") == std::string::npos, "N8 the ms-apps variant itself is untouched (no M6 arguments)");
        }
        // ---- N9 0.0.660 (queue 290): the self-healing surface table, the serialised publisher, the packing, the restart guard, the glue decisions, the clocks -----------------------------------------------------------------
        const bool savedPrep9 = gAutoPrep; gAutoPrep = false;
        {
            const std::string m6p = slurp(K + "src/amd/native_m6_pure.h");
            // ---- S1: the table heals itself ------------------------------------------------------------------------------------------------------------------------------------------------------------
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB);
              expect_u("N9 S1 an ID re-learned on another pipe while its owner is FRESH is ambiguous", learn_surface(t, 7, kInstDp), kLBecameAmbiguous);
              expect(lookup_surface(t, 7).found && lookup_surface(t, 7).ambiguous, "N9 S1 ... and refused");
              bool stillAmb = true; for (int i = 0; i < 2000; ++i) { const uint32_t a = learn_surface(t, 7, (i & 1) ? kInstDp : kInstMonB); stillAmb = stillAmb && a == kLWasAmbiguous; }
              expect(stillAmb && lookup_surface(t, 7).ambiguous && t.learn[kLReplaced] == 0, "N9 S1 a genuinely simultaneous two-pipe ID (both pipes keep submitting it, 2000 alternations) STAYS ambiguous: it is never mistaken for a reused ID"); }
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB);                                  // the OWNER's own clock: kStaleSubmits of its own submits since it last saw the ID
              for (uint32_t i = 0; i + 1 < kStaleSubmits; ++i) (void)learn_surface(t, 8 + (i & 1u), kInstMonB);       // 63 other submits on the monitor B
              expect_u("N9 S1 63 submits of the owner since it last saw the ID: not stale yet", learn_surface(t, 7, kInstDp), kLBecameAmbiguous); }
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB);
              for (uint32_t i = 0; i < kStaleSubmits; ++i) (void)learn_surface(t, 8 + (i & 1u), kInstMonB);           // 64 other submits on the monitor B
              expect_u("N9 S1 kStaleSubmits (64) submits of the owner since it last saw the ID: it went stale, the ID is RE-LEARNED on the new instance", learn_surface(t, 7, kInstDp), kLReplaced);
              { Look l = lookup_surface(t, 7); expect(l.found && !l.ambiguous && l.inst == kInstDp, "N9 S1 ... the entry now belongs to the DP, not ambiguous"); }
              expect_u("N9 S1 ... and stays the DP's", learn_surface(t, 7, kInstDp), kLSame); }
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB);                                  // the table's clock: an IDLE owner (a static monitor B stops submitting) is stale after kStaleEvents events
              for (uint32_t i = 0; i + 2 < kStaleEvents; ++i) (void)learn_surface(t, 20 + (i & 1u), kInstDp);        // 254 events
              expect_u("N9 S1 254 events since an idle owner last saw the ID: not stale", learn_surface(t, 7, kInstDp), kLBecameAmbiguous); }
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB);
              for (uint32_t i = 0; i + 1 < kStaleEvents; ++i) (void)learn_surface(t, 20 + (i & 1u), kInstDp);        // 255 events
              expect_u("N9 S1 kStaleEvents (256) events since an idle owner last saw the ID: stale, the ID is re-learned", learn_surface(t, 7, kInstDp), kLReplaced);
              expect(lookup_surface(t, 7).inst == kInstDp && !lookup_surface(t, 7).ambiguous, "N9 S1 ... and the DP owns it"); }
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB); (void)learn_surface(t, 7, kInstDp);          // an AMBIGUOUS entry heals when its other owner went stale
              expect(lookup_surface(t, 7).ambiguous, "N9 S1 (setup) 7 is ambiguous");
              for (uint32_t i = 0; i < kStaleEvents; ++i) (void)learn_surface(t, 20 + (i & 1u), kInstDp);
              expect_u("N9 S1 an ambiguous entry whose other owner went stale is HEALED by the next sighting on the live pipe", learn_surface(t, 7, kInstDp), kLReplaced);
              { Look l = lookup_surface(t, 7); expect(l.found && !l.ambiguous && l.inst == kInstDp && count_ambiguous(t) == 0, "N9 S1 ... the DP's, unambiguous"); } }
            { SurfTable t {}; (void)learn_surface(t, 7, kInstMonB); (void)learn_surface(t, 7, kInstDp);          // a still-live other owner keeps it ambiguous
              for (uint32_t i = 0; i < 40; ++i) { (void)learn_surface(t, 7, kInstMonB); (void)learn_surface(t, 7, kInstDp); }
              expect(lookup_surface(t, 7).ambiguous && t.learn[kLReplaced] == 0, "N9 S1 an ambiguous entry with both owners alive is NOT healed"); }
            // the churn: WindowServer reallocates ALL its surfaces, the DP's new surfaces reuse IDs the monitor B used to have. The DP's pipe must be ROUTED again (fail closed meanwhile, never forever).
            for (int monbLive = 0; monbLive < 2; ++monbLive) {
                SurfTable t {};
                const uint32_t dpOld[2] = { 1, 2 }, monbOld[3] = { 3, 4, 5 };
                for (int i = 0; i < 40; ++i) { (void)learn_surface(t, dpOld[i % 2], kInstDp); (void)learn_surface(t, monbOld[i % 3], kInstMonB); }
                expect(route_verdict(lookup_surface(t, 1), true, kInstDp) == kRouteOk && route_verdict(lookup_surface(t, 3), true, kInstMonB) == kRouteOk, "N9 S1 churn (setup): the DP routes {1,2}, the monitor B {3,4,5}");
                const uint32_t dpNew[2] = { 3, 4 }, monbNew[3] = { 1, 2, 5 };                     // everything reallocated: the DP now holds IDs the monitor B had
                int firstOk = -1; bool refusedFirst = false;
                for (int step = 0; step < 700; ++step) {
                    const uint32_t id = dpNew[step % 2];
                    (void)learn_surface(t, id, kInstDp);
                    const uint32_t v = route_verdict(lookup_surface(t, id), true, kInstDp);
                    if (step == 0) refusedFirst = v != kRouteOk;
                    if (v == kRouteOk && firstOk < 0 && lookup_surface(t, 3).inst == kInstDp && lookup_surface(t, 4).inst == kInstDp && !lookup_surface(t, 3).ambiguous && !lookup_surface(t, 4).ambiguous) firstOk = step;
                    if (monbLive || step < 6) (void)learn_surface(t, monbNew[step % 3], kInstMonB);     // monbLive: the monitor B keeps submitting its new surfaces; else it goes IDLE after 6 frames (a static screen)
                }
                expect(refusedFirst, "N9 S1 churn: the DP's FIRST present of an ID the monitor B still owns is refused (fail closed, the owner is fresh)");
                expect(firstOk >= 0 && firstOk <= (monbLive ? (int)kStaleSubmits + 4 : (int)kStaleEvents + 4), monbLive ? "N9 S1 churn, monitor B LIVE: the DP is routed again within kStaleSubmits of the monitor B's own submits" : "N9 S1 churn, monitor B IDLE: the DP is routed again within kStaleEvents events (never refused forever)");
                expect(route_verdict(lookup_surface(t, 3), true, kInstDp) == kRouteOk && route_verdict(lookup_surface(t, 4), true, kInstDp) == kRouteOk, "N9 S1 churn: the run ENDS with the DP routed (both of its surfaces)");
                expect(route_verdict(lookup_surface(t, 3), true, kInstMonB) != kRouteOk, "N9 S1 churn: ... and a monitor B pipe is never routed a DP surface"); }
            { SurfTable t {}; bool alwaysOk = true;                                                  // WS allocates 100 DISTINCT new IDs over time on one pipe: the 16-entry table never refuses the newest
              for (uint32_t id = 500; id < 600; ++id) { (void)learn_surface(t, id, kInstDp); alwaysOk = alwaysOk && route_verdict(lookup_surface(t, id), true, kInstDp) == kRouteOk; }
              expect(alwaysOk && t.n == kMaxSurf && t.learn[kLFull] == 0 && t.learn[kLEvicted] == 100 - kMaxSurf, "N9 S1 100 distinct IDs through a 16-entry table: every one routed on arrival, 84 evictions, kLFull never"); }
            { SurfTable t {}; for (uint32_t id = 1; id <= kMaxSurf; ++id) (void)learn_surface(t, id, kInstDp);
              expect(touch_surface(t, 1) && !touch_surface(t, 999) && !touch_surface(t, 0), "N9 S1 touch: a known ID is a sighting, an unknown ID and ID 0 are not");
              expect_u("N9 S1 LRU: a full table evicts the entry NOT SEEN for the longest time", learn_surface(t, 99, kInstDp), kLEvicted);
              expect(lookup_surface(t, 1).found && !lookup_surface(t, 2).found && lookup_surface(t, 3).found && lookup_surface(t, 99).found, "N9 S1 LRU: 1 survived because a routed present touched it, 2 (the oldest untouched) was evicted");
              SurfTable b {}; b.busy = 1; (void)b; expect(!touch_surface(b, 1), "N9 S1 touch is best effort: a busy table declines"); }
            { SurfTable t {}; for (uint32_t id = 1; id <= 5; ++id) (void)learn_surface(t, id, id % 2 ? kInstDp : kInstMonB); (void)learn_surface(t, 1, kInstMonB);
              expect(t.n == 5 && count_ambiguous(t) == 1 && t.txn[kInstDp] != 0 && t.seq != 0, "N9 S1 reset (setup)");
              expect(reset_table(t) && t.n == 0 && t.busy == 0u && t.resets == 1 && count_ambiguous(t) == 0 && !lookup_surface(t, 1).found && !lookup_surface(t, 2).found && t.txn[kInstDp] == 0 && t.txn[kInstMonB] == 0 && t.seq == 0, "N9 S1 reset: the table is EMPTY, the clocks are zero, the writer lock is released");
              expect_u("N9 S1 reset: a reused ID is simply NEW afterwards", learn_surface(t, 1, kInstMonB), kLNew);
              SurfTable b {}; b.busy = 1; expect(!reset_table(b) && b.resets == 0, "N9 S1 reset: gives up (false) when a learn holds the table, rather than waiting forever"); }
            { SurfTable t {}; uint8_t blob[kBlobMax]; for (uint32_t id = 1; id <= 4; ++id) (void)learn_surface(t, id, kInstDp); (void)reset_table(t);
              expect(blob_build(t, blob, sizeof blob) == kBlobHdr && blob[4] == 0, "N9 S1 the table published after a reset is the EMPTY one (the bundle gets n == 0, not the stale table)"); }
            expect(learn_changed(kLNew) && learn_changed(kLBecameAmbiguous) && learn_changed(kLReplaced) && learn_changed(kLEvicted) && !learn_changed(kLSame) && !learn_changed(kLWasAmbiguous) && !learn_changed(kLBusy) && !learn_changed(kLBadId) && !learn_changed(kLBadInst) && !learn_changed(kLFull), "N9 S1 which results change the table (and so are republished)");
            expect(learn_counted(kLNew) && learn_counted(kLSame) && learn_counted(kLReplaced) && learn_counted(kLEvicted) && learn_counted(kLWasAmbiguous) && !learn_counted(kLBusy) && !learn_counted(kLBadId), "N9 S1 which results count as a learned submit");
            // ---- S1 placement: where the reset runs, on the REAL flows ------------------------------------------------------------------------------------------------------------------------------------
            { FakeEnv e; e.m6 = true; e.factMask = kNeedFacts; e.current = good_probe(); (void)learn_surface(e.surfTbl, 5, kInstDp);
              expect_u("N9 S1 arm 1 (latch ON)", arm_flow(e, 1), kOk);
              expect(e.m6Resets == 1 && e.idx("m6_reset") >= 0 && e.idx("m6_reset") < e.idx("arm_write1") && e.surfTbl.n == 0 && e.m6Published == 1, "N9 S1 ORDER: every arm empties (and republishes) the table BEFORE the gate byte is written");
              expect_u("N9 S1 a RE-arm", arm_flow(e, 1), kOk); expect(e.m6Resets == 2, "N9 S1 ... resets again on every re-arm");
              expect_u("N9 S1 arm 0", arm_flow(e, 0), kOk); expect(e.m6Resets == 2, "N9 S1 a disarm does not reset"); }
            { FakeEnv e; e.m6 = true; e.latch = false; e.factMask = kNeedFacts; e.current = good_probe(); expect_u("N9 S1 arm refused (latch)", arm_flow(e, 1), kOff); expect(e.m6Resets == 0, "N9 S1 a REFUSED arm resets nothing (the table is only emptied when the gate is about to open)"); }
            { FakeEnv e; e.m6 = true; e.factMask = kNeedFacts; e.current = good_probe(); e.writeOk = false; (void)learn_surface(e.surfTbl, 5, kInstDp); expect_u("N9 S1 arm whose byte write fails", arm_flow(e, 1), kWriteFailed); expect(e.m6Resets == 1, "N9 S1 ... the reset still came first (the table is empty either way)"); }
            { FakeEnv e; e.m6 = false; e.factMask = kNeedFacts; e.current = good_probe(); (void)learn_surface(e.surfTbl, 5, kInstDp); expect_u("N9 S1 arm (latch OFF)", arm_flow(e, 1), kOk); expect(e.m6Resets == 0 && !e.has("m6_reset") && e.surfTbl.n == 1, "N9 S1 the latch OFF never resets (0.0.658 / 0.0.659 identical)"); }
            { World w; w.e.m6 = true; (void)learn_surface(w.e.surfTbl, 5, kInstDp); reload_open_flow(w.e);
              expect(w.e.m6Resets == 1 && !w.e.resetSawOpen && w.e.surfTbl.n == 0 && __atomic_load_n(&w.e.rwin.open, __ATOMIC_ACQUIRE) == 1u, "N9 S1 ORDER: pipereload empties the table BEFORE the restart window opens (the new WindowServer's first submits are never wiped)");
              reload_open_flow(w.e); expect(w.e.m6Resets == 2, "N9 S1 ... every `pipereload` (re)opens and resets"); }
            { World w; w.e.m6 = false; (void)learn_surface(w.e.surfTbl, 5, kInstDp); reload_open_flow(w.e); expect(w.e.m6Resets == 0 && w.e.surfTbl.n == 1, "N9 S1 pipereload with the latch OFF resets nothing"); }
            // ---- 0.0.661 R1 (queue 293, review B1): with navi48-m6flip the reset moves from the window OPEN to the new start's FIRST slot-267 ---------------------------------------------------------------------------
            { World w; w.e.m6 = true; w.e.m6flip = true; (void)learn_surface(w.e.surfTbl, 5, kInstDp); reload_open_flow(w.e);
              expect(w.e.m6Resets == 0 && w.e.surfTbl.n == 1 && __atomic_load_n(&w.e.rwin.open, __ATOMIC_ACQUIRE) == 1u, "R1: with m6flip `pipereload` does NOT empty the table when the window OPENS (the old WindowServer is still submitting)");
              (void)learn_surface(w.e.surfTbl, 7, kInstDp);   // the DYING WindowServer learns another ID after the window opened: IOSurface IDs are reused lowest-free
              expect(w.e.surfTbl.n == 2, "R1 (setup): the old start refilled the table after the window opened");
              expect(reload_tolerate_close(w.e), "R1 (setup): the old client's close is tolerated");
              expect(w.e.m6Resets == 0 && w.e.surfTbl.n == 2, "R1: the tolerated close resets nothing");
              expect(reload_on_init_fb(w.e) && w.e.m6Resets == 1 && w.e.surfTbl.n == 0 && !lookup_surface(w.e.surfTbl, 5).found && !lookup_surface(w.e.surfTbl, 7).found, "R1 ORDER: the FIRST slot-267 of the new start empties the table: nothing the old start learned survives into the new one");
              (void)learn_surface(w.e.surfTbl, 7, kInstMonB);   // the new WindowServer's monitor B surface reuses ID 7
              expect(reload_on_init_fb(w.e) && w.e.m6Resets == 1 && lookup_surface(w.e.surfTbl, 7).inst == kInstMonB, "R1: a SECOND slot-267 (the monitor B's / the monitor A's pipe) does not reset again, and the new start's learned entry stays (7 -> monitor B, never inherited 7 -> DP)"); }
            { World w; w.e.m6 = true; w.e.m6flip = true; (void)learn_surface(w.e.surfTbl, 5, kInstDp); reload_open_flow(w.e);
              (void)reload_on_init_fb(w.e);      // a slot-267 INSIDE the window before the old client's close (the old start's own call)
              expect(w.e.m6Resets == 0 && w.e.surfTbl.n == 1, "R1: a slot-267 before the old client's close was seen does NOT reset (only the new start's first one does)"); }
            { World w; w.e.m6 = true; w.e.m6flip = false; (void)learn_surface(w.e.surfTbl, 5, kInstDp); reload_open_flow(w.e); (void)reload_tolerate_close(w.e); (void)reload_on_init_fb(w.e);
              expect(w.e.m6Resets == 1, "R1: with m6flip OFF it is 0.0.660: ONE reset, at the window open, none at the 267"); }
            { World w; w.e.m6 = false; w.e.m6flip = true; (void)learn_surface(w.e.surfTbl, 5, kInstDp); reload_open_flow(w.e); (void)reload_tolerate_close(w.e); (void)reload_on_init_fb(w.e);
              expect(w.e.m6Resets == 0 && w.e.surfTbl.n == 1, "R1: m6flip without m6 resets nothing (every Stage-1b path needs both)"); }
            { World w; m6world(w); sub(w, A_PIPE, DPS); perf(w, A_PIPE, DPS); perf(w, A_PIPE, DPS);
              expect(w.e.m6Touches == 2 && w.e.m6Routes[kRouteOk] == 2, "N9 S1 each ROUTED present is a sighting (m6_touch); the learn at submit is the other one");
              w.e.reasons[kPfRouteRefused] = 0; perf(w, A_PIPE, 4242); expect(w.e.m6Touches == 2 && w.e.reasons[kPfRouteRefused] == 1, "N9 S1 a REFUSED present touches nothing"); }
            expect(glue.find("if (!n48m6::reset_table(gD.m6Surf)) {") != std::string::npos && glue.find("republishing the empty table\", (unsigned long long)gD.m6Surf.resets);\n        m6_publish();\n    }\n    void m6_touch(uint32_t id) { (void)n48m6::touch_surface(gD.m6Surf, id); }") != std::string::npos, "N9 S1 the glue's m6_reset empties the table and REPUBLISHES it (the bundle must not keep reading the old one)");
            // ---- S2: the serialised publisher ------------------------------------------------------------------------------------------------------------------------------------------------------------
            {
                SurfTable t {}; PubGate g {}; std::vector<std::vector<uint8_t>> pubs; uint8_t buf[kBlobMax]; int builds = 0;
                auto build = [&]() -> uint32_t { builds++; return blob_build(t, buf, sizeof buf); };
                auto rdn = [&](const std::vector<uint8_t> &b) { return (uint32_t)b[4] | ((uint32_t)b[5] << 8); };
                (void)learn_surface(t, 11, kInstDp);
                expect(publish_serialised(g, build, [&](uint32_t n) { pubs.emplace_back(buf, buf + n); return true; }) == 1 && builds == 1 && pubs.size() == 1 && rdn(pubs[0]) == 1 && g.published == 1 && g.busy == 0u, "N9 S2 one caller: one build, one publish, the gate is released");
                // another pipe's hook changes the table WHILE this one is publishing and asks to publish (it loses the gate and returns at once): the LAST publish must carry its change
                pubs.clear(); builds = 0; bool nested = false; uint32_t innerDone = 99;
                auto pubNested = [&](uint32_t n) { pubs.emplace_back(buf, buf + n);
                                                   if (!nested) { nested = true; (void)learn_surface(t, 12, kInstMonB); innerDone = publish_serialised(g, build, [&](uint32_t m) { pubs.emplace_back(buf, buf + m); return true; }); } return true; };
                (void)learn_surface(t, 13, kInstDp);
                const uint32_t mine = publish_serialised(g, build, pubNested);
                expect(innerDone == 0 && g.coalesced == 1, "N9 S2 a publish requested while another is in progress returns at once (coalesced), it never blocks");
                expect(mine == 2 && pubs.size() == 2 && builds == 2, "N9 S2 ... and the owner publishes AGAIN (it noticed the generation move)");
                expect(rdn(pubs.back()) == 3 && rdn(pubs.front()) == 2, "N9 S2 the LAST property written carries the newest table (3 IDs), the first carried the older one (2)");
                { SurfTable fin {}; uint8_t fb[kBlobMax]; (void)learn_surface(fin, 11, kInstDp); (void)learn_surface(fin, 13, kInstDp); (void)learn_surface(fin, 12, kInstMonB); const uint32_t n = blob_build(fin, fb, sizeof fb);
                  expect(n == pubs.back().size() && std::memcmp(fb, pubs.back().data(), n) == 0, "N9 S2 ... byte for byte the table as it is NOW"); }
                // the second window: a request that arrives after the owner's last generation check but before it lets go (the owner re-checks AFTER releasing the gate)
                pubs.clear(); builds = 0; bool seamDone = false;
                auto build2 = [&]() -> uint32_t { builds++; return blob_build(t, buf, sizeof buf); };
                const uint32_t m2 = publish_serialised_seam(g, build2, [&](uint32_t n) { pubs.emplace_back(buf, buf + n); return true; },
                                                            [&]() { if (!seamDone) { seamDone = true; (void)learn_surface(t, 14, kInstMonB); __atomic_add_fetch(&g.gen, 1u, __ATOMIC_SEQ_CST); __atomic_add_fetch(&g.coalesced, 1ull, __ATOMIC_RELAXED); } });
                expect(m2 == 2 && pubs.size() == 2 && rdn(pubs.back()) == 4, "N9 S2 a request that lost the gate just before the owner let go is NOT lost: the owner's post-release check publishes it (4 IDs last)");
                // a failing property write is not counted and does not loop forever
                PubGate g2 {}; int tries = 0; expect(publish_serialised(g2, build, [&](uint32_t) { tries++; return false; }) == 0 && tries == 1 && g2.published == 0 && g2.busy == 0u, "N9 S2 a publish that fails is not counted, does not retry in a loop, and releases the gate");
                PubGate g3 {}; expect(publish_serialised(g3, []() -> uint32_t { return 0u; }, [&](uint32_t) { return true; }) == 0 && g3.busy == 0u, "N9 S2 an empty blob (nothing to publish) publishes nothing and releases the gate");
            }
            {   // real concurrency: four threads change a versioned table and publish; the last property written must carry the final version
                PubGate g {}; std::atomic<uint32_t> version { 0 }; std::atomic<uint32_t> lastPublished { 0 }; std::atomic<uint32_t> inPub { 0 }; std::atomic<bool> overlap { false };
                auto worker = [&]() { for (int i = 0; i < 4000; ++i) { version.fetch_add(1); (void)publish_serialised(g, [&]() -> uint32_t { return version.load() | 0x80000000u; },
                    [&](uint32_t n) { if (inPub.fetch_add(1) != 0) overlap = true; lastPublished.store(n & 0x7fffffffu); std::this_thread::yield(); inPub.fetch_sub(1); return true; }); } };
                std::vector<std::thread> th; for (int i = 0; i < 4; ++i) th.emplace_back(worker); for (auto &x : th) x.join();
                expect(lastPublished.load() == 16000u && version.load() == 16000u, "N9 S2 four threads x 4000 changes: the LAST publish carries the final version (no older table ever wins)");
                expect(!overlap.load() && g.busy == 0u && g.published + g.coalesced >= 16000u / 2u, "N9 S2 ... no two publishes ever overlapped, the gate is released");
            }
            // ---- S5: the restart-loop guard fails CLOSED ------------------------------------------------------------------------------------------------------------------------------------------------
            expect(restart_guard_counts(false, kInstDp) && restart_guard_counts(false, kInstMonB) && restart_guard_counts(false, kInstMonA) && restart_guard_counts(false, kInstNone), "N9 S5 latch OFF: every pipe is counted (0.0.658)");
            expect(restart_guard_counts(true, kInstDp) && !restart_guard_counts(true, kInstMonB) && !restart_guard_counts(true, kInstMonA) && restart_guard_counts(true, kInstNone), "N9 S5 latch ON: the DP AND an unplaceable pipe are counted; only a KNOWN monitor A / monitor B pipe is exempt");
            { World w; m6world(w); w.e.pinst.erase(A_PIPE2); uint64_t a[2] = { 0, A_RES }, r = 0;      // a pipe whose framebuffer is not in the map (kInstNone)
              for (int i = 0; i < 2; ++i) (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE2, a, 2, &r);
              expect(w.e.autoDisarms == 0 && w.e.armedFlag, "N9 S5 two slot-267 calls on an unplaceable pipe are a healthy start");
              (void)hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE2, a, 2, &r);
              expect(w.e.autoDisarms == 1 && !w.e.armedFlag && w.e.autoCause == kAdGuard, "N9 S5 THREE slot-267 calls on an unplaceable pipe trip the restart-loop guard (0.0.659 counted none of them: a WindowServer crash loop could hide behind it)"); }
            // ---- N2: saturating pack ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
            expect(sat_bits(0, 8) == 0 && sat_bits(255, 8) == 255 && sat_bits(256, 8) == 255 && sat_bits(1ull << 40, 16) == 65535 && sat_bits(~0ull, 64) == ~0ull && sat_bits(65535, 16) == 65535 && sat_bits(65536, 16) == 65535, "N9 N2 sat_bits clamps at the field's maximum");
            { uint32_t bitsum = 0; for (uint8_t b : kPackRoute) bitsum += b; expect(bitsum == 64u, "N9 N2 page 1 out[11]: four 16-bit fields fill the word");
              bitsum = 0; for (uint8_t b : kPackLearnSkip) bitsum += b; expect(bitsum == 64u, "N9 N2 page 2 out[12]: eight 8-bit fields fill the word");
              bitsum = 0; for (uint8_t b : kPackAgdc) bitsum += b; expect(bitsum == 64u, "N9 N2 page 1 out[12] (AGDC): 8+16+16+24 bits");
              bitsum = 0; for (uint8_t b : kPackResetTouch) bitsum += b; uint32_t b2 = 0; for (uint8_t b : kPackPair32) b2 += b; expect(bitsum == 64u && b2 == 64u, "N9 N2 page 3 words"); }
            { const uint64_t f[4] = { 70000, 3, 5, 65535 + 7 };                                     // the first and last overflow 16 bits
              const uint64_t w = pack_sat(f, kPackRoute, 4);
              expect((w & 0xFFFF) == 65535 && ((w >> 16) & 0xFFFF) == 3 && ((w >> 32) & 0xFFFF) == 5 && ((w >> 48) & 0xFFFF) == 65535, "N9 N2 an overflowing route counter saturates at 65535 and does NOT spill into its neighbour (0.0.659 added 70000's high bits to the next field)"); }
            { const uint64_t f[8] = { 300, 1, 2, 3, 4, 1000, 6, 255 };
              const uint64_t w = pack_sat(f, kPackLearnSkip, 8);
              expect((w & 0xFF) == 255 && ((w >> 8) & 0xFF) == 1 && ((w >> 16) & 0xFF) == 2 && ((w >> 24) & 0xFF) == 3 && ((w >> 32) & 0xFF) == 4 && ((w >> 40) & 0xFF) == 255 && ((w >> 48) & 0xFF) == 6 && ((w >> 56) & 0xFF) == 255, "N9 N2 the 8-bit learn-bookkeeping fields saturate at 255 (and keep the 0.0.659 layout the CLI decodes)"); }
            { const uint64_t f[4] = { 3, 70000, 5, 0x1FFFFFFull };
              const uint64_t w = pack_sat(f, kPackAgdc, 4);
              expect((w & 0xFF) == 3 && ((w >> 8) & 0xFFFF) == 65535 && ((w >> 24) & 0xFFFF) == 5 && ((w >> 40) & 0xFFFFFF) == 0xFFFFFFull, "N9 N2 the AGDC word keeps the CLI's layout and saturates each field"); }
            expect(glue.find("out[11] = n48m6::pack_sat(f, n48m6::kPackRoute, 4u);") != std::string::npos && glue.find("out[12] = n48m6::pack_sat(f, n48m6::kPackLearnSkip, 8u);") != std::string::npos && glue.find("out[12] = n48m6::pack_sat(ag, n48m6::kPackAgdc, 4u);") != std::string::npos, "N9 N2 the glue packs through pack_sat at all three sites (no raw shift-or of counters is left)");
            expect(glue.find("(gD.m6Route[n48m6::kRouteNoId] << 48)") == std::string::npos && glue.find("(ag[3] << 40)") == std::string::npos && glue.find("(gD.m6LearnSkip[n48m6::kSkClass] << 24)") == std::string::npos, "N9 N2 the 0.0.659 raw shift-or of the counters is gone from all three sites");
            // ---- N4 -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
            expect(m6p.find("static_assert(kInstMonA >= 1u && kInstMonB > kInstMonA,") != std::string::npos && m6p.find("for (uint32_t inst = kInstMonB; inst >= kInstMonA; --inst)") != std::string::npos && count_of(m6p, "inst >= kInstMonA; --inst") == 2u && count_of(slurp(K + "src/apple/DisplayPipeGuard.cpp"), "inst >= n48m6::kInstMonA") == 0, "N9 N4 the downward unsigned instance loops live in ONE header, next to a static_assert(kInstMonA >= 1) (a 0 would wrap and never end)");
            static_assert(kInstMonA >= 1u, "N9 N4 (compile time) the loops end");
            // ---- T1: the glue decisions the 0.0.659 tests could not reach ------------------------------------------------------------------------------------------------------------------------------
            {
                struct CEnv { bool pub[kInstCount] = { false, false, false }; bool started[kInstCount] = { false, false, false }; std::vector<uint32_t> asked, refused; bool nub_published(uint32_t i) { return pub[i]; } bool fb_started(uint32_t i) { asked.push_back(i); return started[i]; } void refuse(uint32_t i) { refused.push_back(i); } };
                { CEnv e; uint32_t n = 99; expect(collect_flow(e, &n) && n == 1 && e.asked.empty(), "N9 T1 collect: no display nub published: just the DP, nothing is looked up"); }
                { CEnv e; e.pub[kInstMonB] = e.started[kInstMonB] = true; uint32_t n = 0; expect(collect_flow(e, &n) && n == 2 && e.asked.size() == 1 && e.asked[0] == kInstMonB, "N9 T1 collect: the monitor B published and started: two framebuffers"); }
                { CEnv e; e.pub[kInstMonB] = e.started[kInstMonB] = e.pub[kInstMonA] = e.started[kInstMonA] = true; uint32_t n = 0; expect(collect_flow(e, &n) && n == 3 && e.asked.size() == 2 && e.asked[0] == kInstMonB && e.asked[1] == kInstMonA, "N9 T1 collect: both nubs: three framebuffers, the monitor B asked BEFORE the monitor A (display index order)"); }
                { CEnv e; e.pub[kInstMonB] = true; uint32_t n = 77; expect(!collect_flow(e, &n) && e.refused.size() == 1 && e.refused[0] == kInstMonB && n == 77, "N9 T1 collect: a PUBLISHED nub whose framebuffer has not started REFUSES (it is never skipped into a shorter list)"); }
                { CEnv e; e.pub[kInstMonB] = e.started[kInstMonB] = true; e.pub[kInstMonA] = true; uint32_t n = 77; expect(!collect_flow(e, &n) && e.refused.size() == 1 && e.refused[0] == kInstMonA && n == 77, "N9 T1 collect: the monitor A's unstarted framebuffer refuses although the monitor B's is fine"); }
                { CEnv e; e.pub[kInstMonB] = true; e.pub[kInstMonA] = true; e.started[kInstMonA] = true; uint32_t n = 0; expect(!collect_flow(e, &n) && e.refused[0] == kInstMonB && e.asked.size() == 1, "N9 T1 collect: refused at the first bad nub, nothing further is looked up"); }
                expect(collect_verdict(false, false) == kColSkip && collect_verdict(false, true) == kColSkip && collect_verdict(true, true) == kColUse && collect_verdict(true, false) == kColRefuse, "N9 T1 collect_verdict table");
                const bool h0[kInstCount] = { true, false, false }, h1[kInstCount] = { true, false, true }, h2[kInstCount] = { true, true, true }, h3[kInstCount] = { true, true, false }; uint32_t o[kInstCount];
                expect(list_insts(h0, o) == 1 && o[0] == kInstDp, "N9 T1 the 0x980 list: the DP alone");
                expect(list_insts(h1, o) == 2 && o[0] == kInstDp && o[1] == kInstMonB, "N9 T1 the 0x980 list: [DP, monitor B]");
                expect(list_insts(h2, o) == 3 && o[0] == kInstDp && o[1] == kInstMonB && o[2] == kInstMonA, "N9 T1 the 0x980 list: [DP, monitor B, monitor A] (endpoint index = this position)");
                expect(list_insts(h3, o) == 2 && o[0] == kInstDp && o[1] == kInstMonA, "N9 T1 the 0x980 list: [DP, monitor A]");
                struct REnv { bool lit = true; uint32_t aw = 2560, ah = 1440, ht1 = 2719, vt1 = 1480; bool okSize = true, okTot = true; int totCalls = 0;
                              bool active_size(uint32_t, bool *l, uint32_t *w, uint32_t *h) { *l = lit; *w = aw; *h = ah; return okSize; } bool totals(uint32_t, uint32_t *a, uint32_t *b) { totCalls++; *a = ht1; *b = vt1; return okTot; } };
                uint32_t ht = 0, vt = 0;
                { REnv e; expect(raster_flow(e, 2, 2560, 1440, &ht, &vt) && ht == 2720 && vt == 1481 && e.totCalls == 1, "N9 T1 raster: a LIT OTG with exactly the display's size: totals + 1"); }
                { REnv e; e.lit = false; expect(!raster_flow(e, 2, 2560, 1440, &ht, &vt) && e.totCalls == 0, "N9 T1 raster: an unlit OTG is refused before the totals are read"); }
                { REnv e; e.aw = 1920; expect(!raster_flow(e, 2, 2560, 1440, &ht, &vt) && e.totCalls == 0, "N9 T1 raster: a lit OTG with the WRONG WIDTH is refused (0.0.659's untested `aw != w`)"); }
                { REnv e; e.ah = 1080; expect(!raster_flow(e, 2, 2560, 1440, &ht, &vt) && e.totCalls == 0, "N9 T1 raster: a lit OTG with the WRONG HEIGHT is refused (0.0.659's untested `ah != h`)"); }
                { REnv e; e.okSize = false; expect(!raster_flow(e, 2, 2560, 1440, &ht, &vt), "N9 T1 raster: an unreadable active size is refused"); }
                { REnv e; e.okTot = false; expect(!raster_flow(e, 2, 2560, 1440, &ht, &vt), "N9 T1 raster: unreadable totals are refused"); }
                { REnv e; expect(!raster_flow(e, 2, 0, 0, &ht, &vt), "N9 T1 raster: a display of size 0 x 0 is never accepted"); }
                expect(raster_accepted(true, 1920, 1080, 1920, 1080) && !raster_accepted(false, 1920, 1080, 1920, 1080) && !raster_accepted(true, 2560, 1080, 1920, 1080) && !raster_accepted(true, 1920, 1440, 1920, 1080) && !raster_accepted(true, 0, 0, 0, 0), "N9 T1 raster_accepted truth table");
                const std::string dpg = slurp(K + "src/apple/DisplayPipeGuard.cpp"), dcn = slurp(K + "src/dcn/navi48_dcn.cpp");
                expect(dpg.find("bool collect_m6() { mn = 1u; return n48m6::collect_flow(*this, &mn); }") != std::string::npos && dpg.find("const uint32_t k = n48m6::list_insts(have, order);") != std::string::npos, "N9 T1 DisplayPipeGuard.cpp runs the tested collect and list-order code (no loop of its own)");
                expect(dcn.find("if (!n48m6::raster_flow(renv, otg, w, h, &hTot, &vTot)) return false;") != std::string::npos && dcn.find("aw != w") == std::string::npos, "N9 T1 navi48_dcn.cpp's per-instance sample runs the tested raster acceptance (no `aw != w` of its own)");
            }
            // ---- K1: the pipes' clocks -------------------------------------------------------------------------------------------------------------------------------------------------------------------
            expect(txn_age_ms(5000000000ull, 0ull) == kAgeNever && txn_age_ms(5000000000ull, 4995000000ull) == 5ull && txn_age_ms(5000000000ull, 5000000000ull) == 0ull && txn_age_ms(1000ull, 2000ull) == 0ull && txn_age_ms(9000000000ull, 5000000000ull) == 4000ull, "N9 K1 age = ms since the pipe's last transaction; 'never' is all ones; a clock step back is 0, not a huge number");
            expect(glue.find("if (n48m6::inst_valid(inst)) __atomic_store_n(&gD.m6LastTxnNs[inst], now_ns(), __ATOMIC_RELAXED);") != std::string::npos && glue.find("out[2 + i] = n48m6::txn_age_ms(now, __atomic_load_n(&gD.m6LastTxnNs[i], __ATOMIC_RELAXED));") != std::string::npos && glue.find("} else if (arg == 3ull) {") != std::string::npos, "N9 K1 the glue stamps every submit with its instance's clock and m6stat page 3 reports the ages");
            { World w; m6world(w); uint32_t calls = 0; (void)calls; sub(w, A_PIPE, DPS); expect(w.e.m6Submits[kInstDp] == 1, "N9 K1 (flow) every submit is noted against its instance (the glue stamps the time there)"); }
            // ---- S7 ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
            expect(glue.find("void m6_collect_fbs(n48m6::FbMap *m, IOService **refs) {") != std::string::npos && count_of(glue, "m6_collect_fbs(&map, refs);") == 2 && count_of(glue, "m6_release_refs(refs);") == 1 && count_of(glue, "->retain()") == 1 && glue.find("IOService *o = reinterpret_cast<IOService *>(static_cast<uintptr_t>(map.fb[i]));") == std::string::npos, "N9 S7 the framebuffer references come from m6_collect_fbs and are handed over or released: no raw address is re-retained");
            expect(glue.find("if (gD.m6FbRef[i]) { refs[i]->release(); refs[i] = nullptr; continue; }") != std::string::npos && glue.find("gD.m6FbRef[i] = refs[i];") != std::string::npos, "N9 S7 record_map keeps the collected reference (or gives it back when the slot is already recorded)");
        }
        gAutoPrep = savedPrep9;
        // ---- N7 the OFF-identity battery (0.0.658's behaviour, hashed) ----------------------------------------------------------------------------------------------------------------------------
        { const uint64_t h = m6bat::flipoff_table_hash(); std::printf("M6 flip-OFF table hash %016llx (golden from the 0.0.660 sources: 4f2e1572db464b5f)\n", (unsigned long long)h);
          expect(h == 0x4f2e1572db464b5full, "R-OFF: with navi48-m6flip OFF the surface table (learn / evict / ambiguity / reset / blob) is BYTE-FOR-BYTE the 0.0.660 one (the battery hashes to the 0.0.660 golden)"); }
        { const uint64_t h = m6bat::off_identity_hash();
          std::printf("M6 OFF-identity battery hash %016llx (golden from the 0.0.658 sources: 5c2ba78279eaa1f8)\n", (unsigned long long)h);
          expect(h == 0x5c2ba78279eaa1f8ull, "N7 the latch-OFF paths (adopt, arm, the four hooks, perform on one and on two pipes, the stamps, the pure verdicts) are BYTE-FOR-BYTE the 0.0.658 ones"); }
    }

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
