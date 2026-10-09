// gfx_fence828_test.cpp — the offline proof for the one-dword un-NOP and (0.0.413) the OWNED SLOT.
// (; an internal design note,).
//
// The properties under test are the specification of the change:
//
//     1. IT READS BEFORE IT WRITES. Every dword the rewrite depends on is matched against what was read, at the
//        instant of the write, and a single changed dword refuses the whole thing without writing ANY dword. 0.0.413:
//        the two ADDRESS words are re-proven as their own step, so "an address word moved under us" has its own reason
//        (N48_F828_ADDR_UNPROVEN) and a break that skips one of them is caught (M13).
//     2. IT ONLY EVER STARTS FROM THE MEASURED DEAD PAGE. Apple's packet is still required to point at the dead page
//        (that is how we recognise it); the LIVE page is refused by name.
//     3. IT REFUSES, NEVER FORCES. A segment with no trailing NOP, or with more than one buried fence, or whose
//        packet differs from Apple's in ANY field, gets a named refusal and an untouched stream.
//     4. IT DOES NOT CHANGE THE LENGTH OR THE SHAPE. After the rewrite the stream still walks end to end, still
//        ends exactly at n, holds exactly one more packet, and the CP lands on the SAME next dword. FOUR dwords change:
//        the NOP header, the two ADDRESS words (re-pointed at OUR owned slot), and DATA_LO.
//     5. IT RAISES NO INTERRUPT. A packet whose INT_SEL is not 0 is refused rather than enabled.
//     6. THE VERDICT IS A LATCH, NOT A SAMPLE (0.0.378). The question is existential - "did the slot
//        EVER hold a value only we write?" - so a match on ANY poll is a proof, a later non-match is a REVERSION
//        that is recorded beside it and not instead of it, and an unreadable poll decides nothing in either
//        direction. The seven states are a ladder: no two can be true at once.
//     7. THE VALUE IS epoch|ordinal (0.0.413, B3). A value from ANOTHER boot, ANOTHER ordinal, or equal to the value
//        read BEFORE the commit never counts as end-of-pipe (B6).
//     8. THE CANDIDATE AND THE COMMITTED FRAME ARE SEPARATE RECORDS, AND THE WATCH RESET IS AT THE COMMIT
//        PROMOTION (0.0.413 B5; 0.0.415 Q4). The translate-time candidate site writes only the candidate record;
//        a candidate the gate or the keystone refuses never touches the committed record or its latch, so a
//        committed frame still in flight keeps its watch (M18/M19,).
//
// The good input is REAL: two of arm6's own compositor segments as the translator produced them on the armed path
// (fixture_fence828.h). Everything perturbed below is perturbed from those bytes, not from an invention.
//
// PLANTED-DEFECT CONTROL (rule: a test no mutation can break is not testing anything). TWENTY-ONE mutants, each a
// plausible way this could have been written, and each must be CAUGHT by at least one named check. M1 IS the defect
// class this change introduces — "a NOP header is overwritten without verifying the packet beneath it" — and M2 is
// the sketch taken literally (index dword 1017). M13-M17 are 0.0.413's: an address word not re-proven, Apple's
// address not re-pointed, DST_SEL not checked, the epoch dropped from the encoder, and a reset on the candidate.
// M18/M19 are 0.0.415's (Q4): the candidate site clearing/overwriting the committed record, and a promotion that runs
// at the gate before the keystone proves the frame. Both are exercised through the REAL candidate/promotion path
//, so the Q4 defect fails loudly instead of hiding behind a hand-set watch. M20/M21 are 0.0.418's: the E2 rule
// (`ks_eop_seen` = committed && ever, the flight ignored) and the E3 rule (REGION-MOVED drops the candidate but the
// frame still runs, pinning the next candidate at SLOT-PRE). E2 is driven through the same real flow in.
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple src/navi48-bringup/tests/gfx_fence828_test.cpp -o /tmp/f828 && /tmp/f828
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "gfx_fence828.h"
#include "fixture_fence828.h"
#include "fixture_fence71_run10p.h"   // build 0.0.508 (switch 71): real run10p final slices, section 16

static int gFail = 0, gRun = 0, gQuiet = 0;

static void ck(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) {
        gFail++;
        if (!gQuiet) printf("  FAIL %-62s got %#llx want %#llx\n", what, (unsigned long long)got,
                            (unsigned long long)want);
    } else if (!gQuiet) printf("  ok   %-62s %#llx\n", what, (unsigned long long)got);
}

// ---------------------------------------------------------------------------------------------------------------------
// 0.0.413 (B3): the owned fence page as the kext builds it. A fake VA in the same shape (the real one is
// gRingMap.vaBase + XLAT12_FENCE_PAGE_OFF); the arithmetic is tied to xlat12_ib.h in the kext by a static_assert.
// ---------------------------------------------------------------------------------------------------------------------
static const uint64_t kFencePageVa = 0x23F0000000ull + N48_F828_FENCE_PAGE_OFF;   // arm21's ring VA base + 0xA8F000
static const uint32_t kEpoch       = 0x1234u;
static const uint32_t kOrdinal     = 7u;
static const uint32_t kWant        = ((kEpoch & 0xFFFFu) << 16) | (kOrdinal & 0xFFFFu);
static const uint64_t kSlotVa      = kFencePageVa + (uint64_t)kOrdinal * N48_F828_SLOT_BYTES;

// ---------------------------------------------------------------------------------------------------------------------
// THE MUTANTS. Each replaces find/apply/value/reset with a WRONG-BUT-PLAUSIBLE version; gMut selects one, 0 = real.
// ---------------------------------------------------------------------------------------------------------------------
enum {
    MUT_NONE = 0,
    MUT_NO_VERIFY,        // M1: apply() writes without re-matching the dwords find() read  <- THE NEW DEFECT CLASS
    MUT_FIXED_1017,       // M2: find() indexes dword 1017 instead of searching (the sketch, taken literally)
    MUT_ANY_PAGE,         // M3: the destination page is not checked at all
    MUT_LIVE_OK,          // M4: the dead-page check is a "not obviously bogus" check that lets the LIVE page through
    MUT_NO_INT_SEL,       // M5: INT_SEL is not checked, so a fence that interrupts is enabled
    MUT_NO_DATA_SEL,      // M6: DATA_SEL is not checked, so our value need not be what lands in memory
    MUT_ANY_NOP_LEN,      // M7: any NOP >= 9 dwords is accepted, leaving orphan dwords after the un-NOPed packet
    MUT_FIRST_NOT_ONLY,   // M8: the FIRST buried fence is taken instead of refusing when there are several
    MUT_WALK_SKIP,        // M9: the whole-stream walk is skipped; the search scans dwords rather than packets
    MUT_LAST_READ_ONLY,   // M10: the VERDICT is computed from the LAST poll  <- THE SHIPPED 0.0.377 DEFECT
    MUT_LATCH_NOT_STICKY, // M11: `ever` is recomputed each poll instead of latched
    MUT_UNREADABLE_WINS,  // M12: readability is tested BEFORE `ever`, so an unreadable tail buries a proof
    MUT_ADDR_SKIP,        // M13 (0.0.413): one ADDRESS dword is not re-proven at write time
    MUT_NO_ADDR_REWRITE,  // M14 (0.0.413): the packet still names Apple's dead page (0.0.412's behaviour)
    MUT_NO_DST_SEL,       // M15 (0.0.413): DST_SEL is not checked
    MUT_EPOCH_DROP,       // M16 (0.0.413): the encoder drops the epoch, so last boot's value matches this boot's
    MUT_RESET_ON_CAND,    // M17 (0.0.413): the watch is reset on a REFUSED candidate (B5's defect)
    MUT_CAND_CLEARS_COMMITTED, // M18 (0.0.415, Q4): the CANDIDATE site overwrites and clears the committed record
    MUT_PROMOTE_AT_GATE,  // M19 (0.0.415, Q4): the promotion (copy + reset) runs at the gate, before the keystone's proof
    MUT_EOP_IGNORES_FLIGHT, // M20 (0.0.418, E2): ks_eop_seen is `committed && ever`, the flight ignored (0.0.417's rule)
    MUT_REGION_MOVED_RUNS,  // M21 (0.0.418, E3): REGION-MOVED drops the candidate but lets the frame run (0.0.417's rule)
    MUT_PROMOTE_STORE_FLIGHT_FIRST, // M22 (0.0.420, follow-up): the copy/flight stamp precede the watch reset,
                                    // leaving a window where a reader answers end-of-pipe for a flight never promoted
    MUT_HANDOFF_STALE_PENDING,     // M23 (0.0.444, an internal review note (B) item 1): the ring push reads gXdF828Pending
                                    // AFTER the gate block has already cleared it (0.0.443's actual defect)
    MUT_COUNT
};
static const char *gMutName[MUT_COUNT] = {
    "(none)",
    "M1 apply() writes without re-matching what find() read",
    "M2 find() indexes dword 1017 instead of searching",
    "M3 the destination page is not checked at all",
    "M4 the check lets Apple's LIVE fence page through",
    "M5 INT_SEL is not checked (a fence that interrupts is enabled)",
    "M6 DATA_SEL is not checked (our value need not reach memory)",
    "M7 any NOP >= 9 dw is accepted (orphan dwords after the packet)",
    "M8 the FIRST buried fence is taken, several are not refused",
    "M9 the packet walk is skipped; the search scans raw dwords",
    "M10 the VERDICT is the LAST read, not 'did it EVER match'",
    "M11 the `ever` latch is not sticky (a later poll clears it)",
    "M12 an unreadable poll overrules a proof already obtained",
    "M13 one ADDRESS dword is not re-proven at write time",
    "M14 Apple's address is not re-pointed at our owned slot",
    "M15 DST_SEL is not checked (0.0.412's missing clause)",
    "M16 the encoder drops the epoch (last boot's value matches)",
    "M17 the watch is reset on a REFUSED candidate (B5)",
    "M18 the CANDIDATE site clears/overwrites the committed record (Q4)",
    "M19 the promotion runs at the gate, before the keystone proves the frame (Q4)",
    "M20 (E2) ks_eop_seen is committed && ever - the flight is ignored (0.0.417)",
    "M21 (E3) REGION-MOVED drops the candidate but the frame still runs (0.0.417)",
    "M22 the copy/flight stamp precede the watch reset (a promotion window that answers EOP)",
    "M23 (C5-RING-REVIEW (B) item 1) the ring push reads gXdF828Pending AFTER the gate block cleared it",
};
static int gMut = MUT_NONE;
// 0.0.420: set when a reader INSIDE the promotion (the mutant's injected read) answers
// end-of-pipe for the new flight on the OLD, unreset latch. The real order resets first, so it can never set this.
static uint32_t gPromoteWindowFalseEop = 0u;

static uint32_t mfind(const uint32_t *out, uint32_t n, n48_f828 *r)
{
    // Only the FIND mutants take this branch; apply/value/reset-only mutants use the real finder.
    switch (gMut) {
    case MUT_FIXED_1017: case MUT_ANY_PAGE: case MUT_LIVE_OK: case MUT_NO_INT_SEL:
    case MUT_NO_DATA_SEL: case MUT_ANY_NOP_LEN: case MUT_FIRST_NOT_ONLY: case MUT_WALK_SKIP:
    case MUT_NO_DST_SEL:
        break;
    default:
        return n48_f828_find(out, n, r);
    }

    n48_f828 z {};
    if (!r) return N48_F828_ARG;
    *r = z;
    if (!out || n < 10u) { r->why = N48_F828_ARG; return r->why; }

    uint32_t found = 0u, at = 0u;
    if (gMut == MUT_FIXED_1017) {
        if (n <= 1026u) { r->why = N48_F828_NO_NOP; return r->why; }
        at = 1017u; found = 1u;                                   // no search, no header check at all
    } else if (gMut == MUT_WALK_SKIP) {
        for (uint32_t i = 0u; i + 9u < n; i++)
            if (out[i] == N48_F828_NOP9 && out[i + 1u] == N48_F828_RELMEM) { found++; at = i; break; }
        if (!found) { r->why = N48_F828_NO_NOP; return r->why; }
    } else {
        for (uint32_t i = 0u; i < n; ) {
            const uint32_t len = n48_f828_pkt_len(out[i]);
            if (!len || i + len > n) { r->why = N48_F828_WALK; return r->why; }
            const bool hdrOk = (gMut == MUT_ANY_NOP_LEN)
                             ? ((out[i] >> 30) == 3u && ((out[i] >> 8) & 0xFFu) == 0x10u && len >= 9u &&
                                out[i] != N48_F828_NOP1)
                             : (out[i] == N48_F828_NOP9);
            if (hdrOk && out[i + 1u] == N48_F828_RELMEM) {
                if (!found || gMut != MUT_FIRST_NOT_ONLY) at = i;
                found++;
            }
            i += len;
        }
        if (!found) { r->why = N48_F828_NO_NOP; return r->why; }
        if (found > 1u && gMut != MUT_FIRST_NOT_ONLY) { r->why = N48_F828_AMBIGUOUS; return r->why; }
    }

    r->candidates = found;
    r->nop_at = at; r->rel_at = at + 1u; r->data_at = at + 6u; r->next_at = at + 9u;
    r->addr_lo = out[at + 4u]; r->addr_hi = out[at + 5u];
    r->nop_hdr = out[at]; r->old_data = out[at + 6u];
    r->va = ((uint64_t)r->addr_hi << 32) | (uint64_t)r->addr_lo;

    const uint32_t ev = out[at + 2u];
    if ((ev & 0x3Fu) != 0x14u || ((ev >> 8) & 0xFu) != 5u) { r->why = N48_F828_EVENT; return r->why; }
    const uint32_t c = out[at + 3u];
    if (((c >> 16) & 0x3u) != 0u && gMut != MUT_NO_DST_SEL) { r->why = N48_F828_DST_SEL; return r->why; }
    if (gMut != MUT_NO_INT_SEL  && ((c >> 24) & 0x7u) != 0u) { r->why = N48_F828_INT_SEL;  return r->why; }
    if (gMut != MUT_NO_DATA_SEL && ((c >> 29) & 0x7u) != 1u) { r->why = N48_F828_DATA_SEL; return r->why; }
    if (gMut == MUT_ANY_PAGE) { r->why = N48_F828_OK; return r->why; }
    if (gMut == MUT_LIVE_OK) {                                     // "in the 0x4_xxxx_xxxx window and aligned"
        if ((r->va >> 32) != 4ull || (r->va & 3ull)) { r->why = N48_F828_NOT_DEAD; return r->why; }
        r->why = N48_F828_OK; return r->why;
    }
    if ((r->va & ~0xFFFull) == N48_F828_LIVE_PAGE) { r->why = N48_F828_LIVE_PAGE_REFUSED; return r->why; }
    if ((r->va & ~0xFFFull) != N48_F828_DEAD_PAGE || (r->va & 3ull)) { r->why = N48_F828_NOT_DEAD; return r->why; }
    r->why = N48_F828_OK;
    return r->why;
}

// ---------------------------------------------------------------------------------------------------------------------
// M10/M11/M12 — THE LATCH'S OWN MUTANTS. M10 IS THE DEFECT THIS CHANGE REMOVES: 0.0.377 printed its
// VERDICT from gXdF828.post, the last read, and so reported arm8's six matching polls as "the slot is UNCHANGED".
// ---------------------------------------------------------------------------------------------------------------------
static void mpoll(n48_f828_watch *w, uint32_t got, uint32_t val, uint32_t want)
{
    if (gMut != MUT_LATCH_NOT_STICKY) { n48_f828_watch_poll(w, got, val, want); return; }
    n48_f828_watch_poll(w, got, val, want);
    if (got && val != want) { w->ever = 0u; w->first_poll = 0u; }   // "ever" recomputed from the newest sample
}

static uint32_t mstate(uint64_t enabled, uint32_t committed, const n48_f828_watch *w, uint32_t want, uint32_t pre)
{
    if (gMut == MUT_LAST_READ_ONLY) {                               // the 0.0.377 ladder, verbatim in shape
        if (!enabled)        return N48_F828_ST_NEVER_ENABLED;
        if (!committed)      return N48_F828_ST_NOT_COMMITTED;
        if (!w || !w->polls) return N48_F828_ST_NOT_POLLED;
        if (!w->last_ok)     return N48_F828_ST_UNREADABLE;
        if (w->last == want) return N48_F828_ST_MATCHED;
        (void)pre;
        return N48_F828_ST_NEVER_MATCHED;                           // "the slot is UNCHANGED" - the lie
    }
    if (gMut == MUT_UNREADABLE_WINS) {
        if (!enabled)        return N48_F828_ST_NEVER_ENABLED;
        if (!committed)      return N48_F828_ST_NOT_COMMITTED;
        if (!w || !w->polls) return N48_F828_ST_NOT_POLLED;
        if (!w->last_ok)     return N48_F828_ST_UNREADABLE;         // BEFORE `ever`: proves-nothing beats proved
        if (w->ever)         return w->reverted ? N48_F828_ST_MATCHED_REVERTED : N48_F828_ST_MATCHED;
        return N48_F828_ST_NEVER_MATCHED;
    }
    (void)want; (void)pre;
    return n48_f828_state(enabled, committed, w);
}

// 0.0.413 (B3/B6): the value ENCODER, so a mutant that drops the epoch is a planted break. The real path is
// n48_f828_value; only the tests that construct a "stale" value go through mvalue.
static uint32_t mvalue(uint32_t epoch, uint32_t ordinal)
{
    if (gMut == MUT_EPOCH_DROP) return ordinal & 0xFFFFu;   // the epoch is dropped: last boot's value matches this one
    return n48_f828_value(epoch, ordinal);
}

// 0.0.415 (Q4): the CANDIDATE site and the COMMIT PROMOTION, as the kext calls them. M18 is 0.0.413's candidate
// site - it overwrites the committed record and clears the committed flag; M19 is a promotion that runs at the gate,
// before the keystone has proved the frame (so it promotes a frame that may be withdrawn); M17 resets the committed
// latch on a REFUSED candidate (0.0.413's B5 defect in its remaining form).
static void mplace(n48_f828_owned *cand, n48_f828_owned *committed, uint32_t *committedFlag,
                   uint32_t want, uint32_t epoch, uint32_t ordinal, uint64_t slot_va, uint64_t vram_off,
                   uint64_t va, uint32_t pre, uint32_t pre_ok, uint32_t old_data, uint64_t arm_va_base,
                   uint64_t arm_carve)
{
    n48_f828_owned_place(cand, want, epoch, ordinal, slot_va, vram_off, va, pre, pre_ok, old_data, arm_va_base, arm_carve);
    if (gMut == MUT_CAND_CLEARS_COMMITTED) {          // M18: the candidate site writes the committed record and clears it
        *committed = *cand;
        *committedFlag = 0u;
    }
}

static void mcommit_promote(n48_f828_owned *committed, const n48_f828_owned *cand, n48_f828_watch *w,
                            uint32_t permitted, uint32_t flight)
{
    if (gMut == MUT_PROMOTE_AT_GATE) {                // M19: promoted before the keystone's proof (permitted ignored)
        if (committed && cand) { *committed = *cand; committed->flight = flight; }
        n48_f828_watch_reset(w); return;
    }
    if (gMut == MUT_RESET_ON_CAND && !permitted) {    // M17: a REFUSED candidate still resets the committed latch
        n48_f828_watch_reset(w); return;
    }
    if (gMut == MUT_PROMOTE_STORE_FLIGHT_FIRST) {     // M22: the 0.0.419 order - copy/stamp THEN reset
        if (committed && cand) { *committed = *cand; committed->flight = flight; }
        // THE WINDOW. A concurrent reader right here sees the NEW flight on the record and, because the watch has not
        // been reset yet, the OLD sticky `ever`. It answers end-of-pipe for a flight whose fence was never promoted.
        if (committed && n48_f828_eop_seen(1u, w, committed->flight, flight)) gPromoteWindowFalseEop = 1u;
        n48_f828_watch_reset(w);
        return;
    }
    n48_f828_commit_promote(committed, cand, w, permitted, flight);
}

// 0.0.418 (E2): the end-of-pipe question, substitutable so M20 (the 0.0.417 rule, committed && ever) is a planted
// break that the E2 checks above must catch.
static uint32_t meop_seen(uint32_t committed, const n48_f828_watch *w, uint32_t rec_flight, uint32_t cur_flight)
{
    if (gMut == MUT_EOP_IGNORES_FLIGHT) return (committed && w && w->ever) ? 1u : 0u;
    return n48_f828_eop_seen(committed, w, rec_flight, cur_flight);
}

// 0.0.418 (E3): the REGION-MOVED verdict, substitutable so M21 (the 0.0.417 behaviour: the candidate is dropped but
// the frame still runs, and its executed un-promoted fence writes `want` into the next candidate's slot) is a planted
// break that the E3 checks must catch.
static uint32_t mregion_reason(uint32_t region_moved)
{
    if (gMut == MUT_REGION_MOVED_RUNS) return N48_F828_OK;
    return n48_f828_region_moved_reason(region_moved);
}

// 0.0.444 (an internal review note (B) item 1) — THE GATE BLOCK, AS THE KEXT RUNS IT, so the handoff test drives the REAL
// order rather than a hand-picked snapshot. Mirrors AppleHardwareHook.cpp's `if (gXdF828Pending) { gXdF828Pending =
// 0u; gXdF828GateOk = 0u; ... if (live && reason == OK) { gXdF828GateOk = 1u; gXdF828GateSeq = seq; } ... }` for the
// ONE branch this test needs (GATE OK). `pending` is cleared FIRST, exactly as the kext does, before the caller ever
// reaches the push site below.
struct MockF828State { uint32_t pending, gateOk, gateSeq; };
static void mgate_block(MockF828State *s, uint32_t hadCandidate, uint32_t gateSaysOk, uint32_t seq)
{
    if (!hadCandidate) return;             // the kext's own `if (gXdF828Pending)` guard
    s->pending = 0u;                       // THE CLEAR - happens before the push runs, on every frame with a candidate
    s->gateOk = 0u;
    if (gateSaysOk) { s->gateOk = 1u; s->gateSeq = seq; }
}
// The push site's OWN handoff call, substitutable so M23 (reading `pending` AFTER mgate_block already cleared it -
// 0.0.443's actual code) is a planted break driven through this SAME real sequence.
static uint32_t mhandoff(const MockF828State *s, uint32_t seq, uint32_t candOrdinal, uint64_t candVramOff,
                         uint32_t candWant, uint32_t *outOrdinal, uint64_t *outVramOff, uint32_t *outWant)
{
    if (gMut == MUT_HANDOFF_STALE_PENDING) {
        // 0.0.443's actual read: `gXdF828Pending ? gXdF828Cand.* : 0`. mgate_block already cleared `pending` above,
        // exactly as the real gate block clears gXdF828Pending before the push - so this ALWAYS reads 0/0/0.
        if (outOrdinal) *outOrdinal = s->pending ? candOrdinal : 0u;
        if (outVramOff) *outVramOff = s->pending ? candVramOff : 0ull;
        if (outWant)    *outWant    = s->pending ? candWant    : 0u;
        return s->pending;
    }
    return n48_f828_ring_handoff(s->gateOk, s->gateSeq, seq, candOrdinal, candVramOff, candWant,
                                 outOrdinal, outVramOff, outWant);
}

// 0.0.413 (B6): apply, with the two named defects planted. Returns N48_F828_OK on success, else a reason.
static uint32_t mapply(uint32_t *out, uint32_t n, const n48_f828 *r, uint64_t slot_va, uint32_t value)
{
    if (gMut == MUT_ADDR_SKIP) {                                    // M13: one address dword is not re-proven
        if (!out || !r || r->why != N48_F828_OK) return N48_F828_ARG;
        if (r->nop_at + 9u > n) return N48_F828_ARG;
        if (out[r->nop_at] != r->nop_hdr || r->nop_hdr != N48_F828_NOP9) return N48_F828_NOT_MATCHED;
        if (out[r->rel_at] != N48_F828_RELMEM) return N48_F828_NOT_MATCHED;
        if (out[r->data_at] != r->old_data)    return N48_F828_NOT_MATCHED;
        if (out[r->rel_at + 4u] != r->addr_hi) return N48_F828_ADDR_UNPROVEN;   // ... addr_lo is NOT re-proven
        out[r->nop_at]      = N48_F828_NOP1;
        out[r->rel_at + 3u] = (uint32_t)(slot_va & 0xFFFFFFFFull);
        out[r->rel_at + 4u] = (uint32_t)(slot_va >> 32);
        out[r->data_at]     = value;
        return N48_F828_OK;
    }
    if (gMut == MUT_NO_ADDR_REWRITE) {                              // M14: the address still names Apple's dead page
        const uint32_t w = n48_f828_apply(out, n, r, slot_va, kFencePageVa, value);
        if (w == N48_F828_OK) { out[r->rel_at + 3u] = r->addr_lo; out[r->rel_at + 4u] = r->addr_hi; }
        return w;
    }
    if (gMut != MUT_NO_VERIFY) return n48_f828_apply(out, n, r, slot_va, kFencePageVa, value);
    if (!out || !r || r->why != N48_F828_OK) return N48_F828_ARG;   // positional guard ONLY -'s defect verbatim
    if (r->nop_at + 9u > n) return N48_F828_ARG;
    out[r->nop_at]  = N48_F828_NOP1;
    out[r->data_at] = value;
    return N48_F828_OK;
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers over a working copy of a fixture.
// ---------------------------------------------------------------------------------------------------------------------
enum { kN = 1038u };
static uint32_t gBuf[kN];

static void load(const uint32_t *src) { memcpy(gBuf, src, sizeof gBuf); }

// ---------------------------------------------------------------------------------------------------------------------
static void run_checks()
{
    n48_f828 r {};

    // ---- 1. THE REAL SEGMENTS. Both are found, and NEITHER by position. -----------------------------------------
    load(kF828A);
    ck("A: real committed-shape segment -> OK", mfind(gBuf, kN, &r), N48_F828_OK);
    ck("A: found at output dword 1017", r.nop_at, 1017u);
    ck("A: exactly one candidate", r.candidates, 1u);
    ck("A: the packet VA is the dead page", r.va, N48_F828_DEAD_PAGE);
    ck("A: the ADDRESS words are the dead page", ((uint64_t)r.addr_hi << 32) | r.addr_lo, N48_F828_DEAD_PAGE);
    ck("A: Apple's own DATA_LO was 1", r.old_data, 1u);
    ck("A: the NOP header read back is c0071000", r.nop_hdr, N48_F828_NOP9);
    ck("A: the CP lands at 1026 either way", r.next_at, 1026u);

    load(kF828B);
    ck("B: the SAME packet 7 dwords later -> OK", mfind(gBuf, kN, &r), N48_F828_OK);
    ck("B: found at output dword 1024, NOT 1017", r.nop_at, 1024u);
    ck("B: a different slot of the dead array", r.va, N48_F828_DEAD_PAGE + 0xcull);

    // ---- 2. THE REWRITE PRESERVES THE STREAM, AND RE-POINTS THE ADDRESS AT OUR SLOT. ---------------------------
    for (int which = 0; which < 2; which++) {
        const uint32_t *src = which ? kF828B : kF828A;
        uint32_t p0 = 0u, p1 = 0u;
        load(src);
        ck("walk: the fixture itself walks end to end", n48_f828_walk_ok(gBuf, kN, &p0), 1u);
        (void)mfind(gBuf, kN, &r);
        const uint64_t slot = kFencePageVa + (uint64_t)(which ? 3u : kOrdinal) * N48_F828_SLOT_BYTES;
        ck("apply: wrote (N48_F828_OK)", mapply(gBuf, kN, &r, slot, kWant), N48_F828_OK);
        ck("apply: the NOP header is now the ONE-DWORD NOP", gBuf[r.nop_at], N48_F828_NOP1);
        ck("apply: DATA_LO now carries OUR value", gBuf[r.data_at], kWant);
        ck("apply: the RELEASE_MEM header is untouched", gBuf[r.rel_at], N48_F828_RELMEM);
        ck("apply: the address LOW word now names OUR slot", gBuf[r.rel_at + 3u], (uint32_t)(slot & 0xFFFFFFFFull));
        ck("apply: the address HIGH word now names OUR slot", gBuf[r.rel_at + 4u], (uint32_t)(slot >> 32));
        ck("apply: the next packet is untouched", gBuf[r.next_at], src[r.next_at]);
        ck("apply: the stream still walks and ends at n", n48_f828_walk_ok(gBuf, kN, &p1), 1u);
        ck("apply: exactly ONE more packet than before", p1, p0 + 1u);
        uint32_t diff = 0u;
        for (uint32_t i = 0; i < kN; i++) if (gBuf[i] != src[i]) diff++;
        ck("apply: EXACTLY FOUR dwords of the segment changed", diff, 4u);
    }
    {   // a slot VA OUTSIDE our fence page is refused by apply even if the caller computed one wrongly (SLOT-VA)
        load(kF828A);
        (void)mfind(gBuf, kN, &r);
        uint32_t before[kN]; memcpy(before, gBuf, sizeof before);
        ck("apply: a slot VA outside our page is refused",
           mapply(gBuf, kN, &r, kFencePageVa + 0x2000u, kWant), N48_F828_SLOT_VA);
        ck("  ... and nothing was written", memcmp(gBuf, before, sizeof before) == 0 ? 1u : 0u, 1u);
    }

    // ---- 3. EVERY FIELD IS A SEPARATE REFUSAL, AND A REFUSAL WRITES NOTHING. ------------------------------------
    struct { const char *what; uint32_t off; uint32_t val; uint32_t want; } perturb[] = {
        { "no trailing NOP at all (the 93-of-641 case)",   0u, 0xC0071001u, N48_F828_NO_NOP },
        { "a NOP of the wrong count breaks the walk",      0u, 0xC0081000u, N48_F828_WALK },
        { "the body is not a RELEASE_MEM",                 1u, 0xC0064A00u, N48_F828_NO_NOP },
        { "the event is not CACHE_FLUSH_AND_INV_TS",       2u, 0x00000515u, N48_F828_EVENT },
        { "EVENT_INDEX is not 5 (end of pipe)",            2u, 0x00000414u, N48_F828_EVENT },
        { "DST_SEL 1 (0.0.413: it must be the memory controller)", 3u, 0x20010000u, N48_F828_DST_SEL },
        { "DST_SEL 3: not the memory controller",          3u, 0x20030000u, N48_F828_DST_SEL },
        { "INT_SEL 3: it would raise an EOP interrupt",    3u, 0x23000000u, N48_F828_INT_SEL },
        { "INT_SEL 2 (0.0.413: anything but 0 refuses)",   3u, 0x22000000u, N48_F828_INT_SEL },
        { "INT_SEL 1: send interrupt only",                3u, 0x21000000u, N48_F828_INT_SEL },
        { "DATA_SEL 0: our value would not be written",    3u, 0x00000000u, N48_F828_DATA_SEL },
        { "DATA_SEL 3: a timestamp, not our value",        3u, 0x60000000u, N48_F828_DATA_SEL },
    };
    for (auto &p : perturb) {
        load(kF828A);
        gBuf[1017u + p.off] = p.val;
        const uint32_t why = mfind(gBuf, kN, &r);
        ck(p.what, why, p.want);
        uint32_t before[kN]; memcpy(before, gBuf, sizeof before);
        (void)mapply(gBuf, kN, &r, kSlotVa, kWant);
        ck("  ... and the stream was not written", memcmp(gBuf, before, sizeof before) == 0 ? 1u : 0u, 1u);
    }

    // ---- 4. THE PAGE. Apple's LIVE fence page is refused BY NAME. ------------------------------------------------
    {
        load(kF828A);
        gBuf[1021u] = (uint32_t)(N48_F828_LIVE_PAGE & 0xFFFFFFFFull);
        gBuf[1022u] = (uint32_t)(N48_F828_LIVE_PAGE >> 32);
        ck("the LIVE fence page 0x4_0000_1000 is refused", mfind(gBuf, kN, &r), N48_F828_LIVE_PAGE_REFUSED);
        ck("  ... and it is not written", (mapply(gBuf, kN, &r, kSlotVa, kWant) != N48_F828_OK &&
                                           gBuf[1017u] == N48_F828_NOP9) ? 1u : 0u, 1u);
    }
    {
        load(kF828A);
        gBuf[1021u] = 0x00050000u;                        // one page above the dead page
        ck("a VA outside the measured dead page is refused", mfind(gBuf, kN, &r), N48_F828_NOT_DEAD);
    }
    {
        load(kF828A);
        gBuf[1022u] = 0x00000005u;                        // a different 4 GiB window
        ck("the HIGH dword is checked too", mfind(gBuf, kN, &r), N48_F828_NOT_DEAD);
    }
    {
        load(kF828A);
        gBuf[1021u] = 0x00040002u;                        // inside the page, not dword-aligned
        ck("a misaligned slot inside the page is refused", mfind(gBuf, kN, &r), N48_F828_NOT_DEAD);
    }

    {   // A NOP LONGER THAN 9 DWORDS IS NOT A CANDIDATE, even with a perfectly good RELEASE_MEM as its body:
        // un-NOPing it would leave 3 orphan dwords after the packet, which the CP would read as a header.
        load(kF828A);
        gBuf[1018u] = 0xC0064A00u;                         // kill the genuine candidate's body
        gBuf[1027u] = N48_F828_RELMEM;                     // and plant one inside the 12-dword NOP at 1026
        for (uint32_t i = 0; i < 7u; i++) gBuf[1028u + i] = kF828A[1019u + i];
        uint32_t p = 0u;
        ck("the 12-dword-NOP stream is still well formed", n48_f828_walk_ok(gBuf, kN, &p), 1u);
        ck("a 12-dword NOP carrying a RELEASE_MEM is NOT taken", mfind(gBuf, kN, &r), N48_F828_NO_NOP);
        uint32_t before[kN]; memcpy(before, gBuf, sizeof before);
        (void)mapply(gBuf, kN, &r, kSlotVa, kWant);
        ck("  ... and nothing is written", memcmp(gBuf, before, sizeof before) == 0 ? 1u : 0u, 1u);
    }

    // ---- 5. AMBIGUITY. A second buried fence refuses; we do not guess which one Apple meant. ---------------------
    {
        load(kF828A);
        // Turn the 12-dword NOP at 1026 into a 9-dword NOP carrying a copy of the same RELEASE_MEM, then pad.
        gBuf[1026u] = N48_F828_NOP9;
        for (uint32_t i = 0; i < 8u; i++) gBuf[1027u + i] = kF828A[1018u + i];
        for (uint32_t i = 1035u; i < kN; i++) gBuf[i] = N48_F828_NOP1;
        uint32_t p = 0u;
        ck("the two-fence stream is still well formed", n48_f828_walk_ok(gBuf, kN, &p), 1u);
        ck("two buried fences -> AMBIGUOUS", mfind(gBuf, kN, &r), N48_F828_AMBIGUOUS);
        ck("  ... and the count is reported", r.candidates, 2u);
        uint32_t before[kN]; memcpy(before, gBuf, sizeof before);
        (void)mapply(gBuf, kN, &r, kSlotVa, kWant);
        ck("  ... and nothing is written", memcmp(gBuf, before, sizeof before) == 0 ? 1u : 0u, 1u);
    }

    // ---- 6. READ BEFORE WRITE. A dword that changed between find() and apply() refuses. --------------------------
    struct { const char *what; uint32_t off; uint32_t want; } race[] = {
        { "race: the NOP header changed since it was read",      0u, N48_F828_NOT_MATCHED },
        { "race: the RELEASE_MEM header changed since read",     1u, N48_F828_NOT_MATCHED },
        { "race: DATA_LO changed since it was read",             6u, N48_F828_NOT_MATCHED },
        { "race: the ADDRESS LOW word changed since it was read", 4u, N48_F828_ADDR_UNPROVEN },
        { "race: the ADDRESS HIGH word changed since it was read", 5u, N48_F828_ADDR_UNPROVEN },
    };
    for (auto &p : race) {
        load(kF828A);
        ck("race: find() accepted the untouched stream", mfind(gBuf, kN, &r), N48_F828_OK);
        gBuf[1017u + p.off] ^= 0x1u;                       // the world moved under us
        uint32_t before[kN]; memcpy(before, gBuf, sizeof before);
        ck(p.what, mapply(gBuf, kN, &r, kSlotVa, kWant), p.want);
        ck("  ... and NO dword was written", memcmp(gBuf, before, sizeof before) == 0 ? 1u : 0u, 1u);
    }
    {   // and a record that find() never blessed can never be applied
        load(kF828A);
        (void)mfind(gBuf, kN, &r);
        n48_f828 forged = r; forged.why = N48_F828_OK; forged.nop_at = 1024u; forged.rel_at = 1025u;
        forged.data_at = 1030u; forged.next_at = 1033u;
        uint32_t before[kN]; memcpy(before, gBuf, sizeof before);
        ck("a hand-built record over the wrong dwords refuses",
           mapply(gBuf, kN, &forged, kSlotVa, kWant) != N48_F828_OK ? 1u : 0u, 1u);
        ck("  ... and writes nothing", memcmp(gBuf, before, sizeof before) == 0 ? 1u : 0u, 1u);
    }

    // ---- 7. ARGUMENTS AND MALFORMED STREAMS. --------------------------------------------------------------------
    ck("null stream", mfind(nullptr, kN, &r), N48_F828_ARG);
    ck("null record", mfind(kF828A, kN, nullptr), N48_F828_ARG);
    {
        load(kF828A);
        ck("a stream shorter than the packet", mfind(gBuf, 9u, &r), N48_F828_ARG);
    }
    {
        load(kF828A);
        gBuf[0] = 0x40000000u;                             // TYPE1: a header we will not walk over
        ck("a stream we cannot walk is never edited", mfind(gBuf, kN, &r), N48_F828_WALK);
    }
    {
        load(kF828A);
        gBuf[0] = 0xC3FF5800u;                             // a packet that runs past the end of the segment
        ck("a packet running past the end refuses", mfind(gBuf, kN, &r), N48_F828_WALK);
    }
    {   // the one-dword NOP must be walked as ONE dword, or every translated segment fails the walk
        ck("the one-dword NOP is one dword", n48_f828_pkt_len(N48_F828_NOP1), 1u);
        ck("a 9-dword NOP is nine dwords", n48_f828_pkt_len(N48_F828_NOP9), 9u);
        ck("a TYPE2 header is one dword", n48_f828_pkt_len(0x80000000u), 1u);
        ck("a TYPE1 header is not walkable", n48_f828_pkt_len(0x40000000u), 0u);
    }

    // ---- 8. THE READ-BACK LATCH. ------------------------------------------------------------------
    {
        const uint32_t kWantV = 0x4E480828u, kPre = 0u;
        n48_f828_watch w {};

        // (a) arm8, exactly as it ran.
        n48_f828_watch_reset(&w);
        for (uint32_t i = 1u; i <= 8u; i++) mpoll(&w, 1u, i <= 6u ? kWantV : kPre, kWantV);
        ck("arm8: polls fed", w.polls, 8u);
        ck("arm8: all eight readable", w.reads_ok, 8u);
        ck("arm8: EVER equalled ours", w.ever, 1u);
        ck("arm8: first match at poll 1", w.first_poll, 1u);
        ck("arm8: six matches", w.matches, 6u);
        ck("arm8: it reverted", w.reverted, 1u);
        ck("arm8: reverted at poll 7", w.revert_poll, 7u);
        ck("arm8: the last read is kept, not discarded", w.last, kPre);
        ck("arm8: VERDICT is matched-then-reverted",
           mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_MATCHED_REVERTED);
        ck("arm8's sequence is NEVER reported as unchanged",
           mstate(1u, 1u, &w, kWantV, kPre) == N48_F828_ST_NEVER_MATCHED ? 1u : 0u, 0u);
        ck("arm8's sequence is NEVER reported as unreadable",
           mstate(1u, 1u, &w, kWantV, kPre) == N48_F828_ST_UNREADABLE ? 1u : 0u, 0u);

        // (b) ONE match in the middle, pre on either side - the minimal form of the same lie.
        n48_f828_watch_reset(&w);
        for (uint32_t i = 1u; i <= 8u; i++) mpoll(&w, 1u, i == 3u ? kWantV : kPre, kWantV);
        ck("one mid-sequence match still proves it", w.ever, 1u);
        ck("one mid-sequence match: first match at poll 3", w.first_poll, 3u);
        ck("one mid-sequence match is not 'unchanged'",
           mstate(1u, 1u, &w, kWantV, kPre) == N48_F828_ST_NEVER_MATCHED ? 1u : 0u, 0u);
        ck("one mid-sequence match: reverted at poll 4", w.revert_poll, 4u);

        // (c) a match on the LAST poll only: proved, and NOT reverted.
        n48_f828_watch_reset(&w);
        for (uint32_t i = 1u; i <= 8u; i++) mpoll(&w, 1u, i == 8u ? kWantV : kPre, kWantV);
        ck("a match on the last poll only", mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_MATCHED);
        ck("a match on the last poll did not revert", w.reverted, 0u);

        // (d) AN UNREADABLE TAIL CANNOT BURY A PROOF, and cannot count as a reversion (M12).
        n48_f828_watch_reset(&w);
        mpoll(&w, 1u, kWantV, kWantV);
        mpoll(&w, 0u, 0u, kWantV);
        mpoll(&w, 0u, 0u, kWantV);
        ck("an unreadable tail: the proof stands", mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_MATCHED);
        ck("an unreadable poll is not a reversion", w.reverted, 0u);
        ck("an unreadable poll is counted", w.polls, 3u);
        ck("an unreadable poll is not counted as a read", w.reads_ok, 1u);

        // (e) the honest negatives, each its own state.
        n48_f828_watch_reset(&w);
        for (uint32_t i = 0u; i < 8u; i++) mpoll(&w, 1u, kPre, kWantV);
        ck("never matched, all readable", mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_NEVER_MATCHED);
        ck("never matched sets no first poll", w.first_poll, 0u);
        n48_f828_watch_reset(&w);
        for (uint32_t i = 0u; i < 8u; i++) mpoll(&w, 0u, 0u, kWantV);
        ck("polled and NOT ONE read succeeded", mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_UNREADABLE);
        n48_f828_watch_reset(&w);
        ck("committed but never polled", mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_NOT_POLLED);
        ck("enabled but never committed", mstate(1u, 0u, &w, kWantV, kPre), N48_F828_ST_NOT_COMMITTED);
        ck("never enabled", mstate(0u, 0u, &w, kWantV, kPre), N48_F828_ST_NEVER_ENABLED);
        {   // a poll that matched cannot invent a fence that was never enabled: the ladder's first rung holds.
            n48_f828_watch t {};
            n48_f828_watch_reset(&t);
            mpoll(&t, 1u, kWantV, kWantV);
            ck("never enabled even with a matching poll", mstate(0u, 1u, &t, kWantV, kPre),
               N48_F828_ST_NEVER_ENABLED);
        }

        // (f) CHANGED-BUT-NOT-OURS after a match is a reversion too; and reset really resets.
        n48_f828_watch_reset(&w);
        mpoll(&w, 1u, kWantV, kWantV);
        mpoll(&w, 1u, 0x11111100u, kWantV);
        ck("changed-but-not-ours after a match reverts", w.revert_poll, 2u);
        ck("and the proof still stands", mstate(1u, 1u, &w, kWantV, kPre), N48_F828_ST_MATCHED_REVERTED);
        n48_f828_watch_reset(&w);
        ck("reset clears the latch", (uint64_t)w.ever + w.polls + w.first_poll + w.matches + w.reverted +
                                     w.revert_poll + w.reads_ok + w.last + w.last_ok, 0u);
        ck("a null watch is not a crash and not a proof", mstate(1u, 1u, nullptr, kWantV, kPre),
           N48_F828_ST_NOT_POLLED);
        n48_f828_watch_poll(nullptr, 1u, kWantV, kWantV);   // must not fault
        n48_f828_watch_reset(nullptr);
        ck("every state has a distinct name", n48_f828_state_name(N48_F828_ST_MATCHED_REVERTED)[0] == 'O' ? 1u : 0u,
           1u);
    }

    // ---- 9. 0.0.413 (B3/B6): THE epoch|ordinal CODEC. ----------------------------------------------------------
    // A value from another boot, another ordinal, or equal to the pre-commit read never counts as end-of-pipe.
    ck("value: epoch in the high 16", (n48_f828_value(0x1234u, 0x0000u) >> 16) & 0xFFFFu, 0x1234u);
    ck("value: ordinal in the low 16", n48_f828_value(0x0000u, 0x5678u) & 0xFFFFu, 0x5678u);
    ck("value: full codec", n48_f828_value(0x1234u, 0x5678u), 0x12345678u);
    ck("slot: ordinal mod 1024", n48_f828_slot(1024u), 0u);
    ck("slot: ordinal 1025 -> slot 1", n48_f828_slot(1025u), 1u);
    ck("slot: ordinal 4 -> slot 4", n48_f828_slot(4u), 4u);
    ck("slot VA: uses the slot, 4 bytes each", n48_f828_slot_va(kFencePageVa, 1025u), kFencePageVa + 4u);
    ck("slot VA: ordinal 7", n48_f828_slot_va(kFencePageVa, kOrdinal), kSlotVa);
    ck("match: our own value matches", n48_f828_value_matches(mvalue(kEpoch, kOrdinal), kEpoch, kOrdinal), 1u);
    ck("match: ANOTHER boot's epoch does not match",
       n48_f828_value_matches(mvalue(kEpoch + 1u, kOrdinal), kEpoch, kOrdinal), 0u);
    ck("match: a LATER ordinal does not prove an earlier",
       n48_f828_value_matches(mvalue(kEpoch, kOrdinal + 1u), kEpoch, kOrdinal), 0u);
    ck("match: an EARLIER ordinal does not prove a later",
       n48_f828_value_matches(mvalue(kEpoch, kOrdinal - 1u), kEpoch, kOrdinal), 0u);
    ck("match: epoch 0 is never a proof (unstampable)",
       n48_f828_value_matches(mvalue(0u, kOrdinal), 0u, kOrdinal), 0u);
    ck("match: the same slot, another ordinal, is not a match",
       n48_f828_value_matches(n48_f828_value(kEpoch, kOrdinal + 1024u), kEpoch, kOrdinal), 0u);

    // ---- 10. 0.0.413 (B4/B6): THE OWNED-SLOT GATE, each refusal its own name. ----------------------------------
    // 0.0.415 (Q3/Q1): `pre_ok` and `multi_segment` are the two new clauses. The 8th argument of the 0.0.413 calls
    // (pre_ok) is 1 "the pre-read succeeded" and the 9th (multi_segment) is 0 "a single-segment frame". `want` follows.
    ck("gate: ring not built -> NO-RING",
       n48_f828_owned_gate(0u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, 0u, 1u, kWant, 0u), N48_F828_NO_RING);
    ck("gate: vaBase 0 -> NO-RING",
       n48_f828_owned_gate(1u, 0ull, 1u, kEpoch, kFencePageVa, kSlotVa, 0u, 1u, kWant, 0u), N48_F828_NO_RING);
    ck("gate: root[511] not armed -> NO-ROOT",
       n48_f828_owned_gate(1u, kFencePageVa, 0u, kEpoch, kFencePageVa, kSlotVa, 0u, 1u, kWant, 0u), N48_F828_NO_ROOT);
    ck("gate: epoch 0 -> EPOCH (a watch is never armed unstamped)",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, 0u, kFencePageVa, kSlotVa, 0u, 1u, kWant, 0u), N48_F828_EPOCH);
    ck("gate: slot VA outside our page -> SLOT-VA",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa + 0x2000u, 0u, 1u, kWant, 0u),
       N48_F828_SLOT_VA);
    ck("gate: slot already holds the target (value == pre) -> SLOT-PRE",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, kWant, 1u, kWant, 0u), N48_F828_SLOT_PRE);
    ck("gate: a stale zero pre is not the target -> OK",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, 0u, 1u, kWant, 0u), N48_F828_OK);
    ck("gate: the all-clear is OK (positive control)",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, 0xDEADBEEFu, 1u, kWant, 0u), N48_F828_OK);
    ck("gate: another boot's stale value is not the target -> OK",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa,
                           n48_f828_value(kEpoch - 1u, kOrdinal), 1u, kWant, 0u), N48_F828_OK);
    // 0.0.415 (Q3): an UNREADABLE pre-read refuses by name. Through 0.0.413 the failed read left `pre` 0 and the gate
    // compared that 0 against `want` and PASSED - the positive control silently vanished.
    ck("gate: a FAILED pre-read -> PRE-UNREADABLE (Q3)",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, 0u, 0u, kWant, 0u),
       N48_F828_PRE_UNREADABLE);
    ck("gate: a failed pre-read refuses even with the all-clear otherwise",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, 0xDEADBEEFu, 0u, kWant, 0u),
       N48_F828_PRE_UNREADABLE);
    // 0.0.415 (Q1): a multi-segment frame refuses by name - the packet would claim end of pipe at the end of THIS
    // segment, before the later segments ran.
    ck("gate: a multi-segment frame -> MULTI-SEGMENT (Q1)",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa, 0u, 1u, kWant, 1u),
       N48_F828_MULTI_SEGMENT);
    ck("gate: MULTI-SEGMENT is asked before the slot checks",
       n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, kSlotVa + 0x2000u, 0u, 0u, kWant, 1u),
       N48_F828_MULTI_SEGMENT);

    // ---- 11. 0.0.415 (Q4/B5): THE WATCH RESET IS AT THE COMMIT PROMOTION, NOT AT THE CANDIDATE. -----------------
    {
        n48_f828_owned committed {}, cand {};
        n48_f828_watch w {};
        n48_f828_watch_reset(&w);
        mpoll(&w, 1u, kWant, kWant);                       // a committed frame's latch: ever = 1
        n48_f828_owned_place(&cand, kWant + 1u, kEpoch, kOrdinal + 1u, kSlotVa + 4u, 0x2000u, N48_F828_DEAD_PAGE,
                             0u, 1u, 0u, kFencePageVa, 0x100u);
        ck("B5: the candidate site leaves `ever` intact", w.ever, 1u);
        ck("B5: ...and `first_poll` intact", w.first_poll, 1u);
        ck("B5: ...and the poll count intact", w.polls, 1u);
        n48_f828_commit_promote(&committed, &cand, &w, 0u, 1u);    // a REFUSED candidate: nothing changes
        ck("B5: a REFUSED candidate leaves `ever` intact", w.ever, 1u);
        ck("B5: ...and does not copy the candidate", committed.want, 0u);
        n48_f828_commit_promote(&committed, &cand, &w, 1u, 1u);    // the real commit promotion
        ck("B5: the promotion copies the candidate", committed.want, kWant + 1u);
        ck("B5: the promotion resets the watch", (uint64_t)w.ever + w.polls + w.first_poll + w.matches, 0u);
        n48_f828_commit_promote(nullptr, &cand, &w, 1u, 1u);       // must not fault
        n48_f828_commit_promote(&committed, nullptr, &w, 1u, 1u);  // must not fault
        n48_f828_commit_promote(&committed, &cand, nullptr, 1u, 1u);
        ck("B5: null arguments are not a crash", 1u, 1u);
    }

    // ---- 11b. 0.0.420 ('s E2 follow-up): THE PROMOTION'S WRITE ORDER. Reset the watch FIRST, then copy/stamp, so
    // no reader can see the new flight on the record together with the old sticky `ever` (a false end-of-pipe for a
    // flight whose fence was never promoted). The real function is only observable after both writes; M22 injects the
    // read into the 0.0.419 order (copy/stamp THEN reset) and must be caught here. ------------------------------
    {
        n48_f828_owned committed {}, cand {};
        n48_f828_watch w {}; n48_f828_watch_reset(&w);
        committed.flight = 3u;                              // an OLD committed flight
        mpoll(&w, 1u, kWant, kWant);                        // its latch is set: ever = 1
        n48_f828_owned_place(&cand, kWant, kEpoch, kOrdinal + 1u, kSlotVa, 0x1000u, N48_F828_DEAD_PAGE,
                             0u, 1u, 0u, kFencePageVa, 0x100u);
        gPromoteWindowFalseEop = 0u;
        mcommit_promote(&committed, &cand, &w, 1u, 9u);     // promote a NEW flight 9 on top of the old latch
        ck("E2-order: the promotion resets the latch", w.ever, 0u);
        ck("E2-order: the promotion stamps the new flight", committed.flight, 9u);
        ck("E2-order: NO read between the writes answered EOP for the new flight", gPromoteWindowFalseEop, 0u);
    }

    // ---- 13. 0.0.415 (Q4): THE REAL FLOW - CANDIDATE PLACED, REFUSED, THEN THE COMMITTED FRAME'S POLL. ---------
    // THIS IS THE DEFECT THE 0.0.413 REVIEW CONFIRMED. Through 0.0.413 the translate-time candidate site overwrote the
    // committed frame's want/vramOff/ordinal/epoch/slot/pre AND cleared `committed`, so `ks_eop_seen()` (committed &&
    // ever) and the poll (gated on committed) both died and an end-of-pipe window became a timeout. The old test (M17
    // above) hand-set the watch and could not see it. M18 (candidate clears the committed record) and M19 (promotion at
    // the gate, before the keystone proves the frame) MUST fail here. Everything routes through mplace/mcommit_promote,
    // so each mutant replaces the REAL call the kext makes.
    {
        const uint32_t kWantA = n48_f828_value(kEpoch, 5u), kWantB = n48_f828_value(kEpoch, 6u);
        const uint32_t kFlightA = 5u, kFlightB = 6u;   // 0.0.418 (E2): the two flights' own gKsFlight.seq
        n48_f828_owned committed {}, cand {};
        n48_f828_watch w {}; uint32_t committedFlag = 0u;
        // A: a candidate placed and permitted (the keystone proved it): it becomes the committed frame.
        mplace(&cand, &committed, &committedFlag, kWantA, kEpoch, 5u, kFencePageVa + 20u, 0x1000u,
               N48_F828_DEAD_PAGE, 0u, 1u, 1u, kFencePageVa, 0x100u);
        mcommit_promote(&committed, &cand, &w, 1u, kFlightA); committedFlag = 1u;
        ck("Q4: A committed", committedFlag, 1u);
        ck("Q4: the committed record is A's", committed.want, kWantA);
        ck("E2: the committed record carries A's flight", committed.flight, kFlightA);
        mpoll(&w, 1u, kWantA, committed.want);
        ck("Q4: A's poll matched", w.ever, 1u);
        ck("Q4: A's poll count", w.polls, 1u);
        ck("E2: A's latch answers end-of-pipe for A's own flight", meop_seen(1u, &w, committed.flight, kFlightA), 1u);
        // B: a NEW candidate. The candidate site must not touch A's record, A's latch or `committed`.
        mplace(&cand, &committed, &committedFlag, kWantB, kEpoch, 6u, kFencePageVa + 24u, 0x1004u,
               N48_F828_DEAD_PAGE, 0u, 1u, 1u, kFencePageVa, 0x100u);
        ck("Q4: a new candidate leaves A's want", committed.want, kWantA);
        ck("Q4: ... A's vramOff", committed.vram_off, 0x1000u);
        ck("Q4: ... A's slot", committed.slot, 5u);
        ck("Q4: ... A's latch", w.ever, 1u);
        ck("Q4: ... A's poll count", w.polls, 1u);
        ck("Q4: ... and A stays committed", committedFlag, 1u);
        // 0.0.418 (E2): A's latch is STILL SET (Q4) while flight B is now the current one. Under 0.0.417's
        // `committed && ever` rule it would answer end-of-pipe for B - a proof that belongs to a frame that already
        // retired. With the flight recorded at promotion it cannot: B's fence was never promoted, so the record still
        // says A's flight. M20 (the 0.0.417 rule) MUST fail exactly here.
        ck("E2: A's flight is NOT the current one", (committed.flight == kFlightB) ? 1u : 0u, 0u);
        ck("E2: A's latch does NOT answer end-of-pipe for B", meop_seen(1u, &w, committed.flight, kFlightB), 0u);
        ck("E2: ... and it STILL answers for A's own flight", meop_seen(1u, &w, committed.flight, kFlightA), 1u);
        // B REFUSED at the gate: no promotion. A's poll must still run and still match.
        mpoll(&w, 1u, kWantA, committed.want);
        ck("Q4: a refused candidate does not stop A's poll", w.ever, 1u);
        ck("Q4: ... A's poll advanced", w.polls, 2u);
        // B passes the gate but the keystone REFUSES it: the promotion is not permitted, so nothing changes.
        mcommit_promote(&committed, &cand, &w, 0u, kFlightB);
        ck("Q4: a keystone-refused frame leaves A's want", committed.want, kWantA);
        ck("Q4: ... A's vramOff", committed.vram_off, 0x1000u);
        ck("Q4: ... A's latch", w.ever, 1u);
        ck("Q4: ... and A stays committed", committedFlag, 1u);
        ck("E2: a keystone-refused flight B leaves A's flight on the record", committed.flight, kFlightA);
        ck("E2: ... so B's end-of-pipe is still not answered", meop_seen(1u, &w, committed.flight, kFlightB), 0u);
        mpoll(&w, 1u, kWantA, committed.want);
        ck("Q4: ... A's poll still matches", w.ever, 1u);
        ck("Q4: ... A's polls", w.polls, 3u);
        // and a keystone that DOES prove the next frame promotes it, with a fresh latch.
        mcommit_promote(&committed, &cand, &w, 1u, kFlightB);
        ck("Q4: a permitted promotion copies B", committed.want, kWantB);
        ck("Q4: ... and resets the latch", (uint64_t)w.ever + w.polls, 0u);
        ck("E2: a permitted promotion records B's flight", committed.flight, kFlightB);
        mpoll(&w, 1u, kWantB, kWantB);
        ck("E2: ... so B's own end-of-pipe IS answered now", meop_seen(1u, &w, committed.flight, kFlightB), 1u);
    }

    // ---- 14. 0.0.418 (E3): REGION-MOVED NEUTERS THE FRAME, SO AN UN-PROMOTED FENCE CANNOT PIN THE NEXT CANDIDATE. --
    // Through 0.0.417 the REGION-MOVED branch dropped the candidate but left the frame's verdict at COMMIT: the frame
    // ran, its un-NOPed packet executed and wrote `want` into the slot the NEXT candidate recomputes (no promotion
    // advanced the committed ordinal), and that candidate read it back as its `pre` and was refused SLOT-PRE - and so
    // was every candidate after it, for the rest of the boot. This drives both halves: the verdict (never OK for a
    // moved region), and the consequence at the next candidate's gate. M21 is the 0.0.417 behaviour verbatim.
    {
        const uint32_t want = n48_f828_value(kEpoch, kOrdinal);
        const uint64_t slot_va = n48_f828_slot_va(kFencePageVa, kOrdinal);
        // (a) THE VERDICT.
        ck("E3: a moved region is not OK (the frame is neutered)", mregion_reason(1u), N48_F828_REGION_MOVED);
        ck("E3: a region that did NOT move is OK (positive control)", mregion_reason(0u), N48_F828_OK);
        // (b) THE CONSEQUENCE. A neutered frame writes NOTHING, so the slot keeps its pre value and the next
        // candidate's gate passes. A frame that ran writes `want`, so that gate refuses SLOT-PRE.
        uint32_t slot = 0u;
        if (mregion_reason(1u) == N48_F828_OK) slot = want;   // M21: the frame ran; its executed packet wrote `want`
        ck("E3: the next candidate is NOT pinned at SLOT-PRE",
           n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, slot_va, slot, 1u, want, 0u), N48_F828_OK);
        ck("E3: ... and a frame that DID run would have pinned it",
           n48_f828_owned_gate(1u, kFencePageVa, 1u, kEpoch, kFencePageVa, slot_va, want, 1u, want, 0u),
           N48_F828_SLOT_PRE);
        // The line itself, at maximal fields and WITH the logger's own prefix and newline, must fit under 512 bytes.
        {
            char line[512];
            const int n = snprintf(line, sizeof line,
                "AppleHardwareHook: fence828: REGION-MOVED - the ring region moved between the fence and the gate "
                "(carve %#llx -> %#llx, VA base %#llx -> %#llx), so the owned slot VA names different bytes. THE "
                "FRAME IS NEUTERED (reason %s): an executed, un-promoted fence would pin every later candidate at "
                "SLOT-PRE. The committed record and its latch are untouched.\n",
                0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
                "FENCE-REGION-MOVED");
            ck("E3: the REGION-MOVED log line fits under 512 bytes", (uint64_t)n < 512u, 1u);
        }
    }

    // ---- 15. 0.0.444 (an internal review note (B) item 1) — THE FLIGHT-RING FENCE HANDOFF, DRIVEN THROUGH THE REAL
    // ORDER: the gate block runs FIRST (clearing "pending", then setting gateOk/gateSeq only on GATE OK), and ONLY
    // THEN does the push site's handoff run - exactly the two-step sequence AppleHardwareHook.cpp performs, and
    // exactly the order the 0.0.443 headline defect depended on (reading `gXdF828Pending` AFTER its own clear
    // always answers 0). M23 is that literal defect, planted through mhandoff/mgate_block. ------------------------
    {
        const uint32_t seq = 42u;
        const uint32_t candOrdinal = 7u; const uint64_t candVramOff = 0x9000ull; const uint32_t candWant = 0xABCDu;
        // (a) GATE OK: a fence candidate WAS offered and the gate answered OK for THIS frame's seq.
        MockF828State s {}; s.pending = 1u;   // a candidate is sitting in gXdF828Cand, awaiting the gate
        mgate_block(&s, /*hadCandidate=*/1u, /*gateSaysOk=*/1u, seq);
        ck("item 1: the gate block cleared pending (the REAL order)", s.pending, 0u);
        ck("item 1: the gate block recorded GATE OK for this seq", s.gateOk, 1u);
        uint32_t oOrd = 0u; uint64_t oVram = 0ull; uint32_t oWant = 0u;
        const uint32_t has = mhandoff(&s, seq, candOrdinal, candVramOff, candWant, &oOrd, &oVram, &oWant);
        ck("item 1: GATE OK -> the push carries the REAL candidate's fields", has, 1u);
        ck("item 1: ... ordinal", oOrd, candOrdinal);
        ck("item 1: ... vram_off", (uint64_t)oVram, candVramOff);
        ck("item 1: ... want", oWant, candWant);

        // (b) GATE NOT OK (refused, or a different reason): the push must carry a fence-less 0/0/0.
        MockF828State s2 {}; s2.pending = 1u;
        mgate_block(&s2, 1u, /*gateSaysOk=*/0u, seq);
        uint32_t oOrd2 = 1u; uint64_t oVram2 = 1ull; uint32_t oWant2 = 1u;
        const uint32_t has2 = mhandoff(&s2, seq, candOrdinal, candVramOff, candWant, &oOrd2, &oVram2, &oWant2);
        ck("item 1: GATE refused -> fence-less push (has=0)", has2, 0u);
        ck("item 1: ... ordinal 0", oOrd2, 0u);
        ck("item 1: ... vram_off 0", (uint64_t)oVram2, 0ull);
        ck("item 1: ... want 0", oWant2, 0u);

        // (c) NO CANDIDATE OFFERED AT ALL (switch off / no buried RELEASE_MEM this frame), but a STALE gateOk==1
        // survives from an EARLIER frame with a DIFFERENT seq. The seq comparison, not a bare gateOk check, is
        // what stops this frame's push from reading a fence that belongs to someone else.
        MockF828State s3 {}; s3.pending = 0u; s3.gateOk = 1u; s3.gateSeq = 41u;   // stale, from a PRIOR frame
        uint32_t oOrd3 = 1u; uint64_t oVram3 = 1ull; uint32_t oWant3 = 1u;
        const uint32_t has3 = mhandoff(&s3, seq /* THIS frame's seq, 42, != stale 41 */, candOrdinal, candVramOff,
                                       candWant, &oOrd3, &oVram3, &oWant3);
        ck("item 1: a stale GATE OK from a DIFFERENT seq never leaks into this push", has3, 0u);
        ck("item 1: ... ordinal 0", oOrd3, 0u);
        // M23 (0.0.444, an internal review note (B) item 1) is caught by check (a) above through the SAME real
        // gate-then-push order this whole block drives: mhandoff() branches on the file's own `gMut`, exactly as
        // every other m-prefixed wrapper here does, so when the outer driver (main(), below) selects
        // MUT_HANDOFF_STALE_PENDING and re-runs this whole function, check (a)'s `has == 1` and `oWant ==
        // candWant` assertions fail - `mgate_block` has already cleared `pending` by the time mhandoff() reads it
        // under that mutant, exactly as gfxsrc_commit_try's real gate block clears gXdF828Pending before the push.
    }

    // ---- 12. EVERY REFUSAL HAS A NAME, AND EVERY LINE STAYS UNDER THE LOGGER'S 512-BYTE CAP. -------------------
    for (uint32_t why = 0; why < N48_F828_REASONS; why++)
        if (n48_f828_reason_name(why)[0] == '?') ck("every refusal has a name", why, 0xFFFFu);
    // build 0.0.508: TAIL-WORK (switch 71, n48_f828_find_last) is the 22nd reason.
    ck("reasons: 22 (19 + pre-unreadable + multi-segment + tail-work)", N48_F828_REASONS, 22u);
    ck("reason name: pre-unreadable", n48_f828_reason_name(N48_F828_PRE_UNREADABLE)[0] == 'P' ? 1u : 0u, 1u);
    ck("reason name: multi-segment", n48_f828_reason_name(N48_F828_MULTI_SEGMENT)[0] == 'M' ? 1u : 0u, 1u);
    {   // the worst-case per-segment line (0.0.413), assembled with maximal fields, must fit n48log's 512-byte body
        char line[512];
        const int n = snprintf(line, sizeof line,
            "fence828: seg %u %s - %s at dword %u of %u, Apple VA %#llx, owned slot %u/%u VA %#llx (vram+%#llx), "
            "epoch %#x ordinal %u, ours %#x; slot read %s%#x BEFORE. Four dwords, nothing appended, INT_SEL 0.",
            4294967295u, "not enabled", n48_f828_reason_name(N48_F828_SLOT_VA), 4294967295u, 4294967295u,
            0xFFFFFFFFFFFFFFFFull, 1023u, 1024u, 0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFFFFFFull,
            0xFFFFu, 65535u, 0xFFFFFFFFu, "NOT READABLE,", 0xFFFFFFFFu);
        ck("the per-segment line fits under 512 bytes", (uint64_t)n < 512u, 1u);
    }
    {   // 0.0.415: the owned-slot refusals line gained pre-unreadable and multi-segment; it must still fit.
        char line[512];
        const int n = snprintf(line, sizeof line,
            "fence828: owned-slot refusals - no-ring %llu, no-root %llu, slot-VA-outside-page %llu, addr-word-unproven "
            "%llu, slot-already-target %llu, epoch-unstampable %llu, region-moved %llu, pre-unreadable %llu, "
            "multi-segment %llu; last epoch %#x, ordinal %u, slot %u of %u.",
            18446744073709551615ull, 18446744073709551615ull, 18446744073709551615ull, 18446744073709551615ull,
            18446744073709551615ull, 18446744073709551615ull, 18446744073709551615ull, 18446744073709551615ull,
            18446744073709551615ull, 0xFFFFFFFFu, 4294967295u, 1023u, 1024u);
        ck("the owned-slot refusals line fits under 512 bytes", (uint64_t)n < 512u, 1u);
    }
}


// ---------------------------------------------------------------------------------------------------------------------
// 16. build 0.0.508 (switch 71) — THE LAST-CANDIDATE RULE, n48_f828_find_last, over REAL run10p bytes
// (fixture_fence71_run10p.h: each slice runs from the named start dword to the end of its frame's IB1, the final segment's
// end). Absolute dword = slice index + the slice's start. Run once, outside the mutant loop: the planted breaks for this
// rule are source edits (the build's plant list), each of which must make a check below fail.
// ---------------------------------------------------------------------------------------------------------------------
static const uint32_t kF71N52 = (uint32_t)(sizeof(kF71F52) / 4u), kF71N47 = (uint32_t)(sizeof(kF71F47) / 4u);
static const uint32_t kF71N98 = (uint32_t)(sizeof(kF71F98) / 4u);
static uint32_t f71_same(const n48_f828 &a, const n48_f828 &b)
{
    return a.why == b.why && a.nop_at == b.nop_at && a.rel_at == b.rel_at && a.data_at == b.data_at && a.next_at == b.next_at &&
           a.addr_lo == b.addr_lo && a.addr_hi == b.addr_hi && a.nop_hdr == b.nop_hdr && a.old_data == b.old_data &&
           a.candidates == b.candidates && a.va == b.va;
}
static void checks508()
{
    printf("\n== 16. 0.0.508: n48_f828_find_last (switch 71) over real run10p final slices ==\n");
    static uint32_t w[16384];
    n48_f828 r0 {}, r1 {};
    // (1) FRAME b, F52: today AMBIGUOUS (two candidates); the rule picks the LAST (14531, DATA 0x1f), never the first (13161).
    ck("F52: n48_f828_find (71 OFF) answers AMBIGUOUS", n48_f828_find(kF71F52, kF71N52, &r0), N48_F828_AMBIGUOUS);
    ck("F52: ... over 2 candidates", r0.candidates, 2u);
    ck("F52: find_last (71 ON) answers OK", n48_f828_find_last(kF71F52, kF71N52, &r1), N48_F828_OK);
    ck("F52: ... the chosen candidate is IB1 dword 14531 (the LAST)", kF71F52Start + r1.nop_at, 14531u);
    ck("F52: ... rel/data/next at +1/+6/+9", (r1.rel_at == r1.nop_at + 1u && r1.data_at == r1.nop_at + 6u && r1.next_at == r1.nop_at + 9u) ? 1u : 0u, 1u);
    ck("F52: ... its DATA is 0x1f (the maximum)", r1.old_data, 0x1fu);
    ck("F52: ... its VA is 0x4_0004_0010", r1.va, 0x400040010ull);
    ck("F52: ... candidates 2", r1.candidates, 2u);
    ck("F52: ... the first candidate (13161, DATA 0x1e) is NOT chosen", kF71F52Start + r1.nop_at != 13161u ? 1u : 0u, 1u);
    {   // the chosen record drives n48_f828_apply exactly as a lone candidate's would: four dwords of the LAST packet change
        std::memcpy(w, kF71F52, sizeof kF71F52);
        uint32_t p0 = 0, p1 = 0; (void)n48_f828_walk_ok(w, kF71N52, &p0);
        ck("F52: apply over the chosen record is OK", n48_f828_apply(w, kF71N52, &r1, kSlotVa, kFencePageVa, kWant), N48_F828_OK);
        uint32_t diff = 0, outside = 0;
        for (uint32_t i = 0; i < kF71N52; i++) if (w[i] != kF71F52[i]) { diff++; if (i < r1.nop_at || i >= r1.next_at) outside++; }
        ck("F52: ... exactly four dwords change", diff, 4u);
        ck("F52: ... all inside the LAST candidate's packet", outside, 0u);
        ck("F52: ... the first candidate's NOP header is untouched", w[13161u - kF71F52Start], N48_F828_NOP9);
        ck("F52: ... the stream still walks to its end", n48_f828_walk_ok(w, kF71N52, &p1), 1u);
        ck("F52: ... with exactly one more packet", p1, p0 + 1u);
    }
    // (2) F47: ONE candidate at 14329 - the rule answers exactly what find answers (the same record).
    ck("F47: find answers OK", n48_f828_find(kF71F47, kF71N47, &r0), N48_F828_OK);
    ck("F47: ... at 14329", kF71F47Start + r0.nop_at, 14329u);
    ck("F47: find_last answers OK", n48_f828_find_last(kF71F47, kF71N47, &r1), N48_F828_OK);
    ck("F47: ... the SAME record as find (unchanged at 14329)", f71_same(r0, r1), 1u);
    // (3) family e, F98: two candidates, the last at 12902 with the maximum DATA.
    ck("F98 (family e): find AMBIGUOUS", n48_f828_find(kF71F98, kF71N98, &r0), N48_F828_AMBIGUOUS);
    ck("F98 (family e): find_last OK", n48_f828_find_last(kF71F98, kF71N98, &r1), N48_F828_OK);
    ck("F98 (family e): ... at 12902", kF71F98Start + r1.nop_at, 12902u);
    ck("F98 (family e): ... DATA 0x1f", r1.old_data, 0x1fu);
    const uint32_t t52 = 14540u - kF71F52Start;   // F52's tail after the last candidate: one NOP, count 2 (4 dwords)
    ck("F52: the tail after the last candidate is one 4-dword NOP (the plant site)", kF71F52[t52], 0xC0021000u);
    // (4) a planted DRAW after the last candidate -> TAIL-WORK (DRAW_INDEX_AUTO, 3 dwords, then a one-dword NOP)
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[t52] = 0xC0012D00u; w[t52 + 1u] = 3u; w[t52 + 2u] = 2u; w[t52 + 3u] = N48_F828_NOP1;
    ck("planted DRAW after the last candidate: find_last TAIL-WORK", n48_f828_find_last(w, kF71N52, &r1), N48_F828_TAIL_WORK);
    ck("planted DRAW: ... find (71 OFF) still AMBIGUOUS", n48_f828_find(w, kF71N52, &r0), N48_F828_AMBIGUOUS);
    // (5) a NOP-wrapped pool record (D_TBL_NOP_HDR, count 2) whose BODY looks like a draw -> still OK (it never executes)
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[t52] = 0xC0021000u; w[t52 + 1u] = 0xC0012D00u; w[t52 + 2u] = 3u; w[t52 + 3u] = 2u;
    ck("NOP-wrapped pool record after the last candidate: OK", n48_f828_find_last(w, kF71N52, &r1), N48_F828_OK);
    ck("... still the last candidate", kF71F52Start + r1.nop_at, 14531u);
    // (6) VA mismatch -> AMBIGUOUS
    const uint32_t c0 = 13161u - kF71F52Start;
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[c0 + 4u] = 0x00040014u;
    ck("VA mismatch between candidates: AMBIGUOUS", n48_f828_find_last(w, kF71N52, &r1), N48_F828_AMBIGUOUS);
    // (7) reversed DATA -> AMBIGUOUS; equal DATA -> AMBIGUOUS (strictly rising)
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[c0 + 6u] = 0x20u;
    ck("reversed DATA (0x20 then 0x1f): AMBIGUOUS", n48_f828_find_last(w, kF71N52, &r1), N48_F828_AMBIGUOUS);
    w[c0 + 6u] = 0x1fu;
    ck("equal DATA (0x1f then 0x1f): AMBIGUOUS", n48_f828_find_last(w, kF71N52, &r1), N48_F828_AMBIGUOUS);
    // (8) a SINGLE candidate followed by a draw -> TAIL-WORK (find still OK: the rule is the only change)
    std::memcpy(w, kF71F47, sizeof kF71F47);
    const uint32_t t47 = 14338u - kF71F47Start;   // F47: the 14-dword NOP right after its candidate
    ck("F47: the packet after the candidate is a 14-dword NOP (the plant site)", w[t47], 0xC00C1000u);
    w[t47] = 0xC0012D00u; w[t47 + 1u] = 3u; w[t47 + 2u] = 2u;
    for (uint32_t i = t47 + 3u; i < t47 + 14u; i++) w[i] = N48_F828_NOP1;
    ck("single candidate + a draw after it: find_last TAIL-WORK", n48_f828_find_last(w, kF71N47, &r1), N48_F828_TAIL_WORK);
    ck("... find (71 OFF) still OK", n48_f828_find(w, kF71N47, &r0), N48_F828_OK);
    // (9) (a): an EARLIER candidate failing today's field checks refuses with that reason (the checks are find's own)
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[c0 + 3u] |= 1u << 24;   // INT_SEL 1 on the FIRST candidate
    ck("first candidate INT_SEL != 0: find_last INT-SEL", n48_f828_find_last(w, kF71N52, &r1), N48_F828_INT_SEL);
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[c0 + 4u] = 0x00001000u; w[c0 + 5u] = 4u;   // first candidate at the LIVE page (and so a VA mismatch too)
    ck("first candidate at the LIVE page: LIVE-PAGE-REFUSED", n48_f828_find_last(w, kF71N52, &r1), N48_F828_LIVE_PAGE_REFUSED);
    // (10) no candidate, malformed walk, short slice: exactly find's answer
    std::memcpy(w, kF71F52, sizeof kF71F52);
    w[c0] = 0xC0071000u; w[c0 + 1u] = 0xFFFF1000u;                         // bury the first as a plain NOP body
    const uint32_t cl = 14531u - kF71F52Start; w[cl + 1u] = 0xFFFF1000u;   // and the last
    ck("no candidate: NO-TRAILING-NOP, as find", n48_f828_find_last(w, kF71N52, &r1), n48_f828_find(w, kF71N52, &r0));
    ck("... = NO-TRAILING-NOP", r1.why, N48_F828_NO_NOP);
    std::memcpy(w, kF71F52, sizeof kF71F52);
    ck("walk ending past n: WALK, as find", n48_f828_find_last(w, kF71N52 - 1u, &r1), N48_F828_WALK);
    ck("short slice: ARG, as find", n48_f828_find_last(kF71F52, 9u, &r1), N48_F828_ARG);
    ck("null out: ARG", n48_f828_find_last(nullptr, 100u, &r1), N48_F828_ARG);
    // (11) FAIL CLOSED: two ADJACENT candidates (the first's slice is 9 dwords) cannot be field-checked -> refused, never OK
    {
        uint32_t a[28];
        for (uint32_t k = 0; k < 2u; k++) std::memcpy(&a[9u * k], &kF71F52[cl], 9u * 4u);
        a[6] = 0x1eu;
        for (uint32_t i = 18u; i < 28u; i++) a[i] = N48_F828_NOP1;
        ck("adjacent candidates: refused (ARG), never OK", n48_f828_find_last(a, 28u, &r1), N48_F828_ARG);
        a[9] = N48_F828_NOP1; for (uint32_t i = 10u; i < 18u; i++) a[i] = N48_F828_NOP1;   // control: one candidate, NOP tail
        ck("... control: the same first candidate alone with a NOP tail is OK", n48_f828_find_last(a, 28u, &r1), N48_F828_OK);
    }
    ck("reason name: TAIL-WORK", std::strcmp(n48_f828_reason_name(N48_F828_TAIL_WORK), "TAIL-WORK") == 0 ? 1u : 0u, 1u);
    ck("tail_nop: F52 after its last candidate is all NOP", n48_f828_tail_nop(kF71F52, cl + 9u, kF71N52), 1u);
    ck("tail_nop: F52 after its FIRST candidate is NOT (a DISPATCH and draws follow)", n48_f828_tail_nop(kF71F52, c0 + 9u, kF71N52), 0u);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("gfx_fence828: the one-dword un-NOP + the owned slot ()\n\n");
    run_checks();
    checks508();   // build 0.0.508 (switch 71): section 16, once, outside the mutant loop
    const int baseFail = gFail, baseRun = gRun;
    printf("\nchecks %d, failures %d\n", baseRun, baseFail);

    // Planted defects: each mutant must be caught by at least one of the SAME checks.
    printf("\nplanted defects (each must be CAUGHT by the checks above):\n");
    gQuiet = 1;
    int caught = 0;
    for (int m = 1; m < MUT_COUNT; m++) {
        gMut = m; gFail = 0; gRun = 0;
        run_checks();
        if (gFail) { caught++; printf("  %-64s CAUGHT (%d check(s) fail)\n", gMutName[m], gFail); }
        else       { printf("  %-64s NOT CAUGHT\n", gMutName[m]); }
    }
    gMut = MUT_NONE;
    printf("\nmutants caught %d/%d\n", caught, MUT_COUNT - 1);
    if (baseFail || caught != MUT_COUNT - 1) {
        printf("gfx_fence828: FAIL\n");
        return 1;
    }
    printf("gfx_fence828: N48-FENCE828-TEST-PASS\n");
    return 0;
}
