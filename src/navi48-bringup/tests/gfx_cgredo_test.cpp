// gfx_cgredo_test.cpp — build 0.0.523 (notes/design/RING-NEUTER-FORGIVE.md, rev 2 governs, items 8-12; ,
//): SWITCH 78, the copy-guard redo - gfx_copyguard.h (per-page since, n48_cg_check_ex, the plan, the bounded wait, the
// rebase and the ordered sequence), gfx_cgredo.h (the undo), the kext glue's ordering, and reviewer items C1-C3.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ src/navi48-bringup/tests/gfx_cgredo_test.cpp \
//         src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -o /tmp/cgr && /tmp/cgr \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/Navi48Bringup.cpp
// Covers C1-C6 (C1: per_page 0 == the frozen 0.0.522 n48_cg_check; C2: an EVENT completed before the redo mark on a page new
// to segment k passes; C3: the same on an earlier segment's page still refuses; C4: a copy BEGIN/END between the mark and the
// check is EVENT, an open slot IN_FLIGHT; C5: every "no" of the plan and the wait bound; C6: poison survives), the undo
// (Apple's bytes, the pool AND spill journals - reviewer C1 - the frame-local additions, the carry), the source order
// (reachability; reviewer C2: the redo runs inside the policy pass, the spill flush only in gfxsrc_commit_try after it), and
// the widths of every new 0.0.523 line at maximal fields (reviewer C3).
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdarg>
#include <string>
#include "gfx_cgredo.h"
#include "gfx_cgw108.h"   // build 0.0.550: switch 108, the copy-guard wait under switch 37
#include "fixture_cg_check_0522.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-100s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-100s %#llx\n", what, (unsigned long long)got);
}

// ---- a model world: the kext's statics, host-side -------------------------------------------------------------------------
static n48_cg_slot gSlots[N48_CG_SLOTS];
static n48_cg_ring gRing;
static n48_cg_poison gPoison;
static n48_cg_pagerec gRec;
static uint64_t gPassSince;
static const uintptr_t kCopier = 0x2222u;
static void world_reset()
{
    n48_cg_slot_init(gSlots, N48_CG_SLOTS); n48_cg_ring_init(&gRing); n48_cg_poison_init(&gPoison);
    gPassSince = n48_cg_ring_mark(&gRing);
    n48_cg_pagerec_reset_at(&gRec, gPassSince);            // navi48_cg_seg_begin
}
static uint32_t check(n48_cg_why *w = nullptr) { return n48_cg_check_ex(&gRec, &gPoison, gSlots, N48_CG_SLOTS, &gRing, gPassSince, 0u, w); }
// a whole residency copy of [lo, hi): open (BEGIN), close (END), as navi48_cg_open/close order them
static void copy_full(uint64_t lo, uint64_t hi, uintptr_t who = kCopier)
{
    const int32_t sl = n48_cg_slot_open(gSlots, N48_CG_SLOTS, lo, hi, who);
    n48_cg_ring_push_o(&gRing, N48_CG_EV_BEGIN, (uint32_t)sl, lo, hi, who);
    n48_cg_ring_push_o(&gRing, N48_CG_EV_END, (uint32_t)sl, lo, hi, who);
    n48_cg_slot_close(gSlots, sl);
}
static const uint64_t A = 0x10000000ull, B = 0x20000000ull;

// The redo's two steps for the model: the undo (nothing to take back in the recorder model) and the re-translate, which
// re-reads the segment's pages - and optionally lets a copy land during the read.
struct Redo { uint32_t readA, readB, copyDuringB, openDuringB; uint32_t undos; };
static void m_undo(void *ud) { static_cast<Redo *>(ud)->undos++; }
static uint32_t m_xlat(void *ud)
{
    Redo *r = static_cast<Redo *>(ud);
    if (r->readA) n48_cg_pagerec_note(&gRec, A, 64u);
    if (r->copyDuringB) copy_full(B, B + 0x1000u);       // a copy that begins and ends DURING the redo's read of B
    if (r->openDuringB) { const int32_t sl = n48_cg_slot_open(gSlots, N48_CG_SLOTS, B, B + 0x1000u, kCopier);
                          n48_cg_ring_push_o(&gRing, N48_CG_EV_BEGIN, (uint32_t)sl, B, B + 0x1000u, kCopier); }
    if (r->readB) n48_cg_pagerec_note(&gRec, B, 64u);
    return 0u;
}
// A pass: segment 0 reads A; segment k (nk0 = 1) reads B; `copyOn` = which page a copy rewrote after its read.
static uint32_t pass_and_redo(uint64_t copyOn, uint32_t redo, Redo rd, uint32_t *before)
{
    world_reset();
    n48_cg_pagerec_note(&gRec, A, 64u);                  // segment 0's read (its check passed then)
    const uint32_t nk0 = gRec.n;                         // gfxsrc_cg_redo_pre: segment k's nk0
    n48_cg_pagerec_note(&gRec, B, 64u);                  // segment k's first translate reads B
    if (copyOn) copy_full(copyOn, copyOn + 0x1000u);     // a copy rewrote it and completed (END pushed) - an EVENT
    n48_cg_why w {};
    *before = check(&w);                                 // the peek
    if (redo) (void)n48_cg_redo_seq(&gRec, &gRing, nk0, &m_undo, &m_xlat, &rd, nullptr);
    return check();                                      // the counted check at the original site
}

// ---- C1: per_page 0 is the frozen 0.0.522 check ---------------------------------------------------------------------------
static uint64_t rng_s = 0x2545F4914F6CDD1Dull;
static uint64_t rnd() { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return rng_s; }
static void c1()
{
    uint32_t same = 0u, n = 20000u, refusals = 0u;
    for (uint32_t it = 0; it < n; it++) {
        n48_cg_slot_init(gSlots, N48_CG_SLOTS); n48_cg_ring_init(&gRing); n48_cg_poison_init(&gPoison);
        const uint32_t pre = (uint32_t)(rnd() % 300u);
        for (uint32_t i = 0; i < pre; i++) n48_cg_ring_push(&gRing, N48_CG_EV_WRITE, ~0u, (rnd() % 64u) << 12, ((rnd() % 64u) << 12) + 0x1000u);
        const uint64_t since = n48_cg_ring_mark(&gRing) - (rnd() % 8u == 0u ? (rnd() % 300u) : 0u);
        n48_cg_pagerec_reset_at(&gRec, since);
        gRec.cur_since = since + rnd() % 400u;   // per_page stays 0: since[] must NOT be read (the OFF path's own non-vacuity)
        const uint32_t np = (uint32_t)(rnd() % 6u);
        for (uint32_t i = 0; i < np; i++) n48_cg_pagerec_note(&gRec, (rnd() % 64u) << 12, 16u);
        if ((rnd() % 50u) == 0u) gRec.overflow = 1u;
        const uint32_t ev = (uint32_t)(rnd() % 4u);
        for (uint32_t i = 0; i < ev; i++) n48_cg_ring_push(&gRing, N48_CG_EV_BEGIN, 0u, (rnd() % 64u) << 12, ((rnd() % 64u) << 12) + 0x1000u);
        if ((rnd() % 5u) == 0u) (void)n48_cg_slot_open(gSlots, N48_CG_SLOTS, (rnd() % 64u) << 12, ((rnd() % 64u) << 12) + 0x2000u, kCopier);
        if ((rnd() % 7u) == 0u) n48_cg_poison_mark(&gPoison, (rnd() % 64u) << 12, ((rnd() % 64u) << 12) + 0x1000u);
        if ((rnd() % 9u) == 0u) gRing.s[rnd() % N48_CG_RING].stamp = 0ull;   // a torn slot somewhere
        const uint32_t untracked = (rnd() % 40u) == 0u ? 1u : 0u;
        const uint32_t a = n48_cg_check_0522(&gRec, &gPoison, gSlots, N48_CG_SLOTS, &gRing, since, untracked);
        const uint32_t b = n48_cg_check(&gRec, &gPoison, gSlots, N48_CG_SLOTS, &gRing, since, untracked);
        n48_cg_why w {};
        const uint32_t c = n48_cg_check_ex(&gRec, &gPoison, gSlots, N48_CG_SLOTS, &gRing, since, untracked, &w);
        if (a != N48_CG_OK) refusals++;
        if (a == b && a == c && w.reason == c) same++;
    }
    expect_u("C1 per_page 0: n48_cg_check and n48_cg_check_ex(why) == the frozen 0.0.522 check over 20000 random schedules", same, n);
    expect_u("C1 non-vacuity: > 2000 of them refuse", refusals > 2000u, 1u);
}

// ---- C2 / C3 / C4 / C6 -----------------------------------------------------------------------------------------------------
static void c2_c3_c4_c6()
{
    uint32_t before = 0u;
    {   // C2
        Redo rd {}; rd.readB = 1u;
        const uint32_t off = pass_and_redo(B, 0u, rd, &before);
        expect_u("C2 OFF: an EVENT on segment k's own new page (a copy that completed after the read) refuses", off, N48_CG_EVENT);
        const uint32_t on = pass_and_redo(B, 1u, rd, &before);
        expect_u("C2 ON: the peek said EVENT", before, N48_CG_EVENT);
        expect_u("C2 ON: after the redo (truncate, per-page mark, re-read) the full check PASSES", on, N48_CG_OK);
        expect_u("C2 ON: ... and the recorder re-read B with the redo's mark as its since (per_page 1, 2 pages)",
                 gRec.per_page * 16u + gRec.n, 18u);
    }
    {   // C3
        Redo rd {}; rd.readA = 1u; rd.readB = 1u;
        const uint32_t on = pass_and_redo(A, 1u, rd, &before);
        expect_u("C3 ON: the same EVENT on an EARLIER segment's page (index < nk0) still refuses after the redo", on, N48_CG_EVENT);
        n48_cg_why w {}; (void)check(&w);
        expect_u("C3 ... on page A, scanned from the PASS mark", (w.page == A && w.since == gPassSince) ? 1u : 0u, 1u);
    }
    {   // C4 (a): a copy that BEGINs and ENDs between the redo mark and the check
        Redo rd {}; rd.readB = 1u; rd.copyDuringB = 1u;
        const uint32_t on = pass_and_redo(B, 1u, rd, &before);
        expect_u("C4 a copy BEGIN/END between the redo mark and the check -> EVENT", on, N48_CG_EVENT);
        n48_cg_why w {}; (void)check(&w);
        expect_u("C4 ... the event is at or after the redo's mark (the page's since)", (w.ev >= w.since && w.since > gPassSince) ? 1u : 0u, 1u);
    }
    {   // C4 (b): a copy still open at the check
        Redo rd {}; rd.readB = 1u; rd.openDuringB = 1u;
        const uint32_t on = pass_and_redo(B, 1u, rd, &before);
        expect_u("C4 a slot still open over the re-read page at the check -> IN_FLIGHT", on, N48_CG_IN_FLIGHT);
        n48_cg_why w {}; (void)check(&w);
        expect_u("C4 ... why names the slot's owner (the copier, not us)", w.owner == kCopier ? 1u : 0u, 1u);
    }
    {   // C4 (c): the peek admits nothing - a peek that says EVENT, with NO redo run, is still refused by the counted check
        Redo rd {}; rd.readB = 1u;
        const uint32_t off = pass_and_redo(B, 0u, rd, &before);
        expect_u("C4 the peek is not an admit: peek EVENT and no redo -> the check refuses EVENT", before * 16u + off, (uint64_t)N48_CG_EVENT * 17u);
    }
    {   // C6: poison survives the redo
        world_reset();
        n48_cg_pagerec_note(&gRec, A, 64u);
        const uint32_t nk0 = gRec.n;
        n48_cg_pagerec_note(&gRec, B, 64u);
        copy_full(B, B + 0x1000u);
        n48_cg_poison_mark_sticky(&gPoison, B, B + 0x1000u);   // a copy whose fence did not land: sticky
        Redo rd {}; rd.readB = 1u;
        (void)n48_cg_redo_seq(&gRec, &gRing, nk0, &m_undo, &m_xlat, &rd, nullptr);
        expect_u("C6 a poisoned page stays POISONED after the redo", check(), N48_CG_POISONED);
    }
    {   // the sequence's order: the undo runs FIRST, then the mark, then the re-read
        Redo rd {}; rd.readB = 1u;
        world_reset(); n48_cg_pagerec_note(&gRec, B, 64u);
        uint64_t mark = 0u;
        (void)n48_cg_redo_seq(&gRec, &gRing, 0u, &m_undo, &m_xlat, &rd, &mark);
        expect_u("SEQ the undo ran once and the new page's since is the returned mark", rd.undos * 16u + (gRec.since[0] == mark ? 1u : 0u), 17u);
    }
}

// ---- C5: the plan's every "no", and the wait bound -------------------------------------------------------------------------
static int gBusyCalls = 0, gBusyUntil = 0;
static int f_busy(void *) { gBusyCalls++; return gBusyCalls < gBusyUntil; }
static void f_delay(void *, uint32_t) {}
static void c5()
{
    n48_cg_redo_in base {};
    base.on = 1u; base.reason = N48_CG_EVENT; base.pass = 0u; base.is_unit = 1u;
    uint32_t why = 0u;
    expect_u("C5 EVENT, pass 0, a unit, first redo -> REDO", n48_cg_redo_plan(&base, &why), N48_CG_RD_REDO);
    { auto in = base; in.is_unit = 0u; expect_u("C5 a single -> REDO", n48_cg_redo_plan(&in, &why), N48_CG_RD_REDO); }
    { auto in = base; in.on = 0u; expect_u("C5 switch OFF -> no", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_OFF); }
    const uint32_t others[] = { N48_CG_OK, N48_CG_POISONED, N48_CG_TORN, N48_CG_WRAP, N48_CG_UNTRACKED, N48_CG_OVERFLOW };
    for (uint32_t r : others) {
        auto in = base; in.reason = r;
        char lbl[96]; std::snprintf(lbl, sizeof lbl, "C5 reason %s -> no", n48_cg_reason_name(r));
        expect_u(lbl, n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_REASON);
    }
    { auto in = base; in.pass = 1u; expect_u("C5 switch 70's pass 1 -> no", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_PASS1); }
    { auto in = base; in.is_unit = 2u; expect_u("C5 a switch-56 retried single (isUnit 2) -> no", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_UNIT); }
    { auto in = base; in.seg_redone = 1u; expect_u("C5 a second redo of the same segment -> no", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_SEG_DONE); }
    { auto in = base; in.frame_redos = N48_CG_REDO_FRAME_MAX; expect_u("C5 the frame's 4 redos spent -> no", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_FRAME_CAP); }
    { auto in = base; in.frame_redos = N48_CG_REDO_FRAME_MAX - 1u; expect_u("C5 the 4th redo of a frame -> REDO", n48_cg_redo_plan(&in, &why), N48_CG_RD_REDO); }
    { auto in = base; in.reason = N48_CG_IN_FLIGHT; in.mm37 = 1u; expect_u("C5 IN_FLIGHT under 37 -> no wait, no redo", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_MM37); }
    { auto in = base; in.reason = N48_CG_IN_FLIGHT; in.self_owned = 1u; expect_u("C5 IN_FLIGHT on a slot this thread owns -> no", n48_cg_redo_plan(&in, &why) * 16u + why, N48_CG_RDW_SELF); }
    { auto in = base; in.reason = N48_CG_IN_FLIGHT; expect_u("C5 IN_FLIGHT, 37 OFF, another thread's copy -> WAIT", n48_cg_redo_plan(&in, &why), N48_CG_RD_WAIT); }
    { auto in = base; in.reason = N48_CG_EVENT; in.mm37 = 1u; expect_u("C5 EVENT under 37 -> REDO (37 only stops the IN_FLIGHT wait)", n48_cg_redo_plan(&in, &why), N48_CG_RD_REDO); }
    // the bound
    expect_u("C5 wait_more(2000, 2000) = 0 (the budget is a hard bound)", n48_cg_wait_more(2000u, 2000u), 0u);
    expect_u("C5 wait_more(1950, 2000) = 1", n48_cg_wait_more(1950u, 2000u), 1u);
    gBusyCalls = 0; gBusyUntil = 1000000; uint64_t waited = 0u;
    const int clr = n48_cg_redo_wait(&f_busy, &f_delay, nullptr, N48_CG_REDO_WAIT_US, N48_CG_REDO_POLL_US, &waited);
    expect_u("C5 a copy that never closes: the wait times out (0) within the 2000 us budget", (uint64_t)clr * 0x100000u + (waited <= N48_CG_REDO_WAIT_US ? 1u : 0u), 1u);
    expect_u("C5 ... after at most 41 polls", gBusyCalls <= 41 ? 1u : 0u, 1u);
    gBusyCalls = 0; gBusyUntil = 3; waited = 0u;
    expect_u("C5 a copy that closes after 2 polls: clear, waited 100 us", (uint64_t)n48_cg_redo_wait(&f_busy, &f_delay, nullptr, 2000u, 50u, &waited) * 0x10000u + waited, 0x10000u + 100u);
    gBusyCalls = 0; gBusyUntil = 1000000; waited = 0u;
    expect_u("C5 a spent frame budget (left 0): no wait at all, timeout", (uint64_t)n48_cg_redo_wait(&f_busy, &f_delay, nullptr, 0u, 50u, &waited) * 16u + waited, 0u);
}

// ---- the undo (D5 / Q4, Q6, reviewer C1) ---------------------------------------------------------------------------------
static void undo()
{
    static uint32_t apple[64], cand[64], poolHost[16], spillHost[16];
    for (uint32_t i = 0; i < 64u; i++) { apple[i] = 0xA0000000u + i; cand[i] = 0xC0000000u + i; }
    for (uint32_t i = 0; i < 16u; i++) { poolHost[i] = 0xB00u + i; spillHost[i] = 0x5B00u + i; }
    static xlat12_pool pool, spill; pool = xlat12_pool {}; spill = xlat12_pool {};
    pool.nrun = 1u; pool.run[0].host = poolHost + 4; pool.run[0].va = 0x1010ull; pool.run[0].len = 12u;     // after placing 4
    pool.jn = 1u; pool.j[0].host = poolHost; pool.j[0].va = 0x1000ull; pool.j[0].len = 16u; pool.j[0].used = 4u; pool.j[0].r = 0u;
    spill.nrun = 1u; spill.run[0].host = spillHost + 3; spill.run[0].va = 0x9010ull; spill.run[0].len = 13u;
    spill.jn = 1u; spill.j[0].host = spillHost; spill.j[0].va = 0x9000ull; spill.j[0].len = 16u; spill.j[0].used = 3u; spill.j[0].r = 0u;
    static xlat12_unit U; U = xlat12_unit {}; U.spill = &spill;
    static n48_dl fl; fl = n48_dl {};
    fl.n = 2u; fl.e[0].ctx = 1u; fl.e[0].va = 0x100ull; fl.e[1].ctx = 1u; fl.e[1].va = 0x200ull;
    static xlat12_ud_carry carry; carry = xlat12_ud_carry {};
    static n48_cg_redo_pre pre; pre = n48_cg_redo_pre {};
    n48_cg_redo_snap(&pre, 3u, &fl, &carry);                     // before segment 3's first translate
    // the first translate: added a frame-local entry, wrote the candidate, placed 4 pool + 3 spill dwords, touched the carry
    fl.e[2].ctx = 1u; fl.e[2].va = 0x300ull; fl.n = 3u;
    for (uint32_t i = 0; i < 4u; i++) poolHost[i] = 0xDEAD0000u + i;
    for (uint32_t i = 0; i < 3u; i++) spillHost[i] = 0x5EED0000u + i;
    std::memset(&carry, 0x5a, sizeof carry);
    expect_u("UNDO the snapshot is for segment 3 only", (uint64_t)n48_cg_redo_undo_ok(&pre, 3u) * 16u + n48_cg_redo_undo_ok(&pre, 4u), 16u);
    const uint32_t dw = n48_cg_redo_undo(cand, apple, 8u, 40u, 1, 1u, &pool, &U, &pre, &fl, &carry);
    uint32_t bytesOk = 1u;
    for (uint32_t i = 8u; i < 40u; i++) if (cand[i] != apple[i]) bytesOk = 0u;
    if (cand[7] != 0xC0000007u || cand[40] != 0xC0000028u) bytesOk = 0u;
    expect_u("UNDO (D5) Apple's bytes are back over exactly [from, to)", bytesOk, 1u);
    expect_u("UNDO (Q6) the unit's pool records are taken back: journal empty, the run restored, 4 NOPs", pool.jn * 0x1000u + pool.run[0].len * 16u + (poolHost[0] == XLAT12_IB_NOP && poolHost[3] == XLAT12_IB_NOP), 16u * 16u + 1u);
    expect_u("UNDO (reviewer C1) the spill tier's records are taken back too: journal empty, 3 NOPs, no spill record left",
             spill.jn * 0x1000u + spill.run[0].len * 16u + (spillHost[0] == XLAT12_IB_NOP && spillHost[2] == XLAT12_IB_NOP), 16u * 16u + 1u);
    expect_u("UNDO pool + spill dwords undone = 7", dw, 7u);
    expect_u("UNDO the frame-local list lost only the attempt's addition (2 entries, 0x100 and 0x200)", fl.n * 16u + (fl.e[0].va == 0x100ull && fl.e[1].va == 0x200ull), 33u);
    xlat12_ud_carry z {};
    expect_u("UNDO the carry is the snapshot's", std::memcmp(&carry, &z, sizeof z) == 0 ? 1u : 0u, 1u);
    // a single (isUnit 0) never touches a pool
    pool.jn = 1u; pool.j[0].used = 4u; for (uint32_t i = 0; i < 4u; i++) poolHost[i] = 0xDEAD0000u + i;
    (void)n48_cg_redo_undo(cand, apple, 8u, 40u, 1, 0u, &pool, &U, &pre, &fl, &carry);
    expect_u("UNDO a single's undo leaves any pool journal alone (only units place pool records)", pool.jn, 1u);
}

// ---- source order (reachability, reviewer C2) ----------------------------------------------------------------------------
static std::string slurp(const char *p)
{
    std::string s; FILE *f = std::fopen(p, "rb"); if (!f) return s;
    char b[65536]; size_t n; while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n); std::fclose(f); return s;
}
static size_t count(const std::string &h, const char *n) { size_t c = 0; for (size_t a = h.find(n); a != std::string::npos; a = h.find(n, a + 1)) c++; return c; }
static std::string fn_body(const std::string &s, const char *head)
{
    const size_t a = s.find(head); if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a); return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static void source_order(const char *ahhPath, const char *nbPath)
{
    const std::string s = slurp(ahhPath), nb = slurp(nbPath);
    expect_u("SRC both sources read", s.size() > 100000u && nb.size() > 10000u, 1u);
    if (s.size() < 100000u || nb.size() < 10000u) return;
    const std::string pol = fn_body(s, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa,");
    const size_t segBegin = pol.find("navi48_cg_seg_begin();");
    const size_t latch = pol.find("gfxsrc_cg_redo_begin();");
    const size_t loop = pol.find("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {");
    const size_t ask = pol.find("n48_mib_defer_ask_clear(&gUnitDefer);   // build 0.0.511 (switch 70): this attempt's failed asks only");
    const size_t pre = pol.find("gfxsrc_cg_redo_pre(k);");
    const size_t xl = pol.find("uint32_t st = xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from], to - from,");
    const size_t take = pol.find("if (gUnitDefer.on && gfxsrc_defer_take(k, from, to, build, unitMap, isUnit, st, ds.err_op, ds.prov_va)) continue;");
    const size_t redo = pol.find("if (!st && gCgRedoF.on) st = gfxsrc_cg_redo(&ex, k, from, to, out, &olen, &ds, isUnit, unitMap, dp, build);");
    const size_t acc = pol.find("if (st == 0u) gXdUdReemitS.reemitN += ds.ud_reemit_n;");
    const size_t chk = pol.find("if (!st) {\n            const uint32_t cgReason = navi48_cg_seg_check();\n            gfxsrc_cg_checked(k, ns, cgReason);");
    expect_u("SRC the redo call exists exactly once, inside gfxsrc_policy", count(s, "st = gfxsrc_cg_redo(") * 16u + (redo != std::string::npos), 17u);
    expect_u("SRC ORDER (policy): seg_begin < the latch < the loop < redo_pre < ask_clear < the first translate",
             segBegin != std::string::npos && latch > segBegin && loop > latch && pre > loop && ask > pre && xl > ask, 1u);
    expect_u("SRC ORDER (reachability): defer_take's `continue` < the redo < the first `st == 0` accumulation < the counted check",
             take != std::string::npos && redo > take && acc > redo && chk > acc, 1u);
    expect_u("SRC the redo is the statement right after defer_take (only its comment between)",
             redo > take && pol.substr(take, redo - take).find(";\n") == pol.substr(take, redo - take).rfind(";\n"), 1u);
    expect_u("SRC the counted check is unconditional on the redo (`if (!st)` alone) and there is exactly one",
             count(s, "const uint32_t cgReason = navi48_cg_seg_check();") * 16u + (chk != std::string::npos), 17u);
    // the helper: peek (uncounted), plan, undo_ok, wait, then the ordered sequence
    const std::string h = fn_body(s, "static __attribute__((noinline)) uint32_t gfxsrc_cg_redo(");
    expect_u("SRC gfxsrc_cg_redo: the uncounted peek, the pure plan, the snapshot check, the bounded wait, n48_cg_redo_seq",
             h.find("navi48_cg_seg_peek(&gCgRedoWhy)") != std::string::npos && h.find("n48_cg_redo_plan(&in, &why)") != std::string::npos &&
             h.find("n48_cg_redo_undo_ok(&gCgRedoPre, k)") != std::string::npos &&
             h.find("n48_cgw_wait(&cg_redo_busy_cb, &cg_redo_delay_cb, &cg_redo_now_cb, nullptr, left, N48_CG_REDO_POLL_US,\n                                 N48_CGW_WALL_CAP_US, &waited);") != std::string::npos &&
             h.find("n48_cg_redo_seq(navi48_cg_active_recorder(), navi48_cg_ring_ptr(), gCgRedoF.nk0, &cg_redo_undo_cb,") != std::string::npos &&
             h.find("navi48_cg_seg_check") == std::string::npos, 1u);
    {   // fix pass SHOULD 1: a refusal on an EARLIER segment's page (or none) is not redone - before the plan
        const size_t fut = h.find("if (!gCgRedoWhy.has || gCgRedoWhy.idx < gCgRedoF.nk0) { gCgRedoS.ineligible++; return 0u; }");
        expect_u("SRC gfxsrc_cg_redo: a page with index < nk0 (earlier segment) or no page is ineligible, BEFORE n48_cg_redo_plan",
                 fut != std::string::npos && h.find("n48_cg_redo_plan(&in, &why)") > fut, 1u);
    }
    // build 0.0.550 (switch 108): 37 is still READ through navi48_mm_prio_switch(0u); the plan is told it through n48_cgw_mm37
    expect_u("SRC gfxsrc_cg_redo reads 37 through navi48_mm_prio_switch(0u) (a read) and the owner through current_thread()",
             h.find("const uint32_t mm37 = navi48_mm_prio_switch(0u) ? 1u : 0u;") != std::string::npos &&
             h.find("in.mm37 = n48_cgw_mm37(mm37, mode108);") != std::string::npos &&
             h.find("gCgRedoWhy.owner == (uintptr_t)current_thread()") != std::string::npos, 1u);
    const std::string u = fn_body(s, "static void cg_redo_undo_cb(void *ud)");
    expect_u("SRC the undo: n48_cg_redo_undo over gXdNew/gXdIb with gUnitPool and &gUnitState (the spill tier), the list and carry, both memos",
             u.find("n48_cg_redo_undo(gXdNew, gXdIb, c->from, c->to, c->build ? 1 : 0, c->isUnit, &gUnitPool, &gUnitState,") != std::string::npos &&
             u.find("&gCgRedoPre, &gXdFrameLocal, &gPolicyUdCarry);") != std::string::npos &&
             u.find("n48_pm_clear(&gPgmMemo);") != std::string::npos && u.find("n48_pm_clear(&gPgmMemoShadow);") != std::string::npos, 1u);
    const std::string x = fn_body(s, "static uint32_t cg_redo_xlat_cb(void *ud)");
    expect_u("SRC the re-translate: the unit set up again, ds re-zeroed, the same translate call, no retry and no deferral",
             x.find("gfxsrc_unit_setup(c->ex, c->k, c->from, c->dp, c->build)") != std::string::npos &&
             x.find("*c->ds = xlat12_draw_stats {};") != std::string::npos &&
             x.find("xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), c->ex, &gXdIb[c->from], c->to - c->from, c->out, c->olen,") != std::string::npos &&
             x.find("gfxsrc_unit_retry") == std::string::npos && x.find("gfxsrc_defer_take") == std::string::npos, 1u);
    // reviewer C2: the spill flush runs only in gfxsrc_commit_try, and the policy (where the redo lives) runs before it
    const std::string dec = fn_body(s, "static uint32_t gfxsrc_decide_frame(const uint8_t *info, uint32_t shapeOk, uint32_t n, const WsFrame *wf) {");
    const std::string ct = fn_body(s, "static uint32_t gfxsrc_commit_try(const GfxcVm &vm, const uint8_t *info, uint64_t va, uint32_t n, uint32_t nib,");
    const size_t dPol = dec.find("gfxsrc_policy(vm, &f, f.ib[0].got, ib0Va, dp, dkey, arm, boundCtx, wsBound);");
    const size_t dCt = dec.find("? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;");
    expect_u("SRC C2: the spill flush has exactly one call, inside gfxsrc_commit_try",
             count(s, "gfxsrc_spill_flush(arm, verdict);") * 16u + (ct.find("gfxsrc_spill_flush(arm, verdict);") != std::string::npos), 17u);
    expect_u("SRC C2: in gfxsrc_decide_frame the policy pass (the redo's home) is called once and BEFORE gfxsrc_commit_try (the flush)",
             count(s, "gfxsrc_policy(vm, &f,") * 16u + (dPol != std::string::npos && dCt > dPol), 17u);
    expect_u("SRC C2: gfxsrc_policy never calls the flush or commit_try", pol.find("gfxsrc_spill_flush") == std::string::npos && pol.find("gfxsrc_commit_try(") == std::string::npos, 1u);
    // the kext side of the recorder
    const std::string sb = fn_body(nb, "void navi48_cg_seg_begin(void) {");
    expect_u("SRC navi48_cg_seg_begin: reset, the pass mark, then cur_since = the pass mark",
             sb.find("n48_cg_pagerec_reset(&gCgSegRec);") < sb.find("gCgSegSince = n48_cg_ring_mark(&gCgRing);") &&
             sb.find("gCgSegSince = n48_cg_ring_mark(&gCgRing);") < sb.find("gCgSegRec.cur_since = gCgSegSince;") &&
             sb.find("gCgSegRec.cur_since = gCgSegSince;") != std::string::npos, 1u);
    const std::string pk = fn_body(nb, "uint32_t navi48_cg_seg_peek(n48_cg_why *why) {");
    expect_u("SRC navi48_cg_seg_peek is UNCOUNTED (no gCgStats) and the same check", pk.find("gCgStats") == std::string::npos &&
             pk.find("n48_cg_check_ex(&gCgSegRec, &gCgPoison, gCgSlots, N48_CG_SLOTS, &gCgRing, gCgSegSince, untracked, why);") != std::string::npos, 1u);
    const std::string sc = fn_body(nb, "uint32_t navi48_cg_seg_check(void) {");
    expect_u("SRC navi48_cg_seg_check still counts checked/refused and calls the same check",
             sc.find("n48_cg_check_ex(&gCgSegRec, &gCgPoison, gCgSlots, N48_CG_SLOTS, &gCgRing, gCgSegSince, untracked,") != std::string::npos &&
             sc.find("__atomic_fetch_add(&gCgStats.checked, 1ull, __ATOMIC_RELAXED);") != std::string::npos, 1u);
    expect_u("SRC every open/close ring push carries current_thread() (4 pushes, no plain push left in open/close)",
             // build 0.0.529: the tracked BEGIN/END are n48_cg_ring_push_od (+ switch 84's instrument range), still with the owner
             count(nb, "(uintptr_t)current_thread());   // 0.0.523: + owner") * 16u + count(nb, "n48_cg_ring_push(&gCgRing") +
             count(nb, "n48_cg_ring_push_od(&gCgRing, N48_CG_EV_BEGIN, (uint32_t)slot, lo, hi, (uintptr_t)current_thread(), dlo, dhi);") * 16u +
             count(nb, "n48_cg_ring_push_od(&gCgRing, N48_CG_EV_END, (uint32_t)slot, lo, hi, (uintptr_t)current_thread(),") * 16u, 64u);
    expect_u("SRC switch 78: OFF at boot, guarded, latched per pass (gCgRedoF.on read in the loop, never gCgRedoOn)",
             count(s, "static volatile uint32_t gCgRedoOn { 0u };") + count(s, "n48_cm_cont_switch_refused(78u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") +
             (pol.find("gCgRedoOn") == std::string::npos ? 1u : 0u), 3u);
}

// ---- reviewer C3: widths --------------------------------------------------------------------------------------------------
static size_t width(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static size_t width(const char *fmt, ...)
{
    static char b[4096]; va_list ap; va_start(ap, fmt); const int n = std::vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    return n < 0 ? 9999u : (size_t)n;
}
static void widths()
{
    const unsigned long long M = ~0ull; const unsigned U = 0xFFFFFFFFu;
    const char *verb = " - `gfxneuter 78` REFUSED: a continuous arm stands, unchanged";
    const size_t a = width(N48_CG_REDO_FMT, "OFF (default)", M, M, M, M, M, M, M, M, M, M, M, M, M);
    const size_t a2 = width(N48_CG_REDO2_FMT, verb, M, M, M, M, M, M, M);
    expect_u("C3 cguard-redo's second line <= 511 bytes at maximal fields", a2 <= 511u, 1u);
    const size_t b = width(N48_CG_REF_FMT, M, U, U, "RECORDER_OVERFLOW", M, U, U, "earlier-seg", M, U, U, M, M, U, M, M, M, M,
                           "retried-single", M);
    const size_t c = width(N48_CG_D4F_FMT, M, U, U, U, U, U, "R3-neutered-write-destination", M, "RECORDER_OVERFLOW", U, U, U, U);
    const size_t d = width(N48_CG_WOULD_SEG_FMT, M, M, "RECORDER_OVERFLOW", U, U, U, U);
    const size_t e = width(N48_RP_MID_FMT, M, U, M, M, M, M, M, M, M, M, M, M, M, M);
    std::printf("      widths: cguard-redo %zu cguard-ref %zu d4f %zu would-seg %zu resprov-mid %zu\n", a, b, c, d, e);
    expect_u("C3 cguard-redo (the bare-78 line) <= 511 bytes at maximal fields", a <= 511u, 1u);
    expect_u("C3 cguard-ref (the per-refusal line) <= 511 bytes at maximal fields", b <= 511u, 1u);
    expect_u("C3 d4f with the segment-refused attribution <= 511 bytes at maximal fields", c <= 511u, 1u);
    expect_u("C3 the WOULD dep-label companion <= 511 bytes at maximal fields", d <= 511u, 1u);
    expect_u("C3 resprov-mid <= 511 bytes at maximal fields", e <= 511u, 1u);
}

// ---- build 0.0.550 ( PLAN step 1; apple/gfx_cgw108.h): SWITCH 108 "cgwait" -----------------------------------
//   W1 OFF identity: redo_on == 78, mm37 == 37, never a release; the plan over every input equals 0.0.549's
//   W2 ON: an IN_FLIGHT refusal under 37 plans WAIT (not "37-on") and releases the priority; EVENT stays REDO (no release)
//   W3 the bound: however many segments wait in one frame, the frame's total wait never passes N48_CG_REDO_WAIT_US (2000 us)
//   W4 the re-run happens ONCE: a redone segment plans SEG_DONE; the frame cap holds; self-owned stays declined
//   W5 source pins (the release wraps ONLY the wait, the budget is 78's, the latch, the boot value, the RAII pair) and the width
static uint64_t gW3Busy = 0u, gW3Polls = 0u;
static int w3_busy(void *) { gW3Polls++; return gW3Busy ? (gW3Busy--, 1) : 0; }
static void w3_delay(void *, uint32_t) {}
static void cgw108(const char *ahhPath)
{
    uint32_t bad = 0u;
    for (uint32_t m = 0u; m < 512u; m++) {
        n48_cg_redo_in in {};
        in.on = m & 1u; in.reason = (m & 2u) ? N48_CG_IN_FLIGHT : ((m & 4u) ? N48_CG_EVENT : N48_CG_TORN);
        in.pass = (m >> 3) & 1u; in.is_unit = (m >> 4) & 1u; in.seg_redone = (m >> 5) & 1u; in.frame_redos = (m >> 6) & 1u ? 4u : 0u;
        const uint32_t mm37 = (m >> 7) & 1u; in.self_owned = (m >> 8) & 1u;
        n48_cg_redo_in a = in, b = in;
        a.mm37 = mm37; b.mm37 = n48_cgw_mm37(mm37, N48_CGW_OFF);
        uint32_t wa = 0u, wb = 0u;
        const uint32_t pa = n48_cg_redo_plan(&a, &wa), pb = n48_cg_redo_plan(&b, &wb);
        if (pa != pb || wa != wb) bad++;
        if (n48_cgw_release(N48_CGW_OFF, mm37, pb)) bad++;
        if (n48_cgw_redo_on(in.on, N48_CGW_OFF) != in.on) bad++;
    }
    expect_u("W1 108 OFF: the redo latch is 78, 37 is told as it is, never a release; the plan equals 0.0.549's (512 inputs)", bad, 0u);
    n48_cg_redo_in in {};
    in.on = n48_cgw_redo_on(0u, N48_CGW_ON); in.reason = N48_CG_IN_FLIGHT; in.mm37 = n48_cgw_mm37(1u, N48_CGW_ON);
    uint32_t why = 99u;
    const uint32_t plan = n48_cg_redo_plan(&in, &why);
    expect_u("W2 108 ON (78 OFF): the redo is live", in.on, 1u);
    expect_u("W2 108 ON: IN_FLIGHT under 37 plans WAIT, not the 37-on decline", plan, N48_CG_RD_WAIT);
    expect_u("W2 ... and that wait releases the MM priority", n48_cgw_release(N48_CGW_ON, 1u, plan), 1u);
    expect_u("W2 ... with 37 OFF there is nothing to release (78's own wait)", n48_cgw_release(N48_CGW_ON, 0u, plan), 0u);
    in.reason = N48_CG_EVENT;
    expect_u("W2 108 ON: EVENT is redone at once, no wait and no release",
             n48_cg_redo_plan(&in, &why) == N48_CG_RD_REDO && n48_cgw_release(N48_CGW_ON, 1u, N48_CG_RD_REDO) == 0u ? 1u : 0u, 1u);
    in.reason = N48_CG_IN_FLIGHT; in.self_owned = 1u;
    expect_u("W4 108 ON: a slot this thread owns is still declined (self-owned)", n48_cg_redo_plan(&in, &why) == N48_CG_RD_NO &&
             why == N48_CG_RDW_SELF ? 1u : 0u, 1u);
    in.self_owned = 0u; in.seg_redone = 1u;
    expect_u("W4 108 ON: a segment redone once is not redone again (seg-redone)", n48_cg_redo_plan(&in, &why) == N48_CG_RD_NO &&
             why == N48_CG_RDW_SEG_DONE ? 1u : 0u, 1u);
    in.seg_redone = 0u; in.frame_redos = N48_CG_REDO_FRAME_MAX;
    expect_u("W4 108 ON: the frame cap holds", n48_cg_redo_plan(&in, &why) == N48_CG_RD_NO && why == N48_CG_RDW_FRAME_CAP ? 1u : 0u, 1u);
    // W3: a frame with 8 IN_FLIGHT segments, each against a copy that never finishes, then one that finishes after 3 polls
    {
        uint64_t total = 0u; uint32_t over = 0u, timeouts = 0u;
        for (uint32_t k = 0u; k < 8u; k++) {
            gW3Busy = ~0ull;
            uint64_t waited = 0u;
            const int clear = n48_cg_redo_wait(&w3_busy, &w3_delay, nullptr, n48_cgw_left(total), N48_CG_REDO_POLL_US, &waited);
            total += waited;
            if (total > N48_CG_REDO_WAIT_US) over++;
            if (!clear) timeouts++;
        }
        expect_u("W3 the budget is 2000 us per frame (N48_CG_REDO_WAIT_US, the bound)", N48_CG_REDO_WAIT_US, 2000u);
        expect_u("W3 eight never-clearing waits in one frame: the total is exactly the 2000 us budget, never more", total * 16u + over,
                 2000u * 16u);
        expect_u("W3 ... and every one of them timed out (refused as today)", timeouts, 8u);
        gW3Busy = 3u; uint64_t waited = 0u;
        const int clear = n48_cg_redo_wait(&w3_busy, &w3_delay, nullptr, n48_cgw_left(0u), N48_CG_REDO_POLL_US, &waited);
        expect_u("W3 a copy that finishes after 3 polls: clear, 150 us charged", (uint64_t)clear * 100000u + waited, 100000u + 150u);
        expect_u("W3 n48_cgw_left: 2000, 1950, 0, 0", n48_cgw_left(0u) == 2000u && n48_cgw_left(50u) == 1950u &&
                 n48_cgw_left(2000u) == 0u && n48_cgw_left(5000u) == 0u ? 1u : 0u, 1u);
    }
    uint32_t mode = 7u;
    expect_u("W5 the verb: M 1 ON, M 2 OFF, M 3 (SHADOW) and others refused unchanged",
             n48_cgw_set(1u, &mode) && mode == N48_CGW_ON && n48_cgw_set(2u, &mode) && mode == N48_CGW_OFF && !n48_cgw_set(3u, &mode) &&
             mode == N48_CGW_OFF && !n48_cgw_set(0xFFu, &mode) && (108u | 1u << 8) == 364u && (108u | 2u << 8) == 620u ? 1u : 0u, 1u);
    {
        const unsigned long long M = ~0ull;
        const size_t w = width(N48_CGW_FMT, "OFF (default)", "OFF", "OFF", " - `gfxneuter 108` CHANGED BY THIS VERB (counters reset)");
        const size_t w2 = width(N48_CGW_FMT2, M, M, M, M, M, 0xFFFFFFFFu, M, M, M);
        std::printf("      widths: cgwait108 %zu, cgwait108 counters %zu\n", w, w2);
        expect_u("W5 both cgwait108 lines <= 491 bytes at maximal fields", w <= 491u && w2 <= 491u, 1u);
    }
    const std::string s = slurp(ahhPath ? ahhPath : "");
    if (s.size() < 100000u) { expect_u("W5 AppleHardwareHook.cpp read", 0u, 1u); return; }
    std::string dir = ahhPath; dir = dir.substr(0, dir.rfind('/') + 1u);
    const std::string ttl = slurp((dir + "Navi48Ttl.hpp").c_str());
    const std::string h = fn_body(s, "static __attribute__((noinline)) uint32_t gfxsrc_cg_redo(");
    const size_t np = std::string::npos;
    const size_t r1 = h.find("const uint32_t rel108 = n48_cgw_release(mode108, mm37, plan);");
    const size_t r2 = h.find("const uint64_t left = n48_cgw_left(gCgRedoF.waitUs);");
    const size_t r3 = h.find("if (rel108) {\n            // build 0.0.550 (switch 108)");
    const size_t r4 = h.find("Navi48MmPrioRelease rel;");
    const size_t r5 = r4 != np ? h.find("clear = n48_cgw_wait(&cg_redo_busy_cb, &cg_redo_delay_cb, &cg_redo_now_cb, nullptr, left, N48_CG_REDO_POLL_US,", r4) : np;
    const size_t r6 = r5 != np ? h.find("} else {", r5) : np;
    const size_t r7 = h.find("gCgRedoF.waitUs += waited;");
    const size_t r8 = h.find("n48_cg_redo_seq(navi48_cg_active_recorder()");
    expect_u("W5 PIN the release is scoped to the WAIT alone: rel108, the 78 budget, the RAII release, the wait, its block closes, the "
             "budget is charged, THEN the re-translate", r1 != np && r2 != np && r3 != np && r4 != np && r5 != np && r6 != np && r7 != np &&
             r8 != np && r1 < r2 && r2 < r3 && r3 < r4 && r4 < r5 && r5 < r6 && r6 < r7 && r7 < r8 &&
             count(s, "Navi48MmPrioRelease rel;") == 1u ? 1u : 0u, 1u);
    const std::string bg = fn_body(s, "static __attribute__((noinline)) void gfxsrc_cg_redo_begin(void)");
    expect_u("W5 PIN the per-pass latch: 108 read once, 78's `on` = n48_cgw_redo_on(78, 108)",
             bg.find("gCgRedoF.b108 = __atomic_load_n(&gCgw108Mode, __ATOMIC_RELAXED) == N48_CGW_ON ? 1u : 0u;") != np &&
             bg.find("gCgRedoF.on = n48_cgw_redo_on(gCgRedoOn ? 1u : 0u, gCgRedoF.b108 ? N48_CGW_ON : N48_CGW_OFF);") != np &&
             h.find("gCgw108Mode") == np && fn_body(s, "static void gfxsrc_policy(").find("gCgw108Mode") == np ? 1u : 0u, 1u);
    expect_u("W5 PIN the switch: OFF at boot, the verb its only writer, the mid-arm guards, SWITCH-GUARD:108",
             count(s, "static volatile uint32_t gCgw108Mode { N48_CGW_OFF };") == 1u && count(s, "__atomic_store_n(&gCgw108Mode") == 1u &&
             count(s, "n48_cm_cont_switch_refused(108u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             count(s, "const bool armRefused108 = m != 0u && hw_cm_armed() != 0u;") == 1u ? 1u : 0u, 1u);
    expect_u("W5 PIN Navi48MmPrioRelease: the ctor EXITS the priority scope, the dtor RE-ENTERS it",
             ttl.find("    Navi48MmPrioRelease()  { navi48_mm_prio_exit(); }\n    ~Navi48MmPrioRelease() { navi48_mm_prio_enter(); }") != np
             ? 1u : 0u, 1u);
}


// ---- build 0.0.551 (the 0.0.550 review's SHOULD 5; apple/gfx_cgw108.h n48_cgw_wait): THE WALL-CLOCK CAP --------------------
//   W6 `now` null is exactly n48_cg_redo_wait; an oversleeping delay ends the wait NOT clear at the 2000 us cap and charges the wall
//      time; a copy that clears inside the cap is still clear; the kext passes the mach_absolute_time clock and the cap at both waits
static uint64_t gW6Now = 0u, gW6Step = 0u;
static uint64_t w6_now(void *) { return gW6Now; }
static void w6_delay(void *, uint32_t us) { gW6Now += gW6Step ? gW6Step : us; }
static void cgw551(const char *ahhPath)
{
    uint32_t diff = 0u;
    for (uint64_t busyN = 0u; busyN < 50u; busyN += 3u)
        for (uint64_t left = 0u; left <= 2500u; left += 250u) {
            uint64_t wa = 0u, wb = 0u;
            gW3Busy = busyN; const int a = n48_cg_redo_wait(&w3_busy, &w3_delay, nullptr, left, N48_CG_REDO_POLL_US, &wa);
            gW3Busy = busyN; const int b = n48_cgw_wait(&w3_busy, &w3_delay, nullptr, nullptr, left, N48_CG_REDO_POLL_US, N48_CGW_WALL_CAP_US, &wb);
            if (a != b || wa != wb) diff++;
        }
    expect_u("W6 no clock: n48_cgw_wait is exactly n48_cg_redo_wait (busy x left grid)", diff, 0u);
    expect_u("W6 the wall cap is 2000 us", N48_CGW_WALL_CAP_US, 2000u);
    // each 50 us delay really sleeps 700 us: the step count says 150 us after 3 polls, the wall clock 2100 us
    gW6Now = 1000000u; gW6Step = 700u; gW3Busy = ~0ull;
    uint64_t waited = 0u;
    int clear = n48_cgw_wait(&w3_busy, &w6_delay, &w6_now, nullptr, N48_CG_REDO_WAIT_US, N48_CG_REDO_POLL_US, N48_CGW_WALL_CAP_US, &waited);
    expect_u("W6 an oversleeping delay: NOT clear, ended by the wall cap (3 sleeps, 2100 us), the wall time charged", (uint64_t)clear * 100000u + waited,
             2100u);
    const int oldClear = (gW3Busy = ~0ull, n48_cg_redo_wait(&w3_busy, &w6_delay, nullptr, N48_CG_REDO_WAIT_US, N48_CG_REDO_POLL_US, &waited));
    expect_u("W6 ... where the step count alone (0.0.550) slept 40 times (28 ms of wall time)", (uint64_t)oldClear * 100000u + waited, 2000u);
    gW6Now = 5u; gW6Step = 0u; gW3Busy = 3u;
    clear = n48_cgw_wait(&w3_busy, &w6_delay, &w6_now, nullptr, N48_CG_REDO_WAIT_US, N48_CG_REDO_POLL_US, N48_CGW_WALL_CAP_US, &waited);
    expect_u("W6 a copy that clears after 3 honest polls: clear, 150 us", (uint64_t)clear * 100000u + waited, 100000u + 150u);
    gW6Now = 5u; gW6Step = 0u; gW3Busy = ~0ull;
    clear = n48_cgw_wait(&w3_busy, &w6_delay, &w6_now, nullptr, N48_CG_REDO_WAIT_US, N48_CG_REDO_POLL_US, N48_CGW_WALL_CAP_US, &waited);
    expect_u("W6 an honest clock and a copy that never finishes: the frame budget (2000 us) still ends it", (uint64_t)clear * 100000u + waited, 2000u);
    const std::string s = slurp(ahhPath ? ahhPath : "");
    if (s.size() < 100000u) { expect_u("W6 AppleHardwareHook.cpp read", 0u, 1u); return; }
    const std::string h = fn_body(s, "static __attribute__((noinline)) uint32_t gfxsrc_cg_redo(");
    const std::string nowcb = fn_body(s, "static uint64_t cg_redo_now_cb(void *)");
    expect_u("W6 PIN both waits in gfxsrc_cg_redo pass the clock and the cap; the old uncapped wait is gone from it",
             count(h, "n48_cgw_wait(&cg_redo_busy_cb, &cg_redo_delay_cb, &cg_redo_now_cb, nullptr, left, N48_CG_REDO_POLL_US,\n"
                      "                                 N48_CGW_WALL_CAP_US, &waited);") == 2u &&
             count(h, "n48_cg_redo_wait(") == 0u ? 1u : 0u, 1u);
    expect_u("W6 PIN the clock is mach_absolute_time in microseconds",
             nowcb.find("absolutetime_to_nanoseconds(mach_absolute_time(), &ns);") != std::string::npos &&
             nowcb.find("return ns / 1000ull;") != std::string::npos ? 1u : 0u, 1u);
}

int main(int argc, char **argv)
{
    c1(); c2_c3_c4_c6(); c5(); undo();
    if (argc > 2) source_order(argv[1], argv[2]); else { gRun++; gFail++; std::printf("FAIL  pass AppleHardwareHook.cpp and Navi48Bringup.cpp\n"); }
    widths();
    cgw108(argc > 1 ? argv[1] : nullptr);   // build 0.0.550 (switch 108)
    cgw551(argc > 1 ? argv[1] : nullptr);   // build 0.0.551 (SHOULD 5: the wall-clock cap)
    std::printf("gfx_cgredo: %d checks, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
