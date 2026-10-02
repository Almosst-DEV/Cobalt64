// gfx_walknop75_test.cpp — build 0.0.519: switch 75, gfx_walknop75.h + the flight ring's NOPED state,
// driven over run10w's REAL token seq 3 (RUN P), and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_walknop75_test.cpp -o /tmp/wn75 && /tmp/wn75 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_commit.h
//     (gfx_dep.h is read from beside AppleHardwareHook.cpp, or from an optional third argument)
// run10w (notes/logs/runs/run10w/driverlog-stream.txt): the frame [2176..2304) = ring dwords 0x880..0x900, IB 0 expected at 0x8ad
// (`record IN FLIGHT, IB 0 expected at ring dword 0x8ad`), `INDIRECT_BUFFER(0x3f) cnt 2: 000d0000 00000004 02000410` (VA
// 0x4000d0000, 1040 dw, VMID 2), token seq 3, fence828 ordinal 1 epoch 0xadd9 (`writes 0xadd90001 to OUR owned slot 1 ...
// vram+0x3cba8f004`), `RING EXEMPTION for token seq 3: heap-gen - spared 0 of 1 IB(s)`, `1 NOPed IN THE RING (first header
// 0xc0023f00 -> 0xc0021000 ...) with 0 read-back mismatch(es), 0 no longer an IB, 0 over the per-frame cap; CP_RB0_RPTR 0x880
// WPTR 0x880 before the doorbell`; the first unmap deferred `oldest blocking seq 3 committed 32042 us ago` (fire #30) and the
// withdrawal applied at fire #260, `2019980 of 2000000 us bound`, commit stamp 1079121991 -> CONTINUOUS STOP (stop_why 3).
// Covers:
//   W1 the walk's proof on run10w's ring: the real walk (n48_gfxn_walk), the real exemption answer (heap-gen), the NOP pass as
//      gfx_neuter_frame makes it -> proven; each missing clause refuses it (raced, a read-back mismatch, not-an-IB, over the cap, a
//      stopped walk, the IB not in the list, the body naming another VA/length/VMID, the header not NOPed, SPARED, none in flight,
//      no position, a multi-IB frame with one IB left unNOPed);
//   W2 run10w's sequence through the REAL flight ring, hook_unmapVA's verdict block and the continuous stop: switch ON -> the
//      flight is NOPED at once, fire #30 and every later unmap withdraw with nothing live, withdrawnWhileLive stays 0, the arm
//      keeps going; OFF -> 0.0.518: NOT_RUN, 230 fires deferred, TIMEOUT at 2019980 us with a live entry, stop_why 3;
//   W3 the plan: RETIRE only for a proven, never-spared, refused frame with spared 0 of N; a PARTIALLY spared frame (k of N,
//      0 < k < N), a spared-once record, a proof for another seq, a failed walk and OFF all KEEP; the un-feed is asked exactly
//      when today asks it (spared 0) in every case, and it reaches the queue (n48_dl_unfeed_queue) for token seq 3;
//   W4 the ring: n48_fr_retire_nopped moves only COMMITTED; NOPED is not live, never polled, never expires, is reclaimed;
//      the newest-flight names are restored;
//   W5 switch 73: the NOPed P's final outcome is WITHDRAWN (held);
//   W6 the kext glue: 75 OFF at boot, set only by its verb (mid-arm guarded); the proof after the NOP pass and before Apple's
//      writeTail; the record's proof cleared where the record is set up and read where exWhy is read; the plan before the ring
//      call; the un-feed through the plan; the report line bound;
//   W7 build 0.0.520: the strict CP witness - an unreadable CP_RB0_WPTR (before or after the NOP), a
//      WPTR past the frame's start or END, and a publish between the pre-NOP and post-NOP reads each fail the proof with their
//      own reason; the clean case (run10w's 0x880) and a 32-bit wrap pass; n48_cp_told_past unchanged.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_walknop75.h"
#include "gfx_flightring.h"
#include "gfx_desc_port.h"
#include "gfx_present73.h"
#include "gfx_commit.h"

// gfx_dep.h's n48_cp_told_past, VERBATIM (W6 pins that gfx_dep.h still holds this exact body: its callers' semantics unchanged).
static inline uint32_t n48_cp_told_past_copy(uint32_t cp_wptr, uint64_t mark, uint64_t wptr)
{
    if (cp_wptr == 0xFFFFFFFFu || wptr <= mark) return 0u;
    const int32_t ahead  = (int32_t)(cp_wptr - (uint32_t)mark);
    const int32_t within = (int32_t)((uint32_t)wptr - cp_wptr);
    return (ahead > 0 && within >= 0) ? 1u : 0u;
}

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}

// ---- run10w's ring -------------------------------------------------------------------------------------------------------
static const uint32_t kSize = 131072u;          // Apple's GFX ring: 0x80000 bytes
static const uint32_t kFrom = 0x880u, kN = 0x80u, kIb0 = 0x8adu;
static const uint64_t kVa = 0x4000d0000ull;
static const uint32_t kLen = 1040u, kCtl = 0x02000410u, kSeq = 3u;
static uint32_t gRing[kSize];

static void build_frame(uint32_t *g, uint32_t nib)
{
    for (uint32_t i = 0; i < kN; i++) g[kFrom + i] = N48_TMPL_NOP;
    // the wrapper's own packets around the IB list (types as run10w's census names them), count-correct
    g[kFrom + 0x00] = 0xc0032200u; g[kFrom + 0x01] = 0x1cu; g[kFrom + 0x02] = 0x84u; g[kFrom + 0x03] = 0u; g[kFrom + 0x04] = 0x79u;
    for (uint32_t k = 0; k < nib; k++) {
        const uint32_t p = kIb0 + 4u * k;
        g[p] = 0xc0023f00u; g[p + 1u] = (uint32_t)((kVa + 0x10000ull * k) & 0xffffffffu); g[p + 2u] = (uint32_t)((kVa + 0x10000ull * k) >> 32);
        g[p + 3u] = kCtl;
    }
    g[kFrom + 0x3d] = 0xc0064900u;                                 // RELEASE_MEM cnt 6
    for (uint32_t i = 1; i <= 7u; i++) g[kFrom + 0x3d + i] = 0x10u + i;
}
static n48_gfxn_exempt_t record(uint32_t nib)
{
    n48_gfxn_exempt_t ex {};
    ex.inflight = 1u; ex.seq = kSeq; ex.gate_seq = kSeq; ex.nib = nib; ex.same_thread = 1u; ex.pos_known = 1u;
    ex.expect_pos = kIb0; ex.va = kVa; ex.len = kLen; ex.stamp = 0x12u; ex.vmid = 2u; ex.base_valid = 1u;
    ex.mib = nib > 1u ? 1u : 0u;
    for (uint32_t k = 0; k < nib; k++) { ex.va_k[k] = kVa + 0x10000ull * k; ex.len_k[k] = kLen; }
    ex.heap_refuse = 1u;                                           // run10w: heap-gen
    return ex;
}
// gfx_neuter_frame's loop (AppleHardwareHook.cpp), over the model ring: NOP each listed IB header, read back.
static n48_wn75_pass nop_pass(uint32_t *g, const uint32_t *pos, uint32_t npos, uint32_t found, uint32_t stopped, uint32_t raced,
                              uint32_t skipK)
{
    n48_wn75_pass p {};
    for (uint32_t k = 0; k < npos; k++) {
        if (k == skipK) continue;
        const uint32_t nop = n48_gfxn_nop_for(g[pos[k]]);
        if (!nop) { p.not_ib++; continue; }
        g[pos[k]] = nop;
        if (g[pos[k]] == nop) p.done++; else p.bad++;
    }
    p.npos = npos; p.over = found > npos ? found - npos : 0u; p.stopped = stopped; p.raced = raced;
    return p;
}
struct Walk { n48_gfxn_walk_t w; uint32_t pos[2u * N48_GFXN_MAX_IBS]; uint32_t npos, spared, why; n48_wn75_pass pass; };
static Walk walk_and_nop(uint32_t nib, n48_gfxn_exempt_t ex, uint32_t skipK = 0xffu)
{
    Walk r {};
    std::memset(gRing, 0, sizeof gRing);
    build_frame(gRing, nib);
    n48_gfxn_walk(gRing, kFrom, kN, kSize, &r.w, nullptr, nullptr);
    r.npos = n48_gfxn_nop_list(gRing, kSize, kN, &r.w, N48_GFXN_ARM_COMMIT, &ex, r.pos, &r.spared, &r.why);
    r.pass = nop_pass(gRing, r.pos, r.npos, r.w.ibs, r.w.stop_at != kN ? 1u : 0u, 0u, skipK);
    return r;
}

// ---------------------------------------------------------------------------------------------------------------- W1
static void w1_proof()
{
    const n48_gfxn_exempt_t ex = record(1u);
    Walk a = walk_and_nop(1u, ex);
    expect_u("W1 run10w: the walk finds the one VMID-2 IB at 0x8ad", a.w.ibs == 1u && a.w.npos2 == 1u && a.w.pos2[0] == kIb0, 1u);
    expect_u("W1 ... the exemption answers heap-gen (the frame would otherwise be SPARED)", a.why, (uint64_t)N48_GFXN_EX_HEAPGEN);
    expect_u("W1 ... the NOP list holds it; the pass NOPed it (0xc0023f00 -> 0xc0021000) with a clean read-back",
             a.npos == 1u && gRing[kIb0] == 0xc0021000u && a.pass.done == 1u && a.pass.bad == 0u, 1u);
    expect_u("W1 THE PROOF: every IB of the committed frame NOPed before the doorbell",
             n48_wn75_proof(gRing, kSize, &ex, a.why, a.pos, a.npos, &a.pass), 1u);
    expect_u("W1 the NOP header skips the whole 3-dword IB body (type 3, NOP, count 2): no part of the IB can be fetched",
             n48_wn75_is_ib_nop(gRing[kIb0]) && ((gRing[kIb0] >> 16) & 0x3fffu) == 2u && ((gRing[kIb0] >> 8) & 0xffu) == 0x10u, 1u);
    char l[160];
    // each clause, one at a time
    struct { const char *n; int f; } cl[] = {
        { "raced (the CP was told about the frame before the NOP)", 1 }, { "a read-back mismatch", 2 }, { "a position no longer an IB", 3 },
        { "an IB over the per-frame cap", 4 }, { "the walk stopped", 5 }, { "done != npos", 6 }, { "the IB not in the NOP list", 7 },
        { "the body names another VA", 8 }, { "the body names another length", 9 }, { "the body names another VMID", 10 },
        { "the header was not NOPed (still an IB)", 11 }, { "the answer was SPARED", 12 }, { "no record in flight (NONE)", 13 },
        { "no known position", 14 }, { "seq 0", 15 }, { "inflight 0", 16 } };
    for (auto &c : cl) {
        n48_gfxn_exempt_t e = ex; Walk b = walk_and_nop(1u, ex); uint32_t why = b.why; n48_wn75_pass p = b.pass;
        uint32_t pos[2u * N48_GFXN_MAX_IBS]; std::memcpy(pos, b.pos, sizeof pos); uint32_t np = b.npos;
        switch (c.f) {
        case 1: p.raced = 1u; break;              case 2: p.bad = 1u; break;         case 3: p.not_ib = 1u; break;
        case 4: p.over = 1u; break;               case 5: p.stopped = 1u; break;     case 6: p.done = 0u; break;
        case 7: pos[0] = kIb0 + 4u; break;        case 8: gRing[kIb0 + 1u] ^= 0x1000u; break;
        case 9: gRing[kIb0 + 3u] = 0x02000400u; break; case 10: gRing[kIb0 + 3u] = 0x03000410u; break;
        case 11: gRing[kIb0] = 0xc0023f00u; break; case 12: why = N48_GFXN_EX_SPARED; break; case 13: why = N48_GFXN_EX_NONE; break;
        case 14: e.pos_known = 0u; break;         case 15: e.seq = 0u; break;        case 16: e.inflight = 0u; break;
        }
        std::snprintf(l, sizeof l, "W1 not proven when %s", c.n);
        expect_u(l, n48_wn75_proof(gRing, kSize, &e, why, pos, np, &p), 0u);
    }
    // multi-IB: all or nothing
    const n48_gfxn_exempt_t e2 = record(2u);
    Walk m = walk_and_nop(2u, e2);
    expect_u("W1 a 2-IB frame (mib), both IBs NOPed: proven", m.why == N48_GFXN_EX_HEAPGEN &&
             n48_wn75_proof(gRing, kSize, &e2, m.why, m.pos, m.npos, &m.pass) == 1u, 1u);
    Walk m1 = walk_and_nop(2u, e2, 1u);   // the pass skipped IB 1: it would still run
    expect_u("W1 a 2-IB frame with IB 1 left an IB: NOT proven", n48_wn75_proof(gRing, kSize, &e2, m1.why, m1.pos, m1.npos, &m1.pass), 0u);
    n48_gfxn_exempt_t e3 = e2; e3.mib = 0u;
    expect_u("W1 nib 2 without mib: NOT proven", n48_wn75_proof(gRing, kSize, &e3, m.why, m.pos, m.npos, &m.pass), 0u);
}

// ---------------------------------------------------------------------------------------------------------------- W7
// build 0.0.520 (MEDIUM-1 of the 0.0.519 review): the STRICT CP witness. run10w's frame starts at 0x880 and ends
// at 0x900; the CP read WPTR 0x880 before the doorbell. The old race predicate (n48_cp_told_past) answered "not told" for an
// unreadable register and for a register past the frame's END, and one pre-NOP read could not see a publish between it and the
// NOP stores. The proof now needs the pre-NOP read AND a post-NOP read, each readable and not past the frame's start.
static void w7_strict()
{
    const n48_gfxn_exempt_t ex = record(1u);
    Walk a = walk_and_nop(1u, ex);
    const uint64_t from = kFrom, wend = kFrom + kN;
    struct { const char *n; uint32_t cp0, cp1, want, why; } cs[] = {
        { "clean: WPTR 0x880 before and after (run10w)",                         0x880u, 0x880u, 1u, (uint32_t)N48_WN75_PFS },
        { "clean: WPTR behind the frame (the previous frame not yet told)",       0x87cu, 0x880u, 1u, (uint32_t)N48_WN75_PFS },
        { "clean across the 32-bit wrap (from 0xfffffff0, cp 0xffffffe0)",        0u,     0u,     1u, (uint32_t)N48_WN75_PFS },
        { "UNREADABLE before the NOP (0xFFFFFFFF)",                               0xFFFFFFFFu, 0x880u, 0u, (uint32_t)N48_WN75_PF_CP0_UNREADABLE },
        { "UNREADABLE after the NOP (0xFFFFFFFF)",                                0x880u, 0xFFFFFFFFu, 0u, (uint32_t)N48_WN75_PF_CP1_UNREADABLE },
        { "WPTR past the frame END before the NOP (0x904)",                       0x904u, 0x904u, 0u, (uint32_t)N48_WN75_PF_CP0_PAST },
        { "WPTR inside the frame before the NOP (0x881)",                         0x881u, 0x881u, 0u, (uint32_t)N48_WN75_PF_CP0_PAST },
        { "a PUBLISH BETWEEN THE READS: 0x880 before, 0x900 (the frame end) after", 0x880u, 0x900u, 0u, (uint32_t)N48_WN75_PF_CP1_PAST },
        { "a publish between the reads to PAST the frame end (0x904 after)",      0x880u, 0x904u, 0u, (uint32_t)N48_WN75_PF_CP1_PAST },
        { "across the 32-bit wrap, a publish between the reads (after wraps to 0x10)", 1u, 1u, 0u, (uint32_t)N48_WN75_PF_CP1_PAST },
    };
    char l[200];
    for (auto &c : cs) {
        n48_wn75_pass p = a.pass;
        uint64_t f = from; uint32_t cp0 = c.cp0, cp1 = c.cp1;
        if (c.cp0 == 0u && c.cp1 == 0u) { f = 0x1fffffff0ull; cp0 = 0xffffffe0u; cp1 = 0xfffffff0u; }
        if (c.cp0 == 1u && c.cp1 == 1u) { f = 0x1fffffff0ull; cp0 = 0xfffffff0u; cp1 = 0x10u; }
        p.cp0 = cp0;
        uint32_t why = 0xabu;
        const uint32_t ok = n48_wn75_proof_strict(gRing, kSize, &ex, a.why, a.pos, a.npos, &p, f, cp1, &why);
        std::snprintf(l, sizeof l, "W7 %s: %s", c.n, c.want ? "proven" : "NOT proven, reason counted");
        expect_u(l, ok == c.want && why == c.why, 1u);
    }
    // the old predicate is unchanged and blind to exactly these: an unreadable register and a WPTR past the END read "not raced"
    expect_u("W7 n48_cp_told_past unchanged: unreadable -> 0 (so it cannot be the proof's witness)",
             n48_cp_told_past_copy(0xFFFFFFFFu, from, wend), 0u);
    expect_u("W7 n48_cp_told_past unchanged: WPTR past the frame END -> 0", n48_cp_told_past_copy(0x904u, from, wend), 0u);
    expect_u("W7 n48_cp_told_past unchanged: WPTR inside the frame -> 1", n48_cp_told_past_copy(0x881u, from, wend), 1u);
    // the strict proof still needs the whole old proof: a clean CP with a stopped walk is not proven, reason PASS
    n48_wn75_pass p = a.pass; p.cp0 = 0x880u; p.stopped = 1u;
    uint32_t why = 0u;
    expect_u("W7 clean CP reads but the old proof fails (walk stopped): NOT proven, reason PASS",
             n48_wn75_proof_strict(gRing, kSize, &ex, a.why, a.pos, a.npos, &p, from, 0x880u, &why) == 0u && why == N48_WN75_PF_PASS, 1u);
    // the reason feeds the plan exactly like any failure: KEEP (today's NOT_RUN)
    n48_wn75_rec r {}; n48_wn75_note(&r, kSeq, 0u);
    expect_u("W7 a strict-witness failure -> the plan KEEPs (NOT_RUN, the 2 s backstop stands)",
             n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 0u, 1u, 0xFFFFFFFFu, &r).action, (uint64_t)N48_WN75_KEEP);
    // the report line
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    const int w = std::snprintf(b, sizeof b, N48_WN75_PF_FMT, M, M, M, M, M);
    expect_u("W7 the reasons line <= 491 bytes at 20-digit counters", w > 0 && w <= 491, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- W2
static const uint64_t kBound = 2000000ull, kAt = 1079121991ull;
struct U { uint32_t kdv, anyLive, deferNow, wwl; };
// hook_unmapVA's verdict block (kdArmed 1, switch 22 ON, switch 65 OFF as in run10w's recipe): scan, clock, verdict, deferNow,
// and the withdrawnWhileLive condition verbatim.
static U unmap(const n48_fr_ring *r, uint64_t now)
{
    U u {};
    uint32_t bs = 0u; uint64_t ba = 0ull;
    u.anyLive = n48_fr_any_live(r);
    u.kdv = n48_fr_defer_verdict(r, 1u, 1u, now, kBound, &bs, &ba);
    u.deferNow = n48_ksd_defer(u.kdv);
    u.wwl = (!u.deferNow && u.anyLive && (u.kdv == N48_KSD_NOW_TIMEOUT || u.kdv == N48_KSD_NOW_TORN || u.kdv == N48_KSD_NOW_OFF)) ? 1u : 0u;
    return u;
}
struct Run { uint32_t state, deferred, wwlFirst, stopWhy, retiredNow, unfeedQueued, unfeedTok; };
static Run run10w(uint32_t on)
{
    Run o {};
    n48_fr_ring r {}; n48_fr_reset(&r);
    uint32_t last = 0u;
    // the gate stamp and the keystone (fence828 ordinal 1, epoch 0xadd9, slot vram+0x3cba8f004)
    (void)n48_fr_push(&r, kSeq, kAt, 1u, 0x3cba8f004ull, 0xadd90001u, nullptr);
    (void)n48_fr_commit_mark(&r, kSeq, &last);
    // the walk (inside Apple's original): refused heap-gen, NOPed, proven
    const n48_gfxn_exempt_t ex = record(1u);
    Walk a = walk_and_nop(1u, ex);
    n48_wn75_rec rec {};
    if (on) n48_wn75_note(&rec, ex.seq, n48_wn75_proof(gRing, kSize, &ex, a.why, a.pos, a.npos, &a.pass));
    // the hook after Apple's original returned: exWhy heap-gen, spared 0 of 1, never spared
    const uint32_t sparedN = n48_gfxn_spared_n(a.why, 0u, 1u);
    const n48_wn75_plan_t plan = n48_wn75_plan(on, kSeq, a.why, sparedN, 1u, 0xFFFFFFFFu, &rec);
    n48_dl_unfeed_q q {};
    if (sparedN == 0u) {
        uint32_t done = 0u;
        if (plan.action == N48_WN75_RETIRE) done = n48_fr_retire_nopped(&r, kSeq);
        if (!done) (void)n48_fr_mark_not_run(&r, kSeq);
        if (plan.unfeed) n48_dl_unfeed_queue(&q, kSeq);
        o.retiredNow = done;
    }
    o.unfeedQueued = q.n; o.unfeedTok = q.n ? q.tok[0] : 0u;
    uint32_t i = 0u; o.state = n48_fr_find(&r, kSeq, &i) ? r.e[i].state : (uint32_t)N48_FR_FREE;
    // the unmaps: fire #30 at +32042 us, then every 8.59 ms to fire #259, then fire #260 at +2019980 us
    uint64_t wwl = 0ull;
    for (uint32_t fire = 30u; fire <= 260u; fire++) {
        const uint64_t age = fire == 260u ? 2019980ull : 32042ull + (uint64_t)(fire - 30u) * 8590ull;
        const U u = unmap(&r, kAt + age);
        if (u.deferNow) o.deferred++;
        if (fire == 30u) o.wwlFirst = u.wwl;
        wwl += u.wwl;
        if (!u.deferNow) break;   // the first withdrawal ends the deferral window
    }
    n48_cm_shot sh {}; sh.state = N48_CM_SHOT_ARMED; sh.cont = 1u; sh.cont_n = 600u;
    (void)n48_cm_shot_stop_if_rose(&sh, 0ull, wwl, N48_CM_STOP_WITHDRAWAL);
    o.stopWhy = sh.state == N48_CM_SHOT_ARMED ? 0u : sh.stop_why;
    return o;
}
static void w2_run10w()
{
    const Run on = run10w(1u), off = run10w(0u);
    expect_u("W2 ON: token seq 3 retired at once as NOPED", on.retiredNow == 1u && on.state == N48_FR_NOPED, 1u);
    expect_u("W2 ON: fire #30 (+32042 us) is NOT deferred - nothing is live", on.deferred, 0u);
    expect_u("W2 ON: no withdrawal while a flight shows live", on.wwlFirst, 0u);
    expect_u("W2 ON: the continuous arm keeps going (no stop)", on.stopWhy, 0u);
    expect_u("W2 ON: the ledger un-feed of token seq 3 is queued", on.unfeedQueued == 1u && on.unfeedTok == kSeq, 1u);
    expect_u("W2 OFF (0.0.518): NOT_RUN", off.state, (uint64_t)N48_FR_NOT_RUN);
    expect_u("W2 OFF: fires #30-#259 deferred (230, as run10w's `230 fire(s) were deferred`)", off.deferred, 230u);
    expect_u("W2 OFF: fire #260 at 2019980 us withdraws with the entry live -> CONTINUOUS STOP stop_why 3",
             off.stopWhy, (uint64_t)N48_CM_STOP_WITHDRAWAL);
    expect_u("W2 OFF: the same un-feed (unchanged)", off.unfeedQueued == 1u && off.unfeedTok == kSeq, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- W3
static void w3_plan()
{
    n48_wn75_rec ok {}; n48_wn75_note(&ok, kSeq, 1u);
    n48_wn75_plan_t p = n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 0u, 1u, 0xFFFFFFFFu, &ok);
    expect_u("W3 proven, refused, spared 0 of 1, never spared: RETIRE + un-feed", p.action == N48_WN75_RETIRE && p.unfeed == 1u, 1u);
    p = n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 1u, 2u, 0xFFFFFFFFu, &ok);
    expect_u("W3 PARTIALLY spared (spared 1 of 2): KEEP (today's behaviour), no un-feed (today's: spared != 0)",
             p.action == N48_WN75_KEEP && p.unfeed == 0u && p.why == N48_WN75_WHY_SPARED_SOME, 1u);
    p = n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 2u, 3u, 0xFFFFFFFFu, &ok);
    expect_u("W3 PARTIALLY spared (spared 2 of 3): KEEP", p.action, (uint64_t)N48_WN75_KEEP);
    p = n48_wn75_plan(1u, kSeq, N48_GFXN_EX_USED, 0u, 1u, 0x8adu, &ok);
    expect_u("W3 a walk SPARED the record earlier (sparedAt set), a later walk USED: KEEP", p.action == N48_WN75_KEEP &&
             p.why == N48_WN75_WHY_SPARED_ONCE, 1u);
    n48_wn75_rec other {}; n48_wn75_note(&other, 4u, 1u);
    expect_u("W3 a proof for ANOTHER seq: KEEP", n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 0u, 1u, 0xFFFFFFFFu, &other).action,
             (uint64_t)N48_WN75_KEEP);
    n48_wn75_rec mixed = ok; n48_wn75_note(&mixed, kSeq, 0u);
    expect_u("W3 one walk proved it, another walk for the record could not: KEEP",
             n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 0u, 1u, 0xFFFFFFFFu, &mixed).action, (uint64_t)N48_WN75_KEEP);
    n48_wn75_rec none {};
    expect_u("W3 no proof: KEEP", n48_wn75_plan(1u, kSeq, N48_GFXN_EX_HEAPGEN, 0u, 1u, 0xFFFFFFFFu, &none).action, (uint64_t)N48_WN75_KEEP);
    expect_u("W3 NONE (no walk during the call: the NEXT walk NOPs it): KEEP",
             n48_wn75_plan(1u, kSeq, N48_GFXN_EX_NONE, 0u, 1u, 0xFFFFFFFFu, &ok).action, (uint64_t)N48_WN75_KEEP);
    // OFF identity over every answer and spare count: KEEP, and the un-feed exactly when spared 0 (0.0.518's `if (sparedN == 0u)`)
    uint32_t offOk = 1u, unfeedToday = 1u;
    for (uint32_t on = 0u; on < 2u; on++)
        for (uint32_t w = 0; w < N48_GFXN_EX_REASONS; w++)
            for (uint32_t s = 0; s <= 4u; s++) {
                const n48_wn75_plan_t q = n48_wn75_plan(on, kSeq, w, s, 4u, 0xFFFFFFFFu, &ok);
                if (!on && q.action != N48_WN75_KEEP) offOk = 0u;
                if (q.unfeed != (s == 0u ? 1u : 0u)) unfeedToday = 0u;
            }
    expect_u("W3 OFF: KEEP for every exemption answer and spare count", offOk, 1u);
    expect_u("W3 ON or OFF: the un-feed is asked exactly when 0.0.518 asked it (spared 0)", unfeedToday, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- W4
static void w4_ring()
{
    n48_fr_ring r {}; n48_fr_reset(&r);
    uint32_t last = 0u, i = 0u;
    (void)n48_fr_push(&r, 2u, 1000ull, 1u, 0x1000ull, 0x11u, nullptr); (void)n48_fr_commit_mark(&r, 2u, &last);
    (void)n48_fr_find(&r, 2u, &i); (void)n48_fr_poll_entry(&r, i, 1u, 0x11u);   // seq 2 retired: the newest's fate
    (void)n48_fr_reclaim(&r);
    (void)n48_fr_push(&r, 3u, 5000ull, 2u, 0x2000ull, 0x22u, nullptr);
    expect_u("W4 a PENDING entry is not retired as NOPED", n48_fr_retire_nopped(&r, 3u), 0u);
    (void)n48_fr_commit_mark(&r, 3u, &last);
    expect_u("W4 a COMMITTED entry is", n48_fr_retire_nopped(&r, 3u) == 1u && r.noped == 1u, 1u);
    expect_u("W4 ... twice is a no-op", n48_fr_retire_nopped(&r, 3u), 0u);
    expect_u("W4 NOPED is not live", n48_fr_any_live(&r), 0u);
    uint64_t cur = 0ull;
    expect_u("W4 NOPED is never polled", n48_fr_next_poll(&r, &cur), (uint64_t)N48_FR_CAPACITY);
    expect_u("W4 NOPED never expires", n48_fr_expire(&r, 1u, 5000ull + 10ull * kBound, kBound), 0u);
    expect_u("W4 the newest-flight name falls back to seq 2 (retired): EOP, not a flight in bound",
             n48_fr_defer_verdict(&r, 1u, 1u, 6000ull, kBound, nullptr, nullptr), (uint64_t)N48_KSD_NOW_EOP);
    expect_u("W4 NOPED is reclaimed", n48_fr_reclaim(&r) == 1u && !n48_fr_find(&r, 3u, &i), 1u);
    n48_fr_ring q {}; n48_fr_reset(&q);
    (void)n48_fr_push(&q, 5u, 1000ull, 1u, 0x1000ull, 0x11u, nullptr); (void)n48_fr_commit_mark(&q, 5u, &last);
    (void)n48_fr_mark_not_run(&q, 5u);
    expect_u("W4 a NOT_RUN entry is not moved (today's path already ran)", n48_fr_retire_nopped(&q, 5u), 0u);
    expect_u("W4 state name", std::strcmp(n48_fr_state_name(N48_FR_NOPED), "NOPED"), 0u);
}

// ---------------------------------------------------------------------------------------------------------------- W5
static void w5_p73()
{
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    const uint64_t key = 0x10930000ull;
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, key, 0x401800000ull, 1u, kSeq);
    (void)n48_p73_final(&t, kSeq, 0u);   // the hook: the walk did not spare it
    uint32_t why = 0u, slot = 0u;
    expect_u("W5 switch 73: the NOPed P is WITHDRAWN and its present HELD", n48_p73_present(&t, key, &why, &slot) == 0u &&
             why == N48_P73_HOLD_WITHDRAWN, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- W6
static std::string slurp(const char *p)
{
    std::string s; FILE *f = p ? std::fopen(p, "rb") : nullptr;
    if (!f) return s;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &h, const char *n)
{
    uint32_t c = 0;
    for (size_t a = h.find(n); a != std::string::npos; a = h.find(n, a + 1)) c++;
    return c;
}
static size_t at(const std::string &s, const char *n) { return s.find(n); }
static void w6_glue(const char *ahh, const char *cm, const char *dep)
{
    const std::string s = slurp(ahh), c = slurp(cm), dp = slurp(dep);
    expect_u("W6 (0.0.520) gfx_dep.h's n48_cp_told_past is byte-identical to the copy W7 tests (its semantics unchanged)",
             count(dp, "static inline uint32_t n48_cp_told_past(uint32_t cp_wptr, uint64_t mark, uint64_t wptr)\n{\n"
                       "    if (cp_wptr == 0xFFFFFFFFu || wptr <= mark) return 0u;\n"
                       "    const int32_t ahead  = (int32_t)(cp_wptr - (uint32_t)mark);\n"
                       "    const int32_t within = (int32_t)((uint32_t)wptr - cp_wptr);\n"
                       "    return (ahead > 0 && within >= 0) ? 1u : 0u;\n}"), 1u);
    expect_u("W6 the sources read", !s.empty() && !c.empty(), 1u);
    expect_u("W6 switch 75 is OFF at boot", count(s, "static volatile uint32_t gWn75On { 0u };"), 1u);
    expect_u("W6 gWn75On is assigned only by its verb, behind the mid-arm guard",
             count(s, "gWn75On = ") == 1u &&
             count(s, "const bool contRefused75 = n48_cm_cont_switch_refused(75u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u;") == 1u &&
             count(s, "else { changed75 = n48_ra_set(m, &f75); if (changed75) gWn75On = f75; }") == 1u, 1u);
    expect_u("W6 gfx_commit.h guards 75", count(c, "    case 75u:   /* build 0.0.519"), 1u);
    // the walk: the proof after the NOP pass, before Apple's writeTail (the doorbell)
    const size_t wt = at(s, "static uint64_t hook_gfxWriteTail(void *self) {");
    const char *nf = "            gfx_neuter_frame(cpu, from, wptr, size, ibPos, nIbPos, w.ibs - sparedN, stopAt != n, ibVa, ibLen, w.ibs_other,";
    const char *pr = "            const uint32_t wnOk = n48_wn75_proof_strict(g, size, &gCmx, exWhy, ibPos, nIbPos, &gGnPass, from, wnCp1, &wnFail);";
    // build 0.0.520: inside the gate, the fence, then the fresh read, then the strict proof
    const char *fe = "            __asm__ __volatile__(\"mfence\" ::: \"memory\");\n"
                     "            const uint32_t wnCp1 = navi48_reg_read32(kRegCpRb0Wptr);\n"
                     "            uint32_t wnFail = N48_WN75_PFS;\n";
    const char *door = "    const uint64_t rv = reinterpret_cast<Fn>(gOrigRingWriteTail)(self);";
    const char *gate = "        if (gWn75On && w.ibs > sparedN && armedNow && gCmx.inflight == 1u && exWhy != N48_GFXN_EX_NONE &&\n"
                       "            exWhy != N48_GFXN_EX_SPARED) {";
    expect_u("W6 the walk's proof: once, gated on ON + a refused in-flight record, AFTER the NOP pass and BEFORE the doorbell",
             count(s, pr) == 1u && count(s, gate) == 1u && wt != std::string::npos && at(s, nf) > wt && at(s, gate) > at(s, nf) &&
             at(s, pr) > at(s, gate) && at(s, door) > at(s, pr), 1u);
    expect_u("W6 (0.0.520) the proof is the STRICT one only: fence -> fresh CP_RB0_WPTR read -> proof, inside the gate",
             count(s, fe) == 1u && at(s, fe) > at(s, gate) && at(s, pr) > at(s, fe) && at(s, pr) - at(s, fe) < 400u &&
             count(s, "n48_wn75_proof(") == 0u && count(s, "n48_wn75_proof_strict(") == 1u, 1u);
    expect_u("W6 (0.0.520) the pre-NOP read is published with the pass (gGnPass.cp0 = wptr0), once, after the race witness",
             count(s, "    gGnPass.cp0 = wptr0;") == 1u && at(s, "    gGnPass.cp0 = wptr0;") > at(s, "    const bool raced = n48_cp_told_past(wptr0, from, wptr);"), 1u);
    expect_u("W6 (0.0.520) failures counted by reason", count(s, "gWn75.proofFailWhy[wnFail < N48_WN75_PFS ? wnFail : (uint32_t)N48_WN75_PF_PASS]++;"), 1u);
    expect_u("W6 (0.0.520) n48_cp_told_past's two old callers are unchanged",
             count(s, "    const bool raced = n48_cp_told_past(wptr0, from, wptr);") == 1u &&
             count(s, "        const bool early = sizeNow != 0u && n48_cp_told_past(cpw, gGfxDone, wptr);") == 1u &&
             count(s, "n48_cp_told_past(") == 2u, 1u);
    expect_u("W6 the proof is noted for the record's own seq",
             count(s, "            n48_wn75_note(&gCmxWn, gCmx.seq, wnOk);") == 1u, 1u);
    expect_u("W6 gfx_neuter_frame publishes its NOP pass (all seven fields) before anything returns",
             count(s, "    gGnPass.npos = npos; gGnPass.done = done; gGnPass.bad = bad; gGnPass.not_ib = notIb; gGnPass.over = over;\n"
                      "    gGnPass.stopped = walkStopped ? 1u : 0u; gGnPass.raced = raced ? 1u : 0u;") == 1u, 1u);
    // the hook: the record's proof cleared at setup, read with exWhy, the plan before the ring call
    const char *setup = "                gCmxLastWhy = N48_GFXN_EX_NONE; gCmxSparedAt = 0xFFFFFFFFu;\n"
                        "                gCmxWn = n48_wn75_rec {};";
    const char *rd = "                exWhy = gCmxLastWhy; exAt = gCmxSparedAt;\n                wnRec = gCmxWn;\n                gCmxWn = n48_wn75_rec {};";
    const char *plan = "            const n48_wn75_plan_t wnPlan = n48_wn75_plan(gWn75On ? 1u : 0u, seq, exWhy, sparedN, n, exAt, &wnRec);";
    const char *ret = "                    wnDone = n48_fr_retire_nopped(&gKsRing, seq);";
    const char *nr = "                if (!wnDone) (void)n48_fr_mark_not_run(&gKsRing, seq);";
    const char *uf = "                if (wnPlan.unfeed) { if (gXdDescPort) n48_dl_unfeed_queue(&gXdLedUnfeedQ, seq); }";
    const char *orig = "            const uint64_t rvT = orig(self, info);";
    expect_u("W6 the record's proof is cleared where the record is set up, before Apple's original",
             count(s, setup) == 1u && at(s, setup) < at(s, orig), 1u);
    expect_u("W6 ... read (and cleared) with exWhy after Apple's original", count(s, rd) == 1u && at(s, rd) > at(s, orig), 1u);
    expect_u("W6 the plan follows the spare count and precedes the ring call, the NOT_RUN fallback and the un-feed",
             count(s, plan) == 1u && at(s, plan) > at(s, "            const uint32_t sparedN = (exWhy == N48_GFXN_EX_SPARED) ? n : 0u;") &&
             at(s, ret) > at(s, plan) && at(s, nr) > at(s, ret) && at(s, uf) > at(s, nr) && count(s, ret) == 1u && count(s, uf) == 1u, 1u);
    expect_u("W6 the retire publishes with a release fence", count(s, "                    wnDone = n48_fr_retire_nopped(&gKsRing, seq);\n"
             "                    __atomic_thread_fence(__ATOMIC_RELEASE);"), 1u);
    expect_u("W6 n48_fr_retire_nopped is called nowhere else", count(s, "n48_fr_retire_nopped("), 1u);
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    int w = std::snprintf(b, sizeof b, N48_WN75_FMT, "ON (a walk-NOPed flight retires at once)",
                          " - `gfxneuter 75` REFUSED: a continuous arm stands, unchanged", M, M, M, M, M, M);
    expect_u("W6 the report line <= 491 bytes at 20-digit counters", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_WN75_LINE_FMT, 4294967295u, 4u, n48_wn75_why_name(N48_WN75_WHY_RETIRED),
                      n48_gfxn_ex_name(N48_GFXN_EX_BASE));
    expect_u("W6 the retire line <= 491", w > 0 && w <= 491, 1u);
}

int main(int argc, char **argv)
{
    std::printf("== W1 the walk's proof ==\n"); w1_proof();
    std::printf("== W2 run10w token seq 3 ==\n"); w2_run10w();
    std::printf("== W3 the plan ==\n"); w3_plan();
    std::printf("== W4 the ring ==\n"); w4_ring();
    std::printf("== W5 switch 73 ==\n"); w5_p73();
    std::printf("== W7 the strict CP witness (0.0.520) ==\n"); w7_strict();
    // gfx_dep.h: argv[3], else next to the AppleHardwareHook.cpp given (so the suite's existing command line still covers it)
    std::string dep = argc > 3 ? argv[3] : "";
    if (dep.empty() && argc > 1) { dep = argv[1]; const size_t sl = dep.rfind('/'); dep = (sl == std::string::npos ? std::string() : dep.substr(0, sl + 1)) + "gfx_dep.h"; }
    std::printf("== W6 kext glue ==\n"); w6_glue(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, dep.empty() ? nullptr : dep.c_str());
    std::printf("gfx_walknop75: %d run, %d FAILED\n", gRun, gFail);
    return gFail ? 1 : 0;
}
