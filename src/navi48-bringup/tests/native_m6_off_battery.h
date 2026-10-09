// native_m6_off_battery.h - build 0.0.659 (M6 Stage 1a): THE OFF-IDENTITY BATTERY. With the boot-arg navi48-m6 absent every path of the kext must be 0.0.658's. This battery runs the functions M6 changed
// (the pure verdicts of native_disp_pure.h / native_fb_pure.h / native_metal_pure.h and the flows of native_disp_flow.h: adopt, arm, the four pipe hooks, perform, the vblank stamps, auto-disarm, withdraw) through a fixed
// script with the latch OFF - including perform on a SECOND registered pipe, which 0.0.658 copies into the DP console and which the latch must keep copying - and hashes every verdict, return value, call log, counter,
// console byte and fake-kernel-memory byte. The golden hash was produced by compiling THIS FILE against the 0.0.658 sources (git a5fa7557: extract with git archive a5fa7557 src/navi48-bringup into a scratch directory, compile with
// -I <scratch>/src/navi48-bringup/src -I <scratch>/src/navi48-bringup/src/amd) and running its main(): the same hash under the 0.0.659 sources proves the OFF path is unchanged. It uses ONLY members the 0.0.658 sources already
// had (the new reason codes, new verbs and the new fields are never fed: loops stop at action 105 and reason 20), and its environment answers the new concept members (m6_on() false) so the 0.0.659 flows can instantiate.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <functional>
#include "amd/native_disp_flow.h"

namespace m6bat {
using namespace n48disp;

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

struct BatEnv {
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

    BatEnv() { setConsole(2560, 1440, 10240); }
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

    // M6 concept members: NEVER used with the latch OFF (the 0.0.658 flows do not know them; the 0.0.659 flows ask m6_on() first and get false)
    bool m6_on() { return false; }
    bool m6flip_on() { return false; }   // 0.0.661: the flows ask it only behind m6_on()
    uint32_t pipe_inst(uint64_t) { return 0xFFFFFFFFu; }
    bool vbl_sample_inst(uint32_t, VblSample *) { return false; }
#if __has_include("amd/native_m6_pure.h")      // the 0.0.659 tree only: the 0.0.658 flows never name these
    n48m6::Look m6_lookup(uint32_t) { return n48m6::Look { false, false, 0xFFFFFFFFu }; }
    uint32_t m6_learn(uint32_t, uint32_t) { return 0; }
    void m6_note_submit(uint32_t) {}
    void m6_note_learn_skip(uint32_t) {}
    void m6_note_route(uint32_t, uint32_t) {}
    void m6_note_nocopy(uint32_t) {}
    void m6_note_stamp(uint32_t, uint32_t, uint64_t) {}
    void m6_reset() {}                  // 0.0.660: never reached with the latch OFF
    void m6_touch(uint32_t) {}
#endif

    int idx(const char *s) const { for (size_t i = 0; i < log.size(); ++i) if (log[i] == s) return (int)i; return -1; }
    bool has(const char *s) const { return idx(s) >= 0; }
    int count(const char *s) const { int n = 0; for (auto &l : log) if (l == s) n++; return n; }
};

static PipeProbe bat_good_probe() {
    PipeProbe p {}; p.accelOk = true; p.dmReadable = true; p.count = 1; p.havePipe = true; p.classOurs = true; p.traced = true;
    p.backAccel = p.backDm = p.backFb = true; p.fbOurs = true; return p;
}

// A complete world: accelerator, display machine, event machine, pipe, and one transaction carrying a 2560x1440 BGRA plane 0.
static bool gBatAutoPrep = false;     // (battery) the world runs the submit hook once (so perform has a prepared descriptor) unless a test turns this off
struct BatWorld {
    BatEnv e;
    uint64_t W = 2560, H = 1440, sbpr = 10240;
    std::vector<uint8_t> src;
    BatWorld(uint64_t w = 2560, uint64_t h = 1440, uint64_t stride = 10240) : W(w), H(h), sbpr(stride) {
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
        if (gBatAutoPrep) { submit(); e.reads = e.writes = 0; e.log.clear(); }
    }
    int submit(uint64_t *ret = nullptr) { uint64_t a[1] = { A_TXN }, r = 0; const int h = hook_dispatch(e, N48_VC_DISPLAYPIPE, 279, A_PIPE, a, 1, ret ? ret : &r); return h; }
    // run the dispatcher for slot 277 and return (handled, ret)
    int perform(uint64_t *ret) { uint64_t a[1] = { A_TXN }; return hook_dispatch(e, N48_VC_DISPLAYPIPE, 277, A_PIPE, a, 1, ret); }
    const uint8_t *srcRow(uint64_t y) const { return &src[(size_t)(y * sbpr)]; }
};


struct H { uint64_t h = 0xcbf29ce484222325ull; void u(uint64_t v) { for (int i = 0; i < 8; i++) { h ^= (v >> (8 * i)) & 0xFFu; h *= 0x100000001b3ull; } } void b(const void *p, size_t n) { const uint8_t *q = (const uint8_t *)p; for (size_t i = 0; i < n; i++) { h ^= q[i]; h *= 0x100000001b3ull; } } };

static void hw(H &h, BatWorld &w) {      // everything observable after a scenario
    BatEnv &e = w.e;
    for (auto &l : e.log) { h.b(l.data(), l.size()); h.u(l.size()); }
    for (uint32_t r = 0; r < 21u; ++r) h.u(e.reasons[r]);
    h.u(e.bytesNoted); h.u(e.performNotes); h.u(e.sourceNotes); h.u(e.autoDisarms); h.u(e.nullPipeNotes); h.u(e.withdrawNotes); h.u(e.scanSkipSubmits); h.u(e.initFbNotes); h.u(e.reads); h.u(e.writes);
    h.u(e.mdcache.prepared); h.u(e.mdcache.hits); h.u(e.mdcache.misses); h.u(e.mdcache.tornDown); h.u(e.mdcache.leaked);
    for (uint32_t i = 0; i < kVblCount; ++i) h.u(e.vblNotes[i]);
    h.u(e.lastVblPeriod); h.u(e.lastVblDelay); h.u(e.armedFlag ? 1u : 0u); h.u(e.armByte); h.u(e.factMask); h.u(e.autoCause);
    h.b(e.consoleBuf.data(), e.consoleBuf.size());
    for (auto &r : e.mem.regs) { h.u(r.base); h.b(r.b.data(), r.b.size()); }
}

static uint64_t off_identity_hash() {
    H h;
    // ---- the pure verdicts ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    for (int latch = 0; latch < 2; ++latch) for (uint32_t a = 0; a <= 105u; ++a) {
        h.u(action_admitted(latch != 0, a)); h.u(is_new_action(a)); h.u(is_pipe_verb(a));
        for (uint64_t g = 0; g <= 4u; ++g) { h.u(verb_args_ok(a, g)); h.u(native_exempt(latch != 0, a, g)); }
    }
    for (uint32_t a = 83; a <= 90u; ++a) for (uint64_t g = 0; g <= 2u; ++g) for (int nub = 0; nub < 2; ++nub) h.u(fb_interlock_refuses(a, g, nub != 0));
    for (uint32_t m = 0; m < 1024u; ++m) for (uint32_t c = 0; c < 3u; ++c) {
        PipeProbe p {};
        p.accelOk = m & 1; p.dmReadable = (m >> 1) & 1; p.havePipe = (m >> 2) & 1; p.nullPipe = (m >> 3) & 1; p.classOurs = (m >> 4) & 1; p.traced = (m >> 5) & 1;
        p.backAccel = (m >> 6) & 1; p.backDm = (m >> 7) & 1; p.backFb = (m >> 8) & 1; p.fbOurs = (m >> 9) & 1; p.count = c;
        h.u(pipe_ok(p)); h.u(pipe_verdict(p)); h.u(adopt_precheck(p));
    }
    for (uint64_t g = 0; g <= 3u; ++g) for (uint32_t m = 0; m < 16u; ++m) { ArmIn a { (m & 1) != 0, (m & 2) != 0, (m & 4) != 0, (m & 8) != 0 }; h.u(arm_decide(g, a)); }
    for (uint32_t r = 0; r <= 20u; ++r) h.b(perf_name(r), std::strlen(perf_name(r)));
    for (uint32_t s = 0; s <= 18u; ++s) h.b(status_name(s), std::strlen(status_name(s)));
    for (uint32_t m = 0; m < 512u; ++m) for (uint32_t st = 0; st < 3u; ++st) {
        n48metal::GateIn g {};
        g.flags = (m & 1) ? 1u : 0u; g.hello = (m >> 1) & 1; g.bootarg = (m >> 2) & 1; g.s1bGateOn = (m >> 3) & 1; g.s1bRan = (m >> 4) & 1; g.s1bPositive = (m >> 5) & 1; g.s1bStopped = (m >> 6) & 1; g.hung = (m >> 7) & 1; g.dispNub = (m >> 8) & 1;
        h.u(n48metal::publish_verdict(g, (n48metal::State)st)); h.u(n48metal::withdraw_verdict(g, (n48metal::State)st));
    }
    for (uint32_t m = 0; m < 512u; ++m) {
        n48fb::Pre p {};
        p.argOk = m & 1; p.latched = (m >> 1) & 1; p.nubExists = (m >> 2) & 1; p.held = (m >> 3) & 1; p.pinned = (m >> 4) & 1; p.devOk = (m >> 5) & 1; p.pipeAdopted = (m >> 6) & 1; p.wsOpen = (m >> 7) & 1; p.metalNub = (m >> 8) & 1;
        h.u(n48fb::pre_verdict(p));
    }
    // ---- the flows (latch OFF: m6_on() is false) -----------------------------------------------------------------------------------------------------------------------------------------------------
    auto hook = [](BatWorld &w, uint32_t slot, uint64_t pipe, uint64_t arg, uint64_t *ret) { uint64_t a[1] = { arg }; return hook_dispatch(w.e, N48_VC_DISPLAYPIPE, slot, pipe, a, 1, ret); };
    auto scenario = [&](const char *name, const std::function<void(BatWorld &, H &)> &fn) { H local; BatWorld w; w.e.vblOnFlag = true; fn(w, h); hw(h, w); h.b(name, std::strlen(name)); (void)local; };
    scenario("normal", [&](BatWorld &w, H &hh) { uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(r); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); hh.u(r2); });
    scenario("vbl stamps", [&](BatWorld &w, H &hh) { w.e.txnClass[A_TXN] = kTxnClass; uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); hh.u(r2); });
    scenario("vbl no sample", [&](BatWorld &w, H &hh) { w.e.txnClass[A_TXN] = kTxnClass; w.e.sampleOk = false; uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("scan owned", [&](BatWorld &w, H &hh) { w.e.scanOwned = true; uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("disarmed", [&](BatWorld &w, H &hh) { w.e.armedFlag = false; uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("not prepared", [&](BatWorld &w, H &hh) { uint64_t r2 = 7; hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("bad txn", [&](BatWorld &w, H &hh) { uint64_t r2 = 7; hh.u(hook(w, 277, A_PIPE, 0x1000ull, &r2)); hh.u(hook(w, 279, A_PIPE, 0x1000ull, &r2)); });
    scenario("format refused", [&](BatWorld &w, H &hh) { w.e.mem.put<uint32_t>(A_SURF + kSurfFmt, 0x41524742u); uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("geometry disagrees", [&](BatWorld &w, H &hh) { w.e.mem.put<uint16_t>(A_RES + kResW, 2000); uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("source read fails", [&](BatWorld &w, H &hh) { w.e.srcFailAfter = 100; uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE, A_TXN, &r2)); });
    scenario("submit status", [&](BatWorld &w, H &hh) { w.e.mem.put<uint32_t>(A_TXN + kTxnStatus, 0xE00002BEu); uint64_t r = 7; hh.u(hook(w, 279, A_PIPE, A_TXN, &r)); hh.u(r); });
    scenario("is complete", [&](BatWorld &w, H &hh) { uint64_t r = 7; hh.u(hook(w, 278, A_PIPE, A_TXN, &r)); hh.u(r); });
    scenario("unknown pipe", [&](BatWorld &w, H &hh) { uint64_t r = 7; hh.u(hook(w, 279, A_PIPE2, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE2, A_TXN, &r)); hh.u(hook(w, 278, A_PIPE2, A_TXN, &r)); });
    scenario("second registered pipe copies (0.0.658)", [&](BatWorld &w, H &hh) { w.e.pipes.push_back(A_PIPE2); uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE2, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE2, A_TXN, &r2)); hh.u(r2); });
    scenario("second pipe vbl", [&](BatWorld &w, H &hh) { w.e.pipes.push_back(A_PIPE2); w.e.txnClass[A_TXN] = kTxnClass; uint64_t r = 7, r2 = 7; hh.u(hook(w, 279, A_PIPE2, A_TXN, &r)); hh.u(hook(w, 277, A_PIPE2, A_TXN, &r2)); });
    scenario("slot 267", [&](BatWorld &w, H &hh) { uint64_t a[2] = { 0, A_RES }, r = 0; hh.u(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, a, 2, &r)); hh.u(r != 0); hh.u(w.e.mem.get<uint8_t>(A_PIPE + kPipeActive)); });
    scenario("slot 267 x3 trips the guard", [&](BatWorld &w, H &hh) { uint64_t a[2] = { 0, A_RES }, r = 0; for (int i = 0; i < 3; ++i) hh.u(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, a, 2, &r)); });
    scenario("arm 1 / 0", [&](BatWorld &w, H &hh) { w.e.current = bat_good_probe(); w.e.armedFlag = false; hh.u(arm_flow(w.e, 1)); hh.u(arm_flow(w.e, 0)); hh.u(arm_flow(w.e, 2)); });
    scenario("arm 1 no pipe", [&](BatWorld &w, H &hh) { w.e.armedFlag = false; hh.u(arm_flow(w.e, 1)); });
    scenario("adopt good", [&](BatWorld &w, H &hh) { w.e.after = bat_good_probe(); hh.u(adopt_flow(w.e)); });
    scenario("adopt already", [&](BatWorld &w, H &hh) { w.e.before = bat_good_probe(); w.e.after = bat_good_probe(); hh.u(adopt_flow(w.e)); });
    scenario("adopt foreign pipes", [&](BatWorld &w, H &hh) { PipeProbe p = bat_good_probe(); p.classOurs = false; w.e.before = p; hh.u(adopt_flow(w.e)); });
    scenario("adopt not ours after", [&](BatWorld &w, H &hh) { PipeProbe p = bat_good_probe(); p.classOurs = false; w.e.after = p; hh.u(adopt_flow(w.e)); });
    scenario("adopt mismatch after", [&](BatWorld &w, H &hh) { PipeProbe p = bat_good_probe(); p.backFb = false; w.e.after = p; hh.u(adopt_flow(w.e)); });
    scenario("adopt two pipes after", [&](BatWorld &w, H &hh) { PipeProbe p = bat_good_probe(); p.count = 2; w.e.after = p; hh.u(adopt_flow(w.e)); });
    scenario("adopt null pipe", [&](BatWorld &w, H &hh) { PipeProbe p {}; p.accelOk = true; p.dmReadable = true; p.count = 1; p.nullPipe = true; w.e.before = p; hh.u(adopt_flow(w.e)); });
    scenario("adopt probe fails", [&](BatWorld &w, H &hh) { w.e.probeRc = false; hh.u(adopt_flow(w.e)); });
    scenario("adopt caps fail", [&](BatWorld &w, H &hh) { w.e.after = bat_good_probe(); w.e.capsOk = false; hh.u(adopt_flow(w.e)); });
    scenario("adopt off", [&](BatWorld &w, H &hh) { w.e.latch = false; hh.u(adopt_flow(w.e)); });
    scenario("withdraw", [&](BatWorld &w, H &) { withdraw_flow(w.e); });
    scenario("ws close disarms", [&](BatWorld &w, H &hh) { hh.u(ws_client_closed_flow(w.e, true)); hh.u(ws_client_closed_flow(w.e, false)); });
    scenario("hung disarms", [&](BatWorld &w, H &hh) { hh.u(hung_latched_flow(w.e)); });
    scenario("reload window", [&](BatWorld &w, H &hh) { reload_open_flow(w.e); hh.u(ws_client_closed_flow(w.e, true)); uint64_t a[2] = { 0, A_RES }, r = 0; hh.u(hook_dispatch(w.e, N48_VC_DISPLAYPIPE, 267, A_PIPE, a, 2, &r)); });
    scenario("slot 62", [&](BatWorld &w, H &hh) { uint64_t a[2] = { A_MD + 0x10, A_MD + 0x20 }; uint64_t r = 0; hh.u(res62_flow(w.e, A_RES, a, 2, &r, true)); hh.u(r); });
    return h.h;
}
// ---- 0.0.661 (M6 Stage 1b): THE FLIP-OFF IDENTITY. With navi48-m6flip OFF the surface table code is 0.0.660's. This runs the table through a fixed script - learns on all three instances (ambiguity, stale replacement, a FULL table
// that evicts), sightings, resets, the property blob - using ONLY entry points and members 0.0.660 had (the 3-argument learn_surface, blob_build, lookup_surface, touch_surface, reset_table, count_*), and hashes every verdict, every
// lookup, every blob byte and the counters. The golden is the hash of THIS FILE compiled against the 0.0.660 sources (git archive 84f3d2c9); the same hash under 0.0.661 proves the latch-OFF table behaves as before
// (the R3 victim rule, the R4 reset value and the R2 generation are all behind the latch or invisible to the hash: reset writes kInstNone instead of 0, which no lookup, count or blob can see).
static uint64_t flipoff_table_hash() {
    using namespace n48m6;
    H h;
    SurfTable t {};
    uint64_t x = 0x9E3779B97F4A7C15ull;
    auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
    uint8_t blob[kBlobMax];
    for (int step = 0; step < 4000; ++step) {
        const uint32_t id = 1u + (uint32_t)(rnd() % 40u), inst = (uint32_t)(rnd() % 3u), act = (uint32_t)(rnd() % 20u);
        if (act == 0u) { h.u(reset_table(t) ? 1u : 0u); }
        else if (act < 4u) { h.u(touch_surface(t, id) ? 1u : 0u); }
        else { h.u(learn_surface(t, id, inst)); }
        const Look l = lookup_surface(t, id);
        h.u(l.found ? 1u : 0u); h.u(l.ambiguous ? 1u : 0u); h.u(l.inst);
        if (step % 50 == 0) { const uint32_t n = blob_build(t, blob, sizeof blob); h.u(n); h.b(blob, n); h.u(count_for_inst(t, 0)); h.u(count_for_inst(t, 1)); h.u(count_for_inst(t, 2)); h.u(count_ambiguous(t)); }
    }
    h.u(t.n); h.u(t.resets); h.u(t.touches); h.u(t.seq);
    for (uint32_t k = 0; k < kLearnCount; ++k) h.u(t.learn[k]);
    return h.h;
}
}  // namespace m6bat
