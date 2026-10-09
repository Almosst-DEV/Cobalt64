// gfx_rnforgive_test.cpp — build 0.0.523 (an internal design note, rev 2 governs; ):
// SWITCH 77, the ring-neuter forgiveness - gfx_rnforgive.h, gfx_dep.h's fill/identity, and the kext glue's ordering.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_rnforgive_test.cpp -o /tmp/rn && /tmp/rn \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp
// run10z (RUN R, notes/logs/runs/run10z/driverlog-stream.txt): frame 15 = token seq 4, one 1040-dword IB (VA 0x4002a0000),
// `fence828: GATE OK ... (token seq 4)`, `RETIRED AT ONCE AS NOPED (switch 75)`, `RING EXEMPTION for token seq 4: heap-gen -
// spared 0 of 1 IB(s)`; `d4f: frame 19 D4' consumer ... -> list-overflow (unproven 1)`; frame 21 `d4f ... -> clean (unproven 0)`
// then `dep_ok 0 (ring-neuter)`. Frame 15's R5' record is built from its REAL body (tests/fixture_run10z_f15.h, generated from
// capdec/ib/F00015-IB0.bin, the capture.bin decode; fnv re-checked below) - not synthesized.
// Covers T1-T9 of the contract, the source-order (reachability) test, and reviewer item C3 (line widths, maximal fields).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include "gfx_rnforgive.h"
#include "gfx_desc_port.h"
#include "fixture_run10z_f15.h"
#include "fixture_dep_fill_0522.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}

static uint32_t fnv32(const uint32_t *d, uint32_t n)
{
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < n; i++)
        for (unsigned b = 0; b < 4; b++) { h ^= (d[i] >> (8u * b)) & 0xffu; h *= 0x01000193u; }
    return h;
}
static uint32_t res_ident(void *, uint64_t va, uint64_t *page) { if (page) *page = va & ~0xFFFull; return 1u; }

// ---- the world RUN R's frames were judged in (the consumer path, R5', one IB NOPed in the ring) ----------------------------
static n48_dep_src base_src(uint32_t clause, uint64_t unproven)
{
    n48_dep_src s {};
    s.gathered = 1u; s.src_install = 1u; s.ring_state = 1u; s.ring_hooked = 1u; s.sdma_state = 2u;
    s.stall_armed = 1u; s.stall_read_ok = 1u; s.pre_walked = 1u; s.pre_ibs = 0u; s.snap_ok = 1u;
    s.q_hook_live = 1u; s.q_tally_ok = 1u; s.fault_read_ok = 1u;
    uint64_t *v = s.v;
    v[N48_DEPC_SRC_CALLS] = 5u; v[N48_DEPC_SRC_TRANSLATED] = 1u; v[N48_DEPC_SRC_NEUTERED] = 4u;
    v[N48_DEPC_RING_IBS_FOUND] = 1u; v[N48_DEPC_RING_IBS_ARMED] = 1u; v[N48_DEPC_GN_IBS] = 1u;   // frame 15's one IB, NOPed
    v[N48_DEPC_RING_FRAMES_NEUTERED] = 1u; v[N48_DEPC_GN_FRAMES] = 1u;
    s.cp_enabled = 1u; s.cp_enumerated = 1u; s.cp_clause = clause; s.cp_unproven = unproven; s.cp_scoped = 1u;
    s.r5_mode = 1u; s.r5_blind = 0u; s.d4_enabled = 1u; s.d4_enumerated = 1u;
    return s;
}
static uint32_t judge(const n48_dep_src &s, uint64_t *detail, n48_dep_world *out = nullptr)
{
    n48_dep_mono m {};
    n48_dep_world w {};
    n48_dep_fill(&s, &m, &w);
    if (out) *out = w;
    return n48_dep_check(&w, detail);
}

// ---- a model of the kext's four sites, the real functions in the kext's order ---------------------------------------------
struct Kx {
    n48_rn_q q {}; n48_rn_store st {}; n48_rn_stats rn {}; n48_r5_ring r5 {};
    uint32_t on = 1u;
    uint32_t scope = 0x302u;   // gXpScopeSeq: (arm COMMIT + 1) | (epoch << 8)
};
static n48_r5_frame gF15;       // frame 15's resolved R5' record (static: ~1.2 KiB)
static void build_f15()
{
    static n48_gcap_item items[512];
    n48_r5_frame &f = gF15;
    f = n48_r5_frame {};
    f.ib_ok = 1u; f.walk_ok = 1u; f.targets_ok = 1u; f.memw_ok = 1u; f.ib_over = 0u;   // the kext's per-frame seed (reader ok, nib 1)
    uint32_t so = 0u; uint64_t mi = 0ull;
    n48_r5_build_ib(&f, kF15Ib, kF15Len, items, 512u, kF15StartVa, N48_GCAP_F_FILLER, &so, &mi);
    f.out_of_scope = 0u;                                                                 // gfxsrc_rn_save forces it 0
    n48_r5_resolve(&f, &res_ident, nullptr);
}
// SAVE (decide, after the feed), QUEUE (the retire site, inside `if (wnDone)`), DRAIN (next decide, after the scope), GATHER.
static void kx_save(Kx &k, const n48_r5_frame &f, uint32_t seq, uint32_t scope, uint32_t nib) { (void)n48_rn_save(&k.st, &k.rn, seq, scope, nib, &f); }
static void kx_retire(Kx &k, uint32_t seq, uint32_t n) { if (k.on) { n48_rn_queue(&k.q, &k.rn, seq, n); k.rn.retiredIbs += n; } }
// the kext's rn_unfeed_cb: n48_dl_unfeed_tok on the ledger (fix pass MUST-FIX 1); `gLedModel` stands in for gXdLed
static n48_dl gLedModel;
static uint32_t gUnfeedCalls = 0u;
static void kx_unfeed_cb(void *, uint32_t seq) { gUnfeedCalls++; (void)n48_dl_unfeed_tok(&gLedModel, seq); }
static void kx_drain(Kx &k) { if (k.on) (void)n48_rn_drain(&k.q, &k.st, &k.r5, k.scope, 21u, &k.rn, &kx_unfeed_cb, nullptr); }
static void kx_gather(const Kx &k, n48_dep_src &s)
{
    s.rn_enabled = k.on ? 1u : 0u;
    s.rn_forgive = k.rn.forgiven;
    s.rn_clause = n48_rn_verdict(k.rn.forgiven, k.rn.grantedIbs, k.rn.retiredIbs);
    s.r5_blind = s.r5_mode ? n48_r5_unknown(&k.r5) : 0ull;
}

// ---- T1: run10z's real sequence --------------------------------------------------------------------------------------------
static void t1()
{
    expect_u("T1 frame 15's body is the capture's (fnv over 1040 dwords == capdec's 3100842958)", fnv32(kF15Ib, kF15Len), kF15Fnv);
    build_f15();
    expect_u("T1 frame 15's R5' record from its REAL body is READABLE", n48_r5_bucket(&gF15), N48_R5_READABLE);
    uint32_t cb0 = 0u;
    for (uint32_t i = 0; i < gF15.ntgt; i++) if (gF15.tgt[i].va == 0x404800000ull) cb0 = 1u;
    expect_u("T1 ... and names CB0 0x404800000 (the plane, capdec's CB0 region) as a destination", cb0, 1u);
    for (uint32_t on = 0; on < 2u; on++) {
        Kx k; k.on = on;
        if (on) kx_save(k, gF15, 4u, k.scope, 1u);   // OFF: nothing is saved (gfxsrc_rn_save returns at !gRnOn)
        kx_retire(k, 4u, 1u);
        kx_drain(k);                                  // the frame-19 decide's drain
        n48_dep_src s19 = base_src(N48_CP_LIST_OVER, 1u); kx_gather(k, s19);
        uint64_t d = 0u;
        char lbl[160];
        std::snprintf(lbl, sizeof lbl, "T1 %s: frame 19 (D4' list-overflow, unproven 1) refuses CONSUMER_UNPROVEN", on ? "ON" : "OFF");
        expect_u(lbl, judge(s19, &d), N48_DEP_CONSUMER_UNPROVEN);
        n48_dep_src s21 = base_src(N48_CP_OK, 0u); kx_gather(k, s21);
        const uint32_t r21 = judge(s21, &d);
        if (!on) {
            expect_u("T1 OFF: frame 21 (clean) refuses RING_NEUTER", r21, N48_DEP_RING_NEUTER);
            expect_u("T1 OFF: ... with detail 1", d, 1u);
            expect_u("T1 OFF: nothing saved, queued or noted", k.rn.saved + k.rn.queued + k.r5.noted, 0u);
        } else {
            expect_u("T1 ON: frame 21 is no longer refused at RING_NEUTER (clean: OK)", r21, N48_DEP_OK);
            expect_u("T1 ON: one grant of one IB", k.rn.granted * 16u + k.rn.grantedIbs, 17u);
            expect_u("T1 ON: frame 15 noted READABLE into the current scope's R5' ring", k.r5.readable * 16u + k.r5.blind, 16u);
        }
    }
}

// ---- T2: MUST refuse --------------------------------------------------------------------------------------------------------
static void t2()
{
    build_f15();
    // (i) a later consumer whose R3 pointer page is one of frame 15's destinations
    for (uint32_t drained = 0; drained < 2u; drained++) {
        Kx k; kx_save(k, gF15, 4u, k.scope, 1u); kx_retire(k, 4u, 1u);
        if (drained) kx_drain(k);
        n48_r5_scope(&k.r5, k.scope);
        n48_cp_ring ring {}; n48_dep_witness wt {};
        n48_cp_consumer c {};
        c.enumerated = 1u; c.n = 1u; c.va[0] = 0x400900000ull; c.mode[0] = 0u; c.resolved[0] = 1u;
        c.nptr = 1u; c.ptr[0] = 0x404800000ull; c.ptr_resolved[0] = 1u; c.ptr_page[0] = 0x404800000ull;
        n48_cp_consumer_d4 c4 {};
        c4.enumerated = 1u; c4.n = 1u; c4.va[0] = 0x400900000ull; c4.resolved[0] = 1u;
        c4.nptr = 1u; c4.ptr[0] = 0x404800000ull; c4.ptr_resolved[0] = 1u; c4.ptr_page[0] = 0x404800000ull;
        uint64_t u = 0u, st = 0u;
        const uint32_t a = n48_cp_eval_hz(&c, &ring, &wt, &k.r5, &u, &st);
        const uint32_t b = n48_cp_eval_hz_d4(&c4, &ring, &wt, &k.r5, &u, &st);
        if (drained) {
            expect_u("T2(i) after the drain: n48_cp_eval_hz refuses the consumer reading frame 15's CB0 page (R3 memdst)", a, N48_CP_R3_MEMDST);
            expect_u("T2(i) after the drain: n48_cp_eval_hz_d4 refuses it too", b, N48_CP_R3_MEMDST);
        } else {
            expect_u("T2(i) non-vacuity: before any note the same consumer passes R3 (n48_cp_eval_hz)", a, N48_CP_OK);
            expect_u("T2(i) non-vacuity: ... and n48_cp_eval_hz_d4", b, N48_CP_OK);
        }
    }
    // (ii) a surface frame 15 was the FIRST to feed, un-fed through the real queue and drain, leaves the consumer R1-unproven
    {
        static n48_dl l; l = n48_dl {};
        n48_dl_unfeed_q q {};
        n48_dl_sync(&l, 2u, 7u);
        l.feedTok = 4u; l.feedArmEp = 7u;
        n48_dl_set(&l, 0x55ull, 0x404800000ull, 27u, 0x404800000ull, 0u, 0x800000ull);
        l.feedTok = 0u;
        expect_u("T2(ii) non-vacuity: before the un-feed the ledger proves frame 15's surface", n48_dl_tiled_ok(&l, 0x55ull, 0x404800000ull, 27u), 1u);
        n48_dl_unfeed_queue(&q, 4u);                    // the retire site (wnPlan.unfeed)
        (void)n48_dl_unfeed_drain(&l, &q);              // the next decide, BEFORE the rn drain
        expect_u("T2(ii) after n48_dl_unfeed_drain the surface is unproven (R1 refuses the consumer)", n48_dl_tiled_ok(&l, 0x55ull, 0x404800000ull, 27u), 0u);
    }
    // (ii') MUST-FIX 1: the retire site's un-feed push is LOST (two overlapping commitIB calls): the GRANT itself un-feeds
    {
        gLedModel = n48_dl {};
        n48_dl_sync(&gLedModel, 2u, 7u);
        gLedModel.feedTok = 4u; gLedModel.feedArmEp = 7u;
        n48_dl_set(&gLedModel, 0x55ull, 0x404800000ull, 27u, 0x404800000ull, 0u, 0x800000ull);
        gLedModel.feedTok = 0u;
        Kx k; kx_save(k, gF15, 4u, k.scope, 1u); kx_retire(k, 4u, 1u);   // no n48_dl_unfeed_queue: the push was lost
        gUnfeedCalls = 0u;
        kx_drain(k);
        expect_u("T2(ii') the grant happened (forgiven 1) and its own un-feed ran once, before the note", k.rn.forgiven * 16u + gUnfeedCalls, 17u);
        expect_u("T2(ii') with the queued un-feed LOST, R1 still refuses frame 15's surface after the grant",
                 n48_dl_tiled_ok(&gLedModel, 0x55ull, 0x404800000ull, 27u), 0u);
        Kx k2; kx_save(k2, gF15, 4u, k2.scope, 1u); kx_retire(k2, 4u, 1u);
        (void)n48_rn_drain(&k2.q, &k2.st, &k2.r5, k2.scope, 21u, &k2.rn, nullptr, nullptr);
        expect_u("T2(ii') a drain with no un-feed callback grants nothing (fail-closed)", k2.rn.forgiven, 0u);
    }
}

// ---- T3 / T4: no grant -------------------------------------------------------------------------------------------------------
static void t3_t4()
{
    build_f15();
    {   // T3: a BLIND record
        Kx k; n48_r5_frame f = gF15; f.memw_ok = 0u;
        kx_save(k, f, 4u, k.scope, 1u); kx_retire(k, 4u, 1u); kx_drain(k);
        expect_u("T3 BLIND record: no grant", k.rn.forgiven, 0u);
        expect_u("T3 ... counted blind, and charged to R5' (blind 1)", k.rn.blind * 16u + k.r5.blind, 17u);
        n48_dep_src s = base_src(N48_CP_OK, 0u); kx_gather(k, s);
        expect_u("T3 ... the next frame refuses NEUTER_UNREADABLE", judge(s, nullptr), N48_DEP_NEUTER_UNREADABLE);
    }
    {   // T4 (a): no record
        Kx k; kx_retire(k, 4u, 1u); kx_drain(k);
        expect_u("T4 no record: no grant, noRecord 1", k.rn.forgiven * 16u + k.rn.noRecord, 1u);
    }
    {   // T4 (b): wrong scope (saved in arm A, drained in arm B)
        Kx k; kx_save(k, gF15, 4u, 0x302u, 1u); kx_retire(k, 4u, 1u); k.scope = 0x802u; kx_drain(k);
        expect_u("T4 wrong scope: no grant, wrongScope 1, nothing noted", k.rn.forgiven * 256u + k.rn.wrongScope * 16u + k.r5.noted, 16u);
    }
    {   // T4 (b'): scope 0 is never current
        Kx k; kx_save(k, gF15, 4u, 0u, 1u); kx_retire(k, 4u, 1u); k.scope = 0u; kx_drain(k);
        expect_u("T4 scope 0: nothing saved (saveMiss) and no grant", k.rn.forgiven * 16u + k.rn.saveMiss, 1u);
    }
    {   // T4 (c): nib mismatch / 0 / above 4
        const uint32_t sv[3] = { 2u, 0u, 5u }, qv[3] = { 1u, 0u, 5u };
        for (uint32_t i = 0; i < 3u; i++) {
            Kx k; kx_save(k, gF15, 4u, k.scope, sv[i]); kx_retire(k, 4u, qv[i]); kx_drain(k);
            char lbl[120]; std::snprintf(lbl, sizeof lbl, "T4 nib saved %u queued %u: no grant, nibBad 1", sv[i], qv[i]);
            expect_u(lbl, k.rn.forgiven * 16u + k.rn.nibBad, 1u);
        }
    }
    {   // T4 (d): a lost queue grants nothing for what it lost
        Kx k;
        for (uint32_t i = 0; i < N48_RN_QUEUE; i++) { kx_save(k, gF15, 100u + i, k.scope, 1u); kx_retire(k, 100u + i, 1u); }
        kx_save(k, gF15, 4u, k.scope, 1u); kx_retire(k, 4u, 1u);      // the ninth: LOST
        kx_drain(k);
        expect_u("T4 lost queue: overflow counted; only 105-107 granted (the store holds 4: 105, 106, 107 and the lost seq 4)", k.rn.qOverflow * 256u + k.rn.lostDrains * 16u + k.rn.granted, 256u + 16u + 3u);
        const uint32_t g4 = [&]{ for (uint32_t i = 0; i < k.rn.g_n; i++) if (k.rn.g_seq[i] == 4u) return 1u; return 0u; }();
        expect_u("T4 lost queue: seq 4 not granted", g4, 0u);
    }
    {   // a duplicated queue entry never grants twice (the record is consumed)
        Kx k; kx_save(k, gF15, 4u, k.scope, 1u); kx_retire(k, 4u, 1u); kx_retire(k, 4u, 1u); kx_drain(k);
        expect_u("T4 duplicate queue entry: one grant, the second finds no record", k.rn.granted * 16u + k.rn.noRecord, 17u);
    }
}

// ---- T5 / T6 / T7: the fill's guards and the identity ---------------------------------------------------------------------
static void t5_t6_t7()
{
    {   // T5: the legacy path (cp off): source neuters 0, a valid grant of 1 -> still RING_NEUTER
        n48_dep_src s = base_src(N48_CP_OK, 0u);
        s.cp_enabled = 0u; s.cp_enumerated = 0u; s.cp_scoped = 0u; s.r5_mode = 0u; s.d4_enabled = 0u; s.d4_enumerated = 0u;
        s.v[N48_DEPC_SRC_NEUTERED] = 0u; s.v[N48_DEPC_SRC_CALLS] = 1u;   // source neuters 0 (the identity kept)
        s.rn_enabled = 1u; s.rn_forgive = 1u; s.rn_clause = 0u;
        uint64_t d = 0u; n48_dep_world w {};
        expect_u("T5 consumer path off, forgive 1: still RING_NEUTER", judge(s, &d, &w), N48_DEP_RING_NEUTER);
        expect_u("T5 ... nothing subtracted (rn_forgiven 0)", w.rn_forgiven, 0u);
    }
    {   // T6: r5_mode 0 -> nothing subtracted
        n48_dep_src s = base_src(N48_CP_OK, 0u);
        s.r5_mode = 0u; s.r5_blind = 0u;
        s.rn_enabled = 1u; s.rn_forgive = 1u; s.rn_clause = 0u;
        uint64_t d = 0u; n48_dep_world w {};
        expect_u("T6 r5_mode 0, forgive 1: still RING_NEUTER", judge(s, &d, &w), N48_DEP_RING_NEUTER);
        expect_u("T6 ... ring_neuters still the raw 1", w.ring_neuters * 16u + w.rn_forgiven, 16u);
    }
    {   // T7: every identity case is UNACCOUNTED, and the raw RING_NOP identity still holds with a grant standing
        struct { uint32_t en, cl; uint64_t fg; const char *what; } cases[] = {
            { 0u, 0u, 1u, "T7 grant with the switch OFF -> UNACCOUNTED (0x800)" },
            { 1u, 1u, 1u, "T7 grant with verdict SUM -> UNACCOUNTED (0x800)" },
            { 1u, 2u, 1u, "T7 grant with verdict OVER -> UNACCOUNTED (0x800)" },
            { 1u, 0u, 2u, "T7 grant larger than the raw count (2 > 1) -> UNACCOUNTED (0x800)" },
            { 1u, 0u, ~0ull, "T7 a grant that would wrap -> UNACCOUNTED (0x800)" },
        };
        for (auto &c : cases) {
            n48_dep_src s = base_src(N48_CP_OK, 0u);
            s.rn_enabled = c.en; s.rn_clause = c.cl; s.rn_forgive = c.fg;
            uint64_t d = 0u; n48_dep_world w {};
            const uint32_t r = judge(s, &d, &w);
            expect_u(c.what, (uint64_t)r * 0x10000ull + (w.unaccounted & N48_DEP_ID_RNFORGIVE), (uint64_t)N48_DEP_UNACCOUNTED * 0x10000ull + N48_DEP_ID_RNFORGIVE);
        }
        // the verdict itself (the gather's rn_clause): SUM when the amount is not the sum of the grants, OVER when the
        // grants exceed the IBs switch 75 retired into the queue, OK otherwise
        expect_u("T7 n48_rn_verdict(forgiven 1, granted 0, retired 5) = SUM", n48_rn_verdict(1u, 0u, 5u), N48_RN_V_SUM);
        expect_u("T7 n48_rn_verdict(forgiven 2, granted 2, retired 1) = OVER", n48_rn_verdict(2u, 2u, 1u), N48_RN_V_OVER);
        expect_u("T7 n48_rn_verdict(1, 1, 1) = OK and (0, 0, 0) = OK", n48_rn_verdict(1u, 1u, 1u) * 16u + n48_rn_verdict(0u, 0u, 0u), N48_RN_V_OK);
        n48_dep_src s = base_src(N48_CP_OK, 0u); s.rn_enabled = 1u; s.rn_forgive = 1u;
        n48_dep_world w {};
        expect_u("T7 a valid grant: the raw RING_NOP identity still adds up (no 0x4 bit), clean", judge(s, nullptr, &w), N48_DEP_OK);
        expect_u("T7 ... unaccounted 0", w.unaccounted, 0u);
        // a gGn.ibs decremented by a grant (planted break B6) breaks RING_NOP: ARMED 1 != GN_IBS 0 + ...
        n48_dep_src s6 = base_src(N48_CP_OK, 0u); s6.v[N48_DEPC_GN_IBS] = 0u;
        n48_dep_world w6 {};
        (void)judge(s6, nullptr, &w6);
        expect_u("T7 a decremented gGn.ibs is caught by the RING_NOP identity (0x4)", w6.unaccounted & N48_DEP_ID_RING_NOP, N48_DEP_ID_RING_NOP);
    }
}

// ---- T8: retire, judge, drain, judge ---------------------------------------------------------------------------------------
static void t8()
{
    build_f15();
    Kx k; kx_save(k, gF15, 4u, k.scope, 1u);
    kx_retire(k, 4u, 1u);                            // hook_gfxCommitIB returned: queued
    n48_dep_src a = base_src(N48_CP_OK, 0u); kx_gather(k, a);   // a frame judged BEFORE any drain (another thread's decide)
    expect_u("T8 the frame judged between the retire and the drain refuses RING_NEUTER", judge(a, nullptr), N48_DEP_RING_NEUTER);
    kx_drain(k);
    n48_dep_src b = base_src(N48_CP_OK, 0u); kx_gather(k, b);
    expect_u("T8 after the drain the next frame passes", judge(b, nullptr), N48_DEP_OK);
}

// ---- T9: OFF identity against the frozen 0.0.522 fill ---------------------------------------------------------------------
static uint64_t rng_s = 0x9e3779b97f4a7c15ull;
static uint64_t rnd() { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }
static void t9()
{
    uint32_t same = 0u, n = 20000u, nonClean = 0u;
    for (uint32_t it = 0; it < n; it++) {
        n48_dep_src s = base_src((uint32_t)(rnd() % N48_CP_REASONS), rnd() % 3u);
        for (uint32_t i = 0; i < N48_DEPC_COUNT; i++) if ((rnd() & 7u) == 0u) s.v[i] = rnd() % 4u;
        uint32_t *u32[] = { &s.src_install, &s.ring_state, &s.ring_hooked, &s.sdma_state, &s.stall_armed, &s.stall_read_ok,
                            &s.pre_walked, &s.pre_wrapped, &s.pre_stopped, &s.pre_ibs, &s.pre_e1, &s.snap_ok, &s.q_hook_live,
                            &s.q_tally_ok, &s.fault_read_ok, &s.fg_enabled, &s.fg_clause, &s.cp_enabled, &s.cp_enumerated,
                            &s.cp_scoped, &s.r5_mode, &s.d4_enabled, &s.d4_enumerated };
        for (uint32_t *p : u32) if ((rnd() & 15u) == 0u) *p = (uint32_t)(rnd() % 3u);
        if ((rnd() & 7u) == 0u) s.fg_forgive = rnd() % 3u;
        if ((rnd() & 7u) == 0u) s.r5_blind = rnd() % 2u;
        if ((rnd() & 7u) == 0u) s.cp_unknown_ws = rnd() % 2u;
        // switch 77 OFF: the gather hands rn_enabled 0, rn_forgive 0 (never ON this boot), rn_clause n48_rn_verdict(0,0,0) = 0
        s.rn_enabled = 0u; s.rn_forgive = 0u; s.rn_clause = n48_rn_verdict(0u, 0u, (rnd() & 1u) ? 0u : rnd() % 5u);
        n48_dep_mono m1 {}, m2 {};
        n48_dep_world a {}, b {};
        n48_dep_fill(&s, &m1, &a);
        n48_dep_fill_0522(&s, &m2, &b);
        uint64_t da = 0u, db = 0u;
        const uint32_t ra = n48_dep_check(&a, &da), rb = n48_dep_check(&b, &db);
        if (ra != N48_DEP_OK) nonClean++;
        if (!std::memcmp(&a, &b, sizeof a) && ra == rb && da == db) same++;
    }
    expect_u("T9 OFF: 20000 random worlds - today's fill is byte-identical to the frozen 0.0.522 fill, reason and detail", same, n);
    expect_u("T9 non-vacuity: the random worlds reach refusals (> 1000 not clean)", nonClean > 1000u, 1u);
}

// ---- the source-order / reachability test and C3's widths -----------------------------------------------------------------
static std::string slurp(const char *p)
{
    std::string s; FILE *f = std::fopen(p, "rb"); if (!f) return s;
    char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); return s;
}
static size_t count(const std::string &h, const char *n) { size_t c = 0; for (size_t a = h.find(n); a != std::string::npos; a = h.find(n, a + 1)) c++; return c; }
static void source_order(const char *ahhPath)
{
    const std::string s = slurp(ahhPath);
    expect_u("SRC AppleHardwareHook.cpp read", s.size() > 100000u, 1u);
    if (s.size() < 100000u) return;
    const size_t fn = s.find("static uint32_t gfxsrc_decide_frame(const uint8_t *info, uint32_t shapeOk, uint32_t n, const WsFrame *wf) {");
    const size_t unfeed = s.find("if (dp) (void)n48_dl_unfeed_drain(&gXdLed, &gXdLedUnfeedQ);", fn);
    const size_t scope = s.find("if (gXpOn) gXpScopeSeq = dp ? ((arm + 1u) | (dpEp << 8)) : 0u;", fn);
    const size_t drain = s.find("if (gRnOn) gfxsrc_rn_drain();", fn);
    const size_t rpd = s.find("if (dp && gXdResProv) gfxsrc_rp_drain_locked();", fn);
    const size_t loop = s.find("for (uint32_t k = 0; k < f.nib && f.reader_ok; k++) {", fn);
    const size_t commit = s.find("? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;", fn);
    const size_t feed = s.find("const uint32_t added = n48_dl_feed(&gXdLed, &lf);", fn);
    const size_t save = s.find("gfxsrc_rn_save(vm, n, r5ShortRead, action, commitOk);", fn);
    const size_t nextFn = s.find("\n}\n", fn);
    expect_u("SRC the drain call exists exactly once", count(s, "gfxsrc_rn_drain();"), 1u);
    expect_u("SRC ORDER: n48_dl_unfeed_drain < gXpScopeSeq assignment < the rn drain < the resprov drain < the IB read loop < commit_try",
             fn != std::string::npos && unfeed > fn && scope > unfeed && drain > scope && rpd > drain && loop > rpd && commit > loop, 1u);
    expect_u("SRC the drain is the statement right after the scope assignment (nothing between them)",
             drain > scope && s.find_first_not_of(" \n", scope + std::strlen("if (gXpOn) gXpScopeSeq = dp ? ((arm + 1u) | (dpEp << 8)) : 0u;")) == drain, 1u);
    expect_u("SRC the save is the statement right after n48_dl_feed, in gfxsrc_decide_frame after commit_try",
             feed != std::string::npos && save > feed && save > commit && s.find_first_not_of(" \n", feed + std::strlen("const uint32_t added = n48_dl_feed(&gXdLed, &lf);")) == save, 1u);
    (void)nextFn;
    // the queue: inside `if (sparedN == 0u)`, after the proof's retire, and it does NOT grant
    const size_t spared = s.find("if (sparedN == 0u) {");
    const size_t retire = s.find("wnDone = n48_fr_retire_nopped(&gKsRing, seq);", spared);
    const size_t q = s.find("if (wnDone && gRnOn) { n48_rn_queue(&gRnQ, &gRn, seq, n); gRn.retiredIbs += n; }", spared);
    expect_u("SRC the queue site exists exactly once, gated on wnDone (75's PROOF), inside `if (sparedN == 0u)`, after n48_fr_retire_nopped",
             count(s, "n48_rn_queue(") == 1u && spared != std::string::npos && retire != std::string::npos && q != std::string::npos &&
             retire > spared && q > retire, 1u);
    // fix pass: the drain passes the un-feed callback, and the callback un-feeds the ledger by the seq
    expect_u("SRC the drain passes rn_unfeed_cb, which calls n48_dl_unfeed_tok(&gXdLed, seq) under gXdDescPort",
             count(s, "(void)n48_rn_drain(&gRnQ, &gRnStore, &gR5Ring, gXpScopeSeq, gXdC.judged + 1u, &gRn, &rn_unfeed_cb, nullptr);") +
             count(s, "static void rn_unfeed_cb(void *, uint32_t seq) { if (gXdDescPort) (void)n48_dl_unfeed_tok(&gXdLed, seq); }"), 2u);
    expect_u("SRC gRn.forgiven is never WRITTEN in the kext (only n48_rn_drain writes it)",
             count(s, "gRn.forgiven +=") + count(s, "gRn.forgiven =") + count(s, "gRn.forgiven++") + count(s, "gRn.grantedIbs +="), 0u);
    // gGn.ibs has exactly one writer, the walk's `gGn.ibs += done` (planted break B6: a grant that decrements it)
    expect_u("SRC gGn.ibs is written once (`gGn.ibs += done;`), never decremented",
             count(s, "gGn.ibs += done;") * 16u + count(s, "gGn.ibs -=") + count(s, "gGn.ibs--") + count(s, "gGn.ibs = "), 16u);
    // the gather hands the fill the three scalars, never v[]
    expect_u("SRC the gather: s.rn_enabled / s.rn_forgive / s.rn_clause from gRnOn / gRn.forgiven / n48_rn_verdict",
             count(s, "s.rn_enabled = gRnOn ? 1u : 0u;") + count(s, "s.rn_forgive = gRn.forgiven;") +
             count(s, "s.rn_clause = n48_rn_verdict(gRn.forgiven, gRn.grantedIbs, gRn.retiredIbs);"), 3u);
    // the save resolves exactly as the held-back site does, with out_of_scope forced 0
    const size_t sv = s.find("static __attribute__((noinline)) void gfxsrc_rn_save(");
    const size_t svEnd = s.find("\n}\n", sv);
    const std::string body = sv == std::string::npos ? std::string() : s.substr(sv, svEnd - sv);
    expect_u("SRC gfxsrc_rn_save: TRANSLATE + gate seq + 28 + 30 + a scope, out_of_scope 0, n48_r5_resolve via r5_resolve_cb, the short-read rule",
             body.find("action != N48_SD_ACT_TRANSLATE || !gXdCmGateSeq || !gXpOn || !gR5On || !gXpScopeSeq") != std::string::npos &&
             body.find("r5f.out_of_scope = 0u;") != std::string::npos && body.find("n48_r5_resolve(&r5f, r5_resolve_cb, &r5c);") != std::string::npos &&
             body.find("if (shortRead) r5f.ib_ok = 0u;") != std::string::npos, 1u);
    expect_u("SRC gfxsrc_rn_save saves ONLY a committed frame: its first statement returns unless gRnOn AND commitOk",
             body.find("{\n    if (!gRnOn || !commitOk) return;\n") != std::string::npos, 1u);
    // the switch: 77 guarded, OFF at boot
    expect_u("SRC switch 77: OFF at boot and set only through n48_ra_set under the mid-arm guard",
             count(s, "static volatile uint32_t gRnOn { 0u };") + count(s, "n48_cm_cont_switch_refused(77u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") +
             count(s, "if (changed77) gRnOn = f77;"), 3u);
    // the header: `forgiven +=` exactly once, in n48_rn_drain after the note
    const std::string h = slurp((std::string(ahhPath).substr(0, std::string(ahhPath).rfind('/') + 1) + "gfx_rnforgive.h").c_str());
    const size_t dr = h.find("static inline uint64_t n48_rn_drain(");
    const size_t note = h.find("n48_r5_note_counted(r5, scopeSeq, &r->f, namer, 0u, 0u, 0ull);", dr);
    const size_t grant = h.find("s->forgiven += (uint64_t)r->nib;", dr);
    const size_t unf = h.find("unfeed(ud, seq);", dr);
    expect_u("SRC gfx_rnforgive.h: `forgiven +=` exactly once, inside n48_rn_drain, AFTER the un-feed and AFTER the R5' note",
             count(h, "s->forgiven +=") == 1u && dr != std::string::npos && unf != std::string::npos && unf > dr && note > unf && grant > note, 1u);
}

static size_t width(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static size_t width(const char *fmt, ...)
{
    static char b[4096]; va_list ap; va_start(ap, fmt); const int n = std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    return n < 0 ? 9999u : (size_t)n;
}
static void widths()
{
    const unsigned long long M = ~0ull; const unsigned U = 0xFFFFFFFFu;
    const char *longSt = "ON but INERT (75, 28 or 30 is OFF)";
    const char *longVerb = " - `gfxneuter 77` REFUSED: a continuous arm stands, unchanged";
    const size_t a = width(N48_RN_REPORT_FMT, longSt, M, M, M, M, M, M, M, M, M, M, M, M);
    const size_t a2 = width(N48_RN_REPORT2_FMT, longVerb, M, M, M, U);
    expect_u("C3 the second `rnforgive:` report line at maximal fields is <= 511 bytes", a2 <= 511u, 1u);
    const size_t b = width(N48_RN_GRANT_FMT, U, U, U, U, M, M);
    const size_t c = width(N48_RN_X9_FMT, "OFF (the default; nothing is forgiven)", M, M, M, U, U, U);
    std::printf("      widths: report %zu grant %zu x9 %zu\n", a, b, c);
    expect_u("C3 the `rnforgive:` report line at maximal fields is <= 511 bytes", a <= 511u, 1u);
    expect_u("C3 the per-grant line at maximal fields is <= 511 bytes", b <= 511u, 1u);
    expect_u("C3 the X9 ring-neuter line at maximal fields is <= 511 bytes", c <= 511u, 1u);
}

int main(int argc, char **argv)
{
    t1(); t2(); t3_t4(); t5_t6_t7(); t8(); t9();
    if (argc > 1) source_order(argv[1]); else { gRun++; gFail++; std::printf("FAIL  no AppleHardwareHook.cpp path given\n"); }
    widths();
    std::printf("gfx_rnforgive: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
