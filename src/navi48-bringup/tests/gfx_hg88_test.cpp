// gfx_hg88_test.cpp — build 0.0.533 (; an internal design note): SWITCH 88, THE RANGE-PRECISE HEAP-GENERATION
// JUDGE. Host proof of gfx_heapgen.h's switch-88 steps (the copy records, the frame's program pages, n48_hg88_eval inside
// n48_hg_judge) and of the kext's wiring.
//
// T33a  the real sequences, in the kext's order, with the pure functions the kext compiles:
//         RUN AE (run11q): copy #29 then #40 (SecurityAgent's heap, resource 0xffffff960a979400, VRAM [0x12300000,0x12309000), VA
//                          0x400028000 in ITS VM) against frame 12 (WindowServer's twin fill, VS 0x400029300 / PS 0x400029100 in
//                          WindowServer's heap VA 0x400028000 -> VRAM 0x10020000): M1 OK, M2 copy-in-progress (the log's answer).
//         RUN AB2 (run11m): copy #73 then #99 (a WindowServer heap, resource 0xffffff903dc40800, VRAM [0x1349c000,0x134a5000), VA
//                          0x4011f0000) active at frame 20's snapshot (VS 0x400029300, PS 0x400565600 in heap 0x400560000 -> VRAM
//                          0x13175000, SUSPECTED linear as the design says): M1 OK, M2 copy-in-progress.
//         RUN L seq 10 (run10r): a copy over the frame's OWN heap between the judge and the walk: still refused under 88.
//       and T33b, the SWITCH-72 SCHEDULER of tests/gfx_hw72_test.cpp (T27) grown to 88 (its copies carry their real ranges and
//       identities; the frame's programs are identified at the top of the pass): AC seq 4 / seq 5 (run11o), Z2 (run11i) and AB3
//       (run11n) under 72 M3 answer EXACTLY as without 88 (walk answer, bump times, waits, LIVE); AB2 under 72 M3 + 88 is rescued
//       at the judge with no wait.
// T33c  the judge's rules one by one (boundaries, pid, VA, in_place, untracked, unresolved, pva_over, eviction, lost events,
//       the ring walk judged with the COMMITTED record's pages, poison and STALE unchanged).
// T33d  the sweeps: (1) 88 OFF (r88 0) answers exactly as 0.0.532's judge (a verbatim copy below) over every state; (2) a spare
//       under 88 is never given when an independently computed ground truth says a relevant copy wrote a byte of the frame's
//       programs (VRAM) or, in the frame's own VM, their VAs; (3) after CONTINUOUS START 88 ON equals 88 OFF in every observable.
// T33e  the lines (<= 491 body bytes at their widest) and the kext's wiring (ordering, not literal presence alone).
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_hg88_test.cpp -o /tmp/hg88 && \
//         /tmp/hg88 src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/Navi48AccelPeer.cpp \
//         src/navi48-bringup/src/apple/gfx_commit.h
//
// The three source arguments are REQUIRED (the wiring section fails without them).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <initializer_list>

#include "gfx_heapgen.h"

static int gChecks = 0, gFails = 0;
static void expect(const char *what, bool ok)
{
    gChecks++;
    if (!ok) gFails++;
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
}

// =============================================================================================================================
// THE VMs: linear mappings (va, len) -> vram, and host pages. A page not covered is UNMAPPED.
// =============================================================================================================================
static const uint64_t kHost = ~0ull;
struct Map { uint64_t va, len, vram; };
struct Vm { const Map *m; uint32_t n; };
static bool vm_page(const Vm &v, uint64_t va, uint64_t &vram, bool &sys)
{
    for (uint32_t i = 0; i < v.n; i++)
        if (va >= v.m[i].va && va < v.m[i].va + v.m[i].len) {
            sys = v.m[i].vram == kHost;
            vram = sys ? 0x7f0000000ull + (va - v.m[i].va) : v.m[i].vram + (va - v.m[i].va);
            return true;
        }
    return false;
}

// The processes and resources of the real runs.
static const int32_t kWsPidAE = 3616, kSaPidAE = 3720, kWsPidAB2 = 3527, kWsPid = 3527;
static const uintptr_t kResSaAE = 0xffffff960a979400u, kResWsAB2 = 0xffffff903dc40800u, kResWsHeap = 0xffffff8f8b4e1000u;
static const uintptr_t kResAC5 = 0xffffff8f8b4e6700u;
static const uint64_t kWsHeapVa = 0x400028000ull, kWsHeapLo = 0x10020000ull, kWsHeapHi = 0x10029000ull;
// WindowServer's VM (AE, AB2, AC, Z2, AB3, RUN L): its shader heap at 0x400028000 -> VRAM 0x10020000; AB2's heap 0x400560000 ->
// VRAM 0x13175000 (copies #66/#70); AB2's copied heap 0x4011f0000 -> 0x1349c000; AC seq 5's copied heap 0x4011c8000 -> 0x13cfc000.
static const Map kWsMaps[] = { { 0x400028000ull, 0x9000ull, 0x10020000ull }, { 0x400560000ull, 0x9000ull, 0x13175000ull },
                               { 0x4011f0000ull, 0x9000ull, 0x1349c000ull }, { 0x4011c8000ull, 0x9000ull, 0x13cfc000ull },
                               { 0x400700000ull, 0x4000ull, kHost } };
static const Vm kWsVm = { kWsMaps, sizeof kWsMaps / sizeof kWsMaps[0] };

// =============================================================================================================================
// THE KEXT's STEPS, in its order (AppleHardwareHook.cpp: hg_frame_begin_pass, hg88_note_program + n48_hg_frame_note_pva in
// gfxsrc_identify, hg_commit_judge, hg_commit_record, hg_exempt_refuse's n48_hg_walk_answer; hw_hg_copy_begin / hw_hg_copy_end)
// =============================================================================================================================
struct K {
    n48_hg_state st; n48_hg_frame f, rec; n48_hg_pend pd;
    uint64_t judged;
    uint32_t on88, startup;
};
static void k_init(K &k, uint32_t on88, uint32_t startup = 1u)
{
    std::memset(&k, 0, sizeof k);
    k.on88 = on88; k.startup = startup; k.judged = 10u;
}
// hg88_note_program, then the VA (gfxsrc_identify's order)
static void k_identify(K &k, const Vm &vm, uint64_t va, int32_t pid)
{
    if (k.f.r88) {
        n48_hg88_note_pid(&k.f, pid);
        if (!n48_hg88_va_known(&k.f, va)) {
            const uint64_t lo = va & ~0xfffull, hi = (va + (uint64_t)N48_HG88_PGM_BYTES + 0xfffull) & ~0xfffull;
            for (uint64_t pg = lo; pg < hi; pg += 0x1000ull) {
                uint64_t off = 0; bool sys = false;
                const bool ok = vm_page(vm, pg, off, sys) && !sys;
                n48_hg88_note_page(&k.f, ok ? off : 0ull, ok ? 1u : 0u);
            }
        }
    }
    if (k.f.on) n48_hg_frame_note_pva(&k.f, va);
}
static void k_pass(K &k, const Vm &vm, std::initializer_list<uint64_t> pgms, int32_t pid)
{
    k.judged++;
    n48_hg_frame_begin(&k.f, 1u, k.judged, &k.st);
    k.st.judged++;
    n48_hg88_latch(&k.f, k.on88, k.startup);
    for (uint64_t va : pgms) k_identify(k, vm, va, pid);
}
static uint32_t k_judge(K &k) { return n48_hg_judge(&k.st, &k.f, k.judged); }
static void k_record(K &k) { k.rec = k.f; }
static uint32_t k_walk(K &k) { return n48_hg_walk_answer(&k.st, &k.rec, &k.pd, 7u, 0x5501u); }
struct Cp { uint64_t lo, hi; uintptr_t res; uint64_t va, bytes; int32_t pid; uint32_t va_ok, range_ok, np; };
static uint64_t k_copy_begin(K &k, const Cp &c, uint32_t *bumpedOut = nullptr)
{
    const uint32_t overlaps = (c.np || n48_hg_reg_overlaps(&k.st, c.lo, c.hi)) ? 1u : 0u;
    const uint32_t bumped = n48_hg_copy_begin(&k.st, overlaps);
    n48_hg_copy_id id {};
    id.res = c.res; id.va = c.va; id.bytes = c.bytes; id.pid = c.pid; id.va_ok = c.va_ok; id.range_ok = c.range_ok;
    const uint64_t tok = n48_hg88_copy_note(&k.st, bumped, c.lo, c.hi, c.range_ok || c.res || c.pid ? &id : nullptr);
    if (bumpedOut) *bumpedOut = bumped;
    return tok;
}
static void k_copy_end(K &k, const Cp &c, uint64_t tok)
{
    n48_hg_copy_end(&k.st, 1u, c.va_ok, c.va, c.va + c.bytes, c.lo, c.hi, nullptr, 0u);
    n48_hg88_close(&k.st, tok);
}
static void k_copy(K &k, const Cp &c) { const uint64_t t = k_copy_begin(k, c); k_copy_end(k, c, t); }

// The real copies.
static const Cp kAE29 = { 0x12300000ull, 0x12309000ull, kResSaAE, 0x400028000ull, 0x9000ull, kSaPidAE, 1u, 1u, 11u };
static const Cp kAE40 = { 0x12300000ull, 0x12309000ull, kResSaAE, 0x400028000ull, 0x9000ull, kSaPidAE, 1u, 1u, 24u };
static const Cp kAB2_73 = { 0x1349c000ull, 0x134a5000ull, kResWsAB2, 0x4011f0000ull, 0x9000ull, kWsPidAB2, 1u, 1u, 19u };
static const Cp kAB2_99 = kAB2_73;
static const Cp kWsHeap = { kWsHeapLo, kWsHeapHi, kResWsHeap, kWsHeapVa, 0x9000ull, kWsPid, 1u, 1u, 32u };

// =============================================================================================================================
// T33a — THE REAL SEQUENCES (AE, AB2, RUN L) with the pure functions in the kext's order
// =============================================================================================================================
static uint32_t gAeWalk = 0u;
static uint32_t rp_ae(K &k, uint32_t on88, uint32_t startup = 1u)
{
    k_init(k, on88, startup);
    k.st.gen = 10u;                                   // the copies before #29 (the generation at #29's start was 10 -> 11)
    k_copy(k, kAE29);                                 // :15775 COPIED #29 (gen -> 12)
    k_pass(k, kWsVm, { 0x400029300ull, 0x400029100ull }, kWsPidAE);   // frame 12's pass: snapshot gen 12, active 0
    const uint64_t t40 = k_copy_begin(k, kAE40);      // :16180 bump -> 13 (odd = in progress), active 1
    const uint32_t why = k_judge(k);                  // :16195 the commit's judge
    k_record(k);
    k_copy_end(k, kAE40, t40);                        // :16309 -> 14
    gAeWalk = why == N48_HG_OK ? k_walk(k) : 0xffu;   // the walk (only a committed frame is walked)
    return why;
}
static uint32_t rp_ab2(K &k, uint32_t on88, uint32_t *walk = nullptr)
{
    k_init(k, on88);
    k.st.gen = 22u;
    k_copy(k, kAB2_73);                               // :17997 COPIED #73 (gen -> 24)
    const uint64_t t99 = k_copy_begin(k, kAB2_99);    // :18715 bump -> 25, BEFORE frame 20's pass
    k_pass(k, kWsVm, { 0x400029300ull, 0x400565600ull }, kWsPidAB2);   // snapshot 25 with a copy active
    const uint32_t why = k_judge(k);                  // :18747
    k_record(k);
    k_copy_end(k, kAB2_99, t99);                      // :18852 -> 26
    if (walk) *walk = why == N48_HG_OK ? k_walk(k) : 0xffu;
    return why;
}
static uint32_t rp_runl(K &k, uint32_t on88, uint32_t *judge = nullptr)
{
    k_init(k, on88);
    k_copy(k, kWsHeap); k_copy(k, kWsHeap);           // the heap's earlier copies (#1, #2 ...): the next one is in place
    k_pass(k, kWsVm, { 0x400028f00ull, 0x40002a300ull }, kWsPid);     // seq 10's frame: its own programs in that heap
    const uint32_t j = k_judge(k);
    if (judge) *judge = j;
    k_record(k);
    const uint64_t t = k_copy_begin(k, kWsHeap);      // 14367: over [0x10020000,0x10029000), after the judge, before the walk
    const uint32_t w = k_walk(k);                     // 14476: the walk
    k_copy_end(k, kWsHeap, t);
    return w;
}
static void test_real()
{
    K k; char b[420];
    const uint32_t m1 = rp_ae(k, 1u); const uint32_t m1w = gAeWalk;
    std::snprintf(b, sizeof b, "T33a RUN AE (run11q) M1 (344): frame 12 judged with SecurityAgent's copy #40 in progress is SPARED "
                  "(%s), and its walk after the copy ended too (%s): VRAM disjoint, another pid, in place (#29 then #40)",
                  n48_hg_reason_name(m1), n48_hg_reason_name(m1w));
    expect(b, m1 == N48_HG_OK && m1w == N48_HG_OK && k.f.npg == 2u && k.f.pg[0] == 0x10021000ull && k.f.pg[1] == 0x10022000ull &&
              k.f.pid == kWsPidAE && k.st.c88_pushed == 2u);
    const uint32_t m2 = rp_ae(k, 0u);
    expect("T33a RUN AE M2 (600, OFF): copy-in-progress - the log's answer (\"generation at the verdict 12, now 13, active 1\")",
           m2 == N48_HG_IN_PROGRESS && k.f.snap_gen == 12u && k.f.r88 == 0u);
    const uint32_t ps = rp_ae(k, 1u, 0u);
    expect("T33a RUN AE M1 AFTER CONTINUOUS START (startup 0): copy-in-progress, exactly M2", ps == N48_HG_IN_PROGRESS && k.f.r88 == 0u);
    uint32_t w = 0u;
    const uint32_t b1 = rp_ab2(k, 1u, &w);
    std::snprintf(b, sizeof b, "T33a RUN AB2 (run11m) M1: frame 20 judged at generation 25 WITH copy #99 active is SPARED (%s; walk %s): "
                  "same pid, VA [0x4011f0000,0x4011f9000) misses both programs, VRAM misses their 4 pages, in place (#73 then #99)",
                  n48_hg_reason_name(b1), n48_hg_reason_name(w));
    expect(b, b1 == N48_HG_OK && w == N48_HG_OK && k.f.snap_gen == 25u && k.f.snap_active == 1u && k.f.npg == 4u);
    const uint32_t b2 = rp_ab2(k, 0u);
    expect("T33a RUN AB2 M2: copy-in-progress - the log's answer", b2 == N48_HG_IN_PROGRESS);
    uint32_t j1 = 0u, j2 = 0u;
    const uint32_t l1 = rp_runl(k, 1u, &j1), l2 = rp_runl(k, 0u, &j2);
    std::snprintf(b, sizeof b, "T33a RUN L seq 10 (run10r): the copy over the frame's OWN heap between the judge and the walk is STILL "
                  "REFUSED under M1 (%s) as under M2 (%s); both judges were ok", n48_hg_reason_name(l1), n48_hg_reason_name(l2));
    expect(b, l1 == N48_HG_IN_PROGRESS && l2 == N48_HG_IN_PROGRESS && j1 == N48_HG_OK && j2 == N48_HG_OK);
    // the same RUN L with the walk after the copy completed: generation-changed, still refused (the ended record is relevant)
    k_init(k, 1u); k_copy(k, kWsHeap);
    k_pass(k, kWsVm, { 0x400028f00ull, 0x40002a300ull }, kWsPid);
    const uint32_t jj = k_judge(k); k_record(k); k_copy(k, kWsHeap);
    expect("T33a RUN L with the copy COMPLETED before the walk: generation-changed under M1 (an ended copy after the snapshot is relevant)",
           jj == N48_HG_OK && k_walk(k) == N48_HG_CHANGED);
}

// =============================================================================================================================
// T33b — THE SWITCH-72 SCHEDULER (gfx_hw72_test.cpp's T27 Sim) GROWN TO 88
// =============================================================================================================================
static const uintptr_t kSub = 0x5501u, kCopier = 0x7702u;
static const uint64_t kNever = ~0ull;
static const uint64_t kLineUs = 50u;
struct HCopy { uint64_t at, dur; uint32_t np, npoison; uintptr_t thread; Cp c; };
enum { EV_FB = 0, EV_J, EV_R, EV_P, EV_W, EV_X, EV_POLL0, EV_N = EV_POLL0 + 2 };
struct Sim {
    n48_hg_state st; n48_hg_pend pd; n48_hg_frame f, rec;
    uint64_t now; int held; uint32_t delays, reads, readsHeld;
    uint32_t mode, startup, on88, gate, seq; uint64_t fenceOff; uint32_t want;
    uint64_t pg0, pg1; int32_t pid;                  // the frame's two programs and its pid
    uint64_t t[EV_N]; uint64_t fenceLat;
    uint32_t done[EV_N];
    uint32_t judgeWhy, committed, walked, walkWhy, spared, x3;
    uint64_t walkedAt, tF;
    HCopy c[3]; uint32_t nc, ci, started[3], bumped[3], ended[3], look[3]; uint64_t tok[3];
    uint64_t bumpAt[3], endAt[3], waited[3], lastEnd;
    uint32_t bumpUnderLive;
};
static void sim_submit_due(Sim &s);
static uint64_t cb_now(void *c) { return static_cast<Sim *>(c)->now; }
static void cb_lock(void *c) { static_cast<Sim *>(c)->held++; }
static void cb_unlock(void *c) { static_cast<Sim *>(c)->held--; }
static void cb_delay(void *c, uint32_t us) { Sim *s = static_cast<Sim *>(c); s->delays++; s->now += us; sim_submit_due(*s); }
static uint32_t cb_read(void *c, uint64_t off, uint32_t *val)
{
    Sim *s = static_cast<Sim *>(c);
    s->reads++;
    if (s->held) s->readsHeld++;
    *val = (off == s->fenceOff && s->tF != kNever && s->now >= s->tF) ? s->want : 0u;
    return 1u;
}
static const n48_hg_wait_ops kOps3 = { &cb_now, &cb_lock, &cb_unlock, &cb_delay, &cb_read };
static const n48_hg_wait_ops kOps1 = { &cb_now, &cb_lock, &cb_unlock, &cb_delay, nullptr };

static void sim_submit_due(Sim &s)
{
    if (s.held) return;
    const uint32_t latch = s.mode == 1u || s.mode == 3u;
    for (uint32_t e = 0; e < EV_N; e++) {
        if (s.done[e] || s.t[e] == kNever || s.now < s.t[e]) continue;
        if (e >= EV_J && e <= EV_X && !s.done[e - 1u]) break;
        s.done[e] = 1u;
        switch (e) {
        case EV_FB: {                                                    // hg_frame_begin_pass, then gfxsrc_identify per program
            n48_hg_frame_begin(&s.f, 1u, 5u, &s.st);
            n48_hg88_latch(&s.f, s.on88, s.startup);
            for (uint64_t va : { s.pg0, s.pg1 }) {
                if (s.f.r88) {
                    n48_hg88_note_pid(&s.f, s.pid);
                    if (!n48_hg88_va_known(&s.f, va))
                        for (uint64_t pg = va & ~0xfffull; pg < ((va + N48_HG88_PGM_BYTES + 0xfffull) & ~0xfffull); pg += 0x1000ull) {
                            uint64_t off = 0; bool sys = false; const bool ok = vm_page(kWsVm, pg, off, sys) && !sys;
                            n48_hg88_note_page(&s.f, ok ? off : 0ull, ok ? 1u : 0u);
                        }
                }
                n48_hg_frame_note_pva(&s.f, va);
            }
            break;
        }
        case EV_J:
            s.judgeWhy = n48_hg_judge(&s.st, &s.f, 5u);
            if (s.pd.m3) s.pd.startup = s.startup;
            if (s.judgeWhy == N48_HG_OK && latch) n48_hg_pend_set(&s.pd, kSub, s.now);
            break;
        case EV_R:
            s.committed = (s.judgeWhy == N48_HG_OK && s.gate) ? 1u : 0u;
            if (latch) n48_hg_pend_record(&s.pd, s.committed ? s.seq : 0u, kSub);
            s.rec = s.f;
            break;
        case EV_P: if (s.committed && s.pd.m3) (void)n48_hg_pend_fence(&s.pd, s.seq, kSub, s.fenceOff, s.want); break;
        case EV_W:
            if (s.committed) {
                s.walkWhy = n48_hg_walk_answer(&s.st, &s.rec, &s.pd, s.seq, kSub);
                s.walked = 1u; s.walkedAt = s.now; s.spared = s.walkWhy == N48_HG_OK;
                if (s.spared && s.fenceLat != kNever) s.tF = s.now + s.fenceLat;
            }
            break;
        case EV_X:
            if (s.pd.m3) s.x3 = n48_hg_pend_exit3(&s.pd, s.seq, s.spared, kSub, s.now);
            (void)n48_hg_pend_clear(&s.pd, N48_HG_PCLR_EXIT, 0u, kSub);
            break;
        default:
            if (s.spared && s.tF != kNever && s.now >= s.tF) (void)n48_hg_pend_retired(&s.pd, s.seq, N48_HG_LCLR_RET_POLL);
            break;
        }
    }
}
// hw_hg_copy_begin, in the kext's order (the record in the bump's own section)
static void sim_copy_begin(Sim &s, uint32_t i)
{
    const HCopy &c = s.c[i];
    cb_lock(&s);
    uint64_t w = 0u; uint32_t pw = N48_HG_PW_GO;
    const uint32_t mayWait = n48_hg_copy_may_wait(c.np, c.npoison, 0u, 0u, 0u);
    const uint32_t own = (c.np || c.npoison) ? 1u : 0u;
    if (s.pd.pending && !mayWait && (own || n48_hg_reg_overlaps(&s.st, c.c.lo, c.c.hi))) s.pd.unpatched++;
    if (s.pd.m3) pw = n48_hg_copy_wait3(&kOps3, &s, &s.st, &s.pd, c.thread, own, mayWait, c.c.lo, c.c.hi, &w);
    else
    if (s.pd.pending && mayWait) pw = n48_hg_copy_wait(&kOps1, &s, &s.st, &s.pd, c.thread, own, c.c.lo, c.c.hi, &w);
    s.bumped[i] = n48_hg_copy_begin(&s.st, (own || n48_hg_reg_overlaps(&s.st, c.c.lo, c.c.hi)) ? 1u : 0u);
    n48_hg_copy_id id {}; id.res = c.c.res; id.va = c.c.va; id.bytes = c.c.bytes; id.pid = c.c.pid; id.va_ok = 1u; id.range_ok = 1u;
    s.tok[i] = n48_hg88_copy_note(&s.st, s.bumped[i], c.c.lo, c.c.hi, &id);
    s.bumpAt[i] = s.now; s.look[i] = pw; s.waited[i] = w;
    if (s.bumped[i] && s.spared && s.now >= s.walkedAt && (s.tF == kNever || s.now < s.tF)) s.bumpUnderLive++;
    cb_unlock(&s);
}
static void sim_init(Sim &s, uint32_t mode, uint32_t on88, uint32_t startup = 1u)
{
    std::memset(&s, 0, sizeof s);
    s.mode = mode; s.on88 = on88; s.startup = startup; s.gate = 1u;
    s.pd.m3 = mode == 3u ? 1u : 0u;
    s.seq = 4u; s.fenceOff = 0x3cba8f004ull; s.want = 0x50ae0001u;
    s.pg0 = 0x400029300ull; s.pg1 = 0x400029100ull; s.pid = kWsPid;
    for (uint32_t e = 0; e < EV_N; e++) s.t[e] = kNever;
    s.fenceLat = 400u; s.tF = kNever; s.walkedAt = kNever;
    s.st.gen = 12u;
    // the heaps' earlier copies (already complete): every later copy of the same resource over the same range is IN PLACE
    const Cp prior[] = { kWsHeap, kAB2_73, { 0x13cfc000ull, 0x13d05000ull, kResAC5, 0x4011c8000ull, 0x9000ull, kWsPid, 1u, 1u, 18u } };
    for (const Cp &c : prior) {
        n48_hg_reg_add(&s.st, c.lo + 0x100u, c.lo + 0x200u);   // a substituted program in each heap: its copies overlap the registry
        const uint32_t b = n48_hg_copy_begin(&s.st, 1u);
        n48_hg_copy_id id {}; id.res = c.res; id.va = c.va; id.bytes = c.bytes; id.pid = c.pid; id.va_ok = 1u; id.range_ok = 1u;
        const uint64_t t = n48_hg88_copy_note(&s.st, b, c.lo, c.hi, &id);
        n48_hg_copy_end(&s.st, 1u, 1u, c.va, c.va + c.bytes, c.lo, c.hi, nullptr, 0u);
        n48_hg88_close(&s.st, t);
    }
}
static void sim_add_copy(Sim &s, uint64_t at, uint32_t np, const Cp &c, uint64_t dur = 400u, uintptr_t thread = kCopier)
{
    HCopy &h = s.c[s.nc++];
    h = HCopy { at, dur, np, 0u, thread, c };
}
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
            uint32_t before = 0; for (uint32_t e = 0; e < EV_N; e++) before += s.done[e];
            sim_submit_due(s);
            uint32_t after = 0; for (uint32_t e = 0; e < EV_N; e++) after += s.done[e];
            if (after == before) for (uint32_t e = 0; e < EV_N; e++) if (!s.done[e] && s.t[e] == nextSub) { s.done[e] = 1u; break; }
            continue;
        }
        if (s.now < nextCopy) s.now = nextCopy;
        const uint32_t i = s.ci;
        if (!s.started[i]) { s.started[i] = 1u; sim_copy_begin(s, i); s.endAt[i] = s.bumped[i] ? s.now + s.c[i].dur : s.now; continue; }
        if (s.bumped[i]) {
            const Cp &c = s.c[i].c;
            n48_hg_copy_end(&s.st, 1u, 1u, c.va, c.va + c.bytes, c.lo, c.hi, nullptr, 0u);
            n48_hg88_close(&s.st, s.tok[i]);
        }
        s.ended[i] = 1u; s.lastEnd = s.now; s.ci++;
    }
}
static uint64_t L(uint64_t line, uint64_t base) { return (line - base) * kLineUs; }
static void rp_ac4(Sim &s, uint32_t mode, uint32_t on88, uint32_t startup = 1u)
{
    const uint64_t b = 16090u;
    sim_init(s, mode, on88, startup);
    s.t[EV_FB] = L(16093, b); s.t[EV_J] = L(16094, b); s.t[EV_R] = L(16095, b); s.t[EV_P] = L(16218, b); s.t[EV_W] = L(16299, b);
    s.t[EV_X] = L(16300, b);
    sim_add_copy(s, L(16216, b), 32u, kWsHeap);                       // over WindowServer's own heap (the fill's programs)
    sim_run(s);
}
static void rp_ac5(Sim &s, uint32_t mode, uint32_t on88)
{
    const uint64_t b = 19230u;
    sim_init(s, mode, on88);
    s.seq = 5u; s.fenceOff = 0x3cba8f008ull; s.want = 0x50ae0002u;
    s.pg0 = 0x400029300ull; s.pg1 = 0x400565600ull;                   // frame 21: PS 0x400565600 (the census), VS SUSPECTED 0x400029300
    s.t[EV_FB] = L(19239, b); s.t[EV_J] = L(19240, b); s.t[EV_R] = L(19254, b); s.t[EV_P] = L(19276, b); s.t[EV_W] = L(19348, b);
    s.t[EV_X] = L(19349, b);
    sim_add_copy(s, L(19241, b), 18u, { 0x13cfc000ull, 0x13d05000ull, kResAC5, 0x4011c8000ull, 0x9000ull, kWsPid, 1u, 1u, 18u });   // #109
    sim_run(s);
}
static void rp_z2(Sim &s, uint32_t mode, uint32_t on88)
{
    const uint64_t b = 15700u;
    sim_init(s, mode, on88);
    s.want = 0x76270001u;
    s.t[EV_FB] = L(15710, b); s.t[EV_J] = L(15711, b); s.t[EV_R] = L(15712, b); s.t[EV_P] = L(15847, b); s.t[EV_W] = L(15922, b);
    s.t[EV_X] = L(15923, b);
    sim_add_copy(s, L(15822, b), 32u, kWsHeap);
    sim_run(s);
}
static void rp_ab3(Sim &s, uint32_t mode, uint32_t on88, uint64_t fenceLat)
{
    const uint64_t b = 16070u;
    sim_init(s, mode, on88);
    s.want = 0xfff00001u; s.fenceLat = fenceLat;
    s.t[EV_FB] = L(16072, b); s.t[EV_J] = L(16073, b); s.t[EV_R] = L(16074, b); s.t[EV_P] = L(16192, b); s.t[EV_W] = L(16265, b);
    s.t[EV_X] = L(16266, b); s.t[EV_POLL0] = L(16980, b);
    sim_add_copy(s, L(16278, b), 30u, kWsHeap);
    sim_run(s);
}
static void rp_ab2s(Sim &s, uint32_t mode, uint32_t on88)
{
    const uint64_t b = 18710u;
    sim_init(s, mode, on88);
    s.pg0 = 0x400029300ull; s.pg1 = 0x400565600ull;
    s.t[EV_FB] = L(18732, b); s.t[EV_J] = L(18747, b); s.t[EV_R] = L(18753, b); s.t[EV_P] = L(18754, b); s.t[EV_W] = L(18760, b);
    s.t[EV_X] = L(18761, b);
    sim_add_copy(s, L(18715, b), 19u, kAB2_99, 5000u);                // still writing at the judge
    sim_run(s);
}
static void rp_runls(Sim &s, uint32_t mode, uint32_t on88)
{
    sim_init(s, mode, on88);
    s.seq = 10u; s.pg0 = 0x400028f00ull; s.pg1 = 0x40002a300ull;
    s.t[EV_FB] = 0u; s.t[EV_J] = 10u; s.t[EV_R] = 20u; s.t[EV_P] = 21u; s.t[EV_W] = 50u; s.t[EV_X] = 51u;
    sim_add_copy(s, 30u, 1u, kWsHeap);
    sim_run(s);
}
static bool same_obs(const Sim &a, const Sim &b)
{
    for (uint32_t i = 0; i < 3; i++) if (a.bumpAt[i] != b.bumpAt[i] || a.waited[i] != b.waited[i] || a.look[i] != b.look[i]) return false;
    return a.judgeWhy == b.judgeWhy && a.walked == b.walked && a.walkWhy == b.walkWhy && a.x3 == b.x3 && a.st.gen == b.st.gen &&
           a.pd.live_sets == b.pd.live_sets && a.pd.waits == b.pd.waits && a.pd.sets == b.pd.sets &&
           std::memcmp(a.pd.lclr, b.pd.lclr, sizeof a.pd.lclr) == 0 && std::memcmp(a.pd.clr, b.pd.clr, sizeof a.pd.clr) == 0 &&
           a.pd.live == b.pd.live && a.pd.pending == b.pd.pending && a.bumpUnderLive == b.bumpUnderLive;
}
static void test_sched()
{
    Sim with {}, without {};
    char b[420];
    struct { const char *name; void (*run)(Sim &, uint32_t, uint32_t); } rps[] = {
        { "AC seq 4 (run11o)", [](Sim &s, uint32_t m, uint32_t o) { rp_ac4(s, m, o); } },
        { "AC seq 5 (run11o)", [](Sim &s, uint32_t m, uint32_t o) { rp_ac5(s, m, o); } },
        { "Z2 seq 4 (run11i)", [](Sim &s, uint32_t m, uint32_t o) { rp_z2(s, m, o); } },
        { "AB3 seq 4 (run11n), fence 5 ms", [](Sim &s, uint32_t m, uint32_t o) { rp_ab3(s, m, o, 5000u); } },
        { "AB3 seq 4 (run11n), fence 200 us", [](Sim &s, uint32_t m, uint32_t o) { rp_ab3(s, m, o, 200u); } },
        { "RUN L seq 10 (run10r)", [](Sim &s, uint32_t m, uint32_t o) { rp_runls(s, m, o); } },
    };
    for (const auto &r : rps) {
        r.run(with, 3u, 1u); r.run(without, 3u, 0u);
        std::snprintf(b, sizeof b, "T33b %s under 72 M3 (840): 88 ON (344) answers EXACTLY as 88 OFF - walk %s, bump at %llu us, waited "
                      "%llu us, LIVE sets %llu, nothing under the live frame", r.name, n48_hg_reason_name(with.walkWhy),
                      (unsigned long long)with.bumpAt[0], (unsigned long long)with.waited[0], (unsigned long long)with.pd.live_sets);
        expect(b, same_obs(with, without) && with.walked && with.walkWhy == N48_HG_OK && with.bumpUnderLive == 0u && with.readsHeld == 0u);
    }
    // 72 OFF (584): AC seq 4 / Z2 / RUN L stay refused under 88 (the copy is over the fill's own heap); AC seq 5 is spared
    rp_ac4(with, 2u, 1u); rp_ac4(without, 2u, 0u);
    expect("T33b AC seq 4, 72 OFF: the walk REFUSES with or without 88 (the copy writes the fill's own heap pages)",
           with.walked && without.walked && with.walkWhy != N48_HG_OK && without.walkWhy != N48_HG_OK);
    rp_z2(with, 2u, 1u);
    expect("T33b Z2 seq 4, 72 OFF, 88 ON: the walk REFUSES (own heap)", with.walked && with.walkWhy != N48_HG_OK);
    rp_runls(with, 2u, 1u);
    expect("T33b RUN L seq 10, 72 OFF, 88 ON: the walk REFUSES (own heap)", with.walked && with.walkWhy != N48_HG_OK);
    rp_ac5(with, 2u, 1u); rp_ac5(without, 2u, 0u);
    std::snprintf(b, sizeof b, "T33b AC seq 5, 72 OFF: 88 SPARES the walk (%s; 88 OFF: %s) - copy #109 over [0x13cfc000,0x13d05000) at "
                  "VA 0x4011c8000 misses both programs (a change from 0.0.532 72 OFF: reported)", n48_hg_reason_name(with.walkWhy),
                  n48_hg_reason_name(without.walkWhy));
    expect(b, with.walked && with.walkWhy == N48_HG_OK && without.walked && without.walkWhy != N48_HG_OK);
    // AB2 under 72 M3 + 88: no wait (the copy began before the pass) and the judge now SPARES it
    rp_ab2s(with, 3u, 1u); rp_ab2s(without, 3u, 0u);
    expect("T33b AB2 frame 20 under 72 M3 + 88: NO WAIT (the copy began before the pass), the judge SPARES it and the walk too; 88 OFF: "
           "copy-in-progress, as 0.0.532", with.pd.waits == 0u && with.waited[0] == 0u && with.judgeWhy == N48_HG_OK && with.walked &&
           with.walkWhy == N48_HG_OK && without.judgeWhy == N48_HG_IN_PROGRESS && !without.committed);
    // after CONTINUOUS START (startup 0): 88 ON == 88 OFF in every replay, 72 M2 and M3
    uint32_t diff = 0u;
    for (uint32_t m : { 2u, 3u }) {
        rp_ab2s(with, m, 1u); with.startup = 0u;
        Sim a {}, c {};
        sim_init(a, m, 1u, 0u); sim_init(c, m, 0u, 0u);
        for (Sim *p : { &a, &c }) {
            const uint64_t bb = 18710u;
            p->pg0 = 0x400029300ull; p->pg1 = 0x400565600ull;
            p->t[EV_FB] = L(18732, bb); p->t[EV_J] = L(18747, bb); p->t[EV_R] = L(18753, bb); p->t[EV_P] = L(18754, bb);
            p->t[EV_W] = L(18760, bb); p->t[EV_X] = L(18761, bb);
            sim_add_copy(*p, L(18715, bb), 19u, kAB2_99, 5000u);
            sim_run(*p);
        }
        if (!same_obs(a, c) || a.f.r88) diff++;
    }
    expect("T33b AB2 after CONTINUOUS START: 88 ON is 88 OFF (r88 0; copy-in-progress) under 72 M2 and M3", diff == 0u);
}

// =============================================================================================================================
// T33c — THE RULES, one by one
// =============================================================================================================================
// A frame of WindowServer (pid kWsPid) with its programs in the WS heap; `extra` pages/VAs for the rules below.
static void f_ws(K &k, uint32_t on88 = 1u)
{
    k_init(k, on88);
    k.st.gen = 40u;
    k_copy(k, kWsHeap);                                               // WS heap's earlier copy: in place from now on
    k_copy(k, kAE29);                                                 // SecurityAgent's earlier copy: in place from now on
}
static void test_rules()
{
    K k;
    // the page boundaries: frame pages 0x10021000/0x10022000; a copy that ENDS at 0x10021000 is disjoint, one that ends 1 byte later is not
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        Cp c = kAE29; c.res = 0x1111u; c.lo = 0x10020000ull; c.hi = 0x10021000ull;   // another resource, first copy at this range
        k_copy(k, c);                                                  // its first copy: NOT in place, so check the boundary with in_place set
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t = k_copy_begin(k, c);
        const uint32_t edge = k_judge(k);
        k_copy_end(k, c, t);
        Cp d = c; d.res = 0x2222u; d.hi = 0x10021001ull; k_copy(k, d);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t2 = k_copy_begin(k, d);
        const uint32_t over = k_judge(k);
        k_copy_end(k, d, t2);
        Cp e = c; e.res = 0x3333u; e.lo = 0x10023000ull; e.hi = 0x10024000ull; k_copy(k, e);   // starts exactly at the last page's end
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t3 = k_copy_begin(k, e);
        const uint32_t hiEdge = k_judge(k);
        k_copy_end(k, e, t3);
        expect("T33c the page overlap is half-open both ways: a copy ending AT the frame's first page is spared, one byte into it is "
               "refused, one starting AT the last page's end is spared", edge == N48_HG_OK && over == N48_HG_IN_PROGRESS &&
               hiEdge == N48_HG_OK);
    }
    // another pid, VRAM disjoint, in place: spared; the same with the SAME pid and the VA over a program: refused (the VA clause)
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        Cp c = kAE29; c.pid = kWsPid;                                  // WindowServer's own copy of a heap whose VA covers the program
        const uint64_t t = k_copy_begin(k, c);
        const uint32_t same = k_judge(k);
        uint32_t first = 99u; const uint32_t why = n48_hg88_eval(&k.st, &k.f, &first);
        k_copy_end(k, c, t);
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t2 = k_copy_begin(k, kAE29);
        const uint32_t other = k_judge(k);
        k_copy_end(k, kAE29, t2);
        expect("T33c the same-pid VA clause: WindowServer's own in-place copy at VA [0x400028000,+0x9000) over a program is refused even "
               "though its VRAM misses the frame's pages; SecurityAgent's (another VM) is spared", same == N48_HG_IN_PROGRESS &&
               why == N48_HG88_OVERLAP && first < N48_HG88_RING && other == N48_HG_OK);
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        Cp z = kAE29; z.pid = 0; const uint64_t t3 = k_copy_begin(k, z);
        const uint32_t kern = k_judge(k); k_copy_end(k, z, t3);
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        Cp nv = kAE29; nv.pid = kWsPid; nv.va_ok = 0u; nv.va = 0x500000000ull; const uint64_t t4 = k_copy_begin(k, nv);
        const uint32_t noVa = k_judge(k); k_copy_end(k, nv, t4);
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid); k_identify(k, kWsVm, 0x400565600ull, kSaPidAE);   // a frame of two pids
        const uint64_t t5 = k_copy_begin(k, kAE29);
        const uint32_t mixed = k_judge(k); k_copy_end(k, kAE29, t5);
        expect("T33c pid 0 (kernel), a same-pid copy with no VA, and a frame of two pids all take the VA clause and are refused",
               kern == N48_HG_IN_PROGRESS && noVa == N48_HG_IN_PROGRESS && mixed == N48_HG_IN_PROGRESS);
    }
    // not in place: the resource's first copy (or a moved range) is refused, however disjoint
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        Cp c = kAE29; c.res = 0x4444u;                                 // never copied before
        const uint64_t t = k_copy_begin(k, c);
        const uint32_t first = k_judge(k); const uint32_t why = n48_hg88_eval(&k.st, &k.f, nullptr); k_copy_end(k, c, t);
        f_ws(k);
        Cp mv = kAE29; mv.lo = 0x12400000ull; mv.hi = 0x12409000ull;   // SecurityAgent's heap MOVED
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t2 = k_copy_begin(k, mv);
        const uint32_t moved = k_judge(k); k_copy_end(k, mv, t2);
        expect("T33c NOT IN PLACE: a resource's first copy, and a copy over a new range, are refused (not-in-place)",
               first == N48_HG_IN_PROGRESS && why == N48_HG88_NOT_IN_PLACE && moved == N48_HG_IN_PROGRESS);
        // an unknown-range copy of the resource in between makes the next copy not in place either
        f_ws(k);
        Cp u = kAE29; u.range_ok = 0u; k_copy(k, u);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t3 = k_copy_begin(k, kAE29);
        const uint32_t after = n48_hg88_eval(&k.st, &k.f, nullptr); k_copy_end(k, kAE29, t3);
        expect("T33c an unknown-range copy of the resource resets its in-place memory (the next copy is not in place)",
               after == N48_HG88_NOT_IN_PLACE);
    }
    // UNTRACKED (the no-slot copy: no identity, an unknown range) is refused; so is a record with range_ok 0
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint32_t b = n48_hg_copy_begin(&k.st, 1u);
        (void)n48_hg88_copy_note(&k.st, b, 0x12300000ull, 0x12309000ull, nullptr);   // hw_hg_copy_begin(..., nullptr)
        const uint32_t why = k_judge(k), r = n48_hg88_eval(&k.st, &k.f, nullptr);
        expect("T33c UNTRACKED: the no-slot copy (no identity) is an UNKNOWN range and refuses every frame while relevant",
               why == N48_HG_IN_PROGRESS && r == N48_HG88_UNTRACKED && k.st.c88_untracked == 1u);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        expect("T33c ... it never closes, so it refuses every later frame too", k_judge(k) == N48_HG_IN_PROGRESS);
    }
    // UNRESOLVED pages (a host page, an unmapped page, the list FULL) and pva_over
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull, 0x400700100ull }, kWsPid);   // the second program sits in HOST memory
        const uint64_t t = k_copy_begin(k, kAE40);
        const uint32_t host = k_judge(k), rh = n48_hg88_eval(&k.st, &k.f, nullptr); k_copy_end(k, kAE40, t);
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull, 0x480000000ull }, kWsPid);   // unmapped
        const uint64_t t2 = k_copy_begin(k, kAE40);
        const uint32_t unm = k_judge(k); k_copy_end(k, kAE40, t2);
        expect("T33c UNRESOLVED: a program page in host memory (isSys) or unmapped refuses under 88 (disjoint copy notwithstanding)",
               host == N48_HG_IN_PROGRESS && rh == N48_HG88_UNRESOLVED && unm == N48_HG_IN_PROGRESS && k.f.unresolved == 1u);
        n48_hg_frame f {}; f.on = 1u; f.r88 = 1u;
        for (uint32_t i = 0; i < N48_HG88_PG_MAX; i++) n48_hg88_note_page(&f, 0x20000000ull + i * 0x1000ull, 1u);
        const uint32_t notFull = f.unresolved;
        n48_hg88_note_page(&f, 0x20000000ull, 1u); const uint32_t dup = f.unresolved;
        n48_hg88_note_page(&f, 0x30000000ull, 1u);
        expect("T33c the page list: 128 distinct pages fit, a repeat is deduplicated, the 129th sets unresolved (FULL)",
               notFull == 0u && dup == 0u && f.unresolved == 1u && f.npg == N48_HG88_PG_MAX);
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        k.f.pva_over = 1u;                                             // a program VA the list could not hold
        const uint64_t t3 = k_copy_begin(k, kAE40);
        const uint32_t over = k_judge(k), ro = n48_hg88_eval(&k.st, &k.f, nullptr); k_copy_end(k, kAE40, t3);
        expect("T33c pva_over (a program not recorded) refuses under 88", over == N48_HG_IN_PROGRESS && ro == N48_HG88_UNRESOLVED);
    }
    // EVICTION / LOST EVENTS
    {
        // 16 disjoint in-place copies after the snapshot evict the overlapping one that ended after it: refused (lost)
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        k_copy(k, kWsHeap);                                            // overlaps the frame, ends after the snapshot
        for (uint32_t i = 0; i < N48_HG88_RING; i++) k_copy(k, kAE29);
        const uint32_t why = k_judge(k), r = n48_hg88_eval(&k.st, &k.f, nullptr);
        expect("T33c EVICTION: an overlapping copy pushed out of the 16-row ring by later ones is LOST events: refused",
               why == N48_HG_CHANGED && r == N48_HG88_LOST && k.st.c88_evict_end > k.f.snap_gen && k.st.c88_evicted >= 1u);
        // a frame snapshotted AFTER the evictions: they are history (evict_end <= snap): spared
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t = k_copy_begin(k, kAE40);
        const uint32_t later = k_judge(k); k_copy_end(k, kAE40, t);
        expect("T33c ... a frame snapshotted after the evictions is judged on the ring alone (spared)", later == N48_HG_OK);
        // an ACTIVE record evicted (17 copies in progress): lost until it ends, and after it ends for frames that saw it
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        uint64_t toks[N48_HG88_RING + 1u];
        for (uint32_t i = 0; i <= N48_HG88_RING; i++) toks[i] = k_copy_begin(k, kAE29);
        const uint32_t la = k.st.c88_lost_active, ra = n48_hg88_eval(&k.st, &k.f, nullptr);
        for (uint32_t i = 0; i <= N48_HG88_RING; i++) k_copy_end(k, kAE29, toks[i]);
        const uint32_t rb = n48_hg88_eval(&k.st, &k.f, nullptr);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t2 = k_copy_begin(k, kAE40); const uint32_t fresh = k_judge(k); k_copy_end(k, kAE40, t2);
        expect("T33c an ACTIVE record evicted is lost until it ends (lost_active 1), its end is recorded (orphan end), frames that saw "
               "it stay refused, a fresh frame is not", la == 1u && ra == N48_HG88_LOST && rb == N48_HG88_LOST &&
               k.st.c88_lost_active == 0u && k.st.c88_orphan_ends == 1u && fresh == N48_HG_OK);
        // the victim is the closed record that ended FIRST, never an active one while a closed one exists
        n48_hg_state st {}; st.gen = 100u;
        n48_hg_copy_id id {}; id.res = 9u; id.range_ok = 1u;
        uint64_t tk[N48_HG88_RING];
        for (uint32_t i = 0; i < N48_HG88_RING; i++) { st.gen++; tk[i] = n48_hg88_push(&st, &id, 0x1000u, 0x2000u, 1u); }
        st.gen = 200u; n48_hg88_close(&st, tk[5]); st.gen = 150u; n48_hg88_close(&st, tk[9]);
        st.gen = 300u; (void)n48_hg88_push(&st, &id, 0x1000u, 0x2000u, 1u);
        bool nine = false; for (uint32_t i = 0; i < N48_HG88_RING; i++) if (st.c88[i].tok == tk[9]) nine = true;
        expect("T33c the eviction order: a full ring evicts the closed record with the smallest end_gen (150), never an active one",
               !nine && st.c88_evict_end == 150u && st.c88_lost_active == 0u);
    }
    // ACTIVE COUNT CONSISTENCY: a bump with no record (as if the record had been pushed outside the bump's section) is LOST
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        (void)n48_hg_copy_begin(&k.st, 1u);                            // the bump, and no record yet
        const uint32_t r = n48_hg88_eval(&k.st, &k.f, nullptr), why = k_judge(k);
        expect("T33c a bump whose record is not there (active 1, no active record) is LOST events: refused", r == N48_HG88_LOST &&
               why == N48_HG_IN_PROGRESS);
        K g; f_ws(g); k_pass(g, kWsVm, { 0x400029300ull }, kWsPid); g.st.gen += 2u;   // the generation moved, no record says why
        expect("T33c a generation move with no relevant record is LOST (refused)", n48_hg88_eval(&g.st, &g.f, nullptr) == N48_HG88_LOST &&
               k_judge(g) == N48_HG_CHANGED);
    }
    // THE WALK judges the COMMITTED record's pages (not the next pass's)
    {
        f_ws(k);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);                // frame N: programs in WindowServer's heap
        const uint32_t j = k_judge(k); k_record(k);
        const uint64_t recJudged = k.rec.judged;
        k_pass(k, kWsVm, { 0x400565600ull }, kWsPid);                // frame N+1's pass (the next gHgFrame): pages elsewhere
        const uint64_t t = k_copy_begin(k, kWsHeap);                 // a copy over frame N's heap before N's walk
        const uint32_t w = n48_hg_walk_answer(&k.st, &k.rec, &k.pd, 7u, 0x5501u);
        n48_hg_frame wrong = k.f; wrong.judged = recJudged; wrong.snap_gen = k.rec.snap_gen; wrong.snap_active = k.rec.snap_active;
        const uint32_t w2 = n48_hg_judge(&k.st, &wrong, recJudged);
        k_copy_end(k, kWsHeap, t);
        expect("T33c the WALK judges the committed record's pages: frame N is refused for a copy over ITS heap (judged with frame N+1's "
               "pages it would have been spared)", j == N48_HG_OK && w == N48_HG_IN_PROGRESS && w2 == N48_HG_OK);
    }
    // POISON and STALE unchanged under 88
    {
        f_ws(k);
        n48_hg_poison p {}; p.va_lo = 0x400029000ull; p.va_hi = 0x40002a000ull; p.va_ok = 1u; n48_hg_poison_add(&k.st, &p);
        k_pass(k, kWsVm, { 0x400029300ull }, kWsPid);
        const uint64_t t = k_copy_begin(k, kAE40);
        const uint32_t pz = k_judge(k);
        const uint32_t st = n48_hg_judge(&k.st, &k.f, k.judged + 1u);
        k_copy_end(k, kAE40, t);
        expect("T33c poison and STALE are unchanged: a spared move over a poisoned program answers program-poisoned; a stale record "
               "answers stale-verdict", pz == N48_HG_POISONED && st == N48_HG_STALE);
    }
    // 62 OFF latched: 88 does nothing (r88 0) and the answer is OK
    {
        n48_hg_frame f {}; n48_hg_state st {}; st.gen = 3u; st.active = 1u;
        n48_hg_frame_begin(&f, 0u, 1u, &st); n48_hg88_latch(&f, 1u, 1u);
        expect("T33c 62 OFF: 88 never latches (r88 0), the judge answers ok as always", f.r88 == 0u && n48_hg_judge(&st, &f, 1u) == N48_HG_OK);
    }
}

// =============================================================================================================================
// T33d — THE SWEEPS
// =============================================================================================================================
// 0.0.532's n48_hg_judge, verbatim (the OFF identity's reference).
static uint32_t judge532(const n48_hg_state *s, const n48_hg_frame *f, uint64_t judged_now)
{
    if (!f->on) return N48_HG_OK;
    if (f->judged != judged_now) return N48_HG_STALE;
    if ((f->snap_gen & 1ull) || f->snap_active) return N48_HG_IN_PROGRESS;
    if ((s->gen & 1ull) || s->active) return N48_HG_IN_PROGRESS;
    if (s->gen != f->snap_gen) return N48_HG_CHANGED;
    if (n48_hg_frame_poisoned(s, f)) return N48_HG_POISONED;
    return N48_HG_OK;
}
static uint32_t gRng = 0x1234567u;
static uint32_t rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5; return gRng; }

// The copies a sweep draws from: SecurityAgent (another VM, disjoint), WindowServer's heap (the frame's own), AB2's heap (same VM,
// disjoint), a moved SecurityAgent range, a first-copy resource, an untracked copy, a copy over the frame's host page region.
static Cp sweep_copy(uint32_t kind)
{
    switch (kind % 7u) {
    case 0: return kAE29;
    case 1: return kWsHeap;
    case 2: return kAB2_73;
    case 3: { Cp c = kAE29; c.lo = 0x12400000ull; c.hi = 0x12409000ull; return c; }
    case 4: { Cp c = kAE29; c.res = 0x5555u + (rnd() & 0xffu); return c; }
    case 5: { Cp c = kAE29; c.range_ok = 0u; return c; }
    default: { Cp c = kAB2_73; c.pid = kSaPidAE; c.lo = 0x13175000ull; c.hi = 0x1317e000ull; c.res = 0x6666u; return c; }   // over AB2's PS heap, another VM
    }
}
// Ground truth, computed without n48_hg88_eval: could a relevant copy have touched a byte of the frame's programs? VRAM through the
// frame's own page map; VA only in the frame's own VM (same pid, or pid 0). An unknown range or VA is "could".
static bool touches(const Cp &c, std::initializer_list<uint64_t> pgms, int32_t pid)
{
    if (!c.range_ok) return true;
    for (uint64_t va : pgms)
        for (uint64_t b = va; b < va + N48_HG88_PGM_BYTES; b += 0x100u) {
            uint64_t off = 0; bool sys = false;
            if (!vm_page(kWsVm, b & ~0xfffull, off, sys) || sys) return true;          // cannot say where it is: could
            off += b & 0xfffull;
            if (off >= c.lo && off < c.hi) return true;
            if ((c.pid == pid || c.pid == 0) && (!c.va_ok || (b >= c.va && b < c.va + c.bytes))) return true;
        }
    return false;
}
static void test_sweeps()
{
    char b[420];
    // (1) 88 OFF (r88 0) == 0.0.532 over random states and frames (with records present: they must not matter)
    uint32_t n1 = 0u, d1 = 0u;
    for (uint32_t i = 0; i < 40000u; i++) {
        n48_hg_state st {}; n48_hg_frame f {};
        st.gen = rnd() & 0x3fu; st.active = rnd() & 3u; if (rnd() & 1u) st.poison_over = (rnd() & 7u) == 0u;
        if ((rnd() & 3u) == 0u) { n48_hg_poison p {}; p.va_lo = 0x400029000ull; p.va_hi = 0x400029400ull; p.va_ok = rnd() & 1u; n48_hg_poison_add(&st, &p); }
        for (uint32_t r = 0; r < (rnd() & 7u); r++) { n48_hg_copy_id id {}; id.res = rnd() & 3u; id.range_ok = rnd() & 1u; (void)n48_hg88_push(&st, &id, 0x10020000ull, 0x10029000ull, rnd() & 1u); }
        const uint32_t on = (rnd() & 7u) != 0u;
        n48_hg_frame_begin(&f, on, 7u, &st);
        n48_hg88_latch(&f, 0u, 1u);
        f.snap_gen = rnd() & 0x3fu; f.snap_active = (rnd() & 3u) == 0u;
        for (uint32_t p = 0; p < (rnd() & 3u); p++) n48_hg_frame_note_pva(&f, (rnd() & 1u) ? 0x400029300ull : 0x400565600ull);
        if ((rnd() & 15u) == 0u) f.pva_over = 1u;
        const uint64_t now = 7u + ((rnd() & 7u) == 0u);
        n1++;
        if (n48_hg_judge(&st, &f, now) != judge532(&st, &f, now)) d1++;
    }
    std::snprintf(b, sizeof b, "T33d 88 OFF (r88 0): n48_hg_judge equals 0.0.532's verbatim judge in %u random states (%u differ)", n1, d1);
    expect(b, n1 == 40000u && d1 == 0u);
    // (2) the sequences: before-pass / in-pass / after-judge copies of every kind, the walk with or without the copy complete
    uint32_t runs = 0u, unsafe = 0u, spares = 0u, refusedSafe = 0u, offDiff = 0u, postStartDiff = 0u;
    const std::initializer_list<uint64_t> pgmSets[] = { { 0x400029300ull, 0x400029100ull }, { 0x400029300ull, 0x400565600ull },
                                                        { 0x400565600ull }, { 0x400029300ull, 0x400700100ull } };
    for (uint32_t ps = 0; ps < 4u; ps++)
        for (uint32_t kind1 = 0; kind1 < 7u; kind1++)
            for (uint32_t kind2 = 0; kind2 < 8u; kind2++)          // 7 = no second copy
                for (uint32_t shape = 0; shape < 6u; shape++)
                    for (uint32_t pidSel = 0; pidSel < 2u; pidSel++) {
                        const int32_t pid = pidSel ? kWsPid : kSaPidAE;
                        uint32_t ans[3][2];   // [on88 0 / 1 / 1 post-START][judge, walk]
                        for (uint32_t v = 0; v < 3u; v++) {
                            K k; k_init(k, v ? 1u : 0u, v == 2u ? 0u : 1u);
                            k.st.gen = 20u;
                            gRng = 0x9e3779b9u ^ (ps * 131u + kind1 * 17u + kind2 * 7u + shape);
                            k_copy(k, kWsHeap); k_copy(k, kAE29); k_copy(k, kAB2_73);   // every heap's first copy
                            const Cp c1 = sweep_copy(kind1), c2 = sweep_copy(kind2);
                            uint64_t t1 = 0, t2 = 0;
                            if (shape == 0) t1 = k_copy_begin(k, c1);                 // active at the snapshot
                            if (shape == 1) k_copy(k, c1);                            // complete before the pass
                            k_pass(k, kWsVm, pgmSets[ps], pid);
                            if (shape == 2 || shape == 3) t1 = k_copy_begin(k, c1);   // in the pass, before the judge
                            if (shape == 4) k_copy(k, c1);                            // begun and ended in the pass
                            const uint32_t j = k_judge(k); k_record(k);
                            if (shape == 5) t1 = k_copy_begin(k, c1);                 // after the judge, before the walk
                            if (kind2 < 7u) t2 = k_copy_begin(k, c2);
                            if (shape == 3) k_copy_end(k, c1, t1);                    // ended before the walk
                            const uint32_t w = j == N48_HG_OK ? k_walk(k) : 0xffu;
                            if (shape == 0 || shape == 2 || shape == 5) k_copy_end(k, c1, t1);
                            if (kind2 < 7u) k_copy_end(k, c2, t2);
                            ans[v][0] = j; ans[v][1] = w;
                        }
                        runs++;
                        // ground truth: which copies are relevant to the judge (all but shape 1's) and to the walk (all but shape 1's)
                        const Cp c1 = sweep_copy(kind1); const Cp c2 = sweep_copy(kind2);
                        const bool rel1 = shape != 1u, t1 = rel1 && touches(c1, pgmSets[ps], pid);
                        const bool judgeT = (shape != 5u) && t1;                         // the judge sees c1 unless it came after it
                        const bool walkT = t1 || (kind2 < 7u && touches(c2, pgmSets[ps], pid));
                        const bool moved0j = ans[0][0] == N48_HG_IN_PROGRESS || ans[0][0] == N48_HG_CHANGED;
                        if (ans[1][0] == N48_HG_OK && moved0j) { spares++; if (judgeT) unsafe++; }
                        if (ans[1][1] == N48_HG_OK && ans[0][1] != N48_HG_OK && walkT) unsafe++;
                        if (ans[1][0] != N48_HG_OK && moved0j && !judgeT && ps < 2u && (kind1 == 0u || kind1 == 2u) && pid == kWsPid && shape != 5u && kind2 == 7u)
                            refusedSafe++;
                        if (ans[1][0] != N48_HG_OK && ans[0][0] == N48_HG_OK) offDiff++;                 // 88 never refuses what OFF spares
                        if (ans[1][1] != N48_HG_OK && ans[1][1] != 0xffu && ans[0][1] == N48_HG_OK) offDiff++;
                        if (ans[2][0] != ans[0][0] || ans[2][1] != ans[0][1]) postStartDiff++;
                    }
    std::snprintf(b, sizeof b, "T33d SWEEP over %u sequences (4 program sets x 7 x 8 copy kinds x 6 timings x 2 pids): 88 spared %u moved "
                  "frames; a spare where a relevant copy could touch a program (ground truth, computed apart): %u; 88 refused what OFF "
                  "spared: %u", runs, spares, unsafe, offDiff);
    expect(b, runs == 2688u && spares > 100u && unsafe == 0u && offDiff == 0u);
    std::snprintf(b, sizeof b, "T33d after CONTINUOUS START 88 ON answers exactly as 88 OFF in every sequence (%u differ); disjoint "
                  "same-VM copies of the other heaps refused anyway: %u (0 expected: AB2's case)", postStartDiff, refusedSafe);
    expect(b, postStartDiff == 0u && refusedSafe == 0u);
}

// =============================================================================================================================
// T33e — THE LINES AND THE KEXT's WIRING
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
static std::string slurp(const char *p)
{
    std::ifstream in(p ? p : "");
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}
static const char kFmtSwitch[] = "heapgen88: switch 88 is %s%s; copy records pushed %llu (untracked %llu), evicted %llu (active %u), orphan ends %llu";

static void test_lines_and_wiring(const char *ahhPath, const char *peerPath, const char *cmPath)
{
    char line[1400];
    n48_hg_state big; std::memset(&big, 0xff, sizeof big);
    const int n1 = std::snprintf(line, sizeof line, N48_HG88_FMT, N48_HG88_ARGS(1u, &big));
    const int n2 = std::snprintf(line, sizeof line, N48_HG88_RESCUE_FMT, ~0ull, "generation-changed", ~0ull, ~0ull, (int)0x80000000,
                                 (int)0x80000000, 0xffffffffu, ~0ull, ~0ull, 0xffffffffu);
    const int n3 = std::snprintf(line, sizeof line, kFmtSwitch, "ON (344, the start-up window only)",
                                 " - `gfxneuter 88` REFUSED: a continuous arm stands, unchanged", ~0ull, ~0ull, ~0ull, 0xffffffffu, ~0ull);
    std::snprintf(line, sizeof line, "T33e the heapgen88 lines at their widest fit n48log's 491-byte body: counts %d, rescue %d, switch %d",
                  n1, n2, n3);
    expect(line, n1 > 0 && n1 <= 491 && n2 > 0 && n2 <= 491 && n3 > 0 && n3 <= 491);
    std::snprintf(line, sizeof line, N48_HG88_FMT, N48_HG88_ARGS(0u, &big));
    expect("T33e the counts line is the design's, verbatim (M2 when OFF)", std::strncmp(line, "heapgen88: M2; judged ", 22) == 0 &&
           std::strcmp(N48_HG88_FMT, "heapgen88: M%u; judged %llu; spared disjoint in-progress/changed %llu/%llu; refused overlap %llu, "
                                     "not-in-place %llu, unresolved %llu, lost %llu, untracked %llu; walk spared %llu") == 0);

    const std::string ahh = slurp(ahhPath), peer = slurp(peerPath), cm = slurp(cmPath);
    expect("T33e the kext sources were given and read (AppleHardwareHook.cpp, Navi48AccelPeer.cpp, gfx_commit.h)",
           !ahh.empty() && !peer.empty() && !cm.empty());
    if (ahh.empty() || peer.empty() || cm.empty()) return;
    const std::string cb = fn_text(ahh, "uint32_t hw_hg_copy_begin(uint64_t vramLo, uint64_t vramHi, const n48_hg_patch *p, uint32_t np, uint32_t npoison, uint64_t *regSeqOut,");
    const std::string ce = fn_text(ahh, "void hw_hg_copy_end(uint32_t clean, uint32_t vaOk, uint64_t vaLo, uint64_t vaHi, uint64_t vramLo, uint64_t vramHi,");
    const std::string rc = fn_text(ahh, "static __attribute__((noinline)) void hg88_copy_record(");
    const std::string fb = fn_text(ahh, "static __attribute__((noinline)) void hg_frame_begin_pass()");
    const std::string cj = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_commit_judge(uint32_t wouldCommit)");
    const std::string jn = fn_text(ahh, "static __attribute__((noinline)) void hg88_judge_note(uint32_t wouldCommit)");
    const std::string rl = fn_text(ahh, "static __attribute__((noinline)) void hg88_rescue_line()");
    const std::string cr = fn_text(ahh, "static __attribute__((noinline)) void hg_commit_record(uint32_t gateSeq)");
    const std::string ex = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_exempt_refuse(uint32_t seq)");
    const std::string np = fn_text(ahh, "static __attribute__((noinline)) void hg88_note_program(const GfxcVm &vm, uint64_t va, int32_t pid)");
    const std::string id = fn_text(ahh, "static void gfxsrc_identify(const GfxcVm &vm, uint64_t va, uint32_t stage, int32_t pid, n48_xv_program *p) {");
    const std::string rep = fn_text(ahh, "static void hg88_report_line(const char *why)");
    const std::string icb = fn_text(peer, "static __attribute__((noinline)) void ic_begin(");
    const std::string icc = fn_text(peer, "__attribute__((noinline)) void navi48_ic_scope_closed(");
    expect("T33e the functions are found", !cb.empty() && !ce.empty() && !rc.empty() && !fb.empty() && !cj.empty() && !jn.empty() &&
           !rl.empty() && !cr.empty() && !ex.empty() && !np.empty() && !id.empty() && !rep.empty() && !icb.empty() && !icc.empty());
    // the copier: the record in the BUMP's OWN gHgLock section, right after the bump; the close after the end bump
    expect("T33e copier: lock -> bump -> hg88_copy_record (the record, same section) -> unlock; the record helper is the pure note",
           order(cb, { "IOLockLock(l);", "const uint32_t bumped = n48_hg_copy_begin(&gHg, overlaps);",
                       "hg88_copy_record(bumped, vramLo, vramHi, id);", "IOLockUnlock(l);" }) &&
           has(rc, "const uint64_t tok = n48_hg88_copy_note(&gHg, bumped, vramLo, vramHi, id);") && has(rc, "if (id) id->tok = tok;") &&
           !has(rc, "IOLock") && count_of(ahh, "hg88_copy_record(bumped") == 1u);
    expect("T33e copier: the close follows n48_hg_copy_end in hw_hg_copy_end's section, by the copy's token",
           order(ce, { "IOLockLock(l);", "n48_hg_copy_end(&gHg, clean, vaOk, vaLo, vaHi, vramLo, vramHi, add, nadd);",
                       "n48_hg88_close(&gHg, tok88);", "IOLockUnlock(l);" }));
    expect("T33e copier (Navi48AccelPeer.cpp): the slot's identity (resource = residency_copy_to_vram's self, VA, bytes, pid) is set "
           "before the bump call, passed, and its token closes the copy; the untracked copy passes no identity",
           order(icb, { "s->hgId.res = reinterpret_cast<uintptr_t>(res);", "s->hgId.pid = proc_selfpid(); s->hgId.va_ok = s->vaOk; s->hgId.range_ok = 1u;",
                        "s->bumped = n48::hw_hg_copy_begin(cgLo, cgHi, s->plan ? s->plan->patch : nullptr, np, npo, &s->regSeq, mayWait, &s->hgId);" }) &&
           has(icb, "(void)n48::hw_hg_copy_begin(cgLo, cgHi, nullptr, 0u, 1u, nullptr, 0u, nullptr);") &&
           has(icc, "np, s->regSeq, s->hgId.tok);") &&
           has(peer, "if (n48::hw_hg_on()) ic_begin(dstMem, dSeg, md, backingOffset, bytes, (retile || wBytes != bytes) ? 1u : 0u, cgLo, cgHi, dstMap, self);"));
    // the frame side
    expect("T33e the pass latch: after 62's frame_begin, 88 ON and the start-up window (hg_startup_now), in hg_frame_begin_pass only",
           order(fb, { "n48_hg_frame_begin(&gHgFrame, 1u, gXdC.judged + 1u, &gHg);", "IOLockUnlock(gHgLock);",
                       "n48_hg88_latch(&gHgFrame, gHg88On ? 1u : 0u, hg_startup_now());" }) && count_of(ahh, "n48_hg88_latch(") == 1u);
    expect("T33e gfxsrc_identify: the pages (88 latched) BEFORE the VA is recorded and BEFORE the memo's identification",
           order(id, { "if (gHgFrame.r88) hg88_note_program(vm, va, pid);", "if (gHgFrame.on) n48_hg_frame_note_pva(&gHgFrame, va);",
                       "gfxsrc_identify_pgm(vm, va, stage, pid, p);" }));
    expect("T33e hg88_note_program: pid, then the window's pages through gfxc_page; a HOST page (isSys) or an unmappable one is "
           "UNRESOLVED; VRAM offsets by vmib_to_vram_off; no gHgLock",
           order(np, { "n48_hg88_note_pid(&gHgFrame, pid);", "if (n48_hg88_va_known(&gHgFrame, va)) return;",
                       "const uint64_t off = (gfxc_page(vm, pg, base, sys) && !sys) ? vmib_to_vram_off(base, vm.fbStart, vm.vramSize, ok) : 0ull;",
                       "n48_hg88_note_page(&gHgFrame, off, ok ? 1u : 0u);" }) && !has(np, "gHgLock") &&
           has(ahh, "static_assert(N48_HG88_PGM_BYTES == kXdPgmDwords * 4u,"));
    // the judge and the walk: the same judge (n48_hg_judge, unchanged call sites); the counts are arithmetic under gHgLock
    expect("T33e the commit judge: n48_hg_judge, then (88 latched) hg88_judge_note in the same gHgLock section, then 72's steps; the "
           "rescue line after the unlock",
           order(cj, { "IOLockLock(gHgLock);", "const uint32_t why = n48_hg_judge(&gHg, &gHgFrame, gXdC.judged + 1u);",
                       "if (gHgFrame.r88) hg88_judge_note(wouldCommit);", "if (gHgPend.m3) gHgPend.startup = hg_startup_now();",
                       "IOLockUnlock(gHgLock);", "if (gHg88Rescue) hg88_rescue_line();" }));
    expect("T33e the walk judges the COMMITTED record (gHgCommit, a struct copy of this pass's gHgFrame: its pages) and counts a spare",
           has(ex, "const uint32_t why = n48_hg_walk_answer(&gHg, &gHgCommit, &gHgPend, seq, reinterpret_cast<uintptr_t>(current_thread()));") &&
           has(ex, "if (why == N48_HG_OK && gHgCommit.r88 && n48_hg88_rescued(&gHg, &gHgCommit)) gHg.j88_walk_spared++;") &&
           has(cr, "gHgCommit = gHgFrame;") && !has(ex, "gHgFrame"));
    // 88 never touches switch 72's state
    expect("T33e 88 never reads or writes switch 72's PRE/LIVE state (gHgPend) in any of its helpers",
           !has(jn, "gHgPend") && !has(rl, "gHgPend") && !has(rc, "gHgPend") && !has(np, "gHgPend") && !has(rep, "gHgPend") &&
           count_of(ahh, "n48_hg_pend_set(") == 1u);
    // the switch: default OFF, n48_ra_set, the mid-arm guard (in the kext and in gfx_commit.h's list)
    expect("T33e switch 88 boots OFF; `88 | M << 8` via n48_ra_set (M1 344 ON, M2 600 OFF); mid-arm guarded; the report line",
           has(ahh, "static volatile uint32_t gHg88On { 0u };") && has(ahh, "} else if ((arg & 0xffull) == 88ull) {") &&
           has(ahh, "else { changed88 = n48_ra_set(m, &f88); if (changed88) { gHg88On = f88; if (f88) gHg88Ever = 1u; } }") &&
           has(ahh, "const bool contRefused88 = n48_cm_cont_switch_refused(88u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") &&
           has(cm, "    case 88u:   /* build 0.0.533") && (88u | 1u << 8) == 344u && (88u | 2u << 8) == 600u &&
           has(rep, "HWLOG(N48_HG88_FMT, N48_HG88_ARGS(gHg88On, &gHg));") &&
           has(ahh, "HWLOG(\"heapgen88: switch 88 is %s%s; copy records pushed %llu (untracked %llu), evicted %llu (active %u), orphan ends %llu\","));
    expect("T33e the rescue line uses the header's format; it is printed only after the unlock",
           has(rl, "HWLOG(N48_HG88_RESCUE_FMT,") && !has(cj.substr(0, cj.find("IOLockUnlock(gHgLock);")), "hg88_rescue_line"));
}

int main(int argc, char **argv)
{
    test_real();
    test_sched();
    test_rules();
    test_sweeps();
    test_lines_and_wiring(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("gfx_hg88: %d check(s), %d failed\n", gChecks, gFails);
    std::printf("gfx_hg88: %s\n", gFails ? "N48-HG88-TEST-FAIL" : "N48-HG88-TEST-PASS");
    return gFails ? 1 : 0;
}
