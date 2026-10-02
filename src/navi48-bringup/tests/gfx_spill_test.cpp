// gfx_spill_test.cpp — build 0.0.522: the kext spill tier's slice table (apple/gfx_spill.h), the
// two gfx_mib.h helpers it changed (n48_mib_unit_setup's journal reset, n48_mib_unit_undo2, n48_mib_retry_single's sentinel check
// over the spill journal), and the kext's glue in AppleHardwareHook.cpp (source pins: order and wiring).
//
// build: clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//          -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ src/navi48-bringup/tests/gfx_spill_test.cpp \
//          src/xlat12/xlat12_ib.c src/xlat12/xlat12.c -o /tmp/spill
// run:   /tmp/spill src/navi48-bringup/src/apple/AppleHardwareHook.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "xlat12.h"
#include "xlat12_ib.h"
#include "gfx_mib.h"
#include "gfx_spill.h"

static int gFail = 0, gPass = 0;
static void expect(const char *what, bool ok)
{
    if (ok) gPass++; else { gFail++; std::printf("FAIL %s\n", what); }
}

static const uint64_t kRowVa = 0x23F0A81000ull, kRowVram = 0x3CBA81000ull, kVaBase = 0x23F0000000ull, kCarve = 0x3CB000000ull;

static void ring_push(n48_fr_ring *r, uint32_t seq, uint32_t want)
{
    uint32_t idx = 0u;
    (void)n48_fr_push(r, seq, 1000ull + seq, want ? seq : 0u, want ? 0x1000ull + seq : 0ull, want, &idx);
}
static void ring_state(n48_fr_ring *r, uint32_t seq, uint32_t st)
{
    uint32_t idx = 0u;
    if (n48_fr_find(r, seq, &idx)) r->e[idx].state = st;
}

// A fake VRAM for the flush: `drop` makes the write of that dword index a no-op (a lost write); `rdfail` refuses reads.
static uint32_t gVram[N48_SP_SLICES * N48_SP_SLICE_DW];
static int gDrop = -1, gRdFail = 0;
static uint32_t fwr(void *, uint64_t vram, uint32_t *buf, uint32_t n)
{
    const uint64_t at = (vram - kRowVram) / 4u;
    for (uint32_t k = 0; k < n; k++) if ((int)(at + k) != gDrop) gVram[at + k] = buf[k];
    return 1u;
}
static uint32_t frd(void *, uint64_t vram, uint32_t *buf, uint32_t n)
{
    if (gRdFail) return 0u;
    const uint64_t at = (vram - kRowVram) / 4u;
    for (uint32_t k = 0; k < n; k++) buf[k] = gVram[at + k];
    return 1u;
}

static void state_machine_checks()
{
    static n48_sp_table t; static n48_fr_ring r;
    std::memset(&t, 0, sizeof t); std::memset(&r, 0, sizeof r); t.cur = N48_SP_NONE;
    char lbl[200];
    // unarmed: nothing offered even with the switch on
    expect("SM0 an unarmed table offers nothing", n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u) == N48_SP_NONE);
    expect("SM0 arm refuses a row VA that is not 256-aligned", n48_sp_arm(&t, kRowVa + 4u, kRowVram, kVaBase, kCarve) == 0u);
    expect("SM1 arm", n48_sp_arm(&t, kRowVa, kRowVram, kVaBase, kCarve) == 1u && t.armed == 1u);
    // the switch OFF (latched 0) offers nothing although armed
    expect("SM1 76 OFF: nothing offered", n48_sp_begin(&t, &r, 0u, 1u, kVaBase, kCarve, 1u) == N48_SP_NONE);
    expect("SM1 row not valid: nothing offered", n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 0u) == N48_SP_NONE);
    uint32_t s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
    expect("SM2 the first free slice (0) is taken BUILDING", s == 0u && t.st[0] == N48_SP_BUILDING && t.cur == 0u);
    // VA arithmetic: each slice is its own 2 KiB of the row, and nothing outside it
    for (uint32_t q = 0; q < N48_SP_SLICES; q++) {
        std::snprintf(lbl, sizeof lbl, "VA slice %u = row + %u * 2 KiB, VRAM likewise, inside the row", q, q);
        expect(lbl, n48_sp_slice_va(&t, q) == kRowVa + 2048ull * q && n48_sp_slice_vram(&t, q) == kRowVram + 2048ull * q &&
                    n48_sp_in_slice(&t, q, n48_sp_slice_va(&t, q), N48_SP_SLICE_DW) &&
                    !n48_sp_in_slice(&t, q, n48_sp_slice_va(&t, q) + 4u, N48_SP_SLICE_DW) &&
                    n48_sp_slice_va(&t, q) + N48_SP_SLICE_BYTES <= kRowVa + N48_SP_ROW_BYTES);
    }
    expect("VA the row is 16 KiB of 8 x 512 dw", N48_SP_ROW_BYTES == 16384u);
    // the pool the translator sees
    static xlat12_pool pl; static uint32_t sh[N48_SP_SLICE_DW];
    n48_sp_pool_init(&t, 0u, &pl, sh);
    expect("POOL one run over the whole slice at its VA", pl.nrun == 1u && pl.run[0].host == sh && pl.run[0].va == kRowVa &&
                                                          pl.run[0].len == N48_SP_SLICE_DW && pl.jn == 0u && sh[0] == XLAT12_IB_NOP);
    expect("USED 0 before any placement", n48_sp_used(&t, 0u, &pl, sh) == 0u);
    pl.run[0].host += 40; pl.run[0].va += 160u; pl.run[0].len -= 40u;
    expect("USED 40 after a 40-dword placement", n48_sp_used(&t, 0u, &pl, sh) == 40u);
    pl.run[0].va += 4u;
    expect("USED refuses a cursor whose VA disagrees with its host", n48_sp_used(&t, 0u, &pl, sh) == N48_SP_NONE);
    pl.run[0].va -= 4u; pl.run[0].len += 1u;
    expect("USED refuses a cursor whose length disagrees", n48_sp_used(&t, 0u, &pl, sh) == N48_SP_NONE);
    pl.run[0].len -= 1u;
    expect("USED refuses the wrong slice", n48_sp_used(&t, 1u, &pl, sh) == N48_SP_NONE);
    // tag: fenced -> INFLIGHT
    ring_push(&r, 5u, 1u);
    expect("TAG seq 5 fenced -> INFLIGHT", n48_sp_tag(&t, 5u, 1u) == 0u && t.st[0] == N48_SP_INFLIGHT && t.seq[0] == 5u && t.cur == N48_SP_NONE);
    expect("TAG with no BUILDING slice does nothing", n48_sp_tag(&t, 6u, 1u) == N48_SP_NONE);
    // FREE ONLY ON RETIRED: PENDING / COMMITTED / NOT_RUN keep it
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
    expect("TAKE the next frame gets slice 1 (0 is in flight)", s == 1u && t.st[0] == N48_SP_INFLIGHT);
    const uint32_t keep[3] = { N48_FR_PENDING, N48_FR_COMMITTED, N48_FR_NOT_RUN };
    for (uint32_t k = 0; k < 3u; k++) {
        ring_state(&r, 5u, keep[k]);
        (void)n48_sp_sync(&t, &r);
        std::snprintf(lbl, sizeof lbl, "SYNC a flight in %s keeps its slice INFLIGHT", n48_fr_state_name(keep[k]));
        expect(lbl, t.st[0] == N48_SP_INFLIGHT);
    }
    ring_state(&r, 5u, N48_FR_RETIRED);
    (void)n48_sp_sync(&t, &r);
    expect("SYNC RETIRED frees it", t.st[0] == N48_SP_FREE && t.freedRetired == 1u);
    // NOPED frees; EXPIRED quarantines until disarm
    (void)n48_sp_release_unused(&t);   // slice 1 back (the frame placed nothing)
    expect("RELEASE an unused BUILDING slice goes straight back", t.st[1] == N48_SP_FREE && t.cur == N48_SP_NONE);
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&r, 7u, 1u); (void)n48_sp_tag(&t, 7u, 1u);
    const uint32_t s7 = s;
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&r, 8u, 1u); (void)n48_sp_tag(&t, 8u, 1u);
    const uint32_t s8 = s;
    ring_state(&r, 7u, N48_FR_NOPED); ring_state(&r, 8u, N48_FR_EXPIRED);
    (void)n48_sp_sync(&t, &r);
    expect("SYNC NOPED frees", t.st[s7] == N48_SP_FREE && t.freedNoped == 1u);
    expect("SYNC EXPIRED QUARANTINES (never FREE)", t.st[s8] == N48_SP_QUARANTINE && t.quarantined == 1u);
    for (uint32_t k = 0; k < 20u; k++) {
        const uint32_t g = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
        if (g == s8) { expect("QUARANTINE a quarantined slice was offered", false); break; }
        (void)n48_sp_release_unused(&t);
    }
    expect("QUARANTINE a quarantined slice is never offered while armed", t.st[s8] == N48_SP_QUARANTINE);
    ring_state(&r, 8u, N48_FR_FREE);   // reclaimed: the quarantine does not depend on the ring keeping the evidence
    (void)n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
    expect("QUARANTINE survives the ring entry's reclamation", t.st[s8] == N48_SP_QUARANTINE);
    n48_sp_disarm(&t);
    expect("DISARM ends the quarantine and frees an unstamped BUILDING slice", t.st[s8] == N48_SP_FREE && t.armed == 0u &&
                                                                                t.unquarantined == 1u);
    for (uint32_t q = 0; q < N48_SP_SLICES; q++) if (t.st[q] != N48_SP_FREE) { expect("DISARM leaves every slice FREE here", false); break; }
    // hook requests: a keystone refusal of the frame's OWN seq (the only kind since fix pass M1); applied at the next pass top
    expect("ARM again, the same row keeps the table", n48_sp_arm(&t, kRowVa, kRowVram, kVaBase, kCarve) == 1u);
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&r, 9u, 1u); (void)n48_sp_tag(&t, 9u, 1u);
    const uint32_t s9 = s;
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&r, 10u, 1u); (void)n48_sp_tag(&t, 10u, 1u);
    const uint32_t s10 = s;
    expect("REQ a request for a seq no slice carries is refused", n48_sp_request(&t, 99u, N48_SP_REQ_KS_REFUSED) == 0u);
    expect("REQ an unknown kind is refused", n48_sp_request(&t, 9u, 7u) == 0u);
    expect("REQ keystone refusal of seq 9 recorded", n48_sp_request(&t, 9u, N48_SP_REQ_KS_REFUSED) == 1u);
    expect("REQ there is no NOT_RUN request kind (M1): kind 2 is refused", n48_sp_request(&t, 10u, 2u) == 0u);
    expect("REQ nothing changes before the pass top", t.st[s9] == N48_SP_INFLIGHT && t.st[s10] == N48_SP_INFLIGHT);
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
    expect("REQ applied at the pass top: the refused frame's slice freed, the other kept", t.freedKs == 1u && t.st[s9] != N48_SP_INFLIGHT &&
                                                                                          t.st[s10] == N48_SP_INFLIGHT && s != N48_SP_NONE);
    (void)n48_sp_release_unused(&t);
    // the race: the hook read INFLIGHT, the ring then EXPIRED the flight (QUARANTINE), then the hook's store landed. A request
    // found on a slice that is no longer INFLIGHT is dropped, never applied (c12)
    ring_state(&r, 10u, N48_FR_EXPIRED); (void)n48_sp_sync(&t, &r);
    expect("REQ-RACE setup: slice 10 quarantined", t.st[s10] == N48_SP_QUARANTINE);
    t.req[s10] = N48_SP_REQ_KS_REFUSED;
    (void)n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); (void)n48_sp_release_unused(&t);
    expect("REQ-RACE a request on a QUARANTINE slice does not free it", t.st[s10] == N48_SP_QUARANTINE && t.req[s10] == N48_SP_REQ_NONE);
    // fence-less: LEAKED, nothing frees it
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&r, 11u, 0u);
    expect("LEAK a fence-less frame's slice is LEAKED", n48_sp_tag(&t, 11u, 0u) == s && t.st[s] == N48_SP_LEAKED && t.leaked == 1u);
    const uint32_t s11 = s;
    ring_state(&r, 11u, N48_FR_RETIRED);
    (void)n48_sp_request(&t, 11u, N48_SP_REQ_KS_REFUSED);
    (void)n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); (void)n48_sp_release_unused(&t);
    n48_sp_disarm(&t);
    expect("LEAK never freed (sync, request, disarm)", t.st[s11] == N48_SP_LEAKED);
    // disarm never frees a slice whose frame may still run
    { (void)n48_sp_arm(&t, kRowVa, kRowVram, kVaBase, kCarve);
      const uint32_t g = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&r, 12u, 1u); (void)n48_sp_tag(&t, 12u, 1u);
      n48_sp_disarm(&t);
      expect("DISARM keeps an INFLIGHT slice INFLIGHT (its frame may still run)", g != N48_SP_NONE && t.st[g] == N48_SP_INFLIGHT);
      ring_state(&r, 12u, N48_FR_RETIRED); (void)n48_sp_sync(&t, &r);
      expect("DISARM ...and its own retirement still frees it after the disarm", t.st[g] == N48_SP_FREE); }
    // unstamped: a BUILDING slice at the next pass top is freed (the frame was never stamped)
    (void)n48_sp_arm(&t, kRowVa, kRowVram, kVaBase, kCarve);
    s = n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
    const uint64_t un0 = t.unstamped;
    (void)n48_sp_begin(&t, &r, 1u, 1u, kVaBase, kCarve, 1u);
    expect("UNSTAMPED a slice still BUILDING at the next pass top is freed and counted", t.unstamped == un0 + 1u);
    (void)n48_sp_release_unused(&t);
    // exhaustion: every slice in flight -> nothing offered (the tier offers no run: NO_ROOM as today)
    static n48_sp_table e; std::memset(&e, 0, sizeof e); e.cur = N48_SP_NONE;
    static n48_fr_ring er; std::memset(&er, 0, sizeof er);
    (void)n48_sp_arm(&e, kRowVa, kRowVram, kVaBase, kCarve);
    for (uint32_t q = 0; q < N48_SP_SLICES; q++) {
        const uint32_t g = n48_sp_begin(&e, &er, 1u, 1u, kVaBase, kCarve, 1u);
        ring_push(&er, 100u + q, 1u);
        (void)n48_sp_tag(&e, 100u + q, 1u);
        if (g != q) expect("EXHAUST slices taken in order", false);
    }
    expect("EXHAUST the ninth frame is offered nothing", n48_sp_begin(&e, &er, 1u, 1u, kVaBase, kCarve, 1u) == N48_SP_NONE && e.noFree == 1u);
    ring_state(&er, 103u, N48_FR_RETIRED);
    expect("EXHAUST one retirement makes exactly that slice available, applied by n48_sp_begin itself (no separate sync)",
           n48_sp_begin(&e, &er, 1u, 1u, kVaBase, kCarve, 1u) == 3u);
    { // ORDER (c9): the verdicts are applied BEFORE the take - an EXPIRED flight is quarantined by begin itself, never offered
      static n48_sp_table o; std::memset(&o, 0, sizeof o); o.cur = N48_SP_NONE;
      static n48_fr_ring orr; std::memset(&orr, 0, sizeof orr);
      (void)n48_sp_arm(&o, kRowVa, kRowVram, kVaBase, kCarve);
      (void)n48_sp_begin(&o, &orr, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&orr, 300u, 1u); (void)n48_sp_tag(&o, 300u, 1u);
      ring_state(&orr, 300u, N48_FR_RETIRED);
      expect("ORDER a RETIRED slice is freed by begin and taken by the SAME begin", n48_sp_begin(&o, &orr, 1u, 1u, kVaBase, kCarve, 1u) == 0u &&
                                                                                 o.freedRetired == 1u); }
    // a base move or ringmap rebuild empties the table
    expect("MOVE a moved VA base empties the table and offers nothing",
           n48_sp_begin(&e, &er, 1u, 1u, kVaBase + 0x10000000ull, kCarve, 1u) == N48_SP_NONE && e.armed == 0u && e.st[0] == N48_SP_FREE);
    (void)n48_sp_arm(&e, kRowVa, kRowVram, kVaBase, kCarve);
    (void)n48_sp_begin(&e, &er, 1u, 1u, kVaBase, kCarve, 1u);
    expect("MOVE an unbuilt ring region empties it too", n48_sp_begin(&e, &er, 1u, 0u, kVaBase, kCarve, 1u) == N48_SP_NONE && e.armed == 0u);
    n48_sp_disarm(&e);
    expect("MOVE a different row at the arm empties the table (nothing in flight)",
           n48_sp_arm(&e, kRowVa + 0x4000u, kRowVram + 0x4000u, kVaBase, kCarve) == 1u && e.rowVa == kRowVa + 0x4000u);
    { const uint32_t g = n48_sp_begin(&e, &er, 1u, 1u, kVaBase, kCarve, 1u); ring_push(&er, 400u, 1u); (void)n48_sp_tag(&e, 400u, 1u);
      n48_sp_disarm(&e);
      expect("ARM-ROW a different row is REFUSED while a slice of the old row is in flight (the table kept)",
             n48_sp_arm(&e, kRowVa + 0x8000u, kRowVram + 0x8000u, kVaBase, kCarve) == 0u && e.rowVa == kRowVa + 0x4000u &&
             g != N48_SP_NONE && e.st[g] == N48_SP_INFLIGHT);
      expect("BUSY an unarmed table with a slice in flight is busy", n48_sp_busy(&e) == 1u);
      ring_state(&er, 400u, N48_FR_RETIRED); (void)n48_sp_sync(&e, &er);
      expect("BUSY unarmed and every slice FREE is not busy", n48_sp_busy(&e) == 0u);
      (void)n48_sp_arm(&e, kRowVa + 0x4000u, kRowVram + 0x4000u, kVaBase, kCarve);
      expect("BUSY armed is busy", n48_sp_busy(&e) == 1u); }
}

static void flush_checks()
{
    static n48_sp_table t; std::memset(&t, 0, sizeof t); t.cur = N48_SP_NONE;
    (void)n48_sp_arm(&t, kRowVa, kRowVram, kVaBase, kCarve);
    static uint32_t sh[N48_SP_SLICE_DW], back[64];
    for (uint32_t k = 0; k < N48_SP_SLICE_DW; k++) sh[k] = 0xA5000000u + k;
    std::memset(gVram, 0, sizeof gVram);
    gDrop = -1; gRdFail = 0;
    expect("FLUSH 200 dw of slice 2 written and read back clean", n48_sp_flush(&t, 2u, sh, 200u, &fwr, &frd, nullptr, back) == 0u &&
                                                                   gVram[2u * 512u + 199u] == sh[199] && gVram[2u * 512u + 200u] == 0u &&
                                                                   gVram[2u * 512u - 1u] == 0u);
    gDrop = 3 * 512 + 130;
    expect("FLUSH a lost write is caught by the read-back (mismatch)", n48_sp_flush(&t, 3u, sh, 200u, &fwr, &frd, nullptr, back) == 2u);
    gDrop = -1;
    // c10: VRAM holds an OLD record where the write was lost: the read-back must compare what came BACK, not the shadow itself
    std::memset(gVram, 0, sizeof gVram);
    gDrop = 4 * 512 + 3;
    expect("FLUSH a single lost dword (VRAM keeps its old 0) is a mismatch", n48_sp_flush(&t, 4u, sh, 8u, &fwr, &frd, nullptr, back) == 2u &&
                                                                             gVram[4u * 512u + 3u] == 0u);
    gDrop = -1; gRdFail = 1;
    expect("FLUSH an unreadable window refuses", n48_sp_flush(&t, 3u, sh, 200u, &fwr, &frd, nullptr, back) == 1u);
    gRdFail = 0;
    expect("FLUSH 0 or more than a slice refuses", n48_sp_flush(&t, 3u, sh, 0u, &fwr, &frd, nullptr, back) == 3u &&
                                                    n48_sp_flush(&t, 3u, sh, N48_SP_SLICE_DW + 1u, &fwr, &frd, nullptr, back) == 3u);
    expect("FLUSH a full slice (512 dw, 8 chunks)", n48_sp_flush(&t, 7u, sh, N48_SP_SLICE_DW, &fwr, &frd, nullptr, back) == 0u &&
                                                    gVram[8u * 512u - 1u] == sh[511]);
}

// n48_mib_unit_setup / n48_mib_unit_undo2 / n48_mib_retry_single over the spill journal
static xlat12_unit gU; static xlat12_pool gPool, gSp; static uint32_t gPoolHost[64], gSpHost[64];
static uint32_t fake_xlat(const xlat12_draw_extra *ex, const uint32_t *in, uint32_t n, uint32_t *out, uint32_t *olen, xlat12_draw_stats *ds)
{
    (void)in; (void)ds;
    for (uint32_t k = 0; k < n; k++) out[k] = XLAT12_IB_NOP;
    *olen = n;
    xlat12_pool *sp = ex->unit->spill;   // "places" one record whose second dword is the sentinel (a leak from `out`)
    sp->j[0].host = sp->run[0].host; sp->j[0].va = sp->run[0].va; sp->j[0].len = sp->run[0].len; sp->j[0].used = 4u; sp->j[0].r = 0u;
    sp->jn = 1u;
    sp->run[0].host[0] = 0xC0021000u; sp->run[0].host[1] = N48_MIB_RETRY_SENTINEL; sp->run[0].host[2] = 0u; sp->run[0].host[3] = 0u;
    sp->run[0].host += 4; sp->run[0].va += 16u; sp->run[0].len -= 4u;
    return 0u;
}
static void mib_checks()
{
    for (uint32_t k = 0; k < 64u; k++) { gPoolHost[k] = XLAT12_IB_NOP; gSpHost[k] = XLAT12_IB_NOP; }
    std::memset(&gU, 0, sizeof gU); std::memset(&gPool, 0, sizeof gPool); std::memset(&gSp, 0, sizeof gSp);
    gSp.run[0].host = gSpHost; gSp.run[0].va = kRowVa; gSp.run[0].len = 64u; gSp.nrun = 1u;
    gU.spill = &gSp;
    gSp.jn = 3u; gPool.jn = 2u;
    xlat12_draw_extra ex; std::memset(&ex, 0, sizeof ex);
    xlat12_ib_segment seg; std::memset(&seg, 0, sizeof seg);
    (void)n48_mib_unit_setup(&ex, &gU, &gPool, 1, 0u, &seg, 0u, nullptr, nullptr);
    expect("SETUP empties the spill journal too (F3), even for a single", gSp.jn == 0u && gPool.jn == 0u);
    // undo2: a refused unit takes back its spill records; a translated one does not
    gSp.j[0].host = gSpHost; gSp.j[0].va = kRowVa; gSp.j[0].len = 64u; gSp.j[0].used = 10u; gSp.j[0].r = 0u; gSp.jn = 1u;
    for (uint32_t k = 0; k < 10u; k++) gSpHost[k] = 0x1234u;
    gSp.run[0].host = gSpHost + 10; gSp.run[0].va = kRowVa + 40u; gSp.run[0].len = 54u;
    expect("UNDO2 a translated unit keeps its spill records", n48_mib_unit_undo2(1u, 0u, &gPool, &gU) == 0u && gSpHost[0] == 0x1234u);
    const uint32_t back = n48_mib_unit_undo2(1u, 7u, &gPool, &gU);
    uint32_t notNop = 0; for (uint32_t k = 0; k < 64u; k++) notNop += gSpHost[k] != XLAT12_IB_NOP;
    expect("UNDO2 a refused unit's spill records are taken back and the run restored",
           back == 10u && notNop == 0u && gSp.jn == 0u && gSp.run[0].host == gSpHost && gSp.run[0].len == 64u && gSp.run[0].va == kRowVa);
    { xlat12_unit u0; std::memset(&u0, 0, sizeof u0);
      expect("UNDO2 with no spill tier is n48_mib_unit_undo", n48_mib_unit_undo2(1u, 7u, &gPool, &u0) == n48_mib_unit_undo(1u, 7u, &gPool)); }
    // the retry's sentinel proof covers the spill journal
    static uint32_t in[16], out[16]; for (uint32_t k = 0; k < 16u; k++) in[k] = XLAT12_IB_NOP;
    xlat12_draw_stats ds; uint32_t olen = 0, why = 0;
    std::memset(&ex, 0, sizeof ex);
    const uint32_t st = n48_mib_retry_single(&fake_xlat, &ex, &gU, &gPool, 1, &seg, 0u, in, 16u, out, &olen, &ds, 29u, &why);
    notNop = 0; for (uint32_t k = 0; k < 64u; k++) notNop += gSpHost[k] != XLAT12_IB_NOP;
    expect("RETRY a sentinel in a spill record refuses VERIFY (SENTINEL_LEFT, dword n) and undoes the spill",
           st == (uint32_t)XLAT12_IB_ERR_VERIFY && why == N48_MIB_RETRY_SENTINEL_LEFT && ds.err_in_dword == 16u && notNop == 0u && gSp.jn == 0u);
}

// ---- the kext's glue: order and wiring (AppleHardwareHook.cpp) ----
static std::string slurp(const char *p)
{
    FILE *f = std::fopen(p, "rb"); if (!f) return std::string();
    std::string s; char b[65536]; size_t r; while ((r = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, r); std::fclose(f); return s;
}
static size_t count(const std::string &s, const char *needle)
{
    size_t c = 0, p = 0; const size_t L = std::strlen(needle);
    while ((p = s.find(needle, p)) != std::string::npos) { c++; p += L; }
    return c;
}
static size_t at(const std::string &s, const char *needle, size_t from = 0) { return s.find(needle, from); }
static std::string body(const std::string &s, const char *head)
{
    const size_t a = s.find(head); if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a); return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static void glue_checks(const char *ahh)
{
    const std::string s = slurp(ahh);
    expect("GLUE AppleHardwareHook.cpp read", s.size() > 100000u);
    const size_t npos = std::string::npos;
    expect("GLUE switch 76 is OFF at boot", count(s, "static volatile uint32_t gSpillOn { 0u };") == 1u);
    // the pass top: after `pack`'s latch, before the first translate of the pass
    const size_t pol = at(s, "static void gfxsrc_policy(");
    const size_t sb = at(s, "    gfxsrc_spill_begin(arm, dp);", pol), pk = at(s, "    gUnitState.pack = gUnitPackOn ? 1u : 0u;", pol);
    const size_t tr = at(s, "xlat12_ib_translate_draw_ex(xlat12_ib_m2tri_profile(), &ex, &gXdIb[from]", pol);
    expect("GLUE spill_begin once, in the policy, after pack's latch and before any translate",
           count(s, "gfxsrc_spill_begin(arm, dp);") == 1u && pol != npos && pk != npos && sb != npos && tr != npos && pk < sb && sb < tr);
    const std::string beg = body(s, "static __attribute__((noinline)) void gfxsrc_spill_begin(uint32_t arm, uint32_t dp)");
    expect("GLUE spill_begin: the latched switch AND a COMMIT arm AND the descriptor path, else spill NULL",
           beg.find("(gSpillOn && arm == N48_SD_ARM_COMMIT && dp && gXdNew && gXdOut)") != npos &&
           beg.find("gUnitState.spill = &gUnitSpill;") != npos && beg.find("gUnitState.spill = nullptr;") != npos &&
           beg.find("n48_sp_begin(&gSpill, &gKsRing, on,") != npos && beg.find("n48_reloc_find(&gReloc, N48_SP_KEY") != npos);
    // the flush: in commit_try, before `live`, which is before the first IB write
    const size_t ct = at(s, "static uint32_t gfxsrc_commit_try(");
    const size_t fl = at(s, "    gfxsrc_spill_flush(arm, verdict);", ct), lv = at(s, "    const bool live = (arm == N48_SD_ARM_COMMIT)", ct);
    const size_t wr = at(s, "gfxc_write_sys(vm,", ct);
    expect("GLUE the flush runs once, in commit_try, before `live` and before the IB write",
           count(s, "gfxsrc_spill_flush(arm, verdict);") == 1u && fl != npos && lv != npos && wr != npos && fl < lv && lv < wr);
    const std::string fb = body(s, "static __attribute__((noinline)) void gfxsrc_spill_flush(uint32_t arm, uint32_t verdict)");
    expect("GLUE the flush reads back through n48_sp_flush and refuses the frame on any failure",
           fb.find("n48_sp_flush(&gSpill, s, gSpillShadow, used, &spill_mm_wr, &spill_mm_rd, nullptr, gSpillBack)") != npos &&
           count(fb, "gXdBuild.ok = 0u;") >= 4u && fb.find("gSpillFrame != gXdC.judged + 1u") != npos &&
           fb.find("n48_sp_in_slice(&gSpill, s, va, used)") != npos);
    expect("GLUE (c2) a failed flush refuses the frame: the failure branch is unconditional and clears ok",
           fb.find("    if (fw != 0u) {\n        if (fw == 1u) gSpillS.writeFail++; else if (fw == 2u) gSpillS.mismatch++; else gSpillS.outside++;\n        gSpillS.refused++; gXdBuild.ok = 0u; return;\n    }") != npos);
    const size_t fwAt = fb.find("const uint32_t fw = n48_sp_flush("), hdAt = fb.find("    if (!navi48_hdp_flush_now()) { gSpillS.writeFail++; gSpillS.refused++; gXdBuild.ok = 0u; return; }");
    const size_t rfAt = fb.find("if (!n48_reloc_find(&gReloc, N48_SP_KEY, gRingMap.vaBase, &offNow) || n48_reloc_va(gRingMap.vaBase, offNow) != gSpill.rowVa) {\n            gSpillS.rowLost++; gXdBuild.ok = 0u; return;");
    expect("GLUE (c7) a slice taken by another judged frame refuses THIS frame",
           fb.find("    if (gSpillFrame != gXdC.judged + 1u) { gSpillS.frameMismatch++; gXdBuild.ok = 0u; return; }") != npos);
    expect("GLUE (M2) the HDP flush follows the write and a failed flush refuses", fwAt != npos && hdAt != npos && fwAt < hdAt);
    expect("GLUE (SHOULD) the row is re-found at its VA immediately before the write, else rowLost and refuse", rfAt != npos && rfAt < fwAt);
    expect("GLUE (c4) a lost row is counted AND never offered",
           beg.find("        if (!rowOk) gSpillS.rowLost++;") != npos &&
           beg.find("        rowOk = (n48_reloc_find(&gReloc, N48_SP_KEY, gRingMap.vaBase, &off) &&\n                 n48_reloc_va(gRingMap.vaBase, off) == gSpill.rowVa) ? 1u : 0u;") != npos &&
           count(beg, "rowOk = 1u") == 0u && beg.find("gRingMap.base, rowOk);") != npos);
    expect("GLUE (SHOULD) disarm on the ARM's state, not the frame's level",
           beg.find("if (gSpill.armed && (gXdArm != N48_SD_ARM_COMMIT || gXdShot.state != N48_CM_SHOT_ARMED)) { n48_sp_disarm(&gSpill);") != npos);
    expect("GLUE (M3b) verb 6 and switch 31's ON refuse while the spill tier holds the arena",
           count(s, "        if (n48_sp_busy(&gSpill)) {\n            st = 5u;\n            HWLOG(\"gfx-reloc: `gfxneuter 6` REFUSED") == 1u &&
           count(s, "            if (n48_sp_busy(&gSpill)) {   // build 0.0.522 (fix pass M3b): the spill tier holds the arena\n                gXdFillColorOn = 0u; st = 5;") == 1u &&
           s.find("if (n48_sp_busy(&gSpill)) {\n            st = 5u;") < s.find("        st = gfxsrc_reloc_upload_all() ? 5u : 0u;"));
    expect("GLUE the MM callbacks are the window's own write and read",
           count(s, "return navi48_vram_write_mm(vram, buf, ndw) ? 1u : 0u;") == 1u && count(s, "return navi48_vram_read_mm(vram, buf, ndw) ? 1u : 0u;") == 1u);
    // the stamp tags the slice, right after the push, only when it was pushed
    const size_t pu = at(s, "const uint32_t frPushed = n48_fr_push(&gKsRing, gXdCmToken.seq,", ct);
    const size_t tg = at(s, "if (frPushed && gSpillFrame == gXdC.judged + 1u) (void)n48_sp_tag(&gSpill, gXdCmToken.seq, frWant ? 1u : 0u);", ct);
    expect("GLUE the tag follows the push (its seq, its fence)", pu != npos && tg != npos && pu < tg && tg - pu < 600u &&
                                                               count(s, "n48_sp_tag(") == 1u);
    // the ring's verdicts before reclamation, and in the expiry poll
    const size_t ex = at(s, "frNewlyExpired = n48_fr_expire(&gKsRing"), sy = at(s, "(void)n48_sp_sync(&gSpill, &gKsRing);", ex);
    const size_t rc = at(s, "(void)n48_fr_reclaim_hold(&gKsRing, frHold, frNHold);", ex);
    expect("GLUE sync after the expiry and BEFORE the reclamation", ex != npos && sy != npos && rc != npos && ex < sy && sy < rc);
    expect("GLUE sync after the expiry poll's retirements", count(s, "if (n) (void)n48_sp_sync(&gSpill, &gKsRing);") == 1u);
    // the hook's two requests, each with the frame's OWN seq
    const size_t fb2 = at(s, "(void)n48_fr_free_by_seq(&gKsRing, here.seq);"), rq = at(s, "(void)n48_sp_request(&gSpill, here.seq, N48_SP_REQ_KS_REFUSED);");
    expect("GLUE keystone refusal requests the free right after the ring frees the entry", fb2 != npos && rq != npos && fb2 < rq && rq - fb2 < 400u &&
                                                                                           count(s, "N48_SP_REQ_KS_REFUSED") == 1u);
    expect("GLUE (M1) no NOT_RUN request anywhere in the hook: only a proven NOP (NOPED, via sync) or a keystone refusal frees",
           count(s, "N48_SP_REQ_NOT_RUN") == 0u && count(s, "n48_sp_request(") == 1u);
    expect("GLUE no request on the token-mismatch NOT_RUN (shared seq)", count(s, "n48_sp_request(&gSpill, gXdCmToken.seq") == 0u);
    // undo: both kext sites take back the spill tier too; the old one-tier call is gone
    expect("GLUE both undo sites use n48_mib_unit_undo2", count(s, "n48_mib_unit_undo2(isUnit, st, &gUnitPool, &gUnitState)") == 2u &&
                                                           count(s, "n48_mib_unit_undo(isUnit, st, &gUnitPool)") == 0u);
    // the arm reserves; the verb is guarded
    const size_t am = at(s, "                gXdArm = N48_SD_ARM_COMMIT;\n                (void)gfxsrc_spill_arm();");
    expect("GLUE the COMMIT arm reserves the row", am != npos && count(s, "gfxsrc_spill_arm();") == 1u);
    const std::string ab = body(s, "static __attribute__((noinline)) uint32_t gfxsrc_spill_arm(void)");
    expect("GLUE the reservation is gfx_reloc.h's allocator, key N48_SP_KEY, the whole row",
           ab.find("n48_reloc_place(&gReloc, gRingMap.vaBase, 1u, gRingMap.base, (gReloc.used ? gReloc.epoch : navi48_shadercache_epoch()), N48_SP_KEY,") != npos &&
           ab.find("N48_SP_ROW_BYTES, 0u, &off, &fresh)") != npos && ab.find("if (!gSpillOn) return 0u;") != npos);
    expect("GLUE the row's VA and VRAM offset are the placement's own (arena base + off, the same offset in both spaces)",
           ab.find("const uint64_t rowVa = n48_reloc_va(gRingMap.vaBase, off);") != npos &&
           ab.find("const uint64_t rowVram = gRingMap.base + (uint64_t)XLAT12_RELOC_ARENA_OFF + (uint64_t)off;") != npos &&
           ab.find("armed = n48_sp_arm(&gSpill, rowVa, rowVram, gRingMap.vaBase, gRingMap.base);") != npos);
    expect("GLUE the flush checks the slice against the arena and the VA/VRAM correspondence before any write",
           fb.find("va - gRingMap.vaBase != vram - gRingMap.base") != npos && fb.find("vram + 4ull * used > arenaLo + (uint64_t)XLAT12_RELOC_ARENA_BYTES") != npos);
    expect("GLUE switch 76's verb is guarded", count(s, "n48_cm_cont_switch_refused(76u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u);
}

int main(int argc, char **argv)
{
    state_machine_checks();
    flush_checks();
    mib_checks();
    if (argc > 1) glue_checks(argv[1]); else expect("GLUE needs AppleHardwareHook.cpp as argv[1]", false);
    std::printf("gfx_spill: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
