// gfx_cg84_test.cpp — build 0.0.529 (notes/design/CG84.md items 1-12, X1-X4; apple/gfx_copyguard.h + apple/gfx_cg84.h):
// switch 84, the granule copy guard and the delta residency write (SHADOW 340 / OFF 596 / ON 852).
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -I src/navi48-bringup/tests src/navi48-bringup/tests/gfx_cg84_test.cpp \
//         -o /tmp/cg84 && /tmp/cg84 src/navi48-bringup/src/Navi48Bringup.cpp src/navi48-bringup/src/apple/AppleHardwareHook.cpp \
//         src/navi48-bringup/src/apple/Navi48AccelPeer.cpp src/navi48-bringup/src/apple/gfx_copyguard.h \
//         src/navi48-bringup/src/apple/gfx_commit.h
// Covers:
//   C1  the selector (M1 340 SHADOW, M2 596 OFF, M3 852 ON, bare 84, any other M refused), the fine latch, the counted answer
//   C2  every new log line <= 491 bytes at every field's widest
//   C3  the recorder: granule and page boundaries, the page clip, OR on dedupe, a new index zeroes its mask, whole-page notes (the
//       memo hits' n48_cg_pagerec_note(page, 4096) and note_page) set all 256 bits, the census list (fine only) and its overflow
//   C4  OFF IDENTITY: `fine` 0 against a FROZEN 0.0.528 (tests/frozen/gfx_copyguard_f824012f.h) over 20000 random schedules
//   C5  the exhaustive OPEN/BEGIN/W/END/CLOSE interleaving with byte ranges, the writer's steps also run at the check's own step
//       points (N48_CG_CHECK_STEP): no hazard accepted; accepts are non-vacuous (a disjoint read during a delta is accepted, an
//       overlapping one refused; the page model refuses the disjoint one)
//   C6  the writer: eligibility, the diff's rounding, the delta loop's containment in its scope, the plan (SHADOW never acts, ON
//       establishes then deltas), every invalidation (open bump / own open skipped, page-out, unmap, commit targets, poison, state),
//       the update's conditions (mismatch, failed, programs, busy, a second copy)
//   C7  the D1 ORDER: a foreign writer's four steps (slot open, bump, write, close) at every position of our G0, copy and update;
//       never a VALID key whose shadow differs from VRAM; positive controls
//   C8  the capdec fixture (RUN AA run11k, RUN Z3 run11j; gen_fixture_cg84.py): page model 146/146 and 147/147 refused, granule
//       model 0/0 at K = 1 and 4/1 at K = 4; X1 on the same data: SHADOW's counted verdicts equal OFF's, its instrument equals ON's
//   C9  X1: SHADOW's copies - the plan never acts, so every byte a copy writes and every verdict is OFF's; the switch never ON at boot
//   C10 source pins (Navi48Bringup.cpp, AppleHardwareHook.cpp, Navi48AccelPeer.cpp, gfx_copyguard.h, gfx_commit.h) and the reader
//       census allowlist (X2)
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// C5: the check's step points run the scheduled writer steps (the kext compiles N48_CG_CHECK_STEP to nothing).
static void cg84_check_step(unsigned at);
#define N48_CG_CHECK_STEP(at) cg84_check_step(at)
#include "gfx_cg84.h"
namespace cg528 {
#include "frozen/gfx_copyguard_f824012f.h"
}
#include "fixture_cg84_capdec.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-110s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-110s %#llx\n", what, (unsigned long long)got);
}
static void expect(const char *what, bool c) { expect_u(what, c ? 1u : 0u, 1u); }

// ------------------------------------------------------------------------------------------------------------------ the world
struct Cg {
    n48_cg_slot slots[N48_CG_SLOTS];
    n48_cg_ring ring;
    n48_cg_poison poison;
    uint32_t untracked;
    Cg() { n48_cg_slot_init(slots, N48_CG_SLOTS); n48_cg_ring_init(&ring); n48_cg_poison_init(&poison); untracked = 0u; }
    // navi48_cg_open / navi48_cg_close, split into their steps (the kext's order: slot, then BEGIN; END, then seq even)
    int32_t open(uint64_t lo, uint64_t hi, uint64_t dlo = 0, uint64_t dhi = 0) {
        return n48_cg_slot_open_d(slots, N48_CG_SLOTS, lo, hi, 0x100u, dlo, dhi);
    }
    void begin(int32_t s, uint64_t lo, uint64_t hi, uint64_t dlo = 0, uint64_t dhi = 0) {
        n48_cg_ring_push_od(&ring, N48_CG_EV_BEGIN, (uint32_t)s, lo, hi, 0x100u, dlo, dhi);
    }
    void end(int32_t s, uint64_t lo, uint64_t hi, uint64_t dlo = 0, uint64_t dhi = 0) {
        n48_cg_ring_push_od(&ring, N48_CG_EV_END, (uint32_t)s, lo, hi, 0x100u, dlo, dhi);
    }
    void close(int32_t s) { n48_cg_slot_close(slots, s); }
    uint32_t check(const n48_cg_pagerec *r, uint32_t fine, uint32_t used, uint64_t since, n48_cg_why *w = nullptr) {
        return n48_cg_check_fx(r, fine, used, &poison, slots, N48_CG_SLOTS, &ring, since, untracked, w);
    }
};
static void rec_reset(n48_cg_pagerec *r, uint32_t fine) { std::memset(r, 0, sizeof *r); n48_cg_pagerec_reset(r); r->fine = fine; }

// ================================================================================================================ C1 selector
static void test_C1()
{
    expect_u("C1 M1 -> SHADOW", n48_cg84_mode_of_m(1u), N48_CG84_SHADOW);
    expect_u("C1 M2 -> OFF", n48_cg84_mode_of_m(2u), N48_CG84_OFF);
    expect_u("C1 M3 -> ON", n48_cg84_mode_of_m(3u), N48_CG84_ON);
    expect_u("C1 bare 84 reads", n48_cg84_mode_of_m(0u), N48_CG84_M_READ);
    for (uint32_t m = 4u; m < 300u; m++) if (n48_cg84_mode_of_m(m) != N48_CG84_M_BAD) { expect_u("C1 an unknown M is refused", m, 0u); break; }
    expect_u("C1 an unknown M (4, 255) is refused", (n48_cg84_mode_of_m(4u) == N48_CG84_M_BAD) + (n48_cg84_mode_of_m(255u) == N48_CG84_M_BAD), 2u);
    expect_u("C1 the verbs: 84 | 1 << 8 = 340", 84u | (1u << 8), 340u);
    expect_u("C1 the verbs: 84 | 2 << 8 = 596", 84u | (2u << 8), 596u);
    expect_u("C1 the verbs: 84 | 3 << 8 = 852", 84u | (3u << 8), 852u);
    expect_u("C1 842 is 74 | 3 << 8 (no clash with 84)", 74u | (3u << 8), 842u);
    expect_u("C1 fine latch: SHADOW is NOT fine (X1: SHADOW changes no verdict)", n48_cg84_fine_of(N48_CG84_SHADOW), 0u);
    expect_u("C1 fine latch: OFF is not fine", n48_cg84_fine_of(N48_CG84_OFF), 0u);
    expect_u("C1 fine latch: ON is fine", n48_cg84_fine_of(N48_CG84_ON), 1u);
    expect_u("C1 counted: SHADOW counts the page answer", n48_cg84_counted(N48_CG84_SHADOW, N48_CG_EVENT, N48_CG_OK), N48_CG_EVENT);
    expect_u("C1 counted: OFF counts the page answer", n48_cg84_counted(N48_CG84_OFF, N48_CG_EVENT, N48_CG_OK), N48_CG_EVENT);
    expect_u("C1 counted: ON counts the fine answer", n48_cg84_counted(N48_CG84_ON, N48_CG_EVENT, N48_CG_OK), N48_CG_OK);
    n48_cg84_stats s {};
    n48_cg84_note(&s, N48_CG_EVENT, N48_CG_OK); n48_cg84_note(&s, N48_CG_EVENT, N48_CG_EVENT); n48_cg84_note(&s, N48_CG_OK, N48_CG_OK);
    expect_u("C1 counters: fine 3, admitted-by-granule 1, refused 1", s.fine * 100 + s.admitted * 10 + s.refused, 311u);
}

// ================================================================================================================ C2 lines
static size_t widest(const char *fmt, const char *s0, const char *s1, const char *s2)
{
    // every %llu at UINT64_MAX, %u at UINT32_MAX, %p at 0xffffffffffffffff, %#llx at the same, %s the longest given
    std::string out;
    const char *ss[3] = { s0, s1, s2 }; uint32_t si = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { out += *p; continue; }
        std::string spec = "%";
        p++;
        while (*p && !std::strchr("dusxp", *p)) spec += *p++;
        spec += *p;
        char buf[64];
        if (*p == 's') { out += ss[si < 3 ? si : 2] ? ss[si < 3 ? si : 2] : ""; si++; continue; }
        if (*p == 'p') { out += "0xffffffffffffffff"; continue; }
        if (spec.find("ll") != std::string::npos) std::snprintf(buf, sizeof buf, spec.c_str(), (unsigned long long)~0ull);
        else std::snprintf(buf, sizeof buf, spec.c_str(), ~0u);
        out += buf;
    }
    return out.size();
}
static void test_C2()
{
    const size_t pre = std::strlen("Navi48AccelPeer: ");
    expect("C2 cg84: line <= 491 B at its widest", widest(N48_CG84_FMT, "", "", "") + pre <= 491u);
    expect("C2 d84: line 1 <= 491 B at its widest", widest(N48_D84_FMT, "", "", "") + pre <= 491u);
    expect("C2 d84: line 2 <= 491 B at its widest",
           widest(N48_D84_FMT2, "SHADOW(340) sampled-trust keys", " - REFUSED: a continuous arm stands, unchanged", "not allocated") + pre <= 491u);
    expect("C2 DELTA line <= 491 B at its widest", widest(N48_D84_DELTA_FMT, "", "", "") + pre <= 491u);
    expect("C2 d84: line 3 (fix pass) <= 491 B at its widest", widest(N48_D84_FMT3, "", "", "") + pre <= 491u);
    expect("C2 MF-6 SHADOW-VRAM MISMATCH line <= 491 B at its widest", widest(N48_D84_PROBE_FMT, "", "", "") + std::strlen("Navi48Bringup: ") <= 491u);
    std::printf("      widths: d84-3 %zu probe %zu\n", widest(N48_D84_FMT3, "", "", "") + pre, widest(N48_D84_PROBE_FMT, "", "", "") + pre);
    std::printf("      widths: cg84 %zu d84 %zu d84-2 %zu DELTA %zu\n", widest(N48_CG84_FMT, "", "", "") + pre,
                widest(N48_D84_FMT, "", "", "") + pre,
                widest(N48_D84_FMT2, "SHADOW(340) sampled-trust keys", " - REFUSED: a continuous arm stands, unchanged", "not allocated") + pre,
                widest(N48_D84_DELTA_FMT, "", "", "") + pre);
}

// ================================================================================================================ C3 recorder
static uint32_t popcount_page(const n48_cg_pagerec *r, uint32_t i)
{
    uint32_t c = 0;
    for (uint32_t w = 0; w < 4; w++) c += (uint32_t)__builtin_popcountll(r->gm[i][w]);
    return c;
}
static void test_C3()
{
    const uint64_t P = 0x10010000ull;
    n48_cg_pagerec r; rec_reset(&r, 1u);
    n48_cg_pagerec_note(&r, P + 0x10, 16u);
    expect_u("C3 one granule noted: exactly one bit", popcount_page(&r, 0), 1u);
    expect_u("C3 granule 1 is the bit (a >>5 shift would set bit 0)", (r.gm[0][0] >> 1) & 1u, 1u);
    expect_u("C3 hit [P+0x10,P+0x20)", n48_cg_rec_hit(&r, 0, P + 0x10, P + 0x20), 1u);
    expect_u("C3 no hit [P+0x20,P+0x30) (the next granule)", n48_cg_rec_hit(&r, 0, P + 0x20, P + 0x30), 0u);
    expect_u("C3 no hit [P,P+0x10) (the previous granule)", n48_cg_rec_hit(&r, 0, P, P + 0x10), 0u);
    expect_u("C3 hit [P+0x1f,P+0x21) (a range straddling granules 1-2)", n48_cg_rec_hit(&r, 0, P + 0x1f, P + 0x21), 1u);
    expect_u("C3 hit [P+0xf,P+0x11) (a range straddling granules 0-1)", n48_cg_rec_hit(&r, 0, P + 0xf, P + 0x11), 1u);
    expect_u("C3 page model: [P+0x800,P+0x810) hits the page", n48_cg_rec_hit_f(&r, 0, P + 0x800, P + 0x810, 0u), 1u);
    expect_u("C3 fine model: [P+0x800,P+0x810) does not", n48_cg_rec_hit_f(&r, 0, P + 0x800, P + 0x810, 1u), 0u);
    // byte-level reads inside a granule
    n48_cg_pagerec_note(&r, P + 0x42, 4u);   // granule 4
    expect_u("C3 a 4-byte read at +0x42 notes granule 4 only (2 bits total)", popcount_page(&r, 0) * 100 + ((r.gm[0][0] >> 4) & 1u), 201u);
    expect_u("C3 a read ending at +0x40 exactly does not touch granule 4", n48_cg_rec_hit(&r, 0, P + 0x30, P + 0x40), 0u);
    // the page clip: a range starting below the page, a range ending past it
    n48_cg_pagerec a; rec_reset(&a, 1u);
    n48_cg_pagerec_note(&a, P + 0xff8, 16u);   // granule 255 of P, granule 0 of P + 4096
    expect_u("C3 a read across the page boundary takes two pages", a.n, 2u);
    expect_u("C3 page P: granule 255 only", popcount_page(&a, 0) * 1000 + ((a.gm[0][3] >> 63) & 1u), 1001u);
    expect_u("C3 page P+4096: granule 0 only", popcount_page(&a, 1) * 1000 + (a.gm[1][0] & 1u), 1001u);
    expect_u("C3 clip: [P-0x100,P+0x10) hits P+4096? no; P's granule 0? not set", n48_cg_rec_hit(&a, 0, P - 0x100, P + 0x10), 0u);
    expect_u("C3 clip: [P+4096-0x100,P+4096+0x10) hits page P+4096 granule 0", n48_cg_rec_hit(&a, 1, P + 4096 - 0x100, P + 4096 + 0x10), 1u);
    expect_u("C3 clip: [P+0xff0,P+0x2000) hits page P granule 255", n48_cg_rec_hit(&a, 0, P + 0xff0, P + 0x2000), 1u);
    expect_u("C3 clip: [P+0x1010,P+0x3000) does not hit page P+4096 (granule 0 only)", n48_cg_rec_hit(&a, 1, P + 0x1010, P + 0x3000), 0u);
    expect_u("C3 clip: a range entirely below the page never hits", n48_cg_rec_hit(&a, 1, P, P + 0x800), 0u);
    // OR on dedupe
    n48_cg_pagerec d; rec_reset(&d, 1u);
    n48_cg_pagerec_note(&d, P, 16u);
    n48_cg_pagerec_note(&d, P + 0x100, 16u);
    expect_u("C3 dedupe: one page", d.n, 1u);
    expect_u("C3 dedupe ORs: granule 0 survives the second note", n48_cg_rec_hit(&d, 0, P, P + 16), 1u);
    expect_u("C3 dedupe ORs: granule 16 set", n48_cg_rec_hit(&d, 0, P + 0x100, P + 0x110), 1u);
    // whole-page notes
    n48_cg_pagerec f; rec_reset(&f, 1u);
    n48_cg_pagerec_note(&f, P, 4096u);                 // the memo hits' and the T+M copy's call shape
    n48_cg_pagerec_note_page(&f, P + 0x3000);
    expect_u("C3 a memo-hit note (page, 4096) sets all 256 bits", popcount_page(&f, 0), 256u);
    expect_u("C3 note_page sets all 256 bits", popcount_page(&f, 1), 256u);
    expect_u("C3 whole-page notes are counted (nfull 2)", f.nfull, 2u);
    expect_u("C3 a whole-page note is hit by any byte of the page", n48_cg_rec_hit(&f, 1, P + 0x3ff0, P + 0x3ff1), 1u);
    // a new index zeroes its mask (a switch-78 rebase drops pages; the index is taken again)
    n48_cg_pagerec z; rec_reset(&z, 1u);
    n48_cg_pagerec_note(&z, P, 4096u);
    Cg zc;
    (void)n48_cg_redo_rebase(&z, &zc.ring, 0u);
    n48_cg_pagerec_note(&z, P + 0x5000 + 0x20, 16u);
    expect_u("C3 a new index starts with no granule (the old page's 256 bits are gone)", popcount_page(&z, 0), 1u);
    // the census list: fine only
    Cg cg;
    n48_cg_pagerec c; rec_reset(&c, 0u);
    n48_cg_pagerec_note_census(&c, P + 0x7123);
    const uint64_t since = n48_cg_ring_mark(&cg.ring);
    const int32_t s = cg.open(P + 0x7000, P + 0x7010); cg.begin(s, P + 0x7000, P + 0x7010); cg.end(s, P + 0x7000, P + 0x7010); cg.close(s);
    expect_u("C3 census page: the page check never reads it (OK)", cg.check(&c, 0u, 0u, since), N48_CG_OK);
    expect_u("C3 census page: the fine check reads it as a whole page (EVENT)", cg.check(&c, 1u, 0u, since), N48_CG_EVENT);
    for (uint32_t i = 0; i < N48_CG_XPAGES + 1u; i++) n48_cg_pagerec_note_census(&c, P + 0x100000ull + 0x1000ull * i);
    expect_u("C3 census overflow: xover set", c.xover, 1u);
    expect_u("C3 census overflow: the page check is unaffected (EVENT stays out)", cg.check(&c, 0u, 0u, n48_cg_ring_mark(&cg.ring)), N48_CG_OK);
    expect_u("C3 census overflow: the fine check refuses OVERFLOW", cg.check(&c, 1u, 0u, n48_cg_ring_mark(&cg.ring)), N48_CG_OVERFLOW);
}

// ================================================================================================================ C4 OFF identity
static uint64_t gRng = 0x9e3779b97f4a7c15ull;
static uint64_t rnd() { gRng ^= gRng << 13; gRng ^= gRng >> 7; gRng ^= gRng << 17; return gRng; }
static void test_C4()
{
    uint32_t same = 0, total = 0, refusedNew = 0, acceptedNew = 0;
    static const uint32_t reasons[] = { N48_CG_OK, N48_CG_POISONED, N48_CG_IN_FLIGHT, N48_CG_EVENT, N48_CG_WRAP, N48_CG_OVERFLOW, N48_CG_UNTRACKED };
    uint32_t seen[8] = {};
    for (uint32_t it = 0; it < 20000u; it++) {
        const uint64_t base = 0x10000000ull + ((rnd() % 64u) << 12);
        n48_cg_pagerec *rn = new n48_cg_pagerec; rec_reset(rn, 0u);
        cg528::n48_cg_pagerec *ro = new cg528::n48_cg_pagerec; std::memset(ro, 0, sizeof *ro); cg528::n48_cg_pagerec_reset(ro);
        Cg cn;
        cg528::n48_cg_slot so[N48_CG_SLOTS]; cg528::n48_cg_slot_init(so, N48_CG_SLOTS);
        cg528::n48_cg_ring *go = new cg528::n48_cg_ring; cg528::n48_cg_ring_init(go);
        cg528::n48_cg_poison po; cg528::n48_cg_poison_init(&po);
        // pre-pass events
        const uint32_t pre = (uint32_t)(rnd() % 4u);
        for (uint32_t k = 0; k < pre; k++) {
            const uint64_t lo = base + (rnd() % 0x8000u), hi = lo + 16u + (rnd() % 0x3000u);
            n48_cg_ring_push(&cn.ring, N48_CG_EV_BEGIN, 0u, lo, hi); cg528::n48_cg_ring_push(go, N48_CG_EV_BEGIN, 0u, lo, hi);
        }
        const uint64_t since = n48_cg_ring_mark(&cn.ring);
        const uint32_t perPage = (rnd() % 4u) == 0u ? 1u : 0u;
        rn->cur_since = since; ro->cur_since = since;
        const uint32_t nreads = (uint32_t)(rnd() % 12u);
        for (uint32_t k = 0; k < nreads; k++) {
            const uint64_t off = base + (rnd() % 0x9000u);
            const uint32_t bytes = (rnd() % 3u) == 0u ? 4096u : (uint32_t)(4u + (rnd() % 300u));
            n48_cg_pagerec_note(rn, off, bytes); cg528::n48_cg_pagerec_note(ro, off, bytes);
            if (perPage && (rnd() % 3u) == 0u) {   // a switch-78 style re-base mid-pass (both, the same)
                n48_cg_ring_push(&cn.ring, N48_CG_EV_WRITE, 0u, 0x1ull, 0x2ull); cg528::n48_cg_ring_push(go, N48_CG_EV_WRITE, 0u, 0x1ull, 0x2ull);
                (void)n48_cg_redo_rebase(rn, &cn.ring, rn->n); (void)cg528::n48_cg_redo_rebase(ro, go, ro->n);
            }
        }
        if ((rnd() % 50u) == 0u) { for (uint32_t k = 0; k < N48_CG_REC_PAGES + 2u; k++) { n48_cg_pagerec_note(rn, 0x20000000ull + 4096ull * k, 4u); cg528::n48_cg_pagerec_note(ro, 0x20000000ull + 4096ull * k, 4u); } }
        n48_cg_pagerec_note_census(rn, base + 0x2000u);   // never read by the page check
        // slots
        const uint32_t nsl = (uint32_t)(rnd() % 3u);
        for (uint32_t k = 0; k < nsl; k++) {
            const uint64_t lo = base + (rnd() % 0x9000u), hi = lo + 16u + (rnd() % 0x2000u);
            const int32_t a = n48_cg_slot_open_d(cn.slots, N48_CG_SLOTS, lo, hi, 7u, lo + 16u, lo + 32u);
            const int32_t b = cg528::n48_cg_slot_open(so, N48_CG_SLOTS, lo, hi, 7u);
            if ((rnd() % 2u) == 0u) { n48_cg_slot_close(cn.slots, a); cg528::n48_cg_slot_close(so, b); }
        }
        if ((rnd() % 40u) == 0u) { cn.slots[3].seq = 5u; so[3].seq = 5u; }   // a slot seen torn is not modelled: both read it stable
        // events in the window
        const uint32_t nev = (uint32_t)(rnd() % 5u);
        for (uint32_t k = 0; k < nev; k++) {
            const uint64_t lo = base + (rnd() % 0x9000u), hi = lo + 16u + (rnd() % 0x2000u);
            const uint32_t kind = 1u + (uint32_t)(rnd() % 3u);
            n48_cg_ring_push_od(&cn.ring, kind, 0u, lo, hi, 0u, lo, lo + 16u); cg528::n48_cg_ring_push(go, kind, 0u, lo, hi);
        }
        if ((rnd() % 60u) == 0u) for (uint32_t k = 0; k < N48_CG_RING + 1u; k++) { n48_cg_ring_push(&cn.ring, 1u, 0u, 1u, 2u); cg528::n48_cg_ring_push(go, 1u, 0u, 1u, 2u); }
        if ((rnd() % 10u) == 0u) { const uint64_t lo = base + (rnd() % 0x9000u); n48_cg_poison_mark(&cn.poison, lo, lo + 64u); cg528::n48_cg_poison_mark(&po, lo, lo + 64u); }
        const uint32_t untr = (rnd() % 30u) == 0u ? 1u : 0u;
        cn.untracked = untr;
        n48_cg_why wn {}; cg528::n48_cg_why wo {};
        const uint32_t a = n48_cg_check_ex(rn, &cn.poison, cn.slots, N48_CG_SLOTS, &cn.ring, since, untr, &wn);
        const uint32_t b = cg528::n48_cg_check_ex(ro, &po, so, N48_CG_SLOTS, go, since, untr, &wo);
        total++;
        if (a == b && wn.idx == wo.idx && wn.page == wo.page && wn.has == wo.has) same++;
        else if (total - same <= 3u) std::printf("      C4 differ it %u: new %u (idx %u page %#llx) old %u (idx %u page %#llx)\n", it, a, wn.idx,
                                                (unsigned long long)wn.page, b, wo.idx, (unsigned long long)wo.page);
        if (a == N48_CG_OK) acceptedNew++; else refusedNew++;
        for (uint32_t q = 0; q < 7; q++) if (reasons[q] == a) seen[q] = 1u;
        delete rn; delete ro; delete go;
    }
    expect_u("C4 OFF IDENTITY: `fine` 0 = frozen 0.0.528, verdict + page for page, 20000 random schedules", same, total);
    expect("C4 non-vacuous: both accepts and refusals occurred", acceptedNew > 100u && refusedNew > 100u);
    uint32_t nseen = 0; for (uint32_t q = 0; q < 7; q++) nseen += seen[q];
    expect_u("C4 non-vacuous: all 7 answers (OK POISONED IN_FLIGHT EVENT WRAP OVERFLOW UNTRACKED) occurred", nseen, 7u);
    std::printf("      C4 accepted %u refused %u of %u\n", acceptedNew, refusedNew, total);
    expect_u("C4 the recorder grew: sizeof(n48_cg_pagerec) 6,256 B (0.0.528: 2,072; +4,096 masks, +64 census, +24 fields)", sizeof(n48_cg_pagerec), 6256u);
    expect_u("C4 the frozen recorder is 0.0.528's size (2,072 B)", sizeof(cg528::n48_cg_pagerec), 2072u);
}

// ================================================================================================================ C5 interleaving
// The reader: MARK (since), READ (note the read range), then the check; the writer: OPEN (slot), BEGIN, WRITE, END, CLOSE. The
// writer's five steps are placed in the 7 gaps (before MARK, MARK..READ, READ..check, check step 1, step 2, step 3, after) in
// order: C(11, 5) = 462 schedules per range pair. A HAZARD: the writer's range overlaps the read AND its writes ([BEGIN, END])
// meet [MARK, the ring-position load] (gaps 1..4) - every hazard must be refused (a copy whose BEGIN/END falls inside the pass
// window, or whose slot is open at the scan).
static Cg *gC5 = nullptr;
static int gC5Place[5];
static int gC5Done[5];
static uint64_t gC5Lo, gC5Hi, gC5Dlo, gC5Dhi;
static int32_t gC5Slot;
static void c5_run_gap(int gap)
{
    for (int k = 0; k < 5; k++) {
        if (gC5Done[k] || gC5Place[k] != gap) continue;
        switch (k) {
        case 0: gC5Slot = gC5->open(gC5Lo, gC5Hi, gC5Dlo, gC5Dhi); break;
        case 1: gC5->begin(gC5Slot, gC5Lo, gC5Hi, gC5Dlo, gC5Dhi); break;
        case 2: break;   // the write itself: the guard sees only the announced ranges
        case 3: gC5->end(gC5Slot, gC5Lo, gC5Hi, gC5Dlo, gC5Dhi); break;
        case 4: gC5->close(gC5Slot); break;
        }
        gC5Done[k] = 1;
    }
}
static void cg84_check_step(unsigned at)
{
    if (!gC5) return;
    if (at == 1u) c5_run_gap(3);        // after the fence and OVERFLOW/UNTRACKED, before poison + the slot scan
    else if (at == 2u) c5_run_gap(4);   // after the slot scan, before the ring position is loaded
    else if (at == 3u) c5_run_gap(5);   // after the ring position is loaded
}
struct C5Out { uint32_t hazards, hazardAccepted, accepted, refused; };
static C5Out c5_walk(uint64_t rlo, uint32_t rbytes, uint64_t wlo, uint64_t whi, uint64_t dlo, uint64_t dhi, uint32_t fine, uint32_t used)
{
    C5Out o {};
    int p[5];
    for (p[0] = 0; p[0] < 7; p[0]++) for (p[1] = p[0]; p[1] < 7; p[1]++) for (p[2] = p[1]; p[2] < 7; p[2]++)
    for (p[3] = p[2]; p[3] < 7; p[3]++) for (p[4] = p[3]; p[4] < 7; p[4]++) {
        Cg cg; gC5 = nullptr;
        for (int k = 0; k < 5; k++) { gC5Place[k] = p[k]; gC5Done[k] = 0; }
        gC5Lo = wlo; gC5Hi = whi; gC5Dlo = dlo; gC5Dhi = dhi; gC5Slot = -1;
        n48_cg_pagerec *r = new n48_cg_pagerec; rec_reset(r, fine);
        Cg *w = &cg;
        gC5 = w;
        c5_run_gap(0);
        const uint64_t since = n48_cg_ring_mark(&cg.ring);
        c5_run_gap(1);
        n48_cg_pagerec_note(r, rlo, rbytes);
        c5_run_gap(2);
        const uint32_t v = cg.check(r, fine, used, since);
        c5_run_gap(3); c5_run_gap(4); c5_run_gap(5);   // (anything the check's steps did not run: an early refusal)
        c5_run_gap(6);
        gC5 = nullptr;
        // the hazard uses the range the write REALLY covers: the delta when one is announced as dlo/dhi (SHADOW's would-be) or lo/hi
        const uint64_t hl = (used && dhi) ? dlo : wlo, hh = (used && dhi) ? dhi : whi;
        const bool overl = n48_cg_overlap(hl, hh, rlo, rlo + rbytes);
        // the writes lie between BEGIN and END (the kext: navi48_cg_open pushes BEGIN before the first write; END follows the last)
        const bool meets = p[1] <= 4 && p[3] >= 1;
        const bool hazard = overl && meets;
        if (hazard) { o.hazards++; if (v == N48_CG_OK) { o.hazardAccepted++; if (getenv("C5DBG")) std::printf("      hazard accepted: %d %d %d %d %d\n", p[0], p[1], p[2], p[3], p[4]); } }
        if (v == N48_CG_OK) o.accepted++; else o.refused++;
        delete r;
    }
    return o;
}
static void test_C5()
{
    const uint64_t P = 0x10010000ull;
    // (a) the design's target: a pass reads an EARLIER entry (+0x40, 32 B); the delta appends at +0x700
    C5Out a = c5_walk(P + 0x40, 32u, P + 0x700, P + 0x720, 0, 0, 1u, 0u);
    expect_u("C5a fine: a disjoint read during a delta write is never refused (462 of 462 accepted)", a.accepted, 462u);
    expect_u("C5a fine: no hazard exists for disjoint ranges", a.hazards, 0u);
    C5Out ap = c5_walk(P + 0x40, 32u, P + 0x700, P + 0x720, 0, 0, 0u, 0u);
    expect("C5a page model: the same disjoint schedules are refused whenever the copy meets the pass", ap.refused > 0u && ap.refused < 462u);
    // (b) overlapping granule
    C5Out b = c5_walk(P + 0x40, 32u, P + 0x50, P + 0x60, 0, 0, 1u, 0u);
    expect_u("C5b fine: every hazard (an overlapping granule, copy meets the pass) is refused", b.hazardAccepted, 0u);
    expect("C5b fine: hazards exist and non-hazard accepts exist (all-old / all-new)", b.hazards > 100u && b.accepted > 10u);
    // (c) adjacent granules: the read ends at +0x60, the write starts at +0x60
    C5Out c = c5_walk(P + 0x40, 32u, P + 0x60, P + 0x70, 0, 0, 1u, 0u);
    expect_u("C5c fine: adjacent (read [0x40,0x60), write [0x60,0x70)) is never refused", c.accepted, 462u);
    // (d) a write inside the same granule as a 4-byte read
    C5Out d = c5_walk(P + 0x44, 4u, P + 0x48, P + 0x4c, 0, 0, 1u, 0u);
    expect("C5d fine: a write in the SAME granule as the read (bytes disjoint) is refused whenever it meets the pass (16-byte "
           "granularity is conservative)", d.hazards == 0u && d.refused == ap.refused);
    // (e) SHADOW's instrument: full-range events carrying the delta (used 1) decide as ON's delta events do
    C5Out e1 = c5_walk(P + 0x40, 32u, P, P + 0x8000, P + 0x700, P + 0x720, 1u, 1u);
    expect_u("C5e SHADOW instrument (full copy, would-be delta +0x700): every schedule accepted, as ON's", e1.accepted, a.accepted);
    C5Out e2 = c5_walk(P + 0x40, 32u, P, P + 0x8000, P + 0x50, P + 0x60, 1u, 1u);
    expect_u("C5e SHADOW instrument (would-be delta over the read): refusals equal ON's", e2.refused, b.refused);
    // (f) a torn/wrap-free page-crossing read against a write in the next page
    C5Out f = c5_walk(P + 0xff0, 32u, P + 0x1000, P + 0x1010, 0, 0, 1u, 0u);
    expect_u("C5f fine: a read across pages meets a write in page 2's granule 0: every hazard refused", f.hazardAccepted, 0u);
    expect("C5f fine: ... hazards exist", f.hazards > 100u);
}

// ================================================================================================================ C6 writer
struct FakeVram { uint8_t b[0x20000]; uint64_t base; uint64_t wlo, whi; uint32_t refuseAt; uint32_t nwr; uint32_t corruptAt; };
static uint32_t tb[64], tk[64];
static int fv_wr(void *c, uint64_t vram, const uint32_t *src, uint32_t dwords)
{
    FakeVram *f = static_cast<FakeVram *>(c);
    if (f->refuseAt && ++f->nwr >= f->refuseAt) return 0;
    if (vram < f->wlo) f->wlo = vram;
    if (vram + 4ull * dwords > f->whi) f->whi = vram + 4ull * dwords;
    std::memcpy(f->b + (vram - f->base), src, 4u * dwords);
    return 1;
}
static int fv_rd(void *c, uint64_t vram, uint32_t *dst, uint32_t dwords)
{
    FakeVram *f = static_cast<FakeVram *>(c);
    std::memcpy(dst, f->b + (vram - f->base), 4u * dwords);
    if (f->corruptAt && vram - f->base <= f->corruptAt && f->corruptAt < vram - f->base + 4u * dwords) dst[0] ^= 1u;
    return 1;
}
struct WT {   // the writer's world
    Cg cg; n48_d84_tab t; uint8_t pool[(N48_D84_KEYS + 1) * N48_D84_MAX_BYTES]; uint8_t src[N48_D84_MAX_BYTES];
    n48_d84_world w; n48_sk82_snap snap;
    WT() {
        std::memset(&t, 0, sizeof t); std::memset(pool, 0, sizeof pool); std::memset(src, 0, sizeof src);
        for (uint32_t i = 0; i < N48_D84_KEYS; i++) t.k[i].shadow = pool + i * N48_D84_MAX_BYTES;
        t.scratch = pool + N48_D84_KEYS * N48_D84_MAX_BYTES;
        w.slots = cg.slots; w.nslots = N48_CG_SLOTS; w.untracked = &cg.untracked; w.poison = &cg.poison; w.step = nullptr; w.stepCtx = nullptr;
        std::memset(&snap, 0, sizeof snap);
    }
};
static int wt_src(void *c, uint8_t *dst, uint64_t n) { std::memcpy(dst, static_cast<WT *>(c)->src, n); return 1; }
// MF-3's predicate, faked: a program candidate at resource offset gProgsAt (~0: none)
static uint64_t gProgsAt = ~0ull;
static uint32_t t_progs(void *, const uint8_t *, uint64_t, uint64_t ds, uint64_t de) { return (gProgsAt >= ds && gProgsAt < de) ? 1u : 0u; }
static uint64_t gNowUs = 1000000000ull, gLastUnk = 0ull;
static int64_t gWsPid = 100;
static n48_d84_in wt_in(uint32_t mode, const n48_d84_id &id)
{
    n48_d84_in in {};
    in.mode = mode; in.elig = 1u; in.wsPid = gWsPid; in.nowUs = gNowUs; in.lastUnknownUs = gLastUnk;
    in.progs = &t_progs; in.progsCtx = nullptr;
    in.cgLo = id.vram; in.cgHi = id.vram + id.bytes; in.dirtyLo = id.vram; in.dirtyHi = id.vram + 0x8000u;
    return in;
}
static n48_d84_id wt_id(uint64_t res = 0xffffff80000a0000ull) {
    n48_d84_id id {}; id.res = res; id.dstMem = 0xffffff80000b0000ull; id.vram = 0x10010000ull; id.bytes = 0x8000u; id.boff = 0; id.md = 0xffffff80000c0000ull;
    id.va = 0x4000b0000ull; id.pid = 100; return id;
}
// One whole copy under the pure model, as the kext runs it: G0 plan, the scope (open: slot + own-bump skip + BEGIN), the write
// (a delta from the scratch, or the whole source), the result, the close (END, seq even), the update. Returns the plan.
static n48_d84_plan_out wt_copy(WT *x, FakeVram *v, uint32_t mode, uintptr_t me, bool mism = false, bool failed = false, uint32_t programs = 0u)
{
    const n48_d84_id id = wt_id();
    n48_d84_plan_out o {};
    const n48_d84_in in = wt_in(mode, id);
    (void)n48_d84_plan(&x->t, &x->w, &in, &id, &x->snap, &wt_src, x, me, &o);
    const uint64_t lo = x->t.p.thr == me ? x->t.p.lo : id.vram, hi = x->t.p.thr == me ? x->t.p.hi : id.vram + id.bytes;
    const int32_t s = x->cg.open(lo, hi);
    (void)n48_d84_bump(x->t.k, N48_D84_KEYS, lo, hi, me);
    x->cg.begin(s, lo, hi);
    uint64_t compared = 0, mm = 0;
    if (o.act && o.kind == N48_D84_K_DELTA) {
        (void)n48_d84_copy_loop(v, &fv_wr, &fv_rd, x->t.scratch, id.vram, o.ds, o.de, &compared, &mm, tb, tk);
    } else {
        const uint8_t *img = (o.act) ? x->t.scratch : x->src;   // ON establishes from the scratch; SHADOW/OFF from the backing
        std::memcpy(v->b + (id.vram - v->base), img, id.bytes);
        compared = o.act ? id.bytes / 4u : 8u;                  // SHADOW: a sampled verify
    }
    if (mism) mm = 1;
    n48_d84_census(&x->t, me, programs);
    n48_d84_result(&x->t, me, 1u, compared, mm, o.act && o.kind == N48_D84_K_DELTA ? o.de - o.ds : id.bytes);
    x->cg.end(s, lo, hi); x->cg.close(s);
    (void)n48_d84_closed(&x->t, &x->w, me, lo, hi, 1u, failed ? 1u : 0u, mm ? 1u : 0u);
    return o;
}
// R6: a resource's FIRST eligible copy only records a sighting (no key, no read, no pending)
static void wt_sight(WT *x, uint32_t mode)
{
    const n48_d84_id id = wt_id();
    n48_d84_plan_out o {};
    const n48_d84_in in = wt_in(mode, id);
    (void)n48_d84_plan(&x->t, &x->w, &in, &id, &x->snap, &wt_src, x, 0xa1, &o);
}
static void test_C6()
{
    // eligibility
    n48_d84_elig e {}; e.type = 0x40u; e.swz = 0x20u; e.swz1dc = 0x20u; e.bytes = 0x8000u; e.dsegs = 1u; e.vaOk = 1u; e.keyOk = 1u;
    expect_u("C6 eligible: USER2's shape (type 0x40, SW_MODE 0, 32 KiB, one segment)", n48_d84_eligible(&e), 1u);
    n48_d84_elig e2 = e; e2.swz = 0x36u; expect_u("C6 not eligible: the hardware's record says SW_MODE 22 (a re-tile is possible)", n48_d84_eligible(&e2), 0u);
    e2 = e; e2.swz1dc = 0x16u; expect_u("C6 not eligible: res+0x1dc SW_MODE != 0", n48_d84_eligible(&e2), 0u);
    e2 = e; e2.bytes = 0x8010u; expect_u("C6 not eligible: bytes > 0x8000", n48_d84_eligible(&e2), 0u);
    e2 = e; e2.dsegs = 2u; expect_u("C6 not eligible: two destination segments", n48_d84_eligible(&e2), 0u);
    e2 = e; e2.linBytes = 1u; expect_u("C6 not eligible: a switch-59 image", n48_d84_eligible(&e2), 0u);
    e2 = e; e2.type = 0xc0u; expect_u("C6 not eligible: type != 0x40", n48_d84_eligible(&e2), 0u);
    e2 = e; e2.maskBad = 1u; expect_u("C6 not eligible: an unreadable res+0x180 record", n48_d84_eligible(&e2), 0u);
    // the diff: every differing byte inside [ds, de), 16-aligned, not wider than one granule each side
    static uint8_t a[0x8000], b[0x8000];
    uint32_t bad = 0;
    for (uint32_t it = 0; it < 3000u; it++) {
        std::memset(a, 0x5a, sizeof a); std::memcpy(b, a, sizeof b);
        const uint32_t k = (uint32_t)(rnd() % 4u);
        uint64_t f = ~0ull, l = 0;
        for (uint32_t q = 0; q < k; q++) { const uint64_t i = rnd() % 0x8000u; b[i] ^= 0x81u; if (i < f) f = i; if (i > l) l = i; }
        uint64_t ds = 0, de = 0;
        n48_d84_diff(b, a, 0x8000u, &ds, &de);
        if (k == 0u) { if (ds != 0u || de != 16u) bad++; continue; }
        if ((ds & 15u) || (de & 15u) || ds > f || de <= l || f - ds >= 16u || de - l > 16u) bad++;
        for (uint64_t i = 0; i < 0x8000u; i++) if (a[i] != b[i] && (i < ds || i >= de)) { bad++; break; }
    }
    expect_u("C6 diff: every differing byte in [ds, de); ds = first & ~15; de = (last + 16) & ~15 (3000 random)", bad, 0u);
    { std::memset(a, 0, sizeof a); std::memcpy(b, a, sizeof b); b[0x7ffc] = 1; uint64_t ds, de; n48_d84_diff(b, a, 0x8000u, &ds, &de);
      expect_u("C6 diff: a byte at 0x7ffc -> [0x7ff0, 0x8000)", (ds << 16) | de, (0x7ff0ull << 16) | 0x8000u); }
    { std::memset(a, 0, sizeof a); std::memcpy(b, a, sizeof b); b[0x41] = 1; b[0x59] = 1; uint64_t ds, de; n48_d84_diff(b, a, 0x8000u, &ds, &de);
      expect_u("C6 diff: RUN AA's append [0x41,0x59] -> [0x40, 0x60)", (ds << 16) | de, (0x40ull << 16) | 0x60u); }
    { std::memset(a, 0, sizeof a); std::memcpy(b, a, sizeof b); uint64_t ds, de; n48_d84_diff(b, a, 0x8000u, &ds, &de);
      expect_u("C6 diff: an empty diff writes [0, 16) (SKIP stays forbidden,)", (ds << 16) | de, 16u); }
    // the delta loop: every write inside the scope, every byte of [ds, de) written, nothing else
    {
        static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull; v.whi = 0;
        static uint8_t img[0x8000]; for (uint32_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i * 7u + 3u);
        uint64_t lo, hi, cmp = 0, mm = 0;
        n48_d84_scope(v.base, 0x6e0u, 0x700u + 0x20u, &lo, &hi);
        const int ok = n48_d84_copy_loop(&v, &fv_wr, &fv_rd, img, v.base, 0x6e0u, 0x720u, &cmp, &mm, tb, tk);
        expect_u("C6 delta loop: ok", (uint64_t)ok, 1u);
        expect("C6 delta loop: every write inside the scope [vram + ds, vram + de)", v.wlo >= lo && v.whi <= hi);
        expect("C6 delta loop: the whole scope written", v.wlo == lo && v.whi == hi);
        expect_u("C6 delta loop: every dword read back (compared x 4 == de - ds)", cmp * 4u, 0x40u);
        bool eq = true; for (uint32_t i = 0x6e0; i < 0x720; i++) if (v.b[i] != img[i]) eq = false;
        bool untouched = true; for (uint32_t i = 0; i < 0x8000; i++) if ((i < 0x6e0 || i >= 0x720) && v.b[i] != 0) untouched = false;
        expect("C6 delta loop: the delta holds the image, nothing outside it was written", eq && untouched);
        FakeVram v2; std::memset(&v2, 0, sizeof v2); v2.base = 0x10010000ull; v2.wlo = ~0ull; v2.refuseAt = 2u;
        uint64_t c2 = 0, m2 = 0;
        expect_u("C6 delta loop: a refused batch answers 0 (a partial write: the caller poisons)",
                 (uint64_t)n48_d84_copy_loop(&v2, &fv_wr, &fv_rd, img, v2.base, 0u, 0x400u, &c2, &m2, tb, tk), 0u);
        FakeVram v3; std::memset(&v3, 0, sizeof v3); v3.base = 0x10010000ull; v3.wlo = ~0ull; v3.corruptAt = 0x10u;
        uint64_t c3 = 0, m3 = 0; (void)n48_d84_copy_loop(&v3, &fv_wr, &fv_rd, img, v3.base, 0u, 0x40u, &c3, &m3, tb, tk);
        expect_u("C6 delta loop: a read-back that differs is counted", m3, 1u);
    }
    // the flow under ON: establish, then deltas
    {
        WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
        for (uint32_t i = 0; i < 0x40; i++) x->src[i] = (uint8_t)(i + 1);
        n48_d84_plan_out o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 R6: ON 1st copy of a resource: only a sighting (no key claimed, nothing read, no pending, never acts)",
                 (o.act << 16) | (o.kind << 8) | (uint32_t)(x->t.k[0].state), 0u);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 ON 2nd copy: no key -> ESTABLISH (acts: MM from the scratch)", (o.act << 8) | o.kind, (1u << 8) | N48_D84_K_EST);
        expect_u("C6 ON 2nd copy: the key is VALID after a clean full read-back", x->t.k[0].state, N48_D84_S_VALID);
        for (uint32_t i = 0x40; i < 0x5a; i++) x->src[i] = 0x77;   // RUN AA's append
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 ON 2nd copy: DELTA [0x40, 0x60)", ((uint64_t)o.kind << 32) | (o.ds << 16) | o.de, ((uint64_t)N48_D84_K_DELTA << 32) | (0x40ull << 16) | 0x60u);
        expect_u("C6 ON 2nd copy: the scope is exactly the delta [vram + 0x40, vram + 0x60)", ((x->t.p.lo - 0x10010000ull) << 16) | (x->t.p.hi - 0x10010000ull), (0x40ull << 16) | 0x60u);
        bool eq = std::memcmp(v.b, x->src, 0x8000u) == 0;
        expect("C6 ON: VRAM equals the source after the delta", eq);
        expect("C6 ON: the shadow equals VRAM after the delta's update", std::memcmp(x->t.k[0].shadow, v.b, 0x8000u) == 0);
        // SHADOW never acts
        WT *y = new WT; static FakeVram w; std::memset(&w, 0, sizeof w); w.base = 0x10010000ull; w.wlo = ~0ull;
        wt_sight(y, N48_CG84_SHADOW);
        o = wt_copy(y, &w, N48_CG84_SHADOW, 0xa1);
        expect_u("C6 SHADOW 1st keyed copy: never acts", o.act, 0u);
        for (uint32_t i = 0x40; i < 0x5a; i++) y->src[i] = 0x77;
        o = wt_copy(y, &w, N48_CG84_SHADOW, 0xa1);
        expect_u("C6 SHADOW 2nd copy: never acts (X1: no delta written)", o.act, 0u);
        expect_u("C6 SHADOW 2nd copy: the WOULD-BE delta is [0x40, 0x60) (dlo/dhi)", ((o.dlo - 0x10010000ull) << 16) | (o.dhi - 0x10010000ull), (0x40ull << 16) | 0x60u);
        expect_u("C6 SHADOW: counted as would-delta, not delta", (y->t.st.wouldDelta << 8) | y->t.st.delta, 1u << 8);
        // poison forces a full copy
        n48_cg_poison_mark(&x->cg.poison, 0x10010100ull, 0x10010110ull);
        for (uint32_t i = 0x60; i < 0x70; i++) x->src[i] = 0x33;
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 ON: poison over the range forces a full copy (ESTABLISH, cause POISON)", (o.kind << 8) | o.cause, (N48_D84_K_EST << 8) | N48_D84_C_POISON);
        expect("C6 ON: ... and no key is established over poison", x->t.k[0].state != N48_D84_S_VALID);
        n48_cg_poison_init(&x->cg.poison);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 ON: after the poison is gone, a clean full copy re-establishes", x->t.k[0].state, N48_D84_S_VALID);
        // an overlapping open by another thread invalidates; our own open does not
        const uint64_t before = x->t.k[0].inval;
        expect_u("C6 an overlapping open (another thread) moves the key", n48_d84_bump(x->t.k, N48_D84_KEYS, 0x10017000ull, 0x10017004ull, 0xbb), 1u);
        expect("C6 ... its counter moved", x->t.k[0].inval == before + 1u);
        expect_u("C6 a disjoint open does not", n48_d84_bump(x->t.k, N48_D84_KEYS, 0x10018000ull, 0x10019000ull, 0xbb), 0u);
        for (uint32_t i = 0x70; i < 0x80; i++) x->src[i] = 0x44;
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 ON: after a foreign open, the next copy is full (cause INVAL)", (o.kind << 8) | o.cause, (N48_D84_K_EST << 8) | N48_D84_C_INVAL);
        for (uint32_t i = 0x80; i < 0x90; i++) x->src[i] = 0x45;
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 ON: our own opens never moved the key (the marker): the next copy is a delta", o.kind, N48_D84_K_DELTA);
        // page-out, unmap, commit targets, WindowServer
        n48_d84_inval_res(&x->t, 0x1234u);
        expect_u("C6 a page-out of ANOTHER resource leaves the key", x->t.k[0].state, N48_D84_S_VALID);
        n48_d84_inval_res(&x->t, wt_id().res);
        expect_u("C6 a page-out of the resource invalidates the key", x->t.k[0].state, N48_D84_S_INVALID);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        n48_d84_inval_va(&x->t, 0x4000c0000ull, 0x1000u);
        expect_u("C6 an unmap outside the VA leaves the key", x->t.k[0].state, N48_D84_S_VALID);
        n48_d84_inval_va(&x->t, 0x4000b7000ull, 0x2000u);
        expect_u("C6 an unmap overlapping the VA invalidates the key", x->t.k[0].state, N48_D84_S_INVALID);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        uint64_t tg[2] = { 0x400800000ull, 0x401800000ull };
        n48_d84_inval_targets(&x->t, tg, 2u, 1u);
        expect_u("C6 a committed frame whose targets all lie ABOVE the key's end leaves the key", x->t.k[0].state, N48_D84_S_VALID);
        { uint64_t lowT[1] = { 0x400000000ull };   // MF-2: a base BELOW the key (its surface may extend over it)
          n48_d84_inval_targets(&x->t, lowT, 1u, 1u);
          expect_u("C6 MF-2: a committed target base BELOW the key's VA invalidates it (any base at or below the end)", x->t.k[0].state, N48_D84_S_INVALID);
          o = wt_copy(x, &v, N48_CG84_ON, 0xa1); }
        tg[1] = 0x4000b4000ull;
        n48_d84_inval_targets(&x->t, tg, 2u, 1u);
        expect_u("C6 a committed target base inside the VA range invalidates the key", x->t.k[0].state, N48_D84_S_INVALID);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        n48_d84_inval_targets(&x->t, tg, 0u, 0u);
        expect_u("C6 a committed frame with more targets than held invalidates every key", x->t.k[0].state, N48_D84_S_INVALID);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        n48_d84_inval_all(&x->t);
        expect_u("C6 a WindowServer drop / context release invalidates every key", x->t.k[0].state, N48_D84_S_INVALID);
        // the update's conditions
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1, /*mism=*/true);
        expect("C6 a read-back mismatch never establishes (the shadow is not published)", x->t.k[0].state != N48_D84_S_VALID);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1, false, /*failed=*/true);
        expect("C6 a FAILED copy never establishes", x->t.k[0].state != N48_D84_S_VALID);
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);   // re-established clean
        // a mismatched DELTA leaves the key invalid and the shadow unpublished
        for (uint32_t i = 0x90; i < 0xa0; i++) x->src[i] = 0x46;
        const uint8_t keep = x->t.k[0].shadow[0x90];
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1, /*mism=*/true);
        expect("C6 a mismatched delta: no update (the shadow keeps its old bytes), the key invalid",
               o.kind == N48_D84_K_DELTA && x->t.k[0].shadow[0x90] == keep && x->t.k[0].state != N48_D84_S_VALID);
        // the state snapshot
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        x->snap.v[N48_SK82_SN_ARM] = 7u;
        for (uint32_t i = 0xa0; i < 0xb0; i++) x->src[i] = 0x47;
        o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
        expect_u("C6 an arm change (the snapshot) forces a full copy", o.cause, N48_D84_C_STATE);
        // busy: the scratch is owned
        const n48_d84_id id = wt_id();
        n48_d84_plan_out o1 {}, o2 {};
        { const n48_d84_in in1 = wt_in(N48_CG84_ON, id); (void)n48_d84_plan(&x->t, &x->w, &in1, &id, &x->snap, &wt_src, x, 0xa1, &o1); }
        n48_d84_id id2 = wt_id(0xffffff80000d0000ull); id2.vram = 0x10020000ull; id2.va = 0x400200000ull;
        { const n48_d84_in in2 = wt_in(N48_CG84_ON, id2); (void)n48_d84_plan(&x->t, &x->w, &in2, &id2, &x->snap, &wt_src, x, 0xa2, &o2); }
        expect_u("C6 a second eligible copy while the scratch is owned: BUSY, never acts", (o2.act << 8) | o2.cause, N48_D84_C_BUSY);
        (void)n48_d84_closed(&x->t, &x->w, 0xa1, x->t.p.lo, x->t.p.hi, 0u, 0u, 0u);
        expect_u("C6 the scratch is released at the post-close", (uint64_t)x->t.p.thr, 0u);
        // OFF never plans
        n48_d84_plan_out o3 {};
        { const n48_d84_in in3 = wt_in(N48_CG84_OFF, id);
          expect_u("C6 OFF: the plan does nothing", n48_d84_plan(&x->t, &x->w, &in3, &id, &x->snap, &wt_src, x, 0xa1, &o3) + (uint64_t)x->t.p.thr, 0u); }
        delete x; delete y;
    }
}

// ================================================================================================================ C7 D1 order
// A foreign writer F (another thread) over [flo, fhi) inside the key: 1 publish its slot, 2 bump (n48_d84_bump), 3 write a marker
// into VRAM, 4 close. Our copy (ON, an establishing full copy then a delta) runs: G0 (plan: marker, source, c0, [step AT_G0_C0],
// slot scan, [AT_G0_SCAN]), our open, our write, our close, the update ([AT_UPD_SCAN] after its slot scan, [AT_UPD_C1] after c0's
// re-read). F's four steps are placed at the 8 points in order (C(11,4) = 330 schedules). Violation: the key VALID and its shadow
// != VRAM.
struct C7 { WT *x; FakeVram *v; int place[4]; int done[4]; int32_t fslot; uint64_t flo, fhi; };
static C7 *gC7 = nullptr;
static void c7_run(int at)
{
    C7 *c = gC7;
    if (!c) return;
    for (int k = 0; k < 4; k++) {
        if (c->done[k] || c->place[k] != at) continue;
        if (k == 0) c->fslot = c->x->cg.open(c->flo, c->fhi);
        else if (k == 1) (void)n48_d84_bump(c->x->t.k, N48_D84_KEYS, c->flo, c->fhi, 0xf0f0u);
        else if (k == 2) c->v->b[c->flo - c->v->base] ^= 0xffu;
        else c->x->cg.close(c->fslot);
        c->done[k] = 1;
    }
}
static void c7_step(void *, uint32_t at) { c7_run((int)at); }
static uint32_t c7_walk(bool delta)
{
    uint32_t viol = 0, validCount = 0;
    int p[4];
    for (p[0] = 0; p[0] < 8; p[0]++) for (p[1] = p[0]; p[1] < 8; p[1]++) for (p[2] = p[1]; p[2] < 8; p[2]++) for (p[3] = p[2]; p[3] < 8; p[3]++) {
        WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
        for (uint32_t i = 0; i < 0x100; i++) x->src[i] = (uint8_t)i;
        wt_sight(x, N48_CG84_ON);
        if (delta) { (void)wt_copy(x, &v, N48_CG84_ON, 0xa1); for (uint32_t i = 0x10; i < 0x20; i++) x->src[i] = 0x99; }
        C7 c {}; c.x = x; c.v = &v; c.fslot = -1; c.flo = 0x10010400ull; c.fhi = 0x10010410ull;
        static const int E[8] = { 0, 1, 2, 5, 6, 3, 4, 7 };   // the points in EXECUTION order: G0 (1, 2), our open..close (5, 6), update (3, 4)
        for (int k = 0; k < 4; k++) { c.place[k] = E[p[k]]; c.done[k] = 0; }
        gC7 = &c;
        x->w.step = &c7_step; x->w.stepCtx = nullptr;
        // point 0: before G0
        c7_run(0);
        const n48_d84_id id = wt_id();
        n48_d84_plan_out o {};
        const n48_d84_in in = wt_in(N48_CG84_ON, id);
        (void)n48_d84_plan(&x->t, &x->w, &in, &id, &x->snap, &wt_src, x, 0xa1, &o);  // points 1, 2
        c7_run(1); c7_run(2);
        const uint64_t lo = x->t.p.lo, hi = x->t.p.hi;
        const int32_t s = x->cg.open(lo, hi);
        (void)n48_d84_bump(x->t.k, N48_D84_KEYS, lo, hi, 0xa1);
        c7_run(5);   // point 5: after our open, before our write
        uint64_t cmp = 0, mm = 0;
        if (o.kind == N48_D84_K_DELTA) (void)n48_d84_copy_loop(&v, &fv_wr, &fv_rd, x->t.scratch, id.vram, o.ds, o.de, &cmp, &mm, tb, tk);
        else { std::memcpy(v.b, x->t.scratch, id.bytes); cmp = id.bytes / 4u; }
        n48_d84_result(&x->t, 0xa1, 1u, cmp, mm, o.de - o.ds);
        c7_run(6);   // point 6: after our write, before our close
        x->cg.close(s);
        (void)n48_d84_closed(&x->t, &x->w, 0xa1, lo, hi, 1u, 0u, 0u);   // points 3, 4
        c7_run(3); c7_run(4); c7_run(7);
        gC7 = nullptr;
        // the key is TRUSTED only while VALID at inval0 (a later bump already bars it); a trusted key must equal VRAM
        if (x->t.k[0].state == N48_D84_S_VALID && x->t.k[0].inval == x->t.k[0].inval0) {
            validCount++;
            if (std::memcmp(x->t.k[0].shadow, v.b, id.bytes) != 0) viol++;
        }
        delete x;
    }
    std::printf("      C7 %s: %u schedules left the key VALID\n", delta ? "delta" : "establish", validCount);
    return viol | (validCount ? 0u : 0x80000000u);
}
static void test_C7()
{
    expect_u("C7 D1 order (establishing copy): 330 schedules, never a VALID key whose shadow differs from VRAM (and some VALID)", c7_walk(false), 0u);
    expect_u("C7 D1 order (delta copy): 330 schedules, never a VALID key whose shadow differs from VRAM (and some VALID)", c7_walk(true), 0u);
}

// ================================================================================================================ C8 capdec fixture
// The design's worst case: every pass overlaps the next K page-ins of the table. The page model's page-in writes [0x10010000,
// 0x10018000); the granule model's writes each entry whose content changed (read by frame N and by the later frame with another
// FNV) - the design's measure; the contiguous [first, last] delta is printed beside it (not asserted).
struct FixRun { const char *name; uint32_t n; const uint32_t *frame, *start; const Cg84Read *rd; };
static uint32_t fix_find(const FixRun &f, uint32_t i, uint32_t off, uint32_t *fnv)
{
    for (uint32_t q = f.start[i]; q < f.start[i + 1]; q++) if (f.rd[q].off == off) { *fnv = f.rd[q].fnv; return 1u; }
    return 0u;
}
struct FixOut { uint32_t page, gran, granContig, off, shadowEqOff, instrEqOn; };
static FixOut fix_run(const FixRun &f, uint32_t K)
{
    const uint64_t T = 0x10010000ull;
    FixOut out {};
    for (uint32_t i = 0; i < f.n; i++) {
        // the deltas of the next K page-ins, as entries (and their contiguous hull)
        std::vector<std::pair<uint64_t, uint64_t>> dl;
        uint64_t hlo = ~0ull, hhi = 0;
        for (uint32_t j = i + 1; j <= i + K && j < f.n; j++) {
            for (uint32_t q = f.start[i]; q < f.start[i + 1]; q++) {
                uint32_t fnvM = 0;
                if (fix_find(f, j, f.rd[q].off, &fnvM) && fnvM != f.rd[q].fnv) {
                    dl.push_back({ T + f.rd[q].off, T + f.rd[q].off + 32u });
                    if (T + f.rd[q].off < hlo) hlo = T + f.rd[q].off;
                    if (T + f.rd[q].off + 32u > hhi) hhi = T + f.rd[q].off + 32u;
                }
            }
            // entries frame j reads whose CONTENT no earlier captured frame read (a new entry: an append or a rewrite N did not
            // read) - also inside that page-in's delta
            for (uint32_t q = f.start[j]; q < f.start[j + 1]; q++) {
                bool seen = false;
                for (uint32_t e = 0; e < j && !seen; e++)
                    for (uint32_t u = f.start[e]; u < f.start[e + 1]; u++)
                        if (f.rd[u].off == f.rd[q].off && f.rd[u].fnv == f.rd[q].fnv) { seen = true; break; }
                if (!seen) {
                    dl.push_back({ T + f.rd[q].off, T + f.rd[q].off + 32u });
                    if (T + f.rd[q].off < hlo) hlo = T + f.rd[q].off;
                    if (T + f.rd[q].off + 32u > hhi) hhi = T + f.rd[q].off + 32u;
                }
            }
        }
        // no captured entry changed: the page-in's change is outside every captured entry (the table differs on nearly every copy,
        //) - modelled as the table's last granule, which no captured frame reads (the design's measure ignores it)
        if (dl.empty()) { dl.push_back({ T + 0x7ff0u, T + 0x8000u }); hlo = T + 0x7ff0u; hhi = T + 0x8000u; }
        for (uint32_t mode = 0; mode < 5; mode++) {   // 0 OFF, 1 page full (== OFF), 2 ON granule entries, 3 ON contiguous, 4 SHADOW
            Cg cg;
            n48_cg_pagerec *r = new n48_cg_pagerec; rec_reset(r, (mode == 2 || mode == 3) ? 1u : 0u);
            const uint64_t since = n48_cg_ring_mark(&cg.ring);
            for (uint32_t q = f.start[i]; q < f.start[i + 1]; q++) n48_cg_pagerec_note(r, T + f.rd[q].off, 32u);
            uint32_t v = 0, vi = 0;
            if (mode <= 1) { cg.begin(0, T, T + 0x8000u); cg.end(0, T, T + 0x8000u); v = cg.check(r, 0u, 0u, since); }
            else if (mode == 2) { for (auto &d : dl) { cg.begin(0, d.first, d.second); cg.end(0, d.first, d.second); } v = cg.check(r, 1u, 0u, since); }
            else if (mode == 3) { cg.begin(0, hlo, hhi); cg.end(0, hlo, hhi); v = cg.check(r, 1u, 0u, since); }
            else {
                for (auto &d : dl) { cg.begin(0, T, T + 0x8000u, d.first, d.second); cg.end(0, T, T + 0x8000u, d.first, d.second); }
                v = cg.check(r, 0u, 0u, since);                       // SHADOW's counted check: the page check
                vi = n48_cg_check_fx(r, 1u, 1u, &cg.poison, cg.slots, N48_CG_SLOTS, &cg.ring, since, 0u, nullptr);   // its instrument
            }
            if (mode == 0 && v) out.off++;
            if (mode == 1 && v) out.page++;
            if (mode == 2 && v) out.gran++;
            if (mode == 3 && v) out.granContig++;
            if (mode == 4) {
                // SHADOW's counted verdict == OFF's (the page check over full copies); its instrument == ON's granule verdict
                Cg c2; n48_cg_pagerec *r2 = new n48_cg_pagerec; rec_reset(r2, 1u);
                const uint64_t s2 = n48_cg_ring_mark(&c2.ring);
                for (uint32_t q = f.start[i]; q < f.start[i + 1]; q++) n48_cg_pagerec_note(r2, T + f.rd[q].off, 32u);
                for (auto &d : dl) { c2.begin(0, d.first, d.second); c2.end(0, d.first, d.second); }
                const uint32_t on = c2.check(r2, 1u, 0u, s2);
                Cg c3; n48_cg_pagerec *r3 = new n48_cg_pagerec; rec_reset(r3, 0u);
                const uint64_t s3 = n48_cg_ring_mark(&c3.ring);
                for (uint32_t q = f.start[i]; q < f.start[i + 1]; q++) n48_cg_pagerec_note(r3, T + f.rd[q].off, 32u);
                c3.begin(0, T, T + 0x8000u); c3.end(0, T, T + 0x8000u);
                const uint32_t off = c3.check(r3, 0u, 0u, s3);
                if (v == off) out.shadowEqOff++;
                if (vi == on) out.instrEqOn++;
                delete r2; delete r3;
            }
            delete r;
        }
    }
    return out;
}
static void test_C8()
{
    const FixRun aa { "RUN AA (run11k)", CG84_AA_NFRAMES, CG84_AA_FRAME, CG84_AA_START, CG84_AA_READ };
    const FixRun z3 { "RUN Z3 (run11j)", CG84_Z3_NFRAMES, CG84_Z3_FRAME, CG84_Z3_START, CG84_Z3_READ };
    expect_u("C8 fixture: RUN AA frames reading USER2 T#s", aa.n, 146u);
    expect_u("C8 fixture: RUN Z3 frames reading USER2 T#s", z3.n, 147u);
    const FixOut a1 = fix_run(aa, 1), a4 = fix_run(aa, 4), z1 = fix_run(z3, 1), z4 = fix_run(z3, 4);
    expect_u("C8 RUN AA: the page model refuses 146 of 146", a1.page, 146u);
    expect_u("C8 RUN Z3: the page model refuses 147 of 147", z1.page, 147u);
    expect_u("C8 RUN AA: the granule model refuses 0 at K = 1", a1.gran, 0u);
    expect_u("C8 RUN Z3: the granule model refuses 0 at K = 1", z1.gran, 0u);
    expect_u("C8 RUN AA: the granule model refuses 4 at K = 4", a4.gran, 4u);
    expect_u("C8 RUN Z3: the granule model refuses 1 at K = 4", z4.gran, 1u);
    expect_u("C8 X1 RUN AA K=1: SHADOW's counted verdicts equal OFF's, frame for frame", a1.shadowEqOff, 146u);
    expect_u("C8 X1 RUN Z3 K=1: SHADOW's counted verdicts equal OFF's, frame for frame", z1.shadowEqOff, 147u);
    expect_u("C8 X1 RUN AA K=4: SHADOW's instrument equals ON's counted verdicts, frame for frame", a4.instrEqOn, 146u);
    expect_u("C8 X1 RUN Z3 K=4: SHADOW's instrument equals ON's counted verdicts, frame for frame", z4.instrEqOn, 147u);
    expect_u("C8 X1 RUN AA K=1: SHADOW's instrument equals ON's", a1.instrEqOn, 146u);
    expect_u("C8 X1 RUN Z3 K=1: SHADOW's instrument equals ON's", z1.instrEqOn, 147u);
    std::printf("      C8 RUN AA: page %u/%u; granule K1 %u K4 %u; contiguous-delta hull K1 %u K4 %u (informational)\n", a1.page, aa.n, a1.gran,
                a4.gran, a1.granContig, a4.granContig);
    std::printf("      C8 RUN Z3: page %u/%u; granule K1 %u K4 %u; contiguous-delta hull K1 %u K4 %u (informational)\n", z1.page, z3.n, z1.gran,
                z4.gran, z1.granContig, z4.granContig);
}

// ================================================================================================================ C9 X1 copies
static void test_C9()
{
    // SHADOW over 200 random copies: the plan never acts, so the copy (from the backing, full) is OFF's byte for byte
    WT *x = new WT; static FakeVram vs, vo; std::memset(&vs, 0, sizeof vs); std::memset(&vo, 0, sizeof vo); vs.base = vo.base = 0x10010000ull;
    uint32_t acted = 0, same = 0;
    for (uint32_t it = 0; it < 200u; it++) {
        const uint32_t k = (uint32_t)(rnd() % 3u);
        for (uint32_t q = 0; q < k; q++) x->src[rnd() % 0x8000u] ^= (uint8_t)(1u + (rnd() % 255u));
        const n48_d84_plan_out o = wt_copy(x, &vs, N48_CG84_SHADOW, 0xa1);
        if (o.act) acted++;
        std::memcpy(vo.b, x->src, 0x8000u);   // OFF: the whole source
        if (std::memcmp(vs.b, vo.b, 0x8000u) == 0) same++;
        if ((it % 17u) == 0u) (void)n48_d84_bump(x->t.k, N48_D84_KEYS, 0x10010000ull, 0x10010010ull, 0xbb);
    }
    expect_u("C9 X1: SHADOW never acts (200 copies)", acted, 0u);
    expect_u("C9 X1: SHADOW's VRAM equals OFF's after every copy", same, 200u);
    expect("C9 X1: SHADOW still maintains keys (would-delta counted)", x->t.st.wouldDelta > 50u);
    delete x;
}

// ================================================================================================================ C11 fix pass
static n48_d84_plan_out wt_plan(WT *x, uint32_t mode, const n48_d84_id &id, uintptr_t me = 0xa1)
{
    n48_d84_plan_out o {};
    const n48_d84_in in = wt_in(mode, id);
    (void)n48_d84_plan(&x->t, &x->w, &in, &id, &x->snap, &wt_src, x, me, &o);
    return o;
}
static void test_C11()
{
    const n48_d84_id id = wt_id();
    // MF-5: a foreign pid never takes a key
    { WT *x = new WT; gWsPid = 200;
      const n48_d84_plan_out o = wt_plan(x, N48_CG84_ON, id); const n48_d84_plan_out o2 = wt_plan(x, N48_CG84_ON, id);
      expect("C11 MF-5: a copy of a table not owned by the bound WindowServer never claims a key (twice; FREE, counted)",
             o.act == 0u && o2.act == 0u && x->t.k[0].state == N48_D84_S_FREE && x->t.st.foreign == 2u && x->t.p.thr == 0u);
      gWsPid = -1; const n48_d84_plan_out o3 = wt_plan(x, N48_CG84_ON, id);
      expect("C11 MF-5: no WindowServer bound: no key", o3.act == 0u && x->t.k[0].state == N48_D84_S_FREE);
      gWsPid = 100; delete x; }
    // MF-4
    { WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
      wt_sight(x, N48_CG84_ON);
      n48_d84_plan_out o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect_u("C11 MF-4: NOKEY acts (an establishing MM copy)", o.act, 1u);
      (void)n48_d84_bump(x->t.k, N48_D84_KEYS, 0x10010000ull, 0x10010004ull, 0xbb);
      x->src[3] ^= 1u;
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect_u("C11 MF-4: INVAL acts (re-establishes)", (o.act << 8) | o.cause, (1u << 8) | N48_D84_C_INVAL);
      x->snap.v[N48_SK82_SN_S63] ^= 1u; x->src[4] ^= 1u;
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect_u("C11 MF-4: STATE never acts (0.0.528's copy, fast copy allowed)", (o.act << 8) | o.cause, N48_D84_C_STATE);
      expect("C11 MF-4: ... and a copy that did not act never establishes under ON (it wrote the backing, not the scratch)",
             x->t.k[0].state != N48_D84_S_VALID);
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1);   // re-established under the new state
      n48_cg_poison_mark(&x->cg.poison, 0x10010100ull, 0x10010110ull);
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect_u("C11 MF-4: POISON never acts", (o.act << 8) | o.cause, N48_D84_C_POISON);
      n48_cg_poison_init(&x->cg.poison);
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1, false, false, /*programs=*/2u);
      expect("C11 MF-4: the census with programs: no key", x->t.k[0].state != N48_D84_S_VALID && x->t.k[0].programs == 1u);
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1); const n48_d84_plan_out o2 = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect_u("C11 MF-4: programs are tested BEFORE the state (cause SC, not NOKEY), and never act - no establishing loop",
             (o.cause << 16) | (o.act << 8) | (o2.cause << 4) | o2.act, ((uint32_t)N48_D84_C_SC << 16) | ((uint32_t)N48_D84_C_SC << 4));
      delete x; }
    // MF-3: a program candidate inside the delta
    { WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
      wt_sight(x, N48_CG84_ON); (void)wt_copy(x, &v, N48_CG84_ON, 0xa1);
      for (uint32_t i = 0x40; i < 0x5a; i++) x->src[i] = 0x77;
      gProgsAt = 0x48;
      n48_d84_plan_out o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect("C11 MF-3: a program-candidate start inside [ds, de): no delta, no act (0.0.528's copy with the scan and ic_begin)",
             o.act == 0u && o.kind == N48_D84_K_EST && x->t.st.deltaPrograms == 1u);
      gProgsAt = 0x7000;
      (void)wt_copy(x, &v, N48_CG84_ON, 0xa1); (void)wt_copy(x, &v, N48_CG84_ON, 0xa1);
      for (uint32_t i = 0x60; i < 0x70; i++) x->src[i] = 0x78;
      o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect("C11 MF-3: a candidate OUTSIDE the delta does not stop it", o.act == 1u && o.kind == N48_D84_K_DELTA);
      gProgsAt = ~0ull; delete x; }
    // MF-1: the 100 ms window after an unknown write
    { WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
      wt_sight(x, N48_CG84_ON); (void)wt_copy(x, &v, N48_CG84_ON, 0xa1);
      x->src[0x100] ^= 1u;
      gLastUnk = gNowUs - 50000ull;
      n48_d84_plan_out o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect("C11 MF-1 (ON): an unknown write 50 ms ago: cause INVAL, tainted, no act, no key", o.act == 0u && o.cause == N48_D84_C_INVAL &&
             x->t.st.quiet == 1u && x->t.k[0].state != N48_D84_S_VALID);
      gLastUnk = gNowUs - 200000ull;
      (void)wt_copy(x, &v, N48_CG84_ON, 0xa1); x->src[0x101] ^= 1u; o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect("C11 MF-1 (ON): 200 ms later deltas resume", o.act == 1u && o.kind == N48_D84_K_DELTA);
      WT *y = new WT; static FakeVram w; std::memset(&w, 0, sizeof w); w.base = 0x10010000ull; w.wlo = ~0ull;
      gLastUnk = 0; wt_sight(y, N48_CG84_SHADOW); (void)wt_copy(y, &w, N48_CG84_SHADOW, 0xa1);
      y->src[0x100] ^= 1u; gLastUnk = gNowUs - 50000ull;
      o = wt_copy(y, &w, N48_CG84_SHADOW, 0xa1);
      expect("C11 MF-1 (SHADOW): priced, not acted on (quiet-window would 1, still a would-delta)", y->t.st.quietWould == 1u &&
             o.kind == N48_D84_K_DELTA && y->t.st.quiet == 0u);
      gLastUnk = 0; delete x; delete y; }
    // MF-6
    expect("C11 MF-6: the probe is due on the 32nd, 64th ... delta update only",
           n48_d84_probe_due(31u) == 1u && n48_d84_probe_due(63u) == 1u && n48_d84_probe_due(0u) == 0u && n48_d84_probe_due(30u) == 0u &&
           n48_d84_probe_due(32u) == 0u);
    { uint8_t sh[16] = {}; uint32_t vr[4] = { 0u, 0u, 0x00ff0000u, 0u };
      expect_u("C11 MF-6: the probe compare names the first differing byte", n48_d84_probe_cmp(sh, 0u, vr, 4u), 10u);
      uint32_t eq[4] = { 0u, 0u, 0u, 0u };
      expect_u("C11 MF-6: equal answers ~0", n48_d84_probe_cmp(sh, 0u, eq, 4u), ~0ull); }
    { WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
      wt_sight(x, N48_CG84_ON); (void)wt_copy(x, &v, N48_CG84_ON, 0xa1);
      x->src[0x200] ^= 1u;
      n48_d84_plan_out o {}; const n48_d84_in in = wt_in(N48_CG84_ON, id);
      (void)n48_d84_plan(&x->t, &x->w, &in, &id, &x->snap, &wt_src, x, 0xa1, &o);
      uint64_t c = 0, m = 0; (void)n48_d84_copy_loop(&v, &fv_wr, &fv_rd, x->t.scratch, id.vram, o.ds, o.de, &c, &m, tb, tk);
      n48_d84_result(&x->t, 0xa1, 1u, c, m, o.de - o.ds);
      const uint32_t go = n48_d84_closed_begin(&x->t, &x->w, 0xa1, x->t.p.lo, x->t.p.hi, 1u, 0u, 0u);
      n48_d84_shadow_copy(&x->t);
      const uint32_t upd = n48_d84_closed_end(&x->t, 0xa1, /*probeBad=*/1u);
      expect("C11 MF-6: a probe mismatch is never ignored: END does not publish, the key is invalid, the scratch released",
             go == 1u && upd == 0u && x->t.k[0].state != N48_D84_S_VALID && x->t.p.thr == 0u);
      delete x; }
    // S2
    { WT *x = new WT; static FakeVram v; std::memset(&v, 0, sizeof v); v.base = 0x10010000ull; v.wlo = ~0ull;
      wt_sight(x, N48_CG84_ON); (void)wt_copy(x, &v, N48_CG84_ON, 0xa1);
      const uint64_t i0 = x->t.k[0].inval;
      expect_u("C11 S2: bump_all moves every non-FREE key (one here), FREE keys untouched", n48_d84_bump_all(x->t.k, N48_D84_KEYS), 1u);
      x->src[9] ^= 1u;
      const n48_d84_plan_out o = wt_copy(x, &v, N48_CG84_ON, 0xa1);
      expect("C11 S2: after a lock-free bump the next copy is INVAL (c0 != inval0)", x->t.k[0].inval > i0 && o.cause == N48_D84_C_INVAL);
      delete x; }
}

// ================================================================================================================ C12 census walk
// TEST GAP (fix pass): every VRAM read reachable from the policy pass's roots - through ANY callee defined in AppleHardwareHook.cpp - is
// pinned, function by function. A new read primitive call anywhere in that call graph fails this test until it is censused.
#include <map>
#include <set>
static std::string slurp(const char *p);
static uint32_t count(const std::string &s, const char *needle);
// The body with comments and string / character literals blanked (a name in a comment is not a call).
static std::string code_only(const std::string &b)
{
    std::string o = b;
    for (size_t q = 0; q < o.size(); q++) {
        if (o[q] == '/' && q + 1 < o.size() && o[q + 1] == '/') { while (q < o.size() && o[q] != '\n') o[q++] = ' '; continue; }
        if (o[q] == '/' && q + 1 < o.size() && o[q + 1] == '*') {
            while (q + 1 < o.size() && !(o[q] == '*' && o[q + 1] == '/')) o[q++] = ' ';
            if (q + 1 < o.size()) { o[q] = ' '; o[q + 1] = ' '; q++; }
            continue;
        }
        if (o[q] == '"' || o[q] == '\'') {
            const char e = o[q]; q++;
            while (q < o.size() && o[q] != e) { if (o[q] == '\\' && q + 1 < o.size()) o[q++] = ' '; o[q++] = ' '; }
            continue;
        }
    }
    return o;
}
static std::map<std::string, std::string> parse_funcs(const std::string &s)
{
    std::map<std::string, std::string> out;
    size_t i = 0; const size_t n = s.size();
    auto isid = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    while (i < n) {
        // a definition starts at column 0 with a letter (not '#', '/', '}', ' ')
        const size_t ls = i;
        const size_t le = s.find('\n', i);
        const size_t lend = le == std::string::npos ? n : le;
        const char c0 = s[ls];
        if (((c0 >= 'a' && c0 <= 'z') || (c0 >= 'A' && c0 <= 'Z')) && s.compare(ls, 7, "typedef") && s.compare(ls, 6, "struct")) {
            // the signature runs to the first '{' or ';' at depth 0 of parens
            size_t j = ls; int par = 0; size_t nameEnd = std::string::npos;
            for (; j < n; j++) {
                const char c = s[j];
                if (c == '(') { if (par == 0 && nameEnd == std::string::npos) nameEnd = j; par++; }
                else if (c == ')') par--;
                else if (par == 0 && (c == '{' || c == ';' || c == '=')) break;
            }
            if (j < n && s[j] == '{' && nameEnd != std::string::npos && nameEnd < j) {
                size_t k = nameEnd; while (k > ls && s[k - 1] == ' ') k--;
                size_t b = k; while (b > ls && isid(s[b - 1])) b--;
                const std::string name = s.substr(b, k - b);
                // the body: brace-count, skipping strings, chars and comments
                int depth = 0; size_t q = j;
                for (; q < n; q++) {
                    const char c = s[q];
                    if (c == '/' && q + 1 < n && s[q + 1] == '/') { q = s.find('\n', q); if (q == std::string::npos) q = n; continue; }
                    if (c == '/' && q + 1 < n && s[q + 1] == '*') { q = s.find("*/", q + 2); if (q == std::string::npos) q = n; else q++; continue; }
                    if (c == '"' || c == '\'') { const char e = c; q++; while (q < n && s[q] != e) { if (s[q] == '\\') q++; q++; } continue; }
                    if (c == '{') depth++;
                    else if (c == '}') { if (--depth == 0) break; }
                }
                if (!name.empty() && q < n) { out[name] += code_only(s.substr(j, q - j + 1)); i = q + 1; continue; }
            }
        }
        i = lend + 1;
    }
    return out;
}
static void test_C12(const char *ahh)
{
    const std::string H = slurp(ahh);
    if (H.empty()) { expect("C12 needs AppleHardwareHook.cpp", false); return; }
    const auto F = parse_funcs(H);
    expect("C12 the parser finds the roots", F.count("gfxsrc_policy") && F.count("gfxsrc_lut_learn") && F.count("gfxsrc_desc_tiled_ok") &&
           F.count("gfxc_read_core") && F.count("gfxsrc_pgm_profile"));
    static const char *prims[] = { "gfxc_read_rs(", "gfxc_read(", "gfxc_read_core(", "gfxc_page(", "gfxc_read_sub(", "gfxc_page_sub(",
                                   "vram_read_sub(", "navi48_vram_read_mm(", "gfxc_census_note(" };
    const std::set<std::string> stop = { "gfxc_read_rs", "gfxc_read", "gfxc_read_core", "gfxc_page", "gfxc_read_sub", "gfxc_page_sub",
                                         "vram_read_sub", "gfxc_census_note" };
    std::set<std::string> seen; std::vector<std::string> work = { "gfxsrc_policy", "gfxsrc_lut_learn", "gfxsrc_desc_tiled_ok" };
    auto isid = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    while (!work.empty()) {
        const std::string f = work.back(); work.pop_back();
        if (seen.count(f)) continue;
        seen.insert(f);
        if (stop.count(f)) continue;
        const std::string &b = F.at(f);
        for (size_t i = 0; i < b.size(); i++) {
            if (!isid(b[i]) || (i && isid(b[i - 1]))) continue;
            size_t j = i; while (j < b.size() && isid(b[j])) j++;
            const std::string id = b.substr(i, j - i);
            size_t k = j; while (k < b.size() && b[k] == ' ') k++;
            const bool call = k < b.size() && b[k] == '(';
            const bool ref = i && b[i - 1] == '&';
            if ((call || ref) && F.count(id) && !seen.count(id)) work.push_back(id);
            i = j;
        }
    }
    std::string got;
    for (const auto &f : seen) {
        if (stop.count(f) || !F.count(f)) continue;
        const std::string &b = F.at(f);
        for (const char *pr : prims) {
            const uint32_t c = count(b, pr);
            if (c) got += f + ":" + std::string(pr, std::strlen(pr) - 1) + "=" + std::to_string(c) + "\n";
        }
    }
    std::printf("      C12 reachable functions %zu; read sites:\n%s", seen.size(), got.c_str());
    extern const char *kC12Census;
    expect("C12 X2: the read sites reachable from gfxsrc_policy / gfxsrc_lut_learn / gfxsrc_desc_tiled_ok equal the censused list",
           got == std::string(kC12Census));
}

// ================================================================================================================ C10 pins
static std::string slurp(const char *p)
{
    std::string s;
    if (!p) return s;
    FILE *f = std::fopen(p, "rb");
    if (!f) return s;
    char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t count(const std::string &s, const char *needle)
{
    uint32_t c = 0; size_t at = 0; const size_t L = std::strlen(needle);
    while ((at = s.find(needle, at)) != std::string::npos) { c++; at += L; }
    return c;
}
static std::string body(const std::string &s, const char *head)
{
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return s.substr(a, b == std::string::npos ? std::string::npos : b - a);
}
static bool before(const std::string &s, const char *x, const char *y)
{
    const size_t a = s.find(x), b = s.find(y);
    return a != std::string::npos && b != std::string::npos && a < b;
}
static void test_C10(const char *nb, const char *ahh, const char *peer, const char *cgh, const char *cmh)
{
    const std::string B = slurp(nb), H = slurp(ahh), P = slurp(peer), G = slurp(cgh), C = slurp(cmh);
    if (B.empty() || H.empty() || P.empty() || G.empty() || C.empty()) { expect("C10 needs the five source files", false); return; }
    // the switch
    expect_u("C10 PIN SWITCH-GUARD:84: the selector calls n48_cm_cont_switch_refused(84u, ...), exactly once",
             count(H, "n48_cm_cont_switch_refused(84u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)"), 1u);
    expect_u("C10 the selector: `} else if ((arg & 0xffull) == 84ull) {`, exactly once", count(H, "} else if ((arg & 0xffull) == 84ull) {"), 1u);
    expect_u("C10 the selector calls navi48_cg84_switch, exactly once", count(H, "(void)navi48_cg84_switch(m, contRefused84 ? 1u : 0u, &st);"), 1u);
    expect_u("C10 gfx_commit.h guards 84 (case 84u)", count(C, "case 84u:"), 1u);
    expect_u("C10 X3: switch 84 is OFF at boot", count(B, "static volatile uint32_t gCg84Mode { N48_CG84_OFF };"), 1u);
    expect_u("C10 X3: gCg84Mode has ONE writer (navi48_cg84_switch)", count(B, "gCg84Mode = "), 1u);
    expect_u("C10 X3: nothing writes ON at boot (no `gCg84Mode { N48_CG84_ON`)", count(B, "gCg84Mode { N48_CG84_ON"), 0u);
    // the reader
    const std::string sb = body(B, "void navi48_cg_seg_begin(void) {");
    expect_u("C10 seg_begin latches fine = n48_cg84_fine_of(m84) (SHADOW is not fine)", count(sb, "gCgSegRec.fine = n48_cg84_fine_of(m84);"), 1u);
    const std::string sc = body(B, "uint32_t navi48_cg_seg_check(void) {");
    expect("C10 seg_check: the counted check is n48_cg_check_ex, then the instrument; it returns the counted `r`",
           before(sc, "const uint32_t r = n48_cg_check_ex(&gCgSegRec,", "cg84_instrument(r, untracked);") && count(sc, "return r;") == 1u);
    const std::string ci = body(B, "static __attribute__((noinline)) void cg84_instrument(uint32_t r, uint32_t untracked) {");
    expect("C10 cg84_instrument: SHADOW notes (page r, fine rf), ON notes (page rp, fine r); it assigns no counted state",
           count(ci, "n48_cg84_note(&gCg84S, r, rf);") == 1u && count(ci, "n48_cg84_note(&gCg84S, rp, r);") == 1u &&
           count(ci, "gCgStats") == 0u && count(ci, "gCgLastWhy") == 0u && count(ci, "n48_cg_check_fx(&gCgSegRec, 1u, 1u,") == 1u &&
           count(ci, "n48_cg_check_fx(&gCgSegRec, 0u, 0u,") == 1u);
    // the check's order (item 11: ring position after the slot scan) - the header's own text
    const std::string fx = body(G, "static inline uint32_t n48_cg_check_fx(");
    expect("C10 n48_cg_check_fx: poison, THEN the slot scan, THEN the ring position, THEN the ring",
           before(fx, "n48_cg_poison_overlaps(poison,", "n48_cg_slot_scan_rec(slots,") &&
           before(fx, "n48_cg_slot_scan_rec(slots,", "const uint64_t now = n48_cg_ring_mark(ring);") &&
           before(fx, "const uint64_t now = n48_cg_ring_mark(ring);", "n48_cg_ring_scan_rec(ring,"));
    expect_u("C10 the counted check passes used 0", count(G, "return n48_cg_check_fx(rec, rec->fine, 0u, poison,"), 1u);
    // the writer's scope protocol in navi48_cg_open / navi48_cg_close
    const std::string op = body(B, "int32_t navi48_cg_open(uint64_t lo, uint64_t hi) {");
    expect("C10 navi48_cg_open: the slot is published, THEN the key bump, THEN BEGIN (with dlo/dhi)",
           before(op, "n48_cg_slot_open_d(gCgSlots,", "if (gN48D84Live) d84_open_bump(lo, hi);   // build 0.0.529 (CG84.md item 7)") &&
           before(op, "if (gN48D84Live) d84_open_bump(lo, hi);   // build 0.0.529 (CG84.md item 7)",
                  "n48_cg_ring_push_od(&gCgRing, N48_CG_EV_BEGIN, (uint32_t)slot, lo, hi, (uintptr_t)current_thread(), dlo, dhi);"));
    expect("C10 navi48_cg_open (untracked): the untracked count is published, THEN the key bump",
           before(op, "__atomic_fetch_add(&gCgUntracked, 1u, __ATOMIC_SEQ_CST);", "if (gN48D84Live) d84_open_bump(lo, hi);   // build 0.0.529: after"));
    const std::string cl = body(B, "void navi48_cg_close(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {");
    expect("C10 navi48_cg_close: END is pushed BEFORE seq goes even (n48_cg_slot_close)",
           before(cl, "n48_cg_ring_push_od(&gCgRing, N48_CG_EV_END, (uint32_t)slot, lo, hi,", "n48_cg_slot_close(gCgSlots, slot);"));
    const std::string cs = body(P, "__attribute__((noinline)) void navi48_cg_close_scope(");
    expect("C10 navi48_cg_close_scope: the shadow is published only AFTER the scope closed (navi48_cg_close, then navi48_d84_closed)",
           before(cs, "navi48_cg_close(slot, lo, hi,", "if (gN48D84Live) navi48_d84_closed(lo, hi, wrote, failed, mismatch);"));
    // the copier
    const std::string rc = body(P, "static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    expect("C10 residency_copy_to_vram: d84_plan after the pre-flight, BEFORE the copy's scope; the delta replaces the rest",
           before(rc, "Pre-flight: every destination segment", "const uint32_t d84 = d84_plan(self, dstMap, md, cgLo, cgHi, (uint64_t)dSegs | (linBytes ? (1ull << 32) : 0ull));") &&
           before(rc, "if (d84) return d84 == 1u;", "Navi48CopyScope cgScope(cgLo, cgHi);") &&
           count(body(P, "static __attribute__((noinline)) uint32_t d84_plan("), "return d84_delta_copy(self, cgLo, bytes, packed) ? 1u : 2u;") == 1u);
    expect_u("C10 residency_copy_to_vram: ONE load while 84 never left OFF (`if (__builtin_expect(gN48D84Live != 0u, 0)) {`)",
             count(rc, "if (__builtin_expect(gN48D84Live != 0u, 0)) {"), 1u);
    const std::string dc = body(P, "static __attribute__((noinline)) bool d84_delta_copy(");
    expect("C10 d84_delta_copy: the scope is n48_d84_scope(vram, ds, de) and the loop n48_d84_copy_loop over [ds, de)",
           count(dc, "n48_d84_scope(vram, ds, de, &lo, &hi);") == 1u && count(dc, "Navi48CopyScope cgScope(lo, hi);") == 1u &&
           count(dc, "n48_d84_copy_loop(nullptr, &d84_wr, &d84_rd, img, vram, ds, de, &compared, &mismatched, gD84Buf, gD84Back)") == 1u);
    expect("C10 d84_delta_copy: MM only - no fc_copy_chunk, no shadercache_scan_resource, no ic_begin, no resprov record",
           count(dc, "fc_copy_chunk") == 0u && count(dc, "shadercache_scan_resource") == 0u && count(dc, "ic_begin") == 0u &&
           count(dc, "hw_resprov_note_copy") == 0u);
    expect("C10 d84_delta_copy: the result is reported BEFORE the scope closes", before(dc, "navi48_d84_result(1u, compared, mismatched, de - ds);", "    }\n    clock_get_uptime(&t1);"));
    expect_u("C10 NOTHING OF APPLE'S IS WRITTEN: no store to res+0x138 / +0x140 / +0x258 (reads only)",
             count(P, "<uint64_t *>(r + 0x138) =") + count(P, "<uint64_t *>(r + 0x140) =") + count(P, "(r + 0x258) =") +
             count(P, "<uint64_t *>(r + 0x138)) =") , 0u);
    expect_u("C10 d84_plan_snap reads Apple's dirty union (const), once", count(P, "const uint64_t dLo = *reinterpret_cast<const uint64_t *>(r + 0x138), dLen = *reinterpret_cast<const uint64_t *>(r + 0x140);"), 1u);
    expect("C10 ic_read: the scratch replaces readBytes only for THIS thread's ON copy",
           count(P, "const uint64_t got = (d84 && d84 == (uintptr_t)current_thread()) ? navi48_d84_src(resOff, raw, take)") == 1u);
    expect_u("C10 fc_copy_chunk: an ON establishing copy is declined before navi48_fc_chunk (the MM path)",
             count(P, "if (d84 && d84 == (uintptr_t)current_thread()) return 0ull;"), 1u);
    expect_u("C10 the page-out invalidates the resource's key", count(body(P, "static uint8_t hook_page_texture("), "if (gN48D84Live) navi48_d84_pageout(self);"), 1u);
    expect_u("C10 the census note in shadercache_scan_resource", count(P, "navi48_d84_census(census.programs + (uint32_t)(pc > 0xffffffull ? 0xffffffull : pc) + ctx.nDone);"), 1u);
    // the event sites
    expect_u("C10 unmapVA invalidates by VA", count(H, "if (gN48D84Live) navi48_d84_unmap(va, size);"), 1u);
    expect_u("C10 a context release invalidates every key", count(H, "if (gN48D84Live) navi48_d84_inval_all();   // build 0.0.529 (CG84.md item 7): a context"), 1u);
    expect_u("C10 a WindowServer drop invalidates every key", count(H, "if (dropped && gN48D84Live) navi48_d84_inval_all();"), 1u);
    expect_u("C10 MF-2: a committed frame moves every key (its stores are not enumerated)", count(H, "if (commitOk && gN48D84Live) navi48_d84_commit();"), 1u);
    expect("C10 MF-2: navi48_d84_commit bumps every key and starts the 100 ms window",
           count(body(B, "void navi48_d84_commit(void) {"), "d84_bump_all_counted(&gD84T.st.commits);") == 1u &&
           count(body(B, "void navi48_d84_commit(void) {"), "__atomic_store_n(&gD84LastUnknownUs, sk82_now_us() | 1ull, __ATOMIC_SEQ_CST);") == 1u);
    // MF-1 / S3: the three unknown-write sites
    expect_u("C10 MF-1: the Apple SDMA submission site calls navi48_d84_unknown", count(body(H, "static uint64_t hook_ringWriteTail(void *self) {"), "if (gN48D84Live) navi48_d84_unknown();"), 1u);
    expect_u("C10 MF-1: un-walked client work calls navi48_d84_unknown", count(H, "if (gN48D84Live && wptr != gGfxDone) navi48_d84_unknown();"), 1u);
    expect_u("C10 MF-1: an IB let run calls navi48_d84_unknown", count(H, "if (gN48D84Live && w.ibs && (!armedNow || sparedN)) navi48_d84_unknown();"), 1u);
    const std::string un = body(B, "void navi48_d84_unknown(void) {");
    expect("C10 MF-1/S3: unknown writes move every key under ON only, and are counted under SHADOW",
           count(un, "== N48_CG84_ON) d84_bump_all_counted(&gD84T.st.unknowns);") == 1u && count(un, "__atomic_fetch_add(&gD84T.st.unknowns, 1ull, __ATOMIC_RELAXED);") == 1u);
    // S2: the producers are lock-free
    uint32_t locks = 0;
    for (const char *f : { "static inline void d84_bump_all_counted(uint64_t *ctr) {", "void navi48_d84_pageout(const void *res) {",
                           "void navi48_d84_unmap(uint64_t va, uint64_t size) {", "void navi48_d84_inval_all(void) {",
                           "void navi48_d84_commit(void) {", "void navi48_d84_unknown(void) {" }) {
        const size_t a = B.find(f);
        const size_t e = a == std::string::npos ? a : B.find("\n}\n", a);
        const std::string fb = a == std::string::npos ? std::string("MISSING gD84Lock") : B.substr(a, e - a);
        locks += count(fb, "gD84Lock") + (count(fb, "{") ? 0u : 1u);
    }
    expect_u("C10 S2: the commit, unmap, page-out, context and WindowServer producers (and MF-1's) never take gD84Lock", locks, 0u);
    // S1: the plan's three steps and the update's
    const std::string pl = body(B, "uint64_t navi48_d84_plan(const n48_d84_elig *e, const n48_d84_id *id, const n48_sk82_snap *sn, void *md,");
    expect("C10 S1: CLAIM under the lock, the lock dropped, THEN the read, the lock again, THEN decide (c0, slots)",
           before(pl, "IOLockLock(gD84Lock);", "const int32_t ki = n48_d84_claim(&gD84T, &in, id, me, &o);") &&
           before(pl, "const int32_t ki = n48_d84_claim(&gD84T, &in, id, me, &o);", "const int rdOk = d84_read_src(&src, gD84T.scratch, id->bytes);") &&
           count(pl.substr(0, pl.find("const int rdOk")), "IOLockUnlock(gD84Lock);") == 1u &&
           before(pl, "const int rdOk = d84_read_src(&src, gD84T.scratch, id->bytes);", "act = n48_d84_decide(&gD84T, &w, &in, id, sn, ki, me, &o);") &&
           count(pl, "d84_read_src(") == 1u);
    expect("C10 MF-5 / MF-3: the plan's inputs are the bound WindowServer pid and shadercache's predicate",
           count(pl, "in.wsPid = n48::hw_d84_ws_pid();") == 1u && count(pl, "in.progs = &navi48_d84_programs_in;") == 1u);
    const std::string cz = body(B, "void navi48_d84_closed(uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {");
    expect("C10 S1: the update's shadow copy (and MF-6's probe) run with the lock dropped, between BEGIN and END",
           before(cz, "go = n48_d84_closed_begin(&gD84T,", "n48_d84_shadow_copy(&gD84T);") &&
           before(cz, "n48_d84_shadow_copy(&gD84T);", "if (probeNow) bad = d84_probe(k);") &&
           before(cz, "if (probeNow) bad = d84_probe(k);", "(void)n48_d84_closed_end(&gD84T, me, bad != ~0ull ? 1u : 0u);"));
    expect("C10 MF-6: a probe mismatch invalidates every key and prints the STOP line",
           count(cz, "if (bad != ~0ull) { gD84T.st.probeBad++; (void)n48_d84_bump_all(gD84T.k, N48_D84_KEYS); n48_d84_inval_all(&gD84T); }") == 1u &&
           count(cz, "N48LOG(N48_D84_PROBE_FMT,") == 1u && count(cz, "n48_d84_probe_due(gD84T.st.deltaUpd)") == 1u);
    expect("C10 MF-3: the kext predicate is sc_lookup's, empty and no-terminator starts excepted, fail-closed otherwise",
           count(P, "if (r == SC_MISS && (m.miss_reason == SC_MISS_EMPTY || m.miss_reason == SC_MISS_NOEND)) continue;") == 1u);
    expect_u("C10 the establishing census counts program candidates, not every grid start",
             count(P, "const uint64_t pc = mat + amb + nokey + cmp + shrt;"), 1u);
    expect_u("C10 SHOULD: the delta's gCopy.lastDst is the resource's VRAM", count(P, "gCopy.lastDst = vram; gCopy.lastBytes = de - ds;"), 1u);
    // X2: the reader census
    expect_u("C10 X2 memo hits note WHOLE pages", count(H, "for (uint32_t i = 0; i < row->npages; i++) n48_cg_pagerec_note(rec, row->pages[i], 4096u);"), 1u);
    expect_u("C10 X2 the T+M miss copies its pages as WHOLE pages", count(H, "n48_cg_pagerec_note(activeRec, gPgmProfileScratchTM.page[i], 4096u);"), 1u);
    expect_u("C10 X2 the descriptor reader records exact bytes", count(H, "return gfxc_read_rs(*static_cast<const GfxcVm *>(vm), va, dst, n, nullptr, navi48_cg_active_recorder());"), 1u);
    // build 0.0.553 (switch 111): the census note follows the read's own entry (li111_learn_off(): entry 4 unless 111 ON)
    expect_u("C10 X2 lut_learn's record read notes its whole page (census), at the SAME entry it read",
             count(H, "gfxc_census_note(vm, imgVa + li111_learn_off(), N48_LUT_RECORD_DWORDS * 4u);") +
             count(H, "if (gfxc_read(vm, imgVa + li111_learn_off(), rec, N48_LUT_RECORD_DWORDS, nullptr) != N48_LUT_RECORD_DWORDS) {"), 2u);
    // the allowlist: every VRAM read inside gfxsrc_policy's own body, by kind (a new one fails this until it is censused)
    const std::string pol = body(H, "static void gfxsrc_policy(const GfxcVm &vm, n48_xv_frame *f, uint32_t n, uint64_t ibVa, uint32_t dp, uint64_t dctxKey,");
    expect_u("C10 X2 allowlist: gfxsrc_policy's body: navi48_vram_read_mm x1 (the f828 fence slot pre-read: our ring page)", count(pol, "navi48_vram_read_mm("), 1u);
    expect_u("C10 X2 allowlist: gfxsrc_policy's body: gfxc_page x1 (the R1 md walk: page tables)", count(pol, "gfxc_page("), 1u);
    expect_u("C10 X2 allowlist: gfxsrc_policy's body: no gfxc_read / gfxc_read_sub / vram_read_sub", count(pol, "gfxc_read(") + count(pol, "gfxc_read_sub(") + count(pol, "vram_read_sub("), 0u);
    const std::string ll = body(H, "static void gfxsrc_lut_learn(const GfxcVm &vm, uint64_t imgVa) {");
    expect_u("C10 X2 allowlist: gfxsrc_lut_learn: gfxc_read x1 (censused) + gfxc_page x1 (its page table)", count(ll, "gfxc_read(") * 10 + count(ll, "gfxc_page("), 11u);
    const std::string tk = body(H, "static int gfxsrc_desc_tiled_ok(void *ctx, uint64_t va, uint32_t mode, uint32_t elemBytes, const uint32_t *t10, uint32_t *clamp) {");
    expect_u("C10 X2 allowlist: gfxsrc_desc_tiled_ok: gfxc_page x1 (page tables only)", count(tk, "gfxc_page(") * 10 + count(tk, "gfxc_read("), 10u);
}

int main(int argc, char **argv)
{
    test_C1(); test_C2(); test_C3(); test_C4(); test_C5(); test_C6(); test_C7(); test_C8(); test_C9(); test_C11();
    if (argc >= 6) { test_C10(argv[1], argv[2], argv[3], argv[4], argv[5]); test_C12(argv[2]); }
    else std::printf("C10 skipped: pass Navi48Bringup.cpp AppleHardwareHook.cpp Navi48AccelPeer.cpp gfx_copyguard.h gfx_commit.h\n");
    std::printf("gfx_cg84: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}

// C12's census (generated from this build's AppleHardwareHook.cpp and reviewed: see the report's X2 table). A change here is a census
// change: re-derive the reader table first.
const char *kC12Census =
    "gfxsrc_desc_dcc_ok:gfxc_page=1\n"               // page tables (switch 18 key): never recorded, outside every copy destination
    "gfxsrc_desc_gfxc:gfxc_read_rs=1\n"              // descriptors: exact granules
    "gfxsrc_desc_tiled_ok:gfxc_page=1\n"             // page tables (switch 18 key)
    "gfxsrc_lut_learn:gfxc_read=1\n"                 // the LUT slot-4 record: census-noted (the next line)
    "gfxsrc_lut_learn:gfxc_page=1\n"                 // page tables
    "gfxsrc_lut_learn:gfxc_census_note=1\n"          // X2: the slot-4 record's whole page(s)
    "gfxsrc_pgm_profile_headfirst:gfxc_read_rs=3\n"  // program identity: exact granules
    "gfxsrc_pgm_profile_id_read:gfxc_read_rs=3\n"    // program identity: exact granules
    "gfxsrc_policy:gfxc_page=1\n"                    // R1 md walk: page tables, counted-only writable bit
    "gfxsrc_policy:navi48_vram_read_mm=1\n"          // f828 fence-slot pre-read: our own ring page
    "gfxsrc_rp_walk:gfxc_page=1\n"                   // resprov walk: page tables
    "lutfill_thread:navi48_vram_read_mm=1\n"         // the LUT fill's deferred thread (outside the pass; writes our colour)
    "xd_rb_detect:gfxc_page=1\n";                    // read-back-detect instrument: page tables
