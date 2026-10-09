// gfx_hw72_test.cpp — build 0.0.532 (; an internal design note): SWITCH 72 M3 (840), THE HEAP COPY HELD FOR
// START-UP FILLS UNTIL THE FILL RETIRES, BY ITS OWN FENCE. Host proof of gfx_heapgen.h's M3 steps and the kext's wiring.
//
// T27 (the design's name; tests/gfx_copyguard_test.cpp's T27 is 0.0.511's, untouched) is T26's scripted two-thread scheduler grown
// to M3: the SUBMIT thread runs the kext's events at fixed times - the top of the pass (FB), the commit judge (J: hg_commit_judge,
// startup latch + n48_hg_pend_set), the gate's record (R), the fence stamp after the ring push (P: hg_pend_fence), the ring walk
// (W: n48_hg_walk_answer) and the hook's EXIT 3 (X: hg_pend_exit3, then hg_pend_exit), plus judged-frame polls (RET_POLL) - and the
// GPU writes the flight's fence `want` into its owned slot at tF (only a SPARED flight runs). The COPIER runs hw_hg_copy_begin's
// order for each copy: the lock, the 0.0.511 `unpatched` count, n48_hg_copy_wait3 (M3) or n48_hg_copy_wait (M1), the bump. Its
// `delay` advances the clock and runs every submit event now due (the lock released, as the other thread would); its `read_fence`
// returns the slot as the GPU has left it at that instant and records whether the lock was held (it must not be). Every decision
// is gfx_heapgen.h's. The copier's writes are "under the live frame" when they start (the bump) after the walk SPARED the frame
// and before its fence landed: that is the F4-BAD exposure M3 exists to remove.
//
// Times come from the real logs, one log line = 50 us (notes/logs/runs/run11o, run11i, run11n, run11m; RUN L as T26 has it):
//   AC seq 4 (run11o): gate 16095, copy 16216 (32 patches, 0 unpatchable), push 16218, walk/EXIT 16299/16300, fence 0x50ae0001 at
//                      vram+0x3cba8f004 (the 72 OFF log: heap-gen, NOPed).
//   AC seq 5 (run11o): copy 19241 (18 patches) AFTER the judge and BEFORE the gate 19254, push 19276, walk/EXIT 19348/19349, fence
//                      0x50ae0002 at vram+0x3cba8f008.
//   Z2 seq 4 (run11i): gate 15712, copy 15822 (32 patches), push 15847, walk/EXIT 15922/15923, fence 0x76270001.
//   AB3 seq 4 (run11n): gate 16074, push 16192, EXIT 16266 SPARED, copy 16278 (30 patches) DURING the flight, poll 16980 retired
//                      (0xfff00001).
//   AB2 frame 20 (run11m): copy 18715 (19 patches) BEFORE the pass (18732), judge 18747 refused copy-in-progress.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_hw72_test.cpp -o /tmp/hw72 && \
//         /tmp/hw72 src/navi48-bringup/src/apple/AppleHardwareHook.cpp
//
// The AppleHardwareHook.cpp argument is REQUIRED (the wiring section fails without it).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <initializer_list>

#include "gfx_heapgen.h"
#include "gfx_fillset.h"
#include "gfx_desc_port.h"

static int gChecks = 0, gFails = 0;
static void expect(const char *what, bool ok)
{
    gChecks++;
    if (!ok) gFails++;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// =============================================================================================================================
// THE MODEL
// =============================================================================================================================
static const uintptr_t kSub = 0x5501u, kCopier = 0x7702u;
static const uint64_t kLo = 0x10020000ull, kHi = 0x10029000ull;
static const uint64_t kNever = ~0ull;
static const uint64_t kLineUs = 50u;

struct HCopy { uint64_t at, dur; uint32_t np, npoison, noOverlay, scanFailed, noPlan; uintptr_t thread; };
enum { EV_FB = 0, EV_J, EV_R, EV_P, EV_W, EV_X, EV_D, EV_POLL0, EV_N = EV_POLL0 + 4 };
struct Sim {
    n48_hg_state st; n48_hg_pend pd; n48_hg_frame f, rec;
    uint64_t now; int held; uint32_t lockOps, delays, reads, readsHeld;
    // configuration
    uint32_t mode;          // 2 = M2 (72 OFF, the default), 1 = M1 (0.0.511), 3 = M3
    uint32_t startup;       // the pre-plane window at the judge
    uint32_t gate;          // the gate commits an OK frame
    uint32_t seq; uint64_t fenceOff; uint32_t want, slot0;   // slot0: the slot's value before this flight's fence lands
    uint64_t t[EV_N];       // event times (kNever = no such event); EV_D: a disarm
    uint64_t fenceLat;      // the fence lands this long after the walk, if SPARED (kNever: never)
    // outcomes
    uint32_t done[EV_N];
    uint32_t judgeWhy, committed, walked, walkWhy, spared, x3;
    uint64_t walkedAt, tF;
    HCopy c[4]; uint32_t nc, ci, started[4], bumped[4], ended[4], look[4];
    uint64_t bumpAt[4], endAt[4], waited[4], lastEnd;
    uint32_t bumpUnderLive, bumpInWindow;
};
static Sim gS;

static uint32_t sim_slot(const Sim &s) { return (s.tF != kNever && s.now >= s.tF) ? s.want : s.slot0; }
static void sim_submit_due(Sim &s);
static uint64_t cb_now(void *c) { return static_cast<Sim *>(c)->now; }
static void cb_lock(void *c) { Sim *s = static_cast<Sim *>(c); s->held++; s->lockOps++; }
static void cb_unlock(void *c) { Sim *s = static_cast<Sim *>(c); s->held--; s->lockOps++; }
static void cb_delay(void *c, uint32_t us) { Sim *s = static_cast<Sim *>(c); s->delays++; s->now += us; sim_submit_due(*s); }
static uint32_t cb_read(void *c, uint64_t off, uint32_t *val)
{
    Sim *s = static_cast<Sim *>(c);
    s->reads++;
    if (s->held) s->readsHeld++;                 // PLANT 10's catch: the fence is read with gHgLock released, always
    *val = (off == s->fenceOff) ? sim_slot(*s) : 0xdeadbeefu;
    return 1u;
}
static const n48_hg_wait_ops kOps3 = { &cb_now, &cb_lock, &cb_unlock, &cb_delay, &cb_read };
static const n48_hg_wait_ops kOps1 = { &cb_now, &cb_lock, &cb_unlock, &cb_delay, nullptr };

// The submit thread's events, in the kext's order; each runs only when gHgLock is free (the copier holds it except around its
// delays and its fence read) and its predecessor ran.
static void sim_submit_due(Sim &s)
{
    if (s.held) return;
    const uint32_t latch = s.mode == 1u || s.mode == 3u;
    for (uint32_t e = 0; e < EV_N; e++) {
        if (s.done[e] || s.t[e] == kNever || s.now < s.t[e]) continue;
        if (e >= EV_J && e <= EV_X && !s.done[e - 1u]) break;      // FB -> J -> R -> P -> W -> X, in order
        s.done[e] = 1u;
        switch (e) {
        case EV_FB: n48_hg_frame_begin(&s.f, 1u, 5u, &s.st); break;                                   // hg_frame_begin_pass
        case EV_J:                                                                                        // hg_commit_judge
            s.judgeWhy = n48_hg_judge(&s.st, &s.f, 5u);
            if (s.pd.m3) s.pd.startup = s.startup;                                                        // item 3's latch
            if (s.judgeWhy == N48_HG_OK && latch) n48_hg_pend_set(&s.pd, kSub, s.now);
            break;
        case EV_R:                                                                                        // hg_commit_record
            s.committed = (s.judgeWhy == N48_HG_OK && s.gate) ? 1u : 0u;
            if (latch) n48_hg_pend_record(&s.pd, s.committed ? s.seq : 0u, kSub);
            s.rec = s.f;
            break;
        case EV_P: if (s.committed && s.pd.m3) (void)n48_hg_pend_fence(&s.pd, s.seq, kSub, s.fenceOff, s.want); break;  // item 4
        case EV_W:                                                                                        // the ring walk
            if (s.committed) {
                s.walkWhy = n48_hg_walk_answer(&s.st, &s.rec, &s.pd, s.seq, kSub);
                s.walked = 1u; s.walkedAt = s.now; s.spared = s.walkWhy == N48_HG_OK;
                if (s.spared && s.fenceLat != kNever) s.tF = s.now + s.fenceLat;   // only a spared flight runs and writes its fence
            }
            break;
        case EV_X:                                                                                        // EXIT 3
            if (s.pd.m3) s.x3 = n48_hg_pend_exit3(&s.pd, s.seq, s.spared, kSub, s.now);                   // hg_pend_exit3
            (void)n48_hg_pend_clear(&s.pd, N48_HG_PCLR_EXIT, 0u, kSub);                                   // hg_pend_exit
            break;
        case EV_D: (void)n48_hg_pend_clear(&s.pd, N48_HG_PCLR_DISARM, 0u, 0u); break;                     // hg_pend_disarm
        default:                                                                                          // a judged-frame poll
            if (s.spared && s.tF != kNever && s.now >= s.tF) (void)n48_hg_pend_retired(&s.pd, s.seq, N48_HG_LCLR_RET_POLL);
            break;
        }
    }
}

// hw_hg_copy_begin, in the kext's order
static void sim_copy_begin(Sim &s, uint32_t i)
{
    const HCopy &c = s.c[i];
    cb_lock(&s);
    uint64_t w = 0u;
    uint32_t pw = N48_HG_PW_GO;
    const uint32_t mayWait = n48_hg_copy_may_wait(c.np, c.npoison, c.noOverlay, c.scanFailed, c.noPlan);
    const uint32_t own = (c.np || c.npoison) ? 1u : 0u;
    if (s.pd.pending && !mayWait && (own || n48_hg_reg_overlaps(&s.st, kLo, kHi))) s.pd.unpatched++;
    if (s.pd.m3) pw = n48_hg_copy_wait3(&kOps3, &s, &s.st, &s.pd, c.thread, own, mayWait, kLo, kHi, &w);
    else
    if (s.pd.pending && mayWait) pw = n48_hg_copy_wait(&kOps1, &s, &s.st, &s.pd, c.thread, own, kLo, kHi, &w);
    s.bumped[i] = n48_hg_copy_begin(&s.st, (own || n48_hg_reg_overlaps(&s.st, kLo, kHi)) ? 1u : 0u);
    s.bumpAt[i] = s.now; s.look[i] = pw; s.waited[i] = w;
    if (s.bumped[i] && s.spared && s.now >= s.walkedAt && (s.tF == kNever || s.now < s.tF)) s.bumpUnderLive++;
    if (s.bumped[i] && s.done[EV_J] && !s.done[EV_W]) s.bumpInWindow++;
    cb_unlock(&s);
}

static void sim_init(Sim &s, uint32_t mode, uint32_t startup)
{
    std::memset(&s, 0, sizeof s);
    n48_hg_reg_add(&s.st, kLo + 0x2300u, kLo + 0x2400u);    // a substituted program inside WindowServer's heap: the copies overlap it
    s.mode = mode; s.startup = startup; s.gate = 1u;
    s.pd.m3 = mode == 3u ? 1u : 0u;
    s.seq = 4u; s.fenceOff = 0x3cba8f004ull; s.want = 0x50ae0001u; s.slot0 = 0u;
    for (uint32_t e = 0; e < EV_N; e++) s.t[e] = kNever;
    s.fenceLat = 400u; s.tF = kNever; s.walkedAt = kNever;
}
static void sim_add_copy(Sim &s, uint64_t at, uint32_t np, uint32_t npoison, uint64_t dur = 400u, uintptr_t thread = kCopier)
{
    HCopy &c = s.c[s.nc++];
    c = HCopy { at, dur, np, npoison, 0u, 0u, 0u, thread };
}
// Run to completion. Ties at equal times: the submit thread first.
static void sim_run(Sim &s)
{
    for (uint64_t guard = 0; guard < 2000000u; guard++) {
        uint64_t nextSub = kNever;
        for (uint32_t e = 0; e < EV_N; e++) if (!s.done[e] && s.t[e] != kNever && s.t[e] < nextSub) nextSub = s.t[e];
        uint64_t nextCopy = kNever;
        if (s.ci < s.nc) nextCopy = !s.started[s.ci] ? (s.c[s.ci].at > s.lastEnd ? s.c[s.ci].at : s.lastEnd) : s.endAt[s.ci];
        if (nextSub == kNever && nextCopy == kNever) break;
        if (nextSub != kNever && nextSub <= nextCopy) {
            if (s.now < nextSub) s.now = nextSub;
            const uint32_t before = [&] { uint32_t n = 0; for (uint32_t e = 0; e < EV_N; e++) n += s.done[e]; return n; }();
            sim_submit_due(s);
            const uint32_t after = [&] { uint32_t n = 0; for (uint32_t e = 0; e < EV_N; e++) n += s.done[e]; return n; }();
            if (after == before) {   // an event blocked by its predecessor: drop it (never happens in the scripted orders)
                for (uint32_t e = 0; e < EV_N; e++) if (!s.done[e] && s.t[e] == nextSub) { s.done[e] = 1u; break; }
            }
            continue;
        }
        if (s.now < nextCopy) s.now = nextCopy;
        const uint32_t i = s.ci;
        if (!s.started[i]) {
            s.started[i] = 1u;
            sim_copy_begin(s, i);
            s.endAt[i] = s.bumped[i] ? s.now + s.c[i].dur : s.now;
            continue;
        }
        if (s.bumped[i]) n48_hg_copy_end(&s.st, 1u, 1u, 0x400020000ull, 0x400029000ull, kLo, kHi, nullptr, 0u);
        s.ended[i] = 1u; s.lastEnd = s.now; s.ci++;
    }
}
static uint64_t L(uint64_t line, uint64_t base) { return (line - base) * kLineUs; }

// The replays. Every one returns the configured, run simulation.
static void rp_ac4(Sim &s, uint32_t mode, uint32_t startup = 1u)   // run11o seq 4
{
    const uint64_t b = 16090u;
    sim_init(s, mode, startup);
    s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
    s.t[EV_X] = L(16300, b);
    sim_add_copy(s, L(16216, b), 32u, 0u);
    sim_run(s);
}
static void rp_ac5(Sim &s, uint32_t mode)   // run11o seq 5: the copy after the judge, BEFORE the gate
{
    const uint64_t b = 19230u;
    sim_init(s, mode, 1u);
    s.seq = 5u; s.fenceOff = 0x3cba8f008ull; s.want = 0x50ae0002u;
    s.t[EV_FB] = L(19239, b); s.t[EV_J] = L(19240, b); s.t[EV_R] = L(19254, b); s.t[EV_P] = L(19276, b); s.t[EV_W] = L(19348, b);
    s.t[EV_X] = L(19349, b);
    sim_add_copy(s, L(19241, b), 18u, 0u);
    sim_run(s);
}
static void rp_z2(Sim &s, uint32_t mode)   // run11i seq 4
{
    const uint64_t b = 15700u;
    sim_init(s, mode, 1u);
    s.want = 0x76270001u;
    s.t[EV_FB] = L(15710, b); s.t[EV_J] = L(15711, b); s.t[EV_R] = L(15712, b); s.t[EV_P] = L(15847, b); s.t[EV_W] = L(15922, b);
    s.t[EV_X] = L(15923, b);
    sim_add_copy(s, L(15822, b), 32u, 0u);
    sim_run(s);
}
static void rp_ab3(Sim &s, uint32_t mode, uint64_t fenceLat)   // run11n seq 4: the copy arrives DURING the spared flight
{
    const uint64_t b = 16070u;
    sim_init(s, mode, 1u);
    s.want = 0xfff00001u; s.fenceLat = fenceLat;
    s.t[EV_FB] = L(16072, b); s.t[EV_J] = L(16073, b); s.t[EV_R] = L(16074, b); s.t[EV_P] = L(16192, b); s.t[EV_W] = L(16265, b);
    s.t[EV_X] = L(16266, b); s.t[EV_POLL0] = L(16980, b);
    sim_add_copy(s, L(16278, b), 30u, 0u);
    sim_run(s);
}
static void rp_ab2(Sim &s, uint32_t mode)   // run11m frame 20: the copy BEFORE the pass
{
    const uint64_t b = 18710u;
    sim_init(s, mode, 1u);
    s.t[EV_FB] = L(18732, b); s.t[EV_J] = L(18747, b); s.t[EV_R] = L(18753, b); s.t[EV_P] = L(18754, b); s.t[EV_W] = L(18760, b);
    s.t[EV_X] = L(18761, b);
    sim_add_copy(s, L(18715, b), 19u, 0u, 5000u);   // still writing at the judge (the log: "with a copy active, now 25, active 1")
    sim_run(s);
}
static void rp_runl(Sim &s, uint32_t mode)   // run10r seq 10 (T26's RUN L window, in us)
{
    sim_init(s, mode, 1u);
    s.seq = 10u;
    s.t[EV_FB] = 0u; s.t[EV_J] = 10u; s.t[EV_R] = 20u; s.t[EV_P] = 21u; s.t[EV_W] = 50u; s.t[EV_X] = 51u;
    sim_add_copy(s, 30u, 1u, 0u);
    sim_run(s);
}

// =============================================================================================================================
// T27a — THE REPLAYS
// =============================================================================================================================
static void test_replays()
{
    Sim &s = gS;
    char b[400];
    // AC seq 4: 72 OFF reproduces the log (the walk refused: heap-gen); M3 spares it and the copy bumps only after RET_COPIER.
    rp_ac4(s, 2u);
    expect("T27a AC seq 4 (run11o) 72 OFF (584): the copy bumps at once and the walk REFUSES (heap-gen) - the log's answer",
           s.committed && s.walked && s.walkWhy != N48_HG_OK && s.bumpAt[0] == L(16216, 16090) && s.pd.waits == 0u);
    rp_ac4(s, 3u);
    std::snprintf(b, sizeof b, "T27a AC seq 4 M3 (840): the copy WAITS (%llu us), the walk SPARES, EXIT 3 promotes to LIVE, the copy "
                  "bumps only after its own fence read (bump %llu us >= fence %llu us), RET_COPIER 1, nothing under the live frame",
                  (unsigned long long)s.waited[0], (unsigned long long)s.bumpAt[0], (unsigned long long)s.tF);
    expect(b, s.walked && s.walkWhy == N48_HG_OK && s.x3 == N48_HG_X3_PROMOTED && s.pd.live_sets == 1u && s.bumped[0] &&
              s.bumpAt[0] >= s.tF && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u && s.pd.lclr[N48_HG_LCLR_TIMEOUT_LIVE] == 0u &&
              s.pd.live == 0u && s.bumpUnderLive == 0u && s.bumpInWindow == 0u && s.pd.waits == 1u && s.pd.live_waits == 1u &&
              s.readsHeld == 0u && s.reads > 0u && s.held == 0 && s.pd.pending == 0u);
    rp_ac4(s, 1u);
    expect("T27a AC seq 4 M1 (328, 0.0.511): the walk spares, but the copy bumps RIGHT AFTER THE WALK, under the running frame (the "
           "F4-BAD gap: M1 must never be sent)",
           s.walked && s.walkWhy == N48_HG_OK && s.bumpUnderLive == 1u && s.bumpAt[0] < s.tF);
    // AC seq 5: the copy lands after the judge and BEFORE the gate: a tentative PRE (seq 0), stamped by the record, fenced by the push.
    rp_ac5(s, 2u);
    expect("T27a AC seq 5 (run11o) 72 OFF: the walk REFUSES (the log's heap-gen)", s.walked && s.walkWhy != N48_HG_OK);
    rp_ac5(s, 3u);
    expect("T27a AC seq 5 M3: the copy after the judge and before the gate WAITS; the walk SPARES; LIVE ends by RET_COPIER; bump after "
           "the fence", s.walked && s.walkWhy == N48_HG_OK && s.x3 == N48_HG_X3_PROMOTED && s.bumpAt[0] >= s.tF &&
           s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u && s.bumpUnderLive == 0u && s.pd.live_seq == 5u && s.pd.want == 0x50ae0002u &&
           s.pd.fence_off == 0x3cba8f008ull);
    // Z2
    rp_z2(s, 2u);
    expect("T27a Z2 seq 4 (run11i) 72 OFF: the walk REFUSES (the log's heap-gen)", s.walked && s.walkWhy != N48_HG_OK);
    rp_z2(s, 3u);
    expect("T27a Z2 seq 4 M3: SPARED, the copy bumps after the fence, RET_COPIER", s.walked && s.walkWhy == N48_HG_OK &&
           s.bumpAt[0] >= s.tF && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u && s.bumpUnderLive == 0u);
    // AB3: already spared; a copy arriving during LIVE waits until the fence; the answer is unchanged.
    rp_ab3(s, 2u, 5000u);
    const uint32_t offUnder = s.bumpUnderLive, offWhy = s.walkWhy;
    expect("T27a AB3 seq 4 (run11n) 72 OFF: SPARED; the copy at 16278 bumps at once, WHILE the flight runs (fence 5 ms later) - the "
           "status quo exposure", s.walked && offWhy == N48_HG_OK && offUnder == 1u);
    rp_ab3(s, 3u, 5000u);
    std::snprintf(b, sizeof b, "T27a AB3 M3: the same answer (SPARED); the copy arriving during LIVE waits %llu us until the fence "
                  "lands and bumps after it (RET_COPIER)", (unsigned long long)s.waited[0]);
    expect(b, s.walked && s.walkWhy == offWhy && s.bumpAt[0] >= s.tF && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u &&
              s.bumpUnderLive == 0u && s.waited[0] >= 4000u && s.waited[0] < 6100u);
    rp_ab3(s, 3u, 200u);
    expect("T27a AB3 M3, fence already landed when the copy arrives (400 us < 600 us): one read, RET_COPIER, no delay at all",
           s.walkWhy == N48_HG_OK && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u && s.delays == 0u && s.waited[0] == 0u);
    // AB3 with no fence read possible until the poll: the POLL retires it (the copier's own read disabled: PLANT 3's shape, modelled)
    // is covered by the direct test below; here the poll after the copy is a no-op (LIVE already ended) - nothing is counted twice.
    rp_ab3(s, 3u, 5000u);
    expect("T27a AB3 M3: the judged-frame poll at 16980 finds LIVE already ended by the copier (RET_POLL 0, no double count)",
           s.pd.lclr[N48_HG_LCLR_RET_POLL] == 0u && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u);
    // AB2: the copy precedes the pass; no PRE can be set; M3 changes nothing.
    rp_ab2(s, 2u);
    const uint32_t ab2OffJudge = s.judgeWhy;
    expect("T27a AB2 frame 20 (run11m) 72 OFF: the judge refuses copy-in-progress; no flag", ab2OffJudge == N48_HG_IN_PROGRESS &&
           s.pd.sets == 0u && !s.committed);
    rp_ab2(s, 3u);
    expect("T27a AB2 M3: NO WAIT and STILL copy-in-progress (the copy started before the pass: M3 cannot help, as the design says)",
           s.judgeWhy == N48_HG_IN_PROGRESS && s.pd.sets == 0u && s.pd.waits == 0u && s.waited[0] == 0u && !s.committed &&
           s.pd.live_sets == 0u);
    // RUN L seq 10
    rp_runl(s, 2u);
    expect("T27a RUN L seq 10 (run10r) 72 OFF: REFUSED", s.walked && s.walkWhy != N48_HG_OK);
    rp_runl(s, 3u);
    expect("T27a RUN L seq 10 M3: SPARED, bump after the fence", s.walked && s.walkWhy == N48_HG_OK && s.bumpAt[0] >= s.tF &&
           s.bumpUnderLive == 0u && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u);
}

// =============================================================================================================================
// T27b — THE OTHER PATHS: fence-less, NOPed, SELF on LIVE, disarm, scope, stale slot, TIMEOUT_LIVE, the polls and the age
// =============================================================================================================================
static void test_paths()
{
    Sim &s = gS;
    // NOPed at the walk (an unpatched copy in the window bumps at once, the walk refuses) -> EXIT 3 clears; a later copy never waits.
    {
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b);
        sim_add_copy(s, L(16216, b), 31u, 1u);          // one unpatchable program: not fully patched (no PRE wait, 0.0.511)
        sim_add_copy(s, L(16400, b), 32u, 0u);          // a later fully patched copy
        sim_run(s);
        expect("T27b a NOT fully patched copy in the window bumps at once (unpatched counted), the walk REFUSES, EXIT 3 CLEARS: the "
               "next copy does not wait at all (no full-bound wait on a NOPed flight)",
               s.walked && s.walkWhy != N48_HG_OK && s.x3 == N48_HG_X3_CLEARED && s.pd.unpatched == 1u && s.pd.live_sets == 0u &&
               s.waited[1] == 0u && s.look[1] == N48_HG_PW_GO && s.pd.lclr[N48_HG_LCLR_TIMEOUT_LIVE] == 0u && s.pd.pending == 0u);
    }
    // Fence-less (the push stamped want 0): cleared at EXIT 3 and counted; never LIVE.
    {
        rp_ac4(s, 3u);   // warm-up for the stamp's shape, then the fence-less variant
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.want = 0u;
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b);
        sim_add_copy(s, L(16216, b), 32u, 0u);
        sim_add_copy(s, L(16400, b), 32u, 0u);
        sim_run(s);
        expect("T27b a SPARED FENCE-LESS flight: EXIT 3 clears it and counts `fenceless` (0.0.511's behaviour), never LIVE; the later "
               "copy does not wait", s.walkWhy == N48_HG_OK && s.x3 == N48_HG_X3_FENCELESS && s.pd.fenceless == 1u &&
               s.pd.live_sets == 0u && s.pd.live == 0u && s.waited[1] == 0u);
    }
    // SELF applies to PRE only: a copy on the committing thread during LIVE waits for the fence.
    {
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.fenceLat = 3000u;
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b);
        sim_add_copy(s, L(16301, b), 32u, 0u, 400u, kSub);   // the committing thread's own copy, during LIVE
        sim_run(s);
        expect("T27b SELF is PRE's only: a copy on the COMMITTING thread during LIVE still waits for the fence (bump after it)",
               s.x3 == N48_HG_X3_PROMOTED && s.bumpAt[0] >= s.tF && s.bumpUnderLive == 0u && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u);
    }
    // A copy on the committing thread during PRE: SELF (never waits for itself), as 0.0.511.
    {
        rp_ac4(s, 3u);
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b);
        sim_add_copy(s, L(16216, b), 32u, 0u, 400u, kSub);
        sim_run(s);
        expect("T27b a copy on the committing thread during PRE never waits for itself (SELF, counted); the walk then refuses",
               s.look[0] == N48_HG_PW_SELF && s.pd.self_skips == 1u && s.waited[0] == 0u && s.walkWhy != N48_HG_OK);
    }
    // A disarm while LIVE: LIVE stays (only the fence, the polls, the expiry, the timeout or the age end it).
    {
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.fenceLat = 3000u;
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b); s.t[EV_D] = L(16302, b);
        sim_add_copy(s, L(16310, b), 32u, 0u);
        sim_run(s);
        expect("T27b a DISARM while LIVE leaves LIVE set: the copy after it still waits for the fence",
               s.x3 == N48_HG_X3_PROMOTED && s.done[EV_D] && s.bumpAt[0] >= s.tF && s.bumpUnderLive == 0u &&
               s.pd.clr[N48_HG_PCLR_DISARM] == 0u && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u);
        n48_hg_pend p {}; p.m3 = 1u; p.startup = 1u;
        n48_hg_pend_set(&p, kSub, 0u); n48_hg_pend_record(&p, 4u, kSub); (void)n48_hg_pend_fence(&p, 4u, kSub, 0x1000u, 0x77u);
        (void)n48_hg_pend_exit3(&p, 4u, 1u, kSub, 5u);
        const uint32_t dis = n48_hg_pend_clear(&p, N48_HG_PCLR_DISARM, 0u, 0u);
        const uint32_t ex = n48_hg_pend_clear(&p, N48_HG_PCLR_EXIT, 0u, kSub);
        expect("T27b direct: after a promotion, a disarm, an exit (1/2/4) and a walk clear nothing LIVE",
               p.live == 1u && dis == 0u && ex == 0u && n48_hg_pend_clear(&p, N48_HG_PCLR_WALK, 4u, kSub) == 0u && p.live_seq == 4u);
    }
    // The scope: after CONTINUOUS START (startup 0) M3 sets nothing; the same replay then equals 72 OFF.
    {
        rp_ac4(s, 3u, 0u);
        const uint32_t sets = s.pd.sets, skips = s.pd.scope_skips, why = s.walkWhy; const uint64_t ba = s.bumpAt[0];
        rp_ac4(s, 2u);
        expect("T27b after CONTINUOUS START M3 sets NOTHING (scope skip counted) and AC seq 4 plays exactly as 72 OFF",
               sets == 0u && skips == 1u && why == s.walkWhy && ba == s.bumpAt[0]);
    }
    // A stale value in the owned slot (an earlier arm's epoch): only `val == want` retires.
    {
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.slot0 = 0x76270001u;   // Z2's epoch in the same slot 1 (the slots are reused across arms)
        s.fenceLat = 2000u;
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b);
        sim_add_copy(s, L(16216, b), 32u, 0u);
        sim_run(s);
        expect("T27b a STALE non-zero value in the owned slot never retires LIVE: the copy bumps only after the exact want lands",
               s.x3 == N48_HG_X3_PROMOTED && s.bumpAt[0] >= s.tF && s.bumpUnderLive == 0u && s.pd.lclr[N48_HG_LCLR_RET_COPIER] == 1u);
    }
    // TIMEOUT_LIVE: the fence never lands within the bound (no poll either): a FINDING, counted, LIVE cleared, the copy bumps.
    {
        rp_ac4(s, 3u);
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.fenceLat = 250000u;
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16300, b);
        sim_add_copy(s, L(16216, b), 32u, 0u);
        sim_run(s);
        expect("T27b the fence does not land within 100 ms: TIMEOUT_LIVE (a finding) counted, LIVE cleared, the copy bumps (bounded "
               "at 100 ms + one step)", s.look[0] == N48_HG_PW_TIMEOUT_LIVE && s.pd.lclr[N48_HG_LCLR_TIMEOUT_LIVE] == 1u &&
               s.pd.live == 0u && s.bumped[0] && s.waited[0] >= N48_HG_WAIT_US && s.waited[0] < N48_HG_WAIT_US + 1100u &&
               s.bumpUnderLive == 1u && s.pd.timeouts == 0u);
    }
    // FIX PASS (the HIGH review's SHOULD-FIX): a PRE copier arriving before the walk; the walk SPARES; EXIT 3 comes > 100 ms later.
    // The PRE timeout then bumps UNDER the running frame: TIMEOUT_LIVE-class, counted pre_timeout_after_walk, never a plain PRE timeout.
    {
        const uint64_t b = 16090u;
        sim_init(s, 3u, 1u);
        s.fenceLat = 300000u;
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
        s.t[EV_X] = L(16299, b) + 150000u;
        sim_add_copy(s, L(16216, b), 32u, 0u);
        sim_run(s);
        expect("T27b FIX a PRE copier whose walk SPARED times out before EXIT 3: TIMEOUT_LIVE-class, pre_timeout_after_walk 1, NOT a "
               "plain PRE timeout (timeouts 0), and it is the bump under the running frame the finding names",
               s.walkWhy == N48_HG_OK && s.look[0] == N48_HG_PW_TIMEOUT_LIVE && s.pd.pre_timeout_after_walk == 1u &&
               s.pd.timeouts == 0u && s.pd.pre_walk_ok == 1u && s.bumpUnderLive == 1u);
        // the same copier, but the walk comes only after the bound: a PRE timeout before the walk stays fail-closed
        sim_init(s, 3u, 1u);
        s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b);
        s.t[EV_W] = L(16299, b) + 150000u; s.t[EV_X] = L(16299, b) + 150001u;
        sim_add_copy(s, L(16216, b), 32u, 0u);
        sim_run(s);
        expect("T27b FIX a PRE timeout BEFORE the walk stays a plain fail-closed TIMEOUT (timeouts 1, after-walk 0; the late walk refuses)",
               s.look[0] == N48_HG_PW_TIMEOUT && s.pd.timeouts == 1u && s.pd.pre_timeout_after_walk == 0u && s.walkWhy != N48_HG_OK);
    }
    // The ring's own retirement paths and the age: direct.
    {
        n48_hg_pend p {}; p.m3 = 1u; p.startup = 1u;
        n48_hg_pend_set(&p, kSub, 0u); n48_hg_pend_record(&p, 4u, kSub); (void)n48_hg_pend_fence(&p, 4u, kSub, 0x1000u, 0x77u);
        (void)n48_hg_pend_exit3(&p, 4u, 1u, kSub, 5u);
        const uint32_t other = n48_hg_pend_retired(&p, 3u, N48_HG_LCLR_RET_POLL);
        const uint32_t mine = n48_hg_pend_retired(&p, 4u, N48_HG_LCLR_RET_POLL);
        expect("T27b RET_POLL: only the LIVE seq's own retirement ends it (an older seq's does not)", other == 0u && mine == 1u &&
               p.live == 0u && p.lclr[N48_HG_LCLR_RET_POLL] == 1u);
        n48_hg_pend_set(&p, kSub, 10u); n48_hg_pend_record(&p, 6u, kSub); (void)n48_hg_pend_fence(&p, 6u, kSub, 0x1004u, 0x78u);
        (void)n48_hg_pend_exit3(&p, 6u, 1u, kSub, 11u);
        n48_hg_pend_set(&p, kSub, 12u); n48_hg_pend_record(&p, 7u, kSub); (void)n48_hg_pend_fence(&p, 7u, kSub, 0x1008u, 0x79u);
        (void)n48_hg_pend_exit3(&p, 7u, 1u, kSub, 13u);
        expect("T27b REPLACED: a newer spared flight replaces LIVE (the newest is held; counted)", p.live == 1u && p.live_seq == 7u &&
               p.want == 0x79u && p.fence_off == 0x1008u && p.lclr[N48_HG_LCLR_REPLACED] == 1u && p.live_sets == 3u);
        expect("T27b RET_EXPIRY and FLIGHT_EXPIRED end LIVE by their own reasons",
               n48_hg_pend_retired(&p, 7u, N48_HG_LCLR_RET_EXPIRY) == 1u && p.lclr[N48_HG_LCLR_RET_EXPIRY] == 1u && [&] {
                   n48_hg_pend_set(&p, kSub, 20u); n48_hg_pend_record(&p, 8u, kSub); (void)n48_hg_pend_fence(&p, 8u, kSub, 0x100cu, 0x7au);
                   (void)n48_hg_pend_exit3(&p, 8u, 1u, kSub, 21u);
                   return n48_hg_pend_retired(&p, 8u, N48_HG_LCLR_FLIGHT_EXPIRED) == 1u && p.lclr[N48_HG_LCLR_FLIGHT_EXPIRED] == 1u;
               }());
        // the age: LIVE set at 21 us; a copier looking 2 s later clears it by age without reading
        n48_hg_pend_set(&p, kSub, 30u); n48_hg_pend_record(&p, 9u, kSub); (void)n48_hg_pend_fence(&p, 9u, kSub, 0x1010u, 0x7bu);
        (void)n48_hg_pend_exit3(&p, 9u, 1u, kSub, 31u);
        Sim &q = gS; std::memset(&q, 0, sizeof q); q.fenceOff = 0x1010u; q.want = 0x7bu; q.tF = kNever;
        n48_hg_state st {}; n48_hg_reg_add(&st, kLo, kLo + 0x100u);
        q.now = 31u + N48_HG_LIVE_AGE_US; q.held = 1;
        uint64_t w = 0u;
        const uint32_t d = n48_hg_copy_wait3(&kOps3, &q, &st, &p, kCopier, 1u, 1u, kLo, kHi, &w);
        expect("T27b LIVE_AGE: a LIVE 2 s old (== kKsFlightUs) is cleared by the next copier without a read, and it goes on",
               d == N48_HG_PW_GO && p.live == 0u && p.lclr[N48_HG_LCLR_LIVE_AGE] == 1u && q.reads == 0u && q.delays == 0u);
    }
    // The setter and the switch values.
    {
        uint32_t f = 7u;
        const int r1 = n48_hg72_set(1u, &f); const uint32_t v1 = f;
        const int r2 = n48_hg72_set(2u, &f); const uint32_t v2 = f;
        const int r3 = n48_hg72_set(3u, &f); const uint32_t v3 = f;
        const int r4 = n48_hg72_set(4u, &f); const int r0 = n48_hg72_set(0u, &f);
        expect("T27b 72's setter: M1 -> 1 (328), M2 -> 0 (584), M3 -> 3 (840); M0/M4 refused unchanged; 840 == 72 | 3 << 8",
               r1 == 1 && v1 == 1u && r2 == 1 && v2 == 0u && r3 == 1 && v3 == N48_HG72_M3 && r4 == 0 && r0 == 0 && f == N48_HG72_M3 &&
               (72u | 3u << 8) == 840u && (72u | 1u << 8) == 328u && (72u | 2u << 8) == 584u);
    }
    // M1/M2 identity at the pure level: nothing M3 does is reachable with m3 0.
    {
        n48_hg_pend p {};
        n48_hg_pend_set(&p, kSub, 0u); n48_hg_pend_record(&p, 4u, kSub);
        const uint32_t f = n48_hg_pend_fence(&p, 4u, kSub, 0x1000u, 0x77u);
        const uint32_t x = n48_hg_pend_exit3(&p, 4u, 1u, kSub, 5u);
        const uint32_t w = n48_hg_pend_clear(&p, N48_HG_PCLR_WALK, 4u, kSub);
        expect("T27b m3 0 (M1/M2): no fence stamp, no EXIT 3 decision, and the WALK clears PRE exactly as 0.0.511",
               f == 0u && x == N48_HG_X3_NONE && w == 1u && p.live == 0u && p.pre_want == 0u && p.clr[N48_HG_PCLR_WALK] == 1u);
    }
}

// =============================================================================================================================
// T27c — THE SWEEPS
// =============================================================================================================================
static void test_sweeps()
{
    Sim on {}, off {};
    const uint64_t copyAts[] = { 3u, 15u, 25u, 30u, 45u, 55u, 60u, 500u, 5000u, 120000u };
    const uint64_t walks[] = { 50u, 2000u, 90000u, 150000u };
    const uint64_t lats[] = { 0u, 300u, 20000u, 99000u, 250000u, kNever };
    const uint32_t kinds[][2] = { { 1u, 0u }, { 0u, 1u }, { 3u, 1u } };   // fully patched; unpatchable only; mixed
    uint32_t runs = 0u, worse = 0u, better = 0u, underNoTimeout = 0u, timeoutsSeen = 0u, heldReads = 0u, postStartDiff = 0u;
    uint32_t pollRet = 0u;
    for (uint64_t ca : copyAts)
        for (uint64_t tw : walks)
            for (uint64_t lat : lats)
                for (const auto &k : kinds)
                    for (uint32_t second = 0u; second < 2u; second++) {
                        auto setup = [&](Sim &s, uint32_t mode, uint32_t startup) {
                            sim_init(s, mode, startup);
                            s.fenceLat = lat;
                            s.t[EV_FB] = 0u; s.t[EV_J] = 10u; s.t[EV_R] = 20u; s.t[EV_P] = 21u; s.t[EV_W] = tw; s.t[EV_X] = tw + 1u;
                            s.t[EV_POLL0] = tw + 30000u; s.t[EV_POLL0 + 1] = tw + 400000u;
                            sim_add_copy(s, ca, k[0], k[1], 400u);
                            if (second) sim_add_copy(s, tw + 5u, 2u, 0u, 400u);
                            sim_run(s);
                        };
                        setup(on, 3u, 1u);
                        setup(off, 2u, 1u);
                        runs++;
                        const uint32_t sOn = on.walked && on.walkWhy == N48_HG_OK, sOff = off.walked && off.walkWhy == N48_HG_OK;
                        if (sOff && !sOn) worse++;
                        if (!sOff && sOn) better++;
                        if (on.bumpUnderLive && on.pd.lclr[N48_HG_LCLR_TIMEOUT_LIVE] == 0u && on.pd.lclr[N48_HG_LCLR_LIVE_AGE] == 0u)
                            underNoTimeout++;
                        if (on.pd.lclr[N48_HG_LCLR_TIMEOUT_LIVE]) timeoutsSeen++;
                        if (on.pd.lclr[N48_HG_LCLR_RET_POLL]) pollRet++;
                        heldReads += on.readsHeld;
                        // post-START: M3 with startup 0 equals M2 in every observable
                        Sim ps {}; setup(ps, 3u, 0u);
                        if (ps.walked != off.walked || ps.walkWhy != off.walkWhy || ps.pd.sets != 0u || ps.pd.waits != off.pd.waits ||
                            ps.bumpAt[0] != off.bumpAt[0] || ps.bumpAt[1] != off.bumpAt[1] || ps.st.gen != off.st.gen)
                            postStartDiff++;
                    }
    char b[420];
    std::snprintf(b, sizeof b, "T27c SWEEP over %u runs (copy arrival x walk x fence latency x copy kind x a second copy): the M3 walk "
                  "never refuses what 72 OFF spares (%u); it spares %u that 72 OFF refused", runs, worse, better);
    expect(b, runs >= 400u && worse == 0u && better > 0u);
    std::snprintf(b, sizeof b, "T27c no bump while LIVE unless a timeout (or the 2 s age) ended it (%u); TIMEOUT_LIVE exercised (%u "
                  "runs, each a counted finding); RET_POLL exercised (%u); every fence read with the lock released (%u held)",
                  underNoTimeout, timeoutsSeen, pollRet, heldReads);
    expect(b, underNoTimeout == 0u && timeoutsSeen > 0u && heldReads == 0u);
    std::snprintf(b, sizeof b, "T27c any post-START sequence equals 72 OFF (M2) in walk answer, bump times, waits and generation (%u "
                  "differ)", postStartDiff);
    expect(b, postStartDiff == 0u);
    // RET_POLL: the copier's read disabled (no read_fence), the poll ends LIVE
    {
        Sim &s = gS;
        sim_init(s, 3u, 1u);
        s.t[EV_FB] = 0u; s.t[EV_J] = 10u; s.t[EV_R] = 20u; s.t[EV_P] = 21u; s.t[EV_W] = 50u; s.t[EV_X] = 51u;
        s.fenceLat = 100u; s.t[EV_POLL0] = 80u;
        sim_add_copy(s, 30u, 1u, 0u);
        sim_run(s);
        expect("T27c the judged-frame poll retires LIVE when it sees the fence first (RET_POLL; the copier then goes on)",
               s.walkWhy == N48_HG_OK && (s.pd.lclr[N48_HG_LCLR_RET_POLL] + s.pd.lclr[N48_HG_LCLR_RET_COPIER]) == 1u &&
               s.bumpUnderLive == 0u);
    }
}

// =============================================================================================================================
// T27d — ITEM 11: the fill-set's walk-NOPed mark and the ledger's "un-fed"
// =============================================================================================================================
static void test_item11()
{
    n48_fs f {}; f.on = 1u; n48_fs_open(&f);
    n48_fs_nop x {}; n48_fs_nop_open(&x);
    (void)n48_fs_commit(&f, f.member_va[0], 5u);
    (void)n48_fs_commit(&f, f.member_va[1], 4u);
    const uint32_t a = n48_fs_note_nopped(&f, &x, 4u), again = n48_fs_note_nopped(&f, &x, 4u), none = n48_fs_note_nopped(&f, &x, 9u);
    expect("T27d 11(a) the walk-NOPed committed member (seq 4) is marked once; an unknown seq marks nothing; committed stays committed",
           a == 1u && again == 0u && none == 0u && n48_fs_is_nopped(&f, &x, 1u) == 1u && n48_fs_is_nopped(&f, &x, 0u) == 0u &&
           f.committed_n == 2u && f.member_committed[1] == 1u && f.retired_n == 0u);
    char line[1024];
    const int n = std::snprintf(line, sizeof line, N48_FILLSET_NOP_FMT, (unsigned long long)f.member_va[0], "-",
                                (uint32_t)f.member_seq[0], "", (unsigned long long)f.member_va[1], "committed",
                                (uint32_t)f.member_seq[1], " (walk-NOPed)");
    expect("T27d 11(a) the line reads \"committed seq 4 (walk-NOPed)\"", n > 0 && std::strstr(line, "committed seq 4 (walk-NOPed)"));
    const int w = std::snprintf(line, sizeof line, N48_FILLSET_NOP_FMT, 0xffffffffffffffffull, "committed", 0xffffffffu, " (walk-NOPed)",
                                0xffffffffffffffffull, "committed", 0xffffffffu, " (walk-NOPed)");
    std::snprintf(line + 600, 200, "T27d 11(a) the walk-NOPed line at its widest: %d body bytes (<= 491)", w);
    expect(line + 600, w > 0 && w <= 491);
    n48_fs_open(&f); n48_fs_nop_open(&x);
    (void)n48_fs_commit(&f, f.member_va[1], 4u);
    expect("T27d 11(a) a new window's open resets the marks (a reused seq is not marked by an older window's NOP)",
           n48_fs_is_nopped(&f, &x, 1u) == 0u);
    n48_fs g {}; n48_fs_open(&g); n48_fs_nop y {};
    (void)n48_fs_commit(&g, g.member_va[0], 4u);
    expect("T27d 11(a) switch 33 OFF marks nothing", n48_fs_note_nopped(&g, &y, 4u) == 0u);
    // 11(b)
    static n48_dl l;
    std::memset(&l, 0, sizeof l);
    l.feeds = 3u; l.fedOk = 2u; l.added = 2u; l.unfeedRemoved = 2u;
    const uint32_t un = n48_dl_empty_why(&l);
    l.unfeedRemoved = 0u; const uint32_t unk = n48_dl_empty_why(&l);
    l.unfeedRemoved = 2u; l.clears = 1u; const uint32_t cl = n48_dl_empty_why(&l);
    l.clears = 0u; l.unmapDropped = 1u; const uint32_t um = n48_dl_empty_why(&l);
    expect("T27d 11(b) an empty ledger emptied only by the un-feed reads \"un-fed\" (was \"?\"); CLEARED and unmapped still win; with no "
           "un-feed it stays \"?\"", un == N48_DL_WHY_UNFED && std::strcmp(n48_dl_why_name(un), "un-fed") == 0 &&
           unk == N48_DL_WHY_UNKNOWN && cl == N48_DL_WHY_CLEARED && um == N48_DL_WHY_UNMAPPED && N48_DL_WHY_UNFED == N48_DL_WHY_WITHDRAWN + 1u);
}

// =============================================================================================================================
// T27e — THE LINES (<= 491 body bytes at their widest) and THE KEXT's WIRING (ordering, not literal presence alone)
// =============================================================================================================================
static bool has(const std::string &t, const char *s) { return t.find(s) != std::string::npos; }
static size_t count_of(const std::string &t, const char *s)
{
    size_t n = 0, at = 0; const size_t k = std::strlen(s);
    while ((at = t.find(s, at)) != std::string::npos) { n++; at += k; }
    return n;
}
static std::string fn_text(const std::string &src, const char *head)
{
    const size_t a = src.find(head);
    if (a == std::string::npos) return std::string();
    size_t i = src.find('{', a + std::strlen(head) - 1u);
    if (i == std::string::npos) return std::string();
    int depth = 0;
    for (size_t j = i; j < src.size(); j++) {
        if (src[j] == '{') depth++;
        else if (src[j] == '}') { if (--depth == 0) return src.substr(a, j - a + 1u); }
    }
    return std::string();
}
static bool order(const std::string &t, std::initializer_list<const char *> steps)
{
    size_t prev = 0; bool first = true;
    for (const char *st : steps) {
        const size_t at = t.find(st, first ? 0 : prev);
        if (at == std::string::npos || count_of(t, st) != 1u) return false;
        prev = at + 1; first = false;
    }
    return true;
}

// The kext-side format strings (not in a header): pinned below as literally present in the kext, measured here.
static const char kFmtTimeout[] = "heapwait72: the copy over VRAM [%#llx,%#llx) %s after %llu us waiting for %s (line %u of at most 16)";
static const char kFmtPromote[] = "heapwait72: M3 token seq %u SPARED in the start-up window - held LIVE: an overlapping heap copy now waits (at most "
                                  "%u us) until this flight's own fence at vram+%#llx reads %#x (line %u of at most 16)";
static const char kFmtFenceless[] = "heapwait72: M3 token seq %u SPARED with NO FENCE - cleared at once (fence-less: 0.0.511's behaviour, a copy may "
                                    "land under the running frame) (line %u of at most 16)";

static void test_lines_and_wiring(const char *ahhPath)
{
    char line[1400];
    n48_hg_pend big; std::memset(&big, 0xff, sizeof big);
    const int n3 = std::snprintf(line, sizeof line, N48_HG_PEND_FMT3, N48_HG_PEND_ARGS3(&big));
    const int n1 = std::snprintf(line, sizeof line, N48_HG_PEND_FMT, N48_HG_PEND_ARGS(3u, " - `gfxneuter 72` REFUSED: a continuous arm stands, unchanged", &big, ~0ull));
    const int nt = std::snprintf(line, sizeof line, kFmtTimeout, ~0ull, ~0ull, "TIMED OUT AFTER A SPARE (a finding)", ~0ull,
                                 "a spared start-up fill (its fence, or EXIT 3 after its walk) - it bumps now, while that flight may still run", 0xffffffffu);
    const int nt0 = std::snprintf(line, sizeof line, kFmtTimeout, ~0ull, ~0ull, "found the flag EXPIRED (a leak)", ~0ull,
                                  "a committed frame's walk - it bumps now, and that walk refuses (fail-closed, as without 72)", 0xffffffffu);
    const int np = std::snprintf(line, sizeof line, kFmtPromote, 0xffffffffu, 0xffffffffu, ~0ull, 0xffffffffu, 0xffffffffu);
    const int nf = std::snprintf(line, sizeof line, kFmtFenceless, 0xffffffffu, 0xffffffffu);
    std::snprintf(line, sizeof line, "T27e the heapwait72 lines at their widest fit n48log's 491-byte body: FMT3 %d, FMT (M3 word) %d, "
                  "timeout %d / %d, promote %d, fence-less %d", n3, n1, nt, nt0, np, nf);
    expect(line, n3 > 0 && n3 <= 491 && n1 <= 491 && nt <= 491 && nt0 <= 491 && np <= 491 && nf <= 491);
    // the old timeout line's text is reproduced exactly for M1/M2's two cases
    std::snprintf(line, sizeof line, kFmtTimeout, 0x10020000ull, 0x10029000ull, "TIMED OUT", 100020ull,
                  "a committed frame's walk - it bumps now, and that walk refuses (fail-closed, as without 72)", 1u);
    expect("T27e the 0.0.511 timeout line reads byte for byte as before for M1/M2",
           std::strcmp(line, "heapwait72: the copy over VRAM [0x10020000,0x10029000) TIMED OUT after 100020 us waiting for a committed "
                             "frame's walk - it bumps now, and that walk refuses (fail-closed, as without 72) (line 1 of at most 16)") == 0);

    std::ifstream in(ahhPath ? ahhPath : "");
    std::stringstream ss; ss << in.rdbuf();
    const std::string ahh = ss.str();
    expect("T27e the kext source (AppleHardwareHook.cpp) was given and read", !ahh.empty());
    if (ahh.empty()) return;
    expect("T27e the kext prints exactly these formats", has(ahh, "\"heapwait72: the copy over VRAM [%#llx,%#llx) %s after %llu us waiting for %s (line %u of at most 16)\"") &&
           has(ahh, "\"heapwait72: M3 token seq %u SPARED in the start-up window - held LIVE: an overlapping heap copy now waits (at most \"\n                  \"%u us) until this flight's own fence at vram+%#llx reads %#x (line %u of at most 16)\"") &&
           has(ahh, "\"heapwait72: M3 token seq %u SPARED with NO FENCE - cleared at once (fence-less: 0.0.511's behaviour, a copy may \"\n                  \"land under the running frame) (line %u of at most 16)\"") &&
           has(ahh, "HWLOG(N48_HG_PEND_FMT3, N48_HG_PEND_ARGS3(&gHgPend));"));
    const std::string cb = fn_text(ahh, "uint32_t hw_hg_copy_begin(uint64_t vramLo, uint64_t vramHi, const n48_hg_patch *p, uint32_t np, uint32_t npoison, uint64_t *regSeqOut,");
    const std::string wm = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_copy_wait_m3(");
    const std::string cj = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_commit_judge(uint32_t wouldCommit)");
    const std::string x3 = fn_text(ahh, "static __attribute__((noinline)) void hg_pend_exit3(uint32_t seq, uint32_t exWhy)");
    const std::string fe = fn_text(ahh, "static __attribute__((noinline)) void hg_pend_fence(uint32_t seq, uint64_t vramOff, uint32_t want)");
    const std::string rt = fn_text(ahh, "static __attribute__((noinline)) void hg_pend_retired(uint32_t seq, uint32_t why)");
    const std::string fx = fn_text(ahh, "static __attribute__((noinline)) void hg_pend_flight_expired()");
    const std::string hk = fn_text(ahh, "static uint64_t hook_gfxCommitIB(void *self, void *info) {");
    const std::string ks = fn_text(ahh, "__attribute__((noinline)) static void ks_eop_at_expiry(");
    const std::string fr = fn_text(ahh, "static void fillset_report_line(const char *how)");
    expect("T27e the functions are found", !cb.empty() && !wm.empty() && !cj.empty() && !x3.empty() && !fe.empty() && !rt.empty() &&
           !fx.empty() && !hk.empty() && !ks.empty() && !fr.empty());
    // item 8: the copier - M3's wait in the one gHgLock section, before the bump, no return between; 0.0.511's wait otherwise
    const size_t w0 = cb.find("if (gHgPend.m3) pw = hg_copy_wait_m3("), w1 = cb.find("const uint32_t bumped = n48_hg_copy_begin(&gHg, overlaps);");
    expect("T27e item 8: hw_hg_copy_begin: lock -> unpatched count -> (m3) hg_copy_wait_m3, else 0.0.511's wait -> overlap -> bump -> "
           "unlock; nothing returns between the wait and the bump",
           order(cb, { "IOLockLock(l);", "gHgPend.unpatched++;", "if (gHgPend.m3) pw = hg_copy_wait_m3(l, (np || npoison) ? 1u : 0u, mayWait, vramLo, vramHi, &pwUs);",
                       "    else\n    if (gHgPend.pending && mayWait)\n        pw = n48_hg_copy_wait(&kHgWaitOps, l,",
                       "const uint32_t bumped = n48_hg_copy_begin(&gHg, overlaps);", "IOLockUnlock(l);" }) &&
           w0 != std::string::npos && w1 != std::string::npos && cb.substr(w0, w1 - w0).find("return") == std::string::npos);
    expect("T27e item 8: the M3 wait uses the ops WITH the fence read, and the read is navi48_vram_read_mm of one dword (never gXdLock)",
           has(wm, "return n48_hg_copy_wait3(&kHgWaitOps3, l, &gHg, &gHgPend, reinterpret_cast<uintptr_t>(current_thread()), own, mayWait, vramLo,") &&
           has(ahh, "static const n48_hg_wait_ops kHgWaitOps3 = { &hg_wait_now, &hg_wait_lock, &hg_wait_unlock, &hg_wait_delay, &hg_wait_read_fence };") &&
           has(ahh, "static uint32_t hg_wait_read_fence(void *, uint64_t off, uint32_t *val) { return navi48_vram_read_mm(off, val, 1) ? 1u : 0u; }") &&
           !has(wm, "gXdLock"));
    // item 3: the scope, latched in the judge's own gHgLock section, before the set
    expect("T27e item 3: hg_commit_judge latches `startup` (pre-plane window) under gHgLock, after the judge, before the flag's set",
           order(cj, { "IOLockLock(gHgLock);", "const uint32_t why = n48_hg_judge(&gHg, &gHgFrame, gXdC.judged + 1u);",
                       "if (gHgPend.m3) gHgPend.startup = hg_startup_now();", "if (why == N48_HG_OK && wouldCommit && gHgPendLatch)",
                       "IOLockUnlock(gHgLock);" }) &&
           has(ahh, "static uint32_t hg_startup_now() { return (gXdShot.cont && gXdShot.cont_start_us == 0ull) ? 1u : 0u; }"));
    // item 4: the fence stamp right after the push (after the spill tag, which its own test keeps within 600 bytes of the push)
    const size_t pu = ahh.find("const uint32_t frPushed = n48_fr_push(&gKsRing, gXdCmToken.seq, frAtUs, frOrdinal, frVramOff, frWant, &frIdx);");
    const size_t st = ahh.find("if (frPushed && hg_pend_m3()) hg_pend_fence(gXdCmToken.seq, frVramOff, frWant);");
    const size_t nf2 = ahh.find("if (!frPushed) {", pu == std::string::npos ? 0 : pu);
    expect("T27e item 4: the fence stamp follows the push (pushed only, this seq, its slot and want) in the same block",
           pu != std::string::npos && st != std::string::npos && pu < st && st < nf2 && count_of(ahh, "hg_pend_fence(gXdCmToken.seq") == 1u &&
           has(fe, "(void)n48_hg_pend_fence(&gHgPend, seq, reinterpret_cast<uintptr_t>(current_thread()), vramOff, want);"));
    // item 5: EXIT 3 - after the record's teardown (exWhy known), before 0.0.510's hg_pend_exit
    expect("T27e item 5: EXIT 3's decision runs after Apple's original and the teardown (exWhy read), before hg_pend_exit, once",
           order(hk, { "const uint64_t rvT = orig(self, info);", "exWhy = gCmxLastWhy; exAt = gCmxSparedAt;",
                       "if (hg_pend_m3()) hg_pend_exit3(seq, exWhy);", "// build 0.0.510 (switch 72) EXIT 3 of 4",
                       "            hg_pend_exit();\n            // 0.0.426 (B8)", "return rvT;" }) &&
           has(x3, "const uint32_t x3 = n48_hg_pend_exit3(&gHgPend, seq, exWhy == N48_GFXN_EX_SPARED ? 1u : 0u,") &&
           has(x3, "if (exWhy == N48_GFXN_EX_HEAPGEN && gXdShot.cont && gXdShot.cont_start_us) gHgPend.walknop_post_start++;") &&
           count_of(ahh, "hg_pend_exit3(seq, exWhy)") == 1u);
    // item 7: the ring's retirement sites
    expect("T27e item 7: RET_POLL beside p73_retired in the judged-frame poll's retirement branch; RET_EXPIRY in ks_eop_at_expiry's",
           has(ahh, "            if (frRetiredNow) {\n                p73_retired(frSeq);   // build 0.0.519 (switch 73 Part B): a P's plane may copy only now\n                hg_pend_retired(frSeq, N48_HG_LCLR_RET_POLL);") &&
           has(ks, "            p73_retired(x.seq);   // build 0.0.519 (switch 73 Part B): a P's plane may copy only now\n            hg_pend_retired(x.seq, N48_HG_LCLR_RET_EXPIRY);") &&
           count_of(ahh, "hg_pend_retired(frSeq") == 1u && count_of(ahh, "hg_pend_retired(x.seq") == 1u && has(rt, "if (!l || !__atomic_load_n(&gHgPend.live, __ATOMIC_RELAXED)) return;"));
    const size_t ex = ahh.find("if (frMaintNow) frNewlyExpired = n48_fr_expire(&gKsRing, 1u, frMaintNow, kKsFlightUs);");
    const size_t fxc = ahh.find("if (frNewlyExpired) hg_pend_flight_expired();");
    const size_t rc = ahh.find("(void)n48_fr_reclaim_hold(&gKsRing, frHold, frNHold);");
    expect("T27e item 7: FLIGHT_EXPIRED is asked after n48_fr_expire and BEFORE the reclamation frees the slot; it matches the LIVE seq's "
           "own EXPIRED entry", ex != std::string::npos && fxc != std::string::npos && rc != std::string::npos && ex < fxc && fxc < rc &&
           has(fx, "gKsRing.e[idx].state == N48_FR_EXPIRED") && has(fx, "n48_fr_find(&gKsRing, s, &idx)"));
    expect("T27e the LIVE age equals the flight bound (asserted in the kext)", has(ahh, "static_assert(N48_HG_LIVE_AGE_US == kKsFlightUs,"));
    // item 1: the switch
    expect("T27e item 1: 72 uses its own setter (M1/M2/M3), M3's mode follows every change, the mid-arm guard is unchanged; the "
           "handler says M1 must never be sent",
           has(ahh, "else { changed72 = n48_hg72_set(m, &fpw); if (changed72) { gHgPendOn = fpw; if (!fpw) hg_pend_disarm(); } }") &&
           has(ahh, "if (changed72) hg_pend_mode3(fpw == N48_HG72_M3 ? 1u : 0u);") && !has(ahh, "n48_ra_set(m, &fpw)") &&
           has(ahh, "const bool contRefused72 = n48_cm_cont_switch_refused(72u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") &&
           has(ahh, "NEVER BE SENT"));
    // item 11 (a)
    expect("T27e item 11 (a): EXIT 3 marks a non-spared committed member; the fillset line is followed by the walk-NOPed line; the arm's "
           "open resets the marks", has(hk, "if (exWhy != N48_GFXN_EX_SPARED && gFs.on) fs_note_walk_nopped(seq);") &&
           has(fr, "fs_nop_line();   // build 0.0.532 item 11 (a)") &&
           has(ahh, "                n48_fs85_open(&gFs85);   // build 0.0.530: switch 85's per-scope fields start fresh with the same arm\n                n48_fs_nop_open(&gFsNop);"));
}

int main(int argc, char **argv)
{
    test_replays();
    test_paths();
    test_sweeps();
    test_item11();
    test_lines_and_wiring(argc > 1 ? argv[1] : nullptr);
    std::printf("gfx_hw72: %d check(s), %d failed\n", gChecks, gFails);
    std::printf("gfx_hw72: %s\n", gFails ? "N48-HW72-TEST-FAIL" : "N48-HW72-TEST-PASS");
    return gFails ? 1 : 0;
}
