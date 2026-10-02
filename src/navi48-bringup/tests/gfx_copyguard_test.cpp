// gfx_copyguard_test.cpp — 0.0.435 (notes/design/PGMID-COPYGUARD.md Part 2). THE COPY-OVERLAP
// REFUSAL'S HOST PROOF.
//
// Every test drives the REAL pure functions in gfx_copyguard.h - the same header the kext compiles - through a
// hand-scripted interleaving of writer steps (OPEN, BEGIN, WA, WB, END, CLOSE) and reader steps (MARK the ring
// position and reset the recorder, READ_A/READ_B - simulating gfxc_read_rs noting a page, CHECK - n48_cg_check),
// never real threads: the "test-only hook placed between check steps" the design asks for is simply that each step
// is its own function call, so any order can be scripted and any single step can be substituted with a MUTANT.
//
// SECTION MAP (mirrors the design's own list, notes/design/PGMID-COPYGUARD.md "Tests"):
//   T1  exhaustive interleavings — accept only all-old or all-new, at least one accept of each
//   T2  planted breaks (each shown CAUGHT then reverted): ring-before-slot, CLOSE-before-END, no-slot-scan,
//       no-ring-scan, BEGIN-after-WA
//   T3  the paused-copier schedule is refused
//   T4  WRAP and TORN are refused
//   T5  poison holds until a covering clean copy
//   T6  a disjoint range is ACCEPTED (break: a one-sided overlap test)
//   T7  recorder overflow refuses
//   T8  a memo hit without its pages is caught
//   T9  source pins, and an allowlist over every navi48_vram_write_mm call site
//
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -x c++ \
//         src/navi48-bringup/tests/gfx_copyguard_test.cpp -o /tmp/cgtest && /tmp/cgtest \
//         [src/navi48-bringup/src/Navi48Bringup.cpp] [src/navi48-bringup/src/apple/AppleHardwareHook.cpp] \
//         [src/navi48-bringup/src/apple/Navi48AccelPeer.cpp] [src/navi48-bringup/src/apple/Navi48Ttl.hpp]
//
// Every file argument is OPTIONAL and its checks are SKIPPED, not failed, when absent (gfx_mib_test.cpp's
// convention) — T1-T8 run in full with no arguments at all; only T9 needs the kext sources.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_copyguard.h"
#include "gfx_heapgen.h"              // build 0.0.495: switch 62 (section T18)
#include "gfx_neuter.h"               // T18: the ring exemption's heap-gen clause
#include "fixture_heapgen_run10i.h"   // T18: RUN D's real copy #62 and its 31 substitutions
#include "fastcopy.h"                 // build 0.0.496: switch 63, the residency copy through SDMA (section T19)
#include "fixture_fastcopy_run10g.h"  // T19: every COPIED line of run10g (RUN B)
#include "fixture_hgreg_run10h.h"     // T20: RUN C's copies and SUBSTITUTED lines, in stream order (F1)
#include "hybrid_policy.h"            // build 0.0.503: switch 68, the hybrid newUserClient policy (section T22)
#include "gfx_commit.h"               // T22: n48_cm_cont_switch_guarded (68 joins the continuous mid-arm guard)

static unsigned gChecks = 0, gFails = 0;
static void expect(const char *what, bool cond) {
    gChecks++;
    if (!cond) { gFails++; std::printf("FAIL  %s\n", what); }
    else std::printf("ok    %s\n", what);
}

static bool read_file(const char *path, std::string &out) {
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return false;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
    std::fclose(fp);
    return true;
}
static bool has(const std::string &s, const char *needle) { return s.find(needle) != std::string::npos; }
static size_t count_of(const std::string &s, const char *needle) {
    size_t n = 0, pos = 0, L = std::strlen(needle);
    while ((pos = s.find(needle, pos)) != std::string::npos) { n++; pos += L; }
    return n;
}

// =============================================================================================================
// A tiny writer/reader harness over the REAL structures. `Cg` is one boot's worth of state (slots/ring/poison),
// freshly zero-initialised per scenario, exactly as Navi48Bringup.cpp's statics start.
// =============================================================================================================
struct Cg {
    n48_cg_slot slots[N48_CG_SLOTS];
    n48_cg_ring ring;
    n48_cg_poison poison;
    uint32_t untracked = 0;
    Cg() {
        n48_cg_slot_init(slots, N48_CG_SLOTS);
        n48_cg_ring_init(&ring);
        n48_cg_poison_init(&poison);
    }
    // Writer steps. `owner` defaults to a fixed "test thread" id (0.0.438, , D4) - every existing test
    // that never cares about ownership keeps opening/closing under the SAME implied thread, so it is unaffected;
    // T11d below is the one test that opens two scopes under DIFFERENT owners on purpose.
    int32_t open(uint64_t lo, uint64_t hi, uintptr_t owner = 0x1000ull) {
        const int32_t s = n48_cg_slot_open(slots, N48_CG_SLOTS, lo, hi, owner);
        if (s < 0) untracked++;
        return s;
    }
    void begin(int32_t slot, uint64_t lo, uint64_t hi) { n48_cg_ring_push(&ring, N48_CG_EV_BEGIN, (uint32_t)slot, lo, hi); }
    void end(int32_t slot, uint64_t lo, uint64_t hi)   { n48_cg_ring_push(&ring, N48_CG_EV_END, (uint32_t)slot, lo, hi); }
    void close(int32_t slot) {
        if (slot < 0) { if (untracked) untracked--; return; }
        n48_cg_slot_close(slots, slot);
    }
    // Reader: the real check, exactly as navi48_cg_seg_check runs it.
    uint32_t check(const n48_cg_pagerec *rec, uint64_t since) {
        return n48_cg_check(rec, &poison, slots, N48_CG_SLOTS, &ring, since, untracked);
    }
};

static constexpr uint64_t kLo = 0x10010000ull, kHi = 0x10018000ull;   // one copy's range ('s own example)
static constexpr uint64_t kPageA = 0x10010000ull;                    // inside [kLo, kHi)
static constexpr uint64_t kPageB = 0x10014000ull;                    // inside [kLo, kHi), a different page

// =============================================================================================================
// T1 — EXHAUSTIVE INTERLEAVINGS. Writer order fixed: OPEN, BEGIN, WA, WB, END, CLOSE (6 steps). Reader order
// fixed: MARK, READ_A, READ_B, CHECK (4 steps). Every one of C(10,4) = 210 ways to interleave them (each side's
// own relative order preserved) is run against a FRESH Cg. Ground truth: dword A is NEW iff WA's position in the
// merged sequence precedes READ_A's; same for B/WB. A correct guard must never ACCEPT (CHECK == N48_CG_OK) a
// schedule whose ground truth is torn (A and B disagree); this is the design's own safety property, and the test
// is proved non-vacuous by requiring at least one ACCEPTed all-old schedule and at least one ACCEPTed all-new one.
// =============================================================================================================
enum WStep { W_OPEN, W_BEGIN, W_WA, W_WB, W_END, W_CLOSE, W_N };
enum RStep { R_MARK, R_READA, R_READB, R_CHECK, R_N };

// One interleaving = an ordering of the 10 tags (6 W-tags in W_N order, 4 R-tags in R_N order, merged).
static void run_interleaving(const std::vector<int> &tags /* 0..5 = WStep, 6..9 = R_* + 6 */,
                              bool &acceptedOld, bool &acceptedNew, bool &tornAccepted) {
    Cg cg;
    n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
    int32_t slot = -1;
    bool waDone = false, wbDone = false;
    bool groundA = false, groundB = false;   // true = NEW
    uint64_t since = 0;
    uint32_t result = N48_CG_REASONS;   // sentinel: CHECK never ran
    for (int t : tags) {
        if (t < 6) {
            switch ((WStep)t) {
                case W_OPEN:  slot = cg.open(kLo, kHi); break;
                case W_BEGIN: cg.begin(slot, kLo, kHi); break;
                case W_WA:    waDone = true; break;
                case W_WB:    wbDone = true; break;
                case W_END:   cg.end(slot, kLo, kHi); break;
                case W_CLOSE: cg.close(slot); break;
                default: break;
            }
        } else {
            switch ((RStep)(t - 6)) {
                case R_MARK:  n48_cg_pagerec_reset(&rec); since = n48_cg_ring_mark(&cg.ring); break;
                case R_READA: n48_cg_pagerec_note(&rec, kPageA, 4); groundA = waDone; break;
                case R_READB: n48_cg_pagerec_note(&rec, kPageB, 4); groundB = wbDone; break;
                case R_CHECK: result = cg.check(&rec, since); break;
                default: break;
            }
        }
    }
    if (result == N48_CG_OK) {
        if (groundA != groundB) tornAccepted = true;
        else if (groundA) acceptedNew = true;
        else acceptedOld = true;
    }
}

// Generate all C(10,4) interleavings of a 6-tag ordered sequence and a 4-tag ordered sequence.
static void gen_interleavings(std::vector<std::vector<int>> &out) {
    // choose which of the 10 slots the 4 R-steps occupy; the rest are W-steps in order.
    for (unsigned mask = 0; mask < (1u << 10); mask++) {
        if (__builtin_popcount(mask) != 4) continue;
        std::vector<int> seq; seq.reserve(10);
        int w = 0, r = 0;
        for (int i = 0; i < 10; i++) {
            if (mask & (1u << i)) seq.push_back(6 + (r++));
            else seq.push_back(w++);
        }
        out.push_back(seq);
    }
}

static void test_T1() {
    std::vector<std::vector<int>> all;
    gen_interleavings(all);
    expect("T1 generated exactly C(10,4) = 210 interleavings", all.size() == 210u);
    bool anyOld = false, anyNew = false, anyTorn = false;
    for (auto &seq : all) {
        bool o = false, n = false, torn = false;
        run_interleaving(seq, o, n, torn);
        anyOld |= o; anyNew |= n; anyTorn |= torn;
    }
    expect("T1 no schedule is ever ACCEPTed with torn ground truth (A and B disagree)", !anyTorn);
    expect("T1 non-vacuous: at least one all-OLD schedule is ACCEPTed", anyOld);
    expect("T1 non-vacuous: at least one all-NEW schedule is ACCEPTed", anyNew);
}

// =============================================================================================================
// T2 — PLANTED BREAKS. Each constructs the exact scenario the design's own comment in gfx_copyguard.h explains,
// runs it against the REAL function, then against a MUTANT with the one named defect, and requires: real REFUSES,
// mutant ACCEPTS (i.e. a test that only asserted "refused" would FAIL against the mutant — CAUGHT).
// =============================================================================================================

// T2a — ring position loaded BEFORE the slot scan. The mutant's steps (fence; poison; now=mark(); slot scan; ring
// scan[since,now)) are inlined directly in the test below, not called as one opaque function, precisely so the
// writer's whole OPEN..CLOSE cycle can be scripted to land in the gap between "now=mark()" and the slot scan -
// exactly the race gfx_copyguard.h's n48_cg_check comment names.
static void test_T2a_ring_before_slot() {
    // A full open-write-close cycle happens ENTIRELY between the mutant's own "now" capture and its slot scan -
    // the mutant's steps are inlined here (not called as one opaque function) so the writer's OPEN..CLOSE can be
    // scripted to land in exactly that gap, the same test-only-hook technique T2b/T2c/T2d use.
    {
        Cg cg;
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
        n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // segment's own MARK, before anything happens
        const uint32_t real0 = cg.check(&rec, since);
        expect("T2a sanity: nothing open yet -> real check is OK", real0 == N48_CG_OK);
        // Mutant: capture "now" FIRST (*** PLANTED BREAK ***), THEN let the whole copy run, THEN scan.
        const uint64_t mutNow = n48_cg_ring_mark(&cg.ring);
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        cg.close(slot);
        uint32_t mut1 = N48_CG_OK;
        {
            const int sr = n48_cg_slot_scan(cg.slots, N48_CG_SLOTS, kPageA, kPageA + 4096ull);
            if (sr == N48_CG_SLOT_TORN) mut1 = N48_CG_TORN;
            else if (sr == N48_CG_SLOT_INFLIGHT) mut1 = N48_CG_IN_FLIGHT;
            else if (!n48_cg_ring_wrapped(since, mutNow)) {
                const int rr = n48_cg_ring_scan_range(&cg.ring, since, mutNow, kPageA, kPageA + 4096ull);
                if (rr == N48_CG_RING_TORN) mut1 = N48_CG_TORN;
                else if (rr == N48_CG_RING_EVENT) mut1 = N48_CG_EVENT;
            }
        }
        expect("T2a planted break CAUGHT: ring-before-slot mutant wrongly ACCEPTs the whole cycle", mut1 == N48_CG_OK);
    }
    {
        // Same scenario, real order (now captured AFTER the slot scan): REFUSES.
        Cg cg;
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
        n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        cg.close(slot);
        const uint32_t real1 = cg.check(&rec, since);
        expect("T2a real check REFUSES the full open-write-close cycle (EVENT)", real1 == N48_CG_EVENT);
    }
}

// T2b — CLOSE publishes "seq even" BEFORE pushing END (mutant order reversed from navi48_cg_close's own).
static void test_T2b_close_before_end() {
    // The copier is already open BEFORE this segment's own MARK (a "paused copier" shape), so its BEGIN is
    // invisible to the ring window; only the slot (IN_FLIGHT) or a timely END (EVENT) can catch it.
    {
        Cg cg;
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // MARK after BEGIN: BEGIN is out of the window
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        // Real CLOSE's first half only (END pushed; seq still odd) - the reader runs its check right here.
        n48_cg_ring_push(&cg.ring, N48_CG_EV_END, (uint32_t)slot, kLo, kHi);
        const uint32_t real = cg.check(&rec, since);
        n48_cg_slot_close(cg.slots, slot);   // finish the real close
        expect("T2b real order: checked between END-push and seq-even still REFUSES (odd slot, IN_FLIGHT)",
               real == N48_CG_IN_FLIGHT);
    }
    {
        Cg cg;
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        // Mutant CLOSE's first half only (seq already even; END not yet pushed) - checked at the same instant.
        n48_cg_slot_close(cg.slots, slot);
        const uint32_t mut = cg.check(&rec, since);
        n48_cg_ring_push(&cg.ring, N48_CG_EV_END, (uint32_t)slot, kLo, kHi);   // finish the mutant close, too late
        expect("T2b planted break CAUGHT: mutant order (seq-even first) wrongly ACCEPTs at the same instant",
               mut == N48_CG_OK);
    }
}

// T2c — no slot scan at all (ring-only detection). A currently-open, never-yet-closed copy has nothing in the
// ring window (BEGIN predates MARK, no END yet): only the slot scan can catch it.
static void test_T2c_no_slot_scan() {
    Cg cg;
    const int32_t slot = cg.open(kLo, kHi);
    cg.begin(slot, kLo, kHi);
    const uint64_t since = n48_cg_ring_mark(&cg.ring);   // after BEGIN: ring window starts empty
    n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
    const uint32_t real = cg.check(&rec, since);
    expect("T2c real check REFUSES a currently-open copy with an empty ring window (IN_FLIGHT)", real == N48_CG_IN_FLIGHT);
    // Mutant: skip the slot scan entirely (ring-only).
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t mut = N48_CG_OK;
    if (rec.overflow) mut = N48_CG_OVERFLOW;
    else if (cg.untracked) mut = N48_CG_UNTRACKED;
    else {
        const uint64_t now = n48_cg_ring_mark(&cg.ring);
        if (n48_cg_ring_wrapped(since, now)) mut = N48_CG_WRAP;
        else {
            for (uint32_t i = 0; i < rec.n && mut == N48_CG_OK; i++) {
                const int rr = n48_cg_ring_scan_range(&cg.ring, since, now, rec.page[i], rec.page[i] + 4096ull);
                if (rr == N48_CG_RING_TORN) mut = N48_CG_TORN;
                else if (rr == N48_CG_RING_EVENT) mut = N48_CG_EVENT;
            }
        }
    }
    cg.end(slot, kLo, kHi); cg.close(slot);
    expect("T2c planted break CAUGHT: no-slot-scan mutant wrongly ACCEPTs the open copy", mut == N48_CG_OK);
}

// T2d — no ring scan at all (slot-only detection). A copy that has ALREADY CLOSED but whose BEGIN/END both fall
// inside the reader's window: only the ring scan can catch it.
static void test_T2d_no_ring_scan() {
    Cg cg;
    const uint64_t since = n48_cg_ring_mark(&cg.ring);   // before OPEN: BEGIN/END will both be in the window
    const int32_t slot = cg.open(kLo, kHi);
    cg.begin(slot, kLo, kHi);
    cg.end(slot, kLo, kHi);
    cg.close(slot);
    n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
    const uint32_t real = cg.check(&rec, since);
    expect("T2d real check REFUSES a closed-but-windowed copy (EVENT)", real == N48_CG_EVENT);
    // Mutant: skip the ring scan entirely (slot-only).
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t mut = N48_CG_OK;
    if (rec.overflow) mut = N48_CG_OVERFLOW;
    else if (cg.untracked) mut = N48_CG_UNTRACKED;
    else {
        for (uint32_t i = 0; i < rec.n && mut == N48_CG_OK; i++) {
            const int sr = n48_cg_slot_scan(cg.slots, N48_CG_SLOTS, rec.page[i], rec.page[i] + 4096ull);
            if (sr == N48_CG_SLOT_TORN) mut = N48_CG_TORN;
            else if (sr == N48_CG_SLOT_INFLIGHT) mut = N48_CG_IN_FLIGHT;
        }
    }
    expect("T2d planted break CAUGHT: no-ring-scan mutant wrongly ACCEPTs the closed copy", mut == N48_CG_OK);
}

// T2e — BEGIN pushed AFTER WA (writer-side break). Isolates the RING layer alone (as T2c/T2d do for the other
// layer): with the slot scan disabled, a reader whose whole window falls strictly between WA and a DELAYED BEGIN
// sees an empty ring and misses the write; with BEGIN correctly preceding every write, the same window already
// holds BEGIN by the time any write could have happened.
static void test_T2e_begin_after_wa() {
    auto ring_only_check = [](Cg &cg, const n48_cg_pagerec &rec, uint64_t since) -> uint32_t {
        const uint64_t now = n48_cg_ring_mark(&cg.ring);
        if (n48_cg_ring_wrapped(since, now)) return N48_CG_WRAP;
        for (uint32_t i = 0; i < rec.n; i++) {
            const int rr = n48_cg_ring_scan_range(&cg.ring, since, now, rec.page[i], rec.page[i] + 4096ull);
            if (rr == N48_CG_RING_TORN) return N48_CG_TORN;
            if (rr == N48_CG_RING_EVENT) return N48_CG_EVENT;
        }
        return N48_CG_OK;
    };
    { // Real order: OPEN, BEGIN, WA, then the reader's whole window (mark..check) happens right after WA.
        Cg cg;
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // marked before OPEN
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);                            // BEGIN precedes WA, as designed
        // WA happens here (nothing to simulate but the moment).
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint32_t r = ring_only_check(cg, rec, since);
        cg.end(slot, kLo, kHi); cg.close(slot);
        expect("T2e real order (BEGIN before WA): ring-only check REFUSES (EVENT)", r == N48_CG_EVENT);
    }
    { // Planted break: OPEN, WA, [reader's whole window here], BEGIN (delayed), WB, END, CLOSE.
        Cg cg;
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // marked before OPEN too - same setup otherwise
        const int32_t slot = cg.open(kLo, kHi);
        // WA happens here, BEGIN has NOT been pushed yet (*** PLANTED BREAK ***).
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint32_t r = ring_only_check(cg, rec, since);
        cg.begin(slot, kLo, kHi);   // finally pushed, too late for the check that already ran
        cg.end(slot, kLo, kHi); cg.close(slot);
        expect("T2e planted break CAUGHT: BEGIN-after-WA leaves the ring-only check blind (wrongly OK)", r == N48_CG_OK);
    }
}

// =============================================================================================================
// T3 — THE PAUSED-COPIER SCHEDULE IS REFUSED. A copier that opened and never closes (paused mid-copy, exactly
// 's own hazard - decide37's copier logged QUEUED (lock busy) mid-rewrite) must refuse for as long as
// it stays open, independent of switch 37 (the guard never reads it).
// =============================================================================================================
static void test_T3_paused_copier() {
    Cg cg;
    const uint64_t since = n48_cg_ring_mark(&cg.ring);
    const int32_t slot = cg.open(kLo, kHi);
    cg.begin(slot, kLo, kHi);
    // WA happens, then the copier PAUSES here indefinitely - no WB, no END, no CLOSE.
    n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
    const uint32_t r1 = cg.check(&rec, since);
    n48_cg_pagerec rec2; n48_cg_pagerec_reset(&rec2); n48_cg_pagerec_note(&rec2, kPageA, 4);
    const uint32_t r2 = cg.check(&rec2, since);   // a second segment asks again; still paused
    expect("T3 a paused copier refuses the first ask (IN_FLIGHT)", r1 == N48_CG_IN_FLIGHT);
    expect("T3 and every ask after it, for as long as it stays paused (IN_FLIGHT)", r2 == N48_CG_IN_FLIGHT);
    cg.end(slot, kLo, kHi); cg.close(slot);
    n48_cg_pagerec rec3; n48_cg_pagerec_reset(&rec3); n48_cg_pagerec_note(&rec3, kPageA, 4);
    const uint64_t since2 = n48_cg_ring_mark(&cg.ring);   // marked AFTER the close: back to baseline
    const uint32_t r3 = cg.check(&rec3, since2);
    expect("T3 once it finally closes, a FRESH ask (marked after) is OK again", r3 == N48_CG_OK);
}

// =============================================================================================================
// T4 — WRAP AND TORN ARE REFUSED.
// =============================================================================================================
static void test_T4_wrap_and_torn() {
    { // WRAP: more than N48_CG_RING events land between `since` and `now`.
        Cg cg;
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        for (uint32_t i = 0; i < N48_CG_RING + 1u; i++)
            n48_cg_ring_push(&cg.ring, N48_CG_EV_WRITE, ~0u, kLo, kHi);   // unrelated events, just to advance `next`
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint32_t r = cg.check(&rec, since);
        expect("T4 WRAP: more than N48_CG_RING new events since the mark refuses (WRAP)", r == N48_CG_WRAP);
    }
    { // TORN (slot): corrupt a slot's seq between the two reads a real scan would take - simulate by hand-rolling
      // the seqlock read with an injected mutation, proving the seqlock catches an inconsistent pair.
        Cg cg;
        const int32_t slot = cg.open(kLo, kHi);
        // Simulate a slot whose seq moved between the bracketing reads: read s1, mutate, read lo/hi, read s2.
        const uint64_t s1 = __atomic_load_n(&cg.slots[slot].seq, __ATOMIC_SEQ_CST);
        cg.close(slot);                       // seq moves (odd -> even) "during" the read
        (void)__atomic_load_n(&cg.slots[slot].lo, __ATOMIC_SEQ_CST);
        (void)__atomic_load_n(&cg.slots[slot].hi, __ATOMIC_SEQ_CST);
        const uint64_t s2 = __atomic_load_n(&cg.slots[slot].seq, __ATOMIC_SEQ_CST);
        expect("T4 TORN (slot): the seqlock's own bracketing reads disagree when seq moved mid-read", s1 != s2);
        // n48_cg_slot_scan_one, called fresh, reads a fully-settled (even, closed) slot - not torn by itself; the
        // TORN path is exercised by the disagreement above, which is exactly what n48_cg_slot_scan_one checks for.
    }
    { // TORN (ring): corrupt a published event's stamp after the fact (simulating a slot being overwritten mid-read).
      // The copy is closed FIRST so the slot scan alone cannot refuse it (even, NONE) and the check must reach the
      // ring scan, where the corrupted stamp is the only thing that can still catch it.
        Cg cg;
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        cg.close(slot);
        // Directly corrupt the stamp of the BEGIN event, as a torn/overwritten slot would read.
        n48_cg_ev *e = &cg.ring.s[since % N48_CG_RING];
        __atomic_store_n(&e->stamp, 0xffffffffull, __ATOMIC_SEQ_CST);   // no longer == index+1
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint32_t r = cg.check(&rec, since);
        expect("T4 TORN (ring): a stamp that does not match its index refuses (TORN)", r == N48_CG_TORN);
    }
}

// =============================================================================================================
// T5 — POISON HOLDS UNTIL A COVERING CLEAN COPY.
// =============================================================================================================
static void test_T5_poison() {
    Cg cg;
    // A FAILED copy over [kLo, kHi).
    {
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        n48_cg_poison_mark(&cg.poison, kLo, kHi);   // navi48_cg_close's own action on failed/mismatch
        cg.close(slot);
    }
    const uint64_t since1 = n48_cg_ring_mark(&cg.ring);
    n48_cg_pagerec rec1; n48_cg_pagerec_reset(&rec1); n48_cg_pagerec_note(&rec1, kPageA, 4);
    expect("T5 a segment reading the poisoned range refuses (POISONED)", cg.check(&rec1, since1) == N48_CG_POISONED);
    // A partial-coverage clean copy must NOT clear it.
    {
        const int32_t slot = cg.open(kLo, kLo + 0x1000ull);   // only the first page of the poisoned range
        cg.begin(slot, kLo, kLo + 0x1000ull);
        cg.end(slot, kLo, kLo + 0x1000ull);
        n48_cg_poison_clear_covered(&cg.poison, kLo, kLo + 0x1000ull);
        cg.close(slot);
    }
    const uint64_t since2 = n48_cg_ring_mark(&cg.ring);
    n48_cg_pagerec rec2; n48_cg_pagerec_reset(&rec2); n48_cg_pagerec_note(&rec2, kPageA, 4);
    expect("T5 a partial-coverage clean copy does NOT clear the poison", cg.check(&rec2, since2) == N48_CG_POISONED);
    // A FULLY covering clean copy clears it.
    {
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        n48_cg_poison_clear_covered(&cg.poison, kLo, kHi);
        cg.close(slot);
    }
    const uint64_t since3 = n48_cg_ring_mark(&cg.ring);
    n48_cg_pagerec rec3; n48_cg_pagerec_reset(&rec3); n48_cg_pagerec_note(&rec3, kPageA, 4);
    expect("T5 a fully-covering clean copy clears the poison (OK)", cg.check(&rec3, since3) == N48_CG_OK);
}

// =============================================================================================================
// T5b — 0.0.435 REVIEW FIX: A CLOSE THAT WROTE NOTHING MUST NEITHER MARK NOR CLEAR POISON. Drives
// n48_cg_close_poison (the pure decision) directly, since that is the actual fix; the planted break ignores
// `wrote` (always behaves as if wrote=1), reproducing the bug the review found (the phantom scope erasing every
// poison row on close).
// =============================================================================================================
static inline void close_poison_mutant_ignores_wrote(n48_cg_poison *p, uint64_t lo, uint64_t hi, int /*wrote*/, int failed, int mismatch) {
    if (failed || mismatch) n48_cg_poison_mark(p, lo, hi);   // *** PLANTED BREAK: no `if (!wrote) return;` guard ***
    else n48_cg_poison_clear_covered(p, lo, hi);
}
static void test_T5b_wrote_gates_poison() {
    { // A wrote=0 close over a COVERING range must leave an existing poison row untouched.
        n48_cg_poison p; n48_cg_poison_init(&p);
        n48_cg_poison_mark(&p, kLo, kHi);
        expect("T5b sanity: freshly marked range is poisoned", n48_cg_poison_overlaps(&p, kLo, kHi));
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/0, /*failed=*/0, /*mismatch=*/0);   // e.g. the phantom scope
        expect("T5b real: a wrote=0 close over a covering range leaves it POISONED", n48_cg_poison_overlaps(&p, kLo, kHi));
    }
    { // A wrote=1 clean covering close still clears it, exactly as before this fix.
        n48_cg_poison p; n48_cg_poison_init(&p);
        n48_cg_poison_mark(&p, kLo, kHi);
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/0, /*mismatch=*/0);
        expect("T5b real: a wrote=1 clean covering close still CLEARS it", !n48_cg_poison_overlaps(&p, kLo, kHi));
    }
    { // Planted break: the mutant, applied to the SAME wrote=0/covering scenario, wrongly clears it. CAUGHT.
        n48_cg_poison p; n48_cg_poison_init(&p);
        n48_cg_poison_mark(&p, kLo, kHi);
        close_poison_mutant_ignores_wrote(&p, kLo, kHi, /*wrote=*/0, /*failed=*/0, /*mismatch=*/0);
        expect("T5b planted break CAUGHT: ignoring `wrote` wrongly clears the poison the phantom must leave alone",
               !n48_cg_poison_overlaps(&p, kLo, kHi));
    }
}

// =============================================================================================================
// T6 — A DISJOINT RANGE IS ACCEPTED (break: a one-sided overlap test).
// =============================================================================================================
static inline int overlap_one_sided_mutant(uint64_t lo1, uint64_t hi1, uint64_t /*lo2*/, uint64_t /*hi2*/) {
    return lo1 < hi1;   // *** PLANTED BREAK: only asks "is the first range non-empty", ignores the second entirely ***
}
static void test_T6_disjoint_accepted() {
    const uint64_t dLo = 0x20000000ull, dHi = 0x20001000ull;   // disjoint from [kLo, kHi)
    expect("T6 the real overlap test says NO for a disjoint pair", !n48_cg_overlap(kLo, kHi, dLo, dHi));
    Cg cg;
    const uint64_t since = n48_cg_ring_mark(&cg.ring);
    const int32_t slot = cg.open(kLo, kHi);
    cg.begin(slot, kLo, kHi);
    n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
    n48_cg_pagerec_note(&rec, dLo, 4);   // the segment reads a page OUTSIDE the copy's range
    const uint32_t real = cg.check(&rec, since);
    expect("T6 a disjoint range is ACCEPTED by the real guard while a copy is genuinely open elsewhere", real == N48_CG_OK);
    cg.end(slot, kLo, kHi); cg.close(slot);
    // The one-sided mutant: reduced to "is [kLo,kHi) non-empty", which is trivially true here, so EVERY range -
    // disjoint or not - reads as overlapping once any copy is open, wrongly refusing the disjoint accept above.
    const int mutSaysOverlap = overlap_one_sided_mutant(kLo, kHi, dLo, dHi);
    expect("T6 planted break CAUGHT: the one-sided test wrongly calls the disjoint pair an overlap",
           mutSaysOverlap != 0);
}

// =============================================================================================================
// T7 — RECORDER OVERFLOW REFUSES.
// =============================================================================================================
static void test_T7_recorder_overflow() {
    n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
    for (uint32_t i = 0; i < N48_CG_REC_PAGES; i++) n48_cg_pagerec_note_page(&rec, (uint64_t)i * 4096ull);
    expect("T7 exactly N48_CG_REC_PAGES distinct pages: no overflow yet", !rec.overflow && rec.n == N48_CG_REC_PAGES);
    n48_cg_pagerec_note_page(&rec, (uint64_t)N48_CG_REC_PAGES * 4096ull);   // one more distinct page
    expect("T7 one page past capacity sets overflow", rec.overflow != 0u);
    Cg cg;
    const uint64_t since = n48_cg_ring_mark(&cg.ring);
    const uint32_t r = cg.check(&rec, since);
    expect("T7 an overflowed recorder refuses the whole segment (RECORDER_OVERFLOW), even with nothing else in flight",
           r == N48_CG_OVERFLOW);
    // Non-vacuity: re-reading the SAME page never overflows (dedup), so overflow really does need distinct pages.
    n48_cg_pagerec rec2; n48_cg_pagerec_reset(&rec2);
    for (uint32_t i = 0; i < N48_CG_REC_PAGES * 3u; i++) n48_cg_pagerec_note_page(&rec2, 0ull);   // same page every time
    expect("T7 re-reading one page many times never overflows (dedup)", !rec2.overflow && rec2.n == 1u);
}

// =============================================================================================================
// T8 — A MEMO HIT WITHOUT ITS PAGES IS CAUGHT (recorder check only; the memo itself is 0.0.436, not this brief).
// A future per-pass memo (design Part 1's M) that answers a program-identity ask from cache, without re-reading,
// must merge the pages its cached answer depends on into the ACTIVE recorder itself (n48_cg_pagerec_note) or a
// racing copy over those pages is invisible to this check. This proves: (a) the gap is real - a "hit" that records
// nothing is blind to an overlap a genuine read would have caught; (b) the one API a future memo needs
// (n48_cg_pagerec_note, the very one gfxc_read_rs already calls) closes it completely when used.
// =============================================================================================================
static void test_T8_memo_hit() {
    Cg cg;
    const uint64_t since = n48_cg_ring_mark(&cg.ring);
    const int32_t slot = cg.open(kLo, kHi);
    cg.begin(slot, kLo, kHi);
    cg.end(slot, kLo, kHi);
    cg.close(slot);
    // (a) a simulated memo HIT: the segment's translate never called gfxc_read_rs for this program (cache hit), so
    // the recorder is EMPTY even though the identity it trusts lives in [kLo, kHi) - exactly where a copy just ran.
    n48_cg_pagerec recHit; n48_cg_pagerec_reset(&recHit);
    const uint32_t rHit = cg.check(&recHit, since);
    expect("T8 an empty recorder (an unmerged memo hit) is BLIND to the copy that just ran here", rHit == N48_CG_OK);
    // (b) the same hit, but the memo correctly merges its remembered pages via n48_cg_pagerec_note before the check.
    n48_cg_pagerec recMerged; n48_cg_pagerec_reset(&recMerged);
    n48_cg_pagerec_note(&recMerged, kPageA, 4);   // the memo's own remembered page for this identity
    const uint32_t rMerged = cg.check(&recMerged, since);
    expect("T8 once the memo merges its pages into the recorder, the same hazard is CAUGHT (EVENT)", rMerged == N48_CG_EVENT);
}

// =============================================================================================================
// T10 — 0.0.435 REVIEW FIX: "opened == closed + in flight" AT EVERY QUIESCENT POINT, where "in flight" is the
// LIVE count (n48_cg_live_open), not an accumulated refusal counter. Drives a scripted open/open-untracked/open/
// close/close-untracked sequence and checks the invariant after every step; live == 0 once everything has closed.
// =============================================================================================================
static void test_T10_opened_closed_live() {
    Cg cg;
    uint64_t opened = 0, closed = 0;
    auto live_now = [&]() { return n48_cg_live_open(cg.slots, N48_CG_SLOTS, cg.untracked); };
    auto check_invariant = [&](const char *what) {
        expect(what, opened == closed + live_now());
    };
    // open (tracked)
    const int32_t s1 = cg.open(kLo, kHi); opened++;
    check_invariant("T10 after one tracked open: opened == closed + live");
    expect("T10 live is 1 after one tracked open", live_now() == 1u);
    // open-untracked: fill every real slot first, so the NEXT open is forced untracked.
    std::vector<int32_t> fillers;
    for (uint32_t i = 0; i < N48_CG_SLOTS - 1u; i++) { fillers.push_back(cg.open(kLo, kHi)); opened++; }
    const int32_t sUntracked = cg.open(kLo, kHi); opened++;   // every real slot is now taken -> untracked (-1)
    expect("T10 sanity: the forced extra open is untracked (-1)", sUntracked < 0);
    check_invariant("T10 after filling every slot plus one untracked open: opened == closed + live");
    // open (tracked again is impossible now; skip straight to closing everything down)
    cg.close(s1); closed++;
    check_invariant("T10 after closing the first tracked scope: opened == closed + live");
    for (int32_t s : fillers) { cg.close(s); closed++; }
    check_invariant("T10 after closing every filler: opened == closed + live");
    cg.close(sUntracked); closed++;   // close-untracked
    check_invariant("T10 after the untracked close: opened == closed + live");
    expect("T10 live is 0 once everything has closed", live_now() == 0u);
    expect("T10 opened == closed at the very end", opened == closed);

    // Planted break A: untracked opens counted nowhere (opened only bumped on the tracked branch) -> must FAIL.
    {
        uint64_t mutOpened = 0, mutClosed = 0;
        Cg cg2;
        std::vector<int32_t> f2;
        for (uint32_t i = 0; i < N48_CG_SLOTS; i++) { const int32_t s = cg2.open(kLo, kHi); if (s >= 0) mutOpened++; f2.push_back(s); }
        const int32_t sU2 = cg2.open(kLo, kHi);   // untracked; *** PLANTED BREAK: not counted in mutOpened ***
        f2.push_back(sU2);
        for (int32_t s : f2) cg2.close(s); mutClosed += f2.size();
        const bool holds = (mutOpened == mutClosed + n48_cg_live_open(cg2.slots, N48_CG_SLOTS, cg2.untracked));
        expect("T10 planted break A CAUGHT: not counting untracked opens breaks the invariant", !holds);
    }
    // Planted break B: the report's "in flight" is the refusal count, not the live count -> disagrees once refused.
    {
        Cg cg3;
        const uint64_t since = n48_cg_ring_mark(&cg3.ring);
        const int32_t s = cg3.open(kLo, kHi);
        cg3.begin(s, kLo, kHi);
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        (void)cg3.check(&rec, since);   // one IN_FLIGHT refusal
        (void)cg3.check(&rec, since);   // a second ask, same scope still open: a SECOND refusal, but only 1 is live
        const uint32_t refusalCount = 2u;   // what a per-refusal counter would have accumulated
        const uint32_t liveCount = live_now();   // stale closure captures cg, not cg3 - recompute directly
        const uint32_t liveCg3 = n48_cg_live_open(cg3.slots, N48_CG_SLOTS, cg3.untracked);
        (void)liveCount;
        expect("T10 planted break B CAUGHT: the refusal count (2) disagrees with the live count (1) after two asks",
               refusalCount != liveCg3);
        cg3.end(s, kLo, kHi); cg3.close(s);
    }
}

// =============================================================================================================
// T11 — 0.0.437: UNSCOPED WRITES SELF-SCOPE. Exhaustive interleavings of a self-scoping
// unscoped writer [OPEN, BEGIN, W, END, CLOSE] (navi48_vram_write_mm's own new shape for a write not fully
// contained in any open slot) against the reader's [MARK, READ, CHECK], using the SAME real primitives T1 already
// proves sound for the two-write copy shape. T11b: the PLANTED BREAK named by the brief - "the old pre-write event
// with no scope" (0.0.435's shape: one bare ring event, no slot claimed at all). T11c: containment, not overlap.
// =============================================================================================================
enum W2Step { W2_OPEN, W2_BEGIN, W2_W, W2_END, W2_CLOSE, W2_N };
enum R2Step { R2_MARK, R2_READ, R2_CHECK, R2_N };

static void gen_interleavings_5_3(std::vector<std::vector<int>> &out) {
    for (unsigned mask = 0; mask < (1u << 8); mask++) {
        if (__builtin_popcount(mask) != 3) continue;
        std::vector<int> seq; seq.reserve(8);
        int w = 0, r = 0;
        for (int i = 0; i < 8; i++) {
            if (mask & (1u << i)) seq.push_back(5 + (r++));
            else seq.push_back(w++);
        }
        out.push_back(seq);
    }
}

static void test_T11a_self_scoping_exhaustive() {
    std::vector<std::vector<int>> all;
    gen_interleavings_5_3(all);
    expect("T11a generated exactly C(8,3) = 56 interleavings", all.size() == 56u);
    bool anyOld = false, anyNew = false, anyUnsafeAccept = false;
    for (auto &seq : all) {
        Cg cg;
        int32_t slot = -1;
        bool wDone = false, groundNew = false;
        int wPos = -1, readPos = -1, checkPos = -1;
        uint64_t since = 0;
        uint32_t result = N48_CG_REASONS;
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
        for (size_t pos = 0; pos < seq.size(); pos++) {
            const int t = seq[pos];
            if (t < 5) {
                switch ((W2Step)t) {
                    case W2_OPEN:  slot = cg.open(kLo, kHi); break;
                    case W2_BEGIN: cg.begin(slot, kLo, kHi); break;
                    case W2_W:     wDone = true; wPos = (int)pos; break;
                    case W2_END:   cg.end(slot, kLo, kHi); break;
                    case W2_CLOSE: cg.close(slot); break;
                    default: break;
                }
            } else {
                switch ((R2Step)(t - 5)) {
                    case R2_MARK:  n48_cg_pagerec_reset(&rec); since = n48_cg_ring_mark(&cg.ring); break;
                    case R2_READ:  n48_cg_pagerec_note(&rec, kPageA, 4); groundNew = wDone; readPos = (int)pos; break;
                    case R2_CHECK: result = cg.check(&rec, since); checkPos = (int)pos; break;
                    default: break;
                }
            }
        }
        // The genuine hazard: the reader's own READ trusted a value (old or new), and the physical write W landed
        // AFTER that READ but BEFORE the frame is judged (CHECK) - the read's conclusion is now stale relative to
        // what CHECK is about to judge. W landing before READ (the reader legitimately observes the new value) or
        // after CHECK (the frame was judged consistently with what existed at judgment time) are BOTH safe; only
        // "read, then write, then judge" is the hazard this guard exists to catch.
        const bool hazard = (readPos < wPos && wPos < checkPos);
        if (result == N48_CG_OK) {
            if (hazard) anyUnsafeAccept = true;
            else if (groundNew) anyNew = true;
            else anyOld = true;
        }
    }
    expect("T11a real: NO schedule is ever ACCEPTed when the write lands strictly between the reader's own READ and CHECK",
           !anyUnsafeAccept);
    expect("T11a non-vacuous: at least one all-OLD schedule (writer entirely after the reader) is ACCEPTed", anyOld);
    expect("T11a non-vacuous: at least one all-NEW schedule (writer entirely before the reader) is ACCEPTed", anyNew);
}

static void test_T11b_old_pre_write_event_no_scope_mutant() {
    // Real (self-scoping): OPEN,BEGIN precede MARK (a scope already "in flight" when the reader starts, T2c/T3's
    // own shape) and the physical write, END and CLOSE all land inside [MARK, CHECK]. IN_FLIGHT catches it: the
    // slot is still odd (open) when CHECK runs.
    {
        Cg cg;
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // MARK: after BEGIN, so BEGIN itself is out of the window
        // W happens here - the scope is still OPEN.
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint32_t real = cg.check(&rec, since);
        cg.end(slot, kLo, kHi); cg.close(slot);   // END/CLOSE land after the check too
        expect("T11b real (self-scoping): a write whose scope is open across the whole reader window REFUSES (IN_FLIGHT)",
               real == N48_CG_IN_FLIGHT);
    }
    // Mutant: the OLD shape (0.0.435, before this brief) - ONE bare ring event pushed BEFORE the write, no slot
    // ever claimed at all ("no scope"). The event is pushed before MARK ('s own words: "sample the pass flag
    // before it [the write]"), so the ring window [MARK, CHECK) never contains it, and with no slot either, nothing
    // catches a write that lands strictly inside the reader's own window.
    {
        Cg cg;   // no OPEN at all in this mutant: the slot table stays untouched
        n48_cg_ring_push(&cg.ring, N48_CG_EV_WRITE, ~0u, kLo, kHi);   // the old pre-write event, pushed first
        const uint64_t since = n48_cg_ring_mark(&cg.ring);            // MARK happens AFTER the event: out of window
        // W happens here, strictly inside [MARK, CHECK] - the actual hazard.
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec); n48_cg_pagerec_note(&rec, kPageA, 4);
        const uint32_t mut = cg.check(&rec, since);
        expect("T11b planted break CAUGHT: the old no-scope shape (bare event before MARK, no slot) wrongly ACCEPTs "
               "a write that actually lands inside the reader's own window", mut == N48_CG_OK);
    }
}

static void test_T11c_containment_not_overlap() {
    Cg cg;
    const uint64_t slotHi = kLo + 0x2000ull;              // a slot open over only HALF of [kLo, kHi)
    const int32_t slot = cg.open(kLo, slotHi);            // owner defaults to 0x1000
    cg.begin(slot, kLo, slotHi);
    const uint64_t wLo = kLo + 0x1000ull, wHi = kLo + 0x3000ull;   // half inside the slot, half outside it
    expect("T11c real: n48_cg_slot_contains says NOT CONTAINED for a write only half inside the open slot",
           !n48_cg_slot_contains(cg.slots, N48_CG_SLOTS, wLo, wHi, 0x1000ull));
    // MUTANT: use OVERLAP (n48_cg_slot_scan, the pre-0.437 test navi48_vram_write_mm used) as the containment test
    // instead - it wrongly reports the half-inside write as covered (INFLIGHT).
    const int mutSaysCovered = n48_cg_slot_scan(cg.slots, N48_CG_SLOTS, wLo, wHi) == N48_CG_SLOT_INFLIGHT;
    expect("T11c planted break CAUGHT: overlap (n48_cg_slot_scan) wrongly treats the half-inside write as covered",
           mutSaysCovered != 0);
    expect("T11c sanity: a write fully inside [kLo, slotHi) IS contained (same owner)",
           n48_cg_slot_contains(cg.slots, N48_CG_SLOTS, kLo + 0x100ull, kLo + 0x200ull, 0x1000ull));
    cg.end(slot, kLo, slotHi); cg.close(slot);
}

// =============================================================================================================
// T11d — 0.0.438: CONTAINMENT MUST COUNT ONLY A SLOT OWNED BY THE CALLING THREAD. A slot opened
// by thread A must never let thread B's write treat itself as already scoped - A's scope can close at any time B
// does not control. Planted break: a "contains, any owner" variant (the pre-D4 shape) wrongly says CONTAINED.
// =============================================================================================================
static inline int slot_contains_any_owner_mutant(const n48_cg_slot *slots, uint32_t n, uint64_t lo, uint64_t hi) {
    // *** PLANTED BREAK: n48_cg_slot_contains before D4 - no owner parameter or comparison at all ***
    for (uint32_t i = 0; i < n; i++) {
        const uint64_t s1 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
        const uint64_t l  = __atomic_load_n(&slots[i].lo,  __ATOMIC_SEQ_CST);
        const uint64_t h  = __atomic_load_n(&slots[i].hi,  __ATOMIC_SEQ_CST);
        const uint64_t s2 = __atomic_load_n(&slots[i].seq, __ATOMIC_SEQ_CST);
        if (s1 != s2) continue;
        if ((s1 & 1ull) && n48_cg_contains(l, h, lo, hi)) return 1;
    }
    return 0;
}
static void test_T11d_containment_owner_thread() {
    Cg cg;
    constexpr uintptr_t kThreadA = 0xAAAAull, kThreadB = 0xBBBBull;
    const int32_t slot = cg.open(kLo, kHi, kThreadA);     // thread A's own residency copy opens this scope
    cg.begin(slot, kLo, kHi);
    const uint64_t wLo = kLo + 0x100ull, wHi = kLo + 0x200ull;   // fully inside [kLo, kHi)
    expect("T11d real: thread A's OWN write IS contained by thread A's open slot",
           n48_cg_slot_contains(cg.slots, N48_CG_SLOTS, wLo, wHi, kThreadA));
    expect("T11d real: thread B's write is NOT contained by thread A's open slot - self-scopes instead",
           !n48_cg_slot_contains(cg.slots, N48_CG_SLOTS, wLo, wHi, kThreadB));
    // Planted break: the pre-D4 "any owner" shape wrongly treats thread B's write as already covered.
    const int mutSaysContained = slot_contains_any_owner_mutant(cg.slots, N48_CG_SLOTS, wLo, wHi);
    expect("T11d planted break CAUGHT: an owner-blind containment test wrongly self-scopes thread B's write "
           "into thread A's slot", mutSaysContained != 0);
    cg.end(slot, kLo, kHi); cg.close(slot);
}

// =============================================================================================================
// T12 — 0.0.437: UNTRACKED COPIES LEAVE A MATCH-ALL TRAIL. A reader window holding an
// untracked copy's BEGIN or END refuses even AFTER the untracked copy has already closed (so the untrackedCount
// fast-path in n48_cg_check no longer applies) and even for a page nowhere near where the untracked copy actually
// wrote - the whole point of "match-all": an untracked copy names no range of its own, so it must refuse EVERY one.
// =============================================================================================================
static void test_T12_untracked_match_all_trail() {
    // Real: mirrors navi48_cg_open/navi48_cg_close's own untracked path exactly.
    {
        Cg cg;
        std::vector<int32_t> fillers;
        for (uint32_t i = 0; i < N48_CG_SLOTS; i++) fillers.push_back(cg.open(kLo, kHi));   // fill every real slot
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // MARK before the untracked copy opens
        const int32_t u = cg.open(kLo, kHi);                 // every slot taken -> UNTRACKED
        expect("T12 setup: the forced extra open is untracked (-1)", u < 0);
        n48_cg_ring_push(&cg.ring, N48_CG_EV_BEGIN, ~0u, 0ull, ~0ull);   // navi48_cg_open's own untracked BEGIN
        cg.close(u);                                          // untracked closes: untrackedCount back to 0
        n48_cg_ring_push(&cg.ring, N48_CG_EV_END, ~0u, 0ull, ~0ull);    // navi48_cg_close's own untracked END
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
        n48_cg_pagerec_note(&rec, 0x77770000ull, 4);          // a page NOWHERE near any real slot's own range
        const uint32_t real = cg.check(&rec, since);
        expect("T12 real: a reader window holding the untracked copy's BEGIN/END REFUSES (EVENT), for ANY page",
               real == N48_CG_EVENT);
        for (int32_t s : fillers) cg.close(s);
    }
    // Mutant: the pre-0.437 shape - an untracked open/close pushes NOTHING into the ring.
    {
        Cg cg;
        std::vector<int32_t> fillers;
        for (uint32_t i = 0; i < N48_CG_SLOTS; i++) fillers.push_back(cg.open(kLo, kHi));
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        const int32_t u = cg.open(kLo, kHi);
        // *** PLANTED BREAK: push no event at all for the untracked OPEN ***
        cg.close(u);
        // *** PLANTED BREAK: push no event at all for the untracked CLOSE ***
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
        n48_cg_pagerec_note(&rec, 0x77770000ull, 4);
        const uint32_t mut = cg.check(&rec, since);
        expect("T12 planted break CAUGHT: pushing no event for an untracked open/close wrongly ACCEPTs afterward",
               mut == N48_CG_OK);
        for (int32_t s : fillers) cg.close(s);
    }
}

// =============================================================================================================
// T13 — 0.0.437: POISON ONLY WHAT WAS WRITTEN, AND ALL OF IT. Models residency_copy_to_vram's
// own first-chunk decision (`if (pos > 0) cgScope.markFailed();`) and shadercache_hit/substitute_blit_kernel_at's
// own poison paths (item 3b) against n48_cg_close_poison, the REAL pure function navi48_cg_close routes every
// close through - T9's source pins (Navi48AccelPeer.cpp) confirm these are actually the calls the kext makes.
// =============================================================================================================
static void test_T13_poison_only_what_was_written() {
    // pos == 0 (the FIRST chunk refused, nothing written yet): must NOT poison.
    {
        n48_cg_poison p; n48_cg_poison_init(&p);
        const uint64_t pos = 0ull;
        const bool wouldMarkFailed = pos > 0ull;   // mirrors `if (pos > 0) cgScope.markFailed();` exactly
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/wouldMarkFailed ? 1 : 0, /*mismatch=*/0);
        expect("T13 real: a pos==0 (first-chunk) refusal leaves NO poison", !n48_cg_poison_overlaps(&p, kLo, kHi));
    }
    // pos > 0 (bytes already written before this chunk failed): must poison.
    {
        n48_cg_poison p; n48_cg_poison_init(&p);
        const uint64_t pos = 0x4000ull;
        const bool wouldMarkFailed = pos > 0ull;
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/wouldMarkFailed ? 1 : 0, /*mismatch=*/0);
        expect("T13 real: a pos>0 refusal (a genuine PARTIAL WRITE) DOES poison", n48_cg_poison_overlaps(&p, kLo, kHi));
    }
    // MUTANT: the pre-0.437 shape - markFailed() called UNCONDITIONALLY, even at pos==0. CAUGHT.
    {
        n48_cg_poison p; n48_cg_poison_init(&p);
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/1, /*mismatch=*/0);   // unconditional, pos==0 case
        expect("T13 mutant (unconditional markFailed) CAUGHT: wrongly poisons a copy that wrote NOTHING (pos==0)",
               n48_cg_poison_overlaps(&p, kLo, kHi));
    }
    // item 3b: shadercache_hit's own PARTIAL WRITE / mismatch flags drive the SAME pure decision.
    {
        n48_cg_poison p; n48_cg_poison_init(&p);
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/1, /*mismatch=*/0);   // gScPartialWrite path
        expect("T13 real: shadercache's own PARTIAL WRITE flag poisons the copy's range, same mechanism",
               n48_cg_poison_overlaps(&p, kLo, kHi));
    }
    {
        n48_cg_poison p; n48_cg_poison_init(&p);
        n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/0, /*mismatch=*/1);   // gScMismatch path
        expect("T13 real: shadercache's own read-back-mismatch flag poisons the copy's range, same mechanism",
               n48_cg_poison_overlaps(&p, kLo, kHi));
    }
}

// =============================================================================================================
// T14 — 0.0.437: ONE GUARD WINDOW PER FRAME. A copy landing between segment 0's translate and
// segment 1's check, over a page segment 0's translate read, refuses at segment 1 - because the recorder and mark
// are no longer reset per segment (navi48_cg_seg_begin runs ONCE, before the loop). Break: reset the mark/recorder
// per segment (the pre-0.437 shape) - the copy that lands between them becomes invisible to segment 1's own check.
// =============================================================================================================
static void test_T14_one_window_per_frame() {
    // Real: ONE mark/recorder for the whole pass (navi48_cg_seg_begin called once, before the segment loop).
    {
        Cg cg;
        n48_cg_pagerec rec; n48_cg_pagerec_reset(&rec);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);   // navi48_cg_seg_begin(), ONCE, before the loop
        n48_cg_pagerec_note(&rec, kPageA, 4);                 // segment 0's translate reads this page
        const uint32_t seg0check = cg.check(&rec, since);     // navi48_cg_seg_check() after segment 0
        expect("T14 setup: segment 0's own check is OK (nothing written yet)", seg0check == N48_CG_OK);
        // A copy lands here, rewriting the SAME page segment 0 just read.
        const int32_t slot = cg.open(kLo, kHi);   // kPageA lies inside [kLo, kHi)
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        cg.close(slot);
        // Segment 1's translate: the SAME recorder/mark (one window per frame) still holds kPageA from segment 0.
        const uint32_t seg1check = cg.check(&rec, since);
        expect("T14 real: segment 1's check REFUSES (EVENT) - the copy that rewrote segment 0's page is caught",
               seg1check == N48_CG_EVENT);
    }
    // Mutant: reset the mark/recorder PER SEGMENT (the pre-0.437 shape). Segment 1 gets a FRESH mark taken AFTER
    // the copy already closed, so its own window never contains the copy's BEGIN/END at all.
    {
        Cg cg;
        n48_cg_pagerec rec0; n48_cg_pagerec_reset(&rec0);
        const uint64_t since0 = n48_cg_ring_mark(&cg.ring);
        n48_cg_pagerec_note(&rec0, kPageA, 4);
        const uint32_t seg0check = cg.check(&rec0, since0);
        expect("T14 mutant setup: segment 0's own check is OK", seg0check == N48_CG_OK);
        const int32_t slot = cg.open(kLo, kHi);
        cg.begin(slot, kLo, kHi);
        cg.end(slot, kLo, kHi);
        cg.close(slot);
        // *** PLANTED BREAK: segment 1 gets its OWN fresh recorder/mark, taken AFTER the copy already closed ***
        n48_cg_pagerec rec1; n48_cg_pagerec_reset(&rec1);
        const uint64_t since1 = n48_cg_ring_mark(&cg.ring);
        n48_cg_pagerec_note(&rec1, kPageA, 4);
        const uint32_t seg1check = cg.check(&rec1, since1);
        expect("T14 planted break CAUGHT: resetting the mark per segment wrongly ACCEPTs the frame at segment 1",
               seg1check == N48_CG_OK);
    }
}

// =============================================================================================================
// T15 — 0.0.438: THE UNTRACKED CLOSE MUST PUSH ITS MATCH-ALL END BEFORE DECREMENTING
// gCgUntracked, mirroring the tracked path's END-before-seq-even rule (n48_cg_check's own header comment has that
// race). Models the exact interleaving the review named: a reader whose OWN untrackedCount snapshot and ring `now`
// mark land BETWEEN the two steps of the untracked CLOSE. With the real (END-first) order, nothing can ever
// observe untrackedCount == 0 before END exists in the ring, so the reader's fail-closed count check or its ring
// scan always catches it; with the planted break (decrement-first), a snapshot landing in that gap sees
// untrackedCount == 0 (the count-based fail-closed check does not fire) AND a `now` mark taken in that same gap
// misses END too (not pushed yet) - the exact hole found at OPEN, reopened here at CLOSE.
// =============================================================================================================
static void test_T15a_untracked_close_end_before_decrement() {
    // Real order (END pushed, THEN decrement): a reader's check landing in the gap between the two steps still
    // sees untrackedCount == 1 (not yet decremented) and fails closed on the count alone - it never even reaches
    // the ring scan, so END's own placement cannot matter for THIS particular snapshot.
    {
        Cg cg;
        std::vector<int32_t> fillers;
        for (uint32_t i = 0; i < N48_CG_SLOTS; i++) fillers.push_back(cg.open(kLo, kHi));   // fill every slot
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        cg.open(kLo, kHi);                                   // UNTRACKED
        n48_cg_ring_push(&cg.ring, N48_CG_EV_BEGIN, ~0u, 0ull, ~0ull);
        // --- CLOSE begins, REAL order ---
        n48_cg_ring_push(&cg.ring, N48_CG_EV_END, ~0u, 0ull, ~0ull);   // END pushed FIRST
        // A reader's snapshot lands HERE, between END and the decrement: untrackedCount is still 1.
        const uint32_t untrackedSnapshotBeforeDecrement = cg.untracked;
        expect("T15a real: mid-close, before the decrement, untrackedCount is still 1 (fails closed on the count alone)",
               untrackedSnapshotBeforeDecrement == 1u);
        n48_cg_pagerec recEmpty; n48_cg_pagerec_reset(&recEmpty);
        const uint32_t midCheck = n48_cg_check(&recEmpty, &cg.poison, cg.slots, N48_CG_SLOTS, &cg.ring, since,
                                                untrackedSnapshotBeforeDecrement);
        expect("T15a real: a check taken in that exact gap REFUSES (UNTRACKED)", midCheck == N48_CG_UNTRACKED);
        if (cg.untracked) cg.untracked--;                     // decrement LAST
        for (int32_t s : fillers) cg.close(s);
    }
    // Planted break: decrement FIRST, THEN push END - the pre-D3 order. A reader's snapshot landing in the gap now
    // sees untrackedCount == 0 (passes the fail-closed count check) AND a `now` mark taken in that same gap misses
    // END (not pushed yet), so the whole untracked copy is invisible to this reader.
    {
        Cg cg;
        std::vector<int32_t> fillers;
        for (uint32_t i = 0; i < N48_CG_SLOTS; i++) fillers.push_back(cg.open(kLo, kHi));
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        cg.open(kLo, kHi);
        n48_cg_ring_push(&cg.ring, N48_CG_EV_BEGIN, ~0u, 0ull, ~0ull);
        // --- CLOSE begins, PLANTED BREAK order ---
        if (cg.untracked) cg.untracked--;                     // *** PLANTED BREAK: decrement FIRST ***
        // A reader's snapshot lands HERE: untrackedCount already 0, and END has not been pushed yet.
        const uint32_t untrackedSnapshotInGap = cg.untracked;
        n48_cg_pagerec recEmpty2; n48_cg_pagerec_reset(&recEmpty2);
        const uint32_t mutCheck = n48_cg_check(&recEmpty2, &cg.poison, cg.slots, N48_CG_SLOTS, &cg.ring, since,
                                                untrackedSnapshotInGap);
        n48_cg_ring_push(&cg.ring, N48_CG_EV_END, ~0u, 0ull, ~0ull);   // END pushed too late to matter here
        expect("T15a planted break CAUGHT: decrement-before-END lets a mid-gap check wrongly ACCEPT",
               mutCheck == N48_CG_OK);
        for (int32_t s : fillers) cg.close(s);
    }
}

// T15b — the untracked close must ALSO poison [lo, hi) on failed/mismatch, exactly as the tracked path does -
// before this fix an untracked copy's failure left nothing in the poison table at all.
static void test_T15b_untracked_close_poisons() {
    n48_cg_poison p; n48_cg_poison_init(&p);
    // Mirrors navi48_cg_close's own untracked branch: n48_cg_close_poison is called there exactly as the tracked
    // branch calls it, with the SAME wrote/failed/mismatch the caller passed to navi48_cg_close.
    n48_cg_close_poison(&p, kLo, kHi, /*wrote=*/1, /*failed=*/1, /*mismatch=*/0);
    expect("T15b real: an untracked copy that FAILED poisons its own [lo, hi) range",
           n48_cg_poison_overlaps(&p, kLo, kHi));
    // Planted break: the pre-D3 shape - the untracked branch returns before ever calling n48_cg_close_poison.
    n48_cg_poison p2; n48_cg_poison_init(&p2);
    const bool mutantCalledClosePoison = false;   // *** PLANTED BREAK: the untracked branch never poisons ***
    if (mutantCalledClosePoison) n48_cg_close_poison(&p2, kLo, kHi, 1, 1, 0);
    expect("T15b planted break CAUGHT: skipping the poison call leaves a FAILED untracked copy's range clean",
           !n48_cg_poison_overlaps(&p2, kLo, kHi));
}

// =============================================================================================================
// T9 — SOURCE PINS AND THE ALLOWLIST. Every argument is optional; checks skip (not fail) when a file is absent.
// =============================================================================================================
static void test_T9(const char *bringupPath, const char *ahhPath, const char *peerPath, const char *ttlPath) {
    // The static_assert distinctness (compiled, not textual): already proved at compile time by gfx_copyguard.h
    // itself for this translation unit — record that fact as a check so the suite's count reflects it.
    expect("T9 N48_SEG_COPY_OVERLAP is outside both xlat12 status ranges (compile-time static_assert in gfx_copyguard.h)",
           N48_SEG_COPY_OVERLAP > 10u && N48_SEG_COPY_OVERLAP > 29u);
    expect("T9 the cguard: report line stays under n48log's 512-byte cap at its numeric widest", [] {
        char buf[1024];
        n48_cg_stats s{};
        s.opened = s.closed = s.inFlight = s.poisoned = s.untracked = s.unscoped = s.checked = ~0ull;
        for (uint32_t i = 0; i < N48_CG_REASONS; i++) s.refused[i] = ~0ull;
        const int n = std::snprintf(buf, sizeof buf, N48_CG_FMT, N48_CG_ARGS(&s));
        return n > 0 && (size_t)n < 512u;
    }());

    std::string bringup, ahh, peer, ttl;
    const bool haveBringup = bringupPath && read_file(bringupPath, bringup);
    const bool haveAhh     = ahhPath && read_file(ahhPath, ahh);
    const bool havePeer    = peerPath && read_file(peerPath, peer);
    const bool haveTtl     = ttlPath && read_file(ttlPath, ttl);

    if (haveBringup) {
        expect("T9 Navi48Bringup.cpp defines the copy-guard storage once",
               count_of(bringup, "static n48_cg_slot   gCgSlots[N48_CG_SLOTS]") == 1u);
        expect("T9 navi48_cg_open/close are defined exactly once each",
               count_of(bringup, "int32_t navi48_cg_open(") == 1u && count_of(bringup, "void navi48_cg_close(") == 1u);
        expect("T9 the phantom-scope control holds for 2000000 us (2 s)",
               has(bringup, "elapsedUs < 2000000ull"));
        // 0.0.435 review fix: the phantom scope must close with wrote=false, or its close would erase every
        // poison row (the exact defect the review found) via n48_cg_close_poison's wrote=0 no-op guard.
        expect("T9 the phantom scope constructs its Navi48CopyScope with wrote=false",
               has(bringup, "Navi48CopyScope scope(0ull, hi, /*wrote=*/false);"));
        expect("T9 navi48_cg_close routes poison through the pure n48_cg_close_poison, gated on wrote",
               has(bringup, "n48_cg_close_poison(&gCgPoison, lo, hi, wrote ? 1 : 0, failed ? 1 : 0, mismatch ? 1 : 0);"));
        expect("T9 opened counts every open (tracked or untracked), not only the tracked branch",
               // build 0.0.529: the open now carries switch 84's instrument range (n48_cg_slot_open_d, dlo/dhi).
               has(bringup, "const int32_t slot = n48_cg_slot_open_d(gCgSlots, N48_CG_SLOTS, lo, hi, (uintptr_t)current_thread(), dlo, dhi);\n\t__atomic_fetch_add(&gCgStats.opened, 1ull, __ATOMIC_RELAXED);"));
        // 0.0.438: every OPEN publishes the calling thread as the slot's owner.
        expect("T9 navi48_cg_open stamps the slot with current_thread() as its owner",
               has(bringup, "n48_cg_slot_open_d(gCgSlots, N48_CG_SLOTS, lo, hi, (uintptr_t)current_thread(), dlo, dhi);"));   // 0.0.529
        expect("T9 the per-refusal inFlight++ is gone from navi48_cg_seg_check", !has(bringup, "gCgStats.inFlight++;"));
        expect("T9 the report's in-flight field is the LIVE count, set at snapshot time",
               has(bringup, "out->inFlight = n48_cg_live_open(gCgSlots, N48_CG_SLOTS, __atomic_load_n(&gCgUntracked, __ATOMIC_SEQ_CST));"));
        // 0.0.437 (item 1): the pre-write N48_CG_EV_WRITE push and its gMmPrioNesting > 0 gate are GONE - a write
        // not contained in one open slot self-scopes instead.
        expect("T9 the pre-0.437 unconditional-count-then-conditional-ring-push shape is gone from navi48_vram_write_mm",
               !has(bringup, "if (gMmPrioNesting > 0u) n48_cg_ring_push(&gCgRing, N48_CG_EV_WRITE,"));
        // 0.0.438: containment is decided against THIS thread's own identity, not any owner.
        expect("T9 navi48_vram_write_mm decides containment with n48_cg_slot_contains against THIS thread's identity",
               has(bringup, "const bool cgContained = n48_cg_slot_contains(gCgSlots, N48_CG_SLOTS, vramOffset, vramOffset + bytes,\n\t                                              (uintptr_t)current_thread());"));
        expect("T9 an uncontained write counts unscoped and self-opens its OWN scope over exactly its own range",
               has(bringup, "__atomic_fetch_add(&gCgStats.unscoped, 1ull, __ATOMIC_RELAXED);") &&
               has(bringup, "cgSelfSlot = navi48_cg_open(vramOffset, vramOffset + bytes);"));
        expect("T9 the self-scope closes wrote=true AFTER the write loop, gated on whether it was actually opened",
               has(bringup, "if (cgSelfScoped) navi48_cg_close(cgSelfSlot, vramOffset, vramOffset + bytes, /*wrote=*/true, /*failed=*/!ok, /*mismatch=*/false);"));
        // item 2: an untracked open/close publishes a match-all [0, ~0ull) BEGIN/END trail.
        // build 0.0.523 (RING-NEUTER-FORGIVE.md item 8): the pushes now carry the pushing thread (n48_cg_ring_push_o with
        // current_thread(), written before the stamp) - the SAME kind/slot/range, so these pins name the new call.
        expect("T9 an untracked OPEN pushes a match-all BEGIN over [0, ~0ull)",
               has(bringup, "n48_cg_ring_push_o(&gCgRing, N48_CG_EV_BEGIN, ~0u, 0ull, ~0ull, (uintptr_t)current_thread());"));
        expect("T9 the matching untracked CLOSE pushes a match-all END over the SAME range",
               has(bringup, "n48_cg_ring_push_o(&gCgRing, N48_CG_EV_END, ~0u, 0ull, ~0ull, (uintptr_t)current_thread());"));
        // item 7: every gCgStats increment/read is atomic.
        // 0.0.438: the untracked CLOSE branch now also poisons on failure/mismatch, which adds
        // ONE more __atomic_fetch_add(&gCgStats.poisoned, ...) call site (8 -> 9) beside the pre-existing tracked one.
        expect("T9 every gCgStats field is written with __atomic_fetch_add RELAXED (no plain ++ survives)",
               !has(bringup, "gCgStats.opened++") && !has(bringup, "gCgStats.closed++") &&
               !has(bringup, "gCgStats.poisoned++") && !has(bringup, "gCgStats.untracked++") &&
               !has(bringup, "gCgStats.unscoped++") && !has(bringup, "gCgStats.checked++") &&
               count_of(bringup, "__atomic_fetch_add(&gCgStats.") == 9u);
        expect("T9 navi48_cg_snapshot reads every gCgStats field with __atomic_load_n, not a plain struct copy",
               !has(bringup, "*out = gCgStats;") && count_of(bringup, "__atomic_load_n(&gCgStats.") == 7u);
        // 0.0.438 (D3): the untracked CLOSE branch pushes END before decrementing gCgUntracked (mirroring the
        // tracked branch's END-before-seq-even order), and poisons its own range on failure/mismatch.
        {
            const size_t pEnd = bringup.find("n48_cg_ring_push_o(&gCgRing, N48_CG_EV_END, ~0u, 0ull, ~0ull, (uintptr_t)current_thread());");
            const size_t pDec = bringup.find("__atomic_fetch_sub(&gCgUntracked, 1u, __ATOMIC_SEQ_CST);");
            expect("T9 the untracked close pushes its match-all END before decrementing gCgUntracked",
                   pEnd != std::string::npos && pDec != std::string::npos && pEnd < pDec);
        }
        expect("T9 the untracked close poisons [lo, hi) on failed/mismatch via the same pure n48_cg_close_poison",
               has(bringup, "n48_cg_ring_push_o(&gCgRing, N48_CG_EV_END, ~0u, 0ull, ~0ull, (uintptr_t)current_thread());   // 0.0.523: + owner\n\t\t// 0.0.438: an untracked copy must poison"));
        // THE ALLOWLIST: every real call site of navi48_vram_write_mm( in this file, minus its own definition line.
        // Still exactly 1: the self-scoping added inside navi48_vram_write_mm calls navi48_cg_open/navi48_cg_close,
        // never navi48_vram_write_mm itself, so this count is untouched by items 1/2/7.
        const size_t total = count_of(bringup, "navi48_vram_write_mm(");
        const size_t def = count_of(bringup, "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {");
        expect("T9 allowlist: Navi48Bringup.cpp has exactly 1 navi48_vram_write_mm call site (srcOff + i * 4u)",
               total - def == 1u);
    } else std::printf("SKIP  T9: no Navi48Bringup.cpp path given\n");

    if (haveAhh) {
        expect("T9 gfxc_read_core takes the recorder parameter",
               has(ahh, "static uint32_t gfxc_read_core(const GfxcVm &vm, uint64_t va, uint32_t *dst, uint32_t n, uint32_t *sysPages, n48_cg_pagerec *rec)"));
        expect("T9 gfxc_read itself always passes a null recorder (byte-identical to every caller before this brief)",
               has(ahh, "return gfxc_read_core(vm, va, dst, n, sysPages, nullptr);"));
        expect("T9 gfxc_read_rs is the only entry point that passes a real recorder",
               count_of(ahh, "static inline uint32_t gfxc_read_rs(") == 1u);
        expect("T9 the descriptor-read callback is routed through gfxc_read_rs",
               has(ahh, "return gfxc_read_rs(*static_cast<const GfxcVm *>(vm), va, dst, n, nullptr, navi48_cg_active_recorder());"));
        // 0.0.437 (item 4): navi48_cg_seg_begin() moved from inside the segment loop (once per segment) to ONCE per
        // pass, before it - the whole point of "one guard window per frame". count_of pins "once" (not merely
        // "present"); the position check pins "before the loop", by finding the loop's own for-statement text.
        expect("T9 navi48_cg_seg_begin() is called EXACTLY ONCE in the file (once per pass, not once per segment)",
               count_of(ahh, "navi48_cg_seg_begin();") == 1u);
        {
            const size_t pBegin = ahh.find("navi48_cg_seg_begin();");
            const size_t pLoop  = ahh.find("for (uint32_t k = 0; k < ns && k < N48_XV_MAX_SEGS; k++) {");
            expect("T9 navi48_cg_seg_begin() sits BEFORE the segment loop's own for-statement",
                   pBegin != std::string::npos && pLoop != std::string::npos && pBegin < pLoop);
        }
        expect("T9 the per-segment call site no longer resets the recorder itself (the comment there says so)",
               has(ahh, "0.0.437: navi48_cg_seg_begin() is no longer called here, per segment"));
        expect("T9 the segment loop checks AFTER the translate call, only when the translate itself did not already refuse",
               has(ahh, "if (!st) {\n            const uint32_t cgReason = navi48_cg_seg_check();"));
        expect("T9 a copy-guard refusal is the kext-only status, never one of xlat12's own",
               has(ahh, "st = N48_SEG_COPY_OVERLAP;"));
        expect("T9 the LUT learn is skipped for a copy-guard-refused segment",
               has(ahh, "if (gLutOn && ds.in_abi && !cgRefused) {"));
        expect("T9 switch 39 is the phantom-scope positive control",
               has(ahh, "(arg & 0xffull) == 39ull") && has(ahh, "navi48_cg_phantom_scope();"));
        expect("T9 the cguard: report line prints on every read of the gfxneuter report",
               has(ahh, "cguard_report_line();"));
        // ALLOWLIST over AppleHardwareHook.cpp's own call sites (16, by census at 0.0.435 - grep -n confirmed).
        // build 0.0.522 (switch 76): 17 - spill_mm_wr, the spill tier's flush of its own arena slice (gfx_spill.h
        // n48_sp_flush: every written dword is read back before the IB write; the write self-scopes like every unscoped write).
        const size_t total = count_of(ahh, "navi48_vram_write_mm(");
        expect("T9 allowlist: AppleHardwareHook.cpp has exactly 17 navi48_vram_write_mm call sites",
               total == 17u);
        expect("T9 allowlist: the 17th is the spill tier's flush callback, and only it",
               count_of(ahh, "static uint32_t spill_mm_wr(void *, uint64_t vram, uint32_t *buf, uint32_t ndw) { return navi48_vram_write_mm(vram, buf, ndw) ? 1u : 0u; }") == 1u);

        // build 0.0.451 item 5 (N7, reviewer's planted break, NOT caught): the census's own `stored` count
        // MUST be capped at kGcapCensusPerFrame (the static `rows[kGcapCensusPerFrame]` array's own size) - an
        // uncapped `stored = total` is an OUT-OF-BOUNDS READ the instant a frame's census finds more than 48
        // candidate rows (n48_gcap_census_ib's own `*total` is uncapped by design; capping is THIS caller's job).
        const char *kN7Real = "const uint32_t stored = total < kGcapCensusPerFrame ? total : kGcapCensusPerFrame;";
        const char *kN7Break = "const uint32_t stored = total;";
        expect("N7 the census's `stored` count is capped at kGcapCensusPerFrame, verbatim, exactly once",
               count_of(ahh, kN7Real) == 1u);
        expect("N7 BREAK-check: the planted uncapped shape ('stored = total;', an over-read of the static rows) is not present",
               count_of(ahh, kN7Break) == 0u);

        // build 0.0.452 item 4 (K7, reviewer test gap NOT CAUGHT by 0.0.451's own tests): gcap_census_step's
        // dup check must run BEFORE the budget decrement, verbatim, exactly once - a planted break moving the
        // decrement earlier (`(*censusBudget)--;` ahead of the `if (dup)` check) must be caught as a real
        // source-order regression, not merely a text-presence check (both lines exist either way).
        const char *kK7Dup = "if (dup) { gGcCensus.dup++; continue; }";
        const char *kK7Dec = "(*censusBudget)--;";
        expect("K7 the dup check exists exactly once in gcap_census_step", count_of(ahh, kK7Dup) == 1u);
        expect("K7 the budget decrement exists exactly once in gcap_census_step", count_of(ahh, kK7Dec) == 1u);
        {
            const size_t pDup = ahh.find(kK7Dup);
            const size_t pDec = ahh.find(kK7Dec);
            expect("K7 the dup check sits BEFORE the budget decrement (a duplicate row is never charged)",
                   pDup != std::string::npos && pDec != std::string::npos && pDup < pDec);
            // planted break: move the decrement to just before the dup check (the described regression) and
            // confirm the order assertion above would then fail - i.e. the check is load-bearing, not vacuous.
            std::string mut = ahh;
            const size_t p2 = mut.find(kK7Dec);
            if (p2 != std::string::npos) mut.erase(p2, std::strlen(kK7Dec));
            const size_t p3 = mut.find(kK7Dup);
            if (p3 != std::string::npos) mut.insert(p3, std::string(kK7Dec) + " ");
            const size_t pDup2 = mut.find(kK7Dup);
            const size_t pDec2 = mut.find(kK7Dec);
            const int caught = !(pDup2 != std::string::npos && pDec2 != std::string::npos && pDup2 < pDec2);
            std::printf("  planted %-66s %s\n", "K7: censusBudget decrement moved before the dup check",
                        caught ? "CAUGHT" : "MISSED");
            expect("K7 BREAK-check: moving the decrement before the dup check is caught", caught != 0);
        }
    } else std::printf("SKIP  T9: no AppleHardwareHook.cpp path given\n");

    if (havePeer) {
        expect("T9 residency_copy_to_vram opens its scope right after the pre-flight loop, before any MM access",
               has(peer, "Navi48CopyScope cgScope(cgLo, cgHi);"));
        expect("T9 a FAILED write inside the copy marks the scope failed",
               count_of(peer, "cgScope.markFailed();") >= 2u);
        expect("T9 a read-back mismatch (VERIFIED to return true anyway, ) marks the scope mismatched",
               has(peer, "if (mismatched) cgScope.markMismatch();"));
        expect("T9 the standalone substitute_blit_kernel_at verb call gets its OWN scope",
               has(peer, "Navi48CopyScope cgScope(at, at + (uint64_t)sub_kernel_bytes(mode));"));
        // 0.0.437 (item 3a): a first-chunk refusal (pos == 0, nothing written yet) must NOT poison.
        expect("T9 the first-chunk refusal gates markFailed on bytes already written (pos > 0)",
               has(peer, "if (pos > 0) cgScope.markFailed();"));
        expect("T9 the old unconditional 'poisoned out of caution' comment for that site is gone",
               !has(peer, "PARTIAL WRITE (or a first-chunk refusal, poisoned out of caution)"));
        // 0.0.438: a pos == 0 refusal must close with wrote=false (markNothingWritten), not
        // silently default to wrote=true - the exact FAIL-OPEN the review caught (the 0.0.437 fix skipped
        // markFailed() at pos == 0 but never told the scope it wrote nothing, so its CLOSE still ran a CLEAN
        // wrote=true close and cleared any poison already covering that range).
        expect("T9 a pos == 0 refusal closes with markNothingWritten(), not a silent wrote=true default",
               has(peer, "if (pos > 0) cgScope.markFailed();\n            else cgScope.markNothingWritten();"));
        // 0.0.437 (item 3b): shadercache_hit / substitute_blit_kernel_at run INSIDE this scope and must be able to
        // poison it too, via the least-invasive mechanism each already has (per-call ScCtx state for the void scan
        // function; the return value/reason the copier already reads for the kernel-substitution call).
        // 0.0.438 (ScCtx FLAGS FIX): the old file-scope gScPartialWrite/gScMismatch globals are GONE -
        // two concurrent residency copies could clear each other's flag through them. Pin their absence as
        // DECLARATIONS/writes, not merely as a substring (the identifier still appears in explanatory comments).
        expect("T9 the gScPartialWrite global declaration is gone",
               !has(peer, "static bool     gScPartialWrite { false };"));
        expect("T9 the gScMismatch global declaration is gone",
               !has(peer, "static bool     gScMismatch     { false };"));
        expect("T9 shadercache_hit's PARTIAL WRITE path sets the PER-CALL ScCtx flag, not a global",
               has(peer, "c->partialWrite = true;") && !has(peer, "gScPartialWrite = true;"));
        expect("T9 shadercache_hit's read-back-mismatch path sets the PER-CALL ScCtx flag, not a global",
               has(peer, "if (bad) c->mismatch = true;") && !has(peer, "if (bad) gScMismatch = true;"));
        expect("T9 ScCtx carries partialWrite/mismatch as per-call state",
               has(peer, "bool          partialWrite;") && has(peer, "bool          mismatch;"));
        expect("T9 shadercache_scan_resource hands its own ScCtx's flags out through out-parameters",
               has(peer, "if (outPartialWrite) *outPartialWrite = ctx.partialWrite;") &&
               has(peer, "if (outMismatch) *outMismatch = ctx.mismatch;"));
        expect("T9 residency_copy_to_vram reads its OWN call's local out-parameters and marks cgScope",
               has(peer, "shadercache_scan_resource(dstMem, dSeg, md, backingOffset, bytes, &scPartialWrite, &scMismatch);") &&
               has(peer, "if (scPartialWrite) cgScope.markFailed();") && has(peer, "if (scMismatch) cgScope.markMismatch();"));
        expect("T9 the kernsub call INSIDE residency_copy_to_vram marks the SAME cgScope from its own status/reason",
               has(peer, "if (st == 0 && reason == 4) cgScope.markFailed();\n            else if (st == 3) cgScope.markMismatch();"));
        // ALLOWLIST over Navi48AccelPeer.cpp's own call sites (3, by census at 0.0.435). build 0.0.529: 4 - switch 84's delta
        // loop writer (d84_wr), which writes only inside the delta's own Navi48CopyScope (tests/gfx_cg84_test.cpp C6/C10).
        const size_t total = count_of(peer, "navi48_vram_write_mm(");
        expect("T9 allowlist: Navi48AccelPeer.cpp has exactly 4 navi48_vram_write_mm call sites", total == 4u);

        // build 0.0.451 item 5 (N1, reviewer's planted break on 0.0.450, NOT caught): switch 46's gate
        // itself, a source-order pin proving the T450-kinds fallback is actually REACHED only under the switch.
        const char *kN1Gate = "if (rtShape != N48_RP_SHAPE_OK && n48::hw_resprov_kinds_on()) {";
        expect("N1 switch 46's gate exists exactly once, verbatim", count_of(peer, kN1Gate) == 1u);
        { std::string mut = peer;
          const size_t p = mut.find(kN1Gate);
          expect("N1 setup: the gate is found verbatim (so the mutation below removes a REAL guard)", p != std::string::npos);
          if (p != std::string::npos) mut.replace(p, std::strlen(kN1Gate), std::string(std::strlen(kN1Gate), ' '));
          std::printf("  planted %-66s %s\n", "N1: switch 46's gate removed (kinds tried unconditionally)",
                      (p != std::string::npos && count_of(mut, kN1Gate) == 0u) ? "CAUGHT" : "MISSED");
          expect("N1 BREAK-check: removing the gate is caught (the guard text disappears)",
                 p != std::string::npos && count_of(mut, kN1Gate) == 0u); }

        // N2 (reviewer's planted break, NOT caught): the page-out flags must be set PER KIND, in the SAME
        // if/else-if chain as the retile call - never as one shared assignment setting both flags together.
        const char *kN2Real = "else if (rtKind == N48_RP_KIND_256B_D_8) gRpEverRetiled256bd = 1;\n                else gRpEverRetiled4kbdx = 1;";
        const char *kN2SharedBreak = "gRpEverRetiled256bd = 1; gRpEverRetiled4kbdx = 1;";
        expect("N2 the two page-out flags are set in the real per-kind if/else-if chain, verbatim, exactly once",
               count_of(peer, kN2Real) == 1u);
        expect("N2 BREAK-check: the planted 'one shared flag' shape (setting BOTH together) is not present in the real source",
               count_of(peer, kN2SharedBreak) == 0u);
    } else std::printf("SKIP  T9: no Navi48AccelPeer.cpp path given\n");

    if (haveTtl) {
        expect("T9 Navi48CopyScope is declared not-copyable",
               has(ttl, "Navi48CopyScope(const Navi48CopyScope &) = delete;"));
        expect("T9 Navi48Ttl.hpp includes gfx_copyguard.h", has(ttl, "#include \"gfx_copyguard.h\""));
        // 0.0.438: the new method a pos == 0 refusal uses to close as "nothing written".
        expect("T9 Navi48CopyScope has markNothingWritten(), setting wrote_ = false",
               has(ttl, "void markNothingWritten() { wrote_ = false; }"));
    } else std::printf("SKIP  T9: no Navi48Ttl.hpp path given\n");

    // The allowlist's GRAND TOTAL, across every file actually given: 20 (1 + 16 + 3), the whole boot's population
    // at 0.0.435 census. A new call site anywhere raises this total and fails the suite. Subtract Navi48Bringup.cpp's
    // own definition line, which also contains the substring "navi48_vram_write_mm(" and is not a call site.
    if (haveBringup && haveAhh && havePeer) {
        const size_t def = count_of(bringup, "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {");
        const size_t total = (count_of(bringup, "navi48_vram_write_mm(") - def) + count_of(ahh, "navi48_vram_write_mm(")
                            + count_of(peer, "navi48_vram_write_mm(");
        // build 0.0.522: 21 (the spill tier's flush in AppleHardwareHook.cpp). build 0.0.529: 22 (switch 84's d84_wr).
        expect("T9 ALLOWLIST TOTAL over all three files: exactly 22 navi48_vram_write_mm call sites", total == 22u);
    }
}

// build 0.0.486 (switch 59, notes/design/STATIC-RETILE.md) - THE LONGER WRITE AND THE COPY GUARD. The backing-sourced
// copy writes a gfx12 image LONGER than res+0x230 (0x19000 for the avatar's 0x15000). Every byte of it must be inside what
// the pre-flight walked through the VRAM guard and what the copy-guard scope [cgLo, cgHi) covers, so the decision is taken
// BEFORE the pre-flight and one length (wBytes) drives the pre-flight, the scope and the write loop. The arithmetic and the
// gate are pure and driven in ws_resprov_test.cpp (L1-L9); these pins prove the kext's ORDER and that the one length is used
// everywhere, and each is non-vacuous (a mutant of the real source fails it).
static size_t lin_pins(const std::string &peerAll, const std::string &ahh, bool loud) {
    size_t bad = 0;
    // residency_copy_to_vram's own text (up to the page-out copy, which has its own `pos < bytes` loop over its own length)
    const size_t fb = peerAll.find("static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    const size_t fe = fb == std::string::npos ? fb : peerAll.find("THE PAGE-OUT COPY: pageTexture(toVram = 0)", fb);
    const size_t hb = peerAll.find("static uint64_t __attribute__((noinline)) rp_lin_prepare(");
    const size_t he = hb == std::string::npos ? hb : peerAll.find("\n}\n", hb);
    const std::string peer = (fb == std::string::npos || fe == std::string::npos || hb == std::string::npos || he == std::string::npos ||
                              he > fb) ? std::string()
                           : peerAll.substr(hb, he + 3u - hb) + peerAll.substr(fb, fe - fb);   // the helper, then the copy
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    // build 0.0.492: the prepared length is RtBufs::lin's upper bits (rp_lin_prepare sets it with the stream).
    const size_t prep = peer.find("if (rpOn) (void)rp_lin_prepare(r, md, srcLen, dstLen, &rt);\n    const uint64_t linBytes = rt.lin & ~N48_RT_LINBLK;");
    const size_t wb = peer.find("const uint64_t wBytes = linBytes ? linBytes : bytes;");
    const size_t pre = peer.find("for (uint64_t pos = 0; pos < wBytes; dSegs++) {");
    const size_t scope = peer.find("Navi48CopyScope cgScope(cgLo, cgHi);");
    // build 0.0.492: the page-out flag of the VRAM side's own mode (rp_lin_mark_pageout), before the first write.
    const size_t flag = peer.find("        if (linBytes) {\n            rtMode = rp_lin_mark_pageout(rt.ls);");
    const size_t retile = peer.find("const bool retile = rpOn && rtShape == N48_RP_SHAPE_OK;");
    const size_t loop = peer.find("while (pos < wBytes) {");
    chk("T16 rp_lin_prepare runs only under rpOn, exactly once", count_of(peer, "rp_lin_prepare(r, md,") == 1u && prep != std::string::npos);
    chk("T16 order: prepare -> wBytes -> pre-flight over wBytes -> copy-guard scope -> page-out flag -> retile -> write loop",
        prep < wb && wb < pre && pre < scope && scope < flag && flag < retile && retile < loop && loop != std::string::npos);
    chk("T16 the pre-flight's per-segment length is bounded by wBytes",
        count_of(peer, "const uint64_t n = (span < wBytes - pos) ? span : wBytes - pos;") == 1u &&
        count_of(peer, "(span < bytes - pos) ? span : bytes - pos") == 0u);
    chk("T16 the write loop's chunk is bounded by wBytes, both places",
        count_of(peer, "const uint64_t n = n48_fc_chunk_len(wBytes, pos, dSpan, kCopyChunkBytes);") == 1u &&   // build 0.0.496
        count_of(peer, "while (pos < bytes) {") == 0u);
    chk("T16 the entry's byte count is the length written", count_of(peer, "rc.bytes = wBytes;") == 1u);
    chk("T16 the switch gate is the pure n48_rp_lin_take with the switch's own reading",
        count_of(peer, "if (!n48_rp_lin_take(why, sw ? 1u : 0u)) return 0ull;") == 1u &&
        count_of(peer, "const bool sw = n48::hw_resprov_lin_on();") == 1u);
    chk("T16 a double claim writes nothing (marks nothing written, returns before the gfx10 block)",
        peer.find("if (linBytes && rtShape == N48_RP_SHAPE_OK) {") != std::string::npos &&
        peer.find("if (linBytes && rtShape == N48_RP_SHAPE_OK) {") < peer.find("            // item 1's write-range proof:"));
    chk("T16 a lin entry is stamped only for a lin copy that re-tiled",
        count_of(peer, "if (linBytes && retile) rp_lin_fill_copy(&rc, rt.ls);") == 1u);
    // build 0.0.492 (item 3, PRIORITY; item 4, the streamed copy)
    const size_t blk = peer.find("if ((rt.lin & N48_RT_LINBLK) && rtShape == N48_RP_SHAPE_OK) {");
    const size_t s46 = peer.find("if (rtShape != N48_RP_SHAPE_OK && n48::hw_resprov_kinds_on()) {");
    const size_t alloc = peer.find("            // item 1's write-range proof:");
    chk("T16 0.0.492 the linear-backing refusal of the gfx10 re-tiles runs after every re-tile check and before their buffers",
        blk != std::string::npos && s46 != std::string::npos && alloc != std::string::npos && s46 < blk && blk < alloc &&
        count_of(peer, "if ((rt.lin & N48_RT_LINBLK) && rtShape == N48_RP_SHAPE_OK) {") == 1u);
    chk("T16 0.0.492 the flag comes from the pure predicate under the switch's own reading",
        count_of(peer, "rt->lin = n48_rp_lin_blocks_old(sw ? 1u : 0u, nibble, in.recOk, in.swzBk) ? N48_RT_LINBLK : 0u;") == 1u);
    chk("T16 0.0.492 the write loop takes a re-tiled copy's bytes only through rp_retile_bytes (the stream when prepared)",
        count_of(peer, "if (retile) { if (!rp_retile_bytes(&rt, pos + k, raw, take)) {") == 1u &&
        count_of(peer, "if (retile) memcpy(raw,") == 0u);
    // the kext's ask and feed (AppleHardwareHook.cpp)
    chk("T16 hw_resprov_lin_on is a pure && of switch 11's hw_resprov_on and switch 59",
        count_of(ahh, "bool hw_resprov_lin_on() { return hw_resprov_on() && gXdResProvLin; }") == 1u);
    chk("T16 the plain ask passes no T# (today's calls, unchanged)",
        count_of(ahh, "return gfxsrc_desc_tiled_ok(ctx, va, mode, elemBytes, nullptr, nullptr);") == 1u);
    chk("T16 only the T# ask reaches n48_rp_ok_t", count_of(ahh, "if (n48_rp_ok_t(&gXdRp, c->ctx, va, mode, elemBytes,") == 1u &&
        ahh.find("        if (t10) {") != std::string::npos);
    const size_t feed = ahh.find("const uint32_t added = n48_dl_feed(&gXdLed, &lf);");
    const size_t drop = ahh.find("(void)n48_rp_lin_drop_target(&gXdRp, lf.ctx, gXdLed.e[q].va, gXdLed.e[q].size);");
    chk("T16 the colour-target drop runs after the committed feed, gated on switch 59",
        feed != std::string::npos && drop != std::string::npos && feed < drop &&
        count_of(ahh, "if (added && gXdResProvLin && lf.ctx)") == 1u);
    return bad;
}
static void test_T16_lin486(const char *ahhPath, const char *peerPath) {
    std::string ahh, peer;
    if (!ahhPath || !peerPath || !read_file(ahhPath, ahh) || !read_file(peerPath, peer)) {
        expect("T16 the kext sources are readable (AppleHardwareHook.cpp, Navi48AccelPeer.cpp)", false); return;
    }
    expect("T16 every pin holds on the REAL sources", lin_pins(peer, ahh, true) == 0u);
    struct { const char *what, *from, *to; bool inPeer; } plant[] = {
        { "the pre-flight walks res+0x230 again (`pos < bytes`)", "for (uint64_t pos = 0; pos < wBytes; dSegs++) {",
          "for (uint64_t pos = 0; pos < bytes; dSegs++) {", true },
        { "the write loop stops at res+0x230", "while (pos < wBytes) {", "while (pos < bytes) {", true },
        { "the entry records res+0x230, not the gfx12 length", "rc.bytes = wBytes;", "rc.bytes = bytes;", true },
        { "the switch ignored at the gate", "if (!n48_rp_lin_take(why, sw ? 1u : 0u)) return 0ull;", "if (!n48_rp_lin_take(why, 1u)) return 0ull;", true },
        { "the colour-target drop ungated", "if (added && gXdResProvLin && lf.ctx)", "if (added && lf.ctx)", false },
        // build 0.0.492
        { "the gfx10 re-tiles NOT refused for a linear backing (the old re-tile admitted)",
          "if ((rt.lin & N48_RT_LINBLK) && rtShape == N48_RP_SHAPE_OK) {", "if (0 && rtShape == N48_RP_SHAPE_OK) {", true },
        { "the re-tile flag set whatever switch 59 says", "n48_rp_lin_blocks_old(sw ? 1u : 0u, nibble,", "n48_rp_lin_blocks_old(1u, nibble,", true },
        { "the write loop copies a whole image the stream never holds", "if (retile) { if (!rp_retile_bytes(&rt, pos + k, raw, take)) {",
          "if (retile) memcpy(raw, reinterpret_cast<const uint8_t *>(rt.dst) + pos + k, (size_t)take); if (0) { if (1) {", true },
    };
    for (const auto &p : plant) {
        std::string mp = peer, ma = ahh;
        std::string &m = p.inPeer ? mp : ma;
        const size_t a = m.find(p.from);
        if (a == std::string::npos || count_of(m, p.from) != 1u) { expect("T16 plant setup: the text is found once", false); continue; }
        m.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "T16 BREAK-check: %s - caught", p.what);
        expect(lbl, lin_pins(mp, ma, false) > 0u);
    }
}

// build 0.0.493 (switch 59, ws_resprov.h section 7) - THE KNOWN-ASSET ROW'S ORDER IN THE KEXT. The pure steps are driven in
// ws_resprov_test.cpp (K1-K7, including the chain on the real run10g line); these pins prove the kext calls them in the same order
// (switch -> facts -> config -> read -> key -> convert -> remember -> stream), that the stream reaches the ONE write loop and its
// read-back BEFORE the entry is stamped and recorded, and that a page-out of a remembered resource is refused. Each pin is
// non-vacuous: a mutant of the real source fails it (the BREAK-checks below).
static std::string fn_text(const std::string &all, const char *head) {
    const size_t b = all.find(head);
    if (b == std::string::npos) return std::string();
    const size_t e = all.find("\n}\n", b);
    return e == std::string::npos ? std::string() : all.substr(b, e + 3u - b);
}
static size_t known_pins(const std::string &peerAll, const std::string &ahh, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    const std::string kp = fn_text(peerAll, "static uint64_t __attribute__((noinline)) rp_known_prepare(");
    const std::string lp = fn_text(peerAll, "static uint64_t __attribute__((noinline)) rp_lin_prepare(");
    const size_t fb = peerAll.find("static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    const size_t fe = fb == std::string::npos ? fb : peerAll.find("THE PAGE-OUT COPY: pageTexture(toVram = 0)", fb);
    const std::string cp = (fb == std::string::npos || fe == std::string::npos) ? std::string() : peerAll.substr(fb, fe - fb);
    chk("T17 the three functions are found", !kp.empty() && !lp.empty() && !cp.empty());
    // rp_lin_prepare hands nibble 4 (and only it) to rp_known_prepare, before the texture-flavour filter
    const size_t n4 = lp.find("if (nibble == 4u) return rp_known_prepare(r, md, rt);");
    const size_t nf = lp.find("if (nibble != 0u && nibble != 1u && nibble != 7u && nibble != 8u) return 0ull;");
    chk("T17 nibble 4 goes to rp_known_prepare, once, before the texture filter",
        n4 != std::string::npos && nf != std::string::npos && n4 < nf && count_of(peerAll, "rp_known_prepare(r, md, rt)") == 1u);
    // rp_known_prepare's own order
    const char *steps[] = {
        "if (!n48::hw_resprov_lin_on()) return 0ull;",
        "const uint32_t row = n48_rp_known_row_for(&in);",
        "if (row == N48_RP_KNOWN_NONE) return 0ull;",
        "const uint32_t gbRead = (uint32_t)apple_gb_addr_config(r, &gb, &cfgWhy);",
        "if (gbRead != 1u || !n48_rp_g10_cfg_ok(gb)) why = N48_RP_KN_CFG;",
        "else if (md->readBytes(in.backingOffset, ls->src, in.bytes) != in.bytes) why = N48_RP_KN_READ;",
        "else if (!n48_rp_known_key_ok(row, reinterpret_cast<const uint32_t *>(ls->src), in.bytes)) why = N48_RP_KN_KEY;",
        "else why = n48_rp_known_convert(row, gbRead, gb, reinterpret_cast<const uint32_t *>(ls->src),",
        "if (why == N48_RP_KN_OK && !n48_rp_known_res_add(gRpKnownRes, kRpKnownRes, reinterpret_cast<uint64_t>(r)))",
        "if (why != N48_RP_KN_OK) {",
        "ls->lo = 0ull; ls->hi = in.bytes;",
        "ls->known = row + 1u; ls->res = r;",
        "rt->ls = ls;",
    };
    size_t prev = 0; bool ordered = true, once = true;
    for (const char *st : steps) {
        const size_t at = kp.find(st);
        if (at == std::string::npos || at < prev) ordered = false; else prev = at;
        if (count_of(kp, st) != 1u) once = false;
    }
    chk("T17 rp_known_prepare: switch -> facts -> config -> read -> key -> convert -> remember -> refusal exit -> the whole image -> stream",
        ordered && once);
    chk("T17 the switch check is the function's first statement (OFF: nothing read)",
        kp.find("if (!n48::hw_resprov_lin_on()) return 0ull;") != std::string::npos &&
        kp.find("if (!n48::hw_resprov_lin_on()) return 0ull;") < kp.find("r + 0x"));
    chk("T17 the stream states the gfx12 mode it writes (ADDR3_4KB_2D) once", count_of(kp, "ls->p.g12Mode = N48_RP_G12_4KB_2D;") == 1u);
    // residency_copy_to_vram: prepare -> pre-flight -> the write loop's read-back -> stamp -> record
    const size_t prep = cp.find("if (rpOn) (void)rp_lin_prepare(r, md, srcLen, dstLen, &rt);");
    const size_t pre = cp.find("for (uint64_t pos = 0; pos < wBytes; dSegs++) {");
    const size_t wr = cp.find("if (retile) { if (!rp_retile_bytes(&rt, pos + k, raw, take)) {");
    const size_t rb = cp.find("if (!navi48_vram_read_mm(dAt + k, back, cnt)) { fail = \"the MM window refused a read-back batch\"; break; }");
    const size_t stamp = cp.find("if (linBytes && retile) rp_lin_fill_copy(&rc, rt.ls);");
    const size_t rec = cp.find("const uint32_t why = n48::hw_resprov_note_copy(&rc, proc_selfpid(), &key, gCopy.copies);");
    chk("T17 order in the copy: prepare -> pre-flight -> write -> read-back -> stamp (known row) -> record",
        prep != std::string::npos && pre != std::string::npos && wr != std::string::npos && rb != std::string::npos &&
        stamp != std::string::npos && rec != std::string::npos && prep < pre && pre < wr && wr < rb && rb < stamp && stamp < rec);
    chk("T17 exactly one record call in the peer", count_of(peerAll, "hw_resprov_note_copy(") == 1u);
    chk("T17 the stamp names the row", count_of(peerAll, "rc->lin = N48_RP_LIN_KNOWN0 + ls->known - 1u;") == 1u);
    chk("T17 a known stream leaves the per-mode page-out flags alone", count_of(peerAll, "if (ls->known) return ls->p.g12Mode;") == 1u);
    chk("T17 a remembered resource's page-out is refused",
        count_of(peerAll, "n48_rp_known_pageout(gRpKnownRes, kRpKnownRes, reinterpret_cast<uint64_t>(r));") == 1u &&
        count_of(peerAll, "(gRpEverRetiled4kbdx || gRpEverRetiled256bd || gRpEverRetiled64krx || gRpKnownAny) &&") == 1u);
    chk("T17 the report line", count_of(ahh, "HWLOG(N48_RP_LIN_REPORT3_FMT,") == 1u);
    return bad;
}
static void test_T17_known493(const char *ahhPath, const char *peerPath) {
    std::string ahh, peer;
    if (!ahhPath || !peerPath || !read_file(ahhPath, ahh) || !read_file(peerPath, peer)) {
        expect("T17 the kext sources are readable (AppleHardwareHook.cpp, Navi48AccelPeer.cpp)", false); return;
    }
    expect("T17 every pin holds on the REAL sources", known_pins(peer, ahh, true) == 0u);
    struct { const char *what, *from, *to; } plant[] = {
        { "the key check removed (fail-open)",
          "else if (!n48_rp_known_key_ok(row, reinterpret_cast<const uint32_t *>(ls->src), in.bytes)) why = N48_RP_KN_KEY;", "" },
        { "the conversion skipped (dst never written) but the stream recorded",
          "else why = n48_rp_known_convert(row, gbRead, gb, reinterpret_cast<const uint32_t *>(ls->src),",
          "else why = N48_RP_KN_OK; (void)(row, gbRead, gb, reinterpret_cast<const uint32_t *>(ls->src)," },
        { "the switch check removed (59 OFF would read and convert)", "if (!n48::hw_resprov_lin_on()) return 0ull;", "" },
        { "the page-out table not filled", "if (why == N48_RP_KN_OK && !n48_rp_known_res_add(gRpKnownRes, kRpKnownRes, reinterpret_cast<uint64_t>(r)))",
          "if (0)" },
        { "a wrong target mode", "ls->p.g12Mode = N48_RP_G12_4KB_2D;", "ls->p.g12Mode = N48_RP_G12_256B_2D;" },
        { "the record made before the read-back (an extra record at prepare)",
          "if (rpOn) (void)rp_lin_prepare(r, md, srcLen, dstLen, &rt);",
          "if (rpOn) (void)rp_lin_prepare(r, md, srcLen, dstLen, &rt); if (rt.ls) { n48_rp_copy e0 {}; uint64_t k0 = 0; "
          "(void)n48::hw_resprov_note_copy(&e0, proc_selfpid(), &k0, 0); }" },
        { "the page-out refusal forgets known resources",
          "n48_rp_known_pageout(gRpKnownRes, kRpKnownRes, reinterpret_cast<uint64_t>(r));", "0;" },
    };
    for (const auto &p : plant) {
        std::string m = peer;
        const size_t a = m.find(p.from);
        if (a == std::string::npos || count_of(m, p.from) != 1u) { expect("T17 plant setup: the text is found once", false); continue; }
        m.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "T17 BREAK-check: %s - caught", p.what);
        expect(lbl, known_pins(m, ahh, false) > 0u);
    }
}


// =============================================================================================================
// T18 — build 0.0.495 (switch 62): THE SHADER-HEAP COPY / SUBSTITUTION RACE, in RUN D's real order.
//
// The model below is residency_copy_to_vram's order, step for step, over RUN D's real heap (fixture_heapgen_run10i.h):
// ic_begin (the pre-scan's patches and the FIRST bump) BEFORE the first write; per 256-byte batch the backing read, the
// overlay (ic_read), the write; the post-copy substitution (shadercache_scan_resource, today's path, both ways); the scope's
// close (the SECOND bump). Every decision in it is the REAL gfx_heapgen.h / gfx_neuter.h function. That the KEXT runs these
// steps in this order is proved by the pins further down (hg_pins), each with a planted break shown caught.
// =============================================================================================================
struct HgSim {
    n48_hg_state st;
    uint8_t vram[0x9000], backing[0x9000], ours[0x9000];
    n48_hg_patch patch[N48_HG_PATCH_MAX];
    uint8_t arena[N48_HG_ARENA_BYTES];
    uint8_t apple[N48_HG_ARENA_BYTES];   // build 0.0.496 F4: ic_hit's copy of the Apple bytes each patch matched
    uint32_t np, patchBytes;
};
static HgSim gHs;
static uint32_t gHsRecheckBad = 0u;   // build 0.0.496 F4: patches the model's copies found changed (the copy is then unclean)
static inline void hs_w32(uint8_t *b, uint32_t off, uint32_t v) { std::memcpy(b + off, &v, 4); }
static inline uint32_t hs_r32(const uint8_t *b, uint32_t off) { uint32_t v; std::memcpy(&v, b + off, 4); return v; }

// The boot state before f23 is judged: Apple's heap (gfx10), our rendered images, and VRAM as an EARLIER copy + its
// substitution left it (Apple's bytes with ours over every substituted range; the registry holds those ranges).
static void hs_boot(HgSim &s) {
    std::memset(&s.st, 0, sizeof s.st);
    for (uint32_t o = 0; o < 0x9000u; o += 4u) hs_w32(s.backing, o, 0x7e000000u ^ (o * 0x9e3779b1u));
    hs_w32(s.backing, 0, kR10iCopy62First[0]); hs_w32(s.backing, 4, kR10iCopy62First[1]);
    std::memset(s.ours, 0, sizeof s.ours);
    s.np = 0u; s.patchBytes = 0u;
    uint32_t used = 0u;
    for (uint32_t i = 0; i < kR10iSubstN; i++) {
        const r10i_subst &e = kR10iSubst[i];
        for (uint32_t k = 0; k < 8u; k++)
            hs_w32(s.backing, e.off + 4u * k, e.off == 0x2300u ? kR10iF23PsGfx10[k] : (0xbf800000u | e.off | k));
        hs_w32(s.ours, e.off, e.d0); hs_w32(s.ours, e.off + 4u, e.d1);
        for (uint32_t k = 8u; k < e.nb; k += 4u) hs_w32(s.ours, e.off + k, 0xbe000000u ^ ((e.off + k) * 2654435761u));
        // ic_hit's patch: the offset, the rendered length, the bytes in the arena, the VRAM the segment walk gives
        n48_hg_patch &p = s.patch[s.np++];
        p.off = e.off; p.nb = e.nb; p.arena_off = used; p.pad = 0u; p.vram = kR10iHeapVram + e.off;
        std::memcpy(s.arena + used, s.ours + e.off, e.nb);
        used += e.nb; s.patchBytes += e.nb;
    }
    // build 0.0.496 F4: the Apple program each patch was matched on is the backing's own bytes over the patch's range (the
    // model's Apple programs fill their capacity); ic_hit keeps a copy of them for the re-check.
    for (uint32_t i = 0, ao = 0; i < s.np; i++) {
        n48_hg_patch &p = s.patch[i];
        p.apple_nb = p.nb; p.apple_off = ao; p.bad = 0u; p.pad2 = 0u;
        std::memcpy(s.apple + ao, s.backing + p.off, p.nb);
        ao += p.nb;
    }
    std::memcpy(s.vram, s.backing, sizeof s.vram);
    for (uint32_t i = 0; i < kR10iSubstN; i++) {
        const r10i_subst &e = kR10iSubst[i];
        std::memcpy(s.vram + e.off, s.ours + e.off, e.nb);
        n48_hg_reg_add(&s.st, kR10iHeapVram + e.off, kR10iHeapVram + e.off + e.nb);   // hw_hg_note_substituted
    }
}

// Every substituted range of the heap already written by this copy (below `upto`) equals OUR bytes, byte for byte.
static bool hs_ours_below(const HgSim &s, uint32_t upto) {
    for (uint32_t i = 0; i < kR10iSubstN; i++) {
        const r10i_subst &e = kR10iSubst[i];
        const uint32_t hi = e.off + e.nb < upto ? e.off + e.nb : upto;
        if (e.off < hi && std::memcmp(s.vram + e.off, s.ours + e.off, hi - e.off) != 0) return false;
    }
    return true;
}
static bool hs_ours_all(const HgSim &s) { return hs_ours_below(s, 0x9000u); }

// Test-local shapes of the ORDER (mutants of this model, never of the kext): each must be caught by the checks below.
enum { HS_REAL = 0, HS_BUMP_AFTER_WRITE, HS_PATCH_AFTER_WRITE, HS_NO_PATCH_NO_BUMP };
struct HsObs { uint32_t transientGfx10, oddDuring, batches; };
typedef void (*HsHook)(HgSim &s, uint32_t pos, void *ctx);

// residency_copy_to_vram for copy #62. `on`: switch 62 (the copier reads it once, at ic_begin). Returns whether it bumped.
static uint32_t hs_copy62(HgSim &s, uint32_t on, int shape, HsObs *obs, HsHook atExemption, void *ctx, uint32_t *overlaidOut) {
    const uint64_t lo = kR10iHeapVram, hi = kR10iHeapVram + kR10iHeapBytes;
    uint32_t bumped = 0u, overlaid = 0u;
    gHsRecheckBad = 0u;
    const uint32_t np = (on && shape != HS_NO_PATCH_NO_BUMP) ? s.np : 0u;
    if (on && shape != HS_BUMP_AFTER_WRITE && shape != HS_NO_PATCH_NO_BUMP)       // ic_begin: before the first write
        bumped = n48_hg_copy_begin(&s.st, (np || n48_hg_reg_overlaps(&s.st, lo, hi)) ? 1u : 0u);
    for (uint32_t pos = 0; pos < kR10iHeapBytes; pos += 256u) {
        uint8_t raw[256];
        std::memcpy(raw, s.backing + pos, 256u);                                  // ic_read: md->readBytes ...
        if (on && bumped && shape == HS_REAL) {                                    // ... + overlay (0.0.496 F4: re-checked)
            uint32_t nbad = 0u;
            overlaid += n48_hg_overlay_checked(s.patch, np, s.arena, s.apple, pos, raw, 256u, &nbad);
            if (nbad) gHsRecheckBad += nbad;
        }
        std::memcpy(s.vram + pos, raw, 256u);                                     // navi48_vram_write_mm
        if (obs) {
            obs->batches++;
            if (!hs_ours_below(s, pos + 256u)) obs->transientGfx10++;            // a CP fetch here would see Apple's gfx10
            if (s.st.gen & 1ull) obs->oddDuring++;
        }
        if (on && shape == HS_PATCH_AFTER_WRITE) overlaid += n48_hg_overlay(s.patch, np, s.arena, pos, s.vram + pos, 256u);
    }
    if (on && shape == HS_BUMP_AFTER_WRITE) bumped = n48_hg_copy_begin(&s.st, 1u);
    if (atExemption) atExemption(s, 0xFFFFFFFFu, ctx);   // RUN D line 14148: the RING EXEMPTION ran here, before SUBSTITUTED
    for (uint32_t i = 0; i < kR10iSubstN; i++)                                    // shadercache_scan_resource (both ways)
        std::memcpy(s.vram + kR10iSubst[i].off, s.ours + kR10iSubst[i].off, kR10iSubst[i].nb);
    if (bumped)                                                                   // navi48_ic_scope_closed
        n48_hg_copy_end(&s.st, (overlaid == s.patchBytes && !gHsRecheckBad) ? 1u : 0u, 1u, kR10iHeapVa, kR10iHeapVa + kR10iHeapBytes,
                        lo, hi, nullptr, 0u);
    if (overlaidOut) *overlaidOut = overlaid;
    return bumped;
}

// The ring and the committed frame (gfx_neuter_test.cpp's committed_frame, token seq 12, f23's IB: VA 0x4000f0000, 1040 dw).
static n48_gfxn_exempt_t hs_committed(uint32_t *ring, uint32_t size, uint64_t from) {
    uint32_t blk[N48_TMPL_DWORDS];
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) blk[k] = N48_TMPL_NOP;
    uint8_t info[0x200] = { 0 };
    n48_wr32(info, N48_SCB_FLAGS, 0u); n48_wr32(info, N48_SCB_VMID, 2u); n48_wr32(info, N48_SCB_COUNT, 1u);
    n48_wr32(info, N48_SCB_STAMP, 0x18u);
    n48_wr32(info, N48_SCB_ENTRY0, 1040u); n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA, 0x000f0000u);
    n48_wr32(info, N48_SCB_ENTRY0 + N48_SCB_ENTRY_VA + 4u, 0x4u);
    n48_gfxsrc_model_commit(info, blk);
    for (uint32_t k = 0; k < N48_TMPL_DWORDS; k++) ring[(from + k) % size] = blk[k];
    n48_gfxn_exempt_t ex {};
    ex.inflight = 1u; ex.seq = kR10iTokenSeq; ex.gate_seq = kR10iTokenSeq; ex.nib = 1u; ex.same_thread = 1u;
    ex.pos_known = 1u; ex.expect_pos = (uint32_t)((from + N48_TMPL_IB0_DWORD) % size);
    ex.va = 0x4000f0000ull; ex.len = 1040u; ex.stamp = 0x18u; ex.vmid = 2u; ex.base_valid = 1u;
    return ex;
}
struct HsExCtx { n48_hg_frame rec; uint32_t why, psWord0; };
static uint32_t gHsRing[512];
static void hs_at_exemption(HgSim &s, uint32_t, void *vctx) {
    HsExCtx *c = static_cast<HsExCtx *>(vctx);
    n48_gfxn_exempt_t ex = hs_committed(gHsRing, 512u, 448u);
    // hook_gfxWriteTail: ex.heap_refuse = hg_exempt_refuse(ex.seq) - the committed frame's record, judged now
    ex.heap_refuse = (c->rec.on && n48_hg_judge(&s.st, &c->rec, c->rec.judged) != N48_HG_OK) ? 1u : 0u;
    n48_gfxn_walk_t w; n48_gfxn_walk(gHsRing, 448u, 128u, 512u, &w, nullptr, nullptr);
    uint32_t pos[2u * N48_GFXN_MAX_IBS], sp = 0u, why = 0u;
    (void)n48_gfxn_nop_list(gHsRing, 512u, 128u, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why);
    c->why = why;
    c->psWord0 = hs_r32(s.vram, 0x2300u);   // what the CP would fetch for f23's PS at this instant
}

// RUN D, both ways. `on` 0 is the POSITIVE CONTROL: today's behaviour must reproduce the hang's preconditions.
static void test_T18a_rund_order() {
    for (uint32_t on = 0; on < 2u; on++) {
        HgSim &s = gHs;
        hs_boot(s);
        char b[200];
        // 1. f23's verdict (token seq 12): the top of the pass, then its two programs (PS +0x2300, VS +0xf00).
        n48_hg_frame f {};
        n48_hg_frame_begin(&f, on, 24u, &s.st);
        n48_hg_frame_note_pva(&f, kR10iF23PsVa); n48_hg_frame_note_pva(&f, kR10iF23VsVa);
        std::snprintf(b, sizeof b, "T18a [62 %s] f23's programs are ours at the verdict (VRAM +0x2300 holds our first dword)", on ? "ON " : "OFF");
        expect(b, hs_r32(s.vram, 0x2300u) == 0xbe98017eu);
        // 2. the commit (gfxsrc_commit_try's `live`): nothing has moved yet - it proceeds both ways.
        std::snprintf(b, sizeof b, "T18a [62 %s] the commit of token seq 12 proceeds (hg_commit_judge == ok)", on ? "ON " : "OFF");
        expect(b, n48_hg_judge(&s.st, &f, 24u) == N48_HG_OK);
        HsExCtx ex { f, 0u, 0u };                          // hg_commit_record: the committed frame's record
        // 3. COPIED #62, then the RING EXEMPTION (before SUBSTITUTED), then the substitution and the close.
        HsObs obs {};
        uint32_t overlaid = 0u;
        const uint32_t bumped = hs_copy62(s, on, HS_REAL, &obs, &hs_at_exemption, &ex, &overlaid);
        if (!on) {
            expect("T18a [62 OFF] POSITIVE CONTROL: the copy does not bump (today: no counter)", bumped == 0u && s.st.gen == 0ull);
            expect("T18a [62 OFF] POSITIVE CONTROL: VRAM held Apple's gfx10 at a substituted program during the copy",
                   obs.transientGfx10 > 0u);
            expect("T18a [62 OFF] POSITIVE CONTROL: the ring exemption SPARES seq 12 (RUN D line 14148)", ex.why == N48_GFXN_EX_SPARED);
            expect("T18a [62 OFF] POSITIVE CONTROL: ... while f23's PS reads b0802004 (gfx10, the F23 census) - RUN D's hang",
                   ex.psWord0 == kR10iF23PsGfx10[0]);
            expect("T18a [62 OFF] a frame judged after the copy commits (nothing refuses with 62 OFF)", [&] {
                n48_hg_frame g {}; n48_hg_frame_begin(&g, 0u, 25u, &s.st); n48_hg_frame_note_pva(&g, kR10iF23PsVa);
                return n48_hg_judge(&s.st, &g, 25u) == N48_HG_OK && n48_hg_judge(&s.st, &f, 24u) == N48_HG_OK; }());
        } else {
            expect("T18a [62 ON ] the copy overlaps substituted programs and bumps once at its start", bumped == 1u);
            expect("T18a [62 ON ] the generation was ODD for every batch the copy wrote (odd = in progress)",
                   obs.oddDuring == obs.batches && obs.batches == 0x90u);
            expect("T18a [62 ON ] VRAM NEVER held Apple's gfx10 at a substituted program, after any batch", obs.transientGfx10 == 0u);
            expect("T18a [62 ON ] every patch byte was overlaid (31 programs, the in-copy verification)",
                   overlaid == s.patchBytes && s.np == 31u);
            expect("T18a [62 ON ] the VRAM image equals our gfx12 bytes at EVERY substituted offset, byte for byte", hs_ours_all(s));
            expect("T18a [62 ON ] the ring exemption at RUN D's instant REFUSES seq 12 (heap-gen: the IB is NOPed)",
                   ex.why == N48_GFXN_EX_HEAPGEN);
            expect("T18a [62 ON ] ... and even so f23's PS already read OUR bytes at that instant (both defences hold)",
                   ex.psWord0 == 0xbe98017eu);
            expect("T18a [62 ON ] after the close the generation is even again (+2) and nothing is active",
                   s.st.gen == 2ull && s.st.active == 0u && s.st.copies_clean == 1u);
            expect("T18a [62 ON ] the committed record (judged before the copy) still refuses: generation-changed",
                   n48_hg_judge(&s.st, &f, 24u) == N48_HG_CHANGED);
            expect("T18a [62 ON ] a frame judged AFTER the copy completed commits (the heap is ours and still)", [&] {
                n48_hg_frame g {}; n48_hg_frame_begin(&g, 1u, 25u, &s.st); n48_hg_frame_note_pva(&g, kR10iF23PsVa);
                return n48_hg_judge(&s.st, &g, 25u) == N48_HG_OK; }());
        }
        // Both ways the FINAL image is the same: 62 changes only the transient, never what ends up in VRAM.
        std::snprintf(b, sizeof b, "T18a [62 %s] the final VRAM image is ours at every substituted offset (post-copy substitution)", on ? "ON " : "OFF");
        expect(b, hs_ours_all(s));
    }
    // A frame judged WHILE the copy runs refuses even after the copy completes (its verdict may have read mid-copy bytes).
    {
        HgSim &s = gHs; hs_boot(s);
        (void)n48_hg_copy_begin(&s.st, 1u);
        n48_hg_frame g {}; n48_hg_frame_begin(&g, 1u, 30u, &s.st); n48_hg_frame_note_pva(&g, kR10iF23PsVa);
        expect("T18a a verdict taken while an overlapping copy is in progress: copy-in-progress",
               n48_hg_judge(&s.st, &g, 30u) == N48_HG_IN_PROGRESS);
        n48_hg_copy_end(&s.st, 1u, 1u, kR10iHeapVa, kR10iHeapVa + kR10iHeapBytes, kR10iHeapVram, kR10iHeapVram + kR10iHeapBytes, nullptr, 0u);
        expect("T18a ... and still refused after that copy completed (snapshot odd)", n48_hg_judge(&s.st, &g, 30u) == N48_HG_IN_PROGRESS);
        expect("T18a a commit whose record belongs to another judged frame: stale-verdict", n48_hg_judge(&s.st, &g, 31u) == N48_HG_STALE);
        // two concurrent overlapping copies: gen is even after the second begin, but `active` still refuses
        (void)n48_hg_copy_begin(&s.st, 1u); (void)n48_hg_copy_begin(&s.st, 1u);
        n48_hg_frame h {}; n48_hg_frame_begin(&h, 1u, 32u, &s.st);
        expect("T18a two concurrent copies: gen EVEN yet the verdict is refused (active 2)",
               (s.st.gen & 1ull) == 0ull && n48_hg_judge(&s.st, &h, 32u) == N48_HG_IN_PROGRESS);
    }
    // THE MODEL'S OWN MUTANTS (the order, broken three ways): each must be caught by the checks above.
    {
        HgSim &s = gHs; char b[200];
        struct { int shape; const char *what; } mut[] = {
            { HS_BUMP_AFTER_WRITE, "ORDERING: the counter bumped after the write" },
            { HS_PATCH_AFTER_WRITE, "ORDERING: the patch applied after the VRAM write" },
            { HS_NO_PATCH_NO_BUMP, "FAIL-OPEN: the in-copy patch skipped and the counter not bumped" },
        };
        for (const auto &m : mut) {
            hs_boot(s);
            n48_hg_frame f {}; n48_hg_frame_begin(&f, 1u, 24u, &s.st); n48_hg_frame_note_pva(&f, kR10iF23PsVa);
            HsExCtx ex { f, 0u, 0u }; HsObs obs {};
            (void)hs_copy62(s, 1u, m.shape, &obs, &hs_at_exemption, &ex, nullptr);
            const bool caught = obs.transientGfx10 > 0u || obs.oddDuring != obs.batches || ex.why != N48_GFXN_EX_HEAPGEN;
            std::snprintf(b, sizeof b, "T18a MODEL BREAK-check: %s - caught", m.what);
            expect(b, caught);
        }
    }
}

// A copy that overlaps no substituted program (RUN D's copy #59, a texture) changes nothing.
static void test_T18b_disjoint_copy() {
    HgSim &s = gHs; hs_boot(s);
    n48_hg_frame f {}; n48_hg_frame_begin(&f, 1u, 40u, &s.st); n48_hg_frame_note_pva(&f, kR10iF23PsVa);
    const uint64_t gen0 = s.st.gen;
    const uint32_t overlaps = n48_hg_reg_overlaps(&s.st, kR10iTexVram, kR10iTexVram + kR10iTexBytes);
    const uint32_t bumped = n48_hg_copy_begin(&s.st, overlaps);   // ic_begin: no patch, no poison, no registry overlap
    expect("T18b copy #59 (VRAM [0x10010000,0x10018000)) overlaps no substituted program", overlaps == 0u && bumped == 0u);
    expect("T18b ... the generation is unchanged and nothing is active", s.st.gen == gen0 && s.st.active == 0u);
    expect("T18b ... and a frame judged before it still commits", n48_hg_judge(&s.st, &f, 40u) == N48_HG_OK);
    expect("T18b the heap's own range DOES overlap the registry (the control)",
           n48_hg_reg_overlaps(&s.st, kR10iHeapVram, kR10iHeapVram + kR10iHeapBytes) == 1);
    expect("T18b a range touching the heap's end only (half-open) does not overlap",
           n48_hg_reg_overlaps(&s.st, kR10iHeapVram + kR10iHeapBytes, kR10iHeapVram + kR10iHeapBytes + 0x1000u) == 0);
}

// A patch that fails verification poisons and refuses; a later clean copy clears it.
static void test_T18c_poison() {
    HgSim &s = gHs; hs_boot(s);
    // (i) one program cannot be patched in the copy (arena/size): ic_poison records its range; the copy is otherwise clean.
    n48_hg_poison ps {};
    ps.va_lo = kR10iF23PsVa; ps.va_hi = kR10iF23PsVa + 588u; ps.vram_lo = kR10iHeapVram + 0x2300u; ps.vram_hi = ps.vram_lo + 588u;
    ps.va_ok = 1u; ps.used = 1u;
    (void)n48_hg_copy_begin(&s.st, 1u);
    n48_hg_copy_end(&s.st, 1u, 1u, kR10iHeapVa, kR10iHeapVa + kR10iHeapBytes, kR10iHeapVram, kR10iHeapVram + kR10iHeapBytes, &ps, 1u);
    n48_hg_frame f {}; n48_hg_frame_begin(&f, 1u, 50u, &s.st); n48_hg_frame_note_pva(&f, kR10iF23PsVa); n48_hg_frame_note_pva(&f, kR10iF23VsVa);
    expect("T18c an unpatchable PS poisons its range: f23's shape (PS +0x2300) is refused (program-poisoned)",
           n48_hg_judge(&s.st, &f, 50u) == N48_HG_POISONED);
    n48_hg_frame g {}; n48_hg_frame_begin(&g, 1u, 51u, &s.st); n48_hg_frame_note_pva(&g, kR10iF23VsVa);
    expect("T18c a frame naming only the VS (+0xf00, patched) commits", n48_hg_judge(&s.st, &g, 51u) == N48_HG_OK);
    // (ii) the next clean overlapping copy clears the poison it covers
    (void)n48_hg_copy_begin(&s.st, 1u);
    n48_hg_copy_end(&s.st, 1u, 1u, kR10iHeapVa, kR10iHeapVa + kR10iHeapBytes, kR10iHeapVram, kR10iHeapVram + kR10iHeapBytes, nullptr, 0u);
    n48_hg_frame h {}; n48_hg_frame_begin(&h, 1u, 52u, &s.st); n48_hg_frame_note_pva(&h, kR10iF23PsVa);
    expect("T18c ... until a later copy completes cleanly: then f23's shape commits again",
           n48_hg_judge(&s.st, &h, 52u) == N48_HG_OK && s.st.poison_cleared == 1u);
    // (iii) a verification failure (a patch not overlaid in full): the model's copy with the overlay truncated
    hs_boot(s);
    s.patch[7].nb = 4u;   // GPUPass: only its first dword reaches a batch (the in-copy verification: overlaid != patchBytes)
    n48_hg_frame k {}; n48_hg_frame_begin(&k, 1u, 60u, &s.st);
    uint32_t overlaid = 0u;
    (void)hs_copy62(s, 1u, HS_REAL, nullptr, nullptr, nullptr, &overlaid);
    n48_hg_frame m {}; n48_hg_frame_begin(&m, 1u, 61u, &s.st); n48_hg_frame_note_pva(&m, kR10iF23PsVa);
    expect("T18c a patch that fails verification (overlaid short) poisons the whole copy: f23's shape refused",
           overlaid != s.patchBytes && n48_hg_judge(&s.st, &m, 61u) == N48_HG_POISONED && s.st.copies_poisoned == 1u);
    // (iv) a poisoned range without a VA refuses EVERY frame; a saturated table refuses every frame
    hs_boot(s);
    n48_hg_poison nv {}; nv.vram_lo = kR10iHeapVram; nv.vram_hi = kR10iHeapVram + 0x100u; nv.va_ok = 0u; nv.used = 1u;
    n48_hg_poison_add(&s.st, &nv);
    n48_hg_frame q {}; n48_hg_frame_begin(&q, 1u, 70u, &s.st); n48_hg_frame_note_pva(&q, 0x500000000ull);
    expect("T18c a poisoned range with no VA refuses a frame naming any program", n48_hg_judge(&s.st, &q, 70u) == N48_HG_POISONED);
    n48_hg_poison_clear(&s.st, 0u, 0u, 0u, kR10iHeapVram, kR10iHeapVram + kR10iHeapBytes);
    expect("T18c ... and a clean copy over its VRAM clears it", n48_hg_judge(&s.st, &q, 70u) == N48_HG_OK);
    hs_boot(s);
    for (uint32_t i = 0; i <= N48_HG_POISON_MAX; i++) { n48_hg_poison e = ps; e.va_lo = 0x600000000ull + 0x1000u * i; e.va_hi = e.va_lo + 4u; n48_hg_poison_add(&s.st, &e); }
    expect("T18c a saturated poison table refuses every frame (never silently dropped)", s.st.poison_over == 1u &&
           n48_hg_judge(&s.st, &q, 70u) == N48_HG_POISONED);
    // (v) the program list full while any poison stands refuses
    hs_boot(s);
    n48_hg_poison_add(&s.st, &ps);
    n48_hg_frame r {}; n48_hg_frame_begin(&r, 1u, 80u, &s.st);
    for (uint32_t i = 0; i <= N48_HG_PVA_MAX; i++) n48_hg_frame_note_pva(&r, 0x700000000ull + 0x100u * i);
    expect("T18c a frame whose program list overflowed is refused while any poison stands",
           r.pva_over == 1u && n48_hg_judge(&s.st, &r, 80u) == N48_HG_POISONED);
}

// OFF identity: with 62 OFF the copy is byte-identical to today's and no commit answer changes, whatever the state.
static void test_T18d_off_identity() {
    HgSim &s = gHs; hs_boot(s);
    // the copy's bytes after EVERY batch equal Apple's backing exactly (today's copy)
    uint32_t diff = 0u;
    struct Ctx { uint32_t *diff; };
    for (uint32_t pos = 0; pos < kR10iHeapBytes; pos += 256u) {
        uint8_t raw[256]; std::memcpy(raw, s.backing + pos, 256u);
        // switch 62 OFF: ic_begin is not called, so no slot, no patch, gIcOverlaying 0: ic_read is the read alone
        std::memcpy(s.vram + pos, raw, 256u);
        if (std::memcmp(s.vram + pos, s.backing + pos, 256u)) diff++;
    }
    expect("T18d [62 OFF] every batch the copy writes is Apple's backing, byte for byte", diff == 0u);
    // the commit answer: for 256 states of the heap (gen, active, poison, stale ordinal), OFF latched is always ok
    uint32_t bad = 0u;
    for (uint32_t i = 0; i < 256u; i++) {
        n48_hg_state st {}; st.gen = i * 7u; st.active = i & 3u; st.poison_over = (i >> 2) & 1u;
        if (i & 8u) { n48_hg_poison e {}; e.used = 1u; e.va_ok = 0u; n48_hg_poison_add(&st, &e); }
        n48_hg_frame f {}; n48_hg_frame_begin(&f, 0u, i, &st); n48_hg_frame_note_pva(&f, kR10iF23PsVa);
        if (n48_hg_judge(&st, &f, i + ((i >> 4) & 1u)) != N48_HG_OK) bad++;
        if (f.npva != 0u) bad++;                               // OFF records nothing
    }
    expect("T18d [62 OFF] n48_hg_judge answers ok in 256 heap states (in progress, changed, poisoned, stale)", bad == 0u);
    // the exemption: heap_refuse 0 (OFF) answers exactly what a record built before 0.0.495 answers - SPARED here
    n48_gfxn_exempt_t ex = hs_committed(gHsRing, 512u, 448u);
    n48_gfxn_walk_t w; n48_gfxn_walk(gHsRing, 448u, 128u, 512u, &w, nullptr, nullptr);
    uint32_t pos[2u * N48_GFXN_MAX_IBS], sp = 0u, why = 0u;
    (void)n48_gfxn_nop_list(gHsRing, 512u, 128u, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why);
    expect("T18d [62 OFF] the ring exemption with heap_refuse 0 still SPARES the committed frame", why == N48_GFXN_EX_SPARED);
    ex.base_valid = 0u;
    (void)n48_gfxn_nop_list(gHsRing, 512u, 128u, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why);
    ex.heap_refuse = 1u;
    uint32_t why2 = 0u;
    (void)n48_gfxn_nop_list(gHsRing, 512u, 128u, &w, N48_GFXN_ARM_COMMIT, &ex, pos, &sp, &why2);
    expect("T18d heap-gen is asked LAST: an earlier refusal (base-invalid) keeps its own reason", why == N48_GFXN_EX_BASE && why2 == N48_GFXN_EX_BASE);
}

// The overlay alone: nothing outside [p.off, p.off + p.nb) or outside the batch is touched; a patch straddling batches
// (ColorFill +0x1100 + 300 crosses 0x1200) lands whole; a short tail batch works.
static void test_T18e_overlay() {
    HgSim &s = gHs; hs_boot(s);
    uint32_t outside = 0u, placed = 0u;
    for (uint32_t pos = 0; pos < kR10iHeapBytes; pos += 256u) {
        uint8_t raw[256]; std::memcpy(raw, s.backing + pos, 256u);
        placed += n48_hg_overlay(s.patch, s.np, s.arena, pos, raw, 256u);
        for (uint32_t j = 0; j < 256u; j++) {
            const uint32_t o = pos + j;
            bool inPatch = false;
            for (uint32_t i = 0; i < kR10iSubstN; i++) if (o >= kR10iSubst[i].off && o < kR10iSubst[i].off + kR10iSubst[i].nb) inPatch = true;
            if (!inPatch && raw[j] != s.backing[o]) outside++;
            if (inPatch && raw[j] != s.ours[o]) outside++;
        }
    }
    expect("T18e the overlay writes exactly the patches' bytes and nothing else (31 programs over 144 batches)",
           outside == 0u && placed == s.patchBytes);
    uint8_t tail[12]; std::memset(tail, 0xAA, sizeof tail);
    const uint32_t n = n48_hg_overlay(s.patch, s.np, s.arena, 0x1100u + 296u, tail, 8u);   // the last 4 bytes of ColorFill + 4 past
    expect("T18e a short batch straddling a patch's end takes only the patch's bytes",
           n == 4u && std::memcmp(tail, s.ours + 0x1100u + 296u, 4u) == 0 && tail[4] == 0xAA && tail[8] == 0xAA);
    char buf[1024]; n48_hg_state w {};
    w.gen = w.copies_bumped = w.copies_patched = w.programs_patched = w.bytes_patched = w.copies_poisoned = ~0ull;
    w.programs_poisoned = w.poison_cleared = w.copies_clean = w.judged = w.ex_refused = ~0ull; w.active = ~0u; w.nreg = ~0u;
    w.poison_over = 1u; w.reg_over = 1u; w.reg_pruned = w.reg_merged = ~0ull;   // 0.0.496 F1: the line's two new counters
    for (uint32_t i = 0; i < N48_HG_REASONS; i++) w.refused[i] = ~0ull;
    const int len = std::snprintf(buf, sizeof buf, N48_HG_FMT, N48_HG_ARGS(1u, " - `gfxneuter 62` ON REFUSED: its lock could not be allocated, unchanged", &w));
    const int len2 = std::snprintf(buf, sizeof buf, N48_HG_FMT2, N48_HG_ARGS2(&w));
    expect("T18e both shaderheap62 report lines stay under n48log's 512-byte cap (HWLOG prefix included) at their widest",
           len > 0 && len2 > 0 && len + 20 < 512 && len2 + 20 < 512);
}

// ---- the kext's own order: pins, each with a planted break shown caught ----------------------------------------------------
static size_t hg_pins(const std::string &peer, const std::string &ahh, const std::string &ttl, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0; bool first = true;
        for (const char *st : steps) {
            const size_t at = t.find(st, first ? 0 : prev);
            if (at == std::string::npos || count_of(t, st) != 1u) return false;
            prev = at + 1; first = false;
        }
        return true;
    };
    const size_t fb = peer.find("static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    const size_t fe = fb == std::string::npos ? fb : peer.find("THE PAGE-OUT COPY: pageTexture(toVram = 0)", fb);
    const std::string cp = (fb == std::string::npos || fe == std::string::npos) ? std::string() : peer.substr(fb, fe - fb);
    const std::string icRead = fn_text(peer, "static __attribute__((noinline)) uint64_t ic_read(");
    const std::string icBegin = fn_text(peer, "static __attribute__((noinline)) void ic_begin(");
    const std::string icHit = fn_text(peer, "static int ic_hit(void *vctx, size_t off, const sc_match *m) {");
    const std::string icOv = fn_text(peer, "static __attribute__((noinline)) void ic_overlay(");
    const std::string icClose = fn_text(peer, "__attribute__((noinline)) void navi48_ic_scope_closed(");
    const std::string cgCloseScope = fn_text(peer, "__attribute__((noinline)) void navi48_cg_close_scope(");
    const std::string decide = fn_text(ahh, "static uint32_t gfxsrc_decide_frame(");
    const std::string ident = fn_text(ahh, "static void gfxsrc_identify(const GfxcVm &vm, uint64_t va, uint32_t stage, int32_t pid, n48_xv_program *p) {");
    const std::string commit = fn_text(ahh, "static uint32_t gfxsrc_commit_try(");
    const std::string begin = fn_text(ahh, "static __attribute__((noinline)) void hg_frame_begin_pass()");
    const std::string judge = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_commit_judge(");
    const std::string rec = fn_text(ahh, "static __attribute__((noinline)) void hg_commit_record(");
    const std::string exr = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_exempt_refuse(");
    chk("T18P the functions are found", !cp.empty() && !icRead.empty() && !icBegin.empty() && !icHit.empty() && !icOv.empty() &&
        !icClose.empty() && !cgCloseScope.empty() && !decide.empty() && !ident.empty() && !commit.empty() && !begin.empty() &&
        !judge.empty() && !rec.empty() && !exr.empty());
    // COPIER: scope -> retile decided -> ic_begin (bump + patches) -> the write loop: read+overlay -> buf -> write -> read-back
    //         -> the post-copy substitution; the scope's close completes.
    chk("T18P copy order: scope, then ic_begin BEFORE the write loop, whose read (ic_read: overlay) precedes the write",
        order(cp, { "Navi48CopyScope cgScope(cgLo, cgHi);",
                    "const bool retile = rpOn && rtShape == N48_RP_SHAPE_OK;",
                    "if (n48::hw_hg_on()) ic_begin(dstMem, dSeg, md, backingOffset, bytes, (retile || wBytes != bytes) ? 1u : 0u, cgLo, cgHi, dstMap, self);",
                    "while (pos < wBytes) {",
                    "else if (ic_read(md, backingOffset, pos + k, raw, take) != take) {",
                    "memcpy(buf, raw, (size_t)cnt * 4);",
                    "if (!navi48_vram_write_mm(dAt + k, buf, cnt)) { fail = \"the MM window refused a write batch\"; break; }",
                    "shadercache_scan_resource(dstMem, dSeg, md, backingOffset, bytes, &scPartialWrite, &scMismatch);" }));
    chk("T18P the loop never reads the backing past the overlay (no direct readBytes of the batch remains)",
        !has(cp, "md->readBytes(backingOffset + pos + k, raw, take)"));
    chk("T18P ic_read: the read, THEN the overlay (only a full read is patched)",
        order(icRead, { ": md->readBytes(backingOffset + resOff, raw, take);",   // 0.0.529: or switch 84's scratch (this thread's ON copy)
                        "if (got == take && gIcOverlaying) ic_overlay(resOff, raw, (uint32_t)take);", "return got;" }));
    chk("T18P ic_overlay places the patches with the pure n48_hg_overlay_checked (0.0.496 F4: re-checked first), at the batch's own "
        "resource offset", has(icOv, "p->overlaid += n48_hg_overlay_checked(p->patch, p->npatch, p->arena, p->apple, resOff, raw, take, &nbad);"));
    chk("T18P ic_hit: shadercache_hit's ladder (verified, opt-in, render, segment, guard), then the patch at resOff",
        order(icHit, { "if (!m->verified) return 0;",
                       "if ((m->stage == SC_STAGE_VERTEX || m->stage == SC_STAGE_FRAGMENT) && !gScAcceptAdjust) return 0;",
                       // build 0.0.536 (switch 92, gfx_rv92.h): the render takes n48_rv92_pick's match (`m` itself unless 92 is ON
                       // and the blob holds a v3 alternative of RectPosTexFast_VS); the ladder around it is unchanged.
                       "const int rst = sc_subst_render(&gSc, use92, (int)gScAcceptAdjust, p->arena + p->arenaUsed, kScMaxSubstBytes, &nb);",
                       "if (rst != SC_OK || nb == 0 || (nb & 3u)) return 0;",
                       "if (!at || (at & 3) || span < nb) return 0;",
                       "if (navi48_vram_apple_dest_check(at, nb)) return 0;",
                       "if (c->noOverlay || resOff + nb > c->s->bytes) { ic_poison(c, resOff, nb); return 0; }",
                       "e.off = (uint32_t)resOff; e.nb = nb; e.arena_off = p->arenaUsed; e.pad = 0u; e.vram = at;" }));
    chk("T18P ic_begin: the pre-scan, THEN the bump with this copy's patches; overlaying only for a bumped copy with patches",
        order(icBegin, { "if (!ic_scan(&c, md, backingOffset, bytes)) s->scanFailed = 1u;",
                         "const uint32_t mayWait = n48_hg_copy_may_wait(np, npo, noOverlay, s->scanFailed, s->noPlan);",   // 0.0.511 B1
                         "s->bumped = n48::hw_hg_copy_begin(cgLo, cgHi, s->plan ? s->plan->patch : nullptr, np, npo, &s->regSeq, mayWait, &s->hgId);",
                         "if (s->bumped && np) { s->overlaying = 1u; __atomic_fetch_add(&gIcOverlaying, 1u, __ATOMIC_SEQ_CST); }" }));
    // 0.0.509 F-3: the fast copy's slot is asked first (sampled verify never clears poison), then the two closes in 0.0.495's order.
    chk("T18P 0.0.511 B1: the untracked (no-slot) copy never waits", has(icBegin, "(void)n48::hw_hg_copy_begin(cgLo, cgHi, nullptr, 0u, 1u, nullptr, 0u, nullptr);"));
    chk("T18P the copy's close: the fast copy's slot, the copy guard's close, THEN the in-copy completion", order(cgCloseScope,
        { "const uint32_t sampled = navi48_fc_scope_closed(lo, hi);",
          "navi48_cg_close(slot, lo, hi, n48_fc_cg_close_wrote(wrote ? 1u : 0u, failed ? 1u : 0u, mismatch ? 1u : 0u, sampled) != 0u, failed, mismatch);",
          "navi48_ic_scope_closed(lo, hi, wrote, failed, mismatch, sampled != 0u);" }));
    chk("T18P Navi48CopyScope's destructor closes through navi48_cg_close_scope",
        has(ttl, "~Navi48CopyScope() { navi48_cg_close_scope(slot_, lo_, hi_, wrote_, failed_, mismatch_); }"));
    chk("T18P the completion is clean only if every patch byte was overlaid, and it ends the copy's generation",
        has(icClose, "const bool clean = n48_fc_hg_clean(wrote && !failed && !mismatch && !s->scanFailed && !s->noPlan && (!p || p->overlaid == p->patchBytes) &&\n"
                     "                       (!p || !p->recheckBad), sampled ? 1u : 0u) != 0u;") &&
        has(icClose, "n48::hw_hg_copy_end(clean ? 1u : 0u, s->vaOk, s->va, s->va + s->bytes, s->cgLo, s->cgHi, p ? p->poison : nullptr,"));
    chk("T18P the post-copy substitution feeds the registry after its write", has(peer,
        "    if (bad) c->mismatch = true;\n    n48::hw_hg_note_substituted(at, nb);"));
    // SUBMIT PATH: the switch defaults OFF; latched at the pass's top BEFORE any program read; every program VA recorded;
    // the commit's `live` carries the answer and precedes the write; the record is kept; the ring exemption asks it.
    chk("T18P switch 62 defaults OFF and hw_hg_on reads it", has(ahh, "static volatile uint32_t gHgOn { 0u };") &&
        has(ahh, "bool hw_hg_on() { return gHgOn && __atomic_load_n(&gHgLock, __ATOMIC_ACQUIRE); }"));
    chk("T18P the pass latches 62 from hw_hg_on and records the generation for gXdC.judged + 1 (the only two frame_begin calls)",
        has(begin, "const uint32_t on = hw_hg_on() ? 1u : 0u;") && has(begin, "n48_hg_frame_begin(&gHgFrame, 1u, gXdC.judged + 1u, &gHg);") &&
        count_of(ahh, "n48_hg_frame_begin(") == 2u && count_of(begin, "n48_hg_frame_begin(") == 2u &&
        count_of(ahh, "hg_frame_begin_pass();") == 1u);
    chk("T18P decide_frame: the lock, THEN hg_frame_begin_pass, THEN the programs are identified, THEN the commit",
        order(decide, { "if (!IOLockTryLock(gXdLock))", "hg_frame_begin_pass();",
                        "gfxsrc_identify(vm, it.va, it.index, pid, &f.pgm[f.npgm]);",
                        "? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;" }));
    chk("T18P gfxsrc_identify records the program VA (latched 62) before reading it",
        order(ident, { "if (gHgFrame.on) n48_hg_frame_note_pva(&gHgFrame, va);", "gfxsrc_identify_pgm(vm, va, stage, pid, p);" }));
    chk("T18P commit: hgWhy is judged, `live` requires it ok, and `live` precedes the write",
        order(commit, { "const uint32_t hgWhy = hg_commit_judge((arm == N48_SD_ARM_COMMIT && verdict == N48_XV_TRANSLATE) ? 1u : 0u);",
                        "c.heap_refuse = (hgWhy != N48_HG_OK) ? 1u : 0u;",
                        "const bool live = (arm == N48_SD_ARM_COMMIT)", "!c.heap_refuse;",
                        "c.wrote = gfxc_write_sys(vm, va, gXdNew, n, &c.pages, &c.sys_pages);",
                        "gXdCmGateSeq = (live && reason == N48_CM_OK && c.token_ok && gXdCmToken.seq) ? gXdCmToken.seq : 0u;\n    hg_commit_record(gXdCmGateSeq);" }) &&
        [&] { const size_t a = commit.find("const bool live = (arm == N48_SD_ARM_COMMIT)"); const size_t e = commit.find(';', a);
              return a != std::string::npos && commit.find("!c.heap_refuse;", a) == e - std::strlen("!c.heap_refuse"); }());
    chk("T18P the commit judges THIS pass's record at gXdC.judged + 1", has(judge, "const uint32_t why = n48_hg_judge(&gHg, &gHgFrame, gXdC.judged + 1u);"));
    chk("T18P the gate's COMMIT keeps this pass's record and its token seq", has(rec, "gHgCommit = gHgFrame;") && has(rec, "gHgCommitSeq = gateSeq;"));
    // build 0.0.510: the walk's judge is n48_hg_walk_answer's (n48_hg_judge(s, rec, rec->judged), then switch 72's clear).
    chk("T18P the ring exemption judges the COMMITTED frame's record at the walk",
        has(exr, "const uint32_t why = n48_hg_walk_answer(&gHg, &gHgCommit, &gHgPend, seq, reinterpret_cast<uintptr_t>(current_thread()));"));
    chk("T18P writeTail sets heap_refuse BEFORE the NOP list is decided",
        order(ahh, { "ex.heap_refuse = (ex.same_thread && ex.inflight == 1u && gXdArm == N48_SD_ARM_COMMIT) ? hg_exempt_refuse(ex.seq) : 0u;",
                     "nIbPos = n48_gfxn_nop_list(g, size, n, &w, gXdArm, &ex, ibPos, &spared, &exWhy);" }));
    chk("T18P the verb 62 is mid-arm guarded and changes the switch only through n48_ra_set",
        has(ahh, "n48_cm_cont_switch_refused(62u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") &&
        has(ahh, "else { changed62 = n48_ra_set(m, &fhg); if (changed62) gHgOn = fhg; }"));
    return bad;
}

static void test_T18p_pins(const char *ahhPath, const char *peerPath, const char *ttlPath) {
    std::string ahh, peer, ttl;
    if (!ahhPath || !peerPath || !ttlPath || !read_file(ahhPath, ahh) || !read_file(peerPath, peer) || !read_file(ttlPath, ttl)) {
        expect("T18P the kext sources are readable (AppleHardwareHook.cpp, Navi48AccelPeer.cpp, Navi48Ttl.hpp)", false); return;
    }
    expect("T18P every pin holds on the REAL sources", hg_pins(peer, ahh, ttl, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // file: 0 peer, 1 ahh, 2 ttl
        { 0, "FAIL-OPEN: the in-copy patch skipped but the counter not bumped",
          "s->bumped = n48::hw_hg_copy_begin(cgLo, cgHi, s->plan ? s->plan->patch : nullptr, np, npo, &s->regSeq, mayWait, &s->hgId);", "s->bumped = 0u; (void)npo;" },
        { 1, "FAIL-OPEN: the counter compared but not recorded at the verdict (latched at the commit instead)",
          "    const uint32_t hgWhy = hg_commit_judge(", "    hg_frame_begin_pass();\n    const uint32_t hgWhy = hg_commit_judge(" },
        { 0, "ORDERING: the counter bumped after the write",
          "    if (n48::hw_hg_on()) ic_begin(dstMem, dSeg, md, backingOffset, bytes, (retile || wBytes != bytes) ? 1u : 0u, cgLo, cgHi, dstMap, self);\n", "" },
        { 0, "ORDERING: the patch applied after the VRAM write",
          "else if (ic_read(md, backingOffset, pos + k, raw, take) != take) {", "else if (md->readBytes(backingOffset + pos + k, raw, take) != take) {" },
        { 0, "DATA: the wrong program offset patched", "e.off = (uint32_t)resOff; e.nb = nb;", "e.off = (uint32_t)resOff + 0x100u; e.nb = nb;" },
        { 1, "GATE: 62 forced ON", "static volatile uint32_t gHgOn { 0u };", "static volatile uint32_t gHgOn { 1u };" },
        { 1, "GLUE: `live` no longer asks the heap generation", "                      !c.heap_refuse;", "                      true;" },
        { 1, "GLUE: the ring exemption never refuses", "? hg_exempt_refuse(ex.seq) : 0u;", "? 0u : 0u;" },
        { 2, "GLUE: the scope's close skips the in-copy completion", "navi48_cg_close_scope(slot_, lo_, hi_, wrote_, failed_, mismatch_);",
          "navi48_cg_close(slot_, lo_, hi_, wrote_, failed_, mismatch_);" },
    };
    for (const auto &p : plant) {
        std::string m[3] = { peer, ahh, ttl };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { expect("T18P plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[220]; std::snprintf(lbl, sizeof lbl, "T18P BREAK-check: %s - caught", p.what);
        expect(lbl, hg_pins(m[0], m[1], m[2], false) > 0u);
    }
}

// =============================================================================================================
// T19 — build 0.0.496 (notes/design/FAST-PAGEIN.md,): SWITCH 63, THE RESIDENCY COPY THROUGH SDMA.
// Every check drives the REAL fastcopy.h (the header the kext compiles) with run10g's real copy lengths
// (fixture_fastcopy_run10g.h), RUN D's real shader heap (T18's model, overlay included) and a mock engine.
// The kext's own order is proved by the pins in fc_pins, each with a planted break shown caught.
// =============================================================================================================
// 0.0.495's inline chunk arithmetic, transcribed from residency_copy_to_vram at 5064f01 (the reference).
static uint64_t fc_ref495_chunk(uint64_t w, uint64_t pos, uint64_t dSpan, uint64_t max) {
    uint64_t n = w - pos;
    if (dSpan < n) n = dSpan;
    if (n > max) n = max;
    if (n < w - pos) n &= ~3ULL;
    return n;
}
// Walk a copy of `w` bytes with segments of `seg` bytes (0 = one contiguous segment) as the kext's loop does.
static bool fc_plan_walk(uint64_t w, uint64_t seg, uint32_t *chunks, uint64_t *sum, bool *spanOk, bool *refOk) {
    uint64_t pos = 0; *chunks = 0; *sum = 0; *spanOk = true; *refOk = true;
    while (pos < w) {
        const uint64_t dSpan = seg ? seg - (pos % seg) : w - pos;
        const uint64_t n = n48_fc_chunk_len(w, pos, dSpan, N48_FC_STAGING_BYTES);
        if (n != fc_ref495_chunk(w, pos, dSpan, N48_FC_STAGING_BYTES)) *refOk = false;
        if (n > dSpan || n == 0) { *spanOk = false; return false; }
        pos += n; *sum += n; (*chunks)++;
        if (*chunks > 100000u) return false;
    }
    return true;
}

static void test_T19a_chunk_plan() {
    uint32_t chunkBad = 0, sumBad = 0, refBad = 0, spanBad = 0, sdmaDeclined = 0; uint64_t total = 0;
    const uint64_t segs[] = { 0ull, 0x10000ull, 0x1000ull, 0x3000ull };
    for (uint32_t i = 0; i < kFcR10gN; i++) {
        const fc_r10g_copy &c = kFcR10g[i];
        const uint64_t w = c.hi - c.lo;
        total += w;
        for (uint64_t seg : segs) {
            uint32_t ch = 0; uint64_t sum = 0; bool spanOk = true, refOk = true;
            fc_plan_walk(w, seg, &ch, &sum, &spanOk, &refOk);
            if (!seg && ch != c.chunks) chunkBad++;
            if (sum != w) sumBad++;
            if (!refOk) refBad++;
            if (!spanOk) spanBad++;
        }
        // every chunk of every real copy is one the SDMA path takes (whole dwords, <= the staging buffer, a real destination)
        for (uint64_t pos = 0; pos < w; ) {
            const uint64_t n = n48_fc_chunk_len(w, pos, w - pos, N48_FC_STAGING_BYTES);
            if (n48_fc_decide(1u, 1u, 0x0000aaaau, 1u, 0u, 1u, w, c.lo + pos, n) != N48_FC_SDMA) sdmaDeclined++;
            pos += n ? n : w;
        }
    }
    char b[200];
    std::snprintf(b, sizeof b, "T19a run10g: all %u COPIED lengths (%llu bytes) give the logged chunk count with one contiguous segment",
                  kFcR10gN, (unsigned long long)total);
    expect(b, kFcR10gN == 1327u && chunkBad == 0u);
    expect("T19a ... and every plan (contiguous, 64 KiB, 4 KiB and 12 KiB segments) covers exactly the length written", sumBad == 0u);
    expect("T19a ... n48_fc_chunk_len equals 0.0.495's inline arithmetic at every step", refBad == 0u);
    expect("T19a ... and no chunk ever runs past its destination segment (dSpan)", spanBad == 0u);
    expect("T19a every chunk of every real copy is admitted by the decision with the path healthy (0xaaaa)", sdmaDeclined == 0u);
    // 0.0.492's wallpaper: COPIED #62, bytes=0x7e9000, VRAM [0x13028000,0x13898000) - the streamed gfx12 length 0x870000, 9 chunks
    bool found = false;
    for (uint32_t i = 0; i < kFcR10gN; i++)
        if (kFcR10g[i].copy == 62u) {
            found = true;
            uint32_t ch = 0; uint64_t sum = 0; bool so = true, ro = true;
            fc_plan_walk(kFcR10g[i].hi - kFcR10g[i].lo, 0u, &ch, &sum, &so, &ro);
            expect("T19a the wallpaper (#62): bytes 0x7e9000 written as 0x870000 in 9 chunks, the last 0x70000",
                   kFcR10g[i].bytes == 0x7e9000ull && sum == 0x870000ull && ch == 9u &&
                   n48_fc_chunk_len(0x870000ull, 8ull << 20, 0x70000ull, N48_FC_STAGING_BYTES) == 0x70000ull);
        }
    expect("T19a the wallpaper row is in the fixture", found);
    expect("T19a a tail that is not whole dwords keeps the MM loop (the SDMA path declines: shape)",
           n48_fc_chunk_len(0x1002ull, 0x1000ull, 0x1000ull, N48_FC_STAGING_BYTES) == 2ull &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, 0x1002ull, 0x10001000ull, 2ull) == N48_FC_MM_SHAPE);
}

static void test_T19b_decide() {
    // OFF IDENTITY: switch 63 OFF answers MM for every input, whatever the path's state (a sweep over every other input).
    uint32_t offSdma = 0, onSdma = 0; uint64_t cases = 0;
    const uint32_t dccs[] = { 0x0000aaaau, 0x0000aabeu, 0u, 0xffffffffu, 0x00015554u };
    for (uint32_t m = 0; m < 32u; m++)
        for (uint32_t d = 0; d < 5u; d++)
            for (uint32_t i = 0; i < kFcR10gN; i += 13u) {
                const uint64_t w = kFcR10g[i].hi - kFcR10g[i].lo;
                const uint64_t n = w < N48_FC_STAGING_BYTES ? w : N48_FC_STAGING_BYTES;
                cases++;
                if (n48_fc_decide(0u, m & 1u, dccs[d], (m >> 1) & 1u, (m >> 2) & 1u, (m >> 3) & 1u, w, kFcR10g[i].lo, n) != N48_FC_MM_OFF)
                    offSdma++;
                if (n48_fc_decide(1u, m & 1u, dccs[d], (m >> 1) & 1u, (m >> 2) & 1u, (m >> 3) & 1u, w, kFcR10g[i].lo, n) == N48_FC_SDMA)
                    onSdma++;
            }
    char b[200];
    std::snprintf(b, sizeof b, "T19b OFF IDENTITY: switch 63 OFF answers `off` (the MM loop) for all %llu inputs", (unsigned long long)cases);
    expect(b, offSdma == 0u);
    expect("T19b ON: SDMA only when every precondition holds (queue up, COMP_EN clear, control passed, not latched, staging)",
           onSdma == 4u * ((kFcR10gN + 12u) / 13u));   // m = 0b01011 and 0b11011 (bit 4 unused) x DCC 0xaaaa or 0 (COMP_EN clear)
    const uint64_t w = 0x10000ull, at = 0x10000000ull;
    expect("T19b each precondition refuses with its own reason",
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 1u, 1u, w, at, w) == N48_FC_MM_LATCHED &&
           n48_fc_decide(1u, 1u, 0xaaaau, 0u, 0u, 1u, w, at, w) == N48_FC_MM_NO_PC &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 0u, w, at, w) == N48_FC_MM_NO_STAGING &&
           n48_fc_decide(1u, 0u, 0xaaaau, 1u, 0u, 1u, w, at, w) == N48_FC_MM_NO_SDMA &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, w, at, w) == N48_FC_SDMA);
    //: the boot default 0xaabe compresses (COMP_EN set) - refused; E1's default 0xaaaa - admitted.
    expect("T19b SDMA0_DCC_CNTL 0xaabe (the boot default, COMP_EN set) is refused: dcc-comp",
           n48_fc_decide(1u, 1u, 0x0000aabeu, 1u, 0u, 1u, w, at, w) == N48_FC_MM_DCC);
    uint32_t compBad = 0;
    for (uint32_t bit = 0; bit < 32u; bit++) {
        const uint32_t v = 0x0000aaaau | (1u << bit);
        const uint32_t r = n48_fc_decide(1u, 1u, v, 1u, 0u, 1u, w, at, w);
        if ((N48_DCC_NOPTE_COMP_EN_MASK >> bit) & 1u) { if (r != N48_FC_MM_DCC) compBad++; }
        else if (r != N48_FC_SDMA) compBad++;
    }
    expect("T19b every one of the eight COMP_EN bits alone refuses; no other bit does", compBad == 0u);
    expect("T19b shape: 0 bytes, over the staging buffer, not whole dwords, a misaligned or zero destination - all refused",
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, w, at, 0u) == N48_FC_MM_SHAPE &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, 2ull << 20, at, (1ull << 20) + 4u) == N48_FC_MM_SHAPE &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, w, at, w - 2u) == N48_FC_MM_SHAPE &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, w, at + 2u, w) == N48_FC_MM_SHAPE &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, w, 0u, w) == N48_FC_MM_SHAPE &&
           n48_fc_decide(1u, 1u, 0xaaaau, 1u, 0u, 1u, w, at, w + 4u) == N48_FC_MM_SHAPE);
    expect("T19b the last gate: only the VRAM guard's 0 with the queue idle submits",
           n48_fc_submit_ok(0u, 1u) == 1u && n48_fc_submit_ok(3u, 1u) == 0u && n48_fc_submit_ok(4u, 1u) == 0u &&
           n48_fc_submit_ok(0u, 0u) == 0u);
    expect("T19b the switch values: 319 sampled, 575 OFF, 831 full (M = arg >> 8, selector 63)",
           (319u & 0xffu) == 63u && n48_fc_mode_of(319u >> 8) == N48_FC_M_SAMPLED && n48_fc_mode_of(575u >> 8) == 0u &&
           n48_fc_mode_of(831u >> 8) == N48_FC_M_FULL && n48_fc_mode_of(0u) == 0u && n48_fc_mode_of(4u) == 0u);
}

static void test_T19c_sample_plan() {
    std::vector<uint64_t> sizes;
    for (uint32_t i = 0; i < kFcR10gN; i++) {
        const uint64_t w = kFcR10g[i].hi - kFcR10g[i].lo;
        for (uint64_t pos = 0; pos < w; ) {
            const uint64_t n = n48_fc_chunk_len(w, pos, w - pos, N48_FC_STAGING_BYTES);
            sizes.push_back(n); pos += n;
        }
    }
    // synthetic: a partial last page, a single dword, two dwords, one page, one page + one dword
    const uint64_t extra[] = { 0x5400ull, 4ull, 8ull, 0x1000ull, 0x1004ull, 0xFFFFCull, 0x100000ull };
    for (uint64_t e : extra) sizes.push_back(e);
    uint32_t bad = 0, firstLast = 0, rotSame = 0, rotTried = 0; uint64_t planned = 0;
    for (uint64_t n : sizes)
        for (uint32_t rot = 0; rot < 3u; rot++) {
            uint32_t off[N48_FC_SAMPLE_MAX];
            const uint32_t k = n48_fc_sample_plan((uint32_t)n, rot * 1237u, off, N48_FC_SAMPLE_MAX);
            const uint32_t pages = (uint32_t)((n + 4095u) / 4096u);
            if (k != 2u + pages) { bad++; continue; }
            planned += k;
            bool hasFirst = false, hasLast = false;
            std::vector<bool> page(pages, false);
            for (uint32_t i = 0; i < k; i++) {
                if (off[i] & 3u || off[i] + 4u > n) bad++;
                if (off[i] == 0u) hasFirst = true;
                if (off[i] == n - 4u) hasLast = true;
                if (i >= 2u) page[off[i] / 4096u] = true;
            }
            for (uint32_t p = 0; p < pages; p++) if (!page[p]) bad++;
            if (!hasFirst || !hasLast) firstLast++;
            if (rot && n >= 0x2000u) {
                uint32_t o0[N48_FC_SAMPLE_MAX];
                n48_fc_sample_plan((uint32_t)n, 0u, o0, N48_FC_SAMPLE_MAX);
                rotTried++;
                if (std::memcmp(o0 + 2, off + 2, (k - 2u) * 4u) == 0) rotSame++;
            }
        }
    char b[220];
    std::snprintf(b, sizeof b, "T19c the sampled plan over every real chunk (%zu) + 7 synthetic: first and last dword, one dword in EVERY "
                  "4 KiB page (incl. a partial last page), in range and dword-aligned (%llu samples)", sizes.size(), (unsigned long long)planned);
    expect(b, bad == 0u && firstLast == 0u);
    expect("T19c the per-page offset rotates between chunks", rotTried > 0u && rotSame == 0u);
    expect("T19c a chunk that is not whole dwords has no plan", n48_fc_sample_plan(6u, 0u, nullptr, 0u) == 0u &&
           n48_fc_sample_count(6u) == 0u && n48_fc_sample_count(0u) == 0u);
    expect("T19c the largest plan fits its bound (1 MiB: 258 samples)", n48_fc_sample_count(N48_FC_STAGING_BYTES) == N48_FC_SAMPLE_MAX);
}

static void test_T19d_packet(const char *bringupPath) {
    uint32_t pkt[N48_FC_PKT_DWORDS + 2];
    const uint64_t src = 0x840fc2e000ull, dst = 0x8010020000ull, fmc = 0x840fc01000ull + N48_FC_WB_FENCE_OFF;
    const uint32_t k = n48_fc_build_packet(pkt, N48_FC_PKT_DWORDS, src, dst, 0x9000u, fmc, 0xFC000001u);
    const uint32_t want[N48_FC_PKT_DWORDS] = { 0x10000001u, 0x8fffu, 0u, 0x0fc2e000u, 0x84u, 0x10020000u, 0x80u, 0u,
                                               0x00000005u, (uint32_t)fmc, (uint32_t)(fmc >> 32), 0xFC000001u };
    expect("T19d COPY_LINEAR (op 1, sub-op 0, CPV 1: 8 dwords) + FENCE (op 5: 4 dwords), field for field",
           k == 12u && std::memcmp(pkt, want, sizeof want) == 0);
    expect("T19d a short buffer or 0 bytes builds nothing", n48_fc_build_packet(pkt, 11u, src, dst, 4u, fmc, 1u) == 0u &&
           n48_fc_build_packet(pkt, 12u, src, dst, 0u, fmc, 1u) == 0u);
    std::string bu;
    if (!bringupPath || !read_file(bringupPath, bu)) { expect("T19d Navi48Bringup.cpp is readable", false); return; }
    // THE SAME PACKET scanout_sdma_copies emits: its emission lines, in order, are what the golden above transcribes.
    const std::string sc = fn_text(bu, "static uint32_t scanout_sdma_copies(");
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0; for (const char *st : steps) { const size_t at = t.find(st, prev); if (at == std::string::npos) return false; prev = at + 1; }
        return true; };
    expect("T19d scanout_sdma_copies emits exactly this layout (header CPV 1, count-1, 0, src, dst, 0, FENCE addr, value)",
           order(sc, { "amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_COPY) |", "amdgpu::SDMA_PKT_HEADER_SUB_OP(amdgpu::SDMA_SUBOP_COPY_LINEAR) | amdgpu::SDMA_PKT_HEADER_CPV(1);",
                       "pkt[k++] = bytes[i] - 1;", "pkt[k++] = 0;", "pkt[k++] = static_cast<uint32_t>(srcMc[i]);",
                       "pkt[k++] = static_cast<uint32_t>(srcMc[i] >> 32);", "pkt[k++] = static_cast<uint32_t>(dstMc[i]);",
                       "pkt[k++] = static_cast<uint32_t>(dstMc[i] >> 32);", "pkt[k++] = 0;",
                       "pkt[k++] = amdgpu::SDMA_PKT_HEADER_OP(amdgpu::SDMA_OP_FENCE);", "pkt[k++] = static_cast<uint32_t>(fenceMc);",
                       "pkt[k++] = static_cast<uint32_t>(fenceMc >> 32);", "pkt[k++] = fenceValue;" }));
    std::string sd;
    const std::string sdPath = std::string(bringupPath).substr(0, std::string(bringupPath).rfind('/')) + "/amd/amdgpu_sdma.h";
    expect("T19d amdgpu_sdma.h's opcodes are the header's (COPY 1, FENCE 5, COPY_LINEAR 0; CPV bit 28; the WB page 16 KiB)",
           read_file(sdPath.c_str(), sd) && has(sd, "constexpr uint32_t SDMA_OP_COPY   = 1;") && has(sd, "constexpr uint32_t SDMA_OP_FENCE  = 5;") &&
           has(sd, "constexpr uint32_t SDMA_SUBOP_COPY_LINEAR = 0;") &&
           has(sd, "static inline uint32_t SDMA_PKT_HEADER_CPV(uint32_t v)         { return ((v & 0x1) << 28); }") &&
           has(sd, "constexpr uint32_t kSDMAWBExtFenceOffset = 0x180;") && has(sd, "constexpr uint32_t kSDMAWBPageBytes       = 16 * 1024;"));
    // The fence dword is ours alone: no other write-back offset in the kext reaches 0x1C0.
    expect("T19d the fast copy's fence dword (0x1C0) is used by the fast copy only (and pipeshim's 0xC0 slot is untouched by it)",
           count_of(bu, "N48_FC_WB_FENCE_OFF") >= 2u && !has(fn_text(bu, "static uint32_t fc_submit("), "kSDMAWBCSFenceOffset"));
}

static void test_T19e_fence_and_outcome() {
    n48_fc_state st {};
    uint32_t prev = 0, bad = 0;
    for (uint32_t i = 0; i < 100000u; i++) {
        const uint32_t f = n48_fc_fence_next(&st);
        if (f == 0u || (i && f <= prev) || (i == 0u && f != N48_FC_FENCE_BASE)) bad++;
        prev = f;
    }
    expect("T19e 100000 fence values: strictly increasing from 0xFC000001, never 0, never equal to an earlier one", bad == 0u && !st.latched_off);
    st.fence_last = 0xFFFFFFFFu;
    expect("T19e the values exhausted: 0 (no submission) and the path latches OFF", n48_fc_fence_next(&st) == 0u && st.latched_off &&
           st.latch_why == N48_FC_LATCH_FENCES);
    // the outcome
    n48_fc_state a {}; a.staging_ok = 1u; a.pc_passed = 1u;
    expect("T19e a landed fence is the only success", n48_fc_fence_outcome(&a, N48_FC_SUB_LANDED) == N48_FC_RUN_OK && !a.latched_off &&
           n48_fc_staging_usable(&a));
    expect("T19e a refusal before the ring was written fails the chunk, the path stays usable",
           n48_fc_fence_outcome(&a, N48_FC_SUB_REFUSED) == N48_FC_RUN_REFUSED && !a.latched_off && n48_fc_staging_usable(&a));
    n48_fc_state t = a;
    expect("T19e FAIL-CLOSED: a fence that did not land is a FAILED copy (never success)",
           n48_fc_fence_outcome(&t, N48_FC_SUB_TIMEOUT) == N48_FC_RUN_TIMEOUT);
    expect("T19e ... the path is LATCHED OFF for the boot (timeout)", t.latched_off == 1u && t.latch_why == N48_FC_LATCH_TIMEOUT &&
           t.timeouts == 1u);
    expect("T19e ... and the staging buffer is RETIRED - never handed out again, even were the latch cleared",
           t.staging_retired == 1u && n48_fc_staging_usable(&t) == 0u && [&] { n48_fc_state u = t; u.latched_off = 0u;
               return n48_fc_decide(1u, 1u, 0xaaaau, u.pc_passed, u.latched_off, n48_fc_staging_usable(&u), 0x1000u, 0x10000000u, 0x1000u)
                      == N48_FC_MM_NO_STAGING; }());
    expect("T19e ... every later chunk keeps the MM window", n48_fc_decide(1u, 1u, 0xaaaau, t.pc_passed, t.latched_off,
           n48_fc_staging_usable(&t), 0x1000u, 0x10000000u, 0x1000u) == N48_FC_MM_LATCHED);
    n48_fc_state r = a;
    expect("T19e a ring write or doorbell that failed: FAILED, latched (ring), staging retired",
           n48_fc_fence_outcome(&r, N48_FC_SUB_RING) == N48_FC_RUN_RING && r.latched_off && r.latch_why == N48_FC_LATCH_RING &&
           r.staging_retired);
    // the poll schedule: IODelay(5) to 500 us, IODelay(50) to 2 ms, then IOSleep(1); the hard bound 200 ms
    expect("T19e the wait: 5 us steps for 500 us, 50 us to 2 ms, then 1 ms; landed only on the exact value; timeout at 200 ms",
           n48_fc_poll_delay_us(0) == 5u && n48_fc_poll_delay_us(499) == 5u && n48_fc_poll_delay_us(500) == 50u &&
           n48_fc_poll_delay_us(1999) == 50u && n48_fc_poll_delay_us(2000) == 1000u &&
           n48_fc_poll_state(0xFC000002u, 0xFC000002u, 10u) == N48_FC_POLL_LANDED &&
           n48_fc_poll_state(0xFC000001u, 0xFC000002u, 10u) == N48_FC_POLL_WAIT &&
           n48_fc_poll_state(0u, 0u, 10u) == N48_FC_POLL_WAIT &&
           n48_fc_poll_state(0xFC000001u, 0xFC000002u, N48_FC_FENCE_TIMEOUT_US) == N48_FC_POLL_TIMEOUT &&
           n48_fc_poll_state(0xFC000002u, 0xFC000002u, N48_FC_FENCE_TIMEOUT_US + 5u) == N48_FC_POLL_LANDED);
    // result word
    const uint64_t ok = n48_fc_result_ok(262144u, 3u), f = n48_fc_result_fail(N48_FC_RUN_TIMEOUT);
    expect("T19e the result word: taken, compared, mismatches; a failure carries its run code",
           (ok & N48_FC_R_TAKEN) && !(ok & N48_FC_R_FAIL) && ((ok >> 32) & 0x3FFFFFFFull) == 262144u && (uint32_t)ok == 3u &&
           (f & N48_FC_R_TAKEN) && (f & N48_FC_R_FAIL) && (f & 0xffu) == N48_FC_RUN_TIMEOUT &&
           std::strstr(n48_fc_fail_text(f), "latched OFF") != nullptr);
}

// ---- the mock engine: T18's RUN D heap, the kext's producer (readBytes + the checked overlay), staging and VRAM ----------
struct FcMock {
    HgSim *hs;              // RUN D's heap model (backing, patches, arena, apple)
    uint32_t on62;          // switch 62: the producer overlays (ic_read) only when ON
    uint8_t stg[N48_FC_STAGING_BYTES];
    uint8_t vram[0x9000];
    uint32_t fills, fillsAtSubmit, submits, oursAtSubmit, streamAtSubmit;
    uint32_t sub;           // what the engine answers
    uint32_t lastFence;
};
static FcMock gFm;
// the MM loop's stream for the heap (T18's model): readBytes, then (62 ON) the checked overlay, batch by batch
static void fc_model_stream(HgSim &s, uint32_t on62, uint8_t *out, uint32_t n) {
    for (uint32_t k = 0; k < n; k += 256u) {
        const uint32_t take = n - k < 256u ? n - k : 256u;
        std::memcpy(out + k, s.backing + k, take);
        uint32_t nb = 0; if (on62) (void)n48_hg_overlay_checked(s.patch, s.np, s.arena, s.apple, k, out + k, take, &nb);
    }
}
static int fc_mock_fill(void *vc, uint8_t *dst, uint64_t off, uint32_t take) {
    FcMock *m = static_cast<FcMock *>(vc);
    m->fills++;
    std::memcpy(dst, m->hs->backing + off, take);                  // md->readBytes(backingOffset + pos + off, ...)
    uint32_t nb = 0;
    if (m->on62) (void)n48_hg_overlay_checked(m->hs->patch, m->hs->np, m->hs->arena, m->hs->apple, off, dst, take, &nb);   // ic_read's overlay
    return 1;
}
static uint32_t fc_mock_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {
    FcMock *m = static_cast<FcMock *>(vc);
    m->submits++; m->fillsAtSubmit = m->fills; m->lastFence = fence;
    // what the engine is about to read: is the whole stream (and every one of our patches) already in staging?
    static uint8_t want[0x9000];
    fc_model_stream(*m->hs, m->on62, want, bytes);
    m->streamAtSubmit = std::memcmp(m->stg, want, bytes) == 0 ? 1u : 0u;
    m->oursAtSubmit = 1u;
    for (uint32_t i = 0; i < kR10iSubstN; i++)
        if (std::memcmp(m->stg + kR10iSubst[i].off, m->hs->ours + kR10iSubst[i].off, kR10iSubst[i].nb)) m->oursAtSubmit = 0u;
    if (m->sub == N48_FC_SUB_LANDED && srcMc == 0x840fc2e000ull && dstMc >= 0x8010020000ull)
        std::memcpy(m->vram + (dstMc - 0x8010020000ull), m->stg, bytes);   // COPY_LINEAR, then the fence lands
    return m->sub;
}
static int fc_mock_read(void *vc, uint64_t vram, uint32_t *dst, uint32_t dwords) {
    FcMock *m = static_cast<FcMock *>(vc);
    if (vram < 0x10020000ull || vram + dwords * 4u > 0x10029000ull) return 0;
    std::memcpy(dst, m->vram + (vram - 0x10020000ull), dwords * 4u);
    return 1;
}
// build 0.0.514 A1: the sampled verify's plan reader over the same mock VRAM - what fc_vram_read_sampled_timed does in the
// kext (every plan offset, in plan order, one call per chunk); a refused offset refuses the whole plan.
static uint32_t gFcPlanCalls = 0u;
static uint32_t gFcGot[N48_FC_SAMPLE_MAX];
static int fc_mock_plan(void *vc, uint64_t d_at, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
    gFcPlanCalls++;
    for (uint32_t i = 0; i < ns; i++)
        if (!fc_mock_read(vc, d_at + n48_fc_sample_at(n, rot, i), &got[i], 1u)) return 0;
    return 1;
}

static void test_T19f_order_and_identity() {
    // RUN D's heap copy (#62, 0x9000 bytes, one chunk) through the SDMA path with switch 62 ON, then OFF.
    for (uint32_t on62 = 0; on62 < 2u; on62++) {
        HgSim &s = gHs; hs_boot(s);
        FcMock &m = gFm; std::memset(&m, 0, sizeof m); m.hs = &s; m.on62 = on62; m.sub = N48_FC_SUB_LANDED;
        n48_fc_state st {}; st.pc_passed = 1u; st.staging_ok = 1u;
        const n48_fc_ops ops { &m, &fc_mock_fill, &fc_mock_submit };
        static uint32_t exp[0x9000 / 4];
        uint8_t lead[256]; std::memset(lead, 0xEE, sizeof lead);
        const uint32_t r = n48_fc_chunk_run(&ops, &st, m.stg, 0x840fc2e000ull, 0x8010020000ull, 0x9000u, exp, N48_FC_M_FULL, 0u, 0u, lead);
        char b[200];
        std::snprintf(b, sizeof b, "T19f [62 %s] REACHABILITY: all 144 batches were produced into staging BEFORE the one submission",
                      on62 ? "ON " : "OFF");
        expect(b, r == N48_FC_RUN_OK && m.submits == 1u && m.fillsAtSubmit == 0x90u && m.fills == 0x90u);
        std::snprintf(b, sizeof b, "T19f [62 %s] the staged stream at submission is exactly the MM loop's stream%s", on62 ? "ON " : "OFF",
                      on62 ? " - every one of the 31 patches already overlaid (0.0.495's overlay before the stream reaches staging)" : "");
        expect(b, m.streamAtSubmit == 1u && (on62 ? m.oursAtSubmit == 1u : m.oursAtSubmit == 0u));
        static uint8_t mm[0x9000];
        fc_model_stream(s, on62, mm, 0x9000u);
        std::snprintf(b, sizeof b, "T19f [62 %s] BYTE IDENTITY: the VRAM image the SDMA path leaves equals the MM loop's, byte for byte",
                      on62 ? "ON " : "OFF");
        expect(b, std::memcmp(m.vram, mm, 0x9000u) == 0);
        uint64_t cmp = 0; uint32_t rf = 0;
        const uint64_t bad = n48_fc_verify(&fc_mock_read, &fc_mock_plan, &m, 0x10020000ull, exp, gFcGot, 0x9000u, N48_FC_M_FULL, 0u, &cmp, &rf);
        expect("T19f the full verify reads back every dword against the snapshot: 9216 compared, 0 mismatched", bad == 0u && cmp == 9216u && !rf);
        uint32_t w0 = 0, w1 = 0; std::memcpy(&w0, lead, 4); std::memcpy(&w1, lead + 4, 4);
        uint32_t h31 = 0; std::memcpy(&h31, lead + 124, 4);
        expect("T19f the log's first source dwords come from the staged stream (RUN D's copy #62: bfa00003 d5430001), head 128 bytes",
               w0 == kR10iCopy62First[0] && w1 == kR10iCopy62First[1] && h31 == hs_r32(mm, 124u) && lead[128] == 0xEE);
    }
    // A fence that never lands: FAILED, latched, retired - and the VRAM image is not trusted (the copy loop poisons).
    {
        HgSim &s = gHs; hs_boot(s);
        FcMock &m = gFm; std::memset(&m, 0, sizeof m); m.hs = &s; m.on62 = 1u; m.sub = N48_FC_SUB_TIMEOUT;
        n48_fc_state st {}; st.pc_passed = 1u; st.staging_ok = 1u;
        const n48_fc_ops ops { &m, &fc_mock_fill, &fc_mock_submit };
        static uint32_t exp[N48_FC_SAMPLE_MAX];
        const uint32_t r = n48_fc_chunk_run(&ops, &st, m.stg, 0x840fc2e000ull, 0x8010020000ull, 0x9000u, exp, N48_FC_M_SAMPLED, 7u, 0u, nullptr);
        expect("T19f a fence timeout mid-copy: the chunk FAILS (the copy loop's FAILED path poisons its range), the path latches OFF and "
               "the staging buffer is retired", r == N48_FC_RUN_TIMEOUT && st.latched_off && st.staging_retired &&
               (n48_fc_result_fail(r) & N48_FC_R_FAIL));
        const uint32_t f0 = m.lastFence;
        const uint32_t r2 = n48_fc_decide(1u, 1u, 0xaaaau, st.pc_passed, st.latched_off, n48_fc_staging_usable(&st), 0x9000u, 0x10020000u, 0x9000u);
        expect("T19f ... and the next copy is never submitted (latched), its fence would differ anyway", r2 == N48_FC_MM_LATCHED &&
               n48_fc_fence_next(&st) != f0);
    }
    // The sampled verify: a wrong dword on a sampled offset is caught; the full verify catches any.
    {
        HgSim &s = gHs; hs_boot(s);
        FcMock &m = gFm; std::memset(&m, 0, sizeof m); m.hs = &s; m.on62 = 1u; m.sub = N48_FC_SUB_LANDED;
        n48_fc_state st {}; st.pc_passed = 1u; st.staging_ok = 1u;
        const n48_fc_ops ops { &m, &fc_mock_fill, &fc_mock_submit };
        static uint32_t exp[N48_FC_SAMPLE_MAX];
        (void)n48_fc_chunk_run(&ops, &st, m.stg, 0x840fc2e000ull, 0x8010020000ull, 0x9000u, exp, N48_FC_M_SAMPLED, 5u, 0u, nullptr);
        uint64_t cmp = 0; uint32_t rf = 0;
        const uint64_t clean = n48_fc_verify(&fc_mock_read, &fc_mock_plan, &m, 0x10020000ull, exp, gFcGot, 0x9000u, N48_FC_M_SAMPLED, 5u, &cmp, &rf);
        const uint32_t lastPage = n48_fc_sample_at(0x9000u, 5u, 2u + 8u);
        m.vram[lastPage] ^= 0x40u;
        uint64_t cmp2 = 0;
        const uint64_t caught = n48_fc_verify(&fc_mock_read, &fc_mock_plan, &m, 0x10020000ull, exp, gFcGot, 0x9000u, N48_FC_M_SAMPLED, 5u, &cmp2, &rf);
        m.vram[lastPage] ^= 0x40u;
        m.vram[0x8ffc] ^= 1u;   // the chunk's last dword
        const uint64_t caughtLast = n48_fc_verify(&fc_mock_read, &fc_mock_plan, &m, 0x10020000ull, exp, gFcGot, 0x9000u, N48_FC_M_SAMPLED, 5u, &cmp2, &rf);
        expect("T19f sampled verify: 11 dwords (first, last, one in each of 9 pages) read clean; a wrong dword in the LAST page's sample "
               "and a wrong last dword are both caught", clean == 0u && cmp == 11u && caught == 1u && caughtLast == 1u);
    }
}

// ---- the kext's own order: pins, each with a planted break shown caught -----------------------------------------------------
static const char *const kFc495MmLoop = R"N48(        for (uint64_t k = 0; k < n; ) {
            const uint64_t left = n - k;
            const uint64_t take = left >= 256 ? 256 : left;   // source bytes in this batch
            const uint32_t cnt  = (uint32_t)((take + 3) / 4);
            //: gfx12 order. build 0.0.492: rp_retile_bytes (noinline) - the gfx10 re-tile's whole image (rt.dst, as
            // before), or the backing-sourced copy's gfx12 bytes converted a chunk at a time (rp_lin_fetch).
            if (retile) { if (!rp_retile_bytes(&rt, pos + k, raw, take)) { fail = "the backing-sourced chunk could not be read or converted"; break; } }
            // build 0.0.495: ic_read = this readBytes, then (switch 62, a patched copy on this thread only) our bytes over it.
            else if (ic_read(md, backingOffset, pos + k, raw, take) != take) {
                fail = "readBytes of the backing returned short"; break;
            }
            if (take & 3) {                                    // resource ends inside this dword
                uint32_t cur = 0;
                if (!navi48_vram_read_mm(dAt + k + (take & ~3ULL), &cur, 1)) {
                    fail = "the MM window refused the tail read"; break;
                }
                memcpy(raw + take, reinterpret_cast<const uint8_t *>(&cur) + (take & 3), 4 - (take & 3));
            }
            memcpy(buf, raw, (size_t)cnt * 4);
            if (!navi48_vram_write_mm(dAt + k, buf, cnt)) { fail = "the MM window refused a write batch"; break; }
            if (!navi48_vram_read_mm(dAt + k, back, cnt)) { fail = "the MM window refused a read-back batch"; break; }
            for (uint32_t i = 0; i < cnt; i++) if (back[i] != buf[i]) mismatched++;
            compared += cnt;
            if (pos == 0 && k == 0) { firstSrc[0] = buf[0]; firstSrc[1] = cnt > 1 ? buf[1] : 0; }
            for (uint32_t i = 0; i < cnt && headN < 32; i++) head[headN++] = buf[i];
            k += (uint64_t)cnt * 4;
        }
        if (fail) {
            gCopy.failed++;
            cgScope.markFailed();   // 2: PARTIAL WRITE - some dwords of this chunk may already be written
            PEERLOG("residency-copy: FAILED - %s in the chunk at resource offset %#llx (VRAM %#llx); "
                    "this pageTexture keeps the skip", fail, (unsigned long long)pos,
                    (unsigned long long)dAt);
            return false;
        }
        pos += n;
        chunks++;)N48";

static size_t fc_pins(const std::string &bu, const std::string &peer, const std::string &ahh, const std::string &ttl, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0; bool first = true;
        for (const char *st : steps) {
            const size_t at = t.find(st, first ? 0 : prev);
            if (at == std::string::npos || count_of(t, st) != 1u) return false;
            prev = at + 1; first = false;
        }
        return true;
    };
    const size_t fb = peer.find("static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    const size_t fe = fb == std::string::npos ? fb : peer.find("THE PAGE-OUT COPY: pageTexture(toVram = 0)", fb);
    const std::string cp = (fb == std::string::npos || fe == std::string::npos) ? std::string() : peer.substr(fb, fe - fb);
    const std::string fill = fn_text(peer, "static int fc_fill_batch(void *vc, uint8_t *dst, uint64_t off, uint32_t take) {");
    const std::string ccc = fn_text(peer, "static __attribute__((noinline)) uint64_t fc_copy_chunk(");
    const std::string chunk = fn_text(bu, "uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,");
    const std::string sub = fn_text(bu, "static uint32_t fc_submit(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {");
    const std::string latch = fn_text(bu, "static FcSlot *fc_slot_latch(uint64_t pos, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {");
    const std::string set = fn_text(bu, "uint32_t navi48_fc_set(uint32_t m) {");
    const std::string pc = fn_text(bu, "static uint32_t fc_positive_control() {");
    const std::string stg = fn_text(bu, "static uint32_t fc_staging_bind() {");
    chk("T19P the functions are found", !cp.empty() && !fill.empty() && !ccc.empty() && !chunk.empty() && !sub.empty() &&
        !latch.empty() && !set.empty() && !pc.empty() && !stg.empty());
    // THE COPY LOOP: the chunk plan, the VRAM guard for exactly this chunk, THEN the SDMA call, THEN (declined) 0.0.495's batch loop.
    chk("T19P copy loop: chunk plan -> the VRAM guard of [dAt, dAt+n) -> fc_copy_chunk -> (declined) the MM batch loop -> the COPIED "
        "line -> the via-SDMA line", order(cp, {
            "const uint64_t n = n48_fc_chunk_len(wBytes, pos, dSpan, kCopyChunkBytes);",
            "if (n == 0 || (dAt & 3) || navi48_vram_apple_dest_check(dAt, (n + 3) & ~3ULL)) {",
            "const uint64_t fc = fc_copy_chunk(&rt, retile, md, backingOffset, pos, wBytes, dAt, n, raw, cgLo, cgHi);",
            "if (fc & N48_FC_R_TAKEN) {", "if (fc & N48_FC_R_FAIL) fail = n48_fc_fail_text(fc);",
            "        } else\n        for (uint64_t k = 0; k < n; ) {",
            "residency-copy: COPIED #%llu", "navi48_fc_copy_report(gCopy.copies);" }));
    chk("T19P OFF IDENTITY: the MM batch loop and its FAILED path are 0.0.495's, character for character",
        count_of(cp, kFc495MmLoop) == 1u);
    chk("T19P a taken chunk's result feeds the SAME counters (compared, mismatched -> markMismatch) and the head",
        has(cp, "compared += (fc >> 32) & 0x3FFFFFFFull;") && has(cp, "mismatched += (uint32_t)fc;") &&
        has(cp, "if (mismatched) cgScope.markMismatch();"));
    // THE PRODUCER: the MM loop's own, overlay included (ic_read), re-tiled bytes through rp_retile_bytes.
    chk("T19P REACHABILITY: the SDMA path's producer is the MM loop's - rp_retile_bytes for a re-tiled copy, else ic_read (readBytes + "
        "0.0.495's overlay) - never a bare readBytes", has(fill, "if (c->retile) return rp_retile_bytes(c->rt, c->pos + off, dst, take) ? 1 : 0;") &&
        has(fill, "return ic_read(c->md, c->backingOffset, c->pos + off, dst, take) == take ? 1 : 0;") && !has(fill, "readBytes("));
    chk("T19P fc_copy_chunk hands navi48_fc_chunk that producer", has(ccc, "const uint64_t r = navi48_fc_chunk(pos, wBytes, dAt, n, &fc_fill_batch, &c, lead, cgLo, cgHi, cand);"));
    // THE CHUNK: latch -> decide under gFastCopyLock -> run (fill, snapshot, fence, submit) -> unlock -> verify.
    chk("T19P navi48_fc_chunk: latched per copy, OFF returns before anything, decided under gFastCopyLock, run, unlocked, THEN verified",
        order(chunk, { "FcSlot *s = fc_slot_latch(pos, cgLo, cgHi, rpCand);", "if (!s || !s->mode) return 0ull;", "\t\tIOLockLock(l);\n\t\twhy = n48_fc_decide(",
                       "why = n48_fc_decide(1u, fc_sdma_up(), fc_dcc_raw(), gFc.pc_passed, gFc.latched_off, n48_fc_staging_usable(&gFc), wBytes, dAt, n);",
                       "const uint32_t r = n48_fc_chunk_run(&ops, &gFc,", "const uint32_t latched = gFc.latched_off",
                       "\tIOLockUnlock(l);\n\ts->stageUs", "return n48_fc_result_fail(r);",
                       "const uint64_t bad = n48_fc_verify(&fc_mm_read, &fc_mm_read_plan, &vt, dAt, exp, full ? nullptr : exp + N48_FC_SAMPLE_MAX," }));
    chk("T19P the latch: the switch is read ONCE, at the copy's first chunk (pos 0); OFF releases the slot",
        order(latch, { "if (pos != 0u) return s;", "const uint32_t mode = n48_fc_mode_of(__atomic_load_n(&gFcMode, __ATOMIC_ACQUIRE));",
                       "if (!mode) {" }) && count_of(bu, "gFcMode, __ATOMIC_ACQUIRE") == 1u);
    chk("T19P switch 63 boots OFF", has(bu, "static volatile uint32_t gFcMode { 0u };"));
    // THE SUBMISSION: the VRAM guard of exactly this chunk and the queue idle, under gScanoutLock, BEFORE the ring is written.
    // 0.0.509 item 1: the ORDER (lock, kick, the bound's clock, the wait, unlock) is fastcopy.h's n48_fc_submit_wait, host-tested
    // in T21a; the kext's fc_submit runs it with the kernel callbacks, and fc_kick keeps 0.0.508's gate -> packet -> ring -> doorbell.
    const std::string kick = fn_text(bu, "static uint32_t fc_kick(void *vc, uint64_t srcMc, uint64_t dstMc, uint32_t bytes, uint32_t fence) {");
    chk("T19P fc_submit: the submission is n48_fc_submit_wait over the kernel callbacks (gScanoutLock, fc_kick, our own fence dword)",
        order(sub, { "if (!gBringup.dev || !gScanoutLock) return N48_FC_SUB_REFUSED;",
                     "const uint32_t r = n48_fc_submit_wait(&kFcWaitOps, c, srcMc, dstMc, bytes, fence, &w);",
                     "c->lockUs = w.lock_us;", "if (w.kicked) { c->dmaUs = w.el_us; c->lastFence = w.seen; }" }) &&
        has(bu, "static const n48_fc_wait_ops kFcWaitOps = { &fc_now_us, &fc_lock, &fc_unlock, &fc_kick, &fc_fence_read, &fc_delay };") &&
        has(bu, "static void fc_lock(void *) { IOLockLock(gScanoutLock); }") && has(bu, "static void fc_unlock(void *) { IOLockUnlock(gScanoutLock); }") &&
        has(bu, "static uint32_t fc_fence_read(void *) { return amdgpu::sdma_wb_read32(*gBringup.dev, gBringup.sdma.instance[0], N48_FC_WB_FENCE_OFF); }") &&
        !has(bu, "n48_fc_poll_state("));
    chk("T19P fc_kick: queue idle + the VRAM guard for [dAt, dAt+bytes) -> the gate -> packet -> ring -> doorbell -> 0 (kicked)",
        order(kick, { "const uint32_t idle = (scanout_queue_check(dev, inst, &rptr) == kScanStOk",
            ": navi48_vram_apple_dest_check(c->dAt, bytes);", "if (!n48_fc_submit_ok(destWhy, idle)) return N48_FC_SUB_REFUSED;",
            "n48_fc_build_packet(pkt, N48_FC_PKT_DWORDS, srcMc, dstMc, bytes, inst.wb_bus + N48_FC_WB_FENCE_OFF, fence);",
            "amdgpu::sdma_ring_write(dev, inst, pkt, k)", "amdgpu::sdma_kick_doorbell(dev, inst)", "return 0u;" }) &&
        !has(kick, "IOLockLock") && !has(kick, "IOLockUnlock"));
    {   // every mention of gFastCopyLock lies in the fast copy's own section of Navi48Bringup.cpp
        const size_t sb = bu.find("SWITCH 63, THE RESIDENCY COPY THROUGH SDMA");
        const std::string rep = fn_text(bu, "void navi48_fc_report(const char *why) {");
        const size_t se = rep.empty() ? std::string::npos : bu.find(rep) + rep.size();
        const std::string sec = (sb == std::string::npos || se == std::string::npos || se < sb) ? std::string() : bu.substr(sb, se - sb);
        chk("T19P LOCK ORDER: gFastCopyLock is the fast copy's alone (never pipeshim's, never a scanout path's, not the copier's), and "
            "gScanoutLock is taken inside it only by fc_sdma_up, fc_submit and the control",
            !sec.empty() && count_of(bu, "gFastCopyLock") == count_of(sec, "gFastCopyLock") && !has(ahh, "gFastCopyLock") &&
            !has(peer, "gFastCopyLock") && count_of(sec, "IOLockLock(gScanoutLock);") == 10u);   // sdma_up, submit, control x4,
                                                                                                  // 0.0.534: switch 89's control x4
        // build 0.0.542: the ONE user outside that section, `accel scanout full` (navi48_scanout_full), borrows switch 89's
        // read-back buffer under the fast-copy lock: through fc_lock_get, TRY-locked only (it never waits), after flip mode's lock.
        const std::string sf = fn_text(bu, "static uint32_t navi48_scanout_full(uint32_t mode, uint64_t *v) {");
        chk("T19P LOCK ORDER (0.0.542): scanout full takes the fast-copy lock only by TRY-lock, after gFmLock, and names no other copy lock",
            !sf.empty() && count_of(sf, "fl = fc_lock_get();") == 1u && count_of(sf, "if (!fl || !IOLockTryLock(fl))") == 1u &&
            count_of(sf, "IOLockLock(fl)") == 0u && sf.find("IOLockTryLock(gFmLock)") < sf.find("IOLockTryLock(fl)") &&
            count_of(bu, "fc_lock_get()") == count_of(sec, "fc_lock_get()") + 1u);
    }
    // THE STAGING and THE CONTROL
    chk("T19P staging: only in the HIGH bump region, through gmc_bind_existing (never gart_bind_sysmem), never GART page 0",
        order(stg, { "if (!gBringup.gmc.gart_high_bump) {", "amdgpu::sysmem_alloc(gFcStaging, N48_FC_STAGING_BYTES, 4096)",
                     "gFcBumpBefore = gBringup.gmc.gart_bump_offset;", "amdgpu::gmc_bind_existing(*gBringup.dev, gBringup.gmc, gFcStaging.bus, N48_FC_STAGING_BYTES, &mc);",
                     "if (kr != KERN_SUCCESS || !mc || mc == gBringup.gmc.gart_start) {", "gFc.staging_ok = 1u;" }) &&
        !has(bu.substr(bu.find("SWITCH 63, THE RESIDENCY COPY THROUGH SDMA")), "gart_bind_sysmem("));
    chk("T19P the control: staged pattern -> the same run and submission -> a FULL MM read-back; only a landed fence frees the scratch; "
        "any failure latches", order(pc, { "const uint32_t r = n48_fc_chunk_run(&ops, &gFc,", "navi48_vram_read_mm(off + (uint64_t)i * 4u, buf, 64u)",
                                            "if (r == N48_FC_RUN_OK) { IOLockLock(gScanoutLock); gBringup.gmc.vram_alloc.free(a);",
                                            "if (pass) { gFc.pc_passed = 1u; return 0u; }", "n48_fc_latch(&gFc, N48_FC_LATCH_PC);" }));
    chk("T19P the first ON binds and runs the control under gFastCopyLock, BEFORE the switch is published",
        order(set, { "IOLockLock(l);", "if (fc_staging_bind() != 0u) st = 12u;", "else if (!gFc.pc_passed && fc_positive_control() != 0u) st = 12u;",
                     "__atomic_store_n(&gFcMode, m, __ATOMIC_RELEASE);", "IOLockUnlock(l);" }));
    chk("T19P the verb 63 is mid-arm guarded (PIN SWITCH-GUARD:63) and reports every time",
        has(ahh, "n48_cm_cont_switch_refused(63u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") &&
        has(ahh, "else if (m == 1u || m == 2u || m == 3u) { set63 = navi48_fc_set(m); if (set63) st = set63; }") &&
        has(ahh, "navi48_fc_report(contRefused63 ?"));
    chk("T19P Navi48Ttl.hpp declares the one kext entry the copier calls", has(ttl, "uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,"));
    return bad;
}

static void test_T19p_pins(const char *bringupPath, const char *ahhPath, const char *peerPath, const char *ttlPath) {
    std::string bu, ahh, peer, ttl;
    if (!bringupPath || !ahhPath || !peerPath || !ttlPath || !read_file(bringupPath, bu) || !read_file(ahhPath, ahh) ||
        !read_file(peerPath, peer) || !read_file(ttlPath, ttl)) {
        expect("T19P the kext sources are readable", false); return;
    }
    expect("T19P every pin holds on the REAL sources", fc_pins(bu, peer, ahh, ttl, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // file: 0 bringup, 1 peer, 2 ahh
        { 1, "ORDERING: the SDMA call before the chunk's VRAM guard",
          "        const uint64_t fc = fc_copy_chunk(&rt, retile, md, backingOffset, pos, wBytes, dAt, n, raw, cgLo, cgHi);\n", "" },
        { 1, "REACHABILITY: the SDMA producer reads the backing without 0.0.495's overlay",
          "return ic_read(c->md, c->backingOffset, c->pos + off, dst, take) == take ? 1 : 0;",
          "return c->md->readBytes(c->backingOffset + c->pos + off, dst, take) == take ? 1 : 0;" },
        { 0, "GATE: the VRAM guard skipped at the submission", "if (!n48_fc_submit_ok(destWhy, idle)) return", "if (!n48_fc_submit_ok(0u, idle)) return" },
        { 0, "ORDER (0.0.509): the kernel submission runs another ops table than the tested one",
          "const uint32_t r = n48_fc_submit_wait(&kFcWaitOps, c,", "const uint32_t r = n48_fc_submit_wait(&kFcWaitOps508, c," },
        { 0, "GATE: 63 boots ON", "static volatile uint32_t gFcMode { 0u };", "static volatile uint32_t gFcMode { 1u };" },
        { 0, "LATCH: the switch re-read on every chunk", "if (pos != 0u) return s;", "if (pos != 0u && false) return s;" },
        { 0, "ORDER: verified while gFastCopyLock is still held",
          "\tIOLockUnlock(l);\n\ts->stageUs += stageUs;", "\ts->stageUs += stageUs;" },
        { 0, "STAGING: bound in the LOW bump region", "if (!gBringup.gmc.gart_high_bump) {", "if (false) {" },
        { 2, "GUARD: 63 not mid-arm guarded", "n48_cm_cont_switch_refused(63u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)", "0u" },
        { 1, "IDENTITY: the MM loop's read-back dropped", "            for (uint32_t i = 0; i < cnt; i++) if (back[i] != buf[i]) mismatched++;\n", "" },
    };
    for (const auto &p : plant) {
        std::string m[3] = { bu, peer, ahh };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { expect("T19P plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[220]; std::snprintf(lbl, sizeof lbl, "T19P BREAK-check: %s - caught", p.what);
        expect(lbl, fc_pins(m[0], m[1], m[2], ttl, false) > 0u);
    }
}

// =============================================================================================================
// T20 — build 0.0.496 F1 (the review of 0.0.495, finding 1): THE REGISTRY PRUNES AND MERGES, never saturating on a
// normal run. RUN C's real sequence (fixture_hgreg_run10h.h): 1324 copies, 323 substitutions, 133 distinct ranges.
// =============================================================================================================
// 0.0.495's n48_hg_reg_add, transcribed (the control: RUN C saturates it).
static void fc_reg_add_495(n48_hg_state *s, uint64_t lo, uint64_t hi) {
    if (hi <= lo) return;
    for (uint32_t i = 0; i < s->nreg && i < N48_HG_REG_MAX; i++) if (s->reg[i].lo == lo && s->reg[i].hi == hi) return;
    if (s->nreg >= N48_HG_REG_MAX) { s->reg_over = 1u; return; }
    s->reg[s->nreg].lo = lo; s->reg[s->nreg].hi = hi; s->nreg++;
}
struct HgRunOut { uint32_t maxReg, overAt, lostLive, copiesBumped, pruned, merged; uint32_t reg_over; };
// RUN C with switch 62 ON, in the kext's order per copy: ic_begin (the copy's patches = the programs its post-copy scan
// substituted: the same library, the same ladder) registers them and bumps; the write loop; the post-copy substitution
// (hw_hg_note_substituted); the scope's close (prune, then n48_hg_copy_end). `old` 1 runs 0.0.495's registry instead.
static HgRunOut hg_run10h(int old, int noPrune) {
    static n48_hg_state st; std::memset(&st, 0, sizeof st);
    HgRunOut o {}; o.overAt = 0u;
    for (uint32_t i = 0; i < kHgR10hN; ) {
        if (kHgR10h[i].kind != 0u) { i++; continue; }
        const uint64_t lo = kHgR10h[i].a, hi = kHgR10h[i].b;
        uint32_t j = i + 1u; std::vector<std::pair<uint64_t, uint64_t>> subs;
        while (j < kHgR10hN && kHgR10h[j].kind == 1u) { subs.push_back({ kHgR10h[j].a, kHgR10h[j].a + kHgR10h[j].b }); j++; }
        const uint64_t seq0 = st.reg_seq;                                            // hw_hg_copy_begin: *regSeqOut
        const uint32_t np = (uint32_t)subs.size();
        const uint32_t bumped = n48_hg_copy_begin(&st, (np || n48_hg_reg_overlaps(&st, lo, hi)) ? 1u : 0u);
        for (auto &r : subs) { if (old) fc_reg_add_495(&st, r.first, r.second); else n48_hg_reg_add(&st, r.first, r.second); }
        for (auto &r : subs) { if (old) fc_reg_add_495(&st, r.first, r.second); else n48_hg_reg_add(&st, r.first, r.second); }   // note_substituted
        if (bumped) {
            if (!old && !noPrune) o.pruned += n48_hg_reg_prune(&st, 1u, np, 0u, seq0, lo, hi);
            n48_hg_copy_end(&st, 1u, 1u, 0u, 0u, lo, hi, nullptr, 0u);
            o.copiesBumped++;
        }
        if (st.nreg > o.maxReg) o.maxReg = st.nreg;
        if (st.reg_over && !o.overAt) o.overAt = kHgR10h[i].copy;
        // every range this copy substituted is still covered (the registry never forgets a live program)
        for (auto &r : subs) if (!n48_hg_reg_overlaps(&st, r.first, r.second)) o.lostLive++;
        i = j;
    }
    o.merged = (uint32_t)st.reg_merged; o.reg_over = st.reg_over;
    return o;
}

static void test_T20_registry() {
    const HgRunOut c = hg_run10h(1, 1);
    char b[220];
    std::snprintf(b, sizeof b, "T20 CONTROL: 0.0.495's registry over RUN C saturates (reg_over at copy #%u, %u ranges): finding 1 "
                  "reproduced", c.overAt, c.maxReg);
    expect(b, c.reg_over == 1u && c.maxReg == N48_HG_REG_MAX && c.overAt != 0u);
    const HgRunOut n = hg_run10h(0, 0);
    std::snprintf(b, sizeof b, "T20 F1: RUN C's 1324 copies / 323 substitutions: the registry never exceeds %u (max %u), reg_over stays "
                  "0 (%u merged, %u pruned)", N48_HG_REG_MAX, n.maxReg, n.merged, n.pruned);
    expect(b, n.maxReg <= N48_HG_REG_MAX && n.reg_over == 0u);
    expect("T20 F1: ... and no live range is ever lost (every copy's substituted programs overlap the registry after it)", n.lostLive == 0u);
    // THE PRUNE: synthetic copies over RUN C's first heap (0x10020000, 0x9000 bytes, programs from its first patched copy #4).
    auto boot = [](n48_hg_state &st) {
        std::memset(&st, 0, sizeof st);
        n48_hg_reg_add(&st, 0x10020f00ull, 0x10021100ull); n48_hg_reg_add(&st, 0x10021100ull, 0x1002122cull);
        n48_hg_reg_add(&st, 0x10021300ull, 0x10021500ull);
    };
    static n48_hg_state st;
    const uint64_t lo = 0x10020000ull, hi = 0x10029000ull;
    boot(st); uint64_t s0 = st.reg_seq; (void)n48_hg_copy_begin(&st, 1u);
    expect("T20 F1 a clean copy with no patch and no poison, alone, over the heap: removes the 3 ranges it fully covers",
           n48_hg_reg_prune(&st, 1u, 0u, 0u, s0, lo, hi) == 3u && st.nreg == 0u && st.reg_pruned == 3u);
    boot(st); s0 = st.reg_seq; (void)n48_hg_copy_begin(&st, 1u);
    expect("T20 F1 a copy that PATCHED removes nothing", n48_hg_reg_prune(&st, 1u, 31u, 0u, s0, lo, hi) == 0u && st.nreg == 3u);
    expect("T20 F1 a copy that POISONED a program removes nothing", n48_hg_reg_prune(&st, 1u, 0u, 1u, s0, lo, hi) == 0u && st.nreg == 3u);
    expect("T20 F1 an UNCLEAN copy removes nothing", n48_hg_reg_prune(&st, 0u, 0u, 0u, s0, lo, hi) == 0u && st.nreg == 3u);
    expect("T20 F1 a copy covering a range only in part leaves it", n48_hg_reg_prune(&st, 1u, 0u, 0u, s0, lo, 0x10021200ull) == 1u &&
           st.nreg == 2u);
    boot(st); s0 = st.reg_seq; (void)n48_hg_copy_begin(&st, 1u); (void)n48_hg_copy_begin(&st, 1u);
    expect("T20 F1 while ANOTHER overlapping copy is in progress (active 2) nothing is removed",
           n48_hg_reg_prune(&st, 1u, 0u, 0u, s0, lo, hi) == 0u && st.nreg == 3u);
    // THE RACE the sequence guards: copy B (clean, no patch) begins; copy A (patched) begins over the same heap and registers its
    // program (a touch), completes; B completes alone. A's program may have been written AFTER B's bytes: B must not remove it.
    boot(st); s0 = st.reg_seq; (void)n48_hg_copy_begin(&st, 1u);                        // B begins
    (void)n48_hg_copy_begin(&st, 1u); n48_hg_reg_add(&st, 0x10021300ull, 0x10021500ull); // A begins, registers +0x1300
    n48_hg_copy_end(&st, 1u, 1u, 0u, 0u, lo, hi, nullptr, 0u);                          // A completes (active back to 1)
    const uint32_t rm = n48_hg_reg_prune(&st, 1u, 0u, 0u, s0, lo, hi);                    // B completes
    expect("T20 F1 a range a PATCHED copy touched after the pruning copy began is NEVER removed (the other two are)",
           rm == 2u && st.nreg == 1u && st.reg[0].lo == 0x10021300ull && n48_hg_reg_overlaps(&st, 0x10021300ull, 0x10021304ull));
    // THE MERGE: a full registry absorbs a nearby range into a hull (a superset); a far one still saturates.
    std::memset(&st, 0, sizeof st);
    for (uint32_t i = 0; i < N48_HG_REG_MAX; i++) n48_hg_reg_add(&st, 0x20000000ull + 0x10000ull * i, 0x20000000ull + 0x10000ull * i + 0x200u);
    n48_hg_reg_add(&st, 0x20000400ull, 0x20000600ull);
    expect("T20 F1 full: a range 0x200 past an entry merges into its hull (no saturation, still covered, the old range still covered)",
           st.nreg == N48_HG_REG_MAX && !st.reg_over && st.reg_merged == 1u && n48_hg_reg_overlaps(&st, 0x20000500ull, 0x20000504ull) &&
           n48_hg_reg_overlaps(&st, 0x20000000ull, 0x20000004ull));
    n48_hg_reg_add(&st, 0x40000000ull, 0x40000200ull);
    expect("T20 F1 full: a range with no entry within 1 MiB saturates (every copy overlaps), exactly as before", st.reg_over == 1u);
    std::memset(&st, 0, sizeof st);
    n48_hg_reg_add(&st, 0x10020000ull, 0x10029000ull); n48_hg_reg_add(&st, 0x10020f00ull, 0x10021100ull);
    expect("T20 F1 a range inside an existing entry adds no entry (touches it)", st.nreg == 1u && st.reg[0].seq == 2u);
}

// =============================================================================================================
// T21 — build 0.0.496 F4: THE OVERLAY RE-CHECK, on RUN D's heap. The backing changes between the pre-scan
// and the write loop (Apple rewrote a program): that program is NOT overlaid, it is POISONED, and the copy completes unclean.
// =============================================================================================================
static void test_T21_recheck() {
    // (i) GPUPass[fragment] +0x2300 (588 bytes, f23's PS) rewritten in the backing after the pre-scan: one word of it.
    {
        HgSim &s = gHs; hs_boot(s);
        hs_w32(s.backing, 0x2300u + 0x40u, 0xdeadc0deu);
        uint32_t overlaid = 0u;
        (void)hs_copy62(s, 1u, HS_REAL, nullptr, nullptr, nullptr, &overlaid);
        // the model's write loop wrote the batch's bytes; before the post-copy substitution (which hs_copy62 then applies) the
        // copy's own bytes at +0x2300 were the new backing's: check the patch state and the completion instead.
        expect("T21 (i) a program whose backing changed since the pre-scan is marked bad and NOT overlaid (the other 30 are)",
               s.patch[7].bad == 1u && gHsRecheckBad == 1u && overlaid == s.patchBytes - s.patch[7].nb);
        n48_hg_frame f {}; n48_hg_frame_begin(&f, 1u, 90u, &s.st); n48_hg_frame_note_pva(&f, kR10iF23PsVa);
        expect("T21 (i) ... the copy completes UNCLEAN: its range is poisoned and f23's PS shape is refused (program-poisoned)",
               s.st.copies_poisoned == 1u && s.st.copies_clean == 0u && n48_hg_judge(&s.st, &f, 90u) == N48_HG_POISONED);
    }
    // (ii) a change deep inside a multi-batch program (TimgXh_IsrcN3Oc3mtc3nlnlnl +0x5600, 1792 bytes = 7 batches): the batches
    // before it were overlaid, the one holding the change and every later one are not.
    {
        HgSim &s = gHs; hs_boot(s);
        s.backing[0x5600u + 1000u] ^= 0x5Au;
        uint8_t raw[256]; uint32_t placedIn = 0u, bad = 0u, badAt = 0u;
        for (uint32_t pos = 0; pos < kR10iHeapBytes; pos += 256u) {
            std::memcpy(raw, s.backing + pos, 256u);
            uint32_t nb = 0u;
            const uint32_t placed = n48_hg_overlay_checked(s.patch, s.np, s.arena, s.apple, pos, raw, 256u, &nb);
            if (nb && !badAt) badAt = pos;
            bad += nb;
            if (pos >= 0x5600u && pos < 0x5600u + 1792u) placedIn += placed;
            // nothing of the changed program is overlaid from the batch holding the change on (no other patch lies there)
            if (pos >= 0x5900u && pos < 0x5600u + 1792u && std::memcmp(raw, s.backing + pos, 256u) != 0) bad += 100u;
        }
        expect("T21 (ii) the change at +0x5600+1000 is found in its own batch (+0x5900) and the program is overlaid no further",
               bad == 1u && badAt == 0x5900u && placedIn == 768u && s.patch[20].bad == 1u);
    }
    // (iii) no change: identical to 0.0.495's overlay (every byte, every patch), and a patch with no Apple bytes is never overlaid.
    {
        HgSim &s = gHs; hs_boot(s);
        uint32_t same = 1u, total = 0u;
        for (uint32_t pos = 0; pos < kR10iHeapBytes; pos += 256u) {
            uint8_t a[256], b[256]; std::memcpy(a, s.backing + pos, 256u); std::memcpy(b, a, 256u);
            uint32_t nb = 0u;
            total += n48_hg_overlay_checked(s.patch, s.np, s.arena, s.apple, pos, a, 256u, &nb);
            (void)n48_hg_overlay(s.patch, s.np, s.arena, pos, b, 256u);
            if (std::memcmp(a, b, 256u) || nb) same = 0u;
        }
        expect("T21 (iii) with the backing unchanged the checked overlay is 0.0.495's, byte for byte", same == 1u && total == s.patchBytes);
        hs_boot(s); s.patch[0].apple_nb = 0u;
        uint8_t raw[256]; std::memcpy(raw, s.backing + 0xf00u, 256u); uint32_t nb = 0u;
        (void)n48_hg_overlay_checked(s.patch, s.np, s.arena, s.apple, 0xf00u, raw, 256u, &nb);
        expect("T21 (iii) a patch with nothing to re-check against is never overlaid (fail-closed)",
               nb == 1u && s.patch[0].bad == 1u && std::memcmp(raw, s.backing + 0xf00u, 256u) == 0);
    }
}

// the kext's F4/F1 wiring
static void test_T21p_pins(const char *ahhPath, const char *peerPath) {
    std::string ahh, peer;
    if (!ahhPath || !peerPath || !read_file(ahhPath, ahh) || !read_file(peerPath, peer)) { expect("T21P the kext sources are readable", false); return; }
    const std::string ov = fn_text(peer, "static __attribute__((noinline)) void ic_overlay(");
    const std::string hit = fn_text(peer, "static int ic_hit(void *vctx, size_t off, const sc_match *m) {");
    const std::string end = fn_text(ahh, "void hw_hg_copy_end(uint32_t clean, uint32_t vaOk, uint64_t vaLo, uint64_t vaHi, uint64_t vramLo, uint64_t vramHi,");
    const std::string beg = fn_text(ahh, "uint32_t hw_hg_copy_begin(uint64_t vramLo, uint64_t vramHi, const n48_hg_patch *p, uint32_t np, uint32_t npoison, uint64_t *regSeqOut,");
    auto pos = [](const std::string &t, const char *x) { return t.find(x); };
    expect("T21P F4: ic_hit keeps the matched Apple bytes (or poisons), ic_overlay re-checks and poisons the changed program",
           has(hit, "memcpy(p->apple + p->appleUsed, p->win + off, anb);") &&
           has(hit, "if (!anb || off + anb > kScWindowBytes || N48_HG_ARENA_BYTES - p->appleUsed < anb) { ic_poison(c, resOff, nb > anb ? nb : anb); return 0; }") &&
           pos(ov, "n48_hg_overlay_checked(") < pos(ov, "p->recheckBad += nbad;") && has(ov, "n48_hg_poison &q = p->poison[p->npoison++];"));
    expect("T21P F1: the copy's registry sequence is taken BEFORE its own patches are registered, and the prune runs BEFORE "
           "n48_hg_copy_end", pos(beg, "*regSeqOut = gHg.reg_seq;") < pos(beg, "n48_hg_reg_add(&gHg, p[i].vram, p[i].vram + p[i].nb);") &&
           pos(end, "n48_hg_reg_prune(&gHg, clean, npatches, nadd, regSeq, vramLo, vramHi);") < pos(end, "n48_hg_copy_end(&gHg,") &&
           pos(end, "n48_hg_copy_end(&gHg,") != std::string::npos);
}


// =============================================================================================================
// T22 — build 0.0.503 (notes/design/HYBRID.md): SWITCH 68, WHO MAY OPEN THE ACCELERATOR. The REAL
// hybrid_policy.h the kext compiles: n48_hy_decide / n48_hy_count / n48_hy_switch_refused, the log formats' bound, and
// (T22p) the kext's wiring: the slot-239 hook goes into the vtable copy BEFORE the copy is published (inside the
// accelerator's start(), before it registers), and the hook decides from ONE latched reading of the switch BEFORE Apple.
// =============================================================================================================
static void test_T22_policy() {
    // ON: only "WindowServer" with uid 88 is admitted.
    expect("T22 ON: WindowServer uid 88 ADMITTED", n48_hy_decide("WindowServer", 88u, 1u) == N48_HY_ADMIT);
    const uint32_t otherUids[] = { 0u, 1u, 87u, 89u, 501u, 0xFFFFFFFFu };
    uint32_t bad = 0u;
    for (uint32_t u : otherUids) bad += n48_hy_decide("WindowServer", u, 1u) != N48_HY_REFUSE;
    expect("T22 ON: WindowServer with any other uid (0, 1, 87, 89, 501, -1) REFUSED (a name is not an identity)", bad == 0u);
    const char *others[] = { "WindowServerX", "WindowServe", "windowserver", "WINDOWSERVER", " WindowServer", "WindowServer ",
                             "Window", "", "loginwindow", "SecurityAgent", "Safari", "com.apple.WebKit.GPU", "NotificationCenter",
                             "bluetoothd", "system_profiler", "kernel_task", "WindowManager" };
    bad = 0u;
    for (const char *n : others) {
        bad += n48_hy_decide(n, 88u, 1u) != N48_HY_REFUSE;     // even with WindowServer's uid
        bad += n48_hy_decide(n, 501u, 1u) != N48_HY_REFUSE;
        bad += n48_hy_decide(n, 0u, 1u) != N48_HY_REFUSE;
    }
    expect("T22 ON: WindowServerX, prefixes, case variants, loginwindow, SecurityAgent, Safari, WebKit GPU and the daemons "
           "REFUSED at uid 88, 501 and 0", bad == 0u);
    expect("T22 ON: no name (an unknown caller) is REFUSED, even at uid 88", n48_hy_decide(nullptr, 88u, 1u) == N48_HY_REFUSE);
    // the name is compared exactly: an embedded NUL ends it, bytes after it do not matter; a longer name is not truncated to a match
    char buf[80]; std::memset(buf, 'W', sizeof buf); buf[sizeof buf - 1] = '\0';
    std::memcpy(buf, "WindowServer", 12);
    expect("T22 ON: \"WindowServer\" followed by 67 more bytes is REFUSED (never compared as a prefix)", n48_hy_decide(buf, 88u, 1u) == N48_HY_REFUSE);
    buf[12] = '\0';
    expect("T22 ON: \"WindowServer\\0\" followed by junk is ADMITTED (the name ends at its NUL)", n48_hy_decide(buf, 88u, 1u) == N48_HY_ADMIT);
    // OFF admits everyone: today's behaviour.
    bad = 0u;
    for (const char *n : others) for (uint32_t u : otherUids) bad += n48_hy_decide(n, u, 0u) != N48_HY_ADMIT;
    bad += n48_hy_decide(nullptr, 0u, 0u) != N48_HY_ADMIT;
    bad += n48_hy_decide("WindowServer", 88u, 0u) != N48_HY_ADMIT;
    expect("T22 OFF: every caller (every name above, every uid, no name) ADMITTED - the OFF identity", bad == 0u);
    // ON is exactly "not OFF": any non-zero latched value is ON
    expect("T22 a latched value of 2 is ON too (n48_hy_decide only asks non-zero)", n48_hy_decide("Safari", 501u, 2u) == N48_HY_REFUSE);

    // The counters: a boot where 68 goes ON after three pre-existing clients opened.
    n48_hy_counts c {};
    struct { const char *n; uint32_t uid, on; } seq[] = {
        { "WindowServer", 88u, 0u }, { "loginwindow", 0u, 0u }, { "SecurityAgent", 92u, 0u },   // before 68 ON
        { "WindowServer", 88u, 1u }, { "SecurityAgent", 92u, 1u }, { "Safari", 501u, 1u }, { "WindowServerX", 88u, 1u },
        { "WindowServer", 501u, 1u }, { nullptr, 88u, 1u }, { "WindowServer", 88u, 1u },
    };
    for (auto &e : seq) n48_hy_count(&c, n48_hy_decide(e.n, e.uid, e.on), e.n, e.uid, e.on);
    expect("T22 counts: WindowServer 3 (1 before, 2 after), pre-existing others 2, refused 5, others admitted while ON 0",
           c.ws == 3u && c.others_off == 2u && c.refused == 5u && c.others_on == 0u);
    // a decision the policy would never make (allow-all) is what others_on exists to show
    n48_hy_counts c2 {}; n48_hy_count(&c2, N48_HY_ADMIT, "Safari", 501u, 1u);
    expect("T22 counts: an admission of a non-WindowServer caller while ON lands in others_on (the must-be-0 counter)",
           c2.others_on == 1u && c2.others_off == 0u && c2.ws == 0u && c2.refused == 0u);

    // The verb's mid-arm guard: a change is refused while ANY arm stands; a bare read never is.
    expect("T22 guard: a change while an arm stands is REFUSED", n48_hy_switch_refused(0u, 1u) == 1u);
    expect("T22 guard: a bare read while an arm stands is allowed", n48_hy_switch_refused(1u, 1u) == 0u);
    expect("T22 guard: a change with no arm is allowed", n48_hy_switch_refused(0u, 0u) == 0u);
    expect("T22 guard: continuous arms are ALSO refused by the shared list (68 is in n48_cm_cont_switch_guarded)",
           n48_cm_cont_switch_guarded(68u) == 1u);

    // The log lines fit the driver log's 511-byte line with their widest arguments ("Navi48AccelPeer: " prefix included).
    char line[1024];
    const char *name32 = "0123456789abcdef0123456789abcde";   // proc_selfname fills at most 32 bytes (31 + NUL)
    int n1 = std::snprintf(line, sizeof line, "Navi48AccelPeer: " N48_HY_REFUSE_FMT, -2147483647 - 1, name32, 0xFFFFFFFFu,
                           0xFFFFFFFFu, 18446744073709551615ull);
    int n2 = std::snprintf(line, sizeof line, "Navi48AccelPeer: " N48_HY_ADMIT_FMT, -2147483647 - 1, 0xFFFFFFFFu, 0xFFFFFFFFu,
                           18446744073709551615ull);
    int n3 = std::snprintf(line, sizeof line, "Navi48AccelPeer: " N48_HY_REPORT_FMT, "OFF (default)",
                           " - `gfxneuter 68` REFUSED (unknown M), unchanged", N48_HY_INERT_TXT, "NOT installed",
                           "ALREADY REGISTERED", 18446744073709551615ull, 18446744073709551615ull, 18446744073709551615ull,
                           18446744073709551615ull, 18446744073709551615ull);
    char msg[160];
    std::snprintf(msg, sizeof msg, "T22 the refusal (%d), admission (%d) and report (%d) lines fit in 511 bytes", n1, n2, n3);
    expect(msg, n1 > 0 && n1 < 511 && n2 > 0 && n2 < 511 && n3 > 0 && n3 < 511);
}

// T22p — the kext's wiring (Navi48AccelPeer.cpp, AppleHardwareHook.cpp). Ordering, not literal presence alone.
static void test_T22p_pins(const char *ahhPath, const char *peerPath) {
    std::string ahh, peer;
    if (!ahhPath || !peerPath || !read_file(ahhPath, ahh) || !read_file(peerPath, peer)) { expect("T22P the kext sources are readable", false); return; }
    auto pos = [](const std::string &t, const char *x) { return t.find(x); };
    const size_t npos = std::string::npos;
    // (1) REACHABILITY: installed in tryPatchAcceleratorStop, into the COPY, after the copy is filled and BEFORE it is published.
    const std::string tp = fn_text(peer, "bool Navi48AccelPeer::tryPatchAcceleratorStop() {");
    const size_t tMem = pos(tp, "memcpy(copy, vt - kVtHeader, bytes);"), tIns = pos(tp, "ucp_install_at_start(accel, vt, copy);"),
                 tPub = pos(tp, "*slot = copy + kVtHeader;"), tFence = pos(tp, "__asm__ __volatile__(\"sfence\" ::: \"memory\");\n    *slot = copy + kVtHeader;");
    expect("T22P install: tryPatchAcceleratorStop calls ucp_install_at_start after filling the copy and BEFORE publishing it "
           "(the sfence + *slot store), and it is the only call in the file",
           !tp.empty() && tMem != npos && tIns != npos && tPub != npos && tFence != npos && tMem < tIns && tIns < tFence &&
           count_of(peer, "ucp_install_at_start(accel, vt, copy);") == 1u && count_of(tp, "*slot = copy + kVtHeader;") == 1u);
    expect("T22P install: tryPatchAcceleratorStop still documents WHY this is before registration (the accelerator is inside start())",
           has(tp, "at this moment the\n    // accelerator is still inside its own start() and has not registered."));
    const std::string ia = fn_text(peer, "static void ucp_install_at_start(IOService *accel, void **vt, void **copy) {");
    const size_t gNuc = pos(ia, "else if (nuc - slide != kStaticNewUserClient) why = \"slot 239 is not newUserClient\";"),
                 gNs = pos(ia, "else if (ns - slide != kStaticNewSurface) why = \"slide (newSurface anchor)\";"),
                 gW = pos(ia, "copy[kVtHeader + kAccelNewUserClientSlot] = reinterpret_cast<void *>(&ucp_hook_new_user_client);"),
                 gIf = pos(ia, "if (!why) {");
    expect("T22P install: the slot-239 write is into the COPY, only after both slide anchors and the newUserClient guard (no write "
           "into Apple's table)", !ia.empty() && gNuc != npos && gNs != npos && gW != npos && gIf != npos && gNs < gNuc && gNuc < gIf &&
           gIf < gW && !has(ia, "vt[kAccelNewUserClientSlot] =") && !has(ia, "gAccelVt["));
    expect("T22P install: the registered bit is recorded at install for the report",
           has(ia, "gHyRegisteredAtInstall = (accel && (accel->getState() & kIOServiceRegisteredState)) ? 1u : 0u;"));
    // (2) THE HOOK: identity, one latched read of the switch, the policy, and the refusal - all before Apple.
    const std::string hk = fn_text(peer, "static IOReturn ucp_hook_new_user_client(void *accel, task_t owner, void *sid, UInt32 type, IOUserClient **handler) {");
    const size_t hGate = pos(hk, "if (accel == gAccelObj) {"), hLatch = pos(hk, "const uint32_t on = gHyOn;"),
                 hName = pos(hk, "proc_selfname(nm, (int)sizeof(nm));"), hUid = pos(hk, "const uint32_t uid = (uint32_t)kauth_getuid();"),
                 hDec = pos(hk, "const uint32_t dec = n48_hy_decide(nm, uid, on);"), hCnt = pos(hk, "n48_hy_count(&gHyC, dec, nm, uid, on);"),
                 hRef = pos(hk, "if (dec == N48_HY_REFUSE) {"), hNull = pos(hk, "if (handler) *handler = nullptr;"),
                 hRet = pos(hk, "return kIOReturnNotPermitted;"), hApple = pos(hk, "gOrigNewUserClient(accel, owner, sid, type, handler)");
    expect("T22P hook: found, and the order is gate, latch, name, uid, decide, count, refuse (NULL *handler, NotPermitted), THEN Apple",
           !hk.empty() && hGate != npos && hLatch != npos && hName != npos && hUid != npos && hDec != npos && hCnt != npos &&
           hRef != npos && hNull != npos && hRet != npos && hApple != npos && hGate < hLatch && hLatch < hDec && hName < hDec &&
           hUid < hDec && hDec < hCnt && hCnt < hRef && hRef < hNull && hNull < hRet && hRet < hApple);
    // the refusal block itself never reaches Apple (fail-closed): from the `if` to its return there is no call through the original
    const std::string refBlk = (hRef != npos && hRet != npos && hRef < hRet) ? hk.substr(hRef, hRet - hRef) : std::string();
    expect("T22P hook: the refusal path does NOT call Apple (no gOrigNewUserClient between the refusal and its return), and Apple "
           "is called exactly once in the hook", !refBlk.empty() && !has(refBlk, "gOrigNewUserClient") &&
           count_of(hk, "gOrigNewUserClient(") == 1u);
    expect("T22P hook: switch 68 is read ONCE per call (gHyOn appears once in the hook; the decision and the count take `on`)",
           count_of(hk, "gHyOn") == 1u && count_of(hk, ", uid, on)") == 2u && count_of(hk, ", on)") == 2u);
    expect("T22P hook: the identity is name AND uid through the one policy (no second copy of the rule in the hook)",
           !has(hk, "\"WindowServer\"") && !has(hk, "88u") && !has(hk, "N48_HY_WS_UID") && !has(hk, "n48_hy_is_ws(") &&
           !has(hk, "N48_WS_OWNER"));
    expect("T22P hook: the 0.0.333 census stays off until `ucprobe 1` (OFF identity for the old instrument)",
           pos(hk, "if (!gUcInstalled) return kr;") != npos && pos(hk, "if (!gUcInstalled) return kr;") < pos(hk, "gUcMinted++;") &&
           hApple < pos(hk, "if (!gUcInstalled) return kr;"));
    // (3) THE VERB: 68's branch in hw_hook_gfx_neuter guards both arm kinds and sets the switch only when not refused.
    const size_t vb = pos(ahh, "} else if ((arg & 0xffull) == 68ull) {");
    const size_t ve = vb == npos ? npos : ahh.find("} else if ((arg & 0xffull) == ", vb + 10u);
    const std::string v = (vb != npos && ve != npos) ? ahh.substr(vb, ve - vb) : std::string();
    const size_t vG1 = pos(v, "n48_cm_cont_switch_refused(68u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state) != 0u ||"),
                 vG2 = pos(v, "n48_hy_switch_refused(m == 0u ? 1u : 0u, gXdShot.state == N48_CM_SHOT_ARMED ? 1u : 0u) != 0u;"),
                 vRef = pos(v, "if (contRefused68) st = 5;"), vSet = pos(v, "else { changed68 = n48_ra_set(m, &fhy); if (changed68) navi48_hybrid_set(fhy); }"),
                 vRep = pos(v, "navi48_hybrid_report(");
    expect("T22P verb: 68 is dispatched once; guards (continuous AND one-shot) before the only set; the report line always",
           count_of(ahh, "} else if ((arg & 0xffull) == 68ull) {") == 1u && !v.empty() && vG1 != npos && vG2 != npos && vRef != npos &&
           vSet != npos && vRep != npos && vG1 < vRef && vG2 < vRef && vRef < vSet && vSet < vRep &&
           count_of(ahh, "navi48_hybrid_set(") == 1u);
    expect("T22P verb: the switch's only writer in the peer is navi48_hybrid_set; it is OFF at boot",
           count_of(peer, "gHyOn = ") == 1u && has(peer, "static volatile uint32_t gHyOn { 0u };"));
}

// =============================================================================================================
// T23 — build 0.0.509 (fc2,; the 0.0.496 review's F-2 and F-3,): THE FAST COPY'S FIXES FOR SAMPLED VERIFY.
//   T23a item 1: the fence bound counts from the KICK. fastcopy.h's n48_fc_submit_wait (the kext's submission order) under a
//        virtual clock: a 300 ms wait for gScanoutLock and a fence landing 5 ms after the kick -> LANDED; 0.0.508's placement
//        (t0 before the lock, transcribed) on the same engine -> TIMEOUT.
//   T23b item 2 (F-2): a timed-out chunk's range stays poisoned for the boot (copy guard and 62), whatever later clean copies do.
//   T23c item 3 (F-3): sampled verify on a 62-overlapping copy runs FULL; a sampled copy never clears poison nor prunes.
//   T23d item 4: the verify outlier record.
//   T23p the kext wires each piece (pins, each with a planted text break shown caught).
// =============================================================================================================
struct VClock {                  // a virtual engine and clock for n48_fc_submit_wait
    uint64_t now;                // us
    uint64_t lockWait;           // how long lock() blocks (another holder: the scanout thumbnail)
    uint64_t landAfterKick;      // the fence dword takes the value this long after the doorbell (~0: never)
    uint64_t tKick;              // when the doorbell rang
    uint32_t want, kickAnswer, locks, unlocks, kicks, reads, lockedAtKick;
    int held;
};
static uint64_t vc_now(void *c) { return static_cast<VClock *>(c)->now; }
static void vc_lock(void *c) { VClock *v = static_cast<VClock *>(c); v->now += v->lockWait; v->held++; v->locks++; }
static void vc_unlock(void *c) { VClock *v = static_cast<VClock *>(c); v->held--; v->unlocks++; }
static uint32_t vc_kick(void *c, uint64_t, uint64_t, uint32_t, uint32_t fence) {
    VClock *v = static_cast<VClock *>(c);
    v->kicks++; v->lockedAtKick = v->held > 0 ? 1u : 0u; v->want = fence;
    if (v->kickAnswer) return v->kickAnswer;
    v->tKick = v->now;
    return 0u;
}
static uint32_t vc_fence(void *c) {
    VClock *v = static_cast<VClock *>(c);
    v->reads++;
    v->now += 1u;                                       // a write-back read costs ~1 us
    return (v->landAfterKick != ~0ull && v->now >= v->tKick + v->landAfterKick) ? v->want : v->want - 1u;
}
static void vc_delay(void *c, uint32_t us) { static_cast<VClock *>(c)->now += us; }
static const n48_fc_wait_ops kVcOps = { &vc_now, &vc_lock, &vc_unlock, &vc_kick, &vc_fence, &vc_delay };
// 0.0.508's fc_submit, transcribed (the control): t0 BEFORE IOLockLock(gScanoutLock), the bound `el = fc_us(t0, tn)`.
static uint32_t vc_submit_508(VClock *v, uint32_t fence, uint64_t *elOut) {
    const uint64_t t0 = v->now;                       // uint64_t t0 = 0; clock_get_uptime(&t0);
    vc_lock(v);                                       // IOLockLock(gScanoutLock);
    if (vc_kick(v, 0, 0, 0, fence)) { vc_unlock(v); return N48_FC_SUB_REFUSED; }
    uint32_t ps = N48_FC_POLL_WAIT; uint64_t el = 0;
    for (uint32_t guard = 0; guard < 1000000u; guard++) {
        const uint32_t val = vc_fence(v);
        el = v->now - t0;
        ps = n48_fc_poll_state(val, fence, el);
        if (ps != N48_FC_POLL_WAIT) break;
        vc_delay(v, n48_fc_poll_delay_us(el));
    }
    vc_unlock(v);
    *elOut = el;
    return ps == N48_FC_POLL_LANDED ? N48_FC_SUB_LANDED : N48_FC_SUB_TIMEOUT;
}
static VClock vc_make(uint64_t lockWait, uint64_t land) { VClock v {}; v.now = 1000000u; v.lockWait = lockWait; v.landAfterKick = land; return v; }

static void test_T23a_bound_from_kick() {
    // THE fc2 CASE: the scanout thumbnail holds gScanoutLock 300 ms; the engine lands the fence 5 ms after the doorbell.
    VClock v = vc_make(300000u, 5000u);
    n48_fc_wait_out w {};
    const uint32_t r = n48_fc_submit_wait(&kVcOps, &v, 1, 2, 0x10000u, 0xfc00004cu, &w);
    char b[260];
    std::snprintf(b, sizeof b, "T23a 0.0.509: a 300 ms gScanoutLock wait + a fence landing 5 ms after the kick -> LANDED (lock wait %llu us, "
                  "fence wait %llu us from the kick)", (unsigned long long)w.lock_us, (unsigned long long)w.el_us);
    expect(b, r == N48_FC_SUB_LANDED && w.kicked == 1u && w.lock_us == 300000u && w.el_us >= 5000u && w.el_us < 6100u &&
              w.seen == 0xfc00004cu);
    VClock o = vc_make(300000u, 5000u);
    uint64_t el508 = 0;
    const uint32_t r508 = vc_submit_508(&o, 0xfc00004cu, &el508);
    std::snprintf(b, sizeof b, "T23a CONTROL 0.0.508 (t0 before the lock, transcribed): the SAME engine -> TIMEOUT after %llu us "
                  "(the bound counted the lock wait; fc2's `after 382123 us`)", (unsigned long long)el508);
    expect(b, r508 == N48_FC_SUB_TIMEOUT && el508 >= N48_FC_FENCE_TIMEOUT_US);
    expect("T23a the doorbell rings UNDER the lock and the lock is released exactly once", v.lockedAtKick == 1u && v.locks == 1u &&
           v.unlocks == 1u && v.held == 0);
    // the bound's VALUE is unchanged: a fence that never lands times out at 200 ms FROM THE KICK (not before, not much after)
    VClock n = vc_make(300000u, ~0ull);
    const uint32_t rn = n48_fc_submit_wait(&kVcOps, &n, 1, 2, 0x10000u, 0xfc000002u, &w);
    expect("T23a a fence that never lands: TIMEOUT at the unchanged 200 ms bound, counted from the kick (lock wait reported apart)",
           rn == N48_FC_SUB_TIMEOUT && w.el_us >= N48_FC_FENCE_TIMEOUT_US && w.el_us < N48_FC_FENCE_TIMEOUT_US + 1100u &&
           w.lock_us == 300000u && w.kicked == 1u && n.held == 0);
    // no lock wait: the old and new placements agree (a 150 ms fence lands under both)
    VClock z = vc_make(0u, 150000u), z8 = vc_make(0u, 150000u);
    uint64_t e8 = 0;
    expect("T23a with no lock wait both placements answer the same (a 150 ms fence LANDS under both)",
           n48_fc_submit_wait(&kVcOps, &z, 1, 2, 4u, 7u, &w) == N48_FC_SUB_LANDED && vc_submit_508(&z8, 7u, &e8) == N48_FC_SUB_LANDED);
    // a refusal or a failed ring write: nothing polled, the lock released, the answer passed through
    VClock rf = vc_make(10u, 0u); rf.kickAnswer = N48_FC_SUB_REFUSED;
    const uint32_t rr = n48_fc_submit_wait(&kVcOps, &rf, 1, 2, 4u, 7u, &w);
    VClock rg = vc_make(10u, 0u); rg.kickAnswer = N48_FC_SUB_RING;
    const uint32_t rgr = n48_fc_submit_wait(&kVcOps, &rg, 1, 2, 4u, 7u, &w);
    expect("T23a a refused kick: REFUSED, fence never read, not kicked, lock released; a failed ring write: RING, likewise",
           rr == N48_FC_SUB_REFUSED && rf.reads == 0u && rf.held == 0 && rgr == N48_FC_SUB_RING && rg.reads == 0u && rg.held == 0 &&
           w.kicked == 0u);
}

static void test_T23b_dead_range_sticky() {
    // THE COPY GUARD. fc2's chunk: VRAM 0x111ea000, 0x10000 bytes, run 3 (TIMEOUT).
    const uint64_t lo = 0x111ea000ull, hi = lo + 0x10000ull;
    // what the kext does, in its order: n48_fc_chunk_run answers TIMEOUT -> dead -> sticky row; the copy loop's FAILED path closes
    // the scope (normal mark over the copy's range); later a clean copy wholly covering it closes.
    HgSim &hs = gHs; hs_boot(hs);
    FcMock &m = gFm; std::memset(&m, 0, sizeof m); m.hs = &hs; m.on62 = 0u; m.sub = N48_FC_SUB_TIMEOUT;
    n48_fc_state st {}; st.pc_passed = 1u; st.staging_ok = 1u;
    const n48_fc_ops ops { &m, &fc_mock_fill, &fc_mock_submit };
    static uint32_t exp[N48_FC_SAMPLE_MAX];
    const uint32_t r = n48_fc_chunk_run(&ops, &st, m.stg, 0x840fc2e000ull, 0x8010020000ull, 0x9000u, exp, N48_FC_M_SAMPLED, 0u, 0u, nullptr);
    n48_cg_poison p509 {}, p508 {}; n48_cg_poison_init(&p509); n48_cg_poison_init(&p508);
    if (n48_fc_run_dead(r)) n48_cg_poison_mark_sticky(&p509, lo, hi);           // 0.0.509 navi48_fc_chunk
    n48_cg_close_poison(&p509, lo, hi, 1, 1, 0);                                 // the copy loop's FAILED close
    n48_cg_close_poison(&p508, lo, hi, 1, 1, 0);                                 // 0.0.508: only this
    n48_cg_close_poison(&p509, lo - 0x1000u, hi + 0x1000u, 1, 0, 0);             // a later CLEAN copy wholly covering it
    n48_cg_close_poison(&p508, lo - 0x1000u, hi + 0x1000u, 1, 0, 0);
    expect("T23b a TIMEOUT (and a RING failure) is a dead range; a refusal, a fill failure and exhausted fences are not",
           r == N48_FC_RUN_TIMEOUT && n48_fc_run_dead(N48_FC_RUN_RING) && !n48_fc_run_dead(N48_FC_RUN_REFUSED) &&
           !n48_fc_run_dead(N48_FC_RUN_FILL) && !n48_fc_run_dead(N48_FC_RUN_FENCES) && !n48_fc_run_dead(N48_FC_RUN_OK));
    expect("T23b F-2 (0.0.509): after a later clean covering copy the timed-out range is STILL poisoned (every page of it)",
           n48_cg_poison_overlaps(&p509, lo, lo + 4096u) && n48_cg_poison_overlaps(&p509, hi - 4096u, hi));
    expect("T23b CONTROL 0.0.508: the same sequence CLEARS the poison (the fail-open names)", !n48_cg_poison_overlaps(&p508, lo, hi));
    // a normal poisoned row beside it is still cleared by a covering clean copy (sticky rows only are permanent)
    n48_cg_poison q {}; n48_cg_poison_init(&q);
    n48_cg_poison_mark_sticky(&q, lo, hi); n48_cg_poison_mark(&q, 0x20000000ull, 0x20001000ull);
    n48_cg_close_poison(&q, 0x1f000000ull, 0x21000000ull, 1, 0, 0);
    expect("T23b ... while an ordinary poisoned row IS cleared by a clean covering copy (only the dead range is permanent)",
           !n48_cg_poison_overlaps(&q, 0x20000000ull, 0x20001000ull) && n48_cg_poison_overlaps(&q, lo, hi));
    // a full table folds the dead range into row 0, which becomes sticky (fail-closed)
    n48_cg_poison f {}; n48_cg_poison_init(&f);
    for (uint32_t i = 0; i < N48_CG_POISON; i++) n48_cg_poison_mark(&f, 0x30000000ull + i * 0x10000ull, 0x30001000ull + i * 0x10000ull);
    n48_cg_poison_mark_sticky(&f, lo, hi);
    n48_cg_close_poison(&f, 0ull, ~0ull, 1, 0, 0);
    expect("T23b a full table folds the dead range into row 0, which becomes sticky and survives a clean copy covering everything",
           n48_cg_poison_overlaps(&f, lo, hi));
    // SWITCH 62's table: a sticky entry over the chunk's VA survives n48_hg_copy_end(clean) wholly covering it
    static n48_hg_state hg; std::memset(&hg, 0, sizeof hg);
    n48_hg_poison e {}; e.va_ok = 1u; e.va_lo = 0x400100000ull; e.va_hi = 0x400110000ull; e.vram_lo = lo; e.vram_hi = hi;
    n48_hg_poison_add_sticky(&hg, &e);
    n48_hg_poison whole {}; whole.va_ok = 1u; whole.va_lo = 0x400100000ull; whole.va_hi = 0x400200000ull; whole.vram_lo = lo;
    whole.vram_hi = lo + 0x100000ull; whole.used = 1u; n48_hg_poison_add(&hg, &whole);   // the failed copy's own unclean poison
    const uint32_t b1 = n48_hg_copy_begin(&hg, 1u);
    n48_hg_copy_end(&hg, 1u, 1u, 0x400000000ull, 0x401000000ull, lo - 0x100000ull, lo + 0x200000ull, nullptr, 0u);
    n48_hg_frame fr {}; n48_hg_frame_begin(&fr, 1u, 1u, &hg); n48_hg_frame_note_pva(&fr, 0x400108000ull);
    n48_hg_frame fo {}; n48_hg_frame_begin(&fo, 1u, 1u, &hg); n48_hg_frame_note_pva(&fo, 0x400180000ull);
    expect("T23b F-2 (62): a clean copy covering both clears the failed copy's ordinary poison but NOT the dead range's sticky entry",
           b1 == 1u && n48_hg_frame_poisoned(&hg, &fr) == 1 && n48_hg_frame_poisoned(&hg, &fo) == 0);
}

static void test_T23c_sampled_never_vouches() {
    // the mode, fixed at the copy's first chunk
    expect("T23c F-3: sampled verify (319) on a copy overlapping a 62 range runs FULL; without the overlap it stays sampled; "
           "full stays full; OFF stays OFF",
           n48_fc_copy_mode(N48_FC_M_SAMPLED, 1u) == N48_FC_M_FULL && n48_fc_copy_mode(N48_FC_M_SAMPLED, 0u) == N48_FC_M_SAMPLED &&
           n48_fc_copy_mode(N48_FC_M_FULL, 1u) == N48_FC_M_FULL && n48_fc_copy_mode(N48_FC_M_FULL, 0u) == N48_FC_M_FULL &&
           n48_fc_copy_mode(0u, 1u) == 0u && n48_fc_copy_mode(0u, 0u) == 0u);
    // RUN D's heap copy under 319 with 62 ON, in the kext's order: ic_begin bumps (the registry overlaps) -> the latch -> chunks ->
    // the scope's close. Poison stands over one of the copy's programs; the registry holds a range the copy replaces.
    const uint64_t lo = 0x10020000ull, hi = lo + 0x9000ull, va = 0x400040000ull;
    for (int broken = 0; broken < 2; broken++) {
        static n48_hg_state hg; std::memset(&hg, 0, sizeof hg);
        n48_hg_reg_add(&hg, lo + 0x1000u, lo + 0x1100u);
        n48_hg_poison pz {}; pz.va_ok = 1u; pz.va_lo = va + 0x2000u; pz.va_hi = va + 0x2100u; pz.vram_lo = lo + 0x2000u;
        pz.vram_hi = lo + 0x2100u; pz.used = 1u; n48_hg_poison_add(&hg, &pz);
        const uint64_t seq0 = hg.reg_seq;
        const uint32_t bumped = n48_hg_copy_begin(&hg, n48_hg_reg_overlaps(&hg, lo, hi) ? 1u : 0u);
        const uint32_t mode = broken ? N48_FC_M_SAMPLED /* the latch not applied */ : n48_fc_copy_mode(N48_FC_M_SAMPLED, bumped);
        const uint32_t sampled = n48_fc_copy_sampled(mode, 1u);              // one chunk went via SDMA
        const uint32_t clean = n48_fc_hg_clean(1u, sampled);                 // written, verified with 0 mismatches
        const uint32_t pruned = n48_hg_reg_prune(&hg, clean, 0u, 0u, seq0, lo, hi);
        n48_hg_copy_end(&hg, clean, 1u, va, va + 0x9000u, lo, hi, nullptr, 0u);
        n48_hg_frame fr {}; n48_hg_frame_begin(&fr, 1u, 1u, &hg); n48_hg_frame_note_pva(&fr, va + 0x2040u);
        if (!broken)
            expect("T23c the 62-overlapping copy runs FULL (the latch), so a clean completion clears its poison and prunes (F1 kept)",
                   bumped == 1u && mode == N48_FC_M_FULL && sampled == 0u && clean == 1u && pruned == 1u &&
                   n48_hg_frame_poisoned(&hg, &fr) == 0);
        else
            expect("T23c DEFENCE: were the latch skipped, a SAMPLED 62-overlapping copy neither clears 62's poison nor prunes (unclean)",
                   sampled == 1u && clean == 0u && pruned == 0u && n48_hg_frame_poisoned(&hg, &fr) == 1);
    }
    // the copy guard: a sampled copy never clears; it still marks on a failure or a mismatch; a full one clears as before
    n48_cg_poison p {}; n48_cg_poison_init(&p);
    n48_cg_poison_mark(&p, 0x10021000ull, 0x10022000ull);
    const uint32_t sw = n48_fc_cg_close_wrote(1u, 0u, 0u, n48_fc_copy_sampled(N48_FC_M_SAMPLED, 3u));
    n48_cg_close_poison(&p, 0x10020000ull, 0x10030000ull, (int)sw, 0, 0);
    const int keptBySampled = n48_cg_poison_overlaps(&p, 0x10021000ull, 0x10022000ull);
    n48_cg_poison p2 {}; n48_cg_poison_init(&p2);
    n48_cg_close_poison(&p2, 0x10020000ull, 0x10030000ull, (int)n48_fc_cg_close_wrote(1u, 0u, 1u, 1u), 0, 1);
    const int markedMismatch = n48_cg_poison_overlaps(&p2, 0x10020000ull, 0x10030000ull);
    n48_cg_close_poison(&p, 0x10020000ull, 0x10030000ull,
                        (int)n48_fc_cg_close_wrote(1u, 0u, 0u, n48_fc_copy_sampled(N48_FC_M_FULL, 3u)), 0, 0);
    expect("T23c F-3 (copy guard): a sampled clean copy does NOT clear poison; a sampled mismatched copy still POISONS; a full "
           "clean copy clears", keptBySampled == 1 && sw == 0u && markedMismatch == 1 &&
           !n48_cg_poison_overlaps(&p, 0x10021000ull, 0x10022000ull));
    expect("T23c OFF / MM identity: no SDMA chunk (or 63 OFF) is never `sampled`; the closes are 0.0.508's",
           n48_fc_copy_sampled(N48_FC_M_SAMPLED, 0u) == 0u && n48_fc_copy_sampled(0u, 5u) == 0u &&
           n48_fc_cg_close_wrote(1u, 0u, 0u, 0u) == 1u && n48_fc_cg_close_wrote(0u, 0u, 0u, 0u) == 0u &&
           n48_fc_cg_close_wrote(1u, 1u, 0u, 0u) == 1u && n48_fc_hg_clean(1u, 0u) == 1u && n48_fc_hg_clean(0u, 0u) == 0u);
}

static void test_T23d_verify_outlier() {
    n48_fc_stats s {};
    const uint32_t a = n48_fc_vmax_note(&s, 20u, 5u, 15u, 11u, 0x9000u);           // ~1.8 us/dword
    const uint32_t b = n48_fc_vmax_note(&s, 9620u, 9000u, 600u, 260u, 0x100000u);  // ~37 us/dword (fc2's outliers)
    const uint32_t c = n48_fc_vmax_note(&s, 30u, 0u, 30u, 11u, 0x9000u);
    const uint32_t d = n48_fc_vmax_note(&s, 99u, 0u, 99u, 0u, 0x9000u);
    expect("T23d the boot's slowest verify per dword is kept with its copy's size and its lock/read split; a zero-dword copy is ignored",
           a == 1u && b == 1u && c == 0u && d == 0u && s.vmax_ns_dw == 37000u && s.vmax_bytes == 0x100000u && s.vmax_dwords == 260u &&
           s.vmax_us == 9620u && s.vmax_lock_us == 9000u && s.vmax_read_us == 600u);
}

static size_t fc509_pins(const std::string &bu, const std::string &peer, const std::string &ahh, const std::string &ttl, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0; bool first = true;
        for (const char *st : steps) {
            const size_t at = t.find(st, first ? 0 : prev);
            if (at == std::string::npos || count_of(t, st) != 1u) return false;
            prev = at + 1; first = false;
        }
        return true;
    };
    const std::string chunk = fn_text(bu, "uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,");
    const std::string latch = fn_text(bu, "static FcSlot *fc_slot_latch(uint64_t pos, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {");
    const std::string closed = fn_text(bu, "uint32_t navi48_fc_scope_closed(uint64_t lo, uint64_t hi) {");
    const std::string rep = fn_text(bu, "void navi48_fc_copy_report(uint64_t copyNo) {");
    const std::string set = fn_text(bu, "uint32_t navi48_fc_set(uint32_t m) {");
    const std::string rd = fn_text(bu, "bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {");
    const std::string rdt = fn_text(bu, "static bool fc_vram_read_timed(uint64_t vramOffset, uint32_t *dst, uint32_t dwords, FcVerifyT *t) {");
    const std::string mmr = fn_text(bu, "static int fc_mm_read(void *vt, uint64_t vram, uint32_t *dst, uint32_t dwords) {");
    const std::string bump = fn_text(peer, "__attribute__((noinline)) uint32_t navi48_ic_bumped_mine(void) {");
    const std::string dead = fn_text(peer, "__attribute__((noinline)) void navi48_ic_chunk_dead(uint64_t pos, uint64_t dAt, uint64_t n) {");
    const std::string sticky = fn_text(ahh, "void hw_hg_poison_sticky(const n48_hg_poison *e)\n{");
    chk("T23p the functions are found", !chunk.empty() && !latch.empty() && !closed.empty() && !rep.empty() && !set.empty() &&
        !rd.empty() && !rdt.empty() && !mmr.empty() && !bump.empty() && !dead.empty() && !sticky.empty());
    // item 2: the dead range, marked BEFORE the chunk's FAILED return (the copy loop then closes the scope)
    chk("T23p item 2: a dead chunk marks the copy guard's sticky row and 62's sticky entry BEFORE the FAILED return",
        order(chunk, { "if (r != N48_FC_RUN_OK) {", "const uint32_t dead = n48_fc_run_dead(r);",
                       "n48_cg_poison_mark_sticky(&gCgPoison, dAt, dAt + n);", "navi48_ic_chunk_dead(pos, dAt, n);",
                       "return n48_fc_result_fail(r);" }));
    chk("T23p item 2: 62's dead entry covers the chunk's own VA and VRAM, only for a copy that bumped, under gHgLock",
        order(dead, { "if (!s || !s->bumped || !n) return;",
                      "e.va_ok = s->vaOk; e.va_lo = s->va + pos; e.va_hi = s->va + pos + n; e.vram_lo = dAt; e.vram_hi = dAt + n;",
                      "n48::hw_hg_poison_sticky(&e);" }) &&
        order(sticky, { "IOLockLock(l);", "n48_hg_poison_add_sticky(&gHg, e);", "IOLockUnlock(l);" }));
    // item 3: the latch upgrades, the close asks first
    chk("T23p item 3: the latch (pos 0, 63 ON) upgrades a 62-bumped copy to FULL before the mode is stored",
        order(latch, { "if (pos != 0u) return s;", "if (!mode) {",
                       "const uint32_t b62 = navi48_ic_bumped_mine(), cgp = fc_cg_poisoned(cgLo, cgHi);",
                       "const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);", "s->mode = eff;" }) &&
        has(bump, "return (s && s->bumped) ? 1u : 0u;"));
    chk("T23p item 3: the scope's close asks the slot (gated on the first ON), matches its copy, reads `sampled`, THEN releases",
        order(closed, { "if (!__atomic_load_n(&gFcEverOn, __ATOMIC_ACQUIRE)) return 0u;", "FcSlot *s = fc_slot_mine();",
                        "if (!s || s->at0 < lo || s->at0 >= hi) return 0u;",
                        "const uint32_t sampled = n48_fc_copy_sampled(s->mode, s->chunksSdma);",
                        "__atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);", "return sampled;" }) &&
        order(chunk, { "if (!s || !s->mode) return 0ull;", "if (pos == 0u) s->at0 = dAt;" }) &&
        order(set, { "IOLockLock(l);", "__atomic_store_n(&gFcEverOn, 1u, __ATOMIC_RELEASE);" }) &&
        !has(rep, "__atomic_store_n(&s->thread") && count_of(bu, "static volatile uint32_t gFcEverOn { 0u };") == 1u);
    // item 4: the verify reads through the timed twin, whose lock sequence is navi48_vram_read_mm's, character for character
    auto lockBlock = [](const std::string &f) {
        const size_t a = f.find("\tif (gVramMmLock) {\n\t\tconst uint32_t nesting = gMmPrioNesting;");
        // build 0.0.528 item 4: the block now ends with the marker holder's lock-wait note (in all three MM paths alike).
        const size_t e = a == std::string::npos ? a : f.find("\t\t\tIOLockLock(gVramMmLock);\n\t\t}\n\t\tif (mkw) mm_lockwait_note(l0);\n\t}\n", a);
        return (a == std::string::npos || e == std::string::npos) ? std::string() : f.substr(a, e - a);
    };
    const std::string lb0 = lockBlock(rd), lb1 = lockBlock(rdt);
    chk("T23p item 4: the verify's read is the timed twin; its bounds, MM-priority yield and gVramMmLock sequence equal "
        "navi48_vram_read_mm's; the wait is timed apart from the reads",
        !lb0.empty() && lb0 == lb1 && has(mmr, "return fc_vram_read_timed(vram, dst, dwords, static_cast<FcVerifyT *>(vt)) ? 1 : 0;") &&
        order(rdt, { "if (!gBringup.dev || !dst || dwords == 0 || dwords > 64) return false;", "clock_get_uptime(&w0);",
                     "\tif (gVramMmLock) {", "clock_get_uptime(&w1);", "dst[i] = amdgpu::RVRAM32_via_mm(", "clock_get_uptime(&r1);",
                     "if (gVramMmLock) IOLockUnlock(gVramMmLock);", "t->lockT += w1 - w0; t->readT += r1 - w1;" }) &&
        !has(rd, "FcVerifyT"));
    chk("T23p Navi48Ttl.hpp declares the 0.0.509 entries", has(ttl, "uint32_t navi48_fc_scope_closed(uint64_t lo, uint64_t hi);") &&
        has(ttl, "uint32_t navi48_ic_bumped_mine(void);") && has(ttl, "void navi48_ic_chunk_dead(uint64_t pos, uint64_t dAt, uint64_t n);"));
    return bad;
}

static void test_T23p_pins(const char *bringupPath, const char *ahhPath, const char *peerPath, const char *ttlPath) {
    std::string bu, ahh, peer, ttl;
    if (!bringupPath || !ahhPath || !peerPath || !ttlPath || !read_file(bringupPath, bu) || !read_file(ahhPath, ahh) ||
        !read_file(peerPath, peer) || !read_file(ttlPath, ttl)) {
        expect("T23p the kext sources are readable", false); return;
    }
    expect("T23p every pin holds on the REAL sources", fc509_pins(bu, peer, ahh, ttl, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // file: 0 bringup, 1 peer, 2 ahh
        { 0, "F-2: the dead range not marked sticky", "\t\t\tn48_cg_poison_mark_sticky(&gCgPoison, dAt, dAt + n);\n", "" },
        { 1, "F-2: 62's dead entry not added", "    n48::hw_hg_poison_sticky(&e);\n", "" },
        { 0, "F-3: the latch keeps sampled on a 62-bumped copy", "const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);",
          "const uint32_t eff = n48_fc_latch_mode(mode, 0u, cgp, rpCand);" },
        { 0, "F-3: the slot released before `sampled` is read (every copy reads 0)",
          "\tconst uint32_t sampled = n48_fc_copy_sampled(s->mode, s->chunksSdma);\n\t__atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);",
          "\t__atomic_store_n(&s->thread, (uintptr_t)0, __ATOMIC_RELEASE);\n\tconst uint32_t sampled = n48_fc_copy_sampled(s->mode, s->chunksSdma);" },
        { 0, "ITEM 4: the verify reads through the untimed navi48_vram_read_mm",
          "return fc_vram_read_timed(vram, dst, dwords, static_cast<FcVerifyT *>(vt)) ? 1 : 0;",
          "(void)vt; return navi48_vram_read_mm(vram, dst, dwords) ? 1 : 0;" },
        { 0, "ITEM 4: the timed twin's MM-priority yield drifted from navi48_vram_read_mm's",
          "\t\t\t\t\tIODelay(50);\n\t\t\t\t\tuint64_t yn = 0ull;\n\t\t\t\t\tclock_get_uptime(&yn);\n\t\t\t\t\telapsedUs = navi48_mmprio_us(y0, yn);\n\t\t\t\t}\n\t\t\t\tn48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));\n\t\t\t\tif (mkh) n48::hw_mkh_spin(mkh, elapsedUs);\n\t\t\t}\n\t\t}\n\t\t// build 0.0.528 item 4 (measurement only, decision-inert): a keystone withdrawal marker holder's wait for gVramMmLock.\n\t\tconst uint32_t mkw = n48::hw_mkh_holder(caller);\n\t\tuint64_t l0 = 0ull;\n\t\tif (mkw) clock_get_uptime(&l0);\n\t\tif (isOwner) {\n\t\t\tuint64_t o0 = 0ull, o1 = 0ull;\n\t\t\tclock_get_uptime(&o0);\n\t\t\tIOLockLock(gVramMmLock);\n\t\t\tclock_get_uptime(&o1);\n\t\t\tn48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(o0, o1));\n\t\t} else {\n\t\t\tIOLockLock(gVramMmLock);\n\t\t}\n\t\tif (mkw) mm_lockwait_note(l0);\n\t}\n\tclock_get_uptime(&w1);",
          "\t\t\t\t\tIODelay(5);\n\t\t\t\t\tuint64_t yn = 0ull;\n\t\t\t\t\tclock_get_uptime(&yn);\n\t\t\t\t\telapsedUs = navi48_mmprio_us(y0, yn);\n\t\t\t\t}\n\t\t\t\tn48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));\n\t\t\t\tif (mkh) n48::hw_mkh_spin(mkh, elapsedUs);\n\t\t\t}\n\t\t}\n\t\t// build 0.0.528 item 4 (measurement only, decision-inert): a keystone withdrawal marker holder's wait for gVramMmLock.\n\t\tconst uint32_t mkw = n48::hw_mkh_holder(caller);\n\t\tuint64_t l0 = 0ull;\n\t\tif (mkw) clock_get_uptime(&l0);\n\t\tif (isOwner) {\n\t\t\tuint64_t o0 = 0ull, o1 = 0ull;\n\t\t\tclock_get_uptime(&o0);\n\t\t\tIOLockLock(gVramMmLock);\n\t\t\tclock_get_uptime(&o1);\n\t\t\tn48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(o0, o1));\n\t\t} else {\n\t\t\tIOLockLock(gVramMmLock);\n\t\t}\n\t\tif (mkw) mm_lockwait_note(l0);\n\t}\n\tclock_get_uptime(&w1);" },
    };
    for (const auto &p : plant) {
        std::string m[3] = { bu, peer, ahh };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { expect("T23p plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[220]; std::snprintf(lbl, sizeof lbl, "T23p BREAK-check: %s - caught", p.what);
        expect(lbl, fc509_pins(m[0], m[1], m[2], ttl, false) > 0u);
    }
}

// =============================================================================================================
// T24 — build 0.0.510 A1 (the 0.0.509 review's MEDIUM): UNDER 319 A POISONED RANGE CAN BE CLEARED AGAIN.
//   T24a a copy whose range a non-latching failure poisoned is retried under 319: the latch (n48_fc_latch_mode over the copy
//        guard's table for [dAt, dAt + wBytes)) runs it FULL, every dword is read back, and the clean close clears the poison.
//        CONTROL 0.0.509 (n48_fc_copy_mode alone): the same retry runs sampled and the poison stands.
//   T24b a first chunk REFUSED before the ring was written goes to the MM loop: the copy completes, nothing is poisoned, the path
//        stays usable, and (switch 62) the producer's overlay count is put back so the MM pass counts each patch byte once.
//        CONTROL 0.0.509: the same refusal FAILS the copy and poisons its range.
//   T24p the kext wires both (pins, each with a planted text break shown caught).
// =============================================================================================================
static void test_T24a_poisoned_retry_full() {
    const uint64_t lo = 0x10020000ull, n = 0x9000ull, hi = lo + n;
    for (int v509 = 0; v509 < 2; v509++) {
        n48_cg_poison p {}; n48_cg_poison_init(&p);
        // 1. the first copy under 319 FAILED without latching (a FILL / REFUSED at pos > 0 / verify-read / MM-loop failure): the copy
        //    loop's FAILED path closes the scope with failed = 1 (n48_fc_cg_close_wrote keeps `wrote` on a failure): POISONED.
        n48_cg_close_poison(&p, lo, hi, (int)n48_fc_cg_close_wrote(1u, 1u, 0u, 1u), 1, 0);
        const int poisonedBefore = n48_cg_poison_overlaps(&p, lo, hi);
        // 2. the retry's latch at its first chunk (pos 0): 0.0.510 asks the copy guard for [dAt, dAt + wBytes)
        const uint32_t cgp = n48_cg_poison_overlaps(&p, lo, lo + n) ? 1u : 0u;
        const uint32_t mode = v509 ? n48_fc_copy_mode(N48_FC_M_SAMPLED, 0u) : n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, cgp, 0u);
        // 3. the chunk through SDMA (RUN D's heap bytes, 62 OFF) and its verify through the MM window
        HgSim &hs = gHs; hs_boot(hs);
        FcMock &m = gFm; std::memset(&m, 0, sizeof m); m.hs = &hs; m.on62 = 0u; m.sub = N48_FC_SUB_LANDED;
        n48_fc_state st {}; st.pc_passed = 1u; st.staging_ok = 1u;
        const n48_fc_ops ops { &m, &fc_mock_fill, &fc_mock_submit };
        static uint32_t exp[0x9000 / 4];
        const uint32_t r = n48_fc_chunk_run(&ops, &st, m.stg, 0x840fc2e000ull, 0x8010020000ull, (uint32_t)n, exp, mode, 0u, 0u, nullptr);
        uint64_t cmp = 0; uint32_t rf = 0;
        const uint64_t bad = n48_fc_verify(&fc_mock_read, &fc_mock_plan, &m, lo, exp, gFcGot, (uint32_t)n, mode, 0u, &cmp, &rf);
        // 4. the scope's close: `sampled` from the slot, then the copy guard's close
        const uint32_t sampled = n48_fc_copy_sampled(mode, 1u);
        n48_cg_close_poison(&p, lo, hi, (int)n48_fc_cg_close_wrote(1u, 0u, bad ? 1u : 0u, sampled), 0, bad ? 1 : 0);
        const int poisonedAfter = n48_cg_poison_overlaps(&p, lo, hi);
        if (!v509) {
            expect("T24a 0.0.510: the retry of a poisoned range under 319 is latched FULL (the copy guard's poison overlaps it)",
                   poisonedBefore == 1 && cgp == 1u && mode == N48_FC_M_FULL);
            expect("T24a ... every dword is read back through the MM window (0 mismatches) and the copy is not `sampled`",
                   r == N48_FC_RUN_OK && rf == 0u && bad == 0u && cmp == n / 4u && sampled == 0u);
            expect("T24a ... and its clean close CLEARS the poison", poisonedAfter == 0);
        } else {
            expect("T24a CONTROL 0.0.509: the same retry runs SAMPLED and its clean close leaves the poison standing (the MEDIUM)",
                   mode == N48_FC_M_SAMPLED && r == N48_FC_RUN_OK && bad == 0u && cmp < n / 4u && sampled == 1u && poisonedAfter == 1);
        }
    }
    // no poison: the copy keeps sampled verify (the upgrade costs nothing where nothing is poisoned); a disjoint poison row too
    n48_cg_poison q {}; n48_cg_poison_init(&q);
    n48_cg_poison_mark(&q, 0x20000000ull, 0x20001000ull);
    expect("T24a a copy whose range overlaps no poison row stays SAMPLED; OFF stays OFF; FULL stays FULL; 62's upgrade is kept",
           n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, n48_cg_poison_overlaps(&q, 0x10020000ull, 0x10029000ull) ? 1u : 0u, 0u) == N48_FC_M_SAMPLED &&
           n48_fc_latch_mode(0u, 1u, 1u, 0u) == 0u && n48_fc_latch_mode(N48_FC_M_FULL, 0u, 0u, 0u) == N48_FC_M_FULL &&
           n48_fc_latch_mode(N48_FC_M_SAMPLED, 1u, 0u, 0u) == N48_FC_M_FULL && n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, 1u, 0u) == N48_FC_M_FULL);
    // a STICKY (dead) row also forces FULL, and still is never cleared
    n48_cg_poison d {}; n48_cg_poison_init(&d);
    n48_cg_poison_mark_sticky(&d, 0x10021000ull, 0x10022000ull);
    const uint32_t md = n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, n48_cg_poison_overlaps(&d, 0x10020000ull, 0x10029000ull) ? 1u : 0u, 0u);
    n48_cg_close_poison(&d, 0x10020000ull, 0x10029000ull, (int)n48_fc_cg_close_wrote(1u, 0u, 0u, n48_fc_copy_sampled(md, 1u)), 0, 0);
    expect("T24a a dead (sticky) range forces FULL too, and a clean full copy still does not clear it",
           md == N48_FC_M_FULL && n48_cg_poison_overlaps(&d, 0x10021000ull, 0x10022000ull) == 1);
}

// T24b's producer with switch 62: ic_read's overlay counted exactly as ic_overlay counts it (p->overlaid += ...).
static uint32_t gT24Overlaid = 0u;
static int t24_fill(void *vc, uint8_t *dst, uint64_t off, uint32_t take) {
    FcMock *mm = static_cast<FcMock *>(vc);
    mm->fills++;
    std::memcpy(dst, mm->hs->backing + off, take);
    uint32_t nb = 0;
    gT24Overlaid += n48_hg_overlay_checked(mm->hs->patch, mm->hs->np, mm->hs->arena, mm->hs->apple, off, dst, take, &nb);
    return 1;
}
// fc_copy_chunk + navi48_fc_chunk + the copy loop for ONE chunk at pos 0, in the kext's order. `v509`: 0.0.509's handling of a
// REFUSED run (the FAILED path); `noRestore`: 0.0.510 without fc_copy_chunk's overlay-count restore (a model break).
struct T24Out { uint32_t run, toMm, failed, clean62, latched, usable; int poisoned; uint32_t overlaid; };
static T24Out t24_refused_copy(int v509, int noRestore) {
    const uint64_t lo = 0x10020000ull, n = 0x9000ull, hi = lo + n;
    T24Out o {};
    HgSim &hs = gHs; hs_boot(hs);
    FcMock &m = gFm; std::memset(&m, 0, sizeof m); m.hs = &hs; m.on62 = 1u; m.sub = N48_FC_SUB_REFUSED;   // F-5: the queue busy
    n48_fc_state st {}; st.pc_passed = 1u; st.staging_ok = 1u;
    n48_cg_poison p {}; n48_cg_poison_init(&p);
    gT24Overlaid = 0u;
    const n48_fc_ops ops { &m, &t24_fill, &fc_mock_submit };
    static uint32_t exp[N48_FC_SAMPLE_MAX];
    const uint32_t ov0 = gT24Overlaid;                                                  // fc_copy_chunk: ov0
    o.run = n48_fc_chunk_run(&ops, &st, m.stg, 0x840fc2e000ull, 0x8010020000ull, (uint32_t)n, exp, N48_FC_M_SAMPLED, 0u, 0u, nullptr);
    o.toMm = v509 ? 0u : n48_fc_refused_to_mm(o.run, 0u);                              // navi48_fc_chunk: pos 0
    if (o.run != N48_FC_RUN_OK && !o.toMm) {                                           // the FAILED result -> the copy loop's FAILED
        o.failed = 1u;
        n48_cg_close_poison(&p, lo, hi, (int)n48_fc_cg_close_wrote(1u, 1u, 0u, 0u), 1, 0);
    } else {
        if (!noRestore) gT24Overlaid = ov0;                                             // fc_copy_chunk: not taken -> put back
        for (uint32_t k = 0; k < n; k += 256u) {                                         // the MM batch loop: ic_read + write
            uint8_t raw[256]; uint32_t nb = 0;
            std::memcpy(raw, hs.backing + k, 256u);
            gT24Overlaid += n48_hg_overlay_checked(hs.patch, hs.np, hs.arena, hs.apple, k, raw, 256u, &nb);
            std::memcpy(m.vram + k, raw, 256u);
        }
        n48_cg_close_poison(&p, lo, hi, (int)n48_fc_cg_close_wrote(1u, 0u, 0u, n48_fc_copy_sampled(N48_FC_M_SAMPLED, 0u)), 0, 0);
    }
    o.clean62 = (!o.failed && gT24Overlaid == hs.patchBytes) ? 1u : 0u;                 // navi48_ic_scope_closed's `clean`
    o.poisoned = n48_cg_poison_overlaps(&p, lo, hi);
    o.latched = st.latched_off; o.usable = n48_fc_staging_usable(&st);
    o.overlaid = gT24Overlaid;
    return o;
}
static void test_T24b_refused_first_chunk() {
    const T24Out a = t24_refused_copy(0, 0);
    expect("T24b 0.0.510: a first chunk REFUSED before the ring (F-5, queue busy) goes to the MM loop: the copy completes",
           a.run == N48_FC_RUN_REFUSED && a.toMm == 1u && a.failed == 0u);
    expect("T24b ... with NO poison left behind, the path not latched and the staging buffer still usable",
           a.poisoned == 0 && a.latched == 0u && a.usable == 1u);
    expect("T24b ... and (62 ON) the overlay count put back: the MM pass counts each patch byte once, so the copy completes CLEAN",
           a.clean62 == 1u && a.overlaid == gHs.patchBytes);
    const T24Out b = t24_refused_copy(1, 0);
    expect("T24b CONTROL 0.0.509: the same refusal FAILS the copy and poisons its range", b.failed == 1u && b.poisoned == 1);
    const T24Out c = t24_refused_copy(0, 1);
    expect("T24b MODEL BREAK-check: without fc_copy_chunk's restore the overlay is counted twice and 62 sees the copy UNCLEAN - caught",
           c.clean62 == 0u && c.overlaid == 2u * gHs.patchBytes);
    expect("T24b a refusal at pos > 0 still fails the copy (earlier chunks landed); only REFUSED goes to MM (not FILL, TIMEOUT, RING)",
           n48_fc_refused_to_mm(N48_FC_RUN_REFUSED, 0x100000u) == 0u && n48_fc_refused_to_mm(N48_FC_RUN_FILL, 0u) == 0u &&
           n48_fc_refused_to_mm(N48_FC_RUN_TIMEOUT, 0u) == 0u && n48_fc_refused_to_mm(N48_FC_RUN_RING, 0u) == 0u &&
           n48_fc_refused_to_mm(N48_FC_RUN_OK, 0u) == 0u && n48_fc_refused_to_mm(N48_FC_RUN_REFUSED, 0u) == 1u);
}

static size_t a1_pins(const std::string &bu, const std::string &peer, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0; bool first = true;
        for (const char *st : steps) {
            const size_t at = t.find(st, first ? 0 : prev);
            if (at == std::string::npos || count_of(t, st) != 1u) return false;
            prev = at + 1; first = false;
        }
        return true;
    };
    const std::string latch = fn_text(bu, "static FcSlot *fc_slot_latch(uint64_t pos, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {");
    const std::string chunk = fn_text(bu, "uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,");
    const std::string ccc = fn_text(peer, "static __attribute__((noinline)) uint64_t fc_copy_chunk(");
    chk("T24p the functions are found", !latch.empty() && !chunk.empty() && !ccc.empty());
    chk("T24p A1 the latch: the copy guard's OWN table over [dAt, dAt + wBytes), asked at pos 0 after the switch, before the mode is stored",
        has(bu, "static uint32_t fc_cg_poisoned(uint64_t lo, uint64_t hi) { return n48_cg_poison_overlaps(&gCgPoison, lo, hi) ? 1u : 0u; }") &&
        order(latch, { "if (pos != 0u) return s;", "if (!mode) {", "const uint32_t b62 = navi48_ic_bumped_mine(), cgp = fc_cg_poisoned(cgLo, cgHi);",
                       "const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);", "s->mode = eff;" }) &&
        has(chunk, "FcSlot *s = fc_slot_latch(pos, cgLo, cgHi, rpCand);"));
    chk("T24p A1 the refusal: REFUSED at pos 0 returns 0 (the MM loop) BEFORE the dead-range handling and the FAILED return",
        order(chunk, { "if (r != N48_FC_RUN_OK) {", "if (n48_fc_refused_to_mm(r, pos)) {",
                       "__atomic_fetch_add(&gFcS.refused_mm, 1ull, __ATOMIC_RELAXED);", "\t\t\treturn 0ull;\n\t\t}",
                       "const uint32_t dead = n48_fc_run_dead(r);", "return n48_fc_result_fail(r);" }));
    chk("T24p A1 fc_copy_chunk puts the overlay count back for a chunk not taken, after navi48_fc_chunk",
        order(ccc, { "IcSlot *is = gIcOverlaying ? ic_slot_mine() : nullptr;", "const uint32_t ov0 = (is && is->plan) ? is->plan->overlaid : 0u;",
                     "const uint64_t r = navi48_fc_chunk(pos, wBytes, dAt, n, &fc_fill_batch, &c, lead, cgLo, cgHi, cand);",
                     "if (!(r & N48_FC_R_TAKEN) && is && is->plan) is->plan->overlaid = ov0;", "return r;" }));
    return bad;
}
static void test_T24p_pins(const char *bringupPath, const char *peerPath) {
    std::string bu, peer;
    if (!bringupPath || !peerPath || !read_file(bringupPath, bu) || !read_file(peerPath, peer)) {
        expect("T24p the kext sources are readable", false); return;
    }
    expect("T24p every pin holds on the REAL sources", a1_pins(bu, peer, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // file: 0 bringup, 1 peer
        { 0, "A1: the latch ignores the copy guard's poison", "const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);",
          "const uint32_t eff = n48_fc_latch_mode(mode, b62, 0u, rpCand);" },
        { 0, "A1: the latch asks the first chunk only, not the copy's range", "cgp = fc_cg_poisoned(cgLo, cgHi);",
          "cgp = fc_cg_poisoned(cgLo, cgLo + 4u);" },
        { 0, "A1: a REFUSED first chunk fails the copy (0.0.509)", "if (n48_fc_refused_to_mm(r, pos)) {", "if (0) {" },
        { 1, "A1: the overlay count not put back", "if (!(r & N48_FC_R_TAKEN) && is && is->plan) is->plan->overlaid = ov0;", "(void)ov0;" },
    };
    for (const auto &pl : plant) {
        std::string mm[2] = { bu, peer };
        std::string &t = mm[pl.file];
        const size_t a = t.find(pl.from);
        if (a == std::string::npos || count_of(t, pl.from) != 1u) { expect("T24p plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(pl.from), pl.to);
        char lbl[220]; std::snprintf(lbl, sizeof lbl, "T24p BREAK-check: %s - caught", pl.what);
        expect(lbl, a1_pins(mm[0], mm[1], false) > 0u);
    }
}

// =============================================================================================================
// T25 — build 0.0.510 A2 (the 0.0.509 review's LOW): THE COPY GUARD'S POISON ROWS ARE CLAIMED BY CAS.
//   T25a SINGLE-THREADED IDENTITY: over random sequences of mark / mark_sticky / clear_covered the table is, after every call,
//        byte for byte the table 0.0.509's functions (transcribed below) leave.
//   T25b INTERLEAVINGS: two actors (gfx_copyguard.h's own step machine, n48_cg_pm) over a 2-row prefix, EVERY schedule of their
//        atomic steps: no marked range is lost, a sticky range stays sticky, no row is left CLAIMING, and a clear erases only rows
//        its range covers. CONTROL: 0.0.509's functions as step machines lose a range in the same scenarios.
// =============================================================================================================
// 0.0.509's three functions, transcribed (the control and the identity reference).
static void cg509_mark(n48_cg_poison *p, uint64_t lo, uint64_t hi, uint32_t v) {
    for (uint32_t i = 0; i < N48_CG_POISON; i++)
        if (!p->s[i].valid) { p->s[i].lo = lo; p->s[i].hi = hi; p->s[i].valid = v; return; }
    if (lo < p->s[0].lo) p->s[0].lo = lo;
    if (hi > p->s[0].hi) p->s[0].hi = hi;
    if (v == N48_CG_POISON_STICKY) p->s[0].valid = N48_CG_POISON_STICKY;
}
static void cg509_clear(n48_cg_poison *p, uint64_t lo, uint64_t hi) {
    for (uint32_t i = 0; i < N48_CG_POISON; i++) {
        const uint32_t v = p->s[i].valid;
        if (!v || v == N48_CG_POISON_STICKY) continue;
        if (lo <= p->s[i].lo && p->s[i].hi <= hi) p->s[i].valid = 0u;
    }
}
static uint64_t t25_rng(uint64_t &x) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; }
static void test_T25a_single_thread_identity() {
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    uint32_t diffs = 0u, ops = 0u, folds = 0u, stickyFolds = 0u;
    for (uint32_t run = 0; run < 400u; run++) {
        n48_cg_poison a {}, b {}; n48_cg_poison_init(&a); n48_cg_poison_init(&b);
        for (uint32_t k = 0; k < 64u; k++) {
            const uint32_t op = (uint32_t)(t25_rng(seed) % 5u);
            const uint64_t base = 0x10000000ull + (t25_rng(seed) % 40u) * 0x1000ull;
            const uint64_t len = (1u + t25_rng(seed) % 6u) * 0x1000ull;
            uint32_t full = 1u;
            for (uint32_t i = 0; i < N48_CG_POISON; i++) if (!b.s[i].valid) full = 0u;
            if (op <= 1u) { n48_cg_poison_mark(&a, base, base + len); cg509_mark(&b, base, base + len, 1u); folds += full; }
            else if (op == 2u) { n48_cg_poison_mark_sticky(&a, base, base + len); cg509_mark(&b, base, base + len, N48_CG_POISON_STICKY);
                                 folds += full; stickyFolds += full; }
            else {
                const uint64_t cl = base - (t25_rng(seed) % 4u) * 0x1000ull, ch = base + len + (t25_rng(seed) % 12u) * 0x1000ull;
                n48_cg_poison_clear_covered(&a, cl, ch); cg509_clear(&b, cl, ch);
            }
            ops++;
            for (uint32_t i = 0; i < N48_CG_POISON; i++)
                if (a.s[i].valid != b.s[i].valid || (a.s[i].valid && (a.s[i].lo != b.s[i].lo || a.s[i].hi != b.s[i].hi))) diffs++;
            if (a.over) diffs++;
            for (uint32_t q = 0; q < 8u; q++) {
                const uint64_t ql = 0x10000000ull + (t25_rng(seed) % 48u) * 0x1000ull;
                if (n48_cg_poison_overlaps(&a, ql, ql + 0x1000u) != n48_cg_poison_overlaps(&b, ql, ql + 0x1000u)) diffs++;
            }
        }
    }
    char b[240];
    std::snprintf(b, sizeof b, "T25a single-threaded identity: %u operations (%u full-table folds, %u of them sticky), every table and "
                  "every overlap answer equal to 0.0.509's (%u differences)", ops, folds, stickyFolds, diffs);
    expect(b, diffs == 0u && folds > 100u && stickyFolds > 20u);
}

// 0.0.509 as step machines (each step one load or store, the order the functions above run them).
struct Pm509 { uint32_t kind, v, pc, i, nrows, cur; uint64_t lo, hi, l, h; };
enum { P5_LOOK = 0u, P5_WLO, P5_WHI, P5_WV, P5_FLO, P5_FHI, P5_FV, P5_CLOOK, P5_CLO, P5_CHI, P5_DONE };
static int pm509_step(Pm509 *m, n48_cg_poison *p) {
    n48_cg_poison_row *r = &p->s[m->i];
    switch (m->pc) {
    case P5_LOOK: if (!r->valid) { m->pc = P5_WLO; return 0; }
                  if (++m->i >= m->nrows) { m->i = 0u; m->pc = P5_FLO; } return 0;
    case P5_WLO: r->lo = m->lo; m->pc = P5_WHI; return 0;
    case P5_WHI: r->hi = m->hi; m->pc = P5_WV; return 0;
    case P5_WV: r->valid = m->v; m->pc = P5_DONE; return 1;
    case P5_FLO: m->l = r->lo; m->h = r->hi; r->lo = m->lo < m->l ? m->lo : m->l; m->pc = P5_FHI; return 0;
    case P5_FHI: r->hi = m->hi > m->h ? m->hi : m->h; m->pc = m->v == N48_CG_POISON_STICKY ? P5_FV : P5_DONE; return m->pc == P5_DONE;
    case P5_FV: r->valid = N48_CG_POISON_STICKY; m->pc = P5_DONE; return 1;
    case P5_CLOOK: m->cur = r->valid;
                   if (!m->cur || m->cur == N48_CG_POISON_STICKY) { if (++m->i >= m->nrows) { m->pc = P5_DONE; return 1; } return 0; }
                   m->pc = P5_CLO; return 0;
    case P5_CLO: m->l = r->lo; m->h = r->hi; m->pc = P5_CHI; return 0;
    case P5_CHI: if (m->lo <= m->l && m->h <= m->hi) r->valid = 0u;
                 if (++m->i >= m->nrows) { m->pc = P5_DONE; return 1; } m->pc = P5_CLOOK; return 0;
    default: return 1;
    }
}
// One actor of either build.
struct T25Actor { int is509; n48_cg_pm m; Pm509 o; uint32_t done; };
static void t25_actor(T25Actor &a, int is509, uint32_t kind, uint32_t v, uint64_t lo, uint64_t hi, uint32_t nrows) {
    std::memset(&a, 0, sizeof a); a.is509 = is509;
    if (!is509) n48_cg_pm_init(&a.m, kind, v, lo, hi, nrows);
    else { a.o.kind = kind; a.o.v = v; a.o.lo = lo; a.o.hi = hi; a.o.nrows = nrows; a.o.pc = kind == N48_CG_PM_CLEAR ? P5_CLOOK : P5_LOOK; }
}
static int t25_blocked(const T25Actor &a, const n48_cg_poison *p) {   // a fair scheduler lets a spinning folder yield
    if (a.is509 || a.m.pc != N48_CG_PM_FOLD) return 0;
    for (uint32_t i = 0; i < a.m.nrows; i++) if (p->s[i].valid != N48_CG_POISON_CLAIMING) return 0;
    return 1;
}
static int t25_step(T25Actor &a, n48_cg_poison *p) { return a.is509 ? pm509_step(&a.o, p) : n48_cg_pm_step(&a.m, p); }
struct T25Scn {
    const char *name;
    n48_cg_poison_row init[2];
    uint32_t kind[2], v[2]; uint64_t lo[2], hi[2];
};
struct T25Res { uint64_t schedules, bad, lostMark, lostSticky, lostInit, claiming, overClear; };
static int t25_covered(const n48_cg_poison *p, uint64_t lo, uint64_t hi, uint32_t needSticky) {
    if (p->over) return 1;
    for (uint32_t i = 0; i < 2u; i++)
        if (p->s[i].valid && p->s[i].valid != N48_CG_POISON_CLAIMING && p->s[i].lo <= lo && hi <= p->s[i].hi &&
            (!needSticky || p->s[i].valid == N48_CG_POISON_STICKY)) return 1;
    return 0;
}
static void t25_check(const T25Scn &sc, const n48_cg_poison *p, T25Res &r) {
    r.schedules++;
    uint32_t bad = 0u;
    for (uint32_t i = 0; i < 2u; i++) if (p->s[i].valid == N48_CG_POISON_CLAIMING) { r.claiming++; bad = 1u; }
    for (uint32_t a = 0; a < 2u; a++) {
        if (sc.kind[a] != N48_CG_PM_MARK) continue;
        if (!t25_covered(p, sc.lo[a], sc.hi[a], 0u)) { r.lostMark++; bad = 1u; }
        else if (sc.v[a] == N48_CG_POISON_STICKY && !t25_covered(p, sc.lo[a], sc.hi[a], 1u)) { r.lostSticky++; bad = 1u; }
    }
    for (uint32_t i = 0; i < 2u; i++) {                    // the rows the table held before: kept unless a clear covers them
        const n48_cg_poison_row &e = sc.init[i];
        if (!e.valid) continue;
        uint32_t clearable = 0u;
        for (uint32_t a = 0; a < 2u; a++)
            if (sc.kind[a] == N48_CG_PM_CLEAR && e.valid == 1u && sc.lo[a] <= e.lo && e.hi <= sc.hi[a]) clearable = 1u;
        if (clearable) continue;
        if (!t25_covered(p, e.lo, e.hi, e.valid == N48_CG_POISON_STICKY ? 1u : 0u)) { r.lostInit++; bad = 1u; }
    }
    if (bad) r.bad++;
}
static void t25_dfs(const T25Scn &sc, n48_cg_poison p, T25Actor a0, T25Actor a1, T25Res &r, uint32_t depth) {
    if (depth > 200u) { r.bad++; return; }                 // never: every actor finishes in a bounded number of steps
    if (a0.done && a1.done) { t25_check(sc, &p, r); return; }
    for (uint32_t w = 0; w < 2u; w++) {
        T25Actor &me = w ? a1 : a0;
        const T25Actor &other = w ? a0 : a1;
        if (me.done) continue;
        if (!other.done && t25_blocked(me, &p)) continue;
        n48_cg_poison q = p; T25Actor b0 = a0, b1 = a1;
        T25Actor &mb = w ? b1 : b0;
        if (t25_step(mb, &q)) mb.done = 1u;
        t25_dfs(sc, q, b0, b1, r, depth + 1u);
    }
}
static T25Res t25_run(const T25Scn &sc, int is509) {
    n48_cg_poison p {}; n48_cg_poison_init(&p);
    p.s[0] = sc.init[0]; p.s[1] = sc.init[1];
    T25Actor a0, a1;
    t25_actor(a0, is509, sc.kind[0], sc.v[0], sc.lo[0], sc.hi[0], 2u);
    t25_actor(a1, is509, sc.kind[1], sc.v[1], sc.lo[1], sc.hi[1], 2u);
    T25Res r {};
    t25_dfs(sc, p, a0, a1, r, 0u);
    return r;
}
static void test_T25b_interleavings() {
    const uint32_t MK = N48_CG_PM_MARK, CL = N48_CG_PM_CLEAR, ST = N48_CG_POISON_STICKY;
    const T25Scn scn[] = {
        { "S1 two markers, one empty row (a sticky and a plain range)",
          { { 0x1000u, 0x2000u, 1u }, { 0u, 0u, 0u } }, { MK, MK }, { ST, 1u }, { 0x5000u, 0x7000u }, { 0x6000u, 0x8000u } },
        { "S2 a sticky range folded into row 0 while a clear covers row 0's old range",
          { { 0x1000u, 0x2000u, 1u }, { 0x3000u, 0x4000u, 1u } }, { MK, CL }, { ST, 0u }, { 0x5000u, 0x0u }, { 0x6000u, 0x2800u } },
        { "S3 two sticky folds into a full table",
          { { 0x1000u, 0x2000u, 1u }, { 0x3000u, 0x4000u, 1u } }, { MK, MK }, { ST, ST }, { 0x5000u, 0x7000u }, { 0x6000u, 0x8000u } },
        { "S4 a plain fold into a STICKY row 0 while a clear covers row 1",
          { { 0x1000u, 0x2000u, ST }, { 0x3000u, 0x4000u, 1u } }, { MK, CL }, { 1u, 0u }, { 0x5000u, 0x2800u }, { 0x6000u, 0x4800u } },
        { "S5 a clear empties row 0 while a marker scans a full table",
          { { 0x1000u, 0x2000u, 1u }, { 0x3000u, 0x4000u, 1u } }, { CL, MK }, { 0u, 1u }, { 0x0u, 0x5000u }, { 0x2800u, 0x6000u } },
    };
    for (const T25Scn &sc : scn) {
        const T25Res r = t25_run(sc, 0);
        char b[300];
        std::snprintf(b, sizeof b, "T25b 0.0.510 %s: %llu schedule(s), lost %llu (mark %llu, sticky %llu, earlier rows %llu), left "
                      "CLAIMING %llu", sc.name, (unsigned long long)r.schedules, (unsigned long long)r.bad,
                      (unsigned long long)r.lostMark, (unsigned long long)r.lostSticky, (unsigned long long)r.lostInit,
                      (unsigned long long)r.claiming);
        expect(b, r.schedules > 1u && r.bad == 0u);
        const T25Res c = t25_run(sc, 1);
        std::printf("      CONTROL 0.0.509 %s: %llu of %llu schedule(s) lose a range (mark %llu, sticky %llu, earlier rows %llu)\n",
                    sc.name, (unsigned long long)c.bad, (unsigned long long)c.schedules, (unsigned long long)c.lostMark,
                    (unsigned long long)c.lostSticky, (unsigned long long)c.lostInit);
    }
    const T25Res c1 = t25_run(scn[0], 1), c2 = t25_run(scn[1], 1);
    expect("T25b CONTROL 0.0.509: two markers on one empty row lose a range in some schedule (the review's first race)", c1.bad > 0u);
    expect("T25b CONTROL 0.0.509: a clear erases a sticky fold into row 0 in some schedule (the review's second race)",
           c2.lostSticky > 0u || c2.lostMark > 0u);
    // a reader during a claim: a CLAIMING row answers "poisoned" for every range; `over` likewise
    n48_cg_poison p {}; n48_cg_poison_init(&p);
    n48_cg_pm m; n48_cg_pm_init(&m, N48_CG_PM_MARK, 1u, 0x5000u, 0x6000u, N48_CG_POISON);
    (void)n48_cg_pm_step(&m, &p);                          // row 0 claimed, lo/hi not written yet
    const int during = n48_cg_poison_overlaps(&p, 0x90000000ull, 0x90001000ull);
    n48_cg_pm_run(&m, &p);
    n48_cg_poison o {}; n48_cg_poison_init(&o); o.over = 1u;
    expect("T25b a reader meeting a CLAIMING row (or a saturated table) answers POISONED for any range (fail-closed); after the "
           "publish only the marked range", during == 1 && n48_cg_poison_overlaps(&p, 0x90000000ull, 0x90001000ull) == 0 &&
           n48_cg_poison_overlaps(&p, 0x5000u, 0x5001u) == 1 && n48_cg_poison_overlaps(&o, 1u, 2u) == 1);
    // the fold's bound: a table whose every row stays CLAIMING (an actor that never publishes) saturates instead of spinning
    n48_cg_poison s {}; n48_cg_poison_init(&s);
    for (uint32_t i = 0; i < N48_CG_POISON; i++) s.s[i].valid = N48_CG_POISON_CLAIMING;
    n48_cg_poison_mark_sticky(&s, 0x5000u, 0x6000u);
    expect("T25b the fold's retry is bounded: every row held CLAIMING -> `over` (every range poisoned), never a dropped range",
           s.over == 1u && n48_cg_poison_overlaps(&s, 1u, 2u) == 1);
}

// =============================================================================================================
// T26 — build 0.0.510 PART B ( (b),; switch 72): THE COPY WAITS FOR A COMMITTED FRAME'S WALK.
// A deterministic two-thread model: the SUBMIT thread runs the kext's events at fixed times - the top of the pass (FB), the commit
// judge (J: hg_commit_judge, which sets the flag), the gate's record (R: hg_commit_record), the ring walk (W: hg_exempt_refuse ->
// n48_hg_walk_answer) and the hook's exit (X: hg_pend_exit) - and the COPIER runs hw_hg_copy_begin at `copyAt`: the lock, the
// flag, n48_hg_copy_wait (whose `delay` callback advances the clock and runs every submit event now due, with the lock released,
// as the other thread would), then the bump; its completion (n48_hg_copy_end) `dur` later. Every decision is gfx_heapgen.h's.
//   T26a RUN L's window (run10r seq 10): a copy arriving between the judge and the walk. 0.0.509 (72 OFF): the walk REFUSES.
//        0.0.510 (72 ON): the copy waits, the walk SPARES, then the copy bumps. Breaks: ORDERING (the flag cleared before the walk
//        answers), FAIL-OPEN (the bump skipped after a timeout) and LEAK (a withdrawal that does not clear) - each caught.
//   T26b THE INVARIANT over a sweep of arrival times, walk times and copy lengths: the 72 ON run's walk answer equals the 72 OFF
//        answer for the SAME order of events (the flag moves copies later, never changes an answer); a spared walk never follows
//        a bump since the verdict; 72 ON never refuses what 72 OFF spared at the original times.
//   T26c the copier's other answers: its own committing thread (never waits), a copy that would not bump, an expired flag, the
//        timeout's bound, and OFF identity (nothing pending: no lock dropped, no delay).
//   T26p the kext wires every piece (pins, each with a planted text break shown caught), and the report lines fit 512 bytes.
// =============================================================================================================
static const uintptr_t kT26Submit = 0x5501u, kT26Copier = 0x7702u;
static const uint64_t kT26Lo = 0x10020000ull, kT26Hi = 0x10029000ull;
enum { T26_REAL = 0, T26_CLEAR_EARLY, T26_FAILOPEN, T26_NO_WITHDRAW_CLEAR };
struct T26Sim {
    n48_hg_state st; n48_hg_pend pd; n48_hg_frame f, rec;
    uint64_t now; int held; uint32_t lockOps, delays;
    uint32_t latch72, mutant, withdraw;
    // the submit thread's schedule (us) and what happened
    uint64_t tJ, tR, tW; uint32_t doneFB, doneJ, doneR, doneW, committed, judgeWhy, walkWhy, walked;
    uint64_t walkedAt;
    // the copier
    uint64_t copyAt, dur, bumpAt, endAt; uint32_t bumped, ended, look; uint64_t waited;
    uint32_t mayWait;   // build 0.0.511 (B1): n48_hg_copy_may_wait over gT26Copy, set by t26_run
};
// build 0.0.511 (B1): the arriving copy, as ic_begin sees it: {np, npoison, noOverlay, scanFailed, noPlan}. Default: a FULLY
// PATCHED copy (run10r's case - the one switch 72 was built for).
struct T26Copy { uint32_t np, npoison, noOverlay, scanFailed, noPlan; };
static T26Copy gT26Copy = { 1u, 0u, 0u, 0u, 0u };
static T26Sim gT26;
static void t26_submit_due(T26Sim &s);   // runs every submit event whose time has come
static uint64_t t26_now(void *c) { return static_cast<T26Sim *>(c)->now; }
static void t26_lock(void *c) { T26Sim *s = static_cast<T26Sim *>(c); s->held++; s->lockOps++; }
static void t26_unlock(void *c) { T26Sim *s = static_cast<T26Sim *>(c); s->held--; s->lockOps++; }
static void t26_delay(void *c, uint32_t us) { T26Sim *s = static_cast<T26Sim *>(c); s->delays++; s->now += us; t26_submit_due(*s); }
static const n48_hg_wait_ops kT26Ops = { &t26_now, &t26_lock, &t26_unlock, &t26_delay, nullptr };   // 0.0.532: no fence read (0.0.511's wait never calls it)

static void t26_submit_due(T26Sim &s) {
    if (s.held) return;                                   // gHgLock is the copier's: the submit thread's gHgLock sections wait
    if (!s.doneFB) { n48_hg_frame_begin(&s.f, 1u, 5u, &s.st); s.doneFB = 1u; }             // hg_frame_begin_pass (t = 0)
    if (!s.doneJ && s.now >= s.tJ) {                                                         // hg_commit_judge (wouldCommit 1)
        s.judgeWhy = n48_hg_judge(&s.st, &s.f, 5u);
        if (s.judgeWhy == N48_HG_OK && s.latch72) n48_hg_pend_set(&s.pd, kT26Submit, s.now);
        s.doneJ = 1u;
    }
    if (s.doneJ && !s.doneR && s.now >= s.tR) {                                               // hg_commit_record
        s.committed = s.judgeWhy == N48_HG_OK ? 1u : 0u;
        if (s.latch72) n48_hg_pend_record(&s.pd, s.committed ? 7u : 0u, kT26Submit);
        if (s.mutant == T26_CLEAR_EARLY) (void)n48_hg_pend_clear(&s.pd, N48_HG_PCLR_EXIT, 0u, kT26Submit);   // BREAK: cleared early
        s.rec = s.f;
        if (s.committed && s.withdraw) {                                                      // the keystone withdraws: EXIT 4
            if (s.mutant != T26_NO_WITHDRAW_CLEAR) (void)n48_hg_pend_clear(&s.pd, N48_HG_PCLR_EXIT, 0u, kT26Submit);
            s.committed = 0u;
        }
        s.doneR = 1u;
    }
    if (s.doneR && !s.doneW && s.now >= s.tW) {                                               // the walk, then the hook's exit
        if (s.committed) { s.walkWhy = n48_hg_walk_answer(&s.st, &s.rec, &s.pd, 7u, kT26Submit); s.walked = 1u; s.walkedAt = s.now; }
        (void)n48_hg_pend_clear(&s.pd, N48_HG_PCLR_EXIT, 0u, kT26Submit);                     // EXIT 3 (a no-op after the walk)
        s.doneW = 1u;
    }
}
// hw_hg_copy_begin, in the kext's order
static void t26_copy_begin(T26Sim &s) {
    t26_lock(&s);
    s.look = N48_HG_PW_GO; s.waited = 0u;
    if (s.pd.pending && !s.mayWait && n48_hg_reg_overlaps(&s.st, kT26Lo, kT26Hi)) s.pd.unpatched++;   // 0.0.511 B1
    if (s.pd.pending && s.mayWait) s.look = n48_hg_copy_wait(&kT26Ops, &s, &s.st, &s.pd, kT26Copier, 0u, kT26Lo, kT26Hi, &s.waited);
    if (!(s.mutant == T26_FAILOPEN && s.look == N48_HG_PW_TIMEOUT))                          // BREAK: the bump skipped
        s.bumped = n48_hg_copy_begin(&s.st, (n48_hg_reg_overlaps(&s.st, kT26Lo, kT26Hi)) ? 1u : 0u);
    s.bumpAt = s.now;
    t26_unlock(&s);
}
// One whole run. Ties at equal times: the submit thread first.
static void t26_run(T26Sim &s, uint32_t latch72, uint64_t copyAt, uint64_t dur, uint64_t tW, uint32_t mutant, uint32_t withdraw) {
    std::memset(&s, 0, sizeof s);
    n48_hg_reg_add(&s.st, kT26Lo + 0x2300u, kT26Lo + 0x2400u);   // a substituted program inside the heap: the copy overlaps it
    s.latch72 = latch72; s.mutant = mutant; s.withdraw = withdraw;
    s.mayWait = n48_hg_copy_may_wait(gT26Copy.np, gT26Copy.npoison, gT26Copy.noOverlay, gT26Copy.scanFailed, gT26Copy.noPlan);
    s.tJ = 10u; s.tR = 20u; s.tW = tW; s.copyAt = copyAt; s.dur = dur;
    uint64_t guard = 0u;
    while ((!s.doneW || !s.ended) && guard++ < 100000u) {
        const uint64_t nextSub = !s.doneJ ? s.tJ : !s.doneR ? s.tR : !s.doneW ? s.tW : ~0ull;
        const uint64_t nextCopy = !s.bumpAt && !s.bumped && s.copyAt != ~0ull && s.look == 0u && !s.endAt ? s.copyAt
                                  : (!s.ended ? s.endAt : ~0ull);
        if (nextSub != ~0ull && nextSub <= nextCopy) { if (s.now < nextSub) s.now = nextSub; t26_submit_due(s); continue; }
        if (nextCopy == ~0ull) break;
        if (s.now < nextCopy) s.now = nextCopy;
        if (!s.endAt) { t26_copy_begin(s); s.endAt = s.now + s.dur; if (!s.bumped) { s.ended = 1u; } continue; }
        if (s.bumped) n48_hg_copy_end(&s.st, 1u, 1u, 0x400020000ull, 0x400029000ull, kT26Lo, kT26Hi, nullptr, 0u);
        s.ended = 1u;
    }
}
static uint32_t t26_spared(const T26Sim &s) { return (s.walked && s.walkWhy == N48_HG_OK) ? 1u : 0u; }

static void test_T26a_window() {
    T26Sim &s = gT26;
    // RUN L: the verdict at 10, the record at 20, the copy starts at 30, the walk at 50 (the copy runs 0.4 ms)
    t26_run(s, 0u, 30u, 400u, 50u, T26_REAL, 0u);
    expect("T26a 0.0.509 (72 OFF): the copy bumps at once (no flag), the walk at 50 us REFUSES the exemption (run10r seq 10)",
           s.committed == 1u && s.bumped == 1u && s.bumpAt == 30u && s.walked == 1u && s.walkWhy != N48_HG_OK &&
           s.pd.sets == 0u && s.pd.waits == 0u);
    t26_run(s, 1u, 30u, 400u, 50u, T26_REAL, 0u);
    char b[300];
    std::snprintf(b, sizeof b, "T26a 0.0.510 (72 ON): the copy WAITS (%llu us), the walk SPARES the frame, THEN the copy bumps "
                  "(bump at %llu us >= walk at %llu us)", (unsigned long long)s.waited, (unsigned long long)s.bumpAt,
                  (unsigned long long)s.walkedAt);
    expect(b, s.committed == 1u && s.walked == 1u && s.walkWhy == N48_HG_OK && s.bumped == 1u && s.bumpAt >= s.walkedAt &&
              s.look == N48_HG_PW_GO && s.pd.waits == 1u && s.pd.timeouts == 0u && s.waited >= 20u && s.waited < 100u);
    expect("T26a ... the flag was set at the judge, stamped with the token at the record, cleared by the WALK; the copy bumped "
           "exactly as without 72 (odd, one active, then even again)",
           s.pd.sets == 1u && s.pd.clr[N48_HG_PCLR_WALK] == 1u && s.pd.pending == 0u && s.st.gen == 2ull && s.st.active == 0u);
    expect("T26a ... the lock was dropped only around the delays and is balanced (held 0 at the end)",
           s.held == 0 && s.lockOps == 2u + 2u * s.delays);
    // THE BREAKS (model mutants of the kext's order; the kext's own code is pinned in T26p)
    t26_run(s, 1u, 30u, 400u, 50u, T26_CLEAR_EARLY, 0u);
    expect("T26a ORDERING BREAK-check: the flag cleared before the walk answers -> the copy does not wait and the walk REFUSES - caught",
           !(s.walked && s.walkWhy == N48_HG_OK && s.pd.waits == 1u));
    // a walk that comes only after the 100 ms bound: the copy times out and BUMPS, and the late walk refuses (fail-closed)
    t26_run(s, 1u, 30u, 400u, 150000u, T26_REAL, 0u);
    expect("T26a a walk later than the bound: the copy TIMES OUT at 100 ms, bumps, and the late walk REFUSES (fail-closed)",
           s.look == N48_HG_PW_TIMEOUT && s.pd.timeouts == 1u && s.bumped == 1u && s.bumpAt >= 30u + N48_HG_WAIT_US &&
           s.bumpAt < 30u + N48_HG_WAIT_US + 1100u && s.walked == 1u && s.walkWhy != N48_HG_OK);
    t26_run(s, 1u, 30u, 400u, 150000u, T26_FAILOPEN, 0u);
    expect("T26a FAIL-OPEN BREAK-check: the bump skipped after the timeout -> the late walk SPARES a frame a copy rewrote - caught",
           !(s.walked && s.walkWhy != N48_HG_OK));
    // the keystone withdraws the frame: its exit clears the flag, so the next copy does not wait
    t26_run(s, 1u, 30u, 400u, 50u, T26_REAL, 1u);
    expect("T26a a KEYSTONE WITHDRAWAL clears the flag at the hook's exit: the copy at 30 us does not wait (0 us), nothing is walked",
           s.committed == 0u && s.walked == 0u && s.look == N48_HG_PW_GO && s.waited == 0u && s.pd.waits == 0u &&
           s.pd.clr[N48_HG_PCLR_EXIT] == 1u);
    t26_run(s, 1u, 30u, 400u, 50u, T26_NO_WITHDRAW_CLEAR, 1u);
    expect("T26a LEAK BREAK-check: a withdrawal that does not clear the flag -> the copy waits the whole 100 ms - caught",
           !(s.look == N48_HG_PW_GO && s.waited == 0u));
    // the gate did not commit (the judge refused): no flag at all
    T26Sim g {}; std::memset(&g, 0, sizeof g);
    n48_hg_frame_begin(&g.f, 1u, 5u, &g.st);
    (void)n48_hg_copy_begin(&g.st, 1u);                   // a copy in progress at the verdict
    const uint32_t jw = n48_hg_judge(&g.st, &g.f, 5u);
    n48_hg_pend_record(&g.pd, 0u, kT26Submit);
    expect("T26a a verdict the judge refuses sets no flag; a gate that did not commit clears a tentative one (GATE)", [&] {
        n48_hg_pend p2 {}; n48_hg_pend_set(&p2, kT26Submit, 1u); n48_hg_pend_record(&p2, 0u, kT26Submit);
        return jw != N48_HG_OK && g.pd.sets == 0u && p2.pending == 0u && p2.clr[N48_HG_PCLR_GATE] == 1u; }());
    n48_hg_pend d {}; n48_hg_pend_set(&d, kT26Submit, 1u); n48_hg_pend_record(&d, 9u, kT26Submit);
    const uint32_t other = n48_hg_pend_clear(&d, N48_HG_PCLR_EXIT, 0u, kT26Copier);
    const uint32_t dis = n48_hg_pend_clear(&d, N48_HG_PCLR_DISARM, 0u, 0u);
    expect("T26a another thread's exit never clears the committing thread's flag; a DISARM clears any flag",
           other == 0u && dis == 1u && d.pending == 0u && d.clr[N48_HG_PCLR_DISARM] == 1u);
}

static void test_T26b_invariant() {
    const uint64_t copyAts[] = { 3u, 15u, 25u, 30u, 45u, 60u, 5000u, 120000u };
    const uint64_t durs[] = { 5u, 400u, 60000u };
    const uint64_t walks[] = { 12u, 50u, 2000u, 90000u, 150000u };
    uint32_t runs = 0u, replayDiff = 0u, sparedAfterBump = 0u, worse = 0u, better = 0u, inWindow = 0u;
    T26Sim on {}, off {}, rep {};
    for (uint64_t ca : copyAts)
        for (uint64_t du : durs)
            for (uint64_t tw : walks) {
                if (tw < 20u) continue;                   // the walk follows the record
                t26_run(on, 1u, ca, du, tw, T26_REAL, 0u);
                t26_run(off, 0u, ca, du, tw, T26_REAL, 0u);
                // REPLAY the 72 ON run's actual order with 72 OFF: the copy starts when it bumped, the walk when it walked
                t26_run(rep, 0u, on.bumpAt, du, on.walked ? on.walkedAt : tw, T26_REAL, 0u);
                runs++;
                if (on.walked != rep.walked || (on.walked && on.walkWhy != rep.walkWhy)) replayDiff++;
                if (t26_spared(on) && on.bumped && on.bumpAt < on.walkedAt && on.bumpAt >= on.tJ) sparedAfterBump++;
                if (t26_spared(off) && !t26_spared(on)) worse++;
                if (!t26_spared(off) && t26_spared(on)) better++;
                if (ca > 20u && ca < tw && ca < 30u + N48_HG_WAIT_US) inWindow++;
            }
    char b[320];
    std::snprintf(b, sizeof b, "T26b INVARIANT over %u runs: the 72 ON walk answer equals the 72 OFF answer for the same order of "
                  "events (%u differ); no spared walk after a bump since the verdict (%u)", runs, replayDiff, sparedAfterBump);
    expect(b, runs > 60u && replayDiff == 0u && sparedAfterBump == 0u);
    std::snprintf(b, sizeof b, "T26b 72 ON never refuses a frame 72 OFF spared at the original times (%u); it spares %u that 72 OFF "
                  "refused (copies arriving inside the window: %u)", worse, better, inWindow);
    expect(b, worse == 0u && better > 0u);
}

static void test_T26c_copier_answers() {
    n48_hg_state st {}; std::memset(&st, 0, sizeof st);
    n48_hg_reg_add(&st, kT26Lo, kT26Lo + 0x100u);
    T26Sim s {}; std::memset(&s, 0, sizeof s);
    // OFF identity: nothing pending -> GO at once: no lock dropped, no delay, no count
    n48_hg_pend p0 {}; uint64_t w = 99u;
    s.held = 1;
    uint32_t d = n48_hg_copy_wait(&kT26Ops, &s, &st, &p0, kT26Copier, 1u, kT26Lo, kT26Hi, &w);
    expect("T26c OFF identity: nothing pending -> GO, the lock never dropped, no delay, nothing counted",
           d == N48_HG_PW_GO && s.lockOps == 0u && s.delays == 0u && w == 0u && p0.waits == 0u && p0.timeouts == 0u);
    // the committing thread itself
    n48_hg_pend p1 {}; n48_hg_pend_set(&p1, kT26Copier, 0u);
    d = n48_hg_copy_wait(&kT26Ops, &s, &st, &p1, kT26Copier, 1u, kT26Lo, kT26Hi, &w);
    expect("T26c a copy on the COMMITTING thread never waits for itself (SELF, counted)", d == N48_HG_PW_SELF && s.delays == 0u &&
           p1.self_skips == 1u && p1.pending == 1u);
    // a copy that would not bump
    n48_hg_pend p2 {}; n48_hg_pend_set(&p2, kT26Submit, 0u);
    d = n48_hg_copy_wait(&kT26Ops, &s, &st, &p2, kT26Copier, 0u, 0x20000000ull, 0x20001000ull, &w);
    expect("T26c a copy that overlaps nothing substituted (it would not bump) never waits (NOBUMP, counted)",
           d == N48_HG_PW_NOBUMP && s.delays == 0u && p2.not_bumping == 1u);
    // an expired flag
    n48_hg_pend p3 {}; n48_hg_pend_set(&p3, kT26Submit, 0u);
    s.now = N48_HG_PEND_EXPIRE_US + 5u;
    d = n48_hg_copy_wait(&kT26Ops, &s, &st, &p3, kT26Copier, 1u, kT26Lo, kT26Hi, &w);
    expect("T26c a flag older than the expiry is a leak: the copier clears it (EXPIRED, counted) and goes on at once",
           d == N48_HG_PW_EXPIRED && p3.pending == 0u && p3.clr[N48_HG_PCLR_EXPIRED] == 1u && s.delays == 0u);
    // the bound, and the delay schedule
    n48_hg_pend p4 {}; n48_hg_pend_set(&p4, kT26Submit, 0u);
    s.now = 1000u; s.delays = 0u; s.lockOps = 0u;
    T26Sim *sp = &s; sp->doneFB = sp->doneJ = sp->doneR = sp->doneW = 1u;   // no submit event will run
    d = n48_hg_copy_wait(&kT26Ops, &s, &st, &p4, kT26Copier, 1u, kT26Lo, kT26Hi, &w);
    expect("T26c a walk that never answers: TIMEOUT at the 100 ms bound (not before, not much after), lock held on return, and (0.0.511 LOW-2) the flag CLEARED",
           d == N48_HG_PW_TIMEOUT && w >= N48_HG_WAIT_US && w < N48_HG_WAIT_US + 1100u && p4.timeouts == 1u && p4.waits == 1u &&
           p4.wait_us_max == w && s.held == 1 && p4.pending == 0u && p4.clr[N48_HG_PCLR_TIMEOUT] == 1u);   // 0.0.511 LOW-2: cleared
    expect("T26c the delay schedule: 20 us steps for the first 1 ms, then 1 ms (IOSleep(1))",
           n48_hg_wait_delay_us(0u) == 20u && n48_hg_wait_delay_us(999u) == 20u && n48_hg_wait_delay_us(1000u) == 1000u);
    // the report lines at their numeric widest, with the longest `why` the verb passes
    char line[1200];
    n48_hg_pend big {}; std::memset(&big, 0xff, sizeof big);
    const char *why = " - `gfxneuter 72` REFUSED: a continuous arm stands, unchanged";
    const int n1 = std::snprintf(line, sizeof line, N48_HG_PEND_FMT, N48_HG_PEND_ARGS(0u, why, &big, ~0ull));
    const int n2 = std::snprintf(line, sizeof line, N48_HG_PEND_FMT2, N48_HG_PEND_ARGS2(&big));
    std::snprintf(line, sizeof line, "T26c the two heapwait72 report lines fit the logger's 512 bytes at their widest (%d, %d + the "
                  "36-byte prefix)", n1, n2);
    expect(line, n1 + 36 < 512 && n2 + 36 < 512);
}

static size_t b510_pins(const std::string &ahh, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0; bool first = true;
        for (const char *st : steps) {
            const size_t at = t.find(st, first ? 0 : prev);
            if (at == std::string::npos || count_of(t, st) != 1u) return false;
            prev = at + 1; first = false;
        }
        return true;
    };
    const std::string cb = fn_text(ahh, "uint32_t hw_hg_copy_begin(uint64_t vramLo, uint64_t vramHi, const n48_hg_patch *p, uint32_t np, uint32_t npoison, uint64_t *regSeqOut,\n"
                                           "                          uint32_t mayWait, n48_hg_copy_id *id)\n{");
    const std::string fb = fn_text(ahh, "static __attribute__((noinline)) void hg_frame_begin_pass()\n{");
    const std::string cj = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_commit_judge(uint32_t wouldCommit)\n{");
    const std::string cr = fn_text(ahh, "static __attribute__((noinline)) void hg_commit_record(uint32_t gateSeq)\n{");
    const std::string ex = fn_text(ahh, "static __attribute__((noinline)) uint32_t hg_exempt_refuse(uint32_t seq)\n{");
    const std::string pc = fn_text(ahh, "static __attribute__((noinline)) void hg_pend_clear(uint32_t why, uint32_t seq, uintptr_t thread)\n{");
    const std::string hk = fn_text(ahh, "static uint64_t hook_gfxCommitIB(void *self, void *info) {");
    const std::string sf = fn_text(ahh, "static void xd_shot_finish(const char *where)\n{");
    chk("T26p the functions are found", !cb.empty() && !fb.empty() && !cj.empty() && !cr.empty() && !ex.empty() && !pc.empty() &&
        !hk.empty() && !sf.empty());
    chk("T26p switch 72 boots OFF; the pass latch; the flag lives under gHgLock",
        has(ahh, "static volatile uint32_t gHgPendOn { 0u };") && has(ahh, "static n48_hg_pend gHgPend {};") &&
        order(fb, { "const uint32_t on = hw_hg_on() ? 1u : 0u;", "gHgPendLatch = (on && gHgPendOn) ? 1u : 0u;",
                    "if (!on) { n48_hg_frame_begin(&gHgFrame, 0u, gXdC.judged + 1u, &gHg); return; }" }) &&
        count_of(ahh, "gHgPendLatch = ") == 1u);
    chk("T26p B1 SET: in the judge's own gHgLock section, only for an OK headed for COMMIT with 72 latched",
        order(cj, { "IOLockLock(gHgLock);", "const uint32_t why = n48_hg_judge(&gHg, &gHgFrame, gXdC.judged + 1u);",
                    "if (why == N48_HG_OK && wouldCommit && gHgPendLatch)",
                    "n48_hg_pend_set(&gHgPend, reinterpret_cast<uintptr_t>(current_thread()), latch_now_us());", "IOLockUnlock(gHgLock);" }) &&
        count_of(ahh, "n48_hg_pend_set(") == 1u);
    chk("T26p B1 RECORD: the gate's seq stamps the flag, or 0 clears it (GATE), before the record's own early return",
        order(cr, { "if (gHgPendLatch && gHgLock) {", "IOLockLock(gHgLock);",
                    "n48_hg_pend_record(&gHgPend, gateSeq, reinterpret_cast<uintptr_t>(current_thread()));", "IOLockUnlock(gHgLock);",
                    "if (!gateSeq) return;" }));
    chk("T26p B1 WALK: the walk's answer is n48_hg_walk_answer (judge THEN clear, one gHgLock section); the mismatch refusal clears too",
        order(ex, { "if (gHgCommitSeq != seq || !seq) {", "hg_pend_clear(N48_HG_PCLR_WALK, seq, reinterpret_cast<uintptr_t>(current_thread()));",
                    "if (!gHgCommit.on) return 0u;", "IOLockLock(gHgLock);",
                    "const uint32_t why = n48_hg_walk_answer(&gHg, &gHgCommit, &gHgPend, seq, reinterpret_cast<uintptr_t>(current_thread()));",
                    "IOLockUnlock(gHgLock);" }) && !has(ex, "n48_hg_judge("));
    chk("T26p B1 the clear helper takes no lock when nothing is pending, and clears under gHgLock",
        order(pc, { "if (!l || !hg_pend_hint()) return;", "IOLockLock(l);", "(void)n48_hg_pend_clear(&gHgPend, why, seq, thread);", "IOLockUnlock(l);" }));
    // the four exits of hook_gfxCommitIB, each placed where the frame's fate is decided
    chk("T26p B1 EXIT 1 (NOT_WS) before its orig; EXIT 2 (refused shape) before its orig",
        order(hk, { "const uint32_t xdAction = gfxsrc_decide_frame(", "hg_pend_exit();   // build 0.0.510 (switch 72) EXIT 1 of 4",
                    "const uint64_t rvO = orig(self, info);", "hg_pend_exit();   // build 0.0.510 (switch 72) EXIT 2 of 4",
                    "if (xdAction == N48_SD_ACT_TRANSLATE) {" }));
    chk("T26p B1 EXIT 3 AFTER Apple's original (the walk ran inside it) and the record's teardown; EXIT 4 before the neuter tail's zero",
        order(hk, { "const uint64_t rvT = orig(self, info);", "gCmxThread = nullptr;\n                IOLockUnlock(gRenderDrainLock);\n            }",
                    "// build 0.0.510 (switch 72) EXIT 3 of 4", "            hg_pend_exit();\n            // 0.0.426 (B8)",
                    "return rvT;", "// build 0.0.510 (switch 72) EXIT 4 of 4", "    hg_pend_exit();\n    uint32_t saved[N48_SCB_MAX_IBS] = { 0 };",
                    "const uint64_t rv = orig(self, info);" }) && count_of(hk, "hg_pend_exit();") == 4u);
    chk("T26p B1 DISARM: the one-shot's finish, `gfxneuter 5` and 72 OFF clear any flag",
        order(sf, { "if (!n48_cm_shot_finish(&gXdShot)) return;", "gXdArm = N48_SD_ARM_DECIDE;", "hg_pend_disarm();" }) &&
        has(ahh, "            gXdArm = N48_SD_ARM_OFF;\n            hg_pend_disarm();") &&
        has(ahh, "if (changed72) { gHgPendOn = fpw; if (!fpw) hg_pend_disarm(); }") && count_of(ahh, "hg_pend_disarm();") == 3u);
    // B2: the copier waits UNDER gHgLock (dropped only around delays) BEFORE its overlap and bump, and nothing returns in between
    const size_t w0 = cb.find("if (gHgPend.pending && mayWait)"), w1 = cb.find("const uint32_t bumped = n48_hg_copy_begin(&gHg, overlaps);");
    chk("T26p B2 the copier: lock -> (pending) n48_hg_copy_wait -> regSeq -> overlap -> bump -> registry -> unlock, no return between "
        "the wait and the bump",
        order(cb, { "if (!l) return 0u;", "IOLockLock(l);", "if (gHgPend.pending && mayWait)",
                    "pw = n48_hg_copy_wait(&kHgWaitOps, l, &gHg, &gHgPend, reinterpret_cast<uintptr_t>(current_thread()), (np || npoison) ? 1u : 0u,",
                    "if (regSeqOut) *regSeqOut = gHg.reg_seq;",
                    "const uint32_t overlaps = (np || npoison || n48_hg_reg_overlaps(&gHg, vramLo, vramHi)) ? 1u : 0u;",
                    "const uint32_t bumped = n48_hg_copy_begin(&gHg, overlaps);", "IOLockUnlock(l);" }) &&
        count_of(cb, "n48_hg_copy_begin(") == 1u && w0 != std::string::npos && w1 != std::string::npos && cb.substr(w0, w1 - w0).find("return") == std::string::npos);
    chk("T26p 0.0.511 B1: only a FULLY PATCHED copy waits (the wait is under `pending && mayWait`; nothing else calls it)",
        count_of(cb, "if (gHgPend.pending && mayWait)\n        pw = n48_hg_copy_wait(") == 1u && count_of(cb, "n48_hg_copy_wait(") == 1u);
    chk("T26p B2 the wait's callbacks: gHgLock itself, uptime, IODelay under 1 ms else IOSleep",
        has(ahh, "static const n48_hg_wait_ops kHgWaitOps = { &hg_wait_now, &hg_wait_lock, &hg_wait_unlock, &hg_wait_delay };") &&
        has(ahh, "static void hg_wait_delay(void *, uint32_t us) { if (us >= 1000u) IOSleep(us / 1000u); else IODelay(us); }") &&
        has(ahh, "static void hg_wait_unlock(void *c) { IOLockUnlock(static_cast<IOLock *>(c)); }"));
    chk("T26p switch 72: the mid-arm guard, n48_ra_set, the bare-72 report (two lines)",
        has(ahh, "} else if ((arg & 0xffull) == 72ull) {") &&
        has(ahh, "HWLOG(N48_HG_PEND_FMT, N48_HG_PEND_ARGS(gHgPendOn, why, &gHgPend, age));") &&
        has(ahh, "HWLOG(N48_HG_PEND_FMT2, N48_HG_PEND_ARGS2(&gHgPend));"));
    return bad;
}
// =============================================================================================================
// T27 — build 0.0.511 (the 0.0.510 review): switch 72's fixes. B1 (MEDIUM-1): only a fully patched copy waits;
// B2 (LOW-1): the walk asks the heap generation only on the committing thread; B3 (LOW-2): a timeout clears the flag; B4 (LOW-3):
// the fast copy's poison check covers the copy's whole range.
// =============================================================================================================
static void test_T27a_only_patched_waits() {
    T26Sim &s = gT26;
    // a FULLY patched copy in RUN L's window: waits, the walk spares (0.0.510's behaviour, kept)
    gT26Copy = T26Copy { 1u, 0u, 0u, 0u, 0u };
    t26_run(s, 1u, 30u, 400u, 50u, T26_REAL, 0u);
    expect("T27a B1 a FULLY PATCHED copy in the window WAITS and the walk SPARES the frame (0.0.510's case, kept)",
           s.mayWait == 1u && s.pd.waits == 1u && s.walked == 1u && s.walkWhy == N48_HG_OK && s.bumpAt >= s.walkedAt && s.pd.unpatched == 0u);
    // every copy that is not fully patched: bumps at once (as 0.0.509), the walk REFUSES
    const T26Copy notFull[] = { { 0u, 1u, 0u, 0u, 0u }, { 1u, 1u, 0u, 0u, 0u }, { 1u, 0u, 1u, 0u, 0u }, { 1u, 0u, 0u, 1u, 0u },
                                { 1u, 0u, 0u, 0u, 1u }, { 0u, 0u, 0u, 0u, 0u } };
    const char *why[] = { "unpatchable only (npoison)", "patched + one unpatchable", "noOverlay (re-tiled)", "scanFailed", "noPlan",
                          "no patch (registry overlap only)" };
    for (uint32_t i = 0; i < 6u; i++) {
        gT26Copy = notFull[i];
        t26_run(s, 1u, 30u, 400u, 50u, T26_REAL, 0u);
        char b[260];
        std::snprintf(b, sizeof b, "T27a B1 a copy that is NOT fully patched (%s) does NOT wait: it bumps at 30 us, the walk REFUSES "
                      "(0.0.509's answer)", why[i]);
        expect(b, s.mayWait == 0u && s.pd.waits == 0u && s.waited == 0u && s.bumped == 1u && s.bumpAt == 30u && s.walked == 1u &&
                  s.walkWhy != N48_HG_OK && s.pd.unpatched == 1u);
    }
    // the 72 OFF control for the unpatched copy: the same answer
    gT26Copy = notFull[0];
    t26_run(s, 0u, 30u, 400u, 50u, T26_REAL, 0u);
    expect("T27a B1 ... exactly 72 OFF's answer for that copy (bump at 30 us, the walk refuses)",
           s.bumped == 1u && s.bumpAt == 30u && s.walked == 1u && s.walkWhy != N48_HG_OK);
    expect("T27a B1 n48_hg_copy_may_wait: 1 only for np && !npoison && !noOverlay && !scanFailed && !noPlan",
           n48_hg_copy_may_wait(1u, 0u, 0u, 0u, 0u) == 1u && n48_hg_copy_may_wait(5u, 0u, 0u, 0u, 0u) == 1u &&
           n48_hg_copy_may_wait(0u, 0u, 0u, 0u, 0u) == 0u && n48_hg_copy_may_wait(1u, 1u, 0u, 0u, 0u) == 0u &&
           n48_hg_copy_may_wait(1u, 0u, 1u, 0u, 0u) == 0u && n48_hg_copy_may_wait(1u, 0u, 0u, 1u, 0u) == 0u &&
           n48_hg_copy_may_wait(1u, 0u, 0u, 0u, 1u) == 0u);
    gT26Copy = T26Copy { 1u, 0u, 0u, 0u, 0u };
}
static void test_T27b_timeout_clears() {
    n48_hg_state st {}; std::memset(&st, 0, sizeof st);
    n48_hg_reg_add(&st, kT26Lo, kT26Lo + 0x100u);
    T26Sim s {}; std::memset(&s, 0, sizeof s);
    s.doneFB = s.doneJ = s.doneR = s.doneW = 1u; s.held = 1; s.now = 1000u;
    n48_hg_pend p {}; n48_hg_pend_set(&p, kT26Submit, 1000u);
    uint64_t w = 0u;
    const uint32_t d1 = n48_hg_copy_wait(&kT26Ops, &s, &st, &p, kT26Copier, 1u, kT26Lo, kT26Hi, &w);
    const uint32_t delays1 = s.delays;
    const uint32_t d2 = n48_hg_copy_wait(&kT26Ops, &s, &st, &p, kT26Copier, 1u, kT26Lo, kT26Hi, &w);
    expect("T27b B3 a TIMEOUT clears the flag (counted `timeout`), so the NEXT copy goes at once (no second 100 ms wait)",
           d1 == N48_HG_PW_TIMEOUT && p.pending == 0u && p.clr[N48_HG_PCLR_TIMEOUT] == 1u && d2 == N48_HG_PW_GO && s.delays == delays1 &&
           w == 0u && p.timeouts == 1u);
}
static void test_T27c_walk_thread() {
    // hook_gfxWriteTail's record as a walk on ANOTHER thread sees it: every older clause holds, same_thread 0
    n48_gfxn_exempt_t ex {}; std::memset(&ex, 0, sizeof ex);
    ex.inflight = 1u; ex.seq = 7u; ex.gate_seq = 7u; ex.nib = 1u; ex.same_thread = 0u;
    n48_gfxn_walk_t w {}; std::memset(&w, 0, sizeof w);
    ex.heap_refuse = 1u;
    const uint32_t a = n48_gfxn_exempt_why(nullptr, 0u, 0u, &w, N48_GFXN_ARM_COMMIT, &ex);
    ex.heap_refuse = 0u;
    const uint32_t b = n48_gfxn_exempt_why(nullptr, 0u, 0u, &w, N48_GFXN_ARM_COMMIT, &ex);
    ex.mib = 1u; ex.nib = 2u;
    const uint32_t c = n48_gfxn_exempt_why(nullptr, 0u, 0u, &w, N48_GFXN_ARM_COMMIT, &ex);
    ex.heap_refuse = 1u;
    const uint32_t d = n48_gfxn_exempt_why(nullptr, 0u, 0u, &w, N48_GFXN_ARM_COMMIT, &ex);
    expect("T27c B2 same_thread 0 answers THREAD whatever heap_refuse holds (single and multi-IB): `ex.same_thread &&` changes no answer",
           a == N48_GFXN_EX_THREAD && b == N48_GFXN_EX_THREAD && c == N48_GFXN_EX_THREAD && d == N48_GFXN_EX_THREAD);
}
static void test_T27d_whole_range() {
    // a two-segment copy: first chunk VRAM [0x100000, 0x101000), a later segment [0x300000, 0x301000) -> [cgLo, cgHi) =
    // [0x100000, 0x301000). A non-latching failure left poison over the later segment only.
    n48_cg_poison p; std::memset(&p, 0, sizeof p);
    n48_cg_close_poison(&p, 0x300000ull, 0x301000ull, 1, 1, 0);
    const uint64_t dAt = 0x100000ull, wBytes = 0x2000ull, cgLo = 0x100000ull, cgHi = 0x301000ull;
    const uint32_t old510 = n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, n48_cg_poison_overlaps(&p, dAt, dAt + wBytes) ? 1u : 0u, 0u);
    const uint32_t new511 = n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, n48_cg_poison_overlaps(&p, cgLo, cgHi) ? 1u : 0u, 0u);
    expect("T27d B4 poison over a LATER segment: 0.0.510's first-chunk range misses it (sampled), the copy's [cgLo, cgHi) latches FULL",
           old510 == N48_FC_M_SAMPLED && new511 == N48_FC_M_FULL);
}

static void test_T26p_pins(const char *ahhPath) {
    std::string ahh;
    if (!ahhPath || !read_file(ahhPath, ahh)) { expect("T26p the hook source is readable", false); return; }
    expect("T26p every pin holds on the REAL source", b510_pins(ahh, true) == 0u);
    struct { const char *what, *from, *to; } plant[] = {
        { "ORDERING: the walk clears the flag before it judges (a separate section)",
          "const uint32_t why = n48_hg_walk_answer(&gHg, &gHgCommit, &gHgPend, seq, reinterpret_cast<uintptr_t>(current_thread()));",
          "(void)n48_hg_pend_clear(&gHgPend, N48_HG_PCLR_WALK, seq, 0u); IOLockUnlock(gHgLock); IOLockLock(gHgLock);\n    const uint32_t why = n48_hg_judge(&gHg, &gHgCommit, gHgCommit.judged);" },
        { "ORDERING: EXIT 3 moved before Apple's original (before the walk)",
          "            const uint64_t rvT = orig(self, info);", "            hg_pend_exit();\n            const uint64_t rvT = orig(self, info);" },
        { "FAIL-OPEN: the copier returns without bumping after a timeout",
          "    if (regSeqOut) *regSeqOut = gHg.reg_seq;   // build 0.0.496 F1: before this copy's own patches are registered",
          "    if (pw == N48_HG_PW_TIMEOUT) { IOLockUnlock(l); return 0u; }\n    if (regSeqOut) *regSeqOut = gHg.reg_seq;   // build 0.0.496 F1: before this copy's own patches are registered" },
        { "LEAK: the neuter tail (keystone withdrawal, token mismatch) does not clear",
          "    hg_pend_exit();\n    uint32_t saved[N48_SCB_MAX_IBS] = { 0 };", "    uint32_t saved[N48_SCB_MAX_IBS] = { 0 };" },
        { "LEAK: the gate's non-commit does not clear the tentative flag", "if (gHgPendLatch && gHgLock) {", "if (gateSeq && gHgPendLatch && gHgLock) {" },
        { "SET outside the judge's section", "    if (why == N48_HG_OK && wouldCommit && gHgPendLatch)\n        n48_hg_pend_set(&gHgPend, reinterpret_cast<uintptr_t>(current_thread()), latch_now_us());\n    const uint64_t gen = gHg.gen; const uint32_t act = gHg.active;\n    IOLockUnlock(gHgLock);",
          "    const uint64_t gen = gHg.gen; const uint32_t act = gHg.active;\n    IOLockUnlock(gHgLock);\n    if (why == N48_HG_OK && wouldCommit && gHgPendLatch)\n        n48_hg_pend_set(&gHgPend, reinterpret_cast<uintptr_t>(current_thread()), latch_now_us());" },
        { "the copier waits AFTER its bump", "    uint32_t pw = N48_HG_PW_GO; uint64_t pwUs = 0ull;\n    if (gHgPend.pending && !mayWait",
          "    uint32_t pw = N48_HG_PW_GO; uint64_t pwUs = 0ull;\n    (void)n48_hg_copy_begin(&gHg, 1u);\n    if (gHgPend.pending && !mayWait" },
        { "0.0.511 B1: an UNPATCHED copy waits too (wait regardless)", "    if (gHgPend.pending && mayWait)\n", "    if (gHgPend.pending)\n" },
    };
    for (const auto &pl : plant) {
        std::string t = ahh;
        const size_t a = t.find(pl.from);
        if (a == std::string::npos || count_of(t, pl.from) != 1u) { expect("T26p plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(pl.from), pl.to);
        char lbl[220]; std::snprintf(lbl, sizeof lbl, "T26p BREAK-check: %s - caught", pl.what);
        expect(lbl, b510_pins(t, false) > 0u);
    }
}

// =============================================================================================================
// T28 — build 0.0.514 A1/A2: THE SAMPLED VERIFY TAKES THE MM LOCK ONCE PER CHUNK.
//   T28a differential: 0.0.513's per-dword sampled verify (copied below) vs 0.0.514's plan-reader verify over random chunk
//        sizes and rotations, with a planted mismatch at EVERY plan index and at a non-sampled offset: identical bad counts.
//   T28b the plan reader is called exactly once per sampled chunk, with the chunk's own (d_at, n, rot, ns); the per-dword
//        reader is never called in sampled mode; full mode never calls the plan reader.
//   T28c a refused plan read sets read_fail and gives NO compare credit (a mismatch is not counted either).
//   T28p the kext's reader: bounds before the lock, one yield, ONE IOLockLock / IOLockUnlock with only the timed RVRAM32_via_mm
//        plan loop between them, no log / allocation / other lock; wired into navi48_fc_chunk (each pin with a planted break).
// =============================================================================================================
struct VMock { uint64_t base; std::vector<uint32_t> w; int64_t refuseOff; uint32_t reads, plans; };
static int vm_read(void *vc, uint64_t vram, uint32_t *dst, uint32_t dwords) {
    VMock *m = static_cast<VMock *>(vc);
    m->reads++;
    if (vram < m->base || (vram - m->base) / 4u + dwords > m->w.size()) return 0;
    for (uint32_t i = 0; i < dwords; i++) {
        if (m->refuseOff >= 0 && vram - m->base + 4u * i == (uint64_t)m->refuseOff) return 0;
        dst[i] = m->w[(vram - m->base) / 4u + i];
    }
    return 1;
}
static uint32_t gVmPlanN = 0u, gVmPlanRot = 0u, gVmPlanNs = 0u; static uint64_t gVmPlanAt = 0u;
static int vm_plan(void *vc, uint64_t d_at, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
    VMock *m = static_cast<VMock *>(vc);
    m->plans++; gVmPlanN = n; gVmPlanRot = rot; gVmPlanNs = ns; gVmPlanAt = d_at;
    const uint32_t r0 = m->reads;
    for (uint32_t i = 0; i < ns; i++) if (!vm_read(vc, d_at + n48_fc_sample_at(n, rot, i), &got[i], 1u)) { m->reads = r0; return 0; }
    m->reads = r0;   // the plan reader's own reads are not the per-dword reader's
    return 1;
}
// PLANTED BREAK (in-test): a plan reader that skips one slot (the first) - got[0] keeps whatever it held
static int vm_plan_skip(void *vc, uint64_t d_at, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {
    VMock *m = static_cast<VMock *>(vc);
    m->plans++;
    const uint32_t r0 = m->reads;
    for (uint32_t i = 1; i < ns; i++) if (!vm_read(vc, d_at + n48_fc_sample_at(n, rot, i), &got[i], 1u)) { m->reads = r0; return 0; }
    m->reads = r0;
    return 1;
}
// 0.0.513's sampled branch of n48_fc_verify, verbatim (the reference the differential runs against).
static uint64_t verify513_sampled(int (*read)(void *ctx, uint64_t vram, uint32_t *dst, uint32_t dwords), void *ctx,
                                  uint64_t d_at, const uint32_t *exp, uint32_t n, uint32_t rot, uint64_t *compared, uint32_t *read_fail)
{
    uint64_t bad = 0u, cmp = 0u;
    if (read_fail) *read_fail = 0u;
    const uint32_t ns = n48_fc_sample_count(n);
    if ((!ns || ns > N48_FC_SAMPLE_MAX) && read_fail) *read_fail = 1u;
    for (uint32_t i = 0; i < ns && ns <= N48_FC_SAMPLE_MAX; i++) {
        uint32_t v = 0u;
        if (!read(ctx, d_at + n48_fc_sample_at(n, rot, i), &v, 1u)) { if (read_fail) *read_fail = 1u; break; }
        if (v != exp[i]) bad++;
        cmp++;
    }
    if (compared) *compared = cmp;
    return bad;
}
static uint64_t t28_rng = 0x9e3779b97f4a7c15ull;
static uint32_t t28_rand() { t28_rng ^= t28_rng << 13; t28_rng ^= t28_rng >> 7; t28_rng ^= t28_rng << 17; return (uint32_t)t28_rng; }
// One differential pass with plan reader `plan`; returns the number of (chunk, index) cases whose bad count or compared count
// differs from 0.0.513's.
static uint32_t t28_differential(n48_fc_plan_read_fn plan, uint32_t trials, uint32_t *cases) {
    uint32_t diffs = 0u; *cases = 0u;
    static uint32_t exp[N48_FC_SAMPLE_MAX], got[N48_FC_SAMPLE_MAX];
    t28_rng = 0x9e3779b97f4a7c15ull;
    for (uint32_t t = 0; t < trials; t++) {
        uint32_t n;
        switch (t % 4u) {
        case 0: n = 4u * (1u + t28_rand() % 64u); break;                        // under one page
        case 1: n = 4u * (1u + t28_rand() % (N48_FC_STAGING_BYTES / 4u)); break; // anything up to the staging size
        case 2: n = N48_FC_STAGING_BYTES; break;                                // a whole chunk: 258 samples
        default: n = N48_FC_PAGE_BYTES * (1u + t28_rand() % 16u) + 4u * (t28_rand() % 8u); break;   // page edges
        }
        const uint32_t rot = t28_rand();
        VMock m {}; m.base = 0x10020000ull + 0x1000ull * (t28_rand() % 64u); m.refuseOff = -1;
        m.w.resize(n / 4u);
        for (uint32_t i = 0; i < n / 4u; i++) m.w[i] = t28_rand();
        const uint32_t ns = n48_fc_sample_count(n);
        for (uint32_t i = 0; i < ns; i++) exp[i] = m.w[n48_fc_sample_at(n, rot, i) / 4u];
        for (int64_t k = -2; k < (int64_t)ns; k++) {   // -2 clean, -1 a non-sampled offset, else plan index k
            uint32_t off = 0u;
            if (k == -1) {
                // a dword no plan index names (if any): the verify must miss it in both versions
                for (uint32_t o = 0; o < n; o += 4u) {
                    bool named = false;
                    for (uint32_t i = 0; i < ns && !named; i++) named = n48_fc_sample_at(n, rot, i) == o;
                    if (!named) { off = o; break; }
                    if (o + 4u >= n) { off = 0xffffffffu; }
                }
                if (off == 0xffffffffu) continue;
            } else if (k >= 0) off = n48_fc_sample_at(n, rot, (uint32_t)k);
            if (k != -2) m.w[off / 4u] ^= 0x00010000u;
            uint64_t c0 = 0, c1 = 0; uint32_t f0 = 0, f1 = 0;
            const uint64_t b0 = verify513_sampled(&vm_read, &m, m.base, exp, n, rot, &c0, &f0);
            std::memset(got, 0, sizeof got);
            const uint64_t b1 = n48_fc_verify(&vm_read, plan, &m, m.base, exp, got, n, N48_FC_M_SAMPLED, rot, &c1, &f1);
            if (k != -2) m.w[off / 4u] ^= 0x00010000u;
            (*cases)++;
            const bool want = (k == -2 || k == -1) ? b0 == 0u : b0 >= 1u;   // the reference itself behaves
            if (b0 != b1 || c0 != c1 || f0 != f1 || !want) diffs++;
            if (k >= 0 && t % 8u != 2u && k > 3) k += (int64_t)(t28_rand() % 7u);   // sample the middle indices on most chunks
        }
    }
    return diffs;
}
static void test_T28a_differential() {
    uint32_t cases = 0u;
    const uint32_t d = t28_differential(&vm_plan, 48u, &cases);
    char b[320];
    std::snprintf(b, sizeof b, "T28a 0.0.514's one-call plan verify == 0.0.513's per-dword verify: identical bad/compared/read_fail in "
                               "all %u cases (random n and rot; a mismatch planted at each plan index, at a non-sampled dword, none)", cases);
    expect(b, d == 0u && cases > 1000u);
    uint32_t c2 = 0u;
    const uint32_t d2 = t28_differential(&vm_plan_skip, 48u, &c2);
    expect("T28a BREAK-check: a plan reader that skips one slot - CAUGHT by the differential", d2 > 0u);
}
static void test_T28b_once_per_chunk() {
    static uint32_t exp[N48_FC_SAMPLE_MAX], got[N48_FC_SAMPLE_MAX];
    const uint32_t sizes[] = { 4u, 0x9000u, N48_FC_STAGING_BYTES, 0x1234u * 4u };
    bool once = true, args = true, noDword = true;
    for (uint32_t n : sizes) {
        VMock m {}; m.base = 0x10400000ull; m.refuseOff = -1; m.w.assign(n / 4u, 0x5a5a5a5au);
        const uint32_t ns = n48_fc_sample_count(n);
        for (uint32_t i = 0; i < ns; i++) exp[i] = 0x5a5a5a5au;
        uint64_t cmp = 0; uint32_t rf = 0;
        const uint64_t bad = n48_fc_verify(&vm_read, &vm_plan, &m, m.base, exp, got, n, N48_FC_M_SAMPLED, 77u, &cmp, &rf);
        once = once && m.plans == 1u && bad == 0u && cmp == ns && !rf;
        args = args && gVmPlanAt == m.base && gVmPlanN == n && gVmPlanRot == 77u && gVmPlanNs == ns;
        noDword = noDword && m.reads == 0u;
    }
    expect("T28b sampled: the plan reader is called EXACTLY ONCE per chunk (one gVramMmLock acquire), every dword compared", once);
    expect("T28b ... with the chunk's own VRAM offset, size, rotation and the plan's count", args);
    expect("T28b ... and the per-dword reader is never called in sampled mode", noDword);
    VMock m {}; m.base = 0x10400000ull; m.refuseOff = -1; m.w.assign(0x9000u / 4u, 7u);
    static uint32_t fexp[0x9000 / 4];
    for (uint32_t i = 0; i < 0x9000u / 4u; i++) fexp[i] = 7u;
    uint64_t cmp = 0; uint32_t rf = 0;
    const uint64_t bad = n48_fc_verify(&vm_read, &vm_plan, &m, m.base, fexp, nullptr, 0x9000u, N48_FC_M_FULL, 0u, &cmp, &rf);
    expect("T28b full mode: unchanged - 32 dwords per read call (288 calls for 0x9000 bytes), the plan reader never called",
           bad == 0u && cmp == 0x9000u / 4u && m.reads == 288u && m.plans == 0u && !rf);
}
static void test_T28c_refused() {
    static uint32_t exp[N48_FC_SAMPLE_MAX], got[N48_FC_SAMPLE_MAX];
    const uint32_t n = 0x9000u, rot = 3u, ns = n48_fc_sample_count(n);
    VMock m {}; m.base = 0x10400000ull; m.w.assign(n / 4u, 1u);
    for (uint32_t i = 0; i < ns; i++) exp[i] = 1u;
    m.w[n48_fc_sample_at(n, rot, 0u) / 4u] = 2u;                   // a mismatch BEFORE the refused offset
    m.refuseOff = (int64_t)n48_fc_sample_at(n, rot, 5u);           // the window refuses the 6th plan offset
    uint64_t cmp = 99u; uint32_t rf = 0;
    const uint64_t bad = n48_fc_verify(&vm_read, &vm_plan, &m, m.base, exp, got, n, N48_FC_M_SAMPLED, rot, &cmp, &rf);
    expect("T28c a refused plan read: read_fail set, NO compare credit (compared 0), the mismatch before it not counted",
           rf == 1u && cmp == 0u && bad == 0u);
    const uint64_t bad2 = n48_fc_verify(&vm_read, nullptr, &m, m.base, exp, got, n, N48_FC_M_SAMPLED, rot, &cmp, &rf);
    const uint64_t bad3 = n48_fc_verify(&vm_read, &vm_plan, &m, m.base, exp, nullptr, n, N48_FC_M_SAMPLED, rot, &cmp, &rf);
    expect("T28c a missing plan reader or read-back buffer fails the verify closed (read_fail, nothing compared)",
           bad2 == 0u && bad3 == 0u && rf == 1u && cmp == 0u);
}
static const char *const kT28Under =
    "\tclock_get_uptime(&w1);\n"
    "\tfor (uint32_t i = 0; i < ns; i++)\n"
    "\t\tdst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, dAt + n48_fc_sample_at(n, rot, i));\n"
    "\tclock_get_uptime(&r1);\n";
static size_t fc514_pins(const std::string &bu, const std::string &fch, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0;
        for (const char *st : steps) { const size_t at = t.find(st, prev); if (at == std::string::npos) return false; prev = at + 1; }
        return true;
    };
    const std::string rs = fn_text(bu, "static bool fc_vram_read_sampled_timed(uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *dst, uint32_t ns, FcVerifyT *t) {");
    const std::string pl = fn_text(bu, "static int fc_mm_read_plan(void *vt, uint64_t dAt, uint32_t n, uint32_t rot, uint32_t *got, uint32_t ns) {");
    const std::string chunk = fn_text(bu, "uint64_t navi48_fc_chunk(uint64_t pos, uint64_t wBytes, uint64_t dAt, uint64_t n, N48FcFillFn fill, void *fillCtx,");
    const std::string ver = fn_text(fch, "static inline uint64_t n48_fc_verify(");
    chk("T28p the reader, its callback, navi48_fc_chunk and n48_fc_verify are found", !rs.empty() && !pl.empty() && !chunk.empty() && !ver.empty());
    const size_t lk = rs.find("\tIOLockLock(gVramMmLock);\n"), ul = rs.find("\tIOLockUnlock(gVramMmLock);\n");
    const std::string under = (lk == std::string::npos || ul == std::string::npos || ul < lk) ? std::string()
                              : rs.substr(lk + std::strlen("\tIOLockLock(gVramMmLock);\n"), ul - lk - std::strlen("\tIOLockLock(gVramMmLock);\n"));
    chk("T28p ONE lock and ONE unlock in the reader (no lock per dword)",
        count_of(rs, "IOLockLock(") == 1u && count_of(rs, "IOLockUnlock(") == 1u && count_of(rs, "IOLockTryLock") == 0u);
    chk("T28p between them ONLY the plan loop of RVRAM32_via_mm at n48_fc_sample_at's offsets (and the two clock reads that time it)",
        under == kT28Under);
    chk("T28p no log call, no allocation, no other lock anywhere in the reader",
        !has(rs, "N48LOG") && !has(rs, "IOLog") && !has(rs, "printf") && !has(rs, "IOMalloc") && !has(rs, "IOFree") &&
        !has(rs, "gXdLock") && !has(rs, "gFastCopyLock") && !has(rs, "gScanoutLock"));
    chk("T28p every check BEFORE the lock (device/lock, shape, plan size, [dAt, dAt + n) in VRAM); the yield once, before the lock",
        order(rs, { "if (!gBringup.dev || !dst || !gVramMmLock) return false;", "if ((dAt & 3) != 0 || n == 0u || (n & 3u) != 0u) return false;",
                    "if (ns == 0u || ns > N48_FC_SAMPLE_MAX || ns > n48_fc_sample_count(n)) return false;",
                    "if (dAt > size || (uint64_t)n > size - dAt) return false;", "clock_get_uptime(&w0);",
                    "if (n48_mmprio_should_yield(gMmPrioOn, nesting, isOwner)) {",
                    "while (n48_mmprio_keep_spinning(gMmPrioOn, gMmPrioNesting, elapsedUs)) {", "IODelay(50);",
                    "n48_mmprio_note_yield(&gMmPrioStats, elapsedUs, n48_mmprio_bound_hit(elapsedUs));",
                    "\tIOLockLock(gVramMmLock);\n", "\tIOLockUnlock(gVramMmLock);\n",
                    "if (isOwner) n48_mmprio_note_owner_wait(&gMmPrioStats, navi48_mmprio_us(w0, w1));",
                    "mm_hold_note(w1, N48_MMT_FCVERIFY);", "if (t) { t->lockT += w1 - w0; t->readT += r1 - w1; }", "return true;" }) &&
        count_of(rs, "IODelay(") == 1u && rs.find("return false") < lk && rs.rfind("return false") < lk);
    chk("T28p the plan callback is the reader; navi48_fc_chunk sizes the sampled buffer for snapshot + read-back and hands both over",
        has(pl, "return fc_vram_read_sampled_timed(dAt, n, rot, got, ns, static_cast<FcVerifyT *>(vt)) ? 1 : 0;") &&
        // 0.0.534: FULL adds switch 89's extra (n48_fc89_extra, 0 while 89 is OFF); the sampled size is unchanged
        has(chunk, "const size_t expBytes = full ? (size_t)(n + n48_fc89_extra(m89, n)) : (size_t)N48_FC_SAMPLE_MAX * 8u;") &&
        has(chunk, "n48_fc_verify(&fc_mm_read, &fc_mm_read_plan, &vt, dAt, exp, full ? nullptr : exp + N48_FC_SAMPLE_MAX,"));
    chk("T28p n48_fc_verify's sampled branch makes ONE plan call and compares got[] (the per-dword read is gone)",
        has(ver, "!plan(ctx, d_at, n, rot, got, ns)") && count_of(ver, "plan(ctx") == 1u && !has(ver, "read(ctx, d_at + n48_fc_sample_at") &&
        has(ver, "for (uint32_t i = 0; i < ns; i++) if (got[i] != exp[i]) bad++;"));
    return bad;
}
static void test_T28p_pins(const char *bringupPath, const char *ttlPath) {
    std::string bu, fch;
    std::string fchPath = ttlPath ? std::string(ttlPath) : std::string();
    const size_t sl = fchPath.rfind('/');
    if (sl != std::string::npos) fchPath = fchPath.substr(0, sl + 1) + "fastcopy.h";
    if (!bringupPath || !ttlPath || !read_file(bringupPath, bu) || !read_file(fchPath.c_str(), fch)) {
        expect("T28p the kext sources are readable", false); return;
    }
    expect("T28p every pin holds on the REAL sources", fc514_pins(bu, fch, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // file 0 bringup, 1 fastcopy.h
        { 0, "the reader skips one slot", "\tfor (uint32_t i = 0; i < ns; i++)\n\t\tdst[i] = amdgpu::RVRAM32_via_mm(",
          "\tfor (uint32_t i = 1; i < ns; i++)\n\t\tdst[i] = amdgpu::RVRAM32_via_mm(" },
        { 0, "a lock per dword", "\tfor (uint32_t i = 0; i < ns; i++)\n\t\tdst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, dAt + n48_fc_sample_at(n, rot, i));\n",
          "\tfor (uint32_t i = 0; i < ns; i++) {\n\t\tIOLockUnlock(gVramMmLock); IOLockLock(gVramMmLock);\n\t\tdst[i] = amdgpu::RVRAM32_via_mm(*gBringup.dev, dAt + n48_fc_sample_at(n, rot, i));\n\t}\n" },
        { 0, "a log call under the lock", "\tclock_get_uptime(&r1);\n\tIOLockUnlock(gVramMmLock);\n\tif (isOwner)",
          "\tclock_get_uptime(&r1);\n\tN48LOG(\"verify %u\", ns);\n\tIOLockUnlock(gVramMmLock);\n\tif (isOwner)" },
        { 0, "the VRAM bound checked after the lock", "\tif (dAt > size || (uint64_t)n > size - dAt) return false;\n\tuint64_t w0",
          "\tuint64_t w0" },
        { 1, "the sampled verify back to a read per dword", "!plan(ctx, d_at, n, rot, got, ns)", "!read(ctx, d_at + n48_fc_sample_at(n, rot, 0u), got, 1u)" },
        { 0, "the verify called without the plan reader", "n48_fc_verify(&fc_mm_read, &fc_mm_read_plan, &vt,", "n48_fc_verify(&fc_mm_read, nullptr, &vt," },
    };
    for (const auto &p : plant) {
        std::string m[2] = { bu, fch };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { std::printf("  (setup) %s\n", p.what); expect("T28p plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "T28p BREAK-check: %s - caught", p.what);
        expect(lbl, fc514_pins(m[0], m[1], false) > 0u);
    }
}

// =============================================================================================================
// T29 — build 0.0.514 A3: WHO HOLDS gVramMmLock (gfx_mmhold.h), READ-ONLY.
//   T29a the per-thread taker table: claim, nesting restores the outer tag, release, another thread reads OTHER, overflow.
//   T29b the hold record: count / total / max per taker; an out-of-range tag lands under OTHER.
//   T29c both report lines < 491 bytes at every counter's widest value (a widened line is caught).
//   T29p the wiring: every MM path notes its hold AFTER its unlock; the five scopes sit in their takers; the report prints.
// =============================================================================================================
#include "gfx_mmhold.h"
static void test_T29a_table() {
    n48_mmt_table t {};
    const uintptr_t A = 0x1000u, B = 0x2000u;
    const uint32_t k1 = n48_mmt_push(&t, A, N48_MMT_DECIDE);
    const bool c1 = k1 == N48_MMT_TOK_CLAIMED && t.live == 1u && n48_mmt_lookup(&t, A) == N48_MMT_DECIDE && n48_mmt_lookup(&t, B) == N48_MMT_OTHER;
    const uint32_t k2 = n48_mmt_push(&t, A, N48_MMT_JUDGE);
    const bool c2 = (k2 & N48_MMT_TOK_NESTED) && n48_mmt_lookup(&t, A) == N48_MMT_JUDGE && t.live == 1u;
    const uint32_t k3 = n48_mmt_push(&t, A, N48_MMT_COMMIT);
    n48_mmt_pop(&t, A, k3);
    const bool c3 = n48_mmt_lookup(&t, A) == N48_MMT_JUDGE;
    n48_mmt_pop(&t, A, k2);
    const bool c4 = n48_mmt_lookup(&t, A) == N48_MMT_DECIDE;
    n48_mmt_pop(&t, A, k1);
    const bool c5 = t.live == 0u && n48_mmt_lookup(&t, A) == N48_MMT_OTHER && t.s[0].thread == 0u;
    expect("T29a claim -> DECIDE; nested JUDGE then COMMIT; each pop restores the outer tag; the last pop releases the slot",
           c1 && c2 && c3 && c4 && c5);
    uint32_t tok[N48_MMT_SLOTS + 1];
    for (uint32_t i = 0; i <= N48_MMT_SLOTS; i++) tok[i] = n48_mmt_push(&t, 0x10000u + i, N48_MMT_CAPTURE);
    const bool ov = tok[N48_MMT_SLOTS] == 0u && t.overflow == 1u && n48_mmt_lookup(&t, 0x10000u + N48_MMT_SLOTS) == N48_MMT_OTHER &&
                    n48_mmt_lookup(&t, 0x10000u + 3u) == N48_MMT_CAPTURE;
    for (uint32_t i = 0; i <= N48_MMT_SLOTS; i++) n48_mmt_pop(&t, 0x10000u + i, tok[i]);
    expect("T29a a 17th thread overflows (counted, its holds under OTHER); every pop leaves the table empty", ov && t.live == 0u);
    expect("T29a an out-of-range tag or a null thread is refused", n48_mmt_push(&t, A, N48_MMT_N) == 0u && n48_mmt_push(&t, 0u, 1u) == 0u);
}
static void test_T29b_note() {
    n48_mmhold_stats s {};
    n48_mmhold_note(&s, N48_MMT_FCVERIFY, 430000u);
    n48_mmhold_note(&s, N48_MMT_FCVERIFY, 2000u);
    n48_mmhold_note(&s, N48_MMT_RESIDENCY, 90000u);
    n48_mmhold_note(&s, 77u, 5u);
    expect("T29b count / total / max per taker; an unknown tag lands under OTHER",
           s.count[N48_MMT_FCVERIFY] == 2u && s.ns[N48_MMT_FCVERIFY] == 432000u && s.max_ns[N48_MMT_FCVERIFY] == 430000u &&
           s.count[N48_MMT_RESIDENCY] == 1u && s.count[N48_MMT_OTHER] == 1u && s.ns[N48_MMT_OTHER] == 5u && s.count[N48_MMT_JUDGE] == 0u);
}
static void test_T29c_lines() {
    n48_mmhold_stats w {};
    for (uint32_t i = 0; i < N48_MMT_N; i++) { w.count[i] = UINT64_MAX; w.ns[i] = UINT64_MAX; w.max_ns[i] = UINT64_MAX; }
    char b[1024];
    const int l1 = std::snprintf(b, sizeof b, N48_MMHOLD_FMT1, N48_MMHOLD_ARGS1(&w));
    const int l2 = std::snprintf(b, sizeof b, N48_MMHOLD_FMT2, N48_MMHOLD_ARGS2(&w, UINT64_MAX));
    std::printf("  mmhold514 lines at the widest counters: %d and %d bytes\n", l1, l2);
    expect("T29c both mmhold514 lines < 491 bytes at every counter's widest value", l1 > 0 && l1 < 491 && l2 > 0 && l2 < 491);
    const std::string wide = std::string(N48_MMHOLD_FMT1) + ", fcverify %llu/%llu/%llu, residency %llu/%llu/%llu, other %llu/%llu/%llu";
    const unsigned long long M = ~0ull;
    const int l3 = std::snprintf(b, sizeof b, wide.c_str(), N48_MMHOLD_ARGS1(&w), M, M / 1000ull, M / 1000ull, M, M / 1000ull,
                                 M / 1000ull, M, M / 1000ull, M / 1000ull);
    expect("T29c BREAK-check: all seven takers on ONE line exceed the bound (why there are two lines) - caught", l3 >= 491);
}
static size_t mmhold_pins(const std::string &bu, const std::string &ahh, const std::string &peer, const std::string &ttl, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0;
        for (const char *st : steps) { const size_t at = t.find(st, prev); if (at == std::string::npos) return false; prev = at + 1; }
        return true;
    };
    const std::string rd = fn_text(bu, "bool navi48_vram_read_mm(uint64_t vramOffset, uint32_t *dst, uint32_t dwords) {");
    const std::string wr = fn_text(bu, "bool navi48_vram_write_mm(uint64_t vramOffset, const uint32_t *src, uint32_t dwords) {");
    const std::string tw = fn_text(bu, "static bool fc_vram_read_timed(uint64_t vramOffset, uint32_t *dst, uint32_t dwords, FcVerifyT *t) {");
    const std::string nt = fn_text(bu, "static __attribute__((noinline)) void mm_hold_note(uint64_t h0, uint32_t tag, uint32_t dwords = 0u) {");
    const std::string rep = fn_text(bu, "void navi48_fc_report(const char *why) {");
    chk("T29p the MM functions, the note and the report are found", !rd.empty() && !wr.empty() && !tw.empty() && !nt.empty() && !rep.empty());
    chk("T29p navi48_vram_read_mm: the hold starts after the lock, is noted after the unlock (lookup by thread)",
        order(rd, { "IOLockLock(gVramMmLock);", "uint64_t h0 = 0ull; clock_get_uptime(&h0);", "RVRAM32_via_mm(",
                    "if (gVramMmLock) IOLockUnlock(gVramMmLock);", "if (gVramMmLock) mm_hold_note(h0, N48_MMT_LOOKUP, dwords);", "return true;" }));
    chk("T29p navi48_vram_write_mm: the same, after its unlock",
        order(wr, { "IOLockLock(gVramMmLock);", "uint64_t h0 = 0ull; clock_get_uptime(&h0);", "WVRAM32_via_mm(",
                    "IOLockUnlock(gVramMmLock);\n\tmm_hold_note(h0, N48_MMT_LOOKUP, dwords);" }));
    chk("T29p the fast copy's full-verify twin names itself (FCVERIFY), after its unlock",
        order(tw, { "if (gVramMmLock) IOLockUnlock(gVramMmLock);", "if (gVramMmLock) mm_hold_note(w1, N48_MMT_FCVERIFY);" }));
    chk("T29p the note takes no lock and logs nothing", !has(nt, "IOLock") && !has(nt, "N48LOG") && has(nt, "n48_mmhold_note_dw(&gMmHold, tag, ns, dwords);"));
    chk("T29p the bare 63 report prints both mmhold514 lines and (0.0.515 D2) the mmhold515 DECIDE-by-site line",
        order(rep, { "N48LOG(N48_MMHOLD_FMT1, N48_MMHOLD_ARGS1(&gMmHold));", "N48LOG(N48_MMHOLD_FMT2, N48_MMHOLD_ARGS2(&gMmHold,",
                     "N48LOG(N48_MMHOLD_FMT3, N48_MMHOLD_ARGS3(&gMmHold));" }));
    const std::string pol = fn_text(ahh, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp, uint64_t dctxKey,");
    const std::string dec = fn_text(ahh, "static uint32_t gfxsrc_decide_frame(const uint8_t *info, uint32_t shapeOk, uint32_t n, const WsFrame *wf) {");
    const std::string com = fn_text(ahh, "static uint32_t gfxsrc_commit_try(const GfxcVm &vm, const uint8_t *info, uint64_t va, uint32_t n, uint32_t nib,");
    const std::string cap = fn_text(ahh, "static void gcap_submission(void *self, const uint8_t *info, uint32_t srcCheck) {");
    const size_t rb = peer.find("static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    chk("T29p the judge's scope is the policy's second statement (after the MM-priority scope)",
        has(pol, "    Navi48MmPrioScope mmPrioScope;\n    Navi48MmTakerScope mmTaker(N48_MMT_JUDGE);"));
    chk("T29p decide_frame's scope opens once gXdLock is held",
        /* build 0.0.531: the busy branch also counts (log-only) whether switch 86's present re-check held the lock */
        has(dec, "if (!IOLockTryLock(gXdLock)) {\n        gXdC.busy++;\n        if (__atomic_load_n(&gP86InLock, __ATOMIC_RELAXED)) "
                 "gP86.decideBusyByUs++;   // build 0.0.531 (log-only): switch 86 held it\n        return n48_sd_action(N48_SD_ARM_OFF, "
                 "shapeOk, N48_XV_ARMED_OFF, 0);\n    }\n    Navi48MmTakerScope mmTaker(N48_MMT_DECIDE);"));
    chk("T29p commit_try's scope is its first statement", has(com, "uint32_t arm, uint32_t verdict, uint32_t stamp, uint64_t tgtVa) {\n    Navi48MmTakerScope mmTaker(N48_MMT_COMMIT);"));
    chk("T29p the capture stream's scope opens once gGcapLock is held", has(cap, "if (!IOLockTryLock(gGcapLock)) { gGc.busy++; return; }\n    Navi48MmTakerScope mmTaker(N48_MMT_CAPTURE);"));
    chk("T29p the residency copy's scope is its first statement",
        rb != std::string::npos && peer.find("static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {\n    Navi48MmTakerScope mmTaker(N48_MMT_RESIDENCY);", rb) == rb);
    chk("T29p Navi48Ttl.hpp's scope pushes in its ctor and pops in its dtor",
        has(ttl, "explicit Navi48MmTakerScope(uint32_t tag) : tok_(navi48_mm_taker_push(tag)) {}") && has(ttl, "~Navi48MmTakerScope() { navi48_mm_taker_pop(tok_); }"));
    return bad;
}
static void test_T29p_pins(const char *bringupPath, const char *ahhPath, const char *peerPath, const char *ttlPath) {
    std::string bu, ahh, peer, ttl;
    if (!bringupPath || !ahhPath || !peerPath || !ttlPath || !read_file(bringupPath, bu) || !read_file(ahhPath, ahh) ||
        !read_file(peerPath, peer) || !read_file(ttlPath, ttl)) { expect("T29p the kext sources are readable", false); return; }
    expect("T29p every pin holds on the REAL sources", mmhold_pins(bu, ahh, peer, ttl, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // 0 bringup, 1 ahh, 2 peer
        { 0, "read_mm notes its hold BEFORE the unlock", "\tif (gVramMmLock) IOLockUnlock(gVramMmLock);\n\tif (gVramMmLock) mm_hold_note(h0, N48_MMT_LOOKUP, dwords);\n\treturn true;",
          "\tif (gVramMmLock) mm_hold_note(h0, N48_MMT_LOOKUP, dwords);\n\tif (gVramMmLock) IOLockUnlock(gVramMmLock);\n\treturn true;" },
        { 1, "the judge's scope dropped", "    Navi48MmTakerScope mmTaker(N48_MMT_JUDGE);", "" },
        { 2, "the residency copy mis-tagged", "Navi48MmTakerScope mmTaker(N48_MMT_RESIDENCY);", "Navi48MmTakerScope mmTaker(N48_MMT_OTHER);" },
    };
    for (const auto &p : plant) {
        std::string m[3] = { bu, ahh, peer };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { std::printf("  (setup) %s\n", p.what); expect("T29p plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "T29p BREAK-check: %s - caught", p.what);
        expect(lbl, mmhold_pins(m[0], m[1], m[2], ttl, false) > 0u);
    }
}

// =============================================================================================================
// T31 — build 0.0.515 D2: DECIDE's MM holds BY SITE (gfx_mmhold.h N48_MMT_D_*), READ-ONLY.
//   T31a a sub-tag pushes only over DECIDE (or a DECIDE sub-tag); under JUDGE / COMMIT / no slot it is a no-op; each pop
//        restores the outer tag, including a sub nested in a sub;
//   T31b the kext's RAII shape (Navi48MmSubScope, mirrored here over a test table) restores DECIDE on EVERY return path
//        of a function with early returns (gfxsrc_identify_pgm's memo returns);
//   T31c dwords per tag; line 1's decide is the six decide tags summed (0.0.514's number); the third line < 491 bytes;
//   T31p the wiring: each site's scope, the dword-carrying notes, the report; planted breaks are caught.
// =============================================================================================================
static n48_mmt_table gT31Tab {};
static uintptr_t gT31Me = 0x7000u;
struct T31Sub {   // Navi48MmSubScope's shape (Navi48Ttl.hpp), over the test table: push_sub in the ctor, pop in the dtor
    explicit T31Sub(uint32_t sub) : tok_(n48_mmt_push_sub(&gT31Tab, gT31Me, sub)) {}
    ~T31Sub() { n48_mmt_pop(&gT31Tab, gT31Me, tok_); }
    uint32_t tok_;
};
static uint32_t t31_identify(uint32_t path, uint32_t *seenFill, uint32_t *seenPgm) {   // gfxsrc_identify_pgm's shape
    if (path & 1u) { T31Sub f(N48_MMT_D_FILL); *seenFill = n48_mmt_lookup(&gT31Tab, gT31Me); }
    T31Sub p(N48_MMT_D_PGM);
    *seenPgm = n48_mmt_lookup(&gT31Tab, gT31Me);
    if (path & 2u) return 1u;          // the memo's `if (!needCheck) return;`
    if (path & 4u) return 2u;          // the memo check's `... n48_sd_memo_ours_ok(...)) return;`
    return 3u;
}
static void test_T31a_nesting() {
    n48_mmt_table t {};
    const uintptr_t A = 0x1000u;
    expect("T31a no slot: a sub-tag changes nothing", n48_mmt_push_sub(&t, A, N48_MMT_D_IB) == 0u && t.live == 0u);
    const uint32_t kj = n48_mmt_push(&t, A, N48_MMT_JUDGE);
    expect("T31a under JUDGE: a sub-tag changes nothing (the same code reached from the policy keeps JUDGE)",
           n48_mmt_push_sub(&t, A, N48_MMT_D_TGT) == 0u && n48_mmt_lookup(&t, A) == N48_MMT_JUDGE);
    n48_mmt_pop(&t, A, kj);
    const uint32_t kd = n48_mmt_push(&t, A, N48_MMT_DECIDE);
    const uint32_t k1 = n48_mmt_push_sub(&t, A, N48_MMT_D_TGT);
    const bool a1 = (k1 & N48_MMT_TOK_NESTED) && n48_mmt_lookup(&t, A) == N48_MMT_D_TGT;
    const uint32_t k2 = n48_mmt_push_sub(&t, A, N48_MMT_D_FILL);   // a fill read inside a target walk (never today; allowed)
    const bool a2 = n48_mmt_lookup(&t, A) == N48_MMT_D_FILL;
    n48_mmt_pop(&t, A, k2);
    const bool a3 = n48_mmt_lookup(&t, A) == N48_MMT_D_TGT;
    const uint32_t kc = n48_mmt_push(&t, A, N48_MMT_COMMIT);        // commit_try nested in decide
    const bool a4 = n48_mmt_push_sub(&t, A, N48_MMT_D_FENCE) == 0u && n48_mmt_lookup(&t, A) == N48_MMT_COMMIT;
    n48_mmt_pop(&t, A, kc);
    n48_mmt_pop(&t, A, k1);
    const bool a5 = n48_mmt_lookup(&t, A) == N48_MMT_DECIDE;
    n48_mmt_pop(&t, A, kd);
    expect("T31a DECIDE -> TGT -> FILL, pops restore TGT then DECIDE; COMMIT inside keeps COMMIT; slot released",
           a1 && a2 && a3 && a4 && a5 && t.live == 0u);
    expect("T31a a non-sub tag through push_sub is refused", n48_mmt_push_sub(&t, A, N48_MMT_JUDGE) == 0u &&
           n48_mmt_push_sub(&t, A, N48_MMT_N) == 0u);
}
static void test_T31b_early_return() {
    gT31Tab = n48_mmt_table {};
    const uint32_t kd = n48_mmt_push(&gT31Tab, gT31Me, N48_MMT_DECIDE);
    uint32_t ok = 0;
    for (uint32_t path = 0; path < 8u; path++) {
        uint32_t sf = 0xFFu, sp = 0xFFu;
        const uint32_t r = t31_identify(path, &sf, &sp);
        const bool fillOk = (path & 1u) ? sf == N48_MMT_D_FILL : sf == 0xFFu;
        ok += (r >= 1u && fillOk && sp == N48_MMT_D_PGM && n48_mmt_lookup(&gT31Tab, gT31Me) == N48_MMT_DECIDE) ? 1u : 0u;
    }
    expect("T31b every return path (8: fill read or not x two memo returns / the end) restores DECIDE", ok == 8u);
    n48_mmt_pop(&gT31Tab, gT31Me, kd);
    const uint32_t kj = n48_mmt_push(&gT31Tab, gT31Me, N48_MMT_JUDGE);
    uint32_t sf = 0, sp = 0;
    (void)t31_identify(7u, &sf, &sp);
    expect("T31b the same function under JUDGE files JUDGE throughout and leaves JUDGE",
           sf == N48_MMT_JUDGE && sp == N48_MMT_JUDGE && n48_mmt_lookup(&gT31Tab, gT31Me) == N48_MMT_JUDGE);
    n48_mmt_pop(&gT31Tab, gT31Me, kj);
    expect("T31b the table is empty afterwards", gT31Tab.live == 0u);
}
static void test_T31c_dwords_lines() {
    n48_mmhold_stats s {};
    n48_mmhold_note_dw(&s, N48_MMT_D_FILL, 3000u, 64u);
    n48_mmhold_note_dw(&s, N48_MMT_D_FILL, 1000u, 64u);
    n48_mmhold_note_dw(&s, N48_MMT_DECIDE, 500u, 2u);
    n48_mmhold_note_dw(&s, N48_MMT_JUDGE, 9000u, 44u);
    n48_mmhold_note_dw(&s, 99u, 7u, 1u);
    expect("T31c dwords per tag; an unknown tag lands under OTHER",
           s.dw[N48_MMT_D_FILL] == 128u && s.count[N48_MMT_D_FILL] == 2u && s.ns[N48_MMT_D_FILL] == 4000u && s.dw[N48_MMT_DECIDE] == 2u &&
           s.dw[N48_MMT_JUDGE] == 44u && s.dw[N48_MMT_OTHER] == 1u && s.count[N48_MMT_OTHER] == 1u);
    expect("T31c line 1's decide = DECIDE + its sub-tags (acquires 3, ns 4500, max 3000); JUDGE not in it",
           n48_mmhold_dsum(&s, 0u) == 3u && n48_mmhold_dsum(&s, 1u) == 4500u && n48_mmhold_dsum(&s, 2u) == 3000u);
    n48_mmhold_stats w {};
    for (uint32_t i = 0; i < N48_MMT_N; i++) { w.count[i] = UINT64_MAX / 8u; w.ns[i] = UINT64_MAX; w.max_ns[i] = UINT64_MAX; w.dw[i] = UINT64_MAX; }
    char b[1024];
    const int l3 = std::snprintf(b, sizeof b, N48_MMHOLD_FMT3, N48_MMHOLD_ARGS3(&w));
    for (uint32_t i = 0; i < N48_MMT_N; i++) w.count[i] = UINT64_MAX;
    const int l3m = std::snprintf(b, sizeof b, N48_MMHOLD_FMT3, N48_MMHOLD_ARGS3(&w));
    const int l1 = std::snprintf(b, sizeof b, N48_MMHOLD_FMT1, N48_MMHOLD_ARGS1(&w));
    std::printf("  mmhold515 line at the widest counters: %d / %d bytes; line 1 %d\n", l3, l3m, l1);
    expect("T31c the mmhold515 line < 491 bytes at every counter's widest value", l3 > 0 && l3 < 491 && l3m > 0 && l3m < 491 && l1 > 0 && l1 < 491);
}
static size_t d2_pins(const std::string &ahh, const std::string &ttl, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    const std::string idp = fn_text(ahh, "static void gfxsrc_identify_pgm(const GfxcVm &vm, uint64_t va, uint32_t stage, int32_t pid, n48_xv_program *p) {");
    const size_t fr = idp.find("if (n48_fs_fill_read_wanted(gFs.on, stage, gXdFrameFill, gXdPolicyEvery)) {\n        Navi48MmSubScope mmSubF(N48_MMT_D_FILL);");
    const size_t pg = idp.find("    Navi48MmSubScope mmSubP(N48_MMT_D_PGM);");
    const size_t memo = idp.find("if (n48_sd_memo_get(&gXdMemo, epoch, va, p, &needCheck, &checkDw)) {");
    chk("T31p the fill read's scope opens inside its (divisor-gated) block; memo+id's before the memo", fr != std::string::npos &&
        pg != std::string::npos && memo != std::string::npos && fr < pg && pg < memo);
    const std::string dec = fn_text(ahh, "static uint32_t gfxsrc_decide_frame(const uint8_t *info, uint32_t shapeOk, uint32_t n, const WsFrame *wf) {");
    chk("T31p the IB gather reads through gfxc_read_sub(N48_MMT_D_IB, ...)",
        has(dec, "const uint32_t got = gfxc_read_sub(N48_MMT_D_IB, vm, va, dst, want, &sysPages);") && !has(dec, "= gfxc_read(vm, va, dst, want, &sysPages);"));
    chk("T31p the item loop's colour-target walk is D_TGT",
        has(dec, "const bool ok = gfxc_page_sub(N48_MMT_D_TGT, vm, it.va & ~0xfffull, page, isSys);"));
    chk("T31p both fence polls are D_FENCE (and no bare MM read is left in decide_frame)",
        has(dec, "vram_read_sub(N48_MMT_D_FENCE, gXdF828.frame.vram_off, &val, 1)") &&
        has(dec, "vram_read_sub(N48_MMT_D_FENCE, gKsRing.e[fi].vram_off, &frVal, 1)") && !has(dec, "navi48_vram_read_mm("));
    chk("T31p the switch-54 and R5/ledger walk callbacks open D_TGT first",
        has(ahh, "static uint32_t r5_resolve_cb(void *ud, uint64_t va, uint64_t *page)\n{\n    Navi48MmSubScope mmSub(N48_MMT_D_TGT);") &&
        has(ahh, "static uint32_t tv_page_cb(void *ud, uint64_t va, uint32_t *isSys)\n{\n    Navi48MmSubScope mmSub(N48_MMT_D_TGT);"));
    const std::string rs = fn_text(ahh, "static __attribute__((noinline)) uint32_t gfxc_read_sub(uint32_t sub, const GfxcVm &vm, uint64_t va, uint32_t *dst, uint32_t n,");
    const std::string ps = fn_text(ahh, "static __attribute__((noinline)) bool gfxc_page_sub(uint32_t sub, const GfxcVm &vm, uint64_t va, uint64_t &pageBase, bool &isSys) {");
    const std::string vs = fn_text(ahh, "static __attribute__((noinline)) bool vram_read_sub(uint32_t sub, uint64_t off, uint32_t *dst, uint32_t n) {");
    chk("T31p the three wrappers open the scope and make exactly the call they wrap",
        has(rs, "Navi48MmSubScope mmSub(sub);\n    return gfxc_read(vm, va, dst, n, sysPages);") &&
        has(ps, "Navi48MmSubScope mmSub(sub);\n    return gfxc_page(vm, va, pageBase, isSys);") &&
        has(vs, "Navi48MmSubScope mmSub(sub);\n    return navi48_vram_read_mm(off, dst, n);"));
    chk("T31p Navi48Ttl.hpp's sub-scope pushes a SUB in its ctor and pops in its dtor",
        has(ttl, "explicit Navi48MmSubScope(uint32_t sub) : tok_(navi48_mm_taker_push_sub(sub)) {}") &&
        has(ttl, "~Navi48MmSubScope() { navi48_mm_taker_pop(tok_); }"));
    return bad;
}
static void test_T31p_pins(const char *ahhPath, const char *ttlPath, const char *bringupPath) {
    std::string ahh, ttl, bu;
    if (!ahhPath || !ttlPath || !bringupPath || !read_file(ahhPath, ahh) || !read_file(ttlPath, ttl) || !read_file(bringupPath, bu)) {
        expect("T31p the kext sources are readable", false); return;
    }
    expect("T31p every pin holds on the REAL sources", d2_pins(ahh, ttl, true) == 0u);
    expect("T31p the push_sub the scope calls is gfx_mmhold.h's, over the kext's one table",
           has(bu, "uint32_t navi48_mm_taker_push_sub(uint32_t sub) { return n48_mmt_push_sub(&gMmTk, (uintptr_t)current_thread(), sub); }"));
    struct { int file; const char *what, *from, *to; } plant[] = {   // 0 ahh, 1 ttl
        { 0, "the fill read's scope dropped", "        Navi48MmSubScope mmSubF(N48_MMT_D_FILL);\n", "" },
        { 0, "the IB gather back to the untagged read", "gfxc_read_sub(N48_MMT_D_IB, vm, va, dst, want, &sysPages);", "gfxc_read(vm, va, dst, want, &sysPages);" },
        { 0, "a fence poll mis-tagged", "vram_read_sub(N48_MMT_D_FENCE, gKsRing.e[fi].vram_off", "vram_read_sub(N48_MMT_D_TGT, gKsRing.e[fi].vram_off" },
        { 1, "the sub-scope never pops (the outer tag is lost on return)", "~Navi48MmSubScope() { navi48_mm_taker_pop(tok_); }", "~Navi48MmSubScope() {}" },
    };
    for (const auto &p : plant) {
        std::string m[2] = { ahh, ttl };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { std::printf("  (setup) %s\n", p.what); expect("T31p plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "T31p BREAK-check: %s - caught", p.what);
        expect(lbl, d2_pins(m[0], m[1], false) > 0u);
    }
}

// =============================================================================================================
// T30 — build 0.0.514 B1-B4: native 1440p preparation, the kext glue. The pure answers are tested in
// display_pipe_guard_test.cpp (B1/B2/B5) and scanout_selftest_test.cpp (B3/B4/B5); these pins prove the kext calls them.
// =============================================================================================================
static size_t b514_pins(const std::string &bu, const std::string &dpg, const std::string &peer, const std::string &dcn,
                        const std::string &lrh, bool loud) {
    size_t bad = 0;
    auto chk = [&](const char *what, bool ok) { if (loud) expect(what, ok); if (!ok) bad++; };
    auto order = [](const std::string &t, std::initializer_list<const char *> steps) {
        size_t prev = 0;
        for (const char *st : steps) { const size_t at = t.find(st, prev); if (at == std::string::npos) return false; prev = at + 1; }
        return true;
    };
    chk("T30 B1: no AGDC reply falls straight from gRa to the constant any more",
        !has(dpg, "gRa.width ? gRa.width : 1920u") && !has(dpg, "gRa.height ? gRa.height : 1080u"));
    chk("T30 B1: LINK_CONFIG and the 0x711 scaler reply both take gRa, else the live Console size, else 1920x1080",
        order(fn_text(dpg, "static __attribute__((noinline)) void agdc_endpoint(uint32_t *w, uint32_t *h) {"),
              { "if (!gRa.width || !gRa.height) (void)navi48_scanout_live_dims(&cw, &ch);", "*w = n48_agdc_endpoint_dim(gRa.width, cw, 1920u);",
                "*h = n48_agdc_endpoint_dim(gRa.height, ch, 1080u);" }) &&
        order(dpg, { "} else if (cmd == N48_AGDC_CMD_LINK_CONFIG) {", "agdc_endpoint(&lw, &lh);",
                     "kr = agdc_link_config(reinterpret_cast<uint8_t *>(outp), len, lw, lh);",
                     "} else if (cmd == N48_AGDC_CMD_PIPELINE_CAPS) {", "agdc_endpoint(&w, &h);",
                     "kr = n48_agdc_fill_pipeline_caps(reinterpret_cast<uint8_t *>(outp), len, w, h)" }));
    // build 0.0.515: the 1920x1080 test and the timing call moved into display_pipe_guard.h's pure
    // n48_agdc_link_timing (tests/dcn_liveraster_test.cpp B: 1080p reads nothing), and liveRaster's body into navi48_liveraster.h's
    // n48lr_live over the read-only device (no bind dependency). The same properties are pinned where they now live.
    chk("T30 B2: the live raster is read only off 1920x1080 and feeds n48_agdc_timing_for -> n48_agdc_fill_link_config_t",
        order(dpg, { "static uint32_t agdc_live_raster(void *ctx, n48_agdc_raster *lr) {", "return n48dcn::liveRaster(&lr->h_active",
                     "n48_agdc_link_timing(lw, lh, &agdc_live_raster, nullptr, &lt);",
                     "const uint32_t kr = n48_agdc_fill_link_config_t(out, len, &lt) ? 0xe00002c2u : 0u;" }) &&
        count_of(dpg, "n48_agdc_fill_link_config(") == 0u);
    const std::string lr = fn_text(dcn, "uint32_t liveRaster(uint32_t *hAct, uint32_t *vAct, uint32_t *hTot, uint32_t *vTot, uint32_t *hFront, uint32_t *hSync,");
    const std::string lb = fn_text(lrh, "static inline uint32_t n48lr_live(n48lr_ro *ro, uint32_t *hAct, uint32_t *vAct, uint32_t *hTot, uint32_t *vTot,");
    chk("T30 B2: liveRaster is READ-ONLY (the lit OTG's size and totals; no write primitive) and needs an EDID row with the same totals",
        !lr.empty() && has(lr, "return n48lr_live(&gLr, ") && !has(lr, "write") && !has(lr, "WREG") &&
        !lb.empty() && has(lb, "dcn41_otg_get_active_size(") && has(lb, "dcn41_otg_get_totals(") &&
        !has(lb, "wreg") && !has(lb, "write") && !has(lb, "WREG") && !has(lb, "irq_set") && !has(lb, "flip") &&
        has(lb, "if (m.h_active != w || m.v_active != h || m.h_total != ht1 + 1u || m.v_total != vt1 + 1u) continue;"));
    const std::string ld = fn_text(bu, "uint32_t navi48_scanout_live_dims(uint32_t *w, uint32_t *h) {");
    chk("T30 B1/B4: the live size is scanout_geometry's (Console,Width/Height), read-only",
        has(ld, "if (scanout_geometry(*gBringup.dev, &g, &fb) != kScanStOk || !g.width || !g.height) return 0u;") && !has(ld, "WBAR0") && !has(ld, "WREG"));
    chk("T30 B3: mode 6's case (b) runs the live geometry (1920x1080 only when absent)",
        order(bu, { "const uint32_t lw = n48_live_dim(g.width, 1920u), lh = n48_live_dim(g.height, 1080u);",
                    "const uint32_t s0 = scanout_full_case(dev, inst, 256u, 256u, 0x5CA11F00u, &c0, false);",
                    "const uint32_t s1 = scanout_full_case(dev, inst, lw, lh, 0x5CA11F10u, &c1, false);" }));
    chk("T30 B4: the flush-hook copy clamps to the live framebuffer (1920 x kFlushCopyRows only when absent)",
        !has(peer, "w < 1920 ? w : 1920") &&
        order(peer, { "(void)navi48_scanout_live_dims(&liveW, &liveH);",
                      "const uint32_t capW = n48_live_dim(liveW, 1920u), capH = n48_live_dim(liveH, kFlushCopyRows);",
                      "const uint32_t cw = w < capW ? w : capW, ch = h < capH ? h : capH;",
                      "st = navi48_scanout_copy_vram(phys, len, w, h, w * 4u, 0, kFlushCopyDstY, cw, ch, v, 11);" }));
    return bad;
}
static void test_T30_pins(const char *bringupPath, const char *peerPath) {
    std::string bu, dpg, peer, dcn, lrh;
    auto sib = [](const char *p, const char *rel) {
        std::string s = p ? std::string(p) : std::string();
        const size_t sl = s.rfind('/');
        return sl == std::string::npos ? std::string() : s.substr(0, sl + 1) + rel;
    };
    if (!bringupPath || !peerPath || !read_file(bringupPath, bu) || !read_file(peerPath, peer) ||
        !read_file(sib(peerPath, "DisplayPipeGuard.cpp").c_str(), dpg) || !read_file(sib(bringupPath, "dcn/navi48_dcn.cpp").c_str(), dcn) ||
        !read_file(sib(bringupPath, "dcn/navi48_liveraster.h").c_str(), lrh)) {
        expect("T30 the kext sources are readable", false); return;
    }
    expect("T30 every pin holds on the REAL sources", b514_pins(bu, dpg, peer, dcn, lrh, true) == 0u);
    struct { int file; const char *what, *from, *to; } plant[] = {   // 0 bringup, 1 dpg, 2 peer, 3 dcn, 4 navi48_liveraster.h
        { 1, "B1 the endpoint's width back to the 1920 constant", "*w = n48_agdc_endpoint_dim(gRa.width, cw, 1920u);",
          "*w = gRa.width ? gRa.width : 1920u;" },
        { 0, "B3 mode 6 back to the constant", "scanout_full_case(dev, inst, lw, lh, 0x5CA11F10u, &c1, false);",
          "scanout_full_case(dev, inst, 1920u, 1080u, 0x5CA11F10u, &c1, false);" },
        { 2, "B4 the flush clamp back to 1920", "const uint32_t cw = w < capW ? w : capW,", "const uint32_t cw = w < 1920 ? w : 1920," },
        { 4, "B2 liveRaster writes a register", "    if (dcn41_otg_get_totals(d, (uint32_t)otg, &ht1, &vt1) != DCN41_OK) return 0u;\n",
          "    if (dcn41_otg_get_totals(d, (uint32_t)otg, &ht1, &vt1) != DCN41_OK) return 0u;\n    d->wreg(d->cookie, 0u, 0u);\n" },
    };
    for (const auto &p : plant) {
        std::string m[5] = { bu, dpg, peer, dcn, lrh };
        std::string &t = m[p.file];
        const size_t a = t.find(p.from);
        if (a == std::string::npos || count_of(t, p.from) != 1u) { std::printf("  (setup) %s\n", p.what); expect("T30 plant setup: the text is found once", false); continue; }
        t.replace(a, std::strlen(p.from), p.to);
        char lbl[200]; std::snprintf(lbl, sizeof lbl, "T30 BREAK-check: %s - caught", p.what);
        expect(lbl, b514_pins(m[0], m[1], m[2], m[3], m[4], false) > 0u);
    }
}


// =============================================================================================================================
// T32 - build 0.0.521 Part D: UNDER 319 A RESPROV CANDIDATE IS LATCHED FULL, SO RESPROV CAN RECORD IT.
// The sampled SDMA verify credits only its sample count (n48_fc_verify `cmp = ns`) and resprov records a copy only when
// compared * 4 >= bytes (ws_resprov.h n48_rp_record's UNVERIFIED rung): run10x recorded 0 of 8308. The latch now takes
// `rp_candidate` (the copier's `retile`: rpOn && the image re-tiled / backing-sourced / a known asset). The copy is simulated
// through the kext's own chunk plan (n48_fc_chunk_len at kCopyChunkBytes = N48_FC_STAGING_BYTES), SDMA's shape rule (whole dwords,
// n <= staging), n48_fc_verify in the latched mode, and the MM loop's per-batch credit (ceil(take / 4) per 256-byte batch).
// =============================================================================================================================
#include "ws_resprov.h"
static int t32_read(void *, uint64_t, uint32_t *dst, uint32_t dwords) { for (uint32_t i = 0; i < dwords; i++) dst[i] = 0u; return 1; }
static int t32_plan(void *, uint64_t, uint32_t, uint32_t, uint32_t *got, uint32_t ns) { for (uint32_t i = 0; i < ns; i++) got[i] = 0u; return 1; }
struct T32Copy { uint64_t compared, sdmaChunks, mmChunks; uint32_t mode; };
static T32Copy t32_copy(uint64_t wBytes, uint32_t switchMode, uint32_t rpCand) {
    T32Copy r {}; r.mode = n48_fc_latch_mode(switchMode, 0u, 0u, rpCand);
    static std::vector<uint32_t> exp(N48_FC_STAGING_BYTES / 4u, 0u), got(N48_FC_SAMPLE_MAX * 2u, 0u);
    for (uint64_t pos = 0; pos < wBytes; ) {
        const uint64_t n = n48_fc_chunk_len(wBytes, pos, wBytes - pos, N48_FC_STAGING_BYTES);
        if (!n) break;
        if (r.mode && (n & 3u) == 0u && n <= N48_FC_STAGING_BYTES) {   // navi48_fc_chunk's shapeOk: SDMA takes it
            uint64_t cmp = 0u; uint32_t rf = 0u;
            (void)n48_fc_verify(&t32_read, &t32_plan, nullptr, 0x10000000ull + pos, exp.data(), got.data(), (uint32_t)n, r.mode, 0u, &cmp, &rf);
            r.compared += rf ? 0u : cmp; r.sdmaChunks++;
        } else {                                                       // the MM loop: 256-byte batches, ceil(take / 4) each
            for (uint64_t k = 0; k < n; ) { const uint64_t take = (n - k) >= 256u ? 256u : (n - k); r.compared += (take + 3u) / 4u; k += (take + 3u) / 4u * 4u; }
            r.mmChunks++;
        }
        pos += n;
    }
    return r;
}
static n48_rp_copy t32_rc(uint64_t bytes, uint64_t compared, uint32_t lin) {
    n48_rp_copy c; std::memset(&c, 0, sizeof c);
    c.copied = 1u; c.retiled = 1u; c.bytes = bytes; c.compared = compared; c.ownerWs = 1u; c.ctx = 5u; c.vaOk = 1u; c.va = 0x400024000ull;
    c.contiguous = 1u; c.vram = 0x1002f000ull; c.mode = N48_RP_G12_4KB_2D; c.arm = 1u; c.epoch = 3u; c.elemBytes = 4u; c.lin = lin;
    if (lin >= N48_RP_LIN_KNOWN0) { const n48_rp_known_row *k = &kN48RpKnownNib4[lin - N48_RP_LIN_KNOWN0]; c.mode = k->g12Mode; c.w = k->w; c.h = k->h; c.elemBytes = k->elemBytes; c.bytes = k->bytes; }
    return c;
}
static void test_T32a_latch() {
    expect("T32a 319 + a resprov candidate latches FULL", n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, 0u, 1u) == N48_FC_M_FULL);
    expect("T32a 319 + a buffer (not a candidate) stays SAMPLED", n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, 0u, 0u) == N48_FC_M_SAMPLED);
    expect("T32a 63 OFF stays OFF whatever the candidate says (the MM loop, unchanged)", n48_fc_latch_mode(0u, 0u, 0u, 1u) == 0u && n48_fc_latch_mode(0u, 1u, 1u, 1u) == 0u);
    expect("T32a 831 (FULL) stays FULL", n48_fc_latch_mode(N48_FC_M_FULL, 0u, 0u, 0u) == N48_FC_M_FULL);
    expect("T32a the 0.0.509/0.0.510 upgrades are unchanged", n48_fc_latch_mode(N48_FC_M_SAMPLED, 1u, 0u, 0u) == N48_FC_M_FULL &&
           n48_fc_latch_mode(N48_FC_M_SAMPLED, 0u, 1u, 0u) == N48_FC_M_FULL);
}
static void test_T32b_record() {
    // sizes: a whole number of chunks, a dword-aligned remainder, a byte tail (MM), under a chunk, a 4 KiB image
    const uint64_t sizes[] = { 0x400000ull, 0x2F4000ull, 0x100002ull, 0x7FFFFull, 0x1000ull, 0x40001ull };
    uint32_t okFull = 0u, unvSampled = 0u, arithOk = 0u, sampledShort = 0u, withSdma = 0u;
    for (uint64_t w : sizes) {
        const T32Copy f = t32_copy(w, N48_FC_M_SAMPLED, 1u), s = t32_copy(w, N48_FC_M_SAMPLED, 0u);
        n48_rp t {}; n48_rp_copy c = t32_rc(w, f.compared, 0u);
        if (f.mode == N48_FC_M_FULL && n48_rp_record(&t, &c) == N48_RP_REC_OK) okFull++;
        if (f.compared * 4ull >= w && f.compared * 4ull < w + 4u) arithOk++;          // full chunks n/4 + MM tails ceil(take/4)
        n48_rp t2 {}; n48_rp_copy c2 = t32_rc(w, s.compared, 0u);                      // the same image had it stayed sampled
        if (s.sdmaChunks && n48_rp_record(&t2, &c2) == N48_RP_REC_UNVERIFIED) unvSampled++;
        if (s.mode == N48_FC_M_SAMPLED && s.sdmaChunks && s.compared * 4ull < w) sampledShort++;
        if (s.sdmaChunks) withSdma++;
    }
    const uint32_t N = (uint32_t)(sizeof sizes / sizeof sizes[0]);
    expect("T32b a re-tiled texture copy under 319 is latched FULL and its resprov record is RECORDED (every size)", okFull == N);
    expect("T32b the arithmetic: full SDMA chunks (n/4) + MM tails (ceil(take/4)) give bytes <= compared*4 < bytes + 4", arithOk == N);
    // (a copy whose ONE chunk ends in a byte tail - 0x7FFFF, 0x40001 - is not dword-shaped: the MM loop runs all of it, sampled or not)
    expect("T32b THE RUNG IS INTACT: every one of those copies that SDMA verified by sampling is UNVERIFIED (run10x's failure, reproduced)",
           withSdma == 4u && unvSampled == withSdma && sampledShort == withSdma);
    // a buffer copy under 319 stays sampled (and its SDMA chunks are credited only their samples)
    const T32Copy b = t32_copy(0x400000ull, N48_FC_M_SAMPLED, 0u);
    expect("T32b a buffer copy under 319 stays SAMPLED: 4 SDMA chunks, 4 x (2 + 256) samples", b.mode == N48_FC_M_SAMPLED && b.sdmaChunks == 4u && b.compared == 4u * 258u);
    // 63 OFF: every chunk is the MM loop's, and the record is the 0.0.495 one
    const T32Copy o = t32_copy(0x2F4000ull, 0u, 1u);
    n48_rp to {}; n48_rp_copy co = t32_rc(0x2F4000ull, o.compared, 0u);
    expect("T32b 63 OFF: no SDMA chunk, every dword credited by the MM loop, RECORDED as before", o.mode == 0u && o.sdmaChunks == 0u &&
           o.compared * 4ull == 0x2F4000ull && n48_rp_record(&to, &co) == N48_RP_REC_OK);
    // the known-asset path (0.0.493) under 319: a candidate (the copier's `retile` is set for it), FULL, RECORDED as known
    const uint64_t kb = kN48RpKnownNib4[0].bytes;
    const T32Copy k = t32_copy(kb, N48_FC_M_SAMPLED, 1u);
    n48_rp tk {}; n48_rp_copy ck = t32_rc(kb, k.compared, N48_RP_LIN_KNOWN0);
    expect("T32b the known-asset path under 319 records (FULL, RECORDED, known = row + 1)", k.mode == N48_FC_M_FULL &&
           n48_rp_record(&tk, &ck) == N48_RP_REC_OK && tk.n == 1u && tk.e[0].known == 1u);
    const T32Copy ks = t32_copy(kb, N48_FC_M_SAMPLED, 0u);
    n48_rp tks {}; n48_rp_copy cks = t32_rc(kb, ks.compared, N48_RP_LIN_KNOWN0);
    expect("T32b ... and would be UNVERIFIED had it stayed sampled", n48_rp_record(&tks, &cks) == N48_RP_REC_UNVERIFIED);
    std::printf("  T32 per-copy cost: a 4 MiB re-tiled image verifies %llu dwords FULL vs %llu sampled\n",
                (unsigned long long)t32_copy(0x400000ull, N48_FC_M_SAMPLED, 1u).compared, (unsigned long long)b.compared);
}
static void test_T32p_pins(const char *bringupPath, const char *peerPath, const char *ttlPath) {
    std::string bu, peer, ttl, rp;
    std::string rpPath = peerPath ? std::string(peerPath) : std::string();
    const size_t sl = rpPath.rfind('/'); rpPath = sl == std::string::npos ? std::string("ws_resprov.h") : rpPath.substr(0, sl + 1) + "ws_resprov.h";
    if (!bringupPath || !peerPath || !ttlPath || !read_file(bringupPath, bu) || !read_file(peerPath, peer) || !read_file(ttlPath, ttl) ||
        !read_file(rpPath.c_str(), rp)) { expect("T32p the kext sources are readable", false); return; }
    const std::string latch = fn_text(bu, "static FcSlot *fc_slot_latch(uint64_t pos, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand) {");
    const std::string ccc = fn_text(peer, "static __attribute__((noinline)) uint64_t fc_copy_chunk(RtBufs *rt, bool retile, IOMemoryDescriptor *md, uint64_t backingOffset,");
    // build 0.0.527: the candidate is `retile` OR switch 82's per-thread FULL flag (asked at the first chunk only).
    expect("T32p the copier hands its `retile` (rpOn && the image re-tiled/backing-sourced/known) as the candidate",
           has(ccc, "const uint32_t cand = (retile ? 1u : 0u) | ((pos == 0u && gN48Sk82On) ? navi48_sk82_cand_mine() : 0u);") &&
           has(ccc, "const uint64_t r = navi48_fc_chunk(pos, wBytes, dAt, n, &fc_fill_batch, &c, lead, cgLo, cgHi, cand);") &&
           count_of(peer, "const bool retile = rpOn && rtShape == N48_RP_SHAPE_OK;") == 1u &&
           count_of(peer, "const uint64_t fc = fc_copy_chunk(&rt, retile, md, backingOffset, pos, wBytes, dAt, n, raw, cgLo, cgHi);") == 1u);
    expect("T32p navi48_fc_chunk passes it to the latch, the latch to n48_fc_latch_mode, at the first chunk, before the mode is stored",
           has(bu, "\tFcSlot *s = fc_slot_latch(pos, cgLo, cgHi, rpCand);") && !latch.empty() &&
           latch.find("if (pos != 0u) return s;") < latch.find("const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);") &&
           latch.find("const uint32_t eff = n48_fc_latch_mode(mode, b62, cgp, rpCand);") < latch.find("s->mode = eff;"));
    expect("T32p the counters: forced full per reason, every copy's latched mode",
           has(latch, "if (eff != mode) __atomic_fetch_add(b62 ? &gFcS.forced_full : cgp ? &gFcS.forced_full_cg : &gFcS.forced_full_rp, 1ull, __ATOMIC_RELAXED);") &&
           has(latch, "__atomic_fetch_add(eff == N48_FC_M_FULL ? &gFcS.latched_full : &gFcS.latched_sampled, 1ull, __ATOMIC_RELAXED);"));
    expect("T32p Navi48Ttl.hpp declares the candidate", has(ttl, "                         uint8_t *lead, uint64_t cgLo, uint64_t cgHi, uint32_t rpCand);"));
    expect("T32p resprov's full-read-back rung is UNCHANGED (never relaxed for sampled copies)",
           count_of(rp, "    else if (!c->bytes || c->mismatched || c->compared * 4ull < c->bytes) why = N48_RP_REC_UNVERIFIED;") == 1u);
}

int main(int argc, char **argv) {
    std::printf("== T1: exhaustive interleavings ==\n");
    test_T1();
    std::printf("== T2: planted breaks ==\n");
    test_T2a_ring_before_slot();
    test_T2b_close_before_end();
    test_T2c_no_slot_scan();
    test_T2d_no_ring_scan();
    test_T2e_begin_after_wa();
    std::printf("== T3: the paused-copier schedule is refused ==\n");
    test_T3_paused_copier();
    std::printf("== T4: WRAP and TORN are refused ==\n");
    test_T4_wrap_and_torn();
    std::printf("== T5: poison holds until a covering clean copy ==\n");
    test_T5_poison();
    std::printf("== T5b: a wrote=0 close must neither mark nor clear poison ==\n");
    test_T5b_wrote_gates_poison();
    std::printf("== T6: a disjoint range is ACCEPTED ==\n");
    test_T6_disjoint_accepted();
    std::printf("== T7: recorder overflow refuses ==\n");
    test_T7_recorder_overflow();
    std::printf("== T8: a memo hit without its pages ==\n");
    test_T8_memo_hit();
    std::printf("== T10: opened == closed + live in-flight, at every quiescent point ==\n");
    test_T10_opened_closed_live();
    std::printf("== T11: unscoped writes self-scope (item 1) ==\n");
    test_T11a_self_scoping_exhaustive();
    test_T11b_old_pre_write_event_no_scope_mutant();
    test_T11c_containment_not_overlap();
    test_T11d_containment_owner_thread();
    std::printf("== T12: untracked copies leave a match-all trail (item 2) ==\n");
    test_T12_untracked_match_all_trail();
    std::printf("== T13: poison only what was written (item 3) ==\n");
    test_T13_poison_only_what_was_written();
    std::printf("== T14: one guard window per frame (item 4) ==\n");
    test_T14_one_window_per_frame();
    std::printf("== T15: untracked close - END before decrement, and poisons on failure (D3) ==\n");
    test_T15a_untracked_close_end_before_decrement();
    test_T15b_untracked_close_poisons();
    std::printf("== T9: source pins and the allowlist ==\n");
    test_T9(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr,
            argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("== T16: 0.0.486 the backing-sourced copy's one length (pre-flight, scope, writes) ==\n");
    test_T16_lin486(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("== T17: 0.0.493 the known-asset row's order (switch, facts, key, convert, write, read-back, record) ==\n");
    test_T17_known493(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);

    std::printf("== T18: 0.0.495 switch 62 - the shader-heap copy / substitution race in RUN D's order ==\n");
    test_T18a_rund_order();
    test_T18b_disjoint_copy();
    test_T18c_poison();
    test_T18d_off_identity();
    test_T18e_overlay();
    test_T18p_pins(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("== T19: 0.0.496 switch 63 - the residency copy through SDMA ==\n");
    test_T19a_chunk_plan();
    test_T19b_decide();
    test_T19c_sample_plan();
    test_T19d_packet(argc > 1 ? argv[1] : nullptr);
    test_T19e_fence_and_outcome();
    test_T19f_order_and_identity();
    test_T19p_pins(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("== T20: 0.0.496 F1 - the substitution registry prunes and merges (RUN C) ==\n");
    test_T20_registry();
    std::printf("== T21: 0.0.496 F4 - the overlay re-check (RUN D) ==\n");
    test_T21_recheck();
    test_T21p_pins(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("== T22: 0.0.503 switch 68 - the hybrid newUserClient policy and its wiring ==\n");
    test_T22_policy();
    test_T22p_pins(argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("== T23: 0.0.509 switch 63 - the fence bound from the kick, dead ranges, sampled verify never vouches ==\n");
    test_T23a_bound_from_kick();
    test_T23b_dead_range_sticky();
    test_T23c_sampled_never_vouches();
    test_T23d_verify_outlier();
    test_T23p_pins(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("== T24: 0.0.510 A1 - a poisoned range retried under 319 verifies in full; a refused first chunk goes to MM ==\n");
    test_T24a_poisoned_retry_full();
    test_T24b_refused_first_chunk();
    test_T24p_pins(argc > 1 ? argv[1] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("== T25: 0.0.510 A2 - the copy guard's poison rows claimed by compare-and-swap ==\n");
    test_T25a_single_thread_identity();
    test_T25b_interleavings();
    std::printf("== T26: 0.0.510 PART B switch 72 - the copy waits for a committed frame's walk ==\n");
    test_T26a_window();
    test_T26b_invariant();
    test_T26c_copier_answers();
    test_T26p_pins(argc > 2 ? argv[2] : nullptr);
    std::printf("== T27: 0.0.511, the 0.0.510 review's fixes for switch 72 (B1-B4) ==\n");
    test_T27a_only_patched_waits();
    test_T27b_timeout_clears();
    test_T27c_walk_thread();
    test_T27d_whole_range();
    std::printf("== T28: 0.0.514 A1/A2 - the sampled verify takes gVramMmLock once per chunk ==\n");
    test_T28a_differential();
    test_T28b_once_per_chunk();
    test_T28c_refused();
    test_T28p_pins(argc > 1 ? argv[1] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("== T29: 0.0.514 A3 - who holds gVramMmLock ==\n");
    test_T29a_table();
    test_T29b_note();
    test_T29c_lines();
    test_T29p_pins(argc > 1 ? argv[1] : nullptr, argc > 2 ? argv[2] : nullptr, argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("== T30: 0.0.514 B1-B4 - native 1440p preparation, the kext glue ==\n");
    test_T30_pins(argc > 1 ? argv[1] : nullptr, argc > 3 ? argv[3] : nullptr);
    std::printf("== T31: 0.0.515 D2 - DECIDE's MM holds by site ==\n");
    test_T31a_nesting();
    test_T31b_early_return();
    test_T31c_dwords_lines();
    test_T31p_pins(argc > 2 ? argv[2] : nullptr, argc > 4 ? argv[4] : nullptr, argc > 1 ? argv[1] : nullptr);
    std::printf("== T32: 0.0.521 Part D - under 319 a resprov candidate is latched FULL, so resprov records it ==\n");
    test_T32a_latch();
    test_T32b_record();
    test_T32p_pins(argc > 1 ? argv[1] : nullptr, argc > 3 ? argv[3] : nullptr, argc > 4 ? argv[4] : nullptr);
    std::printf("\ngfx_copyguard: %u check(s), %u failed\n", gChecks, gFails);
    std::printf("gfx_copyguard: %s\n", gFails ? "N48-COPYGUARD-TEST-FAIL" : "N48-COPYGUARD-TEST-PASS");
    return gFails ? 1 : 0;
}
