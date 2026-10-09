// gfx_sk82_test.cpp — build 0.0.527 (an internal design note items 1-12; apple/gfx_sk82.h): switch 82, skip a byte-identical
// residency re-copy (MEASURE 338 / OFF 594 / SKIP 850).
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
//         -I src/navi48-bringup/src/apple -I src/xlat12 -I src/navi48-bringup/tests src/navi48-bringup/tests/gfx_sk82_test.cpp \
//         -o /tmp/sk82 && /tmp/sk82 src/navi48-bringup/src/apple/Navi48AccelPeer.cpp src/navi48-bringup/src/Navi48Bringup.cpp \
//         src/navi48-bringup/src/apple/AppleHardwareHook.cpp src/navi48-bringup/src/apple/gfx_commit.h
// Covers:
//   S1 the selector: M1 MEASURE (338), M2 OFF (594), M3 SKIP (850), bare 82 reads, any other M refused;
//   S2 every log line <= 491 bytes at every field's widest;
//   S3 the counters' arithmetic: one/two buckets, 64 vs 65 buckets, past the array, empty and match-all ranges -> everything;
//   S4 the flow: establish from a fully read-back copy, then SKIP skips and MEASURE only counts; a skip moves no counter, opens no
//      slot, touches no poison row, pushes no event and leaves no pending; MEASURE never skips and never asks for FULL;
//   S5 a difference only at 0x7ffc copies (DIFFER first = last = 0x7ffc);
//   S6 every item-10 refusal: a concurrent open, a close-before-establishment interleave, poison, unmap (and a non-overlapping one
//      that does not refuse), a committed frame, a dstMem change, a switch change, a sampled-verify establishment (no entry; SKIP
//      makes the next equal copy a CANDIDATE), the everything counter, an untracked copy, a taint, a source change during the copy,
//      BUSY at G0, an event-ring wrap, a racing second copy, eviction (LRU), and a mode change (reset);
//   S7 the ordering proof (gfx_sk82.h DEVIATION D1): a foreign writer's four steps (slot open, bump, write, close) placed at every
//      position of our G0, copy, establishment and skip, 1001 schedules x 3 writers: never a skip over VRAM that differs from the
//      image; positive controls;
//   S8 the RUN Y replay (run11g, 7986 copies, SKIP82.md item 10): the first 128 bytes as the content proxy, every logged copy's
//      range as counter bumps; the skip count, the bucket bumps from [0x10018000, 0x10020000), the 33 collisions' mapping; and
//      the same replay with X2's unknown-write rule (the 600 committed frames) and the 384 unmaps as events;
//   S9 source pins (Navi48AccelPeer.cpp, Navi48Bringup.cpp, AppleHardwareHook.cpp, gfx_commit.h): the decision before
//      rp_lin_prepare and the scope; the skip path does nothing else; the result before, the establishment after the scope's
//      close; the FULL flag at the first chunk; the counters before the slot opens and before it closes; no allocation in the copy
//      path; every event site; the taints; the switch OFF at boot, written in one place, mid-arm guarded; OFF = one load.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_sk82.h"
#include "fixture_sk82_run11g.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-110s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-110s %#llx\n", what, (unsigned long long)got);
}

// ------------------------------------------------------------------------------------------------------------------ the test world
struct TW {
    uint32_t *cnt; uint64_t nb; uint64_t every;
    n48_cg_slot slots[N48_CG_SLOTS]; uint32_t untracked;
    n48_cg_poison poison; n48_sk82_evring ev; uint32_t busy;
    n48_sk82_tab t; uint8_t *pool; n48_sk82_world w; n48_sk82_snap snap;
    void (*stepFn)(TW *, uint32_t); void *stepArg;
};
static uint32_t tw_busy(void *c) { return static_cast<TW *>(c)->busy; }
static void tw_step(void *c, uint32_t at) { TW *x = static_cast<TW *>(c); if (x->stepFn) x->stepFn(x, at); }
static TW *tw_new(uint64_t nb = 1ull << 16)
{
    TW *x = static_cast<TW *>(std::calloc(1, sizeof(TW)));
    x->nb = nb; x->cnt = static_cast<uint32_t *>(std::calloc(nb, sizeof(uint32_t)));
    x->pool = static_cast<uint8_t *>(std::calloc(N48_SK82_ENTRIES, N48_SK82_MAX_BYTES));
    for (uint32_t i = 0; i < N48_SK82_ENTRIES; i++) x->t.e[i].img = x->pool + (size_t)i * N48_SK82_MAX_BYTES;
    n48_cg_slot_init(x->slots, N48_CG_SLOTS); n48_cg_poison_init(&x->poison);
    x->w.cnt = x->cnt; x->w.nb = nb; x->w.every = &x->every; x->w.slots = x->slots; x->w.nslots = N48_CG_SLOTS;
    x->w.untracked = &x->untracked; x->w.poison = &x->poison; x->w.ev = &x->ev; x->w.busy = &tw_busy; x->w.busyCtx = x;
    x->w.step = &tw_step; x->w.stepCtx = x;
    n48_sk82_reset(&x->t, &x->ev);
    return x;
}
static void tw_free(TW *x) { std::free(x->cnt); std::free(x->pool); std::free(x); }
// The kext's writer, step for step: navi48_cg_open publishes the slot (or the untracked count) and THEN bumps (D1); navi48_cg_close
// releases it. S9 pins that order in Navi48Bringup.cpp.
static int32_t wr_open(TW *x, uint64_t lo, uint64_t hi, uintptr_t thr)
{
    const int32_t s = n48_cg_slot_open(x->slots, N48_CG_SLOTS, lo, hi, thr);
    if (s < 0) x->untracked++;
    n48_sk82_bump(x->cnt, x->nb, &x->every, lo, hi);
    if (s < 0) x->every++;
    return s;
}
static void wr_close(TW *x, int32_t s, uint64_t, uint64_t)
{
    if (s < 0) x->untracked--; else n48_cg_slot_close(x->slots, s);
}
static n48_sk82_key key_of(uint64_t res, uint64_t vram, uint64_t bytes, uint64_t va)
{
    n48_sk82_key k {}; k.res = res; k.dstMem = res + 0x1000; k.vram = vram; k.bytes = bytes; k.boff = 0; k.va = va; k.pid = 100;
    return k;
}
// One residency copy through the pure half exactly as the kext drives it: the decision (G0), the scope's open, the write, the
// result, the scope's close, the establishment (post = the source re-read; default the same). `full`: read back in full.
struct CopyRes { uint32_t act, established, cause; n48_sk82_out o; };
static CopyRes run_copy(TW *x, uint32_t mode, const n48_sk82_key &k, const uint8_t *src, uint32_t full, uint8_t *vram = nullptr,
                        uintptr_t thr = 1u, const uint8_t *post = nullptr, uint32_t taint = 0u)
{
    CopyRes r {};
    n48_sk82_try(&x->t, &x->w, mode, &k, &x->snap, src, thr, 7u, nullptr, &r.o);
    r.act = r.o.act;
    if (r.act == N48_SK82_A_SKIP) return r;
    const int32_t s = wr_open(x, k.vram, k.vram + k.bytes, thr);
    if (vram) std::memcpy(vram, src, (size_t)k.bytes);
    if (taint) n48_sk82_taint(&x->t, thr);
    n48_sk82_result(&x->t, thr, (full || r.o.cand) ? 1u : 0u, k.bytes);
    wr_close(x, s, k.vram, k.vram + k.bytes);
    r.established = n48_sk82_establish(&x->t, &x->w, thr, k.vram, k.vram + k.bytes, &x->snap, post ? post : src, &r.cause);
    return r;
}

// ------------------------------------------------------------------------------------------------------------------------ S1
static void s1_selector()
{
    expect_u("S1 338 = 82 | 1 << 8 is MEASURE", n48_sk82_mode_of_m(338u >> 8) == N48_SK82_MEASURE && (338u & 0xffu) == 82u, 1u);
    expect_u("S1 594 = 82 | 2 << 8 is OFF (0)", n48_sk82_mode_of_m(594u >> 8), N48_SK82_OFF);
    expect_u("S1 850 = 82 | 3 << 8 is SKIP", n48_sk82_mode_of_m(850u >> 8), N48_SK82_SKIP);
    expect_u("S1 bare 82 reads", n48_sk82_mode_of_m(0u), N48_SK82_M_READ);
    uint32_t bad = 0;
    for (uint32_t m = 4; m < 256; m++) if (n48_sk82_mode_of_m(m) != N48_SK82_M_BAD) bad++;
    expect_u("S1 every other M (4..255) is refused", bad, 0u);
    expect_u("S1 OFF is 0 (a zero-initialised mode is OFF)", N48_SK82_OFF, 0u);
}

// ------------------------------------------------------------------------------------------------------------------------ S2
static void s2_widths()
{
    char buf[2048];
    const unsigned long long M = ~0ull;
    // build 0.0.528 item 5b: + dirtyStart and dirtyLen (res+0x138, res+0x140), every field at its widest.
    int w = std::snprintf(buf, sizeof buf, N48_SK82_SKIP_FMT, "WOULD-SKIP (MEASURE)", M, (void *)~(uintptr_t)0, M, M, M, M, M, M, M, M);
    std::printf("      %d: %s\n", w, buf);
    expect_u("S2 SKIP / WOULD-SKIP line <= 491 bytes at its widest (with dirtyStart and dirtyLen)", w > 0 && w <= 491, 1u);
    w = std::snprintf(buf, sizeof buf, N48_SK82_DIFFER_FMT, (void *)~(uintptr_t)0, M, M, M, M, M, M, M);
    std::printf("      %d: %s\n", w, buf);
    expect_u("S2 DIFFER line <= 491 bytes at its widest (with dirtyStart and dirtyLen)", w > 0 && w <= 491, 1u);
    const char *how = " - REFUSED: a continuous arm stands";
    w = std::snprintf(buf, sizeof buf, N48_SK82_REPORT1_FMT, n48_sk82_mode_name(N48_SK82_MEASURE), how, M, M, M, M, M, M, M, M, M,
                      M, M, M, M, "not allocated");
    std::printf("      %d: %s\n", w, buf);
    expect_u("S2 report line 1 <= 491 bytes at its widest (longest mode name, longest verb suffix)", w > 0 && w <= 491, 1u);
    n48_sk82_stats s {};
    for (uint32_t i = 0; i < N48_SK82_CAUSES; i++) s.inval[i] = ~0ull;
    s.readFail = s.noThr = ~0ull;
    w = std::snprintf(buf, sizeof buf, N48_SK82_REPORT2_FMT, N48_SK82_REPORT2_ARGS(&s));
    std::printf("      %d: %s\n", w, buf);
    expect_u("S2 report line 2 <= 491 bytes at its widest", w > 0 && w <= 491, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ S3
static void s3_counters()
{
    std::vector<uint32_t> c(128, 0u); uint64_t ev = 0;
    n48_sk82_bump(c.data(), 128, &ev, 0x100000ull, 0x100004ull);                         // bucket 0x10 only
    n48_sk82_bump(c.data(), 128, &ev, 0x1fff0ull, 0x20010ull);                           // buckets 1 and 2
    expect_u("S3 one bucket; a range across a boundary bumps both", c[0x10] == 1u && c[1] == 1u && c[2] == 1u && ev == 0u, 1u);
    n48_sk82_bump(c.data(), 128, &ev, 0, 64ull << 16);                                    // exactly 64 buckets
    expect_u("S3 64 buckets are bumped one by one", c[0] == 1u && c[63] == 1u && c[64] == 0u && ev == 0u, 1u);
    n48_sk82_bump(c.data(), 128, &ev, 0, 65ull << 16);
    expect_u("S3 65 buckets -> the everything counter instead", ev == 1u && c[0] == 1u && c[64] == 0u, 1u);
    n48_sk82_bump(c.data(), 128, &ev, 127ull << 16, 129ull << 16);
    expect_u("S3 a bucket past the array -> everything", ev == 2u && c[127] == 0u, 1u);
    n48_sk82_bump(c.data(), 128, &ev, 5, 5);
    n48_sk82_bump(c.data(), 128, &ev, 0ull, ~0ull);
    expect_u("S3 an empty range and the match-all range -> everything", ev, 4u);
    n48_sk82_bump(nullptr, 128, &ev, 0, 16);
    expect_u("S3 no array (never ON): nothing at all", ev, 4u);
    expect_u("S3 buckets of [0x10010000, +0x8000) = 1, [0x1000fffc, +8) = 2", n48_sk82_nbk(0x10010000ull, 0x8000) == 1u &&
             n48_sk82_nbk(0x1000fffcull, 8) == 2u && n48_sk82_nbk(0x10010000ull, 0x10000) == 1u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ S4
static const uint64_t kV = 0x10010000ull, kB = 0x8000ull, kVa = 0x4000b0000ull, kRes = 0xffffff99e1452f00ull;
static void fill(uint8_t *p, uint64_t n, uint32_t seed) { for (uint64_t i = 0; i < n; i++) p[i] = (uint8_t)(seed * 131u + i * 7u + (i >> 9)); }
static void s4_flow()
{
    TW *x = tw_new();
    std::vector<uint8_t> src(kB), vram(kB);
    fill(src.data(), kB, 1);
    const n48_sk82_key k = key_of(kRes, kV, kB, kVa);
    CopyRes r = run_copy(x, N48_SK82_SKIP, k, src.data(), 1u, vram.data());
    expect_u("S4 first copy: no entry, COPY, a pending, established (read back in full)",
             r.act == N48_SK82_A_COPY && r.o.pend == 1u && r.established == 1u && x->t.st.noEntry == 1u && x->t.st.established == 1u, 1u);
    // snapshot everything a skip must not touch
    const uint32_t c0 = x->cnt[kV >> 16]; const uint64_t e0 = x->every, ev0 = x->ev.next;
    const n48_cg_poison p0 = x->poison;
    r = run_copy(x, N48_SK82_SKIP, k, src.data(), 0u, vram.data());
    expect_u("S4 SKIP: the same source over a TRUSTED entry is SKIPPED", r.act, N48_SK82_A_SKIP);
    expect_u("S4 the skip moved no counter, no everything, pushed no event, touched no poison row",
             x->cnt[kV >> 16] == c0 && x->every == e0 && x->ev.next == ev0 && std::memcmp(&p0, &x->poison, sizeof p0) == 0, 1u);
    expect_u("S4 the skip opened no slot and left no pending", n48_cg_live_open(x->slots, N48_CG_SLOTS, 0) == 0u &&
             n48_sk82_thr_find(&x->t, 1u) < 0 && !x->t.e[r.o.ent].pend, 1u);
    expect_u("S4 the skip names the establishing copy and counts its bytes", r.o.fromCopy == 7u && x->t.st.skipped == 1u &&
             x->t.st.skippedBytes == kB, 1u);
    // MEASURE over the same TRUSTED entry: would skip, copies anyway, never a candidate
    TW *y = tw_new();
    run_copy(y, N48_SK82_MEASURE, k, src.data(), 1u);
    r = run_copy(y, N48_SK82_MEASURE, k, src.data(), 0u);
    expect_u("S4 MEASURE: every check passes -> WOULD skip, but COPIES", r.act == N48_SK82_A_COPY && r.o.would == 1u &&
             y->t.st.wouldSkip == 1u && y->t.st.skipped == 0u, 1u);
    expect_u("S4 MEASURE never asks for FULL (no candidate), and a sampled copy then establishes nothing",
             r.o.cand == 0u && r.established == 0u && y->t.st.candidate == 0u, 1u);
    r = run_copy(y, N48_SK82_MEASURE, k, src.data(), 0u);
    expect_u("S4 MEASURE: after a sampled copy the entry is only a shadow - equal counted, no would-skip",
             r.o.cmp == N48_SK82_CMP_EQUAL && r.o.would == 0u && y->t.st.equal == 2u, 1u);
    tw_free(y);
    tw_free(x);
}

// ------------------------------------------------------------------------------------------------------------------------ S5
static void s5_tail()
{
    TW *x = tw_new();
    std::vector<uint8_t> a(kB), b(kB);
    fill(a.data(), kB, 2); b = a; b[0x7ffc] ^= 0x40u;
    const n48_sk82_key k = key_of(kRes, kV, kB, kVa);
    run_copy(x, N48_SK82_SKIP, k, a.data(), 1u);
    CopyRes r = run_copy(x, N48_SK82_SKIP, k, b.data(), 0u);
    expect_u("S5 a difference ONLY at 0x7ffc copies", r.act, N48_SK82_A_COPY);
    expect_u("S5 DIFFER first 0x7ffc last 0x7ffc ndiff 1, logged", r.o.cmp == N48_SK82_CMP_DIFFER && r.o.first == 0x7ffcu &&
             r.o.last == 0x7ffcu && r.o.ndiff == 1u && r.o.logIt == 1u, 1u);
    b = a; b[3] ^= 1u; b[0x4000] ^= 1u; b[kB - 1] ^= 1u;
    run_copy(x, N48_SK82_SKIP, k, a.data(), 1u);
    r = run_copy(x, N48_SK82_SKIP, k, b.data(), 0u);
    expect_u("S5 three scattered bytes: first 3, last 0x7fff, ndiff 3", r.act == N48_SK82_A_COPY && r.o.first == 3u &&
             r.o.last == kB - 1u && r.o.ndiff == 3u, 1u);
    tw_free(x);
}

// ------------------------------------------------------------------------------------------------------------------------ S6
struct Fx { TW *x; std::vector<uint8_t> src; n48_sk82_key k; };
static Fx fx_trusted(uint32_t mode = N48_SK82_SKIP)
{
    Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 3); f.k = key_of(kRes, kV, kB, kVa);
    const CopyRes r = run_copy(f.x, mode, f.k, f.src.data(), 1u);
    if (!r.established) { gFail++; std::printf("FAIL  fixture: the first copy did not establish (cause %u)\n", r.cause); }
    return f;
}
static uint32_t try_only(Fx &f, uint32_t mode = N48_SK82_SKIP, const n48_sk82_key *kk = nullptr)
{
    n48_sk82_out o {};
    n48_sk82_try(&f.x->t, &f.x->w, mode, kk ? kk : &f.k, &f.x->snap, f.src.data(), 2u, 8u, nullptr, &o);
    return o.act;
}
static void s6_refusals()
{
    { Fx f = fx_trusted(); const int32_t s = n48_cg_slot_open(f.x->slots, N48_CG_SLOTS, kV + 0x100, kV + 0x200, 99u);
      expect_u("S6 a concurrent OPEN overlapping scope: no skip", try_only(f), N48_SK82_A_COPY);
      expect_u("S6   counted OPEN, and G0 set up no pending (T0 saw the slot)", f.x->t.st.inval[N48_SK82_C_OPEN] >= 2u &&
               n48_sk82_thr_find(&f.x->t, 2u) < 0, 1u);
      n48_cg_slot_close(f.x->slots, s); tw_free(f.x); }
    { Fx f = fx_trusted(); f.x->untracked = 1u;
      expect_u("S6 an UNTRACKED copy running: no skip (OPEN)", try_only(f), N48_SK82_A_COPY); tw_free(f.x); }
    { // close-before-establishment: a foreign scope opens after our G0 and closes before the establishment
      Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 4); f.k = key_of(kRes, kV, kB, kVa);
      n48_sk82_out o {};
      n48_sk82_try(&f.x->t, &f.x->w, N48_SK82_SKIP, &f.k, &f.x->snap, f.src.data(), 1u, 7u, nullptr, &o);
      const int32_t fs = wr_open(f.x, kV + 0x4000, kV + 0x4100, 55u); wr_close(f.x, fs, kV + 0x4000, kV + 0x4100);
      const int32_t s = wr_open(f.x, kV, kV + kB, 1u); n48_sk82_result(&f.x->t, 1u, 1u, kB); wr_close(f.x, s, kV, kV + kB);
      uint32_t cause = 0;
      expect_u("S6 a foreign scope opened AND closed between G0 and the establishment: NOT established (COUNTER)",
               n48_sk82_establish(&f.x->t, &f.x->w, 1u, kV, kV + kB, &f.x->snap, f.src.data(), &cause) == 0u &&
               cause == N48_SK82_C_COUNTER, 1u);
      expect_u("S6   and the next equal copy does not skip", try_only(f), N48_SK82_A_COPY); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_cg_poison_mark(&f.x->poison, kV + 0x7000, kV + 0x7004);
      expect_u("S6 a POISON row inside the range: no skip", try_only(f) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_POISON] == 1u, 1u);
      CopyRes r = run_copy(f.x, N48_SK82_SKIP, f.k, f.src.data(), 1u);
      expect_u("S6   and poison refuses the establishment too", r.established == 0u && r.cause == N48_SK82_C_POISON, 1u);
      tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_ev_push(&f.x->ev, N48_SK82_EV_UNMAP, kVa + 0x7000, 0x1000);
      expect_u("S6 an UNMAP overlapping the VA: no skip (UNMAP)", try_only(f) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_UNMAP] == 1u, 1u); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_ev_push(&f.x->ev, N48_SK82_EV_UNMAP, kVa + kB, 0x1000);
      n48_sk82_ev_push(&f.x->ev, N48_SK82_EV_UNMAP, kVa - 0x1000, 0x1000);
      expect_u("S6 unmaps just past and just before the VA do NOT refuse (range-scoped)", try_only(f), N48_SK82_A_SKIP);
      n48_sk82_ev_push(&f.x->ev, N48_SK82_EV_UNMAP, 0x123000, 0);
      expect_u("S6 an unmap of size 0 (the whole context) refuses", try_only(f), N48_SK82_A_COPY); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_ev_push(&f.x->ev, N48_SK82_EV_COMMIT, 0, 0);
      expect_u("S6 a COMMITTED frame (target / unknown write): no skip (TARGET)", try_only(f) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_TARGET] == 1u, 1u); tw_free(f.x); }
    for (uint32_t kind : { (uint32_t)N48_SK82_EV_RUN, (uint32_t)N48_SK82_EV_SDMA, (uint32_t)N48_SK82_EV_WS, (uint32_t)N48_SK82_EV_CTX }) {
      Fx f = fx_trusted(); n48_sk82_ev_push(&f.x->ev, kind, 0, 0);
      char l[128]; std::snprintf(l, sizeof l, "S6 event kind %u (ring escape / SDMA / WS drop / context release): no skip", kind);
      expect_u(l, try_only(f), N48_SK82_A_COPY); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_key k2 = f.k; k2.dstMem += 0x40;
      expect_u("S6 a dstMem change: no skip (KEY)", try_only(f, N48_SK82_SKIP, &k2) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_KEY] == 1u, 1u);
      expect_u("S6   and the original key does not skip either (the entry was replaced)", try_only(f), N48_SK82_A_COPY);
      tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_key k2 = f.k; k2.va += 0x1000;
      expect_u("S6 a VA change: no skip", try_only(f, N48_SK82_SKIP, &k2), N48_SK82_A_COPY); tw_free(f.x); }
    for (uint32_t i = 0; i < N48_SK82_SNAP_N; i++) {
      Fx f = fx_trusted(); f.x->snap.v[i] ^= 1u;
      char l[128]; std::snprintf(l, sizeof l, "S6 a switch/state change (snapshot field %u): no skip (STATE)", i);
      expect_u(l, try_only(f) == N48_SK82_A_COPY && f.x->t.st.inval[N48_SK82_C_STATE] == 1u, 1u); tw_free(f.x); }
    { Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 5); f.k = key_of(kRes, kV, kB, kVa);
      CopyRes r = run_copy(f.x, N48_SK82_SKIP, f.k, f.src.data(), 0u);
      expect_u("S6 a SAMPLED-verify copy establishes nothing (not-full)", r.established == 0u && f.x->t.st.notFull == 1u &&
               f.x->t.e[r.o.ent].state == N48_SK82_S_SHADOW, 1u);
      r = run_copy(f.x, N48_SK82_SKIP, f.k, f.src.data(), 0u);
      expect_u("S6   SKIP: the next equal copy is a CANDIDATE (FULL) and copies; it establishes", r.act == N48_SK82_A_COPY &&
               r.o.cand == 1u && r.established == 1u && f.x->t.st.candidate == 1u, 1u);
      expect_u("S6   the one after that skips", try_only(f), N48_SK82_A_SKIP); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_bump(f.x->cnt, f.x->nb, &f.x->every, 0ull, ~0ull);
      expect_u("S6 the EVERYTHING counter moved: no skip (EVERYTHING)", try_only(f) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_EVERY] == 1u, 1u); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_bump(f.x->cnt, f.x->nb, &f.x->every, kV + 0xf000, kV + 0xf004);
      expect_u("S6 one of our writes in the same 64 KiB bucket: no skip (COUNTER)", try_only(f) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_COUNTER] == 1u, 1u); tw_free(f.x); }
    { Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 6); f.k = key_of(kRes, kV, kB, kVa);
      CopyRes r = run_copy(f.x, N48_SK82_SKIP, f.k, f.src.data(), 1u, nullptr, 1u, nullptr, 1u);
      expect_u("S6 a TAINTED copy (shadercache / kernsub wrote inside its scope) establishes nothing", r.established == 0u &&
               r.cause == N48_SK82_C_TAINT, 1u); tw_free(f.x); }
    { Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 7); f.k = key_of(kRes, kV, kB, kVa);
      std::vector<uint8_t> post = f.src; post[0x1234] ^= 0x80u;
      CopyRes r = run_copy(f.x, N48_SK82_SKIP, f.k, f.src.data(), 1u, nullptr, 1u, post.data());
      expect_u("S6 the source CHANGED during the copy (post != pre): not established (SRC)", r.established == 0u &&
               r.cause == N48_SK82_C_SRC, 1u); tw_free(f.x); }
    { Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 8); f.k = key_of(kRes, kV, kB, kVa); f.x->busy = 1u;
      CopyRes r = run_copy(f.x, N48_SK82_SKIP, f.k, f.src.data(), 1u);
      expect_u("S6 BUSY at G0 (a committed frame in flight / a recent unknown write): no pending, nothing established",
               r.o.pend == 0u && r.established == 0u && f.x->t.st.inval[N48_SK82_C_FLIGHT] == 1u, 1u); tw_free(f.x); }
    { Fx f = fx_trusted(); for (uint32_t i = 0; i < N48_SK82_EV + 1u; i++) n48_sk82_ev_push(&f.x->ev, N48_SK82_EV_UNMAP, 0x999000, 0x1000);
      expect_u("S6 the event ring WRAPPED (257 non-overlapping unmaps): no skip (WRAP)", try_only(f) == N48_SK82_A_COPY &&
               f.x->t.st.inval[N48_SK82_C_WRAP] >= 1u, 1u); tw_free(f.x); }
    { // a racing second copy of the same entry: the first's establishment must fail
      Fx f; f.x = tw_new(); f.src.resize(kB); fill(f.src.data(), kB, 9); f.k = key_of(kRes, kV, kB, kVa);
      n48_sk82_out o1 {}, o2 {};
      n48_sk82_try(&f.x->t, &f.x->w, N48_SK82_SKIP, &f.k, &f.x->snap, f.src.data(), 1u, 7u, nullptr, &o1);
      n48_sk82_try(&f.x->t, &f.x->w, N48_SK82_SKIP, &f.k, &f.x->snap, f.src.data(), 2u, 8u, nullptr, &o2);
      const int32_t s1 = wr_open(f.x, kV, kV + kB, 1u); n48_sk82_result(&f.x->t, 1u, 1u, kB); wr_close(f.x, s1, kV, kV + kB);
      uint32_t c1 = 0, c2 = 0;
      const uint32_t e1 = n48_sk82_establish(&f.x->t, &f.x->w, 1u, kV, kV + kB, &f.x->snap, f.src.data(), &c1);
      const int32_t s2 = wr_open(f.x, kV, kV + kB, 2u); n48_sk82_result(&f.x->t, 2u, 1u, kB); wr_close(f.x, s2, kV, kV + kB);
      const uint32_t e2 = n48_sk82_establish(&f.x->t, &f.x->w, 2u, kV, kV + kB, &f.x->snap, f.src.data(), &c2);
      expect_u("S6 two copies of one entry: the first loses its pending (RACE), the second sees the first's bumps (COUNTER)",
               e1 == 0u && c1 == N48_SK82_C_RACE && e2 == 0u && c2 == N48_SK82_C_COUNTER, 1u); tw_free(f.x); }
    { Fx f = fx_trusted();
      std::vector<uint8_t> s2(kB); fill(s2.data(), kB, 10);
      for (uint32_t i = 1; i <= N48_SK82_ENTRIES; i++) run_copy(f.x, N48_SK82_SKIP, key_of(0x1000000ull * i, 0x20000000ull + (uint64_t)i * 0x10000u, kB, 0x500000000ull + i * 0x10000u), s2.data(), 1u);
      expect_u("S6 17 resources in 16 entries: the LRU (the first) was evicted", f.x->t.st.evicted, 1u);
      const uint64_t ne = f.x->t.st.noEntry;
      expect_u("S6   it finds no entry and copies again", try_only(f) == N48_SK82_A_COPY && f.x->t.st.noEntry == ne + 1u, 1u);
      tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_reset(&f.x->t, &f.x->ev);
      expect_u("S6 a mode change resets the table: no skip afterwards", try_only(f), N48_SK82_A_COPY); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_out o {};
      n48_sk82_try(&f.x->t, &f.x->w, N48_SK82_OFF, &f.k, &f.x->snap, f.src.data(), 2u, 8u, nullptr, &o);
      expect_u("S6 OFF decides nothing (never a skip, never an entry touched)", o.act == N48_SK82_A_COPY && o.cmp == 0u &&
               f.x->t.st.eligible == 1u, 1u); tw_free(f.x); }
    { Fx f = fx_trusted(); n48_sk82_out o {};
      n48_sk82_try(&f.x->t, &f.x->w, N48_SK82_SKIP, &f.k, &f.x->snap, nullptr, 2u, 8u, nullptr, &o);
      expect_u("S6 an ineligible copy (or a failed source read: no image) never skips", o.act, N48_SK82_A_COPY); tw_free(f.x); }
}

// ------------------------------------------------------------------------------------------------------------------------ S7
// A foreign writer W over [wlo, whi) takes the kext's four steps in order - S (slot open, or untracked U+), B (bump), WRITE, SC
// (slot close, or U-). Our timeline has positions 0..10:
//   0 before our G0 | 1 inside G0 (after C0, before T0) | 2 after G0 | 3 after our scope opened | 4 after our write
//   5 after our scope closed | 6 inside the establishment (after T1, before C1) | 7 after the establishment
//   8 inside the skip (after T, before C) | 9 after the skip decision | 10 never (W's remaining steps do not happen)
// A schedule is a non-decreasing assignment of W's steps to positions. FAIL-OPEN = the skip returned SKIP while VRAM (our copy's
// bytes X, W's bytes Y where W wrote after us, over the overlap) differs from the image when the skip decided.
#define S7_STEPS 4u
struct S7 { TW *x; uint32_t pos[S7_STEPS]; uint32_t done; int32_t wslot; uint64_t wlo, whi; bool untr; bool vramBad; bool decided;
            bool contractOrder; };
static S7 *gS7;
static void s7_do(S7 *s, uint32_t upTo)
{
    while (s->done < S7_STEPS && s->pos[s->done] <= upTo) {
        // contractOrder (SKIP82.md item 3 as written, the NEGATIVE CONTROL): the bump is step 0 and the slot's open step 1
        const uint32_t st = s->contractOrder && s->done < 2u ? 1u - s->done : s->done;
        switch (st) {
        case 0: if (s->untr) s->x->untracked++; else s->wslot = n48_cg_slot_open(s->x->slots, N48_CG_SLOTS, s->wlo, s->whi, 77u); break;
        case 1: n48_sk82_bump(s->x->cnt, s->x->nb, &s->x->every, s->wlo, s->whi); if (s->untr) s->x->every++; break;
        case 2: if (n48_cg_overlap(s->wlo, s->whi, kV, kV + kB) && !s->decided) s->vramBad = true; break;
        case 3: if (s->untr) s->x->untracked--; else n48_cg_slot_close(s->x->slots, s->wslot); break;
        }
        s->done++;
    }
}
static void s7_step(TW *, uint32_t at)
{
    // at: G0_MID -> position 1, EST_MID -> 6, SKIP_MID -> 8
    s7_do(gS7, at == N48_SK82_AT_G0_MID ? 1u : at == N48_SK82_AT_EST_MID ? 6u : 8u);
}
static void s7_one(const uint32_t pos[S7_STEPS], uint64_t wlo, uint64_t whi, bool untr, uint32_t *failOpen, uint32_t *skips, uint32_t *estab,
                   bool contractOrder = false)
{
    S7 s {}; s.x = tw_new(); std::memcpy(s.pos, pos, sizeof s.pos); s.wlo = wlo; s.whi = whi; s.untr = untr; s.wslot = -1;
    s.contractOrder = contractOrder;
    gS7 = &s; s.x->stepFn = &s7_step;
    std::vector<uint8_t> src(kB); fill(src.data(), kB, 11);
    const n48_sk82_key k = key_of(kRes, kV, kB, kVa);
    n48_sk82_out o {};
    s7_do(&s, 0u);
    n48_sk82_try(&s.x->t, &s.x->w, N48_SK82_SKIP, &k, &s.x->snap, src.data(), 1u, 7u, nullptr, &o);   // G0 (calls position 1)
    s7_do(&s, 2u);
    const int32_t sl = wr_open(s.x, kV, kV + kB, 1u);
    s7_do(&s, 3u);
    s.vramBad = false;                       // our write: VRAM = X (anything W wrote before is overwritten)
    s7_do(&s, 4u);
    n48_sk82_result(&s.x->t, 1u, 1u, kB);
    wr_close(s.x, sl, kV, kV + kB);
    s7_do(&s, 5u);
    uint32_t cause = 0;
    const uint32_t est = n48_sk82_establish(&s.x->t, &s.x->w, 1u, kV, kV + kB, &s.x->snap, src.data(), &cause);   // position 6
    s7_do(&s, 7u);
    n48_sk82_out o2 {};
    n48_sk82_try(&s.x->t, &s.x->w, N48_SK82_SKIP, &k, &s.x->snap, src.data(), 2u, 8u, nullptr, &o2);   // position 8 inside
    const bool bad = s.vramBad;             // what VRAM held when the skip decided
    s.decided = true;
    s7_do(&s, 9u);
    if (o2.act == N48_SK82_A_SKIP) (*skips)++;
    if (est) (*estab)++;
    if (o2.act == N48_SK82_A_SKIP && bad) {
        (*failOpen)++;
        if (!contractOrder) std::printf("      S7 FAIL-OPEN schedule S@%u B@%u WRITE@%u SC@%u (%s, [%#llx,%#llx)) established %u\n", pos[0], pos[1],
                    pos[2], pos[3], untr ? "untracked" : "tracked", (unsigned long long)wlo, (unsigned long long)whi, est);
    }
    tw_free(s.x);
}
static void s7_orders()
{
    uint32_t n = 0, fo = 0, sk = 0, es = 0, foU = 0, skU = 0, esU = 0, foB = 0, skB = 0, esB = 0;
    uint32_t p[S7_STEPS];
    for (p[0] = 0; p[0] <= 10; p[0]++) for (p[1] = p[0]; p[1] <= 10; p[1]++) for (p[2] = p[1]; p[2] <= 10; p[2]++)
    for (p[3] = p[2]; p[3] <= 10; p[3]++) {
        n++;
        s7_one(p, kV + 0x3000, kV + 0x3100, false, &fo, &sk, &es);        // overlapping our bytes, tracked
        s7_one(p, kV + 0x3000, kV + 0x3100, true, &foU, &skU, &esU);      // overlapping, untracked
        s7_one(p, kV + 0x9000, kV + 0x9100, false, &foB, &skB, &esB);     // same bucket, NOT overlapping our bytes
    }
    std::printf("      S7 %u schedules x 3 writers: skips %u/%u/%u, establishments %u/%u/%u\n", n, sk, skU, skB, es, esU, esB);
    expect_u("S7 1001 schedules of W's four steps over our eleven positions", n, 1001u);
    expect_u("S7 a TRACKED overlapping writer: never a skip over VRAM that differs from the image", fo, 0u);
    expect_u("S7 an UNTRACKED overlapping writer: never a skip over differing VRAM", foU, 0u);
    expect_u("S7 a same-bucket non-overlapping writer: never a skip over differing VRAM", foB, 0u);
    // positive controls: W entirely before our G0 (all at 0) and entirely after the skip (all at 9/10) -> the skip happens
    uint32_t a = 0, b = 0, c = 0;
    const uint32_t before[S7_STEPS] = { 0, 0, 0, 0 }, after[S7_STEPS] = { 9, 9, 9, 9 }, never[S7_STEPS] = { 10, 10, 10, 10 };
    s7_one(before, kV + 0x3000, kV + 0x3100, false, &a, &b, &c);
    s7_one(after, kV + 0x3000, kV + 0x3100, false, &a, &b, &c);
    s7_one(never, kV + 0x3000, kV + 0x3100, false, &a, &b, &c);
    expect_u("S7 positive controls: W before G0, after the skip, or never -> 3 establishments, 3 skips, 0 fail-open",
             a == 0u && b == 3u && c == 3u, 1u);
    expect_u("S7 non-vacuous: some schedules establish and skip (a writer that finished before G0)", sk > 0u && es > 0u, 1u);
    // NEGATIVE CONTROL: the same enumeration with the foreign writer bumping BEFORE its slot opens (SKIP82.md item 3 as written)
    uint32_t cfo = 0, csk = 0, ces = 0;
    for (p[0] = 0; p[0] <= 10; p[0]++) for (p[1] = p[0]; p[1] <= 10; p[1]++) for (p[2] = p[1]; p[2] <= 10; p[2]++)
    for (p[3] = p[2]; p[3] <= 10; p[3]++) s7_one(p, kV + 0x3000, kV + 0x3100, false, &cfo, &csk, &ces, true);
    std::printf("      S7 NEGATIVE CONTROL (bump before the slot opens, as SKIP82.md item 3 is written): %u fail-open schedule(s)\n", cfo);
    expect_u("S7 NEGATIVE CONTROL: the contract's order admits a skip over rewritten VRAM (why D1 moved the bump)", cfo > 0u, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ S8
struct Replay { uint32_t user2Copies, user2Skipped, user2Cand, bumps18, colls, collsSkipped, copies, skipped, eligible; };
static Replay s8_replay(bool events, bool print)
{
    TW *x = tw_new(1ull << 18);   // 16 GiB of buckets
    Replay R {};
    std::vector<uint8_t> src(N48_SK82_MAX_BYTES);
    uint32_t lastUser2Skipped = 0u, lastUser2Seen = 0u;
    uint32_t lastUser2Idx = ~0u;
    std::vector<uint32_t> collNear;
    for (uint32_t i = 0; i < kSk82FxN; i++) {
        const sk82_fx_ev &e = kSk82FxEv[i];
        if (e.kind == K_COMMIT) { if (events) n48_sk82_ev_push(&x->ev, N48_SK82_EV_COMMIT, 0, 0); continue; }
        if (e.kind == K_UNMAP) { if (events) n48_sk82_ev_push(&x->ev, N48_SK82_EV_UNMAP, e.va, e.bytes ? e.bytes : e.swz); continue; }
        if (e.kind == K_CGREF) { R.colls++; if (lastUser2Seen && lastUser2Skipped) R.collsSkipped++; continue; }
        if (e.kind == K_SUBST) continue;   // handled by the lookahead below
        // K_COPY: was the next line a substitution for it? (a tainted copy)
        const bool taint = (i + 1u < kSk82FxN && kSk82FxEv[i + 1u].kind == K_SUBST);
        R.copies++;
        const bool user2 = e.lo == 0x10010000ull && e.hi == 0x10018000ull;
        if (e.lo == 0x10018000ull && e.hi == 0x10020000ull) R.bumps18++;
        const bool elig = e.type == 0x40u && (e.swz & 0x1fu) == 0u && e.bytes <= N48_SK82_MAX_BYTES && !(e.bytes & 3u) &&
                          e.hi - e.lo == e.bytes && e.va;
        n48_sk82_key k = key_of(0x1000ull + e.res, e.lo, e.bytes, e.va);
        uint32_t act = N48_SK82_A_COPY, cand = 0u;
        if (elig) {
            R.eligible++;
            std::memset(src.data(), 0, (size_t)e.bytes);
            std::memcpy(src.data(), kSk82FxHeads[e.head], e.bytes < 128u ? (size_t)e.bytes : 128u);
            const CopyRes r = run_copy(x, N48_SK82_SKIP, k, src.data(), 0u, nullptr, 1u, nullptr, taint ? 1u : 0u);
            act = r.act; cand = r.o.cand;
        } else {
            const int32_t s = wr_open(x, e.lo, e.hi, 1u); wr_close(x, s, e.lo, e.hi);
        }
        if (act == N48_SK82_A_SKIP) R.skipped++;
        if (user2) { R.user2Copies++; if (act == N48_SK82_A_SKIP) R.user2Skipped++; if (cand) R.user2Cand++;
                     lastUser2Seen = 1u; lastUser2Skipped = act == N48_SK82_A_SKIP ? 1u : 0u; lastUser2Idx = e.n; }
    }
    (void)lastUser2Idx;
    if (print)
        std::printf("      S8 replay%s: %u copies (%u eligible), %u skipped; USER2 [0x10010000,0x10018000): %u copies, %u SKIPPED, "
                    "%u candidates; copies into [0x10018000,0x10020000) (bucket 0x1001 bumps): %u; 0x10010000 collisions %u, "
                    "nearest-preceding USER2 copy skipped for %u\n", events ? " WITH the 600 commits and 384 unmaps as events" : "",
                    R.copies, R.eligible, R.skipped, R.user2Copies, R.user2Skipped, R.user2Cand, R.bumps18, R.colls, R.collsSkipped);
    tw_free(x);
    return R;
}
static void s8_runy()
{
    expect_u("S8 the fixture: 7986 copies, 33 collisions on 0x10010000, 600 commits", [] {
        uint32_t c = 0, g = 0, m = 0;
        for (uint32_t i = 0; i < kSk82FxN; i++) { c += kSk82FxEv[i].kind == K_COPY; g += kSk82FxEv[i].kind == K_CGREF; m += kSk82FxEv[i].kind == K_COMMIT; }
        return (uint64_t)(c == 7986u && g == 33u && m == 600u); }(), 1u);
    const Replay a = s8_replay(false, true);
    expect_u("S8 contract replay: 350 USER2 copies, 5 copies into [0x10018000,0x10020000)", a.user2Copies == 350u && a.bumps18 == 5u, 1u);
    expect_u("S8 contract replay: the USER2 skips land in SKIP82.md's ~340-of-350 band (>= 300)", a.user2Skipped >= 300u, 1u);
    expect_u("S8 contract replay: every one of the 33 collisions maps to a skipped USER2 copy", a.collsSkipped, 33u);
    const Replay b = s8_replay(true, true);
    expect_u("S8 X2 replay (commits clear all): fewer USER2 skips than the contract replay", b.user2Skipped < a.user2Skipped, 1u);
}

// ------------------------------------------------------------------------------------------------------------------------ S9
static std::string slurp(const char *p)
{
    FILE *f = std::fopen(p, "rb");
    if (!f) return std::string();
    std::string s; char buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}
static uint32_t pin_count(const std::string &s, const char *needle)
{
    uint32_t c = 0; size_t at = 0; const size_t ln = std::strlen(needle);
    while ((at = s.find(needle, at)) != std::string::npos) { c++; at += ln; }
    return c;
}
static size_t pos_of(const std::string &s, const char *needle, size_t from = 0) { return s.find(needle, from); }
// the body of the function whose definition starts with `head` (to the first "\n}\n" after it)
static std::string body_of(const std::string &s, const char *head)
{
    const size_t a = s.find(head);
    if (a == std::string::npos) return std::string();
    const size_t b = s.find("\n}\n", a);
    return s.substr(a, (b == std::string::npos ? s.size() : b) - a);
}
static void s9_pins(const char *peerP, const char *bringP, const char *ahhP, const char *commitP)
{
    const std::string peer = slurp(peerP), br = slurp(bringP), ahh = slurp(ahhP), cm = slurp(commitP);
    expect_u("S9 read the four source files", !peer.empty() && !br.empty() && !ahh.empty() && !cm.empty(), 1u);
    // --- the copier (Navi48AccelPeer.cpp)
    // build 0.0.528 item 5b: Apple's dirty START and LENGTH, read from res+0x138 and res+0x140, printed as two
    // fields by BOTH lines (SKIP / WOULD-SKIP and DIFFER), after dirtyEnd, which stays their sum.
    {
        const std::string ts = body_of(peer, "static __attribute__((noinline)) bool sk82_try_skip(void *self, void *dstMap, void *dstMem, PhysSegmentFn dSeg, IOMemoryDescriptor *md) {");
        expect_u("S9 5b: dirtyStart = *(res+0x138), dirtyLen = *(res+0x140), dirtyEnd = their sum, once each in sk82_try_skip",
                 pin_count(ts, "const uint64_t dirtyStart = *reinterpret_cast<const uint64_t *>(r + 0x138);") == 1u &&
                 pin_count(ts, "const uint64_t dirtyLen = *reinterpret_cast<const uint64_t *>(r + 0x140);") == 1u &&
                 pin_count(ts, "const uint64_t dirtyEnd = dirtyStart + dirtyLen;") == 1u, 1u);
        expect_u("S9 5b: both lines pass dirtyEnd, dirtyStart, dirtyLen in the format's order",
                 pin_count(ts, "(unsigned long long)dirtyEnd,\n                    (unsigned long long)dirtyStart, (unsigned long long)dirtyLen);") == 1u &&
                 pin_count(ts, "(unsigned long long)dirtyEnd, (unsigned long long)dirtyStart,\n                    (unsigned long long)dirtyLen, (unsigned long long)o.n);") == 1u, 1u);
        expect_u("S9 5b: both formats name dirtyStart and dirtyLen after dirtyEnd",
                 std::string(N48_SK82_SKIP_FMT).find("dirtyEnd %#llx dirtyStart %#llx dirtyLen %#llx") != std::string::npos &&
                 std::string(N48_SK82_DIFFER_FMT).find("dirtyEnd %#llx dirtyStart %#llx dirtyLen %#llx") != std::string::npos, 1u);
    }
    const std::string rc = body_of(peer, "static bool residency_copy_to_vram(void *self, void *dstMap, void *srcMap) {");
    const char *call = "if (gN48Sk82On && sk82_try_skip(self, dstMap, dstMem, dSeg, md)) return true;";
    expect_u("S9 the decision is called exactly once, in residency_copy_to_vram", pin_count(peer, call) == 1u && pin_count(rc, call) == 1u, 1u);
    const size_t pCall = pos_of(rc, call);
    const size_t pBack = pos_of(rc, "COPY_UNHANDLED(kCopyShapeBacking");
    const size_t pLin = pos_of(rc, "if (rpOn) (void)rp_lin_prepare(r, md, srcLen, dstLen, &rt);");
    const size_t pScope = pos_of(rc, "Navi48CopyScope cgScope(cgLo, cgHi);");
    const size_t pIc = pos_of(rc, "ic_begin(dstMem, dSeg, md");
    const size_t pCopies = pos_of(rc, "gCopy.copies++;");
    const size_t pNote = pos_of(rc, "n48::hw_resprov_note_copy(&rc");
    const size_t pC88 = pos_of(rc, "navi48_c88_copy_note(");
    const size_t pLine = pos_of(rc, "residency-copy: COPIED #%llu");
    expect_u("S9 ORDER: the backing check, THEN the decision, THEN rp_lin_prepare, the scope's open (G0 before it), ic_begin, "
             "gCopy, the COPIED line, the c88 note and resprov's note_copy",
             pBack < pCall && pCall < pLin && pLin < pScope && pScope < pIc && pIc < pCopies && pCopies < pLine && pLine < pC88 &&
             pC88 < pNote && pNote != std::string::npos, 1u);
    const std::string tk = body_of(peer, "static __attribute__((noinline)) bool sk82_try_skip(");
    uint32_t forb = 0;
    for (const char *t : { "Navi48CopyScope", "navi48_cg_open", "hw_resprov_note_copy", "gCopy.copies++", "gCopy.bytes", "navi48_c88_copy_note",
                           "ic_begin", "navi48_fc_chunk", "navi48_fc_copy_report", "navi48_vram_write_mm", "IOMalloc", "residency-copy: COPIED" })
        forb += pin_count(tk, t);
    expect_u("S9 the skip path does nothing else: sk82_try_skip opens no scope, writes no VRAM, notes no provenance, moves no gCopy, "
             "logs no COPIED line, takes no fast-copy slot, allocates nothing", !tk.empty() && forb == 0u, 1u);
    // build 0.0.529: the same helper also reports switch 84's result (one more load: gN48D84Live)
    const char *res = "if (gN48Sk82On || gN48D84Live) sk82_copy_result(compared, mismatched, wBytes, retile ? 1u : 0u);";
    expect_u("S9 the result is noted once, after gCopy's update and BEFORE the scope closes (inside residency_copy_to_vram)",
             pin_count(peer, res) == 1u && pos_of(rc, "gCopy.lastMicros  = ns / 1000;") < pos_of(rc, res) &&
             pos_of(rc, res) < pos_of(rc, "navi48_fc_copy_report(gCopy.copies);"), 1u);
    const std::string cs = body_of(peer, "__attribute__((noinline)) void navi48_cg_close_scope(");
    expect_u("S9 the establishment runs from navi48_cg_close_scope AFTER navi48_cg_close and navi48_ic_scope_closed",
             pos_of(cs, "navi48_cg_close(slot") < pos_of(cs, "navi48_ic_scope_closed(") &&
             pos_of(cs, "navi48_ic_scope_closed(") < pos_of(cs, "if (gN48Sk82On) sk82_scope_closed(lo, hi);") &&
             pos_of(cs, "if (gN48Sk82On) sk82_scope_closed(lo, hi);") != std::string::npos, 1u);
    const std::string fcb = body_of(peer, "static __attribute__((noinline)) uint64_t fc_copy_chunk(");
    expect_u("S9 fc_copy_chunk ORs the FULL flag (asked at the first chunk only) into rpCand",
             pin_count(fcb, "const uint32_t cand = (retile ? 1u : 0u) | ((pos == 0u && gN48Sk82On) ? navi48_sk82_cand_mine() : 0u);") == 1u &&
             pin_count(fcb, "&fc_fill_batch, &c, lead, cgLo, cgHi, cand);") == 1u, 1u);
    const std::string cr = body_of(peer, "static __attribute__((noinline)) void sk82_copy_result(");
    expect_u("S9 the result requires compared x 4 >= wBytes, 0 mismatches, not re-tiled and no switch-62 bump",
             pin_count(cr, "const uint32_t bumped = navi48_ic_bumped_mine();") == 1u &&
             pos_of(cr, "const uint32_t bumped = navi48_ic_bumped_mine();") < pos_of(cr, "if (gN48Sk82On) {") &&
             pin_count(cr, "const uint32_t ok = (compared * 4ull >= wBytes && !mismatched && !retiled && !bumped) ? 1u : 0u;") == 1u &&
             pos_of(cr, "if (gN48Sk82On) {") < pos_of(cr, "navi48_sk82_result(ok, wBytes);"), 1u);
    const std::string sh = body_of(peer, "static int shadercache_hit(void *vctx, size_t off, const sc_match *m) {");
    expect_u("S9 shadercache's substitution taints the copy BEFORE its write loop",
             pos_of(sh, "if (gN48Sk82On) navi48_sk82_taint_mine();") < pos_of(sh, "ok = navi48_vram_write_mm(at + o, w, nd)") &&
             pos_of(sh, "if (gN48Sk82On) navi48_sk82_taint_mine();") != std::string::npos, 1u);
    const std::string kb = body_of(peer, "static uint32_t substitute_blit_kernel_at(uint64_t vramAt, uint32_t mode, uint32_t *reason) {");
    expect_u("S9 kernsub's write taints the copy BEFORE it writes",
             pos_of(kb, "if (gN48Sk82On) navi48_sk82_taint_mine();") < pos_of(kb, "navi48_vram_write_mm(vramAt, buf, nd)") &&
             pos_of(kb, "if (gN48Sk82On) navi48_sk82_taint_mine();") != std::string::npos, 1u);
    // --- the counters and the glue (Navi48Bringup.cpp)
    const std::string op = body_of(br, "int32_t navi48_cg_open(uint64_t lo, uint64_t hi) {");
    const size_t pOpen = pos_of(op, "n48_cg_slot_open_d("), pUn = pos_of(op, "__atomic_fetch_add(&gCgUntracked, 1u");   // 0.0.529: _d
    const size_t pBu = pos_of(op, "if (sk82) { n48_sk82_bump(sk82, gSk82Nb, &gSk82Every, lo, hi); __atomic_fetch_add(&gSk82Every, 1ull, __ATOMIC_SEQ_CST); }");
    // build 0.0.529: switch 84's key bump sits between the switch-82 bump and BEGIN (both after the slot's publication)
    const size_t pBt = pos_of(op, "\tif (sk82) n48_sk82_bump(sk82, gSk82Nb, &gSk82Every, lo, hi);\n\tif (gN48D84Live) d84_open_bump(lo, hi);") < pos_of(op, "n48_cg_ring_push_od(&gCgRing, N48_CG_EV_BEGIN, (uint32_t)slot")
                       ? pos_of(op, "\tif (sk82) n48_sk82_bump(sk82, gSk82Nb, &gSk82Every, lo, hi);\n\tif (gN48D84Live) d84_open_bump(lo, hi);") : std::string::npos;
    expect_u("S9 navi48_cg_open (D1): ONE load of gSk82Cnt; the bump AFTER n48_cg_slot_open (tracked) and AFTER the untracked count "
             "(untracked, + everything), before the BEGIN and the return",
             pin_count(op, "uint32_t *const sk82 = __atomic_load_n(&gSk82Cnt, __ATOMIC_ACQUIRE);") == 1u && pin_count(op, "n48_sk82_bump(") == 2u &&
             pOpen < pUn && pUn < pBu && pBu != std::string::npos && pOpen < pBt && pBt != std::string::npos, 1u);
    const std::string cl = body_of(br, "void navi48_cg_close(int32_t slot, uint64_t lo, uint64_t hi, bool wrote, bool failed, bool mismatch) {");
    expect_u("S9 navi48_cg_close is 0.0.526's (no switch-82 code)", !cl.empty() && pin_count(cl, "sk82") == 0u, 1u);
    const size_t gA = pos_of(br, "uint32_t navi48_sk82_try("), gB = pos_of(br, "static bool sk82_alloc() {");
    const std::string glue = (gA != std::string::npos && gB != std::string::npos && gA < gB) ? br.substr(gA, gB - gA) : std::string();
    expect_u("S9 X4: no allocation in the copy-path glue (try, result, taint, cand, closed); sk82_alloc called once, by the switch",
             !glue.empty() && pin_count(glue, "IOMalloc") == 0u && pin_count(glue, "IOLockAlloc") == 0u &&
             pin_count(glue, "sk82_alloc()") == 0u && pin_count(br, "sk82_alloc()") == 2u &&
             pos_of(body_of(br, "uint32_t navi48_sk82_switch("), "!sk82_alloc()") != std::string::npos, 1u);
    expect_u("S9 switch 82 is OFF at boot and its mode is written in ONE place (the switch, under the lock)",
             pin_count(br, "static volatile uint32_t gSk82Mode { N48_SK82_OFF };") == 1u && pin_count(br, "gSk82Mode = ") == 1u &&
             pin_count(br, "volatile uint32_t gN48Sk82On { 0u };") == 1u && pin_count(br, "volatile uint32_t gN48Sk82Live { 0u };") == 1u, 1u);
    expect_u("S9 the establishment's glue re-reads the source and runs n48_sk82_establish under the lock",
             pin_count(body_of(br, "void navi48_sk82_closed("), "n48_sk82_establish(&gSk82T, &w, me, lo, hi, sn, post, &cause);") == 1u, 1u);
    // --- the selector and the event sites (AppleHardwareHook.cpp, gfx_commit.h)
    expect_u("S9 the selector: `82 | M << 8` guarded mid-arm, through navi48_sk82_switch, exactly once",
             pin_count(ahh, "} else if ((arg & 0xffull) == 82ull) {") == 1u &&
             pin_count(ahh, "n48_cm_cont_switch_refused(82u, m == 0u ? 1u : 0u, gXdShot.cont, gXdShot.state)") == 1u &&
             pin_count(ahh, "(void)navi48_sk82_switch(m, contRefused82 ? 1u : 0u, &st);") == 1u, 1u);
    expect_u("S9 gfx_commit.h's guard lists 82", pin_count(cm, "    case 82u:") == 1u, 1u);
    const std::string um = body_of(ahh, "static uint64_t hook_unmapVA(void *self, uint64_t va, uint64_t size) {");
    expect_u("S9 every unmapVA pushes its range BEFORE the scope check (vmctx_find)",
             pos_of(um, "if (gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_UNMAP, va, size);") < pos_of(um, "VmCtxObs *e = vmctx_find(self);") &&
             pos_of(um, "if (gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_UNMAP, va, size);") != std::string::npos, 1u);
    expect_u("S9 WindowServer's drop and a context release push their events",
             pin_count(ahh, "if (dropped && gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_WS, 0u, 0u);") == 1u &&
             pin_count(ahh, "if (gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_CTX, 0u, 0u);") == 1u, 1u);
    const std::string rw = body_of(ahh, "static uint64_t hook_ringWriteTail(void *self) {");
    expect_u("S9 an Apple SDMA submission pushes BEFORE drain_write_tail rings",
             pos_of(rw, "if (gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_SDMA, 0u, 0u);") < pos_of(rw, "drain_write_tail(self, drv)") &&
             pos_of(rw, "if (gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_SDMA, 0u, 0u);") != std::string::npos, 1u);
    const size_t pCo = pos_of(ahh, "? gfxsrc_commit_try(vm, info, ib0Va, f.ib[0].got, f.nib, arm, verdict, stampNow, tgtVa) : 0u;");
    expect_u("S9 a committed frame pushes right after the gate's answer",
             pin_count(ahh, "if (commitOk && gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_COMMIT, 0u, 0u);") == 1u &&
             pCo < pos_of(ahh, "if (commitOk && gN48Sk82Live) navi48_sk82_ev(N48_SK82_EV_COMMIT, 0u, 0u);"), 1u);
    const std::string gw = body_of(ahh, "static uint64_t hook_gfxWriteTail(void *self) {");
    expect_u("S9 the GFX walk pushes for un-walked new work and for any IB it lets run, before the NOP pass",
             pin_count(gw, "if (gN48Sk82Live && wptr != gGfxDone) navi48_sk82_ev(N48_SK82_EV_RUN, 0u, 0u);") == 1u &&
             pos_of(gw, "if (gN48Sk82Live && w.ibs && (!armedNow || sparedN)) navi48_sk82_ev(N48_SK82_EV_RUN, 0u, 0u);") <
             pos_of(gw, "gfx_neuter_frame(cpu, from"), 1u);
    // OFF identity: every copy-path site is behind ONE load
    expect_u("S9 OFF: every copier site is behind ONE load of gN48Sk82On (decision, result, cand, establishment, two taints)",
             pin_count(peer, call) == 1u && pin_count(peer, res) == 1u &&
             pin_count(peer, "(pos == 0u && gN48Sk82On) ? navi48_sk82_cand_mine() : 0u") == 1u &&
             pin_count(peer, "if (gN48Sk82On) sk82_scope_closed(lo, hi);") == 1u &&
             pin_count(peer, "if (gN48Sk82On) navi48_sk82_taint_mine();") == 2u &&
             pin_count(peer, "sk82_try_skip(") == 2u && pin_count(peer, "sk82_copy_result(") == 2u &&
             pin_count(peer, "sk82_scope_closed(") == 3u && pin_count(peer, "navi48_sk82_taint_mine()") == 2u &&
             // build 0.0.529: 7 - sk82_copy_result now also serves switch 84, so its switch-82 half is gated inside it
             pin_count(peer, "gN48Sk82On") == 7u && pin_count(body_of(peer, "static __attribute__((noinline)) void sk82_copy_result("), "if (gN48Sk82On) {") == 1u, 1u);
}

int main(int argc, char **argv)
{
    s1_selector();
    s2_widths();
    s3_counters();
    s4_flow();
    s5_tail();
    s6_refusals();
    s7_orders();
    s8_runy();
    if (argc >= 5) s9_pins(argv[1], argv[2], argv[3], argv[4]);
    else { std::printf("FAIL  S9 needs Navi48AccelPeer.cpp Navi48Bringup.cpp AppleHardwareHook.cpp gfx_commit.h\n"); gFail++; gRun++; }
    std::printf("gfx_sk82_test: %d run, %d failed\n", gRun, gFail);
    return gFail ? 1 : 0;
}
