// gfx_present73_test.cpp — build 0.0.516 ( option (a),): switch 73, gfx_present73.h over run10u's REAL P
// sequence, and the kext glue.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_present73_test.cpp -o /tmp/p73 && /tmp/p73 \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/DisplayPipeGuard.cpp \
//         src/navi48-bringup/src/apple/Navi48AccelPeer.cpp
// Covers:
//   T1 the key: run10u's P CB0 VAs resolve to the pipeshim's VidMemory VRAM offsets (the FRAME TARGET conversion + page offset);
//      an unresolved / host / unconverted target has no key;
//   T2 run10u F1..F171 replayed (the gate's own answers, a present of P's plane after every P whose CB0 is a plane, the final
//      outcome = the gate's answer): EARLY WINDOW - every present of a plane before its first committed P is HELD (F3/F6/F8
//      RESERVED-FOR-FILL and F17 not-translate, REFUSED; a probe finds NO-MATCH before a plane's first P); SETTLED - every present
//      after a committed P copies, and copies == committed plane P's; the uncaptured frames as UNJUDGED invalidate every plane
//      until each plane's next committed P;
//   T3 ORDER: a present between the gate's COMMIT and the hook's final outcome is HELD (PENDING);
//   T4 a P withdrawn after the gate (keystone / token / ring walk) HOLDS its plane (F125 on plane 0x401800000), until F133
//      commits; a final outcome with the wrong seq is only counted (the plane stays PENDING = held);
//   T5 fail-closed edges: an unresolved CB0 invalidates every plane; a non-P writer naming a plane; a full table (NO-MATCH, no
//      eviction); the reset;
//   T6 switch OFF: every present copies and the table is never touched (0.0.515's behaviour);
//   T7 the report lines fit the 491-byte log body at 20-digit counters;
//   T9 build 0.0.517: the non-P key built like the P's (a plane off a page start is matched); the first 8
//      COPIES are logged (n48_p73_copy_line_due) and the line fits; the p73 call starts only on the GFX channel;
//   T12 build 0.0.520: the promotion's CAS races a new P's gate for the same plane - every interleaving of
//      the promotion's READY / CAS / RE-CHECK with the gate's three slot writes leaves the new P held (PENDING);
//   T8 the kext glue: the switch is OFF at boot; the judge keys by the VRAM offset; the final outcome follows Apple's original and
//      the ring walk; dpg_perform asks before the copy; the flush-hook copy is idle while ON; the verb resets on ON.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "gfx_present73.h"
#include "fixture_cycle515_run10u.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-96s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-96s %#llx\n", what, (unsigned long long)got);
}

// run10u's own FRAME TARGET lines (notes/logs/runs/run10u/driverlog-stream.txt): `the first is VA V -> VRAM offset O (converted)`,
// and the pipeshim's `VidMemory VRAM` for the three planes (perform #1-#3: 0x10930000, 0x11200000, 0x11a70000).
struct VaMap { uint64_t va, voff; };
static const VaMap kMap[] = {
    { 0x401800000ull, 0x10930000ull }, { 0x402800000ull, 0x11200000ull }, { 0x403800000ull, 0x11a70000ull },   // the planes
    { 0x400800000ull, 0x10030000ull }, { 0x404800000ull, 0x125e0000ull }, { 0x400240000ull, 0x111ba000ull },   // X, X', the LUT
};
static const uint64_t kPlaneVa[3] = { 0x401800000ull, 0x402800000ull, 0x403800000ull };
static const uint64_t kPlaneVram[3] = { 0x10930000ull, 0x11200000ull, 0x11a70000ull };
static uint64_t page_voff(uint64_t va, uint32_t *ok)
{
    for (const VaMap &m : kMap) if (m.va == (va & ~0xfffull)) { *ok = 1u; return m.voff; }
    *ok = 0u; return 0ull;
}
static uint64_t key_of(uint64_t va)
{
    uint32_t ok = 0u;
    const uint64_t pv = page_voff(va, &ok);
    return n48_p73_key(va, pv, ok, 0u, ok);
}
static int plane_of(uint64_t va) { for (int i = 0; i < 3; i++) if (kPlaneVa[i] == va) return i; return -1; }
static bool is_p(const cy515_fx &r) { return r.captured && r.nib == 1u && r.len[0] == 1040u && r.nws == 1u; }
static uint32_t probe(const n48_p73 *t, uint64_t key)   // the state a present would see, without counting
{
    const uint32_t i = n48_p73_find(t, key);
    return i < N48_P73_SLOTS ? n48_p73_ld32(&t->s[i].state) : 0xFFu;
}

// ---------------------------------------------------------------------------------------------------------------- T1
static void t1_key()
{
    for (int p = 0; p < 3; p++) {
        char l[160]; std::snprintf(l, sizeof l, "T1 P CB0 VA %#llx keys to the pipeshim's plane VRAM", (unsigned long long)kPlaneVa[p]);
        expect_u(l, key_of(kPlaneVa[p]), kPlaneVram[p]);
    }
    expect_u("T1 the page offset is kept (CB0 0x401800100 -> 0x10930100)", n48_p73_key(0x401800100ull, 0x10930000ull, 1u, 0u, 1u), 0x10930100ull);
    expect_u("T1 unresolved -> no key", n48_p73_key(0x401800000ull, 0x10930000ull, 0u, 0u, 1u), 0u);
    expect_u("T1 host memory -> no key", n48_p73_key(0x401800000ull, 0x10930000ull, 1u, 1u, 1u), 0u);
    expect_u("T1 not converted -> no key", n48_p73_key(0x401800000ull, 0x10930000ull, 1u, 0u, 0u), 0u);
    expect_u("T1 the key is NOT the GPU VA", key_of(0x401800000ull) != 0x401800000ull, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T2
// One judged frame as the kext sees it: the gate, then (a COMMIT) the final outcome, then the present of P's plane.
// `unjudged_q`: an uncaptured row ("?") is a frame the judge never saw. `withdraw_f`: that P is withdrawn after the gate.
struct Replay { uint64_t copies, holds, holdsByReason[N48_P73_HOLDS], planePs, planePsCommitted; uint32_t firstCommit[3]; };
static Replay replay(n48_p73 *t, bool unjudged_q, uint32_t withdraw_f, uint32_t *earlyOk, uint32_t *probeNoMatchOk)
{
    Replay r; std::memset(&r, 0, sizeof r);
    *earlyOk = 1u; *probeNoMatchOk = 1u;
    uint32_t seenP[3] = { 0u, 0u, 0u };
    for (const cy515_fx &x : kCy515Run10u) {
        // before this frame: a plane that no P has named yet answers NO-MATCH (the early window's first half)
        for (int p = 0; p < 3; p++) if (!seenP[p] && probe(t, kPlaneVram[p]) != 0xFFu) *probeNoMatchOk = 0u;
        n48_p73_call_begin(t);
        if (!x.captured) {
            if (!unjudged_q) (void)n48_p73_gate_nonp(t, nullptr, 0u);
            (void)n48_p73_after_decide(t);
            continue;
        }
        if (is_p(x)) {
            const uint64_t va = x.ws[0];
            const uint32_t seq = x.f;   // the gate's token seq (any non-zero, unique per frame)
            (void)n48_p73_gate_p(t, key_of(va), va, x.committed, x.committed ? seq : 0u);
            (void)n48_p73_after_decide(t);
            if (x.committed) (void)n48_p73_final(t, seq, x.f == withdraw_f ? 0u : 1u);
            // 0.0.519 (Part B): the replay's present follows its P's end of pipe - the flight RETIRES first (a withdrawn P never
            // ran and never retires; T10 below replays run10v's REAL present/RETIRED order)
            if (x.committed && x.f != withdraw_f) (void)n48_p73_retired(t, seq);
            const int p = plane_of(va);
            if (p >= 0) {
                seenP[p] = 1u;
                r.planePs++;
                const uint32_t comm = x.committed && x.f != withdraw_f;
                if (comm) { r.planePsCommitted++; if (!r.firstCommit[p]) r.firstCommit[p] = x.f; }
                uint32_t why = 0u, slot = 0u;
                const uint32_t c = n48_p73_should_copy(1u, t, key_of(va), &why, &slot);
                if (c) r.copies++; else { r.holds++; r.holdsByReason[why]++; }
                if (!r.firstCommit[p] && c) *earlyOk = 0u;       // a copy before the plane's first committed P
                if (c != comm) *earlyOk = 0u;                      // a copy exactly when THIS P committed
            }
        } else {
            uint64_t keys[24]; uint32_t n = 0;
            for (uint32_t k = 0; k < x.nws && k < 24u; k++) { const uint64_t kk = key_of(x.ws[k]); if (kk) keys[n++] = kk; }
            (void)n48_p73_gate_nonp(t, keys, n);
            (void)n48_p73_after_decide(t);
        }
    }
    return r;
}
static n48_p73 gT;
static void t2_run10u()
{
    uint32_t early = 0, nomatch = 0;
    std::memset(&gT, 0, sizeof gT); n48_p73_reset(&gT);
    const Replay a = replay(&gT, false, 0u, &early, &nomatch);
    expect_u("T2 plane P's in run10u (P frames whose CB0 is a plane)", a.planePs, 36u);
    expect_u("T2 EARLY WINDOW: F3, F6, F8 (RESERVED-FOR-FILL) and F17 (not-translate) are HELD, REFUSED", a.holdsByReason[N48_P73_HOLD_REFUSED], 4u);
    expect_u("T2 ... and nothing else is held", a.holds, 4u);
    expect_u("T2 the first committed P per plane: 0x401800000 at F32", a.firstCommit[0], 32u);
    expect_u("T2 ... 0x402800000 at F22", a.firstCommit[1], 22u);
    expect_u("T2 ... 0x403800000 at F27", a.firstCommit[2], 27u);
    expect_u("T2 no present copied before its plane's first committed P, and each copied exactly when its P committed", early, 1u);
    expect_u("T2 a plane no P has named answers NO-MATCH (no slot)", nomatch, 1u);
    expect_u("T2 SETTLED: copies == committed plane P's (32)", a.copies == a.planePsCommitted && a.copies == 32u, 1u);
    {
        uint32_t hits = 0, late = 0;
        for (const cy515_fx &x : kCy515Run10u)
            if (x.captured && !is_p(x))
                for (uint32_t k = 0; k < x.nws && k < 24u; k++) if (plane_of(x.ws[k]) >= 0) { hits++; if (x.f >= 22u) late++; }
        expect_u("T2 run10u's non-P plane writers: F5, F7, F10 (1472 dw, refused), all before any plane's first committed P",
                 hits == 3u && late == 0u, 1u);
    }
    expect_u("T2 five surfaces are CB0 of a 1040-dword frame (X, X', three planes): 5 slots, no overflow",
             gT.overflow == 0u && n48_p73_find(&gT, 0x10030000ull) < N48_P73_SLOTS && n48_p73_find(&gT, 0x125e0000ull) < N48_P73_SLOTS, 1u);
    // The uncaptured rows ("?") as frames the judge never saw: every plane UNKNOWN until its next committed P.
    std::memset(&gT, 0, sizeof gT); n48_p73_reset(&gT);
    // Probe after F60 (the last "?" of F55..F60): all three planes UNKNOWN; after F71 plane 0 COMMITTED, the others still UNKNOWN.
    uint32_t st60[3] = { 9u, 9u, 9u }, st71[3] = { 9u, 9u, 9u }, st89[3] = { 9u, 9u, 9u };
    for (const cy515_fx &x : kCy515Run10u) {
        n48_p73_call_begin(&gT);
        if (!x.captured) (void)n48_p73_after_decide(&gT);
        else if (is_p(x)) {
            (void)n48_p73_gate_p(&gT, key_of(x.ws[0]), x.ws[0], x.committed, x.committed ? x.f : 0u);
            (void)n48_p73_after_decide(&gT);
            if (x.committed) { (void)n48_p73_final(&gT, x.f, 1u); (void)n48_p73_retired(&gT, x.f); }
        } else { (void)n48_p73_gate_nonp(&gT, nullptr, 0u); (void)n48_p73_after_decide(&gT); }
        for (int p = 0; p < 3; p++) {
            if (x.f == 60u) st60[p] = probe(&gT, kPlaneVram[p]);
            if (x.f == 71u) st71[p] = probe(&gT, kPlaneVram[p]);
            if (x.f == 89u) st89[p] = probe(&gT, kPlaneVram[p]);
        }
    }
    expect_u("T2 UNJUDGED F55..F60: after F60 all three planes UNKNOWN (held)",
             st60[0] == N48_P73_ST_UNKNOWN && st60[1] == N48_P73_ST_UNKNOWN && st60[2] == N48_P73_ST_UNKNOWN, 1u);
    expect_u("T2 ... after F71 (plane 0 commits) plane 0 COMMITTED, planes 1/2 still UNKNOWN",
             st71[0] == N48_P73_ST_COMMITTED && st71[1] == N48_P73_ST_UNKNOWN && st71[2] == N48_P73_ST_UNKNOWN, 1u);
    expect_u("T2 ... after F89 all three COMMITTED again (F80 plane 2, F89 plane 1)",
             st89[0] == N48_P73_ST_COMMITTED && st89[1] == N48_P73_ST_COMMITTED && st89[2] == N48_P73_ST_COMMITTED, 1u);
    expect_u("T2 unjudged frames counted (11 uncaptured rows)", gT.unjudged, 11u);
}

// ---------------------------------------------------------------------------------------------------------------- T3
static void t3_order()
{
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    const uint64_t k = kPlaneVram[0];
    uint32_t why = 0, slot = 0;
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, k, kPlaneVa[0], 1u, 7u);
    expect_u("T3 between the gate's COMMIT and the final outcome the plane is PENDING", probe(&t, k), N48_P73_ST_PENDING);
    expect_u("T3 ... and a present then is HELD", n48_p73_present(&t, k, &why, &slot), 0u);
    expect_u("T3 ... as UNKNOWN", why, N48_P73_HOLD_UNKNOWN);
    expect_u("T3 the final outcome (SPARED) applies", n48_p73_final(&t, 7u, 1u), 1u);
    expect_u("T3 (0.0.519) ... but the plane stays PENDING until the P's flight retires: the present is HELD",
             n48_p73_present(&t, k, &why, &slot) == 0u && why == N48_P73_HOLD_UNKNOWN && probe(&t, k) == N48_P73_ST_PENDING, 1u);
    expect_u("T3 (0.0.519) the P's flight RETIRES: promoted", n48_p73_retired(&t, 7u), 1u);
    expect_u("T3 ... then the present copies", n48_p73_present(&t, k, &why, &slot), 1u);
    // a refused P after a committed one: held again
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, k, kPlaneVa[0], 0u, 0u);
    expect_u("T3 a later REFUSED P holds the plane it last committed", n48_p73_present(&t, k, &why, &slot) == 0u && why == N48_P73_HOLD_REFUSED, 1u);
    // a COMMIT without a token seq can never be matched: held
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, k, kPlaneVa[0], 1u, 0u);
    expect_u("T3 a COMMIT with no token seq is UNKNOWN (held)", probe(&t, k), N48_P73_ST_UNKNOWN);
}

// ---------------------------------------------------------------------------------------------------------------- T4
static void t4_withdrawn()
{
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    uint32_t early = 0, nomatch = 0;
    const Replay r = replay(&t, false, 125u, &early, &nomatch);
    expect_u("T4 F125 (plane 0x401800000) withdrawn after the gate: its present is HELD, WITHDRAWN", r.holdsByReason[N48_P73_HOLD_WITHDRAWN], 1u);
    expect_u("T4 ... every other present as before (4 refused holds, 31 copies)", r.holds == 5u && r.copies == 31u, 1u);
    expect_u("T4 ... and each present copied exactly when its own P committed (F133 re-commits the plane)", early, 1u);
    // the three withdrawal sites all reach the same final(seq, 0)
    static n48_p73 u; std::memset(&u, 0, sizeof u); n48_p73_reset(&u);
    const uint64_t k = kPlaneVram[1];
    n48_p73_call_begin(&u); (void)n48_p73_gate_p(&u, k, kPlaneVa[1], 1u, 40u); (void)n48_p73_final(&u, 40u, 1u);
    n48_p73_call_begin(&u); (void)n48_p73_gate_p(&u, k, kPlaneVa[1], 1u, 41u);
    expect_u("T4 a final with ANOTHER seq (token mismatch across submissions) is not applied", n48_p73_final(&u, 99u, 1u), 0u);
    expect_u("T4 ... it is counted, and the plane stays PENDING (held), never COMMITTED",
             u.finalUnmatched == 1u && probe(&u, k) == N48_P73_ST_PENDING, 1u);
    n48_p73_call_begin(&u); (void)n48_p73_gate_p(&u, k, kPlaneVa[1], 1u, 42u);
    expect_u("T4 keystone / ring-walk withdrawal: final(seq, 0) -> WITHDRAWN", n48_p73_final(&u, 42u, 0u) == 1u && probe(&u, k) == N48_P73_ST_WITHDRAWN, 1u);
    expect_u("T4 a second final for the same submission is a no-op", n48_p73_final(&u, 42u, 1u) == 0u && probe(&u, k) == N48_P73_ST_WITHDRAWN, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T5
static void t5_edges()
{
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    for (int p = 0; p < 3; p++) {
        n48_p73_call_begin(&t);
        (void)n48_p73_gate_p(&t, kPlaneVram[p], kPlaneVa[p], 1u, 10u + (uint32_t)p);
        (void)n48_p73_final(&t, 10u + (uint32_t)p, 1u);
        (void)n48_p73_retired(&t, 10u + (uint32_t)p);
    }
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, 0ull, 0x405800000ull, 1u, 20u);   // CB0 did not resolve
    expect_u("T5 an unresolved P CB0 makes EVERY plane UNKNOWN",
             probe(&t, kPlaneVram[0]) == N48_P73_ST_UNKNOWN && probe(&t, kPlaneVram[1]) == N48_P73_ST_UNKNOWN &&
             probe(&t, kPlaneVram[2]) == N48_P73_ST_UNKNOWN && t.pUnresolved == 1u, 1u);
    for (int p = 0; p < 3; p++) {
        n48_p73_call_begin(&t);
        (void)n48_p73_gate_p(&t, kPlaneVram[p], kPlaneVa[p], 1u, 30u + (uint32_t)p);
        (void)n48_p73_final(&t, 30u + (uint32_t)p, 1u);
        (void)n48_p73_retired(&t, 30u + (uint32_t)p);
    }
    const uint64_t ks[2] = { 0x10030000ull, kPlaneVram[2] };
    n48_p73_call_begin(&t);
    expect_u("T5 a non-P frame naming plane 2 as a colour target: one slot invalidated", n48_p73_gate_nonp(&t, ks, 2u), 1u);
    expect_u("T5 ... plane 2 UNKNOWN, planes 0/1 still COMMITTED",
             probe(&t, kPlaneVram[2]) == N48_P73_ST_UNKNOWN && probe(&t, kPlaneVram[0]) == N48_P73_ST_COMMITTED, 1u);
    n48_p73_call_begin(&t);
    expect_u("T5 a frame the judge never saw invalidates every plane", n48_p73_after_decide(&t) == 1u &&
             probe(&t, kPlaneVram[0]) == N48_P73_ST_UNKNOWN && probe(&t, kPlaneVram[1]) == N48_P73_ST_UNKNOWN, 1u);
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_nonp(&t, nullptr, 0u);
    expect_u("T5 ... a judged frame does not", n48_p73_after_decide(&t), 0u);
    // the full table: 8 slots, the 9th key is not recorded (NO-MATCH, held), nothing is evicted
    static n48_p73 f; std::memset(&f, 0, sizeof f); n48_p73_reset(&f);
    for (uint32_t i = 0; i < 9u; i++) {
        n48_p73_call_begin(&f);
        (void)n48_p73_gate_p(&f, 0x20000000ull + i * 0x1000000ull, 0x410000000ull + i * 0x1000000ull, 1u, 100u + i);
        (void)n48_p73_final(&f, 100u + i, 1u);
        (void)n48_p73_retired(&f, 100u + i);
    }
    uint32_t why = 0, slot = 0;
    expect_u("T5 a full table: the 9th plane is NO-MATCH (held) and counted",
             n48_p73_present(&f, 0x28000000ull, &why, &slot) == 0u && why == N48_P73_HOLD_NOMATCH && f.overflow == 1u, 1u);
    expect_u("T5 ... the first 8 are not evicted (still COMMITTED)", n48_p73_present(&f, 0x20000000ull, &why, &slot), 1u);
    n48_p73_reset(&f);
    expect_u("T5 the reset (switch turned ON) forgets every slot", n48_p73_find(&f, 0x20000000ull), N48_P73_SLOTS);
    expect_u("T5 key 0 never matches", n48_p73_find(&f, 0ull), N48_P73_SLOTS);
}

// ---------------------------------------------------------------------------------------------------------------- T9
static void t9_0517()
{
    // (a) a plane whose P CB0 is NOT at a page start: the P key keeps the page offset, so the non-P key must too.
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    const uint64_t va = 0x401800100ull, pageVoff = 0x10930000ull;
    const uint64_t kP = n48_p73_key(va, pageVoff, 1u, 0u, 1u);
    n48_p73_call_begin(&t);
    (void)n48_p73_gate_p(&t, kP, va, 1u, 50u);
    (void)n48_p73_final(&t, 50u, 1u);
    (void)n48_p73_retired(&t, 50u);
    expect_u("T9 a plane at page offset 0x100 is COMMITTED under key 0x10930100", probe(&t, 0x10930100ull), N48_P73_ST_COMMITTED);
    const uint64_t oldKey[1] = { pageVoff };                                    // 0.0.516's non-P key: the page only
    n48_p73_call_begin(&t);
    expect_u("T9 0.0.516's page-only non-P key misses that plane (the LOW finding)", n48_p73_gate_nonp(&t, oldKey, 1u), 0u);
    const uint64_t newKey[1] = { n48_p73_key(va, pageVoff, 1u, 0u, 1u) };       // 0.0.517: the P's own builder
    n48_p73_call_begin(&t);
    expect_u("T9 the non-P key built like the P's matches it: one slot invalidated", n48_p73_gate_nonp(&t, newKey, 1u), 1u);
    expect_u("T9 ... and that plane is UNKNOWN (held)", probe(&t, 0x10930100ull), N48_P73_ST_UNKNOWN);
    // (b) the first 8 copies are logged, no more; OFF, a hold or no slot never logs; the reset restarts the count
    static n48_p73 c; std::memset(&c, 0, sizeof c); n48_p73_reset(&c);
    uint32_t due = 0u;
    for (uint32_t k = 0; k < 12u; k++) due += n48_p73_copy_line_due(&c, 1u, 1u, 2u);
    expect_u("T9 12 copies: exactly the first 8 are logged", due, N48_P73_COPY_LINES);
    static n48_p73 d; std::memset(&d, 0, sizeof d); n48_p73_reset(&d);
    expect_u("T9 OFF never logs a copy", n48_p73_copy_line_due(&d, 0u, 1u, 2u), 0u);
    expect_u("T9 a hold never logs a copy", n48_p73_copy_line_due(&d, 1u, 0u, 2u), 0u);
    expect_u("T9 no slot never logs a copy", n48_p73_copy_line_due(&d, 1u, 1u, N48_P73_SLOTS), 0u);
    expect_u("T9 ... none of those spent a line", d.copy_lines, 0u);
    n48_p73_reset(&c);
    expect_u("T9 the reset (switch ON) restarts the copy lines", n48_p73_copy_line_due(&c, 1u, 1u, 0u) == 1u && c.copy_lines == 1u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T6
static void t6_off()
{
    static n48_p73 t, z; std::memset(&t, 0, sizeof t); std::memset(&z, 0, sizeof z);
    uint64_t presents = 0, copies = 0;
    for (const cy515_fx &x : kCy515Run10u) {
        if (!is_p(x) || plane_of(x.ws[0]) < 0) continue;
        presents++;
        uint32_t why = 7u, slot = 7u;
        copies += n48_p73_should_copy(0u, &t, key_of(x.ws[0]), &why, &slot);
    }
    expect_u("T6 OFF: every present of the replay copies (0.0.515)", presents == 36u && copies == presents, 1u);
    expect_u("T6 OFF: the table and counters are never touched", std::memcmp(&t, &z, sizeof t), 0u);
}

// ---------------------------------------------------------------------------------------------------------------- T7
static void t7_lines()
{
    char b[1024];
    const unsigned long long M = 18446744073709551615ull;
    int w = std::snprintf(b, sizeof b, N48_P73_FMT, "ON (a present copies only a plane whose last P COMMITTED)",
                          " - `gfxneuter 73` REFUSED: a continuous arm stands, unchanged", M, M, M, M, M, M, M, 8u, M, 4294967295u);
    expect_u("T7 present73 report line 1 <= 491 bytes at 20-digit counters", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_FMT2, M, M, M, M, M, M, M, M, M);
    expect_u("T7 present73 report line 2 <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_SLOT_FMT, 7u, M, M, "WITHDRAWN", 4294967295u);
    expect_u("T7 slot line <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_PLINE_FMT, M, M, 7u, M);
    expect_u("T7 P CB0 line <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_PRLINE_FMT, M, M, 7u, M);
    expect_u("T7 present-match line <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_HOLD_FMT, M, M, "WITHDRAWN", 8u, "no slot", 4294967295u, M, M);
    expect_u("T7 hold line <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_COPY_FMT, M, M, 7u, 4294967295u, 4294967295u, 4294967295u);
    expect_u("T7 (0.0.517) COPIED line <= 491", w > 0 && w <= 491, 1u);
    w = std::snprintf(b, sizeof b, N48_P73_FMT3, M, M, M, M, M);
    expect_u("T7 (0.0.519) the copy-after-retire line <= 491", w > 0 && w <= 491, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T10
// build 0.0.519: RUN O's (run10v) REAL order of gate COMMIT, the walk's SPARED final, the present (pipeshim
// perform #N and its `present73: COPIED` line) and the P's `flightring: RETIRED` line (notes/logs/runs/run10v/driverlog-stream.txt
// lines 14622-18125): P seq 12 -> plane 0x111f0000, 13 -> 0x10930000, 15 -> 0x111f0000, 17 -> 0x10930000 (seq 14 is CB0 0x404800000 ->
// 0x12570000, its FRAME TARGET line 16193; seq 16 is slot 0, 0x10030000, the report's `slot 0 ... COMMITTED seq 16`; neither is
// presented in this window). 0.0.518 COPIED presents #5, #6 and #7 BEFORE their P's RETIRED line.
enum { EV_GATE, EV_FINAL, EV_PRESENT, EV_RETIRED };
struct Ev { int kind; uint32_t seq; uint64_t key; uint32_t presentNo; uint32_t line; };
static const Ev kRun10v[] = {
    { EV_GATE, 12u, 0x111f0000ull, 0u, 14534u }, { EV_FINAL, 12u, 0, 0u, 14622u },
    { EV_PRESENT, 0u, 0x111f0000ull, 5u, 14646u }, { EV_RETIRED, 12u, 0, 0u, 15374u },
    { EV_GATE, 13u, 0x10930000ull, 0u, 15961u }, { EV_FINAL, 13u, 0, 0u, 16050u },
    { EV_PRESENT, 0u, 0x10930000ull, 6u, 16075u }, { EV_RETIRED, 13u, 0, 0u, 16187u },
    { EV_GATE, 14u, 0x12570000ull, 0u, 16189u }, { EV_FINAL, 14u, 0, 0u, 16276u }, { EV_RETIRED, 14u, 0, 0u, 16521u },
    { EV_GATE, 15u, 0x111f0000ull, 0u, 16754u }, { EV_FINAL, 15u, 0, 0u, 16840u },
    { EV_PRESENT, 0u, 0x111f0000ull, 7u, 16854u }, { EV_RETIRED, 15u, 0, 0u, 16976u },
    { EV_GATE, 16u, 0x10030000ull, 0u, 16978u }, { EV_FINAL, 16u, 0, 0u, 17064u }, { EV_RETIRED, 16u, 0, 0u, 17285u },
    { EV_PRESENT, 0u, 0x10930000ull, 8u, 17354u },
    { EV_GATE, 17u, 0x10930000ull, 0u, 17604u }, { EV_FINAL, 17u, 0, 0u, 17690u }, { EV_RETIRED, 17u, 0, 0u, 17781u },
    { EV_PRESENT, 0u, 0x10930000ull, 10u, 18125u },
};
// `old518`: the 0.0.518 rule, modelled as the final promoting at once (a SPARED final is COMMITTED immediately).
static void t10_run10v(bool old518, uint32_t copied[16], uint32_t heldSeq[16])
{
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    for (const Ev &e : kRun10v) {
        if (e.kind == EV_GATE) { n48_p73_call_begin(&t); (void)n48_p73_gate_p(&t, e.key, 0x400000000ull, 1u, e.seq); }
        else if (e.kind == EV_FINAL) { (void)n48_p73_final(&t, e.seq, 1u); if (old518) (void)n48_p73_retired(&t, e.seq); }
        else if (e.kind == EV_RETIRED) (void)n48_p73_retired(&t, e.seq);
        else {
            uint32_t why = 0u, slot = 0u;
            copied[e.presentNo] = n48_p73_present(&t, e.key, &why, &slot);
            heldSeq[e.presentNo] = slot < N48_P73_SLOTS ? t.s[slot].seq : 0u;
        }
    }
}
static void t10()
{
    uint32_t c[16] = { 0 }, h[16] = { 0 }, o[16] = { 0 }, oh[16] = { 0 };
    t10_run10v(false, c, h);
    t10_run10v(true, o, oh);
    expect_u("T10 run10v under 0.0.518's rule: presents #5, #6, #7 COPIED before their P's RETIRED ( MEDIUM-1 on hardware)",
             o[5] == 1u && o[6] == 1u && o[7] == 1u, 1u);
    expect_u("T10 0.0.519: present #5 (P seq 12, RETIRED 728 lines later) is HELD", c[5] == 0u && h[5] == 12u, 1u);
    expect_u("T10 0.0.519: present #6 (P seq 13, RETIRED at 16187) is HELD", c[6] == 0u && h[6] == 13u, 1u);
    expect_u("T10 0.0.519: present #7 (P seq 15, RETIRED at 16976) is HELD", c[7] == 0u && h[7] == 15u, 1u);
    expect_u("T10 0.0.519: present #8 (P seq 13, retired) COPIES", c[8], 1u);
    expect_u("T10 0.0.519: present #10 (P seq 17, RETIRED at 17781) COPIES", c[10], 1u);
    // edges: retirement before the final; a retirement for another seq; a later P replacing a retired-but-unspared one
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    uint32_t why = 0u, slot = 0u;
    n48_p73_call_begin(&t); (void)n48_p73_gate_p(&t, 0x10930000ull, 0x401800000ull, 1u, 40u);
    expect_u("T10 a retirement that arrives before the final: not yet copyable", n48_p73_retired(&t, 40u) == 0u &&
             probe(&t, 0x10930000ull) == N48_P73_ST_PENDING, 1u);
    expect_u("T10 ... the SPARED final then promotes it (counted once)", n48_p73_final(&t, 40u, 1u) == 1u &&
             probe(&t, 0x10930000ull) == N48_P73_ST_COMMITTED && t.pCommitted == 1u && t.pRetiredFirst == 1u, 1u);
    expect_u("T10 ... a second retirement of the same seq changes nothing", n48_p73_retired(&t, 40u) == 0u && t.pCommitted == 1u, 1u);
    n48_p73_call_begin(&t); (void)n48_p73_gate_p(&t, 0x10930000ull, 0x401800000ull, 1u, 41u); (void)n48_p73_final(&t, 41u, 1u);
    expect_u("T10 a retirement of ANOTHER seq (the plane's previous P) does not promote the new P",
             n48_p73_retired(&t, 40u) == 0u && probe(&t, 0x10930000ull) == N48_P73_ST_PENDING && t.retireNoSlot == 2u, 1u);
    expect_u("T10 ... its own retirement does", n48_p73_retired(&t, 41u) == 1u && n48_p73_present(&t, 0x10930000ull, &why, &slot) == 1u, 1u);
    n48_p73_call_begin(&t); (void)n48_p73_gate_p(&t, 0x10930000ull, 0x401800000ull, 1u, 42u); (void)n48_p73_final(&t, 42u, 0u);
    expect_u("T10 a WITHDRAWN P (the walk NOPed it) is never promoted by a retirement",
             n48_p73_retired(&t, 42u) == 0u && probe(&t, 0x10930000ull) == N48_P73_ST_WITHDRAWN, 1u);
    n48_p73_call_begin(&t); (void)n48_p73_gate_p(&t, 0x10930000ull, 0x401800000ull, 1u, 43u); (void)n48_p73_final(&t, 43u, 1u);
    (void)n48_p73_invalidate_all(&t);
    expect_u("T10 a spared P whose plane was invalidated before it retired stays UNKNOWN (held)",
             n48_p73_retired(&t, 43u) == 0u && probe(&t, 0x10930000ull) == N48_P73_ST_UNKNOWN, 1u);
    expect_u("T10 seq 0 is never a retirement", n48_p73_retired(&t, 0u), 0u);
}

// ---------------------------------------------------------------------------------------------------------------- T12
// build 0.0.520: THE PROMOTION RACE, as an interleaving model. Thread R promotes P seq 50 (spared AND
// retired) in its three steps (READY, CAS, RE-CHECK); thread G is a NEW P (seq 51) for the SAME plane passing the gate
// (n48_p73_gate_p's slot writes, in its own order: marks cleared, seq = 51, state = PENDING). Every one of the C(6,3) = 20
// interleavings is run; afterwards the new P - neither spared nor retired - must be PENDING (held) with seq 51, pCommitted
// counts only a promotion that really committed seq 50, and a COMMITTED state never names seq 51.
static void t12_race()
{
    const uint64_t key = 0x10930000ull, va = 0x401800000ull;
    uint32_t allHeld = 1u, neverWrong = 1u, counted = 1u, n = 0u, reverted = 0u, bad = 0u;
    for (uint32_t mask = 0; mask < 64u; mask++) {
        if (__builtin_popcount(mask) != 3) continue;          // bit i set = step i of the merged 6 is R's
        n++;
        static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
        n48_p73_call_begin(&t); (void)n48_p73_gate_p(&t, key, va, 1u, 50u);
        (void)n48_p73_retired(&t, 50u);
        // P 50's final marks it spared WITHOUT promoting (the promotion is R's, stepped below)
        n48_p73_slot *s = &t.s[n48_p73_find(&t, key)];
        __atomic_store_n(&s->spared, 50u, __ATOMIC_SEQ_CST); t.pend_seq = 0u;
        uint32_t r = 0u, g = 0u, rOk = 1u, rRes = 0u;
        for (uint32_t k = 0; k < 6u; k++) {
            if (mask & (1u << k)) {
                if (r == 0u) rOk = n48_p73_promote_ready(s, 50u);
                else if (r == 1u) rOk = rOk ? n48_p73_promote_cas(s) : 0u;
                else rRes = rOk ? n48_p73_promote_recheck(&t, s, 50u) : 0u;
                r++;
            } else {
                if (g == 0u) { s->spared = 0u; s->retired = 0u; }
                else if (g == 1u) s->seq = 51u;
                else n48_p73_st32(&s->state, N48_P73_ST_PENDING);
                g++;
            }
            const uint32_t st = n48_p73_ld32(&s->state);
            // a COMMITTED visible after RE-CHECK ran must name 50 (the reverting window is between R's CAS and its re-check).
            // Excluded: the gate's own window between its seq store and its PENDING store (g == 2) - there a slot COMMITTED for
            // the PREVIOUS P shows the new seq until the gate's next store; that is 0.0.519's gate order, before Apple's
            // original runs, and not the promotion's race.
            if (r == 3u && g != 2u && st == N48_P73_ST_COMMITTED && s->seq != 50u) neverWrong = 0u;
        }
        uint32_t why = 0u, slot = 0u;
        const uint32_t copy = n48_p73_present(&t, key, &why, &slot);
        if (copy || n48_p73_ld32(&s->state) != N48_P73_ST_PENDING || s->seq != 51u) { allHeld = 0u; bad = mask; }
        if (t.pCommitted != rRes) counted = 0u;
        reverted += (uint32_t)t.promoteReverted;
    }
    expect_u("T12 all 20 interleavings of the promotion (READY, CAS, RE-CHECK) with a new P's gate were run", n, 20u);
    expect_u("T12 in every interleaving the new P (seq 51, not spared, not retired) ends PENDING and its plane is HELD", allHeld, 1u);
    if (!allHeld) std::printf("      first bad interleaving mask %#x\n", bad);
    expect_u("T12 a COMMITTED state never names the new P once the promotion finished", neverWrong, 1u);
    expect_u("T12 pCommitted counts exactly the promotions that committed seq 50", counted, 1u);
    expect_u("T12 some interleavings needed the re-check's revert (the model reaches the race)", reverted > 0u, 1u);
    // the sequential cases are unchanged: promotion with no racer commits and copies
    static n48_p73 u; std::memset(&u, 0, sizeof u); n48_p73_reset(&u);
    n48_p73_call_begin(&u); (void)n48_p73_gate_p(&u, key, va, 1u, 60u); (void)n48_p73_final(&u, 60u, 1u);
    uint32_t why = 0u, slot = 0u;
    expect_u("T12 no racer: spared then retired -> COMMITTED, copied, counted once, nothing reverted",
             n48_p73_retired(&u, 60u) == 1u && n48_p73_present(&u, key, &why, &slot) == 1u && u.pCommitted == 1u &&
             u.promoteReverted == 0u, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T8
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
static void t8_glue(const char *ahh, const char *dpg, const char *peer)
{
    const std::string s = slurp(ahh), d = slurp(dpg), p = slurp(peer);
    expect_u("T8 the three sources read", !s.empty() && !d.empty() && !p.empty(), 1u);
    expect_u("T8 switch 73 is OFF at boot (gP73On { 0u })", count(s, "static volatile uint32_t gP73On { 0u };"), 1u);
    expect_u("T8 gP73On is assigned in exactly one place (the verb)", count(s, "gP73On = "), 1u);
    // the verb: guarded, ON/OFF by n48_ra_set, the table reset on OFF -> ON
    expect_u("T8 the verb turns it ON/OFF through n48_ra_set and resets the table on OFF -> ON",
             count(s, "            changed73 = n48_ra_set(m, &f73);\n"
                      "            if (changed73) { if (f73 && !gP73On) n48_p73_reset(&gP73); gP73On = f73; }") == 1u, 1u);
    // the judge: keyed by the VRAM offset (tgtVoff), after the gate's action, inside gfxsrc_decide_frame
    const char *cAct = "    const uint32_t action = n48_sd_action(arm, shapeOk, verdict, commitOk);\n";
    const char *cFill = "gP73J.gateSeq = gXdCmGateSeq; gP73J.tgtVa = tgtVa; gP73J.tgtVoff = tgtVoff; gP73J.tgtN = tgtN; gP73J.tgtOk = tgtOk;";
    const char *cCall = "        p73_judge(hVa, hPage, hRes, hSys);\n";
    const char *cKey = "        const uint64_t key = n48_p73_key(gP73J.tgtVa, gP73J.tgtVoff, gP73J.tgtOk, gP73J.tgtSys, gP73J.tgtVoffOk);";
    const size_t dec = at(s, "static uint32_t gfxsrc_decide_frame(");
    const size_t decEnd = s.find("\n}\n", dec);
    expect_u("T8 the judge's inputs carry CB0's VRAM offset (tgtVoff), exactly once", count(s, cFill), 1u);
    expect_u("T8 p73_judge keys by n48_p73_key(VA, the page's VRAM offset, ...), exactly once", count(s, cKey), 1u);
    expect_u("T8 the judge is called once, inside gfxsrc_decide_frame, after the gate's action",
             count(s, cCall) == 1u && dec != std::string::npos && at(s, cAct) > dec && at(s, cCall) > at(s, cAct) &&
             at(s, cCall) < decEnd, 1u);
    // the hook: begin before the disarmed pass, after_decide after the judge was asked, the finals after their outcomes
    const size_t hk = at(s, "static uint64_t hook_gfxCommitIB(void *self, void *info) {");
    const char *hBegin = "    if (gP73On && self == gPm4GfxChan) n48_p73_call_begin(&gP73);";
    const char *hPass = "    if (gGfxNeuter != 1u || self != gPm4GfxChan || reinterpret_cast<uintptr_t>(info) < kKernelHalfBase) {";
    const char *hDec = "    const uint32_t xdAction = gfxsrc_decide_frame(";
    const char *hAfter = "    if (gP73On && route == N48_ROUTE_DECIDE) p73_after_decide();";
    const char *hNotWs = "    if (route == N48_ROUTE_NOT_WS) {";
    const char *hWd = "        if (gP73On && !(tokMatch && ksOk)) p73_final(here.seq, 0u);";
    const char *hKs = "        const bool ksOk = frHasPendingEntry && commit_keystone_arm(wf, 1u, gXdCmGateSeq, kr, ks64);";
    const char *hOrig = "            const uint64_t rvT = orig(self, info);";
    const char *hWhy = "                exWhy = gCmxLastWhy; exAt = gCmxSparedAt;";
    const char *hFin = "            if (gP73On) p73_final(seq, exWhy == N48_GFXN_EX_SPARED ? 1u : 0u);";
    expect_u("T8 hook order: begin -> the disarmed pass -> the judge asked -> after_decide -> the NOT_WS branch",
             hk != std::string::npos && at(s, hBegin) > hk && at(s, hBegin) < at(s, hPass) && at(s, hPass) < at(s, hDec) &&
             at(s, hDec) < at(s, hAfter) && at(s, hAfter) < at(s, hNotWs) && count(s, hBegin) == 1u && count(s, hAfter) == 1u, 1u);
    expect_u("T8 the token / guard / keystone withdrawal is final(seq, 0), after the keystone answered, exactly once",
             count(s, hWd) == 1u && at(s, hWd) > at(s, hKs), 1u);
    expect_u("T8 COMMITTED only from the ring walk's answer, AFTER Apple's original returned, exactly once",
             count(s, hFin) == 1u && at(s, hFin) > at(s, hOrig) && at(s, hFin) > at(s, hWhy), 1u);
    expect_u("T8 p73_final appears only in its definition, its n48_p73_final call and the two sites",
             count(s, "p73_final(") == 4u && count(s, "    (void)n48_p73_final(&gP73, seq, committed);") == 1u, 1u);
    // build 0.0.517: the call starts only for a GFX-channel submission; no unfiltered begin is left.
    expect_u("T8 (0.0.517) no p73 call begin without the GFX-channel filter",
             count(s, "n48_p73_call_begin(&gP73);") == 1u && count(s, "if (gP73On) n48_p73_call_begin") == 0u, 1u);
    expect_u("T8 (0.0.517) the non-P key is n48_p73_key over the held VA, exactly once; the page-only form is gone",
             count(s, "        gP73J.keys[k] = n48_p73_key(hVa[k], voff, hRes[k], hSys[k], ok ? 1u : 0u);") == 1u &&
             count(s, "ok ? voff : 0ull") == 0u, 1u);
    {
        /* build 0.0.531: the answer comes through n48_p86_should_copy (switch 86 OFF = n48_p73_should_copy, unchanged); the
         * COPIED line prints the LAST gate seq (gXdCmGateSeqLast), and the present's wall time is noted before the return. */
        const char *pSc = "    const uint32_t copy = n48_p86_should_copy(gP73On ? 1u : 0u, p86_mode() == N48_P86_ON ? 1u : 0u, &gP73, &gP86, phys, &kP86Io,";
        const char *pCl = "    if (n48_p73_copy_line_due(&gP73, gP73On ? 1u : 0u, copy, i))\n"
                          "        HWLOG(N48_P86_COPY_FMT, (unsigned long long)presentNo, (unsigned long long)phys, i, gP73.s[i].seq,\n"
                          "              __atomic_load_n(&gXdCmGateSeqLast, __ATOMIC_RELAXED), gP73.copy_lines);\n"
                          "    wt_note_since(&gWtPresent, wt0);\n    return copy;";
        const size_t pp = at(s, "uint32_t hw_p73_present(uint64_t phys, uint64_t presentNo)");
        expect_u("T8 (0.0.517) the COPIED line: once, in hw_p73_present, after the answer and right before its return",
                 count(s, pCl) == 1u && pp != std::string::npos && at(s, pSc) > pp && at(s, pCl) > at(s, pSc) &&
                 at(s, pCl) < at(s, "void hw_p73_flush_held()"), 1u);
    }
    // build 0.0.519 (Part B): the two retirement sites feed the table, each exactly once, after the ring retired the entry.
    {
        const char *rj = "            if (frRetiredNow) {\n                p73_retired(frSeq);";
        const char *rx = "            const n48_fr_xret &x = gKsXRet[i];\n            p73_retired(x.seq);";
        const char *rd = "static __attribute__((noinline)) void p73_retired(uint32_t seq)\n{\n    if (!gP73On) return;\n    (void)n48_p73_retired(&gP73, seq);\n}";
        expect_u("T8 (0.0.519) p73_retired: its definition is gated on gP73On, and it is called at exactly the two retirement sites",
                 count(s, rd) == 1u && count(s, rj) == 1u && count(s, rx) == 1u && count(s, "p73_retired(") == 5u &&   /* the declaration, the definition, its n48_ call, two sites */
                 at(s, rj) > at(s, "            const bool frRetiredNow = n48_fr_poll_entry(&gKsRing, fi, frGot, frVal);") &&
                 at(s, rx) > at(s, "        n = n48_fr_expiry_poll(&gKsRing, 1u, *nowOk, *nowUs, kKsFlightUs, &ks_x_read, nullptr, gKsXRet, N48_FR_CAPACITY, &o);"), 1u);
    }
    {   // build 0.0.520: the promotion the kext runs IS the three steps T12 interleaves, in that order
        std::string hp = ahh ? ahh : "";
        const size_t sl = hp.rfind('/');
        hp = (sl == std::string::npos ? std::string() : hp.substr(0, sl + 1)) + "gfx_present73.h";
        const std::string h = slurp(hp.c_str());
        expect_u("T8 (0.0.520) n48_p73_promote = READY, then CAS, then the RE-CHECK (the steps T12 interleaves), and nothing else",
                 count(h, "static inline uint32_t n48_p73_promote(n48_p73 *t, n48_p73_slot *s, uint32_t seq)\n{\n"
                          "    if (!n48_p73_promote_ready(s, seq)) return 0u;\n"
                          "    if (!n48_p73_promote_cas(s)) return 0u;\n"
                          "    return n48_p73_promote_recheck(t, s, seq);\n}") == 1u &&
                 count(h, "n48_p73_promote(t, s, seq)") == 2u, 1u);
    }
    expect_u("T8 the disarmed GFX pass invalidates while ON",
             count(s, "        if (gP73On && self == gPm4GfxChan) p73_after_decide();") == 1u, 1u);
    expect_u("T8 the present side answers through n48_p86_should_copy(gP73On ...) (0.0.531: switch 86 OFF = n48_p73_should_copy)",
             count(s, "    const uint32_t copy = n48_p86_should_copy(gP73On ? 1u : 0u, p86_mode() == N48_P86_ON ? 1u : 0u, &gP73, &gP86, phys, &kP86Io,") == 1u, 1u);
    // dpg_perform: asks before the copy, after the descriptor refusal, and holds by returning before any copy or readback
    const char *dAsk = "    if (n48::hw_p73_on() && !n48::hw_p73_present(phys, gSh.perform)) return 0;\n    uint64_t cv[11] = { 0 };";
    const size_t dpf = at(d, "static uint32_t dpg_perform(void *self, void *txn) {");
    expect_u("T8 dpg_perform asks exactly once, right before the copy's status array",
             count(d, dAsk) == 1u && at(d, dAsk) > dpf && at(d, dAsk) > at(d, "    if (!descOk || (swz != 0u && swz != 3u)) {") &&
             at(d, dAsk) < at(d, "? navi48_scanout_copy_tiled(phys, len, surfW, surfH, swz,") &&
             at(d, dAsk) < at(d, "n48_dpg_verify_due(phys, gSh.lastVerifyToken, gSh.verifyRuns)"), 1u);
    // 0.0.521 Part E adds exactly one site (the delivered call after the copy's bucket: T13 pins its place): 2 -> 4 mentions
    expect_u("T8 the shim has no other switch-73 site (0.0.521: + the delivered call)", count(d, "hw_p73_") * 0x10u +
             count(d, "if (n48::hw_p73_on() && n48_dpg_present_bucket(cst) != N48_DPG_PRESENT_REFUSED) n48::hw_p73_delivered();"), 0x41u);
    // the other scanout writer: idle while ON
    const char *pIdle = "        if ((mode & 2u) && n48::hw_p73_on()) n48::hw_p73_flush_held();\n        else if (mode & 2u) {";
    expect_u("T8 the flush-hook COPY is idle while 73 is ON (the copy only in the else branch)",
             count(p, pIdle) == 1u && at(p, pIdle) < at(p, "st = navi48_scanout_copy_vram(phys, len, w, h, w * 4u, 0, kFlushCopyDstY, cw, ch, v, 11);") &&
             at(p, pIdle) < at(p, "st = navi48_scanout_copy_staged(backing, w * 4u, w, h, 0, kFlushCopyDstY, cw, ch, v, 11);") &&
             count(p, "if (mode & 2u) {") == count(p, "else if (mode & 2u) {"), 1u);
    expect_u("T8 the switch-73 code never reads the cyc515 instrument (gCy515) in its helpers",
             count(s.substr(at(s, "static n48_p73 gP73 {};"), at(s, "void hw_p73_flush_held()") - at(s, "static n48_p73 gP73 {};")), "gCy515"), 0u);
}


// ---------------------------------------------------------------------------------------------------------------- T13
// build 0.0.521 Part E: MONOTONIC copies. Three planes rotate; each plane's COMMITTED P is from a different time. A present
// whose plane's P is not NEWER than the P last delivered to the glass is HELD (older); a newer one copies; the gate's seq wraps
// 0xFFFFFFFF -> 1 (serial order); turning 73 ON forgets the glass's P; OFF touches nothing.
static void commit_plane(n48_p73 *t, uint64_t va, uint32_t seq)
{
    n48_p73_call_begin(t);
    (void)n48_p73_gate_p(t, key_of(va), va, 1u, seq);
    (void)n48_p73_after_decide(t);
    (void)n48_p73_final(t, seq, 1u);
    (void)n48_p73_retired(t, seq);
}
static uint32_t present_deliver(n48_p73 *t, uint64_t va, uint32_t *why)
{
    uint32_t w = 0u, slot = 0u;
    const uint32_t c = n48_p73_should_copy(1u, t, key_of(va), &w, &slot);
    if (c) n48_p73_delivered(t);   // dpg_perform: the copy happened
    if (why) *why = w;
    return c;
}
static void t13_monotonic()
{
    static n48_p73 t; std::memset(&t, 0, sizeof t); n48_p73_reset(&t);
    const uint64_t A = 0x401800000ull, B = 0x402800000ull, C = 0x404800000ull;   // the three presented planes
    commit_plane(&t, A, 10u); commit_plane(&t, B, 12u); commit_plane(&t, C, 11u);
    uint32_t why = 0u;
    expect_u("T13 after ON the first committed plane copies (A, P 10)", present_deliver(&t, A, &why), 1u);
    expect_u("T13 B (P 12, newer) copies", present_deliver(&t, B, &why), 1u);
    expect_u("T13 C (P 11, OLDER than the glass's 12) is HELD, reason OLDER", present_deliver(&t, C, &why) * 0x10u + why, N48_P73_HOLD_OLDER);
    expect_u("T13 A again (P 10) is HELD older; B again (P 12, the SAME P) is HELD too (not newer)",
             present_deliver(&t, A, &why) + present_deliver(&t, B, &why), 0u);
    expect_u("T13 the held-older counter and the glass's P", t.held[N48_P73_HOLD_OLDER] * 0x100u + t.lastDelivered, 3u * 0x100u + 12u);
    commit_plane(&t, C, 13u);
    expect_u("T13 C's NEWER P 13 copies and becomes the glass's P", present_deliver(&t, C, &why) * 0x100u + t.lastDelivered, 0x100u + 13u);
    // a decision whose copy did not happen (dpg_perform refused it) does not move the glass's P
    commit_plane(&t, A, 14u);
    uint32_t w = 0u, sl = 0u;
    expect_u("T13 A's P 14 decided but its copy failed: the glass stays at 13", n48_p73_should_copy(1u, &t, key_of(A), &w, &sl) * 0x100u + t.lastDelivered, 0x100u + 13u);
    n48_p73_delivered(&t); n48_p73_delivered(&t);
    expect_u("T13 delivered takes only the last decision, once", t.lastDelivered * 0x100u + (uint32_t)t.delivered, 14u * 0x100u + 4u);
    // the wrap
    expect_u("T13 serial order: 1 is newer than 0xFFFFFFFF, 0xFFFFFFFF older than 1, 5 not newer than 5",
             n48_p73_seq_newer(1u, 0xFFFFFFFFu) * 0x100u + n48_p73_seq_newer(0xFFFFFFFFu, 1u) * 0x10u + n48_p73_seq_newer(5u, 5u), 0x100u);
    static n48_p73 u; std::memset(&u, 0, sizeof u); n48_p73_reset(&u);
    commit_plane(&u, A, 0xFFFFFFFFu);
    expect_u("T13 WRAP: A at P 0xFFFFFFFF copies", present_deliver(&u, A, &why), 1u);
    commit_plane(&u, B, 1u);
    expect_u("T13 WRAP: B at P 1 (after the wrap) is NEWER and copies", present_deliver(&u, B, &why), 1u);
    commit_plane(&u, C, 0xFFFFFFFEu);
    expect_u("T13 WRAP: C at P 0xFFFFFFFE (before the wrap) is held older", present_deliver(&u, C, &why) * 0x10u + why, N48_P73_HOLD_OLDER);
    // ON again forgets the glass's P: the first committed plane copies whatever its seq
    n48_p73_reset(&u);
    commit_plane(&u, C, 0xFFFFFFFEu);
    expect_u("T13 73 turned ON again (reset): the glass's P is forgotten, an 'older' P copies", present_deliver(&u, C, &why), 1u);
    // OFF: nothing is asked, nothing counted
    static n48_p73 z; std::memset(&z, 0, sizeof z); n48_p73_reset(&z);
    commit_plane(&z, A, 20u); z.lastDelivered = 30u; z.deliveredOk = 1u;
    static n48_p73 z0; std::memcpy(&z0, &z, sizeof z);
    expect_u("T13 OFF: an older plane copies (0.0.515) and the table is untouched",
             n48_p73_should_copy(0u, &z, key_of(A), &w, &sl) == 1u && std::memcmp(&z, &z0, sizeof z) == 0, 1u);
}
static void t13_glue(const char *ahhPath, const char *dpgPath)
{
    if (!ahhPath || !dpgPath) { expect_u("T13 glue: sources given", 0u, 1u); return; }
    const std::string h = slurp(ahhPath), d = slurp(dpgPath);
    if (h.empty() || d.empty()) { expect_u("T13 glue: sources readable", 0u, 1u); return; }
    const size_t c0 = d.find("if (n48::hw_p73_on() && !n48::hw_p73_present(phys, gSh.perform)) return 0;");
    const size_t c1 = d.find("if (navi48_fm_on()) {");
    const size_t c2 = d.find("switch (n48_dpg_present_bucket(cst)) {");
    const size_t c3 = d.find("if (n48::hw_p73_on() && n48_dpg_present_bucket(cst) != N48_DPG_PRESENT_REFUSED) n48::hw_p73_delivered();");
    expect_u("T13 glue ORDER: 73's question -> flip mode / the copy -> the bucket -> delivered (only a copy that happened)",
             c0 != std::string::npos && c1 != std::string::npos && c2 != std::string::npos && c3 != std::string::npos && c0 < c1 && c1 < c2 && c2 < c3, 1u);
    expect_u("T13 glue: delivered is 73-gated and calls the pure step", h.find("void hw_p73_delivered() { if (gP73On) n48_p73_delivered(&gP73); }") != std::string::npos, 1u);
    expect_u("T13 glue: turning 73 ON resets the table (and the glass's P)", h.find("if (changed73) { if (f73 && !gP73On) n48_p73_reset(&gP73); gP73On = f73; }") != std::string::npos, 1u);
    expect_u("T13 glue: the bare-73 line carries older, delivered and the glass's P",
             h.find("(unsigned long long)gP73.held[N48_P73_HOLD_NOMATCH], (unsigned long long)gP73.held[N48_P73_HOLD_OLDER],") != std::string::npos &&
             h.find("(unsigned long long)gP73.flushHeld, gP73.hold_lines, (unsigned long long)gP73.delivered, gP73.lastDelivered);") != std::string::npos, 1u);
}

int main(int argc, char **argv)
{
    std::printf("== T1 the key ==\n"); t1_key();
    std::printf("== T2 run10u replayed ==\n"); t2_run10u();
    std::printf("== T3 order ==\n"); t3_order();
    std::printf("== T4 withdrawn ==\n"); t4_withdrawn();
    std::printf("== T5 edges ==\n"); t5_edges();
    std::printf("== T6 OFF ==\n"); t6_off();
    std::printf("== T7 line bounds ==\n"); t7_lines();
    std::printf("== T9 0.0.517 ==\n"); t9_0517();
    std::printf("== T10 0.0.519 run10v copy-after-retire ==\n"); t10();
    std::printf("== T12 the promotion race (0.0.520) ==\n"); t12_race();
    std::printf("== T13 0.0.521 Part E: monotonic copies ==\n"); t13_monotonic();
    t13_glue(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr);
    std::printf("== T8 kext glue ==\n");
    t8_glue(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("gfx_present73: %d run, %d FAILED\n", gRun, gFail);
    return gFail ? 1 : 0;
}
