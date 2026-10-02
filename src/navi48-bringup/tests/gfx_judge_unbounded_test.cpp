// gfx_judge_unbounded_test.cpp — build 0.0.517: THE JUDGE NEVER STOPS.
//     clang++ -std=c++17 -Wall -Wextra -Werror -O1 -fsanitize=address,undefined -I src/navi48-bringup/src/apple \
//         src/navi48-bringup/tests/gfx_judge_unbounded_test.cpp -o /tmp/judgeu && /tmp/judgeu src/navi48-bringup/src/apple/AppleHardwareHook.cpp
// fc5 stopped judging at `frames judged 4096 ... over the 4096-frame cap 2602`. The cap guarded no table (nothing the judge keeps
// is indexed by the frame number), so 0.0.517 turns it into a MARK (n48_sd_past_mark, a count) and judges every frame. Covers:
//   T1 the mark's boundary (a frame is past it exactly when the judged count before it is >= the mark) - the old stop's own
//      condition, so for every frame below the mark 0.0.516 and 0.0.517 take the same (not-taken) branch;
//   T2 the cycle515 bookkeeping (per-frame, frame-numbered) over 140,000 frames of a 12-frame pattern: every frame's decision
//      equals the same pattern position's decision before 4096 and before 65,536, and a run started at 2^32 - 60 decides the
//      same as one started at 0 (frame numbers are 64-bit end to end: the "dirtied by" distance stays the pattern's);
//   T3 heapgen's per-frame identity (n48_hg_frame_begin / n48_hg_judge) at judged 1, 4096, 4097, 65,536, 65,537, 2^32, 2^32+1:
//      OK for its own frame, STALE for the next, at every one of them;
//   T4 the fill-set second window's uint16 counters over 70,000 judged frames (past 65,536): they stop at the 60-frame bound
//      and never wrap, and the window answers PASS for every frame after its expiry;
//   T5 the token seq wrap (n48_cm_seq_next): 0xFFFFFFFE -> 0xFFFFFFFF -> 1, never 0; the flight ring and the token match
//      driven across that wrap answer exactly as they do at seq 1;
//   T6 the kext glue (AppleHardwareHook.cpp): the judge's only use of the mark is a count, before the frame's work and with no
//      return or unlock between it and the frame's record; the frame-cap stop and the cap arithmetic are gone; both token
//      seq sites call n48_cm_seq_next and no raw increment is left.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "gfx_commit.h"
#include "gfx_cycle515.h"
#include "gfx_heapgen.h"
#include "gfx_fillset.h"
#include "gfx_flightring.h"

static int gFail = 0, gRun = 0;
static void expect_u(const char *what, uint64_t got, uint64_t want)
{
    gRun++;
    if (got != want) { gFail++; std::printf("FAIL  %-86s got %#llx want %#llx\n", what, (unsigned long long)got, (unsigned long long)want); }
    else std::printf("ok    %-86s %#llx\n", what, (unsigned long long)got);
}

// ---------------------------------------------------------------------------------------------------------------- T1
static void t1_mark()
{
    expect_u("T1 judged 0 against mark 4096: not past", n48_sd_past_mark(0ull, 4096u), 0u);
    expect_u("T1 judged 4095 against mark 4096: not past (frame 4096, the old last judged frame)", n48_sd_past_mark(4095ull, 4096u), 0u);
    expect_u("T1 judged 4096 against mark 4096: past (frame 4097, the old first skipped frame)", n48_sd_past_mark(4096ull, 4096u), 1u);
    expect_u("T1 judged 1023 against the default mark 1024: not past", n48_sd_past_mark(1023ull, 1024u), 0u);
    expect_u("T1 judged 1024 against the default mark 1024: past", n48_sd_past_mark(1024ull, 1024u), 1u);
    expect_u("T1 judged 2^32 against mark 0xFFFFFFFF: past (64-bit count, no truncation)", n48_sd_past_mark(1ull << 32, 0xFFFFFFFFu), 1u);
    expect_u("T1 judged 2^32 + 5 against mark 4096: past", n48_sd_past_mark((1ull << 32) + 5u, 4096u), 1u);
    // For every judged count below the mark the old stop's condition was false and the mark answers 0: same branch.
    uint32_t agree = 1u;
    for (uint64_t j = 0; j < 9000ull; j++) {
        const uint32_t oldStop = (j >= 4096ull) ? 1u : 0u;
        if (n48_sd_past_mark(j, 4096u) != oldStop) agree = 0u;
    }
    expect_u("T1 the mark is exactly the old stop's condition for judged 0..8999 at mark 4096", agree, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T2
// A 12-frame pattern of the run10u shape: writer a (X), writer b (X'), P sampling X; writer a (X'), writer b (X), P sampling
// X'; ... with the writer at position 7 REFUSED (a refused writer dirties its write set) and every other frame committed.
static const uint64_t kX = 0x400800000ull, kXp = 0x404800000ull, kPlane = 0x401800000ull;
struct Dec { uint32_t kind, dirtyA, dirtyB; uint64_t distA, distB; };
static Dec cy_step(n48_cy *c, uint64_t frame, uint32_t pos)
{
    n48_cy_frame_begin(c, 1u);
    const uint32_t isP = (pos % 3u) == 2u;
    const uint32_t committed = pos == 7u ? 0u : 1u;
    if (isP) {
        const uint64_t layer = ((pos / 3u) & 1u) ? kXp : kX;
        const uint64_t va[2] = { layer, kPlane };
        const uint32_t mode[2] = { 27u, 0u };
        n48_cy_note_inputs(c, va, mode, 2u, 0u);
        n48_cy_ws_add(c, kPlane);
    } else {
        n48_cy_ws_add(c, (pos & 1u) ? kXp : kX);
    }
    const n48_cy_judge j = n48_cy_frame(c, frame, committed, isP, 1u, committed ? (uint32_t)(frame & 0xFFFFu) + 1u : 0u);
    Dec d { j.kind, j.dirtyA, j.dirtyB, j.byA ? frame - j.byA : 0ull, j.byB ? frame - j.byB : 0ull };
    return d;
}
static bool same(const Dec &a, const Dec &b)
{
    return a.kind == b.kind && a.dirtyA == b.dirtyA && a.dirtyB == b.dirtyB && a.distA == b.distA && a.distB == b.distB;
}
static void t2_cycle515()
{
    static n48_cy c;
    std::memset(&c, 0, sizeof c);
    const uint64_t N = 140000ull;
    std::vector<Dec> d(N + 1u);
    for (uint64_t f = 1; f <= N; f++) d[f] = cy_step(&c, f, (uint32_t)((f - 1u) % 12u));
    // The steady state starts once every tracked surface has been touched (the first 24 frames); compare each frame with the
    // SAME pattern position one or more whole patterns earlier, over the 4096 and 65,536 boundaries.
    uint32_t bad4096 = 0u, bad65536 = 0u, badLate = 0u, dirty = 0u;
    for (uint64_t f = 4096ull - 48u; f <= 4096ull + 48u; f++) if (!same(d[f], d[f - 12u * 300u])) bad4096++;
    for (uint64_t f = 65536ull - 48u; f <= 65536ull + 48u; f++) if (!same(d[f], d[f - 12u * 5000u])) bad65536++;
    for (uint64_t f = N - 48u; f <= N; f++) { if (!same(d[f], d[48u + ((f - 48u) % 12u)])) badLate++; if (d[f].kind == N48_CY_DIRTY) dirty++; }
    expect_u("T2 cycle515: frames 4048..4144 decide as the same positions 3600 frames earlier", bad4096, 0u);
    expect_u("T2 cycle515: frames 65488..65584 decide as the same positions 60000 frames earlier", bad65536, 0u);
    expect_u("T2 cycle515: the last 49 of 140000 frames decide as frames 48..59", badLate, 0u);
    expect_u("T2 cycle515: the pattern's refused writer still dirties a P at frame 140000 (the judge still sees it)", dirty > 0u, 1u);
    expect_u("T2 cycle515: frames counted = 140000 (no frame skipped)", c.frames, N);
    expect_u("T2 cycle515: table full never (the table is keyed by surface, not frame)", c.tableFull, 0u);
    // The same pattern started just below 2^32: a 32-bit frame anywhere would make a "dirtied by" distance huge.
    static n48_cy c2;
    std::memset(&c2, 0, sizeof c2);
    const uint64_t base = (1ull << 32) - 60ull;
    uint32_t badWrap = 0u;
    for (uint64_t i = 1; i <= 240ull; i++) {
        const Dec w = cy_step(&c2, base + i, (uint32_t)((i - 1u) % 12u));
        if (!same(w, d[i])) badWrap++;
    }
    expect_u("T2 cycle515: 240 frames across frame 2^32 decide exactly as frames 1..240", badWrap, 0u);
}

// ---------------------------------------------------------------------------------------------------------------- T3
static void t3_heapgen()
{
    static n48_hg_state s;
    std::memset(&s, 0, sizeof s);
    s.gen = 2u;
    const uint64_t js[] = { 1ull, 4096ull, 4097ull, 65536ull, 65537ull, 1ull << 32, (1ull << 32) + 1u, (1ull << 33) + 7u };
    uint32_t okAll = 1u, staleAll = 1u;
    for (uint64_t j : js) {
        static n48_hg_frame f;
        n48_hg_frame_begin(&f, 1u, j, &s);
        if (n48_hg_judge(&s, &f, j) != N48_HG_OK) okAll = 0u;
        if (n48_hg_judge(&s, &f, j + 1u) != N48_HG_STALE) staleAll = 0u;
        char lbl[120];
        std::snprintf(lbl, sizeof lbl, "T3 heapgen: frame %llu judged OK for itself", (unsigned long long)j);
        expect_u(lbl, n48_hg_judge(&s, &f, j), N48_HG_OK);
    }
    expect_u("T3 heapgen: every frame number is OK for itself (1 .. 2^33+7)", okAll, 1u);
    expect_u("T3 heapgen: every frame number is STALE for the next frame", staleAll, 1u);
    static n48_hg_frame f32;
    n48_hg_frame_begin(&f32, 1u, (1ull << 32) + 1u, &s);
    expect_u("T3 heapgen: frame 2^32+1 is STALE for frame 1 (no 32-bit alias)", n48_hg_judge(&s, &f32, 1ull), N48_HG_STALE);
}

// ---------------------------------------------------------------------------------------------------------------- T4
static void t4_fillset16()
{
    static n48_fs f;
    std::memset(&f, 0, sizeof f);
    f.plane_on = 1u;               // the fill window is OFF (on 0): the second window opens at the arm
    n48_fs_open(&f);
    uint32_t refuse = 0u, passAfter = 0u, other = 0u;
    for (uint32_t i = 0; i < 70000u; i++) {
        const uint32_t v = n48_fs_plane_step(&f, 1u, 0u);
        if (i < N48_FS_PLANE_EXPIRE_JUDGED) { if (v == N48_FS_REFUSE) refuse++; else other++; }
        else { if (v == N48_FS_PASS) passAfter++; else other++; }
    }
    expect_u("T4 fillset: the second window refuses its first 60 eligible non-plane frames", refuse, N48_FS_PLANE_EXPIRE_JUDGED);
    expect_u("T4 fillset: and PASSES all 69,940 after (past 65,536: no uint16 wrap reopens it)", passAfter, 70000u - N48_FS_PLANE_EXPIRE_JUDGED);
    expect_u("T4 fillset: no other answer", other, 0u);
    expect_u("T4 fillset: plane_judged stops at 60", f.plane_judged, N48_FS_PLANE_EXPIRE_JUDGED);
    expect_u("T4 fillset: plane_refused stops at 60", f.plane_refused, N48_FS_PLANE_EXPIRE_JUDGED);
    expect_u("T4 fillset: plane_expiry fired once", f.plane_expiry, 1u);
}

// ---------------------------------------------------------------------------------------------------------------- T5
static void t5_seq()
{
    expect_u("T5 seq: 0 -> 1 (the first rewrite, as ++gXdCmSeq gave)", n48_cm_seq_next(0u), 1u);
    expect_u("T5 seq: 41 -> 42", n48_cm_seq_next(41u), 42u);
    expect_u("T5 seq: 0xFFFFFFFD -> 0xFFFFFFFE", n48_cm_seq_next(0xFFFFFFFDu), 0xFFFFFFFEu);
    expect_u("T5 seq: 0xFFFFFFFE -> 0xFFFFFFFF (the last value is used)", n48_cm_seq_next(0xFFFFFFFEu), 0xFFFFFFFFu);
    expect_u("T5 seq: 0xFFFFFFFF -> 1 (never 0)", n48_cm_seq_next(0xFFFFFFFFu), 1u);
    uint32_t same = 1u;
    for (uint32_t c = 0; c < 100000u; c++) if (n48_cm_seq_next(c) != c + 1u) same = 0u;
    expect_u("T5 seq: identical to cur + 1 for cur 0..99999", same, 1u);
    // The flight ring and the token match across the wrap, one flight at a time (push, commit, retire, reclaim), from 0xFFFFFFF0.
    static n48_fr_ring r;
    n48_fr_reset(&r);
    uint32_t seq = 0xFFFFFFF0u, zero = 0u, pushOk = 0u, findOk = 0u, commitOk = 0u, freeOk = 0u, tokOk = 0u, liveAfter = 0u;
    for (uint32_t i = 0; i < 40u; i++) {
        seq = n48_cm_seq_next(seq);
        if (!seq) zero++;
        uint32_t idx = 99u, fidx = 98u;
        if (n48_fr_push(&r, seq, 1000u + i, i + 1u, 0x1000ull, 0x5u, &idx)) pushOk++;
        if (n48_fr_find(&r, seq, &fidx) && fidx == idx) findOk++;
        if (n48_fr_mark_committed(&r, seq)) commitOk++;
        if (n48_fr_poll_entry(&r, idx < N48_FR_CAPACITY ? idx : 0u, 1u, 0x5u) && n48_fr_reclaim(&r) == 1u) freeOk++;
        n48_cm_token t {};
        t.info = &r; t.va = 0x400004000ull; t.stamp = 0x10u; t.n = 1040u; t.nib = 1u; t.seq = seq;
        if (n48_cm_token_match(&t, &t)) tokOk++;
    }
    const n48_fr_counts cnt = n48_fr_count(&r);
    liveAfter = cnt.pending + cnt.committed + cnt.notRun;
    expect_u("T5 seq: 40 seqs across the wrap, none 0", zero, 0u);
    expect_u("T5 flight ring: 40 pushes across the wrap all accepted", pushOk, 40u);
    expect_u("T5 flight ring: each found at its own slot", findOk, 40u);
    expect_u("T5 flight ring: each PENDING -> COMMITTED", commitOk, 40u);
    expect_u("T5 flight ring: each retired by its own fence and reclaimed", freeOk, 40u);
    expect_u("T5 flight ring: nothing live afterwards", liveAfter, 0u);
    expect_u("T5 token: each seq's token matches itself (seq 0 never would)", tokOk, 40u);
}

// ---------------------------------------------------------------------------------------------------------------- T6
static std::string readAll(const char *path)
{
    std::string s;
    FILE *fp = std::fopen(path, "rb");
    if (!fp) return s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) s.append(buf, n);
    std::fclose(fp);
    return s;
}
static uint32_t countOf(const std::string &hay, const char *needle)
{
    uint32_t c = 0u;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1u)) c++;
    return c;
}
static void t6_glue(const char *ahhPath)
{
    const std::string src = readAll(ahhPath);
    expect_u("T6 AppleHardwareHook.cpp read", src.size() > 1000000u, 1u);
    const char *sig = "static uint32_t gfxsrc_decide_frame(const uint8_t *info, uint32_t shapeOk, uint32_t n, const WsFrame *wf) {";
    const size_t b0 = src.find(sig);
    const size_t b1 = b0 == std::string::npos ? std::string::npos : src.find("\n}\n", b0);
    expect_u("T6 the judge's body located", b0 != std::string::npos && b1 != std::string::npos, 1u);
    if (b0 == std::string::npos || b1 == std::string::npos) return;
    const std::string body = src.substr(b0, b1 - b0);
    const char *markLine = "if (n48_sd_past_mark(gXdC.judged, gXdFrameCap)) gXdC.pastMark++;\n";
    expect_u("T6 the mark is a count, exactly once in the file", countOf(src, markLine), 1u);
    expect_u("T6 the judge names gXdFrameCap exactly once (the count)", countOf(body, "gXdFrameCap"), 1u);
    const size_t pm = body.find(markLine);
    const size_t fr = body.find("static n48_xv_frame f;");
    const size_t inc = body.find("gXdC.judged++;");
    expect_u("T6 order: the mark precedes the frame record, which precedes `gXdC.judged++`",
             pm != std::string::npos && fr != std::string::npos && inc != std::string::npos && pm < fr && fr < inc, 1u);
    if (pm != std::string::npos && fr != std::string::npos && pm < fr) {
        const std::string span = body.substr(pm, fr - pm);
        expect_u("T6 no `return` between the mark and the frame record", countOf(span, "return"), 0u);
        expect_u("T6 no unlock between the mark and the frame record", countOf(span, "IOLockUnlock"), 0u);
        expect_u("T6 no `if (` other than the mark's own between it and the frame record", countOf(span, "if ("), 1u);
    }
    expect_u("T6 `gXdC.judged++` exactly once in the judge", countOf(body, "gXdC.judged++;"), 1u);
    expect_u("T6 the frame-cap stop reason is used nowhere in the kext", countOf(src, "N48_CM_STOP_FRAME_CAP"), 0u);
    expect_u("T6 the old skip counter is gone", countOf(src, "skippedBudget"), 0u);
    expect_u("T6 no `>= gXdFrameCap` comparison anywhere", countOf(src, ">= gXdFrameCap"), 0u);
    expect_u("T6 the continuous arm's cap arithmetic is gone", countOf(src, "gXdFrameCap - gXdC.judged"), 0u);
    expect_u("T6 capRoom is 1 at the arm verb, exactly once", countOf(src, "const uint32_t capRoom = 1u;"), 1u);
    expect_u("T6 the rewrite's token seq comes from n48_cm_seq_next, exactly once",
             countOf(src, "tok.seq = gXdCmSeq = n48_cm_seq_next(gXdCmSeq);"), 1u);
    expect_u("T6 the rehearsal's token seq comes from n48_cm_seq_next, exactly once",
             countOf(src, "nib, n48_cm_seq_next(gXdCmSeq) };"), 1u);
    expect_u("T6 no raw `++gXdCmSeq`", countOf(src, "++gXdCmSeq"), 0u);
    expect_u("T6 no raw `gXdCmSeq++`", countOf(src, "gXdCmSeq++"), 0u);
    expect_u("T6 no raw `gXdCmSeq + 1`", countOf(src, "gXdCmSeq + 1"), 0u);
    expect_u("T6 gXdCmSeq is written at exactly one site", countOf(src, "gXdCmSeq ="), 1u);
    expect_u("T6 the report line names the mark", countOf(src, "judged past the %u-frame mark %llu (no cap)"), 1u);
}

int main(int argc, char **argv)
{
    t1_mark();
    t2_cycle515();
    t3_heapgen();
    t4_fillset16();
    t5_seq();
    if (argc > 1) t6_glue(argv[1]);
    else { gFail++; std::printf("FAIL  T6 needs AppleHardwareHook.cpp as argv[1]\n"); }
    std::printf("gfx_judge_unbounded: %d/%d passed%s\n", gRun - gFail, gRun, gFail ? " - FAILURES" : "");
    return gFail ? 1 : 0;
}
